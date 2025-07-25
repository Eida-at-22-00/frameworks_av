/*
 * Copyright (C) 2020 The Android Open Source Project
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
#define LOG_TAG "AudioPowerUsage"
#include <utils/Log.h>

#include "AudioAnalytics.h"
#include "MediaMetricsService.h"
#include "StringUtils.h"
#include <map>
#include <sstream>
#include <string>
#include <audio_utils/clock.h>
#include <audio_utils/StringUtils.h>
#include <cutils/properties.h>
#include <stats_media_metrics.h>
#include <sys/timerfd.h>
#include <system/audio.h>

// property to disable audio power use metrics feature, default is enabled
#define PROP_AUDIO_METRICS_DISABLED "persist.media.audio_metrics.power_usage_disabled"
#define AUDIO_METRICS_DISABLED_DEFAULT (false)

// property to set how long to send audio power use metrics data to statsd, default is 24hrs
#define PROP_AUDIO_METRICS_INTERVAL_HR "persist.media.audio_metrics.interval_hr"
#define INTERVAL_HR_DEFAULT (24)

// for Audio Power Usage Metrics
#define AUDIO_POWER_USAGE_KEY_AUDIO_USAGE     "audio.power.usage"

#define AUDIO_POWER_USAGE_PROP_DEVICE         "device"     // int32
#define AUDIO_POWER_USAGE_PROP_DURATION_NS    "durationNs" // int64
#define AUDIO_POWER_USAGE_PROP_TYPE           "type"       // int32
#define AUDIO_POWER_USAGE_PROP_VOLUME         "volume"     // double
#define AUDIO_POWER_USAGE_PROP_MIN_VOLUME_DURATION_NS "minVolumeDurationNs" // int64
#define AUDIO_POWER_USAGE_PROP_MIN_VOLUME             "minVolume"           // double
#define AUDIO_POWER_USAGE_PROP_MAX_VOLUME_DURATION_NS "maxVolumeDurationNs" // int64
#define AUDIO_POWER_USAGE_PROP_MAX_VOLUME             "maxVolume"           // double

namespace android::mediametrics {

/* static */
bool AudioPowerUsage::typeFromString(const std::string& type_string, int32_t& type) {
    static std::map<std::string, int32_t> typeTable = {
        { "AUDIO_STREAM_VOICE_CALL",          VOIP_CALL_TYPE },
        { "AUDIO_STREAM_SYSTEM",              MEDIA_TYPE },
        { "AUDIO_STREAM_RING",                RINGTONE_NOTIFICATION_TYPE },
        { "AUDIO_STREAM_MUSIC",               MEDIA_TYPE },
        { "AUDIO_STREAM_ALARM",               ALARM_TYPE },
        { "AUDIO_STREAM_NOTIFICATION",        RINGTONE_NOTIFICATION_TYPE },

        { "AUDIO_CONTENT_TYPE_SPEECH",        VOIP_CALL_TYPE },
        { "AUDIO_CONTENT_TYPE_MUSIC",         MEDIA_TYPE },
        { "AUDIO_CONTENT_TYPE_MOVIE",         MEDIA_TYPE },
        { "AUDIO_CONTENT_TYPE_SONIFICATION",  RINGTONE_NOTIFICATION_TYPE },

        { "AUDIO_USAGE_MEDIA",                MEDIA_TYPE },
        { "AUDIO_USAGE_VOICE_COMMUNICATION",  VOIP_CALL_TYPE },
        { "AUDIO_USAGE_ALARM",                ALARM_TYPE },
        { "AUDIO_USAGE_NOTIFICATION",         RINGTONE_NOTIFICATION_TYPE },

        { "AUDIO_SOURCE_CAMCORDER",           CAMCORDER_TYPE },
        { "AUDIO_SOURCE_VOICE_COMMUNICATION", VOIP_CALL_TYPE },
        { "AUDIO_SOURCE_DEFAULT",             RECORD_TYPE },
        { "AUDIO_SOURCE_MIC",                 RECORD_TYPE },
        { "AUDIO_SOURCE_UNPROCESSED",         RECORD_TYPE },
        { "AUDIO_SOURCE_VOICE_RECOGNITION",   RECORD_TYPE },
    };

    auto it = typeTable.find(type_string);
    if (it == typeTable.end()) {
        type = UNKNOWN_TYPE;
        return false;
    }

    type = it->second;
    return true;
}

