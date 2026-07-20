/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef DL_CANN_API_H
#define DL_CANN_API_H

#include "zbal_common_includes.h"
#include "dl_cann_api_def.h"

namespace zbal {
namespace underapi {
using aclrtGetSocNameFunc = const char *(*)();
using aclrtSetDeviceFunc = int32_t (*)(int32_t);
using aclrtGetDeviceFunc = int32_t (*)(int32_t *);
using aclrtSynchronizeStreamFunc = int (*)(void *);
using aclrtMallocFunc = int32_t (*)(void **, size_t, uint32_t);
using aclrtFreeFunc = int (*)(void *);
using aclrtMallocHostFunc = int32_t (*)(void **, size_t);
using aclrtFreeHostFunc = int (*)(void *);
using aclrtMemcpyFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t);
using aclrtMemcpyAsyncFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t, void *);
using aclrtMemsetFunc = int32_t (*)(void *, size_t, int32_t, size_t);
using rtGetLogicDevIdByUserDevIdFunc = int32_t (*)(const int32_t, int32_t *const);
using rtGetC2cCtrlAddrFunc = int32_t (*)(uint64_t *, uint32_t *);
using aclrtGetResInCurrentThreadFunc = int32_t (*)(aclrtDevResType, uint32_t *);
using aclrtHostRegisterFunc = int32_t (*)(void *, uint64_t, aclrtHostRegisterType, void **);
using aclrtHostUnregisterFunc = int32_t (*)(void *);
using aclrtSynchronizeEventFunc = int (*)(void *);
using aclrtReserveMemAddressFunc = int (*)(void **, size_t, size_t, void *, uint64_t);
using aclrtReleaseMemAddressFunc = int (*)(void *);
using aclrtMallocPhysicalFunc = int32_t (*)(void **, size_t, aclrtPhysicalMemProp *, uint64_t);
using aclrtFreePhysicalFunc = int32_t (*)(void *);
using aclrtMapMemFunc = int32_t (*)(void *, size_t, size_t, void *, uint64_t);
using aclrtUnmapMemFunc = int32_t (*)(void *);
using aclrtMallocAlign32Func = int32_t (*)(void **, size_t, aclrtMemMallocPolicy);
using aclrtGetCurrentContextFunc = int32_t (*)(aclrtContext *);
using aclrtSetCurrentContextFunc = int32_t (*)(aclrtContext);
using aclrtGetMemInfoFunc = int32_t (*)(aclrtMemAttr, size_t *, size_t *);

/* Kernel launch */
using aclrtCreateStreamWithConfigFunc = int32_t (*)(void **, int32_t, uint32_t);
using aclrtDestroyStreamFunc = int32_t (*)(void *);
using aclrtBinaryLoadFromFileFunc = int32_t (*)(const char *, aclrtBinaryLoadOptions *, aclrtBinHandle *);
using aclrtBinaryGetFunctionFunc = int32_t (*)(const aclrtBinHandle, const char *, aclrtFuncHandle *);
using aclrtBinaryUnLoadFunc = int32_t (*)(aclrtBinHandle);
using aclrtLaunchKernelWithHostArgsFunc = int32_t (*)(aclrtFuncHandle, uint32_t, void *, aclrtLaunchKernelCfg *, void *,
                                                      uint64_t, void *, uint64_t);

