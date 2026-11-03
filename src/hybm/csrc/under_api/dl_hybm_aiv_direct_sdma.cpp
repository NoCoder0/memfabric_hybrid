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
#include <dlfcn.h>
#include "dl_hybm_aiv_direct_sdma.h"
#include "../copy_aiv_direct_sdma/aiv_direct_sdma_abi.h"

namespace ock {
namespace mf {
bool DlHybmAivDirectSdmaApi::gLoaded = false;
std::mutex DlHybmAivDirectSdmaApi::gMutex;
void *DlHybmAivDirectSdmaApi::libHandle = nullptr;
const char *DlHybmAivDirectSdmaApi::gHybmAivDirectSdmaLibName = "libmf_hybm_aiv_direct_sdma.so";
hybmAivDirectSdmaLaunchFunc DlHybmAivDirectSdmaApi::pHybmAivDirectSdmaLaunch = nullptr;

Result DlHybmAivDirectSdmaApi::TryLoadLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return BM_OK;
    }

    char *path = std::getenv("MEMFABRIC_HYBRID_EXTEND_LIB_PATH");
    if (path == nullptr) {
        BM_LOG_WARN("Environment MEMFABRIC_HYBRID_EXTEND_LIB_PATH is not set for AIV Direct SDMA.");
        return BM_ERROR;
    }
    std::string libPath(path);
    if (!FileUtil::Realpath(libPath) || !FileUtil::IsDir(libPath)) {
        BM_LOG_WARN("Environment MEMFABRIC_HYBRID_EXTEND_LIB_PATH check failed for AIV Direct SDMA.");
        return BM_ERROR;
    }

    std::string realPath;
    if (!FileUtil::LibraryRealPath(libPath, std::string(gHybmAivDirectSdmaLibName), realPath)) {
        BM_LOG_WARN(libPath << " get AIV Direct SDMA library path failed");
        return BM_ERROR;
    }

    libHandle = dlopen(realPath.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (libHandle == nullptr) {
        BM_LOG_WARN("Failed to open library [" << realPath << "], error: " << dlerror());
        return BM_DL_FUNCTION_FAILED;
    }

    auto launch = reinterpret_cast<hybmAivDirectSdmaLaunchFunc>(dlsym(libHandle, "hybm_aiv_direct_sdma_launch"));
    auto version =
        reinterpret_cast<hybmAivDirectSdmaAbiVersionFunc>(dlsym(libHandle, "hybm_aiv_direct_sdma_abi_version"));
    if (launch == nullptr || version == nullptr || version() != AIV_DIRECT_SDMA_ABI_VERSION) {
        BM_LOG_WARN("AIV Direct SDMA extension symbols or ABI version do not match MemFabric Hybrid.");
        dlclose(libHandle);
        libHandle = nullptr;
        return BM_DL_FUNCTION_FAILED;
    }

    pHybmAivDirectSdmaLaunch = launch;
    gLoaded = true;
    return BM_OK;
}

void DlHybmAivDirectSdmaApi::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    pHybmAivDirectSdmaLaunch = nullptr;
    if (libHandle != nullptr) {
        dlclose(libHandle);
        libHandle = nullptr;
    }
    gLoaded = false;
}
} // namespace mf
} // namespace ock
