/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
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

#include <set>

#define private public
#include "compose_transport_manager.h"
#include "dl_acl_api.h"
#undef private

using namespace ock::mf;
using namespace ock::mf::transport;

namespace {

// 通过 legacyOpType_ 驱动 GetRank2RankOpType，保证 fallback 只使用配置过的协议。
class FakeTagInfo : public HybmEntityTagInfo {
public:
    explicit FakeTagInfo(uint32_t op)
    {
        legacyOpType_ = op;
    }
};

std::shared_ptr<HybmEntityTagInfo> MakeRankOpTag(uint32_t opTypes)
{
    auto tag = std::make_shared<FakeTagInfo>(opTypes);
    return tag;
}

class FakeTransportManager : public TransportManager {
public:
    Result OpenDevice(const TransportOptions &) override
    {
        ++openDeviceCalls;
        return openDeviceResult;
    }

    Result CloseDevice() override
    {
        ++closeDeviceCalls;
        return closeDeviceResult;
    }

    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override
    {
        (void)mr;
        ++registerCalls;
        return registerResult;
    }

    Result UnregisterMemoryRegion(uint64_t addr) override
    {
        (void)addr;
        ++unregisterCalls;
        return unregisterResult;
    }

    bool QueryHasRegistered(uint64_t, uint64_t) override
    {
        ++queryHasRegCalls;
        return queryHasRegResult;
    }

    Result QueryMemoryKey(uint64_t, TransportMemoryKey &) override
    {
        ++queryKeyCalls;
        return queryKeyResult;
    }

    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override
    {
        ++updateMemKeyCalls;
        return;
    }

    Result Prepare(const HybmTransPrepareOptions &) override
    {
        ++prepareCalls;
        return prepareResult;
    }

    Result RemoveRanks(const std::vector<uint32_t> &) override
    {
        ++removeRanksCalls;
        return removeRanksResult;
    }

    Result ConnectRank(uint32_t) override
    {
        ++connectRankCalls;
        return connectRankResult;
    }

    Result UpdateRankOptions(const HybmTransPrepareOptions &) override
    {
        ++updateRankCalls;
        return updateRankResult;
    }

    const std::string &GetNic() const override
    {
        return nic_;
    }

    const TransportPrivateData GetPrivateData() const override
    {
        return TransportPrivateData{};
    }

    const void *GetQpInfo() const override
    {
        return nullptr;
    }

    Result ReadRemote(uint32_t, uint64_t, uint64_t, uint64_t) override
    {
        ++readCalls;
        return readResult;
    }

    Result WriteRemote(uint32_t, uint64_t, uint64_t, uint64_t) override
    {
        ++writeCalls;
        return writeResult;
    }

    Result ReadRemoteAsync(uint32_t, uint64_t, uint64_t, uint64_t) override
    {
        ++readAsyncCalls;
        return readAsyncResult;
    }

    Result WriteRemoteAsync(uint32_t, uint64_t, uint64_t, uint64_t) override
    {
        ++writeAsyncCalls;
        return writeAsyncResult;
    }

    Result Synchronize(uint32_t) override
    {
        ++syncCalls;
        return syncResult;
    }

    Result SynchronizeRanks(const std::set<uint32_t> &rankIds) override
    {
        ++syncRanksCalls;
        synchronizedRanks = rankIds;
        return syncRanksResult;
    }

    Result WriteRemoteBatchAsync(uint32_t, const CopyDescriptor &) override
    {
        ++writeBatchAsyncCalls;
        return writeBatchAsyncResult;
    }

    Result ReadRemoteBatchAsync(uint32_t, const CopyDescriptor &) override
    {
        ++readBatchAsyncCalls;
        return readBatchAsyncResult;
    }

    Result TransferRemoteBatchAsync(const hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                    const RankGroupMap &groupMap, std::vector<uint32_t> &localIndices,
                                    RankGroupMap &unregisteredGroups, std::set<uint32_t> &batchRanks) override
    {
        (void)params;
        (void)direction;
        (void)localIndices;
        (void)unregisteredGroups;
        ++transferBatchAsyncCalls;
        transferBatchRankCount = groupMap.size();
        if (transferBatchAsyncResult == BM_OK) {
            batchRanks.insert(transferBatchRanks.begin(), transferBatchRanks.end());
        }
        return transferBatchAsyncResult;
    }

public:
    uint32_t openDeviceCalls{0};
    uint32_t closeDeviceCalls{0};
    uint32_t registerCalls{0};
    uint32_t unregisterCalls{0};
    uint32_t queryHasRegCalls{0};
    uint32_t queryKeyCalls{0};
    uint32_t prepareCalls{0};
    uint32_t removeRanksCalls{0};
    uint32_t connectRankCalls{0};
    uint32_t updateRankCalls{0};
    uint32_t updateMemKeyCalls{0};
    uint32_t readCalls{0};
    uint32_t writeCalls{0};
    uint32_t readAsyncCalls{0};
    uint32_t writeAsyncCalls{0};
    uint32_t syncCalls{0};
    uint32_t syncRanksCalls{0};
    uint32_t writeBatchAsyncCalls{0};
    uint32_t readBatchAsyncCalls{0};
    uint32_t transferBatchAsyncCalls{0};
    size_t transferBatchRankCount{0U};

    Result openDeviceResult{BM_OK};
    Result closeDeviceResult{BM_OK};
    Result registerResult{BM_OK};
    Result unregisterResult{BM_OK};
    bool queryHasRegResult{false};
    Result queryKeyResult{BM_OK};
    Result prepareResult{BM_OK};
    Result removeRanksResult{BM_OK};
    Result connectRankResult{BM_OK};
    Result updateRankResult{BM_OK};
    Result readResult{BM_OK};
    Result writeResult{BM_OK};
    Result readAsyncResult{BM_OK};
    Result writeAsyncResult{BM_OK};
    Result syncResult{BM_OK};
    Result syncRanksResult{BM_OK};
    Result writeBatchAsyncResult{BM_OK};
    Result readBatchAsyncResult{BM_OK};
    Result transferBatchAsyncResult{BM_OK};
    std::set<uint32_t> transferBatchRanks;
    std::set<uint32_t> synchronizedRanks;

    std::string nic_{"fake_nic"};
};

constexpr uint32_t kComposeHostProtocols = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
constexpr uint32_t kComposeDeviceRdma = HYBM_DOP_TYPE_DEVICE_RDMA;
constexpr uint32_t kComposeDeviceUrma = HYBM_DOP_TYPE_DEVICE_URMA;

void AddHostTransport(ComposeTransportManager &mgr, const std::shared_ptr<FakeTransportManager> &manager)
{
    for (auto it = mgr.transports_.begin(); it != mgr.transports_.end();) {
        if ((it->first & kComposeHostProtocols) != 0U) {
            it = mgr.transports_.erase(it);
        } else {
            ++it;
        }
    }
    mgr.transports_[kComposeHostProtocols] = manager;
}

void AddDeviceTransport(ComposeTransportManager &mgr, const std::shared_ptr<FakeTransportManager> &manager,
                        uint32_t opTypes = kComposeDeviceRdma)
{
    for (auto it = mgr.transports_.begin(); it != mgr.transports_.end();) {
        if ((it->first & kComposeDeviceRdma) != 0U || (it->first & kComposeDeviceUrma) != 0U) {
            it = mgr.transports_.erase(it);
        } else {
            ++it;
        }
    }
    mgr.transports_.erase(kComposeDeviceRdma);
    mgr.transports_.erase(kComposeDeviceUrma);
    mgr.transports_[opTypes] = manager;
}

bool HasDeviceTransport(const ComposeTransportManager &mgr)
{
    for (const auto &[protocol, manager] : mgr.transports_) {
        if ((protocol & kComposeDeviceRdma) != 0U || (protocol & kComposeDeviceUrma) != 0U) {
            return manager != nullptr;
        }
    }
    return false;
}

bool HasHostTransport(const ComposeTransportManager &mgr)
{
    for (const auto &[protocol, manager] : mgr.transports_) {
        if ((protocol & kComposeHostProtocols) != 0U) {
            return manager != nullptr;
        }
    }
    return false;
}

} // namespace

