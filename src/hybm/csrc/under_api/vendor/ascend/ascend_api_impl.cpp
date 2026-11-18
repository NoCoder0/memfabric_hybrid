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

#include <dlfcn.h>

#include <mutex>

#include "ascend_api_impl.h"
#include "../vendor_api.h"
#include "mf_env_define.h"

namespace ock {
namespace mf {
namespace {
constexpr const char *ASCEND_ACL_LIB_NAME = "libascendcl.so";
constexpr const char *RUNTIME_LIB_NAME = "libruntime.so";
} // namespace

Result AscendApiImpl::LoadLibrary()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (loaded_) {
        return BM_OK;
    }

    std::string realPath = ASCEND_ACL_LIB_NAME;
    if (!env::ASCEND_HOME_PATH.empty()) {
        const std::string libDirPath = env::ASCEND_HOME_PATH + "/lib64";
        if (!FileUtil::LibraryRealPath(libDirPath, std::string(ASCEND_ACL_LIB_NAME), realPath)) {
            BM_LOG_ERROR("get lib path failed, libDir: " << libDirPath << " libName: " << ASCEND_ACL_LIB_NAME);
            return BM_ERROR;
        }
    }

    rtHandle_ = dlopen(realPath.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (rtHandle_ == nullptr) {
        BM_LOG_ERROR("Failed to open library [" << realPath << "], error: " << dlerror());
        return BM_DL_FUNCTION_FAILED;
    }

    DL_LOAD_SYM(pAclrtGetDevice_, aclrtGetDeviceFunc, rtHandle_, "aclrtGetDevice");
    DL_LOAD_SYM_OPTIONAL(pAclrtGetDeviceCount_, aclrtGetDeviceCountFunc, rtHandle_, "aclrtGetDeviceCount");
    DL_LOAD_SYM(pAclrtSetDevice_, aclrtSetDeviceFunc, rtHandle_, "aclrtSetDevice");
    DL_LOAD_SYM(pRtDeviceGetBareTgid_, rtDeviceGetBareTgidFunc, rtHandle_, "rtDeviceGetBareTgid");
    DL_LOAD_SYM(pRtGetDeviceInfo_, rtGetDeviceInfoFunc, rtHandle_, "rtGetDeviceInfo");
    DL_LOAD_SYM(pAclrtCreateStream_, aclrtCreateStreamFunc, rtHandle_, "aclrtCreateStream");
    DL_LOAD_SYM(pAclrtCreateStreamWithConfig_, aclrtCreateStreamWithConfigFunc, rtHandle_,
                "aclrtCreateStreamWithConfig");
    DL_LOAD_SYM(pAclrtStreamGetId_, aclrtStreamGetIdFunc, rtHandle_, "aclrtStreamGetId");
    DL_LOAD_SYM_OPTIONAL(pRtStreamGetSqid_, rtStreamGetSqidFunc, rtHandle_, "rtStreamGetSqid");
    DL_LOAD_SYM_OPTIONAL(pRtStreamGetCqid_, rtStreamGetCqidFunc, rtHandle_, "rtStreamGetCqid");
    DL_LOAD_SYM(pAclrtCreateNotify_, aclrtCreateNotifyFunc, rtHandle_, "aclrtCreateNotify");
    DL_LOAD_SYM(pAclrtGetNotifyId_, aclrtGetNotifyIdFunc, rtHandle_, "aclrtGetNotifyId");
    DL_LOAD_SYM(pAclrtDestroyNotify_, aclrtDestroyNotifyFunc, rtHandle_, "aclrtDestroyNotify");
    DL_LOAD_SYM(pAclrtWaitAndResetNotify_, aclrtWaitAndResetNotifyFunc, rtHandle_, "aclrtWaitAndResetNotify");
    DL_LOAD_SYM(pAclrtGetCurrentContext_, aclrtGetCurrentContextFunc, rtHandle_, "aclrtGetCurrentContext");
    DL_LOAD_SYM(pAclrtSetCurrentContext_, aclrtSetCurrentContextFunc, rtHandle_, "aclrtSetCurrentContext");
    DL_LOAD_SYM(pAclrtSetStreamAttribute_, aclrtSetStreamAttributeFunc, rtHandle_, "aclrtSetStreamAttribute");
    DL_LOAD_SYM(pAclrtDestroyStream_, aclrtDestroyStreamFunc, rtHandle_, "aclrtDestroyStream");
    DL_LOAD_SYM(pAclrtSynchronizeStream_, aclrtSynchronizeStreamFunc, rtHandle_, "aclrtSynchronizeStream");
    DL_LOAD_SYM_OPTIONAL(pAclrtStreamAbort_, aclrtStreamAbortFunc, rtHandle_, "aclrtStreamAbort");
    DL_LOAD_SYM(pAclrtMalloc_, aclrtMallocFunc, rtHandle_, "aclrtMalloc");
    DL_LOAD_SYM(pAclrtFree_, aclrtFreeFunc, rtHandle_, "aclrtFree");
    DL_LOAD_SYM(pAclrtMallocHost_, aclrtMallocHostFunc, rtHandle_, "aclrtMallocHost");
    DL_LOAD_SYM(pAclrtFreeHost_, aclrtFreeHostFunc, rtHandle_, "aclrtFreeHost");
    DL_LOAD_SYM(pAclrtMemcpy_, aclrtMemcpyFunc, rtHandle_, "aclrtMemcpy");
    DL_LOAD_SYM(pAclrtMemcpyAsync_, aclrtMemcpyAsyncFunc, rtHandle_, "aclrtMemcpyAsync");
    DL_LOAD_SYM(pAclrtMemset_, aclrtMemsetFunc, rtHandle_, "aclrtMemset");
    DL_LOAD_SYM(pRtIpcSetMemoryName_, rtIpcSetMemoryNameFunc, rtHandle_, "rtIpcSetMemoryName");
    DL_LOAD_SYM(pRtIpcOpenMemory_, rtIpcOpenMemoryFunc, rtHandle_, "rtIpcOpenMemory");
    DL_LOAD_SYM(pRtIpcCloseMemory_, rtIpcCloseMemoryFunc, rtHandle_, "rtIpcCloseMemory");
    DL_LOAD_SYM(pRtIpcDestroyMemoryName_, rtIpcDestroyMemoryNameFunc, rtHandle_, "rtIpcDestroyMemoryName");
    DL_LOAD_SYM(pRtSetIpcMemorySuperPodPid_, rtSetIpcMemorySuperPodPidFunc, rtHandle_, "rtSetIpcMemorySuperPodPid");
    DL_LOAD_SYM(pAclrtGetSocName_, aclrtGetSocNameFunc, rtHandle_, "aclrtGetSocName");
    DL_LOAD_SYM(pRtEnableP2P_, rtEnableP2PFunc, rtHandle_, "rtEnableP2P");
    DL_LOAD_SYM(pRtDisableP2P_, rtDisableP2PFunc, rtHandle_, "rtDisableP2P");
    DL_LOAD_SYM(pRtMemcpyAsync_, rtMemcpyAsyncFunc, rtHandle_, "rtMemcpyAsyncWithoutCheckKind");

    DL_LOAD_SYM_OPTIONAL(pAclrtBinaryLoadFromFile_, aclrtBinaryLoadFromFileFunc, rtHandle_, "aclrtBinaryLoadFromFile");
    DL_LOAD_SYM_OPTIONAL(pAclrtBinaryGetFunction_, aclrtBinaryGetFunctionFunc, rtHandle_, "aclrtBinaryGetFunction");
    DL_LOAD_SYM_OPTIONAL(pAclrtLaunchKernelWithConfig_, aclrtLaunchKernelWithConfigFunc, rtHandle_,
                         "aclrtLaunchKernelWithConfig");
    DL_LOAD_SYM_OPTIONAL(pAclrtKernelArgsInit_, aclrtKernelArgsInitFunc, rtHandle_, "aclrtKernelArgsInit");
    DL_LOAD_SYM_OPTIONAL(pAclrtKernelArgsAppend_, aclrtKernelArgsAppendFunc, rtHandle_, "aclrtKernelArgsAppend");
    DL_LOAD_SYM_OPTIONAL(pAclrtKernelArgsFinalize_, aclrtKernelArgsFinalizeFunc, rtHandle_, "aclrtKernelArgsFinalize");
    DL_LOAD_SYM_OPTIONAL(pAclrtMemcpyBatch_, aclrtMemcpyBatchFunc, rtHandle_, "aclrtMemcpyBatch");
    DL_LOAD_SYM_OPTIONAL(pAclrtGetVersion_, aclrtGetVersionFunc, rtHandle_, "aclrtGetVersion");
    DL_LOAD_SYM_OPTIONAL(pAclsysGetVersionStr_, aclsysGetVersionStrFunc, rtHandle_, "aclsysGetVersionStr");
    DL_LOAD_SYM_OPTIONAL(pAclrtGetPhyDevIdByLogicDevId_, aclrtGetPhyDevIdByLogicDevIdFunc, rtHandle_,
                         "aclrtGetPhyDevIdByLogicDevId");
    DL_LOAD_SYM(pRtGetLogicDevIdByUserDevId_, rtGetLogicDevIdByUserDevIdFunc, rtHandle_, "rtGetLogicDevIdByUserDevId");
    DL_LOAD_SYM_OPTIONAL(pRtGetP2PStatus_, rtGetP2PStatusFunc, rtHandle_, "rtGetP2PStatus");
    DL_LOAD_SYM_OPTIONAL(pRtGetDevResAddress_, rtGetDevResAddressFunc, rtHandle_, "rtGetDevResAddress");

    runtimeHandle_ = dlopen(RUNTIME_LIB_NAME, RTLD_NOW);
    if (runtimeHandle_ != nullptr) {
        DL_LOAD_SYM_OPTIONAL(pRtOpenNetService_, rtOpenNetServiceFunc, runtimeHandle_, "rtOpenNetService");
        DL_LOAD_SYM_OPTIONAL(pRtCloseNetService_, rtCloseNetServiceFunc, runtimeHandle_, "rtCloseNetService");
        DL_LOAD_SYM_OPTIONAL(pRtRDMADBSend_, rtRDMADBSendFunc, runtimeHandle_, "rtRDMADBSend");
    } else {
        BM_LOG_WARN("Failed to open library [" << RUNTIME_LIB_NAME << "], error: " << dlerror());
    }

    loaded_ = true;
    return BM_OK;
}

void AscendApiImpl::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!loaded_) {
        return;
    }

