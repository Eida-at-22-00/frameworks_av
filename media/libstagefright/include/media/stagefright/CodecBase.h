/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef CODEC_BASE_H_

#define CODEC_BASE_H_

#include <list>
#include <memory>

#include <stdint.h>

#define STRINGIFY_ENUMS

#include <media/hardware/CryptoAPI.h>
#include <media/hardware/HardwareAPI.h>
#include <media/MediaCodecInfo.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/ResourceInfo.h>
#include <system/graphics.h>
#include <utils/NativeHandle.h>

class C2Buffer;

namespace android {
class BufferChannelBase;
struct BufferProducerWrapper;
class MediaCodecBuffer;
struct PersistentSurface;
class RenderedFrameInfo;
class Surface;
struct ICrypto;
class IMemory;

namespace hardware {
class HidlMemory;
namespace cas {
namespace native {
namespace V1_0 {
struct IDescrambler;
}}}
namespace drm {
namespace V1_0 {
struct SharedBuffer;
}}
}

using hardware::cas::native::V1_0::IDescrambler;

struct AccessUnitInfo {
    uint32_t mFlags;
    uint32_t mSize;
    int64_t mTimestamp;
    AccessUnitInfo(uint32_t flags, uint32_t size, int64_t ptsUs)
            :mFlags(flags), mSize(size), mTimestamp(ptsUs) {
    }
    ~AccessUnitInfo() {}
};

struct CodecCryptoInfo {
    size_t mNumSubSamples{0};
    CryptoPlugin::SubSample *mSubSamples{nullptr};
    uint8_t *mIv{nullptr};
    uint8_t *mKey{nullptr};
    enum CryptoPlugin::Mode mMode;
    CryptoPlugin::Pattern mPattern;

    virtual ~CodecCryptoInfo() {}
protected:
    CodecCryptoInfo():
            mNumSubSamples(0),
            mSubSamples(nullptr),
            mIv(nullptr),
            mKey(nullptr),
            mMode{CryptoPlugin::kMode_Unencrypted},
            mPattern{0, 0} {
    }
};

struct CodecParameterDescriptor {
    std::string name;
    AMessage::Type type;
};

struct CodecBase : public AHandler, /* static */ ColorUtils {
    /**
     * This interface defines events firing from CodecBase back to MediaCodec.
     * All methods must not block.
     */
    class CodecCallback {
    public:
        virtual ~CodecCallback() = default;

        /**
         * Notify MediaCodec for seeing an output EOS.
         *
         * @param err the underlying cause of the EOS. If the value is neither
         *            OK nor ERROR_END_OF_STREAM, the EOS is declared
         *            prematurely for that error.
         */
        virtual void onEos(status_t err) = 0;
        /**
         * Notify MediaCodec that start operation is complete.
         */
        virtual void onStartCompleted() = 0;
        /**
         * Notify MediaCodec that stop operation is complete.
         */
        virtual void onStopCompleted() = 0;
        /**
         * Notify MediaCodec that release operation is complete.
         */
        virtual void onReleaseCompleted() = 0;
        /**
         * Notify MediaCodec that flush operation is complete.
         */
        virtual void onFlushCompleted() = 0;
        /**
         * Notify MediaCodec that an error is occurred.
         *
         * @param err         an error code for the occurred error.
         * @param actionCode  an action code for severity of the error.
         */
        virtual void onError(status_t err, enum ActionCode actionCode) = 0;
        /**
         * Notify MediaCodec that the underlying component is allocated.
         *
         * @param componentName the unique name of the component specified in
         *                      MediaCodecList.
         */
        virtual void onComponentAllocated(const char *componentName) = 0;
        /**
         * Notify MediaCodec that the underlying component is configured.
         *
         * @param inputFormat   an input format at configure time.
         * @param outputFormat  an output format at configure time.
         */
        virtual void onComponentConfigured(
                const sp<AMessage> &inputFormat, const sp<AMessage> &outputFormat) = 0;
        /**
         * Notify MediaCodec that the input surface is created.
         *
         * @param inputFormat   an input format at surface creation. Formats
         *                      could change from the previous state as a result
         *                      of creating a surface.
         * @param outputFormat  an output format at surface creation.
         * @param inputSurface  the created surface.
         */
        virtual void onInputSurfaceCreated(
                const sp<AMessage> &inputFormat,
                const sp<AMessage> &outputFormat,
                const sp<BufferProducerWrapper> &inputSurface) = 0;
        /**
         * Notify MediaCodec that the input surface creation is failed.
         *
         * @param err an error code of the cause.
         */
        virtual void onInputSurfaceCreationFailed(status_t err) = 0;
        /**
         * Notify MediaCodec that the component accepted the provided input
         * surface.
         *
         * @param inputFormat   an input format at surface assignment. Formats
         *                      could change from the previous state as a result
         *                      of assigning a surface.
         * @param outputFormat  an output format at surface assignment.
         */
        virtual void onInputSurfaceAccepted(
                const sp<AMessage> &inputFormat,
                const sp<AMessage> &outputFormat) = 0;
        /**
         * Notify MediaCodec that the component declined the provided input
         * surface.
         *
         * @param err an error code of the cause.
         */
        virtual void onInputSurfaceDeclined(status_t err) = 0;
        /**
         * Noitfy MediaCodec that the requested input EOS is sent to the input
         * surface.
         *
         * @param err an error code returned from the surface. If there is no
         *            input surface, the value is INVALID_OPERATION.
         */
        virtual void onSignaledInputEOS(status_t err) = 0;
        /**
         * Notify MediaCodec that output frames are rendered with information on
         * those frames.
         *
         * @param done  a list of rendered frames.
         */
        virtual void onOutputFramesRendered(const std::list<RenderedFrameInfo> &done) = 0;
        /**
         * Notify MediaCodec that output buffers are changed.
         */
        virtual void onOutputBuffersChanged() = 0;
        /**
         * Notify MediaCodec that the first tunnel frame is ready.
         */
        virtual void onFirstTunnelFrameReady() = 0;
        /**
         * Notify MediaCodec that there are metrics to be updated.
         *
         * @param updatedMetrics metrics need to be updated.
         */
        virtual void onMetricsUpdated(const sp<AMessage> &updatedMetrics) = 0;
        /**
         * Notify MediaCodec that there is a change in the required resources.
         */
        virtual void onRequiredResourcesChanged() = 0;
    };

