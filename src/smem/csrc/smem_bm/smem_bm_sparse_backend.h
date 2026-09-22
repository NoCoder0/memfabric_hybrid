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
#ifndef MEMFABRIC_SMEM_BM_SPARSE_BACKEND_H
#define MEMFABRIC_SMEM_BM_SPARSE_BACKEND_H
#include "smem_bm_sparse.h"
#include "smem_define.h"

namespace ock::smem {
enum class HostRdmaSparseMode : uint32_t { BASELINE, CONT, GATHER };

struct HostRdmaSparseLayout {
    uint64_t message, request, done, status, signal, watermark, scratch, bytes;
    static SMEM_API HostRdmaSparseLayout Make(uint32_t count, uint64_t blockBytes, uint32_t links, uint32_t chunk);
};

struct HostRdmaSparseConfig {
    smem_bm_host_rdma_sparse_options_t options{};
    HostRdmaSparseMode mode = HostRdmaSparseMode::BASELINE;
    uint32_t rank = 0;
    uint32_t links = 1;
    uint64_t localGva = 0;
    uint64_t localVa = 0;
    uint64_t localBytes = 0;
    uint64_t peerGva = 0;
    uint64_t peerBytes = 0;
};

// Internal lifecycle bridge: SMEM owns the attachment, acc_offload owns its code.
// The provider library must remain loaded until every attached BM is destroyed.
struct HostRdmaSparseBackend {
    int32_t (*create)(const HostRdmaSparseConfig &, smem_bm_t, void **);
    void (*destroy)(void *);
    int32_t (*process)(void *, uint64_t request);
};

// The application owns the calling thread; this object only waits and dispatches.
class HostRdmaSparsePoller {
public:
    using Process = int32_t (*)(void *, uint64_t);
    HostRdmaSparsePoller(uint64_t requestVa, void *context, Process process)
        : requestVa_(requestVa), context_(context), process_(process)
    {}
    SMEM_API int32_t Poll(uint32_t timeoutMs);

private:
    uint64_t requestVa_;
    void *context_;
    Process process_;
    uint64_t sequence_ = 0;
    bool failed_ = false;
};
SMEM_API int32_t RegisterHostRdmaSparseBackend(const HostRdmaSparseBackend *backend);
using HostRdmaSparseVisitor = int32_t (*)(void *context, void *args);
SMEM_API int32_t UseHostRdmaSparseContext(smem_bm_t handle, HostRdmaSparseVisitor visitor, void *args);
} // namespace ock::smem
#endif
