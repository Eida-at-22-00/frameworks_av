/*
 * Copyright (C) 2008 The Android Open Source Project
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

#define LOG_TAG "CameraService"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <algorithm>
#include <climits>
#include <stdio.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <inttypes.h>
#include <pthread.h>
#include <poll.h>

#include <android/hardware/ICamera.h>
#include <android/hardware/ICameraClient.h>

#include <aidl/AidlCameraService.h>
#include <android-base/macros.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/companion/virtualnative/IVirtualDeviceManagerNative.h>
#include <binder/ActivityManager.h>
#include <binder/AppOpsManager.h>
#include <binder/IPCThreadState.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <binder/PermissionController.h>
#include <binder/IResultReceiver.h>
#include <binderthreadstate/CallerUtils.h>
#include <com_android_internal_camera_flags.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <cutils/misc.h>
#include <gui/Surface.h>
#include <hardware/hardware.h>
#include "hidl/HidlCameraService.h"
#include <hidl/HidlTransportSupport.h>
#include <hwbinder/IPCThreadState.h>
#include <memunreachable/memunreachable.h>
#include <media/AudioSystem.h>
#include <media/IMediaHTTPService.h>
#include <media/mediaplayer.h>
#include <mediautils/BatteryNotifier.h>
#include <processinfo/ProcessInfoService.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/String16.h>
#include <utils/SystemClock.h>
#include <utils/Trace.h>
#include <utils/CallStack.h>
#include <private/android_filesystem_config.h>
#include <system/camera_vendor_tags.h>
#include <system/camera_metadata.h>
#include <binder/IServiceManager.h>
#include <binder/IActivityManager.h>
#include <camera/CameraUtils.h>
#include <camera/StringUtils.h>

#include <system/camera.h>

#include "CameraService.h"
#include "api1/Camera2Client.h"
#include "api2/CameraDeviceClient.h"
#include "utils/CameraServiceProxyWrapper.h"
#include "utils/CameraTraces.h"
#include "utils/SessionConfigurationUtils.h"
#include "utils/TagMonitor.h"
#include "utils/Utils.h"

namespace {
    const char* kActivityServiceName = "activity";
    const char* kSensorPrivacyServiceName = "sensor_privacy";
    const char* kAppopsServiceName = "appops";
    const char* kProcessInfoServiceName = "processinfo";
    const char* kVirtualDeviceBackCameraId = "0";
    const char* kVirtualDeviceFrontCameraId = "1";
    const char* kUnknownPackageName = "<unknown>";

    android::PermissionChecker::PermissionResult appOpModeToPermissionResult(int32_t res) {
        switch (res) {
            case android::AppOpsManager::MODE_ERRORED:
                return android::PermissionChecker::PERMISSION_HARD_DENIED;
            case android::AppOpsManager::MODE_IGNORED:
                return android::PermissionChecker::PERMISSION_SOFT_DENIED;
            case android::AppOpsManager::MODE_ALLOWED:
                return android::PermissionChecker::PERMISSION_GRANTED;
        }

        ALOGE("%s: Unexpected appOpMode %d", __FUNCTION__, res);
        return android::PermissionChecker::PERMISSION_HARD_DENIED;
    }
} // namespace anonymous

namespace android {

using base::SetProperty;
using namespace camera3;
using namespace camera3::SessionConfigurationUtils;

using binder::Status;
using companion::virtualnative::IVirtualDeviceManagerNative;
using frameworks::cameraservice::service::V2_0::implementation::HidlCameraService;
using frameworks::cameraservice::service::implementation::AidlCameraService;
using hardware::ICamera;
using hardware::ICameraClient;
using hardware::ICameraServiceListener;
using hardware::camera2::ICameraInjectionCallback;
using hardware::camera2::ICameraInjectionSession;
using hardware::camera2::utils::CameraIdAndSessionConfiguration;
using hardware::camera2::utils::ConcurrentCameraIdCombination;

namespace flags = com::android::internal::camera::flags;

// ----------------------------------------------------------------------------
// Logging support -- this is for debugging only
// Use "adb shell dumpsys media.camera -v 1" to change it.
volatile int32_t gLogLevel = 0;

#define LOG1(...) ALOGD_IF(gLogLevel >= 1, __VA_ARGS__);
#define LOG2(...) ALOGD_IF(gLogLevel >= 2, __VA_ARGS__);

static void setLogLevel(int level) {
    android_atomic_write(level, &gLogLevel);
}

int32_t format_as(CameraService::StatusInternal s) {
  return fmt::underlying(s);
}

// ----------------------------------------------------------------------------

// Permission strings (references to AttributionAndPermissionUtils for brevity)
static const std::string &sDumpPermission =
        AttributionAndPermissionUtils::sDumpPermission;
static const std::string &sManageCameraPermission =
        AttributionAndPermissionUtils::sManageCameraPermission;
static const std::string &sCameraSendSystemEventsPermission =
        AttributionAndPermissionUtils::sCameraSendSystemEventsPermission;
static const std::string &sCameraInjectExternalCameraPermission =
        AttributionAndPermissionUtils::sCameraInjectExternalCameraPermission;

// Constant integer for FGS Logging, used to denote the API type for logger
static const int LOG_FGS_CAMERA_API = 1;
const char *sFileName = "lastOpenSessionDumpFile";
static constexpr int32_t kSystemNativeClientScore = resource_policy::PERCEPTIBLE_APP_ADJ;
static constexpr int32_t kSystemNativeClientState =
        ActivityManager::PROCESS_STATE_PERSISTENT_UI;
static const std::string kServiceName("cameraserver");

const std::string CameraService::kOfflineDevice("offline-");
const std::string CameraService::kWatchAllClientsFlag("all");

constexpr int32_t kInvalidDeviceId = -1;

// Set to keep track of logged service error events.
static std::set<std::string> sServiceErrorEventSet;

// Current camera package name
static std::string sCurrPackageName;

CameraService::CameraService(
        std::shared_ptr<CameraServiceProxyWrapper> cameraServiceProxyWrapper,
        std::shared_ptr<AttributionAndPermissionUtils> attributionAndPermissionUtils) :
        AttributionAndPermissionUtilsEncapsulator(attributionAndPermissionUtils == nullptr ?
                std::make_shared<AttributionAndPermissionUtils>()\
                : attributionAndPermissionUtils),
        mCameraServiceProxyWrapper(cameraServiceProxyWrapper == nullptr ?
                std::make_shared<CameraServiceProxyWrapper>() : cameraServiceProxyWrapper),
        mEventLog(DEFAULT_EVENT_LOG_LENGTH),
        mNumberOfCameras(0),
        mNumberOfCamerasWithoutSystemCamera(0),
        mSoundRef(0), mInitialized(false),
        mAudioRestriction(hardware::camera2::ICameraDeviceUser::AUDIO_RESTRICTION_NONE) {
    ALOGI("CameraService started (pid=%d)", getpid());
    mAttributionAndPermissionUtils->setCameraService(this);
    mServiceLockWrapper = std::make_shared<WaitableMutexWrapper>(&mServiceLock);
    mMemFd = memfd_create(sFileName, MFD_ALLOW_SEALING);
    if (mMemFd == -1) {
        ALOGE("%s: Error while creating the file: %s", __FUNCTION__, sFileName);
    }
}

// Enable processes with isolated AID to request the binder
void CameraService::instantiate() {
    CameraService::publish(true);
}

void CameraService::onServiceRegistration(const String16& name, const sp<IBinder>&) {
    if (name != toString16(kAppopsServiceName)) {
        return;
    }

    ALOGV("appops service registered. setting camera audio restriction");
    mAppOps.setCameraAudioRestriction(mAudioRestriction);
}

void CameraService::onFirstRef()
{
    ALOGI("CameraService process starting");

    BnCameraService::onFirstRef();

    // Update battery life tracking if service is restarting
    BatteryNotifier& notifier(BatteryNotifier::getInstance());
    notifier.noteResetCamera();
    notifier.noteResetFlashlight();

    status_t res = INVALID_OPERATION;

    res = enumerateProviders();
    if (res == OK) {
        mInitialized = true;
    }

    mUidPolicy = new UidPolicy(this);
    mUidPolicy->registerSelf();
    mSensorPrivacyPolicy = new SensorPrivacyPolicy(this, mAttributionAndPermissionUtils);
    mSensorPrivacyPolicy->registerSelf();
    mInjectionStatusListener = new InjectionStatusListener(this);

    // appops function setCamerAudioRestriction uses getService which
    // is blocking till the appops service is ready. To enable early
    // boot availability for cameraservice, use checkService which is
    // non blocking and register for notifications
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->checkService(toString16(kAppopsServiceName));
    if (!binder) {
        sm->registerForNotifications(toString16(kAppopsServiceName), this);
    } else {
        mAppOps.setCameraAudioRestriction(mAudioRestriction);
    }

    sp<HidlCameraService> hcs = HidlCameraService::getInstance(this);
    if (hcs->registerAsService() != android::OK) {
        // Deprecated, so it will fail to register on newer devices
        ALOGW("%s: Did not register default android.frameworks.cameraservice.service@2.2",
              __FUNCTION__);
    }

    if (!AidlCameraService::registerService(this)) {
        ALOGE("%s: Failed to register default AIDL VNDK CameraService", __FUNCTION__);
    }

    // This needs to be last call in this function, so that it's as close to
    // ServiceManager::addService() as possible.
    mCameraServiceProxyWrapper->pingCameraServiceProxy();
    ALOGI("CameraService pinged cameraservice proxy");
}

status_t CameraService::enumerateProviders() {
    status_t res;

    std::vector<std::string> deviceIds;
    std::unordered_map<std::string, std::set<std::string>> unavailPhysicalIds;
    {
        Mutex::Autolock l(mServiceLock);

        if (nullptr == mCameraProviderManager.get()) {
            mCameraProviderManager = new CameraProviderManager();
            res = mCameraProviderManager->initialize(this);
            if (res != OK) {
                ALOGE("%s: Unable to initialize camera provider manager: %s (%d)",
                        __FUNCTION__, strerror(-res), res);
                logServiceError("Unable to initialize camera provider manager",
                        ERROR_DISCONNECTED);
                return res;
            }
        }

        // Setup vendor tags before we call get_camera_info the first time
        // because HAL might need to setup static vendor keys in get_camera_info
        // TODO: maybe put this into CameraProviderManager::initialize()?
        mCameraProviderManager->setUpVendorTags();

        if (nullptr == mFlashlight.get()) {
            mFlashlight = new CameraFlashlight(mCameraProviderManager, this);
        }

        res = mFlashlight->findFlashUnits();
        if (res != OK) {
            ALOGE("Failed to enumerate flash units: %s (%d)", strerror(-res), res);
        }

        deviceIds = mCameraProviderManager->getCameraDeviceIds(&unavailPhysicalIds);
    }

    for (auto& cameraId : deviceIds) {
        if (getCameraState(cameraId) == nullptr) {
            onDeviceStatusChanged(cameraId, CameraDeviceStatus::PRESENT);
        }
        if (unavailPhysicalIds.count(cameraId) > 0) {
            for (const auto& physicalId : unavailPhysicalIds[cameraId]) {
                onDeviceStatusChanged(cameraId, physicalId, CameraDeviceStatus::NOT_PRESENT);
            }
        }
    }

    // Derive primary rear/front cameras, and filter their charactierstics.
    // This needs to be done after all cameras are enumerated and camera ids are sorted.
    if (SessionConfigurationUtils::IS_PERF_CLASS) {
        // Assume internal cameras are advertised from the same
        // provider. If multiple providers are registered at different time,
        // and each provider contains multiple internal color cameras, the current
        // logic may filter the characteristics of more than one front/rear color
        // cameras.
        Mutex::Autolock l(mServiceLock);
        filterSPerfClassCharacteristicsLocked();
    }

    return OK;
}

void CameraService::broadcastTorchModeStatus(const std::string& cameraId, TorchModeStatus status,
        SystemCameraKind systemCameraKind) {
    // Get the device id and app-visible camera id for the given HAL-visible camera id.
    auto [deviceId, mappedCameraId] =
            mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(cameraId);

    Mutex::Autolock lock(mStatusListenerLock);
    for (auto& i : mListenerList) {
        if (shouldSkipStatusUpdates(systemCameraKind, i->isVendorListener(), i->getListenerPid(),
                i->getListenerUid())) {
            ALOGV("%s: Skipping torch callback for system-only camera device %s",
                    __FUNCTION__, cameraId.c_str());
            continue;
        }

        auto ret = i->getListener()->onTorchStatusChanged(mapToInterface(status),
                mappedCameraId, deviceId);
        i->handleBinderStatus(ret, "%s: Failed to trigger onTorchStatusChanged for %d:%d: %d",
                __FUNCTION__, i->getListenerUid(), i->getListenerPid(), ret.exceptionCode());
    }
}

CameraService::~CameraService() {
    VendorTagDescriptor::clearGlobalVendorTagDescriptor();
    mUidPolicy->unregisterSelf();
    mSensorPrivacyPolicy->unregisterSelf();
    mInjectionStatusListener->removeListener();
}

void CameraService::onNewProviderRegistered() {
    enumerateProviders();
}

void CameraService::filterAPI1SystemCameraLocked(
        const std::vector<std::string> &normalDeviceIds) {
    mNormalDeviceIdsWithoutSystemCamera.clear();
    for (auto &cameraId : normalDeviceIds) {
        CameraMetadata cameraInfo;
        status_t res = mCameraProviderManager->getCameraCharacteristics(
                cameraId, false, &cameraInfo,
                hardware::ICameraService::ROTATION_OVERRIDE_NONE);
        int32_t deviceId = kDefaultDeviceId;
        if (res != OK) {
            ALOGW("%s: Not able to get camera characteristics for camera id %s",
                  __FUNCTION__, cameraId.c_str());
        } else {
            deviceId = getDeviceId(cameraInfo);
        }
        // Cameras associated with non-default device id's (i.e., virtual cameras) can never be
        // system cameras, so skip for non-default device id's.
        if (deviceId != kDefaultDeviceId) {
            continue;
        }

        SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
        if (getSystemCameraKind(cameraId, &deviceKind) != OK) {
            ALOGE("%s: Invalid camera id %s, skipping", __FUNCTION__, cameraId.c_str());
            continue;
        }
        if (deviceKind == SystemCameraKind::SYSTEM_ONLY_CAMERA) {
            // All system camera ids will necessarily come after public camera
            // device ids as per the HAL interface contract.
            break;
        }
        mNormalDeviceIdsWithoutSystemCamera.push_back(cameraId);
    }
    ALOGV("%s: number of API1 compatible public cameras is %zu", __FUNCTION__,
              mNormalDeviceIdsWithoutSystemCamera.size());
}

status_t CameraService::getSystemCameraKind(const std::string& cameraId,
        SystemCameraKind *kind) const {
    auto state = getCameraState(cameraId);
    if (state != nullptr) {
        *kind = state->getSystemCameraKind();
        return OK;
    }
    // Hidden physical camera ids won't have CameraState
    return mCameraProviderManager->getSystemCameraKind(cameraId, kind);
}

void CameraService::updateCameraNumAndIds() {
    Mutex::Autolock l(mServiceLock);
    std::pair<int, int> systemAndNonSystemCameras = mCameraProviderManager->getCameraCount();
    // Excludes hidden secure cameras
    mNumberOfCameras =
            systemAndNonSystemCameras.first + systemAndNonSystemCameras.second;
    mNumberOfCamerasWithoutSystemCamera = systemAndNonSystemCameras.second;
    mNormalDeviceIds =
            mCameraProviderManager->getAPI1CompatibleCameraDeviceIds();
    filterAPI1SystemCameraLocked(mNormalDeviceIds);
}

void CameraService::filterSPerfClassCharacteristicsLocked() {
    // To claim to be S Performance primary cameras, the cameras must be
    // backward compatible. So performance class primary camera Ids must be API1
    // compatible.
    bool firstRearCameraSeen = false, firstFrontCameraSeen = false;
    for (const auto& cameraId : mNormalDeviceIdsWithoutSystemCamera) {
        int facing = -1;
        int orientation = 0;
        int portraitRotation;
        getDeviceVersion(cameraId,
                /*rotationOverride*/hardware::ICameraService::ROTATION_OVERRIDE_NONE,
                /*out*/&portraitRotation, /*out*/&facing, /*out*/&orientation);
        if (facing == -1) {
            ALOGE("%s: Unable to get camera device \"%s\" facing", __FUNCTION__, cameraId.c_str());
            return;
        }

        if ((facing == hardware::CAMERA_FACING_BACK && !firstRearCameraSeen) ||
                (facing == hardware::CAMERA_FACING_FRONT && !firstFrontCameraSeen)) {
            status_t res = mCameraProviderManager->filterSmallJpegSizes(cameraId);
            if (res == OK) {
                mPerfClassPrimaryCameraIds.insert(cameraId);
            } else {
                ALOGE("%s: Failed to filter small JPEG sizes for performance class primary "
                        "camera %s: %s(%d)", __FUNCTION__, cameraId.c_str(), strerror(-res), res);
                break;
            }

            if (facing == hardware::CAMERA_FACING_BACK) {
                firstRearCameraSeen = true;
            }
            if (facing == hardware::CAMERA_FACING_FRONT) {
                firstFrontCameraSeen = true;
            }
        }

        if (firstRearCameraSeen && firstFrontCameraSeen) {
            break;
        }
    }
}

void CameraService::addStates(const std::string& cameraId) {
    CameraResourceCost cost;
    status_t res = mCameraProviderManager->getResourceCost(cameraId, &cost);
    if (res != OK) {
        ALOGE("Failed to query device resource cost: %s (%d)", strerror(-res), res);
        return;
    }
    SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
    res = mCameraProviderManager->getSystemCameraKind(cameraId, &deviceKind);
    if (res != OK) {
        ALOGE("Failed to query device kind: %s (%d)", strerror(-res), res);
        return;
    }
    std::vector<std::string> physicalCameraIds;
    mCameraProviderManager->isLogicalCamera(cameraId, &physicalCameraIds);
    std::set<std::string> conflicting;
    for (size_t i = 0; i < cost.conflictingDevices.size(); i++) {
        conflicting.emplace(cost.conflictingDevices[i]);
    }

    {
        Mutex::Autolock lock(mCameraStatesLock);
        mCameraStates.emplace(cameraId, std::make_shared<CameraState>(cameraId, cost.resourceCost,
                conflicting, deviceKind, physicalCameraIds));
    }

    if (mFlashlight->hasFlashUnit(cameraId)) {
        Mutex::Autolock al(mTorchStatusMutex);
        mTorchStatusMap.add(cameraId, TorchModeStatus::AVAILABLE_OFF);

        broadcastTorchModeStatus(cameraId, TorchModeStatus::AVAILABLE_OFF, deviceKind);
    }

    updateCameraNumAndIds();
    logDeviceAdded(cameraId, "Device added");
}

void CameraService::removeStates(const std::string& cameraId) {
    updateCameraNumAndIds();
    if (mFlashlight->hasFlashUnit(cameraId)) {
        Mutex::Autolock al(mTorchStatusMutex);
        mTorchStatusMap.removeItem(cameraId);
    }

    {
        Mutex::Autolock lock(mCameraStatesLock);
        mCameraStates.erase(cameraId);
    }
}

void CameraService::onDeviceStatusChanged(const std::string& cameraId,
        CameraDeviceStatus newHalStatus) {
    ALOGI("%s: Status changed for cameraId=%s, newStatus=%d", __FUNCTION__,
            cameraId.c_str(), eToI(newHalStatus));

    StatusInternal newStatus = mapToInternal(newHalStatus);

    std::shared_ptr<CameraState> state = getCameraState(cameraId);

    if (state == nullptr) {
        if (newStatus == StatusInternal::PRESENT) {
            ALOGI("%s: Unknown camera ID %s, a new camera is added",
                    __FUNCTION__, cameraId.c_str());

            // First add as absent to make sure clients are notified below
            addStates(cameraId);

            updateStatus(newStatus, cameraId);
        } else {
            ALOGE("%s: Bad camera ID %s", __FUNCTION__, cameraId.c_str());
        }
        return;
    }

    StatusInternal oldStatus = state->getStatus();

    if (oldStatus == newStatus) {
        ALOGE("%s: State transition to the same status %#x not allowed", __FUNCTION__,
                eToI(newStatus));
        return;
    }

    if (newStatus == StatusInternal::NOT_PRESENT) {
        logDeviceRemoved(cameraId, fmt::format("Device status changed from {} to {}",
                oldStatus, newStatus));
        // Set the device status to NOT_PRESENT, clients will no longer be able to connect
        // to this device until the status changes
        updateStatus(StatusInternal::NOT_PRESENT, cameraId);
        mVirtualDeviceCameraIdMapper.removeCamera(cameraId);

        std::vector<sp<BasicClient>> clientsToDisconnectOnline, clientsToDisconnectOffline;
        {
            // Don't do this in updateStatus to avoid deadlock over mServiceLock
            Mutex::Autolock lock(mServiceLock);

            // Remove cached shim parameters
            state->setShimParams(CameraParameters());

            // Remove online as well as offline client from the list of active clients,
            // if they are present
            clientsToDisconnectOnline = removeClientsLocked(cameraId);
            clientsToDisconnectOffline = removeClientsLocked(kOfflineDevice + cameraId);
        }

        disconnectClients(cameraId, clientsToDisconnectOnline);
        disconnectClients(kOfflineDevice + cameraId, clientsToDisconnectOffline);

        removeStates(cameraId);
    } else {
        if (oldStatus == StatusInternal::NOT_PRESENT) {
            logDeviceAdded(cameraId, fmt::format("Device status changed from {} to {}",
                    oldStatus, newStatus));
        }
        updateStatus(newStatus, cameraId);
    }
}

void CameraService::onDeviceStatusChanged(const std::string& id,
        const std::string& physicalId,
        CameraDeviceStatus newHalStatus) {
    ALOGI("%s: Status changed for cameraId=%s, physicalCameraId=%s, newStatus=%d",
            __FUNCTION__, id.c_str(), physicalId.c_str(), eToI(newHalStatus));

    StatusInternal newStatus = mapToInternal(newHalStatus);

    std::shared_ptr<CameraState> state = getCameraState(id);

    if (state == nullptr) {
        ALOGE("%s: Physical camera id %s status change on a non-present ID %s",
                __FUNCTION__, physicalId.c_str(), id.c_str());
        return;
    }

    StatusInternal logicalCameraStatus = state->getStatus();
    if (logicalCameraStatus != StatusInternal::PRESENT &&
            logicalCameraStatus != StatusInternal::NOT_AVAILABLE) {
        ALOGE("%s: Physical camera id %s status %d change for an invalid logical camera state %d",
                __FUNCTION__, physicalId.c_str(), eToI(newHalStatus), eToI(logicalCameraStatus));
        return;
    }

    bool updated = false;
    if (newStatus == StatusInternal::PRESENT) {
        updated = state->removeUnavailablePhysicalId(physicalId);
    } else {
        updated = state->addUnavailablePhysicalId(physicalId);
    }

    if (updated) {
        std::string idCombo = id + " : " + physicalId;
        if (newStatus == StatusInternal::PRESENT) {
            logDeviceAdded(idCombo, fmt::format("Device status changed to {}", newStatus));
        } else {
            logDeviceRemoved(idCombo, fmt::format("Device status changed to {}", newStatus));
        }
        // Avoid calling getSystemCameraKind() with mStatusListenerLock held (b/141756275)
        SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
        if (getSystemCameraKind(id, &deviceKind) != OK) {
            ALOGE("%s: Invalid camera id %s, skipping", __FUNCTION__, id.c_str());
            return;
        }
        Mutex::Autolock lock(mStatusListenerLock);
        for (auto& listener : mListenerList) {
            if (shouldSkipStatusUpdates(deviceKind, listener->isVendorListener(),
                    listener->getListenerPid(), listener->getListenerUid())) {
                ALOGV("Skipping discovery callback for system-only camera device %s",
                        id.c_str());
                continue;
            }
            auto ret = listener->getListener()->onPhysicalCameraStatusChanged(
                    mapToInterface(newStatus), id, physicalId, kDefaultDeviceId);
            listener->handleBinderStatus(ret,
                    "%s: Failed to trigger onPhysicalCameraStatusChanged for %d:%d: %d",
                    __FUNCTION__, listener->getListenerUid(), listener->getListenerPid(),
                    ret.exceptionCode());
        }
    }
}

void CameraService::disconnectClients(const std::string& id,
        std::vector<sp<BasicClient>> clientsToDisconnect) {
    for (auto& client : clientsToDisconnect) {
        disconnectClient(id, client);
    }
}

void CameraService::disconnectClient(const std::string& id, sp<BasicClient> clientToDisconnect) {
    if (clientToDisconnect.get() != nullptr) {
        ALOGI("%s: Client for camera ID %s evicted due to device status change from HAL",
                __FUNCTION__, id.c_str());
        // Notify the client of disconnection
        clientToDisconnect->notifyError(
                hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                CaptureResultExtras{});
        clientToDisconnect->disconnect();
    }
}

void CameraService::onTorchStatusChanged(const std::string& cameraId,
        TorchModeStatus newStatus) {
    SystemCameraKind systemCameraKind = SystemCameraKind::PUBLIC;
    status_t res = getSystemCameraKind(cameraId, &systemCameraKind);
    if (res != OK) {
        ALOGE("%s: Could not get system camera kind for camera id %s", __FUNCTION__,
                cameraId.c_str());
        return;
    }
    Mutex::Autolock al(mTorchStatusMutex);
    onTorchStatusChangedLocked(cameraId, newStatus, systemCameraKind);
}

void CameraService::onTorchStatusChanged(const std::string& cameraId,
        TorchModeStatus newStatus, SystemCameraKind systemCameraKind) {
    Mutex::Autolock al(mTorchStatusMutex);
    onTorchStatusChangedLocked(cameraId, newStatus, systemCameraKind);
}

void CameraService::broadcastTorchStrengthLevel(const std::string& cameraId,
        int32_t newStrengthLevel) {
    // Get the device id and app-visible camera id for the given HAL-visible camera id.
    auto [deviceId, mappedCameraId] =
            mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(cameraId);

    Mutex::Autolock lock(mStatusListenerLock);
    for (auto& i : mListenerList) {
        auto ret = i->getListener()->onTorchStrengthLevelChanged(mappedCameraId,
                newStrengthLevel, deviceId);
        i->handleBinderStatus(ret,
                "%s: Failed to trigger onTorchStrengthLevelChanged for %d:%d: %d", __FUNCTION__,
                i->getListenerUid(), i->getListenerPid(), ret.exceptionCode());
    }
}

void CameraService::onTorchStatusChangedLocked(const std::string& cameraId,
        TorchModeStatus newStatus, SystemCameraKind systemCameraKind) {
    ALOGI("%s: Torch status changed for cameraId=%s, newStatus=%d",
            __FUNCTION__, cameraId.c_str(), eToI(newStatus));

    TorchModeStatus status;
    status_t res = getTorchStatusLocked(cameraId, &status);
    if (res) {
        ALOGE("%s: cannot get torch status of camera %s: %s (%d)",
                __FUNCTION__, cameraId.c_str(), strerror(-res), res);
        return;
    }
    if (status == newStatus) {
        return;
    }

    res = setTorchStatusLocked(cameraId, newStatus);
    if (res) {
        ALOGE("%s: Failed to set the torch status to %d: %s (%d)", __FUNCTION__,
                (uint32_t)newStatus, strerror(-res), res);
        return;
    }

    {
        // Update battery life logging for flashlight
        Mutex::Autolock al(mTorchUidMapMutex);
        auto iter = mTorchUidMap.find(cameraId);
        if (iter != mTorchUidMap.end()) {
            int oldUid = iter->second.second;
            int newUid = iter->second.first;
            BatteryNotifier& notifier(BatteryNotifier::getInstance());
            if (oldUid != newUid) {
                // If the UID has changed, log the status and update current UID in mTorchUidMap
                if (status == TorchModeStatus::AVAILABLE_ON) {
                    notifier.noteFlashlightOff(toString8(cameraId), oldUid);
                }
                if (newStatus == TorchModeStatus::AVAILABLE_ON) {
                    notifier.noteFlashlightOn(toString8(cameraId), newUid);
                }
                iter->second.second = newUid;
            } else {
                // If the UID has not changed, log the status
                if (newStatus == TorchModeStatus::AVAILABLE_ON) {
                    notifier.noteFlashlightOn(toString8(cameraId), oldUid);
                } else {
                    notifier.noteFlashlightOff(toString8(cameraId), oldUid);
                }
            }
        }
    }
    broadcastTorchModeStatus(cameraId, newStatus, systemCameraKind);
}

bool CameraService::isAutomotiveExteriorSystemCamera(const std::string& cam_id) const {
    // Returns false if this is not an automotive device type.
    if (!isAutomotiveDevice())
        return false;

    // Returns false if no camera id is provided.
    if (cam_id.empty())
        return false;

    SystemCameraKind systemCameraKind = SystemCameraKind::PUBLIC;
    if (getSystemCameraKind(cam_id, &systemCameraKind) != OK) {
        // This isn't a known camera ID, so it's not a system camera.
        ALOGE("%s: Unknown camera id %s, ", __FUNCTION__, cam_id.c_str());
        return false;
    }

    if (systemCameraKind != SystemCameraKind::SYSTEM_ONLY_CAMERA) {
        ALOGE("%s: camera id %s is not a system camera", __FUNCTION__, cam_id.c_str());
        return false;
    }

    CameraMetadata cameraInfo;
    status_t res = mCameraProviderManager->getCameraCharacteristics(
            cam_id, false, &cameraInfo, hardware::ICameraService::ROTATION_OVERRIDE_NONE);
    if (res != OK){
        ALOGE("%s: Not able to get camera characteristics for camera id %s",__FUNCTION__,
                cam_id.c_str());
        return false;
    }

    camera_metadata_entry auto_location  = cameraInfo.find(ANDROID_AUTOMOTIVE_LOCATION);
    if (auto_location.count != 1)
        return false;

    uint8_t location = auto_location.data.u8[0];
    if ((location != ANDROID_AUTOMOTIVE_LOCATION_EXTERIOR_FRONT) &&
            (location != ANDROID_AUTOMOTIVE_LOCATION_EXTERIOR_REAR) &&
            (location != ANDROID_AUTOMOTIVE_LOCATION_EXTERIOR_LEFT) &&
            (location != ANDROID_AUTOMOTIVE_LOCATION_EXTERIOR_RIGHT)) {
        return false;
    }

    return true;
}

Status CameraService::getNumberOfCameras(int32_t type,
        const AttributionSourceState& clientAttribution, int32_t devicePolicy,
        int32_t* numCameras) {
    ATRACE_CALL();
    if (clientAttribution.deviceId != kDefaultDeviceId
            && (devicePolicy != IVirtualDeviceManagerNative::DEVICE_POLICY_DEFAULT)) {
        *numCameras = mVirtualDeviceCameraIdMapper.getNumberOfCameras(clientAttribution.deviceId);
        return Status::ok();
    }

    Mutex::Autolock l(mServiceLock);
    bool hasSystemCameraPermissions =
            hasPermissionsForSystemCamera(std::string(), getCallingPid(),
                    getCallingUid());
    switch (type) {
        case CAMERA_TYPE_BACKWARD_COMPATIBLE:
            if (hasSystemCameraPermissions) {
                *numCameras = static_cast<int>(mNormalDeviceIds.size());
            } else {
                *numCameras = static_cast<int>(mNormalDeviceIdsWithoutSystemCamera.size());
            }
            break;
        case CAMERA_TYPE_ALL:
            if (hasSystemCameraPermissions) {
                *numCameras = mNumberOfCameras;
            } else {
                *numCameras = mNumberOfCamerasWithoutSystemCamera;
            }
            break;
        default:
            ALOGW("%s: Unknown camera type %d",
                    __FUNCTION__, type);
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Unknown camera type %d", type);
    }
    return Status::ok();
}

Status CameraService::createDefaultRequest(const std::string& unresolvedCameraId, int templateId,
        const AttributionSourceState& clientAttribution, int32_t devicePolicy,
        /* out */
        hardware::camera2::impl::CameraMetadataNative* request) {
    ATRACE_CALL();

    if (!mInitialized) {
        ALOGE("%s: Camera subsystem is not available", __FUNCTION__);
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera subsystem is not available");
    }

    std::optional<std::string> cameraIdOptional =
            resolveCameraId(unresolvedCameraId, clientAttribution.deviceId, devicePolicy);
    if (!cameraIdOptional.has_value()) {
        std::string msg = fmt::sprintf("Camera %s: Invalid camera id for device id %d",
                unresolvedCameraId.c_str(), clientAttribution.deviceId);
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    std::string cameraId = cameraIdOptional.value();

    binder::Status res;
    if (request == nullptr) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %s: Error creating default request", cameraId.c_str());
        return res;
    }
    camera_request_template_t tempId = camera_request_template_t::CAMERA_TEMPLATE_COUNT;
    res = SessionConfigurationUtils::mapRequestTemplateFromClient(
            cameraId, templateId, &tempId);
    if (!res.isOk()) {
        ALOGE("%s: Camera %s: failed to map request Template %d",
                __FUNCTION__, cameraId.c_str(), templateId);
        return res;
    }

    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to create default"
                "request for system only device %s: ", cameraId.c_str());
    }

    CameraMetadata metadata;
    status_t err = mCameraProviderManager->createDefaultRequest(cameraId, tempId, &metadata);
    if (err == OK) {
        request->swap(metadata);
    } else if (err == BAD_VALUE) {
        res = STATUS_ERROR_FMT(CameraService::ERROR_ILLEGAL_ARGUMENT,
                "Camera %s: Template ID %d is invalid or not supported: %s (%d)",
                cameraId.c_str(), templateId, strerror(-err), err);
    } else {
        res = STATUS_ERROR_FMT(CameraService::ERROR_INVALID_OPERATION,
                "Camera %s: Error creating default request for template %d: %s (%d)",
                cameraId.c_str(), templateId, strerror(-err), err);
    }
    return res;
}

