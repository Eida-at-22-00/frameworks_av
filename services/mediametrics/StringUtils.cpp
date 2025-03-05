/*
 * Copyright (C) 2020 The Android Open Source Project
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
#define LOG_TAG "mediametrics::stringutils"
#include <utils/Log.h>

#include "StringUtils.h"
#include "AudioTypes.h"
#include <audio_utils/StringUtils.h>
#include <charconv>

namespace android::mediametrics::stringutils {

size_t replace(std::string &str, const char *targetChars, const char replaceChar)
{
    size_t replaced = 0;
    for (char &c : str) {
        if (strchr(targetChars, c) != nullptr) {
            c = replaceChar;
            ++replaced;
        }
    }
    return replaced;
}

template <types::AudioEnumCategory CATEGORY>
std::pair<std::string /* external statsd */, std::string /* internal */>
parseDevicePairs(const std::string& devicePairs) {
    std::pair<std::string, std::string> result{};
    const auto devaddrvec = audio_utils::stringutils::getDeviceAddressPairs(devicePairs);
    for (const auto& [device, addr] : devaddrvec) { // addr ignored for now.
        if (!result.second.empty()) {
            result.second.append("|"); // delimit devices with '|'.
            result.first.append("|");
        }
        result.second.append(device);
        result.first.append(types::lookup<CATEGORY, std::string>(device));
    }
    return result;
}

std::pair<std::string /* external statsd */, std::string /* internal */>
parseOutputDevicePairs(const std::string& devicePairs) {
    return parseDevicePairs<types::OUTPUT_DEVICE>(devicePairs);
}

std::pair<std::string /* external statsd */, std::string /* internal */>
parseInputDevicePairs(const std::string& devicePairs) {
    return parseDevicePairs<types::INPUT_DEVICE>(devicePairs);
}

} // namespace android::mediametrics::stringutils
