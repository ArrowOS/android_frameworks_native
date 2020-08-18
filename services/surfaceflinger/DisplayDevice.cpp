/*
 * Copyright (C) 2007 The Android Open Source Project
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

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

// #define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "DisplayDevice"

#include <android-base/stringprintf.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/Display.h>
#include <compositionengine/DisplayColorProfile.h>
#include <compositionengine/DisplayColorProfileCreationArgs.h>
#include <compositionengine/DisplayCreationArgs.h>
#include <compositionengine/DisplaySurface.h>
#include <compositionengine/ProjectionSpace.h>
#include <compositionengine/RenderSurface.h>
#include <compositionengine/RenderSurfaceCreationArgs.h>
#include <compositionengine/impl/OutputCompositionState.h>
#include <configstore/Utils.h>
#include <log/log.h>
#include <system/window.h>
#include <ui/GraphicTypes.h>

#include "DisplayDevice.h"
#include "Layer.h"
#include "SurfaceFlinger.h"

namespace android {

namespace hal = hardware::graphics::composer::hal;

using android::base::StringAppendF;

ui::Transform::RotationFlags DisplayDevice::sPrimaryDisplayRotationFlags = ui::Transform::ROT_0;

DisplayDeviceCreationArgs::DisplayDeviceCreationArgs(
        const sp<SurfaceFlinger>& flinger, const wp<IBinder>& displayToken,
        std::shared_ptr<compositionengine::Display> compositionDisplay)
      : flinger(flinger), displayToken(displayToken), compositionDisplay(compositionDisplay) {}

DisplayDevice::DisplayDevice(DisplayDeviceCreationArgs& args)
      : mFlinger(args.flinger),
        mDisplayToken(args.displayToken),
        mSequenceId(args.sequenceId),
        mConnectionType(args.connectionType),
        mCompositionDisplay{args.compositionDisplay},
        mPhysicalOrientation(args.physicalOrientation),
        mIsPrimary(args.isPrimary) {
    mCompositionDisplay->editState().isSecure = args.isSecure;
    mCompositionDisplay->createRenderSurface(
            compositionengine::RenderSurfaceCreationArgs{ANativeWindow_getWidth(
                                                                 args.nativeWindow.get()),
                                                         ANativeWindow_getHeight(
                                                                 args.nativeWindow.get()),
                                                         args.nativeWindow, args.displaySurface});

    if (!mFlinger->mDisableClientCompositionCache &&
        SurfaceFlinger::maxFrameBufferAcquiredBuffers > 0) {
        mCompositionDisplay->createClientCompositionCache(
                static_cast<uint32_t>(SurfaceFlinger::maxFrameBufferAcquiredBuffers));
    }

    mCompositionDisplay->createDisplayColorProfile(
            compositionengine::DisplayColorProfileCreationArgs{args.hasWideColorGamut,
                                                               std::move(args.hdrCapabilities),
                                                               args.supportedPerFrameMetadata,
                                                               args.hwcColorModes});

    if (!mCompositionDisplay->isValid()) {
        ALOGE("Composition Display did not validate!");
    }

    mCompositionDisplay->getRenderSurface()->initialize();

    setPowerMode(args.initialPowerMode);

    // initialize the display orientation transform.
    setProjection(ui::ROTATION_0, Rect::INVALID_RECT, Rect::INVALID_RECT);
}

DisplayDevice::~DisplayDevice() = default;

void DisplayDevice::disconnect() {
    mCompositionDisplay->disconnect();
}

int DisplayDevice::getWidth() const {
    return mCompositionDisplay->getState().displaySpace.bounds.getWidth();
}

int DisplayDevice::getHeight() const {
    return mCompositionDisplay->getState().displaySpace.bounds.getHeight();
}

void DisplayDevice::setDisplayName(const std::string& displayName) {
    if (!displayName.empty()) {
        // never override the name with an empty name
        mDisplayName = displayName;
        mCompositionDisplay->setName(displayName);
    }
}

void DisplayDevice::setDeviceProductInfo(std::optional<DeviceProductInfo> info) {
    mDeviceProductInfo = std::move(info);
}

uint32_t DisplayDevice::getPageFlipCount() const {
    return mCompositionDisplay->getRenderSurface()->getPageFlipCount();
}

// ----------------------------------------------------------------------------
void DisplayDevice::setPowerMode(hal::PowerMode mode) {
    mPowerMode = mode;
    getCompositionDisplay()->setCompositionEnabled(mPowerMode != hal::PowerMode::OFF);
}

hal::PowerMode DisplayDevice::getPowerMode() const {
    return mPowerMode;
}

bool DisplayDevice::isPoweredOn() const {
    return mPowerMode != hal::PowerMode::OFF;
}

void DisplayDevice::setActiveConfig(HwcConfigIndexType mode) {
    mActiveConfig = mode;
}

HwcConfigIndexType DisplayDevice::getActiveConfig() const {
    return mActiveConfig;
}

ui::Dataspace DisplayDevice::getCompositionDataSpace() const {
    return mCompositionDisplay->getState().dataspace;
}

void DisplayDevice::setLayerStack(ui::LayerStack stack) {
    mCompositionDisplay->setLayerStackFilter(stack, isPrimary());
}

void DisplayDevice::setDisplaySize(int width, int height) {
    mCompositionDisplay->setDisplaySpaceSize(ui::Size(width, height));
}

void DisplayDevice::setProjection(ui::Rotation orientation, Rect layerStackSpaceRect,
                                  Rect orientedDisplaySpaceRect) {
    mOrientation = orientation;

    const Rect& displayBounds = getCompositionDisplay()->getState().displaySpace.bounds;
    const int displayWidth = displayBounds.width();
    const int displayHeight = displayBounds.height();

    ui::Transform rotation;
    if (const auto flags = ui::Transform::toRotationFlags(orientation);
        flags != ui::Transform::ROT_INVALID) {
        rotation.set(flags, displayWidth, displayHeight);
    }

    if (!orientedDisplaySpaceRect.isValid()) {
        // the destination frame can be invalid if it has never been set,
        // in that case we assume the whole display frame.
        orientedDisplaySpaceRect = Rect(displayWidth, displayHeight);
    }

    if (layerStackSpaceRect.isEmpty()) {
        // layerStackSpaceRect can be invalid if it has never been set, in that case
        // we assume the whole display size.
        // It's also invalid to have an empty layerStackSpaceRect, so we handle that
        // case in the same way.
        layerStackSpaceRect = Rect(displayWidth, displayHeight);
        if (rotation.getOrientation() & ui::Transform::ROT_90) {
            std::swap(layerStackSpaceRect.right, layerStackSpaceRect.bottom);
        }
    }

    ui::Transform logicalTranslation, physicalTranslation, scale;
    const float sourceWidth = layerStackSpaceRect.width();
    const float sourceHeight = layerStackSpaceRect.height();
    const float destWidth = orientedDisplaySpaceRect.width();
    const float destHeight = orientedDisplaySpaceRect.height();
    if (sourceWidth != destWidth || sourceHeight != destHeight) {
        const float scaleX = destWidth / sourceWidth;
        const float scaleY = destHeight / sourceHeight;
        scale.set(scaleX, 0, 0, scaleY);
    }

    const float sourceX = layerStackSpaceRect.left;
    const float sourceY = layerStackSpaceRect.top;
    const float destX = orientedDisplaySpaceRect.left;
    const float destY = orientedDisplaySpaceRect.top;
    logicalTranslation.set(-sourceX, -sourceY);
    physicalTranslation.set(destX, destY);

    // need to take care of primary display rotation for globalTransform
    // for case if the panel is not installed aligned with device orientation
    if (isPrimary()) {
        if (const auto flags = ui::Transform::toRotationFlags(orientation + mPhysicalOrientation);
            flags != ui::Transform::ROT_INVALID) {
            rotation.set(flags, displayWidth, displayHeight);
        }
    }

    // The layerStackSpaceRect and orientedDisplaySpaceRect are both in the logical orientation.
    // Apply the logical translation, scale to physical size, apply the
    // physical translation and finally rotate to the physical orientation.
    ui::Transform globalTransform = rotation * physicalTranslation * scale * logicalTranslation;

    const uint8_t type = globalTransform.getType();
    const bool needsFiltering =
            (!globalTransform.preserveRects() || (type >= ui::Transform::SCALE));

    Rect displaySpaceRect = globalTransform.transform(layerStackSpaceRect);
    if (displaySpaceRect.isEmpty()) {
        displaySpaceRect = displayBounds;
    }
    // Make sure the displaySpaceRect is contained in the display bounds
    displaySpaceRect.intersect(displayBounds, &displaySpaceRect);

    uint32_t transformOrientation;

    if (isPrimary()) {
        sPrimaryDisplayRotationFlags = ui::Transform::toRotationFlags(orientation);
        transformOrientation = ui::Transform::toRotationFlags(orientation + mPhysicalOrientation);
    } else {
        transformOrientation = ui::Transform::toRotationFlags(orientation);
    }

    getCompositionDisplay()->setProjection(globalTransform, transformOrientation,
                                           orientedDisplaySpaceRect, layerStackSpaceRect,
                                           displaySpaceRect, needsFiltering);
}

ui::Transform::RotationFlags DisplayDevice::getPrimaryDisplayRotationFlags() {
    return sPrimaryDisplayRotationFlags;
}

std::string DisplayDevice::getDebugName() const {
    std::string displayId;
    if (const auto id = getId()) {
        displayId = to_string(*id) + ", ";
    }

    const char* type = "virtual";
    if (mConnectionType) {
        type = *mConnectionType == DisplayConnectionType::Internal ? "internal" : "external";
    }

    return base::StringPrintf("DisplayDevice{%s%s%s, \"%s\"}", displayId.c_str(), type,
                              isPrimary() ? ", primary" : "", mDisplayName.c_str());
}

void DisplayDevice::dump(std::string& result) const {
    StringAppendF(&result, "+ %s\n", getDebugName().c_str());

    result.append("   ");
    StringAppendF(&result, "powerMode=%s (%d), ", to_string(mPowerMode).c_str(),
                  static_cast<int32_t>(mPowerMode));
    StringAppendF(&result, "activeConfig=%d, ", mActiveConfig.value());
    StringAppendF(&result, "deviceProductInfo=");
    if (mDeviceProductInfo) {
        mDeviceProductInfo->dump(result);
    } else {
        result.append("{}");
    }
    getCompositionDisplay()->dump(result);
}

bool DisplayDevice::hasRenderIntent(ui::RenderIntent intent) const {
    return mCompositionDisplay->getDisplayColorProfile()->hasRenderIntent(intent);
}

// ----------------------------------------------------------------------------

const std::optional<DisplayId>& DisplayDevice::getId() const {
    return mCompositionDisplay->getId();
}

bool DisplayDevice::isSecure() const {
    return mCompositionDisplay->isSecure();
}

const Rect& DisplayDevice::getBounds() const {
    return mCompositionDisplay->getState().displaySpace.bounds;
}

const Region& DisplayDevice::getUndefinedRegion() const {
    return mCompositionDisplay->getState().undefinedRegion;
}

bool DisplayDevice::needsFiltering() const {
    return mCompositionDisplay->getState().needsFiltering;
}

ui::LayerStack DisplayDevice::getLayerStack() const {
    return mCompositionDisplay->getState().layerStackId;
}

const ui::Transform& DisplayDevice::getTransform() const {
    return mCompositionDisplay->getState().transform;
}

const Rect& DisplayDevice::getLayerStackSpaceRect() const {
    return mCompositionDisplay->getState().layerStackSpace.content;
}

const Rect& DisplayDevice::getOrientedDisplaySpaceRect() const {
    return mCompositionDisplay->getState().orientedDisplaySpace.content;
}

bool DisplayDevice::hasWideColorGamut() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasWideColorGamut();
}

bool DisplayDevice::hasHDR10PlusSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHDR10PlusSupport();
}

bool DisplayDevice::hasHDR10Support() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHDR10Support();
}

bool DisplayDevice::hasHLGSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasHLGSupport();
}

bool DisplayDevice::hasDolbyVisionSupport() const {
    return mCompositionDisplay->getDisplayColorProfile()->hasDolbyVisionSupport();
}

int DisplayDevice::getSupportedPerFrameMetadata() const {
    return mCompositionDisplay->getDisplayColorProfile()->getSupportedPerFrameMetadata();
}

const HdrCapabilities& DisplayDevice::getHdrCapabilities() const {
    return mCompositionDisplay->getDisplayColorProfile()->getHdrCapabilities();
}

std::atomic<int32_t> DisplayDeviceState::sNextSequenceId(1);

}  // namespace android

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion"