/*
 * Copyright (C) 2019 The Android Open Source Project
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
#define LOG_TAG "AmrnbEncoderTest"

#include <utils/Log.h>

#include <audio_utils/sndfile.h>
#include <stdio.h>
#include <fstream>

#include "gsmamr_enc.h"

#include "AmrnbEncTestEnvironment.h"

#define OUTPUT_FILE "/data/local/tmp/amrnbEncode.out"

constexpr int32_t kInputBufferSize = L_FRAME * 2;  // 160 samples * 16-bit per sample.
constexpr int32_t kOutputBufferSize = 1024;
constexpr int32_t kNumFrameReset = 200;
constexpr int32_t kMaxCount = 10;
struct AmrNbEncState {
    void *encCtx;
    void *pidSyncCtx;
};

static AmrnbEncTestEnvironment *gEnv = nullptr;

class AmrnbEncoderTest : public ::testing::TestWithParam<tuple<string, int32_t, string>> {
  public:
    AmrnbEncoderTest() : mAmrEncHandle(nullptr) {}

    ~AmrnbEncoderTest() {
        if (mAmrEncHandle) {
            free(mAmrEncHandle);
            mAmrEncHandle = nullptr;
        }
    }

    AmrNbEncState *mAmrEncHandle;
    int32_t EncodeFrames(int32_t mode, FILE *fpInput, FILE *mFpOutput,
                         int32_t frameCount = INT32_MAX);
    bool compareBinaryFiles(const string& refFilePath, const string& outFilePath);
};

int32_t AmrnbEncoderTest::EncodeFrames(int32_t mode, FILE *fpInput, FILE *mFpOutput,
                                       int32_t frameCount) {
    int32_t frameNum = 0;
    uint16_t inputBuf[kInputBufferSize];
    uint8_t outputBuf[kOutputBufferSize];
    while (frameNum < frameCount) {
        int32_t bytesRead = fread(inputBuf, 1, kInputBufferSize, fpInput);
        if (bytesRead != kInputBufferSize && !feof(fpInput)) {
            ALOGE("Unable to read data from input file");
            return -1;
        } else if (feof(fpInput) && bytesRead == 0) {
            break;
        }
        Frame_Type_3GPP frame_type = (Frame_Type_3GPP)mode;
        int32_t bytesGenerated =
                AMREncode(mAmrEncHandle->encCtx, mAmrEncHandle->pidSyncCtx, (Mode)mode,
                          (Word16 *)inputBuf, outputBuf, &frame_type, AMR_TX_WMF);
        frameNum++;
        if (bytesGenerated < 0) {
            ALOGE("Error in encoging the file: Invalid output format");
            return -1;
        }

        // Convert from WMF to RFC 3267 format.
        if (bytesGenerated > 0) {
            outputBuf[0] = ((outputBuf[0] << 3) | 4) & 0x7c;
        }
        fwrite(outputBuf, 1, bytesGenerated, mFpOutput);
    }
    return 0;
}

bool AmrnbEncoderTest::compareBinaryFiles(const std::string &refFilePath,
                                          const std::string &outFilePath) {
    std::ifstream refFile(refFilePath, std::ios::binary | std::ios::ate);
    std::ifstream outFile(outFilePath, std::ios::binary | std::ios::ate);
    assert(refFile.is_open() && "Error opening reference file " + refFilePath);
    assert(outFile.is_open() && "Error opening output file " + outFilePath);

    std::streamsize refFileSize = refFile.tellg();
    std::streamsize outFileSize = outFile.tellg();
    if (refFileSize != outFileSize) {
        ALOGE("Error, File size mismatch: Reference file size = %td bytes,"
              " but output file size = %td bytes.", refFileSize, outFileSize);
        return false;
    }

    refFile.seekg(0, std::ios::beg);
    outFile.seekg(0, std::ios::beg);
    constexpr std::streamsize kBufferSize = 16 * 1024;
    char refBuffer[kBufferSize];
    char outBuffer[kBufferSize];

    while (refFile && outFile) {
        refFile.read(refBuffer, kBufferSize);
        outFile.read(outBuffer, kBufferSize);

        std::streamsize refBytesRead = refFile.gcount();
        std::streamsize outBytesRead = outFile.gcount();

        if (refBytesRead != outBytesRead || memcmp(refBuffer, outBuffer, refBytesRead) != 0) {
            ALOGE("Error, File content mismatch.");
            return false;
        }
    }
    return true;
}

TEST_F(AmrnbEncoderTest, CreateAmrnbEncoderTest) {
    mAmrEncHandle = (AmrNbEncState *)malloc(sizeof(AmrNbEncState));
    ASSERT_NE(mAmrEncHandle, nullptr) << "Error in allocating memory to Codec handle";
    for (int count = 0; count < kMaxCount; count++) {
        int32_t status = AMREncodeInit(&mAmrEncHandle->encCtx, &mAmrEncHandle->pidSyncCtx, 0);
        ASSERT_EQ(status, 0) << "Error creating AMR-NB encoder";
        ALOGV("Successfully created encoder");
    }
    if (mAmrEncHandle) {
        AMREncodeExit(&mAmrEncHandle->encCtx, &mAmrEncHandle->pidSyncCtx);
        ASSERT_EQ(mAmrEncHandle->encCtx, nullptr) << "Error deleting AMR-NB encoder";
        ASSERT_EQ(mAmrEncHandle->pidSyncCtx, nullptr) << "Error deleting AMR-NB encoder";
        free(mAmrEncHandle);
        mAmrEncHandle = nullptr;
        ALOGV("Successfully deleted encoder");
    }
}

TEST_P(AmrnbEncoderTest, EncodeTest) {
    mAmrEncHandle = (AmrNbEncState *)malloc(sizeof(AmrNbEncState));
    ASSERT_NE(mAmrEncHandle, nullptr) << "Error in allocating memory to Codec handle";
    int32_t status = AMREncodeInit(&mAmrEncHandle->encCtx, &mAmrEncHandle->pidSyncCtx, 0);
    ASSERT_EQ(status, 0) << "Error creating AMR-NB encoder";

    string inputFile = gEnv->getRes() + std::get<0>(GetParam());
    FILE *fpInput = fopen(inputFile.c_str(), "rb");
    ASSERT_NE(fpInput, nullptr) << "Error opening input file " << inputFile;

    FILE *fpOutput = fopen(OUTPUT_FILE, "wb");
    ASSERT_NE(fpOutput, nullptr) << "Error opening output file " << OUTPUT_FILE;

    // Write file header.
    fwrite("#!AMR\n", 1, 6, fpOutput);

    int32_t mode = std::get<1>(GetParam());
    int32_t encodeErr = EncodeFrames(mode, fpInput, fpOutput);
    ASSERT_EQ(encodeErr, 0) << "EncodeFrames returned error for Codec mode: " << mode;

    fclose(fpOutput);
    fclose(fpInput);

    AMREncodeExit(&mAmrEncHandle->encCtx, &mAmrEncHandle->pidSyncCtx);
    ASSERT_EQ(mAmrEncHandle->encCtx, nullptr) << "Error deleting AMR-NB encoder";
    ASSERT_EQ(mAmrEncHandle->pidSyncCtx, nullptr) << "Error deleting AMR-NB encoder";
    free(mAmrEncHandle);
    mAmrEncHandle = nullptr;
    ALOGV("Successfully deleted encoder");

    string refFilePath = gEnv->getRes() + std::get<2>(GetParam());
    ASSERT_TRUE(compareBinaryFiles(refFilePath, OUTPUT_FILE))
       << "Error, Binary file comparison failed: Output file " << OUTPUT_FILE
       << " does not match the reference file " << refFilePath << ".";
}

TEST_P(AmrnbEncoderTest, ResetEncoderTest) {
    mAmrEncHandle = (AmrNbEncState *)malloc(sizeof(AmrNbEncState));
    ASSERT_NE(mAmrEncHandle, nullptr) << "Error in allocating memory to Codec handle";
    int32_t status = AMREncodeInit(&mAmrEncHandle->encCtx, &mAmrEncHandle->pidSyncCtx, 0);
    ASSERT_EQ(status, 0) << "Error creating AMR-NB encoder";

    string inputFile = gEnv->getRes() + std::get<0>(GetParam());
    FILE *fpInput = fopen(inputFile.c_str(), "rb");
    ASSERT_NE(fpInput, nullptr) << "Error opening input file " << inputFile;

    FILE *fpOutput = fopen(OUTPUT_FILE, "wb");
    ASSERT_NE(fpOutput, nullptr) << "Error opening output file " << OUTPUT_FILE;

    // Write file header.
    fwrite("#!AMR\n", 1, 6, fpOutput);

    int32_t mode = std::get<1>(GetParam());
    // Encode kNumFrameReset first
    int32_t encodeErr = EncodeFrames(mode, fpInput, fpOutput, kNumFrameReset);
    ASSERT_EQ(encodeErr, 0) << "EncodeFrames returned error for Codec mode: " << mode;

    status = AMREncodeReset(mAmrEncHandle->encCtx, mAmrEncHandle->pidSyncCtx);
    ASSERT_EQ(status, 0) << "Error resting AMR-NB encoder";

    // Start encoding again
    encodeErr = EncodeFrames(mode, fpInput, fpOutput);
    ASSERT_EQ(encodeErr, 0) << "EncodeFrames returned error for Codec mode: " << mode;

    fclose(fpOutput);
    fclose(fpInput);

    AMREncodeExit(&mAmrEncHandle->encCtx, &mAmrEncHandle->pidSyncCtx);
    ASSERT_EQ(mAmrEncHandle->encCtx, nullptr) << "Error deleting AMR-NB encoder";
    ASSERT_EQ(mAmrEncHandle->pidSyncCtx, nullptr) << "Error deleting AMR-NB encoder";
    free(mAmrEncHandle);
    mAmrEncHandle = nullptr;
    ALOGV("Successfully deleted encoder");
}

// TODO: Add more test vectors
INSTANTIATE_TEST_SUITE_P(AmrnbEncoderTestAll, AmrnbEncoderTest,
    ::testing::Values(
        make_tuple("bbb_raw_1ch_8khz_s16le.raw", MR475, "bbb_raw_1ch_8khz_s16le_MR475_ref.amrnb"),
        make_tuple("bbb_raw_1ch_8khz_s16le.raw", MR515, "bbb_raw_1ch_8khz_s16le_MR515_ref.amrnb"),
        make_tuple("bbb_raw_1ch_8khz_s16le.raw", MR59, "bbb_raw_1ch_8khz_s16le_MR59_ref.amrnb"),
        make_tuple("bbb_raw_1ch_8khz_s16le.raw", MR67, "bbb_raw_1ch_8khz_s16le_MR67_ref.amrnb"),
        make_tuple("bbb_raw_1ch_8khz_s16le.raw", MR74, "bbb_raw_1ch_8khz_s16le_MR74_ref.amrnb"),
        make_tuple("bbb_raw_1ch_8khz_s16le.raw", MR795, "bbb_raw_1ch_8khz_s16le_MR795_ref.amrnb"),
        make_tuple("bbb_raw_1ch_8khz_s16le.raw", MR102, "bbb_raw_1ch_8khz_s16le_MR102_ref.amrnb"),
        make_tuple("bbb_raw_1ch_8khz_s16le.raw", MR122, "bbb_raw_1ch_8khz_s16le_MR122_ref.amrnb"),
        make_tuple("sinesweepraw.raw", MR475, "sinesweepraw_MR475_ref.amrnb"),
        make_tuple("sinesweepraw.raw", MR515, "sinesweepraw_MR515_ref.amrnb"),
        make_tuple("sinesweepraw.raw", MR59, "sinesweepraw_MR59_ref.amrnb"),
        make_tuple("sinesweepraw.raw", MR67, "sinesweepraw_MR67_ref.amrnb"),
        make_tuple("sinesweepraw.raw", MR74, "sinesweepraw_MR74_ref.amrnb"),
        make_tuple("sinesweepraw.raw", MR795, "sinesweepraw_MR795_ref.amrnb"),
        make_tuple("sinesweepraw.raw", MR102, "sinesweepraw_MR102_ref.amrnb"),
        make_tuple("sinesweepraw.raw", MR122, "sinesweepraw_MR122_ref.amrnb")));

int main(int argc, char **argv) {
    gEnv = new AmrnbEncTestEnvironment();
    ::testing::AddGlobalTestEnvironment(gEnv);
    ::testing::InitGoogleTest(&argc, argv);
    int status = gEnv->initFromOptions(argc, argv);
    if (status == 0) {
        status = RUN_ALL_TESTS();
        ALOGV("Test result = %d\n", status);
    }
    return status;
}