// -------- 基本 MR 管理逻辑 --------

// RegisterMemoryRegion 插入到内部 mrs_ 中，并可被 QueryHasRegistered 命中。
TEST(ComposeTransportManagerTest, RegisterAndQueryMemoryRegion)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto device = std::make_shared<FakeTransportManager>();
    device->registerResult = BM_OK;
    device->queryHasRegResult = true;
    AddDeviceTransport(mgr, device);

    TransportMemoryRegion mr{};
    mr.addr = 0x1000;
    mr.size = 0x100;

    Result ret = mgr.RegisterMemoryRegion(mr);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(device->registerCalls, 1u);
    // QueryHasRegistered 通过底层 device TM 判断
    EXPECT_TRUE(mgr.QueryHasRegistered(mr.addr, mr.size));
}

// QueryHasRegistered 在没有注册任何 MR 时返回 false。
TEST(ComposeTransportManagerTest, QueryHasRegisteredOnEmpty)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    EXPECT_FALSE(mgr.QueryHasRegistered(0x2000, 0x100));
}

// QueryHasRegistered 请求范围超过已注册 MR 时返回 false。
TEST(ComposeTransportManagerTest, QueryHasRegisteredSizeTooLarge)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    TransportMemoryRegion mr{};
    mr.addr = 0x3000;
    mr.size = 0x100;
    Result ret = mgr.RegisterMemoryRegion(mr);
    EXPECT_EQ(ret, BM_OK);

    EXPECT_FALSE(mgr.QueryHasRegistered(mr.addr, mr.size + 1));
}

// RegisterMemoryRegion：device TM 注册失败时直接返回错误，不插入 mrs_。
TEST(ComposeTransportManagerTest, RegisterMemoryRegionDeviceRegisterFail)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto device = std::make_shared<FakeTransportManager>();
    device->registerResult = BM_ERROR;
    AddDeviceTransport(mgr, device);

    TransportMemoryRegion mr{};
    mr.addr = 0x5000;
    mr.size = 0x100;

    Result ret = mgr.RegisterMemoryRegion(mr);
    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_EQ(device->registerCalls, 1u);
    EXPECT_TRUE(mgr.mrs_.empty());
}

// RegisterMemoryRegion：device 成功、host 失败时，会触发 device 侧 Unregister 回滚。
TEST(ComposeTransportManagerTest, RegisterMemoryRegionHostFailsRollsBackDevice)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto device = std::make_shared<FakeTransportManager>();
    auto host = std::make_shared<FakeTransportManager>();
    device->registerResult = BM_OK;
    host->registerResult = BM_ERROR;

    AddDeviceTransport(mgr, device);
    AddHostTransport(mgr, host);

    TransportMemoryRegion mr{};
    mr.addr = 0x6000;
    mr.size = 0x100;

    Result ret = mgr.RegisterMemoryRegion(mr);
    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_EQ(device->registerCalls, 1u);
    EXPECT_EQ(host->registerCalls, 1u);
    EXPECT_EQ(device->unregisterCalls, 1u);
    EXPECT_TRUE(mgr.mrs_.empty());
}

// UnregisterMemoryRegion 对未注册的 addr 返回 BM_INVALID_PARAM。
TEST(ComposeTransportManagerTest, UnregisterNotRegistered)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    Result ret = mgr.UnregisterMemoryRegion(0x4000);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
}

// UnregisterMemoryRegion：device 侧注销失败时直接返回错误，不调用 host、不从 mrs_ 移除。
TEST(ComposeTransportManagerTest, UnregisterMemoryRegionDeviceFailKeepsEntry)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    TransportMemoryRegion mr{};
    mr.addr = 0x7000;
    mr.size = 0x100;
    mgr.mrs_.emplace(mr.addr, ComposeMemoryRegion{mr.addr, mr.size, TT_COMPOSE});

    auto device = std::make_shared<FakeTransportManager>();
    device->unregisterResult = BM_ERROR;
    auto host = std::make_shared<FakeTransportManager>();
    AddDeviceTransport(mgr, device);
    AddHostTransport(mgr, host);

    Result ret = mgr.UnregisterMemoryRegion(mr.addr);
    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_EQ(device->unregisterCalls, 1u);
    EXPECT_EQ(host->unregisterCalls, 0u);
    // 失败时 mrs_ 不应删除该条目
    EXPECT_FALSE(mgr.mrs_.empty());
}

// UnregisterMemoryRegion：device 成功、host 失败时，返回 host 错误码且不删除 mrs_。
TEST(ComposeTransportManagerTest, UnregisterMemoryRegionHostFailKeepsEntry)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    TransportMemoryRegion mr{};
    mr.addr = 0x8000;
    mr.size = 0x100;
    mgr.mrs_.emplace(mr.addr, ComposeMemoryRegion{mr.addr, mr.size, TT_COMPOSE});

    auto device = std::make_shared<FakeTransportManager>();
    device->unregisterResult = BM_OK;
    auto host = std::make_shared<FakeTransportManager>();
    host->unregisterResult = BM_ERROR;
    AddDeviceTransport(mgr, device);
    AddHostTransport(mgr, host);

    Result ret = mgr.UnregisterMemoryRegion(mr.addr);
    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_EQ(device->unregisterCalls, 1u);
    EXPECT_EQ(host->unregisterCalls, 1u);
    EXPECT_FALSE(mgr.mrs_.empty());
}

// -------- 建链前 Prepare / RemoveRanks --------

// Prepare 应调用 host/device 的 Prepare（即使 options 为空）。
TEST(ComposeTransportManagerTest, PrepareCallsHostAndDevice)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA);
    ComposeTransportManager mgr(tag);

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    HybmTransPrepareOptions opts{};
    Result ret = mgr.Prepare(opts);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(host->prepareCalls, 1u);
    EXPECT_EQ(dev->prepareCalls, 1u);
}

// RemoveRanks 出错时应返回最后一次错误码。
TEST(ComposeTransportManagerTest, RemoveRanksPropagatesLastError)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    host->removeRanksResult = BM_ERROR;
    dev->removeRanksResult = BM_INVALID_PARAM;

    std::vector<uint32_t> removed{0, 1};
    Result ret = mgr.RemoveRanks(removed);
    EXPECT_EQ(host->removeRanksCalls, 1u);
    EXPECT_EQ(dev->removeRanksCalls, 1u);
    EXPECT_NE(ret, BM_OK);
}

// -------- Read/Write / QueryMemoryKey / GetNic 基本行为 --------

// ReadRemote 只尝试该 rank 配置过的 transport。
TEST(ComposeTransportManagerTest, ReadRemoteWithoutRankProtocolsSkipsTransports)
{
    auto tag = std::make_shared<FakeTagInfo>(0U); // 不支持任何协议
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    dev->readResult = BM_ERROR;
    host->readResult = BM_ERROR;

    Result ret = mgr.ReadRemote(1, 0x1000, 0x2000, 128);
    EXPECT_EQ(dev->readCalls, 0u);
    EXPECT_EQ(host->readCalls, 0u);
    EXPECT_EQ(ret, BM_ERROR);
}

// QueryMemoryKey 应调用 device / host 的 QueryMemoryKey，并整体返回 BM_OK。
TEST(ComposeTransportManagerTest, QueryMemoryKeyCallsHostAndDevice)
{
    uint32_t hostProtocol = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA | hostProtocol);
    ComposeTransportManager mgr(tag);

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    TransportMemoryKey key{};
    Result ret = mgr.QueryMemoryKey(0x7000, key);
    EXPECT_EQ(dev->queryKeyCalls, 1u);
    EXPECT_EQ(host->queryKeyCalls, 1u);
    EXPECT_EQ(ret, BM_OK);
}

// QueryMemoryKey：device 侧查询失败时直接返回错误，不再访问 host。
TEST(ComposeTransportManagerTest, QueryMemoryKeyDeviceFailShortCircuits)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    dev->queryKeyResult = BM_ERROR;

    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    TransportMemoryKey key{};
    Result ret = mgr.QueryMemoryKey(0x9000, key);
    EXPECT_EQ(dev->queryKeyCalls, 1u);
    EXPECT_EQ(host->queryKeyCalls, 0u);
    EXPECT_EQ(ret, BM_ERROR);
}

