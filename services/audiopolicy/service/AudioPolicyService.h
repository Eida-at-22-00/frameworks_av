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

#ifndef ANDROID_AUDIOPOLICYSERVICE_H
#define ANDROID_AUDIOPOLICYSERVICE_H

#include <android/media/BnAudioPolicyService.h>
#include <android/media/GetSpatializerResponse.h>
#include <android-base/thread_annotations.h>
#include <audio_utils/mutex.h>
#include <com/android/media/permission/INativePermissionController.h>
#include <cutils/misc.h>
#include <cutils/config_utils.h>
#include <cutils/compiler.h>
#include <utils/String8.h>
#include <utils/Vector.h>
#include <utils/SortedVector.h>
#include <binder/ActivityManager.h>
#include <binder/BinderService.h>
#include <binder/IUidObserver.h>
#include <system/audio.h>
#include <system/audio_policy.h>
#include <media/ToneGenerator.h>
#include <media/AudioEffect.h>
#include <media/AudioPolicy.h>
#include <media/IAudioPolicyServiceLocal.h>
#include <media/NativePermissionController.h>
#include <media/UsecaseValidator.h>
#include <mediautils/ServiceUtilities.h>
#include "AudioPolicyEffects.h"
#include "CaptureStateNotifier.h"
#include "Spatializer.h"
#include <AudioPolicyInterface.h>
#include <android/hardware/BnSensorPrivacyListener.h>
#include <android/content/AttributionSourceState.h>

#include <numeric>
#include <unordered_map>

