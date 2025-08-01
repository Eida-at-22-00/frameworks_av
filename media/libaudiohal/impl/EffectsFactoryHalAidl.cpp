/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#define LOG_TAG "EffectsFactoryHalAidl"
//#define LOG_NDEBUG 0

#include <error/expected_utils.h>
#include <aidl/android/media/audio/common/AudioStreamType.h>
#include <android/binder_manager.h>
#include <com_android_media_audio.h>
#include <media/AidlConversionCppNdk.h>
#include <media/AidlConversionEffect.h>
#include <system/audio.h>
#include <system/audio_aidl_utils.h>
#include <system/audio_effects/effect_uuid.h>
#include <utils/Log.h>

#include "AidlUtils.h"
#include "EffectBufferHalAidl.h"
#include "EffectHalAidl.h"
#include "EffectProxy.h"
#include "EffectsFactoryHalAidl.h"

using ::aidl::android::legacy2aidl_audio_uuid_t_AudioUuid;
using ::aidl::android::aidl_utils::statusTFromBinderStatus;
using ::aidl::android::hardware::audio::effect::Descriptor;
using ::aidl::android::hardware::audio::effect::IFactory;
using ::aidl::android::hardware::audio::effect::Processing;
using ::aidl::android::media::audio::common::AudioDevice;
using ::aidl::android::media::audio::common::AudioDeviceAddress;
using ::aidl::android::media::audio::common::AudioSource;
using ::aidl::android::media::audio::common::AudioStreamType;
using ::aidl::android::media::audio::common::AudioUuid;
using ::android::base::unexpected;
using ::android::detail::AudioHalVersionInfo;