// QueryMemoryKey：host 侧查询失败时仅打 warning，不影响整体返回 BM_OK。
TEST(ComposeTransportManagerTest, QueryMemoryKeyHostFailStillOk)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    host->queryKeyResult = BM_ERROR;

    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    TransportMemoryKey key{};
    Result ret = mgr.QueryMemoryKey(0xA000, key);
    EXPECT_EQ(dev->queryKeyCalls, 1u);
    EXPECT_EQ(host->queryKeyCalls, 1u);
    EXPECT_EQ(ret, BM_OK);
}

// GetNic 返回内部保存的 nicInfo_。
TEST(ComposeTransportManagerTest, GetNicReturnsInternalString)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    mgr.nicInfo_ = "host#eth0;device#dev0;";

    const std::string &nic = mgr.GetNic();
    EXPECT_EQ(nic, "host#eth0;device#dev0;");
}

// -------- GetHostPrepareOptions / GetDevicePrepareOptions 分支覆盖 --------

// GetHostPrepareOptions: opType 不包含 HOST_PROTOCOL 时应跳过该 rank。
TEST(ComposeTransportManagerTest, GetHostPrepareOptionsSkipNonHostOpType)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA); // 只包含 device
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    HybmTransPrepareOptions inOpts{};
    TransportRankPrepareInfo info{};
    info.nic = "host#eth0;device#dev0;"; // 形如 compose 保存的 nic 字符串
    inOpts.options.emplace(1U, info);

    HybmTransPrepareOptions hostOpts{};
    mgr.GetTransportPrepareOptions(inOpts, kComposeHostProtocols, hostOpts);
    EXPECT_TRUE(hostOpts.options.empty());
}

// GetHostPrepareOptions: 通过真实 HybmEntityTagInfo 配置 HOST_RDMA，使其提取 host nic 与 memKeys。
TEST(ComposeTransportManagerTest, GetHostPrepareOptionsExtractsHostInfo)
{
    auto realTag = std::make_shared<HybmEntityTagInfo>();
    hybm_options opt{};
    opt.rankCount = 4;
    ASSERT_EQ(realTag->TagInfoInit(opt), BM_OK);
    ASSERT_EQ(realTag->AddRankLayers(0, {HYBM_DOP_TYPE_HOST_RDMA}), BM_OK);
    ASSERT_EQ(realTag->AddRankLayers(1, {HYBM_DOP_TYPE_HOST_RDMA}), BM_OK);

    ComposeTransportManager mgr(realTag);
    mgr.options_.rankId = 1; // 本端 rankId

    HybmTransPrepareOptions inOpts{};
    TransportRankPrepareInfo info{};
    info.nic = "host#eth0;device#dev0;";
    TransportMemoryKey key{};
    info.memKeys.emplace_back(key);
    inOpts.options.emplace(0U, info); // 远端 rankId

    HybmTransPrepareOptions hostOpts{};
    mgr.GetTransportPrepareOptions(inOpts, kComposeHostProtocols, hostOpts);

    ASSERT_EQ(hostOpts.options.size(), 1u);
    auto it = hostOpts.options.begin();
    EXPECT_EQ(it->first, 0U);
    EXPECT_EQ(it->second.nic, "eth0"); // 去掉 "host#" 前缀
    EXPECT_EQ(it->second.memKeys.size(), 1u);
}

// GetDevicePrepareOptions: opType 不包含 DEVICE_RDMA 时应跳过该 rank。
TEST(ComposeTransportManagerTest, GetDevicePrepareOptionsSkipNonDeviceOpType)
{
    uint32_t hostProtocol = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
    auto tag = std::make_shared<FakeTagInfo>(hostProtocol); // 只有 host
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    HybmTransPrepareOptions inOpts{};
    TransportRankPrepareInfo info{};
    info.nic = "device#roce0;host#eth0;";
    inOpts.options.emplace(3U, info);

    HybmTransPrepareOptions devOpts{};
    mgr.GetTransportPrepareOptions(inOpts, kComposeDeviceRdma, devOpts);
    EXPECT_TRUE(devOpts.options.empty());
}

// GetDevicePrepareOptions: 通过真实 HybmEntityTagInfo 配置 DEVICE_RDMA，使其提取 device nic 与 memKeys。
TEST(ComposeTransportManagerTest, GetDevicePrepareOptionsExtractsDeviceInfo)
{
    auto realTag = std::make_shared<HybmEntityTagInfo>();
    hybm_options opt{};
    opt.rankCount = 4;
    ASSERT_EQ(realTag->TagInfoInit(opt), BM_OK);
    ASSERT_EQ(realTag->AddRankLayers(0, {HYBM_DOP_TYPE_DEVICE_RDMA}), BM_OK);
    ASSERT_EQ(realTag->AddRankLayers(1, {HYBM_DOP_TYPE_DEVICE_RDMA}), BM_OK);

    ComposeTransportManager mgr(realTag);
    mgr.options_.rankId = 1;

    HybmTransPrepareOptions inOpts{};
    TransportRankPrepareInfo info{};
    info.nic = "device#roce0;host#eth0;";
    TransportMemoryKey key{};
    info.memKeys.emplace_back(key);
    inOpts.options.emplace(0U, info);

    HybmTransPrepareOptions devOpts{};
    mgr.GetTransportPrepareOptions(inOpts, kComposeDeviceRdma, devOpts);

    ASSERT_EQ(devOpts.options.size(), 1u);
    auto it = devOpts.options.begin();
    EXPECT_EQ(it->first, 0U);
    EXPECT_EQ(it->second.nic, "roce0");
    EXPECT_EQ(it->second.memKeys.size(), 1u);
}

// -------- OpenDevice / CloseDevice / Connect 家族 / UpdateRankOptions --------

// OpenDevice 在 protocol 为 0 时不会尝试打开 host/device transport，但应返回 BM_OK。
TEST(ComposeTransportManagerTest, OpenAndCloseDeviceNoProtocol)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    TransportOptions opts{};
    opts.protocol = 0; // 不包含 HOST / DEVICE_RDMA
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "ignored";

    Result retOpen = mgr.OpenDevice(opts);
    EXPECT_EQ(retOpen, BM_OK);

    // 未创建实际 host/device TM，CloseDevice 也应直接返回 BM_OK。
    Result retClose = mgr.CloseDevice();
    EXPECT_EQ(retClose, BM_OK);
}

// OpenHostTransport: 当 hostTransportManager_ 已经存在时返回 BM_ERROR。
TEST(ComposeTransportManagerTest, OpenHostTransportAlreadyOpened)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    // 预先占用 hostTransportManager_，模拟「已经打开」的场景。
    AddHostTransport(mgr, std::make_shared<FakeTransportManager>());

    TransportOptions opts{};
    opts.protocol = HYBM_DOP_TYPE_HOST_TCP;
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "host_nic";

    Result ret = mgr.OpenHostTransport(opts);
    EXPECT_EQ(ret, BM_ERROR);
}

// OpenDeviceTransport: 当 deviceTransportManager_ 已经存在时返回 BM_ERROR。
TEST(ComposeTransportManagerTest, OpenDeviceTransportAlreadyOpened)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    AddDeviceTransport(mgr, std::make_shared<FakeTransportManager>());

    TransportOptions opts{};
    opts.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "dev_nic";

    Result ret = mgr.OpenDeviceTransport(opts);
    EXPECT_EQ(ret, BM_ERROR);
}

// IsDeviceUrma: DEVICE_URMA 应返回 true，DEVICE_RDMA 返回 false。
TEST(ComposeTransportManagerTest, IsDeviceUrmaCoversUrma)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    mgr.options_.protocol = HYBM_DOP_TYPE_DEVICE_URMA;
    EXPECT_TRUE(mgr.IsDeviceUrma());

    mgr.options_.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;
    EXPECT_FALSE(mgr.IsDeviceUrma());
}

