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

/* ubs 侧 cpu id 已从 uint8_t 放宽到 uint32_t（上限仍是 NN_NO612-1 = 611），
   所以这里也放宽到 611，让高编号 NUMA 节点（如 240-319）的核可以被绑定。
   注意 UINT32_MAX(0xFFFFFFFF) 是 ubs 内部"不绑核"的哨兵值，绝不能作为 CPU 号传入。 */
constexpr uint32_t WORKER_CPU_ID_MAX = 611;

/* 单边写的 WR 额度 = QP 的 max_send_wr − 预留，而 ubs 是**按"远端地址的连续段数"逐个扣额度**的
   （CreateOneSideCtx 里对 groupCount 每个段都调一次 GetOneSideWr）：
   600 个 4KB 离散块（stride 4096 > size 1024）就是 600 段 = 600 个额度。
   ubs 的 qpSendQueueSize 默认只有 256（hcom_c.cpp 里的 NN_NO256），于是完成回收稍慢就会报
   "no one side wr left"（内部重试 8×64µs，延迟直接飙到 ms 级）。这里把 SQ/CQ 放大留足余量：
   ubs 会向上取整到 2 的幂，合法范围 16~65535。 */
constexpr uint32_t HCOM_QP_SEND_QUEUE_SIZE = 4096;
constexpr uint16_t HCOM_QP_COMPLETION_QUEUE_DEPTH = 4096;

/* ubs 的 workerGroupCpuRange 格式是 "<起始CPU>-<结束CPU>"（含两端，单个区间），例如 "6-10"。
   同时要求"该组 CPU 数 == 该组 worker 数"（BUSY_POLLING 下严格相等），否则拒绝启动。
   返回该区间的 CPU 个数；格式不合法返回 false。 */
bool ParseWorkerCpuRange(const std::string &range, uint32_t &cpuCount)
{
    cpuCount = 0;
    auto dash = range.find('-');
    if (dash == std::string::npos) {
        return false;
    }
    uint32_t beginId = 0;
    uint32_t endId = 0;
    if (!StrUtil::String2Uint(StrUtil::StrTrim(range.substr(0, dash)), beginId) ||
        !StrUtil::String2Uint(StrUtil::StrTrim(range.substr(dash + 1)), endId) || endId < beginId ||
        endId > WORKER_CPU_ID_MAX) {
        return false;
    }
    cpuCount = endId - beginId + 1;
    return true;
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

/* MultiRail 分流阈值缺省值(字节)：库默认 8192，对小包等于不生效，这里按"单包大小"取 1024。
   可用 MF_HYBM_HCOM_MULTIRAIL_THRESHOLD 覆盖。 */
constexpr uint32_t kDefaultMultiRailThreshold = 1024;
} // namespace

