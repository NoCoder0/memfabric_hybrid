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
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "zbal_comm_host_device_struct.h"
#include "zbal_init_state.h"
#include "dl_cann_api.h"
#include "zbal_npu_aicpu_launcher.h"

namespace zbal {
namespace operators {

using namespace underapi;

constexpr uint32_t ACL_MEMCPY_DEVICE_TO_HOST = 2;
constexpr uint32_t ACL_MEMCPY_HOST_TO_DEVICE = 1;

ZResult NpuAicpuLauncher::Init(const std::string &jsonPath, uint64_t workspaceGva, const CommGroupInfo &groupInfo)
{
    if (initialized_) {
        return Z_OK;
    }

    /* Step 1: Write init context to workspace */
    ZResult ret = WriteInitContext(workspaceGva, groupInfo);
    if (ret != Z_OK) {
        return ret;
    }

    /* Step 2: Load JSON descriptor, resolve kernel function */
    ret = LoadKernelJson(jsonPath);
    if (ret != Z_OK) {
        return ret;
    }

    workspaceGva_ = workspaceGva;
    initialized_ = true;

    ZBAL_LOG_INFO("AICPU dispatcher initialized, workspaceGva=0x" << std::hex << workspaceGva << ", rankId=" << std::dec
                                                                  << groupInfo.myGroupRank
                                                                  << ", groupSize=" << groupInfo.groupSize);
    return Z_OK;
}

ZResult NpuAicpuLauncher::Finalize()
{
    if (!initialized_) {
        return Z_OK;
    }
    ZBAL_LOG_INFO("AICPU dispatcher finalized");
    return Z_OK;
}

void NpuAicpuLauncher::Destroy()
{
    if (!initialized_) {
        return;
    }

    if (kernelBinaryHandle_ != nullptr) {
        DlCannApi::AclrtBinaryUnLoad(kernelBinaryHandle_);
        kernelBinaryHandle_ = nullptr;
    }

    workspaceGva_ = 0;
    initialized_ = false;
}

ZResult NpuAicpuLauncher::Launch(const AicpuWorkDesc &desc, void *stream)
{
    if (!initialized_ || stream == nullptr) {
        ZBAL_LOG_ERROR("AICPU Launch: not initialized or stream is null");
        return Z_RT_ERROR;
    }

    /* Fill workspace GVA and waitSymbol — ACL runtime handles H2D automatically */
    AicpuWorkDesc param = desc;
    param.sdmaWorkspaceGva = workspaceGva_;
    param.waitSymbol = ++waitSymbol_;

    /* Select per-op function handle for profiling visibility */
    uint32_t commIdx = param.commType < kMaxCommType ? param.commType : 0;
    aclrtFuncHandle launchHandle = opFuncHandles_[commIdx];
    uint32_t numBlocks = (param.commType == ZBAL_CMD_SEND || param.commType == ZBAL_CMD_RECV) ? 1U : 4U;

    /* V2 API: single call, passes host buffer directly (no ArgsInit/Append/Finalize) */
    int32_t aclRet =
        DlCannApi::AclrtLaunchKernelWithHostArgs(launchHandle, numBlocks, stream, nullptr, &param, sizeof(param));
    if (aclRet != 0) {
        ZBAL_LOG_ERROR("AclrtLaunchKernelWithHostArgs failed, ret=" << static_cast<int32_t>(aclRet));
        return Z_DL_FUNCTION_UNLOAD;
    }

    return Z_OK;
}

int32_t NpuAicpuLauncher::SyncAndDumpDebug(void *stream)
{
    DlCannApi::AclrtSynchronizeStream(stream);
    return DumpDebugBuffer();
}

/* ================================================================
 * Internal helpers
 * ================================================================ */

ZResult NpuAicpuLauncher::WriteInitContext(uint64_t workspaceGva, const CommGroupInfo &groupInfo)
{
    AicpuInitContext initCtx{};

    initCtx.rankId = groupInfo.myGroupRank;
    initCtx.rankNum = groupInfo.groupSize;
    initCtx.localDeviceMemSize = groupInfo.localDeviceMemSize;
    initCtx.exchangeGva = groupInfo.myAddressExchangeGva;

    uint64_t initCtxGva = workspaceGva + ZBAL_AICPU_INIT_CTX_OFFSET;
    ZResult ret = DlCannApi::AclrtMemcpy(reinterpret_cast<void *>(initCtxGva), sizeof(AicpuInitContext), &initCtx,
                                         sizeof(AicpuInitContext), ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != Z_OK) {
        ZBAL_LOG_ERROR("Failed to write AICPU init context to workspace, gva=0x" << std::hex << initCtxGva);
        return ret;
    }

    ZBAL_LOG_INFO("AICPU init context written, offset=0x" << std::hex << ZBAL_AICPU_INIT_CTX_OFFSET
                                                          << ", rankId=" << std::dec << initCtx.rankId
                                                          << ", rankNum=" << initCtx.rankNum);
    return Z_OK;
}

/* ================================================================
 * Debug buffer host-side structs — must match device zbal_aicpu_debug.h
 * ================================================================ */
constexpr uint32_t AICPU_DEBUG_MAGIC_HOST = 0xA1C0DEB0U;
constexpr uint32_t AICPU_DEBUG_HEADER_SIZE_HOST = 16;
constexpr uint32_t AICPU_DEBUG_ENTRY_SIZE_HOST = 24;

struct AicpuDebugHeaderHost {
    uint32_t magic;
    uint32_t count;
    uint32_t seq;
    uint32_t reserved;
};

struct AicpuDebugEntryHost {
    uint32_t tag;
    uint32_t info;
    uint64_t val0;
    uint64_t val1;
};

/* Debug tag IDs — must match device-side AicpuDebugTag enum in zbal_aicpu_debug.h */
constexpr uint32_t K_TAG_ENTRY = 0;
constexpr uint32_t K_TAG_INIT_CTX_RANK = 10;
constexpr uint32_t K_TAG_UPDATE_CTX = 13;
constexpr uint32_t K_TAG_ALGO_ALLGATHER = 30;
constexpr uint32_t K_TAG_RETURN = 61;

static const char *AicpuDebugTagName(uint32_t tag)
{
    switch (tag) {
        case K_TAG_ENTRY:
            return "ENTRY";
        case K_TAG_INIT_CTX_RANK:
            return "INIT_CTX_RANK";
        case K_TAG_UPDATE_CTX:
            return "UPDATE_CTX";
        case K_TAG_ALGO_ALLGATHER:
            return "ALGO_ALLGATHER";
        case K_TAG_RETURN:
            return "RETURN";
        default:
            return "UNKNOWN";
    }
}

int32_t NpuAicpuLauncher::DumpDebugBuffer()
{
    if (!initialized_ || workspaceGva_ == 0) {
        ZBAL_LOG_ERROR("AICPU DumpDebugBuffer: not initialized");
        return -1;
    }

    uint64_t debugBufGva = workspaceGva_ + ZBAL_AICPU_DEBUG_BUF_OFFSET;

    /* Read header first */
    AicpuDebugHeaderHost header;
    ZResult ret = DlCannApi::AclrtMemcpy(&header, sizeof(header), reinterpret_cast<void *>(debugBufGva), sizeof(header),
                                         ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != Z_OK) {
        ZBAL_LOG_ERROR("AICPU DumpDebugBuffer: failed to read header from 0x" << std::hex << debugBufGva);
        return -1;
    }

    if (header.magic != AICPU_DEBUG_MAGIC_HOST) {
        ZBAL_LOG_WARN("AICPU debug buffer not written (magic=0x" << std::hex << header.magic << ", expected=0x"
                                                                 << AICPU_DEBUG_MAGIC_HOST << ")");
        return 0;
    }

    if (header.count == 0) {
        ZBAL_LOG_INFO("AICPU debug buffer empty (count=0)");
        return 0;
    }

    uint32_t maxEntries = (ZBAL_AICPU_DEBUG_BUF_SIZE - AICPU_DEBUG_HEADER_SIZE_HOST) / AICPU_DEBUG_ENTRY_SIZE_HOST;
    if (header.count > maxEntries) {
        ZBAL_LOG_WARN("AICPU debug buffer count clamped: " << header.count << " -> " << maxEntries);
        header.count = maxEntries;
    }

    /* Read entries */
    constexpr uint32_t kMaxEntries = 256;
    AicpuDebugEntryHost entries[kMaxEntries];
    uint32_t readCount = (header.count < kMaxEntries) ? header.count : kMaxEntries;
    uint64_t entriesGva = debugBufGva + AICPU_DEBUG_HEADER_SIZE_HOST;

    ret = DlCannApi::AclrtMemcpy(entries, readCount * sizeof(AicpuDebugEntryHost), reinterpret_cast<void *>(entriesGva),
                                 readCount * sizeof(AicpuDebugEntryHost), ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != Z_OK) {
        ZBAL_LOG_ERROR("AICPU DumpDebugBuffer: failed to read entries");
        return -1;
    }

    ZBAL_LOG_INFO("=== AICPU Debug Trace: " << header.count << " entries, seq=" << header.seq << " ===");

    for (uint32_t i = 0; i < readCount; i++) {
        uint32_t line = entries[i].info & 0xFFFFU;
        uint32_t seq = (entries[i].info >> 16) & 0xFFFFU;
        ZBAL_LOG_INFO("  [" << i << "] " << AicpuDebugTagName(entries[i].tag) << " tag=" << entries[i].tag
                            << " line=" << line << " seq=" << seq << " val0=0x" << std::hex << entries[i].val0
                            << std::dec << " val1=0x" << std::hex << entries[i].val1 << std::dec);
    }

    if (header.count > readCount) {
        ZBAL_LOG_INFO("  ... (" << (header.count - readCount) << " more entries truncated)");
    }

    return static_cast<int32_t>(header.count);
}

ZResult NpuAicpuLauncher::LoadKernelJson(const std::string &jsonPath)
{
    if (jsonPath.empty()) {
        ZBAL_LOG_ERROR("AICPU kernel JSON path is empty");
        return Z_FILE_NOT_FOUND;
    }

    /* Resolve canonical absolute path */
    char resolvedPath[PATH_MAX] = {0};
    if (::realpath(jsonPath.c_str(), resolvedPath) == nullptr) {
        ZBAL_LOG_ERROR("AICPU kernel JSON realpath failed: " << jsonPath << " errno=" << errno);
        return Z_FILE_NOT_FOUND;
    }

    if (::access(resolvedPath, F_OK) != 0) {
        ZBAL_LOG_ERROR("AICPU kernel JSON not accessible: " << resolvedPath);
        return Z_FILE_NOT_FOUND;
    }

    /* cpuKernelMode=0 for AICPUKernel (.so loaded by device-side runtime from tar.gz) */
    aclrtBinaryLoadOptions loadOptions{};
    aclrtBinaryLoadOption option{};
    option.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    constexpr uint32_t kAicpuKernelMode = 0U;
    option.value.cpuKernelMode = kAicpuKernelMode;
    loadOptions.numOpt = 1U;
    loadOptions.options = &option;

    aclrtBinHandle binHandle;
    int32_t aclRet = DlCannApi::AclrtBinaryLoadFromFile(resolvedPath, &loadOptions, &binHandle);
    if (aclRet != 0) {
        ZBAL_LOG_ERROR("DlCannApi::AclrtBinaryLoadFromFile failed, path=" << resolvedPath
                                                                          << ", ret=" << static_cast<int32_t>(aclRet));
        return Z_DL_FUNCTION_UNLOAD;
    }

    /* Resolve per-op kernel function handles for profiling visibility */
    struct OpKernelEntry {
        uint32_t commType;
        const char *name;
    };
    static const OpKernelEntry opKernels[] = {
        {ZBAL_CMD_ALLGATHER, "ZBALAicpuAllGather"},
        {ZBAL_CMD_SCATTER, "ZBALAicpuScatter"},
        {ZBAL_CMD_REDUCE_SCATTER, "ZBALAicpuReduceScatter"},
        {ZBAL_CMD_BROADCAST, "ZBALAicpuBroadcast"},
        {ZBAL_CMD_ALLREDUCE, "ZBALAicpuAllReduce"},
        {ZBAL_CMD_ALLTOALLV, "ZBALAicpuAlltoAllV"},
        {ZBAL_CMD_SEND, "ZBALAicpuSend"},
        {ZBAL_CMD_RECV, "ZBALAicpuRecv"},
    };

    for (uint32_t i = 0; i < sizeof(opKernels) / sizeof(opKernels[0]); i++) {
        aclrtFuncHandle opHandle = nullptr;
        aclRet = DlCannApi::AclrtBinaryGetFunction(binHandle, opKernels[i].name, &opHandle);
        if (aclRet != 0 || opHandle == nullptr) {
            ZBAL_LOG_ERROR("AclrtBinaryGetFunction for " << opKernels[i].name
                                                         << " failed, ret=" << static_cast<int32_t>(aclRet));
            DlCannApi::AclrtBinaryUnLoad(binHandle);
            return Z_DL_FUNCTION_UNLOAD;
        }
        opFuncHandles_[opKernels[i].commType] = opHandle;
    }

    kernelBinaryHandle_ = binHandle;

    ZBAL_LOG_INFO("Loaded AICPU dispatcher from JSON: " << resolvedPath);
    return Z_OK;
}

} // namespace operators
} // namespace zbal
