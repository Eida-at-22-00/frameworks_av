/*
 * Copyright 2019 The Android Open Source Project
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

#include "CCodecConfig.h"

#include <set>

#include <gtest/gtest.h>

#include <android_media_codec.h>

#include <codec2/hidl/1.0/Configurable.h>
#include <codec2/hidl/client.h>
#include <util/C2InterfaceHelper.h>

#include <media/stagefright/MediaCodecConstants.h>

namespace {

enum ExtendedC2ParamIndexKind : C2Param::type_index_t {
    kParamIndexVendorInt32 = C2Param::TYPE_INDEX_VENDOR_START,
    kParamIndexVendorInt64,
    kParamIndexVendorString,
};

typedef C2PortParam<C2Info, C2Int32Value, kParamIndexVendorInt32> C2PortVendorInt32Info;
constexpr char C2_PARAMKEY_VENDOR_INT32[] = "example.int32";
constexpr char KEY_VENDOR_INT32[] = "vendor.example.int32.value";

typedef C2StreamParam<C2Info, C2Int64Value, kParamIndexVendorInt64> C2StreamVendorInt64Info;
constexpr char C2_PARAMKEY_VENDOR_INT64[] = "example.int64";
constexpr char KEY_VENDOR_INT64[] = "vendor.example.int64.value";

typedef C2PortParam<C2Info, C2StringValue, kParamIndexVendorString> C2PortVendorStringInfo;
constexpr char C2_PARAMKEY_VENDOR_STRING[] = "example.string";
constexpr char KEY_VENDOR_STRING[] = "vendor.example.string.value";

}  // namespace

namespace android {

class CCodecConfigTest : public ::testing::Test {
public:
    constexpr static int32_t kCodec2Int32 = 0xC0DEC2;
    constexpr static int64_t kCodec2Int64 = 0xC0DEC2C0DEC2ll;
    constexpr static char kCodec2Str[] = "codec2";

    CCodecConfigTest()
        : mReflector{std::make_shared<C2ReflectorHelper>()} {
          initializeSystemResources();
    }

    void init(
            C2Component::domain_t domain,
            C2Component::kind_t kind,
            const char *mediaType) {
        sp<hardware::media::c2::V1_0::utils::CachedConfigurable> cachedConfigurable =
            new hardware::media::c2::V1_0::utils::CachedConfigurable(
                    std::make_unique<Configurable>(mReflector, domain, kind, mediaType,
                                                   mSystemResources, mExcludedResources));
        cachedConfigurable->init(std::make_shared<Cache>());
        mConfigurable = std::make_shared<Codec2Client::Configurable>(cachedConfigurable);
    }

    struct Cache : public hardware::media::c2::V1_0::utils::ParameterCache {
        c2_status_t validate(const std::vector<std::shared_ptr<C2ParamDescriptor>>&) override {
            return C2_OK;
        }
    };

    class Configurable : public hardware::media::c2::V1_0::utils::ConfigurableC2Intf {
    public:
        Configurable(
                const std::shared_ptr<C2ReflectorHelper> &reflector,
                C2Component::domain_t domain,
                C2Component::kind_t kind,
                const char *mediaType,
                const std::vector<C2SystemResourceStruct>& systemResources,
                const std::vector<C2SystemResourceStruct>& excludedResources)
            : ConfigurableC2Intf("name", 0u),
              mImpl(reflector, domain, kind, mediaType, systemResources, excludedResources) {
        }

        c2_status_t query(
                const std::vector<C2Param::Index> &indices,
                c2_blocking_t mayBlock,
                std::vector<std::unique_ptr<C2Param>>* const params) const override {
            return mImpl.query({}, indices, mayBlock, params);
        }

        c2_status_t config(
                const std::vector<C2Param*> &params,
                c2_blocking_t mayBlock,
                std::vector<std::unique_ptr<C2SettingResult>>* const failures) override {
            return mImpl.config(params, mayBlock, failures);
        }

        c2_status_t querySupportedParams(
                std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const override {
            return mImpl.querySupportedParams(params);
        }

        c2_status_t querySupportedValues(
                std::vector<C2FieldSupportedValuesQuery>& fields,
                c2_blocking_t mayBlock) const override {
            return mImpl.querySupportedValues(fields, mayBlock);
        }

    private:
        class Impl : public C2InterfaceHelper {
        public:
            Impl(const std::shared_ptr<C2ReflectorHelper> &reflector,
                    C2Component::domain_t domain,
                    C2Component::kind_t kind,
                    const char *mediaType,
                    const std::vector<C2SystemResourceStruct>& systemResources,
                    const std::vector<C2SystemResourceStruct>& excludedResources)
                : C2InterfaceHelper{reflector} {

                setDerivedInstance(this);

                addParameter(
                        DefineParam(mDomain, C2_PARAMKEY_COMPONENT_DOMAIN)
                        .withConstValue(new C2ComponentDomainSetting(domain))
                        .build());

                addParameter(
                        DefineParam(mKind, C2_PARAMKEY_COMPONENT_KIND)
                        .withConstValue(new C2ComponentKindSetting(kind))
                        .build());

                addParameter(
                        DefineParam(mInputStreamCount, C2_PARAMKEY_INPUT_STREAM_COUNT)
                        .withConstValue(new C2PortStreamCountTuning::input(1))
                        .build());

                addParameter(
                        DefineParam(mOutputStreamCount, C2_PARAMKEY_OUTPUT_STREAM_COUNT)
                        .withConstValue(new C2PortStreamCountTuning::output(1))
                        .build());

                const char *rawMediaType = "";
                switch (domain) {
                    case C2Component::DOMAIN_IMAGE: [[fallthrough]];
                    case C2Component::DOMAIN_VIDEO:
                        rawMediaType = MIMETYPE_VIDEO_RAW;
                        break;
                    case C2Component::DOMAIN_AUDIO:
                        rawMediaType = MIMETYPE_AUDIO_RAW;
                        break;
                    default:
                        break;
                }
                bool isEncoder = kind == C2Component::KIND_ENCODER;
                std::string inputMediaType{isEncoder ? rawMediaType : mediaType};
                std::string outputMediaType{isEncoder ? mediaType : rawMediaType};

                auto allocSharedString = [](const auto &param, const std::string &str) {
                    typedef typename std::remove_reference<decltype(param)>::type::element_type T;
                    std::shared_ptr<T> ret = T::AllocShared(str.length() + 1);
                    strcpy(ret->m.value, str.c_str());
                    return ret;
                };

                addParameter(
                        DefineParam(mInputMediaType, C2_PARAMKEY_INPUT_MEDIA_TYPE)
                        .withConstValue(allocSharedString(mInputMediaType, inputMediaType))
                        .build());

                addParameter(
                        DefineParam(mOutputMediaType, C2_PARAMKEY_OUTPUT_MEDIA_TYPE)
                        .withConstValue(allocSharedString(mOutputMediaType, outputMediaType))
                        .build());

                addParameter(
                        DefineParam(mInt32Input, C2_PARAMKEY_VENDOR_INT32)
                        .withDefault(new C2PortVendorInt32Info::input(0))
                        .withFields({C2F(mInt32Input, value).any()})
                        .withSetter(Setter<decltype(mInt32Input)::element_type>)
                        .build());

                addParameter(
                        DefineParam(mInt64Output, C2_PARAMKEY_VENDOR_INT64)
                        .withDefault(new C2StreamVendorInt64Info::output(0u, 0))
                        .withFields({C2F(mInt64Output, value).any()})
                        .withSetter(Setter<decltype(mInt64Output)::element_type>)
                        .build());

                addParameter(
                        DefineParam(mStringInput, C2_PARAMKEY_VENDOR_STRING)
                        .withDefault(decltype(mStringInput)::element_type::AllocShared(1, ""))
                        .withFields({C2F(mStringInput, m.value).any()})
                        .withSetter(Setter<decltype(mStringInput)::element_type>)
                        .build());

                addParameter(
                        DefineParam(mPixelAspectRatio, C2_PARAMKEY_PIXEL_ASPECT_RATIO)
                        .withDefault(new C2StreamPixelAspectRatioInfo::output(0u, 1, 1))
                        .withFields({
                            C2F(mPixelAspectRatio, width).any(),
                            C2F(mPixelAspectRatio, height).any(),
                        })
                        .withSetter(Setter<C2StreamPixelAspectRatioInfo::output>)
                        .build());

                // Add System Resource Capacity
                addParameter(
                    DefineParam(mResourcesCapacity, C2_PARAMKEY_RESOURCES_CAPACITY)
                    .withDefault(C2ResourcesCapacityTuning::AllocShared(
                            systemResources.size(), systemResources))
                    .withFields({
                            C2F(mResourcesCapacity, m.values[0].id).any(),
                            C2F(mResourcesCapacity, m.values[0].kind).any(),
                            C2F(mResourcesCapacity, m.values[0].amount).any(),
                    })
                    .withSetter(Setter<C2ResourcesCapacityTuning>)
                    .build());

                // Add Excluded System Resources
                addParameter(
                    DefineParam(mResourcesExcluded, C2_PARAMKEY_RESOURCES_EXCLUDED)
                    .withDefault(C2ResourcesExcludedTuning::AllocShared(
                            excludedResources.size(), excludedResources))
                    .withFields({
                            C2F(mResourcesExcluded, m.values[0].id).any(),
                            C2F(mResourcesExcluded, m.values[0].kind).any(),
                            C2F(mResourcesExcluded, m.values[0].amount).any(),
                    })
                    .withSetter(Setter<C2ResourcesExcludedTuning>)
                    .build());

                if (isEncoder) {
                    addParameter(
                            DefineParam(mInputBitrate, C2_PARAMKEY_BITRATE)
                            .withDefault(new C2StreamBitrateInfo::input(0u))
                            .withFields({C2F(mInputBitrate, value).any()})
                            .withSetter(Setter<C2StreamBitrateInfo::input>)
                            .build());

                    addParameter(
                            DefineParam(mOutputBitrate, C2_PARAMKEY_BITRATE)
                            .withDefault(new C2StreamBitrateInfo::output(0u))
                            .withFields({C2F(mOutputBitrate, value).any()})
                            .calculatedAs(
                                Copy<C2StreamBitrateInfo::output, C2StreamBitrateInfo::input>,
                                mInputBitrate)
                            .build());

                    addParameter(
                            DefineParam(mOutputProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                            .withDefault(new C2StreamProfileLevelInfo::output(
                                    0u, PROFILE_UNUSED, LEVEL_UNUSED))
                            .withFields({
                                C2F(mOutputProfileLevel, profile).any(),
                                C2F(mOutputProfileLevel, level).any(),
                            })
                            .withSetter(Setter<C2StreamProfileLevelInfo::output>)
                            .build());

                    std::vector<C2QpOffsetRectStruct> c2QpOffsetRectsInfo;
                    addParameter(
                            DefineParam(mInputQpOffsetRects, C2_PARAMKEY_QP_OFFSET_RECTS)
                                    .withDefault(C2StreamQpOffsetRects::output::AllocShared(
                                            c2QpOffsetRectsInfo.size(), 0, c2QpOffsetRectsInfo))
                                    .withFields({
                                            C2F(mInputQpOffsetRects, m.values[0].qpOffset)
                                                    .inRange(-128, 127),
                                            C2F(mInputQpOffsetRects, m.values[0].left).any(),
                                            C2F(mInputQpOffsetRects, m.values[0].top).any(),
                                            C2F(mInputQpOffsetRects, m.values[0].width).any(),
                                            C2F(mInputQpOffsetRects, m.values[0].height).any(),
                                    })
                                    .withSetter(Setter<C2StreamQpOffsetRects::output>)
                                    .build());
                }

                // TODO: more SDK params
            }
        private:
            std::shared_ptr<C2ComponentDomainSetting> mDomain;
            std::shared_ptr<C2ComponentKindSetting> mKind;
            std::shared_ptr<C2PortStreamCountTuning::input> mInputStreamCount;
            std::shared_ptr<C2PortStreamCountTuning::output> mOutputStreamCount;
            std::shared_ptr<C2PortMediaTypeSetting::input> mInputMediaType;
            std::shared_ptr<C2PortMediaTypeSetting::output> mOutputMediaType;
            std::shared_ptr<C2PortVendorInt32Info::input> mInt32Input;
            std::shared_ptr<C2StreamVendorInt64Info::output> mInt64Output;
            std::shared_ptr<C2PortVendorStringInfo::input> mStringInput;
            std::shared_ptr<C2StreamPixelAspectRatioInfo::output> mPixelAspectRatio;
            std::shared_ptr<C2StreamBitrateInfo::input> mInputBitrate;
            std::shared_ptr<C2StreamBitrateInfo::output> mOutputBitrate;
            std::shared_ptr<C2StreamProfileLevelInfo::input> mInputProfileLevel;
            std::shared_ptr<C2StreamProfileLevelInfo::output> mOutputProfileLevel;
            std::shared_ptr<C2StreamQpOffsetRects::output> mInputQpOffsetRects;
            std::shared_ptr<C2ResourcesCapacityTuning> mResourcesCapacity;
            std::shared_ptr<C2ResourcesExcludedTuning> mResourcesExcluded;

            template<typename T>
            static C2R Setter(bool, C2P<T> &) {
                return C2R::Ok();
            }

            template<typename ME, typename DEP>
            static C2R Copy(bool, C2P<ME> &me, const C2P<DEP> &dep) {
                me.set().value = dep.v.value;
                return C2R::Ok();
            }
        };

        Impl mImpl;
    };

    std::shared_ptr<C2ReflectorHelper> mReflector;
    std::shared_ptr<Codec2Client::Configurable> mConfigurable;
    CCodecConfig mConfig;

    /*
     * This test tracks two system resources:
     *  - max instance limit, which is capped at 64
     *  - max pixel count: up to 4 instances of 4K ==> 4 * 3840 * 2400
     *
     *  These 2 resource types are given 2 different ids as below.
     */
    void initializeSystemResources() {
        // max instance limit 64
        const uint32_t kMaxInstanceCount = 0x1000;
        // max pixel count: up to 4 instances of 4K
        const uint32_t kMaxPixelCount = 0x1001;
        mSystemResources.push_back(C2SystemResourceStruct(kMaxInstanceCount, CONST, 64));
        mSystemResources.push_back(C2SystemResourceStruct(kMaxPixelCount, CONST, 4 * 3840 * 2400));

        // Nothing is excluded, but lets just add them with amount as 0.
        mExcludedResources.push_back(C2SystemResourceStruct(kMaxInstanceCount, CONST, 0));
        mExcludedResources.push_back(C2SystemResourceStruct(kMaxPixelCount, CONST, 0));
    }

    bool validateSystemResources(const std::vector<C2SystemResourceStruct>& resources) const {
        if (resources.size() != mSystemResources.size()) {
            return false;
        }

        for (const auto& resource : mSystemResources) {
            auto found = std::find_if(resources.begin(),
                                      resources.end(),
                                      [resource](const C2SystemResourceStruct& item) {
                                          return (item.id == resource.id &&
                                                  item.kind == resource.kind &&
                                                  item.amount == resource.amount); });

            if (found == resources.end()) {
                return false;
            }
        }

        return true;
    }

