/*
 **
 ** Copyright 2011 Intel Corporation
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **      http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

//small rework for a new upload
#include <stdio.h>
#include <sys/poll.h>
#define _POSIX_C_SOURCE
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <linux/soundcard.h>
#define LOG_TAG "ALSAModemModule"
#include <utils/Log.h>
#include <sys/time.h>

#define MEDFIELDAUDIO "medfieldaudio"
#define MELFIELDALSAIFX "IntelALSAIFX"

typedef struct snd_pcm_modem {
    snd_pcm_ioplug_t io;
    char *device;
    int fd;
    snd_pcm_t *pcm_md_handle;
    int fragment_set;
    int caps;
    int format;
    int ptr;
    unsigned int period_shift;
    unsigned int periods;
    unsigned int frame_bytes;
    unsigned int last_size;
} snd_pcm_modem_t;

struct alsa_handle_t {
    uint32_t            devices;
    uint32_t            curDev;
    int                 curMode;
    snd_pcm_t *         handle;
    snd_pcm_format_t    format;
    uint32_t            channels;
    uint32_t            sampleRate;
    unsigned int        latency;         // Delay in usec
    unsigned int        bufferSize;      // Size of sample buffer
    void *              modPrivate;
};

static int period_frames_t;
static void  xrun(snd_pcm_t *handle);
static int xrun_recovery(snd_pcm_t *handle, int err);
static int setHardwareParams(struct alsa_handle_t *handle);
static int modem_ifx_open(snd_pcm_ioplug_t *io);
static int modem_ifx_close(snd_pcm_ioplug_t *io);

static ssize_t pcm_write(snd_pcm_t *pcm_handle, const char *data, size_t count,size_t bytes_per_frame)
{
    ssize_t r;
    ssize_t result = 0;
    int err;
    size_t frame_num = count;

    while (frame_num > 0) {
        r = snd_pcm_writei (pcm_handle, data, frame_num);
        if (r == -EAGAIN) {
            LOGE("EAGIN wait r is %d\n",(int)r);
            //snd_pcm_wait(pcm_handle, 1000);
            continue;
        } else if (r == -EPIPE) {
            LOGE("EPIPE xrun r is %d\n",(int)r);
            xrun(pcm_handle);
            continue;
        } else if (r < 0) {
            LOGE("OTHER xrun r is %d\n",(int)r);
            if ((err = xrun_recovery(pcm_handle, r)) < 0) {
                LOGE("xrun_recovery failed\n");
                return err;
            }
            continue;
        }
        result += r;
        frame_num -= r;
        data += r* bytes_per_frame ;
    }

    return result;
}

static snd_pcm_sframes_t modem_write(snd_pcm_ioplug_t *io,
                                     const snd_pcm_channel_area_t *areas,
                                     snd_pcm_uframes_t offset,
                                     snd_pcm_uframes_t size)
{
    snd_pcm_modem_t *modem = io->private_data;
    int err;
    const char *buf = NULL;
    ssize_t result;
    int bytes_per_frame=io->channels*2; //only support 16 bits format

    if(!modem->pcm_md_handle) {
        err = modem_ifx_open(io);
        if(err < 0)
            return err;
    }

    /* we handle only an interleaved buffer */
    buf = (char *)areas->addr + (areas->first + areas->step * offset) / 8;

    result = pcm_write (modem->pcm_md_handle, buf, size, bytes_per_frame );
    if (result <= 0) {
        LOGE("%s out error \n", __func__);
        return result;
    }

    return result;
}

static snd_pcm_sframes_t modem_read(snd_pcm_ioplug_t *io,
                                    const snd_pcm_channel_area_t *areas,
                                    snd_pcm_uframes_t offset,
                                    snd_pcm_uframes_t size)
{
    snd_pcm_modem_t *modem = io->private_data;
    char *buf;
    ssize_t result;
    int err;

    if(!modem->pcm_md_handle) {
        err = modem_ifx_open(io);
        if(err < 0)
            return err;
    }

    /* we handle only an interleaved buffer */
    buf = (char *)areas->addr + (areas->first + areas->step * offset) / 8;
    result = snd_pcm_readi(modem->pcm_md_handle, buf, size);
    if (result <= 0) {
        LOGE("%s out error \n", __func__);
        return result;
    }

    return result;
}

