/*
 * Copyright 2018 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "Codec2Mapper"
#include <utils/Log.h>

#include <map>
#include <optional>

#include <media/stagefright/MediaCodecConstants.h>
#include <media/stagefright/SurfaceUtils.h>
#include <media/stagefright/foundation/ALookup.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <media/stagefright/foundation/MediaDefs.h>

#include <stdint.h>  // for INT32_MAX

#include "Codec2Mapper.h"

using namespace android;

namespace {

ALookup<C2Config::profile_t, int32_t> sAacProfiles = {
    { C2Config::PROFILE_AAC_LC,         AACObjectLC },
    { C2Config::PROFILE_AAC_MAIN,       AACObjectMain },
    { C2Config::PROFILE_AAC_SSR,        AACObjectSSR },
    { C2Config::PROFILE_AAC_LTP,        AACObjectLTP },
    { C2Config::PROFILE_AAC_HE,         AACObjectHE },
    { C2Config::PROFILE_AAC_SCALABLE,   AACObjectScalable },
    { C2Config::PROFILE_AAC_ER_LC,      AACObjectERLC },
    { C2Config::PROFILE_AAC_ER_SCALABLE, AACObjectERScalable },
    { C2Config::PROFILE_AAC_LD,         AACObjectLD },
    { C2Config::PROFILE_AAC_HE_PS,      AACObjectHE_PS },
    { C2Config::PROFILE_AAC_ELD,        AACObjectELD },
    { C2Config::PROFILE_AAC_XHE,        AACObjectXHE },
};

ALookup<C2Config::level_t, int32_t> sAvcLevels = {
    { C2Config::LEVEL_AVC_1,    AVCLevel1 },
    { C2Config::LEVEL_AVC_1B,   AVCLevel1b },
    { C2Config::LEVEL_AVC_1_1,  AVCLevel11 },
    { C2Config::LEVEL_AVC_1_2,  AVCLevel12 },
    { C2Config::LEVEL_AVC_1_3,  AVCLevel13 },
    { C2Config::LEVEL_AVC_2,    AVCLevel2 },
    { C2Config::LEVEL_AVC_2_1,  AVCLevel21 },
    { C2Config::LEVEL_AVC_2_2,  AVCLevel22 },
    { C2Config::LEVEL_AVC_3,    AVCLevel3 },
    { C2Config::LEVEL_AVC_3_1,  AVCLevel31 },
    { C2Config::LEVEL_AVC_3_2,  AVCLevel32 },
    { C2Config::LEVEL_AVC_4,    AVCLevel4 },
    { C2Config::LEVEL_AVC_4_1,  AVCLevel41 },
    { C2Config::LEVEL_AVC_4_2,  AVCLevel42 },
    { C2Config::LEVEL_AVC_5,    AVCLevel5 },
    { C2Config::LEVEL_AVC_5_1,  AVCLevel51 },
    { C2Config::LEVEL_AVC_5_2,  AVCLevel52 },
    { C2Config::LEVEL_AVC_6,    AVCLevel6 },
    { C2Config::LEVEL_AVC_6_1,  AVCLevel61 },
    { C2Config::LEVEL_AVC_6_2,  AVCLevel62 },
};

ALookup<C2Config::profile_t, int32_t> sAvcProfiles = {
    // treat restricted profiles as full profile if there is no equivalent - which works for
    // decoders, but not for encoders
    { C2Config::PROFILE_AVC_BASELINE,               AVCProfileBaseline },
    { C2Config::PROFILE_AVC_CONSTRAINED_BASELINE,   AVCProfileConstrainedBaseline },
    { C2Config::PROFILE_AVC_MAIN,                   AVCProfileMain },
    { C2Config::PROFILE_AVC_EXTENDED,               AVCProfileExtended },
    { C2Config::PROFILE_AVC_HIGH,                   AVCProfileHigh },
    { C2Config::PROFILE_AVC_PROGRESSIVE_HIGH,       AVCProfileHigh },
    { C2Config::PROFILE_AVC_CONSTRAINED_HIGH,       AVCProfileConstrainedHigh },
    { C2Config::PROFILE_AVC_HIGH_10,                AVCProfileHigh10 },
    { C2Config::PROFILE_AVC_PROGRESSIVE_HIGH_10,    AVCProfileHigh10 },
    { C2Config::PROFILE_AVC_HIGH_422,               AVCProfileHigh422 },
    { C2Config::PROFILE_AVC_HIGH_444_PREDICTIVE,    AVCProfileHigh444 },
    { C2Config::PROFILE_AVC_HIGH_10_INTRA,          AVCProfileHigh10 },
    { C2Config::PROFILE_AVC_HIGH_422_INTRA,         AVCProfileHigh422 },
    { C2Config::PROFILE_AVC_HIGH_444_INTRA,         AVCProfileHigh444 },
    { C2Config::PROFILE_AVC_CAVLC_444_INTRA,        AVCProfileHigh444 },
};

ALookup<C2Config::bitrate_mode_t, int32_t> sBitrateModes = {
    { C2Config::BITRATE_CONST,      BITRATE_MODE_CBR },
    { C2Config::BITRATE_CONST_SKIP_ALLOWED, BITRATE_MODE_CBR_FD },
    { C2Config::BITRATE_VARIABLE,   BITRATE_MODE_VBR },
    { C2Config::BITRATE_IGNORE,     BITRATE_MODE_CQ },
};

ALookup<C2Color::matrix_t, ColorAspects::MatrixCoeffs> sColorMatricesSf = {
    { C2Color::MATRIX_UNSPECIFIED,     ColorAspects::MatrixUnspecified },
    { C2Color::MATRIX_BT709,           ColorAspects::MatrixBT709_5 },
    { C2Color::MATRIX_FCC47_73_682,    ColorAspects::MatrixBT470_6M },
    { C2Color::MATRIX_BT601,           ColorAspects::MatrixBT601_6 },
    { C2Color::MATRIX_240M,       ColorAspects::MatrixSMPTE240M },
    { C2Color::MATRIX_BT2020,          ColorAspects::MatrixBT2020 },
    { C2Color::MATRIX_BT2020_CONSTANT, ColorAspects::MatrixBT2020Constant },
    { C2Color::MATRIX_OTHER,           ColorAspects::MatrixOther },
};

ALookup<C2Color::primaries_t, ColorAspects::Primaries> sColorPrimariesSf = {
    { C2Color::PRIMARIES_UNSPECIFIED,  ColorAspects::PrimariesUnspecified },
    { C2Color::PRIMARIES_BT709,        ColorAspects::PrimariesBT709_5 },
    { C2Color::PRIMARIES_BT470_M,      ColorAspects::PrimariesBT470_6M },
    { C2Color::PRIMARIES_BT601_625,    ColorAspects::PrimariesBT601_6_625 },
    { C2Color::PRIMARIES_BT601_525,    ColorAspects::PrimariesBT601_6_525 },
    { C2Color::PRIMARIES_GENERIC_FILM, ColorAspects::PrimariesGenericFilm },
    { C2Color::PRIMARIES_BT2020,       ColorAspects::PrimariesBT2020 },
    { C2Color::PRIMARIES_RP431,        ColorAspects::PrimariesRP431 },
    { C2Color::PRIMARIES_EG432,        ColorAspects::PrimariesEG432 },
//    { C2Color::PRIMARIES_EBU3213,      ColorAspects::Primaries... },
    { C2Color::PRIMARIES_OTHER,        ColorAspects::PrimariesOther },
};

ALookup<C2Color::range_t, int32_t> sColorRanges = {
    { C2Color::RANGE_FULL,    COLOR_RANGE_FULL },
    { C2Color::RANGE_LIMITED, COLOR_RANGE_LIMITED },
};

ALookup<C2Color::range_t, ColorAspects::Range> sColorRangesSf = {
    { C2Color::RANGE_UNSPECIFIED, ColorAspects::RangeUnspecified },
    { C2Color::RANGE_FULL,        ColorAspects::RangeFull },
    { C2Color::RANGE_LIMITED,     ColorAspects::RangeLimited },
    { C2Color::RANGE_OTHER,       ColorAspects::RangeOther },
};

ALookup<C2Color::transfer_t, int32_t> sColorTransfers = {
    { C2Color::TRANSFER_LINEAR, COLOR_TRANSFER_LINEAR },
    { C2Color::TRANSFER_170M,   COLOR_TRANSFER_SDR_VIDEO },
    { C2Color::TRANSFER_ST2084, COLOR_TRANSFER_ST2084 },
    { C2Color::TRANSFER_HLG,    COLOR_TRANSFER_HLG },
};

ALookup<C2Color::transfer_t, ColorAspects::Transfer> sColorTransfersSf = {
    { C2Color::TRANSFER_UNSPECIFIED, ColorAspects::TransferUnspecified },
    { C2Color::TRANSFER_LINEAR,      ColorAspects::TransferLinear },
    { C2Color::TRANSFER_SRGB,        ColorAspects::TransferSRGB },
    { C2Color::TRANSFER_170M,        ColorAspects::TransferSMPTE170M },
    { C2Color::TRANSFER_GAMMA22,     ColorAspects::TransferGamma22 },
    { C2Color::TRANSFER_GAMMA28,     ColorAspects::TransferGamma28 },
    { C2Color::TRANSFER_ST2084,      ColorAspects::TransferST2084 },
    { C2Color::TRANSFER_HLG,         ColorAspects::TransferHLG },
    { C2Color::TRANSFER_240M,        ColorAspects::TransferSMPTE240M },
    { C2Color::TRANSFER_XVYCC,       ColorAspects::TransferXvYCC },
    { C2Color::TRANSFER_BT1361,      ColorAspects::TransferBT1361 },
    { C2Color::TRANSFER_ST428,       ColorAspects::TransferST428 },
    { C2Color::TRANSFER_OTHER,       ColorAspects::TransferOther },
};

ALookup<C2Config::level_t, int32_t> sDolbyVisionLevels = {
    { C2Config::LEVEL_DV_MAIN_HD_24,  DolbyVisionLevelHd24 },
    { C2Config::LEVEL_DV_MAIN_HD_30,  DolbyVisionLevelHd30 },
    { C2Config::LEVEL_DV_MAIN_FHD_24, DolbyVisionLevelFhd24 },
    { C2Config::LEVEL_DV_MAIN_FHD_30, DolbyVisionLevelFhd30 },
    { C2Config::LEVEL_DV_MAIN_FHD_60, DolbyVisionLevelFhd60 },
    { C2Config::LEVEL_DV_MAIN_UHD_24, DolbyVisionLevelUhd24 },
    { C2Config::LEVEL_DV_MAIN_UHD_30, DolbyVisionLevelUhd30 },
    { C2Config::LEVEL_DV_MAIN_UHD_48, DolbyVisionLevelUhd48 },
    { C2Config::LEVEL_DV_MAIN_UHD_60, DolbyVisionLevelUhd60 },
    { C2Config::LEVEL_DV_MAIN_UHD_120, DolbyVisionLevelUhd120 },
    { C2Config::LEVEL_DV_MAIN_8K_30,  DolbyVisionLevel8k30 },
    { C2Config::LEVEL_DV_MAIN_8K_60,  DolbyVisionLevel8k60 },

    // high tiers are not yet supported on android, for now map them to main tier
    { C2Config::LEVEL_DV_HIGH_HD_24,  DolbyVisionLevelHd24 },
    { C2Config::LEVEL_DV_HIGH_HD_30,  DolbyVisionLevelHd30 },
    { C2Config::LEVEL_DV_HIGH_FHD_24, DolbyVisionLevelFhd24 },
    { C2Config::LEVEL_DV_HIGH_FHD_30, DolbyVisionLevelFhd30 },
    { C2Config::LEVEL_DV_HIGH_FHD_60, DolbyVisionLevelFhd60 },
    { C2Config::LEVEL_DV_HIGH_UHD_24, DolbyVisionLevelUhd24 },
    { C2Config::LEVEL_DV_HIGH_UHD_30, DolbyVisionLevelUhd30 },
    { C2Config::LEVEL_DV_HIGH_UHD_48, DolbyVisionLevelUhd48 },
    { C2Config::LEVEL_DV_HIGH_UHD_60, DolbyVisionLevelUhd60 },
    { C2Config::LEVEL_DV_HIGH_UHD_120, DolbyVisionLevelUhd120 },
    { C2Config::LEVEL_DV_HIGH_8K_30,  DolbyVisionLevel8k30 },
    { C2Config::LEVEL_DV_HIGH_8K_60,  DolbyVisionLevel8k60 },
};

ALookup<C2Config::profile_t, int32_t> sDolbyVisionProfiles = {
    { C2Config::PROFILE_DV_AV_PER, DolbyVisionProfileDvavPer },
    { C2Config::PROFILE_DV_AV_PEN, DolbyVisionProfileDvavPen },
    { C2Config::PROFILE_DV_HE_DER, DolbyVisionProfileDvheDer },
    { C2Config::PROFILE_DV_HE_DEN, DolbyVisionProfileDvheDen },
    { C2Config::PROFILE_DV_HE_04, DolbyVisionProfileDvheDtr },
    { C2Config::PROFILE_DV_HE_05, DolbyVisionProfileDvheStn },
    { C2Config::PROFILE_DV_HE_DTH, DolbyVisionProfileDvheDth },
    { C2Config::PROFILE_DV_HE_07, DolbyVisionProfileDvheDtb },
    { C2Config::PROFILE_DV_HE_08, DolbyVisionProfileDvheSt },
    { C2Config::PROFILE_DV_AV_09, DolbyVisionProfileDvavSe },
    { C2Config::PROFILE_DV_AV1_10, DolbyVisionProfileDvav110 },
};

ALookup<C2Config::level_t, int32_t> sH263Levels = {
    { C2Config::LEVEL_H263_10, H263Level10 },
    { C2Config::LEVEL_H263_20, H263Level20 },
    { C2Config::LEVEL_H263_30, H263Level30 },
    { C2Config::LEVEL_H263_40, H263Level40 },
    { C2Config::LEVEL_H263_45, H263Level45 },
    { C2Config::LEVEL_H263_50, H263Level50 },
    { C2Config::LEVEL_H263_60, H263Level60 },
    { C2Config::LEVEL_H263_70, H263Level70 },
};

ALookup<C2Config::profile_t, int32_t> sH263Profiles = {
    { C2Config::PROFILE_H263_BASELINE,          H263ProfileBaseline },
    { C2Config::PROFILE_H263_H320,              H263ProfileH320Coding },
    { C2Config::PROFILE_H263_V1BC,              H263ProfileBackwardCompatible },
    { C2Config::PROFILE_H263_ISWV2,             H263ProfileISWV2 },
    { C2Config::PROFILE_H263_ISWV3,             H263ProfileISWV3 },
    { C2Config::PROFILE_H263_HIGH_COMPRESSION,  H263ProfileHighCompression },
    { C2Config::PROFILE_H263_INTERNET,          H263ProfileInternet },
    { C2Config::PROFILE_H263_INTERLACE,         H263ProfileInterlace },
    { C2Config::PROFILE_H263_HIGH_LATENCY,      H263ProfileHighLatency },
};

ALookup<C2Config::level_t, int32_t> sHevcLevels = {
    { C2Config::LEVEL_HEVC_MAIN_1,      HEVCMainTierLevel1 },
    { C2Config::LEVEL_HEVC_MAIN_2,      HEVCMainTierLevel2 },
    { C2Config::LEVEL_HEVC_MAIN_2_1,    HEVCMainTierLevel21 },
    { C2Config::LEVEL_HEVC_MAIN_3,      HEVCMainTierLevel3 },
    { C2Config::LEVEL_HEVC_MAIN_3_1,    HEVCMainTierLevel31 },
    { C2Config::LEVEL_HEVC_MAIN_4,      HEVCMainTierLevel4 },
    { C2Config::LEVEL_HEVC_MAIN_4_1,    HEVCMainTierLevel41 },
    { C2Config::LEVEL_HEVC_MAIN_5,      HEVCMainTierLevel5 },
    { C2Config::LEVEL_HEVC_MAIN_5_1,    HEVCMainTierLevel51 },
    { C2Config::LEVEL_HEVC_MAIN_5_2,    HEVCMainTierLevel52 },
    { C2Config::LEVEL_HEVC_MAIN_6,      HEVCMainTierLevel6 },
    { C2Config::LEVEL_HEVC_MAIN_6_1,    HEVCMainTierLevel61 },
    { C2Config::LEVEL_HEVC_MAIN_6_2,    HEVCMainTierLevel62 },

    { C2Config::LEVEL_HEVC_HIGH_4,      HEVCHighTierLevel4 },
    { C2Config::LEVEL_HEVC_HIGH_4_1,    HEVCHighTierLevel41 },
    { C2Config::LEVEL_HEVC_HIGH_5,      HEVCHighTierLevel5 },
    { C2Config::LEVEL_HEVC_HIGH_5_1,    HEVCHighTierLevel51 },
    { C2Config::LEVEL_HEVC_HIGH_5_2,    HEVCHighTierLevel52 },
    { C2Config::LEVEL_HEVC_HIGH_6,      HEVCHighTierLevel6 },
    { C2Config::LEVEL_HEVC_HIGH_6_1,    HEVCHighTierLevel61 },
    { C2Config::LEVEL_HEVC_HIGH_6_2,    HEVCHighTierLevel62 },

    // map high tier levels below 4 to main tier
    { C2Config::LEVEL_HEVC_MAIN_1,      HEVCHighTierLevel1 },
    { C2Config::LEVEL_HEVC_MAIN_2,      HEVCHighTierLevel2 },
    { C2Config::LEVEL_HEVC_MAIN_2_1,    HEVCHighTierLevel21 },
    { C2Config::LEVEL_HEVC_MAIN_3,      HEVCHighTierLevel3 },
    { C2Config::LEVEL_HEVC_MAIN_3_1,    HEVCHighTierLevel31 },
};

ALookup<C2Config::profile_t, int32_t> sHevcProfiles = {
    { C2Config::PROFILE_HEVC_MAIN, HEVCProfileMain },
    { C2Config::PROFILE_HEVC_MAIN_10, HEVCProfileMain10 },
    { C2Config::PROFILE_HEVC_MAIN_STILL, HEVCProfileMainStill },
    { C2Config::PROFILE_HEVC_MAIN_INTRA, HEVCProfileMain },
    { C2Config::PROFILE_HEVC_MAIN_10_INTRA, HEVCProfileMain10 },
    { C2Config::PROFILE_HEVC_MAIN_10, HEVCProfileMain10HDR10 },
    { C2Config::PROFILE_HEVC_MAIN_10, HEVCProfileMain10HDR10Plus },
};

ALookup<C2Config::profile_t, int32_t> sHevcHdrProfiles = {
    { C2Config::PROFILE_HEVC_MAIN_10, HEVCProfileMain10HDR10 },
};

ALookup<C2Config::profile_t, int32_t> sHevcHdr10PlusProfiles = {
    { C2Config::PROFILE_HEVC_MAIN_10, HEVCProfileMain10HDR10Plus },
};

ALookup<C2Config::hdr_format_t, int32_t> sHevcHdrFormats = {
    { C2Config::hdr_format_t::SDR, HEVCProfileMain },
    { C2Config::hdr_format_t::HLG, HEVCProfileMain10 },
    { C2Config::hdr_format_t::HDR10, HEVCProfileMain10HDR10 },
    { C2Config::hdr_format_t::HDR10_PLUS, HEVCProfileMain10HDR10Plus },
};

ALookup<C2Config::level_t, int32_t> sMpeg2Levels = {
    { C2Config::LEVEL_MP2V_LOW,         MPEG2LevelLL },
    { C2Config::LEVEL_MP2V_MAIN,        MPEG2LevelML },
    { C2Config::LEVEL_MP2V_HIGH_1440,   MPEG2LevelH14 },
    { C2Config::LEVEL_MP2V_HIGH,        MPEG2LevelHL },
    { C2Config::LEVEL_MP2V_HIGHP,       MPEG2LevelHP },
};

ALookup<C2Config::profile_t, int32_t> sMpeg2Profiles = {
    { C2Config::PROFILE_MP2V_SIMPLE,                MPEG2ProfileSimple },
    { C2Config::PROFILE_MP2V_MAIN,                  MPEG2ProfileMain },
    { C2Config::PROFILE_MP2V_SNR_SCALABLE,          MPEG2ProfileSNR },
    { C2Config::PROFILE_MP2V_SPATIALLY_SCALABLE,    MPEG2ProfileSpatial },
    { C2Config::PROFILE_MP2V_HIGH,                  MPEG2ProfileHigh },
    { C2Config::PROFILE_MP2V_422,                   MPEG2Profile422 },
};

ALookup<C2Config::level_t, int32_t> sMpeg4Levels = {
    { C2Config::LEVEL_MP4V_0,   MPEG4Level0 },
    { C2Config::LEVEL_MP4V_0B,  MPEG4Level0b },
    { C2Config::LEVEL_MP4V_1,   MPEG4Level1 },
    { C2Config::LEVEL_MP4V_2,   MPEG4Level2 },
    { C2Config::LEVEL_MP4V_3,   MPEG4Level3 },
    { C2Config::LEVEL_MP4V_3B,  MPEG4Level3b },
    { C2Config::LEVEL_MP4V_4,   MPEG4Level4 },
    { C2Config::LEVEL_MP4V_4A,  MPEG4Level4a },
    { C2Config::LEVEL_MP4V_5,   MPEG4Level5 },
    { C2Config::LEVEL_MP4V_6,   MPEG4Level6 },
};

ALookup<C2Config::profile_t, int32_t> sMpeg4Profiles = {
    { C2Config::PROFILE_MP4V_SIMPLE,            MPEG4ProfileSimple },
    { C2Config::PROFILE_MP4V_SIMPLE_SCALABLE,   MPEG4ProfileSimpleScalable },
    { C2Config::PROFILE_MP4V_CORE,              MPEG4ProfileCore },
    { C2Config::PROFILE_MP4V_MAIN,              MPEG4ProfileMain },
    { C2Config::PROFILE_MP4V_NBIT,              MPEG4ProfileNbit },
    { C2Config::PROFILE_MP4V_ARTS,              MPEG4ProfileAdvancedRealTime },
    { C2Config::PROFILE_MP4V_CORE_SCALABLE,     MPEG4ProfileCoreScalable },
    { C2Config::PROFILE_MP4V_ACE,               MPEG4ProfileAdvancedCoding },
    { C2Config::PROFILE_MP4V_ADVANCED_CORE,     MPEG4ProfileAdvancedCore },
    { C2Config::PROFILE_MP4V_ADVANCED_SIMPLE,   MPEG4ProfileAdvancedSimple },
};

ALookup<C2Config::pcm_encoding_t, int32_t> sPcmEncodings = {
    { C2Config::PCM_8, kAudioEncodingPcm8bit },
    { C2Config::PCM_16, kAudioEncodingPcm16bit },
    { C2Config::PCM_FLOAT, kAudioEncodingPcmFloat },
    { C2Config::PCM_24, kAudioEncodingPcm24bitPacked },
    { C2Config::PCM_32, kAudioEncodingPcm32bit },
};

ALookup<C2Config::level_t, int32_t> sVp9Levels = {
    { C2Config::LEVEL_VP9_1,    VP9Level1 },
    { C2Config::LEVEL_VP9_1_1,  VP9Level11 },
    { C2Config::LEVEL_VP9_2,    VP9Level2 },
    { C2Config::LEVEL_VP9_2_1,  VP9Level21 },
    { C2Config::LEVEL_VP9_3,    VP9Level3 },
    { C2Config::LEVEL_VP9_3_1,  VP9Level31 },
    { C2Config::LEVEL_VP9_4,    VP9Level4 },
    { C2Config::LEVEL_VP9_4_1,  VP9Level41 },
    { C2Config::LEVEL_VP9_5,    VP9Level5 },
    { C2Config::LEVEL_VP9_5_1,  VP9Level51 },
    { C2Config::LEVEL_VP9_5_2,  VP9Level52 },
    { C2Config::LEVEL_VP9_6,    VP9Level6 },
    { C2Config::LEVEL_VP9_6_1,  VP9Level61 },
    { C2Config::LEVEL_VP9_6_2,  VP9Level62 },
};

ALookup<C2Config::profile_t, int32_t> sVp9Profiles = {
    { C2Config::PROFILE_VP9_0, VP9Profile0 },
    { C2Config::PROFILE_VP9_1, VP9Profile1 },
    { C2Config::PROFILE_VP9_2, VP9Profile2 },
    { C2Config::PROFILE_VP9_3, VP9Profile3 },
    { C2Config::PROFILE_VP9_2, VP9Profile2HDR },
    { C2Config::PROFILE_VP9_3, VP9Profile3HDR },
    { C2Config::PROFILE_VP9_2, VP9Profile2HDR10Plus },
    { C2Config::PROFILE_VP9_3, VP9Profile3HDR10Plus },
};

ALookup<C2Config::profile_t, int32_t> sVp9HdrProfiles = {
    { C2Config::PROFILE_VP9_2, VP9Profile2HDR },
    { C2Config::PROFILE_VP9_3, VP9Profile3HDR },
};

ALookup<C2Config::profile_t, int32_t> sVp9Hdr10PlusProfiles = {
    { C2Config::PROFILE_VP9_2, VP9Profile2HDR10Plus },
    { C2Config::PROFILE_VP9_3, VP9Profile3HDR10Plus },
};

ALookup<C2Config::hdr_format_t, int32_t> sVp9HdrFormats = {
    { C2Config::hdr_format_t::SDR, VP9Profile0 },
    { C2Config::hdr_format_t::SDR, VP9Profile1 },
    { C2Config::hdr_format_t::HLG, VP9Profile2 },
    { C2Config::hdr_format_t::HLG, VP9Profile3 },
    { C2Config::hdr_format_t::HDR10, VP9Profile2HDR },
    { C2Config::hdr_format_t::HDR10, VP9Profile3HDR },
    { C2Config::hdr_format_t::HDR10_PLUS, VP9Profile2HDR10Plus },
    { C2Config::hdr_format_t::HDR10_PLUS, VP9Profile3HDR10Plus },
};

ALookup<C2Config::level_t, int32_t> sAv1Levels = {
    { C2Config::LEVEL_AV1_2,    AV1Level2  },
    { C2Config::LEVEL_AV1_2_1,  AV1Level21 },
    { C2Config::LEVEL_AV1_2_2,  AV1Level22 },
    { C2Config::LEVEL_AV1_2_3,  AV1Level23 },
    { C2Config::LEVEL_AV1_3,    AV1Level3  },
    { C2Config::LEVEL_AV1_3_1,  AV1Level31 },
    { C2Config::LEVEL_AV1_3_2,  AV1Level32 },
    { C2Config::LEVEL_AV1_3_3,  AV1Level33 },
    { C2Config::LEVEL_AV1_4,    AV1Level4  },
    { C2Config::LEVEL_AV1_4_1,  AV1Level41 },
    { C2Config::LEVEL_AV1_4_2,  AV1Level42 },
    { C2Config::LEVEL_AV1_4_3,  AV1Level43 },
    { C2Config::LEVEL_AV1_5,    AV1Level5  },
    { C2Config::LEVEL_AV1_5_1,  AV1Level51 },
    { C2Config::LEVEL_AV1_5_2,  AV1Level52 },
    { C2Config::LEVEL_AV1_5_3,  AV1Level53 },
    { C2Config::LEVEL_AV1_6,    AV1Level6  },
    { C2Config::LEVEL_AV1_6_1,  AV1Level61 },
    { C2Config::LEVEL_AV1_6_2,  AV1Level62 },
    { C2Config::LEVEL_AV1_6_3,  AV1Level63 },
    { C2Config::LEVEL_AV1_7,    AV1Level7  },
    { C2Config::LEVEL_AV1_7_1,  AV1Level71 },
    { C2Config::LEVEL_AV1_7_2,  AV1Level72 },
    { C2Config::LEVEL_AV1_7_3,  AV1Level73 },
};

ALookup<C2Config::profile_t, int32_t> sAv1Profiles = {
    { C2Config::PROFILE_AV1_0, AV1ProfileMain8 },
    { C2Config::PROFILE_AV1_0, AV1ProfileMain10 },
    { C2Config::PROFILE_AV1_0, AV1ProfileMain10HDR10 },
    { C2Config::PROFILE_AV1_0, AV1ProfileMain10HDR10Plus },
};

ALookup<C2Config::profile_t, int32_t> sAv1TenbitProfiles = {
    { C2Config::PROFILE_AV1_0, AV1ProfileMain10 },
};

ALookup<C2Config::profile_t, int32_t> sAv1HdrProfiles = {
    { C2Config::PROFILE_AV1_0, AV1ProfileMain10HDR10 },
};

ALookup<C2Config::profile_t, int32_t> sAv1Hdr10PlusProfiles = {
    { C2Config::PROFILE_AV1_0, AV1ProfileMain10HDR10Plus },
};

ALookup<C2Config::hdr_format_t, int32_t> sAv1HdrFormats = {
    { C2Config::hdr_format_t::SDR, AV1ProfileMain8 },
    { C2Config::hdr_format_t::HLG, AV1ProfileMain10 },
    { C2Config::hdr_format_t::HDR10, AV1ProfileMain10HDR10 },
    { C2Config::hdr_format_t::HDR10_PLUS, AV1ProfileMain10HDR10Plus },
};

// APV
ALookup<C2Config::profile_t, int32_t> sApvProfiles = {
    { C2Config::PROFILE_APV_422_10, APVProfile422_10 },
    { C2Config::PROFILE_APV_422_10, APVProfile422_10HDR10 },
    { C2Config::PROFILE_APV_422_10, APVProfile422_10HDR10Plus },
};

ALookup<C2Config::profile_t, int32_t> sApvHdrProfiles = {
    { C2Config::PROFILE_APV_422_10, APVProfile422_10HDR10 },
};

ALookup<C2Config::profile_t, int32_t> sApvHdr10PlusProfiles = {
    { C2Config::PROFILE_APV_422_10, APVProfile422_10HDR10Plus },
};

ALookup<C2Config::level_t, int32_t> sApvLevels = {
    { C2Config::LEVEL_APV_1_BAND_0, APVLevel1Band0 },
    { C2Config::LEVEL_APV_1_BAND_1, APVLevel1Band1 },
    { C2Config::LEVEL_APV_1_BAND_2, APVLevel1Band2 },
    { C2Config::LEVEL_APV_1_BAND_3, APVLevel1Band3 },
    { C2Config::LEVEL_APV_1_1_BAND_0, APVLevel11Band0 },
    { C2Config::LEVEL_APV_1_1_BAND_1, APVLevel11Band1 },
    { C2Config::LEVEL_APV_1_1_BAND_2, APVLevel11Band2 },
    { C2Config::LEVEL_APV_1_1_BAND_3, APVLevel11Band3 },
    { C2Config::LEVEL_APV_2_BAND_0, APVLevel2Band0 },
    { C2Config::LEVEL_APV_2_BAND_1, APVLevel2Band1 },
    { C2Config::LEVEL_APV_2_BAND_2, APVLevel2Band2 },
    { C2Config::LEVEL_APV_2_BAND_3, APVLevel2Band3 },
    { C2Config::LEVEL_APV_2_1_BAND_0, APVLevel21Band0 },
    { C2Config::LEVEL_APV_2_1_BAND_1, APVLevel21Band1 },
    { C2Config::LEVEL_APV_2_1_BAND_2, APVLevel21Band2 },
    { C2Config::LEVEL_APV_2_1_BAND_3, APVLevel21Band3 },
    { C2Config::LEVEL_APV_3_BAND_0, APVLevel3Band0 },
    { C2Config::LEVEL_APV_3_BAND_1, APVLevel3Band1 },
    { C2Config::LEVEL_APV_3_BAND_2, APVLevel3Band2 },
    { C2Config::LEVEL_APV_3_BAND_3, APVLevel3Band3 },
    { C2Config::LEVEL_APV_3_1_BAND_0, APVLevel31Band0 },
    { C2Config::LEVEL_APV_3_1_BAND_1, APVLevel31Band1 },
    { C2Config::LEVEL_APV_3_1_BAND_2, APVLevel31Band2 },
    { C2Config::LEVEL_APV_3_1_BAND_3, APVLevel31Band3 },
    { C2Config::LEVEL_APV_4_BAND_0, APVLevel4Band0 },
    { C2Config::LEVEL_APV_4_BAND_1, APVLevel4Band1 },
    { C2Config::LEVEL_APV_4_BAND_2, APVLevel4Band2 },
    { C2Config::LEVEL_APV_4_BAND_3, APVLevel4Band3 },
    { C2Config::LEVEL_APV_4_1_BAND_0, APVLevel41Band0 },
    { C2Config::LEVEL_APV_4_1_BAND_1, APVLevel41Band1 },
    { C2Config::LEVEL_APV_4_1_BAND_2, APVLevel41Band2 },
    { C2Config::LEVEL_APV_4_1_BAND_3, APVLevel41Band3 },
    { C2Config::LEVEL_APV_5_BAND_0, APVLevel5Band0 },
    { C2Config::LEVEL_APV_5_BAND_1, APVLevel5Band1 },
    { C2Config::LEVEL_APV_5_BAND_2, APVLevel5Band2 },
    { C2Config::LEVEL_APV_5_BAND_3, APVLevel5Band3 },
    { C2Config::LEVEL_APV_5_1_BAND_0, APVLevel51Band0 },
    { C2Config::LEVEL_APV_5_1_BAND_1, APVLevel51Band1 },
    { C2Config::LEVEL_APV_5_1_BAND_2, APVLevel51Band2 },
    { C2Config::LEVEL_APV_5_1_BAND_3, APVLevel51Band3 },
    { C2Config::LEVEL_APV_6_BAND_0, APVLevel6Band0 },
    { C2Config::LEVEL_APV_6_BAND_1, APVLevel6Band1 },
    { C2Config::LEVEL_APV_6_BAND_2, APVLevel6Band2 },
    { C2Config::LEVEL_APV_6_BAND_3, APVLevel6Band3 },
    { C2Config::LEVEL_APV_6_1_BAND_0, APVLevel61Band0 },
    { C2Config::LEVEL_APV_6_1_BAND_1, APVLevel61Band1 },
    { C2Config::LEVEL_APV_6_1_BAND_2, APVLevel61Band2 },
    { C2Config::LEVEL_APV_6_1_BAND_3, APVLevel61Band3 },
    { C2Config::LEVEL_APV_7_BAND_0, APVLevel7Band0 },
    { C2Config::LEVEL_APV_7_BAND_1, APVLevel7Band1 },
    { C2Config::LEVEL_APV_7_BAND_2, APVLevel7Band2 },
    { C2Config::LEVEL_APV_7_BAND_3, APVLevel7Band3 },
    { C2Config::LEVEL_APV_7_1_BAND_0, APVLevel71Band0 },
    { C2Config::LEVEL_APV_7_1_BAND_1, APVLevel71Band1 },
    { C2Config::LEVEL_APV_7_1_BAND_2, APVLevel71Band2 },
    { C2Config::LEVEL_APV_7_1_BAND_3, APVLevel71Band3 },
};

ALookup<C2Config::hdr_format_t, int32_t> sApvHdrFormats = {
    { C2Config::hdr_format_t::HLG, APVProfile422_10 },
    { C2Config::hdr_format_t::HDR10, APVProfile422_10HDR10 },
    { C2Config::hdr_format_t::HDR10_PLUS, APVProfile422_10HDR10Plus },
};

// HAL_PIXEL_FORMAT_* -> COLOR_Format*
ALookup<uint32_t, int32_t> sPixelFormats = {
    { HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, COLOR_FormatSurface },

    // YCBCR_420_888 maps to YUV420Flexible and vice versa
    { HAL_PIXEL_FORMAT_YCBCR_420_888,          COLOR_FormatYUV420Flexible },

    // Fallback matches for YCBCR_420_888
    { HAL_PIXEL_FORMAT_YCBCR_420_888,          COLOR_FormatYUV420Planar },
    { HAL_PIXEL_FORMAT_YCBCR_420_888,          COLOR_FormatYUV420SemiPlanar },
    { HAL_PIXEL_FORMAT_YCBCR_420_888,          COLOR_FormatYUV420PackedPlanar },
    { HAL_PIXEL_FORMAT_YCBCR_420_888,          COLOR_FormatYUV420PackedSemiPlanar },

    // Fallback matches for YUV420Flexible
    { HAL_PIXEL_FORMAT_YCRCB_420_SP,           COLOR_FormatYUV420Flexible },
    { HAL_PIXEL_FORMAT_YV12,                   COLOR_FormatYUV420Flexible },

    { HAL_PIXEL_FORMAT_YCBCR_422_SP,           COLOR_FormatYUV422PackedSemiPlanar },
    { HAL_PIXEL_FORMAT_YCBCR_422_I,            COLOR_FormatYUV422PackedPlanar },
    { HAL_PIXEL_FORMAT_YCBCR_P010,             COLOR_FormatYUVP010 },
    { HAL_PIXEL_FORMAT_RGBA_1010102,           COLOR_Format32bitABGR2101010 },
    { HAL_PIXEL_FORMAT_RGBA_FP16,              COLOR_Format64bitABGRFloat },
};

ALookup<C2Config::picture_type_t, int32_t> sPictureType = {
    { C2Config::picture_type_t::SYNC_FRAME,     PICTURE_TYPE_I },
    { C2Config::picture_type_t::I_FRAME,        PICTURE_TYPE_I },
    { C2Config::picture_type_t::P_FRAME,        PICTURE_TYPE_P },
    { C2Config::picture_type_t::B_FRAME,        PICTURE_TYPE_B },
};

ALookup<C2Config::profile_t, int32_t> sAc4Profiles = {
    { C2Config::PROFILE_AC4_0_0, AC4Profile00 },
    { C2Config::PROFILE_AC4_1_0, AC4Profile10 },
    { C2Config::PROFILE_AC4_1_1, AC4Profile11 },
    { C2Config::PROFILE_AC4_2_1, AC4Profile21 },
    { C2Config::PROFILE_AC4_2_2, AC4Profile22 },
};

ALookup<C2Config::level_t, int32_t> sAc4Levels = {
    { C2Config::LEVEL_AC4_0, AC4Level0 },
    { C2Config::LEVEL_AC4_1, AC4Level1 },
    { C2Config::LEVEL_AC4_2, AC4Level2 },
    { C2Config::LEVEL_AC4_3, AC4Level3 },
    { C2Config::LEVEL_AC4_4, AC4Level4 },
};

/**
 * A helper that passes through vendor extension profile and level values.
 */