Status CameraService::isSessionConfigurationWithParametersSupported(
        const std::string& unresolvedCameraId, int targetSdkVersion,
        const SessionConfiguration& sessionConfiguration,
        const AttributionSourceState& clientAttribution, int32_t devicePolicy,
        /*out*/ bool* supported) {
    ATRACE_CALL();

    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera subsystem is not available");
    }

    std::optional<std::string> cameraIdOptional =
            resolveCameraId(unresolvedCameraId, clientAttribution.deviceId, devicePolicy);
    if (!cameraIdOptional.has_value()) {
        std::string msg = fmt::sprintf("Camera %s: Invalid camera id for device id %d",
                unresolvedCameraId.c_str(), clientAttribution.deviceId);
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    std::string cameraId = cameraIdOptional.value();

    if (supported == nullptr) {
        std::string msg = fmt::sprintf("Camera %s: Invalid 'support' input!",
                unresolvedCameraId.c_str());
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }

    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to query "
                "session configuration with parameters support for system only device %s: ",
                cameraId.c_str());
    }

    bool overrideForPerfClass = SessionConfigurationUtils::targetPerfClassPrimaryCamera(
                                        mPerfClassPrimaryCameraIds, cameraId, targetSdkVersion);

    auto ret = isSessionConfigurationWithParametersSupportedUnsafe(cameraId,
            sessionConfiguration, overrideForPerfClass, supported);
    if (flags::analytics_24q3()) {
        mCameraServiceProxyWrapper->logFeatureCombinationQuery(cameraId,
                getCallingUid(), sessionConfiguration, ret);
    }
    return ret;
}

Status CameraService::isSessionConfigurationWithParametersSupportedUnsafe(
        const std::string& cameraId, const SessionConfiguration& sessionConfiguration,
        bool overrideForPerfClass, /*out*/ bool* supported) {
    *supported = false;
    status_t ret = mCameraProviderManager->isSessionConfigurationSupported(
            cameraId, sessionConfiguration, overrideForPerfClass,
            /*checkSessionParams=*/true, supported);
    binder::Status res;
    switch (ret) {
        case OK:
            // Expected. Do Nothing.
            return Status::ok();
        case INVALID_OPERATION: {
                std::string msg = fmt::sprintf(
                        "Camera %s: Session configuration with parameters supported query not "
                        "supported!",
                        cameraId.c_str());
                ALOGW("%s: %s", __FUNCTION__, msg.c_str());
                logServiceError(msg, CameraService::ERROR_INVALID_OPERATION);
                *supported = false;
                return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.c_str());
            }
            break;
        case NAME_NOT_FOUND: {
                std::string msg = fmt::sprintf("Camera %s: Unknown camera ID.", cameraId.c_str());
                ALOGW("%s: %s", __FUNCTION__, msg.c_str());
                logServiceError(msg, CameraService::ERROR_ILLEGAL_ARGUMENT);
                *supported = false;
                return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
            }
            break;
        default: {
                std::string msg = fmt::sprintf(
                        "Unable to retrieve session configuration support for camera "
                        "device %s: Error: %s (%d)",
                        cameraId.c_str(), strerror(-ret), ret);
                ALOGE("%s: %s", __FUNCTION__, msg.c_str());
                logServiceError(msg, CameraService::ERROR_ILLEGAL_ARGUMENT);
                *supported = false;
                return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
            }
            break;
    }
}

Status CameraService::getSessionCharacteristics(const std::string& unresolvedCameraId,
        int targetSdkVersion, int rotationOverride,
        const SessionConfiguration& sessionConfiguration,
        const AttributionSourceState& clientAttribution, int32_t devicePolicy,
        /*out*/ CameraMetadata* outMetadata) {
    ATRACE_CALL();

    if (outMetadata == nullptr) {
        std::string msg =
                fmt::sprintf("Camera %s: Invalid 'outMetadata' input!", unresolvedCameraId.c_str());
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }

    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera subsystem is not available");
    }

    std::optional<std::string> cameraIdOptional =
            resolveCameraId(unresolvedCameraId, clientAttribution.deviceId, devicePolicy);
    if (!cameraIdOptional.has_value()) {
        std::string msg = fmt::sprintf("Camera %s: Invalid camera id for device id %d",
                                       unresolvedCameraId.c_str(), clientAttribution.deviceId);
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    std::string cameraId = cameraIdOptional.value();

    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                                "Unable to retrieve camera"
                                "characteristics for system only device %s: ",
                                cameraId.c_str());
    }

    bool overrideForPerfClass = SessionConfigurationUtils::targetPerfClassPrimaryCamera(
            mPerfClassPrimaryCameraIds, cameraId, targetSdkVersion);

    bool sessionConfigSupported;
    Status res = isSessionConfigurationWithParametersSupportedUnsafe(
            cameraId, sessionConfiguration, overrideForPerfClass, &sessionConfigSupported);
    if (!res.isOk()) {
        // isSessionConfigurationWithParametersSupportedUnsafe should log what went wrong and
        // report the correct Status to send to the client. Simply forward the error to
        // the client.
        outMetadata->clear();
        return res;
    }

    if (!sessionConfigSupported) {
        std::string msg = fmt::sprintf("Session configuration not supported for camera device %s.",
                                       cameraId.c_str());
        outMetadata->clear();
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }

    status_t ret = mCameraProviderManager->getSessionCharacteristics(
            cameraId, sessionConfiguration, overrideForPerfClass, rotationOverride, outMetadata);

    switch (ret) {
        case OK:
            // Expected, no handling needed.
            break;
        case INVALID_OPERATION: {
                std::string msg = fmt::sprintf(
                        "Camera %s: Session characteristics query not supported!",
                        cameraId.c_str());
                ALOGW("%s: %s", __FUNCTION__, msg.c_str());
                logServiceError(msg, CameraService::ERROR_INVALID_OPERATION);
                outMetadata->clear();
                return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.c_str());
            }
            break;
        case NAME_NOT_FOUND: {
                std::string msg = fmt::sprintf(
                        "Camera %s: Unknown camera ID.",
                        cameraId.c_str());
                ALOGW("%s: %s", __FUNCTION__, msg.c_str());
                logServiceError(msg, CameraService::ERROR_ILLEGAL_ARGUMENT);
                outMetadata->clear();
                return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
            }
            break;
        default: {
                std::string msg = fmt::sprintf(
                        "Unable to retrieve session characteristics for camera device %s: "
                        "Error: %s (%d)",
                        cameraId.c_str(), strerror(-ret), ret);
                ALOGE("%s: %s", __FUNCTION__, msg.c_str());
                logServiceError(msg, CameraService::ERROR_INVALID_OPERATION);
                outMetadata->clear();
                return STATUS_ERROR(CameraService::ERROR_INVALID_OPERATION, msg.c_str());
            }
    }

    res = filterSensitiveMetadataIfNeeded(cameraId, outMetadata);
    if (flags::analytics_24q3()) {
        mCameraServiceProxyWrapper->logSessionCharacteristicsQuery(cameraId,
                getCallingUid(), sessionConfiguration, res);
    }
    return res;
}

Status CameraService::filterSensitiveMetadataIfNeeded(
        const std::string& cameraId, CameraMetadata* metadata) {
    int callingPid = getCallingPid();
    int callingUid = getCallingUid();

    if (callingPid == getpid()) {
        // Caller is cameraserver; no need to remove keys
        return Status::ok();
    }

    SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
    if (getSystemCameraKind(cameraId, &deviceKind) != OK) {
        ALOGE("%s: Couldn't get camera kind for camera id %s", __FUNCTION__, cameraId.c_str());
        metadata->clear();
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                                "Unable to retrieve camera kind for device %s", cameraId.c_str());
    }
    if (deviceKind == SystemCameraKind::SYSTEM_ONLY_CAMERA) {
        // Attempting to query system only camera without system camera permission would have
        // failed the shouldRejectSystemCameraConnection in the caller. So if we get here
        // for a system only camera, then the caller has the required permission.
        // No need to remove keys
        return Status::ok();
    }

    std::vector<int32_t> tagsRemoved;
    // Get the device id that owns this camera.
    auto [cameraOwnerDeviceId, _] = mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(
            cameraId);
    bool hasCameraPermission = hasPermissionsForCamera(cameraId, callingPid, callingUid,
            cameraOwnerDeviceId);
    if (hasCameraPermission) {
        // Caller has camera permission; no need to remove keys
        return Status::ok();
    }

    status_t ret = metadata->removePermissionEntries(
            mCameraProviderManager->getProviderTagIdLocked(cameraId), &tagsRemoved);
    if (ret != OK) {
        metadata->clear();
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                                "Failed to remove camera characteristics needing camera permission "
                                "for device %s:%s (%d)",
                                cameraId.c_str(), strerror(-ret), ret);
    }

    if (!tagsRemoved.empty()) {
        ret = metadata->update(ANDROID_REQUEST_CHARACTERISTIC_KEYS_NEEDING_PERMISSION,
                                  tagsRemoved.data(), tagsRemoved.size());
        if (ret != OK) {
            metadata->clear();
            return STATUS_ERROR_FMT(
                    ERROR_INVALID_OPERATION,
                    "Failed to insert camera keys needing permission for device %s: %s (%d)",
                    cameraId.c_str(), strerror(-ret), ret);
        }
    }
    return Status::ok();
}

Status CameraService::injectSessionParams(
        const std::string& cameraId,
        const CameraMetadata& sessionParams) {
    if (!checkCallingPermission(toString16(sCameraInjectExternalCameraPermission))) {
        const int pid = getCallingPid();
        const int uid = getCallingUid();
        ALOGE("%s: Permission Denial: can't inject session params pid=%d, uid=%d",
                __FUNCTION__, pid, uid);
        return STATUS_ERROR(ERROR_PERMISSION_DENIED,
                "Permission Denial: no permission to inject session params");
    }

    // Do not allow session params injection for a virtual camera.
    auto [deviceId, _] = mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(cameraId);
    if (deviceId != kDefaultDeviceId) {
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                "Cannot inject session params for a virtual camera");
    }

    std::unique_ptr<AutoConditionLock> serviceLockWrapper =
            AutoConditionLock::waitAndAcquire(mServiceLockWrapper);

    auto clientDescriptor = mActiveClientManager.get(cameraId);
    if (clientDescriptor == nullptr) {
        ALOGI("%s: No active client for camera id %s", __FUNCTION__, cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "No active client for camera id %s", cameraId.c_str());
    }

    sp<BasicClient> clientSp = clientDescriptor->getValue();
    status_t res = clientSp->injectSessionParams(sessionParams);

    if (res != OK) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "Error injecting session params into camera \"%s\": %s (%d)",
                cameraId.c_str(), strerror(-res), res);
    }
    return Status::ok();
}

std::optional<std::string> CameraService::resolveCameraId(
        const std::string& inputCameraId,
        int32_t deviceId,
        int32_t devicePolicy) {
    if ((deviceId == kDefaultDeviceId)
            || (devicePolicy == IVirtualDeviceManagerNative::DEVICE_POLICY_DEFAULT)) {
        auto [storedDeviceId, _] =
                mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(inputCameraId);
        if (storedDeviceId != kDefaultDeviceId) {
            // Trying to access a virtual camera from default-policy device context, we should fail.
            std::string msg = fmt::sprintf("Camera %s: Invalid camera id for device id %d",
                    inputCameraId.c_str(), deviceId);
            ALOGE("%s: %s", __FUNCTION__, msg.c_str());
            return std::nullopt;
        }
        return inputCameraId;
    }

    return mVirtualDeviceCameraIdMapper.getActualCameraId(deviceId, inputCameraId);
}

Status CameraService::getCameraInfo(int cameraId,  int rotationOverride,
        const AttributionSourceState& clientAttribution, int32_t devicePolicy,
        CameraInfo* cameraInfo) {
    ATRACE_CALL();
    Mutex::Autolock l(mServiceLock);
    std::string cameraIdStr =
            cameraIdIntToStrLocked(cameraId, clientAttribution.deviceId, devicePolicy);
    if (cameraIdStr.empty()) {
        std::string msg = fmt::sprintf("Camera %d: Invalid camera id for device id %d",
                cameraId, clientAttribution.deviceId);
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }

    if (shouldRejectSystemCameraConnection(cameraIdStr)) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to retrieve camera"
                "characteristics for system only device %s: ", cameraIdStr.c_str());
    }

    if (!mInitialized) {
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem is not available");
    }
    bool hasSystemCameraPermissions = hasPermissionsForSystemCamera(std::to_string(cameraId),
            getCallingPid(), getCallingUid());
    int cameraIdBound = mNumberOfCamerasWithoutSystemCamera;
    if (hasSystemCameraPermissions) {
        cameraIdBound = mNumberOfCameras;
    }
    if (cameraId < 0 || cameraId >= cameraIdBound) {
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                "CameraId is not valid");
    }

    Status ret = Status::ok();
    int portraitRotation;
    status_t err = mCameraProviderManager->getCameraInfo(
            cameraIdStr, rotationOverride, &portraitRotation, cameraInfo);
    if (err != OK) {
        ret = STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "Error retrieving camera info from device %d: %s (%d)", cameraId,
                strerror(-err), err);
        logServiceError(std::string("Error retrieving camera info from device ")
                + std::to_string(cameraId), ERROR_INVALID_OPERATION);
    }

    return ret;
}

std::string CameraService::cameraIdIntToStrLocked(int cameraIdInt,
        int32_t deviceId, int32_t devicePolicy) {
    if (deviceId != kDefaultDeviceId
            && (devicePolicy != IVirtualDeviceManagerNative::DEVICE_POLICY_DEFAULT)) {
        std::optional<std::string> cameraIdOptional =
                mVirtualDeviceCameraIdMapper.getActualCameraId(cameraIdInt, deviceId);
        return cameraIdOptional.has_value() ? cameraIdOptional.value() : std::string{};
    }

    const std::vector<std::string> *cameraIds = &mNormalDeviceIdsWithoutSystemCamera;
    auto callingPid = getCallingPid();
    auto callingUid = getCallingUid();
    bool systemCameraPermissions = hasPermissionsForSystemCamera(std::to_string(cameraIdInt),
            callingPid, callingUid, /* checkCameraPermissions= */ false);
    if (systemCameraPermissions || getpid() == callingPid) {
        cameraIds = &mNormalDeviceIds;
    }
    if (cameraIdInt < 0 || cameraIdInt >= static_cast<int>(cameraIds->size())) {
        ALOGE("%s: input id %d invalid: valid range (0, %zu)",
                __FUNCTION__, cameraIdInt, cameraIds->size());
        return std::string{};
    }

    return (*cameraIds)[cameraIdInt];
}

std::string CameraService::cameraIdIntToStr(int cameraIdInt, int32_t deviceId,
        int32_t devicePolicy) {
    Mutex::Autolock lock(mServiceLock);
    return cameraIdIntToStrLocked(cameraIdInt, deviceId, devicePolicy);
}

Status CameraService::getCameraCharacteristics(const std::string& unresolvedCameraId,
        int targetSdkVersion, int rotationOverride, const AttributionSourceState& clientAttribution,
        int32_t devicePolicy, CameraMetadata* cameraInfo) {
    ATRACE_CALL();

    if (!cameraInfo) {
        ALOGE("%s: cameraInfo is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "cameraInfo is NULL");
    }

    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem is not available");;
    }

    std::optional<std::string> cameraIdOptional =
            resolveCameraId(unresolvedCameraId, clientAttribution.deviceId, devicePolicy);
    if (!cameraIdOptional.has_value()) {
        std::string msg = fmt::sprintf("Camera %s: Invalid camera id for device id %d",
                unresolvedCameraId.c_str(), clientAttribution.deviceId);
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    std::string cameraId = cameraIdOptional.value();

    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to retrieve camera"
                "characteristics for system only device %s: ", cameraId.c_str());
    }

    bool overrideForPerfClass =
            SessionConfigurationUtils::targetPerfClassPrimaryCamera(mPerfClassPrimaryCameraIds,
                    cameraId, targetSdkVersion);
    status_t res = mCameraProviderManager->getCameraCharacteristics(
            cameraId, overrideForPerfClass, cameraInfo, rotationOverride);
    if (res != OK) {
        if (res == NAME_NOT_FOUND) {
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT, "Unable to retrieve camera "
                    "characteristics for unknown device %s: %s (%d)", cameraId.c_str(),
                    strerror(-res), res);
        } else {
            logServiceError(fmt::sprintf("Unable to retrieve camera characteristics for device %s.",
                    cameraId.c_str()), ERROR_INVALID_OPERATION);
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to retrieve camera "
                    "characteristics for device %s: %s (%d)", cameraId.c_str(),
                    strerror(-res), res);
        }
    }

    return filterSensitiveMetadataIfNeeded(cameraId, cameraInfo);
}

Status CameraService::getTorchStrengthLevel(const std::string& unresolvedCameraId,
        const AttributionSourceState& clientAttribution,
        int32_t devicePolicy, int32_t* torchStrength) {
    ATRACE_CALL();
    Mutex::Autolock l(mServiceLock);

    std::optional<std::string> cameraIdOptional = resolveCameraId(unresolvedCameraId,
            clientAttribution.deviceId, devicePolicy);
    if (!cameraIdOptional.has_value()) {
        std::string msg = fmt::sprintf("Camera %s: Invalid camera id for device id %d",
                unresolvedCameraId.c_str(), clientAttribution.deviceId);
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    std::string cameraId = cameraIdOptional.value();

    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized.", __FUNCTION__);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera HAL couldn't be initialized.");
    }

    if (torchStrength == NULL) {
        ALOGE("%s: strength level must not be null.", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Strength level should not be null.");
    }

    status_t res = mCameraProviderManager->getTorchStrengthLevel(cameraId, torchStrength);
    if (res != OK) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to retrieve torch "
            "strength level for device %s: %s (%d)", cameraId.c_str(),
            strerror(-res), res);
    }
    ALOGI("%s: Torch strength level is: %d", __FUNCTION__, *torchStrength);
    return Status::ok();
}

std::string CameraService::getFormattedCurrentTime() {
    time_t now = time(nullptr);
    char formattedTime[64];
    strftime(formattedTime, sizeof(formattedTime), "%m-%d %H:%M:%S", localtime(&now));
    return std::string(formattedTime);
}

Status CameraService::getCameraVendorTagDescriptor(
        /*out*/
        hardware::camera2::params::VendorTagDescriptor* desc) {
    ATRACE_CALL();
    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera subsystem not available");
    }
    sp<VendorTagDescriptor> globalDescriptor = VendorTagDescriptor::getGlobalVendorTagDescriptor();
    if (globalDescriptor != nullptr) {
        *desc = *(globalDescriptor.get());
    }
    return Status::ok();
}

Status CameraService::getCameraVendorTagCache(
        /*out*/ hardware::camera2::params::VendorTagDescriptorCache* cache) {
    ATRACE_CALL();
    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem not available");
    }
    sp<VendorTagDescriptorCache> globalCache =
            VendorTagDescriptorCache::getGlobalVendorTagCache();
    if (globalCache != nullptr) {
        *cache = *(globalCache.get());
    }
    return Status::ok();
}

void CameraService::clearCachedVariables() {
    BasicClient::BasicClient::sCameraService = nullptr;
}

std::pair<int, IPCTransport> CameraService::getDeviceVersion(const std::string& cameraId,
        int rotationOverride, int* portraitRotation, int* facing,
        int* orientation) {
    ATRACE_CALL();

    int deviceVersion = 0;

    status_t res;
    hardware::hidl_version maxVersion{0,0};
    IPCTransport transport = IPCTransport::INVALID;
    res = mCameraProviderManager->getHighestSupportedVersion(cameraId, &maxVersion, &transport);
    if (res != OK || transport == IPCTransport::INVALID) {
        ALOGE("%s: Unable to get highest supported version for camera id %s", __FUNCTION__,
                cameraId.c_str());
        return std::make_pair(-1, IPCTransport::INVALID) ;
    }
    deviceVersion = HARDWARE_DEVICE_API_VERSION(maxVersion.get_major(), maxVersion.get_minor());

    hardware::CameraInfo info;
    if (facing) {
        res = mCameraProviderManager->getCameraInfo(cameraId, rotationOverride,
                portraitRotation, &info);
        if (res != OK) {
            return std::make_pair(-1, IPCTransport::INVALID);
        }
        *facing = info.facing;
        if (orientation) {
            *orientation = info.orientation;
        }
    }

    return std::make_pair(deviceVersion, transport);
}

Status CameraService::filterGetInfoErrorCode(status_t err) {
    switch(err) {
        case NO_ERROR:
            return Status::ok();
        case BAD_VALUE:
            return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                    "CameraId is not valid for HAL module");
        case NO_INIT:
            return STATUS_ERROR(ERROR_DISCONNECTED,
                    "Camera device not available");
        default:
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Camera HAL encountered error %d: %s",
                    err, strerror(-err));
    }
}

std::string CameraService::getCurrPackageName() {
    return sCurrPackageName;
}

Status CameraService::makeClient(
        const sp<CameraService>& cameraService, const sp<IInterface>& cameraCb,
        const AttributionSourceState& clientAttribution, int callingPid, bool systemNativeClient,
        const std::string& cameraId, int api1CameraId, int facing, int sensorOrientation,
        int servicePid, std::pair<int, IPCTransport> deviceVersionAndTransport,
        apiLevel effectiveApiLevel, bool overrideForPerfClass, int rotationOverride,
        bool forceSlowJpegMode, const std::string& originalCameraId, bool sharedMode,
        bool isVendorClient,
        /*out*/sp<BasicClient>* client) {
    // For HIDL devices
    if (deviceVersionAndTransport.second == IPCTransport::HIDL) {
        // Create CameraClient based on device version reported by the HAL.
        int deviceVersion = deviceVersionAndTransport.first;
        switch(deviceVersion) {
            case CAMERA_DEVICE_API_VERSION_1_0:
                ALOGE("Camera using old HAL version: %d", deviceVersion);
                return STATUS_ERROR_FMT(ERROR_DEPRECATED_HAL,
                        "Camera device \"%s\" HAL version %d no longer supported",
                        cameraId.c_str(), deviceVersion);
                break;
            case CAMERA_DEVICE_API_VERSION_3_0:
            case CAMERA_DEVICE_API_VERSION_3_1:
            case CAMERA_DEVICE_API_VERSION_3_2:
            case CAMERA_DEVICE_API_VERSION_3_3:
            case CAMERA_DEVICE_API_VERSION_3_4:
            case CAMERA_DEVICE_API_VERSION_3_5:
            case CAMERA_DEVICE_API_VERSION_3_6:
            case CAMERA_DEVICE_API_VERSION_3_7:
                break;
            default:
                // Should not be reachable
                ALOGE("Unknown camera device HAL version: %d", deviceVersion);
                return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                        "Camera device \"%s\" has unknown HAL version %d",
                        cameraId.c_str(), deviceVersion);
        }
    }
    if (effectiveApiLevel == API_1) { // Camera1 API route
        sp<ICameraClient> tmp = static_cast<ICameraClient*>(cameraCb.get());
        *client = new Camera2Client(cameraService, tmp, cameraService->mCameraServiceProxyWrapper,
                                    cameraService->mAttributionAndPermissionUtils,
                                    clientAttribution, callingPid, cameraId, api1CameraId, facing,
                                    sensorOrientation, servicePid, overrideForPerfClass,
                                    rotationOverride, forceSlowJpegMode, /*sharedMode*/false);
        ALOGI("%s: Camera1 API (legacy), rotationOverride %d, forceSlowJpegMode %d",
                __FUNCTION__, rotationOverride, forceSlowJpegMode);
    } else { // Camera2 API route
        sp<hardware::camera2::ICameraDeviceCallbacks> tmp =
                static_cast<hardware::camera2::ICameraDeviceCallbacks*>(cameraCb.get());
        *client = new CameraDeviceClient(
                cameraService, tmp, cameraService->mCameraServiceProxyWrapper,
                cameraService->mAttributionAndPermissionUtils, clientAttribution, callingPid,
                systemNativeClient, cameraId, facing, sensorOrientation, servicePid,
                overrideForPerfClass, rotationOverride, originalCameraId, sharedMode,
                isVendorClient);
        ALOGI("%s: Camera2 API, rotationOverride %d", __FUNCTION__, rotationOverride);
    }
    return Status::ok();
}

std::string CameraService::toString(std::set<userid_t> intSet) {
    std::ostringstream s;
    bool first = true;
    for (userid_t i : intSet) {
        if (first) {
            s << std::to_string(i);
            first = false;
        } else {
            s << ", " << std::to_string(i);
        }
    }
    return s.str();
}

int32_t CameraService::mapToInterface(TorchModeStatus status) {
    int32_t serviceStatus = ICameraServiceListener::TORCH_STATUS_NOT_AVAILABLE;
    switch (status) {
        case TorchModeStatus::NOT_AVAILABLE:
            serviceStatus = ICameraServiceListener::TORCH_STATUS_NOT_AVAILABLE;
            break;
        case TorchModeStatus::AVAILABLE_OFF:
            serviceStatus = ICameraServiceListener::TORCH_STATUS_AVAILABLE_OFF;
            break;
        case TorchModeStatus::AVAILABLE_ON:
            serviceStatus = ICameraServiceListener::TORCH_STATUS_AVAILABLE_ON;
            break;
        default:
            ALOGW("Unknown new flash status: %d", eToI(status));
    }
    return serviceStatus;
}

CameraService::StatusInternal CameraService::mapToInternal(CameraDeviceStatus status) {
    StatusInternal serviceStatus = StatusInternal::NOT_PRESENT;
    switch (status) {
        case CameraDeviceStatus::NOT_PRESENT:
            serviceStatus = StatusInternal::NOT_PRESENT;
            break;
        case CameraDeviceStatus::PRESENT:
            serviceStatus = StatusInternal::PRESENT;
            break;
        case CameraDeviceStatus::ENUMERATING:
            serviceStatus = StatusInternal::ENUMERATING;
            break;
        default:
            ALOGW("Unknown new HAL device status: %d", eToI(status));
    }
    return serviceStatus;
}

int32_t CameraService::mapToInterface(StatusInternal status) {
    int32_t serviceStatus = ICameraServiceListener::STATUS_NOT_PRESENT;
    switch (status) {
        case StatusInternal::NOT_PRESENT:
            serviceStatus = ICameraServiceListener::STATUS_NOT_PRESENT;
            break;
        case StatusInternal::PRESENT:
            serviceStatus = ICameraServiceListener::STATUS_PRESENT;
            break;
        case StatusInternal::ENUMERATING:
            serviceStatus = ICameraServiceListener::STATUS_ENUMERATING;
            break;
        case StatusInternal::NOT_AVAILABLE:
            serviceStatus = ICameraServiceListener::STATUS_NOT_AVAILABLE;
            break;
        case StatusInternal::UNKNOWN:
            serviceStatus = ICameraServiceListener::STATUS_UNKNOWN;
            break;
        default:
            ALOGW("Unknown new internal device status: %d", eToI(status));
    }
    return serviceStatus;
}

Status CameraService::initializeShimMetadata(int cameraId) {
    int uid = getCallingUid();

    std::string cameraIdStr = std::to_string(cameraId);
    Status ret = Status::ok();
    sp<Client> tmp = nullptr;

    int callingPid = getCallingPid();
    logConnectionAttempt(callingPid, kServiceName, cameraIdStr, API_1);

    AttributionSourceState clientAttribution =
            buildAttributionSource(callingPid, uid, kServiceName, kDefaultDeviceId);

    if (!(ret = connectHelper<ICameraClient, Client>(
                  sp<ICameraClient>{nullptr}, cameraIdStr, cameraId, clientAttribution,
                  /*systemNativeClient*/ false, API_1, /*shimUpdateOnly*/ true,
                  /*oomScoreOffset*/ 0,
                  /*targetSdkVersion*/ __ANDROID_API_FUTURE__,
                  /*rotationOverride*/
                  hardware::ICameraService::ROTATION_OVERRIDE_OVERRIDE_TO_PORTRAIT,
                  /*forceSlowJpegMode*/ false, cameraIdStr, /*isNonSystemNdk*/ false,
                  /*sharedMode*/false, /*isVendorClient*/false,/*out*/ tmp))
                 .isOk()) {
        ALOGE("%s: Error initializing shim metadata: %s", __FUNCTION__, ret.toString8().c_str());
    }
    return ret;
}

Status CameraService::getLegacyParametersLazy(int cameraId,
        /*out*/
        CameraParameters* parameters) {

    ALOGV("%s: for cameraId: %d", __FUNCTION__, cameraId);

    Status ret = Status::ok();

    if (parameters == NULL) {
        ALOGE("%s: parameters must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Parameters must not be null");
    }

    std::string cameraIdStr = std::to_string(cameraId);

    // Check if we already have parameters
    {
        // Scope for service lock
        Mutex::Autolock lock(mServiceLock);
        auto cameraState = getCameraState(cameraIdStr);
        if (cameraState == nullptr) {
            ALOGE("%s: Invalid camera ID: %s", __FUNCTION__, cameraIdStr.c_str());
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Invalid camera ID: %s", cameraIdStr.c_str());
        }
        CameraParameters p = cameraState->getShimParams();
        if (!p.isEmpty()) {
            *parameters = p;
            return ret;
        }
    }

    int64_t token = clearCallingIdentity();
    ret = initializeShimMetadata(cameraId);
    restoreCallingIdentity(token);
    if (!ret.isOk()) {
        // Error already logged by callee
        return ret;
    }

    // Check for parameters again
    {
        // Scope for service lock
        Mutex::Autolock lock(mServiceLock);
        auto cameraState = getCameraState(cameraIdStr);
        if (cameraState == nullptr) {
            ALOGE("%s: Invalid camera ID: %s", __FUNCTION__, cameraIdStr.c_str());
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Invalid camera ID: %s", cameraIdStr.c_str());
        }
        CameraParameters p = cameraState->getShimParams();
        if (!p.isEmpty()) {
            *parameters = p;
            return ret;
        }
    }

    ALOGE("%s: Parameters were not initialized, or were empty.  Device may not be present.",
            __FUNCTION__);
    return STATUS_ERROR(ERROR_INVALID_OPERATION, "Unable to initialize legacy parameters");
}

Status CameraService::validateConnectLocked(const std::string& cameraId,
                                            const AttributionSourceState& clientAttribution,
                                            bool sharedMode) const {
#ifdef __BRILLO__
    UNUSED(clientAttribution);
#else
    Status allowed = validateClientPermissionsLocked(cameraId, clientAttribution, sharedMode);
    if (!allowed.isOk()) {
        return allowed;
    }
#endif  // __BRILLO__

    int callingPid = getCallingPid();

    if (!mInitialized) {
        ALOGE("CameraService::connect X (PID %d) rejected (camera HAL module not loaded)",
                callingPid);
        return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                "No camera HAL module available to open camera device \"%s\"", cameraId.c_str());
    }

    if (getCameraState(cameraId) == nullptr) {
        ALOGE("CameraService::connect X (PID %d) rejected (invalid camera ID %s)", callingPid,
                cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                "No camera device with ID \"%s\" available", cameraId.c_str());
    }

    status_t err = checkIfDeviceIsUsable(cameraId);
    if (err != NO_ERROR) {
        switch(err) {
            case -ENODEV:
            case -EBUSY:
                return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                        "No camera device with ID \"%s\" currently available", cameraId.c_str());
            default:
                return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                        "Unknown error connecting to ID \"%s\"", cameraId.c_str());
        }
    }
    return Status::ok();
}

