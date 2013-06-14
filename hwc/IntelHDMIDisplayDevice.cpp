/*
 * Copyright © 2012 Intel Corporation
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jackie Li <yaodong.li@intel.com>
 *
 */
#include <cutils/log.h>
#include <cutils/atomic.h>

#include <IntelHWComposer.h>
#include <IntelDisplayDevice.h>
#include <IntelOverlayUtil.h>
#include <IntelHWComposerCfg.h>

IntelHDMIDisplayDevice::IntelHDMIDisplayDevice(IntelBufferManager *bm,
                                    IntelBufferManager *gm,
                                    IntelDisplayPlaneManager *pm,
                                    IntelHWComposerDrm *drm,
                                    uint32_t index)
                                  : IntelDisplayDevice(pm, drm, bm, gm, index),
                                    mGraphicPlaneVisible(true)
{
    ALOGD_IF(ALLOW_HWC_PRINT, "%s\n", __func__);

    //check buffer manager
    if (!mBufferManager) {
        ALOGE("%s: Invalid buffer manager\n", __func__);
        goto init_err;
    }

    // check buffer manager for gralloc buffer
    if (!mGrallocBufferManager) {
        ALOGE("%s: Invalid Gralloc buffer manager\n", __func__);
        goto init_err;
    }

    //create new DRM object if not exists
    if (!mDrm) {
        ALOGE("%s: failed to initialize DRM instance\n", __func__);
        goto init_err;
    }

    // check display plane manager
    if (!mPlaneManager) {
        ALOGE("%s: Invalid plane manager\n", __func__);
        goto init_err;
    }

    // create layer list
    mLayerList = new IntelHWComposerLayerList(mPlaneManager);
    if (!mLayerList) {
        ALOGE("%s: Failed to create layer list\n", __func__);
        goto init_err;
    }

    mInitialized = true;
    return;

init_err:
    mInitialized = false;
    return;
}

IntelHDMIDisplayDevice::~IntelHDMIDisplayDevice()
{
    ALOGD_IF(ALLOW_HWC_PRINT, "%s\n", __func__);
}

// When the geometry changed, we need
// 0) reclaim all allocated planes, reclaimed planes will be disabled
//    on the start of next frame. A little bit tricky, we cannot disable the
//    planes right after geometry is changed since there's no data in FB now,
//    so we need to wait FB is update then disable these planes.
// 1) build a new layer list for the changed hwc_layer_list
// 2) attach planes to these layers which can be handled by HWC
void IntelHDMIDisplayDevice::onGeometryChanged(hwc_display_contents_1_t *list)
{
    //TODO: need to implement this function for HDMI display.

    // update layer list with new list
    mLayerList->updateLayerList(list);
    mGraphicPlaneVisible = true;

    intel_overlay_mode_t mode = mDrm->getDisplayMode();
    if (mode == OVERLAY_MIPI0) {
        mGraphicPlaneVisible = false;
    } else if (mode == OVERLAY_EXTEND) {
        for (size_t i = 0; list && i < list->numHwLayers-1; i++) {
            /*When do HDMI extend mode, press power key will start ElectronBeam application. *
              * it will create a Dim layer to do GLES compostion. So mark the Dim layer to overlay *
              *to avoid do GLES  compostion. change for Bug82263      */
            if( (mLayerList->getLayerType(i) ==
                    IntelHWComposerLayer::LAYER_TYPE_INVALID) &&
                 (list->hwLayers[i].flags & HWC_SKIP_LAYER)) {
                list->hwLayers[i].flags &= ~HWC_SKIP_LAYER;
                list->hwLayers[i].compositionType = HWC_OVERLAY;
            }
            if (mLayerList->getLayerType(i) ==
                    IntelHWComposerLayer::LAYER_TYPE_YUV) {
                list->hwLayers[i].compositionType = HWC_OVERLAY;
                list->hwLayers[i].hints = 0;

                // If the seeking is active, ignore the following logic
                if (mVideoSeekingActive)
                    continue;

                // Set graphic plane invisible when
                // 1) the video is placed to a window
                // 2) only video layer exists.(Exclude FramebufferTarget)
                bool isVideoInWin = isVideoPutInWindow(OUTPUT_HDMI, &(list->hwLayers[i]));
                if (isVideoInWin || list->numHwLayers == 2 ||
                        list->flags & HWC_ROTATION_IN_PROGRESS) {
                    ALOGD_IF(ALLOW_HWC_PRINT,
                            "%s: In window mode:%d layer num:%d",
                            __func__, isVideoInWin, list->numHwLayers);
                    // Disable graphic plane
                    mGraphicPlaneVisible = false;
                    struct drm_psb_disp_ctrl dp_ctrl;
                    memset(&dp_ctrl, 0, sizeof(dp_ctrl));
                    dp_ctrl.cmd = DRM_PSB_DISP_PLANEB_DISABLE;
                    drmCommandWriteRead(mDrm->getDrmFd(), DRM_PSB_HDMI_FB_CMD, &dp_ctrl, sizeof(dp_ctrl));
                }
            }
        }
    }

    if (mGraphicPlaneVisible) {
        struct drm_psb_disp_ctrl dp_ctrl;
        memset(&dp_ctrl, 0, sizeof(dp_ctrl));
        dp_ctrl.cmd = DRM_PSB_DISP_PLANEB_ENABLE;
        drmCommandWriteRead(mDrm->getDrmFd(), DRM_PSB_HDMI_FB_CMD, &dp_ctrl, sizeof(dp_ctrl));
    }
}

