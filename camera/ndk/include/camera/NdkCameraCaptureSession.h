/*
 * Copyright (C) 2015 The Android Open Source Project
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

/**
 * @addtogroup Camera
 * @{
 */

/**
 * @file NdkCameraCaptureSession.h
 */

/*
 * This file defines an NDK API.
 * Do not remove methods.
 * Do not change method signatures.
 * Do not change the value of constants.
 * Do not change the size of any of the classes defined in here.
 * Do not reference types that are not part of the NDK.
 * Do not #include files that aren't part of the NDK.
 */
#include <sys/cdefs.h>
#include <stdbool.h>

#include "NdkCameraError.h"
#include "NdkCameraMetadata.h"
#include "NdkCaptureRequest.h"
#include "NdkCameraWindowType.h"

#ifndef _NDK_CAMERA_CAPTURE_SESSION_H
#define _NDK_CAMERA_CAPTURE_SESSION_H

__BEGIN_DECLS

/**
 * ACameraCaptureSession is an opaque type that manages frame captures of a camera device.
 *
 * A pointer can be obtained using {@link ACameraDevice_createCaptureSession} method.
 */
typedef struct ACameraCaptureSession ACameraCaptureSession;

/**
 * The definition of camera capture session state callback.
 *
 * @param context The optional application context provided by user in
 *                {@link ACameraCaptureSession_stateCallbacks}.
 * @param session The camera capture session whose state is changing.
 */
typedef void (*ACameraCaptureSession_stateCallback)(void* context, ACameraCaptureSession *session);

/**
 * Capture session state callbacks used in {@link ACameraDevice_createCaptureSession} and
 * {@link ACameraDevice_createCaptureSessionWithSessionParameters}
 */
typedef struct ACameraCaptureSession_stateCallbacks {
    /// optional application context.
    void*                               context;

    /**
     * This callback is called when the session is closed and deleted from memory.
     *
     * <p>A session is closed when {@link ACameraCaptureSession_close} is called, a new session
     * is created by the parent camera device,
     * or when the parent camera device is closed (either by the user closing the device,
     * or due to a camera device disconnection or fatal error).</p>
     *
     * <p>Once this callback is called, all access to this ACameraCaptureSession object will cause
     * a crash.</p>
     */
    ACameraCaptureSession_stateCallback onClosed;

    /**
     * This callback is called every time the session has no more capture requests to process.
     *
     * <p>This callback will be invoked any time the session finishes processing
     * all of its active capture requests, and no repeating request or burst is set up.</p>
     */
    ACameraCaptureSession_stateCallback onReady;

    /**
     * This callback is called when the session starts actively processing capture requests.
     *
     * <p>If the session runs out of capture requests to process and calls {@link onReady},
     * then this callback will be invoked again once new requests are submitted for capture.</p>
     */
    ACameraCaptureSession_stateCallback onActive;
} ACameraCaptureSession_stateCallbacks;

/**
 * The definition of camera capture session onWindowPrepared callback.
 *
 * <p>This callback is called when the buffer pre-allocation for an output window Surface is
 * complete. </p>
 *
 * <p>Buffer pre-allocation for an output window is started by
 * {@link ACameraCaptureSession_prepare}
 * call. While allocation is underway, the output must not be used in a capture request.
 * Once this callback is called, the output provided can be used as a target for a
 * capture request. In case of an error during pre-allocation (such as running out of
 * suitable-memory), this callback is still invoked after the error is encountered, though some
 * buffers may not have been successfully pre-allocated.</p>
 *
 * Introduced in API 34.
 *
 * @param context The optional app-provided context pointer that was included in
 *        the {@link ACameraCaptureSession_setWindowPreparedCallback} method
 *        call.
 * @param window The window that {@link ACameraCaptureSession_prepare} was called on.
 * @param session The camera capture session on which {@link ACameraCaptureSession_prepare} was
 *                called on.
 */
typedef void (*ACameraCaptureSession_prepareCallback)(
        void *context,
        ANativeWindow *window,
        ACameraCaptureSession *session);

/// Enum for describing error reason in {@link ACameraCaptureFailure}
enum {
    /**
     * The capture session has dropped this frame due to an
     * {@link ACameraCaptureSession_abortCaptures} call.
     */
    CAPTURE_FAILURE_REASON_FLUSHED = 0,

    /**
     * The capture session has dropped this frame due to an error in the framework.
     */
    CAPTURE_FAILURE_REASON_ERROR
};

/// Struct to describe a capture failure
typedef struct ACameraCaptureFailure {
    /**
     * The frame number associated with this failed capture.
     *
     * <p>Whenever a request has been processed, regardless of failed capture or success,
     * it gets a unique frame number assigned to its future result/failed capture.</p>
     *
     * <p>This value monotonically increments, starting with 0,
     * for every new result or failure; and the scope is the lifetime of the
     * {@link ACameraDevice}.</p>
     */
    int64_t frameNumber;

    /**
     * Determine why the request was dropped, whether due to an error or to a user
     * action.
     *
     * @see CAPTURE_FAILURE_REASON_ERROR
     * @see CAPTURE_FAILURE_REASON_FLUSHED
     */
    int     reason;

    /**
     * The sequence ID for this failed capture that was returned by the
     * {@link ACameraCaptureSession_capture} or {@link ACameraCaptureSession_setRepeatingRequest}.
     *
     * <p>The sequence ID is a unique monotonically increasing value starting from 0,
     * incremented every time a new group of requests is submitted to the ACameraDevice.</p>
     */
    int     sequenceId;

    /**
     * Determine if the image was captured from the camera.
     *
     * <p>If the image was not captured, no image buffers will be available.
     * If the image was captured, then image buffers may be available.</p>
     *
     */
    bool    wasImageCaptured;
} ACameraCaptureFailure;

/**
 * The definition of camera capture start callback.
 *
 * @param context The optional application context provided by user in
 *                {@link ACameraCaptureSession_captureCallbacks}.
 * @param session The camera capture session of interest.
 * @param request The capture request that is starting. Note that this pointer points to a copy of
 *                capture request sent by application, so the address is different to what
 *                application sent but the content will match. This request will be freed by
 *                framework immediately after this callback returns.
 * @param timestamp The timestamp when the capture is started. This timestamp will match
 *                  {@link ACAMERA_SENSOR_TIMESTAMP} of the {@link ACameraMetadata} in
 *                  {@link ACameraCaptureSession_captureCallbacks#onCaptureCompleted} callback.
 */