static snd_pcm_sframes_t modem_pointer(snd_pcm_ioplug_t *io)
{
    snd_pcm_modem_t *modem = io->private_data;
    int err, ptr, size;

    assert(modem);

    if(!modem->pcm_md_handle) {
        err = modem_ifx_open(io);
        if(err < 0)
            return err;
    }

    size = snd_pcm_avail(modem->pcm_md_handle);
    if(size < 0)
        return size;

    modem->ptr += size - modem->last_size;
    modem->ptr %= io->buffer_size;

    modem->last_size = size;
    ptr = modem->ptr;

    return ptr;
}

static int modem_start(snd_pcm_ioplug_t *io)
{
    snd_pcm_modem_t *modem = io->private_data;
    int err;

    LOGD("%s in \n", __func__);

    if(!modem->pcm_md_handle) {
        err = modem_ifx_open(io);
        if(err < 0)
            return err;
    }
    return 0;
}

static int modem_stop(snd_pcm_ioplug_t *io)
{
    snd_pcm_modem_t *modem = io->private_data;

    LOGD("%s in \n", __func__);

    modem_ifx_close(io);

    return 0;
}

static int modem_drain(snd_pcm_ioplug_t *io)
{
    snd_pcm_modem_t *modem = io->private_data;

    LOGD("%s in \n", __func__);

    if(modem->pcm_md_handle)
        snd_pcm_drain(modem->pcm_md_handle);

    return 0;
}

static int modem_prepare(snd_pcm_ioplug_t *io)
{
    snd_pcm_modem_t *modem = io->private_data;
    int tmp, err;
    LOGD("%s in \n", __func__);

    if(io->stream == SND_PCM_STREAM_PLAYBACK) {
        modem->ptr = 0;
        modem->last_size = 0;
    } else {
        modem->ptr = 0;
        modem->last_size = io->buffer_size;
    }

    LOGD("%s  modem-ptr = %d io->bufsize = %d in \n", __func__, (int)modem->ptr, (int)io->buffer_size);

    if (modem->pcm_md_handle && (err = snd_pcm_prepare (modem->pcm_md_handle)) < 0) {
        LOGE ("cannot prepare audio interface for use \n");
        return err;
    }

    return 0;
}

static int modem_hw_params(snd_pcm_ioplug_t *io,
                           snd_pcm_hw_params_t *params ATTRIBUTE_UNUSED)
{
    snd_pcm_modem_t *modem = io->private_data;

    LOGD("%s in \n", __func__);

    return 0;
}

#define ARRAY_SIZE(ary) (sizeof(ary)/sizeof(ary[0]))

