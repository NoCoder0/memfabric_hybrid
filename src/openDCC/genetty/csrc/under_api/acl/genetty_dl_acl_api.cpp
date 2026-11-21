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
#include "genetty_dl_acl_api.h"

namespace ock {
namespace openDCC {
namespace {
constexpr int32_t ACL_REPEAT_INITIALIZE = 100002;

template<typename T>
T LoadAclSymbol(void *handle, const char *name, const char *fallback = nullptr) noexcept
{
    auto ptr = dlsym(handle, name);
    if (ptr == nullptr && fallback != nullptr) {
        ptr = dlsym(handle, fallback);
    }
    if (ptr == nullptr) {
        GEN_LOG_ERROR("Load ACL symbol failed, name("
                      << name << "), fallback(" << (fallback == nullptr ? "none" : fallback) << "), error(" << dlerror()
                      << ")");
    }
    return reinterpret_cast<T>(ptr);
}
} // namespace

std::mutex AclApi::LOAD_MUTEX;
bool AclApi::LOADED = false;
bool AclApi::INITIALIZED = false;
bool AclApi::OWNS_INITIALIZATION = false;

DL_API_DEFINE(AclApi, aclrtMalloc);
DL_API_DEFINE(AclApi, aclrtFree);
DL_API_DEFINE(AclApi, aclrtMemcpy);
DL_API_DEFINE(AclApi, aclInit);
DL_API_DEFINE(AclApi, aclFinalize);
DL_API_DEFINE(AclApi, aclFinalizeReference);
DL_API_DEFINE(AclApi, aclrtSetDevice);
DL_API_DEFINE(AclApi, aclrtResetDevice);
DL_API_DEFINE(AclApi, aclrtGetDevice);
DL_API_DEFINE(AclApi, aclrtGetDeviceCount);
DL_API_DEFINE(AclApi, aclrtGetLogicDevIdByUserDevId);
DL_API_DEFINE(AclApi, aclrtGetPhyDevIdByUserDevId);

Result AclApi::Init() noexcept
{
    auto result = Load();
    if (result != GEN_OK) {
        return result;
    }

    result = AclInit(nullptr);
    return result == 0 ? GEN_OK : GEN_DEVICE_ERROR;
}

int32_t AclApi::AclInit(const char *configPath)
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    GEN_ASSERT_MSG_RETURN(aclInit_ptr != nullptr, "ACL init API is not loaded", GEN_NOT_INITIALIZED);
    if (INITIALIZED) {
        return 0;
    }

    auto result = aclInit_ptr(configPath);
    GEN_ASSERT_MSG_RETURN(result == 0 || result == ACL_REPEAT_INITIALIZE,
                          "Initialize ACL failed, result(" << result << ")", result);
    OWNS_INITIALIZATION = result == 0;
    INITIALIZED = true;
    return 0;
}

int32_t AclApi::AclFinalize()
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    if (!INITIALIZED) {
        return 0;
    }

    /* Release only our reference on newer runtimes; do not finalize an externally owned legacy runtime. */
    int32_t result = 0;
    if (aclFinalizeReference_ptr != nullptr) {
        result = aclFinalizeReference_ptr(nullptr);
    } else if (OWNS_INITIALIZATION) {
        GEN_ASSERT_MSG_RETURN(aclFinalize_ptr != nullptr, "ACL finalize API is not loaded", GEN_NOT_INITIALIZED);
        result = aclFinalize_ptr();
    } else {
        GEN_ASSERT_MSG_RETURN(LOADED, "ACL APIs are not loaded", GEN_NOT_INITIALIZED);
    }
    GEN_ASSERT_MSG_RETURN(result == 0, "Finalize ACL failed, result(" << result << ")", result);
    INITIALIZED = false;
    OWNS_INITIALIZATION = false;
    return result;
}

