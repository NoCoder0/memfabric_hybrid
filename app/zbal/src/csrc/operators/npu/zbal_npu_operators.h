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
#ifndef ZBAL_NPU_OPERATORS_H
#define ZBAL_NPU_OPERATORS_H

#include "zbal.h"
#include "zbal_communicator.h"
#include "zbal_defines.h"
#include "zbal_logger.h"

int32_t ZBALOpAllGather(const void *sendBuff, void *recvBuff, size_t sendCount, zbal_datatype_t dataType,
                        aclrtStream stream, CommGroupInfo &groupInfo);

int32_t ZBALOpReduceScatter(const void *inp, void *out, size_t recvNumel, zbal_datatype_t dataType, aclrtStream stream,
                            zbal_reduce_op_t reduceOp, CommGroupInfo &groupInfo);

int32_t ZBALOpAllReduce(const void *inp, void *out, void *buf, size_t numel, zbal_datatype_t dataType,
                        aclrtStream stream, zbal_reduce_op_t reduceOp, CommGroupInfo &groupInfo);

int32_t ZBALOpAlltoAllV(const void *sendBuff, void *recvBuff, void *sendCumSum, void *recvSplitCounts, void *elements,
                        zbal_datatype_t dataType, aclrtStream stream, CommGroupInfo &groupInfo);

int32_t ZBALOpBroadcast(const void *buff, size_t sendCount, zbal_datatype_t dataType, uint16_t root, aclrtStream stream,
                        CommGroupInfo &groupInfo);

int32_t ZBALOpGather(const void *sendBuff, void *recvBuff, size_t sendCount, zbal_datatype_t dataType, uint16_t root,
                     aclrtStream stream, CommGroupInfo &groupInfo);

int32_t ZBALOpScatter(const void *sendBuff, void *recvBuff, size_t sendCount, zbal_datatype_t dataType, uint16_t root,
                      aclrtStream stream, CommGroupInfo &groupInfo);

int32_t ZBALOpBarrier(aclrtStream stream, CommGroupInfo &groupInfo);

int32_t ZBALOpSend(const void *sendBuff, zbal_datatype_t dataType, uint32_t peer, aclrtStream stream,
                   CommGroupInfo &groupInfo);

int32_t ZBALOpRecv(const void *recvBuff, size_t recvCount, zbal_datatype_t dataType, uint32_t peer, aclrtStream stream,
                   CommGroupInfo &groupInfo);

int32_t ZBALOpDispatchLayout(const zbal_tensor_info_t *topkIndex, int64_t tokens, int64_t expertNum, int64_t topkNum,
                             const zbal_tensor_info_t *tokensPerRank, const zbal_tensor_info_t *tokensPerExpert,
                             const zbal_tensor_info_t *sendTokensIndex, const zbal_tensor_info_t *notifySendData,
                             aclrtStream stream, const CommGroupInfo &groupInfo, int64_t flags);

int32_t ZBALOpNotifyDispatch(const zbal_tensor_info_t *sendTokensPerExpert, int64_t sendCount, int64_t topKNum,
                             const zbal_tensor_info_t *recvBuff, const zbal_tensor_info_t *totalRecvTokens,
                             const zbal_tensor_info_t *recvTokensPerExpert, const zbal_tensor_info_t *pushTargetOffset,
                             const zbal_tensor_info_t *balanceMatrix, float factorHigh, float factorLow,
                             aclrtStream stream, const CommGroupInfo &groupInfo, int64_t flags);

int32_t ZBALOpDispatchNormal(const zbal_tensor_info_t *srcTokens, const zbal_tensor_info_t *topkIndex,
                             const zbal_tensor_info_t *sendTokensIndex, const zbal_tensor_info_t *pushTargetOffset,
                             const zbal_tensor_info_t *balanceMatrix, int64_t expertNum, zbal_quant_mode_t quantMode,
                             const zbal_tensor_info_t *destTokens, const zbal_tensor_info_t *destScale,
                             bool enableBalance, aclrtStream stream, const CommGroupInfo &groupInfo, int64_t flags);

int32_t ZBALOpCombineNormal(const zbal_tensor_info_t *srcTokens, const zbal_tensor_info_t *putOffset,
                            const zbal_tensor_info_t *topKWeight, const zbal_tensor_info_t *topkIndex,
                            const zbal_tensor_info_t *sendTokensIndex, const zbal_tensor_info_t *balanceMatrix,
                            uint16_t expertNum, const zbal_tensor_info_t *destTokens, bool enableBalance,
                            aclrtStream stream, const CommGroupInfo &groupInfo, int64_t flags);

int32_t ZBALOpDispatchLowLatency(const zbal_tensor_info_t *x, const zbal_tensor_info_t *expertIds, int64_t moeExpertNum,
                                 int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode,
                                 int64_t globalBs, int64_t magicVal, int64_t expertTokenNumsType,
                                 const zbal_tensor_info_t *expandXOut, const zbal_tensor_info_t *dynamicScalesOut,
                                 const zbal_tensor_info_t *expandIdxOut, const zbal_tensor_info_t *expertTokenNumsOut,
                                 const zbal_tensor_info_t *epRecvCountsOut, const zbal_tensor_info_t *putOffset,
                                 const zbal_tensor_info_t *putOffsetStatus, aclrtStream stream,
                                 const CommGroupInfo &groupInfo, int64_t flags);

