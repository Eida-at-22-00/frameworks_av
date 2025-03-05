/*
 * Copyright (C) 2024 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "C2BqPoolInvalidator"
#include <utils/Log.h>
#include <utils/SystemClock.h>

#include <C2BqBufferPriv.h>
#include <C2BqPoolInvalidator.h>

namespace android {

namespace {
    static constexpr int64_t kBqPoolInvalidateDelayMs = 1000;
} // anonymous namespace

C2BqPoolInvalidateItem::C2BqPoolInvalidateItem(
        std::list<std::shared_ptr<C2BufferQueueBlockPool>> &&pools) : mPools(std::move(pools)) {
    if (!mPools.empty()) {
        mNeedsInvalidate = true;
    } else {
        mNeedsInvalidate = false;
    }
}

void C2BqPoolInvalidateItem::invalidate() {
    std::list<std::shared_ptr<C2BufferQueueBlockPool>> pools;
    {
        std::unique_lock<std::mutex> l(mLock);
        if (!mNeedsInvalidate) {
            return;
        }
        pools = std::move(mPools);
        mNeedsInvalidate = false;
    }
    for(auto it = pools.begin(); it != pools.end(); ++it) {
        (*it)->invalidate();
    }
}

void C2BqPoolInvalidateItem::skip() {
    std::unique_lock<std::mutex> l(mLock);
    mNeedsInvalidate = false;
    mPools.clear();
}

bool C2BqPoolInvalidateItem::needsInvalidate() {
    std::unique_lock<std::mutex> l(mLock);
    return mNeedsInvalidate;
}

C2BqPoolInvalidator &C2BqPoolInvalidator::getInstance() {
    static android::base::NoDestructor<C2BqPoolInvalidator> sInvalidator;
    return *sInvalidator;
}

C2BqPoolInvalidator::C2BqPoolInvalidator() : mDone(false) {
    mThread = std::thread(&C2BqPoolInvalidator::run, this);
}

C2BqPoolInvalidator::~C2BqPoolInvalidator() {
    {
        std::unique_lock<std::mutex> l(mMutex);
        mDone = true;
        mCv.notify_one();
    }
    if (mThread.joinable()) {
        mThread.join();
    }
}

void C2BqPoolInvalidator::queue(std::shared_ptr<C2BqPoolInvalidateItem> &item) {
    std::unique_lock<std::mutex> l(mMutex);
    std::pair<int64_t, std::shared_ptr<C2BqPoolInvalidateItem>> p =
            std::make_pair(::android::elapsedRealtime() + kBqPoolInvalidateDelayMs, item);
    mItems.push_back(p);
    mCv.notify_one();
}

void C2BqPoolInvalidator::run() {
    while(true) {
        int64_t nowMs = ::android::elapsedRealtime();
        std::unique_lock<std::mutex> l(mMutex);
        if (mDone) {
            break;
        }
        std::list<std::shared_ptr<C2BqPoolInvalidateItem>> items;
        while (!mItems.empty()) {
            if (mItems.front().first <= nowMs) {
                items.push_back(mItems.front().second);
                mItems.pop_front();
            } else {
                break;
            }
        }
        if (items.empty()) {
            if (mItems.empty()) {
                mCv.wait(l);
            } else {
                int64_t nextMs = mItems.front().first;
                if (nextMs > nowMs) {
                    mCv.wait_for(l, std::chrono::milliseconds(nextMs - nowMs));
                }
            }
        } else {
            l.unlock();
            int invalidated = 0;
            for (auto it = items.begin(); it != items.end(); ++it, ++invalidated) {
                (*it)->invalidate();
            }
            ALOGD("invalidated %d bqpool items", invalidated);
        }
    }
}

} // android
