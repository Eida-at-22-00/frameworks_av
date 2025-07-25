/*
 * Copyright (C) 2012 The Android Open Source Project
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
#define LOG_TAG "ServiceUtilities"

#include <audio_utils/clock.h>
#include <android-base/properties.h>
#include <binder/AppOpsManager.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/PermissionCache.h>
#include "mediautils/ServiceUtilities.h"
#include <system/audio-hal-enums.h>
#include <media/AidlConversion.h>
#include <media/AidlConversionUtil.h>
#include <android/content/AttributionSourceState.h>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <pwd.h>
#include <thread>

/* When performing permission checks we do not use permission cache for
 * runtime permissions (protection level dangerous) as they may change at
 * runtime. All other permissions (protection level normal and dangerous)
 * can be cached as they never change. Of course all permission checked
 * here are platform defined.
 */

namespace android {

namespace {
constexpr auto PERMISSION_GRANTED = permission::PermissionChecker::PERMISSION_GRANTED;
constexpr auto PERMISSION_HARD_DENIED = permission::PermissionChecker::PERMISSION_HARD_DENIED;
}

using content::AttributionSourceState;

static const String16 sAndroidPermissionRecordAudio("android.permission.RECORD_AUDIO");
static const String16 sModifyPhoneState("android.permission.MODIFY_PHONE_STATE");
static const String16 sModifyAudioRouting("android.permission.MODIFY_AUDIO_ROUTING");
static const String16 sCallAudioInterception("android.permission.CALL_AUDIO_INTERCEPTION");
static const String16 sModifyAudioSettingsPrivileged(
        "android.permission.MODIFY_AUDIO_SETTINGS_PRIVILEGED");

static String16 resolveCallingPackage(PermissionController& permissionController,
        const std::optional<String16> opPackageName, uid_t uid) {
    if (opPackageName.has_value() && opPackageName.value().size() > 0) {
        return opPackageName.value();
    }
    // In some cases the calling code has no access to the package it runs under.
    // For example, code using the wilhelm framework's OpenSL-ES APIs. In this
    // case we will get the packages for the calling UID and pick the first one
    // for attributing the app op. This will work correctly for runtime permissions
    // as for legacy apps we will toggle the app op for all packages in the UID.
    // The caveat is that the operation may be attributed to the wrong package and
    // stats based on app ops may be slightly off.
    Vector<String16> packages;
    permissionController.getPackagesForUid(uid, packages);
    if (packages.isEmpty()) {
        ALOGE("No packages for uid %d", uid);
        return String16();
    }
    return packages[0];
}

// NOTE/TODO(b/379754682):
// AUDIO_SOURCE_VOICE_CALL is handled specially:
// CALL includes both uplink and downlink, but we attribute RECORD_OP (only), since
// there is not support for noting multiple ops.
int32_t getOpForSource(audio_source_t source) {
  switch (source) {
    // BEGIN output sources
    case AUDIO_SOURCE_FM_TUNER:
        return AppOpsManager::OP_NONE;
    case AUDIO_SOURCE_ECHO_REFERENCE: // fallthrough
    case AUDIO_SOURCE_REMOTE_SUBMIX:
        // TODO -- valid in all cases?
      return AppOpsManager::OP_RECORD_AUDIO_OUTPUT;
    case AUDIO_SOURCE_VOICE_DOWNLINK:
      return AppOpsManager::OP_RECORD_INCOMING_PHONE_AUDIO;
    // END output sources
    case AUDIO_SOURCE_HOTWORD:
      return AppOpsManager::OP_RECORD_AUDIO_HOTWORD;
    case AUDIO_SOURCE_DEFAULT:
    default:
      return AppOpsManager::OP_RECORD_AUDIO;
  }
}

bool isRecordOpRequired(audio_source_t source) {
  switch (source) {
    case AUDIO_SOURCE_FM_TUNER:
    case AUDIO_SOURCE_ECHO_REFERENCE: // fallthrough
    case AUDIO_SOURCE_REMOTE_SUBMIX:
    case AUDIO_SOURCE_VOICE_DOWNLINK:
        return false;
    default:
      return true;
  }
}

std::optional<AttributionSourceState> resolveAttributionSource(
        const AttributionSourceState& callerAttributionSource, const uint32_t virtualDeviceId) {
    AttributionSourceState nextAttributionSource = callerAttributionSource;

    if (!nextAttributionSource.packageName.has_value()) {
        nextAttributionSource = AttributionSourceState(nextAttributionSource);
        PermissionController permissionController;
        const uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(nextAttributionSource.uid));
        nextAttributionSource.packageName = VALUE_OR_FATAL(legacy2aidl_String16_string(
                resolveCallingPackage(permissionController, VALUE_OR_FATAL(
                aidl2legacy_string_view_String16(nextAttributionSource.packageName.value_or(""))),
                uid)));
        if (!nextAttributionSource.packageName.has_value()) {
            return std::nullopt;
        }
    }
    nextAttributionSource.deviceId = virtualDeviceId;