namespace android {

using content::AttributionSourceState;
using media::audio::common::AudioConfig;
using media::audio::common::AudioConfigBase;
using media::audio::common::AudioDevice;
using media::audio::common::AudioDeviceDescription;
using media::audio::common::AudioFormatDescription;
using media::audio::common::AudioMMapPolicyInfo;
using media::audio::common::AudioMMapPolicyType;
using media::audio::common::AudioMode;
using media::audio::common::AudioSource;
using media::audio::common::AudioStreamType;
using media::audio::common::AudioUsage;
using media::audio::common::AudioUuid;
using media::audio::common::Int;

// ----------------------------------------------------------------------------

namespace media::audiopolicy {
    class AudioRecordClient;
}

using ::android::media::audiopolicy::AudioRecordClient;
using ::com::android::media::permission::INativePermissionController;
using ::com::android::media::permission::NativePermissionController;
using ::com::android::media::permission::IPermissionProvider;

class AudioPolicyService :
    public BinderService<AudioPolicyService>,
    public media::BnAudioPolicyService,
    public IBinder::DeathRecipient,
    public SpatializerPolicyCallback,
    public media::IAudioPolicyServiceLocal
{
    friend class sp<AudioPolicyService>;

public:
    // for BinderService
    static const char *getServiceName() ANDROID_API { return "media.audio_policy"; }

    virtual status_t    dump(int fd, const Vector<String16>& args);

    //
    // BnAudioPolicyService (see AudioPolicyInterface for method descriptions)
    //
    binder::Status onNewAudioModulesAvailable() override;
    binder::Status setDeviceConnectionState(
            media::AudioPolicyDeviceState state,
            const android::media::audio::common::AudioPort& port,
            const AudioFormatDescription& encodedFormat,
            bool deviceSwitch) override;
    binder::Status getDeviceConnectionState(const AudioDevice& device,
                                            media::AudioPolicyDeviceState* _aidl_return) override;
    binder::Status handleDeviceConfigChange(
            const AudioDevice& device,
            const std::string& deviceName,
            const AudioFormatDescription& encodedFormat) override;
    binder::Status setPhoneState(AudioMode state, int32_t uid) override;
    binder::Status setForceUse(media::AudioPolicyForceUse usage,
                               media::AudioPolicyForcedConfig config) override;
    binder::Status getForceUse(media::AudioPolicyForceUse usage,
                               media::AudioPolicyForcedConfig* _aidl_return) override;
    binder::Status getOutput(AudioStreamType stream, int32_t* _aidl_return) override;
    binder::Status getOutputForAttr(const media::audio::common::AudioAttributes& attr,
                                    int32_t session,
                                    const AttributionSourceState &attributionSource,
                                    const AudioConfig& config,
                                    int32_t flags, const std::vector<int32_t>& selectedDeviceIds,
                                    media::GetOutputForAttrResponse* _aidl_return) override;
    binder::Status startOutput(int32_t portId) override;
    binder::Status stopOutput(int32_t portId) override;
    binder::Status releaseOutput(int32_t portId) override;
    binder::Status getInputForAttr(const media::audio::common::AudioAttributes& attr, int32_t input,
                                   int32_t riid, int32_t session,
                                   const AttributionSourceState &attributionSource,
                                   const AudioConfigBase& config, int32_t flags,
                                   int32_t selectedDeviceId,
                                   media::GetInputForAttrResponse* _aidl_return) override;
    binder::Status startInput(int32_t portId) override;
    binder::Status stopInput(int32_t portId) override;
    binder::Status releaseInput(int32_t portId) override;
    binder::Status setDeviceAbsoluteVolumeEnabled(const AudioDevice& device,
                                                  bool enabled,
                                                  AudioStreamType streamToDriveAbs) override;
    binder::Status initStreamVolume(AudioStreamType stream, int32_t indexMin,
                                    int32_t indexMax) override;
    binder::Status setStreamVolumeIndex(AudioStreamType stream,
                                        const AudioDeviceDescription& device,
                                        int32_t index, bool muted) override;
    binder::Status getStreamVolumeIndex(AudioStreamType stream,
                                        const AudioDeviceDescription& device,
                                        int32_t* _aidl_return) override;
    binder::Status setVolumeIndexForAttributes(const media::audio::common::AudioAttributes& attr,
                                               const AudioDeviceDescription& device,
                                               int32_t index, bool muted) override;
    binder::Status getVolumeIndexForAttributes(const media::audio::common::AudioAttributes& attr,
                                               const AudioDeviceDescription& device,
                                               int32_t* _aidl_return) override;
    binder::Status getMaxVolumeIndexForAttributes(const media::audio::common::AudioAttributes& attr,
                                                  int32_t* _aidl_return) override;
    binder::Status getMinVolumeIndexForAttributes(const media::audio::common::AudioAttributes& attr,
                                                  int32_t* _aidl_return) override;
    binder::Status getStrategyForStream(AudioStreamType stream,
                                        int32_t* _aidl_return) override;
    binder::Status getDevicesForAttributes(const media::audio::common::AudioAttributes& attr,
                                           bool forVolume,
                                           std::vector<AudioDevice>* _aidl_return) override;
    binder::Status getOutputForEffect(const media::EffectDescriptor& desc,
                                      int32_t* _aidl_return) override;
    binder::Status registerEffect(const media::EffectDescriptor& desc, int32_t io, int32_t strategy,
                                  int32_t session, int32_t id) override;
    binder::Status unregisterEffect(int32_t id) override;
    binder::Status setEffectEnabled(int32_t id, bool enabled) override;
    binder::Status moveEffectsToIo(const std::vector<int32_t>& ids, int32_t io) override;
    binder::Status isStreamActive(AudioStreamType stream, int32_t inPastMs,
                                  bool* _aidl_return) override;
    binder::Status isStreamActiveRemotely(AudioStreamType stream, int32_t inPastMs,
                                          bool* _aidl_return) override;
    binder::Status isSourceActive(AudioSource source, bool* _aidl_return) override;
    binder::Status queryDefaultPreProcessing(
            int32_t audioSession, Int* count,
            std::vector<media::EffectDescriptor>* _aidl_return) override;
    binder::Status addSourceDefaultEffect(const AudioUuid& type,
                                          const std::string& opPackageName,
                                          const AudioUuid& uuid, int32_t priority,
                                          AudioSource source,
                                          int32_t* _aidl_return) override;
    binder::Status addStreamDefaultEffect(const AudioUuid& type,
                                          const std::string& opPackageName,
                                          const AudioUuid& uuid, int32_t priority,
                                          AudioUsage usage, int32_t* _aidl_return) override;
    binder::Status removeSourceDefaultEffect(int32_t id) override;
    binder::Status removeStreamDefaultEffect(int32_t id) override;
    binder::Status setSupportedSystemUsages(
            const std::vector<AudioUsage>& systemUsages) override;
    binder::Status setAllowedCapturePolicy(int32_t uid, int32_t capturePolicy) override;
    binder::Status getOffloadSupport(const media::audio::common::AudioOffloadInfo& info,
                                     media::AudioOffloadMode* _aidl_return) override;
    binder::Status isDirectOutputSupported(const AudioConfigBase& config,
                                           const media::audio::common::AudioAttributes& attributes,
                                           bool* _aidl_return) override;
    binder::Status listAudioPorts(media::AudioPortRole role, media::AudioPortType type,
                                  Int* count, std::vector<media::AudioPortFw>* ports,
                                  int32_t* _aidl_return) override;
    binder::Status listDeclaredDevicePorts(media::AudioPortRole role,
                                           std::vector<media::AudioPortFw>* _aidl_return) override;
    binder::Status getAudioPort(int portId,
                                media::AudioPortFw* _aidl_return) override;
    binder::Status createAudioPatch(const media::AudioPatchFw& patch, int32_t handle,
                                    int32_t* _aidl_return) override;
    binder::Status releaseAudioPatch(int32_t handle) override;
    binder::Status listAudioPatches(Int* count, std::vector<media::AudioPatchFw>* patches,
                                    int32_t* _aidl_return) override;
    binder::Status setAudioPortConfig(const media::AudioPortConfigFw& config) override;
    binder::Status registerClient(const sp<media::IAudioPolicyServiceClient>& client) override;
    binder::Status setAudioPortCallbacksEnabled(bool enabled) override;
    binder::Status setAudioVolumeGroupCallbacksEnabled(bool enabled) override;
    binder::Status acquireSoundTriggerSession(media::SoundTriggerSession* _aidl_return) override;
    binder::Status releaseSoundTriggerSession(int32_t session) override;
    binder::Status getPhoneState(AudioMode* _aidl_return) override;
    binder::Status registerPolicyMixes(const std::vector<media::AudioMix>& mixes,
                                       bool registration) override;
    binder::Status updatePolicyMixes(
        const ::std::vector<::android::media::AudioMixUpdate>& updates) override;
    binder::Status setUidDeviceAffinities(int32_t uid,
                                          const std::vector<AudioDevice>& devices) override;
    binder::Status removeUidDeviceAffinities(int32_t uid) override;
    binder::Status setUserIdDeviceAffinities(
            int32_t userId,
            const std::vector<AudioDevice>& devices) override;
    binder::Status removeUserIdDeviceAffinities(int32_t userId) override;
    binder::Status startAudioSource(const media::AudioPortConfigFw& source,
                                    const media::audio::common::AudioAttributes& attributes,
                                    int32_t* _aidl_return) override;
    binder::Status stopAudioSource(int32_t portId) override;
    binder::Status setMasterMono(bool mono) override;
    binder::Status getMasterMono(bool* _aidl_return) override;
    binder::Status getStreamVolumeDB(AudioStreamType stream, int32_t index,
                                     const AudioDeviceDescription& device,
                                     float* _aidl_return) override;
    binder::Status getSurroundFormats(Int* count,
                                      std::vector<AudioFormatDescription>* formats,
                                      std::vector<bool>* formatsEnabled) override;
    binder::Status getReportedSurroundFormats(
            Int* count, std::vector<AudioFormatDescription>* formats) override;
    binder::Status getHwOffloadFormatsSupportedForBluetoothMedia(
            const AudioDeviceDescription& device,
            std::vector<AudioFormatDescription>* _aidl_return) override;
    binder::Status setSurroundFormatEnabled(const AudioFormatDescription& audioFormat,
                                            bool enabled) override;
    binder::Status setAssistantServicesUids(const std::vector<int32_t>& uids) override;
    binder::Status setActiveAssistantServicesUids(const std::vector<int32_t>& activeUids) override;
    binder::Status setA11yServicesUids(const std::vector<int32_t>& uids) override;
    binder::Status setCurrentImeUid(int32_t uid) override;
    binder::Status isHapticPlaybackSupported(bool* _aidl_return) override;
    binder::Status isUltrasoundSupported(bool* _aidl_return) override;
    binder::Status isHotwordStreamSupported(bool lookbackAudio, bool* _aidl_return) override;
    binder::Status listAudioProductStrategies(
            std::vector<media::AudioProductStrategy>* _aidl_return) override;
    binder::Status getProductStrategyFromAudioAttributes(
            const media::audio::common::AudioAttributes& aa,
            bool fallbackOnDefault,
            int32_t* _aidl_return) override;
    binder::Status listAudioVolumeGroups(
            std::vector<media::AudioVolumeGroup>* _aidl_return) override;
    binder::Status getVolumeGroupFromAudioAttributes(
            const media::audio::common::AudioAttributes& aa,
            bool fallbackOnDefault,
            int32_t* _aidl_return) override;
    binder::Status setRttEnabled(bool enabled) override;
    binder::Status isCallScreenModeSupported(bool* _aidl_return) override;
    binder::Status setDevicesRoleForStrategy(
            int32_t strategy, media::DeviceRole role,
            const std::vector<AudioDevice>& devices) override;
    binder::Status removeDevicesRoleForStrategy(
            int32_t strategy, media::DeviceRole role,
            const std::vector<AudioDevice>& devices) override;
    binder::Status clearDevicesRoleForStrategy(
            int32_t strategy,
            media::DeviceRole role) override;
    binder::Status getDevicesForRoleAndStrategy(
            int32_t strategy, media::DeviceRole role,
            std::vector<AudioDevice>* _aidl_return) override;
    binder::Status setDevicesRoleForCapturePreset(
            AudioSource audioSource,
            media::DeviceRole role,
            const std::vector<AudioDevice>& devices) override;
    binder::Status addDevicesRoleForCapturePreset(
            AudioSource audioSource,
            media::DeviceRole role,
            const std::vector<AudioDevice>& devices) override;
    binder::Status removeDevicesRoleForCapturePreset(
            AudioSource audioSource,
            media::DeviceRole role,
            const std::vector<AudioDevice>& devices) override;
    binder::Status clearDevicesRoleForCapturePreset(AudioSource audioSource,
                                                    media::DeviceRole role) override;
    binder::Status getDevicesForRoleAndCapturePreset(
            AudioSource audioSource,
            media::DeviceRole role,
            std::vector<AudioDevice>* _aidl_return) override;
    binder::Status registerSoundTriggerCaptureStateListener(
            const sp<media::ICaptureStateListener>& listener, bool* _aidl_return) override;

    binder::Status getSpatializer(const sp<media::INativeSpatializerCallback>& callback,
            media::GetSpatializerResponse* _aidl_return) override;
    binder::Status canBeSpatialized(
            const std::optional<media::audio::common::AudioAttributes>& attr,
            const std::optional<AudioConfig>& config,
            const std::vector<AudioDevice>& devices,
            bool* _aidl_return) override;

    binder::Status getDirectPlaybackSupport(const media::audio::common::AudioAttributes& attr,
                                            const AudioConfig& config,
                                            media::AudioDirectMode* _aidl_return) override;

    binder::Status getDirectProfilesForAttributes(const media::audio::common::AudioAttributes& attr,
                        std::vector<media::audio::common::AudioProfile>* _aidl_return) override;

    binder::Status getSupportedMixerAttributes(
            int32_t portId,
            std::vector<media::AudioMixerAttributesInternal>* _aidl_return) override;
    binder::Status setPreferredMixerAttributes(
            const media::audio::common::AudioAttributes& attr,
            int32_t portId,
            int32_t uid,
            const media::AudioMixerAttributesInternal& mixerAttr) override;
    binder::Status getPreferredMixerAttributes(
            const media::audio::common::AudioAttributes& attr,
            int32_t portId,
            std::optional<media::AudioMixerAttributesInternal>* _aidl_return) override;
    binder::Status clearPreferredMixerAttributes(const media::audio::common::AudioAttributes& attr,
                                                 int32_t portId,
                                                 int32_t uid) override;
    binder::Status getRegisteredPolicyMixes(
            std::vector <::android::media::AudioMix>* mixes) override;

    // Should only be called by AudioService to push permission data down to audioserver
    binder::Status getPermissionController(sp<INativePermissionController>* out) override;

    binder::Status getMmapPolicyInfos(
            AudioMMapPolicyType policyType,
            std::vector<AudioMMapPolicyInfo>* _aidl_return) override;
    binder::Status getMmapPolicyForDevice(
            AudioMMapPolicyType policyType,
            AudioMMapPolicyInfo* policyInfo) override;

    binder::Status setEnableHardening(bool shouldEnable) override;

    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override;

    // -- IAudioPolicyLocal methods
    const IPermissionProvider& getPermissionProvider() const override;
    bool isHardeningOverrideEnabled() const override { return mShouldEnableHardening.load(); };

    // IBinder::DeathRecipient
    virtual     void        binderDied(const wp<IBinder>& who);

    // RefBase
    virtual     void        onFirstRef();

    // Commence initialization when AudioSystem is ready.
    void onAudioSystemReady();

    //
    // Helpers for the struct audio_policy_service_ops implementation.
    // This is used by the audio policy manager for certain operations that
    // are implemented by the policy service.
    //
    virtual void setParameters(audio_io_handle_t ioHandle,
                               const char *keyValuePairs,
                               int delayMs);

    virtual status_t setStreamVolume(audio_stream_type_t stream,
                                     float volume,
                                     bool muted,
                                     audio_io_handle_t output,
                                     int delayMs = 0);

    /**
     * Set a volume on AudioTrack port id(s) for a particular output.
     * For the same user setting, a volume group (and associated given port of the
     * client's track) can have different volumes for each output destination device
     * it is attached to.
     *
     * @param ports to consider
     * @param volume to set
     * @param muted to set
     * @param output to consider
     * @param delayMs to use
     * @return NO_ERROR if successful
     */
    virtual status_t setPortsVolume(const std::vector<audio_port_handle_t> &ports, float volume,
            bool muted, audio_io_handle_t output, int delayMs = 0);
    virtual status_t setVoiceVolume(float volume, int delayMs = 0);

    void doOnNewAudioModulesAvailable();
    status_t doStopOutput(audio_port_handle_t portId);
    void doReleaseOutput(audio_port_handle_t portId);

    status_t clientCreateAudioPatch(const struct audio_patch *patch,
                              audio_patch_handle_t *handle,
                              int delayMs);
    status_t clientReleaseAudioPatch(audio_patch_handle_t handle,
                                     int delayMs);
    virtual status_t clientSetAudioPortConfig(const struct audio_port_config *config,
                                              int delayMs);

    void removeNotificationClient(uid_t uid, pid_t pid);
    void onAudioPortListUpdate();
    void doOnAudioPortListUpdate();
    void onAudioPatchListUpdate();
    void doOnAudioPatchListUpdate();

    void onDynamicPolicyMixStateUpdate(const String8& regId, int32_t state);
    void doOnDynamicPolicyMixStateUpdate(const String8& regId, int32_t state);
    void onRecordingConfigurationUpdate(int event,
                                        const record_client_info_t *clientInfo,
                                        const audio_config_base_t *clientConfig,
                                        std::vector<effect_descriptor_t> clientEffects,
                                        const audio_config_base_t *deviceConfig,
                                        std::vector<effect_descriptor_t> effects,
                                        audio_patch_handle_t patchHandle,
                                        audio_source_t source);
    void doOnRecordingConfigurationUpdate(int event,
                                          const record_client_info_t *clientInfo,
                                          const audio_config_base_t *clientConfig,
                                          std::vector<effect_descriptor_t> clientEffects,
                                          const audio_config_base_t *deviceConfig,
                                          std::vector<effect_descriptor_t> effects,
                                          audio_patch_handle_t patchHandle,
                                          audio_source_t source);

    void onAudioVolumeGroupChanged(volume_group_t group, int flags);
    void doOnAudioVolumeGroupChanged(volume_group_t group, int flags);

    void onRoutingUpdated();
    void doOnRoutingUpdated();

    void onVolumeRangeInitRequest();
    void doOnVolumeRangeInitRequest();

    /**
     * Spatializer SpatializerPolicyCallback implementation.
     * onCheckSpatializer() sends an event on mOutputCommandThread which executes
     * doOnCheckSpatializer() to check if a Spatializer output must be opened or closed
     * by audio policy manager and attach/detach the spatializer effect accordingly.
     */
    void onCheckSpatializer() override;
    void onCheckSpatializer_l() REQUIRES(mMutex);
    void doOnCheckSpatializer();

    void onUpdateActiveSpatializerTracks_l() REQUIRES(mMutex);
    void doOnUpdateActiveSpatializerTracks();


    void setEffectSuspended(int effectId,
                            audio_session_t sessionId,
                            bool suspended);

private:
                        AudioPolicyService() ANDROID_API;
    virtual             ~AudioPolicyService();

    status_t dumpInternals(int fd) REQUIRES(mMutex);

    // Handles binder shell commands
    virtual status_t shellCommand(int in, int out, int err, Vector<String16>& args);


    // Sets whether the given UID records only silence
    virtual void setAppState_l(sp<AudioRecordClient> client, app_state_t state) REQUIRES(mMutex);

    // Overrides the UID state as if it is idle
    status_t handleSetUidState(Vector<String16>& args, int err);

    // Clears the override for the UID state
    status_t handleResetUidState(Vector<String16>& args, int err);

    // Gets the UID state
    status_t handleGetUidState(Vector<String16>& args, int out, int err);

    // Prints the shell command help
    status_t printHelp(int out);

    std::string getDeviceTypeStrForPortId(audio_port_handle_t portId);

    std::string getDeviceTypeStrForPortIds(DeviceIdVector portIds);

    status_t getAudioPolicyEffects(sp<AudioPolicyEffects>& audioPolicyEffects);

    app_state_t apmStatFromAmState(int amState);

    bool isSupportedSystemUsage(audio_usage_t usage);
    binder::Status validateUsage(const audio_attributes_t& attr);
    binder::Status validateUsage(const audio_attributes_t& attr,
                           const AttributionSourceState& attributionSource);

    void updateUidStates();
    void updateUidStates_l() REQUIRES(mMutex);

    void silenceAllRecordings_l() REQUIRES(mMutex);

    static bool isVirtualSource(audio_source_t source);

    /** returns true if the audio source must be silenced when the corresponding app op is denied.
     *          false if the audio source does not actually capture from the microphone while still
     *          being mapped to app op OP_RECORD_AUDIO and not a specialized op tracked separately.
     *          See getOpForSource().
     */
    static bool isAppOpSource(audio_source_t source);

    status_t registerOutput(audio_io_handle_t output,
                            const audio_config_base_t& config,
                            const audio_output_flags_t flags);
    status_t unregisterOutput(audio_io_handle_t output);

    // If recording we need to make sure the UID is allowed to do that. If the UID is idle
    // then it cannot record and gets buffers with zeros - silence. As soon as the UID
    // transitions to an active state we will start reporting buffers with data. This approach
    // transparently handles recording while the UID transitions between idle/active state
    // avoiding to get stuck in a state receiving non-empty buffers while idle or in a state
    // receiving empty buffers while active.
    class UidPolicy : public BnUidObserver, public virtual IBinder::DeathRecipient {
    public:
        explicit UidPolicy(wp<AudioPolicyService> service)
                : mService(service), mObserverRegistered(false),
                  mCurrentImeUid(0),
                  mRttEnabled(false) {}

        void registerSelf();
        void unregisterSelf();

        // IBinder::DeathRecipient implementation
        void binderDied(const wp<IBinder> &who) override;

        bool isUidActive(uid_t uid);
        int getUidState(uid_t uid);
        void setAssistantUids(const std::vector<uid_t>& uids);
        bool isAssistantUid(uid_t uid);
        void setActiveAssistantUids(const std::vector<uid_t>& activeUids);
        bool isActiveAssistantUid(uid_t uid);
        void setA11yUids(const std::vector<uid_t>& uids) { mA11yUids.clear(); mA11yUids = uids; }
        bool isA11yUid(uid_t uid);
        bool isA11yOnTop();
        void setCurrentImeUid(uid_t uid) { mCurrentImeUid = uid; }
        bool isCurrentImeUid(uid_t uid) { return uid == mCurrentImeUid; }
        void setRttEnabled(bool enabled) { mRttEnabled = enabled; }
        bool isRttEnabled() { return mRttEnabled; }

        // BnUidObserver implementation
        void onUidActive(uid_t uid) override;
        void onUidGone(uid_t uid, bool disabled) override;
        void onUidIdle(uid_t uid, bool disabled) override;
        void onUidStateChanged(uid_t uid, int32_t procState, int64_t procStateSeq,
                int32_t capability) override;
        void onUidProcAdjChanged(uid_t uid, int32_t adj) override;


        void updateUid(std::unordered_map<uid_t, std::pair<bool, int>> *uids,
                       uid_t uid, bool active, int state, bool insert);

        void dumpInternals(int fd);

     private:
        void notifyService();
        void updateUidLocked(std::unordered_map<uid_t, std::pair<bool, int>> *uids,
                             uid_t uid, bool active, int state, bool insert);
        void checkRegistered();

        wp<AudioPolicyService> mService;
        audio_utils::mutex mMutex{audio_utils::MutexOrder::kUidPolicy_Mutex};
        ActivityManager mAm;
        bool mObserverRegistered = false;
        std::unordered_map<uid_t, std::pair<bool, int>> mCachedUids GUARDED_BY(mMutex);
        std::vector<uid_t> mAssistantUids;
        std::vector<uid_t> mActiveAssistantUids;
        std::vector<uid_t> mA11yUids;
        uid_t mCurrentImeUid = -1;
        bool mRttEnabled = false;
    };

    // If sensor privacy is enabled then all apps, including those that are active, should be
    // prevented from recording. This is handled similar to idle UIDs, any app that attempts
    // to record while sensor privacy is enabled will receive buffers with zeros. As soon as
    // sensor privacy is disabled active apps will receive the expected data when recording.
    class SensorPrivacyPolicy : public hardware::BnSensorPrivacyListener {
        public:
            explicit SensorPrivacyPolicy(wp<AudioPolicyService> service)
                    : mService(service) {}

            void registerSelf();
            void unregisterSelf();

            bool isSensorPrivacyEnabled();

            binder::Status onSensorPrivacyChanged(int toggleType, int sensor,
                                                  bool enabled);

            binder::Status onSensorPrivacyStateChanged(int, int, int) {
                return binder::Status::ok();
            }

        private:
            wp<AudioPolicyService> mService;
            std::atomic_bool mSensorPrivacyEnabled = false;
    };

    // Thread used to send audio config commands to audio flinger
    // For audio config commands, it is necessary because audio flinger requires that the calling
    // process (user) has permission to modify audio settings.
    public:
    class AudioCommandThread : public Thread {
        class AudioCommand;
    public:

        // commands for tone AudioCommand
        enum {
            SET_VOLUME,
            SET_PORTS_VOLUME,
            SET_PARAMETERS,
            SET_VOICE_VOLUME,
            STOP_OUTPUT,
            RELEASE_OUTPUT,
            CREATE_AUDIO_PATCH,
            RELEASE_AUDIO_PATCH,
            UPDATE_AUDIOPORT_LIST,
            UPDATE_AUDIOPATCH_LIST,
            CHANGED_AUDIOVOLUMEGROUP,
            SET_AUDIOPORT_CONFIG,
            DYN_POLICY_MIX_STATE_UPDATE,
            RECORDING_CONFIGURATION_UPDATE,
            SET_EFFECT_SUSPENDED,
            AUDIO_MODULES_UPDATE,
            ROUTING_UPDATED,
            UPDATE_UID_STATES,
            CHECK_SPATIALIZER_OUTPUT, // verify if spatializer effect should be created or moved
            UPDATE_ACTIVE_SPATIALIZER_TRACKS, // Update active track counts on spalializer output
            VOL_RANGE_INIT_REQUEST, // request to reset the volume range indices
        };

        AudioCommandThread (String8 name, const wp<AudioPolicyService>& service);
        virtual             ~AudioCommandThread();

                    status_t    dump(int fd);

        // Thread virtuals
        virtual     void        onFirstRef();
        virtual     bool        threadLoop();

                    void        exit();
                    status_t    volumeCommand(audio_stream_type_t stream, float volume, bool muted,
                                            audio_io_handle_t output, int delayMs = 0);
                    status_t    volumePortsCommand(const std::vector<audio_port_handle_t> &ports,
                            float volume, bool muted, audio_io_handle_t output, int delayMs = 0);
                    status_t    parametersCommand(audio_io_handle_t ioHandle,
                                            const char *keyValuePairs, int delayMs = 0);
                    status_t    voiceVolumeCommand(float volume, int delayMs = 0);
                    void        stopOutputCommand(audio_port_handle_t portId);
                    void        releaseOutputCommand(audio_port_handle_t portId);
                    status_t    sendCommand(sp<AudioCommand>& command, int delayMs = 0);
                    void        insertCommand_l(sp<AudioCommand>& command, int delayMs = 0);
                    status_t    createAudioPatchCommand(const struct audio_patch *patch,
                                                        audio_patch_handle_t *handle,
                                                        int delayMs);
                    status_t    releaseAudioPatchCommand(audio_patch_handle_t handle,
                                                         int delayMs);
                    void        updateAudioPortListCommand();
                    void        updateAudioPatchListCommand();
                    void        changeAudioVolumeGroupCommand(volume_group_t group, int flags);
                    status_t    setAudioPortConfigCommand(const struct audio_port_config *config,
                                                          int delayMs);
                    void        dynamicPolicyMixStateUpdateCommand(const String8& regId,
                                                                   int32_t state);
                    void        recordingConfigurationUpdateCommand(
                                                    int event,
                                                    const record_client_info_t *clientInfo,
                                                    const audio_config_base_t *clientConfig,
                                                    std::vector<effect_descriptor_t> clientEffects,
                                                    const audio_config_base_t *deviceConfig,
                                                    std::vector<effect_descriptor_t> effects,
                                                    audio_patch_handle_t patchHandle,
                                                    audio_source_t source);
                    void        setEffectSuspendedCommand(int effectId,
                                                          audio_session_t sessionId,
                                                          bool suspended);
                    void        audioModulesUpdateCommand();
                    void        routingChangedCommand();
                    void        updateUidStatesCommand();
                    void        checkSpatializerCommand();
                    void        updateActiveSpatializerTracksCommand();
                    void        volRangeInitReqCommand();

                    void        insertCommand_l(AudioCommand *command, int delayMs = 0);
    private:
        class AudioCommandData;

        // descriptor for requested tone playback event
        class AudioCommand: public RefBase {

        public:
            AudioCommand()
            : mCommand(-1), mStatus(NO_ERROR), mWaitStatus(false) {}

            void dump(char* buffer, size_t size);

            int mCommand;   // SET_VOLUME, SET_PARAMETERS...
            nsecs_t mTime;  // time stamp
            audio_utils::mutex mMutex{audio_utils::MutexOrder::kAudioCommand_Mutex};
            audio_utils::condition_variable mCond; // condition for status return
            status_t mStatus; // command status
            bool mWaitStatus; // true if caller is waiting for status
            sp<AudioCommandData> mParam;     // command specific parameter data
        };

        class AudioCommandData: public RefBase {
        public:
            virtual ~AudioCommandData() {}
        protected:
            AudioCommandData() {}
        };

        class VolumeData : public AudioCommandData {
        public:
            audio_stream_type_t mStream;
            float mVolume;
            bool mIsMuted;
            audio_io_handle_t mIO;
        };

        class VolumePortsData : public AudioCommandData {
        public:
            std::vector<audio_port_handle_t> mPorts;
            float mVolume;
            bool mMuted;
            audio_io_handle_t mIO;
            std::string dumpPorts() {
                return std::string("volume ") + std::to_string(mVolume) + std::string("muted ") +
                       std::to_string(mMuted) + " on IO " + std::to_string(mIO) + " and ports " +
                       std::accumulate(std::begin(mPorts), std::end(mPorts), std::string{},
                                       [](const std::string &ls, int rs) {
                                           return ls + std::to_string(rs) + " ";
                                       });
            }
        };

        class ParametersData : public AudioCommandData {
        public:
            audio_io_handle_t mIO;
            String8 mKeyValuePairs;
        };

        class VoiceVolumeData : public AudioCommandData {
        public:
            float mVolume;
        };

        class StopOutputData : public AudioCommandData {
        public:
            audio_port_handle_t mPortId;
        };

        class ReleaseOutputData : public AudioCommandData {
        public:
            audio_port_handle_t mPortId;
        };

        class CreateAudioPatchData : public AudioCommandData {
        public:
            struct audio_patch mPatch;
            audio_patch_handle_t mHandle;
        };

        class ReleaseAudioPatchData : public AudioCommandData {
        public:
            audio_patch_handle_t mHandle;
        };

        class AudioVolumeGroupData : public AudioCommandData {
        public:
            volume_group_t mGroup;
            int mFlags;
        };

        class SetAudioPortConfigData : public AudioCommandData {
        public:
            struct audio_port_config mConfig;
        };

        class DynPolicyMixStateUpdateData : public AudioCommandData {
        public:
            String8 mRegId;
            int32_t mState;
        };

        class RecordingConfigurationUpdateData : public AudioCommandData {
        public:
            int mEvent;
            record_client_info_t mClientInfo;
            struct audio_config_base mClientConfig;
            std::vector<effect_descriptor_t> mClientEffects;
            struct audio_config_base mDeviceConfig;
            std::vector<effect_descriptor_t> mEffects;
            audio_patch_handle_t mPatchHandle;
            audio_source_t mSource;
        };

        class SetEffectSuspendedData : public AudioCommandData {
        public:
            int mEffectId;
            audio_session_t mSessionId;
            bool mSuspended;
        };

        mutable audio_utils::mutex mMutex{audio_utils::MutexOrder::kCommandThread_Mutex};
        audio_utils::condition_variable mWaitWorkCV;
        Vector < sp<AudioCommand> > mAudioCommands; // list of pending commands
        sp<AudioCommand> mLastCommand;      // last processed command (used by dump)
        String8 mName;                      // string used by wake lock fo delayed commands
        wp<AudioPolicyService> mService;
    };

    private:
    class AudioPolicyClient : public AudioPolicyClientInterface
    {
     public:
        explicit AudioPolicyClient(AudioPolicyService *service) : mAudioPolicyService(service) {}
        virtual ~AudioPolicyClient() {}

        virtual status_t getAudioPolicyConfig(media::AudioPolicyConfig *config);

        //
        // Audio HW module functions
        //

        // loads a HW module.
        virtual audio_module_handle_t loadHwModule(const char *name);

        //
        // Audio output Control functions
        //

        // opens an audio output with the requested parameters. The parameter values can indicate to use the default values
        // in case the audio policy manager has no specific requirements for the output being opened.
        // When the function returns, the parameter values reflect the actual values used by the audio hardware output stream.
        // The audio policy manager can check if the proposed parameters are suitable or not and act accordingly.
        virtual status_t openOutput(audio_module_handle_t module,
                                    audio_io_handle_t *output,
                                    audio_config_t *halConfig,
                                    audio_config_base_t *mixerConfig,
                                    const sp<DeviceDescriptorBase>& device,
                                    uint32_t *latencyMs,
                                    audio_output_flags_t *flags,
                                    audio_attributes_t attributes);
        // creates a special output that is duplicated to the two outputs passed as arguments. The duplication is performed by
        // a special mixer thread in the AudioFlinger.
        virtual audio_io_handle_t openDuplicateOutput(audio_io_handle_t output1, audio_io_handle_t output2);
        // closes the output stream
        virtual status_t closeOutput(audio_io_handle_t output);
        // suspends the output. When an output is suspended, the corresponding audio hardware output stream is placed in
        // standby and the AudioTracks attached to the mixer thread are still processed but the output mix is discarded.
        virtual status_t suspendOutput(audio_io_handle_t output);
        // restores a suspended output.
        virtual status_t restoreOutput(audio_io_handle_t output);

        //
        // Audio input Control functions
        //

        // opens an audio input
        virtual audio_io_handle_t openInput(audio_module_handle_t module,
                                            audio_io_handle_t *input,
                                            audio_config_t *config,
                                            audio_devices_t *devices,
                                            const String8& address,
                                            audio_source_t source,
                                            audio_input_flags_t flags);
        // closes an audio input
        virtual status_t closeInput(audio_io_handle_t input);
        //
        // misc control functions
        //

        // set a stream volume for a particular output. For the same user setting, a given stream
        // type can have different volumes for each output (destination device) it is attached to.
        virtual status_t setStreamVolume(audio_stream_type_t stream, float volume, bool muted,
                audio_io_handle_t output, int delayMs = 0);
        /**
         * Set a volume on port(s) for a particular output. For the same user setting, a volume
         * group (and associated given port of the client's track) can have different volumes for
         * each output (destination device) it is attached to.
         *
         * @param ports to consider
         * @param volume to set
         * @param muted to set
         * @param output to consider
         * @param delayMs to use
         * @return NO_ERROR if successful
         */
        status_t setPortsVolume(const std::vector<audio_port_handle_t> &ports, float volume,
                bool muted, audio_io_handle_t output, int delayMs = 0) override;

        // function enabling to send proprietary informations directly from audio policy manager to audio hardware interface.
        virtual void setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs, int delayMs = 0);
        // function enabling to receive proprietary informations directly from audio hardware interface to audio policy manager.
        virtual String8 getParameters(audio_io_handle_t ioHandle, const String8& keys);

        // set down link audio volume.
        virtual status_t setVoiceVolume(float volume, int delayMs = 0);

        // move effect to the specified output
        virtual status_t moveEffects(audio_session_t session,
                                         audio_io_handle_t srcOutput,
                                         audio_io_handle_t dstOutput);

                void setEffectSuspended(int effectId,
                                        audio_session_t sessionId,
                                        bool suspended) override;

        /* Create a patch between several source and sink ports */
        virtual status_t createAudioPatch(const struct audio_patch *patch,
                                           audio_patch_handle_t *handle,
                                           int delayMs);

        /* Release a patch */
        virtual status_t releaseAudioPatch(audio_patch_handle_t handle,
                                           int delayMs);

        /* Set audio port configuration */
        virtual status_t setAudioPortConfig(const struct audio_port_config *config, int delayMs);

        virtual void onAudioPortListUpdate();
        virtual void onAudioPatchListUpdate();
        virtual void onDynamicPolicyMixStateUpdate(String8 regId, int32_t state);
        virtual void onRecordingConfigurationUpdate(int event,
                                                    const record_client_info_t *clientInfo,
                                                    const audio_config_base_t *clientConfig,
                                                    std::vector<effect_descriptor_t> clientEffects,
                                                    const audio_config_base_t *deviceConfig,
                                                    std::vector<effect_descriptor_t> effects,
                                                    audio_patch_handle_t patchHandle,
                                                    audio_source_t source);

        virtual void onAudioVolumeGroupChanged(volume_group_t group, int flags);

        virtual void onRoutingUpdated();

        virtual void onVolumeRangeInitRequest();

        virtual audio_unique_id_t newAudioUniqueId(audio_unique_id_use_t use);

        void setSoundTriggerCaptureState(bool active) override;

        status_t getAudioPort(struct audio_port_v7 *port) override;

        status_t updateSecondaryOutputs(
                const TrackSecondaryOutputsMap& trackSecondaryOutputs) override;

        status_t setDeviceConnectedState(
                const struct audio_port_v7 *port, media::DeviceConnectedState state) override;

        status_t invalidateTracks(const std::vector<audio_port_handle_t>& portIds) override;

        status_t getAudioMixPort(const struct audio_port_v7 *devicePort,
                                 struct audio_port_v7 *port) override;

        status_t setTracksInternalMute(
                const std::vector<media::TrackInternalMuteInfo>& tracksInternalMute) override;

        status_t getMmapPolicyInfos(
                media::audio::common::AudioMMapPolicyType policyType,
                std::vector<media::audio::common::AudioMMapPolicyInfo> *policyInfos) override;

        error::BinderResult<bool> checkPermissionForInput(const AttributionSourceState& attr,
                const PermissionReqs& req) override;

     private:
        AudioPolicyService *mAudioPolicyService;
    };

