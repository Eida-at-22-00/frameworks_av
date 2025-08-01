/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_NDEBUG 0
#define LOG_TAG "CameraCharacteristicsPermission"

#include <gtest/gtest.h>

#include <android/content/AttributionSourceState.h>
#include <binder/ProcessState.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <camera/CameraMetadata.h>
#include <camera/Camera.h>
#include <camera/CameraUtils.h>
#include <android/hardware/ICameraService.h>

using namespace android;
using namespace android::hardware;

class CameraCharacteristicsPermission : public ::testing::Test {
protected:
    CameraCharacteristicsPermission() : numCameras(0){}
    //Gtest interface
    void SetUp() override;
    void TearDown() override;

    int32_t numCameras;
    sp<ICameraService> mCameraService;
};

void CameraCharacteristicsPermission::SetUp() {
    ::android::binder::Status rc;
    ProcessState::self()->startThreadPool();
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.camera"));
    mCameraService = interface_cast<ICameraService>(binder);
    AttributionSourceState clientAttribution;
    clientAttribution.deviceId = kDefaultDeviceId;
    rc = mCameraService->getNumberOfCameras(
            hardware::ICameraService::CAMERA_TYPE_ALL, clientAttribution, /*devicePolicy*/0,
            &numCameras);
    EXPECT_TRUE(rc.isOk());
}

void CameraCharacteristicsPermission::TearDown() {
    mCameraService.clear();
}

// Revoking and acquiring permissions automatically might not be possible.
// Test the functionality for removal of camera characteristics needing
// a camera permission.
TEST_F(CameraCharacteristicsPermission, TestCameraPermission) {
    for (int32_t cameraId = 0; cameraId < numCameras; cameraId++) {
        std::string cameraIdStr = std::to_string(cameraId);

        CameraMetadata metadata;
        std::vector<int32_t> tagsNeedingPermission;
        AttributionSourceState clientAttribution;
        clientAttribution.deviceId = kDefaultDeviceId;
        auto rc = mCameraService->getCameraCharacteristics(cameraIdStr,
                /*targetSdkVersion*/__ANDROID_API_FUTURE__,
                /*overrideToPortrait*/false, clientAttribution, /*devicePolicy*/0, &metadata);
        ASSERT_TRUE(rc.isOk());
        EXPECT_FALSE(metadata.isEmpty());
        EXPECT_EQ(metadata.removePermissionEntries(CAMERA_METADATA_INVALID_VENDOR_ID,
                    &tagsNeedingPermission), NO_ERROR);
        camera_metadata_entry_t availableCharacteristics =
                metadata.find(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
        EXPECT_TRUE(0 < availableCharacteristics.count);

        std::vector<uint32_t> availableKeys;
        availableKeys.reserve(availableCharacteristics.count);
        availableKeys.insert(availableKeys.begin(), availableCharacteristics.data.i32,
                availableCharacteristics.data.i32 + availableCharacteristics.count);

        for (const auto &key : tagsNeedingPermission) {
            ASSERT_FALSE(metadata.exists(key));
            auto it = std::find(availableKeys.begin(), availableKeys.end(), key);
            ASSERT_TRUE(it == availableKeys.end());
        }
    }
}
