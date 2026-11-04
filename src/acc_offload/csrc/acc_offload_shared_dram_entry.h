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
#ifndef MEMFABRIC_HYBRID_ACC_SHARED_DRAM_OFFLOAD_ENTRY_H
#define MEMFABRIC_HYBRID_ACC_SHARED_DRAM_OFFLOAD_ENTRY_H

#include <mutex>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "hybm_def.h"
#include "smem_net_group_engine.h"
#include "acc_offload.h"
#include "acc_offload_entry.h"
#include "acc_offload_mem_manager.h"
#include "acc_offload_launch.h"

namespace ock {
namespace offload {

class AccOffloadSharedDramEntry : public AccOffloadEntry {
public:
    explicit AccOffloadSharedDramEntry(uint16_t entityId) : entityId_(entityId) {}
    ~AccOffloadSharedDramEntry() override
    {
        UnInitialize();
    };

    AccOffloadSharedDramEntry(const AccOffloadSharedDramEntry &) = delete;
    AccOffloadSharedDramEntry &operator=(const AccOffloadSharedDramEntry &) = delete;

    int32_t Initialize(const offload_config_t &config) override;

    void UnInitialize() override;

public:
    void *MallocHost(size_t size) override;

    void FreeHost(void *ptr) override;

    int32_t GetDva(uint64_t hostPtr, uint64_t *dvaPtr) override;

    int32_t HostRegister(uint64_t hostPtr) override;

    int32_t HostFree(uint64_t hostPtr, uint64_t flags) override;

    int32_t SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *sizePtr,
                       uint8_t devIdx) override;

    int32_t GroupPackCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *numLocalExpertPtr,
                          int64_t *groupList, int64_t *packedGroupList, uint8_t devIdx) override;

    int32_t KvExchangeCopy(uint64_t *metaPtr, uint8_t devIdx) override;

    int32_t RegisterEntryTable(uint32_t entryBytes, uint32_t rowsPerSlot) override;

    int32_t EntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint8_t devIdx) override;

private:
    // Caller holds mutex_, including initialization failure paths.
    void Cleanup();

    int32_t ValidateHostPointer(uint64_t hostPtr) const;

    int32_t AllocAndExportHostSlices();

    int32_t ValidateSharedConfig(const offload_config_t &config);

    int32_t InitSharedPool(const offload_config_t &config);

    int32_t ResolveStoreUrl(const offload_config_t &config);

    int32_t CreateGroupStore(const offload_config_t &config);

    smem::StorePtr AcquireStore(const offload_config_t &config);

    void ReleaseStore();

    int32_t CreateEntityAndPool(const offload_config_t &config);

    int32_t ReserveSharedPool();

    int32_t ExportAndImportEntityInfo();

    int32_t MapPool(const offload_config_t &config);

    int32_t ExchangeAndValidatePoolFingerprint(const offload_config_t &config);

private:
    static std::mutex gStoreMutex;
    static std::set<std::string> gStores;

    std::mutex mutex_;
    const uint16_t entityId_;
    bool inited_ = false;
    bool launchLoaded_ = false;
    bool multiNode_ = false;
    std::string storeUrl_;
    smem::StorePtr entryStore_;
    smem::SmemGroupEnginePtr group_;
    hybm_entity_t entity_ = nullptr;
    std::vector<hybm_mem_slice_t> slices_;
    std::vector<hybm_exchange_info> sliceInfos_;
    void *hostGva_ = nullptr;
    hybm_options options_{};
    uint8_t *base_ = nullptr;
    uint64_t size_ = 0;
    AccOffloadEntryGatherLayout entryTable_{};
    bool entryTableRegistered_ = false;
    std::shared_ptr<AccOffloadMemManager> memMng_;
};

} // namespace offload
} // namespace ock

#endif // MEMFABRIC_HYBRID_ACC_SHARED_DRAM_OFFLOAD_ENTRY_H