int32_t ZBALOpCombineLowLatency(const zbal_tensor_info_t *expandX, const zbal_tensor_info_t *expertIds,
                                const zbal_tensor_info_t *expertIdx, const zbal_tensor_info_t *epSendCounts,
                                const zbal_tensor_info_t *expertScales, const zbal_tensor_info_t *xOut,
                                int64_t moeExpertNum, aclrtStream stream, const CommGroupInfo &groupInfo,
                                int64_t flags);

#if defined(ZBAL_ASCEND_NPU_A3) && defined(ZBAL_FUSED_DEEP_MOE_ENABLED)
int32_t ZBALOpFusedDeepMoe(const zbal_tensor_info_t *x, const zbal_tensor_info_t *expertIds,
                           const zbal_tensor_info_t *gmm1Weight, const zbal_tensor_info_t *gmm1Scale,
                           const zbal_tensor_info_t *gmm2Weight, const zbal_tensor_info_t *gmm2Scale,
                           const zbal_tensor_info_t *expertScales, const zbal_tensor_info_t *expertSmoothScales,
                           const zbal_tensor_info_t *shareGmm1Weight, const zbal_tensor_info_t *shareGmm1Scale,
                           const zbal_tensor_info_t *shareGmm2Weight, const zbal_tensor_info_t *shareGmm2Scale,
                           const zbal_tensor_info_t *shareSmoothScales, const zbal_tensor_info_t *xActiveMask,
                           const zbal_tensor_info_t *output, const zbal_tensor_info_t *shareOutput,
                           const zbal_tensor_info_t *expertTokenNums, const zbal_tensor_info_t *workspace,
                           int64_t moeExpertNum, int64_t quantMode, int64_t globalBs, int64_t gmm1HLen,
                           int64_t shareGmm1HLen, bool isTensorList, void *tilingDevBuf, const std::string &groupName,
                           aclrtStream stream, const CommGroupInfo &epGroupInfo, const CommGroupInfo &tpGroupInfo,
                           int64_t flags, bool needTilingCopy = true);
#endif // ZBAL_ASCEND_NPU_A3 && ZBAL_FUSED_DEEP_MOE_ENABLED

// When an operator group is filtered out (see ZBAL_OP_BUILD_FILTER), its device launcher
// declared above is not built. Redirect the host call sites to an inline error stub so the
// host objects emit no undefined reference to the missing group.
namespace zbal {
namespace operators {
namespace detail {

inline int32_t ZBALOpUnavailable(const char *op, const char *group) noexcept
{
    ZBAL_LOG_ERROR(op << " is unavailable: operator group '" << group << "' was not built (add it via --build-filter)");
    return Z_ERROR;
}

} // namespace detail
} // namespace operators
} // namespace zbal

#define ZBAL_OP_MISSING(op, group) ::zbal::operators::detail::ZBALOpUnavailable(op, group)

#if !defined(ZBAL_OP_CLASSIC_ENABLED)
#define ZBALOpAllGather(...)     ZBAL_OP_MISSING("AllGather", "classic")
#define ZBALOpReduceScatter(...) ZBAL_OP_MISSING("ReduceScatter", "classic")
#define ZBALOpAllReduce(...)     ZBAL_OP_MISSING("AllReduce", "classic")
#define ZBALOpAlltoAllV(...)     ZBAL_OP_MISSING("AlltoAllV", "classic")
#define ZBALOpBroadcast(...)     ZBAL_OP_MISSING("Broadcast", "classic")
#define ZBALOpGather(...)        ZBAL_OP_MISSING("Gather", "classic")
#define ZBALOpScatter(...)       ZBAL_OP_MISSING("Scatter", "classic")
#define ZBALOpBarrier(...)       ZBAL_OP_MISSING("Barrier", "classic")
#define ZBALOpSend(...)          ZBAL_OP_MISSING("Send", "classic")
#define ZBALOpRecv(...)          ZBAL_OP_MISSING("Recv", "classic")
#endif

#if !defined(ZBAL_OP_DISPATCH_NORMAL_ENABLED)
#define ZBALOpDispatchLayout(...) ZBAL_OP_MISSING("DispatchLayout", "dispatch_combine_normal")
#define ZBALOpNotifyDispatch(...) ZBAL_OP_MISSING("NotifyDispatch", "dispatch_combine_normal")
#define ZBALOpDispatchNormal(...) ZBAL_OP_MISSING("DispatchNormal", "dispatch_combine_normal")
#define ZBALOpCombineNormal(...)  ZBAL_OP_MISSING("CombineNormal", "dispatch_combine_normal")
#endif

#if !defined(ZBAL_OP_DISPATCH_LOW_LATENCY_ENABLED)
#define ZBALOpDispatchLowLatency(...) ZBAL_OP_MISSING("DispatchLowLatency", "dispatch_combine_low_latency")
#define ZBALOpCombineLowLatency(...)  ZBAL_OP_MISSING("CombineLowLatency", "dispatch_combine_low_latency")
#endif

#endif // ZBAL_NPU_OPERATORS_H