struct ProfileLevelMapperHelper : C2Mapper::ProfileLevelMapper {
    virtual bool simpleMap(C2Config::level_t from, int32_t *to) = 0;
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) = 0;
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) = 0;
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) = 0;

    template<typename T, typename U>
    bool passThroughMap(T from, U *to) {
        // allow (and pass through) vendor extensions
        if (from >= (T)C2_PROFILE_LEVEL_VENDOR_START && from < (T)INT32_MAX) {
            *to = (U)from;
            return true;
        }
        return simpleMap(from, to);
    }

    virtual bool mapLevel(C2Config::level_t from, int32_t *to) {
        return passThroughMap(from, to);
    }

    virtual bool mapLevel(int32_t from, C2Config::level_t *to) {
        return passThroughMap(from, to);
    }

    virtual bool mapProfile(C2Config::profile_t from, int32_t *to) {
        return passThroughMap(from, to);
    }

    virtual bool mapProfile(int32_t from, C2Config::profile_t *to) {
        return passThroughMap(from, to);
    }
};

// AAC only uses profiles, map all levels to unused or 0
struct AacProfileLevelMapper : ProfileLevelMapperHelper {
    virtual bool simpleMap(C2Config::level_t, int32_t *to) {
        *to = 0;
        return true;
    }
    virtual bool simpleMap(int32_t, C2Config::level_t *to) {
        *to = C2Config::LEVEL_UNUSED;
        return true;
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return sAacProfiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return sAacProfiles.map(from, to);
    }
    // AAC does not have HDR format
    virtual bool mapHdrFormat(int32_t, C2Config::hdr_format_t*) override {
        return false;
    }
};

