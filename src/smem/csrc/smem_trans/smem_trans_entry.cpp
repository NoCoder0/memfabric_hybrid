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
#include "smem_trans_entry.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include "mf_syntactic_sugar.h"
#include "hybm.h"
#include "hybm_big_mem.h"
#include "hybm_data_op.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "mf_fault_injection_point.h"
#include "mf_str_util.h"
#include "mf_syntactic_sugar.h"
#include "smem_net_common.h"
#include "smem_store_factory.h"
#include "smem_trans_def.h"
#include "smem_trans_entry_manager.h"
#include "mf_fault_injection_point.h"
#include "smem_tcp_config_store.h"
#include "smem_group_manager_client.h"
#include "smem_trans_entry.h"

namespace ock {
namespace smem {
// reserve 128GB hbm va for malloc per rank, refine to configurable later
constexpr uint64_t TRANS_RESERVE_HBM_VA_SIZE = 1024ULL * 1024 * 1024 * 128; // 128G
constexpr uint64_t TRANS_RESERVE_DRAM_VA_SIZE = 1024ULL * 1024 * 1024 * 64; // 64G
// LinkState wire values (keep in sync with LinkState in smem_group_manager_server.h)
constexpr int8_t TRANS_LINK_IDLE = 0;
constexpr int8_t TRANS_LINK_CONNECTED = 4;

SmemTransEntryPtr SmemTransEntry::Create(const std::string &name, const std::string &storeUrl,
                                         const smem_trans_config_t &config)
{
    /* create entry and initialize */
    SmemTransEntryPtr transEntry;
    auto result = SmemTransEntryManager::Instance().CreateEntryByName(name, storeUrl, config, transEntry);
    if (result != SM_OK) {
        SM_LOG_AND_SET_LAST_ERROR("create trans entry failed, result: " << result << ", name: " << name);
        return nullptr;
    }

    /* initialize */
    result = transEntry->Initialize();
    if (result != SM_OK) {
        SmemTransEntryManager::Instance().RemoveEntryByPtr(reinterpret_cast<uintptr_t>(transEntry.Get()));
        SM_LOG_AND_SET_LAST_ERROR("initialize trans entry failed, result " << result);
        return nullptr;
    }

    result = transEntry->Join(0);
    if (result != SM_OK) {
        SM_LOG_AND_SET_LAST_ERROR("trans join failed, ret:" << result);
        SmemTransEntryManager::Instance().RemoveEntryByPtr(reinterpret_cast<uintptr_t>(transEntry.Get()));
        return nullptr;
    }
    return transEntry;
}

SmemTransEntry::~SmemTransEntry()
{
    UnInitialize();
}

int32_t SmemTransEntry::Initialize()
{
    SM_VALIDATE_RETURN(rankId_ < SMEM_TRANS_RANK_COUNT_MAX, "rankId:" << rankId_ << " is too large.", SM_INVALID_PARAM);
    if (!ParseTransName(name_, workerUniqueId_.address, workerUniqueId_.port)) {
        return SM_INVALID_PARAM;
    }

    auto coreStore = store_->GetCoreStore();
    auto tcpStore = dynamic_cast<TcpConfigStore *>(coreStore.Get());
    if (tcpStore == nullptr) {
        SM_LOG_ERROR("Initialize failed, tcp store is nullptr");
        return SM_ERROR;
    }

    if (auto ret = SetupGroupManagerCallbacks(); ret != SMEM_OK) {
        SM_LOG_ERROR("Initialize failed to setup callbacks, ret: " << ret);
        return SM_ERROR;
    }

    auto options = GenerateHybmOptions();
    options.bmDataOpType = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_DEFAULT);
    if (!ApplyDataOpType(options, SMEMB_DATA_OP_SDMA, HYBM_DOP_TYPE_SDMA, "device_sdma") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_DEVICE_RDMA, HYBM_DOP_TYPE_DEVICE_RDMA, "device_rdma") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_DEVICE_URMA, HYBM_DOP_TYPE_DEVICE_URMA, "device_urma") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_DEVICE_UBOE, HYBM_DOP_TYPE_DEVICE_UBOE, "device_uboe") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_HOST_RDMA, HYBM_DOP_TYPE_HOST_RDMA, "host_rdma") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_HOST_URMA, HYBM_DOP_TYPE_HOST_URMA, "host_urma") ||
        !ApplyDataOpType(options, SMEMB_DATA_OP_HOST_TCP, HYBM_DOP_TYPE_HOST_TCP, "host_tcp")) {
        return SM_ERROR;
    }

    auto entityId = entityId_ + HYBM_ENTITY_ID_TRANS_BASE;
    entity_ = hybm_create_entity(entityId, &options, 0);
    SM_VALIDATE_RETURN(entity_ != nullptr,
                       "hybm_create_entity failed, entityId: " << entityId << " rankId: " << rankId_, SM_ERROR);

    auto ret = hybm_reserve_mem_space(entity_, 0);
    SM_VALIDATE_RETURN(ret == SM_OK, "hybm_reserve_mem_space failed, ret: " << ret, SM_ERROR);

    if (auto ret = hybm_export(entity_, nullptr, HYBM_FLAG_EXPORT_ENTITY, &entityInfo_.hybmInfo); ret != SMEM_OK) {
        SM_LOG_ERROR("HybmExport device info failed: " << ret);
        hybm_export_info_free(&entityInfo_.hybmInfo);
        return SM_ERROR;
    }

    entityInfo_.u.session = workerUniqueId_;
    entityInfo_.u.session.reserved = config_.role;
    return SM_OK;
}

void SmemTransEntry::UnInitialize()
{
    // Perform a graceful group leave so that peer ranks can synchronously
    // clean up their imported state via OnRemoveFromWhitelist.
    // This must happen before the local entity is destroyed because the
    // leave callbacks on the peer side still reference entity_.
    if (joined_) {
        auto coreStore = store_->GetCoreStore();
        auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
        if (groupMgr != nullptr) {
            groupMgr->Leave();
        }
        joined_ = false;
    }

    {
        mf::WriteGuard locker(remoteSliceRwMutex_);
        remoteSlices_.clear();
        rankToWorkerId_.clear();
        ranksRole_.clear();
        nameToWorkerId_.clear();
    }

    hybm_export_info_free(&entityInfo_.hybmInfo);

    if (entity_ != nullptr) {
        hybm_destroy_entity(entity_, 0);
        entity_ = nullptr;
    }
}

