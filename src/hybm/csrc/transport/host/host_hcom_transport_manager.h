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

#ifndef MF_HYBRID_HOST_HCOM_TRANSPORT_MANAGER_H
#define MF_HYBRID_HOST_HCOM_TRANSPORT_MANAGER_H

#include <mutex>
#include <set>
#include <atomic>
#include "hybm_transport_manager.h"
#include "hcom_service_c_define.h"
#include "host_hcom_counter_stream.h"
#include "host_hcom_reconnector.h"
#include "host_hcom_submit_pool.h"
#include "mf_rwlock.h"

namespace ock {
namespace mf {
namespace transport {
namespace host {

struct HcomRuntimeConfig {
    uint64_t maxSliceSize;
    uint64_t recvDataSize;
};

struct HcomMemoryRegion {
    uint64_t lva;  // Local rank: lva=addr, Remote rank: lva is remote lva
    uint64_t addr; // Local rank: addr=lva, Remote rank: addr is gva or remote lva
    uint64_t size;
    TransportMemoryKey lKey;
    Service_MemoryRegion mr;

    bool operator<(const HcomMemoryRegion &b) const
    {
        return addr < b.addr;
    }
};

union HcomPayload {
    uint64_t payload;
    struct {
        uint32_t client;      // local(active side) rank id
        uint32_t serverAndEp; // (server rank << 4) | ep
    };
};

constexpr size_t KEYPASS_MAX_LEN = 10000;

class HcomTransportManager : public TransportManager {
public:
    static std::shared_ptr<HcomTransportManager> GetInstance()
    {
        static auto instance = std::make_shared<HcomTransportManager>();
        return instance;
    }

    Result OpenDevice(const TransportOptions &options) override;

    Result CloseDevice() override;

    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override;

    Result UnregisterMemoryRegion(uint64_t addr) override;

    bool QueryHasRegistered(uint64_t addr, uint64_t size) override;

    Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) override;

    uint32_t GetLinkCount() const override;
    /* 双连接(多 rail)：一个 channel 内的网卡连接数（= 建链时传的 url 数；单连接为 1）。
       注意别拿它替换 GetLinkCount()：后者是"channel/ep 数"，语义不同。 */
    uint32_t GetRailCount() const override;
    bool AllRailsReady(uint32_t rankId) const override;

    Result QueryMemoryKeyByEp(uint64_t addr, uint32_t ep, TransportMemoryKey &key) override;

    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;

    Result Prepare(const HybmTransPrepareOptions &parma) override;

    Result RemoveRanks(const std::vector<uint32_t> &removedRanks) override;

    Result Connect() override;

    Result ConnectRank(uint32_t rankId) override;
    Result WaitChannelReady(uint32_t rankId, uint32_t timeoutMs) noexcept;

    Result AsyncConnect() override;

    Result WaitForConnected(int64_t timeoutNs) override;

    Result UpdateRankOptions(const HybmTransPrepareOptions &param) override;

    const std::string &GetNic() const override;

    const TransportPrivateData GetPrivateData() const override;

    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result SubmitWriteBatchOnEp(uint32_t rankId, uint32_t ep, const CopyDescriptor &descriptor, size_t begin,
                                size_t end) override;

    Result WriteRemoteAsyncOnEp(uint32_t rankId, uint32_t ep, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    /* 双连接：带 rail 的版本（rail 与 ep 不同 —— 多 rail 共用 ep0 的 channel，railIdx 决定走哪张网卡） */
    Result SubmitWriteBatchOnEpOnRail(uint32_t rankId, uint32_t ep, int32_t railIdx, const CopyDescriptor &descriptor,
                                      size_t begin, size_t end) override;
    Result WriteRemoteAsyncOnEpOnRail(uint32_t rankId, uint32_t ep, int32_t railIdx, uint64_t lAddr, uint64_t rAddr,
                                      uint64_t size) override;

    bool AllLinksReady(uint32_t rankId) const override;

    Result Synchronize(uint32_t rankId) override;

private:
    Result InnerReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size);

    Result InnerWriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size);

    Result SubmitWriteBatchSlice(uint32_t rankId, uint32_t ep, const CopyDescriptor &descriptor, size_t begin,
                                 size_t end, int32_t railIdx = -1);

    Result SubmitReadBatchSlice(uint32_t rankId, uint32_t ep, const CopyDescriptor &descriptor, size_t begin,
                                size_t end);

    /* WriteRemoteAsyncOnEp / WriteRemoteAsyncOnEpOnRail 的公共实现：railIdx < 0 表示不带 rail */
    Result WriteRemoteAsyncOnEpImpl(uint32_t rankId, uint32_t ep, uint64_t lAddr, uint64_t rAddr, uint64_t size,
                                    int32_t railIdx);

    Result CheckTransportOptions(const TransportOptions &options);

    static Result TransportRpcHcomNewEndPoint(Hcom_Channel newCh, uint64_t usrCtx, const char *payLoad);

    static Result TransportRpcHcomEndPointBroken(Hcom_Channel ch, uint64_t usrCtx, const char *payLoad);

    static Result TransportRpcHcomRequestReceived(Service_Context ctx, uint64_t usrCtx);

    static Result TransportRpcHcomRequestPosted(Service_Context ctx, uint64_t usrCtx);

