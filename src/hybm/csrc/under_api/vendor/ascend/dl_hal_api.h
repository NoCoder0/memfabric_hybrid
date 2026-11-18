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

#ifndef MF_HYBM_CORE_DL_HAL_API_H
#define MF_HYBM_CORE_DL_HAL_API_H

#include <mutex>
#include <string>

#include "dl_hal_api_def.h"
#include "hybm_types.h"

namespace ock {
namespace mf {
using halSqTaskSendFunc = int (*)(uint32_t, halTaskSendInfo *);
using halCqReportRecvFunc = int (*)(uint32_t, halReportRecvInfo *);
using halSqCqAllocateFunc = int (*)(uint32_t, halSqCqInputInfo *, halSqCqOutputInfo *);
using halSqCqFreeFunc = int (*)(uint32_t, halSqCqFreeInfo *);
using halResourceIdAllocFunc = int (*)(uint32_t, struct halResourceIdInputInfo *, struct halResourceIdOutputInfo *);
using halResourceIdFreeFunc = int (*)(uint32_t, struct halResourceIdInputInfo *);
using halResourceConfigFunc = int (*)(uint32_t, struct halResourceIdInputInfo *, struct halResourceConfigInfo *);
using halResourceDetailQueryFunc = int (*)(uint32_t, struct halResourceIdInputInfo *, struct halResourceDetailInfo *);
using halSqCqQueryFunc = int (*)(uint32_t devId, struct halSqCqQueryInfo *info);
using halHostRegisterFunc = int (*)(void *, uint64_t, uint32_t, uint32_t, void **);
using halHostUnregisterExFunc = int (*)(void *, uint32_t, uint32_t);
using drvNotifyIdAddrOffsetFunc = int (*)(uint32_t, struct drvNotifyInfo *);

using halMemAddressReserveFunc = int (*)(void **, size_t, size_t, void *, uint64_t);
using halMemAddressFreeFunc = int (*)(void *);
using halMemCreateFunc = int (*)(drv_mem_handle_t **, size_t, const struct drv_mem_prop *, uint64_t);
using halMemReleaseFunc = int (*)(drv_mem_handle_t *);
using halMemMapFunc = int (*)(void *, size_t, size_t, drv_mem_handle_t *, uint64_t);
using halMemUnmapFunc = int (*)(void *);
using halMemSetAccessFunc = int (*)(void *, size_t, struct drv_mem_access_desc *, size_t);
using halMemExportFunc = int (*)(drv_mem_handle_t *, drv_mem_handle_type, uint64_t, struct MemShareHandle *);
using halMemImportFunc = int (*)(drv_mem_handle_type, struct MemShareHandle *, uint32_t, drv_mem_handle_t **);
using halMemShareHandleSetAttributeFunc = int (*)(uint64_t, enum ShareHandleAttrType, struct ShareHandleAttr);
using halMemTransShareableHandleFunc = int (*)(drv_mem_handle_type, struct MemShareHandle *, uint32_t *, uint64_t *);
using halMemGetAllocationGranularityFunc = int (*)(const struct drv_mem_prop *, drv_mem_granularity_options, size_t *);
using halMemAllocFunc = int (*)(void **, uint64_t, uint64_t);
using halMemFreeFunc = int (*)(void *);
using halMemRetainAllocationHandleFunc = int (*)(drv_mem_handle_t **, void *);
using drvMemGetAttributeFunc = DVresult (*)(DVdeviceptr, struct DVattribute *);
using dcmiInitFunc = int32_t (*)();
using dcmiGetAffinityCpuInfoFunc = int32_t (*)(int32_t, char *, int32_t *);
using dcmiGetUrmaDeviceCntFunc = int (*)(int npuId, unsigned int *devCnt);
using dcmiGetEidListByUrmaDevIndexFunc = int (*)(int npuId, int urmaDevIndex, dcmi_urma_eid_info *eidList, int *eidCnt);
using dcmiGetDevicePcieInfoFunc = int (*)(int npuId, dcmi_pcie_info_all *pcieInfo);

class DlHalApi {
public:
    static Result LoadLibrary(uint32_t gvaVersion);
    static void CleanupLibrary();
    static void CleanupHalApi();
    static Result DcmiGetAffinityCpuInfo(int32_t deviceId, std::string &cpuList);

