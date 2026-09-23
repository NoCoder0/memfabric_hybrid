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

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include "hybm_def.h"
#include "hybm_transport_manager.h"
#include "hcom_service_c_define.h"
#include "host_hcom_counter_stream.h"
#include "host_hcom_reconnector.h"
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
        uint32_t client;
        uint32_t server;
    };
};

constexpr size_t KEYPASS_MAX_LEN = 10000;

class HcomTransportManager : public TransportManager {
public:
    // Only for unit tests / compatibility; the production path (ComposeTransportManager::OpenHostTransport)
    // creates one independent instance per entity.
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

    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;

    Result Prepare(const HybmTransPrepareOptions &parma) override;

    Result RemoveRanks(const std::vector<uint32_t> &removedRanks) override;

    Result ConnectRank(uint32_t rankId) override;
    Result WaitChannelReady(uint32_t rankId, uint32_t timeoutMs) noexcept;

    Result UpdateRankOptions(const HybmTransPrepareOptions &param) override;

    const std::string &GetNic() const override;

    const TransportPrivateData GetPrivateData() const override;

    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;

    Result Synchronize(uint32_t rankId) override;

private:
    Result InnerReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size);

    Result InnerWriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size);

    Result CheckTransportOptions(const TransportOptions &options);

    // Binds from basePort + group rankId and retries with a small increment on conflict; on success localNic_
    // holds the address actually bound.
    Result BindServiceWithPortRetry();

    // ---- per-instance callback routing ----
    // HCOM's static callbacks carry no instance context, so every instance gets a slot and a per-slot
    // trampoline table routes to the matching instance.
    static constexpr size_t kMaxInstances = 64U;
    static constexpr size_t kInvalidSlot = static_cast<size_t>(-1);

    inline static std::array<std::atomic<HcomTransportManager *>, kMaxInstances> g_slotInstances{};

    template<size_t I>
    static Result NewEndPointTramp(Hcom_Channel ch, uint64_t, const char *payLoad)
    {
        auto *self = g_slotInstances[I].load(std::memory_order_acquire);
        return (self != nullptr) ? self->OnNewEndPoint(ch, payLoad) : BM_ERROR;
    }

    template<size_t I>
    static Result EndPointBrokenTramp(Hcom_Channel ch, uint64_t, const char *payLoad)
    {
        auto *self = g_slotInstances[I].load(std::memory_order_acquire);
        return (self != nullptr) ? self->OnEndPointBroken(ch, payLoad) : BM_ERROR;
    }

    template<size_t... Is>
    static std::array<Service_ChannelHandler, sizeof...(Is)> MakeNewEndPointTramps(std::index_sequence<Is...>)
    {
        return {{&NewEndPointTramp<Is>...}};
    }

    template<size_t... Is>
    static std::array<Service_ChannelHandler, sizeof...(Is)> MakeEndPointBrokenTramps(std::index_sequence<Is...>)
    {
        return {{&EndPointBrokenTramp<Is>...}};
    }

    // Built on first call (magic static): member function bodies are not yet visible when in-class initializers
    // are evaluated.
    static const std::array<Service_ChannelHandler, kMaxInstances> &NewEndPointTramps()
    {
        static const std::array<Service_ChannelHandler, kMaxInstances> tramps =
            MakeNewEndPointTramps(std::make_index_sequence<kMaxInstances>{});
        return tramps;
    }

    // The two tables must be generated separately: the trampoline table pins function pointers per slot, and
    // mixing them would deliver link-break events to the wrong callback.
    static const std::array<Service_ChannelHandler, kMaxInstances> &EndPointBrokenTramps()
    {
        static const std::array<Service_ChannelHandler, kMaxInstances> tramps =
            MakeEndPointBrokenTramps(std::make_index_sequence<kMaxInstances>{});
        return tramps;
    }

    Result OnNewEndPoint(Hcom_Channel newCh, const char *payLoad);

    Result OnEndPointBroken(Hcom_Channel ch, const char *payLoad);

    bool BindSlot();

    void ReleaseSlot();

    static Result TransportRpcHcomRequestReceived(Service_Context ctx, uint64_t usrCtx);

    static Result TransportRpcHcomRequestPosted(Service_Context ctx, uint64_t usrCtx);

    static Result TransportRpcHcomOneSideDone(Service_Context ctx, uint64_t usrCtx);

    Result ConnectHcomChannel(uint32_t rankId, const std::string &url);

    void DisConnectHcomChannel(uint32_t rankId, Hcom_Channel ch);

    void HcomChannelDisconnected(uint32_t rankId, Hcom_Channel ch);

    Result GetMemoryRegionByAddr(const uint32_t &rankId, const uint64_t &addr, HcomMemoryRegion &mr);

    // When an MR at the same address exists: same address and same size counts as a multi-entity replay and is
    // reused with the reference count incremented; otherwise it is reported as a range conflict.
    Result HandleExistingMemoryRegion(const TransportMemoryRegion &mr, const HcomMemoryRegion &info);

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

    void SetHcomServiceConfig(Hcom_Service service);

private:
    hybm_data_op_type bmOptype_{};
    ReadWriteLock lock_;
    static thread_local HcomCounterStreamPtr stream_;
    std::string localNic_{};
    std::string localIp_{};
    Hcom_Service rpcService_{0};
    HcomRuntimeConfig runtimeConfig_{};
    uint32_t rankId_{UINT32_MAX};
    uint32_t rankCount_{0};
    std::vector<std::mutex> mrMutex_;
    std::vector<std::set<HcomMemoryRegion>> mrs_;
    // MR reference count (keyed by addr): several entities register/deregister the same range, and the MR is
    // destroyed when the last referrer deregisters.
    std::vector<std::map<uint64_t, uint32_t>> mrRefCnts_;
    // service reference count: the hcom service is really destroyed only after all entities have closed it
    std::mutex openMutex_;
    uint32_t openRefCnt_{0};
    std::vector<std::mutex> channelMutex_;
    std::vector<std::string> nics_;
    std::vector<Hcom_Channel> channels_;
    HcomReconnector reconnect_;
    // per-instance: callback routing slot and independent service name
    size_t slot_{kInvalidSlot};
    std::string serviceName_{};
    std::string nicProtocol_{};
    uint32_t hcomAutoPort_{0};
    static hybm_tls_config tlsConfig_;
    static char keyPass_[KEYPASS_MAX_LEN];
    static std::mutex keyPassMutex;
};
} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HOST_HCOM_TRANSPORT_MANAGER_H