/* static */
bool AudioPowerUsage::deviceFromString(const std::string& device_string, int32_t& device) {
    static std::map<std::string, int32_t> deviceTable = {
        { "AUDIO_DEVICE_OUT_EARPIECE",                  OUTPUT_EARPIECE },
        { "AUDIO_DEVICE_OUT_SPEAKER_SAFE",              OUTPUT_SPEAKER_SAFE },
        { "AUDIO_DEVICE_OUT_SPEAKER",                   OUTPUT_SPEAKER },
        { "AUDIO_DEVICE_OUT_WIRED_HEADSET",             OUTPUT_WIRED_HEADSET },
        { "AUDIO_DEVICE_OUT_WIRED_HEADPHONE",           OUTPUT_WIRED_HEADSET },
        { "AUDIO_DEVICE_OUT_BLUETOOTH_SCO",             OUTPUT_BLUETOOTH_SCO },
        { "AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET",     OUTPUT_BLUETOOTH_SCO },
        { "AUDIO_DEVICE_OUT_BLUETOOTH_A2DP",            OUTPUT_BLUETOOTH_A2DP },
        { "AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES", OUTPUT_BLUETOOTH_A2DP },
        { "AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER",    OUTPUT_BLUETOOTH_A2DP },
        { "AUDIO_DEVICE_OUT_BLE_HEADSET",               OUTPUT_BLUETOOTH_BLE },
        { "AUDIO_DEVICE_OUT_BLE_SPEAKER",               OUTPUT_BLUETOOTH_BLE },
        { "AUDIO_DEVICE_OUT_BLE_BROADCAST",             OUTPUT_BLUETOOTH_BLE },
        { "AUDIO_DEVICE_OUT_USB_HEADSET",               OUTPUT_USB_HEADSET },
        { "AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET",         OUTPUT_DOCK },
        { "AUDIO_DEVICE_OUT_HDMI",                      OUTPUT_HDMI },

        { "AUDIO_DEVICE_IN_BUILTIN_MIC",           INPUT_BUILTIN_MIC },
        { "AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET", INPUT_BLUETOOTH_SCO },
        { "AUDIO_DEVICE_IN_BLUETOOTH_BLE",         INPUT_BLUETOOTH_BLE },
        { "AUDIO_DEVICE_IN_BLE_HEADSET",           INPUT_BLUETOOTH_BLE },
        { "AUDIO_DEVICE_IN_WIRED_HEADSET",         INPUT_WIRED_HEADSET_MIC },
        { "AUDIO_DEVICE_IN_USB_DEVICE",            INPUT_USB_HEADSET_MIC },
        { "AUDIO_DEVICE_IN_BACK_MIC",              INPUT_BUILTIN_BACK_MIC },
    };

    auto it = deviceTable.find(device_string);
    if (it == deviceTable.end()) {
        device = 0;
        return false;
    }

    device = it->second;
    return true;
}

int32_t AudioPowerUsage::deviceFromStringPairs(const std::string& device_strings) {
    int32_t deviceMask = 0;
    const auto devaddrvec = audio_utils::stringutils::getDeviceAddressPairs(device_strings);
    for (const auto &[device, addr] : devaddrvec) {
        int32_t combo_device = 0;
        deviceFromString(device, combo_device);
        deviceMask |= combo_device;
    }
    return deviceMask;
}

