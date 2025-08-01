/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef C2CONFIG_H_
#define C2CONFIG_H_

#include <C2.h>
#include <C2Component.h>
#include <C2Enum.h>
#include <C2ParamDef.h>

/// \defgroup config Component configuration
/// @{

/**
 * Enumerated boolean.
 */
C2ENUM(c2_bool_t, uint32_t,
    C2_FALSE, ///< true
    C2_TRUE,  ///< false
)

typedef C2SimpleValueStruct<c2_bool_t> C2BoolValue;

typedef C2SimpleValueStruct<C2EasyEnum<c2_bool_t>> C2EasyBoolValue;

/**
 * Enumerated set tri-state.
 *
 * Used for optional configurations to distinguish between values set by the client,
 * default values set by the component, or unset values.
 */
C2ENUM(c2_set_t, uint32_t,
    C2_UNSET,   // parameter is unset and has no value
    C2_SET,     // parameter is/has been set by the client
    C2_DEFAULT, // parameter has not been set by the client, but is set by the component
)

/** Enumerations used by configuration parameters */
struct C2Config {
    enum aac_packaging_t : uint32_t;        ///< AAC packaging (RAW vs ADTS)
    enum aac_sbr_mode_t : uint32_t;         ///< AAC SBR mode
    enum api_feature_t : uint64_t;          ///< API features
    enum api_level_t : uint32_t;            ///< API level
    enum bitrate_mode_t : uint32_t;         ///< bitrate control mode
    enum drc_compression_mode_t : int32_t;  ///< DRC compression mode
    enum drc_effect_type_t : int32_t;       ///< DRC effect type
    enum drc_album_mode_t : int32_t;        ///< DRC album mode
    enum hdr_dynamic_metadata_type_t : uint32_t;  ///< HDR dynamic metadata type
    enum hdr_format_t : uint32_t;           ///< HDR format
    enum intra_refresh_mode_t : uint32_t;   ///< intra refresh modes
    enum level_t : uint32_t;                ///< coding level
    enum ordinal_key_t : uint32_t;          ///< work ordering keys
    enum pcm_encoding_t : uint32_t;         ///< PCM encoding
    enum picture_type_t : uint32_t;         ///< picture types
    enum platform_feature_t : uint64_t;     ///< platform features
    enum platform_level_t : uint32_t;       ///< platform level
    enum prepend_header_mode_t : uint32_t;  ///< prepend header operational modes
    enum profile_t : uint32_t;              ///< coding profile
    enum resource_kind_t : uint32_t;        ///< resource kinds
    enum scaling_method_t : uint32_t;       ///< scaling methods
    enum scan_order_t : uint32_t;           ///< scan orders
    enum secure_mode_t : uint32_t;          ///< secure/protected modes
    enum supplemental_info_t : uint32_t;    ///< supplemental information types
    enum tiling_mode_t : uint32_t;          ///< tiling modes
};

struct C2PlatformConfig {
    enum encoding_quality_level_t : uint32_t; ///< encoding quality level
    enum resource_id_t : uint32_t;          ///< resource IDs defined by the platform
    enum tunnel_peek_mode_t: uint32_t;      ///< tunnel peek mode
};

namespace {

enum C2ParamIndexKind : C2Param::type_index_t {
    C2_PARAM_INDEX_INVALID             = 0x0,    ///< do not use
    C2_PARAM_INDEX_STRUCT_START        = 0x1,    ///< struct only indices
    C2_PARAM_INDEX_PARAM_START         = 0x800,  ///< regular parameters
    C2_PARAM_INDEX_CODER_PARAM_START   = 0x1000, ///< en/transcoder parameters
    C2_PARAM_INDEX_PICTURE_PARAM_START = 0x1800, ///< image/video parameters
    C2_PARAM_INDEX_VIDEO_PARAM_START   = 0x2000, ///< video parameters
    C2_PARAM_INDEX_IMAGE_PARAM_START   = 0x2800, ///< image parameters
    C2_PARAM_INDEX_AUDIO_PARAM_START   = 0x3000, ///< image parameters
    C2_PARAM_INDEX_PLATFORM_START      = 0x4000, ///< platform-defined parameters

    /* =================================== structure indices =================================== */

    kParamIndexColorXy = C2_PARAM_INDEX_STRUCT_START,
    kParamIndexMasteringDisplayColorVolume,
    kParamIndexChromaOffset,
    kParamIndexGopLayer,
    kParamIndexSystemResource,

    /* =================================== parameter indices =================================== */

    kParamIndexApiLevel = C2_PARAM_INDEX_PARAM_START,
    kParamIndexApiFeatures,

    /* ------------------------------------ all components ------------------------------------ */

    /* generic component characteristics */
    kParamIndexName,
    kParamIndexAliases,
    kParamIndexKind,
    kParamIndexDomain,
    kParamIndexAttributes,
    kParamIndexTimeStretch,

    /* coding characteristics */
    kParamIndexProfileLevel,
    kParamIndexInitData,
    kParamIndexSupplementalData,
    kParamIndexSubscribedSupplementalData,

    /* pipeline characteristics */
    kParamIndexMediaType,
    __kParamIndexRESERVED_0,
    kParamIndexDelay,
    kParamIndexMaxReferenceAge,
    kParamIndexMaxReferenceCount,
    kParamIndexReorderBufferDepth,
    kParamIndexReorderKey,
    kParamIndexStreamCount,
    kParamIndexSubscribedParamIndices,
    kParamIndexSuggestedBufferCount,
    kParamIndexBatchSize,
    kParamIndexCurrentWork,
    kParamIndexLastWorkQueued,

    /* memory allocation */
    kParamIndexAllocators,
    kParamIndexBlockPools,
    kParamIndexBufferType,
    kParamIndexUsage,
    kParamIndexOutOfMemory,
    kParamIndexMaxBufferSize,

    /* misc. state */
    kParamIndexTripped,
    kParamIndexConfigCounter,

    /* resources */
    kParamIndexResourcesNeeded,
    kParamIndexResourcesReserved,
    kParamIndexOperatingRate,
    kParamIndexRealTimePriority,

    /* protected content */
    kParamIndexSecureMode,
    kParamIndexEncryptedBuffer, // info-buffer, used with SM_READ_PROTECTED_WITH_ENCRYPTED

    /* multiple access unit support */
    kParamIndexLargeFrame,
    kParamIndexAccessUnitInfos, // struct

    /* Region of Interest Encoding parameters */
    kParamIndexQpOffsetMapBuffer, // info-buffer, used to signal qp-offset map for a frame

    /* resource capacity and resources excluded */
    kParamIndexResourcesCapacity,
    kParamIndexResourcesExcluded,

    // deprecated
    kParamIndexDelayRequest = kParamIndexDelay | C2Param::CoreIndex::IS_REQUEST_FLAG,

    /* ------------------------------------ (trans/en)coders ------------------------------------ */

    kParamIndexBitrate = C2_PARAM_INDEX_CODER_PARAM_START,
    kParamIndexBitrateMode,
    kParamIndexQuality,
    kParamIndexComplexity,
    kParamIndexPrependHeaderMode,

    /* --------------------------------- image/video components --------------------------------- */

    kParamIndexPictureSize = C2_PARAM_INDEX_PICTURE_PARAM_START,
    kParamIndexCropRect,
    kParamIndexPixelFormat,
    kParamIndexRotation,
    kParamIndexPixelAspectRatio,
    kParamIndexScaledPictureSize,
    kParamIndexScaledCropRect,
    kParamIndexScalingMethod,
    kParamIndexColorInfo,
    kParamIndexColorAspects,
    kParamIndexHdrStaticMetadata,
    kParamIndexDefaultColorAspects,

    kParamIndexBlockSize,
    kParamIndexBlockCount,
    kParamIndexBlockRate,

    kParamIndexPictureTypeMask,
    kParamIndexPictureType,
    // deprecated
    kParamIndexHdr10PlusMetadata,
    kParamIndexPictureQuantization,
    kParamIndexHdrDynamicMetadata,
    kParamIndexHdrFormat,
    kParamIndexQpOffsetRect,
    kParamIndexQpOffsetRects,

    /* ------------------------------------ video components ------------------------------------ */

    kParamIndexFrameRate = C2_PARAM_INDEX_VIDEO_PARAM_START,
    kParamIndexMaxBitrate,
    kParamIndexMaxFrameRate,
    kParamIndexMaxPictureSize,
    kParamIndexGop,
    kParamIndexSyncFrameInterval,
    kParamIndexRequestSyncFrame,
    kParamIndexTemporalLayering,
    kParamIndexLayerIndex,
    kParamIndexLayerCount,
    kParamIndexIntraRefresh,

    /* ------------------------------------ image components ------------------------------------ */

    kParamIndexTileLayout = C2_PARAM_INDEX_IMAGE_PARAM_START,
    kParamIndexTileHandling,

    /* ------------------------------------ audio components ------------------------------------ */

    kParamIndexSampleRate = C2_PARAM_INDEX_AUDIO_PARAM_START,
    kParamIndexChannelCount,
    kParamIndexPcmEncoding,
    kParamIndexAacPackaging,
    kParamIndexMaxChannelCount,
    kParamIndexAacSbrMode, // aac encode, enum
    kParamIndexDrcEncodedTargetLevel,  // drc, float (dBFS)
    kParamIndexDrcTargetReferenceLevel, // drc, float (dBFS)
    kParamIndexDrcCompression, // drc, enum
    kParamIndexDrcBoostFactor, // drc, float (0-1)
    kParamIndexDrcAttenuationFactor, // drc, float (0-1)
    kParamIndexDrcEffectType, // drc, enum
    kParamIndexDrcOutputLoudness, // drc, float (dBFS)
    kParamIndexDrcAlbumMode, // drc, enum
    kParamIndexAudioFrameSize, // int

    /* ============================== platform-defined parameters ============================== */

    kParamIndexPlatformLevel = C2_PARAM_INDEX_PLATFORM_START, // all, u32
    kParamIndexPlatformFeatures, // all, u64 mask
    kParamIndexStoreIonUsage, // store, struct
    kParamIndexAspectsToDataSpace, // store, struct
    kParamIndexFlexiblePixelFormatDescriptor, // store, struct
    kParamIndexFlexiblePixelFormatDescriptors, // store, struct[]
    kParamIndexDataSpaceToAspects, // store, struct
    kParamIndexDataSpace, // u32
    kParamIndexSurfaceScaling, // u32

    // input surface
    kParamIndexInputSurfaceEos, // input-surface, eos
    kParamIndexTimedControl, // struct
    kParamIndexStartAt, // input-surface, struct
    kParamIndexSuspendAt, // input-surface, struct
    kParamIndexResumeAt, // input-surface, struct
    kParamIndexStopAt, // input-surface, struct
    kParamIndexTimeOffset, // input-surface, int64_t
    kParamIndexMinFrameRate, // input-surface, float
    kParamIndexTimestampGapAdjustment, // input-surface, struct

    kParamIndexSurfaceAllocator, // u32

    // low latency mode
    kParamIndexLowLatencyMode, // bool

    // tunneled codec
    kParamIndexTunneledMode, // struct
    kParamIndexTunnelHandle, // int32[]
    kParamIndexTunnelSystemTime, // int64
    kParamIndexTunnelHoldRender, // bool
    kParamIndexTunnelStartRender, // bool

    // dmabuf allocator
    kParamIndexStoreDmaBufUsage,  // store, struct

    // encoding quality requirements
    kParamIndexEncodingQualityLevel, // encoders, enum

    // encoding statistics, average block qp of a frame
    kParamIndexAverageBlockQuantization, // int32

    // channel mask for decoded audio
    kParamIndexAndroidChannelMask, // uint32

    // allow tunnel peek behavior to be unspecified for app compatibility
    kParamIndexTunnelPeekMode, // tunnel mode, enum

    // input surface
    kParamIndexCaptureFrameRate, // input-surface, float
    kParamIndexStopTimeOffset, // input-surface, int64_t

    // display processing token
    kParamIndexDisplayProcessingToken, // int64_t
};

}

/**
 * Codec 2.0 parameter types follow the following naming convention:
 *
 * C2<group><domain><index><type>
 *
 * E.g. C2StreamPictureSizeInfo: group="" domain="Stream" index="PictureSize" type="Info".
 * Group is somewhat arbitrary, but denotes kind of objects the parameter is defined.
 * At this point we use Component and Store to distinguish basic component/store parameters.
 *
 * Parameter keys are named C2_PARAMKEY_[<group>_]<domain>_<index> as type is not expected
 * to distinguish parameters. E.g. a component could change the type of the parameter and it
 * is not expected users would need to change the key.
 */

/* ----------------------------------------- API level ----------------------------------------- */

enum C2Config::api_level_t : uint32_t {
    API_L0_1 = 0,   ///< support for API level 0.1
};

// read-only
typedef C2GlobalParam<C2Setting, C2SimpleValueStruct<C2Config::api_level_t>, kParamIndexApiLevel>
        C2ApiLevelSetting;
constexpr char C2_PARAMKEY_API_LEVEL[] = "api.level";

C2ENUM(C2Config::api_feature_t, uint64_t,
    API_REFLECTION       = (1U << 0),  ///< ability to list supported parameters
    API_VALUES           = (1U << 1),  ///< ability to list supported values for each parameter
    API_CURRENT_VALUES   = (1U << 2),  ///< ability to list currently supported values for each parameter
    API_DEPENDENCY       = (1U << 3),  ///< have a defined parameter dependency

    API_SAME_INPUT_BUFFER = (1U << 16),   ///< supporting multiple input buffers
                                          ///< backed by the same allocation

    API_BLOCK_FENCES     = (1U << 17),    ///< supporting block fences

    API_STREAMS          = (1ULL << 32),  ///< supporting variable number of streams

    API_TUNNELING        = (1ULL << 48)   ///< tunneling API
)

// read-only
typedef C2GlobalParam<C2Setting, C2SimpleValueStruct<C2Config::api_feature_t>, kParamIndexApiFeatures>
        C2ApiFeaturesSetting;
constexpr char C2_PARAMKEY_API_FEATURES[] = "api.features";

/* ----------------------------- generic component characteristics ----------------------------- */

/**
 * The name of the component.
 *
 * This must contain only alphanumeric characters or dot '.', hyphen '-', plus '+', or
 * underline '_'. The name of each component must be unique.
 *
 * For Android: Component names must start with 'c2.' followed by the company name or abbreviation
 * and another dot, e.g. 'c2.android.'. Use of lowercase is preferred but not required.
 */
// read-only
typedef C2GlobalParam<C2Setting, C2StringValue, kParamIndexName> C2ComponentNameSetting;
constexpr char C2_PARAMKEY_COMPONENT_NAME[]  = "component.name";

/**
 * Alternate names (aliases) of the component.
 *
 * This is a comma ',' separated list of alternate component names. Unlike component names that
 * must be unique, multiple components can have the same alias.
 */
// read-only
typedef C2GlobalParam<C2Setting, C2StringValue, kParamIndexAliases> C2ComponentAliasesSetting;
constexpr char C2_PARAMKEY_COMPONENT_ALIASES[]  = "component.aliases";

/**
 * Component kind.
 */
// read-only
typedef C2GlobalParam<C2Setting, C2SimpleValueStruct<C2Component::kind_t>, kParamIndexKind>
        C2ComponentKindSetting;
constexpr char C2_PARAMKEY_COMPONENT_KIND[]  = "component.kind";

/**
 * Component domain.
 */
// read-only
typedef C2GlobalParam<C2Setting, C2SimpleValueStruct<C2Component::domain_t>, kParamIndexDomain>
        C2ComponentDomainSetting;
constexpr char C2_PARAMKEY_COMPONENT_DOMAIN[]  = "component.domain";

/**
 * Component attributes.
 *
 * These are a set of flags provided by the component characterizing its processing algorithm.
 */
C2ENUM(C2Component::attrib_t, uint64_t,
    ATTRIB_IS_TEMPORAL = 1u << 0, ///< component input ordering matters for processing
)

// read-only
typedef C2GlobalParam<C2Setting, C2SimpleValueStruct<C2Component::attrib_t>, kParamIndexAttributes>
        C2ComponentAttributesSetting;
constexpr char C2_PARAMKEY_COMPONENT_ATTRIBUTES[] = "component.attributes";

/**
 * Time stretching.
 *
 * This is the ratio between the rate of the input timestamp, and the rate of the output timestamp.
 * E.g. if this is 4.0, for every 1 seconds of input timestamp difference, the output shall differ
 * by 4 seconds.
 */
typedef C2GlobalParam<C2Tuning, C2FloatValue, kParamIndexTimeStretch> C2ComponentTimeStretchTuning;
constexpr char C2_PARAMKEY_TIME_STRETCH[]  = "algo.time-stretch";

/* ----------------------------------- coding characteristics ----------------------------------- */

/**
 * Profile and level.
 *
 * Profile determines the tools used by the component.
 * Level determines the level of resources used by the component.
 */

namespace {

// Codec bases are ordered by their date of introduction to the code base.
enum : uint32_t {
    _C2_PL_MP2V_BASE = 0x1000,
    _C2_PL_AAC_BASE  = 0x2000,
    _C2_PL_H263_BASE = 0x3000,
    _C2_PL_MP4V_BASE = 0x4000,
    _C2_PL_AVC_BASE  = 0x5000,
    _C2_PL_HEVC_BASE = 0x6000,
    _C2_PL_VP9_BASE  = 0x7000,
    _C2_PL_DV_BASE   = 0x8000,
    _C2_PL_AV1_BASE  = 0x9000,
    _C2_PL_VP8_BASE  = 0xA000,
    _C2_PL_MPEGH_BASE = 0xB000,     // MPEG-H 3D Audio
    _C2_PL_APV_BASE = 0xC000,     // APV
    _C2_PL_AC4_BASE  = 0xD000,

    C2_PROFILE_LEVEL_VENDOR_START = 0x70000000,
};

}

// Profiles and levels for each codec are ordered based on how they are ordered in the
// corresponding standard documents at introduction, and chronologically afterwards.
enum C2Config::profile_t : uint32_t {
    PROFILE_UNUSED = 0,                         ///< profile is not used by this media type