private:
    std::vector<C2SystemResourceStruct> mSystemResources;
    std::vector<C2SystemResourceStruct> mExcludedResources;
};

using D = CCodecConfig::Domain;

template<typename T>
T *FindParam(const std::vector<std::unique_ptr<C2Param>> &vec) {
    for (const std::unique_ptr<C2Param> &param : vec) {
        if (param->coreIndex() == T::CORE_INDEX) {
            return static_cast<T *>(param.get());
        }
    }
    return nullptr;
}

TEST_F(CCodecConfigTest, SetVendorParam) {
    // Test at audio domain, as video domain has a few local parameters that
    // interfere with the testing.
    init(C2Component::DOMAIN_AUDIO, C2Component::KIND_DECODER, MIMETYPE_AUDIO_AAC);

    ASSERT_EQ(OK, mConfig.initialize(mReflector, mConfigurable));

    sp<AMessage> format{new AMessage};
    format->setInt32(KEY_VENDOR_INT32, kCodec2Int32);
    format->setInt64(KEY_VENDOR_INT64, kCodec2Int64);
    format->setString(KEY_VENDOR_STRING, kCodec2Str);

    std::vector<std::unique_ptr<C2Param>> configUpdate;
    ASSERT_EQ(OK, mConfig.getConfigUpdateFromSdkParams(
            mConfigurable, format, D::ALL, C2_MAY_BLOCK, &configUpdate));

    ASSERT_EQ(3u, configUpdate.size());
    C2PortVendorInt32Info::input *i32 =
        FindParam<std::remove_pointer<decltype(i32)>::type>(configUpdate);
    ASSERT_NE(nullptr, i32);
    ASSERT_EQ(kCodec2Int32, i32->value);

    C2StreamVendorInt64Info::output *i64 =
        FindParam<std::remove_pointer<decltype(i64)>::type>(configUpdate);
    ASSERT_NE(nullptr, i64);
    ASSERT_EQ(kCodec2Int64, i64->value);

    C2PortVendorStringInfo::input *str =
        FindParam<std::remove_pointer<decltype(str)>::type>(configUpdate);
    ASSERT_NE(nullptr, str);
    ASSERT_STREQ(kCodec2Str, str->m.value);
}