int SmemTransEntry::SetupGroupManagerCallbacks() noexcept
{
    auto coreStore = store_->GetCoreStore();
    auto tcpStore = dynamic_cast<TcpConfigStore *>(coreStore.Get());
    if (tcpStore == nullptr) {
        SM_LOG_ERROR("SetupGroupManagerCallbacks failed, tcp store is nullptr");
        return SM_ERROR;
    }

    auto *asyncMgr = tcpStore->GetAsyncDispatcher();
    if (asyncMgr == nullptr) {
        SM_LOG_ERROR("SetupGroupManagerCallbacks: async dispatcher is null");
        return SM_ERROR;
    }

    if (auto ret = RegisterAsyncCallbacks(asyncMgr); ret != SMEM_OK) {
        return ret;
    }

    auto executor = SmMakeRef<SmemGroupManagerClient>();
    if (auto ret = RegisterExecutorCallbacks(executor.Get(), asyncMgr); ret != SMEM_OK) {
        return ret;
    }

    auto *groupMgr = static_cast<SmemGroupManager *>(tcpStore);
    groupMgr->SetExecutor(executor.Get());
    SM_LOG_INFO("SmemTransEntry callbacks setup, rank " << groupMgr->GetLocalRankId());
    return SMEM_OK;
}

int SmemTransEntry::RegisterAsyncCallbacks(SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept
{
    asyncMgr->SetWhitelistCallback([this](uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) {
        return OnAddToWhitelist(rankId, others, reqId);
    });
    asyncMgr->SetRemoveWhitelistCallback(
        [this](uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) {
            return OnRemoveFromWhitelist(rankId, others, reqId);
        });
    asyncMgr->SetConnectionCallback([this](uint32_t rankId, const std::vector<RankFullInfo> &peers, uint64_t reqId) {
        return OnEstablishConnection(rankId, peers, reqId);
    });
    asyncMgr->SetLinkStateQueryCallback([this]() { return OnQueryLinkState(); });
    asyncMgr->SetAddSlicesCallback([this](uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) {
        return OnAddSlices(extendingRankId, newSlices, reqId);
    });
    return SMEM_OK;
}

int SmemTransEntry::RegisterExecutorCallbacks(SmemGroupManagerClient *executor,
                                              SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept
{
    executor->onAddToWhitelist_ = [asyncMgr](uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) {
        std::vector<RankFullInfo> copy(others);
        asyncMgr->EnqueueAddToWhitelist(rankId, std::move(copy), reqId);
        return 0;
    };
    executor->onRemoveFromWhitelist_ = [asyncMgr](uint32_t rankId, const std::vector<RankFullInfo> &others,
                                                  uint64_t reqId) {
        std::vector<RankFullInfo> copy(others);
        asyncMgr->EnqueueRemoveFromWhitelist(rankId, std::move(copy), reqId);
        return 0;
    };
    executor->onEstablishConnection_ = [asyncMgr](uint32_t rankId, const std::vector<RankFullInfo> &peers,
                                                  uint64_t reqId) {
        std::vector<RankFullInfo> copy(peers);
        asyncMgr->EnqueueEstablishConnection(rankId, std::move(copy), reqId);
        return 0;
    };
    executor->onQueryLinkState_ = [asyncMgr]() {
        asyncMgr->EnqueueQueryLinkState(0);
        return std::vector<LinkStateEntry>{};
    };
    executor->onPromoteToActive_ = [this](uint32_t rankId) {
        joined_ = true;
        SM_LOG_INFO("SmemTransEntry::onPromoteToActive rankId=" << rankId);
        try {
            joinComplete_->set_value();
        } catch (const std::future_error &e) {
            SM_LOG_WARN("onPromoteToActive: promise already satisfied: " << e.what());
        }
        return 0;
    };
    executor->onAddSlices_ = [asyncMgr](uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) {
        MultiBytes copy(newSlices);
        asyncMgr->EnqueueAddSlices(extendingRankId, std::move(copy), reqId);
        return 0;
    };
    return SMEM_OK;
}

static void SerializeTransExchangeInfo(std::vector<uint8_t> &out, const SmemTransExchangeInfo &info) noexcept
{
    const auto *uPtr = reinterpret_cast<const uint8_t *>(&info.u);
    out.insert(out.end(), uPtr, uPtr + sizeof(info.u));
    const auto *lenPtr = reinterpret_cast<const uint8_t *>(&info.hybmInfo.descLen);
    out.insert(out.end(), lenPtr, lenPtr + sizeof(info.hybmInfo.descLen));
    if (info.hybmInfo.descLen > 0 && info.hybmInfo.desc != nullptr) {
        out.insert(out.end(), info.hybmInfo.desc, info.hybmInfo.desc + info.hybmInfo.descLen);
    }
}

static size_t DeserializeTransExchangeInfo(const uint8_t *buf, size_t bufLen, SmemTransExchangeInfo &info) noexcept
{
    constexpr size_t lenFieldSize = sizeof(uint32_t);
    const size_t uSize = sizeof(SmemTransExchangeInfo::U);
    const size_t headerSize = uSize + lenFieldSize;
    if (buf == nullptr || bufLen < headerSize) {
        return 0;
    }
    uint32_t descLen = 0;
    std::copy_n(buf + uSize, lenFieldSize, reinterpret_cast<uint8_t *>(&descLen));
    const size_t need = headerSize + descLen;
    if (bufLen < need) {
        return 0;
    }
    SmemTransExchangeInfo parsed{};
    std::copy_n(buf, uSize, reinterpret_cast<uint8_t *>(&parsed.u));
    parsed.hybmInfo.descLen = descLen;
    if (descLen > 0) {
        parsed.hybmInfo.desc = new (std::nothrow) uint8_t[descLen];
        if (parsed.hybmInfo.desc == nullptr) {
            return 0;
        }
        std::copy_n(buf + headerSize, descLen, parsed.hybmInfo.desc);
    }
    hybm_export_info_free(&info.hybmInfo);
    info = parsed;
    return need;
}

int SmemTransEntry::ImportPeerSlicesToMap(const RankFullInfo &other, std::vector<void *> &global,
                                          std::vector<SmemTransExchangeInfo> &slices) noexcept
{
    global.assign(slices.size(), nullptr);
    std::vector<hybm_exchange_info> hybmInfos;
    hybmInfos.reserve(slices.size());
    for (const auto &s : slices) {
        hybmInfos.push_back(s.hybmInfo);
    }
    auto ret = hybm_import(entity_, hybmInfos.data(), hybmInfos.size(), global.data(), 0);
    for (auto &s : slices) {
        hybm_export_info_free(&s.hybmInfo);
    }
    if (ret != BM_OK) {
        SM_LOG_ERROR("ImportPeerSlicesToMap: hybm_import failed, rank=" << other.rankId << " ret=" << ret);
        return SMEM_ERROR;
    }
    return BM_OK;
}

int SmemTransEntry::OnAddToWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) noexcept
{
    if (others.empty()) {
        SM_LOG_DEBUG("OnAddToWhitelist rankId=" << rankId << " others empty, skip");
        return SMEM_OK;
    }

    std::vector<SmemTransExchangeInfo> entities(others.size());
    for (auto i = 0U; i < others.size(); ++i) {
        if (DeserializeTransExchangeInfo(others[i].baseInfo.data(), others[i].baseInfo.size(), entities[i]) == 0) {
            for (auto j = 0U; j < i; ++j) {
                hybm_export_info_free(&entities[j].hybmInfo);
            }
            SM_LOG_ERROR("OnAddToWhitelist: invalid baseInfo size for rank " << others[i].rankId);
            return SMEM_ERROR;
        }
    }
    std::vector<hybm_exchange_info> entityInfos;
    entityInfos.reserve(entities.size());
    for (const auto &e : entities) {
        entityInfos.push_back(e.hybmInfo);
    }
    auto ret = hybm_import(entity_, entityInfos.data(), entityInfos.size(), nullptr, HYBM_FLAG_EXPORT_ENTITY);
    for (auto &e : entities) {
        hybm_export_info_free(&e.hybmInfo);
    }
    if (ret != BM_OK) {
        SM_LOG_ERROR("failed to import entity: " << ret);
        return SMEM_ERROR;
    }

    // Record per-peer rank->workerId mapping and import remote slices so that
    // BatchSyncTransfer can resolve remote addresses later. The mapping must be
    // recorded even when the peer has no slices yet: slices registered after
    // Join arrive later via OnAddSlices, which looks up rankToWorkerId_.
    for (auto i = 0U; i < others.size(); ++i) {
        smem_trans_role_t role = static_cast<smem_trans_role_t>(entities[i].u.session.reserved);
        entities[i].u.session.reserved = 0U; // reserved must be cleared so the WorkerId matches ParseNameToUniqueId
        WorkerIdUnion wu(entities[i].u.session);
        WorkerId id = wu.workerId;

        std::vector<void *> global;
        std::vector<LocalMapAddress> local;
        if (!others[i].externalInfo.empty()) {
            std::vector<SmemTransExchangeInfo> slices;
            if (auto ret = ParsePeerSlices(others[i], slices, local); ret != BM_OK) {
                continue;
            }
            if (auto ret = ImportPeerSlicesToMap(others[i], global, slices); ret != BM_OK) {
                return SMEM_ERROR;
            }
        }
        AddRemoteInfo(others[i].rankId, role, id, global, local);
    }

    SM_LOG_DEBUG("OnAddToWhitelist done, rank=" << rankId << " peers=" << others.size());
    return SMEM_OK;
}

int SmemTransEntry::OnRemoveFromWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others,
                                          uint64_t reqId) noexcept
{
    for (const auto &other : others) {
        auto ret = hybm_remove_imported(entity_, other.rankId, 0);
        if (ret != SMEM_OK) {
            SM_LOG_WARN("OnRemoveFromWhitelist remove rank " << other.rankId << " failed: " << ret);
        }
        NotifyPeerDown(other.rankId);
    }
    SM_LOG_DEBUG("OnRemoveFromWhitelist done, rank=" << rankId << " peers=" << others.size());
    return SMEM_OK;
}

