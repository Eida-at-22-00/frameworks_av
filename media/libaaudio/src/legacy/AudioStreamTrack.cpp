/*
 * Copyright 2016 The Android Open Source Project
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

#define LOG_TAG "AudioStreamTrack"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <stdint.h>
#include <media/AudioTrack.h>

#include <aaudio/AAudio.h>
#include <com_android_media_aaudio.h>
#include <system/audio.h>
#include <system/aaudio/AAudio.h>

#include "core/AudioGlobal.h"
#include "legacy/AudioStreamLegacy.h"
#include "legacy/AudioStreamTrack.h"
#include "utility/AudioClock.h"
#include "utility/FixedBlockReader.h"

using namespace android;
using namespace aaudio;

using android::content::AttributionSourceState;

// Arbitrary and somewhat generous number of bursts.
#define DEFAULT_BURSTS_PER_BUFFER_CAPACITY     8

/*
 * Create a stream that uses the AudioTrack.
 */
AudioStreamTrack::AudioStreamTrack()
    : AudioStreamLegacy()
    , mFixedBlockReader(*this)
{
}

AudioStreamTrack::~AudioStreamTrack()
{
    const aaudio_stream_state_t state = getState();
    bool bad = !(state == AAUDIO_STREAM_STATE_UNINITIALIZED || state == AAUDIO_STREAM_STATE_CLOSED);
    ALOGE_IF(bad, "stream not closed, in state %d", state);
}