    // AAC (MPEG-2 Part 7 and MPEG-4 Part 3) profiles
    PROFILE_AAC_LC = _C2_PL_AAC_BASE,           ///< AAC Low-Complexity
    PROFILE_AAC_MAIN,                           ///< AAC Main
    PROFILE_AAC_SSR,                            ///< AAC Scalable Sampling Rate
    PROFILE_AAC_LTP,                            ///< AAC Long Term Prediction
    PROFILE_AAC_HE,                             ///< AAC High-Efficiency
    PROFILE_AAC_SCALABLE,                       ///< AAC Scalable
    PROFILE_AAC_ER_LC,                          ///< AAC Error Resilient Low-Complexity
    PROFILE_AAC_ER_SCALABLE,                    ///< AAC Error Resilient Scalable
    PROFILE_AAC_LD,                             ///< AAC Low Delay
    PROFILE_AAC_HE_PS,                          ///< AAC High-Efficiency Parametric Stereo
    PROFILE_AAC_ELD,                            ///< AAC Enhanced Low Delay
    PROFILE_AAC_XHE,                            ///< AAC Extended High-Efficiency

    // MPEG-2 Video profiles
    PROFILE_MP2V_SIMPLE = _C2_PL_MP2V_BASE,     ///< MPEG-2 Video (H.262) Simple
    PROFILE_MP2V_MAIN,                          ///< MPEG-2 Video (H.262) Main
    PROFILE_MP2V_SNR_SCALABLE,                  ///< MPEG-2 Video (H.262) SNR Scalable
    PROFILE_MP2V_SPATIALLY_SCALABLE,            ///< MPEG-2 Video (H.262) Spatially Scalable
    PROFILE_MP2V_HIGH,                          ///< MPEG-2 Video (H.262) High
    PROFILE_MP2V_422,                           ///< MPEG-2 Video (H.262) 4:2:2
    PROFILE_MP2V_MULTIVIEW,                     ///< MPEG-2 Video (H.262) Multi-view

    // H.263 profiles
    PROFILE_H263_BASELINE = _C2_PL_H263_BASE,   ///< H.263 Baseline (Profile 0)
    PROFILE_H263_H320,                          ///< H.263 H.320 Coding Efficiency Version 2 Backward-Compatibility (Profile 1)
    PROFILE_H263_V1BC,                          ///< H.263 Version 1 Backward-Compatibility (Profile 2)
    PROFILE_H263_ISWV2,                         ///< H.263 Version 2 Interactive and Streaming Wireless (Profile 3)
    PROFILE_H263_ISWV3,                         ///< H.263 Version 3 Interactive and Streaming Wireless (Profile 4)
    PROFILE_H263_HIGH_COMPRESSION,              ///< H.263 Conversational High Compression (Profile 5)
    PROFILE_H263_INTERNET,                      ///< H.263 Conversational Internet (Profile 6)
    PROFILE_H263_INTERLACE,                     ///< H.263 Conversational Interlace (Profile 7)
    PROFILE_H263_HIGH_LATENCY,                  ///< H.263 High Latency (Profile 8)

    // MPEG-4 Part 2 (Video) Natural Visual Profiles
    PROFILE_MP4V_SIMPLE,                        ///< MPEG-4 Video Simple
    PROFILE_MP4V_SIMPLE_SCALABLE,               ///< MPEG-4 Video Simple Scalable
    PROFILE_MP4V_CORE,                          ///< MPEG-4 Video Core
    PROFILE_MP4V_MAIN,                          ///< MPEG-4 Video Main
    PROFILE_MP4V_NBIT,                          ///< MPEG-4 Video N-Bit
    PROFILE_MP4V_ARTS,                          ///< MPEG-4 Video Advanced Realtime Simple
    PROFILE_MP4V_CORE_SCALABLE,                 ///< MPEG-4 Video Core Scalable
    PROFILE_MP4V_ACE,                           ///< MPEG-4 Video Advanced Coding Efficiency
    PROFILE_MP4V_ADVANCED_CORE,                 ///< MPEG-4 Video Advanced Core
    PROFILE_MP4V_SIMPLE_STUDIO,                 ///< MPEG-4 Video Simple Studio
    PROFILE_MP4V_CORE_STUDIO,                   ///< MPEG-4 Video Core Studio
    PROFILE_MP4V_ADVANCED_SIMPLE,               ///< MPEG-4 Video Advanced Simple
    PROFILE_MP4V_FGS,                           ///< MPEG-4 Video Fine Granularity Scalable

    // AVC / MPEG-4 Part 10 (H.264) profiles
    PROFILE_AVC_BASELINE = _C2_PL_AVC_BASE,     ///< AVC (H.264) Baseline
    PROFILE_AVC_CONSTRAINED_BASELINE,           ///< AVC (H.264) Constrained Baseline
    PROFILE_AVC_MAIN,                           ///< AVC (H.264) Main
    PROFILE_AVC_EXTENDED,                       ///< AVC (H.264) Extended
    PROFILE_AVC_HIGH,                           ///< AVC (H.264) High
    PROFILE_AVC_PROGRESSIVE_HIGH,               ///< AVC (H.264) Progressive High
    PROFILE_AVC_CONSTRAINED_HIGH,               ///< AVC (H.264) Constrained High
    PROFILE_AVC_HIGH_10,                        ///< AVC (H.264) High 10
    PROFILE_AVC_PROGRESSIVE_HIGH_10,            ///< AVC (H.264) Progressive High 10
    PROFILE_AVC_HIGH_422,                       ///< AVC (H.264) High 4:2:2
    PROFILE_AVC_HIGH_444_PREDICTIVE,            ///< AVC (H.264) High 4:4:4 Predictive
    PROFILE_AVC_HIGH_10_INTRA,                  ///< AVC (H.264) High 10 Intra
    PROFILE_AVC_HIGH_422_INTRA,                 ///< AVC (H.264) High 4:2:2 Intra
    PROFILE_AVC_HIGH_444_INTRA,                 ///< AVC (H.264) High 4:4:4 Intra
    PROFILE_AVC_CAVLC_444_INTRA,                ///< AVC (H.264) CAVLC 4:4:4 Intra
    PROFILE_AVC_SCALABLE_BASELINE = _C2_PL_AVC_BASE + 0x100,  ///< AVC (H.264) Scalable Baseline
    PROFILE_AVC_SCALABLE_CONSTRAINED_BASELINE,  ///< AVC (H.264) Scalable Constrained Baseline
    PROFILE_AVC_SCALABLE_HIGH,                  ///< AVC (H.264) Scalable High
    PROFILE_AVC_SCALABLE_CONSTRAINED_HIGH,      ///< AVC (H.264) Scalable Constrained High
    PROFILE_AVC_SCALABLE_HIGH_INTRA,            ///< AVC (H.264) Scalable High Intra
    PROFILE_AVC_MULTIVIEW_HIGH = _C2_PL_AVC_BASE + 0x200,  ///< AVC (H.264) Multiview High
    PROFILE_AVC_STEREO_HIGH,                    ///< AVC (H.264) Stereo High
    PROFILE_AVC_MFC_HIGH,                       ///< AVC (H.264) MFC High
    PROFILE_AVC_MULTIVIEW_DEPTH_HIGH = _C2_PL_AVC_BASE + 0x300,  ///< AVC (H.264) Multiview Depth High
    PROFILE_AVC_MFC_DEPTH_HIGH,                 ///< AVC (H.264) MFC Depth High
    PROFILE_AVC_ENHANCED_MULTIVIEW_DEPTH_HIGH = _C2_PL_AVC_BASE + 0x400,  ///< AVC (H.264) Enhanced Multiview Depth High

    // HEVC profiles
    PROFILE_HEVC_MAIN = _C2_PL_HEVC_BASE,       ///< HEVC (H.265) Main
    PROFILE_HEVC_MAIN_10,                       ///< HEVC (H.265) Main 10
    PROFILE_HEVC_MAIN_STILL,                    ///< HEVC (H.265) Main Still Picture
    PROFILE_HEVC_MONO = _C2_PL_HEVC_BASE + 0x100,  ///< HEVC (H.265) Monochrome
    PROFILE_HEVC_MONO_12,                       ///< HEVC (H.265) Monochrome 12
    PROFILE_HEVC_MONO_16,                       ///< HEVC (H.265) Monochrome 16
    PROFILE_HEVC_MAIN_12,                       ///< HEVC (H.265) Main 12
    PROFILE_HEVC_MAIN_422_10,                   ///< HEVC (H.265) Main 4:2:2 10
    PROFILE_HEVC_MAIN_422_12,                   ///< HEVC (H.265) Main 4:2:2 12
    PROFILE_HEVC_MAIN_444,                      ///< HEVC (H.265) Main 4:4:4
    PROFILE_HEVC_MAIN_444_10,                   ///< HEVC (H.265) Main 4:4:4 10
    PROFILE_HEVC_MAIN_444_12,                   ///< HEVC (H.265) Main 4:4:4 12
    PROFILE_HEVC_MAIN_INTRA,                    ///< HEVC (H.265) Main Intra
    PROFILE_HEVC_MAIN_10_INTRA,                 ///< HEVC (H.265) Main 10 Intra
    PROFILE_HEVC_MAIN_12_INTRA,                 ///< HEVC (H.265) Main 12 Intra
    PROFILE_HEVC_MAIN_422_10_INTRA,             ///< HEVC (H.265) Main 4:2:2 10 Intra
    PROFILE_HEVC_MAIN_422_12_INTRA,             ///< HEVC (H.265) Main 4:2:2 12 Intra
    PROFILE_HEVC_MAIN_444_INTRA,                ///< HEVC (H.265) Main 4:4:4 Intra
    PROFILE_HEVC_MAIN_444_10_INTRA,             ///< HEVC (H.265) Main 4:4:4 10 Intra
    PROFILE_HEVC_MAIN_444_12_INTRA,             ///< HEVC (H.265) Main 4:4:4 12 Intra
    PROFILE_HEVC_MAIN_444_16_INTRA,             ///< HEVC (H.265) Main 4:4:4 16 Intra
    PROFILE_HEVC_MAIN_444_STILL,                ///< HEVC (H.265) Main 4:4:4 Still Picture
    PROFILE_HEVC_MAIN_444_16_STILL,             ///< HEVC (H.265) Main 4:4:4 16 Still Picture
    PROFILE_HEVC_HIGH_444 = _C2_PL_HEVC_BASE + 0x200,  ///< HEVC (H.265) High Throughput 4:4:4
    PROFILE_HEVC_HIGH_444_10,                   ///< HEVC (H.265) High Throughput 4:4:4 10
    PROFILE_HEVC_HIGH_444_14,                   ///< HEVC (H.265) High Throughput 4:4:4 14
    PROFILE_HEVC_HIGH_444_16_INTRA,             ///< HEVC (H.265) High Throughput 4:4:4 16 Intra
    PROFILE_HEVC_SX_MAIN = _C2_PL_HEVC_BASE + 0x300,  ///< HEVC (H.265) Screen-Extended Main
    PROFILE_HEVC_SX_MAIN_10,                    ///< HEVC (H.265) Screen-Extended Main 10
    PROFILE_HEVC_SX_MAIN_444,                   ///< HEVC (H.265) Screen-Extended Main 4:4:4
    PROFILE_HEVC_SX_MAIN_444_10,                ///< HEVC (H.265) Screen-Extended Main 4:4:4 10
    PROFILE_HEVC_SX_HIGH_444,                   ///< HEVC (H.265) Screen-Extended High Throughput 4:4:4
    PROFILE_HEVC_SX_HIGH_444_10,                ///< HEVC (H.265) Screen-Extended High Throughput 4:4:4 10
    PROFILE_HEVC_SX_HIGH_444_14,                ///< HEVC (H.265) Screen-Extended High Throughput 4:4:4 14
    PROFILE_HEVC_MULTIVIEW_MAIN = _C2_PL_HEVC_BASE + 0x400,  ///< HEVC (H.265) Multiview Main
    PROFILE_HEVC_SCALABLE_MAIN = _C2_PL_HEVC_BASE + 0x500,  ///< HEVC (H.265) Scalable Main
    PROFILE_HEVC_SCALABLE_MAIN_10,              ///< HEVC (H.265) Scalable Main 10
    PROFILE_HEVC_SCALABLE_MONO = _C2_PL_HEVC_BASE + 0x600,  ///< HEVC (H.265) Scalable Monochrome
    PROFILE_HEVC_SCALABLE_MONO_12,              ///< HEVC (H.265) Scalable Monochrome 12
    PROFILE_HEVC_SCALABLE_MONO_16,              ///< HEVC (H.265) Scalable Monochrome 16
    PROFILE_HEVC_SCALABLE_MAIN_444,             ///< HEVC (H.265) Scalable Main 4:4:4
    PROFILE_HEVC_3D_MAIN = _C2_PL_HEVC_BASE + 0x700,  ///< HEVC (H.265) 3D Main

    // VP9 profiles
    PROFILE_VP9_0 = _C2_PL_VP9_BASE,            ///< VP9 Profile 0 (4:2:0)
    PROFILE_VP9_1,                              ///< VP9 Profile 1 (4:2:2 or 4:4:4)
    PROFILE_VP9_2,                              ///< VP9 Profile 2 (4:2:0, 10 or 12 bit)
    PROFILE_VP9_3,                              ///< VP9 Profile 3 (4:2:2 or 4:4:4, 10 or 12 bit)

    // Dolby Vision profiles
    PROFILE_DV_AV_PER = _C2_PL_DV_BASE + 0,     ///< Dolby Vision dvav.per profile (deprecated)
    PROFILE_DV_AV_PEN,                          ///< Dolby Vision dvav.pen profile (deprecated)
    PROFILE_DV_HE_DER,                          ///< Dolby Vision dvhe.der profile (deprecated)
    PROFILE_DV_HE_DEN,                          ///< Dolby Vision dvhe.den profile (deprecated)
    PROFILE_DV_HE_04 = _C2_PL_DV_BASE + 4,      ///< Dolby Vision dvhe.04 profile
    PROFILE_DV_HE_05 = _C2_PL_DV_BASE + 5,      ///< Dolby Vision dvhe.05 profile
    PROFILE_DV_HE_DTH,                          ///< Dolby Vision dvhe.dth profile (deprecated)
    PROFILE_DV_HE_07 = _C2_PL_DV_BASE + 7,      ///< Dolby Vision dvhe.07 profile
    PROFILE_DV_HE_08 = _C2_PL_DV_BASE + 8,      ///< Dolby Vision dvhe.08 profile
    PROFILE_DV_AV_09 = _C2_PL_DV_BASE + 9,      ///< Dolby Vision dvav.09 profile
    PROFILE_DV_AV1_10 = _C2_PL_DV_BASE + 10,    ///< Dolby Vision dav1.10 profile

    // AV1 profiles
    PROFILE_AV1_0 = _C2_PL_AV1_BASE,            ///< AV1 Profile 0 (4:2:0, 8 to 10 bit)
    PROFILE_AV1_1,                              ///< AV1 Profile 1 (8 to 10 bit)
    PROFILE_AV1_2,                              ///< AV1 Profile 2 (8 to 12 bit)

    // VP8 profiles
    PROFILE_VP8_0 = _C2_PL_VP8_BASE,            ///< VP8 Profile 0
    PROFILE_VP8_1,                              ///< VP8 Profile 1
    PROFILE_VP8_2,                              ///< VP8 Profile 2
    PROFILE_VP8_3,                              ///< VP8 Profile 3

    // MPEG-H 3D Audio profiles
    PROFILE_MPEGH_MAIN = _C2_PL_MPEGH_BASE,     ///< MPEG-H Main
    PROFILE_MPEGH_HIGH,                         ///< MPEG-H High
    PROFILE_MPEGH_LC,                           ///< MPEG-H Low-complexity
    PROFILE_MPEGH_BASELINE,                     ///< MPEG-H Baseline

    // Advanced Professional VideoCodec (APV)
    PROFILE_APV_422_10 = _C2_PL_APV_BASE,       ///< APV 422-10 Profile
    PROFILE_APV_422_12,                         ///< APV 422-12 Profile
    PROFILE_APV_444_10,                         ///< APV 444-10 Profile
    PROFILE_APV_444_12,                         ///< APV 444-12 Profile
    PROFILE_APV_4444_10,                        ///< APV 4444-10 Profile
    PROFILE_APV_4444_12,                        ///< APV 4444-12 Profile
    PROFILE_APV_400_10,                         ///< APV 400-10 Profile

    // AC-4 profiles
    // Below profiles are labelled “AC-4 Profile xx.yy” where xx is the bitstream_version
    // and yy is the presentation_version as described in "The MIME codecs parameter", Annex E.13
    // found at https://www.etsi.org/deliver/etsi_ts/103100_103199/10319002/01.02.01_60/ts_10319002v010201p.pdf
    PROFILE_AC4_0_0 = _C2_PL_AC4_BASE,          ///< AC-4 Profile 00.00
    PROFILE_AC4_1_0,                            ///< AC-4 Profile 01.00
    PROFILE_AC4_1_1,                            ///< AC-4 Profile 01.01
    PROFILE_AC4_2_1,                            ///< AC-4 Profile 02.01
    PROFILE_AC4_2_2,                            ///< AC-4 Profile 02.02
};

enum C2Config::level_t : uint32_t {
    LEVEL_UNUSED = 0,                           ///< level is not used by this media type

    // MPEG-2 Video levels
    LEVEL_MP2V_LOW = _C2_PL_MP2V_BASE,          ///< MPEG-2 Video (H.262) Low Level
    LEVEL_MP2V_MAIN,                            ///< MPEG-2 Video (H.262) Main Level
    LEVEL_MP2V_HIGH_1440,                       ///< MPEG-2 Video (H.262) High 1440 Level
    LEVEL_MP2V_HIGH,                            ///< MPEG-2 Video (H.262) High Level
    LEVEL_MP2V_HIGHP,                           ///< MPEG-2 Video (H.262) HighP Level

