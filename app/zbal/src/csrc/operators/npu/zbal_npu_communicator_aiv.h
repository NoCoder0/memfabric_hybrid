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
#ifndef ZBAL_COMMUNICATOR_AIV_H
#define ZBAL_COMMUNICATOR_AIV_H

#include "zbal_npu_communicator_base.h"

namespace zbal {
namespace operators {

class NpuCommunicatorAIV : public NpuCommunicatorBase {
public:
    using NpuCommunicatorBase::NpuCommunicatorBase;

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
};

} // namespace operators
} // namespace zbal

#endif // ZBAL_COMMUNICATOR_AIV_H
