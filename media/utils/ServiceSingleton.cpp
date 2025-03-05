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
#define LOG_TAG "ServiceSingleton"

#include <mediautils/ServiceSingleton.h>

namespace android::mediautils {

namespace details {

// To prevent multiple instances in different linkages,
// we anchor the singleton in a .cpp instead of inlining in the header.

template<typename T>
requires (std::is_same_v<T, const char*> || std::is_same_v<T, String16>)
std::shared_ptr<ServiceHandler> ServiceHandler::getInstance(const T& name) {
    using Key = std::conditional_t<std::is_same_v<T, String16>, String16, std::string>;
    [[clang::no_destroy]] static constinit std::mutex mutex;
    [[clang::no_destroy]] static constinit std::shared_ptr<
            std::map<Key, std::shared_ptr<ServiceHandler>>> map GUARDED_BY(mutex);
    static constinit bool init GUARDED_BY(mutex) = false;

    std::lock_guard l(mutex);
    if (!init) {
        map = std::make_shared<std::map<Key, std::shared_ptr<ServiceHandler>>>();
        init = true;
    }

    auto& handler = (*map)[name];
    if (!handler) {
        handler = std::make_shared<ServiceHandler>();
        if constexpr (std::is_same_v<T, String16>) {
            handler->init_cpp();
        } else /* constexpr */ {
            handler->init_ndk();
        }
    }
    return handler;
}

// Explicit template function instantiation.
template
std::shared_ptr<ServiceHandler> ServiceHandler::getInstance<const char*>(const char* const& name);

template
std::shared_ptr<ServiceHandler> ServiceHandler::getInstance<String16>(const String16& name);

} // details

} // namespace android::mediautils

