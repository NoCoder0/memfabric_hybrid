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
#include <dlfcn.h>

#include <array>
#include <cstring>
#include <mutex>

#include "hybm_define.h"
#include "hybm_logger.h"
#include "dl_hal_api.h"

namespace ock {
namespace mf {
bool DlHalApi::gLoaded = false;
std::mutex DlHalApi::gMutex;
void *DlHalApi::halHandle;
void *DlHalApi::dcmiHandle;

const char *DlHalApi::gAscendHalLibName = "libascend_hal.so";
const char *DlHalApi::gDcmiLibName = "libdcmi.so";

dcmiInitFunc DlHalApi::pDcmiInit = nullptr;
dcmiGetAffinityCpuInfoFunc DlHalApi::pDcmiGetAffinityCpuInfo = nullptr;

halSqTaskSendFunc DlHalApi::pHalSqTaskSend = nullptr;
halCqReportRecvFunc DlHalApi::pHalCqReportRecv = nullptr;
halSqCqAllocateFunc DlHalApi::pHalSqCqAllocate = nullptr;
halSqCqFreeFunc DlHalApi::pHalSqCqFree = nullptr;
halResourceIdAllocFunc DlHalApi::pHalResourceIdAlloc = nullptr;
halResourceIdFreeFunc DlHalApi::pHalResourceIdFree = nullptr;
halResourceConfigFunc DlHalApi::pHalResourceConfig = nullptr;
halResourceDetailQueryFunc DlHalApi::pHalResourceDetailQuery = nullptr;
halSqCqQueryFunc DlHalApi::pHalSqCqQuery = nullptr;
halHostRegisterFunc DlHalApi::pHalHostRegister = nullptr;
halHostUnregisterExFunc DlHalApi::pHalHostUnregisterEx = nullptr;
drvNotifyIdAddrOffsetFunc DlHalApi::pDrvNotifyIdAddrOffset = nullptr;

halMemAddressReserveFunc DlHalApi::pHalMemAddressReserve = nullptr;
halMemAddressFreeFunc DlHalApi::pHalMemAddressFree = nullptr;
halMemCreateFunc DlHalApi::pHalMemCreate = nullptr;
halMemReleaseFunc DlHalApi::pHalMemRelease = nullptr;
halMemMapFunc DlHalApi::pHalMemMap = nullptr;
halMemUnmapFunc DlHalApi::pHalMemUnmap = nullptr;
halMemSetAccessFunc DlHalApi::pHalMemSetAccess = nullptr;
halMemExportFunc DlHalApi::pHalMemExport = nullptr;
halMemImportFunc DlHalApi::pHalMemImport = nullptr;
halMemShareHandleSetAttributeFunc DlHalApi::pHalMemShareHandleSetAttribute = nullptr;
halMemTransShareableHandleFunc DlHalApi::pHalMemTransShareableHandle = nullptr;
halMemGetAllocationGranularityFunc DlHalApi::pHalMemGetAllocationGranularity = nullptr;
halMemAllocFunc DlHalApi::pHalMemAlloc = nullptr;
halMemFreeFunc DlHalApi::pHalMemFree = nullptr;
halMemRetainAllocationHandleFunc DlHalApi::pHalMemRetainAllocationHandle = nullptr;
drvMemGetAttributeFunc DlHalApi::pDrvMemGetAttribute = nullptr;
dcmiGetUrmaDeviceCntFunc DlHalApi::pDcmiGetUrmaDeviceCnt = nullptr;
dcmiGetEidListByUrmaDevIndexFunc DlHalApi::pDcmiGetEidListByUrmaDevIndex = nullptr;
dcmiGetDevicePcieInfoFunc DlHalApi::pDcmiGetDevicePcieInfo = nullptr;

Result DlHalApi::LoadDcmiLibrary()
{
    dcmiHandle = dlopen(gDcmiLibName, RTLD_LAZY | RTLD_LOCAL);
    if (dcmiHandle == nullptr) {
        BM_LOG_INFO("Failed to open library [" << gDcmiLibName << "], error: " << dlerror());
        return BM_DL_FUNCTION_FAILED;
    }

    DL_LOAD_SYM_OPTIONAL(pDcmiInit, dcmiInitFunc, dcmiHandle, "dcmiv2_init");
    DL_LOAD_SYM_OPTIONAL(pDcmiGetAffinityCpuInfo, dcmiGetAffinityCpuInfoFunc, dcmiHandle,
                         "dcmiv2_get_affinity_cpu_info_by_dev_id");
    DL_LOAD_SYM_OPTIONAL(pDcmiGetUrmaDeviceCnt, dcmiGetUrmaDeviceCntFunc, dcmiHandle, "dcmiv2_get_urma_device_cnt");
    DL_LOAD_SYM_OPTIONAL(pDcmiGetEidListByUrmaDevIndex, dcmiGetEidListByUrmaDevIndexFunc, dcmiHandle,
                         "dcmiv2_get_eid_list_by_urma_dev_index");
    DL_LOAD_SYM_OPTIONAL(pDcmiGetDevicePcieInfo, dcmiGetDevicePcieInfoFunc, dcmiHandle, "dcmiv2_get_device_pcie_info");
    if (pDcmiInit == nullptr) {
        BM_LOG_WARN("DCMI v2 interface is unavailable in [" << gDcmiLibName
                                                            << "], DCMI dependent features are disabled");
        pDcmiGetAffinityCpuInfo = nullptr;
        pDcmiGetUrmaDeviceCnt = nullptr;
        pDcmiGetEidListByUrmaDevIndex = nullptr;
        pDcmiGetDevicePcieInfo = nullptr;
        dlclose(dcmiHandle);
        dcmiHandle = nullptr;
        return BM_OK;
    }

    const int32_t ret = pDcmiInit();
    if (ret != 0) {
        BM_LOG_ERROR("Failed to initialize library [" << gDcmiLibName << "], ret: " << ret);
        pDcmiGetUrmaDeviceCnt = nullptr;
        pDcmiGetEidListByUrmaDevIndex = nullptr;
        pDcmiGetDevicePcieInfo = nullptr;
        dlclose(dcmiHandle);
        dcmiHandle = nullptr;
        return ret;
    }
    return BM_OK;
}

Result DlHalApi::DcmiGetAffinityCpuInfo(int32_t deviceId, std::string &cpuList)
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (pDcmiGetAffinityCpuInfo == nullptr) {
        BM_LOG_ERROR("DCMI affinity interface is unavailable, deviceId: " << deviceId);
        return BM_DL_FUNCTION_FAILED;
    }