Result AclApi::Load() noexcept
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    if (LOADED) {
        return GEN_OK;
    }

    /* step1: open library file */
    void *handle = dlopen("libascendcl.so", RTLD_NOW | RTLD_NODELETE | RTLD_GLOBAL);
    if (handle == nullptr) {
        GEN_LOG_ERROR("Open libascendcl failed, error(" << dlerror() << ")");
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

Result AclApi::LoadInner(void *handle) noexcept
{
    aclrtMalloc_ptr = LoadAclSymbol<aclrtMalloc_api>(handle, "aclrtMalloc");
    aclrtFree_ptr = LoadAclSymbol<aclrtFree_api>(handle, "aclrtFree");
    aclrtMemcpy_ptr = LoadAclSymbol<aclrtMemcpy_api>(handle, "aclrtMemcpy");
    aclInit_ptr = LoadAclSymbol<aclInit_api>(handle, "aclInit");
    aclFinalize_ptr = LoadAclSymbol<aclFinalize_api>(handle, "aclFinalize");
    /* Optional on older CANN versions. */
    aclFinalizeReference_ptr = reinterpret_cast<aclFinalizeReference_api>(dlsym(handle, "aclFinalizeReference"));
    aclrtSetDevice_ptr = LoadAclSymbol<aclrtSetDevice_api>(handle, "aclrtSetDevice");
    aclrtResetDevice_ptr = LoadAclSymbol<aclrtResetDevice_api>(handle, "aclrtResetDevice");
    aclrtGetDevice_ptr = LoadAclSymbol<aclrtGetDevice_api>(handle, "aclrtGetDevice");
    aclrtGetDeviceCount_ptr = LoadAclSymbol<aclrtGetDeviceCount_api>(handle, "aclrtGetDeviceCount");
    aclrtGetLogicDevIdByUserDevId_ptr = LoadAclSymbol<aclrtGetLogicDevIdByUserDevId_api>(
        handle, "aclrtGetLogicDevIdByUserDevId", "rtGetLogicDevIdByUserDevId");
    /* The legacy physical ID API also takes a user device ID, despite its name. */
    aclrtGetPhyDevIdByUserDevId_ptr = LoadAclSymbol<aclrtGetPhyDevIdByUserDevId_api>(
        handle, "aclrtGetPhyDevIdByUserDevId", "aclrtGetPhyDevIdByLogicDevId");

    if (aclrtMalloc_ptr == nullptr || aclrtFree_ptr == nullptr || aclrtMemcpy_ptr == nullptr ||
        aclInit_ptr == nullptr || aclFinalize_ptr == nullptr || aclrtSetDevice_ptr == nullptr ||
        aclrtResetDevice_ptr == nullptr || aclrtGetDevice_ptr == nullptr || aclrtGetDeviceCount_ptr == nullptr ||
        aclrtGetLogicDevIdByUserDevId_ptr == nullptr || aclrtGetPhyDevIdByUserDevId_ptr == nullptr) {
        return GEN_LOAD_LIBRARY_FAILED;
    }
    return GEN_OK;
}

void AclApi::UnLoad() noexcept
{
    std::lock_guard<std::mutex> guard(LOAD_MUTEX);
    UnLoadInner();
    LOADED = false;
    /* Unloading symbols does not finalize the process-wide ACL runtime. */
}

void AclApi::UnLoadInner() noexcept
{
    DL_API_SET_NULL(aclrtMalloc);
    DL_API_SET_NULL(aclrtFree);
    DL_API_SET_NULL(aclrtMemcpy);
    DL_API_SET_NULL(aclInit);
    DL_API_SET_NULL(aclFinalize);
    DL_API_SET_NULL(aclFinalizeReference);
    DL_API_SET_NULL(aclrtSetDevice);
    DL_API_SET_NULL(aclrtResetDevice);
    DL_API_SET_NULL(aclrtGetDevice);
    DL_API_SET_NULL(aclrtGetDeviceCount);
    DL_API_SET_NULL(aclrtGetLogicDevIdByUserDevId);
    DL_API_SET_NULL(aclrtGetPhyDevIdByUserDevId);
}
} // namespace openDCC
} // namespace ock