    // H.263 levels
    LEVEL_H263_10 = _C2_PL_H263_BASE,           ///< H.263 Level 10
    LEVEL_H263_20,                              ///< H.263 Level 20
    LEVEL_H263_30,                              ///< H.263 Level 30
    LEVEL_H263_40,                              ///< H.263 Level 40
    LEVEL_H263_45,                              ///< H.263 Level 45
    LEVEL_H263_50,                              ///< H.263 Level 50
    LEVEL_H263_60,                              ///< H.263 Level 60
    LEVEL_H263_70,                              ///< H.263 Level 70

    // MPEG-4 Part 2 (Video) levels
    LEVEL_MP4V_0 = _C2_PL_MP4V_BASE,            ///< MPEG-4 Video Level 0
    LEVEL_MP4V_0B,                              ///< MPEG-4 Video Level 0b
    LEVEL_MP4V_1,                               ///< MPEG-4 Video Level 1
    LEVEL_MP4V_2,                               ///< MPEG-4 Video Level 2
    LEVEL_MP4V_3,                               ///< MPEG-4 Video Level 3
    LEVEL_MP4V_3B,                              ///< MPEG-4 Video Level 3b
    LEVEL_MP4V_4,                               ///< MPEG-4 Video Level 4
    LEVEL_MP4V_4A,                              ///< MPEG-4 Video Level 4a
    LEVEL_MP4V_5,                               ///< MPEG-4 Video Level 5
    LEVEL_MP4V_6,                               ///< MPEG-4 Video Level 6

    // AVC / MPEG-4 Part 10 (H.264) levels
    LEVEL_AVC_1 = _C2_PL_AVC_BASE,              ///< AVC (H.264) Level 1
    LEVEL_AVC_1B,                               ///< AVC (H.264) Level 1b
    LEVEL_AVC_1_1,                              ///< AVC (H.264) Level 1.1
    LEVEL_AVC_1_2,                              ///< AVC (H.264) Level 1.2
    LEVEL_AVC_1_3,                              ///< AVC (H.264) Level 1.3
    LEVEL_AVC_2,                                ///< AVC (H.264) Level 2
    LEVEL_AVC_2_1,                              ///< AVC (H.264) Level 2.1
    LEVEL_AVC_2_2,                              ///< AVC (H.264) Level 2.2
    LEVEL_AVC_3,                                ///< AVC (H.264) Level 3
    LEVEL_AVC_3_1,                              ///< AVC (H.264) Level 3.1
    LEVEL_AVC_3_2,                              ///< AVC (H.264) Level 3.2
    LEVEL_AVC_4,                                ///< AVC (H.264) Level 4
    LEVEL_AVC_4_1,                              ///< AVC (H.264) Level 4.1
    LEVEL_AVC_4_2,                              ///< AVC (H.264) Level 4.2
    LEVEL_AVC_5,                                ///< AVC (H.264) Level 5
    LEVEL_AVC_5_1,                              ///< AVC (H.264) Level 5.1
    LEVEL_AVC_5_2,                              ///< AVC (H.264) Level 5.2
    LEVEL_AVC_6,                                ///< AVC (H.264) Level 6
    LEVEL_AVC_6_1,                              ///< AVC (H.264) Level 6.1
    LEVEL_AVC_6_2,                              ///< AVC (H.264) Level 6.2

    // HEVC (H.265) tiers and levels
    LEVEL_HEVC_MAIN_1 = _C2_PL_HEVC_BASE,       ///< HEVC (H.265) Main Tier Level 1
    LEVEL_HEVC_MAIN_2,                          ///< HEVC (H.265) Main Tier Level 2
    LEVEL_HEVC_MAIN_2_1,                        ///< HEVC (H.265) Main Tier Level 2.1
    LEVEL_HEVC_MAIN_3,                          ///< HEVC (H.265) Main Tier Level 3
    LEVEL_HEVC_MAIN_3_1,                        ///< HEVC (H.265) Main Tier Level 3.1
    LEVEL_HEVC_MAIN_4,                          ///< HEVC (H.265) Main Tier Level 4
    LEVEL_HEVC_MAIN_4_1,                        ///< HEVC (H.265) Main Tier Level 4.1
    LEVEL_HEVC_MAIN_5,                          ///< HEVC (H.265) Main Tier Level 5
    LEVEL_HEVC_MAIN_5_1,                        ///< HEVC (H.265) Main Tier Level 5.1
    LEVEL_HEVC_MAIN_5_2,                        ///< HEVC (H.265) Main Tier Level 5.2
    LEVEL_HEVC_MAIN_6,                          ///< HEVC (H.265) Main Tier Level 6
    LEVEL_HEVC_MAIN_6_1,                        ///< HEVC (H.265) Main Tier Level 6.1
    LEVEL_HEVC_MAIN_6_2,                        ///< HEVC (H.265) Main Tier Level 6.2

    LEVEL_HEVC_HIGH_4 = _C2_PL_HEVC_BASE + 0x100,  ///< HEVC (H.265) High Tier Level 4
    LEVEL_HEVC_HIGH_4_1,                        ///< HEVC (H.265) High Tier Level 4.1
    LEVEL_HEVC_HIGH_5,                          ///< HEVC (H.265) High Tier Level 5
    LEVEL_HEVC_HIGH_5_1,                        ///< HEVC (H.265) High Tier Level 5.1
    LEVEL_HEVC_HIGH_5_2,                        ///< HEVC (H.265) High Tier Level 5.2
    LEVEL_HEVC_HIGH_6,                          ///< HEVC (H.265) High Tier Level 6
    LEVEL_HEVC_HIGH_6_1,                        ///< HEVC (H.265) High Tier Level 6.1
    LEVEL_HEVC_HIGH_6_2,                        ///< HEVC (H.265) High Tier Level 6.2

    // VP9 levels
    LEVEL_VP9_1 = _C2_PL_VP9_BASE,              ///< VP9 Level 1
    LEVEL_VP9_1_1,                              ///< VP9 Level 1.1
    LEVEL_VP9_2,                                ///< VP9 Level 2
    LEVEL_VP9_2_1,                              ///< VP9 Level 2.1
    LEVEL_VP9_3,                                ///< VP9 Level 3
    LEVEL_VP9_3_1,                              ///< VP9 Level 3.1
    LEVEL_VP9_4,                                ///< VP9 Level 4
    LEVEL_VP9_4_1,                              ///< VP9 Level 4.1
    LEVEL_VP9_5,                                ///< VP9 Level 5
    LEVEL_VP9_5_1,                              ///< VP9 Level 5.1
    LEVEL_VP9_5_2,                              ///< VP9 Level 5.2
    LEVEL_VP9_6,                                ///< VP9 Level 6
    LEVEL_VP9_6_1,                              ///< VP9 Level 6.1
    LEVEL_VP9_6_2,                              ///< VP9 Level 6.2

    // Dolby Vision levels
    LEVEL_DV_MAIN_HD_24 = _C2_PL_DV_BASE,       ///< Dolby Vision main tier hd24
    LEVEL_DV_MAIN_HD_30,                        ///< Dolby Vision main tier hd30
    LEVEL_DV_MAIN_FHD_24,                       ///< Dolby Vision main tier fhd24
    LEVEL_DV_MAIN_FHD_30,                       ///< Dolby Vision main tier fhd30
    LEVEL_DV_MAIN_FHD_60,                       ///< Dolby Vision main tier fhd60
    LEVEL_DV_MAIN_UHD_24,                       ///< Dolby Vision main tier uhd24
    LEVEL_DV_MAIN_UHD_30,                       ///< Dolby Vision main tier uhd30
    LEVEL_DV_MAIN_UHD_48,                       ///< Dolby Vision main tier uhd48
    LEVEL_DV_MAIN_UHD_60,                       ///< Dolby Vision main tier uhd60
    LEVEL_DV_MAIN_UHD_120,                      ///< Dolby Vision main tier uhd120
    LEVEL_DV_MAIN_8K_30,                        ///< Dolby Vision main tier 8k30
    LEVEL_DV_MAIN_8K_60,                        ///< Dolby Vision main tier 8k60

    LEVEL_DV_HIGH_HD_24 = _C2_PL_DV_BASE + 0x100,  ///< Dolby Vision high tier hd24
    LEVEL_DV_HIGH_HD_30,                        ///< Dolby Vision high tier hd30
    LEVEL_DV_HIGH_FHD_24,                       ///< Dolby Vision high tier fhd24
    LEVEL_DV_HIGH_FHD_30,                       ///< Dolby Vision high tier fhd30
    LEVEL_DV_HIGH_FHD_60,                       ///< Dolby Vision high tier fhd60
    LEVEL_DV_HIGH_UHD_24,                       ///< Dolby Vision high tier uhd24
    LEVEL_DV_HIGH_UHD_30,                       ///< Dolby Vision high tier uhd30
    LEVEL_DV_HIGH_UHD_48,                       ///< Dolby Vision high tier uhd48
    LEVEL_DV_HIGH_UHD_60,                       ///< Dolby Vision high tier uhd60
    LEVEL_DV_HIGH_UHD_120,                      ///< Dolby Vision high tier uhd120
    LEVEL_DV_HIGH_8K_30,                        ///< Dolby Vision high tier 8k30
    LEVEL_DV_HIGH_8K_60,                        ///< Dolby Vision high tier 8k60

    // AV1 levels
    LEVEL_AV1_2    = _C2_PL_AV1_BASE ,          ///< AV1 Level 2
    LEVEL_AV1_2_1,                              ///< AV1 Level 2.1
    LEVEL_AV1_2_2,                              ///< AV1 Level 2.2
    LEVEL_AV1_2_3,                              ///< AV1 Level 2.3
    LEVEL_AV1_3,                                ///< AV1 Level 3
    LEVEL_AV1_3_1,                              ///< AV1 Level 3.1
    LEVEL_AV1_3_2,                              ///< AV1 Level 3.2
    LEVEL_AV1_3_3,                              ///< AV1 Level 3.3
    LEVEL_AV1_4,                                ///< AV1 Level 4
    LEVEL_AV1_4_1,                              ///< AV1 Level 4.1
    LEVEL_AV1_4_2,                              ///< AV1 Level 4.2
    LEVEL_AV1_4_3,                              ///< AV1 Level 4.3
    LEVEL_AV1_5,                                ///< AV1 Level 5
    LEVEL_AV1_5_1,                              ///< AV1 Level 5.1
    LEVEL_AV1_5_2,                              ///< AV1 Level 5.2
    LEVEL_AV1_5_3,                              ///< AV1 Level 5.3
    LEVEL_AV1_6,                                ///< AV1 Level 6
    LEVEL_AV1_6_1,                              ///< AV1 Level 6.1
    LEVEL_AV1_6_2,                              ///< AV1 Level 6.2
    LEVEL_AV1_6_3,                              ///< AV1 Level 6.3
    LEVEL_AV1_7,                                ///< AV1 Level 7
    LEVEL_AV1_7_1,                              ///< AV1 Level 7.1
    LEVEL_AV1_7_2,                              ///< AV1 Level 7.2
    LEVEL_AV1_7_3,                              ///< AV1 Level 7.3

    // MPEG-H 3D Audio levels
    LEVEL_MPEGH_1 = _C2_PL_MPEGH_BASE,          ///< MPEG-H L1
    LEVEL_MPEGH_2,                              ///< MPEG-H L2
    LEVEL_MPEGH_3,                              ///< MPEG-H L3
    LEVEL_MPEGH_4,                              ///< MPEG-H L4
    LEVEL_MPEGH_5,                              ///< MPEG-H L5

    // Advanced Professional VideoCodec(APV) levels/bands
    LEVEL_APV_1_BAND_0 = _C2_PL_APV_BASE,            ///< APV L 1, BAND 0
    LEVEL_APV_1_1_BAND_0,                            ///< APV L 1.1, BAND 0
    LEVEL_APV_2_BAND_0,                              ///< APV L 2, BAND 0
    LEVEL_APV_2_1_BAND_0,                            ///< APV L 2.1, BAND 0
    LEVEL_APV_3_BAND_0,                              ///< APV L 3, BAND 0
    LEVEL_APV_3_1_BAND_0,                            ///< APV L 3.1, BAND 0
    LEVEL_APV_4_BAND_0,                              ///< APV L 4, BAND 0
    LEVEL_APV_4_1_BAND_0,                            ///< APV L 4.1, BAND 0
    LEVEL_APV_5_BAND_0,                              ///< APV L 5, BAND 0
    LEVEL_APV_5_1_BAND_0,                            ///< APV L 5.1, BAND 0
    LEVEL_APV_6_BAND_0,                              ///< APV L 6, BAND 0
    LEVEL_APV_6_1_BAND_0,                            ///< APV L 6.1, BAND 0
    LEVEL_APV_7_BAND_0,                              ///< APV L 7, BAND 0
    LEVEL_APV_7_1_BAND_0,                            ///< APV L 7.1, BAND 0

    LEVEL_APV_1_BAND_1 = _C2_PL_APV_BASE + 0x100,    ///< APV L 1, BAND 1
    LEVEL_APV_1_1_BAND_1,                            ///< APV L 1.1, BAND 1
    LEVEL_APV_2_BAND_1,                              ///< APV L 2, BAND 1
    LEVEL_APV_2_1_BAND_1,                            ///< APV L 2.1, BAND 1
    LEVEL_APV_3_BAND_1,                              ///< APV L 3, BAND 1
    LEVEL_APV_3_1_BAND_1,                            ///< APV L 3.1, BAND 1
    LEVEL_APV_4_BAND_1,                              ///< APV L 4, BAND 1
    LEVEL_APV_4_1_BAND_1,                            ///< APV L 4.1, BAND 1
    LEVEL_APV_5_BAND_1,                              ///< APV L 5, BAND 1
    LEVEL_APV_5_1_BAND_1,                            ///< APV L 5.1, BAND 1
    LEVEL_APV_6_BAND_1,                              ///< APV L 6, BAND 1
    LEVEL_APV_6_1_BAND_1,                            ///< APV L 6.1, BAND 1
    LEVEL_APV_7_BAND_1,                              ///< APV L 7, BAND 1
    LEVEL_APV_7_1_BAND_1,                            ///< APV L 7.1, BAND 1

    LEVEL_APV_1_BAND_2 = _C2_PL_APV_BASE + 0x200,    ///< APV L 1, BAND 2
    LEVEL_APV_1_1_BAND_2,                            ///< APV L 1.1, BAND 2
    LEVEL_APV_2_BAND_2,                              ///< APV L 2, BAND 2
    LEVEL_APV_2_1_BAND_2,                            ///< APV L 2.1, BAND 2
    LEVEL_APV_3_BAND_2,                              ///< APV L 3, BAND 2
    LEVEL_APV_3_1_BAND_2,                            ///< APV L 3.1, BAND 2
    LEVEL_APV_4_BAND_2,                              ///< APV L 4, BAND 2
    LEVEL_APV_4_1_BAND_2,                            ///< APV L 4.1, BAND 2
    LEVEL_APV_5_BAND_2,                              ///< APV L 5, BAND 2
    LEVEL_APV_5_1_BAND_2,                            ///< APV L 5.1, BAND 2
    LEVEL_APV_6_BAND_2,                              ///< APV L 6, BAND 2
    LEVEL_APV_6_1_BAND_2,                            ///< APV L 6.1, BAND 2
    LEVEL_APV_7_BAND_2,                              ///< APV L 7, BAND 2
    LEVEL_APV_7_1_BAND_2,                            ///< APV L 7.1, BAND 2

    LEVEL_APV_1_BAND_3 = _C2_PL_APV_BASE + 0x300,    ///< APV L 1, BAND 3
    LEVEL_APV_1_1_BAND_3,                            ///< APV L 1.1, BAND 3
    LEVEL_APV_2_BAND_3,                              ///< APV L 2, BAND 3
    LEVEL_APV_2_1_BAND_3,                            ///< APV L 2.1, BAND 3
    LEVEL_APV_3_BAND_3,                              ///< APV L 3, BAND 3
    LEVEL_APV_3_1_BAND_3,                            ///< APV L 3.1, BAND 3
    LEVEL_APV_4_BAND_3,                              ///< APV L 4, BAND 3
    LEVEL_APV_4_1_BAND_3,                            ///< APV L 4.1, BAND 3
    LEVEL_APV_5_BAND_3,                              ///< APV L 5, BAND 3
    LEVEL_APV_5_1_BAND_3,                            ///< APV L 5.1, BAND 3
    LEVEL_APV_6_BAND_3,                              ///< APV L 6, BAND 3
    LEVEL_APV_6_1_BAND_3,                            ///< APV L 6.1, BAND 3
    LEVEL_APV_7_BAND_3,                              ///< APV L 7, BAND 3
    LEVEL_APV_7_1_BAND_3,                            ///< APV L 7.1, BAND 3

    // AC-4 levels
    // Below levels are labelled “AC-4 Level zz” where zz is the mdcompat as described in
    // "The MIME codecs parameter", Annex E.13
    // found at https://www.etsi.org/deliver/etsi_ts/103100_103199/10319002/01.02.01_60/ts_10319002v010201p.pdf
    LEVEL_AC4_0 = _C2_PL_AC4_BASE,              ///< AC-4 Level 00
    LEVEL_AC4_1,                                ///< AC-4 Level 01
    LEVEL_AC4_2,                                ///< AC-4 Level 02
    LEVEL_AC4_3,                                ///< AC-4 Level 03
    LEVEL_AC4_4,                                ///< AC-4 Level 04
};

struct C2ProfileLevelStruct {
    C2Config::profile_t profile;  ///< coding profile
    C2Config::level_t   level;    ///< coding level

    C2ProfileLevelStruct(
            C2Config::profile_t profile_ = C2Config::PROFILE_UNUSED,
            C2Config::level_t level_ = C2Config::LEVEL_UNUSED)
        : profile(profile_), level(level_) { }

    DEFINE_AND_DESCRIBE_C2STRUCT(ProfileLevel)
    C2FIELD(profile, "profile")
    C2FIELD(level,   "level")
};

// TODO: may need to make this explicit (have .set member)
typedef C2StreamParam<C2Info, C2ProfileLevelStruct, kParamIndexProfileLevel>
        C2StreamProfileLevelInfo;
constexpr char C2_PARAMKEY_PROFILE_LEVEL[] = "coded.pl";