    AttributionSourceState myAttributionSource;
    myAttributionSource.uid = VALUE_OR_FATAL(android::legacy2aidl_uid_t_int32_t(getuid()));
    myAttributionSource.pid = VALUE_OR_FATAL(android::legacy2aidl_pid_t_int32_t(getpid()));
    // Create a static token for audioserver requests, which identifies the
    // audioserver to the app ops system
    static sp<BBinder> appOpsToken = sp<BBinder>::make();
    myAttributionSource.token = appOpsToken;
    myAttributionSource.deviceId = virtualDeviceId;
    myAttributionSource.next.push_back(nextAttributionSource);

    return std::optional<AttributionSourceState>{myAttributionSource};
}


static int checkRecordingInternal(const AttributionSourceState &attributionSource,
                                       const uint32_t virtualDeviceId,
                                       const String16 &msg, bool start, audio_source_t source) {
    // Okay to not track in app ops as audio server or media server is us and if
    // device is rooted security model is considered compromised.
    // system_server loses its RECORD_AUDIO permission when a secondary
    // user is active, but it is a core system service so let it through.
    // TODO(b/141210120): UserManager.DISALLOW_RECORD_AUDIO should not affect system user 0
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    if (isAudioServerOrMediaServerOrSystemServerOrRootUid(uid)) return PERMISSION_GRANTED;

    const int32_t attributedOpCode = getOpForSource(source);
    if (isRecordOpRequired(source)) {
        // We specify a pid and uid here as mediaserver (aka MediaRecorder or StageFrightRecorder)
        // may open a record track on behalf of a client. Note that pid may be a tid.
        // IMPORTANT: DON'T USE PermissionCache - RUNTIME PERMISSIONS CHANGE.
        std::optional<AttributionSourceState> resolvedAttributionSource =
                resolveAttributionSource(attributionSource, virtualDeviceId);
        if (!resolvedAttributionSource.has_value()) {
            return PERMISSION_HARD_DENIED;
        }

        permission::PermissionChecker permissionChecker;
        int permitted;
        if (start) {
            // Do a double-check, where we first check without actually starting in order to handle
            // the behavior of AppOps where ops are sometimes started but paused for SOFT_DENIED.
            // Since there is no way to maintain reference consensus due to this behavior, avoid
            // starting an op when a restriction is in place by first checking. In the case where we
            // startOp would fail, call a noteOp (which will also fail) instead. This preserves
            // behavior that is reliant on listening to op rejected events (such as the hint
            // dialogue to unmute the microphone). Technically racy, but very unlikely.
            //
            // TODO(b/294609684) To be removed when the pause state for an OP is removed.
            permitted = permissionChecker.checkPermissionForPreflightFromDatasource(
                    sAndroidPermissionRecordAudio, resolvedAttributionSource.value(), msg,
                    attributedOpCode);
            if (permitted == PERMISSION_GRANTED) {
                permitted = permissionChecker.checkPermissionForStartDataDeliveryFromDatasource(
                        sAndroidPermissionRecordAudio, resolvedAttributionSource.value(), msg,
                        attributedOpCode);
            } else {
                // intentionally don't set permitted
                permissionChecker.checkPermissionForDataDeliveryFromDatasource(
                            sAndroidPermissionRecordAudio, resolvedAttributionSource.value(), msg,
                            attributedOpCode);
            }
        } else {
            permitted = permissionChecker.checkPermissionForPreflightFromDatasource(
                    sAndroidPermissionRecordAudio, resolvedAttributionSource.value(), msg,
                    attributedOpCode);
        }

        return permitted;
    } else {
        if (attributedOpCode == AppOpsManager::OP_NONE) return PERMISSION_GRANTED;  // nothing to do
        AppOpsManager ap{};
        PermissionController pc{};
        return ap.startOpNoThrow(
                attributedOpCode, attributionSource.uid,
                resolveCallingPackage(pc,
                                      String16{attributionSource.packageName.value_or("").c_str()},
                                      attributionSource.uid),
                false,
                attributionSource.attributionTag.has_value()
                        ? String16{attributionSource.attributionTag.value().c_str()}
                        : String16{},
                msg);
    }
}

