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
#ifndef ZBAL_NPU_COMMUNICATOR_AICPU_H
#define ZBAL_NPU_COMMUNICATOR_AICPU_H

#include "zbal_npu_communicator_base.h"
#include "zbal_npu_aicpu_launcher.h"

namespace zbal {
namespace operators {

class NpuCommunicatorAICPU : public NpuCommunicatorBase {
public:
    using NpuCommunicatorBase::NpuCommunicatorBase;

    ~NpuCommunicatorAICPU() override
    {
        /* Must call here: virtual dispatch from ~NpuCommunicatorBase resolves to the
         * base version, skipping AICPU resource cleanup (workspace/binary/cclBuf).
         * Idempotent when UnInitialize was already called explicitly. */
        UnInitialize();
    }

    ZResult Initialize() noexcept override;
    void UnInitialize() noexcept override;

    int32_t AllGather(const void *sendBuff, void *recvBuff, size_t sendCount, zbal_datatype_t dataType,
                      aclrtStream stream) noexcept override;
    int32_t AllReduce(const void *sendBuff, void *recvBuff, void *buffer, size_t count, zbal_datatype_t dataType,
                      zbal_reduce_op_t reduceOp, aclrtStream stream) noexcept override;
    int32_t ReduceScatter(const void *sendBuff, void *recvBuff, size_t recvCount, zbal_datatype_t dataType,
                          zbal_reduce_op_t reduceOp, aclrtStream stream) noexcept override;
    int32_t Scatter(const void *sendBuff, void *recvBuff, uint64_t dataCount, zbal_datatype_t dataType, uint16_t root,
                    aclrtStream stream) noexcept override;
    int32_t Broadcast(const void *buf, uint64_t dataCount, zbal_datatype_t dataType, uint16_t root,
                      aclrtStream stream) noexcept override;
    int32_t AlltoAllV(const void *sendBuff, void *recvBuff, void *sendCumSum, void *recvSplitCounts, void *elements,
                      zbal_datatype_t dataType, aclrtStream stream) noexcept override;
    int32_t Send(const void *sendBuff, zbal_datatype_t dataType, uint32_t peer, aclrtStream stream) noexcept override;
    int32_t Recv(const void *recvBuff, size_t recvCount, zbal_datatype_t dataType, uint32_t peer,
                 aclrtStream stream) noexcept override;

private:
    int32_t GatherImpl(const GatherParams &params) noexcept override;
    ZResult AllocateAicpuResources();
    uint64_t CalcPeerExchangeGva(uint32_t peer) const noexcept;

    NpuAicpuLauncher launcher_;
    void *aicpuWorkspacePtr_ = nullptr;
    void *reduceScatterCclBuf_ = nullptr; /* lazy-allocated 256M GVA scratch for DOUBLE_RING */
    size_t reduceScatterCclBufSize_ = 0;
    int32_t reduceScatterCclBufDevice_ = -1;
};

} // namespace operators
} // namespace zbal

#endif // ZBAL_NPU_COMMUNICATOR_AICPU_H
