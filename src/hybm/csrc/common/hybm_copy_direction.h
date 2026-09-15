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

#ifndef MF_HYBRID_HYBM_COPY_DIRECTION_H
#define MF_HYBRID_HYBM_COPY_DIRECTION_H

#include <array>

#include "hybm_def.h"

namespace ock {
namespace mf {

// device rdma/urma都用
constexpr std::array<hybm_mem_type, HYBM_DATA_COPY_DIRECTION_BUTT> HybmDirectionSrcMemType = {
    HYBM_MEM_TYPE_HOST,   HYBM_MEM_TYPE_HOST,   HYBM_MEM_TYPE_DEVICE, HYBM_MEM_TYPE_DEVICE, HYBM_MEM_TYPE_HOST,
    HYBM_MEM_TYPE_HOST,   HYBM_MEM_TYPE_HOST,   HYBM_MEM_TYPE_HOST,   HYBM_MEM_TYPE_DEVICE, HYBM_MEM_TYPE_DEVICE,
    HYBM_MEM_TYPE_DEVICE, HYBM_MEM_TYPE_DEVICE, HYBM_MEM_TYPE_BUTT,
};

constexpr std::array<hybm_mem_type, HYBM_DATA_COPY_DIRECTION_BUTT> HybmDirectionDestMemType = {
    HYBM_MEM_TYPE_HOST,   HYBM_MEM_TYPE_DEVICE, HYBM_MEM_TYPE_HOST,   HYBM_MEM_TYPE_DEVICE, HYBM_MEM_TYPE_HOST,
    HYBM_MEM_TYPE_DEVICE, HYBM_MEM_TYPE_HOST,   HYBM_MEM_TYPE_DEVICE, HYBM_MEM_TYPE_HOST,   HYBM_MEM_TYPE_DEVICE,
    HYBM_MEM_TYPE_HOST,   HYBM_MEM_TYPE_DEVICE, HYBM_MEM_TYPE_BUTT,
};

} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_COPY_DIRECTION_H