typedef void (*ACameraCaptureSession_captureCallback_start)(
        void* context, ACameraCaptureSession* session,
        const ACaptureRequest* request, int64_t timestamp);

/**
 * The definition of camera capture progress/result callback.
 *
 * @param context The optional application context provided by user in
 *                {@link ACameraCaptureSession_captureCallbacks}.
 * @param session The camera capture session of interest.
 * @param request The capture request of interest. Note that this pointer points to a copy of
 *                capture request sent by application, so the address is different to what
 *                application sent but the content will match. This request will be freed by
 *                framework immediately after this callback returns.
 * @param result The capture result metadata reported by camera device. The memory is managed by
 *                camera framework. Do not access this pointer after this callback returns.
 */
typedef void (*ACameraCaptureSession_captureCallback_result)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, const ACameraMetadata* result);

/**
 * The definition of camera capture failure callback.
 *
 * @param context The optional application context provided by user in
 *                {@link ACameraCaptureSession_captureCallbacks}.
 * @param session The camera capture session of interest.
 * @param request The capture request of interest. Note that this pointer points to a copy of
 *                capture request sent by application, so the address is different to what
 *                application sent but the content will match. This request will be freed by
 *                framework immediately after this callback returns.
 * @param failure The {@link ACameraCaptureFailure} describes the capture failure. The memory is
 *                managed by camera framework. Do not access this pointer after this callback
 *                returns.
 */
typedef void (*ACameraCaptureSession_captureCallback_failed)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, ACameraCaptureFailure* failure);

/**
 * The definition of camera sequence end callback.
 *
 * @param context The optional application context provided by user in
 *                {@link ACameraCaptureSession_captureCallbacks}.
 * @param session The camera capture session of interest.
 * @param sequenceId The capture sequence ID of the finished sequence.
 * @param frameNumber The frame number of the last frame of this sequence.
 */
typedef void (*ACameraCaptureSession_captureCallback_sequenceEnd)(
        void* context, ACameraCaptureSession* session,
        int sequenceId, int64_t frameNumber);

/**
 * The definition of camera sequence aborted callback.
 *
 * @param context The optional application context provided by user in
 *                {@link ACameraCaptureSession_captureCallbacks}.
 * @param session The camera capture session of interest.
 * @param sequenceId The capture sequence ID of the aborted sequence.
 */
typedef void (*ACameraCaptureSession_captureCallback_sequenceAbort)(
        void* context, ACameraCaptureSession* session,
        int sequenceId);

/**
 * The definition of camera buffer lost callback.
 *
 * @param context The optional application context provided by user in
 *                {@link ACameraCaptureSession_captureCallbacks}.
 * @param session The camera capture session of interest.
 * @param request The capture request of interest. Note that this pointer points to a copy of
 *                capture request sent by application, so the address is different to what
 *                application sent but the content will match. This request will be freed by
 *                framework immediately after this callback returns.
 * @param window The {@link ANativeWindow} that the lost buffer would have been sent to.
 * @param frameNumber The frame number of the lost buffer.
 */
typedef void (*ACameraCaptureSession_captureCallback_bufferLost)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, ANativeWindow* window, int64_t frameNumber);

/**
 * ACaptureCaptureSession_captureCallbacks structure used in
 * {@link ACameraCaptureSession_capture} and {@link ACameraCaptureSession_setRepeatingRequest}.
 */
typedef struct ACameraCaptureSession_captureCallbacks {
    /// optional application context.
    void*                                               context;

    /**
     * This callback is called when the camera device has started capturing
     * the output image for the request, at the beginning of image exposure.
     *
     * <p>This callback is invoked right as
     * the capture of a frame begins, so it is the most appropriate time
     * for playing a shutter sound, or triggering UI indicators of capture.</p>
     *
     * <p>The request that is being used for this capture is provided, along
     * with the actual timestamp for the start of exposure.
     * This timestamp matches the timestamps that will be
     * included in {@link ACAMERA_SENSOR_TIMESTAMP} of the {@link ACameraMetadata} in
     * {@link onCaptureCompleted} callback,
     * and in the buffers sent to each output ANativeWindow. These buffer
     * timestamps are accessible through, for example,
     * {@link AImage_getTimestamp} or
     * <a href="http://developer.android.com/reference/android/graphics/SurfaceTexture.html#getTimestamp()">
     * android.graphics.SurfaceTexture#getTimestamp()</a>.</p>
     *
     * <p>Note that the ACaptureRequest pointer in the callback will not match what application has
     * submitted, but the contents the ACaptureRequest will match what application submitted.</p>
     *
     */
    ACameraCaptureSession_captureCallback_start         onCaptureStarted;

    /**
     * This callback is called when an image capture makes partial forward progress; some
     * (but not all) results from an image capture are available.
     *
     * <p>The result provided here will contain some subset of the fields of
     * a full result. Multiple {@link onCaptureProgressed} calls may happen per
     * capture; a given result field will only be present in one partial
     * capture at most. The final {@link onCaptureCompleted} call will always
     * contain all the fields (in particular, the union of all the fields of all
     * the partial results composing the total result).</p>
     *
     * <p>For each request, some result data might be available earlier than others. The typical
     * delay between each partial result (per request) is a single frame interval.
     * For performance-oriented use-cases, applications should query the metadata they need
     * to make forward progress from the partial results and avoid waiting for the completed
     * result.</p>
     *
     * <p>For a particular request, {@link onCaptureProgressed} may happen before or after
     * {@link onCaptureStarted}.</p>
     *
     * <p>Each request will generate at least `1` partial results, and at most
     * {@link ACAMERA_REQUEST_PARTIAL_RESULT_COUNT} partial results.</p>
     *
     * <p>Depending on the request settings, the number of partial results per request
     * will vary, although typically the partial count could be the same as long as the
     * camera device subsystems enabled stay the same.</p>
     *
     * <p>Note that the ACaptureRequest pointer in the callback will not match what application has
     * submitted, but the contents the ACaptureRequest will match what application submitted.</p>
     */
    ACameraCaptureSession_captureCallback_result        onCaptureProgressed;

    /**
     * This callback is called when an image capture has fully completed and all the
     * result metadata is available.
     *
     * <p>This callback will always fire after the last {@link onCaptureProgressed};
     * in other words, no more partial results will be delivered once the completed result
     * is available.</p>
     *
     * <p>For performance-intensive use-cases where latency is a factor, consider
     * using {@link onCaptureProgressed} instead.</p>
     *
     * <p>Note that the ACaptureRequest pointer in the callback will not match what application has
     * submitted, but the contents the ACaptureRequest will match what application submitted.</p>
     */
    ACameraCaptureSession_captureCallback_result        onCaptureCompleted;

    /**
     * This callback is called instead of {@link onCaptureCompleted} when the
     * camera device failed to produce a capture result for the
     * request.
     *
     * <p>Other requests are unaffected, and some or all image buffers from
     * the capture may have been pushed to their respective output
     * streams.</p>
     *
     * <p>Note that the ACaptureRequest pointer in the callback will not match what application has
     * submitted, but the contents the ACaptureRequest will match what application submitted.</p>
     *
     * @see ACameraCaptureFailure
     */
    ACameraCaptureSession_captureCallback_failed        onCaptureFailed;

    /**
     * This callback is called independently of the others in {@link ACameraCaptureSession_captureCallbacks},
     * when a capture sequence finishes and all capture result
     * or capture failure for it have been returned via this {@link ACameraCaptureSession_captureCallbacks}.
     *
     * <p>In total, there will be at least one result/failure returned by this listener
     * before this callback is invoked. If the capture sequence is aborted before any
     * requests have been processed, {@link onCaptureSequenceAborted} is invoked instead.</p>
     */
    ACameraCaptureSession_captureCallback_sequenceEnd   onCaptureSequenceCompleted;

    /**
     * This callback is called independently of the others in {@link ACameraCaptureSession_captureCallbacks},
     * when a capture sequence aborts before any capture result
     * or capture failure for it have been returned via this {@link ACameraCaptureSession_captureCallbacks}.
     *
     * <p>Due to the asynchronous nature of the camera device, not all submitted captures
     * are immediately processed. It is possible to clear out the pending requests
     * by a variety of operations such as {@link ACameraCaptureSession_stopRepeating} or
     * {@link ACameraCaptureSession_abortCaptures}. When such an event happens,
     * {@link onCaptureSequenceCompleted} will not be called.</p>
     */
    ACameraCaptureSession_captureCallback_sequenceAbort onCaptureSequenceAborted;

    /**
     * This callback is called if a single buffer for a capture could not be sent to its
     * destination ANativeWindow.
     *
     * <p>If the whole capture failed, then {@link onCaptureFailed} will be called instead. If
     * some but not all buffers were captured but the result metadata will not be available,
     * then onCaptureFailed will be invoked with {@link ACameraCaptureFailure#wasImageCaptured}
     * returning true, along with one or more calls to {@link onCaptureBufferLost} for the
     * failed outputs.</p>
     *
     * <p>Note that the ACaptureRequest pointer in the callback will not match what application has
     * submitted, but the contents the ACaptureRequest will match what application submitted.
     * The ANativeWindow pointer will always match what application submitted in
     * {@link ACameraDevice_createCaptureSession}</p>
     *
     */
    ACameraCaptureSession_captureCallback_bufferLost    onCaptureBufferLost;
} ACameraCaptureSession_captureCallbacks;