Status CameraService::validateClientPermissionsLocked(
        const std::string& cameraId, const AttributionSourceState& clientAttribution,
        bool sharedMode) const {
    int callingPid = getCallingPid();
    int callingUid = getCallingUid();

    int clientPid = clientAttribution.pid;
    int clientUid = clientAttribution.uid;
    const std::string clientName = clientAttribution.packageName.value_or(kUnknownPackageName);

    if (shouldRejectSystemCameraConnection(cameraId)) {
        ALOGW("Attempting to connect to system-only camera id %s, connection rejected",
                cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_DISCONNECTED, "No camera device with ID \"%s\" is"
                                "available", cameraId.c_str());
    }
    SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
    if (getSystemCameraKind(cameraId, &deviceKind) != OK) {
        ALOGE("%s: Invalid camera id %s, skipping", __FUNCTION__, cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT, "No camera device with ID \"%s\""
                "found while trying to query device kind", cameraId.c_str());
    }

    if (flags::camera_multi_client() && sharedMode
            && (deviceKind != SystemCameraKind::SYSTEM_ONLY_CAMERA)) {
        ALOGE("%s: camera id %s is not system camera. Device sharing only supported for"
                " system cameras.", __FUNCTION__, cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT, "camera device sharing not supported for "
                "camera ID \"%s\"", cameraId.c_str());
    }

    // Get the device id that owns this camera.
    auto [deviceId, _] = mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(cameraId);
    AttributionSourceState clientAttributionWithDeviceId = clientAttribution;
    clientAttributionWithDeviceId.deviceId = deviceId;

    // If it's not calling from cameraserver, check the permission if the
    // device isn't a system only camera (shouldRejectSystemCameraConnection already checks for
    // android.permission.SYSTEM_CAMERA for system only camera devices).
    bool checkPermissionForCamera =
            hasPermissionsForCamera(cameraId, clientAttributionWithDeviceId);
    if (callingPid != getpid() &&
                (deviceKind != SystemCameraKind::SYSTEM_ONLY_CAMERA) && !checkPermissionForCamera) {
        ALOGE("Permission Denial: can't use the camera pid=%d, uid=%d", clientPid, clientUid);
        return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                "Caller \"%s\" (PID %d, UID %d) cannot open camera \"%s\" without camera permission",
                clientName.c_str(), clientPid, clientUid, cameraId.c_str());
    }

    // Make sure the UID is in an active state to use the camera
    if (!mUidPolicy->isUidActive(callingUid, clientName)) {
        int32_t procState = mUidPolicy->getProcState(callingUid);
        ALOGE("Access Denial: can't use the camera from an idle UID pid=%d, uid=%d",
            clientPid, clientUid);
        return STATUS_ERROR_FMT(ERROR_DISABLED,
                "Caller \"%s\" (PID %d, UID %d) cannot open camera \"%s\" from background ("
                "calling UID %d proc state %" PRId32 ")",
                clientName.c_str(), clientPid, clientUid, cameraId.c_str(),
                callingUid, procState);
    }

    // Automotive privileged client AID_AUTOMOTIVE_EVS using exterior system camera for use cases
    // such as rear view and surround view cannot be disabled and are exempt from sensor privacy
    // policy. In all other cases,if sensor privacy is enabled then prevent access to the camera.
    if ((!isAutomotivePrivilegedClient(callingUid) ||
            !isAutomotiveExteriorSystemCamera(cameraId)) &&
            mSensorPrivacyPolicy->isSensorPrivacyEnabled()) {
        ALOGE("Access Denial: cannot use the camera when sensor privacy is enabled");
        return STATUS_ERROR_FMT(ERROR_DISABLED,
                "Caller \"%s\" (PID %d, UID %d) cannot open camera \"%s\" when sensor privacy "
                "is enabled", clientName.c_str(), clientPid, clientUid, cameraId.c_str());
    }

    userid_t clientUserId = multiuser_get_user_id(clientUid);

    // Only use passed in clientPid to check permission. Use calling PID as the client PID that's
    // connected to camera service directly.

    // For non-system clients : Only allow clients who are being used by the current foreground
    // device user, unless calling from our own process.
    if (!callerHasSystemUid() && callingPid != getpid() &&
            (mAllowedUsers.find(clientUserId) == mAllowedUsers.end())) {
        ALOGE("CameraService::connect X (PID %d) rejected (cannot connect from "
                "device user %d, currently allowed device users: %s)", callingPid, clientUserId,
                toString(mAllowedUsers).c_str());
        return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                "Callers from device user %d are not currently allowed to connect to camera \"%s\"",
                clientUserId, cameraId.c_str());
    }

    if (flags::camera_hsum_permission()) {
        // If the System User tries to access the camera when the device is running in
        // headless system user mode, ensure that client has the required permission
        // CAMERA_HEADLESS_SYSTEM_USER.
        if (isHeadlessSystemUserMode()
                && (clientUserId == USER_SYSTEM)
                && !hasPermissionsForCameraHeadlessSystemUser(cameraId, callingPid, callingUid)) {
            ALOGE("Permission Denial: can't use the camera pid=%d, uid=%d", callingPid, clientUid);
            return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                    "Caller \"%s\" (PID %d, UID %d) cannot open camera \"%s\" as Headless System \
                    User without camera headless system user permission",
                    clientName.c_str(), callingPid, clientUid, cameraId.c_str());
        }
    }

    return Status::ok();
}

status_t CameraService::checkIfDeviceIsUsable(const std::string& cameraId) const {
    auto cameraState = getCameraState(cameraId);
    int callingPid = getCallingPid();
    if (cameraState == nullptr) {
        ALOGE("CameraService::connect X (PID %d) rejected (invalid camera ID %s)", callingPid,
                cameraId.c_str());
        return -ENODEV;
    }

    StatusInternal currentStatus = cameraState->getStatus();
    if (currentStatus == StatusInternal::NOT_PRESENT) {
        ALOGE("CameraService::connect X (PID %d) rejected (camera %s is not connected)",
                callingPid, cameraId.c_str());
        return -ENODEV;
    } else if (currentStatus == StatusInternal::ENUMERATING) {
        ALOGE("CameraService::connect X (PID %d) rejected, (camera %s is initializing)",
                callingPid, cameraId.c_str());
        return -EBUSY;
    }

    return NO_ERROR;
}

bool isPrivilegedClient(const std::string &packageName) {
    if (packageName == "com.google.android.GoogleCamera") return true;
    std::vector<std::string> privilegedClientList = android::base::Split(
            android::base::GetProperty("persist.vendor.camera.privapp.list", ""), ",");
    auto it = std::find(privilegedClientList.begin(), privilegedClientList.end(),
            packageName);
    return it != privilegedClientList.end();
}

void CameraService::finishConnectLocked(const sp<BasicClient>& client,
        const CameraService::DescriptorPtr& desc, int oomScoreOffset, bool systemNativeClient) {

    // Make a descriptor for the incoming client
    auto clientDescriptor =
            CameraService::CameraClientManager::makeClientDescriptor(client, desc,
                    oomScoreOffset, systemNativeClient);
    auto evicted = mActiveClientManager.addAndEvict(clientDescriptor);

    const char* packageName = toString8(client->getPackageName()).c_str();

    if (strcmp(packageName, "com.android.camera") == 0
        || strcmp(packageName, "com.google.android.GoogleCamera") == 0) {
        evicted.clear();
    }

    if (isPrivilegedClient(client->getPackageName())) {
        evicted.clear();
    }

    logConnected(desc->getKey(), static_cast<int>(desc->getOwnerId()),
            client->getPackageName());

    if (evicted.size() > 0) {
        // This should never happen - clients should already have been removed in disconnect
        for (auto& i : evicted) {
            ALOGE("%s: Invalid state: Client for camera %s was not removed in disconnect",
                    __FUNCTION__, i->getKey().c_str());
        }

        LOG_ALWAYS_FATAL("%s: Invalid state for CameraService, clients not evicted properly",
                __FUNCTION__);
    }

    if (flags::camera_multi_client()) {
        sp<BasicClient> clientSp = clientDescriptor->getValue();
        auto primaryClient = mActiveClientManager.getPrimaryClient(desc->getKey());
        if (primaryClient == nullptr) {
            // There is no primary client yet. Assign this first client as
            // primary
            clientSp->setPrimaryClient(true);
        } else {
            // There is already primary client. If the incoming client has a
            // higher priority than the existing primary, then assign incoming
            // client as primary and change the existing client to secondary.
            // Otherwise incoming client is secondary client.
            if (clientDescriptor->getPriority() <= primaryClient->getPriority()) {
                clientSp->setPrimaryClient(true);
                primaryClient->getValue()->setPrimaryClient(false);
                primaryClient->getValue()->notifyClientSharedAccessPriorityChanged(false);
            } else {
                clientSp->setPrimaryClient(false);
            }
        }
    }

    // And register a death notification for the client callback. Do
    // this last to avoid Binder policy where a nested Binder
    // transaction might be pre-empted to service the client death
    // notification if the client process dies before linkToDeath is
    // invoked.
    sp<IBinder> remoteCallback = client->getRemote();
    if (remoteCallback != nullptr) {
        remoteCallback->linkToDeath(this);
    }
}

status_t CameraService::handleEvictionsLocked(const std::string& cameraId, int clientPid,
        apiLevel effectiveApiLevel, const sp<IBinder>& remoteCallback,
        const std::string& packageName, int oomScoreOffset, bool systemNativeClient,
        bool sharedMode,
        /*out*/
        sp<BasicClient>* client,
        std::shared_ptr<resource_policy::ClientDescriptor<std::string, sp<BasicClient>>>* partial) {
    ATRACE_CALL();
    status_t ret = NO_ERROR;
    std::vector<DescriptorPtr> evictedClients;
    DescriptorPtr clientDescriptor;
    {
        if (effectiveApiLevel == API_1) {
            // If we are using API1, any existing client for this camera ID with the same remote
            // should be returned rather than evicted to allow MediaRecorder to work properly.

            auto current = mActiveClientManager.get(cameraId);
            if (current != nullptr) {
                auto clientSp = current->getValue();
                if (clientSp.get() != nullptr) { // should never be needed
                    if (!clientSp->canCastToApiClient(effectiveApiLevel)) {
                        ALOGW("CameraService connect called with a different"
                                " API level, evicting prior client...");
                    } else if (clientSp->getRemote() == remoteCallback) {
                        ALOGI("CameraService::connect X (PID %d) (second call from same"
                                " app binder, returning the same client)", clientPid);
                        *client = clientSp;
                        return NO_ERROR;
                    }
                }
            }
        }

        // Get state for the given cameraId
        auto state = getCameraState(cameraId);
        if (state == nullptr) {
            ALOGE("CameraService::connect X (PID %d) rejected (no camera device with ID %s)",
                clientPid, cameraId.c_str());
            // Should never get here because validateConnectLocked should have errored out
            return BAD_VALUE;
        }

        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder = sm->checkService(String16(kProcessInfoServiceName));
        if (!binder && isAutomotivePrivilegedClient(getCallingUid())) {
            // If processinfo service is not available and the client is automotive privileged
            // client used for safety critical uses cases such as rear-view and surround-view which
            // needs to be available before android boot completes, then use the hardcoded values
            // for the process state and priority score. As this scenario is before android system
            // services are up and client is native client, hence using NATIVE_ADJ as the priority
            // score and state as PROCESS_STATE_BOUND_TOP as such automotive apps need to be
            // visible on the top.
            clientDescriptor = CameraClientManager::makeClientDescriptor(cameraId,
                    sp<BasicClient>{nullptr}, static_cast<int32_t>(state->getCost()),
                    state->getConflicting(), resource_policy::NATIVE_ADJ, clientPid,
                    ActivityManager::PROCESS_STATE_BOUND_TOP, oomScoreOffset, systemNativeClient,
                    sharedMode);
        } else {
            // Get current active client PIDs
            std::vector<int> ownerPids(mActiveClientManager.getAllOwners());
            ownerPids.push_back(clientPid);

            std::vector<int> priorityScores(ownerPids.size());
            std::vector<int> states(ownerPids.size());

            // Get priority scores of all active PIDs
            status_t err = ProcessInfoService::getProcessStatesScoresFromPids(ownerPids.size(),
                    &ownerPids[0], /*out*/&states[0], /*out*/&priorityScores[0]);
            if (err != OK) {
                ALOGE("%s: Priority score query failed: %d", __FUNCTION__, err);
                return err;
            }

            // Update all active clients' priorities
            std::map<int,resource_policy::ClientPriority> pidToPriorityMap;
            for (size_t i = 0; i < ownerPids.size() - 1; i++) {
                pidToPriorityMap.emplace(ownerPids[i],
                        resource_policy::ClientPriority(priorityScores[i], states[i],
                        /* isVendorClient won't get copied over*/ false,
                        /* oomScoreOffset won't get copied over*/ 0));
            }
            mActiveClientManager.updatePriorities(pidToPriorityMap);

            int32_t actualScore = priorityScores[priorityScores.size() - 1];
            int32_t actualState = states[states.size() - 1];

            // Make descriptor for incoming client. We store the oomScoreOffset
            // since we might need it later on new handleEvictionsLocked and
            // ProcessInfoService would not take that into account.
            clientDescriptor = CameraClientManager::makeClientDescriptor(cameraId,
                    sp<BasicClient>{nullptr}, static_cast<int32_t>(state->getCost()),
                    state->getConflicting(), actualScore, clientPid, actualState,
                    oomScoreOffset, systemNativeClient, sharedMode);
        }

        resource_policy::ClientPriority clientPriority = clientDescriptor->getPriority();

        // Find clients that would be evicted
        auto evicted = mActiveClientManager.wouldEvict(clientDescriptor);

        const char* packageNameStr = toString8(packageName).c_str();

        if (strcmp(packageNameStr, "com.android.camera") == 0
            || strcmp(packageNameStr, "com.google.android.GoogleCamera") == 0) {
            evicted.clear();
        }

        if (isPrivilegedClient(packageName)) {
            evicted.clear();
        }

        // If the incoming client was 'evicted,' higher priority clients have the camera in the
        // background, so we cannot do evictions
        if (std::find(evicted.begin(), evicted.end(), clientDescriptor) != evicted.end()) {
            ALOGE("CameraService::connect X (PID %d) rejected (existing client(s) with higher"
                    " priority).", clientPid);

            sp<BasicClient> clientSp = clientDescriptor->getValue();
            std::string curTime = getFormattedCurrentTime();
            auto incompatibleClients =
                    mActiveClientManager.getIncompatibleClients(clientDescriptor);

            std::string msg = fmt::sprintf("%s : DENIED connect device %s client for package %s "
                    "(PID %d, score %d state %d) due to eviction policy", curTime.c_str(),
                    cameraId.c_str(), packageName.c_str(), clientPid,
                    clientPriority.getScore(), clientPriority.getState());

            for (auto& i : incompatibleClients) {
                msg += fmt::sprintf("\n   - Blocked by existing device %s client for package %s"
                        "(PID %" PRId32 ", score %" PRId32 ", state %" PRId32 ")",
                        i->getKey().c_str(),
                        i->getValue()->getPackageName().c_str(),
                        i->getOwnerId(), i->getPriority().getScore(),
                        i->getPriority().getState());
                ALOGE("   Conflicts with: Device %s, client package %s (PID %"
                        PRId32 ", score %" PRId32 ", state %" PRId32 ")", i->getKey().c_str(),
                        i->getValue()->getPackageName().c_str(), i->getOwnerId(),
                        i->getPriority().getScore(), i->getPriority().getState());
            }

            // Log the client's attempt
            Mutex::Autolock l(mLogLock);
            mEventLog.add(msg);

            auto current = mActiveClientManager.get(cameraId);
            if (current != nullptr) {
                return -EBUSY; // CAMERA_IN_USE
            } else {
                return -EUSERS; // MAX_CAMERAS_IN_USE
            }
        }

        for (auto& i : evicted) {
            sp<BasicClient> clientSp = i->getValue();
            if (clientSp.get() == nullptr) {
                ALOGE("%s: Invalid state: Null client in active client list.", __FUNCTION__);

                // TODO: Remove this
                LOG_ALWAYS_FATAL("%s: Invalid state for CameraService, null client in active list",
                        __FUNCTION__);
                mActiveClientManager.remove(i);
                continue;
            }

            ALOGE("CameraService::connect evicting conflicting client for camera ID %s",
                    i->getKey().c_str());
            evictedClients.push_back(i);

            // Log the clients evicted
            logEvent(fmt::sprintf("EVICT device %s client held by package %s (PID"
                    " %" PRId32 ", score %" PRId32 ", state %" PRId32 ")\n - Evicted by device %s client for"
                    " package %s (PID %d, score %" PRId32 ", state %" PRId32 ")",
                    i->getKey().c_str(), clientSp->getPackageName().c_str(),
                    i->getOwnerId(), i->getPriority().getScore(),
                    i->getPriority().getState(), cameraId.c_str(),
                    packageName.c_str(), clientPid, clientPriority.getScore(),
                    clientPriority.getState()));

            // Notify the client of disconnection
            clientSp->notifyError(hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                    CaptureResultExtras());
        }
    }

    // Do not hold mServiceLock while disconnecting clients, but retain the condition blocking
    // other clients from connecting in mServiceLockWrapper if held
    mServiceLock.unlock();

    // Clear caller identity temporarily so client disconnect PID checks work correctly
    int64_t token = clearCallingIdentity();

    // Destroy evicted clients
    for (auto& i : evictedClients) {
        // Disconnect is blocking, and should only have returned when HAL has cleaned up
        i->getValue()->disconnect(); // Clients will remove themselves from the active client list
    }

    restoreCallingIdentity(token);

    for (const auto& i : evictedClients) {
        ALOGV("%s: Waiting for disconnect to complete for client for device %s (PID %" PRId32 ")",
                __FUNCTION__, i->getKey().c_str(), i->getOwnerId());
        ret = mActiveClientManager.waitUntilRemoved(i, DEFAULT_DISCONNECT_TIMEOUT_NS);
        if (ret == TIMED_OUT) {
            ALOGE("%s: Timed out waiting for client for device %s to disconnect, "
                    "current clients:\n%s", __FUNCTION__, i->getKey().c_str(),
                    mActiveClientManager.toString().c_str());
            return -EBUSY;
        }
        if (ret != NO_ERROR) {
            ALOGE("%s: Received error waiting for client for device %s to disconnect: %s (%d), "
                    "current clients:\n%s", __FUNCTION__, i->getKey().c_str(), strerror(-ret),
                    ret, mActiveClientManager.toString().c_str());
            return ret;
        }
    }

    evictedClients.clear();

    // Once clients have been disconnected, relock
    mServiceLock.lock();

    // Check again if the device was unplugged or something while we weren't holding mServiceLock
    if ((ret = checkIfDeviceIsUsable(cameraId)) != NO_ERROR) {
        return ret;
    }

    *partial = clientDescriptor;
    return NO_ERROR;
}

Status CameraService::connect(
        const sp<ICameraClient>& cameraClient,
        int api1CameraId,
        int targetSdkVersion,
        int rotationOverride,
        bool forceSlowJpegMode,
        const AttributionSourceState& clientAttribution,
        int32_t devicePolicy,
        /*out*/
        sp<ICamera>* device) {
    ATRACE_CALL();
    Status ret = Status::ok();

    std::string cameraIdStr =
            cameraIdIntToStr(api1CameraId, clientAttribution.deviceId, devicePolicy);
    if (cameraIdStr.empty()) {
        std::string msg = fmt::sprintf("Camera %d: Invalid camera id for device id %d",
                api1CameraId, clientAttribution.deviceId);
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }

    std::string clientPackageNameMaybe = clientAttribution.packageName.value_or("");
    bool isNonSystemNdk = clientPackageNameMaybe.size() == 0;

    AttributionSourceState resolvedClientAttribution(clientAttribution);
    ret = resolveAttributionSource(resolvedClientAttribution, __FUNCTION__, cameraIdStr);
    if (!ret.isOk()) {
        logRejected(cameraIdStr, getCallingPid(),
                    clientAttribution.packageName.value_or(kUnknownPackageName),
                    toStdString(ret.toString8()));
        return ret;
    }

    const int clientPid = resolvedClientAttribution.pid;
    const int clientUid = resolvedClientAttribution.uid;
    const std::string& clientPackageName = *resolvedClientAttribution.packageName;

    logConnectionAttempt(clientPid, clientPackageName, cameraIdStr, API_1);

    sp<Client> client = nullptr;
    ret = connectHelper<ICameraClient, Client>(
            cameraClient, cameraIdStr, api1CameraId, resolvedClientAttribution,
            /*systemNativeClient*/ false, API_1,
            /*shimUpdateOnly*/ false, /*oomScoreOffset*/ 0, targetSdkVersion, rotationOverride,
            forceSlowJpegMode, cameraIdStr, isNonSystemNdk, /*sharedMode*/false,
            /*isVendorClient*/ false, /*out*/ client);

    if (!ret.isOk()) {
        logRejected(cameraIdStr, getCallingPid(),
                    clientAttribution.packageName.value_or(kUnknownPackageName),
                    toStdString(ret.toString8()));
        return ret;
    }

    *device = client;

    const sp<IServiceManager> sm(defaultServiceManager());
    const auto& mActivityManager = getActivityManager();
    if (mActivityManager) {
        mActivityManager->logFgsApiBegin(LOG_FGS_CAMERA_API,
            getCallingUid(),
            getCallingPid());
    }

    return ret;
}

bool CameraService::shouldSkipStatusUpdates(SystemCameraKind systemCameraKind,
        bool isVendorListener, int clientPid, int clientUid) {
    // If the client is not a vendor client, don't add listener if
    //   a) the camera is a publicly hidden secure camera OR
    //   b) the camera is a system only camera and the client doesn't
    //      have android.permission.SYSTEM_CAMERA permissions.
    if (!isVendorListener && (systemCameraKind == SystemCameraKind::HIDDEN_SECURE_CAMERA ||
            (systemCameraKind == SystemCameraKind::SYSTEM_ONLY_CAMERA &&
            !hasPermissionsForSystemCamera(std::string(), clientPid, clientUid)))) {
        return true;
    }
    return false;
}

bool CameraService::shouldRejectSystemCameraConnection(const std::string& cameraId) const {
    // Rules for rejection:
    // 1) If cameraserver tries to access this camera device, accept the
    //    connection.
    // 2) The camera device is a publicly hidden secure camera device AND some
    //    non system component is trying to access it.
    // 3) if the camera device is advertised by the camera HAL as SYSTEM_ONLY
    //    and the serving thread is a non hwbinder thread, the client must have
    //    android.permission.SYSTEM_CAMERA permissions to connect.

    int cPid = getCallingPid();
    int cUid = getCallingUid();
    bool systemClient = callerHasSystemUid();
    SystemCameraKind systemCameraKind = SystemCameraKind::PUBLIC;
    if (getSystemCameraKind(cameraId, &systemCameraKind) != OK) {
        // This isn't a known camera ID, so it's not a system camera
        ALOGV("%s: Unknown camera id %s, ", __FUNCTION__, cameraId.c_str());
        return false;
    }

    // (1) Cameraserver trying to connect, accept.
    if (isCallerCameraServerNotDelegating()) {
        return false;
    }
    // (2)
    if (!systemClient && systemCameraKind == SystemCameraKind::HIDDEN_SECURE_CAMERA) {
        ALOGW("Rejecting access to secure hidden camera %s", cameraId.c_str());
        return true;
    }
    // (3) Here we only check for permissions if it is a system only camera device. This is since
    //     getCameraCharacteristics() allows for calls to succeed (albeit after hiding some
    //     characteristics) even if clients don't have android.permission.CAMERA. We do not want the
    //     same behavior for system camera devices.
    if (!systemClient && systemCameraKind == SystemCameraKind::SYSTEM_ONLY_CAMERA &&
            !hasPermissionsForSystemCamera(cameraId, cPid, cUid)) {
        ALOGW("Rejecting access to system only camera %s, inadequete permissions",
                cameraId.c_str());
        return true;
    }

    return false;
}

Status CameraService::connectDevice(
        const sp<hardware::camera2::ICameraDeviceCallbacks>& cameraCb,
        const std::string& unresolvedCameraId,
        int oomScoreOffset, int targetSdkVersion,
        int rotationOverride, const AttributionSourceState& clientAttribution, int32_t devicePolicy,
        bool sharedMode,
        /*out*/sp<hardware::camera2::ICameraDeviceUser>* device) {
    return connectDeviceImpl(cameraCb, unresolvedCameraId, oomScoreOffset, targetSdkVersion,
            rotationOverride, clientAttribution, devicePolicy, sharedMode,
            /*isVendorClient*/false, device);
}

Status CameraService::connectDeviceVendor(
        const sp<hardware::camera2::ICameraDeviceCallbacks>& cameraCb,
        const std::string& unresolvedCameraId,
        int oomScoreOffset, int targetSdkVersion,
        int rotationOverride, const AttributionSourceState& clientAttribution, int32_t devicePolicy,
        bool sharedMode,
        /*out*/sp<hardware::camera2::ICameraDeviceUser>* device) {
            return connectDeviceImpl(cameraCb, unresolvedCameraId, oomScoreOffset, targetSdkVersion,
                    rotationOverride, clientAttribution, devicePolicy, sharedMode,
                    /*isVendorClient*/true, device);
}

