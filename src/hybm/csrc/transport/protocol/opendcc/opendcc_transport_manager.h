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
#ifndef MF_OPENDCC_TRANSPORT_MANAGER_H
#define MF_OPENDCC_TRANSPORT_MANAGER_H
#ifdef BUILD_OPENDCC
#include <condition_variable>
#include <map>
#include <mutex>
#include <shared_mutex>
#include "hybm_transport_manager.h"
#include "dl_opendcc_api.h"
#include "acc_tcp_server.h"

namespace ock {
namespace mf {
namespace transport {
class OpenDccTransportManager : public TransportManager {
public:
    ~OpenDccTransportManager() override;
    Result OpenDevice(const TransportOptions &options) override;
    Result CloseDevice() override;
    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override;
    Result UnregisterMemoryRegion(uint64_t addr) override;
    bool QueryHasRegistered(uint64_t addr, uint64_t size) override;
    Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) override;
    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;
    Result Prepare(const HybmTransPrepareOptions &options) override;
    Result RemoveRanks(const std::vector<uint32_t> &ranks) override;
    Result ConnectRank(uint32_t rank) override;
    std::vector<uint32_t> GetConnectedProtocols(uint32_t rank) const override;
    Result UpdateRankOptions(const HybmTransPrepareOptions &options) override;
    const std::string &GetNic() const override
    {
        return options_.nic;
    }
    const TransportPrivateData GetPrivateData() const override;
    Result ReadRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result WriteRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result ReadRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result WriteRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size) override;
    Result Synchronize(uint32_t rank) override;
    Result WriteRemoteBatchAsync(uint32_t rank, const CopyDescriptor &descriptor) override;
    Result ReadRemoteBatchAsync(uint32_t rank, const CopyDescriptor &descriptor) override;

protected:
    virtual Result ContextOptions(const TransportOptions &options, hcc_options_t &context) = 0;
    virtual Result MemorySegment(const TransportMemoryRegion &mr, hcc_mem_segment_t &segment) = 0;

private:
    struct Memory {
        uint64_t base = 0;
        hcc_mem_segment_t segment{};
        uint32_t references = 1;
    };
    struct Peer {
        std::mutex operation;
        std::mutex mutex;
        std::condition_variable changed;
        hcc_tunnel_t tunnel = nullptr;
        acc::AccTcpLinkComplexPtr link;
        std::string nic;
        std::vector<Memory> memories;
        std::vector<char> response;
        genetty_type_t provider = GEN_TYPE_INVALID;
        uint32_t sequence = 0;
        uint32_t received = 0;
        Result result = BM_OK;
        bool ready = false;
        bool removed = false;
    };
    Result StartControl();
    int HandleControl(const acc::AccTcpRequestContext &request);
    Result Exchange(Peer &peer, int16_t phase, const hcc_link_info_t &info, std::vector<char> &reply);
    Result CreateTunnel(Peer &peer);
    Result Transfer(uint32_t rank, const CopyDescriptor &descriptor, genetty_op_type_t operation);
    std::shared_ptr<Peer> FindPeer(uint32_t rank) const;
    static Result ConvertError(int32_t result);
    static Result DecodeMemory(const TransportMemoryKey &key, Memory &memory);
    static const Memory *FindMemory(const std::vector<Memory> &memories, uint64_t address, uint64_t size);

    mutable std::mutex mutex_;
    std::shared_mutex ioGate_;
    TransportOptions options_{};
    hcc_options_t context_{};
    hcc_t communicator_ = nullptr;
    bool initialized_ = false;
    std::map<uint64_t, Memory> memories_;
    std::map<uint32_t, std::shared_ptr<Peer>> peers_;
    acc::AccTcpServerPtr server_;
};
} // namespace transport
} // namespace mf
} // namespace ock
#endif
#endif