hybm_tls_config HcomTransportManager::tlsConfig_ = {};
char HcomTransportManager::keyPass_[KEYPASS_MAX_LEN] = {0};
std::mutex HcomTransportManager::keyPassMutex = {};
thread_local HcomCounterStreamPtr HcomTransportManager::stream_ = nullptr;
thread_local HcomTransportManager::MrHitCache HcomTransportManager::tlsMrHit_[HcomTransportManager::MR_HIT_SLOTS];

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
    /* 可选：把 worker 组钉到指定 CPU 段，格式 "<起始CPU>-<结束CPU>"，例如 "80-80"（1 个核）。
       默认不设置（落核交给内核调度器）。不设置时忙轮询 worker 可能与调用线程落到同一个核上——
       被唤醒的线程要排队等一个调度时间片，小消息写的等待会从十几 µs 抬到 ~4ms。
       ubs 要求"该组 CPU 数 == worker 数"（BUSY_POLLING 严格相等），这里按核数自动设 worker 数；
       校验不过就只打 WARN 并跳过，绝不把服务搞挂。CPU 号须为 0-611 且该核存在。 */
    const auto &workerCpuRange = env::MF_HYBM_HCOM_WORKER_CPU_RANGE;
    if (!workerCpuRange.empty()) {
        uint32_t workerNum = 0; /* 直接按核数当 worker 数 */
        bool parsed = ParseWorkerCpuRange(workerCpuRange, workerNum);
        const uint32_t railCount = GetRailCount();
        if (parsed && railCount > 1) {
            /* 多 rail：一个 service 下有 railCount 个 driver，每个 driver 各建自己的一组 worker。
               ubs 会按 driver 序号在 CPU 段内切片（见 NetDriverRDMA::CreateWorkers），
               所以这里把核数**平均分给 railCount 个 driver**：
               例：双 rail 想每 rail 各绑 2 核 ⇒ 给 "91-94"，这里自动设成每个 driver 2 个 worker。 */
            if (workerNum < railCount || workerNum % railCount != 0) {
                BM_LOG_WARN("skip hcom worker cpu range for multi rail. range: "
                            << workerCpuRange << " 核数(" << workerNum << ") 须为 rail 数(" << railCount
                            << ")的整数倍且不小于 rail 数，例如双 rail 各绑 2 核请给 \"91-94\"");
                parsed = false;
            } else {
                workerNum /= railCount;
            }
        }
        if (!parsed || workerCpuRange.size() >= sizeof(opt.workerGroupCpuRange)) {
            BM_LOG_WARN("skip hcom worker cpu range. range: "
                        << workerCpuRange << " parsed: " << parsed
                        << " (格式须为 \"起始CPU-结束CPU\"，CPU 号须为 0-611 且该核存在)");
        } else {
            std::copy_n(workerCpuRange.c_str(), workerCpuRange.size() + 1, opt.workerGroupCpuRange);
            opt.workerGroupThreadCount = static_cast<uint16_t>(workerNum);
            BM_LOG_INFO("hcom worker group cpu range: " << opt.workerGroupCpuRange
                                                        << " threadCount: " << opt.workerGroupThreadCount
                                                        << " (per driver, railCount: " << railCount << ")");
        }
    }
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
            /* 一组 ipMask（',' 分隔）：库按它挑出本地要用的网卡，多张即多条 rail */
            DlHcomApi::ServiceSetDeviceIpMask(service, localIpMask_.c_str());
            /* threshold 默认 8192，对 1KB 小包等于不生效，这里按单包大小下调 */
            const uint32_t multiRailThresh = static_cast<uint32_t>(
                MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_HCOM_MULTIRAIL_THRESHOLD, kDefaultMultiRailThreshold));
            /* 多 url(多网卡)时必然开 MultiRail；单 url 时默认也开 ——
               实测 MultiRail 关闭时"单笔小消息写"要 ~4ms，开启后只要十几 us，
               所以这里不再跟 url 数绑定，可用 MF_HYBM_HCOM_MULTIRAIL_ENABLE=0 回退。 */
            const bool enableMultiRail =
                (MfEnvUtil::GetOptionalUintOrDefault(env::MF_HYBM_HCOM_MULTIRAIL_ENABLE, 1U) != 0U);
            DlHcomApi::ServiceSetMultiRailOptions(service, enableMultiRail, multiRailThresh);
            /* 放大单边写的 WR 额度（默认 QP 只有 256，离散大 batch 一个 iov 吃一个额度，不够用） */
            DlHcomApi::ServiceSetSendQueueSize(service, HCOM_QP_SEND_QUEUE_SIZE);
            DlHcomApi::ServiceSetCompletionQueueDepth(service, HCOM_QP_COMPLETION_QUEUE_DEPTH);
            /* 用 TRACE：本仓默认日志级别是 WARN，INFO 会被过滤掉，而这几行是验证多轨是否生效的关键 */
            BM_LOG_TRACE("[multirail-check] hcom service ipMask: " << localIpMask_ << " multiRail: " << enableMultiRail
                                                                   << " multiRailThresh: " << multiRailThresh);
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
    BumpMrGeneration(); // MR 集合变了，让各线程的免锁快路径缓存失效
    channelMutex_ = std::vector<std::mutex>(rankCount_);
    nics_ = std::vector<std::vector<std::string>>(rankCount_, std::vector<std::string>(epCount_, ""));
    channels_ = std::vector<std::vector<Hcom_Channel>>(rankCount_, std::vector<Hcom_Channel>(epCount_, 0));
    submitPool_.Start(epCount_); // 常驻拆批 worker（单链路时 1 个空转，开销可忽略）
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
    BumpMrGeneration(); // 所有 MR 都被销毁，快路径缓存必须失效

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
    BumpMrGeneration();
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
            BumpMrGeneration();
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
            BumpMrGeneration();
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
                BumpMrGeneration();
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