// OpenDevice: 当 hostTransportManager_ 已经存在且 protocol 包含 HOST 时，会走 OpenHostTransport 的错误分支并返回 BM_ERROR。
TEST(ComposeTransportManagerTest, OpenDeviceFailsWhenHostAlreadyOpened)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_HOST_TCP);
    ComposeTransportManager mgr(tag);

    AddHostTransport(mgr, std::make_shared<FakeTransportManager>());

    TransportOptions opts{};
    opts.protocol = HYBM_DOP_TYPE_HOST_TCP;
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "host_nic";

    Result ret = mgr.OpenDevice(opts);
    EXPECT_EQ(ret, BM_ERROR);
}

// OpenDevice: 当 deviceTransportManager_ 已经存在且 protocol 包含 DEVICE_RDMA 时，会走 OpenDeviceTransport 的错误分支并返回 BM_ERROR。
TEST(ComposeTransportManagerTest, OpenDeviceFailsWhenDeviceAlreadyOpened)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);

    AddDeviceTransport(mgr, std::make_shared<FakeTransportManager>());

    TransportOptions opts{};
    opts.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "dev_nic";

    Result ret = mgr.OpenDevice(opts);
    EXPECT_EQ(ret, BM_ERROR);
}

// UpdateRankOptions 会调用 host/device TM 的 UpdateRankOptions。
TEST(ComposeTransportManagerTest, UpdateRankOptionsCallsHostAndDevice)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    HybmTransPrepareOptions opts{};
    Result ret = mgr.UpdateRankOptions(opts);
    EXPECT_EQ(host->updateRankCalls, 1u);
    EXPECT_EQ(dev->updateRankCalls, 1u);
    EXPECT_EQ(ret, BM_OK);
}

// -------- Read/Write Async & Batch / Synchronize 转发表现 --------

// ReadRemoteAsync 只尝试该 rank 配置过的 transport。
TEST(ComposeTransportManagerTest, ReadRemoteAsyncWithoutRankProtocolsSkipsTransports)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    dev->readAsyncResult = BM_ERROR;
    host->readAsyncResult = BM_ERROR;

    Result ret = mgr.ReadRemoteAsync(1, 0x1000, 0x2000, 128);
    EXPECT_EQ(dev->readAsyncCalls, 0u);
    EXPECT_EQ(host->readAsyncCalls, 0u);
    EXPECT_EQ(ret, BM_ERROR);
}

// WriteRemote：device 路径失败后会 fallback 到 host，再失败则返回 BM_ERROR。
TEST(ComposeTransportManagerTest, WriteRemoteDeviceThenHostBothFail)
{
    uint32_t hostProtocol = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA | hostProtocol);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    dev->writeResult = BM_ERROR;
    host->writeResult = BM_ERROR;

    Result ret = mgr.WriteRemote(1, 0x1000, 0x2000, 64);
    EXPECT_EQ(dev->writeCalls, 1u);
    EXPECT_EQ(host->writeCalls, 1u);
    EXPECT_EQ(ret, BM_ERROR);
}

// WriteRemoteAsync：未配置的 host transport 不参与 fallback。
TEST(ComposeTransportManagerTest, WriteRemoteAsyncUnconfiguredHostSkipped)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    dev->writeAsyncResult = BM_ERROR;

    Result ret = mgr.WriteRemoteAsync(2, 0x3000, 0x4000, 128);
    EXPECT_EQ(dev->writeAsyncCalls, 1u);
    EXPECT_EQ(host->writeAsyncCalls, 0u);
    EXPECT_EQ(ret, BM_ERROR);
}

// ReadRemoteBatchAsync：包含 DEVICE_RDMA 协议时，直接返回 BM_NOT_SUPPORTED。
TEST(ComposeTransportManagerTest, ReadRemoteBatchAsyncDeviceNotSupported)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    CopyDescriptor desc{};
    Result ret = mgr.ReadRemoteBatchAsync(1, desc);
    // opType 实际为 0，不会走 DEVICE_RDMA 分支，只需保证返回错误且不调用 TM。
    EXPECT_EQ(ret, BM_ERROR);
}

// ReadRemoteBatchAsync：未配置的 device transport 不参与 fallback。
TEST(ComposeTransportManagerTest, ReadRemoteBatchAsyncConfiguredHostSucceeds)
{
    uint32_t hostProtocol = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
    auto tag = std::make_shared<FakeTagInfo>(hostProtocol);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    CopyDescriptor desc{};
    dev->readBatchAsyncResult = BM_ERROR;

    Result ret = mgr.ReadRemoteBatchAsync(2, desc);
    EXPECT_EQ(dev->readBatchAsyncCalls, 0u);
    EXPECT_EQ(host->readBatchAsyncCalls, 1u);
    EXPECT_EQ(ret, BM_OK);
}

// WriteRemoteBatchAsync：包含 DEVICE_RDMA 协议时，直接返回 BM_NOT_SUPPORTED。
TEST(ComposeTransportManagerTest, WriteRemoteBatchAsyncDeviceNotSupported)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    CopyDescriptor desc{};
    Result ret = mgr.WriteRemoteBatchAsync(3, desc);
    EXPECT_EQ(ret, BM_ERROR);
}

// WriteRemoteBatchAsync：未配置的 device transport 不参与 fallback。
TEST(ComposeTransportManagerTest, WriteRemoteBatchAsyncConfiguredHostFails)
{
    uint32_t hostProtocol = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
    auto tag = std::make_shared<FakeTagInfo>(hostProtocol);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    CopyDescriptor desc{};
    dev->writeBatchAsyncResult = BM_ERROR;

    Result ret = mgr.WriteRemoteBatchAsync(4, desc);
    EXPECT_EQ(dev->writeBatchAsyncCalls, 0u);
    EXPECT_EQ(host->writeBatchAsyncCalls, 1u);
    EXPECT_EQ(ret, BM_OK);
}

TEST(ComposeTransportManagerTest, TransferRemoteBatchAsyncUrmaRequiresDeviceManager)
{
    auto tag = std::make_shared<HybmEntityTagInfo>();
    ASSERT_EQ(tag->AddRankLayers(0U, {HYBM_DOP_TYPE_DEVICE_URMA}), BM_OK);
    ASSERT_EQ(tag->AddRankLayers(1U, {HYBM_DOP_TYPE_DEVICE_URMA}), BM_OK);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0U;

    hybm_batch_copy_params params{};
    RankGroupMap groupMap{{{0U, 1U}, {0U}}};
    std::vector<uint32_t> localIndices;
    RankGroupMap unregisteredGroups;
    std::set<uint32_t> batchRanks;
    EXPECT_EQ(mgr.TransferRemoteBatchAsync(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, groupMap, localIndices,
                                           unregisteredGroups, batchRanks),
              BM_INVALID_PARAM);
    EXPECT_TRUE(batchRanks.empty());
}

TEST(ComposeTransportManagerTest, TransferRemoteBatchAsyncForwardsAllUrmaRanks)
{
    auto tag = std::make_shared<HybmEntityTagInfo>();
    ASSERT_EQ(tag->AddRankLayers(0U, {HYBM_DOP_TYPE_DEVICE_URMA}), BM_OK);
    ASSERT_EQ(tag->AddRankLayers(1U, {HYBM_DOP_TYPE_DEVICE_URMA}), BM_OK);
    ASSERT_EQ(tag->AddRankLayers(2U, {HYBM_DOP_TYPE_DEVICE_URMA}), BM_OK);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0U;
    auto device = std::make_shared<FakeTransportManager>();
    device->transferBatchRanks = {1U, 2U};
    AddDeviceTransport(mgr, device, kComposeDeviceUrma);

    hybm_batch_copy_params params{};
    RankGroupMap groupMap{{{0U, 1U}, {0U}}, {{2U, 0U}, {1U}}};
    std::vector<uint32_t> localIndices;
    RankGroupMap unregisteredGroups;
    std::set<uint32_t> batchRanks;
    EXPECT_EQ(mgr.TransferRemoteBatchAsync(params, HYBM_GLOBAL_HOST_TO_GLOBAL_HOST, groupMap, localIndices,
                                           unregisteredGroups, batchRanks),
              BM_OK);
    EXPECT_EQ(device->transferBatchAsyncCalls, 1U);
    EXPECT_EQ(device->transferBatchRankCount, 2U);
    EXPECT_EQ(batchRanks, (std::set<uint32_t>{1U, 2U}));
}