/* I/O error handler */
static void  xrun(snd_pcm_t *handle)
{
    LOGD("%s in \n", __func__);
    int err;
    err = snd_pcm_prepare(handle);
    if (err < 0)
        LOGE("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
    return;
}

static int xrun_recovery(snd_pcm_t *handle, int err)
{
    int result = 0;

    LOGD("%s in \n", __func__);
    if (err == -EPIPE) {    /* under-run */
        result = snd_pcm_prepare(handle);
        if (result < 0)
            LOGE("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
    } else if (err == -ESTRPIPE) {
        while ((result = snd_pcm_resume(handle)) == -EAGAIN)
            sleep(1);       /* wait until the suspend flag is released */
        if (result < 0) {
            result = snd_pcm_prepare(handle);
            if (result < 0)
                LOGE("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
        }
    }

    return result;
}


static int setHardwareParams(struct alsa_handle_t *handle)
{
    snd_pcm_hw_params_t *hardwareParams;
    int err;

    snd_pcm_uframes_t bufferSize = handle->bufferSize;
    unsigned int requestedRate = handle->sampleRate;
    unsigned int latency = handle->latency;

    unsigned int buffer_time = 0;
    unsigned int period_time = 0;
    unsigned int val=0;

    if ( (err = snd_pcm_hw_params_malloc(&hardwareParams)) < 0) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA hardware parameters!");
        return err;
    }

    err = snd_pcm_hw_params_any(handle->handle, hardwareParams);
    if (err < 0) {
        LOGE("Unable to configure hardware: %s", snd_strerror(err));
        goto done;
    }

    // Set the interleaved read and write format.
    err = snd_pcm_hw_params_set_access(handle->handle, hardwareParams,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        LOGE("Unable to configure PCM read/write format: %s",
             snd_strerror(err));
        goto done;
    }

    err = snd_pcm_hw_params_set_format(handle->handle, hardwareParams,
                                       handle->format);
    if (err < 0) {
        goto done;
    }

    err = snd_pcm_hw_params_set_channels(handle->handle, hardwareParams,
                                         handle->channels);
    if (err < 0) {
        LOGE("Unable to set channel count to %i: %s",
             handle->channels, snd_strerror(err));
        goto done;
    } else {
        LOGE("set channel count to %i ",
             handle->channels);

    }

    err = snd_pcm_hw_params_set_rate_near(handle->handle, hardwareParams,
                                          &requestedRate, 0);

    if (err < 0)
        LOGE("Unable to set  sample rate to %u: %s",
             handle->sampleRate, snd_strerror(err));
    else if (requestedRate != handle->sampleRate)
        // Some devices have a fixed sample rate, and can not be changed.
        // This may cause resampling problems; i.e. PCM playback will be too
        // slow or fast.
        LOGW("Requested rate (%u HZ) does not match actual rate (%u HZ)",
             handle->sampleRate, requestedRate);
    else
        LOGD("Set sample rate to %u HZ",  requestedRate);

    err = snd_pcm_hw_params_get_rate(hardwareParams, &val, 0);
    if(err <0)
        LOGE("err!!!!!! Set sample rate to %u HZ",  requestedRate);
    LOGD("Get sample rate to %u HZ",  val);
#ifdef DISABLE_HARWARE_RESAMPLING
    // Disable hardware re-sampling.
    err = snd_pcm_hw_params_set_rate_resample(handle->handle,
            hardwareParams,
            static_cast<int>(resample));
    if (err < 0) {
        LOGE("Unable to %s hardware resampling: %s",
             resample ? "enable" : "disable",
             snd_strerror(err));
        goto done;
    }
#endif


    err = snd_pcm_hw_params_get_buffer_time_max(hardwareParams,
            &buffer_time, 0);
    if (buffer_time > 80000)
        buffer_time = 80000;
    period_time = buffer_time / 4;

    err = snd_pcm_hw_params_set_period_time_near(handle->handle, hardwareParams,
            &period_time, 0);
    if (err < 0) {
        LOGE("Unable to set_period_time_near");
        goto done;
    }
    err = snd_pcm_hw_params_set_buffer_time_near(handle->handle, hardwareParams,
            &buffer_time, 0);
    if (err < 0) {
        LOGE("Unable to set_buffer_time_near");
        goto done;
    }

    LOGD("Buffer size: %d", (int)bufferSize);
    LOGD("Latency: %d", (int)latency);

    handle->bufferSize = bufferSize;
    handle->latency = latency;

    // Commit the hardware parameters back to the device.
    err = snd_pcm_hw_params(handle->handle, hardwareParams);
    if (err < 0) LOGE("Unable to set hardware parameters: %s", snd_strerror(err));

    LOGD("%s  out ", __func__);
done:
    snd_pcm_hw_params_free(hardwareParams);

    return err;
}

static int setSoftwareParams(struct alsa_handle_t *handle)
{
    snd_pcm_sw_params_t * softwareParams;
    int err;

    snd_pcm_uframes_t bufferSize = 0;
    snd_pcm_uframes_t periodSize = 0;
    snd_pcm_uframes_t startThreshold, stopThreshold;

    if (snd_pcm_sw_params_malloc(&softwareParams) < 0) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA software parameters!");
        return -1;
    }

    // Get the current software parameters
    err = snd_pcm_sw_params_current(handle->handle, softwareParams);
    if (err < 0) {
        LOGE("Unable to get software parameters: %s", snd_strerror(err));
        goto done;
    }

    // Configure ALSA to start the transfer when the buffer is almost full.
    snd_pcm_get_params(handle->handle, &bufferSize, &periodSize);

    if (handle->devices) {
        // For playback, configure ALSA to start the transfer when the
        // buffer is full.
        startThreshold = bufferSize - 1;
        stopThreshold = bufferSize;
    } else {
        // For recording, configure ALSA to start the transfer on the
        // first frame.
        startThreshold = 1;
        stopThreshold = bufferSize;
    }

    err = snd_pcm_sw_params_set_start_threshold(handle->handle, softwareParams,
            startThreshold);
    if (err < 0) {
        LOGE("Unable to set start threshold to %lu frames: %s",
             startThreshold, snd_strerror(err));
        goto done;
    }

    err = snd_pcm_sw_params_set_stop_threshold(handle->handle, softwareParams,
            stopThreshold);
    if (err < 0) {
        LOGE("Unable to set stop threshold to %lu frames: %s",
             stopThreshold, snd_strerror(err));
        goto done;
    }

    // Allow the transfer to start when at least periodSize samples can be
    // processed.
    err = snd_pcm_sw_params_set_avail_min(handle->handle, softwareParams,
                                          periodSize);
    if (err < 0) {
        LOGE("Unable to configure available minimum to %lu: %s",
             periodSize, snd_strerror(err));
        goto done;
    }

    // Commit the software parameters back to the device.
    err = snd_pcm_sw_params(handle->handle, softwareParams);
    if (err < 0) LOGE("Unable to configure software parameters: %s",
                          snd_strerror(err));

done:
    snd_pcm_sw_params_free(softwareParams);

    return err;
}

static int modem_close(snd_pcm_ioplug_t *io)
{
    snd_pcm_modem_t *modem = io->private_data;
    LOGD("%s in \n", __func__);

    if(modem->pcm_md_handle)
        snd_pcm_close(modem->pcm_md_handle);

    free(modem->device);
    free(modem);

    return 0;
}

static int modem_hw_constraint(snd_pcm_modem_t * pcm)
{
    snd_pcm_ioplug_t *io = &pcm->io;

    static const snd_pcm_access_t access_list[] = {
        SND_PCM_ACCESS_RW_INTERLEAVED
    };
    static const unsigned int formats[] = {
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S16_BE,
    };

    int err;

    err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
                                        ARRAY_SIZE(access_list),
                                        access_list);
    if (err < 0)
        return err;

    err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
                                        ARRAY_SIZE(formats), formats);
    if (err < 0)
        return err;

    err =
        snd_pcm_ioplug_set_param_minmax(io,
                                        SND_PCM_IOPLUG_HW_BUFFER_BYTES,
                                        1, 4 * 1024 * 1024);
    if (err < 0)
        return err;

    err =
        snd_pcm_ioplug_set_param_minmax(io,
                                        SND_PCM_IOPLUG_HW_PERIOD_BYTES,
                                        128, 2 * 1024 * 1024);
    if (err < 0)
        return err;

    err =
        snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS,
                                        3, 1024);
    if (err < 0)
        return err;

    return 0;
}

