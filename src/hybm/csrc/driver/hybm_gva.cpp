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
#include "dl_api.h"
#include "vendor/vendor_api.h"
#include "vendor/ascend/dl_hal_api.h"
#include "hybm_gva.h"

namespace ock {
namespace mf {

namespace {
int32_t initedLogicDeviceId = -1;
} // namespace

int32_t HybmGetInitedLogicDeviceId()
{
    return initedLogicDeviceId; // logicDeviceId
}

int32_t HybmModernInitMetaGva(void **globalMemoryBase, size_t allocSize, void **allocHandle)
{
    drv_mem_handle_t **handle = (drv_mem_handle_t **)allocHandle;
    uint64_t va = SVM_END_ADDR - GB;
    auto ret = DlHalApi::HalMemAddressReserve(globalMemoryBase, GB, 0, reinterpret_cast<void *>(va), 0);
    if (ret != 0) {
        BM_LOG_ERROR("prepare virtual memory size(" << GB << ") failed. ret: " << ret);
        return BM_ERROR;
    }
    drv_mem_prop memprop;
    memprop.side = MEM_DEV_SIDE;
    memprop.devid = initedLogicDeviceId;
    memprop.module_id = 0;
    memprop.pg_type = MEM_HUGE_PAGE_TYPE;
    memprop.mem_type = MEM_HBM_TYPE;
    memprop.reserve = 0;
    ret = DlHalApi::HalMemCreate(handle, allocSize, &memprop, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemCreate failed: " << ret);
        return BM_ERROR;
    }
    ret = DlHalApi::HalMemMap(reinterpret_cast<void *>(HYBM_DEVICE_META_ADDR), allocSize, 0, *handle, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemMap failed, ret: " << ret << " addr: 0x" << std::hex << HYBM_DEVICE_META_ADDR << " size: 0x"
                                               << allocSize << " handle: " << *handle
                                               << " devid: " << initedLogicDeviceId);
        DlHalApi::HalMemRelease(*handle);
        return BM_ERROR;
    }
    return BM_OK;
}

int32_t hybm_init_hbm_gva(uint16_t deviceId, uint64_t flags, uint64_t &baseAddress, AscendSocType socType,
                          void **allocHandle)
{
#if !defined(ASCEND_NPU)
    return BM_OK;
#else
    if ((flags & HYBM_FLAG_SKIP_DEVICE_INIT) != 0) {
        // Provider opens its NPUs in the provider transport layer; no single-device HBM GVA init here.
        BM_LOG_INFO("skip single-device gva init, deviceId: " << deviceId);
        baseAddress = 0;
        return BM_OK;
    }
    initedLogicDeviceId = -1;
    VendorApi::Api()->GetLogicDevIdByUserDevId(deviceId, &initedLogicDeviceId);
    if (initedLogicDeviceId < 0) {
        BM_LOG_ERROR("RtGetLogicDevIdByUserDevId failed, deviceId: " << deviceId
                                                                     << " logicDeviceId: " << initedLogicDeviceId);
        return BM_ERROR;
    }
    BM_LOG_INFO("Success get deviceId: " << deviceId << ", logicDeviceId: " << initedLogicDeviceId
                                         << " sco:" << (int)socType << " ver:" << HybmGetGvaVersion());
    auto ret = VendorApi::Api()->SetDevice(deviceId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("set device id to be " << deviceId << " failed: " << ret);
        return BM_ERROR;
    }

    if ((flags & HYBM_FLAG_INIT_SHMEM_META) == 0) {
        BM_LOG_DEBUG("skip init shm meta space:" << flags);
        baseAddress = 0;
        return BM_OK;
    } else {
        BM_LOG_DEBUG("restore init flag");
        flags &= ~HYBM_FLAG_INIT_SHMEM_META;
    }

    void *globalMemoryBase = nullptr;
    size_t allocSize = HYBM_DEVICE_INFO_SIZE; // 申请meta空间
    ret = HybmModernInitMetaGva(&globalMemoryBase, allocSize, allocHandle);
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
