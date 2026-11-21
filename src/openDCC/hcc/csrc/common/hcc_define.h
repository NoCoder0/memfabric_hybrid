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
/**
 * @file hcc_def.h
 * @brief Internal common definitions used by the HCC implementation.
 */
#ifndef MF_OPENDCC_HCC_COMMON_DEF_H
#define MF_OPENDCC_HCC_COMMON_DEF_H
#include <cstdint>

#include "mf_ref.h"

namespace ock {
namespace openDCC {
using Result = int32_t;

/* for ref */
using Referable = mf::Referable;
template<typename T>
using Ref = mf::Ref<T>;

/* macros */
#define HCC_API       __attribute__((visibility("default")))
#define ALWAYS_INLINE inline __attribute__((always_inline))
} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_HCC_COMMON_DEF_H
