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
#ifndef MEMFABRIC_ACC_OFFLOAD_HOST_RDMA_SPARSE_H
#define MEMFABRIC_ACC_OFFLOAD_HOST_RDMA_SPARSE_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "smem_bm_sparse_backend.h"
#include "smem_thread_pool.h"
#include "acc_offload.h"

namespace ock::offload {
using smem::HostRdmaSparseConfig;
using smem::HostRdmaSparseLayout;
using smem::HostRdmaSparseMode;
using smem::ExecutorService;

class HostRdmaSparse {
public:
    using Copy = std::function<int32_t(uint64_t, uint64_t, uint64_t)>;
    using Batch = std::function<int32_t(smem_batch_copy_params *)>;
    HostRdmaSparse(const HostRdmaSparseConfig &config, Copy copy, Batch batch);
    ~HostRdmaSparse();
    int32_t Prepare();
    int32_t Run(const uint64_t *sources, const uint64_t *destinations, uint32_t count, uint64_t bytes);
    int32_t ProcessRequest(uint64_t request);
    int32_t LastTiming(offload_host_rdma_sparse_timing_t &timing);
    void Stop();

private:
    bool ValidRange(uint64_t address, uint64_t bytes, bool local, bool workspace = false) const;
    bool ValidRequest(const uint64_t *sources, const uint64_t *destinations, uint32_t count, uint64_t bytes) const;
    int32_t Publish(uint64_t offset, uint64_t value);
    int32_t Submit(const uint64_t *sources, const uint64_t *destinations, uint32_t count, uint64_t bytes);
    int32_t Receive(const uint64_t *destinations, uint32_t count, uint64_t bytes);
    int32_t ConsumeProgress(const uint64_t *destinations, uint32_t count, uint64_t bytes);
    int32_t Wait(uint64_t offset, uint64_t value);
    int32_t Serve();
    int32_t Transfer(const std::vector<uint64_t> &sources, std::vector<uint64_t> &destinations, uint64_t bytes);
    void CpuCopy(const std::vector<uint64_t> &sources, const std::vector<uint64_t> &destinations, uint64_t bytes);
    uint64_t Load(uint64_t offset) const;
    uint64_t Local(uint64_t offset) const;
    uint64_t Remote(uint64_t offset) const;
    uint64_t Va(uint64_t offset) const;

    HostRdmaSparseConfig config_;
    HostRdmaSparseLayout layout_;
    Copy copy_;
    Batch batch_;
    uint64_t workspaceOffset_;
    uint64_t sequence_ = 0;
    offload_host_rdma_sparse_timing_t timing_{};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> failed_{false};
    bool started_ = false;
    std::mutex copyMutex_;
    std::unique_ptr<ExecutorService> workers_;
};
} // namespace ock::offload
#endif
