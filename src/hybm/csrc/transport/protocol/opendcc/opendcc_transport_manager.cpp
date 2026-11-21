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
#ifdef BUILD_OPENDCC
#include "opendcc_transport_manager.h"
#include <cstdlib>
#include <endian.h>
#include "hybm_logger.h"
#include "hybm_va_manager.h"
#include "mf_env_define.h"
#include "mf_env_util.h"

namespace ock {
namespace mf {
namespace transport {
namespace {
constexpr uint64_t MEMORY_FORMAT = 0x4f44434300000001ULL;
constexpr size_t MEMORY_HEADER_WORDS = 4;
constexpr uint32_t REQUEST_TIMEOUT_MS = 30000;
} // namespace

Result OpenDccTransportManager::ConvertError(int32_t result)
{
    switch (result) {
        case HCC_OK:
            return BM_OK;
        case HCC_INVALID_PARAM:
            return BM_INVALID_PARAM;
        case HCC_MALLOC_FAILED:
            return BM_MALLOC_FAILED;
        case HCC_NOT_INITIALIZED:
            return BM_NOT_INITIALIZED;
        case HCC_NOT_FOUND:
            return BM_INVALID_PARAM;
        case HCC_CONNECT_FAILED:
            return BM_NOT_CONNECTED;
        case HCC_NOT_SUPPORTED:
            return BM_NOT_SUPPORTED;
        case HCC_TIMEOUT:
            return BM_TIMEOUT;
        default:
            return BM_DL_FUNCTION_FAILED;
    }
}

OpenDccTransportManager::~OpenDccTransportManager()
{
    if (CloseDevice() != BM_OK) {
        BM_LOG_ERROR("openDCC cleanup could not stop hardware access; terminating before memory release");
        std::abort();
    }
}

Result OpenDccTransportManager::OpenDevice(const TransportOptions &options)
{
    options_ = options;
    BM_ASSERT_LOG_AND_RETURN(communicator_ == nullptr && options.protocol == HYBM_DOP_TYPE_DEVICE_RDMA &&
                                 options.initialType == HYBM_TYPE_HOST_INITIATE && !options.isProxy &&
                                 options.scene == HYBM_SCENE_DEFAULT,
                             "Unsupported openDCC transport configuration", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_RDMA_USE_HCOMM, 0U) == 0,
                             "openDCC conflicts with HCOMM selection", BM_INVALID_PARAM);
    auto ret = DlOpenDccApi::Load();
    if (ret != BM_OK) {
        return ret;
    }
    DlOpenDccApi::hcc_options_init(&context_);
    ret = ContextOptions(options, context_);
    if (ret != BM_OK) {
        return ret;
    }
    bool device = context_.provider == DEVICE_RDMA_GENETTY;
    ret = DlOpenDccApi::hcc_init(device ? GEN_PROT_DEVICE_RDMA : GEN_PROT_HOST_RDMA, device ? "ascend" : "kunpeng", 0);
    if (ret != HCC_OK) {
        return ConvertError(ret);
    }
    initialized_ = true;
    communicator_ = DlOpenDccApi::hcc_create(&context_);
    BM_ASSERT_LOG_AND_RETURN(communicator_ != nullptr, "Create openDCC communicator failed", BM_DL_FUNCTION_FAILED);
    ret = StartControl();
    BM_LOG_INFO("openDCC transport rank=" << options.rankId << " provider=" << context_.provider << " ret=" << ret);
    return ret;
}

Result OpenDccTransportManager::CloseDevice()
{
    if (server_ != nullptr) {
        server_->Stop();
        server_ = nullptr;
    }
    std::unique_lock<std::shared_mutex> gate(ioGate_);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &entry : peers_) {
        auto &peer = *entry.second;
        if (peer.tunnel != nullptr) {
            auto ret = DlOpenDccApi::hcc_tunnel_destroy(&peer.tunnel, 0);
            if (ret != HCC_OK) {
                return ConvertError(ret);
            }
        }
        peer.removed = true;
        peer.changed.notify_all();
    }
    peers_.clear();
    if (communicator_ != nullptr) {
        auto ret = DlOpenDccApi::hcc_destroy(&communicator_, 0);
        if (ret != HCC_OK) {
            return ConvertError(ret);
        }
    }
    memories_.clear();
    if (initialized_) {
        DlOpenDccApi::hcc_uninit(0);
        initialized_ = false;
    }
    return BM_OK;
}

Result OpenDccTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    std::shared_lock<std::shared_mutex> gate(ioGate_);
    std::lock_guard<std::mutex> lock(mutex_);
    hcc_mem_segment_t segment{};
    auto ret = MemorySegment(mr, segment);
    if (ret != BM_OK) {
        return ret;
    }
    ret = DlOpenDccApi::hcc_memory_register(&communicator_, &segment, 0);
    if (ret != HCC_OK) {
        return ConvertError(ret);
    }
    BM_LOG_INFO("openDCC MR rank=" << options_.rankId << " type=" << segment.type << " address=0x" << std::hex
                                   << segment.address << std::dec << " size=" << segment.size);
    auto iter = memories_.find(mr.addr);
    if (iter != memories_.end()) {
        ++iter->second.references;
    } else {
        memories_.emplace(mr.addr, Memory{mr.addr, segment, 1});
    }
    return BM_OK;
}

