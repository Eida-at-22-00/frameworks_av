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

//#define LOG_NDEBUG 0
#define LOG_TAG "StagefrightRecorder"
#define ATRACE_TAG ATRACE_TAG_VIDEO
#include <utils/Trace.h>
#include <inttypes.h>
// TODO/workaround: including base logging now as it conflicts with ADebug.h
// and it must be included first.
#include <android-base/logging.h>
#include <utils/Log.h>

#include <webm/WebmWriter.h>

#include "StagefrightRecorder.h"

#include <algorithm>

#include <android-base/properties.h>
#include <android/hardware/ICamera.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>

#include <media/AidlConversion.h>
#include <media/IMediaPlayerService.h>
#include <media/MediaMetricsItem.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/AudioSource.h>
#include <media/stagefright/AMRWriter.h>
#include <media/stagefright/AACWriter.h>
#include <media/stagefright/CameraSource.h>
#include <media/stagefright/CameraSourceTimeLapse.h>
#include <media/stagefright/MPEG2TSWriter.h>
#include <media/stagefright/MPEG4Writer.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaCodecSource.h>
#include <media/stagefright/OggWriter.h>
#include <media/stagefright/PersistentSurface.h>
#include <media/MediaProfiles.h>
#include <camera/CameraParameters.h>
#include <gui/Flags.h>

#include <utils/Errors.h>
#include <sys/types.h>
#include <ctype.h>
#include <unistd.h>

#include <system/audio.h>

#include <media/stagefright/rtsp/ARTPWriter.h>
#include <com_android_media_editing_flags.h>

namespace android {

static const float kTypicalDisplayRefreshingRate = 60.f;
// display refresh rate drops on battery saver
static const float kMinTypicalDisplayRefreshingRate = kTypicalDisplayRefreshingRate / 2;
static const int kMaxNumVideoTemporalLayers = 8;

// key for media statistics
static const char *kKeyRecorder = "recorder";
// attrs for media statistics
// NB: these are matched with public Java API constants defined
// in frameworks/base/media/java/android/media/MediaRecorder.java
// These must be kept synchronized with the constants there.
static const char *kRecorderLogSessionId = "android.media.mediarecorder.log-session-id";
static const char *kRecorderAudioBitrate = "android.media.mediarecorder.audio-bitrate";
static const char *kRecorderAudioChannels = "android.media.mediarecorder.audio-channels";
static const char *kRecorderAudioSampleRate = "android.media.mediarecorder.audio-samplerate";
static const char *kRecorderAudioTimescale = "android.media.mediarecorder.audio-timescale";
static const char *kRecorderCaptureFps = "android.media.mediarecorder.capture-fps";
static const char *kRecorderCaptureFpsEnable = "android.media.mediarecorder.capture-fpsenable";
static const char *kRecorderFrameRate = "android.media.mediarecorder.frame-rate";
static const char *kRecorderHeight = "android.media.mediarecorder.height";
static const char *kRecorderMovieTimescale = "android.media.mediarecorder.movie-timescale";
static const char *kRecorderRotation = "android.media.mediarecorder.rotation";
static const char *kRecorderVideoBitrate = "android.media.mediarecorder.video-bitrate";
static const char *kRecorderVideoIframeInterval = "android.media.mediarecorder.video-iframe-interval";
static const char *kRecorderVideoLevel = "android.media.mediarecorder.video-encoder-level";
static const char *kRecorderVideoProfile = "android.media.mediarecorder.video-encoder-profile";
static const char *kRecorderVideoTimescale = "android.media.mediarecorder.video-timescale";
static const char *kRecorderWidth = "android.media.mediarecorder.width";

// new fields, not yet frozen in the public Java API definitions
static const char *kRecorderAudioMime = "android.media.mediarecorder.audio.mime";
static const char *kRecorderVideoMime = "android.media.mediarecorder.video.mime";
static const char *kRecorderDurationMs = "android.media.mediarecorder.durationMs";
static const char *kRecorderPaused = "android.media.mediarecorder.pausedMs";
static const char *kRecorderNumPauses = "android.media.mediarecorder.NPauses";


// To collect the encoder usage for the battery app
static void addBatteryData(uint32_t params) {
    sp<IBinder> binder =
        defaultServiceManager()->waitForService(String16("media.player"));
    sp<IMediaPlayerService> service = interface_cast<IMediaPlayerService>(binder);
    if (service.get() == nullptr) {
        ALOGE("%s: Failed to get media.player service", __func__);
        return;
    }

    service->addBatteryData(params);
}


StagefrightRecorder::StagefrightRecorder(const AttributionSourceState& client)
    : MediaRecorderBase(client),
      mWriter(NULL),
      mOutputFd(-1),
      mAudioSource((audio_source_t)AUDIO_SOURCE_CNT), // initialize with invalid value
      mPrivacySensitive(PRIVACY_SENSITIVE_DEFAULT),
      mVideoSource(VIDEO_SOURCE_LIST_END),
      mRTPCVOExtMap(-1),
      mRTPCVODegrees(0),
      mRTPSockDscp(0),
      mRTPSockOptEcn(0),
      mRTPSockNetwork(0),
      mLastSeqNo(0),
      mStarted(false),
      mSelectedDeviceId(AUDIO_PORT_HANDLE_NONE),
      mDeviceCallbackEnabled(false),
      mSelectedMicDirection(MIC_DIRECTION_UNSPECIFIED),
      mSelectedMicFieldDimension(MIC_FIELD_DIMENSION_NORMAL) {

    ALOGV("Constructor");

    mMetricsItem = NULL;
    mAnalyticsDirty = false;
    reset();
}

StagefrightRecorder::~StagefrightRecorder() {
    ALOGV("Destructor");
    stop();

    if (mLooper != NULL) {
        mLooper->stop();
    }

    // log the current record, provided it has some information worth recording
    // NB: this also reclaims & clears mMetricsItem.
    flushAndResetMetrics(false);
}

void StagefrightRecorder::updateMetrics() {
    ALOGV("updateMetrics");

    // we run as part of the media player service; what we really want to
    // know is the app which requested the recording.
    mMetricsItem->setUid(VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(mAttributionSource.uid)));

    mMetricsItem->setCString(kRecorderLogSessionId, mLogSessionId.c_str());

    // populate the values from the raw fields.

    // TBD mOutputFormat  = OUTPUT_FORMAT_THREE_GPP;
    // TBD mAudioEncoder  = AUDIO_ENCODER_AMR_NB;
    // TBD mVideoEncoder  = VIDEO_ENCODER_DEFAULT;
    mMetricsItem->setInt32(kRecorderHeight, mVideoHeight);
    mMetricsItem->setInt32(kRecorderWidth, mVideoWidth);
    mMetricsItem->setInt32(kRecorderFrameRate, mFrameRate);
    mMetricsItem->setInt32(kRecorderVideoBitrate, mVideoBitRate);
    mMetricsItem->setInt32(kRecorderAudioSampleRate, mSampleRate);
    mMetricsItem->setInt32(kRecorderAudioChannels, mAudioChannels);
    mMetricsItem->setInt32(kRecorderAudioBitrate, mAudioBitRate);
    // TBD mInterleaveDurationUs = 0;
    mMetricsItem->setInt32(kRecorderVideoIframeInterval, mIFramesIntervalSec);
    // TBD mAudioSourceNode = 0;
    // TBD mUse64BitFileOffset = false;
    if (mMovieTimeScale != -1)
        mMetricsItem->setInt32(kRecorderMovieTimescale, mMovieTimeScale);
    if (mAudioTimeScale != -1)
        mMetricsItem->setInt32(kRecorderAudioTimescale, mAudioTimeScale);
    if (mVideoTimeScale != -1)
        mMetricsItem->setInt32(kRecorderVideoTimescale, mVideoTimeScale);
    // TBD mCameraId        = 0;
    // TBD mStartTimeOffsetMs = -1;
    mMetricsItem->setInt32(kRecorderVideoProfile, mVideoEncoderProfile);
    mMetricsItem->setInt32(kRecorderVideoLevel, mVideoEncoderLevel);
    // TBD mMaxFileDurationUs = 0;
    // TBD mMaxFileSizeBytes = 0;
    // TBD mTrackEveryTimeDurationUs = 0;
    mMetricsItem->setInt32(kRecorderCaptureFpsEnable, mCaptureFpsEnable);
    mMetricsItem->setDouble(kRecorderCaptureFps, mCaptureFps);
    // TBD mCameraSourceTimeLapse = NULL;
    // TBD mMetaDataStoredInVideoBuffers = kMetadataBufferTypeInvalid;
    // TBD mEncoderProfiles = MediaProfiles::getInstance();
    mMetricsItem->setInt32(kRecorderRotation, mRotationDegrees);
    // PII mLatitudex10000 = -3600000;
    // PII mLongitudex10000 = -3600000;
    // TBD mTotalBitRate = 0;

    // duration information (recorded, paused, # of pauses)
    mMetricsItem->setInt64(kRecorderDurationMs, (mDurationRecordedUs+500)/1000 );
    if (mNPauses != 0) {
        mMetricsItem->setInt64(kRecorderPaused, (mDurationPausedUs+500)/1000 );
        mMetricsItem->setInt32(kRecorderNumPauses, mNPauses);
    }
}

void StagefrightRecorder::flushAndResetMetrics(bool reinitialize) {
    ALOGV("flushAndResetMetrics");
    // flush anything we have, maybe setup a new record
    if (mMetricsItem != NULL) {
        if (mAnalyticsDirty) {
            updateMetrics();
            if (mMetricsItem->count() > 0) {
                mMetricsItem->selfrecord();
            }
        }
        delete mMetricsItem;
        mMetricsItem = NULL;
    }
    mAnalyticsDirty = false;
    if (reinitialize) {
        mMetricsItem = mediametrics::Item::create(kKeyRecorder);
    }
}

status_t StagefrightRecorder::init() {
    ALOGV("init");

    mLooper = new ALooper;
    mLooper->setName("recorder_looper");
    mLooper->start();

    return OK;
}

// The client side of mediaserver asks it to create a SurfaceMediaSource
// and return a interface reference. The client side will use that
// while encoding GL Frames
sp<IGraphicBufferProducer> StagefrightRecorder::querySurfaceMediaSource() const {
    ALOGV("Get SurfaceMediaSource");
    return mGraphicBufferProducer;
}

status_t StagefrightRecorder::setAudioSource(audio_source_t as) {
    ALOGV("setAudioSource: %d", as);

    if (as == AUDIO_SOURCE_DEFAULT) {
        mAudioSource = AUDIO_SOURCE_MIC;
    } else {
        mAudioSource = as;
    }
    // Reset privacy sensitive in case this is the second time audio source is set
    mPrivacySensitive = PRIVACY_SENSITIVE_DEFAULT;
    return OK;
}

status_t StagefrightRecorder::setPrivacySensitive(bool privacySensitive) {
    // privacy sensitive cannot be set before audio source is set
    if (mAudioSource == AUDIO_SOURCE_CNT) {
        return INVALID_OPERATION;
    }
    mPrivacySensitive = privacySensitive ? PRIVACY_SENSITIVE_ENABLED : PRIVACY_SENSITIVE_DISABLED;
    return OK;
}

status_t StagefrightRecorder::isPrivacySensitive(bool *privacySensitive) const {
    *privacySensitive = false;
    if (mAudioSource == AUDIO_SOURCE_CNT) {
        return INVALID_OPERATION;
    }
    if (mPrivacySensitive == PRIVACY_SENSITIVE_DEFAULT) {
         *privacySensitive = mAudioSource == AUDIO_SOURCE_VOICE_COMMUNICATION
                || mAudioSource == AUDIO_SOURCE_CAMCORDER;
    } else {
        *privacySensitive = mPrivacySensitive == PRIVACY_SENSITIVE_ENABLED;
    }
    return OK;
}

status_t StagefrightRecorder::setVideoSource(video_source vs) {
    ALOGV("setVideoSource: %d", vs);
    if (vs < VIDEO_SOURCE_DEFAULT ||
        vs >= VIDEO_SOURCE_LIST_END) {
        ALOGE("Invalid video source: %d", vs);
        return BAD_VALUE;
    }

    if (vs == VIDEO_SOURCE_DEFAULT) {
        mVideoSource = VIDEO_SOURCE_CAMERA;
    } else {
        mVideoSource = vs;
    }

    return OK;
}