void AudioPowerUsage::sendItem(const std::shared_ptr<const mediametrics::Item>& item) const
{
    int32_t type;
    if (!item->getInt32(AUDIO_POWER_USAGE_PROP_TYPE, &type)) return;

    int32_t audio_device;
    if (!item->getInt32(AUDIO_POWER_USAGE_PROP_DEVICE, &audio_device)) return;

    int64_t duration_ns;
    if (!item->getInt64(AUDIO_POWER_USAGE_PROP_DURATION_NS, &duration_ns)) return;

    double volume;
    if (!item->getDouble(AUDIO_POWER_USAGE_PROP_VOLUME, &volume)) return;

    int64_t min_volume_duration_ns;
    if (!item->getInt64(AUDIO_POWER_USAGE_PROP_MIN_VOLUME_DURATION_NS, &min_volume_duration_ns)) {
        return;
    }

    double min_volume;
    if (!item->getDouble(AUDIO_POWER_USAGE_PROP_MIN_VOLUME, &min_volume)) return;

    int64_t max_volume_duration_ns;
    if (!item->getInt64(AUDIO_POWER_USAGE_PROP_MAX_VOLUME_DURATION_NS, &max_volume_duration_ns)) {
        return;
    }

    double max_volume;
    if (!item->getDouble(AUDIO_POWER_USAGE_PROP_MAX_VOLUME, &max_volume)) return;

    const int32_t duration_secs = (int32_t)(duration_ns / NANOS_PER_SECOND);
    const int32_t min_volume_duration_secs = (int32_t)(min_volume_duration_ns / NANOS_PER_SECOND);
    const int32_t max_volume_duration_secs = (int32_t)(max_volume_duration_ns / NANOS_PER_SECOND);
    int result = 0;

    if (__builtin_available(android 33, *)) {
        result = stats::media_metrics::stats_write(
                                         stats::media_metrics::AUDIO_POWER_USAGE_DATA_REPORTED,
                                         audio_device,
                                         duration_secs,
                                         (float)volume,
                                         type,
                                         min_volume_duration_secs,
                                         (float)min_volume,
                                         max_volume_duration_secs,
                                         (float)max_volume);
    }

    std::stringstream log;
    log << "result:" << result << " {"
            << " mediametrics_audio_power_usage_data_reported:"
            << stats::media_metrics::AUDIO_POWER_USAGE_DATA_REPORTED
            << " audio_device:" << audio_device
            << " duration_secs:" << duration_secs
            << " average_volume:" << (float)volume
            << " type:" << type
            << " min_volume_duration_secs:" << min_volume_duration_secs
            << " min_volume:" << (float)min_volume
            << " max_volume_duration_secs:" << max_volume_duration_secs
            << " max_volume:" << (float)max_volume
            << " }";
    mStatsdLog->log(stats::media_metrics::AUDIO_POWER_USAGE_DATA_REPORTED, log.str());
}

void AudioPowerUsage::updateMinMaxVolumeAndDuration(
            const int64_t cur_max_volume_duration_ns, const double cur_max_volume,
            const int64_t cur_min_volume_duration_ns, const double cur_min_volume,
            int64_t& f_max_volume_duration_ns, double& f_max_volume,
            int64_t& f_min_volume_duration_ns, double& f_min_volume)
{
    if (f_min_volume > cur_min_volume) {
        f_min_volume = cur_min_volume;
        f_min_volume_duration_ns = cur_min_volume_duration_ns;
    } else if (f_min_volume == cur_min_volume) {
        f_min_volume_duration_ns += cur_min_volume_duration_ns;
    }
    if (f_max_volume < cur_max_volume) {
        f_max_volume = cur_max_volume;
        f_max_volume_duration_ns = cur_max_volume_duration_ns;
    } else if (f_max_volume == cur_max_volume) {
        f_max_volume_duration_ns += cur_max_volume_duration_ns;
    }
}

