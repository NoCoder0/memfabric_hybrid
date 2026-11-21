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
#pragma once
#ifdef BUILD_OPENDCC
#include "opendcc/opendcc_transport_manager.h"
namespace ock {
namespace mf {
namespace transport {
namespace host {
class HostRdmaOpenDccTransportManager final : public OpenDccTransportManager {
protected:
    Result ContextOptions(const TransportOptions &options, hcc_options_t &context) override;
    Result MemorySegment(const TransportMemoryRegion &mr, hcc_mem_segment_t &segment) override;
};
} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock
#endif