status_t StagefrightRecorder::setOutputFormat(output_format of) {
    ALOGV("setOutputFormat: %d", of);
    if (of < OUTPUT_FORMAT_DEFAULT ||
        of >= OUTPUT_FORMAT_LIST_END) {
        ALOGE("Invalid output format: %d", of);
        return BAD_VALUE;
    }

    if (of == OUTPUT_FORMAT_DEFAULT) {
        mOutputFormat = OUTPUT_FORMAT_THREE_GPP;
    } else {
        mOutputFormat = of;
    }

    return OK;
}

status_t StagefrightRecorder::setAudioEncoder(audio_encoder ae) {
    ALOGV("setAudioEncoder: %d", ae);
    if (ae < AUDIO_ENCODER_DEFAULT ||
        ae >= AUDIO_ENCODER_LIST_END) {
        ALOGE("Invalid audio encoder: %d", ae);
        return BAD_VALUE;
    }

    if (ae == AUDIO_ENCODER_DEFAULT) {
        mAudioEncoder = AUDIO_ENCODER_AMR_NB;
    } else {
        mAudioEncoder = ae;
    }

    return OK;
}

status_t StagefrightRecorder::setVideoEncoder(video_encoder ve) {
    ALOGV("setVideoEncoder: %d", ve);
    if (ve < VIDEO_ENCODER_DEFAULT ||
        ve >= VIDEO_ENCODER_LIST_END) {
        ALOGE("Invalid video encoder: %d", ve);
        return BAD_VALUE;
    }

    mVideoEncoder = ve;

    return OK;
}

status_t StagefrightRecorder::setVideoSize(int width, int height) {
    ALOGV("setVideoSize: %dx%d", width, height);
    if (width <= 0 || height <= 0) {
        ALOGE("Invalid video size: %dx%d", width, height);
        return BAD_VALUE;
    }

    // Additional check on the dimension will be performed later
    mVideoWidth = width;
    mVideoHeight = height;

    return OK;
}

status_t StagefrightRecorder::setVideoFrameRate(int frames_per_second) {
    ALOGV("setVideoFrameRate: %d", frames_per_second);
    if ((frames_per_second <= 0 && frames_per_second != -1) ||
        frames_per_second > kMaxHighSpeedFps) {
        ALOGE("Invalid video frame rate: %d", frames_per_second);
        return BAD_VALUE;
    }

    // Additional check on the frame rate will be performed later
    mFrameRate = frames_per_second;

    return OK;
}

status_t StagefrightRecorder::setCamera(const sp<hardware::ICamera> &camera,
                                        const sp<ICameraRecordingProxy> &proxy) {
    ALOGV("setCamera");
    if (camera == 0) {
        ALOGE("camera is NULL");
        return BAD_VALUE;
    }
    if (proxy == 0) {
        ALOGE("camera proxy is NULL");
        return BAD_VALUE;
    }

    mCamera = camera;
    mCameraProxy = proxy;
    return OK;
}

status_t StagefrightRecorder::setPreviewSurface(const sp<IGraphicBufferProducer> &surface) {
    ALOGV("setPreviewSurface: %p", surface.get());
    mPreviewSurface = surface;

    return OK;
}

status_t StagefrightRecorder::setInputSurface(
        const sp<PersistentSurface>& surface) {
    mPersistentSurface = surface;

    return OK;
}

status_t StagefrightRecorder::setOutputFile(int fd) {
    ALOGV("setOutputFile: %d", fd);

    if (fd < 0) {
        ALOGE("Invalid file descriptor: %d", fd);
        return -EBADF;
    }

    // start with a clean, empty file
    ftruncate(fd, 0);

    if (mOutputFd >= 0) {
        ::close(mOutputFd);
    }
    mOutputFd = dup(fd);

    return OK;
}

status_t StagefrightRecorder::setNextOutputFile(int fd) {
    Mutex::Autolock autolock(mLock);
    // Only support MPEG4
    if (mOutputFormat != OUTPUT_FORMAT_MPEG_4) {
        ALOGE("Only MP4 file format supports setting next output file");
        return INVALID_OPERATION;
    }
    ALOGV("setNextOutputFile: %d", fd);

    if (fd < 0) {
        ALOGE("Invalid file descriptor: %d", fd);
        return -EBADF;
    }

    if (mWriter == nullptr) {
        ALOGE("setNextOutputFile failed. Writer has been freed");
        return INVALID_OPERATION;
    }

    // start with a clean, empty file
    ftruncate(fd, 0);

    return mWriter->setNextFd(fd);
}

// Attempt to parse an float literal optionally surrounded by whitespace,
// returns true on success, false otherwise.
static bool safe_strtod(const char *s, double *val) {
    char *end;

    // It is lame, but according to man page, we have to set errno to 0
    // before calling strtod().
    errno = 0;
    *val = strtod(s, &end);

    if (end == s || errno == ERANGE) {
        return false;
    }

    // Skip trailing whitespace
    while (isspace(*end)) {
        ++end;
    }

    // For a successful return, the string must contain nothing but a valid
    // float literal optionally surrounded by whitespace.

    return *end == '\0';
}

// Attempt to parse an int64 literal optionally surrounded by whitespace,
// returns true on success, false otherwise.
static bool safe_strtoi64(const char *s, int64_t *val) {
    char *end;

    // It is lame, but according to man page, we have to set errno to 0
    // before calling strtoll().
    errno = 0;
    *val = strtoll(s, &end, 10);

    if (end == s || errno == ERANGE) {
        return false;
    }

    // Skip trailing whitespace
    while (isspace(*end)) {
        ++end;
    }

    // For a successful return, the string must contain nothing but a valid
    // int64 literal optionally surrounded by whitespace.

    return *end == '\0';
}

// Return true if the value is in [0, 0x007FFFFFFF]
static bool safe_strtoi32(const char *s, int32_t *val) {
    int64_t temp;
    if (safe_strtoi64(s, &temp)) {
        if (temp >= 0 && temp <= 0x007FFFFFFF) {
            *val = static_cast<int32_t>(temp);
            return true;
        }
    }
    return false;
}

// Trim both leading and trailing whitespace from the given string.
static void TrimString(String8 *s) {
    size_t num_bytes = s->bytes();
    const char *data = s->c_str();

    size_t leading_space = 0;
    while (leading_space < num_bytes && isspace(data[leading_space])) {
        ++leading_space;
    }

    size_t i = num_bytes;
    while (i > leading_space && isspace(data[i - 1])) {
        --i;
    }

    *s = String8(&data[leading_space], i - leading_space);
}

status_t StagefrightRecorder::setParamAudioSamplingRate(int32_t sampleRate) {
    ALOGV("setParamAudioSamplingRate: %d", sampleRate);
    if (sampleRate <= 0) {
        ALOGE("Invalid audio sampling rate: %d", sampleRate);
        return BAD_VALUE;
    }

    // Additional check on the sample rate will be performed later.
    mSampleRate = sampleRate;

    return OK;
}

status_t StagefrightRecorder::setParamAudioNumberOfChannels(int32_t channels) {
    ALOGV("setParamAudioNumberOfChannels: %d", channels);
    if (channels <= 0 || channels >= 3) {
        ALOGE("Invalid number of audio channels: %d", channels);
        return BAD_VALUE;
    }

    // Additional check on the number of channels will be performed later.
    mAudioChannels = channels;

    return OK;
}

status_t StagefrightRecorder::setParamAudioEncodingBitRate(int32_t bitRate) {
    ALOGV("setParamAudioEncodingBitRate: %d", bitRate);
    if (bitRate <= 0) {
        ALOGE("Invalid audio encoding bit rate: %d", bitRate);
        return BAD_VALUE;
    }

    // The target bit rate may not be exactly the same as the requested.
    // It depends on many factors, such as rate control, and the bit rate
    // range that a specific encoder supports. The mismatch between the
    // the target and requested bit rate will NOT be treated as an error.
    mAudioBitRate = bitRate;
    return OK;
}

status_t StagefrightRecorder::setParamVideoEncodingBitRate(int32_t bitRate) {
    ALOGV("setParamVideoEncodingBitRate: %d", bitRate);
    if (bitRate <= 0) {
        ALOGE("Invalid video encoding bit rate: %d", bitRate);
        return BAD_VALUE;
    }

    // The target bit rate may not be exactly the same as the requested.
    // It depends on many factors, such as rate control, and the bit rate
    // range that a specific encoder supports. The mismatch between the
    // the target and requested bit rate will NOT be treated as an error.
    mVideoBitRate = bitRate;

    // A new bitrate(TMMBR) should be applied on runtime as well if OutputFormat is RTP_AVP
    if (mOutputFormat == OUTPUT_FORMAT_RTP_AVP) {
        // Regular I frames may overload the network so we reduce the bitrate to allow
        // margins for the I frame overruns.
        // Still send requested bitrate (TMMBR) in the reply (TMMBN).
        const float coefficient = 0.8f;
        mVideoBitRate = (bitRate * coefficient) / 1000 * 1000;
    }
    if (mOutputFormat == OUTPUT_FORMAT_RTP_AVP && mStarted && mPauseStartTimeUs == 0) {
        mVideoEncoderSource->setEncodingBitrate(mVideoBitRate);
        ARTPWriter* rtpWriter  = static_cast<ARTPWriter*>(mWriter.get());
        rtpWriter->setTMMBNInfo(mOpponentID, bitRate);
    }

    return OK;
}

status_t StagefrightRecorder::setParamVideoBitRateMode(int32_t bitRateMode) {
    ALOGV("setParamVideoBitRateMode: %d", bitRateMode);
    // TODO: clarify what bitrate mode of -1 is as these start from 0
    if (bitRateMode < -1) {
        ALOGE("Unsupported video bitrate mode: %d", bitRateMode);
        return BAD_VALUE;
    }
    mVideoBitRateMode = bitRateMode;
    return OK;
}

// Always rotate clockwise, and only support 0, 90, 180 and 270 for now.
status_t StagefrightRecorder::setParamVideoRotation(int32_t degrees) {
    ALOGV("setParamVideoRotation: %d", degrees);
    if (degrees < 0 || degrees % 90 != 0) {
        ALOGE("Unsupported video rotation angle: %d", degrees);
        return BAD_VALUE;
    }
    mRotationDegrees = degrees % 360;
    return OK;
}

status_t StagefrightRecorder::setParamMaxFileDurationUs(int64_t timeUs) {
    ALOGV("setParamMaxFileDurationUs: %lld us", (long long)timeUs);

    // This is meant for backward compatibility for MediaRecorder.java
    if (timeUs <= 0) {
        ALOGW("Max file duration is not positive: %lld us. Disabling duration limit.",
                (long long)timeUs);
        timeUs = 0; // Disable the duration limit for zero or negative values.
    } else if (timeUs <= 100000LL) {  // XXX: 100 milli-seconds
        ALOGE("Max file duration is too short: %lld us", (long long)timeUs);
        return BAD_VALUE;
    }

    if (timeUs <= 15 * 1000000LL) {
        ALOGW("Target duration (%lld us) too short to be respected", (long long)timeUs);
    }
    mMaxFileDurationUs = timeUs;
    return OK;
}

status_t StagefrightRecorder::setParamMaxFileSizeBytes(int64_t bytes) {
    ALOGV("setParamMaxFileSizeBytes: %lld bytes", (long long)bytes);

    // This is meant for backward compatibility for MediaRecorder.java
    if (bytes <= 0) {
        ALOGW("Max file size is not positive: %lld bytes. "
             "Disabling file size limit.", (long long)bytes);
        bytes = 0; // Disable the file size limit for zero or negative values.
    } else if (bytes <= 1024) {  // XXX: 1 kB
        ALOGE("Max file size is too small: %lld bytes", (long long)bytes);
        return BAD_VALUE;
    }

    if (bytes <= 100 * 1024) {
        ALOGW("Target file size (%lld bytes) is too small to be respected", (long long)bytes);
    }

    mMaxFileSizeBytes = bytes;
    return OK;
}

status_t StagefrightRecorder::setParamInterleaveDuration(int32_t durationUs) {
    ALOGV("setParamInterleaveDuration: %d", durationUs);
    if (durationUs <= 500000) {           //  500 ms
        // If interleave duration is too small, it is very inefficient to do
        // interleaving since the metadata overhead will count for a significant
        // portion of the saved contents
        ALOGE("Audio/video interleave duration is too small: %d us", durationUs);
        return BAD_VALUE;
    } else if (durationUs >= 10000000) {  // 10 seconds
        // If interleaving duration is too large, it can cause the recording
        // session to use too much memory since we have to save the output
        // data before we write them out
        ALOGE("Audio/video interleave duration is too large: %d us", durationUs);
        return BAD_VALUE;
    }
    mInterleaveDurationUs = durationUs;
    return OK;
}