uint32_t HcomTransportManager::GetRailCount() const
{
    /* 建链时传了多个 url(网卡) 就是多 rail；单连接返回 1，行为与以前完全一致。 */
    return (localNics_.size() > 1) ? static_cast<uint32_t>(localNics_.size()) : 1;
}

bool HcomTransportManager::AllRailsReady(uint32_t rankId) const
{
    /* rail 共用同一个 channel（rail 数只决定库内把请求投到哪张网卡，不额外建 channel），
       所以只要 ep0 的 channel 建好就算就绪。 */
    return rankId < channels_.size() && !channels_[rankId].empty() && channels_[rankId][0] != 0;
}

bool HcomTransportManager::AllLinksReady(uint32_t rankId) const
{
    if (epCount_ == 0 || rankId >= channels_.size() || rankId >= nics_.size() ||
        channels_[rankId].size() < epCount_ || nics_[rankId].size() < epCount_) {
        return false;
    }
    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        if (channels_[rankId][ep] == 0 || nics_[rankId][ep].empty()) {
            return false;
        }
    }
    return true;
}

Result HcomTransportManager::SubmitWriteBatchOnEp(uint32_t rankId, uint32_t ep, const CopyDescriptor &descriptor,
                                                  size_t begin, size_t end)
{
    return SubmitWriteBatchSlice(rankId, ep, descriptor, begin, end);
}

Result HcomTransportManager::SubmitWriteBatchOnEpOnRail(uint32_t rankId, uint32_t ep, int32_t railIdx,
                                                        const CopyDescriptor &descriptor, size_t begin, size_t end)
{
    return SubmitWriteBatchSlice(rankId, ep, descriptor, begin, end, railIdx);
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
                BumpMrGeneration();
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
    /* stream_ 是 thread_local，纯线程私有，不存在跨线程共享，无需加锁 */
    if (stream_ == nullptr) {
        stream_ = std::make_shared<HostHcomCounterStream>(0);
    }
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
    return WriteRemoteAsyncOnEp(rankId, 0, lAddr, rAddr, size);
}

Result HcomTransportManager::WriteRemoteAsyncOnEp(uint32_t rankId, uint32_t ep, uint64_t lAddr, uint64_t rAddr,
                                                  uint64_t size)
{
    return WriteRemoteAsyncOnEpImpl(rankId, ep, lAddr, rAddr, size, -1);
}

Result HcomTransportManager::WriteRemoteAsyncOnEpOnRail(uint32_t rankId, uint32_t ep, int32_t railIdx, uint64_t lAddr,
                                                        uint64_t rAddr, uint64_t size)
{
    return WriteRemoteAsyncOnEpImpl(rankId, ep, lAddr, rAddr, size, railIdx);
}

