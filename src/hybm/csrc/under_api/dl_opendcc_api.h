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
#ifndef MEMFABRIC_DL_OPENDCC_API_H
#define MEMFABRIC_DL_OPENDCC_API_H
#ifdef BUILD_OPENDCC
#include "hcc.h"
#include "hcc_p2p.h"
#include "hybm_types.h"

namespace ock {
namespace mf {
class DlOpenDccApi {
public:
    static Result Load();
    static decltype(&::hcc_init) hcc_init;
    static decltype(&::hcc_uninit) hcc_uninit;
    static decltype(&::hcc_options_init) hcc_options_init;
    static decltype(&::hcc_create) hcc_create;
    static decltype(&::hcc_destroy) hcc_destroy;
    static decltype(&::hcc_memory_register) hcc_memory_register;
    static decltype(&::hcc_memory_unregister) hcc_memory_unregister;
    static decltype(&::hcc_memory_info_encode) hcc_memory_info_encode;
    static decltype(&::hcc_memory_info_decode) hcc_memory_info_decode;
    static decltype(&::hcc_memory_free_info) hcc_memory_free_info;
    static decltype(&::hcc_tunnel_create) hcc_tunnel_create;
    static decltype(&::hcc_tunnel_destroy) hcc_tunnel_destroy;
    static decltype(&::hcc_tunnel_export) hcc_tunnel_export;
    static decltype(&::hcc_tunnel_import) hcc_tunnel_import;
    static decltype(&::hcc_tunnel_free_link_info) hcc_tunnel_free_link_info;
    static decltype(&::hcc_tunnel_submit) hcc_tunnel_submit;
    static decltype(&::hcc_request_wait) hcc_request_wait;
    static decltype(&::hcc_request_release) hcc_request_release;
    static decltype(&::hcc_tunnel_quiesce) hcc_tunnel_quiesce;
};
} // namespace mf
} // namespace ock
#endif
#endif