struct AvcProfileLevelMapper : ProfileLevelMapperHelper {
    virtual bool simpleMap(C2Config::level_t from, int32_t *to) {
        return sAvcLevels.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) {
        return sAvcLevels.map(from, to);
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return sAvcProfiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return sAvcProfiles.map(from, to);
    }
};

struct DolbyVisionProfileLevelMapper : ProfileLevelMapperHelper {
    virtual bool simpleMap(C2Config::level_t from, int32_t *to) {
        return sDolbyVisionLevels.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) {
        return sDolbyVisionLevels.map(from, to);
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return sDolbyVisionProfiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return sDolbyVisionProfiles.map(from, to);
    }
    // Dolby Vision is always HDR and the profile is fully expressive so use unknown
    // HDR format
    virtual bool mapHdrFormat(int32_t, C2Config::hdr_format_t *to) override {
        *to = C2Config::hdr_format_t::UNKNOWN;
        return true;
    }
};

struct H263ProfileLevelMapper : ProfileLevelMapperHelper {
    virtual bool simpleMap(C2Config::level_t from, int32_t *to) {
        return sH263Levels.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) {
        return sH263Levels.map(from, to);
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return sH263Profiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return sH263Profiles.map(from, to);
    }
};

struct HevcProfileLevelMapper : ProfileLevelMapperHelper {
    HevcProfileLevelMapper(bool isHdr = false, bool isHdr10Plus = false) :
        ProfileLevelMapperHelper(),
        mIsHdr(isHdr), mIsHdr10Plus(isHdr10Plus) {}

