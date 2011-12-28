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

#ifndef __INTEL_HWCOMPOSER_LAYER_H__
#define __INTEL_HWCOMPOSER_LAYER_H__

#include <string.h>
#include <hardware/hwcomposer.h>
#include <IntelDisplayPlaneManager.h>

class IntelHWComposerLayer {
private:
    hwc_layer_t *mHWCLayer;
    IntelDisplayPlane *mPlane;
    int mFlags;
    int mTransform;
public:
    IntelHWComposerLayer();
    IntelHWComposerLayer(hwc_layer_t *layer,
                         IntelDisplayPlane *plane,
                         int flags);
    ~IntelHWComposerLayer();

friend class IntelHWComposerLayerList;
};

class IntelHWComposerLayerList {
private:
    IntelHWComposerLayer *mLayerList;
    IntelDisplayPlaneManager *mPlaneManager;
    int mNumLayers;
    int mNumAttachedPlanes;
    bool mInitialized;
public:
    IntelHWComposerLayerList(IntelDisplayPlaneManager *pm);
    ~IntelHWComposerLayerList();
    bool initCheck() { return mInitialized; }
    void updateLayerList(hwc_layer_list_t *layerList);
    bool invalidatePlanes();
    void attachPlane(int index, IntelDisplayPlane *plane, int flags);
    void detachPlane(int index, IntelDisplayPlane *plane);
    IntelDisplayPlane* getPlane(int index);
    void setFlags(int index, int flags);
    int getFlags(int index);
    void setTransform(int index, int transform);
    int getTransform(int index);
    int getLayersCount() const { return mNumLayers; }
    int getAttachedPlanesCount() const { return mNumAttachedPlanes; }
};

#endif /*__INTEL_HWCOMPOSER_LAYER_H__*/
