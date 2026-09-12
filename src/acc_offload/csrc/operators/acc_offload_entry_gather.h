/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the Mulan PSL v2 for more details.
 */

#ifndef ACC_OFFLOAD_ENTRY_GATHER_H
#define ACC_OFFLOAD_ENTRY_GATHER_H

#include "kernel_operator.h"

#define HYBM_AICORE_KERNEL __attribute__((always_inline)) __aicore__ __inline__

/* Random-entry gather over one uniform row grid: for a GLOBAL row id,
 *   src = poolGva + (id / rowsPerSlot) * slotStride + (id % rowsPerSlot) * entryBytes
 * id is the global row number of the whole grid (< 2^32); callers with several
 * same-width tables fold their per-segment offsets into the id space.
 * entryBytes/rowsPerSlot are registered once (offload_register_entry_table). */
/* SYMMETRIC GVA contract: the row grid starts at the SAME GVA offset (the slot
 * start) inside every slot, so row addresses across ranks differ by exact
 * multiples of slotStride and every slot's byte layout is identical. The SHARED
 * vmm pool provides the symmetric base; every rank registers the identical
 * layout. This lets the kernel resolve any row on any rank from the row id
 * alone — no world size, no rank ids, no per-rank tables. */
/* Per-vector-core UB: A5 256KB, A3 192KB; TPipe reserves a small system area,
 * so the safe ceiling is 240KB on A5 / 176KB on A3 (ACC_SOC_VERSION_Ax from
 * the kernel cmake). Split into two equal ping-pong slots. */
#if defined(ACC_SOC_VERSION_A5)
constexpr int64_t ENTRY_GATHER_UB_ONCE_SIZE = 240 * 1024;
#else
constexpr int64_t ENTRY_GATHER_UB_ONCE_SIZE = 176 * 1024;
#endif

/*
 * Fused address-computation + copy kernel for random entry reads. No
 * (src, dst, len) table is built on the host: every AIV core reads its share
 * of the device-resident int64 global row ids, maps id -> (slot, in-slot row)
 * with one 32-bit division and copies the fixed entryBytes row through a
 * two-slot ping-pong pipeline. The registered layout is baked into the launch
 * (graph-capture safe); the entry count is read from GM, never a host scalar.
 * Every id GM read is a scalar read; never stage 64-bit GM values through UB
 * (LocalTensor::GetValue corrupts them on A5).
 */
class OffloadEntryGatherKernel {
public:
    HYBM_AICORE_KERNEL OffloadEntryGatherKernel() {}

    HYBM_AICORE_KERNEL void Init(GM_ADDR ids, GM_ADDR dst, GM_ADDR count, uint64_t poolGva, uint64_t slotStride,
                                 uint32_t entryBytes, uint32_t rowsPerSlot)
    {
        aivNum_ = AscendC::GetBlockNum();
        aivIndex_ = AscendC::GetBlockIdx();
        uint32_t idsCount = *(reinterpret_cast<__gm__ uint32_t *>(count));
        /* no batch cap: any uint32 count is executable (0 gathers nothing) */
        count_ = idsCount;
        ids_ = reinterpret_cast<__gm__ uint64_t *>(ids);
        dstBase_ = reinterpret_cast<__gm__ uint8_t *>(dst);
        poolGva_ = poolGva;
        slotStride_ = slotStride;
        entryBytes_ = entryBytes;
        rowsPerSlot_ = rowsPerSlot;
        pipe_.InitBuffer(queA_, 1, SLOT_BYTES);
        pipe_.InitBuffer(queB_, 1, SLOT_BYTES);
    }