    // --- Notification Client ---
    class NotificationClient : public IBinder::DeathRecipient {
    public:
                            NotificationClient(const sp<AudioPolicyService>& service,
                                                const sp<media::IAudioPolicyServiceClient>& client,
                                                uid_t uid, pid_t pid);
        virtual             ~NotificationClient();

                            void      onAudioPortListUpdate();
                            void      onAudioPatchListUpdate();
                            void      onDynamicPolicyMixStateUpdate(const String8& regId,
                                                                    int32_t state);
                            void      onAudioVolumeGroupChanged(volume_group_t group, int flags);
                            void      onRecordingConfigurationUpdate(
                                                    int event,
                                                    const record_client_info_t *clientInfo,
                                                    const audio_config_base_t *clientConfig,
                                                    std::vector<effect_descriptor_t> clientEffects,
                                                    const audio_config_base_t *deviceConfig,
                                                    std::vector<effect_descriptor_t> effects,
                                                    audio_patch_handle_t patchHandle,
                                                    audio_source_t source);
                            void      onRoutingUpdated();
                            void      onVolumeRangeInitRequest();
                            void      setAudioPortCallbacksEnabled(bool enabled);
                            void setAudioVolumeGroupCallbacksEnabled(bool enabled);

                            uid_t uid() {
                                return mUid;
                            }

