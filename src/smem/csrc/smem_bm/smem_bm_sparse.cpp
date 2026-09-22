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
#include "smem_bm_entry.h"
#include "smem_bm_entry_manager.h"
#include "hybm_big_mem.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <random>
#include <sstream>

namespace ock::smem {
namespace {
std::atomic<const HostRdmaSparseBackend *> &Backend()
{
    static std::atomic<const HostRdmaSparseBackend *> backend{nullptr};
    return backend;
}
uint64_t Align(uint64_t value)
{
    return (value + 63) / 64 * 64;
}
Result ReadMode(HostRdmaSparseMode &mode)
{
    const char *value = std::getenv("MF_HOST_RDMA_SPARSE_MODE");
    std::string name = value == nullptr ? "" : value;
    if (name == "baseline") {
        mode = HostRdmaSparseMode::BASELINE;
    } else if (name == "cont") {
        mode = HostRdmaSparseMode::CONT;
    } else if (name == "gather") {
        mode = HostRdmaSparseMode::GATHER;
    } else {
        SM_LOG_ERROR("set MF_HOST_RDMA_SPARSE_MODE to baseline, cont or gather before preparation");
        return SM_INVALID_PARAM;
    }
    return SM_OK;
}

std::string Signature(const HostRdmaSparseConfig &c)
{
    std::ostringstream out;
    out << "v1:" << static_cast<uint32_t>(c.mode) << ':' << c.options.maxBlocks << ':' << c.options.maxBlockBytes << ':'
        << c.options.progressInterval << ':' << c.links << ':' << c.options.workspaceGva - c.localGva;
    return out.str();
}

Result ExchangeSparseConfig(const StorePtr &store, HostRdmaSparseConfig &c, std::string &readyKey)
{
    std::random_device random;
    auto nonce = std::to_string(random()) + "-" + std::to_string(random()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string prefix = "host_rdma_sparse/";
    auto local = Signature(c) + " " + std::to_string(c.localBytes) + " " + nonce;
    auto ret = store->Set(prefix + std::to_string(c.rank), local);
    if (ret != SM_OK) {
        return ret;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c.options.timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string peer, signature, peerNonce, extra;
        ret = store->Get(prefix + std::to_string(1 - c.rank), peer, 100);
        if (ret != SM_OK) {
            continue;
        }
        std::istringstream input(peer);
        if ((input >> signature >> c.peerBytes >> peerNonce) && !(input >> extra) && signature == Signature(c)) {
            // A matched nonce pair cannot be satisfied by a previous BM's store records.
            auto pair = prefix + (c.rank == 0 ? nonce + "/" + peerNonce : peerNonce + "/" + nonce);
            ret = store->Set(pair + "/matched/" + std::to_string(c.rank), "matched");
            if (ret != SM_OK) {
                return ret;
            }
            ret = store->Get(pair + "/matched/" + std::to_string(1 - c.rank), peer, 100);
            if (ret == SM_OK && peer == "matched") {
                readyKey = pair + "/ready/";
                return SM_OK;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SM_LOG_ERROR("sparse peer configuration mismatch/unavailable, rank=" << c.rank);
    return SM_TIMEOUT;
}
} // namespace

HostRdmaSparseLayout HostRdmaSparseLayout::Make(uint32_t count, uint64_t blockBytes, uint32_t links, uint32_t chunk)
{
    if (count == 0 || blockBytes == 0 || links == 0 || links > 64 || chunk == 0) {
        SM_LOG_ERROR("invalid sparse workspace shape: count=" << count << " links=" << links << " chunk=" << chunk);
        return {};
    }
    uint64_t slots = (static_cast<uint64_t>(count) + chunk - 1) / chunk * links;
    uint64_t overhead = (4 + 2ULL * count) * 8 + slots * 8 + links * 8 + 512;
    if (blockBytes > (UINT64_MAX - overhead) / count) {
        SM_LOG_ERROR("sparse workspace size overflow, count=" << count << " blockBytes=" << blockBytes);
        return {};
    }
    HostRdmaSparseLayout result{};
    result.message = Align(count * blockBytes);
    result.request = Align(result.message + (4 + 2ULL * count) * 8);
    result.done = result.request + 64;
    result.status = result.request + 128;
    result.signal = result.request + 192;
    result.watermark = result.request + 256;
    result.scratch = Align(result.watermark + links * 8);
    result.bytes = Align(result.scratch + slots * 8);
    return result;
}

int32_t RegisterHostRdmaSparseBackend(const HostRdmaSparseBackend *backend)
{
    SM_VALIDATE_RETURN(backend && backend->create && backend->destroy && backend->process, "invalid sparse backend",
                       SM_INVALID_PARAM);
    const HostRdmaSparseBackend *expected = nullptr;
    if (!Backend().compare_exchange_strong(expected, backend) && expected != backend) {
        SM_LOG_ERROR("HOST_RDMA sparse backend already registered");
        return SM_ERROR;
    }
    return SM_OK;
}

Result SmemBmEntry::BuildHostRdmaSparseConfig(const smem_bm_host_rdma_sparse_options_t &options,
                                              HostRdmaSparseConfig &c) const
{
    SM_VALIDATE_RETURN(inited_ && joined_ && !sparseUsed_, "sparse requires joined, unprepared BM", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(coreOptions_.rankCount == 2,
                       "sparse requires two ranks, rankCount: " << coreOptions_.rankCount, SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(coreOptions_.bmDataOpType == HYBM_DOP_TYPE_HOST_RDMA,
                       "sparse requires HOST_RDMA only, bmDataOpType: " << coreOptions_.bmDataOpType,
                       SM_INVALID_PARAM);
    c.options = options;
    c.rank = options_.rank;
    c.localGva = reinterpret_cast<uint64_t>(hostGva_) + coreOptions_.maxDRAMSize * c.rank;
    c.peerGva = reinterpret_cast<uint64_t>(hostGva_) + coreOptions_.maxDRAMSize * (1 - c.rank);
    c.localBytes = realDRAMSize_;
    std::string urls(coreOptions_.transUrl);
    c.links = std::count(urls.begin(), urls.end(), ';') + 1;
    auto ret = ReadMode(c.mode);
    if (ret != SM_OK) {
        return ret;
    }
    uint64_t lastVa = 0;
    SM_VALIDATE_RETURN(c.localBytes != 0 && options.timeoutMs != 0 &&
                           hybm_gva_to_va(c.localGva, HYBM_MEM_TYPE_HOST, &c.localVa) == 0 &&
                           hybm_gva_to_va(c.localGva + c.localBytes - 1, HYBM_MEM_TYPE_HOST, &lastVa) == 0 &&
                           lastVa - c.localVa == c.localBytes - 1,
                       "sparse requires contiguous local DRAM mapping", SM_INVALID_PARAM);
    return SM_OK;
}

Result SmemBmEntry::AttachHostRdmaSparse(const HostRdmaSparseConfig &config, const HostRdmaSparseBackend &backend)
{
    void *context = nullptr;
    auto ret = backend.create(config, reinterpret_cast<smem_bm_t>(this), &context);
    if (context != nullptr) {
        sparse_ = std::shared_ptr<void>(context, backend.destroy);
    }
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "cannot prepare sparse backend");
    SM_VALIDATE_RETURN(context != nullptr, "sparse backend returned no context", SM_ERROR);
    if (config.rank == 1) {
        const auto &options = config.options;
        auto layout = HostRdmaSparseLayout::Make(options.maxBlocks, options.maxBlockBytes, config.links,
                                                 options.progressInterval);
        auto requestVa = config.localVa + options.workspaceGva - config.localGva + layout.request;
        sparsePoller_ = std::make_unique<HostRdmaSparsePoller>(requestVa, context, backend.process);
    }
    return SM_OK;
}

Result SmemBmEntry::PrepareHostRdmaSparse(const smem_bm_host_rdma_sparse_options_t &options)
{
    std::lock_guard<std::mutex> lock(sparseMutex_);
    try {
        HostRdmaSparseConfig c;
        auto ret = BuildHostRdmaSparseConfig(options, c);
        if (ret != SM_OK) {
            return ret;
        }
        const auto *backend = Backend().load();
        SM_VALIDATE_RETURN(backend != nullptr, "load libmf_acc_offload before sparse preparation", SM_NOT_INITIALIZED);
        sparseUsed_ = true;
        std::string readyKey;
        ret = ExchangeSparseConfig(_configStore, c, readyKey);
        SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "sparse configuration exchange failed");
        SM_VALIDATE_RETURN(c.peerBytes <= coreOptions_.maxDRAMSize, "invalid peer sparse pool size", SM_INVALID_PARAM);
        ret = AttachHostRdmaSparse(c, *backend);
        if (ret == SM_OK) {
            ret = _configStore->Set(readyKey + std::to_string(c.rank), "ready");
        }
        std::string peer;
        if (ret == SM_OK) {
            ret = _configStore->Get(readyKey + std::to_string(1 - c.rank), peer, options.timeoutMs);
        }
        if (ret == SM_OK && peer == "ready") {
            return SM_OK;
        }
        SM_LOG_ERROR("sparse prepare/ready handshake failed, rank=" << c.rank << " ret=" << ret);
    } catch (const std::exception &error) {
        SM_LOG_ERROR("sparse preparation failed: " << error.what());
    }
    sparsePoller_.reset();
    sparse_.reset();
    return SM_ERROR;
}

Result SmemBmEntry::UseHostRdmaSparse(HostRdmaSparseVisitor visitor, void *args)
{
    std::lock_guard<std::mutex> lock(sparseMutex_);
    SM_VALIDATE_RETURN(sparse_ != nullptr && visitor != nullptr,
                       "call prepare_host_rdma_sparse before using sparse context", SM_NOT_INITIALIZED);
    return visitor(sparse_.get(), args);
}

int32_t HostRdmaSparsePoller::Poll(uint32_t timeoutMs)
{
    SM_VALIDATE_RETURN(!failed_ && requestVa_ != 0 && requestVa_ % 8 == 0 && context_ && process_,
                       "invalid/failed sparse poll context", SM_INVALID_PARAM);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        auto request = __atomic_load_n(reinterpret_cast<const uint64_t *>(requestVa_), __ATOMIC_ACQUIRE);
        if (request != sequence_) {
            int32_t ret = SM_ERROR;
            try {
                ret = process_(context_, request);
            } catch (const std::exception &error) {
                SM_LOG_ERROR("sparse poll handler failed: " << error.what());
            }
            if (ret != SM_OK) {
                failed_ = true;
                SM_LOG_ERROR("sparse poll request failed, sequence=" << request << " ret=" << ret);
                return ret < 0 ? ret : SM_ERROR;
            }
            sequence_ = request;
            return 1;
        }
        if (timeoutMs == 0) {
            break;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return 0;
}

int32_t SmemBmEntry::PollHostRdmaSparse(uint32_t timeoutMs)
{
    std::lock_guard<std::mutex> lock(sparseMutex_);
    SM_VALIDATE_RETURN(sparse_ != nullptr && sparsePoller_ != nullptr, "poll requires a prepared rank 1 BM",
                       SM_INVALID_PARAM);
    return sparsePoller_->Poll(timeoutMs);
}

void SmemBmEntry::StopHostRdmaSparse()
{
    std::lock_guard<std::mutex> lock(sparseMutex_);
    sparsePoller_.reset();
    sparse_.reset();
}
} // namespace ock::smem

using namespace ock::smem;
extern bool g_smemBmInited;

SMEM_API uint64_t smem_bm_host_rdma_sparse_workspace_size(uint32_t count, uint64_t bytes, uint32_t links,
                                                          uint32_t chunk)
{
    return HostRdmaSparseLayout::Make(count, bytes, links, chunk).bytes;
}

SMEM_API int32_t smem_bm_prepare_host_rdma_sparse(smem_bm_t handle, const smem_bm_host_rdma_sparse_options_t *options)
{
    SM_VALIDATE_RETURN(g_smemBmInited && handle != nullptr && options != nullptr,
                       "invalid/uninitialized sparse BM handle or options", SM_INVALID_PARAM);
    SmemBmEntryPtr entry;
    auto ret = SmemBmEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    SM_VALIDATE_RETURN(ret == SM_OK && entry != nullptr, "invalid sparse BM handle", SM_INVALID_PARAM);
    return entry->PrepareHostRdmaSparse(*options);
}

SMEM_API int32_t smem_bm_poll_host_rdma_sparse(smem_bm_t handle, uint32_t timeoutMs)
{
    SM_VALIDATE_RETURN(g_smemBmInited && handle != nullptr, "invalid/uninitialized sparse BM", SM_INVALID_PARAM);
    SmemBmEntryPtr entry;
    auto ret = SmemBmEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    SM_VALIDATE_RETURN(ret == SM_OK && entry != nullptr, "invalid sparse BM handle", SM_INVALID_PARAM);
    return entry->PollHostRdmaSparse(timeoutMs);
}

namespace ock::smem {
int32_t UseHostRdmaSparseContext(smem_bm_t handle, HostRdmaSparseVisitor visitor, void *args)
{
    SM_VALIDATE_RETURN(g_smemBmInited && handle != nullptr, "invalid/uninitialized sparse BM", SM_INVALID_PARAM);
    SmemBmEntryPtr entry;
    auto ret = SmemBmEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    SM_VALIDATE_RETURN(ret == SM_OK && entry != nullptr, "invalid sparse BM handle", SM_INVALID_PARAM);
    return entry->UseHostRdmaSparse(visitor, args);
}
} // namespace ock::smem
