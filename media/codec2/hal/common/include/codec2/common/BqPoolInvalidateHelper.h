/*
 * Copyright 2024 The Android Open Source Project
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

#include <C2BqBufferPriv.h>
#include <C2PlatformSupport.h>

namespace android {

// filter fn from component's blockpool container to bqpool conatainer
static inline bool BqPoolFilterFn(
        std::pair<const uint64_t, std::shared_ptr<C2BlockPool>> pool) {
    return (pool.second->getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE);
}

// convert fn from component's blockpool container to bqpool container
static inline std::shared_ptr<C2BufferQueueBlockPool> BqPoolConvertFn(
        std::pair<const uint64_t, std::shared_ptr<C2BlockPool>> pool) {
    return std::static_pointer_cast<C2BufferQueueBlockPool>(pool.second);
}

// This is similar to std::transform excpet there is \pred functor parameter.
// The elements with \pred function value \true only will be transformed and
// added to the dest container. (For portability std::ranges are not used.)
template <class InputIt, class OutputIt, class Pred, class Fct>
void transform_if(InputIt first, InputIt last, OutputIt dest, Pred pred, Fct transform)
{
   while (first != last) {
      if (pred(*first)) {
         *dest++ = transform(*first);
      }
      ++first;
   }
}

}  // namespace android
