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

//#define LOG_NDEBUG 0
#define LOG_TAG "C2BqBuffer"
#include <utils/Log.h>

#include <ui/BufferQueueDefs.h>
#include <ui/GraphicBuffer.h>
#include <ui/Fence.h>

#include <types.h>

#include <hidl/HidlSupport.h>

#include <C2AllocatorGralloc.h>
#include <C2BqBufferPriv.h>
#include <C2BlockInternal.h>

#include <list>
#include <map>
#include <mutex>

using ::android::BufferQueueDefs::NUM_BUFFER_SLOTS;
using ::android::C2AllocatorGralloc;
using ::android::C2AndroidMemoryUsage;
using ::android::Fence;
using ::android::GraphicBuffer;
using ::android::sp;
using ::android::status_t;
using ::android::wp;
using ::android::hardware::hidl_handle;
using ::android::hardware::Return;

using HBuffer = ::android::hardware::graphics::common::V1_2::HardwareBuffer;
using HStatus = ::android::hardware::graphics::bufferqueue::V2_0::Status;
using ::android::hardware::graphics::bufferqueue::V2_0::utils::b2h;
using ::android::hardware::graphics::bufferqueue::V2_0::utils::h2b;
using ::android::hardware::graphics::bufferqueue::V2_0::utils::HFenceWrapper;

using HGraphicBufferProducer = ::android::hardware::graphics::bufferqueue::V2_0
        ::IGraphicBufferProducer;

struct C2BufferQueueBlockPoolData : public _C2BlockPoolData {

    bool held;
    bool local;
    uint32_t generation;
    uint64_t bqId;
    int32_t bqSlot;
    sp<HGraphicBufferProducer> igbp;
    std::shared_ptr<C2BufferQueueBlockPool::Impl> localPool;

    virtual type_t getType() const override {
        return TYPE_BUFFERQUEUE;
    }

    // Create a remote BlockPoolData.
    C2BufferQueueBlockPoolData(
            uint32_t generation, uint64_t bqId, int32_t bqSlot,
            const sp<HGraphicBufferProducer>& producer = nullptr);

    // Create a local BlockPoolData.
    C2BufferQueueBlockPoolData(
            uint32_t generation, uint64_t bqId, int32_t bqSlot,
            const std::shared_ptr<C2BufferQueueBlockPool::Impl>& pool);

    virtual ~C2BufferQueueBlockPoolData() override;

};

bool _C2BlockFactory::GetBufferQueueData(
        const std::shared_ptr<_C2BlockPoolData>& data,
        uint32_t* generation, uint64_t* bqId, int32_t* bqSlot) {
    if (data && data->getType() == _C2BlockPoolData::TYPE_BUFFERQUEUE) {
        if (generation) {
            const std::shared_ptr<C2BufferQueueBlockPoolData> poolData =
                    std::static_pointer_cast<C2BufferQueueBlockPoolData>(data);
            *generation = poolData->generation;
            if (bqId) {
                *bqId = poolData->bqId;
            }
            if (bqSlot) {
                *bqSlot = poolData->bqSlot;
            }
        }
        return true;
    }
    return false;
}

bool _C2BlockFactory::AssignBlockToBufferQueue(
        const std::shared_ptr<_C2BlockPoolData>& data,
        const sp<HGraphicBufferProducer>& igbp,
        uint32_t generation,
        uint64_t bqId,
        int32_t bqSlot,
        bool held) {
    if (data && data->getType() == _C2BlockPoolData::TYPE_BUFFERQUEUE) {
        const std::shared_ptr<C2BufferQueueBlockPoolData> poolData =
                std::static_pointer_cast<C2BufferQueueBlockPoolData>(data);
        poolData->igbp = igbp;
        poolData->generation = generation;
        poolData->bqId = bqId;
        poolData->bqSlot = bqSlot;
        poolData->held = held;
        return true;
    }
    return false;
}

bool _C2BlockFactory::HoldBlockFromBufferQueue(
        const std::shared_ptr<_C2BlockPoolData>& data,
        const sp<HGraphicBufferProducer>& igbp) {
    const std::shared_ptr<C2BufferQueueBlockPoolData> poolData =
            std::static_pointer_cast<C2BufferQueueBlockPoolData>(data);
    if (!poolData->local) {
        poolData->igbp = igbp;
    }
    if (poolData->held) {
        poolData->held = true;
        return false;
    }
    poolData->held = true;
    return true;
}