// If seconds <  0, only the first frame is I frame, and rest are all P frames
// If seconds == 0, all frames are encoded as I frames. No P frames
// If seconds >  0, it is the time spacing (seconds) between 2 neighboring I frames
status_t StagefrightRecorder::setParamVideoIFramesInterval(int32_t seconds) {
    ALOGV("setParamVideoIFramesInterval: %d seconds", seconds);
    mIFramesIntervalSec = seconds;
    return OK;
}

status_t StagefrightRecorder::setParam64BitFileOffset(bool use64Bit) {
    ALOGV("setParam64BitFileOffset: %s",
        use64Bit? "use 64 bit file offset": "use 32 bit file offset");
    mUse64BitFileOffset = use64Bit;
    return OK;
}

status_t StagefrightRecorder::setParamVideoCameraId(int32_t cameraId) {
    ALOGV("setParamVideoCameraId: %d", cameraId);
    if (cameraId < 0) {
        return BAD_VALUE;
    }
    mCameraId = cameraId;
    return OK;
}

status_t StagefrightRecorder::setParamTrackTimeStatus(int64_t timeDurationUs) {
    ALOGV("setParamTrackTimeStatus: %lld", (long long)timeDurationUs);
    if (timeDurationUs < 20000) {  // Infeasible if shorter than 20 ms?
        ALOGE("Tracking time duration too short: %lld us", (long long)timeDurationUs);
        return BAD_VALUE;
    }
    mTrackEveryTimeDurationUs = timeDurationUs;
    return OK;
}

status_t StagefrightRecorder::setParamVideoEncoderProfile(int32_t profile) {
    ALOGV("setParamVideoEncoderProfile: %d", profile);

    // Additional check will be done later when we load the encoder.
    // For now, we are accepting values defined in OpenMAX IL.
    mVideoEncoderProfile = profile;
    return OK;
}

status_t StagefrightRecorder::setParamVideoEncoderLevel(int32_t level) {
    ALOGV("setParamVideoEncoderLevel: %d", level);

    // Additional check will be done later when we load the encoder.
    // For now, we are accepting values defined in OpenMAX IL.
    mVideoEncoderLevel = level;
    return OK;
}

status_t StagefrightRecorder::setParamMovieTimeScale(int32_t timeScale) {
    ALOGV("setParamMovieTimeScale: %d", timeScale);

    // The range is set to be the same as the audio's time scale range
    // since audio's time scale has a wider range.
    if (timeScale < 600 || timeScale > 96000) {
        ALOGE("Time scale (%d) for movie is out of range [600, 96000]", timeScale);
        return BAD_VALUE;
    }
    mMovieTimeScale = timeScale;
    return OK;
}

status_t StagefrightRecorder::setParamVideoTimeScale(int32_t timeScale) {
    ALOGV("setParamVideoTimeScale: %d", timeScale);

    // 60000 is chosen to make sure that each video frame from a 60-fps
    // video has 1000 ticks.
    if (timeScale < 600 || timeScale > 60000) {
        ALOGE("Time scale (%d) for video is out of range [600, 60000]", timeScale);
        return BAD_VALUE;
    }
    mVideoTimeScale = timeScale;
    return OK;
}

status_t StagefrightRecorder::setParamAudioTimeScale(int32_t timeScale) {
    ALOGV("setParamAudioTimeScale: %d", timeScale);

    // 96000 Hz is the highest sampling rate support in AAC.
    if (timeScale < 600 || timeScale > 96000) {
        ALOGE("Time scale (%d) for audio is out of range [600, 96000]", timeScale);
        return BAD_VALUE;
    }
    mAudioTimeScale = timeScale;
    return OK;
}

status_t StagefrightRecorder::setParamCaptureFpsEnable(int32_t captureFpsEnable) {
    ALOGV("setParamCaptureFpsEnable: %d", captureFpsEnable);

    if(captureFpsEnable == 0) {
        mCaptureFpsEnable = false;
    } else if (captureFpsEnable == 1) {
        mCaptureFpsEnable = true;
    } else {
        return BAD_VALUE;
    }
    return OK;
}

status_t StagefrightRecorder::setParamCaptureFps(double fps) {
    ALOGV("setParamCaptureFps: %.2f", fps);

    if (!(fps >= 1.0 / 86400)) {
        ALOGE("FPS is too small");
        return BAD_VALUE;
    }
    mCaptureFps = fps;
    return OK;
}

status_t StagefrightRecorder::setParamGeoDataLongitude(
    int64_t longitudex10000) {

    if (longitudex10000 > 1800000 || longitudex10000 < -1800000) {
        return BAD_VALUE;
    }
    mLongitudex10000 = longitudex10000;
    return OK;
}

status_t StagefrightRecorder::setParamGeoDataLatitude(
    int64_t latitudex10000) {

    if (latitudex10000 > 900000 || latitudex10000 < -900000) {
        return BAD_VALUE;
    }
    mLatitudex10000 = latitudex10000;
    return OK;
}

status_t StagefrightRecorder::setParamRtpLocalIp(const String8 &localIp) {
    ALOGV("setParamVideoLocalIp: %s", localIp.c_str());

    mLocalIp = localIp.c_str();
    return OK;
}

status_t StagefrightRecorder::setParamRtpLocalPort(int32_t localPort) {
    ALOGV("setParamVideoLocalPort: %d", localPort);

    mLocalPort = localPort;
    return OK;
}

status_t StagefrightRecorder::setParamRtpRemoteIp(const String8 &remoteIp) {
    ALOGV("setParamVideoRemoteIp: %s", remoteIp.c_str());

    mRemoteIp = remoteIp.c_str();
    return OK;
}

status_t StagefrightRecorder::setParamRtpRemotePort(int32_t remotePort) {
    ALOGV("setParamVideoRemotePort: %d", remotePort);

    mRemotePort = remotePort;
    return OK;
}

status_t StagefrightRecorder::setParamSelfID(int32_t selfID) {
    ALOGV("setParamSelfID: %x", selfID);

    mSelfID = selfID;
    return OK;
}

status_t StagefrightRecorder::setParamVideoOpponentID(int32_t opponentID) {
    mOpponentID = opponentID;
    return OK;
}

status_t StagefrightRecorder::setParamPayloadType(int32_t payloadType) {
    ALOGV("setParamPayloadType: %d", payloadType);

    mPayloadType = payloadType;

    if (mStarted && mOutputFormat == OUTPUT_FORMAT_RTP_AVP) {
        mWriter->updatePayloadType(mPayloadType);
    }

    return OK;
}

status_t StagefrightRecorder::setRTPCVOExtMap(int32_t extmap) {
    ALOGV("setRtpCvoExtMap: %d", extmap);

    mRTPCVOExtMap = extmap;
    return OK;
}

status_t StagefrightRecorder::setRTPCVODegrees(int32_t cvoDegrees) {
    Mutex::Autolock autolock(mLock);
    ALOGV("setRtpCvoDegrees: %d", cvoDegrees);

    mRTPCVODegrees = cvoDegrees;

    if (mStarted && mOutputFormat == OUTPUT_FORMAT_RTP_AVP) {
        mWriter->updateCVODegrees(mRTPCVODegrees);
    }

    return OK;
}

status_t StagefrightRecorder::setParamRtpDscp(int32_t dscp) {
    ALOGV("setParamRtpDscp: %d", dscp);

    mRTPSockDscp = dscp;
    return OK;
}

status_t StagefrightRecorder::setSocketNetwork(int64_t networkHandle) {
    ALOGV("setSocketNetwork: %llu", (unsigned long long) networkHandle);

    mRTPSockNetwork = networkHandle;
    if (mStarted && mOutputFormat == OUTPUT_FORMAT_RTP_AVP) {
        mWriter->updateSocketNetwork(mRTPSockNetwork);
    }
    return OK;
}

status_t StagefrightRecorder::setParamRtpEcn(int32_t ecn) {
    ALOGV("setParamRtpEcn: %d", ecn);

    mRTPSockOptEcn = ecn;
    return OK;
}

status_t StagefrightRecorder::requestIDRFrame() {
    status_t ret = BAD_VALUE;
    if (mVideoEncoderSource != NULL) {
        ret = mVideoEncoderSource->requestIDRFrame();
    } else {
        ALOGV("requestIDRFrame: Encoder not ready");
    }
    return ret;
}

status_t StagefrightRecorder::setLogSessionId(const String8 &log_session_id) {
    ALOGV("setLogSessionId: %s", log_session_id.c_str());

    // TODO: validity check that log_session_id is a 32-byte hex digit.
    mLogSessionId = log_session_id.c_str();
    return OK;
}

status_t StagefrightRecorder::setParameter(
        const String8 &key, const String8 &value) {
    ALOGV("setParameter: key (%s) => value (%s)", key.c_str(), value.c_str());
    if (key == "max-duration") {
        int64_t max_duration_ms;
        if (safe_strtoi64(value.c_str(), &max_duration_ms)) {
            return setParamMaxFileDurationUs(1000LL * max_duration_ms);
        }
    } else if (key == "max-filesize") {
        int64_t max_filesize_bytes;
        if (safe_strtoi64(value.c_str(), &max_filesize_bytes)) {
            return setParamMaxFileSizeBytes(max_filesize_bytes);
        }
    } else if (key == "interleave-duration-us") {
        int32_t durationUs;
        if (safe_strtoi32(value.c_str(), &durationUs)) {
            return setParamInterleaveDuration(durationUs);
        }
    } else if (key == "param-movie-time-scale") {
        int32_t timeScale;
        if (safe_strtoi32(value.c_str(), &timeScale)) {
            return setParamMovieTimeScale(timeScale);
        }
    } else if (key == "param-use-64bit-offset") {
        int32_t use64BitOffset;
        if (safe_strtoi32(value.c_str(), &use64BitOffset)) {
            return setParam64BitFileOffset(use64BitOffset != 0);
        }
    } else if (key == "param-geotag-longitude") {
        int64_t longitudex10000;
        if (safe_strtoi64(value.c_str(), &longitudex10000)) {
            return setParamGeoDataLongitude(longitudex10000);
        }
    } else if (key == "param-geotag-latitude") {
        int64_t latitudex10000;
        if (safe_strtoi64(value.c_str(), &latitudex10000)) {
            return setParamGeoDataLatitude(latitudex10000);
        }
    } else if (key == "param-track-time-status") {
        int64_t timeDurationUs;
        if (safe_strtoi64(value.c_str(), &timeDurationUs)) {
            return setParamTrackTimeStatus(timeDurationUs);
        }
    } else if (key == "audio-param-sampling-rate") {
        int32_t sampling_rate;
        if (safe_strtoi32(value.c_str(), &sampling_rate)) {
            return setParamAudioSamplingRate(sampling_rate);
        }
    } else if (key == "audio-param-number-of-channels") {
        int32_t number_of_channels;
        if (safe_strtoi32(value.c_str(), &number_of_channels)) {
            return setParamAudioNumberOfChannels(number_of_channels);
        }
    } else if (key == "audio-param-encoding-bitrate") {
        int32_t audio_bitrate;
        if (safe_strtoi32(value.c_str(), &audio_bitrate)) {
            return setParamAudioEncodingBitRate(audio_bitrate);
        }
    } else if (key == "audio-param-time-scale") {
        int32_t timeScale;
        if (safe_strtoi32(value.c_str(), &timeScale)) {
            return setParamAudioTimeScale(timeScale);
        }
    } else if (key == "video-param-encoding-bitrate") {
        int32_t video_bitrate;
        if (safe_strtoi32(value.c_str(), &video_bitrate)) {
            return setParamVideoEncodingBitRate(video_bitrate);
        }
    } else if (key == "video-param-bitrate-mode") {
        int32_t video_bitrate_mode;
        if (safe_strtoi32(value.c_str(), &video_bitrate_mode)) {
            return setParamVideoBitRateMode(video_bitrate_mode);
        }
    } else if (key == "video-param-rotation-angle-degrees") {
        int32_t degrees;
        if (safe_strtoi32(value.c_str(), &degrees)) {
            return setParamVideoRotation(degrees);
        }
    } else if (key == "video-param-i-frames-interval") {
        int32_t seconds;
        if (safe_strtoi32(value.c_str(), &seconds)) {
            return setParamVideoIFramesInterval(seconds);
        }
    } else if (key == "video-param-encoder-profile") {
        int32_t profile;
        if (safe_strtoi32(value.c_str(), &profile)) {
            return setParamVideoEncoderProfile(profile);
        }
    } else if (key == "video-param-encoder-level") {
        int32_t level;
        if (safe_strtoi32(value.c_str(), &level)) {
            return setParamVideoEncoderLevel(level);
        }
    } else if (key == "video-param-camera-id") {
        int32_t cameraId;
        if (safe_strtoi32(value.c_str(), &cameraId)) {
            return setParamVideoCameraId(cameraId);
        }
    } else if (key == "video-param-time-scale") {
        int32_t timeScale;
        if (safe_strtoi32(value.c_str(), &timeScale)) {
            return setParamVideoTimeScale(timeScale);
        }
    } else if (key == "time-lapse-enable") {
        int32_t captureFpsEnable;
        if (safe_strtoi32(value.c_str(), &captureFpsEnable)) {
            return setParamCaptureFpsEnable(captureFpsEnable);
        }
    } else if (key == "time-lapse-fps") {
        double fps;
        if (safe_strtod(value.c_str(), &fps)) {
            return setParamCaptureFps(fps);
        }
    } else if (key == "rtp-param-local-ip") {
        return setParamRtpLocalIp(value);
    } else if (key == "rtp-param-local-port") {
        int32_t localPort;
        if (safe_strtoi32(value.c_str(), &localPort)) {
            return setParamRtpLocalPort(localPort);
        }
    } else if (key == "rtp-param-remote-ip") {
        return setParamRtpRemoteIp(value);
    } else if (key == "rtp-param-remote-port") {
        int32_t remotePort;
        if (safe_strtoi32(value.c_str(), &remotePort)) {
            return setParamRtpRemotePort(remotePort);
        }
    } else if (key == "rtp-param-self-id") {
        int32_t selfID;
        int64_t temp;
        if (safe_strtoi64(value.c_str(), &temp)) {
            selfID = static_cast<int32_t>(temp);
            return setParamSelfID(selfID);
        }
    } else if (key == "rtp-param-opponent-id") {
        int32_t opnId;
        int64_t temp;
        if (safe_strtoi64(value.c_str(), &temp)) {
            opnId = static_cast<int32_t>(temp);
            return setParamVideoOpponentID(opnId);
        }
    } else if (key == "rtp-param-payload-type") {
        int32_t payloadType;
        if (safe_strtoi32(value.c_str(), &payloadType)) {
            return setParamPayloadType(payloadType);
        }
    } else if (key == "rtp-param-ext-cvo-extmap") {
        int32_t extmap;
        if (safe_strtoi32(value.c_str(), &extmap)) {
            return setRTPCVOExtMap(extmap);
        }
    } else if (key == "rtp-param-ext-cvo-degrees") {
        int32_t degrees;
        if (safe_strtoi32(value.c_str(), &degrees)) {
            return setRTPCVODegrees(degrees);
        }
    } else if (key == "video-param-request-i-frame") {
        return requestIDRFrame();
    } else if (key == "rtp-param-set-socket-dscp") {
        int32_t dscp;
        if (safe_strtoi32(value.c_str(), &dscp)) {
            return setParamRtpDscp(dscp);
        }
    } else if (key == "rtp-param-set-socket-ecn") {
        int32_t targetEcn;
        if (safe_strtoi32(value.c_str(), &targetEcn)) {
            return setParamRtpEcn(targetEcn);
        }
    } else if (key == "rtp-param-set-socket-network") {
        int64_t networkHandle;
        if (safe_strtoi64(value.c_str(), &networkHandle)) {
            return setSocketNetwork(networkHandle);
        }
    } else if (key == "log-session-id") {
        return setLogSessionId(value);
    } else if (key == "set-title") {
        return OK;
    } else {
        ALOGE("setParameter: failed to find key %s", key.c_str());
    }
    return BAD_VALUE;
}

