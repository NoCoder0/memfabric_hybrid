/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/

#include "hybm_common_include.h"
#include "devmm_svm_gva.h"
#include "dl_api.h"
#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "hybm_cmd.h"
#include "hybm_gva.h"

namespace ock {
namespace mf {

namespace {
int32_t initedLogicDeviceId = -1;

void RollbackModernMetaGva(void **globalMemoryBase, drv_mem_handle_t **handle)
{
    if (handle != nullptr && *handle != nullptr) {
        const auto ret = DlHalApi::HalMemRelease(*handle);
        if (ret != BM_OK) {
            BM_LOG_ERROR("HalMemRelease rollback failed, ret: " << ret << " handle: " << *handle);
        }
        *handle = nullptr;
    }
    if (globalMemoryBase != nullptr && *globalMemoryBase != nullptr) {
        const auto ret = DlHalApi::HalMemAddressFree(*globalMemoryBase);
        if (ret != BM_OK) {
            BM_LOG_ERROR("HalMemAddressFree rollback failed, ret: " << ret << " addr: " << *globalMemoryBase);
        }
        *globalMemoryBase = nullptr;
    }
}

bool ValidateMetaGvaParams(void **globalMemoryBase, size_t allocSize, void *const *allocHandle)
{
    const auto base = globalMemoryBase == nullptr ? nullptr : *globalMemoryBase;
    const auto handle = allocHandle == nullptr ? nullptr : *allocHandle;
    if (globalMemoryBase != nullptr && allocSize == HYBM_DEVICE_CONTROL_SIZE && allocHandle != nullptr &&
        base == nullptr && handle == nullptr) {
        return true;
    }
    BM_LOG_ERROR("invalid control mapping params, allocSize: " << allocSize << " expected: " << HYBM_DEVICE_CONTROL_SIZE
                                                               << " base: " << base << " handle: " << handle);
    return false;
}
} // namespace

int32_t HybmGetInitedLogicDeviceId()
{
    return initedLogicDeviceId; // logicDeviceId
}

int32_t HybmModernInitMetaGva(void **globalMemoryBase, size_t allocSize, void **allocHandle)
{
    BM_LOG_ERROR("HybmModernInitMetaGva");
    if (!ValidateMetaGvaParams(globalMemoryBase, allocSize, allocHandle)) {
        return BM_INVALID_PARAM;
    }
    auto **handle = reinterpret_cast<drv_mem_handle_t **>(allocHandle);
    const uint64_t va = SVM_END_ADDR - GB;
    auto ret = DlHalApi::HalMemAddressReserve(globalMemoryBase, GB, 0, reinterpret_cast<void *>(va), 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemAddressReserve failed, ret: " << ret << " expectedAddr: 0x" << std::hex << va
                                                          << " actualAddr: " << *globalMemoryBase << std::dec
                                                          << " size: " << GB);
        *globalMemoryBase = nullptr;
        return BM_ERROR;
    }
    if (reinterpret_cast<uint64_t>(*globalMemoryBase) != va) {
        BM_LOG_ERROR("HalMemAddressReserve returned unexpected address, expectedAddr: 0x"
                     << std::hex << va << " actualAddr: " << *globalMemoryBase << std::dec << " size: " << GB);
        RollbackModernMetaGva(globalMemoryBase, handle);
        return BM_ERROR;
    }
    drv_mem_prop memprop{};
    memprop.side = MEM_DEV_SIDE;
    memprop.devid = initedLogicDeviceId;
    memprop.pg_type = MEM_HUGE_PAGE_TYPE;
    memprop.mem_type = MEM_HBM_TYPE;
    ret = DlHalApi::HalMemCreate(handle, allocSize, &memprop, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemCreate failed, ret: " << ret << " size: " << allocSize
                                                  << " logicDeviceId: " << initedLogicDeviceId);
        RollbackModernMetaGva(globalMemoryBase, handle);
        return BM_ERROR;
    }
    ret = DlHalApi::HalMemMap(reinterpret_cast<void *>(HYBM_DEVICE_CONTROL_ADDR), allocSize, 0, *handle, 0);
    BM_LOG_ERROR("HalMemMap, ret: " << ret << " addr: 0x" << std::hex << HYBM_DEVICE_CONTROL_ADDR
                                               << " size: 0x" << allocSize << " handle: " << *handle << std::dec
                                               << " logicDeviceId: " << initedLogicDeviceId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemMap failed, ret: " << ret << " addr: 0x" << std::hex << HYBM_DEVICE_CONTROL_ADDR
                                               << " size: 0x" << allocSize << " handle: " << *handle << std::dec
                                               << " logicDeviceId: " << initedLogicDeviceId);
        RollbackModernMetaGva(globalMemoryBase, handle);
        return BM_ERROR;
    }
    return BM_OK;
}