/**
 * Codec-specific initialization data.
 *
 * This is initialization data for the codec.
 *
 * For AVC/HEVC, these are the concatenated SPS/PPS/VPS NALs.
 *
 * TODO: define for other codecs.
 */
typedef C2StreamParam<C2Info, C2BlobValue, kParamIndexInitData> C2StreamInitDataInfo;
constexpr char C2_PARAMKEY_INIT_DATA[] = "coded.init-data";

/**
 * Supplemental Data.
 *
 * This is coding-specific supplemental informational data, e.g. SEI for AVC/HEVC.
 * This structure is not a configuration so it does not have a parameter key.
 * This structure shall be returned in the configuration update, and can be repeated as needed
 * in the same update.
 */
C2ENUM(C2Config::supplemental_info_t, uint32_t,
    INFO_NONE = 0,

    INFO_PREFIX_SEI_UNIT = 0x10000, ///< prefix SEI payload types add this flag
    INFO_SUFFIX_SEI_UNIT = 0x20000, ///< suffix SEI payload types add this flag

    INFO_SEI_USER_DATA = INFO_PREFIX_SEI_UNIT | 4,    ///< closed-captioning data (ITU-T T35)
    INFO_SEI_MDCV      = INFO_PREFIX_SEI_UNIT | 137,  ///< mastering display color volume
    INFO_SET_USER_DATA_SFX = INFO_SUFFIX_SEI_UNIT | 4, ///< closed-captioning data (ITU-T T35)

    INFO_VENDOR_START = 0x70000000
)

struct C2SupplementalDataStruct {
    C2SupplementalDataStruct()
        : type_(INFO_NONE) { }

    C2SupplementalDataStruct(
            size_t flexCount, C2Config::supplemental_info_t type, std::vector<uint8_t> data_)
        : type_(type) {
            memcpy(data, &data_[0], c2_min(data_.size(), flexCount));
    }

    C2Config::supplemental_info_t type_;
    uint8_t data[];

    DEFINE_AND_DESCRIBE_FLEX_C2STRUCT(SupplementalData, data)
    C2FIELD(type_, "type")
    C2FIELD(data, "data")
};
typedef C2StreamParam<C2Info, C2SupplementalDataStruct, kParamIndexSupplementalData>
        C2StreamSupplementalDataInfo;

/**
 * Supplemental Data Subscription
 */
typedef C2StreamParam<C2Tuning, C2SimpleArrayStruct<C2Config::supplemental_info_t>,
                kParamIndexSubscribedSupplementalData>
        C2StreamSubscribedSupplementalDataTuning;
constexpr char C2_PARAMKEY_SUBSCRIBED_SUPPLEMENTAL_DATA[] = "output.subscribed-supplemental";

/* ---------------------------------- pipeline characteristics ---------------------------------- */

/**
 * Media-type.
 *
 * This is defined for both port and stream, but stream media type may be a subtype of the
 * port media type.
 */
typedef C2PortParam<C2Setting, C2StringValue, kParamIndexMediaType> C2PortMediaTypeSetting;
constexpr char C2_PARAMKEY_INPUT_MEDIA_TYPE[] = "input.media-type";
constexpr char C2_PARAMKEY_OUTPUT_MEDIA_TYPE[] = "output.media-type";

typedef C2StreamParam<C2Setting, C2StringValue, kParamIndexMediaType> C2StreamMediaTypeSetting;

/**
 * Pipeline delays.
 *
 * Input delay is the number of additional input frames requested by the component to process
 * an input frame.
 *
 * Output delay is the number of additional output frames that need to be generated before an
 * output can be released by the component.
 *
 * Pipeline delay is the number of additional frames that are processed at one time by the
 * component.
 *
 * As these may vary from frame to frame, the number is the maximum required value. E.g. if
 * input delay is 0, the component is expected to consume each frame queued even if no further
 * frames are queued. Similarly, if input delay is 1, as long as there are always exactly 2
 * outstanding input frames queued to the component, it shall produce output.
 */

typedef C2PortParam<C2Tuning, C2Uint32Value, kParamIndexDelay | C2Param::CoreIndex::IS_REQUEST_FLAG>
        C2PortRequestedDelayTuning;
constexpr char C2_PARAMKEY_INPUT_DELAY_REQUEST[] = "input.delay"; // deprecated
constexpr char C2_PARAMKEY_OUTPUT_DELAY_REQUEST[] = "output.delay"; // deprecated

typedef C2GlobalParam<C2Tuning, C2Uint32Value,
                kParamIndexDelay | C2Param::CoreIndex::IS_REQUEST_FLAG>
        C2RequestedPipelineDelayTuning;
constexpr char C2_PARAMKEY_PIPELINE_DELAY_REQUEST[] = "algo.delay"; // deprecated

// read-only
typedef C2PortParam<C2Tuning, C2Uint32Value, kParamIndexDelay> C2PortDelayTuning;
typedef C2PortDelayTuning C2PortActualDelayTuning; // deprecated
constexpr char C2_PARAMKEY_INPUT_DELAY[] = "input.delay";
constexpr char C2_PARAMKEY_OUTPUT_DELAY[] = "output.delay";

// read-only
typedef C2GlobalParam<C2Tuning, C2Uint32Value, kParamIndexDelay> C2PipelineDelayTuning;
typedef C2PipelineDelayTuning C2ActualPipelineDelayTuning; // deprecated
constexpr char C2_PARAMKEY_PIPELINE_DELAY[] = "algo.delay";

/**
 * Enable/disable low latency mode.
 * If true, low latency is preferred over low power. Disable power optimizations that
 * may result in increased latency. For decoders, this means that the decoder does not
 * hold input and output data more than required by the codec standards.
 */
typedef C2GlobalParam<C2Tuning, C2EasyBoolValue, kParamIndexLowLatencyMode>
        C2GlobalLowLatencyModeTuning;
constexpr char C2_PARAMKEY_LOW_LATENCY_MODE[] = "algo.low-latency";

/**
 * Reference characteristics.
 *
 * The component may hold onto input and output buffers even after completing the corresponding
 * work item.
 *
 * Max reference age is the longest number of additional frame processing that a component may
 * hold onto a buffer for. Max reference count is the number of buffers that a component may
 * hold onto at the same time at the worst case. These numbers assume single frame per buffers.
 *
 * Use max-uint32 if there is no limit for the max age or count.
 */
typedef C2StreamParam<C2Tuning, C2Uint32Value, kParamIndexMaxReferenceAge>
        C2StreamMaxReferenceAgeTuning;
constexpr char C2_PARAMKEY_INPUT_MAX_REFERENCE_AGE[] = "input.reference.max-age";
constexpr char C2_PARAMKEY_OUTPUT_MAX_REFERENCE_AGE[] = "output.reference.max-age";

typedef C2StreamParam<C2Tuning, C2Uint32Value, kParamIndexMaxReferenceCount>
        C2StreamMaxReferenceCountTuning;
constexpr char C2_PARAMKEY_INPUT_MAX_REFERENCE_COUNT[] = "input.reference.max-count";
constexpr char C2_PARAMKEY_OUTPUT_MAX_REFERENCE_COUNT[] = "output.reference.max-count";

/**
 * Output reordering.
 *
 * The size of the window to use for output buffer reordering. 0 is interpreted as 1.
 */
// output only
typedef C2PortParam<C2Tuning, C2Uint32Value, kParamIndexReorderBufferDepth>
        C2PortReorderBufferDepthTuning;
constexpr char C2_PARAMKEY_OUTPUT_REORDER_DEPTH[] = "output.reorder.depth";

C2ENUM(C2Config::ordinal_key_t, uint32_t,
        ORDINAL,
        TIMESTAMP,
        CUSTOM)

// read-only, output only
typedef C2PortParam<C2Setting, C2SimpleValueStruct<C2Config::ordinal_key_t>, kParamIndexReorderKey>
        C2PortReorderKeySetting;
constexpr char C2_PARAMKEY_OUTPUT_REORDER_KEY[] = "output.reorder.key";

/**
 * Stream count.
 */
// private
typedef C2PortParam<C2Tuning, C2Uint32Value, kParamIndexStreamCount> C2PortStreamCountTuning;
constexpr char C2_PARAMKEY_INPUT_STREAM_COUNT[] = "input.stream-count";
constexpr char C2_PARAMKEY_OUTPUT_STREAM_COUNT[] = "output.stream-count";

/**
 * Config update subscription.
 */
// private
typedef C2GlobalParam<C2Tuning, C2Uint32Array, kParamIndexSubscribedParamIndices>
        C2SubscribedParamIndicesTuning;
constexpr char C2_PARAMKEY_SUBSCRIBED_PARAM_INDICES[] = "output.subscribed-indices";

/**
 * Suggested buffer (C2Frame) count. This is a suggestion by the component for the number of
 * input and output frames allocated for the component's use in the buffer pools.
 *
 * Component shall set the acceptable range of buffers allocated for it. E.g. client shall
 * allocate at least the minimum required value.
 */
// read-only
typedef C2PortParam<C2Tuning, C2Uint64Array, kParamIndexSuggestedBufferCount>
        C2PortSuggestedBufferCountTuning;
constexpr char C2_PARAMKEY_INPUT_SUGGESTED_BUFFER_COUNT[] = "input.buffers.pool-size";
constexpr char C2_PARAMKEY_OUTPUT_SUGGESTED_BUFFER_COUNT[] = "output.buffers.pool-size";

/**
 * Input/output batching.
 *
 * For input, component requests that client batches work in batches of specified size. For output,
 * client requests that the component batches work completion in given batch size.
 * Value 0 means don't care.
 */
typedef C2PortParam<C2Tuning, C2Uint64Array, kParamIndexBatchSize> C2PortBatchSizeTuning;
constexpr char C2_PARAMKEY_INPUT_BATCH_SIZE[] = "input.buffers.batch-size";
constexpr char C2_PARAMKEY_OUTPUT_BATCH_SIZE[] = "output.buffers.batch-size";

/**
 * Current & last work ordinals.
 *
 * input port: last work queued to component.
 * output port: last work completed by component.
 * global: current work.
 */
typedef C2PortParam<C2Tuning, C2WorkOrdinalStruct, kParamIndexLastWorkQueued> C2LastWorkQueuedTuning;
typedef C2GlobalParam<C2Tuning, C2WorkOrdinalStruct, kParamIndexCurrentWork> C2CurrentWorkTuning;


/* ------------------------------------- memory allocation ------------------------------------- */

/**
 * Allocators to use.
 *
 * These are requested by the component.
 *
 * If none specified, client will use the default allocator ID based on the component domain and
 * kind.
 */
typedef C2PortParam<C2Tuning, C2SimpleArrayStruct<C2Allocator::id_t>, kParamIndexAllocators>
        C2PortAllocatorsTuning;
constexpr char C2_PARAMKEY_INPUT_ALLOCATORS[] = "input.buffers.allocator-ids";
constexpr char C2_PARAMKEY_OUTPUT_ALLOCATORS[] = "output.buffers.allocator-ids";

typedef C2GlobalParam<C2Tuning, C2SimpleArrayStruct<C2Allocator::id_t>, kParamIndexAllocators>
        C2PrivateAllocatorsTuning;
constexpr char C2_PARAMKEY_PRIVATE_ALLOCATORS[] = "algo.buffers.allocator-ids";

/**
 * Allocator to use for outputting to surface.
 *
 * Components can optionally request allocator type for outputting to surface.
 *
 * If none specified, client will use the default BufferQueue-backed allocator ID for outputting to
 * surface.
 */
typedef C2PortParam<C2Tuning, C2Uint32Value, kParamIndexSurfaceAllocator>
        C2PortSurfaceAllocatorTuning;
constexpr char C2_PARAMKEY_OUTPUT_SURFACE_ALLOCATOR[] = "output.buffers.surface-allocator-id";

/**
 * Block pools to use.
 *
 * These are allocated by the client for the component using the allocator IDs specified by the
 * component. This is not used for the input port.
 */
typedef C2PortParam<C2Tuning, C2SimpleArrayStruct<C2BlockPool::local_id_t>, kParamIndexBlockPools>
        C2PortBlockPoolsTuning;
constexpr char C2_PARAMKEY_OUTPUT_BLOCK_POOLS[] = "output.buffers.pool-ids";

typedef C2GlobalParam<C2Tuning, C2SimpleArrayStruct<C2BlockPool::local_id_t>, kParamIndexBlockPools>
        C2PrivateBlockPoolsTuning;
constexpr char C2_PARAMKEY_PRIVATE_BLOCK_POOLS[] = "algo.buffers.pool-ids";

/**
 * The max number of private allocations at any one time by the component.
 * (This is an array with a corresponding value for each private allocator)
 */
typedef C2GlobalParam<C2Tuning, C2Uint32Array, kParamIndexMaxReferenceCount>
        C2MaxPrivateBufferCountTuning;
constexpr char C2_PARAMKEY_MAX_PRIVATE_BUFFER_COUNT[] = "algo.buffers.max-count";

/**
 * Buffer type
 *
 * This is provided by the component for the client to allocate the proper buffer type for the
 * input port, and can be provided by the client to control the buffer type for the output.
 */
// private
typedef C2StreamParam<C2Setting, C2SimpleValueStruct<C2EasyEnum<C2BufferData::type_t>>,
                kParamIndexBufferType>
        C2StreamBufferTypeSetting;
constexpr char C2_PARAMKEY_INPUT_STREAM_BUFFER_TYPE[] = "input.buffers.type";
constexpr char C2_PARAMKEY_OUTPUT_STREAM_BUFFER_TYPE[] = "output.buffers.type";

/**
 * Memory usage.
 *
 * Suggested by component for input and negotiated between client and component for output.
 */
typedef C2StreamParam<C2Tuning, C2Uint64Value, kParamIndexUsage> C2StreamUsageTuning;
constexpr char C2_PARAMKEY_INPUT_STREAM_USAGE[] = "input.buffers.usage";
constexpr char C2_PARAMKEY_OUTPUT_STREAM_USAGE[] = "output.buffers.usage";

/**
 * Picture (video or image frame) size.
 */
struct C2PictureSizeStruct {
    inline C2PictureSizeStruct()
        : width(0), height(0) { }

    inline C2PictureSizeStruct(uint32_t width_, uint32_t height_)
        : width(width_), height(height_) { }

    uint32_t width;     ///< video width
    uint32_t height;    ///< video height

    DEFINE_AND_DESCRIBE_C2STRUCT(PictureSize)
    C2FIELD(width, "width")
    C2FIELD(height, "height")
};

/**
 * Out of memory signaling
 *
 * This is a configuration for the client to mark that it cannot allocate necessary private and/
 * or output buffers to continue operation, and to signal the failing configuration.
 */
struct C2OutOfMemoryStruct {
    C2BlockPool::local_id_t pool;   ///< pool ID that failed the allocation
    uint64_t usage;                 ///< memory usage used
    C2PictureSizeStruct planar;     ///< buffer dimensions to be allocated if 2D
    uint32_t format;                ///< pixel format to be used if 2D
    uint32_t capacity;              ///< buffer capacity to be allocated if 1D
    c2_bool_t outOfMemory;           ///< true if component is out of memory

    DEFINE_AND_DESCRIBE_C2STRUCT(OutOfMemory)
    C2FIELD(pool, "pool")
    C2FIELD(usage, "usage")
    C2FIELD(planar, "planar")
    C2FIELD(format, "format")
    C2FIELD(capacity, "capacity")
    C2FIELD(outOfMemory, "out-of-memory")
};

typedef C2GlobalParam<C2Tuning, C2OutOfMemoryStruct, kParamIndexOutOfMemory> C2OutOfMemoryTuning;
constexpr char C2_PARAMKEY_OUT_OF_MEMORY[] = "algo.oom";

/**
 * Max buffer size
 *
 * This is a hint provided by the component for the maximum buffer size expected on a stream for the
 * current configuration on its input and output streams. This is communicated to clients so they
 * can preallocate input buffers, or configure downstream components that require a maximum size on
 * their buffers.
 *
 * Read-only. Required to be provided by components on all compressed streams.
 */
typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexMaxBufferSize> C2StreamMaxBufferSizeInfo;
constexpr char C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE[] = "input.buffers.max-size";
constexpr char C2_PARAMKEY_OUTPUT_MAX_BUFFER_SIZE[] = "output.buffers.max-size";

/**
 * Large frame struct
 *
 * This structure describes the size limits for large frames (frames with multiple
 * access units.)
 */
struct C2LargeFrameStruct {
    uint32_t maxSize;         ///< maximum size of the buffer in bytes
    uint32_t thresholdSize;   ///< size threshold for the buffer in bytes. The buffer is considered
                              ///< full as soon as its size reaches or surpasses this limit.
    C2LargeFrameStruct()
        : maxSize(0),
          thresholdSize(0) {}

    C2LargeFrameStruct(uint32_t maxSize_, uint32_t thresholdSize_)
        : maxSize(maxSize_), thresholdSize(thresholdSize_) {}

    DEFINE_AND_DESCRIBE_C2STRUCT(LargeFrame)
    C2FIELD(maxSize, "max-size")
    C2FIELD(thresholdSize, "threshold-size")
};

/**
 * This tuning controls the size limits for large output frames for the component.
 * The default value for this tuning is platform specific.
 */
typedef C2StreamParam<C2Tuning, C2LargeFrameStruct, kParamIndexLargeFrame>
        C2LargeFrame;
constexpr char C2_PARAMKEY_OUTPUT_LARGE_FRAME[] = "output.large-frame";

/* ---------------------------------------- misc. state ---------------------------------------- */

/**
 * Tripped state,
 *
 * This state exists to be able to provide reasoning for a tripped state during normal
 * interface operations, as well as to allow client to trip the component on demand.
 */
typedef C2GlobalParam<C2Tuning, C2BoolValue, kParamIndexTripped>
        C2TrippedTuning;
constexpr char C2_PARAMKEY_TRIPPED[] = "algo.tripped";

/**
 * Configuration counters.
 *
 * Configurations are tracked using three counters. The input counter is incremented exactly
 * once with each work accepted by the component. The output counter is incremented exactly
 * once with each work completed by the component (in the order of work completion). The
 * global counter is incremented exactly once during to each config() call. These counters
 * shall be read-only.
 *
 * TODO: these should be counters.
 */