status_t StagefrightRecorder::setParameters(const String8 &params) {
    ALOGV("setParameters: %s", params.c_str());
    const char *cparams = params.c_str();
    const char *key_start = cparams;
    for (;;) {
        const char *equal_pos = strchr(key_start, '=');
        if (equal_pos == NULL) {
            ALOGE("Parameters %s miss a value", cparams);
            return BAD_VALUE;
        }
        String8 key(key_start, equal_pos - key_start);
        TrimString(&key);
        if (key.length() == 0) {
            ALOGE("Parameters %s contains an empty key", cparams);
            return BAD_VALUE;
        }
        const char *value_start = equal_pos + 1;
        const char *semicolon_pos = strchr(value_start, ';');
        String8 value;
        if (semicolon_pos == NULL) {
            value = value_start;
        } else {
            value = String8(value_start, semicolon_pos - value_start);
        }
        if (setParameter(key, value) != OK) {
            return BAD_VALUE;
        }
        if (semicolon_pos == NULL) {
            break;  // Reaches the end
        }
        key_start = semicolon_pos + 1;
    }
    return OK;
}

status_t StagefrightRecorder::setListener(const sp<IMediaRecorderClient> &listener) {
    mListener = listener;

    return OK;
}

status_t StagefrightRecorder::setClientName(const String16& clientName) {

    mAttributionSource.packageName = VALUE_OR_RETURN_STATUS(
            legacy2aidl_String16_string(clientName));

    return OK;
}

status_t StagefrightRecorder::prepareInternal() {
    ALOGV("prepare");
    if (mOutputFd < 0) {
        ALOGE("Output file descriptor is invalid");
        return INVALID_OPERATION;
    }

    status_t status = OK;

    switch (mOutputFormat) {
        case OUTPUT_FORMAT_DEFAULT:
        case OUTPUT_FORMAT_THREE_GPP:
        case OUTPUT_FORMAT_MPEG_4:
        case OUTPUT_FORMAT_WEBM:
            status = setupMPEG4orWEBMRecording();
            break;

        case OUTPUT_FORMAT_AMR_NB:
        case OUTPUT_FORMAT_AMR_WB:
            status = setupAMRRecording();
            break;

        case OUTPUT_FORMAT_AAC_ADIF:
        case OUTPUT_FORMAT_AAC_ADTS:
            status = setupAACRecording();
            break;

        case OUTPUT_FORMAT_RTP_AVP:
            status = setupRTPRecording();
            break;

        case OUTPUT_FORMAT_MPEG2TS:
            status = setupMPEG2TSRecording();
            break;

        case OUTPUT_FORMAT_OGG:
            status = setupOggRecording();
            break;

        default:
            ALOGE("Unsupported output file format: %d", mOutputFormat);
            status = UNKNOWN_ERROR;
            break;
    }

    ALOGV("Recording frameRate: %d captureFps: %f",
            mFrameRate, mCaptureFps);

    return status;
}

status_t StagefrightRecorder::prepare() {
    ALOGV("prepare");
    Mutex::Autolock autolock(mLock);
    if (mVideoSource == VIDEO_SOURCE_SURFACE) {
        return prepareInternal();
    }
    return OK;
}

status_t StagefrightRecorder::start() {
    ALOGV("start");
    Mutex::Autolock autolock(mLock);
    if (mOutputFd < 0) {
        ALOGE("Output file descriptor is invalid");
        return INVALID_OPERATION;
    }

    status_t status = OK;

    if (mVideoSource != VIDEO_SOURCE_SURFACE) {
        status = prepareInternal();
        if (status != OK) {
            return status;
        }
    }

    if (mWriter == NULL) {
        ALOGE("File writer is not avaialble");
        return UNKNOWN_ERROR;
    }

    switch (mOutputFormat) {
        case OUTPUT_FORMAT_DEFAULT:
        case OUTPUT_FORMAT_THREE_GPP:
        case OUTPUT_FORMAT_MPEG_4:
        case OUTPUT_FORMAT_WEBM:
        {
            sp<MetaData> meta = new MetaData;
            setupMPEG4orWEBMMetaData(&meta);
            status = mWriter->start(meta.get());
            break;
        }

        case OUTPUT_FORMAT_AMR_NB:
        case OUTPUT_FORMAT_AMR_WB:
        case OUTPUT_FORMAT_AAC_ADIF:
        case OUTPUT_FORMAT_AAC_ADTS:
        case OUTPUT_FORMAT_RTP_AVP:
        case OUTPUT_FORMAT_MPEG2TS:
        case OUTPUT_FORMAT_OGG:
        {
            sp<MetaData> meta = new MetaData;
            int64_t startTimeUs = systemTime() / 1000;
            meta->setInt64(kKeyTime, startTimeUs);
            meta->setInt32(kKeySelfID, mSelfID);
            meta->setInt32(kKeyPayloadType, mPayloadType);
            meta->setInt64(kKeySocketNetwork, mRTPSockNetwork);
            if (mRTPCVOExtMap > 0) {
                meta->setInt32(kKeyRtpExtMap, mRTPCVOExtMap);
                meta->setInt32(kKeyRtpCvoDegrees, mRTPCVODegrees);
            }
            if (mRTPSockDscp > 0) {
                meta->setInt32(kKeyRtpDscp, mRTPSockDscp);
            }
            if (mRTPSockOptEcn > 0) {
                meta->setInt32(kKeyRtpEcn, mRTPSockOptEcn);
            }

            status = mWriter->start(meta.get());
            break;
        }

        default:
        {
            ALOGE("Unsupported output file format: %d", mOutputFormat);
            status = UNKNOWN_ERROR;
            break;
        }
    }

    if (status != OK) {
        mWriter.clear();
        mWriter = NULL;
    }

    if ((status == OK) && (!mStarted)) {
        mAnalyticsDirty = true;
        mStarted = true;

        mStartedRecordingUs = systemTime() / 1000;

        uint32_t params = IMediaPlayerService::kBatteryDataCodecStarted;
        if (mAudioSource != AUDIO_SOURCE_CNT) {
            params |= IMediaPlayerService::kBatteryDataTrackAudio;
        }
        if (mVideoSource != VIDEO_SOURCE_LIST_END) {
            params |= IMediaPlayerService::kBatteryDataTrackVideo;
        }

        addBatteryData(params);
    }

    return status;
}