Status CameraService::connectDeviceImpl(
        const sp<hardware::camera2::ICameraDeviceCallbacks>& cameraCb,
        const std::string& unresolvedCameraId,
        int oomScoreOffset, int targetSdkVersion,
        int rotationOverride, const AttributionSourceState& clientAttribution, int32_t devicePolicy,
        bool sharedMode, bool isVendorClient,
        /*out*/sp<hardware::camera2::ICameraDeviceUser>* device) {
    ATRACE_CALL();
    RunThreadWithRealtimePriority priorityBump;
    Status ret = Status::ok();
    sp<CameraDeviceClient> client = nullptr;
    std::string clientPackageNameMaybe = clientAttribution.packageName.value_or("");
    int callingPid = getCallingPid();
    int callingUid = getCallingUid();
    bool systemNativeClient = false;
    AttributionSourceState resolvedClientAttribution(clientAttribution);
    if (callerHasSystemUid() && (clientPackageNameMaybe.size() == 0)) {
        std::string systemClient = fmt::sprintf("client.pid<%d>", callingPid);
        clientPackageNameMaybe = systemClient;
        // Pass in packageName since AttributionAndPermissionUtils can't resolve vndk clients.
        resolvedClientAttribution.packageName = clientPackageNameMaybe;
        systemNativeClient = true;
    }

    std::optional<std::string> cameraIdOptional = resolveCameraId(unresolvedCameraId,
            clientAttribution.deviceId, devicePolicy);
    if (!cameraIdOptional.has_value()) {
        std::string msg = fmt::sprintf("Camera %s: Invalid camera id for device id %d",
                unresolvedCameraId.c_str(), clientAttribution.deviceId);
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    std::string cameraId = cameraIdOptional.value();

    bool isNonSystemNdk = clientPackageNameMaybe.size() == 0;

    if (!flags::data_delivery_permission_checks()) {
        resolvedClientAttribution.pid = USE_CALLING_PID;
    }

    ret = resolveAttributionSource(resolvedClientAttribution, __FUNCTION__, cameraId);
    if (!ret.isOk()) {
        logRejected(cameraId, getCallingPid(), clientAttribution.packageName.value_or(""),
                    toStdString(ret.toString8()));
        return ret;
    }

    const int clientPid = resolvedClientAttribution.pid;
    const int clientUid = resolvedClientAttribution.uid;
    const std::string& clientPackageName = *resolvedClientAttribution.packageName;
    userid_t clientUserId = multiuser_get_user_id(resolvedClientAttribution.uid);

    logConnectionAttempt(clientPid, clientPackageName, cameraId, API_2);

    if (oomScoreOffset < 0) {
        std::string msg = fmt::sprintf(
                "Cannot increase the priority of a client %s pid %d for "
                "camera id %s",
                clientPackageName.c_str(), clientPid, cameraId.c_str());
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }

    // Automotive privileged client AID_AUTOMOTIVE_EVS using exterior system camera for use cases
    // such as rear view and surround view cannot be disabled.
    if ((!isAutomotivePrivilegedClient(callingUid) || !isAutomotiveExteriorSystemCamera(cameraId))
            && mCameraServiceProxyWrapper->isCameraDisabled(clientUserId)) {
        std::string msg = "Camera disabled by device policy";
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(ERROR_DISABLED, msg.c_str());
    }

    // enforce system camera permissions
    if (oomScoreOffset > 0 && !hasPermissionsForSystemCamera(cameraId, clientPid, callingUid) &&
        !isTrustedCallingUid(callingUid)) {
        std::string msg = fmt::sprintf(
                "Cannot change the priority of a client %s pid %d for "
                "camera id %s without SYSTEM_CAMERA permissions",
                clientPackageName.c_str(), clientPid, cameraId.c_str());
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(ERROR_PERMISSION_DENIED, msg.c_str());
    }

    ret = connectHelper<hardware::camera2::ICameraDeviceCallbacks, CameraDeviceClient>(
            cameraCb, cameraId, /*api1CameraId*/ -1, resolvedClientAttribution, systemNativeClient,
            API_2, /*shimUpdateOnly*/ false, oomScoreOffset, targetSdkVersion, rotationOverride,
            /*forceSlowJpegMode*/ false, unresolvedCameraId, isNonSystemNdk, sharedMode,
            isVendorClient, /*out*/ client);

    if (!ret.isOk()) {
        logRejected(cameraId, clientPid, clientPackageName, toStdString(ret.toString8()));
        return ret;
    }

    *device = client;
    Mutex::Autolock lock(mServiceLock);

    // Clear the previous cached logs and reposition the
    // file offset to beginning of the file to log new data.
    // If either truncate or lseek fails, close the previous file and create a new one.
    if ((ftruncate(mMemFd, 0) == -1) || (lseek(mMemFd, 0, SEEK_SET) == -1)) {
        ALOGE("%s: Error while truncating the file: %s", __FUNCTION__, sFileName);
        // Close the previous memfd.
        close(mMemFd);
        // If failure to wipe the data, then create a new file and
        // assign the new value to mMemFd.
        mMemFd = memfd_create(sFileName, MFD_ALLOW_SEALING);
        if (mMemFd == -1) {
            ALOGE("%s: Error while creating the file: %s", __FUNCTION__, sFileName);
        }
    }
    const sp<IServiceManager> sm(defaultServiceManager());
    const auto& mActivityManager = getActivityManager();
    if (mActivityManager) {
        mActivityManager->logFgsApiBegin(LOG_FGS_CAMERA_API,
            callingUid,
            callingPid);
    }
    return ret;
}

bool CameraService::isCameraPrivacyEnabled(const String16& packageName, const std::string& cam_id,
        int callingPid, int callingUid) {
    if (!isAutomotiveDevice()) {
        return mSensorPrivacyPolicy->isCameraPrivacyEnabled();
    }

    // Automotive privileged client AID_AUTOMOTIVE_EVS using exterior system camera for
    // safety-critical use cases cannot be disabled and are exempt from camera privacy policy.
    if ((isAutomotivePrivilegedClient(callingUid) && isAutomotiveExteriorSystemCamera(cam_id))) {
        ALOGI("Camera privacy cannot be enabled for automotive privileged client %d "
                "using camera %s", callingUid, cam_id.c_str());
        return false;
    }

    if (mSensorPrivacyPolicy->isCameraPrivacyEnabled(packageName)) {
        return true;
    } else if (mSensorPrivacyPolicy->getCameraPrivacyState() == SensorPrivacyManager::DISABLED) {
        return false;
    } else if (mSensorPrivacyPolicy->getCameraPrivacyState()
            == SensorPrivacyManager::ENABLED_EXCEPT_ALLOWLISTED_APPS) {
        if (hasPermissionsForCameraPrivacyAllowlist(callingPid, callingUid)) {
            return false;
        } else {
            return true;
        }
    }
    return false;
}

void CameraService::logConnectionAttempt(int clientPid, const std::string& clientPackageName,
                                         const std::string& cameraId,
                                         apiLevel effectiveApiLevel) const {
    ALOGI("CameraService::connect call (PID %d \"%s\", camera ID %s) and "
          "Camera API version %d",
          clientPid, clientPackageName.c_str(), cameraId.c_str(),
          static_cast<int>(effectiveApiLevel));
}

template <class CALLBACK, class CLIENT>
Status CameraService::connectHelper(const sp<CALLBACK>& cameraCb, const std::string& cameraId,
                                    int api1CameraId,
                                    const AttributionSourceState& clientAttribution,
                                    bool systemNativeClient, apiLevel effectiveApiLevel,
                                    bool shimUpdateOnly, int oomScoreOffset, int targetSdkVersion,
                                    int rotationOverride, bool forceSlowJpegMode,
                                    const std::string& originalCameraId, bool isNonSystemNdk,
                                    bool sharedMode, bool isVendorClient,
                                    /*out*/ sp<CLIENT>& device) {
    binder::Status ret = binder::Status::ok();

    nsecs_t openTimeNs = systemTime();

    sp<CLIENT> client = nullptr;
    int facing = -1;
    int orientation = 0;

    const std::string clientPackageName =
            clientAttribution.packageName.value_or(kUnknownPackageName);

    sCurrPackageName = clientPackageName.c_str();

    {
        // Acquire mServiceLock and prevent other clients from connecting
        std::unique_ptr<AutoConditionLock> lock =
                AutoConditionLock::waitAndAcquire(mServiceLockWrapper, DEFAULT_CONNECT_TIMEOUT_NS);

        if (lock == nullptr) {
            ALOGE("CameraService::connect (PID %d) rejected (too many other clients connecting).",
                  clientAttribution.pid);
            return STATUS_ERROR_FMT(
                    ERROR_MAX_CAMERAS_IN_USE,
                    "Cannot open camera %s for \"%s\" (PID %d): Too many other clients connecting",
                    cameraId.c_str(), clientPackageName.c_str(), clientAttribution.pid);
        }

        // Enforce client permissions and do basic validity checks
        if (!(ret = validateConnectLocked(cameraId, clientAttribution, sharedMode)).isOk()) {
            return ret;
        }

        // Check the shim parameters after acquiring lock, if they have already been updated and
        // we were doing a shim update, return immediately
        if (shimUpdateOnly) {
            auto cameraState = getCameraState(cameraId);
            if (cameraState != nullptr) {
                if (!cameraState->getShimParams().isEmpty()) return ret;
            }
        }

        status_t err;

        sp<BasicClient> clientTmp = nullptr;
        std::shared_ptr<resource_policy::ClientDescriptor<std::string, sp<BasicClient>>> partial;
        if ((err = handleEvictionsLocked(
                     cameraId, clientAttribution.pid, effectiveApiLevel,
                     IInterface::asBinder(cameraCb),
                     clientAttribution.packageName.value_or(kUnknownPackageName), oomScoreOffset,
                     systemNativeClient, sharedMode, /*out*/ &clientTmp,
                     /*out*/ &partial)) != NO_ERROR) {
            switch (err) {
                case -ENODEV:
                    return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                            "No camera device with ID \"%s\" currently available",
                            cameraId.c_str());
                case -EBUSY:
                    return STATUS_ERROR_FMT(ERROR_CAMERA_IN_USE,
                            "Higher-priority client using camera, ID \"%s\" currently unavailable",
                            cameraId.c_str());
                case -EUSERS:
                    return STATUS_ERROR_FMT(ERROR_MAX_CAMERAS_IN_USE,
                            "Too many cameras already open, cannot open camera \"%s\"",
                            cameraId.c_str());
                default:
                    return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                            "Unexpected error %s (%d) opening camera \"%s\"",
                            strerror(-err), err, cameraId.c_str());
            }
        }

        if (clientTmp.get() != nullptr) {
            // Handle special case for API1 MediaRecorder where the existing client is returned
            device = static_cast<CLIENT*>(clientTmp.get());
            return ret;
        }

        // give flashlight a chance to close devices if necessary.
        mFlashlight->prepareDeviceOpen(cameraId);

        int portraitRotation;
        auto deviceVersionAndTransport =
                getDeviceVersion(cameraId, rotationOverride, /*out*/&portraitRotation,
                        /*out*/&facing, /*out*/&orientation);
        if (facing == -1) {
            ALOGE("%s: Unable to get camera device \"%s\"  facing", __FUNCTION__, cameraId.c_str());
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Unable to get camera device \"%s\" facing", cameraId.c_str());
        }

        sp<BasicClient> tmp = nullptr;
        bool overrideForPerfClass = SessionConfigurationUtils::targetPerfClassPrimaryCamera(
                mPerfClassPrimaryCameraIds, cameraId, targetSdkVersion);

        // Only use passed in clientPid to check permission. Use calling PID as the client PID
        // that's connected to camera service directly.
        if (!(ret = makeClient(this, cameraCb, clientAttribution, getCallingPid(),
                               systemNativeClient, cameraId, api1CameraId, facing, orientation,
                               getpid(), deviceVersionAndTransport, effectiveApiLevel,
                               overrideForPerfClass, rotationOverride, forceSlowJpegMode,
                               originalCameraId, sharedMode, isVendorClient,
                               /*out*/ &tmp))
                     .isOk()) {
            return ret;
        }
        client = static_cast<CLIENT*>(tmp.get());

        LOG_ALWAYS_FATAL_IF(client.get() == nullptr, "%s: CameraService in invalid state",
                __FUNCTION__);

        std::string monitorTags = isClientWatched(client.get()) ? mMonitorTags : std::string();
        err = client->initialize(mCameraProviderManager, monitorTags);
        if (err != OK) {
            ALOGE("%s: Could not initialize client from HAL.", __FUNCTION__);
            // Errors could be from the HAL module open call or from AppOpsManager
            mServiceLock.unlock();
            client->disconnect();
            mServiceLock.lock();
            switch(err) {
                case BAD_VALUE:
                    return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                            "Illegal argument to HAL module for camera \"%s\"", cameraId.c_str());
                case -EBUSY:
                    return STATUS_ERROR_FMT(ERROR_CAMERA_IN_USE,
                            "Camera \"%s\" is already open", cameraId.c_str());
                case -EUSERS:
                    return STATUS_ERROR_FMT(ERROR_MAX_CAMERAS_IN_USE,
                            "Too many cameras already open, cannot open camera \"%s\"",
                            cameraId.c_str());
                case PERMISSION_DENIED:
                    return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                            "No permission to open camera \"%s\"", cameraId.c_str());
                case -EACCES:
                    return STATUS_ERROR_FMT(ERROR_DISABLED,
                            "Camera \"%s\" disabled by policy", cameraId.c_str());
                case -ENODEV:
                default:
                    return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                            "Failed to initialize camera \"%s\": %s (%d)", cameraId.c_str(),
                            strerror(-err), err);
            }
        }

        // Update shim paremeters for legacy clients
        if (effectiveApiLevel == API_1) {
            // Assume we have always received a Client subclass for API1
            sp<Client> shimClient = reinterpret_cast<Client*>(client.get());
            String8 rawParams = shimClient->getParameters();
            CameraParameters params(rawParams);

            auto cameraState = getCameraState(cameraId);
            if (cameraState != nullptr) {
                cameraState->setShimParams(params);
            } else {
                ALOGE("%s: Cannot update shim parameters for camera %s, no such device exists.",
                        __FUNCTION__, cameraId.c_str());
            }
        }

        // Enable/disable camera service watchdog
        client->setCameraServiceWatchdog(mCameraServiceWatchdogEnabled);

        CameraMetadata chars;
        bool rotateAndCropSupported = true;
        err = mCameraProviderManager->getCameraCharacteristics(cameraId, overrideForPerfClass,
                &chars, rotationOverride);
        if (err == OK) {
            auto availableRotateCropEntry = chars.find(
                    ANDROID_SCALER_AVAILABLE_ROTATE_AND_CROP_MODES);
            if (availableRotateCropEntry.count <= 1) {
                rotateAndCropSupported = false;
            }
        } else {
            ALOGE("%s: Unable to query static metadata for camera %s: %s (%d)", __FUNCTION__,
                    cameraId.c_str(), strerror(-err), err);
        }

        if (rotateAndCropSupported) {
            // Set rotate-and-crop override behavior
            if (mOverrideRotateAndCropMode != ANDROID_SCALER_ROTATE_AND_CROP_AUTO) {
                client->setRotateAndCropOverride(mOverrideRotateAndCropMode);
            } else if (rotationOverride != hardware::ICameraService::ROTATION_OVERRIDE_NONE &&
                    portraitRotation != 0) {
                uint8_t rotateAndCropMode = ANDROID_SCALER_ROTATE_AND_CROP_AUTO;
                switch (portraitRotation) {
                    case 90:
                        rotateAndCropMode = ANDROID_SCALER_ROTATE_AND_CROP_90;
                        break;
                    case 180:
                        rotateAndCropMode = ANDROID_SCALER_ROTATE_AND_CROP_180;
                        break;
                    case 270:
                        rotateAndCropMode = ANDROID_SCALER_ROTATE_AND_CROP_270;
                        break;
                    default:
                        ALOGE("Unexpected portrait rotation: %d", portraitRotation);
                        break;
                }
                // Here we're communicating to the client the chosen rotate
                // and crop mode to send to the HAL
                client->setRotateAndCropOverride(rotateAndCropMode);
            } else {
                client->setRotateAndCropOverride(
                        mCameraServiceProxyWrapper->getRotateAndCropOverride(
                                clientPackageName, facing,
                                multiuser_get_user_id(clientAttribution.uid)));
            }
        }

        bool autoframingSupported = true;
        auto availableAutoframingEntry = chars.find(ANDROID_CONTROL_AUTOFRAMING_AVAILABLE);
        if ((availableAutoframingEntry.count == 1) && (availableAutoframingEntry.data.u8[0] ==
                    ANDROID_CONTROL_AUTOFRAMING_AVAILABLE_FALSE)) {
            autoframingSupported = false;
        }

        if (autoframingSupported) {
            // Set autoframing override behaviour
            if (mOverrideAutoframingMode != ANDROID_CONTROL_AUTOFRAMING_AUTO) {
                client->setAutoframingOverride(mOverrideAutoframingMode);
            } else {
                client->setAutoframingOverride(
                    mCameraServiceProxyWrapper->getAutoframingOverride(
                        clientPackageName));
            }
        }

        bool isCameraPrivacyEnabled;
        if (flags::camera_privacy_allowlist()) {
            // Set camera muting behavior.
            isCameraPrivacyEnabled =
                    this->isCameraPrivacyEnabled(toString16(client->getPackageName()), cameraId,
                                                 clientAttribution.pid, clientAttribution.uid);
        } else {
            isCameraPrivacyEnabled =
                    mSensorPrivacyPolicy->isCameraPrivacyEnabled();
        }

        if (client->supportsCameraMute()) {
            client->setCameraMute(
                    mOverrideCameraMuteMode || isCameraPrivacyEnabled);
        } else if (isCameraPrivacyEnabled) {
            // no camera mute supported, but privacy is on! => disconnect
            ALOGI("Camera mute not supported for package: %s, camera id: %s",
                    client->getPackageName().c_str(), cameraId.c_str());
            // Do not hold mServiceLock while disconnecting clients, but
            // retain the condition blocking other clients from connecting
            // in mServiceLockWrapper if held.
            mServiceLock.unlock();
            // Clear caller identity temporarily so client disconnect PID
            // checks work correctly
            int64_t token = clearCallingIdentity();
            // Note AppOp to trigger the "Unblock" dialog
            client->noteAppOp();
            client->disconnect();
            restoreCallingIdentity(token);
            // Reacquire mServiceLock
            mServiceLock.lock();

            return STATUS_ERROR_FMT(ERROR_DISABLED,
                    "Camera \"%s\" disabled due to camera mute", cameraId.c_str());
        }

        if (shimUpdateOnly) {
            // If only updating legacy shim parameters, immediately disconnect client
            mServiceLock.unlock();
            client->disconnect();
            mServiceLock.lock();
        } else {
            // Otherwise, add client to active clients list
            finishConnectLocked(client, partial, oomScoreOffset, systemNativeClient);
        }

        client->setImageDumpMask(mImageDumpMask);
        client->setStreamUseCaseOverrides(mStreamUseCaseOverrides);
        client->setZoomOverride(mZoomOverrideValue);
    } // lock is destroyed, allow further connect calls

    // Important: release the mutex here so the client can call back into the service from its
    // destructor (can be at the end of the call)
    device = client;

    int32_t openLatencyMs = ns2ms(systemTime() - openTimeNs);
    mCameraServiceProxyWrapper->logOpen(cameraId, facing, clientPackageName,
            effectiveApiLevel, isNonSystemNdk, openLatencyMs);

    {
        Mutex::Autolock lock(mInjectionParametersLock);
        if (cameraId == mInjectionInternalCamId && mInjectionInitPending) {
            mInjectionInitPending = false;
            status_t res = NO_ERROR;
            auto clientDescriptor = mActiveClientManager.get(mInjectionInternalCamId);
            if (clientDescriptor != nullptr) {
                sp<BasicClient> clientSp = clientDescriptor->getValue();
                res = checkIfInjectionCameraIsPresent(mInjectionExternalCamId, clientSp);
                if(res != OK) {
                    return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                            "No camera device with ID \"%s\" currently available",
                            mInjectionExternalCamId.c_str());
                }
                res = clientSp->injectCamera(mInjectionExternalCamId, mCameraProviderManager);
                if (res != OK) {
                    mInjectionStatusListener->notifyInjectionError(mInjectionExternalCamId, res);
                }
            } else {
                ALOGE("%s: Internal camera ID = %s 's client does not exist!",
                        __FUNCTION__, mInjectionInternalCamId.c_str());
                res = NO_INIT;
                mInjectionStatusListener->notifyInjectionError(mInjectionExternalCamId, res);
            }
        }
    }

    return ret;
}

status_t CameraService::addOfflineClient(const std::string &cameraId,
        sp<BasicClient> offlineClient) {
    if (offlineClient.get() == nullptr) {
        return BAD_VALUE;
    }

    {
        // Acquire mServiceLock and prevent other clients from connecting
        std::unique_ptr<AutoConditionLock> lock =
                AutoConditionLock::waitAndAcquire(mServiceLockWrapper, DEFAULT_CONNECT_TIMEOUT_NS);

        if (lock == nullptr) {
            ALOGE("%s: (PID %d) rejected (too many other clients connecting)."
                    , __FUNCTION__, offlineClient->getClientCallingPid());
            return TIMED_OUT;
        }

        auto onlineClientDesc = mActiveClientManager.get(cameraId);
        if (onlineClientDesc.get() == nullptr) {
            ALOGE("%s: No active online client using camera id: %s", __FUNCTION__,
                    cameraId.c_str());
            return BAD_VALUE;
        }

        // Offline clients do not evict or conflict with other online devices. Resource sharing
        // conflicts are handled by the camera provider which will either succeed or fail before
        // reaching this method.
        const auto& onlinePriority = onlineClientDesc->getPriority();
        auto offlineClientDesc = CameraClientManager::makeClientDescriptor(
                kOfflineDevice + onlineClientDesc->getKey(), offlineClient, /*cost*/ 0,
                /*conflictingKeys*/ std::set<std::string>(), onlinePriority.getScore(),
                onlineClientDesc->getOwnerId(), onlinePriority.getState(),
                // native clients don't have offline processing support.
                /*ommScoreOffset*/ 0, /*systemNativeClient*/false, /*sharedMode*/false);
        if (offlineClientDesc == nullptr) {
            ALOGE("%s: Offline client descriptor was NULL", __FUNCTION__);
            return BAD_VALUE;
        }

        // Allow only one offline device per camera
        auto incompatibleClients = mActiveClientManager.getIncompatibleClients(offlineClientDesc);
        if (!incompatibleClients.empty()) {
            ALOGE("%s: Incompatible offline clients present!", __FUNCTION__);
            return BAD_VALUE;
        }

        std::string monitorTags = isClientWatched(offlineClient.get())
                ? mMonitorTags : std::string();
        auto err = offlineClient->initialize(mCameraProviderManager, monitorTags);
        if (err != OK) {
            ALOGE("%s: Could not initialize offline client.", __FUNCTION__);
            return err;
        }

        auto evicted = mActiveClientManager.addAndEvict(offlineClientDesc);
        if (evicted.size() > 0) {
            for (auto& i : evicted) {
                ALOGE("%s: Invalid state: Offline client for camera %s was not removed ",
                        __FUNCTION__, i->getKey().c_str());
            }

            LOG_ALWAYS_FATAL("%s: Invalid state for CameraService, offline clients not evicted "
                    "properly", __FUNCTION__);

            return BAD_VALUE;
        }

        logConnectedOffline(offlineClientDesc->getKey(),
                static_cast<int>(offlineClientDesc->getOwnerId()),
                offlineClient->getPackageName());

        sp<IBinder> remoteCallback = offlineClient->getRemote();
        if (remoteCallback != nullptr) {
            remoteCallback->linkToDeath(this);
        }
    } // lock is destroyed, allow further connect calls

    return OK;
}

Status CameraService::turnOnTorchWithStrengthLevel(const std::string& unresolvedCameraId,
        int32_t torchStrength, const sp<IBinder>& clientBinder,
        const AttributionSourceState& clientAttribution, int32_t devicePolicy) {
    Mutex::Autolock lock(mServiceLock);

    ATRACE_CALL();
    if (clientBinder == nullptr) {
        ALOGE("%s: torch client binder is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                "Torch client binder in null.");
    }

    int uid = getCallingUid();
    std::optional<std::string> cameraIdOptional = resolveCameraId(unresolvedCameraId,
            clientAttribution.deviceId, devicePolicy);
    if (!cameraIdOptional.has_value()) {
        std::string msg = fmt::sprintf("Camera %s: Invalid camera id for device id %d",
                unresolvedCameraId.c_str(), clientAttribution.deviceId);
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    std::string cameraId = cameraIdOptional.value();

    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT, "Unable to change the strength level"
                "for system only device %s: ", cameraId.c_str());
    }

    // verify id is valid
    auto state = getCameraState(cameraId);
    if (state == nullptr) {
        ALOGE("%s: camera id is invalid %s", __FUNCTION__, cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
            "Camera ID \"%s\" is a not valid camera ID", cameraId.c_str());
    }

    StatusInternal cameraStatus = state->getStatus();
    if (cameraStatus != StatusInternal::NOT_AVAILABLE &&
            cameraStatus != StatusInternal::PRESENT) {
        ALOGE("%s: camera id is invalid %s, status %d", __FUNCTION__, cameraId.c_str(),
            (int)cameraStatus);
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                "Camera ID \"%s\" is a not valid camera ID", cameraId.c_str());
    }

    {
        Mutex::Autolock al(mTorchStatusMutex);
        TorchModeStatus status;
        status_t err = getTorchStatusLocked(cameraId, &status);
        if (err != OK) {
            if (err == NAME_NOT_FOUND) {
             return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Camera \"%s\" does not have a flash unit", cameraId.c_str());
            }
            ALOGE("%s: getting current torch status failed for camera %s",
                    __FUNCTION__, cameraId.c_str());
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Error changing torch strength level for camera \"%s\": %s (%d)",
                    cameraId.c_str(), strerror(-err), err);
        }

        if (status == TorchModeStatus::NOT_AVAILABLE) {
            if (cameraStatus == StatusInternal::NOT_AVAILABLE) {
                ALOGE("%s: torch mode of camera %s is not available because "
                        "camera is in use.", __FUNCTION__, cameraId.c_str());
                return STATUS_ERROR_FMT(ERROR_CAMERA_IN_USE,
                        "Torch for camera \"%s\" is not available due to an existing camera user",
                        cameraId.c_str());
            } else {
                ALOGE("%s: torch mode of camera %s is not available due to "
                       "insufficient resources", __FUNCTION__, cameraId.c_str());
                return STATUS_ERROR_FMT(ERROR_MAX_CAMERAS_IN_USE,
                        "Torch for camera \"%s\" is not available due to insufficient resources",
                        cameraId.c_str());
            }
        }
    }

    {
        Mutex::Autolock al(mTorchUidMapMutex);
        updateTorchUidMapLocked(cameraId, uid);
    }
    // Check if the current torch strength level is same as the new one.
    bool shouldSkipTorchStrengthUpdates = mCameraProviderManager->shouldSkipTorchStrengthUpdate(
            cameraId, torchStrength);

    status_t err = mFlashlight->turnOnTorchWithStrengthLevel(cameraId, torchStrength);

    if (err != OK) {
        int32_t errorCode;
        std::string msg;
        switch (err) {
            case -ENOSYS:
                msg = fmt::sprintf("Camera \"%s\" has no flashlight.",
                    cameraId.c_str());
                errorCode = ERROR_ILLEGAL_ARGUMENT;
                break;
            case -EBUSY:
                msg = fmt::sprintf("Camera \"%s\" is in use",
                    cameraId.c_str());
                errorCode = ERROR_CAMERA_IN_USE;
                break;
            case -EINVAL:
                msg = fmt::sprintf("Torch strength level %d is not within the "
                        "valid range.", torchStrength);
                errorCode = ERROR_ILLEGAL_ARGUMENT;
                break;
            default:
                msg = "Changing torch strength level failed.";
                errorCode = ERROR_INVALID_OPERATION;
        }
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(errorCode, msg.c_str());
    }

    {
        // update the link to client's death
        // Store the last client that turns on each camera's torch mode.
        Mutex::Autolock al(mTorchClientMapMutex);
        ssize_t index = mTorchClientMap.indexOfKey(cameraId);
        if (index == NAME_NOT_FOUND) {
            mTorchClientMap.add(cameraId, clientBinder);
        } else {
            mTorchClientMap.valueAt(index)->unlinkToDeath(this);
            mTorchClientMap.replaceValueAt(index, clientBinder);
        }
        clientBinder->linkToDeath(this);
    }

    int clientPid = getCallingPid();
    ALOGI("%s: Torch strength for camera id %s changed to %d for client PID %d",
            __FUNCTION__, cameraId.c_str(), torchStrength, clientPid);
    if (!shouldSkipTorchStrengthUpdates) {
        broadcastTorchStrengthLevel(cameraId, torchStrength);
    }
    return Status::ok();
}

Status CameraService::setTorchMode(const std::string& unresolvedCameraId, bool enabled,
        const sp<IBinder>& clientBinder, const AttributionSourceState& clientAttribution,
        int32_t devicePolicy) {
    Mutex::Autolock lock(mServiceLock);

    ATRACE_CALL();
    if (enabled && clientBinder == nullptr) {
        ALOGE("%s: torch client binder is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                "Torch client Binder is null");
    }

    int uid = getCallingUid();
    std::optional<std::string> cameraIdOptional = resolveCameraId(unresolvedCameraId,
            clientAttribution.deviceId, devicePolicy);
    if (!cameraIdOptional.has_value()) {
        std::string msg = fmt::sprintf("Camera %s: Invalid camera id for device id %d",
                unresolvedCameraId.c_str(), clientAttribution.deviceId);
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
    }
    std::string cameraId = cameraIdOptional.value();

    if (shouldRejectSystemCameraConnection(cameraId)) {
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT, "Unable to set torch mode"
                " for system only device %s: ", cameraId.c_str());
    }
    // verify id is valid.
    auto state = getCameraState(cameraId);
    if (state == nullptr) {
        ALOGE("%s: camera id is invalid %s", __FUNCTION__, cameraId.c_str());
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                "Camera ID \"%s\" is a not valid camera ID", cameraId.c_str());
    }

    StatusInternal cameraStatus = state->getStatus();
    if (cameraStatus != StatusInternal::PRESENT &&
            cameraStatus != StatusInternal::NOT_AVAILABLE) {
        ALOGE("%s: camera id is invalid %s, status %d", __FUNCTION__, cameraId.c_str(),
                (int)cameraStatus);
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                "Camera ID \"%s\" is a not valid camera ID", cameraId.c_str());
    }

    {
        Mutex::Autolock al(mTorchStatusMutex);
        TorchModeStatus status;
        status_t err = getTorchStatusLocked(cameraId, &status);
        if (err != OK) {
            if (err == NAME_NOT_FOUND) {
                return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                        "Camera \"%s\" does not have a flash unit", cameraId.c_str());
            }
            ALOGE("%s: getting current torch status failed for camera %s",
                    __FUNCTION__, cameraId.c_str());
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Error updating torch status for camera \"%s\": %s (%d)", cameraId.c_str(),
                    strerror(-err), err);
        }

        if (status == TorchModeStatus::NOT_AVAILABLE) {
            if (cameraStatus == StatusInternal::NOT_AVAILABLE) {
                ALOGE("%s: torch mode of camera %s is not available because "
                        "camera is in use", __FUNCTION__, cameraId.c_str());
                return STATUS_ERROR_FMT(ERROR_CAMERA_IN_USE,
                        "Torch for camera \"%s\" is not available due to an existing camera user",
                        cameraId.c_str());
            } else {
                ALOGE("%s: torch mode of camera %s is not available due to "
                        "insufficient resources", __FUNCTION__, cameraId.c_str());
                return STATUS_ERROR_FMT(ERROR_MAX_CAMERAS_IN_USE,
                        "Torch for camera \"%s\" is not available due to insufficient resources",
                        cameraId.c_str());
            }
        }
    }

    {
        // Update UID map - this is used in the torch status changed callbacks, so must be done
        // before setTorchMode
        Mutex::Autolock al(mTorchUidMapMutex);
        updateTorchUidMapLocked(cameraId, uid);
    }

    status_t err = mFlashlight->setTorchMode(cameraId, enabled);

    if (err != OK) {
        int32_t errorCode;
        std::string msg;
        switch (err) {
            case -ENOSYS:
                msg = fmt::sprintf("Camera \"%s\" has no flashlight",
                    cameraId.c_str());
                errorCode = ERROR_ILLEGAL_ARGUMENT;
                break;
            case -EBUSY:
                msg = fmt::sprintf("Camera \"%s\" is in use",
                    cameraId.c_str());
                errorCode = ERROR_CAMERA_IN_USE;
                break;
            default:
                msg = fmt::sprintf(
                    "Setting torch mode of camera \"%s\" to %d failed: %s (%d)",
                    cameraId.c_str(), enabled, strerror(-err), err);
                errorCode = ERROR_INVALID_OPERATION;
        }
        ALOGE("%s: %s", __FUNCTION__, msg.c_str());
        logServiceError(msg, errorCode);
        return STATUS_ERROR(errorCode, msg.c_str());
    }

    {
        // update the link to client's death
        Mutex::Autolock al(mTorchClientMapMutex);
        ssize_t index = mTorchClientMap.indexOfKey(cameraId);
        if (enabled) {
            if (index == NAME_NOT_FOUND) {
                mTorchClientMap.add(cameraId, clientBinder);
            } else {
                mTorchClientMap.valueAt(index)->unlinkToDeath(this);
                mTorchClientMap.replaceValueAt(index, clientBinder);
            }
            clientBinder->linkToDeath(this);
        } else if (index != NAME_NOT_FOUND) {
            mTorchClientMap.valueAt(index)->unlinkToDeath(this);
        }
    }

    int clientPid = getCallingPid();
    std::string torchState = enabled ? "on" : "off";
    ALOGI("Torch for camera id %s turned %s for client PID %d", cameraId.c_str(),
            torchState.c_str(), clientPid);
    logTorchEvent(cameraId, torchState, clientPid);
    return Status::ok();
}

void CameraService::updateTorchUidMapLocked(const std::string& cameraId, int uid) {
    if (mTorchUidMap.find(cameraId) == mTorchUidMap.end()) {
        mTorchUidMap[cameraId].first = uid;
        mTorchUidMap[cameraId].second = uid;
    } else {
        // Set the pending UID
        mTorchUidMap[cameraId].first = uid;
    }
}

Status CameraService::notifySystemEvent(int32_t eventId,
        const std::vector<int32_t>& args) {
    const int pid = getCallingPid();
    const int selfPid = getpid();

    // Permission checks
    if (pid != selfPid) {
        // Ensure we're being called by system_server, or similar process with
        // permissions to notify the camera service about system events
        if (!checkCallingPermission(toString16(sCameraSendSystemEventsPermission))) {
            const int uid = getCallingUid();
            ALOGE("Permission Denial: cannot send updates to camera service about system"
                    " events from pid=%d, uid=%d", pid, uid);
            return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                    "No permission to send updates to camera service about system events"
                    " from pid=%d, uid=%d", pid, uid);
        }
    }

    ATRACE_CALL();

    switch(eventId) {
        case ICameraService::EVENT_USER_SWITCHED: {
            // Try to register for UID and sensor privacy policy updates, in case we're recovering
            // from a system server crash
            mUidPolicy->registerSelf();
            mSensorPrivacyPolicy->registerSelf();
            doUserSwitch(/*newUserIds*/ args);
            break;
        }
        case ICameraService::EVENT_USB_DEVICE_ATTACHED:
        case ICameraService::EVENT_USB_DEVICE_DETACHED: {
            if (args.size() != 1) {
                return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT,
                    "USB Device Event requires 1 argument");
            }

            // Notify CameraProviderManager for lazy HALs
            mCameraProviderManager->notifyUsbDeviceEvent(eventId,
                                                        std::to_string(args[0]));
            break;
        }
        case ICameraService::EVENT_NONE:
        default: {
            ALOGW("%s: Received invalid system event from system_server: %d", __FUNCTION__,
                    eventId);
            break;
        }
    }
    return Status::ok();
}

void CameraService::notifyMonitoredUids() {
    Mutex::Autolock lock(mStatusListenerLock);

    for (const auto& it : mListenerList) {
        auto ret = it->getListener()->onCameraAccessPrioritiesChanged();
        it->handleBinderStatus(ret, "%s: Failed to trigger permission callback for %d:%d: %d",
                __FUNCTION__, it->getListenerUid(), it->getListenerPid(), ret.exceptionCode());
    }
}

void CameraService::notifyMonitoredUids(const std::unordered_set<uid_t> &notifyUidSet) {
    Mutex::Autolock lock(mStatusListenerLock);

    for (const auto& it : mListenerList) {
        if (notifyUidSet.find(it->getListenerUid()) != notifyUidSet.end()) {
            ALOGV("%s: notifying uid %d", __FUNCTION__, it->getListenerUid());
            auto ret = it->getListener()->onCameraAccessPrioritiesChanged();
            it->handleBinderStatus(ret, "%s: Failed to trigger permission callback for %d:%d: %d",
                    __FUNCTION__, it->getListenerUid(), it->getListenerPid(), ret.exceptionCode());
        }
    }
}

void CameraService::updateSharedClientAccessPriorities(std::vector<int> sharedClientPids) {
    Mutex::Autolock lock(mServiceLock);
    if (!flags::camera_multi_client() || sharedClientPids.empty()) {
        return;
    }
    std::vector<int> scores(sharedClientPids.size());
    std::vector<int> states(sharedClientPids.size());
    status_t err = ProcessInfoService::getProcessStatesScoresFromPids(sharedClientPids.size(),
                &sharedClientPids[0], /*out*/&states[0], /*out*/&scores[0]);
    if (err != OK) {
        return;
    }
    for (size_t i = 0; i < sharedClientPids.size(); i++) {
        auto clientDescriptorPtr = mActiveClientManager.getSharedClient(sharedClientPids[i]);
        if (clientDescriptorPtr == nullptr) {
            continue;
        }
        const auto& clientPriority = clientDescriptorPtr->getPriority();
        int score = clientPriority.getScore();
        int state = clientPriority.getState();
        if ((score != scores[i])  || (state != states[i])){
            clientDescriptorPtr->setPriority(resource_policy::ClientPriority(scores[i], states[i],
                    false, 0));
            notifySharedClientPrioritiesChanged(clientDescriptorPtr->getKey());
        }
    }
}

void CameraService::notifySharedClientPrioritiesChanged(const std::string& cameraId) {
    if (!flags::camera_multi_client()) {
        return;
    }
    auto primaryClientDesc = mActiveClientManager.getPrimaryClient(cameraId);
    if (primaryClientDesc == nullptr) {
        return;
    }
    auto primaryClient = primaryClientDesc->getValue();
    if (primaryClient == nullptr) {
        return;
    }
    auto highestPriorityClient = mActiveClientManager.getHighestPrioritySharedClient(cameraId);
    if (highestPriorityClient == primaryClient) {
        return;
    }
    highestPriorityClient->setPrimaryClient(true);
    highestPriorityClient->notifyClientSharedAccessPriorityChanged(true);
    primaryClient->setPrimaryClient(false);
    primaryClient->notifyClientSharedAccessPriorityChanged(false);
}

Status CameraService::notifyDeviceStateChange(int64_t newState) {
    const int pid = getCallingPid();
    const int selfPid = getpid();

    // Permission checks
    if (pid != selfPid) {
        // Ensure we're being called by system_server, or similar process with
        // permissions to notify the camera service about system events
        if (!checkCallingPermission(toString16(sCameraSendSystemEventsPermission))) {
            const int uid = getCallingUid();
            ALOGE("Permission Denial: cannot send updates to camera service about device"
                    " state changes from pid=%d, uid=%d", pid, uid);
            return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                    "No permission to send updates to camera service about device state"
                    " changes from pid=%d, uid=%d", pid, uid);
        }
    }

    ATRACE_CALL();

    {
        Mutex::Autolock lock(mServiceLock);
        mDeviceState = newState;
    }

    mCameraProviderManager->notifyDeviceStateChange(newState);

    return Status::ok();
}

Status CameraService::notifyDisplayConfigurationChange() {
    ATRACE_CALL();
    const int callingPid = getCallingPid();
    const int selfPid = getpid();

    // Permission checks
    if (callingPid != selfPid) {
        // Ensure we're being called by system_server, or similar process with
        // permissions to notify the camera service about system events
        if (!checkCallingPermission(toString16(sCameraSendSystemEventsPermission))) {
            const int uid = getCallingUid();
            ALOGE("Permission Denial: cannot send updates to camera service about orientation"
                    " changes from pid=%d, uid=%d", callingPid, uid);
            return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                    "No permission to send updates to camera service about orientation"
                    " changes from pid=%d, uid=%d", callingPid, uid);
        }
    }

    Mutex::Autolock lock(mServiceLock);

    // Don't do anything if rotate-and-crop override via cmd is active
    if (mOverrideRotateAndCropMode != ANDROID_SCALER_ROTATE_AND_CROP_AUTO) return Status::ok();

    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr && !basicClient->getOverrideToPortrait()) {
                basicClient->setRotateAndCropOverride(
                        mCameraServiceProxyWrapper->getRotateAndCropOverride(
                                basicClient->getPackageName(),
                                basicClient->getCameraFacing(),
                                multiuser_get_user_id(basicClient->getClientUid())));
            }
        }
    }

    return Status::ok();
}

Status CameraService::getConcurrentCameraIds(
        std::vector<ConcurrentCameraIdCombination>* concurrentCameraIds) {
    ATRACE_CALL();
    if (!concurrentCameraIds) {
        ALOGE("%s: concurrentCameraIds is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "concurrentCameraIds is NULL");
    }

    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        logServiceError("Camera subsystem is not available", ERROR_DISCONNECTED);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem is not available");
    }
    // First call into the provider and get the set of concurrent camera
    // combinations
    std::vector<std::unordered_set<std::string>> concurrentCameraCombinations =
            mCameraProviderManager->getConcurrentCameraIds();
    for (auto &combination : concurrentCameraCombinations) {
        std::vector<std::pair<std::string, int32_t>> validCombination;
        int32_t firstDeviceId = kInvalidDeviceId;
        for (auto &cameraId : combination) {
            // if the camera state is not present, skip
            auto state = getCameraState(cameraId);
            if (state == nullptr) {
                ALOGW("%s: camera id %s does not exist", __FUNCTION__, cameraId.c_str());
                continue;
            }
            StatusInternal status = state->getStatus();
            if (status == StatusInternal::NOT_PRESENT || status == StatusInternal::ENUMERATING) {
                continue;
            }
            if (shouldRejectSystemCameraConnection(cameraId)) {
                continue;
            }
            auto [cameraOwnerDeviceId, mappedCameraId] =
                    mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(cameraId);
            if (firstDeviceId == kInvalidDeviceId) {
                firstDeviceId = cameraOwnerDeviceId;
            } else if (firstDeviceId != cameraOwnerDeviceId) {
                // Found an invalid combination which contains cameras with different device id's,
                // hence discard it.
                validCombination.clear();
                break;
            }
            validCombination.push_back({mappedCameraId, cameraOwnerDeviceId});
        }
        if (validCombination.size() != 0) {
            concurrentCameraIds->push_back(std::move(validCombination));
        }
    }
    return Status::ok();
}

