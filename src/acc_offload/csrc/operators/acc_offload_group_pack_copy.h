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

#ifndef ACC_OFFLOAD_GROUP_PACK_COPY_H
#define ACC_OFFLOAD_GROUP_PACK_COPY_H

#include "kernel_operator.h"

#define HYBM_AICORE_KERNEL __attribute__((always_inline)) __aicore__ __inline__

constexpr int64_t UB_ONCE_SIZE_GPC = 176 * 1024;
constexpr uint32_t UB_ALIGN_SIZE_GPC = 32;

/*
 * Group-pack compacted copy operator for MoE-style expert dispatch.
 *
 * inputs / outputs / lens / groupList are ALL arrays of the same length N (= *numLocalExpertPtr).
 * lens[i] is the count in units of T elements of inputs[i] / outputs[i]. The kernel is
 * instantiated with T=uint8_t (byte copy), so lens[i] is effectively the BYTE count; callers
 * pass tensor.numel() * tensor.element_size(). The kernel scans [0, N) and compactly writes
 * the non-zero entries to the front of outputs / packedGroupList.
 * For the j-th non-zero entry (at original index i, where groupList[i] != 0, j = 0..M-1):
 *   - inputs[i] is copied to outputs[j] (compacted, immediately after the previous one), and
 *   - groupList[i] is written to packedGroupList[j].
 * M = total count of non-zero entries. After the kernel, outputs[0..M) and
 * packedGroupList[0..M) hold the compacted results; the tails are left untouched.
 *
 * Metadata staging (modeled on zbal AlltoAllV): numLocalExpert is read once via GM scalar in Init to
 * size the UB buffers, then numLocalExpert / inputs / outputs / lens / groupList are bulk-copied
 * (DataCopyPad GM->UB, MTE2) into LocalTensors in LoadMetadata. After a PipeBarrier all
 * metadata is read via LocalTensor.GetValue (S-pipe UB read) -- no repeated GM scalar reads.
 *
 * Core partitioning: the TOTAL element count across all non-zero entries is split across cores
 * as [coreLeft, coreRight). All cores collaborate on each non-zero entry -- a single entry's
 * data is sliced across cores (mid-entry startOffset) so that even a single large entry keeps
 * all cores busy. effAivNum = min(aivNum, totalElements); cores with idx >= effAivNum do
 * nothing. Unaligned slice offsets/lengths are handled by DataCopyPad. The last core takes the
 * remainder, so [0, totalElements) is fully covered with no gaps/overlaps. Accumulators are
 * 64-bit to avoid overflow for large totalElements.
 *
 * packedGroupList write strategy: ONLY Core 0 touches packedGroupList_. It counts non-zero
 * entries (packedCount = M), stashes the non-zero groupList values into packedLocalBuf_
 * (pre-allocated in Init for the worst case N entries) via SetValue, and bulk-copies them to
 * packedGroupList_ in a single DataCopyPad. This avoids cross-core GM scalar writes entirely
 * and matches the AscendC UB-staging paradigm. The data copy path (CopyCoreRange) is
 * independent of packedGroupList and runs on all cores.
 *
 * Invariants (guaranteed by caller, kernel does not re-validate):
 *   - num_local_expert equals N, the common length of inputs / outputs / lens / groupList.
 *   - outputs and packedGroupList each have at least N slots (only [0, M) are written).
 *   - Each outputs[j] backing buffer holds at least lens[i] elements (lens[i]*sizeof(T) bytes)
 *     for the corresponding source index i.
 *   - N is small enough that the staged metadata + packedLocalBuf_ fit in UB alongside
 *     bindQueue_ (typical MoE expert counts << 1K; A3 UB=192KB, A5 UB=256KB).
*/
template<typename T>
class OffloadGroupPackCopyKernel {
public:
    HYBM_AICORE_KERNEL OffloadGroupPackCopyKernel() {}