    /**
     * This interface defines events firing from BufferChannelBase back to MediaCodec.
     * All methods must not block.
     */
    class BufferCallback {
    public:
        virtual ~BufferCallback() = default;

        /**
         * Notify MediaCodec that an input buffer is available with given index.
         * When BufferChannelBase::getInputBufferArray() is not called,
         * BufferChannelBase may report different buffers with the same index if
         * MediaCodec already queued/discarded the buffer. After calling
         * BufferChannelBase::getInputBufferArray(), the buffer and index match the
         * returned array.
         */
        virtual void onInputBufferAvailable(
                size_t index, const sp<MediaCodecBuffer> &buffer) = 0;
        /**
         * Notify MediaCodec that an output buffer is available with given index.
         * When BufferChannelBase::getOutputBufferArray() is not called,
         * BufferChannelBase may report different buffers with the same index if
         * MediaCodec already queued/discarded the buffer. After calling
         * BufferChannelBase::getOutputBufferArray(), the buffer and index match the
         * returned array.
         */
        virtual void onOutputBufferAvailable(
                size_t index, const sp<MediaCodecBuffer> &buffer) = 0;
    };
    enum {
        kMaxCodecBufferSize = 8192 * 4096 * 4, // 8K RGBA
    };

    inline void setCallback(std::unique_ptr<CodecCallback> &&callback) {
        mCallback = std::move(callback);
    }
    virtual std::shared_ptr<BufferChannelBase> getBufferChannel() = 0;

    virtual void initiateAllocateComponent(const sp<AMessage> &msg) = 0;
    virtual void initiateConfigureComponent(const sp<AMessage> &msg) = 0;
    virtual void initiateCreateInputSurface() = 0;
    virtual void initiateSetInputSurface(
            const sp<PersistentSurface> &surface) = 0;
    virtual void initiateStart() = 0;
    virtual void initiateShutdown(bool keepComponentAllocated = false) = 0;

    // require an explicit message handler
    virtual void onMessageReceived(const sp<AMessage> &msg) = 0;

    virtual status_t setSurface(const sp<Surface>& /*surface*/, uint32_t /*generation*/) {
        return INVALID_OPERATION;
    }

    virtual void signalFlush() = 0;
    virtual void signalResume() = 0;

    virtual void signalRequestIDRFrame() = 0;
    virtual void signalSetParameters(const sp<AMessage> &msg) = 0;
    virtual void signalEndOfInputStream() = 0;