enum {
    CAPTURE_SEQUENCE_ID_NONE = -1
};

/**
 * Close this capture session.
 *
 * <p>Closing a session frees up the target output Surfaces of the session for reuse with either
 * a new session, or to other APIs that can draw to Surfaces.</p>
 *
 * <p>Note that creating a new capture session with {@link ACameraDevice_createCaptureSession}
 * will close any existing capture session automatically, and call the older session listener's
 * {@link ACameraCaptureSession_stateCallbacks#onClosed} callback. Using
 * {@link ACameraDevice_createCaptureSession} directly without closing is the recommended approach
 * for quickly switching to a new session, since unchanged target outputs can be reused more
 * efficiently.</p>
 *
 * <p>After a session is closed and before {@link ACameraCaptureSession_stateCallbacks#onClosed}
 * is called, all methods invoked on the session will return {@link ACAMERA_ERROR_SESSION_CLOSED},
 * and any repeating requests are stopped (as if {@link ACameraCaptureSession_stopRepeating} was
 * called). However, any in-progress capture requests submitted to the session will be completed as
 * normal; once all captures have completed and the session has been torn down,
 * {@link ACameraCaptureSession_stateCallbacks#onClosed} callback will be called and the session
 * will be removed from memory.</p>
 *
 * <p>Closing a session is idempotent; closing more than once has no effect.</p>
 *
 * @param session the capture session of interest
 */
void ACameraCaptureSession_close(ACameraCaptureSession* session);

/**
 * ACameraDevice is opaque type that provides access to a camera device.
 * A pointer can be obtained using {@link ACameraManager_openCamera} method.
 */
typedef struct ACameraDevice ACameraDevice;

/**
 * Get the ACameraDevice pointer associated with this capture session in the device argument
 * if the method succeeds.
 *
 * @param session the capture session of interest
 * @param device the {@link ACameraDevice} associated with session. Will be set to NULL
 *        if the session is closed or this method fails.
 * @return <ul><li>
 *             {@link ACAMERA_OK} if the method call succeeds. The {@link ACameraDevice}
 *                                will be stored in device argument</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session or device is NULL</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for some other reasons</li></ul>
 *
 */
camera_status_t ACameraCaptureSession_getDevice(
        ACameraCaptureSession* session, /*out*/ACameraDevice** device) __INTRODUCED_IN(24);

/**
 * Submit an array of requests to be captured in sequence as a burst in the minimum of time possible.
 *
 * <p>The burst will be captured in the minimum amount of time possible, and will not be
 * interleaved with requests submitted by other capture or repeat calls.</p>
 *
 * <p>Each capture produces one {@link ACameraMetadata} as a capture result and image buffers for
 * one or more target {@link ANativeWindow}s. The target ANativeWindows (set with
 * {@link ACaptureRequest_addTarget}) must be a subset of the ANativeWindow provided when
 * this capture session was created.</p>
 *
 * @param session the capture session of interest
 * @param callbacks the {@link ACameraCaptureSession_captureCallbacks} to be associated this capture
 *        sequence. No capture callback will be fired if this is set to NULL.
 * @param numRequests number of requests in requests argument. Must be at least 1.
 * @param requests an array of {@link ACaptureRequest} to be captured. Length must be at least
 *        numRequests.
 * @param captureSequenceId the capture sequence ID associated with this capture method invocation
 *        will be stored here if this argument is not NULL and the method call succeeds.
 *        When this argument is set to NULL, the capture sequence ID will not be returned.
 *
 * @return <ul><li>
 *             {@link ACAMERA_OK} if the method succeeds. captureSequenceId will be filled
 *             if it is not NULL.</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session or requests is NULL, or
 *             if numRequests < 1</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DISCONNECTED} if the camera device is closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DEVICE} if the camera device encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_SERVICE} if the camera service encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for some other reasons</li></ul>
 */
