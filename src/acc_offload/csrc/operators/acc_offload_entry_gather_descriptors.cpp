/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * http://license.coscl.org.cn/MulanPSL2
 */
#include "kernel_operator.h"

extern "C" __global__ __aicore__ void OffloadEntryGatherDescriptorsOps(GM_ADDR ids, GM_ADDR count, GM_ADDR sources,
                                                                       GM_ADDR destinations, GM_ADDR lengths,
                                                                       uint64_t dst, uint64_t poolGva,
                                                                       uint64_t slotStride, uint32_t entryBytes,
                                                                       uint32_t rowsPerSlot, uint32_t capacity)
{
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_AIV_ONLY);
    const auto active = *reinterpret_cast<__gm__ uint32_t *>(count);
    if (active == 0 || active > capacity) {
        return;
    }
    constexpr uint32_t tile = 32;
    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::TPosition::VECCALC> buffer;
    pipe.InitBuffer(buffer, tile * (2 * sizeof(uint64_t) + sizeof(uint32_t)));
    auto srcLocal = buffer.Get<uint64_t>();
    auto dstLocal = srcLocal[tile];
    auto lenLocal = buffer.Get<uint32_t>()[tile * 4];
    auto bytes = buffer.Get<uint8_t>();
    AscendC::GlobalTensor<uint8_t> srcGm, dstGm, lenGm;
    srcGm.SetGlobalBuffer(sources);
    dstGm.SetGlobalBuffer(destinations);
    lenGm.SetGlobalBuffer(lengths);
    for (uint32_t start = AscendC::GetBlockIdx() * tile; start < active; start += AscendC::GetBlockNum() * tile) {
        const auto size = active - start < tile ? active - start : tile;
        for (uint32_t i = 0; i < size; ++i) {
            const auto id = static_cast<uint32_t>(reinterpret_cast<__gm__ uint64_t *>(ids)[start + i]);
            const auto slot = id / rowsPerSlot;
            const auto row = id - slot * rowsPerSlot;
            srcLocal.SetValue(i, poolGva + static_cast<uint64_t>(slot) * slotStride +
                                     static_cast<uint64_t>(row) * entryBytes);
            dstLocal.SetValue(i, dst + static_cast<uint64_t>(start + i) * entryBytes);
            lenLocal.SetValue(i, entryBytes);
        }
        AscendC::SetFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
        AscendC::WaitFlag<AscendC::HardEvent::S_MTE3>(EVENT_ID0);
        AscendC::DataCopyExtParams addresses{1, size * static_cast<uint32_t>(sizeof(uint64_t)), 0, 0, 0};
        AscendC::DataCopyExtParams sizes{1, size * static_cast<uint32_t>(sizeof(uint32_t)), 0, 0, 0};
        AscendC::DataCopyPad(srcGm[start * sizeof(uint64_t)], bytes, addresses);
        AscendC::DataCopyPad(dstGm[start * sizeof(uint64_t)], bytes[tile * sizeof(uint64_t)], addresses);
        AscendC::DataCopyPad(lenGm[start * sizeof(uint32_t)], bytes[tile * 2 * sizeof(uint64_t)], sizes);
        AscendC::SetFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
        AscendC::WaitFlag<AscendC::HardEvent::MTE3_S>(EVENT_ID0);
    }
    AscendC::PipeBarrier<PIPE_ALL>();
}

extern "C" void OffloadOpsEntryGatherDescriptors(uint64_t dst, uint64_t ids, uint64_t count, uint64_t *sources,
                                                 uint64_t *destinations, uint32_t *lengths, uint32_t capacity,
                                                 uint64_t poolGva, uint64_t slotStride, uint32_t entryBytes,
                                                 uint32_t rowsPerSlot, void *stream)
{
    constexpr uint32_t blockDim = 32;
    OffloadEntryGatherDescriptorsOps<<<blockDim, nullptr, stream>>>(
        reinterpret_cast<uint8_t *>(ids), reinterpret_cast<uint8_t *>(count), reinterpret_cast<uint8_t *>(sources),
        reinterpret_cast<uint8_t *>(destinations), reinterpret_cast<uint8_t *>(lengths), dst, poolGva, slotStride,
        entryBytes, rowsPerSlot, capacity);
}
