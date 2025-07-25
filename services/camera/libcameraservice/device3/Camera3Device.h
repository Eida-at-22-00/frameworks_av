/*
 * Copyright (C) 2013-2018 The Android Open Source Project
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

#ifndef ANDROID_SERVERS_CAMERA3DEVICE_H
#define ANDROID_SERVERS_CAMERA3DEVICE_H

#include <utility>
#include <unordered_map>
#include <set>
#include <tuple>

#include <utils/Condition.h>
#include <utils/Errors.h>
#include <utils/List.h>
#include <utils/Mutex.h>
#include <utils/Thread.h>
#include <utils/KeyedVector.h>
#include <utils/Timers.h>

#include <camera/CaptureResult.h>
#include <gui/Flags.h>

#include "CameraServiceWatchdog.h"
#include <aidl/android/hardware/camera/device/CameraBlob.h>

#include "common/CameraDeviceBase.h"
#include "common/DepthPhotoProcessor.h"
#include "common/FrameProcessorBase.h"
#include "device3/BufferUtils.h"
#include "device3/StatusTracker.h"
#include "device3/Camera3BufferManager.h"
#include "device3/DistortionMapper.h"
#include "device3/ZoomRatioMapper.h"
#include "device3/RotateAndCropMapper.h"
#include "device3/UHRCropAndMeteringRegionMapper.h"
#include "device3/InFlightRequest.h"
#include "device3/Camera3OutputInterface.h"
#include "device3/Camera3OfflineSession.h"
#include "device3/Camera3StreamInterface.h"
#include "utils/AttributionAndPermissionUtils.h"
#include "utils/TagMonitor.h"
#include "utils/IPCTransport.h"
#include "utils/LatencyHistogram.h"
#include "utils/CameraServiceProxyWrapper.h"
#include <camera_metadata_hidden.h>

using android::camera3::camera_capture_request_t;
using android::camera3::camera_request_template;
using android::camera3::camera_stream_buffer_t;
using android::camera3::camera_stream_configuration_t;
using android::camera3::camera_stream_configuration_mode_t;
using android::camera3::CAMERA_TEMPLATE_COUNT;
using android::camera3::OutputStreamInfo;
using android::camera3::SurfaceHolder;

namespace android {

namespace camera3 {

class Camera3Stream;
class Camera3ZslStream;
class Camera3StreamInterface;

} // namespace camera3

/**
 * CameraDevice for HAL devices with version CAMERA_DEVICE_API_VERSION_3_0 or higher.
 */
