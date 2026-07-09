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

#include "zbal_kernel_base.h"

template<typename T>
class ZBALGatherKernel : public ZBALBaseKernel {
public:
    ZBAL_KERNEL ZBALGatherKernel() {}

    ZBAL_KERNEL void Init(GM_ADDR input, GM_ADDR output, GM_ADDR metaGM, uint64_t elements, uint16_t root,
                          uint64_t waitSymbol)
    {
#if defined(ZBAL_ASCEND_NPU_A3) || defined(ZBAL_ASCEND_NPU_A5)
        this->comm = reinterpret_cast<__gm__ CommGroupInfo *>(metaGM);
        this->myGroupRank = comm->myGroupRank;
        this->groupSize = comm->groupSize;
        this->memSize = comm->localDeviceMemSize;
        this->worldRanks = reinterpret_cast<__gm__ uint16_t *>(comm->peerGroupRank2WorldRank);
        this->dataOpType = comm->dataOpType;

        this->aivNum = AscendC::GetBlockNum();
        this->aivIndex = AscendC::GetBlockIdx();
        this->root = root;
        this->input = input;
        this->output = output;
        this->elements = elements;
        this->addrOffset = groupSize * ZBAL_FLAG_SIZE;
        this->flagMagic = waitSymbol;
        // |------input------|------flag------|------stat------|
        this->exchangeAddr = reinterpret_cast<__gm__ uint64_t *>(comm->myAddressExchangeGva);
        this->exchangeFlag = exchangeAddr + addrOffset;
        this->exchangestat = exchangeFlag + addrOffset;

        ZBALBaseKernel::Init();
#endif
    }

    ZBAL_KERNEL void Process()
    {
#if defined(ZBAL_ASCEND_NPU_A3) || defined(ZBAL_ASCEND_NPU_A5)
        ZBAL_PROF_START(comm, ZBAL_PROF_GATHER_KERNEL_ALL);
        InitDataAddrAndFlag();

        if (myGroupRank == root) {
            InnerProcessByElement();
        }

        if (aivIndex == 0) {
            ZBAL_PROF_START(comm, ZBAL_PROF_WAIT_STAT);
            ZBALWaitFlag(exchangestat, flagMagic, root);
            ZBAL_PROF_STOP(comm, ZBAL_PROF_WAIT_STAT);
        }
        AscendC::SyncAll<true>();

        ZBAL_PROF_STOP(comm, ZBAL_PROF_GATHER_KERNEL_ALL);
#endif
    }

    ZBAL_KERNEL void InnerProcessByElement()
    {
        uint64_t totalElements = elements * groupSize;
        uint64_t numPerCore = totalElements / aivNum;
        uint64_t remElems = totalElements % aivNum;
        uint64_t startElem;
        if (aivIndex < remElems) {
            startElem = aivIndex * (numPerCore + 1);
            numPerCore += 1;
        } else {
            startElem = aivIndex * numPerCore + remElems;
        }

        // Each core copies its element range; a range may span ≥1 rank boundaries
        uint64_t curPos = startElem;
        uint64_t remaining = numPerCore;
        while (remaining > 0) {
            uint64_t rankIdx = curPos / elements;
            uint64_t rankOffset = curPos % elements;
            uint64_t copySize = remaining;
            if (rankOffset + copySize > elements) {
                copySize = elements - rankOffset;
            }

            ZBAL_PROF_START(comm, ZBAL_PROF_WAIT_FLAG);
            ZBALWaitFlag(exchangeFlag, flagMagic, rankIdx);
            ZBAL_PROF_STOP(comm, ZBAL_PROF_WAIT_FLAG);

            ZBAL_PROF_START(comm, ZBAL_PROF_GATHER_PREPARE_PTR);
            uint64_t inputDataAddr = ZBALGetFlag(exchangeAddr, rankIdx);
            AscendC::GlobalTensor<T> outputGT;
            outputGT.SetGlobalBuffer(reinterpret_cast<__gm__ T *>(output), totalElements);
            AscendC::GlobalTensor<T> inputGT;
            inputGT.SetGlobalBuffer(reinterpret_cast<__gm__ T *>(inputDataAddr), elements);
            AscendC::PipeBarrier<PIPE_ALL>();
            ZBAL_PROF_STOP(comm, ZBAL_PROF_GATHER_PREPARE_PTR);

            ZBAL_PROF_START(comm, ZBAL_PROF_GATHER_COPY);
            CpGM2GM(inputGT[rankOffset], outputGT[curPos], copySize);
            ZBAL_PROF_STOP(comm, ZBAL_PROF_GATHER_COPY);

            curPos += copySize;
            remaining -= copySize;
        }
        AscendC::SyncAll<true>();

        // Distribute stat writes across cores (one stat per rank)
        uint64_t ranksPerCore = CeilDiv<uint64_t>(groupSize, aivNum);
        uint64_t startRank = aivIndex * ranksPerCore;
        uint64_t endRank = startRank + ranksPerCore;
        if (endRank > groupSize) {
            endRank = groupSize;
        }
        ZBAL_PROF_START(comm, ZBAL_PROF_WRITE_STAT);
        for (uint64_t r = startRank; r < endRank; r++) {
            auto destStatPtr = ZbalPtr(exchangestat, r);
            ZBALSetFlag(destStatPtr, flagMagic, myGroupRank);
        }
        ZBAL_PROF_STOP(comm, ZBAL_PROF_WRITE_STAT);
        AscendC::SyncAll<true>();
    }

private:
    ZBAL_KERNEL void InitDataAddrAndFlag()
    {
        ZBAL_PROF_START(comm, ZBAL_PROF_EXCHANGE_ADDR);
        if (aivIndex == 0) {
            uint64_t dataAddr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(input));
            auto dataPtr = ZbalPtr(exchangeAddr, root);
            ZBALSetFlag(dataPtr, dataAddr, myGroupRank);

            auto flagPtr = ZbalPtr(exchangeFlag, root);
            ZBALSetFlag(flagPtr, flagMagic, myGroupRank);
        }
        ZBAL_PROF_STOP(comm, ZBAL_PROF_EXCHANGE_ADDR);
    }

