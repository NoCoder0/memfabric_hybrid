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
#ifndef MF_GENETTY_DL_ACL_API_H
#define MF_GENETTY_DL_ACL_API_H

#include "under_api/genetty_dl_api.h"

namespace ock {
namespace openDCC {
using aclInit_api = int32_t (*)(const char *configPath);
using aclFinalize_api = int32_t (*)();
using aclFinalizeReference_api = int32_t (*)(uint64_t *refCount);
using aclrtSetDevice_api = int32_t (*)(int32_t deviceId);
using aclrtResetDevice_api = int32_t (*)(int32_t deviceId);
using aclrtGetDevice_api = int32_t (*)(int32_t *deviceId);
using aclrtGetDeviceCount_api = int32_t (*)(uint32_t *count);
using aclrtGetLogicDevIdByUserDevId_api = int32_t (*)(int32_t userDeviceId, int32_t *logicDeviceId);
using aclrtGetPhyDevIdByUserDevId_api = int32_t (*)(int32_t userDeviceId, int32_t *physicalDeviceId);

constexpr int ACL_MEM_MALLOC_HUGE_FIRST = 0;
constexpr int ACL_MEMCPY_HOST_TO_DEVICE = 1;
constexpr int ACL_MEMCPY_DEVICE_TO_HOST = 2;
using aclrtMalloc_api = int32_t (*)(void **address, size_t size, int policy);
using aclrtFree_api = int32_t (*)(void *address);
using aclrtMemcpy_api = int32_t (*)(void *dest, size_t destMax, const void *source, size_t size, int kind);

// TODO: Consolidate these ACL wrappers, including device ID conversion, with MemFabric's DlAclApi
// in src/hybm/csrc/under_api/dl_acl_api.h.
class AclApi {
public:
    /* Initialize ACL once through this wrapper; finalize only after all device users have stopped. */
    static Result Init() noexcept;
    static Result Load() noexcept;
    static void UnLoad() noexcept;

    static int32_t AclInit(const char *configPath);
    static int32_t AclFinalize();

    static int32_t AclrtSetDevice(int32_t userDeviceId)
    {
        GEN_ASSERT_MSG_RETURN(aclrtSetDevice_ptr != nullptr, "ACL set device API is not loaded", GEN_NOT_INITIALIZED);
        return aclrtSetDevice_ptr(userDeviceId);
    }

    static int32_t AclrtResetDevice(int32_t userDeviceId)
    {
        GEN_ASSERT_MSG_RETURN(aclrtResetDevice_ptr != nullptr, "ACL reset device API is not loaded",
                              GEN_NOT_INITIALIZED);
        return aclrtResetDevice_ptr(userDeviceId);
    }

    static int32_t AclrtGetDevice(int32_t *userDeviceId)
    {
        GEN_ASSERT_MSG_RETURN(aclrtGetDevice_ptr != nullptr, "ACL get device API is not loaded", GEN_NOT_INITIALIZED);
        return aclrtGetDevice_ptr(userDeviceId);
    }

    static int32_t AclrtGetDeviceCount(uint32_t *count)
    {
        GEN_ASSERT_MSG_RETURN(aclrtGetDeviceCount_ptr != nullptr, "ACL get device count API is not loaded",
                              GEN_NOT_INITIALIZED);
        GEN_ASSERT_MSG_RETURN(count != nullptr, "Get device count failed, as count is null", GEN_INVALID_PARAM);
        auto result = aclrtGetDeviceCount_ptr(count);
        GEN_ASSERT_MSG_RETURN(result == 0, "Get device count failed, result(" << result << ")", result);
        return result;
    }

    static int32_t AclrtGetLogicDevIdByUserDevId(int32_t userDeviceId, int32_t *logicDeviceId)
    {
        GEN_ASSERT_MSG_RETURN(aclrtGetLogicDevIdByUserDevId_ptr != nullptr, "ACL logical device ID API is not loaded",
                              GEN_NOT_INITIALIZED);
        return aclrtGetLogicDevIdByUserDevId_ptr(userDeviceId, logicDeviceId);
    }

    static int32_t AclrtGetPhyDevIdByUserDevId(int32_t userDeviceId, int32_t *physicalDeviceId)
    {
        GEN_ASSERT_MSG_RETURN(aclrtGetPhyDevIdByUserDevId_ptr != nullptr, "ACL physical device ID API is not loaded",
                              GEN_NOT_INITIALIZED);
        return aclrtGetPhyDevIdByUserDevId_ptr(userDeviceId, physicalDeviceId);
    }

    static int32_t AclrtMalloc(void **address, size_t size, int policy)
    {
        GEN_ASSERT_MSG_RETURN(aclrtMalloc_ptr != nullptr, "aclrtMalloc API is not loaded", GEN_NOT_INITIALIZED);
        auto result = aclrtMalloc_ptr(address, size, policy);
        GEN_ASSERT_MSG_RETURN(result == 0, "aclrtMalloc failed, result(" << result << ")", result);
        return result;
    }

    static int32_t AclrtFree(void *address)
    {
        GEN_ASSERT_MSG_RETURN(aclrtFree_ptr != nullptr, "aclrtFree API is not loaded", GEN_NOT_INITIALIZED);
        auto result = aclrtFree_ptr(address);
        GEN_ASSERT_MSG_RETURN(result == 0, "aclrtFree failed, result(" << result << ")", result);
        return result;
    }

    static int32_t AclrtMemcpy(void *dest, size_t destMax, const void *source, size_t size, int kind)
    {
        GEN_ASSERT_MSG_RETURN(aclrtMemcpy_ptr != nullptr, "aclrtMemcpy API is not loaded", GEN_NOT_INITIALIZED);
        auto result = aclrtMemcpy_ptr(dest, destMax, source, size, kind);
        GEN_ASSERT_MSG_RETURN(result == 0, "aclrtMemcpy failed, result(" << result << ")", result);
        return result;
    }

private:
    static Result LoadInner(void *handle) noexcept;
    static void UnLoadInner() noexcept;

    DL_API_DECLARE(aclrtMalloc);
    DL_API_DECLARE(aclrtFree);
    DL_API_DECLARE(aclrtMemcpy);
    DL_API_DECLARE(aclInit);
    DL_API_DECLARE(aclFinalize);
    DL_API_DECLARE(aclFinalizeReference);
    DL_API_DECLARE(aclrtSetDevice);
    DL_API_DECLARE(aclrtResetDevice);
    DL_API_DECLARE(aclrtGetDevice);
    DL_API_DECLARE(aclrtGetDeviceCount);
    DL_API_DECLARE(aclrtGetLogicDevIdByUserDevId);
    DL_API_DECLARE(aclrtGetPhyDevIdByUserDevId);

    static std::mutex LOAD_MUTEX;
    static bool LOADED;
    static bool INITIALIZED;
    static bool OWNS_INITIALIZATION;
};
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_DL_ACL_API_H