    HYBM_AICORE_KERNEL void Process()
    {
        if (count_ == 0) {
            return;
        }
        for (uint32_t i = aivIndex_; i < count_; i += aivNum_) {
            /* one 32-bit division splits the global id into (slot, in-slot
             * row); the slot IS the rank (symmetric grid), so this GVA needs
             * no rank-specific data */
            const uint32_t id = static_cast<uint32_t>(ids_[i]);
            const uint32_t slot = id / rowsPerSlot_;
            const uint32_t row = id - slot * rowsPerSlot_;
            const uint64_t src =
                poolGva_ + static_cast<uint64_t>(slot) * slotStride_ + static_cast<uint64_t>(row) * entryBytes_;
            __gm__ uint8_t *dst = dstBase_ + static_cast<uint64_t>(i) * entryBytes_;
            PushSingle(reinterpret_cast<__gm__ uint8_t *>(src), dst, entryBytes_);
        }
        FlushPipe();
        AscendC::PipeBarrier<PIPE_ALL>();
    }

private:
    /* Ping-pong slot = half the SoC's user UB ceiling: 120KB on A5, 88KB on A3. */
    static constexpr uint32_t SLOT_BYTES = static_cast<uint32_t>(ENTRY_GATHER_UB_ONCE_SIZE) / 2;

    /* One entry per queue transaction: stage it on the free slot, then
     * drain the previous slot while the next entry keeps filling the other. */
    HYBM_AICORE_KERNEL void PushSingle(__gm__ uint8_t *src, __gm__ uint8_t *dst, uint32_t len)
    {
        if (staged_) {
            CommitPending();
        }
        StageSlot(turn_, src, len);
        pendLen_ = len;
        pendDst_ = dst;
        staged_ = true;
        turn_ ^= 1;
    }

    /* Issue one slot's GM->UB copy-in. */
    HYBM_AICORE_KERNEL void StageSlot(uint32_t slot, __gm__ uint8_t *src, uint32_t len)
    {
        auto &que = (slot == 0) ? queA_ : queB_;
        AscendC::LocalTensor<uint8_t> local = que.AllocTensor<uint8_t>();
        AscendC::DataCopyPadExtParams<uint8_t> padParams;
        AscendC::GlobalTensor<uint8_t> g;
        g.SetGlobalBuffer(src, len);
        AscendC::GlobalTensor<uint8_t> srcGm = g[0];
        AscendC::DataCopyExtParams cp(1, static_cast<int32_t>(len), 0, 0, 0);
        AscendC::DataCopyPad(local, srcGm, cp, padParams);
        que.EnQue(local);
    }

    /* Drain the pending slot (waits only its own copy-in) and issue copy-out. */
    HYBM_AICORE_KERNEL void CommitPending()
    {
        auto &que = ((turn_ ^ 1) == 0) ? queA_ : queB_;
        AscendC::LocalTensor<uint8_t> local = que.DeQue<uint8_t>();
        AscendC::GlobalTensor<uint8_t> g;
        g.SetGlobalBuffer(pendDst_, pendLen_);
        AscendC::GlobalTensor<uint8_t> dstGm = g[0];
        AscendC::DataCopyExtParams cp(1, static_cast<int32_t>(pendLen_), 0, 0, 0);
        AscendC::DataCopyPad(dstGm, local, cp);
        que.FreeTensor(local);
    }

    HYBM_AICORE_KERNEL void FlushPipe()
    {
        if (staged_) {
            CommitPending();
        }
        staged_ = false;
    }

private:
    AscendC::TPipe pipe_;
    AscendC::TQueBind<AscendC::TPosition::VECIN, AscendC::TPosition::VECOUT, 1> queA_;
    AscendC::TQueBind<AscendC::TPosition::VECIN, AscendC::TPosition::VECOUT, 1> queB_;
    __gm__ uint64_t *ids_ = nullptr;
    __gm__ uint8_t *dstBase_ = nullptr;
    __gm__ uint8_t *pendDst_ = nullptr;
    uint64_t poolGva_ = 0;
    uint64_t slotStride_ = 0;
    uint32_t entryBytes_ = 0;
    uint32_t rowsPerSlot_ = 0;
    uint32_t pendLen_ = 0;
    uint32_t aivNum_ = 0;
    uint32_t aivIndex_ = 0;
    uint32_t count_ = 0;
    uint32_t turn_ = 0;
    bool staged_ = false;
};

#endif // ACC_OFFLOAD_ENTRY_GATHER_H
