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
#ifndef MEMFABRIC_HYBRID_ACC_AICPU_OFFLOAD_ENTRY_H
#define MEMFABRIC_HYBRID_ACC_AICPU_OFFLOAD_ENTRY_H

#include <memory>
#include "hybm_def.h"
#include "acc_offload_entry.h"
#include "acc_offload_mem_manager.h"

namespace ock::offload {

class AccOffloadAicpuEntry : public AccOffloadEntry {
public:
    explicit AccOffloadAicpuEntry(uint16_t entityId) : entityId_(entityId) {}
    ~AccOffloadAicpuEntry() override
    {
        UnInitialize();
    }

    int32_t Initialize(const offload_config_t &config) override;
    void UnInitialize() override;
    void *MallocHost(size_t size) override;
    void FreeHost(void *ptr) override;
    int32_t HostFree(uint64_t hostPtr, uint64_t flags) override;
    int32_t HostRegister(uint64_t hostPtr) override;
    int32_t GetDva(uint64_t hostPtr, uint64_t *dvaPtr) override;
    int32_t SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *sizePtr,
                       uint8_t devIdx) override;

private:
    int32_t CreatePool(uint64_t size);

    const uint16_t entityId_;
    uint8_t deviceId_{0};
    bool hybmInitialized_{false};
    bool launchLoaded_{false};
    hybm_entity_t entity_{nullptr};
    hybm_mem_slice_t slice_{nullptr};
    uint64_t poolBase_{0};
    uint64_t poolSize_{0};
    std::unique_ptr<AccOffloadMemManager> allocator_;
};
} // namespace ock::offload

#endif