bool AudioPowerUsage::saveAsItem_l(
        int32_t device, int64_t duration_ns, int32_t type, double average_vol,
        int64_t max_volume_duration_ns, double max_volume,
        int64_t min_volume_duration_ns, double min_volume)
{
    ALOGV("%s: (%#x, %d, %lld, %f)", __func__, device, type,
                                   (long long)duration_ns, average_vol);
    if (duration_ns == 0) {
        return true; // skip duration 0 usage
    }
    if (device == 0) {
        return true; //ignore unknown device
    }

    for (const auto& item : mItems) {
        int32_t item_type = 0, item_device = 0;
        double item_volume = 0.;
        int64_t item_duration_ns = 0;
        item->getInt32(AUDIO_POWER_USAGE_PROP_DEVICE, &item_device);
        item->getInt64(AUDIO_POWER_USAGE_PROP_DURATION_NS, &item_duration_ns);
        item->getInt32(AUDIO_POWER_USAGE_PROP_TYPE, &item_type);
        item->getDouble(AUDIO_POWER_USAGE_PROP_VOLUME, &item_volume);

        // aggregate by device and type
        if (item_device == device && item_type == type) {
            int64_t final_duration_ns = item_duration_ns + duration_ns;
            double final_volume = (device & INPUT_DEVICE_BIT) ? 1.0:
                            ((item_volume * (double)item_duration_ns +
                            average_vol * (double)duration_ns) / (double)final_duration_ns);

            item->setInt64(AUDIO_POWER_USAGE_PROP_DURATION_NS, final_duration_ns);
            item->setDouble(AUDIO_POWER_USAGE_PROP_VOLUME, final_volume);
            item->setTimestamp(systemTime(SYSTEM_TIME_REALTIME));

            // Update the max/min volume and duration
            int64_t final_min_volume_duration_ns;
            int64_t final_max_volume_duration_ns;
            double final_min_volume;
            double final_max_volume;

            item->getInt64(AUDIO_POWER_USAGE_PROP_MIN_VOLUME_DURATION_NS,
                           &final_min_volume_duration_ns);
            item->getDouble(AUDIO_POWER_USAGE_PROP_MIN_VOLUME, &final_min_volume);
            item->getInt64(AUDIO_POWER_USAGE_PROP_MAX_VOLUME_DURATION_NS,
                           &final_max_volume_duration_ns);
            item->getDouble(AUDIO_POWER_USAGE_PROP_MAX_VOLUME, &final_max_volume);
            updateMinMaxVolumeAndDuration(max_volume_duration_ns, max_volume,
                                          min_volume_duration_ns, min_volume,
                                          final_max_volume_duration_ns, final_max_volume,
                                          final_min_volume_duration_ns, final_min_volume);
            item->setInt64(AUDIO_POWER_USAGE_PROP_MIN_VOLUME_DURATION_NS,
                           final_min_volume_duration_ns);
            item->setDouble(AUDIO_POWER_USAGE_PROP_MIN_VOLUME, final_min_volume);
            item->setInt64(AUDIO_POWER_USAGE_PROP_MAX_VOLUME_DURATION_NS,
                           final_max_volume_duration_ns);
            item->setDouble(AUDIO_POWER_USAGE_PROP_MAX_VOLUME, final_max_volume);

            ALOGV("%s: update (%#x, %d, %lld, %f) --> (%lld, %f) min(%lld, %f) max(%lld, %f)",
                  __func__,
                  device, type,
                  (long long)item_duration_ns, item_volume,
                  (long long)final_duration_ns, final_volume,
                  (long long)final_min_volume_duration_ns, final_min_volume,
                  (long long)final_max_volume_duration_ns, final_max_volume);

            return true;
        }
    }

    auto sitem = std::make_shared<mediametrics::Item>(AUDIO_POWER_USAGE_KEY_AUDIO_USAGE);
    sitem->setTimestamp(systemTime(SYSTEM_TIME_REALTIME));
    sitem->setInt32(AUDIO_POWER_USAGE_PROP_DEVICE, device);
    sitem->setInt64(AUDIO_POWER_USAGE_PROP_DURATION_NS, duration_ns);
    sitem->setInt32(AUDIO_POWER_USAGE_PROP_TYPE, type);
    sitem->setDouble(AUDIO_POWER_USAGE_PROP_VOLUME, average_vol);
    sitem->setInt64(AUDIO_POWER_USAGE_PROP_MIN_VOLUME_DURATION_NS, min_volume_duration_ns);
    sitem->setDouble(AUDIO_POWER_USAGE_PROP_MIN_VOLUME, min_volume);
    sitem->setInt64(AUDIO_POWER_USAGE_PROP_MAX_VOLUME_DURATION_NS, max_volume_duration_ns);
    sitem->setDouble(AUDIO_POWER_USAGE_PROP_MAX_VOLUME, max_volume);
    mItems.emplace_back(sitem);
    return true;
}

