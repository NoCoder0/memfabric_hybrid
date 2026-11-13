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
#include <cstring>
#include <dlfcn.h>
#include "dl_hybm_copy_extend.h"

namespace ock {
namespace mf {
bool DlHybmExtendApi::gLoaded = false;
std::mutex DlHybmExtendApi::gMutex;
void *DlHybmExtendApi::libHandle;
static const char *CopyExtendLibName = "libmf_hybm_copy_extend.so";

hybmCopyExtendFunc DlHybmExtendApi::pHybmCopyExtend = nullptr;
hybmBatchCopyExtendFunc DlHybmExtendApi::pHybmBatchCopyExtend = nullptr;

std::string DlHybmExtendApi::GetSelfLibDir()
{
    Dl_info info;
    if (dladdr(static_cast<const void *>(&CopyExtendLibName), &info) == 0) {
        BM_LOG_ERROR("dladdr failed to locate self library, libName: " << CopyExtendLibName);
        return "";
    }
    const char *fname = info.dli_fname;
    if (fname == nullptr || fname[0] == '\0') {
        BM_LOG_ERROR("dladdr returned empty file name, libName: " << CopyExtendLibName);
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

Result DlHybmExtendApi::TryLoadLibrary()
{
    std::unique_lock<std::mutex> guard(gMutex);
    if (gLoaded) {
        return BM_OK;
    }

    std::string libDir = GetSelfLibDir();
    if (libDir.empty()) {
        BM_LOG_WARN("failed to locate self library directory for copy_extend.");
        return BM_ERROR;
    }

    std::string realPath;
    if (!FileUtil::LibraryRealPath(libDir, CopyExtendLibName, realPath)) {
        BM_LOG_WARN(libDir << " get copy_extend lib path failed");
        return BM_ERROR;
    }

    /* dlopen library */
    libHandle = dlopen(realPath.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (libHandle == nullptr) {
        BM_LOG_WARN("Unable to open library [" << realPath << "], " << dlerror());
        return BM_DL_FUNCTION_FAILED;
    }

    /* load sym */
    DL_LOAD_SYM_OPTIONAL(pHybmCopyExtend, hybmCopyExtendFunc, libHandle, "hybm_copy_extend");
    DL_LOAD_SYM_OPTIONAL(pHybmBatchCopyExtend, hybmBatchCopyExtendFunc, libHandle, "hybm_batch_copy_extend");

    gLoaded = true;
    return BM_OK;
}

void DlHybmExtendApi::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gLoaded) {
        return;
    }

    pHybmCopyExtend = nullptr;
    pHybmBatchCopyExtend = nullptr;

    if (libHandle != nullptr) {
        dlclose(libHandle);
        libHandle = nullptr;
    }
    gLoaded = false;
}
} // namespace mf
} // namespace ock