                // IBinder::DeathRecipient
                virtual     void        binderDied(const wp<IBinder>& who);

    private:
                            NotificationClient(const NotificationClient&);
                            NotificationClient& operator = (const NotificationClient&);

        const wp<AudioPolicyService>               mService;
        const uid_t                                mUid;
        const pid_t                                mPid;
        const sp<media::IAudioPolicyServiceClient> mAudioPolicyServiceClient;
              bool                                 mAudioPortCallbacksEnabled;
              bool                                 mAudioVolumeGroupCallbacksEnabled;
    };

    public:
    class AudioClient : public virtual RefBase {
    public:
                AudioClient(const audio_attributes_t attributes,
                            const audio_io_handle_t io,
                            const AttributionSourceState& attributionSource,
                            const audio_session_t session,  audio_port_handle_t portId,
                            const DeviceIdVector deviceIds) :
                                attributes(attributes), io(io), attributionSource(
                                attributionSource), session(session), portId(portId),
                                deviceIds(deviceIds), active(false) {}
                ~AudioClient() override = default;


        const audio_attributes_t attributes; // source, flags ...
        const audio_io_handle_t io;          // audio HAL stream IO handle
        const AttributionSourceState attributionSource; //client attributionsource
        const audio_session_t session;       // audio session ID
        const audio_port_handle_t portId;
        const DeviceIdVector deviceIds;  // selected input device port IDs
              bool active;                   // Playback/Capture is active or inactive
    };
    private:



    // --- AudioPlaybackClient ---
    // Information about each registered AudioTrack client
    // (between calls to getOutputForAttr() and releaseOutput())
    class AudioPlaybackClient : public AudioClient {
    public:
                AudioPlaybackClient(const audio_attributes_t attributes,
                      const audio_io_handle_t io, AttributionSourceState attributionSource,
                            const audio_session_t session, audio_port_handle_t portId,
                            DeviceIdVector deviceIds, audio_stream_type_t stream,
                            bool isSpatialized, audio_channel_mask_t channelMask) :
                    AudioClient(attributes, io, attributionSource, session, portId,
                        deviceIds), stream(stream), isSpatialized(isSpatialized),
                        channelMask(channelMask) {}
                ~AudioPlaybackClient() override = default;

        const audio_stream_type_t stream;
        const bool isSpatialized;
        const audio_channel_mask_t channelMask;
    };

    void getPlaybackClientAndEffects(audio_port_handle_t portId,
                                     sp<AudioPlaybackClient>& client,
                                     sp<AudioPolicyEffects>& effects,
                                     const char *context);


    // A class automatically clearing and restoring binder caller identity inside
    // a code block (scoped variable)
    // Declare one systematically before calling AudioPolicyManager methods so that they are
    // executed with the same level of privilege as audioserver process.
    class AutoCallerClear {
    public:
            AutoCallerClear() :
                mToken(IPCThreadState::self()->clearCallingIdentity()) {}
            ~AutoCallerClear() {
                IPCThreadState::self()->restoreCallingIdentity(mToken);
            }