TEST_F(CCodecConfigTest, VendorParamUpdate_Unsubscribed) {
    // Test at audio domain, as video domain has a few local parameters that
    // interfere with the testing.
    init(C2Component::DOMAIN_AUDIO, C2Component::KIND_DECODER, MIMETYPE_AUDIO_AAC);

    ASSERT_EQ(OK, mConfig.initialize(mReflector, mConfigurable));

    std::vector<std::unique_ptr<C2Param>> configUpdate;
    C2PortVendorInt32Info::input i32(kCodec2Int32);
    C2StreamVendorInt64Info::output i64(0u, kCodec2Int64);
    std::unique_ptr<C2PortVendorStringInfo::input> str =
        C2PortVendorStringInfo::input::AllocUnique(strlen(kCodec2Str) + 1, kCodec2Str);
    configUpdate.push_back(C2Param::Copy(i32));
    configUpdate.push_back(C2Param::Copy(i64));
    configUpdate.push_back(std::move(str));

    // The vendor parameters are not yet subscribed
    ASSERT_FALSE(mConfig.updateConfiguration(configUpdate, D::ALL));

    int32_t vendorInt32{0};
    ASSERT_FALSE(mConfig.mInputFormat->findInt32(KEY_VENDOR_INT32, &vendorInt32))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();
    ASSERT_FALSE(mConfig.mOutputFormat->findInt32(KEY_VENDOR_INT32, &vendorInt32))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    int64_t vendorInt64{0};
    ASSERT_FALSE(mConfig.mInputFormat->findInt64(KEY_VENDOR_INT64, &vendorInt64))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();
    ASSERT_FALSE(mConfig.mOutputFormat->findInt64(KEY_VENDOR_INT64, &vendorInt64))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    AString vendorString;
    ASSERT_FALSE(mConfig.mInputFormat->findString(KEY_VENDOR_STRING, &vendorString))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();
    ASSERT_FALSE(mConfig.mOutputFormat->findString(KEY_VENDOR_STRING, &vendorString))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
}

