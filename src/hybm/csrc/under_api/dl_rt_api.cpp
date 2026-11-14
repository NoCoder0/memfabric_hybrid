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
#include "dl_rt_api.h"

#include <dlfcn.h>

namespace ock {
namespace mf {
bool DlRtApi::gLoaded = false;
std::mutex DlRtApi::gMutex;
void *DlRtApi::rtHandle;
const char *DlRtApi::gRtLibName = "libruntime.so";

rtStreamGetSqidFunc DlRtApi::pRtStreamGetSqid = nullptr;
rtStreamGetCqidFunc DlRtApi::pRtStreamGetCqid = nullptr;
rtGetDevResAddressFunc DlRtApi::pRtGetDevResAddress = nullptr;
rtOpenNetServiceFunc DlRtApi::pRtOpenNetService = nullptr;
rtCloseNetServiceFunc DlRtApi::pRtCloseNetService = nullptr;
rtRDMADBSendFunc DlRtApi::pRtRDMADBSend = nullptr;
rtStreamCreateFunc DlRtApi::pRtStreamCreate = nullptr;
rtStreamDestroyFunc DlRtApi::pRtStreamDestroy = nullptr;
rtStreamSynchronizeFunc DlRtApi::pRtStreamSynchronize = nullptr;

Result DlRtApi::LoadLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return BM_OK;
    }

    /* dlopen library */
    rtHandle = dlopen(gRtLibName, RTLD_NOW);
    if (rtHandle == nullptr) {
        BM_LOG_ERROR("Failed to open library [" << gRtLibName << "], error: " << dlerror());
        return BM_DL_FUNCTION_FAILED;
    }

    /* load sym */
    DL_LOAD_SYM(pRtStreamGetSqid, rtStreamGetSqidFunc, rtHandle, "rtStreamGetSqid");
    DL_LOAD_SYM(pRtStreamGetCqid, rtStreamGetCqidFunc, rtHandle, "rtStreamGetCqid");
    DL_LOAD_SYM_OPTIONAL(pRtGetDevResAddress, rtGetDevResAddressFunc, rtHandle, "rtGetDevResAddress");
    DL_LOAD_SYM_OPTIONAL(pRtOpenNetService, rtOpenNetServiceFunc, rtHandle, "rtOpenNetService");
    DL_LOAD_SYM_OPTIONAL(pRtCloseNetService, rtCloseNetServiceFunc, rtHandle, "rtCloseNetService");
    DL_LOAD_SYM_OPTIONAL(pRtRDMADBSend, rtRDMADBSendFunc, rtHandle, "rtRDMADBSend");
    DL_LOAD_SYM_OPTIONAL(pRtStreamCreate, rtStreamCreateFunc, rtHandle, "rtStreamCreate");
    DL_LOAD_SYM_OPTIONAL(pRtStreamDestroy, rtStreamDestroyFunc, rtHandle, "rtStreamDestroy");
    DL_LOAD_SYM_OPTIONAL(pRtStreamSynchronize, rtStreamSynchronizeFunc, rtHandle, "rtStreamSynchronize");

    gLoaded = true;
    return BM_OK;
}

void DlRtApi::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gLoaded) {
        return;
    }

    pRtStreamGetSqid = nullptr;
    pRtStreamGetCqid = nullptr;
    pRtGetDevResAddress = nullptr;
    pRtOpenNetService = nullptr;
    pRtCloseNetService = nullptr;
    pRtRDMADBSend = nullptr;
    pRtStreamCreate = nullptr;
    pRtStreamDestroy = nullptr;
    pRtStreamSynchronize = nullptr;

    if (rtHandle != nullptr) {
        dlclose(rtHandle);
        rtHandle = nullptr;
    }

    gLoaded = false;
}
} // namespace mf
} // namespace ock
