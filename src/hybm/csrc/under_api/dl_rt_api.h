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

#ifndef MF_HYBM_CORE_DL_RT_API_H
#define MF_HYBM_CORE_DL_RT_API_H

#include <mutex>
#include "hybm_common_include.h"
#include "dl_rt_def.h"

namespace ock {
namespace mf {
using rtStreamGetSqidFunc = int32_t (*)(const void *, uint32_t *);
using rtStreamGetCqidFunc = int32_t (*)(const void *, uint32_t *, uint32_t *);
using rtGetDevResAddressFunc = int32_t (*)(rtDevResInfo *, rtDevResAddrInfo *);
using rtOpenNetServiceFunc = int32_t (*)(const RtNetServiceOpenArgs *);
using rtCloseNetServiceFunc = int32_t (*)();
using rtRDMADBSendFunc = int32_t (*)(uint32_t, uint64_t, void *);
using rtStreamCreateFunc = int32_t (*)(void **, int);
using rtStreamDestroyFunc = int32_t (*)(void *);
using rtStreamSynchronizeFunc = int32_t (*)(void *);

class DlRtApi {
public:
    static Result LoadLibrary();
    static void CleanupLibrary();

    static inline Result RtStreamGetSqid(const void *stm, uint32_t *sqId)
    {
        if (pRtStreamGetSqid == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtStreamGetSqid(stm, sqId);
    }

    static inline Result RtStreamGetCqid(const void *stm, uint32_t *cqId, uint32_t *logicCqId)
    {
        if (pRtStreamGetCqid == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtStreamGetCqid(stm, cqId, logicCqId);
    }

    static inline Result RtGetDevResAddress(rtDevResInfo *resInfo, rtDevResAddrInfo *addrInfo)
    {
        if (pRtGetDevResAddress == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pRtGetDevResAddress(resInfo, addrInfo);
    }

    static inline int32_t RtOpenNetService(const RtNetServiceOpenArgs *args)
    {
        return pRtOpenNetService == nullptr ? -1 : pRtOpenNetService(args);
    }

    static inline int32_t RtCloseNetService()
    {
        return pRtCloseNetService == nullptr ? -1 : pRtCloseNetService();
    }

    static inline int32_t RtRDMADBSend(uint32_t dbIndex, uint64_t dbInfo, void *stream)
    {
        return pRtRDMADBSend == nullptr ? -1 : pRtRDMADBSend(dbIndex, dbInfo, stream);
    }

    static inline int32_t RtStreamCreate(void **stream, int flags = 0)
    {
        return pRtStreamCreate == nullptr ? -1 : pRtStreamCreate(stream, flags);
    }

    static inline int32_t RtStreamDestroy(void *stream)
    {
        return pRtStreamDestroy == nullptr ? -1 : pRtStreamDestroy(stream);
    }

    static inline int32_t RtStreamSynchronize(void *stream)
    {
        return pRtStreamSynchronize == nullptr ? -1 : pRtStreamSynchronize(stream);
    }

private:
    static std::mutex gMutex;
    static bool gLoaded;
    static void *rtHandle;
    static const char *gRtLibName;

    static rtStreamGetSqidFunc pRtStreamGetSqid;
    static rtStreamGetCqidFunc pRtStreamGetCqid;
    static rtGetDevResAddressFunc pRtGetDevResAddress;
    static rtOpenNetServiceFunc pRtOpenNetService;
    static rtCloseNetServiceFunc pRtCloseNetService;
    static rtRDMADBSendFunc pRtRDMADBSend;
    static rtStreamCreateFunc pRtStreamCreate;
    static rtStreamDestroyFunc pRtStreamDestroy;
    static rtStreamSynchronizeFunc pRtStreamSynchronize;
};
} // namespace mf
} // namespace ock

#endif // MF_HYBM_CORE_DL_RT_API_H