camera_status_t ACameraCaptureSession_capture(
        ACameraCaptureSession* session,
        /*optional*/ACameraCaptureSession_captureCallbacks* callbacks,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId) __INTRODUCED_IN(24);

/**
 * Request endlessly repeating capture of a sequence of images by this capture session.
 *
 * <p>With this method, the camera device will continually capture images,
 * cycling through the settings in the provided list of
 * {@link ACaptureRequest}, at the maximum rate possible.</p>
 *
 * <p>If a request is submitted through {@link ACameraCaptureSession_capture},
 * the current repetition of the request list will be
 * completed before the higher-priority request is handled. This guarantees
 * that the application always receives a complete repeat burst captured in
 * minimal time, instead of bursts interleaved with higher-priority
 * captures, or incomplete captures.</p>
 *
 * <p>Repeating burst requests are a simple way for an application to
 * maintain a preview or other continuous stream of frames where each
 * request is different in a predictable way, without having to continually
 * submit requests through {@link ACameraCaptureSession_capture}.</p>
 *
 * <p>To stop the repeating capture, call {@link ACameraCaptureSession_stopRepeating}. Any
 * ongoing burst will still be completed, however. Calling
 * {@link ACameraCaptureSession_abortCaptures} will also clear the request.</p>
 *
 * <p>Calling this method will replace a previously-set repeating requests
 * set up by this method, although any in-progress burst will be completed before the new repeat
 * burst will be used.</p>
 *
 * @param session the capture session of interest
 * @param callbacks the {@link ACameraCaptureSession_captureCallbacks} to be associated with this
 *        capture sequence. No capture callback will be fired if callbacks is set to NULL.
 * @param numRequests number of requests in requests array. Must be at least 1.
 * @param requests an array of {@link ACaptureRequest} to be captured. Length must be at least
 *        numRequests.
 * @param captureSequenceId the capture sequence ID associated with this capture method invocation
 *        will be stored here if this argument is not NULL and the method call succeeds.
 *        When this argument is set to NULL, the capture sequence ID will not be returned.
 *
 * @return <ul><li>
 *             {@link ACAMERA_OK} if the method succeeds. captureSequenceId will be filled
 *             if it is not NULL.</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session or requests is NULL, or
 *             if numRequests < 1</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DISCONNECTED} if the camera device is closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DEVICE} if the camera device encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_SERVICE} if the camera service encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for  some other reasons</li></ul>
 */
camera_status_t ACameraCaptureSession_setRepeatingRequest(
        ACameraCaptureSession* session,
        /*optional*/ACameraCaptureSession_captureCallbacks* callbacks,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId) __INTRODUCED_IN(24);

/**
 * Cancel any ongoing repeating capture set by {@link ACameraCaptureSession_setRepeatingRequest}.
 * Has no effect on requests submitted through {@link ACameraCaptureSession_capture}.
 *
 * <p>Any currently in-flight captures will still complete, as will any burst that is
 * mid-capture. To ensure that the device has finished processing all of its capture requests
 * and is in ready state, wait for the {@link ACameraCaptureSession_stateCallbacks#onReady} callback
 * after calling this method.</p>
 *
 * @param session the capture session of interest
 *
 * @return <ul><li>{@link ACAMERA_OK} if the method succeeds.</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session is NULL.</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DISCONNECTED} if the camera device is closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DEVICE} if the camera device encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_SERVICE} if the camera service encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for some other reasons</li></ul>
 */
camera_status_t ACameraCaptureSession_stopRepeating(ACameraCaptureSession* session)
        __INTRODUCED_IN(24);

/**
 * Discard all captures currently pending and in-progress as fast as possible.
 *
 * <p>The camera device will discard all of its current work as fast as possible. Some in-flight
 * captures may complete successfully and call
 * {@link ACameraCaptureSession_captureCallbacks#onCaptureCompleted},
 * while others will trigger their {@link ACameraCaptureSession_captureCallbacks#onCaptureFailed}
 * callbacks. If a repeating request list is set, it will be cleared.</p>
 *
 * <p>This method is the fastest way to switch the camera device to a new session with
 * {@link ACameraDevice_createCaptureSession}, at the cost of discarding in-progress
 * work. It must be called before the new session is created. Once all pending requests are
 * either completed or thrown away, the {@link ACameraCaptureSession_stateCallbacks#onReady}
 * callback will be called, if the session has not been closed. Otherwise, the
 * {@link ACameraCaptureSession_stateCallbacks#onClosed}
 * callback will be fired when a new session is created by the camera device and the previous
 * session is being removed from memory.</p>
 *
 * <p>Cancelling will introduce at least a brief pause in the stream of data from the camera
 * device, since once the camera device is emptied, the first new request has to make it through
 * the entire camera pipeline before new output buffers are produced.</p>
 *
 * <p>This means that using ACameraCaptureSession_abortCaptures to simply remove pending requests is
 * not recommended; it's best used for quickly switching output configurations, or for cancelling
 * long in-progress requests (such as a multi-second capture).</p>
 *
 * @param session the capture session of interest
 *
 * @return <ul><li> {@link ACAMERA_OK} if the method succeeds</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session is NULL.</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DISCONNECTED} if the camera device is closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DEVICE} if the camera device encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_SERVICE} if the camera service encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for some other reasons</li></ul>
 */
camera_status_t ACameraCaptureSession_abortCaptures(ACameraCaptureSession* session)
        __INTRODUCED_IN(24);

/**
 * Opaque object for capture session output, use {@link ACaptureSessionOutput_create} or
 * {@link ACaptureSessionSharedOutput_create} to create an instance.
 */
typedef struct ACaptureSessionOutput ACaptureSessionOutput;