void SmemTransEntry::NotifyPeerDown(uint32_t leavingRankId) noexcept
{
    if (peerDownCallback_ != nullptr) {
        auto it = rankToWorkerId_.find(leavingRankId);
        if (it != rankToWorkerId_.end()) {
            WorkerIdUnion workerId{it->second};
            WorkerUniqueId &w = workerId.session;

            char ipBuf[INET6_ADDRSTRLEN] = {0};
            if (w.address.type == ock::mf::IpV4) {
                struct in_addr addr;
                addr.s_addr = htonl(w.address.ip.ipv4.s_addr);
                inet_ntop(AF_INET, &addr, ipBuf, sizeof(ipBuf));
            } else if (w.address.type == ock::mf::IpV6) {
                inet_ntop(AF_INET6, &w.address.ip.ipv6, ipBuf, sizeof(ipBuf));
            }
            std::string peerAddr = std::string(ipBuf) + ":" + std::to_string(w.port);
            SM_LOG_INFO("invoking peer down callback for rank " << leavingRankId << " addr " << peerAddr);
            peerDownCallback_(peerAddr.c_str(), peerDownUserData_);
        }
    }
}

int SmemTransEntry::OnEstablishConnection(uint32_t rankId, const std::vector<RankFullInfo> &peers,
                                          uint64_t reqId) noexcept
{
    if (peers.empty()) {
        SM_LOG_DEBUG("OnEstablishConnection rankId=" << rankId << " peers empty, skip");
        return SMEM_OK;
    }

    for (const auto &p : peers) {
        EstablishPeerConnection(p);
    }

    hybm_mmap(entity_, 0);

    std::vector<uint32_t> rankIds;
    rankIds.reserve(peers.size());
    for (const auto &p : peers)
        rankIds.push_back(p.rankId);
    auto ret = hybm_transport_connect(entity_, rankIds.data(), rankIds.size(), 0);
    if (ret != SMEM_OK) {
        SM_LOG_ERROR("OnEstablishConnection rankId=" << rankId << " hybm_transport_connect failed: " << ret);
        return ret;
    }

    SM_LOG_INFO("OnEstablishConnection rankId=" << rankId << " peers.size=" << peers.size());
    return SMEM_OK;
}

