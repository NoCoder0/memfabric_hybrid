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
#ifndef MF_GENETTY_DL_HCCP_API_H
#define MF_GENETTY_DL_HCCP_API_H

#include <netinet/in.h>

#include "under_api/genetty_dl_api.h"
#include "genetty_hccp_queue_types.h"

namespace ock {
namespace openDCC {
/* Keep the layout compatible with the RA initialization configuration in libra.so. */
struct HccpRaInitConfig {
    uint32_t phyId;       /* physical device id */
    uint32_t nicPosition; /* network deployment mode */
    int hdcType;          /* HDC service type */
    bool enableHdcAsync;  /* initialize an extra HDC session for async APIs */
};

union HccpIpAddr {
    struct in_addr addr;
    struct in6_addr addr6;
};

struct HccpRdevInitInfo {
    int mode;            /* network deployment mode */
    uint32_t notifyType; /* notification type */
    bool enabled910aLite;
    bool disabledLiteThread;
    bool enabled2mbLite;
};

struct HccpRdev {
    uint32_t phyId;     /* physical device id */
    int family;         /* AF_INET or AF_INET6 */
    HccpIpAddr localIp; /* local RDMA IP address */
};

constexpr uint32_t HCCP_MAX_INTERFACE_NAME_LEN = 256;

struct HccpRaGetIfAttr {
    uint32_t phyId;       /* physical device id */
    uint32_t nicPosition; /* network deployment mode */
    bool isAll;           /* query all RNIC IP addresses in offline mode */
};

struct HccpIfaddrInfo {
    HccpIpAddr ip;
    struct in_addr mask;
};

struct HccpInterfaceInfo {
    int family;
    int scopeId;
    HccpIfaddrInfo ifaddr;
    char ifname[HCCP_MAX_INTERFACE_NAME_LEN];
};

enum HccpMrAccessFlags {
    HCCP_MR_ACCESS_LOCAL_WRITE = 1 << 0,
    HCCP_MR_ACCESS_REMOTE_WRITE = 1 << 1,
    HCCP_MR_ACCESS_REMOTE_READ = 1 << 2,
};

/* Keep the layout compatible with MrInfoT in the HCCP API. */
struct HccpMrInfo {
    void *addr;              /* starting address of mr */
    unsigned long long size; /* size of mr */
    int access;              /* access of mr */
    unsigned int lkey;       /* local access key */
    unsigned int rkey;       /* remote access key */
};

using RaDeregisterMr_api = int (*)(const void *rdmaHandle, void *mrHandle);
using RaRegisterMr_api = int (*)(const void *rdmaHandle, HccpMrInfo *info, void **mrHandle);
using RaInit_api = int (*)(HccpRaInitConfig *config);
using RaDeinit_api = int (*)(HccpRaInitConfig *config);
using RaRdevInitV2_api = int (*)(HccpRdevInitInfo initInfo, HccpRdev rdevInfo, void **rdmaHandle);
using RaRdevDeinit_api = int (*)(void *rdmaHandle, uint32_t notifyType);
using RaGetIfNum_api = int (*)(HccpRaGetIfAttr *config, uint32_t *num);
using RaGetIfAddrs_api = int (*)(HccpRaGetIfAttr *config, HccpInterfaceInfo infos[], uint32_t *num);

using RaQpCreateWithAttrs_api = int (*)(void *rdmaHandle, HccpQpExtAttrs *attrs, void **qpHandle);
using RaQpDestroy_api = int (*)(void *qpHandle);
using RaGetQpAttr_api = int (*)(void *qpHandle, HccpQpAttr *attr);
using RaTypicalQpModify_api = int (*)(void *qpHandle, HccpTypicalQp *local, HccpTypicalQp *remote);
using RaSendWrV2_api = int (*)(void *qpHandle, HccpSendWrV2 *wr, HccpSendWrRsp *rsp);
using RaPollCq_api = int (*)(void *qpHandle, bool send, unsigned int count, void *wc);

// TODO: Consolidate these RA wrappers with MemFabric's src/hybm/csrc/under_api/dl_hccp_api.h.
class HccpApi {
public:
    static Result Load() noexcept;
    static void UnLoad() noexcept;

    static int RaInit(const HccpRaInitConfig &config)
    {
        GEN_ASSERT_MSG_RETURN(RaInit_ptr != nullptr, "RA init API is not loaded", GEN_NOT_INITIALIZED);
        auto mutableConfig = config;
        auto result = RaInit_ptr(&mutableConfig);
        GEN_ASSERT_MSG_RETURN(result == 0,
                              "RaInit failed, result(" << result << "), phyId(" << config.phyId << "), nicPosition("
                                                       << config.nicPosition << "), hdcType(" << config.hdcType
                                                       << "), enableHdcAsync(" << config.enableHdcAsync << ")",
                              result);
        return result;
    }