sp<MediaCodecSource> StagefrightRecorder::createAudioSource() {
    int32_t sourceSampleRate = mSampleRate;

    if (mCaptureFpsEnable && mCaptureFps >= mFrameRate) {
        // Upscale the sample rate for slow motion recording.
        // Fail audio source creation if source sample rate is too high, as it could
        // cause out-of-memory due to large input buffer size. And audio recording
        // probably doesn't make sense in the scenario, since the slow-down factor
        // is probably huge (eg. mSampleRate=48K, mCaptureFps=240, mFrameRate=1).
        const static int32_t kSampleRateHzMax = 192000;
        sourceSampleRate =
                (mSampleRate * mCaptureFps + mFrameRate / 2) / mFrameRate;
        if (sourceSampleRate < mSampleRate || sourceSampleRate > kSampleRateHzMax) {
            ALOGE("source sample rate out of range! "
                    "(mSampleRate %d, mCaptureFps %.2f, mFrameRate %d",
                    mSampleRate, mCaptureFps, mFrameRate);
            return NULL;
        }
    }

    audio_attributes_t attr = AUDIO_ATTRIBUTES_INITIALIZER;
    attr.source = mAudioSource;
    // attr.flags AUDIO_FLAG_CAPTURE_PRIVATE is cleared by default
    if (mPrivacySensitive == PRIVACY_SENSITIVE_DEFAULT) {
        if (attr.source == AUDIO_SOURCE_VOICE_COMMUNICATION
                || attr.source == AUDIO_SOURCE_CAMCORDER) {
            attr.flags = static_cast<audio_flags_mask_t>(attr.flags | AUDIO_FLAG_CAPTURE_PRIVATE);
            mPrivacySensitive = PRIVACY_SENSITIVE_ENABLED;
        } else {
            mPrivacySensitive = PRIVACY_SENSITIVE_DISABLED;
        }
    } else {
        if (mAudioSource == AUDIO_SOURCE_REMOTE_SUBMIX
                || mAudioSource == AUDIO_SOURCE_FM_TUNER
                || mAudioSource == AUDIO_SOURCE_VOICE_DOWNLINK
                || mAudioSource == AUDIO_SOURCE_VOICE_UPLINK
                || mAudioSource == AUDIO_SOURCE_VOICE_CALL
                || mAudioSource == AUDIO_SOURCE_ECHO_REFERENCE) {
            ALOGE("Cannot request private capture with source: %d", mAudioSource);
            return NULL;
        }
        if (mPrivacySensitive == PRIVACY_SENSITIVE_ENABLED) {
            attr.flags = static_cast<audio_flags_mask_t>(attr.flags | AUDIO_FLAG_CAPTURE_PRIVATE);
        }
    }

    sp<AudioSource> audioSource =
        new AudioSource(
                &attr,
                mAttributionSource,
                sourceSampleRate,
                mAudioChannels,
                mSampleRate,
                mSelectedDeviceId,
                mSelectedMicDirection,
                mSelectedMicFieldDimension);

    status_t err = audioSource->initCheck();

    if (err != OK) {
        ALOGE("audio source is not initialized");
        return NULL;
    }

    sp<AMessage> format = new AMessage;
    switch (mAudioEncoder) {
        case AUDIO_ENCODER_AMR_NB:
        case AUDIO_ENCODER_DEFAULT:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_AMR_NB);
            break;
        case AUDIO_ENCODER_AMR_WB:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_AMR_WB);
            break;
        case AUDIO_ENCODER_AAC:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_AAC);
            format->setInt32("aac-profile", OMX_AUDIO_AACObjectLC);
            break;
        case AUDIO_ENCODER_HE_AAC:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_AAC);
            format->setInt32("aac-profile", OMX_AUDIO_AACObjectHE);
            break;
        case AUDIO_ENCODER_AAC_ELD:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_AAC);
            format->setInt32("aac-profile", OMX_AUDIO_AACObjectELD);
            break;
        case AUDIO_ENCODER_OPUS:
            format->setString("mime", MEDIA_MIMETYPE_AUDIO_OPUS);
            break;

        default:
            ALOGE("Unknown audio encoder: %d", mAudioEncoder);
            return NULL;
    }

    // log audio mime type for media metrics
    if (mMetricsItem != NULL) {
        AString audiomime;
        if (format->findString("mime", &audiomime)) {
            mMetricsItem->setCString(kRecorderAudioMime, audiomime.c_str());
        }
    }

    int32_t maxInputSize;
    CHECK(audioSource->getFormat()->findInt32(
                kKeyMaxInputSize, &maxInputSize));

    format->setInt32("max-input-size", maxInputSize);
    format->setInt32("channel-count", mAudioChannels);
    format->setInt32("sample-rate", mSampleRate);
    format->setInt32("bitrate", mAudioBitRate);
    if (mAudioTimeScale > 0) {
        format->setInt32("time-scale", mAudioTimeScale);
    }
    format->setInt32("priority", 0 /* realtime */);

    sp<MediaCodecSource> audioEncoder =
            MediaCodecSource::Create(mLooper, format, audioSource);
    sp<AudioSystem::AudioDeviceCallback> callback = mAudioDeviceCallback.promote();
    if (mDeviceCallbackEnabled && callback != 0) {
        audioSource->addAudioDeviceCallback(callback);
    }
    mAudioSourceNode = audioSource;

    if (audioEncoder == NULL) {
        ALOGE("Failed to create audio encoder");
    }

    return audioEncoder;
}

status_t StagefrightRecorder::setupAACRecording() {
    // TODO(b/324512842): Add support for OUTPUT_FORMAT_AAC_ADIF
    if (mOutputFormat != OUTPUT_FORMAT_AAC_ADTS) {
        ALOGE("Invalid output format %d used for AAC recording", mOutputFormat);
        return BAD_VALUE;
    }

    if (mAudioEncoder != AUDIO_ENCODER_AAC
            && mAudioEncoder != AUDIO_ENCODER_HE_AAC
            && mAudioEncoder != AUDIO_ENCODER_AAC_ELD) {
        ALOGE("Invalid encoder %d used for AAC recording", mAudioEncoder);
        return BAD_VALUE;
    }

    if (mAudioSource == AUDIO_SOURCE_CNT) {
        ALOGE("Audio source hasn't been set correctly");
        return BAD_VALUE;
    }

    mWriter = new AACWriter(mOutputFd);
    return setupRawAudioRecording();
}

status_t StagefrightRecorder::setupOggRecording() {
    if (mOutputFormat != OUTPUT_FORMAT_OGG) {
        ALOGE("Invalid output format %d used for OGG recording", mOutputFormat);
        return BAD_VALUE;
    }

    mWriter = new OggWriter(mOutputFd);
    return setupRawAudioRecording();
}

status_t StagefrightRecorder::setupAMRRecording() {
    if (mOutputFormat != OUTPUT_FORMAT_AMR_NB
            && mOutputFormat != OUTPUT_FORMAT_AMR_WB) {
        ALOGE("Invalid output format %d used for AMR recording", mOutputFormat);
        return BAD_VALUE;
    }

    if (mOutputFormat == OUTPUT_FORMAT_AMR_NB) {
        if (mAudioEncoder != AUDIO_ENCODER_DEFAULT &&
            mAudioEncoder != AUDIO_ENCODER_AMR_NB) {
            ALOGE("Invalid encoder %d used for AMRNB recording",
                    mAudioEncoder);
            return BAD_VALUE;
        }
    } else {  // mOutputFormat must be OUTPUT_FORMAT_AMR_WB
        if (mAudioEncoder != AUDIO_ENCODER_AMR_WB) {
            ALOGE("Invlaid encoder %d used for AMRWB recording",
                    mAudioEncoder);
            return BAD_VALUE;
        }
    }

    mWriter = new AMRWriter(mOutputFd);
    return setupRawAudioRecording();
}

status_t StagefrightRecorder::setupRawAudioRecording() {
    if (mAudioSource >= AUDIO_SOURCE_CNT && mAudioSource != AUDIO_SOURCE_FM_TUNER) {
        ALOGE("Invalid audio source: %d", mAudioSource);
        return BAD_VALUE;
    }

    status_t status = BAD_VALUE;
    if (OK != (status = checkAudioEncoderCapabilities())) {
        return status;
    }

    sp<MediaCodecSource> audioEncoder = createAudioSource();
    if (audioEncoder == NULL) {
        return UNKNOWN_ERROR;
    }

    CHECK(mWriter != 0);
    mWriter->addSource(audioEncoder);
    mAudioEncoderSource = audioEncoder;

    if (mMaxFileDurationUs != 0) {
        mWriter->setMaxFileDuration(mMaxFileDurationUs);
    }
    if (mMaxFileSizeBytes != 0) {
        mWriter->setMaxFileSize(mMaxFileSizeBytes);
    }
    mWriter->setListener(mListener);

    return OK;
}

status_t StagefrightRecorder::setupRTPRecording() {
    if (mOutputFormat != OUTPUT_FORMAT_RTP_AVP) {
        ALOGE("Invalid output format %d used for RTP recording", mOutputFormat);
        return BAD_VALUE;
    }

    if ((mAudioSource != AUDIO_SOURCE_CNT
                && mVideoSource != VIDEO_SOURCE_LIST_END)
            || (mAudioSource == AUDIO_SOURCE_CNT
                && mVideoSource == VIDEO_SOURCE_LIST_END)) {
        // Must have exactly one source.
        return BAD_VALUE;
    }

    if (mOutputFd < 0) {
        return BAD_VALUE;
    }

    sp<MediaCodecSource> source;

    if (mAudioSource != AUDIO_SOURCE_CNT) {
        source = createAudioSource();
        if (source == NULL) {
            return UNKNOWN_ERROR;
        }
        mAudioEncoderSource = source;
    } else {
        setDefaultVideoEncoderIfNecessary();

        sp<MediaSource> mediaSource;
        status_t err = setupMediaSource(&mediaSource);
        if (err != OK) {
            return err;
        }

        err = setupVideoEncoder(mediaSource, &source);
        if (err != OK) {
            return err;
        }
        mVideoEncoderSource = source;
    }

    mWriter = new ARTPWriter(mOutputFd, mLocalIp, mLocalPort, mRemoteIp, mRemotePort, mLastSeqNo);
    mWriter->addSource(source);
    mWriter->setListener(mListener);

    return OK;
}

status_t StagefrightRecorder::setupMPEG2TSRecording() {
    if (mOutputFormat != OUTPUT_FORMAT_MPEG2TS) {
        ALOGE("Invalid output format %d used for MPEG2TS recording", mOutputFormat);
        return BAD_VALUE;
    }

    sp<MediaWriter> writer = new MPEG2TSWriter(mOutputFd);

    if (mAudioSource != AUDIO_SOURCE_CNT) {
        if (mAudioEncoder != AUDIO_ENCODER_AAC &&
            mAudioEncoder != AUDIO_ENCODER_HE_AAC &&
            mAudioEncoder != AUDIO_ENCODER_AAC_ELD) {
            return ERROR_UNSUPPORTED;
        }

        status_t err = setupAudioEncoder(writer);

        if (err != OK) {
            return err;
        }
    }

    if (mVideoSource < VIDEO_SOURCE_LIST_END) {
        if (mVideoEncoder != VIDEO_ENCODER_H264) {
            ALOGE("MPEG2TS recording only supports H.264 encoding!");
            return ERROR_UNSUPPORTED;
        }

        sp<MediaSource> mediaSource;
        status_t err = setupMediaSource(&mediaSource);
        if (err != OK) {
            return err;
        }

        sp<MediaCodecSource> encoder;
        err = setupVideoEncoder(mediaSource, &encoder);

        if (err != OK) {
            return err;
        }

        writer->addSource(encoder);
        mVideoEncoderSource = encoder;
    }

    if (mMaxFileDurationUs != 0) {
        writer->setMaxFileDuration(mMaxFileDurationUs);
    }

    if (mMaxFileSizeBytes != 0) {
        writer->setMaxFileSize(mMaxFileSizeBytes);
    }

    mWriter = writer;

    return OK;
}

void StagefrightRecorder::clipVideoFrameRate() {
    ALOGV("clipVideoFrameRate: encoder %d", mVideoEncoder);
    if (mFrameRate == -1) {
        mFrameRate = mEncoderProfiles->getCamcorderProfileParamByName(
                "vid.fps", mCameraId, CAMCORDER_QUALITY_LOW);
        ALOGW("Using default video fps %d", mFrameRate);
    }

    int minFrameRate = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.fps.min", mVideoEncoder);
    int maxFrameRate = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.fps.max", mVideoEncoder);
    if (mFrameRate < minFrameRate && minFrameRate != -1) {
        ALOGW("Intended video encoding frame rate (%d fps) is too small"
             " and will be set to (%d fps)", mFrameRate, minFrameRate);
        mFrameRate = minFrameRate;
    } else if (mFrameRate > maxFrameRate && maxFrameRate != -1) {
        ALOGW("Intended video encoding frame rate (%d fps) is too large"
             " and will be set to (%d fps)", mFrameRate, maxFrameRate);
        mFrameRate = maxFrameRate;
    }
}

void StagefrightRecorder::clipVideoBitRate() {
    ALOGV("clipVideoBitRate: encoder %d", mVideoEncoder);
    int minBitRate = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.bps.min", mVideoEncoder);
    int maxBitRate = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.bps.max", mVideoEncoder);
    if (mVideoBitRate < minBitRate && minBitRate != -1) {
        ALOGW("Intended video encoding bit rate (%d bps) is too small"
             " and will be set to (%d bps)", mVideoBitRate, minBitRate);
        mVideoBitRate = minBitRate;
    } else if (mVideoBitRate > maxBitRate && maxBitRate != -1) {
        ALOGW("Intended video encoding bit rate (%d bps) is too large"
             " and will be set to (%d bps)", mVideoBitRate, maxBitRate);
        mVideoBitRate = maxBitRate;
    }
}

void StagefrightRecorder::clipVideoFrameWidth() {
    ALOGV("clipVideoFrameWidth: encoder %d", mVideoEncoder);
    int minFrameWidth = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.width.min", mVideoEncoder);
    int maxFrameWidth = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.width.max", mVideoEncoder);
    if (mVideoWidth < minFrameWidth && minFrameWidth != -1) {
        ALOGW("Intended video encoding frame width (%d) is too small"
             " and will be set to (%d)", mVideoWidth, minFrameWidth);
        mVideoWidth = minFrameWidth;
    } else if (mVideoWidth > maxFrameWidth && maxFrameWidth != -1) {
        ALOGW("Intended video encoding frame width (%d) is too large"
             " and will be set to (%d)", mVideoWidth, maxFrameWidth);
        mVideoWidth = maxFrameWidth;
    }
}

status_t StagefrightRecorder::checkVideoEncoderCapabilities() {
    if (!mCaptureFpsEnable) {
        // Dont clip for time lapse capture as encoder will have enough
        // time to encode because of slow capture rate of time lapse.
        clipVideoBitRate();
        clipVideoFrameRate();
        clipVideoFrameWidth();
        clipVideoFrameHeight();
        setDefaultProfileIfNecessary();
    }
    return OK;
}