static constexpr int DEVICE_ID_DEFAULT = 0;

bool recordingAllowed(const AttributionSourceState &attributionSource, audio_source_t source) {
    return checkRecordingInternal(attributionSource, DEVICE_ID_DEFAULT, String16(), /*start*/ false,
                                  source) != PERMISSION_HARD_DENIED;
}

bool recordingAllowed(const AttributionSourceState &attributionSource,
                      const uint32_t virtualDeviceId,
                      audio_source_t source) {
    return checkRecordingInternal(attributionSource, virtualDeviceId,
                                  String16(), /*start*/ false, source) != PERMISSION_HARD_DENIED;
}

int startRecording(const AttributionSourceState& attributionSource,
                    const uint32_t virtualDeviceId,
                    const String16& msg,
                    audio_source_t source) {
    return checkRecordingInternal(attributionSource, virtualDeviceId, msg, /*start*/ true,
                                  source);
}

void finishRecording(const AttributionSourceState &attributionSource, uint32_t virtualDeviceId,
                     audio_source_t source) {
    // Okay to not track in app ops as audio server is us and if
    // device is rooted security model is considered compromised.
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    if (isAudioServerOrMediaServerOrSystemServerOrRootUid(uid)) return;

    const int32_t attributedOpCode = getOpForSource(source);
    if (isRecordOpRequired(source)) {
        // We specify a pid and uid here as mediaserver (aka MediaRecorder or StageFrightRecorder)
        // may open a record track on behalf of a client. Note that pid may be a tid.
        // IMPORTANT: DON'T USE PermissionCache - RUNTIME PERMISSIONS CHANGE.
        const std::optional<AttributionSourceState> resolvedAttributionSource =
                resolveAttributionSource(attributionSource, virtualDeviceId);
        if (!resolvedAttributionSource.has_value()) {
            return;
        }

        permission::PermissionChecker permissionChecker;
        permissionChecker.finishDataDeliveryFromDatasource(attributedOpCode,
                resolvedAttributionSource.value());
    } else {
        if (attributedOpCode == AppOpsManager::OP_NONE) return;  // nothing to do
        AppOpsManager ap{};
        PermissionController pc{};
        ap.finishOp(attributedOpCode, attributionSource.uid,
                    resolveCallingPackage(
                            pc, String16{attributionSource.packageName.value_or("").c_str()},
                            attributionSource.uid),
                    attributionSource.attributionTag.has_value()
                            ? String16{attributionSource.attributionTag.value().c_str()}
                            : String16{});
    }
}

bool captureAudioOutputAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sCaptureAudioOutput("android.permission.CAPTURE_AUDIO_OUTPUT");
    // Use PermissionChecker, which includes some logic for allowing the isolated
    // HotwordDetectionService to hold certain permissions.
    permission::PermissionChecker permissionChecker;
    bool ok = (permissionChecker.checkPermissionForPreflight(
            sCaptureAudioOutput, attributionSource, String16(),
            AppOpsManager::OP_NONE) != permission::PermissionChecker::PERMISSION_HARD_DENIED);
    if (!ok) ALOGV("Request requires android.permission.CAPTURE_AUDIO_OUTPUT");
    return ok;
}

