/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <stdarg.h>

#include <IntelHWComposerDump.h>

IntelHWComposerDump::IntelHWComposerDump()
{
}

IntelHWComposerDump::~IntelHWComposerDump()
{
}

void IntelHWComposerDump::dumpPrintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    mDumpLen += vsnprintf(mDumpBuf + mDumpLen, mDumpBuflen - mDumpLen, fmt, ap);
    va_end(ap);
}

bool IntelHWComposerDump::dump(char *buff, int buff_len, int * cur_len)
{
    return true;
}