    private:
        const   int64_t mToken;
    };

    // Internal dump utilities.
    status_t dumpPermissionDenial(int fd);
    void loadAudioPolicyManager();
    void unloadAudioPolicyManager();

    /**
     * Returns the channel masks for active audio tracks on the specified output mixer.
     * The query can be specified to only include spatialized audio tracks or consider
     * all tracks.
     * @param output the I/O handle of the output mixer to consider
     * @param spatializedOnly true if only spatialized tracks should be considered
     * @return a list of channel masks for all active tracks matching the condition.
     */
    std::vector<audio_channel_mask_t> getActiveTracksMasks_l(
            audio_io_handle_t output, bool spatializedOnly = true) REQUIRES(mMutex);

    mutable audio_utils::mutex mMutex{audio_utils::MutexOrder::kAudioPolicyService_Mutex};
    // prevents concurrent access to AudioPolicy manager functions changing
    // device connection state or routing.
    // mMutex protects AudioPolicyManager methods that can call into audio flinger
    // and possibly back in to audio policy service and acquire mEffectsLock.
    sp<AudioCommandThread> mAudioCommandThread;     // audio commands thread
    sp<AudioCommandThread> mOutputCommandThread;    // process stop and release output
    AudioPolicyInterface *mAudioPolicyManager;
    AudioPolicyClient *mAudioPolicyClient;
    std::vector<audio_usage_t> mSupportedSystemUsages;

