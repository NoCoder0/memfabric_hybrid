/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include "hybm_data_op_aiv_direct_sdma.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include "aiv_sdma_transport_manager.h"
#include "../../copy_aiv_direct_sdma/aiv_direct_sdma_abi.h"
#include "dl_api.h"
#include "dl_hal_api.h"
#include "dl_hybm_aiv_direct_sdma.h"
#include "hybm_data_op.h"
#include "hybm_gva.h"
#include "hybm_stream_manager.h"
#include "aiv_direct_sdma_plan.h"

namespace ock {
namespace mf {
namespace {
constexpr uint64_t RING_BUDGET_BYTES = 64ULL * 1024 * 1024;
constexpr uint64_t SLOT_BYTES = aiv_direct_sdma::MAX_DESCRIPTORS * sizeof(AivDirectSdmaDescriptor) +
                                aiv_direct_sdma::MAX_AIV_COUNT * AIV_DIRECT_SDMA_CONSUMED_STRIDE;
constexpr uint32_t SLOT_COUNT = RING_BUDGET_BYTES / SLOT_BYTES;
constexpr uint64_t RING_BYTES = SLOT_COUNT * SLOT_BYTES;
static_assert(SLOT_COUNT > 8, "AIV Direct SDMA descriptor ring is unexpectedly small");
} // namespace
class AivDirectSdmaState {
public:
    explicit AivDirectSdmaState(void *stream) : stream_(stream) {}
    ~AivDirectSdmaState()
    {
        // The API owner must keep an external stream alive until bm entity teardown.
        if (submitted_)
            (void)DlAclApi::AclrtSynchronizeStream(stream_);
        manager_.CloseDevice();
        if (mapped_)
            DlHalApi::HalHostUnregisterEx(host_, HybmGetInitedLogicDeviceId(), HOST_MEM_MAP_DEV);
        if (host_)
            DlAclApi::AclrtFreeHost(host_);
    }
    Result Init()
    {
        if (DlAclApi::GetAscendSocType() != AscendSocType::ASCEND_910C) {
            BM_LOG_ERROR("AIV Direct SDMA requires Ascend910_93 (A3)");
            return BM_NOT_SUPPORTED;
        }
        auto ret = ConfigureAivWorkers();
        if (ret)
            return ret;
        ret = DlHybmAivDirectSdmaApi::TryLoadLibrary();
        if (ret != BM_OK) {
            BM_LOG_ERROR("AIV Direct SDMA extension is unavailable; check MEMFABRIC_HYBRID_EXTEND_LIB_PATH");
            return BM_UNDER_API_UNLOAD;
        }
        ret = DlApi::LoadExtendLibrary(DlApiExtendLibraryType::DL_EXT_LIB_DEVICE_SDMA);
        if (ret)
            return ret;
        transport::TransportOptions options{};
        options.rankCount = 1;
        ret = manager_.OpenDevice(options);
        if (ret)
            return ret;
        workspace_ = reinterpret_cast<void *>(manager_.GetSdmaWorkSpaceAddr());
        ret = ValidateQueues();
        if (ret)
            return ret;
        ret = ValidateNotifyIds();
        if (ret)
            return ret;
        return AllocateRing();
    }
    Result ConfigureAivWorkers()
    {
        const char *value = std::getenv("MEMFABRIC_HYBRID_AIV_DIRECT_SDMA_AIV_COUNT");
        if (value) {
            char *end = nullptr;
            errno = 0;
            auto n = std::strtoul(value, &end, 10);
            if (errno || end == value || *end || n < 1 || n > aiv_direct_sdma::MAX_AIV_COUNT) {
                BM_LOG_ERROR("MEMFABRIC_HYBRID_AIV_DIRECT_SDMA_AIV_COUNT must be in [1,"
                             << aiv_direct_sdma::MAX_AIV_COUNT << "]");
                return BM_INVALID_PARAM;
            }
            aivCount_ = static_cast<uint32_t>(n);
        }
        return BM_OK;
    }
    Result ValidateQueues()
    {
        StarsQueueInfo info[aiv_direct_sdma::MAX_AIV_COUNT]{};
        auto ret = DlAclApi::AclrtMemcpy(info, sizeof(info),
                                         reinterpret_cast<uint8_t *>(workspace_) + AIV_DIRECT_SDMA_QUEUE_INFO_OFFSET,
                                         sizeof(info), ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret)
            return ret;
        uint32_t depth = UINT32_MAX;
        for (uint32_t i = 0; i < aivCount_; ++i) {
            if (!info[i].sq_base || !info[i].sq_reg_base || info[i].sq_depth < 4 ||
                info[i].sq_head != info[i].sq_tail || info[i].sq_tail >= info[i].sq_depth ||
                info[i].sq_depth > UINT16_MAX || info[i].stream_id > UINT16_MAX) {
                BM_LOG_ERROR("invalid or nonempty A3 queue metadata for AIV worker " << i);
                return BM_ERROR;
            }
            depth = std::min(depth, info[i].sq_depth);
        }
        capacity_ = aiv_direct_sdma::WaveCapacity(depth, aivCount_);
        if (capacity_ == 0) {
            BM_LOG_ERROR("AIV Direct SDMA wave capacity is zero: queue depth=" << depth << " AIV count=" << aivCount_);
            return BM_NOT_SUPPORTED;
        }
        BM_LOG_INFO("AIV Direct SDMA AIV count=" << aivCount_ << " queue depth=" << depth << " capacity=" << capacity_);
        return BM_OK;
    }
    Result ValidateNotifyIds()
    {
        uint32_t notifyIds[aiv_direct_sdma::MAX_AIV_COUNT]{};
        auto ret = DlAclApi::AclrtMemcpy(notifyIds, sizeof(notifyIds),
                                         static_cast<uint8_t *>(workspace_) + AIV_DIRECT_SDMA_NOTIFY_ID_OFFSET,
                                         sizeof(notifyIds), ACL_MEMCPY_DEVICE_TO_HOST);
        if (ret) {
            BM_LOG_ERROR("read AIV Direct SDMA notify IDs failed, ret=" << ret);
            return ret;
        }
        for (uint32_t i = 0; i < aivCount_; ++i) {
            if (notifyIds[i] >= (1U << 13)) {
                BM_LOG_ERROR("A3 NotifyRecord ID exceeds 13-bit ABI: AIV worker=" << i << " id=" << notifyIds[i]);
                return BM_NOT_SUPPORTED;
            }
        }
        return BM_OK;
    }
    Result AllocateRing()
    {
        auto ret = DlAclApi::AclrtMallocHost(&host_, RING_BYTES);
        if (ret)
            return ret;
        ret = DlHalApi::HalHostRegister(host_, RING_BYTES, HOST_MEM_MAP_DEV, HybmGetInitedLogicDeviceId(), &device_);
        if (ret)
            return ret;
        mapped_ = true;
        std::memset(host_, 0, RING_BYTES);
        for (uint32_t slot = 0; slot < SLOT_COUNT; ++slot)
            for (uint32_t worker = 0; worker < aivCount_; ++worker)
                *Mask(slot, worker) = 1;
        return BM_OK;
    }
    Result Copy(hybm_batch_copy_params &params, uint32_t flags)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (unusable_) {
            BM_LOG_ERROR("AIV Direct SDMA stream state is unavailable after an indeterminate asynchronous failure");
            return BM_ERROR;
        }
        aiv_direct_sdma::Cursor cursor;
        while (cursor.input < params.batchSize) {
            const uint32_t slot = next_;
            next_ = (next_ + 1) % SLOT_COUNT;
            auto ret = AcquireSlot(slot);
            if (ret)
                return ret;
            auto out = reinterpret_cast<AivDirectSdmaDescriptor *>(static_cast<uint8_t *>(host_) + slot * SLOT_BYTES);
            const uint32_t count = aiv_direct_sdma::FillWave(params.sources, params.destinations, params.dataSizes,
                                                             params.batchSize, capacity_, cursor, out);
            if (count == 0) {
                BM_LOG_ERROR("AIV Direct SDMA wave made no progress: batch size="
                             << params.batchSize << " input index=" << cursor.input << " capacity=" << capacity_);
                return BM_ERROR;
            }
            ret = Submit(slot, count);
            if (ret)
                return ret;
        }
        if (flags & ASYNC_COPY_FLAG) {
            return BM_OK;
        }
        return WaitUnlocked();
    }
    Result AcquireSlot(uint32_t slot)
    {
        bool available = true;
        for (uint32_t worker = 0; worker < aivCount_; ++worker)
            available &= (__atomic_load_n(Mask(slot, worker), __ATOMIC_ACQUIRE) == 1);
        if (!available) {
            // Match Copy Extend: synchronize only when the reusable parameter
            // space is exhausted. A successful synchronization proves all
            // descriptor readers and SDMA payloads on this stream are complete.
            auto ret = WaitUnlocked();
            if (ret)
                return ret;
            for (uint32_t worker = 0; worker < aivCount_; ++worker)
                if (__atomic_load_n(Mask(slot, worker), __ATOMIC_ACQUIRE) != 1) {
                    unusable_ = true;
                    BM_LOG_ERROR("descriptor slot unconsumed after sync: slot=" << slot << " AIV worker=" << worker);
                    return BM_ERROR;
                }
        }
        return BM_OK;
    }
    Result Submit(uint32_t slot, uint32_t count)
    {
        for (uint32_t worker = 0; worker < aivCount_; ++worker)
            __atomic_store_n(Mask(slot, worker), 0, __ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        void *desc = static_cast<uint8_t *>(device_) + slot * SLOT_BYTES;
        void *mask = static_cast<uint8_t *>(desc) + aiv_direct_sdma::MAX_DESCRIPTORS * sizeof(AivDirectSdmaDescriptor);
        submitted_ = true;
        auto ret = DlHybmAivDirectSdmaApi::HybmAivDirectSdmaLaunch(workspace_, desc, count, mask, aivCount_, stream_);
        if (ret) {
            // The runtime may have accepted part of an asynchronous launch.
            // Do not reuse its descriptors until the owning entity is closed.
            unusable_ = true;
            BM_LOG_ERROR("AIV Direct SDMA kernel launch failed, ret=" << ret);
            return ret;
        }
        // Descriptor consumption permits slot reuse, but does not complete the
        // payload. Notify waits gate user completion and the next producer wave.
        for (uint32_t worker = 0; worker < aivCount_; ++worker) {
            ret = manager_.WaitNotify(worker, stream_);
            if (ret) {
                // Some waits may already be queued. Without proof that every
                // SDMA queue drained, automatic recovery could overwrite data.
                unusable_ = true;
                BM_LOG_ERROR("AIV Direct SDMA notify wait failed: AIV worker=" << worker << " ret=" << ret);
                return ret;
            }
        }
        return BM_OK;
    }
    Result Wait()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return WaitUnlocked();
    }
    Result WaitUnlocked()
    {
        if (!submitted_)
            return BM_OK;
        auto ret = DlAclApi::AclrtSynchronizeStream(stream_);
        if (ret) {
            unusable_ = true;
            BM_LOG_ERROR("AIV Direct SDMA stream synchronization failed, ret=" << ret);
        } else {
            submitted_ = false;
        }
        return ret;
    }

private:
    uint32_t *Mask(uint32_t slot, uint32_t worker)
    {
        return reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(host_) + slot * SLOT_BYTES +
                                            aiv_direct_sdma::MAX_DESCRIPTORS * sizeof(AivDirectSdmaDescriptor) +
                                            worker * AIV_DIRECT_SDMA_CONSUMED_STRIDE);
    }
    std::mutex mutex_;
    transport::device::SdmaTransportManager manager_;
    void *stream_ = nullptr, *workspace_ = nullptr, *host_ = nullptr, *device_ = nullptr;
    uint32_t aivCount_ = 4, capacity_ = 0, next_ = 0;
    bool mapped_ = false, submitted_ = false, unusable_ = false;
};
HostAivDirectSdma::HostAivDirectSdma() = default;
HostAivDirectSdma::~HostAivDirectSdma() = default;
Result HostAivDirectSdma::Copy(hybm_batch_copy_params &params, void *stream, uint32_t flags) noexcept
{
    if ((flags & COPY_EXTEND_FLAG) || !params.batchSize || !params.sources || !params.destinations ||
        !params.dataSizes) {
        BM_LOG_ERROR("invalid AIV Direct SDMA batch: flags=" << flags << " batchSize=" << params.batchSize);
        return BM_INVALID_PARAM;
    }
    for (uint32_t i = 0; i < params.batchSize; ++i) {
        if (!aiv_direct_sdma::ValidRange(reinterpret_cast<uint64_t>(params.sources[i]), params.dataSizes[i]) ||
            !aiv_direct_sdma::ValidRange(reinterpret_cast<uint64_t>(params.destinations[i]), params.dataSizes[i])) {
            BM_LOG_ERROR("invalid AIV Direct SDMA range: index=" << i << " length=" << params.dataSizes[i]);
            return BM_INVALID_PARAM;
        }
    }
    if (!stream)
        stream = HybmStreamManager::GetThreadAclStream();
    if (!stream) {
        BM_LOG_ERROR("AIV Direct SDMA caller stream is null");
        return BM_ERROR;
    }
    std::shared_ptr<AivDirectSdmaState> state;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = streams_.find(stream);
            if (found == streams_.end()) {
                auto fresh = std::make_shared<AivDirectSdmaState>(stream);
                auto ret = fresh->Init();
                if (ret) {
                    BM_LOG_ERROR("AIV Direct SDMA initialization failed, ret=" << ret);
                    return ret;
                }
                found = streams_.emplace(stream, std::move(fresh)).first;
            }
            state = found->second;
        }
        return state->Copy(params, flags);
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("AIV Direct SDMA state allocation failed");
        return BM_MALLOC_FAILED;
    }
}
Result HostAivDirectSdma::Wait() noexcept
{
    std::vector<std::shared_ptr<AivDirectSdmaState>> states;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        states.reserve(streams_.size());
        for (auto &entry : streams_)
            states.emplace_back(entry.second);
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("AIV Direct SDMA wait state snapshot allocation failed");
        return BM_MALLOC_FAILED;
    }
    Result result = BM_OK;
    for (auto &state : states) {
        auto ret = state->Wait();
        if (ret)
            result = ret;
    }
    return result;
}
void HostAivDirectSdma::Close() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    streams_.clear();
}
} // namespace mf
} // namespace ock