void SmemTransEntry::EstablishPeerConnection(const RankFullInfo &peer) noexcept
{
    if (peer.externalInfo.empty()) {
        return;
    }

    // Parse the peer session from baseInfo so the WorkerId key matches
    // ParseNameToUniqueId (same convention as OnAddToWhitelist).
    SmemTransExchangeInfo entityInfo{};
    smem_trans_role_t role = SMEM_TRANS_NONE;
    WorkerId id;
    if (!ParseExchangeInfo(peer, entityInfo, role, id)) {
        return;
    }

    std::vector<SmemTransExchangeInfo> slices;
    std::vector<void *> global;
    std::vector<LocalMapAddress> local;
    if (auto ret = ParsePeerSlices(peer, slices, local); ret != BM_OK) {
        SM_LOG_ERROR("parse slice from ESTABLISH failed, rank=" << peer.rankId << " ret=" << ret);
        return;
    }
    global.resize(peer.externalInfo.size(), nullptr);

    // Dedup: slices may already have arrived via the ADD_SLICES broadcast
    // (OnAddSlices) when the extension happened after this rank went active.
    bool needImport = false;
    {
        mf::ReadGuard guard(remoteSliceRwMutex_);
        auto it = remoteSlices_.find(id);
        if (it == remoteSlices_.end()) {
            needImport = true;
        } else {
            for (const auto &l : local) {
                if (it->second.find(l.address) == it->second.end()) {
                    needImport = true;
                    break;
                }
            }
        }
    }
    if (needImport) {
        std::vector<hybm_exchange_info> hybmInfos;
        hybmInfos.reserve(slices.size());
        for (const auto &s : slices) {
            hybmInfos.push_back(s.hybmInfo);
        }
        if (auto ret = hybm_import(entity_, hybmInfos.data(), hybmInfos.size(), global.data(), 0); ret != BM_OK) {
            SM_LOG_ERROR("import slice from ESTABLISH failed, rank=" << peer.rankId << " ret=" << ret);
            for (auto &s : slices) {
                hybm_export_info_free(&s.hybmInfo);
            }
            return;
        }
    }
    for (auto &s : slices) {
        hybm_export_info_free(&s.hybmInfo);
    }

    mf::WriteGuard locker(remoteSliceRwMutex_);
    rankToWorkerId_[peer.rankId] = id;
    ranksRole_[peer.rankId] = role;
    if (role == config_.role) { // same role, skip record
        return;
    }
    for (auto i = 0U; i < local.size(); ++i) {
        remoteSlices_[id].emplace(local[i].address, LocalMapAddress(global[i], local[i].size));
    }
}

std::vector<ock::smem::LinkStateEntry> SmemTransEntry::OnQueryLinkState() noexcept
{
    std::vector<ock::smem::LinkStateEntry> entries;
    if (entity_ == nullptr) {
        return entries;
    }
    mf::ReadGuard locker(remoteSliceRwMutex_);
    for (const auto &[peerRank, workerId] : rankToWorkerId_) {
        hybm_data_op_type reachTypes = static_cast<hybm_data_op_type>(0);
        auto ret = hybm_entity_reach_types(entity_, peerRank, reachTypes, 0);
        LinkStateEntry entry;
        entry.dstRankId = peerRank;
        entry.state = (ret == 0 && reachTypes != 0) ? TRANS_LINK_CONNECTED : TRANS_LINK_IDLE;
        entries.push_back(entry);
    }
    SM_LOG_DEBUG("OnQueryLinkState: " << entries.size() << " peers");
    return entries;
}

int SmemTransEntry::OnAddSlices(uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) noexcept
{
    (void)reqId;
    if (newSlices.empty()) {
        return SMEM_OK;
    }
    std::vector<SmemTransExchangeInfo> infos;
    infos.reserve(newSlices.size());
    std::vector<hybm_exchange_info> hybmInfos;
    hybmInfos.reserve(newSlices.size());
    std::vector<void *> globalAddrs;
    globalAddrs.reserve(newSlices.size());
    for (const auto &slice : newSlices) {
        SmemTransExchangeInfo info{};
        if (DeserializeTransExchangeInfo(slice.data(), slice.size(), info) == 0) {
            SM_LOG_ERROR("OnAddSlices: invalid slice payload for rank " << extendingRankId);
            for (auto &parsed : hybmInfos) {
                hybm_export_info_free(&parsed);
            }
            return SMEM_ERROR;
        }
        infos.push_back(info);
        hybmInfos.push_back(info.hybmInfo);
        globalAddrs.push_back(nullptr);
    }
    auto ret = hybm_import(entity_, hybmInfos.data(), hybmInfos.size(), globalAddrs.data(), 0);
    for (auto &parsed : hybmInfos) {
        hybm_export_info_free(&parsed);
    }
    if (ret != BM_OK) {
        SM_LOG_ERROR("OnAddSlices: hybm_import failed, extendingRank=" << extendingRankId << " ret=" << ret);
        return SMEM_ERROR;
    }
    auto it = rankToWorkerId_.find(extendingRankId);
    if (it != rankToWorkerId_.end()) {
        mf::WriteGuard locker(remoteSliceRwMutex_);
        for (size_t i = 0; i < infos.size(); ++i) {
            remoteSlices_[it->second].emplace(infos[i].u.address.address,
                                              LocalMapAddress{globalAddrs[i], infos[i].u.address.size});
        }
    } else {
        SM_LOG_ERROR("OnAddSlices: rank " << extendingRankId << " not found in rankToWorkerId_");
    }
    hybm_mmap(entity_, 0);
    SM_LOG_INFO("OnAddSlices extendingRank=" << extendingRankId << " slices=" << newSlices.size());
    return SMEM_OK;
}

static std::string uniqueToString(const WorkerId &unique)
{
    std::ostringstream oss;
    constexpr int WIDTH = 2;
    for (size_t i = 0; i < unique.size(); ++i) {
        oss << std::hex << std::setw(WIDTH) << std::setfill('0') << static_cast<int>(unique[i]);
        if (i < unique.size() - 1) {
            oss << ":";
        }
    }
    return oss.str();
}

void SmemTransEntry::AddRemoteInfo(uint32_t rk, smem_trans_role_t role, WorkerId &id, std::vector<void *> &global,
                                   std::vector<LocalMapAddress> &local)
{
    mf::WriteGuard locker(remoteSliceRwMutex_);
    rankToWorkerId_[rk] = id;
    ranksRole_[rk] = role;

    if (role == config_.role) { // same role, skip record
        return;
    }
    for (auto i = 0U; i < global.size(); i++) {
        remoteSlices_[id].emplace(local[i].address, LocalMapAddress(global[i], local[i].size));
        SM_LOG_DEBUG("record mem, local_rk:" << rankId_ << " remote_rk:" << rk << " global_addr:0x" << global[i]
                                             << " remote_addr:0x" << local[i].address);
    }
}

smem_trans_role_t SmemTransEntry::QueryRole(uint32_t rk)
{
    mf::ReadGuard locker(remoteSliceRwMutex_);
    auto it = ranksRole_.find(rk);
    if (it == ranksRole_.end()) {
        SM_LOG_ERROR("not found this rank:" << rk << " in ranksRole_");
        return SMEM_TRANS_BUTT;
    }
    return it->second;
}

