/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ANDROID_SERVERS_CAMERA3_STREAM_INTERFACE_H
#define ANDROID_SERVERS_CAMERA3_STREAM_INTERFACE_H

#include <gui/Flags.h>
#include <utils/RefBase.h>

#include <camera/camera2/OutputConfiguration.h>
#include <camera/CameraMetadata.h>
#include "Camera3StreamBufferListener.h"
#include "Camera3StreamBufferFreedListener.h"

namespace android {

namespace camera3 {

typedef enum camera_buffer_status {
    CAMERA_BUFFER_STATUS_OK = 0,
    CAMERA_BUFFER_STATUS_ERROR = 1
} camera_buffer_status_t;

typedef enum camera_stream_type {
    CAMERA_STREAM_OUTPUT = 0,
    CAMERA_STREAM_INPUT = 1,
    CAMERA_NUM_STREAM_TYPES
} camera_stream_type_t;

typedef enum camera_stream_rotation {
    /* No rotation */
    CAMERA_STREAM_ROTATION_0 = 0,

    /* Rotate by 90 degree counterclockwise */
    CAMERA_STREAM_ROTATION_90 = 1,

    /* Rotate by 180 degree counterclockwise */
    CAMERA_STREAM_ROTATION_180 = 2,

    /* Rotate by 270 degree counterclockwise */
    CAMERA_STREAM_ROTATION_270 = 3
} camera_stream_rotation_t;

typedef struct camera_stream {
    camera_stream_type_t stream_type;
    uint32_t width;
    uint32_t height;
    int format;
    uint32_t usage;
    uint32_t max_buffers;
    android_dataspace_t data_space;
    camera_stream_rotation_t rotation;
    std::string physical_camera_id;

    std::unordered_set<int32_t> sensor_pixel_modes_used;
    int64_t dynamic_range_profile;
    int64_t use_case;
    int32_t color_space;
} camera_stream_t;

typedef struct camera_stream_buffer {
    camera_stream_t *stream;
    buffer_handle_t *buffer;
    camera_buffer_status_t status;
    int acquire_fence;
    int release_fence;
} camera_stream_buffer_t;

struct Size {
    uint32_t width;
    uint32_t height;
    explicit Size(uint32_t w = 0, uint32_t h = 0) : width(w), height(h){}
};

enum {
    /**
     * This stream set ID indicates that the set ID is invalid, and this stream doesn't intend to
     * share buffers with any other stream. It is illegal to register this kind of stream to
     * Camera3BufferManager.
     */
    CAMERA3_STREAM_SET_ID_INVALID = -1,

    /**
     * Invalid output stream ID.
     */
    CAMERA3_STREAM_ID_INVALID = -1,
};

class StatusTracker;

// OutputStreamInfo describes the property of a camera stream.
class OutputStreamInfo {
    public:
        int width;
        int height;
        int format;
        android_dataspace dataSpace;
        uint64_t consumerUsage;
        bool finalized = false;
        bool supportsOffline = false;
        std::unordered_set<int32_t> sensorPixelModesUsed;
        int64_t dynamicRangeProfile;
        int64_t streamUseCase;
        int timestampBase;
        int32_t colorSpace;
        OutputStreamInfo() :
            width(-1), height(-1), format(-1), dataSpace(HAL_DATASPACE_UNKNOWN),
            consumerUsage(0),
            dynamicRangeProfile(ANDROID_REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_STANDARD),
            streamUseCase(ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT),
            timestampBase(OutputConfiguration::TIMESTAMP_BASE_DEFAULT),
            colorSpace(ANDROID_REQUEST_AVAILABLE_COLOR_SPACE_PROFILES_MAP_UNSPECIFIED) {}
        OutputStreamInfo(int _width, int _height, int _format, android_dataspace _dataSpace,
                uint64_t _consumerUsage, const std::unordered_set<int32_t>& _sensorPixelModesUsed,
                int64_t _dynamicRangeProfile, int _streamUseCase, int _timestampBase,
                int32_t _colorSpace) :
            width(_width), height(_height), format(_format),
            dataSpace(_dataSpace), consumerUsage(_consumerUsage),
            sensorPixelModesUsed(_sensorPixelModesUsed), dynamicRangeProfile(_dynamicRangeProfile),
            streamUseCase(_streamUseCase), timestampBase(_timestampBase), colorSpace(_colorSpace) {}
        bool operator == (const OutputStreamInfo& other) const {
            return (width == other.width &&
                    height == other.height &&
                    format == other.format &&
                    (other.format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED ||
                    (dataSpace == other.dataSpace && consumerUsage == other.consumerUsage)) &&
                    sensorPixelModesUsed == other.sensorPixelModesUsed &&
                    dynamicRangeProfile == other.dynamicRangeProfile &&
                    colorSpace == other.colorSpace &&
                    streamUseCase == other.streamUseCase &&
                    timestampBase == other.timestampBase);
    }
};

// A holder containing a surface and its corresponding mirroring mode
struct SurfaceHolder {
    sp<Surface> mSurface;
    int mMirrorMode = OutputConfiguration::MIRROR_MODE_AUTO;
};

// Utility class to lock and unlock a GraphicBuffer
class GraphicBufferLocker {
public:
    GraphicBufferLocker(sp<GraphicBuffer> buffer) : _buffer(buffer) {}