Result HcomTransportManager::WriteRemoteAsyncOnEpImpl(uint32_t rankId, uint32_t ep, uint64_t lAddr, uint64_t rAddr,
                                                      uint64_t size, int32_t railIdx)
{
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);
    if (rankId >= channels_.size() || ep >= channels_[rankId].size()) {
        BM_LOG_ERROR("write remote with invalid rank: " << rankId << " ep: " << ep);
        return BM_INVALID_PARAM;
    }
    Hcom_Channel channel = channels_[rankId][ep];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " ep: " << ep << " is not connect");
        return BM_NOT_CONNECTED;
    }
    Channel_OneSideRequest req;
    req.rAddress = reinterpret_cast<void *>(rAddr);
    req.lAddress = reinterpret_cast<void *>(lAddr);
    req.size = static_cast<uint32_t>(size);
    HcomMemoryRegion mr{};
    auto ret = GetMemoryRegionByAddr(rankId_, ep, lAddr, mr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << " ep: " << ep << ", size: " << req.size
                                                     << ", lAddr: " << VaToInfo(lAddr));
        return BM_ERROR;
    }
    std::copy_n(mr.lKey.keys, std::size(req.lKey.keys), req.lKey.keys);
    mr.lKey = {};
    ret = GetMemoryRegionByAddr(rankId, ep, rAddr, mr);
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
        if (railIdx < 0) {
            ret = DlHcomApi::ChannelPut(channel, req, &channelCallback);
        } else {
            /* 指定 rail 时：水位必须和它所属 rail 的数据走同一条连接（靠 QP 内保序保证水位不超前）。
               单点写没有 rail 版本，所以包成 1 个 iov 的 SGL 走 ChannelPutVOnRail。 */
            Channel_OneSideRequestSgl sglReq;
            sglReq.iovCount = 1;
            sglReq.iov[0] = req;
            ret = DlHcomApi::ChannelPutVOnRail(channel, sglReq, static_cast<uint16_t>(railIdx), &channelCallback);
        }
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
Result HcomTransportManager::SubmitWriteBatchSlice(uint32_t rankId, uint32_t ep, const CopyDescriptor &descriptor,
                                                   size_t begin, size_t end, int32_t railIdx)
{
    if (rpcServices_.empty() || rankId >= channels_.size() || ep >= channels_[rankId].size()) {
        BM_LOG_WARN("SubmitWriteBatchSlice while closing, rank: " << rankId << " ep: " << ep);
        return BM_NOT_INITIALIZED;
    }
    Hcom_Channel channel = channels_[rankId][ep];
    if (channel == 0) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " ep: " << ep << " is not connect");
        return BM_NOT_CONNECTED;
    }
    /* stream_ 只与线程相关，整个 slice 准备一次即可（原先是每个 iov 调一次） */
    if (PrepareThreadLocalStream() != BM_OK) {
        BM_LOG_ERROR("prepare stream error rankId: " << rankId);
        return BM_ERROR;
    }
    size_t i = begin;
    while (i < end) {
        TP_TRACE_TRACE_BEGIN(TP_HYBM_HOST_RDMA_MR_BUILD, stageT0);
        Channel_OneSideRequestSgl sglReq;
        sglReq.iovCount = 0;
        for (; i < end && sglReq.iovCount < HCOM_IOV_BATCH_SIZE; ++i) {
            Channel_OneSideRequest req;
            req.lAddress = descriptor.localAddrs[i];
            req.size = static_cast<uint32_t>(descriptor.counts[i]);
            HcomMemoryRegion mr{};
            auto ret = GetMemoryRegionByAddr(rankId_, ep, reinterpret_cast<uint64_t>(req.lAddress), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << " ep: " << ep << ", size: " << req.size
                                                             << ", lAddr: " << VaToStr(req.lAddress));
                return BM_ERROR;
            }
            std::copy_n(mr.lKey.keys, std::size(req.lKey.keys), req.lKey.keys);
            mr.lKey = {};
            auto rAddr = descriptor.globalAddrs[i];
            ret = GetMemoryRegionByAddr(rankId, ep, reinterpret_cast<uint64_t>(rAddr), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << " ep: " << ep << ", size: " << req.size
                                                             << ", rAddr: " << VaToStr(rAddr));
                return BM_ERROR;
            }
            auto offset = reinterpret_cast<uint64_t>(rAddr) - mr.addr;
            req.rAddress = reinterpret_cast<void *>(mr.lva + offset); // rewrite to remote local va
            CopyHcomOneSideKey(mr.lKey, req.rKey);
            BM_LOG_DEBUG("Try to write remote rankId: " << rankId << " ep: " << ep
                                                        << " channel: " << (void *)channel
                                                        << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                                        << " lAddr:" << VaToStr(req.lAddress) << " rAddr: "
                                                        << VaToStr(req.rAddress) << " size: " << descriptor.counts[i]
                                                        << " tokens: " << req.rKey.tokens[0]);
            sglReq.iov[sglReq.iovCount++] = req;
        }
        TP_TRACE_TRACE_END(TP_HYBM_HOST_RDMA_MR_BUILD, stageT0, 0);
        BM_ASSERT_LOG_AND_RETURN(stream_.get() != nullptr, "stream_.get() is nullptr", BM_ERROR);
        Channel_Callback channelCallback;
        channelCallback.arg = stream_.get();
        channelCallback.cb = ChannelAsyncCallback;
        TP_TRACE_TRACE_BEGIN(TP_HYBM_HOST_RDMA_SUBMIT_TASKS, stageT1);
        stream_->SubmitTasks();
        TP_TRACE_TRACE_END(TP_HYBM_HOST_RDMA_SUBMIT_TASKS, stageT1, 0);
        TP_TRACE_TRACE_BEGIN(TP_HYBM_HOST_RDMA_CHANNEL_PUT, stageT2);
        /* railIdx >= 0：本批只走这一条 rail（双连接，不做库内 MultiRail 扇出）；
           railIdx < 0：走库内默认行为（含 MultiRail 自动扇出） */
        int ret = 0;
        if (railIdx < 0) {
            BM_LOG_DEBUG("ChannelPutV start, ep: " << ep << " sglReq iocount " << sglReq.iovCount);
            ret = DlHcomApi::ChannelPutV(channel, sglReq, &channelCallback);
        } else {
            BM_LOG_DEBUG("ChannelPutVOnRail start, rail: " << railIdx << " sglReq iocount " << sglReq.iovCount);
            ret = DlHcomApi::ChannelPutVOnRail(channel, sglReq, static_cast<uint16_t>(railIdx), &channelCallback);
        }
        TP_TRACE_TRACE_END(TP_HYBM_HOST_RDMA_CHANNEL_PUT, stageT2, ret == 0 ? 0 : 1);
        if (ret != BM_OK) {
            stream_->FailedOne(false);
            Synchronize(rankId_);
            BM_LOG_ERROR("Failed to submit put task lRank:" << rankId_ << " rRank:" << rankId << " ep: " << ep);
            return ret;
        }
    }
    return BM_OK;
}

