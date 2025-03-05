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

#pragma once

#include <android-base/no_destructor.h>
#include <media/stagefright/foundation/ABase.h>

#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <thread>

class C2BufferQueueBlockPool;

namespace android {

/**
 * Container class in order to invalidate C2BufferQueueBlockPool(s) and their resources
 * when the client process is dead abruptly.
 */
class C2BqPoolInvalidateItem {
public:

    /**
     * invalidate contained C2BufferQueueBlockPool(s) and their resources
     */
    void invalidate();

    /**
     * skip invalidate(), if it is scheduled and not yet invalidated.
     */
    void skip();

    /**
     * returns whether invalidate() is reuqired or not.
     */
    bool needsInvalidate();

    C2BqPoolInvalidateItem(std::list<std::shared_ptr<C2BufferQueueBlockPool>> &&pools);

    ~C2BqPoolInvalidateItem() = default;
private:

    std::list<std::shared_ptr<C2BufferQueueBlockPool>>  mPools;
    bool mNeedsInvalidate;
    std::mutex mLock;

    DISALLOW_EVIL_CONSTRUCTORS(C2BqPoolInvalidateItem);
};

/**
 * Asynchronous C2BufferQueueBlockPool invalidator.
 *
 * this has C2BqPoolInvalidateItem inside. and call invalidate() from a separate
 * thread asynchronously.
 */
class C2BqPoolInvalidator {
public:
    /**
     * This gets the singleton instance of the class.
     */
    static C2BqPoolInvalidator &getInstance();

    /**
     * queue invalidation items. the item will be invalidated after certain
     * amount of delay from a separate thread.
     */
    void queue(std::shared_ptr<C2BqPoolInvalidateItem> &item);

    ~C2BqPoolInvalidator();
private:

    C2BqPoolInvalidator();

    void run();

    std::thread mThread;
    bool mDone;

    std::mutex mMutex;
    std::condition_variable mCv;

    std::deque<std::pair<int64_t, std::shared_ptr<C2BqPoolInvalidateItem>>> mItems;

    friend class ::android::base::NoDestructor<C2BqPoolInvalidator>;

    DISALLOW_EVIL_CONSTRUCTORS(C2BqPoolInvalidator);
};

}  // namespace android
