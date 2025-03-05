/*
 * Copyright 2024, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CODEC_CAPABILITIES_H_

#define CODEC_CAPABILITIES_H_

#include <media/AudioCapabilities.h>
#include <media/CodecCapabilitiesUtils.h>
#include <media/EncoderCapabilities.h>
#include <media/VideoCapabilities.h>
#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/MediaCodecConstants.h>

#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/Vector.h>
#include <utils/StrongPointer.h>

namespace android {

struct CodecCapabilities {

    static bool SupportsBitrate(Range<int32_t> bitrateRange,
            const sp<AMessage> &format);

    /**
     * Retrieve the codec capabilities for a certain {@code mime type}, {@code
     * profile} and {@code level}.  If the type, or profile-level combination
     * is not understood by the framework, it returns null.
     * <p class=note> In {@link android.os.Build.VERSION_CODES#M}, calling this
     * method without calling any method of the {@link MediaCodecList} class beforehand
     * results in a {@link NullPointerException}.</p>
     */
    static std::shared_ptr<CodecCapabilities> CreateFromProfileLevel(std::string mediaType,
                int32_t profile, int32_t level, int32_t maxConcurrentInstances = -1);

    CodecCapabilities() {};

    /**
     * Init CodecCapabilities with settings.
     */
    void init(std::vector<ProfileLevel> profLevs, std::vector<uint32_t> colFmts, bool encoder,
            sp<AMessage> &defaultFormat, sp<AMessage> &capabilitiesInfo,
            int32_t maxConcurrentInstances = 0);

    /**
     * Returns the media type for which this codec-capability object was created.
     */
    const std::string& getMediaType();

    /**
     * Returns the supported profile levels.
     */
    const std::vector<ProfileLevel>& getProfileLevels();

    /**
     * Returns the supported color formats.
     */
    std::vector<uint32_t> getColorFormats() const;

    /**
     * Returns a media format with default values for configurations that have defaults.
     */
    sp<AMessage> getDefaultFormat() const;

    /**
     * Returns the max number of the supported concurrent codec instances.
     * <p>
     * This is a hint for an upper bound. Applications should not expect to successfully
     * operate more instances than the returned value, but the actual number of
     * concurrently operable instances may be less as it depends on the available
     * resources at time of use.
     */
    int32_t getMaxSupportedInstances() const;

    /**
     * Returns the audio capabilities or {@code null} if this is not an audio codec.
     */
    std::shared_ptr<AudioCapabilities> getAudioCapabilities() const;

    /**
     * Returns the video capabilities or {@code null} if this is not a video codec.
     */
    std::shared_ptr<VideoCapabilities> getVideoCapabilities() const;

    /**
     * Returns the encoding capabilities or {@code null} if this is not an encoder.
     */
    std::shared_ptr<EncoderCapabilities> getEncoderCapabilities() const;

    std::vector<std::string> validFeatures() const;

    /**
     * Query codec feature capabilities.
     * <p>
     * These features are supported to be used by the codec.  These
     * include optional features that can be turned on, as well as
     * features that are always on.
     */
    bool isFeatureSupported(const std::string &name) const;

    /**
     * Query codec feature requirements.
     * <p>
     * These features are required to be used by the codec, and as such,
     * they are always turned on.
     */
    bool isFeatureRequired(const std::string &name) const;

    bool isRegular() const;

    /**
    * Query whether codec supports a given {@link MediaFormat}.
    *
    * <p class=note>
    * <strong>Note:</strong> On {@link android.os.Build.VERSION_CODES#LOLLIPOP},
    * {@code format} must not contain a {@linkplain MediaFormat#KEY_FRAME_RATE
    * frame rate}. Use
    * <code class=prettyprint>format.setString(MediaFormat.KEY_FRAME_RATE, null)</code>
    * to clear any existing frame rate setting in the format.
    * <p>
    *
    * The following table summarizes the format keys considered by this method.
    * This is especially important to consider when targeting a higher SDK version than the
    * minimum SDK version, as this method will disregard some keys on devices below the target
    * SDK version.
    *
    * <table style="width: 0%">
    *  <thead>
    *   <tr>
    *    <th rowspan=3>OS Version(s)</th>
    *    <td colspan=3>{@code MediaFormat} keys considered for</th>
    *   </tr><tr>
    *    <th>Audio Codecs</th>
    *    <th>Video Codecs</th>
    *    <th>Encoders</th>
    *   </tr>
    *  </thead>
    *  <tbody>
    *   <tr>
    *    <td>{@link android.os.Build.VERSION_CODES#LOLLIPOP}</td>
    *    <td rowspan=3>{@link MediaFormat#KEY_MIME}<sup>*</sup>,<br>
    *        {@link MediaFormat#KEY_SAMPLE_RATE},<br>
    *        {@link MediaFormat#KEY_CHANNEL_COUNT},</td>
    *    <td>{@link MediaFormat#KEY_MIME}<sup>*</sup>,<br>
    *        {@link CodecCapabilities#FEATURE_AdaptivePlayback}<sup>D</sup>,<br>
    *        {@link CodecCapabilities#FEATURE_SecurePlayback}<sup>D</sup>,<br>
    *        {@link CodecCapabilities#FEATURE_TunneledPlayback}<sup>D</sup>,<br>
    *        {@link MediaFormat#KEY_WIDTH},<br>
    *        {@link MediaFormat#KEY_HEIGHT},<br>
    *        <strong>no</strong> {@code KEY_FRAME_RATE}</td>
    *    <td rowspan=10>as to the left, plus<br>
    *        {@link MediaFormat#KEY_BITRATE_MODE},<br>
    *        {@link MediaFormat#KEY_PROFILE}
    *        (and/or {@link MediaFormat#KEY_AAC_PROFILE}<sup>~</sup>),<br>
    *        <!-- {link MediaFormat#KEY_QUALITY},<br> -->
    *        {@link MediaFormat#KEY_COMPLEXITY}
    *        (and/or {@link MediaFormat#KEY_FLAC_COMPRESSION_LEVEL}<sup>~</sup>)</td>
    *   </tr><tr>
    *    <td>{@link android.os.Build.VERSION_CODES#LOLLIPOP_MR1}</td>
    *    <td rowspan=2>as above, plus<br>
    *        {@link MediaFormat#KEY_FRAME_RATE}</td>
    *   </tr><tr>
    *    <td>{@link android.os.Build.VERSION_CODES#M}</td>
    *   </tr><tr>
    *    <td>{@link android.os.Build.VERSION_CODES#N}</td>
    *    <td rowspan=2>as above, plus<br>
    *        {@link MediaFormat#KEY_PROFILE},<br>
    *        <!-- {link MediaFormat#KEY_MAX_BIT_RATE},<br> -->
    *        {@link MediaFormat#KEY_BIT_RATE}</td>
    *    <td rowspan=2>as above, plus<br>
    *        {@link MediaFormat#KEY_PROFILE},<br>
    *        {@link MediaFormat#KEY_LEVEL}<sup>+</sup>,<br>
    *        <!-- {link MediaFormat#KEY_MAX_BIT_RATE},<br> -->
    *        {@link MediaFormat#KEY_BIT_RATE},<br>
    *        {@link CodecCapabilities#FEATURE_IntraRefresh}<sup>E</sup></td>
    *   </tr><tr>
    *    <td>{@link android.os.Build.VERSION_CODES#N_MR1}</td>
    *   </tr><tr>
    *    <td>{@link android.os.Build.VERSION_CODES#O}</td>
    *    <td rowspan=3 colspan=2>as above, plus<br>
    *        {@link CodecCapabilities#FEATURE_PartialFrame}<sup>D</sup></td>
    *   </tr><tr>
    *    <td>{@link android.os.Build.VERSION_CODES#O_MR1}</td>
    *   </tr><tr>
    *    <td>{@link android.os.Build.VERSION_CODES#P}</td>
    *   </tr><tr>
    *    <td>{@link android.os.Build.VERSION_CODES#Q}</td>
    *    <td colspan=2>as above, plus<br>
    *        {@link CodecCapabilities#FEATURE_FrameParsing}<sup>D</sup>,<br>
    *        {@link CodecCapabilities#FEATURE_MultipleFrames},<br>
    *        {@link CodecCapabilities#FEATURE_DynamicTimestamp}</td>
    *   </tr><tr>
    *    <td>{@link android.os.Build.VERSION_CODES#R}</td>
    *    <td colspan=2>as above, plus<br>
    *        {@link CodecCapabilities#FEATURE_LowLatency}<sup>D</sup></td>
    *   </tr>
    *   <tr>
    *    <td colspan=4>
    *     <p class=note><strong>Notes:</strong><br>
    *      *: must be specified; otherwise, method returns {@code false}.<br>
    *      +: method does not verify that the format parameters are supported
    *      by the specified level.<br>
    *      D: decoders only<br>
    *      E: encoders only<br>
    *      ~: if both keys are provided values must match
    *    </td>
    *   </tr>
    *  </tbody>
    * </table>
    *
    * @param format media format with optional feature directives.
    * @return whether the codec capabilities support the given format
    *         and feature requests.
    */
    bool isFormatSupported(const sp<AMessage> &format) const;

    /**
     * If the CodecCapabilities contains an AudioCapabilities.
     *
     * Not a public API to users.
     */
    bool isAudio() const;

    /**
     * If the CodecCapabilities contains a VideoCapabilities.
     *
     * Not a public API to users.
     */
    bool isVideo() const;

    /**
     * If the CodecCapabilities contains an EncoderCapabilities.
     *
     * Not a public API to users.
     */
    bool isEncoder() const;

private:
    std::string mMediaType;
    std::vector<ProfileLevel> mProfileLevels;
    std::vector<uint32_t> mColorFormats;
    int32_t mMaxSupportedInstances;

    sp<AMessage> mDefaultFormat;
    sp<AMessage> mCapabilitiesInfo;

    // Features
    std::set<std::string> mFeaturesSupported;
    std::set<std::string> mFeaturesRequired;

    std::shared_ptr<AudioCapabilities> mAudioCaps;
    std::shared_ptr<VideoCapabilities> mVideoCaps;
    std::shared_ptr<EncoderCapabilities> mEncoderCaps;

    bool supportsProfileLevel(int32_t profile, int32_t level) const;
    std::vector<Feature> getValidFeatures() const;
    int32_t getErrors() const;
};

}  // namespace android

#endif // CODEC_CAPABILITIES_H_