bool AudioPowerUsage::saveAsItems_l(
        int32_t device, int64_t duration_ns, int32_t type, double average_vol,
        int64_t max_volume_duration, double max_volume,
        int64_t min_volume_duration, double min_volume)
{
    ALOGV("%s: (%#x, %d, %lld, %f)", __func__, device, type,
                                   (long long)duration_ns, average_vol );
    if (duration_ns == 0) {
        return true; // skip duration 0 usage
    }
    if (device == 0) {
        return true; //ignore unknown device
    }

    bool ret = false;
    const int32_t input_bit = device & INPUT_DEVICE_BIT;
    int32_t device_bits = device ^ input_bit;

    while (device_bits != 0) {
        int32_t tmp_device = device_bits & -device_bits; // get lowest bit
        device_bits ^= tmp_device;  // clear lowest bit
        tmp_device |= input_bit;    // restore input bit
        ret = saveAsItem_l(tmp_device, duration_ns, type, average_vol,
                           max_volume_duration, max_volume,
                           min_volume_duration, min_volume);

        ALOGV("%s: device %#x recorded, remaining device_bits = %#x", __func__,
            tmp_device, device_bits);
    }
    return ret;
}

void AudioPowerUsage::checkTrackRecord(
        const std::shared_ptr<const mediametrics::Item>& item, bool isTrack)
{
    const std::string key = item->getKey();

    int64_t deviceTimeNs = 0;
    if (!item->getInt64(AMEDIAMETRICS_PROP_DEVICETIMENS, &deviceTimeNs)) {
        return;
    }
    double deviceVolume = 1.;
    int64_t maxVolumeDurationNs = 0;
    double maxVolume = AMEDIAMETRICS_INITIAL_MAX_VOLUME;
    int64_t minVolumeDurationNs = 0;
    double minVolume = AMEDIAMETRICS_INITIAL_MIN_VOLUME;
    if (isTrack) {
        if (!item->getDouble(AMEDIAMETRICS_PROP_DEVICEVOLUME, &deviceVolume)) {
            return;
        }
        if (!item->getInt64(AMEDIAMETRICS_PROP_DEVICEMAXVOLUMEDURATIONNS, &maxVolumeDurationNs)) {
            return;
        }
        if (!item->getDouble(AMEDIAMETRICS_PROP_DEVICEMAXVOLUME, &maxVolume)) {
            return;
        }
        if (!item->getInt64(AMEDIAMETRICS_PROP_DEVICEMINVOLUMEDURATIONNS, &minVolumeDurationNs)) {
            return;
        }
        if (!item->getDouble(AMEDIAMETRICS_PROP_DEVICEMINVOLUME, &minVolume)) {
            return;
        }
    }

    int32_t type = 0;
    std::string type_string;
    if ((isTrack && mAudioAnalytics->mAnalyticsState->timeMachine().get(
               key, AMEDIAMETRICS_PROP_STREAMTYPE, &type_string) == OK) ||
        (!isTrack && mAudioAnalytics->mAnalyticsState->timeMachine().get(
               key, AMEDIAMETRICS_PROP_SOURCE, &type_string) == OK)) {
        typeFromString(type_string, type);

        if (isTrack && type == UNKNOWN_TYPE &&
                   mAudioAnalytics->mAnalyticsState->timeMachine().get(
                   key, AMEDIAMETRICS_PROP_USAGE, &type_string) == OK) {
            typeFromString(type_string, type);
        }
        if (isTrack && type == UNKNOWN_TYPE &&
                   mAudioAnalytics->mAnalyticsState->timeMachine().get(
                   key, AMEDIAMETRICS_PROP_CONTENTTYPE, &type_string) == OK) {
            typeFromString(type_string, type);
        }
        ALOGV("type = %s => %d", type_string.c_str(), type);
    }

    int32_t device = 0;
    std::string device_strings;
    if ((isTrack && mAudioAnalytics->mAnalyticsState->timeMachine().get(
         key, AMEDIAMETRICS_PROP_OUTPUTDEVICES, &device_strings) == OK) ||
        (!isTrack && mAudioAnalytics->mAnalyticsState->timeMachine().get(
         key, AMEDIAMETRICS_PROP_INPUTDEVICES, &device_strings) == OK)) {

        device = deviceFromStringPairs(device_strings);
        ALOGV("device = %s => %d", device_strings.c_str(), device);
    }
    std::lock_guard l(mLock);
    saveAsItems_l(device, deviceTimeNs, type, deviceVolume,
                  maxVolumeDurationNs, maxVolume, minVolumeDurationNs, minVolume);
}

