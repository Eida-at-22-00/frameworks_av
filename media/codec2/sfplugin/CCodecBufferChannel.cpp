/*
 * Copyright 2017, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#include <utils/Errors.h>
#define LOG_TAG "CCodecBufferChannel"
#define ATRACE_TAG  ATRACE_TAG_VIDEO
#include <utils/Log.h>
#include <utils/Trace.h>

#include <algorithm>
#include <atomic>
#include <list>
#include <numeric>
#include <thread>
#include <chrono>
#include <regex>

#include <android_media_codec.h>
#include <android_media_tv_flags.h>

#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>
#include <C2BlockInternal.h>
#include <C2Config.h>
#include <C2Debug.h>

#include <android/hardware/cas/native/1.0/IDescrambler.h>
#include <android/hardware/drm/1.0/types.h>
#include <android/sysprop/MediaProperties.sysprop.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/no_destructor.h>
#include <android-base/stringprintf.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryDealer.h>
#include <cutils/properties.h>
#include <gui/Surface.h>
#include <hidlmemory/FrameworkUtils.h>
#include <media/openmax/OMX_Core.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ALookup.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <media/stagefright/SkipCutBuffer.h>
#include <media/stagefright/SurfaceUtils.h>
#include <media/MediaCodecBuffer.h>
#include <mediadrm/ICrypto.h>
#include <server_configurable_flags/get_flags.h>
#include <system/window.h>
#include <ui/PictureProfileHandle.h>

#include "CCodecBufferChannel.h"
#include "Codec2Buffer.h"

namespace android {

using android::base::StringPrintf;
using hardware::hidl_handle;
using hardware::hidl_string;
using hardware::hidl_vec;
using hardware::fromHeap;
using hardware::HidlMemory;
using server_configurable_flags::GetServerConfigurableFlag;

using namespace hardware::cas::V1_0;
using namespace hardware::cas::native::V1_0;

using CasStatus = hardware::cas::V1_0::Status;
using DrmBufferType = hardware::drm::V1_0::BufferType;

namespace {

constexpr size_t kSmoothnessFactor = 4;

// This is for keeping IGBP's buffer dropping logic in legacy mode other
// than making it non-blocking. Do not change this value.
const static size_t kDequeueTimeoutNs = 0;
// If app goes into background, decoding paused. we have WA logic in HAL to sleep some actions.
// This value is to monitor if decoding is paused then we can signal a new empty work to HAL
// after app resume to foreground to notify HAL something
const static uint64_t kPipelinePausedTimeoutMs = 500;

static bool areRenderMetricsEnabled() {
    std::string v = GetServerConfigurableFlag("media_native", "render_metrics_enabled", "false");
    return v == "true";
}

// Flags can come with individual BufferInfos
// when used with large frame audio
constexpr static std::initializer_list<std::pair<uint32_t, uint32_t>> flagList = {
        {BUFFER_FLAG_CODEC_CONFIG, C2FrameData::FLAG_CODEC_CONFIG},
        {BUFFER_FLAG_END_OF_STREAM, C2FrameData::FLAG_END_OF_STREAM},
        {BUFFER_FLAG_DECODE_ONLY, C2FrameData::FLAG_DROP_FRAME}
};

static uint32_t convertFlags(uint32_t flags, bool toC2) {
    return std::transform_reduce(
            flagList.begin(), flagList.end(),
            0u,
            std::bit_or{},
            [flags, toC2](const std::pair<uint32_t, uint32_t> &entry) {
                if (toC2) {
                    return (flags & entry.first) ? entry.second : 0;
                } else {
                    return (flags & entry.second) ? entry.first : 0;
                }
            });
}

class SurfaceCallbackHandler {
public:
    enum callback_type_t {
        ON_BUFFER_RELEASED = 0,
        ON_BUFFER_ATTACHED
    };

    void post(callback_type_t callback,
            std::shared_ptr<Codec2Client::Component> component,
            uint32_t generation) {
        if (!component) {
            ALOGW("surface callback psoted for invalid component");
        }
        std::shared_ptr<SurfaceCallbackItem> item =
                std::make_shared<SurfaceCallbackItem>(callback, component, generation);
        std::unique_lock<std::mutex> lock(mMutex);
        mItems.emplace_back(std::move(item));
        mCv.notify_one();
    }

    ~SurfaceCallbackHandler() {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mDone = true;
            mCv.notify_all();
        }
        if (mThread.joinable()) {
            mThread.join();
        }
    }

    static SurfaceCallbackHandler& GetInstance() {
        static base::NoDestructor<SurfaceCallbackHandler> sSurfaceCallbackHandler{};
        return *sSurfaceCallbackHandler;
    }

private:
    struct SurfaceCallbackItem {
        callback_type_t mCallback;
        std::weak_ptr<Codec2Client::Component> mComp;
        uint32_t mGeneration;

        SurfaceCallbackItem(
                callback_type_t callback,
                std::shared_ptr<Codec2Client::Component> comp,
                uint32_t generation)
                : mCallback(callback), mComp(comp), mGeneration(generation) {}
    };

    SurfaceCallbackHandler() { mThread = std::thread(&SurfaceCallbackHandler::run, this); }

    void run() {
        std::unique_lock<std::mutex> lock(mMutex);
        while (!mDone) {
            while (!mItems.empty()) {
                std::deque<std::shared_ptr<SurfaceCallbackItem>> items = std::move(mItems);
                mItems.clear();
                lock.unlock();
                handle(items);
                lock.lock();
            }
            mCv.wait(lock);
        }
    }

    void handle(std::deque<std::shared_ptr<SurfaceCallbackItem>> &items) {
        while (!items.empty()) {
            std::shared_ptr<SurfaceCallbackItem> item = items.front();
            items.pop_front();
            switch (item->mCallback) {
                case ON_BUFFER_RELEASED: {
                    std::shared_ptr<Codec2Client::Component> comp = item->mComp.lock();;
                    if (comp) {
                        comp->onBufferReleasedFromOutputSurface(item->mGeneration);
                    }
                    break;
                }
                case ON_BUFFER_ATTACHED: {
                    std::shared_ptr<Codec2Client::Component> comp = item->mComp.lock();
                    if (comp) {
                        comp->onBufferAttachedToOutputSurface(item->mGeneration);
                    }
                    break;
                }
                default:
                    ALOGE("Non defined surface callback message");
                    break;
            }
        }
    }

    std::thread mThread;
    bool mDone = false;
    std::deque<std::shared_ptr<SurfaceCallbackItem>> mItems;
    std::mutex mMutex;
    std::condition_variable mCv;


    friend class base::NoDestructor<SurfaceCallbackHandler>;

    DISALLOW_EVIL_CONSTRUCTORS(SurfaceCallbackHandler);
};

}  // namespace

CCodecBufferChannel::QueueGuard::QueueGuard(
        CCodecBufferChannel::QueueSync &sync) : mSync(sync) {
    Mutex::Autolock l(mSync.mGuardLock);
    // At this point it's guaranteed that mSync is not under state transition,
    // as we are holding its mutex.

    Mutexed<CCodecBufferChannel::QueueSync::Counter>::Locked count(mSync.mCount);
    if (count->value == -1) {
        mRunning = false;
    } else {
        ++count->value;
        mRunning = true;
    }
}

CCodecBufferChannel::QueueGuard::~QueueGuard() {
    if (mRunning) {
        // We are not holding mGuardLock at this point so that QueueSync::stop() can
        // keep holding the lock until mCount reaches zero.
        Mutexed<CCodecBufferChannel::QueueSync::Counter>::Locked count(mSync.mCount);
        --count->value;
        count->cond.broadcast();
    }
}

void CCodecBufferChannel::QueueSync::start() {
    Mutex::Autolock l(mGuardLock);
    // If stopped, it goes to running state; otherwise no-op.
    Mutexed<Counter>::Locked count(mCount);
    if (count->value == -1) {
        count->value = 0;
    }
}

void CCodecBufferChannel::QueueSync::stop() {
    Mutex::Autolock l(mGuardLock);
    Mutexed<Counter>::Locked count(mCount);
    if (count->value == -1) {
        // no-op
        return;
    }
    // Holding mGuardLock here blocks creation of additional QueueGuard objects, so
    // mCount can only decrement. In other words, threads that acquired the lock
    // are allowed to finish execution but additional threads trying to acquire
    // the lock at this point will block, and then get QueueGuard at STOPPED
    // state.
    while (count->value != 0) {
        count.waitForCondition(count->cond);
    }
    count->value = -1;
}

// Input

CCodecBufferChannel::Input::Input() : extraBuffers("extra") {}

// CCodecBufferChannel

CCodecBufferChannel::CCodecBufferChannel(
        const std::shared_ptr<CCodecCallback> &callback)
    : mHeapSeqNum(-1),
      mCCodecCallback(callback),
      mFrameIndex(0u),
      mFirstValidFrameIndex(0u),
      mAreRenderMetricsEnabled(areRenderMetricsEnabled()),
      mIsSurfaceToDisplay(false),
      mHasPresentFenceTimes(false),
      mRenderingDepth(3u),
      mMetaMode(MODE_NONE),
      mInputMetEos(false),
      mLastInputBufferAvailableTs(0u),
      mIsHWDecoder(false),
      mSendEncryptedInfoBuffer(false) {
    {
        Mutexed<Input>::Locked input(mInput);
        input->buffers.reset(new DummyInputBuffers(""));
        input->extraBuffers.flush();
        input->inputDelay = 0u;
        input->pipelineDelay = 0u;
        input->numSlots = kSmoothnessFactor;
        input->numExtraSlots = 0u;
        input->lastFlushIndex = 0u;
    }
    {
        Mutexed<Output>::Locked output(mOutput);
        output->outputDelay = 0u;
        output->numSlots = kSmoothnessFactor;
        output->bounded = false;
    }
    {
        Mutexed<BlockPools>::Locked pools(mBlockPools);
        pools->outputPoolId = C2BlockPool::BASIC_LINEAR;
    }
    if (android::media::codec::provider_->rendering_depth_removal()) {
        constexpr int kAndroidApi202404 = 202404;
        int vendorVersion = ::android::base::GetIntProperty("ro.vendor.api_level", -1);
        using ::android::sysprop::MediaProperties::codec2_remove_rendering_depth;
        if (vendorVersion > kAndroidApi202404 || codec2_remove_rendering_depth().value_or(false)) {
            mRenderingDepth = 0;
        }
    } else {
        std::string value = GetServerConfigurableFlag(
                "media_native", "ccodec_rendering_depth", "3");
        android::base::ParseInt(value, &mRenderingDepth);
    }
    mOutputSurface.lock()->maxDequeueBuffers = kSmoothnessFactor + mRenderingDepth;
}

CCodecBufferChannel::~CCodecBufferChannel() {
    if (mCrypto != nullptr && mHeapSeqNum >= 0) {
        mCrypto->unsetHeap(mHeapSeqNum);
    }
}

void CCodecBufferChannel::setComponent(
        const std::shared_ptr<Codec2Client::Component> &component) {
    std::atomic_store(&mComponent, component);
    mComponentName = component->getName() + StringPrintf("#%d", int(uintptr_t(component.get()) % 997));
    mName = mComponentName.c_str();
    std::regex pattern{"c2\\.qti\\..*\\.decoder.*"};
    mIsHWDecoder = std::regex_match(mComponentName, pattern);
}

status_t CCodecBufferChannel::setInputSurface(
        const std::shared_ptr<InputSurfaceWrapper> &surface) {
    ALOGV("[%s] setInputSurface", mName);
    if (!surface) {
        ALOGE("[%s] setInputSurface: surface must not be null", mName);
        return BAD_VALUE;
    }
    Mutexed<InputSurface>::Locked inputSurface(mInputSurface);
    inputSurface->numProcessingBuffersBalance = 0;
    inputSurface->surface = surface;
    mHasInputSurface = true;
    return inputSurface->surface->connect(std::atomic_load(&mComponent));
}

status_t CCodecBufferChannel::signalEndOfInputStream() {
    Mutexed<InputSurface>::Locked inputSurface(mInputSurface);
    if (inputSurface->surface == nullptr) {
        return INVALID_OPERATION;
    }
    return inputSurface->surface->signalEndOfInputStream();
}

status_t CCodecBufferChannel::queueInputBufferInternal(
        sp<MediaCodecBuffer> buffer,
        std::shared_ptr<C2LinearBlock> encryptedBlock,
        size_t blockSize) {
    int64_t timeUs;
    CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

    if (mInputMetEos) {
        ALOGD("[%s] buffers after EOS ignored (%lld us)", mName, (long long)timeUs);
        return OK;
    }

    int32_t flags = 0;
    int32_t tmp = 0;
    bool eos = false;
    bool tunnelFirstFrame = false;
    if (buffer->meta()->findInt32("eos", &tmp) && tmp) {
        eos = true;
        mInputMetEos = true;
        ALOGV("[%s] input EOS", mName);
    }
    if (buffer->meta()->findInt32("csd", &tmp) && tmp) {
        flags |= C2FrameData::FLAG_CODEC_CONFIG;
    }
    if (buffer->meta()->findInt32("tunnel-first-frame", &tmp) && tmp) {
        tunnelFirstFrame = true;
    }
    if (buffer->meta()->findInt32("decode-only", &tmp) && tmp) {
        flags |= C2FrameData::FLAG_DROP_FRAME;
    }
    ALOGV("[%s] queueInputBuffer: buffer->size() = %zu time: %lld",
            mName, buffer->size(), (long long)timeUs);
    std::list<std::unique_ptr<C2Work>> items;
    std::unique_ptr<C2Work> work(new C2Work);
    work->input.ordinal.timestamp = timeUs;
    work->input.ordinal.frameIndex = mFrameIndex++;
    // WORKAROUND: until codecs support handling work after EOS and max output sizing, use timestamp
    // manipulation to achieve image encoding via video codec, and to constrain encoded output.
    // Keep client timestamp in customOrdinal
    work->input.ordinal.customOrdinal = timeUs;
    work->input.buffers.clear();

    sp<Codec2Buffer> copy;
    bool usesFrameReassembler = false;

    if (buffer->size() > 0u) {
        Mutexed<Input>::Locked input(mInput);
        std::shared_ptr<C2Buffer> c2buffer;
        if (!input->buffers->releaseBuffer(buffer, &c2buffer, false)) {
            return -ENOENT;
        }
        // TODO: we want to delay copying buffers.
        if (input->extraBuffers.numComponentBuffers() < input->numExtraSlots) {
            copy = input->buffers->cloneAndReleaseBuffer(buffer);
            if (copy != nullptr) {
                (void)input->extraBuffers.assignSlot(copy);
                if (!input->extraBuffers.releaseSlot(copy, &c2buffer, false)) {
                    return UNKNOWN_ERROR;
                }
                bool released = input->buffers->releaseBuffer(buffer, nullptr, true);
                ALOGV("[%s] queueInputBuffer: buffer copied; %sreleased",
                      mName, released ? "" : "not ");
                buffer = copy;
            } else {
                ALOGW("[%s] queueInputBuffer: failed to copy a buffer; this may cause input "
                      "buffer starvation on component.", mName);
            }
        }
        if (input->frameReassembler) {
            usesFrameReassembler = true;
            input->frameReassembler.process(buffer, &items);
        } else {
            int32_t cvo = 0;
            if (buffer->meta()->findInt32("cvo", &cvo)) {
                int32_t rotation = cvo % 360;
                // change rotation to counter-clock wise.
                rotation = ((rotation <= 0) ? 0 : 360) - rotation;

                Mutexed<OutputSurface>::Locked output(mOutputSurface);
                uint64_t frameIndex = work->input.ordinal.frameIndex.peeku();
                output->rotation[frameIndex] = rotation;
            }
            sp<RefBase> obj;
            if (buffer->meta()->findObject("accessUnitInfo", &obj)) {
                ALOGV("Filling C2Info from multiple access units");
                sp<WrapperObject<std::vector<AccessUnitInfo>>> infos{
                        (decltype(infos.get()))obj.get()};
                std::vector<AccessUnitInfo> &accessUnitInfoVec = infos->value;
                std::vector<C2AccessUnitInfosStruct> multipleAccessUnitInfos;
                uint32_t outFlags = 0;
                for (int i = 0; i < accessUnitInfoVec.size(); i++) {
                    outFlags = 0;
                    outFlags = convertFlags(accessUnitInfoVec[i].mFlags, true);
                    if (eos && (outFlags & C2FrameData::FLAG_END_OF_STREAM)) {
                        outFlags &= (~C2FrameData::FLAG_END_OF_STREAM);
                    }
                    multipleAccessUnitInfos.emplace_back(
                            outFlags,
                            accessUnitInfoVec[i].mSize,
                            accessUnitInfoVec[i].mTimestamp);
                    ALOGV("%d) flags: %d, size: %d, time: %llu",
                            i, outFlags, accessUnitInfoVec[i].mSize,
                            (long long)accessUnitInfoVec[i].mTimestamp);

                }
                const std::shared_ptr<C2AccessUnitInfos::input> c2AccessUnitInfos =
                        C2AccessUnitInfos::input::AllocShared(
                                multipleAccessUnitInfos.size(), 0u, multipleAccessUnitInfos);
                c2buffer->setInfo(c2AccessUnitInfos);
            }
            work->input.buffers.push_back(c2buffer);
            if (encryptedBlock) {
                work->input.infoBuffers.emplace_back(C2InfoBuffer::CreateLinearBuffer(
                        kParamIndexEncryptedBuffer,
                        encryptedBlock->share(0, blockSize, C2Fence())));
            }
        }
    } else if (eos) {
        Mutexed<Input>::Locked input(mInput);
        if (input->frameReassembler) {
            usesFrameReassembler = true;
            // drain any pending items with eos
            input->frameReassembler.process(buffer, &items);
        }
        flags |= C2FrameData::FLAG_END_OF_STREAM;
    }
    if (usesFrameReassembler) {
        if (!items.empty()) {
            items.front()->input.configUpdate = std::move(mParamsToBeSet);
            mFrameIndex = (items.back()->input.ordinal.frameIndex + 1).peek();
        }
    } else {
        work->input.flags = (C2FrameData::flags_t)flags;

        // TODO: fill info's
        if (android::media::codec::provider_->region_of_interest()
                && android::media::codec::provider_->region_of_interest_support()) {
            if (mInfoBuffers.size()) {
                for (auto infoBuffer : mInfoBuffers) {
                    work->input.infoBuffers.emplace_back(*infoBuffer);
                }
                mInfoBuffers.clear();
            }
        }

        work->input.configUpdate = std::move(mParamsToBeSet);
        if (tunnelFirstFrame) {
            C2StreamTunnelHoldRender::input tunnelHoldRender{
                0u /* stream */,
                C2_TRUE /* value */
            };
            work->input.configUpdate.push_back(C2Param::Copy(tunnelHoldRender));
        }
        work->worklets.clear();
        work->worklets.emplace_back(new C2Worklet);

        items.push_back(std::move(work));

        eos = eos && buffer->size() > 0u;
    }
    if (eos) {
        work.reset(new C2Work);
        work->input.ordinal.timestamp = timeUs;
        work->input.ordinal.frameIndex = mFrameIndex++;
        // WORKAROUND: keep client timestamp in customOrdinal
        work->input.ordinal.customOrdinal = timeUs;
        work->input.buffers.clear();
        work->input.flags = C2FrameData::FLAG_END_OF_STREAM;
        work->worklets.emplace_back(new C2Worklet);
        items.push_back(std::move(work));
    }
    c2_status_t err = C2_OK;
    if (!items.empty()) {
        ScopedTrace trace(ATRACE_TAG, android::base::StringPrintf(
                "CCodecBufferChannel::queue(%s@ts=%lld)", mName, (long long)timeUs).c_str());
        {
            Mutexed<PipelineWatcher>::Locked watcher(mPipelineWatcher);
            PipelineWatcher::Clock::time_point now = PipelineWatcher::Clock::now();
            for (const std::unique_ptr<C2Work> &work : items) {
                watcher->onWorkQueued(
                        work->input.ordinal.frameIndex.peeku(),
                        std::vector(work->input.buffers),
                        now);
            }
        }
        err = std::atomic_load(&mComponent)->queue(&items);
    }
    if (err != C2_OK) {
        Mutexed<PipelineWatcher>::Locked watcher(mPipelineWatcher);
        for (const std::unique_ptr<C2Work> &work : items) {
            watcher->onWorkDone(work->input.ordinal.frameIndex.peeku());
        }
    } else {
        Mutexed<Input>::Locked input(mInput);
        bool released = false;
        if (copy) {
            released = input->extraBuffers.releaseSlot(copy, nullptr, true);
        } else if (buffer) {
            released = input->buffers->releaseBuffer(buffer, nullptr, true);
        }
        ALOGV("[%s] queueInputBuffer: buffer%s %sreleased",
              mName, (buffer == nullptr) ? "(copy)" : "", released ? "" : "not ");
    }

    feedInputBufferIfAvailableInternal();
    return err;
}

