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
#include "host_hcom_transport_manager.h"

#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <sstream>
#include <arpa/inet.h>
#include "dl_hcom_api.h"
#include "hybm_logger.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "host_hcom_common.h"
#include "host_hcom_helper.h"
#include "mf_tls_util.h"
#include "hybm_ptracer.h"
#include "hybm_va_manager.h"

using namespace ock::mf;
using namespace ock::mf::transport;
using namespace ock::mf::transport::host;

namespace {
#if defined(NO_XPU)
constexpr uint64_t HCOM_MAX_SLICE_SIZE = 1024 * 1024 * 1024;
constexpr uint64_t HCOM_RECV_DATA_SIZE = 1024 * 1024UL;
constexpr uint64_t HCOM_SEND_QUEUE_SIZE = 16384UL;
constexpr uint64_t HCOM_RECV_QUEUE_SIZE = 16384UL;
constexpr uint64_t HCOM_COMPLETE_QUEUE_SIZE = 8192;
constexpr uint64_t HCOM_QUEUE_PRE_POST_SIZE = 1024UL;
constexpr uint8_t HCOM_TRANS_EP_SIZE = 1;
constexpr int8_t HCOM_THREAD_PRIORITY = -20;
constexpr uint64_t UB_SEGMENT_ADDR_ALIGN_SIZE = 4096UL;
#else
constexpr uint64_t HCOM_MAX_SLICE_SIZE = 1024 * 1024UL;
constexpr uint64_t HCOM_RECV_DATA_SIZE = HCOM_MAX_SLICE_SIZE + 1024;
constexpr uint64_t HCOM_SEND_QUEUE_SIZE = 512UL;
constexpr uint64_t HCOM_RECV_QUEUE_SIZE = 512UL;
constexpr uint64_t HCOM_COMPLETE_QUEUE_SIZE = 8192;
constexpr uint64_t HCOM_QUEUE_PRE_POST_SIZE = 256UL;
constexpr uint8_t HCOM_TRANS_EP_SIZE = 1;
constexpr int8_t HCOM_THREAD_PRIORITY = -20;
#endif
const char *HCOM_RPC_SERVICE_NAME = "hybm_hcom_service";
constexpr uint32_t HCOM_PAYLOAD_EP_SHIFT = 4U;
constexpr uint32_t HCOM_PAYLOAD_EP_MASK = (1U << HCOM_PAYLOAD_EP_SHIFT) - 1U;

HcomRuntimeConfig LoadHcomRuntimeConfig()
{
    HcomRuntimeConfig runtimeConfig{};
    runtimeConfig.maxSliceSize = HCOM_MAX_SLICE_SIZE;
    runtimeConfig.recvDataSize = HCOM_RECV_DATA_SIZE;
    runtimeConfig.maxSliceSize = MfEnvUtil::GetUintOrDefault(env::HCOM_MAX_SLICE_SIZE, HCOM_MAX_SLICE_SIZE);
    runtimeConfig.recvDataSize = MfEnvUtil::GetUintOrDefault(env::HCOM_RECV_DATA_SIZE, HCOM_RECV_DATA_SIZE);
    return runtimeConfig;
}

// Split ';' separated peer nic urls into per-ep urls (multi-link broadcast from remote side).
static void SplitRankNics(const std::string &nic, std::vector<std::string> &out)
{
    auto urls = StrUtil::Split(nic, ';');
    for (const auto &url : urls) {
        auto seg = StrUtil::StrTrim(url);
        if (!seg.empty()) {
            out.emplace_back(seg);
        }
    }
}

void HcomExternalLoggerAdapter(int level, const char *msg)
{
    const char *safeMsg = (msg == nullptr) ? "" : msg;
    switch (level) {
        case ock::mf::DEBUG_LEVEL:
            BM_LOG_DEBUG("[HCOM] " << safeMsg);
            break;
        case ock::mf::INFO_LEVEL:
            BM_LOG_INFO("[HCOM] " << safeMsg);
            break;
        case ock::mf::WARN_LEVEL:
            BM_LOG_WARN("[HCOM] " << safeMsg);
            break;
        case ock::mf::ERROR_LEVEL:
            BM_LOG_ERROR("[HCOM] " << safeMsg);
            break;
        default:
            BM_LOG_INFO("[HCOM-" << level << "] " << safeMsg);
            break;
    }
}
} // namespace

hybm_tls_config HcomTransportManager::tlsConfig_ = {};
char HcomTransportManager::keyPass_[KEYPASS_MAX_LEN] = {0};
std::mutex HcomTransportManager::keyPassMutex = {};
thread_local HcomCounterStreamPtr HcomTransportManager::stream_ = nullptr;

static void CopyHcomOneSideKey(const OneSideKey &from, TransportMemoryKey &to)
{
    std::copy_n(from.keys, std::size(from.keys), to.keys);
    std::copy_n(from.tokens, std::size(from.tokens), to.keys + std::size(from.keys));
#if defined(NO_XPU)
    auto offset = std::size(from.tokens) + std::size(from.keys);
    std::copy_n(from.eid, URMA_EID_LENGTH, reinterpret_cast<uint8_t *>(to.keys + offset));
#endif
}

static void CopyHcomOneSideKey(TransportMemoryKey &from, OneSideKey &to)
{
    std::copy_n(from.keys, std::size(to.keys), to.keys);
    std::copy_n(from.keys + std::size(to.keys), std::size(to.tokens), to.tokens);
#if defined(NO_XPU)
    auto offset = std::size(to.tokens) + std::size(to.keys);
    std::copy_n(reinterpret_cast<uint8_t *>(from.keys + offset), URMA_EID_LENGTH, to.eid);
#endif
}