/**
 * Update shared ACaptureSessionOutput.
 *
 * <p>A shared ACaptureSessionOutput (see {@link ACaptureSessionSharedOutput_create}) that
 * was modified via calls to {@link ACaptureSessionSharedOutput_add} or
 * {@link ACaptureSessionSharedOutput_remove} must be updated by calling this method before its
 * changes take effect. After the update call returns  with {@link ACAMERA_OK}, any newly added
 * native windows can be used as a target in subsequent capture requests.</p>
 *
 * <p>Native windows that get removed must not be part of any active repeating or single/burst
 * request or have any pending results. Consider updating repeating requests via
 * {@link ACameraCaptureSession_setRepeatingRequest} and then wait for the last frame number
 * when the sequence completes
 * {@link ACameraCaptureSession_captureCallbacks#onCaptureSequenceCompleted}.</p>
 *
 * <p>Native windows that get added must not be part of any other registered ACaptureSessionOutput
 * and must be compatible. Compatible windows must have matching format, rotation and
 * consumer usage.</p>
 *
 * <p>A shared ACameraCaptureSession can support up to 4 additional native windows.</p>
 *
 * @param session the capture session of interest
 * @param output the modified output configuration
 *
 * @return <ul><li>
 *             {@link ACAMERA_OK} if the method succeeds.</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session or output is NULL; or output
 *             contains invalid native windows; or if an attempt was made to add
 *             a native window to a different output configuration; or new native window is not
 *             compatible; or any removed native window still has pending requests;</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_OPERATION} if output configuration is not shared (see
 *             {@link ACaptureSessionSharedOutput_create};  or the number of additional
 *             native windows goes beyond the supported limit.</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DISCONNECTED} if the camera device is closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DEVICE} if the camera device encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_SERVICE} if the camera service encounters fatal
 *             error</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for some other reasons</li></ul>
 */
camera_status_t ACameraCaptureSession_updateSharedOutput(ACameraCaptureSession* session,
        ACaptureSessionOutput* output) __INTRODUCED_IN(28);

/**
 * The definition of final capture result callback with logical multi-camera support.
 *
 * This has the same functionality as final ACameraCaptureSession_captureCallback_result, with
 * added ability to return physical camera result metadata within a logical multi-camera.
 *
 * For a logical multi-camera, this function will be called with the Id and result metadata
 * of the underlying physical cameras, which the corresponding capture request contains targets for.
 * If the capture request doesn't contain targets specific to any physical camera, or the current
 * camera device isn't a logical multi-camera, physicalResultCount will be 0.
 *
 * @param context The optional application context provided by user in
 *                {@link ACameraCaptureSession_captureCallbacks}.
 * @param session The camera capture session of interest.
 * @param request The capture request of interest. Note that this pointer points to a copy of
 *                capture request sent by application, so the address is different to what
 *                application sent but the content will match. This request will be freed by
 *                framework immediately after this callback returns.
 * @param result The capture result metadata reported by camera device. The memory is managed by
 *                camera framework. Do not access this pointer after this callback returns.
 * @param physicalResultCount The number of physical camera result metadata
 * @param physicalCameraIds The array of physical camera IDs on which the
 *                physical result metadata are reported.
 * @param physicalResults The array of capture result metadata reported by the
 *                physical camera devices.
 */
typedef void (*ACameraCaptureSession_logicalCamera_captureCallback_result)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, const ACameraMetadata* result,
        size_t physicalResultCount, const char** physicalCameraIds,
        const ACameraMetadata** physicalResults);

/// Struct to describe a logical camera capture failure
typedef struct ALogicalCameraCaptureFailure {
    /**
     * The {@link ACameraCaptureFailure} contains information about regular logical device capture
     * failure.
     */
    struct ACameraCaptureFailure captureFailure;

    /**
     * The physical camera device ID in case the capture failure comes from a capture request
     * with configured physical camera streams for a logical camera. physicalCameraId will be set
     * to NULL in case the capture request has no associated physical camera device.
     *
     */
    const char*    physicalCameraId;
} ALogicalCameraCaptureFailure;

/**
 * The definition of logical camera capture failure callback.
 *
 * @param context The optional application context provided by user in
 *                {@link ACameraCaptureSession_captureCallbacks}.
 * @param session The camera capture session of interest.
 * @param request The capture request of interest. Note that this pointer points to a copy of
 *                capture request sent by application, so the address is different to what
 *                application sent but the content will match. This request will be freed by
 *                framework immediately after this callback returns.
 * @param failure The {@link ALogicalCameraCaptureFailure} describes the capture failure. The memory
 *                is managed by camera framework. Do not access this pointer after this callback
 *                returns.
 */
typedef void (*ACameraCaptureSession_logicalCamera_captureCallback_failed)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, ALogicalCameraCaptureFailure* failure);

/**
 * This has the same functionality as ACameraCaptureSession_captureCallbacks,
 * with the exception that an onLogicalCameraCaptureCompleted callback is
 * used, instead of onCaptureCompleted, to support logical multi-camera.
 */
typedef struct ACameraCaptureSession_logicalCamera_captureCallbacks {
    /**
     * Same as ACameraCaptureSession_captureCallbacks
     */
    void*                                               context;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureStarted}.
     */
    ACameraCaptureSession_captureCallback_start         onCaptureStarted;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureProgressed}.
     */
    ACameraCaptureSession_captureCallback_result        onCaptureProgressed;

    /**
     * This callback is called when an image capture has fully completed and all the
     * result metadata is available. For a logical multi-camera, this callback
     * also returns the result metadata for all physical cameras being
     * explicitly requested on.
     *
     * <p>This callback will always fire after the last {@link onCaptureProgressed};
     * in other words, no more partial results will be delivered once the completed result
     * is available.</p>
     *
     * <p>For performance-intensive use-cases where latency is a factor, consider
     * using {@link onCaptureProgressed} instead.</p>
     *
     * <p>Note that the ACaptureRequest pointer in the callback will not match what application has
     * submitted, but the contents the ACaptureRequest will match what application submitted.</p>
     */
    ACameraCaptureSession_logicalCamera_captureCallback_result onLogicalCameraCaptureCompleted;

    /**
     * This callback is called instead of {@link onLogicalCameraCaptureCompleted} when the
     * camera device failed to produce a capture result for the
     * request.
     *
     * <p>Other requests are unaffected, and some or all image buffers from
     * the capture may have been pushed to their respective output
     * streams.</p>
     *
     * <p>Note that the ACaptureRequest pointer in the callback will not match what application has
     * submitted, but the contents the ACaptureRequest will match what application submitted.</p>
     *
     * @see ALogicalCameraCaptureFailure
     */
    ACameraCaptureSession_logicalCamera_captureCallback_failed onLogicalCameraCaptureFailed;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureSequenceCompleted}.
     */
    ACameraCaptureSession_captureCallback_sequenceEnd   onCaptureSequenceCompleted;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureSequenceAborted}.
     */
    ACameraCaptureSession_captureCallback_sequenceAbort onCaptureSequenceAborted;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureBufferLost}.
     */
    ACameraCaptureSession_captureCallback_bufferLost    onCaptureBufferLost;
} ACameraCaptureSession_logicalCamera_captureCallbacks;