void AudioPowerUsage::checkMode(const std::shared_ptr<const mediametrics::Item>& item)
{
    std::string mode;
    if (!item->getString(AMEDIAMETRICS_PROP_AUDIOMODE, &mode)) return;

    std::lock_guard l(mLock);
    if (mode == mMode) return;  // no change in mode.

    if (mMode == "AUDIO_MODE_IN_CALL") { // leaving call mode
        const int64_t endCallNs = item->getTimestamp();
        const int64_t durationNs = endCallNs - mDeviceTimeNs;
        const int64_t volumeDurationNs = endCallNs - mVolumeTimeNs;
        if (durationNs > 0) {
            mDeviceVolume = (mDeviceVolume * double(mVolumeTimeNs - mDeviceTimeNs) +
                    mVoiceVolume * double(volumeDurationNs)) / (double)durationNs;
            updateMinMaxVolumeAndDuration(volumeDurationNs, mVoiceVolume,
                          volumeDurationNs, mVoiceVolume,
                          mMaxVoiceVolumeDurationNs, mMaxVoiceVolume,
                          mMinVoiceVolumeDurationNs, mMinVoiceVolume);
            saveAsItems_l(mPrimaryDevice, durationNs, VOICE_CALL_TYPE, mDeviceVolume,
                          mMaxVoiceVolumeDurationNs, mMaxVoiceVolume,
                          mMinVoiceVolumeDurationNs, mMinVoiceVolume);
        }
    } else if (mode == "AUDIO_MODE_IN_CALL") { // entering call mode
        mStartCallNs = item->getTimestamp(); // advisory only

        mDeviceVolume = 0;
        mVolumeTimeNs = mStartCallNs;
        mDeviceTimeNs = mStartCallNs;
    }
    ALOGV("%s: new mode:%s  old mode:%s", __func__, mode.c_str(), mMode.c_str());
    mMode = mode;
}

void AudioPowerUsage::checkVoiceVolume(const std::shared_ptr<const mediametrics::Item>& item)
{
    double voiceVolume = 0.;
    if (!item->getDouble(AMEDIAMETRICS_PROP_VOICEVOLUME, &voiceVolume)) return;

    std::lock_guard l(mLock);
    if (voiceVolume == mVoiceVolume) return;  // no change in volume

    // we only track average device volume when we are in-call
    if (mMode == "AUDIO_MODE_IN_CALL") {
        const int64_t timeNs = item->getTimestamp();
        const int64_t durationNs = timeNs - mDeviceTimeNs;
        const int64_t volumeDurationNs = timeNs - mVolumeTimeNs;
        if (durationNs > 0) {
            mDeviceVolume = (mDeviceVolume * double(mVolumeTimeNs - mDeviceTimeNs) +
                    mVoiceVolume * double(volumeDurationNs)) / (double)durationNs;
            mVolumeTimeNs = timeNs;
            updateMinMaxVolumeAndDuration(volumeDurationNs, mVoiceVolume,
                          volumeDurationNs, mVoiceVolume,
                          mMaxVoiceVolumeDurationNs, mMaxVoiceVolume,
                          mMinVoiceVolumeDurationNs, mMinVoiceVolume);
        }
    }
    ALOGV("%s: new voice volume:%lf  old voice volume:%lf", __func__, voiceVolume, mVoiceVolume);
    mVoiceVolume = voiceVolume;
}

