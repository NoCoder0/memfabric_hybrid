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
#pragma once
#include <memory>
#include <mutex>
#include <unordered_map>
#include "hybm_data_operator.h"
namespace ock {
namespace mf {
class AivDirectSdmaState;
class HostAivDirectSdma {
public:
    HostAivDirectSdma();
    ~HostAivDirectSdma();
    Result Copy(hybm_batch_copy_params &params, void *stream, uint32_t flags) noexcept;
    Result Wait() noexcept;
    void Close() noexcept;

private:
    std::mutex mutex_;
    std::unordered_map<void *, std::shared_ptr<AivDirectSdmaState>> streams_;
};
} // namespace mf
} // namespace ock
