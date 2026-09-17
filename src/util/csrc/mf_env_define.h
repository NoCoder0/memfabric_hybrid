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
#ifndef MEMFABRIC_HYBRID_MF_ENV_CONFIG_H
#define MEMFABRIC_HYBRID_MF_ENV_CONFIG_H

#include <cstdlib>
#include <string>

#include "mf_out_logger.h"

namespace ock {
namespace mf {
namespace env {

static std::string GetEnvStr(const char *name, const char *deprecatedName = nullptr) noexcept
{
    if (const char *val = std::getenv(name); val != nullptr) {
        return std::string(val);
    }
    if (deprecatedName != nullptr) {
        if (const char *depVal = std::getenv(deprecatedName); depVal != nullptr) {
            MF_OUT_LOG("[MF ", WARN_LEVEL,
                       "Environment variable '" << deprecatedName << "' is deprecated, please use '" << name
                                                << "' instead.");
            return std::string(depVal);
        }
    }
    return std::string();
}

inline const std::string CUDA_HOME = GetEnvStr("CUDA_HOME");

inline const std::string ASCEND_HOME_PATH = GetEnvStr("ASCEND_HOME_PATH");
inline const std::string ASCEND_RT_VISIBLE_DEVICES = GetEnvStr("ASCEND_RT_VISIBLE_DEVICES");

inline const std::string HCOM_MAX_SLICE_SIZE = GetEnvStr("HCOM_MAX_SLICE_SIZE");
inline const std::string HCOM_RECV_DATA_SIZE = GetEnvStr("HCOM_RECV_DATA_SIZE");

inline const std::string MF_HCOM_CQ_DEPTH = GetEnvStr("MF_HCOM_CQ_DEPTH");
inline const std::string MF_HCOM_SQ_SIZE = GetEnvStr("MF_HCOM_SQ_SIZE");
inline const std::string MF_HCOM_RQ_SIZE = GetEnvStr("MF_HCOM_RQ_SIZE");
inline const std::string MF_HCOM_PREPOST_SIZE = GetEnvStr("MF_HCOM_PREPOST_SIZE");
inline const std::string MF_HCOM_MAX_SEND_RECV_DATA_CNT = GetEnvStr("MF_HCOM_MAX_SEND_RECV_DATA_CNT");
inline const std::string MF_HYBM_RDMA_SWAP_SPACE_SIZE =
    GetEnvStr("MF_HYBM_RDMA_SWAP_SPACE_SIZE", "HYBM_RDMA_SWAP_SPACE_SIZE");
// MultiRail 分流阈值(字节)：host hcom 单 service 多网卡时，超过该长度的传输才交给多 rail。
// 库默认 8192，对小包(如 1KB)等于不生效，所以这里可下调。
inline const std::string MF_HYBM_HCOM_MULTIRAIL_THRESHOLD = GetEnvStr("MF_HYBM_HCOM_MULTIRAIL_THRESHOLD");
// 是否启用 host hcom 的 MultiRail 路径（默认 1=启用）。单个 url 时也建 1 条 rail。
// 实测：不开时单笔小消息写要 ~4ms；开 MultiRail 后同样的写只要十几 us。
inline const std::string MF_HYBM_HCOM_MULTIRAIL_ENABLE = GetEnvStr("MF_HYBM_HCOM_MULTIRAIL_ENABLE");
inline const std::string MF_HYBM_URMA_SWAP_SPACE_SIZE =
    GetEnvStr("MF_HYBM_URMA_SWAP_SPACE_SIZE", "HYBM_URMA_SWAP_SPACE_SIZE");
inline const std::string MF_HYBM_RDMA_FORCE_UNREGISTERED =
    GetEnvStr("MF_HYBM_RDMA_FORCE_UNREGISTERED", "HYBM_RDMA_FORCE_UNREGISTERED");
// 把 HCOM 的忙轮询 worker 组钉到指定 CPU 段（形如 "0-1"），避免 worker 与调用线程挤在同一个核上。
// 默认空 = 不设置，worker 落核交给内核调度器。实测不设置时 worker 可能和调用线程同核，
// 被唤醒线程要排队等一个调度时间片，小消息写延迟能从十几 µs 抬到 ~4ms。
inline const std::string MF_HYBM_HCOM_WORKER_CPU_RANGE =
    GetEnvStr("MF_HYBM_HCOM_WORKER_CPU_RANGE", "HYBM_HCOM_WORKER_CPU_RANGE");
// 把 MF 侧常驻"提交 worker"（HostSubmitPool，多 rail 时每 rail 一个）钉到指定 CPU 段（形如 "99-100"）。
// 提交本身是 CPU 密集的短任务，不绑核时可能被唤醒后落到忙轮询 worker 的核上互相抢占，p99 出尖刺。
// 默认空 = 不绑，落核交给内核调度器（与加这个配置之前的行为完全一致）。
inline const std::string MF_HYBM_SUBMIT_CPU_RANGE = GetEnvStr("MF_HYBM_SUBMIT_CPU_RANGE");
// 双 rail 是否并行提交（每 rail 一个常驻 worker）。默认 0 = 串行，即改动前的行为。
// ⚠ 并行版仅"提交 worker 已绑核"时验证通过；不绑核时实测 e2e 劣化到 ~4ms/轮，且 baseline
// 出现数据校验失败（完成标志与两条 rail 的数据不在同一个 QP 上，失去了原有的保序保证）。
// 因此默认关闭，只作为实验开关：MF_HYBM_RAIL_SUBMIT_PARALLEL=1 + MF_HYBM_SUBMIT_CPU_RANGE=99-100。
inline const std::string MF_HYBM_RAIL_SUBMIT_PARALLEL = GetEnvStr("MF_HYBM_RAIL_SUBMIT_PARALLEL");
/* 多 url(多网卡) 的两种接法，默认 0：
   - 0 单 service + MultiRail（一组 ipMask 交给库，库内 CreateMultiRailDriver 选卡）；
   - 1 每张网卡一个 service（旧的双 service 形态），数据面按 ep 拆批、每 ep 一条独立 channel +
     一个线程并发提交。注意历史结论（f4c50c74）：两个各带 1 个 driver 的 service 实际可能仍走
     同一张网卡（数据面选卡按 driver 序号，ipMask 只管控制面），所以这个模式**不保证两张卡都用上**，
     它的价值是把提交面拆成两条独立 channel 以并行提交。 */
inline const std::string MF_HYBM_HCOM_DUAL_SERVICE = GetEnvStr("MF_HYBM_HCOM_DUAL_SERVICE");
// HCOMM 开关（默认关闭）：关闭时 DEVICE_RDMA 一律走 native；打开后按 CANN 版本判断
inline const std::string MF_HYBM_RDMA_USE_HCOMM = GetEnvStr("MF_HYBM_RDMA_USE_HCOMM", "HYBM_RDMA_USE_HCOMM");
inline const std::string MF_HYBM_ENABLE_4K_PAGE = GetEnvStr("MF_HYBM_ENABLE_4K_PAGE", "HYBM_ENABLE_4K_PAGE");
inline const std::string MF_LOG_LEVEL = GetEnvStr("MF_LOG_LEVEL", "ASCEND_MF_LOG_LEVEL");
inline const std::string MF_SOCKET_URL = GetEnvStr("MF_SOCKET_URL");
inline const std::string MF_TRANSPORT_MANAGER = GetEnvStr("MF_TRANSPORT_MANAGER", "TRANSPORT_MANAGER");
inline const std::string MF_CONFIG_STORE_URL = GetEnvStr("MF_CONFIG_STORE_URL", "ASCEND_MF_STORE_URL");
inline const std::string MF_CONFIG_STORE_PORT_START =
    GetEnvStr("MF_CONFIG_STORE_PORT_START", "MEMFABRIC_HYBRID_CONFIG_STORE_PORT_START");
inline const std::string MF_CONFIG_STORE_PORT_END =
    GetEnvStr("MF_CONFIG_STORE_PORT_END", "MEMFABRIC_HYBRID_CONFIG_STORE_PORT_END");
inline const std::string MF_GROUP_JOIN_MAX_TIMEOUT = GetEnvStr("MF_GROUP_JOIN_MAX_TIMEOUT");
inline const std::string MF_GROUP_RETRY_TIME = GetEnvStr("MF_GROUP_RETRY_TIME", "MF_SMEM_GROUP_RETRY_TIME");
inline const std::string MF_QP_READY_CHECK_TIMEOUT_BASE = GetEnvStr("MF_QP_READY_CHECK_TIMEOUT_BASE");
inline const std::string MF_ACC_CHECK_PERIOD_HOURS =
    GetEnvStr("MF_ACC_CHECK_PERIOD_HOURS", "ACCLINK_CHECK_PERIOD_HOURS");
inline const std::string MF_ACC_CERT_CHECK_AHEAD_DAYS =
    GetEnvStr("MF_ACC_CERT_CHECK_AHEAD_DAYS", "ACCLINK_CERT_CHECK_AHEAD_DAYS");
inline const std::string MF_DEVICE_RDMA_TC = GetEnvStr("HCCL_RDMA_TC");
inline const std::string MF_DEVICE_RDMA_SL = GetEnvStr("HCCL_RDMA_SL");

} // namespace env
} // namespace mf
} // namespace ock

#endif // MEMFABRIC_HYBRID_MF_ENV_CONFIG_H
