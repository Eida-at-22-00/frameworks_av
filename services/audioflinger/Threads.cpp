/*
**
** Copyright 2012, The Android Open Source Project
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


#define LOG_TAG "AudioFlinger"
// #define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_AUDIO

#include "Threads.h"

#include "Client.h"
#include "IAfEffect.h"
#include "MelReporter.h"
#include "ResamplerBufferProvider.h"

#include <afutils/FallibleLockGuard.h>
#include <afutils/Permission.h>
#include <afutils/TypedLogger.h>
#include <afutils/Vibrator.h>
#include <audio_utils/MelProcessor.h>
#include <audio_utils/Metadata.h>
#include <audio_utils/Trace.h>
#ifdef DEBUG_CPU_USAGE
#include <audio_utils/Statistics.h>
#include <cpustats/ThreadCpuUsage.h>
#endif
#include <audio_utils/channels.h>
#include <audio_utils/format.h>
#include <audio_utils/minifloat.h>
#include <audio_utils/mono_blend.h>
#include <audio_utils/primitives.h>
#include <audio_utils/safe_math.h>
#include <audiomanager/AudioManager.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/PersistableBundle.h>
#include <com_android_media_audio.h>
#include <com_android_media_audioserver.h>
#include <cutils/bitops.h>
#include <cutils/properties.h>
#include <fastpath/AutoPark.h>
#include <media/AudioContainers.h>
#include <media/AudioDeviceTypeAddr.h>
#include <media/AudioParameter.h>
#include <media/AudioResamplerPublic.h>
#ifdef ADD_BATTERY_DATA
#include <media/IMediaPlayerService.h>
#include <media/IMediaDeathNotifier.h>
#endif
#include <media/MmapStreamCallback.h>
#include <media/RecordBufferConverter.h>
#include <media/TypeConverter.h>
#include <media/audiohal/EffectsFactoryHalInterface.h>
#include <media/audiohal/StreamHalInterface.h>
#include <media/nbaio/AudioStreamInSource.h>
#include <media/nbaio/AudioStreamOutSink.h>
#include <media/nbaio/MonoPipe.h>
#include <media/nbaio/MonoPipeReader.h>
#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#include <media/nbaio/SourceAudioBufferProvider.h>
#include <media/ValidatedAttributionSourceState.h>
#include <mediautils/BatteryNotifier.h>
#include <mediautils/Process.h>
#include <mediautils/SchedulingPolicyService.h>
#include <mediautils/ServiceUtilities.h>
#include <powermanager/PowerManager.h>
#include <private/android_filesystem_config.h>
#include <private/media/AudioTrackShared.h>
#include <psh_utils/AudioPowerManager.h>
#include <system/audio_effects/effect_aec.h>
#include <system/audio_effects/effect_downmix.h>
#include <system/audio_effects/effect_ns.h>
#include <system/audio_effects/effect_spatializer.h>
#include <utils/Log.h>
#include <utils/Trace.h>

#include <fcntl.h>
#include <linux/futex.h>
#include <math.h>
#include <memory>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>

// ----------------------------------------------------------------------------

// Note: the following macro is used for extremely verbose logging message.  In
// order to run with ALOG_ASSERT turned on, we need to have LOG_NDEBUG set to
// 0; but one side effect of this is to turn all LOGV's as well.  Some messages
// are so verbose that we want to suppress them even when we have ALOG_ASSERT
// turned on.  Do not uncomment the #def below unless you really know what you
// are doing and want to see all of the extremely verbose messages.
//#define VERY_VERY_VERBOSE_LOGGING
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

// TODO: Move these macro/inlines to a header file.
#define max(a, b) ((a) > (b) ? (a) : (b))

template <typename T>
static inline T min(const T& a, const T& b)
{
    return a < b ? a : b;
}

using com::android::media::audio::audioserver_permissions;
using com::android::media::permission::PermissionEnum::CAPTURE_AUDIO_HOTWORD;
using com::android::media::permission::ValidatedAttributionSourceState;
namespace audioserver_flags = com::android::media::audioserver;

namespace android {

using audioflinger::SyncEvent;
using media::IEffectClient;
using content::AttributionSourceState;

// Keep in sync with java definition in media/java/android/media/AudioRecord.java
static constexpr int32_t kMaxSharedAudioHistoryMs = 5000;

// retry counts for buffer fill timeout
// 50 * ~20msecs = 1 second
static const int8_t kMaxTrackRetries = 50;
static const int8_t kMaxTrackStartupRetries = 50;

// allow less retry attempts on direct output thread.
// direct outputs can be a scarce resource in audio hardware and should
// be released as quickly as possible.
// Notes:
// 1) The retry duration kMaxTrackRetriesDirectMs may be increased
//    in case the data write is bursty for the AudioTrack.  The application
//    should endeavor to write at least once every kMaxTrackRetriesDirectMs
//    to prevent an underrun situation.  If the data is bursty, then
//    the application can also throttle the data sent to be even.
// 2) For compressed audio data, any data present in the AudioTrack buffer
//    will be sent and reset the retry count.  This delivers data as
//    it arrives, with approximately kDirectMinSleepTimeUs = 10ms checking interval.
// 3) For linear PCM or proportional PCM, we wait one period for a period's worth
//    of data to be available, then any remaining data is delivered.
//    This is required to ensure the last bit of data is delivered before underrun.
//
// Sleep time per cycle is kDirectMinSleepTimeUs for compressed tracks
// or the size of the HAL period for proportional / linear PCM tracks.
static const int32_t kMaxTrackRetriesDirectMs = 200;

// don't warn about blocked writes or record buffer overflows more often than this
static const nsecs_t kWarningThrottleNs = seconds(5);

// RecordThread loop sleep time upon application overrun or audio HAL read error
static const int kRecordThreadSleepUs = 5000;

// maximum time to wait in sendConfigEvent_l() for a status to be received
static const nsecs_t kConfigEventTimeoutNs = seconds(2);
// longer timeout for create audio patch to account for specific scenarii
// with Bluetooth devices
static const nsecs_t kCreatePatchEventTimeoutNs = seconds(4);

// minimum sleep time for the mixer thread loop when tracks are active but in underrun
static const uint32_t kMinThreadSleepTimeUs = 5000;
// maximum divider applied to the active sleep time in the mixer thread loop
static const uint32_t kMaxThreadSleepTimeShift = 2;

// minimum normal sink buffer size, expressed in milliseconds rather than frames
// FIXME This should be based on experimentally observed scheduling jitter
static const uint32_t kMinNormalSinkBufferSizeMs = 20;
// maximum normal sink buffer size
static const uint32_t kMaxNormalSinkBufferSizeMs = 24;

// minimum capture buffer size in milliseconds to _not_ need a fast capture thread
// FIXME This should be based on experimentally observed scheduling jitter
static const uint32_t kMinNormalCaptureBufferSizeMs = 12;

// Offloaded output thread standby delay: allows track transition without going to standby
static const nsecs_t kOffloadStandbyDelayNs = seconds(1);

// Direct output thread minimum sleep time in idle or active(underrun) state
static const nsecs_t kDirectMinSleepTimeUs = 10000;

// Minimum amount of time between checking to see if the timestamp is advancing
// for underrun detection. If we check too frequently, we may not detect a
// timestamp update and will falsely detect underrun.
static constexpr nsecs_t kMinimumTimeBetweenTimestampChecksNs = 150 /* ms */ * 1'000'000;

// The universal constant for ubiquitous 20ms value. The value of 20ms seems to provide a good
// balance between power consumption and latency, and allows threads to be scheduled reliably
// by the CFS scheduler.
// FIXME Express other hardcoded references to 20ms with references to this constant and move
// it appropriately.
#define FMS_20 20

// Whether to use fast mixer
static const enum {
    FastMixer_Never,    // never initialize or use: for debugging only
    FastMixer_Always,   // always initialize and use, even if not needed: for debugging only
                        // normal mixer multiplier is 1
    FastMixer_Static,   // initialize if needed, then use all the time if initialized,
                        // multiplier is calculated based on min & max normal mixer buffer size
    FastMixer_Dynamic,  // initialize if needed, then use dynamically depending on track load,
                        // multiplier is calculated based on min & max normal mixer buffer size
    // FIXME for FastMixer_Dynamic:
    //  Supporting this option will require fixing HALs that can't handle large writes.
    //  For example, one HAL implementation returns an error from a large write,
    //  and another HAL implementation corrupts memory, possibly in the sample rate converter.
    //  We could either fix the HAL implementations, or provide a wrapper that breaks
    //  up large writes into smaller ones, and the wrapper would need to deal with scheduler.
} kUseFastMixer = FastMixer_Static;

// Whether to use fast capture
static const enum {
    FastCapture_Never,  // never initialize or use: for debugging only
    FastCapture_Always, // always initialize and use, even if not needed: for debugging only
    FastCapture_Static, // initialize if needed, then use all the time if initialized
} kUseFastCapture = FastCapture_Static;

// Priorities for requestPriority
static const int kPriorityAudioApp = 2;
static const int kPriorityFastMixer = 3;
static const int kPriorityFastCapture = 3;
// Request real-time priority for PlaybackThread in ARC
static const int kPriorityPlaybackThreadArc = 1;

// IAudioFlinger::createTrack() has an in/out parameter 'pFrameCount' for the total size of the
// track buffer in shared memory.  Zero on input means to use a default value.  For fast tracks,
// AudioFlinger derives the default from HAL buffer size and 'fast track multiplier'.

// This is the default value, if not specified by property.
static const int kFastTrackMultiplier = 2;

// The minimum and maximum allowed values
static const int kFastTrackMultiplierMin = 1;
static const int kFastTrackMultiplierMax = 2;

// The actual value to use, which can be specified per-device via property af.fast_track_multiplier.
static int sFastTrackMultiplier = kFastTrackMultiplier;

// See Thread::readOnlyHeap().
// Initially this heap is used to allocate client buffers for "fast" AudioRecord.
// Eventually it will be the single buffer that FastCapture writes into via HAL read(),
// and that all "fast" AudioRecord clients read from.  In either case, the size can be small.
static const size_t kRecordThreadReadOnlyHeapSize = 0xD000;

static const nsecs_t kDefaultStandbyTimeInNsecs = seconds(3);

static nsecs_t getStandbyTimeInNanos() {
    static nsecs_t standbyTimeInNanos = []() {
        const int ms = property_get_int32("ro.audio.flinger_standbytime_ms",
                    kDefaultStandbyTimeInNsecs / NANOS_PER_MILLISECOND);
        ALOGI("%s: Using %d ms as standby time", __func__, ms);
        return milliseconds(ms);
    }();
    return standbyTimeInNanos;
}

// Set kEnableExtendedChannels to true to enable greater than stereo output
// for the MixerThread and device sink.  Number of channels allowed is
// FCC_2 <= channels <= FCC_LIMIT.
constexpr bool kEnableExtendedChannels = true;

// Returns true if channel mask is permitted for the PCM sink in the MixerThread
/* static */
bool IAfThreadBase::isValidPcmSinkChannelMask(audio_channel_mask_t channelMask) {
    switch (audio_channel_mask_get_representation(channelMask)) {
    case AUDIO_CHANNEL_REPRESENTATION_POSITION: {
        // Haptic channel mask is only applicable for channel position mask.
        const uint32_t channelCount = audio_channel_count_from_out_mask(
                static_cast<audio_channel_mask_t>(channelMask & ~AUDIO_CHANNEL_HAPTIC_ALL));
        const uint32_t maxChannelCount = kEnableExtendedChannels
                ? FCC_LIMIT : FCC_2;
        if (channelCount < FCC_2 // mono is not supported at this time
                || channelCount > maxChannelCount) {
            return false;
        }
        // check that channelMask is the "canonical" one we expect for the channelCount.
        return audio_channel_position_mask_is_out_canonical(channelMask);
        }
    case AUDIO_CHANNEL_REPRESENTATION_INDEX:
        if (kEnableExtendedChannels) {
            const uint32_t channelCount = audio_channel_count_from_out_mask(channelMask);
            if (channelCount >= FCC_2 // mono is not supported at this time
                    && channelCount <= FCC_LIMIT) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

// Set kEnableExtendedPrecision to true to use extended precision in MixerThread
constexpr bool kEnableExtendedPrecision = true;

// Returns true if format is permitted for the PCM sink in the MixerThread
/* static */
bool IAfThreadBase::isValidPcmSinkFormat(audio_format_t format) {
    switch (format) {
    case AUDIO_FORMAT_PCM_16_BIT:
        return true;
    case AUDIO_FORMAT_PCM_FLOAT:
    case AUDIO_FORMAT_PCM_24_BIT_PACKED:
    case AUDIO_FORMAT_PCM_32_BIT:
    case AUDIO_FORMAT_PCM_8_24_BIT:
        return kEnableExtendedPrecision;
    default:
        return false;
    }
}

// ----------------------------------------------------------------------------

// formatToString() needs to be exact for MediaMetrics purposes.
// Do not use media/TypeConverter.h toString().
/* static */
std::string IAfThreadBase::formatToString(audio_format_t format) {
    std::string result;
    FormatConverter::toString(format, result);
    return result;
}

// TODO: move all toString helpers to audio.h
// under  #ifdef __cplusplus #endif
static std::string patchSinksToString(const struct audio_patch *patch)
{
    std::string s;
    for (size_t i = 0; i < patch->num_sinks; ++i) {
        if (i > 0) s.append("|");
        if (patch->sinks[i].ext.device.address[0]) {
            s.append("(").append(toString(patch->sinks[i].ext.device.type))
                    .append(", ").append(patch->sinks[i].ext.device.address).append(")");
        } else {
            s.append(toString(patch->sinks[i].ext.device.type));
        }
    }
    return s;
}

static std::string patchSourcesToString(const struct audio_patch *patch)
{
    std::string s;
    for (size_t i = 0; i < patch->num_sources; ++i) {
        if (i > 0) s.append("|");
        if (patch->sources[i].ext.device.address[0]) {
            s.append("(").append(toString(patch->sources[i].ext.device.type))
                    .append(", ").append(patch->sources[i].ext.device.address).append(")");
        } else {
            s.append(toString(patch->sources[i].ext.device.type));
        }
    }
    return s;
}

static std::string toString(audio_latency_mode_t mode) {
    // We convert to the AIDL type to print (eventually the legacy type will be removed).
    const auto result = legacy2aidl_audio_latency_mode_t_AudioLatencyMode(mode);
    return result.has_value() ? media::audio::common::toString(*result) : "UNKNOWN";
}

// Could be made a template, but other toString overloads for std::vector are confused.
static std::string toString(const std::vector<audio_latency_mode_t>& elements) {
    std::string s("{ ");
    for (const auto& e : elements) {
        s.append(toString(e));
        s.append(" ");
    }
    s.append("}");
    return s;
}

static pthread_once_t sFastTrackMultiplierOnce = PTHREAD_ONCE_INIT;

static void sFastTrackMultiplierInit()
{
    char value[PROPERTY_VALUE_MAX];
    if (property_get("af.fast_track_multiplier", value, NULL) > 0) {
        char *endptr;
        unsigned long ul = strtoul(value, &endptr, 0);
        if (*endptr == '\0' && kFastTrackMultiplierMin <= ul && ul <= kFastTrackMultiplierMax) {
            sFastTrackMultiplier = (int) ul;
        }
    }
}

// ----------------------------------------------------------------------------

#ifdef ADD_BATTERY_DATA
// To collect the amplifier usage
static void addBatteryData(uint32_t params) {
    sp<IMediaPlayerService> service = IMediaDeathNotifier::getMediaPlayerService();
    if (service == NULL) {
        // it already logged
        return;
    }

    service->addBatteryData(params);
}
#endif

// Track the CLOCK_BOOTTIME versus CLOCK_MONOTONIC timebase offset
struct {
    // call when you acquire a partial wakelock
    void acquire(const sp<IBinder> &wakeLockToken) {
        pthread_mutex_lock(&mLock);
        if (wakeLockToken.get() == nullptr) {
            adjustTimebaseOffset(&mBoottimeOffset, ExtendedTimestamp::TIMEBASE_BOOTTIME);
        } else {
            if (mCount == 0) {
                adjustTimebaseOffset(&mBoottimeOffset, ExtendedTimestamp::TIMEBASE_BOOTTIME);
            }
            ++mCount;
        }
        pthread_mutex_unlock(&mLock);
    }

    // call when you release a partial wakelock.
    void release(const sp<IBinder> &wakeLockToken) {
        if (wakeLockToken.get() == nullptr) {
            return;
        }
        pthread_mutex_lock(&mLock);
        if (--mCount < 0) {
            ALOGE("negative wakelock count");
            mCount = 0;
        }
        pthread_mutex_unlock(&mLock);
    }

    // retrieves the boottime timebase offset from monotonic.
    int64_t getBoottimeOffset() {
        pthread_mutex_lock(&mLock);
        int64_t boottimeOffset = mBoottimeOffset;
        pthread_mutex_unlock(&mLock);
        return boottimeOffset;
    }

    // Adjusts the timebase offset between TIMEBASE_MONOTONIC
    // and the selected timebase.
    // Currently only TIMEBASE_BOOTTIME is allowed.
    //
    // This only needs to be called upon acquiring the first partial wakelock
    // after all other partial wakelocks are released.
    //
    // We do an empirical measurement of the offset rather than parsing
    // /proc/timer_list since the latter is not a formal kernel ABI.
    static void adjustTimebaseOffset(int64_t *offset, ExtendedTimestamp::Timebase timebase) {
        int clockbase;
        switch (timebase) {
        case ExtendedTimestamp::TIMEBASE_BOOTTIME:
            clockbase = SYSTEM_TIME_BOOTTIME;
            break;
        default:
            LOG_ALWAYS_FATAL("invalid timebase %d", timebase);
            break;
        }
        // try three times to get the clock offset, choose the one
        // with the minimum gap in measurements.
        const int tries = 3;
        nsecs_t bestGap = 0, measured = 0; // not required, initialized for clang-tidy
        for (int i = 0; i < tries; ++i) {
            const nsecs_t tmono = systemTime(SYSTEM_TIME_MONOTONIC);
            const nsecs_t tbase = systemTime(clockbase);
            const nsecs_t tmono2 = systemTime(SYSTEM_TIME_MONOTONIC);
            const nsecs_t gap = tmono2 - tmono;
            if (i == 0 || gap < bestGap) {
                bestGap = gap;
                measured = tbase - ((tmono + tmono2) >> 1);
            }
        }

        // to avoid micro-adjusting, we don't change the timebase
        // unless it is significantly different.
        //
        // Assumption: It probably takes more than toleranceNs to
        // suspend and resume the device.
        static int64_t toleranceNs = 10000; // 10 us
        if (llabs(*offset - measured) > toleranceNs) {
            ALOGV("Adjusting timebase offset old: %lld  new: %lld",
                    (long long)*offset, (long long)measured);
            *offset = measured;
        }
    }

    pthread_mutex_t mLock;
    int32_t mCount;
    int64_t mBoottimeOffset;
} gBoottime = { PTHREAD_MUTEX_INITIALIZER, 0, 0 }; // static, so use POD initialization

// ----------------------------------------------------------------------------
//      CPU Stats
// ----------------------------------------------------------------------------

class CpuStats {
public:
    CpuStats();
    void sample(const String8 &title);
#ifdef DEBUG_CPU_USAGE
private:
    ThreadCpuUsage mCpuUsage;           // instantaneous thread CPU usage in wall clock ns
    audio_utils::Statistics<double> mWcStats; // statistics on thread CPU usage in wall clock ns

    audio_utils::Statistics<double> mHzStats; // statistics on thread CPU usage in cycles

    int mCpuNum;                        // thread's current CPU number
    int mCpukHz;                        // frequency of thread's current CPU in kHz
#endif
};

CpuStats::CpuStats()
#ifdef DEBUG_CPU_USAGE
    : mCpuNum(-1), mCpukHz(-1)
#endif
{
}

void CpuStats::sample(const String8 &title
#ifndef DEBUG_CPU_USAGE
                __unused
#endif
        ) {
#ifdef DEBUG_CPU_USAGE
    // get current thread's delta CPU time in wall clock ns
    double wcNs;
    bool valid = mCpuUsage.sampleAndEnable(wcNs);

    // record sample for wall clock statistics
    if (valid) {
        mWcStats.add(wcNs);
    }

    // get the current CPU number
    int cpuNum = sched_getcpu();

    // get the current CPU frequency in kHz
    int cpukHz = mCpuUsage.getCpukHz(cpuNum);

    // check if either CPU number or frequency changed
    if (cpuNum != mCpuNum || cpukHz != mCpukHz) {
        mCpuNum = cpuNum;
        mCpukHz = cpukHz;
        // ignore sample for purposes of cycles
        valid = false;
    }

    // if no change in CPU number or frequency, then record sample for cycle statistics
    if (valid && mCpukHz > 0) {
        const double cycles = wcNs * cpukHz * 0.000001;
        mHzStats.add(cycles);
    }

    const unsigned n = mWcStats.getN();
    // mCpuUsage.elapsed() is expensive, so don't call it every loop
    if ((n & 127) == 1) {
        const long long elapsed = mCpuUsage.elapsed();
        if (elapsed >= DEBUG_CPU_USAGE * 1000000000LL) {
            const double perLoop = elapsed / (double) n;
            const double perLoop100 = perLoop * 0.01;
            const double perLoop1k = perLoop * 0.001;
            const double mean = mWcStats.getMean();
            const double stddev = mWcStats.getStdDev();
            const double minimum = mWcStats.getMin();
            const double maximum = mWcStats.getMax();
            const double meanCycles = mHzStats.getMean();
            const double stddevCycles = mHzStats.getStdDev();
            const double minCycles = mHzStats.getMin();
            const double maxCycles = mHzStats.getMax();
            mCpuUsage.resetElapsed();
            mWcStats.reset();
            mHzStats.reset();
            ALOGD("CPU usage for %s over past %.1f secs\n"
                "  (%u mixer loops at %.1f mean ms per loop):\n"
                "  us per mix loop: mean=%.0f stddev=%.0f min=%.0f max=%.0f\n"
                "  %% of wall: mean=%.1f stddev=%.1f min=%.1f max=%.1f\n"
                "  MHz: mean=%.1f, stddev=%.1f, min=%.1f max=%.1f",
                    title.c_str(),
                    elapsed * .000000001, n, perLoop * .000001,
                    mean * .001,
                    stddev * .001,
                    minimum * .001,
                    maximum * .001,
                    mean / perLoop100,
                    stddev / perLoop100,
                    minimum / perLoop100,
                    maximum / perLoop100,
                    meanCycles / perLoop1k,
                    stddevCycles / perLoop1k,
                    minCycles / perLoop1k,
                    maxCycles / perLoop1k);

        }
    }
#endif
};

// ----------------------------------------------------------------------------
//      ThreadBase
// ----------------------------------------------------------------------------

// static
const char* IAfThreadBase::threadTypeToString(ThreadBase::type_t type)
{
    switch (type) {
    case MIXER:
        return "MIXER";
    case DIRECT:
        return "DIRECT";
    case DUPLICATING:
        return "DUPLICATING";
    case RECORD:
        return "RECORD";
    case OFFLOAD:
        return "OFFLOAD";
    case MMAP_PLAYBACK:
        return "MMAP_PLAYBACK";
    case MMAP_CAPTURE:
        return "MMAP_CAPTURE";
    case SPATIALIZER:
        return "SPATIALIZER";
    case BIT_PERFECT:
        return "BIT_PERFECT";
    case DIRECT_RECORD:
        return "DIRECT_RECORD";
    default:
        return "unknown";
    }
}

ThreadBase::ThreadBase(const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        type_t type, bool systemReady, bool isOut)
    :   Thread(false /*canCallJava*/),
        mType(type),
        mAfThreadCallback(afThreadCallback),
        mThreadMetrics(std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_THREAD) + std::to_string(id),
               isOut),
        mIsOut(isOut),
        // mSampleRate, mFrameCount, mChannelMask, mChannelCount, mFrameSize, mFormat, mBufferSize
        // are set by PlaybackThread::readOutputParameters_l() or
        // RecordThread::readInputParameters_l()
        //FIXME: mStandby should be true here. Is this some kind of hack?
        mStandby(false),
        mAudioSource(AUDIO_SOURCE_DEFAULT), mId(id),
        // mName will be set by concrete (non-virtual) subclass
        mDeathRecipient(new PMDeathRecipient(this)),
        mSystemReady(systemReady),
        mSignalPending(false)
{
    mThreadMetrics.logConstructor(getpid(), threadTypeToString(type), id);
    memset(&mPatch, 0, sizeof(struct audio_patch));
}

ThreadBase::~ThreadBase()
{
    // mConfigEvents should be empty, but just in case it isn't, free the memory it owns
    mConfigEvents.clear();

    // do not lock the mutex in destructor
    releaseWakeLock_l();
    if (mPowerManager != 0) {
        sp<IBinder> binder = IInterface::asBinder(mPowerManager);
        binder->unlinkToDeath(mDeathRecipient);
    }

    sendStatistics(true /* force */);
}

status_t ThreadBase::readyToRun()
{
    status_t status = initCheck();
    if (status == NO_ERROR) {
        ALOGI("AudioFlinger's thread %p tid=%d ready to run", this, getTid());
    } else {
        ALOGE("No working audio driver found.");
    }
    return status;
}

void ThreadBase::exit()
{
    ALOGV("ThreadBase::exit");
    // do any cleanup required for exit to succeed
    preExit();
    {
        // This lock prevents the following race in thread (uniprocessor for illustration):
        //  if (!exitPending()) {
        //      // context switch from here to exit()
        //      // exit() calls requestExit(), what exitPending() observes
        //      // exit() calls signal(), which is dropped since no waiters
        //      // context switch back from exit() to here
        //      mWaitWorkCV.wait(...);
        //      // now thread is hung
        //  }
        audio_utils::lock_guard lock(mutex());
        requestExit();
        mWaitWorkCV.notify_all();
    }
    // When Thread::requestExitAndWait is made virtual and this method is renamed to
    // "virtual status_t requestExitAndWait()", replace by "return Thread::requestExitAndWait();"

    // For TimeCheck: track waiting on the thread join of getTid().
    audio_utils::mutex::scoped_join_wait_check sjw(getTid());

    requestExitAndWait();
}

status_t ThreadBase::setParameters(const String8& keyValuePairs)
{
    ALOGV("ThreadBase::setParameters() %s", keyValuePairs.c_str());
    audio_utils::lock_guard _l(mutex());

    return sendSetParameterConfigEvent_l(keyValuePairs);
}

// sendConfigEvent_l() must be called with ThreadBase::mLock held
// Can temporarily release the lock if waiting for a reply from processConfigEvents_l().
status_t ThreadBase::sendConfigEvent_l(sp<ConfigEvent>& event)
NO_THREAD_SAFETY_ANALYSIS  // condition variable
{
    status_t status = NO_ERROR;

    if (event->mRequiresSystemReady && !mSystemReady) {
        event->mWaitStatus = false;
        mPendingConfigEvents.add(event);
        return status;
    }
    mConfigEvents.add(event);
    ALOGV("sendConfigEvent_l() num events %zu event %d", mConfigEvents.size(), event->mType);
    mWaitWorkCV.notify_one();
    mutex().unlock();
    {
        audio_utils::unique_lock _l(event->mutex());
        nsecs_t timeoutNs = event->mType == CFG_EVENT_CREATE_AUDIO_PATCH ?
              kCreatePatchEventTimeoutNs : kConfigEventTimeoutNs;
        while (event->mWaitStatus) {
            if (event->mCondition.wait_for(
                    _l, std::chrono::nanoseconds(timeoutNs), getTid())
                            == std::cv_status::timeout) {
                event->mStatus = TIMED_OUT;
                event->mWaitStatus = false;
            }
        }
        status = event->mStatus;
    }
    mutex().lock();
    return status;
}

void ThreadBase::sendIoConfigEvent(audio_io_config_event_t event, pid_t pid,
                                                 audio_port_handle_t portId)
{
    audio_utils::lock_guard _l(mutex());
    sendIoConfigEvent_l(event, pid, portId);
}

// sendIoConfigEvent_l() must be called with ThreadBase::mutex() held
void ThreadBase::sendIoConfigEvent_l(audio_io_config_event_t event, pid_t pid,
                                                   audio_port_handle_t portId)
{
    // The audio statistics history is exponentially weighted to forget events
    // about five or more seconds in the past.  In order to have
    // crisper statistics for mediametrics, we reset the statistics on
    // an IoConfigEvent, to reflect different properties for a new device.
    mIoJitterMs.reset();
    mLatencyMs.reset();
    mProcessTimeMs.reset();
    mMonopipePipeDepthStats.reset();
    mTimestampVerifier.discontinuity(mTimestampVerifier.DISCONTINUITY_MODE_CONTINUOUS);

    sp<ConfigEvent> configEvent = (ConfigEvent *)new IoConfigEvent(event, pid, portId);
    sendConfigEvent_l(configEvent);
}

void ThreadBase::sendPrioConfigEvent(pid_t pid, pid_t tid, int32_t prio, bool forApp)
{
    audio_utils::lock_guard _l(mutex());
    sendPrioConfigEvent_l(pid, tid, prio, forApp);
}

// sendPrioConfigEvent_l() must be called with ThreadBase::mutex() held
void ThreadBase::sendPrioConfigEvent_l(
        pid_t pid, pid_t tid, int32_t prio, bool forApp)
{
    sp<ConfigEvent> configEvent = (ConfigEvent *)new PrioConfigEvent(pid, tid, prio, forApp);
    sendConfigEvent_l(configEvent);
}

// sendSetParameterConfigEvent_l() must be called with ThreadBase::mutex() held
status_t ThreadBase::sendSetParameterConfigEvent_l(const String8& keyValuePair)
{
    sp<ConfigEvent> configEvent;
    AudioParameter param(keyValuePair);
    int value;
    if (param.getInt(String8(AudioParameter::keyMonoOutput), value) == NO_ERROR) {
        setMasterMono_l(value != 0);
        if (param.size() == 1) {
            return NO_ERROR; // should be a solo parameter - we don't pass down
        }
        param.remove(String8(AudioParameter::keyMonoOutput));
        configEvent = new SetParameterConfigEvent(param.toString());
    } else {
        configEvent = new SetParameterConfigEvent(keyValuePair);
    }
    return sendConfigEvent_l(configEvent);
}

status_t ThreadBase::sendCreateAudioPatchConfigEvent(
                                                        const struct audio_patch *patch,
                                                        audio_patch_handle_t *handle)
{
    audio_utils::lock_guard _l(mutex());
    sp<ConfigEvent> configEvent = (ConfigEvent *)new CreateAudioPatchConfigEvent(*patch, *handle);
    status_t status = sendConfigEvent_l(configEvent);
    if (status == NO_ERROR) {
        CreateAudioPatchConfigEventData *data =
                                        (CreateAudioPatchConfigEventData *)configEvent->mData.get();
        *handle = data->mHandle;
    }
    return status;
}

status_t ThreadBase::sendReleaseAudioPatchConfigEvent(
                                                                const audio_patch_handle_t handle)
{
    audio_utils::lock_guard _l(mutex());
    sp<ConfigEvent> configEvent = (ConfigEvent *)new ReleaseAudioPatchConfigEvent(handle);
    return sendConfigEvent_l(configEvent);
}

status_t ThreadBase::sendUpdateOutDeviceConfigEvent(
        const DeviceDescriptorBaseVector& outDevices)
{
    if (type() != RECORD) {
        // The update out device operation is only for record thread.
        return INVALID_OPERATION;
    }
    audio_utils::lock_guard _l(mutex());
    sp<ConfigEvent> configEvent = (ConfigEvent *)new UpdateOutDevicesConfigEvent(outDevices);
    return sendConfigEvent_l(configEvent);
}

void ThreadBase::sendResizeBufferConfigEvent_l(int32_t maxSharedAudioHistoryMs)
{
    ALOG_ASSERT(type() == RECORD, "sendResizeBufferConfigEvent_l() called on non record thread");
    sp<ConfigEvent> configEvent =
            (ConfigEvent *)new ResizeBufferConfigEvent(maxSharedAudioHistoryMs);
    sendConfigEvent_l(configEvent);
}

void ThreadBase::sendCheckOutputStageEffectsEvent()
{
    audio_utils::lock_guard _l(mutex());
    sendCheckOutputStageEffectsEvent_l();
}

void ThreadBase::sendCheckOutputStageEffectsEvent_l()
{
    sp<ConfigEvent> configEvent =
            (ConfigEvent *)new CheckOutputStageEffectsEvent();
    sendConfigEvent_l(configEvent);
}

void ThreadBase::sendHalLatencyModesChangedEvent_l()
{
    sp<ConfigEvent> configEvent = sp<HalLatencyModesChangedEvent>::make();
    sendConfigEvent_l(configEvent);
}

// post condition: mConfigEvents.isEmpty()
void ThreadBase::processConfigEvents_l()
{
    bool configChanged = false;

    while (!mConfigEvents.isEmpty()) {
        ALOGV("processConfigEvents_l() remaining events %zu", mConfigEvents.size());
        sp<ConfigEvent> event = mConfigEvents[0];
        mConfigEvents.removeAt(0);
        switch (event->mType) {
        case CFG_EVENT_PRIO: {
            PrioConfigEventData *data = (PrioConfigEventData *)event->mData.get();
            // FIXME Need to understand why this has to be done asynchronously
            int err = requestPriority(data->mPid, data->mTid, data->mPrio, data->mForApp,
                    true /*asynchronous*/);
            if (err != 0) {
                ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; error %d",
                      data->mPrio, data->mPid, data->mTid, err);
            }
        } break;
        case CFG_EVENT_IO: {
            IoConfigEventData *data = (IoConfigEventData *)event->mData.get();
            ioConfigChanged_l(data->mEvent, data->mPid, data->mPortId);
        } break;
        case CFG_EVENT_SET_PARAMETER: {
            SetParameterConfigEventData *data = (SetParameterConfigEventData *)event->mData.get();
            if (checkForNewParameter_l(data->mKeyValuePairs, event->mStatus)) {
                configChanged = true;
                mLocalLog.log("CFG_EVENT_SET_PARAMETER: (%s) configuration changed",
                        data->mKeyValuePairs.c_str());
            }
        } break;
        case CFG_EVENT_CREATE_AUDIO_PATCH: {
            const DeviceTypeSet oldDevices = getDeviceTypes_l();
            CreateAudioPatchConfigEventData *data =
                                            (CreateAudioPatchConfigEventData *)event->mData.get();
            event->mStatus = createAudioPatch_l(&data->mPatch, &data->mHandle);
            const DeviceTypeSet newDevices = getDeviceTypes_l();
            configChanged = oldDevices != newDevices;
            mLocalLog.log("CFG_EVENT_CREATE_AUDIO_PATCH: old device %s (%s) new device %s (%s)",
                    dumpDeviceTypes(oldDevices).c_str(), toString(oldDevices).c_str(),
                    dumpDeviceTypes(newDevices).c_str(), toString(newDevices).c_str());
        } break;
        case CFG_EVENT_RELEASE_AUDIO_PATCH: {
            const DeviceTypeSet oldDevices = getDeviceTypes_l();
            ReleaseAudioPatchConfigEventData *data =
                                            (ReleaseAudioPatchConfigEventData *)event->mData.get();
            event->mStatus = releaseAudioPatch_l(data->mHandle);
            const DeviceTypeSet newDevices = getDeviceTypes_l();
            configChanged = oldDevices != newDevices;
            mLocalLog.log("CFG_EVENT_RELEASE_AUDIO_PATCH: old device %s (%s) new device %s (%s)",
                    dumpDeviceTypes(oldDevices).c_str(), toString(oldDevices).c_str(),
                    dumpDeviceTypes(newDevices).c_str(), toString(newDevices).c_str());
        } break;
        case CFG_EVENT_UPDATE_OUT_DEVICE: {
            UpdateOutDevicesConfigEventData *data =
                    (UpdateOutDevicesConfigEventData *)event->mData.get();
            updateOutDevices(data->mOutDevices);
        } break;
        case CFG_EVENT_RESIZE_BUFFER: {
            ResizeBufferConfigEventData *data =
                    (ResizeBufferConfigEventData *)event->mData.get();
            resizeInputBuffer_l(data->mMaxSharedAudioHistoryMs);
        } break;

        case CFG_EVENT_CHECK_OUTPUT_STAGE_EFFECTS: {
            setCheckOutputStageEffects();
        } break;

        case CFG_EVENT_HAL_LATENCY_MODES_CHANGED: {
            onHalLatencyModesChanged_l();
        } break;

        default:
            ALOG_ASSERT(false, "processConfigEvents_l() unknown event type %d", event->mType);
            break;
        }
        {
            audio_utils::lock_guard _l(event->mutex());
            if (event->mWaitStatus) {
                event->mWaitStatus = false;
                event->mCondition.notify_one();
            }
        }
        ALOGV_IF(mConfigEvents.isEmpty(), "processConfigEvents_l() DONE thread %p", this);
    }

    if (configChanged) {
        cacheParameters_l();
    }
}

String8 channelMaskToString(audio_channel_mask_t mask, bool output) {
    String8 s;
    const audio_channel_representation_t representation =
            audio_channel_mask_get_representation(mask);

    switch (representation) {
    // Travel all single bit channel mask to convert channel mask to string.
    case AUDIO_CHANNEL_REPRESENTATION_POSITION: {
        if (output) {
            if (mask & AUDIO_CHANNEL_OUT_FRONT_LEFT) s.append("front-left, ");
            if (mask & AUDIO_CHANNEL_OUT_FRONT_RIGHT) s.append("front-right, ");
            if (mask & AUDIO_CHANNEL_OUT_FRONT_CENTER) s.append("front-center, ");
            if (mask & AUDIO_CHANNEL_OUT_LOW_FREQUENCY) s.append("low-frequency, ");
            if (mask & AUDIO_CHANNEL_OUT_BACK_LEFT) s.append("back-left, ");
            if (mask & AUDIO_CHANNEL_OUT_BACK_RIGHT) s.append("back-right, ");
            if (mask & AUDIO_CHANNEL_OUT_FRONT_LEFT_OF_CENTER) s.append("front-left-of-center, ");
            if (mask & AUDIO_CHANNEL_OUT_FRONT_RIGHT_OF_CENTER) s.append("front-right-of-center, ");
            if (mask & AUDIO_CHANNEL_OUT_BACK_CENTER) s.append("back-center, ");
            if (mask & AUDIO_CHANNEL_OUT_SIDE_LEFT) s.append("side-left, ");
            if (mask & AUDIO_CHANNEL_OUT_SIDE_RIGHT) s.append("side-right, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_CENTER) s.append("top-center ,");
            if (mask & AUDIO_CHANNEL_OUT_TOP_FRONT_LEFT) s.append("top-front-left, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_FRONT_CENTER) s.append("top-front-center, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_FRONT_RIGHT) s.append("top-front-right, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_LEFT) s.append("top-back-left, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_CENTER) s.append("top-back-center, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_RIGHT) s.append("top-back-right, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_SIDE_LEFT) s.append("top-side-left, ");
            if (mask & AUDIO_CHANNEL_OUT_TOP_SIDE_RIGHT) s.append("top-side-right, ");
            if (mask & AUDIO_CHANNEL_OUT_BOTTOM_FRONT_LEFT) s.append("bottom-front-left, ");
            if (mask & AUDIO_CHANNEL_OUT_BOTTOM_FRONT_CENTER) s.append("bottom-front-center, ");
            if (mask & AUDIO_CHANNEL_OUT_BOTTOM_FRONT_RIGHT) s.append("bottom-front-right, ");
            if (mask & AUDIO_CHANNEL_OUT_LOW_FREQUENCY_2) s.append("low-frequency-2, ");
            if (mask & AUDIO_CHANNEL_OUT_HAPTIC_B) s.append("haptic-B, ");
            if (mask & AUDIO_CHANNEL_OUT_HAPTIC_A) s.append("haptic-A, ");
            if (mask & ~AUDIO_CHANNEL_OUT_ALL) s.append("unknown,  ");
        } else {
            if (mask & AUDIO_CHANNEL_IN_LEFT) s.append("left, ");
            if (mask & AUDIO_CHANNEL_IN_RIGHT) s.append("right, ");
            if (mask & AUDIO_CHANNEL_IN_FRONT) s.append("front, ");
            if (mask & AUDIO_CHANNEL_IN_BACK) s.append("back, ");
            if (mask & AUDIO_CHANNEL_IN_LEFT_PROCESSED) s.append("left-processed, ");
            if (mask & AUDIO_CHANNEL_IN_RIGHT_PROCESSED) s.append("right-processed, ");
            if (mask & AUDIO_CHANNEL_IN_FRONT_PROCESSED) s.append("front-processed, ");
            if (mask & AUDIO_CHANNEL_IN_BACK_PROCESSED) s.append("back-processed, ");
            if (mask & AUDIO_CHANNEL_IN_PRESSURE) s.append("pressure, ");
            if (mask & AUDIO_CHANNEL_IN_X_AXIS) s.append("X, ");
            if (mask & AUDIO_CHANNEL_IN_Y_AXIS) s.append("Y, ");
            if (mask & AUDIO_CHANNEL_IN_Z_AXIS) s.append("Z, ");
            if (mask & AUDIO_CHANNEL_IN_BACK_LEFT) s.append("back-left, ");
            if (mask & AUDIO_CHANNEL_IN_BACK_RIGHT) s.append("back-right, ");
            if (mask & AUDIO_CHANNEL_IN_CENTER) s.append("center, ");
            if (mask & AUDIO_CHANNEL_IN_LOW_FREQUENCY) s.append("low-frequency, ");
            if (mask & AUDIO_CHANNEL_IN_TOP_LEFT) s.append("top-left, ");
            if (mask & AUDIO_CHANNEL_IN_TOP_RIGHT) s.append("top-right, ");
            if (mask & AUDIO_CHANNEL_IN_VOICE_UPLINK) s.append("voice-uplink, ");
            if (mask & AUDIO_CHANNEL_IN_VOICE_DNLINK) s.append("voice-dnlink, ");
            if (mask & ~AUDIO_CHANNEL_IN_ALL) s.append("unknown,  ");
        }
        const int len = s.length();
        if (len > 2) {
            (void) s.lockBuffer(len);      // needed?
            s.unlockBuffer(len - 2);       // remove trailing ", "
        }
        return s;
    }
    case AUDIO_CHANNEL_REPRESENTATION_INDEX:
        s.appendFormat("index mask, bits:%#x", audio_channel_mask_get_bits(mask));
        return s;
    default:
        s.appendFormat("unknown mask, representation:%d  bits:%#x",
                representation, audio_channel_mask_get_bits(mask));
        return s;
    }
}

void ThreadBase::dump(int fd, const Vector<String16>& args)
{
    dprintf(fd, "\n%s thread %p, name %s, tid %d, type %d (%s):\n", isOutput() ? "Output" : "Input",
            this, mThreadName, getTid(), type(), threadTypeToString(type()));

    {
        afutils::FallibleLockGuard l{mutex()};
        if (!l) {
            dprintf(fd, "  Thread may be deadlocked\n");
        }
        dumpBase_l(fd, args);
        dumpInternals_l(fd, args);
        dumpTracks_l(fd, args);
        dumpEffectChains_l(fd, args);
    }

    dprintf(fd, "  Local log:\n");
    const auto logHeader = this->getLocalLogHeader();
    write(fd, logHeader.data(), logHeader.length());
    mLocalLog.dump(fd, "   " /* prefix */);

    // --all does the statistics
    bool dumpAll = false;
    for (const auto &arg : args) {
        if (arg == String16("--all")) {
            dumpAll = true;
        }
    }
    if (dumpAll || type() == SPATIALIZER) {
        const std::string sched = mThreadSnapshot.toString();
        if (!sched.empty()) {
            (void)write(fd, sched.c_str(), sched.size());
        }
    }
}

void ThreadBase::dumpBase_l(int fd, const Vector<String16>& /* args */)
{
    dprintf(fd, "  I/O handle: %d\n", mId);
    dprintf(fd, "  Standby: %s\n", mStandby ? "yes" : "no");
    dprintf(fd, "  Sample rate: %u Hz\n", mSampleRate);
    dprintf(fd, "  HAL frame count: %zu\n", mFrameCount);
    dprintf(fd, "  HAL format: 0x%x (%s)\n", mHALFormat,
            IAfThreadBase::formatToString(mHALFormat).c_str());
    dprintf(fd, "  HAL buffer size: %zu bytes\n", mBufferSize);
    dprintf(fd, "  Channel count: %u\n", mChannelCount);
    dprintf(fd, "  Channel mask: 0x%08x (%s)\n", mChannelMask,
            channelMaskToString(mChannelMask, mType != RECORD).c_str());
    dprintf(fd, "  Processing format: 0x%x (%s)\n", mFormat,
            IAfThreadBase::formatToString(mFormat).c_str());
    dprintf(fd, "  Processing frame size: %zu bytes\n", mFrameSize);
    dprintf(fd, "  Pending config events:");
    size_t numConfig = mConfigEvents.size();
    if (numConfig) {
        const size_t SIZE = 256;
        char buffer[SIZE];
        for (size_t i = 0; i < numConfig; i++) {
            mConfigEvents[i]->dump(buffer, SIZE);
            dprintf(fd, "\n    %s", buffer);
        }
        dprintf(fd, "\n");
    } else {
        dprintf(fd, " none\n");
    }
    // Note: output device may be used by capture threads for effects such as AEC.
    dprintf(fd, "  Output devices: %s (%s)\n",
            dumpDeviceTypes(outDeviceTypes_l()).c_str(), toString(outDeviceTypes_l()).c_str());
    dprintf(fd, "  Input device: %#x (%s)\n",
            inDeviceType_l(), toString(inDeviceType_l()).c_str());
    dprintf(fd, "  Audio source: %d (%s)\n", mAudioSource, toString(mAudioSource).c_str());

    // Dump timestamp statistics for the Thread types that support it.
    if (mType == RECORD
            || mType == MIXER
            || mType == DUPLICATING
            || mType == DIRECT
            || mType == OFFLOAD
            || mType == SPATIALIZER) {
        dprintf(fd, "  Timestamp stats: %s\n", mTimestampVerifier.toString().c_str());
        dprintf(fd, "  Timestamp corrected: %s\n",
                isTimestampCorrectionEnabled_l() ? "yes" : "no");
    }

    if (mLastIoBeginNs > 0) { // MMAP may not set this
        dprintf(fd, "  Last %s occurred (msecs): %lld\n",
                isOutput() ? "write" : "read",
                (long long) (systemTime() - mLastIoBeginNs) / NANOS_PER_MILLISECOND);
    }

    if (mProcessTimeMs.getN() > 0) {
        dprintf(fd, "  Process time ms stats: %s\n", mProcessTimeMs.toString().c_str());
    }

    if (mIoJitterMs.getN() > 0) {
        dprintf(fd, "  Hal %s jitter ms stats: %s\n",
                isOutput() ? "write" : "read",
                mIoJitterMs.toString().c_str());
    }

    if (mLatencyMs.getN() > 0) {
        dprintf(fd, "  Threadloop %s latency stats: %s\n",
                isOutput() ? "write" : "read",
                mLatencyMs.toString().c_str());
    }

    if (mMonopipePipeDepthStats.getN() > 0) {
        dprintf(fd, "  Monopipe %s pipe depth stats: %s\n",
            isOutput() ? "write" : "read",
            mMonopipePipeDepthStats.toString().c_str());
    }
}

void ThreadBase::dumpEffectChains_l(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];

    size_t numEffectChains = mEffectChains.size();
    snprintf(buffer, SIZE, "  %zu Effect Chains\n", numEffectChains);
    write(fd, buffer, strlen(buffer));

    for (size_t i = 0; i < numEffectChains; ++i) {
        sp<IAfEffectChain> chain = mEffectChains[i];
        if (chain != 0) {
            chain->dump(fd, args);
        }
    }
}

void ThreadBase::acquireWakeLock()
{
    audio_utils::lock_guard _l(mutex());
    acquireWakeLock_l();
}

String16 ThreadBase::getWakeLockTag()
{
    switch (mType) {
    case MIXER:
        return String16("AudioMix");
    case DIRECT:
        return String16("AudioDirectOut");
    case DUPLICATING:
        return String16("AudioDup");
    case RECORD:
        return String16("AudioIn");
    case OFFLOAD:
        return String16("AudioOffload");
    case MMAP_PLAYBACK:
        return String16("MmapPlayback");
    case MMAP_CAPTURE:
        return String16("MmapCapture");
    case SPATIALIZER:
        return String16("AudioSpatial");
    case BIT_PERFECT:
        return String16("AudioBitPerfect");
    default:
        ALOG_ASSERT(false);
        return String16("AudioUnknown");
    }
}

void ThreadBase::acquireWakeLock_l()
{
    getPowerManager_l();
    if (mPowerManager != 0) {
        sp<IBinder> binder = new BBinder();
        // Uses AID_AUDIOSERVER for wakelock.  updateWakeLockUids_l() updates with client uids.
        binder::Status status = mPowerManager->acquireWakeLockAsync(binder,
                    POWERMANAGER_PARTIAL_WAKE_LOCK,
                    getWakeLockTag(),
                    String16("audioserver"),
                    {} /* workSource */,
                    {} /* historyTag */);
        if (status.isOk()) {
            mWakeLockToken = binder;
            if (media::psh_utils::AudioPowerManager::enabled()) {
                mThreadToken = media::psh_utils::createAudioThreadToken(
                        getTid(), String8(getWakeLockTag()).c_str());
            }
        }
        ALOGV("acquireWakeLock_l() %s status %d", mThreadName, status.exceptionCode());
    }

    gBoottime.acquire(mWakeLockToken);
    mTimestamp.mTimebaseOffset[ExtendedTimestamp::TIMEBASE_BOOTTIME] =
            gBoottime.getBoottimeOffset();
}

void ThreadBase::releaseWakeLock()
{
    audio_utils::lock_guard _l(mutex());
    releaseWakeLock_l();
}

void ThreadBase::releaseWakeLock_l()
{
    gBoottime.release(mWakeLockToken);
    if (mWakeLockToken != 0) {
        ALOGV("releaseWakeLock_l() %s", mThreadName);
        if (mPowerManager != 0) {
            mPowerManager->releaseWakeLockAsync(mWakeLockToken, 0);
        }
        mWakeLockToken.clear();
    }
    mThreadToken.reset();
}

void ThreadBase::getPowerManager_l() {
    if (mSystemReady && mPowerManager == 0) {
        // use checkService() to avoid blocking if power service is not up yet
        sp<IBinder> binder =
            defaultServiceManager()->checkService(String16("power"));
        if (binder == 0) {
            ALOGW("Thread %s cannot connect to the power manager service", mThreadName);
        } else {
            mPowerManager = interface_cast<os::IPowerManager>(binder);
            binder->linkToDeath(mDeathRecipient);
        }
    }
}

void ThreadBase::updateWakeLockUids_l(const SortedVector<uid_t>& uids) {
    getPowerManager_l();

#if !LOG_NDEBUG
    std::stringstream s;
    for (uid_t uid : uids) {
        s << uid << " ";
    }
    ALOGD("updateWakeLockUids_l %s uids:%s", mThreadName, s.str().c_str());
#endif

    if (mWakeLockToken == NULL) { // token may be NULL if AudioFlinger::systemReady() not called.
        if (mSystemReady) {
            ALOGE("no wake lock to update, but system ready!");
        } else {
            ALOGW("no wake lock to update, system not ready yet");
        }
        return;
    }
    if (mPowerManager != 0) {
        std::vector<int> uidsAsInt(uids.begin(), uids.end()); // powermanager expects uids as ints
        binder::Status status = mPowerManager->updateWakeLockUidsAsync(
                mWakeLockToken, uidsAsInt);
        ALOGV("updateWakeLockUids_l() %s status %d", mThreadName, status.exceptionCode());
    }
}

void ThreadBase::clearPowerManager()
{
    audio_utils::lock_guard _l(mutex());
    releaseWakeLock_l();
    mPowerManager.clear();
}

void ThreadBase::updateOutDevices(
        const DeviceDescriptorBaseVector& outDevices __unused)
{
    ALOGE("%s should only be called in RecordThread", __func__);
}

void ThreadBase::resizeInputBuffer_l(int32_t /* maxSharedAudioHistoryMs */)
{
    ALOGE("%s should only be called in RecordThread", __func__);
}

void ThreadBase::PMDeathRecipient::binderDied(const wp<IBinder>& /* who */)
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        thread->clearPowerManager();
    }
    ALOGW("power manager service died !!!");
}

void ThreadBase::setEffectSuspended_l(
        const effect_uuid_t *type, bool suspend, audio_session_t sessionId)
{
    sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
    if (chain != 0) {
        if (type != NULL) {
            chain->setEffectSuspended_l(type, suspend);
        } else {
            chain->setEffectSuspendedAll_l(suspend);
        }
    }

    updateSuspendedSessions_l(type, suspend, sessionId);
}

void ThreadBase::checkSuspendOnAddEffectChain_l(const sp<IAfEffectChain>& chain)
{
    ssize_t index = mSuspendedSessions.indexOfKey(chain->sessionId());
    if (index < 0) {
        return;
    }

    const KeyedVector <int, sp<SuspendedSessionDesc> >& sessionEffects =
            mSuspendedSessions.valueAt(index);

    for (size_t i = 0; i < sessionEffects.size(); i++) {
        const sp<SuspendedSessionDesc>& desc = sessionEffects.valueAt(i);
        for (int j = 0; j < desc->mRefCount; j++) {
            if (sessionEffects.keyAt(i) == IAfEffectChain::kKeyForSuspendAll) {
                chain->setEffectSuspendedAll_l(true);
            } else {
                ALOGV("checkSuspendOnAddEffectChain_l() suspending effects %08x",
                    desc->mType.timeLow);
                chain->setEffectSuspended_l(&desc->mType, true);
            }
        }
    }
}

void ThreadBase::updateSuspendedSessions_l(const effect_uuid_t* type,
                                                         bool suspend,
                                                         audio_session_t sessionId)
{
    ssize_t index = mSuspendedSessions.indexOfKey(sessionId);

    KeyedVector <int, sp<SuspendedSessionDesc> > sessionEffects;

    if (suspend) {
        if (index >= 0) {
            sessionEffects = mSuspendedSessions.valueAt(index);
        } else {
            mSuspendedSessions.add(sessionId, sessionEffects);
        }
    } else {
        if (index < 0) {
            return;
        }
        sessionEffects = mSuspendedSessions.valueAt(index);
    }


    int key = IAfEffectChain::kKeyForSuspendAll;
    if (type != NULL) {
        key = type->timeLow;
    }
    index = sessionEffects.indexOfKey(key);

    sp<SuspendedSessionDesc> desc;
    if (suspend) {
        if (index >= 0) {
            desc = sessionEffects.valueAt(index);
        } else {
            desc = new SuspendedSessionDesc();
            if (type != NULL) {
                desc->mType = *type;
            }
            sessionEffects.add(key, desc);
            ALOGV("updateSuspendedSessions_l() suspend adding effect %08x", key);
        }
        desc->mRefCount++;
    } else {
        if (index < 0) {
            return;
        }
        desc = sessionEffects.valueAt(index);
        if (--desc->mRefCount == 0) {
            ALOGV("updateSuspendedSessions_l() restore removing effect %08x", key);
            sessionEffects.removeItemsAt(index);
            if (sessionEffects.isEmpty()) {
                ALOGV("updateSuspendedSessions_l() restore removing session %d",
                                 sessionId);
                mSuspendedSessions.removeItem(sessionId);
            }
        }
    }
    if (!sessionEffects.isEmpty()) {
        mSuspendedSessions.replaceValueFor(sessionId, sessionEffects);
    }
}

void ThreadBase::checkSuspendOnEffectEnabled(bool enabled,
                                                           audio_session_t sessionId,
                                                           bool threadLocked)
NO_THREAD_SAFETY_ANALYSIS  // manual locking
{
    if (!threadLocked) {
        mutex().lock();
    }

    if (mType != RECORD) {
        // suspend all effects in AUDIO_SESSION_OUTPUT_MIX when enabling any effect on
        // another session. This gives the priority to well behaved effect control panels
        // and applications not using global effects.
        // Enabling post processing in AUDIO_SESSION_OUTPUT_STAGE session does not affect
        // global effects
        if (!audio_is_global_session(sessionId)) {
            setEffectSuspended_l(NULL, enabled, AUDIO_SESSION_OUTPUT_MIX);
        }
    }

    if (!threadLocked) {
        mutex().unlock();
    }
}

// checkEffectCompatibility_l() must be called with ThreadBase::mutex() held
status_t RecordThread::checkEffectCompatibility_l(
        const effect_descriptor_t *desc, audio_session_t sessionId)
{
    // No global output effect sessions on record threads
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX
            || sessionId == AUDIO_SESSION_OUTPUT_STAGE) {
        ALOGW("checkEffectCompatibility_l(): global effect %s on record thread %s",
                desc->name, mThreadName);
        return BAD_VALUE;
    }
    // only pre processing effects on record thread
    if ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_PRE_PROC) {
        ALOGW("checkEffectCompatibility_l(): non pre processing effect %s on record thread %s",
                desc->name, mThreadName);
        return BAD_VALUE;
    }

    // always allow effects without processing load or latency
    if ((desc->flags & EFFECT_FLAG_NO_PROCESS_MASK) == EFFECT_FLAG_NO_PROCESS) {
        return NO_ERROR;
    }

    audio_input_flags_t flags = mInput->flags;
    if (hasFastCapture() || (flags & AUDIO_INPUT_FLAG_FAST)) {
        if (flags & AUDIO_INPUT_FLAG_RAW) {
            ALOGW("checkEffectCompatibility_l(): effect %s on record thread %s in raw mode",
                  desc->name, mThreadName);
            return BAD_VALUE;
        }
        if ((desc->flags & EFFECT_FLAG_HW_ACC_TUNNEL) == 0) {
            ALOGW("checkEffectCompatibility_l(): non HW effect %s on record thread %s in fast mode",
                  desc->name, mThreadName);
            return BAD_VALUE;
        }
    }

    if (IAfEffectModule::isHapticGenerator(&desc->type)) {
        ALOGE("%s(): HapticGenerator is not supported in RecordThread", __func__);
        return BAD_VALUE;
    }
    return NO_ERROR;
}

// checkEffectCompatibility_l() must be called with ThreadBase::mutex() held
status_t PlaybackThread::checkEffectCompatibility_l(
        const effect_descriptor_t *desc, audio_session_t sessionId)
{
    // no preprocessing on playback threads
    if ((desc->flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_PRE_PROC) {
        ALOGW("%s: pre processing effect %s created on playback"
                " thread %s", __func__, desc->name, mThreadName);
        return BAD_VALUE;
    }

    // always allow effects without processing load or latency
    if ((desc->flags & EFFECT_FLAG_NO_PROCESS_MASK) == EFFECT_FLAG_NO_PROCESS) {
        return NO_ERROR;
    }

    if (IAfEffectModule::isHapticGenerator(&desc->type) && mHapticChannelCount == 0) {
        ALOGW("%s: thread (%s) doesn't support haptic playback while the effect is HapticGenerator",
              __func__, threadTypeToString(mType));
        return BAD_VALUE;
    }

    if (IAfEffectModule::isSpatializer(&desc->type)
            && mType != SPATIALIZER) {
        ALOGW("%s: attempt to create a spatializer effect on a thread of type %d",
                __func__, mType);
        return BAD_VALUE;
    }

    switch (mType) {
    case MIXER: {
        audio_output_flags_t flags = mOutput->flags;
        if (hasFastMixer() || (flags & AUDIO_OUTPUT_FLAG_FAST)) {
            if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
                // global effects are applied only to non fast tracks if they are SW
                if ((desc->flags & EFFECT_FLAG_HW_ACC_TUNNEL) == 0) {
                    break;
                }
            } else if (sessionId == AUDIO_SESSION_OUTPUT_STAGE) {
                // only post processing on output stage session
                if ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_POST_PROC) {
                    ALOGW("%s: non post processing effect %s not allowed on output stage session",
                            __func__, desc->name);
                    return BAD_VALUE;
                }
            } else if (sessionId == AUDIO_SESSION_DEVICE) {
                // only post processing on output stage session
                if ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_POST_PROC) {
                    ALOGW("%s: non post processing effect %s not allowed on device session",
                            __func__, desc->name);
                    return BAD_VALUE;
                }
            } else {
                // no restriction on effects applied on non fast tracks
                if ((hasAudioSession_l(sessionId) & ThreadBase::FAST_SESSION) == 0) {
                    break;
                }
            }

            if (flags & AUDIO_OUTPUT_FLAG_RAW) {
                ALOGW("%s: effect %s on playback thread in raw mode", __func__, desc->name);
                return BAD_VALUE;
            }
            if ((desc->flags & EFFECT_FLAG_HW_ACC_TUNNEL) == 0) {
                ALOGW("%s: non HW effect %s on playback thread in fast mode",
                        __func__, desc->name);
                return BAD_VALUE;
            }
        }
    } break;
    case OFFLOAD:
        // nothing actionable on offload threads, if the effect:
        //   - is offloadable: the effect can be created
        //   - is NOT offloadable: the effect should still be created, but EffectHandle::enable()
        //     will take care of invalidating the tracks of the thread
        break;
    case DIRECT:
        // Reject any effect on Direct output threads for now, since the format of
        // mSinkBuffer is not guaranteed to be compatible with effect processing (PCM 16 stereo).
        ALOGW("%s: effect %s on DIRECT output thread %s",
                __func__, desc->name, mThreadName);
        return BAD_VALUE;
    case DUPLICATING:
        if (audio_is_global_session(sessionId)) {
            ALOGW("%s: global effect %s on DUPLICATING thread %s",
                    __func__, desc->name, mThreadName);
            return BAD_VALUE;
        }
        if ((desc->flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_POST_PROC) {
            ALOGW("%s: post processing effect %s on DUPLICATING thread %s",
                __func__, desc->name, mThreadName);
            return BAD_VALUE;
        }
        if ((desc->flags & EFFECT_FLAG_HW_ACC_TUNNEL) != 0) {
            ALOGW("%s: HW tunneled effect %s on DUPLICATING thread %s",
                    __func__, desc->name, mThreadName);
            return BAD_VALUE;
        }
        break;
    case SPATIALIZER:
        // Global effects (AUDIO_SESSION_OUTPUT_MIX) are supported on spatializer mixer, but only
        // the spatialized track have global effects applied for now.
        // Post processing effects (AUDIO_SESSION_OUTPUT_STAGE or AUDIO_SESSION_DEVICE)
        // are supported and added after the spatializer.
        if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
            ALOGD("%s: global effect %s on spatializer thread %s", __func__, desc->name,
                  mThreadName);
        } else if (sessionId == AUDIO_SESSION_OUTPUT_STAGE) {
            // only post processing , downmixer or spatializer effects on output stage session
            if (IAfEffectModule::isSpatializer(&desc->type)
                    || memcmp(&desc->type, EFFECT_UIID_DOWNMIX, sizeof(effect_uuid_t)) == 0) {
                break;
            }
            if ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_POST_PROC) {
                ALOGW("%s: non post processing effect %s not allowed on output stage session",
                        __func__, desc->name);
                return BAD_VALUE;
            }
        } else if (sessionId == AUDIO_SESSION_DEVICE) {
            // only post processing on output stage session
            if ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_POST_PROC) {
                ALOGW("%s: non post processing effect %s not allowed on device session",
                        __func__, desc->name);
                return BAD_VALUE;
            }
        }
        break;
    case BIT_PERFECT:
        if ((desc->flags & EFFECT_FLAG_HW_ACC_TUNNEL) != 0) {
            // Allow HW accelerated effects of tunnel type
            break;
        }
        // As bit-perfect tracks will not be allowed to apply audio effect that will touch the audio
        // data, effects will not be allowed on 1) global effects (AUDIO_SESSION_OUTPUT_MIX),
        // 2) post-processing effects (AUDIO_SESSION_OUTPUT_STAGE or AUDIO_SESSION_DEVICE) and
        // 3) there is any bit-perfect track with the given session id.
        if (sessionId == AUDIO_SESSION_OUTPUT_MIX || sessionId == AUDIO_SESSION_OUTPUT_STAGE ||
            sessionId == AUDIO_SESSION_DEVICE) {
            ALOGW("%s: effect %s not supported on bit-perfect thread %s",
                  __func__, desc->name, mThreadName);
            return BAD_VALUE;
        } else if ((hasAudioSession_l(sessionId) & ThreadBase::BIT_PERFECT_SESSION) != 0) {
            ALOGW("%s: effect %s not supported as there is a bit-perfect track with session as %d",
                  __func__, desc->name, sessionId);
            return BAD_VALUE;
        }
        break;
    default:
        LOG_ALWAYS_FATAL("checkEffectCompatibility_l(): wrong thread type %d", mType);
    }

    return NO_ERROR;
}

// ThreadBase::createEffect_l() must be called with AudioFlinger::mutex() held
sp<IAfEffectHandle> ThreadBase::createEffect_l(
        const sp<Client>& client,
        const sp<IEffectClient>& effectClient,
        int32_t priority,
        audio_session_t sessionId,
        effect_descriptor_t *desc,
        int *enabled,
        status_t *status,
        bool pinned,
        bool probe,
        bool notifyFramesProcessed)
{
    sp<IAfEffectModule> effect;
    sp<IAfEffectHandle> handle;
    status_t lStatus;
    sp<IAfEffectChain> chain;
    bool chainCreated = false;
    bool effectCreated = false;
    audio_unique_id_t effectId = AUDIO_UNIQUE_ID_USE_UNSPECIFIED;

    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGW("createEffect_l() Audio driver not initialized.");
        goto Exit;
    }

    ALOGV("createEffect_l() thread %p effect %s on session %d", this, desc->name, sessionId);

    { // scope for mutex()
        audio_utils::lock_guard _l(mutex());

        lStatus = checkEffectCompatibility_l(desc, sessionId);
        if (probe || lStatus != NO_ERROR) {
            goto Exit;
        }

        // check for existing effect chain with the requested audio session
        chain = getEffectChain_l(sessionId);
        if (chain == 0) {
            // create a new chain for this session
            ALOGV("createEffect_l() new effect chain for session %d", sessionId);
            chain = IAfEffectChain::create(this, sessionId, mAfThreadCallback);
            addEffectChain_l(chain);
            chain->setStrategy(getStrategyForSession_l(sessionId));
            chainCreated = true;
        } else {
            effect = chain->getEffectFromDesc(desc);
        }

        ALOGV("createEffect_l() got effect %p on chain %p", effect.get(), chain.get());

        if (effect == 0) {
            effectId = mAfThreadCallback->nextUniqueId(AUDIO_UNIQUE_ID_USE_EFFECT);
            // create a new effect module if none present in the chain
            lStatus = chain->createEffect(effect, desc, effectId, sessionId, pinned);
            if (lStatus != NO_ERROR) {
                goto Exit;
            }
            effectCreated = true;

            // FIXME: use vector of device and address when effect interface is ready.
            effect->setDevices(outDeviceTypeAddrs());
            effect->setInputDevice(inDeviceTypeAddr());
            effect->setMode(mAfThreadCallback->getMode());
            effect->setAudioSource(mAudioSource);
        }
        if (effect->isHapticGenerator()) {
            // TODO(b/184194057): Use the vibrator information from the vibrator that will be used
            // for the HapticGenerator.
            const std::optional<media::AudioVibratorInfo> defaultVibratorInfo =
                    mAfThreadCallback->getDefaultVibratorInfo_l();
            if (defaultVibratorInfo) {
                audio_utils::lock_guard _cl(chain->mutex());
                // Only set the vibrator info when it is a valid one.
                effect->setVibratorInfo_l(*defaultVibratorInfo);
            }
        }
        // create effect handle and connect it to effect module
        handle = IAfEffectHandle::create(
                effect, client, effectClient, priority, notifyFramesProcessed);
        lStatus = handle->initCheck();
        if (lStatus == OK) {
            lStatus = effect->addHandle(handle.get());
            sendCheckOutputStageEffectsEvent_l();
        }
        if (enabled != NULL) {
            *enabled = (int)effect->isEnabled();
        }
    }

Exit:
    if (!probe && lStatus != NO_ERROR && lStatus != ALREADY_EXISTS) {
        audio_utils::lock_guard _l(mutex());
        if (effectCreated) {
            chain->removeEffect(effect);
        }
        if (chainCreated) {
            removeEffectChain_l(chain);
        }
        // handle must be cleared by caller to avoid deadlock.
    }

    *status = lStatus;
    return handle;
}

void ThreadBase::disconnectEffectHandle(IAfEffectHandle* handle,
                                                      bool unpinIfLast)
{
    bool remove = false;
    sp<IAfEffectModule> effect;
    {
        audio_utils::lock_guard _l(mutex());
        sp<IAfEffectBase> effectBase = handle->effect().promote();
        if (effectBase == nullptr) {
            return;
        }
        effect = effectBase->asEffectModule();
        if (effect == nullptr) {
            return;
        }
        // restore suspended effects if the disconnected handle was enabled and the last one.
        remove = (effect->removeHandle(handle) == 0) && (!effect->isPinned() || unpinIfLast);
        if (remove) {
            removeEffect_l(effect, true);
        }
        sendCheckOutputStageEffectsEvent_l();
    }
    if (remove) {
        mAfThreadCallback->updateOrphanEffectChains(effect);
        if (handle->enabled()) {
            effect->checkSuspendOnEffectEnabled(false, false /*threadLocked*/);
        }
    }
}

void ThreadBase::onEffectEnable(const sp<IAfEffectModule>& effect) {
    if (isOffloadOrMmap()) {
        audio_utils::lock_guard _l(mutex());
        broadcast_l();
    }
    if (!effect->isOffloadable()) {
        if (mType == ThreadBase::OFFLOAD) {
            PlaybackThread *t = (PlaybackThread *)this;
            t->invalidateTracks(AUDIO_STREAM_MUSIC);
        }
        if (effect->sessionId() == AUDIO_SESSION_OUTPUT_MIX) {
            mAfThreadCallback->onNonOffloadableGlobalEffectEnable();
        }
    }
}

void ThreadBase::onEffectDisable() {
    if (isOffloadOrMmap()) {
        audio_utils::lock_guard _l(mutex());
        broadcast_l();
    }
}

sp<IAfEffectModule> ThreadBase::getEffect(audio_session_t sessionId,
        int effectId) const
{
    audio_utils::lock_guard _l(mutex());
    return getEffect_l(sessionId, effectId);
}

sp<IAfEffectModule> ThreadBase::getEffect_l(audio_session_t sessionId,
        int effectId) const
{
    sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
    return chain != 0 ? chain->getEffectFromId_l(effectId) : 0;
}

std::vector<int> ThreadBase::getEffectIds_l(audio_session_t sessionId) const
{
    sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
    return chain != nullptr ? chain->getEffectIds_l() : std::vector<int>{};
}

// PlaybackThread::addEffect_ll() must be called with AudioFlinger::mutex() and
// ThreadBase::mutex() held
status_t ThreadBase::addEffect_ll(const sp<IAfEffectModule>& effect)
{
    // check for existing effect chain with the requested audio session
    audio_session_t sessionId = effect->sessionId();
    sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
    bool chainCreated = false;

    ALOGD_IF((mType == OFFLOAD) && !effect->isOffloadable(),
             "%s: on offloaded thread %p: effect %s does not support offload flags %#x",
             __func__, this, effect->desc().name, effect->desc().flags);

    if (chain == 0) {
        // create a new chain for this session
        ALOGV("%s: new effect chain for session %d", __func__, sessionId);
        chain = IAfEffectChain::create(this, sessionId, mAfThreadCallback);
        addEffectChain_l(chain);
        chain->setStrategy(getStrategyForSession_l(sessionId));
        chainCreated = true;
    }
    ALOGV("%s: %p chain %p effect %p", __func__, this, chain.get(), effect.get());

    if (chain->getEffectFromId_l(effect->id()) != 0) {
        ALOGW("%s: %p effect %s already present in chain %p",
                __func__, this, effect->desc().name, chain.get());
        return BAD_VALUE;
    }

    effect->setOffloaded_l(mType == OFFLOAD, mId);

    status_t status = chain->addEffect(effect);
    if (status != NO_ERROR) {
        if (chainCreated) {
            removeEffectChain_l(chain);
        }
        return status;
    }

    effect->setDevices(outDeviceTypeAddrs());
    effect->setInputDevice(inDeviceTypeAddr());
    effect->setMode(mAfThreadCallback->getMode());
    effect->setAudioSource(mAudioSource);

    return NO_ERROR;
}

void ThreadBase::removeEffect_l(const sp<IAfEffectModule>& effect, bool release) {

    ALOGV("%s %p effect %p", __FUNCTION__, this, effect.get());
    effect_descriptor_t desc = effect->desc();
    if ((desc.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
        detachAuxEffect_l(effect->id());
    }

    sp<IAfEffectChain> chain = effect->getCallback()->chain().promote();
    if (chain != 0) {
        // remove effect chain if removing last effect
        if (chain->removeEffect(effect, release) == 0) {
            removeEffectChain_l(chain);
        }
    } else {
        ALOGW("removeEffect_l() %p cannot promote chain for effect %p", this, effect.get());
    }
}

void ThreadBase::lockEffectChains_l(Vector<sp<IAfEffectChain>>& effectChains)
        NO_THREAD_SAFETY_ANALYSIS  // calls EffectChain::lock()
{
    effectChains = mEffectChains;
    for (const auto& effectChain : effectChains) {
        effectChain->mutex().lock();
    }
}

void ThreadBase::unlockEffectChains(const Vector<sp<IAfEffectChain>>& effectChains)
        NO_THREAD_SAFETY_ANALYSIS  // calls EffectChain::unlock()
{
    for (const auto& effectChain : effectChains) {
        effectChain->mutex().unlock();
    }
}

sp<IAfEffectChain> ThreadBase::getEffectChain(audio_session_t sessionId) const
{
    audio_utils::lock_guard _l(mutex());
    return getEffectChain_l(sessionId);
}

sp<IAfEffectChain> ThreadBase::getEffectChain_l(audio_session_t sessionId)
        const
{
    size_t size = mEffectChains.size();
    for (size_t i = 0; i < size; i++) {
        if (mEffectChains[i]->sessionId() == sessionId) {
            return mEffectChains[i];
        }
    }
    return 0;
}

void ThreadBase::setMode(audio_mode_t mode)
{
    audio_utils::lock_guard _l(mutex());
    size_t size = mEffectChains.size();
    for (size_t i = 0; i < size; i++) {
        mEffectChains[i]->setMode_l(mode);
    }
}

void ThreadBase::toAudioPortConfig(struct audio_port_config* config)
{
    config->type = AUDIO_PORT_TYPE_MIX;
    config->ext.mix.handle = mId;
    config->sample_rate = mSampleRate;
    config->format = mHALFormat;
    config->channel_mask = mChannelMask;
    config->config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE|AUDIO_PORT_CONFIG_CHANNEL_MASK|
                            AUDIO_PORT_CONFIG_FORMAT;
}

void ThreadBase::systemReady()
{
    audio_utils::lock_guard _l(mutex());
    if (mSystemReady) {
        return;
    }
    mSystemReady = true;

    for (size_t i = 0; i < mPendingConfigEvents.size(); i++) {
        sendConfigEvent_l(mPendingConfigEvents.editItemAt(i));
    }
    mPendingConfigEvents.clear();
}

template <typename T>
ssize_t ThreadBase::ActiveTracks<T>::add(const sp<T>& track) {
    ssize_t index = mActiveTracks.indexOf(track);
    if (index >= 0) {
        ALOGW("ActiveTracks<T>::add track %p already there", track.get());
        return index;
    }
    logTrack("add", track);
    mActiveTracksGeneration++;
    mLatestActiveTrack = track;
    track->beginBatteryAttribution();
    mHasChanged = true;
    return mActiveTracks.add(track);
}

template <typename T>
ssize_t ThreadBase::ActiveTracks<T>::remove(const sp<T>& track) {
    ssize_t index = mActiveTracks.remove(track);
    if (index < 0) {
        ALOGW("ActiveTracks<T>::remove nonexistent track %p", track.get());
        return index;
    }
    logTrack("remove", track);
    mActiveTracksGeneration++;
    track->endBatteryAttribution();
    // mLatestActiveTrack is not cleared even if is the same as track.
    mHasChanged = true;
#ifdef TEE_SINK
    track->dumpTee(-1 /* fd */, "_REMOVE");
#endif
    track->logEndInterval(); // log to MediaMetrics
    return index;
}

template <typename T>
void ThreadBase::ActiveTracks<T>::clear() {
    for (const sp<T> &track : mActiveTracks) {
        track->endBatteryAttribution();
        logTrack("clear", track);
    }
    mLastActiveTracksGeneration = mActiveTracksGeneration;
    if (!mActiveTracks.empty()) { mHasChanged = true; }
    mActiveTracks.clear();
    mLatestActiveTrack.clear();
}

template <typename T>
void ThreadBase::ActiveTracks<T>::updatePowerState_l(
        const sp<ThreadBase>& thread, bool force) {
    // Updates ActiveTracks client uids to the thread wakelock.
    if (mActiveTracksGeneration != mLastActiveTracksGeneration || force) {
        thread->updateWakeLockUids_l(getWakeLockUids());
        mLastActiveTracksGeneration = mActiveTracksGeneration;
    }
}

template <typename T>
bool ThreadBase::ActiveTracks<T>::readAndClearHasChanged() {
    bool hasChanged = mHasChanged;
    mHasChanged = false;

    for (const sp<T> &track : mActiveTracks) {
        // Do not short-circuit as all hasChanged states must be reset
        // as all the metadata are going to be sent
        hasChanged |= track->readAndClearHasChanged();
    }
    return hasChanged;
}

template <typename T>
void ThreadBase::ActiveTracks<T>::logTrack(
        const char *funcName, const sp<T> &track) const {
    if (mLocalLog != nullptr) {
        String8 result;
        track->appendDump(result, false /* active */);
        mLocalLog->log("AT::%-10s(%p) %s", funcName, track.get(), result.c_str());
    }
}

void ThreadBase::broadcast_l()
{
    // Thread could be blocked waiting for async
    // so signal it to handle state changes immediately
    // If threadLoop is currently unlocked a signal of mWaitWorkCV will
    // be lost so we also flag to prevent it blocking on mWaitWorkCV
    mSignalPending = true;
    mWaitWorkCV.notify_all();
}

// Call only from threadLoop() or when it is idle.
// Do not call from high performance code as this may do binder rpc to the MediaMetrics service.
void ThreadBase::sendStatistics(bool force)
NO_THREAD_SAFETY_ANALYSIS
{
    // Do not log if we have no stats.
    // We choose the timestamp verifier because it is the most likely item to be present.
    const int64_t nstats = mTimestampVerifier.getN() - mLastRecordedTimestampVerifierN;
    if (nstats == 0) {
        return;
    }

    // Don't log more frequently than once per 12 hours.
    // We use BOOTTIME to include suspend time.
    const int64_t timeNs = systemTime(SYSTEM_TIME_BOOTTIME);
    const int64_t sinceNs = timeNs - mLastRecordedTimeNs; // ok if mLastRecordedTimeNs = 0
    if (!force && sinceNs <= 12 * NANOS_PER_HOUR) {
        return;
    }

    mLastRecordedTimestampVerifierN = mTimestampVerifier.getN();
    mLastRecordedTimeNs = timeNs;

    std::unique_ptr<mediametrics::Item> item(mediametrics::Item::create("audiothread"));

#define MM_PREFIX "android.media.audiothread." // avoid cut-n-paste errors.

    // thread configuration
    item->setInt32(MM_PREFIX "id", (int32_t)mId); // IO handle
    // item->setInt32(MM_PREFIX "portId", (int32_t)mPortId);
    item->setCString(MM_PREFIX "type", threadTypeToString(mType));
    item->setInt32(MM_PREFIX "sampleRate", (int32_t)mSampleRate);
    item->setInt64(MM_PREFIX "channelMask", (int64_t)mChannelMask);
    item->setCString(MM_PREFIX "encoding", toString(mFormat).c_str());
    item->setInt32(MM_PREFIX "frameCount", (int32_t)mFrameCount);
    item->setCString(MM_PREFIX "outDevice", toString(outDeviceTypes_l()).c_str());
    item->setCString(MM_PREFIX "inDevice", toString(inDeviceType_l()).c_str());

    // thread statistics
    if (mIoJitterMs.getN() > 0) {
        item->setDouble(MM_PREFIX "ioJitterMs.mean", mIoJitterMs.getMean());
        item->setDouble(MM_PREFIX "ioJitterMs.std", mIoJitterMs.getStdDev());
    }
    if (mProcessTimeMs.getN() > 0) {
        item->setDouble(MM_PREFIX "processTimeMs.mean", mProcessTimeMs.getMean());
        item->setDouble(MM_PREFIX "processTimeMs.std", mProcessTimeMs.getStdDev());
    }
    const auto tsjitter = mTimestampVerifier.getJitterMs();
    if (tsjitter.getN() > 0) {
        item->setDouble(MM_PREFIX "timestampJitterMs.mean", tsjitter.getMean());
        item->setDouble(MM_PREFIX "timestampJitterMs.std", tsjitter.getStdDev());
    }
    if (mLatencyMs.getN() > 0) {
        item->setDouble(MM_PREFIX "latencyMs.mean", mLatencyMs.getMean());
        item->setDouble(MM_PREFIX "latencyMs.std", mLatencyMs.getStdDev());
    }
    if (mMonopipePipeDepthStats.getN() > 0) {
        item->setDouble(MM_PREFIX "monopipePipeDepthStats.mean",
                        mMonopipePipeDepthStats.getMean());
        item->setDouble(MM_PREFIX "monopipePipeDepthStats.std",
                        mMonopipePipeDepthStats.getStdDev());
    }

    item->selfrecord();
}

product_strategy_t ThreadBase::getStrategyForStream(audio_stream_type_t stream) const
{
    if (!mAfThreadCallback->isAudioPolicyReady()) {
        return PRODUCT_STRATEGY_NONE;
    }
    return AudioSystem::getStrategyForStream(stream);
}

// startMelComputation_l() must be called with AudioFlinger::mutex() held
void ThreadBase::startMelComputation_l(
        const sp<audio_utils::MelProcessor>& /*processor*/)
{
    // Do nothing
    ALOGW("%s: ThreadBase does not support CSD", __func__);
}

// stopMelComputation_l() must be called with AudioFlinger::mutex() held
void ThreadBase::stopMelComputation_l()
{
    // Do nothing
    ALOGW("%s: ThreadBase does not support CSD", __func__);
}

// ----------------------------------------------------------------------------
//      Playback
// ----------------------------------------------------------------------------

PlaybackThread::PlaybackThread(const sp<IAfThreadCallback>& afThreadCallback,
                                             AudioStreamOut* output,
                                             audio_io_handle_t id,
                                             type_t type,
                                             bool systemReady,
                                             audio_config_base_t *mixerConfig)
    :   ThreadBase(afThreadCallback, id, type, systemReady, true /* isOut */),
        mNormalFrameCount(0), mSinkBuffer(NULL),
        mMixerBufferEnabled(kEnableExtendedPrecision || type == SPATIALIZER),
        mMixerBuffer(NULL),
        mMixerBufferSize(0),
        mMixerBufferFormat(AUDIO_FORMAT_INVALID),
        mMixerBufferValid(false),
        mEffectBufferEnabled(kEnableExtendedPrecision || type == SPATIALIZER),
        mEffectBuffer(NULL),
        mEffectBufferSize(0),
        mEffectBufferFormat(AUDIO_FORMAT_INVALID),
        mEffectBufferValid(false),
        mSuspended(0), mBytesWritten(0),
        mFramesWritten(0),
        mSuspendedFrames(0),
        mActiveTracks(&this->mLocalLog),
        // mStreamTypes[] initialized in constructor body
        mTracks(type == MIXER),
        mOutput(output),
        mNumWrites(0), mNumDelayedWrites(0), mInWrite(false),
        mMixerStatus(MIXER_IDLE),
        mMixerStatusIgnoringFastTracks(MIXER_IDLE),
        mStandbyDelayNs(getStandbyTimeInNanos()),
        mBytesRemaining(0),
        mCurrentWriteLength(0),
        mUseAsyncWrite(false),
        mWriteAckSequence(0),
        mDrainSequence(0),
        mScreenState(mAfThreadCallback->getScreenState()),
        // index 0 is reserved for normal mixer's submix
        mFastTrackAvailMask(((1 << FastMixerState::sMaxFastTracks) - 1) & ~1),
        mHwSupportsPause(false), mHwPaused(false), mFlushPending(false),
        mLeftVolFloat(-1.0), mRightVolFloat(-1.0),
        mDownStreamPatch{},
        mIsTimestampAdvancing(kMinimumTimeBetweenTimestampChecksNs)
{
    snprintf(mThreadName, kThreadNameLength, "AudioOut_%X", id);
    mFlagsAsString = toString(output->flags);

    // Assumes constructor is called by AudioFlinger with its mutex() held, but
    // it would be safer to explicitly pass initial masterVolume/masterMute as
    // parameter.
    //
    // If the HAL we are using has support for master volume or master mute,
    // then do not attenuate or mute during mixing (just leave the volume at 1.0
    // and the mute set to false).
    mMasterVolume = afThreadCallback->masterVolume_l();
    mMasterMute = afThreadCallback->masterMute_l();
    if (mOutput->audioHwDev) {
        if (mOutput->audioHwDev->canSetMasterVolume()) {
            mMasterVolume = 1.0;
        }

        if (mOutput->audioHwDev->canSetMasterMute()) {
            mMasterMute = false;
        }
        mIsMsdDevice = strcmp(
                mOutput->audioHwDev->moduleName(), AUDIO_HARDWARE_MODULE_ID_MSD) == 0;
    }

    if (mixerConfig != nullptr && mixerConfig->channel_mask != AUDIO_CHANNEL_NONE) {
        mMixerChannelMask = mixerConfig->channel_mask;
    }

    readOutputParameters_l();

    if (mType != SPATIALIZER
            && mMixerChannelMask != mChannelMask) {
        LOG_ALWAYS_FATAL("HAL channel mask %#x does not match mixer channel mask %#x",
                mChannelMask, mMixerChannelMask);
    }

    // TODO: We may also match on address as well as device type for
    // AUDIO_DEVICE_OUT_BUS, AUDIO_DEVICE_OUT_ALL_A2DP, AUDIO_DEVICE_OUT_REMOTE_SUBMIX
    if (type == MIXER || type == DIRECT || type == OFFLOAD) {
        // TODO: This property should be ensure that only contains one single device type.
        mTimestampCorrectedDevice = (audio_devices_t)property_get_int64(
                "audio.timestamp.corrected_output_device",
                (int64_t)(mIsMsdDevice ? AUDIO_DEVICE_OUT_BUS // turn on by default for MSD
                                       : AUDIO_DEVICE_NONE));
    }
    if (!audioserver_flags::portid_volume_management()) {
        for (int i = AUDIO_STREAM_MIN; i < AUDIO_STREAM_FOR_POLICY_CNT; ++i) {
            const audio_stream_type_t stream{static_cast<audio_stream_type_t>(i)};
            mStreamTypes[stream].volume = 0.0f;
            mStreamTypes[stream].mute = mAfThreadCallback->streamMute_l(stream);
        }
        // Audio patch and call assistant volume are always max
        mStreamTypes[AUDIO_STREAM_PATCH].volume = 1.0f;
        mStreamTypes[AUDIO_STREAM_PATCH].mute = false;
        mStreamTypes[AUDIO_STREAM_CALL_ASSISTANT].volume = 1.0f;
        mStreamTypes[AUDIO_STREAM_CALL_ASSISTANT].mute = false;
    }
}

PlaybackThread::~PlaybackThread()
{
    free(mSinkBuffer);
    free(mMixerBuffer);
    free(mEffectBuffer);
    free(mPostSpatializerBuffer);
}

// Thread virtuals

void PlaybackThread::onFirstRef()
{
    if (!isStreamInitialized()) {
        ALOGE("The stream is not open yet"); // This should not happen.
    } else {
        // Callbacks take strong or weak pointers as a parameter.
        // Since PlaybackThread passes itself as a callback handler, it can only
        // be done outside of the constructor. Creating weak and especially strong
        // pointers to a refcounted object in its own constructor is strongly
        // discouraged, see comments in system/core/libutils/include/utils/RefBase.h.
        // Even if a function takes a weak pointer, it is possible that it will
        // need to convert it to a strong pointer down the line.
        if (mOutput->flags & AUDIO_OUTPUT_FLAG_NON_BLOCKING &&
                mOutput->stream->setCallback(this) == OK) {
            mUseAsyncWrite = true;
            mCallbackThread = sp<AsyncCallbackThread>::make(this);
        }

        if (mOutput->stream->setEventCallback(this) != OK) {
            ALOGD("Failed to add event callback");
        }
    }
    run(mThreadName, ANDROID_PRIORITY_URGENT_AUDIO);
    mThreadSnapshot.setTid(getTid());
}

// ThreadBase virtuals
void PlaybackThread::preExit()
{
    ALOGV("  preExit()");
    status_t result = mOutput->stream->exit();
    ALOGE_IF(result != OK, "Error when calling exit(): %d", result);
}

void PlaybackThread::dumpTracks_l(int fd, const Vector<String16>& /* args */)
{
    String8 result;
    if (!audioserver_flags::portid_volume_management()) {
        result.appendFormat("  Stream volumes in dB: ");
        for (int i = 0; i < AUDIO_STREAM_CNT; ++i) {
            const stream_type_t *st = &mStreamTypes[i];
            if (i > 0) {
                result.appendFormat(", ");
            }
            result.appendFormat("%d:%.2g", i, 20.0 * log10(st->volume));
            if (st->mute) {
                result.append("M");
            }
        }
    }
    result.append("\n");
    write(fd, result.c_str(), result.length());
    result.clear();

    // These values are "raw"; they will wrap around.  See prepareTracks_l() for a better way.
    FastTrackUnderruns underruns = getFastTrackUnderruns(0);
    dprintf(fd, "  Normal mixer raw underrun counters: partial=%u empty=%u\n",
            underruns.mBitFields.mPartial, underruns.mBitFields.mEmpty);

    size_t numtracks = mTracks.size();
    size_t numactive = mActiveTracks.size();
    dprintf(fd, "  %zu Tracks", numtracks);
    size_t numactiveseen = 0;
    const char *prefix = "    ";
    if (numtracks) {
        dprintf(fd, " of which %zu are active\n", numactive);
        result.append(prefix);
        mTracks[0]->appendDumpHeader(result);
        for (size_t i = 0; i < numtracks; ++i) {
            sp<IAfTrack> track = mTracks[i];
            if (track != 0) {
                bool active = mActiveTracks.indexOf(track) >= 0;
                if (active) {
                    numactiveseen++;
                }
                result.append(prefix);
                track->appendDump(result, active);
            }
        }
    } else {
        result.append("\n");
    }
    if (numactiveseen != numactive) {
        // some tracks in the active list were not in the tracks list
        result.append("  The following tracks are in the active list but"
                " not in the track list\n");
        result.append(prefix);
        mActiveTracks[0]->appendDumpHeader(result);
        for (size_t i = 0; i < numactive; ++i) {
            sp<IAfTrack> track = mActiveTracks[i];
            if (mTracks.indexOf(track) < 0) {
                result.append(prefix);
                track->appendDump(result, true /* active */);
            }
        }
    }

    write(fd, result.c_str(), result.size());
}

void PlaybackThread::dumpInternals_l(int fd, const Vector<String16>& args)
{
    dprintf(fd, "  Master volume: %f\n", mMasterVolume);
    dprintf(fd, "  Master mute: %s\n", mMasterMute ? "on" : "off");
    dprintf(fd, "  Mixer channel Mask: %#x (%s)\n",
            mMixerChannelMask, channelMaskToString(mMixerChannelMask, true /* output */).c_str());
    if (mHapticChannelMask != AUDIO_CHANNEL_NONE) {
        dprintf(fd, "  Haptic channel mask: %#x (%s)\n", mHapticChannelMask,
                channelMaskToString(mHapticChannelMask, true /* output */).c_str());
    }
    dprintf(fd, "  Normal frame count: %zu\n", mNormalFrameCount);
    dprintf(fd, "  Total writes: %d\n", mNumWrites);
    dprintf(fd, "  Delayed writes: %d\n", mNumDelayedWrites);
    dprintf(fd, "  Blocked in write: %s\n", mInWrite ? "yes" : "no");
    dprintf(fd, "  Suspend count: %d\n", (int32_t)mSuspended);
    dprintf(fd, "  Fast track availMask=%#x\n", mFastTrackAvailMask);
    dprintf(fd, "  Standby delay ns=%lld\n", (long long)mStandbyDelayNs);
    AudioStreamOut *output = mOutput;
    audio_output_flags_t flags = output != NULL ? output->flags : AUDIO_OUTPUT_FLAG_NONE;
    dprintf(fd, "  AudioStreamOut: %p flags %#x (%s)\n",
            output, flags, toString(flags).c_str());
    dprintf(fd, "  Frames written: %lld\n", (long long)mFramesWritten);
    dprintf(fd, "  Suspended frames: %lld\n", (long long)mSuspendedFrames);
    if (mPipeSink.get() != nullptr) {
        dprintf(fd, "  PipeSink frames written: %lld\n", (long long)mPipeSink->framesWritten());
    }
    if (output != nullptr) {
        dprintf(fd, "  Hal stream dump:\n");
        (void)output->stream->dump(fd, args);
    }
}

// PlaybackThread::createTrack_l() must be called with AudioFlinger::mutex() held
sp<IAfTrack> PlaybackThread::createTrack_l(
        const sp<Client>& client,
        audio_stream_type_t streamType,
        const audio_attributes_t& attr,
        uint32_t *pSampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t *pFrameCount,
        size_t *pNotificationFrameCount,
        uint32_t notificationsPerBuffer,
        float speed,
        const sp<IMemory>& sharedBuffer,
        audio_session_t sessionId,
        audio_output_flags_t *flags,
        pid_t creatorPid,
        const AttributionSourceState& attributionSource,
        pid_t tid,
        status_t *status,
        audio_port_handle_t portId,
        const sp<media::IAudioTrackCallback>& callback,
        bool isSpatialized,
        bool isBitPerfect,
        audio_output_flags_t *afTrackFlags,
        float volume,
        bool muted)
{
    size_t frameCount = *pFrameCount;
    size_t notificationFrameCount = *pNotificationFrameCount;
    sp<IAfTrack> track;
    status_t lStatus;
    audio_output_flags_t outputFlags = mOutput->flags;
    audio_output_flags_t requestedFlags = *flags;
    uint32_t sampleRate;

    if (sharedBuffer != 0 && checkIMemory(sharedBuffer) != NO_ERROR) {
        lStatus = BAD_VALUE;
        goto Exit;
    }

    if (*pSampleRate == 0) {
        *pSampleRate = mSampleRate;
    }
    sampleRate = *pSampleRate;

    // special case for FAST flag considered OK if fast mixer is present
    if (hasFastMixer()) {
        outputFlags = (audio_output_flags_t)(outputFlags | AUDIO_OUTPUT_FLAG_FAST);
    }

    // Check if requested flags are compatible with output stream flags
    if ((*flags & outputFlags) != *flags) {
        ALOGW("createTrack_l(): mismatch between requested flags (%08x) and output flags (%08x)",
              *flags, outputFlags);
        *flags = (audio_output_flags_t)(*flags & outputFlags);
    }

    if (isBitPerfect) {
        audio_utils::lock_guard _l(mutex());
        sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
        if (chain.get() != nullptr) {
            // Bit-perfect is required according to the configuration and preferred mixer
            // attributes, but it is not in the output flag from the client's request. Explicitly
            // adding bit-perfect flag to check the compatibility
            audio_output_flags_t flagsToCheck =
                    (audio_output_flags_t)(*flags & AUDIO_OUTPUT_FLAG_BIT_PERFECT);
            chain->checkOutputFlagCompatibility(&flagsToCheck);
            if ((flagsToCheck & AUDIO_OUTPUT_FLAG_BIT_PERFECT) == AUDIO_OUTPUT_FLAG_NONE) {
                ALOGE("%s cannot create track as there is data-processing effect attached to "
                      "given session id(%d)", __func__, sessionId);
                lStatus = BAD_VALUE;
                goto Exit;
            }
            *flags = flagsToCheck;
        }
    }

    // client expresses a preference for FAST, but we get the final say
    if (*flags & AUDIO_OUTPUT_FLAG_FAST) {
      if (
            // PCM data
            audio_is_linear_pcm(format) &&
            // TODO: extract as a data library function that checks that a computationally
            // expensive downmixer is not required: isFastOutputChannelConversion()
            (channelMask == (mChannelMask | mHapticChannelMask) ||
                    mChannelMask != AUDIO_CHANNEL_OUT_STEREO ||
                    (channelMask == AUDIO_CHANNEL_OUT_MONO
                            /* && mChannelMask == AUDIO_CHANNEL_OUT_STEREO */)) &&
            // hardware sample rate
            (sampleRate == mSampleRate) &&
            // normal mixer has an associated fast mixer
            hasFastMixer() &&
            // there are sufficient fast track slots available
            (mFastTrackAvailMask != 0)
            // FIXME test that MixerThread for this fast track has a capable output HAL
            // FIXME add a permission test also?
        ) {
        // static tracks can have any nonzero framecount, streaming tracks check against minimum.
        if (sharedBuffer == 0) {
            // read the fast track multiplier property the first time it is needed
            int ok = pthread_once(&sFastTrackMultiplierOnce, sFastTrackMultiplierInit);
            if (ok != 0) {
                ALOGE("%s pthread_once failed: %d", __func__, ok);
            }
            frameCount = max(frameCount, mFrameCount * sFastTrackMultiplier); // incl framecount 0
        }

        // check compatibility with audio effects.
        { // scope for mutex()
            audio_utils::lock_guard _l(mutex());
            for (audio_session_t session : {
                    AUDIO_SESSION_DEVICE,
                    AUDIO_SESSION_OUTPUT_STAGE,
                    AUDIO_SESSION_OUTPUT_MIX,
                    sessionId,
                }) {
                sp<IAfEffectChain> chain = getEffectChain_l(session);
                if (chain.get() != nullptr) {
                    audio_output_flags_t old = *flags;
                    chain->checkOutputFlagCompatibility(flags);
                    if (old != *flags) {
                        ALOGV("AUDIO_OUTPUT_FLAGS denied by effect, session=%d old=%#x new=%#x",
                                (int)session, (int)old, (int)*flags);
                    }
                }
            }
        }
        ALOGV_IF((*flags & AUDIO_OUTPUT_FLAG_FAST) != 0,
                 "AUDIO_OUTPUT_FLAG_FAST accepted: frameCount=%zu mFrameCount=%zu",
                 frameCount, mFrameCount);
      } else {
        ALOGD("AUDIO_OUTPUT_FLAG_FAST denied: sharedBuffer=%p frameCount=%zu "
                "mFrameCount=%zu format=%#x mFormat=%#x isLinear=%d channelMask=%#x "
                "sampleRate=%u mSampleRate=%u "
                "hasFastMixer=%d tid=%d fastTrackAvailMask=%#x",
                sharedBuffer.get(), frameCount, mFrameCount, format, mFormat,
                audio_is_linear_pcm(format), channelMask, sampleRate,
                mSampleRate, hasFastMixer(), tid, mFastTrackAvailMask);
        *flags = (audio_output_flags_t)(*flags & ~AUDIO_OUTPUT_FLAG_FAST);
      }
    }

    if (!audio_has_proportional_frames(format)) {
        if (sharedBuffer != 0) {
            // Same comment as below about ignoring frameCount parameter for set()
            frameCount = sharedBuffer->size();
        } else if (frameCount == 0) {
            frameCount = mNormalFrameCount;
        }
        if (notificationFrameCount != frameCount) {
            notificationFrameCount = frameCount;
        }
    } else if (sharedBuffer != 0) {
        // FIXME: Ensure client side memory buffers need
        // not have additional alignment beyond sample
        // (e.g. 16 bit stereo accessed as 32 bit frame).
        size_t alignment = audio_bytes_per_sample(format);
        if (alignment & 1) {
            // for AUDIO_FORMAT_PCM_24_BIT_PACKED (not exposed through Java).
            alignment = 1;
        }
        uint32_t channelCount = audio_channel_count_from_out_mask(channelMask);
        size_t frameSize = channelCount * audio_bytes_per_sample(format);
        if (channelCount > 1) {
            // More than 2 channels does not require stronger alignment than stereo
            alignment <<= 1;
        }
        if (((uintptr_t)sharedBuffer->unsecurePointer() & (alignment - 1)) != 0) {
            ALOGE("Invalid buffer alignment: address %p, channel count %u",
                  sharedBuffer->unsecurePointer(), channelCount);
            lStatus = BAD_VALUE;
            goto Exit;
        }

        // When initializing a shared buffer AudioTrack via constructors,
        // there's no frameCount parameter.
        // But when initializing a shared buffer AudioTrack via set(),
        // there _is_ a frameCount parameter.  We silently ignore it.
        frameCount = sharedBuffer->size() / frameSize;
    } else {
        size_t minFrameCount = 0;
        // For fast tracks we try to respect the application's request for notifications per buffer.
        if (*flags & AUDIO_OUTPUT_FLAG_FAST) {
            if (notificationsPerBuffer > 0) {
                // Avoid possible arithmetic overflow during multiplication.
                if (notificationsPerBuffer > SIZE_MAX / mFrameCount) {
                    ALOGE("Requested notificationPerBuffer=%u ignored for HAL frameCount=%zu",
                          notificationsPerBuffer, mFrameCount);
                } else {
                    minFrameCount = mFrameCount * notificationsPerBuffer;
                }
            }
        } else {
            // For normal PCM streaming tracks, update minimum frame count.
            // Buffer depth is forced to be at least 2 x the normal mixer frame count and
            // cover audio hardware latency.
            // This is probably too conservative, but legacy application code may depend on it.
            // If you change this calculation, also review the start threshold which is related.
            uint32_t latencyMs = latency_l();
            if (latencyMs == 0) {
                ALOGE("Error when retrieving output stream latency");
                lStatus = UNKNOWN_ERROR;
                goto Exit;
            }

            minFrameCount = AudioSystem::calculateMinFrameCount(latencyMs, mNormalFrameCount,
                                mSampleRate, sampleRate, speed /*, 0 mNotificationsPerBufferReq*/);

        }
        if (frameCount < minFrameCount) {
            frameCount = minFrameCount;
        }
    }

    // Make sure that application is notified with sufficient margin before underrun.
    // The client can divide the AudioTrack buffer into sub-buffers,
    // and expresses its desire to server as the notification frame count.
    if (sharedBuffer == 0 && audio_is_linear_pcm(format)) {
        size_t maxNotificationFrames;
        if (*flags & AUDIO_OUTPUT_FLAG_FAST) {
            // notify every HAL buffer, regardless of the size of the track buffer
            maxNotificationFrames = mFrameCount;
        } else {
            // Triple buffer the notification period for a triple buffered mixer period;
            // otherwise, double buffering for the notification period is fine.
            //
            // TODO: This should be moved to AudioTrack to modify the notification period
            // on AudioTrack::setBufferSizeInFrames() changes.
            const int nBuffering =
                    (uint64_t{frameCount} * mSampleRate)
                            / (uint64_t{mNormalFrameCount} * sampleRate) == 3 ? 3 : 2;

            maxNotificationFrames = frameCount / nBuffering;
            // If client requested a fast track but this was denied, then use the smaller maximum.
            if (requestedFlags & AUDIO_OUTPUT_FLAG_FAST) {
                size_t maxNotificationFramesFastDenied = FMS_20 * sampleRate / 1000;
                if (maxNotificationFrames > maxNotificationFramesFastDenied) {
                    maxNotificationFrames = maxNotificationFramesFastDenied;
                }
            }
        }
        if (notificationFrameCount == 0 || notificationFrameCount > maxNotificationFrames) {
            if (notificationFrameCount == 0) {
                ALOGD("Client defaulted notificationFrames to %zu for frameCount %zu",
                    maxNotificationFrames, frameCount);
            } else {
                ALOGW("Client adjusted notificationFrames from %zu to %zu for frameCount %zu",
                      notificationFrameCount, maxNotificationFrames, frameCount);
            }
            notificationFrameCount = maxNotificationFrames;
        }
    }

    *pFrameCount = frameCount;
    *pNotificationFrameCount = notificationFrameCount;

    switch (mType) {
    case BIT_PERFECT:
        if (isBitPerfect) {
            if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
                ALOGE("%s, bad parameter when request streaming bit-perfect, sampleRate=%u, "
                      "format=%#x, channelMask=%#x, mSampleRate=%u, mFormat=%#x, mChannelMask=%#x",
                      __func__, sampleRate, format, channelMask, mSampleRate, mFormat,
                      mChannelMask);
                lStatus = BAD_VALUE;
                goto Exit;
            }
        }
        break;

    case DIRECT:
        if (audio_is_linear_pcm(format)) { // TODO maybe use audio_has_proportional_frames()?
            if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
                ALOGE("createTrack_l() Bad parameter: sampleRate %u format %#x, channelMask 0x%08x "
                        "for output %p with format %#x",
                        sampleRate, format, channelMask, mOutput, mFormat);
                lStatus = BAD_VALUE;
                goto Exit;
            }
        }
        break;

    case OFFLOAD:
        if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
            ALOGE("createTrack_l() Bad parameter: sampleRate %d format %#x, channelMask 0x%08x \""
                    "for output %p with format %#x",
                    sampleRate, format, channelMask, mOutput, mFormat);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        break;

    default:
        if (!audio_is_linear_pcm(format)) {
                ALOGE("createTrack_l() Bad parameter: format %#x \""
                        "for output %p with format %#x",
                        format, mOutput, mFormat);
                lStatus = BAD_VALUE;
                goto Exit;
        }
        if (sampleRate > mSampleRate * AUDIO_RESAMPLER_DOWN_RATIO_MAX) {
            ALOGE("Sample rate out of range: %u mSampleRate %u", sampleRate, mSampleRate);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        break;

    }

    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGE("createTrack_l() audio driver not initialized");
        goto Exit;
    }

    { // scope for mutex()
        audio_utils::lock_guard _l(mutex());

        // all tracks in same audio session must share the same routing strategy otherwise
        // conflicts will happen when tracks are moved from one output to another by audio policy
        // manager
        product_strategy_t strategy = getStrategyForStream(streamType);
        for (size_t i = 0; i < mTracks.size(); ++i) {
            sp<IAfTrack> t = mTracks[i];
            if (t != 0 && t->isExternalTrack()) {
                product_strategy_t actual = getStrategyForStream(t->streamType());
                if (sessionId == t->sessionId() && strategy != actual) {
                    ALOGE("createTrack_l() mismatched strategy; expected %u but found %u",
                            strategy, actual);
                    lStatus = BAD_VALUE;
                    goto Exit;
                }
            }
        }

        // Set DIRECT/OFFLOAD flag if current thread is DirectOutputThread/OffloadThread.
        // This can happen when the playback is rerouted to direct output/offload thread by
        // dynamic audio policy.
        // Do NOT report the flag changes back to client, since the client
        // doesn't explicitly request a direct/offload flag.
        audio_output_flags_t trackFlags = *flags;
        if (mType == DIRECT) {
            trackFlags = static_cast<audio_output_flags_t>(trackFlags | AUDIO_OUTPUT_FLAG_DIRECT);
        } else if (mType == OFFLOAD) {
            trackFlags = static_cast<audio_output_flags_t>(trackFlags |
                                   AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD | AUDIO_OUTPUT_FLAG_DIRECT);
        }
        *afTrackFlags = trackFlags;

        track = IAfTrack::create(this, client, streamType, attr, sampleRate, format,
                          channelMask, frameCount,
                          nullptr /* buffer */, (size_t)0 /* bufferSize */, sharedBuffer,
                          sessionId, creatorPid, attributionSource, trackFlags,
                          IAfTrackBase::TYPE_DEFAULT, portId, SIZE_MAX /*frameCountToBeReady*/,
                          speed, isSpatialized, isBitPerfect, volume, muted);

        lStatus = track != 0 ? track->initCheck() : (status_t) NO_MEMORY;
        if (lStatus != NO_ERROR) {
            ALOGE("createTrack_l() initCheck failed %d; no control block?", lStatus);
            // track must be cleared from the caller as the caller has the AF lock
            goto Exit;
        }
        mTracks.add(track);
        {
            audio_utils::lock_guard _atCbL(audioTrackCbMutex());
            if (callback.get() != nullptr) {
                mAudioTrackCallbacks.emplace(track, callback);
            }
        }

        sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
        if (chain != 0) {
            ALOGV("createTrack_l() setting main buffer %p", chain->inBuffer());
            track->setMainBuffer(chain->inBuffer());
            chain->setStrategy(getStrategyForStream(track->streamType()));
            chain->incTrackCnt();
        }

        if ((*flags & AUDIO_OUTPUT_FLAG_FAST) && (tid != -1)) {
            pid_t callingPid = IPCThreadState::self()->getCallingPid();
            // we don't have CAP_SYS_NICE, nor do we want to have it as it's too powerful,
            // so ask activity manager to do this on our behalf
            sendPrioConfigEvent_l(callingPid, tid, kPriorityAudioApp, true /*forApp*/);
        }
    }

    lStatus = NO_ERROR;

Exit:
    *status = lStatus;
    return track;
}

template<typename T>
ssize_t PlaybackThread::Tracks<T>::remove(const sp<T>& track)
{
    const int trackId = track->id();
    const ssize_t index = mTracks.remove(track);
    if (index >= 0) {
        if (mSaveDeletedTrackIds) {
            // We can't directly access mAudioMixer since the caller may be outside of threadLoop.
            // Instead, we add to mDeletedTrackIds which is solely used for mAudioMixer update,
            // to be handled when MixerThread::prepareTracks_l() next changes mAudioMixer.
            mDeletedTrackIds.emplace(trackId);
        }
    }
    return index;
}

uint32_t PlaybackThread::correctLatency_l(uint32_t latency) const
{
    return latency;
}

uint32_t PlaybackThread::latency() const
{
    audio_utils::lock_guard _l(mutex());
    return latency_l();
}
uint32_t PlaybackThread::latency_l() const
NO_THREAD_SAFETY_ANALYSIS
// Fix later.
{
    uint32_t latency;
    if (initCheck() == NO_ERROR && mOutput->stream->getLatency(&latency) == OK) {
        return correctLatency_l(latency);
    }
    return 0;
}

void PlaybackThread::setMasterVolume(float value)
{
    audio_utils::lock_guard _l(mutex());
    // Don't apply master volume in SW if our HAL can do it for us.
    if (mOutput && mOutput->audioHwDev &&
        mOutput->audioHwDev->canSetMasterVolume()) {
        mMasterVolume = 1.0;
    } else {
        mMasterVolume = value;
    }
}

void PlaybackThread::setMasterBalance(float balance)
{
    mMasterBalance.store(balance);
}

void PlaybackThread::setMasterMute(bool muted)
{
    if (isDuplicating()) {
        return;
    }
    audio_utils::lock_guard _l(mutex());
    // Don't apply master mute in SW if our HAL can do it for us.
    if (mOutput && mOutput->audioHwDev &&
        mOutput->audioHwDev->canSetMasterMute()) {
        mMasterMute = false;
    } else {
        mMasterMute = muted;
    }
}

void PlaybackThread::setStreamVolume(audio_stream_type_t stream, float value, bool muted)
{
    ALOGV("%s: stream %d value %f muted %d", __func__, stream, value, muted);
    audio_utils::lock_guard _l(mutex());
    mStreamTypes[stream].volume = value;
    if (com_android_media_audio_ring_my_car()) {
        mStreamTypes[stream].mute = muted;
    }
    broadcast_l();
}

void PlaybackThread::setStreamMute(audio_stream_type_t stream, bool muted)
{
    audio_utils::lock_guard _l(mutex());
    mStreamTypes[stream].mute = muted;
    broadcast_l();
}

float PlaybackThread::streamVolume(audio_stream_type_t stream) const
{
    audio_utils::lock_guard _l(mutex());
    return mStreamTypes[stream].volume;
}

status_t PlaybackThread::setPortsVolume(
        const std::vector<audio_port_handle_t>& portIds, float volume, bool muted) {
    audio_utils::lock_guard _l(mutex());
    for (const auto& portId : portIds) {
        for (size_t i = 0; i < mTracks.size(); i++) {
            sp<IAfTrack> track = mTracks[i].get();
            if (portId == track->portId()) {
                track->setPortVolume(volume);
                track->setPortMute(muted);
                break;
            }
        }
    }
    broadcast_l();
    return NO_ERROR;
}

void PlaybackThread::setVolumeForOutput_l(float left, float right) const
{
    mOutput->stream->setVolume(left, right);
}

void PlaybackThread::checkUpdateTrackMetadataForUid(uid_t uid) {
    audio_utils::lock_guard _l(mutex());
    for (const sp<IAfTrack>& track : mActiveTracks) {
        if (track->uid() == uid) {
            track->setMetadataHasChanged();
        }
    }
}

// addTrack_l() must be called with ThreadBase::mutex() held
status_t PlaybackThread::addTrack_l(const sp<IAfTrack>& track)
{
    status_t status = ALREADY_EXISTS;

    if (mActiveTracks.indexOf(track) < 0) {
        // the track is newly added, make sure it fills up all its
        // buffers before playing. This is to ensure the client will
        // effectively get the latency it requested.
        if (track->isExternalTrack()) {
            IAfTrackBase::track_state state = track->state();
            // Because the track is not on the ActiveTracks,
            // at this point, only the TrackHandle will be adding the track.
            mutex().unlock();
            status = AudioSystem::startOutput(track->portId());
            mutex().lock();
            // abort track was stopped/paused while we released the lock
            if (state != track->state()) {
                if (status == NO_ERROR) {
                    mutex().unlock();
                    AudioSystem::stopOutput(track->portId());
                    mutex().lock();
                }
                return INVALID_OPERATION;
            }
            // abort if start is rejected by audio policy manager
            if (status != NO_ERROR) {
                // Do not replace the error if it is DEAD_OBJECT. When this happens, it indicates
                // current playback thread is reopened, which may happen when clients set preferred
                // mixer configuration. Returning DEAD_OBJECT will make the client restore track
                // immediately.
                return status == DEAD_OBJECT ? status : PERMISSION_DENIED;
            }
#ifdef ADD_BATTERY_DATA
            // to track the speaker usage
            addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStart);
#endif
            sendIoConfigEvent_l(AUDIO_CLIENT_STARTED, track->creatorPid(), track->portId());
        }

        // set retry count for buffer fill
        if (track->isOffloaded()) {
            if (track->isStopping_1()) {
                track->retryCount() = kMaxTrackStopRetriesOffload;
            } else {
                track->retryCount() = kMaxTrackStartupRetriesOffload;
            }
            track->fillingStatus() = mStandby ? IAfTrack::FS_FILLING : IAfTrack::FS_FILLED;
        } else {
            track->retryCount() = kMaxTrackStartupRetries;
            track->fillingStatus() =
                    track->sharedBuffer() != 0 ? IAfTrack::FS_FILLED : IAfTrack::FS_FILLING;
        }

        sp<IAfEffectChain> chain = getEffectChain_l(track->sessionId());
        if (mHapticChannelMask != AUDIO_CHANNEL_NONE
                && ((track->channelMask() & AUDIO_CHANNEL_HAPTIC_ALL) != AUDIO_CHANNEL_NONE
                        || (chain != nullptr && chain->containsHapticGeneratingEffect()))) {
            // Unlock due to VibratorService will lock for this call and will
            // call Tracks.mute/unmute which also require thread's lock.
            mutex().unlock();
            const os::HapticScale hapticScale = afutils::onExternalVibrationStart(
                    track->getExternalVibration());
            std::optional<media::AudioVibratorInfo> vibratorInfo;
            {
                // TODO(b/184194780): Use the vibrator information from the vibrator that will be
                // used to play this track.
                 audio_utils::lock_guard _l(mAfThreadCallback->mutex());
                vibratorInfo = mAfThreadCallback->getDefaultVibratorInfo_l();
            }
            mutex().lock();
            track->setHapticScale(hapticScale);
            if (vibratorInfo) {
                track->setHapticMaxAmplitude(vibratorInfo->maxAmplitude);
            }

            // Haptic playback should be enabled by vibrator service.
            if (track->getHapticPlaybackEnabled()) {
                // Disable haptic playback of all active track to ensure only
                // one track playing haptic if current track should play haptic.
                for (const auto &t : mActiveTracks) {
                    t->setHapticPlaybackEnabled(false);
                }
            }

            // Set haptic intensity for effect
            if (chain != nullptr) {
                chain->setHapticScale_l(track->id(), hapticScale);
            }
        }

        track->setResetDone(false);
        track->resetPresentationComplete();

        // Do not release the ThreadBase mutex after the track is added to mActiveTracks unless
        // all key changes are complete.  It is possible that the threadLoop will begin
        // processing the added track immediately after the ThreadBase mutex is released.
        mActiveTracks.add(track);

        if (chain != 0) {
            ALOGV("addTrack_l() starting track on chain %p for session %d", chain.get(),
                    track->sessionId());
            chain->incActiveTrackCnt();
        }

        track->logBeginInterval(patchSinksToString(&mPatch)); // log to MediaMetrics
        status = NO_ERROR;
    }

    onAddNewTrack_l();
    return status;
}

bool PlaybackThread::destroyTrack_l(const sp<IAfTrack>& track)
{
    track->terminate();
    // active tracks are removed by threadLoop()
    bool trackActive = (mActiveTracks.indexOf(track) >= 0);
    track->setState(IAfTrackBase::STOPPED);
    if (!trackActive) {
        removeTrack_l(track);
    } else if (track->isFastTrack() || track->isOffloaded() || track->isDirect()) {
        if (track->isPausePending()) {
            track->pauseAck();
        }
        track->setState(IAfTrackBase::STOPPING_1);
    }

    return trackActive;
}

void PlaybackThread::removeTrack_l(const sp<IAfTrack>& track)
{
    track->triggerEvents(AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE);

    String8 result;
    track->appendDump(result, false /* active */);
    mLocalLog.log("removeTrack_l (%p) %s", track.get(), result.c_str());

    mTracks.remove(track);
    {
        audio_utils::lock_guard _atCbL(audioTrackCbMutex());
        mAudioTrackCallbacks.erase(track);
    }
    if (track->isFastTrack()) {
        int index = track->fastIndex();
        ALOG_ASSERT(0 < index && index < (int)FastMixerState::sMaxFastTracks);
        ALOG_ASSERT(!(mFastTrackAvailMask & (1 << index)));
        mFastTrackAvailMask |= 1 << index;
        // redundant as track is about to be destroyed, for dumpsys only
        track->fastIndex() = -1;
    }
    sp<IAfEffectChain> chain = getEffectChain_l(track->sessionId());
    if (chain != 0) {
        chain->decTrackCnt();
    }
}

std::set<audio_port_handle_t> PlaybackThread::getTrackPortIds_l()
{
    std::set<int32_t> result;
    for (const auto& t : mTracks) {
        if (t->isExternalTrack()) {
            result.insert(t->portId());
        }
    }
    return result;
}

std::set<audio_port_handle_t> PlaybackThread::getTrackPortIds()
{
    audio_utils::lock_guard _l(mutex());
    return getTrackPortIds_l();
}

String8 PlaybackThread::getParameters(const String8& keys)
{
    audio_utils::lock_guard _l(mutex());
    String8 out_s8;
    if (initCheck() == NO_ERROR && mOutput->stream->getParameters(keys, &out_s8) == OK) {
        return out_s8;
    }
    return {};
}

status_t DirectOutputThread::selectPresentation(int presentationId, int programId) {
    audio_utils::lock_guard _l(mutex());
    if (!isStreamInitialized()) {
        return NO_INIT;
    }
    return mOutput->stream->selectPresentation(presentationId, programId);
}

void PlaybackThread::ioConfigChanged_l(audio_io_config_event_t event, pid_t pid,
                                                   audio_port_handle_t portId) {
    ALOGV("PlaybackThread::ioConfigChanged, thread %p, event %d", this, event);
    sp<AudioIoDescriptor> desc;
    const struct audio_patch patch = isMsdDevice() ? mDownStreamPatch : mPatch;
    switch (event) {
    case AUDIO_OUTPUT_OPENED:
    case AUDIO_OUTPUT_REGISTERED:
    case AUDIO_OUTPUT_CONFIG_CHANGED:
        desc = sp<AudioIoDescriptor>::make(mId, patch, false /*isInput*/,
                mSampleRate, mFormat, mChannelMask,
                // FIXME AudioFlinger::frameCount(audio_io_handle_t) instead of mNormalFrameCount?
                mNormalFrameCount, mFrameCount, latency_l());
        break;
    case AUDIO_CLIENT_STARTED:
        desc = sp<AudioIoDescriptor>::make(mId, patch, portId);
        break;
    case AUDIO_OUTPUT_CLOSED:
    default:
        desc = sp<AudioIoDescriptor>::make(mId);
        break;
    }
    mAfThreadCallback->ioConfigChanged_l(event, desc, pid);
}

void PlaybackThread::onWriteReady()
{
    mCallbackThread->resetWriteBlocked();
}

void PlaybackThread::onDrainReady()
{
    mCallbackThread->resetDraining();
}

void PlaybackThread::onError(bool isHardError)
{
    mCallbackThread->setAsyncError(isHardError);
}

void PlaybackThread::onCodecFormatChanged(
        const std::vector<uint8_t>& metadataBs)
{
    const auto weakPointerThis = wp<PlaybackThread>::fromExisting(this);
    std::thread([this, metadataBs, weakPointerThis]() {
            const sp<PlaybackThread> playbackThread = weakPointerThis.promote();
            if (playbackThread == nullptr) {
                ALOGW("PlaybackThread was destroyed, skip codec format change event");
                return;
            }

            audio_utils::metadata::Data metadata =
                    audio_utils::metadata::dataFromByteString(metadataBs);
            if (metadata.empty()) {
                ALOGW("Can not transform the buffer to audio metadata, %s, %d",
                      reinterpret_cast<char*>(const_cast<uint8_t*>(metadataBs.data())),
                      (int)metadataBs.size());
                return;
            }

            audio_utils::metadata::ByteString metaDataStr =
                    audio_utils::metadata::byteStringFromData(metadata);
            std::vector metadataVec(metaDataStr.begin(), metaDataStr.end());
            audio_utils::lock_guard _l(audioTrackCbMutex());
            for (const auto& callbackPair : mAudioTrackCallbacks) {
                callbackPair.second->onCodecFormatChanged(metadataVec);
            }
    }).detach();
}

void PlaybackThread::resetWriteBlocked(uint32_t sequence)
{
    audio_utils::lock_guard _l(mutex());
    // reject out of sequence requests
    if ((mWriteAckSequence & 1) && (sequence == mWriteAckSequence)) {
        mWriteAckSequence &= ~1;
        mWaitWorkCV.notify_one();
    }
}

void PlaybackThread::resetDraining(uint32_t sequence)
{
    audio_utils::lock_guard _l(mutex());
    // reject out of sequence requests
    if ((mDrainSequence & 1) && (sequence == mDrainSequence)) {
        // Register discontinuity when HW drain is completed because that can cause
        // the timestamp frame position to reset to 0 for direct and offload threads.
        // (Out of sequence requests are ignored, since the discontinuity would be handled
        // elsewhere, e.g. in flush).
        mTimestampVerifier.discontinuity(mTimestampVerifier.DISCONTINUITY_MODE_ZERO);
        mDrainSequence &= ~1;
        mWaitWorkCV.notify_one();
    }
}

void PlaybackThread::readOutputParameters_l()
NO_THREAD_SAFETY_ANALYSIS
// 'moveEffectChain_ll' requires holding mutex 'AudioFlinger_Mutex' exclusively
{
    // unfortunately we have no way of recovering from errors here, hence the LOG_ALWAYS_FATAL
    const audio_config_base_t audioConfig = mOutput->getAudioProperties();
    mSampleRate = audioConfig.sample_rate;
    mChannelMask = audioConfig.channel_mask;
    if (!audio_is_output_channel(mChannelMask)) {
        LOG_ALWAYS_FATAL("HAL channel mask %#x not valid for output", mChannelMask);
    }
    if (hasMixer() && !isValidPcmSinkChannelMask(mChannelMask)) {
        LOG_ALWAYS_FATAL("HAL channel mask %#x not supported for mixed output",
                mChannelMask);
    }

    if (mMixerChannelMask == AUDIO_CHANNEL_NONE) {
        mMixerChannelMask = mChannelMask;
    }

    mChannelCount = audio_channel_count_from_out_mask(mChannelMask);
    mBalance.setChannelMask(mChannelMask);

    uint32_t mixerChannelCount = audio_channel_count_from_out_mask(mMixerChannelMask);

    // Get actual HAL format.
    status_t result = mOutput->stream->getAudioProperties(nullptr, nullptr, &mHALFormat);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error when retrieving output stream format: %d", result);
    // Get format from the shim, which will be different than the HAL format
    // if playing compressed audio over HDMI passthrough.
    mFormat = audioConfig.format;
    if (!audio_is_valid_format(mFormat)) {
        LOG_ALWAYS_FATAL("HAL format %#x not valid for output", mFormat);
    }
    if (hasMixer() && !isValidPcmSinkFormat(mFormat)) {
        LOG_FATAL("HAL format %#x not supported for mixed output",
                mFormat);
    }
    mFrameSize = mOutput->getFrameSize();
    result = mOutput->stream->getBufferSize(&mBufferSize);
    LOG_ALWAYS_FATAL_IF(result != OK,
            "Error when retrieving output stream buffer size: %d", result);
    mFrameCount = mBufferSize / mFrameSize;
    if (hasMixer() && (mFrameCount & 15)) {
        ALOGW("HAL output buffer size is %zu frames but AudioMixer requires multiples of 16 frames",
                mFrameCount);
    }

    mHwSupportsPause = false;
    if (mOutput->flags & AUDIO_OUTPUT_FLAG_DIRECT) {
        bool supportsPause = false, supportsResume = false;
        if (mOutput->stream->supportsPauseAndResume(&supportsPause, &supportsResume) == OK) {
            if (supportsPause && supportsResume) {
                mHwSupportsPause = true;
            } else if (supportsPause) {
                ALOGW("direct output implements pause but not resume");
            } else if (supportsResume) {
                ALOGW("direct output implements resume but not pause");
            }
        }
    }
    if (!mHwSupportsPause && mOutput->flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) {
        LOG_ALWAYS_FATAL("HW_AV_SYNC requested but HAL does not implement pause and resume");
    }

    if (mType == DUPLICATING && mMixerBufferEnabled && mEffectBufferEnabled) {
        // For best precision, we use float instead of the associated output
        // device format (typically PCM 16 bit).

        mFormat = AUDIO_FORMAT_PCM_FLOAT;
        mFrameSize = mChannelCount * audio_bytes_per_sample(mFormat);
        mBufferSize = mFrameSize * mFrameCount;

        // TODO: We currently use the associated output device channel mask and sample rate.
        // (1) Perhaps use the ORed channel mask of all downstream MixerThreads
        // (if a valid mask) to avoid premature downmix.
        // (2) Perhaps use the maximum sample rate of all downstream MixerThreads
        // instead of the output device sample rate to avoid loss of high frequency information.
        // This may need to be updated as MixerThread/OutputTracks are added and not here.
    }

    // Calculate size of normal sink buffer relative to the HAL output buffer size
    double multiplier = 1.0;
    // Note: mType == SPATIALIZER does not support FastMixer and DEEP is by definition not "fast"
    if ((mType == MIXER && !(mOutput->flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER)) &&
            (kUseFastMixer == FastMixer_Static || kUseFastMixer == FastMixer_Dynamic)) {
        size_t minNormalFrameCount = (kMinNormalSinkBufferSizeMs * mSampleRate) / 1000;
        size_t maxNormalFrameCount = (kMaxNormalSinkBufferSizeMs * mSampleRate) / 1000;

        // round up minimum and round down maximum to nearest 16 frames to satisfy AudioMixer
        minNormalFrameCount = (minNormalFrameCount + 15) & ~15;
        maxNormalFrameCount = maxNormalFrameCount & ~15;
        if (maxNormalFrameCount < minNormalFrameCount) {
            maxNormalFrameCount = minNormalFrameCount;
        }
        multiplier = (double) minNormalFrameCount / (double) mFrameCount;
        if (multiplier <= 1.0) {
            multiplier = 1.0;
        } else if (multiplier <= 2.0) {
            if (2 * mFrameCount <= maxNormalFrameCount) {
                multiplier = 2.0;
            } else {
                multiplier = (double) maxNormalFrameCount / (double) mFrameCount;
            }
        } else {
            multiplier = floor(multiplier);
        }
    }
    mNormalFrameCount = multiplier * mFrameCount;
    // round up to nearest 16 frames to satisfy AudioMixer
    if (hasMixer()) {
        mNormalFrameCount = (mNormalFrameCount + 15) & ~15;
    }
    ALOGI("HAL output buffer size %zu frames, normal sink buffer size %zu frames",
            (size_t)mFrameCount, mNormalFrameCount);

    // Check if we want to throttle the processing to no more than 2x normal rate
    mThreadThrottle = property_get_bool("af.thread.throttle", true /* default_value */);
    mThreadThrottleTimeMs = 0;
    mThreadThrottleEndMs = 0;
    mHalfBufferMs = mNormalFrameCount * 1000 / (2 * mSampleRate);

    // mSinkBuffer is the sink buffer.  Size is always multiple-of-16 frames.
    // Originally this was int16_t[] array, need to remove legacy implications.
    free(mSinkBuffer);
    mSinkBuffer = NULL;

    // For sink buffer size, we use the frame size from the downstream sink to avoid problems
    // with non PCM formats for compressed music, e.g. AAC, and Offload threads.
    const size_t sinkBufferSize = mNormalFrameCount * mFrameSize;
    (void)posix_memalign(&mSinkBuffer, 32, sinkBufferSize);

    // We resize the mMixerBuffer according to the requirements of the sink buffer which
    // drives the output.
    free(mMixerBuffer);
    mMixerBuffer = NULL;
    if (mMixerBufferEnabled) {
        mMixerBufferFormat = AUDIO_FORMAT_PCM_FLOAT; // no longer valid: AUDIO_FORMAT_PCM_16_BIT.
        mMixerBufferSize = mNormalFrameCount * mixerChannelCount
                * audio_bytes_per_sample(mMixerBufferFormat);
        (void)posix_memalign(&mMixerBuffer, 32, mMixerBufferSize);
    }
    free(mEffectBuffer);
    mEffectBuffer = NULL;
    if (mEffectBufferEnabled) {
        mEffectBufferFormat = AUDIO_FORMAT_PCM_FLOAT;
        mEffectBufferSize = mNormalFrameCount * mixerChannelCount
                * audio_bytes_per_sample(mEffectBufferFormat);
        (void)posix_memalign(&mEffectBuffer, 32, mEffectBufferSize);
    }

    if (mType == SPATIALIZER) {
        free(mPostSpatializerBuffer);
        mPostSpatializerBuffer = nullptr;
        mPostSpatializerBufferSize = mNormalFrameCount * mChannelCount
                * audio_bytes_per_sample(mEffectBufferFormat);
        (void)posix_memalign(&mPostSpatializerBuffer, 32, mPostSpatializerBufferSize);
    }

    mHapticChannelMask = static_cast<audio_channel_mask_t>(mChannelMask & AUDIO_CHANNEL_HAPTIC_ALL);
    mChannelMask = static_cast<audio_channel_mask_t>(mChannelMask & ~mHapticChannelMask);
    mHapticChannelCount = audio_channel_count_from_out_mask(mHapticChannelMask);
    mChannelCount -= mHapticChannelCount;
    mMixerChannelMask = static_cast<audio_channel_mask_t>(mMixerChannelMask & ~mHapticChannelMask);

    // force reconfiguration of effect chains and engines to take new buffer size and audio
    // parameters into account
    // Note that mutex() is not held when readOutputParameters_l() is called from the constructor
    // but in this case nothing is done below as no audio sessions have effect yet so it doesn't
    // matter.
    // create a copy of mEffectChains as calling moveEffectChain_ll()
    // can reorder some effect chains
    Vector<sp<IAfEffectChain>> effectChains = mEffectChains;
    for (size_t i = 0; i < effectChains.size(); i ++) {
        mAfThreadCallback->moveEffectChain_ll(effectChains[i]->sessionId(),
            this/* srcThread */, this/* dstThread */);
    }

    audio_output_flags_t flags = mOutput->flags;
    mediametrics::LogItem item(mThreadMetrics.getMetricsId()); // TODO: method in ThreadMetrics?
    item.set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_READPARAMETERS)
        .set(AMEDIAMETRICS_PROP_ENCODING, IAfThreadBase::formatToString(mFormat).c_str())
        .set(AMEDIAMETRICS_PROP_SAMPLERATE, (int32_t)mSampleRate)
        .set(AMEDIAMETRICS_PROP_CHANNELMASK, (int32_t)mChannelMask)
        .set(AMEDIAMETRICS_PROP_CHANNELCOUNT, (int32_t)mChannelCount)
        .set(AMEDIAMETRICS_PROP_FRAMECOUNT, (int32_t)mNormalFrameCount)
        .set(AMEDIAMETRICS_PROP_FLAGS, toString(flags).c_str())
        .set(AMEDIAMETRICS_PROP_PREFIX_HAPTIC AMEDIAMETRICS_PROP_CHANNELMASK,
                (int32_t)mHapticChannelMask)
        .set(AMEDIAMETRICS_PROP_PREFIX_HAPTIC AMEDIAMETRICS_PROP_CHANNELCOUNT,
                (int32_t)mHapticChannelCount)
        .set(AMEDIAMETRICS_PROP_PREFIX_HAL    AMEDIAMETRICS_PROP_ENCODING,
                IAfThreadBase::formatToString(mHALFormat).c_str())
        .set(AMEDIAMETRICS_PROP_PREFIX_HAL    AMEDIAMETRICS_PROP_FRAMECOUNT,
                (int32_t)mFrameCount) // sic - added HAL
        ;
    uint32_t latencyMs;
    if (mOutput->stream->getLatency(&latencyMs) == NO_ERROR) {
        item.set(AMEDIAMETRICS_PROP_PREFIX_HAL AMEDIAMETRICS_PROP_LATENCYMS, (double)latencyMs);
    }
    item.record();
}

ThreadBase::MetadataUpdate PlaybackThread::updateMetadata_l()
{
    if (!isStreamInitialized() || !mActiveTracks.readAndClearHasChanged()) {
        return {}; // nothing to do
    }
    StreamOutHalInterface::SourceMetadata metadata;
    std::map<audio_session_t, std::vector<playback_track_metadata_v7_t> >allSessionsMetadata;
    for (const sp<IAfTrack>& track : mActiveTracks) {
        std::vector<playback_track_metadata_v7_t>& sessionMetadata =
                allSessionsMetadata[track->sessionId()];
        auto backInserter = std::back_inserter(sessionMetadata);
        // No track is invalid as this is called after prepareTrack_l in the same
        // critical section
        track->copyMetadataTo(backInserter);
    }
    std::vector<playback_track_metadata_v7_t> spatializedTracksMetaData;
    for (const auto& [session, sessionTrackMetadata] : allSessionsMetadata) {
        metadata.tracks.insert(metadata.tracks.end(),
                sessionTrackMetadata.begin(), sessionTrackMetadata.end());
        if (auto chain = getEffectChain_l(session) ; chain != nullptr) {
            chain->sendMetadata_l(sessionTrackMetadata, {});
        }
        if ((hasAudioSession_l(session) & IAfThreadBase::SPATIALIZED_SESSION) != 0) {
            spatializedTracksMetaData.insert(spatializedTracksMetaData.end(),
                    sessionTrackMetadata.begin(), sessionTrackMetadata.end());
        }
    }
    if (auto chain = getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX); chain != nullptr) {
        chain->sendMetadata_l(metadata.tracks, {});
    }
    if (auto chain = getEffectChain_l(AUDIO_SESSION_OUTPUT_STAGE); chain != nullptr) {
        chain->sendMetadata_l(metadata.tracks, spatializedTracksMetaData);
    }
    if (auto chain = getEffectChain_l(AUDIO_SESSION_DEVICE); chain != nullptr) {
        chain->sendMetadata_l(metadata.tracks, {});
    }

    sendMetadataToBackend_l(metadata);
    MetadataUpdate change;
    change.playbackMetadataUpdate = metadata.tracks;
    return change;
}

void PlaybackThread::sendMetadataToBackend_l(
        const StreamOutHalInterface::SourceMetadata& metadata)
{
    mOutput->stream->updateSourceMetadata(metadata);
};

status_t PlaybackThread::getRenderPosition(
        uint32_t* halFrames, uint32_t* dspFrames) const
{
    if (halFrames == NULL || dspFrames == NULL) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    if (initCheck() != NO_ERROR) {
        return INVALID_OPERATION;
    }
    int64_t framesWritten = mBytesWritten / mFrameSize;
    *halFrames = framesWritten;

    if (isSuspended()) {
        // return an estimation of rendered frames when the output is suspended
        size_t latencyFrames = (latency_l() * mSampleRate) / 1000;
        *dspFrames = (uint32_t)
                (framesWritten >= (int64_t)latencyFrames ? framesWritten - latencyFrames : 0);
        return NO_ERROR;
    } else {
        status_t status;
        uint64_t frames = 0;
        status = mOutput->getRenderPosition(&frames);
        *dspFrames = (uint32_t)frames;
        return status;
    }
}

product_strategy_t PlaybackThread::getStrategyForSession_l(audio_session_t sessionId) const
{
    // session AUDIO_SESSION_OUTPUT_MIX is placed in same strategy as MUSIC stream so that
    // it is moved to correct output by audio policy manager when A2DP is connected or disconnected
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
        return getStrategyForStream(AUDIO_STREAM_MUSIC);
    }
    for (size_t i = 0; i < mTracks.size(); i++) {
        sp<IAfTrack> track = mTracks[i];
        if (sessionId == track->sessionId() && !track->isInvalid()) {
            return getStrategyForStream(track->streamType());
        }
    }
    return getStrategyForStream(AUDIO_STREAM_MUSIC);
}


AudioStreamOut* PlaybackThread::getOutput() const
{
    audio_utils::lock_guard _l(mutex());
    return mOutput;
}

AudioStreamOut* PlaybackThread::clearOutput()
{
    audio_utils::lock_guard _l(mutex());
    AudioStreamOut *output = mOutput;
    mOutput = NULL;
    // FIXME FastMixer might also have a raw ptr to mOutputSink;
    //       must push a NULL and wait for ack
    mOutputSink.clear();
    mPipeSink.clear();
    mNormalSink.clear();
    return output;
}

// this method must always be called either with ThreadBase mutex() held or inside the thread loop
sp<StreamHalInterface> PlaybackThread::stream() const
{
    if (mOutput == NULL) {
        return NULL;
    }
    return mOutput->stream;
}

uint32_t PlaybackThread::activeSleepTimeUs() const
{
    return (uint32_t)((uint32_t)((mNormalFrameCount * 1000) / mSampleRate) * 1000);
}

status_t PlaybackThread::setSyncEvent(const sp<SyncEvent>& event)
{
    if (!isValidSyncEvent(event)) {
        return BAD_VALUE;
    }

    audio_utils::lock_guard _l(mutex());

    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<IAfTrack> track = mTracks[i];
        if (event->triggerSession() == track->sessionId()) {
            (void) track->setSyncEvent(event);
            return NO_ERROR;
        }
    }

    return NAME_NOT_FOUND;
}

bool PlaybackThread::isValidSyncEvent(const sp<SyncEvent>& event) const
{
    return event->type() == AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE;
}

void PlaybackThread::threadLoop_removeTracks(
        [[maybe_unused]] const Vector<sp<IAfTrack>>& tracksToRemove)
{
    // Miscellaneous track cleanup when removed from the active list,
    // called without Thread lock but synchronized with threadLoop processing.
#ifdef ADD_BATTERY_DATA
    for (const auto& track : tracksToRemove) {
        if (track->isExternalTrack()) {
            // to track the speaker usage
            addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStop);
        }
    }
#endif
}

void PlaybackThread::checkSilentMode_l()
{
    if (property_get_bool("ro.audio.silent", false)) {
        ALOGW("ro.audio.silent is now ignored");
    }
}

// shared by MIXER and DIRECT, overridden by DUPLICATING
ssize_t PlaybackThread::threadLoop_write()
{
    LOG_HIST_TS();
    mInWrite = true;
    ssize_t bytesWritten;
    const size_t offset = mCurrentWriteLength - mBytesRemaining;

    // If an NBAIO sink is present, use it to write the normal mixer's submix
    if (mNormalSink != 0) {

        const size_t count = mBytesRemaining / mFrameSize;

        ATRACE_BEGIN("write");
        // update the setpoint when AudioFlinger::mScreenState changes
        const uint32_t screenState = mAfThreadCallback->getScreenState();
        if (screenState != mScreenState) {
            mScreenState = screenState;
            MonoPipe *pipe = (MonoPipe *)mPipeSink.get();
            if (pipe != NULL) {
                pipe->setAvgFrames((mScreenState & 1) ?
                        (pipe->maxFrames() * 7) / 8 : mNormalFrameCount * 2);
            }
        }
        ssize_t framesWritten = mNormalSink->write((char *)mSinkBuffer + offset, count);
        ATRACE_END();

        if (framesWritten > 0) {
            bytesWritten = framesWritten * mFrameSize;

#ifdef TEE_SINK
            mTee.write((char *)mSinkBuffer + offset, framesWritten);
#endif
        } else {
            bytesWritten = framesWritten;
        }
    // otherwise use the HAL / AudioStreamOut directly
    } else {
        // Direct output and offload threads

        if (mUseAsyncWrite) {
            ALOGW_IF(mWriteAckSequence & 1, "threadLoop_write(): out of sequence write request");
            mWriteAckSequence += 2;
            mWriteAckSequence |= 1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setWriteBlocked(mWriteAckSequence);
        }
        ATRACE_BEGIN("write");
        // FIXME We should have an implementation of timestamps for direct output threads.
        // They are used e.g for multichannel PCM playback over HDMI.
        bytesWritten = mOutput->write((char *)mSinkBuffer + offset, mBytesRemaining);
        ATRACE_END();

        if (mUseAsyncWrite &&
                ((bytesWritten < 0) || (bytesWritten == (ssize_t)mBytesRemaining))) {
            // do not wait for async callback in case of error of full write
            mWriteAckSequence &= ~1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setWriteBlocked(mWriteAckSequence);
        }
    }

    mNumWrites++;
    mInWrite = false;
    if (mStandby) {
        mThreadMetrics.logBeginInterval();
        mThreadSnapshot.onBegin();
        mStandby = false;
    }
    return bytesWritten;
}

// startMelComputation_l() must be called with AudioFlinger::mutex() held
void PlaybackThread::startMelComputation_l(
        const sp<audio_utils::MelProcessor>& processor)
{
    auto outputSink = static_cast<AudioStreamOutSink*>(mOutputSink.get());
    if (outputSink != nullptr) {
        outputSink->startMelComputation(processor);
    }
}

// stopMelComputation_l() must be called with AudioFlinger::mutex() held
void PlaybackThread::stopMelComputation_l()
{
    auto outputSink = static_cast<AudioStreamOutSink*>(mOutputSink.get());
    if (outputSink != nullptr) {
        outputSink->stopMelComputation();
    }
}

void PlaybackThread::threadLoop_drain()
{
    bool supportsDrain = false;
    if (mOutput->stream->supportsDrain(&supportsDrain) == OK && supportsDrain) {
        ALOGV("draining %s", (mMixerStatus == MIXER_DRAIN_TRACK) ? "early" : "full");
        if (mUseAsyncWrite) {
            ALOGW_IF(mDrainSequence & 1, "threadLoop_drain(): out of sequence drain request");
            mDrainSequence |= 1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setDraining(mDrainSequence);
        }
        status_t result = mOutput->stream->drain(mMixerStatus == MIXER_DRAIN_TRACK);
        ALOGE_IF(result != OK, "Error when draining stream: %d", result);
    }
}

void PlaybackThread::threadLoop_exit()
{
    {
        audio_utils::lock_guard _l(mutex());
        for (size_t i = 0; i < mTracks.size(); i++) {
            sp<IAfTrack> track = mTracks[i];
            track->invalidate();
        }
        // Clear ActiveTracks to update BatteryNotifier in case active tracks remain.
        // After we exit there are no more track changes sent to BatteryNotifier
        // because that requires an active threadLoop.
        // TODO: should we decActiveTrackCnt() of the cleared track effect chain?
        mActiveTracks.clear();
    }
}

/*
The derived values that are cached:
 - mSinkBufferSize from frame count * frame size
 - mActiveSleepTimeUs from activeSleepTimeUs()
 - mIdleSleepTimeUs from idleSleepTimeUs()
 - mStandbyDelayNs from mActiveSleepTimeUs (DIRECT only) or forced to at least
   kDefaultStandbyTimeInNsecs when connected to an A2DP device.
 - maxPeriod from frame count and sample rate (MIXER only)

The parameters that affect these derived values are:
 - frame count
 - frame size
 - sample rate
 - device type: A2DP or not
 - device latency
 - format: PCM or not
 - active sleep time
 - idle sleep time
*/

void PlaybackThread::cacheParameters_l()
{
    mSinkBufferSize = mNormalFrameCount * mFrameSize;
    mActiveSleepTimeUs = activeSleepTimeUs();
    mIdleSleepTimeUs = idleSleepTimeUs();

    mStandbyDelayNs = getStandbyTimeInNanos();

    // make sure standby delay is not too short when connected to an A2DP sink to avoid
    // truncating audio when going to standby.
    if (!Intersection(outDeviceTypes_l(),  getAudioDeviceOutAllA2dpSet()).empty()) {
        if (mStandbyDelayNs < kDefaultStandbyTimeInNsecs) {
            mStandbyDelayNs = kDefaultStandbyTimeInNsecs;
        }
    }
}

bool PlaybackThread::invalidateTracks_l(audio_stream_type_t streamType)
{
    ALOGV("MixerThread::invalidateTracks() mixer %p, streamType %d, mTracks.size %zu",
            this,  streamType, mTracks.size());
    bool trackMatch = false;
    size_t size = mTracks.size();
    for (size_t i = 0; i < size; i++) {
        sp<IAfTrack> t = mTracks[i];
        if (t->streamType() == streamType && t->isExternalTrack()) {
            t->invalidate();
            trackMatch = true;
        }
    }
    return trackMatch;
}

void PlaybackThread::invalidateTracks(audio_stream_type_t streamType)
{
    audio_utils::lock_guard _l(mutex());
    invalidateTracks_l(streamType);
}

void PlaybackThread::invalidateTracks(std::set<audio_port_handle_t>& portIds) {
    audio_utils::lock_guard _l(mutex());
    invalidateTracks_l(portIds);
}

bool PlaybackThread::invalidateTracks_l(std::set<audio_port_handle_t>& portIds) {
    bool trackMatch = false;
    const size_t size = mTracks.size();
    for (size_t i = 0; i < size; i++) {
        sp<IAfTrack> t = mTracks[i];
        if (t->isExternalTrack() && portIds.find(t->portId()) != portIds.end()) {
            t->invalidate();
            portIds.erase(t->portId());
            trackMatch = true;
        }
        if (portIds.empty()) {
            break;
        }
    }
    return trackMatch;
}

// getTrackById_l must be called with holding thread lock
IAfTrack* PlaybackThread::getTrackById_l(
        audio_port_handle_t trackPortId) {
    for (size_t i = 0; i < mTracks.size(); i++) {
        if (mTracks[i]->portId() == trackPortId) {
            return mTracks[i].get();
        }
    }
    return nullptr;
}

// getTracks_l must be called with holding thread lock
std::vector<sp<IAfTrack>> PlaybackThread::getTracks_l() {
    return std::vector(mTracks.begin(), mTracks.end());
}

status_t PlaybackThread::addEffectChain_l(const sp<IAfEffectChain>& chain)
{
    audio_session_t session = chain->sessionId();
    sp<EffectBufferHalInterface> halInBuffer, halOutBuffer;
    float *buffer = nullptr; // only used for non global sessions

    if (mType == SPATIALIZER) {
        if (!audio_is_global_session(session)) {
            // player sessions on a spatializer output will use a dedicated input buffer and
            // will either output multi channel to mEffectBuffer if the track is spatilaized
            // or stereo to mPostSpatializerBuffer if not spatialized.
            uint32_t channelMask;
            bool isSessionSpatialized =
                (hasAudioSession_l(session) & ThreadBase::SPATIALIZED_SESSION) != 0;
            if (isSessionSpatialized) {
                channelMask = mMixerChannelMask;
            } else {
                channelMask = mChannelMask;
            }
            size_t numSamples = mNormalFrameCount
                    * (audio_channel_count_from_out_mask(channelMask) + mHapticChannelCount);
            status_t result = mAfThreadCallback->getEffectsFactoryHal()->allocateBuffer(
                    numSamples * sizeof(float),
                    &halInBuffer);
            if (result != OK) return result;

            result = mAfThreadCallback->getEffectsFactoryHal()->mirrorBuffer(
                    isSessionSpatialized ? mEffectBuffer : mPostSpatializerBuffer,
                    isSessionSpatialized ? mEffectBufferSize : mPostSpatializerBufferSize,
                    &halOutBuffer);
            if (result != OK) return result;

            buffer = halInBuffer ? halInBuffer->audioBuffer()->f32 : buffer;

            ALOGV("addEffectChain_l() creating new input buffer %p session %d",
                    buffer, session);
        } else {
            status_t result = INVALID_OPERATION;
            // Buffer configuration for global sessions on a SPATIALIZER thread:
            // - AUDIO_SESSION_OUTPUT_MIX session uses the mEffectBuffer as input and output buffer
            // - AUDIO_SESSION_OUTPUT_STAGE session uses the mEffectBuffer as input buffer and
            //   mPostSpatializerBuffer as output buffer
            // - AUDIO_SESSION_DEVICE session uses the mPostSpatializerBuffer as input and output
            //   buffer
            if (session == AUDIO_SESSION_OUTPUT_MIX || session == AUDIO_SESSION_OUTPUT_STAGE) {
                result = mAfThreadCallback->getEffectsFactoryHal()->mirrorBuffer(
                        mEffectBuffer, mEffectBufferSize, &halInBuffer);
                if (result != OK) return result;

                if (session == AUDIO_SESSION_OUTPUT_MIX) {
                    halOutBuffer = halInBuffer;
                }
            }

            if (session == AUDIO_SESSION_OUTPUT_STAGE || session == AUDIO_SESSION_DEVICE) {
                result = mAfThreadCallback->getEffectsFactoryHal()->mirrorBuffer(
                        mPostSpatializerBuffer, mPostSpatializerBufferSize, &halOutBuffer);
                if (result != OK) return result;

                if (session == AUDIO_SESSION_DEVICE) {
                    halInBuffer = halOutBuffer;
                }
            }
        }
    } else {
        status_t result = mAfThreadCallback->getEffectsFactoryHal()->mirrorBuffer(
                mEffectBufferEnabled ? mEffectBuffer : mSinkBuffer,
                mEffectBufferEnabled ? mEffectBufferSize : mSinkBufferSize,
                &halInBuffer);
        if (result != OK) return result;
        halOutBuffer = halInBuffer;
        ALOGV("addEffectChain_l() %p on thread %p for session %d", chain.get(), this, session);
        if (!audio_is_global_session(session)) {
            buffer = halInBuffer ? reinterpret_cast<float*>(halInBuffer->externalData())
                                 : buffer;
            // Only one effect chain can be present in direct output thread and it uses
            // the sink buffer as input
            if (mType != DIRECT) {
                size_t numSamples = mNormalFrameCount
                        * (audio_channel_count_from_out_mask(mMixerChannelMask)
                                                             + mHapticChannelCount);
                const status_t allocateStatus =
                        mAfThreadCallback->getEffectsFactoryHal()->allocateBuffer(
                        numSamples * sizeof(float),
                        &halInBuffer);
                if (allocateStatus != OK) return allocateStatus;

                buffer = halInBuffer ? halInBuffer->audioBuffer()->f32 : buffer;
                ALOGV("addEffectChain_l() creating new input buffer %p session %d",
                        buffer, session);
            }
        }
    }

    if (!audio_is_global_session(session)) {
        // Attach all tracks with same session ID to this chain.
        for (size_t i = 0; i < mTracks.size(); ++i) {
            sp<IAfTrack> track = mTracks[i];
            if (session == track->sessionId()) {
                ALOGV("addEffectChain_l() track->setMainBuffer track %p buffer %p",
                        track.get(), buffer);
                track->setMainBuffer(buffer);
                chain->incTrackCnt();
            }
        }

        // indicate all active tracks in the chain
        for (const sp<IAfTrack>& track : mActiveTracks) {
            if (session == track->sessionId()) {
                ALOGV("addEffectChain_l() activating track %p on session %d",
                        track.get(), session);
                chain->incActiveTrackCnt();
            }
        }
    }

    chain->setThread(this);
    chain->setInBuffer(halInBuffer);
    chain->setOutBuffer(halOutBuffer);
    // Effect chain for session AUDIO_SESSION_DEVICE is inserted at end of effect
    // chains list in order to be processed last as it contains output device effects.
    // Effect chain for session AUDIO_SESSION_OUTPUT_STAGE is inserted just before to apply post
    // processing effects specific to an output stream before effects applied to all streams
    // routed to a given device.
    // Effect chain for session AUDIO_SESSION_OUTPUT_MIX is inserted before
    // session AUDIO_SESSION_OUTPUT_STAGE to be processed
    // after track specific effects and before output stage.
    // It is therefore mandatory that AUDIO_SESSION_OUTPUT_MIX == 0 and
    // that AUDIO_SESSION_OUTPUT_STAGE < AUDIO_SESSION_OUTPUT_MIX.
    // Effect chain for other sessions are inserted at beginning of effect
    // chains list to be processed before output mix effects. Relative order between other
    // sessions is not important.
    static_assert(AUDIO_SESSION_OUTPUT_MIX == 0 &&
            AUDIO_SESSION_OUTPUT_STAGE < AUDIO_SESSION_OUTPUT_MIX &&
            AUDIO_SESSION_DEVICE < AUDIO_SESSION_OUTPUT_STAGE,
            "audio_session_t constants misdefined");
    size_t size = mEffectChains.size();
    size_t i = 0;
    for (i = 0; i < size; i++) {
        if (mEffectChains[i]->sessionId() < session) {
            break;
        }
    }
    mEffectChains.insertAt(chain, i);
    checkSuspendOnAddEffectChain_l(chain);

    return NO_ERROR;
}

size_t PlaybackThread::removeEffectChain_l(const sp<IAfEffectChain>& chain)
{
    audio_session_t session = chain->sessionId();

    ALOGV("removeEffectChain_l() %p from thread %p for session %d", chain.get(), this, session);

    for (size_t i = 0; i < mEffectChains.size(); i++) {
        if (chain == mEffectChains[i]) {
            mEffectChains.removeAt(i);
            // detach all active tracks from the chain
            for (const sp<IAfTrack>& track : mActiveTracks) {
                if (session == track->sessionId()) {
                    ALOGV("removeEffectChain_l(): stopping track on chain %p for session Id: %d",
                            chain.get(), session);
                    chain->decActiveTrackCnt();
                }
            }

            // detach all tracks with same session ID from this chain
            for (size_t j = 0; j < mTracks.size(); ++j) {
                sp<IAfTrack> track = mTracks[j];
                if (session == track->sessionId()) {
                    track->setMainBuffer(reinterpret_cast<float*>(mSinkBuffer));
                    chain->decTrackCnt();
                }
            }
            break;
        }
    }
    return mEffectChains.size();
}

status_t PlaybackThread::attachAuxEffect(
        const sp<IAfTrack>& track, int EffectId)
{
    audio_utils::lock_guard _l(mutex());
    return attachAuxEffect_l(track, EffectId);
}

status_t PlaybackThread::attachAuxEffect_l(
        const sp<IAfTrack>& track, int EffectId)
{
    status_t status = NO_ERROR;

    if (EffectId == 0) {
        track->setAuxBuffer(0, NULL);
    } else {
        // Auxiliary effects are always in audio session AUDIO_SESSION_OUTPUT_MIX
        sp<IAfEffectModule> effect = getEffect_l(AUDIO_SESSION_OUTPUT_MIX, EffectId);
        if (effect != 0) {
            if ((effect->desc().flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
                track->setAuxBuffer(EffectId, (int32_t *)effect->inBuffer());
            } else {
                status = INVALID_OPERATION;
            }
        } else {
            status = BAD_VALUE;
        }
    }
    return status;
}

void PlaybackThread::detachAuxEffect_l(int effectId)
{
    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<IAfTrack> track = mTracks[i];
        if (track->auxEffectId() == effectId) {
            attachAuxEffect_l(track, 0);
        }
    }
}

bool PlaybackThread::threadLoop()
NO_THREAD_SAFETY_ANALYSIS  // manual locking of AudioFlinger
{
    if (mType == SPATIALIZER) {
        const pid_t tid = getTid();
        if (tid == -1) {  // odd: we are here, we must be a running thread.
            ALOGW("%s: Cannot update Spatializer mixer thread priority, no tid", __func__);
        } else {
            const int priorityBoost = requestSpatializerPriority(getpid(), tid);
            if (priorityBoost > 0) {
                stream()->setHalThreadPriority(priorityBoost);
            }
        }
    } else if (property_get_bool("ro.boot.container", false /* default_value */)) {
        // In ARC experiments (b/73091832), the latency under using CFS scheduler with any priority
        // is not enough for PlaybackThread to process audio data in time. We request the lowest
        // real-time priority, SCHED_FIFO=1, for PlaybackThread in ARC. ro.boot.container is true
        // only on ARC.
        const pid_t tid = getTid();
        if (tid == -1) {
            ALOGW("%s: Cannot update PlaybackThread priority for ARC, no tid", __func__);
        } else {
            const status_t status = requestPriority(getpid(),
                                                    tid,
                                                    kPriorityPlaybackThreadArc,
                                                    false /* isForApp */,
                                                    true /* asynchronous */);
            if (status != OK) {
                ALOGW("%s: Cannot update PlaybackThread priority for ARC, status %d", __func__,
                        status);
            } else {
                stream()->setHalThreadPriority(kPriorityPlaybackThreadArc);
            }
        }
    }

    Vector<sp<IAfTrack>> tracksToRemove;

    mStandbyTimeNs = systemTime();
    int64_t lastLoopCountWritten = -2; // never matches "previous" loop, when loopCount = 0.

    // MIXER
    nsecs_t lastWarning = 0;

    // DUPLICATING
    // FIXME could this be made local to while loop?
    writeFrames = 0;

    {
        audio_utils::lock_guard l(mutex());

        cacheParameters_l();
        checkSilentMode_l();
    }

    mSleepTimeUs = mIdleSleepTimeUs;

    if (mType == MIXER || mType == SPATIALIZER) {
        sleepTimeShift = 0;
    }

    CpuStats cpuStats;
    const String8 myName(String8::format("thread %p type %d TID %d", this, mType, gettid()));

    acquireWakeLock();

    // Estimated time for next buffer to be written to hal. This is used only on
    // suspended mode (for now) to help schedule the wait time until next iteration.
    nsecs_t timeLoopNextNs = 0;

    audio_patch_handle_t lastDownstreamPatchHandle = AUDIO_PATCH_HANDLE_NONE;

    sendCheckOutputStageEffectsEvent();

    // loopCount is used for statistics and diagnostics.
    for (int64_t loopCount = 0; !exitPending(); ++loopCount)
    {
        cpuStats.sample(myName);

        Vector<sp<IAfEffectChain>> effectChains;
        audio_session_t activeHapticSessionId = AUDIO_SESSION_NONE;
        bool isHapticSessionSpatialized = false;
        std::vector<sp<IAfTrack>> activeTracks;

        // If the device is AUDIO_DEVICE_OUT_BUS, check for downstream latency.
        //
        // Note: we access outDeviceTypes() outside of mutex().
        if (isMsdDevice() && outDeviceTypes_l().count(AUDIO_DEVICE_OUT_BUS) != 0) {
            // Here, we try for the AF lock, but do not block on it as the latency
            // is more informational.
            if (mAfThreadCallback->mutex().try_lock()) {
                std::vector<SoftwarePatch> swPatches;
                double latencyMs = 0.; // not required; initialized for clang-tidy
                status_t status = INVALID_OPERATION;
                audio_patch_handle_t downstreamPatchHandle = AUDIO_PATCH_HANDLE_NONE;
                if (mAfThreadCallback->getPatchPanel()->getDownstreamSoftwarePatches(
                                id(), &swPatches) == OK
                        && swPatches.size() > 0) {
                        status = swPatches[0].getLatencyMs_l(&latencyMs);
                        downstreamPatchHandle = swPatches[0].getPatchHandle();
                }
                if (downstreamPatchHandle != lastDownstreamPatchHandle) {
                    mDownstreamLatencyStatMs.reset();
                    lastDownstreamPatchHandle = downstreamPatchHandle;
                }
                if (status == OK) {
                    // verify downstream latency (we assume a max reasonable
                    // latency of 5 seconds).
                    const double minLatency = 0., maxLatency = 5000.;
                    if (latencyMs >= minLatency && latencyMs <= maxLatency) {
                        ALOGVV("new downstream latency %lf ms", latencyMs);
                    } else {
                        ALOGD("out of range downstream latency %lf ms", latencyMs);
                        latencyMs = std::clamp(latencyMs, minLatency, maxLatency);
                    }
                    mDownstreamLatencyStatMs.add(latencyMs);
                }
                mAfThreadCallback->mutex().unlock();
            }
        } else {
            if (lastDownstreamPatchHandle != AUDIO_PATCH_HANDLE_NONE) {
                // our device is no longer AUDIO_DEVICE_OUT_BUS, reset patch handle and stats.
                mDownstreamLatencyStatMs.reset();
                lastDownstreamPatchHandle = AUDIO_PATCH_HANDLE_NONE;
            }
        }

        if (mCheckOutputStageEffects.exchange(false)) {
            checkOutputStageEffects();
        }

        MetadataUpdate metadataUpdate;
        { // scope for mutex()

            audio_utils::unique_lock _l(mutex());

            processConfigEvents_l();
            if (mCheckOutputStageEffects.load()) {
                continue;
            }

            collectTimestamps_l();

            saveOutputTracks();
            if (mSignalPending) {
                // A signal was raised while we were unlocked
                mSignalPending = false;
            } else if (waitingAsyncCallback_l()) {
                if (exitPending()) {
                    break;
                }
                bool released = false;
                if (!keepWakeLock()) {
                    releaseWakeLock_l();
                    released = true;
                }

                const int64_t waitNs = computeWaitTimeNs_l();
                ALOGV("wait async completion (wait time: %lld)", (long long)waitNs);
                std::cv_status cvstatus =
                        mWaitWorkCV.wait_for(_l, std::chrono::nanoseconds(waitNs));
                if (cvstatus == std::cv_status::timeout) {
                    mSignalPending = true; // if timeout recheck everything
                }
                ALOGV("async completion/wake");
                if (released) {
                    acquireWakeLock_l();
                }
                mStandbyTimeNs = systemTime() + mStandbyDelayNs;
                mSleepTimeUs = 0;

                continue;
            }
            if ((mActiveTracks.isEmpty() && systemTime() > mStandbyTimeNs) ||
                                   isSuspended()) {
                // put audio hardware into standby after short delay
                if (shouldStandby_l()) {

                    threadLoop_standby();

                    // This is where we go into standby
                    if (!mStandby) {
                        LOG_AUDIO_STATE();
                        mThreadMetrics.logEndInterval();
                        mThreadSnapshot.onEnd();
                        setStandby_l();
                    }
                    sendStatistics(false /* force */);
                }

                if (mActiveTracks.isEmpty() && mConfigEvents.isEmpty()) {
                    // we're about to wait, flush the binder command buffer
                    IPCThreadState::self()->flushCommands();

                    clearOutputTracks();

                    if (exitPending()) {
                        break;
                    }

                    releaseWakeLock_l();
                    // wait until we have something to do...
                    ALOGV("%s going to sleep", myName.c_str());
                    mWaitWorkCV.wait(_l);
                    ALOGV("%s waking up", myName.c_str());
                    acquireWakeLock_l();

                    mMixerStatus = MIXER_IDLE;
                    mMixerStatusIgnoringFastTracks = MIXER_IDLE;
                    mBytesWritten = 0;
                    mBytesRemaining = 0;
                    checkSilentMode_l();

                    mStandbyTimeNs = systemTime() + mStandbyDelayNs;
                    mSleepTimeUs = mIdleSleepTimeUs;
                    if (mType == MIXER || mType == SPATIALIZER) {
                        sleepTimeShift = 0;
                    }

                    continue;
                }
            }
            // mMixerStatusIgnoringFastTracks is also updated internally
            mMixerStatus = prepareTracks_l(&tracksToRemove);

            mActiveTracks.updatePowerState_l(this);

            metadataUpdate = updateMetadata_l();

            // Acquire a local copy of active tracks with lock (release w/o lock).
            //
            // Control methods on the track acquire the ThreadBase lock (e.g. start()
            // stop(), pause(), etc.), but the threadLoop is entitled to call audio
            // data / buffer methods on tracks from activeTracks without the ThreadBase lock.
            activeTracks.insert(activeTracks.end(), mActiveTracks.begin(), mActiveTracks.end());

            setHalLatencyMode_l();

            // updateTeePatches_l will acquire the ThreadBase_Mutex of other threads,
            // so this is done before we lock our effect chains.
            for (const auto& track : mActiveTracks) {
                track->updateTeePatches_l();
            }

            // check if traces have been enabled.
            bool atraceEnabled = ATRACE_ENABLED();
            if (atraceEnabled != mAtraceEnabled) [[unlikely]] {
                mAtraceEnabled = atraceEnabled;
                if (atraceEnabled) {
                    const auto devices = patchSinksToString(&mPatch);
                    for (const auto& track : activeTracks) {
                        track->logRefreshInterval(devices);
                    }
                }
            }
            // signal actual start of output stream when the render position reported by
            // the kernel starts moving.
            if (!mHalStarted && ((isSuspended() && (mBytesWritten != 0)) || (!mStandby
                    && (mKernelPositionOnStandby
                            != mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL])))) {
                mHalStarted = true;
                mWaitHalStartCV.notify_all();
            }

            // prevent any changes in effect chain list and in each effect chain
            // during mixing and effect process as the audio buffers could be deleted
            // or modified if an effect is created or deleted
            lockEffectChains_l(effectChains);

            // Determine which session to pick up haptic data.
            // This must be done under the same lock as prepareTracks_l().
            // The haptic data from the effect is at a higher priority than the one from track.
            // TODO: Write haptic data directly to sink buffer when mixing.
            if (mHapticChannelCount > 0) {
                for (const auto& track : mActiveTracks) {
                    sp<IAfEffectChain> effectChain = getEffectChain_l(track->sessionId());
                    if (effectChain != nullptr
                            && effectChain->containsHapticGeneratingEffect_l()) {
                        activeHapticSessionId = track->sessionId();
                        isHapticSessionSpatialized =
                                mType == SPATIALIZER && track->isSpatialized();
                        break;
                    }
                    if (activeHapticSessionId == AUDIO_SESSION_NONE
                            && track->getHapticPlaybackEnabled()) {
                        activeHapticSessionId = track->sessionId();
                        isHapticSessionSpatialized =
                                mType == SPATIALIZER && track->isSpatialized();
                    }
                }
            }
        } // mutex() scope ends

        if (mBytesRemaining == 0) {
            mCurrentWriteLength = 0;
            if (mMixerStatus == MIXER_TRACKS_READY) {
                // threadLoop_mix() sets mCurrentWriteLength
                threadLoop_mix();
            } else if ((mMixerStatus != MIXER_DRAIN_TRACK)
                        && (mMixerStatus != MIXER_DRAIN_ALL)) {
                // threadLoop_sleepTime sets mSleepTimeUs to 0 if data
                // must be written to HAL
                threadLoop_sleepTime();
                if (mSleepTimeUs == 0) {
                    mCurrentWriteLength = mSinkBufferSize;

                    // Tally underrun frames as we are inserting 0s here.
                    for (const auto& track : activeTracks) {
                        if (track->fillingStatus() == IAfTrack::FS_ACTIVE
                                && !track->isStopped()
                                && !track->isPaused()
                                && !track->isTerminated()) {
                            ALOGV("%s: track(%d) %s underrun due to thread sleep of %zu frames",
                                    __func__, track->id(), track->getTrackStateAsString(),
                                    mNormalFrameCount);
                            track->audioTrackServerProxy()->tallyUnderrunFrames(
                                    mNormalFrameCount);
                        }
                    }
                }
            }
            // Either threadLoop_mix() or threadLoop_sleepTime() should have set
            // mMixerBuffer with data if mMixerBufferValid is true and mSleepTimeUs == 0.
            // Merge mMixerBuffer data into mEffectBuffer (if any effects are valid)
            // or mSinkBuffer (if there are no effects and there is no data already copied to
            // mSinkBuffer).
            //
            // This is done pre-effects computation; if effects change to
            // support higher precision, this needs to move.
            //
            // mMixerBufferValid is only set true by MixerThread::prepareTracks_l().
            // TODO use mSleepTimeUs == 0 as an additional condition.
            uint32_t mixerChannelCount = mEffectBufferValid ?
                        audio_channel_count_from_out_mask(mMixerChannelMask) : mChannelCount;
            if (mMixerBufferValid && (mEffectBufferValid || !mHasDataCopiedToSinkBuffer)) {
                void *buffer = mEffectBufferValid ? mEffectBuffer : mSinkBuffer;
                audio_format_t format = mEffectBufferValid ? mEffectBufferFormat : mFormat;

                // Apply mono blending and balancing if the effect buffer is not valid. Otherwise,
                // do these processes after effects are applied.
                if (!mEffectBufferValid) {
                    // mono blend occurs for mixer threads only (not direct or offloaded)
                    // and is handled here if we're going directly to the sink.
                    if (requireMonoBlend()) {
                        mono_blend(mMixerBuffer, mMixerBufferFormat, mChannelCount,
                                mNormalFrameCount, true /*limit*/);
                    }

                    if (!hasFastMixer()) {
                        // Balance must take effect after mono conversion.
                        // We do it here if there is no FastMixer.
                        // mBalance detects zero balance within the class for speed
                        // (not needed here).
                        mBalance.setBalance(mMasterBalance.load());
                        mBalance.process((float *)mMixerBuffer, mNormalFrameCount);
                    }
                }

                memcpy_by_audio_format(buffer, format, mMixerBuffer, mMixerBufferFormat,
                        mNormalFrameCount * (mixerChannelCount + mHapticChannelCount));

                // If we're going directly to the sink and there are haptic channels,
                // we should adjust channels as the sample data is partially interleaved
                // in this case.
                if (!mEffectBufferValid && mHapticChannelCount > 0) {
                    adjust_channels_non_destructive(buffer, mChannelCount, buffer,
                            mChannelCount + mHapticChannelCount,
                            audio_bytes_per_sample(format),
                            audio_bytes_per_frame(mChannelCount, format) * mNormalFrameCount);
                }
            }

            mBytesRemaining = mCurrentWriteLength;
            if (isSuspended()) {
                // Simulate write to HAL when suspended (e.g. BT SCO phone call).
                mSleepTimeUs = suspendSleepTimeUs(); // assumes full buffer.
                const size_t framesRemaining = mBytesRemaining / mFrameSize;
                mBytesWritten += mBytesRemaining;
                mFramesWritten += framesRemaining;
                mSuspendedFrames += framesRemaining; // to adjust kernel HAL position
                mBytesRemaining = 0;
            }

            // only process effects if we're going to write
            if (mSleepTimeUs == 0 && mType != OFFLOAD) {
                for (size_t i = 0; i < effectChains.size(); i ++) {
                    effectChains[i]->process_l();
                    // TODO: Write haptic data directly to sink buffer when mixing.
                    if (activeHapticSessionId != AUDIO_SESSION_NONE
                            && activeHapticSessionId == effectChains[i]->sessionId()) {
                        // Haptic data is active in this case, copy it directly from
                        // in buffer to out buffer.
                        uint32_t hapticSessionChannelCount = mEffectBufferValid ?
                                            audio_channel_count_from_out_mask(mMixerChannelMask) :
                                            mChannelCount;
                        if (mType == SPATIALIZER && !isHapticSessionSpatialized) {
                            hapticSessionChannelCount = mChannelCount;
                        }

                        const size_t audioBufferSize = mNormalFrameCount
                            * audio_bytes_per_frame(hapticSessionChannelCount,
                                                    AUDIO_FORMAT_PCM_FLOAT);
                        memcpy_by_audio_format(
                                (uint8_t*)effectChains[i]->outBuffer() + audioBufferSize,
                                AUDIO_FORMAT_PCM_FLOAT,
                                (const uint8_t*)effectChains[i]->inBuffer() + audioBufferSize,
                                AUDIO_FORMAT_PCM_FLOAT, mNormalFrameCount * mHapticChannelCount);
                    }
                }
            }
        }
        // Process effect chains for offloaded thread even if no audio
        // was read from audio track: process only updates effect state
        // and thus does have to be synchronized with audio writes but may have
        // to be called while waiting for async write callback
        if (mType == OFFLOAD) {
            for (size_t i = 0; i < effectChains.size(); i ++) {
                effectChains[i]->process_l();
            }
        }

        // Only if the Effects buffer is enabled and there is data in the
        // Effects buffer (buffer valid), we need to
        // copy into the sink buffer.
        // TODO use mSleepTimeUs == 0 as an additional condition.
        if (mEffectBufferValid && !mHasDataCopiedToSinkBuffer) {
            //ALOGV("writing effect buffer to sink buffer format %#x", mFormat);
            void *effectBuffer = (mType == SPATIALIZER) ? mPostSpatializerBuffer : mEffectBuffer;
            if (requireMonoBlend()) {
                mono_blend(effectBuffer, mEffectBufferFormat, mChannelCount, mNormalFrameCount,
                           true /*limit*/);
            }

            if (!hasFastMixer()) {
                // Balance must take effect after mono conversion.
                // We do it here if there is no FastMixer.
                // mBalance detects zero balance within the class for speed (not needed here).
                mBalance.setBalance(mMasterBalance.load());
                mBalance.process((float *)effectBuffer, mNormalFrameCount);
            }

            // for SPATIALIZER thread, Move haptics channels from mEffectBuffer to
            // mPostSpatializerBuffer if the haptics track is spatialized.
            // Otherwise, the haptics channels are already in mPostSpatializerBuffer.
            // For other thread types, the haptics channels are already in mEffectBuffer.
            if (mType == SPATIALIZER && isHapticSessionSpatialized) {
                const size_t srcBufferSize = mNormalFrameCount *
                        audio_bytes_per_frame(audio_channel_count_from_out_mask(mMixerChannelMask),
                                              mEffectBufferFormat);
                const size_t dstBufferSize = mNormalFrameCount
                        * audio_bytes_per_frame(mChannelCount, mEffectBufferFormat);

                memcpy_by_audio_format((uint8_t*)mPostSpatializerBuffer + dstBufferSize,
                                       mEffectBufferFormat,
                                       (uint8_t*)mEffectBuffer + srcBufferSize,
                                       mEffectBufferFormat,
                                       mNormalFrameCount * mHapticChannelCount);
            }
            const size_t framesToCopy = mNormalFrameCount * (mChannelCount + mHapticChannelCount);
            if (mFormat == AUDIO_FORMAT_PCM_FLOAT &&
                    mEffectBufferFormat == AUDIO_FORMAT_PCM_FLOAT) {
                // Clamp PCM float values more than this distance from 0 to insulate
                // a HAL which doesn't handle NaN correctly.
                static constexpr float HAL_FLOAT_SAMPLE_LIMIT = 2.0f;
                memcpy_to_float_from_float_with_clamping(static_cast<float*>(mSinkBuffer),
                        static_cast<const float*>(effectBuffer),
                        framesToCopy, HAL_FLOAT_SAMPLE_LIMIT /* absMax */);
            } else {
                memcpy_by_audio_format(mSinkBuffer, mFormat,
                        effectBuffer, mEffectBufferFormat, framesToCopy);
            }
            // The sample data is partially interleaved when haptic channels exist,
            // we need to adjust channels here.
            if (mHapticChannelCount > 0) {
                adjust_channels_non_destructive(mSinkBuffer, mChannelCount, mSinkBuffer,
                        mChannelCount + mHapticChannelCount,
                        audio_bytes_per_sample(mFormat),
                        audio_bytes_per_frame(mChannelCount, mFormat) * mNormalFrameCount);
            }
        }

        // enable changes in effect chain
        unlockEffectChains(effectChains);

        if (!metadataUpdate.playbackMetadataUpdate.empty()) {
            mAfThreadCallback->getMelReporter()->updateMetadataForCsd(id(),
                    metadataUpdate.playbackMetadataUpdate);
        }

        if (!waitingAsyncCallback()) {
            // mSleepTimeUs == 0 means we must write to audio hardware
            if (mSleepTimeUs == 0) {
                ssize_t ret = 0;
                // writePeriodNs is updated >= 0 when ret > 0.
                int64_t writePeriodNs = -1;
                if (mBytesRemaining) {
                    // FIXME rewrite to reduce number of system calls
                    const int64_t lastIoBeginNs = systemTime();
                    ret = threadLoop_write();
                    const int64_t lastIoEndNs = systemTime();
                    if (ret < 0) {
                        mBytesRemaining = 0;
                    } else if (ret > 0) {
                        mBytesWritten += ret;
                        mBytesRemaining -= ret;
                        const int64_t frames = ret / mFrameSize;
                        mFramesWritten += frames;

                        writePeriodNs = lastIoEndNs - mLastIoEndNs;
                        // process information relating to write time.
                        if (audio_has_proportional_frames(mFormat)) {
                            // we are in a continuous mixing cycle
                            if (mMixerStatus == MIXER_TRACKS_READY &&
                                    loopCount == lastLoopCountWritten + 1) {

                                const double jitterMs =
                                        TimestampVerifier<int64_t, int64_t>::computeJitterMs(
                                                {frames, writePeriodNs},
                                                {0, 0} /* lastTimestamp */, mSampleRate);
                                const double processMs =
                                       (lastIoBeginNs - mLastIoEndNs) * 1e-6;

                                audio_utils::lock_guard _l(mutex());
                                mIoJitterMs.add(jitterMs);
                                mProcessTimeMs.add(processMs);

                                if (mPipeSink.get() != nullptr) {
                                    // Using the Monopipe availableToWrite, we estimate the current
                                    // buffer size.
                                    MonoPipe* monoPipe = static_cast<MonoPipe*>(mPipeSink.get());
                                    const ssize_t
                                            availableToWrite = mPipeSink->availableToWrite();
                                    const size_t pipeFrames = monoPipe->maxFrames();
                                    const size_t
                                            remainingFrames = pipeFrames - max(availableToWrite, 0);
                                    mMonopipePipeDepthStats.add(remainingFrames);
                                }
                            }

                            // write blocked detection
                            const int64_t deltaWriteNs = lastIoEndNs - lastIoBeginNs;
                            if ((mType == MIXER || mType == SPATIALIZER)
                                    && deltaWriteNs > maxPeriod) {
                                mNumDelayedWrites++;
                                if ((lastIoEndNs - lastWarning) > kWarningThrottleNs) {
                                    ATRACE_NAME("underrun");
                                    ALOGW("write blocked for %lld msecs, "
                                            "%d delayed writes, thread %d",
                                            (long long)deltaWriteNs / NANOS_PER_MILLISECOND,
                                            mNumDelayedWrites, mId);
                                    lastWarning = lastIoEndNs;
                                }
                            }
                        }
                        // update timing info.
                        mLastIoBeginNs = lastIoBeginNs;
                        mLastIoEndNs = lastIoEndNs;
                        lastLoopCountWritten = loopCount;
                    }
                } else if ((mMixerStatus == MIXER_DRAIN_TRACK) ||
                        (mMixerStatus == MIXER_DRAIN_ALL)) {
                    threadLoop_drain();
                }
                if ((mType == MIXER || mType == SPATIALIZER) && !mStandby) {

                    if (mThreadThrottle
                            && mMixerStatus == MIXER_TRACKS_READY // we are mixing (active tracks)
                            && writePeriodNs > 0) {               // we have write period info
                        // Limit MixerThread data processing to no more than twice the
                        // expected processing rate.
                        //
                        // This helps prevent underruns with NuPlayer and other applications
                        // which may set up buffers that are close to the minimum size, or use
                        // deep buffers, and rely on a double-buffering sleep strategy to fill.
                        //
                        // The throttle smooths out sudden large data drains from the device,
                        // e.g. when it comes out of standby, which often causes problems with
                        // (1) mixer threads without a fast mixer (which has its own warm-up)
                        // (2) minimum buffer sized tracks (even if the track is full,
                        //     the app won't fill fast enough to handle the sudden draw).
                        //
                        // Total time spent in last processing cycle equals time spent in
                        // 1. threadLoop_write, as well as time spent in
                        // 2. threadLoop_mix (significant for heavy mixing, especially
                        //                    on low tier processors)

                        // it's OK if deltaMs is an overestimate.

                        const int32_t deltaMs = writePeriodNs / NANOS_PER_MILLISECOND;

                        const int32_t throttleMs = (int32_t)mHalfBufferMs - deltaMs;
                        if ((signed)mHalfBufferMs >= throttleMs && throttleMs > 0) {
                            mThreadMetrics.logThrottleMs((double)throttleMs);

                            usleep(throttleMs * 1000);
                            // notify of throttle start on verbose log
                            ALOGV_IF(mThreadThrottleEndMs == mThreadThrottleTimeMs,
                                    "mixer(%p) throttle begin:"
                                    " ret(%zd) deltaMs(%d) requires sleep %d ms",
                                    this, ret, deltaMs, throttleMs);
                            mThreadThrottleTimeMs += throttleMs;
                            // Throttle must be attributed to the previous mixer loop's write time
                            // to allow back-to-back throttling.
                            // This also ensures proper timing statistics.
                            mLastIoEndNs = systemTime();  // we fetch the write end time again.
                        } else {
                            uint32_t diff = mThreadThrottleTimeMs - mThreadThrottleEndMs;
                            if (diff > 0) {
                                // notify of throttle end on debug log
                                // but prevent spamming for bluetooth
                                ALOGD_IF(!isSingleDeviceType(
                                                 outDeviceTypes_l(), audio_is_a2dp_out_device) &&
                                         !isSingleDeviceType(
                                                 outDeviceTypes_l(),
                                                 audio_is_hearing_aid_out_device),
                                        "mixer(%p) throttle end: throttle time(%u)", this, diff);
                                mThreadThrottleEndMs = mThreadThrottleTimeMs;
                            }
                        }
                    }
                }

            } else {
                ATRACE_BEGIN("sleep");
                audio_utils::unique_lock _l(mutex());
                // suspended requires accurate metering of sleep time.
                if (isSuspended()) {
                    // advance by expected sleepTime
                    timeLoopNextNs += microseconds((nsecs_t)mSleepTimeUs);
                    const nsecs_t nowNs = systemTime();

                    // compute expected next time vs current time.
                    // (negative deltas are treated as delays).
                    nsecs_t deltaNs = timeLoopNextNs - nowNs;
                    if (deltaNs < -kMaxNextBufferDelayNs) {
                        // Delays longer than the max allowed trigger a reset.
                        ALOGV("DelayNs: %lld, resetting timeLoopNextNs", (long long) deltaNs);
                        deltaNs = microseconds((nsecs_t)mSleepTimeUs);
                        timeLoopNextNs = nowNs + deltaNs;
                    } else if (deltaNs < 0) {
                        // Delays within the max delay allowed: zero the delta/sleepTime
                        // to help the system catch up in the next iteration(s)
                        ALOGV("DelayNs: %lld, catching-up", (long long) deltaNs);
                        deltaNs = 0;
                    }
                    // update sleep time (which is >= 0)
                    mSleepTimeUs = deltaNs / 1000;
                }
                if (!mSignalPending && mConfigEvents.isEmpty() && !exitPending()) {
                    mWaitWorkCV.wait_for(_l, std::chrono::microseconds(mSleepTimeUs));
                }
                ATRACE_END();
            }
        }

        // Finally let go of removed track(s), without the lock held
        // since we can't guarantee the destructors won't acquire that
        // same lock.  This will also mutate and push a new fast mixer state.
        threadLoop_removeTracks(tracksToRemove);
        tracksToRemove.clear();

        // FIXME I don't understand the need for this here;
        //       it was in the original code but maybe the
        //       assignment in saveOutputTracks() makes this unnecessary?
        clearOutputTracks();

        // Effect chains will be actually deleted here if they were removed from
        // mEffectChains list during mixing or effects processing
        effectChains.clear();

        // FIXME Note that the above .clear() is no longer necessary since effectChains
        // is now local to this block, but will keep it for now (at least until merge done).

        mThreadloopExecutor.process();
    }
    mThreadloopExecutor.process(); // process any remaining deferred actions.
    // deferred actions after this point are ignored.

    threadLoop_exit();

    if (!mStandby) {
        threadLoop_standby();
        setStandby();
    }

    releaseWakeLock();

    ALOGV("Thread %p type %d exiting", this, mType);
    return false;
}

void PlaybackThread::collectTimestamps_l()
{
    if (mStandby) {
        mTimestampVerifier.discontinuity(discontinuityForStandbyOrFlush());
        return;
    } else if (mHwPaused) {
        mTimestampVerifier.discontinuity(mTimestampVerifier.DISCONTINUITY_MODE_CONTINUOUS);
        return;
    }

    // Gather the framesReleased counters for all active tracks,
    // and associate with the sink frames written out.  We need
    // this to convert the sink timestamp to the track timestamp.
    bool kernelLocationUpdate = false;
    ExtendedTimestamp timestamp; // use private copy to fetch

    // Always query HAL timestamp and update timestamp verifier. In standby or pause,
    // HAL may be draining some small duration buffered data for fade out.
    if (threadloop_getHalTimestamp_l(&timestamp) == OK) {
        mTimestampVerifier.add(timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL],
                timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL],
                mSampleRate);

        if (isTimestampCorrectionEnabled_l()) {
            ALOGVV("TS_BEFORE: %d %lld %lld", id(),
                    (long long)timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL],
                    (long long)timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]);
            auto correctedTimestamp = mTimestampVerifier.getLastCorrectedTimestamp();
            timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]
                    = correctedTimestamp.mFrames;
            timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL]
                    = correctedTimestamp.mTimeNs;
            ALOGVV("TS_AFTER: %d %lld %lld", id(),
                    (long long)timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL],
                    (long long)timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]);

            // Note: Downstream latency only added if timestamp correction enabled.
            if (mDownstreamLatencyStatMs.getN() > 0) { // we have latency info.
                const int64_t newPosition =
                        timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]
                        - int64_t(mDownstreamLatencyStatMs.getMean() * mSampleRate * 1e-3);
                // prevent retrograde
                timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL] = max(
                        newPosition,
                        (mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]
                                - mSuspendedFrames));
            }
        }

        // We always fetch the timestamp here because often the downstream
        // sink will block while writing.

        // We keep track of the last valid kernel position in case we are in underrun
        // and the normal mixer period is the same as the fast mixer period, or there
        // is some error from the HAL.
        if (mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] >= 0) {
            mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL_LASTKERNELOK] =
                    mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL];
            mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL_LASTKERNELOK] =
                    mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL];

            mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER_LASTKERNELOK] =
                    mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER];
            mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_SERVER_LASTKERNELOK] =
                    mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_SERVER];
        }

        if (timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] >= 0) {
            kernelLocationUpdate = true;
        } else {
            ALOGVV("getTimestamp error - no valid kernel position");
        }

        // copy over kernel info
        mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL] =
                timestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL]
                + mSuspendedFrames; // add frames discarded when suspended
        mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] =
                timestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL];
    } else {
        mTimestampVerifier.error();
    }

    // mFramesWritten for non-offloaded tracks are contiguous
    // even after standby() is called. This is useful for the track frame
    // to sink frame mapping.
    bool serverLocationUpdate = false;
    if (mFramesWritten != mLastFramesWritten) {
        serverLocationUpdate = true;
        mLastFramesWritten = mFramesWritten;
    }
    // Only update timestamps if there is a meaningful change.
    // Either the kernel timestamp must be valid or we have written something.
    if (kernelLocationUpdate || serverLocationUpdate) {
        if (serverLocationUpdate) {
            // use the time before we called the HAL write - it is a bit more accurate
            // to when the server last read data than the current time here.
            //
            // If we haven't written anything, mLastIoBeginNs will be -1
            // and we use systemTime().
            mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER] = mFramesWritten;
            mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_SERVER] = mLastIoBeginNs == -1
                    ? systemTime() : (int64_t)mLastIoBeginNs;
        }

        for (const sp<IAfTrack>& t : mActiveTracks) {
            if (!t->isFastTrack()) {
                t->updateTrackFrameInfo(
                        t->audioTrackServerProxy()->framesReleased(),
                        mFramesWritten,
                        mSampleRate,
                        mTimestamp);
            }
        }
    }

    if (audio_has_proportional_frames(mFormat)) {
        const double latencyMs = mTimestamp.getOutputServerLatencyMs(mSampleRate);
        if (latencyMs != 0.) { // note 0. means timestamp is empty.
            mLatencyMs.add(latencyMs);
        }
    }
#if 0
    // logFormat example
    if (z % 100 == 0) {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        LOGT("This is an integer %d, this is a float %f, this is my "
            "pid %p %% %s %t", 42, 3.14, "and this is a timestamp", ts);
        LOGT("A deceptive null-terminated string %\0");
    }
    ++z;
#endif
}

// removeTracks_l() must be called with ThreadBase::mutex() held
void PlaybackThread::removeTracks_l(const Vector<sp<IAfTrack>>& tracksToRemove)
NO_THREAD_SAFETY_ANALYSIS  // release and re-acquire mutex()
{
    if (tracksToRemove.empty()) return;

    // Block all incoming TrackHandle requests until we are finished with the release.
    setThreadBusy_l(true);

    for (const auto& track : tracksToRemove) {
        ALOGV("%s(%d): removing track on session %d", __func__, track->id(), track->sessionId());
        sp<IAfEffectChain> chain = getEffectChain_l(track->sessionId());
        if (chain != 0) {
            ALOGV("%s(%d): stopping track on chain %p for session Id: %d",
                    __func__, track->id(), chain.get(), track->sessionId());
            chain->decActiveTrackCnt();
        }

        // If an external client track, inform APM we're no longer active, and remove if needed.
        // Since the track is active, we do it here instead of TrackBase::destroy().
        if (track->isExternalTrack()) {
            mutex().unlock();
            AudioSystem::stopOutput(track->portId());
            if (track->isTerminated()) {
                AudioSystem::releaseOutput(track->portId());
            }
            mutex().lock();
        }
        if (mHapticChannelCount > 0 &&
                ((track->channelMask() & AUDIO_CHANNEL_HAPTIC_ALL) != AUDIO_CHANNEL_NONE
                        || (chain != nullptr && chain->containsHapticGeneratingEffect()))) {
            mutex().unlock();
            // Unlock due to VibratorService will lock for this call and will
            // call Tracks.mute/unmute which also require thread's lock.
            afutils::onExternalVibrationStop(track->getExternalVibration());
            mutex().lock();

            // When the track is stop, set the haptic intensity as MUTE
            // for the HapticGenerator effect.
            if (chain != nullptr) {
                chain->setHapticScale_l(track->id(), os::HapticScale::mute());
            }
        }

        // Under lock, the track is removed from the active tracks list.
        //
        // Once the track is no longer active, the TrackHandle may directly
        // modify it as the threadLoop() is no longer responsible for its maintenance.
        // Do not modify the track from threadLoop after the mutex is unlocked
        // if it is not active.
        mActiveTracks.remove(track);

        if (track->isTerminated()) {
            // remove from our tracks vector
            removeTrack_l(track);
        }
    }

    // Allow incoming TrackHandle requests.  We still hold the mutex,
    // so pending TrackHandle requests will occur after we unlock it.
    setThreadBusy_l(false);
}

status_t PlaybackThread::getTimestamp_l(AudioTimestamp& timestamp)
{
    if (mNormalSink != 0) {
        ExtendedTimestamp ets;
        status_t status = mNormalSink->getTimestamp(ets);
        if (status == NO_ERROR) {
            status = ets.getBestTimestamp(&timestamp);
        }
        return status;
    }
    if ((mType == OFFLOAD || mType == DIRECT) && mOutput != NULL) {
        collectTimestamps_l();
        if (mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] <= 0) {
            return INVALID_OPERATION;
        }
        timestamp.mPosition = mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL];
        const int64_t timeNs = mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL];
        timestamp.mTime.tv_sec = timeNs / NANOS_PER_SECOND;
        timestamp.mTime.tv_nsec = timeNs - (timestamp.mTime.tv_sec * NANOS_PER_SECOND);
        return NO_ERROR;
    }
    return INVALID_OPERATION;
}

// For dedicated VoIP outputs, let the HAL apply the stream volume. Track volume is
// still applied by the mixer.
// All tracks attached to a mixer with flag VOIP_RX are tied to the same
// stream type STREAM_VOICE_CALL so this will only change the HAL volume once even
// if more than one track are active
status_t PlaybackThread::handleVoipVolume_l(float* volume)
{
    status_t result = NO_ERROR;
    if ((mOutput->flags & AUDIO_OUTPUT_FLAG_VOIP_RX) != 0) {
        if (*volume != mLeftVolFloat) {
            result = mOutput->stream->setVolume(*volume, *volume);
            // HAL can return INVALID_OPERATION if operation is not supported.
            ALOGE_IF(result != OK && result != INVALID_OPERATION,
                     "Error when setting output stream volume: %d", result);
            if (result == NO_ERROR) {
                mLeftVolFloat = *volume;
            }
        }
        // if stream volume was successfully sent to the HAL, mLeftVolFloat == v here and we
        // remove stream volume contribution from software volume.
        if (mLeftVolFloat == *volume) {
            *volume = 1.0f;
        }
    }
    return result;
}

status_t MixerThread::createAudioPatch_l(const struct audio_patch* patch,
                                                          audio_patch_handle_t *handle)
{
    status_t status;
    if (property_get_bool("af.patch_park", false /* default_value */)) {
        // Park FastMixer to avoid potential DOS issues with writing to the HAL
        // or if HAL does not properly lock against access.
        AutoPark<FastMixer> park(mFastMixer);
        status = PlaybackThread::createAudioPatch_l(patch, handle);
    } else {
        status = PlaybackThread::createAudioPatch_l(patch, handle);
    }

    updateHalSupportedLatencyModes_l();
    return status;
}

status_t PlaybackThread::createAudioPatch_l(const struct audio_patch *patch,
                                                          audio_patch_handle_t *handle)
{
    status_t status = NO_ERROR;

    // store new device and send to effects
    audio_devices_t type = AUDIO_DEVICE_NONE;
    AudioDeviceTypeAddrVector deviceTypeAddrs;
    for (unsigned int i = 0; i < patch->num_sinks; i++) {
        LOG_ALWAYS_FATAL_IF(popcount(patch->sinks[i].ext.device.type) > 1
                            && !mOutput->audioHwDev->supportsAudioPatches(),
                            "Enumerated device type(%#x) must not be used "
                            "as it does not support audio patches",
                            patch->sinks[i].ext.device.type);
        type = static_cast<audio_devices_t>(type | patch->sinks[i].ext.device.type);
        deviceTypeAddrs.emplace_back(patch->sinks[i].ext.device.type,
                patch->sinks[i].ext.device.address);
    }

    audio_port_handle_t sinkPortId = patch->sinks[0].id;
#ifdef ADD_BATTERY_DATA
    // when changing the audio output device, call addBatteryData to notify
    // the change
    if (outDeviceTypes() != deviceTypes) {
        uint32_t params = 0;
        // check whether speaker is on
        if (deviceTypes.count(AUDIO_DEVICE_OUT_SPEAKER) > 0) {
            params |= IMediaPlayerService::kBatteryDataSpeakerOn;
        }

        // check if any other device (except speaker) is on
        if (!isSingleDeviceType(deviceTypes, AUDIO_DEVICE_OUT_SPEAKER)) {
            params |= IMediaPlayerService::kBatteryDataOtherAudioDeviceOn;
        }

        if (params != 0) {
            addBatteryData(params);
        }
    }
#endif

    for (size_t i = 0; i < mEffectChains.size(); i++) {
        mEffectChains[i]->setDevices_l(deviceTypeAddrs);
    }

    // mPatch.num_sinks is not set when the thread is created so that
    // the first patch creation triggers an ioConfigChanged callback
    bool configChanged = (mPatch.num_sinks == 0) ||
                         (mPatch.sinks[0].id != sinkPortId);
    mPatch = *patch;
    mOutDeviceTypeAddrs = deviceTypeAddrs;
    checkSilentMode_l();

    if (mOutput->audioHwDev->supportsAudioPatches()) {
        sp<DeviceHalInterface> hwDevice = mOutput->audioHwDev->hwDevice();
        status = hwDevice->createAudioPatch(patch->num_sources,
                                            patch->sources,
                                            patch->num_sinks,
                                            patch->sinks,
                                            handle);
    } else {
        status = mOutput->stream->legacyCreateAudioPatch(patch->sinks[0], std::nullopt, type);
        *handle = AUDIO_PATCH_HANDLE_NONE;
    }
    const std::string patchSinksAsString = patchSinksToString(patch);

    mThreadMetrics.logEndInterval();
    mThreadMetrics.logCreatePatch(/* inDevices */ {}, patchSinksAsString);
    mThreadMetrics.logBeginInterval();
    // also dispatch to active AudioTracks for MediaMetrics
    for (const auto &track : mActiveTracks) {
        track->logEndInterval();
        track->logBeginInterval(patchSinksAsString);
    }

    if (configChanged) {
        sendIoConfigEvent_l(AUDIO_OUTPUT_CONFIG_CHANGED);
    }
    // Force metadata update after a route change
    mActiveTracks.setHasChanged();

    return status;
}

status_t MixerThread::releaseAudioPatch_l(const audio_patch_handle_t handle)
{
    status_t status;
    if (property_get_bool("af.patch_park", false /* default_value */)) {
        // Park FastMixer to avoid potential DOS issues with writing to the HAL
        // or if HAL does not properly lock against access.
        AutoPark<FastMixer> park(mFastMixer);
        status = PlaybackThread::releaseAudioPatch_l(handle);
    } else {
        status = PlaybackThread::releaseAudioPatch_l(handle);
    }
    return status;
}

status_t PlaybackThread::releaseAudioPatch_l(const audio_patch_handle_t handle)
{
    status_t status = NO_ERROR;

    mPatch = audio_patch{};
    mOutDeviceTypeAddrs.clear();

    if (mOutput->audioHwDev->supportsAudioPatches()) {
        sp<DeviceHalInterface> hwDevice = mOutput->audioHwDev->hwDevice();
        status = hwDevice->releaseAudioPatch(handle);
    } else {
        status = mOutput->stream->legacyReleaseAudioPatch();
    }
    // Force meteadata update after a route change
    mActiveTracks.setHasChanged();

    return status;
}

void PlaybackThread::addPatchTrack(const sp<IAfPatchTrack>& track)
{
    audio_utils::lock_guard _l(mutex());
    mTracks.add(track);
}

void PlaybackThread::deletePatchTrack(const sp<IAfPatchTrack>& track)
{
    audio_utils::lock_guard _l(mutex());
    destroyTrack_l(track);
}

void PlaybackThread::toAudioPortConfig(struct audio_port_config* config)
{
    ThreadBase::toAudioPortConfig(config);
    config->role = AUDIO_PORT_ROLE_SOURCE;
    config->ext.mix.hw_module = mOutput->audioHwDev->handle();
    config->ext.mix.usecase.stream = AUDIO_STREAM_DEFAULT;
    if (mOutput && mOutput->flags != AUDIO_OUTPUT_FLAG_NONE) {
        config->config_mask |= AUDIO_PORT_CONFIG_FLAGS;
        config->flags.output = mOutput->flags;
    }
}

std::string PlaybackThread::getLocalLogHeader() const {
    using namespace std::literals;
    static constexpr auto indent = "                             "
                                   "                            "sv;
    return std::string{indent}.append(IAfTrack::getLogHeader());
}
// ----------------------------------------------------------------------------

/* static */
sp<IAfPlaybackThread> IAfPlaybackThread::createMixerThread(
        const sp<IAfThreadCallback>& afThreadCallback, AudioStreamOut* output,
        audio_io_handle_t id, bool systemReady, type_t type, audio_config_base_t* mixerConfig) {
    return sp<MixerThread>::make(afThreadCallback, output, id, systemReady, type, mixerConfig);
}

MixerThread::MixerThread(const sp<IAfThreadCallback>& afThreadCallback, AudioStreamOut* output,
        audio_io_handle_t id, bool systemReady, type_t type, audio_config_base_t *mixerConfig)
    :   PlaybackThread(afThreadCallback, output, id, type, systemReady, mixerConfig),
        // mAudioMixer below
        // mFastMixer below
        mBluetoothLatencyModesEnabled(false),
        mFastMixerFutex(0),
        mMasterMono(false)
        // mOutputSink below
        // mPipeSink below
        // mNormalSink below
{
    ALOGV("MixerThread() id=%d type=%d", id, type);
    ALOGV("mSampleRate=%u, mChannelMask=%#x, mChannelCount=%u, mFormat=%#x, mFrameSize=%zu, "
            "mFrameCount=%zu, mNormalFrameCount=%zu",
            mSampleRate, mChannelMask, mChannelCount, mFormat, mFrameSize, mFrameCount,
            mNormalFrameCount);
    mAudioMixer = new AudioMixer(mNormalFrameCount, mSampleRate);

    if (type == DUPLICATING) {
        // The Duplicating thread uses the AudioMixer and delivers data to OutputTracks
        // (downstream MixerThreads) in DuplicatingThread::threadLoop_write().
        // Do not create or use mFastMixer, mOutputSink, mPipeSink, or mNormalSink.
        // Balance is *not* set in the DuplicatingThread here (or from AudioFlinger),
        // as the downstream MixerThreads implement it.
        return;
    }
    // create an NBAIO sink for the HAL output stream, and negotiate
    mOutputSink = new AudioStreamOutSink(output->stream);
    size_t numCounterOffers = 0;
    const NBAIO_Format offers[1] = {Format_from_SR_C(
            mSampleRate, mChannelCount + mHapticChannelCount, mFormat)};
#if !LOG_NDEBUG
    ssize_t index =
#else
    (void)
#endif
            mOutputSink->negotiate(offers, 1, NULL, numCounterOffers);
    ALOG_ASSERT(index == 0);

    // initialize fast mixer depending on configuration
    bool initFastMixer;
    if (mType == SPATIALIZER || mType == BIT_PERFECT) {
        initFastMixer = false;
    } else {
        switch (kUseFastMixer) {
        case FastMixer_Never:
            initFastMixer = false;
            break;
        case FastMixer_Always:
            initFastMixer = true;
            break;
        case FastMixer_Static:
        case FastMixer_Dynamic:
            if (mType == MIXER && (output->flags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER)) {
                /* Do not init fast mixer on deep buffer, warn if buffers are confed too small */
                initFastMixer = false;
                ALOGW_IF(mFrameCount * 1000 / mSampleRate < kMinNormalSinkBufferSizeMs,
                         "HAL DEEP BUFFER Buffer (%zu ms) is smaller than set minimal buffer "
                         "(%u ms), seems like a configuration error",
                         mFrameCount * 1000 / mSampleRate, kMinNormalSinkBufferSizeMs);
            } else {
                initFastMixer = mFrameCount < mNormalFrameCount;
            }
            break;
        }
        ALOGW_IF(initFastMixer == false && mFrameCount < mNormalFrameCount,
                "FastMixer is preferred for this sink as frameCount %zu is less than threshold %zu",
                mFrameCount, mNormalFrameCount);
    }
    if (initFastMixer) {
        audio_format_t fastMixerFormat;
        if (mMixerBufferEnabled && mEffectBufferEnabled) {
            fastMixerFormat = AUDIO_FORMAT_PCM_FLOAT;
        } else {
            fastMixerFormat = AUDIO_FORMAT_PCM_16_BIT;
        }
        if (mFormat != fastMixerFormat) {
            // change our Sink format to accept our intermediate precision
            mFormat = fastMixerFormat;
            free(mSinkBuffer);
            mFrameSize = audio_bytes_per_frame(mChannelCount + mHapticChannelCount, mFormat);
            const size_t sinkBufferSize = mNormalFrameCount * mFrameSize;
            (void)posix_memalign(&mSinkBuffer, 32, sinkBufferSize);
        }

        // create a MonoPipe to connect our submix to FastMixer
        NBAIO_Format format = mOutputSink->format();

        // adjust format to match that of the Fast Mixer
        ALOGV("format changed from %#x to %#x", format.mFormat, fastMixerFormat);
        format.mFormat = fastMixerFormat;
        format.mFrameSize = audio_bytes_per_sample(format.mFormat) * format.mChannelCount;

        // This pipe depth compensates for scheduling latency of the normal mixer thread.
        // When it wakes up after a maximum latency, it runs a few cycles quickly before
        // finally blocking.  Note the pipe implementation rounds up the request to a power of 2.
        MonoPipe *monoPipe = new MonoPipe(mNormalFrameCount * 4, format, true /*writeCanBlock*/);
        const NBAIO_Format offersFast[1] = {format};
        size_t numCounterOffersFast = 0;
#if !LOG_NDEBUG
        index =
#else
        (void)
#endif
                monoPipe->negotiate(offersFast, std::size(offersFast),
                        nullptr /* counterOffers */, numCounterOffersFast);
        ALOG_ASSERT(index == 0);
        monoPipe->setAvgFrames((mScreenState & 1) ?
                (monoPipe->maxFrames() * 7) / 8 : mNormalFrameCount * 2);
        mPipeSink = monoPipe;

        // create fast mixer and configure it initially with just one fast track for our submix
        mFastMixer = new FastMixer(mId);
        FastMixerStateQueue *sq = mFastMixer->sq();
#ifdef STATE_QUEUE_DUMP
        sq->setObserverDump(&mStateQueueObserverDump);
        sq->setMutatorDump(&mStateQueueMutatorDump);
#endif
        FastMixerState *state = sq->begin();
        FastTrack *fastTrack = &state->mFastTracks[0];
        // wrap the source side of the MonoPipe to make it an AudioBufferProvider
        fastTrack->mBufferProvider = new SourceAudioBufferProvider(new MonoPipeReader(monoPipe));
        fastTrack->mVolumeProvider = NULL;
        fastTrack->mChannelMask = static_cast<audio_channel_mask_t>(
                mChannelMask | mHapticChannelMask); // mPipeSink channel mask for
                                                    // audio to FastMixer
        fastTrack->mFormat = mFormat; // mPipeSink format for audio to FastMixer
        fastTrack->mHapticPlaybackEnabled = mHapticChannelMask != AUDIO_CHANNEL_NONE;
        fastTrack->mHapticScale = os::HapticScale::none();
        fastTrack->mHapticMaxAmplitude = NAN;
        fastTrack->mGeneration++;
        snprintf(fastTrack->mTraceName, sizeof(fastTrack->mTraceName),
                 "%s.0.0.%d", AUDIO_TRACE_PREFIX_AUDIO_TRACK_FRDY, mId);
        state->mFastTracksGen++;
        state->mTrackMask = 1;
        // fast mixer will use the HAL output sink
        state->mOutputSink = mOutputSink.get();
        state->mOutputSinkGen++;
        state->mFrameCount = mFrameCount;
        // specify sink channel mask when haptic channel mask present as it can not
        // be calculated directly from channel count
        state->mSinkChannelMask = mHapticChannelMask == AUDIO_CHANNEL_NONE
                ? AUDIO_CHANNEL_NONE
                : static_cast<audio_channel_mask_t>(mChannelMask | mHapticChannelMask);
        state->mCommand = FastMixerState::COLD_IDLE;
        // already done in constructor initialization list
        //mFastMixerFutex = 0;
        state->mColdFutexAddr = &mFastMixerFutex;
        state->mColdGen++;
        state->mDumpState = &mFastMixerDumpState;
        sq->end();
        {
            audio_utils::mutex::scoped_queue_wait_check queueWaitCheck(mFastMixer->getTid());
            sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
        }

        // start the fast mixer
        mFastMixer->run("FastMixer", PRIORITY_URGENT_AUDIO);
        pid_t tid = mFastMixer->getTid();
        sendPrioConfigEvent(getpid(), tid, kPriorityFastMixer, false /*forApp*/);
        stream()->setHalThreadPriority(kPriorityFastMixer);

#ifdef AUDIO_WATCHDOG
        // create and start the watchdog
        mAudioWatchdog = new AudioWatchdog();
        mAudioWatchdog->setDump(&mAudioWatchdogDump);
        mAudioWatchdog->run("AudioWatchdog", PRIORITY_URGENT_AUDIO);
        tid = mAudioWatchdog->getTid();
        sendPrioConfigEvent(getpid(), tid, kPriorityFastMixer, false /*forApp*/);
#endif
    } else {
#ifdef TEE_SINK
        // Only use the MixerThread tee if there is no FastMixer.
        mTee.set(mOutputSink->format(), NBAIO_Tee::TEE_FLAG_OUTPUT_THREAD);
        mTee.setId(std::string("_") + std::to_string(mId) + "_M");
#endif
    }

    switch (kUseFastMixer) {
    case FastMixer_Never:
    case FastMixer_Dynamic:
        mNormalSink = mOutputSink;
        break;
    case FastMixer_Always:
        mNormalSink = mPipeSink;
        break;
    case FastMixer_Static:
        mNormalSink = initFastMixer ? mPipeSink : mOutputSink;
        break;
    }
    // setMasterBalance needs to be called after the FastMixer
    // (if any) is set up, in order to deliver the balance settings to it.
    setMasterBalance(afThreadCallback->getMasterBalance_l());
}

MixerThread::~MixerThread()
{
    if (mFastMixer != 0) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (state->mCommand == FastMixerState::COLD_IDLE) {
            int32_t old = android_atomic_inc(&mFastMixerFutex);
            if (old == -1) {
                (void) syscall(__NR_futex, &mFastMixerFutex, FUTEX_WAKE_PRIVATE, 1);
            }
        }
        state->mCommand = FastMixerState::EXIT;
        sq->end();
        {
            audio_utils::mutex::scoped_join_wait_check queueWaitCheck(mFastMixer->getTid());
            sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
            mFastMixer->join();
        }
        // Though the fast mixer thread has exited, it's state queue is still valid.
        // We'll use that extract the final state which contains one remaining fast track
        // corresponding to our sub-mix.
        state = sq->begin();
        ALOG_ASSERT(state->mTrackMask == 1);
        FastTrack *fastTrack = &state->mFastTracks[0];
        ALOG_ASSERT(fastTrack->mBufferProvider != NULL);
        delete fastTrack->mBufferProvider;
        sq->end(false /*didModify*/);
        mFastMixer.clear();
#ifdef AUDIO_WATCHDOG
        if (mAudioWatchdog != 0) {
            mAudioWatchdog->requestExit();
            mAudioWatchdog->requestExitAndWait();
            mAudioWatchdog.clear();
        }
#endif
    }
    delete mAudioMixer;
}

void MixerThread::onFirstRef() {
    PlaybackThread::onFirstRef();

    audio_utils::lock_guard _l(mutex());
    if (mOutput != nullptr && mOutput->stream != nullptr) {
        status_t status = mOutput->stream->setLatencyModeCallback(this);
        if (status != INVALID_OPERATION) {
            updateHalSupportedLatencyModes_l();
        }
        // Default to enabled if the HAL supports it. This can be changed by Audioflinger after
        // the thread construction according to AudioFlinger::mBluetoothLatencyModesEnabled
        mBluetoothLatencyModesEnabled.store(
                mOutput->audioHwDev->supportsBluetoothVariableLatency());
    }
}

uint32_t MixerThread::correctLatency_l(uint32_t latency) const
{
    if (mFastMixer != 0) {
        MonoPipe *pipe = (MonoPipe *)mPipeSink.get();
        latency += (pipe->getAvgFrames() * 1000) / mSampleRate;
    }
    return latency;
}

ssize_t MixerThread::threadLoop_write()
{
    // FIXME we should only do one push per cycle; confirm this is true
    // Start the fast mixer if it's not already running
    if (mFastMixer != 0) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (state->mCommand != FastMixerState::MIX_WRITE &&
                (kUseFastMixer != FastMixer_Dynamic || state->mTrackMask > 1)) {
            if (state->mCommand == FastMixerState::COLD_IDLE) {

                // FIXME workaround for first HAL write being CPU bound on some devices
                ATRACE_BEGIN("write");
                mOutput->write((char *)mSinkBuffer, 0);
                ATRACE_END();

                int32_t old = android_atomic_inc(&mFastMixerFutex);
                if (old == -1) {
                    (void) syscall(__NR_futex, &mFastMixerFutex, FUTEX_WAKE_PRIVATE, 1);
                }
#ifdef AUDIO_WATCHDOG
                if (mAudioWatchdog != 0) {
                    mAudioWatchdog->resume();
                }
#endif
            }
            state->mCommand = FastMixerState::MIX_WRITE;
#ifdef FAST_THREAD_STATISTICS
            mFastMixerDumpState.increaseSamplingN(mAfThreadCallback->isLowRamDevice() ?
                FastThreadDumpState::kSamplingNforLowRamDevice : FastThreadDumpState::kSamplingN);
#endif
            sq->end();
            {
                audio_utils::mutex::scoped_queue_wait_check queueWaitCheck(mFastMixer->getTid());
                sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
            }
            if (kUseFastMixer == FastMixer_Dynamic) {
                mNormalSink = mPipeSink;
            }
        } else {
            sq->end(false /*didModify*/);
        }
    }
    return PlaybackThread::threadLoop_write();
}

void MixerThread::threadLoop_standby()
{
    // Idle the fast mixer if it's currently running
    if (mFastMixer != 0) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (!(state->mCommand & FastMixerState::IDLE)) {
            // Report any frames trapped in the Monopipe
            MonoPipe *monoPipe = (MonoPipe *)mPipeSink.get();
            const long long pipeFrames = monoPipe->maxFrames() - monoPipe->availableToWrite();
            mLocalLog.log("threadLoop_standby: framesWritten:%lld  suspendedFrames:%lld  "
                    "monoPipeWritten:%lld  monoPipeLeft:%lld",
                    (long long)mFramesWritten, (long long)mSuspendedFrames,
                    (long long)mPipeSink->framesWritten(), pipeFrames);
            mLocalLog.log("threadLoop_standby: %s", mTimestamp.toString().c_str());

            state->mCommand = FastMixerState::COLD_IDLE;
            state->mColdFutexAddr = &mFastMixerFutex;
            state->mColdGen++;
            mFastMixerFutex = 0;
            sq->end();
            // BLOCK_UNTIL_PUSHED would be insufficient, as we need it to stop doing I/O now
            {
                audio_utils::mutex::scoped_queue_wait_check queueWaitCheck(mFastMixer->getTid());
                sq->push(FastMixerStateQueue::BLOCK_UNTIL_ACKED);
            }
            if (kUseFastMixer == FastMixer_Dynamic) {
                mNormalSink = mOutputSink;
            }
#ifdef AUDIO_WATCHDOG
            if (mAudioWatchdog != 0) {
                mAudioWatchdog->pause();
            }
#endif
        } else {
            sq->end(false /*didModify*/);
        }
    }
    PlaybackThread::threadLoop_standby();
}

bool PlaybackThread::waitingAsyncCallback_l()
{
    return false;
}

bool PlaybackThread::shouldStandby_l()
{
    return !mStandby;
}

bool PlaybackThread::waitingAsyncCallback()
{
    audio_utils::lock_guard _l(mutex());
    return waitingAsyncCallback_l();
}

// shared by MIXER and DIRECT, overridden by DUPLICATING
void PlaybackThread::threadLoop_standby()
{
    ALOGV("%s: audio hardware entering standby, mixer %p, suspend count %d",
            __func__, this, (int32_t)mSuspended);
    mOutput->standby();
    if (mUseAsyncWrite != 0) {
        // discard any pending drain or write ack by incrementing sequence
        mWriteAckSequence = (mWriteAckSequence + 2) & ~1;
        mDrainSequence = (mDrainSequence + 2) & ~1;
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->setWriteBlocked(mWriteAckSequence);
        mCallbackThread->setDraining(mDrainSequence);
    }
    mHwPaused = false;
    setHalLatencyMode_l();
}

void PlaybackThread::onAddNewTrack_l()
{
    ALOGV("signal playback thread");
    broadcast_l();
}

void PlaybackThread::onAsyncError(bool isHardError)
{
    auto allTrackPortIds = getTrackPortIds();
    for (int i = AUDIO_STREAM_SYSTEM; i < (int)AUDIO_STREAM_CNT; i++) {
        invalidateTracks((audio_stream_type_t)i);
    }
    if (isHardError) {
        mAfThreadCallback->onHardError(allTrackPortIds);
    }
}

void MixerThread::threadLoop_mix()
{
    // mix buffers...
    mAudioMixer->process();
    mCurrentWriteLength = mSinkBufferSize;
    // increase sleep time progressively when application underrun condition clears.
    // Only increase sleep time if the mixer is ready for two consecutive times to avoid
    // that a steady state of alternating ready/not ready conditions keeps the sleep time
    // such that we would underrun the audio HAL.
    if ((mSleepTimeUs == 0) && (sleepTimeShift > 0)) {
        sleepTimeShift--;
    }
    mSleepTimeUs = 0;
    mStandbyTimeNs = systemTime() + mStandbyDelayNs;
    //TODO: delay standby when effects have a tail

}

void MixerThread::threadLoop_sleepTime()
{
    // If no tracks are ready, sleep once for the duration of an output
    // buffer size, then write 0s to the output
    if (mSleepTimeUs == 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            if (mPipeSink.get() != nullptr && mPipeSink == mNormalSink) {
                // Using the Monopipe availableToWrite, we estimate the
                // sleep time to retry for more data (before we underrun).
                MonoPipe *monoPipe = static_cast<MonoPipe *>(mPipeSink.get());
                const ssize_t availableToWrite = mPipeSink->availableToWrite();
                const size_t pipeFrames = monoPipe->maxFrames();
                const size_t framesLeft = pipeFrames - max(availableToWrite, 0);
                // HAL_framecount <= framesDelay ~ framesLeft / 2 <= Normal_Mixer_framecount
                const size_t framesDelay = std::min(
                        mNormalFrameCount, max(framesLeft / 2, mFrameCount));
                ALOGV("pipeFrames:%zu framesLeft:%zu framesDelay:%zu",
                        pipeFrames, framesLeft, framesDelay);
                mSleepTimeUs = framesDelay * MICROS_PER_SECOND / mSampleRate;
            } else {
                mSleepTimeUs = mActiveSleepTimeUs >> sleepTimeShift;
                if (mSleepTimeUs < kMinThreadSleepTimeUs) {
                    mSleepTimeUs = kMinThreadSleepTimeUs;
                }
                // reduce sleep time in case of consecutive application underruns to avoid
                // starving the audio HAL. As activeSleepTimeUs() is larger than a buffer
                // duration we would end up writing less data than needed by the audio HAL if
                // the condition persists.
                if (sleepTimeShift < kMaxThreadSleepTimeShift) {
                    sleepTimeShift++;
                }
            }
        } else {
            mSleepTimeUs = mIdleSleepTimeUs;
        }
    } else if (mBytesWritten != 0 || (mMixerStatus == MIXER_TRACKS_ENABLED)) {
        // clear out mMixerBuffer or mSinkBuffer, to ensure buffers are cleared
        // before effects processing or output.
        if (mMixerBufferValid) {
            memset(mMixerBuffer, 0, mMixerBufferSize);
            if (mType == SPATIALIZER) {
                memset(mSinkBuffer, 0, mSinkBufferSize);
            }
        } else {
            memset(mSinkBuffer, 0, mSinkBufferSize);
        }
        mSleepTimeUs = 0;
        ALOGV_IF(mBytesWritten == 0 && (mMixerStatus == MIXER_TRACKS_ENABLED),
                "anticipated start");
    }
    // TODO add standby time extension fct of effect tail
}

// prepareTracks_l() must be called with ThreadBase::mutex() held
PlaybackThread::mixer_state MixerThread::prepareTracks_l(
        Vector<sp<IAfTrack>>* tracksToRemove)
{
    // clean up deleted track ids in AudioMixer before allocating new tracks
    (void)mTracks.processDeletedTrackIds([this](int trackId) {
        // for each trackId, destroy it in the AudioMixer
        if (mAudioMixer->exists(trackId)) {
            mAudioMixer->destroy(trackId);
        }
    });
    mTracks.clearDeletedTrackIds();

    mixer_state mixerStatus = MIXER_IDLE;
    // find out which tracks need to be processed
    size_t count = mActiveTracks.size();
    size_t mixedTracks = 0;
    size_t tracksWithEffect = 0;
    // counts only _active_ fast tracks
    size_t fastTracks = 0;
    uint32_t resetMask = 0; // bit mask of fast tracks that need to be reset

    float masterVolume = mMasterVolume;
    bool masterMute = mMasterMute;

    if (masterMute) {
        masterVolume = 0;
    }
    // Delegate master volume control to effect in output mix effect chain if needed
    sp<IAfEffectChain> chain = getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX);
    if (chain != 0) {
        uint32_t v = (uint32_t)(masterVolume * (1 << 24));
        chain->setVolume(&v, &v);
        masterVolume = (float)((v + (1 << 23)) >> 24);
        chain.clear();
    }

    // prepare a new state to push
    FastMixerStateQueue *sq = NULL;
    FastMixerState *state = NULL;
    bool didModify = false;
    FastMixerStateQueue::block_t block = FastMixerStateQueue::BLOCK_UNTIL_PUSHED;
    bool coldIdle = false;
    if (mFastMixer != 0) {
        sq = mFastMixer->sq();
        state = sq->begin();
        coldIdle = state->mCommand == FastMixerState::COLD_IDLE;
    }

    mMixerBufferValid = false;  // mMixerBuffer has no valid data until appropriate tracks found.
    mEffectBufferValid = false; // mEffectBuffer has no valid data until tracks found.

    // DeferredOperations handles statistics after setting mixerStatus.
    class DeferredOperations {
    public:
        DeferredOperations(mixer_state *mixerStatus, ThreadMetrics *threadMetrics)
            : mMixerStatus(mixerStatus)
            , mThreadMetrics(threadMetrics) {}

        // when leaving scope, tally frames properly.
        ~DeferredOperations() {
            // Tally underrun frames only if we are actually mixing (MIXER_TRACKS_READY)
            // because that is when the underrun occurs.
            // We do not distinguish between FastTracks and NormalTracks here.
            size_t maxUnderrunFrames = 0;
            if (*mMixerStatus == MIXER_TRACKS_READY && mUnderrunFrames.size() > 0) {
                for (const auto &underrun : mUnderrunFrames) {
                    underrun.first->tallyUnderrunFrames(underrun.second);
                    maxUnderrunFrames = max(underrun.second, maxUnderrunFrames);
                }
            }
            // send the max underrun frames for this mixer period
            mThreadMetrics->logUnderrunFrames(maxUnderrunFrames);
        }

        // tallyUnderrunFrames() is called to update the track counters
        // with the number of underrun frames for a particular mixer period.
        // We defer tallying until we know the final mixer status.
        void tallyUnderrunFrames(const sp<IAfTrack>& track, size_t underrunFrames) {
            mUnderrunFrames.emplace_back(track, underrunFrames);
        }

    private:
        const mixer_state * const mMixerStatus;
        ThreadMetrics * const mThreadMetrics;
        std::vector<std::pair<sp<IAfTrack>, size_t>> mUnderrunFrames;
    } deferredOperations(&mixerStatus, &mThreadMetrics);
    // implicit nested scope for variable capture

    bool noFastHapticTrack = true;
    for (size_t i=0 ; i<count ; i++) {
        const sp<IAfTrack> t = mActiveTracks[i];

        // this const just means the local variable doesn't change
        IAfTrack* const track = t.get();

        // process fast tracks
        if (track->isFastTrack()) {
            LOG_ALWAYS_FATAL_IF(mFastMixer.get() == nullptr,
                    "%s(%d): FastTrack(%d) present without FastMixer",
                     __func__, id(), track->id());

            if (track->getHapticPlaybackEnabled()) {
                noFastHapticTrack = false;
            }

            // It's theoretically possible (though unlikely) for a fast track to be created
            // and then removed within the same normal mix cycle.  This is not a problem, as
            // the track never becomes active so it's fast mixer slot is never touched.
            // The converse, of removing an (active) track and then creating a new track
            // at the identical fast mixer slot within the same normal mix cycle,
            // is impossible because the slot isn't marked available until the end of each cycle.
            int j = track->fastIndex();
            ALOG_ASSERT(0 < j && j < (int)FastMixerState::sMaxFastTracks);
            ALOG_ASSERT(!(mFastTrackAvailMask & (1 << j)));
            FastTrack *fastTrack = &state->mFastTracks[j];

            // Determine whether the track is currently in underrun condition,
            // and whether it had a recent underrun.
            FastTrackDump *ftDump = &mFastMixerDumpState.mTracks[j];
            FastTrackUnderruns underruns = ftDump->mUnderruns;
            uint32_t recentFull = (underruns.mBitFields.mFull -
                    track->fastTrackUnderruns().mBitFields.mFull) & UNDERRUN_MASK;
            uint32_t recentPartial = (underruns.mBitFields.mPartial -
                    track->fastTrackUnderruns().mBitFields.mPartial) & UNDERRUN_MASK;
            uint32_t recentEmpty = (underruns.mBitFields.mEmpty -
                    track->fastTrackUnderruns().mBitFields.mEmpty) & UNDERRUN_MASK;
            uint32_t recentUnderruns = recentPartial + recentEmpty;
            track->fastTrackUnderruns() = underruns;
            // don't count underruns that occur while stopping or pausing
            // or stopped which can occur when flush() is called while active
            size_t underrunFrames = 0;
            if (!(track->isStopping() || track->isPausing()
                    || track->isStopped() || track->isPaused())
                && recentUnderruns > 0) {
                // FIXME fast mixer will pull & mix partial buffers, but we count as a full underrun
                underrunFrames = recentUnderruns * mFrameCount;
            }
            // Immediately account for FastTrack underruns.
            track->audioTrackServerProxy()->tallyUnderrunFrames(underrunFrames);

            // This is similar to the state machine for normal tracks,
            // with a few modifications for fast tracks.
            bool isActive = true;
            switch (track->state()) {
            case IAfTrackBase::STOPPING_1:
                // track stays active in STOPPING_1 state until first underrun
                if (recentUnderruns > 0 || track->isTerminated()) {
                    track->setState(IAfTrackBase::STOPPING_2);
                }
                break;
            case IAfTrackBase::PAUSING:
                // ramp down is not yet implemented
                track->setPaused();
                break;
            case IAfTrackBase::RESUMING:
                // ramp up is not yet implemented
                track->setState(IAfTrackBase::ACTIVE);
                break;
            case IAfTrackBase::ACTIVE:
                if (recentFull > 0 || recentPartial > 0) {
                    // track has provided at least some frames recently: reset retry count
                    track->retryCount() = kMaxTrackRetries;
                }
                if (recentUnderruns == 0) {
                    // no recent underruns: stay active
                    break;
                }
                // there has recently been an underrun of some kind
                if (track->sharedBuffer() == 0) {
                    // were any of the recent underruns "empty" (no frames available)?
                    if (recentEmpty == 0) {
                        // no, then ignore the partial underruns as they are allowed indefinitely
                        break;
                    }
                    // there has recently been an "empty" underrun: decrement the retry counter
                    if (--(track->retryCount()) > 0) {
                        break;
                    }
                    // indicate to client process that the track was disabled because of underrun;
                    // it will then automatically call start() when data is available
                    track->disable();
                    // remove from active list, but state remains ACTIVE [confusing but true]
                    isActive = false;
                    break;
                }
                FALLTHROUGH_INTENDED;
            case IAfTrackBase::STOPPING_2:
            case IAfTrackBase::PAUSED:
            case IAfTrackBase::STOPPED:
            case IAfTrackBase::FLUSHED:   // flush() while active
                // Check for presentation complete if track is inactive
                // We have consumed all the buffers of this track.
                // This would be incomplete if we auto-paused on underrun
                {
                    uint32_t latency = 0;
                    status_t result = mOutput->stream->getLatency(&latency);
                    ALOGE_IF(result != OK,
                            "Error when retrieving output stream latency: %d", result);
                    size_t audioHALFrames = (latency * mSampleRate) / 1000;
                    int64_t framesWritten = mBytesWritten / mFrameSize;
                    if (!(mStandby || track->presentationComplete(framesWritten, audioHALFrames))) {
                        // track stays in active list until presentation is complete
                        break;
                    }
                }
                if (track->isStopping_2()) {
                    track->setState(IAfTrackBase::STOPPED);
                }
                if (track->isStopped()) {
                    // Can't reset directly, as fast mixer is still polling this track
                    //   track->reset();
                    // So instead mark this track as needing to be reset after push with ack
                    resetMask |= 1 << i;
                }
                isActive = false;
                break;
            case IAfTrackBase::IDLE:
            default:
                LOG_ALWAYS_FATAL("unexpected track state %d", (int)track->state());
            }

            if (isActive) {
                // was it previously inactive?
                if (!(state->mTrackMask & (1 << j))) {
                    ExtendedAudioBufferProvider *eabp = track->asExtendedAudioBufferProvider();
                    VolumeProvider *vp = track->asVolumeProvider();
                    fastTrack->mBufferProvider = eabp;
                    fastTrack->mVolumeProvider = vp;
                    fastTrack->mChannelMask = track->channelMask();
                    fastTrack->mFormat = track->format();
                    fastTrack->mHapticPlaybackEnabled = track->getHapticPlaybackEnabled();
                    fastTrack->mHapticScale = track->getHapticScale();
                    fastTrack->mHapticMaxAmplitude = track->getHapticMaxAmplitude();
                    fastTrack->mGeneration++;
                    snprintf(fastTrack->mTraceName, sizeof(fastTrack->mTraceName),
                             "%s%s", AUDIO_TRACE_PREFIX_AUDIO_TRACK_FRDY,
                             track->getTraceSuffix().c_str());
                    state->mTrackMask |= 1 << j;
                    didModify = true;
                    // no acknowledgement required for newly active tracks
                }
                sp<AudioTrackServerProxy> proxy = track->audioTrackServerProxy();
                float volume;
                if (!audioserver_flags::portid_volume_management()) {
                    if (track->isPlaybackRestricted() || mStreamTypes[track->streamType()].mute) {
                        volume = 0.f;
                    } else {
                        volume = masterVolume * mStreamTypes[track->streamType()].volume;
                    }
                } else {
                    if (track->isPlaybackRestricted() || track->getPortMute()) {
                        volume = 0.f;
                    } else {
                        volume = masterVolume * track->getPortVolume();
                    }
                }
                const auto amn = mAfThreadCallback->getAudioManagerNative();
                if (amn) {
                    track->maybeLogPlaybackHardening(*amn);
                }
                handleVoipVolume_l(&volume);

                // cache the combined master volume and stream type volume for fast mixer; this
                // lacks any synchronization or barrier so VolumeProvider may read a stale value
                const float vh = track->getVolumeHandler()->getVolume(
                    proxy->framesReleased()).first;
                volume *= vh;
                track->setCachedVolume(volume);
                gain_minifloat_packed_t vlr = proxy->getVolumeLR();
                float vlf = float_from_gain(gain_minifloat_unpack_left(vlr));
                float vrf = float_from_gain(gain_minifloat_unpack_right(vlr));
                if (amn) {
                    if (!audioserver_flags::portid_volume_management()) {
                        track->processMuteEvent(*amn,
                                /*muteState=*/{masterVolume == 0.f,
                                               mStreamTypes[track->streamType()].volume == 0.f,
                                               mStreamTypes[track->streamType()].mute,
                                               track->isPlaybackRestrictedOp(),
                                               vlf == 0.f && vrf == 0.f,
                                               vh == 0.f,
                                               /*muteFromPortVolume=*/false,
                                               track->isPlaybackRestrictedControl()});
                    } else {
                        track->processMuteEvent(*amn,
                                /*muteState=*/{masterVolume == 0.f,
                                               track->getPortVolume() == 0.f,
                                               /* muteFromStreamMuted= */ false,
                                               track->isPlaybackRestrictedOp(),
                                               vlf == 0.f && vrf == 0.f,
                                               vh == 0.f,
                                           track->getPortMute(),
                                           track->isPlaybackRestrictedControl()});
                    }
                }
                vlf *= volume;
                vrf *= volume;

                if (track->getInternalMute()) {
                    vlf = 0.f;
                    vrf = 0.f;
                }

                track->setFinalVolume(vlf, vrf);
                ++fastTracks;
            } else {
                // was it previously active?
                if (state->mTrackMask & (1 << j)) {
                    fastTrack->mBufferProvider = NULL;
                    fastTrack->mGeneration++;
                    state->mTrackMask &= ~(1 << j);
                    didModify = true;
                    // If any fast tracks were removed, we must wait for acknowledgement
                    // because we're about to decrement the last sp<> on those tracks.
                    block = FastMixerStateQueue::BLOCK_UNTIL_ACKED;
                } else {
                    // ALOGW rather than LOG_ALWAYS_FATAL because it seems there are cases where an
                    // AudioTrack may start (which may not be with a start() but with a write()
                    // after underrun) and immediately paused or released.  In that case the
                    // FastTrack state hasn't had time to update.
                    // TODO Remove the ALOGW when this theory is confirmed.
                    ALOGW("fast track %d should have been active; "
                            "mState=%d, mTrackMask=%#x, recentUnderruns=%u, isShared=%d",
                            j, (int)track->state(), state->mTrackMask, recentUnderruns,
                            track->sharedBuffer() != 0);
                    // Since the FastMixer state already has the track inactive, do nothing here.
                }
                tracksToRemove->add(track);
                // Avoids a misleading display in dumpsys
                track->fastTrackUnderruns().mBitFields.mMostRecent = UNDERRUN_FULL;
            }
            if (fastTrack->mHapticPlaybackEnabled != track->getHapticPlaybackEnabled()) {
                fastTrack->mHapticPlaybackEnabled = track->getHapticPlaybackEnabled();
                didModify = true;
            }
            continue;
        }

        {   // local variable scope to avoid goto warning

        audio_track_cblk_t* cblk = track->cblk();

        // The first time a track is added we wait
        // for all its buffers to be filled before processing it
        const int trackId = track->id();

        // if an active track doesn't exist in the AudioMixer, create it.
        // use the trackId as the AudioMixer name.
        if (!mAudioMixer->exists(trackId)) {
            status_t status = mAudioMixer->create(
                    trackId,
                    track->channelMask(),
                    track->format(),
                    track->sessionId());
            if (status != OK) {
                ALOGW("%s(): AudioMixer cannot create track(%d)"
                        " mask %#x, format %#x, sessionId %d",
                        __func__, trackId,
                        track->channelMask(), track->format(), track->sessionId());
                tracksToRemove->add(track);
                track->invalidate(); // consider it dead.
                continue;
            }
        }

        // make sure that we have enough frames to mix one full buffer.
        // enforce this condition only once to enable draining the buffer in case the client
        // app does not call stop() and relies on underrun to stop:
        // hence the test on (mMixerStatus == MIXER_TRACKS_READY) meaning the track was mixed
        // during last round
        size_t desiredFrames;
        const uint32_t sampleRate = track->audioTrackServerProxy()->getSampleRate();
        const AudioPlaybackRate playbackRate = track->audioTrackServerProxy()->getPlaybackRate();

        desiredFrames = sourceFramesNeededWithTimestretch(
                sampleRate, mNormalFrameCount, mSampleRate, playbackRate.mSpeed);
        // TODO: ONLY USED FOR LEGACY RESAMPLERS, remove when they are removed.
        // add frames already consumed but not yet released by the resampler
        // because mAudioTrackServerProxy->framesReady() will include these frames
        desiredFrames += mAudioMixer->getUnreleasedFrames(trackId);

        uint32_t minFrames = 1;
        if ((track->sharedBuffer() == 0) && !track->isStopped() && !track->isPausing() &&
                (mMixerStatusIgnoringFastTracks == MIXER_TRACKS_READY)) {
            minFrames = desiredFrames;
        }

        size_t framesReady = track->framesReady();
        if (ATRACE_ENABLED()) [[unlikely]] {
            ATRACE_INT(std::string(AUDIO_TRACE_PREFIX_AUDIO_TRACK_NRDY)
                    .append(track->getTraceSuffix()).c_str(), framesReady);
        }
        if ((framesReady >= minFrames) && track->isReady() &&
                !track->isPaused() && !track->isTerminated())
        {
            ALOGVV("track(%d) s=%08x [OK] on thread %p", trackId, cblk->mServer, this);

            mixedTracks++;

            // track->mainBuffer() != mSinkBuffer and mMixerBuffer means
            // there is an effect chain connected to the track
            chain.clear();
            if (track->mainBuffer() != mSinkBuffer &&
                    track->mainBuffer() != mMixerBuffer) {
                if (mEffectBufferEnabled) {
                    mEffectBufferValid = true; // Later can set directly.
                }
                chain = getEffectChain_l(track->sessionId());
                // Delegate volume control to effect in track effect chain if needed
                if (chain != 0) {
                    tracksWithEffect++;
                } else {
                    ALOGW("prepareTracks_l(): track(%d) attached to effect but no chain found on "
                            "session %d",
                            trackId, track->sessionId());
                }
            }


            int param = AudioMixer::VOLUME;
            if (track->fillingStatus() == IAfTrack::FS_FILLED) {
                // no ramp for the first volume setting
                track->fillingStatus() = IAfTrack::FS_ACTIVE;
                if (track->state() == IAfTrackBase::RESUMING) {
                    track->setState(IAfTrackBase::ACTIVE);
                    // If a new track is paused immediately after start, do not ramp on resume.
                    if (cblk->mServer != 0) {
                        param = AudioMixer::RAMP_VOLUME;
                    }
                }
                mAudioMixer->setParameter(trackId, AudioMixer::RESAMPLE, AudioMixer::RESET, NULL);
                mLeftVolFloat = -1.0;
            // FIXME should not make a decision based on mServer
            } else if (cblk->mServer != 0) {
                // If the track is stopped before the first frame was mixed,
                // do not apply ramp
                param = AudioMixer::RAMP_VOLUME;
            }

            // compute volume for this track
            uint32_t vl, vr;       // in U8.24 integer format
            float vlf, vrf, vaf;   // in [0.0, 1.0] float format
            // read original volumes with volume control
            // Always fetch volumeshaper volume to ensure state is updated.
            const sp<AudioTrackServerProxy> proxy = track->audioTrackServerProxy();
            const float vh = track->getVolumeHandler()->getVolume(
                    track->audioTrackServerProxy()->framesReleased()).first;
            float v;
            if (!audioserver_flags::portid_volume_management()) {
                v = masterVolume * mStreamTypes[track->streamType()].volume;
                if (mStreamTypes[track->streamType()].mute || track->isPlaybackRestricted()) {
                    v = 0;
                }
            } else {
                v = masterVolume * track->getPortVolume();
                if (track->isPlaybackRestricted() || track->getPortMute()) {
                    v = 0;
                }
            }

            handleVoipVolume_l(&v);
            const auto amn = mAfThreadCallback->getAudioManagerNative();
            if (amn) {
                track->maybeLogPlaybackHardening(*amn);
            }

            if (track->isPausing()) {
                vl = vr = 0;
                vlf = vrf = vaf = 0.;
                track->setPaused();
            } else {
                gain_minifloat_packed_t vlr = proxy->getVolumeLR();
                vlf = float_from_gain(gain_minifloat_unpack_left(vlr));
                vrf = float_from_gain(gain_minifloat_unpack_right(vlr));
                // track volumes come from shared memory, so can't be trusted and must be clamped
                if (vlf > GAIN_FLOAT_UNITY) {
                    ALOGV("Track left volume out of range: %.3g", vlf);
                    vlf = GAIN_FLOAT_UNITY;
                }
                if (vrf > GAIN_FLOAT_UNITY) {
                    ALOGV("Track right volume out of range: %.3g", vrf);
                    vrf = GAIN_FLOAT_UNITY;
                }
                if (amn) {
                    if (!audioserver_flags::portid_volume_management()) {
                        track->processMuteEvent(*amn,
                                /*muteState=*/{masterVolume == 0.f,
                                               mStreamTypes[track->streamType()].volume == 0.f,
                                               mStreamTypes[track->streamType()].mute,
                                               track->isPlaybackRestrictedOp(),
                                               vlf == 0.f && vrf == 0.f,
                                               vh == 0.f,
                                               /*muteFromPortVolume=*/false,
                                               track->isPlaybackRestrictedControl()});
                    } else {
                        track->processMuteEvent(*amn,
                                /*muteState=*/{masterVolume == 0.f,
                                               track->getPortVolume() == 0.f,
                                               /* muteFromStreamMuted= */ false,
                                               track->isPlaybackRestrictedOp(),
                                               vlf == 0.f && vrf == 0.f,
                                               vh == 0.f,
                                               track->getPortMute(),
                                               track->isPlaybackRestrictedControl()});
                    }
                }
                // now apply the master volume and stream type volume and shaper volume
                vlf *= v * vh;
                vrf *= v * vh;
                // assuming master volume and stream type volume each go up to 1.0,
                // then derive vl and vr as U8.24 versions for the effect chain
                const float scaleto8_24 = MAX_GAIN_INT * MAX_GAIN_INT;
                vl = (uint32_t) (scaleto8_24 * vlf);
                vr = (uint32_t) (scaleto8_24 * vrf);
                // vl and vr are now in U8.24 format
                uint16_t sendLevel = proxy->getSendLevel_U4_12();
                // send level comes from shared memory and so may be corrupt
                if (sendLevel > MAX_GAIN_INT) {
                    ALOGV("Track send level out of range: %04X", sendLevel);
                    sendLevel = MAX_GAIN_INT;
                }
                // vaf is represented as [0.0, 1.0] float by rescaling sendLevel
                vaf = v * sendLevel * (1. / MAX_GAIN_INT);
            }

            if (track->getInternalMute()) {
                vrf = 0.f;
                vlf = 0.f;
            }

            track->setFinalVolume(vlf, vrf);

            // Delegate volume control to effect in track effect chain if needed
            if (chain != 0 && chain->setVolume(&vl, &vr)) {
                // Do not ramp volume if volume is controlled by effect
                param = AudioMixer::VOLUME;
                // Update remaining floating point volume levels
                vlf = (float)vl / (1 << 24);
                vrf = (float)vr / (1 << 24);
                track->setHasVolumeController(true);
            } else {
                // force no volume ramp when volume controller was just disabled or removed
                // from effect chain to avoid volume spike
                if (track->hasVolumeController()) {
                    param = AudioMixer::VOLUME;
                }
                track->setHasVolumeController(false);
            }

            // XXX: these things DON'T need to be done each time
            mAudioMixer->setBufferProvider(trackId, track->asExtendedAudioBufferProvider());
            mAudioMixer->enable(trackId);

            mAudioMixer->setParameter(trackId, param, AudioMixer::VOLUME0, &vlf);
            mAudioMixer->setParameter(trackId, param, AudioMixer::VOLUME1, &vrf);
            mAudioMixer->setParameter(trackId, param, AudioMixer::AUXLEVEL, &vaf);
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::FORMAT, (void *)track->format());
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::CHANNEL_MASK, (void *)(uintptr_t)track->channelMask());

            if (mType == SPATIALIZER && !track->isSpatialized()) {
                mAudioMixer->setParameter(
                    trackId,
                    AudioMixer::TRACK,
                    AudioMixer::MIXER_CHANNEL_MASK,
                    (void *)(uintptr_t)(mChannelMask | mHapticChannelMask));
            } else {
                mAudioMixer->setParameter(
                    trackId,
                    AudioMixer::TRACK,
                    AudioMixer::MIXER_CHANNEL_MASK,
                    (void *)(uintptr_t)(mMixerChannelMask | mHapticChannelMask));
            }

            // limit track sample rate to 2 x output sample rate, which changes at re-configuration
            uint32_t maxSampleRate = mSampleRate * AUDIO_RESAMPLER_DOWN_RATIO_MAX;
            uint32_t reqSampleRate = proxy->getSampleRate();
            if (reqSampleRate == 0) {
                reqSampleRate = mSampleRate;
            } else if (reqSampleRate > maxSampleRate) {
                reqSampleRate = maxSampleRate;
            }
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::RESAMPLE,
                AudioMixer::SAMPLE_RATE,
                (void *)(uintptr_t)reqSampleRate);

            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TIMESTRETCH,
                AudioMixer::PLAYBACK_RATE,
                // cast away constness for this generic API.
                const_cast<void *>(reinterpret_cast<const void *>(&playbackRate)));

            /*
             * Select the appropriate output buffer for the track.
             *
             * Tracks with effects go into their own effects chain buffer
             * and from there into either mEffectBuffer or mSinkBuffer.
             *
             * Other tracks can use mMixerBuffer for higher precision
             * channel accumulation.  If this buffer is enabled
             * (mMixerBufferEnabled true), then selected tracks will accumulate
             * into it.
             *
             */
            if (mMixerBufferEnabled
                    && (track->mainBuffer() == mSinkBuffer
                            || track->mainBuffer() == mMixerBuffer)) {
                if (mType == SPATIALIZER && !track->isSpatialized()) {
                    mAudioMixer->setParameter(
                            trackId,
                            AudioMixer::TRACK,
                            AudioMixer::MIXER_FORMAT, (void *)mEffectBufferFormat);
                    mAudioMixer->setParameter(
                            trackId,
                            AudioMixer::TRACK,
                            AudioMixer::MAIN_BUFFER, (void *)mPostSpatializerBuffer);
                } else {
                    mAudioMixer->setParameter(
                            trackId,
                            AudioMixer::TRACK,
                            AudioMixer::MIXER_FORMAT, (void *)mMixerBufferFormat);
                    mAudioMixer->setParameter(
                            trackId,
                            AudioMixer::TRACK,
                            AudioMixer::MAIN_BUFFER, (void *)mMixerBuffer);
                    // TODO: override track->mainBuffer()?
                    mMixerBufferValid = true;
                }
            } else {
                mAudioMixer->setParameter(
                        trackId,
                        AudioMixer::TRACK,
                        AudioMixer::MIXER_FORMAT, (void *)AUDIO_FORMAT_PCM_FLOAT);
                mAudioMixer->setParameter(
                        trackId,
                        AudioMixer::TRACK,
                        AudioMixer::MAIN_BUFFER, (void *)track->mainBuffer());
            }
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::AUX_BUFFER, (void *)track->auxBuffer());
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::HAPTIC_ENABLED, (void *)(uintptr_t)track->getHapticPlaybackEnabled());
            const os::HapticScale hapticScale = track->getHapticScale();
            mAudioMixer->setParameter(
                    trackId,
                    AudioMixer::TRACK,
                    AudioMixer::HAPTIC_SCALE, (void *)&hapticScale);
            const float hapticMaxAmplitude = track->getHapticMaxAmplitude();
            mAudioMixer->setParameter(
                trackId,
                AudioMixer::TRACK,
                AudioMixer::HAPTIC_MAX_AMPLITUDE, (void *)&hapticMaxAmplitude);

            // reset retry count
            track->retryCount() = kMaxTrackRetries;

            // If one track is ready, set the mixer ready if:
            //  - the mixer was not ready during previous round OR
            //  - no other track is not ready
            if (mMixerStatusIgnoringFastTracks != MIXER_TRACKS_READY ||
                    mixerStatus != MIXER_TRACKS_ENABLED) {
                mixerStatus = MIXER_TRACKS_READY;
            }

            // Enable the next few lines to instrument a test for underrun log handling.
            // TODO: Remove when we have a better way of testing the underrun log.
#if 0
            static int i;
            if ((++i & 0xf) == 0) {
                deferredOperations.tallyUnderrunFrames(track, 10 /* underrunFrames */);
            }
#endif
        } else {
            size_t underrunFrames = 0;
            if (framesReady < desiredFrames && !track->isStopped() && !track->isPaused()) {
                ALOGV("track(%d) underrun, track state %s  framesReady(%zu) < framesDesired(%zd)",
                        trackId, track->getTrackStateAsString(), framesReady, desiredFrames);
                underrunFrames = desiredFrames;
            }
            deferredOperations.tallyUnderrunFrames(track, underrunFrames);

            // clear effect chain input buffer if an active track underruns to avoid sending
            // previous audio buffer again to effects
            chain = getEffectChain_l(track->sessionId());
            if (chain != 0) {
                chain->clearInputBuffer();
            }

            ALOGVV("track(%d) s=%08x [NOT READY] on thread %p", trackId, cblk->mServer, this);
            if ((track->sharedBuffer() != 0) || track->isTerminated() ||
                    track->isStopped() || track->isPaused()) {
                // We have consumed all the buffers of this track.
                // Remove it from the list of active tracks.
                // TODO: use actual buffer filling status instead of latency when available from
                // audio HAL
                size_t audioHALFrames = (latency_l() * mSampleRate) / 1000;
                int64_t framesWritten = mBytesWritten / mFrameSize;
                if (mStandby || track->presentationComplete(framesWritten, audioHALFrames)) {
                    if (track->isStopped()) {
                        track->reset();
                    }
                    tracksToRemove->add(track);
                }
            } else {
                // No buffers for this track. Give it a few chances to
                // fill a buffer, then remove it from active list.
                if (--(track->retryCount()) <= 0) {
                    ALOGI("%s BUFFER TIMEOUT: remove track(%d) from active list due to underrun"
                          " on thread %d", __func__, trackId, mId);
                    tracksToRemove->add(track);
                    // indicate to client process that the track was disabled because of underrun;
                    // it will then automatically call start() when data is available
                    track->disable();
                // If one track is not ready, mark the mixer also not ready if:
                //  - the mixer was ready during previous round OR
                //  - no other track is ready
                } else if (mMixerStatusIgnoringFastTracks == MIXER_TRACKS_READY ||
                                mixerStatus != MIXER_TRACKS_READY) {
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
            mAudioMixer->disable(trackId);
        }

        }   // local variable scope to avoid goto warning

    }

    if (mHapticChannelMask != AUDIO_CHANNEL_NONE && sq != NULL) {
        // When there is no fast track playing haptic and FastMixer exists,
        // enabling the first FastTrack, which provides mixed data from normal
        // tracks, to play haptic data.
        FastTrack *fastTrack = &state->mFastTracks[0];
        if (fastTrack->mHapticPlaybackEnabled != noFastHapticTrack) {
            fastTrack->mHapticPlaybackEnabled = noFastHapticTrack;
            didModify = true;
        }
    }

    // Push the new FastMixer state if necessary
    [[maybe_unused]] bool pauseAudioWatchdog = false;
    if (didModify) {
        state->mFastTracksGen++;
        // if the fast mixer was active, but now there are no fast tracks, then put it in cold idle
        if (kUseFastMixer == FastMixer_Dynamic &&
                state->mCommand == FastMixerState::MIX_WRITE && state->mTrackMask <= 1) {
            state->mCommand = FastMixerState::COLD_IDLE;
            state->mColdFutexAddr = &mFastMixerFutex;
            state->mColdGen++;
            mFastMixerFutex = 0;
            if (kUseFastMixer == FastMixer_Dynamic) {
                mNormalSink = mOutputSink;
            }
            // If we go into cold idle, need to wait for acknowledgement
            // so that fast mixer stops doing I/O.
            block = FastMixerStateQueue::BLOCK_UNTIL_ACKED;
            pauseAudioWatchdog = true;
        }
    }
    if (sq != NULL) {
        sq->end(didModify);
        // No need to block if the FastMixer is in COLD_IDLE as the FastThread
        // is not active. (We BLOCK_UNTIL_ACKED when entering COLD_IDLE
        // when bringing the output sink into standby.)
        //
        // We will get the latest FastMixer state when we come out of COLD_IDLE.
        //
        // This occurs with BT suspend when we idle the FastMixer with
        // active tracks, which may be added or removed.
        {
            audio_utils::mutex::scoped_queue_wait_check queueWaitCheck(mFastMixer->getTid());
            sq->push(coldIdle ? FastMixerStateQueue::BLOCK_NEVER : block);
        }
    }
#ifdef AUDIO_WATCHDOG
    if (pauseAudioWatchdog && mAudioWatchdog != 0) {
        mAudioWatchdog->pause();
    }
#endif

    // Now perform the deferred reset on fast tracks that have stopped
    while (resetMask != 0) {
        size_t i = __builtin_ctz(resetMask);
        ALOG_ASSERT(i < count);
        resetMask &= ~(1 << i);
        sp<IAfTrack> track = mActiveTracks[i];
        ALOG_ASSERT(track->isFastTrack() && track->isStopped());
        track->reset();
    }

    // Track destruction may occur outside of threadLoop once it is removed from active tracks.
    // Ensure the AudioMixer doesn't have a raw "buffer provider" pointer to the track if
    // it ceases to be active, to allow safe removal from the AudioMixer at the start
    // of prepareTracks_l(); this releases any outstanding buffer back to the track.
    // See also the implementation of destroyTrack_l().
    for (const auto &track : *tracksToRemove) {
        const int trackId = track->id();
        if (mAudioMixer->exists(trackId)) { // Normal tracks here, fast tracks in FastMixer.
            mAudioMixer->setBufferProvider(trackId, nullptr /* bufferProvider */);
        }
    }

    // remove all the tracks that need to be...
    removeTracks_l(*tracksToRemove);

    if (getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX) != 0 ||
            getEffectChain_l(AUDIO_SESSION_OUTPUT_STAGE) != 0) {
        mEffectBufferValid = true;
    }

    if (mEffectBufferValid) {
        // as long as there are effects we should clear the effects buffer, to avoid
        // passing a non-clean buffer to the effect chain
        memset(mEffectBuffer, 0, mEffectBufferSize);
        if (mType == SPATIALIZER) {
            memset(mPostSpatializerBuffer, 0, mPostSpatializerBufferSize);
        }
    }
    // sink or mix buffer must be cleared if all tracks are connected to an
    // effect chain as in this case the mixer will not write to the sink or mix buffer
    // and track effects will accumulate into it
    // always clear sink buffer for spatializer output as the output of the spatializer
    // effect will be accumulated into it
    if ((mBytesRemaining == 0) && (((mixedTracks != 0 && mixedTracks == tracksWithEffect) ||
            (mixedTracks == 0 && fastTracks > 0)) || (mType == SPATIALIZER))) {
        // FIXME as a performance optimization, should remember previous zero status
        if (mMixerBufferValid) {
            memset(mMixerBuffer, 0, mMixerBufferSize);
            // TODO: In testing, mSinkBuffer below need not be cleared because
            // the PlaybackThread::threadLoop() copies mMixerBuffer into mSinkBuffer
            // after mixing.
            //
            // To enforce this guarantee:
            // ((mixedTracks != 0 && mixedTracks == tracksWithEffect) ||
            // (mixedTracks == 0 && fastTracks > 0))
            // must imply MIXER_TRACKS_READY.
            // Later, we may clear buffers regardless, and skip much of this logic.
        }
        // FIXME as a performance optimization, should remember previous zero status
        memset(mSinkBuffer, 0, mNormalFrameCount * mFrameSize);
    }

    // if any fast tracks, then status is ready
    mMixerStatusIgnoringFastTracks = mixerStatus;
    if (fastTracks > 0) {
        mixerStatus = MIXER_TRACKS_READY;
    }
    return mixerStatus;
}

// trackCountForUid_l() must be called with ThreadBase::mutex() held
uint32_t PlaybackThread::trackCountForUid_l(uid_t uid) const
{
    uint32_t trackCount = 0;
    for (size_t i = 0; i < mTracks.size() ; i++) {
        if (mTracks[i]->uid() == uid) {
            trackCount++;
        }
    }
    return trackCount;
}

bool PlaybackThread::IsTimestampAdvancing::check(AudioStreamOut* output)
{
    // Check the timestamp to see if it's advancing once every 150ms. If we check too frequently, we
    // could falsely detect that the frame position has stalled due to underrun because we haven't
    // given the Audio HAL enough time to update.
    const nsecs_t nowNs = systemTime();
    if (nowNs - mPreviousNs < mMinimumTimeBetweenChecksNs) {
        return mLatchedValue;
    }
    mPreviousNs = nowNs;
    mLatchedValue = false;
    // Determine if the presentation position is still advancing.
    uint64_t position = 0;
    struct timespec unused;
    const status_t ret = output->getPresentationPosition(&position, &unused);
    if (ret == NO_ERROR) {
        if (position != mPreviousPosition) {
            mPreviousPosition = position;
            mLatchedValue = true;
        }
    }
    return mLatchedValue;
}

void PlaybackThread::IsTimestampAdvancing::clear()
{
    mLatchedValue = true;
    mPreviousPosition = 0;
    mPreviousNs = 0;
}

// isTrackAllowed_l() must be called with ThreadBase::mutex() held
bool MixerThread::isTrackAllowed_l(
        audio_channel_mask_t channelMask, audio_format_t format,
        audio_session_t sessionId, uid_t uid) const
{
    if (!PlaybackThread::isTrackAllowed_l(channelMask, format, sessionId, uid)) {
        return false;
    }
    // Check validity as we don't call AudioMixer::create() here.
    if (!mAudioMixer->isValidFormat(format)) {
        ALOGW("%s: invalid format: %#x", __func__, format);
        return false;
    }
    if (!mAudioMixer->isValidChannelMask(channelMask)) {
        ALOGW("%s: invalid channelMask: %#x", __func__, channelMask);
        return false;
    }
    return true;
}

// checkForNewParameter_l() must be called with ThreadBase::mutex() held
bool MixerThread::checkForNewParameter_l(const String8& keyValuePair,
                                                       status_t& status)
{
    bool reconfig = false;
    status = NO_ERROR;

    AutoPark<FastMixer> park(mFastMixer);

    AudioParameter param = AudioParameter(keyValuePair);
    int value;
    if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
        reconfig = true;
    }
    if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
        if (!isValidPcmSinkFormat(static_cast<audio_format_t>(value))) {
            status = BAD_VALUE;
        } else {
            // no need to save value, since it's constant
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
        if (!isValidPcmSinkChannelMask(static_cast<audio_channel_mask_t>(value))) {
            status = BAD_VALUE;
        } else {
            // no need to save value, since it's constant
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
        // do not accept frame count changes if tracks are open as the track buffer
        // size depends on frame count and correct behavior would not be guaranteed
        // if frame count is changed after track creation
        if (!mTracks.isEmpty()) {
            status = INVALID_OPERATION;
        } else {
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        LOG_FATAL("Should not set routing device in MixerThread");
    }

    if (status == NO_ERROR) {
        status = mOutput->stream->setParameters(keyValuePair);
        if (!mStandby && status == INVALID_OPERATION) {
            ALOGW("%s: setParameters failed with keyValuePair %s, entering standby",
                    __func__, keyValuePair.c_str());
            mOutput->standby();
            mThreadMetrics.logEndInterval();
            mThreadSnapshot.onEnd();
            setStandby_l();
            mBytesWritten = 0;
            status = mOutput->stream->setParameters(keyValuePair);
        }
        if (status == NO_ERROR && reconfig) {
            readOutputParameters_l();
            delete mAudioMixer;
            mAudioMixer = new AudioMixer(mNormalFrameCount, mSampleRate);
            for (const auto &track : mTracks) {
                const int trackId = track->id();
                const status_t createStatus = mAudioMixer->create(
                        trackId,
                        track->channelMask(),
                        track->format(),
                        track->sessionId());
                ALOGW_IF(createStatus != NO_ERROR,
                        "%s(): AudioMixer cannot create track(%d)"
                        " mask %#x, format %#x, sessionId %d",
                        __func__,
                        trackId, track->channelMask(), track->format(), track->sessionId());
            }
            sendIoConfigEvent_l(AUDIO_OUTPUT_CONFIG_CHANGED);
        }
    }

    return reconfig;
}


void MixerThread::dumpInternals_l(int fd, const Vector<String16>& args)
{
    PlaybackThread::dumpInternals_l(fd, args);
    dprintf(fd, "  Thread throttle time (msecs): %u\n", (uint32_t)mThreadThrottleTimeMs);
    dprintf(fd, "  AudioMixer tracks: %s\n", mAudioMixer->trackNames().c_str());
    dprintf(fd, "  Master mono: %s\n", mMasterMono ? "on" : "off");
    dprintf(fd, "  Master balance: %f (%s)\n", mMasterBalance.load(),
            (hasFastMixer() ? std::to_string(mFastMixer->getMasterBalance())
                            : mBalance.toString()).c_str());
    if (hasFastMixer()) {
        dprintf(fd, "  FastMixer thread %p tid=%d", mFastMixer.get(), mFastMixer->getTid());

        // Make a non-atomic copy of fast mixer dump state so it won't change underneath us
        // while we are dumping it.  It may be inconsistent, but it won't mutate!
        // This is a large object so we place it on the heap.
        // FIXME 25972958: Need an intelligent copy constructor that does not touch unused pages.
        const std::unique_ptr<FastMixerDumpState> copy =
                std::make_unique<FastMixerDumpState>(mFastMixerDumpState);
        copy->dump(fd);

#ifdef STATE_QUEUE_DUMP
        // Similar for state queue
        StateQueueObserverDump observerCopy = mStateQueueObserverDump;
        observerCopy.dump(fd);
        StateQueueMutatorDump mutatorCopy = mStateQueueMutatorDump;
        mutatorCopy.dump(fd);
#endif

#ifdef AUDIO_WATCHDOG
        if (mAudioWatchdog != 0) {
            // Make a non-atomic copy of audio watchdog dump so it won't change underneath us
            AudioWatchdogDump wdCopy = mAudioWatchdogDump;
            wdCopy.dump(fd);
        }
#endif

    } else {
        dprintf(fd, "  No FastMixer\n");
    }

     dprintf(fd, "Bluetooth latency modes are %senabled\n",
            mBluetoothLatencyModesEnabled ? "" : "not ");
     dprintf(fd, "HAL does %ssupport Bluetooth latency modes\n", mOutput != nullptr &&
             mOutput->audioHwDev->supportsBluetoothVariableLatency() ? "" : "not ");
     dprintf(fd, "Supported latency modes: %s\n", toString(mSupportedLatencyModes).c_str());
}

uint32_t MixerThread::idleSleepTimeUs() const
{
    return (uint32_t)(((mNormalFrameCount * 1000) / mSampleRate) * 1000) / 2;
}

uint32_t MixerThread::suspendSleepTimeUs() const
{
    return (uint32_t)(((mNormalFrameCount * 1000) / mSampleRate) * 1000);
}

void MixerThread::cacheParameters_l()
{
    PlaybackThread::cacheParameters_l();

    // FIXME: Relaxed timing because of a certain device that can't meet latency
    // Should be reduced to 2x after the vendor fixes the driver issue
    // increase threshold again due to low power audio mode. The way this warning
    // threshold is calculated and its usefulness should be reconsidered anyway.
    maxPeriod = seconds(mNormalFrameCount) / mSampleRate * 15;
}

void MixerThread::onHalLatencyModesChanged_l() {
    mAfThreadCallback->onSupportedLatencyModesChanged(mId, mSupportedLatencyModes);
}

void MixerThread::setHalLatencyMode_l() {
    // Only handle latency mode if:
    // - mBluetoothLatencyModesEnabled is true
    // - the HAL supports latency modes
    // - the selected device is Bluetooth LE or A2DP
    if (!mBluetoothLatencyModesEnabled.load() || mSupportedLatencyModes.empty()) {
        return;
    }
    if (mOutDeviceTypeAddrs.size() != 1
            || !(audio_is_a2dp_out_device(mOutDeviceTypeAddrs[0].mType)
                 || audio_is_ble_out_device(mOutDeviceTypeAddrs[0].mType))) {
        return;
    }

    audio_latency_mode_t latencyMode = AUDIO_LATENCY_MODE_FREE;
    if (mSupportedLatencyModes.size() == 1) {
        // If the HAL only support one latency mode currently, confirm the choice
        latencyMode = mSupportedLatencyModes[0];
    } else if (mSupportedLatencyModes.size() > 1) {
        // Request low latency if:
        // - At least one active track is either:
        //   - a fast track with gaming usage or
        //   - a track with acessibility usage
        for (const auto& track : mActiveTracks) {
            if ((track->isFastTrack() && track->attributes().usage == AUDIO_USAGE_GAME)
                    || track->attributes().usage == AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY) {
                latencyMode = AUDIO_LATENCY_MODE_LOW;
                break;
            }
        }
    }

    if (latencyMode != mSetLatencyMode) {
        status_t status = mOutput->stream->setLatencyMode(latencyMode);
        ALOGD("%s: thread(%d) setLatencyMode(%s) returned %d",
                __func__, mId, toString(latencyMode).c_str(), status);
        if (status == NO_ERROR) {
            mSetLatencyMode = latencyMode;
        }
    }
}

void MixerThread::updateHalSupportedLatencyModes_l() {

    if (mOutput == nullptr || mOutput->stream == nullptr) {
        return;
    }
    std::vector<audio_latency_mode_t> latencyModes;
    const status_t status = mOutput->stream->getRecommendedLatencyModes(&latencyModes);
    if (status != NO_ERROR) {
        latencyModes.clear();
    }
    if (latencyModes != mSupportedLatencyModes) {
        ALOGD("%s: thread(%d) status %d supported latency modes: %s",
            __func__, mId, status, toString(latencyModes).c_str());
        mSupportedLatencyModes.swap(latencyModes);
        sendHalLatencyModesChangedEvent_l();
    }
}

status_t MixerThread::getSupportedLatencyModes(
        std::vector<audio_latency_mode_t>* modes) {
    if (modes == nullptr) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    *modes = mSupportedLatencyModes;
    return NO_ERROR;
}

void MixerThread::onRecommendedLatencyModeChanged(
        std::vector<audio_latency_mode_t> modes) {
    audio_utils::lock_guard _l(mutex());
    if (modes != mSupportedLatencyModes) {
        ALOGD("%s: thread(%d) supported latency modes: %s",
            __func__, mId, toString(modes).c_str());
        mSupportedLatencyModes.swap(modes);
        sendHalLatencyModesChangedEvent_l();
    }
}

status_t MixerThread::setBluetoothVariableLatencyEnabled(bool enabled) {
    if (mOutput == nullptr || mOutput->audioHwDev == nullptr
            || !mOutput->audioHwDev->supportsBluetoothVariableLatency()) {
        return INVALID_OPERATION;
    }
    mBluetoothLatencyModesEnabled.store(enabled);
    return NO_ERROR;
}

// ----------------------------------------------------------------------------

/* static */
sp<IAfPlaybackThread> IAfPlaybackThread::createDirectOutputThread(
        const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output, audio_io_handle_t id, bool systemReady,
        const audio_offload_info_t& offloadInfo) {
    return sp<DirectOutputThread>::make(
            afThreadCallback, output, id, systemReady, offloadInfo);
}

DirectOutputThread::DirectOutputThread(const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output, audio_io_handle_t id, ThreadBase::type_t type, bool systemReady,
        const audio_offload_info_t& offloadInfo)
    :   PlaybackThread(afThreadCallback, output, id, type, systemReady)
    , mOffloadInfo(offloadInfo)
{
    setMasterBalance(afThreadCallback->getMasterBalance_l());
}

DirectOutputThread::~DirectOutputThread()
{
}

void DirectOutputThread::dumpInternals_l(int fd, const Vector<String16>& args)
{
    PlaybackThread::dumpInternals_l(fd, args);
    dprintf(fd, "  Master balance: %f  Left: %f  Right: %f\n",
            mMasterBalance.load(), mMasterBalanceLeft, mMasterBalanceRight);
}

void DirectOutputThread::setMasterBalance(float balance)
{
    audio_utils::lock_guard _l(mutex());
    if (mMasterBalance != balance) {
        mMasterBalance.store(balance);
        mBalance.computeStereoBalance(balance, &mMasterBalanceLeft, &mMasterBalanceRight);
        broadcast_l();
    }
}

void DirectOutputThread::processVolume_l(IAfTrack* track, bool lastTrack)
{
    float left, right;

    // Ensure volumeshaper state always advances even when muted.
    const sp<AudioTrackServerProxy> proxy = track->audioTrackServerProxy();

    const int64_t frames = mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL];
    const int64_t time = mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL];

    ALOGVV("%s: Direct/Offload bufferConsumed:%zu  timestamp frames:%lld  time:%lld",
            __func__, proxy->framesReleased(), (long long)frames, (long long)time);

    const int64_t volumeShaperFrames =
            mMonotonicFrameCounter.updateAndGetMonotonicFrameCount(frames, time);
    const auto [shaperVolume, shaperActive] =
            track->getVolumeHandler()->getVolume(volumeShaperFrames);
    mVolumeShaperActive = shaperActive;

    gain_minifloat_packed_t vlr = proxy->getVolumeLR();
    left = float_from_gain(gain_minifloat_unpack_left(vlr));
    right = float_from_gain(gain_minifloat_unpack_right(vlr));

    const bool clientVolumeMute = (left == 0.f && right == 0.f);

    const auto amn = mAfThreadCallback->getAudioManagerNative();
    if (!audioserver_flags::portid_volume_management()) {
        if (mMasterMute || mStreamTypes[track->streamType()].mute ||
            track->isPlaybackRestricted()) {
            left = right = 0;
        } else {
            float typeVolume = mStreamTypes[track->streamType()].volume;
            const float v = mMasterVolume * typeVolume * shaperVolume;

            if (left > GAIN_FLOAT_UNITY) {
                left = GAIN_FLOAT_UNITY;
            }
            if (right > GAIN_FLOAT_UNITY) {
                right = GAIN_FLOAT_UNITY;
            }
            left *= v;
            right *= v;
            if (mAfThreadCallback->getMode() != AUDIO_MODE_IN_COMMUNICATION
                || audio_channel_count_from_out_mask(mChannelMask) > 1) {
                left *= mMasterBalanceLeft; // DirectOutputThread balance applied as track volume
                right *= mMasterBalanceRight;
            }
        }
        if (amn) {
            track->processMuteEvent(*amn,
                    /*muteState=*/{mMasterMute,
                                   mStreamTypes[track->streamType()].volume == 0.f,
                                   mStreamTypes[track->streamType()].mute,
                                   track->isPlaybackRestrictedOp(),
                                   clientVolumeMute,
                                   shaperVolume == 0.f,
                                   /*muteFromPortVolume=*/false,
                                   track->isPlaybackRestrictedControl()});
        }
    } else {
        if (mMasterMute || track->isPlaybackRestricted()) {
            left = right = 0;
        } else {
            float typeVolume = track->getPortVolume();
            const float v = mMasterVolume * typeVolume * shaperVolume;

            if (left > GAIN_FLOAT_UNITY) {
                left = GAIN_FLOAT_UNITY;
            }
            if (right > GAIN_FLOAT_UNITY) {
                right = GAIN_FLOAT_UNITY;
            }
            left *= v;
            right *= v;
            if (mAfThreadCallback->getMode() != AUDIO_MODE_IN_COMMUNICATION
                || audio_channel_count_from_out_mask(mChannelMask) > 1) {
                left *= mMasterBalanceLeft; // DirectOutputThread balance applied as track volume
                right *= mMasterBalanceRight;
            }
        }
        if (amn) {
            track->processMuteEvent(*amn,
                    /*muteState=*/{mMasterMute,
                                   track->getPortVolume() == 0.f,
                                   /* muteFromStreamMuted= */ false,
                                   track->isPlaybackRestrictedOp(),
                                   clientVolumeMute,
                                   shaperVolume == 0.f,
                                   track->getPortMute(),
                                   track->isPlaybackRestrictedControl()});
        }
    }
    if (amn) {
        track->maybeLogPlaybackHardening(*amn);
    }
    if (lastTrack) {
        track->setFinalVolume(left, right);
        if (left != mLeftVolFloat || right != mRightVolFloat) {
            mLeftVolFloat = left;
            mRightVolFloat = right;

            // Delegate volume control to effect in track effect chain if needed
            // only one effect chain can be present on DirectOutputThread, so if
            // there is one, the track is connected to it
            if (!mEffectChains.isEmpty()) {
                // if effect chain exists, volume is handled by it.
                // Convert volumes from float to 8.24
                uint32_t vl = (uint32_t)(left * (1 << 24));
                uint32_t vr = (uint32_t)(right * (1 << 24));
                // Direct/Offload effect chains set output volume in setVolume().
                (void)mEffectChains[0]->setVolume(&vl, &vr);
            } else {
                // otherwise we directly set the volume.
                setVolumeForOutput_l(left, right);
            }
        }
    }
}

void DirectOutputThread::onAddNewTrack_l()
{
    sp<IAfTrack> previousTrack = mPreviousTrack.promote();
    sp<IAfTrack> latestTrack = mActiveTracks.getLatest();

    if (previousTrack != 0 && latestTrack != 0) {
        if (mType == DIRECT) {
            if (previousTrack.get() != latestTrack.get()) {
                mFlushPending = true;
            }
        } else /* mType == OFFLOAD */ {
            if (previousTrack->sessionId() != latestTrack->sessionId() ||
                previousTrack->isFlushPending()) {
                mFlushPending = true;
            }
        }
    } else if (previousTrack == 0) {
        // there could be an old track added back during track transition for direct
        // output, so always issues flush to flush data of the previous track if it
        // was already destroyed with HAL paused, then flush can resume the playback
        mFlushPending = true;
    }
    PlaybackThread::onAddNewTrack_l();
}

PlaybackThread::mixer_state DirectOutputThread::prepareTracks_l(
    Vector<sp<IAfTrack>>* tracksToRemove
)
{
    size_t count = mActiveTracks.size();
    mixer_state mixerStatus = MIXER_IDLE;
    bool doHwPause = false;
    bool doHwResume = false;

    // find out which tracks need to be processed
    for (const sp<IAfTrack>& t : mActiveTracks) {
        if (t->isInvalid()) {
            ALOGW("An invalidated track shouldn't be in active list");
            tracksToRemove->add(t);
            continue;
        }

        IAfTrack* const track = t.get();
#ifdef VERY_VERY_VERBOSE_LOGGING
        audio_track_cblk_t* cblk = track->cblk();
#endif
        // Only consider last track started for volume and mixer state control.
        // In theory an older track could underrun and restart after the new one starts
        // but as we only care about the transition phase between two tracks on a
        // direct output, it is not a problem to ignore the underrun case.
        sp<IAfTrack> l = mActiveTracks.getLatest();
        bool last = l.get() == track;

        if (track->isPausePending()) {
            track->pauseAck();
            // It is possible a track might have been flushed or stopped.
            // Other operations such as flush pending might occur on the next prepare.
            if (track->isPausing()) {
                track->setPaused();
            }
            // Always perform pause, as an immediate flush will change
            // the pause state to be no longer isPausing().
            if (mHwSupportsPause && last && !mHwPaused) {
                doHwPause = true;
                mHwPaused = true;
            }
        } else if (track->isFlushPending()) {
            track->flushAck();
            if (last) {
                mFlushPending = true;
            }
        } else if (track->isResumePending()) {
            track->resumeAck();
            if (last) {
                mLeftVolFloat = mRightVolFloat = -1.0;
                if (mHwPaused) {
                    doHwResume = true;
                    mHwPaused = false;
                }
            }
        }

        // The first time a track is added we wait
        // for all its buffers to be filled before processing it.
        // Allow draining the buffer in case the client
        // app does not call stop() and relies on underrun to stop:
        // hence the test on (track->retryCount() > 1).
        // If track->retryCount() <= 1 then track is about to be disabled, paused, removed,
        // so we accept any nonzero amount of data delivered by the AudioTrack (which will
        // reset the retry counter).
        // Do not use a high threshold for compressed audio.

        // target retry count that we will use is based on the time we wait for retries.
        const int32_t targetRetryCount = kMaxTrackRetriesDirectMs * 1000 / mActiveSleepTimeUs;
        // the retry threshold is when we accept any size for PCM data.  This is slightly
        // smaller than the retry count so we can push small bits of data without a glitch.
        const int32_t retryThreshold = targetRetryCount > 2 ? targetRetryCount - 1 : 1;
        uint32_t minFrames;
        if ((track->sharedBuffer() == 0) && !track->isStopping_1() && !track->isPausing()
            && (track->retryCount() > retryThreshold) && audio_has_proportional_frames(mFormat)) {
            minFrames = mNormalFrameCount;
        } else {
            minFrames = 1;
        }

        const size_t framesReady = track->framesReady();
        const int trackId = track->id();
        if (ATRACE_ENABLED()) [[unlikely]] {
            ATRACE_INT(std::string(AUDIO_TRACE_PREFIX_AUDIO_TRACK_NRDY)
                    .append(track->getTraceSuffix()).c_str(), framesReady);
        }
        if ((framesReady >= minFrames) && track->isReady() && !track->isPaused() &&
                !track->isStopping_2() && !track->isStopped())
        {
            ALOGVV("track(%d) s=%08x [OK]", trackId, cblk->mServer);

            if (track->fillingStatus() == IAfTrack::FS_FILLED) {
                track->fillingStatus() = IAfTrack::FS_ACTIVE;
                if (last) {
                    // make sure processVolume_l() will apply new volume even if 0
                    mLeftVolFloat = mRightVolFloat = -1.0;
                }
                if (!mHwSupportsPause) {
                    track->resumeAck();
                }
            }

            // compute volume for this track
            processVolume_l(track, last);
            if (last) {
                sp<IAfTrack> previousTrack = mPreviousTrack.promote();
                if (previousTrack != 0) {
                    if (track != previousTrack.get()) {
                        // Flush any data still being written from last track
                        mBytesRemaining = 0;
                        // Invalidate previous track to force a seek when resuming.
                        previousTrack->invalidate();
                    }
                }
                mPreviousTrack = track;

                // reset retry count
                track->retryCount() = targetRetryCount;
                mActiveTrack = t;
                mixerStatus = MIXER_TRACKS_READY;
                if (mHwPaused) {
                    doHwResume = true;
                    mHwPaused = false;
                }
            }
        } else {
            // clear effect chain input buffer if the last active track started underruns
            // to avoid sending previous audio buffer again to effects
            if (!mEffectChains.isEmpty() && last) {
                mEffectChains[0]->clearInputBuffer();
            }
            if (track->isStopping_1()) {
                track->setState(IAfTrackBase::STOPPING_2);
                if (last && mHwPaused) {
                     doHwResume = true;
                     mHwPaused = false;
                 }
            }
            if ((track->sharedBuffer() != 0) || track->isStopped() ||
                    track->isStopping_2() || track->isPaused()) {
                // We have consumed all the buffers of this track.
                // Remove it from the list of active tracks.
                bool presComplete = false;
                if (mStandby || !last ||
                        (presComplete = track->presentationComplete(latency_l())) ||
                        track->isPaused() || mHwPaused) {
                    if (presComplete) {
                        mOutput->presentationComplete();
                    }
                    if (track->isStopping_2()) {
                        track->setState(IAfTrackBase::STOPPED);
                    }
                    if (track->isStopped()) {
                        track->reset();
                    }
                    tracksToRemove->add(track);
                }
            } else {
                // No buffers for this track. Give it a few chances to
                // fill a buffer, then remove it from active list.
                // Only consider last track started for mixer state control
                bool isTimestampAdvancing = mIsTimestampAdvancing.check(mOutput);
                if (!isTunerStream()  // tuner streams remain active in underrun
                        && --(track->retryCount()) <= 0) {
                    if (isTimestampAdvancing) { // HAL is still playing audio, give us more time.
                        track->retryCount() = kMaxTrackRetriesOffload;
                    } else {
                        ALOGI("%s BUFFER TIMEOUT: remove track(%d) from active list due to"
                              " underrun on thread %d", __func__, trackId, mId);
                        tracksToRemove->add(track);
                        // indicate to client process that the track was disabled because of
                        // underrun; it will then automatically call start() when data is available
                        track->disable();
                        // only do hw pause when track is going to be removed due to BUFFER TIMEOUT.
                        // unlike mixerthread, HAL can be paused for direct output
                        ALOGW("pause because of UNDERRUN, framesReady = %zu,"
                                "minFrames = %u, mFormat = %#x",
                                framesReady, minFrames, mFormat);
                        if (last && mHwSupportsPause && !mHwPaused && !mStandby) {
                            doHwPause = true;
                            mHwPaused = true;
                        }
                    }
                } else if (last) {
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
        }
    }

    // if an active track did not command a flush, check for pending flush on stopped tracks
    if (!mFlushPending) {
        for (size_t i = 0; i < mTracks.size(); i++) {
            if (mTracks[i]->isFlushPending()) {
                mTracks[i]->flushAck();
                mFlushPending = true;
            }
        }
    }

    // make sure the pause/flush/resume sequence is executed in the right order.
    // If a flush is pending and a track is active but the HW is not paused, force a HW pause
    // before flush and then resume HW. This can happen in case of pause/flush/resume
    // if resume is received before pause is executed.
    if (mHwSupportsPause && !mStandby &&
            (doHwPause || (mFlushPending && !mHwPaused && (count != 0)))) {
        status_t result = mOutput->stream->pause();
        ALOGE_IF(result != OK, "Error when pausing output stream: %d", result);
        doHwResume = !doHwPause;  // resume if pause is due to flush.
    }
    if (mFlushPending) {
        flushHw_l();
    }
    if (mHwSupportsPause && !mStandby && doHwResume) {
        status_t result = mOutput->stream->resume();
        ALOGE_IF(result != OK, "Error when resuming output stream: %d", result);
    }
    // remove all the tracks that need to be...
    removeTracks_l(*tracksToRemove);

    return mixerStatus;
}

void DirectOutputThread::threadLoop_mix()
{
    size_t frameCount = mFrameCount;
    int8_t *curBuf = (int8_t *)mSinkBuffer;
    // output audio to hardware
    while (frameCount) {
        AudioBufferProvider::Buffer buffer;
        buffer.frameCount = frameCount;
        status_t status = mActiveTrack->getNextBuffer(&buffer);
        if (status != NO_ERROR || buffer.raw == NULL) {
            // no need to pad with 0 for compressed audio
            if (audio_has_proportional_frames(mFormat)) {
                memset(curBuf, 0, frameCount * mFrameSize);
            }
            break;
        }
        memcpy(curBuf, buffer.raw, buffer.frameCount * mFrameSize);
        frameCount -= buffer.frameCount;
        curBuf += buffer.frameCount * mFrameSize;
        mActiveTrack->releaseBuffer(&buffer);
    }
    mCurrentWriteLength = curBuf - (int8_t *)mSinkBuffer;
    mSleepTimeUs = 0;
    mStandbyTimeNs = systemTime() + mStandbyDelayNs;
    mActiveTrack.clear();
}

void DirectOutputThread::threadLoop_sleepTime()
{
    // do not write to HAL when paused
    if (mHwPaused || (usesHwAvSync() && mStandby)) {
        mSleepTimeUs = mIdleSleepTimeUs;
        return;
    }
    if (mMixerStatus == MIXER_TRACKS_ENABLED) {
        mSleepTimeUs = mActiveSleepTimeUs;
    } else {
        mSleepTimeUs = mIdleSleepTimeUs;
    }
    // Note: In S or later, we do not write zeroes for
    // linear or proportional PCM direct tracks in underrun.
}

void DirectOutputThread::threadLoop_exit()
{
    {
        audio_utils::lock_guard _l(mutex());
        for (size_t i = 0; i < mTracks.size(); i++) {
            if (mTracks[i]->isFlushPending()) {
                mTracks[i]->flushAck();
                mFlushPending = true;
            }
        }
        if (mFlushPending) {
            flushHw_l();
        }
    }
    PlaybackThread::threadLoop_exit();
}

// must be called with thread mutex locked
bool DirectOutputThread::shouldStandby_l()
{
    bool trackPaused = false;
    bool trackStopped = false;
    bool trackDisabled = false;

    // do not put the HAL in standby when paused. NuPlayer clear the offloaded AudioTrack
    // after a timeout and we will enter standby then.
    // On offload threads, do not enter standby if the main track is still underrunning.
    if (mTracks.size() > 0) {
        const auto& mainTrack = mTracks[mTracks.size() - 1];

        trackPaused = mainTrack->isPaused();
        trackStopped = mainTrack->isStopped() || mainTrack->state() == IAfTrackBase::IDLE;
        trackDisabled = (mType == OFFLOAD) && mainTrack->isDisabled();
    }

    return !mStandby && !(trackPaused || (mHwPaused && !trackStopped) || trackDisabled);
}

// checkForNewParameter_l() must be called with ThreadBase::mutex() held
bool DirectOutputThread::checkForNewParameter_l(const String8& keyValuePair,
                                                              status_t& status)
{
    bool reconfig = false;
    status = NO_ERROR;

    AudioParameter param = AudioParameter(keyValuePair);
    int value;
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        LOG_FATAL("Should not set routing device in DirectOutputThread");
    }
    if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
        // do not accept frame count changes if tracks are open as the track buffer
        // size depends on frame count and correct behavior would not be garantied
        // if frame count is changed after track creation
        if (!mTracks.isEmpty()) {
            status = INVALID_OPERATION;
        } else {
            reconfig = true;
        }
    }
    if (status == NO_ERROR) {
        status = mOutput->stream->setParameters(keyValuePair);
        if (!mStandby && status == INVALID_OPERATION) {
            mOutput->standby();
            if (!mStandby) {
                mThreadMetrics.logEndInterval();
                mThreadSnapshot.onEnd();
                setStandby_l();
            }
            mBytesWritten = 0;
            status = mOutput->stream->setParameters(keyValuePair);
        }
        if (status == NO_ERROR && reconfig) {
            readOutputParameters_l();
            sendIoConfigEvent_l(AUDIO_OUTPUT_CONFIG_CHANGED);
        }
    }

    return reconfig;
}

uint32_t DirectOutputThread::activeSleepTimeUs() const
{
    uint32_t time;
    if (audio_has_proportional_frames(mFormat) && mType != OFFLOAD) {
        time = PlaybackThread::activeSleepTimeUs();
    } else {
        time = kDirectMinSleepTimeUs;
    }
    return time;
}

uint32_t DirectOutputThread::idleSleepTimeUs() const
{
    uint32_t time;
    if (audio_has_proportional_frames(mFormat) && mType != OFFLOAD) {
        time = (uint32_t)(((mFrameCount * 1000) / mSampleRate) * 1000) / 2;
    } else {
        time = kDirectMinSleepTimeUs;
    }
    return time;
}

uint32_t DirectOutputThread::suspendSleepTimeUs() const
{
    uint32_t time;
    if (audio_has_proportional_frames(mFormat) && mType != OFFLOAD) {
        time = (uint32_t)(((mFrameCount * 1000) / mSampleRate) * 1000);
    } else {
        time = kDirectMinSleepTimeUs;
    }
    return time;
}

void DirectOutputThread::cacheParameters_l()
{
    PlaybackThread::cacheParameters_l();

    // use shorter standby delay as on normal output to release
    // hardware resources as soon as possible
    // no delay on outputs with HW A/V sync
    if (usesHwAvSync()) {
        mStandbyDelayNs = 0;
    } else if (mType == OFFLOAD) {
        mStandbyDelayNs = kOffloadStandbyDelayNs;
    } else {
        mStandbyDelayNs = microseconds(mActiveSleepTimeUs*2);
    }
}

void DirectOutputThread::flushHw_l()
{
    PlaybackThread::flushHw_l();
    mOutput->flush();
    mFlushPending = false;
    mTimestampVerifier.discontinuity(discontinuityForStandbyOrFlush());
    mTimestamp.clear();
    mMonotonicFrameCounter.onFlush();
    // We do not reset mHwPaused which is hidden from the Track client.
    // Note: the client track in Tracks.cpp and AudioTrack.cpp
    // has a FLUSHED state but the DirectOutputThread does not;
    // those tracks will continue to show isStopped().
}

int64_t DirectOutputThread::computeWaitTimeNs_l() const {
    // If a VolumeShaper is active, we must wake up periodically to update volume.
    const int64_t NS_PER_MS = 1000000;
    return mVolumeShaperActive ?
            kMinNormalSinkBufferSizeMs * NS_PER_MS : PlaybackThread::computeWaitTimeNs_l();
}

// ----------------------------------------------------------------------------

AsyncCallbackThread::AsyncCallbackThread(
        const wp<PlaybackThread>& playbackThread)
    :   Thread(false /*canCallJava*/),
        mPlaybackThread(playbackThread),
        mWriteAckSequence(0),
        mDrainSequence(0),
        mAsyncError(ASYNC_ERROR_NONE)
{
}

void AsyncCallbackThread::onFirstRef()
{
    run("Offload Cbk", ANDROID_PRIORITY_URGENT_AUDIO);
}

bool AsyncCallbackThread::threadLoop()
{
    while (!exitPending()) {
        uint32_t writeAckSequence;
        uint32_t drainSequence;
        AsyncError asyncError;

        {
            audio_utils::unique_lock _l(mutex());
            while (!((mWriteAckSequence & 1) ||
                     (mDrainSequence & 1) ||
                     mAsyncError ||
                     exitPending())) {
                mWaitWorkCV.wait(_l);
            }

            if (exitPending()) {
                break;
            }
            ALOGV("AsyncCallbackThread mWriteAckSequence %d mDrainSequence %d",
                  mWriteAckSequence, mDrainSequence);
            writeAckSequence = mWriteAckSequence;
            mWriteAckSequence &= ~1;
            drainSequence = mDrainSequence;
            mDrainSequence &= ~1;
            asyncError = mAsyncError;
            mAsyncError = ASYNC_ERROR_NONE;
        }
        {
            const sp<PlaybackThread> playbackThread = mPlaybackThread.promote();
            if (playbackThread != 0) {
                if (writeAckSequence & 1) {
                    playbackThread->resetWriteBlocked(writeAckSequence >> 1);
                }
                if (drainSequence & 1) {
                    playbackThread->resetDraining(drainSequence >> 1);
                }
                if (asyncError != ASYNC_ERROR_NONE) {
                    playbackThread->onAsyncError(asyncError == ASYNC_ERROR_HARD);
                }
            }
        }
    }
    return false;
}

void AsyncCallbackThread::exit()
{
    ALOGV("AsyncCallbackThread::exit");
    audio_utils::lock_guard _l(mutex());
    requestExit();
    mWaitWorkCV.notify_all();
}

void AsyncCallbackThread::setWriteBlocked(uint32_t sequence)
{
    audio_utils::lock_guard _l(mutex());
    // bit 0 is cleared
    mWriteAckSequence = sequence << 1;
}

void AsyncCallbackThread::resetWriteBlocked()
{
    audio_utils::lock_guard _l(mutex());
    // ignore unexpected callbacks
    if (mWriteAckSequence & 2) {
        mWriteAckSequence |= 1;
        mWaitWorkCV.notify_one();
    }
}

void AsyncCallbackThread::setDraining(uint32_t sequence)
{
    audio_utils::lock_guard _l(mutex());
    // bit 0 is cleared
    mDrainSequence = sequence << 1;
}

void AsyncCallbackThread::resetDraining()
{
    audio_utils::lock_guard _l(mutex());
    // ignore unexpected callbacks
    if (mDrainSequence & 2) {
        mDrainSequence |= 1;
        mWaitWorkCV.notify_one();
    }
}

void AsyncCallbackThread::setAsyncError(bool isHardError)
{
    audio_utils::lock_guard _l(mutex());
    mAsyncError = isHardError ? ASYNC_ERROR_HARD : ASYNC_ERROR_SOFT;
    mWaitWorkCV.notify_one();
}


// ----------------------------------------------------------------------------

/* static */
sp<IAfPlaybackThread> IAfPlaybackThread::createOffloadThread(
        const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output, audio_io_handle_t id, bool systemReady,
        const audio_offload_info_t& offloadInfo) {
    return sp<OffloadThread>::make(afThreadCallback, output, id, systemReady, offloadInfo);
}

OffloadThread::OffloadThread(const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output, audio_io_handle_t id, bool systemReady,
        const audio_offload_info_t& offloadInfo)
    :   DirectOutputThread(afThreadCallback, output, id, OFFLOAD, systemReady, offloadInfo),
        mPausedWriteLength(0), mPausedBytesRemaining(0), mKeepWakeLock(true)
{
    //FIXME: mStandby should be set to true by ThreadBase constructo
    mStandby = true;
    mKeepWakeLock = property_get_bool("ro.audio.offload_wakelock", true /* default_value */);
}

void OffloadThread::threadLoop_exit()
{
    if (mFlushPending || mHwPaused) {
        // If a flush is pending or track was paused, just discard buffered data
        audio_utils::lock_guard l(mutex());
        flushHw_l();
    } else {
        mMixerStatus = MIXER_DRAIN_ALL;
        threadLoop_drain();
    }
    if (mUseAsyncWrite) {
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->exit();
    }
    PlaybackThread::threadLoop_exit();
}

PlaybackThread::mixer_state OffloadThread::prepareTracks_l(
    Vector<sp<IAfTrack>>* tracksToRemove
)
{
    size_t count = mActiveTracks.size();

    mixer_state mixerStatus = MIXER_IDLE;
    bool doHwPause = false;
    bool doHwResume = false;

    ALOGV("OffloadThread::prepareTracks_l active tracks %zu", count);

    // find out which tracks need to be processed
    for (const sp<IAfTrack>& t : mActiveTracks) {
        IAfTrack* const track = t.get();
#ifdef VERY_VERY_VERBOSE_LOGGING
        audio_track_cblk_t* cblk = track->cblk();
#endif
        // Only consider last track started for volume and mixer state control.
        // In theory an older track could underrun and restart after the new one starts
        // but as we only care about the transition phase between two tracks on a
        // direct output, it is not a problem to ignore the underrun case.
        sp<IAfTrack> l = mActiveTracks.getLatest();
        bool last = l.get() == track;

        if (track->isInvalid()) {
            ALOGW("An invalidated track shouldn't be in active list");
            tracksToRemove->add(track);
            continue;
        }

        if (track->state() == IAfTrackBase::IDLE) {
            ALOGW("An idle track shouldn't be in active list");
            continue;
        }

        const size_t framesReady = track->framesReady();
        if (ATRACE_ENABLED()) [[unlikely]] {
            ATRACE_INT(std::string(AUDIO_TRACE_PREFIX_AUDIO_TRACK_NRDY)
                    .append(track->getTraceSuffix()).c_str(), framesReady);
        }
        if (track->isPausePending()) {
            track->pauseAck();
            // It is possible a track might have been flushed or stopped.
            // Other operations such as flush pending might occur on the next prepare.
            if (track->isPausing()) {
                track->setPaused();
            }
            // Always perform pause if last, as an immediate flush will change
            // the pause state to be no longer isPausing().
            if (last) {
                if (mHwSupportsPause && !mHwPaused) {
                    doHwPause = true;
                    mHwPaused = true;
                }
                // If we were part way through writing the mixbuffer to
                // the HAL we must save this until we resume
                // BUG - this will be wrong if a different track is made active,
                // in that case we want to discard the pending data in the
                // mixbuffer and tell the client to present it again when the
                // track is resumed
                mPausedWriteLength = mCurrentWriteLength;
                mPausedBytesRemaining = mBytesRemaining;
                mBytesRemaining = 0;    // stop writing
            }
            tracksToRemove->add(track);
        } else if (track->isFlushPending()) {
            if (track->isStopping_1()) {
                track->retryCount() = kMaxTrackStopRetriesOffload;
            } else {
                track->retryCount() = kMaxTrackRetriesOffload;
            }
            track->flushAck();
            if (last) {
                mFlushPending = true;
            }
        } else if (track->isResumePending()){
            track->resumeAck();
            if (last) {
                if (mPausedBytesRemaining) {
                    // Need to continue write that was interrupted
                    mCurrentWriteLength = mPausedWriteLength;
                    mBytesRemaining = mPausedBytesRemaining;
                    mPausedBytesRemaining = 0;
                }
                if (mHwPaused) {
                    doHwResume = true;
                    mHwPaused = false;
                    // threadLoop_mix() will handle the case that we need to
                    // resume an interrupted write
                }
                // enable write to audio HAL
                mSleepTimeUs = 0;

                mLeftVolFloat = mRightVolFloat = -1.0;

                // Do not handle new data in this iteration even if track->framesReady()
                mixerStatus = MIXER_TRACKS_ENABLED;
            }
        } else if (framesReady && track->isReady() &&
                !track->isPaused() && !track->isTerminated() && !track->isStopping_2()) {
            ALOGVV("OffloadThread: track(%d) s=%08x [OK]", track->id(), cblk->mServer);
            if (track->fillingStatus() == IAfTrack::FS_FILLED) {
                track->fillingStatus() = IAfTrack::FS_ACTIVE;
                if (last) {
                    // make sure processVolume_l() will apply new volume even if 0
                    mLeftVolFloat = mRightVolFloat = -1.0;
                }
            }

            if (last) {
                sp<IAfTrack> previousTrack = mPreviousTrack.promote();
                if (previousTrack != 0) {
                    if (track != previousTrack.get()) {
                        // Flush any data still being written from last track
                        mBytesRemaining = 0;
                        if (mPausedBytesRemaining) {
                            // Last track was paused so we also need to flush saved
                            // mixbuffer state and invalidate track so that it will
                            // re-submit that unwritten data when it is next resumed
                            mPausedBytesRemaining = 0;
                            // Invalidate is a bit drastic - would be more efficient
                            // to have a flag to tell client that some of the
                            // previously written data was lost
                            previousTrack->invalidate();
                        }
                        // flush data already sent to the DSP if changing audio session as audio
                        // comes from a different source. Also invalidate previous track to force a
                        // seek when resuming.
                        if (previousTrack->sessionId() != track->sessionId()) {
                            previousTrack->invalidate();
                        }
                    }
                }
                mPreviousTrack = track;
                // reset retry count
                if (track->isStopping_1()) {
                    track->retryCount() = kMaxTrackStopRetriesOffload;
                } else {
                    track->retryCount() = kMaxTrackRetriesOffload;
                }
                mActiveTrack = t;
                mixerStatus = MIXER_TRACKS_READY;
            }
        } else {
            ALOGVV("OffloadThread: track(%d) s=%08x [NOT READY]", track->id(), cblk->mServer);
            if (track->isStopping_1()) {
                if (--(track->retryCount()) <= 0) {
                    // Hardware buffer can hold a large amount of audio so we must
                    // wait for all current track's data to drain before we say
                    // that the track is stopped.
                    if (mBytesRemaining == 0) {
                        // Only start draining when all data in mixbuffer
                        // has been written
                        ALOGV("OffloadThread: underrun and STOPPING_1 -> draining, STOPPING_2");
                        track->setState(IAfTrackBase::STOPPING_2);
                        // so presentation completes after
                        // drain do not drain if no data was ever sent to HAL (mStandby == true)
                        if (last && !mStandby) {
                            // do not modify drain sequence if we are already draining. This happens
                            // when resuming from pause after drain.
                            if ((mDrainSequence & 1) == 0) {
                                mSleepTimeUs = 0;
                                mStandbyTimeNs = systemTime() + mStandbyDelayNs;
                                mixerStatus = MIXER_DRAIN_TRACK;
                                mDrainSequence += 2;
                            }
                            if (mHwPaused) {
                                // It is possible to move from PAUSED to STOPPING_1 without
                                // a resume so we must ensure hardware is running
                                doHwResume = true;
                                mHwPaused = false;
                            }
                        }
                    }
                } else if (last) {
                    ALOGV("stopping1 underrun retries left %d", track->retryCount());
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            } else if (track->isStopping_2()) {
                // Drain has completed or we are in standby, signal presentation complete
                if (!(mDrainSequence & 1) || !last || mStandby) {
                    track->setState(IAfTrackBase::STOPPED);
                    mOutput->presentationComplete();
                    track->presentationComplete(latency_l()); // always returns true
                    track->reset();
                    tracksToRemove->add(track);
                    // OFFLOADED stop resets frame counts.
                    if (!mUseAsyncWrite) {
                        // If we don't get explicit drain notification we must
                        // register discontinuity regardless of whether this is
                        // the previous (!last) or the upcoming (last) track
                        // to avoid skipping the discontinuity.
                        mTimestampVerifier.discontinuity(
                                mTimestampVerifier.DISCONTINUITY_MODE_ZERO);
                    }
                }
            } else {
                // No buffers for this track. Give it a few chances to
                // fill a buffer, then remove it from active list.
                bool isTimestampAdvancing = mIsTimestampAdvancing.check(mOutput);
                if (!isTunerStream()  // tuner streams remain active in underrun
                        && --(track->retryCount()) <= 0) {
                    if (isTimestampAdvancing) { // HAL is still playing audio, give us more time.
                        track->retryCount() = kMaxTrackRetriesOffload;
                    } else {
                        ALOGI("%s BUFFER TIMEOUT: remove track(%d) from active list due to"
                              " underrun on thread %d", __func__, track->id(), mId);
                        tracksToRemove->add(track);
                        // tell client process that the track was disabled because of underrun;
                        // it will then automatically call start() when data is available
                        track->disable();
                    }
                } else if (last){
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
        }
        // compute volume for this track
        if (track->isReady()) {  // check ready to prevent premature start.
            processVolume_l(track, last);
        }
    }

    // make sure the pause/flush/resume sequence is executed in the right order.
    // If a flush is pending and a track is active but the HW is not paused, force a HW pause
    // before flush and then resume HW. This can happen in case of pause/flush/resume
    // if resume is received before pause is executed.
    if (!mStandby && (doHwPause || (mFlushPending && !mHwPaused && (count != 0)))) {
        status_t result = mOutput->stream->pause();
        ALOGE_IF(result != OK, "Error when pausing output stream: %d", result);
        doHwResume = !doHwPause;  // resume if pause is due to flush.
    }
    if (mFlushPending) {
        flushHw_l();
    }
    if (!mStandby && doHwResume) {
        status_t result = mOutput->stream->resume();
        ALOGE_IF(result != OK, "Error when resuming output stream: %d", result);
    }

    // remove all the tracks that need to be...
    removeTracks_l(*tracksToRemove);

    return mixerStatus;
}

// must be called with thread mutex locked
bool OffloadThread::waitingAsyncCallback_l()
{
    ALOGVV("waitingAsyncCallback_l mWriteAckSequence %d mDrainSequence %d",
          mWriteAckSequence, mDrainSequence);
    if (mUseAsyncWrite && ((mWriteAckSequence & 1) || (mDrainSequence & 1))) {
        return true;
    }
    return false;
}

bool OffloadThread::waitingAsyncCallback()
{
    audio_utils::lock_guard _l(mutex());
    return waitingAsyncCallback_l();
}

void OffloadThread::flushHw_l()
{
    DirectOutputThread::flushHw_l();
    // Flush anything still waiting in the mixbuffer
    mCurrentWriteLength = 0;
    mBytesRemaining = 0;
    mPausedWriteLength = 0;
    mPausedBytesRemaining = 0;
    // reset bytes written count to reflect that DSP buffers are empty after flush.
    mBytesWritten = 0;

    if (mUseAsyncWrite) {
        // discard any pending drain or write ack by incrementing sequence
        mWriteAckSequence = (mWriteAckSequence + 2) & ~1;
        mDrainSequence = (mDrainSequence + 2) & ~1;
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->setWriteBlocked(mWriteAckSequence);
        mCallbackThread->setDraining(mDrainSequence);
    }
}

void OffloadThread::invalidateTracks(audio_stream_type_t streamType)
{
    audio_utils::lock_guard _l(mutex());
    if (PlaybackThread::invalidateTracks_l(streamType)) {
        mFlushPending = true;
    }
}

void OffloadThread::invalidateTracks(std::set<audio_port_handle_t>& portIds) {
    audio_utils::lock_guard _l(mutex());
    if (PlaybackThread::invalidateTracks_l(portIds)) {
        mFlushPending = true;
    }
}

// ----------------------------------------------------------------------------

/* static */
sp<IAfDuplicatingThread> IAfDuplicatingThread::create(
        const sp<IAfThreadCallback>& afThreadCallback,
        IAfPlaybackThread* mainThread, audio_io_handle_t id, bool systemReady) {
    return sp<DuplicatingThread>::make(afThreadCallback, mainThread, id, systemReady);
}

DuplicatingThread::DuplicatingThread(const sp<IAfThreadCallback>& afThreadCallback,
       IAfPlaybackThread* mainThread, audio_io_handle_t id, bool systemReady)
    :   MixerThread(afThreadCallback, mainThread->getOutput(), id,
                    systemReady, DUPLICATING),
        mWaitTimeMs(UINT_MAX)
{
    addOutputTrack(mainThread);
}

DuplicatingThread::~DuplicatingThread()
{
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        mOutputTracks[i]->destroy();
    }
}

void DuplicatingThread::threadLoop_mix()
{
    // mix buffers...
    if (outputsReady()) {
        mAudioMixer->process();
    } else {
        if (mMixerBufferValid) {
            memset(mMixerBuffer, 0, mMixerBufferSize);
        } else {
            memset(mSinkBuffer, 0, mSinkBufferSize);
        }
    }
    mSleepTimeUs = 0;
    writeFrames = mNormalFrameCount;
    mCurrentWriteLength = mSinkBufferSize;
    mStandbyTimeNs = systemTime() + mStandbyDelayNs;
}

void DuplicatingThread::threadLoop_sleepTime()
{
    if (mSleepTimeUs == 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            mSleepTimeUs = mActiveSleepTimeUs;
        } else {
            mSleepTimeUs = mIdleSleepTimeUs;
        }
    } else if (mBytesWritten != 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            writeFrames = mNormalFrameCount;
            memset(mSinkBuffer, 0, mSinkBufferSize);
        } else {
            // flush remaining overflow buffers in output tracks
            writeFrames = 0;
        }
        mSleepTimeUs = 0;
    }
}

ssize_t DuplicatingThread::threadLoop_write()
{
    ATRACE_BEGIN("write");
    for (size_t i = 0; i < outputTracks.size(); i++) {
        const ssize_t actualWritten = outputTracks[i]->write(mSinkBuffer, writeFrames);

        // Consider the first OutputTrack for timestamp and frame counting.

        // The threadLoop() generally assumes writing a full sink buffer size at a time.
        // Here, we correct for writeFrames of 0 (a stop) or underruns because
        // we always claim success.
        if (i == 0) {
            const ssize_t correction = mSinkBufferSize / mFrameSize - actualWritten;
            ALOGD_IF(correction != 0 && writeFrames != 0,
                    "%s: writeFrames:%u  actualWritten:%zd  correction:%zd  mFramesWritten:%lld",
                    __func__, writeFrames, actualWritten, correction, (long long)mFramesWritten);
            mFramesWritten -= correction;
        }

        // TODO: Report correction for the other output tracks and show in the dump.
    }
    ATRACE_END();
    if (mStandby) {
        mThreadMetrics.logBeginInterval();
        mThreadSnapshot.onBegin();
        mStandby = false;
    }
    return (ssize_t)mSinkBufferSize;
}

void DuplicatingThread::threadLoop_standby()
{
    // DuplicatingThread implements standby by stopping all tracks
    for (size_t i = 0; i < outputTracks.size(); i++) {
        outputTracks[i]->stop();
    }
}

void DuplicatingThread::threadLoop_exit()
{
    // Prevent calling the OutputTrack dtor in the DuplicatingThread dtor
    // where other mutexes (i.e. AudioPolicyService_Mutex) may be held.
    // Do so here in the threadLoop_exit().

    SortedVector <sp<IAfOutputTrack>> localTracks;
    {
        audio_utils::lock_guard l(mutex());
        localTracks = std::move(mOutputTracks);
        mOutputTracks.clear();
        for (size_t i = 0; i < localTracks.size(); ++i) {
            localTracks[i]->destroy();
        }
    }
    localTracks.clear();
    outputTracks.clear();
    PlaybackThread::threadLoop_exit();
}

void DuplicatingThread::dumpInternals_l(int fd, const Vector<String16>& args)
{
    MixerThread::dumpInternals_l(fd, args);

    std::stringstream ss;
    const size_t numTracks = mOutputTracks.size();
    ss << "  " << numTracks << " OutputTracks";
    if (numTracks > 0) {
        ss << ":";
        for (const auto &track : mOutputTracks) {
            const auto thread = track->thread().promote();
            ss << " (" << track->id() << " : ";
            if (thread.get() != nullptr) {
                ss << thread.get() << ", " << thread->id();
            } else {
                ss << "null";
            }
            ss << ")";
        }
    }
    ss << "\n";
    std::string result = ss.str();
    write(fd, result.c_str(), result.size());
}

void DuplicatingThread::saveOutputTracks()
{
    outputTracks = mOutputTracks;
}

void DuplicatingThread::clearOutputTracks()
{
    outputTracks.clear();
}

void DuplicatingThread::addOutputTrack(IAfPlaybackThread* thread)
{
    audio_utils::lock_guard _l(mutex());
    // The downstream MixerThread consumes thread->frameCount() amount of frames per mix pass.
    // Adjust for thread->sampleRate() to determine minimum buffer frame count.
    // Then triple buffer because Threads do not run synchronously and may not be clock locked.
    const size_t frameCount =
            3 * sourceFramesNeeded(mSampleRate, thread->frameCount(), thread->sampleRate());
    // TODO: Consider asynchronous sample rate conversion to handle clock disparity
    // from different OutputTracks and their associated MixerThreads (e.g. one may
    // nearly empty and the other may be dropping data).

    // TODO b/182392769: use attribution source util, move to server edge
    AttributionSourceState attributionSource = AttributionSourceState();
    attributionSource.uid = VALUE_OR_FATAL(legacy2aidl_uid_t_int32_t(
        IPCThreadState::self()->getCallingUid()));
    attributionSource.pid = VALUE_OR_FATAL(legacy2aidl_pid_t_int32_t(
      IPCThreadState::self()->getCallingPid()));
    attributionSource.token = sp<BBinder>::make();
    sp<IAfOutputTrack> outputTrack = IAfOutputTrack::create(thread,
                                            this,
                                            mSampleRate,
                                            mFormat,
                                            mChannelMask,
                                            frameCount,
                                            attributionSource);
    status_t status = outputTrack != 0 ? outputTrack->initCheck() : (status_t) NO_MEMORY;
    if (status != NO_ERROR) {
        ALOGE("addOutputTrack() initCheck failed %d", status);
        return;
    }
    if (!audioserver_flags::portid_volume_management()) {
        thread->setStreamVolume(AUDIO_STREAM_PATCH, /*volume=*/1.0f, /*muted=*/false);
    }

    mOutputTracks.add(outputTrack);
    ALOGV("addOutputTrack() track %p, on thread %p", outputTrack.get(), thread);
    updateWaitTime_l();
}

void DuplicatingThread::removeOutputTrack(IAfPlaybackThread* thread)
{
    audio_utils::lock_guard _l(mutex());
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        if (mOutputTracks[i]->thread() == thread) {
            mOutputTracks[i]->destroy();
            mOutputTracks.removeAt(i);
            updateWaitTime_l();
            // NO_THREAD_SAFETY_ANALYSIS
            // Lambda workaround: as thread != this
            // we can safely call the remote thread getOutput.
            const bool equalOutput =
                    [&](){ return thread->getOutput() == mOutput; }();
            if (equalOutput) {
                mOutput = nullptr;
            }
            return;
        }
    }
    ALOGV("removeOutputTrack(): unknown thread: %p", thread);
}

// caller must hold mutex()
void DuplicatingThread::updateWaitTime_l()
{
    // Initialize mWaitTimeMs according to the mixer buffer size.
    mWaitTimeMs = mNormalFrameCount * 2 * 1000 / mSampleRate;
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        const auto strong = mOutputTracks[i]->thread().promote();
        if (strong != 0) {
            uint32_t waitTimeMs = (strong->frameCount() * 2 * 1000) / strong->sampleRate();
            if (waitTimeMs < mWaitTimeMs) {
                mWaitTimeMs = waitTimeMs;
            }
        }
    }
}

bool DuplicatingThread::outputsReady()
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        const auto thread = outputTracks[i]->thread().promote();
        if (thread == 0) {
            ALOGW("DuplicatingThread::outputsReady() could not promote thread on output track %p",
                    outputTracks[i].get());
            return false;
        }
        IAfPlaybackThread* const playbackThread = thread->asIAfPlaybackThread().get();
        // see note at standby() declaration
        if (playbackThread->inStandby() && !playbackThread->isSuspended()) {
            ALOGV("DuplicatingThread output track %p on thread %p Not Ready", outputTracks[i].get(),
                    thread.get());
            return false;
        }
    }
    return true;
}

void DuplicatingThread::sendMetadataToBackend_l(
        const StreamOutHalInterface::SourceMetadata& metadata)
{
    for (auto& outputTrack : outputTracks) { // not mOutputTracks
        outputTrack->setMetadatas(metadata.tracks);
    }
}

uint32_t DuplicatingThread::activeSleepTimeUs() const
{
    // return half the wait time in microseconds.
    return std::min(mWaitTimeMs * 500ULL, (unsigned long long)UINT32_MAX);  // prevent overflow.
}

void DuplicatingThread::cacheParameters_l()
{
    // updateWaitTime_l() sets mWaitTimeMs, which affects activeSleepTimeUs(), so call it first
    updateWaitTime_l();

    MixerThread::cacheParameters_l();
}

// ----------------------------------------------------------------------------

/* static */
sp<IAfPlaybackThread> IAfPlaybackThread::createSpatializerThread(
        const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output,
        audio_io_handle_t id,
        bool systemReady,
        audio_config_base_t* mixerConfig) {
    return sp<SpatializerThread>::make(afThreadCallback, output, id, systemReady, mixerConfig);
}

SpatializerThread::SpatializerThread(const sp<IAfThreadCallback>& afThreadCallback,
                                                             AudioStreamOut* output,
                                                             audio_io_handle_t id,
                                                             bool systemReady,
                                                             audio_config_base_t *mixerConfig)
    : MixerThread(afThreadCallback, output, id, systemReady, SPATIALIZER, mixerConfig)
{
}

void SpatializerThread::setHalLatencyMode_l() {
    // if mSupportedLatencyModes is empty, the HAL stream does not support
    // latency mode control and we can exit.
    if (mSupportedLatencyModes.empty()) {
        return;
    }
    // Do not update the HAL latency mode if no track is active
    if (mActiveTracks.isEmpty()) {
        return;
    }

    audio_latency_mode_t latencyMode = AUDIO_LATENCY_MODE_FREE;
    if (mSupportedLatencyModes.size() == 1) {
        // If the HAL only support one latency mode currently, confirm the choice
        latencyMode = mSupportedLatencyModes[0];
    } else if (mSupportedLatencyModes.size() > 1) {
        // Request low latency if:
        // - The low latency mode is requested by the spatializer controller
        //   (mRequestedLatencyMode = AUDIO_LATENCY_MODE_LOW)
        //      AND
        // - At least one active track is spatialized
        for (const auto& track : mActiveTracks) {
            if (track->isSpatialized()) {
                latencyMode = mRequestedLatencyMode;
                break;
            }
        }
    }

    if (latencyMode != mSetLatencyMode) {
        status_t status = mOutput->stream->setLatencyMode(latencyMode);
        ALOGD("%s: thread(%d) setLatencyMode(%s) returned %d",
                __func__, mId, toString(latencyMode).c_str(), status);
        if (status == NO_ERROR) {
            mSetLatencyMode = latencyMode;
        }
    }
}

status_t SpatializerThread::setRequestedLatencyMode(audio_latency_mode_t mode) {
    if (mode < 0 || mode >= AUDIO_LATENCY_MODE_CNT) {
        return BAD_VALUE;
    }
    audio_utils::lock_guard _l(mutex());
    mRequestedLatencyMode = mode;
    return NO_ERROR;
}

void SpatializerThread::checkOutputStageEffects()
NO_THREAD_SAFETY_ANALYSIS
//  'createEffect_l' requires holding mutex 'AudioFlinger_Mutex' exclusively
{
    bool hasVirtualizer = false;
    bool hasDownMixer = false;
    sp<IAfEffectHandle> finalDownMixer;
    {
        audio_utils::lock_guard _l(mutex());
        sp<IAfEffectChain> chain = getEffectChain_l(AUDIO_SESSION_OUTPUT_STAGE);
        if (chain != 0) {
            hasVirtualizer = chain->getEffectFromType_l(FX_IID_SPATIALIZER) != nullptr;
            hasDownMixer = chain->getEffectFromType_l(EFFECT_UIID_DOWNMIX) != nullptr;
        }

        finalDownMixer = mFinalDownMixer;
        mFinalDownMixer.clear();
    }

    if (hasVirtualizer) {
        if (finalDownMixer != nullptr) {
            int32_t ret;
            finalDownMixer->asIEffect()->disable(&ret);
        }
        finalDownMixer.clear();
    } else if (!hasDownMixer) {
        std::vector<effect_descriptor_t> descriptors;
        status_t status = mAfThreadCallback->getEffectsFactoryHal()->getDescriptors(
                                                        EFFECT_UIID_DOWNMIX, &descriptors);
        if (status != NO_ERROR) {
            return;
        }
        ALOG_ASSERT(!descriptors.empty(),
                "%s getDescriptors() returned no error but empty list", __func__);

        finalDownMixer = createEffect_l(nullptr /*client*/, nullptr /*effectClient*/,
                0 /*priority*/, AUDIO_SESSION_OUTPUT_STAGE, &descriptors[0], nullptr /*enabled*/,
                &status, false /*pinned*/, false /*probe*/, false /*notifyFramesProcessed*/);

        if (finalDownMixer == nullptr || (status != NO_ERROR && status != ALREADY_EXISTS)) {
            ALOGW("%s error creating downmixer %d", __func__, status);
            finalDownMixer.clear();
        } else {
            int32_t ret;
            finalDownMixer->asIEffect()->enable(&ret);
        }
    }

    {
        audio_utils::lock_guard _l(mutex());
        mFinalDownMixer = finalDownMixer;
    }
}

void SpatializerThread::threadLoop_exit()
{
    // The Spatializer EffectHandle must be released on the PlaybackThread
    // threadLoop() to prevent lock inversion in the SpatializerThread dtor.
    mFinalDownMixer.clear();

    PlaybackThread::threadLoop_exit();
}

// ----------------------------------------------------------------------------
//      Record
// ----------------------------------------------------------------------------

sp<IAfRecordThread> IAfRecordThread::create(const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamIn* input,
        audio_io_handle_t id,
        bool systemReady) {
    if (input->flags & AUDIO_INPUT_FLAG_DIRECT) {
        return sp<DirectRecordThread>::make(afThreadCallback, input, id, systemReady);
    }
    return sp<RecordThread>::make(afThreadCallback, RECORD, input, id, systemReady);
}

RecordThread::RecordThread(const sp<IAfThreadCallback>& afThreadCallback,
                                         ThreadBase::type_t type,
                                         AudioStreamIn *input,
                                         audio_io_handle_t id,
                                         bool systemReady
                                         ) :
    ThreadBase(afThreadCallback, id, type, systemReady, false /* isOut */),
    mInput(input),
    mSource(mInput),
    mActiveTracks(&this->mLocalLog),
    mRsmpInBuffer(NULL),
    // mRsmpInFrames, mRsmpInFramesP2, and mRsmpInFramesOA are set by readInputParameters_l()
    mRsmpInRear(0)
    , mReadOnlyHeap(new MemoryDealer(kRecordThreadReadOnlyHeapSize,
            "RecordThreadRO", MemoryHeapBase::READ_ONLY))
    // mFastCapture below
    , mFastCaptureFutex(0)
    // mInputSource
    // mPipeSink
    // mPipeSource
    , mPipeFramesP2(0)
    // mPipeMemory
    // mFastCaptureNBLogWriter
    , mFastTrackAvail(false)
    , mBtNrecSuspended(false)
{
    snprintf(mThreadName, kThreadNameLength, "AudioIn_%X", id);
    mFlagsAsString = toString(input->flags);

    if (mInput->audioHwDev != nullptr) {
        mIsMsdDevice = strcmp(
                mInput->audioHwDev->moduleName(), AUDIO_HARDWARE_MODULE_ID_MSD) == 0;
    }

    readInputParameters_l();

    // TODO: We may also match on address as well as device type for
    // AUDIO_DEVICE_IN_BUS, AUDIO_DEVICE_IN_BLUETOOTH_A2DP, AUDIO_DEVICE_IN_REMOTE_SUBMIX
    // TODO: This property should be ensure that only contains one single device type.
    mTimestampCorrectedDevice = (audio_devices_t)property_get_int64(
            "audio.timestamp.corrected_input_device",
            (int64_t)(mIsMsdDevice ? AUDIO_DEVICE_IN_BUS // turn on by default for MSD
                                   : AUDIO_DEVICE_NONE));

    // create an NBAIO source for the HAL input stream, and negotiate
    mInputSource = new AudioStreamInSource(input->stream);
    size_t numCounterOffers = 0;
    const NBAIO_Format offers[1] = {Format_from_SR_C(mSampleRate, mChannelCount, mFormat)};
#if !LOG_NDEBUG
    [[maybe_unused]] ssize_t index =
#else
    (void)
#endif
            mInputSource->negotiate(offers, 1, NULL, numCounterOffers);
    ALOG_ASSERT(index == 0);

    // initialize fast capture depending on configuration
    bool initFastCapture;
    switch (kUseFastCapture) {
    case FastCapture_Never:
        initFastCapture = false;
        ALOGV("%p kUseFastCapture = Never, initFastCapture = false", this);
        break;
    case FastCapture_Always:
        initFastCapture = true;
        ALOGV("%p kUseFastCapture = Always, initFastCapture = true", this);
        break;
    case FastCapture_Static:
        initFastCapture = !mIsMsdDevice // Disable fast capture for MSD BUS devices.
                && audio_is_linear_pcm(mFormat)
                && (mFrameCount * 1000) / mSampleRate < kMinNormalCaptureBufferSizeMs;
        ALOGV("%p kUseFastCapture = Static, format = 0x%x, (%lld * 1000) / %u vs %u, "
                "initFastCapture = %d, mIsMsdDevice = %d", this, mFormat, (long long)mFrameCount,
                mSampleRate, kMinNormalCaptureBufferSizeMs, initFastCapture, mIsMsdDevice);
        break;
    // case FastCapture_Dynamic:
    }

    if (initFastCapture) {
        // create a Pipe for FastCapture to write to, and for us and fast tracks to read from
        NBAIO_Format format = mInputSource->format();
        // quadruple-buffering of 20 ms each; this ensures we can sleep for 20ms in RecordThread
        size_t pipeFramesP2 = roundup(4 * FMS_20 * mSampleRate / 1000);
        size_t pipeSize = pipeFramesP2 * Format_frameSize(format);
        void *pipeBuffer = nullptr;
        const sp<MemoryDealer> roHeap(readOnlyHeap());
        sp<IMemory> pipeMemory;
        if ((roHeap == 0) ||
                (pipeMemory = roHeap->allocate(pipeSize)) == 0 ||
                (pipeBuffer = pipeMemory->unsecurePointer()) == nullptr) {
            ALOGE("not enough memory for pipe buffer size=%zu; "
                    "roHeap=%p, pipeMemory=%p, pipeBuffer=%p; roHeapSize: %lld",
                    pipeSize, roHeap.get(), pipeMemory.get(), pipeBuffer,
                    (long long)kRecordThreadReadOnlyHeapSize);
            goto failed;
        }
        // pipe will be shared directly with fast clients, so clear to avoid leaking old information
        memset(pipeBuffer, 0, pipeSize);
        Pipe *pipe = new Pipe(pipeFramesP2, format, pipeBuffer);
        const NBAIO_Format offersFast[1] = {format};
        size_t numCounterOffersFast = 0;
        [[maybe_unused]] ssize_t index2 = pipe->negotiate(offersFast, std::size(offersFast),
                nullptr /* counterOffers */, numCounterOffersFast);
        ALOG_ASSERT(index2 == 0);
        mPipeSink = pipe;
        PipeReader *pipeReader = new PipeReader(*pipe);
        numCounterOffersFast = 0;
        index2 = pipeReader->negotiate(offersFast, std::size(offersFast),
                nullptr /* counterOffers */, numCounterOffersFast);
        ALOG_ASSERT(index2 == 0);
        mPipeSource = pipeReader;
        mPipeFramesP2 = pipeFramesP2;
        mPipeMemory = pipeMemory;

        // create fast capture
        mFastCapture = new FastCapture();
        FastCaptureStateQueue *sq = mFastCapture->sq();
#ifdef STATE_QUEUE_DUMP
        // FIXME
#endif
        FastCaptureState *state = sq->begin();
        state->mCblk = NULL;
        state->mInputSource = mInputSource.get();
        state->mInputSourceGen++;
        state->mPipeSink = pipe;
        state->mPipeSinkGen++;
        state->mFrameCount = mFrameCount;
        state->mCommand = FastCaptureState::COLD_IDLE;
        // already done in constructor initialization list
        //mFastCaptureFutex = 0;
        state->mColdFutexAddr = &mFastCaptureFutex;
        state->mColdGen++;
        state->mDumpState = &mFastCaptureDumpState;
#ifdef TEE_SINK
        // FIXME
#endif
        sq->end();
        {
            audio_utils::mutex::scoped_queue_wait_check queueWaitCheck(mFastCapture->getTid());
            sq->push(FastCaptureStateQueue::BLOCK_UNTIL_PUSHED);
        }
        // start the fast capture
        mFastCapture->run("FastCapture", ANDROID_PRIORITY_URGENT_AUDIO);
        pid_t tid = mFastCapture->getTid();
        sendPrioConfigEvent(getpid(), tid, kPriorityFastCapture, false /*forApp*/);
        stream()->setHalThreadPriority(kPriorityFastCapture);
#ifdef AUDIO_WATCHDOG
        // FIXME
#endif

        mFastTrackAvail = true;
    }
#ifdef TEE_SINK
    mTee.set(mInputSource->format(), NBAIO_Tee::TEE_FLAG_INPUT_THREAD);
    mTee.setId(std::string("_") + std::to_string(mId) + "_C");
#endif
failed: ;

    // FIXME mNormalSource
}

RecordThread::~RecordThread()
{
    if (mFastCapture != 0) {
        FastCaptureStateQueue *sq = mFastCapture->sq();
        FastCaptureState *state = sq->begin();
        if (state->mCommand == FastCaptureState::COLD_IDLE) {
            int32_t old = android_atomic_inc(&mFastCaptureFutex);
            if (old == -1) {
                (void) syscall(__NR_futex, &mFastCaptureFutex, FUTEX_WAKE_PRIVATE, 1);
            }
        }
        state->mCommand = FastCaptureState::EXIT;
        sq->end();
        {
            audio_utils::mutex::scoped_join_wait_check queueWaitCheck(mFastCapture->getTid());
            sq->push(FastCaptureStateQueue::BLOCK_UNTIL_PUSHED);
            mFastCapture->join();
        }
        mFastCapture.clear();
    }
    free(mRsmpInBuffer);
}

void RecordThread::onFirstRef()
{
    run(mThreadName, PRIORITY_URGENT_AUDIO);
}

void RecordThread::preExit()
{
    ALOGV("  preExit()");
    audio_utils::lock_guard _l(mutex());
    for (size_t i = 0; i < mTracks.size(); i++) {
        sp<IAfRecordTrack> track = mTracks[i];
        track->invalidate();
    }
    mActiveTracks.clear();
    mStartStopCV.notify_all();
}

bool RecordThread::threadLoop()
{
    nsecs_t lastWarning = 0;

    inputStandBy();

reacquire_wakelock:
    {
        audio_utils::lock_guard _l(mutex());
        acquireWakeLock_l();
    }

    // used to request a deferred sleep, to be executed later while mutex is unlocked
    uint32_t sleepUs = 0;

    // timestamp correction enable is determined under lock, used in processing step.
    bool timestampCorrectionEnabled = false;

    int64_t lastLoopCountRead = -2;  // never matches "previous" loop, when loopCount = 0.

    // loop while there is work to do
    for (int64_t loopCount = 0;; ++loopCount) {  // loopCount used for statistics tracking
        // Note: these sp<> are released at the end of the for loop outside of the mutex() lock.
        sp<IAfRecordTrack> activeTrack;
        std::vector<sp<IAfRecordTrack>> oldActiveTracks;
        Vector<sp<IAfEffectChain>> effectChains;

        // activeTracks accumulates a copy of a subset of mActiveTracks
        Vector<sp<IAfRecordTrack>> activeTracks;

        // reference to the (first and only) active fast track
        sp<IAfRecordTrack> fastTrack;

        // reference to a fast track which is about to be removed
        sp<IAfRecordTrack> fastTrackToRemove;

        bool silenceFastCapture = false;

        { // scope for mutex()
            audio_utils::unique_lock _l(mutex());

            processConfigEvents_l();

            // check exitPending here because checkForNewParameters_l() and
            // checkForNewParameters_l() can temporarily release mutex()
            if (exitPending()) {
                break;
            }

            // sleep with mutex unlocked
            if (sleepUs > 0) {
                ATRACE_BEGIN("sleepC");
                (void)mWaitWorkCV.wait_for(_l, std::chrono::microseconds(sleepUs));
                ATRACE_END();
                sleepUs = 0;
                continue;
            }

            // if no active track(s), then standby and release wakelock
            size_t size = mActiveTracks.size();
            if (size == 0) {
                standbyIfNotAlreadyInStandby();
                // exitPending() can't become true here
                releaseWakeLock_l();
                ALOGV("RecordThread: loop stopping");
                // go to sleep
                mWaitWorkCV.wait(_l);
                ALOGV("RecordThread: loop starting");
                goto reacquire_wakelock;
            }

            bool doBroadcast = false;
            bool allStopped = true;
            for (size_t i = 0; i < size; ) {
                if (activeTrack) {  // ensure track release is outside lock.
                    oldActiveTracks.emplace_back(std::move(activeTrack));
                }
                activeTrack = mActiveTracks[i];
                if (activeTrack->isTerminated()) {
                    if (activeTrack->isFastTrack()) {
                        ALOG_ASSERT(fastTrackToRemove == 0);
                        fastTrackToRemove = activeTrack;
                    }
                    removeTrack_l(activeTrack);
                    mActiveTracks.remove(activeTrack);
                    size--;
                    continue;
                }

                IAfTrackBase::track_state activeTrackState = activeTrack->state();
                switch (activeTrackState) {

                case IAfTrackBase::PAUSING:
                    mActiveTracks.remove(activeTrack);
                    activeTrack->setState(IAfTrackBase::PAUSED);
                    if (activeTrack->isFastTrack()) {
                        ALOGV("%s fast track is paused, thus removed from active list", __func__);
                        // Keep a ref on fast track to wait for FastCapture thread to get updated
                        // state before potential track removal
                        fastTrackToRemove = activeTrack;
                    }
                    doBroadcast = true;
                    size--;
                    continue;

                case IAfTrackBase::STARTING_1:
                    sleepUs = 10000;
                    i++;
                    allStopped = false;
                    continue;

                case IAfTrackBase::STARTING_2:
                    doBroadcast = true;
                    if (mStandby) {
                        mThreadMetrics.logBeginInterval();
                        mThreadSnapshot.onBegin();
                        mStandby = false;
                    }
                    activeTrack->setState(IAfTrackBase::ACTIVE);
                    allStopped = false;
                    break;

                case IAfTrackBase::ACTIVE:
                    allStopped = false;
                    break;

                case IAfTrackBase::IDLE:    // cannot be on ActiveTracks if idle
                case IAfTrackBase::PAUSED:  // cannot be on ActiveTracks if paused
                case IAfTrackBase::STOPPED: // cannot be on ActiveTracks if destroyed/terminated
                default:
                    LOG_ALWAYS_FATAL("%s: Unexpected active track state:%d, id:%d, tracks:%zu",
                            __func__, activeTrackState, activeTrack->id(), size);
                }

                if (activeTrack->isFastTrack()) {
                    ALOG_ASSERT(!mFastTrackAvail);
                    ALOG_ASSERT(fastTrack == 0);
                    // if the active fast track is silenced either:
                    // 1) silence the whole capture from fast capture buffer if this is
                    //    the only active track
                    // 2) invalidate this track: this will cause the client to reconnect and possibly
                    //    be invalidated again until unsilenced
                    bool invalidate = false;
                    if (activeTrack->isSilenced()) {
                        if (size > 1) {
                            invalidate = true;
                        } else {
                            silenceFastCapture = true;
                        }
                    }
                    // Invalidate fast tracks if access to audio history is required as this is not
                    // possible with fast tracks. Once the fast track has been invalidated, no new
                    // fast track will be created until mMaxSharedAudioHistoryMs is cleared.
                    if (mMaxSharedAudioHistoryMs != 0) {
                        invalidate = true;
                    }
                    if (invalidate) {
                        activeTrack->invalidate();
                        fastTrackToRemove = activeTrack;
                        removeTrack_l(activeTrack);
                        mActiveTracks.remove(activeTrack);
                        size--;
                        continue;
                    }
                    fastTrack = activeTrack;
                }

                activeTracks.add(activeTrack);
                i++;

            }

            mActiveTracks.updatePowerState_l(this);

            // check if traces have been enabled.
            bool atraceEnabled = ATRACE_ENABLED();
            if (atraceEnabled != mAtraceEnabled) [[unlikely]] {
                mAtraceEnabled = atraceEnabled;
                if (atraceEnabled) {
                    const auto devices = patchSourcesToString(&mPatch);
                    for (const auto& track : activeTracks) {
                        track->logRefreshInterval(devices);
                    }
                }
            }

            updateMetadata_l();

            if (allStopped) {
                standbyIfNotAlreadyInStandby();
            }
            if (doBroadcast) {
                mStartStopCV.notify_all();
            }

            // sleep if there are no active tracks to process
            if (activeTracks.isEmpty()) {
                if (sleepUs == 0) {
                    sleepUs = kRecordThreadSleepUs;
                }
                continue;
            }
            sleepUs = 0;

            timestampCorrectionEnabled = isTimestampCorrectionEnabled_l();
            lockEffectChains_l(effectChains);
            // We're exiting locked scope with non empty activeTracks, make sure
            // that we're not in standby mode which we could have entered if some
            // tracks were muted/unmuted.
            mStandby = false;
        }

        // thread mutex is now unlocked, mActiveTracks unknown, activeTracks.size() > 0

        size_t size = effectChains.size();
        for (size_t i = 0; i < size; i++) {
            // thread mutex is not locked, but effect chain is locked
            effectChains[i]->process_l();
        }

        // Push a new fast capture state if fast capture is not already running, or cblk change
        if (mFastCapture != 0) {
            FastCaptureStateQueue *sq = mFastCapture->sq();
            FastCaptureState *state = sq->begin();
            bool didModify = false;
            FastCaptureStateQueue::block_t block = FastCaptureStateQueue::BLOCK_UNTIL_PUSHED;
            if (state->mCommand != FastCaptureState::READ_WRITE /* FIXME &&
                    (kUseFastMixer != FastMixer_Dynamic || state->mTrackMask > 1)*/) {
                if (state->mCommand == FastCaptureState::COLD_IDLE) {
                    int32_t old = android_atomic_inc(&mFastCaptureFutex);
                    if (old == -1) {
                        (void) syscall(__NR_futex, &mFastCaptureFutex, FUTEX_WAKE_PRIVATE, 1);
                    }
                }
                state->mCommand = FastCaptureState::READ_WRITE;
#if 0   // FIXME
                mFastCaptureDumpState.increaseSamplingN(mAfThreadCallback->isLowRamDevice() ?
                        FastThreadDumpState::kSamplingNforLowRamDevice :
                        FastThreadDumpState::kSamplingN);
#endif
                didModify = true;
            }
            audio_track_cblk_t *cblkOld = state->mCblk;
            audio_track_cblk_t *cblkNew = fastTrack != 0 ? fastTrack->cblk() : NULL;
            if (cblkNew != cblkOld) {
                state->mCblk = cblkNew;
                // block until acked if removing a fast track
                if (cblkOld != NULL) {
                    block = FastCaptureStateQueue::BLOCK_UNTIL_ACKED;
                }
                didModify = true;
            }
            AudioBufferProvider* abp = (fastTrack != 0 && fastTrack->isPatchTrack()) ?
                    reinterpret_cast<AudioBufferProvider*>(fastTrack.get()) : nullptr;
            if (state->mFastPatchRecordBufferProvider != abp) {
                state->mFastPatchRecordBufferProvider = abp;
                state->mFastPatchRecordFormat = fastTrack == 0 ?
                        AUDIO_FORMAT_INVALID : fastTrack->format();
                didModify = true;
            }
            if (state->mSilenceCapture != silenceFastCapture) {
                state->mSilenceCapture = silenceFastCapture;
                didModify = true;
            }
            sq->end(didModify);
            if (didModify) {
                sq->push(block);
#if 0
                if (kUseFastCapture == FastCapture_Dynamic) {
                    mNormalSource = mPipeSource;
                }
#endif
            }
        }

        // now run the fast track destructor with thread mutex unlocked
        fastTrackToRemove.clear();

        // Read from HAL to keep up with fastest client if multiple active tracks, not slowest one.
        // Only the client(s) that are too slow will overrun. But if even the fastest client is too
        // slow, then this RecordThread will overrun by not calling HAL read often enough.
        // If destination is non-contiguous, first read past the nominal end of buffer, then
        // copy to the right place.  Permitted because mRsmpInBuffer was over-allocated.

        int32_t rear = mRsmpInRear & (mRsmpInFramesP2 - 1);
        ssize_t framesRead = 0; // not needed, remove clang-tidy warning.
        const int64_t lastIoBeginNs = systemTime(); // start IO timing

        // If an NBAIO source is present, use it to read the normal capture's data
        if (mPipeSource != 0) {
            size_t framesToRead = min(mRsmpInFramesOA - rear, mRsmpInFramesP2 / 2);

            // The audio fifo read() returns OVERRUN on overflow, and advances the read pointer
            // to the full buffer point (clearing the overflow condition).  Upon OVERRUN error,
            // we immediately retry the read() to get data and prevent another overflow.
            for (int retries = 0; retries <= 2; ++retries) {
                ALOGW_IF(retries > 0, "overrun on read from pipe, retry #%d", retries);
                framesRead = mPipeSource->read((uint8_t*)mRsmpInBuffer + rear * mFrameSize,
                        framesToRead);
                if (framesRead != OVERRUN) break;
            }

            const ssize_t availableToRead = mPipeSource->availableToRead();
            if (availableToRead >= 0) {
                mMonopipePipeDepthStats.add(availableToRead);
                // PipeSource is the primary clock.  It is up to the AudioRecord client to keep up.
                LOG_ALWAYS_FATAL_IF((size_t)availableToRead > mPipeFramesP2,
                        "more frames to read than fifo size, %zd > %zu",
                        availableToRead, mPipeFramesP2);
                const size_t pipeFramesFree = mPipeFramesP2 - availableToRead;
                const size_t sleepFrames = min(pipeFramesFree, mRsmpInFramesP2) / 2;
                ALOGVV("mPipeFramesP2:%zu mRsmpInFramesP2:%zu sleepFrames:%zu availableToRead:%zd",
                        mPipeFramesP2, mRsmpInFramesP2, sleepFrames, availableToRead);
                sleepUs = (sleepFrames * 1000000LL) / mSampleRate;
            }
            if (framesRead < 0) {
                status_t status = (status_t) framesRead;
                switch (status) {
                case OVERRUN:
                    ALOGW("overrun on read from pipe");
                    framesRead = 0;
                    break;
                case NEGOTIATE:
                    ALOGE("re-negotiation is needed");
                    framesRead = -1;  // Will cause an attempt to recover.
                    break;
                default:
                    ALOGE("unknown error %d on read from pipe", status);
                    break;
                }
            }
        // otherwise use the HAL / AudioStreamIn directly
        } else {
            ATRACE_BEGIN("read");
            size_t bytesRead;
            status_t result = mSource->read(
                    (uint8_t*)mRsmpInBuffer + rear * mFrameSize, mBufferSize, &bytesRead);
            ATRACE_END();
            if (result < 0) {
                framesRead = result;
            } else {
                framesRead = bytesRead / mFrameSize;
            }
        }

        const int64_t lastIoEndNs = systemTime(); // end IO timing

        // Update server timestamp with server stats
        // systemTime() is optional if the hardware supports timestamps.
        if (framesRead >= 0) {
            mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER] += framesRead;
            mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_SERVER] = lastIoEndNs;
        }

        // Update server timestamp with kernel stats
        if (mPipeSource.get() == nullptr /* don't obtain for FastCapture, could block */) {
            int64_t position, time;
            if (mStandby) {
                mTimestampVerifier.discontinuity(audio_is_linear_pcm(mFormat) ?
                    mTimestampVerifier.DISCONTINUITY_MODE_CONTINUOUS :
                    mTimestampVerifier.DISCONTINUITY_MODE_ZERO);
            } else if (mSource->getCapturePosition(&position, &time) == NO_ERROR
                    && time > mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL]) {

                mTimestampVerifier.add(position, time, mSampleRate);
                if (timestampCorrectionEnabled) {
                    ALOGVV("TS_BEFORE: %d %lld %lld",
                            id(), (long long)time, (long long)position);
                    auto correctedTimestamp = mTimestampVerifier.getLastCorrectedTimestamp();
                    position = correctedTimestamp.mFrames;
                    time = correctedTimestamp.mTimeNs;
                    ALOGVV("TS_AFTER: %d %lld %lld",
                            id(), (long long)time, (long long)position);
                }

                mTimestamp.mPosition[ExtendedTimestamp::LOCATION_KERNEL] = position;
                mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_KERNEL] = time;
                // Note: In general record buffers should tend to be empty in
                // a properly running pipeline.
                //
                // Also, it is not advantageous to call get_presentation_position during the read
                // as the read obtains a lock, preventing the timestamp call from executing.
            } else {
                mTimestampVerifier.error();
            }
        }

        // From the timestamp, input read latency is negative output write latency.
        const audio_input_flags_t flags = mInput != NULL ? mInput->flags : AUDIO_INPUT_FLAG_NONE;
        const double latencyMs = IAfRecordTrack::checkServerLatencySupported(mFormat, flags)
                ? - mTimestamp.getOutputServerLatencyMs(mSampleRate) : 0.;
        if (latencyMs != 0.) { // note 0. means timestamp is empty.
            mLatencyMs.add(latencyMs);
        }

        // Use this to track timestamp information
        // ALOGD("%s", mTimestamp.toString().c_str());

        if (framesRead < 0 || (framesRead == 0 && mPipeSource == 0)) {
            ALOGE("read failed: framesRead=%zd", framesRead);
            // Force input into standby so that it tries to recover at next read attempt
            inputStandBy();
            sleepUs = kRecordThreadSleepUs;
        }
        if (framesRead <= 0) {
            goto unlock;
        }
        ALOG_ASSERT(framesRead > 0);
        mFramesRead += framesRead;

#ifdef TEE_SINK
        (void)mTee.write((uint8_t*)mRsmpInBuffer + rear * mFrameSize, framesRead);
#endif
        // If destination is non-contiguous, we now correct for reading past end of buffer.
        {
            size_t part1 = mRsmpInFramesP2 - rear;
            if ((size_t) framesRead > part1) {
                memcpy(mRsmpInBuffer, (uint8_t*)mRsmpInBuffer + mRsmpInFramesP2 * mFrameSize,
                        (framesRead - part1) * mFrameSize);
            }
        }
        mRsmpInRear = audio_utils::safe_add_overflow(mRsmpInRear, (int32_t)framesRead);

        size = activeTracks.size();

        // loop over each active track
        for (size_t i = 0; i < size; i++) {
            if (activeTrack) {  // ensure track release is outside lock.
                oldActiveTracks.emplace_back(std::move(activeTrack));
            }
            activeTrack = activeTracks[i];

            // skip fast tracks, as those are handled directly by FastCapture
            if (activeTrack->isFastTrack()) {
                continue;
            }

            // TODO: This code probably should be moved to RecordTrack.
            // TODO: Update the activeTrack buffer converter in case of reconfigure.

            enum {
                OVERRUN_UNKNOWN,
                OVERRUN_TRUE,
                OVERRUN_FALSE
            } overrun = OVERRUN_UNKNOWN;

            // loop over getNextBuffer to handle circular sink
            for (;;) {

                activeTrack->sinkBuffer().frameCount = ~0;
                status_t status = activeTrack->getNextBuffer(&activeTrack->sinkBuffer());
                size_t framesOut = activeTrack->sinkBuffer().frameCount;
                LOG_ALWAYS_FATAL_IF((status == OK) != (framesOut > 0));

                // check available frames and handle overrun conditions
                // if the record track isn't draining fast enough.
                bool hasOverrun;
                size_t framesIn;
                activeTrack->resamplerBufferProvider()->sync(&framesIn, &hasOverrun);
                if (hasOverrun) {
                    overrun = OVERRUN_TRUE;
                }
                if (framesOut == 0 || framesIn == 0) {
                    break;
                }

                // Don't allow framesOut to be larger than what is possible with resampling
                // from framesIn.
                // This isn't strictly necessary but helps limit buffer resizing in
                // RecordBufferConverter.  TODO: remove when no longer needed.
                if (audio_is_linear_pcm(activeTrack->format())) {
                    framesOut = min(framesOut,
                            destinationFramesPossible(
                                    framesIn, mSampleRate, activeTrack->sampleRate()));
                }

                if (activeTrack->isDirect()) {
                    // No RecordBufferConverter used for direct streams. Pass
                    // straight from RecordThread buffer to RecordTrack buffer.
                    AudioBufferProvider::Buffer buffer;
                    buffer.frameCount = framesOut;
                    const status_t getNextBufferStatus =
                            activeTrack->resamplerBufferProvider()->getNextBuffer(&buffer);
                    if (getNextBufferStatus == OK && buffer.frameCount != 0) {
                        ALOGV_IF(buffer.frameCount != framesOut,
                                "%s() read less than expected (%zu vs %zu)",
                                __func__, buffer.frameCount, framesOut);
                        framesOut = buffer.frameCount;
                        memcpy(activeTrack->sinkBuffer().raw,
                                buffer.raw, buffer.frameCount * mFrameSize);
                        activeTrack->resamplerBufferProvider()->releaseBuffer(&buffer);
                    } else {
                        framesOut = 0;
                        ALOGE("%s() cannot fill request, status: %d, frameCount: %zu",
                            __func__, getNextBufferStatus, buffer.frameCount);
                    }
                } else {
                    // process frames from the RecordThread buffer provider to the RecordTrack
                    // buffer
                    framesOut = activeTrack->recordBufferConverter()->convert(
                            activeTrack->sinkBuffer().raw,
                            activeTrack->resamplerBufferProvider(),
                            framesOut);
                }

                if (framesOut > 0 && (overrun == OVERRUN_UNKNOWN)) {
                    overrun = OVERRUN_FALSE;
                }

                // MediaSyncEvent handling: Synchronize AudioRecord to AudioTrack completion.
                const ssize_t framesToDrop =
                        activeTrack->synchronizedRecordState().updateRecordFrames(framesOut);
                if (framesToDrop == 0) {
                    // no sync event, process normally, otherwise ignore.
                    if (framesOut > 0) {
                        activeTrack->sinkBuffer().frameCount = framesOut;
                        // Sanitize before releasing if the track has no access to the source data
                        // An idle UID receives silence from non virtual devices until active
                        if (activeTrack->isSilenced()) {
                            memset(activeTrack->sinkBuffer().raw,
                                    0, framesOut * activeTrack->frameSize());
                        }
                        activeTrack->releaseBuffer(&activeTrack->sinkBuffer());
                    }
                }
                if (framesOut == 0) {
                    break;
                }
            }

            switch (overrun) {
            case OVERRUN_TRUE:
                // client isn't retrieving buffers fast enough
                if (!activeTrack->setOverflow()) {
                    nsecs_t now = systemTime();
                    // FIXME should lastWarning per track?
                    if ((now - lastWarning) > kWarningThrottleNs) {
                        ALOGW("RecordThread: buffer overflow");
                        lastWarning = now;
                    }
                }
                break;
            case OVERRUN_FALSE:
                activeTrack->clearOverflow();
                break;
            case OVERRUN_UNKNOWN:
                break;
            }

            // update frame information and push timestamp out
            activeTrack->updateTrackFrameInfo(
                    activeTrack->serverProxy()->framesReleased(),
                    mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER],
                    mSampleRate, mTimestamp);
        }

unlock:
        // enable changes in effect chain
        unlockEffectChains(effectChains);
        // effectChains doesn't need to be cleared, since it is cleared by destructor at scope end
        if (audio_has_proportional_frames(mFormat)
            && loopCount == lastLoopCountRead + 1) {
            const int64_t readPeriodNs = lastIoEndNs - mLastIoEndNs;
            const double jitterMs =
                TimestampVerifier<int64_t, int64_t>::computeJitterMs(
                    {framesRead, readPeriodNs},
                    {0, 0} /* lastTimestamp */, mSampleRate);
            const double processMs = (lastIoBeginNs - mLastIoEndNs) * 1e-6;

            audio_utils::lock_guard _l(mutex());
            mIoJitterMs.add(jitterMs);
            mProcessTimeMs.add(processMs);
        }
       mThreadloopExecutor.process();
        // update timing info.
        mLastIoBeginNs = lastIoBeginNs;
        mLastIoEndNs = lastIoEndNs;
        lastLoopCountRead = loopCount;
    }
    mThreadloopExecutor.process(); // process any remaining deferred actions.
    // deferred actions after this point are ignored.

    standbyIfNotAlreadyInStandby();

    {
        audio_utils::lock_guard _l(mutex());
        for (size_t i = 0; i < mTracks.size(); i++) {
            sp<IAfRecordTrack> track = mTracks[i];
            track->invalidate();
        }
        mActiveTracks.clear();
        mStartStopCV.notify_all();
    }

    releaseWakeLock();

    ALOGV("RecordThread %p exiting", this);
    return false;
}

void RecordThread::standbyIfNotAlreadyInStandby()
{
    if (!mStandby) {
        inputStandBy();
        mThreadMetrics.logEndInterval();
        mThreadSnapshot.onEnd();
        mStandby = true;
    }
}

void RecordThread::inputStandBy()
{
    // Idle the fast capture if it's currently running
    if (mFastCapture != 0) {
        FastCaptureStateQueue *sq = mFastCapture->sq();
        FastCaptureState *state = sq->begin();
        if (!(state->mCommand & FastCaptureState::IDLE)) {
            state->mCommand = FastCaptureState::COLD_IDLE;
            state->mColdFutexAddr = &mFastCaptureFutex;
            state->mColdGen++;
            mFastCaptureFutex = 0;
            sq->end();
            // BLOCK_UNTIL_PUSHED would be insufficient, as we need it to stop doing I/O now
            {
                audio_utils::mutex::scoped_queue_wait_check queueWaitCheck(mFastCapture->getTid());
                sq->push(FastCaptureStateQueue::BLOCK_UNTIL_ACKED);
            }

#if 0
            if (kUseFastCapture == FastCapture_Dynamic) {
                // FIXME
            }
#endif
#ifdef AUDIO_WATCHDOG
            // FIXME
#endif
        } else {
            sq->end(false /*didModify*/);
        }
    }
    status_t result = mSource->standby();
    ALOGE_IF(result != OK, "Error when putting input stream into standby: %d", result);

    // If going into standby, flush the pipe source.
    if (mPipeSource.get() != nullptr) {
        const ssize_t flushed = mPipeSource->flush();
        if (flushed > 0) {
            ALOGV("Input standby flushed PipeSource %zd frames", flushed);
            mTimestamp.mPosition[ExtendedTimestamp::LOCATION_SERVER] += flushed;
            mTimestamp.mTimeNs[ExtendedTimestamp::LOCATION_SERVER] = systemTime();
        }
    }
}

// RecordThread::createRecordTrack_l() must be called with AudioFlinger::mutex() held
sp<IAfRecordTrack> RecordThread::createRecordTrack_l(
        const sp<Client>& client,
        const audio_attributes_t& attr,
        uint32_t *pSampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t *pFrameCount,
        audio_session_t sessionId,
        size_t *pNotificationFrameCount,
        pid_t creatorPid,
        const AttributionSourceState& attributionSource,
        audio_input_flags_t *flags,
        pid_t tid,
        status_t *status,
        audio_port_handle_t portId,
        int32_t maxSharedAudioHistoryMs)
{
    size_t frameCount = *pFrameCount;
    size_t notificationFrameCount = *pNotificationFrameCount;
    sp<IAfRecordTrack> track;
    status_t lStatus;
    audio_input_flags_t inputFlags = mInput->flags;
    audio_input_flags_t requestedFlags = *flags;
    uint32_t sampleRate;

    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGE("createRecordTrack_l() audio driver not initialized");
        goto Exit;
    }

    if (!audio_is_linear_pcm(mFormat) && (*flags & AUDIO_INPUT_FLAG_DIRECT) == 0) {
        ALOGE("createRecordTrack_l() on an encoded stream requires AUDIO_INPUT_FLAG_DIRECT");
        lStatus = BAD_VALUE;
        goto Exit;
    }

    if (maxSharedAudioHistoryMs != 0) {
        if (audioserver_permissions()) {
            const auto res = mAfThreadCallback->getPermissionProvider().checkPermission(
                    CAPTURE_AUDIO_HOTWORD,
                    attributionSource.uid);
            if (!res.ok()) {
                lStatus = aidl_utils::statusTFromBinderStatus(res.error());
            }
            if (!res.value()) {
                lStatus = PERMISSION_DENIED;
                goto Exit;
            }
        } else {
            if (!captureHotwordAllowed(attributionSource)) {
                lStatus = PERMISSION_DENIED;
                goto Exit;
            }
        }
        if (maxSharedAudioHistoryMs < 0
                || maxSharedAudioHistoryMs > kMaxSharedAudioHistoryMs) {
            lStatus = BAD_VALUE;
            goto Exit;
        }
    }
    if (*pSampleRate == 0) {
        *pSampleRate = mSampleRate;
    }
    sampleRate = *pSampleRate;

    // special case for FAST flag considered OK if fast capture is present and access to
    // audio history is not required
    if (hasFastCapture() && mMaxSharedAudioHistoryMs == 0) {
        inputFlags = (audio_input_flags_t)(inputFlags | AUDIO_INPUT_FLAG_FAST);
    }

    // Check if requested flags are compatible with input stream flags
    if ((*flags & inputFlags) != *flags) {
        ALOGW("createRecordTrack_l(): mismatch between requested flags (%08x) and"
                " input flags (%08x)",
              *flags, inputFlags);
        *flags = (audio_input_flags_t)(*flags & inputFlags);
    }

    // client expresses a preference for FAST and no access to audio history,
    // but we get the final say
    if (*flags & AUDIO_INPUT_FLAG_FAST && maxSharedAudioHistoryMs == 0) {
      if (
            // we formerly checked for a callback handler (non-0 tid),
            // but that is no longer required for TRANSFER_OBTAIN mode
            // No need to match hardware format, format conversion will be done in client side.
            //
            // Frame count is not specified (0), or is less than or equal the pipe depth.
            // It is OK to provide a higher capacity than requested.
            // We will force it to mPipeFramesP2 below.
            (frameCount <= mPipeFramesP2) &&
            // PCM data
            audio_is_linear_pcm(format) &&
            // hardware channel mask
            (channelMask == mChannelMask) &&
            // hardware sample rate
            (sampleRate == mSampleRate) &&
            // record thread has an associated fast capture
            hasFastCapture() &&
            // there are sufficient fast track slots available
            mFastTrackAvail
        ) {
          // check compatibility with audio effects.
          audio_utils::lock_guard _l(mutex());
          // Do not accept FAST flag if the session has software effects
          sp<IAfEffectChain> chain = getEffectChain_l(sessionId);
          if (chain != 0) {
              audio_input_flags_t old = *flags;
              chain->checkInputFlagCompatibility(flags);
              if (old != *flags) {
                  ALOGV("%p AUDIO_INPUT_FLAGS denied by effect old=%#x new=%#x",
                          this, (int)old, (int)*flags);
              }
          }
          ALOGV_IF((*flags & AUDIO_INPUT_FLAG_FAST) != 0,
                   "%p AUDIO_INPUT_FLAG_FAST accepted: frameCount=%zu mFrameCount=%zu",
                   this, frameCount, mFrameCount);
      } else {
        ALOGV("%p AUDIO_INPUT_FLAG_FAST denied: frameCount=%zu mFrameCount=%zu mPipeFramesP2=%zu "
                "format=%#x isLinear=%d mFormat=%#x channelMask=%#x sampleRate=%u mSampleRate=%u "
                "hasFastCapture=%d tid=%d mFastTrackAvail=%d",
                this, frameCount, mFrameCount, mPipeFramesP2,
                format, audio_is_linear_pcm(format), mFormat, channelMask, sampleRate, mSampleRate,
                hasFastCapture(), tid, mFastTrackAvail);
        *flags = (audio_input_flags_t)(*flags & ~AUDIO_INPUT_FLAG_FAST);
      }
    }

    // If FAST or RAW flags were corrected, ask caller to request new input from audio policy
    if ((*flags & AUDIO_INPUT_FLAG_FAST) !=
            (requestedFlags & AUDIO_INPUT_FLAG_FAST)) {
        *flags = (audio_input_flags_t) (*flags & ~(AUDIO_INPUT_FLAG_FAST | AUDIO_INPUT_FLAG_RAW));
        lStatus = BAD_TYPE;
        goto Exit;
    }

    // compute track buffer size in frames, and suggest the notification frame count
    if (*flags & AUDIO_INPUT_FLAG_FAST) {
        // fast track: frame count is exactly the pipe depth
        frameCount = mPipeFramesP2;
        // ignore requested notificationFrames, and always notify exactly once every HAL buffer
        notificationFrameCount = mFrameCount;
    } else {
        // not fast track: max notification period is resampled equivalent of one HAL buffer time
        //                 or 20 ms if there is a fast capture
        // TODO This could be a roundupRatio inline, and const
        size_t maxNotificationFrames = ((int64_t) (hasFastCapture() ? mSampleRate/50 : mFrameCount)
                * sampleRate + mSampleRate - 1) / mSampleRate;
        // minimum number of notification periods is at least kMinNotifications,
        // and at least kMinMs rounded up to a whole notification period (minNotificationsByMs)
        static const size_t kMinNotifications = 3;
        static const uint32_t kMinMs = 30;
        // TODO This could be a roundupRatio inline
        const size_t minFramesByMs = (sampleRate * kMinMs + 1000 - 1) / 1000;
        // TODO This could be a roundupRatio inline
        const size_t minNotificationsByMs = (minFramesByMs + maxNotificationFrames - 1) /
                maxNotificationFrames;
        const size_t minFrameCount = maxNotificationFrames *
                max(kMinNotifications, minNotificationsByMs);
        frameCount = max(frameCount, minFrameCount);
        if (notificationFrameCount == 0 || notificationFrameCount > maxNotificationFrames) {
            notificationFrameCount = maxNotificationFrames;
        }
    }
    *pFrameCount = frameCount;
    *pNotificationFrameCount = notificationFrameCount;

    { // scope for mutex()
        audio_utils::lock_guard _l(mutex());
        int32_t startFrames = -1;
        if (!mSharedAudioPackageName.empty()
                && mSharedAudioPackageName == attributionSource.packageName
                && mSharedAudioSessionId == sessionId
                && (audioserver_permissions() ?
                      mAfThreadCallback->getPermissionProvider().checkPermission(
                          CAPTURE_AUDIO_HOTWORD,
                          attributionSource.uid).value_or(false)
                    : captureHotwordAllowed(attributionSource))) {
            startFrames = mSharedAudioStartFrames;
        }

        track = IAfRecordTrack::create(this, client, attr, sampleRate,
                      format, channelMask, frameCount,
                      nullptr /* buffer */, (size_t)0 /* bufferSize */, sessionId, creatorPid,
                      attributionSource, *flags, IAfTrackBase::TYPE_DEFAULT, portId,
                      startFrames);

        lStatus = track->initCheck();
        if (lStatus != NO_ERROR) {
            ALOGE("createRecordTrack_l() initCheck failed %d; no control block?", lStatus);
            // track must be cleared from the caller as the caller has the AF lock
            goto Exit;
        }
        mTracks.add(track);

        if ((*flags & AUDIO_INPUT_FLAG_FAST) && (tid != -1)) {
            pid_t callingPid = IPCThreadState::self()->getCallingPid();
            // we don't have CAP_SYS_NICE, nor do we want to have it as it's too powerful,
            // so ask activity manager to do this on our behalf
            sendPrioConfigEvent_l(callingPid, tid, kPriorityAudioApp, true /*forApp*/);
        }

        if (maxSharedAudioHistoryMs != 0) {
            sendResizeBufferConfigEvent_l(maxSharedAudioHistoryMs);
        }
    }

    lStatus = NO_ERROR;

Exit:
    *status = lStatus;
    return track;
}

status_t RecordThread::start(IAfRecordTrack* recordTrack,
                                           AudioSystem::sync_event_t event,
                                           audio_session_t triggerSession)
{
    ALOGV("RecordThread::start event %d, triggerSession %d", event, triggerSession);
    sp<ThreadBase> strongMe = this;
    status_t status = NO_ERROR;

    if (event == AudioSystem::SYNC_EVENT_NONE) {
        recordTrack->clearSyncStartEvent();
    } else if (event != AudioSystem::SYNC_EVENT_SAME) {
        recordTrack->synchronizedRecordState().startRecording(
                mAfThreadCallback->createSyncEvent(
                        event, triggerSession,
                        recordTrack->sessionId(), syncStartEventCallback, recordTrack));
    }

    {
        // This section is a rendezvous between binder thread executing start() and RecordThread
         audio_utils::lock_guard lock(mutex());
        if (recordTrack->isInvalid()) {
            recordTrack->clearSyncStartEvent();
            ALOGW("%s track %d: invalidated before startInput", __func__, recordTrack->portId());
            return DEAD_OBJECT;
        }
        if (mActiveTracks.indexOf(recordTrack) >= 0) {
            if (recordTrack->state() == IAfTrackBase::PAUSING) {
                // We haven't stopped yet (moved to PAUSED and not in mActiveTracks)
                // so no need to startInput().
                ALOGV("active record track PAUSING -> ACTIVE");
                recordTrack->setState(IAfTrackBase::ACTIVE);
            } else {
                ALOGV("active record track state %d", (int)recordTrack->state());
            }
            return status;
        }

        // TODO consider other ways of handling this, such as changing the state to :STARTING and
        //      adding the track to mActiveTracks after returning from AudioSystem::startInput(),
        //      or using a separate command thread
        recordTrack->setState(IAfTrackBase::STARTING_1);
        mActiveTracks.add(recordTrack);
        if (recordTrack->isExternalTrack()) {
            mutex().unlock();
            status = AudioSystem::startInput(recordTrack->portId());
            mutex().lock();
            if (recordTrack->isInvalid()) {
                recordTrack->clearSyncStartEvent();
                if (status == NO_ERROR && recordTrack->state() == IAfTrackBase::STARTING_1) {
                    recordTrack->setState(IAfTrackBase::STARTING_2);
                    // STARTING_2 forces destroy to call stopInput.
                }
                ALOGW("%s track %d: invalidated after startInput", __func__, recordTrack->portId());
                return DEAD_OBJECT;
            }
            if (recordTrack->state() != IAfTrackBase::STARTING_1) {
                ALOGW("%s(%d): unsynchronized mState:%d change",
                    __func__, recordTrack->id(), (int)recordTrack->state());
                // Someone else has changed state, let them take over,
                // leave mState in the new state.
                recordTrack->clearSyncStartEvent();
                return INVALID_OPERATION;
            }
            // we're ok, but perhaps startInput has failed
            if (status != NO_ERROR) {
                ALOGW("%s(%d): startInput failed, status %d",
                    __func__, recordTrack->id(), status);
                // We are in ActiveTracks if STARTING_1 and valid, so remove from ActiveTracks,
                // leave in STARTING_1, so destroy() will not call stopInput.
                mActiveTracks.remove(recordTrack);
                recordTrack->clearSyncStartEvent();
                return status;
            }
            sendIoConfigEvent_l(
                AUDIO_CLIENT_STARTED, recordTrack->creatorPid(), recordTrack->portId());
        }

        recordTrack->logBeginInterval(patchSourcesToString(&mPatch)); // log to MediaMetrics

        // Catch up with current buffer indices if thread is already running.
        // This is what makes a new client discard all buffered data.  If the track's mRsmpInFront
        // was initialized to some value closer to the thread's mRsmpInFront, then the track could
        // see previously buffered data before it called start(), but with greater risk of overrun.

        recordTrack->resamplerBufferProvider()->reset();
        if (!recordTrack->isDirect()) {
            // clear any converter state as new data will be discontinuous
            recordTrack->recordBufferConverter()->reset();
        }
        recordTrack->setState(IAfTrackBase::STARTING_2);
        // signal thread to start
        mWaitWorkCV.notify_all();
        return status;
    }
}

void RecordThread::syncStartEventCallback(const wp<SyncEvent>& event)
{
    const sp<SyncEvent> strongEvent = event.promote();

    if (strongEvent != 0) {
        sp<IAfTrackBase> ptr =
                std::any_cast<const wp<IAfTrackBase>>(strongEvent->cookie()).promote();
        if (ptr != nullptr) {
            // TODO(b/291317898) handleSyncStartEvent is in IAfTrackBase not IAfRecordTrack.
            ptr->handleSyncStartEvent(strongEvent);
        }
    }
}

bool RecordThread::stop(IAfRecordTrack* recordTrack) {
    ALOGV("RecordThread::stop");
    audio_utils::unique_lock _l(mutex());
    // if we're invalid, we can't be on the ActiveTracks.
    if (mActiveTracks.indexOf(recordTrack) < 0 || recordTrack->state() == IAfTrackBase::PAUSING) {
        return false;
    }
    // note that threadLoop may still be processing the track at this point [without lock]
    recordTrack->setState(IAfTrackBase::PAUSING);

    // NOTE: Waiting here is important to keep stop synchronous.
    // This is needed for proper patchRecord peer release.
    while (recordTrack->state() == IAfTrackBase::PAUSING && !recordTrack->isInvalid()) {
        mWaitWorkCV.notify_all(); // signal thread to stop
        mStartStopCV.wait(_l, getTid());
    }

    if (recordTrack->state() == IAfTrackBase::PAUSED) { // successful stop
        ALOGV("Record stopped OK");
        return true;
    }

    // don't handle anything - we've been invalidated or restarted and in a different state
    ALOGW_IF("%s(%d): unsynchronized stop, state: %d",
            __func__, recordTrack->id(), recordTrack->state());
    return false;
}

bool RecordThread::isValidSyncEvent(const sp<SyncEvent>& /* event */) const
{
    return false;
}

status_t RecordThread::setSyncEvent(const sp<SyncEvent>& /* event */)
{
#if 0   // This branch is currently dead code, but is preserved in case it will be needed in future
    if (!isValidSyncEvent(event)) {
        return BAD_VALUE;
    }

    audio_session_t eventSession = event->triggerSession();
    status_t ret = NAME_NOT_FOUND;

    audio_utils::lock_guard _l(mutex());

    for (size_t i = 0; i < mTracks.size(); i++) {
        sp<IAfRecordTrack> track = mTracks[i];
        if (eventSession == track->sessionId()) {
            (void) track->setSyncEvent(event);
            ret = NO_ERROR;
        }
    }
    return ret;
#else
    return BAD_VALUE;
#endif
}

status_t RecordThread::getActiveMicrophones(
        std::vector<media::MicrophoneInfoFw>* activeMicrophones) const
{
    ALOGV("RecordThread::getActiveMicrophones");
     audio_utils::lock_guard _l(mutex());
    if (!isStreamInitialized()) {
        return NO_INIT;
    }
    status_t status = mInput->stream->getActiveMicrophones(activeMicrophones);
    return status;
}

status_t RecordThread::setPreferredMicrophoneDirection(
            audio_microphone_direction_t direction)
{
    ALOGV("setPreferredMicrophoneDirection(%d)", direction);
     audio_utils::lock_guard _l(mutex());
    if (!isStreamInitialized()) {
        return NO_INIT;
    }
    return mInput->stream->setPreferredMicrophoneDirection(direction);
}

status_t RecordThread::setPreferredMicrophoneFieldDimension(float zoom)
{
    ALOGV("setPreferredMicrophoneFieldDimension(%f)", zoom);
     audio_utils::lock_guard _l(mutex());
    if (!isStreamInitialized()) {
        return NO_INIT;
    }
    return mInput->stream->setPreferredMicrophoneFieldDimension(zoom);
}

status_t RecordThread::shareAudioHistory(
        const std::string& sharedAudioPackageName, audio_session_t sharedSessionId,
        int64_t sharedAudioStartMs) {
     audio_utils::lock_guard _l(mutex());
    return shareAudioHistory_l(sharedAudioPackageName, sharedSessionId, sharedAudioStartMs);
}

status_t RecordThread::shareAudioHistory_l(
        const std::string& sharedAudioPackageName, audio_session_t sharedSessionId,
        int64_t sharedAudioStartMs) {

    if ((hasAudioSession_l(sharedSessionId) & ThreadBase::TRACK_SESSION) == 0) {
        return BAD_VALUE;
    }

    if (sharedAudioStartMs < 0
        || sharedAudioStartMs > INT64_MAX / mSampleRate) {
        return BAD_VALUE;
    }

    // Current implementation of the input resampling buffer wraps around indexes at 32 bit.
    // As we cannot detect more than one wraparound, only accept values up current write position
    // after one wraparound
    // We assume recent wraparounds on mRsmpInRear only given it is unlikely that the requesting
    // app waits several hours after the start time was computed.
    int64_t sharedAudioStartFrames = sharedAudioStartMs * mSampleRate / 1000;
    const int32_t sharedOffset = audio_utils::safe_sub_overflow(mRsmpInRear,
          (int32_t)sharedAudioStartFrames);
    // Bring the start frame position within the input buffer to match the documented
    // "best effort" behavior of the API.
    if (sharedOffset < 0) {
        sharedAudioStartFrames = mRsmpInRear;
    } else if (sharedOffset > static_cast<signed>(mRsmpInFrames)) {
        sharedAudioStartFrames =
                audio_utils::safe_sub_overflow(mRsmpInRear, (int32_t)mRsmpInFrames);
    }

    mSharedAudioPackageName = sharedAudioPackageName;
    if (mSharedAudioPackageName.empty()) {
        resetAudioHistory_l();
    } else {
        mSharedAudioSessionId = sharedSessionId;
        mSharedAudioStartFrames = (int32_t)sharedAudioStartFrames;
    }
    return NO_ERROR;
}

void RecordThread::resetAudioHistory_l() {
    mSharedAudioSessionId = AUDIO_SESSION_NONE;
    mSharedAudioStartFrames = -1;
    mSharedAudioPackageName = "";
}

ThreadBase::MetadataUpdate RecordThread::updateMetadata_l()
{
    if (!isStreamInitialized() || !mActiveTracks.readAndClearHasChanged()) {
        return {}; // nothing to do
    }
    StreamInHalInterface::SinkMetadata metadata;
    auto backInserter = std::back_inserter(metadata.tracks);
    for (const sp<IAfRecordTrack>& track : mActiveTracks) {
        track->copyMetadataTo(backInserter);
    }
    mInput->stream->updateSinkMetadata(metadata);
    MetadataUpdate change;
    change.recordMetadataUpdate = metadata.tracks;
    return change;
}

// destroyTrack_l() must be called with ThreadBase::mutex() held
void RecordThread::destroyTrack_l(const sp<IAfRecordTrack>& track)
{
    track->terminate();
    track->setState(IAfTrackBase::STOPPED);

    // active tracks are removed by threadLoop()
    if (mActiveTracks.indexOf(track) < 0) {
        removeTrack_l(track);
    }
}

void RecordThread::removeTrack_l(const sp<IAfRecordTrack>& track)
{
    String8 result;
    track->appendDump(result, false /* active */);
    mLocalLog.log("removeTrack_l (%p) %s", track.get(), result.c_str());

    mTracks.remove(track);
    // need anything related to effects here?
    if (track->isFastTrack()) {
        ALOG_ASSERT(!mFastTrackAvail);
        mFastTrackAvail = true;
    }
}

void RecordThread::dumpInternals_l(int fd, const Vector<String16>& /* args */)
{
    AudioStreamIn *input = mInput;
    audio_input_flags_t flags = input != NULL ? input->flags : AUDIO_INPUT_FLAG_NONE;
    dprintf(fd, "  AudioStreamIn: %p flags %#x (%s)\n",
            input, flags, toString(flags).c_str());
    dprintf(fd, "  Frames read: %lld\n", (long long)mFramesRead);
    if (mActiveTracks.isEmpty()) {
        dprintf(fd, "  No active record clients\n");
    }

    if (input != nullptr) {
        dprintf(fd, "  Hal stream dump:\n");
        (void)input->stream->dump(fd);
    }

    dprintf(fd, "  Fast capture thread: %s\n", hasFastCapture() ? "yes" : "no");
    dprintf(fd, "  Fast track available: %s\n", mFastTrackAvail ? "yes" : "no");

    // Make a non-atomic copy of fast capture dump state so it won't change underneath us
    // while we are dumping it.  It may be inconsistent, but it won't mutate!
    // This is a large object so we place it on the heap.
    // FIXME 25972958: Need an intelligent copy constructor that does not touch unused pages.
    const std::unique_ptr<FastCaptureDumpState> copy =
            std::make_unique<FastCaptureDumpState>(mFastCaptureDumpState);
    copy->dump(fd);
}

void RecordThread::dumpTracks_l(int fd, const Vector<String16>& /* args */)
{
    String8 result;
    size_t numtracks = mTracks.size();
    size_t numactive = mActiveTracks.size();
    size_t numactiveseen = 0;
    dprintf(fd, "  %zu Tracks", numtracks);
    const char *prefix = "    ";
    if (numtracks) {
        dprintf(fd, " of which %zu are active\n", numactive);
        result.append(prefix);
        mTracks[0]->appendDumpHeader(result);
        for (size_t i = 0; i < numtracks ; ++i) {
            sp<IAfRecordTrack> track = mTracks[i];
            if (track != 0) {
                bool active = mActiveTracks.indexOf(track) >= 0;
                if (active) {
                    numactiveseen++;
                }
                result.append(prefix);
                track->appendDump(result, active);
            }
        }
    } else {
        dprintf(fd, "\n");
    }

    if (numactiveseen != numactive) {
        result.append("  The following tracks are in the active list but"
                " not in the track list\n");
        result.append(prefix);
        mActiveTracks[0]->appendDumpHeader(result);
        for (size_t i = 0; i < numactive; ++i) {
            sp<IAfRecordTrack> track = mActiveTracks[i];
            if (mTracks.indexOf(track) < 0) {
                result.append(prefix);
                track->appendDump(result, true /* active */);
            }
        }

    }
    write(fd, result.c_str(), result.size());
}

void RecordThread::setRecordSilenced(audio_port_handle_t portId, bool silenced)
{
    audio_utils::lock_guard _l(mutex());
    for (size_t i = 0; i < mTracks.size() ; i++) {
        sp<IAfRecordTrack> track = mTracks[i];
        if (track != 0 && track->portId() == portId) {
            track->setSilenced(silenced);
        }
    }
}

// --------------------------------------------------------------------------------------
//              DirectRecordThread
// --------------------------------------------------------------------------------------

DirectRecordThread::DirectRecordThread(const sp<IAfThreadCallback>& afThreadCallback,
                                     AudioStreamIn* input, audio_io_handle_t id, bool systemReady)
    : RecordThread(afThreadCallback, DIRECT_RECORD, input, id, systemReady) {
    ALOGD("%s:", __func__);
}

DirectRecordThread::~DirectRecordThread() {}

void ResamplerBufferProvider::reset()
{
    const auto threadBase = mRecordTrack->thread().promote();
    auto* const recordThread = static_cast<RecordThread *>(threadBase->asIAfRecordThread().get());
    mRsmpInUnrel = 0;
    const int32_t rear = recordThread->mRsmpInRear;
    ssize_t deltaFrames = 0;
    if (mRecordTrack->startFrames() >= 0) {
        int32_t startFrames = mRecordTrack->startFrames();
        // Accept a recent wraparound of mRsmpInRear
        if (startFrames <= rear) {
            deltaFrames = rear - startFrames;
        } else {
            deltaFrames = (int32_t)((int64_t)rear + UINT32_MAX + 1 - startFrames);
        }
        // start frame cannot be further in the past than start of resampling buffer
        if ((size_t) deltaFrames > recordThread->mRsmpInFrames) {
            deltaFrames = recordThread->mRsmpInFrames;
        }
    }
    mRsmpInFront = audio_utils::safe_sub_overflow(rear, static_cast<int32_t>(deltaFrames));
}

void ResamplerBufferProvider::sync(
        size_t *framesAvailable, bool *hasOverrun)
{
    const auto threadBase = mRecordTrack->thread().promote();
    auto* const recordThread = static_cast<RecordThread *>(threadBase->asIAfRecordThread().get());
    const int32_t rear = recordThread->mRsmpInRear;
    const int32_t front = mRsmpInFront;
    const ssize_t filled = audio_utils::safe_sub_overflow(rear, front);

    size_t framesIn;
    bool overrun = false;
    if (filled < 0) {
        // should not happen, but treat like a massive overrun and re-sync
        framesIn = 0;
        mRsmpInFront = rear;
        overrun = true;
    } else if ((size_t) filled <= recordThread->mRsmpInFrames) {
        framesIn = (size_t) filled;
    } else {
        // client is not keeping up with server, but give it latest data
        framesIn = recordThread->mRsmpInFrames;
        mRsmpInFront = /* front = */ audio_utils::safe_sub_overflow(
                rear, static_cast<int32_t>(framesIn));
        overrun = true;
    }
    if (framesAvailable != NULL) {
        *framesAvailable = framesIn;
    }
    if (hasOverrun != NULL) {
        *hasOverrun = overrun;
    }
}

// AudioBufferProvider interface
status_t ResamplerBufferProvider::getNextBuffer(
        AudioBufferProvider::Buffer* buffer)
{
    const auto threadBase = mRecordTrack->thread().promote();
    if (threadBase == 0) {
        buffer->frameCount = 0;
        buffer->raw = NULL;
        return NOT_ENOUGH_DATA;
    }
    auto* const recordThread = static_cast<RecordThread *>(threadBase->asIAfRecordThread().get());
    int32_t rear = recordThread->mRsmpInRear;
    int32_t front = mRsmpInFront;
    ssize_t filled = audio_utils::safe_sub_overflow(rear, front);
    // FIXME should not be P2 (don't want to increase latency)
    // FIXME if client not keeping up, discard
    LOG_ALWAYS_FATAL_IF(!(0 <= filled && (size_t) filled <= recordThread->mRsmpInFrames));
    // 'filled' may be non-contiguous, so return only the first contiguous chunk

    front &= recordThread->mRsmpInFramesP2 - 1;
    size_t part1 = recordThread->mRsmpInFramesP2 - front;
    if (part1 > (size_t) filled) {
        part1 = filled;
    }
    size_t ask = buffer->frameCount;
    ALOG_ASSERT(ask > 0);
    if (part1 > ask) {
        part1 = ask;
    }
    if (part1 == 0) {
        // out of data is fine since the resampler will return a short-count.
        buffer->raw = NULL;
        buffer->frameCount = 0;
        mRsmpInUnrel = 0;
        return NOT_ENOUGH_DATA;
    }

    buffer->raw = (uint8_t*)recordThread->mRsmpInBuffer + front * recordThread->mFrameSize;
    buffer->frameCount = part1;
    mRsmpInUnrel = part1;
    return NO_ERROR;
}

// AudioBufferProvider interface
void ResamplerBufferProvider::releaseBuffer(
        AudioBufferProvider::Buffer* buffer)
{
    int32_t stepCount = static_cast<int32_t>(buffer->frameCount);
    if (stepCount == 0) {
        return;
    }
    ALOG_ASSERT(stepCount <= (int32_t)mRsmpInUnrel);
    mRsmpInUnrel -= stepCount;
    mRsmpInFront = audio_utils::safe_add_overflow(mRsmpInFront, stepCount);
    buffer->raw = NULL;
    buffer->frameCount = 0;
}

void RecordThread::checkBtNrec()
{
    audio_utils::lock_guard _l(mutex());
    checkBtNrec_l();
}

void RecordThread::checkBtNrec_l()
{
    // disable AEC and NS if the device is a BT SCO headset supporting those
    // pre processings
    bool suspend = audio_is_bluetooth_sco_device(inDeviceType_l()) &&
                        mAfThreadCallback->btNrecIsOff();
    if (mBtNrecSuspended.exchange(suspend) != suspend) {
        for (size_t i = 0; i < mEffectChains.size(); i++) {
            setEffectSuspended_l(FX_IID_AEC, suspend, mEffectChains[i]->sessionId());
            setEffectSuspended_l(FX_IID_NS, suspend, mEffectChains[i]->sessionId());
        }
    }
}


bool RecordThread::checkForNewParameter_l(const String8& keyValuePair,
                                                        status_t& status)
{
    bool reconfig = false;

    status = NO_ERROR;

    audio_format_t reqFormat = mFormat;
    uint32_t samplingRate = mSampleRate;
    // TODO this may change if we want to support capture from HDMI PCM multi channel (e.g on TVs).
    [[maybe_unused]] audio_channel_mask_t channelMask =
                                audio_channel_in_mask_from_count(mChannelCount);

    AudioParameter param = AudioParameter(keyValuePair);
    int value;

    // scope for AutoPark extends to end of method
    AutoPark<FastCapture> park(mFastCapture);

    // TODO Investigate when this code runs. Check with audio policy when a sample rate and
    //      channel count change can be requested. Do we mandate the first client defines the
    //      HAL sampling rate and channel count or do we allow changes on the fly?
    if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
        samplingRate = value;
        reconfig = true;
    }
    if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
        if (!audio_is_linear_pcm((audio_format_t) value)) {
            status = BAD_VALUE;
        } else {
            reqFormat = (audio_format_t) value;
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
        audio_channel_mask_t mask = (audio_channel_mask_t) value;
        if (!audio_is_input_channel(mask) ||
                audio_channel_count_from_in_mask(mask) > FCC_LIMIT) {
            status = BAD_VALUE;
        } else {
            channelMask = mask;
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
        // do not accept frame count changes if tracks are open as the track buffer
        // size depends on frame count and correct behavior would not be guaranteed
        // if frame count is changed after track creation
        if (mActiveTracks.size() > 0) {
            status = INVALID_OPERATION;
        } else {
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        LOG_FATAL("Should not set routing device in RecordThread");
    }
    if (param.getInt(String8(AudioParameter::keyInputSource), value) == NO_ERROR &&
            mAudioSource != (audio_source_t)value) {
        LOG_FATAL("Should not set audio source in RecordThread");
    }

    if (status == NO_ERROR) {
        status = mInput->stream->setParameters(keyValuePair);
        if (status == INVALID_OPERATION) {
            inputStandBy();
            status = mInput->stream->setParameters(keyValuePair);
        }
        if (reconfig) {
            if (status == BAD_VALUE) {
                audio_config_base_t config = AUDIO_CONFIG_BASE_INITIALIZER;
                if (mInput->stream->getAudioProperties(&config) == OK &&
                        audio_is_linear_pcm(config.format) && audio_is_linear_pcm(reqFormat) &&
                        config.sample_rate <= (AUDIO_RESAMPLER_DOWN_RATIO_MAX * samplingRate) &&
                        audio_channel_count_from_in_mask(config.channel_mask) <= FCC_LIMIT) {
                    status = NO_ERROR;
                }
            }
            if (status == NO_ERROR) {
                readInputParameters_l();
                sendIoConfigEvent_l(AUDIO_INPUT_CONFIG_CHANGED);
            }
        }
    }

    return reconfig;
}

String8 RecordThread::getParameters(const String8& keys)
{
    audio_utils::lock_guard _l(mutex());
    if (initCheck() == NO_ERROR) {
        String8 out_s8;
        if (mInput->stream->getParameters(keys, &out_s8) == OK) {
            return out_s8;
        }
    }
    return {};
}

void RecordThread::ioConfigChanged_l(audio_io_config_event_t event, pid_t pid,
                                                 audio_port_handle_t portId) {
    sp<AudioIoDescriptor> desc;
    switch (event) {
    case AUDIO_INPUT_OPENED:
    case AUDIO_INPUT_REGISTERED:
    case AUDIO_INPUT_CONFIG_CHANGED:
        desc = sp<AudioIoDescriptor>::make(mId, mPatch, true /*isInput*/,
                mSampleRate, mFormat, mChannelMask, mFrameCount, mFrameCount);
        break;
    case AUDIO_CLIENT_STARTED:
        desc = sp<AudioIoDescriptor>::make(mId, mPatch, portId);
        break;
    case AUDIO_INPUT_CLOSED:
    default:
        desc = sp<AudioIoDescriptor>::make(mId);
        break;
    }
    mAfThreadCallback->ioConfigChanged_l(event, desc, pid);
}

void RecordThread::readInputParameters_l()
{
    const audio_config_base_t audioConfig = mInput->getAudioProperties();
    mSampleRate = audioConfig.sample_rate;
    mChannelMask = audioConfig.channel_mask;
    if (!audio_is_input_channel(mChannelMask)) {
        LOG_ALWAYS_FATAL("Channel mask %#x not valid for input", mChannelMask);
    }

    mChannelCount = audio_channel_count_from_in_mask(mChannelMask);

    // Get actual HAL format.
    status_t result = mInput->stream->getAudioProperties(nullptr, nullptr, &mHALFormat);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error when retrieving input stream format: %d", result);
    // Get format from the shim, which will be different than the HAL format
    // if recording compressed audio from IEC61937 wrapped sources.
    mFormat = audioConfig.format;
    if (!audio_is_valid_format(mFormat)) {
        LOG_ALWAYS_FATAL("Format %#x not valid for input", mFormat);
    }
    if (audio_is_linear_pcm(mFormat)) {
        LOG_ALWAYS_FATAL_IF(mChannelCount > FCC_LIMIT, "HAL channel count %d > %d",
                mChannelCount, FCC_LIMIT);
    } else {
        // Can have more that FCC_LIMIT channels in encoded streams.
        ALOGI("HAL format %#x is not linear pcm", mFormat);
    }
    mFrameSize = mInput->getFrameSize();
    LOG_ALWAYS_FATAL_IF(mFrameSize <= 0, "Error frame size was %zu but must be greater than zero",
            mFrameSize);
    result = mInput->stream->getBufferSize(&mBufferSize);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error retrieving buffer size from HAL: %d", result);
    mFrameCount = mBufferSize / mFrameSize;
    ALOGV("%p RecordThread params: mChannelCount=%u, mFormat=%#x, mFrameSize=%zu, "
            "mBufferSize=%zu, mFrameCount=%zu",
            this, mChannelCount, mFormat, mFrameSize, mBufferSize, mFrameCount);

    // mRsmpInFrames must be 0 before calling resizeInputBuffer_l for the first time
    mRsmpInFrames = 0;
    resizeInputBuffer_l(0 /*maxSharedAudioHistoryMs*/);

    // AudioRecord mSampleRate and mChannelCount are constant due to AudioRecord API constraints.
    // But if thread's mSampleRate or mChannelCount changes, how will that affect active tracks?

    audio_input_flags_t flags = mInput->flags;
    mediametrics::LogItem item(mThreadMetrics.getMetricsId());
    item.set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_READPARAMETERS)
        .set(AMEDIAMETRICS_PROP_ENCODING, IAfThreadBase::formatToString(mFormat).c_str())
        .set(AMEDIAMETRICS_PROP_FLAGS, toString(flags).c_str())
        .set(AMEDIAMETRICS_PROP_SAMPLERATE, (int32_t)mSampleRate)
        .set(AMEDIAMETRICS_PROP_CHANNELMASK, (int32_t)mChannelMask)
        .set(AMEDIAMETRICS_PROP_CHANNELCOUNT, (int32_t)mChannelCount)
        .set(AMEDIAMETRICS_PROP_FRAMECOUNT, (int32_t)mFrameCount)
        .record();
}

uint32_t RecordThread::getInputFramesLost() const
{
    audio_utils::lock_guard _l(mutex());
    uint32_t result;
    if (initCheck() == NO_ERROR && mInput->stream->getInputFramesLost(&result) == OK) {
        return result;
    }
    return 0;
}

KeyedVector<audio_session_t, bool> RecordThread::sessionIds() const
{
    KeyedVector<audio_session_t, bool> ids;
    audio_utils::lock_guard _l(mutex());
    for (size_t j = 0; j < mTracks.size(); ++j) {
        sp<IAfRecordTrack> track = mTracks[j];
        audio_session_t sessionId = track->sessionId();
        if (ids.indexOfKey(sessionId) < 0) {
            ids.add(sessionId, true);
        }
    }
    return ids;
}

AudioStreamIn* RecordThread::clearInput()
{
    audio_utils::lock_guard _l(mutex());
    AudioStreamIn *input = mInput;
    mInput = NULL;
    mInputSource.clear();
    return input;
}

// this method must always be called either with ThreadBase mutex() held or inside the thread loop
sp<StreamHalInterface> RecordThread::stream() const
{
    if (mInput == NULL) {
        return NULL;
    }
    return mInput->stream;
}

status_t RecordThread::addEffectChain_l(const sp<IAfEffectChain>& chain)
{
    ALOGV("addEffectChain_l() %p on thread %p", chain.get(), this);
    chain->setThread(this);
    chain->setInBuffer(NULL);
    chain->setOutBuffer(NULL);

    checkSuspendOnAddEffectChain_l(chain);

    // make sure enabled pre processing effects state is communicated to the HAL as we
    // just moved them to a new input stream.
    chain->syncHalEffectsState_l();

    mEffectChains.add(chain);

    return NO_ERROR;
}

size_t RecordThread::removeEffectChain_l(const sp<IAfEffectChain>& chain)
{
    ALOGV("removeEffectChain_l() %p from thread %p", chain.get(), this);

    for (size_t i = 0; i < mEffectChains.size(); i++) {
        if (chain == mEffectChains[i]) {
            mEffectChains.removeAt(i);
            break;
        }
    }
    return mEffectChains.size();
}

status_t RecordThread::createAudioPatch_l(const struct audio_patch* patch,
                                                          audio_patch_handle_t *handle)
{
    status_t status = NO_ERROR;

    // store new device and send to effects
    mInDeviceTypeAddr.mType = patch->sources[0].ext.device.type;
    mInDeviceTypeAddr.setAddress(patch->sources[0].ext.device.address);
    audio_port_handle_t deviceId = patch->sources[0].id;
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        mEffectChains[i]->setInputDevice_l(inDeviceTypeAddr());
    }

    checkBtNrec_l();

    // store new source and send to effects
    if (mAudioSource != patch->sinks[0].ext.mix.usecase.source) {
        mAudioSource = patch->sinks[0].ext.mix.usecase.source;
        for (size_t i = 0; i < mEffectChains.size(); i++) {
            mEffectChains[i]->setAudioSource_l(mAudioSource);
        }
    }

    if (mInput->audioHwDev->supportsAudioPatches()) {
        sp<DeviceHalInterface> hwDevice = mInput->audioHwDev->hwDevice();
        status = hwDevice->createAudioPatch(patch->num_sources,
                                            patch->sources,
                                            patch->num_sinks,
                                            patch->sinks,
                                            handle);
    } else {
        status = mInput->stream->legacyCreateAudioPatch(patch->sources[0],
                                                        patch->sinks[0].ext.mix.usecase.source,
                                                        patch->sources[0].ext.device.type);
        *handle = AUDIO_PATCH_HANDLE_NONE;
    }

    if ((mPatch.num_sources == 0) || (mPatch.sources[0].id != deviceId)) {
        sendIoConfigEvent_l(AUDIO_INPUT_CONFIG_CHANGED);
        mPatch = *patch;
    }

    const std::string pathSourcesAsString = patchSourcesToString(patch);
    mThreadMetrics.logEndInterval();
    mThreadMetrics.logCreatePatch(pathSourcesAsString, /* outDevices */ {});
    mThreadMetrics.logBeginInterval();
    // also dispatch to active AudioRecords
    for (const auto &track : mActiveTracks) {
        track->logEndInterval();
        track->logBeginInterval(pathSourcesAsString);
    }
    // Force meteadata update after a route change
    mActiveTracks.setHasChanged();

    return status;
}

status_t RecordThread::releaseAudioPatch_l(const audio_patch_handle_t handle)
{
    status_t status = NO_ERROR;

    mPatch = audio_patch{};
    mInDeviceTypeAddr.reset();

    if (mInput->audioHwDev->supportsAudioPatches()) {
        sp<DeviceHalInterface> hwDevice = mInput->audioHwDev->hwDevice();
        status = hwDevice->releaseAudioPatch(handle);
    } else {
        status = mInput->stream->legacyReleaseAudioPatch();
    }
    // Force meteadata update after a route change
    mActiveTracks.setHasChanged();

    return status;
}

void RecordThread::updateOutDevices(const DeviceDescriptorBaseVector& outDevices)
{
    audio_utils::lock_guard _l(mutex());
    mOutDevices = outDevices;
    mOutDeviceTypeAddrs = deviceTypeAddrsFromDescriptors(mOutDevices);
    for (size_t i = 0; i < mEffectChains.size(); i++) {
        mEffectChains[i]->setDevices_l(outDeviceTypeAddrs());
    }
}

int32_t RecordThread::getOldestFront_l()
{
    if (mTracks.size() == 0) {
        return mRsmpInRear;
    }
    int32_t oldestFront = mRsmpInRear;
    int32_t maxFilled = 0;
    for (size_t i = 0; i < mTracks.size(); i++) {
        int32_t front = mTracks[i]->resamplerBufferProvider()->getFront();
        int32_t filled;
        (void)__builtin_sub_overflow(mRsmpInRear, front, &filled);
        if (filled > maxFilled) {
            oldestFront = front;
            maxFilled = filled;
        }
    }
    if (maxFilled > static_cast<signed>(mRsmpInFrames)) {
        (void)__builtin_sub_overflow(mRsmpInRear, mRsmpInFrames, &oldestFront);
    }
    return oldestFront;
}

void RecordThread::updateFronts_l(int32_t offset)
{
    if (offset == 0) {
        return;
    }
    for (size_t i = 0; i < mTracks.size(); i++) {
        int32_t front = mTracks[i]->resamplerBufferProvider()->getFront();
        front = audio_utils::safe_sub_overflow(front, offset);
        mTracks[i]->resamplerBufferProvider()->setFront(front);
    }
}

void RecordThread::resizeInputBuffer_l(int32_t maxSharedAudioHistoryMs)
{
    // This is the formula for calculating the temporary buffer size.
    // With 7 HAL buffers, we can guarantee ability to down-sample the input by ratio of 6:1 to
    // 1 full output buffer, regardless of the alignment of the available input.
    // The value is somewhat arbitrary, and could probably be even larger.
    // A larger value should allow more old data to be read after a track calls start(),
    // without increasing latency.
    //
    // Note this is independent of the maximum downsampling ratio permitted for capture.
    size_t minRsmpInFrames = mFrameCount * 7;

    // maxSharedAudioHistoryMs != 0 indicates a request to possibly make some part of the audio
    // capture history available to another client using the same session ID:
    // dimension the resampler input buffer accordingly.

    // Get oldest client read position:  getOldestFront_l() must be called before altering
    // mRsmpInRear, or mRsmpInFrames
    int32_t previousFront = getOldestFront_l();
    size_t previousRsmpInFramesP2 = mRsmpInFramesP2;
    int32_t previousRear = mRsmpInRear;
    mRsmpInRear = 0;

    ALOG_ASSERT(maxSharedAudioHistoryMs >= 0
            && maxSharedAudioHistoryMs <= kMaxSharedAudioHistoryMs,
            "resizeInputBuffer_l() called with invalid max shared history %d",
            maxSharedAudioHistoryMs);
    if (maxSharedAudioHistoryMs != 0) {
        // resizeInputBuffer_l should never be called with a non zero shared history if the
        // buffer was not already allocated
        ALOG_ASSERT(mRsmpInBuffer != nullptr && mRsmpInFrames != 0,
                "resizeInputBuffer_l() called with shared history and unallocated buffer");
        size_t rsmpInFrames = (size_t)maxSharedAudioHistoryMs * mSampleRate / 1000;
        // never reduce resampler input buffer size
        if (rsmpInFrames <= mRsmpInFrames) {
            return;
        }
        mRsmpInFrames = rsmpInFrames;
    }
    mMaxSharedAudioHistoryMs = maxSharedAudioHistoryMs;
    // Note: mRsmpInFrames is 0 when called with maxSharedAudioHistoryMs equals to 0 so it is always
    // initialized
    if (mRsmpInFrames < minRsmpInFrames) {
        mRsmpInFrames = minRsmpInFrames;
    }
    mRsmpInFramesP2 = roundup(mRsmpInFrames);

    // TODO optimize audio capture buffer sizes ...
    // Here we calculate the size of the sliding buffer used as a source
    // for resampling.  mRsmpInFramesP2 is currently roundup(mFrameCount * 7).
    // For current HAL frame counts, this is usually 2048 = 40 ms.  It would
    // be better to have it derived from the pipe depth in the long term.
    // The current value is higher than necessary.  However it should not add to latency.

    // Over-allocate beyond mRsmpInFramesP2 to permit a HAL read past end of buffer
    mRsmpInFramesOA = mRsmpInFramesP2 + mFrameCount - 1;

    void *rsmpInBuffer;
    (void)posix_memalign(&rsmpInBuffer, 32, mRsmpInFramesOA * mFrameSize);
    // if posix_memalign fails, will segv here.
    memset(rsmpInBuffer, 0, mRsmpInFramesOA * mFrameSize);

    // Copy audio history if any from old buffer before freeing it
    if (previousRear != 0) {
        ALOG_ASSERT(mRsmpInBuffer != nullptr,
                "resizeInputBuffer_l() called with null buffer but frames already read from HAL");

        ssize_t unread = audio_utils::safe_sub_overflow(previousRear, previousFront);
        previousFront &= previousRsmpInFramesP2 - 1;
        size_t part1 = previousRsmpInFramesP2 - previousFront;
        if (part1 > (size_t) unread) {
            part1 = unread;
        }
        if (part1 != 0) {
            memcpy(rsmpInBuffer, (const uint8_t*)mRsmpInBuffer + previousFront * mFrameSize,
                   part1 * mFrameSize);
            mRsmpInRear = part1;
            part1 = unread - part1;
            if (part1 != 0) {
                memcpy((uint8_t*)rsmpInBuffer + mRsmpInRear * mFrameSize,
                       (const uint8_t*)mRsmpInBuffer, part1 * mFrameSize);
                mRsmpInRear += part1;
            }
        }
        // Update front for all clients according to new rear
        updateFronts_l(audio_utils::safe_sub_overflow(previousRear, mRsmpInRear));
    } else {
        mRsmpInRear = 0;
    }
    free(mRsmpInBuffer);
    mRsmpInBuffer = rsmpInBuffer;
}

void RecordThread::addPatchTrack(const sp<IAfPatchRecord>& record)
{
    audio_utils::lock_guard _l(mutex());
    mTracks.add(record);
    if (record->getSource()) {
        mSource = record->getSource();
    }
}

void RecordThread::deletePatchTrack(const sp<IAfPatchRecord>& record)
{
    audio_utils::lock_guard _l(mutex());
    if (mSource == record->getSource()) {
        mSource = mInput;
    }
    destroyTrack_l(record);
}

void RecordThread::toAudioPortConfig(struct audio_port_config* config)
{
    ThreadBase::toAudioPortConfig(config);
    config->role = AUDIO_PORT_ROLE_SINK;
    config->ext.mix.hw_module = mInput->audioHwDev->handle();
    config->ext.mix.usecase.source = mAudioSource;
    if (mInput && mInput->flags != AUDIO_INPUT_FLAG_NONE) {
        config->config_mask |= AUDIO_PORT_CONFIG_FLAGS;
        config->flags.input = mInput->flags;
    }
}

std::string RecordThread::getLocalLogHeader() const {
    using namespace std::literals;
    static constexpr auto indent = "                             "
                                   "                            "sv;
    return std::string{indent}.append(IAfRecordTrack::getLogHeader());
}

// ----------------------------------------------------------------------------
//      Mmap
// ----------------------------------------------------------------------------

// Mmap stream control interface implementation. Each MmapThreadHandle controls one
// MmapPlaybackThread or MmapCaptureThread instance.
class MmapThreadHandle : public MmapStreamInterface {
public:
    explicit MmapThreadHandle(const sp<IAfMmapThread>& thread);
    ~MmapThreadHandle() override;

    // MmapStreamInterface virtuals
    status_t createMmapBuffer(int32_t minSizeFrames,
        struct audio_mmap_buffer_info* info) final;
    status_t getMmapPosition(struct audio_mmap_position* position) final;
    status_t getExternalPosition(uint64_t* position, int64_t* timeNanos) final;
    status_t start(const AudioClient& client,
           const audio_attributes_t* attr, audio_port_handle_t* handle) final;
    status_t stop(audio_port_handle_t handle) final;
    status_t standby() final;
    status_t reportData(const void* buffer, size_t frameCount) final;
private:
    const sp<IAfMmapThread> mThread;
};

/* static */
sp<MmapStreamInterface> IAfMmapThread::createMmapStreamInterfaceAdapter(
        const sp<IAfMmapThread>& mmapThread) {
    return sp<MmapThreadHandle>::make(mmapThread);
}

MmapThreadHandle::MmapThreadHandle(const sp<IAfMmapThread>& thread)
    : mThread(thread)
{
    assert(thread != 0); // thread must start non-null and stay non-null
}

// MmapStreamInterface could be directly implemented by MmapThread excepting this
// special handling on adapter dtor.
MmapThreadHandle::~MmapThreadHandle()
{
    mThread->disconnect();
}

status_t MmapThreadHandle::createMmapBuffer(int32_t minSizeFrames,
                                  struct audio_mmap_buffer_info *info)
{
    return mThread->createMmapBuffer(minSizeFrames, info);
}

status_t MmapThreadHandle::getMmapPosition(struct audio_mmap_position* position)
{
    return mThread->getMmapPosition(position);
}

status_t MmapThreadHandle::getExternalPosition(uint64_t* position,
                                                             int64_t *timeNanos) {
    return mThread->getExternalPosition(position, timeNanos);
}

status_t MmapThreadHandle::start(const AudioClient& client,
        const audio_attributes_t *attr, audio_port_handle_t *handle)
{
    return mThread->start(client, attr, handle);
}

status_t MmapThreadHandle::stop(audio_port_handle_t handle)
{
    return mThread->stop(handle);
}

status_t MmapThreadHandle::standby()
{
    return mThread->standby();
}

status_t MmapThreadHandle::reportData(const void* buffer, size_t frameCount)
{
    return mThread->reportData(buffer, frameCount);
}


MmapThread::MmapThread(
        const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        AudioHwDevice *hwDev, const sp<StreamHalInterface>& stream, bool systemReady, bool isOut)
    : ThreadBase(afThreadCallback, id, (isOut ? MMAP_PLAYBACK : MMAP_CAPTURE), systemReady, isOut),
      mSessionId(AUDIO_SESSION_NONE),
      mPortId(AUDIO_PORT_HANDLE_NONE),
      mHalStream(stream), mHalDevice(hwDev->hwDevice()), mAudioHwDev(hwDev),
      mActiveTracks(&this->mLocalLog),
      mHalVolFloat(-1.0f), // Initialize to illegal value so it always gets set properly later.
      mNoCallbackWarningCount(0)
{
    mStandby = true;
    readHalParameters_l();
}

void MmapThread::onFirstRef()
{
    run(mThreadName, ANDROID_PRIORITY_URGENT_AUDIO);
}

void MmapThread::disconnect()
{
    ActiveTracks<IAfMmapTrack> activeTracks;
    audio_port_handle_t localPortId;
    {
        audio_utils::lock_guard _l(mutex());
        for (const sp<IAfMmapTrack>& t : mActiveTracks) {
            activeTracks.add(t);
        }
        localPortId = mPortId;
        ALOGD("%s: localPortId = %d", __func__, localPortId);
        mPortId = AUDIO_PORT_HANDLE_NONE;
    }
    for (const sp<IAfMmapTrack>& t : activeTracks) {
        ALOGD("%s: t->portId() = %d", __func__, t->portId());
        stop(t->portId());
    }
    // This will decrement references and may cause the destruction of this thread.
    if (isOutput()) {
        AudioSystem::releaseOutput(localPortId);
    } else {
        AudioSystem::releaseInput(localPortId);
    }
}


void MmapThread::configure_l(const audio_attributes_t* attr,
                                                audio_stream_type_t streamType __unused,
                                                audio_session_t sessionId,
                                                const sp<MmapStreamCallback>& callback,
                                                const DeviceIdVector& deviceIds,
                                                audio_port_handle_t portId)
{
    mAttr = *attr;
    mSessionId = sessionId;
    mCallback = callback;
    mDeviceIds = deviceIds;
    mPortId = portId;
}

status_t MmapThread::createMmapBuffer(int32_t minSizeFrames,
                                  struct audio_mmap_buffer_info *info)
{
    audio_utils::lock_guard l(mutex());
    if (mHalStream == 0) {
        return NO_INIT;
    }
    mStandby = true;
    return mHalStream->createMmapBuffer(minSizeFrames, info);
}

status_t MmapThread::getMmapPosition(struct audio_mmap_position* position) const
{
    audio_utils::lock_guard l(mutex());
    if (mHalStream == 0) {
        return NO_INIT;
    }
    return mHalStream->getMmapPosition(position);
}

status_t MmapThread::exitStandby_l()
{
    // The HAL must receive track metadata before starting the stream
    updateMetadata_l();
    status_t ret = mHalStream->start();
    if (ret != NO_ERROR) {
        ALOGE("%s: error mHalStream->start() = %d for first track", __FUNCTION__, ret);
        return ret;
    }
    if (mStandby) {
        mThreadMetrics.logBeginInterval();
        mThreadSnapshot.onBegin();
        mStandby = false;
    }
    return NO_ERROR;
}

status_t MmapThread::start(const AudioClient& client,
                                         const audio_attributes_t *attr,
                                         audio_port_handle_t *handle)
{
    audio_utils::lock_guard l(mutex());
    ALOGV("%s clientUid %d mStandby %d mPortId %d *handle %d", __FUNCTION__,
          client.attributionSource.uid, mStandby, mPortId, *handle);
    if (mHalStream == 0) {
        return NO_INIT;
    }

    status_t ret;

    // For the first track, reuse portId and session allocated when the stream was opened.
    if (*handle == mPortId) {
        acquireWakeLock_l();
        return NO_ERROR;
    }

    audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE;

    audio_io_handle_t io = mId;
    AttributionSourceState adjAttributionSource;
    if (!com::android::media::audio::audioserver_permissions()) {
        adjAttributionSource = afutils::checkAttributionSourcePackage(
                client.attributionSource);
    } else {
        // TODO(b/342475009) validate in oboeservice, and plumb downwards
        auto validatedRes = ValidatedAttributionSourceState::createFromTrustedUidNoPackage(
                    client.attributionSource,
                    mAfThreadCallback->getPermissionProvider()
                );
        if (!validatedRes.has_value()) {
            ALOGE("MMAP client package validation fail: %s",
                    validatedRes.error().toString8().c_str());
            return aidl_utils::statusTFromBinderStatus(validatedRes.error());
        }
        adjAttributionSource = std::move(validatedRes.value()).unwrapInto();
    }

    const auto localSessionId = mSessionId;
    auto localAttr = mAttr;
    float volume = 0.0f;
    bool muted = false;
    if (isOutput()) {
        audio_config_t config = AUDIO_CONFIG_INITIALIZER;
        config.sample_rate = mSampleRate;
        config.channel_mask = mChannelMask;
        config.format = mFormat;
        audio_stream_type_t stream = streamType_l();
        audio_output_flags_t flags =
                (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_MMAP_NOIRQ | AUDIO_OUTPUT_FLAG_DIRECT);
        DeviceIdVector deviceIds = mDeviceIds;
        std::vector<audio_io_handle_t> secondaryOutputs;
        bool isSpatialized;
        bool isBitPerfect;
        mutex().unlock();
        ret = AudioSystem::getOutputForAttr(&localAttr, &io,
                                            localSessionId,
                                            &stream,
                                            adjAttributionSource,
                                            &config,
                                            flags,
                                            &deviceIds,
                                            &portId,
                                            &secondaryOutputs,
                                            &isSpatialized,
                                            &isBitPerfect,
                                            &volume,
                                            &muted);
        mutex().lock();
        mAttr = localAttr;
        ALOGD_IF(!secondaryOutputs.empty(),
                 "MmapThread::start does not support secondary outputs, ignoring them");
    } else {
        audio_config_base_t config;
        config.sample_rate = mSampleRate;
        config.channel_mask = mChannelMask;
        config.format = mFormat;
        audio_port_handle_t deviceId = getFirstDeviceId(mDeviceIds);
        audio_source_t source = AUDIO_SOURCE_DEFAULT;
        mutex().unlock();
        ret = AudioSystem::getInputForAttr(&localAttr, &io,
                                              RECORD_RIID_INVALID,
                                              localSessionId,
                                              adjAttributionSource,
                                              &config,
                                              AUDIO_INPUT_FLAG_MMAP_NOIRQ,
                                              &deviceId,
                                              &portId,
                                              &source);
        mutex().lock();
        // localAttr is const for getInputForAttr.
        localAttr.source = source;
    }
    // APM should not chose a different input or output stream for the same set of attributes
    // and audo configuration
    if (ret != NO_ERROR || io != mId) {
        ALOGE("%s: error getting output or input from APM (error %d, io %d expected io %d)",
              __FUNCTION__, ret, io, mId);
        return BAD_VALUE;
    }

    if (isOutput()) {
        mutex().unlock();
        ret = AudioSystem::startOutput(portId);
        mutex().lock();
    } else {
        {
            // Add the track record before starting input so that the silent status for the
            // client can be cached.
            setClientSilencedState_l(portId, false /*silenced*/);
        }
        mutex().unlock();
        ret = AudioSystem::startInput(portId);
        mutex().lock();
    }

    // abort if start is rejected by audio policy manager
    if (ret != NO_ERROR) {
        ALOGE("%s: error start rejected by AudioPolicyManager = %d", __FUNCTION__, ret);
        if (!mActiveTracks.isEmpty()) {
            mutex().unlock();
            if (isOutput()) {
                AudioSystem::releaseOutput(portId);
            } else {
                AudioSystem::releaseInput(portId);
            }
            mutex().lock();
        } else {
            mHalStream->stop();
        }
        eraseClientSilencedState_l(portId);
        return PERMISSION_DENIED;
    }

    // Given that MmapThread::mAttr is mutable, should a MmapTrack have attributes ?
    sp<IAfMmapTrack> track = IAfMmapTrack::create(
            this, attr == nullptr ? mAttr : *attr, mSampleRate, mFormat,
                                        mChannelMask, mSessionId, isOutput(),
                                        adjAttributionSource,
                                        IPCThreadState::self()->getCallingPid(), portId,
                                        volume, muted);

    // MMAP tracks are only created when they are started, so mark them as Start for the purposes
    // of the IAfTrackBase interface
    track->start();
    if (!isOutput()) {
        track->setSilenced_l(isClientSilenced_l(portId));
    }

    if (isOutput()) {
        // force volume update when a new track is added
        mHalVolFloat = -1.0f;
    } else if (!track->isSilenced_l()) {
        for (const sp<IAfMmapTrack>& t : mActiveTracks) {
            if (t->isSilenced_l()
                    && t->uid() != static_cast<uid_t>(adjAttributionSource.uid)) {
                t->invalidate();
            }
        }
    }

    mActiveTracks.add(track);
    sp<IAfEffectChain> chain = getEffectChain_l(mSessionId);
    if (chain != 0) {
        chain->setStrategy(getStrategyForStream(streamType_l()));
        chain->incTrackCnt();
        chain->incActiveTrackCnt();
    }

    // log to MediaMetrics
    track->logBeginInterval(
            isOutput() ? patchSinksToString(&mPatch) : patchSourcesToString(&mPatch));
    *handle = portId;

    if (mActiveTracks.size() == 1) {
        ret = exitStandby_l();
    }

    broadcast_l();

    ALOGV("%s DONE status %d handle %d stream %p", __FUNCTION__, ret, *handle, mHalStream.get());

    return ret;
}

status_t MmapThread::stop(audio_port_handle_t handle)
{
    ALOGV("%s handle %d", __FUNCTION__, handle);
    audio_utils::lock_guard l(mutex());

    if (mHalStream == 0) {
        return NO_INIT;
    }

    if (handle == mPortId) {
        releaseWakeLock_l();
        return NO_ERROR;
    }

    sp<IAfMmapTrack> track;
    for (const sp<IAfMmapTrack>& t : mActiveTracks) {
        if (handle == t->portId()) {
            track = t;
            break;
        }
    }
    if (track == 0) {
        return BAD_VALUE;
    }

    mActiveTracks.remove(track);
    eraseClientSilencedState_l(track->portId());
    track->stop();

    mutex().unlock();
    if (isOutput()) {
        AudioSystem::stopOutput(track->portId());
        AudioSystem::releaseOutput(track->portId());
    } else {
        AudioSystem::stopInput(track->portId());
        AudioSystem::releaseInput(track->portId());
    }
    mutex().lock();

    sp<IAfEffectChain> chain = getEffectChain_l(track->sessionId());
    if (chain != 0) {
        chain->decActiveTrackCnt();
        chain->decTrackCnt();
    }

    if (mActiveTracks.isEmpty()) {
        mHalStream->stop();
    }

    broadcast_l();

    return NO_ERROR;
}

status_t MmapThread::standby()
NO_THREAD_SAFETY_ANALYSIS  // clang bug
{
    ALOGV("%s", __FUNCTION__);
    audio_utils::lock_guard l_{mutex()};

    if (mHalStream == 0) {
        return NO_INIT;
    }
    if (!mActiveTracks.isEmpty()) {
        return INVALID_OPERATION;
    }
    mHalStream->standby();
    if (!mStandby) {
        mThreadMetrics.logEndInterval();
        mThreadSnapshot.onEnd();
        mStandby = true;
    }
    releaseWakeLock_l();
    return NO_ERROR;
}

status_t MmapThread::reportData(const void* /*buffer*/, size_t /*frameCount*/) {
    // This is a stub implementation. The MmapPlaybackThread overrides this function.
    return INVALID_OPERATION;
}

void MmapThread::readHalParameters_l()
{
    status_t result = mHalStream->getAudioProperties(&mSampleRate, &mChannelMask, &mHALFormat);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error retrieving audio properties from HAL: %d", result);
    mFormat = mHALFormat;
    LOG_ALWAYS_FATAL_IF(!audio_is_linear_pcm(mFormat), "HAL format %#x is not linear pcm", mFormat);
    result = mHalStream->getFrameSize(&mFrameSize);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error retrieving frame size from HAL: %d", result);
    LOG_ALWAYS_FATAL_IF(mFrameSize <= 0, "Error frame size was %zu but must be greater than zero",
            mFrameSize);
    result = mHalStream->getBufferSize(&mBufferSize);
    LOG_ALWAYS_FATAL_IF(result != OK, "Error retrieving buffer size from HAL: %d", result);
    mFrameCount = mBufferSize / mFrameSize;

    // TODO: make a readHalParameters call?
    mediametrics::LogItem item(mThreadMetrics.getMetricsId());
    item.set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_READPARAMETERS)
        .set(AMEDIAMETRICS_PROP_ENCODING, IAfThreadBase::formatToString(mFormat).c_str())
        .set(AMEDIAMETRICS_PROP_SAMPLERATE, (int32_t)mSampleRate)
        .set(AMEDIAMETRICS_PROP_CHANNELMASK, (int32_t)mChannelMask)
        .set(AMEDIAMETRICS_PROP_CHANNELCOUNT, (int32_t)mChannelCount)
        .set(AMEDIAMETRICS_PROP_FRAMECOUNT, (int32_t)mFrameCount)
        /*
        .set(AMEDIAMETRICS_PROP_FLAGS, toString(flags).c_str())
        .set(AMEDIAMETRICS_PROP_PREFIX_HAPTIC AMEDIAMETRICS_PROP_CHANNELMASK,
                (int32_t)mHapticChannelMask)
        .set(AMEDIAMETRICS_PROP_PREFIX_HAPTIC AMEDIAMETRICS_PROP_CHANNELCOUNT,
                (int32_t)mHapticChannelCount)
        */
        .set(AMEDIAMETRICS_PROP_PREFIX_HAL    AMEDIAMETRICS_PROP_ENCODING,
                IAfThreadBase::formatToString(mHALFormat).c_str())
        .set(AMEDIAMETRICS_PROP_PREFIX_HAL    AMEDIAMETRICS_PROP_FRAMECOUNT,
                (int32_t)mFrameCount) // sic - added HAL
        .record();
}

bool MmapThread::threadLoop()
{
    {
        audio_utils::unique_lock _l(mutex());
        checkSilentMode_l();
    }

    const String8 myName(String8::format("thread %p type %d TID %d", this, mType, gettid()));

    while (!exitPending())
    {
        Vector<sp<IAfEffectChain>> effectChains;

        { // under Thread lock
        audio_utils::unique_lock _l(mutex());

        if (mSignalPending) {
            // A signal was raised while we were unlocked
            mSignalPending = false;
        } else {
            if (mConfigEvents.isEmpty()) {
                // we're about to wait, flush the binder command buffer
                IPCThreadState::self()->flushCommands();

                if (exitPending()) {
                    break;
                }

                // wait until we have something to do...
                ALOGV("%s going to sleep", myName.c_str());
                mWaitWorkCV.wait(_l);
                ALOGV("%s waking up", myName.c_str());

                checkSilentMode_l();

                continue;
            }
        }

        processConfigEvents_l();

        processVolume_l();

        checkInvalidTracks_l();

        mActiveTracks.updatePowerState_l(this);

        updateMetadata_l();

        lockEffectChains_l(effectChains);
        } // release Thread lock

        for (size_t i = 0; i < effectChains.size(); i ++) {
            effectChains[i]->process_l(); // Thread is not locked, but effect chain is locked
        }

        // enable changes in effect chain, including moving to another thread.
        unlockEffectChains(effectChains);
        // Effect chains will be actually deleted here if they were removed from
        // mEffectChains list during mixing or effects processing
        mThreadloopExecutor.process();
    }
    mThreadloopExecutor.process(); // process any remaining deferred actions.
    // deferred actions after this point are ignored.

    threadLoop_exit();

    if (!mStandby) {
        threadLoop_standby();
        mStandby = true;
    }

    ALOGV("Thread %p type %d exiting", this, mType);
    return false;
}

// checkForNewParameter_l() must be called with ThreadBase::mutex() held
bool MmapThread::checkForNewParameter_l(const String8& keyValuePair,
                                                              status_t& status)
{
    AudioParameter param = AudioParameter(keyValuePair);
    int value;
    bool sendToHal = true;
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        LOG_FATAL("Should not happen set routing device in MmapThread");
    }
    if (sendToHal) {
        status = mHalStream->setParameters(keyValuePair);
    } else {
        status = NO_ERROR;
    }

    return false;
}

String8 MmapThread::getParameters(const String8& keys)
{
    audio_utils::lock_guard _l(mutex());
    String8 out_s8;
    if (initCheck() == NO_ERROR && mHalStream->getParameters(keys, &out_s8) == OK) {
        return out_s8;
    }
    return {};
}

void MmapThread::ioConfigChanged_l(audio_io_config_event_t event, pid_t pid,
                                               audio_port_handle_t portId __unused) {
    sp<AudioIoDescriptor> desc;
    bool isInput = false;
    switch (event) {
    case AUDIO_INPUT_OPENED:
    case AUDIO_INPUT_REGISTERED:
    case AUDIO_INPUT_CONFIG_CHANGED:
        isInput = true;
        FALLTHROUGH_INTENDED;
    case AUDIO_OUTPUT_OPENED:
    case AUDIO_OUTPUT_REGISTERED:
    case AUDIO_OUTPUT_CONFIG_CHANGED:
        desc = sp<AudioIoDescriptor>::make(mId, mPatch, isInput,
                mSampleRate, mFormat, mChannelMask, mFrameCount, mFrameCount);
        break;
    case AUDIO_INPUT_CLOSED:
    case AUDIO_OUTPUT_CLOSED:
    default:
        desc = sp<AudioIoDescriptor>::make(mId);
        break;
    }
    mAfThreadCallback->ioConfigChanged_l(event, desc, pid);
}

status_t MmapThread::createAudioPatch_l(const struct audio_patch* patch,
                                                          audio_patch_handle_t *handle)
NO_THREAD_SAFETY_ANALYSIS  // elease and re-acquire mutex()
{
    status_t status = NO_ERROR;

    // store new device and send to effects
    audio_devices_t type = AUDIO_DEVICE_NONE;
    DeviceIdVector deviceIds;
    AudioDeviceTypeAddrVector sinkDeviceTypeAddrs;
    AudioDeviceTypeAddr sourceDeviceTypeAddr;
    uint32_t numDevices = 0;
    if (isOutput()) {
        for (unsigned int i = 0; i < patch->num_sinks; i++) {
            LOG_ALWAYS_FATAL_IF(popcount(patch->sinks[i].ext.device.type) > 1
                                && !mAudioHwDev->supportsAudioPatches(),
                                "Enumerated device type(%#x) must not be used "
                                "as it does not support audio patches",
                                patch->sinks[i].ext.device.type);
            type = static_cast<audio_devices_t>(type | patch->sinks[i].ext.device.type);
            sinkDeviceTypeAddrs.emplace_back(patch->sinks[i].ext.device.type,
                    patch->sinks[i].ext.device.address);
            deviceIds.push_back(patch->sinks[i].id);
        }
        numDevices = mPatch.num_sinks;
    } else {
        type = patch->sources[0].ext.device.type;
        deviceIds.push_back(patch->sources[0].id);
        numDevices = mPatch.num_sources;
        sourceDeviceTypeAddr.mType = patch->sources[0].ext.device.type;
        sourceDeviceTypeAddr.setAddress(patch->sources[0].ext.device.address);
    }

    for (size_t i = 0; i < mEffectChains.size(); i++) {
        if (isOutput()) {
            mEffectChains[i]->setDevices_l(sinkDeviceTypeAddrs);
        } else {
            mEffectChains[i]->setInputDevice_l(sourceDeviceTypeAddr);
        }
    }

    if (!isOutput()) {
        // store new source and send to effects
        if (mAudioSource != patch->sinks[0].ext.mix.usecase.source) {
            mAudioSource = patch->sinks[0].ext.mix.usecase.source;
            for (size_t i = 0; i < mEffectChains.size(); i++) {
                mEffectChains[i]->setAudioSource_l(mAudioSource);
            }
        }
    }

    // For mmap streams, once the routing has changed, they will be disconnected. It should be
    // okay to notify the client earlier before the new patch creation.
    if (!areDeviceIdsEqual(deviceIds, mDeviceIds)) {
        if (const sp<MmapStreamCallback> callback = mCallback.promote()) {
            // The aaudioservice handle the routing changed event asynchronously. In that case,
            // it is safe to hold the lock here.
            callback->onRoutingChanged(deviceIds);
        }
    }

    if (mAudioHwDev->supportsAudioPatches()) {
        status = mHalDevice->createAudioPatch(patch->num_sources, patch->sources, patch->num_sinks,
                                              patch->sinks, handle);
    } else {
        audio_port_config port;
        std::optional<audio_source_t> source;
        if (isOutput()) {
            port = patch->sinks[0];
        } else {
            port = patch->sources[0];
            source = patch->sinks[0].ext.mix.usecase.source;
        }
        status = mHalStream->legacyCreateAudioPatch(port, source, type);
        *handle = AUDIO_PATCH_HANDLE_NONE;
    }

    if (numDevices == 0 || (!areDeviceIdsEqual(deviceIds, mDeviceIds))) {
        if (isOutput()) {
            sendIoConfigEvent_l(AUDIO_OUTPUT_CONFIG_CHANGED);
            mOutDeviceTypeAddrs = sinkDeviceTypeAddrs;
            checkSilentMode_l();
        } else {
            sendIoConfigEvent_l(AUDIO_INPUT_CONFIG_CHANGED);
            mInDeviceTypeAddr = sourceDeviceTypeAddr;
        }
        mPatch = *patch;
        mDeviceIds = deviceIds;
    }
    // Force meteadata update after a route change
    mActiveTracks.setHasChanged();

    const std::string patchSourcesAsString = isOutput() ? "" : patchSourcesToString(patch);
    const std::string patchSinksAsString = isOutput() ? patchSinksToString(patch) : "";
    mThreadMetrics.logEndInterval();
    mThreadMetrics.logCreatePatch(patchSourcesAsString, patchSinksAsString);
    mThreadMetrics.logBeginInterval();
    for (const auto &track : mActiveTracks) {
        track->logEndInterval();
        track->logBeginInterval(isOutput() ? patchSinksAsString : patchSourcesAsString);
    }

    return status;
}

status_t MmapThread::releaseAudioPatch_l(const audio_patch_handle_t handle)
{
    status_t status = NO_ERROR;

    mPatch = audio_patch{};
    mOutDeviceTypeAddrs.clear();
    mInDeviceTypeAddr.reset();

    bool supportsAudioPatches = mHalDevice->supportsAudioPatches(&supportsAudioPatches) == OK ?
                                        supportsAudioPatches : false;

    if (supportsAudioPatches) {
        status = mHalDevice->releaseAudioPatch(handle);
    } else {
        status = mHalStream->legacyReleaseAudioPatch();
    }
    // Force meteadata update after a route change
    mActiveTracks.setHasChanged();

    return status;
}

void MmapThread::toAudioPortConfig(struct audio_port_config* config)
NO_THREAD_SAFETY_ANALYSIS // mAudioHwDev handle access
{
    ThreadBase::toAudioPortConfig(config);
    if (isOutput()) {
        config->role = AUDIO_PORT_ROLE_SOURCE;
        config->ext.mix.hw_module = mAudioHwDev->handle();
        config->ext.mix.usecase.stream = AUDIO_STREAM_DEFAULT;
    } else {
        config->role = AUDIO_PORT_ROLE_SINK;
        config->ext.mix.hw_module = mAudioHwDev->handle();
        config->ext.mix.usecase.source = mAudioSource;
    }
}

status_t MmapThread::addEffectChain_l(const sp<IAfEffectChain>& chain)
{
    audio_session_t session = chain->sessionId();

    ALOGV("addEffectChain_l() %p on thread %p for session %d", chain.get(), this, session);
    // Attach all tracks with same session ID to this chain.
    // indicate all active tracks in the chain
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        if (session == track->sessionId()) {
            chain->incTrackCnt();
            chain->incActiveTrackCnt();
        }
    }

    chain->setThread(this);
    chain->setInBuffer(nullptr);
    chain->setOutBuffer(nullptr);
    chain->syncHalEffectsState_l();

    mEffectChains.add(chain);
    checkSuspendOnAddEffectChain_l(chain);
    return NO_ERROR;
}

size_t MmapThread::removeEffectChain_l(const sp<IAfEffectChain>& chain)
{
    audio_session_t session = chain->sessionId();

    ALOGV("removeEffectChain_l() %p from thread %p for session %d", chain.get(), this, session);

    for (size_t i = 0; i < mEffectChains.size(); i++) {
        if (chain == mEffectChains[i]) {
            mEffectChains.removeAt(i);
            // detach all active tracks from the chain
            // detach all tracks with same session ID from this chain
            for (const sp<IAfMmapTrack>& track : mActiveTracks) {
                if (session == track->sessionId()) {
                    chain->decActiveTrackCnt();
                    chain->decTrackCnt();
                }
            }
            break;
        }
    }
    return mEffectChains.size();
}

void MmapThread::threadLoop_standby()
{
    mHalStream->standby();
}

void MmapThread::threadLoop_exit()
{
    // Do not call callback->onTearDown() because it is redundant for thread exit
    // and because it can cause a recursive mutex lock on stop().
}

status_t MmapThread::setSyncEvent(const sp<SyncEvent>& /* event */)
{
    return BAD_VALUE;
}

bool MmapThread::isValidSyncEvent(
        const sp<SyncEvent>& /* event */) const
{
    return false;
}

status_t MmapThread::checkEffectCompatibility_l(
        const effect_descriptor_t *desc, audio_session_t sessionId)
{
    // No global effect sessions on mmap threads
    if (audio_is_global_session(sessionId)) {
        ALOGW("checkEffectCompatibility_l(): global effect %s on MMAP thread %s",
                desc->name, mThreadName);
        return BAD_VALUE;
    }

    if (!isOutput() && ((desc->flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_PRE_PROC)) {
        ALOGW("checkEffectCompatibility_l(): non pre processing effect %s on capture mmap thread",
                desc->name);
        return BAD_VALUE;
    }
    if (isOutput() && ((desc->flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_PRE_PROC)) {
        ALOGW("checkEffectCompatibility_l(): pre processing effect %s created on playback mmap "
              "thread", desc->name);
        return BAD_VALUE;
    }

    // Only allow effects without processing load or latency
    if ((desc->flags & EFFECT_FLAG_NO_PROCESS_MASK) != EFFECT_FLAG_NO_PROCESS) {
        return BAD_VALUE;
    }

    if (IAfEffectModule::isHapticGenerator(&desc->type)) {
        ALOGE("%s(): HapticGenerator is not supported for MmapThread", __func__);
        return BAD_VALUE;
    }

    return NO_ERROR;
}

void MmapThread::checkInvalidTracks_l()
{
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        if (track->isInvalid()) {
            if (const sp<MmapStreamCallback> callback = mCallback.promote()) {
                // The aaudioservice handle the routing changed event asynchronously. In that case,
                // it is safe to hold the lock here.
                callback->onRoutingChanged({});
            } else if (mNoCallbackWarningCount < kMaxNoCallbackWarnings) {
                ALOGW("Could not notify MMAP stream tear down: no onRoutingChanged callback!");
                mNoCallbackWarningCount++;
            }
            break;
        }
    }
}

void MmapThread::dumpInternals_l(int fd, const Vector<String16>& /* args */)
{
    dprintf(fd, "  Attributes: content type %d usage %d source %d\n",
            mAttr.content_type, mAttr.usage, mAttr.source);
    dprintf(fd, "  Session: %d port Id: %d\n", mSessionId, mPortId);
    if (mActiveTracks.isEmpty()) {
        dprintf(fd, "  No active clients\n");
    }
}

void MmapThread::dumpTracks_l(int fd, const Vector<String16>& /* args */)
{
    String8 result;
    size_t numtracks = mActiveTracks.size();
    dprintf(fd, "  %zu Tracks\n", numtracks);
    const char *prefix = "    ";
    if (numtracks) {
        result.append(prefix);
        mActiveTracks[0]->appendDumpHeader(result);
        for (size_t i = 0; i < numtracks ; ++i) {
            sp<IAfMmapTrack> track = mActiveTracks[i];
            result.append(prefix);
            track->appendDump(result, true /* active */);
        }
    } else {
        dprintf(fd, "\n");
    }
    write(fd, result.c_str(), result.size());
}

std::string MmapThread::getLocalLogHeader() const {
    using namespace std::literals;
    static constexpr auto indent = "                             "
                                   "                            "sv;
    return std::string{indent}.append(IAfMmapTrack::getLogHeader());
}

/* static */
sp<IAfMmapPlaybackThread> IAfMmapPlaybackThread::create(
        const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        AudioHwDevice* hwDev,  AudioStreamOut* output, bool systemReady) {
    return sp<MmapPlaybackThread>::make(afThreadCallback, id, hwDev, output, systemReady);
}

MmapPlaybackThread::MmapPlaybackThread(
        const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        AudioHwDevice *hwDev,  AudioStreamOut *output, bool systemReady)
    : MmapThread(afThreadCallback, id, hwDev, output->stream, systemReady, true /* isOut */),
      mStreamType(AUDIO_STREAM_MUSIC),
      mOutput(output)
{
    snprintf(mThreadName, kThreadNameLength, "AudioMmapOut_%X", id);
    mFlagsAsString = toString(output->flags);
    mChannelCount = audio_channel_count_from_out_mask(mChannelMask);
    mMasterVolume = afThreadCallback->masterVolume_l();
    mMasterMute = afThreadCallback->masterMute_l();
    if (!audioserver_flags::portid_volume_management()) {
        for (int i = AUDIO_STREAM_MIN; i < AUDIO_STREAM_FOR_POLICY_CNT; ++i) {
            const audio_stream_type_t stream{static_cast<audio_stream_type_t>(i)};
            mStreamTypes[stream].volume = 0.0f;
            mStreamTypes[stream].mute = mAfThreadCallback->streamMute_l(stream);
        }
        // Audio patch and call assistant volume are always max
        mStreamTypes[AUDIO_STREAM_PATCH].volume = 1.0f;
        mStreamTypes[AUDIO_STREAM_PATCH].mute = false;
        mStreamTypes[AUDIO_STREAM_CALL_ASSISTANT].volume = 1.0f;
        mStreamTypes[AUDIO_STREAM_CALL_ASSISTANT].mute = false;
    }
    if (mAudioHwDev) {
        if (mAudioHwDev->canSetMasterVolume()) {
            mMasterVolume = 1.0;
        }

        if (mAudioHwDev->canSetMasterMute()) {
            mMasterMute = false;
        }
    }
}

void MmapPlaybackThread::configure(const audio_attributes_t* attr,
                                                audio_stream_type_t streamType,
                                                audio_session_t sessionId,
                                                const sp<MmapStreamCallback>& callback,
                                                const DeviceIdVector& deviceIds,
                                                audio_port_handle_t portId)
{
    audio_utils::lock_guard l(mutex());
    MmapThread::configure_l(attr, streamType, sessionId, callback, deviceIds, portId);
    mStreamType = streamType;
}

AudioStreamOut* MmapPlaybackThread::clearOutput()
{
    audio_utils::lock_guard _l(mutex());
    AudioStreamOut *output = mOutput;
    mOutput = NULL;
    return output;
}

void MmapPlaybackThread::setMasterVolume(float value)
{
    audio_utils::lock_guard _l(mutex());
    // Don't apply master volume in SW if our HAL can do it for us.
    if (mAudioHwDev &&
            mAudioHwDev->canSetMasterVolume()) {
        mMasterVolume = 1.0;
    } else {
        mMasterVolume = value;
    }
}

void MmapPlaybackThread::setMasterMute(bool muted)
{
    audio_utils::lock_guard _l(mutex());
    // Don't apply master mute in SW if our HAL can do it for us.
    if (mAudioHwDev && mAudioHwDev->canSetMasterMute()) {
        mMasterMute = false;
    } else {
        mMasterMute = muted;
    }
}

void MmapPlaybackThread::setStreamVolume(audio_stream_type_t stream, float value, bool muted)
{
    ALOGV("%s: stream %d value %f muted %d", __func__, stream, value, muted);
    audio_utils::lock_guard _l(mutex());
    mStreamTypes[stream].volume = value;
    if (com_android_media_audio_ring_my_car()) {
        mStreamTypes[stream].mute = muted;
    }
    if (stream == mStreamType) {
        broadcast_l();
    }
}

float MmapPlaybackThread::streamVolume(audio_stream_type_t stream) const
{
    audio_utils::lock_guard _l(mutex());
    return mStreamTypes[stream].volume;
}

void MmapPlaybackThread::setStreamMute(audio_stream_type_t stream, bool muted)
{
    audio_utils::lock_guard _l(mutex());
    mStreamTypes[stream].mute = muted;
    if (stream == mStreamType) {
        broadcast_l();
    }
}

status_t MmapPlaybackThread::setPortsVolume(
        const std::vector<audio_port_handle_t>& portIds, float volume, bool muted) {
    audio_utils::lock_guard _l(mutex());
    for (const auto& portId : portIds) {
        for (const sp<IAfMmapTrack>& track : mActiveTracks) {
            if (portId == track->portId()) {
                track->setPortVolume(volume);
                track->setPortMute(muted);
                break;
            }
        }
    }
    broadcast_l();
    return NO_ERROR;
}

void MmapPlaybackThread::checkUpdateTrackMetadataForUid(uid_t uid) {
    audio_utils::lock_guard _l(mutex());
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        if (track->uid() == uid) {
            track->setMetadataHasChanged();
        }
    }
}

void MmapPlaybackThread::invalidateTracks(audio_stream_type_t streamType)
{
    audio_utils::lock_guard _l(mutex());
    if (streamType == mStreamType) {
        for (const sp<IAfMmapTrack>& track : mActiveTracks) {
            track->invalidate();
        }
        broadcast_l();
    }
}

void MmapPlaybackThread::invalidateTracks(std::set<audio_port_handle_t>& portIds)
{
    audio_utils::lock_guard _l(mutex());
    bool trackMatch = false;
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        if (portIds.find(track->portId()) != portIds.end()) {
            track->invalidate();
            trackMatch = true;
            portIds.erase(track->portId());
        }
        if (portIds.empty()) {
            break;
        }
    }
    if (trackMatch) {
        broadcast_l();
    }
}

void MmapPlaybackThread::processVolume_l()
NO_THREAD_SAFETY_ANALYSIS // access of track->processMuteEvent
{
    float volume = 0;
    if (!audioserver_flags::portid_volume_management()) {
        if (mMasterMute || streamMuted_l()) {
            volume = 0;
        } else {
            volume = mMasterVolume * streamVolume_l();
        }
    } else {
        if (mMasterMute) {
            volume = 0;
        } else {
            // All mmap tracks are declared with the same audio attributes to the audio policy
            // manager. Hence, they follow the same routing / volume group. Any change of volume
            // will be broadcasted to all tracks. Thus, take arbitrarily first track volume.
            size_t numtracks = mActiveTracks.size();
            if (numtracks) {
                if (mActiveTracks[0]->getPortMute()) {
                    volume = 0;
                } else {
                    volume = mMasterVolume * mActiveTracks[0]->getPortVolume();
                }
            }
        }
    }

    bool shouldMutePlaybackHardening = std::all_of(mActiveTracks.begin(), mActiveTracks.end(),
            [](const auto& x) { return x->isPlaybackRestrictedControl(); });
    if (shouldMutePlaybackHardening) {
        volume = 0;
    }

    if (volume != mHalVolFloat) {
        // Convert volumes from float to 8.24
        uint32_t vol = (uint32_t)(volume * (1 << 24));

        // Delegate volume control to effect in track effect chain if needed
        // only one effect chain can be present on DirectOutputThread, so if
        // there is one, the track is connected to it
        if (!mEffectChains.isEmpty()) {
            mEffectChains[0]->setVolume(&vol, &vol);
            volume = (float)vol / (1 << 24);
        }
        // Try to use HW volume control and fall back to SW control if not implemented
        if (mOutput->stream->setVolume(volume, volume) == NO_ERROR) {
            mHalVolFloat = volume; // HW volume control worked, so update value.
            mNoCallbackWarningCount = 0;
        } else {
            sp<MmapStreamCallback> callback = mCallback.promote();
            if (callback != 0) {
                mHalVolFloat = volume; // SW volume control worked, so update value.
                mNoCallbackWarningCount = 0;
                mutex().unlock();
                callback->onVolumeChanged(volume);
                mutex().lock();
            } else {
                if (mNoCallbackWarningCount < kMaxNoCallbackWarnings) {
                    ALOGW("Could not set MMAP stream volume: no volume callback!");
                    mNoCallbackWarningCount++;
                }
            }
        }
        const auto amn = mAfThreadCallback->getAudioManagerNative();
        for (const sp<IAfMmapTrack>& track : mActiveTracks) {
            track->setMetadataHasChanged();
            if (amn) {
                if (!audioserver_flags::portid_volume_management()) {
                    track->processMuteEvent(*amn,
                            /*muteState=*/{mMasterMute,
                            streamVolume_l() == 0.f,
                            streamMuted_l(),
                            // TODO(b/241533526): adjust logic to include mute from AppOps
                            false /*muteFromPlaybackRestricted*/,
                            false /*muteFromClientVolume*/,
                            false /*muteFromVolumeShaper*/,
                            false /*muteFromPortVolume*/,
                            shouldMutePlaybackHardening});
                } else {
                    track->processMuteEvent(*amn,
                        /*muteState=*/{mMasterMute,
                                       track->getPortVolume() == 0.f,
                                       /* muteFromStreamMuted= */ false,
                                       // TODO(b/241533526): adjust logic to include mute from AppOp
                                       false /*muteFromPlaybackRestricted*/,
                                       false /*muteFromClientVolume*/,
                                       false /*muteFromVolumeShaper*/,
                                       track->getPortMute(),
                                       shouldMutePlaybackHardening});
                }
                track->maybeLogPlaybackHardening(*amn);
            }
        }
    }
}

ThreadBase::MetadataUpdate MmapPlaybackThread::updateMetadata_l()
{
    if (!isStreamInitialized() || !mActiveTracks.readAndClearHasChanged()) {
        return {}; // nothing to do
    }
    StreamOutHalInterface::SourceMetadata metadata;
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        // No track is invalid as this is called after prepareTrack_l in the same critical section
        playback_track_metadata_v7_t trackMetadata;
        trackMetadata.base = {
                .usage = track->attributes().usage,
                .content_type = track->attributes().content_type,
                .gain = mHalVolFloat, // TODO: propagate from aaudio pre-mix volume
        };
        trackMetadata.channel_mask = track->channelMask();
        std::string tagStr(track->attributes().tags);
        if (audioserver_flags::enable_gmap_mode() && track->attributes().usage == AUDIO_USAGE_GAME
                && afThreadCallback()->hasAlreadyCaptured(track->uid())
                && (tagStr.size() + strlen(AUDIO_ATTRIBUTES_TAG_GMAP_BIDIRECTIONAL)
                    + (tagStr.size() ? 1 : 0))
                    < AUDIO_ATTRIBUTES_TAGS_MAX_SIZE) {

            if (tagStr.size() != 0) {
                tagStr.append(1, AUDIO_ATTRIBUTES_TAGS_SEPARATOR);
            }
            tagStr.append(AUDIO_ATTRIBUTES_TAG_GMAP_BIDIRECTIONAL);
        }
        strncpy(trackMetadata.tags, tagStr.c_str(), AUDIO_ATTRIBUTES_TAGS_MAX_SIZE);
        trackMetadata.tags[AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - 1] = '\0';
        metadata.tracks.push_back(trackMetadata);
    }
    mOutput->stream->updateSourceMetadata(metadata);

    MetadataUpdate change;
    change.playbackMetadataUpdate = metadata.tracks;
    return change;
};

void MmapPlaybackThread::checkSilentMode_l()
{
    if (property_get_bool("ro.audio.silent", false)) {
        ALOGW("ro.audio.silent is now ignored");
    }
}

void MmapPlaybackThread::toAudioPortConfig(struct audio_port_config* config)
{
    MmapThread::toAudioPortConfig(config);
    if (mOutput && mOutput->flags != AUDIO_OUTPUT_FLAG_NONE) {
        config->config_mask |= AUDIO_PORT_CONFIG_FLAGS;
        config->flags.output = mOutput->flags;
    }
}

status_t MmapPlaybackThread::getExternalPosition(uint64_t* position,
        int64_t* timeNanos) const
{
    if (mOutput == nullptr) {
        return NO_INIT;
    }
    struct timespec timestamp;
    status_t status = mOutput->getPresentationPosition(position, &timestamp);
    if (status == NO_ERROR) {
        *timeNanos = timestamp.tv_sec * NANOS_PER_SECOND + timestamp.tv_nsec;
    }
    return status;
}

status_t MmapPlaybackThread::reportData(const void* buffer, size_t frameCount) {
    // Send to MelProcessor for sound dose measurement.
    auto processor = mMelProcessor.load();
    if (processor) {
        processor->process(buffer, frameCount * mFrameSize);
    }

    return NO_ERROR;
}

// startMelComputation_l() must be called with AudioFlinger::mutex() held
void MmapPlaybackThread::startMelComputation_l(
        const sp<audio_utils::MelProcessor>& processor)
{
    ALOGV("%s: starting mel processor for thread %d", __func__, id());
    mMelProcessor.store(processor);
    if (processor) {
        processor->resume();
    }

    // no need to update output format for MMapPlaybackThread since it is
    // assigned constant for each thread
}

// stopMelComputation_l() must be called with AudioFlinger::mutex() held
void MmapPlaybackThread::stopMelComputation_l()
{
    ALOGV("%s: pausing mel processor for thread %d", __func__, id());
    auto melProcessor = mMelProcessor.load();
    if (melProcessor != nullptr) {
        melProcessor->pause();
    }
}

void MmapPlaybackThread::dumpInternals_l(int fd, const Vector<String16>& args)
{
    MmapThread::dumpInternals_l(fd, args);
    if (!audioserver_flags::portid_volume_management()) {
        dprintf(fd, "  Stream type: %d Stream volume: %f HAL volume: %f Stream mute %d",
                mStreamType, streamVolume_l(), mHalVolFloat, streamMuted_l());
    } else {
        dprintf(fd, "  HAL volume: %f", mHalVolFloat);
    }
    dprintf(fd, "\n");
    dprintf(fd, "  Master volume: %f Master mute %d\n", mMasterVolume, mMasterMute);
}

/* static */
sp<IAfMmapCaptureThread> IAfMmapCaptureThread::create(
        const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        AudioHwDevice* hwDev,  AudioStreamIn* input, bool systemReady) {
    return sp<MmapCaptureThread>::make(afThreadCallback, id, hwDev, input, systemReady);
}

MmapCaptureThread::MmapCaptureThread(
        const sp<IAfThreadCallback>& afThreadCallback, audio_io_handle_t id,
        AudioHwDevice *hwDev,  AudioStreamIn *input, bool systemReady)
    : MmapThread(afThreadCallback, id, hwDev, input->stream, systemReady, false /* isOut */),
      mInput(input)
{
    snprintf(mThreadName, kThreadNameLength, "AudioMmapIn_%X", id);
    mFlagsAsString = toString(input->flags);
    mChannelCount = audio_channel_count_from_in_mask(mChannelMask);
}

status_t MmapCaptureThread::exitStandby_l()
{
    {
        // mInput might have been cleared by clearInput()
        if (mInput != nullptr && mInput->stream != nullptr) {
            mInput->stream->setGain(1.0f);
        }
    }
    return MmapThread::exitStandby_l();
}

AudioStreamIn* MmapCaptureThread::clearInput()
{
    audio_utils::lock_guard _l(mutex());
    AudioStreamIn *input = mInput;
    mInput = NULL;
    return input;
}

void MmapCaptureThread::processVolume_l()
{
    bool changed = false;
    bool silenced = false;

    sp<MmapStreamCallback> callback = mCallback.promote();
    if (callback == 0) {
        if (mNoCallbackWarningCount < kMaxNoCallbackWarnings) {
            ALOGW("Could not set MMAP stream silenced: no onStreamSilenced callback!");
            mNoCallbackWarningCount++;
        }
    }

    // After a change occurred in track silenced state, mute capture in audio DSP if at least one
    // track is silenced and unmute otherwise
    for (size_t i = 0; i < mActiveTracks.size() && !silenced; i++) {
        if (!mActiveTracks[i]->getAndSetSilencedNotified_l()) {
            changed = true;
            silenced = mActiveTracks[i]->isSilenced_l();
        }
    }

    if (changed) {
        mInput->stream->setGain(silenced ? 0.0f: 1.0f);
    }
}

ThreadBase::MetadataUpdate MmapCaptureThread::updateMetadata_l()
{
    if (!isStreamInitialized() || !mActiveTracks.readAndClearHasChanged()) {
        return {}; // nothing to do
    }
    StreamInHalInterface::SinkMetadata metadata;
    for (const sp<IAfMmapTrack>& track : mActiveTracks) {
        // No track is invalid as this is called after prepareTrack_l in the same critical section
        record_track_metadata_v7_t trackMetadata;
        trackMetadata.base = {
                .source = track->attributes().source,
                .gain = 1, // capture tracks do not have volumes
        };
        trackMetadata.channel_mask = track->channelMask(),
        strncpy(trackMetadata.tags, track->attributes().tags, AUDIO_ATTRIBUTES_TAGS_MAX_SIZE);
        metadata.tracks.push_back(trackMetadata);
    }
    mInput->stream->updateSinkMetadata(metadata);
    MetadataUpdate change;
    change.recordMetadataUpdate = metadata.tracks;
    return change;
}

void MmapCaptureThread::setRecordSilenced(audio_port_handle_t portId, bool silenced)
{
    audio_utils::lock_guard _l(mutex());
    for (size_t i = 0; i < mActiveTracks.size() ; i++) {
        if (mActiveTracks[i]->portId() == portId) {
            mActiveTracks[i]->setSilenced_l(silenced);
            broadcast_l();
        }
    }
    setClientSilencedIfExists_l(portId, silenced);
}

void MmapCaptureThread::toAudioPortConfig(struct audio_port_config* config)
{
    MmapThread::toAudioPortConfig(config);
    if (mInput && mInput->flags != AUDIO_INPUT_FLAG_NONE) {
        config->config_mask |= AUDIO_PORT_CONFIG_FLAGS;
        config->flags.input = mInput->flags;
    }
}

status_t MmapCaptureThread::getExternalPosition(
        uint64_t* position, int64_t* timeNanos) const
{
    if (mInput == nullptr) {
        return NO_INIT;
    }
    return mInput->getCapturePosition((int64_t*)position, timeNanos);
}

// ----------------------------------------------------------------------------

/* static */
sp<IAfPlaybackThread> IAfPlaybackThread::createBitPerfectThread(
        const sp<IAfThreadCallback>& afThreadCallback,
        AudioStreamOut* output, audio_io_handle_t id, bool systemReady) {
    return sp<BitPerfectThread>::make(afThreadCallback, output, id, systemReady);
}

BitPerfectThread::BitPerfectThread(const sp<IAfThreadCallback> &afThreadCallback,
        AudioStreamOut *output, audio_io_handle_t id, bool systemReady)
        : MixerThread(afThreadCallback, output, id, systemReady, BIT_PERFECT) {}

PlaybackThread::mixer_state BitPerfectThread::prepareTracks_l(
        Vector<sp<IAfTrack>>* tracksToRemove) {
    mixer_state result = MixerThread::prepareTracks_l(tracksToRemove);
    // If there is only one active track and it is bit-perfect, enable tee buffer.
    float volumeLeft = 1.0f;
    float volumeRight = 1.0f;
    if (sp<IAfTrack> bitPerfectTrack = getTrackToStreamBitPerfectly_l();
        bitPerfectTrack != nullptr) {
        const int trackId = bitPerfectTrack->id();
        mAudioMixer->setParameter(
                    trackId, AudioMixer::TRACK, AudioMixer::TEE_BUFFER, (void *)mSinkBuffer);
        mAudioMixer->setParameter(
                    trackId, AudioMixer::TRACK, AudioMixer::TEE_BUFFER_FRAME_COUNT,
                    (void *)(uintptr_t)mNormalFrameCount);
        bitPerfectTrack->getFinalVolume(&volumeLeft, &volumeRight);
        mIsBitPerfect = true;
    } else {
        mIsBitPerfect = false;
        // No need to copy bit-perfect data directly to sink buffer given there are multiple tracks
        // active.
        for (const auto& track : mActiveTracks) {
            const int trackId = track->id();
            mAudioMixer->setParameter(
                        trackId, AudioMixer::TRACK, AudioMixer::TEE_BUFFER, nullptr);
        }
    }
    if (mVolumeLeft != volumeLeft || mVolumeRight != volumeRight) {
        mVolumeLeft = volumeLeft;
        mVolumeRight = volumeRight;
        setVolumeForOutput_l(volumeLeft, volumeRight);
    }
    return result;
}

void BitPerfectThread::threadLoop_mix() {
    MixerThread::threadLoop_mix();
    mHasDataCopiedToSinkBuffer = mIsBitPerfect;
}

void BitPerfectThread::setTracksInternalMute(
        std::map<audio_port_handle_t, bool>* tracksInternalMute) {
    audio_utils::lock_guard _l(mutex());
    for (auto& track : mTracks) {
        if (auto it = tracksInternalMute->find(track->portId()); it != tracksInternalMute->end()) {
            track->setInternalMute(it->second);
            tracksInternalMute->erase(it);
        }
    }
}

sp<IAfTrack> BitPerfectThread::getTrackToStreamBitPerfectly_l() {
    if (com::android::media::audioserver::
                fix_concurrent_playback_behavior_with_bit_perfect_client()) {
        sp<IAfTrack> bitPerfectTrack = nullptr;
        bool allOtherTracksMuted = true;
        // Return the bit perfect track if all other tracks are muted
        for (const auto& track : mActiveTracks) {
            if (track->isBitPerfect()) {
                if (track->getInternalMute()) {
                    // There can only be one bit-perfect client active. If it is mute internally,
                    // there is no need to stream bit-perfectly.
                    break;
                }
                bitPerfectTrack = track;
            } else if (track->getFinalVolume() != 0.f) {
                allOtherTracksMuted = false;
                if (bitPerfectTrack != nullptr) {
                    break;
                }
            }
        }
        return allOtherTracksMuted ? bitPerfectTrack : nullptr;
    } else {
        if (mActiveTracks.size() == 1 && mActiveTracks[0]->isBitPerfect()) {
            return mActiveTracks[0];
        }
    }
    return nullptr;
}

} // namespace android