Result HcomTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    BM_LOG_DEBUG("WriteRemoteBatchAsync start " << rankId << " rankId");
    BM_ASSERT_LOG_AND_RETURN(!descriptor.counts.empty(), "descriptor.counts is empty", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);

    uint32_t total = descriptor.counts.size();
    /* 双连接（多 rail）：把一个 batch 的 iov 连续切到多条 rail(网卡) 上，**默认行为**。
       例如 600 个 iov + 2 条 rail ⇒ rail0 拿 [0,300)、rail1 拿 [300,600)。
       单连接（只有 1 个 url）时 localNics_ 只有 1 项，自动不生效、行为与以前一致。
       rail = "同一个 channel 内的多条网卡连接"，所以与下面 epCount_>1 的多 channel 路径互斥。
       两条 rail 的提交都记在同一个线程本地 stream 上，外层一次 Synchronize 即可。 */
    if (localNics_.size() > 1) {
        const uint32_t railCount = static_cast<uint32_t>(localNics_.size());
        size_t begin = 0;
        for (uint32_t rail = 0; rail < railCount; ++rail) {
            const size_t cnt = total / railCount + (rail < total % railCount ? 1 : 0);
            if (cnt == 0) {
                continue;
            }
            auto ret = SubmitWriteBatchSlice(rankId, 0, descriptor, begin, begin + cnt, static_cast<int32_t>(rail));
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to submit rail " << rail << " rankId: " << rankId);
                Synchronize(rankId);
                return ret;
            }
            begin += cnt;
        }
        return BM_OK;
    }
    // single link keeps original submit + outer synchronize behavior
    if (epCount_ <= 1) {
        return SubmitWriteBatchSlice(rankId, 0, descriptor, 0, total);
    }

    // multi link: only eps with a ready channel can carry data
    std::vector<uint32_t> activeEps;
    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        if (!nics_[rankId][ep].empty() && channels_[rankId][ep] != 0) {
            activeEps.emplace_back(ep);
        }
    }
    if (activeEps.empty()) {
        BM_LOG_WARN("Unable to write remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    if (activeEps.size() == 1) {
        return SubmitWriteBatchSlice(rankId, activeEps[0], descriptor, 0, total);
    }

    // split iov evenly across active eps and submit via resident worker pool concurrently;
    // inactive ep slots get a no-op task to keep task count == worker count
    std::vector<std::function<Result()>> tasks(epCount_);
    size_t base = total / activeEps.size();
    size_t rem = total % activeEps.size();
    size_t begin = 0;
    size_t activeIdx = 0;
    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        bool active = !nics_[rankId][ep].empty() && channels_[rankId][ep] != 0;
        if (!active) {
            tasks[ep] = []() { return BM_OK; };
            continue;
        }
        size_t end = begin + base + (activeIdx < rem ? 1 : 0);
        uint32_t slotEp = ep;
        // 拷贝 descriptor 到任务闭包：pool worker 执行期可能晚于调用方释放
        tasks[ep] = [this, rankId, slotEp, descriptor, begin, end]() {
            auto ret = SubmitWriteBatchSlice(rankId, slotEp, descriptor, begin, end);
            if (ret == BM_OK && stream_ != nullptr) {
                ret = stream_->Synchronize(static_cast<int32_t>(rankId));
            }
            return ret;
        };
        begin = end;
        ++activeIdx;
    }
    return submitPool_.RunTasks(std::move(tasks));
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
    localNics_ = std::move(epNics);
    localIps_ = std::move(epIps);
    /* options.nic 里的多个 url = "本地要多用几张网卡"，但**不能**因此建多个 service：
       ubs-comm 明确要求上层禁止同时创建同种协议的 2 个不同 Service 实例
       （service_ctx_store.h 里 GetOrReturn 的注释：否则 Service2 会引用 Service1 的内存池），
       而 HCOM 原生支持一个 service 带多条 rail（MultiRail：库内 CreateMultiRailDriver 按 ipMask
       选出多张卡建多个 driver，上限 MAX_ENABLE_DEVCOUNT=4，并自动分流）。
       所以这里把多 url 收敛成：1 个 service + 1 个 oob 监听 url + 一组 ipMask；多网卡交给库。
       传输层的 epCount_ 因此恒为 1，数据面自动走单链路路径。 */
    localIpMask_.clear();
    for (const auto &ip : localIps_) { /* ',' 分隔的一组 mask（ubs_hcom_service_set_ipmask 按 ',' 拆） */
        localIpMask_ = localIpMask_.empty() ? (ip + "/32") : (localIpMask_ + "," + ip + "/32");
    }
    epCount_ = 1;
    /* 向上层发布**全部** url（';' 分隔，与 compose_transport_manager 的 NIC_DELIMITER 一致）：
       compose 层会把每个 url 广播成一个 host# 段，对端据此为**每条 rail** 建连。
       这里只发第一个会让对端永远建不起第二条 rail，MultiRail 退化成单链接。 */
    localNic_.clear();
    for (const auto &url : localNics_) {
        if (url.empty()) {
            continue;
        }
        if (!localNic_.empty()) {
            localNic_ += ';';
        }
        localNic_ += url;
    }
    localIp_ = localIps_[0];
    BM_LOG_TRACE("[multirail-check] hcom single service: ipMask(" << localIpMask_ << ") url-count(" << localNics_.size()
                                                                  << ") listen(" << localNic_ << ")");
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