Result HcomTransportManager::OpenDevice(const TransportOptions &options)
{
    BM_ASSERT_LOG_AND_RETURN(rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_OK);
    auto ret = CheckTransportOptions(options);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "ret = " << ret, BM_INVALID_PARAM);
    runtimeConfig_ = LoadHcomRuntimeConfig();
    DlHcomApi::SetExternalLogger(HcomExternalLoggerAdapter);
    Service_Options opt{};
    opt.workerGroupMode = C_SERVICE_BUSY_POLLING;
    opt.maxSendRecvDataSize = runtimeConfig_.recvDataSize;
    opt.workerThreadPriority = HCOM_THREAD_PRIORITY;
    Service_Type enumProtocolType = HostHcomHelper::HybmDopTransHcomProtocol(options.protocol, options.nic);
    tlsConfig_ = options.tlsOption;

    // One hcom service per ep(nic): dual cards need dual services (ubs limits one driver per service).
    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        Hcom_Service service = 0;
        auto serviceName = HCOM_RPC_SERVICE_NAME + std::string("_ep") + std::to_string(ep);
        ret = DlHcomApi::ServiceCreate(enumProtocolType, serviceName.c_str(), opt, &service);
        if (ret != 0) {
            BM_LOG_ERROR("Failed to create hcom service, ep: " << ep << " nic: " << localNics_[ep]
                                                               << " type: " << enumProtocolType << " ret: " << ret);
            DestroyServices();
            return BM_DL_FUNCTION_FAILED;
        }
        rpcServices_.emplace_back(service);
        BM_LOG_TRACE("Create hcom service successful, ep: " << ep << " nic: " << localNics_[ep]
                                                            << " type: " << enumProtocolType);
        DlHcomApi::ServiceSetHeartBeatOptions(service, 10, 3, 5); /* idle 10s, probe 3*5s, total ~25s */
        DlHcomApi::ServiceSetTlsOptions(service, options.tlsOption.tlsEnable, C_SERVICE_TLS_1_3, C_SERVICE_AES_GCM_256,
                                        GetCertCallBack, GetPrivateKeyCallBack, GetCACallBack);
        DlHcomApi::SetUbsModeFunc(service, UbsHcomServiceUbcMode::C_SERVICE_HIGHBANDWIDTH);
        DlHcomApi::ServiceRegisterChannelBrokerHandler(service, TransportRpcHcomEndPointBroken, C_CHANNEL_RECONNECT, 1);
        DlHcomApi::ServiceRegisterHandler(service, C_SERVICE_REQUEST_RECEIVED, TransportRpcHcomRequestReceived, 1);
        DlHcomApi::ServiceRegisterHandler(service, C_SERVICE_REQUEST_POSTED, TransportRpcHcomRequestPosted, 1);
        DlHcomApi::ServiceRegisterHandler(service, C_SERVICE_READWRITE_DONE, TransportRpcHcomOneSideDone, 1);
        if (enumProtocolType != Service_Type::C_SERVICE_UBC) {
            std::string ipMask = localIps_[ep] + "/32";
            DlHcomApi::ServiceSetDeviceIpMask(service, ipMask.c_str());
        }
        SetHcomServiceConfig(service);
        BM_LOG_INFO("bind hcom service ep: " << ep << " listen url: " << localNics_[ep]);
        DlHcomApi::ServiceBind(service, localNics_[ep].c_str(), TransportRpcHcomNewEndPoint);
    }

    ret = reconnect_.Start(
        [this](uint32_t rankId, uint32_t ep, const std::string &nic) { return ConnectHcomChannel(rankId, ep, nic); });
    if (ret != BM_OK) {
        BM_LOG_ERROR("start reconnect service failed: " << ret);
        DestroyServices();
        return ret;
    }

    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        ret = DlHcomApi::ServiceStart(rpcServices_[ep]);
        if (ret != 0) {
            BM_LOG_ERROR("Failed to start hcom service, ep: " << ep << " nic: " << localNics_[ep]
                                                              << " type: " << enumProtocolType << " ret: " << ret);
            reconnect_.Stop();
            DestroyServices();
            return BM_DL_FUNCTION_FAILED;
        }
    }
    bmOptype_ = static_cast<hybm_data_op_type>(options.protocol);
    rankId_ = options.rankId;
    rankCount_ = options.rankCount;
    mrMutex_ = std::vector<std::mutex>(rankCount_);
    mrs_.assign(rankCount_, std::vector<std::set<HcomMemoryRegion>>(epCount_));
    channelMutex_ = std::vector<std::mutex>(rankCount_);
    nics_ = std::vector<std::vector<std::string>>(rankCount_, std::vector<std::string>(epCount_, ""));
    channels_ = std::vector<std::vector<Hcom_Channel>>(rankCount_, std::vector<Hcom_Channel>(epCount_, 0));
    return BM_OK;
}

Result HcomTransportManager::CloseDevice()
{
    DlHcomApi::SetExternalLogger([]([[maybe_unused]] int level, [[maybe_unused]] const char *msg) {});
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_OK);

    reconnect_.Stop();
    for (uint32_t i = 0; i < rankCount_; ++i) {
        ClearRankChannels(i);
    }

    // destroy all registered MRs first to release UBContext refs before ServiceDestroy
    for (uint32_t i = 0; i < mrMutex_.size(); ++i) {
        std::unique_lock<std::mutex> lock(mrMutex_[i]);
        for (uint32_t ep = 0; ep < epCount_; ++ep) {
            for (auto it = mrs_[i][ep].begin(); it != mrs_[i][ep].end();) {
                DlHcomApi::ServiceDestroyMemoryRegion(rpcServices_[ep], it->mr);
                it = mrs_[i][ep].erase(it);
            }
        }
    }

    DestroyServices();
    mf::MfTlsUtil::CloseTlsLib();
    localNic_ = "";
    localIp_ = "";
    localNics_.clear();
    localIps_.clear();
    rankId_ = UINT32_MAX;
    rankCount_ = 0;
    runtimeConfig_ = {};
    mrMutex_.clear();
    mrs_.clear();
    channelMutex_.clear();
    nics_.clear();
    channels_.clear();
    return BM_OK;
}

void HcomTransportManager::DestroyServices()
{
    for (uint32_t ep = 0; ep < rpcServices_.size(); ++ep) {
        auto serviceName = HCOM_RPC_SERVICE_NAME + std::string("_ep") + std::to_string(ep);
        auto ret = DlHcomApi::ServiceDestroy(rpcServices_[ep], serviceName.c_str());
        if (ret != 0) {
            BM_LOG_WARN("Unable to destroy hcom service, ep: " << ep << " ret: " << ret);
        }
    }
    rpcServices_.clear();
}

#if defined(ASCEND_NPU) || defined(NVIDIA_GPU)
Result HcomTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(mr.addr != 0 && mr.size != 0, "mr.addr = " << mr.addr << ", " << "mr.size = " << mr.size,
                             BM_INVALID_PARAM);
    const bool isHbm = (mr.flags & transport::REG_MR_FLAG_HBM) != 0;
    if ((mr.flags & (transport::REG_MR_FLAG_DRAM | transport::REG_MR_FLAG_HBM)) == 0) {
        BM_LOG_WARN("Only support register dram/hbm memory, skip flag:" << mr.flags);
        return BM_OK;
    }

    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        HcomMemoryRegion info{};
        if (GetMemoryRegionByAddr(rankId_, ep, mr.addr, info) == BM_OK) {
            BM_LOG_ERROR("Failed to register mem region, addr: " << mr.addr << " already registered, ep: " << ep);
            return BM_ERROR;
        }

        Service_MemoryRegion memoryRegion;
        int32_t ret = DlHcomApi::ServiceRegisterAssignMemoryRegion(rpcServices_[ep], mr.addr, mr.size, &memoryRegion);
        if (ret != 0) {
            if (isHbm) {
                // Old HDK libhcom can not register hbm to host rdma service; skip and degrade to swap path.
                BM_LOG_WARN("Failed to register hbm mem region, maybe hdk not support (ret: "
                            << ret << "), fall back to swap path, size: " << mr.size << " addr:" << std::hex << mr.addr
                            << " ep: " << ep);
                continue;
            }
            BM_LOG_ERROR("Failed to register mem region, size: " << mr.size << " addr:" << std::hex << mr.addr
                                                                 << " service: " << rpcServices_[ep] << " ret: " << ret);
            return BM_DL_FUNCTION_FAILED;
        }

        Service_MemoryRegionInfo memoryRegionInfo;
        ret = DlHcomApi::ServiceGetMemoryRegionInfo(memoryRegion, &memoryRegionInfo);
        if (ret != 0) {
            BM_LOG_ERROR("Failed to get mem region info, size: " << mr.size << " service: " << rpcServices_[ep]
                                                                 << " ret: " << ret);
            DlHcomApi::ServiceDestroyMemoryRegion(rpcServices_[ep], memoryRegion);
            return BM_DL_FUNCTION_FAILED;
        }

        HcomMemoryRegion mrInfo{};
        mrInfo.lva = mr.addr;
        mrInfo.addr = mr.addr;
        mrInfo.size = mr.size;
        mrInfo.mr = memoryRegion;
        std::copy_n(memoryRegionInfo.lKey.keys,
                    sizeof(memoryRegionInfo.lKey.keys) / sizeof(memoryRegionInfo.lKey.keys[0]), mrInfo.lKey.keys);
        {
            std::unique_lock<std::mutex> lock(mrMutex_[rankId_]);
            mrs_[rankId_][ep].insert(mrInfo);
        }
        BM_LOG_INFO("Success to register " << (isHbm ? "hbm" : "dram") << " mr info size: " << mrInfo.size
                                           << " ep: " << ep << " lKey: " << mrInfo.lKey.keys[0]);
    }
    return BM_OK;
}
#else
Result HcomTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(mr.addr != 0 && mr.size != 0, "mr.addr = " << mr.addr << ", " << "mr.size = " << mr.size,
                             BM_INVALID_PARAM);
    if ((mr.flags & transport::REG_MR_FLAG_DRAM) == 0) {
        BM_LOG_WARN("Unable to register hcom mr, mem type flag should be dram.");
        return BM_OK;
    }

    if ((bmOptype_ == HYBM_DOP_TYPE_HOST_URMA) && (mr.addr & (UB_SEGMENT_ADDR_ALIGN_SIZE - 1)) != 0) {
        BM_LOG_ERROR("Failed to register ub mem region, whose addr must be 4k aligned");
        return BM_INVALID_PARAM;
    }

    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        HcomMemoryRegion info{};
        if (GetMemoryRegionByAddr(rankId_, ep, mr.addr, info) == BM_OK) {
            BM_LOG_ERROR("Failed to register mem region, addr already registered, ep: " << ep);
            return BM_ERROR;
        }

        Service_MemoryRegion memoryRegion;
        int32_t ret = DlHcomApi::ServiceRegisterAssignMemoryRegion(rpcServices_[ep], mr.addr, mr.size, &memoryRegion);
        // 单rank不需要hcom,目的是在无网卡的情况下也可以测试
        if (ret != 0) {
            BM_LOG_ERROR("Failed to register mem region, size: " << mr.size << " addr:" << std::hex << mr.addr
                                                                 << " service: " << rpcServices_[ep] << " ret: " << ret);
            return BM_ERROR;
        }

        Service_MemoryRegionInfo memoryRegionInfo;
        if (ret == 0) {
            ret = DlHcomApi::ServiceGetMemoryRegionInfo(memoryRegion, &memoryRegionInfo);
        }
        if (ret != 0) {
            BM_LOG_ERROR("Failed to get mem region info, size: " << mr.size << " service: " << rpcServices_[ep]
                                                                 << " ret: " << ret);
            DlHcomApi::ServiceDestroyMemoryRegion(rpcServices_[ep], memoryRegion);
            return BM_ERROR;
        }

        HcomMemoryRegion mrInfo{};
        mrInfo.lva = mr.addr;
        mrInfo.addr = mr.addr;
        mrInfo.size = mr.size;
        mrInfo.mr = memoryRegion;
        CopyHcomOneSideKey(memoryRegionInfo.lKey, mrInfo.lKey);
        {
            std::unique_lock<std::mutex> lock(mrMutex_[rankId_]);
            mrs_[rankId_][ep].insert(mrInfo);
        }
        BM_LOG_INFO("Success to register to mr info size: " << mrInfo.size << " ep: " << ep
                                                            << " lKey: " << mrInfo.lKey.keys[0] << std::hex
                                                            << " laddr:" << mr.addr);
    }
    return BM_OK;
}
#endif