TEST_F(CCodecConfigTest, VendorParamUpdate_AllSubscribed) {
    // Test at audio domain, as video domain has a few local parameters that
    // interfere with the testing.
    init(C2Component::DOMAIN_AUDIO, C2Component::KIND_DECODER, MIMETYPE_AUDIO_AAC);

    ASSERT_EQ(OK, mConfig.initialize(mReflector, mConfigurable));

    // Force subscribe to all vendor params
    ASSERT_EQ(OK, mConfig.subscribeToAllVendorParams(mConfigurable, C2_MAY_BLOCK));

    std::vector<std::unique_ptr<C2Param>> configUpdate;
    C2PortVendorInt32Info::input i32(kCodec2Int32);
    C2StreamVendorInt64Info::output i64(0u, kCodec2Int64);
    std::unique_ptr<C2PortVendorStringInfo::input> str =
        C2PortVendorStringInfo::input::AllocUnique(strlen(kCodec2Str) + 1, kCodec2Str);
    configUpdate.push_back(C2Param::Copy(i32));
    configUpdate.push_back(C2Param::Copy(i64));
    configUpdate.push_back(std::move(str));

    ASSERT_TRUE(mConfig.updateConfiguration(configUpdate, D::ALL));

    int32_t vendorInt32{0};
    ASSERT_TRUE(mConfig.mInputFormat->findInt32(KEY_VENDOR_INT32, &vendorInt32))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();
    ASSERT_EQ(kCodec2Int32, vendorInt32);
    ASSERT_FALSE(mConfig.mOutputFormat->findInt32(KEY_VENDOR_INT32, &vendorInt32))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    int64_t vendorInt64{0};
    ASSERT_FALSE(mConfig.mInputFormat->findInt64(KEY_VENDOR_INT64, &vendorInt64))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();
    ASSERT_TRUE(mConfig.mOutputFormat->findInt64(KEY_VENDOR_INT64, &vendorInt64))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    ASSERT_EQ(kCodec2Int64, vendorInt64);

    AString vendorString;
    ASSERT_TRUE(mConfig.mInputFormat->findString(KEY_VENDOR_STRING, &vendorString))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();
    ASSERT_STREQ(kCodec2Str, vendorString.c_str());
    ASSERT_FALSE(mConfig.mOutputFormat->findString(KEY_VENDOR_STRING, &vendorString))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
}