class DlCannApi {
public:
    static ZResult LoadLibrary(const std::string &libDirPath);
    static void CleanupLibrary();

public:
    static const char *AclrtGetSocName();
    static ZResult AclrtSetDevice(int32_t deviceId, bool force = false);
    static ZResult AclrtGetDevice(int32_t *deviceId);
    static ZResult AclrtSynchronizeStream(void *stream);
    static ZResult AclrtMalloc(void **ptr, size_t count, uint32_t type);
    static ZResult AclrtFree(void *ptr);
    static ZResult AclrtMallocHost(void **ptr, size_t count);
    static ZResult AclrtFreeHost(void *ptr);
    static ZResult AclrtMemcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind);
    static ZResult AclrtMemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind,
                                    void *stream);
    static ZResult AclrtMemset(void *ptr, size_t maxCount, int32_t value, size_t count);
    static ZResult RtGetLogicDevIdByUserDevId(const int32_t userDevId, int32_t *const logicDevId);
    static ZResult RtGetC2cCtrlAddr(uint64_t *address, uint32_t *len);
    static ZResult AclrtGetResInCurrentThread(aclrtDevResType type, uint32_t *value);
    static ZResult AclrtGetAIVCountInCurrentThread(uint32_t *value);
    static ZResult AclrtGetAICCountInCurrentThread(uint32_t *value);
    static ZResult AclrtHostRegister(void *hostPtr, uint64_t size, void **outDevPtr);
    static ZResult AclrtHostUnRegister(void *hostPtr);
    static ZResult AclrtSynchronizeEvent(void *event);
    static ZResult AclrtReserveMemAddress(void **virPtr, size_t size, size_t alignment, void *expectPtr,
                                          uint64_t flags);
    static ZResult AclrtReleaseMemAddress(void *virPtr);
    static ZResult AclrtMallocPhysical(void **ptr, size_t size, aclrtPhysicalMemProp *prop, uint64_t flags);
    static ZResult AclrtFreePhysical(void *handle);
    static ZResult AclrtMapMem(void *virPtr, size_t size, size_t offset, void *handle, uint64_t flags);
    static ZResult AclrtUnmapMem(void *virPtr);
    static ZResult AclrtMallocAlign32(void **ptr, size_t size, aclrtMemMallocPolicy policy);
    static ZResult AclrtGetCurrentContext(aclrtContext *ctx);
    static ZResult AclrtSetCurrentContext(aclrtContext ctx);
    static ZResult AclrtGetMemInfo(aclrtMemAttr attr, size_t *freeBytes, size_t *total);

    /* Kernel launch */
    static ZResult AclrtCreateStreamWithConfig(void **stream, int32_t priority, uint32_t flag);
    static ZResult AclrtDestroyStream(void *stream);
    static ZResult AclrtBinaryLoadFromFile(const char *binPath, aclrtBinaryLoadOptions *options,
                                           aclrtBinHandle *binHandle);
    static ZResult AclrtBinaryGetFunction(aclrtBinHandle binHandle, const char *kernelName,
                                          aclrtFuncHandle *funcHandle);
    static ZResult AclrtBinaryUnLoad(aclrtBinHandle binHandle);
    static ZResult AclrtKernelArgsInit(aclrtFuncHandle funcHandle, aclrtArgsHandle *argsHandle);
    static ZResult AclrtKernelArgsAppend(aclrtArgsHandle argsHandle, void *param, size_t paramSize,
                                         aclrtParamHandle *paramHandle);
    static ZResult AclrtKernelArgsFinalize(aclrtArgsHandle argsHandle);
    static ZResult AclrtLaunchKernelWithConfig(aclrtFuncHandle funcHandle, uint32_t blockDim, void *stream,
                                               aclrtLaunchKernelCfg *cfg, aclrtArgsHandle argsHandle, void *reserved);
    static ZResult AclrtLaunchKernelWithHostArgs(aclrtFuncHandle funcHandle, uint32_t blockDim, void *stream,
                                                 aclrtLaunchKernelCfg *cfg, void *hostArgs, uint64_t hostArgsSize);

private:
    static std::mutex gMutex;
    static bool gLoaded;
    static void *gAclHandle;
    static const char *gAscendAclLibName;

    static aclrtGetSocNameFunc pAclrtGetSocName;
    static aclrtSetDeviceFunc pAclrtSetDevice;
    static aclrtGetDeviceFunc pAclrtGetDevice;
    static aclrtSynchronizeStreamFunc pAclrtSynchronizeStream;
    static aclrtMallocFunc pAclrtMalloc;
    static aclrtFreeFunc pAclrtFree;
    static aclrtMallocHostFunc pAclrtMallocHost;
    static aclrtFreeHostFunc pAclrtFreeHost;
    static aclrtMemcpyFunc pAclrtMemcpy;
    static aclrtMemcpyAsyncFunc pAclrtMemcpyAsync;
    static aclrtMemsetFunc pAclrtMemset;
    static rtGetLogicDevIdByUserDevIdFunc pRtGetLogicDevIdByUserDevId;
    static rtGetC2cCtrlAddrFunc pRtGetC2cCtrlAddr;
    static aclrtGetResInCurrentThreadFunc pAclrtGetResInCurrentThread;
    static aclrtHostRegisterFunc pAclrtHostRegister;
    static aclrtHostUnregisterFunc pAclrtHostUnregister;
    static aclrtSynchronizeEventFunc pAclrtSynchronizeEvent;
    static aclrtReserveMemAddressFunc pAclrtReserveMemAddress;
    static aclrtReleaseMemAddressFunc pAclrtReleaseMemAddress;
    static aclrtMallocPhysicalFunc pAclrtMallocPhysical;
    static aclrtFreePhysicalFunc pAclrtFreePhysical;
    static aclrtMapMemFunc pAclrtMapMem;
    static aclrtUnmapMemFunc pAclrtUnmapMem;
    static aclrtMallocAlign32Func pAclrtMallocAlign32;
    static aclrtGetCurrentContextFunc pAclrtGetCurrentContext;
    static aclrtSetCurrentContextFunc pAclrtSetCurrentContext;
    static aclrtGetMemInfoFunc pAclrtGetMemInfo;

    /* Kernel launch pointers */
    static aclrtCreateStreamWithConfigFunc pAclrtCreateStreamWithConfig;
    static aclrtDestroyStreamFunc pAclrtDestroyStream;
    static aclrtBinaryLoadFromFileFunc pAclrtBinaryLoadFromFile;
    static aclrtBinaryGetFunctionFunc pAclrtBinaryGetFunction;
    static aclrtBinaryUnLoadFunc pAclrtBinaryUnLoad;
    static aclrtLaunchKernelWithHostArgsFunc pAclrtLaunchKernelWithHostArgs;

    static ZResult LoadRtSymbols();
};