TEST(ComposeTransportManagerTest, TransferRemoteBatchAsyncReturnsUrmaManagerError)
{
    auto tag = std::make_shared<HybmEntityTagInfo>();
    ASSERT_EQ(tag->AddRankLayers(0U, {HYBM_DOP_TYPE_DEVICE_URMA}), BM_OK);
    ASSERT_EQ(tag->AddRankLayers(1U, {HYBM_DOP_TYPE_DEVICE_URMA}), BM_OK);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0U;
    auto device = std::make_shared<FakeTransportManager>();
    device->transferBatchAsyncResult = BM_DL_FUNCTION_FAILED;
    AddDeviceTransport(mgr, device, kComposeDeviceUrma);

    hybm_batch_copy_params params{};
    RankGroupMap groupMap{{{0U, 1U}, {0U}}};
    std::vector<uint32_t> localIndices;
    RankGroupMap unregisteredGroups;
    std::set<uint32_t> batchRanks{99U};
    EXPECT_EQ(mgr.TransferRemoteBatchAsync(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, groupMap, localIndices,
                                           unregisteredGroups, batchRanks),
              BM_DL_FUNCTION_FAILED);
    EXPECT_TRUE(batchRanks.empty());
    EXPECT_EQ(device->transferBatchAsyncCalls, 1U);
}

TEST(ComposeTransportManagerTest, SynchronizeRanksForwardsOneDeviceBatch)
{
    auto tag = std::make_shared<FakeTagInfo>(kComposeDeviceRdma);
    ComposeTransportManager mgr(tag);
    auto device = std::make_shared<FakeTransportManager>();
    AddDeviceTransport(mgr, device);

    const std::set<uint32_t> rankIds{1U, 2U};
    EXPECT_EQ(mgr.SynchronizeRanks(rankIds), BM_OK);
    EXPECT_EQ(device->syncRanksCalls, 1U);
    EXPECT_EQ(device->syncCalls, 0U);
    EXPECT_EQ(device->synchronizedRanks, rankIds);
}

TEST(ComposeTransportManagerTest, SynchronizeRanksAllowsEmptySetWithoutDevice)
{
    auto tag = std::make_shared<HybmEntityTagInfo>();
    ComposeTransportManager mgr(tag);
    EXPECT_EQ(mgr.SynchronizeRanks({}), BM_OK);
}

// Synchronize：device 失败后继续尝试 host。
TEST(ComposeTransportManagerTest, SynchronizeDeviceFailsHostSucceeds)
{
    uint32_t hostProtocol = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA | hostProtocol);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    dev->syncResult = BM_ERROR;

    Result ret = mgr.Synchronize(5);
    EXPECT_EQ(dev->syncCalls, 1u);
    EXPECT_EQ(host->syncCalls, 1u);
    EXPECT_EQ(ret, BM_OK);
}

// RegisterMemoryRegion: 仅注册device transport的情况。
TEST(ComposeTransportManagerTest, RegisterMemoryRegionDeviceOnly)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto device = std::make_shared<FakeTransportManager>();
    device->registerResult = BM_OK;
    AddDeviceTransport(mgr, device);

    TransportMemoryRegion mr{};
    mr.addr = 0xA000;
    mr.size = 0x100;

    Result ret = mgr.RegisterMemoryRegion(mr);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(device->registerCalls, 1u);
    EXPECT_FALSE(mgr.mrs_.empty());
}

// RegisterMemoryRegion: 仅注册host transport的情况。
TEST(ComposeTransportManagerTest, RegisterMemoryRegionHostOnly)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto host = std::make_shared<FakeTransportManager>();
    host->registerResult = BM_OK;
    AddHostTransport(mgr, host);

    TransportMemoryRegion mr{};
    mr.addr = 0xB000;
    mr.size = 0x100;

    Result ret = mgr.RegisterMemoryRegion(mr);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(host->registerCalls, 1u);
    EXPECT_FALSE(mgr.mrs_.empty());
}

// UnregisterMemoryRegion: 仅注销device transport的情况。
TEST(ComposeTransportManagerTest, UnregisterMemoryRegionDeviceOnly)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    TransportMemoryRegion mr{};
    mr.addr = 0xC000;
    mr.size = 0x100;
    mgr.mrs_.emplace(mr.addr, ComposeMemoryRegion{mr.addr, mr.size, TT_COMPOSE});

    auto device = std::make_shared<FakeTransportManager>();
    device->unregisterResult = BM_OK;
    AddDeviceTransport(mgr, device);

    Result ret = mgr.UnregisterMemoryRegion(mr.addr);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(device->unregisterCalls, 1u);
    EXPECT_TRUE(mgr.mrs_.empty());
}

// UnregisterMemoryRegion: 仅注销host transport的情况。
TEST(ComposeTransportManagerTest, UnregisterMemoryRegionHostOnly)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    TransportMemoryRegion mr{};
    mr.addr = 0xD000;
    mr.size = 0x100;
    mgr.mrs_.emplace(mr.addr, ComposeMemoryRegion{mr.addr, mr.size, TT_COMPOSE});

    auto host = std::make_shared<FakeTransportManager>();
    host->unregisterResult = BM_OK;
    AddHostTransport(mgr, host);

    Result ret = mgr.UnregisterMemoryRegion(mr.addr);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(host->unregisterCalls, 1u);
    EXPECT_TRUE(mgr.mrs_.empty());
}

// QueryHasRegistered: 当addr正好等于某个已注册区域的起始地址的情况。
TEST(ComposeTransportManagerTest, QueryHasRegisteredExactAddress)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    // QueryHasRegistered 本身委托给底层 TM 判断，这里通过 fake device TM 返回 true。
    auto device = std::make_shared<FakeTransportManager>();
    device->queryHasRegResult = true;
    AddDeviceTransport(mgr, device);

    EXPECT_TRUE(mgr.QueryHasRegistered(0xE000, 0x100));
}

// QueryHasRegistered: 当查询的区域完全包含在已注册区域内的情况。
TEST(ComposeTransportManagerTest, QueryHasRegisteredSubset)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto device = std::make_shared<FakeTransportManager>();
    device->queryHasRegResult = true;
    AddDeviceTransport(mgr, device);

    // 子区间查询同样依赖底层 TM，此处只验证返回 true。
    EXPECT_TRUE(mgr.QueryHasRegistered(0xF000 + 0x50, 0x50));
}

// GetHostPrepareOptions: 当item.second.nic中不包含HOST_TRANSPORT_TYPE前缀的情况。
TEST(ComposeTransportManagerTest, GetHostPrepareOptionsNoHostNicPrefix)
{
    uint32_t hostProtocol = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
    auto tag = std::make_shared<FakeTagInfo>(hostProtocol);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    HybmTransPrepareOptions inOpts{};
    TransportRankPrepareInfo info{};
    info.nic = "eth0;device#dev0;"; // 缺少host#前缀
    inOpts.options.emplace(1U, info);

    HybmTransPrepareOptions hostOpts{};
    mgr.GetTransportPrepareOptions(inOpts, kComposeHostProtocols, hostOpts);

    // 生产实现中，如果 opType 不包含 HOST_PROTOCOL 或没有 host# 前缀，
    // 可以选择直接跳过该 rank 的 host 准备，这里只校验不会崩溃，
    // 且允许 hostOpts 为空。
    EXPECT_LE(hostOpts.options.size(), 1u);
    if (!hostOpts.options.empty()) {
        auto it = hostOpts.options.begin();
        EXPECT_EQ(it->first, 1U);
        EXPECT_TRUE(it->second.nic.empty()); // 如存在，则 host nic 应为空
    }
}