    HYBM_AICORE_KERNEL void Init(GM_ADDR inputs, GM_ADDR outputs, GM_ADDR lens, GM_ADDR numLocalExpert,
                                 GM_ADDR groupList, GM_ADDR packedGroupList)
    {
        aivNum_ = AscendC::GetBlockNum();
        aivIndex_ = AscendC::GetBlockIdx();

        // numLocalExpert is read once via GM scalar to size the UB buffers; this must happen before
        // InitBuffer. Subsequent metadata access goes through LocalTensors (LoadMetadata).
        size_ = *(reinterpret_cast<__gm__ uint32_t *>(numLocalExpert));

        numLocalExpertGm_ = reinterpret_cast<__gm__ uint32_t *>(numLocalExpert);
        inputsGm_ = reinterpret_cast<__gm__ uint64_t *>(inputs);
        outputsGm_ = reinterpret_cast<__gm__ uint64_t *>(outputs);
        lensGm_ = reinterpret_cast<__gm__ uint32_t *>(lens);
        groupListGm_ = reinterpret_cast<__gm__ int64_t *>(groupList);
        packedGroupListGm_ = reinterpret_cast<__gm__ int64_t *>(packedGroupList);

        pipe_.InitBuffer(numLocalExpertBuf_, AlignUp(static_cast<uint32_t>(sizeof(uint32_t))));
        pipe_.InitBuffer(inputsBuf_, AlignUp(size_ * static_cast<uint32_t>(sizeof(uint64_t))));
        pipe_.InitBuffer(outputsBuf_, AlignUp(size_ * static_cast<uint32_t>(sizeof(uint64_t))));
        pipe_.InitBuffer(lensBuf_, AlignUp(size_ * static_cast<uint32_t>(sizeof(uint32_t))));
        pipe_.InitBuffer(groupListBuf_, AlignUp(size_ * static_cast<uint32_t>(sizeof(int64_t))));
        pipe_.InitBuffer(bindQueue_, 1, UB_ONCE_SIZE_GPC);
        pipe_.InitBuffer(packedLocalBuf_, AlignUp(size_ * static_cast<uint32_t>(sizeof(int64_t))));
    }

    HYBM_AICORE_KERNEL void Process()
    {
        LoadMetadata();
        uint64_t totalElements = CountTotalElements();
        uint32_t effAivNum = (totalElements < aivNum_) ? static_cast<uint32_t>(totalElements) : aivNum_;
        if (totalElements > 0 && aivIndex_ < effAivNum) {
            uint64_t coreLeft = 0;
            uint64_t coreRight = 0;
            ComputeCoreRange(totalElements, effAivNum, coreLeft, coreRight);
            CopyCoreRange(coreLeft, coreRight);
        }
        // Core 0 collects packedGroupList independently of the data copy path. Even when
        // totalElements == 0 (all active experts have zero-length data), non-zero groupList
        // entries must still be packed into packedGroupList_.
        CollectPackedGroupList();
    }

private:
    // Align bytes up to UB_ALIGN_SIZE_GPC (required by TPipe::InitBuffer).
    HYBM_AICORE_KERNEL uint32_t AlignUp(uint32_t bytes)
    {
        return ((bytes + UB_ALIGN_SIZE_GPC - 1) / UB_ALIGN_SIZE_GPC) * UB_ALIGN_SIZE_GPC;
    }

    // Bulk-copy a typed GM array into a UB LocalTensor via DataCopyPad (GM->UB, MTE2). Uses the
    // 4-arg overload (with padExtParams), matching zbal CpGM2GMMTE for the GM->UB direction.
    template<typename U>
    HYBM_AICORE_KERNEL void CopyGmToUb(AscendC::LocalTensor<U> &local, __gm__ U *gm, uint32_t count)
    {
        if (count == 0) {
            return;
        }
        AscendC::GlobalTensor<U> gmTensor;
        gmTensor.SetGlobalBuffer(gm, count);
        uint32_t copyBytes = count * static_cast<uint32_t>(sizeof(U));
        AscendC::DataCopyExtParams copyParams(1, copyBytes, 0, 0, 0);
        AscendC::DataCopyPadExtParams<U> padParams{};
        AscendC::DataCopyPad(local, gmTensor, copyParams, padParams);
    }