void AudioPowerUsage::checkCreatePatch(const std::shared_ptr<const mediametrics::Item>& item)
{
    std::string outputDevices;
    if (!item->get(AMEDIAMETRICS_PROP_OUTPUTDEVICES, &outputDevices)) return;

    const std::string& key = item->getKey();
    std::string flags;
    if (mAudioAnalytics->mAnalyticsState->timeMachine().get(
         key, AMEDIAMETRICS_PROP_FLAGS, &flags) != OK) return;

    if (flags.find("AUDIO_OUTPUT_FLAG_PRIMARY") == std::string::npos) return;

    const int32_t device = deviceFromStringPairs(outputDevices);

    std::lock_guard l(mLock);
    if (mPrimaryDevice == device) return;

    if (mMode == "AUDIO_MODE_IN_CALL") {
        // Save statistics
        const int64_t endDeviceNs = item->getTimestamp();
        const int64_t durationNs = endDeviceNs - mDeviceTimeNs;
        const int64_t volumeDurationNs = endDeviceNs - mVolumeTimeNs;
        if (durationNs > 0) {
            mDeviceVolume = (mDeviceVolume * double(mVolumeTimeNs - mDeviceTimeNs) +
                    mVoiceVolume * double(volumeDurationNs)) / (double)durationNs;
            updateMinMaxVolumeAndDuration(volumeDurationNs, mVoiceVolume,
                          volumeDurationNs, mVoiceVolume,
                          mMaxVoiceVolumeDurationNs, mMaxVoiceVolume,
                          mMinVoiceVolumeDurationNs, mMinVoiceVolume);
            saveAsItems_l(mPrimaryDevice, durationNs, VOICE_CALL_TYPE, mDeviceVolume,
                          mMaxVoiceVolumeDurationNs, mMaxVoiceVolume,
                          mMinVoiceVolumeDurationNs, mMinVoiceVolume);
        }
        // reset statistics
        mDeviceVolume = 0;
        mDeviceTimeNs = endDeviceNs;
        mVolumeTimeNs = endDeviceNs;
        mMaxVoiceVolume = AMEDIAMETRICS_INITIAL_MAX_VOLUME;
        mMinVoiceVolume = AMEDIAMETRICS_INITIAL_MIN_VOLUME;
        mMaxVoiceVolumeDurationNs = 0;
        mMinVoiceVolumeDurationNs = 0;
    }
    ALOGV("%s: new primary device:%#x  old primary device:%#x", __func__, device, mPrimaryDevice);
    mPrimaryDevice = device;
}

AudioPowerUsage::AudioPowerUsage(
        AudioAnalytics *audioAnalytics, const std::shared_ptr<StatsdLog>& statsdLog)
    : mAudioAnalytics(audioAnalytics)
    , mStatsdLog(statsdLog)
    , mDisabled(property_get_bool(PROP_AUDIO_METRICS_DISABLED, AUDIO_METRICS_DISABLED_DEFAULT))
    , mIntervalHours(property_get_int32(PROP_AUDIO_METRICS_INTERVAL_HR, INTERVAL_HR_DEFAULT))
{
    ALOGD("%s", __func__);
    ALOGI_IF(mDisabled, "AudioPowerUsage is disabled.");
    collect(); // send items
}

AudioPowerUsage::~AudioPowerUsage()
{
    ALOGD("%s", __func__);
}

void AudioPowerUsage::clear()
{
    std::lock_guard _l(mLock);
    mItems.clear();
}

void AudioPowerUsage::collect()
{
    std::lock_guard _l(mLock);
    for (const auto &item : mItems) {
        sendItem(item);
    }
    mItems.clear();
    mAudioAnalytics->mTimedAction.postIn(
        mIntervalHours <= 0 ? std::chrono::seconds(5) : std::chrono::hours(mIntervalHours),
        [this](){ collect(); });
}

std::pair<std::string, int32_t> AudioPowerUsage::dump(int limit) const {
    if (limit <= 2) {
        return {{}, 0};
    }
    std::lock_guard _l(mLock);
    if (mDisabled) {
        return {"AudioPowerUsage disabled\n", 1};
    }
    if (mItems.empty()) {
        return {"AudioPowerUsage empty\n", 1};
    }

    int slot = 1;
    std::stringstream ss;
    ss << "AudioPowerUsage interval " << mIntervalHours << " hours:\n";
    for (const auto &item : mItems) {
        if (slot >= limit - 1) {
            ss << "-- AudioPowerUsage may be truncated!\n";
            ++slot;
            break;
        }
        ss << " " << slot << " " << item->toString() << "\n";
        slot++;
    }
    return { ss.str(), slot };
}

} // namespace android::mediametrics