Status CameraService::isConcurrentSessionConfigurationSupported(
        const std::vector<CameraIdAndSessionConfiguration>& cameraIdsAndSessionConfigurations,
        int targetSdkVersion, const AttributionSourceState& clientAttribution, int32_t devicePolicy,
        /*out*/bool* isSupported) {
    if (!isSupported) {
        ALOGE("%s: isSupported is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "isSupported is NULL");
    }

    if (!mInitialized) {
        ALOGE("%s: Camera HAL couldn't be initialized", __FUNCTION__);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem is not available");
    }

    for (auto cameraIdAndSessionConfiguration : cameraIdsAndSessionConfigurations) {
        std::optional<std::string> cameraIdOptional =
                resolveCameraId(cameraIdAndSessionConfiguration.mCameraId,
                        clientAttribution.deviceId, devicePolicy);
        if (!cameraIdOptional.has_value()) {
            std::string msg = fmt::sprintf("Camera %s: Invalid camera id for device id %d",
                    cameraIdAndSessionConfiguration.mCameraId.c_str(), clientAttribution.deviceId);
            ALOGE("%s: %s", __FUNCTION__, msg.c_str());
            return STATUS_ERROR(CameraService::ERROR_ILLEGAL_ARGUMENT, msg.c_str());
        }
        cameraIdAndSessionConfiguration.mCameraId = cameraIdOptional.value();
    }

    // Check for camera permissions
    int callingPid = getCallingPid();
    int callingUid = getCallingUid();
    bool hasCameraPermission = ((callingPid == getpid()) ||
            hasPermissionsForCamera(callingPid, callingUid,
                    devicePolicy == IVirtualDeviceManagerNative::DEVICE_POLICY_DEFAULT
                        ? kDefaultDeviceId : clientAttribution.deviceId));
    if (!hasCameraPermission) {
        return STATUS_ERROR(ERROR_PERMISSION_DENIED,
                "android.permission.CAMERA needed to call"
                "isConcurrentSessionConfigurationSupported");
    }

    status_t res =
            mCameraProviderManager->isConcurrentSessionConfigurationSupported(
                    cameraIdsAndSessionConfigurations, mPerfClassPrimaryCameraIds,
                    targetSdkVersion, isSupported);
    if (res != OK) {
        logServiceError("Unable to query session configuration support",
            ERROR_INVALID_OPERATION);
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION, "Unable to query session configuration "
                "support %s (%d)", strerror(-res), res);
    }
    return Status::ok();
}

Status CameraService::addListener(const sp<ICameraServiceListener>& listener,
        /*out*/
        std::vector<hardware::CameraStatus> *cameraStatuses) {
    return addListenerHelper(listener, cameraStatuses);
}

binder::Status CameraService::addListenerTest(const sp<hardware::ICameraServiceListener>& listener,
            std::vector<hardware::CameraStatus>* cameraStatuses) {
    return addListenerHelper(listener, cameraStatuses, false, true);
}

Status CameraService::addListenerHelper(const sp<ICameraServiceListener>& listener,
        /*out*/
        std::vector<hardware::CameraStatus> *cameraStatuses,
        bool isVendorListener, bool isProcessLocalTest) {
    ATRACE_CALL();

    ALOGV("%s: Add listener %p", __FUNCTION__, listener.get());

    if (listener == nullptr) {
        ALOGE("%s: Listener must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Null listener given to addListener");
    }

    auto clientPid = getCallingPid();
    auto clientUid = getCallingUid();
    bool openCloseCallbackAllowed = hasPermissionsForOpenCloseListener(clientPid, clientUid);

    Mutex::Autolock lock(mServiceLock);

    {
        Mutex::Autolock lock(mStatusListenerLock);
        for (const auto &it : mListenerList) {
            if (IInterface::asBinder(it->getListener()) == IInterface::asBinder(listener)) {
                ALOGW("%s: Tried to add listener %p which was already subscribed",
                      __FUNCTION__, listener.get());
                return STATUS_ERROR(ERROR_ALREADY_EXISTS, "Listener already registered");
            }
        }

        sp<ServiceListener> serviceListener =
                new ServiceListener(this, listener, clientUid, clientPid, isVendorListener,
                        openCloseCallbackAllowed);
        auto ret = serviceListener->initialize(isProcessLocalTest);
        if (ret != NO_ERROR) {
            std::string msg = fmt::sprintf("Failed to initialize service listener: %s (%d)",
                    strerror(-ret), ret);
            logServiceError(msg, ERROR_ILLEGAL_ARGUMENT);
            ALOGE("%s: %s", __FUNCTION__, msg.c_str());
            return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, msg.c_str());
        }
        // The listener still needs to be added to the list of listeners, regardless of what
        // permissions the listener process has / whether it is a vendor listener. Since it might be
        // eligible to listen to other camera ids.
        mListenerList.emplace_back(serviceListener);
        mUidPolicy->registerMonitorUid(clientUid, /*openCamera*/false);
    }

    /* Collect current devices and status */
    {
        Mutex::Autolock lock(mCameraStatesLock);
        for (auto& i : mCameraStates) {
            // Get the device id and app-visible camera id for the given HAL-visible camera id.
            auto [deviceId, mappedCameraId] =
                    mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(i.first);

            cameraStatuses->emplace_back(mappedCameraId,
                    mapToInterface(i.second->getStatus()), i.second->getUnavailablePhysicalIds(),
                    openCloseCallbackAllowed ? i.second->getClientPackage() : std::string(),
                    deviceId);
        }
    }
    // Remove the camera statuses that should be hidden from the client, we do
    // this after collecting the states in order to avoid holding
    // mCameraStatesLock and mInterfaceLock (held in getSystemCameraKind()) at
    // the same time.
    cameraStatuses->erase(std::remove_if(cameraStatuses->begin(), cameraStatuses->end(),
                [this, &isVendorListener, &clientPid, &clientUid](const hardware::CameraStatus& s) {
                        std::string cameraId = s.cameraId;
                        std::optional<std::string> cameraIdOptional = resolveCameraId(s.cameraId,
                                s.deviceId, IVirtualDeviceManagerNative::DEVICE_POLICY_CUSTOM);
                        if (!cameraIdOptional.has_value()) {
                            std::string msg =
                                    fmt::sprintf(
                                            "Camera %s: Invalid camera id for device id %d",
                                            s.cameraId.c_str(), s.deviceId);
                            ALOGE("%s: %s", __FUNCTION__, msg.c_str());
                            return true;
                        }
                        cameraId = cameraIdOptional.value();
                        SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
                        if (getSystemCameraKind(cameraId, &deviceKind) != OK) {
                            ALOGE("%s: Invalid camera id %s, skipping status update",
                                    __FUNCTION__, s.cameraId.c_str());
                            return true;
                        }
                        return shouldSkipStatusUpdates(deviceKind, isVendorListener, clientPid,
                                clientUid);
                     }), cameraStatuses->end());

    // cameraStatuses will have non-eligible camera ids removed.
    std::set<std::string> idsChosenForCallback;
    for (const auto &s : *cameraStatuses) {
        // Add only default device cameras here, as virtual cameras currently don't support torch
        // anyway. Note that this is a simplification of the implementation here, and we should
        // change this when virtual cameras support torch.
        if (s.deviceId == kDefaultDeviceId) {
            idsChosenForCallback.insert(s.cameraId);
        }
    }

    /*
     * Immediately signal current torch status to this listener only
     * This may be a subset of all the devices, so don't include it in the response directly
     */
    {
        Mutex::Autolock al(mTorchStatusMutex);
        for (size_t i = 0; i < mTorchStatusMap.size(); i++ ) {
            const std::string &id = mTorchStatusMap.keyAt(i);
            // The camera id is visible to the client. Fine to send torch
            // callback.
            if (idsChosenForCallback.find(id) != idsChosenForCallback.end()) {
                listener->onTorchStatusChanged(mapToInterface(mTorchStatusMap.valueAt(i)), id,
                        kDefaultDeviceId);
            }
        }
    }

    return Status::ok();
}

Status CameraService::removeListener(const sp<ICameraServiceListener>& listener) {
    ATRACE_CALL();

    ALOGV("%s: Remove listener %p", __FUNCTION__, listener.get());

    if (listener == 0) {
        ALOGE("%s: Listener must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Null listener given to removeListener");
    }

    Mutex::Autolock lock(mServiceLock);

    {
        Mutex::Autolock lock(mStatusListenerLock);
        for (auto it = mListenerList.begin(); it != mListenerList.end(); it++) {
            if (IInterface::asBinder((*it)->getListener()) == IInterface::asBinder(listener)) {
                mUidPolicy->unregisterMonitorUid((*it)->getListenerUid(), /*closeCamera*/false);
                IInterface::asBinder(listener)->unlinkToDeath(*it);
                mListenerList.erase(it);
                return Status::ok();
            }
        }
    }

    ALOGW("%s: Tried to remove a listener %p which was not subscribed",
          __FUNCTION__, listener.get());

    return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Unregistered listener given to removeListener");
}

Status CameraService::getLegacyParameters(int cameraId, /*out*/std::string* parameters) {

    ATRACE_CALL();
    ALOGV("%s: for camera ID = %d", __FUNCTION__, cameraId);

    if (parameters == NULL) {
        ALOGE("%s: parameters must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Parameters must not be null");
    }

    Status ret = Status::ok();

    CameraParameters shimParams;
    if (!(ret = getLegacyParametersLazy(cameraId, /*out*/&shimParams)).isOk()) {
        // Error logged by caller
        return ret;
    }

    String8 shimParamsString8 = shimParams.flatten();

    *parameters = toStdString(shimParamsString8);

    return ret;
}

Status CameraService::isHiddenPhysicalCamera(const std::string& cameraId,
        /*out*/ bool *isSupported) {
    ATRACE_CALL();

    ALOGV("%s: for camera ID = %s", __FUNCTION__, cameraId.c_str());
    *isSupported = mCameraProviderManager->isHiddenPhysicalCamera(cameraId);

    return Status::ok();
}

Status CameraService::injectCamera(
        const std::string& packageName, const std::string& internalCamId,
        const std::string& externalCamId,
        const sp<ICameraInjectionCallback>& callback,
        /*out*/
        sp<ICameraInjectionSession>* cameraInjectionSession) {
    ATRACE_CALL();

    if (!checkCallingPermission(toString16(sCameraInjectExternalCameraPermission))) {
        const int pid = getCallingPid();
        const int uid = getCallingUid();
        ALOGE("Permission Denial: can't inject camera pid=%d, uid=%d", pid, uid);
        return STATUS_ERROR(ERROR_PERMISSION_DENIED,
                "Permission Denial: no permission to inject camera");
    }

    // Do not allow any camera injection that injects or replaces a virtual camera.
    auto [deviceIdForInternalCamera, _] =
            mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(internalCamId);
    if (deviceIdForInternalCamera != kDefaultDeviceId) {
        return STATUS_ERROR(ICameraInjectionCallback::ERROR_INJECTION_UNSUPPORTED,
                "Cannot replace a virtual camera");
    }
    [[maybe_unused]] auto [deviceIdForExternalCamera, unusedMappedCameraId] =
            mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(externalCamId);
    if (deviceIdForExternalCamera != kDefaultDeviceId) {
        return STATUS_ERROR(ICameraInjectionCallback::ERROR_INJECTION_UNSUPPORTED,
                "Cannot inject a virtual camera to replace an internal camera");
    }

    ALOGV(
        "%s: Package name = %s, Internal camera ID = %s, External camera ID = "
        "%s",
        __FUNCTION__, packageName.c_str(),
        internalCamId.c_str(), externalCamId.c_str());

    {
        Mutex::Autolock lock(mInjectionParametersLock);
        mInjectionInternalCamId = internalCamId;
        mInjectionExternalCamId = externalCamId;
        mInjectionStatusListener->addListener(callback);
        *cameraInjectionSession = new CameraInjectionSession(this);
        status_t res = NO_ERROR;
        auto clientDescriptor = mActiveClientManager.get(mInjectionInternalCamId);
        // If the client already exists, we can directly connect to the camera device through the
        // client's injectCamera(), otherwise we need to wait until the client is established
        // (execute connectHelper()) before injecting the camera to the camera device.
        if (clientDescriptor != nullptr) {
            mInjectionInitPending = false;
            sp<BasicClient> clientSp = clientDescriptor->getValue();
            res = checkIfInjectionCameraIsPresent(mInjectionExternalCamId, clientSp);
            if(res != OK) {
                return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                        "No camera device with ID \"%s\" currently available",
                        mInjectionExternalCamId.c_str());
            }
            res = clientSp->injectCamera(mInjectionExternalCamId, mCameraProviderManager);
            if (res != OK) {
                mInjectionStatusListener->notifyInjectionError(mInjectionExternalCamId, res);
            }
        } else {
            mInjectionInitPending = true;
        }
    }

    return binder::Status::ok();
}

Status CameraService::reportExtensionSessionStats(
        const hardware::CameraExtensionSessionStats& stats, std::string* sessionKey /*out*/) {
    ALOGV("%s: reported %s", __FUNCTION__, stats.toString().c_str());
    *sessionKey = mCameraServiceProxyWrapper->updateExtensionStats(stats);
    return Status::ok();
}

void CameraService::removeByClient(const BasicClient* client) {
    Mutex::Autolock lock(mServiceLock);
    for (auto& i : mActiveClientManager.getAll()) {
        auto clientSp = i->getValue();
        if (clientSp.get() == client) {
            cacheClientTagDumpIfNeeded(client->mCameraIdStr, clientSp.get());
            mActiveClientManager.remove(i);
        }
    }
    updateAudioRestrictionLocked();
}

bool CameraService::isOnlyClient(const BasicClient* client) {
    Mutex::Autolock lock(mServiceLock);
    bool ret = true;
    if (!flags::camera_multi_client()) {
        return ret;
    }
    if (client != nullptr) {
        std::string camId = client->mCameraIdStr;
        for (const auto& i : mActiveClientManager.getAll()) {
            auto clientSp = i->getValue();
            auto curCamId = i->getKey();
            if (!curCamId.compare(camId) && clientSp.get() != client) {
                return false;
            }
        }
    }
    return ret;
}

bool CameraService::evictClientIdByRemote(const wp<IBinder>& remote) {
    bool ret = false;
    {
        // Acquire mServiceLock and prevent other clients from connecting
        std::unique_ptr<AutoConditionLock> lock =
                AutoConditionLock::waitAndAcquire(mServiceLockWrapper);

        std::vector<sp<BasicClient>> evicted;
        for (auto& i : mActiveClientManager.getAll()) {
            auto clientSp = i->getValue();
            if (clientSp.get() == nullptr) {
                ALOGE("%s: Dead client still in mActiveClientManager.", __FUNCTION__);
                mActiveClientManager.remove(i);
                continue;
            }
            if (remote == clientSp->getRemote()) {
                mActiveClientManager.remove(i);
                evicted.push_back(clientSp);

                // Notify the client of disconnection
                clientSp->notifyError(
                        hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                        CaptureResultExtras());
            }
        }

        // Do not hold mServiceLock while disconnecting clients, but retain the condition blocking
        // other clients from connecting in mServiceLockWrapper if held
        mServiceLock.unlock();

        // Do not clear caller identity, remote caller should be client proccess

        for (auto& i : evicted) {
            if (i.get() != nullptr) {
                i->disconnect();
                ret = true;
            }
        }
        //clear the evicted client list before acquring service lock again.
        evicted.clear();
        // Reacquire mServiceLock
        mServiceLock.lock();

    } // lock is destroyed, allow further connect calls

    return ret;
}

std::shared_ptr<CameraService::CameraState> CameraService::getCameraState(
        const std::string& cameraId) const {
    std::shared_ptr<CameraState> state;
    {
        Mutex::Autolock lock(mCameraStatesLock);
        auto iter = mCameraStates.find(cameraId);
        if (iter != mCameraStates.end()) {
            state = iter->second;
        }
    }
    return state;
}

std::vector<sp<CameraService::BasicClient>> CameraService::removeClientsLocked(
        const std::string& cameraId) {
    // Remove from active clients list
    std::vector<sp<CameraService::BasicClient>> clients;
    if (flags::camera_multi_client()) {
        std::vector<CameraService::DescriptorPtr> clientDescriptors;
        clientDescriptors =  mActiveClientManager.removeAll(cameraId);
        for (const auto& clientDescriptorPtr : clientDescriptors) {
            if (clientDescriptorPtr != nullptr) {
                sp<BasicClient> client = clientDescriptorPtr->getValue();
                if (client.get() != nullptr) {
                    cacheClientTagDumpIfNeeded(clientDescriptorPtr->getKey(), client.get());
                    clients.push_back(client);
                }
            }
        }
    } else {
        auto clientDescriptorPtr = mActiveClientManager.remove(cameraId);
        if (clientDescriptorPtr == nullptr) {
            ALOGW("%s: Could not evict client, no client for camera ID %s", __FUNCTION__,
                    cameraId.c_str());
            return clients;
        }

        sp<BasicClient> client = clientDescriptorPtr->getValue();
        if (client.get() != nullptr) {
            cacheClientTagDumpIfNeeded(clientDescriptorPtr->getKey(), client.get());
            clients.push_back(client);
        }
    }
    return clients;
}

void CameraService::doUserSwitch(const std::vector<int32_t>& newUserIds) {
    // Acquire mServiceLock and prevent other clients from connecting
    std::unique_ptr<AutoConditionLock> lock =
            AutoConditionLock::waitAndAcquire(mServiceLockWrapper);

    std::set<userid_t> newAllowedUsers;
    for (size_t i = 0; i < newUserIds.size(); i++) {
        if (newUserIds[i] < 0) {
            ALOGE("%s: Bad user ID %d given during user switch, ignoring.",
                    __FUNCTION__, newUserIds[i]);
            return;
        }
        newAllowedUsers.insert(static_cast<userid_t>(newUserIds[i]));
    }


    if (newAllowedUsers == mAllowedUsers) {
        ALOGW("%s: Received notification of user switch with no updated user IDs.", __FUNCTION__);
        return;
    }

    logUserSwitch(mAllowedUsers, newAllowedUsers);

    mAllowedUsers = std::move(newAllowedUsers);

    // Current user has switched, evict all current clients.
    std::vector<sp<BasicClient>> evicted;
    for (auto& i : mActiveClientManager.getAll()) {
        auto clientSp = i->getValue();

        if (clientSp.get() == nullptr) {
            ALOGE("%s: Dead client still in mActiveClientManager.", __FUNCTION__);
            continue;
        }

        // Don't evict clients that are still allowed.
        uid_t clientUid = clientSp->getClientUid();
        userid_t clientUserId = multiuser_get_user_id(clientUid);
        if (mAllowedUsers.find(clientUserId) != mAllowedUsers.end()) {
            continue;
        }

        evicted.push_back(clientSp);

        ALOGE("Evicting conflicting client for camera ID %s due to user change",
                i->getKey().c_str());

        // Log the clients evicted
        logEvent(fmt::sprintf("EVICT device %s client held by package %s (PID %"
                PRId32 ", score %" PRId32 ", state %" PRId32 ")\n   - Evicted due"
                " to user switch.", i->getKey().c_str(),
                clientSp->getPackageName().c_str(),
                i->getOwnerId(), i->getPriority().getScore(),
                i->getPriority().getState()));

    }

    // Do not hold mServiceLock while disconnecting clients, but retain the condition
    // blocking other clients from connecting in mServiceLockWrapper if held.
    mServiceLock.unlock();

    // Clear caller identity temporarily so client disconnect PID checks work correctly
    int64_t token = clearCallingIdentity();

    for (auto& i : evicted) {
        i->disconnect();
    }

    restoreCallingIdentity(token);

    // Reacquire mServiceLock
    mServiceLock.lock();
}

void CameraService::logEvent(const std::string &event) {
    std::string curTime = getFormattedCurrentTime();
    Mutex::Autolock l(mLogLock);
    std::string msg = curTime + " : " + event;
    // For service error events, print the msg only once.
    if (msg.find("SERVICE ERROR") != std::string::npos) {
        mEventLog.add(msg);
    } else if(sServiceErrorEventSet.find(msg) == sServiceErrorEventSet.end()) {
        // Error event not added to the dumpsys log before
        mEventLog.add(msg);
        sServiceErrorEventSet.insert(msg);
    }
}

void CameraService::logDisconnected(const std::string &cameraId, int clientPid,
        const std::string &clientPackage) {
    // Log the clients evicted
    logEvent(fmt::sprintf("DISCONNECT device %s client for package %s (PID %d)", cameraId.c_str(),
            clientPackage.c_str(), clientPid));
}

void CameraService::logDisconnectedOffline(const std::string &cameraId, int clientPid,
        const std::string &clientPackage) {
    // Log the clients evicted
    logEvent(fmt::sprintf("DISCONNECT offline device %s client for package %s (PID %d)",
            cameraId.c_str(), clientPackage.c_str(), clientPid));
}

void CameraService::logConnected(const std::string &cameraId, int clientPid,
        const std::string &clientPackage) {
    // Log the clients evicted
    logEvent(fmt::sprintf("CONNECT device %s client for package %s (PID %d)", cameraId.c_str(),
            clientPackage.c_str(), clientPid));
}

void CameraService::logConnectedOffline(const std::string &cameraId, int clientPid,
        const std::string &clientPackage) {
    // Log the clients evicted
    logEvent(fmt::sprintf("CONNECT offline device %s client for package %s (PID %d)",
            cameraId.c_str(), clientPackage.c_str(), clientPid));
}

void CameraService::logRejected(const std::string &cameraId, int clientPid,
        const std::string &clientPackage, const std::string &reason) {
    // Log the client rejected
    logEvent(fmt::sprintf("REJECT device %s client for package %s (PID %d), reason: (%s)",
            cameraId.c_str(), clientPackage.c_str(), clientPid, reason.c_str()));
}

void CameraService::logTorchEvent(const std::string &cameraId, const std::string &torchState,
        int clientPid) {
    // Log torch event
    logEvent(fmt::sprintf("Torch for camera id %s turned %s for client PID %d", cameraId.c_str(),
            torchState.c_str(), clientPid));
}

void CameraService::logUserSwitch(const std::set<userid_t>& oldUserIds,
        const std::set<userid_t>& newUserIds) {
    std::string newUsers = toString(newUserIds);
    std::string oldUsers = toString(oldUserIds);
    if (oldUsers.size() == 0) {
        oldUsers = "<None>";
    }
    // Log the new and old users
    logEvent(fmt::sprintf("USER_SWITCH previous allowed user IDs: %s, current allowed user IDs: %s",
            oldUsers.c_str(), newUsers.c_str()));
}

void CameraService::logDeviceRemoved(const std::string &cameraId, const std::string &reason) {
    // Log the device removal
    logEvent(fmt::sprintf("REMOVE device %s, reason: (%s)", cameraId.c_str(), reason.c_str()));
}

void CameraService::logDeviceAdded(const std::string &cameraId, const std::string &reason) {
    // Log the device removal
    logEvent(fmt::sprintf("ADD device %s, reason: (%s)", cameraId.c_str(), reason.c_str()));
}

void CameraService::logClientDied(int clientPid, const std::string &reason) {
    // Log the device removal
    logEvent(fmt::sprintf("DIED client(s) with PID %d, reason: (%s)", clientPid, reason.c_str()));
}

void CameraService::logServiceError(const std::string &msg, int errorCode) {
    logEvent(fmt::sprintf("SERVICE ERROR: %s : %d (%s)", msg.c_str(), errorCode,
            strerror(-errorCode)));
}

status_t CameraService::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
        uint32_t flags) {

    // Permission checks
    switch (code) {
        case SHELL_COMMAND_TRANSACTION: {
            int in = data.readFileDescriptor();
            int out = data.readFileDescriptor();
            int err = data.readFileDescriptor();
            int argc = data.readInt32();
            Vector<String16> args;
            for (int i = 0; i < argc && data.dataAvail() > 0; i++) {
               args.add(data.readString16());
            }
            sp<IBinder> unusedCallback;
            sp<IResultReceiver> resultReceiver;
            status_t status;
            if ((status = data.readNullableStrongBinder(&unusedCallback)) != NO_ERROR) {
                return status;
            }
            if ((status = data.readNullableStrongBinder(&resultReceiver)) != NO_ERROR) {
                return status;
            }
            status = shellCommand(in, out, err, args);
            if (resultReceiver != nullptr) {
                resultReceiver->send(status);
            }
            return NO_ERROR;
        }
    }

    return BnCameraService::onTransact(code, data, reply, flags);
}

// We share the media players for shutter and recording sound for all clients.
// A reference count is kept to determine when we will actually release the
// media players.
sp<MediaPlayer> CameraService::newMediaPlayer(const char *file) {
    sp<MediaPlayer> mp = new MediaPlayer();
    status_t error;
    if ((error = mp->setDataSource(NULL /* httpService */, file, NULL)) == NO_ERROR) {
        mp->setAudioStreamType(AUDIO_STREAM_ENFORCED_AUDIBLE);
        error = mp->prepare();
    }
    if (error != NO_ERROR) {
        ALOGE("Failed to load CameraService sounds: %s", file);
        mp->disconnect();
        mp.clear();
        return nullptr;
    }
    return mp;
}

void CameraService::increaseSoundRef() {
    Mutex::Autolock lock(mSoundLock);
    mSoundRef++;
}

void CameraService::loadSoundLocked(sound_kind kind) {
    ATRACE_CALL();

    LOG1("CameraService::loadSoundLocked ref=%d", mSoundRef);
    if (SOUND_SHUTTER == kind && mSoundPlayer[SOUND_SHUTTER] == NULL) {
        mSoundPlayer[SOUND_SHUTTER] = newMediaPlayer("/product/media/audio/ui/camera_click.ogg");
        if (mSoundPlayer[SOUND_SHUTTER] == nullptr) {
            mSoundPlayer[SOUND_SHUTTER] = newMediaPlayer("/system/media/audio/ui/camera_click.ogg");
        }
    } else if (SOUND_RECORDING_START == kind && mSoundPlayer[SOUND_RECORDING_START] ==  NULL) {
        mSoundPlayer[SOUND_RECORDING_START] = newMediaPlayer("/product/media/audio/ui/VideoRecord.ogg");
        if (mSoundPlayer[SOUND_RECORDING_START] == nullptr) {
            mSoundPlayer[SOUND_RECORDING_START] =
                newMediaPlayer("/system/media/audio/ui/VideoRecord.ogg");
        }
    } else if (SOUND_RECORDING_STOP == kind && mSoundPlayer[SOUND_RECORDING_STOP] == NULL) {
        mSoundPlayer[SOUND_RECORDING_STOP] = newMediaPlayer("/product/media/audio/ui/VideoStop.ogg");
        if (mSoundPlayer[SOUND_RECORDING_STOP] == nullptr) {
            mSoundPlayer[SOUND_RECORDING_STOP] = newMediaPlayer("/system/media/audio/ui/VideoStop.ogg");
        }
    }
}

void CameraService::decreaseSoundRef() {
    Mutex::Autolock lock(mSoundLock);
    LOG1("CameraService::decreaseSoundRef ref=%d", mSoundRef);
    if (--mSoundRef) return;

    for (int i = 0; i < NUM_SOUNDS; i++) {
        if (mSoundPlayer[i] != 0) {
            mSoundPlayer[i]->disconnect();
            mSoundPlayer[i].clear();
        }
    }
}

void CameraService::playSound(sound_kind kind) {
    ATRACE_CALL();

    LOG1("playSound(%d)", kind);
    if (kind < 0 || kind >= NUM_SOUNDS) {
        ALOGE("%s: Invalid sound id requested: %d", __FUNCTION__, kind);
        return;
    }

    Mutex::Autolock lock(mSoundLock);
    loadSoundLocked(kind);
    sp<MediaPlayer> player = mSoundPlayer[kind];
    if (player != 0) {
        player->seekTo(0);
        player->start();
    }
}

// ----------------------------------------------------------------------------

CameraService::Client::Client(
        const sp<CameraService>& cameraService, const sp<ICameraClient>& cameraClient,
        std::shared_ptr<AttributionAndPermissionUtils> attributionAndPermissionUtils,
        const AttributionSourceState& clientAttribution, int callingPid, bool systemNativeClient,
        const std::string& cameraIdStr, int api1CameraId, int cameraFacing, int sensorOrientation,
        int servicePid, int rotationOverride, bool sharedMode)
    : CameraService::BasicClient(cameraService, IInterface::asBinder(cameraClient),
                                 attributionAndPermissionUtils, clientAttribution, callingPid,
                                 systemNativeClient, cameraIdStr, cameraFacing, sensorOrientation,
                                 servicePid, rotationOverride, sharedMode),
      mCameraId(api1CameraId) {
    LOG1("Client::Client E (pid %d, id %d)", callingPid, mCameraId);

    mRemoteCallback = cameraClient;

    cameraService->increaseSoundRef();

    LOG1("Client::Client X (pid %d, id %d)", callingPid, mCameraId);
}

// tear down the client
CameraService::Client::~Client() {
    ALOGV("~Client");
    mDestructionStarted = true;

    sCameraService->decreaseSoundRef();
    // unconditionally disconnect. function is idempotent
    Client::disconnect();
}

sp<CameraService> CameraService::BasicClient::BasicClient::sCameraService;

CameraService::BasicClient::BasicClient(
        const sp<CameraService>& cameraService, const sp<IBinder>& remoteCallback,
        std::shared_ptr<AttributionAndPermissionUtils> attributionAndPermissionUtils,
        const AttributionSourceState& clientAttribution, int callingPid, bool nativeClient,
        const std::string& cameraIdStr, int cameraFacing, int sensorOrientation, int servicePid,
        int rotationOverride, bool sharedMode)
    : AttributionAndPermissionUtilsEncapsulator(attributionAndPermissionUtils),
      mDestructionStarted(false),
      mCameraIdStr(cameraIdStr),
      mCameraFacing(cameraFacing),
      mOrientation(sensorOrientation),
      mClientAttribution(clientAttribution),
      mCallingPid(callingPid),
      mSystemNativeClient(nativeClient),
      mServicePid(servicePid),
      mDisconnected(false),
      mUidIsTrusted(false),
      mRotationOverride(rotationOverride), mSharedMode(sharedMode),
      mAudioRestriction(hardware::camera2::ICameraDeviceUser::AUDIO_RESTRICTION_NONE),
      mRemoteBinder(remoteCallback),
      mCameraOpen(false),
      mCameraStreaming(false) {
    if (sCameraService == nullptr) {
        sCameraService = cameraService;
    }

    // There are 2 scenarios in which a client won't have AppOps operations
    // (both scenarios : native clients)
    //    1) It's an system native client*, the package name will be empty
    //       and it will return from this function in the previous if condition
    //       (This is the same as the previously existing behavior).
    //    2) It is a system native client, but its package name has been
    //       modified for debugging, however it still must not use AppOps since
    //       the package name is not a real one.
    //
    //       * system native client - native client with UID < AID_APP_START. It
    //         doesn't exclude clients not on the system partition.
    if (!mSystemNativeClient) {
        mAppOpsManager = std::make_unique<AppOpsManager>();
    }

    mUidIsTrusted = isTrustedCallingUid(mClientAttribution.uid);
}

CameraService::BasicClient::~BasicClient() {
    ALOGV("~BasicClient");
    mDestructionStarted = true;
}

binder::Status CameraService::BasicClient::disconnect() {
    binder::Status res = Status::ok();
    if (mDisconnected) {
        return res;
    }
    mDisconnected = true;

    sCameraService->removeByClient(this);
    sCameraService->logDisconnected(mCameraIdStr, mCallingPid, getPackageName());
    if (!flags::camera_multi_client() || !mSharedMode || (mSharedMode &&
            sCameraService->isOnlyClient(this))) {
        // Remove the HAL reference for the camera in either of the following scenarios :
        // 1) Camera was opened in non-shared mode.
        // 2) Camera was opened in shared mode and this is the last client using
        //    the camera which is being disconnected
        sCameraService->mCameraProviderManager->removeRef(CameraProviderManager::DeviceMode::CAMERA,
                mCameraIdStr);
    }

    sp<IBinder> remote = getRemote();
    if (remote != nullptr) {
        remote->unlinkToDeath(sCameraService);
    }

    notifyCameraClosing();
    if (!flags::camera_multi_client() || !mSharedMode || (mSharedMode &&
            sCameraService->isOnlyClient(this))) {
        // Notify flashlight that a camera device is closed.
        sCameraService->mFlashlight->deviceClosed(mCameraIdStr);
    }
    ALOGI("%s: Disconnected client for camera %s for PID %d", __FUNCTION__, mCameraIdStr.c_str(),
            mCallingPid);

    // client shouldn't be able to call into us anymore
    mCallingPid = 0;

    const auto& mActivityManager = getActivityManager();
    if (mActivityManager) {
        mActivityManager->logFgsApiEnd(LOG_FGS_CAMERA_API,
            getCallingUid(),
            getCallingPid());
    }

    return res;
}

