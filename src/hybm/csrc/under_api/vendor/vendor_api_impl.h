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

#ifndef MF_HYBM_CORE_VENDOR_API_IMPL_H
#define MF_HYBM_CORE_VENDOR_API_IMPL_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "hybm_common_include.h"

namespace ock {
namespace mf {
class VendorApiImpl {
public:
    virtual ~VendorApiImpl() = default;
    virtual Result LoadLibrary()
    {
        return BM_OK;
    }
    virtual void CleanupLibrary()
    {
        return;
    }

    virtual Result SetDevice(int32_t deviceId, bool force = false)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result GetDevice(int32_t *deviceId)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result GetDeviceCount(uint32_t *count)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result GetDeviceInfo(uint32_t deviceId, int32_t moduleType, int32_t infoType, int64_t *val)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result DeviceGetBareTgid(uint32_t *pid)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result CreateStream(void **stream)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result CreateStreamWithConfig(void **stream, uint32_t prot, uint32_t config)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result StreamGetId(void *stream, int32_t *streamId)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result RtStreamGetSqid(const void *stream, uint32_t *sqId)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result RtStreamGetCqid(const void *stream, uint32_t *cqId, uint32_t *logicCqId)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result DestroyStream(void *stream)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result SynchronizeStream(void *stream)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result StreamAbort(void *stream)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result SetStreamAttribute(void *stream, int32_t attrType, void *attrValue)
    {
        return BM_UNDER_API_UNLOAD;
    }

    virtual Result CreateNotify(void **notify, uint64_t flag)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result GetNotifyId(void *notify, uint32_t *notifyId)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result DestroyNotify(void *notify)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result WaitAndResetNotify(void *notify, void *stream, uint32_t timeout)
    {
        return BM_UNDER_API_UNLOAD;
    }

    virtual Result RtGetDevResAddress(void *resInfo, void *addrInfo)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result OpenNetService(void *args)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result CloseNetService()
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result RDMADBSend(uint32_t dbIndex, uint64_t dbInfo, void *stream)
    {
        return BM_UNDER_API_UNLOAD;
    }

    virtual Result GetCurrentContext(void **context)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result SetCurrentContext(void *context)
    {
        return BM_UNDER_API_UNLOAD;
    }

    virtual Result BinaryLoadFromFile(const char *binPath, void *options, void **binHandle)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result BinaryGetFunction(void *binHandle, const char *kernelName, void **funcHandle)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result LaunchKernelWithConfig(void *funcHandle, uint32_t blockDim, void *stream, void *cfg,
                                          void *argsHandle, void *reserved)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result KernelArgsInit(void *funcHandle, void **argsHandle)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result KernelArgsAppend(void *argsHandle, void *param, size_t paramSize, void **paramHandle)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result KernelArgsFinalize(void *argsHandle)
    {
        return BM_UNDER_API_UNLOAD;
    }

    virtual Result Malloc(void **ptr, size_t size, uint32_t type)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result Free(void *ptr)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result MallocHost(void **ptr, size_t size)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result FreeHost(void *ptr)
    {
        return BM_UNDER_API_UNLOAD;
    }

    virtual Result Memcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind)
    {
        if (kind != ACL_MEMCPY_HOST_TO_HOST || count > destMax) {
            return BM_INVALID_PARAM;
        }
        std::memcpy(dst, src, count);
        return BM_OK;
    }
    virtual Result MemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind, void *stream)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result MemcpyAsyncWithoutCheckKind(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind,
                                               void *stream)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result MemcpyBatch(void **dsts, size_t *destMax, void **srcs, size_t *sizes, size_t numBatches, void *attrs,
                               size_t *attrsIndexes, size_t numAttrs, size_t *failIndex)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result Memset(void *dst, size_t destMax, int32_t value, size_t count)
    {
        return BM_UNDER_API_UNLOAD;
    }

    virtual Result IpcSetMemoryName(const void *ptr, uint64_t byteCount, char *name, uint32_t len)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result IpcOpenMemory(void **ptr, const char *name)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result IpcCloseMemory(const void *ptr)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result IpcDestroyMemoryName(const char *name)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result IpcSetMemorySuperPodPid(const char *name, uint32_t sdid, int32_t *pid, int32_t num)
    {
        return BM_UNDER_API_UNLOAD;
    }

    virtual Result EnableP2P(uint32_t devIdDes, uint32_t phyIdSrc, uint32_t flag)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result DisableP2P(uint32_t devIdDes, uint32_t phyIdSrc)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result GetP2PStatus(uint32_t devId, uint32_t peerDevId, uint32_t &status)
    {
        return BM_UNDER_API_UNLOAD;
    }

    virtual AscendSocType GetSocName()
    {
        return ASCEND_UNKNOWN;
    }
    virtual Result GetVersion(int32_t *major, int32_t *minor, int32_t *patch)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result GetSysVersionStr(char *pkgName, char *versionStr)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result GetPhyDevIdByLogicDevId(int32_t logicDevId, int32_t *phyDevId)
    {
        return BM_UNDER_API_UNLOAD;
    }
    virtual Result GetLogicDevIdByUserDevId(int32_t userDevId, int32_t *logicDevId)
    {
        return BM_UNDER_API_UNLOAD;
    }
};

} // namespace mf
} // namespace ock
#endif // MF_HYBM_CORE_VENDOR_API_IMPL_H