    virtual bool simpleMap(C2Config::level_t from, int32_t *to) {
        return sHevcLevels.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) {
        return sHevcLevels.map(from, to);
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return mIsHdr10Plus ? sHevcHdr10PlusProfiles.map(from, to) :
                     mIsHdr ? sHevcHdrProfiles.map(from, to) :
                              sHevcProfiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return mIsHdr10Plus ? sHevcHdr10PlusProfiles.map(from, to) :
                     mIsHdr ? sHevcHdrProfiles.map(from, to) :
                              sHevcProfiles.map(from, to);
    }
    virtual bool mapHdrFormat(int32_t from, C2Config::hdr_format_t *to) override {
        return sHevcHdrFormats.map(from, to);
    }

private:
    bool mIsHdr;
    bool mIsHdr10Plus;
};

struct Mpeg2ProfileLevelMapper : ProfileLevelMapperHelper {
    virtual bool simpleMap(C2Config::level_t from, int32_t *to) {
        return sMpeg2Levels.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) {
        return sMpeg2Levels.map(from, to);
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return sMpeg2Profiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return sMpeg2Profiles.map(from, to);
    }
};

struct Mpeg4ProfileLevelMapper : ProfileLevelMapperHelper {
    virtual bool simpleMap(C2Config::level_t from, int32_t *to) {
        return sMpeg4Levels.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) {
        return sMpeg4Levels.map(from, to);
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return sMpeg4Profiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return sMpeg4Profiles.map(from, to);
    }
};

