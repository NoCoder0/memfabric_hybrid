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
#include "zbal_npu_communicator_aiv.h"
#include "zbal_npu_operators.h"

namespace zbal {
namespace operators {

int32_t NpuCommunicatorAIV::AllGather(const void *sendBuff, void *recvBuff, size_t sendCount, zbal_datatype_t dataType,
                                      aclrtStream stream) noexcept
{
    return ZBALOpAllGather(sendBuff, recvBuff, sendCount, dataType, stream, const_cast<CommGroupInfo &>(GetMetaInfo()));
}

int32_t NpuCommunicatorAIV::AllReduce(const void *sendBuff, void *recvBuff, void *buffer, size_t count,
                                      zbal_datatype_t dataType, zbal_reduce_op_t reduceOp, aclrtStream stream) noexcept
{
    return ZBALOpAllReduce(sendBuff, recvBuff, buffer, count, dataType, stream, reduceOp,
                           const_cast<CommGroupInfo &>(GetMetaInfo()));
}

int32_t NpuCommunicatorAIV::ReduceScatter(const void *sendBuff, void *recvBuff, size_t recvCount,
                                          zbal_datatype_t dataType, zbal_reduce_op_t reduceOp,
                                          aclrtStream stream) noexcept
{
    return ZBALOpReduceScatter(sendBuff, recvBuff, recvCount, dataType, stream, reduceOp,
                               const_cast<CommGroupInfo &>(GetMetaInfo()));
}

int32_t NpuCommunicatorAIV::Scatter(const void *sendBuff, void *recvBuff, uint64_t dataCount, zbal_datatype_t dataType,
                                    uint16_t root, aclrtStream stream) noexcept
{
    return ZBALOpScatter(sendBuff, recvBuff, static_cast<size_t>(dataCount), dataType, root, stream,
                         const_cast<CommGroupInfo &>(GetMetaInfo()));
}

int32_t NpuCommunicatorAIV::Broadcast(const void *buf, uint64_t dataCount, zbal_datatype_t dataType, uint16_t root,
                                      aclrtStream stream) noexcept
{
    return ZBALOpBroadcast(buf, static_cast<size_t>(dataCount), dataType, root, stream,
                           const_cast<CommGroupInfo &>(GetMetaInfo()));
}

int32_t NpuCommunicatorAIV::AlltoAllV(const void *sendBuff, void *recvBuff, void *sendCumSum, void *recvSplitCounts,
                                      void *elements, zbal_datatype_t dataType, aclrtStream stream) noexcept
{
    return ZBALOpAlltoAllV(sendBuff, recvBuff, sendCumSum, recvSplitCounts, elements, dataType, stream,
                           const_cast<CommGroupInfo &>(GetMetaInfo()));
}

int32_t NpuCommunicatorAIV::Send(const void *sendBuff, zbal_datatype_t dataType, uint32_t peer,
                                 aclrtStream stream) noexcept
{
    return ZBALOpSend(sendBuff, dataType, peer, stream, const_cast<CommGroupInfo &>(GetMetaInfo()));
}

int32_t NpuCommunicatorAIV::Recv(const void *recvBuff, size_t recvCount, zbal_datatype_t dataType, uint32_t peer,
                                 aclrtStream stream) noexcept
{
    return ZBALOpRecv(recvBuff, recvCount, dataType, peer, stream, const_cast<CommGroupInfo &>(GetMetaInfo()));
}
} // namespace operators
} // namespace zbal