TEST_F(CCodecConfigTest, VendorParamUpdate_PartiallySubscribed) {
    // Test at audio domain, as video domain has a few local parameters that
    // interfere with the testing.
    init(C2Component::DOMAIN_AUDIO, C2Component::KIND_DECODER, MIMETYPE_AUDIO_AAC);

    ASSERT_EQ(OK, mConfig.initialize(mReflector, mConfigurable));

    // Subscribe to example.int32 only
    std::vector<std::unique_ptr<C2Param>> configUpdate;
    sp<AMessage> format{new AMessage};
    format->setInt32(KEY_VENDOR_INT32, 0);
    configUpdate.clear();
    ASSERT_EQ(OK, mConfig.getConfigUpdateFromSdkParams(
            mConfigurable, format, D::ALL, C2_MAY_BLOCK, &configUpdate));
    ASSERT_EQ(OK, mConfig.setParameters(mConfigurable, configUpdate, C2_MAY_BLOCK));

    C2PortVendorInt32Info::input i32(kCodec2Int32);
    C2StreamVendorInt64Info::output i64(0u, kCodec2Int64);
    std::unique_ptr<C2PortVendorStringInfo::input> str =
        C2PortVendorStringInfo::input::AllocUnique(strlen(kCodec2Str) + 1, kCodec2Str);
    configUpdate.clear();
    configUpdate.push_back(C2Param::Copy(i32));
    configUpdate.push_back(C2Param::Copy(i64));
    configUpdate.push_back(std::move(str));

    // Only example.i32 should be updated
    ASSERT_TRUE(mConfig.updateConfiguration(configUpdate, D::ALL));

    int32_t vendorInt32{0};
    ASSERT_TRUE(mConfig.mInputFormat->findInt32(KEY_VENDOR_INT32, &vendorInt32))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();
    ASSERT_EQ(kCodec2Int32, vendorInt32);
    ASSERT_FALSE(mConfig.mOutputFormat->findInt32(KEY_VENDOR_INT32, &vendorInt32))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    int64_t vendorInt64{0};
    ASSERT_FALSE(mConfig.mInputFormat->findInt64(KEY_VENDOR_INT64, &vendorInt64))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();
    ASSERT_FALSE(mConfig.mOutputFormat->findInt64(KEY_VENDOR_INT64, &vendorInt64))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    AString vendorString;
    ASSERT_FALSE(mConfig.mInputFormat->findString(KEY_VENDOR_STRING, &vendorString))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();
    ASSERT_FALSE(mConfig.mOutputFormat->findString(KEY_VENDOR_STRING, &vendorString))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
}