    /**
     * Query supported parameters from this instance, and fill |names| with the
     * names of the parameters.
     *
     * \param names string vector to fill with supported parameters.
     * \return OK if successful;
     *         BAD_VALUE if |names| is null;
     *         INVALID_OPERATION if already released;
     *         ERROR_UNSUPPORTED if not supported.
     */
    virtual status_t querySupportedParameters(std::vector<std::string> *names);
    /**
     * Fill |desc| with description of the parameter with |name|.
     *
     * \param name name of the parameter to describe
     * \param desc pointer to CodecParameterDescriptor to be filled
     * \return OK if successful;
     *         BAD_VALUE if |desc| is null;
     *         NAME_NOT_FOUND if |name| is not recognized by the component;
     *         INVALID_OPERATION if already released;
     *         ERROR_UNSUPPORTED if not supported.
     */
    virtual status_t describeParameter(
            const std::string &name,
            CodecParameterDescriptor *desc);
    /**
     * Subscribe to parameters in |names| and get output format change event
     * when they change.
     * Unrecognized / already subscribed parameters are ignored.
     *
     * \param names names of parameters to subscribe
     * \return OK if successful;
     *         INVALID_OPERATION if already released;
     *         ERROR_UNSUPPORTED if not supported.
     */
    virtual status_t subscribeToParameters(const std::vector<std::string> &names);
    /**
     * Unsubscribe from parameters in |names| and no longer get
     * output format change event when they change.
     * Unrecognized / already unsubscribed parameters are ignored.
     *
     * \param names names of parameters to unsubscribe
     * \return OK if successful;
     *         INVALID_OPERATION if already released;
     *         ERROR_UNSUPPORTED if not supported.
     */
    virtual status_t unsubscribeFromParameters(const std::vector<std::string> &names);

    /**
     * Get the required resources for the compomemt at the current
     * configuration.
     *
     */
    virtual std::vector<InstanceResourceInfo> getRequiredSystemResources();

    typedef CodecBase *(*CreateCodecFunc)(void);
    typedef PersistentSurface *(*CreateInputSurfaceFunc)(void);

protected:
    CodecBase() = default;
    virtual ~CodecBase() = default;

    std::unique_ptr<CodecCallback> mCallback;

private:
    DISALLOW_EVIL_CONSTRUCTORS(CodecBase);
};

/**
 * A channel between MediaCodec and CodecBase object which manages buffer
 * passing. Only MediaCodec is expected to call these methods, and
 * underlying CodecBase implementation should define its own interface
 * separately for itself.
 *
 * Concurrency assumptions:
 *
 * 1) Clients may access the object at multiple threads concurrently.
 * 2) All methods do not call underlying CodecBase object while holding a lock.
 * 3) Code inside critical section executes within 1ms.
 */
class BufferChannelBase {
public:
    BufferChannelBase() = default;
    virtual ~BufferChannelBase() = default;

    inline void setCallback(std::unique_ptr<CodecBase::BufferCallback> &&callback) {
        mCallback = std::move(callback);
    }

    virtual void setCrypto(const sp<ICrypto> &) {}
    virtual void setDescrambler(const sp<IDescrambler> &) {}

    /**
     * Queue an input buffer into the buffer channel.
     *
     * @return    OK if successful;
     *            -ENOENT if the buffer is not known (TODO: this should be
     *            handled gracefully in the future, here and below).
     */
    virtual status_t queueInputBuffer(const sp<MediaCodecBuffer> &buffer) = 0;
    /**
     * Queue a secure input buffer into the buffer channel.
     *
     * @return    OK if successful;
     *            -ENOENT if the buffer is not known;
     *            -ENOSYS if mCrypto is not set so that decryption is not
     *            possible;
     *            other errors if decryption failed.
     */
    virtual status_t queueSecureInputBuffer(
            const sp<MediaCodecBuffer> &buffer,
            bool secure,
            const uint8_t *key,
            const uint8_t *iv,
            CryptoPlugin::Mode mode,
            CryptoPlugin::Pattern pattern,
            const CryptoPlugin::SubSample *subSamples,
            size_t numSubSamples,
            AString *errorDetailMsg) = 0;

    /**
     * Queue a secure input buffer with multiple access units into the buffer channel.
     *
     * @param buffer The buffer to queue. The access unit delimiters and crypto
     *               subsample information is included in the buffer metadata.
     * @param secure Whether the buffer is secure.
     * @param errorDetailMsg The error message to be set in case of error.
     * @return OK if successful;
     *         -ENOENT of the buffer is not known
     *         -ENOSYS if mCrypto is not set so that decryption is not
     *         possible;
     *         other errors if decryption failed.
     */
     virtual status_t queueSecureInputBuffers(
            const sp<MediaCodecBuffer> &buffer,
            bool secure,
            AString *errorDetailMsg) {
        (void)buffer;
        (void)secure;
        (void)errorDetailMsg;
        return -ENOSYS;
     }

