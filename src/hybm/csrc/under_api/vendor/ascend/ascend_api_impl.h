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
/*
 * Related upstream references; the original source is not established.
 * https://gitcode.com/cann/runtime/blob/v8.5.0/include/external/acl/acl_rt.h
 * https://gitcode.com/cann/runtime/blob/v8.5.0/pkg_inc/runtime/rt_external_device.h
 * Upstream copyright:
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * Compared with references: Selected ACL and runtime device resource types and added dynamic wrappers.
 * Upstream copyright and license notices: Open_Source_Software_Notice.md
 */

#ifndef MF_HYBM_CORE_ASCEND_API_IMPL_H
#define MF_HYBM_CORE_ASCEND_API_IMPL_H

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "hybm_common_include.h"
#include "../vendor_api_impl.h"
#include "acl_rt_def.h"

struct RtProcExtParam {
    const char *paramInfo;
    uint64_t paramLen;
};

struct RtNetServiceOpenArgs {
    RtProcExtParam *extParamList;
    uint64_t extParamCnt;
};

namespace ock {
namespace mf {

typedef enum {
    RT_PROCESS_CP1 = 0,
    RT_PROCESS_CP2,
    RT_PROCESS_DEV_ONLY,
    RT_PROCESS_QS,
    RT_PROCESS_HCCP,
    RT_PROCESS_USER,
    RT_PROCESS_CPTYPE_MAX
} rtDevResProcType_t;

typedef enum {
    RT_RES_TYPE_STARS_NOTIFY_RECORD = 0,
    RT_RES_TYPE_STARS_CNT_NOTIFY_RECORD,
    RT_RES_TYPE_STARS_RTSQ,
    RT_RES_TYPE_CCU_CKE,
    RT_RES_TYPE_CCU_XN,
    RT_RES_TYPE_STARS_CNT_NOTIFY_BIT_WR,
    RT_RES_TYPE_STARS_CNT_NOTIFY_ADD,
    RT_RES_TYPE_STARS_CNT_NOTIFY_BIT_CLR,
    RT_RES_TYPE_MAX
} rtDevResType_t;

typedef struct {
    uint32_t dieId;
    rtDevResProcType_t procType;
    rtDevResType_t resType;
    uint32_t resId;
    uint32_t flag;
} rtDevResInfo;

typedef struct {
    uint64_t *resAddress;
    uint32_t *len;
} rtDevResAddrInfo;

using aclrtSetDeviceFunc = int32_t (*)(int32_t);
using aclrtGetDeviceFunc = int32_t (*)(int32_t *);
using aclrtGetDeviceCountFunc = int32_t (*)(uint32_t *);
using rtDeviceGetBareTgidFunc = int32_t (*)(uint32_t *);
using rtGetDeviceInfoFunc = int32_t (*)(uint32_t, int32_t, int32_t, int64_t *val);
using aclrtCreateStreamFunc = int (*)(void **);
using aclrtCreateStreamWithConfigFunc = int (*)(void **, int32_t, uint32_t);
using aclrtStreamGetIdFunc = int (*)(void *, int32_t *);
using rtStreamGetSqidFunc = int32_t (*)(const void *, uint32_t *);
using rtStreamGetCqidFunc = int32_t (*)(const void *, uint32_t *, uint32_t *);
using aclrtSetStreamAttributeFunc = int (*)(void *, aclrtStreamAttr, aclrtStreamAttrValue *);
using aclrtDestroyStreamFunc = int (*)(void *);
using aclrtSynchronizeStreamFunc = int (*)(void *);
using aclrtStreamAbortFunc = int (*)(void *);
using aclrtCreateNotifyFunc = int (*)(void **, uint64_t);
using aclrtGetNotifyIdFunc = int (*)(void *, uint32_t *);
using aclrtDestroyNotifyFunc = int (*)(void *);
using aclrtWaitAndResetNotifyFunc = int (*)(void *, void *, uint32_t);
using aclrtGetCurrentContextFunc = int (*)(void **);
using aclrtSetCurrentContextFunc = int (*)(void *);
using aclrtBinaryLoadFromFileFunc = int32_t (*)(const char *, aclrtBinaryLoadOptions *, aclrtBinHandle *);
using aclrtBinaryGetFunctionFunc = int32_t (*)(const aclrtBinHandle, const char *, aclrtFuncHandle *);
using aclrtLaunchKernelWithConfigFunc = int32_t (*)(aclrtFuncHandle, uint32_t, void *, aclrtLaunchKernelCfg *,
                                                    aclrtArgsHandle, void *);
using aclrtKernelArgsInitFunc = int32_t (*)(aclrtFuncHandle, aclrtArgsHandle *);
using aclrtKernelArgsAppendFunc = int32_t (*)(aclrtArgsHandle, void *, size_t, aclrtParamHandle *);
using aclrtKernelArgsFinalizeFunc = int32_t (*)(aclrtArgsHandle);
using aclrtMallocFunc = int32_t (*)(void **, size_t, uint32_t);
using aclrtFreeFunc = int (*)(void *);
using aclrtMallocHostFunc = int32_t (*)(void **, size_t);
using aclrtFreeHostFunc = int (*)(void *);
using aclrtMemcpyFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t);
using aclrtMemcpyAsyncFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t, void *);
using rtMemcpyAsyncFunc = int32_t (*)(void *, size_t, const void *, size_t, uint32_t, void *);
using aclrtMemcpyBatchFunc = int32_t (*)(void **, size_t *, void **, size_t *, size_t, aclrtMemcpyBatchAttr *, size_t *,
                                         size_t, size_t *);
