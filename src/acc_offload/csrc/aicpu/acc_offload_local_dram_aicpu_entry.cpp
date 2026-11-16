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
#include <thread>
#include "hybm_big_mem.h"
#include "hybm_device_copy.h"
#include "dl_acl_api.h"
#include "acc_offload_launch.h"
#include "acc_offload_entry_manager.h"
#include "acc_offload_local_dram_aicpu_entry.h"

namespace ock::offload {

int32_t AccOffloadLocalDramAicpuEntry::InitializePool(const offload_config_t &config)
{
    if (size_ == 0 || config.flags != 0) {
        OFFLOAD_LOG_ERROR("invalid AICPU pool config, size: " << size_ << ", flags: " << config.flags);
        return OFFLOAD_ERROR;
    }
    hybm_options options{};
    options.memType = HYBM_MEM_TYPE_HOST;
    options.scene = HYBM_SCENE_OFFLOAD;
    options.devId = deviceId_;
    options.maxDRAMSize = size_;
    options.hostVASpace = size_;
    options.dramShmFd = -1;

    const auto soc = ock::mf::DlAclApi::GetAscendSocType();
    int32_t ret = OFFLOAD_OK;
    switch (soc) {
        case ock::mf::AscendSocType::ASCEND_910C:
            options.bmType = HYBM_TYPE_AICPU_INITIATE;
            options.bmDataOpType = HYBM_DOP_TYPE_AIV_SDMA;
            options.rankCount = 1;
            options.flags = HYBM_FLAG_DRAM_MAP_HOST_VA;
            ret = CreateHostPool(options);
            break;
        case ock::mf::AscendSocType::ASCEND_950:
            options.bmType = HYBM_TYPE_HOST_INITIATE;
            options.bmDataOpType = HYBM_DOP_TYPE_HOST_URMA;
            options.rankCount = 2;
            ret = CreateUrmaPool(options);
            break;
        default:
            OFFLOAD_LOG_ERROR("unsupported AICPU offload SoC, soc=" << static_cast<uint32_t>(soc)
                                                                    << " device=" << +deviceId_);
            return OFFLOAD_FUNCTION_FAILED;
    }
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    return hybm_prepare_device_copy(deviceEntity_ != nullptr ? deviceEntity_ : entity_, 0,
                                    reinterpret_cast<uint64_t>(base_), size_);
}

int32_t AccOffloadLocalDramAicpuEntry::CreateUrmaPool(const hybm_options &options)
{
    uint16_t deviceEntityId = 0;
    auto ret = AccOffloadEntryManager::AllocateEntityId(deviceEntityId);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    auto deviceOptions = options;
    deviceOptions.bmDataOpType = HYBM_DOP_TYPE_DEVICE_URMA;
    deviceOptions.rankId = 1;
    deviceOptions.hostVASpace = 0;
    deviceEntity_ = hybm_create_entity(deviceEntityId, &deviceOptions, 0);
    if (deviceEntity_ == nullptr) {
        OFFLOAD_LOG_ERROR("create AICPU URMA device entity failed, entityId=" << deviceEntityId);
        return OFFLOAD_ERROR;
    }
    ret = hybm_reserve_mem_space(deviceEntity_, 0);
    if (ret != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("reserve AICPU URMA device entity failed, entityId=" << deviceEntityId << " ret=" << ret);
        return ret;
    }
    ret = CreateHostPool(options);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    return ConnectUrmaEntities();
}

int32_t AccOffloadLocalDramAicpuEntry::ConnectUrmaEntities()
{
    hybm_exchange_info hostInfo{};
    hybm_exchange_info deviceInfo{};
    hybm_exchange_info sliceInfo{};
    auto ret = hybm_export(entity_, nullptr, HYBM_FLAG_EXPORT_ENTITY, &hostInfo);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = hybm_export(deviceEntity_, nullptr, HYBM_FLAG_EXPORT_ENTITY, &deviceInfo);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    ret = hybm_export(entity_, slice_, 0, &sliceInfo);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    auto connect = [](hybm_entity_t entity, const hybm_exchange_info &peerInfo, uint32_t peerRank) {
        auto result = hybm_import(entity, &peerInfo, 1, nullptr, HYBM_FLAG_EXPORT_ENTITY);
        if (result == OFFLOAD_OK) {
            result = hybm_transport_connect(entity, &peerRank, 1, 0);
        }
        if (result != OFFLOAD_OK) {
            OFFLOAD_LOG_ERROR("AICPU URMA peer setup failed, peerRank=" << peerRank << " ret=" << result);
        }
        return result;
    };
    int32_t hostRet = OFFLOAD_OK;
    int32_t deviceRet = OFFLOAD_OK;
    // Both endpoints must advance the HCOMM handshake before either side can become ready.
    std::thread hostThread([&]() { hostRet = connect(entity_, deviceInfo, 1); });
    std::thread deviceThread([&]() { deviceRet = connect(deviceEntity_, hostInfo, 0); });
    hostThread.join();
    deviceThread.join();
    if (hostRet != OFFLOAD_OK || deviceRet != OFFLOAD_OK) {
        return hostRet != OFFLOAD_OK ? hostRet : deviceRet;
    }
    ret = hybm_import(deviceEntity_, &sliceInfo, 1, nullptr, 0);
    if (ret != OFFLOAD_OK) {
        return ret;
    }
    return hybm_mmap(deviceEntity_, 0);
}

int32_t AccOffloadLocalDramAicpuEntry::GetDva(uint64_t hostPtr, uint64_t *)
{
    OFFLOAD_LOG_ERROR("AICPU copy uses the original host address; get_dva is unsupported, ptr=" << hostPtr);
    return OFFLOAD_FUNCTION_FAILED;
}

int32_t AccOffloadLocalDramAicpuEntry::SparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs,
                                                  uint32_t *sizePtr, uint8_t devIdx)
{
    AccOffloadAicpuParam param{deviceEntity_ != nullptr ? deviceEntity_ : entity_, srcPtrs, dstPtrs, lenPtrs, sizePtr};
    auto callback = [](const AccOffloadAicpuParam &input, void *stream) {
        hybm_device_copy_params params{input.srcPtrs, input.dstPtrs, input.lenPtrs, input.sizePtr, 0, stream, {}};
        return hybm_data_device_copy(input.entity, &params);
    };
    return AccOffloadLaunchApi::AccOffloadAicpuLaunch(callback, param, devIdx);
}

void AccOffloadLocalDramAicpuEntry::ReleaseBackendResources()
{
    if (deviceEntity_ != nullptr) {
        hybm_destroy_entity(deviceEntity_, 0);
        deviceEntity_ = nullptr;
    }
}

} // namespace ock::offload