bool IntelHDMIDisplayDevice::prepare(hwc_display_contents_1_t *list)
{
    ALOGD_IF(ALLOW_HWC_PRINT, "%s", __func__);

    if (!initCheck()) {
        ALOGE("%s: failed to initialize HWComposer\n", __func__);
        return false;
    }

    if (mIsBlank) {
        //ALOGW("%s: HWC is blank, bypass", __func__);
        return false;
    }

    int index = checkVideoLayerHint(list, GRALLOC_HAL_HINT_TIME_SEEKING, false);
    bool findHint = (index >= 0);
    bool forceCheckingList = (findHint != mVideoSeekingActive);
    mVideoSeekingActive = findHint;

    if (!list || (list->flags & HWC_GEOMETRY_CHANGED) || forceCheckingList) {
        onGeometryChanged(list);

        if (findHint) {
            hwc_layer_1_t *layer = &list->hwLayers[index];
            if (layer != NULL)
                layer->compositionType = HWC_FRAMEBUFFER;
        }
    }

    // handle hotplug event here
    if (mHotplugEvent) {
        ALOGD_IF(ALLOW_HWC_PRINT, "%s: reset hotplug event flag\n", __func__);
        mHotplugEvent = false;
    }

    return true;
}

bool IntelHDMIDisplayDevice::commit(hwc_display_contents_1_t *list,
                                    buffer_handle_t *bh,
                                    int* acquireFenceFd,
                                    int** releaseFenceFd,
                                    int &numBuffers)
{
    ALOGD_IF(ALLOW_HWC_PRINT, "%s\n", __func__);

    if (!initCheck()) {
        ALOGE("%s: failed to initialize HWComposer\n", __func__);
        return false;
    }

    if (mIsBlank) {
        //ALOGW("%s: HWC is blank, bypass", __func__);
        return false;
    }

    // if hotplug was happened & didn't be handled skip the flip
    if (mHotplugEvent) {
        ALOGW("%s: hotplug event is true\n", __func__);
        return true;
    }

    int output = 1;
    drmModeConnection connection = mDrm->getOutputConnection(output);
    if (connection != DRM_MODE_CONNECTED) {
        ALOGW("%s: HDMI does not connected\n", __func__);
        return false;
    }

    if (!mGraphicPlaneVisible) {
        ALOGV("%s: Skip FRAMEBUFFER_TARGET flip\n", __func__);
        return false;
    }

    void *context = mPlaneManager->getPlaneContexts();
    if (!context) {
        ALOGW("%s: invalid plane contexts\n", __func__);
        return false;
    }

    if (list &&
        list->numHwLayers>0 &&
        list->hwLayers[list->numHwLayers-1].handle &&
        list->hwLayers[list->numHwLayers-1].compositionType == HWC_FRAMEBUFFER_TARGET) {

        hwc_layer_1_t *target_layer = &list->hwLayers[list->numHwLayers-1];
        buffer_handle_t *bufferHandles = bh;

        if (!mGraphicPlaneVisible) {
            ALOGD_IF(ALLOW_HWC_PRINT, "%s: Skip FRAMEBUFFER_TARGET \n", __func__);
            return false;
        }

        if (!mGraphicPlaneVisible || !flipFramebufferContexts(context, target_layer))
            ALOGV("%s: skip to flip HDMI fb context !\n", __func__);
        else {
            acquireFenceFd[numBuffers] = target_layer->acquireFenceFd;
            releaseFenceFd[numBuffers] = &target_layer->releaseFenceFd;
            bufferHandles[numBuffers++] = target_layer->handle;
        }
    } else if (list)
        ALOGV("%s: layernum: %d, no found of framebuffer_target!\n",
                                       __func__, list->numHwLayers);
    else
        ALOGW("%s: Invalid list, no found of framebuffer_target!\n",
                                       __func__);

    return true;
}