Result HcomTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    BM_ASSERT_LOG_AND_RETURN(addr != 0, "addr = " << addr, BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);

    std::unique_lock<std::mutex> lock(mrMutex_[rankId_]);
    bool found = false;
    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        auto &localMrs = mrs_[rankId_][ep];
        for (auto it = localMrs.begin(); it != localMrs.end(); ++it) {
            if (it->addr == addr) {
                DlHcomApi::ServiceDestroyMemoryRegion(rpcServices_[ep], it->mr);
                localMrs.erase(it);
                found = true;
                BM_LOG_INFO("Addr: " << addr << " unregistered, ep: " << ep);
                break;
            }
        }
    }
    if (!found) {
        BM_LOG_WARN("Addr: " << addr << " not registered");
    }
    return BM_OK;
}

bool HcomTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    std::unique_lock<std::mutex> lock(mrMutex_[rankId_]);
    for (const auto &mrInfo : mrs_[rankId_][0]) {
        if (mrInfo.addr <= addr && mrInfo.addr + mrInfo.size >= addr + size) {
            return true;
        }
    }
    return false;
}

uint32_t HcomTransportManager::GetLinkCount() const
{
    return epCount_;
}

Result HcomTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    return QueryMemoryKeyByEp(addr, 0, key);
}

Result HcomTransportManager::QueryMemoryKeyByEp(uint64_t addr, uint32_t ep, TransportMemoryKey &key)
{
    HcomMemoryRegion mrInfo{};
    if (GetMemoryRegionByAddr(rankId_, ep, addr, mrInfo) != BM_OK) {
        BM_LOG_ERROR("Failed to query memory region, addr: 0x" << std::hex << addr << " rankId: " << rankId_
                                                               << " ep: " << ep);
        return BM_ERROR;
    }
    RegMemoryKeyUnion hostKey{};
    hostKey.hostKey.type = TT_HCOM;
    hostKey.hostKey.reserved = ep; // 对端据此把 key 落到对应 ep 的 MR 表
    hostKey.hostKey.gva = HybmVaManager::GetInstance().TransformVa(mrInfo.addr, HVM_HVA, HVM_GVA);
    hostKey.hostKey.hcomInfo.lAddress = mrInfo.addr;
    CopyHcomOneSideKey(mrInfo.lKey, hostKey.hostKey.hcomInfo.lKey);
    hostKey.hostKey.hcomInfo.size = mrInfo.size;
    key = hostKey.commonKey;
    BM_LOG_INFO("Success to query memory key ep: " << ep << " addr:" << std::hex << mrInfo.addr
                                                   << " size:" << mrInfo.size);
    return BM_OK;
}

void HcomTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    return;
}

Result HcomTransportManager::Prepare(const HybmTransPrepareOptions &param)
{
    auto options = param.options;
    for (const auto &item : options) {
        auto rankId = item.first;
        if (rankId >= rankCount_) {
            BM_LOG_ERROR("Failed to update rank info ranId: " << rankId << " not match rank count: " << rankCount_);
            return BM_INVALID_PARAM;
        }
    }

    std::vector<uint32_t> toAddRanks;
    toAddRanks.reserve(options.size());
    for (const auto &item : options) {
        auto rankId = item.first;
        std::vector<std::string> eps;
        SplitRankNics(item.second.nic, eps);
        for (uint32_t ep = 0; ep < eps.size() && ep < epCount_; ++ep) {
            nics_[rankId][ep] = eps[ep];
        }
        toAddRanks.emplace_back(rankId);
    }
    reconnect_.AddRanks(toAddRanks);

    return UpdateRankMrInfos(options);
}

Result HcomTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    BM_LOG_DEBUG("HCOM transport manager remove ranks not implements!");
    reconnect_.RemoveRanks(removedRanks);
    return BM_OK;
}

Result HcomTransportManager::Connect()
{
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    TP_TRACE_BEGIN(TP_SMEM_GROUP_HCOM_CONNECT_BATCH);

    // Collect ranks to connect
    std::vector<uint32_t> targets;
    for (uint32_t i = 0; i < rankCount_; ++i) {
        if (rankId_ <= i) {
            continue;
        }
        bool hasNic = false;
        for (uint32_t ep = 0; ep < epCount_ && !hasNic; ++ep) {
            hasNic = !nics_[i][ep].empty();
        }
        if (hasNic) {
            targets.push_back(i);
        }
    }
    if (targets.empty()) {
        TP_TRACE_END(TP_SMEM_GROUP_HCOM_CONNECT_BATCH, 0);
        return BM_OK;
    }

    auto ret = ConnectTargets(targets);
    TP_TRACE_END(TP_SMEM_GROUP_HCOM_CONNECT_BATCH, ret == BM_OK ? 0 : 1);
    return ret;
}