class Camera3Device :
            public CameraDeviceBase,
            public camera3::SetErrorInterface,
            public camera3::InflightRequestUpdateInterface,
            public camera3::RequestBufferInterface,
            public camera3::FlushBufferInterface,
            public AttributionAndPermissionUtilsEncapsulator {
  friend class HidlCamera3Device;
  friend class AidlCamera3Device;
  public:

    explicit Camera3Device(std::shared_ptr<CameraServiceProxyWrapper>& cameraServiceProxyWrapper,
            std::shared_ptr<AttributionAndPermissionUtils> attributionAndPermissionUtils,
            const std::string& id, bool overrideForPerfClass, int rotationOverride,
            bool isVendorClient, bool legacyClient = false);

    virtual ~Camera3Device();
    // Delete and optionally close native handles and clear the input vector afterward
    static void cleanupNativeHandles(
            std::vector<native_handle_t*> *handles, bool closeFd = false);

    virtual IPCTransport getTransportType() const override {
        return mInterface->getTransportType();
    }

    bool isHalBufferManagedStream(int32_t streamId) const {
        return mInterface->isHalBufferManagedStream(streamId);
    };

    /**
     * CameraDeviceBase interface
     */

    const std::string& getId() const override;

    metadata_vendor_id_t getVendorTagId() const override { return mVendorTagId; }

    // Watchdog thread
    sp<CameraServiceWatchdog> mCameraServiceWatchdog;

    // Transitions to idle state on success.
    virtual status_t initialize(sp<CameraProviderManager> /*manager*/,
            const std::string& /*monitorTags*/) = 0;

    static constexpr int32_t METADATA_QUEUE_SIZE = 1 << 20;

    template <typename FMQType>
    static size_t calculateFMQSize(const std::unique_ptr<FMQType> &fmq) {
        if (fmq == nullptr) {
            ALOGE("%s: result metadata queue hasn't been initialized", __FUNCTION__);
            return METADATA_QUEUE_SIZE;
        }
        size_t quantumSize = fmq->getQuantumSize();
        size_t quantumCount = fmq->getQuantumCount();
        if ((quantumSize == 0) || (quantumCount == 0) ||
                ((std::numeric_limits<size_t>::max() / quantumSize) < quantumCount)) {
            ALOGE("%s: Error with FMQ quantum count / quantum size, quantum count %zu"
                    "quantum count %zu", __FUNCTION__, quantumSize, quantumCount);
            return METADATA_QUEUE_SIZE;
        }
        return fmq->getQuantumSize() * fmq->getQuantumCount();
    }
    status_t disconnect() override;
    status_t dump(int fd, const Vector<String16> &args) override;
    status_t startWatchingTags(const std::string &tags) override;
    status_t stopWatchingTags() override;
    status_t dumpWatchedEventsToVector(std::vector<std::string> &out) override;
    const CameraMetadata& info() const override;
    const CameraMetadata& infoPhysical(const std::string& physicalId) const override;
    bool isCompositeJpegRDisabled() const override { return mIsCompositeJpegRDisabled; };
    bool isCompositeHeicDisabled() const override { return mIsCompositeHeicDisabled; }
    bool isCompositeHeicUltraHDRDisabled() const override {
        return mIsCompositeHeicUltraHDRDisabled;
    }

    // Capture and setStreamingRequest will configure streams if currently in
    // idle state
    status_t capture(CameraMetadata &request, int64_t *lastFrameNumber = NULL) override;
    status_t captureList(const List<const PhysicalCameraSettingsList> &requestsList,
            const std::list<SurfaceMap> &surfaceMaps,
            int64_t *lastFrameNumber = NULL) override;
    status_t setStreamingRequest(const CameraMetadata &request,
            int64_t *lastFrameNumber = NULL) override;
    status_t setStreamingRequestList(const List<const PhysicalCameraSettingsList> &requestsList,
            const std::list<SurfaceMap> &surfaceMaps,
            int64_t *lastFrameNumber = NULL) override;
    status_t clearStreamingRequest(int64_t *lastFrameNumber = NULL) override;

    status_t waitUntilRequestReceived(int32_t requestId, nsecs_t timeout) override;

    // Actual stream creation/deletion is delayed until first request is submitted
    // If adding streams while actively capturing, will pause device before adding
    // stream, reconfiguring device, and unpausing. If the client create a stream
    // with nullptr consumer surface, the client must then call setConsumers()
    // and finish the stream configuration before starting output streaming.
    status_t createStream(sp<Surface> consumer,
            uint32_t width, uint32_t height, int format,
            android_dataspace dataSpace, camera_stream_rotation_t rotation, int *id,
            const std::string& physicalCameraId,
            const std::unordered_set<int32_t> &sensorPixelModesUsed,
            std::vector<int> *surfaceIds = nullptr,
            int streamSetId = camera3::CAMERA3_STREAM_SET_ID_INVALID,
            bool isShared = false, bool isMultiResolution = false,
            uint64_t consumerUsage = 0,
            int64_t dynamicRangeProfile =
            ANDROID_REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_STANDARD,
            int64_t streamUseCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT,
            int timestampBase = OutputConfiguration::TIMESTAMP_BASE_DEFAULT,
            int mirrorMode = OutputConfiguration::MIRROR_MODE_AUTO,
            int32_t colorSpace = ANDROID_REQUEST_AVAILABLE_COLOR_SPACE_PROFILES_MAP_UNSPECIFIED,
            bool useReadoutTimestamp = false)
            override;

    status_t createStream(const std::vector<SurfaceHolder>& consumers,
            bool hasDeferredConsumer, uint32_t width, uint32_t height, int format,
            android_dataspace dataSpace, camera_stream_rotation_t rotation, int *id,
            const std::string& physicalCameraId,
            const std::unordered_set<int32_t> &sensorPixelModesUsed,
            std::vector<int> *surfaceIds = nullptr,
            int streamSetId = camera3::CAMERA3_STREAM_SET_ID_INVALID,
            bool isShared = false, bool isMultiResolution = false,
            uint64_t consumerUsage = 0,
            int64_t dynamicRangeProfile =
            ANDROID_REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_STANDARD,
            int64_t streamUseCase = ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT,
            int timestampBase = OutputConfiguration::TIMESTAMP_BASE_DEFAULT,
            int32_t colorSpace = ANDROID_REQUEST_AVAILABLE_COLOR_SPACE_PROFILES_MAP_UNSPECIFIED,
            bool useReadoutTimestamp = false)
            override;

    status_t createInputStream(
            uint32_t width, uint32_t height, int format, bool isMultiResolution,
            int *id) override;

    status_t getStreamInfo(int id, StreamInfo *streamInfo) override;
    status_t setStreamTransform(int id, int transform) override;

    status_t deleteStream(int id) override;

    virtual status_t beginConfigure() override {return OK;};

    virtual status_t getSharedStreamId(const OutputStreamInfo& /*config*/,
            int* /*streamId*/) override {return INVALID_OPERATION;};

    virtual status_t addSharedSurfaces(int /*streamId*/,
            const std::vector<android::camera3::OutputStreamInfo>& /*outputInfo*/,
            const std::vector<SurfaceHolder>& /*surfaces*/,
            std::vector<int>* /*surfaceIds*/) override {return INVALID_OPERATION;};

    virtual status_t removeSharedSurfaces(int /*streamId*/,
            const std::vector<size_t>& /*surfaceIds*/) override {return INVALID_OPERATION;};

    virtual status_t setSharedStreamingRequest(
            const PhysicalCameraSettingsList& /*request*/, const SurfaceMap& /*surfaceMap*/,
            int32_t* /*sharedReqID*/, int64_t* /*lastFrameNumber = NULL*/) override {
        return INVALID_OPERATION;
    };

    virtual status_t clearSharedStreamingRequest(int64_t* /*lastFrameNumber = NULL*/) override {
        return INVALID_OPERATION;
    };

    virtual status_t setSharedCaptureRequest(const PhysicalCameraSettingsList& /*request*/,
            const SurfaceMap& /*surfaceMap*/, int32_t* /*sharedReqID*/,
            int64_t* /*lastFrameNumber = NULL*/) override {return INVALID_OPERATION;};

    virtual sp<camera2::FrameProcessorBase> getSharedFrameProcessor() override {return nullptr;};

    virtual status_t startStreaming(const int32_t /*reqId*/, const SurfaceMap& /*surfaceMap*/,
            int32_t* /*sharedReqID*/, int64_t* /*lastFrameNumber = NULL*/)
            override {return INVALID_OPERATION;};
    status_t configureStreams(const CameraMetadata& sessionParams,
            int operatingMode =
            camera_stream_configuration_mode_t::CAMERA_STREAM_CONFIGURATION_NORMAL_MODE) override;
#if WB_CAMERA3_AND_PROCESSORS_WITH_DEPENDENCIES
    status_t getInputSurface(sp<Surface> *surface) override;
#else
    status_t getInputBufferProducer(
            sp<IGraphicBufferProducer> *producer) override;
#endif

    void getOfflineStreamIds(std::vector<int> *offlineStreamIds) override;

    status_t createDefaultRequest(camera_request_template_t templateId,
            CameraMetadata *request) override;

    // Transitions to the idle state on success
    status_t waitUntilDrained() override;

    virtual status_t setNotifyCallback(wp<NotificationListener> listener) override;
    bool     willNotify3A() override;
    status_t waitForNextFrame(nsecs_t timeout) override;
    status_t getNextResult(CaptureResult *frame) override;

    status_t triggerAutofocus(uint32_t id) override;
    status_t triggerCancelAutofocus(uint32_t id) override;
    status_t triggerPrecaptureMetering(uint32_t id) override;

    status_t flush(int64_t *lastFrameNumber = NULL) override;

    status_t prepare(int streamId) override;

    status_t tearDown(int streamId) override;

    status_t addBufferListenerForStream(int streamId,
            wp<camera3::Camera3StreamBufferListener> listener) override;

    status_t prepare(int maxCount, int streamId) override;

    ssize_t getJpegBufferSize(const CameraMetadata &info, uint32_t width,
            uint32_t height) const override;
    ssize_t getPointCloudBufferSize(const CameraMetadata &info) const;
    ssize_t getRawOpaqueBufferSize(const CameraMetadata &info, int32_t width, int32_t height,
            bool maxResolution) const;

    // Methods called by subclasses
    void             notifyStatus(bool idle); // updates from StatusTracker

    /**
     * Set the deferred consumer surfaces to the output stream and finish the deferred
     * consumer configuration.
     */
    status_t setConsumerSurfaces(
            int streamId, const std::vector<SurfaceHolder>& consumers,
            std::vector<int> *surfaceIds /*out*/) override;

    /**
     * Update a given stream.
     */
    status_t updateStream(int streamId, const std::vector<SurfaceHolder> &newSurfaces,
            const std::vector<OutputStreamInfo> &outputInfo,
            const std::vector<size_t> &removedSurfaceIds,
            KeyedVector<sp<Surface>, size_t> *outputMap/*out*/);

    /**
     * Drop buffers for stream of streamId if dropping is true. If dropping is false, do not
     * drop buffers for stream of streamId.
     */
    status_t dropStreamBuffers(bool dropping, int streamId) override;

    nsecs_t getExpectedInFlightDuration() override;

    virtual status_t switchToOffline(const std::vector<int32_t>& ,
            /*out*/ sp<CameraOfflineSessionBase>* )  override {
      return INVALID_OPERATION;
    };

    // RequestBufferInterface
    bool startRequestBuffer() override;
    void endRequestBuffer() override;
    nsecs_t getWaitDuration() override;

    // FlushBufferInterface
    void getInflightBufferKeys(std::vector<std::pair<int32_t, int32_t>>* out) override;
    void getInflightRequestBufferKeys(std::vector<uint64_t>* out) override;
    std::vector<sp<camera3::Camera3StreamInterface>> getAllStreams() override;

    /**
     * Set the current behavior for the ROTATE_AND_CROP control when in AUTO.
     *
     * The value must be one of the ROTATE_AND_CROP_* values besides AUTO,
     * and defaults to NONE.
     */
    status_t setRotateAndCropAutoBehavior(
            camera_metadata_enum_android_scaler_rotate_and_crop_t rotateAndCropValue, bool fromHal);

    /**
     * Set the current behavior for the AUTOFRAMING control when in AUTO.
     *
     * The value must be one of the AUTOFRAMING_* values besides AUTO.
     */
    status_t setAutoframingAutoBehavior(
            camera_metadata_enum_android_control_autoframing_t autoframingValue);

    /**
     * Whether camera muting (producing black-only output) is supported.
     *
     * Calling setCameraMute(true) when this returns false will return an
     * INVALID_OPERATION error.
     */
    bool supportsCameraMute();

    /**
     * Mute the camera.
     *
     * When muted, black image data is output on all output streams.
     */
    status_t setCameraMute(bool enabled);

    /**
     * Mute the camera.
     *
     * When muted, black image data is output on all output streams.
     * This method assumes the caller already acquired the 'mInterfaceLock'
     * and 'mLock' locks.
     */
    status_t setCameraMuteLocked(bool enabled);

    /**
     * Enables/disables camera service watchdog
     */
    status_t setCameraServiceWatchdog(bool enabled);

    // Set stream use case overrides
    void setStreamUseCaseOverrides(
            const std::vector<int64_t>& useCaseOverrides);

    // Clear stream use case overrides
    void clearStreamUseCaseOverrides();

    /**
     * Whether the camera device supports zoom override.
     */
    bool supportsZoomOverride();

    // Set/reset zoom override
    status_t setZoomOverride(int32_t zoomOverride);

    // Get the status trackeer for the camera device
    wp<camera3::StatusTracker> getStatusTracker() { return mStatusTracker; }

    // Whether the device is in error state
    bool hasDeviceError();

    /**
     * The injection camera session to replace the internal camera
     * session.
     */
    status_t injectCamera(const std::string& injectedCamId,
                          sp<CameraProviderManager> manager);

    /**
     * Stop the injection camera and restore to internal camera session.
     */
    status_t stopInjection();

    /**
     * Inject session params into the current client.
     */
    status_t injectSessionParams(const CameraMetadata& sessionParams);

  protected:
    status_t disconnectImpl();
    static status_t removeFwkOnlyKeys(CameraMetadata *request);

    float getMaxPreviewFps(sp<camera3::Camera3OutputStreamInterface> stream);

    static const size_t        kDumpLockAttempts  = 10;
    static const size_t        kDumpSleepDuration = 100000; // 0.10 sec
    static const nsecs_t       kActiveTimeout     = 500000000;  // 500 ms
    static const nsecs_t       kMinWarnInflightDuration = 5000000000; // 5 s
    static const size_t        kInFlightWarnLimit = 30;
    static const size_t        kInFlightWarnLimitHighSpeed = 256; // batch size 32 * pipe depth 8
    static const nsecs_t       kMinInflightDuration = 5000000000; // 5 s
    static const nsecs_t       kBaseGetBufferWait = 3000000000; // 3 sec.

    struct                     RequestTrigger;
    // minimal jpeg buffer size: 256KB + blob header
    static const ssize_t       kMinJpegBufferSize = camera3::MIN_JPEG_BUFFER_SIZE +
            sizeof(aidl::android::hardware::camera::device::CameraBlob);
    // Constant to use for stream ID when one doesn't exist
    static const int           NO_STREAM = -1;

    std::shared_ptr<CameraServiceProxyWrapper> mCameraServiceProxyWrapper;

    // A lock to enforce serialization on the input/configure side
    // of the public interface.
    // Only locked by public methods inherited from CameraDeviceBase.
    // Not locked by methods guarded by mOutputLock, since they may act
    // concurrently to the input/configure side of the interface.
    // Must be locked before mLock if both will be locked by a method
    Mutex                      mInterfaceLock;

    // The main lock on internal state
    Mutex                      mLock;

    // Camera device ID
    const std::string          mId;

    // Legacy camera client flag
    bool                       mLegacyClient;

    // Current stream configuration mode;
    int                        mOperatingMode;
    // Current session wide parameters
    hardware::camera2::impl::CameraMetadataNative mSessionParams;
    // Constant to use for no set operating mode
    static const int           NO_MODE = -1;

    // Flag indicating is the current active stream configuration is constrained high speed.
    bool                       mIsConstrainedHighSpeedConfiguration;

    /**** Scope for mLock ****/

    class HalInterface : public camera3::Camera3StreamBufferFreedListener,
            public camera3::BufferRecordsInterface {
      public:
        HalInterface(bool useHalBufManager, bool supportOfflineProcessing) :
              mUseHalBufManager(useHalBufManager),
              mIsReconfigurationQuerySupported(true),
              mSupportOfflineProcessing(supportOfflineProcessing)
               {};
        HalInterface(const HalInterface &other);
        HalInterface();

        virtual IPCTransport getTransportType() const = 0;

        // Returns true if constructed with a valid device or session, and not yet cleared
        virtual bool valid() = 0;

        // Reset this HalInterface object (does not call close())
        virtual void clear() = 0;

        // Calls into the HAL interface

        // Caller takes ownership of requestTemplate
        virtual status_t constructDefaultRequestSettings(camera_request_template templateId,
                /*out*/ camera_metadata_t **requestTemplate) = 0;

        virtual status_t configureStreams(const camera_metadata_t * sessionParams,
                /*inout*/ camera_stream_configuration_t * config,
                const std::vector<uint32_t>& bufferSizes, int64_t logId) = 0;

        // The injection camera configures the streams to hal.
        virtual status_t configureInjectedStreams(
                const camera_metadata_t* sessionParams,
                /*inout*/ camera_stream_configuration_t* config,
                const std::vector<uint32_t>& bufferSizes,
                const CameraMetadata& cameraCharacteristics) = 0;

        // When the call succeeds, the ownership of acquire fences in requests is transferred to
        // HalInterface. More specifically, the current implementation will send the fence to
        // HAL process and close the FD in cameraserver process. When the call fails, the ownership
        // of the acquire fence still belongs to the caller.
        virtual status_t processBatchCaptureRequests(
                std::vector<camera_capture_request_t*>& requests,
                /*out*/uint32_t* numRequestProcessed) = 0;

        virtual status_t flush() = 0;

        virtual status_t dump(int fd) = 0;

        virtual status_t close() = 0;

        virtual void signalPipelineDrain(const std::vector<int>& streamIds) = 0;

        virtual bool isReconfigurationRequired(CameraMetadata& oldSessionParams,
                CameraMetadata& newSessionParams) = 0;

        virtual status_t repeatingRequestEnd(uint32_t frameNumber,
                const std::vector<int32_t> &streamIds) = 0;

        /////////////////////////////////////////////////////////////////////
        // Implements BufferRecordsInterface

        std::pair<bool, uint64_t> getBufferId(
                const buffer_handle_t& buf, int streamId) override;

        uint64_t removeOneBufferCache(int streamId, const native_handle_t* handle) override;

        status_t popInflightBuffer(int32_t frameNumber, int32_t streamId,
                /*out*/ buffer_handle_t **buffer) override;

        status_t pushInflightRequestBuffer(
                uint64_t bufferId, buffer_handle_t* buf, int32_t streamId) override;

        status_t popInflightRequestBuffer(uint64_t bufferId,
                /*out*/ buffer_handle_t** buffer,
                /*optional out*/ int32_t* streamId = nullptr) override;

        /////////////////////////////////////////////////////////////////////

        //Check if a stream is hal buffer managed
        bool isHalBufferManagedStream(int32_t streamId) const;

        // Get a vector of (frameNumber, streamId) pair of currently inflight
        // buffers
        void getInflightBufferKeys(std::vector<std::pair<int32_t, int32_t>>* out);

        // Get a vector of bufferId of currently inflight buffers
        void getInflightRequestBufferKeys(std::vector<uint64_t>* out);

        void onStreamReConfigured(int streamId);

      protected:

        // Return true if the input caches match what we have; otherwise false
        bool verifyBufferIds(int32_t streamId, std::vector<uint64_t>& inBufIds);

        template <typename OfflineSessionInfoT>
        status_t verifyBufferCaches(
            const OfflineSessionInfoT *offlineSessionInfo, camera3::BufferRecords *bufferRecords) {
            // Validate buffer caches
            std::vector<int32_t> streams;
            streams.reserve(offlineSessionInfo->offlineStreams.size());
            for (auto offlineStream : offlineSessionInfo->offlineStreams) {
                int32_t id = offlineStream.id;
                streams.push_back(id);
                // Verify buffer caches
                std::vector<uint64_t> bufIds(offlineStream.circulatingBufferIds.begin(),
                        offlineStream.circulatingBufferIds.end());
                {
                    // Due to timing it is possible that we may not have any remaining pending
                    // capture requests that can update the caches on Hal side. This can result in
                    // buffer cache mismatch between the service and the Hal and must be accounted
                    // for.
                    std::lock_guard<std::mutex> l(mFreedBuffersLock);
                    for (const auto& it : mFreedBuffers) {
                        if (it.first == id) {
                            ALOGV("%s: stream ID %d buffer id %" PRIu64 " cache removal still "
                                    "pending", __FUNCTION__, id, it.second);
                            const auto& cachedEntry = std::find(bufIds.begin(), bufIds.end(),
                                    it.second);
                            if (cachedEntry != bufIds.end()) {
                                bufIds.erase(cachedEntry);
                            } else {
                                ALOGE("%s: stream ID %d buffer id %" PRIu64 " cache removal still "
                                        "pending however buffer is no longer in the offline stream "
                                        "info!", __FUNCTION__, id, it.second);
                            }
                        }
                    }
                }
                if (!verifyBufferIds(id, bufIds)) {
                    ALOGE("%s: stream ID %d buffer cache records mismatch!", __FUNCTION__, id);
                    return UNKNOWN_ERROR;
                }
            }

            // Move buffer records
            bufferRecords->takeBufferCaches(mBufferRecords, streams);
            bufferRecords->takeInflightBufferMap(mBufferRecords);
            bufferRecords->takeRequestedBufferMap(mBufferRecords);
            return OK;
        }

        virtual void onBufferFreed(int streamId, const native_handle_t* handle) override;

        std::mutex mFreedBuffersLock;
        std::vector<std::pair<int, uint64_t>> mFreedBuffers;

        // Keep track of buffer cache and inflight buffer records
        camera3::BufferRecords mBufferRecords;

        uint32_t mNextStreamConfigCounter = 1;

        // TODO: This can be removed after flags::session_hal_buf_manager is removed
        bool mUseHalBufManager = false;
        std::set<int32_t > mHalBufManagedStreamIds;
        bool mIsReconfigurationQuerySupported;

        const bool mSupportOfflineProcessing;
    }; // class HalInterface

    sp<HalInterface> mInterface;

    CameraMetadata             mDeviceInfo;
    bool                       mSupportNativeZoomRatio;
    bool                       mIsCompositeJpegRDisabled;
    bool                       mIsCompositeHeicDisabled;
    bool                       mIsCompositeHeicUltraHDRDisabled;
    std::unordered_map<std::string, CameraMetadata> mPhysicalDeviceInfoMap;

    CameraMetadata             mRequestTemplateCache[CAMERA_TEMPLATE_COUNT];

    struct Size {
        uint32_t width;
        uint32_t height;
        explicit Size(uint32_t w = 0, uint32_t h = 0) : width(w), height(h){}
    };

    enum Status {
        STATUS_ERROR,
        STATUS_UNINITIALIZED,
        STATUS_UNCONFIGURED,
        STATUS_CONFIGURED,
        STATUS_ACTIVE
    }                          mStatus;

    struct StatusInfo {
        Status status;
        bool isInternal; // status triggered by internal reconfigureCamera.
    };

    bool                       mStatusIsInternal;

    // Only clear mRecentStatusUpdates, mStatusWaiters from waitUntilStateThenRelock
    Vector<StatusInfo>         mRecentStatusUpdates;
    int                        mStatusWaiters;

    Condition                  mStatusChanged;

    // Tracking cause of fatal errors when in STATUS_ERROR
    std::string                mErrorCause;

    camera3::StreamSet         mOutputStreams;
    sp<camera3::Camera3Stream> mInputStream;
    bool                       mIsInputStreamMultiResolution;
    SessionStatsBuilder        mSessionStatsBuilder;
    // Map from stream group ID to physical cameras backing the stream group
    std::map<int32_t, std::set<std::string>> mGroupIdPhysicalCameraMap;

    int                        mNextStreamId;
    bool                       mNeedConfig;

    int                        mFakeStreamId;

    // Whether to send state updates upstream
    // Pause when doing transparent reconfiguration
    bool                       mPauseStateNotify;

    // Need to hold on to stream references until configure completes.
    Vector<sp<camera3::Camera3StreamInterface> > mDeletedStreams;

    // Whether the HAL will send partial result
    bool                       mUsePartialResult;

    // Number of partial results that will be delivered by the HAL.
    uint32_t                   mNumPartialResults;

    /**** End scope for mLock ****/

    bool                       mDeviceTimeBaseIsRealtime;
    // The offset converting from clock domain of other subsystem
    // (video/hardware composer) to that of camera. Assumption is that this
    // offset won't change during the life cycle of the camera device. In other
    // words, camera device shouldn't be open during CPU suspend.
    nsecs_t                    mTimestampOffset;

    class CaptureRequest : public LightRefBase<CaptureRequest> {
      public:
        PhysicalCameraSettingsList          mSettingsList;
        sp<camera3::Camera3Stream>          mInputStream;
        camera_stream_buffer_t              mInputBuffer;
        camera3::Size                       mInputBufferSize;
        Vector<sp<camera3::Camera3OutputStreamInterface> >
                                            mOutputStreams;
        SurfaceMap                          mOutputSurfaces;
        CaptureResultExtras                 mResultExtras;
        // The number of requests that should be submitted to HAL at a time.
        // For example, if batch size is 8, this request and the following 7
        // requests will be submitted to HAL at a time. The batch size for
        // the following 7 requests will be ignored by the request thread.
        int                                 mBatchSize;
        //  Whether this request is from a repeating or repeating burst.
        bool                                mRepeating;
        // Whether this request has ROTATE_AND_CROP_AUTO set, so needs both
        // overriding of ROTATE_AND_CROP value and adjustment of coordinates
        // in several other controls in both the request and the result
        bool                                mRotateAndCropAuto;
        // Indicates that the ROTATE_AND_CROP value within 'mSettingsList' was modified
        // irrespective of the original value.
        bool                                mRotateAndCropChanged = false;
        // Whether this request has AUTOFRAMING_AUTO set, so need to override the AUTOFRAMING value
        // in the capture request.
        bool                                mAutoframingAuto;
        // Indicates that the auto framing value within 'mSettingsList' was modified
        bool                                mAutoframingChanged = false;
        // Indicates that the camera test pattern setting is modified
        bool                                mTestPatternChanged = false;

        // Whether this capture request has its zoom ratio set to 1.0x before
        // the framework overrides it for camera HAL consumption.
        bool                                mZoomRatioIs1x;
        // The systemTime timestamp when the request is created.
        nsecs_t                             mRequestTimeNs;

        // Whether this capture request's distortion correction update has
        // been done.
        bool                                mDistortionCorrectionUpdated = false;
        // Whether this capture request's rotation and crop update has been
        // done.
        bool                                mRotationAndCropUpdated = false;
        // Whether this capture request's autoframing has been done.
        bool                                mAutoframingUpdated = false;
        // Whether this capture request's zoom ratio update has been done.
        bool                                mZoomRatioUpdated = false;
        // Whether this max resolution capture request's  crop / metering region update has been
        // done.
        bool                                mUHRCropAndMeteringRegionsUpdated = false;
    };
    typedef List<sp<CaptureRequest> > RequestList;

    status_t checkStatusOkToCaptureLocked();

    status_t convertMetadataListToRequestListLocked(
            const List<const PhysicalCameraSettingsList> &metadataList,
            const std::list<SurfaceMap> &surfaceMaps,
            bool repeating, nsecs_t requestTimeNs,
            /*out*/
            RequestList *requestList);

    void convertToRequestList(List<const PhysicalCameraSettingsList>& requestsList,
            std::list<SurfaceMap>& surfaceMaps,
            const CameraMetadata& request);

    status_t submitRequestsHelper(const List<const PhysicalCameraSettingsList> &requestsList,
                                  const std::list<SurfaceMap> &surfaceMaps,
                                  bool repeating,
                                  int64_t *lastFrameNumber = NULL);

    // lock to ensure only one processCaptureResult is called at a time.
    Mutex mProcessCaptureResultLock;

    /**
     * Common initialization code shared by both HAL paths
     *
     * Must be called with mLock and mInterfaceLock held.
     */
    status_t initializeCommonLocked(sp<CameraProviderManager> manager);

    /**
     * Update capture request list so that each batch size honors the batch_size_max report from
     * the HAL. Set the batch size to output stream for buffer operations.
     *
     * Must be called with mLock held.
     */
    virtual void applyMaxBatchSizeLocked(
            RequestList* requestList, const sp<camera3::Camera3OutputStreamInterface>& stream) = 0;

    struct LatestRequestInfo {
        CameraMetadata requestSettings;
        std::unordered_map<std::string, CameraMetadata> physicalRequestSettings;
        int32_t inputStreamId = -1;
        std::set<int32_t> outputStreamIds;
    };

    /**
     * Get the first repeating request in the ongoing repeating request list.
     */
    const sp<CaptureRequest> getOngoingRepeatingRequestLocked();

    /**
     * Update the first repeating request in the ongoing repeating request list
     * with the surface map provided.
     */
    status_t updateOngoingRepeatingRequestLocked(const SurfaceMap& surfaceMap);

    /**
     * Get the repeating request last frame number.
     */
    int64_t getRepeatingRequestLastFrameNumberLocked();

    /**
     * Get the last request submitted to the hal by the request thread.
     *
     * Must be called with mLock held.
     */
    virtual LatestRequestInfo getLatestRequestInfoLocked();

    virtual status_t injectionCameraInitialize(const std::string &injectCamId,
            sp<CameraProviderManager> manager) = 0;

    /**
     * Update the current device status and wake all waiting threads.
     *
     * Must be called with mLock held.
     */
    void internalUpdateStatusLocked(Status status);

    /**
     * Pause processing and flush everything, but don't tell the clients.
     * This is for reconfiguring outputs transparently when according to the
     * CameraDeviceBase interface we shouldn't need to.
     * Must be called with mLock and mInterfaceLock both held.
     */
    status_t internalPauseAndWaitLocked(nsecs_t maxExpectedDuration,
                     bool requestThreadInvocation);

    /**
     * Resume work after internalPauseAndWaitLocked()
     * Must be called with mLock and mInterfaceLock both held.
     */
    status_t internalResumeLocked();

    /**
     * Wait until status tracker tells us we've transitioned to the target state
     * set, which is either ACTIVE when active==true or IDLE (which is any
     * non-ACTIVE state) when active==false.
     *
     * Needs to be called with mLock and mInterfaceLock held.  This means there
     * can ever only be one waiter at most.
     *
     * During the wait mLock is released.
     *
     */
    status_t waitUntilStateThenRelock(bool active, nsecs_t timeout,
                     bool requestThreadInvocation);

    /**
     * Implementation of waitUntilDrained. On success, will transition to IDLE state.
     *
     * Need to be called with mLock and mInterfaceLock held.
     */
    status_t waitUntilDrainedLocked(nsecs_t maxExpectedDuration);

    /**
     * Do common work for setting up a streaming or single capture request.
     * On success, will transition to ACTIVE if in IDLE.
     */
    sp<CaptureRequest> setUpRequestLocked(const PhysicalCameraSettingsList &request,
                                          const SurfaceMap &surfaceMap);

    /**
     * Build a CaptureRequest request from the CameraDeviceBase request
     * settings.
     */
    sp<CaptureRequest> createCaptureRequest(const PhysicalCameraSettingsList &request,
                                            const SurfaceMap &surfaceMap);

    /**
     * Internally re-configure camera device using new session parameters.
     * This will get triggered by the request thread.
     */
    bool reconfigureCamera(const CameraMetadata& sessionParams, int clientStatusId);

    /**
     * Return true in case of any output or input abandoned streams,
     * otherwise return false.
     */
    bool checkAbandonedStreamsLocked();

    /**
     * Filter stream session parameters and configure camera HAL.
     */
    status_t filterParamsAndConfigureLocked(const CameraMetadata& sessionParams,
            int operatingMode);

    /**
     * Take the currently-defined set of streams and configure the HAL to use
     * them. This is a long-running operation (may be several hundered ms).
     */
    status_t           configureStreamsLocked(int operatingMode,
            const CameraMetadata& sessionParams, bool notifyRequestThread = true);

    /**
     * Cancel stream configuration that did not finish successfully.
     */
    void               cancelStreamsConfigurationLocked();

    /**
     * Add a fake stream to the current stream set as a workaround for
     * not allowing 0 streams in the camera HAL spec.
     */
    status_t           addFakeStreamLocked();

    /**
     * Remove a fake stream if the current config includes real streams.
     */
    status_t           tryRemoveFakeStreamLocked();

    /**
     * Set device into an error state due to some fatal failure, and set an
     * error message to indicate why. Only the first call's message will be
     * used. The message is also sent to the log.
     */
    void               setErrorState(const char *fmt, ...) override;
    void               setErrorStateLocked(const char *fmt, ...) override;
    void               setErrorStateV(const char *fmt, va_list args);
    void               setErrorStateLockedV(const char *fmt, va_list args);

    /////////////////////////////////////////////////////////////////////
    // Implements InflightRequestUpdateInterface

    void onInflightEntryRemovedLocked(nsecs_t duration) override;
    void checkInflightMapLengthLocked() override;
    void onInflightMapFlushedLocked() override;

    /////////////////////////////////////////////////////////////////////

    /**
     * Debugging trylock/spin method
     * Try to acquire a lock a few times with sleeps between before giving up.
     */
    bool               tryLockSpinRightRound(Mutex& lock);

    /**
     * Helper function to get the offset between MONOTONIC and BOOTTIME
     * timestamp.
     */
    static nsecs_t getMonoToBoottimeOffset();

    // Override rotate_and_crop control if needed
    static bool    overrideAutoRotateAndCrop(const sp<CaptureRequest> &request /*out*/,
            int rotationOverride,
            camera_metadata_enum_android_scaler_rotate_and_crop_t rotateAndCropOverride);

    // Override auto framing control if needed
    static bool    overrideAutoframing(const sp<CaptureRequest> &request /*out*/,
            camera_metadata_enum_android_control_autoframing_t autoframingOverride);

    struct RequestTrigger {
        // Metadata tag number, e.g. android.control.aePrecaptureTrigger
        uint32_t metadataTag;
        // Metadata value, e.g. 'START' or the trigger ID
        int32_t entryValue;

        // The last part of the fully qualified path, e.g. afTrigger
        const char *getTagName() const {
            return get_camera_metadata_tag_name(metadataTag) ?: "NULL";
        }

        // e.g. TYPE_BYTE, TYPE_INT32, etc.
        int getTagType() const {
            return get_camera_metadata_tag_type(metadataTag);
        }
    };

    /**
     * Thread for managing capture request submission to HAL device.
     */
    class RequestThread : public Thread {

      public:

        RequestThread(wp<Camera3Device> parent,
                sp<camera3::StatusTracker> statusTracker,
                sp<HalInterface> interface,
                const Vector<int32_t>& sessionParamKeys,
                bool useHalBufManager,
                bool supportCameraMute,
                int rotationOverride,
                bool supportSettingsOverride);
        ~RequestThread();

        void     setNotificationListener(wp<NotificationListener> listener);

        /**
         * Call after stream (re)-configuration is completed.
         */
        void     configurationComplete(bool isConstrainedHighSpeed,
                const CameraMetadata& sessionParams,
                const std::map<int32_t, std::set<std::string>>& groupIdPhysicalCameraMap);

        /**
         * Set or clear the list of repeating requests. Does not block
         * on either. Use waitUntilPaused to wait until request queue
         * has emptied out.
         */
        status_t setRepeatingRequests(const RequestList& requests,
                                      /*out*/
                                      int64_t *lastFrameNumber = NULL);
        status_t clearRepeatingRequests(/*out*/
                                        int64_t *lastFrameNumber = NULL);

        status_t queueRequestList(List<sp<CaptureRequest> > &requests,
                                  /*out*/
                                  int64_t *lastFrameNumber = NULL);

        /**
         * Remove all queued and repeating requests, and pending triggers
         */
        status_t clear(/*out*/int64_t *lastFrameNumber = NULL);

        /**
         * Flush all pending requests in HAL.
         */
        status_t flush();

        /**
         * Queue a trigger to be dispatched with the next outgoing
         * process_capture_request. The settings for that request only
         * will be temporarily rewritten to add the trigger tag/value.
         * Subsequent requests will not be rewritten (for this tag).
         */
        status_t queueTrigger(RequestTrigger trigger[], size_t count);

        /**
         * Pause/unpause the capture thread. Doesn't block, so use
         * waitUntilPaused to wait until the thread is paused.
         */
        void     setPaused(bool paused);

        /**
         * Set Hal buffer managed streams
         * @param halBufferManagedStreams The streams for which hal buffer manager is enabled
         *
         */
        void setHalBufferManagedStreams(const std::set<int32_t> &halBufferManagedStreams);

        /**
         * Wait until thread processes the capture request with settings'
         * android.request.id == requestId.
         *
         * Returns TIMED_OUT in case the thread does not process the request
         * within the timeout.
         */
        status_t waitUntilRequestProcessed(int32_t requestId, nsecs_t timeout);

        /**
         * Shut down the thread. Shutdown is asynchronous, so thread may
         * still be running once this method returns.
         */
        virtual void requestExit();

        /**
         * Get the latest request that was sent to the HAL
         * with process_capture_request.
         */
        LatestRequestInfo getLatestRequestInfo() const;

        /**
         * Returns true if the stream is a target of any queued or repeating
         * capture request
         */
        bool isStreamPending(sp<camera3::Camera3StreamInterface>& stream);

        /**
         * Returns true if the surface is a target of any queued or repeating
         * capture request
         */
        bool isOutputSurfacePending(int streamId, size_t surfaceId);

        // dump processCaptureRequest latency
        void dumpCaptureRequestLatency(int fd, const char* name) {
            mRequestLatency.dump(fd, name);
        }

        void signalPipelineDrain(const std::vector<int>& streamIds);
        void resetPipelineDrain();

        void clearPreviousRequest();

        status_t setRotateAndCropAutoBehavior(
                camera_metadata_enum_android_scaler_rotate_and_crop_t rotateAndCropValue);

        status_t setAutoframingAutoBehaviour(
                camera_metadata_enum_android_control_autoframing_t autoframingValue);

        status_t setComposerSurface(bool composerSurfacePresent);

        status_t setCameraMute(int32_t muteMode);

        status_t setZoomOverride(int32_t zoomOverride);

        status_t setHalInterface(sp<HalInterface> newHalInterface);

        status_t setInjectedSessionParams(const CameraMetadata& sessionParams);

        void injectSessionParams(
            const sp<CaptureRequest> &request,
            const CameraMetadata& injectedSessionParams);

        /**
         * signal mLatestRequestmutex
         **/
        void wakeupLatestRequest(bool latestRequestFailed, int32_t latestRequestId);

        /**
         * Get the first repeating request in the ongoing repeating request list.
         */
        const sp<CaptureRequest> getOngoingRepeatingRequest();

        /**
         * Update the first repeating request in the ongoing repeating request list
         * with the surface map provided.
         */
        status_t updateOngoingRepeatingRequest(const SurfaceMap& surfaceMap);

        // Get the repeating request last frame number.
        int64_t getRepeatingRequestLastFrameNumber();

      protected:

        virtual bool threadLoop();

        static const std::string& getId(const wp<Camera3Device> &device);

        status_t           queueTriggerLocked(RequestTrigger trigger);
        // Mix-in queued triggers into this request
        int32_t            insertTriggers(const sp<CaptureRequest> &request);
        // Purge the queued triggers from this request,
        //  restoring the old field values for those tags.
        status_t           removeTriggers(const sp<CaptureRequest> &request);

        // HAL workaround: Make sure a trigger ID always exists if
        // a trigger does
        status_t           addFakeTriggerIds(const sp<CaptureRequest> &request);

        // Override rotate_and_crop control if needed; returns true if the current value was changed
        bool               overrideAutoRotateAndCrop(const sp<CaptureRequest> &request /*out*/);

        // Override autoframing control if needed; returns true if the current value was changed
        bool               overrideAutoframing(const sp<CaptureRequest> &request);

        // Override test_pattern control if needed for camera mute; returns true
        // if the current value was changed
        bool               overrideTestPattern(const sp<CaptureRequest> &request);

        // Override settings override if needed for lower zoom latency; return
        // true if the current value was changed
        bool               overrideSettingsOverride(const sp<CaptureRequest> &request);

        static const nsecs_t kRequestTimeout = 50e6; // 50 ms

        // TODO: does this need to be adjusted for long exposure requests?
        static const nsecs_t kRequestSubmitTimeout = 500e6; // 500 ms

        // Used to prepare a batch of requests.
        struct NextRequest {
            sp<CaptureRequest>              captureRequest;
            camera_capture_request_t       halRequest;
            Vector<camera_stream_buffer_t> outputBuffers;
            bool                            submitted;
        };

        // Wait for the next batch of requests and put them in mNextRequests. mNextRequests will
        // be empty if it times out.
        void waitForNextRequestBatch();

        // Waits for a request, or returns NULL if times out. Must be called with mRequestLock hold.
        sp<CaptureRequest> waitForNextRequestLocked();

        // Prepare HAL requests and output buffers in mNextRequests. Return TIMED_OUT if getting any
        // output buffer timed out. If an error is returned, the caller should clean up the pending
        // request batch.
        status_t prepareHalRequests();

        // Return buffers, etc, for requests in mNextRequests that couldn't be fully constructed and
        // send request errors if sendRequestError is true. The buffers will be returned in the
        // ERROR state to mark them as not having valid data. mNextRequests will be cleared.
        void cleanUpFailedRequests(bool sendRequestError);

        // Stop the repeating request if any of its output streams is abandoned.
        void checkAndStopRepeatingRequest();

        // Release physical camera settings and camera id resources.
        void cleanupPhysicalSettings(sp<CaptureRequest> request,
                /*out*/camera_capture_request_t *halRequest);

        // Pause handling
        bool               waitIfPaused();
        void               unpauseForNewRequests();

        // Relay error to parent device object setErrorState
        void               setErrorState(const char *fmt, ...);

        // If the input request is in mRepeatingRequests. Must be called with mRequestLock hold
        bool isRepeatingRequestLocked(const sp<CaptureRequest>&);

        // Clear repeating requests. Must be called with mRequestLock held.
        status_t clearRepeatingRequestsLocked(/*out*/ int64_t *lastFrameNumber = NULL);

        // send request in mNextRequests to HAL in a batch. Return true = sucssess
        bool sendRequestsBatch();

        // Calculate the expected (minimum, maximum, isFixedFps) duration info for a request
        struct ExpectedDurationInfo {
            nsecs_t minDuration;
            nsecs_t maxDuration;
            bool isFixedFps;
        };
        ExpectedDurationInfo calculateExpectedDurationRange(
                const camera_metadata_t *request);

        // Check and update latest session parameters based on the current request settings.
        bool updateSessionParameters(const CameraMetadata& settings);

        // Check whether FPS range session parameter re-configuration is needed in constrained
        // high speed recording camera sessions.
        bool skipHFRTargetFPSUpdate(int32_t tag, const camera_metadata_ro_entry_t& newEntry,
                const camera_metadata_entry_t& currentEntry);

        // Update next request sent to HAL
        void updateNextRequest(NextRequest& nextRequest);

        wp<Camera3Device>  mParent;
        wp<camera3::StatusTracker>  mStatusTracker;
        sp<HalInterface>   mInterface;

        wp<NotificationListener> mListener;

        const std::string  mId;       // The camera ID
        int                mStatusId; // The RequestThread's component ID for
                                      // status tracking

        Mutex              mRequestLock;
        Condition          mRequestSignal;
        bool               mRequestClearing;

        Condition          mRequestSubmittedSignal;
        RequestList        mRequestQueue;
        RequestList        mRepeatingRequests;
        bool               mFirstRepeating;
        // The next batch of requests being prepped for submission to the HAL, no longer
        // on the request queue. Read-only even with mRequestLock held, outside
        // of threadLoop
        Vector<NextRequest> mNextRequests;

        // To protect flush() and sending a request batch to HAL.
        Mutex              mFlushLock;

        bool               mReconfigured;

        // Used by waitIfPaused, waitForNextRequest, waitUntilPaused, and signalPipelineDrain
        Mutex              mPauseLock;
        bool               mDoPause;
        Condition          mDoPauseSignal;
        bool               mPaused;
        bool               mNotifyPipelineDrain;
        std::vector<int>   mStreamIdsToBeDrained;

        sp<CaptureRequest> mPrevRequest;
        int32_t            mPrevTriggers;
        std::set<std::string> mPrevCameraIdsWithZoom;

        uint32_t           mFrameNumber;

        mutable Mutex      mLatestRequestMutex;
        Condition          mLatestRequestSignal;
        // android.request.id for latest process_capture_request
        int32_t            mLatestRequestId;
        int32_t            mLatestFailedRequestId;
        LatestRequestInfo mLatestRequestInfo;

        typedef KeyedVector<uint32_t/*tag*/, RequestTrigger> TriggerMap;
        Mutex              mTriggerMutex;
        TriggerMap         mTriggerMap;
        TriggerMap         mTriggerRemovedMap;
        TriggerMap         mTriggerReplacedMap;
        uint32_t           mCurrentAfTriggerId;
        uint32_t           mCurrentPreCaptureTriggerId;
        camera_metadata_enum_android_scaler_rotate_and_crop_t mRotateAndCropOverride;
        camera_metadata_enum_android_control_autoframing_t mAutoframingOverride;
        bool               mComposerOutput;
        int32_t            mCameraMute; // 0 = no mute, otherwise the TEST_PATTERN_MODE to use
        int32_t            mSettingsOverride; // -1 = use original, otherwise
                                              // the settings override to use.

        int64_t            mRepeatingLastFrameNumber;

        // Flag indicating if we should prepare video stream for video requests.
        bool               mPrepareVideoStream;

        bool               mConstrainedMode;

        static const int32_t kRequestLatencyBinSize = 40; // in ms
        CameraLatencyHistogram mRequestLatency;

        Vector<int32_t>    mSessionParamKeys;
        CameraMetadata     mLatestSessionParams;
        CameraMetadata     mInjectedSessionParams;
        bool               mForceNewRequestAfterReconfigure;

        std::map<int32_t, std::set<std::string>> mGroupIdPhysicalCameraMap;

        bool               mUseHalBufManager = false;
        std::set<int32_t > mHalBufManagedStreamIds;
        const bool         mSupportCameraMute;
        const bool         mRotationOverride;
        const bool         mSupportSettingsOverride;
        int32_t            mVndkVersion = -1;
    };

    virtual sp<RequestThread> createNewRequestThread(wp<Camera3Device> /*parent*/,
                sp<camera3::StatusTracker> /*statusTracker*/,
                sp<HalInterface> /*interface*/,
                const Vector<int32_t>& /*sessionParamKeys*/,
                bool /*useHalBufManager*/,
                bool /*supportCameraMute*/,
                int /*rotationOverride*/,
                bool /*supportSettingsOverride*/) = 0;

    sp<RequestThread> mRequestThread;

    /**
     * In-flight queue for tracking completion of capture requests.
     */
    std::mutex                    mInFlightLock;
    camera3::InFlightRequestMap   mInFlightMap;
    nsecs_t                       mExpectedInflightDuration = 0;
    int64_t                       mLastCompletedRegularFrameNumber = -1;
    int64_t                       mLastCompletedReprocessFrameNumber = -1;
    int64_t                       mLastCompletedZslFrameNumber = -1;
    // End of mInFlightLock protection scope

    int mInFlightStatusId; // const after initialize

    status_t registerInFlight(uint32_t frameNumber,
            int32_t numBuffers, CaptureResultExtras resultExtras, bool hasInput,
            bool callback, nsecs_t minExpectedDuration, nsecs_t maxExpectedDuration,
            bool isFixedFps, const std::set<std::set<std::string>>& physicalCameraIds,
            bool isStillCapture, bool isZslCapture, bool rotateAndCropAuto, bool autoframingAuto,
            const std::set<std::string>& cameraIdsWithZoom, bool useZoomRatio,
            const SurfaceMap& outputSurfaces, nsecs_t requestTimeNs);

    /**
     * Tracking for idle detection
     */
    sp<camera3::StatusTracker> mStatusTracker;

    /**
     * Graphic buffer manager for output streams. Each device has a buffer manager, which is used
     * by the output streams to get and return buffers if these streams are registered to this
     * buffer manager.
     */
    sp<camera3::Camera3BufferManager> mBufferManager;

    /**
     * Thread for preparing streams
     */
    class PreparerThread : private Thread, public virtual RefBase {
      public:
        PreparerThread();
        ~PreparerThread();

        void setNotificationListener(wp<NotificationListener> listener);

        /**
         * Queue up a stream to be prepared. Streams are processed by a background thread in FIFO
         * order.  Pre-allocate up to maxCount buffers for the stream, or the maximum number needed
         * for the pipeline if maxCount is ALLOCATE_PIPELINE_MAX.
         */
        status_t prepare(int maxCount, sp<camera3::Camera3StreamInterface>& stream);

        /**
         * Cancel all current and pending stream preparation
         */
        status_t clear();

        /**
         * Pause all preparation activities
         */
        void pause();

        /**
         * Resume preparation activities
         */
        status_t resume();

      private:
        Mutex mLock;
        Condition mThreadActiveSignal;

        virtual bool threadLoop();

        // Guarded by mLock

        wp<NotificationListener> mListener;
        std::list<std::tuple<int, sp<camera3::Camera3StreamInterface>>> mPendingStreams;
        bool mActive;
        bool mCancelNow;

        // Only accessed by threadLoop and the destructor

        sp<camera3::Camera3StreamInterface> mCurrentStream;
        int mCurrentMaxCount;
        bool mCurrentPrepareComplete;
    };
    sp<PreparerThread> mPreparerThread;

    /**
     * Output result queue and current HAL device 3A state
     */

    // Lock for output side of device
    std::mutex             mOutputLock;

    /**** Scope for mOutputLock ****/
    // the minimal frame number of the next non-reprocess result
    uint32_t               mNextResultFrameNumber;
    // the minimal frame number of the next reprocess result
    uint32_t               mNextReprocessResultFrameNumber;
    // the minimal frame number of the next ZSL still capture result
    uint32_t               mNextZslStillResultFrameNumber;
    // the minimal frame number of the next non-reprocess shutter
    uint32_t               mNextShutterFrameNumber;
    // the minimal frame number of the next reprocess shutter
    uint32_t               mNextReprocessShutterFrameNumber;
    // the minimal frame number of the next ZSL still capture shutter
    uint32_t               mNextZslStillShutterFrameNumber;
    std::list<CaptureResult>    mResultQueue;
    std::condition_variable  mResultSignal;
    wp<NotificationListener> mListener;

    /**** End scope for mOutputLock ****/

    /**** Scope for mInFlightLock ****/

    // Remove the in-flight map entry of the given index from mInFlightMap.
    // It must only be called with mInFlightLock held.
    void removeInFlightMapEntryLocked(int idx);

    // Remove all in-flight requests and return all buffers.
    // This is used after HAL interface is closed to cleanup any request/buffers
    // not returned by HAL.
    void flushInflightRequests();

    /**** End scope for mInFlightLock ****/

    /**
     * Distortion correction support
     */
    // Map from camera IDs to its corresponding distortion mapper. Only contains
    // 1 ID if the device isn't a logical multi-camera. Otherwise contains both
    // logical camera and its physical subcameras.
    std::unordered_map<std::string, camera3::DistortionMapper> mDistortionMappers;

    /**
     * Zoom ratio mapper support
     */
    std::unordered_map<std::string, camera3::ZoomRatioMapper> mZoomRatioMappers;

    /**
     * UHR request crop / metering region mapper support
     */
    std::unordered_map<std::string, camera3::UHRCropAndMeteringRegionMapper>
            mUHRCropAndMeteringRegionMappers;

    /**
     * RotateAndCrop mapper support
     */
    std::unordered_map<std::string, camera3::RotateAndCropMapper> mRotateAndCropMappers;

    // Debug tracker for metadata tag value changes
    // - Enabled with the -m <taglist> option to dumpsys, such as
    //   dumpsys -m android.control.aeState,android.control.aeMode
    // - Disabled with -m off
    // - dumpsys -m 3a is a shortcut for ae/af/awbMode, State, and Triggers
    TagMonitor mTagMonitor;

    void monitorMetadata(TagMonitor::eventSource source, int64_t frameNumber,
            nsecs_t timestamp, const CameraMetadata& metadata,
            const std::unordered_map<std::string, CameraMetadata>& physicalMetadata,
            const camera_stream_buffer_t *outputBuffers, uint32_t numOutputBuffers,
            int32_t inputStreamId);

    // Collect any statistics that are based on the stream of capture requests sent
    // to the HAL
    void collectRequestStats(int64_t frameNumber, const CameraMetadata& request);

    metadata_vendor_id_t mVendorTagId;

    // Cached last requested template id
    int mLastTemplateId;

    // Synchronizes access to status tracker between inflight updates and disconnect.
    // b/79972865
    Mutex mTrackerLock;

    // Whether HAL request buffers through requestStreamBuffers API
    bool mUseHalBufManager = false;
    std::set<int32_t > mHalBufManagedStreamIds;
    bool mSessionHalBufManager = false;
    // Lock to ensure requestStreamBuffers() callbacks are serialized
    std::mutex mRequestBufferInterfaceLock;

    // The state machine to control when requestStreamBuffers should allow
    // HAL to request buffers.
    enum RequestBufferState {
        /**
         * This is the initial state.
         * requestStreamBuffers call will return FAILED_CONFIGURING in this state.
         * Will switch to RB_STATUS_READY after a successful configureStreams or
         * processCaptureRequest call.
         */
        RB_STATUS_STOPPED,

        /**
         * requestStreamBuffers call will proceed in this state.
         * When device is asked to stay idle via waitUntilStateThenRelock() call:
         *     - Switch to RB_STATUS_STOPPED if there is no inflight requests and
         *       request thread is paused.
         *     - Switch to RB_STATUS_PENDING_STOP otherwise
         */
        RB_STATUS_READY,

        /**
         * requestStreamBuffers call will proceed in this state.
         * Switch to RB_STATUS_STOPPED when all inflight requests are fulfilled
         * and request thread is paused
         */
        RB_STATUS_PENDING_STOP,
    };

    class RequestBufferStateMachine {
      public:
        status_t initialize(sp<camera3::StatusTracker> statusTracker);

        status_t deInit();

        // Return if the state machine currently allows for requestBuffers
        // If the state allows for it, mRequestBufferOngoing will be set to true
        // and caller must call endRequestBuffer() later to unset the flag
        bool startRequestBuffer();
        void endRequestBuffer();

        // Events triggered by application API call
        void onStreamsConfigured();
        void onWaitUntilIdle();

        // Events usually triggered by hwBinder processCaptureResult callback thread
        // But can also be triggered on request thread for failed request, or on
        // hwbinder notify callback thread for shutter/error callbacks
        void onInflightMapEmpty();

        // Events triggered by RequestThread
        void onSubmittingRequest();
        void onRequestThreadPaused();

        // Events triggered by successful switchToOffline call
        // Return true is there is no ongoing requestBuffer call.
        bool onSwitchToOfflineSuccess();

      private:
        void notifyTrackerLocked(bool active);

        // Switch to STOPPED state and return true if all conditions allows for it.
        // Otherwise do nothing and return false.
        bool checkSwitchToStopLocked();

        std::mutex mLock;
        RequestBufferState mStatus = RB_STATUS_STOPPED;

        bool mRequestThreadPaused = true;
        bool mInflightMapEmpty = true;
        bool mRequestBufferOngoing = false;
        bool mSwitchedToOffline = false;

        wp<camera3::StatusTracker> mStatusTracker;
        int  mRequestBufferStatusId;
    } mRequestBufferSM;

    // Fix up result metadata for monochrome camera.
    bool mNeedFixupMonochromeTags;

    // Whether HAL supports offline processing capability.
    bool mSupportOfflineProcessing = false;

    // Whether the HAL supports camera muting via test pattern
    bool mSupportCameraMute = false;
    // Whether the HAL supports SOLID_COLOR or BLACK if mSupportCameraMute is true
    bool mSupportTestPatternSolidColor = false;
    // Whether the HAL supports zoom settings override
    bool mSupportZoomOverride = false;

    // Whether the camera framework overrides the device characteristics for
    // performance class.
    bool mOverrideForPerfClass;

    // Whether the camera framework overrides the device characteristics for
    // app compatibility reasons.
    int mRotationOverride;
    camera_metadata_enum_android_scaler_rotate_and_crop_t mRotateAndCropOverride;
    bool mComposerOutput;

    // Auto framing override value
    camera_metadata_enum_android_control_autoframing mAutoframingOverride;

    // Initial camera mute state stored before the request thread
    // is active.
    bool mCameraMuteInitial = false;

    // Settings override value
    int32_t mSettingsOverride; // -1 = use original, otherwise
                               // the settings override to use.

    // Current active physical id of the logical multi-camera, if any
    std::string mActivePhysicalId;

    // The current minimum expected frame duration based on AE_TARGET_FPS_RANGE
    nsecs_t mMinExpectedDuration = 0;
    // Whether the camera device runs at fixed frame rate based on AE_MODE and
    // AE_TARGET_FPS_RANGE
    bool mIsFixedFps = false;

    // Flag to indicate that we shouldn't forward extension related metadata
    bool mSupportsExtensionKeys = false;

    // If the client is a native client, either opened through vndk, or caling
    // Pid is a platform service.
    bool mIsNativeClient;

    // Injection camera related methods.
    class Camera3DeviceInjectionMethods : public virtual RefBase {
      public:
        Camera3DeviceInjectionMethods(wp<Camera3Device> parent);

        ~Camera3DeviceInjectionMethods();

        // Injection camera will replace the internal camera and configure streams
        // when device is IDLE and request thread is paused.
        status_t injectCamera(
                camera3::camera_stream_configuration& injectionConfig,
                const std::vector<uint32_t>& injectionBufferSizes);

        // Stop the injection camera and switch back to backup hal interface.
        status_t stopInjection();

        bool isInjecting();

        bool isStreamConfigCompleteButNotInjected();

        const std::string& getInjectedCamId() const;

        void getInjectionConfig(/*out*/ camera3::camera_stream_configuration* injectionConfig,
                /*out*/ std::vector<uint32_t>* injectionBufferSizes);

        // When the streaming configuration is completed and the camera device is active, but the
        // injection camera has not yet been injected, the streaming configuration of the internal
        // camera will be stored first.
        void storeInjectionConfig(
                const camera3::camera_stream_configuration& injectionConfig,
                const std::vector<uint32_t>& injectionBufferSizes);

      protected:
        // Configure the streams of injection camera, it need wait until the
        // output streams are created and configured to the original camera before
        // proceeding.
        status_t injectionConfigureStreams(
                camera3::camera_stream_configuration& injectionConfig,
                const std::vector<uint32_t>& injectionBufferSizes);

        // Disconnect the injection camera and delete the hal interface.
        void injectionDisconnectImpl();

        // Use injection camera hal interface to replace and backup original
        // camera hal interface.
        virtual status_t replaceHalInterface(sp<HalInterface> /*newHalInterface*/,
                bool /*keepBackup*/) = 0;

        wp<Camera3Device> mParent;

        // Backup of the original camera hal interface.
        sp<HalInterface> mBackupHalInterface;

        // Generated injection camera hal interface.
        sp<HalInterface> mInjectedCamHalInterface;

        // The flag indicates that the stream configuration is complete, the camera device is
        // active, but the injection camera has not yet been injected.
        bool mIsStreamConfigCompleteButNotInjected = false;

        // Copy the configuration of the internal camera.
        camera3::camera_stream_configuration mInjectionConfig;

        // Copy the streams of the internal camera.
        Vector<camera3::camera_stream_t*> mInjectionStreams;

        // Copy the bufferSizes of the output streams of the internal camera.
        std::vector<uint32_t> mInjectionBufferSizes;

        // Synchronizes access to injection camera between initialize and
        // disconnect.
        Mutex mInjectionLock;

        // The injection camera ID.
        std::string mInjectedCamId;
    };

    virtual sp<Camera3DeviceInjectionMethods>
            createCamera3DeviceInjectionMethods(wp<Camera3Device>) = 0;

    sp<Camera3DeviceInjectionMethods> mInjectionMethods;

    void overrideStreamUseCaseLocked();
    status_t deriveAndSetTransformLocked(camera3::Camera3OutputStreamInterface& stream,
                                   int mirrorMode, int surfaceId);


}; // class Camera3Device

}; // namespace android

#endif
