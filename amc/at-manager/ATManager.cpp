/*
 **
 ** Copyright 2011 Intel Corporation
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 ** http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#define LOG_TAG "AT_MANAGER"
#include "ATManager.h"
#include "EventThread.h"
#include "stmd.h"
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <utils/Log.h>
#include <cutils/sockets.h>
#include "ATParser.h"
#include "TtyHandler.h"
#include "ATNotifier.h"

#define MAX_TIME_MODEM_STATUS_CHECK_SECONDS 60
#define MAX_WAIT_FOR_STMD_CONNECTION_SECONDS 5
#define STMD_CONNECTION_RETRY_TIME_MS 200
#define AT_ANSWER_TIMEOUT_MS 1000
#define INFINITE_TIMEOUT (-1)
#define TTY_OPEN_DELAY_US 200000
#define RECOVER_TIMEOUT_MS 2000
#define MAX_RETRY_AFTER_TIMEOUT 5

CATManager::CATManager(IATNotifier *observer) :
    _pAwaitedTransactionEndATCommand(NULL), _bStarted(false), _bModemAlive(false), _bClientWaiting(false), _pCurrentATCommand(NULL), _pPendingClientATCommand(NULL), _uiTimeoutSec(-1),
    _pEventThread(new CEventThread(this)), _bFirstModemStatusReceivedSemaphoreCreated(false),
    _pATParser(new CATParser), _bTtyListenersStarted(false), _pIATNotifier(observer), _iTimeoutRetry(0)
{
    // Client Mutex
    bzero(&_clientMutex, sizeof(_clientMutex));
    pthread_mutex_init(&_clientMutex, NULL);

    // Client wait semaphore
    bzero(&_clientWaitSemaphore, sizeof(_clientWaitSemaphore));
    sem_init(&_clientWaitSemaphore, 0, 0);


    // Answer Wait semaphore
    bzero(&_answerWaitSemaphore, sizeof(_answerWaitSemaphore));
    sem_init(&_answerWaitSemaphore, 0, 0);
}

CATManager::~CATManager()
{
    // Stop
    stop();

    // AT parser
    delete _pATParser;

    // Thread
    delete _pEventThread;

    // Semaphores
    sem_destroy(&_answerWaitSemaphore);
    sem_destroy(&_clientWaitSemaphore);

    // Mutex
    pthread_mutex_destroy(&_clientMutex);
}


AT_CMD_STATUS CATManager::start(const char* pcModemTty, uint32_t uiTimeoutSec)
{
    LOGD("%s", __FUNCTION__);

    assert(!_bStarted);

    // Create file descriptors
    int iFd;

    // Keep the modem Tty requested
    _strModemTty = pcModemTty;

    // FdStmd
    // Use a retry mechanism for STMD connection (possible race condition)!
    uint32_t iMaxConnectionAttempts = MAX_WAIT_FOR_STMD_CONNECTION_SECONDS * 1000 / STMD_CONNECTION_RETRY_TIME_MS;

    while (iMaxConnectionAttempts-- != 0) {

        // Try to connect
        iFd = socket_local_client(SOCKET_NAME_MODEM_STATUS, ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM);

        if (iFd >= 0) {

            break;
        }
        // Wait
        usleep(STMD_CONNECTION_RETRY_TIME_MS * 1000);
    }
    // Check for uccessfull connection
    if (iFd < 0) {

        LOGE("Failed to connect to modem-status socket %s", strerror(errno));

        stop();

        return AT_CMD_UNABLE_TO_OPEN_DEVICE;
    }
    // Add & Listen
    _pEventThread->addOpenedFd(FdStmd, iFd, true);

    // First modem status wait semaphore
    bzero(&_firstModemStatusReceivedSemaphore, sizeof(_firstModemStatusReceivedSemaphore));
    sem_init(&_firstModemStatusReceivedSemaphore, 0, 0);
    // Record
    _bFirstModemStatusReceivedSemaphoreCreated = true;

    // Start thread
    if (!_pEventThread->start()) {

        LOGE("Failed to create event thread");

        stop();

        return AT_CMD_UNABLE_TO_CREATE_THREAD;
    }

    // Wait for first modem status
    if (!waitSemaphore(&_firstModemStatusReceivedSemaphore, MAX_TIME_MODEM_STATUS_CHECK_SECONDS)) {

        // Unable to get modem status, log
        LOGE("Unable to get modem status after more than %d seconds", MAX_TIME_MODEM_STATUS_CHECK_SECONDS);

        stop();

        return AT_CMD_ERROR;
    }

    // Timeout
    _uiTimeoutSec = uiTimeoutSec;

    // State
    _bStarted = true;

    return AT_CMD_OK;
}

//
// Client thread context
// Add Periodic AT Command to the list
//
void CATManager::addPeriodicATCommand(CPeriodicATCommand* pPeriodicATCommand)
{
    LOGD("%s", __FUNCTION__);

    // Block {
    pthread_mutex_lock(&_clientMutex);

    _periodicATList.push_back(pPeriodicATCommand);

    // } Block
    pthread_mutex_unlock(&_clientMutex);
}

//
// Client thread context
// Remove Periodic AT Command from the list
//
void CATManager::removePeriodicATCommand(CPeriodicATCommand* pPeriodicATCommand)
{
    LOGD("%s", __FUNCTION__);

    // Block {
    pthread_mutex_lock(&_clientMutex);

    _periodicATList.remove(pPeriodicATCommand);

    // } Block
    pthread_mutex_unlock(&_clientMutex);
}

//
// Client thread context
// Some AT command initiated by the modem (also known as Unsollicited)
// requires to send a registration command to be able to receive these
// unsollicited command.
// Upon request of the client, an unsollicited command can be added to the
// unsollicited command list.
// As a result, it will be send to the modem once it will be up and resend
// automatically after each reset/cold reset of the modem
//
void CATManager::addUnsollicitedATCommand(CUnsollicitedATCommand* pUnsollicitedATCommand)
{
    LOGD("%s", __FUNCTION__);

    // Block {
    pthread_mutex_lock(&_clientMutex);

    _unsollicitedATList.push_back(pUnsollicitedATCommand);

    // If tty opened, push the command
    if(_bTtyListenersStarted)
    {
        // Push the command to the tosend list
        pushCommandToSendList(pUnsollicitedATCommand);

        // If no transaction on going, trig the process
        LOGD("%s: trig right now", __FUNCTION__);
        _pEventThread->trig();
    }
    // Else nothing to do, will be done on TTY activation

    // } Block
    pthread_mutex_unlock(&_clientMutex);
}

//
// Client thread context
// Remove Unsollicited AT Command from the list
// Client does not need to received any more this specific unsollicited
// command from the modem.
// Remove it from the Unsollicited AT Command list
//
void CATManager::removeUnsollicitedATCommand(CUnsollicitedATCommand* pUnsollicitedATCommand)
{
    LOGD("%s", __FUNCTION__);

    // Block {
    pthread_mutex_lock(&_clientMutex);

    _unsollicitedATList.remove(pUnsollicitedATCommand);

    // } Block
    pthread_mutex_unlock(&_clientMutex);
}


//
// Client thread context
//
AT_CMD_STATUS CATManager::sendCommand(CATCommand* pATCommand, bool bSynchronous)
{
    LOGD("%s", __FUNCTION__);

    assert(_bStarted);

    AT_CMD_STATUS eStatus = AT_CMD_OK;

    // Block {
    pthread_mutex_lock(&_clientMutex);

    // Check Modem is accessible
    if (!_bTtyListenersStarted) {

        eStatus = AT_CMD_WRITE_ERROR;
        goto error;
    }

    if (_bClientWaiting) {

        eStatus = AT_CMD_BUSY;
        goto error;
    }

    // Push the command to the send list
    pushCommandToSendList(pATCommand);

    if(bSynchronous)
    {
        // Set the client wait sema flag
        _bClientWaiting = true;

        // Set the AT Cmd for which a transaction end is awaited
        _pAwaitedTransactionEndATCommand = pATCommand;

    }

    // Trig the processing of the list
    _pEventThread->trig();

    // } Block
    pthread_mutex_unlock(&_clientMutex);


    return bSynchronous ? waitEndOfTransaction(pATCommand) : eStatus;

error:
    // } Block
    pthread_mutex_unlock(&_clientMutex);

    return eStatus;

}

AT_CMD_STATUS CATManager::waitEndOfTransaction(CATCommand* pATCommand)
{
    assert(pATCommand);

    // Wait
    waitSemaphore(&_clientWaitSemaphore, _uiTimeoutSec);

    // Block {
    pthread_mutex_lock(&_clientMutex);

    // Then check answer status
    AT_CMD_STATUS eCommandStatus = pATCommand->isAnswerOK() ? AT_CMD_OK : AT_CMD_READ_ERROR;

    // Consume
    _pAwaitedTransactionEndATCommand = NULL;
    _bClientWaiting = false;

    // Deal with race conditions
    while (!sem_trywait(&_clientWaitSemaphore));

    // } Block
    pthread_mutex_unlock(&_clientMutex);

    return eCommandStatus;
}

// Wait semaphore with timeout
// Returns false in case of timeout
bool CATManager::waitSemaphore(sem_t* pSemaphore, uint32_t uiTimeoutSec)
{
    struct timespec ts;

    // Current time
    clock_gettime(CLOCK_REALTIME, &ts);

    // Add timeout
    ts.tv_sec += uiTimeoutSec;

    // Wait
    return !sem_timedwait(pSemaphore, &ts);
}

// Stop
void CATManager::stop()
{
    LOGD("%s", __FUNCTION__);

    // Stop Thread
    _pEventThread->stop();

    // Close descriptors
    _pEventThread->closeAndRemoveFd(FdToModem);
    _pEventThread->closeAndRemoveFd(FdFromModem);
    _pEventThread->closeAndRemoveFd(FdStmd);

    // Maintain TTY status accordingly
    _bTtyListenersStarted = false;

    // Reset Modem state
    _bModemAlive = false;

    // AT parser
    _pATParser->clear();

    // First modem status semaphore
    if (_bFirstModemStatusReceivedSemaphoreCreated) {

        // Destroy
        sem_destroy(&_firstModemStatusReceivedSemaphore);

        // Record
        _bFirstModemStatusReceivedSemaphoreCreated = false;
    }

    // Current command
    _pCurrentATCommand = NULL; // Ideally we would have a mean to cancel AT commands

    // Record state
    _bStarted = false;
}

// Get AT Notifier
IATNotifier *CATManager::getATNotifier()
{
    return _pIATNotifier;
}


int CATManager::getModemStatus() const
{
    return _mModemStatus;
}

// terminate the transaction
void CATManager::terminateTransaction(bool bSuccess)
{
    LOGD("%s on %s", __FUNCTION__, _strModemTty.c_str());

    if (_pCurrentATCommand) {

        // Clear timeout retry counter
        _iTimeoutRetry = 0;

        // Record failure status
        _pCurrentATCommand->setAnswerOK(bSuccess);

        // Is a client waiting for this command to terminate?
        if (_bClientWaiting && _pAwaitedTransactionEndATCommand == _pCurrentATCommand) {

            // Warn client
            sem_post(&_clientWaitSemaphore);
        }
        else {
            // Nobody is waiting for this command, clear the answer and status
            LOGD("%s: (%s): %s %s", __FUNCTION__, _pCurrentATCommand->getCommand().c_str(), bSuccess ? "received answer" : " send failed", _pCurrentATCommand->getAnswer().c_str());
            _pCurrentATCommand->clearStatus();
        }
        // Consume
        _pCurrentATCommand = NULL;
    }
    // Clear timeout
    _pEventThread->setTimeoutMs(INFINITE_TIMEOUT);

    LOGD("%s: out", __FUNCTION__);
}

//
// Worker thread context
// Event processing
//
bool CATManager::onEvent(int iFd)
{
    LOGD("%s on %s", __FUNCTION__, _strModemTty.c_str());

    bool bFdChanged;

    // Block {
    pthread_mutex_lock(&_clientMutex);

    if (iFd == _pEventThread->getFd(FdStmd)) {

        // EFromSTMD
        updateModemStatus();

        // FD list changed
        bFdChanged = true;

    } else {

        assert(_bTtyListenersStarted);
        // FdFromModem
        readResponse();

        // FD list not changed
        bFdChanged = false;

        // Could have been an unsollocited cmd
        // So, check if a command is on going
        checksAndProcessSendList();
    }
    // } Block
    pthread_mutex_unlock(&_clientMutex);

    return bFdChanged;
}

//
// Worker thread context
//
bool CATManager::onError(int iFd)
{
    LOGD("%s on %s", __FUNCTION__, _strModemTty.c_str());

    bool bFdChanged;

    // Block {
    pthread_mutex_lock(&_clientMutex);
    // Concerns any TTY?
    if (iFd == _pEventThread->getFd(FdFromModem) || iFd == _pEventThread->getFd(FdToModem)) {

        // Stop receiving AT answer (if AT cmd was on going)
        terminateTransaction(false);

        // Stop the listeners on modem TTYs
        stopModemTtyListeners();

        // We'll need a modem up event to reopen them
        // FD list changed
        bFdChanged = true;

        if (_bModemAlive) {

            // Modem is alive and we got a POLLERR, arms a timeout
            // to perform a recovery (ie a modem reset) if modem
            // is still UP after timeout.
            // If modem is DOWN after the timeout, wait for next up
            LOGE("%s: Modem still alive, try to recover after %d ms", __FUNCTION__, RECOVER_TIMEOUT_MS);
            _pEventThread->setTimeoutMs(RECOVER_TIMEOUT_MS);
        }

    } else {
        // FD list not changed
        bFdChanged = false;
    }
    // } Block
    pthread_mutex_unlock(&_clientMutex);

    return bFdChanged;
}

//
// Worker thread context
//
bool CATManager::onHangup(int iFd)
{
    // Treat as error
    return onError(iFd);
}

//
// Worker thread context
//
void CATManager::onTimeout()
{
    LOGD("%s on %s", __FUNCTION__, _strModemTty.c_str());

    // Block {
    pthread_mutex_lock(&_clientMutex);

    // Timeout case 1:
    // Modem alive, TTy closed -> cleanup request
    if (_bModemAlive && !_bTtyListenersStarted) {

        // Clear timeout
        _pEventThread->setTimeoutMs(INFINITE_TIMEOUT);

        // Recovery needed: ask for a reset of the modem
        LOGE("%s: Modem is alive, %s closed -> TRYING RECOVER", __FUNCTION__, _strModemTty.c_str());
        sendRequestCleanup();

        goto finish;
    }

    // Timeout case 2:
    // AT command sent to the modem without answer
    if (_pCurrentATCommand) {

        // Increment timeout counter
        _iTimeoutRetry += 1;

        if (_iTimeoutRetry < MAX_RETRY_AFTER_TIMEOUT) {

            LOGE("%s: retry #%d -> try again", __FUNCTION__, _iTimeoutRetry);
            // Retry to send the command
            resendCurrentCommand();

            goto finish;
        }

        // Stop receiving AT answer (if AT cmd was on going ie _pCurrentATCommand is set)
        terminateTransaction(false);

        // Stop the listeners on modem TTYs
        stopModemTtyListeners();

        if (_bModemAlive) {

            // Modem is alive but cannot get answer for AT command after 5 retries.
            LOGE("%s: %d retries failed, trying to RECOVER ", __FUNCTION__, MAX_RETRY_AFTER_TIMEOUT);
            _pEventThread->setTimeoutMs(INFINITE_TIMEOUT);

            sendRequestCleanup();
        }

        goto finish;
    }

    // Timeout case 3:
    // periodic AT command to be sent
    // Process the cmd list
    processSendList();

finish:
    // } Block
    pthread_mutex_unlock(&_clientMutex);
}

//
// Worker thread context
//
void CATManager::onPollError()
{
    LOGE("EventThread: poll error");

    // Block {
    pthread_mutex_lock(&_clientMutex);

    // Stop receiving AT answer
    terminateTransaction(false);

    // } Block
    pthread_mutex_unlock(&_clientMutex);
}

//
// Worker thread context
//
void CATManager::onProcess()
{
    LOGD("%s on %s", __FUNCTION__, _strModemTty.c_str());

    // Block {
    pthread_mutex_lock(&_clientMutex);

    assert(_bTtyListenersStarted);

    // Checks and process if need the send list
    checksAndProcessSendList();

    // } Block
    pthread_mutex_unlock(&_clientMutex);
}

// Push AT command to the send list
void CATManager::pushCommandToSendList(CATCommand* pATCommand)
{
    LOGD("%s", __FUNCTION__);

    // push ATCommand at the end of the tosend list
    _toSendATList.push_back(pATCommand);
}

// Pop AT command from the send list
CATCommand* CATManager::popCommandFromSendList(void)
{
    LOGD("%s", __FUNCTION__);

    assert(!_toSendATList.empty());

    // Get the first element of the tosend list
    CATCommand* atCmd = _toSendATList.front();

    // Delete the first element from the tosend list
    _toSendATList.pop_front();

    return atCmd;
}

void CATManager::resendCurrentCommand()
{
    LOGD("%s on %s", __FUNCTION__, _strModemTty.c_str());

    assert(_pCurrentATCommand);

    // Send the command
    if (!sendString(_pCurrentATCommand->getCommand().c_str(), _pEventThread->getFd(FdToModem))) {

        LOGE("%s: Could not write AT command", __FUNCTION__);
        terminateTransaction(false);
        return ;
    }
    // End of line
    if (!sendString("\r\n", _pEventThread->getFd(FdToModem))) {

        LOGE("%s: Could not write AT command", __FUNCTION__);
        terminateTransaction(false);
        return ;
    }
}


//
// Worker thread context
// Checks if no activity in on going and
// process the send command list
//
void CATManager::checksAndProcessSendList()
{
    // To Ensure serialization of the communication with modem
    // by checking if a transaction is on going before processing
    // the tosend list
    if (!_pCurrentATCommand) {

        // Process the tosend command list
        processSendList();
    }
}

//
// Worker thread context
// Process the send command list
//
void CATManager::processSendList()
{
    LOGD("%s on %s", __FUNCTION__, _strModemTty.c_str());

    // If the tosend command list is empty, bailing out
    if(_toSendATList.empty()) {

        // if periodic command(s) to be processed, update
        // timeout accordingly
        _pEventThread->setTimeoutMs(getNextPeriodicTimeout());
        return;
    }

    // Poping cmd from the tosend List
    CATCommand* pATCommand = popCommandFromSendList();

    // Clear command status
    pATCommand->clearStatus();

    // Keep command
    _pCurrentATCommand = pATCommand;

    // Set the timeout
    _pEventThread->setTimeoutMs(AT_ANSWER_TIMEOUT_MS);

    // Send the command
    if (!sendString(pATCommand->getCommand().c_str(), _pEventThread->getFd(FdToModem))) {

        LOGE("%s: Could not write AT command", __FUNCTION__);
        terminateTransaction(false);
        return ;
    }
    // End of line
    if (!sendString("\r\n", _pEventThread->getFd(FdToModem))) {

        LOGE("%s: Could not write AT command", __FUNCTION__);
        terminateTransaction(false);
        return ;
    }

    LOGD("Sent: %s", pATCommand->getCommand().c_str());
}

//
// Worker thread context
// Get Next Periodic Timeout
// Parse the list of Periodic command and return
// the nearest deadline from now.
//
int32_t CATManager::getNextPeriodicTimeout() const
{
    LOGD("%s", __FUNCTION__);

    // Check if periodic cmd has been added to the list
    if(_periodicATList.empty())
        return INFINITE_TIMEOUT;

    // Parse the list of Periodic cmd to find next timeout to set
    CPeriodicATCommandConstListIterator iti;
    for (iti = _periodicATList.begin(); iti != _periodicATList.end(); ++iti) {

        CPeriodicATCommand* pCmd = *iti;
    }
    return INFINITE_TIMEOUT;
}


void CATManager::processUnsollicited(const string& strResponse)
{
    CUnsollicitedATCommand* pUnsollicitedCmd;

    // Check answer adequation with current Unsollicited command list
    if ((pUnsollicitedCmd = findUnsollicitedCmdByPrefix(strResponse)) != NULL) {

        pUnsollicitedCmd->addAnswerFragment(strResponse);

        pUnsollicitedCmd->doProcessAnswer();

        if (getATNotifier()) {

            getATNotifier()->onUnsollicitedReceived(pUnsollicitedCmd);
        }
    }
}

//
// Worker thread context
// Get AT response
//
void CATManager::readResponse()
{
    LOGD("%s IN", __FUNCTION__);

    _pATParser->receive(_pEventThread->getFd(FdFromModem));

    string strResponse;

    // Get responses
    while (_pATParser->extractReceivedSentence(strResponse)) {

        LOGD("Received %s", strResponse.c_str());

        // Check for AT command in progress
        if (!_pCurrentATCommand || _pCurrentATCommand->isComplete()) {

            LOGD("=> Unsollicited");

            processUnsollicited(strResponse);

            continue;
        }

        // Check for success / error first
        if ((strResponse == "OK") || (strResponse == "ERROR")) {

            // Add answer fragment to AT command object
            _pCurrentATCommand->addAnswerFragment(strResponse);

            // Stop receiving AT answer
            terminateTransaction(strResponse == "OK");

        // Check answer adequation to current AT command
        } else if (cmdHasPrefixAndMatches(_pCurrentATCommand, strResponse)) {

            // Match
            // Add answer fragment to AT command object
            _pCurrentATCommand->addAnswerFragment(strResponse);

            // Set a timeout to receive the whole AT answer
            _pEventThread->setTimeoutMs(AT_ANSWER_TIMEOUT_MS);

        // At last, check if an unsollicited cmd matches the answer
        } else {

            LOGD("=> Unsollicited while currentCmd ongoing");

            processUnsollicited(strResponse);
        }
    }
    LOGD("%s OUT", __FUNCTION__);
}

// Send String
bool CATManager::sendString(const char* pcString, int iFd)
{
    int iStringLength = strlen(pcString);

    // Send
    int iNbWrittenChars = write(iFd, pcString, iStringLength);

    // Check for success
    if (iNbWrittenChars != iStringLength) {

        // log
        LOGE("Unable to send full amount of bytes: %d instead of %d", iNbWrittenChars, iStringLength);

        return false;
    }
    // Flush
    fsync(iFd);

    return true;
}

// Update modem status
void CATManager::updateModemStatus()
{
    uint32_t uiStatus;

    // Read status
    int iNbReadChars = read(_pEventThread->getFd(FdStmd), &uiStatus, sizeof(uiStatus));

    if (iNbReadChars != sizeof(uiStatus)) {

        // Failed!
        LOGE("Modem status handler: wrong size [%d]\n", iNbReadChars);

        return;
    }
    // Acknowledge the cold reset to STMD so that it can perform the reset
    if(uiStatus == MODEM_COLD_RESET) {

        sendModemColdResetAck();
    }

    // Update status
    _bModemAlive = uiStatus == MODEM_UP;

    // Reset the timeout
    _pEventThread->setTimeoutMs(INFINITE_TIMEOUT);

    LOGD("Modem status: %s", _bModemAlive ? "UP" : "DOWN");

    // Take care of current request
    if (!_bModemAlive) {

        // Stop receiving AT answer
        terminateTransaction(false);

        // Stop the listeners on modem TTYs
        stopModemTtyListeners();

    } else {
        // Start the listeners on modem TTYs
        startModemTtyListeners();
    }

    setModemStatus(uiStatus);

    // Warn about status reception
    assert(_bFirstModemStatusReceivedSemaphoreCreated);
    // Modem status wait semaphore
    sem_post(&_firstModemStatusReceivedSemaphore);
}

void CATManager::setModemStatus(uint32_t status)
{
    LOGD("%s", __FUNCTION__);

    if (status == MODEM_UP || status == MODEM_DOWN || status == MODEM_COLD_RESET)
        _mModemStatus = status;
    else
        _mModemStatus = MODEM_DOWN;

    LOGD("Modem status received: %d", _mModemStatus);

    /* Informs of the modem state to who implements the observer class */
    if (getATNotifier()) {

        getATNotifier()->onModemStateChanged();
    }
}