Result HcomTransportManager::ConnectTargets(const std::vector<uint32_t> &targets)
{
    // Parallel connect using fixed-size thread pool
    constexpr size_t poolSize = 8;
    std::atomic<int> failed{0};
    std::mutex successMtx;
    std::vector<uint32_t> connected;
    std::vector<std::thread> pool;

    auto worker = [this, &failed, &successMtx, &connected](uint32_t rankId) {
        bool rankOk = true;
        for (uint32_t ep = 0; ep < epCount_; ++ep) {
            if (nics_[rankId][ep].empty()) {
                continue;
            }
            auto ret = ConnectHcomChannel(rankId, ep, nics_[rankId][ep]);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to connect rank " << rankId << " ep: " << ep << " nic: " << nics_[rankId][ep]
                                                       << " ret: " << ret);
                rankOk = false;
                break;
            }
        }
        if (!rankOk) {
            failed.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::lock_guard<std::mutex> lock(successMtx);
            connected.push_back(rankId);
        }
    };

    for (size_t idx = 0; idx < targets.size(); ++idx) {
        pool.emplace_back(worker, targets[idx]);
        if (pool.size() >= poolSize) {
            for (auto &t : pool)
                t.join();
            if (failed.load() != 0) {
                for (auto r : connected) {
                    ClearRankChannels(r);
                }
                return BM_ERROR;
            }
            pool.clear();
            connected.clear();
        }
    }
    for (auto &t : pool)
        t.join();
    bool ok = (failed.load() == 0);
    if (!ok) {
        for (auto r : connected) {
            ClearRankChannels(r);
        }
    }
    return ok ? BM_OK : BM_ERROR;
}

static constexpr uint32_t HCOM_CHANNEL_READY_TIMEOUT_MS = 5000;
static constexpr uint32_t HCOM_CHANNEL_POLL_INTERVAL_US = 1000;

Result HcomTransportManager::WaitChannelReady(uint32_t rankId, uint32_t timeoutMs) noexcept
{
    auto startTime = std::chrono::steady_clock::now();
    auto deadline = startTime + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::unique_lock<std::mutex> lock(channelMutex_[rankId]);
            for (uint32_t ep = 0; ep < epCount_; ++ep) {
                if (channels_[rankId][ep] != 0) {
                    return BM_OK;
                }
            }
        }
        usleep(HCOM_CHANNEL_POLL_INTERVAL_US);
    }
    return BM_ERROR;
}

Result HcomTransportManager::ConnectRank(uint32_t rankId)
{
    if (rankId >= rankCount_) {
        return BM_OK;
    }
    if (rankId_ <= rankId) {
        // Lower rank: wait for higher rank's ServiceConnect to trigger our HCOM callback
        return WaitChannelReady(rankId, HCOM_CHANNEL_READY_TIMEOUT_MS);
    }
    TP_TRACE_BEGIN(TP_SMEM_GROUP_CONNECT_RANK);
    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        if (nics_[rankId][ep].empty()) {
            continue;
        }
        const auto ret = ConnectHcomChannel(rankId, ep, nics_[rankId][ep]);
        if (ret != BM_OK) {
            TP_TRACE_END(TP_SMEM_GROUP_CONNECT_RANK, 1);
            return ret;
        }
    }
    // Active side: serviceConnect is complete, but wait for passive side to confirm too
    auto waitRet = WaitChannelReady(rankId, HCOM_CHANNEL_READY_TIMEOUT_MS);
    if (waitRet != BM_OK) {}
    TP_TRACE_END(TP_SMEM_GROUP_CONNECT_RANK, 0);
    return BM_OK;
}

Result HcomTransportManager::AsyncConnect()
{
    return BM_OK;
}

Result HcomTransportManager::WaitForConnected(int64_t timeoutNs)
{
    return BM_OK;
}

Result HcomTransportManager::UpdateRankMrInfos(const std::unordered_map<uint32_t, TransportRankPrepareInfo> &opt)
{
    for (const auto &item : opt) {
        auto rankId = item.first;
        if (rankId == rankId_) {
            continue;
        }
        for (const auto &memKey : item.second.memKeys) {
            RegMemoryKeyUnion keyUnion{};
            keyUnion.commonKey = memKey;
            HcomMemoryRegion mrInfo{};
            mrInfo.lva = keyUnion.hostKey.hcomInfo.lAddress;
            mrInfo.addr = keyUnion.hostKey.gva;
            mrInfo.size = keyUnion.hostKey.hcomInfo.size;
            if (mrInfo.size == 0) {
                continue;
            }
            uint32_t ep = keyUnion.hostKey.reserved; // QueryMemoryKeyByEp 写入，单链路时恒为 0
            if (ep >= epCount_) {
                BM_LOG_ERROR("import mr with invalid ep: " << ep << " rankId: " << rankId
                                                           << " addr: 0x" << std::hex << mrInfo.addr);
                continue;
            }
            if (rankId != rankId_ && (bmOptype_ & HYBM_DOP_TYPE_HOST_URMA)) {
                auto ret = DlHcomApi::ImportUrmaSegFunc(rpcServices_[ep], mrInfo.addr, mrInfo.size,
                                                        &keyUnion.hostKey.hcomInfo.lKey);
                BM_ASSERT_LOG_AND_RETURN(ret == 0, "ret = " << ret, ret);
                BM_LOG_DEBUG("hcom returned, tokens: " << keyUnion.hostKey.hcomInfo.lKey.tokens[0]);
            }
            CopyHcomOneSideKey(keyUnion.hostKey.hcomInfo.lKey, mrInfo.lKey);
            {
                std::unique_lock<std::mutex> lock(mrMutex_[rankId]);
                mrs_[rankId][ep].insert(mrInfo);
            }
            BM_LOG_INFO("Success to register to mr info rankId: " << rankId << " ep: " << ep
                                                                  << " size: " << mrInfo.size
                                                                  << " lKey: " << mrInfo.lKey.keys[0]);
        }
    }
    return BM_OK;
}

Result HcomTransportManager::UpdateRankConnectInfos(const std::unordered_map<uint32_t, TransportRankPrepareInfo> &opt)
{
    std::vector<uint32_t> addRankList;
    for (uint32_t i = 0; i < rankCount_; ++i) {
        if (i >= rankId_) {
            break;
        }
        auto it = opt.find(i);
        if (it != opt.end()) {
            std::vector<std::string> eps;
            SplitRankNics(it->second.nic, eps);
            for (uint32_t ep = 0; ep < eps.size() && ep < epCount_; ++ep) {
                nics_[i][ep] = eps[ep];
            }
            addRankList.emplace_back(i);
            BM_LOG_DEBUG("UpdateRankConnectInfos: saved nics for rank " << i << " epCount: " << eps.size());
        }
    }

    reconnect_.AddRanks(addRankList);
    return BM_OK;
}

Result HcomTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &param)
{
    auto options = param.options;
    for (const auto &item : options) {
        auto rankId = item.first;
        if (rankId >= rankCount_) {
            BM_LOG_ERROR("Failed to update rank info ranId: " << rankId << " not match rank count: " << rankCount_);
            return BM_INVALID_PARAM;
        }
    }
    auto ret = UpdateRankMrInfos(param.options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to update rank mr info ret: " << ret);
        return ret;
    }
    ret = UpdateRankConnectInfos(param.options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to update rank connect info ret: " << ret);
        return ret;
    }
    return BM_OK;
}

const std::string &HcomTransportManager::GetNic() const
{
    return localNic_;
}

