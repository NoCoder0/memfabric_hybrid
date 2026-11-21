/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include "genetty_types.h"

#include "genetty_info_types.h"

namespace ock {
namespace openDCC {
std::ostream &operator<<(std::ostream &os, GenettyCQPollingType pollingType) noexcept
{
    switch (pollingType) {
        case GCQ_TYPE_EVENT:
            return os << "event";
        case GCQ_TYPE_BUSY:
            return os << "busy";
        default:
            break;
    }

    return os << "unknown";
}

std::ostream &operator<<(std::ostream &os, genetty_type_t type) noexcept
{
    switch (type) {
        case HOST_RDMA_GENETTY:
            return os << "host_rdma";
        case HOST_URMA_GENETTY:
            return os << "host_urma";
        case DEVICE_RDMA_GENETTY:
            return os << "device_rdma";
        case DEVICE_URMA_GENETTY:
            return os << "device_urma";
        case DEVICE_INITIATED_RDMA_GENETTY:
            return os << "device_initiated_rdma";
        case DEVICE_INITIATED_URMA_GENETTY:
            return os << "device_initiated_urma";
        default:
            break;
    }

    return os << "unknown";
}

std::string ProtocolStr(PROTOCOL protocol) noexcept
{
    std::string str;
    if ((protocol & GEN_PROT_HOST_RDMA) == GEN_PROT_HOST_RDMA) {
        str += "host_rdma|";
    }

    if ((protocol & GEN_PROT_HOST_URMA) == GEN_PROT_HOST_URMA) {
        str += "host_urma|";
    }

    if ((protocol & GEN_PROT_DEVICE_RDMA) == GEN_PROT_DEVICE_RDMA) {
        str += "device_rdma|";
    }

    if ((protocol & GEN_PROT_DEVICE_URMA) == GEN_PROT_DEVICE_URMA) {
        str += "device_urma|";
    }

    if ((protocol & GEN_PROT_DEVICE_INITIATED_RDMA) == GEN_PROT_DEVICE_INITIATED_RDMA) {
        str += "device_initiated_rdma|";
    }

    if ((protocol & GEN_PROT_DEVICE_INITIATED_URMA) == GEN_PROT_DEVICE_INITIATED_URMA) {
        str += "device_initiated_urma|";
    }

    if (str.empty()) {
        return protocol == 0 ? "none" : "unknown";
    }

    str.pop_back(); /* remove the last separator */
    return str;
}

std::string CtxPolicyStr(CONTEXT_POLICY policy) noexcept
{
    std::string str;
    if ((policy & GEN_CTX_POLICY_NUMA_AFFINITY) == GEN_CTX_POLICY_NUMA_AFFINITY) {
        str += "numa_affinity|";
    }

    if ((policy & GEN_CTX_POLICY_DEVICE_BALANCED) == GEN_CTX_POLICY_DEVICE_BALANCED) {
        str += "device_balanced|";
    }

    if (str.empty()) {
        return policy == 0 ? "none" : "unknown";
    }

    str.pop_back(); /* remove the last separator */
    return str;
}

std::ostream &operator<<(std::ostream &os, genetty_mem_type_t memType) noexcept
{
    switch (memType) {
        case MT_HOST_DRAM:
            return os << "host_dram";
        case MT_DEVICE_HBM:
            return os << "device_hbm";
        default:
            break;
    }

    return os << "unknown";
}

std::ostream &operator<<(std::ostream &os, GennettyCtxGetPolicy policy) noexcept
{
    switch (policy) {
        case GCCP_INVALID:
            return os << "invalid";
        case GCCP_BY_DEVICE_ID:
            return os << "by_device_id";
        case GCCP_BY_IP:
            return os << "by_ip";
        case GCCP_AUTO:
            return os << "auto";
        default:
            break;
    }

    return os << "unknown";
}

std::ostream &operator<<(std::ostream &os, const GenettyCtxGetOptions &options) noexcept
{
    os << "genetty ctx get options [type: " << options.genettyType << ", policy: " << options.type
       << ", device id: " << options.deviceId << ", ip mask: " << options.ipMask << ", flags: " << options.flags
       << ", which auto policy: " << CtxPolicyStr(options.whichAutoPolicy) << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const GenettyCQOptions &options) noexcept
{
    os << "genetty cq options [event fd: ";
    if (options.eventFd == UINT32_MAX) {
        os << "internal";
    } else {
        os << options.eventFd;
    }

    os << ", queue depth: " << options.queueDepth << ", flags: " << options.flags
       << ", vendor: " << GetVendorType(options.vendor) << ", polling type: " << options.polling_type
       << ", device id: " << options.deviceId << ", preferred ip mask: " << options.preferred_ip_mask
       << ", lock free: " << static_cast<uint32_t>(options.lockFree) << ", context: " << options.context << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const GenettyOptions &options) noexcept
{
    os << "genetty options [cq: " << (options.cq.Get() != nullptr)
       << ", sq depth: " << options.sqDepth
       << ", rq depth: " << options.rqDepth << ", flags: " << options.flags
       << ", vendor: " << GetVendorType(options.vendor)
       << ", lock free: " << static_cast<uint32_t>(options.lockFree) << ", service level: " << options.serviceLevel
       << ", traffic class: " << options.trafficClass << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const GenettyBackendOptions &options) noexcept
{
    os << "genetty backend options [protocol: " << ProtocolStr(options.protocol)
       << ", vendor: " << GetVendorType(options.vendor) << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const genetty_cq_options_t &options) noexcept
{
    os << "genetty cq options [event fd: " << options.event_fd << ", queue depth: " << options.queue_depth
       << ", flags: " << options.flags << ", preferred ip mask: " << options.preferred_ip_mask
       << ", lock free: " << static_cast<uint32_t>(options.lock_free) << ", context: " << options.context << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const genetty_options_t &options) noexcept
{
    os << "genetty options [cq: " << options.cq_handle
       << ", sq depth: " << options.sq_depth << ", rq depth: " << options.rq_depth << ", flags: " << options.flags
       << ", lock free: " << static_cast<uint32_t>(options.lock_free) << ", service level: " << options.service_level
       << ", traffic class: " << options.traffic_class << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const genetty_batch_op_options_t &options) noexcept
{
    os << "genetty batch op options [usr ctx: 0x" << std::hex << options.usr_ctx << std::dec << ", imm: " << options.imm
       << ", op type: " << static_cast<int32_t>(options.op_type)
       << ", signal: " << static_cast<uint32_t>(options.signal)
       << ", kick db: " << static_cast<uint32_t>(options.kick_db) << ", flags: " << options.flags << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const genetty_batch_op_data_t &data) noexcept
{
    os << "genetty batch op data [local: " << data.laddress << ", local size: " << data.lsize << ", lkey: " << data.lkey
       << ", remote: " << data.raddress << ", remote size: " << data.rsize << ", rkey: " << data.rkey << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const genetty_memory_segment_t &segment) noexcept
{
    os << "genetty memory segment [type: " << segment.type << ", address: 0x" << std::hex << segment.address
       << std::dec << ", size: " << segment.size << ", lkey: " << segment.lkey << ", rkey: " << segment.rkey << "]";
    return os;
}

Result GenettyBackend::MemInfoEncode(genetty_memory_segment_t *segment, uint32_t flags, genetty_memory_info_t *info)
{
    GEN_ASSERT_MSG_RETURN(segment != nullptr, "Invalid param, segment is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Invalid param, info is null", GEN_INVALID_PARAM);

    MemoryInfo memoryInfo;
    memoryInfo.Address(segment->address);
    memoryInfo.Size(segment->size);
    memoryInfo.MemType(segment->type);
    memoryInfo.Rkey(segment->lkey);

    auto result = memoryInfo.Export(info, flags);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Encode memory info failed, result(" << result << "), mem type(" << segment->type << "), address("
                                                           << segment->address << "), lkey(" << segment->lkey << ")");
        return result;
    }

    GEN_LOG_DEBUG("Encode memory info finished, mem type(" << segment->type << "), address(" << segment->address
                                                           << "), size(" << segment->size << "), lkey("
                                                           << segment->lkey << ")");
    return GEN_OK;
}

Result GenettyBackend::MemInfoDecode(genetty_memory_info_t *info, genetty_memory_segment_t *segment, uint32_t flags)
{
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Invalid param, info is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(segment != nullptr, "Invalid param, segment is null", GEN_INVALID_PARAM);

    MemoryInfo memoryInfo;
    auto result = memoryInfo.Import(info, flags);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Decode memory info failed, result(" << result << "), length(" << info->length << ")");
        return result;
    }

    segment->type = memoryInfo.MemType();
    segment->address = memoryInfo.Address();
    segment->size = memoryInfo.Size();
    segment->rkey = memoryInfo.Rkey();

    GEN_LOG_DEBUG("Decode memory info finished, mem type(" << segment->type << "), address(" << segment->address
                                                           << "), size(" << segment->size << "), rkey("
                                                           << segment->rkey << ")");
    return GEN_OK;
}

} // namespace openDCC
} // namespace ock
