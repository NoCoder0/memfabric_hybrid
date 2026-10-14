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

#include <cstdio>
#include <dlfcn.h>

#include "dl_acl_api.h"
#include "dl_hccp_api.h"

namespace ock {
namespace mf {
namespace {
constexpr uint32_t CANN_LAYOUT_V2_MAJOR = 9;
constexpr uint32_t CANN_LAYOUT_V2_MINOR = 2;
} // namespace

bool DlHccpApi::gLoaded = false;
std::mutex DlHccpApi::gMutex;
void *DlHccpApi::raHandle;
void *DlHccpApi::tsdHandle;

const char *DlHccpApi::gRaLibName = "libra.so";
const char *DlHccpApi::gTsdLibName = "libtsdclient.so";

raRdevGetHandleFunc DlHccpApi::gRaRdevGetHandle;
raInitFunc DlHccpApi::gRaInit;
raSocketInitFunc DlHccpApi::gRaSocketInit;
raSocketDeinitFunc DlHccpApi::gRaSocketDeinit;
raRdevInitV2Func DlHccpApi::gRaRdevInitV2;
raSocketBatchConnectFunc DlHccpApi::gRaSocketBatchConnect;
raSocketBatchCloseFunc DlHccpApi::gRaSocketBatchClose;
raSocketListenStartFunc DlHccpApi::gRaSocketListenStart;
raSocketListenStopFunc DlHccpApi::gRaSocketListenStop;
raGetSocketsFunc DlHccpApi::gRaGetSockets;
raGetIfNumFunc DlHccpApi::gRaGetIfNum;
raGetIfAddrsFunc DlHccpApi::gRaGetIfAddrs;
raSocketWhiteListAddFunc DlHccpApi::gRaSocketWhiteListAdd;
raSocketWhiteListDelFunc DlHccpApi::gRaSocketWhiteListDel;
raQpCreateFunc DlHccpApi::gRaQpCreate;
raQpCreateWithAttrsFunc DlHccpApi::gRaQpCreateWithAttrs;
raQpAiCreateFunc DlHccpApi::gRaQpAiCreate;
raQpDestroyFunc DlHccpApi::gRaQpDestroy;
raGetQpStatusFunc DlHccpApi::gRaGetQpStatus;
raQpConnectAsyncFunc DlHccpApi::gRaQpConnectAsync;
raRegisterMrFunc DlHccpApi::gRaRegisterMR;
raDeregisterMrFunc DlHccpApi::gRaDeregisterMR;
raSendWrFunc DlHccpApi::gRaSendWr;
raSendWrV2Func DlHccpApi::gRaSendWrV2;
raGetNotifyBaseAddrFunc DlHccpApi::gRaGetNotifyBaseAddr;
raGetNotifyMrInfoFunc DlHccpApi::gRaGetNotifyMrInfo;
raSetQpAttrQosFunc DlHccpApi::gRaSetQpAttrQos;

tsdOpenFunc DlHccpApi::gTsdOpen;

Result DlHccpApi::LoadLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return BM_OK;
    }

    raHandle = dlopen(gRaLibName, RTLD_NOW | RTLD_NODELETE);
    if (raHandle == nullptr) {
        BM_LOG_ERROR(
            "Failed to open library ["
            << gRaLibName
            << "], please source ascend-toolkit set_env.sh, or add ascend driver lib path into LD_LIBRARY_PATH,"
            << " error: " << dlerror());
        return BM_DL_FUNCTION_FAILED;
    }

    tsdHandle = dlopen(gTsdLibName, RTLD_NOW | RTLD_NODELETE);
    if (tsdHandle == nullptr) {
        BM_LOG_ERROR(
            "Failed to open library ["
            << gTsdLibName
            << "], please source ascend-toolkit set_env.sh, or add ascend driver lib path into LD_LIBRARY_PATH,"
            << " error: " << dlerror());
        dlclose(raHandle);
        raHandle = nullptr;
        return BM_DL_FUNCTION_FAILED;
    }

