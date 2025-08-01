/*
**
** Copyright 2007, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#pragma once

// Classes and interfaces directly used.
#include "Client.h"
#include "DeviceEffectManager.h"
#include "EffectConfiguration.h"
#include "IAfEffect.h"
#include "IAfPatchPanel.h"
#include "IAfThread.h"
#include "IAfTrack.h"
#include "MelReporter.h"
#include "PatchCommandThread.h"
#include "audio_utils/clock.h"

// External classes
#include <audio_utils/mutex.h>
#include <audio_utils/FdToString.h>
#include <audio_utils/SimpleLog.h>
#include <com/android/media/permission/PermissionEnum.h>
#include <media/IAudioFlinger.h>
#include <media/IAudioPolicyServiceLocal.h>
#include <media/MediaMetricsItem.h>
#include <media/audiohal/DevicesFactoryHalInterface.h>
#include <mediautils/ServiceUtilities.h>
#include <mediautils/Synchronization.h>
#include <psh_utils/AudioPowerManager.h>

// not needed with the includes above, added to prevent transitive include dependency.
#include <utils/KeyedVector.h>
#include <utils/String16.h>
#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <variant>

namespace android {

class AudioFlinger
    : public AudioFlingerServerAdapter::Delegate  // IAudioFlinger client interface
    , public IAfClientCallback
    , public IAfDeviceEffectManagerCallback
    , public IAfMelReporterCallback
    , public IAfPatchPanelCallback
    , public IAfThreadCallback
{
    friend class sp<AudioFlinger>;
public:
    static void instantiate() ANDROID_API;

    status_t resetReferencesForTest();

    // Called by main when startup finished -- for logging purposes only
    void startupFinished() {
        mStartupFinishedTime.store(audio_utils_get_real_time_ns(), std::memory_order_release);
    }

private:

    // ---- begin IAudioFlinger interface

    status_t dump(int fd, const Vector<String16>& args) final EXCLUDES_AudioFlinger_Mutex;

    status_t createTrack(const media::CreateTrackRequest& input,
            media::CreateTrackResponse& output) final EXCLUDES_AudioFlinger_Mutex;

    status_t createRecord(const media::CreateRecordRequest& input,
            media::CreateRecordResponse& output) final EXCLUDES_AudioFlinger_Mutex;

    uint32_t sampleRate(audio_io_handle_t ioHandle) const final EXCLUDES_AudioFlinger_Mutex;
    audio_format_t format(audio_io_handle_t output) const final EXCLUDES_AudioFlinger_Mutex;
    size_t frameCount(audio_io_handle_t ioHandle) const final EXCLUDES_AudioFlinger_Mutex;
    size_t frameCountHAL(audio_io_handle_t ioHandle) const final EXCLUDES_AudioFlinger_Mutex;
    uint32_t latency(audio_io_handle_t output) const final EXCLUDES_AudioFlinger_Mutex;

    status_t setMasterVolume(float value) final EXCLUDES_AudioFlinger_Mutex;
    status_t setMasterMute(bool muted) final EXCLUDES_AudioFlinger_Mutex;
    float masterVolume() const final EXCLUDES_AudioFlinger_Mutex;
    bool masterMute() const final EXCLUDES_AudioFlinger_Mutex;

    // Balance value must be within -1.f (left only) to 1.f (right only) inclusive.
    status_t setMasterBalance(float balance) final EXCLUDES_AudioFlinger_Mutex;
    status_t getMasterBalance(float* balance) const final EXCLUDES_AudioFlinger_Mutex;

    status_t setStreamVolume(audio_stream_type_t stream, float value,
            bool muted, audio_io_handle_t output) final EXCLUDES_AudioFlinger_Mutex;
    status_t setStreamMute(audio_stream_type_t stream, bool muted) final
            EXCLUDES_AudioFlinger_Mutex;

    status_t setPortsVolume(const std::vector<audio_port_handle_t>& portIds, float volume,
                            bool muted, audio_io_handle_t output) final EXCLUDES_AudioFlinger_Mutex;

    status_t setMode(audio_mode_t mode) final EXCLUDES_AudioFlinger_Mutex;

    status_t setMicMute(bool state) final EXCLUDES_AudioFlinger_Mutex;
    bool getMicMute() const final EXCLUDES_AudioFlinger_Mutex;

    void setRecordSilenced(audio_port_handle_t portId, bool silenced) final
            EXCLUDES_AudioFlinger_Mutex;

    status_t setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs) final
            EXCLUDES_AudioFlinger_Mutex;
    String8 getParameters(audio_io_handle_t ioHandle, const String8& keys) const final
            EXCLUDES_AudioFlinger_Mutex;

    void registerClient(const sp<media::IAudioFlingerClient>& client) final
            EXCLUDES_AudioFlinger_Mutex;
    size_t getInputBufferSize(uint32_t sampleRate, audio_format_t format,
            audio_channel_mask_t channelMask) const final EXCLUDES_AudioFlinger_Mutex;

    status_t openOutput(const media::OpenOutputRequest& request,
            media::OpenOutputResponse* response) final EXCLUDES_AudioFlinger_Mutex;

    audio_io_handle_t openDuplicateOutput(audio_io_handle_t output1,
            audio_io_handle_t output2) final EXCLUDES_AudioFlinger_Mutex;

    status_t closeOutput(audio_io_handle_t output) final EXCLUDES_AudioFlinger_Mutex;

    status_t suspendOutput(audio_io_handle_t output) final EXCLUDES_AudioFlinger_Mutex;

    status_t restoreOutput(audio_io_handle_t output) final EXCLUDES_AudioFlinger_Mutex;

    status_t openInput(const media::OpenInputRequest& request,
            media::OpenInputResponse* response) final EXCLUDES_AudioFlinger_Mutex;

    status_t closeInput(audio_io_handle_t input) final EXCLUDES_AudioFlinger_Mutex;

    status_t setVoiceVolume(float volume) final EXCLUDES_AudioFlinger_Mutex;

    status_t getRenderPosition(uint32_t* halFrames, uint32_t* dspFrames,
            audio_io_handle_t output) const final EXCLUDES_AudioFlinger_Mutex;

    uint32_t getInputFramesLost(audio_io_handle_t ioHandle) const final
            EXCLUDES_AudioFlinger_Mutex;

    // This is the binder API.  For the internal API see nextUniqueId().
    audio_unique_id_t newAudioUniqueId(audio_unique_id_use_t use) final
            EXCLUDES_AudioFlinger_Mutex;

    void acquireAudioSessionId(audio_session_t audioSession, pid_t pid, uid_t uid) final
            EXCLUDES_AudioFlinger_Mutex;

    void releaseAudioSessionId(audio_session_t audioSession, pid_t pid) final
            EXCLUDES_AudioFlinger_Mutex;

    status_t queryNumberEffects(uint32_t* numEffects) const final EXCLUDES_AudioFlinger_Mutex;

    status_t queryEffect(uint32_t index, effect_descriptor_t* descriptor) const final
            EXCLUDES_AudioFlinger_Mutex;

    status_t getEffectDescriptor(const effect_uuid_t* pUuid,
            const effect_uuid_t* pTypeUuid,
            uint32_t preferredTypeFlag,
            effect_descriptor_t* descriptor) const final EXCLUDES_AudioFlinger_Mutex;

    status_t createEffect(const media::CreateEffectRequest& request,
            media::CreateEffectResponse* response) final EXCLUDES_AudioFlinger_Mutex;

    status_t moveEffects(audio_session_t sessionId, audio_io_handle_t srcOutput,
            audio_io_handle_t dstOutput) final EXCLUDES_AudioFlinger_Mutex;

    void setEffectSuspended(int effectId,
            audio_session_t sessionId,
            bool suspended) final EXCLUDES_AudioFlinger_Mutex;

    audio_module_handle_t loadHwModule(const char* name) final EXCLUDES_AudioFlinger_Mutex;

    uint32_t getPrimaryOutputSamplingRate() const final EXCLUDES_AudioFlinger_Mutex;
    size_t getPrimaryOutputFrameCount() const final EXCLUDES_AudioFlinger_Mutex;

    status_t setLowRamDevice(bool isLowRamDevice, int64_t totalMemory) final
            EXCLUDES_AudioFlinger_Mutex;

    /* Get attributes for a given audio port */
    status_t getAudioPort(struct audio_port_v7* port) const final EXCLUDES_AudioFlinger_Mutex;

    /* Create an audio patch between several source and sink ports */
    status_t createAudioPatch(const struct audio_patch *patch,
            audio_patch_handle_t* handle) final EXCLUDES_AudioFlinger_Mutex;

    /* Release an audio patch */
    status_t releaseAudioPatch(audio_patch_handle_t handle) final EXCLUDES_AudioFlinger_Mutex;

    /* List existing audio patches */
    status_t listAudioPatches(unsigned int* num_patches,
            struct audio_patch* patches) const final EXCLUDES_AudioFlinger_Mutex;

    /* Set audio port configuration */
    status_t setAudioPortConfig(const struct audio_port_config* config) final
            EXCLUDES_AudioFlinger_Mutex;

    /* Get the HW synchronization source used for an audio session */
    audio_hw_sync_t getAudioHwSyncForSession(audio_session_t sessionId) final
            EXCLUDES_AudioFlinger_Mutex;

    /* Indicate JAVA services are ready (scheduling, power management ...) */
    status_t systemReady() final EXCLUDES_AudioFlinger_Mutex;
    status_t audioPolicyReady() final { mAudioPolicyReady.store(true); return NO_ERROR; }

    status_t getMicrophones(std::vector<media::MicrophoneInfoFw>* microphones) const final
            EXCLUDES_AudioFlinger_Mutex;

    status_t setAudioHalPids(const std::vector<pid_t>& pids) final
            EXCLUDES_AudioFlinger_Mutex;

    status_t setVibratorInfos(const std::vector<media::AudioVibratorInfo>& vibratorInfos) final
            EXCLUDES_AudioFlinger_Mutex;

    status_t updateSecondaryOutputs(
            const TrackSecondaryOutputsMap& trackSecondaryOutputs) final
            EXCLUDES_AudioFlinger_Mutex;

    status_t getMmapPolicyInfos(
            media::audio::common::AudioMMapPolicyType policyType,
            std::vector<media::audio::common::AudioMMapPolicyInfo>* policyInfos) final
            EXCLUDES_AudioFlinger_Mutex;

    int32_t getAAudioMixerBurstCount() const final EXCLUDES_AudioFlinger_Mutex;

    int32_t getAAudioHardwareBurstMinUsec() const final EXCLUDES_AudioFlinger_Mutex;

    status_t setDeviceConnectedState(const struct audio_port_v7* port,
            media::DeviceConnectedState state) final EXCLUDES_AudioFlinger_Mutex;

    status_t setSimulateDeviceConnections(bool enabled) final EXCLUDES_AudioFlinger_Mutex;

    status_t setRequestedLatencyMode(
            audio_io_handle_t output, audio_latency_mode_t mode) final
            EXCLUDES_AudioFlinger_Mutex;

    status_t getSupportedLatencyModes(audio_io_handle_t output,
            std::vector<audio_latency_mode_t>* modes) const final EXCLUDES_AudioFlinger_Mutex;

    status_t setBluetoothVariableLatencyEnabled(bool enabled) final EXCLUDES_AudioFlinger_Mutex;

    status_t isBluetoothVariableLatencyEnabled(bool* enabled) const final
            EXCLUDES_AudioFlinger_Mutex;

    status_t supportsBluetoothVariableLatency(bool* support) const final
            EXCLUDES_AudioFlinger_Mutex;

    status_t getSoundDoseInterface(const sp<media::ISoundDoseCallback>& callback,
            sp<media::ISoundDose>* soundDose) const final EXCLUDES_AudioFlinger_Mutex;

    status_t invalidateTracks(const std::vector<audio_port_handle_t>& portIds) final
            EXCLUDES_AudioFlinger_Mutex;

    status_t getAudioPolicyConfig(media::AudioPolicyConfig* config) final
            EXCLUDES_AudioFlinger_Mutex;

    // Get the attributes of the mix port when connecting to the given device port.
    status_t getAudioMixPort(const struct audio_port_v7* devicePort,
                             struct audio_port_v7* mixPort) const final EXCLUDES_AudioFlinger_Mutex;

    status_t setTracksInternalMute(
            const std::vector<media::TrackInternalMuteInfo>& tracksInternalMute) final
            EXCLUDES_AudioFlinger_Mutex;

    status_t onTransactWrapper(TransactionCode code, const Parcel& data, uint32_t flags,
            const std::function<status_t()>& delegate) final EXCLUDES_AudioFlinger_Mutex;

    // ---- end of IAudioFlinger interface

    // ---- begin IAfClientCallback interface

    audio_utils::mutex& clientMutex() const final
            RETURN_CAPABILITY(audio_utils::AudioFlinger_ClientMutex) {
        return mClientMutex;
    }
    void removeClient_l(pid_t pid) REQUIRES(clientMutex()) final;
    void removeNotificationClient(pid_t pid) final EXCLUDES_AudioFlinger_Mutex;
    status_t moveAuxEffectToIo(
            int effectId,
            const sp<IAfPlaybackThread>& dstThread,
            sp<IAfPlaybackThread>* srcThread) final EXCLUDES_AudioFlinger_Mutex;

    // ---- end of IAfClientCallback interface

    // ---- begin IAfDeviceEffectManagerCallback interface

    // also used by IAfThreadCallback
    bool isAudioPolicyReady() const final { return mAudioPolicyReady.load(); }
    // below also used by IAfMelReporterCallback, IAfPatchPanelCallback
    const sp<PatchCommandThread>& getPatchCommandThread() final { return mPatchCommandThread; }
    status_t addEffectToHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final
            EXCLUDES_AudioFlinger_HardwareMutex;
    status_t removeEffectFromHal(
            const struct audio_port_config* device, const sp<EffectHalInterface>& effect) final
            EXCLUDES_AudioFlinger_HardwareMutex;

    // ---- end of IAfDeviceEffectManagerCallback interface

    // ---- begin IAfMelReporterCallback interface

    // below also used by IAfThreadCallback
    audio_utils::mutex& mutex() const final
            RETURN_CAPABILITY(audio_utils::AudioFlinger_Mutex)
            EXCLUDES_BELOW_AudioFlinger_Mutex { return mMutex; }
    sp<IAfThreadBase> checkOutputThread_l(audio_io_handle_t ioHandle) const final
            REQUIRES(mutex());

    // ---- end of IAfMelReporterCallback interface

    // ---- begin IAfPatchPanelCallback interface

    void closeThreadInternal_l(const sp<IAfPlaybackThread>& thread) final REQUIRES(mutex());
    void closeThreadInternal_l(const sp<IAfRecordThread>& thread) final REQUIRES(mutex());
    // return thread associated with primary hardware device, or NULL
    IAfPlaybackThread* primaryPlaybackThread_l() const final  REQUIRES(mutex());
    IAfPlaybackThread* checkPlaybackThread_l(audio_io_handle_t output) const final
            REQUIRES(mutex());
    IAfRecordThread* checkRecordThread_l(audio_io_handle_t input) const final  REQUIRES(mutex());
    IAfMmapThread* checkMmapThread_l(audio_io_handle_t io) const final REQUIRES(mutex());
    sp<IAfThreadBase> openInput_l(audio_module_handle_t module,
            audio_io_handle_t* input,
            audio_config_t* config,
            audio_devices_t device,
            const char* address,
            audio_source_t source,
            audio_input_flags_t flags,
            audio_devices_t outputDevice,
            const String8& outputDeviceAddress) final REQUIRES(mutex());
    sp<IAfThreadBase> openOutput_l(audio_module_handle_t module,
            audio_io_handle_t* output,
            audio_config_t* halConfig,
            audio_config_base_t* mixerConfig,
            audio_devices_t deviceType,
            const String8& address,
            audio_output_flags_t* flags,
            audio_attributes_t attributes) final REQUIRES(mutex());
    const DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*>&
            getAudioHwDevs_l() const final REQUIRES(mutex(), hardwareMutex()) {
              return mAudioHwDevs;
            }
    void updateDownStreamPatches_l(const struct audio_patch* patch,
            const std::set<audio_io_handle_t>& streams) final REQUIRES(mutex());
    void updateOutDevicesForRecordThreads_l(const DeviceDescriptorBaseVector& devices) final
            REQUIRES(mutex());

    // ---- end of IAfPatchPanelCallback interface

    // ----- begin IAfThreadCallback interface

    bool isNonOffloadableGlobalEffectEnabled_l() const final
            REQUIRES(mutex()) EXCLUDES_ThreadBase_Mutex;
    bool btNrecIsOff() const final { return mBtNrecIsOff.load(); }
    float masterVolume_l() const final REQUIRES(mutex());
    bool masterMute_l() const final REQUIRES(mutex());
    float getMasterBalance_l() const REQUIRES(mutex());
    // no range check, AudioFlinger::mutex() held
    bool streamMute_l(audio_stream_type_t stream) const final REQUIRES(mutex()) {
        return mStreamTypes[stream].mute;
    }
    audio_mode_t getMode() const final { return mMode; }
    bool isLowRamDevice() const final { return mIsLowRamDevice; }
    uint32_t getScreenState() const final { return mScreenState; }

    std::optional<media::AudioVibratorInfo> getDefaultVibratorInfo_l() const final
            REQUIRES(mutex());
    const sp<IAfPatchPanel>& getPatchPanel() const final { return mPatchPanel; }
    const sp<MelReporter>& getMelReporter() const final { return mMelReporter; }
    const sp<EffectsFactoryHalInterface>& getEffectsFactoryHal() const final {
        return mEffectsFactoryHal;
    }
    sp<IAudioManager> getOrCreateAudioManager() final;
    sp<media::IAudioManagerNative> getAudioManagerNative() const final;

    // Called when the last effect handle on an effect instance is removed. If this
    // effect belongs to an effect chain in mOrphanEffectChains, the chain is updated
    // and removed from mOrphanEffectChains if it does not contain any effect.
    // Return true if the effect was found in mOrphanEffectChains, false otherwise.
    bool updateOrphanEffectChains(const sp<IAfEffectModule>& effect) final
            EXCLUDES_AudioFlinger_Mutex;

    status_t moveEffectChain_ll(audio_session_t sessionId,
            IAfPlaybackThread* srcThread, IAfPlaybackThread* dstThread,
            IAfEffectChain* srcChain = nullptr) final
            REQUIRES(mutex(), audio_utils::ThreadBase_Mutex);

    sp<audioflinger::SyncEvent> createSyncEvent(AudioSystem::sync_event_t type,
            audio_session_t triggerSession,
            audio_session_t listenerSession,
            const audioflinger::SyncEventCallback& callBack,
            const wp<IAfTrackBase>& cookie) final EXCLUDES_AudioFlinger_Mutex;

    // Hold either AudioFlinger::mutex or ThreadBase::mutex
    void ioConfigChanged_l(audio_io_config_event_t event,
            const sp<AudioIoDescriptor>& ioDesc,
            pid_t pid = 0) final EXCLUDES_AudioFlinger_ClientMutex;
    void onNonOffloadableGlobalEffectEnable() final EXCLUDES_AudioFlinger_Mutex;
    void onSupportedLatencyModesChanged(
            audio_io_handle_t output, const std::vector<audio_latency_mode_t>& modes) final
            EXCLUDES_AudioFlinger_ClientMutex;
    void onHardError(std::set<audio_port_handle_t>& trackPortIds) final
            EXCLUDES_AudioFlinger_ClientMutex;

    const ::com::android::media::permission::IPermissionProvider& getPermissionProvider() final;

    bool isHardeningOverrideEnabled() const final;

    bool hasAlreadyCaptured(uid_t uid) const final {
        const std::lock_guard _l(mCapturingClientsMutex);
        return mCapturingClients.contains(uid);
    }

    // ---- end of IAfThreadCallback interface

    void setHasAlreadyCaptured_l(uid_t uid) REQUIRES(mutex());

    /* List available audio ports and their attributes */
    status_t listAudioPorts(unsigned int* num_ports, struct audio_port* ports) const
            EXCLUDES_AudioFlinger_Mutex;

    sp<EffectsFactoryHalInterface> getEffectsFactory();

    int64_t getStartupFinishedTime() {
        return mStartupFinishedTime.load(std::memory_order_acquire);
    }