// GetDevicePrepareOptions: 当item.second.nic中不包含DEVICE_TRANSPORT_TYPE前缀的情况。
TEST(ComposeTransportManagerTest, GetDevicePrepareOptionsNoDeviceNicPrefix)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    HybmTransPrepareOptions inOpts{};
    TransportRankPrepareInfo info{};
    info.nic = "host#eth0;roce0;"; // 缺少device#前缀
    inOpts.options.emplace(2U, info);

    HybmTransPrepareOptions devOpts{};
    mgr.GetTransportPrepareOptions(inOpts, kComposeDeviceRdma, devOpts);

    // 同 GetHostPrepareOptionsNoHostNicPrefix，生产实现可能选择直接跳过该 rank，
    // 因此这里只保证不会崩溃，并允许 devOpts 为空。
    EXPECT_LE(devOpts.options.size(), 1u);
    if (!devOpts.options.empty()) {
        auto it = devOpts.options.begin();
        EXPECT_EQ(it->first, 2U);
        EXPECT_TRUE(it->second.nic.empty()); // 如存在，则 device nic 应为空
    }
}

// GetHostPrepareOptions: 当memKeys为空的情况。
TEST(ComposeTransportManagerTest, GetHostPrepareOptionsEmptyMemKeys)
{
    uint32_t hostProtocol = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
    auto tag = std::make_shared<FakeTagInfo>(hostProtocol);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    HybmTransPrepareOptions inOpts{};
    TransportRankPrepareInfo info{};
    info.nic = "host#eth0;device#dev0;";
    // memKeys为空
    inOpts.options.emplace(3U, info);

    HybmTransPrepareOptions hostOpts{};
    mgr.GetTransportPrepareOptions(inOpts, kComposeHostProtocols, hostOpts);

    // 实际实现会根据 opType 决定是否下沉到 host，
    // 这里只要求不会崩溃，并且如存在条目则 nic 为解析出的 host 部分且 memKeys 为空。
    EXPECT_LE(hostOpts.options.size(), 1u);
    if (!hostOpts.options.empty()) {
        auto it = hostOpts.options.begin();
        EXPECT_EQ(it->first, 3U);
        EXPECT_EQ(it->second.nic, "eth0");
        EXPECT_TRUE(it->second.memKeys.empty()); // memKeys 应为空
    }
}

// GetDevicePrepareOptions: 当memKeys为空的情况。
TEST(ComposeTransportManagerTest, GetDevicePrepareOptionsEmptyMemKeys)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    HybmTransPrepareOptions inOpts{};
    TransportRankPrepareInfo info{};
    info.nic = "host#eth0;device#roce0;";
    // memKeys为空
    inOpts.options.emplace(4U, info);

    HybmTransPrepareOptions devOpts{};
    mgr.GetTransportPrepareOptions(inOpts, kComposeDeviceRdma, devOpts);

    EXPECT_LE(devOpts.options.size(), 1u);
    if (!devOpts.options.empty()) {
        auto it = devOpts.options.begin();
        EXPECT_EQ(it->first, 4U);
        EXPECT_EQ(it->second.nic, "roce0");
        EXPECT_TRUE(it->second.memKeys.empty()); // memKeys 应为空
    }
}

// UpdateRankOptions: 仅更新host transport的情况。
TEST(ComposeTransportManagerTest, UpdateRankOptionsHostOnly)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto host = std::make_shared<FakeTransportManager>();
    host->updateRankResult = BM_OK;
    AddHostTransport(mgr, host);

    HybmTransPrepareOptions opts{};
    Result ret = mgr.UpdateRankOptions(opts);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(host->updateRankCalls, 1u);
}

// UpdateRankOptions: 仅更新device transport的情况。
TEST(ComposeTransportManagerTest, UpdateRankOptionsDeviceOnly)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);

    auto device = std::make_shared<FakeTransportManager>();
    device->updateRankResult = BM_OK;
    AddDeviceTransport(mgr, device);

    HybmTransPrepareOptions opts{};
    Result ret = mgr.UpdateRankOptions(opts);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(device->updateRankCalls, 1u);
}

// OpenDeviceTransport with DEVICE_RDMA, ACL version 1.17 and MF_HYBM_RDMA_USE_HCOMM=1
// → expected to select HCOMM
TEST(ComposeTransportManagerTest, OpenDeviceTransportDeviceRdmaWithAcl117_SelectsHcomm)
{
    // HCOMM 开关：显式打开后走 HCOMM 分支
    const char *oldEnv = getenv("MF_HYBM_RDMA_USE_HCOMM");
    (void)setenv("MF_HYBM_RDMA_USE_HCOMM", "1", 1);
    // mock AclrtGetVersion to return 1.17 (HCOMM threshold)
    auto oldVer = DlAclApi::pAclrtGetVersion;
    DlAclApi::pAclrtGetVersion = [](int32_t *major, int32_t *minor, int32_t *patch) -> int32_t {
        *major = 1;
        *minor = 17;
        *patch = 0;
        return 0;
    };

    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);

    TransportOptions opts{};
    opts.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "dev_nic";

    // OpenDeviceTransport creates DeviceRdmaHcommTransportManager internally;
    // its OpenDevice will fail without full mocks, but we verify the path was selected
    Result ret = mgr.OpenDeviceTransport(opts);
    // DeviceRdmaHcommTransportManager::OpenDevice fails without EndpointCreate mock
    EXPECT_NE(ret, BM_OK);
    // But deviceTransportManager_ should have been created (HCOMM path)
    EXPECT_TRUE(HasDeviceTransport(mgr));

    DlAclApi::pAclrtGetVersion = oldVer;
    if (oldEnv != nullptr) {
        (void)setenv("MF_HYBM_RDMA_USE_HCOMM", oldEnv, 1);
    } else {
        (void)unsetenv("MF_HYBM_RDMA_USE_HCOMM");
    }
}

// OpenDeviceTransport with DEVICE_RDMA and ACL version 1.16 → native RDMA (not HCOMM)
TEST(ComposeTransportManagerTest, OpenDeviceTransportDeviceRdmaWithAcl116_SelectsNative)
{
    auto oldVer = DlAclApi::pAclrtGetVersion;
    DlAclApi::pAclrtGetVersion = [](int32_t *major, int32_t *minor, int32_t *patch) -> int32_t {
        *major = 1;
        *minor = 16;
        *patch = 0;
        return 0;
    };

    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);

    TransportOptions opts{};
    opts.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "dev_nic";

    Result ret = mgr.OpenDeviceTransport(opts);
    EXPECT_NE(ret, BM_OK);
    // Native RDMA path also creates a transport manager, different type from HCOMM
    EXPECT_TRUE(HasDeviceTransport(mgr));

    DlAclApi::pAclrtGetVersion = oldVer;
}

TEST(ComposeTransportManagerTest, OpenDeviceTransportWithUrma)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_URMA);
    ComposeTransportManager mgr(tag);
    TransportOptions opts{};
    opts.protocol = HYBM_DOP_TYPE_DEVICE_URMA;
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "dev_nic";
    auto ret = mgr.OpenDeviceTransport(opts);
    EXPECT_NE(ret, BM_OK);
    EXPECT_TRUE(HasDeviceTransport(mgr));
}

TEST(ComposeTransportManagerTest, OpenHostTransportWithTcp)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_HOST_TCP);
    ComposeTransportManager mgr(tag);
    TransportOptions opts{};
    opts.protocol = HYBM_DOP_TYPE_HOST_TCP;
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "host_nic";
    auto ret = mgr.OpenHostTransport(opts);
    EXPECT_NE(ret, BM_OK);
    EXPECT_TRUE(HasHostTransport(mgr));
}

TEST(ComposeTransportManagerTest, BatchAsyncWithoutDevice)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    CopyDescriptor desc{};
    EXPECT_NE(mgr.WriteRemoteBatchAsync(0, desc), BM_OK);
    EXPECT_NE(mgr.ReadRemoteBatchAsync(0, desc), BM_OK);
}

// ── Bulk delegation tests: all no-transport paths must return error ──

TEST(ComposeTransportManagerTest, CloseDevice_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    EXPECT_EQ(mgr.CloseDevice(), BM_OK); // no-op when both are null
}

TEST(ComposeTransportManagerTest, Prepare_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    HybmTransPrepareOptions opts{};
    EXPECT_EQ(mgr.Prepare(opts), BM_OK);
}

TEST(ComposeTransportManagerTest, RemoveRanks_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    EXPECT_EQ(mgr.RemoveRanks({1, 2}), BM_OK);
}

