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
#include "acc_offload_host_rdma_sparse.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include "smem_types.h"
#include "acc_offload_logger.h"

namespace ock::offload {
using namespace smem;
namespace {
using Clock = std::chrono::steady_clock;
} // namespace

HostRdmaSparse::HostRdmaSparse(const HostRdmaSparseConfig &config, Copy copy, Batch batch)
    : config_(config), layout_(HostRdmaSparseLayout::Make(config.options.maxBlocks, config.options.maxBlockBytes,
                                                          config.links, config.options.progressInterval)),
      copy_(std::move(copy)), batch_(std::move(batch)), workspaceOffset_(config.options.workspaceGva - config.localGva)
{}

HostRdmaSparse::~HostRdmaSparse()
{
    Stop();
}
uint64_t HostRdmaSparse::Local(uint64_t offset) const
{
    return config_.options.workspaceGva + offset;
}
uint64_t HostRdmaSparse::Remote(uint64_t offset) const
{
    return config_.peerGva + workspaceOffset_ + offset;
}
uint64_t HostRdmaSparse::Va(uint64_t offset) const
{
    return config_.localVa + workspaceOffset_ + offset;
}
uint64_t HostRdmaSparse::Load(uint64_t offset) const
{
    return __atomic_load_n(reinterpret_cast<const uint64_t *>(Va(offset)), __ATOMIC_ACQUIRE);
}

bool HostRdmaSparse::ValidRange(uint64_t address, uint64_t bytes, bool local, bool workspace) const
{
    auto base = local ? config_.localGva : config_.peerGva;
    auto capacity = local ? config_.localBytes : config_.peerBytes;
    if (address < base || bytes == 0 || bytes > capacity || address - base > capacity - bytes) {
        return false;
    }
    auto offset = address - base;
    return workspace || offset + bytes <= workspaceOffset_ || offset >= workspaceOffset_ + layout_.bytes;
}

int32_t HostRdmaSparse::Prepare()
{
    const auto &o = config_.options;
    if (started_ || stopped_ || layout_.bytes == 0 || o.workspaceBytes < layout_.bytes || o.workspaceGva % 64 != 0 ||
        config_.rank > 1 || o.timeoutMs == 0 || config_.localVa == 0 || config_.localVa % 8 != 0 ||
        o.gatherThreads == 0 || o.gatherThreads > 64 || o.scatterThreads == 0 || o.scatterThreads > 64 ||
        !ValidRange(Local(0), layout_.bytes, true, true) || !ValidRange(Remote(0), layout_.bytes, false, true)) {
        OFFLOAD_LOG_ERROR("invalid sparse preparation, rank=" << config_.rank
                                                              << " workspaceBytes=" << o.workspaceBytes);
        return SM_INVALID_PARAM;
    }
    // The caller has reserved this region. Only protocol words need initialization.
    std::memset(reinterpret_cast<void *>(Va(layout_.message)), 0, layout_.bytes - layout_.message);
    try {
        if (config_.mode == HostRdmaSparseMode::GATHER) {
            workers_ = std::make_unique<ExecutorService>(config_.rank == 1 ? o.gatherThreads : o.scatterThreads);
            if (!workers_->Start()) {
                OFFLOAD_LOG_ERROR("cannot start sparse CPU workers, rank=" << config_.rank);
                return SM_ERROR;
            }
        }
        started_ = true;
        return SM_OK;
    } catch (const std::exception &error) {
        OFFLOAD_LOG_ERROR("cannot prepare sparse operator: " << error.what());
        return SM_ERROR;
    }
}

void HostRdmaSparse::Stop()
{
    stopped_ = true;
    std::lock_guard<std::mutex> lock(copyMutex_);
    workers_.reset();
}

bool HostRdmaSparse::ValidRequest(const uint64_t *sources, const uint64_t *destinations, uint32_t count,
                                  uint64_t bytes) const
{
    if (sources == nullptr || destinations == nullptr || count == 0 || count > config_.options.maxBlocks ||
        bytes == 0 || bytes > config_.options.maxBlockBytes) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!ValidRange(sources[i], bytes, false) || !ValidRange(destinations[i], bytes, true)) {
            return false;
        }
    }
    std::vector<uint64_t> sorted(destinations, destinations + count);
    std::sort(sorted.begin(), sorted.end());
    for (uint32_t i = 1; i < count; ++i) {
        if (sorted[i] - sorted[i - 1] < bytes) {
            return false;
        }
    }
    return true;
}