void SmemTransEntry::AddRemoteInfo(uint32_t rk, std::vector<void *> &global, std::vector<LocalMapAddress> &local)
{
    mf::WriteGuard locker(remoteSliceRwMutex_);
    auto it = rankToWorkerId_.find(rk);
    if (it == rankToWorkerId_.end()) {
        SM_LOG_ERROR("not found this rank:" << rk << " in rankToWorkerId_");
        return;
    }

    WorkerId id = it->second;
    for (auto i = 0U; i < global.size(); i++) {
        remoteSlices_[id].emplace(local[i].address, LocalMapAddress(global[i], local[i].size));
        SM_LOG_DEBUG("record mem, local_rk:" << rankId_ << " remote_rk:" << rk << " global_addr:0x" << global[i]
                                             << " remote_addr:0x" << local[i].address);
    }
}

void SmemTransEntry::SetPeerDownCallback(smem_trans_peer_down_callback_t callback, void *userData)
{
    peerDownCallback_ = callback;
    peerDownUserData_ = userData;
}

Result SmemTransEntry::Join(uint32_t flags)
{
    auto coreStore = store_->GetCoreStore();
    auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
    if (groupMgr == nullptr) {
        SM_LOG_ERROR("SmemTransEntry::Join failed, group manager is nullptr");
        return SM_ERROR;
    }

    joinComplete_ = std::make_shared<std::promise<void>>();

    RankFullInfo info{groupMgr->GetLocalRankId()};
    info.protocol = SMEM_RANK_PROTOCOL_TRANS;
    SerializeTransExchangeInfo(info.baseInfo, entityInfo_);
    for (const auto &regInfo : registedInfo_) {
        Bytes bytes;
        SerializeTransExchangeInfo(bytes, regInfo);
        info.externalInfo.emplace_back(std::move(bytes));
    }

    if (auto joinRet = groupMgr->Join(info); joinRet != SM_OK) {
        SM_LOG_ERROR("SmemTransEntry::Join failed, result: " << joinRet);
        return SM_ERROR;
    }

    const uint32_t groupJoinTimeoutSec =
        mf::MfEnvUtil::GetOptionalUintOrDefault(mf::env::MF_GROUP_JOIN_MAX_TIMEOUT, MF_GROUP_JOIN_DEFAULT_TIMEOUT);
    auto future = joinComplete_->get_future();
    if (future.wait_for(std::chrono::seconds(groupJoinTimeoutSec)) != std::future_status::ready) {
        SM_LOG_ERROR("SmemTransEntry::Join timeout waiting for PROMOTE_TO_ACTIVE, rank=" << rankId_);
        groupMgr->Leave();
        return SM_ERROR;
    }

    SM_LOG_DEBUG("join success. rank: " << rankId_);
    return SM_OK;
}

Result SmemTransEntry::Update(uint32_t flags)
{
    (void)flags;
    SM_LOG_WARN("SmemTransEntry::Update is deprecated — "
                "dynamic memory registration propagation must use "
                "RegisterLocalMemories which propagates via the server. "
                "Calling Update directly is a no-op. rankId="
                << rankId_);
    return SM_OK;
}

Result SmemTransEntry::Leave(uint32_t flags)
{
    auto coreStore = store_->GetCoreStore();
    auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
    if (groupMgr == nullptr) {
        SM_LOG_ERROR("SmemTransEntry::Leave failed, group manager is nullptr");
        return SM_ERROR;
    }

    if (auto leaveRet = groupMgr->Leave(); leaveRet != SM_OK) {
        SM_LOG_ERROR("rankId:" << rankId_ << " SmemTransEntry::Leave failed, result: " << leaveRet);
        return leaveRet;
    }

    SM_LOG_INFO("rankId:" << rankId_ << " SmemTransEntry::Leave success.");
    return SM_OK;
}

Result SmemTransEntry::RegisterLocalMemory(const void *address, uint64_t size, uint32_t flags)
{
    std::vector<std::pair<const void *, size_t>> regMemories;
    regMemories.emplace_back(address, size);
    return RegisterLocalMemories(regMemories, flags);
}

Result SmemTransEntry::RegisterLocalMemories(const std::vector<std::pair<const void *, size_t>> &regMemories,
                                             uint32_t flags)
{
    if (entity_ == nullptr) {
        SM_LOG_ERROR("not create entity.");
        return SM_ERROR;
    }

    if (regMemories.empty()) {
        return SM_OK;
    }

    for (auto it : regMemories) {
        if (it.first == nullptr || it.second == 0) {
            SM_LOG_ERROR("input address or size invalid, address: " << it.first << " size: " << it.second);
            return SM_INVALID_PARAM;
        }
    }

    auto alignedMemories = regMemories;
    for (auto &it : alignedMemories) {
        AlignMemory(it.first, it.second);
    }
    auto mm = CombineMemories(alignedMemories);
    std::unique_lock<std::mutex> uniqueLock{memMutex_};
    for (auto &m : mm) {
        auto ret = RegisterOneMemory(m.first, m.second, flags);
        if (ret != 0) {
            for (auto &info : registedInfo_) {
                hybm_export_info_free(&info.hybmInfo);
            }
            registedInfo_.clear();
            return ret;
        }
    }
    // Pack and send new slices to server for propagation to all peers
    MultiBytes newSlices;
    for (const auto &info : registedInfo_) {
        Bytes bytes;
        SerializeTransExchangeInfo(bytes, info);
        newSlices.emplace_back(std::move(bytes));
    }

    auto coreStore = store_->GetCoreStore();
    auto groupMgr = dynamic_cast<SmemGroupManager *>(coreStore.Get());
    if (groupMgr == nullptr) {
        SM_LOG_ERROR("RegisterLocalMemories: group manager is nullptr");
        for (auto &info : registedInfo_) {
            hybm_export_info_free(&info.hybmInfo);
        }
        registedInfo_.clear();
        return SM_ERROR;
    }

    if (auto ret = groupMgr->ExtendMemory(newSlices); ret != SM_OK) {
        SM_LOG_ERROR("RegisterLocalMemories: ExtendMemory failed, ret=" << ret);
        for (auto &info : registedInfo_) {
            hybm_export_info_free(&info.hybmInfo);
        }
        registedInfo_.clear();
        return SM_ERROR;
    }

    for (auto &info : registedInfo_) {
        hybm_export_info_free(&info.hybmInfo);
    }
    registedInfo_.clear();
    return SM_OK;
}

Result SmemTransEntry::SyncTransfer(void *localAddr, const std::string &remoteUniqueId, void *remoteAddr,
                                    size_t dataSize, smem_bm_copy_type opcode, void *stream, uint32_t flags)
{
    return BatchSyncTransfer(&localAddr, remoteUniqueId, &remoteAddr, &dataSize, 1U, opcode, stream, flags);
}

