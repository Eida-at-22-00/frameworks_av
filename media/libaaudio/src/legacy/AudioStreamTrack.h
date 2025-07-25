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

#ifndef LEGACY_AUDIO_STREAM_TRACK_H
#define LEGACY_AUDIO_STREAM_TRACK_H

#include <math.h>
#include <media/TrackPlayerBase.h>
#include <media/AudioTrack.h>
#include <aaudio/AAudio.h>

#include "AudioStreamBuilder.h"
#include "AudioStream.h"
#include "legacy/AAudioLegacy.h"
#include "legacy/AudioStreamLegacy.h"
#include "utility/FixedBlockReader.h"

namespace aaudio {

/**
 * Internal stream that uses the legacy AudioTrack path.
 */
class AudioStreamTrack : public AudioStreamLegacy {
public:
    AudioStreamTrack();

    virtual ~AudioStreamTrack();


    aaudio_result_t open(const AudioStreamBuilder & builder) override;
    aaudio_result_t release_l() override;
    void close_l() override;

protected:
    aaudio_result_t requestStart_l() REQUIRES(mStreamLock)  override;
    aaudio_result_t requestPause_l() REQUIRES(mStreamLock) override;
    aaudio_result_t requestFlush_l() REQUIRES(mStreamLock) override;
    aaudio_result_t requestStop_l() REQUIRES(mStreamLock) override;
    aaudio_result_t systemStopInternal_l() REQUIRES(mStreamLock) final;

    bool collidesWithCallback() const final;

    void onStreamEnd() final;

public:
    bool isFlushSupported() const override {
        // Only implement FLUSH for OUTPUT streams.
        return true;
    }

    bool isPauseSupported() const override {
        // Only implement PAUSE for OUTPUT streams.
        return true;
    }

    aaudio_result_t getTimestamp(clockid_t clockId,
                                       int64_t *framePosition,
                                       int64_t *timeNanoseconds) override;

    aaudio_result_t write(const void *buffer,
                             int32_t numFrames,
                             int64_t timeoutNanoseconds) override;

    aaudio_result_t setBufferSize(int32_t requestedFrames) override;
    int32_t getBufferSize() const override;
    int32_t getXRunCount() const override;

    int64_t getFramesRead() override;

    aaudio_direction_t getDirection() const override {
        return AAUDIO_DIRECTION_OUTPUT;
    }

    aaudio_result_t processCommands() override;

    int64_t incrementClientFrameCounter(int32_t frames) override {
        return incrementFramesWritten(frames);
    }

    android::status_t doSetVolume() override;

    void registerPlayerBase() override;

    // Offload begin --------------------------------------
    aaudio_result_t setOffloadDelayPadding(int32_t delayInFrames, int32_t paddingInFrames) final;

    int32_t getOffloadDelay() final;

    int32_t getOffloadPadding() final;

    aaudio_result_t setOffloadEndOfStream() EXCLUDES(mStreamLock) final;

    void setPresentationEndCallbackProc(AAudioStream_presentationEndCallback proc) final {
        mPresentationEndCallbackProc = proc;
    }

    virtual void setPresentationEndCallbackUserData(void *userData) final {
        mPresentationEndCallbackUserData = userData;
    }

    void maybeCallPresentationEndCallback();

    bool shouldStopStream() const final { return !mOffloadEosPending; }
    // Offload end ----------------------------------------

#if AAUDIO_USE_VOLUME_SHAPER
    virtual android::binder::Status applyVolumeShaper(
            const android::media::VolumeShaper::Configuration& configuration,
            const android::media::VolumeShaper::Operation& operation) override;
#endif

protected:

    int32_t getFramesPerBurstFromDevice() const override;
    int32_t getBufferCapacityFromDevice() const override;
    void onNewIAudioTrack() override;

private:

    android::sp<android::AudioTrack> mAudioTrack;

    // adapts between variable sized blocks and fixed size blocks
    FixedBlockReader                 mFixedBlockReader;

    // TODO add 64-bit position reporting to AudioTrack and use it.
    aaudio_wrapping_frames_t         mPositionWhenPausing = 0;

    // Offload --------------------------------------------
    std::atomic<int32_t>        mOffloadDelayFrames = 0;
    std::atomic<int32_t>        mOffloadPaddingFrames = 0;
    bool                        mOffloadEosPending GUARDED_BY(mStreamLock) = false;

    AAudioStream_presentationEndCallback mPresentationEndCallbackProc = nullptr;
    void                                *mPresentationEndCallbackUserData = nullptr;
    std::atomic<pid_t>                   mPresentationEndCallbackThread{CALLBACK_THREAD_NONE};
};

} /* namespace aaudio */

#endif /* LEGACY_AUDIO_STREAM_TRACK_H */