// VP8 has no profiles and levels in Codec 2.0, but we use main profile and level 0 in MediaCodec
// map all profiles and levels to that.
struct Vp8ProfileLevelMapper : ProfileLevelMapperHelper {
    virtual bool simpleMap(C2Config::level_t, int32_t *to) {
        *to = VP8Level_Version0;
        return true;
    }
    virtual bool simpleMap(int32_t, C2Config::level_t *to) {
        *to = C2Config::LEVEL_UNUSED;
        return true;
    }
    virtual bool simpleMap(C2Config::profile_t, int32_t *to) {
        *to = VP8ProfileMain;
        return true;
    }
    virtual bool simpleMap(int32_t, C2Config::profile_t *to) {
        *to = C2Config::PROFILE_UNUSED;
        return true;
    }
};

struct Vp9ProfileLevelMapper : ProfileLevelMapperHelper {
    Vp9ProfileLevelMapper(bool isHdr = false, bool isHdr10Plus = false) :
        ProfileLevelMapperHelper(),
        mIsHdr(isHdr), mIsHdr10Plus(isHdr10Plus) {}

    virtual bool simpleMap(C2Config::level_t from, int32_t *to) {
        return sVp9Levels.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) {
        return sVp9Levels.map(from, to);
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return mIsHdr10Plus ? sVp9Hdr10PlusProfiles.map(from, to) :
                     mIsHdr ? sVp9HdrProfiles.map(from, to) :
                              sVp9Profiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return mIsHdr10Plus ? sVp9Hdr10PlusProfiles.map(from, to) :
                     mIsHdr ? sVp9HdrProfiles.map(from, to) :
                              sVp9Profiles.map(from, to);
    }
    virtual bool mapHdrFormat(int32_t from, C2Config::hdr_format_t *to) override {
        return sVp9HdrFormats.map(from, to);
    }

private:
    bool mIsHdr;
    bool mIsHdr10Plus;
};

