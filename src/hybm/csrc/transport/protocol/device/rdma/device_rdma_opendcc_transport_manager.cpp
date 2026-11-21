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
#include "device_rdma_opendcc_transport_manager.h"
#include <cstdlib>
#include "hybm_logger.h"
#include "hybm_va_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {
Result DeviceRdmaOpenDccTransportManager::ContextOptions(const TransportOptions &options, hcc_options_t &context)
{
    BM_ASSERT_LOG_AND_RETURN(options.deviceId < UINT16_MAX, "Invalid logical device ID", BM_INVALID_PARAM);
    context.provider = DEVICE_RDMA_GENETTY;
    context.logic_device_id = static_cast<uint16_t>(options.deviceId);
    return BM_OK;
}

Result DeviceRdmaOpenDccTransportManager::MemorySegment(const TransportMemoryRegion &mr, hcc_mem_segment_t &segment)
{
    segment.address = mr.addr;
    segment.size = mr.size;
    segment.type = (mr.flags & REG_MR_FLAG_HBM) != 0 ? MT_DEVICE_HBM : MT_HOST_DRAM;
    if ((mr.flags & (REG_MR_FLAG_DRAM | REG_MR_FLAG_ACL_DRAM)) != 0) {
        segment.address = HybmVaManager::GetInstance().TransformVa(mr.addr, HVM_HVA, HVM_DVA);
        BM_ASSERT_LOG_AND_RETURN(segment.address != 0 && segment.address != HVM_INVALID_VA,
                                 "Missing mapped DRAM device address: " << mr.addr, BM_INVALID_PARAM);
    }
    return BM_OK;
}
} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
#endif
