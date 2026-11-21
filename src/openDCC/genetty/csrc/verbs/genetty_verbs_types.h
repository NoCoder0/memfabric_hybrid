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
#ifndef MF_OPENDCC_GENETTY_VERBS_TYPES_H
#define MF_OPENDCC_GENETTY_VERBS_TYPES_H

#include "genetty_types.h"

namespace ock {
namespace openDCC {
namespace verbs {

constexpr uint32_t ROCE_GID_LEN = 16; /* length of a RoCE gid in bytes */

} // namespace verbs
} // namespace openDCC
} // namespace ock

#endif //MF_OPENDCC_GENETTY_VERBS_TYPES_H
