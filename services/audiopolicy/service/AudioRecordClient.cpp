/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <android/content/pm/IPackageManagerNative.h>

#include "AudioRecordClient.h"
#include "AudioPolicyService.h"
#include "binder/AppOpsManager.h"
#include "mediautils/ServiceUtilities.h"
#include <android_media_audiopolicy.h>
#include <media/AttrSourceIter.h>

#include <algorithm>

namespace android::media::audiopolicy {
namespace audiopolicy_flags = android::media::audiopolicy;
using android::AudioPolicyService;

namespace {
bool isAppOpSource(audio_source_t source) {
    switch (source) {
        case AUDIO_SOURCE_FM_TUNER:
        case AUDIO_SOURCE_ECHO_REFERENCE:
        case AUDIO_SOURCE_REMOTE_SUBMIX:
            return false;
        default:
            break;
    }
    return true;
}

int getTargetSdkForPackageName(std::string_view packageName) {
    const auto binder = defaultServiceManager()->checkService(String16{"package_native"});
    int targetSdk = -1;
    if (binder != nullptr) {
        const auto pm = interface_cast<content::pm::IPackageManagerNative>(binder);
        if (pm != nullptr) {
            const auto status = pm->getTargetSdkVersionForPackage(
                    String16{packageName.data(), packageName.size()}, &targetSdk);
            return status.isOk() ? targetSdk : __ANDROID_API_FUTURE__;
        }
    }
    return targetSdk;
}

bool doesPackageTargetAtLeastU(std::string_view packageName) {
    return getTargetSdkForPackageName(packageName) >= __ANDROID_API_U__;
}
} // anonymous

// static
sp<OpRecordAudioMonitor>
OpRecordAudioMonitor::createIfNeeded(
        const AttributionSourceState &attributionSource,
        const uint32_t virtualDeviceId,
        const audio_attributes_t &attr,
        wp<AudioPolicyService::AudioCommandThread> commandThread)
{
    if (isAudioServerOrRootUid(attributionSource.uid)) {
        ALOGV("not silencing record for audio or root source %s",
                attributionSource.toString().c_str());
        return nullptr;
    }

    if (!isAppOpSource(attr.source)) {
        ALOGD("not monitoring app op for uid %d and source %d",
                attributionSource.uid, attr.source);
        return nullptr;
    }

    if (!attributionSource.packageName.has_value()
            || attributionSource.packageName.value().size() == 0) {
        return nullptr;
    }

    return new OpRecordAudioMonitor(attributionSource, virtualDeviceId, attr,
                                    getOpForSource(attr.source),
                                    isRecordOpRequired(attr.source),
                                    commandThread);
}

// The vdi is carried in the attribution source for appops perm checks.
// Overwrite the entire chain with the vdi associated with the mix this client is attached to
// This ensures the checkOps triggered by the listener are correct.
// Note: we still only register for events by package name, so we assume that we get events
// independent of vdi.
static AttributionSourceState& overwriteVdi(AttributionSourceState& chain, int vdi) {
    using permission::AttrSourceIter::begin;
    using permission::AttrSourceIter::end;
    if (vdi != 0 /* default vdi */) {
        std::for_each(begin(chain), end(), [vdi](auto& attr) { attr.deviceId = vdi; });
    }
    return chain;
}

OpRecordAudioMonitor::OpRecordAudioMonitor(
        AttributionSourceState attributionSource,
        const uint32_t virtualDeviceId, const audio_attributes_t &attr,
        int32_t appOp,
        bool shouldMonitorRecord,
        wp<AudioPolicyService::AudioCommandThread> commandThread) :
        mHasOp(true),
        mAttributionSource(overwriteVdi(attributionSource, virtualDeviceId)),
        mVirtualDeviceId(virtualDeviceId), mAttr(attr),
        mAppOp(appOp),
        mShouldMonitorRecord(shouldMonitorRecord),
        mCommandThread(commandThread) {}

OpRecordAudioMonitor::~OpRecordAudioMonitor()
{
    if (mOpCallback != 0) {
        mAppOpsManager.stopWatchingMode(mOpCallback);
    }
    mOpCallback.clear();
}

void OpRecordAudioMonitor::onFirstRef()
{
    using permission::AttrSourceIter::cbegin;
    using permission::AttrSourceIter::cend;

    checkOp();
    mOpCallback = new RecordAudioOpCallback(this);
    ALOGV("start watching op %d for %s", mAppOp, mAttributionSource.toString().c_str());

    int flags = doesPackageTargetAtLeastU(mAttributionSource.packageName.value_or(""))
                        ? AppOpsManager::WATCH_FOREGROUND_CHANGES
                        : 0;

    const auto reg = [&](int32_t op) {
        std::for_each(cbegin(mAttributionSource), cend(),
                      [&](const auto& attr) {
                          mAppOpsManager.startWatchingMode(
                                  op,
                                  VALUE_OR_FATAL(aidl2legacy_string_view_String16(
                                          attr.packageName.value_or(""))),
                                  flags, mOpCallback);
                      });
    };
    reg(mAppOp);
    if (mAppOp != AppOpsManager::OP_RECORD_AUDIO && mShouldMonitorRecord) {
        reg(AppOpsManager::OP_RECORD_AUDIO);
    }
}

bool OpRecordAudioMonitor::hasOp() const {
    return mHasOp.load();
}

// Called by RecordAudioOpCallback when the app op corresponding to this OpRecordAudioMonitor
// is updated in AppOp callback and in onFirstRef()
// Note this method is never called (and never to be) for audio server / root track
// due to the UID in createIfNeeded(). As a result for those record track, it's:
// - not called from constructor,
// - not called from RecordAudioOpCallback because the callback is not installed in this case
void OpRecordAudioMonitor::checkOp(bool updateUidStates) {
    using permission::AttrSourceIter::cbegin;
    using permission::AttrSourceIter::cend;

    const auto check = [&](int32_t op) -> bool {
        return std::all_of(cbegin(mAttributionSource), cend(), [&](const auto& x) {
                    return mAppOpsManager.checkOp(op, x.uid,
                                                  VALUE_OR_FATAL(aidl2legacy_string_view_String16(
                                                          x.packageName.value_or("")))) ==
                           AppOpsManager::MODE_ALLOWED;
                });
    };
    bool hasIt = check(mAppOp);
    if (mAppOp != AppOpsManager::OP_RECORD_AUDIO && mShouldMonitorRecord) {
        hasIt = hasIt && check(AppOpsManager::OP_RECORD_AUDIO);
    }

    if (audiopolicy_flags::record_audio_device_aware_permission()) {
        const bool canRecord = recordingAllowed(mAttributionSource, mVirtualDeviceId, mAttr.source);
        hasIt = hasIt && canRecord;
    }
    // verbose logging only log when appOp changed
    ALOGI_IF(hasIt != mHasOp.load(),
            "App op %d missing, %ssilencing record %s",
            mAppOp, hasIt ? "un" : "", mAttributionSource.toString().c_str());
    mHasOp.store(hasIt);

    if (updateUidStates) {
          sp<AudioPolicyService::AudioCommandThread> commandThread = mCommandThread.promote();
          if (commandThread != nullptr) {
              commandThread->updateUidStatesCommand();
          }
    }
}

OpRecordAudioMonitor::RecordAudioOpCallback::RecordAudioOpCallback(
        const wp<OpRecordAudioMonitor>& monitor) : mMonitor(monitor)
{ }

binder::Status OpRecordAudioMonitor::RecordAudioOpCallback::opChanged(int32_t op, int32_t,
            const String16&, const String16&) {
    sp<OpRecordAudioMonitor> monitor = mMonitor.promote();
    if (monitor != NULL) {
        if (op != monitor->getOp()) {
            return binder::Status::ok();
        }
        monitor->checkOp(true);
    }
    return binder::Status::ok();
}

}  // namespace android::media::audiopolicy