bool _C2BlockFactory::YieldBlockToBufferQueue(
        const std::shared_ptr<_C2BlockPoolData>& data) {
    const std::shared_ptr<C2BufferQueueBlockPoolData> poolData =
            std::static_pointer_cast<C2BufferQueueBlockPoolData>(data);
    if (!poolData->held) {
        poolData->held = false;
        return false;
    }
    poolData->held = false;
    return true;
}

std::shared_ptr<C2GraphicBlock> _C2BlockFactory::CreateGraphicBlock(
        const C2Handle *handle) {
    // TODO: get proper allocator? and mutex?
    static std::unique_ptr<C2AllocatorGralloc> sAllocator = std::make_unique<C2AllocatorGralloc>(0);

    std::shared_ptr<C2GraphicAllocation> alloc;
    if (C2AllocatorGralloc::isValid(handle)) {
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint64_t usage;
        uint32_t stride;
        uint32_t generation;
        uint64_t bqId;
        uint32_t bqSlot;
        android::_UnwrapNativeCodec2GrallocMetadata(
                handle, &width, &height, &format, &usage, &stride, &generation, &bqId, &bqSlot);
        c2_status_t err = sAllocator->priorGraphicAllocation(handle, &alloc);
        if (err == C2_OK) {
            std::shared_ptr<C2GraphicBlock> block;
            if (bqId || bqSlot) {
                // BQBBP
                std::shared_ptr<C2BufferQueueBlockPoolData> poolData =
                        std::make_shared<C2BufferQueueBlockPoolData>(generation,
                                                                     bqId,
                                                                     (int32_t)bqSlot);
                block = _C2BlockFactory::CreateGraphicBlock(alloc, poolData);
            } else {
                block = _C2BlockFactory::CreateGraphicBlock(alloc);
            }
            return block;
        }
    }
    return nullptr;
}

