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
#ifndef HYBM_DATA_OP_TEST_HELPER_H
#define HYBM_DATA_OP_TEST_HELPER_H
#include <mockcpp/mockcpp.hpp>
#include "hybm_va_manager.h"

namespace ock::mf::test {
inline uint64_t IdentityVa(HybmVaManager *, uint64_t va, uint32_t, uint32_t)
{
    return va;
}

// Existing backend dispatch tests use dummy addresses; conversion failures are tested with real VA records.
inline void StubIdentityVaMappings()
{
    union {
        uint64_t (HybmVaManager::*member)(uint64_t, uint32_t, uint32_t);
        uint64_t (*function)(HybmVaManager *, uint64_t, uint32_t, uint32_t);
    } transform;
    transform.member = &HybmVaManager::TransformVa;
    MOCKER(transform.function).stubs().will(invoke(IdentityVa));
}
} // namespace ock::mf::test
#endif