Result HcomTransportManager::InnerReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(size <= std::numeric_limits<uint32_t>::max(),
                             "size = " << size << " > " << std::numeric_limits<uint32_t>::max(), BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId][0];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    Channel_OneSideRequest req;

    req.lAddress = (void *)lAddr;
    req.size = static_cast<uint32_t>(size);

    HcomMemoryRegion mr{};
    auto ret = GetMemoryRegionByAddr(rankId_, 0, lAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find lKey, lAddr: is not register");
        return BM_ERROR;
    }
    std::copy_n(mr.lKey.keys, std::size(req.lKey.keys), req.lKey.keys);
    ret = GetMemoryRegionByAddr(rankId, 0, rAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << " is not set");
        return BM_ERROR;
    }
    CopyHcomOneSideKey(mr.lKey, req.rKey);
    BM_LOG_DEBUG("Try to read remote rankId: " << rankId << " channel: " << (void *)channel
                                               << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                               << " size: " << size);

    auto addrOffset = rAddr - mr.addr;
    rAddr = mr.lva + addrOffset; // rewrite to remote local va
    req.rAddress = (void *)rAddr;
    return DlHcomApi::ChannelGet(channel, req, nullptr);
}

Result HcomTransportManager::InnerWriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(size <= std::numeric_limits<uint32_t>::max(),
                             "size = " << size << " > " << std::numeric_limits<uint32_t>::max(), BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId][0];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    Channel_OneSideRequest req;
    req.lAddress = (void *)lAddr;
    req.size = static_cast<uint32_t>(size);

    HcomMemoryRegion mr{};
    auto ret = GetMemoryRegionByAddr(rankId_, 0, lAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find lKey, lAddr is not register");
        return BM_ERROR;
    }
    std::copy_n(mr.lKey.keys, sizeof(req.lKey.keys) / sizeof(req.lKey.keys[0]), req.lKey.keys);
    ret = GetMemoryRegionByAddr(rankId, 0, rAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << " is not set");
        return BM_ERROR;
    }
    CopyHcomOneSideKey(mr.lKey, req.rKey);

    auto addrOffset = rAddr - mr.addr;
    BM_LOG_DEBUG("Try to write remote rankId: " << rankId << " channel: " << (void *)channel
                                                << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                                << " size: " << size);
    rAddr = mr.lva + addrOffset; // rewrite to remote local va

    req.rAddress = (void *)rAddr;
    return DlHcomApi::ChannelPut(channel, req, nullptr);
}

int HcomTransportManager::PrepareThreadLocalStream()
{
    lock_.LockRead();
    if (stream_ != nullptr) {
        lock_.UnLock();
        return BM_OK;
    }
    lock_.UnLock();
    WriteGuard lockGuard(lock_);
    stream_ = std::make_shared<HostHcomCounterStream>(0);
    return BM_OK;
}

Result HcomTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId][0];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    Channel_OneSideRequest req;
    req.size = static_cast<uint32_t>(size);
    HcomMemoryRegion mr{};
    auto ret = GetMemoryRegionByAddr(rankId_, 0, lAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << ", size: " << req.size
                                                     << ", lAddr: " << VaToInfo(lAddr));
        return BM_ERROR;
    }
    CopyHcomOneSideKey(mr.lKey, req.lKey);
    mr.lKey = {};
    ret = GetMemoryRegionByAddr(rankId, 0, rAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << ", size: " << req.size
                                                     << ", rAddr: " << VaToInfo(rAddr));
        return BM_ERROR;
    }
    CopyHcomOneSideKey(mr.lKey, req.rKey);
    auto addrOffset = rAddr - mr.addr;
    rAddr = mr.lva + addrOffset; // rewrite to remote local va
    BM_LOG_DEBUG("Try to read remote rankId: " << rankId << " channel: " << (void *)channel
                                               << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                               << " lAddr:" << VaToStr(lAddr) << " rAddr: " << VaToStr(rAddr)
                                               << " size: " << size << " tokens: " << req.rKey.tokens[0]);
    ret = PrepareThreadLocalStream();
    if (ret != BM_OK) {
        BM_LOG_ERROR("prepare stream error rankId: " << rankId);
        return ret;
    }
    BM_ASSERT_LOG_AND_RETURN(stream_.get() != nullptr, "stream_.get() is nullptr", BM_ERROR);
    Channel_Callback channelCallback;
    channelCallback.arg = stream_.get();
    channelCallback.cb = ChannelAsyncCallback;
    uint64_t remain = size;
    uint64_t offset = 0;
    while (remain > 0) {
        uint32_t sliceSize = remain > runtimeConfig_.maxSliceSize ? runtimeConfig_.maxSliceSize : remain;

        req.rAddress = reinterpret_cast<void *>(rAddr + offset);
        req.lAddress = reinterpret_cast<void *>(lAddr + offset);
        req.size = sliceSize;
        stream_->SubmitTasks();
        TP_TRACE_BEGIN(TP_HYBM_HOST_RDMA_HCOM_CH_GET);
        ret = DlHcomApi::ChannelGet(channel, req, &channelCallback);
        TP_TRACE_END(TP_HYBM_HOST_RDMA_HCOM_CH_GET, ret);
        if (ret != 0) {
            stream_->FailedOne(false);
            Synchronize(rankId_);
            BM_LOG_ERROR("Failed to submit read task lRank:" << rankId_ << " rRank:" << rankId
                                                             << " lAddr:" << VaToStr(lAddr + offset) << "rAddr:"
                                                             << VaToStr(rAddr + offset) << " size:" << sliceSize);
            return ret;
        }
        offset += sliceSize;
        remain -= sliceSize;
    }
    return ret;
}

