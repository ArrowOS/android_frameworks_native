/*
 * Copyright 2019 The Android Open Source Project
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

#pragma once

#include <gmock/gmock.h>
#include <gui/ISurfaceComposer.h>

#include "Scheduler/DispSync.h"
#include "Scheduler/EventThread.h"
#include "Scheduler/LayerHistory.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/VSyncTracker.h"
#include "mock/MockDispSync.h"

namespace android {

class TestableScheduler : public Scheduler {
public:
    TestableScheduler(const scheduler::RefreshRateConfigs& configs, ISchedulerCallback& callback,
                      bool useContentDetectionV2)
          : TestableScheduler(std::make_unique<mock::DispSync>(), configs, callback,
                              useContentDetectionV2) {}

    TestableScheduler(std::unique_ptr<DispSync> primaryDispSync,
                      const scheduler::RefreshRateConfigs& configs, ISchedulerCallback& callback,
                      bool useContentDetectionV2)
          : Scheduler({std::move(primaryDispSync), nullptr, nullptr}, configs, callback,
                      createLayerHistory(configs, useContentDetectionV2),
                      {.supportKernelTimer = false,
                       .useContentDetection = true,
                       .useContentDetectionV2 = useContentDetectionV2,
                       .useVsyncPredictor = false}) {}

    // Used to inject mock event thread.
    ConnectionHandle createConnection(std::unique_ptr<EventThread> eventThread) {
        return Scheduler::createConnection(std::move(eventThread));
    }

    /* ------------------------------------------------------------------------
     * Read-write access to private data to set up preconditions and assert
     * post-conditions.
     */
    auto& mutablePrimaryHWVsyncEnabled() { return mPrimaryHWVsyncEnabled; }
    auto& mutableHWVsyncAvailable() { return mHWVsyncAvailable; }

    bool hasLayerHistory() const { return static_cast<bool>(mLayerHistory); }

    auto* mutableLayerHistory() {
        LOG_ALWAYS_FATAL_IF(mOptions.useContentDetectionV2);
        return static_cast<scheduler::impl::LayerHistory*>(mLayerHistory.get());
    }

    auto* mutableLayerHistoryV2() {
        LOG_ALWAYS_FATAL_IF(!mOptions.useContentDetectionV2);
        return static_cast<scheduler::impl::LayerHistoryV2*>(mLayerHistory.get());
    }

    size_t layerHistorySize() NO_THREAD_SAFETY_ANALYSIS {
        if (!mLayerHistory) return 0;
        return mOptions.useContentDetectionV2 ? mutableLayerHistoryV2()->mLayerInfos.size()
                                              : mutableLayerHistory()->mLayerInfos.size();
    }

    void replaceTouchTimer(int64_t millis) {
        if (mTouchTimer) {
            mTouchTimer.reset();
        }
        mTouchTimer.emplace(
                std::chrono::milliseconds(millis),
                [this] { touchTimerCallback(TimerState::Reset); },
                [this] { touchTimerCallback(TimerState::Expired); });
        mTouchTimer->start();
    }

    bool isTouchActive() {
        std::lock_guard<std::mutex> lock(mFeatureStateLock);
        return mFeatures.touch == Scheduler::TouchState::Active;
    }

    ~TestableScheduler() {
        // All these pointer and container clears help ensure that GMock does
        // not report a leaked object, since the Scheduler instance may
        // still be referenced by something despite our best efforts to destroy
        // it after each test is done.
        mVsyncSchedule.sync.reset();
        mConnections.clear();
    }
};

} // namespace android