    static int RaDeinit(const HccpRaInitConfig &config)
    {
        GEN_ASSERT_MSG_RETURN(RaDeinit_ptr != nullptr, "RA deinit API is not loaded", GEN_NOT_INITIALIZED);
        auto mutableConfig = config;
        auto result = RaDeinit_ptr(&mutableConfig);
        GEN_ASSERT_MSG_RETURN(result == 0,
                              "RaDeinit failed, result(" << result << "), phyId(" << config.phyId << "), nicPosition("
                                                         << config.nicPosition << "), hdcType(" << config.hdcType
                                                         << "), enableHdcAsync(" << config.enableHdcAsync << ")",
                              result);
        return result;
    }

    static int RaRdevInitV2(const HccpRdevInitInfo &initInfo, const HccpRdev &rdevInfo, void *&rdmaHandle)
    {
        GEN_ASSERT_MSG_RETURN(RaRdevInitV2_ptr != nullptr, "RA rdev init API is not loaded", GEN_NOT_INITIALIZED);
        auto result = RaRdevInitV2_ptr(initInfo, rdevInfo, &rdmaHandle);
        GEN_ASSERT_MSG_RETURN(result == 0,
                              "RaRdevInitV2 failed, result(" << result << "), phyId(" << rdevInfo.phyId << "), family("
                                                             << rdevInfo.family << "), mode(" << initInfo.mode
                                                             << "), notifyType(" << initInfo.notifyType << ")",
                              result);
        return result;
    }

    static int RaGetIfNum(const HccpRaGetIfAttr &config, uint32_t &num)
    {
        GEN_ASSERT_MSG_RETURN(RaGetIfNum_ptr != nullptr, "RA get interface count API is not loaded",
                              GEN_NOT_INITIALIZED);
        auto mutableConfig = config;
        auto result = RaGetIfNum_ptr(&mutableConfig, &num);
        GEN_ASSERT_MSG_RETURN(result == 0, "RaGetIfNum failed, result(" << result << "), phyId(" << config.phyId << ")",
                              result);
        return result;
    }

    static int RaGetIfAddrs(const HccpRaGetIfAttr &config, HccpInterfaceInfo infos[], uint32_t &num)
    {
        GEN_ASSERT_MSG_RETURN(RaGetIfAddrs_ptr != nullptr, "RA get interface addresses API is not loaded",
                              GEN_NOT_INITIALIZED);
        auto mutableConfig = config;
        auto result = RaGetIfAddrs_ptr(&mutableConfig, infos, &num);
        GEN_ASSERT_MSG_RETURN(result == 0,
                              "RaGetIfAddrs failed, result(" << result << "), phyId(" << config.phyId << ")", result);
        return result;
    }

    static int RaRegisterMr(const void *rdmaHandle, HccpMrInfo &info, void *&mrHandle)
    {
        GEN_ASSERT_MSG_RETURN(RaRegisterMr_ptr != nullptr, "RA register MR API is not loaded", GEN_NOT_INITIALIZED);
        auto result = RaRegisterMr_ptr(rdmaHandle, &info, &mrHandle);
        GEN_ASSERT_MSG_RETURN(result == 0,
                              "RaRegisterMr failed, result(" << result << "), rdmaHandle(" << rdmaHandle
                                                             << "), address(" << info.addr << "), size(" << info.size
                                                             << ")",
                              result);
        return result;
    }

    static int RaDeregisterMr(const void *rdmaHandle, void *mrHandle)
    {
        GEN_ASSERT_MSG_RETURN(RaDeregisterMr_ptr != nullptr, "RA deregister MR API is not loaded", GEN_NOT_INITIALIZED);
        auto result = RaDeregisterMr_ptr(rdmaHandle, mrHandle);
        GEN_ASSERT_MSG_RETURN(result == 0,
                              "RaDeregisterMr failed, result(" << result << "), rdmaHandle(" << rdmaHandle
                                                               << "), mrHandle(" << mrHandle << ")",
                              result);
        return result;
    }

    static int RaQpCreateWithAttrs(void *rdmaHandle, HccpQpExtAttrs *attrs, void **qpHandle)
    {
        GEN_ASSERT_MSG_RETURN(RaQpCreateWithAttrs_ptr != nullptr, "RaQpCreateWithAttrs API is not loaded",
                              GEN_NOT_INITIALIZED);
        auto result = RaQpCreateWithAttrs_ptr(rdmaHandle, attrs, qpHandle);
        GEN_ASSERT_MSG_RETURN(result == 0, "RaQpCreateWithAttrs failed, result(" << result << ")", result);
        return result;
    }