/**
 * This has the same functionality as ACameraCaptureSession_capture, with added
 * support for logical multi-camera where the capture callbacks supports result metadata for
 * physical cameras.
 */
camera_status_t ACameraCaptureSession_logicalCamera_capture(
        ACameraCaptureSession* session,
        /*optional*/ACameraCaptureSession_logicalCamera_captureCallbacks* callbacks,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId) __INTRODUCED_IN(29);

/**
 * This has the same functionality as ACameraCaptureSession_setRepeatingRequest, with added
 * support for logical multi-camera where the capture callbacks supports result metadata for
 * physical cameras.
 */
camera_status_t ACameraCaptureSession_logicalCamera_setRepeatingRequest(
        ACameraCaptureSession* session,
        /*optional*/ACameraCaptureSession_logicalCamera_captureCallbacks* callbacks,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId) __INTRODUCED_IN(29);

/**
 * The definition of camera capture start callback. The same as
 * {@link ACameraCaptureSession_captureCallbacks#onCaptureStarted}, except that
 * it has the frame number of the capture as well.
 *
 * @param context The optional application context provided by user in
 *                {@link ACameraCaptureSession_captureCallbacks}.
 * @param session The camera capture session of interest.
 * @param request The capture request that is starting. Note that this pointer points to a copy of
 *                capture request sent by application, so the address is different to what
 *                application sent but the content will match. This request will be freed by
 *                framework immediately after this callback returns.
 * @param timestamp The timestamp when the capture is started. This timestamp will match
 *                  {@link ACAMERA_SENSOR_TIMESTAMP} of the {@link ACameraMetadata} in
 *                  {@link ACameraCaptureSession_captureCallbacks#onCaptureCompleted} callback.
 * @param frameNumber the frame number of the capture started
 */
typedef void (*ACameraCaptureSession_captureCallback_startV2)(
        void* context, ACameraCaptureSession* session,
        const ACaptureRequest* request, int64_t timestamp, int64_t frameNumber);
/**
 * This has the same functionality as ACameraCaptureSession_captureCallbacks,
 * with the exception that captureCallback_startV2 callback is
 * used, instead of captureCallback_start, to support retrieving the frame number.
 */
typedef struct ACameraCaptureSession_captureCallbacksV2 {
    /**
     * Same as ACameraCaptureSession_captureCallbacks
     */
    void*                                               context;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureStarted},
     * except that it has the frame number of the capture added in the parameter
     * list.
     */
    ACameraCaptureSession_captureCallback_startV2         onCaptureStarted;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureProgressed}.
     */
    ACameraCaptureSession_captureCallback_result        onCaptureProgressed;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureCompleted}.
     */
    ACameraCaptureSession_captureCallback_result        onCaptureCompleted;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureFailed}.
     */
    ACameraCaptureSession_captureCallback_failed        onCaptureFailed;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureSequenceCompleted}.
     */
    ACameraCaptureSession_captureCallback_sequenceEnd   onCaptureSequenceCompleted;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureSequenceAborted}.
     */
    ACameraCaptureSession_captureCallback_sequenceAbort onCaptureSequenceAborted;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureBufferLost}.
     */
    ACameraCaptureSession_captureCallback_bufferLost    onCaptureBufferLost;


} ACameraCaptureSession_captureCallbacksV2;

/**
 * This has the same functionality as ACameraCaptureSession_logicalCamera_captureCallbacks,
 * with the exception that an captureCallback_startV2 callback is
 * used, instead of captureCallback_start, to support retrieving frame number.
 */
typedef struct ACameraCaptureSession_logicalCamera_captureCallbacksV2 {
    /**
     * Same as ACameraCaptureSession_captureCallbacks
     */
    void*                                               context;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureStarted},
     * except that it has the frame number of the capture added in the parameter
     * list.
     */
    ACameraCaptureSession_captureCallback_startV2         onCaptureStarted;


    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureProgressed}.
     */
    ACameraCaptureSession_captureCallback_result        onCaptureProgressed;

    /**
     * Same as
     * {@link ACameraCaptureSession_logicalCamera_captureCallbacks#onLogicalCaptureCompleted}.
     */
    ACameraCaptureSession_logicalCamera_captureCallback_result onLogicalCameraCaptureCompleted;

    /**
     * This callback is called instead of {@link onLogicalCameraCaptureCompleted} when the
     * camera device failed to produce a capture result for the
     * request.
     *
     * <p>Other requests are unaffected, and some or all image buffers from
     * the capture may have been pushed to their respective output
     * streams.</p>
     *
     * <p>Note that the ACaptureRequest pointer in the callback will not match what application has
     * submitted, but the contents the ACaptureRequest will match what application submitted.</p>
     *
     * @see ALogicalCameraCaptureFailure
     */
    ACameraCaptureSession_logicalCamera_captureCallback_failed onLogicalCameraCaptureFailed;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureSequenceCompleted}.
     */
    ACameraCaptureSession_captureCallback_sequenceEnd   onCaptureSequenceCompleted;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureSequenceAborted}.
     */
    ACameraCaptureSession_captureCallback_sequenceAbort onCaptureSequenceAborted;

    /**
     * Same as {@link ACameraCaptureSession_captureCallbacks#onCaptureBufferLost}.
     */
    ACameraCaptureSession_captureCallback_bufferLost    onCaptureBufferLost;

} ACameraCaptureSession_logicalCamera_captureCallbacksV2;

/**
 * This has the same functionality as ACameraCaptureSession_capture, with added
 * support for v2 of camera callbacks, where the onCaptureStarted callback
 * adds frame number in its parameter list.
 */
camera_status_t ACameraCaptureSession_captureV2(
        ACameraCaptureSession* session,
        /*optional*/ACameraCaptureSession_captureCallbacksV2* callbacks,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId) __INTRODUCED_IN(33);

/**
 * This has the same functionality as ACameraCaptureSession_logical_setRepeatingRequest, with added
 * support for v2 of logical multi-camera callbacks where the onCaptureStarted
 * callback adds frame number in its parameter list.
 */
camera_status_t ACameraCaptureSession_setRepeatingRequestV2(
        ACameraCaptureSession* session,
        /*optional*/ACameraCaptureSession_captureCallbacksV2* callbacks,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId) __INTRODUCED_IN(33);