bool CATManager::sendModemColdResetAck()
{
    uint32_t uiData = MODEM_COLD_RESET_ACK;

    int iNbSentChars = write(_pEventThread->getFd(FdStmd), &uiData, sizeof(uiData));

    if (iNbSentChars != sizeof(uiData)) {

        LOGE("Could not send MODEM_COLD_RESET ACK\n");

        return false;
    }
    return true;
}

// Called in the context of the Event Listener thread
bool CATManager::startModemTtyListeners()
{
    LOGD("%s", __FUNCTION__);

    if (!_bTtyListenersStarted) {

        // Create file descriptors
        int iFd;
        LOGD("opening read tty %s...", _strModemTty.c_str());

        // FdFromModem
        iFd = CTtyHandler::openTty(_strModemTty.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
        if (iFd < 0) {

            LOGE("Unable to open device for reading: %s, error: %s", _strModemTty.c_str(), strerror(errno));

            return false;
        }
        // Add & Listen
        _pEventThread->addOpenedFd(FdFromModem, iFd, true);

        LOGD("opening write tty %s...", _strModemTty.c_str());
        // FdToModem
        iFd = CTtyHandler::openTty(_strModemTty.c_str(), O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (iFd < 0) {

            LOGE("Unable to open device for writing: %s, error: %s", _strModemTty.c_str(), strerror(errno));

            // Close FdFromModem
            _pEventThread->closeAndRemoveFd(FdFromModem);

            return false;
        }
        // Add & Listen
        _pEventThread->addOpenedFd(FdToModem, iFd);

        LOGD("%s: modem Tty succesfully added to listener thread", __FUNCTION__);

        // Record state
        _bTtyListenersStarted = true;

        // Performs now awaiting actions
        onTtyStateChanged(true);
    }

    return true;
}

// Called in the context of the Event Listener thread
void CATManager::stopModemTtyListeners()
{
    if (_bTtyListenersStarted) {

        LOGD("%s: %s", __FUNCTION__, _strModemTty.c_str());

        // Close descriptors
        _pEventThread->closeAndRemoveFd(FdToModem);
        _pEventThread->closeAndRemoveFd(FdFromModem);

        LOGD("%s: modem Tty removed from listener thread", __FUNCTION__);

        // Record state
        _bTtyListenersStarted = false;

        // Performs some actions
        onTtyStateChanged(false);
    }
}

void CATManager::clearToSendList()
{
    _toSendATList.clear();
}

//
// Worker thread context
//
void CATManager::onTtyStateChanged(bool available)
{
    LOGD("%s", __FUNCTION__);

    if (!available) {

        // Modem just starts, (or restarts)
        // Clear tosend list
        clearToSendList();
        return ;
    }

    // Send the Unsollicited command from the list
    CUnsollicedATCommandListIterator it;
    for (it = _unsollicitedATList.begin(); it != _unsollicitedATList.end(); ++it) {

        CUnsollicitedATCommand* pCmd = *it;

        // Push the command to the tosend list
        pushCommandToSendList(pCmd);
    }

    // Send the periodic command from the list
    CPeriodicATCommandListIterator iti;
    for (iti = _periodicATList.begin(); iti != _periodicATList.end(); ++iti) {

        CPeriodicATCommand* pCmd = *iti;

        // Push the command to the tosend list
        pushCommandToSendList(pCmd);
    }

    // Process the cmd list
    processSendList();

}

CUnsollicitedATCommand* CATManager::findUnsollicitedCmdByPrefix(const string& strRespPrefix) const
{
    LOGD("%s: respPrefix =(%s)", __FUNCTION__, strRespPrefix.c_str());

    CUnsollicedATCommandConstListIterator it;

    for (it = _unsollicitedATList.begin(); it != _unsollicitedATList.end(); ++it) {

        CUnsollicitedATCommand* pCmd = *it;

        if (cmdHasPrefixAndMatches(pCmd, strRespPrefix))
        {
            return pCmd;
        }
    }
    LOGD("%s: NOT FOUND -> UNSOLLICITED UNKNOWN", __FUNCTION__);
    return NULL;
}

//
// Send Cleanup Request to the modem
//
bool CATManager::sendRequestCleanup()
{
    LOGD("%s: RECOVER PROCEDURE", __FUNCTION__);

    int fdCleanupSocket;
    bool ret = false;

    uint32_t iMaxConnectionAttempts = MAX_WAIT_FOR_STMD_CONNECTION_SECONDS * 1000 / STMD_CONNECTION_RETRY_TIME_MS;

    while (iMaxConnectionAttempts-- != 0) {

        // Try to connect
        fdCleanupSocket = socket_local_client(SOCKET_NAME_CLEAN_UP, ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM);

        if (fdCleanupSocket >= 0) {

            break;
        }
        // Wait
        usleep(STMD_CONNECTION_RETRY_TIME_MS * 1000);
    }
    // Check for uccessfull connection
    if (fdCleanupSocket < 0) {

        LOGE("Failed to connect to Cleanup socket %s", strerror(errno));

        return false;
    }
    uint32_t uiData = REQUEST_CLEANUP;

    int iNbSentChars = write(fdCleanupSocket, &uiData, sizeof(uiData));

    if (iNbSentChars != sizeof(uiData)) {

        LOGE("Could not send CLEANUP REQUEST\n");

        ret = false;
    } else {

        LOGD("%s: CLEANUP REQUEST success", __FUNCTION__);
        ret = true;
    }

    close(fdCleanupSocket);
    return ret;

}
