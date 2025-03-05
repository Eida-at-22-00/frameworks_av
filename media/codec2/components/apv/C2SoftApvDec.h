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

#ifndef ANDROID_C2_SOFT_APV_DEC_H_
#define ANDROID_C2_SOFT_APV_DEC_H_

#include <media/stagefright/foundation/ColorUtils.h>

#include <SimpleC2Component.h>
#include <inttypes.h>
#include <atomic>

#include "oapv.h"
#include <C2SoftApvCommon.h>

typedef unsigned int UWORD32;

typedef enum {
    IV_CHROMA_NA = 0xFFFFFFFF,
    IV_YUV_420P = 0x1,
    IV_YUV_422P = 0x2,
    IV_420_UV_INTL = 0x3,
    IV_YUV_422IBE = 0x4,
    IV_YUV_422ILE = 0x5,
    IV_YUV_444P = 0x6,
    IV_YUV_411P = 0x7,
    IV_GRAY = 0x8,
    IV_RGB_565 = 0x9,
    IV_RGB_24 = 0xa,
    IV_YUV_420SP_UV = 0xb,
    IV_YUV_420SP_VU = 0xc,
    IV_YUV_422SP_UV = 0xd,
    IV_YUV_422SP_VU = 0xe

} IV_COLOR_FORMAT_T;

typedef struct {
    /**
     * u4_size of the structure
     */
    UWORD32 u4_size;

    /**
     * Pointer to the API function pointer table of the codec
     */
    void* pv_fxns;

    /**
     * Pointer to the handle of the codec
     */
    void* pv_codec_handle;
} iv_obj_t;

namespace android {

struct C2SoftApvDec final : public SimpleC2Component {
    class IntfImpl;

    C2SoftApvDec(const char* name, c2_node_id_t id, const std::shared_ptr<IntfImpl>& intfImpl);
    virtual ~C2SoftApvDec();

    // From SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(const std::unique_ptr<C2Work>& work,
                 const std::shared_ptr<C2BlockPool>& pool) override;
    c2_status_t drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool>& pool) override;

  private:
    status_t createDecoder();
    status_t initDecoder();
    bool isConfigured() const;
    void drainDecoder();
    status_t setFlushMode();
    status_t resetDecoder();
    void resetPlugin();
    status_t deleteDecoder();
    void finishWork(uint64_t index, const std::unique_ptr<C2Work>& work,
                    const std::shared_ptr<C2GraphicBlock>& block);
    void drainRingBuffer(const std::unique_ptr<C2Work>& work,
                         const std::shared_ptr<C2BlockPool>& pool, bool eos);
    c2_status_t drainInternal(uint32_t drainMode, const std::shared_ptr<C2BlockPool>& pool,
                              const std::unique_ptr<C2Work>& work);

    status_t outputBuffer(const std::shared_ptr<C2BlockPool>& pool,
                          const std::unique_ptr<C2Work>& work);

    std::shared_ptr<IntfImpl> mIntf;
    iv_obj_t* mDecHandle;
    uint8_t* mOutBufferFlush;
    IV_COLOR_FORMAT_T mIvColorformat;
    uint32_t mOutputDelay;
    bool mHeaderDecoded;
    std::atomic_uint64_t mOutIndex;
    std::shared_ptr<C2GraphicBlock> mOutBlock;

    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormatInfo;

    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    uint32_t mHalPixelFormat;
    uint32_t mWidth;
    uint32_t mHeight;
    bool mSignalledOutputEos;
    bool mSignalledError;
    // Color aspects. These are ISO values and are meant to detect changes in aspects to avoid
    // converting them to C2 values for each frame
    struct VuiColorAspects {
        uint8_t primaries;
        uint8_t transfer;
        uint8_t coeffs;
        uint8_t fullRange;

        // default color aspects
        VuiColorAspects()
            : primaries(C2Color::PRIMARIES_UNSPECIFIED),
            transfer(C2Color::TRANSFER_UNSPECIFIED),
            coeffs(C2Color::MATRIX_UNSPECIFIED),
            fullRange(C2Color::RANGE_UNSPECIFIED) { }

        bool operator==(const VuiColorAspects &o) {
            return primaries == o.primaries && transfer == o.transfer && coeffs == o.coeffs
                && fullRange == o.fullRange;
        }
    } mBitstreamColorAspects;

    oapvd_t oapvdHandle;
    oapvm_t oapvmHandle;
    oapvd_cdesc_t cdesc;
    oapv_frms_t ofrms;

    int outputCsp;

    void getVuiParams(VuiColorAspects* buffer);

    C2_DO_NOT_COPY(C2SoftApvDec);
};

}  // namespace android

#endif
