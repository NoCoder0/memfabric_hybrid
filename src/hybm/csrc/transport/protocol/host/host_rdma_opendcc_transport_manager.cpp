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
#include "host_rdma_opendcc_transport_manager.h"
#include <cstdlib>
#include "hybm_logger.h"
#include "mf_env_define.h"

namespace ock {
namespace mf {
namespace transport {
namespace host {
Result HostRdmaOpenDccTransportManager::ContextOptions(const TransportOptions &options, hcc_options_t &context)
{
    (void)options;
    context.provider = HOST_RDMA_GENETTY;
    const auto ip = env::GetEnvStr("MF_OPENDCC_RDMA_IP");
    BM_ASSERT_LOG_AND_RETURN(!ip.empty() && ip.size() < sizeof(context.ip),
                             "MF_OPENDCC_RDMA_IP must select the CX7 RoCE address", BM_INVALID_PARAM);
    std::strcpy(context.ip, ip.c_str());
    return BM_OK;
}

Result HostRdmaOpenDccTransportManager::MemorySegment(const TransportMemoryRegion &mr, hcc_mem_segment_t &segment)
{
    segment.address = mr.addr;
    segment.size = mr.size;
    segment.type = (mr.flags & REG_MR_FLAG_HBM) != 0 ? MT_DEVICE_HBM : MT_HOST_DRAM;
    BM_ASSERT_LOG_AND_RETURN((mr.flags & REG_MR_FLAG_HBM) == 0, "Host provider cannot register HBM", BM_INVALID_PARAM);
    return BM_OK;
}
} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock
#endif