status_t CameraService::BasicClient::dump(int, const Vector<String16>&) {
    // No dumping of clients directly over Binder,
    // must go through CameraService::dump
    android_errorWriteWithInfoLog(SN_EVENT_LOG_ID, "26265403",
            getCallingUid(), NULL, 0);
    return OK;
}

status_t CameraService::BasicClient::startWatchingTags(const std::string&, int) {
    // Can't watch tags directly, must go through CameraService::startWatchingTags
    return OK;
}

status_t CameraService::BasicClient::stopWatchingTags(int) {
    // Can't watch tags directly, must go through CameraService::stopWatchingTags
    return OK;
}

status_t CameraService::BasicClient::dumpWatchedEventsToVector(std::vector<std::string> &) {
    // Can't watch tags directly, must go through CameraService::dumpWatchedEventsToVector
    return OK;
}

std::string CameraService::BasicClient::getPackageName() const {
    return mClientAttribution.packageName.value_or(kUnknownPackageName);
}

int CameraService::BasicClient::getCameraFacing() const {
    return mCameraFacing;
}

int CameraService::BasicClient::getCameraOrientation() const {
    return mOrientation;
}

int CameraService::BasicClient::getClientCallingPid() const {
    return mCallingPid;
}

uid_t CameraService::BasicClient::getClientUid() const {
    return mClientAttribution.uid;
}

const std::optional<std::string>& CameraService::BasicClient::getClientAttributionTag() const {
    return mClientAttribution.attributionTag;
}

bool CameraService::BasicClient::canCastToApiClient(apiLevel level) const {
    // Defaults to API2.
    return level == API_2;
}

status_t CameraService::BasicClient::setAudioRestriction(int32_t mode) {
    {
        Mutex::Autolock l(mAudioRestrictionLock);
        mAudioRestriction = mode;
    }
    sCameraService->updateAudioRestriction();
    return OK;
}

int32_t CameraService::BasicClient::getServiceAudioRestriction() const {
    return sCameraService->updateAudioRestriction();
}

int32_t CameraService::BasicClient::getAudioRestriction() const {
    Mutex::Autolock l(mAudioRestrictionLock);
    return mAudioRestriction;
}

bool CameraService::BasicClient::isValidAudioRestriction(int32_t mode) {
    switch (mode) {
        case hardware::camera2::ICameraDeviceUser::AUDIO_RESTRICTION_NONE:
        case hardware::camera2::ICameraDeviceUser::AUDIO_RESTRICTION_VIBRATION:
        case hardware::camera2::ICameraDeviceUser::AUDIO_RESTRICTION_VIBRATION_SOUND:
            return true;
        default:
            return false;
    }
}

status_t CameraService::BasicClient::handlePermissionResult(
        PermissionChecker::PermissionResult result) {
    if (result == PermissionChecker::PERMISSION_HARD_DENIED) {
        ALOGI("Camera %s: Access for \"%s\" has been revoked", mCameraIdStr.c_str(),
              getPackageName().c_str());
        return PERMISSION_DENIED;
    } else if (!mUidIsTrusted && result == PermissionChecker::PERMISSION_SOFT_DENIED) {
        // If the calling Uid is trusted (a native service), the AppOpsManager/PermissionChecker
        // could return MODE_IGNORED/PERMISSION_SOFT_DENIED. Do not treat such case as error.
        bool isUidActive =
                sCameraService->mUidPolicy->isUidActive(getClientUid(), getPackageName());

        bool isCameraPrivacyEnabled;
        if (flags::camera_privacy_allowlist()) {
            isCameraPrivacyEnabled = sCameraService->isCameraPrivacyEnabled(
                    toString16(getPackageName()), std::string(), mCallingPid, getClientUid());
        } else {
            isCameraPrivacyEnabled =
                sCameraService->mSensorPrivacyPolicy->isCameraPrivacyEnabled();
        }
        // We don't want to return EACCESS if the CameraPrivacy is enabled.
        // We prefer to successfully open the camera and perform camera muting
        // or blocking in connectHelper as handleAppOpMode can be called before the
        // connection has been fully established and at that time camera muting
        // capabilities are unknown.
        if (!isUidActive || !isCameraPrivacyEnabled) {
            ALOGI("Camera %s: Access for \"%s\" has been restricted."
                  "uid active: %s, privacy enabled: %s",
                  mCameraIdStr.c_str(), getPackageName().c_str(), isUidActive ? "true" : "false",
                  isCameraPrivacyEnabled ? "true" : "false");
            // Return the same error as for device policy manager rejection
            return -EACCES;
        }
    }
    return OK;
}

status_t CameraService::BasicClient::handleAppOpMode(int32_t mode) {
    return handlePermissionResult(appOpModeToPermissionResult(mode));
}

status_t CameraService::BasicClient::notifyCameraOpening() {
    ATRACE_CALL();

    // Don't start watching until we're streaming when using permissionChecker for data delivery
    if (!flags::data_delivery_permission_checks()) {
        ALOGD("%s: Start camera ops, package name = %s, client UID = %d", __FUNCTION__,
              getPackageName().c_str(), getClientUid());

        if (mAppOpsManager != nullptr) {
            // Notify app ops that the camera is not available
            mOpsCallback = new OpsCallback(this);

            mAppOpsManager->startWatchingMode(
                    AppOpsManager::OP_CAMERA, toString16(getPackageName()),
                    AppOpsManager::WATCH_FOREGROUND_CHANGES, mOpsCallback);

            // Just check for camera access here on open - delay startOp until
            // camera frames start streaming in startCameraStreamingOps
            int32_t mode = mAppOpsManager->checkOp(AppOpsManager::OP_CAMERA, getClientUid(),
                                                   toString16(getPackageName()));
            status_t res = handleAppOpMode(mode);
            if (res != OK) {
                return res;
            }
        }
    } else {
        // TODO: Remove when removing the data_delivery_permission_checks flag
        ALOGD("%s: Bypassing checkOp for uid %d", __FUNCTION__, getClientUid());
    }

    mCameraOpen = true;

#ifdef USES_MIUI_CAMERA
    // Configure miui camera mode
    if (String8(sCurrPackageName.c_str()) == "com.android.camera") {
        SetProperty("sys.camera.miui.apk", "1");
        ALOGI("Enabling miui camera mode");
    } else {
        SetProperty("sys.camera.miui.apk", "0");
        ALOGI("Disabling miui camera mode");
    }
#endif

    // Transition device availability listeners from PRESENT -> NOT_AVAILABLE
    sCameraService->updateStatus(StatusInternal::NOT_AVAILABLE, mCameraIdStr);

    sCameraService->mUidPolicy->registerMonitorUid(getClientUid(), /*openCamera*/ true);

    if (flags::camera_multi_client() && mSharedMode) {
        sCameraService->mUidPolicy->addSharedClientPid(getClientUid(), getClientCallingPid());
    }

    // Notify listeners of camera open/close status
    sCameraService->updateOpenCloseStatus(mCameraIdStr, true /*open*/, getPackageName(),
            mSharedMode);

    return OK;
}

status_t CameraService::BasicClient::startCameraStreamingOps() {
    ATRACE_CALL();

    if (!mCameraOpen) {
        ALOGE("%s: Calling streaming start when not yet active", __FUNCTION__);
        return INVALID_OPERATION;
    }

    if (mCameraStreaming) {
        ALOGV("%s: Streaming already active!", __FUNCTION__);
        return OK;
    }

    ALOGV("%s: Start camera streaming ops, package name = %s, client UID = %d", __FUNCTION__,
          getPackageName().c_str(), getClientUid());

    if (mAppOpsManager != nullptr) {
        if (flags::data_delivery_permission_checks()) {
            ALOGD("%s: Start data delivery for uid %d", __FUNCTION__, getClientUid());

            const PermissionChecker::PermissionResult result =
                    checkPermissionsForCameraForStartDataDelivery(mCameraIdStr, mClientAttribution);
            status_t res = handlePermissionResult(result);
            if (res != OK) {
                return res;
            }

            mOpsCallback = new OpsCallback(this);
            std::for_each(AttrSourceItr{mClientAttribution}, AttrSourceItr::end(),
                      [&](const auto& attr) {
                          mAppOpsManager->startWatchingMode(
                                  AppOpsManager::OP_CAMERA,
                                  toString16(attr.packageName.value_or("")),
                                  AppOpsManager::WATCH_FOREGROUND_CHANGES, mOpsCallback);
                      });
        } else {
            ALOGD("%s: startOp for uid %d", __FUNCTION__, getClientUid());
            int32_t mode = mAppOpsManager->startOpNoThrow(
                    AppOpsManager::OP_CAMERA, getClientUid(), toString16(getPackageName()),
                    /*startIfModeDefault*/ false, toString16(getClientAttributionTag()),
                    toString16("start camera ") + toString16(mCameraIdStr));
            status_t res = handleAppOpMode(mode);
            if (res != OK) {
                return res;
            }
        }
    }

    mCameraStreaming = true;

    return OK;
}

status_t CameraService::BasicClient::noteAppOp() {
    ATRACE_CALL();

    ALOGV("%s: Start camera noteAppOp, package name = %s, client UID = %d", __FUNCTION__,
          getPackageName().c_str(), getClientUid());

    // noteAppOp is only used for when camera mute is not supported, in order
    // to trigger the sensor privacy "Unblock" dialog
    if (flags::data_delivery_permission_checks()) {
        // Ignore the result, since we're only triggering the dialog
        ALOGD("%s: Check data delivery permissions for uid %d", __FUNCTION__, getClientUid());
        hasPermissionsForCameraForDataDelivery(std::string(), mClientAttribution);
    } else if (mAppOpsManager != nullptr) {
        ALOGD("%s: noteOp for uid %d", __FUNCTION__, getClientUid());
        int32_t mode = mAppOpsManager->noteOp(
                AppOpsManager::OP_CAMERA, getClientUid(), toString16(getPackageName()),
                toString16(getClientAttributionTag()),
                toString16("start camera ") + toString16(mCameraIdStr));
        status_t res = handleAppOpMode(mode);
        if (res != OK) {
            return res;
        }
    }

    return OK;
}

status_t CameraService::BasicClient::finishCameraStreamingOps() {
    ATRACE_CALL();

    if (!mCameraOpen) {
        ALOGE("%s: Calling streaming start when not yet active", __FUNCTION__);
        return INVALID_OPERATION;
    }
    if (!mCameraStreaming) {
        ALOGV("%s: Streaming not active!", __FUNCTION__);
        return OK;
    }

    if (mAppOpsManager != nullptr) {
        if (flags::data_delivery_permission_checks()) {
            ALOGD("%s: finishDataDelivery for uid %d", __FUNCTION__, getClientUid());
            finishDataDelivery(mClientAttribution);

            // Stop watching app op changes after stop streaming
            if (mOpsCallback != nullptr) {
                mAppOpsManager->stopWatchingMode(mOpsCallback);
                mOpsCallback.clear();
            }
        } else {
            ALOGD("%s: finishOp for uid %d", __FUNCTION__, getClientUid());
            mAppOpsManager->finishOp(AppOpsManager::OP_CAMERA, getClientUid(),
                                     toString16(getPackageName()),
                                     toString16(getClientAttributionTag()));
        }
        mCameraStreaming = false;
    }

    return OK;
}

status_t CameraService::BasicClient::notifyCameraClosing() {
    ATRACE_CALL();

    if (mCameraStreaming) {
        // Make sure we've notified everyone about camera stopping
        finishCameraStreamingOps();
    }

    // Check if notifyCameraOpening succeeded, and if so, finish the camera op if necessary
    if (mCameraOpen) {
        mCameraOpen = false;

        // This function is called when a client disconnects. This should
        // release the camera, but actually only if it was in a proper
        // functional state, i.e. with status NOT_AVAILABLE
        std::initializer_list<StatusInternal> rejected = {StatusInternal::PRESENT,
                StatusInternal::ENUMERATING, StatusInternal::NOT_PRESENT};

        // Transition to PRESENT if the camera is not in either of the rejected states
        if (!flags::camera_multi_client() || !mSharedMode || (mSharedMode
                && sCameraService->isOnlyClient(this))) {
            sCameraService->updateStatus(StatusInternal::PRESENT,
                    mCameraIdStr, rejected);
        }
    }

    // When using the data delivery permission checks, the open state does not involve AppOps
    if (!flags::data_delivery_permission_checks()) {
        // Always stop watching, even if no camera op is active
        if (mOpsCallback != nullptr && mAppOpsManager != nullptr) {
            mAppOpsManager->stopWatchingMode(mOpsCallback);
        }
        mOpsCallback.clear();
    }

    sCameraService->mUidPolicy->unregisterMonitorUid(getClientUid(), /*closeCamera*/ true);

    if (flags::camera_multi_client() && mSharedMode) {
        sCameraService->mUidPolicy->removeSharedClientPid(getClientUid(), getClientCallingPid());
    }

    // Notify listeners of camera open/close status
    sCameraService->updateOpenCloseStatus(mCameraIdStr, false /*open*/, getPackageName(),
            mSharedMode);

    return OK;
}

int32_t CameraService::getUidProcessState(int32_t uid) {
    const auto& activityManager = getActivityManager();
    int32_t procState = ActivityManager::PROCESS_STATE_NONEXISTENT;
    if (activityManager != nullptr) {
        procState = activityManager->getUidProcessState(uid, toString16(kServiceName));
    } else {
        ALOGE("%s: getActivityManager returned nullptr.", __FUNCTION__);
    }
    return procState;
}

void CameraService::BasicClient::opChanged(int32_t op, const String16&) {
    ATRACE_CALL();
    if (mAppOpsManager == nullptr) {
        return;
    }
    // TODO : add offline camera session case
    if (op != AppOpsManager::OP_CAMERA) {
        ALOGW("Unexpected app ops notification received: %d", op);
        return;
    }

    PermissionChecker::PermissionResult res;
    if (flags::data_delivery_permission_checks()) {
        int32_t appOpMode = AppOpsManager::MODE_ALLOWED;
        std::for_each(AttrSourceItr{mClientAttribution}, AttrSourceItr::end(),
                [&](const auto& attr) {
                    appOpMode = std::max(appOpMode, mAppOpsManager->checkOp(
                            AppOpsManager::OP_CAMERA, attr.uid,
                            toString16(attr.packageName.value_or(""))));
                });
        res = appOpModeToPermissionResult(appOpMode);
        ALOGV("checkOp returns: %d, %s ", appOpMode,
              appOpMode == AppOpsManager::MODE_ALLOWED   ? "ALLOWED"
              : appOpMode == AppOpsManager::MODE_IGNORED ? "IGNORED"
              : appOpMode == AppOpsManager::MODE_ERRORED ? "ERRORED"
                                                         : "UNKNOWN");
    } else {
        int32_t appOpMode = mAppOpsManager->checkOp(AppOpsManager::OP_CAMERA, getClientUid(),
                                                    toString16(getPackageName()));
        res = appOpModeToPermissionResult(appOpMode);
        ALOGV("checkOp returns: %d, %s ", appOpMode,
              appOpMode == AppOpsManager::MODE_ALLOWED   ? "ALLOWED"
              : appOpMode == AppOpsManager::MODE_IGNORED ? "IGNORED"
              : appOpMode == AppOpsManager::MODE_ERRORED ? "ERRORED"
                                                         : "UNKNOWN");
    }

    if (res == PermissionChecker::PERMISSION_HARD_DENIED) {
        ALOGI("Camera %s: Access for \"%s\" revoked", mCameraIdStr.c_str(),
              getPackageName().c_str());
        block();
    } else if (res == PermissionChecker::PERMISSION_SOFT_DENIED) {
        bool isUidActive =
                sCameraService->mUidPolicy->isUidActive(getClientUid(), getPackageName());

        // Uid may be active, but not visible to the user (e.g. PROCESS_STATE_FOREGROUND_SERVICE).
        // If not visible, but still active, then we want to block instead of muting the camera.
        int32_t procState = ActivityManager::PROCESS_STATE_NONEXISTENT;
        if (flags::data_delivery_permission_checks()) {
            // Use the proc state of the last uid in the chain (ultimately receiving the data)
            // when determining whether to mute or block
            int32_t uid = -1;
            std::for_each(AttrSourceItr{mClientAttribution}, AttrSourceItr::end(),
                      [&](const auto& attr) {
                          uid = static_cast<uid_t>(attr.uid);
                      });
            procState = getUidProcessState(uid);
        } else {
            procState = sCameraService->mUidPolicy->getProcState(getClientUid());
        }
        bool isUidVisible = (procState <= ActivityManager::PROCESS_STATE_BOUND_TOP);

        bool isCameraPrivacyEnabled;
        if (flags::camera_privacy_allowlist()) {
            isCameraPrivacyEnabled = sCameraService->isCameraPrivacyEnabled(
                    toString16(getPackageName()), std::string(), mCallingPid, getClientUid());
        } else {
            isCameraPrivacyEnabled =
                sCameraService->mSensorPrivacyPolicy->isCameraPrivacyEnabled();
        }

        ALOGI("Camera %s: Access for \"%s\" has been restricted, isUidTrusted %d, isUidActive %d"
              " isUidVisible %d, isCameraPrivacyEnabled %d procState %d",
              mCameraIdStr.c_str(), getPackageName().c_str(), mUidIsTrusted, isUidActive,
              isUidVisible, isCameraPrivacyEnabled, procState);
        // If the calling Uid is trusted (a native service), or the client Uid is active / visible
        // (WAR for b/175320666)the AppOpsManager could return MODE_IGNORED. Do not treat such
        // cases as error.
        if (!mUidIsTrusted) {
            if (isUidVisible && isCameraPrivacyEnabled && supportsCameraMute()) {
                setCameraMute(true);
            } else {
                block();
            }
        }
    } else if (res == PermissionChecker::PERMISSION_GRANTED) {
        setCameraMute(sCameraService->mOverrideCameraMuteMode);
    }
}

void CameraService::BasicClient::block() {
    ATRACE_CALL();

    // Reset the client PID to allow server-initiated disconnect,
    // and to prevent further calls by client.
    mCallingPid = getCallingPid();
    CaptureResultExtras resultExtras; // a dummy result (invalid)
    notifyError(hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISABLED, resultExtras);
    disconnect();
}

status_t CameraService::BasicClient::isPrimaryClient(bool* isPrimary) {
    ATRACE_CALL();
    if (!flags::camera_multi_client()) {
        return INVALID_OPERATION;
    }

    if (!mSharedMode) {
        return INVALID_OPERATION;
    }
    *isPrimary = mIsPrimaryClient;
    return OK;
}

status_t CameraService::BasicClient::setPrimaryClient(bool isPrimary) {
    ATRACE_CALL();

    if (!flags::camera_multi_client()) {
        return INVALID_OPERATION;
    }

    if (!mSharedMode) {
        return INVALID_OPERATION;
    }
    mIsPrimaryClient = isPrimary;
    return OK;
}

// ----------------------------------------------------------------------------

void CameraService::Client::notifyError(int32_t errorCode,
        [[maybe_unused]] const CaptureResultExtras& resultExtras) {
    if (mRemoteCallback != NULL) {
        int32_t api1ErrorCode = CAMERA_ERROR_RELEASED;
        if (errorCode == hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISABLED) {
            api1ErrorCode = CAMERA_ERROR_DISABLED;
        }
        mRemoteCallback->notifyCallback(CAMERA_MSG_ERROR, api1ErrorCode, 0);
    } else {
        ALOGE("mRemoteCallback is NULL!!");
    }
}

// NOTE: function is idempotent
binder::Status CameraService::Client::disconnect() {
    ALOGV("Client::disconnect");
    return BasicClient::disconnect();
}

bool CameraService::Client::canCastToApiClient(apiLevel level) const {
    return level == API_1;
}

CameraService::Client::OpsCallback::OpsCallback(wp<BasicClient> client):
        mClient(client) {
}

binder::Status CameraService::Client::OpsCallback::opChanged(int32_t op, int32_t,
        const String16& packageName, const String16&) {
    sp<BasicClient> client = mClient.promote();
    if (client != NULL) {
        client->opChanged(op, packageName);
    }
    return binder::Status::ok();
}

// ----------------------------------------------------------------------------
//                  UidPolicy
// ----------------------------------------------------------------------------

void CameraService::UidPolicy::registerWithActivityManager() {
    Mutex::Autolock _l(mUidLock);
    int32_t emptyUidArray[] = { };

    if (mRegistered) return;
    status_t res = mAm.linkToDeath(this);
    mAm.registerUidObserverForUids(this, ActivityManager::UID_OBSERVER_GONE
            | ActivityManager::UID_OBSERVER_IDLE
            | ActivityManager::UID_OBSERVER_ACTIVE | ActivityManager::UID_OBSERVER_PROCSTATE
            | ActivityManager::UID_OBSERVER_PROC_OOM_ADJ,
            ActivityManager::PROCESS_STATE_UNKNOWN,
            toString16(kServiceName), emptyUidArray, 0, mObserverToken);
    if (res == OK) {
        mRegistered = true;
        ALOGV("UidPolicy: Registered with ActivityManager");
    } else {
        ALOGE("UidPolicy: Failed to register with ActivityManager: 0x%08x", res);
    }
}

void CameraService::UidPolicy::onServiceRegistration(const String16& name, const sp<IBinder>&) {
    if (name != toString16(kActivityServiceName)) {
        return;
    }

    registerWithActivityManager();
}

void CameraService::UidPolicy::registerSelf() {
    // Use check service to see if the activity service is available
    // If not available then register for notifications, instead of blocking
    // till the service is ready
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->checkService(toString16(kActivityServiceName));
    if (!binder) {
        sm->registerForNotifications(toString16(kActivityServiceName), this);
    } else {
        registerWithActivityManager();
    }
}

void CameraService::UidPolicy::unregisterSelf() {
    Mutex::Autolock _l(mUidLock);

    mAm.unregisterUidObserver(this);
    mAm.unlinkToDeath(this);
    mRegistered = false;
    mActiveUids.clear();
    ALOGV("UidPolicy: Unregistered with ActivityManager");
}

void CameraService::UidPolicy::onUidGone(uid_t uid, bool disabled) {
    onUidIdle(uid, disabled);
}

void CameraService::UidPolicy::onUidActive(uid_t uid) {
    Mutex::Autolock _l(mUidLock);
    mActiveUids.insert(uid);
}

void CameraService::UidPolicy::onUidIdle(uid_t uid, bool /* disabled */) {
    bool deleted = false;
    {
        Mutex::Autolock _l(mUidLock);
        if (mActiveUids.erase(uid) > 0) {
            deleted = true;
        }
    }
    if (deleted) {
        sp<CameraService> service = mService.promote();
        if (service != nullptr) {
            service->blockClientsForUid(uid);
        }
    }
}

void CameraService::UidPolicy::onUidStateChanged(uid_t uid, int32_t procState,
        int64_t procStateSeq __unused, int32_t capability __unused) {
    bool procStateChange = false;
    std::vector<int> sharedPids;
    {
        Mutex::Autolock _l(mUidLock);
        if (mMonitoredUids.find(uid) != mMonitoredUids.end()) {
            if (mMonitoredUids[uid].procState != procState) {
                mMonitoredUids[uid].procState = procState;
                procStateChange = true;
            }
            if (flags::camera_multi_client()) {
                std::unordered_set<int> sharedClientPids = mMonitoredUids[uid].sharedClientPids;
                if (!sharedClientPids.empty()) {
                  sharedPids.assign(sharedClientPids.begin(), sharedClientPids.end());
                }
            }
        }
    }

    sp<CameraService> service = mService.promote();
    if (procStateChange) {
        if (service != nullptr) {
            service->notifyMonitoredUids();
        }
    }

    if (flags::camera_multi_client() && !sharedPids.empty()) {
        if (service != nullptr) {
            service->updateSharedClientAccessPriorities(sharedPids);
        }
    }
}

/**
 * When the OOM adj of the uid owning the camera changes, a different uid waiting on camera
 * privileges may take precedence if the owner's new OOM adj is greater than the waiting package.
 * Here, we track which monitoredUid has the camera, and track its adj relative to other
 * monitoredUids. If it is revised above some other monitoredUid, signal
 * onCameraAccessPrioritiesChanged. This only needs to capture the case where there are two
 * foreground apps in split screen - state changes will capture all other cases.
 */
void CameraService::UidPolicy::onUidProcAdjChanged(uid_t uid, int32_t adj) {
    std::unordered_set<uid_t> notifyUidSet;
    std::vector<int> sharedPids;
    {
        Mutex::Autolock _l(mUidLock);
        auto it = mMonitoredUids.find(uid);

        if (it != mMonitoredUids.end()) {
            if (it->second.hasCamera) {
                for (auto &monitoredUid : mMonitoredUids) {
                    if (monitoredUid.first != uid && adj > monitoredUid.second.procAdj) {
                        ALOGV("%s: notify uid %d", __FUNCTION__, monitoredUid.first);
                        notifyUidSet.emplace(monitoredUid.first);
                    }
                }
                ALOGV("%s: notify uid %d", __FUNCTION__, uid);
                notifyUidSet.emplace(uid);
            } else {
                for (auto &monitoredUid : mMonitoredUids) {
                    if (monitoredUid.second.hasCamera && adj < monitoredUid.second.procAdj) {
                        ALOGV("%s: notify uid %d", __FUNCTION__, uid);
                        notifyUidSet.emplace(uid);
                    }
                }
            }
            it->second.procAdj = adj;
            if (flags::camera_multi_client()) {
                std::unordered_set<int> sharedClientPids = it->second.sharedClientPids;
                if (!sharedClientPids.empty()) {
                    sharedPids.assign(sharedClientPids.begin(), sharedClientPids.end());
                }
            }
        }
    }

    sp<CameraService> service = mService.promote();

    if (notifyUidSet.size() > 0) {
        if (service != nullptr) {
            service->notifyMonitoredUids(notifyUidSet);
        }
    }

    if (flags::camera_multi_client() && !sharedPids.empty()) {
        if (service != nullptr) {
            service->updateSharedClientAccessPriorities(sharedPids);
        }
    }
}

/**
 * Register a uid for monitoring, and note whether it owns a camera.
 */
void CameraService::UidPolicy::registerMonitorUid(uid_t uid, bool openCamera) {
    Mutex::Autolock _l(mUidLock);
    auto it = mMonitoredUids.find(uid);
    if (it != mMonitoredUids.end()) {
        it->second.refCount++;
    } else {
        MonitoredUid monitoredUid;
        monitoredUid.procState = ActivityManager::PROCESS_STATE_NONEXISTENT;
        monitoredUid.procAdj = resource_policy::UNKNOWN_ADJ;
        monitoredUid.refCount = 1;
        it = mMonitoredUids.emplace(std::pair<uid_t, MonitoredUid>(uid, monitoredUid)).first;
        status_t res = mAm.addUidToObserver(mObserverToken, toString16(kServiceName), uid);
        if (res != OK) {
            ALOGE("UidPolicy: Failed to add uid to observer: 0x%08x", res);
        }
    }

    if (openCamera) {
        it->second.hasCamera = true;
    }
}

/**
 * Unregister a uid for monitoring, and note whether it lost ownership of a camera.
 */
void CameraService::UidPolicy::unregisterMonitorUid(uid_t uid, bool closeCamera) {
    Mutex::Autolock _l(mUidLock);
    auto it = mMonitoredUids.find(uid);
    if (it != mMonitoredUids.end()) {
        it->second.refCount--;
        if (it->second.refCount == 0) {
            mMonitoredUids.erase(it);
            status_t res = mAm.removeUidFromObserver(mObserverToken, toString16(kServiceName), uid);
            if (res != OK) {
                ALOGE("UidPolicy: Failed to remove uid from observer: 0x%08x", res);
            }
        } else if (closeCamera) {
            it->second.hasCamera = false;
        }
    } else {
        ALOGE("%s: Trying to unregister uid: %d which is not monitored!", __FUNCTION__, uid);
    }
}

bool CameraService::UidPolicy::isUidActive(uid_t uid, const std::string &callingPackage) {
    Mutex::Autolock _l(mUidLock);
    return isUidActiveLocked(uid, callingPackage);
}

static const int64_t kPollUidActiveTimeoutTotalMillis = 300;
static const int64_t kPollUidActiveTimeoutMillis = 50;

bool CameraService::UidPolicy::isUidActiveLocked(uid_t uid, const std::string &callingPackage) {
    // Non-app UIDs are considered always active
    // If activity manager is unreachable, assume everything is active
    if (uid < FIRST_APPLICATION_UID || !mRegistered) {
        return true;
    }
    auto it = mOverrideUids.find(uid);
    if (it != mOverrideUids.end()) {
        return it->second;
    }
    bool active = mActiveUids.find(uid) != mActiveUids.end();
    if (!active) {
        // We want active UIDs to always access camera with their first attempt since
        // there is no guarantee the app is robustly written and would retry getting
        // the camera on failure. The inverse case is not a problem as we would take
        // camera away soon once we get the callback that the uid is no longer active.
        ActivityManager am;
        // Okay to access with a lock held as UID changes are dispatched without
        // a lock and we are a higher level component.
        int64_t startTimeMillis = 0;
        do {
            // TODO: Fix this b/109950150!
            // Okay this is a hack. There is a race between the UID turning active and
            // activity being resumed. The proper fix is very risky, so we temporary add
            // some polling which should happen pretty rarely anyway as the race is hard
            // to hit.
            active = mActiveUids.find(uid) != mActiveUids.end();
            if (!active) active = am.isUidActive(uid, toString16(callingPackage));
            if (active) {
                break;
            }
            if (startTimeMillis <= 0) {
                startTimeMillis = uptimeMillis();
            }
            int64_t ellapsedTimeMillis = uptimeMillis() - startTimeMillis;
            int64_t remainingTimeMillis = kPollUidActiveTimeoutTotalMillis - ellapsedTimeMillis;
            if (remainingTimeMillis <= 0) {
                break;
            }
            remainingTimeMillis = std::min(kPollUidActiveTimeoutMillis, remainingTimeMillis);

            mUidLock.unlock();
            usleep(remainingTimeMillis * 1000);
            mUidLock.lock();
        } while (true);

        if (active) {
            // Now that we found out the UID is actually active, cache that
            mActiveUids.insert(uid);
        }
    }
    return active;
}

int32_t CameraService::UidPolicy::getProcState(uid_t uid) {
    Mutex::Autolock _l(mUidLock);
    return getProcStateLocked(uid);
}

int32_t CameraService::UidPolicy::getProcStateLocked(uid_t uid) {
    int32_t procState = ActivityManager::PROCESS_STATE_UNKNOWN;
    if (mMonitoredUids.find(uid) != mMonitoredUids.end()) {
        procState = mMonitoredUids[uid].procState;
    }
    return procState;
}

void CameraService::UidPolicy::addOverrideUid(uid_t uid,
        const std::string &callingPackage, bool active) {
    updateOverrideUid(uid, callingPackage, active, true);
}

void CameraService::UidPolicy::removeOverrideUid(uid_t uid, const std::string &callingPackage) {
    updateOverrideUid(uid, callingPackage, false, false);
}

void CameraService::UidPolicy::addSharedClientPid(uid_t uid, int pid) {
    Mutex::Autolock _l(mUidLock);
    if (mMonitoredUids.find(uid) != mMonitoredUids.end()) {
        mMonitoredUids[uid].sharedClientPids.insert(pid);
    }
}

void CameraService::UidPolicy::removeSharedClientPid(uid_t uid, int pid) {
    Mutex::Autolock _l(mUidLock);
    if (mMonitoredUids.find(uid) != mMonitoredUids.end()) {
        mMonitoredUids[uid].sharedClientPids.erase(pid);
    }
}

void CameraService::UidPolicy::binderDied(const wp<IBinder>& /*who*/) {
    Mutex::Autolock _l(mUidLock);
    ALOGV("UidPolicy: ActivityManager has died");
    mRegistered = false;
    mActiveUids.clear();
}

void CameraService::UidPolicy::updateOverrideUid(uid_t uid, const std::string &callingPackage,
        bool active, bool insert) {
    bool wasActive = false;
    bool isActive = false;
    {
        Mutex::Autolock _l(mUidLock);
        wasActive = isUidActiveLocked(uid, callingPackage);
        mOverrideUids.erase(uid);
        if (insert) {
            mOverrideUids.insert(std::pair<uid_t, bool>(uid, active));
        }
        isActive = isUidActiveLocked(uid, callingPackage);
    }
    if (wasActive != isActive && !isActive) {
        sp<CameraService> service = mService.promote();
        if (service != nullptr) {
            service->blockClientsForUid(uid);
        }
    }
}

// ----------------------------------------------------------------------------
//                  SensorPrivacyPolicy
// ----------------------------------------------------------------------------

void CameraService::SensorPrivacyPolicy::registerWithSensorPrivacyManager()
{
    Mutex::Autolock _l(mSensorPrivacyLock);
    if (mRegistered) {
        return;
    }
    hasCameraPrivacyFeature(); // Called so the result is cached
    mSpm.addSensorPrivacyListener(this);
    if (isAutomotiveDevice()) {
        mSpm.addToggleSensorPrivacyListener(this);
    }
    mSensorPrivacyEnabled = mSpm.isSensorPrivacyEnabled();
    if (flags::camera_privacy_allowlist()) {
        mCameraPrivacyState = mSpm.getToggleSensorPrivacyState(
                SensorPrivacyManager::TOGGLE_TYPE_SOFTWARE,
                SensorPrivacyManager::TOGGLE_SENSOR_CAMERA);
    }
    status_t res = mSpm.linkToDeath(this);
    if (res == OK) {
        mRegistered = true;
        ALOGV("SensorPrivacyPolicy: Registered with SensorPrivacyManager");
    }
}

