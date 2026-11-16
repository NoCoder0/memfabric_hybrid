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

#include <ATen/ATen.h>
#include "torch_npu/csrc/core/npu/NPUStream.h"
#include "torch_npu/csrc/core/npu/NPUGuard.h"
#include "torch_npu/csrc/framework/OpCommand.h"
#include "acc_offload_operators.h"
#include "acc_offload_operators_launch.h"

#if defined(ACC_SOC_VERSION_A3)
#include <c10/util/ScopeExit.h>
#endif

extern "C" {

#if defined(ACC_SOC_VERSION_A3)
int32_t AccOffloadInitializeAicpu()
{
    aclrtStream stream = nullptr;
    auto ret = aclrtCreateStream(&stream);
    if (ret != ACL_SUCCESS) {
        return ret;
    }
    auto streamGuard = c10::make_scope_exit([&]() { aclrtDestroyStream(stream); });
    OffloadOpsInitializeAicpu(stream);
    return aclrtSynchronizeStream(stream);
}
#endif

void AccOffloadAicpuLaunch(AccOffloadAicpuCallback callback, const AccOffloadAicpuParam &param, uint8_t deviceId)
{
    c10_npu::OptionalNPUGuard npuGuard;
    npuGuard.set_index(deviceId);
    void *stream = c10_npu::getCurrentNPUStream(deviceId).stream(false);

    at_npu::native::OpCommand::RunOpApiV2("acc_aicpu_sparse_copy",
                                          [callback, param, stream]() -> int { return callback(param, stream); });
}

void AccOffloadTransportEntryGather(int32_t (*submit)(void *, uint64_t *, uint64_t *, uint32_t *, uint32_t *, void *),
                                    void *entity, uint64_t dst, uint64_t ids, uint64_t count, uint32_t capacity,
                                    uint64_t poolGva, uint64_t slotStride, uint32_t entryBytes, uint32_t rowsPerSlot,
                                    uint8_t device)
{
    auto launchStream = c10_npu::getCurrentNPUStream(device);
    c10_npu::NPUStreamGuard streamGuard(launchStream);
    auto stream = launchStream.stream(false);
    auto options = at::TensorOptions().device(c10::Device(c10::DeviceType::PrivateUse1, device));
    auto descriptors = at::empty({capacity, 5}, options.dtype(at::kInt));
    // The framework allocator retains captured scratch storage in the graph's memory pool.
    auto callback = [=]() -> int {
        auto srcPtrs = reinterpret_cast<uint64_t *>(descriptors.data_ptr());
        auto dstPtrs = srcPtrs + capacity;
        auto sizes = reinterpret_cast<uint32_t *>(dstPtrs + capacity);
        OffloadOpsEntryGatherDescriptors(dst, ids, count, srcPtrs, dstPtrs, sizes, capacity, poolGva, slotStride,
                                         entryBytes, rowsPerSlot, stream);
        auto ret = submit(entity, srcPtrs, dstPtrs, sizes, reinterpret_cast<uint32_t *>(count), stream);
        TORCH_CHECK(ret == 0, "MemFabric entry gather transport submission failed, ret=", ret);
        return ret;
    };
    at_npu::native::OpCommand::RunOpApiV2("acc_transport_entry_gather", callback);
}

#if !defined(ACC_SOC_VERSION_A2)
void AccOffloadSparseCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *sizePtr, uint8_t devIdx)
{
#if defined(ACC_SOC_VERSION_A5)
    constexpr uint32_t blockDim = 64;
#else
    constexpr uint32_t blockDim = 48;
#endif
    c10_npu::OptionalNPUGuard npuGuard;
    npuGuard.set_index(devIdx);

    auto stream = c10_npu::getCurrentNPUStream(devIdx);
    void *npuStream = stream.stream(false);

    auto callback = [srcPtrs, dstPtrs, lenPtrs, sizePtr, npuStream]() -> int {
        OffloadOpsSparseCopy(srcPtrs, dstPtrs, lenPtrs, sizePtr, blockDim, npuStream);
        return 0;
    };

    at_npu::native::OpCommand::RunOpApiV2("acc_sparse_copy", callback);
}

void AccOffloadGroupPackCopy(uint64_t *srcPtrs, uint64_t *dstPtrs, uint32_t *lenPtrs, uint32_t *numLocalExpertPtr,
                             int64_t *groupList, int64_t *packedGroupList, uint8_t devIdx)
{
    c10_npu::OptionalNPUGuard npuGuard;
    npuGuard.set_index(devIdx);

    auto stream = c10_npu::getCurrentNPUStream(devIdx);
    void *npuStream = stream.stream(false);

    auto callback = [srcPtrs, dstPtrs, lenPtrs, numLocalExpertPtr, groupList, packedGroupList, npuStream]() -> int {
        OffloadOpsGroupPackCopy(srcPtrs, dstPtrs, lenPtrs, numLocalExpertPtr, groupList, packedGroupList, npuStream);
        return 0;
    };

    at_npu::native::OpCommand::RunOpApiV2("acc_group_pack_copy", callback);
}

void AccOffloadKvExchange(uint64_t *metaPtr, uint8_t devIdx)
{
#if defined(ACC_SOC_VERSION_A5)
    constexpr uint32_t blockDim = 64;
#else
    constexpr uint32_t blockDim = 48;
#endif
    c10_npu::OptionalNPUGuard npuGuard;
    npuGuard.set_index(devIdx);

    auto stream = c10_npu::getCurrentNPUStream(devIdx);
    void *npuStream = stream.stream(false);

    auto callback = [metaPtr, blockDim, npuStream]() -> int {
        OffloadOpsKvExchange(metaPtr, blockDim, npuStream);
        return 0;
    };

    at_npu::native::OpCommand::RunOpApiV2("acc_kv_exchange", callback);
}

void AccOffloadEntryGather(uint64_t dstPtr, uint64_t idsPtr, uint64_t countPtr, uint64_t poolGva, uint64_t slotStride,
                           uint32_t entryBytes, uint32_t rowsPerSlot, uint8_t devIdx)
{
    /* Fused random-entry gather: device-resident GLOBAL row ids -> pool GVA
     * conversion in-kernel (uniform row grid from each slot start, tail gaps
     * strided over); the entry count is read from device memory by the kernel
     * (graph-capture safe); the layout comes from the single table
     * registration. */
#if defined(ACC_SOC_VERSION_A5)
    constexpr uint32_t blockDim = 64;
#else
    constexpr uint32_t blockDim = 48;
#endif
    c10_npu::OptionalNPUGuard npuGuard;
    npuGuard.set_index(devIdx);

    auto stream = c10_npu::getCurrentNPUStream(devIdx);
    void *npuStream = stream.stream(false);

    auto callback = [dstPtr, idsPtr, countPtr, poolGva, slotStride, entryBytes, rowsPerSlot, blockDim,
                     npuStream]() -> int {
        OffloadOpsEntryGather(dstPtr, idsPtr, countPtr, poolGva, slotStride, entryBytes, rowsPerSlot, blockDim,
                              npuStream);
        return 0;
    };

    at_npu::native::OpCommand::RunOpApiV2("acc_entry_gather", callback);
}
#endif
}
