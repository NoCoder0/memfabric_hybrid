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
#include "genetty_dl_hccp_api.h"

namespace ock {
namespace openDCC {
std::mutex HccpApi::LOAD_MUTEX;
bool HccpApi::LOADED = false;

DL_API_DEFINE(HccpApi, RaRegisterMr);
DL_API_DEFINE(HccpApi, RaDeregisterMr);
DL_API_DEFINE(HccpApi, RaQpCreateWithAttrs);
DL_API_DEFINE(HccpApi, RaQpDestroy);
DL_API_DEFINE(HccpApi, RaGetQpAttr);
DL_API_DEFINE(HccpApi, RaTypicalQpModify);
DL_API_DEFINE(HccpApi, RaSendWrV2);
DL_API_DEFINE(HccpApi, RaPollCq);
DL_API_DEFINE(HccpApi, RaInit);
DL_API_DEFINE(HccpApi, RaDeinit);
DL_API_DEFINE(HccpApi, RaRdevInitV2);
DL_API_DEFINE(HccpApi, RaRdevDeinit);
DL_API_DEFINE(HccpApi, RaGetIfNum);
DL_API_DEFINE(HccpApi, RaGetIfAddrs);

Result HccpApi::Load() noexcept
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    if (LOADED) {
        return GEN_OK;
    }