TEST_F(CCodecConfigTest, SetPixelAspectRatio) {
    init(C2Component::DOMAIN_VIDEO, C2Component::KIND_DECODER, MIMETYPE_VIDEO_AVC);

    ASSERT_EQ(OK, mConfig.initialize(mReflector, mConfigurable));

    sp<AMessage> format{new AMessage};
    format->setInt32(KEY_PIXEL_ASPECT_RATIO_WIDTH, 12);
    format->setInt32(KEY_PIXEL_ASPECT_RATIO_HEIGHT, 11);

    std::vector<std::unique_ptr<C2Param>> configUpdate;
    ASSERT_EQ(OK, mConfig.getConfigUpdateFromSdkParams(
            mConfigurable, format, D::ALL, C2_MAY_BLOCK, &configUpdate));

    ASSERT_EQ(1u, configUpdate.size());
    C2StreamPixelAspectRatioInfo::output *par =
        FindParam<std::remove_pointer<decltype(par)>::type>(configUpdate);
    ASSERT_NE(nullptr, par);
    ASSERT_EQ(12, par->width);
    ASSERT_EQ(11, par->height);
}

TEST_F(CCodecConfigTest, PixelAspectRatioUpdate) {
    init(C2Component::DOMAIN_VIDEO, C2Component::KIND_DECODER, MIMETYPE_VIDEO_AVC);

    ASSERT_EQ(OK, mConfig.initialize(mReflector, mConfigurable));

    std::vector<std::unique_ptr<C2Param>> configUpdate;
    C2StreamPixelAspectRatioInfo::output par(0u, 12, 11);
    configUpdate.push_back(C2Param::Copy(par));

    ASSERT_TRUE(mConfig.updateConfiguration(configUpdate, D::ALL));

    int32_t parWidth{0};
    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_PIXEL_ASPECT_RATIO_WIDTH, &parWidth))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    ASSERT_EQ(12, parWidth);
    ASSERT_FALSE(mConfig.mInputFormat->findInt32(KEY_PIXEL_ASPECT_RATIO_WIDTH, &parWidth))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();

    int32_t parHeight{0};
    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_PIXEL_ASPECT_RATIO_HEIGHT, &parHeight))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    ASSERT_EQ(11, parHeight);
    ASSERT_FALSE(mConfig.mInputFormat->findInt32(KEY_PIXEL_ASPECT_RATIO_HEIGHT, &parHeight))
            << "mInputFormat = " << mConfig.mInputFormat->debugString().c_str();
}

TEST_F(CCodecConfigTest, DataspaceUpdate) {
    init(C2Component::DOMAIN_VIDEO, C2Component::KIND_ENCODER, MIMETYPE_VIDEO_AVC);

    ASSERT_EQ(OK, mConfig.initialize(mReflector, mConfigurable));
    class InputSurfaceStub : public InputSurfaceWrapper {
    public:
        ~InputSurfaceStub() override = default;
        status_t connect(const std::shared_ptr<Codec2Client::Component> &) override {
            return OK;
        }
        void disconnect() override {}
        status_t start() override { return OK; }
        status_t signalEndOfInputStream() override { return OK; }
        status_t configure(Config &) override { return OK; }
    };
    mConfig.mInputSurface = std::make_shared<InputSurfaceStub>();

    sp<AMessage> format{new AMessage};
    format->setInt32(KEY_COLOR_RANGE, COLOR_RANGE_LIMITED);
    format->setInt32(KEY_COLOR_STANDARD, COLOR_STANDARD_BT709);
    format->setInt32(KEY_COLOR_TRANSFER, COLOR_TRANSFER_SDR_VIDEO);
    format->setInt32(KEY_BIT_RATE, 100);

    std::vector<std::unique_ptr<C2Param>> configUpdate;
    ASSERT_EQ(OK, mConfig.getConfigUpdateFromSdkParams(
            mConfigurable, format, D::ALL, C2_MAY_BLOCK, &configUpdate));
    ASSERT_TRUE(mConfig.updateConfiguration(configUpdate, D::ALL));

    int32_t range{0};
    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_COLOR_RANGE, &range))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    EXPECT_EQ(COLOR_RANGE_LIMITED, range)
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    int32_t standard{0};
    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_COLOR_STANDARD, &standard))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    EXPECT_EQ(COLOR_STANDARD_BT709, standard)
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    int32_t transfer{0};
    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_COLOR_TRANSFER, &transfer))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    EXPECT_EQ(COLOR_TRANSFER_SDR_VIDEO, transfer)
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    mConfig.mInputSurface->setDataSpace(HAL_DATASPACE_BT2020_PQ);

    // Dataspace from input surface should override the configured setting
    mConfig.updateFormats(D::ALL);

    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_COLOR_RANGE, &range))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    EXPECT_EQ(COLOR_RANGE_FULL, range)
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_COLOR_STANDARD, &standard))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    EXPECT_EQ(COLOR_STANDARD_BT2020, standard)
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_COLOR_TRANSFER, &transfer))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    EXPECT_EQ(COLOR_TRANSFER_ST2084, transfer)
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    // Simulate bitrate update
    format = new AMessage;
    format->setInt32(KEY_BIT_RATE, 200);
    configUpdate.clear();
    ASSERT_EQ(OK, mConfig.getConfigUpdateFromSdkParams(
            mConfigurable, format, D::ALL, C2_MAY_BLOCK, &configUpdate));
    ASSERT_EQ(OK, mConfig.setParameters(mConfigurable, configUpdate, C2_MAY_BLOCK));

    // Color information should remain the same
    mConfig.updateFormats(D::ALL);

    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_COLOR_RANGE, &range))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    EXPECT_EQ(COLOR_RANGE_FULL, range)
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_COLOR_STANDARD, &standard))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    EXPECT_EQ(COLOR_STANDARD_BT2020, standard)
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();

    ASSERT_TRUE(mConfig.mOutputFormat->findInt32(KEY_COLOR_TRANSFER, &transfer))
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
    EXPECT_EQ(COLOR_TRANSFER_ST2084, transfer)
            << "mOutputFormat = " << mConfig.mOutputFormat->debugString().c_str();
}

