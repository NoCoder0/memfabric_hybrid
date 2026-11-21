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
#ifdef BUILD_OPENDCC
#include "dl_opendcc_api.h"
#include <dlfcn.h>
#include <mutex>
#include "hybm_logger.h"

namespace ock {
namespace mf {
decltype(&::hcc_init) DlOpenDccApi::hcc_init = nullptr;
decltype(&::hcc_uninit) DlOpenDccApi::hcc_uninit = nullptr;
decltype(&::hcc_options_init) DlOpenDccApi::hcc_options_init = nullptr;
decltype(&::hcc_create) DlOpenDccApi::hcc_create = nullptr;
decltype(&::hcc_destroy) DlOpenDccApi::hcc_destroy = nullptr;
decltype(&::hcc_memory_register) DlOpenDccApi::hcc_memory_register = nullptr;
decltype(&::hcc_memory_unregister) DlOpenDccApi::hcc_memory_unregister = nullptr;
decltype(&::hcc_memory_info_encode) DlOpenDccApi::hcc_memory_info_encode = nullptr;
decltype(&::hcc_memory_info_decode) DlOpenDccApi::hcc_memory_info_decode = nullptr;
decltype(&::hcc_memory_free_info) DlOpenDccApi::hcc_memory_free_info = nullptr;
decltype(&::hcc_tunnel_create) DlOpenDccApi::hcc_tunnel_create = nullptr;
decltype(&::hcc_tunnel_destroy) DlOpenDccApi::hcc_tunnel_destroy = nullptr;
decltype(&::hcc_tunnel_export) DlOpenDccApi::hcc_tunnel_export = nullptr;
decltype(&::hcc_tunnel_import) DlOpenDccApi::hcc_tunnel_import = nullptr;
decltype(&::hcc_tunnel_free_link_info) DlOpenDccApi::hcc_tunnel_free_link_info = nullptr;
decltype(&::hcc_tunnel_submit) DlOpenDccApi::hcc_tunnel_submit = nullptr;
decltype(&::hcc_request_wait) DlOpenDccApi::hcc_request_wait = nullptr;
decltype(&::hcc_request_release) DlOpenDccApi::hcc_request_release = nullptr;
decltype(&::hcc_tunnel_quiesce) DlOpenDccApi::hcc_tunnel_quiesce = nullptr;

Result DlOpenDccApi::Load()
{
    static std::mutex mutex;
    static void *handle = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (handle != nullptr) {
        return BM_OK;
    }
    auto library = dlopen("libmf_opendcc.so", RTLD_NOW | RTLD_LOCAL);
    BM_ASSERT_LOG_AND_RETURN(library != nullptr, "Load openDCC failed: " << dlerror(), BM_UNDER_API_UNLOAD);
#define LOAD_OPENDCC_API(name)                                      \
    name = reinterpret_cast<decltype(name)>(dlsym(library, #name)); \
    if (name == nullptr) {                                          \
        BM_LOG_ERROR("Missing openDCC symbol: " << #name);          \
        dlclose(library);                                           \
        return BM_UNDER_API_UNLOAD;                                 \
    }
    LOAD_OPENDCC_API(hcc_init);
    LOAD_OPENDCC_API(hcc_uninit);
    LOAD_OPENDCC_API(hcc_options_init);
    LOAD_OPENDCC_API(hcc_create);
    LOAD_OPENDCC_API(hcc_destroy);
    LOAD_OPENDCC_API(hcc_memory_register);
    LOAD_OPENDCC_API(hcc_memory_unregister);
    LOAD_OPENDCC_API(hcc_memory_info_encode);
    LOAD_OPENDCC_API(hcc_memory_info_decode);
    LOAD_OPENDCC_API(hcc_memory_free_info);
    LOAD_OPENDCC_API(hcc_tunnel_create);
    LOAD_OPENDCC_API(hcc_tunnel_destroy);
    LOAD_OPENDCC_API(hcc_tunnel_export);
    LOAD_OPENDCC_API(hcc_tunnel_import);
    LOAD_OPENDCC_API(hcc_tunnel_free_link_info);
    LOAD_OPENDCC_API(hcc_tunnel_submit);
    LOAD_OPENDCC_API(hcc_request_wait);
    LOAD_OPENDCC_API(hcc_request_release);
    LOAD_OPENDCC_API(hcc_tunnel_quiesce);
#undef LOAD_OPENDCC_API
    handle = library;
    return BM_OK;
}
} // namespace mf
} // namespace ock
#endif