// Set to use AVC baseline profile if the encoding parameters matches
// CAMCORDER_QUALITY_LOW profile; this is for the sake of MMS service.
void StagefrightRecorder::setDefaultProfileIfNecessary() {
    ALOGV("setDefaultProfileIfNecessary");

    camcorder_quality quality = CAMCORDER_QUALITY_LOW;

    int64_t durationUs   = mEncoderProfiles->getCamcorderProfileParamByName(
                                "duration", mCameraId, quality) * 1000000LL;

    int fileFormat       = mEncoderProfiles->getCamcorderProfileParamByName(
                                "file.format", mCameraId, quality);

    int videoCodec       = mEncoderProfiles->getCamcorderProfileParamByName(
                                "vid.codec", mCameraId, quality);

    int videoBitRate     = mEncoderProfiles->getCamcorderProfileParamByName(
                                "vid.bps", mCameraId, quality);

    int videoFrameRate   = mEncoderProfiles->getCamcorderProfileParamByName(
                                "vid.fps", mCameraId, quality);

    int videoFrameWidth  = mEncoderProfiles->getCamcorderProfileParamByName(
                                "vid.width", mCameraId, quality);

    int videoFrameHeight = mEncoderProfiles->getCamcorderProfileParamByName(
                                "vid.height", mCameraId, quality);

    int audioCodec       = mEncoderProfiles->getCamcorderProfileParamByName(
                                "aud.codec", mCameraId, quality);

    int audioBitRate     = mEncoderProfiles->getCamcorderProfileParamByName(
                                "aud.bps", mCameraId, quality);

    int audioSampleRate  = mEncoderProfiles->getCamcorderProfileParamByName(
                                "aud.hz", mCameraId, quality);

    int audioChannels    = mEncoderProfiles->getCamcorderProfileParamByName(
                                "aud.ch", mCameraId, quality);

    if (durationUs == mMaxFileDurationUs &&
        fileFormat == mOutputFormat &&
        videoCodec == mVideoEncoder &&
        videoBitRate == mVideoBitRate &&
        videoFrameRate == mFrameRate &&
        videoFrameWidth == mVideoWidth &&
        videoFrameHeight == mVideoHeight &&
        audioCodec == mAudioEncoder &&
        audioBitRate == mAudioBitRate &&
        audioSampleRate == mSampleRate &&
        audioChannels == mAudioChannels) {
        if (videoCodec == VIDEO_ENCODER_H264) {
            ALOGI("Force to use AVC baseline profile");
            setParamVideoEncoderProfile(OMX_VIDEO_AVCProfileBaseline);
            // set 0 for invalid levels - this will be rejected by the
            // codec if it cannot handle it during configure
            setParamVideoEncoderLevel(ACodec::getAVCLevelFor(
                    videoFrameWidth, videoFrameHeight, videoFrameRate, videoBitRate));
        }
    }
}

void StagefrightRecorder::setDefaultVideoEncoderIfNecessary() {
    if (mVideoEncoder == VIDEO_ENCODER_DEFAULT) {
        if (mOutputFormat == OUTPUT_FORMAT_WEBM) {
            // default to VP8 for WEBM recording
            mVideoEncoder = VIDEO_ENCODER_VP8;
        } else {
            // pick the default encoder for CAMCORDER_QUALITY_LOW
            int videoCodec = mEncoderProfiles->getCamcorderProfileParamByName(
                    "vid.codec", mCameraId, CAMCORDER_QUALITY_LOW);

            if (videoCodec > VIDEO_ENCODER_DEFAULT &&
                videoCodec < VIDEO_ENCODER_LIST_END) {
                mVideoEncoder = (video_encoder)videoCodec;
            } else {
                // default to H.264 if camcorder profile not available
                mVideoEncoder = VIDEO_ENCODER_H264;
            }
        }
    }
}

status_t StagefrightRecorder::checkAudioEncoderCapabilities() {
    clipAudioBitRate();
    clipAudioSampleRate();
    clipNumberOfAudioChannels();
    return OK;
}

void StagefrightRecorder::clipAudioBitRate() {
    ALOGV("clipAudioBitRate: encoder %d", mAudioEncoder);

    int minAudioBitRate =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.bps.min", mAudioEncoder);
    if (minAudioBitRate != -1 && mAudioBitRate < minAudioBitRate) {
        ALOGW("Intended audio encoding bit rate (%d) is too small"
            " and will be set to (%d)", mAudioBitRate, minAudioBitRate);
        mAudioBitRate = minAudioBitRate;
    }

    int maxAudioBitRate =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.bps.max", mAudioEncoder);
    if (maxAudioBitRate != -1 && mAudioBitRate > maxAudioBitRate) {
        ALOGW("Intended audio encoding bit rate (%d) is too large"
            " and will be set to (%d)", mAudioBitRate, maxAudioBitRate);
        mAudioBitRate = maxAudioBitRate;
    }
}

void StagefrightRecorder::clipAudioSampleRate() {
    ALOGV("clipAudioSampleRate: encoder %d", mAudioEncoder);

    int minSampleRate =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.hz.min", mAudioEncoder);
    if (minSampleRate != -1 && mSampleRate < minSampleRate) {
        ALOGW("Intended audio sample rate (%d) is too small"
            " and will be set to (%d)", mSampleRate, minSampleRate);
        mSampleRate = minSampleRate;
    }

    int maxSampleRate =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.hz.max", mAudioEncoder);
    if (maxSampleRate != -1 && mSampleRate > maxSampleRate) {
        ALOGW("Intended audio sample rate (%d) is too large"
            " and will be set to (%d)", mSampleRate, maxSampleRate);
        mSampleRate = maxSampleRate;
    }
}

void StagefrightRecorder::clipNumberOfAudioChannels() {
    ALOGV("clipNumberOfAudioChannels: encoder %d", mAudioEncoder);

    int minChannels =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.ch.min", mAudioEncoder);
    if (minChannels != -1 && mAudioChannels < minChannels) {
        ALOGW("Intended number of audio channels (%d) is too small"
            " and will be set to (%d)", mAudioChannels, minChannels);
        mAudioChannels = minChannels;
    }

    int maxChannels =
            mEncoderProfiles->getAudioEncoderParamByName(
                "enc.aud.ch.max", mAudioEncoder);
    if (maxChannels != -1 && mAudioChannels > maxChannels) {
        ALOGW("Intended number of audio channels (%d) is too large"
            " and will be set to (%d)", mAudioChannels, maxChannels);
        mAudioChannels = maxChannels;
    }
}

void StagefrightRecorder::clipVideoFrameHeight() {
    ALOGV("clipVideoFrameHeight: encoder %d", mVideoEncoder);
    int minFrameHeight = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.height.min", mVideoEncoder);
    int maxFrameHeight = mEncoderProfiles->getVideoEncoderParamByName(
                        "enc.vid.height.max", mVideoEncoder);
    if (minFrameHeight != -1 && mVideoHeight < minFrameHeight) {
        ALOGW("Intended video encoding frame height (%d) is too small"
             " and will be set to (%d)", mVideoHeight, minFrameHeight);
        mVideoHeight = minFrameHeight;
    } else if (maxFrameHeight != -1 && mVideoHeight > maxFrameHeight) {
        ALOGW("Intended video encoding frame height (%d) is too large"
             " and will be set to (%d)", mVideoHeight, maxFrameHeight);
        mVideoHeight = maxFrameHeight;
    }
}

// Set up the appropriate MediaSource depending on the chosen option
status_t StagefrightRecorder::setupMediaSource(
                      sp<MediaSource> *mediaSource) {
    ATRACE_CALL();
    if (mVideoSource == VIDEO_SOURCE_DEFAULT
            || mVideoSource == VIDEO_SOURCE_CAMERA) {
        sp<CameraSource> cameraSource;
        status_t err = setupCameraSource(&cameraSource);
        if (err != OK) {
            return err;
        }
        *mediaSource = cameraSource;
    } else if (mVideoSource == VIDEO_SOURCE_SURFACE) {
        *mediaSource = NULL;
    } else {
        return INVALID_OPERATION;
    }
    return OK;
}

status_t StagefrightRecorder::setupCameraSource(
        sp<CameraSource> *cameraSource) {
    status_t err = OK;
    if ((err = checkVideoEncoderCapabilities()) != OK) {
        return err;
    }
    Size videoSize;
    videoSize.width = mVideoWidth;
    videoSize.height = mVideoHeight;
    uid_t uid = VALUE_OR_RETURN_STATUS(aidl2legacy_int32_t_uid_t(mAttributionSource.uid));
    pid_t pid = VALUE_OR_RETURN_STATUS(aidl2legacy_int32_t_pid_t(mAttributionSource.pid));
    String16 clientName = VALUE_OR_RETURN_STATUS(
        aidl2legacy_string_view_String16(mAttributionSource.packageName.value_or("")));
    if (mCaptureFpsEnable && mCaptureFps != mFrameRate) {
        if (!(mCaptureFps > 0.)) {
            ALOGE("Invalid mCaptureFps value: %lf", mCaptureFps);
            return BAD_VALUE;
        }

#if WB_LIBCAMERASERVICE_WITH_DEPENDENCIES
        sp<Surface> surface = new Surface(mPreviewSurface);
        mCameraSourceTimeLapse = CameraSourceTimeLapse::CreateFromCamera(
                mCamera, mCameraProxy, mCameraId, clientName, uid, pid,
                videoSize, mFrameRate, surface,
                std::llround(1e6 / mCaptureFps));
#else
        mCameraSourceTimeLapse = CameraSourceTimeLapse::CreateFromCamera(
                mCamera, mCameraProxy, mCameraId, clientName, uid, pid,
                videoSize, mFrameRate, mPreviewSurface,
                std::llround(1e6 / mCaptureFps));
#endif
        *cameraSource = mCameraSourceTimeLapse;
    } else {
#if WB_LIBCAMERASERVICE_WITH_DEPENDENCIES
        sp<Surface> surface = new Surface(mPreviewSurface);
        *cameraSource = CameraSource::CreateFromCamera(
                mCamera, mCameraProxy, mCameraId, clientName, uid, pid,
                videoSize, mFrameRate,
                surface);
#else
        *cameraSource = CameraSource::CreateFromCamera(
                mCamera, mCameraProxy, mCameraId, clientName, uid, pid,
                videoSize, mFrameRate,
                mPreviewSurface);
#endif
    }
    mCamera.clear();
    mCameraProxy.clear();
    if (*cameraSource == NULL) {
        return UNKNOWN_ERROR;
    }

    if ((*cameraSource)->initCheck() != OK) {
        (*cameraSource).clear();
        *cameraSource = NULL;
        return NO_INIT;
    }

    // When frame rate is not set, the actual frame rate will be set to
    // the current frame rate being used.
    if (mFrameRate == -1) {
        int32_t frameRate = 0;
        CHECK ((*cameraSource)->getFormat()->findInt32(
                    kKeyFrameRate, &frameRate));
        ALOGI("Frame rate is not explicitly set. Use the current frame "
             "rate (%d fps)", frameRate);
        mFrameRate = frameRate;
    }

    CHECK(mFrameRate != -1);

    mMetaDataStoredInVideoBuffers =
        (*cameraSource)->metaDataStoredInVideoBuffers();

    return OK;
}