int32_t HybmLegacyInitMetaGva(void **globalMemoryBase, size_t allocSize, uint64_t flags)
{
    auto ret = drv::HalGvaReserveMemory((uint64_t *)globalMemoryBase, allocSize, initedLogicDeviceId, flags);
    if (ret != 0 || reinterpret_cast<uint64_t>(*globalMemoryBase) != (SVM_END_ADDR - GB)) {
        if (ret == 0 && *globalMemoryBase != nullptr) {
            (void)drv::HalGvaUnreserveMemory((uint64_t)(*globalMemoryBase));
        }
        BM_LOG_ERROR("initialize meta memory failed: " << ret << " size:0x" << std::hex << allocSize << " flag:0x"
                                                       << flags << " ret_addr:" << *globalMemoryBase);
        return BM_ERROR;
    }

    ret = drv::HalGvaAlloc(HYBM_DEVICE_META_ADDR, HYBM_DEVICE_INFO_SIZE, 0);
    if (ret != BM_OK) {
        (void)drv::HalGvaUnreserveMemory((uint64_t)(*globalMemoryBase));
        BM_LOG_ERROR("HalGvaAlloc hybm meta memory failed: " << ret);
        return BM_MALLOC_FAILED;
    }
    return BM_OK;
}

int32_t hybm_init_hbm_gva(uint16_t deviceId, uint64_t flags, uint64_t &baseAddress, AscendSocType socType,
                          void **allocHandle)
{
#if !defined(ASCEND_NPU)
    return BM_OK;
#else
    initedLogicDeviceId = -1;
    DlAclApi::RtGetLogicDevIdByUserDevId(deviceId, &initedLogicDeviceId);
    if (initedLogicDeviceId < 0) {
        BM_LOG_ERROR("RtGetLogicDevIdByUserDevId failed, deviceId: " << deviceId
                                                                     << " logicDeviceId: " << initedLogicDeviceId);
        return BM_ERROR;
    }
    BM_LOG_INFO("Success get deviceId: " << deviceId << ", logicDeviceId: " << initedLogicDeviceId
                                         << " sco:" << (int)socType << " ver:" << HybmGvaVersion());
    auto ret = DlAclApi::AclrtSetDevice(deviceId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("set device id to be " << deviceId << " failed: " << ret);
        return BM_ERROR;
    }
    drv::HybmInitialize(initedLogicDeviceId, DlHalApi::GetFd());

    if ((flags & HYBM_FLAG_INIT_SHMEM_META) == 0) {
        BM_LOG_DEBUG("skip init shm meta space:" << flags);
        baseAddress = 0;
        return BM_OK;
    } else {
        BM_LOG_DEBUG("restore init flag");
        flags &= ~HYBM_FLAG_INIT_SHMEM_META;
    }

    void *globalMemoryBase = nullptr;
    if ((socType == AscendSocType::ASCEND_950) || (HybmGetGvaVersion() == HYBM_GVA_V4)) {
        ret = HybmModernInitMetaGva(&globalMemoryBase, HYBM_DEVICE_CONTROL_SIZE, allocHandle);
    } else {
        ret = HybmLegacyInitMetaGva(&globalMemoryBase, HYBM_DEVICE_INFO_SIZE, flags);
    }
    if (ret != BM_OK) {
        BM_LOG_ERROR("hybm init meta gva failed: " << ret);
        return BM_ERROR;
    }
    baseAddress = reinterpret_cast<uint64_t>(globalMemoryBase);
    return BM_OK;
#endif
}

} // namespace mf
} // namespace ock