using aclrtMemsetFunc = int32_t (*)(void *, size_t, int32_t, size_t);
using rtIpcSetMemoryNameFunc = int32_t (*)(const void *, uint64_t, char *, uint32_t);
using rtIpcOpenMemoryFunc = int32_t (*)(void **, const char *);
using rtIpcCloseMemoryFunc = int32_t (*)(const void *);
using rtIpcDestroyMemoryNameFunc = int32_t (*)(const char *);
using rtSetIpcMemorySuperPodPidFunc = int32_t (*)(const char *, uint32_t, int32_t *, int32_t);
using rtEnableP2PFunc = int32_t (*)(uint32_t, uint32_t, uint32_t);
using rtDisableP2PFunc = int32_t (*)(uint32_t, uint32_t);
using rtGetP2PStatusFunc = int32_t (*)(uint32_t, uint32_t, uint32_t *);
using rtGetDevResAddressFunc = int32_t (*)(rtDevResInfo *, rtDevResAddrInfo *);
using rtOpenNetServiceFunc = int32_t (*)(const RtNetServiceOpenArgs *);
using rtCloseNetServiceFunc = int32_t (*)();
using rtRDMADBSendFunc = int32_t (*)(uint32_t, uint64_t, void *);
using aclrtGetSocNameFunc = const char *(*)();
using aclrtGetVersionFunc = int32_t (*)(int32_t *, int32_t *, int32_t *);
using aclsysGetVersionStrFunc = int32_t (*)(char *, char *);
using aclrtGetPhyDevIdByLogicDevIdFunc = int32_t (*)(const int32_t, int32_t *const);
using rtGetLogicDevIdByUserDevIdFunc = int32_t (*)(const int32_t, int32_t *const);

class AscendApiImpl : public VendorApiImpl {
public:
    ~AscendApiImpl() override = default;

    Result LoadLibrary() override;
    void CleanupLibrary() override;

    Result SetDevice(int32_t deviceId, bool force = false) override;
    Result GetDevice(int32_t *deviceId) override;
    Result GetDeviceCount(uint32_t *count) override;
    Result GetDeviceInfo(uint32_t deviceId, int32_t moduleType, int32_t infoType, int64_t *val) override;
    Result DeviceGetBareTgid(uint32_t *pid) override;

    Result CreateStream(void **stream) override;
    Result CreateStreamWithConfig(void **stream, uint32_t prot, uint32_t config) override;
    Result StreamGetId(void *stream, int32_t *streamId) override;
    Result RtStreamGetSqid(const void *stream, uint32_t *sqId) override;
    Result RtStreamGetCqid(const void *stream, uint32_t *cqId, uint32_t *logicCqId) override;
    Result DestroyStream(void *stream) override;
    Result SynchronizeStream(void *stream) override;
    Result StreamAbort(void *stream) override;
    Result SetStreamAttribute(void *stream, int32_t attrType, void *attrValue) override;

