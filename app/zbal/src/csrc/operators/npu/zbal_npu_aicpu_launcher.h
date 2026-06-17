/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef ZBAL_NPU_AICPU_LAUNCHER_H
#define ZBAL_NPU_AICPU_LAUNCHER_H

#include <cstdint>
#include <string>

#include "zbal_common_includes.h"
#include "zbal_comm_host_device_struct.h"
#include "dl_cann_api.h"

namespace zbal {
namespace operators {

class NpuAicpuLauncher {
public:
    NpuAicpuLauncher() = default;

    ZResult Init(const std::string &jsonPath, uint64_t workspaceGva, const CommGroupInfo &groupInfo);
    ZResult Finalize();
    void Destroy();

    ZResult Launch(const AicpuWorkDesc &desc, void *stream = nullptr);
    int32_t SyncAndDumpDebug(void *stream);
    int32_t DumpDebugBuffer();

private:
    ZResult WriteInitContext(uint64_t workspaceGva, const CommGroupInfo &groupInfo);
    ZResult LoadKernelJson(const std::string &jsonPath);

    static constexpr uint32_t kMaxCommType = 16; /* covers all AicpuCommType values */

    underapi::aclrtBinHandle kernelBinaryHandle_{};
    underapi::aclrtFuncHandle opFuncHandles_[kMaxCommType]{}; /* per-op handles for profiling */
    uint64_t workspaceGva_ = 0;
    uint64_t waitSymbol_ = 0; /* auto-incremented per Launch for cross-device barrier flags */
    bool initialized_ = false;
};

} // namespace operators
} // namespace zbal

#endif // ZBAL_NPU_AICPU_LAUNCHER_H