status_t StagefrightRecorder::setupVideoEncoder(
        const sp<MediaSource> &cameraSource,
        sp<MediaCodecSource> *source) {
    ATRACE_CALL();
    source->clear();

    sp<AMessage> format = new AMessage();

    switch (mVideoEncoder) {
        case VIDEO_ENCODER_H263:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_H263);
            break;

        case VIDEO_ENCODER_MPEG_4_SP:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_MPEG4);
            break;

        case VIDEO_ENCODER_H264:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_AVC);
            break;

        case VIDEO_ENCODER_VP8:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_VP8);
            break;

        case VIDEO_ENCODER_HEVC:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_HEVC);
            break;

        case VIDEO_ENCODER_DOLBY_VISION:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_DOLBY_VISION);
            break;

        case VIDEO_ENCODER_AV1:
            format->setString("mime", MEDIA_MIMETYPE_VIDEO_AV1);
            break;

        default:
            CHECK(!"Should not be here, unsupported video encoding.");
            break;
    }

    // log video mime type for media metrics
    if (mMetricsItem != NULL) {
        AString videomime;
        if (format->findString("mime", &videomime)) {
            mMetricsItem->setCString(kRecorderVideoMime, videomime.c_str());
        }
    }

    if (cameraSource != NULL) {
        sp<MetaData> meta = cameraSource->getFormat();

        int32_t width, height, stride, sliceHeight, colorFormat;
        CHECK(meta->findInt32(kKeyWidth, &width));
        CHECK(meta->findInt32(kKeyHeight, &height));
        CHECK(meta->findInt32(kKeyStride, &stride));
        CHECK(meta->findInt32(kKeySliceHeight, &sliceHeight));
        CHECK(meta->findInt32(kKeyColorFormat, &colorFormat));

        format->setInt32("width", width);
        format->setInt32("height", height);
        format->setInt32("stride", stride);
        format->setInt32("slice-height", sliceHeight);
        format->setInt32("color-format", colorFormat);
    } else {
        format->setInt32("width", mVideoWidth);
        format->setInt32("height", mVideoHeight);
        format->setInt32("stride", mVideoWidth);
        format->setInt32("slice-height", mVideoHeight);
        format->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);

        // set up time lapse/slow motion for surface source
        if (mCaptureFpsEnable) {
            if (!(mCaptureFps > 0.)) {
                ALOGE("Invalid mCaptureFps value: %lf", mCaptureFps);
                return BAD_VALUE;
            }
            format->setDouble("time-lapse-fps", mCaptureFps);
        }
    }

    if (mOutputFormat == OUTPUT_FORMAT_RTP_AVP) {
        // This indicates that a raw image provided to encoder needs to be rotated.
        format->setInt32("rotation-degrees", mRotationDegrees);
    }

    format->setInt32("bitrate", mVideoBitRate);
    format->setInt32("bitrate-mode", mVideoBitRateMode);
    format->setInt32("frame-rate", mFrameRate);
    format->setInt32("i-frame-interval", mIFramesIntervalSec);

    if (mVideoTimeScale > 0) {
        format->setInt32("time-scale", mVideoTimeScale);
    }
    if (mVideoEncoderProfile != -1) {
        format->setInt32("profile", mVideoEncoderProfile);
    }
    if (mVideoEncoderLevel != -1) {
        format->setInt32("level", mVideoEncoderLevel);
    }

    uint32_t tsLayers = 1;
    bool preferBFrames = true; // we like B-frames as it produces better quality per bitrate
    format->setInt32("priority", 0 /* realtime */);
    float maxPlaybackFps = mFrameRate; // assume video is only played back at normal speed

    if (mCaptureFpsEnable) {
        format->setFloat("operating-rate", mCaptureFps);

        // enable layering for all time lapse and high frame rate recordings
        if (mFrameRate / mCaptureFps >= 1.9) { // time lapse
            preferBFrames = false;
            tsLayers = 2; // use at least two layers as resulting video will likely be sped up
        } else if (mCaptureFps > maxPlaybackFps) { // slow-mo
            format->setInt32("high-frame-rate", 1);
            maxPlaybackFps = mCaptureFps; // assume video will be played back at full capture speed
            preferBFrames = false;
        }
    }

    // Enable temporal layering if the expected (max) playback frame rate is greater than ~11% of
    // the minimum display refresh rate on a typical device. Add layers until the base layer falls
    // under this limit. Allow device manufacturers to override this limit.

    // TODO: make this configurable by the application
    std::string maxBaseLayerFpsProperty =
        ::android::base::GetProperty("ro.media.recorder-max-base-layer-fps", "");
    float maxBaseLayerFps = (float)::atof(maxBaseLayerFpsProperty.c_str());
    // TRICKY: use !> to fix up any NaN values
    if (!(maxBaseLayerFps >= kMinTypicalDisplayRefreshingRate / 0.9)) {
        maxBaseLayerFps = kMinTypicalDisplayRefreshingRate / 0.9;
    }

    for (uint32_t tryLayers = 1; tryLayers <= kMaxNumVideoTemporalLayers; ++tryLayers) {
        if (tryLayers > tsLayers) {
            tsLayers = tryLayers;
        }
        // keep going until the base layer fps falls below the typical display refresh rate
        float baseLayerFps = maxPlaybackFps / (1 << (tryLayers - 1));
        if (baseLayerFps < maxBaseLayerFps) {
            break;
        }
    }

    if (tsLayers > 1) {
        uint32_t bLayers = std::min(2u, tsLayers - 1); // use up-to 2 B-layers
        // TODO(b/341121900): Remove this once B frames are handled correctly in screen recorder
        // use case in case of mic only
        if (!com::android::media::editing::flags::stagefrightrecorder_enable_b_frames()
                && mAudioSource == AUDIO_SOURCE_MIC && mVideoSource == VIDEO_SOURCE_SURFACE) {
            bLayers = 0;
        }
        uint32_t pLayers = tsLayers - bLayers;
        format->setString(
                "ts-schema", AStringPrintf("android.generic.%u+%u", pLayers, bLayers));

        // TODO: some encoders do not support B-frames with temporal layering, and we have a
        // different preference based on use-case. We could move this into camera profiles.
        format->setInt32("android._prefer-b-frames", preferBFrames);
    }

    if (mMetaDataStoredInVideoBuffers != kMetadataBufferTypeInvalid) {
        format->setInt32("android._input-metadata-buffer-type", mMetaDataStoredInVideoBuffers);
    }

    uint32_t flags = 0;
    if (cameraSource == NULL) {
        flags |= MediaCodecSource::FLAG_USE_SURFACE_INPUT;
    } else {
        // require dataspace setup even if not using surface input
        format->setInt32("android._using-recorder", 1);
    }

    sp<MediaCodecSource> encoder = MediaCodecSource::Create(
            mLooper, format, cameraSource, mPersistentSurface, flags);
    if (encoder == NULL) {
        ALOGE("Failed to create video encoder");
        // When the encoder fails to be created, we need
        // release the camera source due to the camera's lock
        // and unlock mechanism.
        if (cameraSource != NULL) {
            cameraSource->stop();
        }
        return UNKNOWN_ERROR;
    }

    if (cameraSource == NULL) {
        mGraphicBufferProducer = encoder->getGraphicBufferProducer();
    }

    *source = encoder;

    return OK;
}

status_t StagefrightRecorder::setupAudioEncoder(const sp<MediaWriter>& writer) {
    ATRACE_CALL();
    status_t status = BAD_VALUE;
    if (OK != (status = checkAudioEncoderCapabilities())) {
        return status;
    }

    switch(mAudioEncoder) {
        case AUDIO_ENCODER_AMR_NB:
        case AUDIO_ENCODER_AMR_WB:
        case AUDIO_ENCODER_AAC:
        case AUDIO_ENCODER_HE_AAC:
        case AUDIO_ENCODER_AAC_ELD:
        case AUDIO_ENCODER_OPUS:
            break;

        default:
            ALOGE("Unsupported audio encoder: %d", mAudioEncoder);
            return UNKNOWN_ERROR;
    }

    sp<MediaCodecSource> audioEncoder = createAudioSource();
    if (audioEncoder == NULL) {
        return UNKNOWN_ERROR;
    }

    writer->addSource(audioEncoder);
    mAudioEncoderSource = audioEncoder;
    return OK;
}

status_t StagefrightRecorder::setupMPEG4orWEBMRecording() {
    mWriter.clear();
    mTotalBitRate = 0;

    status_t err = OK;
    sp<MediaWriter> writer;
    sp<MPEG4Writer> mp4writer;
    if (mOutputFormat == OUTPUT_FORMAT_WEBM) {
        writer = new WebmWriter(mOutputFd);
    } else {
        writer = mp4writer = new MPEG4Writer(mOutputFd);
    }

    if (mVideoSource < VIDEO_SOURCE_LIST_END) {
        setDefaultVideoEncoderIfNecessary();

        sp<MediaSource> mediaSource;
        err = setupMediaSource(&mediaSource);
        if (err != OK) {
            return err;
        }

        sp<MediaCodecSource> encoder;
        err = setupVideoEncoder(mediaSource, &encoder);
        if (err != OK) {
            return err;
        }

        writer->addSource(encoder);
        mVideoEncoderSource = encoder;
        mTotalBitRate += mVideoBitRate;
    }

    // Audio source is added at the end if it exists.
    // This help make sure that the "recoding" sound is suppressed for
    // camcorder applications in the recorded files.
    // disable audio for time lapse recording
    const bool disableAudio = mCaptureFpsEnable && mCaptureFps < mFrameRate;
    if (!disableAudio && mAudioSource != AUDIO_SOURCE_CNT) {
        err = setupAudioEncoder(writer);
        if (err != OK) return err;
        mTotalBitRate += mAudioBitRate;
    }

    if (mOutputFormat != OUTPUT_FORMAT_WEBM) {
        if (mCaptureFpsEnable) {
            mp4writer->setCaptureRate(mCaptureFps);
        }

        if (mInterleaveDurationUs > 0) {
            mp4writer->setInterleaveDuration(mInterleaveDurationUs);
        }
        if (mLongitudex10000 > -3600000 && mLatitudex10000 > -3600000) {
            mp4writer->setGeoData(mLatitudex10000, mLongitudex10000);
        }
    }
    if (mMaxFileDurationUs != 0) {
        writer->setMaxFileDuration(mMaxFileDurationUs);
    }
    if (mMaxFileSizeBytes != 0) {
        writer->setMaxFileSize(mMaxFileSizeBytes);
    }
    if (mVideoSource == VIDEO_SOURCE_DEFAULT
            || mVideoSource == VIDEO_SOURCE_CAMERA) {
        mStartTimeOffsetMs = mEncoderProfiles->getStartTimeOffsetMs(mCameraId);
    } else if (mVideoSource == VIDEO_SOURCE_SURFACE) {
        // surface source doesn't need large initial delay
        mStartTimeOffsetMs = 100;
    }
    if (mStartTimeOffsetMs > 0) {
        writer->setStartTimeOffsetMs(mStartTimeOffsetMs);
    }

    writer->setListener(mListener);
    mWriter = writer;
    return OK;
}

void StagefrightRecorder::setupMPEG4orWEBMMetaData(sp<MetaData> *meta) {
    int64_t startTimeUs = systemTime() / 1000;
    (*meta)->setInt64(kKeyTime, startTimeUs);
    (*meta)->setInt32(kKeyFileType, mOutputFormat);
    (*meta)->setInt32(kKeyBitRate, mTotalBitRate);
    if (mMovieTimeScale > 0) {
        (*meta)->setInt32(kKeyTimeScale, mMovieTimeScale);
    }
    if (mOutputFormat != OUTPUT_FORMAT_WEBM) {
        if (mTrackEveryTimeDurationUs > 0) {
            (*meta)->setInt64(kKeyTrackTimeStatus, mTrackEveryTimeDurationUs);
        }
        if (mRotationDegrees != 0) {
            (*meta)->setInt32(kKeyRotation, mRotationDegrees);
        }
    }
    if (mOutputFormat == OUTPUT_FORMAT_MPEG_4 || mOutputFormat == OUTPUT_FORMAT_THREE_GPP) {
        (*meta)->setInt32(kKeyEmptyTrackMalFormed, true);
        (*meta)->setInt32(kKey4BitTrackIds, true);
    }
}

status_t StagefrightRecorder::pause() {
    ALOGV("pause");
    if (!mStarted) {
        return INVALID_OPERATION;
    }

    // Already paused --- no-op.
    if (mPauseStartTimeUs != 0) {
        return OK;
    }

    mPauseStartTimeUs = systemTime() / 1000;
    sp<MetaData> meta = new MetaData;
    meta->setInt64(kKeyTime, mPauseStartTimeUs);

    if (mStartedRecordingUs != 0) {
        // should always be true
        int64_t recordingUs = mPauseStartTimeUs - mStartedRecordingUs;
        mDurationRecordedUs += recordingUs;
        mStartedRecordingUs = 0;
    }

    if (mAudioEncoderSource != NULL) {
        mAudioEncoderSource->pause();
    }
    if (mVideoEncoderSource != NULL) {
        mVideoEncoderSource->pause(meta.get());
    }

    return OK;
}