Result HcomTransportManager::SubmitReadBatchSlice(uint32_t rankId, uint32_t ep, const CopyDescriptor &descriptor,
                                                  size_t begin, size_t end)
{
    if (rpcServices_.empty() || rankId >= channels_.size() || ep >= channels_[rankId].size()) {
        BM_LOG_WARN("SubmitReadBatchSlice while closing, rank: " << rankId << " ep: " << ep);
        return BM_NOT_INITIALIZED;
    }
    Hcom_Channel channel = channels_[rankId][ep];
    if (channel == 0) {
        BM_LOG_WARN("Unable to read remote, rankId: " << rankId << " ep: " << ep << " is not connect");
        return BM_NOT_CONNECTED;
    }
    /* stream_ 只与线程相关，整个 slice 准备一次即可（原先是每个 iov 调一次） */
    if (PrepareThreadLocalStream() != BM_OK) {
        BM_LOG_ERROR("prepare stream error rankId: " << rankId);
        return BM_ERROR;
    }
    size_t i = begin;
    while (i < end) {
        Channel_OneSideRequestSgl sglReq;
        sglReq.iovCount = 0;
        for (; i < end && sglReq.iovCount < HCOM_IOV_BATCH_SIZE; ++i) {
            Channel_OneSideRequest req;
            req.lAddress = descriptor.globalAddrs[i];
            req.size = static_cast<uint32_t>(descriptor.counts[i]);
            HcomMemoryRegion mr{};
            auto ret = GetMemoryRegionByAddr(rankId_, ep, reinterpret_cast<uint64_t>(req.lAddress), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find lKey, rankId: " << rankId_ << " ep: " << ep << ", size: " << req.size
                                                             << ", lAddr: " << VaToStr(req.lAddress));
                return BM_ERROR;
            }
            CopyHcomOneSideKey(mr.lKey, req.lKey);
            mr.lKey = {};
            auto rAddr = descriptor.localAddrs[i];
            ret = GetMemoryRegionByAddr(rankId, ep, reinterpret_cast<uint64_t>(rAddr), mr);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to find rKey, rankId: " << rankId << " ep: " << ep << ", size: " << req.size
                                                             << ", rAddr: " << VaToStr(rAddr));
                return BM_ERROR;
            }
            CopyHcomOneSideKey(mr.lKey, req.rKey);
            auto offset = reinterpret_cast<uint64_t>(rAddr) - mr.addr;
            req.rAddress = reinterpret_cast<void *>(mr.lva + offset); // rewrite to remote local va
            BM_LOG_DEBUG("Try to read remote rankId: " << rankId << " ep: " << ep
                                                       << " channel: " << (void *)channel
                                                       << " lKey:" << req.lKey.keys[0] << " rKey: " << req.rKey.keys[0]
                                                       << " lAddr:" << VaToStr(req.lAddress) << " rAddr: "
                                                       << VaToStr(req.rAddress) << " size: " << descriptor.counts[i]
                                                       << " tokens: " << req.rKey.tokens[0]);
            sglReq.iov[sglReq.iovCount++] = req;
        }
        BM_ASSERT_LOG_AND_RETURN(stream_.get() != nullptr, "stream_.get() is nullptr", BM_ERROR);
        Channel_Callback channelCallback;
        channelCallback.arg = stream_.get();
        channelCallback.cb = ChannelAsyncCallback;

        BM_LOG_INFO("ChannelGetV start, ep: " << ep << " sglReq.iovCount " << sglReq.iovCount);
        stream_->SubmitTasks();
        TP_TRACE_BEGIN(TP_HYBM_HOST_RDMA_HCOM_CH_GET);
        auto ret = DlHcomApi::ChannelGetV(channel, sglReq, &channelCallback);
        TP_TRACE_END(TP_HYBM_HOST_RDMA_HCOM_CH_GET, ret);
        if (ret != 0) {
            stream_->FailedOne(false);
            Synchronize(rankId_);
            BM_LOG_ERROR("Failed to submit read task lRank:" << rankId_ << " rRank:" << rankId << " ep: " << ep);
            return ret;
        }
    }
    return BM_OK;
}