private:
    uint32_t aivNum;
    uint32_t aivIndex;
    uint16_t root;
    uint32_t elements;
    uint32_t addrOffset;
    uint64_t flagMagic;
    __gm__ void *input;
    __gm__ void *output;
    __gm__ CommGroupInfo *comm;
    __gm__ uint64_t *exchangeAddr;
    __gm__ uint64_t *exchangeFlag;
    __gm__ uint64_t *exchangestat;
};

#define ZBAL_GATHER_TYPE_MAP(F)        \
    F(int8_t, ZBAL_DATA_TYPE_INT8)     \
    F(int16_t, ZBAL_DATA_TYPE_INT16)   \
    F(int32_t, ZBAL_DATA_TYPE_INT32)   \
    F(float16_t, ZBAL_DATA_TYPE_FP16)  \
    F(float, ZBAL_DATA_TYPE_FP32)      \
    F(int64_t, ZBAL_DATA_TYPE_INT64)   \
    F(uint64_t, ZBAL_DATA_TYPE_UINT64) \
    F(uint8_t, ZBAL_DATA_TYPE_UINT8)   \
    F(uint16_t, ZBAL_DATA_TYPE_UINT16) \
    F(uint32_t, ZBAL_DATA_TYPE_UINT32) \
    F(float64_t, ZBAL_DATA_TYPE_FP64)  \
    F(bfloat16_t, ZBAL_DATA_TYPE_BFP16)

#define ZBAL_GA_CASE(TYPE, ENUM_VAL)                                  \
    case zbal_datatype_t::ENUM_VAL: {                                 \
        ZBALGatherKernel<TYPE> op;                                    \
        op.Init(input, output, metaAddr, elements, root, waitSymbol); \
        op.Process();                                                 \
        break;                                                        \
    }

extern "C" __global__ __aicore__ void ZBALGatherInner(GM_ADDR input, GM_ADDR output, size_t elements, uint32_t dataType,
                                                      GM_ADDR metaAddr, uint16_t root, uint64_t waitSymbol)
{
    zbal_datatype_t ZBAL_DATA_TYPE = static_cast<zbal_datatype_t>(dataType);
    KERNEL_TASK_TYPE_DEFAULT(KERNEL_TYPE_MIX_AIV_1_0);

    switch (ZBAL_DATA_TYPE) {
        ZBAL_GATHER_TYPE_MAP(ZBAL_GA_CASE)
        default:
            break;
    }
}

int32_t ZBALOpGather(const void *sendBuff, void *recvBuff, size_t sendCount, zbal_datatype_t dataType, uint16_t root,
                     aclrtStream stream, CommGroupInfo &groupInfo)
{
    uint32_t blockDim = ZBALOpGetAivBlockDim(groupInfo, sendCount, dataType);
    uint32_t dataTypeNum = static_cast<uint32_t>(dataType);
    uint8_t *metaAddr = reinterpret_cast<uint8_t *>(groupInfo.myMetaGva);
    uint8_t *input = reinterpret_cast<uint8_t *>(const_cast<void *>(sendBuff));
    uint8_t *output = reinterpret_cast<uint8_t *>(recvBuff);
    uint64_t waitSymbol = ++groupInfo.waitSymbol;

    ZBALGatherInner<<<blockDim, nullptr, stream>>>(input, output, sendCount, dataTypeNum, metaAddr, root, waitSymbol);
    return 0;
}