typedef std::tuple<std::string, C2Config::profile_t, int32_t> HdrProfilesParams;

class HdrProfilesTest
    : public CCodecConfigTest,
      public ::testing::WithParamInterface<HdrProfilesParams> {
};

TEST_P(HdrProfilesTest, SetFromSdk) {
    HdrProfilesParams params = GetParam();
    std::string mediaType = std::get<0>(params);
    C2Config::profile_t c2Profile = std::get<1>(params);
    int32_t sdkProfile = std::get<2>(params);

    init(C2Component::DOMAIN_VIDEO, C2Component::KIND_ENCODER, mediaType.c_str());

    ASSERT_EQ(OK, mConfig.initialize(mReflector, mConfigurable));

    sp<AMessage> format{new AMessage};
    format->setInt32(KEY_PROFILE, sdkProfile);

    std::vector<std::unique_ptr<C2Param>> configUpdate;
    ASSERT_EQ(OK, mConfig.getConfigUpdateFromSdkParams(
            mConfigurable, format, D::ALL, C2_MAY_BLOCK, &configUpdate));

    ASSERT_EQ(1u, configUpdate.size());
    C2StreamProfileLevelInfo::input *pl =
        FindParam<std::remove_pointer<decltype(pl)>::type>(configUpdate);
    ASSERT_NE(nullptr, pl);
    ASSERT_EQ(c2Profile, pl->profile);
}

HdrProfilesParams kHdrProfilesParams[] = {
    std::make_tuple(MIMETYPE_VIDEO_HEVC, PROFILE_HEVC_MAIN_10, HEVCProfileMain10HDR10),
    std::make_tuple(MIMETYPE_VIDEO_HEVC, PROFILE_HEVC_MAIN_10, HEVCProfileMain10HDR10Plus),
    std::make_tuple(MIMETYPE_VIDEO_VP9,  PROFILE_VP9_2,        VP9Profile2HDR),
    std::make_tuple(MIMETYPE_VIDEO_VP9,  PROFILE_VP9_2,        VP9Profile2HDR10Plus),
    std::make_tuple(MIMETYPE_VIDEO_VP9,  PROFILE_VP9_3,        VP9Profile3HDR),
    std::make_tuple(MIMETYPE_VIDEO_VP9,  PROFILE_VP9_3,        VP9Profile3HDR10Plus),
    std::make_tuple(MIMETYPE_VIDEO_AV1,  PROFILE_AV1_0,        AV1ProfileMain10HDR10),
    std::make_tuple(MIMETYPE_VIDEO_AV1,  PROFILE_AV1_0,        AV1ProfileMain10HDR10Plus),
};

INSTANTIATE_TEST_SUITE_P(
        CCodecConfig,
        HdrProfilesTest,
        ::testing::ValuesIn(kHdrProfilesParams));

TEST_F(CCodecConfigTest, SetRegionOfInterestParams) {
    if (!android::media::codec::provider_->region_of_interest()
        || !android::media::codec::provider_->region_of_interest_support()) {
        GTEST_SKIP() << "Skipping the test as region_of_interest flags are not enabled.\n";
    }

    init(C2Component::DOMAIN_VIDEO, C2Component::KIND_ENCODER, MIMETYPE_VIDEO_VP9);

    ASSERT_EQ(OK, mConfig.initialize(mReflector, mConfigurable));

    const int kWidth = 32;
    const int kHeight = 32;
    const int kNumBlocks = ((kWidth + 15) / 16) * ((kHeight + 15) / 16);
    int8_t mapInfo[kNumBlocks] = {-1, 0, 1, 1};
    int top[kNumBlocks] = {0, 0, 16, 16};
    int left[kNumBlocks] = {0, 16, 0, 16};
    int bottom[kNumBlocks] = {16, 16, 32, 32};
    int right[kNumBlocks] = {16, 32, 16, 32};
    sp<AMessage> format{new AMessage};
    format->setInt32(KEY_WIDTH, kWidth);
    format->setInt32(KEY_HEIGHT, kHeight);
    AString val;
    for (int i = 0; i < kNumBlocks; i++) {
        val.append(AStringPrintf("%d,%d-%d,%d=%d;", top[i], left[i], bottom[i],
                                 right[i], mapInfo[i]));
    }
    format->setString(PARAMETER_KEY_QP_OFFSET_RECTS, val);

    std::vector<std::unique_ptr<C2Param>> configUpdate;
    ASSERT_EQ(OK, mConfig.getConfigUpdateFromSdkParams(mConfigurable, format, D::CONFIG,
                                                       C2_MAY_BLOCK, &configUpdate));

    EXPECT_EQ(1u, configUpdate.size());

    C2StreamQpOffsetRects::output* qpRectParam =
            FindParam<std::remove_pointer<decltype(qpRectParam)>::type>(configUpdate);
    ASSERT_NE(nullptr, qpRectParam);
    ASSERT_EQ(kNumBlocks, qpRectParam->flexCount());
    for (auto i = 0; i < kNumBlocks; i++) {
        EXPECT_EQ(mapInfo[i], (int8_t)qpRectParam->m.values[i].qpOffset)
                << "qp offset for index " << i << " is not as expected ";
        EXPECT_EQ(left[i], qpRectParam->m.values[i].left)
                << "left for index " << i << " is not as expected ";
        EXPECT_EQ(top[i], qpRectParam->m.values[i].top)
                << "top for index " << i << " is not as expected ";
        EXPECT_EQ(right[i] - left[i], qpRectParam->m.values[i].width)
                << "width for index " << i << " is not as expected ";
        EXPECT_EQ(bottom[i] - top[i], qpRectParam->m.values[i].height)
                << "height for index " << i << " is not as expected ";
    }
}

