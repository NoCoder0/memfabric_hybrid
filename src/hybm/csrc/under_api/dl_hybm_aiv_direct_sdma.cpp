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
#include <cstring>
#include <dlfcn.h>
#include "dl_hybm_aiv_direct_sdma.h"
#include "../copy_aiv_direct_sdma/aiv_direct_sdma_abi.h"

namespace ock {
namespace mf {
bool DlHybmAivDirectSdmaApi::gLoaded = false;
std::mutex DlHybmAivDirectSdmaApi::gMutex;
void *DlHybmAivDirectSdmaApi::libHandle = nullptr;
static const char *AivDirectSdmaLibName = "libmf_hybm_aiv_direct_sdma.so";

hybmAivDirectSdmaLaunchFunc DlHybmAivDirectSdmaApi::pHybmAivDirectSdmaLaunch = nullptr;

std::string DlHybmAivDirectSdmaApi::GetSelfLibDir()
{
    Dl_info info;
    if (dladdr(static_cast<const void *>(&AivDirectSdmaLibName), &info) == 0) {
        BM_LOG_ERROR("dladdr failed to locate self library, libName: " << AivDirectSdmaLibName);
        return "";
    }
    const char *fname = info.dli_fname;
    if (fname == nullptr || fname[0] == '\0') {
        BM_LOG_ERROR("dladdr returned empty file name, libName: " << AivDirectSdmaLibName);
        return "";
    }
    const char *lastSlash = strrchr(fname, '/');
    if (lastSlash == nullptr) {
        BM_LOG_ERROR("self library path contains no directory separator, fname: " << fname);
        return "";
    }
    if (lastSlash == fname) {
        return "/";
    }
    return std::string(fname, lastSlash - fname);
}

Result DlHybmAivDirectSdmaApi::TryLoadLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return BM_OK;
    }

    std::string libPath = GetSelfLibDir();
    if (libPath.empty()) {
        BM_LOG_WARN("failed to locate self library directory for AIV Direct SDMA.");
        return BM_ERROR;
    }

    std::string realPath;
    if (!FileUtil::LibraryRealPath(libPath, AivDirectSdmaLibName, realPath)) {
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
