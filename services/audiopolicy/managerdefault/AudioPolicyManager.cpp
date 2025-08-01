/*
 * Copyright (C) 2009 The Android Open Source Project
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

#define LOG_TAG "APM_AudioPolicyManager"

// Need to keep the log statements even in production builds
// to enable VERBOSE logging dynamically.
// You can enable VERBOSE logging as follows:
// adb shell setprop log.tag.APM_AudioPolicyManager V
#define LOG_NDEBUG 0

//#define VERY_VERBOSE_LOGGING
#ifdef VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#include <algorithm>
#include <inttypes.h>
#include <map>
#include <math.h>
#include <set>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <Serializer.h>
#include <android/media/audio/common/AudioMMapPolicy.h>
#include <android/media/audio/common/AudioPort.h>
#include <com_android_media_audio.h>
#include <android_media_audiopolicy.h>
#include <com_android_media_audioserver.h>
#include <cutils/bitops.h>
#include <error/expected_utils.h>
#include <media/AudioParameter.h>
#include <policy.h>
#include <private/android_filesystem_config.h>
#include <system/audio.h>
#include <system/audio_config.h>
#include <system/audio_effects/effect_hapticgenerator.h>
#include <utils/Log.h>

#include "AudioPolicyManager.h"
#include "SpatializerHelper.h"
#include "TypeConverter.h"

namespace android {


namespace audio_flags = android::media::audiopolicy;

using android::media::audio::common::AudioDevice;
using android::media::audio::common::AudioDeviceAddress;
using android::media::audio::common::AudioDeviceDescription;
using android::media::audio::common::AudioMMapPolicy;
using android::media::audio::common::AudioMMapPolicyInfo;
using android::media::audio::common::AudioMMapPolicyType;
using android::media::audio::common::AudioPortDeviceExt;
using android::media::audio::common::AudioPortExt;
using android::media::audio::common::AudioConfigBase;
using binder::Status;
using com::android::media::audioserver::fix_call_audio_patch;
using content::AttributionSourceState;

//FIXME: workaround for truncated touch sounds
// to be removed when the problem is handled by system UI
#define TOUCH_SOUND_FIXED_DELAY_MS 100

// Largest difference in dB on earpiece in call between the voice volume and another
// media / notification / system volume.
constexpr float IN_CALL_EARPIECE_HEADROOM_DB = 3.f;

template <typename T>
bool operator== (const SortedVector<T> &left, const SortedVector<T> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < right.size(); index++) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

template <typename T>
bool operator!= (const SortedVector<T> &left, const SortedVector<T> &right)
{
    return !(left == right);
}

// ----------------------------------------------------------------------------
// AudioPolicyInterface implementation
// ----------------------------------------------------------------------------

status_t AudioPolicyManager::setDeviceConnectionState(audio_policy_dev_state_t state,
        const android::media::audio::common::AudioPort& port, audio_format_t encodedFormat,
        bool deviceSwitch) {
    status_t status = setDeviceConnectionStateInt(state, port, encodedFormat, deviceSwitch);
    nextAudioPortGeneration();
    return status;
}

status_t AudioPolicyManager::setDeviceConnectionState(audio_devices_t device,
                                                      audio_policy_dev_state_t state,
                                                      const char* device_address,
                                                      const char* device_name,
                                                      audio_format_t encodedFormat) {
    media::AudioPortFw aidlPort;
    if (status_t status = deviceToAudioPort(device, device_address, device_name, &aidlPort);
        status == OK) {
        return setDeviceConnectionState(state, aidlPort.hal, encodedFormat, false /*deviceSwitch*/);
    } else {
        ALOGE("Failed to convert to AudioPort Parcelable: %s", statusToString(status).c_str());
        return status;
    }
}

status_t AudioPolicyManager::broadcastDeviceConnectionState(const sp<DeviceDescriptor> &device,
                                                        media::DeviceConnectedState state)
{
    audio_port_v7 devicePort;
    device->toAudioPort(&devicePort);
    status_t status = mpClientInterface->setDeviceConnectedState(&devicePort, state);
    ALOGE_IF(status != OK, "Error %d while setting connected state %d for device %s", status,
             static_cast<int>(state), device->getDeviceTypeAddr().toString(false).c_str());

    return status;
}

status_t AudioPolicyManager::setDeviceConnectionStateInt(
        audio_policy_dev_state_t state, const android::media::audio::common::AudioPort& port,
        audio_format_t encodedFormat, bool deviceSwitch) {
    if (port.ext.getTag() != AudioPortExt::device) {
        return BAD_VALUE;
    }
    audio_devices_t device_type;
    std::string device_address;
    if (status_t status = aidl2legacy_AudioDevice_audio_device(
                port.ext.get<AudioPortExt::device>().device, &device_type, &device_address);
        status != OK) {
        return status;
    };
    const char* device_name = port.name.c_str();
    // connect/disconnect only 1 device at a time
    if (!audio_is_output_device(device_type) && !audio_is_input_device(device_type))
        return BAD_VALUE;

    sp<DeviceDescriptor> device = mHwModules.getDeviceDescriptor(
            device_type, device_address.c_str(), device_name, encodedFormat,
            state == AUDIO_POLICY_DEVICE_STATE_AVAILABLE);
    if (device == nullptr) {
        return INVALID_OPERATION;
    }
    if (state == AUDIO_POLICY_DEVICE_STATE_AVAILABLE) {
        device->setExtraAudioDescriptors(port.extraAudioDescriptors);
    }
    return setDeviceConnectionStateInt(device, state, deviceSwitch);
}

status_t AudioPolicyManager::setDeviceConnectionStateInt(audio_devices_t deviceType,
                                                         audio_policy_dev_state_t state,
                                                         const char* device_address,
                                                         const char* device_name,
                                                         audio_format_t encodedFormat,
                                                         bool deviceSwitch) {
    media::AudioPortFw aidlPort;
    if (status_t status = deviceToAudioPort(deviceType, device_address, device_name, &aidlPort);
        status == OK) {
        return setDeviceConnectionStateInt(state, aidlPort.hal, encodedFormat, deviceSwitch);
    } else {
        ALOGE("Failed to convert to AudioPort Parcelable: %s", statusToString(status).c_str());
        return status;
    }
}

status_t AudioPolicyManager::setDeviceConnectionStateInt(const sp<DeviceDescriptor> &device,
                                                         audio_policy_dev_state_t state,
                                                         bool deviceSwitch)
{
    // handle output devices
    if (audio_is_output_device(device->type())) {
        SortedVector <audio_io_handle_t> outputs;

        ssize_t index = mAvailableOutputDevices.indexOf(device);

        // save a copy of the opened output descriptors before any output is opened or closed
        // by checkOutputsForDevice(). This will be needed by checkOutputForAllStrategies()
        mPreviousOutputs = mOutputs;

        bool wasLeUnicastActive = isLeUnicastActive();

        switch (state)
        {
        // handle output device connection
        case AUDIO_POLICY_DEVICE_STATE_AVAILABLE: {
            if (index >= 0) {
                ALOGW("%s() device already connected: %s", __func__, device->toString().c_str());
                return INVALID_OPERATION;
            }
            ALOGV("%s() connecting device %s format %x",
                    __func__, device->toString().c_str(), device->getEncodedFormat());

            // register new device as available
            if (mAvailableOutputDevices.add(device) < 0) {
                return NO_MEMORY;
            }

            // Before checking outputs, broadcast connect event to allow HAL to retrieve dynamic
            // parameters on newly connected devices (instead of opening the outputs...)
            if (broadcastDeviceConnectionState(
                        device, media::DeviceConnectedState::CONNECTED) != NO_ERROR) {
                mAvailableOutputDevices.remove(device);
                mHwModules.cleanUpForDevice(device);
                ALOGE("%s() device %s format %x connection failed", __func__,
                      device->toString().c_str(), device->getEncodedFormat());
                return INVALID_OPERATION;
            }

            if (checkOutputsForDevice(device, state, outputs) != NO_ERROR) {
                mAvailableOutputDevices.remove(device);

                broadcastDeviceConnectionState(device, media::DeviceConnectedState::DISCONNECTED);

                mHwModules.cleanUpForDevice(device);
                return INVALID_OPERATION;
            }

            // Populate encapsulation information when a output device is connected.
            device->setEncapsulationInfoFromHal(mpClientInterface);

            // outputs should never be empty here
            ALOG_ASSERT(outputs.size() != 0, "setDeviceConnectionState():"
                    "checkOutputsForDevice() returned no outputs but status OK");
            ALOGV("%s() checkOutputsForDevice() returned %zu outputs", __func__, outputs.size());

            } break;
        // handle output device disconnection
        case AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE: {
            if (index < 0) {
                ALOGW("%s() device not connected: %s", __func__, device->toString().c_str());
                return INVALID_OPERATION;
            }

            ALOGV("%s() disconnecting output device %s", __func__, device->toString().c_str());

            // Notify the HAL to prepare to disconnect device
            broadcastDeviceConnectionState(
                    device, media::DeviceConnectedState::PREPARE_TO_DISCONNECT);

            // remove device from available output devices
            mAvailableOutputDevices.remove(device);

            mOutputs.clearSessionRoutesForDevice(device);

            checkOutputsForDevice(device, state, outputs);

            // Send Disconnect to HALs
            broadcastDeviceConnectionState(device, media::DeviceConnectedState::DISCONNECTED);

            // Reset active device codec
            device->setEncodedFormat(AUDIO_FORMAT_DEFAULT);

            // remove device from mReportedFormatsMap cache
            mReportedFormatsMap.erase(device);

            // remove preferred mixer configurations
            mPreferredMixerAttrInfos.erase(device->getId());

            } break;

        default:
            ALOGE("%s() invalid state: %x", __func__, state);
            return BAD_VALUE;
        }

        // Propagate device availability to Engine
        setEngineDeviceConnectionState(device, state);

        // No need to evaluate playback routing when connecting a remote submix
        // output device used by a dynamic policy of type recorder as no
        // playback use case is affected.
        bool doCheckForDeviceAndOutputChanges = true;
        if (device->type() == AUDIO_DEVICE_OUT_REMOTE_SUBMIX && device->address() != "0") {
            for (audio_io_handle_t output : outputs) {
                sp<SwAudioOutputDescriptor> desc = mOutputs.valueFor(output);
                sp<AudioPolicyMix> policyMix = desc->mPolicyMix.promote();
                if (policyMix != nullptr
                        && policyMix->mMixType == MIX_TYPE_RECORDERS
                        && device->address() == policyMix->mDeviceAddress.c_str()) {
                    doCheckForDeviceAndOutputChanges = false;
                    break;
                }
            }
        }

        auto checkCloseOutputs = [&]() {
            // outputs must be closed after checkOutputForAllStrategies() is executed
            if (!outputs.isEmpty()) {
                for (audio_io_handle_t output : outputs) {
                    sp<SwAudioOutputDescriptor> desc = mOutputs.valueFor(output);
                    // close unused outputs after device disconnection or direct outputs that have
                    // been opened by checkOutputsForDevice() to query dynamic parameters
                    // "outputs" vector never contains duplicated outputs
                    if ((state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE)
                            || (((desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) != 0) &&
                                (desc->mDirectOpenCount == 0))
                            || (((desc->mFlags & AUDIO_OUTPUT_FLAG_SPATIALIZER) != 0) &&
                                !isOutputOnlyAvailableRouteToSomeDevice(desc))) {
                        clearAudioSourcesForOutput(output);
                        closeOutput(output);
                    }
                }
                // check A2DP again after closing A2DP output to reset mA2dpSuspended if needed
                return true;
            }
            return false;
        };

        if (doCheckForDeviceAndOutputChanges && !deviceSwitch) {
            checkForDeviceAndOutputChanges(checkCloseOutputs);
        } else {
            checkCloseOutputs();
        }
        if (!deviceSwitch) {
            (void)updateCallRouting(false /*fromCache*/);
            const DeviceVector msdOutDevices = getMsdAudioOutDevices();
            const DeviceVector activeMediaDevices =
                    mEngine->getActiveMediaDevices(mAvailableOutputDevices);
            std::map<audio_io_handle_t, DeviceVector> outputsToReopenWithDevices;
            for (size_t i = 0; i < mOutputs.size(); i++) {
                sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
                if (desc->isActive() && ((mEngine->getPhoneState() != AUDIO_MODE_IN_CALL) ||
                    (desc != mPrimaryOutput))) {
                    DeviceVector newDevices = getNewOutputDevices(desc, true /*fromCache*/);
                    // do not force device change on duplicated output because if device is 0,
                    // it will also force a device 0 for the two outputs it is duplicated to
                    // a valid device selection on those outputs.
                    bool force = (msdOutDevices.isEmpty() || msdOutDevices != desc->devices())
                            && !desc->isDuplicated()
                            && (!device_distinguishes_on_address(device->type())
                                    // always force when disconnecting (a non-duplicated device)
                                    || (state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE));
                    if (desc->mPreferredAttrInfo != nullptr && newDevices != desc->devices()) {
                        // If the device is using preferred mixer attributes, the output need to
                        // reopen with default configuration when the new selected devices are
                        // different from current routing devices
                        outputsToReopenWithDevices.emplace(mOutputs.keyAt(i), newDevices);
                        continue;
                    }
                    setOutputDevices(__func__, desc, newDevices, force, 0);
                }
                if (!desc->isDuplicated() && desc->mProfile->hasDynamicAudioProfile() &&
                        !activeMediaDevices.empty() && desc->devices() != activeMediaDevices &&
                        desc->supportsDevicesForPlayback(activeMediaDevices)) {
                    // Reopen the output to query the dynamic profiles when there is not active
                    // clients or all active clients will be rerouted. Otherwise, set the flag
                    // `mPendingReopenToQueryProfiles` in the SwOutputDescriptor so that the output
                    // can be reopened to query dynamic profiles when all clients are inactive.
                    if (areAllActiveTracksRerouted(desc)) {
                        outputsToReopenWithDevices.emplace(mOutputs.keyAt(i), activeMediaDevices);
                    } else {
                        desc->mPendingReopenToQueryProfiles = true;
                    }
                }
                if (!desc->supportsDevicesForPlayback(activeMediaDevices)) {
                    // Clear the flag that previously set for re-querying profiles.
                    desc->mPendingReopenToQueryProfiles = false;
                }
            }
            reopenOutputsWithDevices(outputsToReopenWithDevices);
        }

        if (state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE) {
            cleanUpForDevice(device);
        }

        checkLeBroadcastRoutes(wasLeUnicastActive, nullptr, 0);

        mpClientInterface->onAudioPortListUpdate();
        ALOGV("%s() completed for device: %s", __func__, device->toString().c_str());
        return NO_ERROR;
    }  // end if is output device

    // handle input devices
    if (audio_is_input_device(device->type())) {
        ssize_t index = mAvailableInputDevices.indexOf(device);
        switch (state)
        {
        // handle input device connection
        case AUDIO_POLICY_DEVICE_STATE_AVAILABLE: {
            if (index >= 0) {
                ALOGW("%s() device already connected: %s", __func__, device->toString().c_str());
                return INVALID_OPERATION;
            }

            ALOGV("%s() connecting device %s", __func__, device->toString().c_str());

            if (mAvailableInputDevices.add(device) < 0) {
                return NO_MEMORY;
            }

            // Before checking intputs, broadcast connect event to allow HAL to retrieve dynamic
            // parameters on newly connected devices (instead of opening the inputs...)
            if (broadcastDeviceConnectionState(
                        device, media::DeviceConnectedState::CONNECTED) != NO_ERROR) {
                mAvailableInputDevices.remove(device);
                mHwModules.cleanUpForDevice(device);
                ALOGE("%s() device %s format %x connection failed", __func__,
                      device->toString().c_str(), device->getEncodedFormat());
                return INVALID_OPERATION;
            }
            // Propagate device availability to Engine
            setEngineDeviceConnectionState(device, state);

            if (checkInputsForDevice(device, state) != NO_ERROR) {
                setEngineDeviceConnectionState(device, AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE);

                mAvailableInputDevices.remove(device);

                broadcastDeviceConnectionState(device, media::DeviceConnectedState::DISCONNECTED);

                mHwModules.cleanUpForDevice(device);

                return INVALID_OPERATION;
            }

        } break;

        // handle input device disconnection
        case AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE: {
            if (index < 0) {
                ALOGW("%s() device not connected: %s", __func__, device->toString().c_str());
                return INVALID_OPERATION;
            }

            ALOGV("%s() disconnecting input device %s", __func__, device->toString().c_str());

            // Notify the HAL to prepare to disconnect device
            broadcastDeviceConnectionState(
                    device, media::DeviceConnectedState::PREPARE_TO_DISCONNECT);

            mAvailableInputDevices.remove(device);

            checkInputsForDevice(device, state);

            // Set Disconnect to HALs
            broadcastDeviceConnectionState(device, media::DeviceConnectedState::DISCONNECTED);

            // remove device from mReportedFormatsMap cache
            mReportedFormatsMap.erase(device);

            // Propagate device availability to Engine
            setEngineDeviceConnectionState(device, state);
        } break;

        default:
            ALOGE("%s() invalid state: %x", __func__, state);
            return BAD_VALUE;
        }

        if (!deviceSwitch) {
            checkCloseInputs();
            // As the input device list can impact the output device selection, update
            // getDeviceForStrategy() cache
            updateDevicesAndOutputs();

            (void)updateCallRouting(false /*fromCache*/);
            // Reconnect Audio Source
            for (const auto &strategy : mEngine->getOrderedProductStrategies()) {
                auto attributes = mEngine->getAllAttributesForProductStrategy(strategy).front();
                checkAudioSourceForAttributes(attributes);
            }

            if (state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE) {
                cleanUpForDevice(device);
            }
        }

        mpClientInterface->onAudioPortListUpdate();
        ALOGV("%s() completed for device: %s", __func__, device->toString().c_str());
        return NO_ERROR;
    } // end if is input device

    ALOGW("%s() invalid device: %s", __func__, device->toString().c_str());
    return BAD_VALUE;
}

status_t AudioPolicyManager::deviceToAudioPort(audio_devices_t device, const char* device_address,
                                               const char* device_name,
                                               media::AudioPortFw* aidlPort) {
    const auto devDescr = sp<DeviceDescriptorBase>::make(device, device_address);
    devDescr->setName(device_name);
    return devDescr->writeToParcelable(aidlPort);
}

void AudioPolicyManager::setEngineDeviceConnectionState(const sp<DeviceDescriptor> device,
                                      audio_policy_dev_state_t state) {

    // the Engine does not have to know about remote submix devices used by dynamic audio policies
    if (audio_is_remote_submix_device(device->type()) && device->address() != "0") {
        return;
    }
    mEngine->setDeviceConnectionState(device, state);
}


audio_policy_dev_state_t AudioPolicyManager::getDeviceConnectionState(audio_devices_t device,
                                                                      const char *device_address)
{
    sp<DeviceDescriptor> devDesc =
            mHwModules.getDeviceDescriptor(device, device_address, "", AUDIO_FORMAT_DEFAULT,
                                           false /* allowToCreate */,
                                           (strlen(device_address) != 0)/*matchAddress*/);

    if (devDesc == 0) {
        ALOGV("getDeviceConnectionState() undeclared device, type %08x, address: %s",
              device, device_address);
        return AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
    }

    DeviceVector *deviceVector;

    if (audio_is_output_device(device)) {
        deviceVector = &mAvailableOutputDevices;
    } else if (audio_is_input_device(device)) {
        deviceVector = &mAvailableInputDevices;
    } else {
        ALOGW("%s() invalid device type %08x", __func__, device);
        return AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
    }

    return (deviceVector->getDevice(
                device, String8(device_address), AUDIO_FORMAT_DEFAULT) != 0) ?
            AUDIO_POLICY_DEVICE_STATE_AVAILABLE : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
}

status_t AudioPolicyManager::handleDeviceConfigChange(audio_devices_t device,
                                                      const char *device_address,
                                                      const char *device_name,
                                                      audio_format_t encodedFormat)
{
    ALOGV("handleDeviceConfigChange(() device: 0x%X, address %s name %s encodedFormat: 0x%X",
          device, device_address, device_name, encodedFormat);

    // connect/disconnect only 1 device at a time
    if (!audio_is_output_device(device) && !audio_is_input_device(device)) return BAD_VALUE;

    // Check if the device is currently connected
    DeviceVector deviceList = mAvailableOutputDevices.getDevicesFromType(device);
    if (deviceList.empty()) {
        // Nothing to do: device is not connected
        return NO_ERROR;
    }
    sp<DeviceDescriptor> devDesc = deviceList.itemAt(0);

    // For offloaded A2DP, Hw modules may have the capability to
    // configure codecs.
    // Handle two specific cases by sending a set parameter to
    // configure A2DP codecs. No need to toggle device state.
    // Case 1: A2DP active device switches from primary to primary
    // module
    // Case 2: A2DP device config changes on primary module.
    if (device_has_encoding_capability(device) && hasPrimaryOutput()) {
        sp<HwModule> module = mHwModules.getModuleForDeviceType(device, encodedFormat);
        audio_module_handle_t primaryHandle = mPrimaryOutput->getModuleHandle();
        if (availablePrimaryOutputDevices().contains(devDesc) &&
           (module != 0 && module->getHandle() == primaryHandle)) {
            bool isA2dp = audio_is_a2dp_out_device(device);
            const String8 supportKey = isA2dp ? String8(AudioParameter::keyReconfigA2dpSupported)
                    : String8(AudioParameter::keyReconfigLeSupported);
            String8 reply = mpClientInterface->getParameters(AUDIO_IO_HANDLE_NONE, supportKey);
            AudioParameter repliedParameters(reply);
            int isReconfigSupported;
            repliedParameters.getInt(supportKey, isReconfigSupported);
            if (isReconfigSupported) {
                const String8 key = isA2dp ? String8(AudioParameter::keyReconfigA2dp)
                        : String8(AudioParameter::keyReconfigLe);
                AudioParameter param;
                param.add(key, String8("true"));
                mpClientInterface->setParameters(AUDIO_IO_HANDLE_NONE, param.toString());
                devDesc->setEncodedFormat(encodedFormat);
                return NO_ERROR;
            }
        }
    }
    auto musicStrategy = streamToStrategy(AUDIO_STREAM_MUSIC);
    uint32_t muteWaitMs = 0;
    for (size_t i = 0; i < mOutputs.size(); i++) {
       sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
       // mute media strategies to avoid sending the music tail into
       // the earpiece or headset.
       if (desc->isStrategyActive(musicStrategy)) {
           uint32_t tempRecommendedMuteDuration = desc->getRecommendedMuteDurationMs();
           uint32_t tempMuteDurationMs = tempRecommendedMuteDuration > 0 ?
                        tempRecommendedMuteDuration : desc->latency() * 4;
           if (muteWaitMs < tempMuteDurationMs) {
               muteWaitMs = tempMuteDurationMs;
           }
       }
       setStrategyMute(musicStrategy, true, desc);
       setStrategyMute(musicStrategy, false, desc, MUTE_TIME_MS,
          mEngine->getOutputDevicesForAttributes(attributes_initializer(AUDIO_USAGE_MEDIA),
                                              nullptr, true /*fromCache*/).types());
    }
    // Wait for the muted audio to propagate down the audio path see checkDeviceMuteStrategies().
    // We assume that MUTE_TIME_MS is way larger than muteWaitMs so that unmuting still
    // happens after the actual device switch.
    if (muteWaitMs > 0) {
        ALOGW_IF(MUTE_TIME_MS < muteWaitMs * 2, "%s excessive mute wait %d", __func__, muteWaitMs);
        usleep(muteWaitMs * 1000);
    }
    // Toggle the device state: UNAVAILABLE -> AVAILABLE
    // This will force reading again the device configuration
    status_t status = setDeviceConnectionState(device,
                                      AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                      device_address, device_name,
                                      devDesc->getEncodedFormat());
    if (status != NO_ERROR) {
        ALOGW("handleDeviceConfigChange() error disabling connection state: %d",
              status);
        return status;
    }

    status = setDeviceConnectionState(device,
                                      AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                                      device_address, device_name, encodedFormat);
    if (status != NO_ERROR) {
        ALOGW("handleDeviceConfigChange() error enabling connection state: %d",
              status);
        return status;
    }

    return NO_ERROR;
}

status_t AudioPolicyManager::getHwOffloadFormatsSupportedForBluetoothMedia(
                                    audio_devices_t device, std::vector<audio_format_t> *formats)
{
    ALOGV("getHwOffloadFormatsSupportedForBluetoothMedia()");
    status_t status = NO_ERROR;
    std::unordered_set<audio_format_t> formatSet;
    sp<HwModule> primaryModule =
            mHwModules.getModuleFromName(AUDIO_HARDWARE_MODULE_ID_PRIMARY);
    if (primaryModule == nullptr) {
        ALOGE("%s() unable to get primary module", __func__);
        return NO_INIT;
    }

    DeviceTypeSet audioDeviceSet;

    switch(device) {
    case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP:
        audioDeviceSet = getAudioDeviceOutAllA2dpSet();
        break;
    case AUDIO_DEVICE_OUT_BLE_HEADSET:
        audioDeviceSet = getAudioDeviceOutLeAudioUnicastSet();
        break;
    case AUDIO_DEVICE_OUT_BLE_BROADCAST:
        audioDeviceSet = getAudioDeviceOutLeAudioBroadcastSet();
        break;
    default:
        ALOGE("%s() device type 0x%08x not supported", __func__, device);
        return BAD_VALUE;
    }

    DeviceVector declaredDevices = primaryModule->getDeclaredDevices().getDevicesFromTypes(
            audioDeviceSet);
    for (const auto& device : declaredDevices) {
        formatSet.insert(device->encodedFormats().begin(), device->encodedFormats().end());
    }
    formats->assign(formatSet.begin(), formatSet.end());
    return status;
}

DeviceVector AudioPolicyManager::selectBestRxSinkDevicesForCall(bool fromCache)
{
    DeviceVector rxSinkdevices{};
    rxSinkdevices = mEngine->getOutputDevicesForAttributes(
                attributes_initializer(AUDIO_USAGE_VOICE_COMMUNICATION), nullptr, fromCache);
    if (!rxSinkdevices.isEmpty() && mAvailableOutputDevices.contains(rxSinkdevices.itemAt(0))) {
        auto rxSinkDevice = rxSinkdevices.itemAt(0);
        auto telephonyRxModule = mHwModules.getModuleForDeviceType(
                    AUDIO_DEVICE_IN_TELEPHONY_RX, AUDIO_FORMAT_DEFAULT);
        // retrieve Rx Source device descriptor
        sp<DeviceDescriptor> rxSourceDevice = mAvailableInputDevices.getDevice(
                    AUDIO_DEVICE_IN_TELEPHONY_RX, String8(), AUDIO_FORMAT_DEFAULT);

        // RX Telephony and Rx sink devices are declared by Primary Audio HAL
        if (isPrimaryModule(telephonyRxModule) && (telephonyRxModule->getHalVersionMajor() >= 3) &&
                telephonyRxModule->supportsPatch(rxSourceDevice, rxSinkDevice)) {
            ALOGI("%s() device %s using HW Bridge", __func__, rxSinkDevice->toString().c_str());
            return DeviceVector(rxSinkDevice);
        }
    }
    // Note that despite the fact that getNewOutputDevices() is called on the primary output,
    // the device returned is not necessarily reachable via this output
    // (filter later by setOutputDevices())
    return getNewOutputDevices(mPrimaryOutput, fromCache);
}

status_t AudioPolicyManager::updateCallRouting(bool fromCache, uint32_t delayMs, uint32_t *waitMs)
{
    if (mEngine->getPhoneState() == AUDIO_MODE_IN_CALL) {
        DeviceVector rxDevices = selectBestRxSinkDevicesForCall(fromCache);
        return updateCallRoutingInternal(rxDevices, delayMs, waitMs);
    }
    return INVALID_OPERATION;
}

status_t AudioPolicyManager::updateCallRoutingInternal(
        const DeviceVector &rxDevices, uint32_t delayMs, uint32_t *waitMs)
{
    bool createTxPatch = false;
    bool createRxPatch = false;
    uint32_t muteWaitMs = 0;
    if (hasPrimaryOutput() &&
            mPrimaryOutput->devices().onlyContainsDevicesWithType(AUDIO_DEVICE_OUT_STUB)) {
        return INVALID_OPERATION;
    }

    audio_attributes_t attr = { .source = AUDIO_SOURCE_VOICE_COMMUNICATION };
    auto txSourceDevice = mEngine->getInputDeviceForAttributes(attr);

    if (!fix_call_audio_patch()) {
        disconnectTelephonyAudioSource(mCallRxSourceClient);
        disconnectTelephonyAudioSource(mCallTxSourceClient);
    }

    if (rxDevices.isEmpty()) {
        ALOGW("%s() no selected output device", __func__);
        return INVALID_OPERATION;
    }
    if (txSourceDevice == nullptr) {
        ALOGE("%s() selected input device not available", __func__);
        return INVALID_OPERATION;
    }

    ALOGV("%s device rxDevice %s txDevice %s", __func__,
          rxDevices.itemAt(0)->toString().c_str(), txSourceDevice->toString().c_str());

    auto telephonyRxModule =
        mHwModules.getModuleForDeviceType(AUDIO_DEVICE_IN_TELEPHONY_RX, AUDIO_FORMAT_DEFAULT);
    auto telephonyTxModule =
        mHwModules.getModuleForDeviceType(AUDIO_DEVICE_OUT_TELEPHONY_TX, AUDIO_FORMAT_DEFAULT);
    // retrieve Rx Source and Tx Sink device descriptors
    sp<DeviceDescriptor> rxSourceDevice =
        mAvailableInputDevices.getDevice(AUDIO_DEVICE_IN_TELEPHONY_RX,
                                         String8(),
                                         AUDIO_FORMAT_DEFAULT);
    sp<DeviceDescriptor> txSinkDevice =
        mAvailableOutputDevices.getDevice(AUDIO_DEVICE_OUT_TELEPHONY_TX,
                                          String8(),
                                          AUDIO_FORMAT_DEFAULT);

    // RX and TX Telephony device are declared by Primary Audio HAL
    if (isPrimaryModule(telephonyRxModule) && isPrimaryModule(telephonyTxModule) &&
            (telephonyRxModule->getHalVersionMajor() >= 3)) {
        if (rxSourceDevice == 0 || txSinkDevice == 0) {
            // RX / TX Telephony device(s) is(are) not currently available
            ALOGE("%s() no telephony Tx and/or RX device", __func__);
            return INVALID_OPERATION;
        }
        // createAudioPatchInternal now supports both HW / SW bridging
        createRxPatch = true;
        createTxPatch = true;
    } else {
        // If the RX device is on the primary HW module, then use legacy routing method for
        // voice calls via setOutputDevice() on primary output.
        // Otherwise, create two audio patches for TX and RX path.
        createRxPatch = !(availablePrimaryOutputDevices().contains(rxDevices.itemAt(0))) &&
                (rxSourceDevice != 0);
        // If the TX device is also on the primary HW module, setOutputDevice() will take care
        // of it due to legacy implementation. If not, create a patch.
        createTxPatch = !(availablePrimaryModuleInputDevices().contains(txSourceDevice)) &&
                (txSinkDevice != 0);
    }
    // Use legacy routing method for voice calls via setOutputDevice() on primary output.
    // Otherwise, create two audio patches for TX and RX path.
    if (!createRxPatch) {
        if (fix_call_audio_patch()) {
            disconnectTelephonyAudioSource(mCallRxSourceClient);
        }
        if (!hasPrimaryOutput()) {
            ALOGW("%s() no primary output available", __func__);
            return INVALID_OPERATION;
        }
        muteWaitMs = setOutputDevices(__func__, mPrimaryOutput, rxDevices, true, delayMs);
    } else { // create RX path audio patch
        connectTelephonyRxAudioSource(delayMs);
        // If the TX device is on the primary HW module but RX device is
        // on other HW module, SinkMetaData of telephony input should handle it
        // assuming the device uses audio HAL V5.0 and above
    }
    if (createTxPatch) { // create TX path audio patch
        // terminate active capture if on the same HW module as the call TX source device
        // FIXME: would be better to refine to only inputs whose profile connects to the
        // call TX device but this information is not in the audio patch and logic here must be
        // symmetric to the one in startInput()
        for (const auto& activeDesc : mInputs.getActiveInputs()) {
            if (activeDesc->hasSameHwModuleAs(txSourceDevice)) {
                closeActiveClients(activeDesc);
            }
        }
        connectTelephonyTxAudioSource(txSourceDevice, txSinkDevice, delayMs);
    } else if (fix_call_audio_patch()) {
        disconnectTelephonyAudioSource(mCallTxSourceClient);
    }
    if (waitMs != nullptr) {
        *waitMs = muteWaitMs;
    }
    return NO_ERROR;
}

bool AudioPolicyManager::isDeviceOfModule(
        const sp<DeviceDescriptor>& devDesc, const char *moduleId) const {
    sp<HwModule> module = mHwModules.getModuleFromName(moduleId);
    if (module != 0) {
        return mAvailableOutputDevices.getDevicesFromHwModule(module->getHandle())
                .indexOf(devDesc) != NAME_NOT_FOUND
                || mAvailableInputDevices.getDevicesFromHwModule(module->getHandle())
                .indexOf(devDesc) != NAME_NOT_FOUND;
    }
    return false;
}

void AudioPolicyManager::connectTelephonyRxAudioSource(uint32_t delayMs)
{
    const auto aa = mEngine->getAttributesForStreamType(AUDIO_STREAM_VOICE_CALL);

    if (fix_call_audio_patch()) {
        if (mCallRxSourceClient != nullptr) {
            DeviceVector rxDevices =
                  mEngine->getOutputDevicesForAttributes(aa, nullptr, false /*fromCache*/);
            ALOG_ASSERT(!rxDevices.isEmpty() || !mCallRxSourceClient->isConnected(),
                        "connectTelephonyRxAudioSource(): no device found for call RX source");
            sp<DeviceDescriptor> rxDevice = rxDevices.itemAt(0);
            if (mCallRxSourceClient->isConnected()
                    && mCallRxSourceClient->sinkDevice()->equals(rxDevice)) {
                return;
            }
            disconnectTelephonyAudioSource(mCallRxSourceClient);
        }
    } else {
        disconnectTelephonyAudioSource(mCallRxSourceClient);
    }

    const struct audio_port_config source = {
        .role = AUDIO_PORT_ROLE_SOURCE, .type = AUDIO_PORT_TYPE_DEVICE,
        .ext.device.type = AUDIO_DEVICE_IN_TELEPHONY_RX, .ext.device.address = ""
    };
    audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE;

    status_t status = startAudioSourceInternal(&source, &aa, &portId, 0 /*uid*/,
                                       true /*internal*/, true /*isCallRx*/, delayMs);
    ALOGE_IF(status != OK, "%s: failed to start audio source (%d)", __func__, status);
    mCallRxSourceClient = mAudioSources.valueFor(portId);
    ALOGV_IF(mCallRxSourceClient != nullptr, "%s portdID %d between source %s and sink %s",
        __func__, portId, mCallRxSourceClient->srcDevice()->toString().c_str(),
        mCallRxSourceClient->sinkDevice()->toString().c_str());
    ALOGE_IF(mCallRxSourceClient == nullptr,
             "%s failed to start Telephony Rx AudioSource", __func__);
}

void AudioPolicyManager::disconnectTelephonyAudioSource(sp<SourceClientDescriptor> &clientDesc)
{
    if (clientDesc == nullptr) {
        return;
    }
    ALOGW_IF(stopAudioSource(clientDesc->portId()) != NO_ERROR,
            "%s error stopping audio source", __func__);
    clientDesc.clear();
}

void AudioPolicyManager::connectTelephonyTxAudioSource(
        const sp<DeviceDescriptor> &srcDevice, const sp<DeviceDescriptor> &sinkDevice,
        uint32_t delayMs)
{
    if (srcDevice == nullptr || sinkDevice == nullptr) {
        ALOGW("%s could not create patch, invalid sink and/or source device(s)", __func__);
        return;
    }

    if (fix_call_audio_patch()) {
        if (mCallTxSourceClient != nullptr) {
            if (mCallTxSourceClient->isConnected()
                    && mCallTxSourceClient->srcDevice()->equals(srcDevice)) {
                return;
            }
            disconnectTelephonyAudioSource(mCallTxSourceClient);
        }
    } else {
        disconnectTelephonyAudioSource(mCallTxSourceClient);
    }

    PatchBuilder patchBuilder;
    patchBuilder.addSource(srcDevice).addSink(sinkDevice);

    auto callTxSourceClientPortId = PolicyAudioPort::getNextUniqueId();
    const auto aa = mEngine->getAttributesForStreamType(AUDIO_STREAM_VOICE_CALL);

    struct audio_port_config source = {};
    srcDevice->toAudioPortConfig(&source);
    mCallTxSourceClient = new SourceClientDescriptor(
                callTxSourceClientPortId, mUidCached, aa, source, srcDevice, AUDIO_STREAM_PATCH,
                mCommunnicationStrategy, toVolumeSource(aa), true,
                false /*isCallRx*/, true /*isCallTx*/);
    mCallTxSourceClient->setPreferredDeviceId(sinkDevice->getId());

    audio_patch_handle_t patchHandle = AUDIO_PATCH_HANDLE_NONE;
    status_t status = connectAudioSourceToSink(
                mCallTxSourceClient, sinkDevice, patchBuilder.patch(), patchHandle, mUidCached,
                delayMs);
    ALOGE_IF(status != NO_ERROR, "%s() error %d creating TX audio patch", __func__, status);
    ALOGV("%s portdID %d between source %s and sink %s", __func__, callTxSourceClientPortId,
        srcDevice->toString().c_str(), sinkDevice->toString().c_str());
    if (status == NO_ERROR) {
        mAudioSources.add(callTxSourceClientPortId, mCallTxSourceClient);
    }
}

void AudioPolicyManager::setPhoneState(audio_mode_t state)
{
    ALOGV("setPhoneState() state %d", state);
    // store previous phone state for management of sonification strategy below
    int oldState = mEngine->getPhoneState();
    bool wasLeUnicastActive = isLeUnicastActive();

    if (mEngine->setPhoneState(state) != NO_ERROR) {
        ALOGW("setPhoneState() invalid or same state %d", state);
        return;
    }
    /// Opens: can these line be executed after the switch of volume curves???
    if (isStateInCall(oldState)) {
        ALOGV("setPhoneState() in call state management: new state is %d", state);
        // force reevaluating accessibility routing when call stops
        invalidateStreams({AUDIO_STREAM_ACCESSIBILITY});
    }

    /**
     * Switching to or from incall state or switching between telephony and VoIP lead to force
     * routing command.
     */
    bool force = ((isStateInCall(oldState) != isStateInCall(state))
                  || (isStateInCall(state) && (state != oldState)));

    // check for device and output changes triggered by new phone state
    checkForDeviceAndOutputChanges();

    int delayMs = 0;
    if (isStateInCall(state)) {
        nsecs_t sysTime = systemTime();
        auto musicStrategy = streamToStrategy(AUDIO_STREAM_MUSIC);
        auto sonificationStrategy = streamToStrategy(AUDIO_STREAM_ALARM);
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            // mute media and sonification strategies and delay device switch by the largest
            // latency of any output where either strategy is active.
            // This avoid sending the ring tone or music tail into the earpiece or headset.
            if ((desc->isStrategyActive(musicStrategy, SONIFICATION_HEADSET_MUSIC_DELAY, sysTime) ||
                 desc->isStrategyActive(sonificationStrategy, SONIFICATION_HEADSET_MUSIC_DELAY,
                                        sysTime)) &&
                    (delayMs < (int)desc->latency()*2)) {
                delayMs = desc->latency()*2;
            }
            setStrategyMute(musicStrategy, true, desc);
            setStrategyMute(musicStrategy, false, desc, MUTE_TIME_MS,
                mEngine->getOutputDevicesForAttributes(attributes_initializer(AUDIO_USAGE_MEDIA),
                                                       nullptr, true /*fromCache*/).types());
            setStrategyMute(sonificationStrategy, true, desc);
            setStrategyMute(sonificationStrategy, false, desc, MUTE_TIME_MS,
                mEngine->getOutputDevicesForAttributes(attributes_initializer(AUDIO_USAGE_ALARM),
                                                       nullptr, true /*fromCache*/).types());
        }
    }

    if (state == AUDIO_MODE_IN_CALL) {
        (void)updateCallRouting(false /*fromCache*/, delayMs);
    } else {
        if (oldState == AUDIO_MODE_IN_CALL) {
            disconnectTelephonyAudioSource(mCallRxSourceClient);
            disconnectTelephonyAudioSource(mCallTxSourceClient);
        }
        if (hasPrimaryOutput()) {
            DeviceVector rxDevices = getNewOutputDevices(mPrimaryOutput, false /*fromCache*/);
            // force routing command to audio hardware when ending call
            // even if no device change is needed
            if (isStateInCall(oldState) && rxDevices.isEmpty()) {
                rxDevices = mPrimaryOutput->devices();
            }
            setOutputDevices(__func__, mPrimaryOutput, rxDevices, force, 0);
        }
    }

    std::map<audio_io_handle_t, DeviceVector> outputsToReopen;
    // reevaluate routing on all outputs in case tracks have been started during the call
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        DeviceVector newDevices = getNewOutputDevices(desc, true /*fromCache*/);
        if (state != AUDIO_MODE_NORMAL && oldState == AUDIO_MODE_NORMAL
                && desc->mPreferredAttrInfo != nullptr) {
            // If the output is using preferred mixer attributes and the audio mode is not normal,
            // the output need to reopen with default configuration.
            outputsToReopen.emplace(mOutputs.keyAt(i), newDevices);
            continue;
        }
        if (state != AUDIO_MODE_IN_CALL || (desc != mPrimaryOutput && !isTelephonyRxOrTx(desc))) {
            bool forceRouting = !newDevices.isEmpty();
            setOutputDevices(__func__, desc, newDevices, forceRouting, 0 /*delayMs*/, nullptr,
                             true /*requiresMuteCheck*/, !forceRouting /*requiresVolumeCheck*/);
        }
    }
    reopenOutputsWithDevices(outputsToReopen);

    checkLeBroadcastRoutes(wasLeUnicastActive, nullptr, delayMs);

    if (isStateInCall(state)) {
        ALOGV("setPhoneState() in call state management: new state is %d", state);
        // force reevaluating accessibility routing when call starts
        invalidateStreams({AUDIO_STREAM_ACCESSIBILITY});
    }

    // Flag that ringtone volume must be limited to music volume until we exit MODE_RINGTONE
    mLimitRingtoneVolume = (state == AUDIO_MODE_RINGTONE &&
                            isStreamActive(AUDIO_STREAM_MUSIC, SONIFICATION_HEADSET_MUSIC_DELAY));
}

audio_mode_t AudioPolicyManager::getPhoneState() {
    return mEngine->getPhoneState();
}

void AudioPolicyManager::setForceUse(audio_policy_force_use_t usage,
                                     audio_policy_forced_cfg_t config)
{
    ALOGV("setForceUse() usage %d, config %d, mPhoneState %d", usage, config, mEngine->getPhoneState());
    if (config == mEngine->getForceUse(usage)) {
        return;
    }

    if (mEngine->setForceUse(usage, config) != NO_ERROR) {
        ALOGW("setForceUse() could not set force cfg %d for usage %d", config, usage);
        return;
    }
    bool forceVolumeReeval = (usage == AUDIO_POLICY_FORCE_FOR_COMMUNICATION) ||
            (usage == AUDIO_POLICY_FORCE_FOR_DOCK) ||
            (usage == AUDIO_POLICY_FORCE_FOR_SYSTEM);

    // check for device and output changes triggered by new force usage
    checkForDeviceAndOutputChanges();

    // force client reconnection to reevaluate flag AUDIO_FLAG_AUDIBILITY_ENFORCED
    if (usage == AUDIO_POLICY_FORCE_FOR_SYSTEM) {
        invalidateStreams({AUDIO_STREAM_SYSTEM, AUDIO_STREAM_ENFORCED_AUDIBLE});
    }

    //FIXME: workaround for truncated touch sounds
    // to be removed when the problem is handled by system UI
    uint32_t delayMs = 0;
    if (usage == AUDIO_POLICY_FORCE_FOR_COMMUNICATION) {
        delayMs = TOUCH_SOUND_FIXED_DELAY_MS;
    }

    updateCallAndOutputRouting(forceVolumeReeval, delayMs);
    updateInputRouting();
}

void AudioPolicyManager::setSystemProperty(const char* property, const char* value)
{
    ALOGV("setSystemProperty() property %s, value %s", property, value);
}

// Find an MSD output profile compatible with the parameters passed.
// When "directOnly" is set, restrict search to profiles for direct outputs.
sp<IOProfile> AudioPolicyManager::getMsdProfileForOutput(
                                                   const DeviceVector& devices,
                                                   uint32_t samplingRate,
                                                   audio_format_t format,
                                                   audio_channel_mask_t channelMask,
                                                   audio_output_flags_t flags,
                                                   bool directOnly)
{
    flags = getRelevantFlags(flags, directOnly);

    sp<HwModule> msdModule = mHwModules.getModuleFromName(AUDIO_HARDWARE_MODULE_ID_MSD);
    if (msdModule != nullptr) {
        // for the msd module check if there are patches to the output devices
        if (msdHasPatchesToAllDevices(devices.toTypeAddrVector())) {
            HwModuleCollection modules;
            modules.add(msdModule);
            return searchCompatibleProfileHwModules(
                    modules, getMsdAudioOutDevices(), samplingRate, format, channelMask,
                    flags, directOnly);
        }
    }
    return nullptr;
}

// Find an output profile compatible with the parameters passed. When "directOnly" is set, restrict
// search to profiles for direct outputs.
sp<IOProfile> AudioPolicyManager::getProfileForOutput(
                                                   const DeviceVector& devices,
                                                   uint32_t samplingRate,
                                                   audio_format_t format,
                                                   audio_channel_mask_t channelMask,
                                                   audio_output_flags_t flags,
                                                   bool directOnly)
{
    flags = getRelevantFlags(flags, directOnly);

    return searchCompatibleProfileHwModules(
            mHwModules, devices, samplingRate, format, channelMask, flags, directOnly);
}

audio_output_flags_t AudioPolicyManager::getRelevantFlags (
                                            audio_output_flags_t flags, bool directOnly) {
    if (directOnly) {
         // only retain flags that will drive the direct output profile selection
         // if explicitly requested
         static const uint32_t kRelevantFlags =
                (AUDIO_OUTPUT_FLAG_HW_AV_SYNC | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD |
                AUDIO_OUTPUT_FLAG_VOIP_RX | AUDIO_OUTPUT_FLAG_MMAP_NOIRQ);
         flags = (audio_output_flags_t)((flags & kRelevantFlags) | AUDIO_OUTPUT_FLAG_DIRECT);
    }
    return flags;
}

sp<IOProfile> AudioPolicyManager::searchCompatibleProfileHwModules (
                                        const HwModuleCollection& hwModules,
                                        const DeviceVector& devices,
                                        uint32_t samplingRate,
                                        audio_format_t format,
                                        audio_channel_mask_t channelMask,
                                        audio_output_flags_t flags,
                                        bool directOnly) {
    sp<IOProfile> directOnlyProfile = nullptr;
    sp<IOProfile> compressOffloadProfile = nullptr;
    sp<IOProfile> profile = nullptr;
    for (const auto& hwModule : hwModules) {
        for (const auto& curProfile : hwModule->getOutputProfiles()) {
             if (curProfile->getCompatibilityScore(devices,
                     samplingRate, NULL /*updatedSamplingRate*/,
                     format, NULL /*updatedFormat*/,
                     channelMask, NULL /*updatedChannelMask*/,
                     flags) == IOProfile::NO_MATCH) {
                 continue;
             }
             // reject profiles not corresponding to a device currently available
             if (!mAvailableOutputDevices.containsAtLeastOne(curProfile->getSupportedDevices())) {
                 continue;
             }
             // reject profiles if connected device does not support codec
             if (!curProfile->devicesSupportEncodedFormats(devices.types())) {
                 continue;
             }
             if (!directOnly) {
                return curProfile;
             }

             profile = curProfile;
             if ((flags == AUDIO_OUTPUT_FLAG_DIRECT) &&
                 curProfile->getFlags() == AUDIO_OUTPUT_FLAG_DIRECT) {
                 directOnlyProfile = curProfile;
             }

             if ((curProfile->getFlags() & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
                 compressOffloadProfile = curProfile;
             }
        }
    }

    return directOnlyProfile ? directOnlyProfile
                            : (compressOffloadProfile ? compressOffloadProfile : profile);

}

sp<IOProfile> AudioPolicyManager::getSpatializerOutputProfile(
        const audio_config_t *config __unused, const AudioDeviceTypeAddrVector &devices) const
{
    for (const auto& hwModule : mHwModules) {
        for (const auto& curProfile : hwModule->getOutputProfiles()) {
            if (curProfile->getFlags() != AUDIO_OUTPUT_FLAG_SPATIALIZER) {
                continue;
            }
            if (!devices.empty()) {
                // reject profiles not corresponding to a device currently available
                DeviceVector supportedDevices = curProfile->getSupportedDevices();
                if (!mAvailableOutputDevices.containsAtLeastOne(supportedDevices)) {
                    continue;
                }
                if (supportedDevices.getDevicesFromDeviceTypeAddrVec(devices).size()
                        != devices.size()) {
                    continue;
                }
            }
            ALOGV("%s found profile %s", __func__, curProfile->getName().c_str());
            return curProfile;
        }
    }
    return nullptr;
}

audio_io_handle_t AudioPolicyManager::getOutput(audio_stream_type_t stream)
{
    DeviceVector devices = mEngine->getOutputDevicesForStream(stream, false /*fromCache*/);

    // Note that related method getOutputForAttr() uses getOutputForDevice() not selectOutput().
    // We use selectOutput() here since we don't have the desired AudioTrack sample rate,
    // format, flags, etc. This may result in some discrepancy for functions that utilize
    // getOutput() solely on audio_stream_type such as AudioSystem::getOutputFrameCount()
    // and AudioSystem::getOutputSamplingRate().

    SortedVector<audio_io_handle_t> outputs = getOutputsForDevices(devices, mOutputs);
    audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
    if (stream == AUDIO_STREAM_MUSIC && mConfig->useDeepBufferForMedia()) {
        flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
    }
    const audio_io_handle_t output = selectOutput(outputs, flags);

    ALOGV("getOutput() stream %d selected devices %s, output %d", stream,
          devices.toString().c_str(), output);
    return output;
}

status_t AudioPolicyManager::getAudioAttributes(audio_attributes_t *dstAttr,
                                                const audio_attributes_t *srcAttr,
                                                audio_stream_type_t srcStream)
{
    if (srcAttr != NULL) {
        if (!isValidAttributes(srcAttr)) {
            ALOGE("%s invalid attributes: usage=%d content=%d flags=0x%x tags=[%s]",
                    __func__,
                    srcAttr->usage, srcAttr->content_type, srcAttr->flags,
                    srcAttr->tags);
            return BAD_VALUE;
        }
        *dstAttr = *srcAttr;
    } else {
        if (srcStream < AUDIO_STREAM_MIN || srcStream >= AUDIO_STREAM_PUBLIC_CNT) {
            ALOGE("%s:  invalid stream type", __func__);
            return BAD_VALUE;
        }
        *dstAttr = mEngine->getAttributesForStreamType(srcStream);
    }

    // Only honor audibility enforced when required. The client will be
    // forced to reconnect if the forced usage changes.
    if (mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM) != AUDIO_POLICY_FORCE_SYSTEM_ENFORCED) {
        dstAttr->flags = static_cast<audio_flags_mask_t>(
                dstAttr->flags & ~AUDIO_FLAG_AUDIBILITY_ENFORCED);
    }

    return NO_ERROR;
}

status_t AudioPolicyManager::getOutputForAttrInt(
        audio_attributes_t *resultAttr,
        audio_io_handle_t *output,
        audio_session_t session,
        const audio_attributes_t *attr,
        audio_stream_type_t *stream,
        uid_t uid,
        audio_config_t *config,
        audio_output_flags_t *flags,
        DeviceIdVector *selectedDeviceIds,
        bool *isRequestedDeviceForExclusiveUse,
        std::vector<sp<AudioPolicyMix>> *secondaryMixes,
        output_type_t *outputType,
        bool *isSpatialized,
        bool *isBitPerfect)
{
    DeviceVector outputDevices;
    audio_port_handle_t requestedPortId = getFirstDeviceId(*selectedDeviceIds);
    selectedDeviceIds->clear();
    DeviceVector msdDevices = getMsdAudioOutDevices();
    const sp<DeviceDescriptor> requestedDevice =
        mAvailableOutputDevices.getDeviceFromId(requestedPortId);

    *outputType = API_OUTPUT_INVALID;
    *isSpatialized = false;

    status_t status = getAudioAttributes(resultAttr, attr, *stream);
    if (status != NO_ERROR) {
        return status;
    }
    if (auto it = mAllowedCapturePolicies.find(uid); it != end(mAllowedCapturePolicies)) {
        resultAttr->flags = static_cast<audio_flags_mask_t>(resultAttr->flags | it->second);
    }
    *stream = mEngine->getStreamTypeForAttributes(*resultAttr);

    ALOGV("%s() attributes=%s stream=%s session %d selectedDeviceId %d", __func__,
          toString(*resultAttr).c_str(), toString(*stream).c_str(), session, requestedPortId);

    bool usePrimaryOutputFromPolicyMixes = false;

    // The primary output is the explicit routing (eg. setPreferredDevice) if specified,
    //       otherwise, fallback to the dynamic policies, if none match, query the engine.
    // Secondary outputs are always found by dynamic policies as the engine do not support them
    sp<AudioPolicyMix> primaryMix;
    const audio_config_base_t clientConfig = {.sample_rate = config->sample_rate,
        .channel_mask = config->channel_mask,
        .format = config->format,
    };
    status = mPolicyMixes.getOutputForAttr(*resultAttr, clientConfig, uid, session, *flags,
                                           mAvailableOutputDevices, requestedDevice, primaryMix,
                                           secondaryMixes, usePrimaryOutputFromPolicyMixes);
    if (status != OK) {
        return status;
    }

    // FIXME: in case of RENDER policy, the output capabilities should be checked
    if ((secondaryMixes != nullptr && !secondaryMixes->empty())
            && (!audio_is_linear_pcm(config->format) ||
                    *flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)) {
        ALOGD("%s: rejecting request as secondary mixes only support pcm", __func__);
        return BAD_VALUE;
    }
    if (usePrimaryOutputFromPolicyMixes) {
        sp<DeviceDescriptor> policyMixDevice =
                mAvailableOutputDevices.getDevice(primaryMix->mDeviceType,
                                                  primaryMix->mDeviceAddress,
                                                  AUDIO_FORMAT_DEFAULT);
        sp<SwAudioOutputDescriptor> policyDesc = primaryMix->getOutput();
        bool tryDirectForFlags = policyDesc == nullptr ||
                (policyDesc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) ||
                (*flags & (AUDIO_OUTPUT_FLAG_HW_AV_SYNC | AUDIO_OUTPUT_FLAG_MMAP_NOIRQ));
        // if a direct output can be opened to deliver the track's multi-channel content to the
        // output rather than being downmixed by the primary output, then use this direct
        // output by by-passing the primary mix if possible, otherwise fall-through to primary
        // mix.
        bool tryDirectForChannelMask = policyDesc != nullptr
                    && (audio_channel_count_from_out_mask(policyDesc->getConfig().channel_mask) <
                        audio_channel_count_from_out_mask(config->channel_mask));
        if (policyMixDevice != nullptr && (tryDirectForFlags || tryDirectForChannelMask)) {
            audio_io_handle_t newOutput;
            status = openDirectOutput(
                    *stream, session, config,
                    (audio_output_flags_t)(*flags | AUDIO_OUTPUT_FLAG_DIRECT),
                    DeviceVector(policyMixDevice), &newOutput, *resultAttr);
            if (status == NO_ERROR) {
                policyDesc = mOutputs.valueFor(newOutput);
                primaryMix->setOutput(policyDesc);
            } else if (tryDirectForFlags) {
                ALOGW("%s, failed open direct, status: %d", __func__, status);
                policyDesc = nullptr;
            } // otherwise use primary if available.
        }
        if (policyDesc != nullptr) {
            policyDesc->mPolicyMix = primaryMix;
            *output = policyDesc->mIoHandle;
            if (policyMixDevice != nullptr) {
                selectedDeviceIds->push_back(policyMixDevice->getId());
            }
            if ((policyDesc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) != AUDIO_OUTPUT_FLAG_DIRECT) {
                // Remove direct flag as it is not on a direct output.
                *flags = (audio_output_flags_t) (*flags & ~AUDIO_OUTPUT_FLAG_DIRECT);
            }

            ALOGV("getOutputForAttr() returns output %d", *output);
            if (resultAttr->usage == AUDIO_USAGE_VIRTUAL_SOURCE) {
                *outputType = API_OUT_MIX_PLAYBACK;
            } else {
                *outputType = API_OUTPUT_LEGACY;
            }
            return NO_ERROR;
        } else {
            if (policyMixDevice != nullptr) {
                ALOGE("%s, try to use primary mix but no output found", __func__);
                return INVALID_OPERATION;
            }
            // Fallback to default engine selection as the selected primary mix device is not
            // available.
        }
    }
    // Virtual sources must always be dynamicaly or explicitly routed
    if (resultAttr->usage == AUDIO_USAGE_VIRTUAL_SOURCE) {
        ALOGW("getOutputForAttr() no policy mix found for usage AUDIO_USAGE_VIRTUAL_SOURCE");
        return BAD_VALUE;
    }
    // explicit routing managed by getDeviceForStrategy in APM is now handled by engine
    // in order to let the choice of the order to future vendor engine
    outputDevices = mEngine->getOutputDevicesForAttributes(*resultAttr, requestedDevice, false);

    if ((resultAttr->flags & AUDIO_FLAG_HW_AV_SYNC) != 0) {
        *flags = (audio_output_flags_t)(*flags | AUDIO_OUTPUT_FLAG_HW_AV_SYNC);
    }

    // Set incall music only if device was explicitly set, and fallback to the device which is
    // chosen by the engine if not.
    // FIXME: provide a more generic approach which is not device specific and move this back
    // to getOutputForDevice.
    // TODO: Remove check of AUDIO_STREAM_MUSIC once migration is completed on the app side.
    if (outputDevices.onlyContainsDevicesWithType(AUDIO_DEVICE_OUT_TELEPHONY_TX) &&
        (*stream == AUDIO_STREAM_MUSIC  || resultAttr->usage == AUDIO_USAGE_VOICE_COMMUNICATION) &&
        audio_is_linear_pcm(config->format) &&
        isCallAudioAccessible()) {
        if (requestedPortId != AUDIO_PORT_HANDLE_NONE) {
            *flags = (audio_output_flags_t)AUDIO_OUTPUT_FLAG_INCALL_MUSIC;
            *isRequestedDeviceForExclusiveUse = true;
        }
    }

    ALOGV("%s() device %s, sampling rate %d, format %#x, channel mask %#x, flags %#x stream %s",
          __func__, outputDevices.toString().c_str(), config->sample_rate, config->format,
          config->channel_mask, *flags, toString(*stream).c_str());

    *output = AUDIO_IO_HANDLE_NONE;
    if (!msdDevices.isEmpty()) {
        *output = getOutputForDevices(msdDevices, session, resultAttr, config, flags, isSpatialized);
        if (*output != AUDIO_IO_HANDLE_NONE && setMsdOutputPatches(&outputDevices) == NO_ERROR) {
            ALOGV("%s() Using MSD devices %s instead of devices %s",
                  __func__, msdDevices.toString().c_str(), outputDevices.toString().c_str());
        } else {
            *output = AUDIO_IO_HANDLE_NONE;
        }
    }
    if (*output == AUDIO_IO_HANDLE_NONE) {
        sp<PreferredMixerAttributesInfo> info = nullptr;
        if (outputDevices.size() == 1) {
            info = getPreferredMixerAttributesInfo(
                    outputDevices.itemAt(0)->getId(),
                    mEngine->getProductStrategyForAttributes(*resultAttr),
                    true /*activeBitPerfectPreferred*/);
            // Only use preferred mixer if the uid matches or the preferred mixer is bit-perfect
            // and it is currently active.
            if (info != nullptr && info->getUid() != uid &&
                (!info->isBitPerfect() || info->getActiveClientCount() == 0)) {
                info = nullptr;
            }

            if (info != nullptr && info->isBitPerfect() &&
                (*flags & (AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD |
                        AUDIO_OUTPUT_FLAG_HW_AV_SYNC | AUDIO_OUTPUT_FLAG_MMAP_NOIRQ)) != 0) {
                // Reject direct request if a preferred mixer config in use is bit-perfect.
                ALOGD("%s reject direct request as bit-perfect mixer attributes is active",
                      __func__);
                return BAD_VALUE;
            }

            if (com::android::media::audioserver::
                    fix_concurrent_playback_behavior_with_bit_perfect_client()) {
                if (info != nullptr && info->getUid() == uid &&
                    info->configMatches(*config) &&
                    (mEngine->getPhoneState() != AUDIO_MODE_NORMAL ||
                            std::any_of(gHighPriorityUseCases.begin(), gHighPriorityUseCases.end(),
                                        [this, &outputDevices](audio_usage_t usage) {
                                            return mOutputs.isUsageActiveOnDevice(
                                                    usage, outputDevices[0]); }))) {
                    // Bit-perfect request is not allowed when the phone mode is not normal or
                    // there is any higher priority user case active.
                    return INVALID_OPERATION;
                }
            }
        }
        *output = getOutputForDevices(outputDevices, session, resultAttr, config,
                flags, isSpatialized, info, resultAttr->flags & AUDIO_FLAG_MUTE_HAPTIC);
        // The client will be active if the client is currently preferred mixer owner and the
        // requested configuration matches the preferred mixer configuration.
        *isBitPerfect = (info != nullptr
                && info->isBitPerfect()
                && info->getUid() == uid
                && *output != AUDIO_IO_HANDLE_NONE
                // When bit-perfect output is selected for the preferred mixer attributes owner,
                // only need to consider the config matches.
                && mOutputs.valueFor(*output)->isConfigurationMatched(
                        clientConfig, AUDIO_OUTPUT_FLAG_NONE));

        if (*isBitPerfect) {
            *flags = (audio_output_flags_t)(*flags | AUDIO_OUTPUT_FLAG_BIT_PERFECT);
        }
    }
    if (*output == AUDIO_IO_HANDLE_NONE) {
        AudioProfileVector profiles;
        status_t ret = getProfilesForDevices(outputDevices, profiles, *flags, false /*isInput*/);
        if (ret == NO_ERROR && !profiles.empty()) {
            const auto channels = profiles[0]->getChannels();
            if (!channels.empty() && (channels.find(config->channel_mask) == channels.end())) {
                config->channel_mask = *channels.begin();
            }
            const auto sampleRates = profiles[0]->getSampleRates();
            if (!sampleRates.empty() &&
                    (sampleRates.find(config->sample_rate) == sampleRates.end())) {
                config->sample_rate = *sampleRates.begin();
            }
            config->format = profiles[0]->getFormat();
        }
        return INVALID_OPERATION;
    }

    for (auto &outputDevice : outputDevices) {
        if (std::find(selectedDeviceIds->begin(), selectedDeviceIds->end(),
                      outputDevice->getId()) == selectedDeviceIds->end()) {
            selectedDeviceIds->push_back(outputDevice->getId());
            if (outputDevice->getId() == mConfig->getDefaultOutputDevice()->getId()) {
                std::swap(selectedDeviceIds->front(), selectedDeviceIds->back());
            }
        }
    }

    if (outputDevices.onlyContainsDevicesWithType(AUDIO_DEVICE_OUT_TELEPHONY_TX)) {
        *outputType = API_OUTPUT_TELEPHONY_TX;
    } else {
        *outputType = API_OUTPUT_LEGACY;
    }

    ALOGV("%s returns output %d selectedDeviceIds %s", __func__, *output,
            toString(*selectedDeviceIds).c_str());

    return NO_ERROR;
}

status_t AudioPolicyManager::getOutputForAttr(const audio_attributes_t *attr,
                                              audio_io_handle_t *output,
                                              audio_session_t session,
                                              audio_stream_type_t *stream,
                                              const AttributionSourceState& attributionSource,
                                              audio_config_t *config,
                                              audio_output_flags_t *flags,
                                              DeviceIdVector *selectedDeviceIds,
                                              audio_port_handle_t *portId,
                                              std::vector<audio_io_handle_t> *secondaryOutputs,
                                              output_type_t *outputType,
                                              bool *isSpatialized,
                                              bool *isBitPerfect,
                                              float *volume,
                                              bool *muted)
{
    // The supplied portId must be AUDIO_PORT_HANDLE_NONE
    if (*portId != AUDIO_PORT_HANDLE_NONE) {
        return INVALID_OPERATION;
    }
    const uid_t uid = VALUE_OR_RETURN_STATUS(
        aidl2legacy_int32_t_uid_t(attributionSource.uid));
    audio_attributes_t resultAttr;
    bool isRequestedDeviceForExclusiveUse = false;
    std::vector<sp<AudioPolicyMix>> secondaryMixes;
    DeviceIdVector requestedDeviceIds = *selectedDeviceIds;

    // Prevent from storing invalid requested device id in clients
    DeviceIdVector sanitizedRequestedPortIds;
    for (auto deviceId : *selectedDeviceIds) {
        if (mAvailableOutputDevices.getDeviceFromId(deviceId) != nullptr) {
            sanitizedRequestedPortIds.push_back(deviceId);
        }
    }
    *selectedDeviceIds = sanitizedRequestedPortIds;

    status_t status = getOutputForAttrInt(&resultAttr, output, session, attr, stream, uid,
            config, flags, selectedDeviceIds, &isRequestedDeviceForExclusiveUse,
            secondaryOutputs != nullptr ? &secondaryMixes : nullptr, outputType, isSpatialized,
            isBitPerfect);
    if (status != NO_ERROR) {
        return status;
    }
    std::vector<wp<SwAudioOutputDescriptor>> weakSecondaryOutputDescs;
    if (secondaryOutputs != nullptr) {
        for (auto &secondaryMix : secondaryMixes) {
            sp<SwAudioOutputDescriptor> outputDesc = secondaryMix->getOutput();
            if (outputDesc != nullptr &&
                outputDesc->mIoHandle != AUDIO_IO_HANDLE_NONE &&
                outputDesc->mIoHandle != *output) {
                secondaryOutputs->push_back(outputDesc->mIoHandle);
                weakSecondaryOutputDescs.push_back(outputDesc);
            }
        }
    }

    audio_config_base_t clientConfig = {.sample_rate = config->sample_rate,
        .channel_mask = config->channel_mask,
        .format = config->format,
    };
    *portId = PolicyAudioPort::getNextUniqueId();

    sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueFor(*output);
    // TODO(b/367816690): Add device id sets to TrackClientDescriptor
    sp<TrackClientDescriptor> clientDesc =
        new TrackClientDescriptor(*portId, uid, session, resultAttr, clientConfig,
                                  getFirstDeviceId(sanitizedRequestedPortIds), *stream,
                                  mEngine->getProductStrategyForAttributes(resultAttr),
                                  toVolumeSource(resultAttr),
                                  *flags, isRequestedDeviceForExclusiveUse,
                                  std::move(weakSecondaryOutputDescs),
                                  outputDesc->mPolicyMix);
    outputDesc->addClient(clientDesc);

    *volume = Volume::DbToAmpl(outputDesc->getCurVolume(toVolumeSource(resultAttr)));
    *muted = outputDesc->isMutedByGroup(toVolumeSource(resultAttr));

    ALOGV("%s() returns output %d requestedPortIds %s selectedDeviceIds %s for port ID %d",
          __func__, *output, toString(requestedDeviceIds).c_str(),
          toString(*selectedDeviceIds).c_str(), *portId);

    return NO_ERROR;
}

status_t AudioPolicyManager::openDirectOutput(audio_stream_type_t stream,
                                              audio_session_t session,
                                              const audio_config_t *config,
                                              audio_output_flags_t flags,
                                              const DeviceVector &devices,
                                              audio_io_handle_t *output,
                                              audio_attributes_t attributes) {

    *output = AUDIO_IO_HANDLE_NONE;

    // skip direct output selection if the request can obviously be attached to a mixed output
    // and not explicitly requested
    if (((flags & AUDIO_OUTPUT_FLAG_DIRECT) == 0) &&
            audio_is_linear_pcm(config->format) && config->sample_rate <= SAMPLE_RATE_HZ_MAX &&
            audio_channel_count_from_out_mask(config->channel_mask) <= 2) {
        return NAME_NOT_FOUND;
    }

    // Reject flag combinations that do not make sense. Note that the requested flags might not
    // have the 'DIRECT' flag set, however once a direct-capable profile is found, it will
    // combine the requested flags with its own flags, yielding an unsupported combination.
    if ((flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) != 0) {
        return NAME_NOT_FOUND;
    }

    // Do not allow offloading if one non offloadable effect is enabled or MasterMono is enabled.
    // This prevents creating an offloaded track and tearing it down immediately after start
    // when audioflinger detects there is an active non offloadable effect.
    // FIXME: We should check the audio session here but we do not have it in this context.
    // This may prevent offloading in rare situations where effects are left active by apps
    // in the background.
    sp<IOProfile> profile;
    if (((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) == 0) ||
            !(mEffects.isNonOffloadableEffectEnabled() || mMasterMono)) {
        profile = getProfileForOutput(
                devices, config->sample_rate, config->format, config->channel_mask,
                flags, true /* directOnly */);
    }

    if (profile == nullptr) {
        return NAME_NOT_FOUND;
    }

    // exclusive outputs for MMAP and Offload are enforced by different session ids.
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        if (!desc->isDuplicated() && (profile == desc->mProfile)) {
            // reuse direct output if currently open by the same client
            // and configured with same parameters
            if ((config->sample_rate == desc->getSamplingRate()) &&
                (config->format == desc->getFormat()) &&
                (config->channel_mask == desc->getChannelMask()) &&
                (session == desc->mDirectClientSession)) {
                desc->mDirectOpenCount++;
                ALOGI("%s reusing direct output %d for session %d", __func__,
                    mOutputs.keyAt(i), session);
                *output = mOutputs.keyAt(i);
                return NO_ERROR;
            }
        }
    }

    if (!profile->canOpenNewIo()) {
        if ((profile->getFlags() & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ) != 0) {
            // MMAP gracefully handles lack of an exclusive track resource by mixing
            // above the audio framework. For AAudio to know that the limit is reached,
            // return an error.
            ALOGW("%s profile %s can't open new mmap output maxOpenCount reached", __func__,
                  profile->getName().c_str());
            return NAME_NOT_FOUND;
        } else {
            // Close outputs on this profile, if available, to free resources for this request
            for (int i = 0; i < mOutputs.size() && !profile->canOpenNewIo(); i++) {
                const auto desc = mOutputs.valueAt(i);
                if (desc->mProfile == profile) {
                    ALOGV("%s closeOutput %d to prioritize session %d on profile %s", __func__,
                          desc->mIoHandle, session, profile->getName().c_str());
                    closeOutput(desc->mIoHandle);
                }
            }
        }
    }

    // Unable to close streams to find free resources for this request
    if (!profile->canOpenNewIo()) {
        ALOGW("%s profile %s can't open new output maxOpenCount reached", __func__,
              profile->getName().c_str());
        return NAME_NOT_FOUND;
    }

    auto outputDesc = sp<SwAudioOutputDescriptor>::make(profile, mpClientInterface);

    // An MSD patch may be using the only output stream that can service this request. Release
    // all MSD patches to prioritize this request over any active output on MSD.
    releaseMsdOutputPatches(devices);

    status_t status =
            outputDesc->open(config, nullptr /* mixerConfig */, devices, stream, &flags, output,
                             attributes);

    // only accept an output with the requested parameters, unless the format can be IEC61937
    // encapsulated and opened by AudioFlinger as wrapped IEC61937.
    const bool ignoreRequestedParametersCheck = audio_is_iec61937_compatible(config->format)
            && (flags & AUDIO_OUTPUT_FLAG_IEC958_NONAUDIO)
            && audio_has_proportional_frames(outputDesc->getFormat());
    if (status != NO_ERROR ||
        (!ignoreRequestedParametersCheck &&
        ((config->sample_rate != 0 && config->sample_rate != outputDesc->getSamplingRate()) ||
         (config->format != AUDIO_FORMAT_DEFAULT && config->format != outputDesc->getFormat()) ||
         (config->channel_mask != 0 && config->channel_mask != outputDesc->getChannelMask())))) {
        ALOGV("%s failed opening direct output: output %d sample rate %d %d,"
                "format %d %d, channel mask %04x %04x", __func__, *output, config->sample_rate,
                outputDesc->getSamplingRate(), config->format, outputDesc->getFormat(),
                config->channel_mask, outputDesc->getChannelMask());
        if (*output != AUDIO_IO_HANDLE_NONE) {
            outputDesc->close();
        }
        // fall back to mixer output if possible when the direct output could not be open
        if (audio_is_linear_pcm(config->format) &&
                config->sample_rate  <= SAMPLE_RATE_HZ_MAX) {
            return NAME_NOT_FOUND;
        }
        *output = AUDIO_IO_HANDLE_NONE;
        return BAD_VALUE;
    }
    outputDesc->mDirectOpenCount = 1;
    outputDesc->mDirectClientSession = session;

    addOutput(*output, outputDesc);
    // The version check is essentially to avoid making this call in the case of the HIDL HAL.
    if (auto hwModule = mHwModules.getModuleFromHandle(mPrimaryModuleHandle); hwModule &&
            hwModule->getHalVersionMajor() >= 3) {
        setOutputDevices(__func__, outputDesc, devices, true, 0, NULL);
    }
    mPreviousOutputs = mOutputs;
    ALOGV("%s returns new direct output %d", __func__, *output);
    mpClientInterface->onAudioPortListUpdate();
    return NO_ERROR;
}

audio_io_handle_t AudioPolicyManager::getOutputForDevices(
        const DeviceVector &devices,
        audio_session_t session,
        const audio_attributes_t *attr,
        const audio_config_t *config,
        audio_output_flags_t *flags,
        bool *isSpatialized,
        sp<PreferredMixerAttributesInfo> prefMixerConfigInfo,
        bool forceMutingHaptic)
{
    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;

    // Discard haptic channel mask when forcing muting haptic channels.
    audio_channel_mask_t channelMask = forceMutingHaptic
            ? static_cast<audio_channel_mask_t>(config->channel_mask & ~AUDIO_CHANNEL_HAPTIC_ALL)
            : config->channel_mask;

    // open a direct output if required by specified parameters
    //force direct flag if offload flag is set: offloading implies a direct output stream
    // and all common behaviors are driven by checking only the direct flag
    // this should normally be set appropriately in the policy configuration file
    if ((*flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
        *flags = (audio_output_flags_t)(*flags | AUDIO_OUTPUT_FLAG_DIRECT);
    }
    if ((*flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) != 0) {
        *flags = (audio_output_flags_t)(*flags | AUDIO_OUTPUT_FLAG_DIRECT);
    }

    audio_stream_type_t stream = mEngine->getStreamTypeForAttributes(*attr);

    // only allow deep buffering for music stream type
    if (stream != AUDIO_STREAM_MUSIC) {
        *flags = (audio_output_flags_t)(*flags &~AUDIO_OUTPUT_FLAG_DEEP_BUFFER);
    } else if (/* stream == AUDIO_STREAM_MUSIC && */
            *flags == AUDIO_OUTPUT_FLAG_NONE && mConfig->useDeepBufferForMedia()) {
        // use DEEP_BUFFER as default output for music stream type
        *flags = (audio_output_flags_t)AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
    }
    if (stream == AUDIO_STREAM_TTS) {
        *flags = AUDIO_OUTPUT_FLAG_TTS;
    } else if (stream == AUDIO_STREAM_VOICE_CALL &&
               audio_is_linear_pcm(config->format) &&
               (*flags & AUDIO_OUTPUT_FLAG_INCALL_MUSIC) == 0) {
        *flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_VOIP_RX |
                                       AUDIO_OUTPUT_FLAG_DIRECT);
        ALOGV("Set VoIP and Direct output flags for PCM format");
    }

    // Attach the Ultrasound flag for the AUDIO_CONTENT_TYPE_ULTRASOUND
    if (attr->content_type == AUDIO_CONTENT_TYPE_ULTRASOUND) {
        *flags = (audio_output_flags_t)(*flags | AUDIO_OUTPUT_FLAG_ULTRASOUND);
    }

    // Use the spatializer output if the content can be spatialized, no preferred mixer
    // was specified and offload or direct playback is not explicitly requested, and there is no
    // haptic channel included in playback
    *isSpatialized = false;
    if (mSpatializerOutput != nullptr &&
        canBeSpatializedInt(attr, config, devices.toTypeAddrVector()) &&
        prefMixerConfigInfo == nullptr &&
        ((*flags & (AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD | AUDIO_OUTPUT_FLAG_DIRECT)) == 0) &&
        checkHapticCompatibilityOnSpatializerOutput(config, session)) {
        *isSpatialized = true;
        return mSpatializerOutput->mIoHandle;
    }

    audio_config_t directConfig = *config;
    directConfig.channel_mask = channelMask;

    status_t status = openDirectOutput(stream, session, &directConfig, *flags, devices, &output,
                                       *attr);
    if (status != NAME_NOT_FOUND) {
        return output;
    }

    // A request for HW A/V sync cannot fallback to a mixed output because time
    // stamps are embedded in audio data
    if ((*flags & (AUDIO_OUTPUT_FLAG_HW_AV_SYNC | AUDIO_OUTPUT_FLAG_MMAP_NOIRQ)) != 0) {
        return AUDIO_IO_HANDLE_NONE;
    }
    // A request for Tuner cannot fallback to a mixed output
    if ((directConfig.offload_info.content_id || directConfig.offload_info.sync_id)) {
        return AUDIO_IO_HANDLE_NONE;
    }

    // ignoring channel mask due to downmix capability in mixer

    // open a non direct output

    // for non direct outputs, only PCM is supported
    if (audio_is_linear_pcm(config->format)) {
        // get which output is suitable for the specified stream. The actual
        // routing change will happen when startOutput() will be called
        SortedVector<audio_io_handle_t> outputs = getOutputsForDevices(devices, mOutputs);
        if (prefMixerConfigInfo != nullptr) {
            for (audio_io_handle_t outputHandle : outputs) {
                sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueFor(outputHandle);
                if (outputDesc->mProfile == prefMixerConfigInfo->getProfile()) {
                    output = outputHandle;
                    break;
                }
            }
            if (output == AUDIO_IO_HANDLE_NONE) {
                // No output open with the preferred profile. Open a new one.
                audio_config_t config = AUDIO_CONFIG_INITIALIZER;
                config.channel_mask = prefMixerConfigInfo->getConfigBase().channel_mask;
                config.sample_rate = prefMixerConfigInfo->getConfigBase().sample_rate;
                config.format = prefMixerConfigInfo->getConfigBase().format;
                sp<SwAudioOutputDescriptor> preferredOutput = openOutputWithProfileAndDevice(
                        prefMixerConfigInfo->getProfile(), devices, nullptr /*mixerConfig*/,
                        &config, prefMixerConfigInfo->getFlags());
                if (preferredOutput == nullptr) {
                    ALOGE("%s failed to open output with preferred mixer config", __func__);
                } else {
                    output = preferredOutput->mIoHandle;
                }
            }
        } else {
            // at this stage we should ignore the DIRECT flag as no direct output could be
            // found earlier
            *flags = (audio_output_flags_t) (*flags & ~AUDIO_OUTPUT_FLAG_DIRECT);
            if (com::android::media::audioserver::
                    fix_concurrent_playback_behavior_with_bit_perfect_client()) {
                // If the preferred mixer attributes is null, do not select the bit-perfect output
                // unless the bit-perfect output is the only output.
                // The bit-perfect output can exist while the passed in preferred mixer attributes
                // info is null when it is a high priority client. The high priority clients are
                // ringtone or alarm, which is not a bit-perfect use case.
                size_t i = 0;
                while (i < outputs.size() && outputs.size() > 1) {
                    auto desc = mOutputs.valueFor(outputs[i]);
                    // The output descriptor must not be null here.
                    if (desc->isBitPerfect()) {
                        outputs.removeItemsAt(i);
                    } else {
                        i += 1;
                    }
                }
            }
            output = selectOutput(
                    outputs, *flags, config->format, channelMask, config->sample_rate, session);
        }
    }
    ALOGW_IF((output == 0), "getOutputForDevices() could not find output for stream %d, "
            "sampling rate %d, format %#x, channels %#x, flags %#x",
            stream, config->sample_rate, config->format, channelMask, *flags);

    return output;
}

sp<DeviceDescriptor> AudioPolicyManager::getMsdAudioInDevice() const {
    auto msdInDevices = mHwModules.getAvailableDevicesFromModuleName(AUDIO_HARDWARE_MODULE_ID_MSD,
                                                                     mAvailableInputDevices);
    return msdInDevices.isEmpty()? nullptr : msdInDevices.itemAt(0);
}

DeviceVector AudioPolicyManager::getMsdAudioOutDevices() const {
    return mHwModules.getAvailableDevicesFromModuleName(AUDIO_HARDWARE_MODULE_ID_MSD,
                                                        mAvailableOutputDevices);
}

const AudioPatchCollection AudioPolicyManager::getMsdOutputPatches() const {
    AudioPatchCollection msdPatches;
    sp<HwModule> msdModule = mHwModules.getModuleFromName(AUDIO_HARDWARE_MODULE_ID_MSD);
    if (msdModule != 0) {
        for (size_t i = 0; i < mAudioPatches.size(); ++i) {
            sp<AudioPatch> patch = mAudioPatches.valueAt(i);
            for (size_t j = 0; j < patch->mPatch.num_sources; ++j) {
                const struct audio_port_config *source = &patch->mPatch.sources[j];
                if (source->type == AUDIO_PORT_TYPE_DEVICE &&
                        source->ext.device.hw_module == msdModule->getHandle()) {
                    msdPatches.addAudioPatch(patch->getHandle(), patch);
                }
            }
        }
    }
    return msdPatches;
}

bool AudioPolicyManager::isMsdPatch(const audio_patch_handle_t &handle) const {
    ssize_t index = mAudioPatches.indexOfKey(handle);
    if (index < 0) {
        return false;
    }
    const sp<AudioPatch> patch = mAudioPatches.valueAt(index);
    sp<HwModule> msdModule = mHwModules.getModuleFromName(AUDIO_HARDWARE_MODULE_ID_MSD);
    if (msdModule == nullptr) {
        return false;
    }
    const struct audio_port_config *sink = &patch->mPatch.sinks[0];
    if (getMsdAudioOutDevices().contains(mAvailableOutputDevices.getDeviceFromId(sink->id))) {
        return true;
    }
    index = getMsdOutputPatches().indexOfKey(handle);
    if (index < 0) {
        return false;
    }
    return true;
}

status_t AudioPolicyManager::getMsdProfiles(bool hwAvSync,
                                            const InputProfileCollection &inputProfiles,
                                            const OutputProfileCollection &outputProfiles,
                                            const sp<DeviceDescriptor> &sourceDevice,
                                            const sp<DeviceDescriptor> &sinkDevice,
                                            AudioProfileVector& sourceProfiles,
                                            AudioProfileVector& sinkProfiles) const {
    if (inputProfiles.isEmpty()) {
        ALOGE("%s() no input profiles for source module", __func__);
        return NO_INIT;
    }
    if (outputProfiles.isEmpty()) {
        ALOGE("%s() no output profiles for sink module", __func__);
        return NO_INIT;
    }
    for (const auto &inProfile : inputProfiles) {
        if (hwAvSync == ((inProfile->getFlags() & AUDIO_INPUT_FLAG_HW_AV_SYNC) != 0) &&
                inProfile->supportsDevice(sourceDevice)) {
            appendAudioProfiles(sourceProfiles, inProfile->getAudioProfiles());
        }
    }
    for (const auto &outProfile : outputProfiles) {
        if (hwAvSync == ((outProfile->getFlags() & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) != 0) &&
                outProfile->supportsDevice(sinkDevice)) {
            appendAudioProfiles(sinkProfiles, outProfile->getAudioProfiles());
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::getBestMsdConfig(bool hwAvSync,
        const AudioProfileVector &sourceProfiles, const AudioProfileVector &sinkProfiles,
        audio_port_config *sourceConfig, audio_port_config *sinkConfig) const
{
    // Compressed formats for MSD module, ordered from most preferred to least preferred.
    static const std::vector<audio_format_t> formatsOrder = {{
            AUDIO_FORMAT_IEC60958, AUDIO_FORMAT_MAT_2_1, AUDIO_FORMAT_MAT_2_0, AUDIO_FORMAT_E_AC3,
            AUDIO_FORMAT_AC3, AUDIO_FORMAT_PCM_FLOAT, AUDIO_FORMAT_PCM_32_BIT,
            AUDIO_FORMAT_PCM_8_24_BIT, AUDIO_FORMAT_PCM_24_BIT_PACKED, AUDIO_FORMAT_PCM_16_BIT }};
    static const std::vector<audio_channel_mask_t> channelMasksOrder = [](){
        // Channel position masks for MSD module, 3D > 2D > 1D ordering (most preferred to least
        // preferred).
        std::vector<audio_channel_mask_t> masks = {{
            AUDIO_CHANNEL_OUT_3POINT1POINT2, AUDIO_CHANNEL_OUT_3POINT0POINT2,
            AUDIO_CHANNEL_OUT_2POINT1POINT2, AUDIO_CHANNEL_OUT_2POINT0POINT2,
            AUDIO_CHANNEL_OUT_5POINT1, AUDIO_CHANNEL_OUT_STEREO }};
        // insert index masks (higher counts most preferred) as preferred over position masks
        for (int i = 1; i <= AUDIO_CHANNEL_COUNT_MAX; i++) {
            masks.insert(
                    masks.begin(), audio_channel_mask_for_index_assignment_from_count(i));
        }
        return masks;
    }();

    struct audio_config_base bestSinkConfig;
    status_t result = findBestMatchingOutputConfig(sourceProfiles, sinkProfiles, formatsOrder,
            channelMasksOrder, true /*preferHigherSamplingRates*/, bestSinkConfig);
    if (result != NO_ERROR) {
        ALOGD("%s() no matching config found for sink, hwAvSync: %d",
                __func__, hwAvSync);
        return result;
    }
    sinkConfig->sample_rate = bestSinkConfig.sample_rate;
    sinkConfig->channel_mask = bestSinkConfig.channel_mask;
    sinkConfig->format = bestSinkConfig.format;
    // For encoded streams force direct flag to prevent downstream mixing.
    sinkConfig->flags.output = static_cast<audio_output_flags_t>(
            sinkConfig->flags.output | AUDIO_OUTPUT_FLAG_DIRECT);
    if (audio_is_iec61937_compatible(sinkConfig->format)) {
        // For formats compatible with IEC61937 encapsulation, assume that
        // the input is IEC61937 framed (for proportional buffer sizing).
        // Add the AUDIO_OUTPUT_FLAG_IEC958_NONAUDIO flag so downstream HAL can distinguish between
        // raw and IEC61937 framed streams.
        sinkConfig->flags.output = static_cast<audio_output_flags_t>(
                sinkConfig->flags.output | AUDIO_OUTPUT_FLAG_IEC958_NONAUDIO);
    }
    sourceConfig->sample_rate = bestSinkConfig.sample_rate;
    // Specify exact channel mask to prevent guessing by bit count in PatchPanel.
    sourceConfig->channel_mask =
            audio_channel_mask_get_representation(bestSinkConfig.channel_mask)
            == AUDIO_CHANNEL_REPRESENTATION_INDEX ?
            bestSinkConfig.channel_mask : audio_channel_mask_out_to_in(bestSinkConfig.channel_mask);
    sourceConfig->format = bestSinkConfig.format;
    // Copy input stream directly without any processing (e.g. resampling).
    sourceConfig->flags.input = static_cast<audio_input_flags_t>(
            sourceConfig->flags.input | AUDIO_INPUT_FLAG_DIRECT);
    if (hwAvSync) {
        sinkConfig->flags.output = static_cast<audio_output_flags_t>(
                sinkConfig->flags.output | AUDIO_OUTPUT_FLAG_HW_AV_SYNC);
        sourceConfig->flags.input = static_cast<audio_input_flags_t>(
                sourceConfig->flags.input | AUDIO_INPUT_FLAG_HW_AV_SYNC);
    }
    const unsigned int config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE |
            AUDIO_PORT_CONFIG_CHANNEL_MASK | AUDIO_PORT_CONFIG_FORMAT | AUDIO_PORT_CONFIG_FLAGS;
    sinkConfig->config_mask |= config_mask;
    sourceConfig->config_mask |= config_mask;
    return NO_ERROR;
}

PatchBuilder AudioPolicyManager::buildMsdPatch(bool msdIsSource,
                                               const sp<DeviceDescriptor> &device) const
{
    PatchBuilder patchBuilder;
    sp<HwModule> msdModule = mHwModules.getModuleFromName(AUDIO_HARDWARE_MODULE_ID_MSD);
    ALOG_ASSERT(msdModule != nullptr, "MSD module not available");
    sp<HwModule> deviceModule = mHwModules.getModuleForDevice(device, AUDIO_FORMAT_DEFAULT);
    if (deviceModule == nullptr) {
        ALOGE("%s() unable to get module for %s", __func__, device->toString().c_str());
        return patchBuilder;
    }
    const InputProfileCollection inputProfiles = msdIsSource ?
            msdModule->getInputProfiles() : deviceModule->getInputProfiles();
    const OutputProfileCollection outputProfiles = msdIsSource ?
            deviceModule->getOutputProfiles() : msdModule->getOutputProfiles();

    const sp<DeviceDescriptor> sourceDevice = msdIsSource ? getMsdAudioInDevice() : device;
    const sp<DeviceDescriptor> sinkDevice = msdIsSource ?
            device : getMsdAudioOutDevices().itemAt(0);
    patchBuilder.addSource(sourceDevice).addSink(sinkDevice);

    audio_port_config sourceConfig = patchBuilder.patch()->sources[0];
    audio_port_config sinkConfig = patchBuilder.patch()->sinks[0];
    AudioProfileVector sourceProfiles;
    AudioProfileVector sinkProfiles;
    // TODO: Figure out whether MSD module has HW_AV_SYNC flag set in the AP config file.
    // For now, we just forcefully try with HwAvSync first.
    for (auto hwAvSync : { true, false }) {
        if (getMsdProfiles(hwAvSync, inputProfiles, outputProfiles, sourceDevice, sinkDevice,
                sourceProfiles, sinkProfiles) != NO_ERROR) {
            continue;
        }
        if (getBestMsdConfig(hwAvSync, sourceProfiles, sinkProfiles, &sourceConfig,
                &sinkConfig) == NO_ERROR) {
            // Found a matching config. Re-create PatchBuilder with this config.
            return (PatchBuilder()).addSource(sourceConfig).addSink(sinkConfig);
        }
    }
    ALOGV("%s() no matching config found. Fall through to default PCM patch"
            " supporting PCM format conversion.", __func__);
    return patchBuilder;
}

status_t AudioPolicyManager::setMsdOutputPatches(const DeviceVector *outputDevices) {
    DeviceVector devices;
    if (outputDevices != nullptr && outputDevices->size() > 0) {
        devices.add(*outputDevices);
    } else {
        // Use media strategy for unspecified output device. This should only
        // occur on checkForDeviceAndOutputChanges(). Device connection events may
        // therefore invalidate explicit routing requests.
        devices = mEngine->getOutputDevicesForAttributes(
                    attributes_initializer(AUDIO_USAGE_MEDIA), nullptr, false /*fromCache*/);
        LOG_ALWAYS_FATAL_IF(devices.isEmpty(), "no output device to set MSD patch");
    }
    std::vector<PatchBuilder> patchesToCreate;
    for (auto i = 0u; i < devices.size(); ++i) {
        ALOGV("%s() for device %s", __func__, devices[i]->toString().c_str());
        patchesToCreate.push_back(buildMsdPatch(true /*msdIsSource*/, devices[i]));
    }
    // Retain only the MSD patches associated with outputDevices request.
    // Tear down the others, and create new ones as needed.
    AudioPatchCollection patchesToRemove = getMsdOutputPatches();
    for (auto it = patchesToCreate.begin(); it != patchesToCreate.end(); ) {
        auto retainedPatch = false;
        for (auto i = 0u; i < patchesToRemove.size(); ++i) {
            if (audio_patches_are_equal(it->patch(), &patchesToRemove[i]->mPatch)) {
                patchesToRemove.removeItemsAt(i);
                retainedPatch = true;
                break;
            }
        }
        if (retainedPatch) {
            it = patchesToCreate.erase(it);
            continue;
        }
        ++it;
    }
    if (patchesToCreate.size() == 0 && patchesToRemove.size() == 0) {
        return NO_ERROR;
    }
    for (auto i = 0u; i < patchesToRemove.size(); ++i) {
        auto &currentPatch = patchesToRemove.valueAt(i);
        releaseAudioPatch(currentPatch->getHandle(), mUidCached);
    }
    status_t status = NO_ERROR;
    for (const auto &p : patchesToCreate) {
        auto currStatus = installPatch(__func__, -1 /*index*/, nullptr /*patchHandle*/,
                p.patch(), 0 /*delayMs*/, mUidCached, nullptr /*patchDescPtr*/);
        char message[256];
        snprintf(message, sizeof(message), "%s() %s: creating MSD patch from device:IN_BUS to "
            "device:%#x (format:%#x channels:%#x samplerate:%d)", __func__,
                currStatus == NO_ERROR ? "Success" : "Error",
                p.patch()->sinks[0].ext.device.type, p.patch()->sources[0].format,
                p.patch()->sources[0].channel_mask, p.patch()->sources[0].sample_rate);
        if (currStatus == NO_ERROR) {
            ALOGD("%s", message);
        } else {
            ALOGE("%s", message);
            if (status == NO_ERROR) {
                status = currStatus;
            }
        }
    }
    return status;
}

void AudioPolicyManager::releaseMsdOutputPatches(const DeviceVector& devices) {
    AudioPatchCollection msdPatches = getMsdOutputPatches();
    for (size_t i = 0; i < msdPatches.size(); i++) {
        const auto& patch = msdPatches[i];
        for (size_t j = 0; j < patch->mPatch.num_sinks; ++j) {
            const struct audio_port_config *sink = &patch->mPatch.sinks[j];
            if (sink->type == AUDIO_PORT_TYPE_DEVICE && devices.getDevice(sink->ext.device.type,
                    String8(sink->ext.device.address), AUDIO_FORMAT_DEFAULT) != nullptr) {
                releaseAudioPatch(patch->getHandle(), mUidCached);
                break;
            }
        }
    }
}

bool AudioPolicyManager::msdHasPatchesToAllDevices(const AudioDeviceTypeAddrVector& devices) {
    DeviceVector devicesToCheck =
            mConfig->getOutputDevices().getDevicesFromDeviceTypeAddrVec(devices);
    AudioPatchCollection msdPatches = getMsdOutputPatches();
    for (size_t i = 0; i < msdPatches.size(); i++) {
        const auto& patch = msdPatches[i];
        for (size_t j = 0; j < patch->mPatch.num_sinks; ++j) {
            const struct audio_port_config *sink = &patch->mPatch.sinks[j];
            if (sink->type == AUDIO_PORT_TYPE_DEVICE) {
                const auto& foundDevice = devicesToCheck.getDevice(
                    sink->ext.device.type, String8(sink->ext.device.address), AUDIO_FORMAT_DEFAULT);
                if (foundDevice != nullptr) {
                    devicesToCheck.remove(foundDevice);
                    if (devicesToCheck.isEmpty()) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

audio_io_handle_t AudioPolicyManager::selectOutput(const SortedVector<audio_io_handle_t>& outputs,
                                                   audio_output_flags_t flags,
                                                   audio_format_t format,
                                                   audio_channel_mask_t channelMask,
                                                   uint32_t samplingRate,
                                                   audio_session_t sessionId)
{
    LOG_ALWAYS_FATAL_IF(!(format == AUDIO_FORMAT_INVALID || audio_is_linear_pcm(format)),
        "%s called with format %#x", __func__, format);

    // Return the output that haptic-generating attached to when 1) session id is specified,
    // 2) haptic-generating effect exists for given session id and 3) the output that
    // haptic-generating effect attached to is in given outputs.
    if (sessionId != AUDIO_SESSION_NONE) {
        audio_io_handle_t hapticGeneratingOutput = mEffects.getIoForSession(
                sessionId, FX_IID_HAPTICGENERATOR);
        if (outputs.indexOf(hapticGeneratingOutput) >= 0) {
            return hapticGeneratingOutput;
        }
    }

    // Flags disqualifying an output: the match must happen before calling selectOutput()
    static const audio_output_flags_t kExcludedFlags = (audio_output_flags_t)
        (AUDIO_OUTPUT_FLAG_HW_AV_SYNC | AUDIO_OUTPUT_FLAG_MMAP_NOIRQ | AUDIO_OUTPUT_FLAG_DIRECT);

    // Flags expressing a functional request: must be honored in priority over
    // other criteria
    static const audio_output_flags_t kFunctionalFlags = (audio_output_flags_t)
        (AUDIO_OUTPUT_FLAG_VOIP_RX | AUDIO_OUTPUT_FLAG_INCALL_MUSIC |
            AUDIO_OUTPUT_FLAG_TTS | AUDIO_OUTPUT_FLAG_DIRECT_PCM | AUDIO_OUTPUT_FLAG_ULTRASOUND |
            AUDIO_OUTPUT_FLAG_SPATIALIZER);
    // Flags expressing a performance request: have lower priority than serving
    // requested sampling rate or channel mask
    static const audio_output_flags_t kPerformanceFlags = (audio_output_flags_t)
        (AUDIO_OUTPUT_FLAG_FAST | AUDIO_OUTPUT_FLAG_DEEP_BUFFER |
            AUDIO_OUTPUT_FLAG_RAW | AUDIO_OUTPUT_FLAG_SYNC);

    const audio_output_flags_t functionalFlags =
        (audio_output_flags_t)(flags & kFunctionalFlags);
    const audio_output_flags_t performanceFlags =
        (audio_output_flags_t)(flags & kPerformanceFlags);

    audio_io_handle_t bestOutput = (outputs.size() == 0) ? AUDIO_IO_HANDLE_NONE : outputs[0];

    // select one output among several that provide a path to a particular device or set of
    // devices (the list was previously build by getOutputsForDevices()).
    // The priority is as follows:
    // 1: the output supporting haptic playback when requesting haptic playback
    // 2: the output with the highest number of requested functional flags
    //    with tiebreak preferring the minimum number of extra functional flags
    //    (see b/200293124, the incorrect selection of AUDIO_OUTPUT_FLAG_VOIP_RX).
    // 3: the output supporting the exact channel mask
    // 4: the output with a higher channel count than requested
    // 5: the output with the highest sampling rate if the requested sample rate is
    //    greater than default sampling rate
    // 6: the output with the highest number of requested performance flags
    // 7: the output with the bit depth the closest to the requested one
    // 8: the primary output
    // 9: the first output in the list

    // matching criteria values in priority order for best matching output so far
    std::vector<uint32_t> bestMatchCriteria(8, 0);

    const bool hasOrphanHaptic = mEffects.hasOrphansForSession(sessionId, FX_IID_HAPTICGENERATOR);
    const uint32_t channelCount = audio_channel_count_from_out_mask(channelMask);
    const uint32_t hapticChannelCount = audio_channel_count_from_out_mask(
        channelMask & AUDIO_CHANNEL_HAPTIC_ALL);

    for (audio_io_handle_t output : outputs) {
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
        // matching criteria values in priority order for current output
        std::vector<uint32_t> currentMatchCriteria(8, 0);

        if (outputDesc->isDuplicated()) {
            continue;
        }
        if ((kExcludedFlags & outputDesc->mFlags) != 0) {
            continue;
        }

        // If haptic channel is specified, use the haptic output if present.
        // When using haptic output, same audio format and sample rate are required.
        const uint32_t outputHapticChannelCount = audio_channel_count_from_out_mask(
            outputDesc->getChannelMask() & AUDIO_CHANNEL_HAPTIC_ALL);
        // skip if haptic channel specified but output does not support it, or output support haptic
        // but there is no haptic channel requested AND no orphan haptic effect exist
        if ((hapticChannelCount != 0 && outputHapticChannelCount == 0) ||
            (hapticChannelCount == 0 && outputHapticChannelCount != 0 && !hasOrphanHaptic)) {
            continue;
        }
        // In the case of audio-coupled-haptic playback, there is no format conversion and
        // resampling in the framework, same format/channel/sampleRate for client and the output
        // thread is required. In the case of HapticGenerator effect, do not require format
        // matching.
        if ((outputHapticChannelCount >= hapticChannelCount && format == outputDesc->getFormat() &&
             samplingRate == outputDesc->getSamplingRate()) ||
            (outputHapticChannelCount != 0 && hasOrphanHaptic)) {
            currentMatchCriteria[0] = outputHapticChannelCount;
        }

        // functional flags match
        const int matchingFunctionalFlags =
                __builtin_popcount(outputDesc->mFlags & functionalFlags);
        const int totalFunctionalFlags =
                __builtin_popcount(outputDesc->mFlags & kFunctionalFlags);
        // Prefer matching functional flags, but subtract unnecessary functional flags.
        currentMatchCriteria[1] = 100 * (matchingFunctionalFlags + 1) - totalFunctionalFlags;

        // channel mask and channel count match
        uint32_t outputChannelCount = audio_channel_count_from_out_mask(
                outputDesc->getChannelMask());
        if (channelMask != AUDIO_CHANNEL_NONE && channelCount > 2 &&
            channelCount <= outputChannelCount) {
            if ((audio_channel_mask_get_representation(channelMask) ==
                    audio_channel_mask_get_representation(outputDesc->getChannelMask())) &&
                    ((channelMask & outputDesc->getChannelMask()) == channelMask)) {
                currentMatchCriteria[2] = outputChannelCount;
            }
            currentMatchCriteria[3] = outputChannelCount;
        }

        // sampling rate match
        if (samplingRate > SAMPLE_RATE_HZ_DEFAULT) {
            int diff;  // avoid unsigned integer overflow.
            __builtin_sub_overflow(outputDesc->getSamplingRate(), samplingRate, &diff);

            // prefer the closest output sampling rate greater than or equal to target
            // if none exists, prefer the closest output sampling rate less than target.
            //
            // criteria is offset to make non-negative.
            currentMatchCriteria[4] = diff >= 0 ? -diff + 200'000'000 : diff + 100'000'000;
        }

        // performance flags match
        currentMatchCriteria[5] = popcount(outputDesc->mFlags & performanceFlags);

        // format match
        if (format != AUDIO_FORMAT_INVALID) {
            currentMatchCriteria[6] =
                PolicyAudioPort::kFormatDistanceMax -
                PolicyAudioPort::formatDistance(format, outputDesc->getFormat());
        }

        // primary output match
        currentMatchCriteria[7] = outputDesc->mFlags & AUDIO_OUTPUT_FLAG_PRIMARY;

        // compare match criteria by priority then value
        if (std::lexicographical_compare(bestMatchCriteria.begin(), bestMatchCriteria.end(),
                currentMatchCriteria.begin(), currentMatchCriteria.end())) {
            bestMatchCriteria = currentMatchCriteria;
            bestOutput = output;

            std::stringstream result;
            std::copy(bestMatchCriteria.begin(), bestMatchCriteria.end(),
                std::ostream_iterator<int>(result, " "));
            ALOGV("%s new bestOutput %d criteria %s",
                __func__, bestOutput, result.str().c_str());
        }
    }

    return bestOutput;
}

status_t AudioPolicyManager::startOutput(audio_port_handle_t portId)
{
    ALOGV("%s portId %d", __FUNCTION__, portId);

    sp<SwAudioOutputDescriptor> outputDesc = mOutputs.getOutputForClient(portId);
    if (outputDesc == 0) {
        ALOGW("startOutput() no output for client %d", portId);
        return DEAD_OBJECT;
    }
    sp<TrackClientDescriptor> client = outputDesc->getClient(portId);

    ALOGV("startOutput() output %d, stream %d, session %d",
          outputDesc->mIoHandle, client->stream(), client->session());

    if (com::android::media::audioserver::fix_concurrent_playback_behavior_with_bit_perfect_client()
            && gHighPriorityUseCases.count(client->attributes().usage) != 0
            && outputDesc->isBitPerfect()) {
        // Usually, APM selects bit-perfect output for high priority use cases only when
        // bit-perfect output is the only output that can be routed to the selected device.
        // However, here is no need to play high priority use cases such as ringtone and alarm
        // on the bit-perfect path. Reopen the output and return DEAD_OBJECT so that the client
        // can attach to new output.
        ALOGD("%s: reopen bit-perfect output as high priority use case(%d) is starting",
              __func__, client->stream());
        reopenOutput(outputDesc, nullptr /*config*/, AUDIO_OUTPUT_FLAG_NONE, __func__);
        return DEAD_OBJECT;
    }

    status_t status = outputDesc->start();
    if (status != NO_ERROR) {
        return status;
    }

    uint32_t delayMs;
    status = startSource(outputDesc, client, &delayMs);

    if (status != NO_ERROR) {
        outputDesc->stop();
        if (status == DEAD_OBJECT) {
            sp<SwAudioOutputDescriptor> desc =
                    reopenOutput(outputDesc, nullptr /*config*/, AUDIO_OUTPUT_FLAG_NONE, __func__);
            if (desc == nullptr) {
                // This is not common, it may indicate something wrong with the HAL.
                ALOGE("%s unable to open output with default config", __func__);
                return status;
            }
        }
        return status;
    }

    // If the client is the first one active on preferred mixer parameters, reopen the output
    // if the current mixer parameters doesn't match the preferred one.
    if (outputDesc->devices().size() == 1) {
        sp<PreferredMixerAttributesInfo> info = getPreferredMixerAttributesInfo(
                outputDesc->devices()[0]->getId(), client->strategy());
        if (info != nullptr && info->getUid() == client->uid()) {
            if (info->getActiveClientCount() == 0 && !outputDesc->isConfigurationMatched(
                    info->getConfigBase(), info->getFlags())) {
                stopSource(outputDesc, client);
                outputDesc->stop();
                audio_config_t config = AUDIO_CONFIG_INITIALIZER;
                config.channel_mask = info->getConfigBase().channel_mask;
                config.sample_rate = info->getConfigBase().sample_rate;
                config.format = info->getConfigBase().format;
                sp<SwAudioOutputDescriptor> desc =
                        reopenOutput(outputDesc, &config, info->getFlags(), __func__);
                if (desc == nullptr) {
                    return BAD_VALUE;
                }
                desc->mPreferredAttrInfo = info;
                // Intentionally return error to let the client side resending request for
                // creating and starting.
                return DEAD_OBJECT;
            }
            info->increaseActiveClient();
            if (info->getActiveClientCount() == 1 && info->isBitPerfect()) {
                // If it is first bit-perfect client, reroute all clients that will be routed to
                // the bit-perfect sink so that it is guaranteed only bit-perfect stream is active.
                PortHandleVector clientsToInvalidate;
                std::vector<sp<SwAudioOutputDescriptor>> outputsToResetDevice;
                for (size_t i = 0; i < mOutputs.size(); i++) {
                    if (mOutputs[i] == outputDesc || (!mOutputs[i]->devices().isEmpty() &&
                        mOutputs[i]->devices().filter(outputDesc->devices()).isEmpty())) {
                        continue;
                    }
                    if (mOutputs[i]->getPatchHandle() != AUDIO_PATCH_HANDLE_NONE) {
                        outputsToResetDevice.push_back(mOutputs[i]);
                    }
                    for (const auto& c : mOutputs[i]->getClientIterable()) {
                        clientsToInvalidate.push_back(c->portId());
                    }
                }
                if (!clientsToInvalidate.empty()) {
                    ALOGD("%s Invalidate clients due to first bit-perfect client started",
                          __func__);
                    mpClientInterface->invalidateTracks(clientsToInvalidate);
                }
                for (const auto& output : outputsToResetDevice) {
                    resetOutputDevice(output, 0 /*delayMs*/, nullptr /*patchHandle*/);
                }
            }
        }
    }

    if (client->hasPreferredDevice()) {
        // playback activity with preferred device impacts routing occurred, inform upper layers
        mpClientInterface->onRoutingUpdated();
    }
    if (delayMs != 0) {
        usleep(delayMs * 1000);
    }

    if (status == NO_ERROR &&
        outputDesc->mPreferredAttrInfo != nullptr &&
        outputDesc->isBitPerfect() &&
        com::android::media::audioserver::
                fix_concurrent_playback_behavior_with_bit_perfect_client()) {
        // A new client is started on bit-perfect output, update all clients internal mute.
        updateClientsInternalMute(outputDesc);
    }

    return status;
}

bool AudioPolicyManager::isLeUnicastActive() const {
    if (isInCall()) {
        return true;
    }
    return isAnyDeviceTypeActive(getAudioDeviceOutLeAudioUnicastSet());
}

bool AudioPolicyManager::isAnyDeviceTypeActive(const DeviceTypeSet& deviceTypes) const {
    if (mAvailableOutputDevices.getDevicesFromTypes(deviceTypes).isEmpty()) {
        return false;
    }
    bool active = mOutputs.isAnyDeviceTypeActive(deviceTypes);
    ALOGV("%s active %d", __func__, active);
    return active;
}

status_t AudioPolicyManager::startSource(const sp<SwAudioOutputDescriptor>& outputDesc,
                                         const sp<TrackClientDescriptor>& client,
                                         uint32_t *delayMs)
{
    // cannot start beacon playback if any other output is being used
    uint32_t beaconMuteLatency = 0;

    *delayMs = 0;
    audio_stream_type_t stream = client->stream();
    auto clientVolSrc = client->volumeSource();
    auto clientStrategy = client->strategy();
    auto clientAttr = client->attributes();
    // SPEAKER_CLEANUP doesn't the share the high-frequency requirements of beacons
    if (clientAttr.usage != AUDIO_USAGE_SPEAKER_CLEANUP) {
        if (stream == AUDIO_STREAM_TTS) {
            ALOGV("\t found BEACON stream");
            if (!mTtsOutputAvailable && mOutputs.isAnyOutputActive(
                    toVolumeSource(AUDIO_STREAM_TTS, false) /*sourceToIgnore*/)) {
                return INVALID_OPERATION;
            } else {
                beaconMuteLatency = handleEventForBeacon(STARTING_BEACON);
            }
        } else {
            // some playback other than beacon starts
            beaconMuteLatency = handleEventForBeacon(STARTING_OUTPUT);
        }
    } else {
        // TODO handle muting of other streams outside of a11y
    }

    // force device change if the output is inactive and no audio patch is already present.
    // check active before incrementing usage count
    bool force = !outputDesc->isActive() && !outputDesc->isRouted();

    DeviceVector devices;
    sp<AudioPolicyMix> policyMix = outputDesc->mPolicyMix.promote();
    const char *address = NULL;
    if (policyMix != nullptr) {
        audio_devices_t newDeviceType;
        address = policyMix->mDeviceAddress.c_str();
        if ((policyMix->mRouteFlags & MIX_ROUTE_FLAG_LOOP_BACK) == MIX_ROUTE_FLAG_LOOP_BACK) {
            newDeviceType = AUDIO_DEVICE_OUT_REMOTE_SUBMIX;
        } else {
            newDeviceType = policyMix->mDeviceType;
        }
        sp device = mAvailableOutputDevices.getDevice(newDeviceType, String8(address),
                                                        AUDIO_FORMAT_DEFAULT);
        ALOG_ASSERT(device, "%s: no device found t=%u, a=%s", __func__, newDeviceType, address);
        devices.add(device);
    }

    // requiresMuteCheck is false when we can bypass mute strategy.
    // It covers a common case when there is no materially active audio
    // and muting would result in unnecessary delay and dropped audio.
    const uint32_t outputLatencyMs = outputDesc->latency();
    bool requiresMuteCheck = outputDesc->isActive(outputLatencyMs * 2);  // account for drain
    bool wasLeUnicastActive = isLeUnicastActive();

    // increment usage count for this stream on the requested output:
    // NOTE that the usage count is the same for duplicated output and hardware output which is
    // necessary for a correct control of hardware output routing by startOutput() and stopOutput()
    outputDesc->setClientActive(client, true);

    if (client->hasPreferredDevice(true)) {
        if (outputDesc->sameExclusivePreferredDevicesCount() > 0) {
            // Preferred device may be exclusive, use only if no other active clients on this output
            devices = DeviceVector(
                        mAvailableOutputDevices.getDeviceFromId(client->preferredDeviceId()));
        } else {
            devices = getNewOutputDevices(outputDesc, false /*fromCache*/);
        }
        if (devices != outputDesc->devices()) {
            checkStrategyRoute(clientStrategy, outputDesc->mIoHandle);
        }
    }

    if (followsSameRouting(clientAttr, attributes_initializer(AUDIO_USAGE_MEDIA))) {
        selectOutputForMusicEffects();
    }

    if (outputDesc->getActivityCount(clientVolSrc) == 1 || !devices.isEmpty()) {
        // starting an output being rerouted?
        if (devices.isEmpty()) {
            devices = getNewOutputDevices(outputDesc, false /*fromCache*/);
        }
        bool shouldWait =
            (followsSameRouting(clientAttr, attributes_initializer(AUDIO_USAGE_ALARM)) ||
             followsSameRouting(clientAttr, attributes_initializer(AUDIO_USAGE_NOTIFICATION)) ||
             (beaconMuteLatency > 0));
        uint32_t waitMs = beaconMuteLatency;
        const bool needToCloseBitPerfectOutput =
                (com::android::media::audioserver::
                        fix_concurrent_playback_behavior_with_bit_perfect_client() &&
                gHighPriorityUseCases.count(clientAttr.usage) != 0);
        std::vector<sp<SwAudioOutputDescriptor>> outputsToReopen;
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (desc != outputDesc) {
                // An output has a shared device if
                // - managed by the same hw module
                // - supports the currently selected device
                const bool sharedDevice = outputDesc->sharesHwModuleWith(desc)
                        && (!desc->filterSupportedDevices(devices).isEmpty());

                // force a device change if any other output is:
                // - managed by the same hw module
                // - supports currently selected device
                // - has a current device selection that differs from selected device.
                // - has an active audio patch
                // In this case, the audio HAL must receive the new device selection so that it can
                // change the device currently selected by the other output.
                if (sharedDevice &&
                        desc->devices() != devices &&
                        desc->getPatchHandle() != AUDIO_PATCH_HANDLE_NONE) {
                    force = true;
                }
                // wait for audio on other active outputs to be presented when starting
                // a notification so that audio focus effect can propagate, or that a mute/unmute
                // event occurred for beacon
                const uint32_t latencyMs = desc->latency();
                const bool isActive = desc->isActive(latencyMs * 2);  // account for drain

                if (shouldWait && isActive && (waitMs < latencyMs)) {
                    waitMs = latencyMs;
                }

                // Require mute check if another output is on a shared device
                // and currently active to have proper drain and avoid pops.
                // Note restoring AudioTracks onto this output needs to invoke
                // a volume ramp if there is no mute.
                requiresMuteCheck |= sharedDevice && isActive;

                if (desc->isBitPerfect()) {
                    if (needToCloseBitPerfectOutput) {
                        outputsToReopen.push_back(desc);
                    } else if (!desc->devices().filter(devices).isEmpty()) {
                        // There is an active bit-perfect playback on one of the targeted device,
                        // the client should be reattached to the bit-perfect thread.
                        ALOGD("%s, fails as there is bit-perfect playback active", __func__);
                        return DEAD_OBJECT;
                    }
                }
            }
        }

        if (outputDesc->mPreferredAttrInfo != nullptr && devices != outputDesc->devices()) {
            // If the output is open with preferred mixer attributes, but the routed device is
            // changed when calling this function, returning DEAD_OBJECT to indicate routing
            // changed.
            return DEAD_OBJECT;
        }
        for (auto& outputToReopen : outputsToReopen) {
            reopenOutput(outputToReopen, nullptr /*config*/, AUDIO_OUTPUT_FLAG_NONE, __func__);
        }
        const uint32_t muteWaitMs =
                setOutputDevices(__func__, outputDesc, devices, force, 0, nullptr,
                                 requiresMuteCheck);

        // apply volume rules for current stream and device if necessary
        auto &curves = getVolumeCurves(client->attributes());
        if (NO_ERROR != checkAndSetVolume(curves, client->volumeSource(),
                          curves.getVolumeIndex(outputDesc->devices().types()),
                          outputDesc, outputDesc->devices().types(), 0 /*delay*/,
                          outputDesc->useHwGain() /*force*/)) {
            // request AudioService to reinitialize the volume curves asynchronously
            ALOGE("checkAndSetVolume failed, requesting volume range init");
            mpClientInterface->onVolumeRangeInitRequest();
        };

        // update the outputs if starting an output with a stream that can affect notification
        // routing
        handleNotificationRoutingForStream(stream);

        // force reevaluating accessibility routing when ringtone or alarm starts
        if (followsSameRouting(clientAttr, attributes_initializer(AUDIO_USAGE_ALARM))) {
            invalidateStreams({AUDIO_STREAM_ACCESSIBILITY});
        }

        if (waitMs > muteWaitMs) {
            *delayMs = waitMs - muteWaitMs;
        }

        // FIXME: A device change (muteWaitMs > 0) likely introduces a volume change.
        // A volume change enacted by APM with 0 delay is not synchronous, as it goes
        // via AudioCommandThread to AudioFlinger.  Hence it is possible that the volume
        // change occurs after the MixerThread starts and causes a stream volume
        // glitch.
        //
        // We do not introduce additional delay here.
    }

    if (stream == AUDIO_STREAM_ENFORCED_AUDIBLE &&
            mEngine->getForceUse(
                    AUDIO_POLICY_FORCE_FOR_SYSTEM) == AUDIO_POLICY_FORCE_SYSTEM_ENFORCED) {
        setStrategyMute(streamToStrategy(AUDIO_STREAM_ALARM), true, outputDesc);
    }

    // Automatically enable the remote submix input when output is started on a re routing mix
    // of type MIX_TYPE_RECORDERS
    if (isSingleDeviceType(devices.types(), &audio_is_remote_submix_device) &&
        policyMix != NULL && policyMix->mMixType == MIX_TYPE_RECORDERS) {
        setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                                    AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                                    address,
                                    "remote-submix",
                                    AUDIO_FORMAT_DEFAULT);
    }

    checkLeBroadcastRoutes(wasLeUnicastActive, outputDesc, *delayMs);

    return NO_ERROR;
}

void AudioPolicyManager::checkLeBroadcastRoutes(bool wasUnicastActive,
        sp<SwAudioOutputDescriptor> ignoredOutput, uint32_t delayMs) {
    bool isUnicastActive = isLeUnicastActive();

    if (wasUnicastActive != isUnicastActive) {
        std::map<audio_io_handle_t, DeviceVector> outputsToReopen;
        //reroute all outputs routed to LE broadcast if LE unicast activy changed on any output
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (desc != ignoredOutput && desc->isActive()
                    && ((isUnicastActive &&
                            !desc->devices().
                                    getDevicesFromType(AUDIO_DEVICE_OUT_BLE_BROADCAST).isEmpty())
                        || (wasUnicastActive &&
                            !desc->devices().getDevicesFromTypes(
                                    getAudioDeviceOutLeAudioUnicastSet()).isEmpty()))) {
                DeviceVector newDevices = getNewOutputDevices(desc, false /*fromCache*/);
                bool force = desc->devices() != newDevices;
                if (desc->mPreferredAttrInfo != nullptr && force) {
                    // If the device is using preferred mixer attributes, the output need to reopen
                    // with default configuration when the new selected devices are different from
                    // current routing devices.
                    outputsToReopen.emplace(mOutputs.keyAt(i), newDevices);
                    continue;
                }
                setOutputDevices(__func__, desc, newDevices, force, delayMs);
                // re-apply device specific volume if not done by setOutputDevice()
                if (!force) {
                    applyStreamVolumes(desc, newDevices.types(), delayMs);
                }
            }
        }
        reopenOutputsWithDevices(outputsToReopen);
    }
}

status_t AudioPolicyManager::stopOutput(audio_port_handle_t portId)
{
    ALOGV("%s portId %d", __FUNCTION__, portId);

    sp<SwAudioOutputDescriptor> outputDesc = mOutputs.getOutputForClient(portId);
    if (outputDesc == 0) {
        ALOGW("stopOutput() no output for client %d", portId);
        return DEAD_OBJECT;
    }
    sp<TrackClientDescriptor> client = outputDesc->getClient(portId);

    if (client->hasPreferredDevice(true)) {
        // playback activity with preferred device impacts routing occurred, inform upper layers
        mpClientInterface->onRoutingUpdated();
    }

    ALOGV("stopOutput() output %d, stream %d, session %d",
          outputDesc->mIoHandle, client->stream(), client->session());

    status_t status = stopSource(outputDesc, client);

    if (status == NO_ERROR ) {
        outputDesc->stop();
    } else {
        return status;
    }

    if (outputDesc->devices().size() == 1) {
        sp<PreferredMixerAttributesInfo> info = getPreferredMixerAttributesInfo(
                outputDesc->devices()[0]->getId(), client->strategy());
        bool outputReopened = false;
        if (info != nullptr && info->getUid() == client->uid()) {
            info->decreaseActiveClient();
            if (info->getActiveClientCount() == 0) {
                reopenOutput(outputDesc, nullptr /*config*/, AUDIO_OUTPUT_FLAG_NONE, __func__);
                outputReopened = true;
            }
        }
        if (com::android::media::audioserver::
                    fix_concurrent_playback_behavior_with_bit_perfect_client() &&
            !outputReopened && outputDesc->isBitPerfect()) {
            // Only need to update the clients' internal mute when the output is bit-perfect and it
            // is not reopened.
            updateClientsInternalMute(outputDesc);
        }
    }
    return status;
}

status_t AudioPolicyManager::stopSource(const sp<SwAudioOutputDescriptor>& outputDesc,
                                        const sp<TrackClientDescriptor>& client)
{
    // always handle stream stop, check which stream type is stopping
    audio_stream_type_t stream = client->stream();
    auto clientVolSrc = client->volumeSource();
    bool wasLeUnicastActive = isLeUnicastActive();

    // speaker cleanup is not a beacon event
    // TODO handle speaker cleanup activity
    if (client->attributes().usage != AUDIO_USAGE_SPEAKER_CLEANUP) {
        handleEventForBeacon(stream == AUDIO_STREAM_TTS ? STOPPING_BEACON : STOPPING_OUTPUT);
    }

    if (outputDesc->getActivityCount(clientVolSrc) > 0) {
        if (outputDesc->getActivityCount(clientVolSrc) == 1) {
            // Automatically disable the remote submix input when output is stopped on a
            // re routing mix of type MIX_TYPE_RECORDERS
            sp<AudioPolicyMix> policyMix = outputDesc->mPolicyMix.promote();
            if (isSingleDeviceType(
                    outputDesc->devices().types(), &audio_is_remote_submix_device) &&
                policyMix != nullptr &&
                policyMix->mMixType == MIX_TYPE_RECORDERS) {
                setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                                            AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                            policyMix->mDeviceAddress,
                                            "remote-submix", AUDIO_FORMAT_DEFAULT);
            }
        }
        bool forceDeviceUpdate = false;
        if (client->hasPreferredDevice(true) &&
                outputDesc->sameExclusivePreferredDevicesCount() < 2) {
            checkStrategyRoute(client->strategy(), AUDIO_IO_HANDLE_NONE);
            forceDeviceUpdate = true;
        }

        // decrement usage count of this stream on the output
        outputDesc->setClientActive(client, false);

        // store time at which the stream was stopped - see isStreamActive()
        if (outputDesc->getActivityCount(clientVolSrc) == 0 || forceDeviceUpdate) {
            outputDesc->setStopTime(client, systemTime());
            DeviceVector newDevices = getNewOutputDevices(outputDesc, false /*fromCache*/);

            // If the routing does not change, if an output is routed on a device using HwGain
            // (aka setAudioPortConfig) and there are still active clients following different
            // volume group(s), force reapply volume
            bool requiresVolumeCheck = outputDesc->getActivityCount(clientVolSrc) == 0 &&
                    outputDesc->useHwGain() && outputDesc->isAnyActive(VOLUME_SOURCE_NONE);

            // delay the device switch by twice the latency because stopOutput() is executed when
            // the track stop() command is received and at that time the audio track buffer can
            // still contain data that needs to be drained. The latency only covers the audio HAL
            // and kernel buffers. Also the latency does not always include additional delay in the
            // audio path (audio DSP, CODEC ...)
            setOutputDevices(__func__, outputDesc, newDevices, false, outputDesc->latency()*2,
                             nullptr, true /*requiresMuteCheck*/, requiresVolumeCheck);

            // force restoring the device selection on other active outputs if it differs from the
            // one being selected for this output
            std::map<audio_io_handle_t, DeviceVector> outputsToReopen;
            uint32_t delayMs = outputDesc->latency()*2;
            for (size_t i = 0; i < mOutputs.size(); i++) {
                sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
                if (desc != outputDesc &&
                        desc->isActive() &&
                        outputDesc->sharesHwModuleWith(desc) &&
                        (newDevices != desc->devices())) {
                    DeviceVector newDevices2 = getNewOutputDevices(desc, false /*fromCache*/);
                    bool force = desc->devices() != newDevices2;

                    if (desc->mPreferredAttrInfo != nullptr && force) {
                        // If the device is using preferred mixer attributes, the output need to
                        // reopen with default configuration when the new selected devices are
                        // different from current routing devices.
                        outputsToReopen.emplace(mOutputs.keyAt(i), newDevices2);
                        continue;
                    }
                    setOutputDevices(__func__, desc, newDevices2, force, delayMs);

                    // re-apply device specific volume if not done by setOutputDevice()
                    if (!force) {
                        applyStreamVolumes(desc, newDevices2.types(), delayMs);
                    }
                }
            }
            reopenOutputsWithDevices(outputsToReopen);
            // update the outputs if stopping one with a stream that can affect notification routing
            handleNotificationRoutingForStream(stream);
        }

        if (stream == AUDIO_STREAM_ENFORCED_AUDIBLE &&
                mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM) == AUDIO_POLICY_FORCE_SYSTEM_ENFORCED) {
            setStrategyMute(streamToStrategy(AUDIO_STREAM_ALARM), false, outputDesc);
        }

        if (followsSameRouting(client->attributes(), attributes_initializer(AUDIO_USAGE_MEDIA))) {
            selectOutputForMusicEffects();
        }

        checkLeBroadcastRoutes(wasLeUnicastActive, outputDesc, outputDesc->latency()*2);

        return NO_ERROR;
    } else {
        ALOGW("stopOutput() refcount is already 0");
        return INVALID_OPERATION;
    }
}

bool AudioPolicyManager::releaseOutput(audio_port_handle_t portId)
{
    ALOGV("%s portId %d", __FUNCTION__, portId);

    sp<SwAudioOutputDescriptor> outputDesc = mOutputs.getOutputForClient(portId);
    if (outputDesc == 0) {
        // If an output descriptor is closed due to a device routing change,
        // then there are race conditions with releaseOutput from tracks
        // that may be destroyed (with no PlaybackThread) or a PlaybackThread
        // destroyed shortly thereafter.
        //
        // Here we just log a warning, instead of a fatal error.
        ALOGW("releaseOutput() no output for client %d", portId);
        return false;
    }

    ALOGV("releaseOutput() %d", outputDesc->mIoHandle);

    sp<TrackClientDescriptor> client = outputDesc->getClient(portId);
    if (outputDesc->isClientActive(client)) {
        ALOGW("releaseOutput() inactivates portId %d in good faith", portId);
        stopOutput(portId);
    }

    if (outputDesc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) {
        if (outputDesc->mDirectOpenCount <= 0) {
            ALOGW("releaseOutput() invalid open count %d for output %d",
                  outputDesc->mDirectOpenCount, outputDesc->mIoHandle);
            return false;
        }
        if (--outputDesc->mDirectOpenCount == 0) {
            closeOutput(outputDesc->mIoHandle);
            mpClientInterface->onAudioPortListUpdate();
        }
    }

    outputDesc->removeClient(portId);
    if (outputDesc->mPendingReopenToQueryProfiles && outputDesc->getClientCount() == 0) {
        // The output is pending reopened to query dynamic profiles and
        // there is no active clients
        closeOutput(outputDesc->mIoHandle);
        sp<SwAudioOutputDescriptor> newOutputDesc = openOutputWithProfileAndDevice(
                outputDesc->mProfile, mEngine->getActiveMediaDevices(mAvailableOutputDevices));
        if (newOutputDesc == nullptr) {
            ALOGE("%s failed to open output", __func__);
        }
        return true;
    }
    return false;
}


static AudioPolicyClientInterface::MixType getMixType(audio_devices_t deviceType,
                                                      bool externallyRouted,
                                                      const sp<AudioPolicyMix>& mix) {
    using MixType = AudioPolicyClientInterface::MixType;
    // If the client chose the route, special perms
    if (externallyRouted) {
        if (is_mix_loopback_render(mix->mRouteFlags)) {
            return MixType::PUBLIC_CAPTURE_PLAYBACK;
        }
        return MixType::EXT_POLICY_REROUTE;
    }
    switch (deviceType) {
        case AUDIO_DEVICE_IN_ECHO_REFERENCE:
            return MixType::CAPTURE;
        case AUDIO_DEVICE_IN_TELEPHONY_RX:
            return MixType::TELEPHONY_RX_CAPTURE;
        case AUDIO_DEVICE_IN_REMOTE_SUBMIX:
            if (!mix) {
                return MixType::CAPTURE;
            } else {
                ALOG_ASSERT(mix->mMixType == MIX_TYPE_RECORDERS, "Invalid Mix Type");
                // when routed due to a policy, no perms (client not in control)
                // there is an external policy, but this input is attached to a mix of recorders,
                // meaning it receives audio injected into the framework, so the recorder doesn't
                // know about it and is therefore considered "legacy"
                return MixType::NONE;
            }
        default:
            return MixType::NONE;
    }
}

base::expected<media::GetInputForAttrResponse, std::variant<binder::Status, AudioConfigBase>>
AudioPolicyManager::getInputForAttr(audio_attributes_t attributes_,
                                     audio_io_handle_t requestedInput,
                                     audio_port_handle_t requestedDeviceId,
                                     audio_config_base_t config,
                                     const audio_input_flags_t flags,
                                     audio_unique_id_t riid,
                                     audio_session_t session,
                                     const AttributionSourceState& attributionSource)
{
    ALOGV("%s() source %d, sampling rate %d, format %#x, channel mask %#x, session %d, "
          "flags %#x attributes=%s requested device ID %d",
          __func__, attributes_.source, config.sample_rate, config.format, config.channel_mask,
          session, flags, toString(attributes_).c_str(), requestedDeviceId);

    sp<AudioPolicyMix> policyMix;
    sp<DeviceDescriptor> device;
    sp<AudioInputDescriptor> inputDesc;
    sp<AudioInputDescriptor> previousInputDesc;
    sp<RecordClientDescriptor> clientDesc;
    uid_t uid = static_cast<uid_t>(attributionSource.uid);
    bool isSoundTrigger;
    int vdi = 0 /* default device id */;
    audio_io_handle_t input = AUDIO_IO_HANDLE_NONE;

    if (attributes_.source == AUDIO_SOURCE_DEFAULT) {
        attributes_.source = AUDIO_SOURCE_MIC;
    }

    const auto& attributes = attributes_;

    bool externallyRouted = false;
    // Explicit routing?
    sp<DeviceDescriptor> explicitRoutingDevice =
            mAvailableInputDevices.getDeviceFromId(requestedDeviceId);

    // special case for mmap capture: if an input IO handle is specified, we reuse this input if
    // possible
    if ((flags & AUDIO_INPUT_FLAG_MMAP_NOIRQ) == AUDIO_INPUT_FLAG_MMAP_NOIRQ &&
            requestedInput != AUDIO_IO_HANDLE_NONE) {
        ssize_t index = mInputs.indexOfKey(requestedInput);
        if (index < 0) {
            return base::unexpected{Status::fromExceptionCode(
                    EX_ILLEGAL_ARGUMENT,
                    String8::format("%s unknown MMAP input %d", __func__, requestedInput))};
        }
        sp<AudioInputDescriptor> inputDesc = mInputs.valueAt(index);
        RecordClientVector clients = inputDesc->getClientsForSession(session);
        if (clients.size() == 0) {
            return base::unexpected{Status::fromExceptionCode(
                    EX_ILLEGAL_ARGUMENT, String8::format("%s unknown session %d on input %d",
                                                         __func__, session, requestedInput))};
        }
        // For MMAP mode, the first call to getInputForAttr() is made on behalf of audioflinger.
        // The second call is for the first active client and sets the UID. Any further call
        // corresponds to a new client and is only permitted from the same UID.
        // If the first UID is silenced, allow a new UID connection and replace with new UID
        if (clients.size() > 1) {
            for (const auto& client : clients) {
                // The client map is ordered by key values (portId) and portIds are allocated
                // incrementaly. So the first client in this list is the one opened by audio flinger
                // when the mmap stream is created and should be ignored as it does not correspond
                // to an actual client
                if (client == *clients.cbegin()) {
                    continue;
                }
                if (uid != client->uid() && !client->isSilenced()) {
                    return base::unexpected{Status::fromExceptionCode(
                            EX_ILLEGAL_STATE,
                            String8::format("%s bad uid %d for client %d uid %d", __func__, uid,
                                            client->portId(), client->uid()))};
                }
            }
        }
        input = requestedInput;
        device = inputDesc->getDevice();
    } else if (attributes.source == AUDIO_SOURCE_REMOTE_SUBMIX &&
                extractAddressFromAudioAttributes(attributes).has_value()) {
        status_t status = mPolicyMixes.getInputMixForAttr(attributes, &policyMix);
        if (status != NO_ERROR) {
            ALOGW("%s could not find input mix for attr %s",
                    __func__, toString(attributes).c_str());
            return base::unexpected {aidl_utils::binderStatusFromStatusT(status)};
        }
        device = mAvailableInputDevices.getDevice(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                                                  String8(attributes.tags + strlen("addr=")),
                                                  AUDIO_FORMAT_DEFAULT);
        externallyRouted = true;
    } else {
        if (explicitRoutingDevice != nullptr) {
            device = explicitRoutingDevice;
        } else {
            // Prevent from storing invalid requested device id in clients
            requestedDeviceId = AUDIO_PORT_HANDLE_NONE;
            device = mEngine->getInputDeviceForAttributes(
                    attributes, true /*ignorePreferredDevice*/, uid, session, &policyMix);
            ALOGV_IF(device != nullptr, "%s found device type is 0x%X",
                __FUNCTION__, device->type());
        }
    }

    if (device == nullptr) {
        const auto attr = legacy2aidl_audio_attributes_t_AudioAttributes(attributes);
        return base::unexpected{Status::fromExceptionCode(
                EX_ILLEGAL_ARGUMENT,
                String8::format("%s could not find device for attr %s", __func__,
                                attr.has_value() ? attr->toString().c_str() : ""))};
    }

    const auto mixType = getMixType(device->type(), externallyRouted, policyMix);
    const AudioPolicyClientInterface::PermissionReqs permReq {
        .source =  legacy2aidl_audio_source_t_AudioSource(attributes.source).value(),
        .mixType = mixType,
        .virtualDeviceId = (mixType == AudioPolicyClientInterface::MixType::NONE &&
                            policyMix != nullptr) ? policyMix->mVirtualDeviceId : 0,

        .isHotword = (flags & (AUDIO_INPUT_FLAG_HW_HOTWORD | AUDIO_INPUT_FLAG_HOTWORD_TAP |
                               AUDIO_INPUT_FLAG_HW_LOOKBACK)) != 0,
        .isCallRedir = (attributes.flags & AUDIO_FLAG_CALL_REDIRECTION) != 0,
    };

    auto permRes = mpClientInterface->checkPermissionForInput(attributionSource, permReq);
    if (!permRes.has_value()) return base::unexpected {permRes.error()};
    if (!permRes.value()) {
        return base::unexpected{Status::fromExceptionCode(
                EX_SECURITY, String8::format("%s: %s missing perms for source %d mix %d vdi %d"
                    "hotword? %d callredir? %d", __func__, attributionSource.toString().c_str(),
                                             static_cast<int>(permReq.source),
                                             static_cast<int>(permReq.mixType),
                                             permReq.virtualDeviceId,
                                             permReq.isHotword,
                                             permReq.isCallRedir))};
    }

    if (input == AUDIO_IO_HANDLE_NONE) {
        input = getInputForDevice(device, session, attributes, config, flags, policyMix);
        if (input == AUDIO_IO_HANDLE_NONE) {
            AudioProfileVector profiles;
            status_t ret = getProfilesForDevices(
                    DeviceVector(device), profiles, flags, true /*isInput*/);
            if (ret == NO_ERROR && !profiles.empty()) {
                const auto channels = profiles[0]->getChannels();
                if (!channels.empty() && (channels.find(config.channel_mask) == channels.end())) {
                    config.channel_mask = *channels.begin();
                }
                const auto sampleRates = profiles[0]->getSampleRates();
                if (!sampleRates.empty() &&
                        (sampleRates.find(config.sample_rate) == sampleRates.end())) {
                    config.sample_rate = *sampleRates.begin();
                }
                config.format = profiles[0]->getFormat();
            }
            const auto suggestedConfig = VALUE_OR_FATAL(
                legacy2aidl_audio_config_base_t_AudioConfigBase(config, true /*isInput*/));
            return base::unexpected {suggestedConfig};
        }
    }

    auto selectedDeviceId = mAvailableInputDevices.contains(device) ?
                device->getId() : AUDIO_PORT_HANDLE_NONE;

    isSoundTrigger = attributes.source == AUDIO_SOURCE_HOTWORD &&
        mSoundTriggerSessions.indexOfKey(session) >= 0;

    const auto allocatedPortId = PolicyAudioPort::getNextUniqueId();

    clientDesc = new RecordClientDescriptor(allocatedPortId, riid, uid, session, attributes, config,
                                            requestedDeviceId, attributes.source, flags,
                                            isSoundTrigger);
    inputDesc = mInputs.valueFor(input);
    // Move (if found) effect for the client session to its input
    mEffects.moveEffectsForIo(session, input, &mInputs, mpClientInterface);
    inputDesc->addClient(clientDesc);

    ALOGV("getInputForAttr() returns input %d selectedDeviceId %d vdi %d for port ID %d",
            input, selectedDeviceId, permReq.virtualDeviceId, allocatedPortId);

    auto ret = media::GetInputForAttrResponse {};
    ret.input = input;
    ret.selectedDeviceId = selectedDeviceId;
    ret.portId = allocatedPortId;
    ret.virtualDeviceId = permReq.virtualDeviceId;
    ret.config = legacy2aidl_audio_config_base_t_AudioConfigBase(config, true /*isInput*/).value();
    ret.source = legacy2aidl_audio_source_t_AudioSource(attributes.source).value();
    return ret;
}

audio_io_handle_t AudioPolicyManager::getInputForDevice(const sp<DeviceDescriptor>& device,
                                                        audio_session_t session,
                                                        const audio_attributes_t& attributes,
                                                        const audio_config_base_t& config,
                                                        audio_input_flags_t flags,
                                                        const sp<AudioPolicyMix>& policyMix) {
    audio_io_handle_t input = AUDIO_IO_HANDLE_NONE;
    audio_source_t halInputSource = attributes.source;
    bool isSoundTrigger = false;

    if (attributes.source == AUDIO_SOURCE_HOTWORD) {
        ssize_t index = mSoundTriggerSessions.indexOfKey(session);
        if (index >= 0) {
            input = mSoundTriggerSessions.valueFor(session);
            isSoundTrigger = true;
            flags = (audio_input_flags_t)(flags | AUDIO_INPUT_FLAG_HW_HOTWORD);
            ALOGV("SoundTrigger capture on session %d input %d", session, input);
        } else {
            halInputSource = AUDIO_SOURCE_VOICE_RECOGNITION;
        }
    } else if (attributes.source == AUDIO_SOURCE_VOICE_COMMUNICATION &&
               audio_is_linear_pcm(config.format)) {
        flags = (audio_input_flags_t)(flags | AUDIO_INPUT_FLAG_VOIP_TX);
    }

    if (attributes.source == AUDIO_SOURCE_ULTRASOUND) {
        flags = (audio_input_flags_t)(flags | AUDIO_INPUT_FLAG_ULTRASOUND);
    }

    // sampling rate and flags may be updated by getInputProfile
    uint32_t profileSamplingRate = (config.sample_rate == 0) ?
            SAMPLE_RATE_HZ_DEFAULT : config.sample_rate;
    audio_format_t profileFormat = config.format;
    audio_channel_mask_t profileChannelMask = config.channel_mask;
    audio_input_flags_t profileFlags = flags;
    // find a compatible input profile (not necessarily identical in parameters)
    sp<IOProfile> profile = getInputProfile(
            device, profileSamplingRate, profileFormat, profileChannelMask, profileFlags);
    if (profile == nullptr) {
        return input;
    }

    // Pick input sampling rate if not specified by client
    uint32_t samplingRate = config.sample_rate;
    if (samplingRate == 0) {
        samplingRate = profileSamplingRate;
    }

    if (profile->getModuleHandle() == 0) {
        ALOGE("getInputForAttr(): HW module %s not opened", profile->getModuleName());
        return input;
    }

    // Reuse an already opened input if:
    //  - a client with the same session ID already exists on that input
    //  - OR the requested device is a remote submix device with the same adrress
    //    as the one connected to that input
    for (size_t i = 0; i < mInputs.size(); i++) {
        sp <AudioInputDescriptor> desc = mInputs.valueAt(i);
        if (desc->mProfile != profile) {
            continue;
        }
        RecordClientVector clients = desc->clientsList();
        for (const auto &client : clients) {
            if (session == client->session()) {
                return desc->mIoHandle;
            }
        }
        if (audio_is_remote_submix_device(device->type())
                && (device->address() != "0")
                && device->equals(desc->getDevice())) {
            return desc->mIoHandle;
        }
    }

    bool isPreemptor = false;
    if (!profile->canOpenNewIo()) {
        if (com::android::media::audioserver::fix_input_sharing_logic()) {
            //  First pick best candidate for preemption (there may not be any):
            //  - Preempt and input if:
            //     - It has only strictly lower priority use cases than the new client
            //     - It has equal priority use cases than the new client, was not
            //     opened thanks to preemption, is not routed to the same device than the device to
            //     consider or has been active since opened.
            //  - Order the preemption candidates by inactive first and priority second
            sp<AudioInputDescriptor> closeCandidate;
            int leastCloseRank = INT_MAX;
            static const int sCloseActive = 0x100;

            for (size_t i = 0; i < mInputs.size(); i++) {
                sp<AudioInputDescriptor> desc = mInputs.valueAt(i);
                if (desc->mProfile != profile) {
                    continue;
                }
                sp<RecordClientDescriptor> topPrioClient = desc->getHighestPriorityClient();
                if (topPrioClient == nullptr) {
                    continue;
                }
                int topPrio = source_priority(topPrioClient->source());
                if (topPrio < source_priority(attributes.source)
                      || (topPrio == source_priority(attributes.source)
                          && !(desc->isPreemptor() || desc->getDevice() == device))) {
                    int closeRank = (desc->isActive() ? sCloseActive : 0) + topPrio;
                    if (closeRank < leastCloseRank) {
                        leastCloseRank = closeRank;
                        closeCandidate = desc;
                    }
                }
            }

            if (closeCandidate != nullptr) {
                closeInput(closeCandidate->mIoHandle);
                // Mark the new input as being issued from a preemption
                // so that is will not be preempted later
                isPreemptor = true;
            } else {
                // Then pick the best reusable input (There is always one)
                // The order of preference is:
                // 1) active inputs with same use case as the new client
                // 2) inactive inputs with same use case
                // 3) active inputs with different use cases
                // 4) inactive inputs with different use cases
                sp<AudioInputDescriptor> reuseCandidate;
                int leastReuseRank = INT_MAX;
                static const int sReuseDifferentUseCase = 0x100;

                for (size_t i = 0; i < mInputs.size(); i++) {
                    sp<AudioInputDescriptor> desc = mInputs.valueAt(i);
                    if (desc->mProfile != profile) {
                        continue;
                    }
                    int reuseRank = sReuseDifferentUseCase;
                    for (const auto& client: desc->getClientIterable()) {
                        if (client->source() == attributes.source) {
                            reuseRank = 0;
                            break;
                        }
                    }
                    reuseRank += desc->isActive() ? 0 : 1;
                    if (reuseRank < leastReuseRank) {
                        leastReuseRank = reuseRank;
                        reuseCandidate = desc;
                    }
                }
                return reuseCandidate->mIoHandle;
            }
        } else { // fix_input_sharing_logic()
            for (size_t i = 0; i < mInputs.size(); ) {
                 sp<AudioInputDescriptor> desc = mInputs.valueAt(i);
                 if (desc->mProfile != profile) {
                     i++;
                     continue;
                 }
                // if sound trigger, reuse input if used by other sound trigger on same session
                // else
                //    reuse input if active client app is not in IDLE state
                //
                RecordClientVector clients = desc->clientsList();
                bool doClose = false;
                for (const auto& client : clients) {
                    if (isSoundTrigger != client->isSoundTrigger()) {
                        continue;
                    }
                    if (client->isSoundTrigger()) {
                        if (session == client->session()) {
                            return desc->mIoHandle;
                        }
                        continue;
                    }
                    if (client->active() && client->appState() != APP_STATE_IDLE) {
                        return desc->mIoHandle;
                    }
                    doClose = true;
                }
                if (doClose) {
                    closeInput(desc->mIoHandle);
                } else {
                    i++;
                }
            }
        }
    }

    sp<AudioInputDescriptor> inputDesc = new AudioInputDescriptor(
            profile, mpClientInterface, isPreemptor);

    audio_config_t lConfig = AUDIO_CONFIG_INITIALIZER;
    lConfig.sample_rate = profileSamplingRate;
    lConfig.channel_mask = profileChannelMask;
    lConfig.format = profileFormat;

    status_t status = inputDesc->open(&lConfig, device, halInputSource, profileFlags, &input);

    // only accept input with the exact requested set of parameters
    if (status != NO_ERROR || input == AUDIO_IO_HANDLE_NONE ||
        (profileSamplingRate != lConfig.sample_rate) ||
        !audio_formats_match(profileFormat, lConfig.format) ||
        (profileChannelMask != lConfig.channel_mask)) {
        ALOGW("getInputForAttr() failed opening input: sampling rate %d"
              ", format %#x, channel mask %#x",
              profileSamplingRate, profileFormat, profileChannelMask);
        if (input != AUDIO_IO_HANDLE_NONE) {
            inputDesc->close();
        }
        return AUDIO_IO_HANDLE_NONE;
    }

    inputDesc->mPolicyMix = policyMix;

    addInput(input, inputDesc);
    mpClientInterface->onAudioPortListUpdate();

    return input;
}

status_t AudioPolicyManager::startInput(audio_port_handle_t portId)
{
    ALOGV("%s portId %d", __FUNCTION__, portId);

    sp<AudioInputDescriptor> inputDesc = mInputs.getInputForClient(portId);
    if (inputDesc == 0) {
        ALOGW("%s no input for client %d", __FUNCTION__, portId);
        return DEAD_OBJECT;
    }
    audio_io_handle_t input = inputDesc->mIoHandle;
    sp<RecordClientDescriptor> client = inputDesc->getClient(portId);
    if (client->active()) {
        ALOGW("%s input %d client %d already started", __FUNCTION__, input, client->portId());
        return INVALID_OPERATION;
    }

    audio_session_t session = client->session();

    ALOGV("%s input:%d, session:%d)", __FUNCTION__, input, session);

    Vector<sp<AudioInputDescriptor>> activeInputs = mInputs.getActiveInputs();

    status_t status = inputDesc->start();
    if (status != NO_ERROR) {
        return status;
    }

    // increment activity count before calling getNewInputDevice() below as only active sessions
    // are considered for device selection
    inputDesc->setClientActive(client, true);

    // indicate active capture to sound trigger service if starting capture from a mic on
    // primary HW module
    sp<DeviceDescriptor> device = getNewInputDevice(inputDesc);
    if (device != nullptr) {
        status = setInputDevice(input, device, true /* force */);
    } else {
        ALOGW("%s no new input device can be found for descriptor %d",
                __FUNCTION__, inputDesc->getId());
        status = BAD_VALUE;
    }

    if (status == NO_ERROR && inputDesc->activeCount() == 1) {
        sp<AudioPolicyMix> policyMix = inputDesc->mPolicyMix.promote();
        // if input maps to a dynamic policy with an activity listener, notify of state change
        if ((policyMix != nullptr)
                && ((policyMix->mCbFlags & AudioMix::kCbFlagNotifyActivity) != 0)) {
            mpClientInterface->onDynamicPolicyMixStateUpdate(policyMix->mDeviceAddress,
                    MIX_STATE_MIXING);
        }

        DeviceVector primaryInputDevices = availablePrimaryModuleInputDevices();
        if (primaryInputDevices.contains(device) &&
                mInputs.activeInputsCountOnDevices(primaryInputDevices) == 1) {
            mpClientInterface->setSoundTriggerCaptureState(true);
        }

        // automatically enable the remote submix output when input is started if not
        // used by a policy mix of type MIX_TYPE_RECORDERS
        // For remote submix (a virtual device), we open only one input per capture request.
        if (audio_is_remote_submix_device(inputDesc->getDeviceType())) {
            String8 address = String8("");
            if (policyMix == nullptr) {
                address = String8("0");
            } else if (policyMix->mMixType == MIX_TYPE_PLAYERS) {
                address = policyMix->mDeviceAddress;
            }
            if (address != "") {
                setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                        AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                        address, "remote-submix", AUDIO_FORMAT_DEFAULT);
            }
        }
    } else if (status != NO_ERROR) {
        // Restore client activity state.
        inputDesc->setClientActive(client, false);
        inputDesc->stop();
    }

    ALOGV("%s input %d source = %d status = %d exit",
            __FUNCTION__, input, client->source(), status);

    return status;
}

status_t AudioPolicyManager::stopInput(audio_port_handle_t portId)
{
    ALOGV("%s portId %d", __FUNCTION__, portId);

    sp<AudioInputDescriptor> inputDesc = mInputs.getInputForClient(portId);
    if (inputDesc == 0) {
        ALOGW("%s no input for client %d", __FUNCTION__, portId);
        return DEAD_OBJECT;
    }
    audio_io_handle_t input = inputDesc->mIoHandle;
    sp<RecordClientDescriptor> client = inputDesc->getClient(portId);
    if (!client->active()) {
        ALOGW("%s input %d client %d already stopped", __FUNCTION__, input, client->portId());
        return INVALID_OPERATION;
    }
    auto old_source = inputDesc->source();
    inputDesc->setClientActive(client, false);

    inputDesc->stop();
    if (inputDesc->isActive()) {
        auto current_source = inputDesc->source();
        setInputDevice(input, getNewInputDevice(inputDesc),
                old_source != current_source /* force */);
    } else {
        sp<AudioPolicyMix> policyMix = inputDesc->mPolicyMix.promote();
        // if input maps to a dynamic policy with an activity listener, notify of state change
        if ((policyMix != nullptr)
                && ((policyMix->mCbFlags & AudioMix::kCbFlagNotifyActivity) != 0)) {
            mpClientInterface->onDynamicPolicyMixStateUpdate(policyMix->mDeviceAddress,
                    MIX_STATE_IDLE);
        }

        // automatically disable the remote submix output when input is stopped if not
        // used by a policy mix of type MIX_TYPE_RECORDERS
        if (audio_is_remote_submix_device(inputDesc->getDeviceType())) {
            String8 address = String8("");
            if (policyMix == nullptr) {
                address = String8("0");
            } else if (policyMix->mMixType == MIX_TYPE_PLAYERS) {
                address = policyMix->mDeviceAddress;
            }
            if (address != "") {
                setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                                         AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                         address, "remote-submix", AUDIO_FORMAT_DEFAULT);
            }
        }
        resetInputDevice(input);

        // indicate inactive capture to sound trigger service if stopping capture from a mic on
        // primary HW module
        DeviceVector primaryInputDevices = availablePrimaryModuleInputDevices();
        if (primaryInputDevices.contains(inputDesc->getDevice()) &&
                mInputs.activeInputsCountOnDevices(primaryInputDevices) == 0) {
            mpClientInterface->setSoundTriggerCaptureState(false);
        }
        inputDesc->clearPreemptedSessions();
    }
    return NO_ERROR;
}

void AudioPolicyManager::releaseInput(audio_port_handle_t portId)
{
    ALOGV("%s portId %d", __FUNCTION__, portId);

    sp<AudioInputDescriptor> inputDesc = mInputs.getInputForClient(portId);
    if (inputDesc == 0) {
        ALOGW("%s no input for client %d", __FUNCTION__, portId);
        return;
    }
    sp<RecordClientDescriptor> client = inputDesc->getClient(portId);
    audio_io_handle_t input = inputDesc->mIoHandle;

    ALOGV("%s %d", __FUNCTION__, input);

    inputDesc->removeClient(portId);

    // If no more clients are present in this session, park effects to an orphan chain
    RecordClientVector clientsOnSession = inputDesc->getClientsForSession(client->session());
    if (clientsOnSession.size() == 0) {
        mEffects.putOrphanEffects(client->session(), input, &mInputs, mpClientInterface);
    }
    if (inputDesc->getClientCount() > 0) {
        ALOGV("%s(%d) %zu clients remaining", __func__, portId, inputDesc->getClientCount());
        return;
    }

    closeInput(input);
    mpClientInterface->onAudioPortListUpdate();
    ALOGV("%s exit", __FUNCTION__);
}

void AudioPolicyManager::closeActiveClients(const sp<AudioInputDescriptor>& input)
{
    RecordClientVector clients = input->clientsList(true);

    for (const auto& client : clients) {
        closeClient(client->portId());
    }
}

void AudioPolicyManager::closeClient(audio_port_handle_t portId)
{
    stopInput(portId);
    releaseInput(portId);
}

bool AudioPolicyManager::checkCloseInput(const sp<AudioInputDescriptor>& input) {
    if (input->clientsList().size() == 0
            || !mAvailableInputDevices.containsAtLeastOne(input->supportedDevices())) {
        return true;
    }
    for (const auto& client : input->clientsList()) {
        sp<DeviceDescriptor> device =
            mEngine->getInputDeviceForAttributes(
                    client->attributes(), false /*ignorePreferredDevice*/, client->uid(),
                    client->session());
        if (!input->supportedDevices().contains(device)) {
            return true;
        }
    }
    setInputDevice(input->mIoHandle, getNewInputDevice(input));
    return false;
}

void AudioPolicyManager::checkCloseInputs() {
    // After connecting or disconnecting an input device, close input if:
    // - it has no client (was just opened to check profile)  OR
    // - none of its supported devices are connected anymore OR
    // - one of its clients cannot be routed to one of its supported
    // devices anymore. Otherwise update device selection
    std::vector<audio_io_handle_t> inputsToClose;
    for (size_t i = 0; i < mInputs.size(); i++) {
        if (checkCloseInput(mInputs.valueAt(i))) {
            inputsToClose.push_back(mInputs.keyAt(i));
        }
    }
    for (const audio_io_handle_t handle : inputsToClose) {
        ALOGV("%s closing input %d", __func__, handle);
        closeInput(handle);
    }
}

status_t AudioPolicyManager::setDeviceAbsoluteVolumeEnabled(audio_devices_t deviceType,
                                                            const char *address __unused,
                                                            bool enabled,
                                                            audio_stream_type_t streamToDriveAbs)
{
    ALOGI("%s: deviceType 0x%X, enabled %d, streamToDriveAbs %d", __func__, deviceType, enabled,
          streamToDriveAbs);

    bool changed = false;
    audio_attributes_t attributesToDriveAbs = mEngine->getAttributesForStreamType(streamToDriveAbs);
    if (enabled) {
        if (attributesToDriveAbs == AUDIO_ATTRIBUTES_INITIALIZER) {
            ALOGW("%s: no attributes for stream %s, bailing out", __func__,
                  toString(streamToDriveAbs).c_str());
            return BAD_VALUE;
        }

        const auto attrIt = mAbsoluteVolumeDrivingStreams.find(deviceType);
        if (attrIt == mAbsoluteVolumeDrivingStreams.end() ||
            (attrIt->second.usage != attributesToDriveAbs.usage ||
             attrIt->second.content_type != attributesToDriveAbs.content_type ||
             attrIt->second.flags != attributesToDriveAbs.flags)) {
            mAbsoluteVolumeDrivingStreams[deviceType] = attributesToDriveAbs;
            changed = true;
        }
    } else {
        if (mAbsoluteVolumeDrivingStreams.erase(deviceType) != 0) {
            changed = true;
        }
    }

    const DeviceVector devices = mEngine->getOutputDevicesForAttributes(
            attributesToDriveAbs, nullptr /* preferredDevice */, true /* fromCache */);
    audio_devices_t volumeDevice = Volume::getDeviceForVolume(devices.types());
    changed &= (volumeDevice == deviceType);
    // if something changed on the output device for the changed attributes, apply the stream
    // volumes regarding the new absolute mode to all the outputs without any delay
    if (changed) {
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            DeviceTypeSet curDevices = desc->devices().types();
            if (volumeDevice != Volume::getDeviceForVolume(curDevices)) {
                continue;  // skip if not using the target volume device
            }

            ALOGI("%s: apply stream volumes for %s(curDevices %s) and device type 0x%X", __func__,
                  desc->info().c_str(), dumpDeviceTypes(curDevices).c_str(), deviceType);
            applyStreamVolumes(desc, {deviceType});
        }
    }

    return NO_ERROR;
}

void AudioPolicyManager::initStreamVolume(audio_stream_type_t stream, int indexMin, int indexMax)
{
    ALOGV("initStreamVolume() stream %d, min %d, max %d", stream , indexMin, indexMax);
    if (indexMin < 0 || indexMax < 0) {
        ALOGE("%s for stream %d: invalid min %d or max %d", __func__, stream , indexMin, indexMax);
        return;
    }
    getVolumeCurves(stream).initVolume(indexMin, indexMax);

    // initialize other private stream volumes which follow this one
    for (int curStream = 0; curStream < AUDIO_STREAM_FOR_POLICY_CNT; curStream++) {
        if (!streamsMatchForvolume(stream, (audio_stream_type_t)curStream)) {
            continue;
        }
        getVolumeCurves((audio_stream_type_t)curStream).initVolume(indexMin, indexMax);
    }
}

status_t AudioPolicyManager::setStreamVolumeIndex(audio_stream_type_t stream,
                                                  int index,
                                                  bool muted,
                                                  audio_devices_t device)
{
    auto attributes = mEngine->getAttributesForStreamType(stream);
    if (attributes == AUDIO_ATTRIBUTES_INITIALIZER) {
        ALOGW("%s: no group for stream %s, bailing out", __func__, toString(stream).c_str());
        return NO_ERROR;
    }
    ALOGV("%s: stream %s attributes=%s, index %d , device 0x%X", __func__,
          toString(stream).c_str(), toString(attributes).c_str(), index, device);
    return setVolumeIndexForAttributes(attributes, index, muted, device);
}

status_t AudioPolicyManager::getStreamVolumeIndex(audio_stream_type_t stream,
                                                  int *index,
                                                  audio_devices_t device)
{
    // if device is AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME, return volume for device selected for this
    // stream by the engine.
    DeviceTypeSet deviceTypes = {device};
    if (device == AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME) {
        deviceTypes = mEngine->getOutputDevicesForStream(
                stream, true /*fromCache*/).types();
    }
    return getVolumeIndex(getVolumeCurves(stream), *index, deviceTypes);
}

status_t AudioPolicyManager::setVolumeIndexForAttributes(const audio_attributes_t &attributes,
                                                         int index,
                                                         bool muted,
                                                         audio_devices_t device)
{
    // Get Volume group matching the Audio Attributes
    auto group = mEngine->getVolumeGroupForAttributes(attributes);
    if (group == VOLUME_GROUP_NONE) {
        ALOGD("%s: no group matching with %s", __FUNCTION__, toString(attributes).c_str());
        return BAD_VALUE;
    }
    ALOGV("%s: group %d matching with %s index %d",
            __FUNCTION__, group, toString(attributes).c_str(), index);
    if (mEngine->getStreamTypeForAttributes(attributes) == AUDIO_STREAM_PATCH) {
        ALOGV("%s: cannot change volume for PATCH stream, attrs: %s",
                __FUNCTION__, toString(attributes).c_str());
        return NO_ERROR;
    }
    status_t status = NO_ERROR;
    IVolumeCurves &curves = getVolumeCurves(attributes);
    VolumeSource vs = toVolumeSource(group);
    // AUDIO_STREAM_BLUETOOTH_SCO is only used for volume control so we remap
    // to AUDIO_STREAM_VOICE_CALL to match with relevant playback activity
    VolumeSource activityVs = (vs == toVolumeSource(AUDIO_STREAM_BLUETOOTH_SCO, false)) ?
            toVolumeSource(AUDIO_STREAM_VOICE_CALL, false) : vs;
    product_strategy_t strategy = mEngine->getProductStrategyForAttributes(attributes);


    status = setVolumeCurveIndex(index, muted, device, curves);
    if (status != NO_ERROR) {
        ALOGE("%s failed to set curve index for group %d device 0x%X", __func__, group, device);
        return status;
    }

    DeviceTypeSet curSrcDevices;
    auto curCurvAttrs = curves.getAttributes();
    if (!curCurvAttrs.empty() && curCurvAttrs.front() != defaultAttr) {
        auto attr = curCurvAttrs.front();
        curSrcDevices = mEngine->getOutputDevicesForAttributes(attr, nullptr, false).types();
    } else if (!curves.getStreamTypes().empty()) {
        auto stream = curves.getStreamTypes().front();
        curSrcDevices = mEngine->getOutputDevicesForStream(stream, false).types();
    } else {
        ALOGE("%s: Invalid src %d: no valid attributes nor stream",__func__, vs);
        return BAD_VALUE;
    }
    audio_devices_t curSrcDevice = Volume::getDeviceForVolume(curSrcDevices);
    resetDeviceTypes(curSrcDevices, curSrcDevice);

    // update volume on all outputs and streams matching the following:
    // - The requested stream (or a stream matching for volume control) is active on the output
    // - The device (or devices) selected by the engine for this stream includes
    // the requested device
    // - For non default requested device, currently selected device on the output is either the
    // requested device or one of the devices selected by the engine for this stream
    // - For default requested device (AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME), apply volume only if
    // no specific device volume value exists for currently selected device.
    // - Only apply the volume if the requested device is the desired device for volume control.
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        DeviceTypeSet curDevices = desc->devices().types();

        if (curDevices.erase(AUDIO_DEVICE_OUT_SPEAKER_SAFE)) {
            curDevices.insert(AUDIO_DEVICE_OUT_SPEAKER);
        }

        if (!(desc->isActive(activityVs) || isInCallOrScreening())) {
            continue;
        }
        if (device != AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME &&
                curDevices.find(device) == curDevices.end()) {
            continue;
        }
        bool applyVolume = false;
        if (device != AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME) {
            curSrcDevices.insert(device);
            applyVolume = (curSrcDevices.find(
                    Volume::getDeviceForVolume(curDevices)) != curSrcDevices.end())
                    && Volume::getDeviceForVolume(curSrcDevices) == device;
        } else {
            applyVolume = !curves.hasVolumeIndexForDevice(curSrcDevice);
        }
        if (!applyVolume) {
            continue; // next output
        }
        // Inter / intra volume group priority management: Loop on strategies arranged by priority
        // If a higher priority strategy is active, and the output is routed to a device with a
        // HW Gain management, do not change the volume
        if (desc->useHwGain()) {
            applyVolume = false;
            bool swMute = com_android_media_audio_ring_my_car() ? curves.isMuted() : (index == 0);
            // If the volume source is active with higher priority source, ensure at least Sw Muted
            desc->setSwMute(swMute, vs, curves.getStreamTypes(), curDevices, 0 /*delayMs*/);
            for (const auto &productStrategy : mEngine->getOrderedProductStrategies()) {
                auto activeClients = desc->clientsList(true /*activeOnly*/, productStrategy,
                                                       false /*preferredDevice*/);
                if (activeClients.empty()) {
                    continue;
                }
                bool isPreempted = false;
                bool isHigherPriority = productStrategy < strategy;
                for (const auto &client : activeClients) {
                    if (isHigherPriority && (client->volumeSource() != activityVs)) {
                        ALOGV("%s: Strategy=%d (\nrequester:\n"
                              " group %d, volumeGroup=%d attributes=%s)\n"
                              " higher priority source active:\n"
                              " volumeGroup=%d attributes=%s) \n"
                              " on output %zu, bailing out", __func__, productStrategy,
                              group, group, toString(attributes).c_str(),
                              client->volumeSource(), toString(client->attributes()).c_str(), i);
                        applyVolume = false;
                        isPreempted = true;
                        break;
                    }
                    // However, continue for loop to ensure no higher prio clients running on output
                    if (client->volumeSource() == activityVs) {
                        applyVolume = true;
                    }
                }
                if (isPreempted || applyVolume) {
                    break;
                }
            }
            if (!applyVolume) {
                continue; // next output
            }
        }
        //FIXME: workaround for truncated touch sounds
        // delayed volume change for system stream to be removed when the problem is
        // handled by system UI
        status_t volStatus = checkAndSetVolume(curves, vs, index, desc, curDevices,
                    ((vs == toVolumeSource(AUDIO_STREAM_SYSTEM, false))?
                         TOUCH_SOUND_FIXED_DELAY_MS : 0));
        if (volStatus != NO_ERROR) {
            status = volStatus;
        }
    }

    // update voice volume if the an active call route exists and target device is same as current
    if (mCallRxSourceClient != nullptr && mCallRxSourceClient->isConnected()) {
        audio_devices_t rxSinkDevice = mCallRxSourceClient->sinkDevice()->type();
        audio_devices_t curVoiceDevice = Volume::getDeviceForVolume({rxSinkDevice});
        if (curVoiceDevice == device
                && curSrcDevices.find(curVoiceDevice) != curSrcDevices.end()) {
            bool isVoiceVolSrc;
            bool isBtScoVolSrc;
            if (isVolumeConsistentForCalls(vs, {rxSinkDevice},
                    isVoiceVolSrc, isBtScoVolSrc, __func__)
                    && (isVoiceVolSrc || isBtScoVolSrc)) {
                bool voiceVolumeManagedByHost = !isBtScoVolSrc &&
                        !audio_is_ble_out_device(rxSinkDevice);
                setVoiceVolume(index, curves, voiceVolumeManagedByHost, 0);
            }
        }
    }

    mpClientInterface->onAudioVolumeGroupChanged(group, 0 /*flags*/);
    return status;
}

status_t AudioPolicyManager::setVolumeCurveIndex(int index,
                                                 bool muted,
                                                 audio_devices_t device,
                                                 IVolumeCurves &volumeCurves)
{
    // VOICE_CALL stream has minVolumeIndex > 0  but can be muted directly by an
    // app that has MODIFY_PHONE_STATE permission.
    bool hasVoice = hasVoiceStream(volumeCurves.getStreamTypes());
    if (((index < volumeCurves.getVolumeIndexMin()) && !(hasVoice && index == 0)) ||
            (index > volumeCurves.getVolumeIndexMax())) {
        ALOGE("%s: wrong index %d min=%d max=%d, device 0x%X", __FUNCTION__, index,
              volumeCurves.getVolumeIndexMin(), volumeCurves.getVolumeIndexMax(), device);
        return BAD_VALUE;
    }
    if (!audio_is_output_device(device)) {
        return BAD_VALUE;
    }

    // Force max volume if stream cannot be muted
    if (!volumeCurves.canBeMuted()) index = volumeCurves.getVolumeIndexMax();

    ALOGV("%s device %08x, index %d, muted %d", __FUNCTION__ , device, index, muted);
    volumeCurves.addCurrentVolumeIndex(device, index);
    volumeCurves.setIsMuted(muted);
    return NO_ERROR;
}

status_t AudioPolicyManager::getVolumeIndexForAttributes(const audio_attributes_t &attr,
                                                         int &index,
                                                         audio_devices_t device)
{
    // if device is AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME, return volume for device selected for this
    // stream by the engine.
    DeviceTypeSet deviceTypes = {device};
    if (device == AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME) {
        deviceTypes = mEngine->getOutputDevicesForAttributes(
                attr, nullptr, true /*fromCache*/).types();
    }
    return getVolumeIndex(getVolumeCurves(attr), index, deviceTypes);
}

status_t AudioPolicyManager::getVolumeIndex(const IVolumeCurves &curves,
                                            int &index,
                                            const DeviceTypeSet& deviceTypes) const
{
    if (!isSingleDeviceType(deviceTypes, audio_is_output_device)) {
        return BAD_VALUE;
    }
    index = curves.getVolumeIndex(deviceTypes);
    ALOGV("%s: device %s index %d", __FUNCTION__, dumpDeviceTypes(deviceTypes).c_str(), index);
    return NO_ERROR;
}

status_t AudioPolicyManager::getMinVolumeIndexForAttributes(const audio_attributes_t &attr,
                                                            int &index)
{
    index = getVolumeCurves(attr).getVolumeIndexMin();
    return NO_ERROR;
}

status_t AudioPolicyManager::getMaxVolumeIndexForAttributes(const audio_attributes_t &attr,
                                                            int &index)
{
    index = getVolumeCurves(attr).getVolumeIndexMax();
    return NO_ERROR;
}

audio_io_handle_t AudioPolicyManager::selectOutputForMusicEffects()
{
    // select one output among several suitable for global effects.
    // The priority is as follows:
    // 1: An offloaded output. If the effect ends up not being offloadable,
    //    AudioFlinger will invalidate the track and the offloaded output
    //    will be closed causing the effect to be moved to a PCM output.
    // 2: Spatializer output if the stereo spatializer feature enabled
    // 3: A deep buffer output
    // 4: The primary output
    // 5: the first output in the list

    DeviceVector devices = mEngine->getOutputDevicesForAttributes(
                attributes_initializer(AUDIO_USAGE_MEDIA), nullptr, false /*fromCache*/);
    SortedVector<audio_io_handle_t> outputs = getOutputsForDevices(devices, mOutputs);

    if (outputs.size() == 0) {
        return AUDIO_IO_HANDLE_NONE;
    }

    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
    bool activeOnly = true;

    while (output == AUDIO_IO_HANDLE_NONE) {
        audio_io_handle_t outputOffloaded = AUDIO_IO_HANDLE_NONE;
        audio_io_handle_t outputSpatializer = AUDIO_IO_HANDLE_NONE;
        audio_io_handle_t outputDeepBuffer = AUDIO_IO_HANDLE_NONE;
        audio_io_handle_t outputPrimary = AUDIO_IO_HANDLE_NONE;

        for (audio_io_handle_t outputLoop : outputs) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueFor(outputLoop);
            if (activeOnly && !desc->isActive(toVolumeSource(AUDIO_STREAM_MUSIC))) {
                continue;
            }
            ALOGV("selectOutputForMusicEffects activeOnly %d output %d flags 0x%08x",
                  activeOnly, outputLoop, desc->mFlags);
            if ((desc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
                outputOffloaded = outputLoop;
            }
            if ((desc->mFlags & AUDIO_OUTPUT_FLAG_SPATIALIZER) != 0) {
                if (SpatializerHelper::isStereoSpatializationFeatureEnabled()) {
                    outputSpatializer = outputLoop;
                }
            }
            if ((desc->mFlags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) != 0) {
                outputDeepBuffer = outputLoop;
            }
            if ((desc->mFlags & AUDIO_OUTPUT_FLAG_PRIMARY) != 0) {
                outputPrimary = outputLoop;
            }
        }
        if (outputOffloaded != AUDIO_IO_HANDLE_NONE) {
            output = outputOffloaded;
        } else if (outputSpatializer != AUDIO_IO_HANDLE_NONE) {
            output = outputSpatializer;
        } else if (outputDeepBuffer != AUDIO_IO_HANDLE_NONE) {
            output = outputDeepBuffer;
        } else if (outputPrimary != AUDIO_IO_HANDLE_NONE) {
            output = outputPrimary;
        } else {
            output = outputs[0];
        }
        activeOnly = false;
    }

    if (output != mMusicEffectOutput) {
        mEffects.moveEffects(AUDIO_SESSION_OUTPUT_MIX, mMusicEffectOutput, output,
                mpClientInterface);
        mMusicEffectOutput = output;
    }

    ALOGV("selectOutputForMusicEffects selected output %d", output);
    return output;
}

audio_io_handle_t AudioPolicyManager::getOutputForEffect(const effect_descriptor_t *desc __unused)
{
    return selectOutputForMusicEffects();
}

status_t AudioPolicyManager::registerEffect(const effect_descriptor_t *desc,
                                audio_io_handle_t io,
                                product_strategy_t strategy,
                                int session,
                                int id)
{
    if (session != AUDIO_SESSION_DEVICE && io != AUDIO_IO_HANDLE_NONE) {
        ssize_t index = mOutputs.indexOfKey(io);
        if (index < 0) {
            index = mInputs.indexOfKey(io);
            if (index < 0) {
                ALOGW("registerEffect() unknown io %d", io);
                return INVALID_OPERATION;
            }
        }
    }
    bool isMusicEffect = (session != AUDIO_SESSION_OUTPUT_STAGE)
                            && ((strategy == streamToStrategy(AUDIO_STREAM_MUSIC)
                                    || strategy == PRODUCT_STRATEGY_NONE));
    return mEffects.registerEffect(desc, io, session, id, isMusicEffect);
}

status_t AudioPolicyManager::unregisterEffect(int id)
{
    if (mEffects.getEffect(id) == nullptr) {
        return INVALID_OPERATION;
    }
    if (mEffects.isEffectEnabled(id)) {
        ALOGW("%s effect %d enabled", __FUNCTION__, id);
        setEffectEnabled(id, false);
    }
    return mEffects.unregisterEffect(id);
}

status_t AudioPolicyManager::setEffectEnabled(int id, bool enabled)
{
    sp<EffectDescriptor> effect = mEffects.getEffect(id);
    if (effect == nullptr) {
        return INVALID_OPERATION;
    }

    status_t status = mEffects.setEffectEnabled(id, enabled);
    if (status == NO_ERROR) {
        mInputs.trackEffectEnabled(effect, enabled);
    }
    return status;
}


status_t AudioPolicyManager::moveEffectsToIo(const std::vector<int>& ids, audio_io_handle_t io)
{
   mEffects.moveEffects(ids, io);
   return NO_ERROR;
}

bool AudioPolicyManager::isStreamActive(audio_stream_type_t stream, uint32_t inPastMs) const
{
    auto vs = toVolumeSource(stream, false);
    return vs != VOLUME_SOURCE_NONE ? mOutputs.isActive(vs, inPastMs) : false;
}

bool AudioPolicyManager::isStreamActiveRemotely(audio_stream_type_t stream, uint32_t inPastMs) const
{
    auto vs = toVolumeSource(stream, false);
    return vs != VOLUME_SOURCE_NONE ? mOutputs.isActiveRemotely(vs, inPastMs) : false;
}

bool AudioPolicyManager::isSourceActive(audio_source_t source) const
{
    for (size_t i = 0; i < mInputs.size(); i++) {
        const sp<AudioInputDescriptor>  inputDescriptor = mInputs.valueAt(i);
        if (inputDescriptor->isSourceActive(source)) {
            return true;
        }
    }
    return false;
}

// Register a list of custom mixes with their attributes and format.
// When a mix is registered, corresponding input and output profiles are
// added to the remote submix hw module. The profile contains only the
// parameters (sampling rate, format...) specified by the mix.
// The corresponding input remote submix device is also connected.
//
// When a remote submix device is connected, the address is checked to select the
// appropriate profile and the corresponding input or output stream is opened.
//
// When capture starts, getInputForAttr() will:
//  - 1 look for a mix matching the address passed in attribtutes tags if any
//  - 2 if none found, getDeviceForInputSource() will:
//     - 2.1 look for a mix matching the attributes source
//     - 2.2 if none found, default to device selection by policy rules
// At this time, the corresponding output remote submix device is also connected
// and active playback use cases can be transferred to this mix if needed when reconnecting
// after AudioTracks are invalidated
//
// When playback starts, getOutputForAttr() will:
//  - 1 look for a mix matching the address passed in attribtutes tags if any
//  - 2 if none found, look for a mix matching the attributes usage
//  - 3 if none found, default to device and output selection by policy rules.

status_t AudioPolicyManager::registerPolicyMixes(const Vector<AudioMix>& mixes)
{
    ALOGV("registerPolicyMixes() %zu mix(es)", mixes.size());
    status_t res = NO_ERROR;
    bool checkOutputs = false;
    sp<HwModule> rSubmixModule;
    Vector<AudioMix> registeredMixes;
    AudioDeviceTypeAddrVector devices;
    // examine each mix's route type
    for (size_t i = 0; i < mixes.size(); i++) {
        AudioMix mix = mixes[i];
        // Only capture of playback is allowed in LOOP_BACK & RENDER mode
        if (is_mix_loopback_render(mix.mRouteFlags) && mix.mMixType != MIX_TYPE_PLAYERS) {
            ALOGE("Unsupported Policy Mix %zu of %zu: "
                  "Only capture of playback is allowed in LOOP_BACK & RENDER mode",
                   i, mixes.size());
            res = INVALID_OPERATION;
            break;
        }
        // LOOP_BACK and LOOP_BACK | RENDER have the same remote submix backend and are handled
        // in the same way.
        if ((mix.mRouteFlags & MIX_ROUTE_FLAG_LOOP_BACK) == MIX_ROUTE_FLAG_LOOP_BACK) {
            ALOGV("registerPolicyMixes() mix %zu of %zu is LOOP_BACK %d", i, mixes.size(),
                  mix.mRouteFlags);
            if (rSubmixModule == 0) {
                rSubmixModule = mHwModules.getModuleFromName(
                        AUDIO_HARDWARE_MODULE_ID_REMOTE_SUBMIX);
                if (rSubmixModule == 0) {
                    ALOGE("Unable to find audio module for submix, aborting mix %zu registration",
                            i);
                    res = INVALID_OPERATION;
                    break;
                }
            }

            String8 address = mix.mDeviceAddress;
            audio_devices_t deviceTypeToMakeAvailable;
            if (mix.mMixType == MIX_TYPE_PLAYERS) {
                mix.mDeviceType = AUDIO_DEVICE_OUT_REMOTE_SUBMIX;
                deviceTypeToMakeAvailable = AUDIO_DEVICE_IN_REMOTE_SUBMIX;
            } else {
                mix.mDeviceType = AUDIO_DEVICE_IN_REMOTE_SUBMIX;
                deviceTypeToMakeAvailable = AUDIO_DEVICE_OUT_REMOTE_SUBMIX;
            }

            if (mPolicyMixes.registerMix(mix, 0 /*output desc*/) != NO_ERROR) {
                ALOGE("Error registering mix %zu for address %s", i, address.c_str());
                res = INVALID_OPERATION;
                break;
            }
            audio_config_t outputConfig = mix.mFormat;
            audio_config_t inputConfig = mix.mFormat;
            // NOTE: audio flinger mixer does not support mono output: configure remote submix HAL
            // in stereo and let audio flinger do the channel conversion if needed.
            outputConfig.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
            inputConfig.channel_mask = AUDIO_CHANNEL_IN_STEREO;
            rSubmixModule->addOutputProfile(address.c_str(), &outputConfig,
                    AUDIO_DEVICE_OUT_REMOTE_SUBMIX, address,
                    audio_is_linear_pcm(outputConfig.format)
                        ? AUDIO_OUTPUT_FLAG_NONE : AUDIO_OUTPUT_FLAG_DIRECT);
            rSubmixModule->addInputProfile(address.c_str(), &inputConfig,
                    AUDIO_DEVICE_IN_REMOTE_SUBMIX, address,
                    audio_is_linear_pcm(inputConfig.format)
                        ? AUDIO_INPUT_FLAG_NONE : AUDIO_INPUT_FLAG_DIRECT);

            if ((res = setDeviceConnectionStateInt(deviceTypeToMakeAvailable,
                    AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                    address.c_str(), "remote-submix", AUDIO_FORMAT_DEFAULT)) != NO_ERROR) {
                ALOGE("Failed to set remote submix device available, type %u, address %s",
                        mix.mDeviceType, address.c_str());
                break;
            }
        } else if ((mix.mRouteFlags & MIX_ROUTE_FLAG_RENDER) == MIX_ROUTE_FLAG_RENDER) {
            String8 address = mix.mDeviceAddress;
            audio_devices_t type = mix.mDeviceType;
            ALOGV(" registerPolicyMixes() mix %zu of %zu is RENDER, dev=0x%X addr=%s",
                    i, mixes.size(), type, address.c_str());

            sp<DeviceDescriptor> device = mHwModules.getDeviceDescriptor(
                    mix.mDeviceType, mix.mDeviceAddress,
                    String8(), AUDIO_FORMAT_DEFAULT);
            if (device == nullptr) {
                res = INVALID_OPERATION;
                break;
            }

            bool foundOutput = false;
            // First try to find an already opened output supporting the device
            for (size_t j = 0 ; j < mOutputs.size() && !foundOutput && res == NO_ERROR; j++) {
                sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(j);

                if (!desc->isDuplicated() && desc->supportedDevices().contains(device)) {
                    if (mPolicyMixes.registerMix(mix, desc) != NO_ERROR) {
                        ALOGE("Could not register mix RENDER,  dev=0x%X addr=%s", type,
                              address.c_str());
                        res = INVALID_OPERATION;
                    } else {
                        foundOutput = true;
                    }
                }
            }
            // If no output found, try to find a direct output profile supporting the device
            for (size_t i = 0; i < mHwModules.size() && !foundOutput && res == NO_ERROR; i++) {
                sp<HwModule> module = mHwModules[i];
                for (size_t j = 0;
                        j < module->getOutputProfiles().size() && !foundOutput && res == NO_ERROR;
                        j++) {
                    sp<IOProfile> profile = module->getOutputProfiles()[j];
                    if (profile->isDirectOutput() && profile->supportsDevice(device)) {
                        if (mPolicyMixes.registerMix(mix, nullptr) != NO_ERROR) {
                            ALOGE("Could not register mix RENDER,  dev=0x%X addr=%s", type,
                                  address.c_str());
                            res = INVALID_OPERATION;
                        } else {
                            foundOutput = true;
                        }
                    }
                }
            }
            if (res != NO_ERROR) {
                ALOGE(" Error registering mix %zu for device 0x%X addr %s",
                        i, type, address.c_str());
                res = INVALID_OPERATION;
                break;
            } else if (!foundOutput) {
                ALOGE(" Output not found for mix %zu for device 0x%X addr %s",
                        i, type, address.c_str());
                res = INVALID_OPERATION;
                break;
            } else {
                checkOutputs = true;
                devices.push_back(AudioDeviceTypeAddr(mix.mDeviceType, mix.mDeviceAddress.c_str()));
                registeredMixes.add(mix);
            }
        }
    }
    if (res != NO_ERROR) {
        if (audio_flags::audio_mix_ownership()) {
            // Only unregister mixes that were actually registered to not accidentally unregister
            // mixes that already existed previously.
            unregisterPolicyMixes(registeredMixes);
            registeredMixes.clear();
        } else {
            unregisterPolicyMixes(mixes);
        }
    } else if (checkOutputs) {
        checkForDeviceAndOutputChanges();
        changeOutputDevicesMuteState(devices);
        updateCallAndOutputRouting(false /* forceVolumeReeval */, 0 /* delayMs */,
            true /* skipDelays */);
        changeOutputDevicesMuteState(devices);
    }
    return res;
}

status_t AudioPolicyManager::unregisterPolicyMixes(Vector<AudioMix> mixes)
{
    ALOGV("unregisterPolicyMixes() num mixes %zu", mixes.size());
    status_t res = NO_ERROR;
    bool checkOutputs = false;
    sp<HwModule> rSubmixModule;
    AudioDeviceTypeAddrVector devices;
    // examine each mix's route type
    for (const auto& mix : mixes) {
        if ((mix.mRouteFlags & MIX_ROUTE_FLAG_LOOP_BACK) == MIX_ROUTE_FLAG_LOOP_BACK) {

            if (rSubmixModule == 0) {
                rSubmixModule = mHwModules.getModuleFromName(
                        AUDIO_HARDWARE_MODULE_ID_REMOTE_SUBMIX);
                if (rSubmixModule == 0) {
                    res = INVALID_OPERATION;
                    continue;
                }
            }

            String8 address = mix.mDeviceAddress;

            if (mPolicyMixes.unregisterMix(mix) != NO_ERROR) {
                res = INVALID_OPERATION;
                continue;
            }

            for (auto device: {AUDIO_DEVICE_IN_REMOTE_SUBMIX, AUDIO_DEVICE_OUT_REMOTE_SUBMIX}) {
                if (getDeviceConnectionState(device, address.c_str()) ==
                    AUDIO_POLICY_DEVICE_STATE_AVAILABLE) {
                    status_t currentRes =
                            setDeviceConnectionStateInt(device,
                                                        AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                                        address.c_str(),
                                                        "remote-submix",
                                                        AUDIO_FORMAT_DEFAULT);
                    if (!audio_flags::audio_mix_ownership()) {
                        res = currentRes;
                    }
                    if (currentRes != OK) {
                        ALOGE("Error making RemoteSubmix device unavailable for mix "
                              "with type %d, address %s", device, address.c_str());
                        res = INVALID_OPERATION;
                    }
                }
            }
            rSubmixModule->removeOutputProfile(address.c_str());
            rSubmixModule->removeInputProfile(address.c_str());

        } else if ((mix.mRouteFlags & MIX_ROUTE_FLAG_RENDER) == MIX_ROUTE_FLAG_RENDER) {
            if (mPolicyMixes.unregisterMix(mix) != NO_ERROR) {
                res = INVALID_OPERATION;
                continue;
            } else {
                devices.push_back(AudioDeviceTypeAddr(mix.mDeviceType, mix.mDeviceAddress.c_str()));
                checkOutputs = true;
            }
        }
    }

    if (res == NO_ERROR && checkOutputs) {
        checkForDeviceAndOutputChanges();
        changeOutputDevicesMuteState(devices);
        updateCallAndOutputRouting(false /* forceVolumeReeval */, 0 /* delayMs */,
            true /* skipDelays */);
        changeOutputDevicesMuteState(devices);
    }
    return res;
}

status_t AudioPolicyManager::getRegisteredPolicyMixes(std::vector<AudioMix>& _aidl_return) {
    if (!audio_flags::audio_mix_test_api()) {
        return INVALID_OPERATION;
    }

    _aidl_return.clear();
    _aidl_return.reserve(mPolicyMixes.size());
    for (const auto &policyMix: mPolicyMixes) {
        _aidl_return.emplace_back(policyMix->mCriteria, policyMix->mMixType,
                             policyMix->mFormat, policyMix->mRouteFlags, policyMix->mDeviceAddress,
                             policyMix->mCbFlags);
        _aidl_return.back().mDeviceType = policyMix->mDeviceType;
        _aidl_return.back().mToken = policyMix->mToken;
        _aidl_return.back().mVirtualDeviceId = policyMix->mVirtualDeviceId;
    }

    ALOGVV("%s() returning %zu registered mixes", __func__, _aidl_return.size());
    return OK;
}

status_t AudioPolicyManager::updatePolicyMix(
            const AudioMix& mix,
            const std::vector<AudioMixMatchCriterion>& updatedCriteria) {
    status_t res = mPolicyMixes.updateMix(mix, updatedCriteria);
    if (res == NO_ERROR) {
        checkForDeviceAndOutputChanges();
        updateCallAndOutputRouting();
    }
    return res;
}

void AudioPolicyManager::dumpManualSurroundFormats(String8 *dst) const
{
    size_t i = 0;
    constexpr size_t audioFormatPrefixLen = sizeof("AUDIO_FORMAT_");
    for (const auto& fmt : mManualSurroundFormats) {
        if (i++ != 0) dst->append(", ");
        std::string sfmt;
        FormatConverter::toString(fmt, sfmt);
        dst->append(sfmt.size() >= audioFormatPrefixLen ?
                sfmt.c_str() + audioFormatPrefixLen - 1 : sfmt.c_str());
    }
}

// Returns true if all devices types match the predicate and are supported by one HW module
bool  AudioPolicyManager::areAllDevicesSupported(
        const AudioDeviceTypeAddrVector& devices,
        std::function<bool(audio_devices_t)> predicate,
        const char *context,
        bool matchAddress) {
    for (size_t i = 0; i < devices.size(); i++) {
        sp<DeviceDescriptor> devDesc = mHwModules.getDeviceDescriptor(
                devices[i].mType, devices[i].getAddress(), String8(),
                AUDIO_FORMAT_DEFAULT, false /*allowToCreate*/, matchAddress);
        if (devDesc == nullptr || (predicate != nullptr && !predicate(devices[i].mType))) {
            ALOGE("%s: device type %#x address %s not supported or not match predicate",
                    context, devices[i].mType, devices[i].getAddress());
            return false;
        }
    }
    return true;
}

void AudioPolicyManager::changeOutputDevicesMuteState(
        const AudioDeviceTypeAddrVector& devices) {
    ALOGVV("%s() num devices %zu", __func__, devices.size());

    std::vector<sp<SwAudioOutputDescriptor>> outputs =
            getSoftwareOutputsForDevices(devices);

    for (size_t i = 0; i < outputs.size(); i++) {
        sp<SwAudioOutputDescriptor> outputDesc = outputs[i];
        DeviceVector prevDevices = outputDesc->devices();
        checkDeviceMuteStrategies(outputDesc, prevDevices, 0 /* delayMs */);
    }
}

std::vector<sp<SwAudioOutputDescriptor>> AudioPolicyManager::getSoftwareOutputsForDevices(
        const AudioDeviceTypeAddrVector& devices) const
{
    std::vector<sp<SwAudioOutputDescriptor>> outputs;
    DeviceVector deviceDescriptors;
    for (size_t j = 0; j < devices.size(); j++) {
        sp<DeviceDescriptor> desc = mHwModules.getDeviceDescriptor(
                devices[j].mType, devices[j].getAddress(), String8(), AUDIO_FORMAT_DEFAULT);
        if (desc == nullptr || !audio_is_output_device(devices[j].mType)) {
            ALOGE("%s: device type %#x address %s not supported or not an output device",
                __func__, devices[j].mType, devices[j].getAddress());
                    continue;
        }
        deviceDescriptors.add(desc);
    }
    for (size_t i = 0; i < mOutputs.size(); i++) {
        if (!mOutputs.valueAt(i)->supportsAtLeastOne(deviceDescriptors)) {
            continue;
        }
        outputs.push_back(mOutputs.valueAt(i));
    }
    return outputs;
}

status_t AudioPolicyManager::setUidDeviceAffinities(uid_t uid,
        const AudioDeviceTypeAddrVector& devices) {
    ALOGV("%s() uid=%d num devices %zu", __FUNCTION__, uid, devices.size());
    if (!areAllDevicesSupported(devices, audio_is_output_device, __func__)) {
        return BAD_VALUE;
    }
    status_t res =  mPolicyMixes.setUidDeviceAffinities(uid, devices);
    if (res != NO_ERROR) {
        ALOGE("%s() Could not set all device affinities for uid = %d", __FUNCTION__, uid);
        return res;
    }

    checkForDeviceAndOutputChanges();
    updateCallAndOutputRouting();

    return NO_ERROR;
}

status_t AudioPolicyManager::removeUidDeviceAffinities(uid_t uid) {
    ALOGV("%s() uid=%d", __FUNCTION__, uid);
    status_t res = mPolicyMixes.removeUidDeviceAffinities(uid);
    if (res != NO_ERROR) {
        ALOGE("%s() Could not remove all device affinities for uid = %d",
            __FUNCTION__, uid);
        return INVALID_OPERATION;
    }

    checkForDeviceAndOutputChanges();
    updateCallAndOutputRouting();

    return res;
}


status_t AudioPolicyManager::setDevicesRoleForStrategy(product_strategy_t strategy,
                                                       device_role_t role,
                                                       const AudioDeviceTypeAddrVector &devices) {
    ALOGV("%s() strategy=%d role=%d %s", __func__, strategy, role,
            dumpAudioDeviceTypeAddrVector(devices).c_str());

    if (!areAllDevicesSupported(devices, audio_is_output_device, __func__)) {
        return BAD_VALUE;
    }
    status_t status = mEngine->setDevicesRoleForStrategy(strategy, role, devices);
    if (status != NO_ERROR) {
        ALOGW("Engine could not set preferred devices %s for strategy %d role %d",
                dumpAudioDeviceTypeAddrVector(devices).c_str(), strategy, role);
        return status;
    }

    checkForDeviceAndOutputChanges();

    bool forceVolumeReeval = false;
    // FIXME: workaround for truncated touch sounds
    // to be removed when the problem is handled by system UI
    uint32_t delayMs = 0;
    if (strategy == mCommunnicationStrategy) {
        forceVolumeReeval = true;
        delayMs = TOUCH_SOUND_FIXED_DELAY_MS;
        updateInputRouting();
    }
    updateCallAndOutputRouting(forceVolumeReeval, delayMs);

    return NO_ERROR;
}

void AudioPolicyManager::updateCallAndOutputRouting(bool forceVolumeReeval, uint32_t delayMs,
    bool skipDelays)
{
    uint32_t waitMs = 0;
    bool wasLeUnicastActive = isLeUnicastActive();
    if (updateCallRouting(true /*fromCache*/, delayMs, &waitMs) == NO_ERROR) {
        // Only apply special touch sound delay once
        delayMs = 0;
    }
    std::map<audio_io_handle_t, DeviceVector> outputsToReopen;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
        DeviceVector newDevices = getNewOutputDevices(outputDesc, true /*fromCache*/);
        if ((mEngine->getPhoneState() != AUDIO_MODE_IN_CALL) ||
                (outputDesc != mPrimaryOutput && !isTelephonyRxOrTx(outputDesc))) {
            // As done in setDeviceConnectionState, we could also fix default device issue by
            // preventing the force re-routing in case of default dev that distinguishes on address.
            // Let's give back to engine full device choice decision however.
            bool newDevicesNotEmpty = !newDevices.isEmpty();
            if (outputDesc->mPreferredAttrInfo != nullptr && newDevices != outputDesc->devices()
                && newDevicesNotEmpty) {
                // If the device is using preferred mixer attributes, the output need to reopen
                // with default configuration when the new selected devices are different from
                // current routing devices.
                outputsToReopen.emplace(mOutputs.keyAt(i), newDevices);
                continue;
            }

            waitMs = setOutputDevices(__func__, outputDesc, newDevices,
                                      newDevicesNotEmpty /*force*/, delayMs,
                                      nullptr /*patchHandle*/, !skipDelays /*requiresMuteCheck*/,
                                      !newDevicesNotEmpty /*requiresVolumeCheck*/, skipDelays);
            // Only apply special touch sound delay once
            delayMs = 0;
        }
        if (forceVolumeReeval && !newDevices.isEmpty()) {
            applyStreamVolumes(outputDesc, newDevices.types(), waitMs, true);
        }
    }
    reopenOutputsWithDevices(outputsToReopen);
    checkLeBroadcastRoutes(wasLeUnicastActive, nullptr, delayMs);
}

void AudioPolicyManager::updateInputRouting() {
    for (const auto& activeDesc : mInputs.getActiveInputs()) {
        // Skip for hotword recording as the input device switch
        // is handled within sound trigger HAL
        if (activeDesc->isSoundTrigger() && activeDesc->source() == AUDIO_SOURCE_HOTWORD) {
            continue;
        }
        auto newDevice = getNewInputDevice(activeDesc);
        // Force new input selection if the new device can not be reached via current input
        if (activeDesc->mProfile->getSupportedDevices().contains(newDevice)) {
            setInputDevice(activeDesc->mIoHandle, newDevice);
        } else {
            closeInput(activeDesc->mIoHandle);
        }
    }
}

status_t
AudioPolicyManager::removeDevicesRoleForStrategy(product_strategy_t strategy,
                                                 device_role_t role,
                                                 const AudioDeviceTypeAddrVector &devices) {
    ALOGV("%s() strategy=%d role=%d %s", __func__, strategy, role,
            dumpAudioDeviceTypeAddrVector(devices).c_str());

    if (!areAllDevicesSupported(
            devices, audio_is_output_device, __func__, /*matchAddress*/false)) {
        return BAD_VALUE;
    }
    status_t status = mEngine->removeDevicesRoleForStrategy(strategy, role, devices);
    if (status != NO_ERROR) {
        ALOGW("Engine could not remove devices %s for strategy %d role %d",
                dumpAudioDeviceTypeAddrVector(devices).c_str(), strategy, role);
        return status;
    }

    checkForDeviceAndOutputChanges();

    bool forceVolumeReeval = false;
    // TODO(b/263479999): workaround for truncated touch sounds
    // to be removed when the problem is handled by system UI
    uint32_t delayMs = 0;
    if (strategy == mCommunnicationStrategy) {
        forceVolumeReeval = true;
        delayMs = TOUCH_SOUND_FIXED_DELAY_MS;
        updateInputRouting();
    }
    updateCallAndOutputRouting(forceVolumeReeval, delayMs);

    return NO_ERROR;
}

status_t AudioPolicyManager::clearDevicesRoleForStrategy(product_strategy_t strategy,
                                                         device_role_t role)
{
    ALOGV("%s() strategy=%d role=%d", __func__, strategy, role);

    status_t status = mEngine->clearDevicesRoleForStrategy(strategy, role);
    if (status != NO_ERROR) {
        ALOGW_IF(status != NAME_NOT_FOUND,
                "Engine could not remove device role for strategy %d status %d",
                strategy, status);
        return status;
    }

    checkForDeviceAndOutputChanges();

    bool forceVolumeReeval = false;
    // FIXME: workaround for truncated touch sounds
    // to be removed when the problem is handled by system UI
    uint32_t delayMs = 0;
    if (strategy == mCommunnicationStrategy) {
        forceVolumeReeval = true;
        delayMs = TOUCH_SOUND_FIXED_DELAY_MS;
        updateInputRouting();
    }
    updateCallAndOutputRouting(forceVolumeReeval, delayMs);

    return NO_ERROR;
}

status_t AudioPolicyManager::getDevicesForRoleAndStrategy(product_strategy_t strategy,
                                                          device_role_t role,
                                                          AudioDeviceTypeAddrVector &devices) {
    return mEngine->getDevicesForRoleAndStrategy(strategy, role, devices);
}

status_t AudioPolicyManager::setDevicesRoleForCapturePreset(
        audio_source_t audioSource, device_role_t role, const AudioDeviceTypeAddrVector &devices) {
    ALOGV("%s() audioSource=%d role=%d %s", __func__, audioSource, role,
            dumpAudioDeviceTypeAddrVector(devices).c_str());

    if (!areAllDevicesSupported(devices, audio_call_is_input_device, __func__)) {
        return BAD_VALUE;
    }
    status_t status = mEngine->setDevicesRoleForCapturePreset(audioSource, role, devices);
    ALOGW_IF(status != NO_ERROR,
            "Engine could not set preferred devices %s for audio source %d role %d",
            dumpAudioDeviceTypeAddrVector(devices).c_str(), audioSource, role);

    if (status == NO_ERROR) {
        updateInputRouting();
        updateCallRouting(false /*fromCache*/);
    }
    return status;
}

status_t AudioPolicyManager::addDevicesRoleForCapturePreset(
        audio_source_t audioSource, device_role_t role, const AudioDeviceTypeAddrVector &devices) {
    ALOGV("%s() audioSource=%d role=%d %s", __func__, audioSource, role,
            dumpAudioDeviceTypeAddrVector(devices).c_str());

    if (!areAllDevicesSupported(devices, audio_call_is_input_device, __func__)) {
        return BAD_VALUE;
    }
    status_t status = mEngine->addDevicesRoleForCapturePreset(audioSource, role, devices);
    ALOGW_IF(status != NO_ERROR,
            "Engine could not add preferred devices %s for audio source %d role %d",
            dumpAudioDeviceTypeAddrVector(devices).c_str(), audioSource, role);

    if (status == NO_ERROR) {
        updateInputRouting();
        updateCallRouting(false /*fromCache*/);
    }
    return status;
}

status_t AudioPolicyManager::removeDevicesRoleForCapturePreset(
        audio_source_t audioSource, device_role_t role, const AudioDeviceTypeAddrVector& devices)
{
    ALOGV("%s() audioSource=%d role=%d devices=%s", __func__, audioSource, role,
            dumpAudioDeviceTypeAddrVector(devices).c_str());

    if (!areAllDevicesSupported(
            devices, audio_call_is_input_device, __func__, /*matchAddress*/false)) {
        return BAD_VALUE;
    }

    status_t status = mEngine->removeDevicesRoleForCapturePreset(
            audioSource, role, devices);
    ALOGW_IF(status != NO_ERROR && status != NAME_NOT_FOUND,
            "Engine could not remove devices role (%d) for capture preset %d", role, audioSource);
    if (status == NO_ERROR) {
        updateInputRouting();
        updateCallRouting(false /*fromCache*/);
    }
    return status;
}

status_t AudioPolicyManager::clearDevicesRoleForCapturePreset(audio_source_t audioSource,
                                                              device_role_t role) {
    ALOGV("%s() audioSource=%d role=%d", __func__, audioSource, role);

    status_t status = mEngine->clearDevicesRoleForCapturePreset(audioSource, role);
    ALOGW_IF(status != NO_ERROR && status != NAME_NOT_FOUND,
            "Engine could not clear devices role (%d) for capture preset %d", role, audioSource);
    if (status == NO_ERROR) {
        updateInputRouting();
        updateCallRouting(false /*fromCache*/);
    }
    return status;
}

status_t AudioPolicyManager::getDevicesForRoleAndCapturePreset(
        audio_source_t audioSource, device_role_t role, AudioDeviceTypeAddrVector &devices) {
    return mEngine->getDevicesForRoleAndCapturePreset(audioSource, role, devices);
}

status_t AudioPolicyManager::setUserIdDeviceAffinities(int userId,
        const AudioDeviceTypeAddrVector& devices) {
    ALOGV("%s() userId=%d num devices %zu", __func__, userId, devices.size());
    if (!areAllDevicesSupported(devices, audio_is_output_device, __func__)) {
        return BAD_VALUE;
    }
    status_t status =  mPolicyMixes.setUserIdDeviceAffinities(userId, devices);
    if (status != NO_ERROR) {
        ALOGE("%s() could not set device affinity for userId %d",
            __FUNCTION__, userId);
        return status;
    }

    // reevaluate outputs for all devices
    checkForDeviceAndOutputChanges();
    changeOutputDevicesMuteState(devices);
    updateCallAndOutputRouting(false /* forceVolumeReeval */, 0 /* delayMs */,
        true /* skipDelays */);
    changeOutputDevicesMuteState(devices);

    return NO_ERROR;
}

status_t AudioPolicyManager::removeUserIdDeviceAffinities(int userId) {
    ALOGV("%s() userId=%d", __FUNCTION__, userId);
    AudioDeviceTypeAddrVector devices;
    mPolicyMixes.getDevicesForUserId(userId, devices);
    status_t status = mPolicyMixes.removeUserIdDeviceAffinities(userId);
    if (status != NO_ERROR) {
        ALOGE("%s() Could not remove all device affinities fo userId = %d",
            __FUNCTION__, userId);
        return status;
    }

    // reevaluate outputs for all devices
    checkForDeviceAndOutputChanges();
    changeOutputDevicesMuteState(devices);
    updateCallAndOutputRouting(false /* forceVolumeReeval */, 0 /* delayMs */,
        true /* skipDelays */);
    changeOutputDevicesMuteState(devices);

    return NO_ERROR;
}

void AudioPolicyManager::dump(String8 *dst) const
{
    dst->appendFormat("\nAudioPolicyManager Dump: %p\n", this);
    dst->appendFormat(" Primary Output I/O handle: %d\n",
             hasPrimaryOutput() ? mPrimaryOutput->mIoHandle : AUDIO_IO_HANDLE_NONE);
    std::string stateLiteral;
    AudioModeConverter::toString(mEngine->getPhoneState(), stateLiteral);
    dst->appendFormat(" Phone state: %s\n", stateLiteral.c_str());
    const char* forceUses[AUDIO_POLICY_FORCE_USE_CNT] = {
        "communications", "media", "record", "dock", "system",
        "HDMI system audio", "encoded surround output", "vibrate ringing" };
    for (audio_policy_force_use_t i = AUDIO_POLICY_FORCE_FOR_COMMUNICATION;
         i < AUDIO_POLICY_FORCE_USE_CNT; i = (audio_policy_force_use_t)((int)i + 1)) {
        audio_policy_forced_cfg_t forceUseValue = mEngine->getForceUse(i);
        dst->appendFormat(" Force use for %s: %d", forceUses[i], forceUseValue);
        if (i == AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND &&
                forceUseValue == AUDIO_POLICY_FORCE_ENCODED_SURROUND_MANUAL) {
            dst->append(" (MANUAL: ");
            dumpManualSurroundFormats(dst);
            dst->append(")");
        }
        dst->append("\n");
    }
    dst->appendFormat(" TTS output %savailable\n", mTtsOutputAvailable ? "" : "not ");
    dst->appendFormat(" Master mono: %s\n", mMasterMono ? "on" : "off");
    dst->appendFormat(" Communication Strategy id: %d\n", mCommunnicationStrategy);
    dst->appendFormat(" Config source: %s\n", mConfig->getSource().c_str());

    dst->append("\n");
    mAvailableOutputDevices.dump(dst, String8("Available output"), 1);
    dst->append("\n");
    mAvailableInputDevices.dump(dst, String8("Available input"), 1);
    mHwModules.dump(dst);
    mOutputs.dump(dst);
    mInputs.dump(dst);
    mEffects.dump(dst, 1);
    mAudioPatches.dump(dst);
    mPolicyMixes.dump(dst);
    mAudioSources.dump(dst);

    dst->appendFormat(" AllowedCapturePolicies:\n");
    for (auto& policy : mAllowedCapturePolicies) {
        dst->appendFormat("   - uid=%d flag_mask=%#x\n", policy.first, policy.second);
    }

    dst->appendFormat(" Preferred mixer audio configuration:\n");
    for (const auto it : mPreferredMixerAttrInfos) {
        dst->appendFormat("   - device port id: %d\n", it.first);
        for (const auto preferredMixerInfoIt : it.second) {
            dst->appendFormat("     - strategy: %d; ", preferredMixerInfoIt.first);
            preferredMixerInfoIt.second->dump(dst);
        }
    }

    dst->appendFormat("\nPolicy Engine dump:\n");
    mEngine->dump(dst);

    dst->appendFormat("\nAbsolute volume devices with driving streams:\n");
    for (const auto it : mAbsoluteVolumeDrivingStreams) {
        dst->appendFormat("   - device type: %s, driving stream %d\n",
                          dumpDeviceTypes({it.first}).c_str(),
                          mEngine->getVolumeGroupForAttributes(it.second));
    }

    // dump mmap policy by device
    dst->appendFormat("\nMmap policy:\n");
    for (const auto& [policyType, policyByDevice] : mMmapPolicyByDeviceType) {
        std::stringstream ss;
        ss << '{';
        for (const auto& [deviceType, policy] : policyByDevice) {
            ss << deviceType.toString() << ":" << toString(policy) << " ";
        }
        ss << '}';
        dst->appendFormat(" - %s: %s\n", toString(policyType).c_str(), ss.str().c_str());
    }
}

status_t AudioPolicyManager::dump(int fd)
{
    String8 result;
    dump(&result);
    write(fd, result.c_str(), result.size());
    return NO_ERROR;
}

status_t AudioPolicyManager::setAllowedCapturePolicy(uid_t uid, audio_flags_mask_t capturePolicy)
{
    mAllowedCapturePolicies[uid] = capturePolicy;
    return NO_ERROR;
}

// This function checks for the parameters which can be offloaded.
// This can be enhanced depending on the capability of the DSP and policy
// of the system.
audio_offload_mode_t AudioPolicyManager::getOffloadSupport(const audio_offload_info_t& offloadInfo)
{
    ALOGV("%s: SR=%u, CM=0x%x, Format=0x%x, StreamType=%d,"
     " BitRate=%u, duration=%" PRId64 " us, has_video=%d",
     __func__, offloadInfo.sample_rate, offloadInfo.channel_mask,
     offloadInfo.format,
     offloadInfo.stream_type, offloadInfo.bit_rate, offloadInfo.duration_us,
     offloadInfo.has_video);

    if (!isOffloadPossible(offloadInfo)) {
        return AUDIO_OFFLOAD_NOT_SUPPORTED;
    }

    // See if there is a profile to support this.
    // AUDIO_DEVICE_NONE
    sp<IOProfile> profile = getProfileForOutput(DeviceVector() /*ignore device */,
                                            offloadInfo.sample_rate,
                                            offloadInfo.format,
                                            offloadInfo.channel_mask,
                                            AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD,
                                            true /* directOnly */);
    ALOGV("%s: profile %sfound%s", __func__, profile != nullptr ? "" : "NOT ",
            (profile != nullptr && (profile->getFlags() & AUDIO_OUTPUT_FLAG_GAPLESS_OFFLOAD) != 0)
            ? ", supports gapless" : "");
    if (profile == nullptr) {
        return AUDIO_OFFLOAD_NOT_SUPPORTED;
    }
    if ((profile->getFlags() & AUDIO_OUTPUT_FLAG_GAPLESS_OFFLOAD) != 0) {
        return AUDIO_OFFLOAD_GAPLESS_SUPPORTED;
    }
    return AUDIO_OFFLOAD_SUPPORTED;
}

bool AudioPolicyManager::isDirectOutputSupported(const audio_config_base_t& config,
                                                 const audio_attributes_t& attributes) {
    audio_output_flags_t output_flags = AUDIO_OUTPUT_FLAG_NONE;
    audio_flags_to_audio_output_flags(attributes.flags, &output_flags);
    DeviceVector outputDevices = mEngine->getOutputDevicesForAttributes(attributes);
    sp<IOProfile> profile = getProfileForOutput(outputDevices,
                                            config.sample_rate,
                                            config.format,
                                            config.channel_mask,
                                            output_flags,
                                            true /* directOnly */);
    ALOGV("%s() profile %sfound with name: %s, "
        "sample rate: %u, format: 0x%x, channel_mask: 0x%x, output flags: 0x%x",
        __FUNCTION__, profile != 0 ? "" : "NOT ",
        (profile != 0 ? profile->getTagName().c_str() : "null"),
        config.sample_rate, config.format, config.channel_mask, output_flags);

    // also try the MSD module if compatible profile not found
    if (profile == nullptr) {
        profile = getMsdProfileForOutput(outputDevices,
                                              config.sample_rate,
                                              config.format,
                                              config.channel_mask,
                                              output_flags,
                                              true /* directOnly */);
        ALOGV("%s() MSD profile %sfound with name: %s, "
            "sample rate: %u, format: 0x%x, channel_mask: 0x%x, output flags: 0x%x",
            __FUNCTION__, profile != 0 ? "" : "NOT ",
            (profile != 0 ? profile->getTagName().c_str() : "null"),
            config.sample_rate, config.format, config.channel_mask, output_flags);
    }
    return (profile != nullptr);
}

bool AudioPolicyManager::isOffloadPossible(const audio_offload_info_t &offloadInfo,
                                           bool durationIgnored) {
    if (mMasterMono) {
        return false; // no offloading if mono is set.
    }

    // Check if offload has been disabled
    if (property_get_bool("audio.offload.disable", false /* default_value */)) {
        ALOGV("%s: offload disabled by audio.offload.disable", __func__);
        return false;
    }

    // Check if stream type is music, then only allow offload as of now.
    if (offloadInfo.stream_type != AUDIO_STREAM_MUSIC)
    {
        ALOGV("%s: stream_type != MUSIC, returning false", __func__);
        return false;
    }

    //TODO: enable audio offloading with video when ready
    const bool allowOffloadWithVideo =
            property_get_bool("audio.offload.video", false /* default_value */);
    if (offloadInfo.has_video && !allowOffloadWithVideo) {
        ALOGV("%s: has_video == true, returning false", __func__);
        return false;
    }

    //If duration is less than minimum value defined in property, return false
    const int min_duration_secs = property_get_int32(
            "audio.offload.min.duration.secs", -1 /* default_value */);
    if (!durationIgnored) {
        if (min_duration_secs >= 0) {
            if (offloadInfo.duration_us < min_duration_secs * 1000000LL) {
                ALOGV("%s: Offload denied by duration < audio.offload.min.duration.secs(=%d)",
                      __func__, min_duration_secs);
                return false;
            }
        } else if (offloadInfo.duration_us < OFFLOAD_DEFAULT_MIN_DURATION_SECS * 1000000) {
            ALOGV("%s: Offload denied by duration < default min(=%u)",
                  __func__, OFFLOAD_DEFAULT_MIN_DURATION_SECS);
            return false;
        }
    }

    // Do not allow offloading if one non offloadable effect is enabled. This prevents from
    // creating an offloaded track and tearing it down immediately after start when audioflinger
    // detects there is an active non offloadable effect.
    // FIXME: We should check the audio session here but we do not have it in this context.
    // This may prevent offloading in rare situations where effects are left active by apps
    // in the background.
    if (mEffects.isNonOffloadableEffectEnabled()) {
        return false;
    }

    return true;
}

audio_direct_mode_t AudioPolicyManager::getDirectPlaybackSupport(const audio_attributes_t *attr,
                                                                 const audio_config_t *config) {
    audio_offload_info_t offloadInfo = AUDIO_INFO_INITIALIZER;
    offloadInfo.format = config->format;
    offloadInfo.sample_rate = config->sample_rate;
    offloadInfo.channel_mask = config->channel_mask;
    offloadInfo.stream_type = mEngine->getStreamTypeForAttributes(*attr);
    offloadInfo.has_video = false;
    offloadInfo.is_streaming = false;
    const bool offloadPossible = isOffloadPossible(offloadInfo, true /*durationIgnored*/);

    audio_direct_mode_t directMode = AUDIO_DIRECT_NOT_SUPPORTED;
    audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
    audio_flags_to_audio_output_flags(attr->flags, &flags);
    // only retain flags that will drive compressed offload or passthrough
    uint32_t relevantFlags = AUDIO_OUTPUT_FLAG_HW_AV_SYNC;
    if (offloadPossible) {
        relevantFlags |= AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD;
    }
    flags = (audio_output_flags_t)((flags & relevantFlags) | AUDIO_OUTPUT_FLAG_DIRECT);

    DeviceVector engineOutputDevices = mEngine->getOutputDevicesForAttributes(*attr);
    if (std::any_of(engineOutputDevices.begin(), engineOutputDevices.end(),
            [this, attr](sp<DeviceDescriptor> device) {
                    return getPreferredMixerAttributesInfo(
                            device->getId(),
                            mEngine->getProductStrategyForAttributes(*attr),
                            true /*activeBitPerfectPreferred*/) != nullptr;
            })) {
        // Bit-perfect playback is active on one of the selected devices, direct output will
        // be rejected at this instant.
        return AUDIO_DIRECT_NOT_SUPPORTED;
    }
    for (const auto& hwModule : mHwModules) {
        DeviceVector outputDevices = engineOutputDevices;
        // the MSD module checks for different conditions and output devices
        if (strcmp(hwModule->getName(), AUDIO_HARDWARE_MODULE_ID_MSD) == 0) {
            if (!msdHasPatchesToAllDevices(engineOutputDevices.toTypeAddrVector())) {
                continue;
            }
            outputDevices = getMsdAudioOutDevices();
        }
        for (const auto& curProfile : hwModule->getOutputProfiles()) {
            if (curProfile->getCompatibilityScore(outputDevices,
                    config->sample_rate, nullptr /*updatedSamplingRate*/,
                    config->format, nullptr /*updatedFormat*/,
                    config->channel_mask, nullptr /*updatedChannelMask*/,
                    flags) == IOProfile::NO_MATCH) {
                continue;
            }
            // reject profiles not corresponding to a device currently available
            if (!mAvailableOutputDevices.containsAtLeastOne(curProfile->getSupportedDevices())) {
                continue;
            }
            if (offloadPossible && ((curProfile->getFlags() & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
                        != AUDIO_OUTPUT_FLAG_NONE)) {
                if ((directMode & AUDIO_DIRECT_OFFLOAD_GAPLESS_SUPPORTED)
                        != AUDIO_DIRECT_NOT_SUPPORTED) {
                    // Already reports offload gapless supported. No need to report offload support.
                    continue;
                }
                if ((curProfile->getFlags() & AUDIO_OUTPUT_FLAG_GAPLESS_OFFLOAD)
                        != AUDIO_OUTPUT_FLAG_NONE) {
                    // If offload gapless is reported, no need to report offload support.
                    directMode = (audio_direct_mode_t) ((directMode &
                            ~AUDIO_DIRECT_OFFLOAD_SUPPORTED) |
                            AUDIO_DIRECT_OFFLOAD_GAPLESS_SUPPORTED);
                } else {
                    directMode = (audio_direct_mode_t)(directMode | AUDIO_DIRECT_OFFLOAD_SUPPORTED);
                }
            } else {
                directMode = (audio_direct_mode_t) (directMode | AUDIO_DIRECT_BITSTREAM_SUPPORTED);
            }
        }
    }
    return directMode;
}

status_t AudioPolicyManager::getDirectProfilesForAttributes(const audio_attributes_t* attr,
                                                AudioProfileVector& audioProfilesVector) {
    if (mEffects.isNonOffloadableEffectEnabled()) {
        return OK;
    }
    DeviceVector devices;
    status_t status = getDevicesForAttributes(*attr, devices, false /* forVolume */);
    if (status != OK) {
        return status;
    }
    ALOGV("%s: found %zu output devices for attributes.", __func__, devices.size());
    if (devices.empty()) {
        return OK; // no output devices for the attributes
    }
    return getProfilesForDevices(devices, audioProfilesVector,
                                 AUDIO_OUTPUT_FLAG_DIRECT /*flags*/, false /*isInput*/);
}

status_t AudioPolicyManager::getSupportedMixerAttributes(
        audio_port_handle_t portId, std::vector<audio_mixer_attributes_t> &mixerAttrs) {
    ALOGV("%s, portId=%d", __func__, portId);
    sp<DeviceDescriptor> deviceDescriptor = mAvailableOutputDevices.getDeviceFromId(portId);
    if (deviceDescriptor == nullptr) {
        ALOGE("%s the requested device is currently unavailable", __func__);
        return BAD_VALUE;
    }
    if (!audio_is_usb_out_device(deviceDescriptor->type())) {
        ALOGE("%s the requested device(type=%#x) is not usb device", __func__,
              deviceDescriptor->type());
        return BAD_VALUE;
    }
    for (const auto& hwModule : mHwModules) {
        for (const auto& curProfile : hwModule->getOutputProfiles()) {
            if (curProfile->supportsDevice(deviceDescriptor)) {
                curProfile->toSupportedMixerAttributes(&mixerAttrs);
            }
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::setPreferredMixerAttributes(
        const audio_attributes_t *attr,
        audio_port_handle_t portId,
        uid_t uid,
        const audio_mixer_attributes_t *mixerAttributes) {
    ALOGV("%s, attr=%s, mixerAttributes={format=%#x, channelMask=%#x, samplingRate=%u, "
          "mixerBehavior=%d}, uid=%d, portId=%u",
          __func__, toString(*attr).c_str(), mixerAttributes->config.format,
          mixerAttributes->config.channel_mask, mixerAttributes->config.sample_rate,
          mixerAttributes->mixer_behavior, uid, portId);
    if (attr->usage != AUDIO_USAGE_MEDIA) {
        ALOGE("%s failed, only media is allowed, the given usage is %d", __func__, attr->usage);
        return BAD_VALUE;
    }
    sp<DeviceDescriptor> deviceDescriptor = mAvailableOutputDevices.getDeviceFromId(portId);
    if (deviceDescriptor == nullptr) {
        ALOGE("%s the requested device is currently unavailable", __func__);
        return BAD_VALUE;
    }
    if (!audio_is_usb_out_device(deviceDescriptor->type())) {
        ALOGE("%s(%d), type=%d, is not a usb output device",
              __func__, portId, deviceDescriptor->type());
        return BAD_VALUE;
    }

    audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
    audio_flags_to_audio_output_flags(attr->flags, &flags);
    flags = (audio_output_flags_t) (flags |
            audio_output_flags_from_mixer_behavior(mixerAttributes->mixer_behavior));
    sp<IOProfile> profile = nullptr;
    DeviceVector devices(deviceDescriptor);
    for (const auto& hwModule : mHwModules) {
        for (const auto& curProfile : hwModule->getOutputProfiles()) {
            if (curProfile->hasDynamicAudioProfile()
                    && curProfile->getCompatibilityScore(
                            devices,
                            mixerAttributes->config.sample_rate,
                            nullptr /*updatedSamplingRate*/,
                            mixerAttributes->config.format,
                            nullptr /*updatedFormat*/,
                            mixerAttributes->config.channel_mask,
                            nullptr /*updatedChannelMask*/,
                            flags)
                            != IOProfile::NO_MATCH) {
                profile = curProfile;
                break;
            }
        }
    }
    if (profile == nullptr) {
        ALOGE("%s, there is no compatible profile found", __func__);
        return BAD_VALUE;
    }

    sp<PreferredMixerAttributesInfo> mixerAttrInfo =
            sp<PreferredMixerAttributesInfo>::make(
                    uid, portId, profile, flags, *mixerAttributes);
    const product_strategy_t strategy = mEngine->getProductStrategyForAttributes(*attr);
    mPreferredMixerAttrInfos[portId][strategy] = mixerAttrInfo;

    // If 1) there is any client from the preferred mixer configuration owner that is currently
    // active and matches the strategy and 2) current output is on the preferred device and the
    // mixer configuration doesn't match the preferred one, reopen output with preferred mixer
    // configuration.
    std::vector<audio_io_handle_t> outputsToReopen;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        const auto output = mOutputs.valueAt(i);
        if (output->mProfile == profile && output->devices().onlyContainsDevice(deviceDescriptor)) {
            if (output->isConfigurationMatched(mixerAttributes->config, flags)) {
                output->mPreferredAttrInfo = mixerAttrInfo;
            } else {
                for (const auto &client: output->getActiveClients()) {
                    if (client->uid() == uid && client->strategy() == strategy) {
                        client->setIsInvalid();
                        outputsToReopen.push_back(output->mIoHandle);
                    }
                }
            }
        }
    }
    audio_config_t config = AUDIO_CONFIG_INITIALIZER;
    config.sample_rate = mixerAttributes->config.sample_rate;
    config.channel_mask = mixerAttributes->config.channel_mask;
    config.format = mixerAttributes->config.format;
    for (const auto output : outputsToReopen) {
        sp<SwAudioOutputDescriptor> desc =
                reopenOutput(mOutputs.valueFor(output), &config, flags, __func__);
        if (desc == nullptr) {
            ALOGE("%s, failed to reopen output with preferred mixer attributes", __func__);
            continue;
        }
        desc->mPreferredAttrInfo = mixerAttrInfo;
    }

    return NO_ERROR;
}

sp<PreferredMixerAttributesInfo> AudioPolicyManager::getPreferredMixerAttributesInfo(
        audio_port_handle_t devicePortId,
        product_strategy_t strategy,
        bool activeBitPerfectPreferred) {
    auto it = mPreferredMixerAttrInfos.find(devicePortId);
    if (it == mPreferredMixerAttrInfos.end()) {
        return nullptr;
    }
    if (activeBitPerfectPreferred) {
        for (auto [strategy, info] : it->second) {
            if (info->isBitPerfect() && info->getActiveClientCount() != 0) {
                return info;
            }
        }
    }
    auto strategyMatchedMixerAttrInfoIt = it->second.find(strategy);
    return strategyMatchedMixerAttrInfoIt == it->second.end()
            ? nullptr : strategyMatchedMixerAttrInfoIt->second;
}

status_t AudioPolicyManager::getPreferredMixerAttributes(
        const audio_attributes_t *attr,
        audio_port_handle_t portId,
        audio_mixer_attributes_t* mixerAttributes) {
    sp<PreferredMixerAttributesInfo> info = getPreferredMixerAttributesInfo(
            portId, mEngine->getProductStrategyForAttributes(*attr));
    if (info == nullptr) {
        return NAME_NOT_FOUND;
    }
    *mixerAttributes = info->getMixerAttributes();
    return NO_ERROR;
}

status_t AudioPolicyManager::clearPreferredMixerAttributes(const audio_attributes_t *attr,
                                                           audio_port_handle_t portId,
                                                           uid_t uid) {
    const product_strategy_t strategy = mEngine->getProductStrategyForAttributes(*attr);
    const auto preferredMixerAttrInfo = getPreferredMixerAttributesInfo(portId, strategy);
    if (preferredMixerAttrInfo == nullptr) {
        return NAME_NOT_FOUND;
    }
    if (preferredMixerAttrInfo->getUid() != uid) {
        ALOGE("%s, requested uid=%d, owned uid=%d",
              __func__, uid, preferredMixerAttrInfo->getUid());
        return PERMISSION_DENIED;
    }
    mPreferredMixerAttrInfos[portId].erase(strategy);
    if (mPreferredMixerAttrInfos[portId].empty()) {
        mPreferredMixerAttrInfos.erase(portId);
    }

    // Reconfig existing output
    std::vector<audio_io_handle_t> potentialOutputsToReopen;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        if (mOutputs.valueAt(i)->mProfile == preferredMixerAttrInfo->getProfile()) {
            potentialOutputsToReopen.push_back(mOutputs.keyAt(i));
        }
    }
    for (const auto output : potentialOutputsToReopen) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueFor(output);
        if (desc->isConfigurationMatched(preferredMixerAttrInfo->getConfigBase(),
                                         preferredMixerAttrInfo->getFlags())) {
            reopenOutput(desc, nullptr /*config*/, AUDIO_OUTPUT_FLAG_NONE, __func__);
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::listAudioPorts(audio_port_role_t role,
                                            audio_port_type_t type,
                                            unsigned int *num_ports,
                                            struct audio_port_v7 *ports,
                                            unsigned int *generation)
{
    if (num_ports == nullptr || (*num_ports != 0 && ports == nullptr) ||
            generation == nullptr) {
        return BAD_VALUE;
    }
    ALOGV("listAudioPorts() role %d type %d num_ports %d ports %p", role, type, *num_ports, ports);
    if (ports == nullptr) {
        *num_ports = 0;
    }

    size_t portsWritten = 0;
    size_t portsMax = *num_ports;
    *num_ports = 0;
    if (type == AUDIO_PORT_TYPE_NONE || type == AUDIO_PORT_TYPE_DEVICE) {
        // do not report devices with type AUDIO_DEVICE_IN_STUB or AUDIO_DEVICE_OUT_STUB
        // as they are used by stub HALs by convention
        if (role == AUDIO_PORT_ROLE_SINK || role == AUDIO_PORT_ROLE_NONE) {
            for (const auto& dev : mAvailableOutputDevices) {
                if (dev->type() == AUDIO_DEVICE_OUT_STUB) {
                    continue;
                }
                if (portsWritten < portsMax) {
                    dev->toAudioPort(&ports[portsWritten++]);
                }
                (*num_ports)++;
            }
        }
        if (role == AUDIO_PORT_ROLE_SOURCE || role == AUDIO_PORT_ROLE_NONE) {
            for (const auto& dev : mAvailableInputDevices) {
                if (dev->type() == AUDIO_DEVICE_IN_STUB) {
                    continue;
                }
                if (portsWritten < portsMax) {
                    dev->toAudioPort(&ports[portsWritten++]);
                }
                (*num_ports)++;
            }
        }
    }
    if (type == AUDIO_PORT_TYPE_NONE || type == AUDIO_PORT_TYPE_MIX) {
        if (role == AUDIO_PORT_ROLE_SINK || role == AUDIO_PORT_ROLE_NONE) {
            for (size_t i = 0; i < mInputs.size() && portsWritten < portsMax; i++) {
                mInputs[i]->toAudioPort(&ports[portsWritten++]);
            }
            *num_ports += mInputs.size();
        }
        if (role == AUDIO_PORT_ROLE_SOURCE || role == AUDIO_PORT_ROLE_NONE) {
            size_t numOutputs = 0;
            for (size_t i = 0; i < mOutputs.size(); i++) {
                if (!mOutputs[i]->isDuplicated()) {
                    numOutputs++;
                    if (portsWritten < portsMax) {
                        mOutputs[i]->toAudioPort(&ports[portsWritten++]);
                    }
                }
            }
            *num_ports += numOutputs;
        }
    }

    *generation = curAudioPortGeneration();
    ALOGV("listAudioPorts() got %zu ports needed %d", portsWritten, *num_ports);
    return NO_ERROR;
}

status_t AudioPolicyManager::listDeclaredDevicePorts(media::AudioPortRole role,
        std::vector<media::AudioPortFw>* _aidl_return) {
    auto pushPort = [&](const sp<DeviceDescriptor>& dev) -> status_t {
        audio_port_v7 port;
        dev->toAudioPort(&port);
        auto aidlPort = VALUE_OR_RETURN_STATUS(legacy2aidl_audio_port_v7_AudioPortFw(port));
        _aidl_return->push_back(std::move(aidlPort));
        return OK;
    };

    for (const auto& module : mHwModules) {
        for (const auto& dev : module->getDeclaredDevices()) {
            if (role == media::AudioPortRole::NONE ||
                    ((role == media::AudioPortRole::SOURCE)
                            == audio_is_input_device(dev->type()))) {
                RETURN_STATUS_IF_ERROR(pushPort(dev));
            }
        }
    }
    return OK;
}

status_t AudioPolicyManager::getAudioPort(struct audio_port_v7 *port)
{
    if (port == nullptr || port->id == AUDIO_PORT_HANDLE_NONE) {
        return BAD_VALUE;
    }
    sp<DeviceDescriptor> dev = mAvailableOutputDevices.getDeviceFromId(port->id);
    if (dev != 0) {
        dev->toAudioPort(port);
        return NO_ERROR;
    }
    dev = mAvailableInputDevices.getDeviceFromId(port->id);
    if (dev != 0) {
        dev->toAudioPort(port);
        return NO_ERROR;
    }
    sp<SwAudioOutputDescriptor> out = mOutputs.getOutputFromId(port->id);
    if (out != 0) {
        out->toAudioPort(port);
        return NO_ERROR;
    }
    sp<AudioInputDescriptor> in = mInputs.getInputFromId(port->id);
    if (in != 0) {
        in->toAudioPort(port);
        return NO_ERROR;
    }
    return BAD_VALUE;
}

status_t AudioPolicyManager::createAudioPatch(const struct audio_patch *patch,
                                              audio_patch_handle_t *handle,
                                              uid_t uid)
{
    ALOGV("%s", __func__);
    if (handle == NULL || patch == NULL) {
        return BAD_VALUE;
    }
    ALOGV("%s num sources %d num sinks %d", __func__, patch->num_sources, patch->num_sinks);
    if (!audio_patch_is_valid(patch)) {
        return BAD_VALUE;
    }
    // only one source per audio patch supported for now
    if (patch->num_sources > 1) {
        return INVALID_OPERATION;
    }
    if (patch->sources[0].role != AUDIO_PORT_ROLE_SOURCE) {
        return INVALID_OPERATION;
    }
    for (size_t i = 0; i < patch->num_sinks; i++) {
        if (patch->sinks[i].role != AUDIO_PORT_ROLE_SINK) {
            return INVALID_OPERATION;
        }
    }

    sp<DeviceDescriptor> srcDevice = mAvailableInputDevices.getDeviceFromId(patch->sources[0].id);
    sp<DeviceDescriptor> sinkDevice = mAvailableOutputDevices.getDeviceFromId(patch->sinks[0].id);
    if (srcDevice == nullptr || sinkDevice == nullptr) {
        ALOGW("%s could not create patch, invalid sink and/or source device(s)", __func__);
        return BAD_VALUE;
    }
    ALOGV("%s between source %s and sink %s", __func__,
            srcDevice->toString().c_str(), sinkDevice->toString().c_str());
    audio_port_handle_t portId = PolicyAudioPort::getNextUniqueId();
    // Default attributes, default volume priority, not to infer with non raw audio patches.
    audio_attributes_t attributes = attributes_initializer(AUDIO_USAGE_MEDIA);
    const struct audio_port_config *source = &patch->sources[0];
    sp<SourceClientDescriptor> sourceDesc =
            new SourceClientDescriptor(
                portId, uid, attributes, *source, srcDevice, AUDIO_STREAM_PATCH,
                mEngine->getProductStrategyForAttributes(attributes), toVolumeSource(attributes),
                true, false /*isCallRx*/, false /*isCallTx*/);
    sourceDesc->setPreferredDeviceId(sinkDevice->getId());

    status_t status =
            connectAudioSourceToSink(sourceDesc, sinkDevice, patch, *handle, uid, 0 /* delayMs */);

    if (status != NO_ERROR) {
        return INVALID_OPERATION;
    }
    mAudioSources.add(portId, sourceDesc);
    return NO_ERROR;
}

status_t AudioPolicyManager::connectAudioSourceToSink(
        const sp<SourceClientDescriptor>& sourceDesc, const sp<DeviceDescriptor> &sinkDevice,
        const struct audio_patch *patch,
        audio_patch_handle_t &handle,
        uid_t uid, uint32_t delayMs)
{
    status_t status = createAudioPatchInternal(patch, &handle, uid, delayMs, sourceDesc);
    if (status != NO_ERROR || mAudioPatches.indexOfKey(handle) < 0) {
        ALOGW("%s patch panel could not connect device patch, error %d", __func__, status);
        return INVALID_OPERATION;
    }
    sourceDesc->connect(handle, sinkDevice);
    if (isMsdPatch(handle)) {
        return NO_ERROR;
    }
    // SW Bridge? (@todo: HW bridge, keep track of HwOutput for device selection "reconsideration")
    sp<SwAudioOutputDescriptor> swOutput = sourceDesc->swOutput().promote();
    ALOG_ASSERT(swOutput != nullptr, "%s: a swOutput shall always be associated", __func__);
    if (swOutput->getClient(sourceDesc->portId()) != nullptr) {
        ALOGW("%s source portId has already been attached to outputDesc", __func__);
        goto FailurePatchAdded;
    }
    status = swOutput->start();
    if (status != NO_ERROR) {
        goto FailureSourceAdded;
    }
    swOutput->addClient(sourceDesc);
    status = startSource(swOutput, sourceDesc, &delayMs);
    if (status != NO_ERROR) {
        ALOGW("%s failed to start source, error %d", __FUNCTION__, status);
        goto FailureSourceActive;
    }
    if (delayMs != 0) {
        usleep(delayMs * 1000);
    }
    return NO_ERROR;

FailureSourceActive:
    swOutput->stop();
    releaseOutput(sourceDesc->portId());
FailureSourceAdded:
    sourceDesc->setSwOutput(nullptr);
FailurePatchAdded:
    releaseAudioPatchInternal(handle);
    return INVALID_OPERATION;
}

status_t AudioPolicyManager::createAudioPatchInternal(const struct audio_patch *patch,
                                                      audio_patch_handle_t *handle,
                                                      uid_t uid, uint32_t delayMs,
                                                      const sp<SourceClientDescriptor>& sourceDesc)
{
    ALOGV("%s num sources %d num sinks %d", __func__, patch->num_sources, patch->num_sinks);
    sp<AudioPatch> patchDesc;
    ssize_t index = mAudioPatches.indexOfKey(*handle);

    ALOGV("%s source id %d role %d type %d", __func__, patch->sources[0].id,
                                                       patch->sources[0].role,
                                                       patch->sources[0].type);
#if LOG_NDEBUG == 0
    for (size_t i = 0; i < patch->num_sinks; i++) {
        ALOGV("%s sink %zu: id %d role %d type %d", __func__ ,i, patch->sinks[i].id,
                                                                 patch->sinks[i].role,
                                                                 patch->sinks[i].type);
    }
#endif

    if (index >= 0) {
        patchDesc = mAudioPatches.valueAt(index);
        ALOGV("%s mUidCached %d patchDesc->mUid %d uid %d",
              __func__, mUidCached, patchDesc->getUid(), uid);
        if (patchDesc->getUid() != mUidCached && uid != patchDesc->getUid()) {
            return INVALID_OPERATION;
        }
    } else {
        *handle = AUDIO_PATCH_HANDLE_NONE;
    }

    if (patch->sources[0].type == AUDIO_PORT_TYPE_MIX) {
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.getOutputFromId(patch->sources[0].id);
        if (outputDesc == NULL) {
            ALOGV("%s output not found for id %d", __func__, patch->sources[0].id);
            return BAD_VALUE;
        }
        ALOG_ASSERT(!outputDesc->isDuplicated(),"duplicated output %d in source in ports",
                                                outputDesc->mIoHandle);
        if (patchDesc != 0) {
            if (patchDesc->mPatch.sources[0].id != patch->sources[0].id) {
                ALOGV("%s source id differs for patch current id %d new id %d",
                      __func__, patchDesc->mPatch.sources[0].id, patch->sources[0].id);
                return BAD_VALUE;
            }
        }
        DeviceVector devices;
        for (size_t i = 0; i < patch->num_sinks; i++) {
            // Only support mix to devices connection
            // TODO add support for mix to mix connection
            if (patch->sinks[i].type != AUDIO_PORT_TYPE_DEVICE) {
                ALOGV("%s source mix but sink is not a device", __func__);
                return INVALID_OPERATION;
            }
            sp<DeviceDescriptor> devDesc =
                    mAvailableOutputDevices.getDeviceFromId(patch->sinks[i].id);
            if (devDesc == 0) {
                ALOGV("%s out device not found for id %d", __func__, patch->sinks[i].id);
                return BAD_VALUE;
            }

            if (outputDesc->mProfile->getCompatibilityScore(
                    DeviceVector(devDesc),
                    patch->sources[0].sample_rate,
                    nullptr,  // updatedSamplingRate
                    patch->sources[0].format,
                    nullptr,  // updatedFormat
                    patch->sources[0].channel_mask,
                    nullptr,  // updatedChannelMask
                    AUDIO_OUTPUT_FLAG_NONE /*FIXME*/) == IOProfile::NO_MATCH) {
                ALOGV("%s profile not supported for device %08x", __func__, devDesc->type());
                return INVALID_OPERATION;
            }
            devices.add(devDesc);
        }
        if (devices.size() == 0) {
            return INVALID_OPERATION;
        }

        // TODO: reconfigure output format and channels here
        ALOGV("%s setting device %s on output %d",
              __func__, dumpDeviceTypes(devices.types()).c_str(), outputDesc->mIoHandle);
        setOutputDevices(__func__, outputDesc, devices, true, 0, handle);
        index = mAudioPatches.indexOfKey(*handle);
        if (index >= 0) {
            if (patchDesc != 0 && patchDesc != mAudioPatches.valueAt(index)) {
                ALOGW("%s setOutputDevice() did not reuse the patch provided", __func__);
            }
            patchDesc = mAudioPatches.valueAt(index);
            patchDesc->setUid(uid);
            ALOGV("%s success", __func__);
        } else {
            ALOGW("%s setOutputDevice() failed to create a patch", __func__);
            return INVALID_OPERATION;
        }
    } else if (patch->sources[0].type == AUDIO_PORT_TYPE_DEVICE) {
        if (patch->sinks[0].type == AUDIO_PORT_TYPE_MIX) {
            // input device to input mix connection
            // only one sink supported when connecting an input device to a mix
            if (patch->num_sinks > 1) {
                return INVALID_OPERATION;
            }
            sp<AudioInputDescriptor> inputDesc = mInputs.getInputFromId(patch->sinks[0].id);
            if (inputDesc == NULL) {
                return BAD_VALUE;
            }
            if (patchDesc != 0) {
                if (patchDesc->mPatch.sinks[0].id != patch->sinks[0].id) {
                    return BAD_VALUE;
                }
            }
            sp<DeviceDescriptor> device =
                    mAvailableInputDevices.getDeviceFromId(patch->sources[0].id);
            if (device == 0) {
                return BAD_VALUE;
            }

            if (inputDesc->mProfile->getCompatibilityScore(
                    DeviceVector(device),
                    patch->sinks[0].sample_rate,
                    nullptr, /*updatedSampleRate*/
                    patch->sinks[0].format,
                    nullptr, /*updatedFormat*/
                    patch->sinks[0].channel_mask,
                    nullptr, /*updatedChannelMask*/
                    // FIXME for the parameter type,
                    // and the NONE
                    (audio_output_flags_t)
                    AUDIO_INPUT_FLAG_NONE) == IOProfile::NO_MATCH) {
                return INVALID_OPERATION;
            }
            // TODO: reconfigure output format and channels here
            ALOGV("%s setting device %s on output %d", __func__,
                  device->toString().c_str(), inputDesc->mIoHandle);
            setInputDevice(inputDesc->mIoHandle, device, true, handle);
            index = mAudioPatches.indexOfKey(*handle);
            if (index >= 0) {
                if (patchDesc != 0 && patchDesc != mAudioPatches.valueAt(index)) {
                    ALOGW("%s setInputDevice() did not reuse the patch provided", __func__);
                }
                patchDesc = mAudioPatches.valueAt(index);
                patchDesc->setUid(uid);
                ALOGV("%s success", __func__);
            } else {
                ALOGW("%s setInputDevice() failed to create a patch", __func__);
                return INVALID_OPERATION;
            }
        } else if (patch->sinks[0].type == AUDIO_PORT_TYPE_DEVICE) {
            // device to device connection
            if (patchDesc != 0) {
                if (patchDesc->mPatch.sources[0].id != patch->sources[0].id) {
                    return BAD_VALUE;
                }
            }
            sp<DeviceDescriptor> srcDevice =
                    mAvailableInputDevices.getDeviceFromId(patch->sources[0].id);
            if (srcDevice == 0) {
                return BAD_VALUE;
            }

            //update source and sink with our own data as the data passed in the patch may
            // be incomplete.
            PatchBuilder patchBuilder;
            audio_port_config sourcePortConfig = {};

            // if first sink is to MSD, establish single MSD patch
            if (getMsdAudioOutDevices().contains(
                        mAvailableOutputDevices.getDeviceFromId(patch->sinks[0].id))) {
                ALOGV("%s patching to MSD", __FUNCTION__);
                patchBuilder = buildMsdPatch(false /*msdIsSource*/, srcDevice);
                goto installPatch;
            }

            srcDevice->toAudioPortConfig(&sourcePortConfig, &patch->sources[0]);
            patchBuilder.addSource(sourcePortConfig);

            for (size_t i = 0; i < patch->num_sinks; i++) {
                if (patch->sinks[i].type != AUDIO_PORT_TYPE_DEVICE) {
                    ALOGV("%s source device but one sink is not a device", __func__);
                    return INVALID_OPERATION;
                }
                sp<DeviceDescriptor> sinkDevice =
                        mAvailableOutputDevices.getDeviceFromId(patch->sinks[i].id);
                if (sinkDevice == 0) {
                    return BAD_VALUE;
                }
                audio_port_config sinkPortConfig = {};
                sinkDevice->toAudioPortConfig(&sinkPortConfig, &patch->sinks[i]);
                patchBuilder.addSink(sinkPortConfig);

                // Whatever Sw or Hw bridge, we do attach an SwOutput to an Audio Source for
                // volume management purpose (tracking activity)
                // In case of Hw bridge, it is a Work Around. The mixPort used is the one declared
                // in config XML to reach the sink so that is can be declared as available.
                audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
                sp<SwAudioOutputDescriptor> outputDesc;
                if (!sourceDesc->isInternal()) {
                    // take care of dynamic routing for SwOutput selection,
                    audio_attributes_t attributes = sourceDesc->attributes();
                    audio_stream_type_t stream = sourceDesc->stream();
                    audio_attributes_t resultAttr;
                    audio_config_t config = AUDIO_CONFIG_INITIALIZER;
                    config.sample_rate = sourceDesc->config().sample_rate;
                    audio_channel_mask_t sourceMask = sourceDesc->config().channel_mask;
                    config.channel_mask =
                            (audio_channel_mask_get_representation(sourceMask)
                                == AUDIO_CHANNEL_REPRESENTATION_INDEX) ? sourceMask
                                    : audio_channel_mask_in_to_out(sourceMask);
                    config.format = sourceDesc->config().format;
                    audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
                    DeviceIdVector selectedDeviceIds;
                    bool isRequestedDeviceForExclusiveUse = false;
                    output_type_t outputType;
                    bool isSpatialized;
                    bool isBitPerfect;
                    getOutputForAttrInt(&resultAttr, &output, AUDIO_SESSION_NONE, &attributes,
                                        &stream, sourceDesc->uid(), &config, &flags,
                                        &selectedDeviceIds, &isRequestedDeviceForExclusiveUse,
                                        nullptr, &outputType, &isSpatialized, &isBitPerfect);
                    if (output == AUDIO_IO_HANDLE_NONE) {
                        ALOGV("%s no output for device %s",
                              __FUNCTION__, sinkDevice->toString().c_str());
                        return INVALID_OPERATION;
                    }
                    outputDesc = mOutputs.valueFor(output);
                    if (outputDesc->isDuplicated()) {
                        ALOGE("%s output is duplicated", __func__);
                        return INVALID_OPERATION;
                    }
                    bool closeOutput = outputDesc->mDirectOpenCount != 0;
                    sourceDesc->setSwOutput(outputDesc, closeOutput);
                } else {
                    // Same for "raw patches" aka created from createAudioPatch API
                    SortedVector<audio_io_handle_t> outputs =
                            getOutputsForDevices(DeviceVector(sinkDevice), mOutputs);
                    // if the sink device is reachable via an opened output stream, request to
                    // go via this output stream by adding a second source to the patch
                    // description
                    output = selectOutput(outputs);
                    if (output == AUDIO_IO_HANDLE_NONE) {
                        ALOGE("%s no output available for internal patch sink", __func__);
                        return INVALID_OPERATION;
                    }
                    outputDesc = mOutputs.valueFor(output);
                    if (outputDesc->isDuplicated()) {
                        ALOGV("%s output for device %s is duplicated",
                              __func__, sinkDevice->toString().c_str());
                        return INVALID_OPERATION;
                    }
                    sourceDesc->setSwOutput(outputDesc, /* closeOutput= */ false);
                }
                // create a software bridge in PatchPanel if:
                // - source and sink devices are on different HW modules OR
                // - audio HAL version is < 3.0
                // - audio HAL version is >= 3.0 but no route has been declared between devices
                // - called from startAudioSource (aka sourceDesc is not internal) and source device
                //   does not have a gain controller
                if (!srcDevice->hasSameHwModuleAs(sinkDevice) ||
                        (srcDevice->getModuleVersionMajor() < 3) ||
                        !srcDevice->getModule()->supportsPatch(srcDevice, sinkDevice) ||
                        (!sourceDesc->isInternal() &&
                         srcDevice->getAudioPort()->getGains().size() == 0)) {
                    // support only one sink device for now to simplify output selection logic
                    if (patch->num_sinks > 1) {
                        return INVALID_OPERATION;
                    }
                    sourceDesc->setUseSwBridge();
                    if (outputDesc != nullptr) {
                        audio_port_config srcMixPortConfig = {};
                        outputDesc->toAudioPortConfig(&srcMixPortConfig, nullptr);
                        // for volume control, we may need a valid stream
                        srcMixPortConfig.ext.mix.usecase.stream =
                            (!sourceDesc->isInternal() || sourceDesc->isCallTx()) ?
                                    mEngine->getStreamTypeForAttributes(sourceDesc->attributes()) :
                                    AUDIO_STREAM_PATCH;
                        patchBuilder.addSource(srcMixPortConfig);
                    }
                }
            }
            // TODO: check from routing capabilities in config file and other conflicting patches

installPatch:
            status_t status = installPatch(
                        __func__, index, handle, patchBuilder.patch(), delayMs, uid, &patchDesc);
            if (status != NO_ERROR) {
                ALOGW("%s patch panel could not connect device patch, error %d", __func__, status);
                return INVALID_OPERATION;
            }
        } else {
            return BAD_VALUE;
        }
    } else {
        return BAD_VALUE;
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::releaseAudioPatch(audio_patch_handle_t handle, uid_t uid)
{
    ALOGV("%s patch %d", __func__, handle);
    ssize_t index = mAudioPatches.indexOfKey(handle);

    if (index < 0) {
        return BAD_VALUE;
    }
    sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
    ALOGV("%s() mUidCached %d patchDesc->mUid %d uid %d",
          __func__, mUidCached, patchDesc->getUid(), uid);
    if (patchDesc->getUid() != mUidCached && uid != patchDesc->getUid()) {
        return INVALID_OPERATION;
    }
    audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE;
    for (size_t i = 0; i < mAudioSources.size(); i++)  {
        sp<SourceClientDescriptor> sourceDesc = mAudioSources.valueAt(i);
        if (sourceDesc != nullptr && sourceDesc->getPatchHandle() == handle) {
            portId = sourceDesc->portId();
            break;
        }
    }
    return portId != AUDIO_PORT_HANDLE_NONE ?
                stopAudioSource(portId) : releaseAudioPatchInternal(handle);
}

status_t AudioPolicyManager::releaseAudioPatchInternal(audio_patch_handle_t handle,
                                                       uint32_t delayMs,
                                                       const sp<SourceClientDescriptor>& sourceDesc)
{
    ALOGV("%s patch %d", __func__, handle);
    if (mAudioPatches.indexOfKey(handle) < 0) {
        ALOGE("%s: no patch found with handle=%d", __func__, handle);
        return BAD_VALUE;
    }
    sp<AudioPatch> patchDesc = mAudioPatches.valueFor(handle);
    struct audio_patch *patch = &patchDesc->mPatch;
    patchDesc->setUid(mUidCached);
    if (patch->sources[0].type == AUDIO_PORT_TYPE_MIX) {
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.getOutputFromId(patch->sources[0].id);
        if (outputDesc == NULL) {
            ALOGV("%s output not found for id %d", __func__, patch->sources[0].id);
            return BAD_VALUE;
        }

        setOutputDevices(__func__, outputDesc,
                         getNewOutputDevices(outputDesc, true /*fromCache*/),
                         true,
                         0,
                         NULL);
    } else if (patch->sources[0].type == AUDIO_PORT_TYPE_DEVICE) {
        if (patch->sinks[0].type == AUDIO_PORT_TYPE_MIX) {
            sp<AudioInputDescriptor> inputDesc = mInputs.getInputFromId(patch->sinks[0].id);
            if (inputDesc == NULL) {
                ALOGV("%s input not found for id %d", __func__, patch->sinks[0].id);
                return BAD_VALUE;
            }
            setInputDevice(inputDesc->mIoHandle,
                           getNewInputDevice(inputDesc),
                           true,
                           NULL);
        } else if (patch->sinks[0].type == AUDIO_PORT_TYPE_DEVICE) {
            status_t status =
                    mpClientInterface->releaseAudioPatch(patchDesc->getAfHandle(), delayMs);
            ALOGV("%s patch panel returned %d patchHandle %d",
                  __func__, status, patchDesc->getAfHandle());
            removeAudioPatch(patchDesc->getHandle());
            nextAudioPortGeneration();
            mpClientInterface->onAudioPatchListUpdate();
            // SW or HW Bridge
            sp<SwAudioOutputDescriptor> outputDesc = nullptr;
            audio_patch_handle_t patchHandle = AUDIO_PATCH_HANDLE_NONE;
            if (patch->num_sources > 1 && patch->sources[1].type == AUDIO_PORT_TYPE_MIX) {
                outputDesc = mOutputs.getOutputFromId(patch->sources[1].id);
            } else if (patch->num_sources == 1 && sourceDesc != nullptr) {
                outputDesc = sourceDesc->swOutput().promote();
            }
            if (outputDesc == nullptr) {
                ALOGW("%s no output for id %d", __func__, patch->sources[0].id);
                // releaseOutput has already called closeOutput in case of direct output
                return NO_ERROR;
            }
            patchHandle = outputDesc->getPatchHandle();
            // While using a HwBridge, force reconsidering device only if not reusing an existing
            // output and no more activity on output (will force to close).
            const bool force = sourceDesc->canCloseOutput() && !outputDesc->isActive();
            // APM pattern is to have always outputs opened / patch realized for reachable devices.
            // Update device may result to NONE (empty), coupled with force, it releases the patch.
            // Reconsider device only for cases:
            //      1 / Active Output
            //      2 / Inactive Output previously hosting HwBridge
            //      3 / Inactive Output previously hosting SwBridge that can be closed.
            bool updateDevice = outputDesc->isActive() || !sourceDesc->useSwBridge() ||
                    sourceDesc->canCloseOutput();
            setOutputDevices(__func__, outputDesc,
                             updateDevice ? getNewOutputDevices(outputDesc, true /*fromCache*/) :
                                            outputDesc->devices(),
                             force,
                             0,
                             patchHandle == AUDIO_PATCH_HANDLE_NONE ? nullptr : &patchHandle);
        } else {
            return BAD_VALUE;
        }
    } else {
        return BAD_VALUE;
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::listAudioPatches(unsigned int *num_patches,
                                              struct audio_patch *patches,
                                              unsigned int *generation)
{
    if (generation == NULL) {
        return BAD_VALUE;
    }
    *generation = curAudioPortGeneration();
    return mAudioPatches.listAudioPatches(num_patches, patches);
}

status_t AudioPolicyManager::setAudioPortConfig(const struct audio_port_config *config)
{
    ALOGV("setAudioPortConfig()");

    if (config == NULL) {
        return BAD_VALUE;
    }
    ALOGV("setAudioPortConfig() on port handle %d", config->id);
    // Only support gain configuration for now
    if (config->config_mask != AUDIO_PORT_CONFIG_GAIN) {
        return INVALID_OPERATION;
    }

    sp<AudioPortConfig> audioPortConfig;
    if (config->type == AUDIO_PORT_TYPE_MIX) {
        if (config->role == AUDIO_PORT_ROLE_SOURCE) {
            sp<SwAudioOutputDescriptor> outputDesc = mOutputs.getOutputFromId(config->id);
            if (outputDesc == NULL) {
                return BAD_VALUE;
            }
            ALOG_ASSERT(!outputDesc->isDuplicated(),
                        "setAudioPortConfig() called on duplicated output %d",
                        outputDesc->mIoHandle);
            audioPortConfig = outputDesc;
        } else if (config->role == AUDIO_PORT_ROLE_SINK) {
            sp<AudioInputDescriptor> inputDesc = mInputs.getInputFromId(config->id);
            if (inputDesc == NULL) {
                return BAD_VALUE;
            }
            audioPortConfig = inputDesc;
        } else {
            return BAD_VALUE;
        }
    } else if (config->type == AUDIO_PORT_TYPE_DEVICE) {
        sp<DeviceDescriptor> deviceDesc;
        if (config->role == AUDIO_PORT_ROLE_SOURCE) {
            deviceDesc = mAvailableInputDevices.getDeviceFromId(config->id);
        } else if (config->role == AUDIO_PORT_ROLE_SINK) {
            deviceDesc = mAvailableOutputDevices.getDeviceFromId(config->id);
        } else {
            return BAD_VALUE;
        }
        if (deviceDesc == NULL) {
            return BAD_VALUE;
        }
        audioPortConfig = deviceDesc;
    } else {
        return BAD_VALUE;
    }

    struct audio_port_config backupConfig = {};
    status_t status = audioPortConfig->applyAudioPortConfig(config, &backupConfig);
    if (status == NO_ERROR) {
        struct audio_port_config newConfig = {};
        audioPortConfig->toAudioPortConfig(&newConfig, config);
        status = mpClientInterface->setAudioPortConfig(&newConfig, 0);
    }
    if (status != NO_ERROR) {
        audioPortConfig->applyAudioPortConfig(&backupConfig);
    }

    return status;
}

void AudioPolicyManager::releaseResourcesForUid(uid_t uid)
{
    clearAudioSources(uid);
    clearAudioPatches(uid);
    clearSessionRoutes(uid);
}

void AudioPolicyManager::clearAudioPatches(uid_t uid)
{
    for (ssize_t i = (ssize_t)mAudioPatches.size() - 1; i >= 0; i--)  {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(i);
        if (patchDesc->getUid() == uid) {
            releaseAudioPatch(mAudioPatches.keyAt(i), uid);
        }
    }
}

void AudioPolicyManager::checkStrategyRoute(product_strategy_t ps, audio_io_handle_t ouptutToSkip)
{
    // Take the first attributes following the product strategy as it is used to retrieve the routed
    // device. All attributes wihin a strategy follows the same "routing strategy"
    auto attributes = mEngine->getAllAttributesForProductStrategy(ps).front();
    DeviceVector devices = mEngine->getOutputDevicesForAttributes(attributes, nullptr, false);
    SortedVector<audio_io_handle_t> outputs = getOutputsForDevices(devices, mOutputs);
    std::map<audio_io_handle_t, DeviceVector> outputsToReopen;
    for (size_t j = 0; j < mOutputs.size(); j++) {
        if (mOutputs.keyAt(j) == ouptutToSkip) {
            continue;
        }
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(j);
        if (!outputDesc->isStrategyActive(ps)) {
            continue;
        }
        // If the default device for this strategy is on another output mix,
        // invalidate all tracks in this strategy to force re connection.
        // Otherwise select new device on the output mix.
        if (outputs.indexOf(mOutputs.keyAt(j)) < 0) {
            invalidateStreams(mEngine->getStreamTypesForProductStrategy(ps));
        } else {
            DeviceVector newDevices = getNewOutputDevices(outputDesc, false /*fromCache*/);
            if (outputDesc->mPreferredAttrInfo != nullptr && outputDesc->devices() != newDevices) {
                // If the device is using preferred mixer attributes, the output need to reopen
                // with default configuration when the new selected devices are different from
                // current routing devices.
                outputsToReopen.emplace(mOutputs.keyAt(j), newDevices);
                continue;
            }
            setOutputDevices(__func__, outputDesc, newDevices, false);
        }
    }
    reopenOutputsWithDevices(outputsToReopen);
}

void AudioPolicyManager::clearSessionRoutes(uid_t uid)
{
    // remove output routes associated with this uid
    std::vector<product_strategy_t> affectedStrategies;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
        for (const auto& client : outputDesc->getClientIterable()) {
            if (client->hasPreferredDevice() && client->uid() == uid) {
                client->setPreferredDeviceId(AUDIO_PORT_HANDLE_NONE);
                auto clientStrategy = client->strategy();
                if (std::find(begin(affectedStrategies), end(affectedStrategies), clientStrategy) !=
                        end(affectedStrategies)) {
                    continue;
                }
                affectedStrategies.push_back(client->strategy());
            }
        }
    }
    // reroute outputs if necessary
    for (const auto& strategy : affectedStrategies) {
        checkStrategyRoute(strategy, AUDIO_IO_HANDLE_NONE);
    }

    // remove input routes associated with this uid
    SortedVector<audio_source_t> affectedSources;
    for (size_t i = 0; i < mInputs.size(); i++) {
        sp<AudioInputDescriptor> inputDesc = mInputs.valueAt(i);
        for (const auto& client : inputDesc->getClientIterable()) {
            if (client->hasPreferredDevice() && client->uid() == uid) {
                client->setPreferredDeviceId(AUDIO_PORT_HANDLE_NONE);
                affectedSources.add(client->source());
            }
        }
    }
    // reroute inputs if necessary
    SortedVector<audio_io_handle_t> inputsToClose;
    for (size_t i = 0; i < mInputs.size(); i++) {
        sp<AudioInputDescriptor> inputDesc = mInputs.valueAt(i);
        if (affectedSources.indexOf(inputDesc->source()) >= 0) {
            inputsToClose.add(inputDesc->mIoHandle);
        }
    }
    for (const auto& input : inputsToClose) {
        closeInput(input);
    }
}

void AudioPolicyManager::clearAudioSources(uid_t uid)
{
    for (ssize_t i = (ssize_t)mAudioSources.size() - 1; i >= 0; i--)  {
        sp<SourceClientDescriptor> sourceDesc = mAudioSources.valueAt(i);
        if (sourceDesc->uid() == uid) {
            stopAudioSource(mAudioSources.keyAt(i));
        }
    }
}

status_t AudioPolicyManager::acquireSoundTriggerSession(audio_session_t *session,
                                       audio_io_handle_t *ioHandle,
                                       audio_devices_t *device)
{
    *session = (audio_session_t)mpClientInterface->newAudioUniqueId(AUDIO_UNIQUE_ID_USE_SESSION);
    *ioHandle = (audio_io_handle_t)mpClientInterface->newAudioUniqueId(AUDIO_UNIQUE_ID_USE_INPUT);
    audio_attributes_t attr = { .source = AUDIO_SOURCE_HOTWORD };
    sp<DeviceDescriptor> deviceDesc = mEngine->getInputDeviceForAttributes(attr);
    if (deviceDesc == nullptr) {
        return INVALID_OPERATION;
    }
    *device = deviceDesc->type();

    return mSoundTriggerSessions.acquireSession(*session, *ioHandle);
}

status_t AudioPolicyManager::startAudioSource(const struct audio_port_config *source,
                                              const audio_attributes_t *attributes,
                                              audio_port_handle_t *portId,
                                              uid_t uid) {
    return startAudioSourceInternal(source, attributes, portId, uid,
                                    false /*internal*/, false /*isCallRx*/, 0 /*delayMs*/);
}

status_t AudioPolicyManager::startAudioSourceInternal(const struct audio_port_config *source,
                                              const audio_attributes_t *attributes,
                                              audio_port_handle_t *portId,
                                              uid_t uid, bool internal, bool isCallRx,
                                              uint32_t delayMs)
{
    ALOGV("%s", __FUNCTION__);
    *portId = AUDIO_PORT_HANDLE_NONE;

    if (source == NULL || attributes == NULL || portId == NULL) {
        ALOGW("%s invalid argument: source %p attributes %p handle %p",
              __FUNCTION__, source, attributes, portId);
        return BAD_VALUE;
    }

    if (source->role != AUDIO_PORT_ROLE_SOURCE ||
            source->type != AUDIO_PORT_TYPE_DEVICE) {
        ALOGW("%s INVALID_OPERATION source->role %d source->type %d",
              __FUNCTION__, source->role, source->type);
        return INVALID_OPERATION;
    }

    sp<DeviceDescriptor> srcDevice =
            mAvailableInputDevices.getDevice(source->ext.device.type,
                                             String8(source->ext.device.address),
                                             AUDIO_FORMAT_DEFAULT);
    if (srcDevice == 0) {
        ALOGW("%s source->ext.device.type %08x not found", __FUNCTION__, source->ext.device.type);
        return BAD_VALUE;
    }

    *portId = PolicyAudioPort::getNextUniqueId();

    sp<SourceClientDescriptor> sourceDesc =
        new SourceClientDescriptor(*portId, uid, *attributes, *source, srcDevice,
                                   mEngine->getStreamTypeForAttributes(*attributes),
                                   mEngine->getProductStrategyForAttributes(*attributes),
                                   toVolumeSource(*attributes), internal, isCallRx, false);

    status_t status = connectAudioSource(sourceDesc, delayMs);
    if (status == NO_ERROR) {
        mAudioSources.add(*portId, sourceDesc);
    }
    return status;
}

status_t AudioPolicyManager::connectAudioSource(const sp<SourceClientDescriptor>& sourceDesc,
                                                uint32_t delayMs)
{
    ALOGV("%s handle %d", __FUNCTION__, sourceDesc->portId());

    // make sure we only have one patch per source.
    disconnectAudioSource(sourceDesc);

    audio_attributes_t attributes = sourceDesc->attributes();
    // May the device (dynamic) have been disconnected/reconnected, id has changed.
    sp<DeviceDescriptor> srcDevice = mAvailableInputDevices.getDevice(
                sourceDesc->srcDevice()->type(),
                String8(sourceDesc->srcDevice()->address().c_str()),
                AUDIO_FORMAT_DEFAULT);
    DeviceVector sinkDevices =
            mEngine->getOutputDevicesForAttributes(attributes, nullptr, false /*fromCache*/);
    ALOG_ASSERT(!sinkDevices.isEmpty(), "connectAudioSource(): no device found for attributes");
    sp<DeviceDescriptor> sinkDevice = sinkDevices.itemAt(0);
    if (!mAvailableOutputDevices.contains(sinkDevice)) {
        ALOGE("%s Device %s not available", __func__, sinkDevice->toString().c_str());
        return INVALID_OPERATION;
    }
    PatchBuilder patchBuilder;
    patchBuilder.addSink(sinkDevice).addSource(srcDevice);
    audio_patch_handle_t handle = AUDIO_PATCH_HANDLE_NONE;

    return connectAudioSourceToSink(
                sourceDesc, sinkDevice, patchBuilder.patch(), handle, mUidCached, delayMs);
}

status_t AudioPolicyManager::stopAudioSource(audio_port_handle_t portId)
{
    sp<SourceClientDescriptor> sourceDesc = mAudioSources.valueFor(portId);
    ALOGV("%s port ID %d", __FUNCTION__, portId);
    if (sourceDesc == 0) {
        ALOGW("%s unknown source for port ID %d", __FUNCTION__, portId);
        return BAD_VALUE;
    }
    status_t status = disconnectAudioSource(sourceDesc);

    mAudioSources.removeItem(portId);
    return status;
}

status_t AudioPolicyManager::setMasterMono(bool mono)
{
    if (mMasterMono == mono) {
        return NO_ERROR;
    }
    mMasterMono = mono;
    // if enabling mono we close all offloaded devices, which will invalidate the
    // corresponding AudioTrack. The AudioTrack client/MediaPlayer is responsible
    // for recreating the new AudioTrack as non-offloaded PCM.
    //
    // If disabling mono, we leave all tracks as is: we don't know which clients
    // and tracks are able to be recreated as offloaded. The next "song" should
    // play back offloaded.
    if (mMasterMono) {
        Vector<audio_io_handle_t> offloaded;
        for (size_t i = 0; i < mOutputs.size(); ++i) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (desc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
                offloaded.push(desc->mIoHandle);
            }
        }
        for (const auto& handle : offloaded) {
            closeOutput(handle);
        }
    }
    // update master mono for all remaining outputs
    for (size_t i = 0; i < mOutputs.size(); ++i) {
        updateMono(mOutputs.keyAt(i));
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::getMasterMono(bool *mono)
{
    *mono = mMasterMono;
    return NO_ERROR;
}

float AudioPolicyManager::getStreamVolumeDB(
        audio_stream_type_t stream, int index, audio_devices_t device)
{
    return computeVolume(getVolumeCurves(stream), toVolumeSource(stream), index,
                         {device}, /* adjustAttenuation= */false);
}

status_t AudioPolicyManager::getSurroundFormats(unsigned int *numSurroundFormats,
                                                audio_format_t *surroundFormats,
                                                bool *surroundFormatsEnabled)
{
    if (numSurroundFormats == nullptr || (*numSurroundFormats != 0 &&
            (surroundFormats == nullptr || surroundFormatsEnabled == nullptr))) {
        return BAD_VALUE;
    }
    ALOGV("%s() numSurroundFormats %d surroundFormats %p surroundFormatsEnabled %p",
            __func__, *numSurroundFormats, surroundFormats, surroundFormatsEnabled);

    size_t formatsWritten = 0;
    size_t formatsMax = *numSurroundFormats;

    *numSurroundFormats = mConfig->getSurroundFormats().size();
    audio_policy_forced_cfg_t forceUse = mEngine->getForceUse(
            AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND);
    for (const auto& format: mConfig->getSurroundFormats()) {
        if (formatsWritten < formatsMax) {
            surroundFormats[formatsWritten] = format.first;
            bool formatEnabled = true;
            switch (forceUse) {
                case AUDIO_POLICY_FORCE_ENCODED_SURROUND_MANUAL:
                    formatEnabled = mManualSurroundFormats.count(format.first) != 0;
                    break;
                case AUDIO_POLICY_FORCE_ENCODED_SURROUND_NEVER:
                    formatEnabled = false;
                    break;
                default: // AUTO or ALWAYS => true
                    break;
            }
            surroundFormatsEnabled[formatsWritten++] = formatEnabled;
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::getReportedSurroundFormats(unsigned int *numSurroundFormats,
                                                        audio_format_t *surroundFormats) {
    if (numSurroundFormats == nullptr || (*numSurroundFormats != 0 && surroundFormats == nullptr)) {
        return BAD_VALUE;
    }
    ALOGV("%s() numSurroundFormats %d surroundFormats %p",
            __func__, *numSurroundFormats, surroundFormats);

    size_t formatsWritten = 0;
    size_t formatsMax = *numSurroundFormats;
    std::unordered_set<audio_format_t> formats; // Uses primary surround formats only

    // Return formats from all device profiles that have already been resolved by
    // checkOutputsForDevice().
    for (size_t i = 0; i < mAvailableOutputDevices.size(); i++) {
        sp<DeviceDescriptor> device = mAvailableOutputDevices[i];
        audio_devices_t deviceType = device->type();
        // Enabling/disabling formats are applied to only HDMI devices. So, this function
        // returns formats reported by HDMI devices.
        if (deviceType != AUDIO_DEVICE_OUT_HDMI &&
            deviceType != AUDIO_DEVICE_OUT_HDMI_ARC && deviceType != AUDIO_DEVICE_OUT_HDMI_EARC) {
            continue;
        }
        // Formats reported by sink devices
        std::unordered_set<audio_format_t> formatset;
        if (auto it = mReportedFormatsMap.find(device); it != mReportedFormatsMap.end()) {
            formatset.insert(it->second.begin(), it->second.end());
        }

        // Formats hard-coded in the in policy configuration file (if any).
        FormatVector encodedFormats = device->encodedFormats();
        formatset.insert(encodedFormats.begin(), encodedFormats.end());
        // Filter the formats which are supported by the vendor hardware.
        for (auto it = formatset.begin(); it != formatset.end(); ++it) {
            if (mConfig->getSurroundFormats().count(*it) != 0) {
                formats.insert(*it);
            } else {
                for (const auto& pair : mConfig->getSurroundFormats()) {
                    if (pair.second.count(*it) != 0) {
                        formats.insert(pair.first);
                        break;
                    }
                }
            }
        }
    }
    *numSurroundFormats = formats.size();
    for (const auto& format: formats) {
        if (formatsWritten < formatsMax) {
            surroundFormats[formatsWritten++] = format;
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::setSurroundFormatEnabled(audio_format_t audioFormat, bool enabled)
{
    ALOGV("%s() format 0x%X enabled %d", __func__, audioFormat, enabled);
    const auto& formatIter = mConfig->getSurroundFormats().find(audioFormat);
    if (formatIter == mConfig->getSurroundFormats().end()) {
        ALOGW("%s() format 0x%X is not a known surround format", __func__, audioFormat);
        return BAD_VALUE;
    }

    if (mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND) !=
            AUDIO_POLICY_FORCE_ENCODED_SURROUND_MANUAL) {
        ALOGW("%s() not in manual mode for surround sound format selection", __func__);
        return INVALID_OPERATION;
    }

    if ((mManualSurroundFormats.count(audioFormat) != 0) == enabled) {
        return NO_ERROR;
    }

    std::unordered_set<audio_format_t> surroundFormatsBackup(mManualSurroundFormats);
    if (enabled) {
        mManualSurroundFormats.insert(audioFormat);
        for (const auto& subFormat : formatIter->second) {
            mManualSurroundFormats.insert(subFormat);
        }
    } else {
        mManualSurroundFormats.erase(audioFormat);
        for (const auto& subFormat : formatIter->second) {
            mManualSurroundFormats.erase(subFormat);
        }
    }

    sp<SwAudioOutputDescriptor> outputDesc;
    bool profileUpdated = false;
    DeviceVector hdmiOutputDevices = mAvailableOutputDevices.getDevicesFromTypes(
        {AUDIO_DEVICE_OUT_HDMI, AUDIO_DEVICE_OUT_HDMI_ARC, AUDIO_DEVICE_OUT_HDMI_EARC});
    for (size_t i = 0; i < hdmiOutputDevices.size(); i++) {
        // Simulate reconnection to update enabled surround sound formats.
        String8 address = String8(hdmiOutputDevices[i]->address().c_str());
        std::string name = hdmiOutputDevices[i]->getName();
        status_t status = setDeviceConnectionStateInt(hdmiOutputDevices[i]->type(),
                                                      AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                                      address.c_str(),
                                                      name.c_str(),
                                                      AUDIO_FORMAT_DEFAULT);
        if (status != NO_ERROR) {
            continue;
        }
        status = setDeviceConnectionStateInt(hdmiOutputDevices[i]->type(),
                                             AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                                             address.c_str(),
                                             name.c_str(),
                                             AUDIO_FORMAT_DEFAULT);
        profileUpdated |= (status == NO_ERROR);
    }
    // FIXME: Why doing this for input HDMI devices if we don't augment their reported formats?
    DeviceVector hdmiInputDevices = mAvailableInputDevices.getDevicesFromType(
                AUDIO_DEVICE_IN_HDMI);
    for (size_t i = 0; i < hdmiInputDevices.size(); i++) {
        // Simulate reconnection to update enabled surround sound formats.
        String8 address = String8(hdmiInputDevices[i]->address().c_str());
        std::string name = hdmiInputDevices[i]->getName();
        status_t status = setDeviceConnectionStateInt(AUDIO_DEVICE_IN_HDMI,
                                                      AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                                      address.c_str(),
                                                      name.c_str(),
                                                      AUDIO_FORMAT_DEFAULT);
        if (status != NO_ERROR) {
            continue;
        }
        status = setDeviceConnectionStateInt(AUDIO_DEVICE_IN_HDMI,
                                             AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                                             address.c_str(),
                                             name.c_str(),
                                             AUDIO_FORMAT_DEFAULT);
        profileUpdated |= (status == NO_ERROR);
    }

    if (!profileUpdated) {
        ALOGW("%s() no audio profiles updated, undoing surround formats change", __func__);
        mManualSurroundFormats = std::move(surroundFormatsBackup);
    }

    return profileUpdated ? NO_ERROR : INVALID_OPERATION;
}

void AudioPolicyManager::setAppState(audio_port_handle_t portId, app_state_t state)
{
    ALOGV("%s(portId:%d, state:%d)", __func__, portId, state);
    for (size_t i = 0; i < mInputs.size(); i++) {
        mInputs.valueAt(i)->setAppState(portId, state);
    }
}

bool AudioPolicyManager::isHapticPlaybackSupported()
{
    for (const auto& hwModule : mHwModules) {
        const OutputProfileCollection &outputProfiles = hwModule->getOutputProfiles();
        for (const auto &outProfile : outputProfiles) {
            struct audio_port audioPort;
            outProfile->toAudioPort(&audioPort);
            for (size_t i = 0; i < audioPort.num_channel_masks; i++) {
                if (audioPort.channel_masks[i] & AUDIO_CHANNEL_HAPTIC_ALL) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool AudioPolicyManager::isUltrasoundSupported()
{
    bool hasUltrasoundOutput = false;
    bool hasUltrasoundInput = false;
    for (const auto& hwModule : mHwModules) {
        const OutputProfileCollection &outputProfiles = hwModule->getOutputProfiles();
        if (!hasUltrasoundOutput) {
            for (const auto &outProfile : outputProfiles) {
                if (outProfile->getFlags() & AUDIO_OUTPUT_FLAG_ULTRASOUND) {
                    hasUltrasoundOutput = true;
                    break;
                }
            }
        }

        const InputProfileCollection &inputProfiles = hwModule->getInputProfiles();
        if (!hasUltrasoundInput) {
            for (const auto &inputProfile : inputProfiles) {
                if (inputProfile->getFlags() & AUDIO_INPUT_FLAG_ULTRASOUND) {
                    hasUltrasoundInput = true;
                    break;
                }
            }
        }

        if (hasUltrasoundOutput && hasUltrasoundInput)
            return true;
    }
    return false;
}

bool AudioPolicyManager::isHotwordStreamSupported(bool lookbackAudio)
{
    const auto mask = AUDIO_INPUT_FLAG_HOTWORD_TAP |
        (lookbackAudio ? AUDIO_INPUT_FLAG_HW_LOOKBACK : 0);
    for (const auto& hwModule : mHwModules) {
        const InputProfileCollection &inputProfiles = hwModule->getInputProfiles();
        for (const auto &inputProfile : inputProfiles) {
            if ((inputProfile->getFlags() & mask) == mask) {
                return true;
            }
        }
    }
    return false;
}

bool AudioPolicyManager::isCallScreenModeSupported()
{
    return mConfig->isCallScreenModeSupported();
}


status_t AudioPolicyManager::disconnectAudioSource(const sp<SourceClientDescriptor>& sourceDesc)
{
    ALOGV("%s port Id %d", __FUNCTION__, sourceDesc->portId());
    if (!sourceDesc->isConnected()) {
        ALOGV("%s port Id %d already disconnected", __FUNCTION__, sourceDesc->portId());
        return NO_ERROR;
    }
    sp<SwAudioOutputDescriptor> swOutput = sourceDesc->swOutput().promote();
    if (swOutput != 0) {
        status_t status = stopSource(swOutput, sourceDesc);
        if (status == NO_ERROR) {
            swOutput->stop();
        }
        if (releaseOutput(sourceDesc->portId())) {
            // The output descriptor is reopened to query dynamic profiles. In that case, there is
            // no need to release audio patch here but just return NO_ERROR.
            return NO_ERROR;
        }
    } else {
        sp<HwAudioOutputDescriptor> hwOutputDesc = sourceDesc->hwOutput().promote();
        if (hwOutputDesc != 0) {
          //   close Hwoutput and remove from mHwOutputs
        } else {
            ALOGW("%s source has neither SW nor HW output", __FUNCTION__);
        }
    }
    status_t status = releaseAudioPatchInternal(sourceDesc->getPatchHandle(), 0, sourceDesc);
    sourceDesc->disconnect();
    return status;
}

sp<SourceClientDescriptor> AudioPolicyManager::getSourceForAttributesOnOutput(
        audio_io_handle_t output, const audio_attributes_t &attr)
{
    sp<SourceClientDescriptor> source;
    for (size_t i = 0; i < mAudioSources.size(); i++)  {
        sp<SourceClientDescriptor> sourceDesc = mAudioSources.valueAt(i);
        sp<SwAudioOutputDescriptor> outputDesc = sourceDesc->swOutput().promote();
        if (followsSameRouting(attr, sourceDesc->attributes()) &&
                               outputDesc != 0 && outputDesc->mIoHandle == output) {
            source = sourceDesc;
            break;
        }
    }
    return source;
}

bool AudioPolicyManager::canBeSpatializedInt(const audio_attributes_t *attr,
                                      const audio_config_t *config,
                                      const AudioDeviceTypeAddrVector &devices)  const
{
    // The caller can have the audio attributes criteria ignored by either passing a null ptr or
    // the AUDIO_ATTRIBUTES_INITIALIZER value.
    // If attributes are specified, current policy is to only allow spatialization for media
    // and game usages.
    if (attr != nullptr && *attr != AUDIO_ATTRIBUTES_INITIALIZER) {
        if (attr->usage != AUDIO_USAGE_MEDIA && attr->usage != AUDIO_USAGE_GAME) {
            return false;
        }
        if ((attr->flags & (AUDIO_FLAG_CONTENT_SPATIALIZED | AUDIO_FLAG_NEVER_SPATIALIZE)) != 0) {
            return false;
        }
    }

    // The caller can have the audio config criteria ignored by either passing a null ptr or
    // the AUDIO_CONFIG_INITIALIZER value.
    // If an audio config is specified, current policy is to only allow spatialization for
    // some positional channel masks and PCM format and for stereo if low latency performance
    // mode is not requested.

    if (config != nullptr && *config != AUDIO_CONFIG_INITIALIZER) {
        const bool channel_mask_spatialized =
                SpatializerHelper::isStereoSpatializationFeatureEnabled()
                        ? audio_channel_mask_contains_stereo(config->channel_mask)
                        : audio_is_channel_mask_spatialized(config->channel_mask);
        if (!channel_mask_spatialized) {
            return false;
        }
        if (!audio_is_linear_pcm(config->format)) {
            return false;
        }
        if (config->channel_mask == AUDIO_CHANNEL_OUT_STEREO
                && ((attr->flags & AUDIO_FLAG_LOW_LATENCY) != 0)) {
            return false;
        }
    }

    sp<IOProfile> profile =
            getSpatializerOutputProfile(config, devices);
    if (profile == nullptr) {
        return false;
    }

    return true;
}

// The Spatializer output is compatible with Haptic use cases if:
// 1. the Spatializer output thread supports Haptic, and format/sampleRate are same
// with client if client haptic channel bits were set, or
// 2. the Spatializer output thread does not support Haptic, and client did not ask haptic by
// including the haptic bits or creating the HapticGenerator effect for same session.
bool AudioPolicyManager::checkHapticCompatibilityOnSpatializerOutput(
        const audio_config_t* config, audio_session_t sessionId) const {
    const auto clientHapticChannel =
            audio_channel_count_from_out_mask(config->channel_mask & AUDIO_CHANNEL_HAPTIC_ALL);
    const auto threadOutputHapticChannel = audio_channel_count_from_out_mask(
            mSpatializerOutput->getChannelMask() & AUDIO_CHANNEL_HAPTIC_ALL);

    if (threadOutputHapticChannel) {
        // check format and sampleRate match if client haptic channel mask exist
        if (clientHapticChannel) {
            return mSpatializerOutput->getFormat() == config->format &&
                   mSpatializerOutput->getSamplingRate() == config->sample_rate;
        }
        return true;
    } else {
        // in the case of the Spatializer output channel mask does not have haptic channel bits, it
        // means haptic use cases (either the client channelmask includes haptic bits, or created a
        // HapticGenerator effect for this session) are not supported.
        return clientHapticChannel == 0 &&
               !mEffects.hasOrphansForSession(sessionId, FX_IID_HAPTICGENERATOR);
    }
}

void AudioPolicyManager::checkVirtualizerClientRoutes() {
    std::set<audio_stream_type_t> streamsToInvalidate;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        const sp<SwAudioOutputDescriptor>& desc = mOutputs[i];
        for (const sp<TrackClientDescriptor>& client : desc->getClientIterable()) {
            audio_attributes_t attr = client->attributes();
            DeviceVector devices = mEngine->getOutputDevicesForAttributes(attr, nullptr, false);
            AudioDeviceTypeAddrVector devicesTypeAddress = devices.toTypeAddrVector();
            audio_config_base_t clientConfig = client->config();
            audio_config_t config = audio_config_initializer(&clientConfig);
            if (desc != mSpatializerOutput
                    && canBeSpatializedInt(&attr, &config, devicesTypeAddress)) {
                streamsToInvalidate.insert(client->stream());
            }
        }
    }

    invalidateStreams(StreamTypeVector(streamsToInvalidate.begin(), streamsToInvalidate.end()));
}


bool AudioPolicyManager::isOutputOnlyAvailableRouteToSomeDevice(
        const sp<SwAudioOutputDescriptor>& outputDesc) {
    if (outputDesc->isDuplicated()) {
        return false;
    }
    DeviceVector devices = outputDesc->supportedDevices();
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        if (desc == outputDesc || desc->isDuplicated()) {
            continue;
        }
        DeviceVector sharedDevices = desc->filterSupportedDevices(devices);
        if (!sharedDevices.isEmpty()
                && (desc->devicesSupportEncodedFormats(sharedDevices.types())
                    == outputDesc->devicesSupportEncodedFormats(sharedDevices.types()))) {
            return false;
        }
    }
    return true;
}


status_t AudioPolicyManager::getSpatializerOutput(const audio_config_base_t *mixerConfig,
                                                        const audio_attributes_t *attr,
                                                        audio_io_handle_t *output) {
    *output = AUDIO_IO_HANDLE_NONE;

    DeviceVector devices = mEngine->getOutputDevicesForAttributes(*attr, nullptr, false);
    AudioDeviceTypeAddrVector devicesTypeAddress = devices.toTypeAddrVector();
    audio_config_t *configPtr = nullptr;
    audio_config_t config;
    if (mixerConfig != nullptr) {
        config = audio_config_initializer(mixerConfig);
        configPtr = &config;
    }
    if (!canBeSpatializedInt(attr, configPtr, devicesTypeAddress)) {
        ALOGV("%s provided attributes or mixer config cannot be spatialized", __func__);
        return BAD_VALUE;
    }

    sp<IOProfile> profile =
            getSpatializerOutputProfile(configPtr, devicesTypeAddress);
    if (profile == nullptr) {
        ALOGV("%s no suitable output profile for provided attributes or mixer config", __func__);
        return BAD_VALUE;
    }

    std::vector<sp<SwAudioOutputDescriptor>> spatializerOutputs;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        if (!desc->isDuplicated()
                && (desc->mFlags & AUDIO_OUTPUT_FLAG_SPATIALIZER) != 0) {
            spatializerOutputs.push_back(desc);
            ALOGV("%s adding opened spatializer Output %d", __func__, desc->mIoHandle);
        }
    }
    mSpatializerOutput.clear();
    bool outputsChanged = false;
    for (const auto& desc : spatializerOutputs) {
        if (desc->mProfile == profile
                && (configPtr == nullptr
                   || configPtr->channel_mask == desc->mMixerChannelMask)) {
            mSpatializerOutput = desc;
            ALOGV("%s reusing current spatializer output %d", __func__, desc->mIoHandle);
        } else {
            ALOGV("%s closing spatializerOutput output %d to match channel mask %#x"
                    " and devices %s", __func__, desc->mIoHandle,
                    configPtr != nullptr ? configPtr->channel_mask : 0,
                    devices.toString().c_str());
            closeOutput(desc->mIoHandle);
            outputsChanged = true;
        }
    }

    if (mSpatializerOutput == nullptr) {
        sp<SwAudioOutputDescriptor> desc =
                openOutputWithProfileAndDevice(profile, devices, mixerConfig);
        if (desc != nullptr) {
            mSpatializerOutput = desc;
            outputsChanged = true;
        }
    }

    checkVirtualizerClientRoutes();

    if (outputsChanged) {
        mPreviousOutputs = mOutputs;
        mpClientInterface->onAudioPortListUpdate();
    }

    if (mSpatializerOutput == nullptr) {
        ALOGV("%s could not open spatializer output with requested config", __func__);
        return BAD_VALUE;
    }
    *output = mSpatializerOutput->mIoHandle;
    ALOGV("%s returning new spatializer output %d", __func__, *output);
    return OK;
}

status_t AudioPolicyManager::releaseSpatializerOutput(audio_io_handle_t output) {
    if (mSpatializerOutput == nullptr) {
        return INVALID_OPERATION;
    }
    if (mSpatializerOutput->mIoHandle != output) {
        return BAD_VALUE;
    }

    if (!isOutputOnlyAvailableRouteToSomeDevice(mSpatializerOutput)) {
        ALOGV("%s closing spatializer output %d", __func__, mSpatializerOutput->mIoHandle);
        closeOutput(mSpatializerOutput->mIoHandle);
        //from now on mSpatializerOutput is null
        checkVirtualizerClientRoutes();
    }

    return NO_ERROR;
}

// ----------------------------------------------------------------------------
// AudioPolicyManager
// ----------------------------------------------------------------------------
uint32_t AudioPolicyManager::nextAudioPortGeneration()
{
    return mAudioPortGeneration++;
}

AudioPolicyManager::AudioPolicyManager(const sp<const AudioPolicyConfig>& config,
                                       EngineInstance&& engine,
                                       AudioPolicyClientInterface *clientInterface)
    :
    mUidCached(AID_AUDIOSERVER), // no need to call getuid(), there's only one of us running.
    mConfig(config),
    mEngine(std::move(engine)),
    mpClientInterface(clientInterface),
    mLimitRingtoneVolume(false), mLastVoiceVolume(-1.0f),
    mA2dpSuspended(false),
    mAudioPortGeneration(1),
    mBeaconMuteRefCount(0),
    mBeaconPlayingRefCount(0),
    mBeaconMuted(false),
    mTtsOutputAvailable(false),
    mMasterMono(false),
    mMusicEffectOutput(AUDIO_IO_HANDLE_NONE)
{
}

status_t AudioPolicyManager::initialize() {
    if (mEngine == nullptr) {
        return NO_INIT;
    }
    mEngine->setObserver(this);
    status_t status = mEngine->initCheck();
    if (status != NO_ERROR) {
        LOG_FATAL("Policy engine not initialized(err=%d)", status);
        return status;
    }

    // The actual device selection cache will be updated when calling `updateDevicesAndOutputs`
    // at the end of this function.
    mEngine->initializeDeviceSelectionCache();
    mCommunnicationStrategy = mEngine->getProductStrategyForAttributes(
        mEngine->getAttributesForStreamType(AUDIO_STREAM_VOICE_CALL));

    // after parsing the config, mConfig contain all known devices;
    // open all output streams needed to access attached devices
    onNewAudioModulesAvailableInt(nullptr /*newDevices*/);

    // make sure default device is reachable
    if (const auto defaultOutputDevice = mConfig->getDefaultOutputDevice();
            defaultOutputDevice == nullptr ||
            !mAvailableOutputDevices.contains(defaultOutputDevice)) {
        ALOGE_IF(defaultOutputDevice != nullptr, "Default device %s is unreachable",
                 defaultOutputDevice->toString().c_str());
        status = NO_INIT;
    }
    ALOGW_IF(mPrimaryOutput == nullptr, "The policy configuration does not declare a primary output");

    // Silence ALOGV statements
    property_set("log.tag." LOG_TAG, "D");

    updateDevicesAndOutputs();
    return status;
}

AudioPolicyManager::~AudioPolicyManager()
{
   for (size_t i = 0; i < mOutputs.size(); i++) {
        mOutputs.valueAt(i)->close();
   }
   for (size_t i = 0; i < mInputs.size(); i++) {
        mInputs.valueAt(i)->close();
   }
   mAvailableOutputDevices.clear();
   mAvailableInputDevices.clear();
   mOutputs.clear();
   mInputs.clear();
   mHwModules.clear();
   mManualSurroundFormats.clear();
   mConfig.clear();
}

status_t AudioPolicyManager::initCheck()
{
    return hasPrimaryOutput() ? NO_ERROR : NO_INIT;
}

// ---

void AudioPolicyManager::onNewAudioModulesAvailable()
{
    DeviceVector newDevices;
    onNewAudioModulesAvailableInt(&newDevices);
    if (!newDevices.empty()) {
        nextAudioPortGeneration();
        mpClientInterface->onAudioPortListUpdate();
    }
}

void AudioPolicyManager::onNewAudioModulesAvailableInt(DeviceVector *newDevices)
{
    for (const auto& hwModule : mConfig->getHwModules()) {
        if (std::find(mHwModules.begin(), mHwModules.end(), hwModule) != mHwModules.end()) {
            continue;
        }
        if (hwModule->getHandle() == AUDIO_MODULE_HANDLE_NONE) {
            if (audio_module_handle_t handle = mpClientInterface->loadHwModule(hwModule->getName());
                    handle != AUDIO_MODULE_HANDLE_NONE) {
                hwModule->setHandle(handle);
            } else {
                ALOGW("could not load HW module %s", hwModule->getName());
                continue;
            }
        }
        mHwModules.push_back(hwModule);
        // open all output streams needed to access attached devices.
        // direct outputs are closed immediately after checking the availability of attached devices
        // This also validates mAvailableOutputDevices list
        for (const auto& outProfile : hwModule->getOutputProfiles()) {
            if (!outProfile->canOpenNewIo()) {
                ALOGE("Invalid Output profile max open count %u for profile %s",
                      outProfile->maxOpenCount, outProfile->getTagName().c_str());
                continue;
            }
            if (!outProfile->hasSupportedDevices()) {
                ALOGW("Output profile contains no device on module %s", hwModule->getName());
                continue;
            }
            if ((outProfile->getFlags() & AUDIO_OUTPUT_FLAG_TTS) != 0 ||
                (outProfile->getFlags() & AUDIO_OUTPUT_FLAG_ULTRASOUND) != 0) {
                mTtsOutputAvailable = true;
            }

            const DeviceVector &supportedDevices = outProfile->getSupportedDevices();
            DeviceVector availProfileDevices = supportedDevices.filter(mConfig->getOutputDevices());
            sp<DeviceDescriptor> supportedDevice = 0;
            if (supportedDevices.contains(mConfig->getDefaultOutputDevice())) {
                supportedDevice = mConfig->getDefaultOutputDevice();
            } else {
                // choose first device present in profile's SupportedDevices also part of
                // mAvailableOutputDevices.
                if (availProfileDevices.isEmpty()) {
                    continue;
                }
                supportedDevice = availProfileDevices.itemAt(0);
            }
            if (!mConfig->getOutputDevices().contains(supportedDevice)) {
                continue;
            }

            if (outProfile->isMmap() && !outProfile->hasDynamicAudioProfile()
                && availProfileDevices.areAllDevicesAttached()) {
                ALOGV("%s skip opening output for mmap profile %s", __func__,
                        outProfile->getTagName().c_str());
                continue;
            }

            sp<SwAudioOutputDescriptor> outputDesc = new SwAudioOutputDescriptor(outProfile,
                                                                                 mpClientInterface);
            audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
            audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
            audio_attributes_t attributes = AUDIO_ATTRIBUTES_INITIALIZER;
            status_t status = outputDesc->open(nullptr /* halConfig */, nullptr /* mixerConfig */,
                                               DeviceVector(supportedDevice),
                                               AUDIO_STREAM_DEFAULT,
                                               &flags, &output, attributes);
            if (status != NO_ERROR) {
                ALOGW("Cannot open output stream for devices %s on hw module %s",
                      supportedDevice->toString().c_str(), hwModule->getName());
                continue;
            }
            for (const auto &device : availProfileDevices) {
                // give a valid ID to an attached device once confirmed it is reachable
                if (!device->isAttached()) {
                    device->attach(hwModule);
                    mAvailableOutputDevices.add(device);
                    device->setEncapsulationInfoFromHal(mpClientInterface);
                    if (newDevices) newDevices->add(device);
                    setEngineDeviceConnectionState(device, AUDIO_POLICY_DEVICE_STATE_AVAILABLE);
                }
            }
            if (mPrimaryOutput == nullptr &&
                    outProfile->getFlags() & AUDIO_OUTPUT_FLAG_PRIMARY) {
                mPrimaryOutput = outputDesc;
                mPrimaryModuleHandle = mPrimaryOutput->getModuleHandle();
            }
            if ((outProfile->getFlags() & AUDIO_OUTPUT_FLAG_DIRECT) != 0) {
                outputDesc->close();
            } else {
                addOutput(output, outputDesc);
                setOutputDevices(__func__, outputDesc,
                                 DeviceVector(supportedDevice),
                                 true,
                                 0,
                                 NULL);
            }
        }
        // open input streams needed to access attached devices to validate
        // mAvailableInputDevices list
        for (const auto& inProfile : hwModule->getInputProfiles()) {
            if (!inProfile->canOpenNewIo()) {
                ALOGE("Invalid Input profile max open count %u for profile %s",
                      inProfile->maxOpenCount, inProfile->getTagName().c_str());
                continue;
            }
            if (!inProfile->hasSupportedDevices()) {
                ALOGW("Input profile contains no device on module %s", hwModule->getName());
                continue;
            }
            // chose first device present in profile's SupportedDevices also part of
            // available input devices
            const DeviceVector &supportedDevices = inProfile->getSupportedDevices();
            DeviceVector availProfileDevices = supportedDevices.filter(mConfig->getInputDevices());
            if (availProfileDevices.isEmpty()) {
                ALOGV("%s: Input device list is empty! for profile %s",
                    __func__, inProfile->getTagName().c_str());
                continue;
            }

            if (inProfile->isMmap() && !inProfile->hasDynamicAudioProfile()
                && availProfileDevices.areAllDevicesAttached()) {
                ALOGV("%s skip opening input for mmap profile %s", __func__,
                        inProfile->getTagName().c_str());
                continue;
            }

            sp<AudioInputDescriptor> inputDesc = new AudioInputDescriptor(
                    inProfile, mpClientInterface, false /*isPreemptor*/);

            audio_io_handle_t input = AUDIO_IO_HANDLE_NONE;
            status_t status = inputDesc->open(nullptr,
                                              availProfileDevices.itemAt(0),
                                              AUDIO_SOURCE_MIC,
                                              (audio_input_flags_t) inProfile->getFlags(),
                                              &input);
            if (status != NO_ERROR) {
                ALOGW("%s: Cannot open input stream for device %s for profile %s on hw module %s",
                        __func__, availProfileDevices.toString().c_str(),
                        inProfile->getTagName().c_str(), hwModule->getName());
                continue;
            }
            for (const auto &device : availProfileDevices) {
                // give a valid ID to an attached device once confirmed it is reachable
                if (!device->isAttached()) {
                    device->attach(hwModule);
                    device->importAudioPortAndPickAudioProfile(inProfile, true);
                    mAvailableInputDevices.add(device);
                    if (newDevices) newDevices->add(device);
                    setEngineDeviceConnectionState(device, AUDIO_POLICY_DEVICE_STATE_AVAILABLE);
                }
            }
            inputDesc->close();
        }
    }

    // Check if spatializer outputs can be closed until used.
    // mOutputs vector never contains duplicated outputs at this point.
    std::vector<audio_io_handle_t> outputsClosed;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        if ((desc->mFlags & AUDIO_OUTPUT_FLAG_SPATIALIZER) != 0
                && !isOutputOnlyAvailableRouteToSomeDevice(desc)) {
            outputsClosed.push_back(desc->mIoHandle);
            nextAudioPortGeneration();
            ssize_t index = mAudioPatches.indexOfKey(desc->getPatchHandle());
            if (index >= 0) {
                sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
                (void) /*status_t status*/ mpClientInterface->releaseAudioPatch(
                            patchDesc->getAfHandle(), 0);
                mAudioPatches.removeItemsAt(index);
                mpClientInterface->onAudioPatchListUpdate();
            }
            desc->close();
        }
    }
    for (auto output : outputsClosed) {
        removeOutput(output);
    }
}

void AudioPolicyManager::addOutput(audio_io_handle_t output,
                                   const sp<SwAudioOutputDescriptor>& outputDesc)
{
    mOutputs.add(output, outputDesc);
    applyStreamVolumes(outputDesc, DeviceTypeSet(), 0 /* delayMs */, true /* force */);
    updateMono(output); // update mono status when adding to output list
    selectOutputForMusicEffects();
    nextAudioPortGeneration();
}

void AudioPolicyManager::removeOutput(audio_io_handle_t output)
{
    if (mPrimaryOutput != 0 && mPrimaryOutput == mOutputs.valueFor(output)) {
        ALOGV("%s: removing primary output", __func__);
        mPrimaryOutput = nullptr;
    }
    mOutputs.removeItem(output);
    selectOutputForMusicEffects();
}

void AudioPolicyManager::addInput(audio_io_handle_t input,
                                  const sp<AudioInputDescriptor>& inputDesc)
{
    mInputs.add(input, inputDesc);
    nextAudioPortGeneration();
}

status_t AudioPolicyManager::checkOutputsForDevice(const sp<DeviceDescriptor>& device,
                                                   audio_policy_dev_state_t state,
                                                   SortedVector<audio_io_handle_t>& outputs)
{
    audio_devices_t deviceType = device->type();
    const String8 &address = String8(device->address().c_str());
    sp<SwAudioOutputDescriptor> desc;

    if (audio_device_is_digital(deviceType)) {
        // erase all current sample rates, formats and channel masks
        device->clearAudioProfiles();
    }

    if (state == AUDIO_POLICY_DEVICE_STATE_AVAILABLE) {
        // first call getAudioPort to get the supported attributes from the HAL
        struct audio_port_v7 port = {};
        device->toAudioPort(&port);
        status_t status = mpClientInterface->getAudioPort(&port);
        if (status == NO_ERROR) {
            device->importAudioPort(port);
        }

        // then list already open outputs that can be routed to this device
        for (size_t i = 0; i < mOutputs.size(); i++) {
            desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated() && desc->supportsDevice(device)
                    && desc->devicesSupportEncodedFormats({deviceType})) {
                ALOGV("checkOutputsForDevice(): adding opened output %d on device %s",
                      mOutputs.keyAt(i), device->toString().c_str());
                outputs.add(mOutputs.keyAt(i));
            }
        }
        // then look for output profiles that can be routed to this device
        SortedVector< sp<IOProfile> > profiles;
        for (const auto& hwModule : mHwModules) {
            for (size_t j = 0; j < hwModule->getOutputProfiles().size(); j++) {
                sp<IOProfile> profile = hwModule->getOutputProfiles()[j];
                if (profile->supportsDevice(device)) {
                    profiles.add(profile);
                    ALOGV("%s(): adding profile %s from module %s",
                            __func__, profile->getTagName().c_str(), hwModule->getName());
                }
            }
        }

        ALOGV("  found %zu profiles, %zu outputs", profiles.size(), outputs.size());

        if (profiles.isEmpty() && outputs.isEmpty()) {
            ALOGW("checkOutputsForDevice(): No output available for device %04x", deviceType);
            return BAD_VALUE;
        }

        // open outputs for matching profiles if needed. Direct outputs are also opened to
        // query for dynamic parameters and will be closed later by setDeviceConnectionState()
        for (ssize_t profile_index = 0; profile_index < (ssize_t)profiles.size(); profile_index++) {
            sp<IOProfile> profile = profiles[profile_index];

            // nothing to do if one output is already opened for this profile
            size_t j;
            for (j = 0; j < outputs.size(); j++) {
                desc = mOutputs.valueFor(outputs.itemAt(j));
                if (!desc->isDuplicated() && desc->mProfile == profile) {
                    // matching profile: save the sample rates, format and channel masks supported
                    // by the profile in our device descriptor
                    if (audio_device_is_digital(deviceType)) {
                        device->importAudioPortAndPickAudioProfile(profile);
                    }
                    break;
                }
            }
            if (j != outputs.size()) {
                continue;
            }
            if (profile->isMmap() && !profile->hasDynamicAudioProfile()) {
                ALOGV("%s skip opening output for mmap profile %s",
                      __func__, profile->getTagName().c_str());
                continue;
            }
            if (!profile->canOpenNewIo()) {
                ALOGW("Max Output number %u already opened for this profile %s",
                      profile->maxOpenCount, profile->getTagName().c_str());
                continue;
            }

            ALOGV("opening output for device %08x with params %s profile %p name %s",
                  deviceType, address.c_str(), profile.get(), profile->getName().c_str());
            desc = openOutputWithProfileAndDevice(profile, DeviceVector(device));
            audio_io_handle_t output = desc == nullptr ? AUDIO_IO_HANDLE_NONE : desc->mIoHandle;
            if (output == AUDIO_IO_HANDLE_NONE) {
                ALOGW("checkOutputsForDevice() could not open output for device %x", deviceType);
                profiles.removeAt(profile_index);
                profile_index--;
            } else {
                outputs.add(output);
                // Load digital format info only for digital devices
                if (audio_device_is_digital(deviceType)) {
                    // TODO: when getAudioPort is ready, it may not be needed to import the audio
                    // port but just pick audio profile
                    device->importAudioPortAndPickAudioProfile(profile);
                }

                if (device_distinguishes_on_address(deviceType)) {
                    ALOGV("checkOutputsForDevice(): setOutputDevices %s",
                            device->toString().c_str());
                    setOutputDevices(__func__, desc, DeviceVector(device), true/*force*/,
                                      0/*delay*/, NULL/*patch handle*/);
                }
                ALOGV("checkOutputsForDevice(): adding output %d", output);
            }
        }

        if (profiles.isEmpty()) {
            ALOGW("checkOutputsForDevice(): No output available for device %04x", deviceType);
            return BAD_VALUE;
        }
    } else { // Disconnect
        // check if one opened output is not needed any more after disconnecting one device
        for (size_t i = 0; i < mOutputs.size(); i++) {
            desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated()) {
                // exact match on device
                if (device_distinguishes_on_address(deviceType) && desc->supportsDevice(device)
                        && desc->containsSingleDeviceSupportingEncodedFormats(device)) {
                    outputs.add(mOutputs.keyAt(i));
                } else if (!mAvailableOutputDevices.containsAtLeastOne(desc->supportedDevices())) {
                    ALOGV("checkOutputsForDevice(): disconnecting adding output %d",
                            mOutputs.keyAt(i));
                    outputs.add(mOutputs.keyAt(i));
                }
            }
        }
        // Clear any profiles associated with the disconnected device.
        for (const auto& hwModule : mHwModules) {
            for (size_t j = 0; j < hwModule->getOutputProfiles().size(); j++) {
                sp<IOProfile> profile = hwModule->getOutputProfiles()[j];
                if (!profile->supportsDevice(device)) {
                    continue;
                }
                ALOGV("%s(): clearing direct output profile %s on module %s",
                        __func__, profile->getTagName().c_str(), hwModule->getName());
                profile->clearAudioProfiles();
                if (!profile->hasDynamicAudioProfile()) {
                    continue;
                }
                // When a device is disconnected, if there is an IOProfile that contains dynamic
                // profiles and supports the disconnected device, call getAudioPort to repopulate
                // the capabilities of the devices that is supported by the IOProfile.
                for (const auto& supportedDevice : profile->getSupportedDevices()) {
                    if (supportedDevice == device ||
                            !mAvailableOutputDevices.contains(supportedDevice)) {
                        continue;
                    }
                    struct audio_port_v7 port;
                    supportedDevice->toAudioPort(&port);
                    status_t status = mpClientInterface->getAudioPort(&port);
                    if (status == NO_ERROR) {
                        supportedDevice->importAudioPort(port);
                    }
                }
            }
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::checkInputsForDevice(const sp<DeviceDescriptor>& device,
                                                  audio_policy_dev_state_t state)
{
    if (audio_device_is_digital(device->type())) {
        // erase all current sample rates, formats and channel masks
        device->clearAudioProfiles();
    }

    if (state == AUDIO_POLICY_DEVICE_STATE_AVAILABLE) {
        sp<AudioInputDescriptor> desc;

        // first call getAudioPort to get the supported attributes from the HAL
        struct audio_port_v7 port = {};
        device->toAudioPort(&port);
        status_t status = mpClientInterface->getAudioPort(&port);
        if (status == NO_ERROR) {
            device->importAudioPort(port);
        }

        // look for input profiles that can be routed to this device
        SortedVector< sp<IOProfile> > profiles;
        for (const auto& hwModule : mHwModules) {
            for (size_t profile_index = 0;
                 profile_index < hwModule->getInputProfiles().size();
                 profile_index++) {
                sp<IOProfile> profile = hwModule->getInputProfiles()[profile_index];

                if (profile->supportsDevice(device)) {
                    profiles.add(profile);
                    ALOGV("%s : adding profile %s from module %s", __func__,
                          profile->getTagName().c_str(), hwModule->getName());
                }
            }
        }

        if (profiles.isEmpty()) {
            ALOGW("%s: No input profile available for device %s",
                __func__, device->toString().c_str());
            return BAD_VALUE;
        }

        // open inputs for matching profiles if needed. Direct inputs are also opened to
        // query for dynamic parameters and will be closed later by setDeviceConnectionState()
        for (ssize_t profile_index = 0; profile_index < (ssize_t)profiles.size(); profile_index++) {

            sp<IOProfile> profile = profiles[profile_index];

            // nothing to do if one input is already opened for this profile
            size_t input_index;
            for (input_index = 0; input_index < mInputs.size(); input_index++) {
                desc = mInputs.valueAt(input_index);
                if (desc->mProfile == profile) {
                    if (audio_device_is_digital(device->type())) {
                        device->importAudioPortAndPickAudioProfile(profile);
                    }
                    break;
                }
            }
            if (input_index != mInputs.size()) {
                continue;
            }

            if (profile->isMmap() && !profile->hasDynamicAudioProfile()) {
                ALOGV("%s skip opening input for mmap profile %s",
                      __func__, profile->getTagName().c_str());
                continue;
            }
            if (!profile->canOpenNewIo()) {
                ALOGW("%s Max Input number %u already opened for this profile %s",
                      __func__, profile->maxOpenCount, profile->getTagName().c_str());
                continue;
            }

            desc = new AudioInputDescriptor(profile, mpClientInterface, false  /*isPreemptor*/);
            audio_io_handle_t input = AUDIO_IO_HANDLE_NONE;
            ALOGV("%s opening input for profile %s", __func__, profile->getTagName().c_str());
            status = desc->open(nullptr, device, AUDIO_SOURCE_MIC,
                                (audio_input_flags_t) profile->getFlags(), &input);

            if (status == NO_ERROR) {
                const String8& address = String8(device->address().c_str());
                if (!address.empty()) {
                    char *param = audio_device_address_to_parameter(device->type(), address);
                    mpClientInterface->setParameters(input, String8(param));
                    free(param);
                }
                updateAudioProfiles(device, input, profile);
                if (!profile->hasValidAudioProfile()) {
                    ALOGW("%s direct input missing param for profile %s", __func__,
                            profile->getTagName().c_str());
                    desc->close();
                    input = AUDIO_IO_HANDLE_NONE;
                }

                if (input != AUDIO_IO_HANDLE_NONE) {
                    addInput(input, desc);
                }
            } // endif input != 0

            if (input == AUDIO_IO_HANDLE_NONE) {
                ALOGW("%s could not open input for device %s on profile %s", __func__,
                       device->toString().c_str(), profile->getTagName().c_str());
                profiles.removeAt(profile_index);
                profile_index--;
            } else {
                if (audio_device_is_digital(device->type())) {
                    device->importAudioPortAndPickAudioProfile(profile);
                }
                ALOGV("%s: adding input %d for profile %s", __func__,
                        input, profile->getTagName().c_str());

                if (checkCloseInput(desc)) {
                    ALOGV("%s: closing input %d for profile %s", __func__,
                            input, profile->getTagName().c_str());
                    closeInput(input);
                }
            }
        } // end scan profiles

        if (profiles.isEmpty()) {
            ALOGW("%s: No input available for device %s", __func__,  device->toString().c_str());
            return BAD_VALUE;
        }
    } else {
        // Disconnect
        // Clear any profiles associated with the disconnected device.
        for (const auto& hwModule : mHwModules) {
            for (size_t profile_index = 0;
                 profile_index < hwModule->getInputProfiles().size();
                 profile_index++) {
                sp<IOProfile> profile = hwModule->getInputProfiles()[profile_index];
                if (profile->supportsDevice(device)) {
                    ALOGV("%s: clearing direct input profile %s on module %s", __func__,
                            profile->getTagName().c_str(), hwModule->getName());
                    profile->clearAudioProfiles();
                }
            }
        }
    } // end disconnect

    return NO_ERROR;
}


void AudioPolicyManager::closeOutput(audio_io_handle_t output)
{
    ALOGV("closeOutput(%d)", output);

    sp<SwAudioOutputDescriptor> closingOutput = mOutputs.valueFor(output);
    if (closingOutput == NULL) {
        ALOGW("closeOutput() unknown output %d", output);
        return;
    }
    const bool closingOutputWasActive = closingOutput->isActive();
    mPolicyMixes.closeOutput(closingOutput, mOutputs);

    // look for duplicated outputs connected to the output being removed.
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> dupOutput = mOutputs.valueAt(i);
        if (dupOutput->isDuplicated() &&
                (dupOutput->mOutput1 == closingOutput || dupOutput->mOutput2 == closingOutput)) {
            sp<SwAudioOutputDescriptor> remainingOutput =
                dupOutput->mOutput1 == closingOutput ? dupOutput->mOutput2 : dupOutput->mOutput1;
            // As all active tracks on duplicated output will be deleted,
            // and as they were also referenced on the other output, the reference
            // count for their stream type must be adjusted accordingly on
            // the other output.
            const bool wasActive = remainingOutput->isActive();
            // Note: no-op on the closing output where all clients has already been set inactive
            dupOutput->setAllClientsInactive();
            // stop() will be a no op if the output is still active but is needed in case all
            // active streams refcounts where cleared above
            if (wasActive) {
                remainingOutput->stop();
            }
            audio_io_handle_t duplicatedOutput = mOutputs.keyAt(i);
            ALOGV("closeOutput() closing also duplicated output %d", duplicatedOutput);

            mpClientInterface->closeOutput(duplicatedOutput);
            removeOutput(duplicatedOutput);
        }
    }

    nextAudioPortGeneration();

    ssize_t index = mAudioPatches.indexOfKey(closingOutput->getPatchHandle());
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        (void) /*status_t status*/ mpClientInterface->releaseAudioPatch(
                    patchDesc->getAfHandle(), 0);
        mAudioPatches.removeItemsAt(index);
        mpClientInterface->onAudioPatchListUpdate();
    }

    if (closingOutputWasActive) {
        closingOutput->stop();
    }
    closingOutput->close();
    if (closingOutput->isBitPerfect()) {
        for (const auto device : closingOutput->devices()) {
            device->setPreferredConfig(nullptr);
        }
    }

    removeOutput(output);
    mPreviousOutputs = mOutputs;
    if (closingOutput == mSpatializerOutput) {
        mSpatializerOutput.clear();
    }

    // MSD patches may have been released to support a non-MSD direct output. Reset MSD patch if
    // no direct outputs are open.
    if (!getMsdAudioOutDevices().isEmpty()) {
        bool directOutputOpen = false;
        for (size_t i = 0; i < mOutputs.size(); i++) {
            if (mOutputs[i]->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) {
                directOutputOpen = true;
                break;
            }
        }
        if (!directOutputOpen) {
            ALOGV("no direct outputs open, reset MSD patches");
            // TODO: The MSD patches to be established here may differ to current MSD patches due to
            // how output devices for patching are resolved. Avoid by caching and reusing the
            // arguments to mEngine->getOutputDevicesForAttributes() when resolving which output
            // devices to patch to. This may be complicated by the fact that devices may become
            // unavailable.
            setMsdOutputPatches();
        }
    }

    if (closingOutput->mPreferredAttrInfo != nullptr) {
        closingOutput->mPreferredAttrInfo->resetActiveClient();
    }
}

void AudioPolicyManager::closeInput(audio_io_handle_t input)
{
    ALOGV("closeInput(%d)", input);

    sp<AudioInputDescriptor> inputDesc = mInputs.valueFor(input);
    if (inputDesc == NULL) {
        ALOGW("closeInput() unknown input %d", input);
        return;
    }

    nextAudioPortGeneration();

    sp<DeviceDescriptor> device = inputDesc->getDevice();
    ssize_t index = mAudioPatches.indexOfKey(inputDesc->getPatchHandle());
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        (void) /*status_t status*/ mpClientInterface->releaseAudioPatch(
                    patchDesc->getAfHandle(), 0);
        mAudioPatches.removeItemsAt(index);
        mpClientInterface->onAudioPatchListUpdate();
    }

    mEffects.putOrphanEffectsForIo(input);
    inputDesc->close();
    mInputs.removeItem(input);

    DeviceVector primaryInputDevices = availablePrimaryModuleInputDevices();
    if (primaryInputDevices.contains(device) &&
            mInputs.activeInputsCountOnDevices(primaryInputDevices) == 0) {
        mpClientInterface->setSoundTriggerCaptureState(false);
    }
}

SortedVector<audio_io_handle_t> AudioPolicyManager::getOutputsForDevices(
            const DeviceVector &devices,
            const SwAudioOutputCollection& openOutputs)
{
    SortedVector<audio_io_handle_t> outputs;

    ALOGVV("%s() devices %s", __func__, devices.toString().c_str());
    for (size_t i = 0; i < openOutputs.size(); i++) {
        ALOGVV("output %zu isDuplicated=%d device=%s",
                i, openOutputs.valueAt(i)->isDuplicated(),
                openOutputs.valueAt(i)->supportedDevices().toString().c_str());
        if (openOutputs.valueAt(i)->supportsAllDevices(devices)
                && openOutputs.valueAt(i)->devicesSupportEncodedFormats(devices.types())) {
            ALOGVV("%s() found output %d", __func__, openOutputs.keyAt(i));
            outputs.add(openOutputs.keyAt(i));
        }
    }
    return outputs;
}

void AudioPolicyManager::checkForDeviceAndOutputChanges(std::function<bool()> onOutputsChecked)
{
    // checkA2dpSuspend must run before checkOutputForAllStrategies so that A2DP
    // output is suspended before any tracks are moved to it
    checkA2dpSuspend();
    checkOutputForAllStrategies();
    checkSecondaryOutputs();
    if (onOutputsChecked != nullptr && onOutputsChecked()) checkA2dpSuspend();
    updateDevicesAndOutputs();
    if (mHwModules.getModuleFromName(AUDIO_HARDWARE_MODULE_ID_MSD) != 0) {
        // TODO: The MSD patches to be established here may differ to current MSD patches due to how
        // output devices for patching are resolved. Nevertheless, AudioTracks affected by device
        // configuration changes will ultimately be rerouted correctly. We can still avoid
        // unnecessary rerouting by caching and reusing the arguments to
        // mEngine->getOutputDevicesForAttributes() when resolving which output devices to patch to.
        // This may be complicated by the fact that devices may become unavailable.
        setMsdOutputPatches();
    }
    // an event that changed routing likely occurred, inform upper layers
    mpClientInterface->onRoutingUpdated();
}

bool AudioPolicyManager::followsSameRouting(const audio_attributes_t &lAttr,
                                            const audio_attributes_t &rAttr) const
{
    return mEngine->getProductStrategyForAttributes(lAttr) ==
            mEngine->getProductStrategyForAttributes(rAttr);
}

void AudioPolicyManager::checkAudioSourceForAttributes(const audio_attributes_t &attr)
{
    for (size_t i = 0; i < mAudioSources.size(); i++)  {
        sp<SourceClientDescriptor> sourceDesc = mAudioSources.valueAt(i);
        if (sourceDesc != nullptr && followsSameRouting(attr, sourceDesc->attributes())
                && sourceDesc->getPatchHandle() == AUDIO_PATCH_HANDLE_NONE
                && !sourceDesc->isCallRx() && !sourceDesc->isInternal()) {
            connectAudioSource(sourceDesc, 0 /*delayMs*/);
        }
    }
}

void AudioPolicyManager::clearAudioSourcesForOutput(audio_io_handle_t output)
{
    for (size_t i = 0; i < mAudioSources.size(); i++)  {
        sp<SourceClientDescriptor> sourceDesc = mAudioSources.valueAt(i);
        if (sourceDesc != nullptr && sourceDesc->swOutput().promote() != nullptr
                && sourceDesc->swOutput().promote()->mIoHandle == output) {
            disconnectAudioSource(sourceDesc);
        }
    }
}

void AudioPolicyManager::checkOutputForAttributes(const audio_attributes_t &attr)
{
    auto psId = mEngine->getProductStrategyForAttributes(attr);

    DeviceVector oldDevices = mEngine->getOutputDevicesForAttributes(attr, 0, true /*fromCache*/);
    DeviceVector newDevices = mEngine->getOutputDevicesForAttributes(attr, 0, false /*fromCache*/);

    SortedVector<audio_io_handle_t> srcOutputs = getOutputsForDevices(oldDevices, mPreviousOutputs);
    SortedVector<audio_io_handle_t> dstOutputs = getOutputsForDevices(newDevices, mOutputs);

    uint32_t maxLatency = 0;
    bool unneededUsePrimaryOutputFromPolicyMixes = false;
    std::vector<sp<SwAudioOutputDescriptor>> invalidatedOutputs;
    // take into account dynamic audio policies related changes: if a client is now associated
    // to a different policy mix than at creation time, invalidate corresponding stream
    // invalidate clients on outputs that do not support all the newly selected devices for the
    // strategy
    for (size_t i = 0; i < mPreviousOutputs.size(); i++) {
        const sp<SwAudioOutputDescriptor>& desc = mPreviousOutputs.valueAt(i);
        if (desc->isDuplicated() || desc->getClientCount() == 0) {
            continue;
        }

        for (const sp<TrackClientDescriptor>& client : desc->getClientIterable()) {
            if (mEngine->getProductStrategyForAttributes(client->attributes()) != psId
                    || client->isInvalid()) {
                continue;
            }
            if (!desc->supportsAllDevices(newDevices)) {
                invalidatedOutputs.push_back(desc);
                break;
            }
            sp<AudioPolicyMix> primaryMix;
            status_t status = mPolicyMixes.getOutputForAttr(client->attributes(), client->config(),
                    client->uid(), client->session(), client->flags(), mAvailableOutputDevices,
                    nullptr /* requestedDevice */, primaryMix, nullptr /* secondaryMixes */,
                    unneededUsePrimaryOutputFromPolicyMixes);
            if (status == OK) {
                if (client->getPrimaryMix() != primaryMix || client->hasLostPrimaryMix()) {
                    if (desc->isStrategyActive(psId) && maxLatency < desc->latency()) {
                        maxLatency = desc->latency();
                    }
                    invalidatedOutputs.push_back(desc);
                    break;
                }
            }
        }
    }

    if (srcOutputs != dstOutputs || !invalidatedOutputs.empty()) {
        // get maximum latency of all source outputs to determine the minimum mute time guaranteeing
        // audio from invalidated tracks will be rendered when unmuting
        for (audio_io_handle_t srcOut : srcOutputs) {
            sp<SwAudioOutputDescriptor> desc = mPreviousOutputs.valueFor(srcOut);
            if (desc == nullptr) continue;
            if (desc == mSpatializerOutput && newDevices == oldDevices) {
                continue;
            }

            if (desc->isStrategyActive(psId) && maxLatency < desc->latency()) {
                maxLatency = desc->latency();
            }

            bool invalidate = false;
            for (auto client : desc->clientsList(false /*activeOnly*/)) {
                if (client->isInvalid()) {
                    continue;
                }
                if (desc->isDuplicated() || !desc->mProfile->isDirectOutput()) {
                    // a client on a non direct outputs has necessarily a linear PCM format
                    // so we can call selectOutput() safely
                    const audio_io_handle_t newOutput = selectOutput(dstOutputs,
                                                                     client->flags(),
                                                                     client->config().format,
                                                                     client->config().channel_mask,
                                                                     client->config().sample_rate,
                                                                     client->session());
                    if (newOutput != srcOut) {
                        invalidate = true;
                        break;
                    }
                } else {
                    sp<IOProfile> profile = getProfileForOutput(newDevices,
                                   client->config().sample_rate,
                                   client->config().format,
                                   client->config().channel_mask,
                                   client->flags(),
                                   true /* directOnly */);
                    if (profile != desc->mProfile) {
                        invalidate = true;
                        break;
                    }
                }
            }
            // mute strategy while moving tracks from one output to another
            if (invalidate) {
                invalidatedOutputs.push_back(desc);
                if (desc->isStrategyActive(psId)) {
                    setStrategyMute(psId, true, desc);
                    setStrategyMute(psId, false, desc, maxLatency * LATENCY_MUTE_FACTOR,
                                    newDevices.types());
                }
            }
            sp<SourceClientDescriptor> source = getSourceForAttributesOnOutput(srcOut, attr);
            if (source != nullptr && !source->isCallRx() && !source->isInternal()) {
                connectAudioSource(source, 0 /*delayMs*/);
            }
        }

        ALOGV_IF(!(srcOutputs.isEmpty() || dstOutputs.isEmpty()),
              "%s: strategy %d, moving from output %s to output %s", __func__, psId,
              std::to_string(srcOutputs[0]).c_str(),
              std::to_string(dstOutputs[0]).c_str());

        // Move effects associated to this stream from previous output to new output
        if (followsSameRouting(attr, attributes_initializer(AUDIO_USAGE_MEDIA))) {
            selectOutputForMusicEffects();
        }
        // Move tracks associated to this stream (and linked) from previous output to new output
        if (!invalidatedOutputs.empty()) {
            invalidateStreams(mEngine->getStreamTypesForProductStrategy(psId));
            for (sp<SwAudioOutputDescriptor> desc : invalidatedOutputs) {
                desc->setTracksInvalidatedStatusByStrategy(psId);
            }
        }
    }
}

void AudioPolicyManager::checkOutputForAllStrategies()
{
    for (const auto &strategy : mEngine->getOrderedProductStrategies()) {
        auto attributes = mEngine->getAllAttributesForProductStrategy(strategy).front();
        checkOutputForAttributes(attributes);
        checkAudioSourceForAttributes(attributes);
    }
}

void AudioPolicyManager::checkSecondaryOutputs() {
    PortHandleVector clientsToInvalidate;
    TrackSecondaryOutputsMap trackSecondaryOutputs;
    bool unneededUsePrimaryOutputFromPolicyMixes = false;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        const sp<SwAudioOutputDescriptor>& outputDescriptor = mOutputs[i];
        for (const sp<TrackClientDescriptor>& client : outputDescriptor->getClientIterable()) {
            sp<AudioPolicyMix> primaryMix;
            std::vector<sp<AudioPolicyMix>> secondaryMixes;
            status_t status = mPolicyMixes.getOutputForAttr(client->attributes(), client->config(),
                    client->uid(), client->session(), client->flags(), mAvailableOutputDevices,
                    nullptr /* requestedDevice */, primaryMix, &secondaryMixes,
                    unneededUsePrimaryOutputFromPolicyMixes);
            std::vector<sp<SwAudioOutputDescriptor>> secondaryDescs;
            for (auto &secondaryMix : secondaryMixes) {
                sp<SwAudioOutputDescriptor> outputDesc = secondaryMix->getOutput();
                if (outputDesc != nullptr &&
                    outputDesc->mIoHandle != AUDIO_IO_HANDLE_NONE &&
                    outputDesc != outputDescriptor) {
                    secondaryDescs.push_back(outputDesc);
                }
            }

            if (status != OK &&
                (client->flags() & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ) == AUDIO_OUTPUT_FLAG_NONE) {
                // When it failed to query secondary output, only invalidate the client that is not
                // MMAP. The reason is that MMAP stream will not support secondary output.
                clientsToInvalidate.push_back(client->portId());
            } else if (!std::equal(
                    client->getSecondaryOutputs().begin(),
                    client->getSecondaryOutputs().end(),
                    secondaryDescs.begin(), secondaryDescs.end())) {
                if (client->flags() & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD
                        || !audio_is_linear_pcm(client->config().format)) {
                    // If the format is not PCM, the tracks should be invalidated to get correct
                    // behavior when the secondary output is changed.
                    clientsToInvalidate.push_back(client->portId());
                } else {
                    std::vector<wp<SwAudioOutputDescriptor>> weakSecondaryDescs;
                    std::vector<audio_io_handle_t> secondaryOutputIds;
                    for (const auto &secondaryDesc: secondaryDescs) {
                        secondaryOutputIds.push_back(secondaryDesc->mIoHandle);
                        weakSecondaryDescs.push_back(secondaryDesc);
                    }
                    trackSecondaryOutputs.emplace(client->portId(), secondaryOutputIds);
                    client->setSecondaryOutputs(std::move(weakSecondaryDescs));
                }
            }
        }
    }
    if (!trackSecondaryOutputs.empty()) {
        mpClientInterface->updateSecondaryOutputs(trackSecondaryOutputs);
    }
    if (!clientsToInvalidate.empty()) {
        ALOGD("%s Invalidate clients due to fail getting output for attr", __func__);
        mpClientInterface->invalidateTracks(clientsToInvalidate);
    }
}

bool AudioPolicyManager::isScoRequestedForComm() const {
    AudioDeviceTypeAddrVector devices;
    mEngine->getDevicesForRoleAndStrategy(mCommunnicationStrategy, DEVICE_ROLE_PREFERRED, devices);
    for (const auto &device : devices) {
        if (audio_is_bluetooth_out_sco_device(device.mType)) {
            return true;
        }
    }
    return false;
}

bool AudioPolicyManager::isHearingAidUsedForComm() const {
    DeviceVector devices = mEngine->getOutputDevicesForStream(AUDIO_STREAM_VOICE_CALL,
                                                       true /*fromCache*/);
    for (const auto &device : devices) {
        if (device->type() == AUDIO_DEVICE_OUT_HEARING_AID) {
            return true;
        }
    }
    return false;
}


void AudioPolicyManager::checkA2dpSuspend()
{
    audio_io_handle_t a2dpOutput = mOutputs.getA2dpOutput();
    if (a2dpOutput == 0 || mOutputs.isA2dpOffloadedOnPrimary()) {
        mA2dpSuspended = false;
        return;
    }

    bool isScoConnected =
            (mAvailableInputDevices.types().count(AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) != 0 ||
             !Intersection(mAvailableOutputDevices.types(), getAudioDeviceOutAllScoSet()).empty());
    bool isScoRequested = isScoRequestedForComm();

    // if suspended, restore A2DP output if:
    //      ((SCO device is NOT connected) ||
    //       ((SCO is not requested) &&
    //        (phone state is NOT in call) && (phone state is NOT ringing)))
    //
    // if not suspended, suspend A2DP output if:
    //      (SCO device is connected) &&
    //       ((SCO is requested) ||
    //       ((phone state is in call) || (phone state is ringing)))
    //
    if (mA2dpSuspended) {
        if (!isScoConnected ||
             (!isScoRequested &&
              (mEngine->getPhoneState() != AUDIO_MODE_IN_CALL) &&
              (mEngine->getPhoneState() != AUDIO_MODE_RINGTONE))) {

            mpClientInterface->restoreOutput(a2dpOutput);
            mA2dpSuspended = false;
        }
    } else {
        if (isScoConnected &&
             (isScoRequested ||
              (mEngine->getPhoneState() == AUDIO_MODE_IN_CALL) ||
              (mEngine->getPhoneState() == AUDIO_MODE_RINGTONE))) {

            mpClientInterface->suspendOutput(a2dpOutput);
            mA2dpSuspended = true;
        }
    }
}

DeviceVector AudioPolicyManager::getNewOutputDevices(const sp<SwAudioOutputDescriptor>& outputDesc,
                                                     bool fromCache)
{
    if (outputDesc == nullptr) {
        return DeviceVector{};
    }

    ssize_t index = mAudioPatches.indexOfKey(outputDesc->getPatchHandle());
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        if (patchDesc->getUid() != mUidCached) {
            ALOGV("%s device %s forced by patch %d", __func__,
                  outputDesc->devices().toString().c_str(), outputDesc->getPatchHandle());
            return  outputDesc->devices();
        }
    }

    // Do not retrieve engine device for outputs through MSD
    // TODO: support explicit routing requests by resetting MSD patch to engine device.
    if (outputDesc->devices() == getMsdAudioOutDevices()) {
        return outputDesc->devices();
    }

    // Honor explicit routing requests only if no client using default routing is active on this
    // input: a specific app can not force routing for other apps by setting a preferred device.
    bool active; // unused
    sp<DeviceDescriptor> device =
        findPreferredDevice(outputDesc, PRODUCT_STRATEGY_NONE, active, mAvailableOutputDevices);
    if (device != nullptr) {
        return DeviceVector(device);
    }

    // Legacy Engine cannot take care of bus devices and mix, so we need to handle the conflict
    // of setForceUse / Default Bus device here
    device = mPolicyMixes.getDeviceAndMixForOutput(outputDesc, mAvailableOutputDevices);
    if (device != nullptr) {
        return DeviceVector(device);
    }

    DeviceVector devices;
    for (const auto &productStrategy : mEngine->getOrderedProductStrategies()) {
        StreamTypeVector streams = mEngine->getStreamTypesForProductStrategy(productStrategy);
        auto hasStreamActive = [&](auto stream) {
            return hasStream(streams, stream) && isStreamActive(stream, 0);
        };

        auto doGetOutputDevicesForVoice = [&]() {
            return hasVoiceStream(streams) && (outputDesc == mPrimaryOutput ||
                outputDesc->isActive(toVolumeSource(AUDIO_STREAM_VOICE_CALL, false))) &&
                (isInCall() ||
                 mOutputs.isStrategyActiveOnSameModule(productStrategy, outputDesc)) &&
                !isStreamActive(AUDIO_STREAM_ENFORCED_AUDIBLE, 0);
        };

        // With low-latency playing on speaker, music on WFD, when the first low-latency
        // output is stopped, getNewOutputDevices checks for a product strategy
        // from the list, as STRATEGY_SONIFICATION comes prior to STRATEGY_MEDIA.
        // If an ALARM or ENFORCED_AUDIBLE stream is supported by the product strategy,
        // devices are returned for STRATEGY_SONIFICATION without checking whether the
        // stream is associated to the output descriptor.
        if (doGetOutputDevicesForVoice() || outputDesc->isStrategyActive(productStrategy) ||
               ((hasStreamActive(AUDIO_STREAM_ALARM) ||
                hasStreamActive(AUDIO_STREAM_ENFORCED_AUDIBLE)) &&
                mOutputs.isStrategyActiveOnSameModule(productStrategy, outputDesc))) {
            // Retrieval of devices for voice DL is done on primary output profile, cannot
            // check the route (would force modifying configuration file for this profile)
            auto attr = mEngine->getAllAttributesForProductStrategy(productStrategy).front();
            devices = mEngine->getOutputDevicesForAttributes(attr, nullptr, fromCache);
            break;
        }
    }
    ALOGV("%s selected devices %s", __func__, devices.toString().c_str());
    return devices;
}

sp<DeviceDescriptor> AudioPolicyManager::getNewInputDevice(
        const sp<AudioInputDescriptor>& inputDesc)
{
    sp<DeviceDescriptor> device;

    ssize_t index = mAudioPatches.indexOfKey(inputDesc->getPatchHandle());
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        if (patchDesc->getUid() != mUidCached) {
            ALOGV("getNewInputDevice() device %s forced by patch %d",
                  inputDesc->getDevice()->toString().c_str(), inputDesc->getPatchHandle());
            return inputDesc->getDevice();
        }
    }

    // Honor explicit routing requests only if no client using default routing is active on this
    // input or if all active clients are from the same app: a specific app can not force routing
    // for other apps by setting a preferred device.
    bool active;
    device = findPreferredDevice(inputDesc, AUDIO_SOURCE_DEFAULT, active, mAvailableInputDevices);
    if (device != nullptr) {
        return device;
    }

    // If we are not in call and no client is active on this input, this methods returns
    // a null sp<>, causing the patch on the input stream to be released.
    audio_attributes_t attributes;
    uid_t uid;
    audio_session_t session;
    sp<RecordClientDescriptor> topClient = inputDesc->getHighestPriorityClient();
    if (topClient != nullptr) {
        attributes = topClient->attributes();
        uid = topClient->uid();
        session = topClient->session();
    } else {
        attributes = { .source = AUDIO_SOURCE_DEFAULT };
        uid = 0;
        session = AUDIO_SESSION_NONE;
    }

    if (attributes.source == AUDIO_SOURCE_DEFAULT && isInCall()) {
        attributes.source = AUDIO_SOURCE_VOICE_COMMUNICATION;
    }
    if (attributes.source != AUDIO_SOURCE_DEFAULT) {
        device = mEngine->getInputDeviceForAttributes(
                attributes, false /*ignorePreferredDevice*/, uid, session);
    }

    return device;
}

bool AudioPolicyManager::streamsMatchForvolume(audio_stream_type_t stream1,
                                               audio_stream_type_t stream2) {
    return (stream1 == stream2);
}

status_t AudioPolicyManager::getDevicesForAttributes(
        const audio_attributes_t &attr, AudioDeviceTypeAddrVector *devices, bool forVolume) {
    if (devices == nullptr) {
        return BAD_VALUE;
    }

    DeviceVector curDevices;
    if (status_t status = getDevicesForAttributes(attr, curDevices, forVolume); status != OK) {
        return status;
    }
    for (const auto& device : curDevices) {
        devices->push_back(device->getDeviceTypeAddr());
    }
    return NO_ERROR;
}

void AudioPolicyManager::handleNotificationRoutingForStream(audio_stream_type_t stream) {
    switch(stream) {
    case AUDIO_STREAM_MUSIC:
        checkOutputForAttributes(attributes_initializer(AUDIO_USAGE_NOTIFICATION));
        updateDevicesAndOutputs();
        break;
    default:
        break;
    }
}

uint32_t AudioPolicyManager::handleEventForBeacon(int event) {

    // skip beacon mute management if a dedicated TTS output is available
    if (mTtsOutputAvailable) {
        return 0;
    }

    switch(event) {
    case STARTING_OUTPUT:
        mBeaconMuteRefCount++;
        break;
    case STOPPING_OUTPUT:
        if (mBeaconMuteRefCount > 0) {
            mBeaconMuteRefCount--;
        }
        break;
    case STARTING_BEACON:
        mBeaconPlayingRefCount++;
        break;
    case STOPPING_BEACON:
        if (mBeaconPlayingRefCount > 0) {
            mBeaconPlayingRefCount--;
        }
        break;
    }

    if (mBeaconMuteRefCount > 0) {
        // any playback causes beacon to be muted
        return setBeaconMute(true);
    } else {
        // no other playback: unmute when beacon starts playing, mute when it stops
        return setBeaconMute(mBeaconPlayingRefCount == 0);
    }
}

uint32_t AudioPolicyManager::setBeaconMute(bool mute) {
    ALOGV("setBeaconMute(%d) mBeaconMuteRefCount=%d mBeaconPlayingRefCount=%d",
            mute, mBeaconMuteRefCount, mBeaconPlayingRefCount);
    // keep track of muted state to avoid repeating mute/unmute operations
    if (mBeaconMuted != mute) {
        // mute/unmute AUDIO_STREAM_TTS on all outputs
        ALOGV("\t muting %d", mute);
        uint32_t maxLatency = 0;
        auto ttsVolumeSource = toVolumeSource(AUDIO_STREAM_TTS, false);
        if (ttsVolumeSource == VOLUME_SOURCE_NONE) {
            ALOGV("\t no tts volume source available");
            return 0;
        }
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            setVolumeSourceMutedInternally(ttsVolumeSource, mute/*on*/, desc, 0 /*delay*/,
                                           DeviceTypeSet());
            const uint32_t latency = desc->latency() * 2;
            if (desc->isActive(latency * 2) && latency > maxLatency) {
                maxLatency = latency;
            }
        }
        mBeaconMuted = mute;
        return maxLatency;
    }
    return 0;
}

void AudioPolicyManager::updateDevicesAndOutputs()
{
    mEngine->updateDeviceSelectionCache();
    mPreviousOutputs = mOutputs;
}

uint32_t AudioPolicyManager::checkDeviceMuteStrategies(const sp<AudioOutputDescriptor>& outputDesc,
                                                       const DeviceVector &prevDevices,
                                                       uint32_t delayMs)
{
    // mute/unmute strategies using an incompatible device combination
    // if muting, wait for the audio in pcm buffer to be drained before proceeding
    // if unmuting, unmute only after the specified delay
    if (outputDesc->isDuplicated()) {
        return 0;
    }

    uint32_t muteWaitMs = 0;
    DeviceVector devices = outputDesc->devices();
    bool shouldMute = outputDesc->isActive() && (devices.size() >= 2);

    auto productStrategies = mEngine->getOrderedProductStrategies();
    for (const auto &productStrategy : productStrategies) {
        auto attributes = mEngine->getAllAttributesForProductStrategy(productStrategy).front();
        DeviceVector curDevices =
                mEngine->getOutputDevicesForAttributes(attributes, nullptr, false/*fromCache*/);
        curDevices = curDevices.filter(outputDesc->supportedDevices());
        bool mute = shouldMute && curDevices.containsAtLeastOne(devices) && curDevices != devices;
        bool doMute = false;

        if (mute && !outputDesc->isStrategyMutedByDevice(productStrategy)) {
            doMute = true;
            outputDesc->setStrategyMutedByDevice(productStrategy, true);
        } else if (!mute && outputDesc->isStrategyMutedByDevice(productStrategy)) {
            doMute = true;
            outputDesc->setStrategyMutedByDevice(productStrategy, false);
        }
        if (doMute) {
            for (size_t j = 0; j < mOutputs.size(); j++) {
                sp<AudioOutputDescriptor> desc = mOutputs.valueAt(j);
                // skip output if it does not share any device with current output
                if (!desc->supportedDevices().containsAtLeastOne(outputDesc->supportedDevices())) {
                    continue;
                }
                ALOGVV("%s() output %s %s (curDevice %s)", __func__, desc->info().c_str(),
                      mute ? "muting" : "unmuting", curDevices.toString().c_str());
                setStrategyMute(productStrategy, mute, desc, mute ? 0 : delayMs);
                if (desc->isStrategyActive(productStrategy)) {
                    if (mute) {
                        // FIXME: should not need to double latency if volume could be applied
                        // immediately by the audioflinger mixer. We must account for the delay
                        // between now and the next time the audioflinger thread for this output
                        // will process a buffer (which corresponds to one buffer size,
                        // usually 1/2 or 1/4 of the latency).
                        if (muteWaitMs < desc->latency() * 2) {
                            muteWaitMs = desc->latency() * 2;
                        }
                    }
                }
            }
        }
    }

    // temporary mute output if device selection changes to avoid volume bursts due to
    // different per device volumes
    if (outputDesc->isActive() && (devices != prevDevices)) {
        uint32_t tempMuteWaitMs = outputDesc->latency() * 2;

        if (muteWaitMs < tempMuteWaitMs) {
            muteWaitMs = tempMuteWaitMs;
        }

        // If recommended duration is defined, replace temporary mute duration to avoid
        // truncated notifications at beginning, which depends on duration of changing path in HAL.
        // Otherwise, temporary mute duration is conservatively set to 4 times the reported latency.
        uint32_t tempRecommendedMuteDuration = outputDesc->getRecommendedMuteDurationMs();
        uint32_t tempMuteDurationMs = tempRecommendedMuteDuration > 0 ?
                tempRecommendedMuteDuration : outputDesc->latency() * 4;

        for (const auto &activeVs : outputDesc->getActiveVolumeSources()) {
            // make sure that we do not start the temporary mute period too early in case of
            // delayed device change
            setVolumeSourceMutedInternally(activeVs, true, outputDesc, delayMs);
            setVolumeSourceMutedInternally(activeVs, false, outputDesc,
                                           delayMs + tempMuteDurationMs,
                                           devices.types());
        }
    }

    // wait for the PCM output buffers to empty before proceeding with the rest of the command
    if (muteWaitMs > delayMs) {
        muteWaitMs -= delayMs;
        usleep(muteWaitMs * 1000);
        return muteWaitMs;
    }
    return 0;
}

uint32_t AudioPolicyManager::setOutputDevices(const char *caller,
                                              const sp<SwAudioOutputDescriptor>& outputDesc,
                                              const DeviceVector &devices,
                                              bool force,
                                              int delayMs,
                                              audio_patch_handle_t *patchHandle,
                                              bool requiresMuteCheck, bool requiresVolumeCheck,
                                              bool skipMuteDelay)
{
    // TODO(b/262404095): Consider if the output need to be reopened.
    std::string logPrefix = std::string("caller ") + caller + outputDesc->info();
    ALOGV("%s %s device %s delayMs %d", __func__, logPrefix.c_str(),
          devices.toString().c_str(), delayMs);
    uint32_t muteWaitMs;

    if (outputDesc->isDuplicated()) {
        muteWaitMs = setOutputDevices(__func__, outputDesc->subOutput1(), devices, force, delayMs,
                nullptr /* patchHandle */, requiresMuteCheck, skipMuteDelay);
        muteWaitMs += setOutputDevices(__func__, outputDesc->subOutput2(), devices, force, delayMs,
                nullptr /* patchHandle */, requiresMuteCheck, skipMuteDelay);
        return muteWaitMs;
    }

    // filter devices according to output selected
    DeviceVector filteredDevices = outputDesc->filterSupportedDevices(devices);
    DeviceVector prevDevices = outputDesc->devices();
    DeviceVector availPrevDevices = mAvailableOutputDevices.filter(prevDevices);

    ALOGV("%s %s prevDevice %s", __func__, logPrefix.c_str(),
          prevDevices.toString().c_str());

    if (!filteredDevices.isEmpty()) {
        outputDesc->setDevices(filteredDevices);
    }

    // if the outputs are not materially active, there is no need to mute.
    if (requiresMuteCheck) {
        muteWaitMs = checkDeviceMuteStrategies(outputDesc, prevDevices, delayMs);
    } else {
        ALOGV("%s: %s suppressing checkDeviceMuteStrategies", __func__,
              logPrefix.c_str());
        muteWaitMs = 0;
    }

    bool outputRouted = outputDesc->isRouted();

    // no need to proceed if new device is not AUDIO_DEVICE_NONE and not supported by current
    // output profile or if new device is not supported AND previous device(s) is(are) still
    // available (otherwise reset device must be done on the output)
    if (!devices.isEmpty() && filteredDevices.isEmpty() && !availPrevDevices.empty()) {
        ALOGV("%s: %s unsupported device %s for output", __func__, logPrefix.c_str(),
              devices.toString().c_str());
        // restore previous device after evaluating strategy mute state
        outputDesc->setDevices(prevDevices);
        applyStreamVolumes(outputDesc, prevDevices.types(), delayMs, true /*force*/);
        return muteWaitMs;
    }

    // Do not change the routing if:
    //      the requested device is AUDIO_DEVICE_NONE
    //      OR the requested device is the same as current device
    //  AND force is not specified
    //  AND the output is connected by a valid audio patch.
    // Doing this check here allows the caller to call setOutputDevices() without conditions
    if ((filteredDevices.isEmpty() || filteredDevices == prevDevices) && !force && outputRouted) {
        ALOGV("%s %s setting same device %s or null device, force=%d, patch handle=%d",
              __func__, logPrefix.c_str(), filteredDevices.toString().c_str(), force,
              outputDesc->getPatchHandle());
        if (requiresVolumeCheck && !filteredDevices.isEmpty()) {
            ALOGV("%s %s setting same device on routed output, force apply volumes",
                  __func__, logPrefix.c_str());
            applyStreamVolumes(outputDesc, filteredDevices.types(), delayMs, true /*force*/);
        }
        return muteWaitMs;
    }

    ALOGV("%s %s changing device to %s", __func__, logPrefix.c_str(),
          filteredDevices.toString().c_str());

    // do the routing
    if (filteredDevices.isEmpty() || mAvailableOutputDevices.filter(filteredDevices).empty()) {
        resetOutputDevice(outputDesc, delayMs, NULL);
    } else {
        PatchBuilder patchBuilder;
        patchBuilder.addSource(outputDesc);
        ALOG_ASSERT(filteredDevices.size() <= AUDIO_PATCH_PORTS_MAX, "Too many sink ports");
        for (const auto &filteredDevice : filteredDevices) {
            patchBuilder.addSink(filteredDevice);
        }

        // Add half reported latency to delayMs when muteWaitMs is null in order
        // to avoid disordered sequence of muting volume and changing devices.
        int actualDelayMs = !skipMuteDelay && muteWaitMs == 0
                ? (delayMs + (outputDesc->latency() / 2)) : delayMs;
        installPatch(__func__, patchHandle, outputDesc.get(), patchBuilder.patch(), actualDelayMs);
    }

    // Since the mute is skip, also skip the apply stream volume as that will be applied externally
    if (!skipMuteDelay) {
        // update stream volumes according to new device
        applyStreamVolumes(outputDesc, filteredDevices.types(), delayMs);
    }

    return muteWaitMs;
}

status_t AudioPolicyManager::resetOutputDevice(const sp<AudioOutputDescriptor>& outputDesc,
                                               int delayMs,
                                               audio_patch_handle_t *patchHandle)
{
    ssize_t index;
    if (patchHandle == nullptr && !outputDesc->isRouted()) {
        return INVALID_OPERATION;
    }
    if (patchHandle) {
        index = mAudioPatches.indexOfKey(*patchHandle);
    } else {
        index = mAudioPatches.indexOfKey(outputDesc->getPatchHandle());
    }
    if (index < 0) {
        return INVALID_OPERATION;
    }
    sp< AudioPatch> patchDesc = mAudioPatches.valueAt(index);
    status_t status = mpClientInterface->releaseAudioPatch(patchDesc->getAfHandle(), delayMs);
    ALOGV("resetOutputDevice() releaseAudioPatch returned %d", status);
    outputDesc->setPatchHandle(AUDIO_PATCH_HANDLE_NONE);
    removeAudioPatch(patchDesc->getHandle());
    nextAudioPortGeneration();
    mpClientInterface->onAudioPatchListUpdate();
    return status;
}

status_t AudioPolicyManager::setInputDevice(audio_io_handle_t input,
                                            const sp<DeviceDescriptor> &device,
                                            bool force,
                                            audio_patch_handle_t *patchHandle)
{
    status_t status = NO_ERROR;

    sp<AudioInputDescriptor> inputDesc = mInputs.valueFor(input);
    if ((device != nullptr) && ((device != inputDesc->getDevice()) || force)) {
        inputDesc->setDevice(device);

        if (mAvailableInputDevices.contains(device)) {
            PatchBuilder patchBuilder;
            patchBuilder.addSink(inputDesc,
            // AUDIO_SOURCE_HOTWORD is for internal use only:
            // handled as AUDIO_SOURCE_VOICE_RECOGNITION by the audio HAL
                    [inputDesc](const PatchBuilder::mix_usecase_t& usecase) {
                        auto result = usecase;
                        if (result.source == AUDIO_SOURCE_HOTWORD && !inputDesc->isSoundTrigger()) {
                            result.source = AUDIO_SOURCE_VOICE_RECOGNITION;
                        }
                        return result; });
            //only one input device for now
            if (audio_is_remote_submix_device(device->type())) {
                // remote submix HAL does not support audio conversion, need source device
                // audio config to match the sink input descriptor audio config, otherwise AIDL
                // HAL patching will fail
                audio_port_config srcDevicePortConfig = {};
                device->toAudioPortConfig(&srcDevicePortConfig, nullptr);
                srcDevicePortConfig.sample_rate = inputDesc->getSamplingRate();
                srcDevicePortConfig.channel_mask = inputDesc->getChannelMask();
                srcDevicePortConfig.format = inputDesc->getFormat();
                patchBuilder.addSource(srcDevicePortConfig);
            } else {
                patchBuilder.addSource(device);
            }
            status = installPatch(__func__, patchHandle, inputDesc.get(), patchBuilder.patch(), 0);
        }
    }
    return status;
}

status_t AudioPolicyManager::resetInputDevice(audio_io_handle_t input,
                                              audio_patch_handle_t *patchHandle)
{
    sp<AudioInputDescriptor> inputDesc = mInputs.valueFor(input);
    ssize_t index;
    if (patchHandle) {
        index = mAudioPatches.indexOfKey(*patchHandle);
    } else {
        index = mAudioPatches.indexOfKey(inputDesc->getPatchHandle());
    }
    if (index < 0) {
        return INVALID_OPERATION;
    }
    sp< AudioPatch> patchDesc = mAudioPatches.valueAt(index);
    status_t status = mpClientInterface->releaseAudioPatch(patchDesc->getAfHandle(), 0);
    ALOGV("resetInputDevice() releaseAudioPatch returned %d", status);
    inputDesc->setPatchHandle(AUDIO_PATCH_HANDLE_NONE);
    removeAudioPatch(patchDesc->getHandle());
    nextAudioPortGeneration();
    mpClientInterface->onAudioPatchListUpdate();
    return status;
}

sp<IOProfile> AudioPolicyManager::getInputProfile(const sp<DeviceDescriptor> &device,
                                                  uint32_t& samplingRate,
                                                  audio_format_t& format,
                                                  audio_channel_mask_t& channelMask,
                                                  audio_input_flags_t flags)
{
    // Choose an input profile based on the requested capture parameters: select the first available
    // profile supporting all requested parameters.
    // The flags can be ignored if it doesn't contain a much match flag.

    using underlying_input_flag_t = std::underlying_type_t<audio_input_flags_t>;
    const underlying_input_flag_t mustMatchFlag = AUDIO_INPUT_FLAG_MMAP_NOIRQ |
                         AUDIO_INPUT_FLAG_HOTWORD_TAP | AUDIO_INPUT_FLAG_HW_LOOKBACK;

    const underlying_input_flag_t oriFlags = flags;

    for (;;) {
        sp<IOProfile> inexact = nullptr;
        uint32_t inexactSamplingRate = 0;
        audio_format_t inexactFormat = AUDIO_FORMAT_INVALID;
        audio_channel_mask_t inexactChannelMask = AUDIO_CHANNEL_INVALID;
        uint32_t updatedSamplingRate = 0;
        audio_format_t updatedFormat = AUDIO_FORMAT_INVALID;
        audio_channel_mask_t updatedChannelMask = AUDIO_CHANNEL_INVALID;
        auto bestCompatibleScore = IOProfile::NO_MATCH;
        for (const auto& hwModule : mHwModules) {
            for (const auto& profile : hwModule->getInputProfiles()) {
                // profile->log();
                //updatedFormat = format;
                auto compatibleScore = profile->getCompatibilityScore(
                        DeviceVector(device),
                        samplingRate,
                        &updatedSamplingRate,
                        format,
                        &updatedFormat,
                        channelMask,
                        &updatedChannelMask,
                        // FIXME ugly cast
                        (audio_output_flags_t) flags);
                if (compatibleScore == IOProfile::EXACT_MATCH) {
                    samplingRate = updatedSamplingRate;
                    format = updatedFormat;
                    channelMask = updatedChannelMask;
                    return profile;
                } else if ((flags != AUDIO_INPUT_FLAG_NONE
                        && compatibleScore == IOProfile::PARTIAL_MATCH_WITH_FLAG)
                    || (inexact == nullptr && compatibleScore != IOProfile::NO_MATCH)) {
                    if (compatibleScore > bestCompatibleScore) {
                        inexact = profile;
                        inexactSamplingRate = updatedSamplingRate;
                        inexactFormat = updatedFormat;
                        inexactChannelMask = updatedChannelMask;
                        bestCompatibleScore = compatibleScore;
                    }
                }
            }
        }

        if (inexact != nullptr) {
            samplingRate = inexactSamplingRate;
            format = inexactFormat;
            channelMask = inexactChannelMask;
            return inexact;
        } else if (flags & AUDIO_INPUT_FLAG_RAW) {
            flags = (audio_input_flags_t) (flags & ~AUDIO_INPUT_FLAG_RAW); // retry
        } else if ((flags & mustMatchFlag) == AUDIO_INPUT_FLAG_NONE &&
                flags != AUDIO_INPUT_FLAG_NONE && audio_is_linear_pcm(format)) {
            flags = AUDIO_INPUT_FLAG_NONE;
        } else { // fail
            ALOGW("%s could not find profile for device %s, sampling rate %u, format %#x, "
                  "channel mask 0x%X, flags %#x", __func__, device->toString().c_str(),
                  samplingRate, format, channelMask, oriFlags);
            break;
        }
    }

    return nullptr;
}

float AudioPolicyManager::adjustDeviceAttenuationForAbsVolume(IVolumeCurves &curves,
                                                              VolumeSource volumeSource,
                                                              int index,
                                                              const DeviceTypeSet &deviceTypes)
{
    audio_devices_t volumeDevice = Volume::getDeviceForVolume(deviceTypes);
    device_category deviceCategory = Volume::getDeviceCategory({volumeDevice});
    float volumeDb = curves.volIndexToDb(deviceCategory, index);

    const auto it = mAbsoluteVolumeDrivingStreams.find(volumeDevice);
    if (it != mAbsoluteVolumeDrivingStreams.end()) {
        audio_attributes_t attributesToDriveAbs = it->second;
        auto groupToDriveAbs = mEngine->getVolumeGroupForAttributes(attributesToDriveAbs);
        if (groupToDriveAbs == VOLUME_GROUP_NONE) {
            ALOGD("%s: no group matching with %s", __FUNCTION__,
                  toString(attributesToDriveAbs).c_str());
            return volumeDb;
        }

        float volumeDbMax = curves.volIndexToDb(deviceCategory, curves.getVolumeIndexMax());
        VolumeSource vsToDriveAbs = toVolumeSource(groupToDriveAbs);
        if (vsToDriveAbs == volumeSource) {
            // attenuation is applied by the abs volume controller
            // do not mute LE broadcast to allow the secondary device to continue playing
            return (index != 0 || volumeDevice == AUDIO_DEVICE_OUT_BLE_BROADCAST) ? volumeDbMax
                                                                                  : volumeDb;
        } else {
            IVolumeCurves &curvesAbs = getVolumeCurves(vsToDriveAbs);
            int indexAbs = curvesAbs.getVolumeIndex({volumeDevice});
            float volumeDbAbs = curvesAbs.volIndexToDb(deviceCategory, indexAbs);
            float volumeDbAbsMax = curvesAbs.volIndexToDb(deviceCategory,
                                                          curvesAbs.getVolumeIndexMax());
            float newVolumeDb = fminf(volumeDb + volumeDbAbsMax - volumeDbAbs, volumeDbMax);
            ALOGV("%s: abs vol stream %d with attenuation %f is adjusting stream %d from "
                  "attenuation %f to attenuation %f %f", __func__, vsToDriveAbs, volumeDbAbs,
                  volumeSource, volumeDb, newVolumeDb, volumeDbMax);
            return newVolumeDb;
        }
    }
    return volumeDb;
}

float AudioPolicyManager::computeVolume(IVolumeCurves &curves,
                                        VolumeSource volumeSource,
                                        int index,
                                        const DeviceTypeSet& deviceTypes,
                                        bool adjustAttenuation,
                                        bool computeInternalInteraction)
{
    float volumeDb;
    if (adjustAttenuation) {
        volumeDb = adjustDeviceAttenuationForAbsVolume(curves, volumeSource, index, deviceTypes);
    } else {
        volumeDb = curves.volIndexToDb(Volume::getDeviceCategory(deviceTypes), index);
    }
    ALOGV("%s volume source %d, index %d,  devices %s, compute internal %b ", __func__,
          volumeSource, index, dumpDeviceTypes(deviceTypes).c_str(), computeInternalInteraction);

    if (!computeInternalInteraction) {
        return volumeDb;
    }

    // handle the case of accessibility active while a ringtone is playing: if the ringtone is much
    // louder than the accessibility prompt, the prompt cannot be heard, thus masking the touch
    // exploration of the dialer UI. In this situation, bring the accessibility volume closer to
    // the ringtone volume
    const auto callVolumeSrc = toVolumeSource(AUDIO_STREAM_VOICE_CALL, false);
    const auto ringVolumeSrc = toVolumeSource(AUDIO_STREAM_RING, false);
    const auto musicVolumeSrc = toVolumeSource(AUDIO_STREAM_MUSIC, false);
    const auto alarmVolumeSrc = toVolumeSource(AUDIO_STREAM_ALARM, false);
    const auto a11yVolumeSrc = toVolumeSource(AUDIO_STREAM_ACCESSIBILITY, false);
    if (AUDIO_MODE_RINGTONE == mEngine->getPhoneState() &&
            mOutputs.isActive(ringVolumeSrc, 0)) {
        auto &ringCurves = getVolumeCurves(AUDIO_STREAM_RING);
        const float ringVolumeDb = computeVolume(ringCurves, ringVolumeSrc, index, deviceTypes,
                                                 adjustAttenuation,
                                                 /* computeInternalInteraction= */false);
        return ringVolumeDb - 4 > volumeDb ? ringVolumeDb - 4 : volumeDb;
    }

    // in-call: always cap volume by voice volume + some low headroom
    if ((volumeSource != callVolumeSrc && (isInCall() ||
                                           mOutputs.isActiveLocally(callVolumeSrc))) &&
            (volumeSource == toVolumeSource(AUDIO_STREAM_SYSTEM, false) ||
             volumeSource == ringVolumeSrc || volumeSource == musicVolumeSrc ||
             volumeSource == alarmVolumeSrc ||
             volumeSource == toVolumeSource(AUDIO_STREAM_NOTIFICATION, false) ||
             volumeSource == toVolumeSource(AUDIO_STREAM_ENFORCED_AUDIBLE, false) ||
             volumeSource == toVolumeSource(AUDIO_STREAM_DTMF, false) ||
             volumeSource == a11yVolumeSrc)) {
        auto &voiceCurves = getVolumeCurves(callVolumeSrc);
        int voiceVolumeIndex = voiceCurves.getVolumeIndex(deviceTypes);
        const float maxVoiceVolDb =
                computeVolume(voiceCurves, callVolumeSrc, voiceVolumeIndex, deviceTypes,
                        adjustAttenuation, /* computeInternalInteraction= */false)
                + IN_CALL_EARPIECE_HEADROOM_DB;
        // FIXME: Workaround for call screening applications until a proper audio mode is defined
        // to support this scenario : Exempt the RING stream from the audio cap if the audio was
        // programmatically muted.
        // VOICE_CALL stream has minVolumeIndex > 0 : Users cannot set the volume of voice calls to
        // 0. We don't want to cap volume when the system has programmatically muted the voice call
        // stream. See setVolumeCurveIndex() for more information.
        bool exemptFromCapping =
                ((volumeSource == ringVolumeSrc) || (volumeSource == a11yVolumeSrc))
                && (voiceVolumeIndex == 0);
        ALOGV_IF(exemptFromCapping, "%s volume source %d at vol=%f not capped", __func__,
                 volumeSource, volumeDb);
        if ((volumeDb > maxVoiceVolDb) && !exemptFromCapping) {
            ALOGV("%s volume source %d at vol=%f overriden by volume group %d at vol=%f", __func__,
                  volumeSource, volumeDb, callVolumeSrc, maxVoiceVolDb);
            volumeDb = maxVoiceVolDb;
        }
    }
    // if a headset is connected, apply the following rules to ring tones and notifications
    // to avoid sound level bursts in user's ears:
    // - always attenuate notifications volume by 6dB
    // - attenuate ring tones volume by 6dB unless music is not playing and
    // speaker is part of the select devices
    // - if music is playing, always limit the volume to current music volume,
    // with a minimum threshold at -36dB so that notification is always perceived.
    if (!Intersection(deviceTypes,
            {AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES,
             AUDIO_DEVICE_OUT_WIRED_HEADSET, AUDIO_DEVICE_OUT_WIRED_HEADPHONE,
             AUDIO_DEVICE_OUT_USB_HEADSET, AUDIO_DEVICE_OUT_HEARING_AID,
             AUDIO_DEVICE_OUT_BLE_HEADSET}).empty() &&
            ((volumeSource == alarmVolumeSrc ||
              volumeSource == ringVolumeSrc) ||
             (volumeSource == toVolumeSource(AUDIO_STREAM_NOTIFICATION, false)) ||
             (volumeSource == toVolumeSource(AUDIO_STREAM_SYSTEM, false)) ||
             ((volumeSource == toVolumeSource(AUDIO_STREAM_ENFORCED_AUDIBLE, false)) &&
              (mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM) == AUDIO_POLICY_FORCE_NONE))) &&
            curves.canBeMuted()) {

        // when the phone is ringing we must consider that music could have been paused just before
        // by the music application and behave as if music was active if the last music track was
        // just stopped
        if (isStreamActive(AUDIO_STREAM_MUSIC, SONIFICATION_HEADSET_MUSIC_DELAY)
                || mLimitRingtoneVolume) {
            volumeDb += SONIFICATION_HEADSET_VOLUME_FACTOR_DB;
            DeviceTypeSet musicDevice =
                    mEngine->getOutputDevicesForAttributes(attributes_initializer(AUDIO_USAGE_MEDIA),
                                                           nullptr, true /*fromCache*/).types();
            auto &musicCurves = getVolumeCurves(AUDIO_STREAM_MUSIC);
            float musicVolDb = computeVolume(musicCurves,
                                             musicVolumeSrc,
                                             musicCurves.getVolumeIndex(musicDevice),
                                             musicDevice,
                                              adjustAttenuation,
                                              /* computeInternalInteraction= */ false);
            float minVolDb = (musicVolDb > SONIFICATION_HEADSET_VOLUME_MIN_DB) ?
                        musicVolDb : SONIFICATION_HEADSET_VOLUME_MIN_DB;
            if (volumeDb > minVolDb) {
                volumeDb = minVolDb;
                ALOGV("computeVolume limiting volume to %f musicVol %f", minVolDb, musicVolDb);
            }
            if (Volume::getDeviceForVolume(deviceTypes) != AUDIO_DEVICE_OUT_SPEAKER
                    &&  !Intersection(deviceTypes, {AUDIO_DEVICE_OUT_BLUETOOTH_A2DP,
                        AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES,
                        AUDIO_DEVICE_OUT_BLE_HEADSET}).empty()) {
                // on A2DP/BLE, also ensure notification volume is not too low compared to media
                // when intended to be played.
                if ((volumeDb > -96.0f) &&
                        (musicVolDb - SONIFICATION_A2DP_MAX_MEDIA_DIFF_DB > volumeDb)) {
                    ALOGV("%s increasing volume for volume source=%d device=%s from %f to %f",
                          __func__, volumeSource, dumpDeviceTypes(deviceTypes).c_str(), volumeDb,
                          musicVolDb - SONIFICATION_A2DP_MAX_MEDIA_DIFF_DB);
                    volumeDb = musicVolDb - SONIFICATION_A2DP_MAX_MEDIA_DIFF_DB;
                }
            }
        } else if ((Volume::getDeviceForVolume(deviceTypes) != AUDIO_DEVICE_OUT_SPEAKER) ||
                   (!(volumeSource == alarmVolumeSrc || volumeSource == ringVolumeSrc))) {
            volumeDb += SONIFICATION_HEADSET_VOLUME_FACTOR_DB;
        }
    }

    return volumeDb;
}

int AudioPolicyManager::rescaleVolumeIndex(int srcIndex,
                                           VolumeSource fromVolumeSource,
                                           VolumeSource toVolumeSource)
{
    if (fromVolumeSource == toVolumeSource) {
        return srcIndex;
    }
    auto &srcCurves = getVolumeCurves(fromVolumeSource);
    auto &dstCurves = getVolumeCurves(toVolumeSource);
    float minSrc = (float)srcCurves.getVolumeIndexMin();
    float maxSrc = (float)srcCurves.getVolumeIndexMax();
    float minDst = (float)dstCurves.getVolumeIndexMin();
    float maxDst = (float)dstCurves.getVolumeIndexMax();

    // preserve mute request or correct range
    if (srcIndex < minSrc) {
        if (srcIndex == 0) {
            return 0;
        }
        srcIndex = minSrc;
    } else if (srcIndex > maxSrc) {
        srcIndex = maxSrc;
    }
    return (int)(minDst + ((srcIndex - minSrc) * (maxDst - minDst)) / (maxSrc - minSrc));
}

status_t AudioPolicyManager::checkAndSetVolume(IVolumeCurves &curves,
                                               VolumeSource volumeSource,
                                               int index,
                                               const sp<AudioOutputDescriptor>& outputDesc,
                                               DeviceTypeSet deviceTypes,
                                               int delayMs,
                                               bool force)
{
    // APM is single threaded, and single instance.
    static std::set<IVolumeCurves*> invalidCurvesReported;

    // do not change actual attributes volume if the attributes is muted
    if (!com_android_media_audio_ring_my_car() && outputDesc->isMutedInternally(volumeSource)) {
        ALOGVV("%s: volume source %d muted count %d active=%d", __func__, volumeSource,
               outputDesc->getMuteCount(volumeSource), outputDesc->isActive(volumeSource));
        return NO_ERROR;
    }

    bool isVoiceVolSrc;
    bool isBtScoVolSrc;
    if (!isVolumeConsistentForCalls(
            volumeSource, deviceTypes, isVoiceVolSrc, isBtScoVolSrc, __func__)) {
        // Do not return an error here as AudioService will always set both voice call
        // and Bluetooth SCO volumes due to stream aliasing.
        return NO_ERROR;
    }

    if (deviceTypes.empty()) {
        deviceTypes = outputDesc->devices().types();
        index = curves.getVolumeIndex(deviceTypes);
        ALOGV("%s if deviceTypes is change from none to device %s, need get index %d",
                __func__, dumpDeviceTypes(deviceTypes).c_str(), index);
    }

    if (curves.getVolumeIndexMin() < 0 || curves.getVolumeIndexMax() < 0) {
        if (!invalidCurvesReported.count(&curves)) {
            invalidCurvesReported.insert(&curves);
            String8 dump;
            curves.dump(&dump);
            ALOGE("invalid volume index range in the curve:\n%s", dump.c_str());
        }
        return BAD_VALUE;
    }

    float volumeDb = computeVolume(curves, volumeSource, index, deviceTypes);
    const VolumeSource dtmfVolSrc = toVolumeSource(AUDIO_STREAM_DTMF, false);
    if (outputDesc->isFixedVolume(deviceTypes) ||
            // Force VoIP volume to max for bluetooth SCO/BLE device except if muted
            (index != 0 && (isVoiceVolSrc || isBtScoVolSrc
                        || (isInCall() && (dtmfVolSrc == volumeSource))) &&
                    (isSingleDeviceType(deviceTypes, audio_is_bluetooth_out_sco_device)
                    || isSingleDeviceType(deviceTypes, audio_is_ble_out_device)))) {
        volumeDb = 0.0f;
    }

    bool muted;
    if (!com_android_media_audio_ring_my_car()) {
        muted = (index == 0) && (volumeDb != 0.0f);
    } else {
        muted = curves.isMuted();
    }
    outputDesc->setVolume(volumeDb, muted, volumeSource, curves.getStreamTypes(),
            deviceTypes, delayMs, force, isVoiceVolSrc);

    if (outputDesc == mPrimaryOutput && (isVoiceVolSrc || isBtScoVolSrc)) {
        bool voiceVolumeManagedByHost = !isBtScoVolSrc &&
                !isSingleDeviceType(deviceTypes, audio_is_ble_out_device);
        setVoiceVolume(index, curves, voiceVolumeManagedByHost, delayMs);
    }
    return NO_ERROR;
}

void AudioPolicyManager::setVoiceVolume(
        int index, IVolumeCurves &curves, bool voiceVolumeManagedByHost, int delayMs) {
    float voiceVolume;

    if (com_android_media_audio_ring_my_car() && curves.isMuted()) {
        index = 0;
    }

    // Force voice volume to max or mute for Bluetooth SCO/BLE as other attenuations are managed
    // by the headset
    if (voiceVolumeManagedByHost) {
        voiceVolume = (float)index/(float)curves.getVolumeIndexMax();
    } else {
        voiceVolume = index == 0 ? 0.0 : 1.0;
    }
    if (voiceVolume != mLastVoiceVolume) {
        mpClientInterface->setVoiceVolume(voiceVolume, delayMs);
        mLastVoiceVolume = voiceVolume;
    }
}

bool AudioPolicyManager::isVolumeConsistentForCalls(VolumeSource volumeSource,
                                                   const DeviceTypeSet& deviceTypes,
                                                   bool& isVoiceVolSrc,
                                                   bool& isBtScoVolSrc,
                                                   const char* caller) {
    const VolumeSource callVolSrc = toVolumeSource(AUDIO_STREAM_VOICE_CALL, false);
    isVoiceVolSrc = (volumeSource != VOLUME_SOURCE_NONE) && (callVolSrc == volumeSource);

    const bool isScoRequested = isScoRequestedForComm();
    const bool isHAUsed = isHearingAidUsedForComm();

    if (com_android_media_audio_replace_stream_bt_sco()) {
        isBtScoVolSrc = (volumeSource != VOLUME_SOURCE_NONE) && (callVolSrc == volumeSource) &&
                        (isScoRequested || isHAUsed);
        return true;
    }

    const VolumeSource btScoVolSrc = toVolumeSource(AUDIO_STREAM_BLUETOOTH_SCO, false);
    isBtScoVolSrc = (volumeSource != VOLUME_SOURCE_NONE) && (btScoVolSrc == volumeSource);

    if ((callVolSrc != btScoVolSrc) &&
            ((isVoiceVolSrc && isScoRequested) ||
             (isBtScoVolSrc && !(isScoRequested || isHAUsed))) &&
            !isSingleDeviceType(deviceTypes, AUDIO_DEVICE_OUT_TELEPHONY_TX)) {
        ALOGV("%s cannot set volume group %d volume when is%srequested for comm", caller,
             volumeSource, isScoRequested ? " " : " not ");
        return false;
    }
    return true;
}

void AudioPolicyManager::applyStreamVolumes(const sp<AudioOutputDescriptor>& outputDesc,
                                            const DeviceTypeSet& deviceTypes,
                                            int delayMs,
                                            bool force)
{
    ALOGVV("applyStreamVolumes() for device %s", dumpDeviceTypes(deviceTypes).c_str());
    for (const auto &volumeGroup : mEngine->getVolumeGroups()) {
        auto &curves = getVolumeCurves(toVolumeSource(volumeGroup));
        checkAndSetVolume(curves, toVolumeSource(volumeGroup), curves.getVolumeIndex(deviceTypes),
                          outputDesc, deviceTypes, delayMs, force);
    }
}

void AudioPolicyManager::setStrategyMute(product_strategy_t strategy,
                                         bool on,
                                         const sp<AudioOutputDescriptor>& outputDesc,
                                         int delayMs,
                                         DeviceTypeSet deviceTypes)
{
    std::vector<VolumeSource> sourcesToMute;
    for (auto attributes: mEngine->getAllAttributesForProductStrategy(strategy)) {
        ALOGVV("%s() attributes %s, mute %d, output ID %d", __func__,
               toString(attributes).c_str(), on, outputDesc->getId());
        VolumeSource source = toVolumeSource(attributes, false);
        if ((source != VOLUME_SOURCE_NONE) &&
                (std::find(begin(sourcesToMute), end(sourcesToMute), source)
                        == end(sourcesToMute))) {
            sourcesToMute.push_back(source);
        }
    }
    for (auto source : sourcesToMute) {
        setVolumeSourceMutedInternally(source, on, outputDesc, delayMs, deviceTypes);
    }

}

void AudioPolicyManager::setVolumeSourceMutedInternally(VolumeSource volumeSource,
                                                        bool on,
                                                        const sp<AudioOutputDescriptor>& outputDesc,
                                                        int delayMs,
                                                        DeviceTypeSet deviceTypes)
{
    if (deviceTypes.empty()) {
        deviceTypes = outputDesc->devices().types();
    }
    auto &curves = getVolumeCurves(volumeSource);
    if (on) {
        if (!outputDesc->isMutedInternally(volumeSource)) {
            if (curves.canBeMuted() &&
                    (volumeSource != toVolumeSource(AUDIO_STREAM_ENFORCED_AUDIBLE, false) ||
                     (mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM) ==
                      AUDIO_POLICY_FORCE_NONE))) {
                checkAndSetVolume(curves, volumeSource, 0, outputDesc, deviceTypes, delayMs);
            }
        }
        // increment mMuteCount after calling checkAndSetVolume() so that volume change is not
        // ignored
        outputDesc->incMuteCount(volumeSource);
    } else {
        if (!outputDesc->isMutedInternally(volumeSource)) {
            ALOGV("%s unmuting non muted attributes!", __func__);
            return;
        }
        if (outputDesc->decMuteCount(volumeSource) == 0) {
            checkAndSetVolume(curves, volumeSource,
                              curves.getVolumeIndex(deviceTypes),
                              outputDesc,
                              deviceTypes,
                              delayMs);
        }
    }
}

bool AudioPolicyManager::isValidAttributes(const audio_attributes_t *paa)
{
    if ((paa->flags & AUDIO_FLAG_SCO) != 0) {
        ALOGW("%s: deprecated use of AUDIO_FLAG_SCO in attributes flags %d", __func__, paa->flags);
    }

    // has flags that map to a stream type?
    if ((paa->flags & (AUDIO_FLAG_AUDIBILITY_ENFORCED | AUDIO_FLAG_BEACON)) != 0) {
        return true;
    }

    // has known usage?
    switch (paa->usage) {
    case AUDIO_USAGE_UNKNOWN:
    case AUDIO_USAGE_MEDIA:
    case AUDIO_USAGE_VOICE_COMMUNICATION:
    case AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING:
    case AUDIO_USAGE_ALARM:
    case AUDIO_USAGE_NOTIFICATION:
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED:
    case AUDIO_USAGE_NOTIFICATION_EVENT:
    case AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY:
    case AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION:
    case AUDIO_USAGE_GAME:
    case AUDIO_USAGE_VIRTUAL_SOURCE:
    case AUDIO_USAGE_ASSISTANT:
    case AUDIO_USAGE_CALL_ASSISTANT:
    case AUDIO_USAGE_EMERGENCY:
    case AUDIO_USAGE_SAFETY:
    case AUDIO_USAGE_VEHICLE_STATUS:
    case AUDIO_USAGE_ANNOUNCEMENT:
    case AUDIO_USAGE_SPEAKER_CLEANUP:
        break;
    default:
        return false;
    }
    return true;
}

audio_policy_forced_cfg_t AudioPolicyManager::getForceUse(audio_policy_force_use_t usage)
{
    return mEngine->getForceUse(usage);
}

bool AudioPolicyManager::isInCall() const {
    return isStateInCall(mEngine->getPhoneState());
}

bool AudioPolicyManager::isStateInCall(int state) const {
    return is_state_in_call(state);
}

bool AudioPolicyManager::isCallAudioAccessible() const {
    audio_mode_t mode = mEngine->getPhoneState();
    return (mode == AUDIO_MODE_IN_CALL)
            || (mode == AUDIO_MODE_CALL_SCREEN)
            || (mode == AUDIO_MODE_CALL_REDIRECT);
}

bool AudioPolicyManager::isInCallOrScreening() const {
    audio_mode_t mode = mEngine->getPhoneState();
    return isStateInCall(mode) || mode == AUDIO_MODE_CALL_SCREEN;
}

void AudioPolicyManager::cleanUpForDevice(const sp<DeviceDescriptor>& deviceDesc)
{
    for (ssize_t i = (ssize_t)mAudioSources.size() - 1; i >= 0; i--)  {
        sp<SourceClientDescriptor> sourceDesc = mAudioSources.valueAt(i);
        if (sourceDesc->isConnected() && (sourceDesc->srcDevice()->equals(deviceDesc) ||
                                          sourceDesc->sinkDevice()->equals(deviceDesc))
                && !sourceDesc->isCallRx()) {
            disconnectAudioSource(sourceDesc);
        }
    }

    for (ssize_t i = (ssize_t)mAudioPatches.size() - 1; i >= 0; i--)  {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(i);
        bool release = false;
        for (size_t j = 0; j < patchDesc->mPatch.num_sources && !release; j++)  {
            const struct audio_port_config *source = &patchDesc->mPatch.sources[j];
            if (source->type == AUDIO_PORT_TYPE_DEVICE &&
                    source->ext.device.type == deviceDesc->type()) {
                release = true;
            }
        }
        const char *address = deviceDesc->address().c_str();
        for (size_t j = 0; j < patchDesc->mPatch.num_sinks && !release; j++)  {
            const struct audio_port_config *sink = &patchDesc->mPatch.sinks[j];
            if (sink->type == AUDIO_PORT_TYPE_DEVICE &&
                    sink->ext.device.type == deviceDesc->type() &&
                    (strnlen(address, AUDIO_DEVICE_MAX_ADDRESS_LEN) == 0
                     || strncmp(sink->ext.device.address, address,
                                 AUDIO_DEVICE_MAX_ADDRESS_LEN) == 0)) {
                release = true;
            }
        }
        if (release) {
            ALOGV("%s releasing patch %u", __FUNCTION__, patchDesc->getHandle());
            releaseAudioPatch(patchDesc->getHandle(), patchDesc->getUid());
        }
    }

    mInputs.clearSessionRoutesForDevice(deviceDesc);

    mHwModules.cleanUpForDevice(deviceDesc);
}

void AudioPolicyManager::modifySurroundFormats(
        const sp<DeviceDescriptor>& devDesc, FormatVector *formatsPtr) {
    std::unordered_set<audio_format_t> enforcedSurround(
            devDesc->encodedFormats().begin(), devDesc->encodedFormats().end());
    std::unordered_set<audio_format_t> allSurround;  // A flat set of all known surround formats
    for (const auto& pair : mConfig->getSurroundFormats()) {
        allSurround.insert(pair.first);
        for (const auto& subformat : pair.second) allSurround.insert(subformat);
    }

    audio_policy_forced_cfg_t forceUse = mEngine->getForceUse(
            AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND);
    ALOGD("%s: forced use = %d", __FUNCTION__, forceUse);
    // This is the resulting set of formats depending on the surround mode:
    //   'all surround' = allSurround
    //   'enforced surround' = enforcedSurround [may include IEC69137 which isn't raw surround fmt]
    //   'non-surround' = not in 'all surround' and not in 'enforced surround'
    //   'manual surround' = mManualSurroundFormats
    // AUTO:   formats v 'enforced surround'
    // ALWAYS: formats v 'all surround' v 'enforced surround'
    // NEVER:  formats ^ 'non-surround'
    // MANUAL: formats ^ ('non-surround' v 'manual surround' v (IEC69137 ^ 'enforced surround'))

    std::unordered_set<audio_format_t> formatSet;
    if (forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_MANUAL
            || forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_NEVER) {
        // formatSet is (formats ^ 'non-surround')
        for (auto formatIter = formatsPtr->begin(); formatIter != formatsPtr->end(); ++formatIter) {
            if (allSurround.count(*formatIter) == 0 && enforcedSurround.count(*formatIter) == 0) {
                formatSet.insert(*formatIter);
            }
        }
    } else {
        formatSet.insert(formatsPtr->begin(), formatsPtr->end());
    }
    formatsPtr->clear();  // Re-filled from the formatSet at the end.

    if (forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_MANUAL) {
        formatSet.insert(mManualSurroundFormats.begin(), mManualSurroundFormats.end());
        // Enable IEC61937 when in MANUAL mode if it's enforced for this device.
        if (enforcedSurround.count(AUDIO_FORMAT_IEC61937) != 0) {
            formatSet.insert(AUDIO_FORMAT_IEC61937);
        }
    } else if (forceUse != AUDIO_POLICY_FORCE_ENCODED_SURROUND_NEVER) { // AUTO or ALWAYS
        if (forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_ALWAYS) {
            formatSet.insert(allSurround.begin(), allSurround.end());
        }
        formatSet.insert(enforcedSurround.begin(), enforcedSurround.end());
    }
    for (const auto& format : formatSet) {
        formatsPtr->push_back(format);
    }
}

void AudioPolicyManager::modifySurroundChannelMasks(ChannelMaskSet *channelMasksPtr) {
    ChannelMaskSet &channelMasks = *channelMasksPtr;
    audio_policy_forced_cfg_t forceUse = mEngine->getForceUse(
            AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND);

    // If NEVER, then remove support for channelMasks > stereo.
    if (forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_NEVER) {
        for (auto it = channelMasks.begin(); it != channelMasks.end();) {
            audio_channel_mask_t channelMask = *it;
            if (channelMask & ~AUDIO_CHANNEL_OUT_STEREO) {
                ALOGV("%s: force NEVER, so remove channelMask 0x%08x", __FUNCTION__, channelMask);
                it = channelMasks.erase(it);
            } else {
                ++it;
            }
        }
    // If ALWAYS or MANUAL, then make sure we at least support 5.1
    } else if (forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_ALWAYS
            || forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_MANUAL) {
        bool supports5dot1 = false;
        // Are there any channel masks that can be considered "surround"?
        for (audio_channel_mask_t channelMask : channelMasks) {
            if ((channelMask & AUDIO_CHANNEL_OUT_5POINT1) == AUDIO_CHANNEL_OUT_5POINT1) {
                supports5dot1 = true;
                break;
            }
        }
        // If not then add 5.1 support.
        if (!supports5dot1) {
            channelMasks.insert(AUDIO_CHANNEL_OUT_5POINT1);
            ALOGV("%s: force MANUAL or ALWAYS, so adding channelMask for 5.1 surround", __func__);
        }
    }
}

void AudioPolicyManager::updateAudioProfiles(const sp<DeviceDescriptor>& devDesc,
                                             audio_io_handle_t ioHandle,
                                             const sp<IOProfile>& profile) {
    if (!profile->hasDynamicAudioProfile()) {
        return;
    }

    audio_port_v7 devicePort;
    devDesc->toAudioPort(&devicePort);

    audio_port_v7 mixPort;
    profile->toAudioPort(&mixPort);
    mixPort.ext.mix.handle = ioHandle;

    status_t status = mpClientInterface->getAudioMixPort(&devicePort, &mixPort);
    if (status != NO_ERROR) {
        ALOGE("%s failed to query the attributes of the mix port", __func__);
        return;
    }

    std::set<audio_format_t> supportedFormats;
    for (size_t i = 0; i < mixPort.num_audio_profiles; ++i) {
        supportedFormats.insert(mixPort.audio_profiles[i].format);
    }
    FormatVector formats(supportedFormats.begin(), supportedFormats.end());
    mReportedFormatsMap[devDesc] = formats;

    if (devDesc->type() == AUDIO_DEVICE_OUT_HDMI ||
        devDesc->type() == AUDIO_DEVICE_OUT_HDMI_ARC ||
        devDesc->type() == AUDIO_DEVICE_OUT_HDMI_EARC ||
        isDeviceOfModule(devDesc,AUDIO_HARDWARE_MODULE_ID_MSD)) {
        modifySurroundFormats(devDesc, &formats);
        size_t modifiedNumProfiles = 0;
        for (size_t i = 0; i < mixPort.num_audio_profiles; ++i) {
            if (std::find(formats.begin(), formats.end(), mixPort.audio_profiles[i].format) ==
                formats.end()) {
                // Skip the format that is not present after modifying surround formats.
                continue;
            }
            memcpy(&mixPort.audio_profiles[modifiedNumProfiles], &mixPort.audio_profiles[i],
                   sizeof(struct audio_profile));
            ChannelMaskSet channels(mixPort.audio_profiles[modifiedNumProfiles].channel_masks,
                    mixPort.audio_profiles[modifiedNumProfiles].channel_masks +
                            mixPort.audio_profiles[modifiedNumProfiles].num_channel_masks);
            modifySurroundChannelMasks(&channels);
            std::copy(channels.begin(), channels.end(),
                      std::begin(mixPort.audio_profiles[modifiedNumProfiles].channel_masks));
            mixPort.audio_profiles[modifiedNumProfiles++].num_channel_masks = channels.size();
        }
        mixPort.num_audio_profiles = modifiedNumProfiles;
    }
    profile->importAudioPort(mixPort);
}

status_t AudioPolicyManager::installPatch(const char *caller,
                                          audio_patch_handle_t *patchHandle,
                                          AudioIODescriptorInterface *ioDescriptor,
                                          const struct audio_patch *patch,
                                          int delayMs)
{
    ssize_t index = mAudioPatches.indexOfKey(
            patchHandle && *patchHandle != AUDIO_PATCH_HANDLE_NONE ?
            *patchHandle : ioDescriptor->getPatchHandle());
    sp<AudioPatch> patchDesc;
    status_t status = installPatch(
            caller, index, patchHandle, patch, delayMs, mUidCached, &patchDesc);
    if (status == NO_ERROR) {
        ioDescriptor->setPatchHandle(patchDesc->getHandle());
    }
    return status;
}

status_t AudioPolicyManager::installPatch(const char *caller,
                                          ssize_t index,
                                          audio_patch_handle_t *patchHandle,
                                          const struct audio_patch *patch,
                                          int delayMs,
                                          uid_t uid,
                                          sp<AudioPatch> *patchDescPtr)
{
    sp<AudioPatch> patchDesc;
    audio_patch_handle_t afPatchHandle = AUDIO_PATCH_HANDLE_NONE;
    if (index >= 0) {
        patchDesc = mAudioPatches.valueAt(index);
        afPatchHandle = patchDesc->getAfHandle();
    }

    status_t status = mpClientInterface->createAudioPatch(patch, &afPatchHandle, delayMs);
    ALOGV("%s() AF::createAudioPatch returned %d patchHandle %d num_sources %d num_sinks %d",
            caller, status, afPatchHandle, patch->num_sources, patch->num_sinks);
    if (status == NO_ERROR) {
        if (index < 0) {
            patchDesc = new AudioPatch(patch, uid);
            addAudioPatch(patchDesc->getHandle(), patchDesc);
        } else {
            patchDesc->mPatch = *patch;
        }
        patchDesc->setAfHandle(afPatchHandle);
        if (patchHandle) {
            *patchHandle = patchDesc->getHandle();
        }
        nextAudioPortGeneration();
        mpClientInterface->onAudioPatchListUpdate();
    }
    if (patchDescPtr) *patchDescPtr = patchDesc;
    return status;
}

bool AudioPolicyManager::areAllActiveTracksRerouted(const sp<SwAudioOutputDescriptor>& output)
{
    const TrackClientVector activeClients = output->getActiveClients();
    if (activeClients.empty()) {
        return true;
    }
    ssize_t index = mAudioPatches.indexOfKey(output->getPatchHandle());
    if (index < 0) {
        ALOGE("%s, no audio patch found while there are active clients on output %d",
                __func__, output->getId());
        return false;
    }
    sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
    DeviceVector routedDevices;
    for (int i = 0; i < patchDesc->mPatch.num_sinks; ++i) {
        sp<DeviceDescriptor> device = mAvailableOutputDevices.getDeviceFromId(
                patchDesc->mPatch.sinks[i].id);
        if (device == nullptr) {
            ALOGE("%s, no audio device found with id(%d)",
                    __func__, patchDesc->mPatch.sinks[i].id);
            return false;
        }
        routedDevices.add(device);
    }
    for (const auto& client : activeClients) {
        if (client->isInvalid()) {
            // No need to take care about invalidated clients.
            continue;
        }
        sp<DeviceDescriptor> preferredDevice =
                mAvailableOutputDevices.getDeviceFromId(client->preferredDeviceId());
        if (mEngine->getOutputDevicesForAttributes(
                client->attributes(), preferredDevice, false) == routedDevices) {
            return false;
        }
    }
    return true;
}

sp<SwAudioOutputDescriptor> AudioPolicyManager::openOutputWithProfileAndDevice(
        const sp<IOProfile>& profile, const DeviceVector& devices,
        const audio_config_base_t *mixerConfig, const audio_config_t *halConfig,
        audio_output_flags_t flags)
{
    for (const auto& device : devices) {
        // TODO: This should be checking if the profile supports the device combo.
        if (!profile->supportsDevice(device)) {
            ALOGE("%s profile(%s) doesn't support device %#x", __func__, profile->getName().c_str(),
                  device->type());
            return nullptr;
        }
    }
    sp<SwAudioOutputDescriptor> desc = new SwAudioOutputDescriptor(profile, mpClientInterface);
    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
    audio_attributes_t attributes = AUDIO_ATTRIBUTES_INITIALIZER;
    status_t status = desc->open(halConfig, mixerConfig, devices,
            AUDIO_STREAM_DEFAULT, &flags, &output, attributes);
    if (status != NO_ERROR) {
        ALOGE("%s failed to open output %d", __func__, status);
        return nullptr;
    }
    if ((flags & AUDIO_OUTPUT_FLAG_BIT_PERFECT) == AUDIO_OUTPUT_FLAG_BIT_PERFECT) {
        auto portConfig = desc->getConfig();
        for (const auto& device : devices) {
            device->setPreferredConfig(&portConfig);
        }
    }

    // Here is where the out_set_parameters() for card & device gets called
    sp<DeviceDescriptor> device = devices.getDeviceForOpening();
    const audio_devices_t deviceType = device->type();
    const String8 &address = String8(device->address().c_str());
    if (!address.empty()) {
        char *param = audio_device_address_to_parameter(deviceType, address.c_str());
        mpClientInterface->setParameters(output, String8(param));
        free(param);
    }
    updateAudioProfiles(device, output, profile);
    if (!profile->hasValidAudioProfile()) {
        ALOGW("%s() missing param", __func__);
        desc->close();
        return nullptr;
    } else if (profile->hasDynamicAudioProfile() && halConfig == nullptr) {
        // Reopen the output with the best audio profile picked by APM when the profile supports
        // dynamic audio profile and the hal config is not specified.
        desc->close();
        output = AUDIO_IO_HANDLE_NONE;
        audio_config_t config = AUDIO_CONFIG_INITIALIZER;
        profile->pickAudioProfile(
                config.sample_rate, config.channel_mask, config.format);
        config.offload_info.sample_rate = config.sample_rate;
        config.offload_info.channel_mask = config.channel_mask;
        config.offload_info.format = config.format;

        status = desc->open(&config, mixerConfig, devices, AUDIO_STREAM_DEFAULT, &flags, &output,
                            attributes);
        if (status != NO_ERROR) {
            return nullptr;
        }
    }

    addOutput(output, desc);
    // The version check is essentially to avoid making this call in the case of the HIDL HAL.
    if (auto hwModule = mHwModules.getModuleFromHandle(mPrimaryModuleHandle); hwModule &&
            hwModule->getHalVersionMajor() >= 3) {
        setOutputDevices(__func__, desc, devices, true, 0, NULL);
    }
    sp<DeviceDescriptor> speaker = mAvailableOutputDevices.getDevice(
            AUDIO_DEVICE_OUT_SPEAKER, String8(""), AUDIO_FORMAT_DEFAULT);

    if (audio_is_remote_submix_device(deviceType) && address != "0") {
        sp<AudioPolicyMix> policyMix;
        if (mPolicyMixes.getAudioPolicyMix(deviceType, address, policyMix) == NO_ERROR) {
            policyMix->setOutput(desc);
            desc->mPolicyMix = policyMix;
        } else {
            ALOGW("checkOutputsForDevice() cannot find policy for address %s",
                    address.c_str());
        }

    } else if (hasPrimaryOutput() && speaker != nullptr
            && mPrimaryOutput->supportsDevice(speaker) && !desc->supportsDevice(speaker)
            && ((desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) == 0)) {
        // no duplicated output for:
        // - direct outputs
        // - outputs used by dynamic policy mixes
        // - outputs that supports SPEAKER while the primary output does not.
        audio_io_handle_t duplicatedOutput = AUDIO_IO_HANDLE_NONE;

        //TODO: configure audio effect output stage here

        // open a duplicating output thread for the new output and the primary output
        sp<SwAudioOutputDescriptor> dupOutputDesc =
                new SwAudioOutputDescriptor(nullptr, mpClientInterface);
        status = dupOutputDesc->openDuplicating(mPrimaryOutput, desc, &duplicatedOutput);
        if (status == NO_ERROR) {
            // add duplicated output descriptor
            addOutput(duplicatedOutput, dupOutputDesc);
        } else {
            ALOGW("checkOutputsForDevice() could not open dup output for %d and %d",
                  mPrimaryOutput->mIoHandle, output);
            desc->close();
            removeOutput(output);
            nextAudioPortGeneration();
            return nullptr;
        }
    }
    if (mPrimaryOutput == nullptr && profile->getFlags() & AUDIO_OUTPUT_FLAG_PRIMARY) {
        ALOGV("%s(): re-assigning mPrimaryOutput", __func__);
        mPrimaryOutput = desc;
        mPrimaryModuleHandle = mPrimaryOutput->getModuleHandle();
    }
    return desc;
}

status_t AudioPolicyManager::getDevicesForAttributes(
        const audio_attributes_t &attr, DeviceVector &devices, bool forVolume) {
    // attr containing source set by AudioAttributes.Builder.setCapturePreset() has precedence
    // over any usage or content type also present in attr.
    if (com::android::media::audioserver::enable_audio_input_device_routing() &&
        attr.source != AUDIO_SOURCE_INVALID) {
        return getInputDevicesForAttributes(attr, devices);
    }

    // Devices are determined in the following precedence:
    //
    // 1) Devices associated with a dynamic policy matching the attributes.  This is often
    //    a remote submix from MIX_ROUTE_FLAG_LOOP_BACK.
    //
    // If no such dynamic policy then
    // 2) Devices containing an active client using setPreferredDevice
    //    with same strategy as the attributes.
    //    (from the default Engine::getOutputDevicesForAttributes() implementation).
    //
    // If no corresponding active client with setPreferredDevice then
    // 3) Devices associated with the strategy determined by the attributes
    //    (from the default Engine::getOutputDevicesForAttributes() implementation).
    //
    // See related getOutputForAttrInt().

    // check dynamic policies but only for primary descriptors (secondary not used for audible
    // audio routing, only used for duplication for playback capture)
    sp<AudioPolicyMix> policyMix;
    bool unneededUsePrimaryOutputFromPolicyMixes = false;
    status_t status = mPolicyMixes.getOutputForAttr(attr, AUDIO_CONFIG_BASE_INITIALIZER,
            0 /*uid unknown here*/, AUDIO_SESSION_NONE, AUDIO_OUTPUT_FLAG_NONE,
            mAvailableOutputDevices, nullptr /* requestedDevice */, policyMix,
            nullptr /* secondaryMixes */, unneededUsePrimaryOutputFromPolicyMixes);
    if (status != OK) {
        return status;
    }

    if (policyMix != nullptr && policyMix->getOutput() != nullptr &&
            // For volume control, skip LOOPBACK mixes which use AUDIO_DEVICE_OUT_REMOTE_SUBMIX
            // as they are unaffected by device/stream volume
            // (per SwAudioOutputDescriptor::isFixedVolume()).
            (!forVolume || policyMix->mDeviceType != AUDIO_DEVICE_OUT_REMOTE_SUBMIX)
            ) {
        sp<DeviceDescriptor> deviceDesc = mAvailableOutputDevices.getDevice(
                policyMix->mDeviceType, policyMix->mDeviceAddress, AUDIO_FORMAT_DEFAULT);
        devices.add(deviceDesc);
    } else {
        // The default Engine::getOutputDevicesForAttributes() uses findPreferredDevice()
        // which selects setPreferredDevice if active.  This means forVolume call
        // will take an active setPreferredDevice, if such exists.

        devices = mEngine->getOutputDevicesForAttributes(
                attr, nullptr /* preferredDevice */, false /* fromCache */);
    }

    if (forVolume) {
        // We alias the device AUDIO_DEVICE_OUT_SPEAKER_SAFE to AUDIO_DEVICE_OUT_SPEAKER
        // for single volume control in AudioService (such relationship should exist if
        // SPEAKER_SAFE is present).
        //
        // (This is unrelated to a different device grouping as Volume::getDeviceCategory)
        DeviceVector speakerSafeDevices =
                devices.getDevicesFromType(AUDIO_DEVICE_OUT_SPEAKER_SAFE);
        if (!speakerSafeDevices.isEmpty()) {
            devices.merge(mAvailableOutputDevices.getDevicesFromType(AUDIO_DEVICE_OUT_SPEAKER));
            devices.remove(speakerSafeDevices);
        }
    }

    return NO_ERROR;
}

status_t AudioPolicyManager::getInputDevicesForAttributes(
        const audio_attributes_t &attr, DeviceVector &devices) {
    devices = DeviceVector(mEngine->getInputDeviceForAttributes(attr));
    return NO_ERROR;
}

status_t AudioPolicyManager::getProfilesForDevices(const DeviceVector& devices,
                                                   AudioProfileVector& audioProfiles,
                                                   uint32_t flags,
                                                   bool isInput) {
    for (const auto& hwModule : mHwModules) {
        // the MSD module checks for different conditions
        if (strcmp(hwModule->getName(), AUDIO_HARDWARE_MODULE_ID_MSD) == 0) {
            continue;
        }
        IOProfileCollection ioProfiles = isInput ? hwModule->getInputProfiles()
                                                 : hwModule->getOutputProfiles();
        for (const auto& profile : ioProfiles) {
            if (!profile->areAllDevicesSupported(devices) ||
                    !profile->isCompatibleProfileForFlags(flags)) {
                continue;
            }
            audioProfiles.addAllValidProfiles(profile->asAudioPort()->getAudioProfiles());
        }
    }

    if (!isInput) {
        // add the direct profiles from MSD if present and has audio patches to all the output(s)
        const auto &msdModule = mHwModules.getModuleFromName(AUDIO_HARDWARE_MODULE_ID_MSD);
        if (msdModule != nullptr) {
            if (msdHasPatchesToAllDevices(devices.toTypeAddrVector())) {
                ALOGV("%s: MSD audio patches set to all output devices.", __func__);
                for (const auto &profile: msdModule->getOutputProfiles()) {
                    if (!profile->asAudioPort()->isDirectOutput()) {
                        continue;
                    }
                    audioProfiles.addAllValidProfiles(profile->asAudioPort()->getAudioProfiles());
                }
            } else {
                ALOGV("%s: MSD audio patches NOT set to all output devices.", __func__);
            }
        }
    }

    return NO_ERROR;
}

sp<SwAudioOutputDescriptor> AudioPolicyManager::reopenOutput(sp<SwAudioOutputDescriptor> outputDesc,
                                                             const audio_config_t *config,
                                                             audio_output_flags_t flags,
                                                             const char* caller) {
    closeOutput(outputDesc->mIoHandle);
    sp<SwAudioOutputDescriptor> preferredOutput = openOutputWithProfileAndDevice(
            outputDesc->mProfile, outputDesc->devices(), nullptr /*mixerConfig*/, config, flags);
    if (preferredOutput == nullptr) {
        ALOGE("%s failed to reopen output device=%d, caller=%s",
              __func__, outputDesc->devices()[0]->getId(), caller);
    }
    return preferredOutput;
}

void AudioPolicyManager::reopenOutputsWithDevices(
        const std::map<audio_io_handle_t, DeviceVector> &outputsToReopen) {
    for (const auto& [output, devices] : outputsToReopen) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueFor(output);
        closeOutput(output);
        openOutputWithProfileAndDevice(desc->mProfile, devices);
    }
}

PortHandleVector AudioPolicyManager::getClientsForStream(
        audio_stream_type_t streamType) const {
    PortHandleVector clients;
    for (size_t i = 0; i < mOutputs.size(); ++i) {
        PortHandleVector clientsForStream = mOutputs.valueAt(i)->getClientsForStream(streamType);
        clients.insert(clients.end(), clientsForStream.begin(), clientsForStream.end());
    }
    return clients;
}

void AudioPolicyManager::invalidateStreams(StreamTypeVector streams) const {
    PortHandleVector clients;
    for (auto stream : streams) {
        PortHandleVector clientsForStream = getClientsForStream(stream);
        clients.insert(clients.end(), clientsForStream.begin(), clientsForStream.end());
    }
    mpClientInterface->invalidateTracks(clients);
}

void AudioPolicyManager::updateClientsInternalMute(
        const sp<android::SwAudioOutputDescriptor> &desc) {
    if (!desc->isBitPerfect() ||
        !com::android::media::audioserver::
                fix_concurrent_playback_behavior_with_bit_perfect_client()) {
        // This is only used for bit perfect output now.
        return;
    }
    sp<TrackClientDescriptor> bitPerfectClient = nullptr;
    bool bitPerfectClientInternalMute = false;
    std::vector<media::TrackInternalMuteInfo> clientsInternalMute;
    for (const sp<TrackClientDescriptor>& client : desc->getActiveClients()) {
        if ((client->flags() & AUDIO_OUTPUT_FLAG_BIT_PERFECT) != AUDIO_OUTPUT_FLAG_NONE) {
            bitPerfectClient = client;
            continue;
        }
        bool muted = false;
        if (client->stream() == AUDIO_STREAM_SYSTEM) {
            // System sound is muted.
            muted = true;
        } else {
            bitPerfectClientInternalMute = true;
        }
        if (client->setInternalMute(muted)) {
            auto result = legacy2aidl_audio_port_handle_t_int32_t(client->portId());
            if (!result.ok()) {
                ALOGE("%s, failed to convert port id(%d) to aidl", __func__, client->portId());
                continue;
            }
            media::TrackInternalMuteInfo info;
            info.portId = result.value();
            info.muted = client->getInternalMute();
            clientsInternalMute.push_back(std::move(info));
        }
    }
    if (bitPerfectClient != nullptr &&
        bitPerfectClient->setInternalMute(bitPerfectClientInternalMute)) {
        auto result = legacy2aidl_audio_port_handle_t_int32_t(bitPerfectClient->portId());
        if (result.ok()) {
            media::TrackInternalMuteInfo info;
            info.portId = result.value();
            info.muted = bitPerfectClient->getInternalMute();
            clientsInternalMute.push_back(std::move(info));
        } else {
            ALOGE("%s, failed to convert port id(%d) of bit perfect client to aidl",
                  __func__, bitPerfectClient->portId());
        }
    }
    if (!clientsInternalMute.empty()) {
        if (status_t status = mpClientInterface->setTracksInternalMute(clientsInternalMute);
                status != NO_ERROR) {
            ALOGE("%s, failed to update tracks internal mute, err=%d", __func__, status);
        }
    }
}

status_t AudioPolicyManager::getMmapPolicyInfos(AudioMMapPolicyType policyType,
                                                std::vector<AudioMMapPolicyInfo> *policyInfos) {
    if (policyType != AudioMMapPolicyType::DEFAULT &&
        policyType != AudioMMapPolicyType::EXCLUSIVE) {
        return BAD_VALUE;
    }
    if (mMmapPolicyByDeviceType.count(policyType) == 0) {
        if (status_t status = updateMmapPolicyInfos(policyType); status != NO_ERROR) {
            return status;
        }
    }
    *policyInfos = mMmapPolicyInfos[policyType];
    return NO_ERROR;
}

status_t AudioPolicyManager::getMmapPolicyForDevice(
        AudioMMapPolicyType policyType, AudioMMapPolicyInfo *policyInfo) {
    if (policyType != AudioMMapPolicyType::DEFAULT &&
        policyType != AudioMMapPolicyType::EXCLUSIVE) {
        return BAD_VALUE;
    }
    if (mMmapPolicyByDeviceType.count(policyType) == 0) {
        if (status_t status = updateMmapPolicyInfos(policyType); status != NO_ERROR) {
            return status;
        }
    }
    auto it = mMmapPolicyByDeviceType[policyType].find(policyInfo->device.type);
    policyInfo->mmapPolicy = it == mMmapPolicyByDeviceType[policyType].end()
            ? AudioMMapPolicy::NEVER : it->second;
    return NO_ERROR;
}

status_t AudioPolicyManager::updateMmapPolicyInfos(AudioMMapPolicyType policyType) {
    std::vector<AudioMMapPolicyInfo> policyInfos;
    if (status_t status = mpClientInterface->getMmapPolicyInfos(policyType, &policyInfos);
        status != NO_ERROR) {
        ALOGE("%s, failed, error = %d", __func__, status);
        return status;
    }
    std::map<AudioDeviceDescription, AudioMMapPolicy> mmapPolicyByDeviceType;
    if (policyInfos.size() == 1 && policyInfos[0].device == AudioDevice()) {
        // When there is only one AudioMMapPolicyInfo instance and the device is a default value,
        // it indicates the mmap policy is reported via system property. In that case, use the
        // routing information to fill details for how mmap is supported for a particular device.
        for (const auto &hwModule: mHwModules) {
            for (const auto &profile: hwModule->getInputProfiles()) {
                if ((profile->getFlags() & AUDIO_INPUT_FLAG_MMAP_NOIRQ)
                    != AUDIO_INPUT_FLAG_MMAP_NOIRQ) {
                    continue;
                }
                for (const auto &device: profile->getSupportedDevices()) {
                    auto deviceDesc =
                            legacy2aidl_audio_devices_t_AudioDeviceDescription(device->type());
                    if (deviceDesc.ok()) {
                        mmapPolicyByDeviceType.emplace(
                                deviceDesc.value(), policyInfos[0].mmapPolicy);
                    }
                }
            }
            for (const auto &profile: hwModule->getOutputProfiles()) {
                if ((profile->getFlags() & AUDIO_OUTPUT_FLAG_MMAP_NOIRQ)
                    != AUDIO_OUTPUT_FLAG_MMAP_NOIRQ) {
                    continue;
                }
                for (const auto &device: profile->getSupportedDevices()) {
                    auto deviceDesc =
                            legacy2aidl_audio_devices_t_AudioDeviceDescription(device->type());
                    if (deviceDesc.ok()) {
                        mmapPolicyByDeviceType.emplace(
                                deviceDesc.value(), policyInfos[0].mmapPolicy);
                    }
                }
            }
        }
    } else {
        for (const auto &info: policyInfos) {
            mmapPolicyByDeviceType[info.device.type] = info.mmapPolicy;
        }
    }
    mMmapPolicyByDeviceType.emplace(policyType, mmapPolicyByDeviceType);
    mMmapPolicyInfos.emplace(policyType, policyInfos);
    return NO_ERROR;
}

} // namespace android