int32_t HostRdmaSparse::Publish(uint64_t offset, uint64_t value)
{
    __atomic_store_n(reinterpret_cast<uint64_t *>(Va(layout_.signal)), value, __ATOMIC_RELEASE);
    return copy_(Local(layout_.signal), Remote(offset), sizeof(value));
}

int32_t HostRdmaSparse::Wait(uint64_t offset, uint64_t value)
{
    auto deadline = Clock::now() + std::chrono::milliseconds(config_.options.timeoutMs);
    while (!stopped_ && Clock::now() < deadline) {
        if (Load(offset) == value) {
            return SM_OK;
        }
        std::this_thread::yield();
    }
    OFFLOAD_LOG_ERROR("sparse wait interrupted/timed out, rank=" << config_.rank << " sequence=" << sequence_);
    return SM_TIMEOUT;
}

int32_t HostRdmaSparse::Submit(const uint64_t *sources, const uint64_t *destinations, uint32_t count, uint64_t bytes)
{
    uint64_t agg = config_.mode == HostRdmaSparseMode::CONT ? 16 : 1;
    uint64_t dstCount = config_.mode == HostRdmaSparseMode::GATHER ? 1 : (count + agg - 1) / agg;
    auto words = reinterpret_cast<uint64_t *>(Va(layout_.message));
    words[0] = count;
    words[1] = bytes;
    words[2] = dstCount;
    words[3] = agg;
    std::copy_n(sources, count, words + 4);
    for (uint64_t i = 0; i < dstCount; ++i) {
        words[4 + count + i] = config_.mode == HostRdmaSparseMode::BASELINE ? destinations[i] : Local(i * agg * bytes);
    }
    auto ret = copy_(Local(layout_.message), Remote(layout_.message), (4 + count + dstCount) * 8);
    return ret == SM_OK ? Publish(layout_.request, sequence_) : ret;
}

void HostRdmaSparse::CpuCopy(const std::vector<uint64_t> &sources, const std::vector<uint64_t> &destinations,
                             uint64_t bytes)
{
    uint32_t threads = config_.rank == 1 ? config_.options.gatherThreads : config_.options.scatterThreads;
    std::vector<std::future<void>> futures;
    // Wait for every submitted job even on allocation/submission failure.
    try {
        futures.reserve(threads);
        for (uint32_t t = 0; t < threads; ++t) {
            auto task = std::make_shared<std::packaged_task<void()>>([&, t] {
                for (size_t i = sources.size() * t / threads; i < sources.size() * (t + 1) / threads; ++i) {
                    std::memcpy(reinterpret_cast<void *>(destinations[i]), reinterpret_cast<void *>(sources[i]), bytes);
                }
            });
            futures.push_back(task->get_future());
            if (!workers_->Execute([task] { (*task)(); })) {
                (*task)();
            }
        }
    } catch (...) {
        for (auto &future : futures) {
            future.wait();
        }
        throw;
    }
    for (auto &future : futures) {
        future.get();
    }
}