    status_t lockAsync(uint32_t usage, void** dstBuffer, int fenceFd) {
        if (_buffer == nullptr) return BAD_VALUE;

        status_t res = OK;
        if (!_locked) {
            status_t res =  _buffer->lockAsync(usage, dstBuffer, fenceFd);
            if (res == OK) {
                _locked = true;
            }
        }
        return res;
    }

    status_t lockAsync(void** dstBuffer, int fenceFd) {
        return lockAsync(GRALLOC_USAGE_SW_WRITE_OFTEN, dstBuffer, fenceFd);
    }

    ~GraphicBufferLocker() {
        if (_locked && _buffer != nullptr) {
            auto res = _buffer->unlock();
            if (res != OK) {
                ALOGE("%s: Error trying to unlock buffer: %s (%d)", __FUNCTION__,
                        strerror(-res), res);
            }
        }
    }

private:
    sp<GraphicBuffer> _buffer;
    bool _locked = false;
};

/**
 * An interface for managing a single stream of input and/or output data from
 * the camera device.
 */
class Camera3StreamInterface : public virtual RefBase {
  public:

    enum {
        ALLOCATE_PIPELINE_MAX = 0, // Allocate max buffers used by a given surface
    };

    /**
     * Get the stream's ID
     */
    virtual int      getId() const = 0;

    /**
     * Get the output stream set id.
     */
    virtual int      getStreamSetId() const = 0;

    /**
     * Is this stream part of a multi-resolution stream set
     */
    virtual bool     isMultiResolution() const = 0;

    /**
     * Get the HAL stream group id for a multi-resolution stream set
     */
    virtual int      getHalStreamGroupId() const = 0;

    /**
     * Get the stream's dimensions and format
     */
    virtual uint32_t getWidth() const = 0;
    virtual uint32_t getHeight() const = 0;
    virtual int      getFormat() const = 0;
    virtual int64_t  getDynamicRangeProfile() const = 0;
    virtual android_dataspace getDataSpace() const = 0;
    virtual int32_t getColorSpace() const = 0;
    virtual void setFormatOverride(bool formatOverriden) = 0;
    virtual bool isFormatOverridden() const = 0;
    virtual int getOriginalFormat() const = 0;
    virtual void setDataSpaceOverride(bool dataSpaceOverriden) = 0;
    virtual bool isDataSpaceOverridden() const = 0;
    virtual android_dataspace getOriginalDataSpace() const = 0;
    virtual int getMaxHalBuffers() const = 0;
    virtual int getMaxTotalBuffers() const = 0;

    /**
     * Offline processing
     */
    virtual void setOfflineProcessingSupport(bool support) = 0;
    virtual bool getOfflineProcessingSupport() const = 0;

    /**
     * Get a handle for the stream, without starting stream configuration.
     */
    virtual camera_stream* asHalStream() = 0;

    /**
     * Start the stream configuration process. Returns a handle to the stream's
     * information to be passed into the device's configure_streams call.
     *
     * Until finishConfiguration() is called, no other methods on the stream may
     * be called. The usage and max_buffers fields of camera_stream may be
     * modified between start/finishConfiguration, but may not be changed after
     * that. The priv field of camera_stream may be modified at any time after
     * startConfiguration.
     *
     * Returns NULL in case of error starting configuration.
     */
    virtual camera_stream* startConfiguration() = 0;

    /**
     * Check if the stream is mid-configuration (start has been called, but not
     * finish).  Used for lazy completion of configuration.
     */
    virtual bool    isConfiguring() const = 0;

