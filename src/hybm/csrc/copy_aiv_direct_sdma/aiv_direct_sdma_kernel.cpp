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
#include "acl/acl.h"
#include "kernel_operator.h"
#include "aiv_direct_sdma_abi.h"
using namespace AscendC;
extern "C" uint32_t hybm_aiv_direct_sdma_abi_version()
{
    return AIV_DIRECT_SDMA_ABI_VERSION;
}
#define INLINE __aicore__ inline __attribute__((always_inline))
INLINE void Clean(__gm__ uint8_t *ptr)
{
    GlobalTensor<uint8_t> t;
    t.SetGlobalBuffer(ptr);
    __asm__ __volatile__("" ::: "memory");
    DataCacheCleanAndInvalid<uint8_t, CacheLine::SINGLE_CACHE_LINE, DcciDst::CACHELINE_OUT>(t);
    __asm__ __volatile__("" ::: "memory");
}
INLINE void Store(__gm__ uint8_t *ptr, uint32_t value, LocalTensor<uint32_t> &ub)
{
    GlobalTensor<uint32_t> t;
    t.SetGlobalBuffer((__gm__ uint32_t *)ptr);
    ub.SetValue(0, value);
    SetFlag<HardEvent::S_MTE3>(EVENT_ID0);
    WaitFlag<HardEvent::S_MTE3>(EVENT_ID0);
    DataCopyExtParams p{1, 4, 0, 0, 0};
    DataCopyPad(t, ub, p);
    SetFlag<HardEvent::MTE3_S>(EVENT_ID0);
    WaitFlag<HardEvent::MTE3_S>(EVENT_ID0);
}
INLINE void ZeroSqe(__gm__ uint8_t *ptr)
{
    auto words = (__gm__ uint64_t *)ptr;
    for (uint32_t j = 0; j < 8; ++j)
        words[j] = 0;
}
INLINE void WriteDataSqe(__gm__ StarsQueueInfo *queue, uint32_t tail, uint64_t src, uint64_t dst, uint32_t bytes)
{
    auto sqe = (__gm__ stars_sdma_sqe_t *)queue->sq_base + tail;
    ZeroSqe((__gm__ uint8_t *)sqe);
    sqe->header.type = 11;
    sqe->header.rt_streamid = queue->stream_id;
    sqe->header.task_id = tail;
    sqe->kernel_credit = AIV_DIRECT_SDMA_DATA_KERNEL_CREDIT;
    sqe->sssv = 1;
    sqe->dssv = 1;
    sqe->sns = 1;
    sqe->dns = 1;
    sqe->qos = 6;
    sqe->length = bytes;
    sqe->src_addr_low = src;
    sqe->src_addr_high = src >> 32;
    sqe->dst_addr_low = dst;
    sqe->dst_addr_high = dst >> 32;
    sqe->link_type = 255;
    Clean((__gm__ uint8_t *)sqe);
}
extern "C" __global__ __aicore__ void aiv_direct_sdma_kernel(GM_ADDR workspace, GM_ADDR descriptors, uint32_t count,
                                                             GM_ADDR consumed)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    const uint32_t workerIndex = GetBlockIdx(), workerCount = GetBlockNum();
    auto queue = (__gm__ StarsQueueInfo *)(workspace + AIV_DIRECT_SDMA_QUEUE_INFO_OFFSET) + workerIndex;
    Clean((__gm__ uint8_t *)queue);
    uint32_t tail = queue->sq_tail;
    auto input = (__gm__ AivDirectSdmaDescriptor *)descriptors;
    // Stream Notify waits guarantee the previous wave has drained. Host bounds this
    // wave to depth-2 data entries per AIV worker, leaving notify + one empty slot.
    for (uint32_t i = workerIndex; i < count; i += workerCount) {
        // Descriptors are host-written mapped memory; invalidate reused cache lines.
        Clean((__gm__ uint8_t *)&input[i]);
        Clean((__gm__ uint8_t *)&input[i] + 16);
        uint64_t src = input[i].src, dst = input[i].dst;
        uint32_t bytes = static_cast<uint32_t>(input[i].bytes);
        WriteDataSqe(queue, tail, src, dst, bytes);
        tail = (tail + 1) % queue->sq_depth;
    }
    auto notify = (__gm__ stars_notify_sqe_t *)queue->sq_base + tail;
    ZeroSqe((__gm__ uint8_t *)notify);
    notify->header.type = 6;
    notify->header.rt_streamid = queue->stream_id;
    notify->header.task_id = tail;
    notify->notify_id = ((__gm__ uint32_t *)(workspace + AIV_DIRECT_SDMA_NOTIFY_ID_OFFSET))[workerIndex];
    notify->kernel_credit = AIV_DIRECT_SDMA_NOTIFY_KERNEL_CREDIT;
    Clean((__gm__ uint8_t *)notify);
    tail = (tail + 1) % queue->sq_depth;
    PipeBarrier<PIPE_ALL>();
    LocalTensor<uint32_t> ub;
    ub.address_.logicPos = static_cast<uint8_t>(TPosition::VECOUT);
    ub.address_.bufferAddr = 0;
    ub.address_.dataLen = 64;
    Store((__gm__ uint8_t *)queue->sq_reg_base + AIV_DIRECT_SDMA_DOORBELL_TAIL_OFFSET, tail, ub);
    Store((__gm__ uint8_t *)queue + 4, tail, ub);
    // Per-producer cache line: host can reclaim descriptors independently of payload.
    Store(consumed + workerIndex * AIV_DIRECT_SDMA_CONSUMED_STRIDE, 1, ub);
}
extern "C" int hybm_aiv_direct_sdma_launch(void *workspace, void *descriptors, uint32_t count, void *consumed,
                                           uint32_t aivCount, void *stream)
{
    aiv_direct_sdma_kernel<<<aivCount, nullptr, stream>>>((uint8_t *)workspace, (uint8_t *)descriptors, count,
                                                          (uint8_t *)consumed);
    return static_cast<int>(aclrtGetLastError(ACL_RT_THREAD_LEVEL));
}
