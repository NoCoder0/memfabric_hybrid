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
#include <gtest/gtest.h>
#include <cstring>
#include <future>
#include "acc_offload_host_rdma_sparse.h"
#include "smem_types.h"

using namespace ock::smem;
using namespace ock::offload;

namespace {
// Emulate two registered pools with deliberately different GVA and VA values.
class SparsePair {
public:
    static constexpr uint64_t CAPACITY = 65536;
    static constexpr uint64_t BASE = 0x100000000ULL;
    std::vector<uint64_t> memory[2] = {std::vector<uint64_t>(CAPACITY / 8), std::vector<uint64_t>(CAPACITY / 8)};
    HostRdmaSparseConfig config[2];
    std::unique_ptr<HostRdmaSparse> endpoints[2];
    std::atomic<uint32_t> batches{0}, progressBatches{0}, aggregateWrites{0};
    bool failBatch = false;
    bool dropRequest = false;
    bool delayBatch = false;
    std::atomic<bool> batchReturned{false};
    std::unique_ptr<HostRdmaSparsePoller> poller;
    std::atomic<bool> keepPolling{false};
    std::thread applicationThread;

    ~SparsePair()
    {
        StopPolling();
        for (auto &endpoint : endpoints) {
            endpoint.reset();
        }
    }

    explicit SparsePair(HostRdmaSparseMode mode, uint32_t links = 3)
    {
        auto required = HostRdmaSparseLayout::Make(33, 64, links, 4).bytes;
        for (uint32_t rank = 0; rank < 2; ++rank) {
            auto &c = config[rank];
            c.rank = rank;
            c.mode = mode;
            c.links = links;
            c.localGva = BASE + rank * CAPACITY;
            c.peerGva = BASE + (1 - rank) * CAPACITY;
            c.localVa = reinterpret_cast<uint64_t>(memory[rank].data());
            c.localBytes = c.peerBytes = CAPACITY;
            c.options = {c.localGva + 8192, required, 64, 33, 4, 300, 3, 2};
            endpoints[rank] = std::make_unique<HostRdmaSparse>(
                c, [this](uint64_t s, uint64_t d, uint64_t n) { return Copy(s, d, n); },
                [this, links](smem_batch_copy_params *p) { return Batch(p, links); });
        }
    }

    uint8_t *Ptr(uint64_t gva)
    {
        auto offset = gva - BASE;
        return reinterpret_cast<uint8_t *>(memory[offset / CAPACITY].data()) + offset % CAPACITY;
    }

    int32_t Copy(uint64_t source, uint64_t destination, uint64_t bytes)
    {
        auto layout = HostRdmaSparseLayout::Make(33, 64, config[0].links, 4);
        if (dropRequest && destination == config[1].options.workspaceGva + layout.request) {
            return SM_OK;
        }
        if (source == config[1].options.workspaceGva) {
            ++aggregateWrites;
        }
        if (bytes == 8 && destination % 8 == 0) {
            uint64_t value;
            std::memcpy(&value, Ptr(source), 8);
            __atomic_store_n(reinterpret_cast<uint64_t *>(Ptr(destination)), value, __ATOMIC_RELEASE);
        } else {
            std::memcpy(Ptr(destination), Ptr(source), bytes);
        }
        return SM_OK;
    }

    int32_t Batch(smem_batch_copy_params *p, uint32_t links)
    {
        ++batches;
        if (failBatch) {
            return SM_ERROR;
        }
        if (p->progressInterval != 0) {
            ++progressBatches;
        }
        uint32_t begin = 0;
        for (uint32_t rail = 0; rail < links; ++rail) {
            uint32_t end = begin + p->batchSize / links + (rail < p->batchSize % links);
            uint32_t chunkIndex = 0;
            for (uint32_t i = begin; i < end; ++i) {
                Copy(reinterpret_cast<uint64_t>(p->sources[i]), reinterpret_cast<uint64_t>(p->destinations[i]),
                     p->dataSizes[i]);
                if (p->progressInterval != 0 && ((i + 1 - begin) % p->progressInterval == 0 || i + 1 == end)) {
                    auto slot = reinterpret_cast<uint64_t *>(p->progressSrc) + chunkIndex++ * links + rail;
                    *slot = p->progressBase + i + 1;
                    auto watermark = reinterpret_cast<uint64_t *>(Ptr(reinterpret_cast<uint64_t>(p->progressDest)));
                    __atomic_store_n(watermark + rail, *slot, __ATOMIC_RELEASE);
                }
            }
            begin = end;
        }
        if (delayBatch) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        batchReturned = true;
        return SM_OK;
    }