    /* load sym */
    DL_LOAD_SYM_ALT(gRaSocketInit, raSocketInitFunc, raHandle, "ra_socket_init", "RaSocketInit");
    DL_LOAD_SYM_ALT(gRaInit, raInitFunc, raHandle, "ra_init", "RaInit");
    DL_LOAD_SYM_ALT(gRaSocketDeinit, raSocketDeinitFunc, raHandle, "ra_socket_deinit", "RaSocketDeinit");
    DL_LOAD_SYM_ALT(gRaRdevInitV2, raRdevInitV2Func, raHandle, "ra_rdev_init_v2", "RaRdevInitV2");
    DL_LOAD_SYM_ALT(gRaRdevGetHandle, raRdevGetHandleFunc, raHandle, "ra_rdev_get_handle", "RaRdevGetHandle");
    DL_LOAD_SYM_ALT(gRaSocketBatchConnect, raSocketBatchConnectFunc, raHandle, "ra_socket_batch_connect",
                    "RaSocketBatchConnect");
    DL_LOAD_SYM_ALT(gRaSocketBatchClose, raSocketBatchCloseFunc, raHandle, "ra_socket_batch_close",
                    "RaSocketBatchClose");
    DL_LOAD_SYM_ALT(gRaSocketListenStart, raSocketListenStartFunc, raHandle, "ra_socket_listen_start",
                    "RaSocketListenStart");
    DL_LOAD_SYM_ALT(gRaSocketListenStop, raSocketListenStopFunc, raHandle, "ra_socket_listen_stop",
                    "RaSocketListenStop");
    DL_LOAD_SYM_ALT(gRaGetSockets, raGetSocketsFunc, raHandle, "ra_get_sockets", "RaGetSockets");
    DL_LOAD_SYM_ALT(gRaGetIfNum, raGetIfNumFunc, raHandle, "ra_get_ifnum", "RaGetIfnum");
    DL_LOAD_SYM_ALT(gRaGetIfAddrs, raGetIfAddrsFunc, raHandle, "ra_get_ifaddrs", "RaGetIfaddrs");
    DL_LOAD_SYM_ALT(gRaSocketWhiteListAdd, raSocketWhiteListAddFunc, raHandle, "ra_socket_white_list_add",
                    "RaSocketWhiteListAdd");
    DL_LOAD_SYM_ALT(gRaSocketWhiteListDel, raSocketWhiteListDelFunc, raHandle, "ra_socket_white_list_del",
                    "RaSocketWhiteListDel");
    DL_LOAD_SYM_ALT(gRaQpCreate, raQpCreateFunc, raHandle, "ra_qp_create", "RaQpCreate");
    DL_LOAD_SYM_ALT(gRaQpCreateWithAttrs, raQpCreateWithAttrsFunc, raHandle, "ra_qp_create_with_attrs",
                    "RaQpCreateWithAttrs");
    DL_LOAD_SYM_ALT(gRaQpAiCreate, raQpAiCreateFunc, raHandle, "ra_ai_qp_create", "RaAiQpCreate");
    DL_LOAD_SYM_ALT(gRaQpDestroy, raQpDestroyFunc, raHandle, "ra_qp_destroy", "RaQpDestroy");
    DL_LOAD_SYM_ALT(gRaGetQpStatus, raGetQpStatusFunc, raHandle, "ra_get_qp_status", "RaGetQpStatus");
    DL_LOAD_SYM_ALT(gRaQpConnectAsync, raQpConnectAsyncFunc, raHandle, "ra_qp_connect_async", "RaQpConnectAsync");
    DL_LOAD_SYM_ALT(gRaRegisterMR, raRegisterMrFunc, raHandle, "ra_register_mr", "RaRegisterMr");
    DL_LOAD_SYM_ALT(gRaDeregisterMR, raDeregisterMrFunc, raHandle, "ra_deregister_mr", "RaDeregisterMr");
    DL_LOAD_SYM_ALT(gRaSendWr, raSendWrFunc, raHandle, "ra_send_wr", "RaSendWr");
    DL_LOAD_SYM_ALT(gRaSendWrV2, raSendWrV2Func, raHandle, "ra_send_wr_v2", "RaSendWrV2");
    DL_LOAD_SYM_ALT(gRaGetNotifyBaseAddr, raGetNotifyBaseAddrFunc, raHandle, "ra_get_notify_base_addr",
                    "RaGetNotifyBaseAddr");
    DL_LOAD_SYM_ALT(gRaGetNotifyMrInfo, raGetNotifyMrInfoFunc, raHandle, "ra_get_notify_mr_info", "RaGetNotifyMrInfo");
    DL_LOAD_SYM_ALT(gRaSetQpAttrQos, raSetQpAttrQosFunc, raHandle, "ra_set_qp_attr_qos", "RaSetQpAttrQos");