struct Av1ProfileLevelMapper : ProfileLevelMapperHelper {
    Av1ProfileLevelMapper(bool isHdr = false, bool isHdr10Plus = false, int32_t bitDepth = 8) :
        ProfileLevelMapperHelper(),
        mIsHdr(isHdr), mIsHdr10Plus(isHdr10Plus), mBitDepth(bitDepth) {}

    virtual bool simpleMap(C2Config::level_t from, int32_t *to) {
        return sAv1Levels.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) {
        return sAv1Levels.map(from, to);
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return (mBitDepth == 10) ? sAv1TenbitProfiles.map(from, to) :
                    mIsHdr10Plus ? sAv1Hdr10PlusProfiles.map(from, to) :
                          mIsHdr ? sAv1HdrProfiles.map(from, to) :
                                   sAv1Profiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return (mBitDepth == 10) ? sAv1TenbitProfiles.map(from, to) :
                    mIsHdr10Plus ? sAv1Hdr10PlusProfiles.map(from, to) :
                          mIsHdr ? sAv1HdrProfiles.map(from, to) :
                                   sAv1Profiles.map(from, to);
    }
    virtual bool mapHdrFormat(int32_t from, C2Config::hdr_format_t *to) override {
        return sAv1HdrFormats.map(from, to);
    }

private:
    bool mIsHdr;
    bool mIsHdr10Plus;
    int32_t mBitDepth;
};

// APV
struct ApvProfileLevelMapper : ProfileLevelMapperHelper {
    ApvProfileLevelMapper(bool isHdr = false, bool isHdr10Plus = false) :
        ProfileLevelMapperHelper(),
        mIsHdr(isHdr), mIsHdr10Plus(isHdr10Plus) {}

    virtual bool simpleMap(C2Config::level_t from, int32_t *to) {
        return sApvLevels.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) {
        return sApvLevels.map(from, to);
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return mIsHdr10Plus ? sApvHdr10PlusProfiles.map(from, to) :
                     mIsHdr ? sApvHdrProfiles.map(from, to) :
                              sApvProfiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return mIsHdr10Plus ? sApvHdr10PlusProfiles.map(from, to) :
                     mIsHdr ? sApvHdrProfiles.map(from, to) :
                              sApvProfiles.map(from, to);
    }
    virtual bool mapHdrFormat(int32_t from, C2Config::hdr_format_t *to) override {
        return sApvHdrFormats.map(from, to);
    }

private:
    bool mIsHdr;
    bool mIsHdr10Plus;
};

