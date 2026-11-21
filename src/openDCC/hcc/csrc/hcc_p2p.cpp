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
/**
 * @file hcc_p2p.cpp
 * @brief Implementation of the HCC point-to-point APIs.
 */

#include "hcc_p2p.h"
#include "common/hcc_common_includes.h"
#include "hcc_types.h"

namespace ock {
namespace openDCC {
#ifdef __cplusplus
extern "C" {
#endif

int32_t HCC_API hcc_init(PROTOCOL protocol, const char *vendor, uint32_t flags)
{
    /* step1: check param */
    HCC_ASSERT_MSG_RETURN(protocol != 0, "hcc init failed as protocol is 0", HCC_INVALID_PARAM);
    HCC_ASSERT_MSG_RETURN(vendor != nullptr, "hcc init failed as vendor is null", HCC_INVALID_PARAM);

    /* step2: create HccInitOptions */
    HccInitOptions options{protocol, vendor, flags, nullptr};

    /* step3: call Hcc::Instance to initialize */
    Result result = Hcc::Instance().Init(options);
    if (result != HCC_OK) {
        HCC_LOG_ERROR("hcc init failed, protocol(" << protocol << "), vendor(" << vendor << "), result(" << result
                                                   << ")");
        return result;
    }

    return HCC_OK;
}

void HCC_API hcc_uninit(uint32_t flags)
{
    /* call Hcc::Instance() to uninit */
    Hcc::Instance().Uninit(flags);
}

void HCC_API hcc_options_init(hcc_options_t *options)
{
    (void)options;
}

hcc_t HCC_API hcc_create(hcc_options_t *options)
{
    (void)options;
    return nullptr;
}

void hcc_destroy(hcc_t *handle, uint32_t flags)
{
    (void)handle;
    (void)flags;
}

int32_t hcc_memory_register(hcc_t *handle, hcc_mem_segment_t *segment, uint32_t flags)
{
    (void)handle;
    (void)segment;
    (void)flags;
    return HCC_OK;
}

void hcc_memory_unregister(hcc_t *handle, hcc_mem_segment_t *segment, uint32_t flags)
{
    (void)handle;
    (void)segment;
    (void)flags;
}

hcc_tunnel_t hcc_tunnel_create(hcc_t tunnel_handle, hcc_tunnel_options_t *options)
{
    (void)tunnel_handle;
    (void)options;
    return nullptr;
}

void hcc_tunnel_destroy(hcc_tunnel_t *tunnel_handle, uint32_t flags)
{
    (void)tunnel_handle;
    (void)flags;
}

int32_t hcc_tunnel_export(hcc_tunnel_t *tunnel_handle, uint32_t flags, hcc_link_info_t *link_info)
{
    (void)tunnel_handle;
    (void)flags;
    (void)link_info;
    return HCC_OK;
}

void hcc_tunnel_import(hcc_tunnel_t *tunnel_handle, uint32_t flags, hcc_link_info_t *link_info)
{
    (void)tunnel_handle;
    (void)flags;
    (void)link_info;
}

void hcc_tunnel_free_link_info(hcc_link_info_t *link_info)
{
    (void)link_info;
}

#ifdef __cplusplus
}
#endif
} // namespace openDCC
} // namespace ock