class C2BufferQueueBlockPool::Impl
        : public std::enable_shared_from_this<C2BufferQueueBlockPool::Impl> {
private:
    c2_status_t fetchFromIgbp_l(
            uint32_t width,
            uint32_t height,
            uint32_t format,
            C2MemoryUsage usage,
            std::shared_ptr<C2GraphicBlock> *block /* nonnull */) {
        // We have an IGBP now.
        C2AndroidMemoryUsage androidUsage = usage;
        status_t status{};
        int slot{};
        bool bufferNeedsReallocation{};
        sp<Fence> fence = new Fence();
        ALOGV("tries to dequeue buffer");

        { // Call dequeueBuffer().
            using Input = HGraphicBufferProducer::DequeueBufferInput;
            using Output = HGraphicBufferProducer::DequeueBufferOutput;
            Return<void> transResult = mProducer->dequeueBuffer(
                    Input{
                        width,
                        height,
                        format,
                        androidUsage.asGrallocUsage()},
                    [&status, &slot, &bufferNeedsReallocation,
                     &fence](HStatus hStatus,
                             int32_t hSlot,
                             Output const& hOutput) {
                        slot = static_cast<int>(hSlot);
                        if (!h2b(hStatus, &status) ||
                                !h2b(hOutput.fence, &fence)) {
                            status = ::android::BAD_VALUE;
                        } else {
                            bufferNeedsReallocation =
                                    hOutput.bufferNeedsReallocation;
                        }
                    });
            if (!transResult.isOk() || status != android::OK) {
                ALOGD("cannot dequeue buffer %d", status);
                if (transResult.isOk()) {
                    if (status == android::INVALID_OPERATION ||
                        status == android::TIMED_OUT ||
                        status == android::WOULD_BLOCK) {
                        // Dequeue buffer is blocked temporarily. Retrying is
                        // required.
                        return C2_BLOCKING;
                    }
                }
                return C2_BAD_VALUE;
            }
        }
        HFenceWrapper hFenceWrapper{};
        if (!b2h(fence, &hFenceWrapper)) {
            ALOGE("Invalid fence received from dequeueBuffer.");
            return C2_BAD_VALUE;
        }
        ALOGV("dequeued a buffer successfully");
        if (fence) {
            static constexpr int kFenceWaitTimeMs = 10;

            status_t status = fence->wait(kFenceWaitTimeMs);
            if (status == -ETIME) {
                // fence is not signalled yet.
                (void)mProducer->cancelBuffer(slot, hFenceWrapper.getHandle()).isOk();
                return C2_BLOCKING;
            }
            if (status != android::NO_ERROR) {
                ALOGD("buffer fence wait error %d", status);
                (void)mProducer->cancelBuffer(slot, hFenceWrapper.getHandle()).isOk();
                return C2_BAD_VALUE;
            } else if (mRenderCallback) {
                nsecs_t signalTime = fence->getSignalTime();
                if (signalTime >= 0 && signalTime < INT64_MAX) {
                    mRenderCallback(mProducerId, slot, signalTime);
                } else {
                    ALOGV("got fence signal time of %lld", (long long)signalTime);
                }
            }
        }

        sp<GraphicBuffer> &slotBuffer = mBuffers[slot];
        if (bufferNeedsReallocation || !slotBuffer) {
            if (!slotBuffer) {
                slotBuffer = new GraphicBuffer();
            }
            // N.B. This assumes requestBuffer# returns an existing allocation
            // instead of a new allocation.
            Return<void> transResult = mProducer->requestBuffer(
                    slot,
                    [&status, &slotBuffer](
                            HStatus hStatus,
                            HBuffer const& hBuffer,
                            uint32_t generationNumber){
                        if (h2b(hStatus, &status) &&
                                h2b(hBuffer, &slotBuffer) &&
                                slotBuffer) {
                            slotBuffer->setGenerationNumber(generationNumber);
                        } else {
                            status = android::BAD_VALUE;
                        }
                    });
            if (!transResult.isOk()) {
                return C2_BAD_VALUE;
            } else if (status != android::NO_ERROR) {
                slotBuffer.clear();
                (void)mProducer->cancelBuffer(slot, hFenceWrapper.getHandle()).isOk();
                return C2_BAD_VALUE;
            }
        }
        if (slotBuffer) {
            ALOGV("buffer wraps %llu %d", (unsigned long long)mProducerId, slot);
            C2Handle *c2Handle = android::WrapNativeCodec2GrallocHandle(
                    slotBuffer->handle,
                    slotBuffer->width,
                    slotBuffer->height,
                    slotBuffer->format,
                    slotBuffer->usage,
                    slotBuffer->stride,
                    slotBuffer->getGenerationNumber(),
                    mProducerId, slot);
            if (c2Handle) {
                std::shared_ptr<C2GraphicAllocation> alloc;
                c2_status_t err = mAllocator->priorGraphicAllocation(c2Handle, &alloc);
                if (err != C2_OK) {
                    return err;
                }
                std::shared_ptr<C2BufferQueueBlockPoolData> poolData =
                        std::make_shared<C2BufferQueueBlockPoolData>(
                                slotBuffer->getGenerationNumber(),
                                mProducerId, slot,
                                shared_from_this());
                *block = _C2BlockFactory::CreateGraphicBlock(alloc, poolData);
                return C2_OK;
            }
            // Block was not created. call requestBuffer# again next time.
            slotBuffer.clear();
            (void)mProducer->cancelBuffer(slot, hFenceWrapper.getHandle()).isOk();
        }
        return C2_BAD_VALUE;
    }

public:
    Impl(const std::shared_ptr<C2Allocator> &allocator)
        : mInit(C2_OK), mProducerId(0), mAllocator(allocator) {
    }

    ~Impl() {
        bool noInit = false;
        for (int i = 0; i < NUM_BUFFER_SLOTS; ++i) {
            if (!noInit && mProducer) {
                Return<HStatus> transResult =
                        mProducer->detachBuffer(static_cast<int32_t>(i));
                noInit = !transResult.isOk() ||
                         static_cast<HStatus>(transResult) == HStatus::NO_INIT;
            }
            mBuffers[i].clear();
        }
    }

    c2_status_t fetchGraphicBlock(
            uint32_t width,
            uint32_t height,
            uint32_t format,
            C2MemoryUsage usage,
            std::shared_ptr<C2GraphicBlock> *block /* nonnull */) {
        block->reset();
        if (mInit != C2_OK) {
            return mInit;
        }

        static int kMaxIgbpRetry = 20; // TODO: small number can cause crash in releasing.
        static int kMaxIgbpRetryDelayUs = 10000;

        int curTry = 0;

        while (curTry++ < kMaxIgbpRetry) {
            std::unique_lock<std::mutex> lock(mMutex);
            // TODO: return C2_NO_INIT
            if (mProducerId == 0) {
                std::shared_ptr<C2GraphicAllocation> alloc;
                c2_status_t err = mAllocator->newGraphicAllocation(
                        width, height, format, usage, &alloc);
                if (err != C2_OK) {
                    return err;
                }
                std::shared_ptr<C2BufferQueueBlockPoolData> poolData =
                        std::make_shared<C2BufferQueueBlockPoolData>(
                                0, (uint64_t)0, ~0, shared_from_this());
                // TODO: config?
                *block = _C2BlockFactory::CreateGraphicBlock(alloc, poolData);
                ALOGV("allocated a buffer successfully");

                return C2_OK;
            }
            c2_status_t status = fetchFromIgbp_l(width, height, format, usage, block);
            if (status == C2_BLOCKING) {
                lock.unlock();
                ::usleep(kMaxIgbpRetryDelayUs);
                continue;
            }
            return status;
        }
        return C2_BLOCKING;
    }

    void setRenderCallback(const OnRenderCallback &renderCallback) {
        std::lock_guard<std::mutex> lock(mMutex);
        mRenderCallback = renderCallback;
    }

    void configureProducer(const sp<HGraphicBufferProducer> &producer) {
        uint64_t producerId = 0;
        if (producer) {
            Return<uint64_t> transResult = producer->getUniqueId();
            if (!transResult.isOk()) {
                ALOGD("configureProducer -- failed to connect to the producer");
                return;
            }
            producerId = static_cast<uint64_t>(transResult);
        }
        {
            std::lock_guard<std::mutex> lock(mMutex);
            bool noInit = false;
            for (int i = 0; i < NUM_BUFFER_SLOTS; ++i) {
                if (!noInit && mProducer) {
                    Return<HStatus> transResult =
                            mProducer->detachBuffer(static_cast<int32_t>(i));
                    noInit = !transResult.isOk() ||
                             static_cast<HStatus>(transResult) == HStatus::NO_INIT;
                }
                mBuffers[i].clear();
            }
            if (producer) {
                mProducer = producer;
                mProducerId = producerId;
            } else {
                mProducer = nullptr;
                mProducerId = 0;
            }
        }
    }

private:
    friend struct C2BufferQueueBlockPoolData;

    void cancel(uint64_t igbp_id, int32_t igbp_slot) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (igbp_id == mProducerId && mProducer) {
            (void)mProducer->cancelBuffer(igbp_slot, hidl_handle{}).isOk();
        }
    }

    c2_status_t mInit;
    uint64_t mProducerId;
    OnRenderCallback mRenderCallback;

    const std::shared_ptr<C2Allocator> mAllocator;

    std::mutex mMutex;
    sp<HGraphicBufferProducer> mProducer;

    sp<GraphicBuffer> mBuffers[NUM_BUFFER_SLOTS];
};