public:
    // TODO(b/292281786): Remove this when Oboeservice can get access to
    // openMmapStream through an IAudioFlinger handle directly.
    static inline std::atomic<AudioFlinger*> gAudioFlinger = nullptr;

    status_t openMmapStream(MmapStreamInterface::stream_direction_t direction,
                            const audio_attributes_t *attr,
                            audio_config_base_t *config,
                            const AudioClient& client,
                            DeviceIdVector *deviceIds,
                            audio_session_t *sessionId,
                            const sp<MmapStreamCallback>& callback,
                            sp<MmapStreamInterface>& interface,
            audio_port_handle_t *handle) EXCLUDES_AudioFlinger_Mutex;

    void initAudioPolicyLocal(sp<media::IAudioPolicyServiceLocal> audioPolicyLocal) {
        if (mAudioPolicyServiceLocal.load() == nullptr) {
            mAudioPolicyServiceLocal = std::move(audioPolicyLocal);
        }
    }

private:
    // FIXME The 400 is temporarily too high until a leak of writers in media.log is fixed.
    static const size_t kLogMemorySize = 400 * 1024;
    sp<MemoryDealer>    mLogMemoryDealer;   // == 0 when NBLog is disabled
    // When a log writer is unregistered, it is done lazily so that media.log can continue to see it
    // for as long as possible.  The memory is only freed when it is needed for another log writer.
    Vector< sp<NBLog::Writer> > mUnregisteredWriters;
    audio_utils::mutex& unregisteredWritersMutex() const { return mUnregisteredWritersMutex; }
    mutable audio_utils::mutex mUnregisteredWritersMutex{
            audio_utils::MutexOrder::kAudioFlinger_UnregisteredWritersMutex};

                            AudioFlinger() ANDROID_API;
    ~AudioFlinger() override;

    // call in any IAudioFlinger method that accesses mPrimaryHardwareDev
    status_t initCheck() const { return mPrimaryHardwareDev == NULL ?
                                                        NO_INIT : NO_ERROR; }

    // RefBase
    void onFirstRef() override;

    AudioHwDevice*          findSuitableHwDev_l(audio_module_handle_t module,
            audio_devices_t deviceType) REQUIRES(mutex());

    error::BinderResult<std::monostate> enforceCallingPermission(
                    com::android::media::permission::PermissionEnum perm);

    // incremented by 2 when screen state changes, bit 0 == 1 means "off"
    // AudioFlinger::setParameters() updates with mutex().
    std::atomic_uint32_t mScreenState{};

    void dumpPermissionDenial(int fd);
    void dumpClients_ll(int fd, bool dumpAllocators) REQUIRES(mutex(), clientMutex());
    void dumpInternals_l(int fd) REQUIRES(mutex());
    void dumpStats(int fd);

    SimpleLog mThreadLog{16}; // 16 Thread history limit

    void dumpToThreadLog_l(const sp<IAfThreadBase>& thread) REQUIRES(mutex());

    // --- Notification Client ---
    class NotificationClient : public IBinder::DeathRecipient {
    public:
                            NotificationClient(const sp<AudioFlinger>& audioFlinger,
                                                const sp<media::IAudioFlingerClient>& client,
                                                pid_t pid,
                                                uid_t uid);
        virtual             ~NotificationClient();

                sp<media::IAudioFlingerClient> audioFlingerClient() const { return mAudioFlingerClient; }
                pid_t getPid() const { return mPid; }
                uid_t getUid() const { return mUid; }

                // IBinder::DeathRecipient
                virtual     void        binderDied(const wp<IBinder>& who);

    private:
        DISALLOW_COPY_AND_ASSIGN(NotificationClient);

        const sp<AudioFlinger>  mAudioFlinger;
        const pid_t             mPid;
        const uid_t             mUid;
        const sp<media::IAudioFlingerClient> mAudioFlingerClient;
        const std::unique_ptr<media::psh_utils::Token> mClientToken;
    };

    // Find io handle by session id.
    // Preference is given to an io handle with a matching effect chain to session id.
    // If none found, AUDIO_IO_HANDLE_NONE is returned.
    template <typename T>
    static audio_io_handle_t findIoHandleBySessionId_l(
            audio_session_t sessionId, const T& threads)
            REQUIRES(audio_utils::AudioFlinger_Mutex) {
        audio_io_handle_t io = AUDIO_IO_HANDLE_NONE;

        for (size_t i = 0; i < threads.size(); i++) {
            const uint32_t sessionType = threads.valueAt(i)->hasAudioSession(sessionId);
            if (sessionType != 0) {
                io = threads.keyAt(i);
                if ((sessionType & IAfThreadBase::EFFECT_SESSION) != 0) {
                    break; // effect chain here.
                }
            }
        }
        return io;
    }

    IAfThreadBase* checkThread_l(audio_io_handle_t ioHandle) const REQUIRES(mutex());
    IAfPlaybackThread* checkMixerThread_l(audio_io_handle_t output) const REQUIRES(mutex());

    sp<VolumeInterface> getVolumeInterface_l(audio_io_handle_t output) const REQUIRES(mutex());

    std::vector<sp<VolumeInterface>> getAllVolumeInterfaces_l() const REQUIRES(mutex());


    static void closeOutputFinish(const sp<IAfPlaybackThread>& thread);
    void closeInputFinish(const sp<IAfRecordThread>& thread);

              // Allocate an audio_unique_id_t.
              // Specific types are audio_io_handle_t, audio_session_t, effect ID (int),
              // audio_module_handle_t, and audio_patch_handle_t.
              // They all share the same ID space, but the namespaces are actually independent
              // because there are separate KeyedVectors for each kind of ID.
              // The return value is cast to the specific type depending on how the ID will be used.
              // FIXME This API does not handle rollover to zero (for unsigned IDs),
              //       or from positive to negative (for signed IDs).
              //       Thus it may fail by returning an ID of the wrong sign,
              //       or by returning a non-unique ID.
              // This is the internal API.  For the binder API see newAudioUniqueId().
    // used by IAfDeviceEffectManagerCallback, IAfPatchPanelCallback, IAfThreadCallback
    audio_unique_id_t nextUniqueId(audio_unique_id_use_t use) final;

    status_t moveEffectChain_ll(audio_session_t sessionId,
            IAfRecordThread* srcThread, IAfRecordThread* dstThread)
            REQUIRES(mutex(), audio_utils::ThreadBase_Mutex);

              // return thread associated with primary hardware device, or NULL
    DeviceTypeSet primaryOutputDevice_l() const REQUIRES(mutex());

              // return the playback thread with smallest HAL buffer size, and prefer fast
    IAfPlaybackThread* fastPlaybackThread_l() const REQUIRES(mutex());

    sp<IAfThreadBase> getEffectThread_l(audio_session_t sessionId, int effectId)
            REQUIRES(mutex());

    IAfThreadBase* hapticPlaybackThread_l() const REQUIRES(mutex());

              void updateSecondaryOutputsForTrack_l(
                      IAfTrack* track,
                      IAfPlaybackThread* thread,
            const std::vector<audio_io_handle_t>& secondaryOutputs) const REQUIRES(mutex());

    bool isSessionAcquired_l(audio_session_t audioSession) REQUIRES(mutex());

                // Store an effect chain to mOrphanEffectChains keyed vector.
                // Called when a thread exits and effects are still attached to it.
                // If effects are later created on the same session, they will reuse the same
                // effect chain and same instances in the effect library.
                // return ALREADY_EXISTS if a chain with the same session already exists in
                // mOrphanEffectChains. Note that this should never happen as there is only one
                // chain for a given session and it is attached to only one thread at a time.
    status_t putOrphanEffectChain_l(const sp<IAfEffectChain>& chain) REQUIRES(mutex());
                // Get an effect chain for the specified session in mOrphanEffectChains and remove
                // it if found. Returns 0 if not found (this is the most common case).
    sp<IAfEffectChain> getOrphanEffectChain_l(audio_session_t session) REQUIRES(mutex());

    std::vector< sp<IAfEffectModule> > purgeStaleEffects_l() REQUIRES(mutex());

    std::vector< sp<IAfEffectModule> > purgeOrphanEffectChains_l() REQUIRES(mutex());
    bool updateOrphanEffectChains_l(const sp<IAfEffectModule>& effect) REQUIRES(mutex());

    void broadcastParametersToRecordThreads_l(const String8& keyValuePairs) REQUIRES(mutex());
    void forwardParametersToDownstreamPatches_l(
                        audio_io_handle_t upStream, const String8& keyValuePairs,
            const std::function<bool(const sp<IAfPlaybackThread>&)>& useThread = nullptr)
            REQUIRES(mutex());

    // for mAudioSessionRefs only
    struct AudioSessionRef {
        AudioSessionRef(audio_session_t sessionid, pid_t pid, uid_t uid) :
            mSessionid(sessionid), mPid(pid), mUid(uid), mCnt(1) {}
        const audio_session_t mSessionid;
        const pid_t mPid;
        const uid_t mUid;
        int         mCnt;
    };

    mutable audio_utils::mutex mMutex{audio_utils::MutexOrder::kAudioFlinger_Mutex};
                // protects mClients and mNotificationClients.
                // must be locked after mutex() and ThreadBase::mutex() if both must be locked
                // avoids acquiring AudioFlinger::mutex() from inside thread loop.

    mutable audio_utils::mutex mClientMutex{audio_utils::MutexOrder::kAudioFlinger_ClientMutex};

    DefaultKeyedVector<pid_t, wp<Client>> mClients GUARDED_BY(clientMutex());   // see ~Client()

    audio_utils::mutex& hardwareMutex() const { return mHardwareMutex; }

    mutable audio_utils::mutex mHardwareMutex{
            audio_utils::MutexOrder::kAudioFlinger_HardwareMutex};
    // NOTE: If both mMutex and mHardwareMutex mutexes must be held,
    // always take mMutex before mHardwareMutex

    std::atomic<AudioHwDevice*> mPrimaryHardwareDev = nullptr;
    DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*> mAudioHwDevs
            GUARDED_BY(hardwareMutex()) {nullptr /* defValue */};

    static bool inputBufferSizeDevsCmp(const AudioHwDevice* lhs, const AudioHwDevice* rhs);
    std::set<AudioHwDevice*, decltype(&inputBufferSizeDevsCmp)>
            mInputBufferSizeOrderedDevs GUARDED_BY(hardwareMutex()) {inputBufferSizeDevsCmp};

     const sp<DevicesFactoryHalInterface> mDevicesFactoryHal =
             DevicesFactoryHalInterface::create();
     /* const */ sp<DevicesFactoryHalCallback> mDevicesFactoryHalCallback;  // set onFirstRef().

    // for dump, indicates which hardware operation is currently in progress (but not stream ops)
    enum hardware_call_state {
        AUDIO_HW_IDLE = 0,              // no operation in progress
        AUDIO_HW_INIT,                  // init_check
        AUDIO_HW_OUTPUT_OPEN,           // open_output_stream
        AUDIO_HW_OUTPUT_CLOSE,          // unused
        AUDIO_HW_INPUT_OPEN,            // unused
        AUDIO_HW_INPUT_CLOSE,           // unused
        AUDIO_HW_STANDBY,               // unused
        AUDIO_HW_SET_MASTER_VOLUME,     // set_master_volume
        AUDIO_HW_GET_ROUTING,           // unused
        AUDIO_HW_SET_ROUTING,           // unused
        AUDIO_HW_GET_MODE,              // unused
        AUDIO_HW_SET_MODE,              // set_mode
        AUDIO_HW_GET_MIC_MUTE,          // get_mic_mute
        AUDIO_HW_SET_MIC_MUTE,          // set_mic_mute
        AUDIO_HW_SET_VOICE_VOLUME,      // set_voice_volume
        AUDIO_HW_SET_PARAMETER,         // set_parameters
        AUDIO_HW_GET_INPUT_BUFFER_SIZE, // get_input_buffer_size
        AUDIO_HW_GET_MASTER_VOLUME,     // get_master_volume
        AUDIO_HW_GET_PARAMETER,         // get_parameters
        AUDIO_HW_SET_MASTER_MUTE,       // set_master_mute
        AUDIO_HW_GET_MASTER_MUTE,       // get_master_mute
        AUDIO_HW_GET_MICROPHONES,       // getMicrophones
        AUDIO_HW_SET_CONNECTED_STATE,   // setConnectedState
        AUDIO_HW_SET_SIMULATE_CONNECTIONS, // setSimulateDeviceConnections
    };

    mutable hardware_call_state mHardwareStatus = AUDIO_HW_IDLE;  // for dump only
    DefaultKeyedVector<audio_io_handle_t, sp<IAfPlaybackThread>> mPlaybackThreads
            GUARDED_BY(mutex());
    stream_type_t mStreamTypes[AUDIO_STREAM_CNT] GUARDED_BY(mutex());

    float mMasterVolume GUARDED_BY(mutex()) = 1.f;
    bool mMasterMute GUARDED_BY(mutex()) = false;
    float mMasterBalance GUARDED_BY(mutex()) = 0.f;

    DefaultKeyedVector<audio_io_handle_t, sp<IAfRecordThread>> mRecordThreads GUARDED_BY(mutex());

    std::map<pid_t, sp<NotificationClient>> mNotificationClients GUARDED_BY(clientMutex());

                // updated by atomic_fetch_add_explicit
    volatile atomic_uint_fast32_t mNextUniqueIds[AUDIO_UNIQUE_ID_USE_MAX];  // ctor init

    std::atomic<audio_mode_t> mMode = AUDIO_MODE_INVALID;
    std::atomic<bool> mBtNrecIsOff = false;

    Vector<AudioSessionRef*> mAudioSessionRefs GUARDED_BY(mutex());

    AudioHwDevice* loadHwModule_ll(const char *name) REQUIRES(mutex(), hardwareMutex());

                // sync events awaiting for a session to be created.
    std::list<sp<audioflinger::SyncEvent>> mPendingSyncEvents GUARDED_BY(mutex());

                // Effect chains without a valid thread
    DefaultKeyedVector<audio_session_t, sp<IAfEffectChain>> mOrphanEffectChains
            GUARDED_BY(mutex());

                // list of sessions for which a valid HW A/V sync ID was retrieved from the HAL
    DefaultKeyedVector<audio_session_t, audio_hw_sync_t> mHwAvSyncIds GUARDED_BY(mutex());

                // list of MMAP stream control threads. Those threads allow for wake lock, routing
                // and volume control for activity on the associated MMAP stream at the HAL.
                // Audio data transfer is directly handled by the client creating the MMAP stream
    DefaultKeyedVector<audio_io_handle_t, sp<IAfMmapThread>> mMmapThreads GUARDED_BY(mutex());

    // always returns non-null
    sp<Client> registerClient(pid_t pid, uid_t uid) EXCLUDES_AudioFlinger_ClientMutex;

    sp<IAfEffectHandle> createOrphanEffect_l(const sp<Client>& client,
                                          const sp<media::IEffectClient>& effectClient,
                                          int32_t priority,
                                          audio_session_t sessionId,
                                          effect_descriptor_t *desc,
                                          int *enabled,
                                          status_t *status /*non-NULL*/,
                                          bool pinned,
                                          bool notifyFramesProcessed) REQUIRES(mutex());

    // for use from destructor
    status_t closeOutput_nonvirtual(audio_io_handle_t output) EXCLUDES_AudioFlinger_Mutex;
    status_t closeInput_nonvirtual(audio_io_handle_t input) EXCLUDES_AudioFlinger_Mutex;
    void setAudioHwSyncForSession_l(IAfPlaybackThread* thread, audio_session_t sessionId)
            REQUIRES(mutex());

    static status_t checkStreamType(audio_stream_type_t stream);

    // no mutex needed.
    void        filterReservedParameters(String8& keyValuePairs, uid_t callingUid);
    void        logFilteredParameters(size_t originalKVPSize, const String8& originalKVPs,
                                      size_t rejectedKVPSize, const String8& rejectedKVPs,
                                      uid_t callingUid);

    // These methods read variables atomically without mLock,
    // though the variables are updated with mLock.
    size_t getClientSharedHeapSize() const;

    std::atomic<bool> mIsLowRamDevice = true;
    bool mIsDeviceTypeKnown GUARDED_BY(mutex()) = false;
    int64_t mTotalMemory GUARDED_BY(mutex()) = 0;
    std::atomic<size_t> mClientSharedHeapSize = kMinimumClientSharedHeapSizeBytes;
    static constexpr size_t kMinimumClientSharedHeapSizeBytes = 1024 * 1024; // 1MB

    /* const */ sp<IAfPatchPanel> mPatchPanel;

    const sp<EffectsFactoryHalInterface> mEffectsFactoryHal =
            audioflinger::EffectConfiguration::getEffectsFactoryHal();

    const sp<PatchCommandThread> mPatchCommandThread = sp<PatchCommandThread>::make();
    /* const */ sp<DeviceEffectManager> mDeviceEffectManager;  // set onFirstRef
    /* const */ sp<MelReporter> mMelReporter;  // set onFirstRef

    bool mSystemReady GUARDED_BY(mutex()) = false;
    std::atomic<bool> mAudioPolicyReady = false;

    // no mutex needed.
    SimpleLog  mRejectedSetParameterLog;
    SimpleLog  mAppSetParameterLog;
    SimpleLog  mSystemSetParameterLog;

    std::vector<media::AudioVibratorInfo> mAudioVibratorInfos GUARDED_BY(mutex());

    static inline constexpr const char *mMetricsId = AMEDIAMETRICS_KEY_AUDIO_FLINGER;

    std::map<media::audio::common::AudioMMapPolicyType,
             std::vector<media::audio::common::AudioMMapPolicyInfo>> mPolicyInfos
             GUARDED_BY(mutex());
    int32_t mAAudioBurstsPerBuffer GUARDED_BY(mutex()) = 0;
    int32_t mAAudioHwBurstMinMicros GUARDED_BY(mutex()) = 0;

    /** Interfaces for interacting with the AudioService. */
    mediautils::atomic_sp<IAudioManager> mAudioManager;
    mediautils::atomic_sp<media::IAudioManagerNative> mAudioManagerNative;

    // Bluetooth Variable latency control logic is enabled or disabled
    std::atomic<bool> mBluetoothLatencyModesEnabled = true;

    // Local interface to AudioPolicyService, late inited, but logically const
    mediautils::atomic_sp<media::IAudioPolicyServiceLocal> mAudioPolicyServiceLocal;

    const int64_t mStartTime = audio_utils_get_real_time_ns();
    // Late-inited from main()
    std::atomic<int64_t> mStartupFinishedTime {};

    // List of client UIDs having already captured audio in the past.
    // This is used to control GMAP bidirectional mode track metadata tag
    // generation.
    std::set<uid_t> mCapturingClients GUARDED_BY(mCapturingClientsMutex);
    mutable std::mutex  mCapturingClientsMutex; // only for mCapturingClients
};

// ----------------------------------------------------------------------------

} // namespace android