static int modem_ifx_open(snd_pcm_ioplug_t *io)
{
    snd_pcm_modem_t *modem = io->private_data;
    int i, tmp, err, card;
    unsigned int period_bytes;
    struct alsa_handle_t handle;
    char device_ifx[128];

    card = snd_card_get_index(MELFIELDALSAIFX);
    sprintf(device_ifx, "hw:%d,0", card);

    err = snd_pcm_open(&modem->pcm_md_handle, device_ifx, io->stream, 0);

    if (err < 0) {
        LOGE("Cannot open device %s, direction = %d \n", device_ifx, io->stream);
        return err;
    }

    LOGD("open alsa ifx(hw:1,0): dir = %d ~~~~~~~ \n", io->stream);

    io->private_data = modem;

    memset(&handle, 0, sizeof(struct alsa_handle_t));

    if(io->stream == SND_PCM_STREAM_PLAYBACK) {
        handle.bufferSize = io->buffer_size;
        handle.sampleRate = io->rate;
        handle.latency = io->buffer_size * 1000 / io->rate;
        handle.handle = modem->pcm_md_handle;
        handle.format =  SND_PCM_FORMAT_S16_LE;
        handle.channels = io->channels;
        LOGD("playback io->channels = %d io->rate = %d\n",io->channels, io->rate);
    } else {
        handle.bufferSize = io->buffer_size;
        handle.sampleRate = io->rate;
        handle.latency = io->buffer_size * 1000 / io->rate;
        handle.handle = modem->pcm_md_handle;
        handle.format = SND_PCM_FORMAT_S16_LE;
        handle.channels = io->channels;
        LOGD("capture io->channels = %d io->rate = %d\n",io->channels, io->rate);
    }

    err = setHardwareParams(&handle);
    if(err < 0) {
        LOGE("Set Hareware Params failed\n");
        goto error;
    }

    err = setSoftwareParams(&handle);
    if(err < 0){
        LOGE("Set Software Params failed\n");
	return err;
   }

    return 0;

error:
    snd_pcm_close(modem->pcm_md_handle);
    return err;
}

