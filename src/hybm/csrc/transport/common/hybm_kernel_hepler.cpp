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

#include "hybm_kernel_hepler.h"

#include <climits>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "hybm_logger.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {
namespace {
constexpr int32_t CPU_KERNEL_MODE = 0;
constexpr const char *KERNEL_JSON_SUFFIX = "/opp/vendors/cust/op_impl/aicpu/hybm/config/libcann_hybm_kernel.json";
constexpr const char *DEFAULT_ASCEND_PATH = "/usr/local/Ascend/cann";

Result LoadKernelBinary(const std::string &jsonPath, aclrtBinHandle &binHandle)
{
    char resolvedPath[PATH_MAX] = {};
    if (realpath(jsonPath.c_str(), resolvedPath) == nullptr) {
        BM_LOG_ERROR("realpath failed for kernel json: " << jsonPath
                                                         << ", please install aicpu kernel by `mfcli kernel install`");
        return BM_FILE_NOT_ACCESS;
    }
    if (access(resolvedPath, R_OK) != 0) {
        BM_LOG_ERROR("kernel json is not readable: " << resolvedPath);
        return BM_FILE_NOT_ACCESS;
    }

    aclrtBinaryLoadOption loadOption{};
    loadOption.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    loadOption.value.cpuKernelMode = CPU_KERNEL_MODE;
    aclrtBinaryLoadOptions loadOptions{};
    loadOptions.options = &loadOption;
    loadOptions.numOpt = 1;

    const auto ret = DlAclApi::AclrtBinaryLoadFromFile(resolvedPath, &loadOptions, &binHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("aclrtBinaryLoadFromFile failed, path: " << resolvedPath << " ret: " << ret);
        return ret;
    }
    return BM_OK;
}

} // namespace

Result LoadDeviceKernelAndGetHandles(DeviceKernelNames &names, aclrtBinHandle &binHandle,
                                     DeviceFuncHandles &funcHandles)
{
    funcHandles = DeviceFuncHandles{};
    const char *ascendPath = std::getenv("ASCEND_HOME_PATH");
    if (ascendPath == nullptr || ascendPath[0] == '\0') {
        ascendPath = DEFAULT_ASCEND_PATH;
    }
    const std::string jsonPath = std::string(ascendPath) + KERNEL_JSON_SUFFIX;
    BM_LOG_INFO("device_urma kernel json path: " << jsonPath);
    Result ret = BM_OK;
    if (binHandle == nullptr) {
        ret = LoadKernelBinary(jsonPath, binHandle);
        if (ret != BM_OK) {
            return ret;
        }
    }
    struct FuncSpec {
        const char *name;
        const char *label;
        aclrtFuncHandle *handle;
    };
    const FuncSpec specs[] = {
        {names.funcTransfer, "funcTransfer", &funcHandles.batchTransfer},
        {names.funcPostSend, "funcPostSend", &funcHandles.rdmaPostSend},
    };

    for (const auto &spec : specs) {
        if (spec.name == nullptr)
            continue;
        if (binHandle == nullptr) {
            BM_LOG_ERROR("device_urma kernel binary handle is null, func: " << spec.name);
            ret = BM_INVALID_PARAM;
        } else {
            ret = DlAclApi::AclrtBinaryGetFunction(binHandle, spec.name, spec.handle);
            if (ret != BM_OK) {
                BM_LOG_ERROR("aclrtBinaryGetFunction failed, func: " << spec.name << " ret: " << ret);
            } else if (*spec.handle == nullptr) {
                BM_LOG_ERROR("device_urma funcHandle is null after aclrtBinaryGetFunction");
                ret = BM_DL_FUNCTION_FAILED;
            }
        }
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to get func handle for " << spec.label << ": " << spec.name << " ret: " << ret);
            return ret;
        }
    }

    BM_LOG_INFO("AICPU kernel loaded over. Transfer: " << funcHandles.batchTransfer
                                                       << ", PostSend: " << funcHandles.rdmaPostSend);
    return BM_OK;
}

Result LaunchAicpuKernel(aclrtFuncHandle funcHandle, void *stream, const void *args, size_t argsBytes)
{
    if (funcHandle == nullptr || stream == nullptr) {
        return BM_INVALID_PARAM;
    }
    aclrtArgsHandle argsHandle = nullptr;
    auto ret = DlAclApi::AclrtKernelArgsInit(funcHandle, &argsHandle);
    if (ret != BM_OK) {
        return ret;
    }
    aclrtParamHandle paramHandle = nullptr;
    ret = DlAclApi::AclrtKernelArgsAppend(argsHandle, const_cast<void *>(args), argsBytes, &paramHandle);
    if (ret != BM_OK) {
        return ret;
    }
    ret = DlAclApi::AclrtKernelArgsFinalize(argsHandle);
    if (ret != BM_OK) {
        return ret;
    }
    constexpr uint16_t aicpuKernelTimeoutSec = static_cast<uint16_t>(27U * 68U);
    aclrtLaunchKernelAttr attr{};
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attr.value.timeout = aicpuKernelTimeoutSec;
    aclrtLaunchKernelCfg cfg{};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;
    return DlAclApi::AclrtLaunchKernelWithConfig(funcHandle, 1U, stream, &cfg, argsHandle, nullptr);
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