void CameraService::SensorPrivacyPolicy::onServiceRegistration(const String16& name,
                                                               const sp<IBinder>&) {
    if (name != toString16(kSensorPrivacyServiceName)) {
        return;
    }

    registerWithSensorPrivacyManager();
}

void CameraService::SensorPrivacyPolicy::registerSelf() {
    // Use checkservice to see if the sensor_privacy service is available
    // If service is not available then register for notification
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->checkService(toString16(kSensorPrivacyServiceName));
    if (!binder) {
        sm->registerForNotifications(toString16(kSensorPrivacyServiceName),this);
    } else {
        registerWithSensorPrivacyManager();
    }
}

void CameraService::SensorPrivacyPolicy::unregisterSelf() {
    Mutex::Autolock _l(mSensorPrivacyLock);
    mSpm.removeSensorPrivacyListener(this);
    if (isAutomotiveDevice()) {
        mSpm.removeToggleSensorPrivacyListener(this);
    }
    mSpm.unlinkToDeath(this);
    mRegistered = false;
    ALOGV("SensorPrivacyPolicy: Unregistered with SensorPrivacyManager");
}

bool CameraService::SensorPrivacyPolicy::isSensorPrivacyEnabled() {
    if (!mRegistered) {
      registerWithSensorPrivacyManager();
    }

    Mutex::Autolock _l(mSensorPrivacyLock);
    return mSensorPrivacyEnabled;
}

int CameraService::SensorPrivacyPolicy::getCameraPrivacyState() {
    if (!mRegistered) {
        registerWithSensorPrivacyManager();
    }

    Mutex::Autolock _l(mSensorPrivacyLock);
    return mCameraPrivacyState;
}

bool CameraService::SensorPrivacyPolicy::isCameraPrivacyEnabled() {
    if (!hasCameraPrivacyFeature()) {
        return false;
    }
    return mSpm.isToggleSensorPrivacyEnabled(SensorPrivacyManager::TOGGLE_SENSOR_CAMERA);
}

bool CameraService::SensorPrivacyPolicy::isCameraPrivacyEnabled(const String16& packageName) {
    if (!hasCameraPrivacyFeature()) {
        return false;
    }
    return mSpm.isCameraPrivacyEnabled(packageName);
}

binder::Status CameraService::SensorPrivacyPolicy::onSensorPrivacyChanged(
    int toggleType, int sensor, bool enabled) {
    if ((toggleType == SensorPrivacyManager::TOGGLE_TYPE_UNKNOWN)
            && (sensor == SensorPrivacyManager::TOGGLE_SENSOR_UNKNOWN)) {
        {
            Mutex::Autolock _l(mSensorPrivacyLock);
            mSensorPrivacyEnabled = enabled;
        }
        // if sensor privacy is enabled then block all clients from accessing the camera
        if (enabled) {
            sp<CameraService> service = mService.promote();
            if (service != nullptr) {
                service->blockAllClients();
            }
        }
    }
    return binder::Status::ok();
}

binder::Status CameraService::SensorPrivacyPolicy::onSensorPrivacyStateChanged(
    int, int sensor, int state) {
    if (!flags::camera_privacy_allowlist()
            || (sensor != SensorPrivacyManager::TOGGLE_SENSOR_CAMERA)) {
        return binder::Status::ok();
    }
    {
        Mutex::Autolock _l(mSensorPrivacyLock);
        mCameraPrivacyState = state;
    }
    sp<CameraService> service = mService.promote();
    if (!service) {
        return binder::Status::ok();
    }
    // if sensor privacy is enabled then block all clients from accessing the camera
    if (state == SensorPrivacyManager::ENABLED) {
        service->blockAllClients();
    } else if (state == SensorPrivacyManager::ENABLED_EXCEPT_ALLOWLISTED_APPS) {
        service->blockPrivacyEnabledClients();
    }
    return binder::Status::ok();
}

void CameraService::SensorPrivacyPolicy::binderDied(const wp<IBinder>& /*who*/) {
    Mutex::Autolock _l(mSensorPrivacyLock);
    ALOGV("SensorPrivacyPolicy: SensorPrivacyManager has died");
    mRegistered = false;
}

bool CameraService::SensorPrivacyPolicy::hasCameraPrivacyFeature() {
    bool supportsSoftwareToggle = mSpm.supportsSensorToggle(
            SensorPrivacyManager::TOGGLE_TYPE_SOFTWARE, SensorPrivacyManager::TOGGLE_SENSOR_CAMERA);
    bool supportsHardwareToggle = mSpm.supportsSensorToggle(
            SensorPrivacyManager::TOGGLE_TYPE_HARDWARE, SensorPrivacyManager::TOGGLE_SENSOR_CAMERA);
    return supportsSoftwareToggle || supportsHardwareToggle;
}

// ----------------------------------------------------------------------------
//                  CameraState
// ----------------------------------------------------------------------------

CameraService::CameraState::CameraState(const std::string& id, int cost,
        const std::set<std::string>& conflicting, SystemCameraKind systemCameraKind,
        const std::vector<std::string>& physicalCameras) : mId(id),
        mStatus(StatusInternal::NOT_PRESENT), mCost(cost), mConflicting(conflicting),
        mSystemCameraKind(systemCameraKind), mPhysicalCameras(physicalCameras) {}

CameraService::CameraState::~CameraState() {}

CameraService::StatusInternal CameraService::CameraState::getStatus() const {
    Mutex::Autolock lock(mStatusLock);
    return mStatus;
}

std::vector<std::string> CameraService::CameraState::getUnavailablePhysicalIds() const {
    Mutex::Autolock lock(mStatusLock);
    std::vector<std::string> res(mUnavailablePhysicalIds.begin(), mUnavailablePhysicalIds.end());
    return res;
}

CameraParameters CameraService::CameraState::getShimParams() const {
    return mShimParams;
}

void CameraService::CameraState::setShimParams(const CameraParameters& params) {
    mShimParams = params;
}

int CameraService::CameraState::getCost() const {
    return mCost;
}

std::set<std::string> CameraService::CameraState::getConflicting() const {
    return mConflicting;
}

SystemCameraKind CameraService::CameraState::getSystemCameraKind() const {
    return mSystemCameraKind;
}

bool CameraService::CameraState::containsPhysicalCamera(const std::string& physicalCameraId) const {
    return std::find(mPhysicalCameras.begin(), mPhysicalCameras.end(), physicalCameraId)
            != mPhysicalCameras.end();
}

bool CameraService::CameraState::addUnavailablePhysicalId(const std::string& physicalId) {
    Mutex::Autolock lock(mStatusLock);
    auto result = mUnavailablePhysicalIds.insert(physicalId);
    return result.second;
}

bool CameraService::CameraState::removeUnavailablePhysicalId(const std::string& physicalId) {
    Mutex::Autolock lock(mStatusLock);
    auto count = mUnavailablePhysicalIds.erase(physicalId);
    return count > 0;
}

void CameraService::CameraState::setClientPackage(const std::string& clientPackage) {
    Mutex::Autolock lock(mStatusLock);
    mClientPackages.clear();
    mClientPackages.insert(clientPackage);
}

std::string CameraService::CameraState::getClientPackage() const {
    Mutex::Autolock lock(mStatusLock);
    if (!mClientPackages.empty()) {
        std::set<std::string>::iterator it = mClientPackages.begin();
        return *it;
    }
    return std::string();
}

void CameraService::CameraState::addClientPackage(const std::string& clientPackage) {
    Mutex::Autolock lock(mStatusLock);
    mClientPackages.insert(clientPackage);
}

void CameraService::CameraState::removeClientPackage(const std::string& clientPackage) {
    Mutex::Autolock lock(mStatusLock);
    mClientPackages.erase(clientPackage);
}

// ----------------------------------------------------------------------------
//                  ClientEventListener
// ----------------------------------------------------------------------------

void CameraService::ClientEventListener::onClientAdded(
        const resource_policy::ClientDescriptor<std::string,
        sp<CameraService::BasicClient>>& descriptor) {
    const auto& basicClient = descriptor.getValue();
    if (basicClient.get() != nullptr) {
        BatteryNotifier& notifier(BatteryNotifier::getInstance());
        notifier.noteStartCamera(toString8(descriptor.getKey()),
                static_cast<int>(basicClient->getClientUid()));
    }
}

void CameraService::ClientEventListener::onClientRemoved(
        const resource_policy::ClientDescriptor<std::string,
        sp<CameraService::BasicClient>>& descriptor) {
    const auto& basicClient = descriptor.getValue();
    if (basicClient.get() != nullptr) {
        BatteryNotifier& notifier(BatteryNotifier::getInstance());
        notifier.noteStopCamera(toString8(descriptor.getKey()),
                static_cast<int>(basicClient->getClientUid()));
    }
}

// ----------------------------------------------------------------------------
//                  CameraClientManager
// ----------------------------------------------------------------------------

CameraService::CameraClientManager::CameraClientManager() {
    setListener(std::make_shared<ClientEventListener>());
}

CameraService::CameraClientManager::~CameraClientManager() {}

sp<CameraService::BasicClient> CameraService::CameraClientManager::getCameraClient(
        const std::string& id) const {
    auto descriptor = get(id);
    if (descriptor == nullptr) {
        return sp<BasicClient>{nullptr};
    }
    return descriptor->getValue();
}

sp<CameraService::BasicClient> CameraService::CameraClientManager::getHighestPrioritySharedClient(
        const std::string& id) const {
    if (!flags::camera_multi_client()) {
        return sp<BasicClient>{nullptr};
    }
    auto clientDescriptor = get(id);
    if (clientDescriptor == nullptr) {
        ALOGV("CameraService::CameraClientManager::no other clients are using same camera");
        return sp<BasicClient>{nullptr};
    }
    if (!clientDescriptor->getSharedMode()) {
        return sp<BasicClient>{nullptr};
    }
    resource_policy::ClientPriority highestPriority = clientDescriptor->getPriority();
    sp<BasicClient> highestPriorityClient = clientDescriptor->getValue();
    if (highestPriorityClient.get() == nullptr) {
        return sp<BasicClient>{nullptr};
    }
    for (auto& i : getAll()) {
        if ((i->getKey() == id) && (i->getSharedMode()) && (i->getPriority() < highestPriority)) {
            highestPriority = i->getPriority();
            highestPriorityClient = i->getValue();
        }
    }
    return highestPriorityClient;
}

void CameraService::CameraClientManager::remove(const CameraService::DescriptorPtr& value) {
    ClientManager::remove(value);
    if (!flags::camera_multi_client()) {
        return;
    }
    auto clientToRemove = value->getValue();
    if ((clientToRemove.get() != nullptr) && clientToRemove->mSharedMode) {
        bool primaryClient = false;
        status_t ret = clientToRemove->isPrimaryClient(&primaryClient);
        if ((ret == OK) && primaryClient) {
            // Primary client is being removed. Find the next higher priority
            // client to become primary client.
            auto highestPriorityClient = getHighestPrioritySharedClient(value->getKey());
            if (highestPriorityClient != nullptr) {
                highestPriorityClient->setPrimaryClient(true);
                highestPriorityClient->notifyClientSharedAccessPriorityChanged(true);
            }
        }
    }
}

std::string CameraService::CameraClientManager::toString() const {
    auto all = getAll();
    std::ostringstream ret;
    ret << "[";
    bool hasAny = false;
    for (auto& i : all) {
        hasAny = true;
        std::string key = i->getKey();
        int32_t cost = i->getCost();
        int32_t pid = i->getOwnerId();
        int32_t score = i->getPriority().getScore();
        int32_t state = i->getPriority().getState();
        auto conflicting = i->getConflicting();
        auto clientSp = i->getValue();
        std::string packageName;
        userid_t clientUserId = 0;
        if (clientSp.get() != nullptr) {
            packageName = clientSp->getPackageName();
            uid_t clientUid = clientSp->getClientUid();
            clientUserId = multiuser_get_user_id(clientUid);
        }
        ret << fmt::sprintf("\n(Camera ID: %s, Cost: %" PRId32 ", PID: %" PRId32 ", Score: %"
                PRId32 ", State: %" PRId32, key.c_str(), cost, pid, score, state);

        if (clientSp.get() != nullptr) {
            ret << fmt::sprintf("User Id: %d, ", clientUserId);
        }
        if (packageName.size() != 0) {
            ret << fmt::sprintf("Client Package Name: %s", packageName.c_str());
        }

        ret << ", Conflicting Client Devices: {";
        for (auto& j : conflicting) {
            ret << fmt::sprintf("%s, ", j.c_str());
        }
        ret << "})";
    }
    if (hasAny) ret << "\n";
    ret << "]\n";
    return ret.str();
}

CameraService::DescriptorPtr CameraService::CameraClientManager::makeClientDescriptor(
        const std::string& key, const sp<BasicClient>& value, int32_t cost,
        const std::set<std::string>& conflictingKeys, int32_t score, int32_t ownerId,
        int32_t state, int32_t oomScoreOffset, bool systemNativeClient, bool sharedMode) {

    int32_t score_adj = systemNativeClient ? kSystemNativeClientScore : score;
    int32_t state_adj = systemNativeClient ? kSystemNativeClientState : state;

    return std::make_shared<resource_policy::ClientDescriptor<std::string, sp<BasicClient>>>(
            key, value, cost, conflictingKeys, score_adj, ownerId, state_adj,
            systemNativeClient, oomScoreOffset, sharedMode);
}

CameraService::DescriptorPtr CameraService::CameraClientManager::makeClientDescriptor(
        const sp<BasicClient>& value, const CameraService::DescriptorPtr& partial,
        int32_t oomScoreOffset, bool systemNativeClient) {
    return makeClientDescriptor(partial->getKey(), value, partial->getCost(),
            partial->getConflicting(), partial->getPriority().getScore(),
            partial->getOwnerId(), partial->getPriority().getState(), oomScoreOffset,
            systemNativeClient, partial->getSharedMode());
}

// ----------------------------------------------------------------------------
//                  InjectionStatusListener
// ----------------------------------------------------------------------------

void CameraService::InjectionStatusListener::addListener(
        const sp<ICameraInjectionCallback>& callback) {
    Mutex::Autolock lock(mListenerLock);
    if (mCameraInjectionCallback) return;
    status_t res = IInterface::asBinder(callback)->linkToDeath(this);
    if (res == OK) {
        mCameraInjectionCallback = callback;
    }
}

void CameraService::InjectionStatusListener::removeListener() {
    Mutex::Autolock lock(mListenerLock);
    if (mCameraInjectionCallback == nullptr) {
        ALOGW("InjectionStatusListener: mCameraInjectionCallback == nullptr");
        return;
    }
    IInterface::asBinder(mCameraInjectionCallback)->unlinkToDeath(this);
    mCameraInjectionCallback = nullptr;
}

void CameraService::InjectionStatusListener::notifyInjectionError(
        const std::string &injectedCamId, status_t err) {
    if (mCameraInjectionCallback == nullptr) {
        ALOGW("InjectionStatusListener: mCameraInjectionCallback == nullptr");
        return;
    }

    switch (err) {
        case -ENODEV:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_SESSION);
            ALOGE("No camera device with ID \"%s\" currently available!",
                    injectedCamId.c_str());
            break;
        case -EBUSY:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_SESSION);
            ALOGE("Higher-priority client using camera, ID \"%s\" currently unavailable!",
                    injectedCamId.c_str());
            break;
        case DEAD_OBJECT:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_SESSION);
            ALOGE("Camera ID \"%s\" object is dead!",
                    injectedCamId.c_str());
            break;
        case INVALID_OPERATION:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_SESSION);
            ALOGE("Camera ID \"%s\" encountered an operating or internal error!",
                    injectedCamId.c_str());
            break;
        case UNKNOWN_TRANSACTION:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_UNSUPPORTED);
            ALOGE("Camera ID \"%s\" method doesn't support!",
                    injectedCamId.c_str());
            break;
        default:
            mCameraInjectionCallback->onInjectionError(
                    ICameraInjectionCallback::ERROR_INJECTION_INVALID_ERROR);
            ALOGE("Unexpected error %s (%d) opening camera \"%s\"!",
                    strerror(-err), err, injectedCamId.c_str());
    }
}

void CameraService::InjectionStatusListener::binderDied(
        const wp<IBinder>& /*who*/) {
    ALOGV("InjectionStatusListener: ICameraInjectionCallback has died");
    auto parent = mParent.promote();
    if (parent != nullptr) {
        auto clientDescriptor = parent->mActiveClientManager.get(parent->mInjectionInternalCamId);
        if (clientDescriptor != nullptr) {
            BasicClient* baseClientPtr = clientDescriptor->getValue().get();
            baseClientPtr->stopInjection();
        }
        parent->clearInjectionParameters();
    }
}

// ----------------------------------------------------------------------------
//                  CameraInjectionSession
// ----------------------------------------------------------------------------

binder::Status CameraService::CameraInjectionSession::stopInjection() {
    Mutex::Autolock lock(mInjectionSessionLock);
    auto parent = mParent.promote();
    if (parent == nullptr) {
        ALOGE("CameraInjectionSession: Parent is gone");
        return STATUS_ERROR(ICameraInjectionCallback::ERROR_INJECTION_SERVICE,
                "Camera service encountered error");
    }

    status_t res = NO_ERROR;
    auto clientDescriptor = parent->mActiveClientManager.get(parent->mInjectionInternalCamId);
    if (clientDescriptor != nullptr) {
        BasicClient* baseClientPtr = clientDescriptor->getValue().get();
        res = baseClientPtr->stopInjection();
        if (res != OK) {
            ALOGE("CameraInjectionSession: Failed to stop the injection camera!"
                " ret != NO_ERROR: %d", res);
            return STATUS_ERROR(ICameraInjectionCallback::ERROR_INJECTION_SESSION,
                "Camera session encountered error");
        }
    }
    parent->clearInjectionParameters();
    return binder::Status::ok();
}

// ----------------------------------------------------------------------------

static const int kDumpLockRetries = 50;
static const int kDumpLockSleep = 60000;

static bool tryLock(Mutex& mutex)
{
    bool locked = false;
    for (int i = 0; i < kDumpLockRetries; ++i) {
        if (mutex.tryLock() == NO_ERROR) {
            locked = true;
            break;
        }
        usleep(kDumpLockSleep);
    }
    return locked;
}

void CameraService::cacheDump() {
    if (mMemFd != -1) {
        const Vector<String16> args;
        ATRACE_CALL();
        // Acquiring service lock here will avoid the deadlock since
        // cacheDump will not be called during the second disconnect.
        Mutex::Autolock lock(mServiceLock);

        Mutex::Autolock l(mCameraStatesLock);
        // Start collecting the info for open sessions and store it in temp file.
        for (const auto& state : mCameraStates) {
            std::string cameraId = state.first;
            auto clientDescriptor = mActiveClientManager.get(cameraId);
            if (clientDescriptor != nullptr) {
                dprintf(mMemFd, "== Camera device %s dynamic info: ==\n", cameraId.c_str());
                // Log the current open session info before device is disconnected.
                dumpOpenSessionClientLogs(mMemFd, args, cameraId);
            }
        }
    }
}

status_t CameraService::dump(int fd, const Vector<String16>& args) {
    ATRACE_CALL();

    if (checkCallingPermission(toString16(sDumpPermission)) == false) {
        dprintf(fd, "Permission Denial: can't dump CameraService from pid=%d, uid=%d\n",
                getCallingPid(),
                getCallingUid());
        return NO_ERROR;
    }
    bool locked = tryLock(mServiceLock);
    // failed to lock - CameraService is probably deadlocked
    if (!locked) {
        dprintf(fd, "!! CameraService may be deadlocked !!\n");
    }

    if (!mInitialized) {
        dprintf(fd, "!! No camera HAL available !!\n");

        // Dump event log for error information
        dumpEventLog(fd);

        if (locked) mServiceLock.unlock();
        return NO_ERROR;
    }
    dprintf(fd, "\n== Service global info: ==\n\n");
    dprintf(fd, "Number of camera devices: %d\n", mNumberOfCameras);
    dprintf(fd, "Number of normal camera devices: %zu\n", mNormalDeviceIds.size());
    dprintf(fd, "Number of public camera devices visible to API1: %zu\n",
            mNormalDeviceIdsWithoutSystemCamera.size());
    for (size_t i = 0; i < mNormalDeviceIds.size(); i++) {
        dprintf(fd, "    Device %zu maps to \"%s\"\n", i, mNormalDeviceIds[i].c_str());
    }
    std::string activeClientString = mActiveClientManager.toString();
    dprintf(fd, "Active Camera Clients:\n%s", activeClientString.c_str());
    dprintf(fd, "Allowed user IDs: %s\n", toString(mAllowedUsers).c_str());
    if (mStreamUseCaseOverrides.size() > 0) {
        dprintf(fd, "Active stream use case overrides:");
        for (int64_t useCaseOverride : mStreamUseCaseOverrides) {
            dprintf(fd, " %" PRId64, useCaseOverride);
        }
        dprintf(fd, "\n");
    }

    dumpEventLog(fd);

    bool stateLocked = tryLock(mCameraStatesLock);
    if (!stateLocked) {
        dprintf(fd, "CameraStates in use, may be deadlocked\n");
    }

    int argSize = args.size();
    for (int i = 0; i < argSize; i++) {
        if (args[i] == toString16(TagMonitor::kMonitorOption)) {
            if (i + 1 < argSize) {
                mMonitorTags = toStdString(args[i + 1]);
            }
            break;
        }
    }

    for (auto& state : mCameraStates) {
        const std::string &cameraId = state.first;

        dprintf(fd, "== Camera device %s dynamic info: ==\n", cameraId.c_str());

        CameraParameters p = state.second->getShimParams();
        if (!p.isEmpty()) {
            dprintf(fd, "  Camera1 API shim is using parameters:\n        ");
            p.dump(fd, args);
        }

        auto clientDescriptor = mActiveClientManager.get(cameraId);
        if (clientDescriptor != nullptr) {
            // log the current open session info
            dumpOpenSessionClientLogs(fd, args, cameraId);
        } else {
            dumpClosedSessionClientLogs(fd, cameraId);
        }

    }

    if (stateLocked) mCameraStatesLock.unlock();

    if (locked) mServiceLock.unlock();

    mCameraProviderManager->dump(fd, args);

    dprintf(fd, "\n== Vendor tags: ==\n\n");

    sp<VendorTagDescriptor> desc = VendorTagDescriptor::getGlobalVendorTagDescriptor();
    if (desc == NULL) {
        sp<VendorTagDescriptorCache> cache =
                VendorTagDescriptorCache::getGlobalVendorTagCache();
        if (cache == NULL) {
            dprintf(fd, "No vendor tags.\n");
        } else {
            cache->dump(fd, /*verbosity*/2, /*indentation*/2);
        }
    } else {
        desc->dump(fd, /*verbosity*/2, /*indentation*/2);
    }

    // Dump camera traces if there were any
    dprintf(fd, "\n");
    camera3::CameraTraces::dump(fd);

    // Process dump arguments, if any
    int n = args.size();
    String16 verboseOption("-v");
    String16 unreachableOption("--unreachable");
    for (int i = 0; i < n; i++) {
        if (args[i] == verboseOption) {
            // change logging level
            if (i + 1 >= n) continue;
            std::string levelStr = toStdString(args[i+1]);
            int level = atoi(levelStr.c_str());
            dprintf(fd, "\nSetting log level to %d.\n", level);
            setLogLevel(level);
        } else if (args[i] == unreachableOption) {
            // Dump memory analysis
            // TODO - should limit be an argument parameter?
            UnreachableMemoryInfo info;
            bool success = GetUnreachableMemory(info, /*limit*/ 10000);
            if (!success) {
                dprintf(fd, "\n== Unable to dump unreachable memory. "
                        "Try disabling SELinux enforcement. ==\n");
            } else {
                dprintf(fd, "\n== Dumping unreachable memory: ==\n");
                std::string s = info.ToString(/*log_contents*/ true);
                write(fd, s.c_str(), s.size());
            }
        }
    }

    bool serviceLocked = tryLock(mServiceLock);

    // Dump info from previous open sessions.
    // Reposition the offset to beginning of the file before reading

    if ((mMemFd >= 0) && (lseek(mMemFd, 0, SEEK_SET) != -1)) {
        dprintf(fd, "\n**********Dumpsys from previous open session**********\n");
        ssize_t size_read;
        char buf[4096];
        while ((size_read = read(mMemFd, buf, (sizeof(buf) - 1))) > 0) {
            // Read data from file to a small buffer and write it to fd.
            write(fd, buf, size_read);
            if (size_read == -1) {
                ALOGE("%s: Error during reading the file: %s", __FUNCTION__, sFileName);
                break;
            }
        }
        dprintf(fd, "\n**********End of Dumpsys from previous open session**********\n");
    } else {
        ALOGE("%s: Error during reading the file: %s", __FUNCTION__, sFileName);
    }

    if (serviceLocked) mServiceLock.unlock();
    return NO_ERROR;
}

void CameraService::dumpOpenSessionClientLogs(int fd,
        const Vector<String16>& args, const std::string& cameraId) {
    auto clientDescriptor = mActiveClientManager.get(cameraId);
    dprintf(fd, "  %s : Device %s is open. Client instance dump:\n",
            getFormattedCurrentTime().c_str(),
            cameraId.c_str());
    dprintf(fd, "    Client priority score: %d state: %d\n",
        clientDescriptor->getPriority().getScore(),
        clientDescriptor->getPriority().getState());
    dprintf(fd, "    Client PID: %d\n", clientDescriptor->getOwnerId());

    auto client = clientDescriptor->getValue();
    dprintf(fd, "    Client package: %s\n",
        client->getPackageName().c_str());

    client->dumpClient(fd, args);
}

void CameraService::dumpClosedSessionClientLogs(int fd, const std::string& cameraId) {
    dprintf(fd, "  Device %s is closed, no client instance\n",
                    cameraId.c_str());
}

void CameraService::dumpEventLog(int fd) {
    dprintf(fd, "\n== Camera service events log (most recent at top): ==\n");

    Mutex::Autolock l(mLogLock);
    for (const auto& msg : mEventLog) {
        dprintf(fd, "  %s\n", msg.c_str());
    }

    if (mEventLog.size() == DEFAULT_EVENT_LOG_LENGTH) {
        dprintf(fd, "  ...\n");
    } else if (mEventLog.size() == 0) {
        dprintf(fd, "  [no events yet]\n");
    }
    dprintf(fd, "\n");
}

void CameraService::cacheClientTagDumpIfNeeded(const std::string &cameraId, BasicClient* client) {
    Mutex::Autolock lock(mLogLock);
    if (!isClientWatchedLocked(client)) { return; }

    std::vector<std::string> dumpVector;
    client->dumpWatchedEventsToVector(dumpVector);

    if (dumpVector.empty()) { return; }

    std::ostringstream dumpString;

    std::string currentTime = getFormattedCurrentTime();
    dumpString << "Cached @ ";
    dumpString << currentTime;
    dumpString << "\n"; // First line is the timestamp of when client is cached.

    size_t i = dumpVector.size();

    // Store the string in reverse order (latest last)
    while (i > 0) {
         i--;
         dumpString << cameraId;
         dumpString << ":";
         dumpString << client->getPackageName();
         dumpString << "  ";
         dumpString << dumpVector[i]; // implicitly ends with '\n'
    }

    mWatchedClientsDumpCache[client->getPackageName()] = dumpString.str();
}

void CameraService::handleTorchClientBinderDied(const wp<IBinder> &who) {
    Mutex::Autolock al(mTorchClientMapMutex);
    for (size_t i = 0; i < mTorchClientMap.size(); i++) {
        if (mTorchClientMap[i] == who) {
            // turn off the torch mode that was turned on by dead client
            std::string cameraId = mTorchClientMap.keyAt(i);
            status_t res = mFlashlight->setTorchMode(cameraId, false);
            if (res) {
                ALOGE("%s: torch client died but couldn't turn off torch: "
                    "%s (%d)", __FUNCTION__, strerror(-res), res);
                return;
            }
            mTorchClientMap.removeItemsAt(i);
            break;
        }
    }
}

/*virtual*/void CameraService::binderDied(const wp<IBinder> &who) {

    /**
      * While tempting to promote the wp<IBinder> into a sp, it's actually not supported by the
      * binder driver
      */
    // PID here is approximate and can be wrong.
    logClientDied(getCallingPid(), "Binder died unexpectedly");

    // check torch client
    handleTorchClientBinderDied(who);

    // check camera device client
    if(!evictClientIdByRemote(who)) {
        ALOGV("%s: Java client's binder death already cleaned up (normal case)", __FUNCTION__);
        return;
    }

    ALOGE("%s: Java client's binder died, removing it from the list of active clients",
            __FUNCTION__);
}

void CameraService::updateStatus(StatusInternal status, const std::string& cameraId) {
    updateStatus(status, cameraId, {});
}

void CameraService::updateStatus(StatusInternal status, const std::string& cameraId,
        std::initializer_list<StatusInternal> rejectSourceStates) {
    // Do not lock mServiceLock here or can get into a deadlock from
    // connect() -> disconnect -> updateStatus

    auto state = getCameraState(cameraId);

    if (state == nullptr) {
        ALOGW("%s: Could not update the status for %s, no such device exists", __FUNCTION__,
                cameraId.c_str());
        return;
    }

    // Avoid calling getSystemCameraKind() with mStatusListenerLock held (b/141756275)
    SystemCameraKind deviceKind = SystemCameraKind::PUBLIC;
    if (getSystemCameraKind(cameraId, &deviceKind) != OK) {
        ALOGE("%s: Invalid camera id %s, skipping", __FUNCTION__, cameraId.c_str());
        return;
    }

    if (status == StatusInternal::PRESENT) {
        CameraMetadata cameraInfo;
        status_t res = mCameraProviderManager->getCameraCharacteristics(
                cameraId, false, &cameraInfo, hardware::ICameraService::ROTATION_OVERRIDE_NONE);
        if (res != OK) {
            ALOGW("%s: Not able to get camera characteristics for camera id %s",
                  __FUNCTION__, cameraId.c_str());
        } else {
            int32_t deviceId = getDeviceId(cameraInfo);
            if (deviceId != kDefaultDeviceId) {
                const auto &lensFacingEntry = cameraInfo.find(ANDROID_LENS_FACING);
                camera_metadata_enum_android_lens_facing_t androidLensFacing =
                        static_cast<camera_metadata_enum_android_lens_facing_t>(
                                lensFacingEntry.data.u8[0]);
                std::string mappedCameraId;
                if (androidLensFacing == ANDROID_LENS_FACING_BACK) {
                    mappedCameraId = kVirtualDeviceBackCameraId;
                } else if (androidLensFacing == ANDROID_LENS_FACING_FRONT) {
                    mappedCameraId = kVirtualDeviceFrontCameraId;
                } else {
                    ALOGD("%s: Not adding entry for an external camera of a virtual device",
                          __func__);
                }
                if (!mappedCameraId.empty()) {
                    mVirtualDeviceCameraIdMapper.addCamera(cameraId, deviceId, mappedCameraId);
                }
            }
        }
    }

    // Collect the logical cameras without holding mStatusLock in updateStatus
    // as that can lead to a deadlock(b/162192331).
    auto logicalCameraIds = getLogicalCameras(cameraId);
    // Update the status for this camera state, then send the onStatusChangedCallbacks to each
    // of the listeners with both the mStatusLock and mStatusListenerLock held
    state->updateStatus(status, cameraId, rejectSourceStates, [this, &deviceKind,
                        &logicalCameraIds]
            (const std::string& cameraId, StatusInternal status) {
                // Get the device id and app-visible camera id for the given HAL-visible camera id.
                auto [deviceId, mappedCameraId] =
                        mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(cameraId);

                if (status != StatusInternal::ENUMERATING) {
                    // Update torch status if it has a flash unit.
                    Mutex::Autolock al(mTorchStatusMutex);
                    TorchModeStatus torchStatus;
                    if (getTorchStatusLocked(cameraId, &torchStatus) !=
                            NAME_NOT_FOUND) {
                        TorchModeStatus newTorchStatus =
                                status == StatusInternal::PRESENT ?
                                TorchModeStatus::AVAILABLE_OFF :
                                TorchModeStatus::NOT_AVAILABLE;
                        if (torchStatus != newTorchStatus) {
                            onTorchStatusChangedLocked(cameraId, newTorchStatus, deviceKind);
                        }
                    }
                }

                Mutex::Autolock lock(mStatusListenerLock);
                notifyPhysicalCameraStatusLocked(mapToInterface(status), mappedCameraId,
                        logicalCameraIds, deviceKind, deviceId);

                for (auto& listener : mListenerList) {
                    bool isVendorListener = listener->isVendorListener();
                    if (shouldSkipStatusUpdates(deviceKind, isVendorListener,
                            listener->getListenerPid(), listener->getListenerUid())) {
                        ALOGV("Skipping discovery callback for system-only camera device %s",
                              cameraId.c_str());
                        continue;
                    }

                    auto ret = listener->getListener()->onStatusChanged(mapToInterface(status),
                            mappedCameraId, deviceId);
                    listener->handleBinderStatus(ret,
                            "%s: Failed to trigger onStatusChanged callback for %d:%d: %d",
                            __FUNCTION__, listener->getListenerUid(), listener->getListenerPid(),
                            ret.exceptionCode());
                }
            });
}