    static Result TransportRpcHcomOneSideDone(Service_Context ctx, uint64_t usrCtx);

    Result ConnectHcomChannel(uint32_t rankId, uint32_t ep, const std::string &url);

    void DisConnectHcomChannel(uint32_t rankId, uint32_t ep, Hcom_Channel ch);

    void ClearRankChannels(uint32_t rankId);

    Result ConnectTargets(const std::vector<uint32_t> &targets);

    void HcomChannelDisconnected(uint32_t rankId, uint32_t ep, Hcom_Channel ch);

    Result GetMemoryRegionByAddr(const uint32_t &rankId, const uint32_t &ep, const uint64_t &addr,
                                 HcomMemoryRegion &mr);
    /* 与上面同义，但返回本线程 MR 缓存槽里的指针，不再把整个 HcomMemoryRegion(~250B) 拷回调用方。
       HcomMemoryRegion 里 lva/addr/size/lKey/mr 共约 250 字节，批量提交时每个 iov 要查 2 次
       （本端 + 远端），原先每 iov 有 ~1KB 的清零+拷贝开销。
       返回指针指向 thread_local 槽位，有效期到本线程下一次查询该槽为止，只可即时读取。 */
    const HcomMemoryRegion *FindMemoryRegionByAddr(uint32_t rankId, uint32_t ep, uint64_t addr);

    Result UpdateRankMrInfos(const std::unordered_map<uint32_t, TransportRankPrepareInfo> &opt);

    Result UpdateRankConnectInfos(const std::unordered_map<uint32_t, TransportRankPrepareInfo> &options);

    static int GetCACallBack(const char *name, char **caPath, char **crlPath, Hcom_PeerCertVerifyType *verifyType,
                             Hcom_TlsCertVerify *verify);

    static int GetCertCallBack(const char *name, char **certPath);

    static int GetPrivateKeyCallBack(const char *name, char **priKeyPath, char **keyPass, Hcom_TlsKeyPassErase *erase);

    static int CertVerifyCallBack(void *x509, const char *crlPath);

    static void KeyPassEraseCallBack(char *keyPass, int len);

    static void ChannelAsyncCallback(void *arg, Service_Context context);

    int PrepareThreadLocalStream();

    void DestroyServices();

    void SetHcomServiceConfig(Hcom_Service service);

private:
    hybm_data_op_type bmOptype_{};
    ReadWriteLock lock_;
    static thread_local HcomCounterStreamPtr stream_;
    /* MR 查询快路径缓存：mrs_ 每次变动 mrGen_ +1；代际一致时本线程直接复用上次命中的 MR，
       省掉每次查询的 mrMutex_ 加锁与遍历。批量写 600 个 iov 原本要 1200 次加锁，
       且 mrMutex_ 是每 rank 一把，多链路时正是两个提交 worker 的争用点。
       注意必须用**多个槽位**：一个 SGL 请求里会交替查询"本端地址"(rankId_) 和"远端地址"(rankId)，
       单槽会被交替击穿、几乎全部退化成慢路径（实测 32 次查询 ≈ 3.3us/请求）。 */
    static constexpr uint32_t MR_HIT_SLOTS = 4;
    struct MrHitCache {
        const void *self;
        uint64_t gen;
        uint32_t rankId;
        uint32_t ep;
        HcomMemoryRegion mr;
    };
    static uint32_t MrHitSlot(uint32_t rankId, uint32_t ep) noexcept
    {
        return (rankId * 131U + ep * 7U) % MR_HIT_SLOTS;
    }
    static thread_local MrHitCache tlsMrHit_[MR_HIT_SLOTS];
    std::atomic<uint64_t> mrGen_{1};
    void BumpMrGeneration() noexcept
    {
        mrGen_.fetch_add(1, std::memory_order_acq_rel);
    }
    std::string localNic_{};                 // local listen url (single link); multi-link joined by ';'
    std::string localIp_{};
    std::string localIpMask_{};              // 本地多网卡的 ipMask 组(',' 分隔)，交给 ServiceSetDeviceIpMask
    std::vector<std::string> localNics_{};   // per-ep local listen urls
    std::vector<std::string> localIps_{};    // per-ep local nic ip (for ServiceSetDeviceIpMask)
    // 一个 hcom service（多网卡由库内部 MultiRail 建多条 rail）
    std::vector<Hcom_Service> rpcServices_;
    HcomRuntimeConfig runtimeConfig_{};
    uint32_t rankId_{UINT32_MAX};
    uint32_t rankCount_{0};
    uint32_t epCount_{1}; // per-rank endpoint(link) count, 1 for single link
    std::vector<std::mutex> mrMutex_;
    std::vector<std::vector<std::set<HcomMemoryRegion>>> mrs_; // [rankId][ep]
    std::vector<std::mutex> channelMutex_;
    std::vector<std::vector<std::string>> nics_;      // [rankId][ep]
    std::vector<std::vector<Hcom_Channel>> channels_; // [rankId][ep]
    HostSubmitPool submitPool_; // 常驻 worker：multi-link batch 分片并发提交
    HcomReconnector reconnect_;
    static hybm_tls_config tlsConfig_;
    static char keyPass_[KEYPASS_MAX_LEN];
    static std::mutex keyPassMutex;
};
} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HOST_HCOM_TRANSPORT_MANAGER_H
