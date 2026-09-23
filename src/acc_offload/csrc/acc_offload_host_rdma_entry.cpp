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
#include "acc_offload.h"
#include "acc_offload_define.h"
#include "acc_offload_host_rdma_sparse.h"
#include "smem_types.h"

namespace ock::offload {
namespace {
int32_t CreateSparse(const smem::HostRdmaSparseConfig &config, smem_bm_t handle, void **out)
{
    try {
        auto context = std::make_unique<HostRdmaSparse>(
            config,
            [handle](uint64_t src, uint64_t dst, uint64_t bytes) {
                smem_copy_params_t params{reinterpret_cast<void *>(src), reinterpret_cast<void *>(dst), bytes, nullptr};
                return smem_bm_copy(handle, &params, SMEMB_COPY_AUTO, 0);
            },
            [handle](smem_batch_copy_params *params) {
                return smem_bm_copy_batch(handle, params, SMEMB_COPY_AUTO, 0);
            });
        auto ret = context->Prepare();
        if (ret != 0) {
            return ret;
        }
        *out = context.release();
        return 0;
    } catch (const std::exception &error) {
        OFFLOAD_LOG_ERROR("cannot create HOST_RDMA sparse operator: " << error.what());
        return OFFLOAD_ERROR;
    }
}

void DestroySparse(void *context)
{
    delete static_cast<HostRdmaSparse *>(context);
}
int32_t ProcessSparse(void *context, uint64_t request)
{
    return static_cast<HostRdmaSparse *>(context)->ProcessRequest(request);
}
int32_t PrepareSparseCase(void *context, const uint64_t *destinations, uint32_t count, uint64_t bytes)
{
    return static_cast<HostRdmaSparse *>(context)->PrepareCase(destinations, count, bytes);
}
const smem::HostRdmaSparseBackend BACKEND{CreateSparse, DestroySparse, ProcessSparse, PrepareSparseCase};
// Only register the lifecycle hooks at load time: no allocation, threads, MF init or NPU init.
[[maybe_unused]] const int32_t REGISTERED = smem::RegisterHostRdmaSparseBackend(&BACKEND);

struct SparseCopyArgs {
    const uint64_t *sources;
    const uint64_t *destinations;
    uint32_t count;
    uint64_t bytes;
};

int32_t ExecuteSparse(void *context, void *opaque)
{
    auto args = static_cast<const SparseCopyArgs *>(opaque);
    return static_cast<HostRdmaSparse *>(context)->Run(args->sources, args->destinations, args->count, args->bytes);
}

int32_t ReadSparseTiming(void *context, void *opaque)
{
    return static_cast<HostRdmaSparse *>(context)->LastTiming(
        *static_cast<offload_host_rdma_sparse_timing_t *>(opaque));
}
} // namespace
} // namespace ock::offload

OFFLOAD_API int32_t offload_sparse_copy_host_rdma(void *bmHandle, const uint64_t *sources, const uint64_t *destinations,
                                                  uint32_t count, uint64_t blockBytes)
{
    ock::offload::SparseCopyArgs args{sources, destinations, count, blockBytes};
    // SMEM holds the attachment lock for the visitor, so destroy/leave cannot free it during copy.
    return ock::smem::UseHostRdmaSparseContext(bmHandle, ock::offload::ExecuteSparse, &args);
}

OFFLOAD_API int32_t offload_host_rdma_sparse_last_timing(void *bmHandle, offload_host_rdma_sparse_timing_t *timing)
{
    if (timing == nullptr) {
        OFFLOAD_LOG_ERROR("null sparse timing output");
        return ock::smem::SM_INVALID_PARAM;
    }
    return ock::smem::UseHostRdmaSparseContext(bmHandle, ock::offload::ReadSparseTiming, timing);
}
