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
#ifndef MF_GENETTY_VENDOR_TYPE_H
#define MF_GENETTY_VENDOR_TYPE_H

#include "common/genetty_common_includes.h"

namespace ock {
namespace openDCC {
enum VendorType : uint8_t {
    VT_INVALID = 0,
    VT_ASCEND_NPU = 1 << 1,
    VT_KUNPENG_CPU = 1 << 2,
};

inline VendorType GetVendorType(const std::string &name)
{
    if (name == "ascend") {
        return VendorType::VT_ASCEND_NPU;
    }

    if (name == "kunpeng") {
        return VendorType::VT_KUNPENG_CPU;
    }

    return VendorType::VT_INVALID;
}

inline const std::string GetVendorType(const VendorType &type)
{
    switch (type) {
        case VT_ASCEND_NPU:
            return "ascend";
        case VT_KUNPENG_CPU:
            return "kunpeng";
        default:
            break;
    }

    return "Invalid";
}
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_VENDOR_TYPE_H