    /* step1: open library file */
    void *handle = dlopen("libra.so", RTLD_NOW | RTLD_NODELETE | RTLD_GLOBAL);
    if (handle == nullptr) {
        GEN_LOG_ERROR("Open libra failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }

    /* step2: load functions */
    auto result = LoadInner(handle);
    if (result != GEN_OK) {
        UnLoadInner();
    }

    /* step3: close handle; RTLD_NODELETE keeps the loaded code available */
    dlclose(handle);
    LOADED = result == GEN_OK;
    return result;
}

Result HccpApi::LoadInner(void *handle) noexcept
{
    RaInit_ptr = reinterpret_cast<RaInit_api>(dlsym(handle, "ra_init"));
    if (RaInit_ptr == nullptr) {
        RaInit_ptr = reinterpret_cast<RaInit_api>(dlsym(handle, "RaInit"));
    }
    if (RaInit_ptr == nullptr) {
        GEN_LOG_ERROR("Load ra_init/RaInit failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }

    RaDeinit_ptr = reinterpret_cast<RaDeinit_api>(dlsym(handle, "ra_deinit"));
    if (RaDeinit_ptr == nullptr) {
        RaDeinit_ptr = reinterpret_cast<RaDeinit_api>(dlsym(handle, "RaDeinit"));
    }
    if (RaDeinit_ptr == nullptr) {
        GEN_LOG_ERROR("Load ra_deinit/RaDeinit failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }

    RaRdevInitV2_ptr = reinterpret_cast<RaRdevInitV2_api>(dlsym(handle, "ra_rdev_init_v2"));
    if (RaRdevInitV2_ptr == nullptr) {
        RaRdevInitV2_ptr = reinterpret_cast<RaRdevInitV2_api>(dlsym(handle, "RaRdevInitV2"));
    }
    if (RaRdevInitV2_ptr == nullptr) {
        GEN_LOG_ERROR("Load ra_rdev_init_v2/RaRdevInitV2 failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }

    RaRdevDeinit_ptr = reinterpret_cast<RaRdevDeinit_api>(dlsym(handle, "ra_rdev_deinit"));
    if (RaRdevDeinit_ptr == nullptr) {
        RaRdevDeinit_ptr = reinterpret_cast<RaRdevDeinit_api>(dlsym(handle, "RaRdevDeinit"));
    }
    if (RaRdevDeinit_ptr == nullptr) {
        GEN_LOG_ERROR("Load ra_rdev_deinit/RaRdevDeinit failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }
    return LoadInterfaceApis(handle);
}

Result HccpApi::LoadInterfaceApis(void *handle) noexcept
{
    RaGetIfNum_ptr = reinterpret_cast<RaGetIfNum_api>(dlsym(handle, "ra_get_ifnum"));
    if (RaGetIfNum_ptr == nullptr) {
        RaGetIfNum_ptr = reinterpret_cast<RaGetIfNum_api>(dlsym(handle, "RaGetIfnum"));
    }
    if (RaGetIfNum_ptr == nullptr) {
        GEN_LOG_ERROR("Load ra_get_ifnum/RaGetIfnum failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }

    RaGetIfAddrs_ptr = reinterpret_cast<RaGetIfAddrs_api>(dlsym(handle, "ra_get_ifaddrs"));
    if (RaGetIfAddrs_ptr == nullptr) {
        RaGetIfAddrs_ptr = reinterpret_cast<RaGetIfAddrs_api>(dlsym(handle, "RaGetIfaddrs"));
    }
    if (RaGetIfAddrs_ptr == nullptr) {
        GEN_LOG_ERROR("Load ra_get_ifaddrs/RaGetIfaddrs failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }
    return LoadMemoryApis(handle);
}

Result HccpApi::LoadMemoryApis(void *handle) noexcept
{
    RaRegisterMr_ptr = reinterpret_cast<RaRegisterMr_api>(dlsym(handle, "ra_register_mr"));
    if (RaRegisterMr_ptr == nullptr) {
        RaRegisterMr_ptr = reinterpret_cast<RaRegisterMr_api>(dlsym(handle, "RaRegisterMr"));
    }
    if (RaRegisterMr_ptr == nullptr) {
        GEN_LOG_ERROR("Load ra_register_mr/RaRegisterMr failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }
    RaDeregisterMr_ptr = reinterpret_cast<RaDeregisterMr_api>(dlsym(handle, "ra_deregister_mr"));
    if (RaDeregisterMr_ptr == nullptr) {
        RaDeregisterMr_ptr = reinterpret_cast<RaDeregisterMr_api>(dlsym(handle, "RaDeregisterMr"));
    }
    if (RaDeregisterMr_ptr == nullptr) {
        GEN_LOG_ERROR("Load ra_deregister_mr/RaDeregisterMr failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }
    return LoadQueueApis(handle);
}

Result HccpApi::LoadQueueApis(void *handle) noexcept
{
    RaQpCreateWithAttrs_ptr = reinterpret_cast<RaQpCreateWithAttrs_api>(dlsym(handle, "ra_qp_create_with_attrs"));
    if (RaQpCreateWithAttrs_ptr == nullptr) {
        RaQpCreateWithAttrs_ptr = reinterpret_cast<RaQpCreateWithAttrs_api>(dlsym(handle, "RaQpCreateWithAttrs"));
    }
    RaQpDestroy_ptr = reinterpret_cast<RaQpDestroy_api>(dlsym(handle, "ra_qp_destroy"));
    if (RaQpDestroy_ptr == nullptr) {
        RaQpDestroy_ptr = reinterpret_cast<RaQpDestroy_api>(dlsym(handle, "RaQpDestroy"));
    }
    RaGetQpAttr_ptr = reinterpret_cast<RaGetQpAttr_api>(dlsym(handle, "ra_get_qp_attr"));
    if (RaGetQpAttr_ptr == nullptr) {
        RaGetQpAttr_ptr = reinterpret_cast<RaGetQpAttr_api>(dlsym(handle, "RaGetQpAttr"));
    }
    RaTypicalQpModify_ptr = reinterpret_cast<RaTypicalQpModify_api>(dlsym(handle, "ra_typical_qp_modify"));
    if (RaTypicalQpModify_ptr == nullptr) {
        RaTypicalQpModify_ptr = reinterpret_cast<RaTypicalQpModify_api>(dlsym(handle, "RaTypicalQpModify"));
    }
    RaSendWrV2_ptr = reinterpret_cast<RaSendWrV2_api>(dlsym(handle, "ra_send_wr_v2"));
    if (RaSendWrV2_ptr == nullptr) {
        RaSendWrV2_ptr = reinterpret_cast<RaSendWrV2_api>(dlsym(handle, "RaSendWrV2"));
    }
    RaPollCq_ptr = reinterpret_cast<RaPollCq_api>(dlsym(handle, "ra_poll_cq"));
    if (RaPollCq_ptr == nullptr) {
        RaPollCq_ptr = reinterpret_cast<RaPollCq_api>(dlsym(handle, "RaPollCq"));
    }
    if (RaQpCreateWithAttrs_ptr == nullptr || RaQpDestroy_ptr == nullptr || RaGetQpAttr_ptr == nullptr ||
        RaTypicalQpModify_ptr == nullptr || RaSendWrV2_ptr == nullptr || RaPollCq_ptr == nullptr) {
        GEN_LOG_ERROR("Load HCCP QP APIs failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }
    return GEN_OK;
}

void HccpApi::UnLoad() noexcept
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    /* Callers must deinitialize RA before unloading the symbols. */
    UnLoadInner();
    LOADED = false;
}

void HccpApi::UnLoadInner() noexcept
{
    DL_API_SET_NULL(RaRegisterMr);
    DL_API_SET_NULL(RaDeregisterMr);
    DL_API_SET_NULL(RaQpCreateWithAttrs);
    DL_API_SET_NULL(RaQpDestroy);
    DL_API_SET_NULL(RaGetQpAttr);
    DL_API_SET_NULL(RaTypicalQpModify);
    DL_API_SET_NULL(RaSendWrV2);
    DL_API_SET_NULL(RaPollCq);
    DL_API_SET_NULL(RaInit);
    DL_API_SET_NULL(RaDeinit);
    DL_API_SET_NULL(RaRdevInitV2);
    DL_API_SET_NULL(RaRdevDeinit);
    DL_API_SET_NULL(RaGetIfNum);
    DL_API_SET_NULL(RaGetIfAddrs);
}
} // namespace openDCC
} // namespace ock
