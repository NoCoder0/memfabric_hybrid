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
#ifndef MEMFABRIC_ACC_OFFLOAD_SHARED_DRAM_AICPU_ENTRY_H
#define MEMFABRIC_ACC_OFFLOAD_SHARED_DRAM_AICPU_ENTRY_H

#include "acc_offload_shared_dram_entry.h"

namespace ock::offload {

class AccOffloadSharedDramAicpuEntry final : public AccOffloadSharedDramEntry {
public:
    explicit AccOffloadSharedDramAicpuEntry(uint16_t entityId) : AccOffloadSharedDramEntry(entityId) {}
    ~AccOffloadSharedDramAicpuEntry() override
    {
        UnInitialize();
    }

    int32_t GetDva(uint64_t hostPtr, uint64_t *dvaPtr) override;
    int32_t RegisterMemory(const void *address, uint64_t size) override;
    int32_t SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *sizePtr,
                       uint8_t devIdx) override;
    int32_t EntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint8_t devIdx) override;

protected:
    int32_t InitializePool(const offload_config_t &config) override;
    void ReleaseBackendResources() override;

private:
    int32_t ValidateBackendConfig(const offload_config_t &config) const;
    int32_t PrepareSharedCopy();
    int32_t RegisterPeerPool();
    int32_t MapPeerPool(int fd, uint32_t rank, uint64_t size);
    int backingFd_{-1};
    uint32_t localWorldSize_{0};
    uint64_t peerPoolSize_{0};
};

} // namespace ock::offload
#endif
