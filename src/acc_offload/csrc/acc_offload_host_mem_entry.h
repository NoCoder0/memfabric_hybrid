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
#ifndef MEMFABRIC_HYBRID_ACC_OFFLOAD_HOST_MEM_ENTRY_H
#define MEMFABRIC_HYBRID_ACC_OFFLOAD_HOST_MEM_ENTRY_H

#include <memory>
#include <mutex>
#include <string>
#include "hybm_def.h"
#include "smem_net_group_engine.h"
#include "acc_offload.h"
#include "acc_offload_entry.h"
#include "acc_offload_mem_manager.h"

namespace ock {
namespace offload {

class AccOffloadHostMemEntry : public AccOffloadEntry {
public:
    explicit AccOffloadHostMemEntry(uint16_t entityId) : entityId_(entityId) {}
    ~AccOffloadHostMemEntry() override
    {
        UnInitialize();
    };

    AccOffloadHostMemEntry(const AccOffloadHostMemEntry &) = delete;
    AccOffloadHostMemEntry &operator=(const AccOffloadHostMemEntry &) = delete;

    int32_t Initialize(const offload_config_t &config) override;

    void UnInitialize() override;

public:
    void *MallocHost(size_t size) override;

    void FreeHost(void *ptr) override;

    uint64_t HostMalloc(uint64_t size, uint64_t flags) override;

    int32_t HostRegister(uint64_t hostPtr) override;

    int32_t HostGetDevicePtr(uint64_t hostPtr, uint64_t *dvaPtr) override;

    int32_t HostFree(uint64_t hostPtr, uint64_t flags) override;

private:
    int32_t ValidateConfig(const offload_config_t &config);

    uint16_t entityId_;

    int32_t CreateEntity(const offload_config_t &config);

    /* back the whole reserve as one hybm slice and set up the in-pool allocator */
    int32_t AllocatePool();

    int32_t ResolveStoreUrl(const offload_config_t &config);

    int32_t CreateGroupStore(const offload_config_t &config);

    int32_t ExchangeAndValidatePoolFingerprint(const offload_config_t &config);

    /* export/import/mmap the whole pool once: all later in-pool splits share this mapping */
    int32_t RegisterPoolShared();

    bool InPool(uint64_t hostPtr) const;

    std::mutex mutex_;
    bool inited_ = false;
    bool hybmInited_ = false;
    bool launchLoaded_ = false;
    offload_scene_t scene_ = OFFLOAD_SCENE_LOCAL;
    std::string storeUrl_;
    bool storeUrlHeld_ = false;
    smem::StorePtr entryStore_;
    smem::SmemGroupEnginePtr group_;
    hybm_entity_t entity_ = nullptr;
    hybm_options options_{};
    void *poolGva_ = nullptr;
    hybm_mem_slice_t poolSlice_ = nullptr;
    void *poolBase_ = nullptr;
    std::unique_ptr<AccOffloadMemManager> memManager_;
};

} // namespace offload
} // namespace ock

#endif // MEMFABRIC_HYBRID_ACC_OFFLOAD_HOST_MEM_ENTRY_H