Result HcomTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId][0];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    Channel_OneSideRequest req;
    req.rAddress = reinterpret_cast<void *>(rAddr);
    req.lAddress = reinterpret_cast<void *>(lAddr);
    req.size = static_cast<uint32_t>(size);
    HcomMemoryRegion mr{};
    auto ret = GetMemoryRegionByAddr(rankId_, 0, lAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << ", size: " << req.size
                                                     << ", lAddr: " << VaToInfo(lAddr));
        return BM_ERROR;
    }
    std::copy_n(mr.lKey.keys, std::size(req.lKey.keys), req.lKey.keys);
    mr.lKey = {};
    ret = GetMemoryRegionByAddr(rankId, 0, rAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << ", size: " << req.size
                                                     << ", rAddr: " << VaToInfo(rAddr));
        return BM_ERROR;
    }
    CopyHcomOneSideKey(mr.lKey, req.rKey);
    auto addrOffset = rAddr - mr.addr;
    rAddr = mr.lva + addrOffset; // rewrite to remote local va
    BM_LOG_DEBUG("Try to write remote rankId: " << rankId << " channel: " << (void *)channel
                                                << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                                << " lAddr:" << VaToStr(lAddr) << " rAddr: " << VaToStr(rAddr)
                                                << " size: " << size << " tokens: " << req.rKey.tokens[0]);
    ret = PrepareThreadLocalStream();
    if (ret != BM_OK) {
        BM_LOG_ERROR("prepare stream error rankId: " << rankId);
        return ret;
    }
    BM_ASSERT_LOG_AND_RETURN(stream_.get() != nullptr, "stream_.get() is nullptr", BM_ERROR);
    Channel_Callback channelCallback;
    channelCallback.arg = stream_.get();
    channelCallback.cb = ChannelAsyncCallback;
    uint64_t remain = size;
    uint64_t offset = 0;
    while (remain > 0) {
        uint32_t sliceSize = remain > runtimeConfig_.maxSliceSize ? runtimeConfig_.maxSliceSize : remain;

        req.rAddress = reinterpret_cast<void *>(rAddr + offset);
        req.lAddress = reinterpret_cast<void *>(lAddr + offset);
        req.size = sliceSize;
        stream_->SubmitTasks();
        ret = DlHcomApi::ChannelPut(channel, req, &channelCallback);
        if (ret != BM_OK) {
            stream_->FailedOne(false);
            Synchronize(rankId_);
            BM_LOG_ERROR("Failed to submit put task lRank:" << rankId_ << " rRank:" << rankId
                                                            << " lAddr:" << VaToStr(lAddr + offset) << "rAddr:"
                                                            << VaToStr(rAddr + offset) << " size:" << sliceSize);
            return ret;
        }
        offset += sliceSize;
        remain -= sliceSize;
    }
    return ret;
}
Result HcomTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    BM_LOG_INFO("WriteRemoteBatchAsync start " << rankId << " rankId");
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId][0];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }

    uint32_t allBatch = descriptor.counts.size();
    auto batchs = (allBatch + HCOM_IOV_BATCH_SIZE - 1) / HCOM_IOV_BATCH_SIZE; // 向上取整
    uint32_t index = 0;
    while (index < batchs) {
        Channel_OneSideRequestSgl sglReq;
        sglReq.iovCount = 0;
        for (uint32_t i = index * HCOM_IOV_BATCH_SIZE; i < std::min(allBatch, (index + 1) * HCOM_IOV_BATCH_SIZE); ++i) {
            Channel_OneSideRequest req;
            req.lAddress = descriptor.localAddrs[i];
            req.size = static_cast<uint32_t>(descriptor.counts[i]);
            HcomMemoryRegion mr{};
            auto ret = GetMemoryRegionByAddr(rankId_, 0, reinterpret_cast<uint64_t>(req.lAddress), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << ", size: " << req.size
                                                             << ", lAddr: " << VaToStr(req.lAddress));
                return BM_ERROR;
            }
            std::copy_n(mr.lKey.keys, std::size(req.lKey.keys), req.lKey.keys);
            mr.lKey = {};
            auto rAddr = descriptor.globalAddrs[i];
            ret = GetMemoryRegionByAddr(rankId, 0, reinterpret_cast<uint64_t>(rAddr), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << ", size: " << req.size
                                                             << ", rAddr: " << VaToStr(rAddr));
                return BM_ERROR;
            }
            auto offset = reinterpret_cast<uint64_t>(rAddr) - mr.addr;
            req.rAddress = reinterpret_cast<void *>(mr.lva + offset); // rewrite to remote local va
            CopyHcomOneSideKey(mr.lKey, req.rKey);
            BM_LOG_DEBUG("Try to write remote rankId: " << rankId << " channel: " << (void *)channel
                                                        << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                                        << " lAddr:" << VaToStr(req.lAddress) << " rAddr: "
                                                        << VaToStr(req.rAddress) << " size: " << descriptor.counts[i]
                                                        << " tokens: " << req.rKey.tokens[0]);
            ret = PrepareThreadLocalStream();
            if (ret != BM_OK) {
                BM_LOG_ERROR("prepare stream error rankId: " << rankId);
                return ret;
            }

            sglReq.iov[i - index * HCOM_IOV_BATCH_SIZE] = req;
            sglReq.iovCount++;
        }
        index++;
        BM_ASSERT_LOG_AND_RETURN(stream_.get() != nullptr, "stream_.get() is nullptr", BM_ERROR);
        Channel_Callback channelCallback;
        channelCallback.arg = stream_.get();
        channelCallback.cb = ChannelAsyncCallback;

        stream_->SubmitTasks();
        BM_LOG_INFO("DlHcomApi::ChannelPutV start, sglReq iocount " << sglReq.iovCount);
        auto ret = DlHcomApi::ChannelPutV(channel, sglReq, &channelCallback);
        if (ret != BM_OK) {
            stream_->FailedOne(false);
            Synchronize(rankId_);
            BM_LOG_ERROR("Failed to submit put task lRank:" << rankId_ << " rRank:" << rankId);
            return ret;
        }
    }
    return BM_OK;
}

Result HcomTransportManager::Synchronize(const uint32_t rankId)
{
    if (stream_ == nullptr) {
        return BM_OK;
    }
    return stream_->Synchronize(static_cast<int32_t>(rankId));
}

Result HcomTransportManager::CheckTransportOptions(const TransportOptions &options)
{
    // Multi-link support: options.nic may carry several urls separated by ';', one url per ep(nic).
    auto urls = StrUtil::Split(options.nic, ';');
    std::string protocol;
    std::vector<std::string> epNics;
    std::vector<std::string> epIps;
    for (const auto &url : urls) {
        auto seg = StrUtil::StrTrim(url);
        if (seg.empty()) {
            continue;
        }
        std::string ip;
        uint32_t basePort = 0;
        auto ret = HostHcomHelper::AnalysisNic(seg, protocol, ip, basePort);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to check nic, nic: " << seg << " ret: " << ret);
            return ret;
        }
        const auto hcomAutoPort = basePort + options.rankId;
        epNics.emplace_back(protocol + ip + ":" + std::to_string(hcomAutoPort));
        epIps.emplace_back(ip);
        BM_LOG_INFO("hcom base port: " << basePort << ", hcom auto port with rank: " << hcomAutoPort
                                       << ", listen url: " << epNics.back());
    }
    if (epNics.empty()) {
        BM_LOG_ERROR("Failed to check nic, no valid url in nic: " << options.nic);
        return BM_INVALID_PARAM;
    }
    localIps_ = std::move(epIps);
    localNics_ = std::move(epNics);
    epCount_ = static_cast<uint32_t>(localNics_.size());
    std::string joined;
    for (const auto &nic : localNics_) {
        joined = joined.empty() ? nic : joined + ";" + nic;
    }
    localNic_ = joined;
    localIp_ = localIps_[0];
    return BM_OK;
}

Result HcomTransportManager::TransportRpcHcomNewEndPoint(Hcom_Channel newCh, uint64_t usrCtx, const char *payLoad)
{
    const char *logPayLoad = (payLoad != nullptr) ? payLoad : "<null>";
    BM_LOG_DEBUG("New hcom ch, ch: " << std::hex << newCh << " usrCtx: " << usrCtx << " payLoad: " << logPayLoad);
    uint64_t payloadNum = UINT64_MAX;
    if (payLoad == nullptr || !StrUtil::String2Uint<uint64_t>(payLoad, payloadNum)) {
        BM_LOG_ERROR("Failed to get rankId payLoad: " << logPayLoad);
        return BM_ERROR;
    }

    HcomPayload payloadUn{};
    payloadUn.payload = payloadNum;
    auto rankId = payloadUn.client;
    auto ep = payloadUn.serverAndEp & HCOM_PAYLOAD_EP_MASK;
    BM_LOG_DEBUG("new channel from " << payloadUn.client << " to " << (payloadUn.serverAndEp >> HCOM_PAYLOAD_EP_SHIFT)
                                     << " ep: " << ep);

    auto self = HcomTransportManager::GetInstance();
    if (rankId >= self->channels_.size() || ep >= self->epCount_) {
        BM_LOG_ERROR("new channel with invalid rank: " << rankId << " ep: " << ep);
        return BM_ERROR;
    }
    std::unique_lock<std::mutex> locker{self->channelMutex_[rankId]};
    self->channels_[rankId][ep] = newCh;
    locker.unlock();

    return BM_OK;
}

Result HcomTransportManager::TransportRpcHcomEndPointBroken(Hcom_Channel ch, uint64_t usrCtx, const char *payLoad)
{
    const char *logPayLoad = (payLoad != nullptr) ? payLoad : "<null>";
    BM_LOG_DEBUG("Broken on hcom ch, ch: " << ch << " usrCtx: " << usrCtx << " payLoad: " << logPayLoad);
    uint64_t payloadNum = UINT64_MAX;
    if (payLoad == nullptr || !StrUtil::String2Uint<uint64_t>(payLoad, payloadNum)) {
        BM_LOG_ERROR("Failed to get rankId payLoad: " << logPayLoad);
        return BM_ERROR;
    }

    HcomPayload payloadUn{};
    payloadUn.payload = payloadNum;
    auto server = payloadUn.serverAndEp >> HCOM_PAYLOAD_EP_SHIFT;
    auto ep = payloadUn.serverAndEp & HCOM_PAYLOAD_EP_MASK;
    BM_LOG_DEBUG("channel: " << ch << " broken from " << payloadUn.client << " to " << server << " ep: " << ep);

    auto self = HcomTransportManager::GetInstance();
    auto rankId = self->rankId_ == server ? payloadUn.client : server;
    GetInstance()->HcomChannelDisconnected(rankId, ep, ch);
    return BM_OK;
}