Result OpenDccTransportManager::UnregisterMemoryRegion(uint64_t address)
{
    std::unique_lock<std::shared_mutex> gate(ioGate_);
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = memories_.find(address);
    BM_ASSERT_LOG_AND_RETURN(iter != memories_.end(), "Unknown openDCC MR: " << address, BM_INVALID_PARAM);
    auto ret = DlOpenDccApi::hcc_memory_unregister(&communicator_, &iter->second.segment, 0);
    if (ret != HCC_OK) {
        return ConvertError(ret);
    }
    if (--iter->second.references == 0) {
        memories_.erase(iter);
    }
    return BM_OK;
}

bool OpenDccTransportManager::QueryHasRegistered(uint64_t address, uint64_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &entry : memories_) {
        if (ContainsAddressRange(entry.first, entry.second.segment.size, address, size)) {
            return true;
        }
    }
    return false;
}

Result OpenDccTransportManager::QueryMemoryKey(uint64_t address, TransportMemoryKey &key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &entry : memories_) {
        auto &memory = entry.second;
        if (!ContainsAddressRange(memory.base, memory.segment.size, address, 1)) {
            continue;
        }
        hcc_memory_info_t info{};
        auto ret = DlOpenDccApi::hcc_memory_info_encode(&memory.segment, 0, &info);
        if (ret != HCC_OK) {
            return ConvertError(ret);
        }
        uint64_t base = HybmVaManager::GetInstance().TransformVa(memory.base, HVM_HVA, HVM_GVA);
        if (base == 0 || base == HVM_INVALID_VA) {
            base = memory.base;
        }
        key.ResizeBytes(MEMORY_HEADER_WORDS * sizeof(uint64_t) + info.length);
        key.keys[0] = htobe64(MEMORY_FORMAT);
        key.keys[1] = htobe64(base);
        key.keys[2] = htobe64(memory.segment.size);
        key.keys[3] = htobe64(info.length);
        std::memcpy(key.keys.data() + MEMORY_HEADER_WORDS, info.data, info.length);
        DlOpenDccApi::hcc_memory_free_info(&info);
        return BM_OK;
    }
    BM_LOG_ERROR("No registered openDCC memory key for address " << address);
    return BM_INVALID_PARAM;
}

Result OpenDccTransportManager::DecodeMemory(const TransportMemoryKey &key, Memory &memory)
{
    BM_ASSERT_LOG_AND_RETURN(key.WordCount() >= MEMORY_HEADER_WORDS && key.WordCount() <= TRANS_KEY_MAX_WORDS &&
                                 be64toh(key.keys[0]) == MEMORY_FORMAT,
                             "Invalid openDCC memory key version or size", BM_INVALID_PARAM);
    auto length = be64toh(key.keys[3]);
    BM_ASSERT_LOG_AND_RETURN(length != 0 && length <= key.ByteSize() - MEMORY_HEADER_WORDS * sizeof(uint64_t),
                             "Invalid openDCC memory payload length", BM_INVALID_PARAM);
    hcc_memory_info_t info{reinterpret_cast<char *>(const_cast<uint64_t *>(key.keys.data() + MEMORY_HEADER_WORDS)),
                           static_cast<uint32_t>(length)};
    auto ret = DlOpenDccApi::hcc_memory_info_decode(&info, &memory.segment, 0);
    if (ret != HCC_OK) {
        return ConvertError(ret);
    }
    memory.base = be64toh(key.keys[1]);
    BM_ASSERT_LOG_AND_RETURN(memory.segment.size == be64toh(key.keys[2]), "Inconsistent openDCC MR size",
                             BM_INVALID_PARAM);
    return BM_OK;
}

void OpenDccTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *address)
{
    Memory memory;
    if (DecodeMemory(key, memory) == BM_OK) {
        key.keys[1] = htobe64(reinterpret_cast<uint64_t>(address));
    }
}

const TransportPrivateData OpenDccTransportManager::GetPrivateData() const
{
    TransportPrivateData data{};
    data.key.keys = {htobe64(MEMORY_FORMAT), htobe64(context_.provider)};
    return data;
}

Result OpenDccTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    std::unique_lock<std::shared_mutex> gate(ioGate_);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &entry : options.options) {
        if (entry.first == options_.rankId) {
            continue;
        }
        const auto &info = entry.second;
        BM_ASSERT_LOG_AND_RETURN(entry.first < options_.rankCount && !info.isProvider &&
                                     info.privateData.key.WordCount() == 2 &&
                                     be64toh(info.privateData.key.keys[0]) == MEMORY_FORMAT,
                                 "Invalid openDCC peer metadata, rank=" << entry.first, BM_INVALID_PARAM);
        auto &peer = peers_[entry.first];
        if (peer == nullptr) {
            peer = std::make_shared<Peer>();
        }
        std::lock_guard<std::mutex> peerLock(peer->mutex);
        peer->nic = info.nic;
        peer->provider = static_cast<genetty_type_t>(be64toh(info.privateData.key.keys[1]));
        std::vector<Memory> memories;
        for (const auto &key : info.memKeys) {
            Memory memory;
            auto ret = DecodeMemory(key, memory);
            if (ret != BM_OK) {
                return ret;
            }
            memories.push_back(memory);
        }
        peer->memories.swap(memories);
    }
    return BM_OK;
}

Result OpenDccTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    return Prepare(options);
}

std::shared_ptr<OpenDccTransportManager::Peer> OpenDccTransportManager::FindPeer(uint32_t rank) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = peers_.find(rank);
    return iter == peers_.end() ? nullptr : iter->second;
}

std::vector<uint32_t> OpenDccTransportManager::GetConnectedProtocols(uint32_t rank) const
{
    auto peer = FindPeer(rank);
    if (peer != nullptr) {
        std::lock_guard<std::mutex> lock(peer->mutex);
        if (peer->ready && !peer->removed) {
            return {HYBM_DOP_TYPE_DEVICE_RDMA};
        }
    }
    return {};
}

