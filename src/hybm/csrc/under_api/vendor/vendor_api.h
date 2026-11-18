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

#ifndef MF_HYBM_CORE_VENDOR_API_H
#define MF_HYBM_CORE_VENDOR_API_H

#include <cstddef>
#include <string>
#include "vendor_api_impl.h"
#include "ascend/ascend_api_impl.h"

namespace ock {
namespace mf {
class VendorApi {
public:
    static Result LoadLibrary()
    {
        return Instance().LoadLibrary();
    }

    static void CleanupLibrary()
    {
        Instance().CleanupLibrary();
    }

    static VendorApiImpl *Api() // use VendorApi::Api()->func()
    {
        static VendorApiImpl *const api = LoadInstance();
        return api;
    }

private:
    static VendorApiImpl &Instance()
    {
#if defined(ASCEND_NPU)
        static AscendApiImpl instance;
#else
        static VendorApiImpl instance;
#endif
        return instance;
    }

    static VendorApiImpl *LoadInstance()
    {
        auto &instance = Instance();
        (void)instance.LoadLibrary();
        return &instance;
    }
};
} // namespace mf
} // namespace ock
#endif // MF_HYBM_CORE_VENDOR_API_H