    void Start(bool pollInApplication = true)
    {
        ASSERT_EQ(endpoints[0]->Prepare(), SM_OK);
        ASSERT_EQ(endpoints[1]->Prepare(), SM_OK);
        auto layout = HostRdmaSparseLayout::Make(33, 64, config[1].links, 4);
        auto requestVa = reinterpret_cast<uint64_t>(Ptr(config[1].options.workspaceGva + layout.request));
        poller =
            std::make_unique<HostRdmaSparsePoller>(requestVa, endpoints[1].get(), [](void *context, uint64_t request) {
                return static_cast<HostRdmaSparse *>(context)->ProcessRequest(request);
            });
        if (pollInApplication) {
            keepPolling = true;
            applicationThread = std::thread([this] {
                while (keepPolling && poller->Poll(1) >= 0) {}
            });
        }
    }

    void StopPolling()
    {
        keepPolling = false;
        if (applicationThread.joinable()) {
            applicationThread.join();
        }
    }

    void WaitForRequest()
    {
        auto layout = HostRdmaSparseLayout::Make(33, 64, config[1].links, 4);
        auto word = reinterpret_cast<uint64_t *>(Ptr(config[1].options.workspaceGva + layout.request));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (__atomic_load_n(word, __ATOMIC_ACQUIRE) == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        ASSERT_EQ(__atomic_load_n(word, __ATOMIC_ACQUIRE), 1);
    }

    void VerifyCopy(uint32_t count, uint64_t bytes, uint8_t seed)
    {
        std::vector<uint64_t> sources, destinations;
        for (uint32_t i = 0; i < count; ++i) {
            sources.push_back(config[1].localGva + (count - i) * 128);
            destinations.push_back(config[0].localGva + i * 128);
            std::memset(Ptr(sources.back()), seed + i, bytes);
            std::memset(Ptr(destinations.back()), 0xFE, bytes + 1);
        }
        ASSERT_EQ(endpoints[0]->Run(sources.data(), destinations.data(), count, bytes), SM_OK);
        for (uint32_t i = 0; i < count; ++i) {
            EXPECT_EQ(std::memcmp(Ptr(sources[i]), Ptr(destinations[i]), bytes), 0) << "block " << i;
            EXPECT_EQ(Ptr(destinations[i])[bytes], 0xFE) << "overrun block " << i;
        }
    }
};
} // namespace

TEST(HostRdmaSparseTest, ThreeModesPreserveOrderAcrossVariableRequestsAndRails)
{
    for (auto mode : {HostRdmaSparseMode::BASELINE, HostRdmaSparseMode::CONT, HostRdmaSparseMode::GATHER}) {
        for (uint32_t links : {1U, 3U}) {
            SparsePair pair(mode, links);
            pair.Start();
            pair.VerifyCopy(33, 13, 3); // cont compressed destination tail and chunk tail
            pair.VerifyCopy(2, 64, 7);  // fewer blocks than links; stale watermarks from previous request
            pair.VerifyCopy(17, 31, 10);
            EXPECT_EQ(pair.batches.load(), mode == HostRdmaSparseMode::GATHER ? 0 : 3);
            EXPECT_EQ(pair.progressBatches.load(), mode == HostRdmaSparseMode::CONT ? 3 : 0);
            EXPECT_EQ(pair.aggregateWrites.load(), mode == HostRdmaSparseMode::GATHER ? 3 : 0);
        }
    }
}

TEST(HostRdmaSparseTest, TimingTracksCompletedRequestsAndApplicableStages)
{
    for (auto mode : {HostRdmaSparseMode::BASELINE, HostRdmaSparseMode::CONT, HostRdmaSparseMode::GATHER}) {
        SparsePair pair(mode);
        offload_host_rdma_sparse_timing_v2_t local{}, remote{};
        EXPECT_EQ(pair.endpoints[0]->LastTiming(local), SM_INVALID_PARAM);
        pair.delayBatch = mode != HostRdmaSparseMode::GATHER;
        pair.Start();
        for (uint64_t sequence = 1; sequence <= 2; ++sequence) {
            pair.VerifyCopy(17, 31, 10);
            ASSERT_EQ(pair.endpoints[0]->LastTiming(local), SM_OK);
            ASSERT_EQ(pair.endpoints[1]->LastTiming(remote), SM_OK);
            EXPECT_EQ(local.sequence, sequence);
            EXPECT_EQ(remote.sequence, sequence);
            offload_host_rdma_sparse_timing_t legacy{};
            ASSERT_EQ(pair.endpoints[0]->LastTiming(legacy), SM_OK);
            EXPECT_EQ(legacy.sequence, local.sequence);
            EXPECT_EQ(legacy.requestNs, local.requestNs);
            EXPECT_EQ(legacy.scatterNs, local.scatterNs);
            EXPECT_EQ(local.waitRemoteNs > 0, mode != HostRdmaSparseMode::CONT);
            EXPECT_EQ(local.receiveScatterNs > 0, mode == HostRdmaSparseMode::CONT);
            EXPECT_EQ(remote.gatherWriteNs > 0, mode == HostRdmaSparseMode::GATHER);
            if (mode == HostRdmaSparseMode::GATHER) {
                EXPECT_GE(remote.gatherWriteNs, remote.gatherNs + remote.writeNs);
            }
            if (mode == HostRdmaSparseMode::CONT) {
                EXPECT_GE(local.receiveScatterNs, local.scatterNs);
            }
            EXPECT_GT(local.requestNs, 0);
            EXPECT_GT(remote.writeNs, 0);
            EXPECT_EQ(local.gatherNs, 0);
            EXPECT_EQ(local.writeNs, 0);
            EXPECT_EQ(remote.requestNs, 0);
            EXPECT_EQ(remote.scatterNs, 0);
            EXPECT_EQ(remote.gatherNs > 0, mode == HostRdmaSparseMode::GATHER);
            EXPECT_EQ(local.scatterNs > 0, mode != HostRdmaSparseMode::BASELINE);
            if (pair.delayBatch) {
                EXPECT_GE(remote.writeNs, 30000000);
                EXPECT_GE(local.waitRemoteNs + local.receiveScatterNs, 30000000);
            }
        }
        pair.endpoints[0]->Stop();
        EXPECT_EQ(pair.endpoints[0]->LastTiming(local), SM_INVALID_PARAM);
    }
}

TEST(HostRdmaSparseTest, OriginalGatherCpuListSyntaxAndAutomaticSelection)
{
    std::vector<int> cpus;
    ASSERT_TRUE(ParseCpuList("0-2,5,7-8", cpus));
    EXPECT_EQ(cpus, (std::vector<int>{0, 1, 2, 5, 7, 8}));
    for (const auto *invalid : {"", "1,1", "2-1", "1,", "1,,2", "-1", "abc", "999999"}) {
        cpus.clear();
        EXPECT_FALSE(ParseCpuList(invalid, cpus));
    }
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    ASSERT_EQ(sched_getaffinity(0, sizeof(allowed), &allowed), 0);
    cpus = GetGatherCpus(3);
    ASSERT_FALSE(cpus.empty());
    EXPECT_LE(cpus.size(), 3U);
    for (int cpu : cpus) {
        EXPECT_TRUE(CPU_ISSET(cpu, &allowed));
    }
}

TEST(HostRdmaSparseTest, CasePreparationReusesTargetsAndKeepsSequenceAcrossCases)
{
    SparsePair pair(HostRdmaSparseMode::GATHER);
    pair.Start();
    uint64_t sequence = 0;
    for (uint32_t count : {17U, 2U, 33U}) {
        std::vector<uint64_t> targets(count);
        for (uint32_t i = 0; i < count; ++i) {
            targets[i] = pair.config[0].localGva + i * 128;
        }
        ASSERT_EQ(pair.endpoints[0]->PrepareCase(targets.data(), count, 13), SM_OK);
        ASSERT_EQ(pair.endpoints[1]->PrepareCase(nullptr, count, 13), SM_OK);
        for (unsigned round = 0; round < 2; ++round) {
            pair.VerifyCopy(count, 13, 7);
            offload_host_rdma_sparse_timing_t timing{};
            ASSERT_EQ(pair.endpoints[0]->LastTiming(timing), SM_OK);
            EXPECT_EQ(timing.sequence, ++sequence);
        }
    }
    EXPECT_EQ(pair.endpoints[0]->PrepareCase(nullptr, 2, 13), SM_INVALID_PARAM);
    EXPECT_EQ(pair.endpoints[1]->PrepareCase(nullptr, 34, 13), SM_INVALID_PARAM);
}

TEST(HostRdmaSparseTest, RejectsInvalidRangesOverlapsAndProviderCallsWithoutPoisoning)
{
    SparsePair pair(HostRdmaSparseMode::BASELINE);
    pair.Start();
    uint64_t sources[] = {pair.config[1].localGva, pair.config[1].localGva + 128};
    uint64_t targets[] = {pair.config[0].localGva, pair.config[0].localGva + 1};
    EXPECT_EQ(pair.endpoints[0]->Run(sources, targets, 2, 8), SM_INVALID_PARAM);
    targets[0] = pair.config[0].options.workspaceGva;
    EXPECT_EQ(pair.endpoints[0]->Run(sources, targets, 1, 8), SM_INVALID_PARAM);
    targets[0] = pair.config[0].localGva + SparsePair::CAPACITY - 1;
    EXPECT_EQ(pair.endpoints[0]->Run(sources, targets, 1, 8), SM_INVALID_PARAM);
    EXPECT_EQ(pair.endpoints[0]->Run(nullptr, targets, 1, 8), SM_INVALID_PARAM);
    EXPECT_EQ(pair.endpoints[1]->Run(sources, targets, 1, 8), SM_INVALID_PARAM);
    EXPECT_EQ(pair.batches.load(), 0);
    pair.VerifyCopy(2, 13, 17);
}

TEST(HostRdmaSparseTest, PeerFailureIsReportedAndContextCannotBeReused)
{
    for (auto mode : {HostRdmaSparseMode::BASELINE, HostRdmaSparseMode::CONT}) {
        SparsePair pair(mode);
        pair.failBatch = true;
        pair.Start();
        uint64_t source = pair.config[1].localGva, target = pair.config[0].localGva;
        EXPECT_EQ(pair.endpoints[0]->Run(&source, &target, 1, 8), SM_ERROR);
        EXPECT_EQ(pair.endpoints[0]->Run(&source, &target, 1, 8), SM_INVALID_PARAM);
        EXPECT_EQ(pair.batches.load(), 1);
    }
}

TEST(HostRdmaSparseTest, TimeoutPoisonsContextAndApplicationStopsPolling)
{
    SparsePair pair(HostRdmaSparseMode::GATHER);
    pair.dropRequest = true;
    pair.Start();
    uint64_t source = pair.config[1].localGva, target = pair.config[0].localGva;
    EXPECT_EQ(pair.endpoints[0]->Run(&source, &target, 1, 8), SM_TIMEOUT);
    EXPECT_EQ(pair.endpoints[0]->Run(&source, &target, 1, 8), SM_INVALID_PARAM);
    pair.StopPolling();
    pair.endpoints[1]->Stop();
    pair.endpoints[1]->Stop();
}

TEST(HostRdmaSparseTest, WorkspaceSizingRejectsOverflowAndZeroDimensions)
{
    EXPECT_EQ(HostRdmaSparseLayout::Make(0, 1, 1, 1).bytes, 0);
    EXPECT_EQ(HostRdmaSparseLayout::Make(1, UINT64_MAX, 1, 1).bytes, 0);
    EXPECT_EQ(HostRdmaSparseLayout::Make(1, 1, 65, 1).bytes, 0);
    EXPECT_EQ(HostRdmaSparseLayout::Make(1, 1, 1, 0).bytes, 0);
    EXPECT_NE(HostRdmaSparseLayout::Make(33, 64, 3, 4).bytes, 0);
}

TEST(HostRdmaSparseTest, ContWaitsForServiceCompletionAfterConsumingWatermarks)
{
    SparsePair pair(HostRdmaSparseMode::CONT);
    pair.delayBatch = true;
    pair.Start();
    pair.VerifyCopy(17, 31, 12);
    EXPECT_TRUE(pair.batchReturned.load());
}

TEST(HostRdmaSparseTest, PrepareDoesNotProcessUntilApplicationPolls)
{
    SparsePair pair(HostRdmaSparseMode::BASELINE);
    pair.Start(false);
    EXPECT_EQ(pair.poller->Poll(0), 0);
    auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(pair.poller->Poll(10), 0);
    EXPECT_GE(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(10));
    uint64_t source = pair.config[1].localGva, target = pair.config[0].localGva;
    std::memset(pair.Ptr(source), 0x5A, 13);
    auto copy = std::async(std::launch::async, [&] { return pair.endpoints[0]->Run(&source, &target, 1, 13); });
    pair.WaitForRequest();
    EXPECT_EQ(copy.wait_for(std::chrono::milliseconds(5)), std::future_status::timeout);
    EXPECT_EQ(pair.batches.load(), 0);
    EXPECT_EQ(pair.poller->Poll(0), 1);
    EXPECT_EQ(copy.get(), SM_OK);
    EXPECT_EQ(std::memcmp(pair.Ptr(source), pair.Ptr(target), 13), 0);
    EXPECT_EQ(pair.poller->Poll(0), 0); // Do not process the same request twice.
    EXPECT_EQ(pair.batches.load(), 1);
}

TEST(HostRdmaSparseTest, PollReturnsErrorAndRejectsFurtherRequests)
{
    SparsePair pair(HostRdmaSparseMode::CONT);
    pair.failBatch = true;
    pair.Start(false);
    uint64_t source = pair.config[1].localGva, target = pair.config[0].localGva;
    auto copy = std::async(std::launch::async, [&] { return pair.endpoints[0]->Run(&source, &target, 1, 13); });
    pair.WaitForRequest();
    EXPECT_EQ(pair.poller->Poll(0), SM_ERROR);
    EXPECT_EQ(copy.get(), SM_ERROR);
    EXPECT_EQ(pair.poller->Poll(0), SM_INVALID_PARAM);
}

TEST(HostRdmaSparseTest, PollIdleDeadlineDoesNotInterruptAcceptedCopy)
{
    SparsePair pair(HostRdmaSparseMode::CONT);
    pair.delayBatch = true;
    pair.Start(false);
    uint64_t source = pair.config[1].localGva, target = pair.config[0].localGva;
    auto copy = std::async(std::launch::async, [&] { return pair.endpoints[0]->Run(&source, &target, 1, 13); });
    pair.WaitForRequest();
    EXPECT_EQ(pair.poller->Poll(1), 1); // Simulated batch takes 30 ms after accepting the request.
    EXPECT_TRUE(pair.batchReturned.load());
    EXPECT_EQ(copy.get(), SM_OK);
}