    constexpr size_t DCMI_CPU_LIST_BUFFER_SIZE = 4096U;
    std::array<char, DCMI_CPU_LIST_BUFFER_SIZE> buffer{};
    int32_t length = static_cast<int32_t>(buffer.size());
    const int32_t ret = pDcmiGetAffinityCpuInfo(deviceId, buffer.data(), &length);
    if (ret != 0) {
        BM_LOG_ERROR("Failed to get DCMI affinity CPU info, deviceId: " << deviceId << ", ret: " << ret);
    }
    cpuList.assign(buffer.data(), strnlen(buffer.data(), buffer.size()));
    return BM_OK;
}

Result DlHalApi::LoadHybmVmmLibrary(uint32_t gvaVersion)
{
    if (gvaVersion != HYBM_GVA_V4) {
        BM_LOG_ERROR("driver version is too old, not support vmm, gvaVersion: " << gvaVersion);
        return BM_ERROR;
    }

    DL_LOAD_SYM(pHalMemAddressReserve, halMemAddressReserveFunc, halHandle, "halMemAddressReserve");
    DL_LOAD_SYM(pHalMemAddressFree, halMemAddressFreeFunc, halHandle, "halMemAddressFree");
    DL_LOAD_SYM(pHalMemCreate, halMemCreateFunc, halHandle, "halMemCreate");
    DL_LOAD_SYM(pHalMemRelease, halMemReleaseFunc, halHandle, "halMemRelease");
    DL_LOAD_SYM(pHalMemMap, halMemMapFunc, halHandle, "halMemMap");
    DL_LOAD_SYM(pHalMemUnmap, halMemUnmapFunc, halHandle, "halMemUnmap");
    DL_LOAD_SYM(pHalMemSetAccess, halMemSetAccessFunc, halHandle, "halMemSetAccess");
    DL_LOAD_SYM(pHalMemExport, halMemExportFunc, halHandle, "halMemExportToShareableHandleV2");
    DL_LOAD_SYM(pHalMemImport, halMemImportFunc, halHandle, "halMemImportFromShareableHandleV2");
    DL_LOAD_SYM_OPTIONAL(pHalMemShareHandleSetAttribute, halMemShareHandleSetAttributeFunc, halHandle,
                         "halMemShareHandleSetAttribute");
    DL_LOAD_SYM(pHalMemTransShareableHandle, halMemTransShareableHandleFunc, halHandle, "halMemTransShareableHandle");
    DL_LOAD_SYM(pHalMemGetAllocationGranularity, halMemGetAllocationGranularityFunc, halHandle,
                "halMemGetAllocationGranularity");
    DL_LOAD_SYM(pHalMemAlloc, halMemAllocFunc, halHandle, "halMemAlloc");
    DL_LOAD_SYM(pHalMemFree, halMemFreeFunc, halHandle, "halMemFree");
    // 旧驱动无此符号：OPTIONAL 加载，仅 A5 VMM 共享路径调用，缺失时 wrapper 返回 BM_UNDER_API_UNLOAD
    DL_LOAD_SYM_OPTIONAL(pHalMemRetainAllocationHandle, halMemRetainAllocationHandleFunc, halHandle,
                         "halMemRetainAllocationHandle");

    return BM_OK;
}

Result DlHalApi::LoadLibrary(uint32_t gvaVersion)
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return BM_OK;
    }

    BM_ASSERT_LOG_AND_RETURN(gvaVersion != HYBM_GVA_UNKNOWN, "gvaVersion = " << gvaVersion, BM_NOT_INITIALIZED);

    halHandle = dlopen(gAscendHalLibName, RTLD_NOW | RTLD_NODELETE);
    if (halHandle == nullptr) {
        BM_LOG_ERROR(
            "Failed to open library ["
            << gAscendHalLibName
            << "], please source ascend-toolkit set_env.sh, or add ascend driver lib path into LD_LIBRARY_PATH,"
            << " error: " << dlerror());
        return BM_DL_FUNCTION_FAILED;
    }

    /* load sym */
    auto ret = LoadHybmVmmLibrary(gvaVersion);
    if (ret != 0) {
        dlclose(halHandle);
        halHandle = nullptr;
        return ret;
    }

    DL_LOAD_SYM(pHalSqTaskSend, halSqTaskSendFunc, halHandle, "halSqTaskSend");
    DL_LOAD_SYM(pHalCqReportRecv, halCqReportRecvFunc, halHandle, "halCqReportRecv");
    DL_LOAD_SYM(pHalSqCqAllocate, halSqCqAllocateFunc, halHandle, "halSqCqAllocate");
    DL_LOAD_SYM(pHalSqCqFree, halSqCqFreeFunc, halHandle, "halSqCqFree");
    DL_LOAD_SYM(pHalResourceIdAlloc, halResourceIdAllocFunc, halHandle, "halResourceIdAlloc");
    DL_LOAD_SYM(pHalResourceIdFree, halResourceIdFreeFunc, halHandle, "halResourceIdFree");
    DL_LOAD_SYM(pHalResourceConfig, halResourceConfigFunc, halHandle, "halResourceConfig");
    DL_LOAD_SYM_OPTIONAL(pHalResourceDetailQuery, halResourceDetailQueryFunc, halHandle, "halResourceDetailQuery");
    DL_LOAD_SYM(pHalSqCqQuery, halSqCqQueryFunc, halHandle, "halSqCqQuery");
    DL_LOAD_SYM(pHalHostRegister, halHostRegisterFunc, halHandle, "halHostRegister");
    DL_LOAD_SYM(pHalHostUnregisterEx, halHostUnregisterExFunc, halHandle, "halHostUnregisterEx");
    DL_LOAD_SYM_OPTIONAL(pDrvNotifyIdAddrOffset, drvNotifyIdAddrOffsetFunc, halHandle, "drvNotifyIdAddrOffset");
    DL_LOAD_SYM_OPTIONAL(pDrvMemGetAttribute, drvMemGetAttributeFunc, halHandle, "drvMemGetAttribute");

    LoadDcmiLibrary();
    gLoaded = true;
    return BM_OK;
}