typedef C2PortParam<C2Tuning, C2Uint64Value, kParamIndexConfigCounter> C2PortConfigCounterTuning;
typedef C2GlobalParam<C2Tuning, C2Uint64Value, kParamIndexConfigCounter> C2ConfigCounterTuning;
constexpr char C2_PARAMKEY_INPUT_COUNTER[] = "input.buffers.counter";
constexpr char C2_PARAMKEY_OUTPUT_COUNTER[] = "output.buffers.counter";
constexpr char C2_PARAMKEY_CONFIG_COUNTER[] = "algo.config.counter";

/* ----------------------------------------- resources ----------------------------------------- */

/**
 * Resource kind.
 */
C2ENUM(C2Config::resource_kind_t, uint32_t,
    CONST,
    PER_FRAME,
    PER_INPUT_BLOCK,
    PER_OUTPUT_BLOCK
)

/**
 * Definition of a system resource use.
 *
 * [PROPOSED]
 *
 * System resources are defined by the default component store.
 * They represent any physical or abstract entities of limited availability
 * that is required for a component instance to execute and process work.
 *
 * Each defined resource has an id. In general, the id is defined by the vendor,
 * but the platform also defines a limited set of IDs. Vendor IDs SHALL start
 * from C2PlatformConfig::resource_id_t::VENDOR_START.
 *
 * The use of a resource is specified by the amount and the kind (e.g. whether the amount
 * of resources is required for each frame processed, or whether they are required
 * regardless of the processing rate (const amount)).
 *
 * Note: implementations can shadow this structure with their own custom resource
 * structure where a uint32_t based enum is used for id.
 * This can be used to provide a name for each resource, via parameter descriptors.
 */

struct C2SystemResourceStruct {
    C2SystemResourceStruct(uint32_t id_,
                           C2Config::resource_kind_t kind_,
                           uint64_t amount_)
        : id(id_), kind(kind_), amount(amount_) { }
    C2SystemResourceStruct() : C2SystemResourceStruct(0, CONST, 0) {}
    uint32_t id;            ///< resource ID (see C2PlatformConfig::resource_id_t)
    C2Config::resource_kind_t kind;
    uint64_t amount;

    DEFINE_AND_DESCRIBE_C2STRUCT(SystemResource)
    C2FIELD(id, "id")
    C2FIELD(kind, "kind")
    C2FIELD(amount, "amount")
};

/**
 * Total system resource capacity.
 *
 * [PROPOSED]
 *
 * This setting is implemented by the default component store.
 * The total resource capacity is specified as the maximum amount for each resource ID
 * that is supported by the device hardware or firmware.
 * As such, the kind must be CONST for each element.
 */
typedef C2GlobalParam<C2Tuning,
                      C2SimpleArrayStruct<C2SystemResourceStruct>,
                      kParamIndexResourcesCapacity> C2ResourcesCapacityTuning;
constexpr char C2_PARAMKEY_RESOURCES_CAPACITY[] = "resources.capacity";

/**
 * Excluded system resources.
 *
 * [PROPOSED]
 *
 * This setting is implemented by the default component store.
 * Some system resources may be used by components and not tracked by the Codec 2.0 API.
 * This is communicated by this tuning.
 * Excluded resources are the total resources that are used by non-Codec 2.0 components.
 * It is specified as the excluded amount for each resource ID that is used by
 * a non-Codec 2.0 component. As such, the kind must be CONST for each element.
 *
 * The platform can calculate the available resources as total capacity minus
 * excluded resource minus sum of needed resources for each component.
 */
typedef C2GlobalParam<C2Tuning,
                      C2SimpleArrayStruct<C2SystemResourceStruct>,
                      kParamIndexResourcesExcluded> C2ResourcesExcludedTuning;
constexpr char C2_PARAMKEY_RESOURCES_EXCLUDED[] = "resources.excluded";

/**
 * System resources needed for the current configuration.
 *
 * [PROPOSED]
 *
 * Resources are tracked as a list of individual resource use specifications.
 * The resource kind can be CONST, PER_FRAME, PER_INPUT_BLODCK or PER_OUTPUT_BLOCK.
 */
typedef C2GlobalParam<C2Tuning,
                      C2SimpleArrayStruct<C2SystemResourceStruct>,
                      kParamIndexResourcesNeeded> C2ResourcesNeededTuning;
constexpr char C2_PARAMKEY_RESOURCES_NEEDED[] = "resources.needed";

/**
 * System resources reserved for this component
 *
 * [FUTURE]
 *
 * This allows the platform to set aside system resources for the component.
 * Since this is a static resource reservation, kind must be CONST for each element.
 * This resource reservation only considers CONST and PER_FRAME use.
 *
 * By default, no resources are reserved for a component.
 * If resource reservation is successful, the component shall be able to use those
 * resources exclusively. If however, the component is not using all of the
 * reserved resources, those may be shared with other components.
 */
typedef C2GlobalParam<C2Tuning,
                      C2SimpleArrayStruct<C2SystemResourceStruct>,
                      kParamIndexResourcesReserved> C2ResourcesReservedTuning;
constexpr char C2_PARAMKEY_RESOURCES_RESERVED[] = "resources.reserved";

/**
 * Operating rate.
 *
 * Operating rate is the expected rate of work through the component. Negative values is
 * invalid.
 *
 * TODO: this could distinguish set value
 */
typedef C2GlobalParam<C2Tuning, C2FloatValue, kParamIndexOperatingRate> C2OperatingRateTuning;
constexpr char C2_PARAMKEY_OPERATING_RATE[] = "algo.rate";

/**
 * Realtime / operating point.
 *
 * Priority value defines the operating point for the component. Operating points are defined by
 * the vendor. Priority value of 0 means that the client requires operation at the given operating
 * rate. Priority values -1 and below define operating points in decreasing performance. In this
 * case client expects best effort without exceeding the specific operating point. This allows
 * client to run components deeper in the background by using larger priority values. In these
 * cases operating rate is a hint for the maximum rate that the client anticipates.
 *
 * Operating rate and priority are used in tandem. E.g. if there are components that run at a
 * higher operating point (priority) it will make more resources available for components at
 * a lower operating point, so operating rate can be used to gate those components.
 *
 * Positive priority values are not defined at the moment and shall be treated equivalent to 0.
 */
typedef C2GlobalParam<C2Tuning, C2Int32Value, kParamIndexRealTimePriority>
        C2RealTimePriorityTuning;
constexpr char C2_PARAMKEY_PRIORITY[] = "algo.priority";

/* ------------------------------------- protected content ------------------------------------- */

/**
 * Secure mode.
 */
C2ENUM(C2Config::secure_mode_t, uint32_t,
    SM_UNPROTECTED,    ///< no content protection
    SM_READ_PROTECTED, ///< input and output buffers shall be protected from reading
    /// both read protected and readable encrypted buffers are used
    SM_READ_PROTECTED_WITH_ENCRYPTED,
)

typedef C2GlobalParam<C2Tuning, C2SimpleValueStruct<C2Config::secure_mode_t>, kParamIndexSecureMode>
        C2SecureModeTuning;
constexpr char C2_PARAMKEY_SECURE_MODE[] = "algo.secure-mode";

/* ===================================== ENCODER COMPONENTS ===================================== */

/**
 * Bitrate
 */
typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexBitrate> C2StreamBitrateInfo;
constexpr char C2_PARAMKEY_BITRATE[] = "coded.bitrate";

/**
 * Bitrate mode.
 *
 * TODO: refine this with bitrate ranges and suggested window
 */
C2ENUM(C2Config::bitrate_mode_t, uint32_t,
    BITRATE_CONST_SKIP_ALLOWED = 0,      ///< constant bitrate, frame skipping allowed
    BITRATE_CONST = 1,                   ///< constant bitrate, keep all frames
    BITRATE_VARIABLE_SKIP_ALLOWED = 2,   ///< bitrate can vary, frame skipping allowed
    BITRATE_VARIABLE = 3,                ///< bitrate can vary, keep all frames
    BITRATE_IGNORE = 7,                  ///< bitrate can be exceeded at will to achieve
                                         ///< quality or other settings

    // bitrate modes are composed of the following flags
    BITRATE_FLAG_KEEP_ALL_FRAMES = 1,
    BITRATE_FLAG_CAN_VARY = 2,
    BITRATE_FLAG_CAN_EXCEED = 4,
)

typedef C2StreamParam<C2Tuning, C2SimpleValueStruct<C2Config::bitrate_mode_t>,
                kParamIndexBitrateMode>
        C2StreamBitrateModeTuning;
constexpr char C2_PARAMKEY_BITRATE_MODE[] = "algo.bitrate-mode";

/**
 * Quality.
 *
 * This is defined by each component, the higher the better the output quality at the expense of
 * less compression efficiency. This setting is defined for the output streams in case the
 * component can support varying quality on each stream, or as an output port tuning in case the
 * quality is global to all streams.
 */
typedef C2StreamParam<C2Tuning, C2Uint32Value, kParamIndexQuality> C2StreamQualityTuning;
typedef C2PortParam<C2Tuning, C2Uint32Value, kParamIndexQuality> C2QualityTuning;
constexpr char C2_PARAMKEY_QUALITY[] = "algo.quality";

/**
 * Complexity.
 *
 * This is defined by each component, this higher the value, the more resources the component
 * will use to produce better quality at the same compression efficiency or better compression
 * efficiency at the same quality. This setting is defined for the output streams in case the
 * component can support varying complexity on each stream, or as an output port tuning in case the
 * quality is global to all streams
 */
typedef C2StreamParam<C2Tuning, C2Uint32Value, kParamIndexComplexity> C2StreamComplexityTuning;
typedef C2PortParam<C2Tuning, C2Uint32Value, kParamIndexComplexity> C2ComplexityTuning;
constexpr char C2_PARAMKEY_COMPLEXITY[] = "algo.complexity";

/**
 * Header (init-data) handling around sync frames.
 */
C2ENUM(C2Config::prepend_header_mode_t, uint32_t,
    /**
     * don't prepend header. Signal header only through C2StreamInitDataInfo.
     */
    PREPEND_HEADER_TO_NONE,

    /**
     * prepend header before the first output frame and thereafter before the next sync frame
     * if it changes.
     */
    PREPEND_HEADER_ON_CHANGE,

    /**
     * prepend header before every sync frame.
     */
    PREPEND_HEADER_TO_ALL_SYNC,
)

typedef C2GlobalParam<C2Setting, C2SimpleValueStruct<C2Config::prepend_header_mode_t>,
                kParamIndexPrependHeaderMode>
        C2PrependHeaderModeSetting;
constexpr char C2_PARAMKEY_PREPEND_HEADER_MODE[] = "output.buffers.prepend-header";

/* =================================== IMAGE/VIDEO COMPONENTS =================================== */

/*
 * Order of transformation is:
 *
 * crop => (scaling => scaled-crop) => sample-aspect-ratio => flip => rotation
 */

/**
 * Picture (image- and video frame) size.
 *
 * This is used for the output of the video decoder, and the input of the video encoder.
 */
typedef C2StreamParam<C2Info, C2PictureSizeStruct, kParamIndexPictureSize> C2StreamPictureSizeInfo;
constexpr char C2_PARAMKEY_PICTURE_SIZE[] = "raw.size";

/**
 * Crop rectangle.
 */
struct C2RectStruct : C2Rect {
    C2RectStruct() = default;
    C2RectStruct(const C2Rect &rect) : C2Rect(rect) { }

    bool operator==(const C2RectStruct &) = delete;
    bool operator!=(const C2RectStruct &) = delete;

    DEFINE_AND_DESCRIBE_BASE_C2STRUCT(Rect)
    C2FIELD(width, "width")
    C2FIELD(height, "height")
    C2FIELD(left, "left")
    C2FIELD(top, "top")
};

typedef C2StreamParam<C2Info, C2RectStruct, kParamIndexCropRect> C2StreamCropRectInfo;
constexpr char C2_PARAMKEY_CROP_RECT[] = "raw.crop";
constexpr char C2_PARAMKEY_CODED_CROP_RECT[] = "coded.crop";

/**
 * Pixel format.
 */
// TODO: define some

typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexPixelFormat> C2StreamPixelFormatInfo;
constexpr char C2_PARAMKEY_PIXEL_FORMAT[] = "raw.pixel-format";

/**
 * Extended rotation information also incorporating a flip.
 *
 * Rotation is counter clock-wise.
 */
struct C2RotationStruct {
    C2RotationStruct(int32_t rotation = 0)
        : flip(0), value(rotation) { }

    int32_t flip;   ///< horizontal flip (left-right flip applied prior to rotation)
    int32_t value;  ///< rotation in degrees counter clockwise

    DEFINE_AND_DESCRIBE_BASE_C2STRUCT(Rotation)
    C2FIELD(flip, "flip")
    C2FIELD(value, "value")
};

typedef C2StreamParam<C2Info, C2RotationStruct, kParamIndexRotation> C2StreamRotationInfo;
constexpr char C2_PARAMKEY_ROTATION[] = "raw.rotation";
constexpr char C2_PARAMKEY_VUI_ROTATION[] = "coded.vui.rotation";

/**
 * Region of Interest of an image/video frame communicated as an array of C2QpOffsetRectStruct
 *
 * Fields width, height, left and top of C2QpOffsetRectStruct form a bounding box contouring RoI.
 * Field qpOffset of C2QpOffsetRectStruct indicates the qp bias to be used for quantizing the
 * coding units of the bounding box.
 *
 * If Roi rect is not valid that is bounding box width is < 0 or bounding box height is < 0,
 * components may ignore the configuration silently. If Roi rect extends outside frame
 * boundaries, then rect shall be clamped to the frame boundaries.
 *
 * The scope of this key is throughout the encoding session until it is reconfigured with a
 * different value.
 *
 * The number of elements in C2StreamQpOffset array is not limited by C2 specification.
 * However components may mandate a limit. Implementations may drop the rectangles that are beyond
 * the supported limits. Hence it is preferable to place the rects in descending order of
 * importance. Transitively, if the bounding boxes overlap, then the most preferred
 * rectangle's qp offset (earlier rectangle qp offset) will be used to quantize the block.
 */
struct C2QpOffsetRectStruct : C2Rect {
    C2QpOffsetRectStruct() = default;
    C2QpOffsetRectStruct(const C2Rect &rect, int32_t offset) : C2Rect(rect), qpOffset(offset) {}

    bool operator==(const C2QpOffsetRectStruct &) = delete;
    bool operator!=(const C2QpOffsetRectStruct &) = delete;

    int32_t qpOffset;

    DEFINE_AND_DESCRIBE_C2STRUCT(QpOffsetRect)
    C2FIELD(width, "width")
    C2FIELD(height, "height")
    C2FIELD(left, "left")
    C2FIELD(top, "top")
    C2FIELD(qpOffset, "qp-offset")
};

typedef C2StreamParam<C2Info, C2SimpleArrayStruct<C2QpOffsetRectStruct>, kParamIndexQpOffsetRects>
        C2StreamQpOffsetRects;
constexpr char C2_PARAMKEY_QP_OFFSET_RECTS[] = "coding.qp-offset-rects";

/**
 * Pixel (sample) aspect ratio.
 */
typedef C2StreamParam<C2Info, C2PictureSizeStruct, kParamIndexPixelAspectRatio>
        C2StreamPixelAspectRatioInfo;
constexpr char C2_PARAMKEY_PIXEL_ASPECT_RATIO[] = "raw.sar";
constexpr char C2_PARAMKEY_VUI_PIXEL_ASPECT_RATIO[] = "coded.vui.sar";

/**
 * In-line scaling.
 *
 * Components can optionally support scaling of raw image/video frames.  Or scaling only a
 * portion of raw image/video frames (scaled-crop).
 */

C2ENUM(C2Config::scaling_method_t, uint32_t,
    SCALING_ARBITRARY,   ///< arbitrary, unspecified
)

typedef C2StreamParam<C2Tuning, C2SimpleValueStruct<C2Config::scaling_method_t>,
                kParamIndexScalingMethod>
        C2StreamScalingMethodTuning;
constexpr char C2_PARAMKEY_SCALING_MODE[] = "raw.scaling-method";

typedef C2StreamParam<C2Tuning, C2PictureSizeStruct, kParamIndexScaledPictureSize>
        C2StreamScaledPictureSizeTuning;
constexpr char C2_PARAMKEY_SCALED_PICTURE_SIZE[] = "raw.scaled-size";

typedef C2StreamParam<C2Tuning, C2RectStruct, kParamIndexScaledCropRect>
        C2StreamScaledCropRectTuning;
constexpr char C2_PARAMKEY_SCALED_CROP_RECT[] = "raw.scaled-crop";

/* ------------------------------------- color information ------------------------------------- */

/**
 * Color Info
 *
 * Chroma location can vary for top and bottom fields, so use an array, that can have 0 to 2
 * values. Empty array is used for non YUV formats.
 */

struct C2Color {
    enum matrix_t : uint32_t;  ///< matrix coefficient (YUV <=> RGB)
    enum plane_layout_t : uint32_t;  ///< plane layout for flexible formats
    enum primaries_t : uint32_t;  ///< color primaries and white point
    enum range_t : uint32_t;  ///< range of color component values
    enum subsampling_t : uint32_t;  ///< chroma subsampling
    enum transfer_t : uint32_t;  ///< transfer function
};

/// Chroma subsampling
C2ENUM(C2Color::subsampling_t, uint32_t,
    MONOCHROME,     ///< there are no Cr nor Cb planes
    MONOCHROME_ALPHA, ///< there are no Cr nor Cb planes, but there is an alpha plane
    RGB,            ///< RGB
    RGBA,           ///< RGBA
    YUV_420,        ///< Cr and Cb planes are subsampled by 2 both horizontally and vertically
    YUV_422,        ///< Cr and Cb planes are subsampled horizontally
    YUV_444,        ///< Cr and Cb planes are not subsampled
    YUVA_444,       ///< Cr and Cb planes are not subsampled, there is an alpha plane
)

