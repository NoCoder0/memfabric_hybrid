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
#ifndef MEM_FABRIC_HYBRID_HYBM_COMMON_INCLUDE_H
#define MEM_FABRIC_HYBRID_HYBM_COMMON_INCLUDE_H

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>

#include "hybm_big_mem.h"
#include "hybm_define.h"
#include "hybm_functions.h"
#include "hybm_logger.h"
#include "hybm_types.h"
#include "hybm_gva_version.h"
#include "hybm_networks_common.h"
#include "mf_file_util.h"

int32_t HybmGetInitDeviceId(void);

bool HybmHasInited(void);

/* ubs C-API 调用耗时累计（线程本地）：把"ubs 函数内部（同步段）"从"MF 纯软件"里剥出来。
   用法：
   - batch 层（HostDataOpRDMA::BatchDataCopy）打开 enabled 并 Reset()，退出时 Get() 打成取值打点；
   - under_api 层（DlHcomApi 里的单边读/写调用）在每个 ubs 调用前后 Begin/End 累加。
   enabled 为 false 时 Begin() 立即返回 0、End(0) 直接返回，只有一次 TLS 判断的开销。 */
namespace ock {
namespace mf {
namespace ubs_call_time {
inline thread_local bool enabled = false;
inline thread_local uint64_t ns = 0;

inline uint64_t NowNs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline uint64_t Begin() noexcept
{
    return enabled ? NowNs() : 0;
}

inline void End(uint64_t beginNs) noexcept
{
    if (beginNs != 0) {
        ns += NowNs() - beginNs;
    }
}

inline void Reset() noexcept
{
    ns = 0;
}

inline uint64_t Get() noexcept
{
    return ns;
}
} // namespace ubs_call_time
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_COMMON_INCLUDE_H