Result HcomTransportManager::TransportRpcHcomRequestReceived(Service_Context ctx, uint64_t usrCtx)
{
    BM_LOG_DEBUG("Receive hcom req, ctx: " << ctx << " usrCtx: " << usrCtx);
    return BM_OK;
}

Result HcomTransportManager::TransportRpcHcomRequestPosted(Service_Context ctx, uint64_t usrCtx)
{
    BM_LOG_DEBUG("Post hcom req, ctx: " << ctx << " usrCtx: " << usrCtx);
    return BM_OK;
}

Result HcomTransportManager::TransportRpcHcomOneSideDone(Service_Context ctx, uint64_t usrCtx)
{
    BM_LOG_DEBUG("Done hcom one side, ctx: " << ctx << " usrCtx: " << usrCtx);
    return BM_OK;
}

Result HcomTransportManager::ConnectHcomChannel(uint32_t rankId, uint32_t ep, const std::string &url)
{
    TP_TRACE_BEGIN(TP_SMEM_GROUP_HCOM_CONNECT_CH);
    {
        std::unique_lock<std::mutex> lock(channelMutex_[rankId]);
        if (channels_[rankId][ep] != 0) {
            BM_LOG_WARN("Stop connect to hcom service rankId: " << rankId << " ep: " << ep << " url: " << url
                                                                << " is connected");
            TP_TRACE_END(TP_SMEM_GROUP_HCOM_CONNECT_CH, 0);
            return BM_OK;
        }
    }
    Hcom_Channel channel;
    Service_ConnectOptions options;
    options.mode = C_CLIENT_WORKER_POLL;
    options.clientGroupId = 0;
    options.serverGroupId = 0;
    if (StrUtil::StartWith(url, UBC_PROTOCOL_PREFIX)) {
        options.linkCount = 1UL;
    } else {
        options.linkCount = HCOM_TRANS_EP_SIZE;
    }
    HcomPayload payload{};
    payload.client = rankId_;
    payload.serverAndEp = (rankId << HCOM_PAYLOAD_EP_SHIFT) | ep;
    auto rankIdStr = std::to_string(payload.payload);
    std::copy_n(rankIdStr.c_str(), rankIdStr.size() + 1, options.payLoad);
    do {
        auto ret = DlHcomApi::ServiceConnect(rpcServices_[ep], url.c_str(), &channel, options);
        if (ret != 0) {
            BM_LOG_ERROR("Failed to connect remote service, rankId" << rankId << " ep: " << ep << " url: " << url
                                                                    << " ret: " << ret);
            TP_TRACE_END(TP_SMEM_GROUP_HCOM_CONNECT_CH, 1);
            return BM_DL_FUNCTION_FAILED;
        }
    } while (0);
    std::unique_lock<std::mutex> lock(channelMutex_[rankId]);
    channels_[rankId][ep] = channel;
    lock.unlock();

    BM_LOG_DEBUG("Success to connect to hcom service rankId: " << rankId << " ep: " << ep << " url: " << url
                                                               << " channel: " << (void *)channel);
    TP_TRACE_END(TP_SMEM_GROUP_HCOM_CONNECT_CH, 0);
    return BM_OK;
}

void HcomTransportManager::HcomChannelDisconnected(uint32_t rankId, uint32_t ep, Hcom_Channel ch)
{
    BM_LOG_DEBUG("HcomChannelDisconnected for rank: " << rankId << " ep: " << ep << ", channel: " << ch);
    if (rankId >= channelMutex_.size() || ep >= epCount_) {
        BM_LOG_ERROR("channel disconnected with invalid rank: " << rankId << " ep: " << ep << ", channel: " << ch);
        return;
    }

    std::unique_lock<std::mutex> locker{channelMutex_[rankId]};
    channels_[rankId][ep] = 0;
    locker.unlock();
    if (rankId >= rankId_) {
        BM_LOG_TRACE("broken channel local server side:" << rankId_ << ", reconnect by remote side: " << rankId);
        return;
    }

    auto ret = reconnect_.AddReconnectTask(rankId, ep, nics_[rankId][ep]);
    if (ret != BM_OK) {
        BM_LOG_ERROR("add reconnect task for rank:" << rankId << " ep: " << ep << " failed: " << ret);
    }
}

void HcomTransportManager::DisConnectHcomChannel(uint32_t rankId, uint32_t ep, Hcom_Channel ch)
{
    BM_LOG_DEBUG("invoke DisConnectHcomChannel rankId: " << rankId << ", ep: " << ep << ", channel: " << ch);
    if (channels_.empty()) {
        return;
    }
    if (rankId >= rankCount_ || ep >= epCount_ || ch == 0) {
        BM_LOG_ERROR_LIMIT("Failed to remove channel invalid rankId" << rankId << " ep: " << ep << " ch: " << ch);
        return;
    }
    if (!GetInstance()->rpcServices_.empty()) {
        DlHcomApi::ServiceDisConnect(GetInstance()->rpcServices_[ep], ch);
    }
}

void HcomTransportManager::ClearRankChannels(uint32_t rankId)
{
    // Disconnect all endpoints of the rank, then reset channel slots under lock.
    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        if (channels_[rankId][ep] != 0) {
            DisConnectHcomChannel(rankId, ep, channels_[rankId][ep]);
        }
    }
    std::lock_guard<std::mutex> lock(channelMutex_[rankId]);
    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        channels_[rankId][ep] = 0;
    }
}

Result HcomTransportManager::ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return InnerReadRemote(rankId, lAddr, rAddr, size);
}

Result HcomTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    BM_LOG_INFO("ReadRemoteBatchAsync start : " << rankId << " size " << descriptor.counts.size());
    BM_ASSERT_LOG_AND_RETURN(!descriptor.counts.empty(), "descriptor.counts is empty", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    Hcom_Channel channel = channels_[rankId][0];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    uint32_t allBatch = descriptor.counts.size();
    auto batchs = (allBatch + HCOM_IOV_BATCH_SIZE - 1) / HCOM_IOV_BATCH_SIZE; // 向上取整
    uint32_t index = 0;
    while (index < batchs) {
        Channel_OneSideRequestSgl sglReq;
        sglReq.iovCount = 0;
        for (uint32_t i = index * HCOM_IOV_BATCH_SIZE; i < std::min(allBatch, (index + 1) * HCOM_IOV_BATCH_SIZE); ++i) {
            Channel_OneSideRequest req;
            req.lAddress = descriptor.globalAddrs[i];
            req.size = static_cast<uint32_t>(descriptor.counts[i]);
            HcomMemoryRegion mr{};
            auto ret = GetMemoryRegionByAddr(rankId_, 0, reinterpret_cast<uint64_t>(req.lAddress), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << ", size: " << req.size
                                                             << ", lAddr: " << VaToStr(req.lAddress));
                return BM_ERROR;
            }
            CopyHcomOneSideKey(mr.lKey, req.lKey);
            mr.lKey = {};
            auto rAddr = descriptor.localAddrs[i];
            ret = GetMemoryRegionByAddr(rankId, 0, reinterpret_cast<uint64_t>(rAddr), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << ", size: " << req.size
                                                             << ", rAddr: " << VaToStr(rAddr));
                return BM_ERROR;
            }
            CopyHcomOneSideKey(mr.lKey, req.rKey);
            auto offset = reinterpret_cast<uint64_t>(rAddr) - mr.addr;
            req.rAddress = reinterpret_cast<void *>(mr.lva + offset); // rewrite to remote local va
            BM_LOG_DEBUG("Try to read remote rankId: " << rankId << " channel: " << (void *)channel
                                                       << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                                       << " lAddr:" << VaToStr(req.lAddress) << " rAddr: "
                                                       << VaToStr(req.rAddress) << " size: " << descriptor.counts[i]
                                                       << " tokens: " << req.rKey.tokens[0]);
            ret = PrepareThreadLocalStream();
            if (ret != BM_OK) {
                BM_LOG_ERROR("prepare stream error rankId: " << rankId);
                return ret;
            }
            sglReq.iov[i - index * HCOM_IOV_BATCH_SIZE] = req;
            sglReq.iovCount++;
        }
        index++;
        BM_ASSERT_LOG_AND_RETURN(stream_.get() != nullptr, "stream_.get() is nullptr", BM_ERROR);
        Channel_Callback channelCallback;
        channelCallback.arg = stream_.get();
        channelCallback.cb = ChannelAsyncCallback;

        BM_LOG_INFO("ChannelGetV start, sglReq.iovCount " << sglReq.iovCount);
        stream_->SubmitTasks();
        TP_TRACE_BEGIN(TP_HYBM_HOST_RDMA_HCOM_CH_GET);
        auto ret = DlHcomApi::ChannelGetV(channel, sglReq, &channelCallback);
        TP_TRACE_END(TP_HYBM_HOST_RDMA_HCOM_CH_GET, ret);
        if (ret != 0) {
            stream_->FailedOne(false);
            Synchronize(rankId_);
            BM_LOG_ERROR("Failed to submit read task lRank:" << rankId_ << " rRank:" << rankId);
            return ret;
        }
    }
    return BM_OK;
}