    Result CreateNotify(void **notify, uint64_t flag) override;
    Result GetNotifyId(void *notify, uint32_t *notifyId) override;
    Result DestroyNotify(void *notify) override;
    Result WaitAndResetNotify(void *notify, void *stream, uint32_t timeout) override;
    Result RtGetDevResAddress(void *resInfo, void *addrInfo) override;
    Result OpenNetService(void *args) override;
    Result CloseNetService() override;
    Result RDMADBSend(uint32_t dbIndex, uint64_t dbInfo, void *stream) override;

    Result GetCurrentContext(void **context) override;
    Result SetCurrentContext(void *context) override;

    Result BinaryLoadFromFile(const char *binPath, void *options, void **binHandle) override;
    Result BinaryGetFunction(void *binHandle, const char *kernelName, void **funcHandle) override;
    Result LaunchKernelWithConfig(void *funcHandle, uint32_t blockDim, void *stream, void *cfg, void *argsHandle,
                                  void *reserved) override;
    Result KernelArgsInit(void *funcHandle, void **argsHandle) override;
    Result KernelArgsAppend(void *argsHandle, void *param, size_t paramSize, void **paramHandle) override;
    Result KernelArgsFinalize(void *argsHandle) override;

    Result Malloc(void **ptr, size_t size, uint32_t type) override;
    Result Free(void *ptr) override;
    Result MallocHost(void **ptr, size_t size) override;
    Result FreeHost(void *ptr) override;
    Result Memcpy(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind) override;
    Result MemcpyAsync(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind, void *stream) override;
    Result MemcpyAsyncWithoutCheckKind(void *dst, size_t destMax, const void *src, size_t count, uint32_t kind,
                                       void *stream) override;
    Result MemcpyBatch(void **dsts, size_t *destMax, void **srcs, size_t *sizes, size_t numBatches, void *attrs,
                       size_t *attrsIndexes, size_t numAttrs, size_t *failIndex) override;
    Result Memset(void *dst, size_t destMax, int32_t value, size_t count) override;

    Result IpcSetMemoryName(const void *ptr, uint64_t byteCount, char *name, uint32_t len) override;
    Result IpcOpenMemory(void **ptr, const char *name) override;
    Result IpcCloseMemory(const void *ptr) override;
    Result IpcDestroyMemoryName(const char *name) override;
    Result IpcSetMemorySuperPodPid(const char *name, uint32_t sdid, int32_t *pid, int32_t num) override;

    Result EnableP2P(uint32_t devIdDes, uint32_t phyIdSrc, uint32_t flag) override;
    Result DisableP2P(uint32_t devIdDes, uint32_t phyIdSrc) override;
    Result GetP2PStatus(uint32_t devId, uint32_t peerDevId, uint32_t &status) override;

    AscendSocType GetSocName() override;
    Result GetVersion(int32_t *major, int32_t *minor, int32_t *patch) override;
    Result GetSysVersionStr(char *pkgName, char *versionStr) override;
    Result GetPhyDevIdByLogicDevId(int32_t logicDevId, int32_t *phyDevId) override;
    Result GetLogicDevIdByUserDevId(int32_t userDevId, int32_t *logicDevId) override;

private:
    std::mutex mutex_;
    bool loaded_ = false;
    void *rtHandle_ = nullptr;
    void *runtimeHandle_ = nullptr;