Result HcomTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    BM_LOG_INFO("ReadRemoteBatchAsync start : " << rankId << " size " << descriptor.counts.size());
    BM_ASSERT_LOG_AND_RETURN(!descriptor.counts.empty(), "descriptor.counts is empty", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(!rpcServices_.empty(), "rpcServices_.size() = " << rpcServices_.size(), BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(rankId < rankCount_, "rankId = " << rankId << " << rankCount_ = " << rankCount_,
                             BM_INVALID_PARAM);

    uint32_t total = descriptor.counts.size();
    // single link keeps original submit + outer synchronize behavior
    if (epCount_ <= 1) {
        return SubmitReadBatchSlice(rankId, 0, descriptor, 0, total);
    }

    // multi link: only eps with a ready channel can carry data
    std::vector<uint32_t> activeEps;
    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        if (!nics_[rankId][ep].empty() && channels_[rankId][ep] != 0) {
            activeEps.emplace_back(ep);
        }
    }
    if (activeEps.empty()) {
        BM_LOG_WARN("Unable to read remote, rankId: " << rankId << " is not connect");
        return BM_NOT_CONNECTED;
    }
    if (activeEps.size() == 1) {
        return SubmitReadBatchSlice(rankId, activeEps[0], descriptor, 0, total);
    }

    // split iov evenly across active eps and submit via resident worker pool concurrently;
    // inactive ep slots get a no-op task to keep task count == worker count
    std::vector<std::function<Result()>> tasks(epCount_);
    size_t base = total / activeEps.size();
    size_t rem = total % activeEps.size();
    size_t begin = 0;
    size_t activeIdx = 0;
    for (uint32_t ep = 0; ep < epCount_; ++ep) {
        bool active = !nics_[rankId][ep].empty() && channels_[rankId][ep] != 0;
        if (!active) {
            tasks[ep] = []() { return BM_OK; };
            continue;
        }
        size_t end = begin + base + (activeIdx < rem ? 1 : 0);
        uint32_t slotEp = ep;
        // 拷贝 descriptor 到任务闭包：pool worker 执行期可能晚于调用方释放
        tasks[ep] = [this, rankId, slotEp, descriptor, begin, end]() {
            auto ret = SubmitReadBatchSlice(rankId, slotEp, descriptor, begin, end);
            if (ret == BM_OK && stream_ != nullptr) {
                ret = stream_->Synchronize(static_cast<int32_t>(rankId));
            }
            return ret;
        };
        begin = end;
        ++activeIdx;
    }
    return submitPool_.RunTasks(std::move(tasks));
}