void DlHalApi::CleanupHalApi()
{
    pHalSqTaskSend = nullptr;
    pHalCqReportRecv = nullptr;
    pHalSqCqAllocate = nullptr;
    pHalSqCqFree = nullptr;
    pHalResourceIdAlloc = nullptr;
    pHalResourceIdFree = nullptr;
    pHalResourceConfig = nullptr;
    pHalResourceDetailQuery = nullptr;
    pHalSqCqQuery = nullptr;
    pHalHostRegister = nullptr;
    pHalHostUnregisterEx = nullptr;
    pDrvNotifyIdAddrOffset = nullptr;

    pHalMemAddressReserve = nullptr;
    pHalMemAddressFree = nullptr;
    pHalMemCreate = nullptr;
    pHalMemRelease = nullptr;
    pHalMemMap = nullptr;
    pHalMemUnmap = nullptr;
    pHalMemSetAccess = nullptr;
    pHalMemExport = nullptr;
    pHalMemImport = nullptr;
    pHalMemGetAllocationGranularity = nullptr;
    pHalMemShareHandleSetAttribute = nullptr;
    pHalMemTransShareableHandle = nullptr;
    pHalMemAlloc = nullptr;
    pHalMemFree = nullptr;
    pHalMemRetainAllocationHandle = nullptr;
    pDrvMemGetAttribute = nullptr;
    pDcmiInit = nullptr;
    pDcmiGetAffinityCpuInfo = nullptr;
    pDcmiGetUrmaDeviceCnt = nullptr;
    pDcmiGetEidListByUrmaDevIndex = nullptr;
    pDcmiGetDevicePcieInfo = nullptr;
}

void DlHalApi::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gLoaded) {
        return;
    }

    CleanupHalApi();

    if (halHandle != nullptr) {
        dlclose(halHandle);
        halHandle = nullptr;
    }
    if (dcmiHandle != nullptr) {
        dlclose(dcmiHandle);
        dcmiHandle = nullptr;
    }
    gLoaded = false;
}
} // namespace mf
} // namespace ock
