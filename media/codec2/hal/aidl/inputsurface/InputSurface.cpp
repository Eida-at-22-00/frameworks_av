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

//#define LOG_NDEBUG 0
#define LOG_TAG "Codec2-InputSurface"
#include <android-base/logging.h>

#include <codec2/aidl/inputsurface/InputSurface.h>

namespace aidl::android::hardware::media::c2::utils {

// Derived class of C2InterfaceHelper
class InputSurface::Interface : public C2InterfaceHelper {
public:
    explicit Interface(
            const std::shared_ptr<C2ReflectorHelper> &helper)
        : C2InterfaceHelper(helper) {

        setDerivedInstance(this);

    }

private:
};

class InputSurface::ConfigurableIntf : public ConfigurableC2Intf {
public:
};

struct InputSurface::DeathContext {
    // TODO;
};

void InputSurface::OnBinderDied(void *cookie) {
    (void) cookie;
}

void InputSurface::OnBinderUnlinked(void *cookie) {
    (void) cookie;
}

InputSurface::InputSurface() : mDeathContext(nullptr) {
    mInit = C2_OK;
}

InputSurface::~InputSurface() {
}

::ndk::ScopedAStatus InputSurface::getSurface(::aidl::android::view::Surface* surface) {
    (void) surface;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus InputSurface::getConfigurable(
        std::shared_ptr<IConfigurable>* configurable) {
    *configurable = mConfigurable;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus InputSurface::connect(
        const std::shared_ptr<IInputSink>& sink,
        std::shared_ptr<IInputSurfaceConnection>* connection) {
    (void) sink;
    (void) connection;
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::media::c2::utils