Result HcomTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return InnerWriteRemote(rankId, lAddr, rAddr, size);
}

Result HcomTransportManager::GetMemoryRegionByAddr(const uint32_t &rankId, const uint32_t &ep, const uint64_t &addr,
                                                   HcomMemoryRegion &mr)
{
    if (rankId >= mrs_.size() || ep >= mrs_[rankId].size()) {
        BM_LOG_ERROR("query mr with invalid rank: " << rankId << " ep: " << ep);
        return BM_ERROR;
    }
    const uint64_t gen = mrGen_.load(std::memory_order_acquire);
    /* 快路径：代际未变 + 同 rank/ep + 地址仍落在上次命中的 MR 内 → 免锁直接返回。
       批量写场景里数百个地址通常都落在同一对 MR 里，命中率接近 100%，
       因此把“每个 iov 2 次加锁遍历”降到整批 1~2 次。多槽位是为了让"本端/远端"各自有槽，
       否则交替查询会互相击穿缓存。 */
    MrHitCache &hit = tlsMrHit_[MrHitSlot(rankId, ep)];
    if (hit.self == this && hit.gen == gen && hit.rankId == rankId && hit.ep == ep && hit.mr.addr <= addr &&
        hit.mr.addr + hit.mr.size > addr) {
        mr = hit.mr;
        return BM_OK;
    }
    std::unique_lock<std::mutex> lock(mrMutex_[rankId]);
    for (const auto &mrInfo : mrs_[rankId][ep]) {
        BM_LOG_DEBUG("Find rankId:" << rankId << " ep:" << ep << std::hex << " addr:" << mrInfo.addr
                                    << " size:" << mrInfo.size);
        if (mrInfo.addr <= addr && mrInfo.addr + mrInfo.size > addr) {
            mr = mrInfo;
            hit = MrHitCache{this, gen, rankId, ep, mrInfo};
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
