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
#include "zbal_npu_communicator_aicpu.h"
#include "zbal_npu_aicpu_launcher.h"
#include "zbal_comm_host_device_struct.h"
#include "dl_cann_api.h"
#include "zbal_init_state.h"

#include <cstdlib>

namespace zbal {
namespace operators {

using namespace underapi;

static std::string GetAicpuKernelPath()
{
    const char *envPath = std::getenv("ZBAL_AICPU_KERNEL_PATH");
    if (envPath != nullptr && envPath[0] != '\0') {
        return std::string(envPath);
    }

    const char *ascendPath = std::getenv("ASCEND_HOME_PATH");
    if (ascendPath != nullptr && ascendPath[0] != '\0') {
        return std::string(ascendPath) + "/opp/vendors/cust/aicpu/config/libzbal_aicpu_kernel.json";
    }

    return "libzbal_aicpu_kernel.json";
}

/* Workspace layout — see device-side zbal_aicpu_defines.h for full derivation */
/* Total workspace includes per-core SQE ring buffers for all 4 AICPU cores */
ZResult NpuCommunicatorAICPU::AllocateAicpuResources()
{
    /* Allocate workspace (~420KB for channel info + init context + debug buffer + per-core SQE rings) */
    ZResult ret = underapi::DlCannApi::AclrtMalloc(&aicpuWorkspacePtr_, ZBAL_AICPU_WORKSPACE_TOTAL_SIZE, 0);
    if (ret != Z_OK) {
        ZBAL_LOG_ERROR("Failed to allocate AICPU workspace, size=" << ZBAL_AICPU_WORKSPACE_TOTAL_SIZE);
        return ret;
    }

    ret = underapi::DlCannApi::AclrtMemset(aicpuWorkspacePtr_, ZBAL_AICPU_WORKSPACE_TOTAL_SIZE, 0,
                                           ZBAL_AICPU_WORKSPACE_TOTAL_SIZE);
    if (ret != Z_OK) {
        ZBAL_LOG_ERROR("Failed to zero AICPU workspace");
        underapi::DlCannApi::AclrtFree(aicpuWorkspacePtr_);
        aicpuWorkspacePtr_ = nullptr;
        return ret;
    }

    ZBAL_LOG_INFO("Allocated AICPU workspace: ptr=0x" << std::hex << reinterpret_cast<uintptr_t>(aicpuWorkspacePtr_)
                                                      << ", size=" << std::dec << ZBAL_AICPU_WORKSPACE_TOTAL_SIZE);
    return Z_OK;
}

ZResult NpuCommunicatorAICPU::Initialize() noexcept
{
    if (initialized_) {
        return Z_OK;
    }

    ZResult ret = NpuCommunicatorBase::Initialize();
    if (ret != Z_OK) {
        return ret;
    }

    ret = AllocateAicpuResources();
    if (ret != Z_OK) {
        return ret;
    }

    std::string kernelPath = GetAicpuKernelPath();

    ret = launcher_.Init(kernelPath, reinterpret_cast<uint64_t>(aicpuWorkspacePtr_), groupInfo_);
    if (ret != Z_OK) {
        return ret;
    }

    initialized_ = true;
    return Z_OK;
}

void NpuCommunicatorAICPU::UnInitialize() noexcept
{
    launcher_.Finalize();
    launcher_.Destroy();

    if (aicpuWorkspacePtr_ != nullptr) {
        underapi::DlCannApi::AclrtFree(aicpuWorkspacePtr_);
        aicpuWorkspacePtr_ = nullptr;
    }

    NpuCommunicatorBase::UnInitialize();
}

/* Helper: Build AicpuWorkDesc and launch kernel.
 * count is in ELEMENTS and auto-converted to bytes via dataType. */
static int32_t LaunchAicpuOp(NpuAicpuLauncher &launcher, uint32_t commType, uint64_t sendBuf, uint64_t recvBuf,
                             uint64_t count, uint32_t dataType, uint32_t root, const char *opName,
                             void *stream = nullptr, uint32_t commAlg = 0, uint32_t reduceOp = 0, uint64_t buffer = 0,
                             uint64_t reserved0 = 0, uint64_t reserved1 = 0, uint64_t reserved2 = 0)
{
    AicpuWorkDesc desc{};
    desc.commType = commType;
    desc.commAlg = commAlg;
    desc.sendBuffer = sendBuf;
    desc.recvBuffer = recvBuf;
    desc.buffer = buffer;
    desc.count = count * ZBALDataTypeSize(dataType); /* elements → bytes */
    desc.dataType = dataType;
    desc.root = root;
    desc.reduceOp = reduceOp;
    desc.reserved[0] = reserved0;
    desc.reserved[1] = reserved1;
    desc.reserved[2] = reserved2;

    ZResult zret = launcher.Launch(desc, stream);
    if (zret != Z_OK) {
        (void)launcher.SyncAndDumpDebug(stream);
        ZBAL_LOG_ERROR(opName << " Launch failed, ret=" << static_cast<int32_t>(zret));
        return 1;
    }
    return 0;
}

int32_t NpuCommunicatorAICPU::AllGather(const void *sendBuff, void *recvBuff, size_t sendCount,
                                        zbal_datatype_t dataType, aclrtStream stream) noexcept
{
    return LaunchAicpuOp(launcher_, ZBAL_CMD_ALLGATHER, reinterpret_cast<uint64_t>(sendBuff),
                         reinterpret_cast<uint64_t>(recvBuff), sendCount, static_cast<uint32_t>(dataType), 0,
                         "AllGather", stream);
}

int32_t NpuCommunicatorAICPU::AllReduce(const void *sendBuff, void *recvBuff, void *buffer, size_t count,
                                        zbal_datatype_t dataType, zbal_reduce_op_t reduceOp,
                                        aclrtStream stream) noexcept
{
    return LaunchAicpuOp(launcher_, ZBAL_CMD_ALLREDUCE, reinterpret_cast<uint64_t>(sendBuff),
                         reinterpret_cast<uint64_t>(recvBuff), count, static_cast<uint32_t>(dataType), 0, "AllReduce",
                         stream, 0, static_cast<uint32_t>(reduceOp), reinterpret_cast<uint64_t>(buffer));
}

int32_t NpuCommunicatorAICPU::ReduceScatter(const void *sendBuff, void *recvBuff, size_t recvCount,
                                            zbal_datatype_t dataType, zbal_reduce_op_t reduceOp,
                                            aclrtStream stream) noexcept
{
    uint64_t totalCount = static_cast<uint64_t>(recvCount) * GetMetaInfo().groupSize;
    return LaunchAicpuOp(launcher_, ZBAL_CMD_REDUCE_SCATTER, reinterpret_cast<uint64_t>(sendBuff),
                         reinterpret_cast<uint64_t>(recvBuff), totalCount, static_cast<uint32_t>(dataType), 0,
                         "ReduceScatter", stream, 0, static_cast<uint32_t>(reduceOp));
}

int32_t NpuCommunicatorAICPU::Scatter(const void *sendBuff, void *recvBuff, uint64_t dataCount,
                                      zbal_datatype_t dataType, uint16_t root, aclrtStream stream) noexcept
{
    return LaunchAicpuOp(launcher_, ZBAL_CMD_SCATTER, reinterpret_cast<uint64_t>(sendBuff),
                         reinterpret_cast<uint64_t>(recvBuff), dataCount, static_cast<uint32_t>(dataType),
                         static_cast<uint32_t>(root), "Scatter", stream);
}

int32_t NpuCommunicatorAICPU::Broadcast(const void *buf, uint64_t dataCount, zbal_datatype_t dataType, uint16_t root,
                                        aclrtStream stream) noexcept
{
    uint64_t bufGva = reinterpret_cast<uint64_t>(buf);
    return LaunchAicpuOp(launcher_, ZBAL_CMD_BROADCAST, bufGva, bufGva, dataCount, static_cast<uint32_t>(dataType),
                         static_cast<uint32_t>(root), "Broadcast", stream);
}

int32_t NpuCommunicatorAICPU::AlltoAllV(const void *sendBuff, void *recvBuff, void *sendCumSum, void *recvSplitCounts,
                                        void *elements, zbal_datatype_t dataType, aclrtStream stream) noexcept
{
    return LaunchAicpuOp(launcher_, ZBAL_CMD_ALLTOALLV, reinterpret_cast<uint64_t>(sendBuff),
                         reinterpret_cast<uint64_t>(recvBuff), 0, static_cast<uint32_t>(dataType), 0, "AlltoAllV",
                         stream, 0, 0, 0, reinterpret_cast<uint64_t>(sendCumSum),
                         reinterpret_cast<uint64_t>(recvSplitCounts), reinterpret_cast<uint64_t>(elements));
}

int32_t NpuCommunicatorAICPU::Send(const void *sendBuff, zbal_datatype_t dataType, uint32_t peer,
                                   aclrtStream stream) noexcept
{
    return LaunchAicpuOp(launcher_, ZBAL_CMD_SEND, reinterpret_cast<uint64_t>(sendBuff), 0, 0,
                         static_cast<uint32_t>(dataType), peer, "Send", stream);
}

int32_t NpuCommunicatorAICPU::Recv(const void *recvBuff, size_t recvCount, zbal_datatype_t dataType, uint32_t peer,
                                   aclrtStream stream) noexcept
{
    uint64_t recvBufGva = reinterpret_cast<uint64_t>(recvBuff);
    return LaunchAicpuOp(launcher_, ZBAL_CMD_RECV, recvBufGva, recvBufGva, recvCount, static_cast<uint32_t>(dataType),
                         peer, "Recv", stream);
}
} // namespace operators
} // namespace zbal