struct C2ChromaOffsetStruct {
    // chroma offsets defined by ITU
    constexpr static C2ChromaOffsetStruct ITU_YUV_444() { return { 0.0f, 0.0f }; }
    constexpr static C2ChromaOffsetStruct ITU_YUV_422() { return { 0.0f, 0.0f }; }
    constexpr static C2ChromaOffsetStruct ITU_YUV_420_0() { return { 0.0f, 0.5f }; }
    constexpr static C2ChromaOffsetStruct ITU_YUV_420_1() { return { 0.5f, 0.5f }; }
    constexpr static C2ChromaOffsetStruct ITU_YUV_420_2() { return { 0.0f, 0.0f }; }
    constexpr static C2ChromaOffsetStruct ITU_YUV_420_3() { return { 0.5f, 0.0f }; }
    constexpr static C2ChromaOffsetStruct ITU_YUV_420_4() { return { 0.0f, 1.0f }; }
    constexpr static C2ChromaOffsetStruct ITU_YUV_420_5() { return { 0.5f, 1.0f }; }

    float x;    ///< x offset in pixels (towards right)
    float y;    ///< y offset in pixels (towards down)

    DEFINE_AND_DESCRIBE_C2STRUCT(ChromaOffset)
    C2FIELD(x, "x")
    C2FIELD(y, "y")
};

struct C2ColorInfoStruct {
    C2ColorInfoStruct()
        : bitDepth(8), subsampling(C2Color::YUV_420) { }

    uint32_t bitDepth;
    C2Color::subsampling_t subsampling;
    C2ChromaOffsetStruct locations[]; // max 2 elements

    C2ColorInfoStruct(
            size_t /* flexCount */, uint32_t bitDepth_, C2Color::subsampling_t subsampling_)
        : bitDepth(bitDepth_), subsampling(subsampling_) { }

    C2ColorInfoStruct(
            size_t flexCount, uint32_t bitDepth_, C2Color::subsampling_t subsampling_,
            std::initializer_list<C2ChromaOffsetStruct> locations_)
        : bitDepth(bitDepth_), subsampling(subsampling_) {
        size_t ix = 0;
        for (const C2ChromaOffsetStruct &location : locations_) {
            if (ix == flexCount) {
                break;
            }
            locations[ix] = location;
            ++ix;
        }
    }

    DEFINE_AND_DESCRIBE_FLEX_C2STRUCT(ColorInfo, locations)
    C2FIELD(bitDepth, "bit-depth")
    C2FIELD(subsampling, "subsampling")
    C2FIELD(locations, "locations")
};

typedef C2StreamParam<C2Info, C2ColorInfoStruct, kParamIndexColorInfo> C2StreamColorInfo;
constexpr char C2_PARAMKEY_COLOR_INFO[] = "raw.color-format";
constexpr char C2_PARAMKEY_CODED_COLOR_INFO[] = "coded.color-format";

/**
 * Color Aspects
 */

/* The meaning of the following enumerators is as described in ITU-T H.273. */

/// Range
C2ENUM(C2Color::range_t, uint32_t,
    RANGE_UNSPECIFIED,          ///< range is unspecified
    RANGE_FULL,                 ///< full range
    RANGE_LIMITED,              ///< limited range

    RANGE_VENDOR_START = 0x80,  ///< vendor-specific range values start here
    RANGE_OTHER = 0XFF          ///< max value, reserved for undefined values
)

/// Color primaries
C2ENUM(C2Color::primaries_t, uint32_t,
    PRIMARIES_UNSPECIFIED,          ///< primaries are unspecified
    PRIMARIES_BT709,                ///< Rec.ITU-R BT.709-6 or equivalent
    PRIMARIES_BT470_M,              ///< Rec.ITU-R BT.470-6 System M or equivalent
    PRIMARIES_BT601_625,            ///< Rec.ITU-R BT.601-6 625 or equivalent
    PRIMARIES_BT601_525,            ///< Rec.ITU-R BT.601-6 525 or equivalent
    PRIMARIES_GENERIC_FILM,         ///< Generic Film
    PRIMARIES_BT2020,               ///< Rec.ITU-R BT.2020 or equivalent
    PRIMARIES_RP431,                ///< SMPTE RP 431-2 or equivalent
    PRIMARIES_EG432,                ///< SMPTE EG 432-1 or equivalent
    PRIMARIES_EBU3213,              ///< EBU Tech.3213-E or equivalent
                                    ///
    PRIMARIES_VENDOR_START = 0x80,  ///< vendor-specific primaries values start here
    PRIMARIES_OTHER = 0xff          ///< max value, reserved for undefined values
)

/// Transfer function
C2ENUM(C2Color::transfer_t, uint32_t,
    TRANSFER_UNSPECIFIED,           ///< transfer is unspecified
    TRANSFER_LINEAR,                ///< Linear transfer characteristics
    TRANSFER_SRGB,                  ///< sRGB or equivalent
    TRANSFER_170M,                  ///< SMPTE 170M or equivalent (e.g. BT.601/709/2020)
    TRANSFER_GAMMA22,               ///< Assumed display gamma 2.2
    TRANSFER_GAMMA28,               ///< Assumed display gamma 2.8
    TRANSFER_ST2084,                ///< SMPTE ST 2084 for 10/12/14/16 bit systems
    TRANSFER_HLG,                   ///< ARIB STD-B67 hybrid-log-gamma

    TRANSFER_240M = 0x40,           ///< SMPTE 240M or equivalent
    TRANSFER_XVYCC,                 ///< IEC 61966-2-4 or equivalent
    TRANSFER_BT1361,                ///< Rec.ITU-R BT.1361 extended gamut
    TRANSFER_ST428,                 ///< SMPTE ST 428-1 or equivalent
                                    ///
    TRANSFER_VENDOR_START = 0x80,   ///< vendor-specific transfer values start here
    TRANSFER_OTHER = 0xff           ///< max value, reserved for undefined values
)

/// Matrix coefficient
C2ENUM(C2Color::matrix_t, uint32_t,
    MATRIX_UNSPECIFIED,             ///< matrix coefficients are unspecified
    MATRIX_BT709,                   ///< Rec.ITU-R BT.709-5 or equivalent
    MATRIX_FCC47_73_682,            ///< FCC Title 47 CFR 73.682 or equivalent (KR=0.30, KB=0.11)
    MATRIX_BT601,                   ///< Rec.ITU-R BT.470, BT.601-6 625 or equivalent
    MATRIX_240M,                    ///< SMPTE 240M or equivalent
    MATRIX_BT2020,                  ///< Rec.ITU-R BT.2020 non-constant luminance
    MATRIX_BT2020_CONSTANT,         ///< Rec.ITU-R BT.2020 constant luminance
    MATRIX_VENDOR_START = 0x80,     ///< vendor-specific matrix coefficient values start here
    MATRIX_OTHER = 0xff,            ///< max value, reserved for undefined values
)

struct C2ColorAspectsStruct {
    C2Color::range_t range;
    C2Color::primaries_t primaries;
    C2Color::transfer_t transfer;
    C2Color::matrix_t matrix;

    C2ColorAspectsStruct()
        : range(C2Color::RANGE_UNSPECIFIED),
          primaries(C2Color::PRIMARIES_UNSPECIFIED),
          transfer(C2Color::TRANSFER_UNSPECIFIED),
          matrix(C2Color::MATRIX_UNSPECIFIED) { }

    C2ColorAspectsStruct(C2Color::range_t range_, C2Color::primaries_t primaries_,
                         C2Color::transfer_t transfer_, C2Color::matrix_t matrix_)
        : range(range_), primaries(primaries_), transfer(transfer_), matrix(matrix_) {}

    DEFINE_AND_DESCRIBE_C2STRUCT(ColorAspects)
    C2FIELD(range, "range")
    C2FIELD(primaries, "primaries")
    C2FIELD(transfer, "transfer")
    C2FIELD(matrix, "matrix")
};

typedef C2StreamParam<C2Info, C2ColorAspectsStruct, kParamIndexColorAspects>
        C2StreamColorAspectsInfo;
constexpr char C2_PARAMKEY_COLOR_ASPECTS[] = "raw.color";
constexpr char C2_PARAMKEY_VUI_COLOR_ASPECTS[] = "coded.vui.color";

/**
 * Default color aspects to use. These come from the container or client and shall be handled
 * according to the coding standard.
 */
typedef C2StreamParam<C2Tuning, C2ColorAspectsStruct, kParamIndexDefaultColorAspects>
        C2StreamColorAspectsTuning;
constexpr char C2_PARAMKEY_DEFAULT_COLOR_ASPECTS[] = "default.color";

/**
 * HDR Static Metadata Info.
 */
struct C2ColorXyStruct {
    float x; ///< x color coordinate in xyY space [0-1]
    float y; ///< y color coordinate in xyY space [0-1]

    DEFINE_AND_DESCRIBE_C2STRUCT(ColorXy)
    C2FIELD(x, "x")
    C2FIELD(y, "y")
};

struct C2MasteringDisplayColorVolumeStruct {
    C2ColorXyStruct red;    ///< coordinates of red display primary
    C2ColorXyStruct green;  ///< coordinates of green display primary
    C2ColorXyStruct blue;   ///< coordinates of blue display primary
    C2ColorXyStruct white;  ///< coordinates of white point

    float maxLuminance;  ///< max display mastering luminance in cd/m^2
    float minLuminance;  ///< min display mastering luminance in cd/m^2

    DEFINE_AND_DESCRIBE_C2STRUCT(MasteringDisplayColorVolume)
    C2FIELD(red, "red")
    C2FIELD(green, "green")
    C2FIELD(blue, "blue")
    C2FIELD(white, "white")

    C2FIELD(maxLuminance, "max-luminance")
    C2FIELD(minLuminance, "min-luminance")
};

struct C2HdrStaticMetadataStruct {
    C2MasteringDisplayColorVolumeStruct mastering;

    // content descriptors
    float maxCll;  ///< max content light level (pixel luminance) in cd/m^2
    float maxFall; ///< max frame average light level (frame luminance) in cd/m^2

    DEFINE_AND_DESCRIBE_BASE_C2STRUCT(HdrStaticMetadata)
    C2FIELD(mastering, "mastering")
    C2FIELD(maxCll, "max-cll")
    C2FIELD(maxFall, "max-fall")
};
typedef C2StreamParam<C2Info, C2HdrStaticMetadataStruct, kParamIndexHdrStaticMetadata>
        C2StreamHdrStaticMetadataInfo;
typedef C2StreamParam<C2Info, C2HdrStaticMetadataStruct, kParamIndexHdrStaticMetadata>
        C2StreamHdrStaticInfo;  // deprecated
constexpr char C2_PARAMKEY_HDR_STATIC_INFO[] = "raw.hdr-static-info";

/**
 * HDR10+ Metadata Info.
 *
 * Deprecated. Use C2StreamHdrDynamicMetadataInfo with
 * HDR_DYNAMIC_METADATA_TYPE_SMPTE_2094_40
 */
typedef C2StreamParam<C2Info, C2BlobValue, kParamIndexHdr10PlusMetadata>
        C2StreamHdr10PlusInfo;  // deprecated
constexpr char C2_PARAMKEY_INPUT_HDR10_PLUS_INFO[] = "input.hdr10-plus-info";  // deprecated
constexpr char C2_PARAMKEY_OUTPUT_HDR10_PLUS_INFO[] = "output.hdr10-plus-info";  // deprecated

/**
 * HDR dynamic metadata types
 */
C2ENUM(C2Config::hdr_dynamic_metadata_type_t, uint32_t,
    HDR_DYNAMIC_METADATA_TYPE_SMPTE_2094_10,  ///< SMPTE ST 2094-10
    HDR_DYNAMIC_METADATA_TYPE_SMPTE_2094_40,  ///< SMPTE ST 2094-40
)

struct C2HdrDynamicMetadataStruct {
    inline C2HdrDynamicMetadataStruct() { memset(this, 0, sizeof(*this)); }

    inline C2HdrDynamicMetadataStruct(
            size_t flexCount, C2Config::hdr_dynamic_metadata_type_t type)
        : type_(type) {
        memset(data, 0, flexCount);
    }

    C2Config::hdr_dynamic_metadata_type_t type_;
    uint8_t data[];

    DEFINE_AND_DESCRIBE_FLEX_C2STRUCT(HdrDynamicMetadata, data)
    C2FIELD(type_, "type")
    C2FIELD(data, "data")
};

/**
 * Dynamic HDR Metadata Info.
 */
typedef C2StreamParam<C2Info, C2HdrDynamicMetadataStruct, kParamIndexHdrDynamicMetadata>
        C2StreamHdrDynamicMetadataInfo;
constexpr char C2_PARAMKEY_INPUT_HDR_DYNAMIC_INFO[] = "input.hdr-dynamic-info";
constexpr char C2_PARAMKEY_OUTPUT_HDR_DYNAMIC_INFO[] = "output.hdr-dynamic-info";

/**
 * HDR Format
 */
C2ENUM(C2Config::hdr_format_t, uint32_t,
    UNKNOWN,     ///< HDR format not known (default)
    SDR,         ///< not HDR (SDR)
    HLG,         ///< HLG
    HDR10,       ///< HDR10
    HDR10_PLUS,  ///< HDR10+
);

/**
 * HDR Format Info
 *
 * This information may be present during configuration to allow encoders to
 * prepare encoding certain HDR formats. When this information is not present
 * before start, encoders should determine the HDR format based on the available
 * HDR metadata on the first input frame.
 *
 * While this information is optional, it is not a hint. When present, encoders
 * that do not support dynamic reconfiguration do not need to switch to the HDR
 * format based on the metadata on the first input frame.
 */
typedef C2StreamParam<C2Info, C2SimpleValueStruct<C2EasyEnum<C2Config::hdr_format_t>>,
                kParamIndexHdrFormat>
        C2StreamHdrFormatInfo;
constexpr char C2_PARAMKEY_HDR_FORMAT[] = "coded.hdr-format";

/* ------------------------------------ block-based coding ----------------------------------- */

/**
 * Block-size, block count and block rate. Used to determine or communicate profile-level
 * requirements.
 */
typedef C2StreamParam<C2Info, C2PictureSizeStruct, kParamIndexBlockSize> C2StreamBlockSizeInfo;
constexpr char C2_PARAMKEY_BLOCK_SIZE[] = "coded.block-size";

typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexBlockCount> C2StreamBlockCountInfo;
constexpr char C2_PARAMKEY_BLOCK_COUNT[] = "coded.block-count";

typedef C2StreamParam<C2Info, C2FloatValue, kParamIndexBlockRate> C2StreamBlockRateInfo;
constexpr char C2_PARAMKEY_BLOCK_RATE[] = "coded.block-rate";

/* ====================================== VIDEO COMPONENTS ====================================== */

/**
 * Frame rate (coded and port for raw data)
 *
 * Coded frame rates are what is represented in the compressed bitstream and should correspond to
 * the timestamp.
 *
 * Frame rates on raw ports should still correspond to the timestamps.
 *
 * For slow motion or timelapse recording, the timestamp shall be adjusted prior to feeding an
 * encoder, and the time stretch parameter should be used to signal the relationship between
 * timestamp and real-world time.
 */
typedef C2StreamParam<C2Info, C2FloatValue, kParamIndexFrameRate> C2StreamFrameRateInfo;
constexpr char C2_PARAMKEY_FRAME_RATE[] = "coded.frame-rate";

typedef C2PortParam<C2Info, C2FloatValue, kParamIndexFrameRate> C2PortFrameRateInfo;
constexpr char C2_PARAMKEY_INPUT_FRAME_RATE[] = "input.frame-rate";
constexpr char C2_PARAMKEY_OUTPUT_FRAME_RATE[] = "output.frame-rate";

/**
 * Time stretch. Ratio between real-world time and timestamp. E.g. time stretch of 4.0 means that
 * timestamp grows 1/4 the speed of real-world time (e.g. 4x slo-mo input). This can be used to
 * optimize encoding.
 */
typedef C2PortParam<C2Info, C2FloatValue, kParamIndexTimeStretch> C2PortTimeStretchInfo;
constexpr char C2_PARAMKEY_INPUT_TIME_STRETCH[] = "input.time-stretch";
constexpr char C2_PARAMKEY_OUTPUT_TIME_STRETCH[] = "output.time-stretch";

/**
 * Max video frame size.
 */
typedef C2StreamParam<C2Tuning, C2PictureSizeStruct, kParamIndexMaxPictureSize>
        C2StreamMaxPictureSizeTuning;
typedef C2StreamMaxPictureSizeTuning C2MaxVideoSizeHintPortSetting;
constexpr char C2_PARAMKEY_MAX_PICTURE_SIZE[] = "raw.max-size";

/**
 * Picture type mask.
 */
C2ENUM(C2Config::picture_type_t, uint32_t,
    SYNC_FRAME = (1 << 0),  ///< sync frame, e.g. IDR
    I_FRAME    = (1 << 1),  ///< intra frame that is completely encoded
    P_FRAME    = (1 << 2),  ///< inter predicted frame from previous frames
    B_FRAME    = (1 << 3),  ///< bidirectional predicted (out-of-order) frame
)

/**
 * Allowed picture types.
 */
typedef C2StreamParam<C2Tuning, C2SimpleValueStruct<C2EasyEnum<C2Config::picture_type_t>>,
                kParamIndexPictureTypeMask>
        C2StreamPictureTypeMaskTuning;
constexpr char C2_PARAMKEY_PICTURE_TYPE_MASK[] = "coding.picture-type-mask";

/**
 * Resulting picture type
 */
typedef C2StreamParam<C2Info, C2SimpleValueStruct<C2EasyEnum<C2Config::picture_type_t>>,
                kParamIndexPictureType>
        C2StreamPictureTypeInfo;
typedef C2StreamPictureTypeInfo C2StreamPictureTypeMaskInfo;
constexpr char C2_PARAMKEY_PICTURE_TYPE[] = "coded.picture-type";