aaudio_result_t AudioStreamTrack::open(const AudioStreamBuilder& builder)
{
    if (!com::android::media::aaudio::offload_support() &&
        builder.getPerformanceMode() == AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED) {
        return AAUDIO_ERROR_UNIMPLEMENTED;
    }
    aaudio_result_t result = AAUDIO_OK;

    result = AudioStream::open(builder);
    if (result != OK) {
        return result;
    }

    const aaudio_session_id_t requestedSessionId = builder.getSessionId();
    const audio_session_t sessionId = AAudioConvert_aaudioToAndroidSessionId(requestedSessionId);

    audio_channel_mask_t channelMask =
            AAudio_getChannelMaskForOpen(getChannelMask(), getSamplesPerFrame(), false /*isInput*/);

    // Set flags based on selected parameters.
    audio_output_flags_t flags;
    aaudio_performance_mode_t perfMode = getPerformanceMode();
    switch(perfMode) {
        case AAUDIO_PERFORMANCE_MODE_LOW_LATENCY: {
            // Bypass the normal mixer and go straight to the FAST mixer.
            // Some Usages need RAW mode so they can get the lowest possible latency.
            // Other Usages should avoid RAW because it can interfere with
            // dual sink routing or other features.
            bool usageBenefitsFromRaw = getUsage() == AAUDIO_USAGE_GAME ||
                    getUsage() == AAUDIO_USAGE_MEDIA;
            // If an app does not ask for a sessionId then there will be no effects.
            // So we can use the use RAW flag.
            flags = (audio_output_flags_t) (((requestedSessionId == AAUDIO_SESSION_ID_NONE)
                                             && usageBenefitsFromRaw)
                                            ? (AUDIO_OUTPUT_FLAG_FAST | AUDIO_OUTPUT_FLAG_RAW)
                                            : (AUDIO_OUTPUT_FLAG_FAST));
        }
            break;

        case AAUDIO_PERFORMANCE_MODE_POWER_SAVING:
            // This uses a mixer that wakes up less often than the FAST mixer.
            flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
            break;

        case AAUDIO_PERFORMANCE_MODE_NONE:
        default:
            // No flags. Use a normal mixer in front of the FAST mixer.
            flags = AUDIO_OUTPUT_FLAG_NONE;
            break;
    }

    size_t frameCount = (size_t)builder.getBufferCapacity();

    // To avoid glitching, let AudioFlinger pick the optimal burst size.
    int32_t notificationFrames = 0;

    const audio_format_t format = (getFormat() == AUDIO_FORMAT_DEFAULT)
            ? AUDIO_FORMAT_PCM_FLOAT
            : getFormat();

    // Setup the callback if there is one.
    wp<AudioTrack::IAudioTrackCallback> callback;
    // Note that TRANSFER_SYNC does not allow FAST track
    AudioTrack::transfer_type streamTransferType = AudioTrack::transfer_type::TRANSFER_SYNC;
    if (builder.getDataCallbackProc() != nullptr) {
        streamTransferType = AudioTrack::transfer_type::TRANSFER_CALLBACK;
        callback = wp<AudioTrack::IAudioTrackCallback>::fromExisting(this);

        // If the total buffer size is unspecified then base the size on the burst size.
        if (frameCount == 0
                && ((flags & AUDIO_OUTPUT_FLAG_FAST) != 0)) {
            // Take advantage of a special trick that allows us to create a buffer
            // that is some multiple of the burst size.
            notificationFrames = 0 - DEFAULT_BURSTS_PER_BUFFER_CAPACITY;
        }
    } else if (getPerformanceMode() == AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED) {
        streamTransferType = AudioTrack::transfer_type::TRANSFER_SYNC_NOTIF_CALLBACK;
        callback = wp<AudioTrack::IAudioTrackCallback>::fromExisting(this);
    }
    mCallbackBufferSize = builder.getFramesPerDataCallback();

    ALOGD("open(), request notificationFrames = %d, frameCount = %u",
          notificationFrames, (uint)frameCount);

    // Don't call mAudioTrack->setDeviceId() because it will be overwritten by set()!
    audio_port_handle_t selectedDeviceId = getFirstDeviceId(getDeviceIds());

    const audio_content_type_t contentType =
            AAudioConvert_contentTypeToInternal(builder.getContentType());
    const audio_usage_t usage =
            AAudioConvert_usageToInternal(builder.getUsage());
    const audio_flags_mask_t attributesFlags = AAudio_computeAudioFlagsMask(
                                                            builder.getAllowedCapturePolicy(),
                                                            builder.getSpatializationBehavior(),
                                                            builder.isContentSpatialized(),
                                                            flags);

    const std::string tags = getTagsAsString();
    audio_attributes_t attributes = AUDIO_ATTRIBUTES_INITIALIZER;
    attributes.content_type = contentType;
    attributes.usage = usage;
    attributes.flags = attributesFlags;
    if (!tags.empty()) {
        strncpy(attributes.tags, tags.c_str(), AUDIO_ATTRIBUTES_TAGS_MAX_SIZE);
        attributes.tags[AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - 1] = '\0';
    }

    audio_offload_info_t offloadInfo = AUDIO_INFO_INITIALIZER;
    if (getPerformanceMode() == AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED) {
        audio_config_t config = AUDIO_CONFIG_INITIALIZER;
        config.format = format;
        config.channel_mask = channelMask;
        config.sample_rate = getSampleRate();
        audio_direct_mode_t directMode = AUDIO_DIRECT_NOT_SUPPORTED;
        if (status_t status = AudioSystem::getDirectPlaybackSupport(
                &attributes, &config, &directMode);
            status != NO_ERROR) {
            ALOGE("%s, failed to query direct support, error=%d", __func__, status);
            return status;
        }
        static const audio_direct_mode_t offloadMode = static_cast<audio_direct_mode_t>(
                AUDIO_DIRECT_OFFLOAD_SUPPORTED | AUDIO_DIRECT_OFFLOAD_GAPLESS_SUPPORTED);
        if ((directMode & offloadMode) == AUDIO_DIRECT_NOT_SUPPORTED) {
            return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
        }
        flags = AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD;
        frameCount = 0;
        offloadInfo.format = format;
        offloadInfo.sample_rate = getSampleRate();
        offloadInfo.channel_mask = channelMask;
        offloadInfo.has_video = false;
        offloadInfo.stream_type = AUDIO_STREAM_MUSIC;
    }

    mAudioTrack = new AudioTrack();
    // TODO b/182392769: use attribution source util
    mAudioTrack->set(
            AUDIO_STREAM_DEFAULT,  // ignored because we pass attributes below
            getSampleRate(),
            format,
            channelMask,
            frameCount,
            flags,
            callback,
            notificationFrames,
            nullptr,       // DEFAULT sharedBuffer*/,
            false,   // DEFAULT threadCanCallJava
            sessionId,
            streamTransferType,
            getPerformanceMode() == AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED
                    ? &offloadInfo : nullptr,
            AttributionSourceState(), // DEFAULT uid and pid
            &attributes,
            // WARNING - If doNotReconnect set true then audio stops after plugging and unplugging
            // headphones a few times.
            false,   // DEFAULT doNotReconnect,
            1.0f,    // DEFAULT maxRequiredSpeed
            selectedDeviceId
    );

    // Set it here so it can be logged by the destructor if the open failed.
    mAudioTrack->setCallerName(kCallerName);

    // Did we get a valid track?
    status_t status = mAudioTrack->initCheck();
    if (status != NO_ERROR) {
        safeReleaseClose();
        ALOGE("open(), initCheck() returned %d", status);
        return AAudioConvert_androidToAAudioResult(status);
    }

    mMetricsId = std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_TRACK)
            + std::to_string(mAudioTrack->getPortId());
    android::mediametrics::LogItem(mMetricsId)
            .set(AMEDIAMETRICS_PROP_PERFORMANCEMODE,
                 AudioGlobal_convertPerformanceModeToText(builder.getPerformanceMode()))
            .set(AMEDIAMETRICS_PROP_SHARINGMODE,
                 AudioGlobal_convertSharingModeToText(builder.getSharingMode()))
            .set(AMEDIAMETRICS_PROP_ENCODINGCLIENT,
                 android::toString(getFormat()).c_str()).record();

    doSetVolume();

    // Get the actual values from the AudioTrack.
    setChannelMask(AAudioConvert_androidToAAudioChannelMask(
        mAudioTrack->channelMask(), false /*isInput*/,
        AAudio_isChannelIndexMask(getChannelMask())));
    setFormat(mAudioTrack->format());
    setDeviceFormat(mAudioTrack->format());
    setSampleRate(mAudioTrack->getSampleRate());
    setBufferCapacity(getBufferCapacityFromDevice());
    setFramesPerBurst(getFramesPerBurstFromDevice());

    // Use the same values for device values.
    setDeviceSamplesPerFrame(getSamplesPerFrame());
    setDeviceSampleRate(mAudioTrack->getSampleRate());
    setDeviceBufferCapacity(getBufferCapacityFromDevice());
    setDeviceFramesPerBurst(getFramesPerBurstFromDevice());

    setHardwareSamplesPerFrame(mAudioTrack->getHalChannelCount());
    setHardwareSampleRate(mAudioTrack->getHalSampleRate());
    setHardwareFormat(mAudioTrack->getHalFormat());

    // We may need to pass the data through a block size adapter to guarantee constant size.
    if (mCallbackBufferSize != AAUDIO_UNSPECIFIED) {
        // This may need to change if we add format conversion before
        // the block size adaptation.
        mBlockAdapterBytesPerFrame = getBytesPerFrame();
        int callbackSizeBytes = mBlockAdapterBytesPerFrame * mCallbackBufferSize;
        mFixedBlockReader.open(callbackSizeBytes);
        mBlockAdapter = &mFixedBlockReader;
    } else {
        mBlockAdapter = nullptr;
    }

    setDeviceIds(mAudioTrack->getRoutedDeviceIds());

    aaudio_session_id_t actualSessionId =
            (requestedSessionId == AAUDIO_SESSION_ID_NONE)
            ? AAUDIO_SESSION_ID_NONE
            : (aaudio_session_id_t) mAudioTrack->getSessionId();
    setSessionId(actualSessionId);

    mAudioTrack->addAudioDeviceCallback(this);

    // Update performance mode based on the actual stream flags.
    // For example, if the sample rate is not allowed then you won't get a FAST track.
    audio_output_flags_t actualFlags = mAudioTrack->getFlags();
    aaudio_performance_mode_t actualPerformanceMode = AAUDIO_PERFORMANCE_MODE_NONE;
    // We may not get the RAW flag. But as long as we get the FAST flag we can call it LOW_LATENCY.
    if ((actualFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != AUDIO_OUTPUT_FLAG_NONE) {
        actualPerformanceMode = AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED;
    } else if ((actualFlags & AUDIO_OUTPUT_FLAG_FAST) != 0) {
        actualPerformanceMode = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    } else if ((actualFlags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) != 0) {
        actualPerformanceMode = AAUDIO_PERFORMANCE_MODE_POWER_SAVING;
    }
    setPerformanceMode(actualPerformanceMode);

    setSharingMode(AAUDIO_SHARING_MODE_SHARED); // EXCLUSIVE mode not supported in legacy

    // Log if we did not get what we asked for.
    ALOGD_IF(actualFlags != flags,
             "open() flags changed from 0x%08X to 0x%08X",
             flags, actualFlags);
    ALOGD_IF(actualPerformanceMode != perfMode,
             "open() perfMode changed from %d to %d",
             perfMode, actualPerformanceMode);

    if (getState() != AAUDIO_STREAM_STATE_UNINITIALIZED) {
        ALOGE("%s - Open canceled since state = %d", __func__, getState());
        if (isDisconnected())
        {
            ALOGE("%s - Opening while state is disconnected", __func__);
            safeReleaseClose();
            return AAUDIO_ERROR_DISCONNECTED;
        }
        safeReleaseClose();
        return AAUDIO_ERROR_INVALID_STATE;
    }

    setState(AAUDIO_STREAM_STATE_OPEN);
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::release_l() {
    if (getState() != AAUDIO_STREAM_STATE_CLOSING) {
        status_t err = mAudioTrack->removeAudioDeviceCallback(this);
        ALOGE_IF(err, "%s() removeAudioDeviceCallback returned %d", __func__, err);
        logReleaseBufferState();
        // Data callbacks may still be running!
        return AudioStream::release_l();
    } else {
        return AAUDIO_OK; // already released
    }
}

void AudioStreamTrack::close_l() {
    // The callbacks are normally joined in the AudioTrack destructor.
    // But if another object has a reference to the AudioTrack then
    // it will not get deleted here.
    // So we should join callbacks explicitly before returning.
    // Unlock around the join to avoid deadlocks if the callback tries to lock.
    // This can happen if the callback returns AAUDIO_CALLBACK_RESULT_STOP
    mStreamLock.unlock();
    mAudioTrack->stopAndJoinCallbacks();
    mStreamLock.lock();
    mAudioTrack.clear();
    // Do not close mFixedBlockReader. It has a unique_ptr to its buffer
    // so it will clean up by itself.
    AudioStream::close_l();
}


void AudioStreamTrack::onNewIAudioTrack() {
    // Stream got rerouted so we disconnect.
    // request stream disconnect if the restored AudioTrack has properties not matching
    // what was requested initially
    if (mAudioTrack->channelCount() != getSamplesPerFrame()
          || mAudioTrack->format() != getFormat()
          || mAudioTrack->getSampleRate() != getSampleRate()
          || !areDeviceIdsEqual(mAudioTrack->getRoutedDeviceIds(), getDeviceIds())
          || getBufferCapacityFromDevice() != getBufferCapacity()
          || getFramesPerBurstFromDevice() != getFramesPerBurst()) {
        AudioStreamLegacy::onNewIAudioTrack();
    }
}

aaudio_result_t AudioStreamTrack::requestStart_l() {
    if (mAudioTrack.get() == nullptr) {
        ALOGE("requestStart() no AudioTrack");
        return AAUDIO_ERROR_INVALID_STATE;
    }
    // Get current position so we can detect when the track is playing.
    status_t err = mAudioTrack->getPosition(&mPositionWhenStarting);
    if (err != OK) {
        return AAudioConvert_androidToAAudioResult(err);
    }

    // Enable callback before starting AudioTrack to avoid shutting
    // down because of a race condition.
    mCallbackEnabled.store(true);
    aaudio_stream_state_t originalState = getState();
    // Set before starting the callback so that we are in the correct state
    // before updateStateMachine() can be called by the callback.
    setState(AAUDIO_STREAM_STATE_STARTING);
    err = mAudioTrack->start();
    if (err != OK) {
        mCallbackEnabled.store(false);
        setState(originalState);
        return AAudioConvert_androidToAAudioResult(err);
    }
    mOffloadEosPending = false;
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::requestPause_l() {
    if (mAudioTrack.get() == nullptr) {
        ALOGE("%s() no AudioTrack", __func__);
        return AAUDIO_ERROR_INVALID_STATE;
    }

    setState(AAUDIO_STREAM_STATE_PAUSING);
    mAudioTrack->pause();
    mCallbackEnabled.store(false);
    status_t err = mAudioTrack->getPosition(&mPositionWhenPausing);
    if (err != OK) {
        return AAudioConvert_androidToAAudioResult(err);
    }
    return checkForDisconnectRequest(false);
}

aaudio_result_t AudioStreamTrack::requestFlush_l() {
    if (mAudioTrack.get() == nullptr) {
        ALOGE("%s() no AudioTrack", __func__);
        return AAUDIO_ERROR_INVALID_STATE;
    }

    setState(AAUDIO_STREAM_STATE_FLUSHING);
    incrementFramesRead(getFramesWritten() - getFramesRead());
    mAudioTrack->flush();
    mFramesRead.reset32(); // service reads frames, service position reset on flush
    mTimestampPosition.reset32();
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::requestStop_l() {
    if (mAudioTrack.get() == nullptr) {
        ALOGE("%s() no AudioTrack", __func__);
        return AAUDIO_ERROR_INVALID_STATE;
    }

    setState(AAUDIO_STREAM_STATE_STOPPING);
    mFramesRead.catchUpTo(getFramesWritten());
    mTimestampPosition.catchUpTo(getFramesWritten());
    mFramesRead.reset32(); // service reads frames, service position reset on stop
    mTimestampPosition.reset32();
    mAudioTrack->stop();
    mCallbackEnabled.store(false);
    return checkForDisconnectRequest(false);;
}

aaudio_result_t AudioStreamTrack::processCommands() {
    status_t err;
    aaudio_wrapping_frames_t position;
    switch (getState()) {
    // TODO add better state visibility to AudioTrack
    case AAUDIO_STREAM_STATE_STARTING:
        if (mAudioTrack->hasStarted()) {
            setState(AAUDIO_STREAM_STATE_STARTED);
        }
        break;
    case AAUDIO_STREAM_STATE_PAUSING:
        if (mAudioTrack->stopped()) {
            err = mAudioTrack->getPosition(&position);
            if (err != OK) {
                return AAudioConvert_androidToAAudioResult(err);
            } else if (position == mPositionWhenPausing) {
                // Has stream really stopped advancing?
                setState(AAUDIO_STREAM_STATE_PAUSED);
            }
            mPositionWhenPausing = position;
        }
        break;
    case AAUDIO_STREAM_STATE_FLUSHING:
        {
            err = mAudioTrack->getPosition(&position);
            if (err != OK) {
                return AAudioConvert_androidToAAudioResult(err);
            } else if (position == 0) {
                setState(AAUDIO_STREAM_STATE_FLUSHED);
            }
        }
        break;
    case AAUDIO_STREAM_STATE_STOPPING:
        if (mAudioTrack->stopped()) {
            if (getPerformanceMode() == AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED) {
                // For offload mode, the state will be updated as `STOPPED` from
                // stream end callback.
                break;
            }
            setState(AAUDIO_STREAM_STATE_STOPPED);
        }
        break;
    default:
        break;
    }
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::write(const void *buffer,
                                      int32_t numFrames,
                                      int64_t timeoutNanoseconds)
{
    int32_t bytesPerFrame = getBytesPerFrame();
    int32_t numBytes;
    aaudio_result_t result = AAudioConvert_framesToBytes(numFrames, bytesPerFrame, &numBytes);
    if (result != AAUDIO_OK) {
        return result;
    }

    if (isDisconnected()) {
        return AAUDIO_ERROR_DISCONNECTED;
    }

    // TODO add timeout to AudioTrack
    bool blocking = timeoutNanoseconds > 0;
    ssize_t bytesWritten = mAudioTrack->write(buffer, numBytes, blocking);
    if (bytesWritten == WOULD_BLOCK) {
        return 0;
    } else if (bytesWritten < 0) {
        ALOGE("invalid write, returned %d", (int)bytesWritten);
        // in this context, a DEAD_OBJECT is more likely to be a disconnect notification due to
        // AudioTrack invalidation
        if (bytesWritten == DEAD_OBJECT) {
            setDisconnected();
            return AAUDIO_ERROR_DISCONNECTED;
        }
        return AAudioConvert_androidToAAudioResult(bytesWritten);
    }
    int32_t framesWritten = (int32_t)(bytesWritten / bytesPerFrame);
    incrementFramesWritten(framesWritten);

    result = updateStateMachine();
    if (result != AAUDIO_OK) {
        return result;
    }

    return framesWritten;
}

aaudio_result_t AudioStreamTrack::setBufferSize(int32_t requestedFrames)
{
    // Do not ask for less than one burst.
    if (requestedFrames < getFramesPerBurst()) {
        requestedFrames = getFramesPerBurst();
    }
    ssize_t result = mAudioTrack->setBufferSizeInFrames(requestedFrames);
    if (result < 0) {
        return AAudioConvert_androidToAAudioResult(result);
    } else {
        return result;
    }
}

int32_t AudioStreamTrack::getBufferSize() const
{
    return static_cast<int32_t>(mAudioTrack->getBufferSizeInFrames());
}

int32_t AudioStreamTrack::getBufferCapacityFromDevice() const
{
    return static_cast<int32_t>(mAudioTrack->frameCount());
}

int32_t AudioStreamTrack::getXRunCount() const
{
    return static_cast<int32_t>(mAudioTrack->getUnderrunCount());
}

int32_t AudioStreamTrack::getFramesPerBurstFromDevice() const {
    return static_cast<int32_t>(mAudioTrack->getNotificationPeriodInFrames());
}

int64_t AudioStreamTrack::getFramesRead() {
    aaudio_wrapping_frames_t position;
    status_t result;
    switch (getState()) {
    case AAUDIO_STREAM_STATE_STARTING:
    case AAUDIO_STREAM_STATE_STARTED:
    case AAUDIO_STREAM_STATE_STOPPING:
    case AAUDIO_STREAM_STATE_PAUSING:
    case AAUDIO_STREAM_STATE_PAUSED:
        result = mAudioTrack->getPosition(&position);
        if (result == OK) {
            mFramesRead.update32((int32_t)position);
        }
        break;
    default:
        break;
    }
    return AudioStreamLegacy::getFramesRead();
}

aaudio_result_t AudioStreamTrack::getTimestamp(clockid_t clockId,
                                     int64_t *framePosition,
                                     int64_t *timeNanoseconds) {
    ExtendedTimestamp extendedTimestamp;
    status_t status = mAudioTrack->getTimestamp(&extendedTimestamp);
    if (status == WOULD_BLOCK) {
        return AAUDIO_ERROR_INVALID_STATE;
    } if (status != NO_ERROR) {
        return AAudioConvert_androidToAAudioResult(status);
    }
    int64_t position = 0;
    int64_t nanoseconds = 0;
    aaudio_result_t result = getBestTimestamp(clockId, &position,
                                              &nanoseconds, &extendedTimestamp);
    if (result == AAUDIO_OK) {
        if (position < getFramesWritten()) {
            *framePosition = position;
            *timeNanoseconds = nanoseconds;
            return result;
        } else {
            return AAUDIO_ERROR_INVALID_STATE; // TODO review, documented but not consistent
        }
    }
    return result;
}

status_t AudioStreamTrack::doSetVolume() {
    status_t status = NO_INIT;
    if (mAudioTrack.get() != nullptr) {
        float volume = getDuckAndMuteVolume();
        mAudioTrack->setVolume(volume, volume);
        status = NO_ERROR;
    }
    return status;
}

void AudioStreamTrack::registerPlayerBase() {
    AudioStream::registerPlayerBase();

    if (mAudioTrack == nullptr) {
        ALOGW("%s: cannot set piid, AudioTrack is null", __func__);
        return;
    }
    mAudioTrack->setPlayerIId(mPlayerBase->getPlayerIId());
}

aaudio_result_t AudioStreamTrack::systemStopInternal_l() {
    if (aaudio_result_t result = AudioStream::systemStopInternal_l(); result != AAUDIO_OK) {
        return result;
    }
    mOffloadEosPending = false;
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::setOffloadDelayPadding(
        int32_t delayInFrames, int32_t paddingInFrames) {
    if (getPerformanceMode() != AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED ||
        audio_is_linear_pcm(getFormat())) {
        return AAUDIO_ERROR_UNIMPLEMENTED;
    }
    if (mAudioTrack == nullptr) {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    AudioParameter param = AudioParameter();
    param.addInt(String8(AUDIO_OFFLOAD_CODEC_DELAY_SAMPLES),  delayInFrames);
    param.addInt(String8(AUDIO_OFFLOAD_CODEC_PADDING_SAMPLES),  paddingInFrames);
    mAudioTrack->setParameters(param.toString());
    mOffloadDelayFrames.store(delayInFrames);
    mOffloadPaddingFrames.store(paddingInFrames);
    return AAUDIO_OK;
}

int32_t AudioStreamTrack::getOffloadDelay() {
    if (getPerformanceMode() != AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED ||
        audio_is_linear_pcm(getFormat())) {
        return AAUDIO_ERROR_UNIMPLEMENTED;
    }
    if (mAudioTrack == nullptr) {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    return mOffloadDelayFrames.load();
}

int32_t AudioStreamTrack::getOffloadPadding() {
    if (getPerformanceMode() != AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED ||
        audio_is_linear_pcm(getFormat())) {
        return AAUDIO_ERROR_UNIMPLEMENTED;
    }
    if (mAudioTrack == nullptr) {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    return mOffloadPaddingFrames.load();
}

aaudio_result_t AudioStreamTrack::setOffloadEndOfStream() {
    if (getPerformanceMode() != AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED) {
        return AAUDIO_ERROR_UNIMPLEMENTED;
    }
    if (mAudioTrack == nullptr) {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    std::lock_guard<std::mutex> lock(mStreamLock);
    if (aaudio_result_t result = safeStop_l(); result != AAUDIO_OK) {
        return result;
    }
    mOffloadEosPending = true;
    setState(AAUDIO_STREAM_STATE_STOPPING);
    return AAUDIO_OK;
}

bool AudioStreamTrack::collidesWithCallback() const {
    if (AudioStream::collidesWithCallback()) {
        return true;
    }
    pid_t thisThread = gettid();
    return mPresentationEndCallbackThread.load() == thisThread;
}

void AudioStreamTrack::onStreamEnd() {
    if (getPerformanceMode() != AAUDIO_PERFORMANCE_MODE_POWER_SAVING_OFFLOADED) {
        return;
    }
    if (getState() == AAUDIO_STREAM_STATE_STOPPING) {
        std::lock_guard<std::mutex> lock(mStreamLock);
        if (mOffloadEosPending) {
            requestStart_l();
        } else {
            setState(AAUDIO_STREAM_STATE_STOPPED);
        }
        mOffloadEosPending = false;
    }
    maybeCallPresentationEndCallback();
}

void AudioStreamTrack::maybeCallPresentationEndCallback() {
    if (mPresentationEndCallbackProc != nullptr) {
        pid_t expected = CALLBACK_THREAD_NONE;
        if (mPresentationEndCallbackThread.compare_exchange_strong(expected, gettid())) {
            (*mPresentationEndCallbackProc)(
                    (AAudioStream *) this, mPresentationEndCallbackUserData);
            mPresentationEndCallbackThread.store(CALLBACK_THREAD_NONE);
        } else {
            ALOGW("%s() error callback already running!", __func__);
        }
    }
}

#if AAUDIO_USE_VOLUME_SHAPER

using namespace android::media::VolumeShaper;

binder::Status AudioStreamTrack::applyVolumeShaper(
        const VolumeShaper::Configuration& configuration,
        const VolumeShaper::Operation& operation) {

    sp<VolumeShaper::Configuration> spConfiguration = new VolumeShaper::Configuration(configuration);
    sp<VolumeShaper::Operation> spOperation = new VolumeShaper::Operation(operation);

    if (mAudioTrack.get() != nullptr) {
        ALOGD("applyVolumeShaper() from IPlayer");
        binder::Status status = mAudioTrack->applyVolumeShaper(spConfiguration, spOperation);
        if (status < 0) { // a non-negative value is the volume shaper id.
            ALOGE("applyVolumeShaper() failed with status %d", status);
        }
        return aidl_utils::binderStatusFromStatusT(status);
    } else {
        ALOGD("applyVolumeShaper()"
                      " no AudioTrack for volume control from IPlayer");
        return binder::Status::ok();
    }
}
#endif