    pAclrtGetDevice_ = nullptr;
    pAclrtGetDeviceCount_ = nullptr;
    pAclrtSetDevice_ = nullptr;
    pRtDeviceGetBareTgid_ = nullptr;
    pRtGetDeviceInfo_ = nullptr;
    pAclrtCreateStream_ = nullptr;
    pAclrtCreateStreamWithConfig_ = nullptr;
    pAclrtStreamGetId_ = nullptr;
    pRtStreamGetSqid_ = nullptr;
    pRtStreamGetCqid_ = nullptr;
    pAclrtSetStreamAttribute_ = nullptr;
    pAclrtDestroyStream_ = nullptr;
    pAclrtSynchronizeStream_ = nullptr;
    pAclrtStreamAbort_ = nullptr;
    pAclrtCreateNotify_ = nullptr;
    pAclrtGetNotifyId_ = nullptr;
    pAclrtDestroyNotify_ = nullptr;
    pAclrtWaitAndResetNotify_ = nullptr;
    pAclrtGetCurrentContext_ = nullptr;
    pAclrtSetCurrentContext_ = nullptr;
    pAclrtBinaryLoadFromFile_ = nullptr;
    pAclrtBinaryGetFunction_ = nullptr;
    pAclrtLaunchKernelWithConfig_ = nullptr;
    pAclrtKernelArgsInit_ = nullptr;
    pAclrtKernelArgsAppend_ = nullptr;
    pAclrtKernelArgsFinalize_ = nullptr;
    pAclrtMalloc_ = nullptr;
    pAclrtFree_ = nullptr;
    pAclrtMallocHost_ = nullptr;
    pAclrtFreeHost_ = nullptr;
    pAclrtMemcpy_ = nullptr;
    pAclrtMemcpyAsync_ = nullptr;
    pRtMemcpyAsync_ = nullptr;
    pAclrtMemcpyBatch_ = nullptr;
    pAclrtMemset_ = nullptr;
    pRtIpcSetMemoryName_ = nullptr;
    pRtIpcOpenMemory_ = nullptr;
    pRtIpcCloseMemory_ = nullptr;
    pRtIpcDestroyMemoryName_ = nullptr;
    pRtSetIpcMemorySuperPodPid_ = nullptr;
    pRtEnableP2P_ = nullptr;
    pRtDisableP2P_ = nullptr;
    pRtGetP2PStatus_ = nullptr;
    pRtGetDevResAddress_ = nullptr;
    pRtOpenNetService_ = nullptr;
    pRtCloseNetService_ = nullptr;
    pRtRDMADBSend_ = nullptr;
    pAclrtGetSocName_ = nullptr;
    pAclrtGetVersion_ = nullptr;
    pAclsysGetVersionStr_ = nullptr;
    pAclrtGetPhyDevIdByLogicDevId_ = nullptr;
    pRtGetLogicDevIdByUserDevId_ = nullptr;