    // Stage numLocalExpert / inputs / outputs / lens / groupList from GM into UB LocalTensors. After
    // PipeBarrier<PIPE_ALL>, all metadata is readable via GetValue (S-pipe UB read). numLocalExpert is
    // staged too (per alltoallv pattern) even though size_ is already known from Init.
    HYBM_AICORE_KERNEL void LoadMetadata()
    {
        AscendC::LocalTensor<uint32_t> numLocalExpertLT = numLocalExpertBuf_.Get<uint32_t>();
        inputsLT_ = inputsBuf_.Get<uint64_t>();
        outputsLT_ = outputsBuf_.Get<uint64_t>();
        lensLT_ = lensBuf_.Get<uint32_t>();
        groupListLT_ = groupListBuf_.Get<int64_t>();

        CopyGmToUb<uint32_t>(numLocalExpertLT, numLocalExpertGm_, 1);
        CopyGmToUb<uint64_t>(inputsLT_, inputsGm_, size_);
        CopyGmToUb<uint64_t>(outputsLT_, outputsGm_, size_);
        CopyGmToUb<uint32_t>(lensLT_, lensGm_, size_);
        CopyGmToUb<int64_t>(groupListLT_, groupListGm_, size_);
        AscendC::PipeBarrier<PIPE_ALL>();
    }

    // Sum of lens[i] over non-zero groupList entries: total element count to copy.
    HYBM_AICORE_KERNEL uint64_t CountTotalElements()
    {
        uint64_t total = 0;
        for (uint32_t i = 0; i < size_; i++) {
            if (groupListLT_.GetValue(i) != 0) {
                total += lensLT_.GetValue(i);
            }
        }
        return total;
    }

    // Split [0, totalElements) across effAivNum cores. perCore = totalElements / effAivNum;
    // the last core takes the remainder so [0, totalElements) is fully covered with no
    // gaps/overlaps. No artificial block alignment (see file header comment).
    HYBM_AICORE_KERNEL void ComputeCoreRange(uint64_t totalElements, uint32_t effAivNum, uint64_t &coreLeft,
                                             uint64_t &coreRight)
    {
        uint64_t perCore = totalElements / effAivNum;
        coreLeft = aivIndex_ * perCore;
        coreRight = (aivIndex_ == effAivNum - 1) ? totalElements : (coreLeft + perCore);
    }

    // Walk non-zero entries in compacted order; copy the slice of each entry overlapping
    // [coreLeft, coreRight). A single entry may be split across cores (mid-entry startOffset).
    // outIdx is the compacted slot index shared with CopySlice; it advances for every non-zero
    // entry so all cores see a consistent compacted order. packedGroupList is NOT written here --
    // it is handled exclusively by Core 0 in CollectPackedGroupList.
    HYBM_AICORE_KERNEL void CopyCoreRange(uint64_t coreLeft, uint64_t coreRight)
    {
        uint64_t cumLeft = 0;
        uint32_t outIdx = 0;
        for (uint32_t i = 0; i < size_ && cumLeft < coreRight; i++) {
            if (groupListLT_.GetValue(i) == 0) {
                continue;
            }
            uint64_t len = lensLT_.GetValue(i);
            uint64_t cumRight = cumLeft + len;
            if (coreRight > cumLeft && coreLeft < cumRight) {
                uint64_t startOffset = (coreLeft > cumLeft) ? (coreLeft - cumLeft) : 0;
                uint64_t endOffset = (coreRight < cumRight) ? (coreRight - cumLeft) : len;
                CopySlice(i, outIdx, startOffset, endOffset - startOffset);
            }
            cumLeft = cumRight;
            outIdx++;
        }
    }

    // Core 0 alone owns packedGroupList_. Count non-zero entries (M), stash values via SetValue
    // into packedLocalBuf_ (pre-allocated in Init for the worst case N entries), then bulk-copy
    // to GM. Other cores skip this entirely, removing any cross-core concurrent GM scalar
    // writes on packedGroupList_.
    HYBM_AICORE_KERNEL void CollectPackedGroupList()
    {
        if (aivIndex_ != 0) {
            return;
        }
        uint32_t packedCount = 0;
        for (uint32_t i = 0; i < size_; i++) {
            if (groupListLT_.GetValue(i) != 0) {
                packedCount++;
            }
        }
        if (packedCount == 0) {
            return;
        }
        AscendC::LocalTensor<int64_t> packedLocal = packedLocalBuf_.Get<int64_t>();
        uint32_t packedIdx = 0;
        for (uint32_t i = 0; i < size_; i++) {
            int64_t val = groupListLT_.GetValue(i);
            if (val != 0) {
                packedLocal.SetValue(packedIdx, val);
                packedIdx++;
            }
        }
        AscendC::PipeBarrier<PIPE_ALL>();
        AscendC::GlobalTensor<int64_t> packedGm;
        packedGm.SetGlobalBuffer(packedGroupListGm_, packedCount);
        uint32_t copyBytes = packedCount * static_cast<uint32_t>(sizeof(int64_t));
        AscendC::DataCopyExtParams copyParams(1, copyBytes, 0, 0, 0);
        AscendC::DataCopyPad(packedGm, packedLocal, copyParams);
        AscendC::PipeBarrier<PIPE_ALL>();
    }