bool captureMediaOutputAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sCaptureMediaOutput("android.permission.CAPTURE_MEDIA_OUTPUT");
    bool ok = PermissionCache::checkPermission(sCaptureMediaOutput, pid, uid);
    if (!ok) ALOGE("Request requires android.permission.CAPTURE_MEDIA_OUTPUT");
    return ok;
}

bool captureTunerAudioInputAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sCaptureTunerAudioInput("android.permission.CAPTURE_TUNER_AUDIO_INPUT");
    bool ok = PermissionCache::checkPermission(sCaptureTunerAudioInput, pid, uid);
    if (!ok) ALOGV("Request requires android.permission.CAPTURE_TUNER_AUDIO_INPUT");
    return ok;
}

bool captureVoiceCommunicationOutputAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    uid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sCaptureVoiceCommOutput(
        "android.permission.CAPTURE_VOICE_COMMUNICATION_OUTPUT");
    bool ok = PermissionCache::checkPermission(sCaptureVoiceCommOutput, pid, uid);
    if (!ok) ALOGE("Request requires android.permission.CAPTURE_VOICE_COMMUNICATION_OUTPUT");
    return ok;
}

bool bypassConcurrentPolicyAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    uid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sBypassConcurrentPolicy(
            "android.permission.BYPASS_CONCURRENT_RECORD_AUDIO_RESTRICTION ");
    // Use PermissionChecker, which includes some logic for allowing the isolated
    // HotwordDetectionService to hold certain permissions.
    bool ok = PermissionCache::checkPermission(sBypassConcurrentPolicy, pid, uid);
    if (!ok) {
        ALOGV("Request requires android.permission.BYPASS_CONCURRENT_RECORD_AUDIO_RESTRICTION");
    }
    return ok;
}

bool accessUltrasoundAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    uid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerOrRootUid(uid)) return true;
    static const String16 sAccessUltrasound(
        "android.permission.ACCESS_ULTRASOUND");
    bool ok = PermissionCache::checkPermission(sAccessUltrasound, pid, uid);
    if (!ok) ALOGE("Request requires android.permission.ACCESS_ULTRASOUND");
    return ok;
}

bool captureHotwordAllowed(const AttributionSourceState& attributionSource) {
    // CAPTURE_AUDIO_HOTWORD permission implies RECORD_AUDIO permission
    bool ok = recordingAllowed(attributionSource);

    if (ok) {
        static const String16 sCaptureHotwordAllowed("android.permission.CAPTURE_AUDIO_HOTWORD");
        // Use PermissionChecker, which includes some logic for allowing the isolated
        // HotwordDetectionService to hold certain permissions.
        permission::PermissionChecker permissionChecker;
        ok = (permissionChecker.checkPermissionForPreflight(
                sCaptureHotwordAllowed, attributionSource, String16(),
                AppOpsManager::OP_NONE) != permission::PermissionChecker::PERMISSION_HARD_DENIED);
    }
    if (!ok) ALOGV("android.permission.CAPTURE_AUDIO_HOTWORD");
    return ok;
}

bool settingsAllowed() {
    // given this is a permission check, could this be isAudioServerOrRootUid()?
    if (isAudioServerUid(IPCThreadState::self()->getCallingUid())) return true;
    static const String16 sAudioSettings("android.permission.MODIFY_AUDIO_SETTINGS");
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkCallingPermission(sAudioSettings);
    if (!ok) ALOGE("Request requires android.permission.MODIFY_AUDIO_SETTINGS");
    return ok;
}

bool modifyAudioRoutingAllowed() {
    return modifyAudioRoutingAllowed(getCallingAttributionSource());
}

bool modifyAudioRoutingAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerUid(uid)) return true;
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkPermission(sModifyAudioRouting, pid, uid);
    if (!ok) ALOGE("%s(): android.permission.MODIFY_AUDIO_ROUTING denied for uid %d",
        __func__, uid);
    return ok;
}

bool modifyDefaultAudioEffectsAllowed() {
    return modifyDefaultAudioEffectsAllowed(getCallingAttributionSource());
}

bool modifyDefaultAudioEffectsAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerUid(uid)) return true;

    static const String16 sModifyDefaultAudioEffectsAllowed(
            "android.permission.MODIFY_DEFAULT_AUDIO_EFFECTS");
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkPermission(sModifyDefaultAudioEffectsAllowed, pid, uid);
    ALOGE_IF(!ok, "%s(): android.permission.MODIFY_DEFAULT_AUDIO_EFFECTS denied for uid %d",
            __func__, uid);
    return ok;
}

bool modifyAudioSettingsPrivilegedAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    if (isAudioServerUid(uid)) return true;
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkPermission(sModifyAudioSettingsPrivileged, pid, uid);
    if (!ok) ALOGE("%s(): android.permission.MODIFY_AUDIO_SETTINGS_PRIVILEGED denied for uid %d",
                   __func__, uid);
    return ok;
}

bool dumpAllowed() {
    static const String16 sDump("android.permission.DUMP");
    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkCallingPermission(sDump);
    // convention is for caller to dump an error message to fd instead of logging here
    //if (!ok) ALOGE("Request requires android.permission.DUMP");
    return ok;
}

bool modifyPhoneStateAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    bool ok = PermissionCache::checkPermission(sModifyPhoneState, pid, uid);
    ALOGE_IF(!ok, "Request requires %s", String8(sModifyPhoneState).c_str());
    return ok;
}

// privileged behavior needed by Dialer, Settings, SetupWizard and CellBroadcastReceiver
bool bypassInterruptionPolicyAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));
    static const String16 sWriteSecureSettings("android.permission.WRITE_SECURE_SETTINGS");
    bool ok = PermissionCache::checkPermission(sModifyPhoneState, pid, uid)
        || PermissionCache::checkPermission(sWriteSecureSettings, pid, uid)
        || PermissionCache::checkPermission(sModifyAudioRouting, pid, uid);
    ALOGE_IF(!ok, "Request requires %s or %s",
             String8(sModifyPhoneState).c_str(), String8(sWriteSecureSettings).c_str());
    return ok;
}

bool callAudioInterceptionAllowed(const AttributionSourceState& attributionSource) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(attributionSource.pid));

    // IMPORTANT: Use PermissionCache - not a runtime permission and may not change.
    bool ok = PermissionCache::checkPermission(sCallAudioInterception, pid, uid);
    if (!ok) ALOGV("%s(): android.permission.CALL_AUDIO_INTERCEPTION denied for uid %d",
        __func__, uid);
    return ok;
}

AttributionSourceState getCallingAttributionSource() {
    AttributionSourceState attributionSource = AttributionSourceState();
    attributionSource.pid = VALUE_OR_FATAL(legacy2aidl_pid_t_int32_t(
            IPCThreadState::self()->getCallingPid()));
    attributionSource.uid = VALUE_OR_FATAL(legacy2aidl_uid_t_int32_t(
            IPCThreadState::self()->getCallingUid()));
    attributionSource.token = sp<BBinder>::make();
  return attributionSource;
}

void purgePermissionCache() {
    PermissionCache::purgeCache();
}

status_t checkIMemory(const sp<IMemory>& iMemory)
{
    if (iMemory == 0) {
        ALOGE("%s check failed: NULL IMemory pointer", __FUNCTION__);
        return BAD_VALUE;
    }

    sp<IMemoryHeap> heap = iMemory->getMemory();
    if (heap == 0) {
        ALOGE("%s check failed: NULL heap pointer", __FUNCTION__);
        return BAD_VALUE;
    }

    off_t size = lseek(heap->getHeapID(), 0, SEEK_END);
    lseek(heap->getHeapID(), 0, SEEK_SET);

    if (iMemory->unsecurePointer() == NULL || size < (off_t)iMemory->size()) {
        ALOGE("%s check failed: pointer %p size %zu fd size %u",
              __FUNCTION__, iMemory->unsecurePointer(), iMemory->size(), (uint32_t)size);
        return BAD_VALUE;
    }

    return NO_ERROR;
}