    static int RaQpDestroy(void *&qpHandle)
    {
        GEN_ASSERT_MSG_RETURN(RaQpDestroy_ptr != nullptr, "RaQpDestroy API is not loaded", GEN_NOT_INITIALIZED);
        auto handle = qpHandle;
        qpHandle = nullptr;
        auto result = RaQpDestroy_ptr(handle);
        GEN_ASSERT_MSG_RETURN(result == 0, "RaQpDestroy failed, result(" << result << ")", result);
        return result;
    }

    static int RaGetQpAttr(void *qpHandle, HccpQpAttr *attr)
    {
        GEN_ASSERT_MSG_RETURN(RaGetQpAttr_ptr != nullptr, "RaGetQpAttr API is not loaded", GEN_NOT_INITIALIZED);
        auto result = RaGetQpAttr_ptr(qpHandle, attr);
        GEN_ASSERT_MSG_RETURN(result == 0, "RaGetQpAttr failed, result(" << result << ")", result);
        return result;
    }

    static int RaTypicalQpModify(void *qpHandle, HccpTypicalQp *local, HccpTypicalQp *remote)
    {
        GEN_ASSERT_MSG_RETURN(RaTypicalQpModify_ptr != nullptr, "RaTypicalQpModify API is not loaded",
                              GEN_NOT_INITIALIZED);
        auto result = RaTypicalQpModify_ptr(qpHandle, local, remote);
        GEN_ASSERT_MSG_RETURN(result == 0, "RaTypicalQpModify failed, result(" << result << ")", result);
        return result;
    }

    static int RaSendWrV2(void *qpHandle, HccpSendWrV2 *wr, HccpSendWrRsp *rsp)
    {
        GEN_ASSERT_MSG_RETURN(RaSendWrV2_ptr != nullptr, "RaSendWrV2 API is not loaded", GEN_NOT_INITIALIZED);
        auto result = RaSendWrV2_ptr(qpHandle, wr, rsp);
        GEN_ASSERT_MSG_RETURN(result == 0, "RaSendWrV2 failed, result(" << result << ")", result);
        return result;
    }

    static int RaPollCq(void *qpHandle, bool send, unsigned int count, void *wc)
    {
        GEN_ASSERT_MSG_RETURN(RaPollCq_ptr != nullptr, "RaPollCq API is not loaded", GEN_NOT_INITIALIZED);
        auto result = RaPollCq_ptr(qpHandle, send, count, wc);
        GEN_ASSERT_MSG_RETURN(result >= 0, "RaPollCq failed, result(" << result << ")", result);
        return result;
    }

    /* Once called, the RA API can consume the handle even when it returns an error. */
    static int RaRdevDeinit(void *&rdmaHandle, uint32_t notifyType)
    {
        GEN_ASSERT_MSG_RETURN(RaRdevDeinit_ptr != nullptr, "RA rdev deinit API is not loaded", GEN_NOT_INITIALIZED);
        auto handle = rdmaHandle;
        rdmaHandle = nullptr;
        auto result = RaRdevDeinit_ptr(handle, notifyType);
        GEN_ASSERT_MSG_RETURN(result == 0,
                              "RaRdevDeinit failed, result(" << result << "), rdmaHandle(" << handle << "), notifyType("
                                                             << notifyType << ")",
                              result);
        return result;
    }

private:
    static Result LoadInner(void *handle) noexcept;
    static Result LoadInterfaceApis(void *handle) noexcept;
    static Result LoadMemoryApis(void *handle) noexcept;
    static Result LoadQueueApis(void *handle) noexcept;
    static void UnLoadInner() noexcept;

    DL_API_DECLARE(RaRegisterMr);
    DL_API_DECLARE(RaDeregisterMr);
    DL_API_DECLARE(RaQpCreateWithAttrs);
    DL_API_DECLARE(RaQpDestroy);
    DL_API_DECLARE(RaGetQpAttr);
    DL_API_DECLARE(RaTypicalQpModify);
    DL_API_DECLARE(RaSendWrV2);
    DL_API_DECLARE(RaPollCq);
    DL_API_DECLARE(RaInit);
    DL_API_DECLARE(RaDeinit);
    DL_API_DECLARE(RaRdevInitV2);
    DL_API_DECLARE(RaRdevDeinit);
    DL_API_DECLARE(RaGetIfNum);
    DL_API_DECLARE(RaGetIfAddrs);

    static std::mutex LOAD_MUTEX;
    static bool LOADED;
};
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_DL_HCCP_API_H