int32_t HostRdmaSparse::ConsumeProgress(const uint64_t *destinations, uint32_t count, uint64_t bytes)
{
    std::vector<uint32_t> consumed(config_.links), ends(config_.links);
    uint32_t begin = 0;
    for (uint32_t rail = 0; rail < config_.links; ++rail) {
        consumed[rail] = begin;
        begin += count / config_.links + (rail < count % config_.links);
        ends[rail] = begin;
    }
    auto deadline = Clock::now() + std::chrono::milliseconds(config_.options.timeoutMs);
    auto base = (sequence_ - 1) * config_.options.maxBlocks;
    while (consumed != ends && !stopped_ && Clock::now() < deadline) {
        if (Load(layout_.done) == sequence_ && Load(layout_.status) != 0) {
            OFFLOAD_LOG_ERROR("peer failed sparse cont request, sequence=" << sequence_);
            return SM_ERROR;
        }
        for (uint32_t rail = 0; rail < config_.links; ++rail) {
            auto watermark = Load(layout_.watermark + rail * 8);
            uint64_t upto = watermark > base ? std::min(watermark - base, uint64_t(ends[rail])) : 0;
            while (consumed[rail] < upto) {
                auto i = consumed[rail]++;
                std::memcpy(reinterpret_cast<void *>(config_.localVa + destinations[i] - config_.localGva),
                            reinterpret_cast<void *>(Va(i * bytes)), bytes);
            }
        }
        std::this_thread::yield();
    }
    if (consumed == ends) {
        return SM_OK;
    }
    OFFLOAD_LOG_ERROR("sparse progress interrupted/timed out, sequence=" << sequence_);
    return SM_TIMEOUT;
}

int32_t HostRdmaSparse::Receive(const uint64_t *destinations, uint32_t count, uint64_t bytes)
{
    auto ret = config_.mode == HostRdmaSparseMode::CONT ? ConsumeProgress(destinations, count, bytes) : SM_OK;
    if (ret != SM_OK) {
        return ret;
    }
    ret = Wait(layout_.done, sequence_);
    if (ret != SM_OK) {
        return ret;
    }
    if (Load(layout_.status) != 0) {
        OFFLOAD_LOG_ERROR("peer failed sparse copy, sequence=" << sequence_);
        return SM_ERROR;
    }
    if (config_.mode == HostRdmaSparseMode::GATHER) {
        std::vector<uint64_t> sources(count), targets(count);
        for (uint32_t i = 0; i < count; ++i) {
            sources[i] = Va(i * bytes);
            targets[i] = config_.localVa + destinations[i] - config_.localGva;
        }
        CpuCopy(sources, targets, bytes);
    }
    return SM_OK;
}

int32_t HostRdmaSparse::Run(const uint64_t *sources, const uint64_t *destinations, uint32_t count, uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(copyMutex_);
    try {
        if (!started_ || stopped_ || failed_ || config_.rank != 0 ||
            sequence_ >= UINT64_MAX / config_.options.maxBlocks || !ValidRequest(sources, destinations, count, bytes)) {
            OFFLOAD_LOG_ERROR("invalid sparse copy state/addresses, rank=" << config_.rank << " count=" << count);
            return SM_INVALID_PARAM;
        }
        ++sequence_;
        auto ret = Submit(sources, destinations, count, bytes);
        if (ret == SM_OK) {
            ret = Receive(destinations, count, bytes);
        }
        failed_ = ret != SM_OK;
        return ret;
    } catch (const std::exception &error) {
        failed_ = true;
        OFFLOAD_LOG_ERROR("sparse copy failed, sequence=" << sequence_ << ": " << error.what());
        return SM_ERROR;
    }
}