static int modem_ifx_close(snd_pcm_ioplug_t *io)
{
    snd_pcm_modem_t *modem = io->private_data;

    LOGD("%s in \n", __func__);

    if(modem->pcm_md_handle) {
        snd_pcm_close(modem->pcm_md_handle);
        modem->pcm_md_handle = NULL;
        LOGD("%s close alsa ifx(hw:1,0) ~~~~~~ %d\n", __func__, gettid());
    }

    return 0;
}

static int modem_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
    snd_pcm_modem_t *modem = io->private_data;
    return snd_pcm_poll_descriptors_count(modem->pcm_md_handle);
}

static int modem_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd,
                                  unsigned int space)
{
    snd_pcm_modem_t *modem = io->private_data;
    return snd_pcm_poll_descriptors(modem->pcm_md_handle, pfd, space);
}

static int modem_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
                              unsigned int nfds, unsigned short *revents)
{
   snd_pcm_modem_t *modem = io->private_data;
   return snd_pcm_poll_descriptors_revents(modem->pcm_md_handle, pfd, nfds, revents);
}

static const snd_pcm_ioplug_callback_t modem_playback_callback = {
    .start = modem_start,
    .stop = modem_stop,
    .transfer = modem_write,
    .pointer = modem_pointer,
    .close = modem_close,
    .hw_params = modem_hw_params,
    .prepare = modem_prepare,
    .drain = modem_drain,
    .poll_descriptors_count = modem_poll_descriptors_count,
    .poll_descriptors = modem_poll_descriptors,
    .poll_revents = modem_poll_revents,
};

static const snd_pcm_ioplug_callback_t modem_capture_callback = {
    .start = modem_start,
    .stop = modem_stop,
    .transfer = modem_read,
    .pointer = modem_pointer,
    .close = modem_close,
    .hw_params = modem_hw_params,
    .prepare = modem_prepare,
    .drain = modem_drain,
};


SND_PCM_PLUGIN_DEFINE_FUNC(modem)
{
    snd_config_iterator_t i, next;
    const char *device = NULL;
    int err;
    snd_pcm_modem_t *modem;
    int card;
    char device_ifx[128];

    LOGD("%s in \n", __func__);

    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id = NULL;
        if (snd_config_get_id(n, &id) < 0)
            continue;
        if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
            continue;
        if (strcmp(id, "device") == 0) {
            if (snd_config_get_string(n, &device) < 0) {
                LOGE("Invalid type for %s", id);
                return -EINVAL;
            }
            continue;
        }
        LOGE("Unknown field %s", id);
        return -EINVAL;
    }
    modem = calloc(1, sizeof(*modem));
    if (! modem) {
        LOGE("cannot allocate");
        return -ENOMEM;
    }
    if(device == NULL) {
        LOGW("no modem device name");
        modem->device = NULL;
    } else {
        modem->device = strdup(device);
        if(!modem->device) {
            LOGW("modem->device name is NULL");
        }
    }

    modem->io.version = SND_PCM_IOPLUG_VERSION;
    modem->io.name = "ALSA <-> modem PCM I/O Plugin";
    modem->io.mmap_rw = 0;
    modem->io.callback = stream == SND_PCM_STREAM_PLAYBACK ?
                         &modem_playback_callback : &modem_capture_callback;
    modem->io.private_data = modem;

    err = snd_pcm_ioplug_create(&modem->io, name, stream, mode);
    if (err < 0)
        goto error;

    if ((err = modem_hw_constraint(modem)) < 0) {
        snd_pcm_ioplug_delete(&modem->io);
        goto error;
    }

    *pcmp = modem->io.pcm;


    LOGD("%s out \n", __func__);

    return 0;

error:
    if (modem->pcm_md_handle )
        snd_pcm_close(modem->pcm_md_handle);

    free(modem->device);
    free(modem);
    return err;
}

SND_PCM_PLUGIN_SYMBOL(modem);