Result HcomTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return InnerWriteRemote(rankId, lAddr, rAddr, size);
}

Result HcomTransportManager::GetMemoryRegionByAddr(const uint32_t &rankId, const uint32_t &ep, const uint64_t &addr,
                                                   HcomMemoryRegion &mr)
{
    std::unique_lock<std::mutex> lock(mrMutex_[rankId]);
    for (const auto &mrInfo : mrs_[rankId][ep]) {
        BM_LOG_DEBUG("Find rankId:" << rankId << " ep:" << ep << std::hex << " addr:" << mrInfo.addr
                                    << " size:" << mrInfo.size);
        if (mrInfo.addr <= addr && mrInfo.addr + mrInfo.size > addr) {
            mr = mrInfo;
            return BM_OK;
        }
    }
    return BM_ERROR;
}

int HcomTransportManager::GetCACallBack(const char *name, char **caPath, char **crlPath,
                                        Hcom_PeerCertVerifyType *verifyType, Hcom_TlsCertVerify *verify)
{
    if (caPath == nullptr || crlPath == nullptr || verifyType == nullptr || verify == nullptr) {
        BM_LOG_ERROR("Invalid input");
        return 1;
    }
    *caPath = tlsConfig_.caPath;
    *crlPath = tlsConfig_.crlPath;
    *verifyType = C_VERIFY_BY_DEFAULT;
    *verify = CertVerifyCallBack;
    return 0;
}

int HcomTransportManager::GetCertCallBack(const char *name, char **certPath)
{
    if (certPath == nullptr) {
        BM_LOG_ERROR("certPath is nullptr");
        return 1;
    }
    *certPath = tlsConfig_.certPath;
    return 0;
}

int HcomTransportManager::GetPrivateKeyCallBack(const char *name, char **priKeyPath, char **keyPass,
                                                Hcom_TlsKeyPassErase *erase)
{
    std::lock_guard<std::mutex> lock(keyPassMutex);

    if (priKeyPath == nullptr || keyPass == nullptr || erase == nullptr) {
        BM_LOG_ERROR("Invalid input");
        return 1;
    }

    *priKeyPath = tlsConfig_.keyPath;

    std::ifstream fileStream;
    fileStream.open(tlsConfig_.keyPassPath);
    if (!fileStream.is_open()) {
        BM_LOG_ERROR("Failed to open keyPassFile");
        return 1;
    }
    char encryptedKeyPass[KEYPASS_MAX_LEN] = {};
    fileStream.getline(encryptedKeyPass, KEYPASS_MAX_LEN);
    fileStream.close();
    DecryptFunc func;
    if (std::string(tlsConfig_.decrypterLibPath).empty()) {
        BM_LOG_WARN("No decrypter provided, using default decrypter handler");
        func = static_cast<DecryptFunc>(MfTlsUtil::DefaultDecrypter);
    } else {
        func = MfTlsUtil::LoadDecryptFunction(tlsConfig_.decrypterLibPath);
        if (func == nullptr) {
            BM_LOG_ERROR("failed to load customized decrypt function");
            return 1;
        }
    }
    if (func(encryptedKeyPass, KEYPASS_MAX_LEN, keyPass_, KEYPASS_MAX_LEN) != 0) {
        BM_LOG_ERROR("failed to decrypt key pass");
        return 1;
    }

    *keyPass = keyPass_;
    *erase = KeyPassEraseCallBack;

    return 0;
}

int HcomTransportManager::CertVerifyCallBack(void *x509, const char *crlPath)
{
    return 0;
}

void HcomTransportManager::KeyPassEraseCallBack(char *keyPass, int len)
{
    std::lock_guard<std::mutex> lock(keyPassMutex);
    for (int i = 0; i < len; i++) {
        keyPass[i] = 0;
    }
}

void HcomTransportManager::ChannelAsyncCallback(void *arg, Service_Context context)
{
    int res = -1;
    auto ret = DlHcomApi::ContextGetResult(context, &res);
    if (ret != 0) {
        res = ret;
    }

    auto counterStream = static_cast<HostHcomCounterStream *>(arg);
    if (res == 0) {
        counterStream->FinishOne();
    } else {
        counterStream->FailedOne();
    }
}

const TransportPrivateData HcomTransportManager::GetPrivateData() const
{
    return TransportPrivateData{};
}

void HcomTransportManager::SetHcomServiceConfig(Hcom_Service service)
{
    uint16_t cqDepth = 0;
    if (MfEnvUtil::GetUint(env::MF_HCOM_CQ_DEPTH, cqDepth)) {
        BM_LOG_INFO("Set hcom cq depth: " << cqDepth);
        DlHcomApi::ServiceSetCompletionQueueDepth(service, cqDepth);
    }

    uint32_t sqSize = 0;
    if (MfEnvUtil::GetUint(env::MF_HCOM_SQ_SIZE, sqSize)) {
        BM_LOG_INFO("Set hcom sq size: " << sqSize);
        DlHcomApi::ServiceSetSendQueueSize(service, sqSize);
    }

    uint32_t rqSize = 0;
    if (MfEnvUtil::GetUint(env::MF_HCOM_RQ_SIZE, rqSize)) {
        BM_LOG_INFO("Set hcom rq size: " << rqSize);
        DlHcomApi::ServiceSetRecvQueueSize(service, rqSize);
    }

    uint32_t prepostSize = 0;
    if (MfEnvUtil::GetUint(env::MF_HCOM_PREPOST_SIZE, prepostSize)) {
        BM_LOG_INFO("Set hcom prepost size: " << prepostSize);
        DlHcomApi::ServiceSetQueuePrePostSize(service, prepostSize);
    }

    uint32_t maxSendRecvDataCnt = 0;
    if (MfEnvUtil::GetUint(env::MF_HCOM_MAX_SEND_RECV_DATA_CNT, maxSendRecvDataCnt)) {
        BM_LOG_INFO("Set hcom max send recv data cnt: " << maxSendRecvDataCnt);
        DlHcomApi::HcomSetMaxSendRecvDataCnt(service, maxSendRecvDataCnt);
    }
}