status_t CCodecBufferChannel::setParameters(std::vector<std::unique_ptr<C2Param>> &params) {
    QueueGuard guard(mSync);
    if (!guard.isRunning()) {
        ALOGD("[%s] setParameters is only supported in the running state.", mName);
        return -ENOSYS;
    }
    mParamsToBeSet.insert(mParamsToBeSet.end(),
                          std::make_move_iterator(params.begin()),
                          std::make_move_iterator(params.end()));
    params.clear();
    return OK;
}

status_t CCodecBufferChannel::attachBuffer(
        const std::shared_ptr<C2Buffer> &c2Buffer,
        const sp<MediaCodecBuffer> &buffer) {
    if (!buffer->copy(c2Buffer)) {
        return -ENOSYS;
    }
    return OK;
}

void CCodecBufferChannel::ensureDecryptDestination(size_t size) {
    if (!mDecryptDestination || mDecryptDestination->size() < size) {
        sp<IMemoryHeap> heap{new MemoryHeapBase(size * 2)};
        if (mDecryptDestination && mCrypto && mHeapSeqNum >= 0) {
            mCrypto->unsetHeap(mHeapSeqNum);
        }
        mDecryptDestination = new MemoryBase(heap, 0, size * 2);
        if (mCrypto) {
            mHeapSeqNum = mCrypto->setHeap(hardware::fromHeap(heap));
        }
    }
}

int32_t CCodecBufferChannel::getHeapSeqNum(const sp<HidlMemory> &memory) {
    CHECK(mCrypto);
    auto it = mHeapSeqNumMap.find(memory);
    int32_t heapSeqNum = -1;
    if (it == mHeapSeqNumMap.end()) {
        heapSeqNum = mCrypto->setHeap(memory);
        mHeapSeqNumMap.emplace(memory, heapSeqNum);
    } else {
        heapSeqNum = it->second;
    }
    return heapSeqNum;
}

