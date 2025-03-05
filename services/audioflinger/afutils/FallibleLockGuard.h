/*
 *
 * Copyright 2024, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <android-base/thread_annotations.h>
#include <audio_utils/mutex.h>
#include <utils/Mutex.h>

#include <functional>

namespace android::afutils {

// Lock guard with a acquire timeout, which for the purpose of thread safety annotations acts as if
// it has the capability (i.e. the thread annotations *lie*). Used for dump utilities, where if we
// are deadlocked, we access without the lock since we are about to abort due to watchdog anyway.
// If the lock was truly successfully acquired, unlock on dtor. Like all guards (if successful),
// this guard is solely responsible for locking on ctor/unlocking on dtor, and the mutex reference
// must be valid for the lifetime of the object
class [[nodiscard]] SCOPED_CAPABILITY FallibleLockGuard {
  public:
    static constexpr int kDefaultTimeout = 1'000'000'000;

    explicit FallibleLockGuard(Mutex& mutex, int64_t timeoutNs = kDefaultTimeout) ACQUIRE(mutex) {
        if (mutex.timedLock(timeoutNs) == NO_ERROR) {
            mUnlockFunc = [&mutex]() NO_THREAD_SAFETY_ANALYSIS { mutex.unlock(); };
        }
    }

    explicit FallibleLockGuard(audio_utils::mutex& mutex, int64_t timeoutNs = kDefaultTimeout)
            ACQUIRE(mutex) {
        if (mutex.try_lock(timeoutNs)) {
            mUnlockFunc = [&mutex]() NO_THREAD_SAFETY_ANALYSIS { mutex.unlock(); };
        }
    }

    FallibleLockGuard(const FallibleLockGuard& other) = delete;

    FallibleLockGuard(FallibleLockGuard&& other) {
        mUnlockFunc.swap(other.mUnlockFunc);
    }

    FallibleLockGuard& operator=(const FallibleLockGuard& other) = delete;

    // Return if the underlying lock was successfully locked
    explicit operator bool() const { return static_cast<bool>(mUnlockFunc); }

    ~FallibleLockGuard() RELEASE() {
        if (mUnlockFunc) mUnlockFunc();
    }

  private:
    std::function<void()> mUnlockFunc;
};
}  // android::afutils