    static inline int HalSqTaskSend(uint32_t devId, struct halTaskSendInfo *info)
    {
        if (pHalSqTaskSend == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalSqTaskSend(devId, info);
    }

    static inline int HalCqReportRecv(uint32_t devId, struct halReportRecvInfo *info)
    {
        if (pHalCqReportRecv == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalCqReportRecv(devId, info);
    }

    static inline int HalSqCqAllocate(uint32_t devId, struct halSqCqInputInfo *in, struct halSqCqOutputInfo *out)
    {
        if (pHalSqCqAllocate == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalSqCqAllocate(devId, in, out);
    }

    static inline int HalSqCqFree(uint32_t devId, struct halSqCqFreeInfo *info)
    {
        if (pHalSqCqFree == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalSqCqFree(devId, info);
    }

    static inline int HalResourceIdAlloc(uint32_t devId, struct halResourceIdInputInfo *in,
                                         struct halResourceIdOutputInfo *out)
    {
        if (pHalResourceIdAlloc == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalResourceIdAlloc(devId, in, out);
    }

    static inline int HalResourceIdFree(uint32_t devId, struct halResourceIdInputInfo *in)
    {
        if (pHalResourceIdFree == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalResourceIdFree(devId, in);
    }

    static inline int HalResourceConfig(uint32_t devId, struct halResourceIdInputInfo *in,
                                        struct halResourceConfigInfo *para)
    {
        if (pHalResourceConfig == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalResourceConfig(devId, in, para);
    }

    static inline int HalResourceDetailQuery(uint32_t devId, struct halResourceIdInputInfo *in,
                                             struct halResourceDetailInfo *info)
    {
        if (pHalResourceDetailQuery == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalResourceDetailQuery(devId, in, info);
    }

    static inline int HalSqCqQuery(uint32_t devId, struct halSqCqQueryInfo *info)
    {
        if (pHalSqCqQuery == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalSqCqQuery(devId, info);
    }

    static inline int HalHostRegister(void *srcPtr, uint64_t size, uint32_t flag, uint32_t devid, void **dstPtr)
    {
        if (pHalHostRegister == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalHostRegister(srcPtr, size, flag, devid, dstPtr);
    }

    static inline int HalHostUnregisterEx(void *srcPtr, uint32_t devid, uint32_t flag)
    {
        if (pHalHostUnregisterEx == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalHostUnregisterEx(srcPtr, devid, flag);
    }

    static inline int DrvNotifyIdAddrOffset(uint32_t deviceId, struct drvNotifyInfo *drvInfo)
    {
        if (pDrvNotifyIdAddrOffset == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pDrvNotifyIdAddrOffset(deviceId, drvInfo);
    }

    static inline int HalMemAddressReserve(void **ptr, size_t size, size_t alignment, void *addr, uint64_t flag)
    {
        if (pHalMemAddressReserve == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemAddressReserve(ptr, size, alignment, addr, flag);
    }

    static inline int HalMemAddressFree(void *ptr)
    {
        if (pHalMemAddressFree == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemAddressFree(ptr);
    }

    static inline int HalMemCreate(drv_mem_handle_t **handle, size_t size, struct drv_mem_prop *prop, uint64_t flag)
    {
        if (pHalMemCreate == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemCreate(handle, size, prop, flag);
    }

    static inline int HalMemRelease(drv_mem_handle_t *handle)
    {
        if (pHalMemRelease == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemRelease(handle);
    }

    static inline int HalMemMap(void *ptr, size_t size, size_t offset, drv_mem_handle_t *handle, uint64_t flag)
    {
        if (pHalMemMap == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemMap(ptr, size, offset, handle, flag);
    }

    static inline int HalMemUnmap(void *ptr)
    {
        if (pHalMemUnmap == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemUnmap(ptr);
    }

    static inline int HalMemSetAccess(void *ptr, size_t size, struct drv_mem_access_desc *desc, size_t count)
    {
        if (pHalMemSetAccess == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemSetAccess(ptr, size, desc, count);
    }

    static inline int HalMemExport(drv_mem_handle_t *handle, drv_mem_handle_type type, uint64_t flags,
                                   struct MemShareHandle *sHandle)
    {
        if (pHalMemExport == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemExport(handle, type, flags, sHandle);
    }

    static inline int HalMemImport(drv_mem_handle_type type, struct MemShareHandle *sHandle, uint32_t devid,
                                   drv_mem_handle_t **handle)
    {
        if (pHalMemImport == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemImport(type, sHandle, devid, handle);
    }

    // 对用户已分配（非 halMemCreate 出自本模块）的内存取 allocation 引用，用于后续 HalMemExport；
    // 旧驱动无此符号（OPTIONAL 加载），仅 A5 VMM 共享路径使用
    static inline int HalMemRetainAllocationHandle(drv_mem_handle_t **handle, void *ptr)
    {
        if (pHalMemRetainAllocationHandle == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemRetainAllocationHandle(handle, ptr);
    }

    static inline int HalMemShareHandleSetAttribute(uint64_t handle, enum ShareHandleAttrType type,
                                                    struct ShareHandleAttr attr)
    {
        if (pHalMemShareHandleSetAttribute == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemShareHandleSetAttribute(handle, type, attr);
    }

    static inline int HalMemTransShareableHandle(drv_mem_handle_type type, struct MemShareHandle *handle,
                                                 uint32_t *serverId, uint64_t *shareableHandle)
    {
        if (pHalMemTransShareableHandle == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemTransShareableHandle(type, handle, serverId, shareableHandle);
    }

    static inline int HalMemGetAllocationGranularity(const struct drv_mem_prop *prop,
                                                     drv_mem_granularity_options option, size_t *granularity)
    {
        if (pHalMemGetAllocationGranularity == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemGetAllocationGranularity(prop, option, granularity);
    }

    static inline int HalMemAlloc(void **pp, uint64_t size, uint64_t flag)
    {
        if (pHalMemAlloc == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemAlloc(pp, size, flag);
    }

    static inline int HalMemFree(void *pp)
    {
        if (pHalMemFree == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHalMemFree(pp);
    }

    static inline int DrvMemGetAttribute(DVdeviceptr vptr, struct DVattribute *attr)
    {
        if (pDrvMemGetAttribute == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pDrvMemGetAttribute(vptr, attr);
    }

    static inline int DcmiGetUrmaDeviceCnt(int npuId, unsigned int *devCnt)
    {
        if (pDcmiGetUrmaDeviceCnt == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pDcmiGetUrmaDeviceCnt(npuId, devCnt);
    }

    static inline int DcmiGetEidListByUrmaDevIndex(int npuId, int urmaDevIndex, dcmi_urma_eid_info *eidList,
                                                   int *eidCnt)
    {
        if (pDcmiGetEidListByUrmaDevIndex == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pDcmiGetEidListByUrmaDevIndex(npuId, urmaDevIndex, eidList, eidCnt);
    }

    static inline int DcmiGetDevicePcieInfo(int npuId, dcmi_pcie_info_all *pcieInfo)
    {
        if (pDcmiGetDevicePcieInfo == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pDcmiGetDevicePcieInfo(npuId, pcieInfo);
    }

private:
    static Result LoadHybmVmmLibrary(uint32_t gvaVersion);
    static Result LoadDcmiLibrary();

private:
    static std::mutex gMutex;
    static bool gLoaded;
    static void *halHandle;
    static const char *gAscendHalLibName;
    static void *dcmiHandle;
    static const char *gDcmiLibName;
    static dcmiInitFunc pDcmiInit;
    static dcmiGetAffinityCpuInfoFunc pDcmiGetAffinityCpuInfo;

    static halSqTaskSendFunc pHalSqTaskSend;
    static halCqReportRecvFunc pHalCqReportRecv;
    static halSqCqAllocateFunc pHalSqCqAllocate;
    static halSqCqFreeFunc pHalSqCqFree;
    static halResourceIdAllocFunc pHalResourceIdAlloc;
    static halResourceIdFreeFunc pHalResourceIdFree;
    static halResourceConfigFunc pHalResourceConfig;
    static halResourceDetailQueryFunc pHalResourceDetailQuery;
    static halSqCqQueryFunc pHalSqCqQuery;
    static halHostRegisterFunc pHalHostRegister;
    static halHostUnregisterExFunc pHalHostUnregisterEx;
    static drvNotifyIdAddrOffsetFunc pDrvNotifyIdAddrOffset;

    static halMemAddressReserveFunc pHalMemAddressReserve;
    static halMemAddressFreeFunc pHalMemAddressFree;
    static halMemCreateFunc pHalMemCreate;
    static halMemReleaseFunc pHalMemRelease;
    static halMemMapFunc pHalMemMap;
    static halMemUnmapFunc pHalMemUnmap;
    static halMemSetAccessFunc pHalMemSetAccess;
    static halMemExportFunc pHalMemExport;
    static halMemImportFunc pHalMemImport;
    static halMemShareHandleSetAttributeFunc pHalMemShareHandleSetAttribute;
    static halMemTransShareableHandleFunc pHalMemTransShareableHandle;
    static halMemGetAllocationGranularityFunc pHalMemGetAllocationGranularity;
    static halMemAllocFunc pHalMemAlloc;
    static halMemFreeFunc pHalMemFree;
    static halMemRetainAllocationHandleFunc pHalMemRetainAllocationHandle;
    static drvMemGetAttributeFunc pDrvMemGetAttribute;
    static dcmiGetUrmaDeviceCntFunc pDcmiGetUrmaDeviceCnt;
    static dcmiGetEidListByUrmaDevIndexFunc pDcmiGetEidListByUrmaDevIndex;
    static dcmiGetDevicePcieInfoFunc pDcmiGetDevicePcieInfo;
};

} // namespace mf
} // namespace ock

#endif // MF_HYBM_CORE_DL_HAL_API_H