/**
 * This has the same functionality as ACameraCaptureSession_logical_capture, with added
 * support for v2 of logical multi-camera  callbacks where the onCaptureStarted callback
 * adds frame number in its parameter list.
 */
camera_status_t ACameraCaptureSession_logicalCamera_captureV2(
        ACameraCaptureSession* session,
        /*optional*/ACameraCaptureSession_logicalCamera_captureCallbacksV2* callbacks,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId) __INTRODUCED_IN(33);

/**
 * This has the same functionality as ACameraCaptureSession_logical_setRepeatingRequest, with added
 * support for v2 of logical multi-camera callbacks where the onCaptureStarted
 * callback adds frame number in its parameter list.
 */
camera_status_t ACameraCaptureSession_logicalCamera_setRepeatingRequestV2(
        ACameraCaptureSession* session,
        /*optional*/ACameraCaptureSession_logicalCamera_captureCallbacksV2* callbacks,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId) __INTRODUCED_IN(33);

/**
 * Set the callback that is called when the output window for which the client has requested
 * pre-allocation of buffers through the {@link ACameraCaptureSession_prepareWindow} call has
 * completed the pre-allocation of buffers.
 * @param session the ACameraCaptureSession on which ACameraCaptureSession_prepareWindow was called.
 * @param context optional application provided context. This will be passed into the context
 *        parameter of the {@link onWindowPrepared} callback.
 * @param callback the callback to be called when the output window's buffer pre-allocation is
 *        complete.
 * @return <ul><li> {@link ACAMERA_OK} if the method succeeds</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session or callbacks is
 *              NULL. Or if the session has not been configured with the window</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DISCONNECTED} if the camera device is closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DEVICE} if the camera device encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_SERVICE} the camera service encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for some other reasons</li></ul>
 */
camera_status_t ACameraCaptureSession_setWindowPreparedCallback(
    ACameraCaptureSession* session,
    void *context,
    ACameraCaptureSession_prepareCallback callback) __INTRODUCED_IN(34);

/**
 *
 * <p>Pre-allocate all buffers for an output window.</p>
 *
 * <p>Normally, the image buffers for a given output window are allocated on-demand,
 * to minimize startup latency and memory overhead.</p>
 *
 * <p>However, in some cases, it may be desirable for the buffers to be allocated before
 * any requests targeting the window are actually submitted to the device. Large buffers
 * may take some time to allocate, which can result in delays in submitting requests until
 * sufficient buffers are allocated to reach steady-state behavior. Such delays can cause
 * bursts to take longer than desired, or cause skips or stutters in preview output.</p>
 *
 * <p>The ACameraCaptureSession_prepare() call can be used to perform this pre-allocation.
 * It may only be called for a given output window before that window is used as a target for a
 * request. The number of buffers allocated is the sum of the count needed by the consumer providing
 * the output window, and the maximum number needed by the camera device to fill its pipeline.
 * Since this may be a larger number than what is actually required for steady-state operation,
 * using this call may result in higher memory consumption than the normal on-demand behavior
 * results in. This method will also delay the time to first output to a given Surface, in exchange
 * for smoother frame rate once the allocation is complete.</p>
 *
 * <p>For example, an application that creates an
 * {@link AImageReader} with a maxImages argument of 10,
 * but only uses 3 simultaneous {@link AImage}s at once, would normally only cause those 3 images
 * to be allocated (plus what is needed by the camera device for smooth operation).  But using
 * ACameraCaptureSession_prepare() on the {@link AImageReader}'s window will result in all 10
 * {@link AImage}s being allocated. So applications using this method should take care to request
 * only the number of buffers actually necessary for their application.</p>
 *
 * <p>If the same output window is used in consecutive sessions (without closing the first
 * session explicitly), then its already-allocated buffers are carried over, and if it was
 * used as a target of a capture request in the first session, prepare cannot be called on it
 * in the second session. If it is, {@link ACAMERA_ERROR_INVALID_PARAMETER} will
 * be returned by the method</p>
 *
 * <p>Once allocation is complete, {@link ACameraCaptureSession_prepareCallback#onWindowPrepared}
 * will be invoked with the output provided to this method. Between the prepare call and the
 * {@link ACameraCaptureSession_prepareCallback#onWindowPrepared} call,
 * the output provided to prepare must not be used as a target of a capture request submitted
 * to this session.</p>
 *
 * <p>{@link android.hardware.camera2.CameraCharacteristics#INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY LEGACY}
 * devices cannot pre-allocate output buffers; for those devices,
 * {@link ACameraCaptureSession_prepareCallback#onWindowPrepared} will be immediately called,
 * and no pre-allocation is done.</p>
 *
 * @param session the {@link ACameraCaptureSession} that needs to prepare output buffers.
 * @param window the {@link ANativeWindow} for which the output buffers need to be prepared.
 *
 * @return <ul><li>
 *             {@link ACAMERA_OK} if the method succeeds</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session/ window is
 *              NULL. Or if the session has not been configured with the window</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DISCONNECTED} if the camera device is closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DEVICE} if the camera device encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_SERVICE} if the camera service encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for some other reasons</li></ul>
 */
camera_status_t ACameraCaptureSession_prepareWindow(
    ACameraCaptureSession* session,
    ANativeWindow *window) __INTRODUCED_IN(34);