    aclrtSetDeviceFunc pAclrtSetDevice_ = nullptr;
    aclrtGetDeviceFunc pAclrtGetDevice_ = nullptr;
    aclrtGetDeviceCountFunc pAclrtGetDeviceCount_ = nullptr;
    rtDeviceGetBareTgidFunc pRtDeviceGetBareTgid_ = nullptr;
    rtGetDeviceInfoFunc pRtGetDeviceInfo_ = nullptr;
    aclrtCreateStreamFunc pAclrtCreateStream_ = nullptr;
    aclrtCreateStreamWithConfigFunc pAclrtCreateStreamWithConfig_ = nullptr;
    aclrtStreamGetIdFunc pAclrtStreamGetId_ = nullptr;
    rtStreamGetSqidFunc pRtStreamGetSqid_ = nullptr;
    rtStreamGetCqidFunc pRtStreamGetCqid_ = nullptr;
    aclrtSetStreamAttributeFunc pAclrtSetStreamAttribute_ = nullptr;
    aclrtDestroyStreamFunc pAclrtDestroyStream_ = nullptr;
    aclrtSynchronizeStreamFunc pAclrtSynchronizeStream_ = nullptr;
    aclrtStreamAbortFunc pAclrtStreamAbort_ = nullptr;
    aclrtCreateNotifyFunc pAclrtCreateNotify_ = nullptr;
    aclrtGetNotifyIdFunc pAclrtGetNotifyId_ = nullptr;
    aclrtDestroyNotifyFunc pAclrtDestroyNotify_ = nullptr;
    aclrtWaitAndResetNotifyFunc pAclrtWaitAndResetNotify_ = nullptr;
    aclrtGetCurrentContextFunc pAclrtGetCurrentContext_ = nullptr;
    aclrtSetCurrentContextFunc pAclrtSetCurrentContext_ = nullptr;
    aclrtBinaryLoadFromFileFunc pAclrtBinaryLoadFromFile_ = nullptr;
    aclrtBinaryGetFunctionFunc pAclrtBinaryGetFunction_ = nullptr;
    aclrtLaunchKernelWithConfigFunc pAclrtLaunchKernelWithConfig_ = nullptr;
    aclrtKernelArgsInitFunc pAclrtKernelArgsInit_ = nullptr;
    aclrtKernelArgsAppendFunc pAclrtKernelArgsAppend_ = nullptr;
    aclrtKernelArgsFinalizeFunc pAclrtKernelArgsFinalize_ = nullptr;
    aclrtMallocFunc pAclrtMalloc_ = nullptr;
    aclrtFreeFunc pAclrtFree_ = nullptr;
    aclrtMallocHostFunc pAclrtMallocHost_ = nullptr;
    aclrtFreeHostFunc pAclrtFreeHost_ = nullptr;
    aclrtMemcpyFunc pAclrtMemcpy_ = nullptr;
    aclrtMemcpyAsyncFunc pAclrtMemcpyAsync_ = nullptr;
    rtMemcpyAsyncFunc pRtMemcpyAsync_ = nullptr;
    aclrtMemcpyBatchFunc pAclrtMemcpyBatch_ = nullptr;
    aclrtMemsetFunc pAclrtMemset_ = nullptr;
    rtIpcSetMemoryNameFunc pRtIpcSetMemoryName_ = nullptr;
    rtIpcOpenMemoryFunc pRtIpcOpenMemory_ = nullptr;
    rtIpcCloseMemoryFunc pRtIpcCloseMemory_ = nullptr;
    rtIpcDestroyMemoryNameFunc pRtIpcDestroyMemoryName_ = nullptr;
    rtSetIpcMemorySuperPodPidFunc pRtSetIpcMemorySuperPodPid_ = nullptr;
    rtEnableP2PFunc pRtEnableP2P_ = nullptr;
    rtDisableP2PFunc pRtDisableP2P_ = nullptr;
    rtGetP2PStatusFunc pRtGetP2PStatus_ = nullptr;
    rtGetDevResAddressFunc pRtGetDevResAddress_ = nullptr;
    rtOpenNetServiceFunc pRtOpenNetService_ = nullptr;
    rtCloseNetServiceFunc pRtCloseNetService_ = nullptr;
    rtRDMADBSendFunc pRtRDMADBSend_ = nullptr;
    aclrtGetSocNameFunc pAclrtGetSocName_ = nullptr;
    aclrtGetVersionFunc pAclrtGetVersion_ = nullptr;
    aclsysGetVersionStrFunc pAclsysGetVersionStr_ = nullptr;
    aclrtGetPhyDevIdByLogicDevIdFunc pAclrtGetPhyDevIdByLogicDevId_ = nullptr;
    rtGetLogicDevIdByUserDevIdFunc pRtGetLogicDevIdByUserDevId_ = nullptr;
};

} // namespace mf
} // namespace ock
#endif // MF_HYBM_CORE_ASCEND_API_IMPL_H