// TODO(b/285588444), clean this up on main, but soak it for backporting purposes for now
namespace {
class BluetoothPermissionCache {
    static constexpr auto SYSPROP_NAME = "cache_key.system_server.package_info";
    const String16 BLUETOOTH_PERM {"android.permission.BLUETOOTH_CONNECT"};
    mutable std::mutex mLock;
    // Cached property conditionally defined, since only avail on bionic. On host, don't inval cache
#if defined(__BIONIC__)
    // Unlocked, but only accessed from mListenerThread
    base::CachedProperty mCachedProperty;
#endif
    // This thread is designed to never join/terminate, so no signal is fine
    const std::thread mListenerThread;
    GUARDED_BY(mLock)
    std::string mPropValue;
    GUARDED_BY(mLock)
    std::unordered_map<uid_t, bool> mCache;
    PermissionController mPc{};
public:
    BluetoothPermissionCache()
#if defined(__BIONIC__)
            : mCachedProperty{SYSPROP_NAME},
            mListenerThread([this]() mutable {
                    while (true) {
                        std::string newVal = mCachedProperty.WaitForChange() ?: "";
                        std::lock_guard l{mLock};
                        if (newVal != mPropValue) {
                            ALOGV("Bluetooth permission update");
                            mPropValue = newVal;
                            mCache.clear();
                        }
                    }
                })
#endif
            {}

    bool checkPermission(uid_t uid, pid_t pid) {
        std::lock_guard l{mLock};
        auto it = mCache.find(uid);
        if (it == mCache.end()) {
            it = mCache.insert({uid, mPc.checkPermission(BLUETOOTH_PERM, pid, uid)}).first;
        }
        return it->second;
    }
};

// Don't call this from locks, since it potentially calls up to system server!
// Check for non-app UIDs above this method!
bool checkBluetoothPermission(const AttributionSourceState& attr) {
    [[clang::no_destroy]]  static BluetoothPermissionCache impl{};
    return impl.checkPermission(attr.uid, attr.pid);
}
} // anonymous

/**
 * Determines if the MAC address in Bluetooth device descriptors returned by APIs of
 * a native audio service (audio flinger, audio policy) must be anonymized.
 * MAC addresses returned to system server or apps with BLUETOOTH_CONNECT permission
 * are not anonymized.
 *
 * @param attributionSource The attribution source of the calling app.
 * @param caller string identifying the caller for logging.
 * @return true if the MAC addresses must be anonymized, false otherwise.
 */
bool mustAnonymizeBluetoothAddressLegacy(
        const AttributionSourceState& attributionSource, const String16&) {
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(attributionSource.uid));
    bool res;
    switch(multiuser_get_app_id(uid)) {
        case AID_ROOT:
        case AID_SYSTEM:
        case AID_RADIO:
        case AID_BLUETOOTH:
        case AID_MEDIA:
        case AID_AUDIOSERVER:
            // Don't anonymize for privileged clients
            res = false;
            break;
        default:
            res = !checkBluetoothPermission(attributionSource);
            break;
    }
    ALOGV("%s uid: %d, result: %d", __func__, uid, res);
    return res;
}

/**
 * Modifies the passed MAC address string in place for consumption by unprivileged clients.
 * the string is assumed to have a valid MAC address format.
 * the anonymization must be kept in sync with toAnonymizedAddress() in BluetoothUtils.java
 *
 * @param address input/output the char string contining the MAC address to anonymize.
 */
void anonymizeBluetoothAddress(char *address) {
    if (address == nullptr || strlen(address) != strlen("AA:BB:CC:DD:EE:FF")) {
        return;
    }
    memcpy(address, "XX:XX:XX:XX", strlen("XX:XX:XX:XX"));
}

sp<content::pm::IPackageManagerNative> MediaPackageManager::retrievePackageManager() {
    const sp<IServiceManager> sm = defaultServiceManager();
    if (sm == nullptr) {
        ALOGW("%s: failed to retrieve defaultServiceManager", __func__);
        return nullptr;
    }
    sp<IBinder> packageManager = sm->checkService(String16(nativePackageManagerName));
    if (packageManager == nullptr) {
        ALOGW("%s: failed to retrieve native package manager", __func__);
        return nullptr;
    }
    return interface_cast<content::pm::IPackageManagerNative>(packageManager);
}