void CameraService::updateOpenCloseStatus(const std::string& cameraId, bool open,
        const std::string& clientPackageName, bool sharedMode) {
    auto state = getCameraState(cameraId);
    if (state == nullptr) {
        ALOGW("%s: Could not update the status for %s, no such device exists", __FUNCTION__,
                cameraId.c_str());
        return;
    }
    if (open) {
        if (flags::camera_multi_client() && sharedMode) {
            state->addClientPackage(clientPackageName);
        } else {
            state->setClientPackage(clientPackageName);
        }
    } else {
        if (flags::camera_multi_client() && sharedMode) {
            state->removeClientPackage(clientPackageName);
        } else {
            state->setClientPackage(std::string());
        }
    }

    // Get the device id and app-visible camera id for the given HAL-visible camera id.
    auto [deviceId, mappedCameraId] =
            mVirtualDeviceCameraIdMapper.getDeviceIdAndMappedCameraIdPair(cameraId);

    Mutex::Autolock lock(mStatusListenerLock);

    for (const auto& it : mListenerList) {
        if (!it->isOpenCloseCallbackAllowed()) {
            continue;
        }

        binder::Status ret;
        if (open) {
            ret = it->getListener()->onCameraOpened(mappedCameraId, clientPackageName,
                    deviceId);
        } else {
            if (!flags::camera_multi_client() || !sharedMode || (sharedMode &&
                    mActiveClientManager.getCameraClient(cameraId) == nullptr)) {
                ret = it->getListener()->onCameraClosed(mappedCameraId, deviceId);
            }
        }

        it->handleBinderStatus(ret,
                "%s: Failed to trigger onCameraOpened/onCameraClosed callback for %d:%d: %d",
                __FUNCTION__, it->getListenerUid(), it->getListenerPid(), ret.exceptionCode());
    }
}

template<class Func>
void CameraService::CameraState::updateStatus(StatusInternal status,
        const std::string& cameraId,
        std::initializer_list<StatusInternal> rejectSourceStates,
        Func onStatusUpdatedLocked) {
    Mutex::Autolock lock(mStatusLock);
    StatusInternal oldStatus = mStatus;
    mStatus = status;

    if (oldStatus == status) {
        return;
    }

    ALOGV("%s: Status has changed for camera ID %s from %#x to %#x", __FUNCTION__,
            cameraId.c_str(), eToI(oldStatus), eToI(status));

    if (oldStatus == StatusInternal::NOT_PRESENT &&
            (status != StatusInternal::PRESENT &&
             status != StatusInternal::ENUMERATING)) {

        ALOGW("%s: From NOT_PRESENT can only transition into PRESENT or ENUMERATING",
                __FUNCTION__);
        mStatus = oldStatus;
        return;
    }

    /**
     * Sometimes we want to conditionally do a transition.
     * For example if a client disconnects, we want to go to PRESENT
     * only if we weren't already in NOT_PRESENT or ENUMERATING.
     */
    for (auto& rejectStatus : rejectSourceStates) {
        if (oldStatus == rejectStatus) {
            ALOGV("%s: Rejecting status transition for Camera ID %s,  since the source "
                    "state was was in one of the bad states.", __FUNCTION__, cameraId.c_str());
            mStatus = oldStatus;
            return;
        }
    }

    onStatusUpdatedLocked(cameraId, status);
}

status_t CameraService::getTorchStatusLocked(
        const std::string& cameraId,
        TorchModeStatus *status) const {
    if (!status) {
        return BAD_VALUE;
    }
    ssize_t index = mTorchStatusMap.indexOfKey(cameraId);
    if (index == NAME_NOT_FOUND) {
        // invalid camera ID or the camera doesn't have a flash unit
        return NAME_NOT_FOUND;
    }

    *status = mTorchStatusMap.valueAt(index);
    return OK;
}

status_t CameraService::setTorchStatusLocked(const std::string& cameraId,
        TorchModeStatus status) {
    ssize_t index = mTorchStatusMap.indexOfKey(cameraId);
    if (index == NAME_NOT_FOUND) {
        return BAD_VALUE;
    }
    mTorchStatusMap.editValueAt(index) = status;

    return OK;
}

std::list<std::string> CameraService::getLogicalCameras(
        const std::string& physicalCameraId) {
    std::list<std::string> retList;
    Mutex::Autolock lock(mCameraStatesLock);
    for (const auto& state : mCameraStates) {
        if (state.second->containsPhysicalCamera(physicalCameraId)) {
            retList.emplace_back(state.first);
        }
    }
    return retList;
}

void CameraService::notifyPhysicalCameraStatusLocked(int32_t status,
        const std::string& physicalCameraId, const std::list<std::string>& logicalCameraIds,
        SystemCameraKind deviceKind, int32_t deviceId) {
    // mStatusListenerLock is expected to be locked
    for (const auto& logicalCameraId : logicalCameraIds) {
        for (auto& listener : mListenerList) {
            // Note: we check only the deviceKind of the physical camera id
            // since, logical camera ids and their physical camera ids are
            // guaranteed to have the same system camera kind.
            if (shouldSkipStatusUpdates(deviceKind, listener->isVendorListener(),
                    listener->getListenerPid(), listener->getListenerUid())) {
                ALOGV("Skipping discovery callback for system-only camera device %s",
                        physicalCameraId.c_str());
                continue;
            }
            auto ret = listener->getListener()->onPhysicalCameraStatusChanged(status,
                    logicalCameraId, physicalCameraId, deviceId);
            listener->handleBinderStatus(ret,
                    "%s: Failed to trigger onPhysicalCameraStatusChanged for %d:%d: %d",
                    __FUNCTION__, listener->getListenerUid(), listener->getListenerPid(),
                    ret.exceptionCode());
        }
    }
}

void CameraService::blockClientsForUid(uid_t uid) {
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr && basicClient->getClientUid() == uid) {
                basicClient->block();
            }
        }
    }
}

void CameraService::blockAllClients() {
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                basicClient->block();
            }
        }
    }
}

void CameraService::blockPrivacyEnabledClients() {
    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                std::string pkgName = basicClient->getPackageName();
                bool cameraPrivacyEnabled =
                        mSensorPrivacyPolicy->isCameraPrivacyEnabled(toString16(pkgName));
                if (cameraPrivacyEnabled) {
                    basicClient->block();
                }
           }
        }
    }
}

// NOTE: This is a remote API - make sure all args are validated
status_t CameraService::shellCommand(int in, int out, int err, const Vector<String16>& args) {
    if (!checkCallingPermission(toString16(sManageCameraPermission), nullptr, nullptr)) {
        return PERMISSION_DENIED;
    }
    if (in == BAD_TYPE || out == BAD_TYPE || err == BAD_TYPE) {
        return BAD_VALUE;
    }
    if (args.size() >= 3 && args[0] == toString16("set-uid-state")) {
        return handleSetUidState(args, err);
    } else if (args.size() >= 2 && args[0] == toString16("reset-uid-state")) {
        return handleResetUidState(args, err);
    } else if (args.size() >= 2 && args[0] == toString16("get-uid-state")) {
        return handleGetUidState(args, out, err);
    } else if (args.size() >= 2 && args[0] == toString16("set-rotate-and-crop")) {
        return handleSetRotateAndCrop(args);
    } else if (args.size() >= 1 && args[0] == toString16("get-rotate-and-crop")) {
        return handleGetRotateAndCrop(out);
    } else if (args.size() >= 2 && args[0] == toString16("set-autoframing")) {
        return handleSetAutoframing(args);
    } else if (args.size() >= 1 && args[0] == toString16("get-autoframing")) {
        return handleGetAutoframing(out);
    } else if (args.size() >= 2 && args[0] == toString16("set-image-dump-mask")) {
        return handleSetImageDumpMask(args);
    } else if (args.size() >= 1 && args[0] == toString16("get-image-dump-mask")) {
        return handleGetImageDumpMask(out);
    } else if (args.size() >= 2 && args[0] == toString16("set-camera-mute")) {
        return handleSetCameraMute(args);
    } else if (args.size() >= 2 && args[0] == toString16("set-stream-use-case-override")) {
        return handleSetStreamUseCaseOverrides(args);
    } else if (args.size() >= 1 && args[0] == toString16("clear-stream-use-case-override")) {
        handleClearStreamUseCaseOverrides();
        return OK;
    } else if (args.size() >= 1 && args[0] == toString16("set-zoom-override")) {
        return handleSetZoomOverride(args);
    } else if (args.size() >= 2 && args[0] == toString16("watch")) {
        return handleWatchCommand(args, in, out);
    } else if (args.size() >= 2 && args[0] == toString16("set-watchdog")) {
        return handleSetCameraServiceWatchdog(args);
    } else if (args.size() == 1 && args[0] == toString16("help")) {
        printHelp(out);
        return OK;
    }
    printHelp(err);
    return BAD_VALUE;
}

status_t CameraService::handleSetUidState(const Vector<String16>& args, int err) {
    std::string packageName = toStdString(args[1]);

    bool active = false;
    if (args[2] == toString16("active")) {
        active = true;
    } else if ((args[2] != toString16("idle"))) {
        ALOGE("Expected active or idle but got: '%s'", toStdString(args[2]).c_str());
        return BAD_VALUE;
    }

    int userId = 0;
    if (args.size() >= 5 && args[3] == toString16("--user")) {
        userId = atoi(toStdString(args[4]).c_str());
    }

    uid_t uid;
    if (getUidForPackage(packageName, userId, uid, err) == BAD_VALUE) {
        return BAD_VALUE;
    }

    mUidPolicy->addOverrideUid(uid, packageName, active);
    return NO_ERROR;
}

status_t CameraService::handleResetUidState(const Vector<String16>& args, int err) {
    std::string packageName = toStdString(args[1]);

    int userId = 0;
    if (args.size() >= 4 && args[2] == toString16("--user")) {
        userId = atoi(toStdString(args[3]).c_str());
    }

    uid_t uid;
    if (getUidForPackage(packageName, userId, uid, err) == BAD_VALUE) {
        return BAD_VALUE;
    }

    mUidPolicy->removeOverrideUid(uid, packageName);
    return NO_ERROR;
}

status_t CameraService::handleGetUidState(const Vector<String16>& args, int out, int err) {
    std::string packageName = toStdString(args[1]);

    int userId = 0;
    if (args.size() >= 4 && args[2] == toString16("--user")) {
        userId = atoi(toStdString(args[3]).c_str());
    }

    uid_t uid;
    if (getUidForPackage(packageName, userId, uid, err) == BAD_VALUE) {
        return BAD_VALUE;
    }

    if (mUidPolicy->isUidActive(uid, packageName)) {
        return dprintf(out, "active\n");
    } else {
        return dprintf(out, "idle\n");
    }
}

status_t CameraService::handleSetRotateAndCrop(const Vector<String16>& args) {
    int rotateValue = atoi(toStdString(args[1]).c_str());
    if (rotateValue < ANDROID_SCALER_ROTATE_AND_CROP_NONE ||
            rotateValue > ANDROID_SCALER_ROTATE_AND_CROP_AUTO) return BAD_VALUE;
    Mutex::Autolock lock(mServiceLock);

    mOverrideRotateAndCropMode = rotateValue;

    if (rotateValue == ANDROID_SCALER_ROTATE_AND_CROP_AUTO) return OK;

    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                basicClient->setRotateAndCropOverride(rotateValue);
            }
        }
    }

    return OK;
}

status_t CameraService::handleSetAutoframing(const Vector<String16>& args) {
    char* end;
    int autoframingValue = (int) strtol(toStdString(args[1]).c_str(), &end, /*base=*/10);
    if ((*end != '\0') ||
            (autoframingValue != ANDROID_CONTROL_AUTOFRAMING_OFF &&
             autoframingValue != ANDROID_CONTROL_AUTOFRAMING_ON &&
             autoframingValue != ANDROID_CONTROL_AUTOFRAMING_AUTO)) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mServiceLock);
    mOverrideAutoframingMode = autoframingValue;

    if (autoframingValue == ANDROID_CONTROL_AUTOFRAMING_AUTO) return OK;

    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                basicClient->setAutoframingOverride(autoframingValue);
            }
        }
    }

    return OK;
}

status_t CameraService::handleSetCameraServiceWatchdog(const Vector<String16>& args) {
    int enableWatchdog = atoi(toStdString(args[1]).c_str());

    if (enableWatchdog < 0 || enableWatchdog > 1) return BAD_VALUE;

    Mutex::Autolock lock(mServiceLock);

    mCameraServiceWatchdogEnabled = enableWatchdog;

    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                basicClient->setCameraServiceWatchdog(enableWatchdog);
            }
        }
    }

    return OK;
}

status_t CameraService::handleGetRotateAndCrop(int out) {
    Mutex::Autolock lock(mServiceLock);

    return dprintf(out, "rotateAndCrop override: %d\n", mOverrideRotateAndCropMode);
}

status_t CameraService::handleGetAutoframing(int out) {
    Mutex::Autolock lock(mServiceLock);

    return dprintf(out, "autoframing override: %d\n", mOverrideAutoframingMode);
}

status_t CameraService::handleSetImageDumpMask(const Vector<String16>& args) {
    char *endPtr;
    errno = 0;
    std::string maskString = toStdString(args[1]);
    long maskValue = strtol(maskString.c_str(), &endPtr, 10);

    if (errno != 0) return BAD_VALUE;
    if (endPtr != maskString.c_str() + maskString.size()) return BAD_VALUE;
    if (maskValue < 0 || maskValue > 1) return BAD_VALUE;

    Mutex::Autolock lock(mServiceLock);

    mImageDumpMask = maskValue;

    return OK;
}

status_t CameraService::handleGetImageDumpMask(int out) {
    Mutex::Autolock lock(mServiceLock);

    return dprintf(out, "Image dump mask: %d\n", mImageDumpMask);
}

status_t CameraService::handleSetCameraMute(const Vector<String16>& args) {
    int muteValue = strtol(toStdString(args[1]).c_str(), nullptr, 10);
    if (errno != 0) return BAD_VALUE;

    if (muteValue < 0 || muteValue > 1) return BAD_VALUE;
    Mutex::Autolock lock(mServiceLock);

    mOverrideCameraMuteMode = (muteValue == 1);

    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                if (basicClient->supportsCameraMute()) {
                    basicClient->setCameraMute(mOverrideCameraMuteMode);
                }
            }
        }
    }

    return OK;
}

status_t CameraService::handleSetStreamUseCaseOverrides(const Vector<String16>& args) {
    std::vector<int64_t> useCasesOverride;
    for (size_t i = 1; i < args.size(); i++) {
        int64_t useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT;
        std::string arg = toStdString(args[i]);
        if (arg == "DEFAULT") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT;
        } else if (arg == "PREVIEW") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW;
        } else if (arg == "STILL_CAPTURE") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_STILL_CAPTURE;
        } else if (arg == "VIDEO_RECORD") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_RECORD;
        } else if (arg == "PREVIEW_VIDEO_STILL") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW_VIDEO_STILL;
        } else if (arg == "VIDEO_CALL") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_CALL;
        } else if (arg == "CROPPED_RAW") {
            useCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_CROPPED_RAW;
        } else {
            ALOGE("%s: Invalid stream use case %s", __FUNCTION__, arg.c_str());
            return BAD_VALUE;
        }
        useCasesOverride.push_back(useCase);
    }

    Mutex::Autolock lock(mServiceLock);
    mStreamUseCaseOverrides = std::move(useCasesOverride);

    return OK;
}

void CameraService::handleClearStreamUseCaseOverrides() {
    Mutex::Autolock lock(mServiceLock);
    mStreamUseCaseOverrides.clear();
}

status_t CameraService::handleSetZoomOverride(const Vector<String16>& args) {
    char* end;
    int zoomOverrideValue = strtol(toStdString(args[1]).c_str(), &end, /*base=*/10);
    if ((*end != '\0') ||
            (zoomOverrideValue != -1 &&
             zoomOverrideValue != ANDROID_CONTROL_SETTINGS_OVERRIDE_OFF &&
             zoomOverrideValue != ANDROID_CONTROL_SETTINGS_OVERRIDE_ZOOM)) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mServiceLock);
    mZoomOverrideValue = zoomOverrideValue;

    const auto clients = mActiveClientManager.getAll();
    for (auto& current : clients) {
        if (current != nullptr) {
            const auto basicClient = current->getValue();
            if (basicClient.get() != nullptr) {
                if (basicClient->supportsZoomOverride()) {
                    basicClient->setZoomOverride(mZoomOverrideValue);
                }
            }
        }
    }

    return OK;
}

status_t CameraService::handleWatchCommand(const Vector<String16>& args, int inFd, int outFd) {
    if (args.size() >= 3 && args[1] == toString16("start")) {
        return startWatchingTags(args, outFd);
    } else if (args.size() == 2 && args[1] == toString16("stop")) {
        return stopWatchingTags(outFd);
    } else if (args.size() == 2 && args[1] == toString16("dump")) {
        return printWatchedTags(outFd);
    } else if (args.size() >= 2 && args[1] == toString16("live")) {
        return printWatchedTagsUntilInterrupt(args, inFd, outFd);
    } else if (args.size() == 2 && args[1] == toString16("clear")) {
        return clearCachedMonitoredTagDumps(outFd);
    }
    dprintf(outFd, "Camera service watch commands:\n"
                 "  start -m <comma_separated_tag_list> [-c <comma_separated_client_list>]\n"
                 "        starts watching the provided tags for clients with provided package\n"
                 "        recognizes tag shorthands like '3a'\n"
                 "        watches all clients if no client is passed, or if 'all' is listed\n"
                 "  dump dumps the monitoring information and exits\n"
                 "  stop stops watching all tags\n"
                 "  live [-n <refresh_interval_ms>]\n"
                 "        prints the monitored information in real time\n"
                 "        Hit return to exit\n"
                 "  clear clears all buffers storing information for watch command");
    return BAD_VALUE;
}

status_t CameraService::startWatchingTags(const Vector<String16> &args, int outFd) {
    Mutex::Autolock lock(mLogLock);
    size_t tagsIdx; // index of '-m'
    String16 tags("");
    for (tagsIdx = 2; tagsIdx < args.size() && args[tagsIdx] != toString16("-m"); tagsIdx++);
    if (tagsIdx < args.size() - 1) {
        tags = args[tagsIdx + 1];
    } else {
        dprintf(outFd, "No tags provided.\n");
        return BAD_VALUE;
    }

    size_t clientsIdx; // index of '-c'
    // watch all clients if no clients are provided
    String16 clients = toString16(kWatchAllClientsFlag);
    for (clientsIdx = 2; clientsIdx < args.size() && args[clientsIdx] != toString16("-c");
         clientsIdx++);
    if (clientsIdx < args.size() - 1) {
        clients = args[clientsIdx + 1];
    }
    parseClientsToWatchLocked(toStdString(clients));

    // track tags to initialize future clients with the monitoring information
    mMonitorTags = toStdString(tags);

    bool serviceLock = tryLock(mServiceLock);
    int numWatchedClients = 0;
    auto cameraClients = mActiveClientManager.getAll();
    for (const auto &clientDescriptor: cameraClients) {
        if (clientDescriptor == nullptr) { continue; }
        sp<BasicClient> client = clientDescriptor->getValue();
        if (client.get() == nullptr) { continue; }

        if (isClientWatchedLocked(client.get())) {
            client->startWatchingTags(mMonitorTags, outFd);
            numWatchedClients++;
        }
    }
    dprintf(outFd, "Started watching %d active clients\n", numWatchedClients);

    if (serviceLock) { mServiceLock.unlock(); }
    return OK;
}

status_t CameraService::stopWatchingTags(int outFd) {
    // clear mMonitorTags to prevent new clients from monitoring tags at initialization
    Mutex::Autolock lock(mLogLock);
    mMonitorTags = "";

    mWatchedClientPackages.clear();
    mWatchedClientsDumpCache.clear();

    bool serviceLock = tryLock(mServiceLock);
    auto cameraClients = mActiveClientManager.getAll();
    for (const auto &clientDescriptor : cameraClients) {
        if (clientDescriptor == nullptr) { continue; }
        sp<BasicClient> client = clientDescriptor->getValue();
        if (client.get() == nullptr) { continue; }
        client->stopWatchingTags(outFd);
    }
    dprintf(outFd, "Stopped watching all clients.\n");
    if (serviceLock) { mServiceLock.unlock(); }
    return OK;
}

status_t CameraService::clearCachedMonitoredTagDumps(int outFd) {
    Mutex::Autolock lock(mLogLock);
    size_t clearedSize = mWatchedClientsDumpCache.size();
    mWatchedClientsDumpCache.clear();
    dprintf(outFd, "Cleared tag information of %zu cached clients.\n", clearedSize);
    return OK;
}

status_t CameraService::printWatchedTags(int outFd) {
    Mutex::Autolock logLock(mLogLock);
    std::set<std::string> connectedMonitoredClients;

    bool printedSomething = false; // tracks if any monitoring information was printed
                                   // (from either cached or active clients)

    bool serviceLock = tryLock(mServiceLock);
    // get all watched clients that are currently connected
    for (const auto &clientDescriptor: mActiveClientManager.getAll()) {
        if (clientDescriptor == nullptr) { continue; }

        sp<BasicClient> client = clientDescriptor->getValue();
        if (client.get() == nullptr) { continue; }
        if (!isClientWatchedLocked(client.get())) { continue; }

        std::vector<std::string> dumpVector;
        client->dumpWatchedEventsToVector(dumpVector);

        size_t printIdx = dumpVector.size();
        if (printIdx == 0) {
            continue;
        }

        // Print tag dumps for active client
        const std::string &cameraId = clientDescriptor->getKey();
        dprintf(outFd, "Client: %s (active)\n", client->getPackageName().c_str());
        while(printIdx > 0) {
            printIdx--;
            dprintf(outFd, "%s:%s  %s", cameraId.c_str(), client->getPackageName().c_str(),
                    dumpVector[printIdx].c_str());
        }
        dprintf(outFd, "\n");
        printedSomething = true;

        connectedMonitoredClients.emplace(client->getPackageName());
    }
    if (serviceLock) { mServiceLock.unlock(); }

    // Print entries in mWatchedClientsDumpCache for clients that are not connected
    for (const auto &kv: mWatchedClientsDumpCache) {
        const std::string &package = kv.first;
        if (connectedMonitoredClients.find(package) != connectedMonitoredClients.end()) {
            continue;
        }

        dprintf(outFd, "Client: %s (cached)\n", package.c_str());
        dprintf(outFd, "%s\n", kv.second.c_str());
        printedSomething = true;
    }

    if (!printedSomething) {
        dprintf(outFd, "No monitoring information to print.\n");
    }

    return OK;
}

// Print all events in vector `events' that came after lastPrintedEvent
void printNewWatchedEvents(int outFd,
                           const std::string &cameraId,
                           const std::string &packageName,
                           const std::vector<std::string> &events,
                           const std::string &lastPrintedEvent) {
    if (events.empty()) { return; }

    // index of lastPrintedEvent in events.
    // lastPrintedIdx = events.size() if lastPrintedEvent is not in events
    size_t lastPrintedIdx;
    for (lastPrintedIdx = 0;
         lastPrintedIdx < events.size() && lastPrintedEvent != events[lastPrintedIdx];
         lastPrintedIdx++);

    if (lastPrintedIdx == 0) { return; } // early exit if no new event in `events`

    // print events in chronological order (latest event last)
    size_t idxToPrint = lastPrintedIdx;
    do {
        idxToPrint--;
        dprintf(outFd, "%s:%s  %s", cameraId.c_str(), packageName.c_str(),
                events[idxToPrint].c_str());
    } while (idxToPrint != 0);
}

// Returns true if adb shell cmd watch should be interrupted based on data in inFd. The watch
// command should be interrupted if the user presses the return key, or if user loses any way to
// signal interrupt.
// If timeoutMs == 0, this function will always return false
bool shouldInterruptWatchCommand(int inFd, int outFd, long timeoutMs) {
    struct timeval startTime;
    int startTimeError = gettimeofday(&startTime, nullptr);
    if (startTimeError) {
        dprintf(outFd, "Failed waiting for interrupt, aborting.\n");
        return true;
    }

    const nfds_t numFds = 1;
    struct pollfd pollFd = { .fd = inFd, .events = POLLIN, .revents = 0 };

    struct timeval currTime;
    char buffer[2];
    while(true) {
        int currTimeError = gettimeofday(&currTime, nullptr);
        if (currTimeError) {
            dprintf(outFd, "Failed waiting for interrupt, aborting.\n");
            return true;
        }

        long elapsedTimeMs = ((currTime.tv_sec - startTime.tv_sec) * 1000L)
                + ((currTime.tv_usec - startTime.tv_usec) / 1000L);
        int remainingTimeMs = (int) (timeoutMs - elapsedTimeMs);

        if (remainingTimeMs <= 0) {
            // No user interrupt within timeoutMs, don't interrupt watch command
            return false;
        }

        int numFdsUpdated = poll(&pollFd, numFds, remainingTimeMs);
        if (numFdsUpdated < 0) {
            dprintf(outFd, "Failed while waiting for user input. Exiting.\n");
            return true;
        }

        if (numFdsUpdated == 0) {
            // No user input within timeoutMs, don't interrupt watch command
            return false;
        }

        if (!(pollFd.revents & POLLIN)) {
            dprintf(outFd, "Failed while waiting for user input. Exiting.\n");
            return true;
        }

        ssize_t sizeRead = read(inFd, buffer, sizeof(buffer) - 1);
        if (sizeRead < 0) {
            dprintf(outFd, "Error reading user input. Exiting.\n");
            return true;
        }

        if (sizeRead == 0) {
            // Reached end of input fd (can happen if input is piped)
            // User has no way to signal an interrupt, so interrupt here
            return true;
        }

        if (buffer[0] == '\n') {
            // User pressed return, interrupt watch command.
            return true;
        }
    }
}

status_t CameraService::printWatchedTagsUntilInterrupt(const Vector<String16> &args,
                                                       int inFd, int outFd) {
    // Figure out refresh interval, if present in args
    long refreshTimeoutMs = 1000L; // refresh every 1s by default
    if (args.size() > 2) {
        size_t intervalIdx; // index of '-n'
        for (intervalIdx = 2; intervalIdx < args.size() && toString16("-n") != args[intervalIdx];
             intervalIdx++);

        size_t intervalValIdx = intervalIdx + 1;
        if (intervalValIdx < args.size()) {
            refreshTimeoutMs = strtol(toStdString(args[intervalValIdx]).c_str(), nullptr, 10);
            if (errno) { return BAD_VALUE; }
        }
    }

    // Set min timeout of 10ms. This prevents edge cases in polling when timeout of 0 is passed.
    refreshTimeoutMs = refreshTimeoutMs < 10 ? 10 : refreshTimeoutMs;

    dprintf(outFd, "Press return to exit...\n\n");
    std::map<std::string, std::string> packageNameToLastEvent;

    while (true) {
        bool serviceLock = tryLock(mServiceLock);
        auto cameraClients = mActiveClientManager.getAll();
        if (serviceLock) { mServiceLock.unlock(); }

        for (const auto& clientDescriptor : cameraClients) {
            Mutex::Autolock lock(mLogLock);
            if (clientDescriptor == nullptr) { continue; }

            sp<BasicClient> client = clientDescriptor->getValue();
            if (client.get() == nullptr) { continue; }
            if (!isClientWatchedLocked(client.get())) { continue; }

            const std::string &packageName = client->getPackageName();
            // This also initializes the map entries with an empty string
            const std::string& lastPrintedEvent = packageNameToLastEvent[packageName];

            std::vector<std::string> latestEvents;
            client->dumpWatchedEventsToVector(latestEvents);

            if (!latestEvents.empty()) {
                printNewWatchedEvents(outFd,
                                      clientDescriptor->getKey(),
                                      packageName,
                                      latestEvents,
                                      lastPrintedEvent);
                packageNameToLastEvent[packageName] = latestEvents[0];
            }
        }
        if (shouldInterruptWatchCommand(inFd, outFd, refreshTimeoutMs)) {
            break;
        }
    }
    return OK;
}

void CameraService::parseClientsToWatchLocked(const std::string &clients) {
    mWatchedClientPackages.clear();

    std::istringstream iss(clients);
    std::string nextClient;

    while (std::getline(iss, nextClient, ',')) {
        if (nextClient == kWatchAllClientsFlag) {
            // Don't need to track any other package if 'all' is present
            mWatchedClientPackages.clear();
            mWatchedClientPackages.emplace(kWatchAllClientsFlag);
            break;
        }

        // track package names
        mWatchedClientPackages.emplace(nextClient);
    }
}

status_t CameraService::printHelp(int out) {
    return dprintf(out, "Camera service commands:\n"
        "  get-uid-state <PACKAGE> [--user USER_ID] gets the uid state\n"
        "  set-uid-state <PACKAGE> <active|idle> [--user USER_ID] overrides the uid state\n"
        "  reset-uid-state <PACKAGE> [--user USER_ID] clears the uid state override\n"
        "  set-rotate-and-crop <ROTATION> overrides the rotate-and-crop value for AUTO backcompat\n"
        "      Valid values 0=0 deg, 1=90 deg, 2=180 deg, 3=270 deg, 4=No override\n"
        "  get-rotate-and-crop returns the current override rotate-and-crop value\n"
        "  set-autoframing <VALUE> overrides the autoframing value for AUTO\n"
        "      Valid values 0=false, 1=true, 2=auto\n"
        "  get-autoframing returns the current override autoframing value\n"
        "  set-image-dump-mask <MASK> specifies the formats to be saved to disk\n"
        "      Valid values 0=OFF, 1=ON for JPEG\n"
        "  get-image-dump-mask returns the current image-dump-mask value\n"
        "  set-camera-mute <0/1> enable or disable camera muting\n"
        "  set-stream-use-case-override <usecase1> <usecase2> ... override stream use cases\n"
        "      Use cases applied in descending resolutions. So usecase1 is assigned to the\n"
        "      largest resolution, usecase2 is assigned to the 2nd largest resolution, and so\n"
        "      on. In case the number of usecases is smaller than the number of streams, the\n"
        "      last use case is assigned to all the remaining streams. In case of multiple\n"
        "      streams with the same resolution, the tie-breaker is (JPEG, RAW, YUV, and PRIV)\n"
        "      Valid values are (case sensitive): DEFAULT, PREVIEW, STILL_CAPTURE, VIDEO_RECORD,\n"
        "      PREVIEW_VIDEO_STILL, VIDEO_CALL, CROPPED_RAW\n"
        "  clear-stream-use-case-override clear the stream use case override\n"
        "  set-zoom-override <-1/0/1> enable or disable zoom override\n"
        "      Valid values -1: do not override, 0: override to OFF, 1: override to ZOOM\n"
        "  set-watchdog <VALUE> enables or disables the camera service watchdog\n"
        "      Valid values 0=disable, 1=enable\n"
        "  watch <start|stop|dump|print|clear> manages tag monitoring in connected clients\n"
        "  help print this message\n");
}

bool CameraService::isClientWatched(const BasicClient *client) {
    Mutex::Autolock lock(mLogLock);
    return isClientWatchedLocked(client);
}

bool CameraService::isClientWatchedLocked(const BasicClient *client) {
    return mWatchedClientPackages.find(kWatchAllClientsFlag) != mWatchedClientPackages.end() ||
           mWatchedClientPackages.find(client->getPackageName()) != mWatchedClientPackages.end();
}

int32_t CameraService::updateAudioRestriction() {
    Mutex::Autolock lock(mServiceLock);
    return updateAudioRestrictionLocked();
}

int32_t CameraService::updateAudioRestrictionLocked() {
    int32_t mode = 0;
    // iterate through all active client
    for (const auto& i : mActiveClientManager.getAll()) {
        const auto clientSp = i->getValue();
        mode |= clientSp->getAudioRestriction();
    }

    bool modeChanged = (mAudioRestriction != mode);
    mAudioRestriction = mode;
    if (modeChanged) {
        mAppOps.setCameraAudioRestriction(mode);
    }
    return mode;
}

status_t CameraService::checkIfInjectionCameraIsPresent(const std::string& externalCamId,
        sp<BasicClient> clientSp) {
    std::unique_ptr<AutoConditionLock> lock =
            AutoConditionLock::waitAndAcquire(mServiceLockWrapper);
    status_t res = NO_ERROR;
    if ((res = checkIfDeviceIsUsable(externalCamId)) != NO_ERROR) {
        ALOGW("Device %s is not usable!", externalCamId.c_str());
        mInjectionStatusListener->notifyInjectionError(
                externalCamId, UNKNOWN_TRANSACTION);
        clientSp->notifyError(
                hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                CaptureResultExtras());

        // Do not hold mServiceLock while disconnecting clients, but retain the condition blocking
        // other clients from connecting in mServiceLockWrapper if held
        mServiceLock.unlock();

        // Clear caller identity temporarily so client disconnect PID checks work correctly
        int64_t token = clearCallingIdentity();
        clientSp->disconnect();
        restoreCallingIdentity(token);

        // Reacquire mServiceLock
        mServiceLock.lock();
    }

    return res;
}

void CameraService::clearInjectionParameters() {
    {
        Mutex::Autolock lock(mInjectionParametersLock);
        mInjectionInitPending = false;
        mInjectionInternalCamId = "";
    }
    mInjectionExternalCamId = "";
    mInjectionStatusListener->removeListener();
}

} // namespace android