/**
 * GOP specification.
 *
 * GOP is specified in layers between sync frames, by specifying the number of specific type of
 * frames between the previous type (starting with sync frames for the first layer):
 *
 * E.g.
 *      - 4 I frames between each sync frame
 *      - 2 P frames between each I frame
 *      - 1 B frame between each P frame
 *
 *      [ { I, 4 }, { P, 2 }, { B, 1 } ] ==> (Sync)BPBPB IBPBPB IBPBPB IBPBPB IBPBPB (Sync)BPBPB
 *
 * For infinite GOP, I layer can be omitted (as the first frame is always a sync frame.):
 *
 *      [ { P, MAX_UINT } ]   ==> (Sync)PPPPPPPPPPPPPPPPPP...
 *
 * Sync frames can also be requested on demand, and as a time-based interval. For time-based
 * interval, if there hasn't been a sync frame in at least the given time, the next I frame shall
 * be encoded as a sync frame.  For sync request, the next I frame shall be encoded as a sync frame.
 *
 * Temporal layering will determine GOP structure other than the I frame count between sync
 * frames.
 */
struct C2GopLayerStruct {
    C2GopLayerStruct() : type_((C2Config::picture_type_t)0), count(0) {}
    C2GopLayerStruct(C2Config::picture_type_t type, uint32_t count_)
        : type_(type), count(count_) { }

    C2Config::picture_type_t type_;
    uint32_t count;

    DEFINE_AND_DESCRIBE_C2STRUCT(GopLayer)
    C2FIELD(type_, "type")
    C2FIELD(count, "count")
};

typedef C2StreamParam<C2Tuning, C2SimpleArrayStruct<C2GopLayerStruct>, kParamIndexGop>
        C2StreamGopTuning;
constexpr char C2_PARAMKEY_GOP[] = "coding.gop";

/**
 * Quantization
 * min/max for each picture type
 *
 */
struct C2PictureQuantizationStruct {
    C2PictureQuantizationStruct() : type_((C2Config::picture_type_t)0),
                                         min(INT32_MIN), max(INT32_MAX) {}
    C2PictureQuantizationStruct(C2Config::picture_type_t type, int32_t min_, int32_t max_)
        : type_(type), min(min_), max(max_) { }

    C2Config::picture_type_t type_;
    int32_t min;      // INT32_MIN == 'no lower bound specified'
    int32_t max;      // INT32_MAX == 'no upper bound specified'

    DEFINE_AND_DESCRIBE_C2STRUCT(PictureQuantization)
    C2FIELD(type_, "type")
    C2FIELD(min, "min")
    C2FIELD(max, "max")
};

typedef C2StreamParam<C2Tuning, C2SimpleArrayStruct<C2PictureQuantizationStruct>,
        kParamIndexPictureQuantization> C2StreamPictureQuantizationTuning;
constexpr char C2_PARAMKEY_PICTURE_QUANTIZATION[] = "coding.qp";

/**
 * Sync frame can be requested on demand by the client.
 *
 * If true, the next I frame shall be encoded as a sync frame. This config can be passed
 * synchronously with the work, or directly to the component - leading to different result.
 * If it is passed with work, it shall take effect when that work item is being processed (so
 * the first I frame at or after that work item shall be a sync frame).
 */
typedef C2StreamParam<C2Tuning, C2EasyBoolValue, kParamIndexRequestSyncFrame>
        C2StreamRequestSyncFrameTuning;
constexpr char C2_PARAMKEY_REQUEST_SYNC_FRAME[] = "coding.request-sync-frame";

/**
 * Sync frame interval in time domain (timestamp).
 *
 * If there hasn't been a sync frame in at least this value, the next intra frame shall be encoded
 * as a sync frame. The value of MAX_I64 or a negative value means no sync frames after the first
 * frame. A value of 0 means all sync frames.
 */
typedef C2StreamParam<C2Tuning, C2Int64Value, kParamIndexSyncFrameInterval>
        C2StreamSyncFrameIntervalTuning;
constexpr char C2_PARAMKEY_SYNC_FRAME_INTERVAL[] = "coding.sync-frame-interval";

/**
 * Temporal layering
 *
 * Layer index is a value between 0 and layer count - 1. Layers with higher index have higher
 * frequency:
 *     0
 *   1   1
 *  2 2 2 2
 */
typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexLayerIndex> C2StreamLayerIndexInfo;
constexpr char C2_PARAMKEY_LAYER_INDEX[] = "coded.layer-index";

typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexLayerCount> C2StreamLayerCountInfo;
constexpr char C2_PARAMKEY_LAYER_COUNT[] = "coded.layer-count";

struct C2TemporalLayeringStruct {
    C2TemporalLayeringStruct()
        : layerCount(0), bLayerCount(0) { }

    C2TemporalLayeringStruct(size_t /* flexCount */, uint32_t layerCount_, uint32_t bLayerCount_)
        : layerCount(layerCount_), bLayerCount(c2_min(layerCount_, bLayerCount_)) { }

    C2TemporalLayeringStruct(size_t flexCount, uint32_t layerCount_, uint32_t bLayerCount_,
                             std::initializer_list<float> ratios)
        : layerCount(layerCount_), bLayerCount(c2_min(layerCount_, bLayerCount_)) {
        size_t ix = 0;
        for (float ratio : ratios) {
            if (ix == flexCount) {
                break;
            }
            bitrateRatios[ix++] = ratio;
        }
    }

    uint32_t layerCount;     ///< total number of layers (0 means no temporal layering)
    uint32_t bLayerCount;    ///< total number of bidirectional layers (<= num layers)
    /**
     * Bitrate budgets for each layer and the layers below, given as a ratio of the total
     * stream bitrate. This can be omitted or partially specififed by the client while configuring,
     * in which case the component shall fill in appropriate values for the missing layers.
     * This must be provided by the component when queried for at least layer count - 1 (as the
     * last layer's budget is always 1.0).
     */
    float bitrateRatios[];   ///< 1.0-based

    DEFINE_AND_DESCRIBE_FLEX_C2STRUCT(TemporalLayering, bitrateRatios)
    C2FIELD(layerCount, "layer-count")
    C2FIELD(bLayerCount, "b-layer-count")
    C2FIELD(bitrateRatios, "bitrate-ratios")
};

typedef C2StreamParam<C2Tuning, C2TemporalLayeringStruct, kParamIndexTemporalLayering>
        C2StreamTemporalLayeringTuning;
constexpr char C2_PARAMKEY_TEMPORAL_LAYERING[] = "coding.temporal-layering";

/**
 * Intra-refresh.
 */

C2ENUM(C2Config::intra_refresh_mode_t, uint32_t,
    INTRA_REFRESH_DISABLED,     ///< no intra refresh
    INTRA_REFRESH_ARBITRARY,    ///< arbitrary, unspecified
)

struct C2IntraRefreshStruct {
    C2IntraRefreshStruct()
        : mode(C2Config::INTRA_REFRESH_DISABLED), period(0.) { }

    C2IntraRefreshStruct(C2Config::intra_refresh_mode_t mode_, float period_)
        : mode(mode_), period(period_) { }

    C2Config::intra_refresh_mode_t mode; ///< refresh mode
    float period;         ///< intra refresh period in frames (must be >= 1), 0 means disabled

    DEFINE_AND_DESCRIBE_C2STRUCT(IntraRefresh)
    C2FIELD(mode, "mode")
    C2FIELD(period, "period")
};

typedef C2StreamParam<C2Tuning, C2IntraRefreshStruct, kParamIndexIntraRefresh>
        C2StreamIntraRefreshTuning;
constexpr char C2_PARAMKEY_INTRA_REFRESH[] = "coding.intra-refresh";

/* ====================================== IMAGE COMPONENTS ====================================== */

/**
 * Tile layout.
 *
 * This described how the image is decomposed into tiles.
 */
C2ENUM(C2Config::scan_order_t, uint32_t,
    SCAN_LEFT_TO_RIGHT_THEN_DOWN
)

struct C2TileLayoutStruct {
    C2PictureSizeStruct tile;       ///< tile size
    uint32_t columnCount;           ///< number of tiles horizontally
    uint32_t rowCount;              ///< number of tiles vertically
    C2Config::scan_order_t order;   ///< tile order

    DEFINE_AND_DESCRIBE_C2STRUCT(TileLayout)
    C2FIELD(tile, "tile")
    C2FIELD(columnCount, "columns")
    C2FIELD(rowCount, "rows")
    C2FIELD(order, "order")
};

typedef C2StreamParam<C2Info, C2TileLayoutStruct, kParamIndexTileLayout> C2StreamTileLayoutInfo;
constexpr char C2_PARAMKEY_TILE_LAYOUT[] = "coded.tile-layout";

/**
 * Tile handling.
 *
 * Whether to concatenate tiles or output them each.
 */
C2ENUM(C2Config::tiling_mode_t, uint32_t,
    TILING_SEPARATE,    ///< output each tile in a separate onWorkDone
    TILING_CONCATENATE  ///< output one work completion per frame (concatenate tiles)
)

typedef C2StreamParam<C2Tuning, C2TileLayoutStruct, kParamIndexTileHandling>
        C2StreamTileHandlingTuning;
constexpr char C2_PARAMKEY_TILE_HANDLING[] = "coding.tile-handling";

/* ====================================== AUDIO COMPONENTS ====================================== */

/**
 * Sample rate
 */
typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexSampleRate> C2StreamSampleRateInfo;
constexpr char C2_PARAMKEY_SAMPLE_RATE[] = "raw.sample-rate";
constexpr char C2_PARAMKEY_CODED_SAMPLE_RATE[] = "coded.sample-rate";

/**
 * Channel count.
 */
typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexChannelCount> C2StreamChannelCountInfo;
constexpr char C2_PARAMKEY_CHANNEL_COUNT[] = "raw.channel-count";
constexpr char C2_PARAMKEY_CODED_CHANNEL_COUNT[] = "coded.channel-count";

/**
 * Max channel count. Used to limit the number of coded or decoded channels.
 */
typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexMaxChannelCount> C2StreamMaxChannelCountInfo;
constexpr char C2_PARAMKEY_MAX_CHANNEL_COUNT[] = "raw.max-channel-count";
constexpr char C2_PARAMKEY_MAX_CODED_CHANNEL_COUNT[] = "coded.max-channel-count";

/**
 * Audio channel mask. Used by decoder to express audio channel mask of decoded content,
 * or by encoder for the channel mask of the encoded content once decoded.
 * Channel representation is specified according to the Java android.media.AudioFormat
 * CHANNEL_OUT_* constants.
 */
 typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexAndroidChannelMask> C2StreamChannelMaskInfo;
 const char C2_PARAMKEY_CHANNEL_MASK[] = "raw.channel-mask";

/**
 * Audio sample format (PCM encoding)
 */
C2ENUM(C2Config::pcm_encoding_t, uint32_t,
    PCM_16,
    PCM_8,
    PCM_FLOAT,
    PCM_24,
    PCM_32
)

typedef C2StreamParam<C2Info, C2SimpleValueStruct<C2Config::pcm_encoding_t>, kParamIndexPcmEncoding>
        C2StreamPcmEncodingInfo;
constexpr char C2_PARAMKEY_PCM_ENCODING[] = "raw.pcm-encoding";
constexpr char C2_PARAMKEY_CODED_PCM_ENCODING[] = "coded.pcm-encoding";

/**
 * AAC SBR Mode. Used during encoding.
 */
C2ENUM(C2Config::aac_sbr_mode_t, uint32_t,
    AAC_SBR_OFF,
    AAC_SBR_SINGLE_RATE,
    AAC_SBR_DUAL_RATE,
    AAC_SBR_AUTO ///< let the codec decide
)

typedef C2StreamParam<C2Info, C2SimpleValueStruct<C2Config::aac_sbr_mode_t>, kParamIndexAacSbrMode>
        C2StreamAacSbrModeTuning;
constexpr char C2_PARAMKEY_AAC_SBR_MODE[] = "coding.aac-sbr-mode";

/**
 * DRC Compression. Used during decoding.
 */
C2ENUM(C2Config::drc_compression_mode_t, int32_t,
    DRC_COMPRESSION_ODM_DEFAULT, ///< odm's default
    DRC_COMPRESSION_NONE,
    DRC_COMPRESSION_LIGHT,
    DRC_COMPRESSION_HEAVY ///<
)

typedef C2StreamParam<C2Info, C2SimpleValueStruct<C2Config::drc_compression_mode_t>,
                kParamIndexDrcCompression>
        C2StreamDrcCompressionModeTuning;
constexpr char C2_PARAMKEY_DRC_COMPRESSION_MODE[] = "coding.drc.compression-mode";

/**
 * DRC target reference level in dBFS. Used during decoding.
 */
typedef C2StreamParam<C2Info, C2FloatValue, kParamIndexDrcTargetReferenceLevel>
        C2StreamDrcTargetReferenceLevelTuning;
constexpr char C2_PARAMKEY_DRC_TARGET_REFERENCE_LEVEL[] = "coding.drc.reference-level";

/**
 * DRC target reference level in dBFS. Used during decoding.
 */
typedef C2StreamParam<C2Info, C2FloatValue, kParamIndexDrcEncodedTargetLevel>
        C2StreamDrcEncodedTargetLevelTuning;
constexpr char C2_PARAMKEY_DRC_ENCODED_TARGET_LEVEL[] = "coding.drc.encoded-level";

/**
 * DRC target reference level in dBFS. Used during decoding.
 */
typedef C2StreamParam<C2Info, C2FloatValue, kParamIndexDrcBoostFactor>
        C2StreamDrcBoostFactorTuning;
constexpr char C2_PARAMKEY_DRC_BOOST_FACTOR[] = "coding.drc.boost-factor";

/**
 * DRC target reference level in dBFS. Used during decoding.
 */
typedef C2StreamParam<C2Info, C2FloatValue, kParamIndexDrcAttenuationFactor>
        C2StreamDrcAttenuationFactorTuning;
constexpr char C2_PARAMKEY_DRC_ATTENUATION_FACTOR[] = "coding.drc.attenuation-factor";

/**
 * DRC Effect Type (see ISO 23003-4) Uniform Dynamic Range Control. Used during decoding.
 */
C2ENUM(C2Config::drc_effect_type_t, int32_t,
    DRC_EFFECT_ODM_DEFAULT = -2, ///< odm's default
    DRC_EFFECT_OFF = -1,    ///< no DRC
    DRC_EFFECT_NONE = 0,    ///< no DRC except to prevent clipping
    DRC_EFFECT_LATE_NIGHT,
    DRC_EFFECT_NOISY_ENVIRONMENT,
    DRC_EFFECT_LIMITED_PLAYBACK_RANGE,
    DRC_EFFECT_LOW_PLAYBACK_LEVEL,
    DRC_EFFECT_DIALOG_ENHANCEMENT,
    DRC_EFFECT_GENERAL_COMPRESSION
)

typedef C2StreamParam<C2Info, C2SimpleValueStruct<C2Config::drc_effect_type_t>,
                kParamIndexDrcEffectType>
        C2StreamDrcEffectTypeTuning;
constexpr char C2_PARAMKEY_DRC_EFFECT_TYPE[] = "coding.drc.effect-type";

/**
 * DRC album mode. Used during decoding.
 */
C2ENUM(C2Config::drc_album_mode_t, int32_t,
    DRC_ALBUM_MODE_OFF = 0,
    DRC_ALBUM_MODE_ON = 1
)
typedef C2StreamParam<C2Info, C2SimpleValueStruct<C2Config::drc_album_mode_t>, kParamIndexDrcAlbumMode>
        C2StreamDrcAlbumModeTuning;
constexpr char C2_PARAMKEY_DRC_ALBUM_MODE[] = "coding.drc.album-mode";

/**
 * DRC output loudness in dBFS. Retrieved during decoding
 */
typedef C2StreamParam<C2Info, C2FloatValue, kParamIndexDrcOutputLoudness>
        C2StreamDrcOutputLoudnessTuning;
constexpr char C2_PARAMKEY_DRC_OUTPUT_LOUDNESS[] = "output.drc.output-loudness";

/**
 * Audio frame size in samples.
 *
 * Audio encoders can expose this parameter to signal the desired audio frame
 * size that corresponds to a single coded access unit.
 * Default value is 0, meaning that the encoder accepts input buffers of any size.
 */
typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexAudioFrameSize>
        C2StreamAudioFrameSizeInfo;
constexpr char C2_PARAMKEY_AUDIO_FRAME_SIZE[] = "raw.audio-frame-size";

/**
 * Information for an access unit in a large frame (containing multiple access units)
 */
struct C2AccessUnitInfosStruct {

    inline C2AccessUnitInfosStruct() {
        memset(this, 0, sizeof(*this));
    }

    inline C2AccessUnitInfosStruct(
            uint32_t flags_,
            uint32_t size_,
            int64_t timestamp_)
        : flags(flags_),
          size(size_),
          timestamp(timestamp_) { }

    uint32_t flags; ///<flags for the access-unit
    uint32_t size; ///<size of access-unit
    int64_t timestamp; ///<timestamp in us for the access-unit

    DEFINE_AND_DESCRIBE_C2STRUCT(AccessUnitInfos)
    C2FIELD(flags, "flags")
    C2FIELD(size, "size")
    C2FIELD(timestamp, "timestamp")
};

/**
 * Multiple access unit support (e.g large audio frames)
 *
 * If supported by a component, multiple access units may be contained
 * in a single work item. For now this is only defined for linear buffers.
 * The metadata indicates the access-unit boundaries in a single buffer.
 * The boundary of each access-units are marked by its size, immediately
 * followed by the next access-unit.
 */
typedef C2StreamParam<C2Info, C2SimpleArrayStruct<C2AccessUnitInfosStruct>,
                kParamIndexAccessUnitInfos>
        C2AccessUnitInfos;

constexpr char C2_PARAMKEY_INPUT_ACCESS_UNIT_INFOS[] = "input.access-unit-infos";
constexpr char C2_PARAMKEY_OUTPUT_ACCESS_UNIT_INFOS[] = "output.access-unit-infos";

/* --------------------------------------- AAC components --------------------------------------- */

/**
 * AAC stream format
 */
C2ENUM(C2Config::aac_packaging_t, uint32_t,
    AAC_PACKAGING_RAW,
    AAC_PACKAGING_ADTS
)

typedef C2StreamParam<C2Info, C2SimpleValueStruct<C2EasyEnum<C2Config::aac_packaging_t>>,
        kParamIndexAacPackaging> C2StreamAacPackagingInfo;
