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
#ifndef MEMFABRIC_HYBRID_ACC_LOCAL_DRAM_OFFLOAD_ENTRY_H
#define MEMFABRIC_HYBRID_ACC_LOCAL_DRAM_OFFLOAD_ENTRY_H

#include <mutex>
#include <memory>
#include "hybm_def.h"
#include "acc_offload.h"
#include "acc_offload_entry.h"
#include "acc_offload_mem_manager.h"

namespace ock {
namespace offload {

class AccOffloadLocalDramEntry : public AccOffloadEntry {
public:
    explicit AccOffloadLocalDramEntry(uint16_t entityId) : entityId_(entityId) {}
    ~AccOffloadLocalDramEntry() override = default;

    AccOffloadLocalDramEntry(const AccOffloadLocalDramEntry &) = delete;
    AccOffloadLocalDramEntry &operator=(const AccOffloadLocalDramEntry &) = delete;

    int32_t Initialize(const offload_config_t &config) final;

    void UnInitialize() final;

public:
    void *MallocHost(size_t size) override;

    void FreeHost(void *ptr) override;

    int32_t HostRegister(uint64_t hostPtr) override;

    int32_t HostFree(uint64_t hostPtr, uint64_t flags) override;

protected:
    virtual int32_t InitializePool(const offload_config_t &config) = 0;
    virtual void ReleaseBackendResources() {}

    // Caller holds mutex_, including initialization failure paths.
    void Cleanup();

    int32_t CreateHostPool(const hybm_options &options);

    int32_t ValidateHostPointer(uint64_t hostPtr) const;

    std::mutex mutex_;
    const uint16_t entityId_;
    uint32_t deviceId_{0};
    bool poolReserved_{false};
    bool inited_ = false;
    bool hybmInitialized_ = false;
    bool launchLoaded_ = false;
    hybm_entity_t entity_ = nullptr;
    hybm_mem_slice_t slice_ = nullptr;
    uint8_t *base_ = nullptr;
    uint64_t size_ = 0;
    std::shared_ptr<AccOffloadMemManager> memMng_;
};

} // namespace offload
} // namespace ock

#endif // MEMFABRIC_HYBRID_ACC_LOCAL_DRAM_OFFLOAD_ENTRY_H