struct Ac4ProfileLevelMapper : ProfileLevelMapperHelper {
    virtual bool simpleMap(C2Config::level_t from, int32_t *to) {
        return sAc4Levels.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::level_t *to) {
        return sAc4Levels.map(from, to);
    }
    virtual bool simpleMap(C2Config::profile_t from, int32_t *to) {
        return sAc4Profiles.map(from, to);
    }
    virtual bool simpleMap(int32_t from, C2Config::profile_t *to) {
        return sAc4Profiles.map(from, to);
    }
};

} // namespace

// the default mapper is used for media types that do not support HDR
bool C2Mapper::ProfileLevelMapper::mapHdrFormat(int32_t, C2Config::hdr_format_t *to) {
    // by default map all (including vendor) profiles to SDR
    *to = C2Config::hdr_format_t::SDR;
    return true;
}

// static
std::shared_ptr<C2Mapper::ProfileLevelMapper>
C2Mapper::GetProfileLevelMapper(std::string mediaType) {
    std::transform(mediaType.begin(), mediaType.end(), mediaType.begin(), ::tolower);
    if (mediaType == MIMETYPE_AUDIO_AAC) {
        return std::make_shared<AacProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_VIDEO_AVC) {
        return std::make_shared<AvcProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_VIDEO_DOLBY_VISION) {
        return std::make_shared<DolbyVisionProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_VIDEO_H263) {
        return std::make_shared<H263ProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_VIDEO_HEVC) {
        return std::make_shared<HevcProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_VIDEO_MPEG2) {
        return std::make_shared<Mpeg2ProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_VIDEO_MPEG4) {
        return std::make_shared<Mpeg4ProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_VIDEO_VP8) {
        return std::make_shared<Vp8ProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_VIDEO_VP9) {
        return std::make_shared<Vp9ProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_VIDEO_AV1) {
        return std::make_shared<Av1ProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_VIDEO_APV) {
        return std::make_shared<ApvProfileLevelMapper>();
    } else if (mediaType == MIMETYPE_AUDIO_AC4) {
        return std::make_shared<Ac4ProfileLevelMapper>();
    }
    return nullptr;
}

// static
std::shared_ptr<C2Mapper::ProfileLevelMapper>
C2Mapper::GetHdrProfileLevelMapper(std::string mediaType, bool isHdr10Plus) {
    std::transform(mediaType.begin(), mediaType.end(), mediaType.begin(), ::tolower);
    if (mediaType == MIMETYPE_VIDEO_HEVC) {
        return std::make_shared<HevcProfileLevelMapper>(true, isHdr10Plus);
    } else if (mediaType == MIMETYPE_VIDEO_VP9) {
        return std::make_shared<Vp9ProfileLevelMapper>(true, isHdr10Plus);
    } else if (mediaType == MIMETYPE_VIDEO_AV1) {
        return std::make_shared<Av1ProfileLevelMapper>(true, isHdr10Plus);
    } else if (mediaType == MIMETYPE_VIDEO_APV) {
        return std::make_shared<ApvProfileLevelMapper>(true, isHdr10Plus);
    }
    return nullptr;
}

// static
std::shared_ptr<C2Mapper::ProfileLevelMapper>
C2Mapper::GetBitDepthProfileLevelMapper(std::string mediaType, int32_t bitDepth) {
    std::transform(mediaType.begin(), mediaType.end(), mediaType.begin(), ::tolower);
    if (bitDepth == 8) {
        return GetProfileLevelMapper(mediaType);
    } else if (mediaType == MIMETYPE_VIDEO_AV1 && bitDepth == 10) {
        return std::make_shared<Av1ProfileLevelMapper>(false, false, bitDepth);
    } else if (mediaType == MIMETYPE_VIDEO_APV) {
        return std::make_shared<ApvProfileLevelMapper>();
    }
    return nullptr;
}

// static
bool C2Mapper::map(C2Config::bitrate_mode_t from, int32_t *to) {
    return sBitrateModes.map(from, to);
}

// static
bool C2Mapper::map(int32_t from, C2Config::bitrate_mode_t *to) {
    return sBitrateModes.map(from, to);
}

// static
bool C2Mapper::map(C2Config::pcm_encoding_t from, int32_t *to) {
    return sPcmEncodings.map(from, to);
}

// static
bool C2Mapper::map(int32_t from, C2Config::pcm_encoding_t *to) {
    return sPcmEncodings.map(from, to);
}

// static
bool C2Mapper::map(C2Color::range_t from, int32_t *to) {
    bool res = true;
    // map SDK defined values directly. For other values, use wrapping from ColorUtils.
    if (!sColorRanges.map(from, to)) {
        ColorAspects::Range sfRange;

        // map known constants and keep vendor extensions. all other values are mapped to 'Other'
        if (!sColorRangesSf.map(from, &sfRange)) {
            // use static cast and ensure it is in the extension range
            if (from < C2Color::RANGE_VENDOR_START || from > C2Color::RANGE_OTHER) {
                sfRange = ColorAspects::RangeOther;
                res = false;
            }
        }

        *to = ColorUtils::wrapColorAspectsIntoColorRange(sfRange);
    }
    return res;
}

// static
bool C2Mapper::map(int32_t from, C2Color::range_t *to) {
    // map SDK defined values directly. For other values, use wrapping from ColorUtils.
    if (!sColorRanges.map(from, to)) {
        ColorAspects::Range sfRange;
        (void)ColorUtils::unwrapColorAspectsFromColorRange(from, &sfRange);

        // map known constants and keep vendor extensions. all other values are mapped to 'Other'
        if (!sColorRangesSf.map(sfRange, to)) {
            // use static cast and ensure it is in the extension range
            *to = (C2Color::range_t)sfRange;
            if (*to < C2Color::RANGE_VENDOR_START || *to > C2Color::RANGE_OTHER) {
                *to = C2Color::RANGE_OTHER;
                return false;
            }
        }
    }

    return true;
}

// static
bool C2Mapper::map(C2Color::range_t from, ColorAspects::Range *to) {
    return sColorRangesSf.map(from, to);
}

// static
bool C2Mapper::map(ColorAspects::Range from, C2Color::range_t *to) {
    return sColorRangesSf.map(from, to);
}

// static
bool C2Mapper::map(C2Color::primaries_t primaries, C2Color::matrix_t matrix, int32_t *standard) {
    ColorAspects::Primaries sfPrimaries;
    ColorAspects::MatrixCoeffs sfMatrix;
    bool res = true;

    // map known constants and keep vendor extensions. all other values are mapped to 'Other'
    if (!sColorPrimariesSf.map(primaries, &sfPrimaries)) {
        // ensure it is in the extension range and use static cast
        if (primaries < C2Color::PRIMARIES_VENDOR_START || primaries > C2Color::PRIMARIES_OTHER) {
            // undefined non-extension values map to 'Other'
            sfPrimaries = ColorAspects::PrimariesOther;
            res = false;
        } else {
            sfPrimaries = (ColorAspects::Primaries)primaries;
        }
    }

    if (!sColorMatricesSf.map(matrix, &sfMatrix)) {
        // use static cast and ensure it is in the extension range
        if (matrix < C2Color::MATRIX_VENDOR_START || matrix > C2Color::MATRIX_OTHER) {
            // undefined non-extension values map to 'Other'
            sfMatrix = ColorAspects::MatrixOther;
            res = false;
        } else {
            sfMatrix = (ColorAspects::MatrixCoeffs)matrix;
        }
    }

    *standard = ColorUtils::wrapColorAspectsIntoColorStandard(sfPrimaries, sfMatrix);

    return res;
}

