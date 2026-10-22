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

#ifndef MF_HYBM_KERNEL_HEPLER_H
#define MF_HYBM_KERNEL_HEPLER_H

#include "dl_acl_api.h"
#include "hybm_types.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

struct DeviceKernelNames {
    const char *funcRead{nullptr};
    const char *funcWrite{nullptr};
    const char *funcTransfer{nullptr};
    const char *funcPostSend{nullptr};
};

struct DeviceFuncHandles {
    aclrtFuncHandle batchRead{nullptr};
    aclrtFuncHandle batchWrite{nullptr};
    aclrtFuncHandle batchTransfer{nullptr};
    aclrtFuncHandle rdmaPostSend{nullptr};
};

// Load AICPU kernel JSON and resolve function handles.
// @param names         kernel function name for read/write/transfer/rdmaPostSend
// @param binHandle     [in/out] cached bin handle; if nullptr, loads from JSON
// @param funcHandles   [out] resolved function handles
// JSON path: ${ASCEND_HOME_PATH}/opp/vendors/cust/op_impl/aicpu/hybm/config/libcann_hybm_kernel.json
Result LoadDeviceKernelAndGetHandles(DeviceKernelNames &names, aclrtBinHandle &binHandle,
                                     DeviceFuncHandles &funcHandles);

// Common AICPU kernel launch sequence: ArgsInit/Append/Finalize + LaunchKernelWithConfig.
// Shared by the HCOMM/URMA batch path (device_kernel_helper.h) and the native RDMA path
// (rdma_aicpu_helper.h). Returns the first non-OK ret without logging; callers log with context.
Result LaunchAicpuKernel(aclrtFuncHandle funcHandle, void *stream, const void *args, size_t argsBytes);

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBM_KERNEL_HEPLER_H