    DL_LOAD_SYM(gTsdOpen, tsdOpenFunc, tsdHandle, "TsdOpen");
    BM_LOG_INFO("LoadLibrary for DlHccpApi success");
    gLoaded = true;
    return BM_OK;
}

void DlHccpApi::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gLoaded) {
        return;
    }

    gRaRdevGetHandle = nullptr;
    gRaInit = nullptr;
    gRaSocketInit = nullptr;
    gRaSocketDeinit = nullptr;
    gRaRdevInitV2 = nullptr;
    gRaSocketBatchConnect = nullptr;
    gRaSocketBatchClose = nullptr;
    gRaSocketListenStart = nullptr;
    gRaSocketListenStop = nullptr;
    gRaGetSockets = nullptr;
    gRaGetIfNum = nullptr;
    gRaGetIfAddrs = nullptr;
    gRaSocketWhiteListAdd = nullptr;
    gRaSocketWhiteListDel = nullptr;
    gRaQpCreate = nullptr;
    gRaQpAiCreate = nullptr;
    gRaQpDestroy = nullptr;
    gRaGetQpStatus = nullptr;
    gRaQpConnectAsync = nullptr;
    gRaRegisterMR = nullptr;
    gRaDeregisterMR = nullptr;
    gTsdOpen = nullptr;
    gRaSendWr = nullptr;
    gRaSendWrV2 = nullptr;
    gRaGetNotifyBaseAddr = nullptr;
    gRaGetNotifyMrInfo = nullptr;
    gRaQpCreateWithAttrs = nullptr;
    gRaSetQpAttrQos = nullptr;

    if (raHandle != nullptr) {
        dlclose(raHandle);
        raHandle = nullptr;
    }

    if (tsdHandle != nullptr) {
        dlclose(tsdHandle);
        tsdHandle = nullptr;
    }
    gLoaded = false;
}

// CANN >= 9.2.0 为 V2；其余（含版本获取失败，视为旧 CANN）为 V1
RaSockConnLayout DlHccpApi::GetSockConnLayout()
{
    std::string versionStr;
    if (!DlAclApi::AclsysGetVersionStr("runtime", versionStr)) {
        BM_LOG_DEBUG("get CANN version failed, default to V1 layout");
        return RaSockConnLayout::V1;
    }
    BM_LOG_INFO("CANN version string: " << versionStr);
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    if (sscanf(versionStr.c_str(), "%u.%u.%u", &major, &minor, &patch) != 3) {
        BM_LOG_WARN("invalid CANN version string: " << versionStr);
        return RaSockConnLayout::V1;
    }
    if (major > CANN_LAYOUT_V2_MAJOR || (major == CANN_LAYOUT_V2_MAJOR && minor >= CANN_LAYOUT_V2_MINOR)) {
        return RaSockConnLayout::V2;
    }
    return RaSockConnLayout::V1;
}
} // namespace mf
} // namespace ock