/**
 * Request continuous streaming of a sequence of images for the shared capture session
 * when more than one clients can open the same camera in shared mode by calling
 * {@link ACameraManager_openSharedCamera}. In shared mode, the highest priority client among all
 * the clients will be the primary client while the others would be secondary clients. In shared
 * capture session, only primary clients can create a capture request and change capture parameters.
 * Secondary clients can only request streaming of images by calling this api
 * {@link ACameraCaptureSessionShared_startStreaming}. Calling this api for normal sessions when
 * {@link ACameraManager_openCamera} is used to open the camera will throw
 * {@link ACAMERA_ERROR_INVALID_OPERATION}.
 *
 * <p>The priority of client access is determined by considering two factors: its current process
 * state and its "out of memory" score. Clients operating in the background are assigned a lower
 * priority. In contrast, clients running in the foreground, along with system-level clients, are
 * given a higher priority.</p>
 *
 * <p>With this method, the camera device will continually capture images, cycling through the
 * settings in the list of {@link ACaptureRequest} specified by the primary client. If primary
 * client does not have ongoing repeating request, camera service will use a capture request with
 * default capture parameters for preview template.</p>
 *
 * <p>To stop the continuous streaming, call {@link ACameraCaptureSessionShared_stopStreaming}.</p>
 *
 * <p>Calling this method will replace an existing continuous streaming request.</p>
 *
 * @param sharedSession the shared capture session when camera is opened in
 *        shared mode.
 * @param callbacks the {@link ACameraCaptureSession_captureCallbacks} to be associated with this
 *        capture sequence. No capture callback will be fired if callbacks is set to NULL.
 * @param numOutputWindows number of native windows to be used for streaming. Must be at least 1.
 * @param windows an array of {@link ANativeWindow} to be used for streaming. Length must be at
 *        least numOutputWindows.
 * @param captureSequenceId the capture sequence ID associated with this capture method invocation
 *        will be stored here if this argument is not NULL and the method call succeeds.
 *        When this argument is set to NULL, the capture sequence ID will not be returned.
 *
 * @return <ul>
 *         <li>{@link ACAMERA_OK} if the method succeeds. captureSequenceId will be filled
 *             if it is not NULL.</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session or requests is NULL, or
 *             if numRequests < 1</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DISCONNECTED} if the camera device is closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DEVICE} if the camera device encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_SERVICE} if the camera service encounters fatal error
 *         </li>
 *         <li>{@link ACAMERA_ERROR_INVALID_OPERATION} if the session passed is not a shared
 *              session</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for  some other reasons</li>
 *         </ul>
 */
camera_status_t ACameraCaptureSessionShared_startStreaming(
    ACameraCaptureSession* sharedSession,
    /*optional*/ACameraCaptureSession_captureCallbacksV2 *callbacks,
    int numOutputWindows, ANativeWindow **window,
    /*optional*/int *captureSequenceId) __INTRODUCED_IN(36);

/**
 * This has the same functionality as ACameraCaptureSessionShared_startStreaming, with added
 * support for logical multi-camera where the capture callbacks supports result metadata for
 * physical cameras.
 *
 * Request continuous streaming of a sequence of images for the shared capture session
 * when more than one clients can open the same camera in shared mode by calling
 * {@link ACameraManager_openSharedCamera}. In shared mode, the highest priority client among all
 * the clients will be the primary client while the others would be secondary clients. In shared
 * capture session, only primary clients can create a capture request and change capture parameters.
 * Secondary clients can only request streaming of images by calling this api
 * {@link ACameraCaptureSessionShared_logicalCamera_startStreaming}. Calling this api for normal
 * sessions when {@link ACameraManager_openCamera} is used to open the camera will throw
 * {@link ACAMERA_ERROR_INVALID_OPERATION}.
 *
 * <p>The priority of client access is determined by considering two factors: its current process
 * state and its "out of memory" score. Clients operating in the background are assigned a lower
 * priority. In contrast, clients running in the foreground, along with system-level clients, are
 * given a higher priority.</p>
 *
 * <p>With this method, the camera device will continually capture images, cycling through the
 * settings in the list of {@link ACaptureRequest} specified by the primary client. If primary
 * client does not have ongoing repeating request, camera service will use a capture request with
 * default capture parameters for preview template.</p>
 *
 * <p>To stop the continuous streaming, call {@link ACameraCaptureSessionShared_stopStreaming}.</p>
 *
 * <p>Calling this method will replace an existing continuous streaming request.</p>
 *
 * @param sharedSession the shared capture session when camera is opened in
 *        shared mode.
 * @param callbacks the {@link ACameraCaptureSession_logicalCamera_captureCallbacksV2} to be
 *        associated with this capture sequence. No capture callback will be fired if callbacks
 *        is set to NULL.
 * @param numOutputWindows number of native windows to be used for streaming. Must be at least 1.
 * @param windows an array of {@link ANativeWindow} to be used for streaming. Length must be at
 *        least numOutputWindows.
 * @param captureSequenceId the capture sequence ID associated with this capture method invocation
 *        will be stored here if this argument is not NULL and the method call succeeds.
 *        When this argument is set to NULL, the capture sequence ID will not be returned.
 *
 * @return <ul>
 *         <li>{@link ACAMERA_OK} if the method succeeds. captureSequenceId will be filled
 *             if it is not NULL.</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session or requests is NULL, or
 *             if numRequests < 1</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DISCONNECTED} if the camera device is closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DEVICE} if the camera device encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_SERVICE} if the camera service encounters fatal error
 *         </li>
 *         <li>{@link ACAMERA_ERROR_INVALID_OPERATION} if the session passed is not a shared
 *              session</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for  some other reasons</li>
 *         </ul>
 */
camera_status_t ACameraCaptureSessionShared_logicalCamera_startStreaming(
    ACameraCaptureSession* sharedSession,
    /*optional*/ACameraCaptureSession_logicalCamera_captureCallbacksV2 *callbacks,
    int numOutputWindows, ANativeWindow **windows,
    /*optional*/int *captureSequenceId) __INTRODUCED_IN(36);

/**
 * Cancel any ongoing streaming started by {@link ACameraCaptureSessionShared_startStreaming}.
 * Calling this api does not effect any streaming requests submitted by other clients who have
 * opened the camera in shared mode. Calling this api for normal sessions when
 * {@link ACameraManager_openCamera} is used to open the camera will throw
 * {@link ACAMERA_ERROR_INVALID_OPERATION}.
 *
 * @param sharedSession the capture session of interest
 *
 * @return <ul>
 *         <li>{@link ACAMERA_OK} if the method succeeds.</li>
 *         <li>{@link ACAMERA_ERROR_INVALID_PARAMETER} if session is NULL.</li>
 *         <li>{@link ACAMERA_ERROR_SESSION_CLOSED} if the capture session has been closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DISCONNECTED} if the camera device is closed</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_DEVICE} if the camera device encounters fatal error</li>
 *         <li>{@link ACAMERA_ERROR_CAMERA_SERVICE} if the camera service encounters fatal error
 *         </li>
 *         <li>{@link ACAMERA_ERROR_INVALID_OPERATION} if the session passed is not a shared
 *              session</li>
 *         <li>{@link ACAMERA_ERROR_UNKNOWN} if the method fails for some other reasons</li>
 *         </ul>
 */
camera_status_t ACameraCaptureSessionShared_stopStreaming(
    ACameraCaptureSession* sharedSession)  __INTRODUCED_IN(36);
__END_DECLS

#endif /* _NDK_CAMERA_CAPTURE_SESSION_H */

/** @} */
