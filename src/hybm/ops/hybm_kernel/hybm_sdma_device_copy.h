/* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 */
#ifndef MF_HYBM_SDMA_DEVICE_COPY_H
#define MF_HYBM_SDMA_DEVICE_COPY_H
#include <cstdint>
#include <cstddef>
#include <type_traits>

#include "hybm_device_copy_common.h"
constexpr uint32_t HYBM_SDMA_BATCH_SIZE = 256;
constexpr uint32_t HYBM_SDMA_MAX_COPY_BYTES = 32 * 1024 * 1024;

// aclnnShmemSdmaStarsQuery places the channel table after a 64-byte header.
constexpr uint32_t HYBM_SDMA_CHANNEL_TABLE_OFFSET = 64;
struct HybmSdmaChannel {
    uint32_t sqHead, sqTail;
    uint64_t sqBase, sqRegBase;
    uint32_t sqDepth, sqId, cqId, logicCqId;
    uint64_t cqeAddr;
    uint32_t reportCqeNum, streamId, devId;
    uint8_t reserved[4];
};
struct HybmSdmaSqe {
    uint32_t words[16];
};
static_assert(sizeof(HybmSdmaChannel) == 64 && offsetof(HybmSdmaChannel, sqDepth) == 24, "query channel ABI");
static_assert(sizeof(HybmSdmaSqe) == 64, "STARS SQE ABI");

inline HybmSdmaSqe HybmSdmaMemcpySqe(uint32_t streamId, uint64_t src, uint64_t dst, uint32_t bytes)
{
    HybmSdmaSqe sqe{};
    sqe.words[0] = 11;
    sqe.words[1] = streamId;
    sqe.words[3] = 240U << 16U;
    // A3 SDMA: sssv/dssv/sns/dns, qos=6, sro/dro; matches the zbal device SQE ABI.
    sqe.words[4] = (15U << 9U) | (6U << 13U) | (3U << 17U);
    sqe.words[7] = bytes;
    sqe.words[8] = static_cast<uint32_t>(src);
    sqe.words[9] = static_cast<uint32_t>(src >> 32U);
    sqe.words[10] = static_cast<uint32_t>(dst);
    sqe.words[11] = static_cast<uint32_t>(dst >> 32U);
    sqe.words[12] = 255;
    return sqe;
}
inline HybmSdmaSqe HybmSdmaNotifySqe(uint32_t streamId, uint32_t notifyId)
{
    HybmSdmaSqe sqe{};
    sqe.words[0] = 6;
    sqe.words[1] = streamId;
    sqe.words[2] = notifyId;
    sqe.words[3] = 254U << 16U;
    return sqe;
}

struct alignas(128) HybmSdmaDeviceCopyContext {
    HybmSdmaChannel channel;
    uint64_t poolBase, devicePoolBase, poolSize;
    uint32_t notifyId;
    uint32_t faulted;
    uint64_t sequence;
    // DMA writes must not share a cache line with AICPU-owned mutable state.
    alignas(128) uint64_t completed;
    alignas(128) uint64_t sentinel;
    alignas(128) HybmSdmaSqe staging[HYBM_SDMA_BATCH_SIZE + 2];
};
struct HybmSdmaDeviceCopyArgs {
    HybmSdmaDeviceCopyContext *context;
    const uint64_t *srcPtrs;
    const uint64_t *dstPtrs;
    const uint32_t *lengths;
    const uint32_t *count;
};
static_assert(std::is_trivially_copyable<HybmSdmaDeviceCopyContext>::value, "device context ABI");
extern "C" int32_t HybmSdmaDeviceCopy(HybmSdmaDeviceCopyArgs *args);
#endif