typedef WrapperObject<std::vector<AccessUnitInfo>> BufferInfosWrapper;
typedef WrapperObject<std::vector<std::unique_ptr<CodecCryptoInfo>>> CryptoInfosWrapper;
status_t CCodecBufferChannel::attachEncryptedBuffers(
        const sp<hardware::HidlMemory> &memory,
        size_t offset,
        const sp<MediaCodecBuffer> &buffer,
        bool secure,
        AString* errorDetailMsg) {
    static const C2MemoryUsage kDefaultReadWriteUsage{
        C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
    if (!hasCryptoOrDescrambler()) {
        ALOGE("attachEncryptedBuffers requires Crypto/descrambler object");
        return -ENOSYS;
    }
    size_t size = 0;
    CHECK(buffer->meta()->findSize("ssize", &size));
    if (size == 0) {
        buffer->setRange(0, 0);
        return OK;
    }
    sp<RefBase> obj;
    CHECK(buffer->meta()->findObject("cryptoInfos", &obj));
    sp<CryptoInfosWrapper> cryptoInfos{(CryptoInfosWrapper *)obj.get()};
    CHECK(buffer->meta()->findObject("accessUnitInfo", &obj));
    sp<BufferInfosWrapper> bufferInfos{(BufferInfosWrapper *)obj.get()};
    if (secure || (mCrypto == nullptr)) {
        if (cryptoInfos->value.size() != 1) {
            ALOGE("Cannot decrypt multiple access units");
            return -ENOSYS;
        }
        // we are dealing with just one cryptoInfo or descrambler.
        std::unique_ptr<CodecCryptoInfo> &info = cryptoInfos->value[0];
        if (info == nullptr) {
            ALOGE("Cannot decrypt, CryptoInfos are null.");
            return -ENOSYS;
        }
        return attachEncryptedBuffer(
                memory,
                secure,
                info->mKey,
                info->mIv,
                info->mMode,
                info->mPattern,
                offset,
                info->mSubSamples,
                info->mNumSubSamples,
                buffer,
                errorDetailMsg);
    }
    std::shared_ptr<C2BlockPool> pool = mBlockPools.lock()->inputPool;
    std::shared_ptr<C2LinearBlock> block;
    c2_status_t err = pool->fetchLinearBlock(
            size,
            kDefaultReadWriteUsage,
            &block);
    if (err != C2_OK) {
        ALOGI("[%s] attachEncryptedBuffers: fetchLinearBlock failed: size = %zu (%s) err = %d",
              mName, size, secure ? "secure" : "non-secure", err);
        return NO_MEMORY;
    }
    ensureDecryptDestination(size);
    C2WriteView wView = block->map().get();
    if (wView.error() != C2_OK) {
        ALOGI("[%s] attachEncryptedBuffers: block map error: %d (non-secure)",
              mName, wView.error());
        return UNKNOWN_ERROR;
    }

    ssize_t result = -1;
    size_t srcOffset = offset;
    size_t outBufferSize = 0;
    uint32_t cryptoInfoIdx = 0;
    int32_t heapSeqNum = getHeapSeqNum(memory);
    hardware::drm::V1_0::SharedBuffer src{(uint32_t)heapSeqNum, offset, size};
    hardware::drm::V1_0::DestinationBuffer dst;
    dst.type = DrmBufferType::SHARED_MEMORY;
    IMemoryToSharedBuffer(
            mDecryptDestination, mHeapSeqNum, &dst.nonsecureMemory);
    for (int i = 0; i < bufferInfos->value.size(); i++) {
        if (bufferInfos->value[i].mSize > 0) {
            std::unique_ptr<CodecCryptoInfo> &info = cryptoInfos->value[cryptoInfoIdx++];
            src.offset = srcOffset;
            src.size = bufferInfos->value[i].mSize;
            result = mCrypto->decrypt(
                    (uint8_t*)info->mKey,
                    (uint8_t*)info->mIv,
                    info->mMode,
                    info->mPattern,
                    src,
                    0,
                    info->mSubSamples,
                    info->mNumSubSamples,
                    dst,
                    errorDetailMsg);
            srcOffset += bufferInfos->value[i].mSize;
            if (result < 0) {
                ALOGI("[%s] attachEncryptedBuffers: decrypt failed: result = %zd",
                        mName, result);
                return result;
            }
            if (wView.error() == C2_OK) {
                if (wView.size() < result) {
                    ALOGI("[%s] attachEncryptedBuffers: block size too small:"
                            "size=%u result=%zd (non-secure)", mName, wView.size(), result);
                    return UNKNOWN_ERROR;
                }
                memcpy(wView.data(), mDecryptDestination->unsecurePointer(), result);
                bufferInfos->value[i].mSize = result;
                wView.setOffset(wView.offset() + result);
            }
            outBufferSize += result;
        }
    }
    if (wView.error() == C2_OK) {
        wView.setOffset(0);
    }
    std::shared_ptr<C2Buffer> c2Buffer{C2Buffer::CreateLinearBuffer(
            block->share(0, outBufferSize, C2Fence{}))};
    if (!buffer->copy(c2Buffer)) {
        ALOGI("[%s] attachEncryptedBuffers: buffer copy failed", mName);
        return -ENOSYS;
    }
    return OK;
}

status_t CCodecBufferChannel::attachEncryptedBuffer(
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
    static const C2MemoryUsage kSecureUsage{C2MemoryUsage::READ_PROTECTED, 0};
    static const C2MemoryUsage kDefaultReadWriteUsage{
        C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};

    size_t size = 0;
    for (size_t i = 0; i < numSubSamples; ++i) {
        size += subSamples[i].mNumBytesOfClearData + subSamples[i].mNumBytesOfEncryptedData;
    }
    if (size == 0) {
        buffer->setRange(0, 0);
        return OK;
    }
    std::shared_ptr<C2BlockPool> pool = mBlockPools.lock()->inputPool;
    std::shared_ptr<C2LinearBlock> block;
    c2_status_t err = pool->fetchLinearBlock(
            size,
            secure ? kSecureUsage : kDefaultReadWriteUsage,
            &block);
    if (err != C2_OK) {
        ALOGI("[%s] attachEncryptedBuffer: fetchLinearBlock failed: size = %zu (%s) err = %d",
              mName, size, secure ? "secure" : "non-secure", err);
        return NO_MEMORY;
    }
    if (!secure) {
        ensureDecryptDestination(size);
    }
    ssize_t result = -1;
    ssize_t codecDataOffset = 0;
    if (mCrypto) {
        int32_t heapSeqNum = getHeapSeqNum(memory);
        hardware::drm::V1_0::SharedBuffer src{(uint32_t)heapSeqNum, offset, size};
        hardware::drm::V1_0::DestinationBuffer dst;
        if (secure) {
            dst.type = DrmBufferType::NATIVE_HANDLE;
            dst.secureMemory = hardware::hidl_handle(block->handle());
        } else {
            dst.type = DrmBufferType::SHARED_MEMORY;
            IMemoryToSharedBuffer(
                    mDecryptDestination, mHeapSeqNum, &dst.nonsecureMemory);
        }
        result = mCrypto->decrypt(
                key, iv, mode, pattern, src, 0, subSamples, numSubSamples,
                dst, errorDetailMsg);
        if (result < 0) {
            ALOGI("[%s] attachEncryptedBuffer: decrypt failed: result = %zd", mName, result);
            return result;
        }
    } else {
        // Here we cast CryptoPlugin::SubSample to hardware::cas::native::V1_0::SubSample
        // directly, the structure definitions should match as checked in DescramblerImpl.cpp.
        hidl_vec<SubSample> hidlSubSamples;
        hidlSubSamples.setToExternal((SubSample *)subSamples, numSubSamples, false /*own*/);

        hardware::cas::native::V1_0::SharedBuffer src{*memory, offset, size};
        hardware::cas::native::V1_0::DestinationBuffer dst;
        if (secure) {
            dst.type = BufferType::NATIVE_HANDLE;
            dst.secureMemory = hardware::hidl_handle(block->handle());
        } else {
            dst.type = BufferType::SHARED_MEMORY;
            dst.nonsecureMemory = src;
        }

        CasStatus status = CasStatus::OK;
        hidl_string detailedError;
        ScramblingControl sctrl = ScramblingControl::UNSCRAMBLED;

        if (key != nullptr) {
            sctrl = (ScramblingControl)key[0];
            // Adjust for the PES offset
            codecDataOffset = key[2] | (key[3] << 8);
        }

        auto returnVoid = mDescrambler->descramble(
                sctrl,
                hidlSubSamples,
                src,
                0,
                dst,
                0,
                [&status, &result, &detailedError] (
                        CasStatus _status, uint32_t _bytesWritten,
                        const hidl_string& _detailedError) {
                    status = _status;
                    result = (ssize_t)_bytesWritten;
                    detailedError = _detailedError;
                });
        if (errorDetailMsg) {
            errorDetailMsg->setTo(detailedError.c_str(), detailedError.size());
        }
        if (!returnVoid.isOk() || status != CasStatus::OK || result < 0) {
            ALOGI("[%s] descramble failed, trans=%s, status=%d, result=%zd",
                    mName, returnVoid.description().c_str(), status, result);
            return UNKNOWN_ERROR;
        }

        if (result < codecDataOffset) {
            ALOGD("[%s] invalid codec data offset: %zd, result %zd",
                  mName, codecDataOffset, result);
            return BAD_VALUE;
        }
    }
    if (!secure) {
        C2WriteView view = block->map().get();
        if (view.error() != C2_OK) {
            ALOGI("[%s] attachEncryptedBuffer: block map error: %d (non-secure)",
                  mName, view.error());
            return UNKNOWN_ERROR;
        }
        if (view.size() < result) {
            ALOGI("[%s] attachEncryptedBuffer: block size too small: size=%u result=%zd "
                  "(non-secure)",
                  mName, view.size(), result);
            return UNKNOWN_ERROR;
        }
        memcpy(view.data(), mDecryptDestination->unsecurePointer(), result);
    }
    std::shared_ptr<C2Buffer> c2Buffer{C2Buffer::CreateLinearBuffer(
            block->share(codecDataOffset, result - codecDataOffset, C2Fence{}))};
    if (!buffer->copy(c2Buffer)) {
        ALOGI("[%s] attachEncryptedBuffer: buffer copy failed", mName);
        return -ENOSYS;
    }
    return OK;
}

status_t CCodecBufferChannel::queueInputBuffer(const sp<MediaCodecBuffer> &buffer) {
    QueueGuard guard(mSync);
    if (!guard.isRunning()) {
        ALOGD("[%s] No more buffers should be queued at current state.", mName);
        return -ENOSYS;
    }
    return queueInputBufferInternal(buffer);
}

status_t CCodecBufferChannel::queueSecureInputBuffer(
        const sp<MediaCodecBuffer> &buffer, bool secure, const uint8_t *key,
        const uint8_t *iv, CryptoPlugin::Mode mode, CryptoPlugin::Pattern pattern,
        const CryptoPlugin::SubSample *subSamples, size_t numSubSamples,
        AString *errorDetailMsg) {
    QueueGuard guard(mSync);
    if (!guard.isRunning()) {
        ALOGD("[%s] No more buffers should be queued at current state.", mName);
        return -ENOSYS;
    }

    if (!hasCryptoOrDescrambler()) {
        return -ENOSYS;
    }
    sp<EncryptedLinearBlockBuffer> encryptedBuffer((EncryptedLinearBlockBuffer *)buffer.get());

    std::shared_ptr<C2LinearBlock> block;
    size_t allocSize = buffer->size();
    size_t bufferSize = 0;
    c2_status_t blockRes = C2_OK;
    bool copied = false;
    {
        ScopedTrace trace(ATRACE_TAG, android::base::StringPrintf(
                "CCodecBufferChannel::decrypt(%s)", mName).c_str());
        if (mSendEncryptedInfoBuffer) {
            static const C2MemoryUsage kDefaultReadWriteUsage{
                C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
            constexpr int kAllocGranule0 = 1024 * 64;
            constexpr int kAllocGranule1 = 1024 * 1024;
            std::shared_ptr<C2BlockPool> pool = mBlockPools.lock()->inputPool;
            // round up encrypted sizes to limit fragmentation and encourage buffer reuse
            if (allocSize <= kAllocGranule1) {
                bufferSize = align(allocSize, kAllocGranule0);
            } else {
                bufferSize = align(allocSize, kAllocGranule1);
            }
            blockRes = pool->fetchLinearBlock(
                    bufferSize, kDefaultReadWriteUsage, &block);

            if (blockRes == C2_OK) {
                C2WriteView view = block->map().get();
                if (view.error() == C2_OK && view.size() == bufferSize) {
                    copied = true;
                    // TODO: only copy clear sections
                    memcpy(view.data(), buffer->data(), allocSize);
                }
            }
        }

        if (!copied) {
            block.reset();
        }

        ssize_t result = -1;
        ssize_t codecDataOffset = 0;
        if (numSubSamples == 1
                && subSamples[0].mNumBytesOfClearData == 0
                && subSamples[0].mNumBytesOfEncryptedData == 0) {
            // We don't need to go through crypto or descrambler if the input is empty.
            result = 0;
        } else if (mCrypto != nullptr) {
            hardware::drm::V1_0::DestinationBuffer destination;
            if (secure) {
                destination.type = DrmBufferType::NATIVE_HANDLE;
                destination.secureMemory = hidl_handle(encryptedBuffer->handle());
            } else {
                destination.type = DrmBufferType::SHARED_MEMORY;
                IMemoryToSharedBuffer(
                        mDecryptDestination, mHeapSeqNum, &destination.nonsecureMemory);
            }
            hardware::drm::V1_0::SharedBuffer source;
            encryptedBuffer->fillSourceBuffer(&source);
            result = mCrypto->decrypt(
                    key, iv, mode, pattern, source, buffer->offset(),
                    subSamples, numSubSamples, destination, errorDetailMsg);
            if (result < 0) {
                ALOGI("[%s] decrypt failed: result=%zd", mName, result);
                return result;
            }
            if (destination.type == DrmBufferType::SHARED_MEMORY) {
                encryptedBuffer->copyDecryptedContent(mDecryptDestination, result);
            }
        } else {
            // Here we cast CryptoPlugin::SubSample to hardware::cas::native::V1_0::SubSample
            // directly, the structure definitions should match as checked in DescramblerImpl.cpp.
            hidl_vec<SubSample> hidlSubSamples;
            hidlSubSamples.setToExternal((SubSample *)subSamples, numSubSamples, false /*own*/);

            hardware::cas::native::V1_0::SharedBuffer srcBuffer;
            encryptedBuffer->fillSourceBuffer(&srcBuffer);

            DestinationBuffer dstBuffer;
            if (secure) {
                dstBuffer.type = BufferType::NATIVE_HANDLE;
                dstBuffer.secureMemory = hidl_handle(encryptedBuffer->handle());
            } else {
                dstBuffer.type = BufferType::SHARED_MEMORY;
                dstBuffer.nonsecureMemory = srcBuffer;
            }

            CasStatus status = CasStatus::OK;
            hidl_string detailedError;
            ScramblingControl sctrl = ScramblingControl::UNSCRAMBLED;

            if (key != nullptr) {
                sctrl = (ScramblingControl)key[0];
                // Adjust for the PES offset
                codecDataOffset = key[2] | (key[3] << 8);
            }

            auto returnVoid = mDescrambler->descramble(
                    sctrl,
                    hidlSubSamples,
                    srcBuffer,
                    0,
                    dstBuffer,
                    0,
                    [&status, &result, &detailedError] (
                            CasStatus _status, uint32_t _bytesWritten,
                            const hidl_string& _detailedError) {
                        status = _status;
                        result = (ssize_t)_bytesWritten;
                        detailedError = _detailedError;
                    });

            if (!returnVoid.isOk() || status != CasStatus::OK || result < 0) {
                ALOGI("[%s] descramble failed, trans=%s, status=%d, result=%zd",
                        mName, returnVoid.description().c_str(), status, result);
                return UNKNOWN_ERROR;
            }

            if (result < codecDataOffset) {
                ALOGD("invalid codec data offset: %zd, result %zd", codecDataOffset, result);
                return BAD_VALUE;
            }

            ALOGV("[%s] descramble succeeded, %zd bytes", mName, result);

            if (dstBuffer.type == BufferType::SHARED_MEMORY) {
                encryptedBuffer->copyDecryptedContentFromMemory(result);
            }
        }

        buffer->setRange(codecDataOffset, result - codecDataOffset);
    }
    return queueInputBufferInternal(buffer, block, bufferSize);
}

status_t CCodecBufferChannel::queueSecureInputBuffers(
        const sp<MediaCodecBuffer> &buffer,
        bool secure,
        AString *errorDetailMsg) {
    QueueGuard guard(mSync);
    if (!guard.isRunning()) {
        ALOGD("[%s] No more buffers should be queued at current state.", mName);
        return -ENOSYS;
    }

    if (!hasCryptoOrDescrambler()) {
        ALOGE("queueSecureInputBuffers requires a Crypto/descrambler Object");
        return -ENOSYS;
    }
    sp<RefBase> obj;
    CHECK(buffer->meta()->findObject("cryptoInfos", &obj));
    sp<CryptoInfosWrapper> cryptoInfos{(CryptoInfosWrapper *)obj.get()};
    CHECK(buffer->meta()->findObject("accessUnitInfo", &obj));
    sp<BufferInfosWrapper> bufferInfos{(BufferInfosWrapper *)obj.get()};
    if (secure || mCrypto == nullptr) {
        if (cryptoInfos->value.size() != 1) {
            ALOGE("Cannot decrypt multiple access units on native handles");
            return -ENOSYS;
        }
        std::unique_ptr<CodecCryptoInfo> info = std::move(cryptoInfos->value[0]);
        if (info == nullptr) {
            ALOGE("Cannot decrypt, CryptoInfos are null");
            return -ENOSYS;
        }
        return queueSecureInputBuffer(
                buffer,
                secure,
                info->mKey,
                info->mIv,
                info->mMode,
                info->mPattern,
                info->mSubSamples,
                info->mNumSubSamples,
                errorDetailMsg);
    }
    sp<EncryptedLinearBlockBuffer> encryptedBuffer((EncryptedLinearBlockBuffer *)buffer.get());

    std::shared_ptr<C2LinearBlock> block;
    size_t allocSize = buffer->size();
    size_t bufferSize = 0;
    c2_status_t blockRes = C2_OK;
    bool copied = false;
    ScopedTrace trace(ATRACE_TAG, android::base::StringPrintf(
            "CCodecBufferChannel::decrypt(%s)", mName).c_str());
    if (mSendEncryptedInfoBuffer) {
        static const C2MemoryUsage kDefaultReadWriteUsage{
            C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
        constexpr int kAllocGranule0 = 1024 * 64;
        constexpr int kAllocGranule1 = 1024 * 1024;
        std::shared_ptr<C2BlockPool> pool = mBlockPools.lock()->inputPool;
        // round up encrypted sizes to limit fragmentation and encourage buffer reuse
        if (allocSize <= kAllocGranule1) {
            bufferSize = align(allocSize, kAllocGranule0);
        } else {
            bufferSize = align(allocSize, kAllocGranule1);
        }
        blockRes = pool->fetchLinearBlock(
                bufferSize, kDefaultReadWriteUsage, &block);

        if (blockRes == C2_OK) {
            C2WriteView view = block->map().get();
            if (view.error() == C2_OK && view.size() == bufferSize) {
                copied = true;
                // TODO: only copy clear sections
                memcpy(view.data(), buffer->data(), allocSize);
            }
        }
    }

    if (!copied) {
        block.reset();
    }
    // size of cryptoInfo and accessUnitInfo should be the same?
    ssize_t result = -1;
    size_t srcOffset = 0;
    size_t outBufferSize = 0;
    uint32_t cryptoInfoIdx = 0;
    {
        // scoped this block to enable destruction of mappedBlock
        std::unique_ptr<EncryptedLinearBlockBuffer::MappedBlock> mappedBlock = nullptr;
        hardware::drm::V1_0::DestinationBuffer destination;
        destination.type = DrmBufferType::SHARED_MEMORY;
        IMemoryToSharedBuffer(
                mDecryptDestination, mHeapSeqNum, &destination.nonsecureMemory);
        encryptedBuffer->getMappedBlock(&mappedBlock);
        hardware::drm::V1_0::SharedBuffer source;
        encryptedBuffer->fillSourceBuffer(&source);
        srcOffset = source.offset;
        for (int i = 0 ; i < bufferInfos->value.size(); i++) {
            if (bufferInfos->value[i].mSize > 0) {
                std::unique_ptr<CodecCryptoInfo> info =
                        std::move(cryptoInfos->value[cryptoInfoIdx++]);
                if (info->mNumSubSamples == 1
                        && info->mSubSamples[0].mNumBytesOfClearData == 0
                        && info->mSubSamples[0].mNumBytesOfEncryptedData == 0) {
                    // no data so we only populate the bufferInfo
                    result = 0;
                } else {
                    source.offset = srcOffset;
                    source.size = bufferInfos->value[i].mSize;
                    result = mCrypto->decrypt(
                            (uint8_t*)info->mKey,
                            (uint8_t*)info->mIv,
                            info->mMode,
                            info->mPattern,
                            source,
                            buffer->offset(),
                            info->mSubSamples,
                            info->mNumSubSamples,
                            destination,
                            errorDetailMsg);
                    srcOffset += bufferInfos->value[i].mSize;
                    if (result < 0) {
                        ALOGI("[%s] decrypt failed: result=%zd", mName, result);
                        return result;
                    }
                    if (destination.type == DrmBufferType::SHARED_MEMORY && mappedBlock) {
                        mappedBlock->copyDecryptedContent(mDecryptDestination, result);
                    }
                    bufferInfos->value[i].mSize = result;
                    outBufferSize += result;
                }
            }
        }
        buffer->setRange(0, outBufferSize);
    }
    return queueInputBufferInternal(buffer, block, bufferSize);
}

void CCodecBufferChannel::queueDummyWork() {
    std::unique_ptr<C2Work> work(new C2Work);
    // WA: signal a empty work to HAL to trigger specific event, but totally drop the work
    work->input.flags = C2FrameData::FLAG_DROP_FRAME;
    std::list<std::unique_ptr<C2Work>> items;
    items.push_back(std::move(work));
    (void)mComponent->queue(&items);
}

void CCodecBufferChannel::feedInputBufferIfAvailable() {
    QueueGuard guard(mSync);
    if (!guard.isRunning()) {
        ALOGV("[%s] We're not running --- no input buffer reported", mName);
        return;
    }

    feedInputBufferIfAvailableInternal();

    // limit this WA to qc hw decoder only
    // if feedInputBufferIfAvailableInternal() successfully (has available input buffer),
    // mLastInputBufferAvailableTs would be updated. otherwise, not input buffer available
    if (mIsHWDecoder) {
        std::lock_guard<std::mutex> tsLock(mTsLock);
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                PipelineWatcher::Clock::now().time_since_epoch()).count();
        if (now - mLastInputBufferAvailableTs > kPipelinePausedTimeoutMs) {
            ALOGV("long time elapsed since last input available, let's queue a specific work to "
                    "HAL to notify something");
            queueDummyWork();
        }
    }
}

void CCodecBufferChannel::feedInputBufferIfAvailableInternal() {
    if (mInputMetEos) {
        return;
    }
    int64_t numOutputSlots = 0;
    bool outputFull = [this, &numOutputSlots]() {
        Mutexed<Output>::Locked output(mOutput);
        if (!output->buffers) {
            ALOGV("[%s] feedInputBufferIfAvailableInternal: "
                  "return because output buffers are null", mName);
            return true;
        }
        numOutputSlots = int64_t(output->numSlots);
        if (output->buffers->hasPending() ||
                (!output->bounded && output->buffers->numActiveSlots() >= output->numSlots)) {
            ALOGV("[%s] feedInputBufferIfAvailableInternal: "
                  "return because there are no room for more output buffers", mName);
            return true;
        }
        return false;
    }();
    if (android::media::codec::provider_->input_surface_throttle()) {
        Mutexed<InputSurface>::Locked inputSurface(mInputSurface);
        if (inputSurface->surface) {
            if (inputSurface->numProcessingBuffersBalance <= numOutputSlots) {
                ++inputSurface->numProcessingBuffersBalance;
                ALOGV("[%s] feedInputBufferIfAvailableInternal: numProcessingBuffersBalance = %lld",
                      mName, static_cast<long long>(inputSurface->numProcessingBuffersBalance));
                inputSurface->surface->onInputBufferEmptied();
            }
        }
    }
    if (outputFull) {
        return;
    }
    size_t numActiveSlots = 0;
    size_t pipelineRoom = 0;
    size_t numInputBuffersAvailable = 0;
    while (!mPipelineWatcher.lock()->pipelineFull(&pipelineRoom)) {
        sp<MediaCodecBuffer> inBuffer;
        size_t index;
        {
            Mutexed<Input>::Locked input(mInput);
            numActiveSlots = input->buffers->numActiveSlots();
            if (numActiveSlots >= input->numSlots) {
                break;
            }

            // Control the inputs based on pipelineRoom only for HW decoder
            if (!mIsHWDecoder) {
                pipelineRoom = SIZE_MAX;
            }
            if (pipelineRoom <= input->buffers->numClientBuffers()) {
                ALOGV("pipelineRoom(%zu) is <= numClientBuffers(%zu). "
                    "Not signalling any more buffers to client",
                    pipelineRoom, input->buffers->numClientBuffers());
                break;
            }
            if (!input->buffers->requestNewBuffer(&index, &inBuffer)) {
                ALOGV("[%s] no new buffer available", mName);
                break;
            }
        }

        {
            std::lock_guard<std::mutex> tsLock(mTsLock);
            mLastInputBufferAvailableTs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    PipelineWatcher::Clock::now().time_since_epoch()).count();
        }

        ALOGV("[%s] new input index = %zu [%p]", mName, index, inBuffer.get());
        mCallback->onInputBufferAvailable(index, inBuffer);
        if (++numInputBuffersAvailable >= pipelineRoom) {
            ALOGV("[%s] pipeline will overflow after %zu queueInputBuffer", mName,
                    numInputBuffersAvailable);
            break;
        }
    }
    ALOGV("[%s] # active slots after feedInputBufferIfAvailable = %zu", mName, numActiveSlots);
}

status_t CCodecBufferChannel::renderOutputBuffer(
        const sp<MediaCodecBuffer> &buffer, int64_t timestampNs) {
    std::string traceStr;
    if (ATRACE_ENABLED()) {
        traceStr = android::base::StringPrintf(
                "CCodecBufferChannel::renderOutputBuffer-%s", mName);
    }
    ScopedTrace trace(ATRACE_TAG, traceStr.c_str());

    ALOGV("[%s] renderOutputBuffer: %p", mName, buffer.get());
    std::shared_ptr<C2Buffer> c2Buffer;
    bool released = false;
    {
        Mutexed<Output>::Locked output(mOutput);
        if (output->buffers) {
            released = output->buffers->releaseBuffer(buffer, &c2Buffer);
        }
    }
    // NOTE: some apps try to releaseOutputBuffer() with timestamp and/or render
    //       set to true.
    sendOutputBuffers();
    // input buffer feeding may have been gated by pending output buffers
    feedInputBufferIfAvailable();
    if (!c2Buffer) {
        if (released) {
            std::call_once(mRenderWarningFlag, [this] {
                ALOGW("[%s] The app is calling releaseOutputBuffer() with "
                      "timestamp or render=true with non-video buffers. Apps should "
                      "call releaseOutputBuffer() with render=false for those.",
                      mName);
            });
        }
        return INVALID_OPERATION;
    }

#if 0
    const std::vector<std::shared_ptr<const C2Info>> infoParams = c2Buffer->info();
    ALOGV("[%s] queuing gfx buffer with %zu infos", mName, infoParams.size());
    for (const std::shared_ptr<const C2Info> &info : infoParams) {
        AString res;
        for (size_t ix = 0; ix + 3 < info->size(); ix += 4) {
            if (ix) res.append(", ");
            res.append(*((int32_t*)info.get() + (ix / 4)));
        }
        ALOGV("  [%s]", res.c_str());
    }
#endif
    std::shared_ptr<const C2StreamRotationInfo::output> rotation =
        std::static_pointer_cast<const C2StreamRotationInfo::output>(
                c2Buffer->getInfo(C2StreamRotationInfo::output::PARAM_TYPE));
    bool flip = rotation && (rotation->flip & 1);
    uint32_t quarters = ((rotation ? rotation->value : 0) / 90) & 3;

    {
        Mutexed<OutputSurface>::Locked output(mOutputSurface);
        if (output->surface == nullptr) {
            ALOGI("[%s] cannot render buffer without surface", mName);
            return OK;
        }
        int64_t frameIndex;
        buffer->meta()->findInt64("frameIndex", &frameIndex);
        if (output->rotation.count(frameIndex) != 0) {
            auto it = output->rotation.find(frameIndex);
            quarters = (it->second / 90) & 3;
            output->rotation.erase(it);
        }
    }

    uint32_t transform = 0;
    switch (quarters) {
        case 0: // no rotation
            transform = flip ? HAL_TRANSFORM_FLIP_H : 0;
            break;
        case 1: // 90 degrees counter-clockwise
            transform = flip ? (HAL_TRANSFORM_FLIP_V | HAL_TRANSFORM_ROT_90)
                    : HAL_TRANSFORM_ROT_270;
            break;
        case 2: // 180 degrees
            transform = flip ? HAL_TRANSFORM_FLIP_V : HAL_TRANSFORM_ROT_180;
            break;
        case 3: // 90 degrees clockwise
            transform = flip ? (HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_ROT_90)
                    : HAL_TRANSFORM_ROT_90;
            break;
    }

    std::shared_ptr<const C2StreamSurfaceScalingInfo::output> surfaceScaling =
        std::static_pointer_cast<const C2StreamSurfaceScalingInfo::output>(
                c2Buffer->getInfo(C2StreamSurfaceScalingInfo::output::PARAM_TYPE));
    uint32_t videoScalingMode = NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW;
    if (surfaceScaling) {
        videoScalingMode = surfaceScaling->value;
    }

    // Use dataspace from format as it has the default aspects already applied
    android_dataspace_t dataSpace = HAL_DATASPACE_UNKNOWN; // this is 0
    (void)buffer->format()->findInt32("android._dataspace", (int32_t *)&dataSpace);

    // HDR static info
    std::shared_ptr<const C2StreamHdrStaticInfo::output> hdrStaticInfo =
        std::static_pointer_cast<const C2StreamHdrStaticInfo::output>(
                c2Buffer->getInfo(C2StreamHdrStaticInfo::output::PARAM_TYPE));

    // HDR10 plus info
    std::shared_ptr<const C2StreamHdr10PlusInfo::output> hdr10PlusInfo =
        std::static_pointer_cast<const C2StreamHdr10PlusInfo::output>(
                c2Buffer->getInfo(C2StreamHdr10PlusInfo::output::PARAM_TYPE));
    if (hdr10PlusInfo && hdr10PlusInfo->flexCount() == 0) {
        hdr10PlusInfo.reset();
    }

    // HDR dynamic info
    std::shared_ptr<const C2StreamHdrDynamicMetadataInfo::output> hdrDynamicInfo =
        std::static_pointer_cast<const C2StreamHdrDynamicMetadataInfo::output>(
                c2Buffer->getInfo(C2StreamHdrDynamicMetadataInfo::output::PARAM_TYPE));
    // TODO: make this sticky & enable unset
    if (hdrDynamicInfo && hdrDynamicInfo->flexCount() == 0) {
        hdrDynamicInfo.reset();
    }

    if (hdr10PlusInfo) {
        // C2StreamHdr10PlusInfo is deprecated; components should use
        // C2StreamHdrDynamicMetadataInfo
        // TODO: #metric
        if (hdrDynamicInfo) {
            // It is unexpected that C2StreamHdr10PlusInfo and
            // C2StreamHdrDynamicMetadataInfo is both present.
            // C2StreamHdrDynamicMetadataInfo takes priority.
            // TODO: #metric
        } else {
            std::shared_ptr<C2StreamHdrDynamicMetadataInfo::output> info =
                    C2StreamHdrDynamicMetadataInfo::output::AllocShared(
                            hdr10PlusInfo->flexCount(),
                            0u,
                            C2Config::HDR_DYNAMIC_METADATA_TYPE_SMPTE_2094_40);
            memcpy(info->m.data, hdr10PlusInfo->m.value, hdr10PlusInfo->flexCount());
            hdrDynamicInfo = info;
        }
    }

    std::vector<C2ConstGraphicBlock> blocks = c2Buffer->data().graphicBlocks();
    if (blocks.size() != 1u) {
        ALOGD("[%s] expected 1 graphic block, but got %zu", mName, blocks.size());
        return UNKNOWN_ERROR;
    }
    const C2ConstGraphicBlock &block = blocks.front();
    C2Fence c2fence = block.fence();
    sp<Fence> fence = Fence::NO_FENCE;
    // TODO: it's not sufficient to just check isHW() and then construct android::fence from it.
    // Once C2Fence::type() is added, check the exact C2Fence type
    if (c2fence.isHW()) {
        int fenceFd = c2fence.fd();
        fence = sp<Fence>::make(fenceFd);
        if (!fence) {
            ALOGE("[%s] Failed to allocate a fence", mName);
            close(fenceFd);
            return NO_MEMORY;
        }
    }

    // TODO: revisit this after C2Fence implementation.
    IGraphicBufferProducer::QueueBufferInput qbi(
            timestampNs,
            false, // droppable
            dataSpace,
            Rect(blocks.front().crop().left,
                 blocks.front().crop().top,
                 blocks.front().crop().right(),
                 blocks.front().crop().bottom()),
            videoScalingMode,
            transform,
            fence, 0);
    if (hdrStaticInfo || hdrDynamicInfo) {
        HdrMetadata hdr;
        if (hdrStaticInfo) {
            // If mastering max and min luminance fields are 0, do not use them.
            // It indicates the value may not be present in the stream.
            if (hdrStaticInfo->mastering.maxLuminance > 0.0f &&
                hdrStaticInfo->mastering.minLuminance > 0.0f) {
                struct android_smpte2086_metadata smpte2086_meta = {
                    .displayPrimaryRed = {
                        hdrStaticInfo->mastering.red.x, hdrStaticInfo->mastering.red.y
                    },
                    .displayPrimaryGreen = {
                        hdrStaticInfo->mastering.green.x, hdrStaticInfo->mastering.green.y
                    },
                    .displayPrimaryBlue = {
                        hdrStaticInfo->mastering.blue.x, hdrStaticInfo->mastering.blue.y
                    },
                    .whitePoint = {
                        hdrStaticInfo->mastering.white.x, hdrStaticInfo->mastering.white.y
                    },
                    .maxLuminance = hdrStaticInfo->mastering.maxLuminance,
                    .minLuminance = hdrStaticInfo->mastering.minLuminance,
                };
                hdr.validTypes |= HdrMetadata::SMPTE2086;
                hdr.smpte2086 = smpte2086_meta;
            }
            // If the content light level fields are 0, do not use them, it
            // indicates the value may not be present in the stream.
            if (hdrStaticInfo->maxCll > 0.0f && hdrStaticInfo->maxFall > 0.0f) {
                struct android_cta861_3_metadata cta861_meta = {
                    .maxContentLightLevel = hdrStaticInfo->maxCll,
                    .maxFrameAverageLightLevel = hdrStaticInfo->maxFall,
                };
                hdr.validTypes |= HdrMetadata::CTA861_3;
                hdr.cta8613 = cta861_meta;
            }

            // does not have valid info
            if (!(hdr.validTypes & (HdrMetadata::SMPTE2086 | HdrMetadata::CTA861_3))) {
                hdrStaticInfo.reset();
            }
        }
        if (hdrDynamicInfo
                && hdrDynamicInfo->m.type_ == C2Config::HDR_DYNAMIC_METADATA_TYPE_SMPTE_2094_40) {
            hdr.validTypes |= HdrMetadata::HDR10PLUS;
            hdr.hdr10plus.assign(
                    hdrDynamicInfo->m.data,
                    hdrDynamicInfo->m.data + hdrDynamicInfo->flexCount());
        }
        qbi.setHdrMetadata(hdr);
    }
    SetMetadataToGralloc4Handle(dataSpace, hdrStaticInfo, hdrDynamicInfo, block.handle());

    qbi.setSurfaceDamage(Region::INVALID_REGION); // we don't have dirty regions
    qbi.getFrameTimestamps = true; // we need to know when a frame is rendered

    int64_t pictureProfileHandle;
    if (android::media::tv::flags::apply_picture_profiles() &&
                buffer->format()->findInt64(KEY_PICTURE_PROFILE_HANDLE, &pictureProfileHandle)) {
        PictureProfileHandle handle(static_cast<PictureProfileId>(pictureProfileHandle));
        qbi.setPictureProfileHandle(handle);
    }

    IGraphicBufferProducer::QueueBufferOutput qbo;
    status_t result = std::atomic_load(&mComponent)->queueToOutputSurface(block, qbi, &qbo);
    if (result != OK) {
        ALOGI("[%s] queueBuffer failed: %d", mName, result);
        if (result == NO_INIT) {
            mCCodecCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
        }
        return result;
    }

    if(android::base::GetBoolProperty("debug.stagefright.fps", false)) {
        ALOGD("[%s] queue buffer successful", mName);
    } else {
        ALOGV("[%s] queue buffer successful", mName);
    }

    int64_t mediaTimeUs = 0;
    (void)buffer->meta()->findInt64("timeUs", &mediaTimeUs);
    if (mAreRenderMetricsEnabled && mIsSurfaceToDisplay) {
        trackReleasedFrame(qbo, mediaTimeUs, timestampNs);
        processRenderedFrames(qbo.frameTimestamps);
    } else {
        // When the surface is an intermediate surface, onFrameRendered is triggered immediately
        // when the frame is queued to the non-display surface
        mCCodecCallback->onOutputFramesRendered(mediaTimeUs, timestampNs);
    }

    return OK;
}

void CCodecBufferChannel::initializeFrameTrackingFor(ANativeWindow * window) {
    mTrackedFrames.clear();

    int isSurfaceToDisplay = 0;
    window->query(window, NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER, &isSurfaceToDisplay);
    mIsSurfaceToDisplay = isSurfaceToDisplay == 1;
    // No frame tracking is needed if we're not sending frames to the display
    if (!mIsSurfaceToDisplay) {
        // Return early so we don't call into SurfaceFlinger (requiring permissions)
        return;
    }

    int hasPresentFenceTimes = 0;
    window->query(window, NATIVE_WINDOW_FRAME_TIMESTAMPS_SUPPORTS_PRESENT, &hasPresentFenceTimes);
    mHasPresentFenceTimes = hasPresentFenceTimes == 1;
    if (!mHasPresentFenceTimes) {
        ALOGI("Using latch times for frame rendered signals - present fences not supported");
    }
}

void CCodecBufferChannel::trackReleasedFrame(const IGraphicBufferProducer::QueueBufferOutput& qbo,
                                             int64_t mediaTimeUs, int64_t desiredRenderTimeNs) {
    // If the render time is earlier than now, then we're suggesting it should be rendered ASAP,
    // so track the frame as if the desired render time is now.
    int64_t nowNs = systemTime(SYSTEM_TIME_MONOTONIC);
    if (desiredRenderTimeNs < nowNs) {
        desiredRenderTimeNs = nowNs;
    }

    // If the render time is more than a second from now, then pretend the frame is supposed to be
    // rendered immediately, because that's what SurfaceFlinger heuristics will do. This is a tight
    // coupling, but is really the only way to optimize away unnecessary present fence checks in
    // processRenderedFrames.
    if (desiredRenderTimeNs > nowNs + 1*1000*1000*1000LL) {
        desiredRenderTimeNs = nowNs;
    }

    // We've just queued a frame to the surface, so keep track of it and later check to see if it is
    // actually rendered.
    TrackedFrame frame;
    frame.number = qbo.nextFrameNumber - 1;
    frame.mediaTimeUs = mediaTimeUs;
    frame.desiredRenderTimeNs = desiredRenderTimeNs;
    frame.latchTime = -1;
    frame.presentFence = nullptr;
    mTrackedFrames.push_back(frame);
}

void CCodecBufferChannel::processRenderedFrames(const FrameEventHistoryDelta& deltas) {
    // Grab the latch times and present fences from the frame event deltas
    for (const auto& delta : deltas) {
        for (auto& frame : mTrackedFrames) {
            if (delta.getFrameNumber() == frame.number) {
                delta.getLatchTime(&frame.latchTime);
                delta.getDisplayPresentFence(&frame.presentFence);
            }
        }
    }

    // Scan all frames and check to see if the frames that SHOULD have been rendered by now, have,
    // in fact, been rendered.
    int64_t nowNs = systemTime(SYSTEM_TIME_MONOTONIC);
    while (!mTrackedFrames.empty()) {
        TrackedFrame & frame = mTrackedFrames.front();
        // Frames that should have been rendered at least 100ms in the past are checked
        if (frame.desiredRenderTimeNs > nowNs - 100*1000*1000LL) {
            break;
        }

        // If we don't have a render time by now, then consider the frame as dropped
        int64_t renderTimeNs = getRenderTimeNs(frame);
        if (renderTimeNs != -1) {
            mCCodecCallback->onOutputFramesRendered(frame.mediaTimeUs, renderTimeNs);
        }
        mTrackedFrames.pop_front();
    }
}

int64_t CCodecBufferChannel::getRenderTimeNs(const TrackedFrame& frame) {
    // If the device doesn't have accurate present fence times, then use the latch time as a proxy
    if (!mHasPresentFenceTimes) {
        if (frame.latchTime == -1) {
            ALOGD("no latch time for frame %d", (int) frame.number);
            return -1;
        }
        return frame.latchTime;
    }

    if (frame.presentFence == nullptr) {
        ALOGW("no present fence for frame %d", (int) frame.number);
        return -1;
    }

    nsecs_t actualRenderTimeNs = frame.presentFence->getSignalTime();

    if (actualRenderTimeNs == Fence::SIGNAL_TIME_INVALID) {
        ALOGW("invalid signal time for frame %d", (int) frame.number);
        return -1;
    }

    if (actualRenderTimeNs == Fence::SIGNAL_TIME_PENDING) {
        ALOGD("present fence has not fired for frame %d", (int) frame.number);
        return -1;
    }

    return actualRenderTimeNs;
}

void CCodecBufferChannel::pollForRenderedBuffers() {
    FrameEventHistoryDelta delta;
    std::atomic_load(&mComponent)->pollForRenderedFrames(&delta);
    processRenderedFrames(delta);
}

void CCodecBufferChannel::onBufferReleasedFromOutputSurface(uint32_t generation) {
    // Note: Since this is called asynchronously from IProducerListener not
    // knowing the internal state of CCodec/CCodecBufferChannel,
    // prevent mComponent from being destroyed by holding the shared reference
    // during this interface being executed.
    std::shared_ptr<Codec2Client::Component> comp = std::atomic_load(&mComponent);
    if (comp) {
      SurfaceCallbackHandler::GetInstance().post(
                SurfaceCallbackHandler::ON_BUFFER_RELEASED, comp, generation);
    }
}

void CCodecBufferChannel::onBufferAttachedToOutputSurface(uint32_t generation) {
    // Note: Since this is called asynchronously from IProducerListener not
    // knowing the internal state of CCodec/CCodecBufferChannel,
    // prevent mComponent from being destroyed by holding the shared reference
    // during this interface being executed.
    std::shared_ptr<Codec2Client::Component> comp = std::atomic_load(&mComponent);
    if (comp) {
      SurfaceCallbackHandler::GetInstance().post(
                SurfaceCallbackHandler::ON_BUFFER_ATTACHED, comp, generation);
    }
}

status_t CCodecBufferChannel::discardBuffer(const sp<MediaCodecBuffer> &buffer) {
    ALOGV("[%s] discardBuffer: %p", mName, buffer.get());
    bool released = false;
    {
        Mutexed<Input>::Locked input(mInput);
        if (input->buffers && input->buffers->releaseBuffer(buffer, nullptr, true)) {
            released = true;
        }
    }
    {
        Mutexed<Output>::Locked output(mOutput);
        if (output->buffers && output->buffers->releaseBuffer(buffer, nullptr)) {
            released = true;
        }
    }
    if (released) {
        sendOutputBuffers();
        feedInputBufferIfAvailable();
    } else {
        ALOGD("[%s] MediaCodec discarded an unknown buffer", mName);
    }
    return OK;
}

void CCodecBufferChannel::getInputBufferArray(Vector<sp<MediaCodecBuffer>> *array) {
    array->clear();
    Mutexed<Input>::Locked input(mInput);

    if (!input->buffers) {
        ALOGE("getInputBufferArray: No Input Buffers allocated");
        return;
    }
    if (!input->buffers->isArrayMode()) {
        input->buffers = input->buffers->toArrayMode(input->numSlots);
    }

    input->buffers->getArray(array);
}

void CCodecBufferChannel::getOutputBufferArray(Vector<sp<MediaCodecBuffer>> *array) {
    array->clear();
    Mutexed<Output>::Locked output(mOutput);
    if (!output->buffers) {
        ALOGE("getOutputBufferArray: No Output Buffers allocated");
        return;
    }
    if (!output->buffers->isArrayMode()) {
        output->buffers = output->buffers->toArrayMode(output->numSlots);
    }

    output->buffers->getArray(array);
}

status_t CCodecBufferChannel::start(
        const sp<AMessage> &inputFormat,
        const sp<AMessage> &outputFormat,
        bool buffersBoundToCodec) {
    C2StreamBufferTypeSetting::input iStreamFormat(0u);
    C2StreamBufferTypeSetting::output oStreamFormat(0u);
    C2ComponentKindSetting kind;
    C2PortReorderBufferDepthTuning::output reorderDepth;
    C2PortReorderKeySetting::output reorderKey;
    C2PortActualDelayTuning::input inputDelay(0);
    C2PortActualDelayTuning::output outputDelay(0);
    C2ActualPipelineDelayTuning pipelineDelay(0);
    C2SecureModeTuning secureMode(C2Config::SM_UNPROTECTED);

    c2_status_t err = std::atomic_load(&mComponent)->query(
            {
                &iStreamFormat,
                &oStreamFormat,
                &kind,
                &reorderDepth,
                &reorderKey,
                &inputDelay,
                &pipelineDelay,
                &outputDelay,
                &secureMode,
            },
            {},
            C2_DONT_BLOCK,
            nullptr);
    if (err == C2_BAD_INDEX) {
        if (!iStreamFormat || !oStreamFormat || !kind) {
            return UNKNOWN_ERROR;
        }
    } else if (err != C2_OK) {
        return UNKNOWN_ERROR;
    }

    uint32_t inputDelayValue = inputDelay ? inputDelay.value : 0;
    uint32_t pipelineDelayValue = pipelineDelay ? pipelineDelay.value : 0;
    uint32_t outputDelayValue = outputDelay ? outputDelay.value : 0;

    size_t numInputSlots = inputDelayValue + pipelineDelayValue + kSmoothnessFactor;
    size_t numOutputSlots = outputDelayValue + kSmoothnessFactor;

    // TODO: get this from input format
    bool secure = std::atomic_load(&mComponent)->getName().find(".secure") != std::string::npos;

    // secure mode is a static parameter (shall not change in the executing state)
    mSendEncryptedInfoBuffer = secureMode.value == C2Config::SM_READ_PROTECTED_WITH_ENCRYPTED;

    std::shared_ptr<C2AllocatorStore> allocatorStore = GetCodec2PlatformAllocatorStore();
    int poolMask = GetCodec2PoolMask();
    C2PlatformAllocatorStore::id_t preferredLinearId = GetPreferredLinearAllocatorId(poolMask);

    if (inputFormat != nullptr) {
        bool graphic = (iStreamFormat.value == C2BufferData::GRAPHIC);
        bool audioEncoder = !graphic && (kind.value == C2Component::KIND_ENCODER);
        C2Config::api_feature_t apiFeatures = C2Config::api_feature_t(
                API_REFLECTION |
                API_VALUES |
                API_CURRENT_VALUES |
                API_DEPENDENCY |
                API_SAME_INPUT_BUFFER);
        C2StreamAudioFrameSizeInfo::input encoderFrameSize(0u);
        C2StreamSampleRateInfo::input sampleRate(0u);
        C2StreamChannelCountInfo::input channelCount(0u);
        C2StreamPcmEncodingInfo::input pcmEncoding(0u);
        std::shared_ptr<C2BlockPool> pool;
        {
            Mutexed<BlockPools>::Locked pools(mBlockPools);

            // set default allocator ID.
            pools->inputAllocatorId = (graphic) ? C2PlatformAllocatorStore::GRALLOC
                                                : preferredLinearId;

            // query C2PortAllocatorsTuning::input from component. If an allocator ID is obtained
            // from component, create the input block pool with given ID. Otherwise, use default IDs.
            std::vector<std::unique_ptr<C2Param>> params;
            C2ApiFeaturesSetting featuresSetting{apiFeatures};
            std::vector<C2Param *> stackParams({&featuresSetting});
            if (audioEncoder) {
                stackParams.push_back(&encoderFrameSize);
                stackParams.push_back(&sampleRate);
                stackParams.push_back(&channelCount);
                stackParams.push_back(&pcmEncoding);
            } else {
                encoderFrameSize.invalidate();
                sampleRate.invalidate();
                channelCount.invalidate();
                pcmEncoding.invalidate();
            }
            err = std::atomic_load(&mComponent)->query(stackParams,
                                    { C2PortAllocatorsTuning::input::PARAM_TYPE },
                                    C2_DONT_BLOCK,
                                    &params);
            if ((err != C2_OK && err != C2_BAD_INDEX) || params.size() != 1) {
                ALOGD("[%s] Query input allocators returned %zu params => %s (%u)",
                        mName, params.size(), asString(err), err);
            } else if (params.size() == 1) {
                C2PortAllocatorsTuning::input *inputAllocators =
                    C2PortAllocatorsTuning::input::From(params[0].get());
                if (inputAllocators && inputAllocators->flexCount() > 0) {
                    std::shared_ptr<C2Allocator> allocator;
                    // verify allocator IDs and resolve default allocator
                    allocatorStore->fetchAllocator(inputAllocators->m.values[0], &allocator);
                    if (allocator) {
                        pools->inputAllocatorId = allocator->getId();
                    } else {
                        ALOGD("[%s] component requested invalid input allocator ID %u",
                                mName, inputAllocators->m.values[0]);
                    }
                }
            }
            if (featuresSetting) {
                apiFeatures = featuresSetting.value;
            }

            // TODO: use C2Component wrapper to associate this pool with ourselves
            if ((poolMask >> pools->inputAllocatorId) & 1) {
                err = CreateCodec2BlockPool(pools->inputAllocatorId, nullptr, &pool);
                ALOGD("[%s] Created input block pool with allocatorID %u => poolID %llu - %s (%d)",
                        mName, pools->inputAllocatorId,
                        (unsigned long long)(pool ? pool->getLocalId() : 111000111),
                        asString(err), err);
            } else {
                err = C2_NOT_FOUND;
            }
            if (err != C2_OK) {
                C2BlockPool::local_id_t inputPoolId =
                    graphic ? C2BlockPool::BASIC_GRAPHIC : C2BlockPool::BASIC_LINEAR;
                err = GetCodec2BlockPool(inputPoolId, nullptr, &pool);
                ALOGD("[%s] Using basic input block pool with poolID %llu => got %llu - %s (%d)",
                        mName, (unsigned long long)inputPoolId,
                        (unsigned long long)(pool ? pool->getLocalId() : 111000111),
                        asString(err), err);
                if (err != C2_OK) {
                    return NO_MEMORY;
                }
            }
            pools->inputPool = pool;
        }

        bool forceArrayMode = false;
        Mutexed<Input>::Locked input(mInput);
        input->inputDelay = inputDelayValue;
        input->pipelineDelay = pipelineDelayValue;
        input->numSlots = numInputSlots;
        input->extraBuffers.flush();
        input->numExtraSlots = 0u;
        input->lastFlushIndex = mFrameIndex.load(std::memory_order_relaxed);
        if (audioEncoder && encoderFrameSize && sampleRate && channelCount) {
            input->frameReassembler.init(
                    pool,
                    {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE},
                    encoderFrameSize.value,
                    sampleRate.value,
                    channelCount.value,
                    pcmEncoding ? pcmEncoding.value : C2Config::PCM_16);
        }
        if (!buffersBoundToCodec) {
            inputFormat->setInt32(KEY_NUM_SLOTS, numInputSlots);
        }
        bool conforming = (apiFeatures & API_SAME_INPUT_BUFFER);
        // For encrypted content, framework decrypts source buffer (ashmem) into
        // C2Buffers. Thus non-conforming codecs can process these.
        if (!buffersBoundToCodec
                && !input->frameReassembler
                && (hasCryptoOrDescrambler() || conforming)) {
            input->buffers.reset(new SlotInputBuffers(mName));
        } else if (graphic) {
            if (mHasInputSurface) {
                input->buffers.reset(new DummyInputBuffers(mName));
            } else if (mMetaMode == MODE_ANW) {
                input->buffers.reset(new GraphicMetadataInputBuffers(mName));
                // This is to ensure buffers do not get released prematurely.
                // TODO: handle this without going into array mode
                forceArrayMode = true;
            } else {
                input->buffers.reset(new GraphicInputBuffers(mName));
            }
        } else {
            if (hasCryptoOrDescrambler()) {
                int32_t capacity = kLinearBufferSize;
                (void)inputFormat->findInt32(KEY_MAX_INPUT_SIZE, &capacity);
                if ((size_t)capacity > kMaxLinearBufferSize) {
                    ALOGD("client requested %d, capped to %zu", capacity, kMaxLinearBufferSize);
                    capacity = kMaxLinearBufferSize;
                }
                if (mDealer == nullptr) {
                    mDealer = new MemoryDealer(
                            align(capacity, MemoryDealer::getAllocationAlignment())
                                * (numInputSlots + 1),
                            "EncryptedLinearInputBuffers");
                    mDecryptDestination = mDealer->allocate((size_t)capacity);
                }
                if (mCrypto != nullptr && mHeapSeqNum < 0) {
                    sp<HidlMemory> heap = fromHeap(mDealer->getMemoryHeap());
                    mHeapSeqNum = mCrypto->setHeap(heap);
                } else {
                    mHeapSeqNum = -1;
                }
                input->buffers.reset(new EncryptedLinearInputBuffers(
                        secure, mDealer, mCrypto, mHeapSeqNum, (size_t)capacity,
                        numInputSlots, mName));
                forceArrayMode = true;
            } else {
                input->buffers.reset(new LinearInputBuffers(mName));
            }
        }
        input->buffers->setFormat(inputFormat);

        if (err == C2_OK) {
            input->buffers->setPool(pool);
        } else {
            // TODO: error
        }

        if (forceArrayMode) {
            input->buffers = input->buffers->toArrayMode(numInputSlots);
        }
    }

    if (outputFormat != nullptr) {
        sp<IGraphicBufferProducer> outputSurface;
        uint32_t outputGeneration;
        int maxDequeueCount = 0;
        {
            Mutexed<OutputSurface>::Locked output(mOutputSurface);
            maxDequeueCount = output->maxDequeueBuffers = numOutputSlots +
                    reorderDepth.value + mRenderingDepth;
            outputSurface = output->surface ?
                    output->surface->getIGraphicBufferProducer() : nullptr;
            if (outputSurface) {
                (void)SurfaceCallbackHandler::GetInstance();
                output->surface->setMaxDequeuedBufferCount(output->maxDequeueBuffers);
            }
            outputGeneration = output->generation;
        }

        bool graphic = (oStreamFormat.value == C2BufferData::GRAPHIC);
        C2BlockPool::local_id_t outputPoolId_;
        C2BlockPool::local_id_t prevOutputPoolId;

        {
            Mutexed<BlockPools>::Locked pools(mBlockPools);

            prevOutputPoolId = pools->outputPoolId;

            // set default allocator ID.
            pools->outputAllocatorId = (graphic) ? C2PlatformAllocatorStore::GRALLOC
                                                 : preferredLinearId;

            // query C2PortAllocatorsTuning::output from component, or use default allocator if
            // unsuccessful.
            std::vector<std::unique_ptr<C2Param>> params;
            err = std::atomic_load(&mComponent)->query({ },
                                    { C2PortAllocatorsTuning::output::PARAM_TYPE },
                                    C2_DONT_BLOCK,
                                    &params);
            if ((err != C2_OK && err != C2_BAD_INDEX) || params.size() != 1) {
                ALOGD("[%s] Query output allocators returned %zu params => %s (%u)",
                        mName, params.size(), asString(err), err);
            } else if (err == C2_OK && params.size() == 1) {
                C2PortAllocatorsTuning::output *outputAllocators =
                    C2PortAllocatorsTuning::output::From(params[0].get());
                if (outputAllocators && outputAllocators->flexCount() > 0) {
                    std::shared_ptr<C2Allocator> allocator;
                    // verify allocator IDs and resolve default allocator
                    allocatorStore->fetchAllocator(outputAllocators->m.values[0], &allocator);
                    if (allocator) {
                        pools->outputAllocatorId = allocator->getId();
                    } else {
                        ALOGD("[%s] component requested invalid output allocator ID %u",
                                mName, outputAllocators->m.values[0]);
                    }
                }
            }

            // use bufferqueue if outputting to a surface.
            // query C2PortSurfaceAllocatorTuning::output from component, or use default allocator
            // if unsuccessful.
            if (outputSurface) {
                params.clear();
                err = std::atomic_load(&mComponent)->query({ },
                                        { C2PortSurfaceAllocatorTuning::output::PARAM_TYPE },
                                        C2_DONT_BLOCK,
                                        &params);
                if ((err != C2_OK && err != C2_BAD_INDEX) || params.size() != 1) {
                    ALOGD("[%s] Query output surface allocator returned %zu params => %s (%u)",
                            mName, params.size(), asString(err), err);
                } else if (err == C2_OK && params.size() == 1) {
                    C2PortSurfaceAllocatorTuning::output *surfaceAllocator =
                        C2PortSurfaceAllocatorTuning::output::From(params[0].get());
                    if (surfaceAllocator) {
                        std::shared_ptr<C2Allocator> allocator;
                        // verify allocator IDs and resolve default allocator
                        allocatorStore->fetchAllocator(surfaceAllocator->value, &allocator);
                        if (allocator) {
                            pools->outputAllocatorId = allocator->getId();
                        } else {
                            ALOGD("[%s] component requested invalid surface output allocator ID %u",
                                    mName, surfaceAllocator->value);
                            err = C2_BAD_VALUE;
                        }
                    }
                }
                if (pools->outputAllocatorId == C2PlatformAllocatorStore::GRALLOC
                        && err != C2_OK
                        && ((poolMask >> C2PlatformAllocatorStore::BUFFERQUEUE) & 1)) {
                    pools->outputAllocatorId = C2PlatformAllocatorStore::BUFFERQUEUE;
                }
            }

            if ((poolMask >> pools->outputAllocatorId) & 1) {
                err = std::atomic_load(&mComponent)->createBlockPool(
                        pools->outputAllocatorId, &pools->outputPoolId, &pools->outputPoolIntf);
                ALOGI("[%s] Created output block pool with allocatorID %u => poolID %llu - %s",
                        mName, pools->outputAllocatorId,
                        (unsigned long long)pools->outputPoolId,
                        asString(err));
            } else {
                err = C2_NOT_FOUND;
            }
            if (err != C2_OK) {
                // use basic pool instead
                pools->outputPoolId =
                    graphic ? C2BlockPool::BASIC_GRAPHIC : C2BlockPool::BASIC_LINEAR;
            }

            // Configure output block pool ID as parameter C2PortBlockPoolsTuning::output to
            // component.
            std::unique_ptr<C2PortBlockPoolsTuning::output> poolIdsTuning =
                    C2PortBlockPoolsTuning::output::AllocUnique({ pools->outputPoolId });

            std::vector<std::unique_ptr<C2SettingResult>> failures;
            err = std::atomic_load(&mComponent)->config(
                    { poolIdsTuning.get() }, C2_MAY_BLOCK, &failures);
            ALOGD("[%s] Configured output block pool ids %llu => %s",
                    mName, (unsigned long long)poolIdsTuning->m.values[0], asString(err));
            outputPoolId_ = pools->outputPoolId;
        }

        if (prevOutputPoolId != C2BlockPool::BASIC_LINEAR
                && prevOutputPoolId != C2BlockPool::BASIC_GRAPHIC) {
            c2_status_t err = std::atomic_load(&mComponent)->destroyBlockPool(prevOutputPoolId);
            if (err != C2_OK) {
                ALOGW("Failed to clean up previous block pool %llu - %s (%d)\n",
                        (unsigned long long) prevOutputPoolId, asString(err), err);
            }
        }

        Mutexed<Output>::Locked output(mOutput);
        output->outputDelay = outputDelayValue;
        output->numSlots = numOutputSlots;
        output->bounded = bool(outputSurface);
        if (graphic) {
            if (outputSurface || !buffersBoundToCodec) {
                output->buffers.reset(new GraphicOutputBuffers(mName));
            } else {
                output->buffers.reset(new RawGraphicOutputBuffers(mName));
            }
        } else {
            output->buffers.reset(new LinearOutputBuffers(mName));
        }
        output->buffers->setFormat(outputFormat);

        output->buffers->clearStash();
        if (reorderDepth) {
            output->buffers->setReorderDepth(reorderDepth.value);
        }
        if (reorderKey) {
            output->buffers->setReorderKey(reorderKey.value);
        }

        // Try to set output surface to created block pool if given.
        if (outputSurface) {
            std::atomic_load(&mComponent)->setOutputSurface(
                    outputPoolId_,
                    outputSurface,
                    outputGeneration,
                    maxDequeueCount);
        } else {
            // configure CPU read consumer usage
            C2StreamUsageTuning::output outputUsage{0u, C2MemoryUsage::CPU_READ};
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            err = std::atomic_load(&mComponent)->config({ &outputUsage }, C2_MAY_BLOCK, &failures);
            // do not print error message for now as most components may not yet
            // support this setting
            ALOGD_IF(err != C2_BAD_INDEX, "[%s] Configured output usage [%#llx]",
                  mName, (long long)outputUsage.value);
        }

        if (oStreamFormat.value == C2BufferData::LINEAR) {
            if (buffersBoundToCodec) {
                // WORKAROUND: if we're using early CSD workaround we convert to
                //             array mode, to appease apps assuming the output
                //             buffers to be of the same size.
                output->buffers = output->buffers->toArrayMode(numOutputSlots);
            }

            int32_t channelCount;
            int32_t sampleRate;
            if (outputFormat->findInt32(KEY_CHANNEL_COUNT, &channelCount)
                    && outputFormat->findInt32(KEY_SAMPLE_RATE, &sampleRate)) {
                int32_t delay = 0;
                int32_t padding = 0;;
                if (!outputFormat->findInt32("encoder-delay", &delay)) {
                    delay = 0;
                }
                if (!outputFormat->findInt32("encoder-padding", &padding)) {
                    padding = 0;
                }
                if (delay || padding) {
                    // We need write access to the buffers, so turn them into array mode.
                    // TODO: b/321930152 - define SkipCutOutputBuffers that takes output from
                    // component, runs it through SkipCutBuffer and allocate local buffer to be
                    // used by fwk. Make initSkipCutBuffer() return OutputBuffers similar to
                    // toArrayMode().
                    if (!output->buffers->isArrayMode()) {
                        output->buffers = output->buffers->toArrayMode(numOutputSlots);
                    }
                    output->buffers->initSkipCutBuffer(delay, padding, sampleRate, channelCount);
                }
            }
        }

        int32_t tunneled = 0;
        if (!outputFormat->findInt32("android._tunneled", &tunneled)) {
            tunneled = 0;
        }
        mTunneled = (tunneled != 0);
    }

    // Set up pipeline control. This has to be done after mInputBuffers and
    // mOutputBuffers are initialized to make sure that lingering callbacks
    // about buffers from the previous generation do not interfere with the
    // newly initialized pipeline capacity.

    if (inputFormat || outputFormat) {
        Mutexed<PipelineWatcher>::Locked watcher(mPipelineWatcher);
        watcher->inputDelay(inputDelayValue)
                .pipelineDelay(pipelineDelayValue)
                .outputDelay(outputDelayValue)
                .smoothnessFactor(kSmoothnessFactor)
                .tunneled(mTunneled);
        watcher->flush();
    }

    mInputMetEos = false;
    mSync.start();
    return OK;
}

status_t CCodecBufferChannel::prepareInitialInputBuffers(
        std::map<size_t, sp<MediaCodecBuffer>> *clientInputBuffers, bool retry) {
    if (mHasInputSurface) {
        return OK;
    }

    size_t numInputSlots = mInput.lock()->numSlots;
    int retryCount = 1;
    for (; clientInputBuffers->empty() && retryCount >= 0; retryCount--) {
        {
            Mutexed<Input>::Locked input(mInput);
            while (clientInputBuffers->size() < numInputSlots) {
                size_t index;
                sp<MediaCodecBuffer> buffer;
                if (!input->buffers->requestNewBuffer(&index, &buffer)) {
                    break;
                }
                clientInputBuffers->emplace(index, buffer);
            }
        }
        if (!retry || (retryCount <= 0)) {
            break;
        }
        if (clientInputBuffers->empty()) {
            // wait: buffer may be in transit from component.
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
    if (clientInputBuffers->empty()) {
        ALOGW("[%s] start: cannot allocate memory at all", mName);
        return NO_MEMORY;
    } else if (clientInputBuffers->size() < numInputSlots) {
        ALOGD("[%s] start: cannot allocate memory for all slots, "
              "only %zu buffers allocated",
              mName, clientInputBuffers->size());
    } else {
        ALOGV("[%s] %zu initial input buffers available",
              mName, clientInputBuffers->size());
    }
    return OK;
}

status_t CCodecBufferChannel::requestInitialInputBuffers(
        std::map<size_t, sp<MediaCodecBuffer>> &&clientInputBuffers) {
    std::optional<QueueGuard> guard;
    if (android::media::codec::provider_->codec_buffer_state_cleanup()) {
        guard.emplace(mSync);
        if (!guard->isRunning()) {
            ALOGD("[%s] skip requestInitialInputBuffers when not running", mName);
            return OK;
        }
    }
    C2StreamBufferTypeSetting::output oStreamFormat(0u);
    C2PrependHeaderModeSetting prepend(PREPEND_HEADER_TO_NONE);
    c2_status_t err = std::atomic_load(&mComponent)->query(
            { &oStreamFormat, &prepend }, {}, C2_DONT_BLOCK, nullptr);
    if (err != C2_OK && err != C2_BAD_INDEX) {
        return UNKNOWN_ERROR;
    }

    std::list<std::unique_ptr<C2Work>> flushedConfigs;
    mFlushedConfigs.lock()->swap(flushedConfigs);
    if (!flushedConfigs.empty()) {
        {
            Mutexed<PipelineWatcher>::Locked watcher(mPipelineWatcher);
            PipelineWatcher::Clock::time_point now = PipelineWatcher::Clock::now();
            for (const std::unique_ptr<C2Work> &work : flushedConfigs) {
                watcher->onWorkQueued(
                        work->input.ordinal.frameIndex.peeku(),
                        std::vector(work->input.buffers),
                        now);
            }
        }
        err = std::atomic_load(&mComponent)->queue(&flushedConfigs);
        if (err != C2_OK) {
            ALOGW("[%s] Error while queueing a flushed config", mName);
            return UNKNOWN_ERROR;
        }
    }
    if (oStreamFormat.value == C2BufferData::LINEAR &&
            (!prepend || prepend.value == PREPEND_HEADER_TO_NONE) &&
            !clientInputBuffers.empty()) {
        size_t minIndex = clientInputBuffers.begin()->first;
        sp<MediaCodecBuffer> minBuffer = clientInputBuffers.begin()->second;
        for (const auto &[index, buffer] : clientInputBuffers) {
            if (minBuffer->capacity() > buffer->capacity()) {
                minIndex = index;
                minBuffer = buffer;
            }
        }
        // WORKAROUND: Some apps expect CSD available without queueing
        //             any input. Queue an empty buffer to get the CSD.
        minBuffer->setRange(0, 0);
        minBuffer->meta()->clear();
        minBuffer->meta()->setInt64("timeUs", 0);
        if (queueInputBufferInternal(minBuffer) != OK) {
            ALOGW("[%s] Error while queueing an empty buffer to get CSD",
                  mName);
            return UNKNOWN_ERROR;
        }
        clientInputBuffers.erase(minIndex);
    }

    if (!clientInputBuffers.empty()) {
        {
            std::lock_guard<std::mutex> tsLock(mTsLock);
            mLastInputBufferAvailableTs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    PipelineWatcher::Clock::now().time_since_epoch()).count();
        }
    }

    for (const auto &[index, buffer] : clientInputBuffers) {
        mCallback->onInputBufferAvailable(index, buffer);
    }

    return OK;
}

void CCodecBufferChannel::stop() {
    mSync.stop();
    mFirstValidFrameIndex = mFrameIndex.load(std::memory_order_relaxed);
    mInfoBuffers.clear();
}

void CCodecBufferChannel::stopUseOutputSurface(bool pushBlankBuffer) {
    sp<Surface> surface = mOutputSurface.lock()->surface;
    if (surface) {
        C2BlockPool::local_id_t outputPoolId;
        {
            Mutexed<BlockPools>::Locked pools(mBlockPools);
            outputPoolId = pools->outputPoolId;
        }
        std::shared_ptr<Codec2Client::Component> comp = std::atomic_load(&mComponent);
        if (comp) comp->stopUsingOutputSurface(outputPoolId);

        if (pushBlankBuffer) {
            sp<ANativeWindow> anw = static_cast<ANativeWindow *>(surface.get());
            if (anw) {
                pushBlankBuffersToNativeWindow(anw.get());
            }
        }
    }
}

void CCodecBufferChannel::reset() {
    stop();
    mPipelineWatcher.lock()->flush();
    {
        mHasInputSurface = false;
        Mutexed<InputSurface>::Locked inputSurface(mInputSurface);
        inputSurface->surface.reset();
    }
    {
        Mutexed<Input>::Locked input(mInput);
        input->buffers.reset(new DummyInputBuffers(""));
        input->extraBuffers.flush();
    }
    {
        Mutexed<Output>::Locked output(mOutput);
        output->buffers.reset();
    }
    // reset the frames that are being tracked for onFrameRendered callbacks
    mTrackedFrames.clear();
}

void CCodecBufferChannel::release() {
    mInfoBuffers.clear();
    std::shared_ptr<Codec2Client::Component> nullComp;
    std::atomic_store(&mComponent, nullComp);
    mInputAllocator.reset();
    mOutputSurface.lock()->surface.clear();
    {
        Mutexed<BlockPools>::Locked blockPools{mBlockPools};
        blockPools->inputPool.reset();
        blockPools->outputPoolIntf.reset();
    }
    setCrypto(nullptr);
    setDescrambler(nullptr);
}

void CCodecBufferChannel::flush(const std::list<std::unique_ptr<C2Work>> &flushedWork) {
    ALOGV("[%s] flush", mName);
    std::list<std::unique_ptr<C2Work>> configs;
    mInput.lock()->lastFlushIndex = mFrameIndex.load(std::memory_order_relaxed);
    {
        Mutexed<PipelineWatcher>::Locked watcher(mPipelineWatcher);
        for (const std::unique_ptr<C2Work> &work : flushedWork) {
            uint64_t frameIndex = work->input.ordinal.frameIndex.peeku();
            if (!(work->input.flags & C2FrameData::FLAG_CODEC_CONFIG)) {
                watcher->onWorkDone(frameIndex);
                continue;
            }
            if (work->input.buffers.empty()
                    || work->input.buffers.front() == nullptr
                    || work->input.buffers.front()->data().linearBlocks().empty()) {
                ALOGD("[%s] no linear codec config data found", mName);
                watcher->onWorkDone(frameIndex);
                continue;
            }
            std::unique_ptr<C2Work> copy(new C2Work);
            copy->input.flags = C2FrameData::flags_t(
                    work->input.flags | C2FrameData::FLAG_DROP_FRAME);
            copy->input.ordinal = work->input.ordinal;
            copy->input.ordinal.frameIndex = mFrameIndex++;
            for (size_t i = 0; i < work->input.buffers.size(); ++i) {
                copy->input.buffers.push_back(watcher->onInputBufferReleased(frameIndex, i));
            }
            for (const std::unique_ptr<C2Param> &param : work->input.configUpdate) {
                copy->input.configUpdate.push_back(C2Param::Copy(*param));
            }
            copy->input.infoBuffers.insert(
                    copy->input.infoBuffers.begin(),
                    work->input.infoBuffers.begin(),
                    work->input.infoBuffers.end());
            copy->worklets.emplace_back(new C2Worklet);
            configs.push_back(std::move(copy));
            watcher->onWorkDone(frameIndex);
            ALOGV("[%s] stashed flushed codec config data", mName);
        }
    }
    mFlushedConfigs.lock()->swap(configs);
    {
        Mutexed<Input>::Locked input(mInput);
        input->buffers->flush();
        input->extraBuffers.flush();
    }
    {
        Mutexed<Output>::Locked output(mOutput);
        if (output->buffers) {
            output->buffers->flush(flushedWork);
            output->buffers->flushStash();
        }
    }
    mInfoBuffers.clear();
}

void CCodecBufferChannel::onWorkDone(
        std::unique_ptr<C2Work> work,
        const sp<AMessage> &inputFormat,
        const sp<AMessage> &outputFormat,
        const C2StreamInitDataInfo::output *initData) {
    std::string traceStr;
    if (ATRACE_ENABLED()) {
        traceStr = android::base::StringPrintf(
                "CCodecBufferChannel::onWorkDone-%s", mName).c_str();
    }
    ScopedTrace trace(ATRACE_TAG, traceStr.c_str());
    if (handleWork(std::move(work), inputFormat, outputFormat, initData)) {
        feedInputBufferIfAvailable();
    }
}

void CCodecBufferChannel::onInputBufferDone(
        uint64_t frameIndex, size_t arrayIndex) {
    std::shared_ptr<C2Buffer> buffer =
            mPipelineWatcher.lock()->onInputBufferReleased(frameIndex, arrayIndex);
    bool newInputSlotAvailable = false;
    {
        Mutexed<Input>::Locked input(mInput);
        if (input->lastFlushIndex >= frameIndex) {
            ALOGD("[%s] Ignoring stale input buffer done callback: "
                  "last flush index = %lld, frameIndex = %lld",
                  mName, input->lastFlushIndex.peekll(), (long long)frameIndex);
        } else {
            newInputSlotAvailable = input->buffers->expireComponentBuffer(buffer);
            if (!newInputSlotAvailable) {
                (void)input->extraBuffers.expireComponentBuffer(buffer);
            }
        }
    }
    if (newInputSlotAvailable) {
        feedInputBufferIfAvailable();
    }
}

bool CCodecBufferChannel::handleWork(
        std::unique_ptr<C2Work> work,
        const sp<AMessage> &inputFormat,
        const sp<AMessage> &outputFormat,
        const C2StreamInitDataInfo::output *initData) {
    std::string traceStr;
    if (ATRACE_ENABLED()) {
        traceStr = android::base::StringPrintf(
                "CCodecBufferChannel::handleWork-%s", mName).c_str();
    }
    ScopedTrace atrace(ATRACE_TAG, traceStr.c_str());
    {
        Mutexed<Output>::Locked output(mOutput);
        if (!output->buffers) {
            return false;
        }
    }

    // Whether the output buffer should be reported to the client or not.
    bool notifyClient = false;

    if (work->result == C2_OK){
        notifyClient = true;
    } else if (work->result == C2_OMITTED) {
        ALOGV("[%s] empty work returned; omitted.", mName);
        return false; // omitted
    } else if (work->result == C2_NOT_FOUND) {
        if (work->input.flags & C2FrameData::FLAG_DROP_FRAME) {
            // NOTE: This is to solve backward compatibility issue of queueDummyWork. If no HAL fix,
            //       we will receive C2_NOT_FOUND here and then issue fatal error to MediaCodec
            ALOGV("[%s] empty work returned; omitted.", mName);
            return false; // omitted
        }
        ALOGD("[%s] flushed work; ignored.", mName);
    } else {
        // C2_OK and C2_NOT_FOUND are the only results that we accept for processing
        // the config update.
        ALOGD("[%s] work failed to complete: %d", mName, work->result);
        mCCodecCallback->onError(work->result, ACTION_CODE_FATAL);
        return false;
    }

    if ((work->input.ordinal.frameIndex -
            mFirstValidFrameIndex.load()).peek() < 0) {
        // Discard frames from previous generation.
        ALOGD("[%s] Discard frames from previous generation.", mName);
        notifyClient = false;
    }

    if (!mHasInputSurface && (work->worklets.size() != 1u
            || !work->worklets.front()
            || !(work->worklets.front()->output.flags &
                 C2FrameData::FLAG_INCOMPLETE))) {
        mPipelineWatcher.lock()->onWorkDone(
                work->input.ordinal.frameIndex.peeku());
    }

    // NOTE: MediaCodec usage supposedly have only one worklet
    if (work->worklets.size() != 1u) {
        ALOGI("[%s] onWorkDone: incorrect number of worklets: %zu",
                mName, work->worklets.size());
        mCCodecCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
        return false;
    }

    const std::unique_ptr<C2Worklet> &worklet = work->worklets.front();

    std::shared_ptr<C2Buffer> buffer;
    // NOTE: MediaCodec usage supposedly have only one output stream.
    if (worklet->output.buffers.size() > 1u) {
        ALOGI("[%s] onWorkDone: incorrect number of output buffers: %zu",
                mName, worklet->output.buffers.size());
        mCCodecCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
        return false;
    } else if (worklet->output.buffers.size() == 1u) {
        buffer = worklet->output.buffers[0];
        if (!buffer) {
            ALOGD("[%s] onWorkDone: nullptr found in buffers; ignored.", mName);
        }
    }

    std::optional<uint32_t> newInputDelay, newPipelineDelay, newOutputDelay, newReorderDepth;
    std::optional<C2Config::ordinal_key_t> newReorderKey;
    bool needMaxDequeueBufferCountUpdate = false;
    while (!worklet->output.configUpdate.empty()) {
        std::unique_ptr<C2Param> param;
        worklet->output.configUpdate.back().swap(param);
        worklet->output.configUpdate.pop_back();
        switch (param->coreIndex().coreIndex()) {
            case C2PortReorderBufferDepthTuning::CORE_INDEX: {
                C2PortReorderBufferDepthTuning::output reorderDepth;
                if (reorderDepth.updateFrom(*param)) {
                    ALOGV("[%s] onWorkDone: updated reorder depth to %u",
                          mName, reorderDepth.value);
                    newReorderDepth = reorderDepth.value;
                    needMaxDequeueBufferCountUpdate = true;
                } else {
                    ALOGD("[%s] onWorkDone: failed to read reorder depth",
                          mName);
                }
                break;
            }
            case C2PortReorderKeySetting::CORE_INDEX: {
                C2PortReorderKeySetting::output reorderKey;
                if (reorderKey.updateFrom(*param)) {
                    newReorderKey = reorderKey.value;
                    ALOGV("[%s] onWorkDone: updated reorder key to %u",
                          mName, reorderKey.value);
                } else {
                    ALOGD("[%s] onWorkDone: failed to read reorder key", mName);
                }
                break;
            }
            case C2PortActualDelayTuning::CORE_INDEX: {
                if (param->isGlobal()) {
                    C2ActualPipelineDelayTuning pipelineDelay;
                    if (pipelineDelay.updateFrom(*param)) {
                        ALOGV("[%s] onWorkDone: updating pipeline delay %u",
                              mName, pipelineDelay.value);
                        newPipelineDelay = pipelineDelay.value;
                        (void)mPipelineWatcher.lock()->pipelineDelay(
                                pipelineDelay.value);
                    }
                }
                if (param->forInput()) {
                    C2PortActualDelayTuning::input inputDelay;
                    if (inputDelay.updateFrom(*param)) {
                        ALOGV("[%s] onWorkDone: updating input delay %u",
                              mName, inputDelay.value);
                        newInputDelay = inputDelay.value;
                        (void)mPipelineWatcher.lock()->inputDelay(
                                inputDelay.value);
                    }
                }
                if (param->forOutput()) {
                    C2PortActualDelayTuning::output outputDelay;
                    if (outputDelay.updateFrom(*param)) {
                        ALOGV("[%s] onWorkDone: updating output delay %u",
                              mName, outputDelay.value);
                        (void)mPipelineWatcher.lock()->outputDelay(outputDelay.value);
                        newOutputDelay = outputDelay.value;
                        needMaxDequeueBufferCountUpdate = true;

                    }
                }
                break;
            }
            case C2PortTunnelSystemTime::CORE_INDEX: {
                C2PortTunnelSystemTime::output frameRenderTime;
                if (frameRenderTime.updateFrom(*param)) {
                    ALOGV("[%s] onWorkDone: frame rendered (sys:%lld ns, media:%lld us)",
                          mName, (long long)frameRenderTime.value,
                          (long long)worklet->output.ordinal.timestamp.peekll());
                    mCCodecCallback->onOutputFramesRendered(
                            worklet->output.ordinal.timestamp.peek(), frameRenderTime.value);
                }
                break;
            }
            case C2StreamTunnelHoldRender::CORE_INDEX: {
                C2StreamTunnelHoldRender::output firstTunnelFrameHoldRender;
                if (!(worklet->output.flags & C2FrameData::FLAG_INCOMPLETE)) break;
                if (!firstTunnelFrameHoldRender.updateFrom(*param)) break;
                if (firstTunnelFrameHoldRender.value != C2_TRUE) break;
                ALOGV("[%s] onWorkDone: first tunnel frame ready", mName);
                mCCodecCallback->onFirstTunnelFrameReady();
                break;
            }
            default:
                ALOGV("[%s] onWorkDone: unrecognized config update (%08X)",
                      mName, param->index());
                break;
        }
    }
    if (newInputDelay || newPipelineDelay) {
        Mutexed<Input>::Locked input(mInput);
        size_t newNumSlots =
            newInputDelay.value_or(input->inputDelay) +
            newPipelineDelay.value_or(input->pipelineDelay) +
            kSmoothnessFactor;
        input->inputDelay = newInputDelay.value_or(input->inputDelay);
        if (input->buffers->isArrayMode()) {
            if (input->numSlots >= newNumSlots) {
                input->numExtraSlots = 0;
            } else {
                input->numExtraSlots = newNumSlots - input->numSlots;
            }
            ALOGV("[%s] onWorkDone: updated number of extra slots to %zu (input array mode)",
                  mName, input->numExtraSlots);
        } else {
            input->numSlots = newNumSlots;
        }
        if (inputFormat->contains(KEY_NUM_SLOTS)) {
            inputFormat->setInt32(KEY_NUM_SLOTS, input->numSlots);
        }
    }
    size_t numOutputSlots = 0;
    uint32_t reorderDepth = 0;
    bool outputBuffersChanged = false;
    if (newReorderKey || newReorderDepth || needMaxDequeueBufferCountUpdate) {
        Mutexed<Output>::Locked output(mOutput);
        if (!output->buffers) {
            return false;
        }
        numOutputSlots = output->numSlots;
        if (newReorderKey) {
            output->buffers->setReorderKey(newReorderKey.value());
        }
        if (newReorderDepth) {
            output->buffers->setReorderDepth(newReorderDepth.value());
        }
        reorderDepth = output->buffers->getReorderDepth();
        if (newOutputDelay) {
            output->outputDelay = newOutputDelay.value();
            numOutputSlots = newOutputDelay.value() + kSmoothnessFactor;
            if (output->numSlots < numOutputSlots) {
                output->numSlots = numOutputSlots;
                if (output->buffers->isArrayMode()) {
                    OutputBuffersArray *array =
                        (OutputBuffersArray *)output->buffers.get();
                    ALOGV("[%s] onWorkDone: growing output buffer array to %zu",
                          mName, numOutputSlots);
                    array->grow(numOutputSlots);
                    outputBuffersChanged = true;
                }
            }
        }
        numOutputSlots = output->numSlots;
    }
    if (outputBuffersChanged) {
        mCCodecCallback->onOutputBuffersChanged();
    }
    if (needMaxDequeueBufferCountUpdate) {
        int maxDequeueCount = 0;
        {
            Mutexed<OutputSurface>::Locked output(mOutputSurface);
            maxDequeueCount = output->maxDequeueBuffers =
                    numOutputSlots + reorderDepth + mRenderingDepth;
            if (output->surface) {
                output->surface->setMaxDequeuedBufferCount(output->maxDequeueBuffers);
            }
        }
        if (maxDequeueCount > 0) {
            std::atomic_load(&mComponent)->setOutputSurfaceMaxDequeueCount(maxDequeueCount);
        }
    }

    int32_t flags = 0;
    if (worklet->output.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= BUFFER_FLAG_END_OF_STREAM;
        ALOGV("[%s] onWorkDone: output EOS", mName);
    }

    // WORKAROUND: adjust output timestamp based on client input timestamp and codec
    // input timestamp. Codec output timestamp (in the timestamp field) shall correspond to
    // the codec input timestamp, but client output timestamp should (reported in timeUs)
    // shall correspond to the client input timesamp (in customOrdinal). By using the
    // delta between the two, this allows for some timestamp deviation - e.g. if one input
    // produces multiple output.
    c2_cntr64_t timestamp =
        worklet->output.ordinal.timestamp + work->input.ordinal.customOrdinal
                - work->input.ordinal.timestamp;
    if (mHasInputSurface) {
        // When using input surface we need to restore the original input timestamp.
        timestamp = work->input.ordinal.customOrdinal;
    }
    ScopedTrace trace(ATRACE_TAG, android::base::StringPrintf(
            "CCodecBufferChannel::onWorkDone(%s@ts=%lld)", mName, timestamp.peekll()).c_str());
    ALOGV("[%s] onWorkDone: input %lld, codec %lld => output %lld => %lld",
          mName,
          work->input.ordinal.customOrdinal.peekll(),
          work->input.ordinal.timestamp.peekll(),
          worklet->output.ordinal.timestamp.peekll(),
          timestamp.peekll());

    // csd cannot be re-ordered and will always arrive first.
    if (initData != nullptr) {
        Mutexed<Output>::Locked output(mOutput);
        if (!output->buffers) {
            return false;
        }
        if (outputFormat) {
            output->buffers->updateSkipCutBuffer(outputFormat);
            output->buffers->setFormat(outputFormat);
        }
        if (!notifyClient) {
            return false;
        }
        size_t index;
        sp<MediaCodecBuffer> outBuffer;
        if (output->buffers->registerCsd(initData, &index, &outBuffer) == OK) {
            outBuffer->meta()->setInt64("timeUs", timestamp.peek());
            outBuffer->meta()->setInt32("flags", BUFFER_FLAG_CODEC_CONFIG);
            ALOGV("[%s] onWorkDone: csd index = %zu [%p]", mName, index, outBuffer.get());

            // TRICKY: we want popped buffers reported in order, so sending
            // the callback while holding the lock here. This assumes that
            // onOutputBufferAvailable() does not block. onOutputBufferAvailable()
            // callbacks are always sent with the Output lock held.
            mCallback->onOutputBufferAvailable(index, outBuffer);
        } else {
            ALOGD("[%s] onWorkDone: unable to register csd", mName);
            output.unlock();
            mCCodecCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
            return false;
        }
    }

    bool drop = false;
    if (worklet->output.flags & C2FrameData::FLAG_DROP_FRAME) {
        ALOGV("[%s] onWorkDone: drop buffer but keep metadata", mName);
        drop = true;
    }

    // Workaround: if C2FrameData::FLAG_DROP_FRAME is not implemented in
    // HAL, the flag is then removed in the corresponding output buffer.
    if (work->input.flags & C2FrameData::FLAG_DROP_FRAME) {
        flags |= BUFFER_FLAG_DECODE_ONLY;
    }

    if (notifyClient && !buffer && !flags) {
        if (mTunneled && drop && outputFormat) {
            if (mOutputFormat != outputFormat) {
                ALOGV("[%s] onWorkDone: Keep tunneled, drop frame with format change (%lld)",
                      mName, work->input.ordinal.frameIndex.peekull());
                mOutputFormat = outputFormat;
            } else {
                ALOGV("[%s] onWorkDone: Not reporting output buffer without format change (%lld)",
                      mName, work->input.ordinal.frameIndex.peekull());
                notifyClient = false;
            }
        } else {
            ALOGV("[%s] onWorkDone: Not reporting output buffer (%lld)",
                  mName, work->input.ordinal.frameIndex.peekull());
            notifyClient = false;
        }
    }

    if (buffer) {
        for (const std::shared_ptr<const C2Info> &info : buffer->info()) {
            // TODO: properly translate these to metadata
            switch (info->coreIndex().coreIndex()) {
                case C2StreamPictureTypeMaskInfo::CORE_INDEX:
                    if (((C2StreamPictureTypeMaskInfo *)info.get())->value & C2Config::SYNC_FRAME) {
                        flags |= BUFFER_FLAG_KEY_FRAME;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    {
        Mutexed<Output>::Locked output(mOutput);
        if (!output->buffers) {
            return false;
        }
        output->buffers->pushToStash(
                buffer,
                notifyClient,
                timestamp.peek(),
                flags,
                outputFormat,
                worklet->output.ordinal);
    }
    sendOutputBuffers();
    return true;
}

void CCodecBufferChannel::sendOutputBuffers() {
    std::string traceStr;
    if (ATRACE_ENABLED()) {
        traceStr = android::base::StringPrintf(
                "CCodecBufferChannel::sendOutputBuffers-%s", mName);
    }
    ScopedTrace trace(ATRACE_TAG, traceStr.c_str());
    OutputBuffers::BufferAction action;
    size_t index;
    sp<MediaCodecBuffer> outBuffer;
    std::shared_ptr<C2Buffer> c2Buffer;

    constexpr int kMaxReallocTry = 5;
    int reallocTryNum = 0;

    while (true) {
        Mutexed<Output>::Locked output(mOutput);
        if (!output->buffers) {
            return;
        }
        action = output->buffers->popFromStashAndRegister(
                &c2Buffer, &index, &outBuffer);
        if (action != OutputBuffers::REALLOCATE) {
            reallocTryNum = 0;
        }
        switch (action) {
        case OutputBuffers::SKIP:
            return;
        case OutputBuffers::NOTIFY_CLIENT:
        {
            // TRICKY: we want popped buffers reported in order, so sending
            // the callback while holding the lock here. This assumes that
            // onOutputBufferAvailable() does not block. onOutputBufferAvailable()
            // callbacks are always sent with the Output lock held.
            if (c2Buffer) {
                std::shared_ptr<const C2AccessUnitInfos::output> bufferMetadata =
                        std::static_pointer_cast<const C2AccessUnitInfos::output>(
                        c2Buffer->getInfo(C2AccessUnitInfos::output::PARAM_TYPE));
                if (bufferMetadata && bufferMetadata->flexCount() > 0) {
                    uint32_t flag = 0;
                    std::vector<AccessUnitInfo> accessUnitInfos;
                    for (int nMeta = 0; nMeta < bufferMetadata->flexCount(); nMeta++) {
                        const C2AccessUnitInfosStruct &bufferMetadataStruct =
                                bufferMetadata->m.values[nMeta];
                        flag = convertFlags(bufferMetadataStruct.flags, false);
                        accessUnitInfos.emplace_back(flag,
                                bufferMetadataStruct.size,
                                bufferMetadataStruct.timestamp);
                    }
                    sp<WrapperObject<std::vector<AccessUnitInfo>>> obj{
                        new WrapperObject<std::vector<AccessUnitInfo>>{accessUnitInfos}};
                    outBuffer->meta()->setObject("accessUnitInfo", obj);
                }
            }
            mCallback->onOutputBufferAvailable(index, outBuffer);
            [[fallthrough]];
        }
        case OutputBuffers::DISCARD: {
            if (mHasInputSurface && android::media::codec::provider_->input_surface_throttle()) {
                Mutexed<InputSurface>::Locked inputSurface(mInputSurface);
                --inputSurface->numProcessingBuffersBalance;
                ALOGV("[%s] onWorkDone: numProcessingBuffersBalance = %lld",
                        mName, static_cast<long long>(inputSurface->numProcessingBuffersBalance));
            }
            break;
        }
        case OutputBuffers::REALLOCATE:
            if (++reallocTryNum > kMaxReallocTry) {
                output.unlock();
                ALOGE("[%s] sendOutputBuffers: tried %d realloc and failed",
                          mName, kMaxReallocTry);
                mCCodecCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
                return;
            }
            if (!output->buffers->isArrayMode()) {
                output->buffers =
                    output->buffers->toArrayMode(output->numSlots);
            }
            static_cast<OutputBuffersArray*>(output->buffers.get())->
                    realloc(c2Buffer);
            output.unlock();
            mCCodecCallback->onOutputBuffersChanged();
            break;
        case OutputBuffers::RETRY:
            ALOGV("[%s] sendOutputBuffers: unable to register output buffer",
                  mName);
            return;
        default:
            LOG_ALWAYS_FATAL("[%s] sendOutputBuffers: "
                    "corrupted BufferAction value (%d) "
                    "returned from popFromStashAndRegister.",
                    mName, int(action));
            return;
        }
    }
}

status_t CCodecBufferChannel::setSurface(const sp<Surface> &newSurface,
                                         uint32_t generation, bool pushBlankBuffer) {
    sp<IGraphicBufferProducer> producer;
    int maxDequeueCount;
    sp<Surface> oldSurface;
    {
        Mutexed<OutputSurface>::Locked outputSurface(mOutputSurface);
        maxDequeueCount = outputSurface->maxDequeueBuffers;
        oldSurface = outputSurface->surface;
    }
    if (newSurface) {
        (void)SurfaceCallbackHandler::GetInstance();
        newSurface->setScalingMode(NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
        newSurface->setDequeueTimeout(kDequeueTimeoutNs);
        newSurface->setMaxDequeuedBufferCount(maxDequeueCount);
        producer = newSurface->getIGraphicBufferProducer();
    } else {
        ALOGE("[%s] setting output surface to null", mName);
        return INVALID_OPERATION;
    }

    std::shared_ptr<Codec2Client::Configurable> outputPoolIntf;
    C2BlockPool::local_id_t outputPoolId;
    {
        Mutexed<BlockPools>::Locked pools(mBlockPools);
        outputPoolId = pools->outputPoolId;
        outputPoolIntf = pools->outputPoolIntf;
    }

    if (outputPoolIntf) {
        if (std::atomic_load(&mComponent)->setOutputSurface(
                outputPoolId,
                producer,
                generation,
                maxDequeueCount) != C2_OK) {
            ALOGI("[%s] setSurface: component setOutputSurface failed", mName);
            return INVALID_OPERATION;
        }
    }

    {
        Mutexed<OutputSurface>::Locked output(mOutputSurface);
        output->surface = newSurface;
        output->generation = generation;
        initializeFrameTrackingFor(static_cast<ANativeWindow *>(newSurface.get()));
    }

    if (oldSurface && pushBlankBuffer) {
        // When ReleaseSurface was set from MediaCodec,
        // pushing a blank buffer at the end might be necessary.
        sp<ANativeWindow> anw = static_cast<ANativeWindow *>(oldSurface.get());
        if (anw) {
            pushBlankBuffersToNativeWindow(anw.get());
        }
    }

    return OK;
}

PipelineWatcher::Clock::duration CCodecBufferChannel::elapsed() {
    // Otherwise, component may have stalled work due to input starvation up to
    // the sum of the delay in the pipeline.
    // TODO(b/231253301): When client pushed EOS, the pipeline could have less
    //                    number of frames.
    size_t n = 0;
    size_t outputDelay = mOutput.lock()->outputDelay;
    {
        Mutexed<Input>::Locked input(mInput);
        n = input->inputDelay + input->pipelineDelay + outputDelay;
    }
    return mPipelineWatcher.lock()->elapsed(PipelineWatcher::Clock::now(), n);
}

void CCodecBufferChannel::setMetaMode(MetaMode mode) {
    mMetaMode = mode;
}

void CCodecBufferChannel::setCrypto(const sp<ICrypto> &crypto) {
    if (mCrypto != nullptr) {
        for (std::pair<wp<HidlMemory>, int32_t> entry : mHeapSeqNumMap) {
            mCrypto->unsetHeap(entry.second);
        }
        mHeapSeqNumMap.clear();
        if (mHeapSeqNum >= 0) {
            mCrypto->unsetHeap(mHeapSeqNum);
            mHeapSeqNum = -1;
        }
    }
    mCrypto = crypto;
}

void CCodecBufferChannel::setDescrambler(const sp<IDescrambler> &descrambler) {
    mDescrambler = descrambler;
}

uint32_t CCodecBufferChannel::getBuffersPixelFormat(bool isEncoder) {
    if (isEncoder) {
        return getInputBuffersPixelFormat();
    } else {
        return getOutputBuffersPixelFormat();
    }
}

uint32_t CCodecBufferChannel::getInputBuffersPixelFormat() {
    Mutexed<Input>::Locked input(mInput);
    if (input->buffers == nullptr) {
        return PIXEL_FORMAT_UNKNOWN;
    }
    return input->buffers->getPixelFormatIfApplicable();
}

uint32_t CCodecBufferChannel::getOutputBuffersPixelFormat() {
    Mutexed<Output>::Locked output(mOutput);
    if (output->buffers == nullptr) {
        return PIXEL_FORMAT_UNKNOWN;
    }
    return output->buffers->getPixelFormatIfApplicable();
}

void CCodecBufferChannel::resetBuffersPixelFormat(bool isEncoder) {
    if (isEncoder) {
        Mutexed<Input>::Locked input(mInput);
        if (input->buffers == nullptr) {
            return;
        }
        input->buffers->resetPixelFormatIfApplicable();
    } else {
        Mutexed<Output>::Locked output(mOutput);
        if (output->buffers == nullptr) {
            return;
        }
        output->buffers->resetPixelFormatIfApplicable();
    }
}

void CCodecBufferChannel::setInfoBuffer(const std::shared_ptr<C2InfoBuffer> &buffer) {
    if (!mHasInputSurface) {
        mInfoBuffers.push_back(buffer);
    } else {
        std::list<std::unique_ptr<C2Work>> items;
        std::unique_ptr<C2Work> work(new C2Work);
        work->input.infoBuffers.emplace_back(*buffer);
        work->worklets.emplace_back(new C2Worklet);
        items.push_back(std::move(work));
    }
}

status_t toStatusT(c2_status_t c2s, c2_operation_t c2op) {
    // C2_OK is always translated to OK.
    if (c2s == C2_OK) {
        return OK;
    }

    // Operation-dependent translation
    // TODO: Add as necessary
    switch (c2op) {
    case C2_OPERATION_Component_start:
        switch (c2s) {
        case C2_NO_MEMORY:
            return NO_MEMORY;
        default:
            return UNKNOWN_ERROR;
        }
    default:
        break;
    }

    // Backup operation-agnostic translation
    switch (c2s) {
    case C2_BAD_INDEX:
        return BAD_INDEX;
    case C2_BAD_VALUE:
        return BAD_VALUE;
    case C2_BLOCKING:
        return WOULD_BLOCK;
    case C2_DUPLICATE:
        return ALREADY_EXISTS;
    case C2_NO_INIT:
        return NO_INIT;
    case C2_NO_MEMORY:
        return NO_MEMORY;
    case C2_NOT_FOUND:
        return NAME_NOT_FOUND;
    case C2_TIMED_OUT:
        return TIMED_OUT;
    case C2_BAD_STATE:
    case C2_CANCELED:
    case C2_CANNOT_DO:
    case C2_CORRUPTED:
    case C2_OMITTED:
    case C2_REFUSED:
        return UNKNOWN_ERROR;
    default:
        return -static_cast<status_t>(c2s);
    }
}

}  // namespace android