// static
bool C2Mapper::map(int32_t standard, C2Color::primaries_t *primaries, C2Color::matrix_t *matrix) {
    // first map to stagefright foundation aspects => these actually map nearly 1:1 to
    // Codec 2.0 aspects
    ColorAspects::Primaries sfPrimaries;
    ColorAspects::MatrixCoeffs sfMatrix;
    bool res = true;
    (void)ColorUtils::unwrapColorAspectsFromColorStandard(standard, &sfPrimaries, &sfMatrix);

    // map known constants and keep vendor extensions. all other values are mapped to 'Other'
    if (!sColorPrimariesSf.map(sfPrimaries, primaries)) {
        // use static cast and ensure it is in the extension range
        *primaries = (C2Color::primaries_t)sfPrimaries;
        if (*primaries < C2Color::PRIMARIES_VENDOR_START || *primaries > C2Color::PRIMARIES_OTHER) {
            *primaries = C2Color::PRIMARIES_OTHER;
            res = false;
        }
    }

    if (!sColorMatricesSf.map(sfMatrix, matrix)) {
        // use static cast and ensure it is in the extension range
        *matrix = (C2Color::matrix_t)sfMatrix;
        if (*matrix < C2Color::MATRIX_VENDOR_START || *matrix > C2Color::MATRIX_OTHER) {
            *matrix = C2Color::MATRIX_OTHER;
            res = false;
        }
    }

    return res;
}

// static
bool C2Mapper::map(C2Color::primaries_t from, ColorAspects::Primaries *to) {
    return sColorPrimariesSf.map(from, to);
}

// static
bool C2Mapper::map(ColorAspects::Primaries from, C2Color::primaries_t *to) {
    return sColorPrimariesSf.map(from, to);
}

// static
bool C2Mapper::map(C2Color::matrix_t from, ColorAspects::MatrixCoeffs *to) {
    return sColorMatricesSf.map(from, to);
}

// static
bool C2Mapper::map(ColorAspects::MatrixCoeffs from, C2Color::matrix_t *to) {
    return sColorMatricesSf.map(from, to);
}

// static
bool C2Mapper::map(C2Color::transfer_t from, int32_t *to) {
    bool res = true;
    // map SDK defined values directly. For other values, use wrapping from ColorUtils.
    if (!sColorTransfers.map(from, to)) {
        ColorAspects::Transfer sfTransfer;

        // map known constants and keep vendor extensions. all other values are mapped to 'Other'
        if (!sColorTransfersSf.map(from, &sfTransfer)) {
            // use static cast and ensure it is in the extension range
            if (from < C2Color::TRANSFER_VENDOR_START || from > C2Color::TRANSFER_OTHER) {
                sfTransfer = ColorAspects::TransferOther;
                res = false;
            }
        }

        *to = ColorUtils::wrapColorAspectsIntoColorTransfer(sfTransfer);
    }
    return res;
}

// static
bool C2Mapper::map(int32_t from, C2Color::transfer_t *to) {
    // map SDK defined values directly. For other values, use wrapping from ColorUtils.
    if (!sColorTransfers.map(from, to)) {
        ColorAspects::Transfer sfTransfer;
        (void)ColorUtils::unwrapColorAspectsFromColorTransfer(from, &sfTransfer);

        // map known constants and keep vendor extensions. all other values are mapped to 'Other'
        if (!sColorTransfersSf.map(sfTransfer, to)) {
            // use static cast and ensure it is in the extension range
            *to = (C2Color::transfer_t)sfTransfer;
            if (*to < C2Color::TRANSFER_VENDOR_START || *to > C2Color::TRANSFER_OTHER) {
                *to = C2Color::TRANSFER_OTHER;
                return false;
            }
        }
    }

    return true;
}

// static
bool C2Mapper::map(
        C2Color::range_t range, C2Color::primaries_t primaries,
        C2Color::matrix_t matrix, C2Color::transfer_t transfer, uint32_t *dataSpace) {
#if 0
    // pure reimplementation
    *dataSpace = HAL_DATASPACE_UNKNOWN; // this is 0

    switch (range) {
        case C2Color::RANGE_FULL:    *dataSpace |= HAL_DATASPACE_RANGE_FULL;    break;
        case C2Color::RANGE_LIMITED: *dataSpace |= HAL_DATASPACE_RANGE_LIMITED; break;
        default: break;
    }

    switch (transfer) {
        case C2Color::TRANSFER_LINEAR:  *dataSpace |= HAL_DATASPACE_TRANSFER_LINEAR;     break;
        case C2Color::TRANSFER_SRGB:    *dataSpace |= HAL_DATASPACE_TRANSFER_SRGB;       break;
        case C2Color::TRANSFER_170M:    *dataSpace |= HAL_DATASPACE_TRANSFER_SMPTE_170M; break;
        case C2Color::TRANSFER_GAMMA22: *dataSpace |= HAL_DATASPACE_TRANSFER_GAMMA2_2;   break;
        case C2Color::TRANSFER_GAMMA28: *dataSpace |= HAL_DATASPACE_TRANSFER_GAMMA2_8;   break;
        case C2Color::TRANSFER_ST2084:  *dataSpace |= HAL_DATASPACE_TRANSFER_ST2084;     break;
        case C2Color::TRANSFER_HLG:     *dataSpace |= HAL_DATASPACE_TRANSFER_HLG;        break;
        default: break;
    }

    switch (primaries) {
        case C2Color::PRIMARIES_BT601_525:
            *dataSpace |= (matrix == C2Color::MATRIX_240M
                            || matrix == C2Color::MATRIX_BT709)
                    ? HAL_DATASPACE_STANDARD_BT601_525_UNADJUSTED
                    : HAL_DATASPACE_STANDARD_BT601_525;
            break;
        case C2Color::PRIMARIES_BT601_625:
            *dataSpace |= (matrix == C2Color::MATRIX_240M
                            || matrix == C2Color::MATRIX_BT709)
                    ? HAL_DATASPACE_STANDARD_BT601_625_UNADJUSTED
                    : HAL_DATASPACE_STANDARD_BT601_625;
            break;
        case C2Color::PRIMARIES_BT2020:
            *dataSpace |= (matrix == C2Color::MATRIX_BT2020_CONSTANT
                    ? HAL_DATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE
                    : HAL_DATASPACE_STANDARD_BT2020);
            break;
        case C2Color::PRIMARIES_BT470_M:
            *dataSpace |= HAL_DATASPACE_STANDARD_BT470M;
            break;
        case C2Color::PRIMARIES_BT709:
            *dataSpace |= HAL_DATASPACE_STANDARD_BT709;
            break;
        default: break;
    }
#else
    // for now use legacy implementation
    ColorAspects aspects;
    if (!sColorRangesSf.map(range, &aspects.mRange)) {
        aspects.mRange = ColorAspects::RangeUnspecified;
    }
    if (!sColorPrimariesSf.map(primaries, &aspects.mPrimaries)) {
        aspects.mPrimaries = ColorAspects::PrimariesUnspecified;
    }
    if (!sColorMatricesSf.map(matrix, &aspects.mMatrixCoeffs)) {
        aspects.mMatrixCoeffs = ColorAspects::MatrixUnspecified;
    }
    if (!sColorTransfersSf.map(transfer, &aspects.mTransfer)) {
        aspects.mTransfer = ColorAspects::TransferUnspecified;
    }
    *dataSpace = ColorUtils::getDataSpaceForColorAspects(aspects, true /* mayExpand */);
#endif
    return true;
}

// static
bool C2Mapper::map(C2Color::transfer_t from, ColorAspects::Transfer *to) {
    return sColorTransfersSf.map(from, to);
}

// static
bool C2Mapper::map(ColorAspects::Transfer from, C2Color::transfer_t *to) {
    return sColorTransfersSf.map(from, to);
}

// static
bool C2Mapper::mapPixelFormatFrameworkToCodec(
        int32_t frameworkValue, uint32_t *c2Value) {
    if (!sPixelFormats.map(frameworkValue, c2Value)) {
        // passthrough if not mapped
        *c2Value = uint32_t(frameworkValue);
    }
    return true;
}

// static
bool C2Mapper::mapPixelFormatCodecToFramework(
        uint32_t c2Value, int32_t *frameworkValue) {
    if (!sPixelFormats.map(c2Value, frameworkValue)) {
        // passthrough if not mapped
        *frameworkValue = int32_t(c2Value);
    }
    return true;
}

// static
bool C2Mapper::map(C2Config::picture_type_t from, int32_t *to) {
    return sPictureType.map(from, to);
}

// static
bool C2Mapper::map(int32_t from, C2Config::picture_type_t *to) {
    return sPictureType.map(from, to);
}