std::optional<bool> MediaPackageManager::doIsAllowed(uid_t uid) {
    if (mPackageManager == nullptr) {
        /** Can not fetch package manager at construction it may not yet be registered. */
        mPackageManager = retrievePackageManager();
        if (mPackageManager == nullptr) {
            ALOGW("%s: Playback capture is denied as package manager is not reachable", __func__);
            return std::nullopt;
        }
    }

    // Retrieve package names for the UID and transform to a std::vector<std::string>.
    Vector<String16> str16PackageNames;
    PermissionController{}.getPackagesForUid(uid, str16PackageNames);
    std::vector<std::string> packageNames;
    for (const auto& str16PackageName : str16PackageNames) {
        packageNames.emplace_back(String8(str16PackageName).c_str());
    }
    if (packageNames.empty()) {
        ALOGW("%s: Playback capture for uid %u is denied as no package name could be retrieved "
              "from the package manager.", __func__, uid);
        return std::nullopt;
    }
    std::vector<bool> isAllowed;
    auto status = mPackageManager->isAudioPlaybackCaptureAllowed(packageNames, &isAllowed);
    if (!status.isOk()) {
        ALOGW("%s: Playback capture is denied for uid %u as the manifest property could not be "
              "retrieved from the package manager: %s", __func__, uid, status.toString8().c_str());
        return std::nullopt;
    }
    if (packageNames.size() != isAllowed.size()) {
        ALOGW("%s: Playback capture is denied for uid %u as the package manager returned incoherent"
              " response size: %zu != %zu", __func__, uid, packageNames.size(), isAllowed.size());
        return std::nullopt;
    }

    // Zip together packageNames and isAllowed for debug logs
    Packages& packages = mDebugLog[uid];
    packages.resize(packageNames.size()); // Reuse all objects
    std::transform(begin(packageNames), end(packageNames), begin(isAllowed),
                   begin(packages), [] (auto& name, bool isAllowed) -> Package {
                       return {std::move(name), isAllowed};
                   });

    // Only allow playback record if all packages in this UID allow it
    bool playbackCaptureAllowed = std::all_of(begin(isAllowed), end(isAllowed),
                                                  [](bool b) { return b; });

    return playbackCaptureAllowed;
}

void MediaPackageManager::dump(int fd, int spaces) const {
    dprintf(fd, "%*sAllow playback capture log:\n", spaces, "");
    if (mPackageManager == nullptr) {
        dprintf(fd, "%*sNo package manager\n", spaces + 2, "");
    }
    dprintf(fd, "%*sPackage manager errors: %u\n", spaces + 2, "", mPackageManagerErrors);

    for (const auto& uidCache : mDebugLog) {
        for (const auto& package : std::get<Packages>(uidCache)) {
            dprintf(fd, "%*s- uid=%5u, allowPlaybackCapture=%s, packageName=%s\n", spaces + 2, "",
                    std::get<const uid_t>(uidCache),
                    package.playbackCaptureAllowed ? "true " : "false",
                    package.name.c_str());
        }
    }
}