    /**
     * Completes the stream configuration process. During this call, the stream
     * may call the device's register_stream_buffers() method. The stream
     * information structure returned by startConfiguration() may no longer be
     * modified after this call, but can still be read until the destruction of
     * the stream.
     *
     * streamReconfigured: set to true when a stream is being reconfigured.
     *
     * Returns:
     *   OK on a successful configuration
     *   NO_INIT in case of a serious error from the HAL device
     *   NO_MEMORY in case of an error registering buffers
     *   INVALID_OPERATION in case connecting to the consumer failed
     */
    virtual status_t finishConfiguration(/*out*/bool* streamReconfigured = nullptr) = 0;

    /**
     * Cancels the stream configuration process. This returns the stream to the
     * initial state, allowing it to be configured again later.
     * This is done if the HAL rejects the proposed combined stream configuration
     */
    virtual status_t cancelConfiguration() = 0;

    /**
     * Determine whether the stream has already become in-use (has received
     * a valid filled buffer), which determines if a stream can still have
     * prepareNextBuffer called on it.
     */
    virtual bool     isUnpreparable() = 0;

    /**
     * Mark the stream as unpreparable.
     */
    virtual void     markUnpreparable() = 0;

    /**
     * Start stream preparation. May only be called in the CONFIGURED state,
     * when no valid buffers have yet been returned to this stream. Prepares
     * up to maxCount buffers, or the maximum number of buffers needed by the
     * pipeline if maxCount is ALLOCATE_PIPELINE_MAX.
     *
     * If no prepartion is necessary, returns OK and does not transition to
     * PREPARING state. Otherwise, returns NOT_ENOUGH_DATA and transitions
     * to PREPARING.
     *
     * blockRequest specifies whether prepare will block upcoming capture
     * request. This flag should only be set to false if the caller guarantees
     * the whole buffer preparation process is done before capture request
     * comes in.
     *
     * Returns:
     *    OK if no more buffers need to be preallocated
     *    NOT_ENOUGH_DATA if calls to prepareNextBuffer are needed to finish
     *        buffer pre-allocation, and transitions to the PREPARING state.
     *    NO_INIT in case of a serious error from the HAL device
     *    INVALID_OPERATION if called when not in CONFIGURED state, or a
     *        valid buffer has already been returned to this stream.
     */
    virtual status_t startPrepare(int maxCount, bool blockRequest) = 0;

    /**
     * Check if the request on a stream is blocked by prepare.
     */
    virtual bool     isBlockedByPrepare() const = 0;

    /**
     * Continue stream buffer preparation by allocating the next
     * buffer for this stream.  May only be called in the PREPARED state.
     *
     * Returns OK and transitions to the CONFIGURED state if all buffers
     * are allocated after the call concludes. Otherwise returns NOT_ENOUGH_DATA.
     *
     * Returns:
     *    OK if no more buffers need to be preallocated, and transitions
     *        to the CONFIGURED state.
     *    NOT_ENOUGH_DATA if more calls to prepareNextBuffer are needed to finish
     *        buffer pre-allocation.
     *    NO_INIT in case of a serious error from the HAL device
     *    INVALID_OPERATION if called when not in CONFIGURED state, or a
     *        valid buffer has already been returned to this stream.
     */
    virtual status_t prepareNextBuffer() = 0;

    /**
     * Cancel stream preparation early. In case allocation needs to be
     * stopped, this method transitions the stream back to the CONFIGURED state.
     * Buffers that have been allocated with prepareNextBuffer remain that way,
     * but a later use of prepareNextBuffer will require just as many
     * calls as if the earlier prepare attempt had not existed.
     *
     * Returns:
     *    OK if cancellation succeeded, and transitions to the CONFIGURED state
     *    INVALID_OPERATION if not in the PREPARING state
     *    NO_INIT in case of a serious error from the HAL device
     */
    virtual status_t cancelPrepare() = 0;

    /**
     * Tear down memory for this stream. This frees all unused gralloc buffers
     * allocated for this stream, but leaves it ready for operation afterward.
     *
     * May only be called in the CONFIGURED state, and keeps the stream in
     * the CONFIGURED state.
     *
     * Returns:
     *    OK if teardown succeeded.
     *    INVALID_OPERATION if not in the CONFIGURED state
     *    NO_INIT in case of a serious error from the HAL device
     */
    virtual status_t tearDown() = 0;

    /**
     * Fill in the camera_stream_buffer with the next valid buffer for this
     * stream, to hand over to the HAL.
     *
     * Multiple surfaces could share the same HAL stream, but a request may
     * be only for a subset of surfaces. In this case, the
     * Camera3StreamInterface object needs the surface ID information to acquire
     * buffers for those surfaces. For the case of single surface for a HAL
     * stream, surface_ids parameter has no effect.
     *
     * This method may only be called once finishConfiguration has been called.
     * For bidirectional streams, this method applies to the output-side
     * buffers.
     *
     */
    virtual status_t getBuffer(camera_stream_buffer *buffer,
            nsecs_t waitBufferTimeout,
            const std::vector<size_t>& surface_ids = std::vector<size_t>()) = 0;

