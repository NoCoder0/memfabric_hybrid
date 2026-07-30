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

#define private public
#include "host_urma_transport_manager.h"
#undef private

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::host;

namespace {
constexpr uint32_t REMOTE_RANK = 1U;
constexpr ChannelHandle TEST_CHANNEL = 7U;
constexpr uint64_t LOCAL_ADDR = 0x100000UL;
constexpr uint64_t REMOTE_ADDR = 0x200000UL;
constexpr uint64_t HCOMM_ADDR = 0x300000UL;
constexpr uint64_t TEST_SIZE = 576UL;
constexpr int32_t HCOMM_E_AGAIN = 20;
uint32_t g_writeCount = 0;
uint32_t g_fenceCount = 0;

struct HcommSubmitGuard {
    hcommWriteOnThreadFunc oldWrite{DlHcommApi::gHcommWriteOnThread};
    hcommChannelFenceOnThreadFunc oldFence{DlHcommApi::gHcommChannelFenceOnThread};

    ~HcommSubmitGuard()
    {
        DlHcommApi::gHcommWriteOnThread = oldWrite;
        DlHcommApi::gHcommChannelFenceOnThread = oldFence;
    }
};

int32_t WriteAgainThenSuccess(ThreadHandle, ChannelHandle channel, void *dst, const void *src, uint64_t size)
{
    EXPECT_EQ(channel, TEST_CHANNEL);
    EXPECT_EQ(dst, reinterpret_cast<void *>(HCOMM_ADDR));
    EXPECT_EQ(src, reinterpret_cast<const void *>(LOCAL_ADDR));
    EXPECT_EQ(size, TEST_SIZE);
    return g_writeCount++ == 0 ? HCOMM_E_AGAIN : 0;
}

int32_t FenceSuccess(ThreadHandle, ChannelHandle channel)
{
    EXPECT_EQ(channel, TEST_CHANNEL);
    ++g_fenceCount;
    return 0;
}
} // namespace

TEST(HostUrmaTransportManagerTest, WriteRemoteRetriesAfterQueueFull)
{
    HcommSubmitGuard guard;
    DlHcommApi::gHcommWriteOnThread = WriteAgainThenSuccess;
    DlHcommApi::gHcommChannelFenceOnThread = FenceSuccess;
    g_writeCount = 0;
    g_fenceCount = 0;

    HostUrmaTransportManager manager;
    auto &state = manager.remoteRanks_[REMOTE_RANK];
    state.channel = TEST_CHANNEL;
    HostUrmaTransportManager::RemoteRegistration registration{};
    registration.exportedAddr = REMOTE_ADDR;
    registration.size = TEST_SIZE;
    registration.view.addr = HCOMM_ADDR;
    registration.view.size = TEST_SIZE;
    state.imports.push_back(registration);

    EXPECT_EQ(manager.WriteRemoteAsync(REMOTE_RANK, LOCAL_ADDR, REMOTE_ADDR, TEST_SIZE), BM_OK);
    EXPECT_EQ(g_writeCount, 2U);
    EXPECT_EQ(g_fenceCount, 1U);
    EXPECT_TRUE(state.pending);
}