int32_t HostRdmaSparse::Transfer(const std::vector<uint64_t> &sources, std::vector<uint64_t> &destinations,
                                 uint64_t bytes)
{
    auto count = sources.size();
    if (config_.mode == HostRdmaSparseMode::GATHER) {
        std::vector<uint64_t> nativeSources(count), aggregate(count);
        for (size_t i = 0; i < count; ++i) {
            nativeSources[i] = config_.localVa + sources[i] - config_.localGva;
            aggregate[i] = Va(i * bytes);
        }
        CpuCopy(nativeSources, aggregate, bytes);
        return copy_(Local(0), destinations[0], count * bytes);
    }
    std::vector<void *> src(count);
    std::vector<void *> dst(count);
    std::vector<size_t> sizes(count, bytes);
    for (size_t i = 0; i < count; ++i) {
        src[i] = reinterpret_cast<void *>(sources[i]);
        dst[i] = reinterpret_cast<void *>(destinations[i]);
    }
    smem_batch_copy_params params{};
    params.sources = src.data();
    params.destinations = dst.data();
    params.dataSizes = sizes.data();
    params.batchSize = count;
    if (config_.mode == HostRdmaSparseMode::CONT) {
        params.progressSrc = reinterpret_cast<void *>(Va(layout_.scratch));
        params.progressDest = reinterpret_cast<void *>(Remote(layout_.watermark));
        params.progressBase = (sequence_ - 1) * config_.options.maxBlocks;
        params.progressInterval = config_.options.progressInterval;
    }
    return batch_(&params);
}

int32_t HostRdmaSparse::Serve()
{
    auto words = reinterpret_cast<const uint64_t *>(Va(layout_.message));
    uint64_t count = words[0], bytes = words[1];
    uint64_t agg = config_.mode == HostRdmaSparseMode::CONT ? 16 : 1;
    uint64_t dstCount = config_.mode == HostRdmaSparseMode::GATHER ? 1 : (count + agg - 1) / agg;
    if (count == 0 || count > config_.options.maxBlocks || bytes == 0 || bytes > config_.options.maxBlockBytes ||
        words[2] != dstCount || words[3] != agg) {
        OFFLOAD_LOG_ERROR("invalid sparse address message header, sequence=" << sequence_);
        return SM_INVALID_PARAM;
    }
    std::vector<uint64_t> sources(words + 4, words + 4 + count), destinations;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t target = words[4 + count + (config_.mode == HostRdmaSparseMode::GATHER ? 0 : i / agg)];
        if (config_.mode != HostRdmaSparseMode::GATHER) {
            target += (i % agg) * bytes;
        }
        bool validTarget = config_.mode == HostRdmaSparseMode::BASELINE
                               ? ValidRange(target, bytes, false)
                               : target == Remote(config_.mode == HostRdmaSparseMode::GATHER ? 0 : i * bytes);
        if (!ValidRange(sources[i], bytes, true) || !validTarget) {
            OFFLOAD_LOG_ERROR("invalid sparse address message range, index=" << i);
            return SM_INVALID_PARAM;
        }
        destinations.push_back(target);
    }
    return Transfer(sources, destinations, bytes);
}

int32_t HostRdmaSparse::ProcessRequest(uint64_t request)
{
    std::lock_guard<std::mutex> lock(copyMutex_);
    if (!started_ || stopped_ || failed_ || config_.rank != 1) {
        OFFLOAD_LOG_ERROR("invalid sparse provider state, rank=" << config_.rank);
        return SM_INVALID_PARAM;
    }
    int32_t ret = SM_ERROR;
    try {
        if (request == sequence_ + 1) {
            sequence_ = request;
            ret = Serve();
        } else {
            OFFLOAD_LOG_ERROR("out-of-order sparse request=" << request << " previous=" << sequence_);
        }
    } catch (const std::exception &error) {
        OFFLOAD_LOG_ERROR("sparse request failed: " << error.what());
    }
    if (ret != SM_OK) {
        OFFLOAD_LOG_ERROR("sparse request failed, sequence=" << request << " ret=" << ret);
    }
    // Completion follows synchronous data/progress completion on every link.
    try {
        auto statusRet = Publish(layout_.status, ret == SM_OK ? 0 : 1);
        auto doneRet = statusRet == SM_OK ? Publish(layout_.done, request) : statusRet;
        if (ret == SM_OK) {
            ret = doneRet;
        }
    } catch (const std::exception &error) {
        ret = SM_ERROR;
        OFFLOAD_LOG_ERROR("cannot publish sparse completion: " << error.what());
    }
    failed_ = ret != SM_OK;
    return ret;
}
} // namespace ock::offload