    if (runtimeHandle_ != nullptr) {
        dlclose(runtimeHandle_);
        runtimeHandle_ = nullptr;
    }
    if (rtHandle_ != nullptr) {
        dlclose(rtHandle_);
        rtHandle_ = nullptr;
    }
    loaded_ = false;
}

Result AscendApiImpl::SetDevice(int32_t deviceId, bool force)
{
    if (pAclrtSetDevice_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    if (force) {
        return pAclrtSetDevice_(deviceId);
    }
    int32_t nowDeviceId = -1;
    if (GetDevice(&nowDeviceId) == 0 && nowDeviceId == deviceId) {
        return BM_OK;
    }
    return pAclrtSetDevice_(deviceId);
}

Result AscendApiImpl::GetDevice(int32_t *deviceId)
{
    if (pAclrtGetDevice_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtGetDevice_(deviceId);
}

Result AscendApiImpl::GetDeviceCount(uint32_t *count)
{
    if (pAclrtGetDeviceCount_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtGetDeviceCount_(count);
}

Result AscendApiImpl::GetDeviceInfo(uint32_t deviceId, int32_t moduleType, int32_t infoType, int64_t *val)
{
    if (pRtGetDeviceInfo_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtGetDeviceInfo_(deviceId, moduleType, infoType, val);
}

Result AscendApiImpl::DeviceGetBareTgid(uint32_t *pid)
{
    if (pRtDeviceGetBareTgid_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtDeviceGetBareTgid_(pid);
}

Result AscendApiImpl::CreateStream(void **stream)
{
    if (pAclrtCreateStream_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtCreateStream_(stream);
}

Result AscendApiImpl::CreateStreamWithConfig(void **stream, uint32_t prot, uint32_t config)
{
    if (pAclrtCreateStreamWithConfig_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtCreateStreamWithConfig_(stream, static_cast<int32_t>(prot), config);
}

Result AscendApiImpl::StreamGetId(void *stream, int32_t *streamId)
{
    if (pAclrtStreamGetId_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtStreamGetId_(stream, streamId);
}

Result AscendApiImpl::RtStreamGetSqid(const void *stream, uint32_t *sqId)
{
    if (pRtStreamGetSqid_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtStreamGetSqid_(stream, sqId);
}

Result AscendApiImpl::RtStreamGetCqid(const void *stream, uint32_t *cqId, uint32_t *logicCqId)
{
    if (pRtStreamGetCqid_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtStreamGetCqid_(stream, cqId, logicCqId);
}

Result AscendApiImpl::DestroyStream(void *stream)
{
    if (pAclrtDestroyStream_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtDestroyStream_(stream);
}

Result AscendApiImpl::SynchronizeStream(void *stream)
{
    if (pAclrtSynchronizeStream_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtSynchronizeStream_(stream);
}

Result AscendApiImpl::StreamAbort(void *stream)
{
    if (pAclrtStreamAbort_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtStreamAbort_(stream);
}

Result AscendApiImpl::SetStreamAttribute(void *stream, int32_t attrType, void *attrValue)
{
    if (pAclrtSetStreamAttribute_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtSetStreamAttribute_(stream, static_cast<aclrtStreamAttr>(attrType),
                                     static_cast<aclrtStreamAttrValue *>(attrValue));
}

Result AscendApiImpl::CreateNotify(void **notify, uint64_t flag)
{
    if (pAclrtCreateNotify_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtCreateNotify_(notify, flag);
}

Result AscendApiImpl::GetNotifyId(void *notify, uint32_t *notifyId)
{
    if (pAclrtGetNotifyId_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtGetNotifyId_(notify, notifyId);
}

Result AscendApiImpl::DestroyNotify(void *notify)
{
    if (pAclrtDestroyNotify_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtDestroyNotify_(notify);
}

Result AscendApiImpl::WaitAndResetNotify(void *notify, void *stream, uint32_t timeout)
{
    if (pAclrtWaitAndResetNotify_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtWaitAndResetNotify_(notify, stream, timeout);
}

Result AscendApiImpl::RtGetDevResAddress(void *resInfo, void *addrInfo)
{
    if (pRtGetDevResAddress_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtGetDevResAddress_(static_cast<rtDevResInfo *>(resInfo), static_cast<rtDevResAddrInfo *>(addrInfo));
}

Result AscendApiImpl::OpenNetService(void *args)
{
    if (pRtOpenNetService_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtOpenNetService_(static_cast<const RtNetServiceOpenArgs *>(args));
}

Result AscendApiImpl::CloseNetService()
{
    if (pRtCloseNetService_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtCloseNetService_();
}

Result AscendApiImpl::RDMADBSend(uint32_t dbIndex, uint64_t dbInfo, void *stream)
{
    if (pRtRDMADBSend_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtRDMADBSend_(dbIndex, dbInfo, stream);
}

Result AscendApiImpl::GetCurrentContext(void **context)
{
    if (pAclrtGetCurrentContext_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtGetCurrentContext_(context);
}

Result AscendApiImpl::SetCurrentContext(void *context)
{
    if (pAclrtSetCurrentContext_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtSetCurrentContext_(context);
}

Result AscendApiImpl::BinaryLoadFromFile(const char *binPath, void *options, void **binHandle)
{
    if (pAclrtBinaryLoadFromFile_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtBinaryLoadFromFile_(binPath, static_cast<aclrtBinaryLoadOptions *>(options),
                                     static_cast<aclrtBinHandle *>(binHandle));
}

Result AscendApiImpl::BinaryGetFunction(void *binHandle, const char *kernelName, void **funcHandle)
{
    if (pAclrtBinaryGetFunction_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtBinaryGetFunction_(static_cast<aclrtBinHandle>(binHandle), kernelName,
                                    static_cast<aclrtFuncHandle *>(funcHandle));
}

Result AscendApiImpl::LaunchKernelWithConfig(void *funcHandle, uint32_t blockDim, void *stream, void *cfg,
                                             void *argsHandle, void *reserved)
{
    if (pAclrtLaunchKernelWithConfig_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtLaunchKernelWithConfig_(static_cast<aclrtFuncHandle>(funcHandle), blockDim, stream,
                                         static_cast<aclrtLaunchKernelCfg *>(cfg),
                                         static_cast<aclrtArgsHandle>(argsHandle), reserved);
}

Result AscendApiImpl::KernelArgsInit(void *funcHandle, void **argsHandle)
{
    if (pAclrtKernelArgsInit_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtKernelArgsInit_(static_cast<aclrtFuncHandle>(funcHandle), static_cast<aclrtArgsHandle *>(argsHandle));
}

Result AscendApiImpl::KernelArgsAppend(void *argsHandle, void *param, size_t paramSize, void **paramHandle)
{
    if (pAclrtKernelArgsAppend_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtKernelArgsAppend_(static_cast<aclrtArgsHandle>(argsHandle), param, paramSize,
                                   static_cast<aclrtParamHandle *>(paramHandle));
}

Result AscendApiImpl::KernelArgsFinalize(void *argsHandle)
{
    if (pAclrtKernelArgsFinalize_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtKernelArgsFinalize_(static_cast<aclrtArgsHandle>(argsHandle));
}

Result AscendApiImpl::Malloc(void **ptr, size_t size, uint32_t type)
{
    if (pAclrtMalloc_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtMalloc_(ptr, size, type);
}

Result AscendApiImpl::Free(void *ptr)
{
    if (pAclrtFree_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtFree_(ptr);
}

Result AscendApiImpl::MallocHost(void **ptr, size_t size)
{
    if (pAclrtMallocHost_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtMallocHost_(ptr, size);
}

Result AscendApiImpl::FreeHost(void *ptr)
{
    if (pAclrtFreeHost_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtFreeHost_(ptr);
}

Result AscendApiImpl::Memcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind)
{
    if (pAclrtMemcpy_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtMemcpy_(dst, destMax, src, count, kind);
}

Result AscendApiImpl::MemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind, void *stream)
{
    if (pAclrtMemcpyAsync_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtMemcpyAsync_(dst, destMax, src, count, kind, stream);
}

Result AscendApiImpl::MemcpyAsyncWithoutCheckKind(void *dst, size_t destMax, const void *src, size_t count,
                                                  uint32_t kind, void *stream)
{
    if (pRtMemcpyAsync_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtMemcpyAsync_(dst, destMax, src, count, kind, stream);
}

Result AscendApiImpl::MemcpyBatch(void **dsts, size_t *destMax, void **srcs, size_t *sizes, size_t numBatches,
                                  void *attrs, size_t *attrsIndexes, size_t numAttrs, size_t *failIndex)
{
    if (pAclrtMemcpyBatch_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtMemcpyBatch_(dsts, destMax, srcs, sizes, numBatches, static_cast<aclrtMemcpyBatchAttr *>(attrs),
                              attrsIndexes, numAttrs, failIndex);
}

Result AscendApiImpl::Memset(void *dst, size_t destMax, int32_t value, size_t count)
{
    if (pAclrtMemset_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtMemset_(dst, destMax, value, count);
}

Result AscendApiImpl::IpcSetMemoryName(const void *ptr, uint64_t byteCount, char *name, uint32_t len)
{
    if (pRtIpcSetMemoryName_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtIpcSetMemoryName_(ptr, byteCount, name, len);
}

Result AscendApiImpl::IpcOpenMemory(void **ptr, const char *name)
{
    if (pRtIpcOpenMemory_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtIpcOpenMemory_(ptr, name);
}

Result AscendApiImpl::IpcCloseMemory(const void *ptr)
{
    if (pRtIpcCloseMemory_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtIpcCloseMemory_(ptr);
}

Result AscendApiImpl::IpcDestroyMemoryName(const char *name)
{
    if (pRtIpcDestroyMemoryName_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtIpcDestroyMemoryName_(name);
}

Result AscendApiImpl::IpcSetMemorySuperPodPid(const char *name, uint32_t sdid, int32_t *pid, int32_t num)
{
    if (pRtSetIpcMemorySuperPodPid_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtSetIpcMemorySuperPodPid_(name, sdid, pid, num);
}

Result AscendApiImpl::EnableP2P(uint32_t devIdDes, uint32_t phyIdSrc, uint32_t flag)
{
    if (pRtEnableP2P_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtEnableP2P_(devIdDes, phyIdSrc, flag);
}

Result AscendApiImpl::DisableP2P(uint32_t devIdDes, uint32_t phyIdSrc)
{
    if (pRtDisableP2P_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtDisableP2P_(devIdDes, phyIdSrc);
}

Result AscendApiImpl::GetP2PStatus(uint32_t devId, uint32_t peerDevId, uint32_t &status)
{
    if (pRtGetP2PStatus_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtGetP2PStatus_(devId, peerDevId, &status);
}

AscendSocType AscendApiImpl::GetSocName()
{
    if (pAclrtGetSocName_ == nullptr) {
        return ASCEND_UNKNOWN;
    }
    const char *socName = pAclrtGetSocName_();
    if (socName == nullptr) {
        return ASCEND_UNKNOWN;
    }
    const std::string name{socName};
    if (name.find("Ascend910B") != std::string::npos) {
        return ASCEND_910B;
    }
    if (name.find("Ascend910_93") != std::string::npos) {
        return ASCEND_910C;
    }
    if (name.find("Ascend910_95") != std::string::npos || name.find("Ascend950") != std::string::npos) {
        return ASCEND_950;
    }
    return ASCEND_UNKNOWN;
}

Result AscendApiImpl::GetVersion(int32_t *major, int32_t *minor, int32_t *patch)
{
    if (pAclrtGetVersion_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtGetVersion_(major, minor, patch);
}

Result AscendApiImpl::GetSysVersionStr(char *pkgName, char *versionStr)
{
    if (pAclsysGetVersionStr_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclsysGetVersionStr_(pkgName, versionStr);
}

Result AscendApiImpl::GetPhyDevIdByLogicDevId(int32_t logicDevId, int32_t *phyDevId)
{
    if (pAclrtGetPhyDevIdByLogicDevId_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pAclrtGetPhyDevIdByLogicDevId_(logicDevId, phyDevId);
}

Result AscendApiImpl::GetLogicDevIdByUserDevId(int32_t userDevId, int32_t *logicDevId)
{
    if (pRtGetLogicDevIdByUserDevId_ == nullptr) {
        return BM_UNDER_API_UNLOAD;
    }
    return pRtGetLogicDevIdByUserDevId_(userDevId, logicDevId);
}

} // namespace mf
} // namespace ock