typedef C2StreamAacPackagingInfo C2StreamAacFormatInfo;
constexpr char C2_PARAMKEY_AAC_PACKAGING[] = "coded.aac-packaging";

/* ================================ PLATFORM-DEFINED PARAMETERS ================================ */

/**
 * Platform level and features.
 */
enum C2Config::platform_level_t : uint32_t {
    PLATFORM_P,   ///< support for Android 9.0 feature set
};

// read-only
typedef C2GlobalParam<C2Setting, C2SimpleValueStruct<C2Config::platform_level_t>,
                kParamIndexPlatformLevel>
        C2PlatformLevelSetting;
constexpr char C2_PARAMKEY_PLATFORM_LEVEL[] = "api.platform-level";

enum C2Config::platform_feature_t : uint64_t {
    // no platform-specific features have been defined
};

// read-only
typedef C2GlobalParam<C2Setting, C2SimpleValueStruct<C2Config::platform_feature_t>,
                kParamIndexPlatformFeatures>
        C2PlatformFeaturesSetting;
constexpr char C2_PARAMKEY_PLATFORM_FEATURES[] = "api.platform-features";

/**
 * Resource IDs
 */
enum C2PlatformConfig::resource_id_t : uint32_t {
    DMABUF_MEMORY = 16,  ///< memory allocated from a platform allocator (dmabuf or gralloc)

    /// vendor defined resource IDs start from here
    VENDOR_START = 0x1000,
};

/**
 * This structure describes the preferred ion allocation parameters for a given memory usage.
 */
struct C2StoreIonUsageStruct {
    inline C2StoreIonUsageStruct() {
        memset(this, 0, sizeof(*this));
    }

    inline C2StoreIonUsageStruct(uint64_t usage_, uint32_t capacity_)
        : usage(usage_), capacity(capacity_), heapMask(0), allocFlags(0), minAlignment(0) { }

    uint64_t usage;        ///< C2MemoryUsage
    uint32_t capacity;     ///< capacity
    int32_t heapMask;      ///< ion heapMask
    int32_t allocFlags;    ///< ion allocation flags
    uint32_t minAlignment; ///< minimum alignment

    DEFINE_AND_DESCRIBE_C2STRUCT(StoreIonUsage)
    C2FIELD(usage, "usage")
    C2FIELD(capacity, "capacity")
    C2FIELD(heapMask, "heap-mask")
    C2FIELD(allocFlags, "alloc-flags")
    C2FIELD(minAlignment, "min-alignment")
};

// store, private
typedef C2GlobalParam<C2Info, C2StoreIonUsageStruct, kParamIndexStoreIonUsage>
        C2StoreIonUsageInfo;

/**
 * This structure describes the preferred DMA-Buf allocation parameters for a given memory usage.
 */
struct C2StoreDmaBufUsageStruct {
    inline C2StoreDmaBufUsageStruct() { memset(this, 0, sizeof(*this)); }

    inline C2StoreDmaBufUsageStruct(size_t flexCount, uint64_t usage_, uint32_t capacity_)
        : usage(usage_), capacity(capacity_), allocFlags(0) {
        memset(heapName, 0, flexCount);
    }

    uint64_t usage;                         ///< C2MemoryUsage
    uint32_t capacity;                      ///< capacity
    int32_t allocFlags;                     ///< ion allocation flags
    char heapName[];                        ///< dmabuf heap name

    DEFINE_AND_DESCRIBE_FLEX_C2STRUCT(StoreDmaBufUsage, heapName)
    C2FIELD(usage, "usage")
    C2FIELD(capacity, "capacity")
    C2FIELD(allocFlags, "alloc-flags")
    C2FIELD(heapName, "heap-name")
};

// store, private
typedef C2GlobalParam<C2Info, C2StoreDmaBufUsageStruct, kParamIndexStoreDmaBufUsage>
        C2StoreDmaBufUsageInfo;

/**
 * Flexible pixel format descriptors
 */
struct C2FlexiblePixelFormatDescriptorStruct {
    uint32_t pixelFormat;
    uint32_t bitDepth;
    C2Color::subsampling_t subsampling;
    C2Color::plane_layout_t layout;

    DEFINE_AND_DESCRIBE_C2STRUCT(FlexiblePixelFormatDescriptor)
    C2FIELD(pixelFormat, "pixel-format")
    C2FIELD(bitDepth, "bit-depth")
    C2FIELD(subsampling, "subsampling")
    C2FIELD(layout, "layout")
};

/**
 * Plane layout of flexible pixel formats.
 *
 * bpp: bytes per color component, e.g. 1 for 8-bit formats, and 2 for 10-16-bit formats.
 */
C2ENUM(C2Color::plane_layout_t, uint32_t,
       /** Unknown layout */
       UNKNOWN_LAYOUT,

       /** Planar layout with rows of each plane packed (colInc = bpp) */
       PLANAR_PACKED,

       /** Semiplanar layout with rows of each plane packed (colInc_Y/A = bpp (planar),
        *  colInc_Cb/Cr = 2*bpp (interleaved). Used only for YUV(A) formats. */
       SEMIPLANAR_PACKED,

       /** Interleaved packed. colInc = N*bpp (N are the number of color components) */
       INTERLEAVED_PACKED,

       /** Interleaved aligned. colInc = smallest power of 2 >= N*bpp (N are the number of color
        *  components) */
       INTERLEAVED_ALIGNED
)

typedef C2GlobalParam<C2Info, C2SimpleArrayStruct<C2FlexiblePixelFormatDescriptorStruct>,
                kParamIndexFlexiblePixelFormatDescriptors>
        C2StoreFlexiblePixelFormatDescriptorsInfo;

/**
 * This structure describes the android dataspace for a raw video/image frame.
 */
typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexDataSpace> C2StreamDataSpaceInfo;
constexpr char C2_PARAMKEY_DATA_SPACE[] = "raw.data-space";

/**
 * This structure describes the android surface scaling mode for a raw video/image frame.
 */
typedef C2StreamParam<C2Info, C2Uint32Value, kParamIndexSurfaceScaling> C2StreamSurfaceScalingInfo;
constexpr char C2_PARAMKEY_SURFACE_SCALING_MODE[] = "raw.surface-scaling";

/* ======================================= INPUT SURFACE ======================================= */

/**
 * Input surface EOS
 */
typedef C2GlobalParam<C2Tuning, C2EasyBoolValue, kParamIndexInputSurfaceEos>
        C2InputSurfaceEosTuning;
constexpr char C2_PARAMKEY_INPUT_SURFACE_EOS[] = "input-surface.eos";

/**
 * Start/suspend/resume/stop controls and timestamps for input surface.
 *
 * TODO: make these counters
 */

struct C2TimedControlStruct {
    c2_bool_t enabled; ///< control is enabled
    int64_t timestamp; ///< if enabled, time the control should take effect

    C2TimedControlStruct()
        : enabled(C2_FALSE), timestamp(0) { }

    /* implicit */ C2TimedControlStruct(uint64_t timestamp_)
        : enabled(C2_TRUE), timestamp(timestamp_) { }

    DEFINE_AND_DESCRIBE_C2STRUCT(TimedControl)
    C2FIELD(enabled,   "enabled")
    C2FIELD(timestamp, "timestamp")
};

typedef C2PortParam<C2Tuning, C2TimedControlStruct, kParamIndexStartAt>
        C2PortStartTimestampTuning;
constexpr char C2_PARAMKEY_INPUT_SURFACE_START_AT[] = "input-surface.start";
typedef C2PortParam<C2Tuning, C2TimedControlStruct, kParamIndexSuspendAt>
        C2PortSuspendTimestampTuning;
constexpr char C2_PARAMKEY_INPUT_SURFACE_SUSPEND_AT[] = "input-surface.suspend";
typedef C2PortParam<C2Tuning, C2TimedControlStruct, kParamIndexResumeAt>
        C2PortResumeTimestampTuning;
constexpr char C2_PARAMKEY_INPUT_SURFACE_RESUME_AT[] = "input-surface.resume";
typedef C2PortParam<C2Tuning, C2TimedControlStruct, kParamIndexStopAt>
        C2PortStopTimestampTuning;
constexpr char C2_PARAMKEY_INPUT_SURFACE_STOP_AT[] = "input-surface.stop";

/**
 * Time offset for input surface. Input timestamp to codec is surface buffer timestamp plus this
 * time offset.
 */
typedef C2GlobalParam<C2Tuning, C2Int64Value, kParamIndexTimeOffset> C2ComponentTimeOffsetTuning;
constexpr char C2_PARAMKEY_INPUT_SURFACE_TIME_OFFSET[] = "input-surface.time-offset";

/**
 * Minimum fps for input surface.
 *
 * Repeat frame to meet this.
 */
typedef C2PortParam<C2Tuning, C2FloatValue, kParamIndexMinFrameRate> C2PortMinFrameRateTuning;
constexpr char C2_PARAMKEY_INPUT_SURFACE_MIN_FRAME_RATE[] = "input-surface.min-frame-rate";

/**
 * Maximum fps for input surface.
 *
 * Drop frame to meet this.
 */
typedef C2PortParam<C2Tuning, C2FloatValue, kParamIndexMaxFrameRate> C2PortMaxFrameRateTuning;
constexpr char C2_PARAMKEY_INPUT_SURFACE_MAX_FRAME_RATE[] = "input-surface.max-frame-rate";

/**
 * Timestamp adjustment (override) for input surface buffers. These control the input timestamp
 * fed to the codec, but do not impact the output timestamp.
 */
struct C2TimestampGapAdjustmentStruct {
    /// control modes
    enum mode_t : uint32_t;

    inline C2TimestampGapAdjustmentStruct();

    inline C2TimestampGapAdjustmentStruct(mode_t mode_, uint64_t value_)
        : mode(mode_), value(value_) { }

    mode_t mode;    ///< control mode
    uint64_t value; ///< control value for gap between two timestamp

    DEFINE_AND_DESCRIBE_C2STRUCT(TimestampGapAdjustment)
    C2FIELD(mode, "mode")
    C2FIELD(value, "value")
};

C2ENUM(C2TimestampGapAdjustmentStruct::mode_t, uint32_t,
    NONE,
    MIN_GAP,
    FIXED_GAP,
);

inline C2TimestampGapAdjustmentStruct::C2TimestampGapAdjustmentStruct()
    : mode(C2TimestampGapAdjustmentStruct::NONE), value(0) { }

typedef C2PortParam<C2Tuning, C2TimestampGapAdjustmentStruct, kParamIndexTimestampGapAdjustment>
        C2PortTimestampGapTuning;
constexpr char C2_PARAMKEY_INPUT_SURFACE_TIMESTAMP_ADJUSTMENT[] = "input-surface.timestamp-adjustment";

/**
 * Capture frame rate for input surface. During timelapse or slowmo encoding,
 * this represents the frame rate of input surface.
 */
typedef C2PortParam<C2Tuning, C2FloatValue, kParamIndexCaptureFrameRate>
        C2PortCaptureFrameRateTuning;
constexpr char C2_PARAMKEY_INPUT_SURFACE_CAPTURE_FRAME_RATE[] = "input-surface.capture-frame-rate";

/**
 * Stop time offset for input surface. Stop time offset is the elapsed time
 * offset to the last frame time from the stop time. This could be returned from
 * IInputSurface when it is queried.
 */
typedef C2PortParam<C2Tuning, C2Int64Value, kParamIndexStopTimeOffset> C2PortStopTimeOffset;
constexpr char C2_PARAMKEY_INPUT_SURFACE_STOP_TIME_OFFSET[] = "input-surface.stop-time-offset";

/* ===================================== TUNNELED CODEC ==================================== */

/**
 * Tunneled codec control.
 */
struct C2TunneledModeStruct {
    /// mode
    enum mode_t : uint32_t;
    /// sync type
    enum sync_type_t : uint32_t;

    inline C2TunneledModeStruct() = default;

    inline C2TunneledModeStruct(
            size_t flexCount, mode_t mode_, sync_type_t type, std::vector<int32_t> id)
        : mode(mode_), syncType(type) {
        memcpy(&syncId, &id[0], c2_min(id.size(), flexCount) * FLEX_SIZE);
    }

    inline C2TunneledModeStruct(size_t flexCount, mode_t mode_, sync_type_t type, int32_t id)
        : mode(mode_), syncType(type) {
        if (flexCount >= 1) {
            syncId[0] = id;
        }
    }

    mode_t mode;          ///< tunneled mode
    sync_type_t syncType; ///< type of sync used for tunneled mode
    int32_t syncId[];     ///< sync id

    DEFINE_AND_DESCRIBE_FLEX_C2STRUCT(TunneledMode, syncId)
    C2FIELD(mode, "mode")
    C2FIELD(syncType, "sync-type")
    C2FIELD(syncId, "sync-id")

};

C2ENUM(C2TunneledModeStruct::mode_t, uint32_t,
    NONE,
    SIDEBAND,
);


C2ENUM(C2TunneledModeStruct::sync_type_t, uint32_t,
    REALTIME,
    AUDIO_HW_SYNC,
    HW_AV_SYNC,
);

/**
 * Configure tunneled mode
 */
typedef C2PortParam<C2Tuning, C2TunneledModeStruct, kParamIndexTunneledMode>
        C2PortTunneledModeTuning;
constexpr char C2_PARAMKEY_TUNNELED_RENDER[] = "output.tunneled-render";

/**
 * Tunneled mode handle. The meaning of this is depends on the
 * tunneled mode. If the tunneled mode is SIDEBAND, this is the
 * sideband handle.
 */
typedef C2PortParam<C2Tuning, C2Int32Array, kParamIndexTunnelHandle> C2PortTunnelHandleTuning;
constexpr char C2_PARAMKEY_OUTPUT_TUNNEL_HANDLE[] = "output.tunnel-handle";

/**
 * The system time using CLOCK_MONOTONIC in nanoseconds at the tunnel endpoint.
 * For decoders this is the render time for the output frame and
 * this corresponds to the media timestamp of the output frame.
 */
typedef C2PortParam<C2Info, C2SimpleValueStruct<int64_t>, kParamIndexTunnelSystemTime>
        C2PortTunnelSystemTime;
constexpr char C2_PARAMKEY_OUTPUT_RENDER_TIME[] = "output.render-time";


/**
 * Tunneled mode video peek signaling flag.
 *
 * When a video frame is pushed to the decoder with this parameter set to true,
 * the decoder must decode the frame, signal partial completion, and hold on the
 * frame until C2StreamTunnelStartRender is set to true (which resets this
 * flag). Flush will also result in the frames being returned back to the
 * client (but not rendered).
 */
typedef C2StreamParam<C2Info, C2EasyBoolValue, kParamIndexTunnelHoldRender>
        C2StreamTunnelHoldRender;
constexpr char C2_PARAMKEY_TUNNEL_HOLD_RENDER[] = "output.tunnel-hold-render";

/**
 * Tunneled mode video peek signaling flag.
 *
 * Upon receiving this flag, the decoder shall set C2StreamTunnelHoldRender to
 * false, which shall cause any frames held for rendering to be immediately
 * displayed, regardless of their timestamps.
*/
typedef C2StreamParam<C2Info, C2EasyBoolValue, kParamIndexTunnelStartRender>
        C2StreamTunnelStartRender;
constexpr char C2_PARAMKEY_TUNNEL_START_RENDER[] = "output.tunnel-start-render";

/** Tunnel Peek Mode. */
C2ENUM(C2PlatformConfig::tunnel_peek_mode_t, uint32_t,
    UNSPECIFIED_PEEK = 0,
    SPECIFIED_PEEK = 1
);

/**
 * Tunnel Peek Mode Tuning parameter.
 *
 * If set to UNSPECIFIED_PEEK_MODE, the decoder is free to ignore the
 * C2StreamTunnelHoldRender and C2StreamTunnelStartRender flags and associated
 * features. Additionally, it becomes up to the decoder to display any frame
 * before receiving synchronization information.
 *
 * Note: This parameter allows a decoder to ignore the video peek machinery and
 * to revert to its preferred behavior.
 */
typedef C2StreamParam<C2Tuning,
        C2SimpleValueStruct<C2EasyEnum<C2PlatformConfig::tunnel_peek_mode_t>>,
        kParamIndexTunnelPeekMode> C2StreamTunnelPeekModeTuning;
constexpr char C2_PARAMKEY_TUNNEL_PEEK_MODE[] =
        "output.tunnel-peek-mode";

/**
 * Encoding quality level signaling.
 *
 * Signal the 'minimum encoding quality' introduced in Android 12/S. It indicates
 * whether the underlying codec is expected to take extra steps to ensure quality meets the
 * appropriate minimum. A value of NONE indicates that the codec is not to apply
 * any minimum quality bar requirements. Other values indicate that the codec is to apply
 * a minimum quality bar, with the exact quality bar being decided by the parameter value.
 */
typedef C2GlobalParam<C2Setting,
        C2SimpleValueStruct<C2EasyEnum<C2PlatformConfig::encoding_quality_level_t>>,
        kParamIndexEncodingQualityLevel> C2EncodingQualityLevel;
constexpr char C2_PARAMKEY_ENCODING_QUALITY_LEVEL[] = "algo.encoding-quality-level";

C2ENUM(C2PlatformConfig::encoding_quality_level_t, uint32_t,
    NONE = 0,
    S_HANDHELD = 1              // corresponds to VMAF=70
);

/**
 * Display processing token.
 *
 * An int64 token specifying the display processing configuration for the frame.
 * This value is passed to IGraphicBufferProducer via QueueBufferInput::setPictureProfileHandle().
 */
typedef C2StreamParam<C2Info, C2Int64Value, kParamIndexDisplayProcessingToken>
        C2StreamDisplayProcessingToken;
constexpr char C2_PARAMKEY_DISPLAY_PROCESSING_TOKEN[] = "display-processing-token";

/**
 * Video Encoding Statistics Export
 */

/**
 * Average block QP exported from video encoder.
 */
typedef C2StreamParam<C2Info, C2SimpleValueStruct<int32_t>, kParamIndexAverageBlockQuantization>
        C2AndroidStreamAverageBlockQuantizationInfo;
constexpr char C2_PARAMKEY_AVERAGE_QP[] = "coded.average-qp";

/// @}

#endif  // C2CONFIG_H_