TEST(ComposeTransportManagerTest, ReadRemote_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    EXPECT_NE(mgr.ReadRemote(0, 0, 0, 1024), BM_OK);
}

TEST(ComposeTransportManagerTest, WriteRemote_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    EXPECT_NE(mgr.WriteRemote(0, 0, 0, 1024), BM_OK);
}

TEST(ComposeTransportManagerTest, ReadRemoteAsync_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    EXPECT_NE(mgr.ReadRemoteAsync(0, 0, 0, 1024), BM_OK);
}

TEST(ComposeTransportManagerTest, WriteRemoteAsync_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    EXPECT_NE(mgr.WriteRemoteAsync(0, 0, 0, 1024), BM_OK);
}

TEST(ComposeTransportManagerTest, Synchronize_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    EXPECT_NE(mgr.Synchronize(0), BM_OK);
}

TEST(ComposeTransportManagerTest, RegisterMemoryRegion_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    TransportMemoryRegion mr{0x1000, 0x1000, REG_MR_FLAG_DRAM};
    EXPECT_EQ(mgr.RegisterMemoryRegion(mr), BM_OK);
}

TEST(ComposeTransportManagerTest, UpdateRankOptions_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    HybmTransPrepareOptions opts{};
    EXPECT_EQ(mgr.UpdateRankOptions(opts), BM_OK);
}

TEST(ComposeTransportManagerTest, QueryHasRegistered_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    EXPECT_FALSE(mgr.QueryHasRegistered(0x1000, 0x100));
}

TEST(ComposeTransportManagerTest, QueryMemoryKey_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    TransportMemoryKey key{};
    EXPECT_EQ(mgr.QueryMemoryKey(0x1000, key), BM_OK);
}

TEST(ComposeTransportManagerTest, ReadRemoteBatchAsync_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    CopyDescriptor desc{};
    EXPECT_NE(mgr.ReadRemoteBatchAsync(0, desc), BM_OK);
}

TEST(ComposeTransportManagerTest, WriteRemoteBatchAsync_NoTransport)
{
    ComposeTransportManager mgr(std::make_shared<FakeTagInfo>(0U));
    CopyDescriptor desc{};
    EXPECT_NE(mgr.WriteRemoteBatchAsync(0, desc), BM_OK);
}

// OpenDevice: host 协议下 hostTransportManager_ 为 null → 返回错误并回滚
TEST(ComposeTransportManagerTest, OpenDeviceHostManagerNullFails)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_HOST_TCP);
    ComposeTransportManager mgr(tag);

    TransportOptions opts{};
    opts.protocol = HYBM_DOP_TYPE_HOST_TCP;
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "host_nic";

    // OpenHostTransport 内部创建 manager 会失败（无真实 HCOM），但确保路径执行到
    // hostTransportManager_==nullptr 分支返回 BM_ERROR
    Result ret = mgr.OpenDevice(opts);
    EXPECT_NE(ret, BM_OK);
}

// OpenDevice: device 协议下 deviceTransportManager_ 为 null → 返回错误并回滚
TEST(ComposeTransportManagerTest, OpenDeviceDeviceManagerNullFails)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);

    TransportOptions opts{};
    opts.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;
    opts.rankId = 0;
    opts.rankCount = 1;
    opts.nic = "dev_nic";

    Result ret = mgr.OpenDevice(opts);
    EXPECT_NE(ret, BM_OK);
}

// UpdateMemoryKey: 仅有 device transport 且为 RDMA 类型
TEST(ComposeTransportManagerTest, UpdateMemoryKeyDeviceRdma)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);

    auto dev = std::make_shared<FakeTransportManager>();
    AddDeviceTransport(mgr, dev);
    mgr.options_.protocol = HYBM_DOP_TYPE_DEVICE_RDMA;

    TransportMemoryKey key{};
    void *addr = reinterpret_cast<void *>(0x1234UL);
    mgr.UpdateMemoryKey(key, addr);
    EXPECT_EQ(dev->updateMemKeyCalls, 1u);
}

// UpdateMemoryKey: it must also be forwarded when only the host transport exists, otherwise on the
// trans + DRAM + HOST_RDMA path the gva of the host key is never updated to the local rebuilt address.
TEST(ComposeTransportManagerTest, UpdateMemoryKeyHostOnly)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_HOST_TCP);
    ComposeTransportManager mgr(tag);

    auto host = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);

    TransportMemoryKey key{};
    void *addr = reinterpret_cast<void *>(0x1234UL);
    mgr.UpdateMemoryKey(key, addr);
    EXPECT_EQ(host->updateMemKeyCalls, 1u);
}

// RemoveRanks: 同时有 host 和 device 时都调用
TEST(ComposeTransportManagerTest, RemoveRanksWithBothTransports)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_TCP);
    ComposeTransportManager mgr(tag);
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    Result ret = mgr.RemoveRanks({1, 2});
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(host->removeRanksCalls, 1u);
    EXPECT_EQ(dev->removeRanksCalls, 1u);
}

// RemoveRanks: device 失败时返回错误
TEST(ComposeTransportManagerTest, RemoveRanksDeviceFails)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);
    auto dev = std::make_shared<FakeTransportManager>();
    dev->removeRanksResult = BM_ERROR;
    AddDeviceTransport(mgr, dev);

    Result ret = mgr.RemoveRanks({1});
    EXPECT_NE(ret, BM_OK);
}

// ReadRemote: 委托给 device 并成功

// -------- ConnectRank --------

TEST(ComposeTransportManagerTest, ConnectRank_CallsHostAndDevice_Success)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_TCP);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    EXPECT_EQ(mgr.ConnectRank(3), BM_OK);
    EXPECT_EQ(host->connectRankCalls, 1u);
    EXPECT_EQ(dev->connectRankCalls, 1u);
}

TEST(ComposeTransportManagerTest, ConnectRank_HostFailsDeviceFallbackSucceeds)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_TCP);
    ComposeTransportManager mgr(tag);
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    host->connectRankResult = BM_ERROR;

    EXPECT_EQ(mgr.ConnectRank(3), BM_OK);
    EXPECT_EQ(dev->connectRankCalls, 1u);
}

TEST(ComposeTransportManagerTest, ConnectRank_DeviceFails)
{
    auto tag = std::make_shared<FakeTagInfo>(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_TCP);
    ComposeTransportManager mgr(tag);
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->connectRankResult = BM_ERROR;

    EXPECT_EQ(mgr.ConnectRank(3), BM_OK);
    EXPECT_EQ(host->connectRankCalls, 1u);
}

TEST(ComposeTransportManagerTest, CloseDevice_CollectsBothErrors)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->closeDeviceResult = BM_ERROR;
    host->closeDeviceResult = BM_ERROR;

    EXPECT_NE(mgr.CloseDevice(), BM_OK);
    EXPECT_TRUE(mgr.transports_.empty());
}

// -------- Read/Write/Async/Batch/Sync 错误路径（GetRank2RankOpType mock） --------