    mutable audio_utils::mutex mNotificationClientsMutex{
            audio_utils::MutexOrder::kAudioPolicyService_NotificationClientsMutex};
    DefaultKeyedVector<int64_t, sp<NotificationClient>> mNotificationClients
            GUARDED_BY(mNotificationClientsMutex);
    // Manage all effects configured in audio_effects.conf
    // never hold AudioPolicyService::mMutex when calling AudioPolicyEffects methods as
    // those can call back into AudioPolicyService methods and try to acquire the mutex
    sp<AudioPolicyEffects> mAudioPolicyEffects GUARDED_BY(mMutex);
    audio_mode_t mPhoneState GUARDED_BY(mMutex);
    uid_t mPhoneStateOwnerUid GUARDED_BY(mMutex);

    sp<UidPolicy> mUidPolicy GUARDED_BY(mMutex);
    sp<SensorPrivacyPolicy> mSensorPrivacyPolicy GUARDED_BY(mMutex);

    DefaultKeyedVector<audio_port_handle_t, sp<AudioRecordClient>> mAudioRecordClients
            GUARDED_BY(mMutex);
    DefaultKeyedVector<audio_port_handle_t, sp<AudioPlaybackClient>> mAudioPlaybackClients
            GUARDED_BY(mMutex);

    MediaPackageManager mPackageManager; // To check allowPlaybackCapture

    CaptureStateNotifier mCaptureStateNotifier;

    // created in onFirstRef() and never cleared: does not need to be guarded by mMutex
    sp<Spatializer> mSpatializer;

    void *mLibraryHandle = nullptr;
    CreateAudioPolicyManagerInstance mCreateAudioPolicyManager;
    DestroyAudioPolicyManagerInstance mDestroyAudioPolicyManager;
    std::unique_ptr<media::UsecaseValidator> mUsecaseValidator;
    const sp<NativePermissionController> mPermissionController;
    std::atomic<bool> mShouldEnableHardening;
};

} // namespace android

#endif // ANDROID_AUDIOPOLICYSERVICE_H
