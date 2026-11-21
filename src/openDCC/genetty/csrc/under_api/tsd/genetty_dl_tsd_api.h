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
#ifndef MF_GENETTY_DL_TSD_API_H
#define MF_GENETTY_DL_TSD_API_H

#include "under_api/genetty_dl_api.h"

namespace ock {
namespace openDCC {
using TsdOpen_api = uint32_t (*)(uint32_t userDeviceId, uint32_t rankSize);
using TsdClose_api = uint32_t (*)(uint32_t userDeviceId);

// TODO: Consolidate these TSD wrappers with MemFabric's src/hybm/csrc/under_api/dl_hccp_api.h.
class TsdApi {
public:
    static Result Load() noexcept;
    static void UnLoad() noexcept;

    static uint32_t TsdOpen(uint32_t userDeviceId, uint32_t rankSize)
    {
        GEN_ASSERT_MSG_RETURN(TsdOpen_ptr != nullptr, "TSD open API is not loaded", GEN_NOT_INITIALIZED);
        auto result = TsdOpen_ptr(userDeviceId, rankSize);
        GEN_ASSERT_MSG_RETURN(result == 0,
                              "TsdOpen failed, result(" << result << "), userDeviceId(" << userDeviceId
                              << "), rankSize(" << rankSize << ")", result);
        return result;
    }

    static uint32_t TsdClose(uint32_t userDeviceId)
    {
        GEN_ASSERT_MSG_RETURN(TsdClose_ptr != nullptr, "TSD close API is not loaded", GEN_NOT_INITIALIZED);
        auto result = TsdClose_ptr(userDeviceId);
        GEN_ASSERT_MSG_RETURN(result == 0,
                              "TsdClose failed, result(" << result << "), userDeviceId(" << userDeviceId << ")",
                              result);
        return result;
    }

private:
    static Result LoadInner(void *handle) noexcept;
    static void UnLoadInner() noexcept;

    DL_API_DECLARE(TsdOpen);
    DL_API_DECLARE(TsdClose);

    static std::mutex LOAD_MUTEX;
    static bool LOADED;
};
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_DL_TSD_API_H
