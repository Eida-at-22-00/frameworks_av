/*
**
** Copyright (C) 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

//#define LOG_NDEBUG 0
#define LOG_TAG "Camera"
#include <utils/Log.h>
#include <utils/threads.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/IMemory.h>

#include <Camera.h>
#include <android/hardware/ICameraService.h>
#include <android/hardware/ICamera.h>
#include <gui/Surface.h>

namespace android {

Camera::Camera(int cameraId)
    : CameraBase(cameraId)
{
}

CameraTraits<Camera>::TCamConnectService CameraTraits<Camera>::fnConnectService =
        &::android::hardware::ICameraService::connect;

// construct a camera client from an existing camera remote
sp<Camera> Camera::create(const sp<::android::hardware::ICamera>& camera)
{
     ALOGV("create");
     if (camera == 0) {
         ALOGE("camera remote is a NULL pointer");
         return 0;
     }

    sp<Camera> c = new Camera(-1);
    if (camera->connect(c) == NO_ERROR) {
        c->mStatus = NO_ERROR;
        c->mCamera = camera;
        IInterface::asBinder(camera)->linkToDeath(c);
        return c;
    }
    return 0;
}

Camera::~Camera()
{
    // We don't need to call disconnect() here because if the CameraService
    // thinks we are the owner of the hardware, it will hold a (strong)
    // reference to us, and we can't possibly be here. We also don't want to
    // call disconnect() here if we are in the same process as mediaserver,
    // because we may be invoked by CameraService::Client::connect() and will
    // deadlock if we call any method of ICamera here.
}

sp<Camera> Camera::connect(int cameraId, int targetSdkVersion, int rotationOverride,
        bool forceSlowJpegMode, const AttributionSourceState& clientAttribution,
        int32_t devicePolicy)
{
    return CameraBaseT::connect(cameraId, targetSdkVersion, rotationOverride,
            forceSlowJpegMode, clientAttribution, devicePolicy);
}

status_t Camera::reconnect()
{
    ALOGV("reconnect");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->connect(this);
}

status_t Camera::lock()
{
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->lock();
}

status_t Camera::unlock()
{
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->unlock();
}

// pass the Surface to the camera service
status_t Camera::setPreviewTarget(const sp<SurfaceType>& target) {
    ALOGV("setPreviewTarget(%p)", target.get());
    sp<::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    ALOGD_IF(target == 0, "app passed NULL surface");
    return c->setPreviewTarget(target);
}

status_t Camera::setVideoTarget(const sp<SurfaceType>& target) {
    ALOGV("setVideoTarget(%p)", target.get());
    sp<::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    ALOGD_IF(target == 0, "app passed NULL video surface");
    return c->setVideoTarget(target);
}

// start preview mode
status_t Camera::startPreview()
{
    ALOGV("startPreview");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->startPreview();
}

status_t Camera::setVideoBufferMode(int32_t videoBufferMode)
{
    ALOGV("setVideoBufferMode: %d", videoBufferMode);
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->setVideoBufferMode(videoBufferMode);
}

// start recording mode, must call setPreviewTarget first
status_t Camera::startRecording()
{
    ALOGV("startRecording");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->startRecording();
}

// stop preview mode
void Camera::stopPreview()
{
    ALOGV("stopPreview");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return;
    c->stopPreview();
}

// stop recording mode
void Camera::stopRecording()
{
    ALOGV("stopRecording");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return;
    c->stopRecording();
}

// release a recording frame
void Camera::releaseRecordingFrame(const sp<IMemory>& mem)
{
    ALOGV("releaseRecordingFrame");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return;
    c->releaseRecordingFrame(mem);
}

void Camera::releaseRecordingFrameHandle(native_handle_t* handle)
{
    ALOGV("releaseRecordingFrameHandle");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return;
    c->releaseRecordingFrameHandle(handle);
}

void Camera::releaseRecordingFrameHandleBatch(
        const std::vector<native_handle_t*> handles) {
    ALOGV("releaseRecordingFrameHandleBatch");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return;
    c->releaseRecordingFrameHandleBatch(handles);
}

// get preview state
bool Camera::previewEnabled()
{
    ALOGV("previewEnabled");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return false;
    return c->previewEnabled();
}

// get recording state
bool Camera::recordingEnabled()
{
    ALOGV("recordingEnabled");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return false;
    return c->recordingEnabled();
}

status_t Camera::autoFocus()
{
    ALOGV("autoFocus");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->autoFocus();
}

status_t Camera::cancelAutoFocus()
{
    ALOGV("cancelAutoFocus");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->cancelAutoFocus();
}

// take a picture
status_t Camera::takePicture(int msgType)
{
    ALOGV("takePicture: 0x%x", msgType);
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->takePicture(msgType);
}

// set preview/capture parameters - key/value pairs
status_t Camera::setParameters(const String8& params)
{
    ALOGV("setParameters");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->setParameters(params);
}

// get preview/capture parameters - key/value pairs
String8 Camera::getParameters() const
{
    ALOGV("getParameters");
    String8 params;
    sp <::android::hardware::ICamera> c = mCamera;
    if (c != 0) params = c->getParameters();
    return params;
}

// send command to camera driver
status_t Camera::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2)
{
    ALOGV("sendCommand");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->sendCommand(cmd, arg1, arg2);
}

void Camera::setListener(const sp<CameraListener>& listener)
{
    Mutex::Autolock _l(mLock);
    mListener = listener;
}

void Camera::setPreviewCallbackFlags(int flag)
{
    ALOGV("setPreviewCallbackFlags");
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return;
    c->setPreviewCallbackFlag(flag);
}

status_t Camera::setPreviewCallbackTarget(const sp<SurfaceType>& target) {
    sp<::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->setPreviewCallbackTarget(target);
}

status_t Camera::setAudioRestriction(int32_t mode)
{
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->setAudioRestriction(mode);
}

int32_t Camera::getGlobalAudioRestriction()
{
    sp <::android::hardware::ICamera> c = mCamera;
    if (c == 0) return NO_INIT;
    return c->getGlobalAudioRestriction();
}

// callback from camera service
void Camera::notifyCallback(int32_t msgType, int32_t ext1, int32_t ext2)
{
    return CameraBaseT::notifyCallback(msgType, ext1, ext2);
}

// callback from camera service when frame or image is ready
void Camera::dataCallback(int32_t msgType, const sp<IMemory>& dataPtr,
                          camera_frame_metadata_t *metadata)
{
    sp<CameraListener> listener;
    {
        Mutex::Autolock _l(mLock);
        listener = mListener;
    }
    if (listener != NULL) {
        listener->postData(msgType, dataPtr, metadata);
    }
}

// callback from camera service when timestamped frame is ready
void Camera::dataCallbackTimestamp(nsecs_t timestamp, int32_t msgType, const sp<IMemory>& dataPtr)
{
    sp<CameraListener> listener;
    {
        Mutex::Autolock _l(mLock);
        listener = mListener;
    }

    if (listener != NULL) {
        listener->postDataTimestamp(timestamp, msgType, dataPtr);
    } else {
        ALOGW("No listener was set. Drop a recording frame.");
        releaseRecordingFrame(dataPtr);
    }
}

void Camera::recordingFrameHandleCallbackTimestamp(nsecs_t timestamp, native_handle_t* handle)
{
    sp<CameraListener> listener;
    {
        Mutex::Autolock _l(mLock);
        listener = mListener;
    }

    if (listener != NULL) {
        listener->postRecordingFrameHandleTimestamp(timestamp, handle);
    } else {
        ALOGW("No listener was set. Drop a recording frame.");
        releaseRecordingFrameHandle(handle);
    }
}

void Camera::recordingFrameHandleCallbackTimestampBatch(
        const std::vector<nsecs_t>& timestamps,
        const std::vector<native_handle_t*>& handles)
{
    sp<CameraListener> listener;
    {
        Mutex::Autolock _l(mLock);
        listener = mListener;
    }

    if (listener != NULL) {
        listener->postRecordingFrameHandleTimestampBatch(timestamps, handles);
    } else {
        ALOGW("No listener was set. Drop a batch of recording frames.");
        releaseRecordingFrameHandleBatch(handles);
    }
}

sp<ICameraRecordingProxy> Camera::getRecordingProxy() {
    ALOGV("getProxy");
    return new RecordingProxy(this);
}

status_t Camera::RecordingProxy::startRecording()
{
    ALOGV("RecordingProxy::startRecording");
    mCamera->reconnect();
    return mCamera->startRecording();
}

void Camera::RecordingProxy::stopRecording()
{
    ALOGV("RecordingProxy::stopRecording");
    mCamera->stopRecording();
}

Camera::RecordingProxy::RecordingProxy(const sp<Camera>& camera)
{
    mCamera = camera;
}

}; // namespace android
