/*
 * Copyright (C) 2024 The Android Open Source Project
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
#define LOG_TAG "NdkMediaCodecInfo"

#include "NdkMediaCodecInfoPriv.h"

#include <media/NdkMediaFormatPriv.h>

using namespace android;

extern "C" {

// Utils

EXPORT
void AIntRange_delete(AIntRange *range) {
    free(range);
}

EXPORT
void ADoubleRange_delete(ADoubleRange *range) {
    free(range);
}

// AMediaCodecInfo

EXPORT
const char* AMediaCodecInfo_getCanonicalName(const AMediaCodecInfo *info) {
    if (info == nullptr || info->mInfo == nullptr) {
        return nullptr;
    }

    return info->mInfo->getCodecName();
}

EXPORT
bool AMediaCodecInfo_isEncoder(const AMediaCodecInfo *info) {
    return info->mInfo->isEncoder();
}

EXPORT
bool AMediaCodecInfo_isVendor(const AMediaCodecInfo *info) {
    int32_t attributes = info->mInfo->getAttributes();
    return (attributes & android::MediaCodecInfo::kFlagIsVendor);
}

EXPORT
AMediaCodecType AMediaCodecInfo_getMediaCodecInfoType(const AMediaCodecInfo *info) {
    if (info == nullptr || info->mInfo == nullptr) {
        return (AMediaCodecType)0;
    }

    int32_t attributes = info->mInfo->getAttributes();

    if (attributes & android::MediaCodecInfo::kFlagIsSoftwareOnly) {
        return SOFTWARE_ONLY;
    }
    if (attributes & android::MediaCodecInfo::kFlagIsHardwareAccelerated) {
        return HARDWARE_ACCELERATED;
    }
    return SOFTWARE_WITH_DEVICE_ACCESS;
}

EXPORT
const char* AMediaCodecInfo_getMediaType(const AMediaCodecInfo *info) {
    if (info == nullptr || info->mInfo == nullptr) {
        return nullptr;
    }

    return info->mMediaType.c_str();
}

EXPORT
int32_t AMediaCodecInfo_getMaxSupportedInstances(const AMediaCodecInfo *info) {
    if (info == nullptr) {
        return -1;
    }

    return info->mCodecCaps->getMaxSupportedInstances();
}

EXPORT
int32_t AMediaCodecInfo_isFeatureSupported(const AMediaCodecInfo *info, const char *featureName) {
    if (featureName == nullptr) {
        return -1;
    }
    return info->mCodecCaps->isFeatureSupported(std::string(featureName));
}

EXPORT
int32_t AMediaCodecInfo_isFeatureRequired(const AMediaCodecInfo *info, const char *featureName) {
    if (featureName == nullptr) {
        return -1;
    }
    return info->mCodecCaps->isFeatureRequired(std::string(featureName));
}

EXPORT
int32_t AMediaCodecInfo_isFormatSupported(const AMediaCodecInfo *info, const AMediaFormat *format) {
    if (format == nullptr) {
        return -1;
    }

    sp<AMessage> nativeFormat;
    AMediaFormat_getFormat(format, &nativeFormat);

    return info->mCodecCaps->isFormatSupported(nativeFormat);
}

EXPORT
media_status_t AMediaCodecInfo_getAudioCapabilities(const AMediaCodecInfo *info,
        const ACodecAudioCapabilities **outAudioCaps) {
    if (info == nullptr || info->mInfo == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    *outAudioCaps = info->mAAudioCaps.get();

    if ((*outAudioCaps) == nullptr) {
        return AMEDIA_ERROR_UNSUPPORTED;
    }

    return AMEDIA_OK;
}

EXPORT
media_status_t AMediaCodecInfo_getVideoCapabilities(const AMediaCodecInfo *info,
        const ACodecVideoCapabilities **outVideoCaps) {
    if (info == nullptr || info->mInfo == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    *outVideoCaps = info->mAVideoCaps.get();

    if ((*outVideoCaps) == nullptr) {
        return AMEDIA_ERROR_UNSUPPORTED;
    }

    return AMEDIA_OK;
}

EXPORT
media_status_t AMediaCodecInfo_getEncoderCapabilities(const AMediaCodecInfo *info,
        const ACodecEncoderCapabilities **outEncoderCaps) {
    if (info == nullptr || info->mInfo == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    *outEncoderCaps = info->mAEncoderCaps.get();

    if ((*outEncoderCaps) == nullptr) {
        return AMEDIA_ERROR_UNSUPPORTED;
    }

    return AMEDIA_OK;
}

// ACodecAudioCapabilities

EXPORT
media_status_t ACodecAudioCapabilities_getBitrateRange(const ACodecAudioCapabilities *audioCaps,
        AIntRange *outRange) {
    if (audioCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    const Range<int32_t>& bitrateRange = audioCaps->mAudioCaps->getBitrateRange();
    outRange->mLower = bitrateRange.lower();
    outRange->mUpper = bitrateRange.upper();

    return AMEDIA_OK;
}

EXPORT
media_status_t ACodecAudioCapabilities_getSupportedSampleRates(
        const ACodecAudioCapabilities *audioCaps, const int **outArrayPtr, size_t *outCount) {
    if (audioCaps == nullptr || outArrayPtr == nullptr || outCount == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    if (audioCaps->mSampleRates.empty()) {
        return AMEDIA_ERROR_UNSUPPORTED;
    }

    *outArrayPtr = audioCaps->mSampleRates.data();
    *outCount = audioCaps->mSampleRates.size();

    return AMEDIA_OK;
}

EXPORT
media_status_t ACodecAudioCapabilities_getSupportedSampleRateRanges(
        const ACodecAudioCapabilities *audioCaps, const AIntRange **outArrayPtr, size_t *outCount) {
    if (audioCaps == nullptr || outArrayPtr == nullptr || outCount == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    *outArrayPtr = audioCaps->mSampleRateRanges.data();
    *outCount = audioCaps->mSampleRateRanges.size();

    return AMEDIA_OK;
}

EXPORT
int32_t ACodecAudioCapabilities_getMaxInputChannelCount(const ACodecAudioCapabilities *audioCaps) {
    if (audioCaps == nullptr) {
        return -1;
    }
    return audioCaps->mAudioCaps->getMaxInputChannelCount();
}

EXPORT
int32_t ACodecAudioCapabilities_getMinInputChannelCount(const ACodecAudioCapabilities *audioCaps) {
    if (audioCaps == nullptr) {
        return -1;
    }
    return audioCaps->mAudioCaps->getMinInputChannelCount();
}

EXPORT
media_status_t ACodecAudioCapabilities_getInputChannelCountRanges(
        const ACodecAudioCapabilities *audioCaps, const AIntRange **outArrayPtr, size_t *outCount) {
    if (audioCaps == nullptr || outArrayPtr == nullptr || outCount == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    *outArrayPtr = audioCaps->mInputChannelCountRanges.data();
    *outCount = audioCaps->mInputChannelCountRanges.size();

    return AMEDIA_OK;
}

EXPORT
int32_t ACodecAudioCapabilities_isSampleRateSupported(const ACodecAudioCapabilities *audioCaps,
        int32_t sampleRate) {
    if (audioCaps == nullptr) {
        return -1;
    }
    return audioCaps->mAudioCaps->isSampleRateSupported(sampleRate);
}

// ACodecPerformancePoint

EXPORT
ACodecPerformancePoint* ACodecPerformancePoint_create(int32_t width, int32_t height,
        int32_t frameRate) {
    return new ACodecPerformancePoint(
            std::make_shared<VideoCapabilities::PerformancePoint>(width, height, frameRate));
}

EXPORT
media_status_t ACodecPerformancePoint_delete(ACodecPerformancePoint *performancePoint) {
    if (performancePoint == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    delete performancePoint;

    return AMEDIA_OK;
}

EXPORT
bool ACodecPerformancePoint_coversFormat(const ACodecPerformancePoint *performancePoint,
        const AMediaFormat *format) {
    sp<AMessage> nativeFormat;
    AMediaFormat_getFormat(format, &nativeFormat);

    return performancePoint->mPerformancePoint->covers(nativeFormat);
}

EXPORT
bool ACodecPerformancePoint_covers(const ACodecPerformancePoint *one,
        const ACodecPerformancePoint *another) {
    return one->mPerformancePoint->covers(*(another->mPerformancePoint));
}

EXPORT
bool ACodecPerformancePoint_equals(const ACodecPerformancePoint *one,
        const ACodecPerformancePoint *another) {
    return one->mPerformancePoint->equals(*(another->mPerformancePoint));
}

// ACodecVideoCapabilities

EXPORT
media_status_t ACodecVideoCapabilities_getBitrateRange(const ACodecVideoCapabilities *videoCaps,
        AIntRange *outRange) {
    if (videoCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    const Range<int32_t>& bitrateRange = videoCaps->mVideoCaps->getBitrateRange();
    outRange->mLower = bitrateRange.lower();
    outRange->mUpper = bitrateRange.upper();

    return AMEDIA_OK;
}

EXPORT
media_status_t ACodecVideoCapabilities_getSupportedWidths(const ACodecVideoCapabilities *videoCaps,
        AIntRange *outRange) {
    if (videoCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    const Range<int32_t>& supportedWidths = videoCaps->mVideoCaps->getSupportedWidths();
    outRange->mLower = supportedWidths.lower();
    outRange->mUpper = supportedWidths.upper();

    return AMEDIA_OK;
}

EXPORT
media_status_t ACodecVideoCapabilities_getSupportedHeights(const ACodecVideoCapabilities *videoCaps,
        AIntRange *outRange) {
    if (videoCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    const Range<int32_t>& supportedHeights = videoCaps->mVideoCaps->getSupportedHeights();
    outRange->mLower = supportedHeights.lower();
    outRange->mUpper = supportedHeights.upper();

    return AMEDIA_OK;
}

EXPORT
int32_t ACodecVideoCapabilities_getWidthAlignment(const ACodecVideoCapabilities *videoCaps) {
    if (videoCaps == nullptr) {
        return -1;
    }
    return videoCaps->mVideoCaps->getWidthAlignment();
}

EXPORT
int32_t ACodecVideoCapabilities_getHeightAlignment(const ACodecVideoCapabilities *videoCaps) {
    if (videoCaps == nullptr) {
        return -1;
    }
    return videoCaps->mVideoCaps->getHeightAlignment();
}

EXPORT
media_status_t ACodecVideoCapabilities_getSupportedFrameRates(
        const ACodecVideoCapabilities *videoCaps, AIntRange *outRange) {
    if (videoCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    const Range<int32_t>& frameRateRange = videoCaps->mVideoCaps->getSupportedFrameRates();
    outRange->mLower = frameRateRange.lower();
    outRange->mUpper = frameRateRange.upper();

    return AMEDIA_OK;
}

EXPORT
media_status_t ACodecVideoCapabilities_getSupportedWidthsFor(
        const ACodecVideoCapabilities *videoCaps, int32_t height, AIntRange *outRange) {
    if (videoCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    std::optional<Range<int32_t>> widthRange = videoCaps->mVideoCaps->getSupportedWidthsFor(height);
    if (!widthRange) {
        return AMEDIA_ERROR_UNSUPPORTED;
    }

    outRange->mLower = widthRange.value().lower();
    outRange->mUpper = widthRange.value().upper();

    return AMEDIA_OK;
}

EXPORT
media_status_t ACodecVideoCapabilities_getSupportedHeightsFor(
        const ACodecVideoCapabilities *videoCaps, int32_t width, AIntRange *outRange) {
    if (videoCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    std::optional<Range<int32_t>> heightRange
            = videoCaps->mVideoCaps->getSupportedHeightsFor(width);
    if (!heightRange) {
        return AMEDIA_ERROR_UNSUPPORTED;
    }

    outRange->mLower = heightRange.value().lower();
    outRange->mUpper = heightRange.value().upper();

    return AMEDIA_OK;
}

EXPORT
media_status_t ACodecVideoCapabilities_getSupportedFrameRatesFor(
        const ACodecVideoCapabilities *videoCaps, int32_t width, int32_t height,
        ADoubleRange *outRange) {
    if (videoCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    std::optional<Range<double>> frameRates
            = videoCaps->mVideoCaps->getSupportedFrameRatesFor(width, height);
    if (!frameRates) {
        return AMEDIA_ERROR_UNSUPPORTED;
    }

    outRange->mLower = frameRates.value().lower();
    outRange->mUpper = frameRates.value().upper();

    return AMEDIA_OK;
}

EXPORT
media_status_t ACodecVideoCapabilities_getAchievableFrameRatesFor(
        const ACodecVideoCapabilities *videoCaps, int32_t width, int32_t height,
        ADoubleRange *outRange) {
    if (videoCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    std::optional<Range<double>> frameRates
            = videoCaps->mVideoCaps->getAchievableFrameRatesFor(width, height);
    if (!frameRates) {
        return AMEDIA_ERROR_UNSUPPORTED;
    }

    outRange->mLower = frameRates.value().lower();
    outRange->mUpper = frameRates.value().upper();

    return AMEDIA_OK;
}

EXPORT
media_status_t ACodecVideoCapabilities_getSupportedPerformancePoints(
        const ACodecVideoCapabilities *videoCaps,
        const ACodecPerformancePoint **outPerformancePointArray, size_t *outCount) {
    if (videoCaps == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    *outPerformancePointArray = videoCaps->mPerformancePoints.data();
    *outCount = videoCaps->mPerformancePoints.size();

    return AMEDIA_OK;
}

EXPORT
int32_t ACodecVideoCapabilities_areSizeAndRateSupported(const ACodecVideoCapabilities *videoCaps,
        int32_t width, int32_t height, double frameRate) {
    if (videoCaps == nullptr) {
        return -1;
    }
    return videoCaps->mVideoCaps->areSizeAndRateSupported(width, height, frameRate);
}

EXPORT
int32_t ACodecVideoCapabilities_isSizeSupported(const ACodecVideoCapabilities *videoCaps,
        int32_t width, int32_t height) {
    if (videoCaps == nullptr) {
        return -1;
    }
    return videoCaps->mVideoCaps->isSizeSupported(width, height);
}

// ACodecEncoderCapabilities

EXPORT
media_status_t ACodecEncoderCapabilities_getQualityRange(
        const ACodecEncoderCapabilities *encoderCaps, AIntRange *outRange) {
    if (encoderCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    const Range<int32_t>& qualityRange = encoderCaps->mEncoderCaps->getQualityRange();
    outRange->mLower = qualityRange.lower();
    outRange->mUpper = qualityRange.upper();

    return AMEDIA_OK;
}

EXPORT
media_status_t ACodecEncoderCapabilities_getComplexityRange(
        const ACodecEncoderCapabilities *encoderCaps, AIntRange *outRange) {
    if (encoderCaps == nullptr || outRange == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }

    const Range<int32_t>& complexityRange = encoderCaps->mEncoderCaps->getComplexityRange();
    outRange->mLower = complexityRange.lower();
    outRange->mUpper = complexityRange.upper();

    return AMEDIA_OK;
}

int32_t ACodecEncoderCapabilities_isBitrateModeSupported(
        const ACodecEncoderCapabilities *encoderCaps, ABiterateMode mode) {
    if (encoderCaps == nullptr) {
        return -1;
    }
    return encoderCaps->mEncoderCaps->isBitrateModeSupported(mode);
}


}