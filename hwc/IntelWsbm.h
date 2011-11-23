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
#ifndef __INTEL_WSBM_H__
#define __INTEL_WSBM_H__

#include <IntelWsbmWrapper.h>

/**
 * Class: WSBM
 * A wrapper class to use libwsbm functionalities
 */
class IntelWsbm
{
private:
    int mDrmFD;
public:
    IntelWsbm(int drmFD);
    ~IntelWsbm();
    bool initialize();
    bool allocateTTMBuffer(uint32_t size, uint32_t align,void ** buf);
    bool destroyTTMBuffer(void * buf);
    void * getCPUAddress(void * buf);
    uint32_t getGttOffset(void * buf);
    bool wrapTTMBuffer(uint32_t handle, void **buf);
    bool unreferenceTTMBuffer(void *buf);
    uint32_t getKBufHandle(void *buf);
};
#endif /*__INTEL_WSBM_H__*/