bool IntelHDMIDisplayDevice::updateLayersData(hwc_display_contents_1_t *list)
{
    return true;
}

bool IntelHDMIDisplayDevice::dump(char *buff,
                           int buff_len, int *cur_len)
{
    IntelDisplayPlane *plane = NULL;
    bool ret = true;
    int i;

    mDumpBuf = buff;
    mDumpBuflen = buff_len;
    mDumpLen = (int)(*cur_len);

    if (mLayerList) {
       dumpPrintf("------------ HDMI Totally %d layers -------------\n",
                                     mLayerList->getLayersCount());
       for (i = 0; i < mLayerList->getLayersCount(); i++) {
           plane = mLayerList->getPlane(i);

           if (plane) {
               int planeType = plane->getPlaneType();
               dumpPrintf("   # layer %d attached to %s plane \n", i,
                            (planeType == 3) ? "overlay" : "sprite");
           } else
               dumpPrintf("   # layer %d goes through eglswapbuffer\n ", i);
       }
       dumpPrintf("-------------HDMI runtime parameters -------------\n");
       dumpPrintf("  + mHotplugEvent: %d \n", mHotplugEvent);

    }

    *cur_len = mDumpLen;
    return ret;
}

bool IntelHDMIDisplayDevice::getDisplayConfig(uint32_t* configs,
                                        size_t* numConfigs)
{
    if (!numConfigs || !numConfigs[0])
        return false;

    if (mDrm->getOutputConnection(mDisplayIndex) != DRM_MODE_CONNECTED)
        return false;

    *numConfigs = 1;
    configs[0] = 0;

    return true;

}

bool IntelHDMIDisplayDevice::getDisplayAttributes(uint32_t config,
            const uint32_t* attributes, int32_t* values)
{
    if (config != 0)
        return false;

    if (!attributes || !values)
        return false;

    if (mDrm->getOutputConnection(mDisplayIndex) != DRM_MODE_CONNECTED)
        return false;

    drmModeModeInfoPtr mode = mDrm->getOutputMode(mDisplayIndex);

    while (*attributes != HWC_DISPLAY_NO_ATTRIBUTE) {
        switch (*attributes) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            *values = 1e9 / mode->vrefresh;
            break;
        case HWC_DISPLAY_WIDTH:
            *values = mode->hdisplay;
            break;
        case HWC_DISPLAY_HEIGHT:
            *values = mode->vdisplay;
            break;
        case HWC_DISPLAY_DPI_X:
            *values = 0;
            break;
        case HWC_DISPLAY_DPI_Y:
            *values = 0;
            break;
        default:
            break;
        }
        attributes ++;
        values ++;
    }

    return true;
}

void IntelHDMIDisplayDevice::onHotplugEvent(bool hpd)
{
    IntelDisplayDevice::onHotplugEvent(hpd);

    if (mDrm->getDisplayMode() == OVERLAY_MIPI0) {
        mLayerList->invalidatePlanes();
        mLayerList->updateLayerList(NULL);
    }
}
