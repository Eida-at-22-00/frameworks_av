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

#include <aidl/android/hardware/media/c2/BnInputSurface.h>

#include <codec2/aidl/Configurable.h>
#include <util/C2InterfaceHelper.h>

#include <C2.h>

#include <memory>

namespace aidl::android::hardware::media::c2::utils {

struct InputSurface : public BnInputSurface {
    InputSurface();
    c2_status_t status() const;

    // Methods from IInputSurface follow.
    ::ndk::ScopedAStatus getSurface(
            ::aidl::android::view::Surface* surface) override;
    ::ndk::ScopedAStatus getConfigurable(
            std::shared_ptr<IConfigurable>* configurable) override;
    ::ndk::ScopedAStatus connect(
            const std::shared_ptr<IInputSink>& sink,
            std::shared_ptr<IInputSurfaceConnection>* connection) override;

protected:
    class Interface;
    class ConfigurableIntf;

    c2_status_t mInit;
    std::shared_ptr<Interface> mIntf;
    std::shared_ptr<CachedConfigurable> mConfigurable;

    virtual ~InputSurface() override;


    ::ndk::ScopedAIBinder_DeathRecipient mDeathRecipient;
    static void OnBinderDied(void *cookie);
    static void OnBinderUnlinked(void *cookie);
    struct DeathContext;
    DeathContext *mDeathContext;
};

}  // namespace aidl::android::hardware::media::c2::utils