namespace android {
namespace effect {

EffectsFactoryHalAidl::EffectsFactoryHalAidl(std::shared_ptr<IFactory> effectsFactory)
    : mFactory(effectsFactory),
      mHalVersion(AudioHalVersionInfo(
              AudioHalVersionInfo::Type::AIDL,
              [this]() {
                  int32_t majorVersion = 0;
                  return (mFactory && mFactory->getInterfaceVersion(&majorVersion).isOk())
                                 ? majorVersion
                                 : 0;
              }())),
      mHalDescList([this]() {
          std::vector<Descriptor> list;
          if (mFactory) {
              mFactory->queryEffects(std::nullopt, std::nullopt, std::nullopt, &list).isOk();
              filterHalDescriptors(list);
          }
          return list;
      }()),
      mProxyUuidDescriptorMap([this]() {
          std::map<AudioUuid, std::vector<Descriptor>> proxyUuidMap;
          for (const auto& desc : mHalDescList) {
              if (desc.common.id.proxy.has_value()) {
                  const auto& uuid = desc.common.id.proxy.value();
                  if (proxyUuidMap.count(uuid) == 0) {
                      proxyUuidMap.insert({uuid, {desc}});
                  } else {
                      proxyUuidMap[uuid].emplace_back(desc);
                  }
              }
          }
          return proxyUuidMap;
      }()),
      mProxyDescList([this]() {
          std::vector<Descriptor> list;
          for (const auto& proxy : mProxyUuidDescriptorMap) {
              if (Descriptor desc;
                  EffectProxy::buildDescriptor(proxy.first /* uuid */,
                                               proxy.second /* sub-effect descriptor list */,
                                               &desc /* proxy descriptor */)
                          .isOk()) {
                  list.emplace_back(std::move(desc));
              }
          }
          return list;
      }()),
      mNonProxyDescList([this]() {
          std::vector<Descriptor> list;
          std::copy_if(mHalDescList.begin(), mHalDescList.end(), std::back_inserter(list),
                       [](const Descriptor& desc) { return !desc.common.id.proxy.has_value(); });
          return list;
      }()),
      mEffectCount(mNonProxyDescList.size() + mProxyDescList.size()),
      mAidlProcessings([this]() -> std::vector<Processing> {
          std::vector<Processing> processings;
          if (!mFactory || !mFactory->queryProcessing(std::nullopt, &processings).isOk()) {
              ALOGE("%s queryProcessing failed", __func__);
          }
          return processings;
      }()) {
    ALOG_ASSERT(mFactory != nullptr, "Provided IEffectsFactory service is NULL");
    ALOGI("%s with %zu nonProxyEffects and %zu proxyEffects", __func__, mNonProxyDescList.size(),
          mProxyDescList.size());
}

status_t EffectsFactoryHalAidl::queryNumberEffects(uint32_t *pNumEffects) {
    if (pNumEffects == nullptr) {
        return BAD_VALUE;
    }

    *pNumEffects = mEffectCount;
    return OK;
}

status_t EffectsFactoryHalAidl::getDescriptor(uint32_t index, effect_descriptor_t* pDescriptor) {
    if (pDescriptor == nullptr) {
        return BAD_VALUE;
    }

    if (index >= mEffectCount) {
        ALOGE("%s index %d exceed max number %zu", __func__, index, mEffectCount);
        return INVALID_OPERATION;
    }

    if (index >= mNonProxyDescList.size()) {
        *pDescriptor =
                VALUE_OR_RETURN_STATUS(::aidl::android::aidl2legacy_Descriptor_effect_descriptor(
                        mProxyDescList.at(index - mNonProxyDescList.size())));
    } else {
        *pDescriptor =
                VALUE_OR_RETURN_STATUS(::aidl::android::aidl2legacy_Descriptor_effect_descriptor(
                        mNonProxyDescList.at(index)));
    }
    return OK;
}

status_t EffectsFactoryHalAidl::getDescriptor(const effect_uuid_t* halUuid,
                                              effect_descriptor_t* pDescriptor) {
    if (halUuid == nullptr) {
        return BAD_VALUE;
    }

    AudioUuid uuid =
            VALUE_OR_RETURN_STATUS(::aidl::android::legacy2aidl_audio_uuid_t_AudioUuid(*halUuid));
    return getHalDescriptorWithImplUuid(uuid, pDescriptor);
}

status_t EffectsFactoryHalAidl::getDescriptors(const effect_uuid_t* halType,
                                               std::vector<effect_descriptor_t>* descriptors) {
    if (halType == nullptr) {
        return BAD_VALUE;
    }

    AudioUuid type =
            VALUE_OR_RETURN_STATUS(::aidl::android::legacy2aidl_audio_uuid_t_AudioUuid(*halType));
    return getHalDescriptorWithTypeUuid(type, descriptors);
}

status_t EffectsFactoryHalAidl::createEffect(const effect_uuid_t* uuid, int32_t sessionId,
                                             int32_t ioId, int32_t deviceId __unused,
                                             sp<EffectHalInterface>* effect) {
    if (uuid == nullptr || effect == nullptr) {
        return BAD_VALUE;
    }
    ALOGV("%s session %d ioId %d", __func__, sessionId, ioId);

    AudioUuid aidlUuid =
            VALUE_OR_RETURN_STATUS(::aidl::android::legacy2aidl_audio_uuid_t_AudioUuid(*uuid));
    if (!com_android_media_audio_audio_eraser_effect() && isAudioEraser(aidlUuid)) {
        ALOGE("%s Audio eraser effect not supported yet", __func__);
        return BAD_VALUE;
    }

    std::shared_ptr<IEffect> aidlEffect;
    // Use EffectProxy interface instead of IFactory to create
    const bool isProxy = isProxyEffect(aidlUuid);
    if (isProxy) {
        aidlEffect = ndk::SharedRefBase::make<EffectProxy>(
                aidlUuid, mProxyUuidDescriptorMap.at(aidlUuid) /* sub-effect descriptor list */,
                mFactory);
    } else {
        RETURN_STATUS_IF_ERROR(
                statusTFromBinderStatus(mFactory->createEffect(aidlUuid, &aidlEffect)));
    }
    if (aidlEffect == nullptr) {
        ALOGE("%s failed to create effect with UUID: %s", __func__,
              ::android::audio::utils::toString(aidlUuid).c_str());
        return NAME_NOT_FOUND;
    }
    Descriptor desc;
    RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(aidlEffect->getDescriptor(&desc)));