C2BufferQueueBlockPoolData::C2BufferQueueBlockPoolData(
        uint32_t generation, uint64_t bqId, int32_t bqSlot,
        const sp<HGraphicBufferProducer>& producer) :
        held(producer && bqId != 0), local(false),
        generation(generation), bqId(bqId), bqSlot(bqSlot),
        igbp(producer),
        localPool() {
}

C2BufferQueueBlockPoolData::C2BufferQueueBlockPoolData(
        uint32_t generation, uint64_t bqId, int32_t bqSlot,
        const std::shared_ptr<C2BufferQueueBlockPool::Impl>& pool) :
        held(true), local(true),
        generation(generation), bqId(bqId), bqSlot(bqSlot),
        igbp(pool ? pool->mProducer : nullptr),
        localPool(pool) {
}

C2BufferQueueBlockPoolData::~C2BufferQueueBlockPoolData() {
    if (!held || bqId == 0) {
        return;
    }
    if (local && localPool) {
        localPool->cancel(bqId, bqSlot);
    } else if (igbp) {
        igbp->cancelBuffer(bqSlot, hidl_handle{}).isOk();
    }
}

C2BufferQueueBlockPool::C2BufferQueueBlockPool(
        const std::shared_ptr<C2Allocator> &allocator, const local_id_t localId)
        : mAllocator(allocator), mLocalId(localId), mImpl(new Impl(allocator)) {}

C2BufferQueueBlockPool::~C2BufferQueueBlockPool() {}

c2_status_t C2BufferQueueBlockPool::fetchGraphicBlock(
        uint32_t width,
        uint32_t height,
        uint32_t format,
        C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock> *block /* nonnull */) {
    if (mImpl) {
        return mImpl->fetchGraphicBlock(width, height, format, usage, block);
    }
    return C2_CORRUPTED;
}

void C2BufferQueueBlockPool::configureProducer(const sp<HGraphicBufferProducer> &producer) {
    if (mImpl) {
        mImpl->configureProducer(producer);
    }
}

void C2BufferQueueBlockPool::setRenderCallback(const OnRenderCallback &renderCallback) {
    if (mImpl) {
        mImpl->setRenderCallback(renderCallback);
    }
}
