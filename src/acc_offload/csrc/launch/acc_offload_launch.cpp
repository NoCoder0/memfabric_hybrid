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
#include <cstring>
#include "mf_file_util.h"
#include "acc_offload_launch.h"

namespace ock {
namespace offload {

bool AccOffloadLaunchApi::gLoaded = false;
std::mutex AccOffloadLaunchApi::gMutex;
void *AccOffloadLaunchApi::libHandle = nullptr;
const char *AccOffloadLaunchApi::gAccOffloadLibName = "libmf_hybm_accoffload.so";

AccOffloadSparseCopyFunc AccOffloadLaunchApi::pAccOffloadSparseCopy = nullptr;

AccOffloadGroupPackCopyFunc AccOffloadLaunchApi::pAccOffloadGroupPackCopy = nullptr;

AccOffloadKvExchangeFunc AccOffloadLaunchApi::pAccOffloadKvExchange = nullptr;

AccOffloadEntryGatherFunc AccOffloadLaunchApi::pAccOffloadEntryGather = nullptr;

std::string AccOffloadLaunchApi::GetSelfLibDir()
{
    Dl_info info;
    if (dladdr(static_cast<const void *>(&AccOffloadLaunchApi::gAccOffloadLibName), &info) == 0) {
        OFFLOAD_LOG_ERROR("dladdr failed to locate self library, libName: " << gAccOffloadLibName);
        return "";
    }

    const char *fname = info.dli_fname;
    if (fname == nullptr || fname[0] == '\0') {
        OFFLOAD_LOG_ERROR("dladdr returned empty file name, libName: " << gAccOffloadLibName);
        return "";
    }

    const char *lastSlash = strrchr(fname, '/');
    if (lastSlash == nullptr) {
        OFFLOAD_LOG_ERROR("self library path contains no directory separator, fname: " << fname);
        return "";
    }

    if (lastSlash == fname) {
        return "/";
    }

    return std::string(fname, lastSlash - fname);
}

int32_t AccOffloadLaunchApi::TryLoadLibrary()
{
    std::unique_lock<std::mutex> guard(gMutex);
    if (gLoaded) {
        return OFFLOAD_OK;
    }

    std::string libDir = GetSelfLibDir();
    if (libDir.empty()) {
        OFFLOAD_LOG_WARN("failed to locate self library directory.");
        return OFFLOAD_ERROR;
    }

    std::string realPath;
    if (!ock::mf::FileUtil::LibraryRealPath(libDir, std::string(gAccOffloadLibName), realPath)) {
        OFFLOAD_LOG_WARN(libDir << " get lib path failed");
        return OFFLOAD_ERROR;
    }

    libHandle = dlopen(realPath.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (libHandle == nullptr) {
        OFFLOAD_LOG_WARN("Failed to open library [" << realPath << "], error: " << dlerror());
        return OFFLOAD_FUNCTION_FAILED;
    }

    DL_LOAD_SYM_OPTIONAL(pAccOffloadSparseCopy, AccOffloadSparseCopyFunc, libHandle, "AccOffloadSparseCopy");

    DL_LOAD_SYM_OPTIONAL(pAccOffloadGroupPackCopy, AccOffloadGroupPackCopyFunc, libHandle, "AccOffloadGroupPackCopy");

    DL_LOAD_SYM_OPTIONAL(pAccOffloadKvExchange, AccOffloadKvExchangeFunc, libHandle, "AccOffloadKvExchange");

    DL_LOAD_SYM_OPTIONAL(pAccOffloadEntryGather, AccOffloadEntryGatherFunc, libHandle, "AccOffloadEntryGather");

    gLoaded = true;
    return OFFLOAD_OK;
}

void AccOffloadLaunchApi::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gLoaded) {
        return;
    }

    pAccOffloadSparseCopy = nullptr;

    pAccOffloadGroupPackCopy = nullptr;

    pAccOffloadKvExchange = nullptr;

    pAccOffloadEntryGather = nullptr;

    if (libHandle != nullptr) {
        dlclose(libHandle);
        libHandle = nullptr;
    }

    gLoaded = false;
}

} // namespace offload
} // namespace ock