    *effect = sp<EffectHalAidl>::make(mFactory, aidlEffect, sessionId, ioId, desc, isProxy);
    return OK;
}

status_t EffectsFactoryHalAidl::dumpEffects(int fd) {
    // TODO: b/333803769 improve the effect dump implementation
    RETURN_STATUS_IF_ERROR(mFactory->dump(fd, nullptr, 0));
    return OK;
}

status_t EffectsFactoryHalAidl::allocateBuffer(size_t size, sp<EffectBufferHalInterface>* buffer) {
    return EffectBufferHalAidl::allocate(size, buffer);
}

status_t EffectsFactoryHalAidl::mirrorBuffer(void* external, size_t size,
                                             sp<EffectBufferHalInterface>* buffer) {
    return EffectBufferHalAidl::mirror(external, size, buffer);
}

AudioHalVersionInfo EffectsFactoryHalAidl::getHalVersion() const {
    return mHalVersion;
}

status_t EffectsFactoryHalAidl::getHalDescriptorWithImplUuid(const AudioUuid& uuid,
                                                             effect_descriptor_t* pDescriptor) {
    if (pDescriptor == nullptr) {
        return BAD_VALUE;
    }

    const auto& list = isProxyEffect(uuid) ? mProxyDescList : mNonProxyDescList;
    auto matchIt = std::find_if(list.begin(), list.end(),
                                [&](const auto& desc) { return desc.common.id.uuid == uuid; });
    if (matchIt == list.end()) {
        ALOGE("%s UUID not found in HAL and proxy list %s", __func__,
              ::android::audio::utils::toString(uuid).c_str());
        return NAME_NOT_FOUND;
    }

    *pDescriptor = VALUE_OR_RETURN_STATUS(
            ::aidl::android::aidl2legacy_Descriptor_effect_descriptor(*matchIt));
    return OK;
}

status_t EffectsFactoryHalAidl::getHalDescriptorWithTypeUuid(
        const AudioUuid& type, std::vector<effect_descriptor_t>* descriptors) {
    if (descriptors == nullptr) {
        return BAD_VALUE;
    }

    std::vector<Descriptor> result;
    std::copy_if(mNonProxyDescList.begin(), mNonProxyDescList.end(), std::back_inserter(result),
                 [&](auto& desc) { return desc.common.id.type == type; });
    std::copy_if(mProxyDescList.begin(), mProxyDescList.end(), std::back_inserter(result),
                 [&](auto& desc) { return desc.common.id.type == type; });
    if (result.empty()) {
        ALOGW("%s UUID type not found in HAL and proxy list %s", __func__,
              ::android::audio::utils::toString(type).c_str());
        return BAD_VALUE;
    }

    *descriptors = VALUE_OR_RETURN_STATUS(
            aidl::android::convertContainer<std::vector<effect_descriptor_t>>(
                    result, ::aidl::android::aidl2legacy_Descriptor_effect_descriptor));
    return OK;
}

bool EffectsFactoryHalAidl::isProxyEffect(const AudioUuid& uuid) const {
    return 0 != mProxyUuidDescriptorMap.count(uuid);
}

std::shared_ptr<const effectsConfig::Processings> EffectsFactoryHalAidl::getProcessings() const {

    auto getConfigEffectWithDescriptor =
            [](const auto& desc) -> std::shared_ptr<const effectsConfig::Effect> {
        effectsConfig::Effect effect = {.name = desc.common.name, .isProxy = false};
        if (const auto uuid =
                    ::aidl::android::aidl2legacy_AudioUuid_audio_uuid_t(desc.common.id.uuid);
            uuid.ok()) {
            static_cast<effectsConfig::EffectImpl&>(effect).uuid = uuid.value();
            return std::make_shared<const effectsConfig::Effect>(effect);
        } else {
            return nullptr;
        }
    };

    auto getConfigProcessingWithAidlProcessing =
            [&](const auto& aidlProcess, std::vector<effectsConfig::InputStream>& preprocess,
                std::vector<effectsConfig::OutputStream>& postprocess,
                std::vector<effectsConfig::DeviceEffects>& deviceprocess) {
                if (aidlProcess.type.getTag() == Processing::Type::streamType) {
                    AudioStreamType aidlType =
                            aidlProcess.type.template get<Processing::Type::streamType>();
                    const auto type =
                            ::aidl::android::aidl2legacy_AudioStreamType_audio_stream_type_t(
                                    aidlType);
                    if (!type.ok()) {
                        return;
                    }

                    std::vector<std::shared_ptr<const effectsConfig::Effect>> effects;
                    std::transform(aidlProcess.ids.begin(), aidlProcess.ids.end(),
                                   std::back_inserter(effects), getConfigEffectWithDescriptor);
                    effectsConfig::OutputStream stream = {.type = type.value(),
                                                          .effects = std::move(effects)};
                    postprocess.emplace_back(stream);
                } else if (aidlProcess.type.getTag() == Processing::Type::source) {
                    AudioSource aidlType =
                            aidlProcess.type.template get<Processing::Type::source>();
                    const auto type =
                            ::aidl::android::aidl2legacy_AudioSource_audio_source_t(aidlType);
                    if (!type.ok()) {
                        return;
                    }

                    std::vector<std::shared_ptr<const effectsConfig::Effect>> effects;
                    std::transform(aidlProcess.ids.begin(), aidlProcess.ids.end(),
                                   std::back_inserter(effects), getConfigEffectWithDescriptor);
                    effectsConfig::InputStream stream = {.type = type.value(),
                                                         .effects = std::move(effects)};
                    preprocess.emplace_back(stream);
                } else if (aidlProcess.type.getTag() == Processing::Type::device) {
                    AudioDevice aidlDevice =
                            aidlProcess.type.template get<Processing::Type::device>();
                    std::vector<std::shared_ptr<const effectsConfig::Effect>> effects;
                    std::transform(aidlProcess.ids.begin(), aidlProcess.ids.end(),
                                   std::back_inserter(effects), getConfigEffectWithDescriptor);
                    audio_devices_t type;
                    char address[AUDIO_DEVICE_MAX_ADDRESS_LEN];
                    status_t status = ::aidl::android::aidl2legacy_AudioDevice_audio_device(
                            aidlDevice, &type, address);
                    if (status != NO_ERROR) {
                        ALOGE("%s device effect has invalid device type / address", __func__);
                        return;
                    }
                    effectsConfig::DeviceEffects device = {
                            {.type = type, .effects = std::move(effects)},
                            .address = address,
                    };
                    deviceprocess.emplace_back(device);
                }
            };

    static std::shared_ptr<const effectsConfig::Processings> processings(
            [&]() -> std::shared_ptr<const effectsConfig::Processings> {
                std::vector<effectsConfig::InputStream> preprocess;
                std::vector<effectsConfig::OutputStream> postprocess;
                std::vector<effectsConfig::DeviceEffects> deviceprocess;
                for (const auto& processing : mAidlProcessings) {
                    getConfigProcessingWithAidlProcessing(processing, preprocess, postprocess,
                                                          deviceprocess);
                }

                if (0 == preprocess.size() && 0 == postprocess.size() &&
                    0 == deviceprocess.size()) {
                    return nullptr;
                }

                return std::make_shared<const effectsConfig::Processings>(
                        effectsConfig::Processings({.preprocess = std::move(preprocess),
                                                    .postprocess = std::move(postprocess),
                                                    .deviceprocess = std::move(deviceprocess)}));
            }());

    return processings;
}

// Return 0 for AIDL, as the AIDL interface is not aware of the configuration file.
::android::error::Result<size_t> EffectsFactoryHalAidl::getSkippedElements() const {
    return 0;
}


bool EffectsFactoryHalAidl::isAudioEraser(const AudioUuid& uuid) {
    return uuid == getEffectTypeUuidEraser();
}

void EffectsFactoryHalAidl::filterHalDescriptors(std::vector<Descriptor>& descs) {
    if (!com_android_media_audio_audio_eraser_effect()) {
        descs.erase(std::remove_if(descs.begin(), descs.end(),
                                   [](const Descriptor& desc) {
                                       return isAudioEraser(desc.common.id.type);
                                   }),
                    descs.end());
    }

    return;
}

} // namespace effect

// When a shared library is built from a static library, even explicit
// exports from a static library are optimized out unless actually used by
// the shared library. See EffectsFactoryHalEntry.cpp.
extern "C" void* createIEffectsFactoryImpl() {
    return new effect::EffectsFactoryHalAidl(getServiceInstance<IFactory>("default"));
}

} // namespace android