inline const char *DlCannApi::AclrtGetSocName()
{
    return pAclrtGetSocName();
}

inline ZResult DlCannApi::AclrtSetDevice(int32_t deviceId, bool force)
{
    if (UNLIKELY(pAclrtSetDevice == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }

    if (force) {
        return pAclrtSetDevice(deviceId);
    }
    int32_t nowDeviceId = -1;
    if (AclrtGetDevice(&nowDeviceId) == 0 && nowDeviceId == deviceId) {
        return Z_OK;
    } else {
        return pAclrtSetDevice(deviceId);
    }
}

inline ZResult DlCannApi::AclrtGetDevice(int32_t *deviceId)
{
    if (UNLIKELY(pAclrtGetDevice == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtGetDevice(deviceId);
}

inline ZResult DlCannApi::AclrtSynchronizeStream(void *stream)
{
    if (UNLIKELY(pAclrtSynchronizeStream == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtSynchronizeStream(stream);
}

inline ZResult DlCannApi::AclrtSynchronizeEvent(void *event)
{
    if (UNLIKELY(pAclrtSynchronizeEvent == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtSynchronizeEvent(event);
}

inline ZResult DlCannApi::AclrtReserveMemAddress(void **virPtr, size_t size, size_t alignment, void *expectPtr,
                                                 uint64_t flags)
{
    if (UNLIKELY(pAclrtReserveMemAddress == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtReserveMemAddress(virPtr, size, alignment, expectPtr, flags);
}

inline ZResult DlCannApi::AclrtReleaseMemAddress(void *virPtr)
{
    if (UNLIKELY(pAclrtReleaseMemAddress == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtReleaseMemAddress(virPtr);
}

inline ZResult DlCannApi::AclrtMallocPhysical(void **ptr, size_t size, aclrtPhysicalMemProp *prop, uint64_t flags)
{
    if (UNLIKELY(pAclrtMallocPhysical == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtMallocPhysical(ptr, size, prop, flags);
}

inline ZResult DlCannApi::AclrtFreePhysical(void *handle)
{
    if (UNLIKELY(pAclrtFreePhysical == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtFreePhysical(handle);
}

inline ZResult DlCannApi::AclrtMapMem(void *virPtr, size_t size, size_t offset, void *handle, uint64_t flags)
{
    if (UNLIKELY(pAclrtMapMem == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtMapMem(virPtr, size, offset, handle, flags);
}

inline ZResult DlCannApi::AclrtUnmapMem(void *virPtr)
{
    if (UNLIKELY(pAclrtUnmapMem == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtUnmapMem(virPtr);
}

inline ZResult DlCannApi::AclrtMallocAlign32(void **ptr, size_t size, aclrtMemMallocPolicy policy)
{
    if (UNLIKELY(pAclrtMallocAlign32 == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtMallocAlign32(ptr, size, policy);
}

inline ZResult DlCannApi::AclrtGetCurrentContext(aclrtContext *ctx)
{
    if (UNLIKELY(pAclrtGetCurrentContext == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtGetCurrentContext(ctx);
}

inline ZResult DlCannApi::AclrtSetCurrentContext(aclrtContext ctx)
{
    if (UNLIKELY(pAclrtSetCurrentContext == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtSetCurrentContext(ctx);
}

inline ZResult DlCannApi::AclrtGetMemInfo(aclrtMemAttr attr, size_t *freeBytes, size_t *total)
{
    if (UNLIKELY(pAclrtGetMemInfo == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtGetMemInfo(attr, freeBytes, total);
}

inline ZResult DlCannApi::AclrtMalloc(void **ptr, size_t count, uint32_t type)
{
    if (UNLIKELY(pAclrtMalloc == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtMalloc(ptr, count, type);
}

inline ZResult DlCannApi::AclrtFree(void *ptr)
{
    if (UNLIKELY(pAclrtFree == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtFree(ptr);
}

inline ZResult DlCannApi::AclrtMallocHost(void **ptr, size_t count)
{
    if (UNLIKELY(pAclrtMallocHost == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtMallocHost(ptr, count);
}

inline ZResult DlCannApi::AclrtFreeHost(void *ptr)
{
    if (UNLIKELY(pAclrtFreeHost == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtFreeHost(ptr);
}

inline ZResult DlCannApi::AclrtMemcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind)
{
    if (UNLIKELY(pAclrtMemcpy == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtMemcpy(dst, destMax, src, count, kind);
}

inline ZResult DlCannApi::AclrtMemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind,
                                           void *stream)
{
    if (UNLIKELY(pAclrtMemcpyAsync == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtMemcpyAsync(dst, destMax, src, count, kind, stream);
}

inline ZResult DlCannApi::AclrtMemset(void *ptr, size_t maxCount, int32_t value, size_t count)
{
    if (UNLIKELY(pAclrtMemset == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtMemset(ptr, maxCount, value, count);
}

inline ZResult DlCannApi::RtGetLogicDevIdByUserDevId(const int32_t userDevId, int32_t *const logicDevId)
{
    if (UNLIKELY(pRtGetLogicDevIdByUserDevId == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pRtGetLogicDevIdByUserDevId(userDevId, logicDevId);
}

inline ZResult DlCannApi::RtGetC2cCtrlAddr(uint64_t *address, uint32_t *len)
{
    if (UNLIKELY(pRtGetC2cCtrlAddr == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pRtGetC2cCtrlAddr(address, len);
}

inline ZResult DlCannApi::AclrtGetResInCurrentThread(aclrtDevResType type, uint32_t *value)
{
    if (UNLIKELY(pAclrtGetResInCurrentThread == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtGetResInCurrentThread(type, value);
}

inline ZResult DlCannApi::AclrtGetAIVCountInCurrentThread(uint32_t *value)
{
    if (UNLIKELY(pAclrtGetResInCurrentThread == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }

    return pAclrtGetResInCurrentThread(ACL_RT_DEV_RES_VECTOR_CORE, value);
}

inline ZResult DlCannApi::AclrtGetAICCountInCurrentThread(uint32_t *value)
{
    if (UNLIKELY(pAclrtGetResInCurrentThread == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }

    return pAclrtGetResInCurrentThread(ACL_RT_DEV_RES_CUBE_CORE, value);
}

inline ZResult DlCannApi::AclrtHostRegister(void *hostPtr, uint64_t size, void **outDevPtr)
{
    if (UNLIKELY(pAclrtHostRegister == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }

    return pAclrtHostRegister(hostPtr, size, ACL_HOST_REGISTER_MAPPED, outDevPtr);
}

inline ZResult DlCannApi::AclrtHostUnRegister(void *hostPtr)
{
    if (UNLIKELY(pAclrtHostUnregister == nullptr)) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtHostUnregister(hostPtr);
}

inline ZResult DlCannApi::AclrtCreateStreamWithConfig(void **stream, int32_t priority, uint32_t flag)
{
    if (UNLIKELY(pAclrtCreateStreamWithConfig == nullptr))
        return Z_DL_FUNCTION_UNLOAD;
    return pAclrtCreateStreamWithConfig(stream, priority, flag);
}

inline ZResult DlCannApi::AclrtDestroyStream(void *stream)
{
    if (UNLIKELY(pAclrtDestroyStream == nullptr))
        return Z_DL_FUNCTION_UNLOAD;
    return pAclrtDestroyStream(stream);
}

inline ZResult DlCannApi::AclrtBinaryLoadFromFile(const char *binPath, aclrtBinaryLoadOptions *options,
                                                  aclrtBinHandle *binHandle)
{
    if (pAclrtBinaryLoadFromFile == nullptr) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtBinaryLoadFromFile(binPath, options, binHandle);
}

inline ZResult DlCannApi::AclrtBinaryGetFunction(aclrtBinHandle binHandle, const char *kernelName,
                                                 aclrtFuncHandle *funcHandle)
{
    if (pAclrtBinaryGetFunction == nullptr) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtBinaryGetFunction(binHandle, kernelName, funcHandle);
}

inline ZResult DlCannApi::AclrtBinaryUnLoad(aclrtBinHandle binHandle)
{
    if (UNLIKELY(pAclrtBinaryUnLoad == nullptr))
        return Z_DL_FUNCTION_UNLOAD;
    return pAclrtBinaryUnLoad(binHandle);
}

inline ZResult DlCannApi::AclrtLaunchKernelWithHostArgs(aclrtFuncHandle funcHandle, uint32_t blockDim, void *stream,
                                                        aclrtLaunchKernelCfg *cfg, void *hostArgs,
                                                        uint64_t hostArgsSize)
{
    if (pAclrtLaunchKernelWithHostArgs == nullptr) {
        return Z_DL_FUNCTION_UNLOAD;
    }
    return pAclrtLaunchKernelWithHostArgs(funcHandle, blockDim, stream, cfg, hostArgs, hostArgsSize, nullptr, 0);
}
} // namespace underapi
} // namespace zbal

#endif // DL_CANN_API_H