namespace mediautils {

// How long we hold info before we re-fetch it (24 hours) if we found it previously.
static constexpr nsecs_t INFO_EXPIRATION_NS = 24 * 60 * 60 * NANOS_PER_SECOND;
// Maximum info records we retain before clearing everything.
static constexpr size_t INFO_CACHE_MAX = 1000;

// The original code is from MediaMetricsService.cpp.
std::shared_ptr<const UidInfo::Info> UidInfo::getCachedInfo(uid_t uid)
{
    std::shared_ptr<const UidInfo::Info> info;

    const nsecs_t now = systemTime(SYSTEM_TIME_REALTIME);
    {
        std::lock_guard _l(mLock);
        auto it = mInfoMap.find(uid);
        if (it != mInfoMap.end()) {
            info = it->second;
            ALOGV("%s: uid %d expiration %lld now %lld",
                    __func__, uid, (long long)info->expirationNs, (long long)now);
            if (info->expirationNs <= now) {
                // purge the stale entry and fall into re-fetching
                ALOGV("%s: entry for uid %d expired, now %lld",
                        __func__, uid, (long long)now);
                mInfoMap.erase(it);
                info.reset();  // force refetch
            }
        }
    }

    // if we did not find it in our map, look it up
    if (!info) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<content::pm::IPackageManagerNative> package_mgr;
        if (sm.get() == nullptr) {
            ALOGE("%s: Cannot find service manager", __func__);
        } else {
            sp<IBinder> binder = sm->getService(String16("package_native"));
            if (binder.get() == nullptr) {
                ALOGE("%s: Cannot find package_native", __func__);
            } else {
                package_mgr = interface_cast<content::pm::IPackageManagerNative>(binder);
            }
        }

        // find package name
        std::string pkg;
        if (package_mgr != nullptr) {
            std::vector<std::string> names;
            binder::Status status = package_mgr->getNamesForUids({(int)uid}, &names);
            if (!status.isOk()) {
                ALOGE("%s: getNamesForUids failed: %s",
                        __func__, status.exceptionMessage().c_str());
            } else {
                if (!names[0].empty()) {
                    pkg = names[0].c_str();
                }
            }
        }

        if (pkg.empty()) {
            struct passwd pw{}, *result;
            char buf[8192]; // extra buffer space - should exceed what is
                            // required in struct passwd_pw (tested),
                            // and even then this is only used in backup
                            // when the package manager is unavailable.
            if (getpwuid_r(uid, &pw, buf, sizeof(buf), &result) == 0
                    && result != nullptr
                    && result->pw_name != nullptr) {
                pkg = result->pw_name;
            }
        }

        // strip any leading "shared:" strings that came back
        if (pkg.compare(0, 7, "shared:") == 0) {
            pkg.erase(0, 7);
        }

        // determine how pkg was installed and the versionCode
        std::string installer;
        int64_t versionCode = 0;
        bool notFound = false;
        if (pkg.empty()) {
            pkg = std::to_string(uid); // not found
            notFound = true;
        } else if (strchr(pkg.c_str(), '.') == nullptr) {
            // not of form 'com.whatever...'; assume internal
            // so we don't need to look it up in package manager.
        } else if (strncmp(pkg.c_str(), "android.", 8) == 0) {
            // android.* packages are assumed fine
        } else if (package_mgr.get() != nullptr) {
            String16 pkgName16(pkg.c_str());
            binder::Status status = package_mgr->getInstallerForPackage(pkgName16, &installer);
            if (!status.isOk()) {
                ALOGE("%s: getInstallerForPackage failed: %s",
                        __func__, status.exceptionMessage().c_str());
            }

            // skip if we didn't get an installer
            if (status.isOk()) {
                status = package_mgr->getVersionCodeForPackage(pkgName16, &versionCode);
                if (!status.isOk()) {
                    ALOGE("%s: getVersionCodeForPackage failed: %s",
                            __func__, status.exceptionMessage().c_str());
                }
            }

            ALOGV("%s: package '%s' installed by '%s' versioncode %lld",
                    __func__, pkg.c_str(), installer.c_str(), (long long)versionCode);
        }

        // add it to the map, to save a subsequent lookup
        std::lock_guard _l(mLock);
        // first clear if we have too many cached elements.  This would be rare.
        if (mInfoMap.size() >= INFO_CACHE_MAX) mInfoMap.clear();

        info = std::make_shared<const UidInfo::Info>(
                uid,
                std::move(pkg),
                std::move(installer),
                versionCode,
                now + (notFound ? 0 : INFO_EXPIRATION_NS));
        ALOGV("%s: adding uid %d package '%s' expirationNs: %lld",
                __func__, uid, info->package.c_str(), (long long)info->expirationNs);
        mInfoMap[uid] = info;
    }
    return info;
}

/* static */
UidInfo& UidInfo::getUidInfo() {
    [[clang::no_destroy]] static UidInfo uidInfo;
    return uidInfo;
}

/* static */
std::shared_ptr<const UidInfo::Info> UidInfo::getInfo(uid_t uid) {
    return UidInfo::getUidInfo().getCachedInfo(uid);
}

} // namespace mediautils

} // namespace android