    // Copy inputs[inIdx][offset .. offset+count) -> outputs[outIdx][offset .. offset+count).
    HYBM_AICORE_KERNEL void CopySlice(uint32_t inIdx, uint32_t outIdx, uint64_t offset, uint64_t count)
    {
        if (count == 0) {
            return;
        }
        auto inputPtr = reinterpret_cast<__gm__ T *>(inputsLT_.GetValue(inIdx));
        auto outputPtr = reinterpret_cast<__gm__ T *>(outputsLT_.GetValue(outIdx));
        uint32_t len = lensLT_.GetValue(inIdx);
        inputGm_.SetGlobalBuffer(inputPtr, len);
        outputGm_.SetGlobalBuffer(outputPtr, len);
        CpGM2GM(offset, count);
    }

    // Chunked GM->UB->GM copy of `count` elements starting at element `offset`. DataCopyPad
    // handles unaligned slice offsets/lengths (tail chunk may be non-block-aligned). GM->UB uses
    // the 4-arg overload (with padParams); UB->GM uses the 3-arg overload, matching zbal.
    HYBM_AICORE_KERNEL void CpGM2GM(uint64_t offset, uint64_t count)
    {
        uint64_t leftLen = count * sizeof(T);
        uint32_t times = 0;
        uint32_t preCopyNum = UB_ONCE_SIZE_GPC / sizeof(T);
        AscendC::DataCopyPadExtParams<T> padParams{};

        while (leftLen > 0) {
            uint32_t curCopySize =
                (leftLen > UB_ONCE_SIZE_GPC) ? static_cast<uint32_t>(UB_ONCE_SIZE_GPC) : static_cast<uint32_t>(leftLen);
            AscendC::LocalTensor<T> local = bindQueue_.AllocTensor<T>();
            AscendC::DataCopyExtParams dataCopyParams(1, curCopySize, 0, 0, 0);
            uint64_t elemOffset = static_cast<uint64_t>(times) * preCopyNum + offset;
            AscendC::DataCopyPad(local, inputGm_[elemOffset], dataCopyParams, padParams);
            bindQueue_.EnQue(local);
            local = bindQueue_.DeQue<T>();
            AscendC::DataCopyPad(outputGm_[elemOffset], local, dataCopyParams);
            bindQueue_.FreeTensor(local);
            leftLen = (leftLen > UB_ONCE_SIZE_GPC) ? leftLen - UB_ONCE_SIZE_GPC : 0;
            times++;
        }

        AscendC::PipeBarrier<PIPE_ALL>();
    }

private:
    AscendC::TPipe pipe_;
    AscendC::TQueBind<AscendC::TPosition::VECIN, AscendC::TPosition::VECOUT, 1> bindQueue_;
    AscendC::TBuf<> numLocalExpertBuf_;
    AscendC::TBuf<> inputsBuf_;
    AscendC::TBuf<> outputsBuf_;
    AscendC::TBuf<> lensBuf_;
    AscendC::TBuf<> groupListBuf_;
    AscendC::TBuf<> packedLocalBuf_;
    AscendC::GlobalTensor<T> inputGm_;
    AscendC::GlobalTensor<T> outputGm_;
    AscendC::LocalTensor<uint64_t> inputsLT_;
    AscendC::LocalTensor<uint64_t> outputsLT_;
    AscendC::LocalTensor<uint32_t> lensLT_;
    AscendC::LocalTensor<int64_t> groupListLT_;
    uint32_t aivNum_;
    uint32_t aivIndex_;
    uint32_t size_;
    __gm__ uint32_t *numLocalExpertGm_;
    __gm__ uint64_t *inputsGm_;
    __gm__ uint64_t *outputsGm_;
    __gm__ uint32_t *lensGm_;
    __gm__ int64_t *groupListGm_;
    __gm__ int64_t *packedGroupListGm_;
};

#endif // ACC_OFFLOAD_GROUP_PACK_COPY_H