Result OpenDccTransportManager::RemoveRanks(const std::vector<uint32_t> &ranks)
{
    std::unique_lock<std::shared_mutex> gate(ioGate_);
    for (auto rank : ranks) {
        auto peer = FindPeer(rank);
        if (peer == nullptr) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(peer->mutex);
            peer->removed = true;
            peer->ready = false;
            peer->changed.notify_all();
            if (peer->tunnel != nullptr) {
                auto ret = DlOpenDccApi::hcc_tunnel_destroy(&peer->tunnel, 0);
                if (ret != HCC_OK) {
                    return ConvertError(ret);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            peers_.erase(rank);
        }
        if (peer->link != nullptr && server_ != nullptr) {
            (void)server_->ReleaseLink(peer->link);
        }
    }
    return BM_OK;
}

const OpenDccTransportManager::Memory *OpenDccTransportManager::FindMemory(const std::vector<Memory> &memories,
                                                                           uint64_t address, uint64_t size)
{
    for (const auto &memory : memories) {
        if (ContainsAddressRange(memory.base, memory.segment.size, address, size)) {
            return &memory;
        }
    }
    return nullptr;
}

Result OpenDccTransportManager::Transfer(uint32_t rank, const CopyDescriptor &descriptor, genetty_op_type_t operation)
{
    std::shared_lock<std::shared_mutex> gate(ioGate_);
    auto peer = FindPeer(rank);
    BM_ASSERT_LOG_AND_RETURN(peer != nullptr, "Unknown openDCC rank " << rank, BM_NOT_CONNECTED);
    std::lock_guard<std::mutex> operationLock(peer->operation);
    std::lock_guard<std::mutex> peerLock(peer->mutex);
    BM_ASSERT_LOG_AND_RETURN(peer->ready && !peer->removed, "openDCC peer not ready, rank=" << rank, BM_NOT_CONNECTED);
    std::vector<Memory> locals;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : memories_) {
            locals.push_back(entry.second);
        }
    }
    BM_ASSERT_LOG_AND_RETURN(descriptor.counts.size() == descriptor.localAddrs.size() &&
                                 descriptor.counts.size() == descriptor.globalAddrs.size(),
                             "Mismatched openDCC batch arrays", BM_INVALID_PARAM);
    std::vector<hcc_copy_desc_t> descs;
    for (size_t i = 0; i < descriptor.counts.size(); ++i) {
        uint64_t local = reinterpret_cast<uint64_t>(descriptor.localAddrs[i]);
        uint64_t remote = reinterpret_cast<uint64_t>(descriptor.globalAddrs[i]);
        auto size = descriptor.counts[i];
        auto lm = FindMemory(locals, local, size);
        auto rm = FindMemory(peer->memories, remote, size);
        BM_ASSERT_LOG_AND_RETURN(lm != nullptr && rm != nullptr, "Unregistered openDCC range, rank=" << rank,
                                 BM_INVALID_PARAM);
        descs.push_back({lm->segment.address + local - lm->base, rm->segment.address + remote - rm->base, size,
                         lm->segment.lkey, rm->segment.rkey});
    }
    if (descs.empty()) {
        return BM_OK;
    }
    hcc_request_t request = nullptr;
    auto ret = DlOpenDccApi::hcc_tunnel_submit(peer->tunnel, operation, descs.data(), descs.size(), &request);
    if (ret == HCC_OK) {
        ret = DlOpenDccApi::hcc_request_wait(request, REQUEST_TIMEOUT_MS);
    }
    if (ret != HCC_OK && request != nullptr) {
        peer->ready = false;
        if (DlOpenDccApi::hcc_tunnel_quiesce(peer->tunnel) != HCC_OK) {
            BM_LOG_ERROR("Cannot stop DMA after openDCC failure, rank=" << rank);
            std::abort();
        }
    }
    if (request != nullptr && DlOpenDccApi::hcc_request_release(&request) != HCC_OK) {
        BM_LOG_ERROR("Cannot release completed openDCC request, rank=" << rank);
        std::abort();
    }
    if (ret == HCC_OK) {
        for (const auto &desc : descs) {
            BM_LOG_DEBUG("openDCC completed op=" << operation << " rank=" << rank << " local=0x" << std::hex
                                                 << desc.local_address << " remote=0x" << desc.remote_address
                                                 << std::dec << " size=" << desc.size);
        }
    }
    return ConvertError(ret);
}

Result OpenDccTransportManager::Synchronize(uint32_t rank)
{
    BM_ASSERT_LOG_AND_RETURN(!GetConnectedProtocols(rank).empty(), "openDCC peer not ready, rank=" << rank,
                             BM_NOT_CONNECTED);
    return BM_OK;
}

Result OpenDccTransportManager::ReadRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    CopyDescriptor descriptor{};
    descriptor.localAddrs = {reinterpret_cast<void *>(local)};
    descriptor.globalAddrs = {reinterpret_cast<void *>(remote)};
    descriptor.counts = {size};
    return Transfer(rank, descriptor, OP_READ);
}

Result OpenDccTransportManager::ReadRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    return ReadRemote(rank, local, remote, size);
}

Result OpenDccTransportManager::ReadRemoteBatchAsync(uint32_t rank, const CopyDescriptor &descriptor)
{
    return Transfer(rank, descriptor, OP_READ);
}

Result OpenDccTransportManager::WriteRemote(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    CopyDescriptor descriptor{};
    descriptor.localAddrs = {reinterpret_cast<void *>(local)};
    descriptor.globalAddrs = {reinterpret_cast<void *>(remote)};
    descriptor.counts = {size};
    return Transfer(rank, descriptor, OP_WRITE);
}

Result OpenDccTransportManager::WriteRemoteAsync(uint32_t rank, uint64_t local, uint64_t remote, uint64_t size)
{
    return WriteRemote(rank, local, remote, size);
}

Result OpenDccTransportManager::WriteRemoteBatchAsync(uint32_t rank, const CopyDescriptor &descriptor)
{
    return Transfer(rank, descriptor, OP_WRITE);
}
} // namespace transport
} // namespace mf
} // namespace ock
#endif