    struct OutstandingBuffer {
        camera_stream_buffer* outBuffer;

        /**
         * Multiple surfaces could share the same HAL stream, but a request may
         * be only for a subset of surfaces. In this case, the
         * Camera3StreamInterface object needs the surface ID information to acquire
         * buffers for those surfaces. For the case of single surface for a HAL
         * stream, surface_ids parameter has no effect.
         */
        std::vector<size_t> surface_ids;
    };

    /**
     * Return a buffer to the stream after use by the HAL.
     *
     * Multiple surfaces could share the same HAL stream, but a request may
     * be only for a subset of surfaces. In this case, the
     * Camera3StreamInterface object needs the surface ID information to attach
     * buffers for those surfaces. For the case of single surface for a HAL
     * stream, surface_ids parameter has no effect.
     *
     * This method may only be called for buffers provided by getBuffer().
     * For bidirectional streams, this method applies to the output-side buffers
     */
    virtual status_t returnBuffer(const camera_stream_buffer &buffer,
            nsecs_t timestamp, nsecs_t readoutTimestamp, bool timestampIncreasing = true,
            const std::vector<size_t>& surface_ids = std::vector<size_t>(),
            uint64_t frameNumber = 0, int32_t transform = -1) = 0;

    /**
     * Fill in the camera_stream_buffer with the next valid buffer for this
     * stream, to hand over to the HAL.
     *
     * This method may only be called once finishConfiguration has been called.
     * For bidirectional streams, this method applies to the input-side
     * buffers.
     *
     * Normally this call will block until the handed out buffer count is less than the stream
     * max buffer count; if respectHalLimit is set to false, this is ignored.
     */
    virtual status_t getInputBuffer(camera_stream_buffer *buffer,
            Size *size, bool respectHalLimit = true) = 0;

    /**
     * Return a buffer to the stream after use by the HAL.
     *
     * This method may only be called for buffers provided by getBuffer().
     * For bidirectional streams, this method applies to the input-side buffers
     */
    virtual status_t returnInputBuffer(const camera_stream_buffer &buffer) = 0;

#if !WB_CAMERA3_AND_PROCESSORS_WITH_DEPENDENCIES
    /**
     * Get the buffer producer of the input buffer queue.
     *
     * This method only applies to input streams.
     */
    virtual status_t getInputBufferProducer(sp<IGraphicBufferProducer> *producer) = 0;
#endif

    /**
     * Whether any of the stream's buffers are currently in use by the HAL,
     * including buffers that have been returned but not yet had their
     * release fence signaled.
     */
    virtual bool     hasOutstandingBuffers() const = 0;

    /**
     * Get number of buffers currently handed out to HAL
     */
    virtual size_t   getOutstandingBuffersCount() const = 0;

    enum {
        TIMEOUT_NEVER = -1
    };

    /**
     * Set the state tracker to use for signaling idle transitions.
     */
    virtual status_t setStatusTracker(sp<StatusTracker> statusTracker) = 0;

    /**
     * Disconnect stream from its non-HAL endpoint. After this,
     * start/finishConfiguration must be called before the stream can be used
     * again. This cannot be called if the stream has outstanding dequeued
     * buffers.
     */
    virtual status_t disconnect() = 0;

    /**
     * Return if the buffer queue of the stream is abandoned.
     */
    virtual bool isAbandoned() const = 0;

    /**
     * Debug dump of the stream's state.
     */
    virtual void     dump(int fd, const Vector<String16> &args) = 0;

    virtual void     addBufferListener(
            wp<Camera3StreamBufferListener> listener) = 0;
    virtual void     removeBufferListener(
            const sp<Camera3StreamBufferListener>& listener) = 0;

    /**
     * Setting listner will remove previous listener (if exists)
     * Only allow set listener during stream configuration because stream is guaranteed to be IDLE
     * at this state, so setBufferFreedListener won't collide with onBufferFreed callbacks.
     * Client is responsible to keep the listener object alive throughout the lifecycle of this
     * Camera3Stream.
     */
    virtual void setBufferFreedListener(wp<Camera3StreamBufferFreedListener> listener) = 0;

    /**
     * Notify buffer stream listeners about incoming request with particular frame number.
     */
    virtual void fireBufferRequestForFrameNumber(uint64_t frameNumber,
            const CameraMetadata& settings) = 0;
};

} // namespace camera3

} // namespace android

#endif
