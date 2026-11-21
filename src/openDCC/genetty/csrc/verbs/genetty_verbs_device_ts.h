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
#ifndef MF_GENETTY_DEVICE_TS_H
#define MF_GENETTY_DEVICE_TS_H

#include <memory>
#include "genetty_verbs_types.h"

namespace ock {
namespace openDCC {
namespace verbs {
/* One TS stream per Device QP; the QP serializes submission and destruction. */
class DeviceTsWriter {
public:
    DeviceTsWriter();
    ~DeviceTsWriter();
    Result Initialize(uint32_t userDeviceId, uint32_t logicDeviceId) noexcept;
    Result Submit(uint64_t dbInfo, uint32_t timeoutMs) noexcept;
    Result Destroy() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace verbs
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_DEVICE_TS_H
