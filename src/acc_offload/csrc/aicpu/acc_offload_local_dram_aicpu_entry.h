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
#ifndef MEMFABRIC_HYBRID_ACC_OFFLOAD_LOCAL_DRAM_AICPU_ENTRY_H
#define MEMFABRIC_HYBRID_ACC_OFFLOAD_LOCAL_DRAM_AICPU_ENTRY_H

#include "acc_offload_local_dram_entry.h"

namespace ock::offload {

class AccOffloadLocalDramAicpuEntry final : public AccOffloadLocalDramEntry {
public:
    explicit AccOffloadLocalDramAicpuEntry(uint16_t entityId) : AccOffloadLocalDramEntry(entityId) {}
    ~AccOffloadLocalDramAicpuEntry() override
    {
        UnInitialize();
    }

    int32_t GetDva(uint64_t hostPtr, uint64_t *dvaPtr) override;

    int32_t SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *sizePtr,
                       uint8_t devIdx) override;

protected:
    int32_t InitializePool(const offload_config_t &config) override;
    void ReleaseBackendResources() override;

private:
    int32_t CreateUrmaPool(const hybm_options &options);
    int32_t ConnectUrmaEntities();
    hybm_entity_t deviceEntity_{nullptr};
};

} // namespace ock::offload

#endif