static
c2_status_t queryGlobalResources(std::shared_ptr<Codec2Client::Configurable>& configurable,
                                 std::vector<C2SystemResourceStruct>& resources) {
    std::vector<std::unique_ptr<C2Param>> heapParams;
    c2_status_t c2err = configurable->query(
            {},
            {C2ResourcesCapacityTuning::PARAM_TYPE, C2ResourcesExcludedTuning::PARAM_TYPE},
            C2_MAY_BLOCK, &heapParams);

    if (c2err == C2_OK && heapParams.size() == 2u) {
        // Construct Globally available resources now.
        // Get the total capacity first.
        const C2ResourcesCapacityTuning* systemCapacity =
                C2ResourcesCapacityTuning::From(heapParams[0].get());
        if (systemCapacity && *systemCapacity) {
            for (size_t i = 0; i < systemCapacity->flexCount(); ++i) {
                resources.push_back(systemCapacity->m.values[i]);
                ALOGI("System Resource[%zu]{%u %d %jd}", i,
                      systemCapacity->m.values[i].id,
                      systemCapacity->m.values[i].kind,
                      systemCapacity->m.values[i].amount);
            }
        } else {
            ALOGE("Failed to get C2ResourcesCapacityTuning");
            return C2_BAD_VALUE;
        }

        // Get the excluded resource info.
        // The available resource should exclude this, if there are any.
        const C2ResourcesExcludedTuning* systemExcluded =
                C2ResourcesExcludedTuning::From(heapParams[1].get());
        if (systemExcluded && *systemExcluded) {
            for (size_t i = 0; i < systemExcluded->flexCount(); ++i) {
                const C2SystemResourceStruct& resource =
                    systemExcluded->m.values[i];
                ALOGI("Excluded Resource[%zu]{%u %d %jd}", i,
                      resource.id, resource.kind, resource.amount);
                uint64_t excluded = (resource.kind == CONST) ? resource.amount : 0;
                auto found = std::find_if(resources.begin(),
                                          resources.end(),
                                          [resource](const C2SystemResourceStruct& item) {
                                              return item.id == resource.id; });

                if (found != resources.end()) {
                    // Take off excluded resources from available resources.
                    if (found->amount >= excluded) {
                        found->amount -= excluded;
                    } else {
                       ALOGE("Excluded resources(%jd) can't be more than Available resources(%jd)",
                             excluded, found->amount);
                       return C2_BAD_VALUE;
                    }
                } else {
                    ALOGE("Failed to find the resource [%u]", resource.id);
                    return C2_BAD_VALUE;
                }
            }
        } else {
            ALOGE("Failed to get C2ResourcesExcludedTuning");
            return C2_BAD_VALUE;
        }

    } else if (c2err == C2_OK) {
        ALOGE("Expected query results for 2 params, but got %zu", heapParams.size());
        return C2_BAD_VALUE;
    } else {
        ALOGE("Failed to query component store for system resources: %d", c2err);
        return c2err;
    }

    size_t index = 0;
    for (const auto& resource : resources) {
        ALOGI("Globally Available System Resource[%zu]{%u %d %jd}", index++,
              resource.id, resource.kind, resource.amount);
    }
    return c2err;
}

TEST_F(CCodecConfigTest, QuerySystemResources) {
    init(C2Component::DOMAIN_VIDEO, C2Component::KIND_DECODER, MIMETYPE_VIDEO_AVC);

    ASSERT_EQ(OK, mConfig.initialize(mReflector, mConfigurable));

    std::vector<C2SystemResourceStruct> resources;
    ASSERT_EQ(C2_OK, queryGlobalResources(mConfigurable, resources));

    // Make sure that what we got from the query is the same as what was added.
    ASSERT_TRUE(validateSystemResources(resources));
}

} // namespace android
