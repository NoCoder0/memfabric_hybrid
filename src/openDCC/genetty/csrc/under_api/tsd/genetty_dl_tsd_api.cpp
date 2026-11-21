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
#include "genetty_dl_tsd_api.h"

namespace ock {
namespace openDCC {
std::mutex TsdApi::LOAD_MUTEX;
bool TsdApi::LOADED = false;

DL_API_DEFINE(TsdApi, TsdOpen);
DL_API_DEFINE(TsdApi, TsdClose);

Result TsdApi::Load() noexcept
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    if (LOADED) {
        return GEN_OK;
    }

    /* step1: open library file */
    void *handle = dlopen("libtsdclient.so", RTLD_NOW | RTLD_NODELETE | RTLD_GLOBAL);
    if (handle == nullptr) {
        GEN_LOG_ERROR("Open libtsdclient failed, error(" << dlerror() << ")");
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

Result TsdApi::LoadInner(void *handle) noexcept
{
    TsdOpen_ptr = reinterpret_cast<TsdOpen_api>(dlsym(handle, "TsdOpen"));
    if (TsdOpen_ptr == nullptr) {
        GEN_LOG_ERROR("Load TsdOpen failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }

    TsdClose_ptr = reinterpret_cast<TsdClose_api>(dlsym(handle, "TsdClose"));
    if (TsdClose_ptr == nullptr) {
        GEN_LOG_ERROR("Load TsdClose failed, error(" << dlerror() << ")");
        return GEN_LOAD_LIBRARY_FAILED;
    }
    return GEN_OK;
}

void TsdApi::UnLoad() noexcept
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    /* Callers must close their TSD devices before unloading the symbols. */
    UnLoadInner();
    LOADED = false;
}

void TsdApi::UnLoadInner() noexcept
{
    DL_API_SET_NULL(TsdOpen);
    DL_API_SET_NULL(TsdClose);
}
} // namespace openDCC
} // namespace ock