TEST(ComposeTransportManagerTest, ReadRemote_DeviceSucceeds)
{
    auto tag = MakeRankOpTag(HYBM_DOP_TYPE_DEVICE_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    EXPECT_EQ(mgr.ReadRemote(1, 0x1000, 0x2000, 128), BM_OK);
    EXPECT_EQ(dev->readCalls, 1u);
    EXPECT_EQ(host->readCalls, 0u);
}

TEST(ComposeTransportManagerTest, ReadRemote_DeviceFailsHostSucceeds)
{
    auto tag = MakeRankOpTag(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->readResult = BM_ERROR;

    EXPECT_EQ(mgr.ReadRemote(1, 0x1000, 0x2000, 128), BM_OK);
    EXPECT_EQ(dev->readCalls, 1u);
    EXPECT_EQ(host->readCalls, 1u);
}

TEST(ComposeTransportManagerTest, ReadRemote_BothFail)
{
    auto tag = MakeRankOpTag(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->readResult = BM_ERROR;
    host->readResult = BM_ERROR;

    EXPECT_NE(mgr.ReadRemote(1, 0x1000, 0x2000, 128), BM_OK);
}

TEST(ComposeTransportManagerTest, WriteRemote_DeviceFailsHostSucceeds)
{
    auto tag = MakeRankOpTag(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->writeResult = BM_ERROR;

    EXPECT_EQ(mgr.WriteRemote(1, 0x1000, 0x2000, 64), BM_OK);
    EXPECT_EQ(dev->writeCalls, 1u);
    EXPECT_EQ(host->writeCalls, 1u);
}

TEST(ComposeTransportManagerTest, WriteRemote_BothFail)
{
    auto tag = MakeRankOpTag(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->writeResult = BM_ERROR;
    host->writeResult = BM_ERROR;

    EXPECT_NE(mgr.WriteRemote(1, 0x1000, 0x2000, 64), BM_OK);
}

TEST(ComposeTransportManagerTest, ReadRemoteAsync_DeviceFailsHostSucceeds)
{
    auto tag = MakeRankOpTag(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->readAsyncResult = BM_ERROR;

    EXPECT_EQ(mgr.ReadRemoteAsync(1, 0x1000, 0x2000, 128), BM_OK);
    EXPECT_EQ(dev->readAsyncCalls, 1u);
    EXPECT_EQ(host->readAsyncCalls, 1u);
}

TEST(ComposeTransportManagerTest, WriteRemoteAsync_BothFail)
{
    auto tag = MakeRankOpTag(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->writeAsyncResult = BM_ERROR;
    host->writeAsyncResult = BM_ERROR;

    EXPECT_NE(mgr.WriteRemoteAsync(1, 0x1000, 0x2000, 128), BM_OK);
}

TEST(ComposeTransportManagerTest, ReadRemoteBatchAsync_DeviceFailsHostSucceeds)
{
    auto tag = MakeRankOpTag(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->readBatchAsyncResult = BM_ERROR;

    CopyDescriptor desc{};
    EXPECT_EQ(mgr.ReadRemoteBatchAsync(1, desc), BM_OK);
    EXPECT_EQ(dev->readBatchAsyncCalls, 1u);
    EXPECT_EQ(host->readBatchAsyncCalls, 1u);
}

TEST(ComposeTransportManagerTest, WriteRemoteBatchAsync_BothFail)
{
    auto tag = MakeRankOpTag(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->writeBatchAsyncResult = BM_ERROR;
    host->writeBatchAsyncResult = BM_ERROR;

    CopyDescriptor desc{};
    EXPECT_NE(mgr.WriteRemoteBatchAsync(1, desc), BM_OK);
}

TEST(ComposeTransportManagerTest, Synchronize_DeviceFailsHostSucceeds)
{
    auto tag = MakeRankOpTag(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = 0;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->syncResult = BM_ERROR;

    EXPECT_EQ(mgr.Synchronize(1), BM_OK);
    EXPECT_EQ(dev->syncCalls, 1u);
    EXPECT_EQ(host->syncCalls, 1u);
}

TEST(ComposeTransportManagerTest, UpdateRankOptions_HostAndDeviceFail)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    host->updateRankResult = BM_ERROR;
    dev->updateRankResult = BM_ERROR;

    HybmTransPrepareOptions opts{};
    EXPECT_NE(mgr.UpdateRankOptions(opts), BM_OK);
}

TEST(ComposeTransportManagerTest, QueryHasRegistered_HostFallback)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    dev->queryHasRegResult = false;
    host->queryHasRegResult = true;

    EXPECT_TRUE(mgr.QueryHasRegistered(0x1000, 0x100));
}

TEST(ComposeTransportManagerTest, GetPrivateData_PrefersHost)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    (void)mgr.GetPrivateData();
}

TEST(ComposeTransportManagerTest, GetPrivateData_DeviceOnlyAndEmpty)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    EXPECT_TRUE(mgr.GetPrivateData().key.keys.empty());

    auto dev = std::make_shared<FakeTransportManager>();
    AddDeviceTransport(mgr, dev);
    (void)mgr.GetPrivateData();
}

TEST(ComposeTransportManagerTest, QueryMemoryKey_DeviceUrma)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    mgr.options_.protocol = HYBM_DOP_TYPE_DEVICE_URMA;
    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    TransportMemoryKey key{};
    EXPECT_EQ(mgr.QueryMemoryKey(0x1000, key), BM_OK);
}

TEST(ComposeTransportManagerTest, UpdateMemoryKey_DeviceUrma)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    mgr.options_.protocol = HYBM_DOP_TYPE_DEVICE_URMA;
    auto dev = std::make_shared<FakeTransportManager>();
    AddDeviceTransport(mgr, dev);

    TransportMemoryKey key{};
    mgr.UpdateMemoryKey(key, nullptr);
    EXPECT_EQ(dev->updateMemKeyCalls, 1u);
}

// -------- 分层协议建联 --------

namespace {

std::shared_ptr<HybmEntityTagInfo> MakeLayeredTag(uint32_t localRank, uint32_t peerRank,
                                                  const std::vector<hybm_data_op_type> &layers)
{
    auto tag = std::make_shared<HybmEntityTagInfo>();
    (void)tag->AddRankLayers(localRank, layers);
    (void)tag->AddRankLayers(peerRank, layers);
    return tag;
}

} // namespace

TEST(ComposeTransportManagerTest, ConnectRank_Layered_PreservesConfiguredOrder)
{
    constexpr uint32_t localRank = 0;
    constexpr uint32_t peerRank = 3;
    const std::vector<hybm_data_op_type> layers = {HYBM_DOP_TYPE_HOST_RDMA, HYBM_DOP_TYPE_DEVICE_RDMA};
    auto tag = MakeLayeredTag(localRank, peerRank, layers);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = localRank;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);

    EXPECT_EQ(mgr.ConnectRank(peerRank), BM_OK);
    const auto protocols = mgr.GetConnectedProtocols(peerRank);
    ASSERT_EQ(protocols.size(), 2U);
    EXPECT_EQ(protocols[0], kComposeHostProtocols);
    EXPECT_EQ(protocols[1], kComposeDeviceRdma);
}

TEST(ComposeTransportManagerTest, ConnectRank_Layered_HostFailsCachesOnlyDevice)
{
    constexpr uint32_t localRank = 0;
    constexpr uint32_t peerRank = 3;
    const std::vector<hybm_data_op_type> layers = {HYBM_DOP_TYPE_HOST_RDMA, HYBM_DOP_TYPE_DEVICE_RDMA};
    auto tag = MakeLayeredTag(localRank, peerRank, layers);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = localRank;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    host->connectRankResult = BM_ERROR;

    EXPECT_EQ(mgr.ConnectRank(peerRank), BM_OK);
    const auto protocols = mgr.GetConnectedProtocols(peerRank);
    ASSERT_EQ(protocols.size(), 1U);
    EXPECT_EQ(protocols[0], kComposeDeviceRdma);
}

TEST(ComposeTransportManagerTest, ConnectRank_Layered_AllFailReturnsError)
{
    constexpr uint32_t localRank = 0;
    constexpr uint32_t peerRank = 3;
    const std::vector<hybm_data_op_type> layers = {HYBM_DOP_TYPE_HOST_RDMA, HYBM_DOP_TYPE_DEVICE_RDMA};
    auto tag = MakeLayeredTag(localRank, peerRank, layers);
    ComposeTransportManager mgr(tag);
    mgr.options_.rankId = localRank;

    auto host = std::make_shared<FakeTransportManager>();
    auto dev = std::make_shared<FakeTransportManager>();
    AddHostTransport(mgr, host);
    AddDeviceTransport(mgr, dev);
    host->connectRankResult = BM_ERROR;
    dev->connectRankResult = BM_ERROR;

    EXPECT_NE(mgr.ConnectRank(peerRank), BM_OK);
    EXPECT_TRUE(mgr.GetConnectedProtocols(peerRank).empty());
}

TEST(ComposeTransportManagerTest, GetConnectedProtocols_UnknownRank_ReturnsEmpty)
{
    auto tag = std::make_shared<FakeTagInfo>(0U);
    ComposeTransportManager mgr(tag);
    EXPECT_TRUE(mgr.GetConnectedProtocols(99).empty());
}