    /**
     * Attach a Codec 2.0 buffer to MediaCodecBuffer.
     *
     * @return    OK if successful;
     *            -ENOENT if index is not recognized
     *            -ENOSYS if attaching buffer is not possible or not supported
     */
    virtual status_t attachBuffer(
            const std::shared_ptr<C2Buffer> &c2Buffer,
            const sp<MediaCodecBuffer> &buffer) {
        (void)c2Buffer;
        (void)buffer;
        return -ENOSYS;
    }
    /**
     * Attach an encrypted HidlMemory buffer to an index
     *
     * @return    OK if successful;
     *            -ENOENT if index is not recognized
     *            -ENOSYS if attaching buffer is not possible or not supported
     */
    virtual status_t attachEncryptedBuffer(
            const sp<hardware::HidlMemory> &memory,
            bool secure,
            const uint8_t *key,
            const uint8_t *iv,
            CryptoPlugin::Mode mode,
            CryptoPlugin::Pattern pattern,
            size_t offset,
            const CryptoPlugin::SubSample *subSamples,
            size_t numSubSamples,
            const sp<MediaCodecBuffer> &buffer,
            AString* errorDetailMsg) {
        (void)memory;
        (void)secure;
        (void)key;
        (void)iv;
        (void)mode;
        (void)pattern;
        (void)offset;
        (void)subSamples;
        (void)numSubSamples;
        (void)buffer;
        (void)errorDetailMsg;
        return -ENOSYS;
    }

    /**
     * Attach an encrypted HidlMemory buffer containing multiple access units to an index
     *
     * @param memory The memory to attach.
     * @param offset index???
     * @param buffer The MediaCodecBuffer to attach the memory to. The access
     *               unit delimiters and crypto subsample information is included
     *               in the buffer metadata.
     * @param secure Whether the buffer is secure.
     * @param errorDetailMsg The error message to be set if an error occurs.
     * @return    OK if successful;
     *            -ENOENT if index is not recognized
     *            -ENOSYS if attaching buffer is not possible or not supported
     */
    virtual status_t attachEncryptedBuffers(
            const sp<hardware::HidlMemory> &memory,
            size_t offset,
            const sp<MediaCodecBuffer> &buffer,
            bool secure,
            AString* errorDetailMsg) {
        (void)memory;
        (void)offset;
        (void)buffer;
        (void)secure;
        (void)errorDetailMsg;
        return -ENOSYS;
    }
    /**
     * Request buffer rendering at specified time.
     *
     * @param     timestampNs   nanosecond timestamp for rendering time.
     * @return    OK if successful;
     *            -ENOENT if the buffer is not known.
     */
    virtual status_t renderOutputBuffer(
            const sp<MediaCodecBuffer> &buffer, int64_t timestampNs) = 0;

    /**
     * Poll for updates about rendered buffers.
     *
     * Triggers callbacks to CodecCallback::onOutputFramesRendered.
     */
    virtual void pollForRenderedBuffers() = 0;

    /**
     * Notify a buffer is released from output surface.
     *
     * @param     generation    MediaCodec's surface specifier
     */
    virtual void onBufferReleasedFromOutputSurface(uint32_t /*generation*/) {
        // default: no-op
    };

    /**
     * Notify a buffer is attached to output surface.
     *
     * @param     generation    MediaCodec's surface specifier
     */
    virtual void onBufferAttachedToOutputSurface(uint32_t /*generation*/) {
        // default: no-op
    };

    /**
     * Discard a buffer to the underlying CodecBase object.
     *
     * TODO: remove once this operation can be handled by just clearing the
     * reference.
     *
     * @return    OK if successful;
     *            -ENOENT if the buffer is not known.
     */
    virtual status_t discardBuffer(const sp<MediaCodecBuffer> &buffer) = 0;
    /**
     * Clear and fill array with input buffers.
     */
    virtual void getInputBufferArray(Vector<sp<MediaCodecBuffer>> *array) = 0;
    /**
     * Clear and fill array with output buffers.
     */
    virtual void getOutputBufferArray(Vector<sp<MediaCodecBuffer>> *array) = 0;

    /**
     * Convert binder IMemory to drm SharedBuffer
     *
     * \param   memory      IMemory object to store encrypted content.
     * \param   heapSeqNum  Heap sequence number from ICrypto; -1 if N/A
     * \param   buf         SharedBuffer structure to fill.
     */
    static void IMemoryToSharedBuffer(
            const sp<IMemory> &memory,
            int32_t heapSeqNum,
            hardware::drm::V1_0::SharedBuffer *buf);

protected:
    std::unique_ptr<CodecBase::BufferCallback> mCallback;
};

}  // namespace android

#endif  // CODEC_BASE_H_