status_t StagefrightRecorder::resume() {
    ALOGV("resume");
    if (!mStarted) {
        return INVALID_OPERATION;
    }

    // Not paused --- no-op.
    if (mPauseStartTimeUs == 0) {
        return OK;
    }

    int64_t resumeStartTimeUs = systemTime() / 1000;

    int64_t bufferStartTimeUs = 0;
    bool allSourcesStarted = true;
    for (const auto &source : { mAudioEncoderSource, mVideoEncoderSource }) {
        if (source == nullptr) {
            continue;
        }
        int64_t timeUs = source->getFirstSampleSystemTimeUs();
        if (timeUs < 0) {
            allSourcesStarted = false;
        }
        if (bufferStartTimeUs < timeUs) {
            bufferStartTimeUs = timeUs;
        }
    }

    if (allSourcesStarted) {
        if (mPauseStartTimeUs < bufferStartTimeUs) {
            mPauseStartTimeUs = bufferStartTimeUs;
        }
        // 30 ms buffer to avoid timestamp overlap
        mTotalPausedDurationUs += resumeStartTimeUs - mPauseStartTimeUs - 30000;
    }
    double timeOffset = -mTotalPausedDurationUs;
    if (mCaptureFpsEnable && (mVideoSource == VIDEO_SOURCE_CAMERA)) {
        timeOffset *= mCaptureFps / mFrameRate;
    }
    sp<MetaData> meta = new MetaData;
    meta->setInt64(kKeyTime, resumeStartTimeUs);
    for (const auto &source : { mAudioEncoderSource, mVideoEncoderSource }) {
        if (source == nullptr) {
            continue;
        }
        source->setInputBufferTimeOffset((int64_t)timeOffset);
        source->start(meta.get());
    }


    // sum info on pause duration
    // (ignore the 30msec of overlap adjustment factored into mTotalPausedDurationUs)
    int64_t pausedUs = resumeStartTimeUs - mPauseStartTimeUs;
    mDurationPausedUs += pausedUs;
    mNPauses++;
    // and a timestamp marking that we're back to recording....
    mStartedRecordingUs = resumeStartTimeUs;

    mPauseStartTimeUs = 0;

    return OK;
}

status_t StagefrightRecorder::stop() {
    ALOGV("stop");
    Mutex::Autolock autolock(mLock);
    status_t err = OK;

    if (mCaptureFpsEnable && mCameraSourceTimeLapse != NULL) {
        mCameraSourceTimeLapse->startQuickReadReturns();
        mCameraSourceTimeLapse = NULL;
    }

    int64_t stopTimeUs = systemTime() / 1000;
    for (const auto &source : { mAudioEncoderSource, mVideoEncoderSource }) {
        if (source != nullptr && OK != source->setStopTimeUs(stopTimeUs)) {
            ALOGW("Failed to set stopTime %lld us for %s",
                    (long long)stopTimeUs, source->isVideo() ? "Video" : "Audio");
        }
    }

    if (mWriter != NULL) {
        err = mWriter->stop();
        mLastSeqNo = mWriter->getSequenceNum();
        mWriter.clear();
    }

    // account for the last 'segment' -- whether paused or recording
    if (mPauseStartTimeUs != 0) {
        // we were paused
        int64_t additive = stopTimeUs - mPauseStartTimeUs;
        mDurationPausedUs += additive;
        mNPauses++;
    } else if (mStartedRecordingUs != 0) {
        // we were recording
        int64_t additive = stopTimeUs - mStartedRecordingUs;
        mDurationRecordedUs += additive;
    } else {
        ALOGW("stop while neither recording nor paused");
    }

    flushAndResetMetrics(true);

    mDurationRecordedUs = 0;
    mDurationPausedUs = 0;
    mNPauses = 0;
    mTotalPausedDurationUs = 0;
    mPauseStartTimeUs = 0;
    mStartedRecordingUs = 0;

    mGraphicBufferProducer.clear();
    mPersistentSurface.clear();
    mAudioEncoderSource.clear();
    mVideoEncoderSource.clear();

    if (mOutputFd >= 0) {
        ::close(mOutputFd);
        mOutputFd = -1;
    }

    if (mStarted) {
        mStarted = false;

        uint32_t params = 0;
        if (mAudioSource != AUDIO_SOURCE_CNT) {
            params |= IMediaPlayerService::kBatteryDataTrackAudio;
        }
        if (mVideoSource != VIDEO_SOURCE_LIST_END) {
            params |= IMediaPlayerService::kBatteryDataTrackVideo;
        }

        addBatteryData(params);
    }

    return err;
}

status_t StagefrightRecorder::close() {
    ALOGV("close");
    stop();

    return OK;
}

status_t StagefrightRecorder::reset() {
    ALOGV("reset");
    stop();

    // No audio or video source by default
    mAudioSource = (audio_source_t)AUDIO_SOURCE_CNT; // reset to invalid value
    mVideoSource = VIDEO_SOURCE_LIST_END;

    // Default parameters
    mOutputFormat  = OUTPUT_FORMAT_THREE_GPP;
    mAudioEncoder  = AUDIO_ENCODER_AMR_NB;
    mVideoEncoder  = VIDEO_ENCODER_DEFAULT;
    mVideoWidth    = 176;
    mVideoHeight   = 144;
    mFrameRate     = -1;
    mVideoBitRate  = 192000;
    // Following MediaCodec's default
    mVideoBitRateMode = BITRATE_MODE_VBR;
    mSampleRate    = 8000;
    mAudioChannels = 1;
    mAudioBitRate  = 12200;
    mInterleaveDurationUs = 0;
    mIFramesIntervalSec = 1;
    mAudioSourceNode = 0;
    mUse64BitFileOffset = false;
    mMovieTimeScale  = -1;
    mAudioTimeScale  = -1;
    mVideoTimeScale  = -1;
    mCameraId        = 0;
    mStartTimeOffsetMs = -1;
    mVideoEncoderProfile = -1;
    mVideoEncoderLevel   = -1;
    mMaxFileDurationUs = 0;
    mMaxFileSizeBytes = 0;
    mTrackEveryTimeDurationUs = 0;
    mCaptureFpsEnable = false;
    mCaptureFps = -1.0;
    mCameraSourceTimeLapse = NULL;
    mMetaDataStoredInVideoBuffers = kMetadataBufferTypeInvalid;
    mEncoderProfiles = MediaProfiles::getInstance();
    mRotationDegrees = 0;
    mLatitudex10000 = -3600000;
    mLongitudex10000 = -3600000;
    mTotalBitRate = 0;

    // tracking how long we recorded.
    mDurationRecordedUs = 0;
    mStartedRecordingUs = 0;
    mDurationPausedUs = 0;
    mNPauses = 0;

    mOutputFd = -1;

    return OK;
}

status_t StagefrightRecorder::getMaxAmplitude(int *max) {
    ALOGV("getMaxAmplitude");

    if (max == NULL) {
        ALOGE("Null pointer argument");
        return BAD_VALUE;
    }

    if (mAudioSourceNode != 0) {
        *max = mAudioSourceNode->getMaxAmplitude();
    } else {
        *max = 0;
    }

    return OK;
}

status_t StagefrightRecorder::getMetrics(Parcel *reply) {
    ALOGV("StagefrightRecorder::getMetrics");

    if (reply == NULL) {
        ALOGE("Null pointer argument");
        return BAD_VALUE;
    }

    if (mMetricsItem == NULL) {
        return UNKNOWN_ERROR;
    }

    updateMetrics();
    mMetricsItem->writeToParcel(reply);
    return OK;
}

status_t StagefrightRecorder::setInputDevice(audio_port_handle_t deviceId) {
    ALOGV("setInputDevice");

    if (mSelectedDeviceId != deviceId) {
        mSelectedDeviceId = deviceId;
        if (mAudioSourceNode != 0) {
            return mAudioSourceNode->setInputDevice(deviceId);
        }
    }
    return NO_ERROR;
}

status_t StagefrightRecorder::getRoutedDeviceIds(DeviceIdVector& deviceIds) {
    ALOGV("getRoutedDeviceIds");

    if (mAudioSourceNode != 0) {
        status_t status = mAudioSourceNode->getRoutedDeviceIds(deviceIds);
        return status;
    }
    return NO_INIT;
}

void StagefrightRecorder::setAudioDeviceCallback(
        const sp<AudioSystem::AudioDeviceCallback>& callback) {
    mAudioDeviceCallback = callback;
}

status_t StagefrightRecorder::enableAudioDeviceCallback(bool enabled) {
    mDeviceCallbackEnabled = enabled;
    sp<AudioSystem::AudioDeviceCallback> callback = mAudioDeviceCallback.promote();
    if (mAudioSourceNode != 0 && callback != 0) {
        if (enabled) {
            return mAudioSourceNode->addAudioDeviceCallback(callback);
        } else {
            return mAudioSourceNode->removeAudioDeviceCallback(callback);
        }
    }
    return NO_ERROR;
}

status_t StagefrightRecorder::getActiveMicrophones(
        std::vector<media::MicrophoneInfoFw>* activeMicrophones) {
    if (mAudioSourceNode != 0) {
        return mAudioSourceNode->getActiveMicrophones(activeMicrophones);
    }
    return NO_INIT;
}

status_t StagefrightRecorder::setPreferredMicrophoneDirection(audio_microphone_direction_t direction) {
    ALOGV("setPreferredMicrophoneDirection(%d)", direction);
    mSelectedMicDirection = direction;
    if (mAudioSourceNode != 0) {
        return mAudioSourceNode->setPreferredMicrophoneDirection(direction);
    }
    return NO_INIT;
}

status_t StagefrightRecorder::setPreferredMicrophoneFieldDimension(float zoom) {
    ALOGV("setPreferredMicrophoneFieldDimension(%f)", zoom);
    mSelectedMicFieldDimension = zoom;
    if (mAudioSourceNode != 0) {
        return mAudioSourceNode->setPreferredMicrophoneFieldDimension(zoom);
    }
    return NO_INIT;
}

status_t StagefrightRecorder::getPortId(audio_port_handle_t *portId) const {
    if (mAudioSourceNode != 0) {
        return mAudioSourceNode->getPortId(portId);
    }
    return NO_INIT;
}

status_t StagefrightRecorder::getRtpDataUsage(uint64_t *bytes) {
    if (mWriter != 0) {
        *bytes = mWriter->getAccumulativeBytes();
        return OK;
    }
    return NO_INIT;
}

status_t StagefrightRecorder::dump(
        int fd, const Vector<String16>& args) const {
    ALOGV("dump");
    Mutex::Autolock autolock(mLock);
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    if (mWriter != 0) {
        mWriter->dump(fd, args);
    } else {
        snprintf(buffer, SIZE, "   No file writer\n");
        result.append(buffer);
    }
    snprintf(buffer, SIZE, "   Recorder: %p\n", this);
    snprintf(buffer, SIZE, "   Output file (fd %d):\n", mOutputFd);
    result.append(buffer);
    snprintf(buffer, SIZE, "     File format: %d\n", mOutputFormat);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Max file size (bytes): %" PRId64 "\n", mMaxFileSizeBytes);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Max file duration (us): %" PRId64 "\n", mMaxFileDurationUs);
    result.append(buffer);
    snprintf(buffer, SIZE, "     File offset length (bits): %d\n", mUse64BitFileOffset? 64: 32);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Interleave duration (us): %d\n", mInterleaveDurationUs);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Progress notification: %" PRId64 " us\n", mTrackEveryTimeDurationUs);
    result.append(buffer);
    snprintf(buffer, SIZE, "   Audio\n");
    result.append(buffer);
    snprintf(buffer, SIZE, "     Source: %d\n", mAudioSource);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Encoder: %d\n", mAudioEncoder);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Bit rate (bps): %d\n", mAudioBitRate);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Sampling rate (hz): %d\n", mSampleRate);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Number of channels: %d\n", mAudioChannels);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Max amplitude: %d\n", mAudioSourceNode == 0? 0: mAudioSourceNode->getMaxAmplitude());
    result.append(buffer);
    snprintf(buffer, SIZE, "   Video\n");
    result.append(buffer);
    snprintf(buffer, SIZE, "     Source: %d\n", mVideoSource);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Camera Id: %d\n", mCameraId);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Start time offset (ms): %d\n", mStartTimeOffsetMs);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Encoder: %d\n", mVideoEncoder);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Encoder profile: %d\n", mVideoEncoderProfile);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Encoder level: %d\n", mVideoEncoderLevel);
    result.append(buffer);
    snprintf(buffer, SIZE, "     I frames interval (s): %d\n", mIFramesIntervalSec);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Frame size (pixels): %dx%d\n", mVideoWidth, mVideoHeight);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Frame rate (fps): %d\n", mFrameRate);
    result.append(buffer);
    snprintf(buffer, SIZE, "     Bit rate (bps): %d\n", mVideoBitRate);
    result.append(buffer);
    ::write(fd, result.c_str(), result.size());
    return OK;
}
}  // namespace android