Result SmemTransEntry::TransformAddr(Local2GlobalMap &maps, std::vector<void *> &addr, void *remoteAddrs[],
                                     const size_t dataSizes[], uint32_t size)
{
    for (auto i = 0U; i < size; i++) {
        if (remoteAddrs[i] == nullptr) {
            addr[i] = nullptr;
            continue;
        }

        auto pos = maps.lower_bound(remoteAddrs[i]);
        if (pos == maps.end()) {
            SM_LOG_ERROR("remote address[" << i << "] " << remoteAddrs[i] << " is invalid.");
            return SM_INVALID_PARAM;
        }

        if (dataSizes != nullptr &&
            (const uint8_t *)remoteAddrs[i] + dataSizes[i] > (const uint8_t *)(pos->first) + pos->second.size) {
            SM_LOG_ERROR("address[" << i << "], size[" << i << "]=" << dataSizes[i] << " out of range.");
            return SM_INVALID_PARAM;
        }

        addr[i] = (uint8_t *)pos->second.address + ((const uint8_t *)remoteAddrs[i] - (const uint8_t *)(pos->first));
    }
    return SM_OK;
}

Result SmemTransEntry::BatchSyncTransfer(void *localAddrs[], const std::string &remoteUniqueId, void *remoteAddrs[],
                                         const size_t dataSizes[], uint32_t batchSize, smem_bm_copy_type opcode,
                                         void *stream, uint32_t flags)
{
    SM_VALIDATE_RETURN(localAddrs != nullptr, "invalid localAddrs, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(remoteAddrs != nullptr, "invalid remoteAddrs, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(dataSizes != nullptr, "invalid dataSizes, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(batchSize != 0, "invalid batchSize, which is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(flags == 0 || flags == COPY_EXTEND_FLAG, "invalid flags", SM_INVALID_PARAM);
    for (auto i = 0U; i < batchSize; i++) {
        SM_VALIDATE_RETURN(localAddrs[i] != nullptr, "localAddrs, which is null", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(remoteAddrs[i] != nullptr, "remoteAddrs, which is null", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(dataSizes[i] != 0, "invalid dataSizes, which is 0", SM_INVALID_PARAM);
    }
    WorkerId unique;
    auto ret = ParseNameToUniqueId(remoteUniqueId, unique);
    if (ret != 0) {
        return ret;
    }

    std::vector<void *> mappedAddress(batchSize);

    mf::ReadGuard locker(remoteSliceRwMutex_);
    auto it = remoteSlices_.find(unique);
    if (it == remoteSlices_.end()) {
        SM_LOG_ERROR("session:(" << remoteUniqueId << ")(" << uniqueToString(unique) << ") not found.");
        return SM_INVALID_PARAM;
    }

    ret = TransformAddr(it->second, mappedAddress, remoteAddrs, dataSizes, batchSize);
    SM_ASSERT_RETURN_NOLOG(ret == SM_OK, ret);

    uint32_t flag = flags | ((stream != nullptr) ? ASYNC_COPY_FLAG : 0);
    switch (opcode) {
        case SMEMB_COPY_L2G: {
            hybm_batch_copy_params copyParams{};
            copyParams.sources = localAddrs;
            copyParams.destinations = mappedAddress.data();
            copyParams.dataSizes = dataSizes;
            copyParams.batchSize = batchSize;
            ret = hybm_data_batch_copy(entity_, &copyParams, HYBM_DATA_COPY_DIRECTION_AUTO, stream, flag);
        } break;
        case SMEMB_COPY_G2L: {
            hybm_batch_copy_params copyParams{};
            copyParams.sources = mappedAddress.data();
            copyParams.destinations = localAddrs;
            copyParams.dataSizes = dataSizes;
            copyParams.batchSize = batchSize;
            ret = hybm_data_batch_copy(entity_, &copyParams, HYBM_DATA_COPY_DIRECTION_AUTO, stream, flag);
        } break;
        default:
            SM_LOG_ERROR("unexpect copy type[" << opcode << "] is invalid.");
            return SM_INVALID_PARAM;
    }
    if (ret != 0) {
        SM_LOG_ERROR("batch copy data failed:" << ret);
    }
    return ret;
}

Result SmemTransEntry::BatchQuantTransfer(smem_trans_quant_copy_param_t *params, smem_bm_copy_type opcode)
{
    SM_VALIDATE_RETURN(params->localAddrs != nullptr, "invalid localAddrs, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(params->remoteAddrs != nullptr, "invalid remoteAddrs, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(params->dataSizes != nullptr, "invalid dataSizes, which is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(params->batchSize != 0, "invalid batchSize, which is 0", SM_INVALID_PARAM);
    for (auto i = 0U; i < params->batchSize; i++) {
        SM_VALIDATE_RETURN(params->localAddrs[i] != nullptr, "localAddrs, which is null", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(params->remoteAddrs[i] != nullptr, "remoteAddrs, which is null", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(params->dataSizes[i] != 0, "invalid dataSizes, which is 0", SM_INVALID_PARAM);
    }
    WorkerId unique;
    auto ret = ParseNameToUniqueId(params->remoteUniqueId, unique);
    if (ret != 0) {
        return ret;
    }

    std::vector<void *> mappedAddress(params->batchSize);
    std::vector<void *> scaleAddress(params->batchSize);
    std::vector<void *> offsetAddress(params->batchSize);

    mf::ReadGuard locker(remoteSliceRwMutex_);
    auto it = remoteSlices_.find(unique);
    if (it == remoteSlices_.end()) {
        SM_LOG_ERROR("session:(" << params->remoteUniqueId << ")(" << uniqueToString(unique) << ") not found.");
        return SM_INVALID_PARAM;
    }

    ret = TransformAddr(it->second, mappedAddress, params->remoteAddrs, params->dataSizes, params->batchSize);
    SM_ASSERT_RETURN_NOLOG(ret == SM_OK, ret);
    ret = TransformAddr(it->second, scaleAddress, reinterpret_cast<void **>(params->scale), nullptr, params->batchSize);
    SM_ASSERT_RETURN_NOLOG(ret == SM_OK, ret);
    ret =
        TransformAddr(it->second, offsetAddress, reinterpret_cast<void **>(params->offset), nullptr, params->batchSize);
    SM_ASSERT_RETURN_NOLOG(ret == SM_OK, ret);

    uint32_t flag = ((params->stream != nullptr) ? ASYNC_COPY_FLAG : 0);
    switch (opcode) {
        case SMEMB_COPY_L2G: {
            hybm_quant_copy_params copyParams = {
                params->localAddrs, mappedAddress.data(), params->dataSizes, scaleAddress.data(), offsetAddress.data(),
                params->batchSize,  params->unitNum,      params->stream,    params->inputType,   flag};
            ret = hybm_data_quant_copy(entity_, &copyParams);
        } break;
        case SMEMB_COPY_G2L:
        default:
            SM_LOG_ERROR("unexpect copy type[" << opcode << "] is invalid.");
            return SM_INVALID_PARAM;
    }
    if (ret != 0) {
        SM_LOG_ERROR("batch quant copy data failed:" << ret);
    }
    return ret;
}

bool SmemTransEntry::ParseTransName(const std::string &name, ock::mf::net_addr_t &ip, uint16_t &port)
{
    UrlExtraction extraction;
    int ret = extraction.ExtractIpPortFromUrl(std::string("tcp://").append(name));
    if (ret != 0) {
        SM_LOG_ERROR("parse name failed, name=" << name << ", ret=" << ret);
        return false;
    }

    struct in6_addr addr6;
    if (inet_pton(AF_INET6, extraction.ip.c_str(), &addr6) == 1) {
        ip.ip.ipv6 = addr6;
        ip.type = ock::mf::IpV6;
    } else {
        struct in_addr addr4;
        if (inet_pton(AF_INET, extraction.ip.c_str(), &addr4) != 1) {
            SM_LOG_ERROR("Invalid IP address format: " << extraction.ip);
            return false;
        }
        ip.ip.ipv4.s_addr = ntohl(addr4.s_addr);
        ip.type = ock::mf::IpV4;
    }
    port = extraction.port;
    return true;
}

void SmemTransEntry::RemoveRanks(std::vector<uint32_t> &rankSet)
{
    mf::WriteGuard locker(remoteSliceRwMutex_);
    for (auto rankId : rankSet) {
        auto it = rankToWorkerId_.find(rankId);
        if (it == rankToWorkerId_.end()) {
            SM_LOG_INFO("not found this rank:" << rankId);
            continue;
        }

        WorkerId id = it->second;
        rankToWorkerId_.erase(rankId);
        remoteSlices_.erase(id);
        ranksRole_.erase(rankId);

        auto ret = hybm_remove_imported(entity_, rankId, 0);
        if (ret != 0) {
            SM_LOG_ERROR("remove rank:" << rankId << " failed: " << ret);
        }
    }
}

Result SmemTransEntry::ParseNameToUniqueId(const std::string &name, WorkerId &uniqueId)
{
    WorkerUniqueId workerUniqueId;
    auto it = nameToWorkerId_.find(name);
    if (it != nameToWorkerId_.end()) {
        /* fast path */
        uniqueId = it->second;
        return SM_OK;
    }
    auto success = ParseTransName(name, workerUniqueId.address, workerUniqueId.port);
    if (!success) {
        SM_LOG_ERROR("parse name failed, name: " << name);
        return SM_INVALID_PARAM;
    }

    WorkerIdUnion workerId{workerUniqueId};
    uniqueId = workerId.workerId;
    nameToWorkerId_.emplace(name, workerId.workerId);
    return SM_OK;
}

void SmemTransEntry::AlignMemory(const void *&address, uint64_t &size)
{
    constexpr auto NPU_PAGE_SIZE = 2UL * 1024UL * 1024UL;
    constexpr auto NPU_PAGE_MASK = ~(NPU_PAGE_SIZE - 1UL);

    auto pointer = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address));
    auto alignPtr = (pointer & NPU_PAGE_MASK);
    auto diff = pointer - alignPtr;
    size += diff;
    size = ((size + NPU_PAGE_SIZE - 1) & NPU_PAGE_MASK);
    address = reinterpret_cast<const void *>(alignPtr);
}

std::vector<std::pair<const void *, size_t>>
SmemTransEntry::CombineMemories(std::vector<std::pair<const void *, size_t>> &input)
{
    std::sort(input.begin(), input.end());
    std::vector<std::pair<const void *, size_t>> result;
    auto current = input[0];
    for (auto i = 1U; i < input.size(); i++) {
        // Merge only on real overlap (end > next.start); exact adjacency (end == next.start) is NOT
        // merged, to avoid cross-heap merging whose size exceeds a single heap's capacity and gets
        if ((const uint8_t *)current.first + current.second > (const uint8_t *)input[i].first) {
            ptrdiff_t diff = ((const uint8_t *)input[i].first - (const uint8_t *)current.first);
            if (static_cast<size_t>(diff) > std::numeric_limits<size_t>::max() - input[i].second) {
                result.emplace_back(current);
                current = input[i];
                continue;
            }
            current.second = std::max(current.second, diff + input[i].second);
        } else {
            result.emplace_back(current);
            current = input[i];
        }
    }
    result.emplace_back(current);
    return result;
}

Result SmemTransEntry::RegisterOneMemory(const void *address, uint64_t size, uint32_t flags)
{
    auto slice = hybm_register_local_memory(entity_, address, size, 0);
    if (slice == nullptr) {
        SM_LOG_ERROR("hybm_register_local_memory failed, address: " << address << " size: " << size);
        return SM_ERROR;
    }
    // 区间内重复注册由 segment 层 fake 掉（返回首次注册的同一 slice ID），
    // 此处按 ID 去重：已 export 过的 slice 不再生成/广播交换记录，避免对端出现
    // "整段 handle + 子块 u.address" 的错配记录
    const auto sliceKey = reinterpret_cast<uint64_t>(slice);
    if (!exportedSliceIds_.insert(sliceKey).second) {
        SM_LOG_INFO("fake register, skip duplicated export: address: " << address << " size: " << size);
        return SM_OK;
    }
    // fake/区间扩展后 slice 登记的范围可能与本次调用入参不同，
    // u.address 必须用 slice 的真实范围，保证与广播的 handle 覆盖一致，
    // 否则对端 TransformAddr 会因记录范围不含目标地址而报 out of range
    void *sliceVa = hybm_get_slice_va(entity_, slice);
    uint64_t sliceSize = hybm_get_slice_size(entity_, slice);
    if (sliceVa == nullptr || sliceSize == 0) {
        SM_LOG_ERROR("query slice range failed, address: " << address << " size: " << size);
        hybm_free_local_memory(entity_, slice, size, 0);
        return SM_ERROR;
    }
    SM_LOG_DEBUG("register memory(address with size=" << size << ") return slice=" << slice << " sliceVa=" << sliceVa
                                                      << " sliceSize=" << sliceSize);

    SmemTransExchangeInfo info{};
    auto ret = hybm_export(entity_, slice, 0, &info.hybmInfo);
    if (ret != 0) {
        SM_LOG_ERROR("export slice for register address with size: " << size << " failed:" << ret);
        hybm_free_local_memory(entity_, slice, size, 0);
        exportedSliceIds_.erase(sliceKey);
        hybm_export_info_free(&info.hybmInfo);
        return SM_ERROR;
    }

    info.u.address = LocalMapAddress(sliceVa, sliceSize);
    registedInfo_.emplace_back(info);
    return SM_OK;
}

hybm_options SmemTransEntry::GenerateHybmOptions()
{
    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.rankCount = SMEM_TRANS_RANK_COUNT_MAX;
    options.rankId = rankId_;
    options.devId = config_.deviceId;
    options.scene = HYBM_SCENE_TRANS;
    options.role = config_.role == SMEM_TRANS_SENDER ? HYBM_ROLE_SENDER : HYBM_ROLE_RECEIVER;
    options.dramShmFd = -1;
    options.enable56BitsGva = true; // trans enabled
    bzero(options.tag, sizeof(options.tag));
    bzero(options.tagOpInfo, sizeof(options.tagOpInfo));

    const bool isHostRdma = (config_.dataOpType & SMEMB_DATA_OP_HOST_RDMA) != 0U;
    options.memType = static_cast<hybm_mem_type>(HYBM_MEM_TYPE_DEVICE);
    options.maxHBMSize = TRANS_RESERVE_HBM_VA_SIZE;
    options.maxDRAMSize = 0;
    options.hostVASpace = 0;
    options.deviceVASpace = 0;

    bzero(options.transUrl, sizeof(options.transUrl));
    if (isHostRdma && config_.nic[0] != '\0') {
        constexpr size_t NIC_SIZE = sizeof(options.transUrl);
        size_t urlLen = strnlen(config_.nic, sizeof(config_.nic));
        size_t maxChars = std::min(urlLen, NIC_SIZE - 1);
        std::copy_n(config_.nic, maxChars, options.transUrl);
    } else {
        uint16_t port = 11000 + entityId_;
        auto url = "tcp://127.0.0.1:" + std::to_string(port);
        constexpr size_t NIC_SIZE = sizeof(options.transUrl);
        size_t maxChars = std::min(url.length(), NIC_SIZE - 1);
        std::copy_n(url.c_str(), maxChars, options.transUrl);
    }

    options.tlsOption.tlsEnable = config_.hcomTlsConfig.tlsEnable;
    std::copy_n(config_.hcomTlsConfig.caPath, SMEM_TLS_PATH_SIZE, options.tlsOption.caPath);
    std::copy_n(config_.hcomTlsConfig.crlPath, SMEM_TLS_PATH_SIZE, options.tlsOption.crlPath);
    std::copy_n(config_.hcomTlsConfig.certPath, SMEM_TLS_PATH_SIZE, options.tlsOption.certPath);
    std::copy_n(config_.hcomTlsConfig.keyPath, SMEM_TLS_PATH_SIZE, options.tlsOption.keyPath);
    std::copy_n(config_.hcomTlsConfig.keyPassPath, SMEM_TLS_PATH_SIZE, options.tlsOption.keyPassPath);
    std::copy_n(config_.hcomTlsConfig.packagePath, SMEM_TLS_PATH_SIZE, options.tlsOption.packagePath);
    std::copy_n(config_.hcomTlsConfig.decrypterLibPath, SMEM_TLS_PATH_SIZE, options.tlsOption.decrypterLibPath);

    return std::move(options);
}

bool SmemTransEntry::ParseExchangeInfo(const RankFullInfo &peer, SmemTransExchangeInfo &entityInfo,
                                       smem_trans_role_t &role, WorkerId &id) noexcept
{
    if (DeserializeTransExchangeInfo(peer.baseInfo.data(), peer.baseInfo.size(), entityInfo) == 0) {
        SM_LOG_ERROR("ParseExchangeInfo: invalid baseInfo size for rank " << peer.rankId
                                                                          << " size: " << peer.baseInfo.size());
        return false;
    }
    role = static_cast<smem_trans_role_t>(entityInfo.u.session.reserved);
    entityInfo.u.session.reserved = 0U; // reserved must be cleared so the WorkerId matches ParseNameToUniqueId
    WorkerIdUnion wu(entityInfo.u.session);
    id = wu.workerId;
    hybm_export_info_free(&entityInfo.hybmInfo);
    return true;
}

int SmemTransEntry::ParsePeerSlices(const RankFullInfo &peer, std::vector<SmemTransExchangeInfo> &slices,
                                    std::vector<LocalMapAddress> &local) noexcept
{
    slices.resize(peer.externalInfo.size());
    local.reserve(peer.externalInfo.size());
    for (size_t i = 0; i < peer.externalInfo.size(); ++i) {
        SmemTransExchangeInfo info{};
        if (DeserializeTransExchangeInfo(peer.externalInfo[i].data(), peer.externalInfo[i].size(), info) == 0) {
            SM_LOG_ERROR("ParsePeerSlices: invalid externalInfo size for rank "
                         << peer.rankId << " size: " << peer.externalInfo[i].size());
            for (auto &s : slices) {
                hybm_export_info_free(&s.hybmInfo);
            }
            return BM_INVALID_PARAM;
        }
        slices[i] = info;
        local.emplace_back(slices[i].u.address.address, slices[i].u.address.size);
    }
    return BM_OK;
}

bool SmemTransEntry::ApplyDataOpType(hybm_options &options, uint32_t flag, hybm_data_op_type hybmFlag,
                                     const char *opName) noexcept
{
    if (!(config_.dataOpType & flag)) {
        return true;
    }
#if !defined(ASCEND_NPU)
    // host 类 op 为纯主机侧传输，不依赖昇腾设备 SDK，非 NPU 构建同样可用
    const bool needNpu = (flag & (SMEMB_DATA_OP_HOST_RDMA | SMEMB_DATA_OP_HOST_URMA | SMEMB_DATA_OP_HOST_TCP)) == 0U;
    if (needNpu) {
        SM_LOG_ERROR("current memfabric-hybrid binary is not built for ascend npu, can not use " << opName
                                                                                                 << " optype.");
        return false;
    }
#endif
    auto temp = static_cast<uint32_t>(options.bmDataOpType) | hybmFlag;
    options.bmDataOpType = static_cast<hybm_data_op_type>(temp);
    return true;
}

} // namespace smem
} // namespace ock
