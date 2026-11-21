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
#include "hybm_entity_default.h"
#include "mf_opendcc_config.h"
#ifdef BUILD_OPENDCC
#include "dl_opendcc_api.h"
#endif

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <sstream>
#include <cstdlib>
#include <string_view>

#include "compose_transport_manager.h"
#include "vendor/vendor_api.h"
#include "dl_api.h"
#include "vendor/ascend/dl_hal_api.h"

#include "hybm_compose_data_op.h"
#include "hybm_data_op_host_rdma.h"
#include "hybm_data_op_host_shm.h"
#include "hybm_conn_based_segment.h"
#include "hybm_ex_info_transfer.h"
#include "device_rdma_transport_manager.h"
#include "rdma_provider_control_server.h"
#include "rdma_proxy_transport_manager.h"
#include "provider_service.h"
#include "urma_provider_control_server.h"
#include "device_urma_transport_manager.h"
#include "hybm_gva.h"
#include "hybm_logger.h"
#include "hybm_ptracer.h"
#include "hybm_va_manager.h"
#include "mf_fault_injection_point.h"

namespace ock {
namespace mf {

namespace {
struct UnreleasedTransport {
    std::shared_ptr<transport::device::ProviderService> server;
    MemSegmentPtr dram;
    transport::TransManagerPtr transport;
    DataOperatorPtr dataOperator;
    MemSegmentPtr hbm;
    std::unordered_map<void *, std::shared_ptr<void>> allocations;
};

// Final destruction has no retry caller. Keep the backing alive while the device may still access it.
void RetainUnreleasedTransport(UnreleasedTransport resources)
{
    static std::mutex mutex;
    static auto *retained = new std::vector<UnreleasedTransport>();
    std::lock_guard<std::mutex> lock(mutex);
    retained->push_back(std::move(resources));
}
} // namespace

MemEntityDefault::MemEntityDefault(int id) noexcept : id_(id), initialized_(false) {}

MemEntityDefault::~MemEntityDefault()
{
    ReleaseResources();
}

Result MemEntityDefault::InitTagManager()
{
    BM_ASSERT_LOG_AND_RETURN(tagManager_ == nullptr, "tagManager_ is not nullptr", BM_OK);
    tagManager_ = std::make_shared<HybmEntityTagInfo>();
    BM_ASSERT_LOG_AND_RETURN(tagManager_->TagInfoInit(options_) == BM_OK, "Failed to init tagManager",
                             BM_INVALID_PARAM);
    uint32_t localSuperPodId = 0U;
    uint32_t localServerId = 0U;
    int32_t localDevicePhyId = -1;
    MemSegment::GetLocalPhysicalInfo(localSuperPodId, localServerId, localDevicePhyId);
    tagManager_->SetRankTopology(options_.rankId, localSuperPodId, localServerId, localDevicePhyId);
    auto layerProtocols = tagManager_->GetLayersForRank(options_.rankId);
    if (!layerProtocols.empty()) {
        uint32_t layerOpMask = 0U;
        for (auto layer : layerProtocols) {
            layerOpMask |= static_cast<uint32_t>(layer);
        }
        options_.bmDataOpType = static_cast<hybm_data_op_type>(layerOpMask);
        BM_LOG_DEBUG("Layer protocol overrides data op type, rankId:"
                     << options_.rankId << " dataOpType:" << layerOpMask << " layerCount:" << layerProtocols.size());
        return BM_OK;
    }
    BM_LOG_TRACE("Success to init tag manager legacy data op type:" << options_.bmDataOpType
                                                                    << " rankId:" << options_.rankId);
    return BM_OK;
}

int32_t MemEntityDefault::Initialize(const hybm_options *options) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(!initialized_, "the object is initialized.", BM_OK);
    BM_ASSERT_LOG_AND_RETURN((id_ >= 0 && (uint32_t)(id_) < HYBM_ENTITY_NUM_MAX),
                             "input entity id is invalid, input: " << id_
                                                                   << " must be less than: " << HYBM_ENTITY_NUM_MAX,
                             BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(CheckOptions(options) == BM_OK, "options is invalid.", BM_INVALID_PARAM);
    options_ = *options;
    if ((options_.flags & HYBM_FLAG_CREATE_WITH_SHM) == 0) {
        options_.dramShmFd = -1;
    }

    // init tag info
    BM_ASSERT_LOG_AND_RETURN(InitTagManager() == BM_OK, "Failed to init tag manager.", BM_ERROR);
    // load dlopen lib
    BM_ASSERT_LOG_AND_RETURN(LoadExtendLibrary() == BM_OK, "Failed to load extend library.", BM_ERROR);
    // init segment
    BM_ASSERT_LOG_AND_RETURN(InitSegment() == BM_OK, "Failed to init segment.", BM_ERROR);
    // init transManager
    BM_ASSERT_LOG_AND_RETURN(InitTransManager() == BM_OK, "Failed to init trans manager.", BM_ERROR);
    // init dataOperator
    BM_ASSERT_LOG_AND_RETURN(InitDataOperator() == BM_OK, "Failed to init data operator.", BM_ERROR);

    initialized_ = true;
    BM_LOG_INFO("Initialize success: " << options_);
    return BM_OK;
}

void MemEntityDefault::UnInitialize() noexcept
{
    ReleaseResources();
}

int32_t MemEntityDefault::QueryMemorySpace(uint64_t &address) noexcept
{
    uint64_t size = options_.maxDRAMSize * options_.rankCount;
    address = HybmVaManager::GetInstance().QueryReserveGva(size);
    return address == 0 ? BM_ERROR : BM_OK;
}

int32_t MemEntityDefault::ReserveMemorySpaceAt(uint64_t address) noexcept
{
    dramSegment_->SetReserveAddress(address);
    auto ret = ReserveMemorySpace();
    dramSegment_->SetReserveAddress(0);
    return ret;
}

int32_t MemEntityDefault::ReserveMemorySpace() noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_NOT_INITIALIZED;
    }

    if (hbmSegment_ != nullptr) {
        auto ret = hbmSegment_->ReserveMemorySpace(&hbmGva_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to reserver HBM memory space ret: " << ret);
            return ret;
        }
        if (dataOperator_) {
            dataOperator_->UpdateGvaSpace(HYBM_MEM_TYPE_DEVICE, (uint64_t)hbmGva_, options_.maxHBMSize,
                                          options_.rankCount);
        }
    }

    if (dramSegment_ != nullptr) {
        auto ret = dramSegment_->ReserveMemorySpace(&dramGva_);
        if (ret != BM_OK) {
            UnReserveMemorySpace();
            BM_LOG_ERROR("Failed to reserver DRAM memory space ret: " << ret);
            return ret;
        }
        if (providerServer_) {
            auto conn = std::dynamic_pointer_cast<HybmConnBasedSegment>(dramSegment_);
            if (conn == nullptr || !conn->IsHostProvider()) {
                UnReserveMemorySpace();
                BM_LOG_ERROR("provider segment has no host shm.");
                return BM_ERROR;
            }
            if (!providerServer_->RegisterMemory(conn->GetHostShmBase(), conn->GetHostShmSize(),
                                                 conn->GetProviderGvaBase(options_.rankId), options_.rankId)) {
                providerServer_->Stop();
                if (providerServer_->CleanupComplete()) {
                    UnReserveMemorySpace();
                }
                BM_LOG_ERROR("provider register memory failed.");
                return BM_ERROR;
            }
            if (!providerServer_->Start()) {
                BM_LOG_ERROR("provider control server failed to start");
                return BM_ERROR;
            }
        }
        if (dataOperator_) {
            dataOperator_->UpdateGvaSpace(HYBM_MEM_TYPE_HOST, (uint64_t)dramGva_, options_.maxDRAMSize,
                                          options_.rankCount);
        }
    }

    return BM_OK;
}

int32_t MemEntityDefault::UnReserveMemorySpace() noexcept
{
    if (!initialized_) {
        return BM_OK;
    }

    if (providerServer_ != nullptr) {
        providerServer_->Stop();
        if (!providerServer_->CleanupComplete()) {
            BM_LOG_ERROR("provider cleanup incomplete before releasing memory, entityId:" << id_);
            return BM_ERROR;
        }
    }

    if (hbmSegment_ != nullptr) {
        hbmSegment_->UnReserveMemorySpace();
    }
    if (dramSegment_ != nullptr) {
        dramSegment_->UnReserveMemorySpace();
    }
    return BM_OK;
}

int32_t MemEntityDefault::AllocLocalMemory(uint64_t size, hybm_mem_type mType, uint32_t flags,
                                           hybm_mem_slice_t &slice) noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_NOT_INITIALIZED;
    }

    if ((size % HYBM_LARGE_PAGE_SIZE) != 0) {
        BM_LOG_ERROR("allocate memory size: " << size << " invalid, page size is: " << HYBM_LARGE_PAGE_SIZE);
        return BM_INVALID_PARAM;
    }

    auto segment = mType == HYBM_MEM_TYPE_DEVICE ? hbmSegment_ : dramSegment_;
    if (segment == nullptr) {
        BM_LOG_ERROR("allocate memory with mType: " << mType << ", no segment.");
        return BM_INVALID_PARAM;
    }

    MemSlicePtr realSlice;
    Result ret = BM_OK;
    FIP_START(ALLOC_LOCAL_MEMORY, &ret)
    ret = segment->AllocLocalMemory(size, realSlice);
    FIP_END;
    if (ret != 0) {
        BM_LOG_ERROR("segment allocate slice with size: " << size << " failed: " << ret);
        return ret;
    }

    slice = realSlice->ConvertToId();
    transport::TransportMemoryRegion info;
    info.addr = realSlice->vAddress_;
    info.size = realSlice->size_;
    info.flags =
        segment->GetMemoryType() == HYBM_MEM_TYPE_DEVICE ? transport::REG_MR_FLAG_HBM : transport::REG_MR_FLAG_DRAM;
    constexpr auto deviceTransportMask = HYBM_DOP_TYPE_DEVICE_TRANSPORT_MASK;
    const bool skipHostShmDeviceRegistration = mType == HYBM_MEM_TYPE_HOST &&
                                               (options_.bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) != 0U &&
                                               (options_.bmDataOpType & deviceTransportMask) != 0U;
    if (skipHostShmDeviceRegistration) {
        BM_LOG_DEBUG("Skip host shm DRAM registration for device transport, rankId:" << options_.rankId
                                                                                     << " size:" << size);
    }
    if (transportManager_ != nullptr && size > 0 && !skipHostShmDeviceRegistration &&
        !(options_.bmDataOpType & HYBM_DOP_TYPE_AIV_SDMA)) {
        ret = transportManager_->RegisterMemoryRegion(info);
        if (ret != 0) {
            BM_LOG_ERROR("register memory region allocate failed: " << ret << ", info: " << info);
            auto res = segment->ReleaseSliceMemory(realSlice);
            if (res != BM_OK) {
                BM_LOG_ERROR("failed to release slice memory: " << res);
            }
            return ret;
        }
    }

    return UpdateHybmDeviceInfo(0);
}

Result MemEntityDefault::RetainHostMemory(const void *address, uint64_t size) noexcept
{
    try {
        std::shared_ptr<void> owner;
        const auto ret = MemSegment::RetainHostMemory(address, size, owner);
        if (ret != BM_OK || !owner) {
            return ret;
        }
        std::lock_guard<std::mutex> lock(managedAllocationsMutex_);
        managedAllocations_.emplace(owner.get(), owner);
        return BM_OK;
    } catch (const std::exception &error) {
        BM_LOG_ERROR("Retain Fabric Host allocation failed, entityId: " << id_ << " size_bytes: " << size
                                                                        << " reason: " << error.what());
        return BM_MALLOC_FAILED;
    }
}

int32_t MemEntityDefault::RegisterLocalMemory(const void *ptr, uint64_t size, uint32_t flags,
                                              hybm_mem_slice_t &slice) noexcept
{
    if (ptr == nullptr || size == 0 || size > TB) {
        BM_LOG_ERROR("input ptr or size(" << size << ") is invalid");
        return BM_INVALID_PARAM;
    }

    auto addr = static_cast<uint64_t>(reinterpret_cast<ptrdiff_t>(ptr));
    std::shared_ptr<MemSegment> segment = nullptr;
    if (options_.scene == HYBM_SCENE_TRANS || dramSegment_ == nullptr) {
        segment = hbmSegment_;
    } else {
        segment = dramSegment_;
    }
    BM_VALIDATE_RETURN(segment != nullptr, "address for segment is null.", BM_INVALID_PARAM);

    auto ret = RetainHostMemory(ptr, size);
    if (ret != BM_OK) {
        return ret;
    }
    MemSlicePtr realSlice;
    ret = segment->RegisterMemory(ptr, size, realSlice);
    if (ret != 0) {
        BM_LOG_ERROR("segment register slice with size: " << size << " failed: " << ret);
        return ret;
    }

    bool isHbm = realSlice->GetMemoryType() == HYBM_MEM_TYPE_DEVICE;
    BM_LOG_DEBUG("Hbm: " << isHbm << std::hex << ", addrs: 0x" << addr);

    if (transportManager_ != nullptr) {
        transport::TransportMemoryRegion mr;
        // Transport managers use HVA as the registration key. Device RDMA obtains DVA from VaManager when needed;
        // pure SDMA does not register an MR here.
        mr.addr = realSlice->vAddress_;
        mr.size = realSlice->size_;
        mr.flags = (isHbm ? transport::REG_MR_FLAG_HBM : transport::REG_MR_FLAG_DRAM);
        ret = transportManager_->RegisterMemoryRegion(mr);
        if (ret != 0) {
            BM_LOG_ERROR("register MR: " << mr << " to transport failed: " << ret);
            auto relRet = segment->ReleaseSliceMemory(realSlice);
            if (relRet != BM_OK) {
                BM_LOG_ERROR("rollback ReleaseSliceMemory after MR register failed, ret:" << relRet);
            }
            return ret;
        }
    }

    slice = realSlice->ConvertToId();
    return BM_OK;
}

int32_t MemEntityDefault::FreeLocalMemory(hybm_mem_slice_t slice, uint32_t flags) noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_INVALID_PARAM;
    }
    HybmVaManager::GetInstance().DumpReservedGvaInfo();
    HybmVaManager::GetInstance().DumpAllocatedGvaInfo();
    MemSlicePtr memSlice;
    bool fromHbm = false;
    if (hbmSegment_ != nullptr && (memSlice = hbmSegment_->GetMemSlice(slice, true)) != nullptr) {
        fromHbm = true;
    } else if (dramSegment_ != nullptr && (memSlice = dramSegment_->GetMemSlice(slice)) != nullptr) {
        fromHbm = false;
    } else {
        memSlice = nullptr;
    }

    if (memSlice == nullptr) {
        BM_LOG_ERROR("FreeLocalMemory: cannot resolve slice id="
                     << slice << ", neither hbmSegment_ nor dramSegment_ owns it; VaManager entry may leak");
        return BM_OK;
    }

    if (transportManager_ != nullptr && memSlice->size_ > 0 && !(options_.bmDataOpType & HYBM_DOP_TYPE_AIV_SDMA)) {
        auto ret = transportManager_->UnregisterMemoryRegion(memSlice->vAddress_);
        if (ret != BM_OK) {
            BM_LOG_ERROR("UnregisterMemoryRegion failed, please check input slice.");
            if (OpenDccSelected(options_) || ((options_.bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) != 0U &&
                                              (options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_URMA) != 0U)) {
                return ret;
            }
        }
    }

    Result relRet = BM_OK;
    if (fromHbm) {
        relRet = hbmSegment_->ReleaseSliceMemory(memSlice);
    } else {
        relRet = dramSegment_->ReleaseSliceMemory(memSlice);
    }
    if (relRet != BM_OK) {
        BM_LOG_ERROR("ReleaseSliceMemory failed, ret:" << relRet);
    }
    return BM_OK;
}

// 按 flags 取 slice 的本地虚拟地址（flags=0）或 GVA（flags=HYBM_GET_SLICE_GVA）
static void *GetSliceAddrByFlags(const MemSlice &memSlice, uint32_t flags)
{
    if (flags == 0U) {
        return reinterpret_cast<void *>(memSlice.vAddress_);
    }
    if (memSlice.gva_ == 0) {
        BM_LOG_ERROR("slice gva is 0, slice index:" << memSlice.index_ << " memType:" << memSlice.memType_);
        return nullptr;
    }
    return reinterpret_cast<void *>(memSlice.gva_);
}

void *MemEntityDefault::GetSliceVa(hybm_mem_slice_t slice, uint32_t flags)
{
    if (flags != 0U && flags != HYBM_GET_SLICE_GVA) {
        BM_LOG_ERROR("invalid flags:" << flags << " slice:" << slice);
        return nullptr;
    }
    std::shared_ptr<MemSlice> memSlice;
    if (hbmSegment_ != nullptr && (memSlice = hbmSegment_->GetMemSlice(slice, true)) != nullptr) {
        return GetSliceAddrByFlags(*memSlice, flags);
    } else if (dramSegment_ != nullptr && (memSlice = dramSegment_->GetMemSlice(slice)) != nullptr) {
        return GetSliceAddrByFlags(*memSlice, flags);
    }

    BM_LOG_ERROR("failed to get slice va, invalid slice:" << slice);
    return nullptr;
}

uint64_t MemEntityDefault::GetSliceSize(hybm_mem_slice_t slice)
{
    std::shared_ptr<MemSlice> memSlice;
    if (hbmSegment_ != nullptr && (memSlice = hbmSegment_->GetMemSlice(slice, true)) != nullptr) {
        return memSlice->size_;
    } else if (dramSegment_ != nullptr && (memSlice = dramSegment_->GetMemSlice(slice)) != nullptr) {
        return memSlice->size_;
    }

    BM_LOG_ERROR("failed to get slice size, invalid slice:" << slice);
    return 0;
}

uint16_t MemEntityDefault::LocalPhyDevId() const noexcept
{
    // The ACL logic device id is node-unique (e.g. 0-7 on an 8-card node) and identical for the same physical
    // NPU on both nodes, so it can be used to pair proxies across nodes. Fall back to devId when unavailable.
    int32_t logicId = HybmGetInitedLogicDeviceId();
    if (logicId < 0 || logicId > static_cast<int32_t>(std::numeric_limits<uint16_t>::max())) {
        return static_cast<uint16_t>(options_.devId);
    }
    return static_cast<uint16_t>(logicId);
}

namespace {
// Upper bound for the expansion of MEMFABRIC_PROVIDER_DEVICES, so a typo like "0-4294967295" stays bounded.
constexpr uint32_t kMaxProviderDeviceIds = 1024U;

// std::strtoul accepts " 3", "+3", "3abc" and returns 0 for garbage, which would silently pick the wrong NPU.
bool ParseProviderDeviceId(std::string_view text, uint32_t &id)
{
    if (text.empty() || text.size() > 10U) {
        return false;
    }
    uint32_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<uint32_t>(ch - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    id = value;
    return true;
}

bool ParseProviderDeviceToken(std::string_view token, std::vector<uint32_t> &ids)
{
    // Keeps the caller's list bounded no matter how many comma-separated ids the environment holds.
    if (ids.size() >= kMaxProviderDeviceIds) {
        return false;
    }
    const auto dash = token.find('-');
    uint32_t begin = 0;
    uint32_t end = 0;
    if (dash == std::string_view::npos) {
        if (!ParseProviderDeviceId(token, begin)) {
            return false;
        }
        ids.push_back(begin);
        return true;
    }
    if (!ParseProviderDeviceId(token.substr(0, dash), begin) || !ParseProviderDeviceId(token.substr(dash + 1), end) ||
        begin > end) {
        return false;
    }
    if (end - begin >= kMaxProviderDeviceIds || end - begin + 1U > kMaxProviderDeviceIds - ids.size()) {
        return false;
    }
    // 用不会回绕的计数器迭代：begin == end == UINT32_MAX 时 ++id 会回绕成 0，id <= end 恒成立。
    for (uint64_t id = begin; id <= static_cast<uint64_t>(end); ++id) {
        ids.push_back(static_cast<uint32_t>(id));
    }
    return true;
}

std::vector<uint32_t> ProviderDeviceIds()
{
    std::vector<uint32_t> ids;
    const char *env = std::getenv("MEMFABRIC_PROVIDER_DEVICES");
    if (env != nullptr && *env != '\0') {
        const std::string spec(env);
        std::stringstream ss(spec);
        std::string token;
        while (std::getline(ss, token, ',')) {
            std::vector<uint32_t> parsed;
            if (!ParseProviderDeviceToken(token, parsed)) {
                // Fail closed: falling back to every NPU would take over cards owned by another provider instance.
                BM_LOG_ERROR("invalid MEMFABRIC_PROVIDER_DEVICES token '["
                             << token << "]', expect \"0,1\" or \"0-7\" with ids <= " << kMaxProviderDeviceIds
                             << ", device list ignored.");
                return {};
            }
            // 单个 token 的解析结果各自受 kMaxProviderDeviceIds 限制，合并后的总数必须单独把关，
            // 否则 "0,1,...,n" 这类多 token 配置可以突破文档承诺的上限。
            if (ids.size() + parsed.size() > kMaxProviderDeviceIds) {
                BM_LOG_ERROR("MEMFABRIC_PROVIDER_DEVICES lists more than " << kMaxProviderDeviceIds
                                                                           << " device ids, device list ignored.");
                return {};
            }
            for (auto id : parsed) {
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
                    ids.push_back(id);
                }
            }
        }
        if (!ids.empty()) {
            return ids;
        }
        BM_LOG_ERROR("MEMFABRIC_PROVIDER_DEVICES is set but lists no device id, refusing to bind every NPU.");
        return {};
    }
    uint32_t count = 0;
    if (VendorApi::Api()->GetDeviceCount(&count) == BM_OK && count > 0) {
        ids.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            ids[i] = i;
        }
    } else {
        ids.push_back(0);
    }
    return ids;
}
} // namespace

namespace {
constexpr size_t MAX_TRANSPORT_KEY_WORDS = transport::TRANS_KEY_MAX_WORDS;

int AppendEntityExportInfo(ExchangeInfoWriter &desc, const EntityExportInfo &info)
{
    int ret = desc.Append(info.magic);
    ret = (ret != 0) ? ret : desc.Append(info.rankId);
    ret = (ret != 0) ? ret : desc.Append(info.phyDevId);
    ret = (ret != 0) ? ret : desc.Append(info.nic, sizeof(info.nic));
    ret = (ret != 0) ? ret : desc.Append(info.layerCount);
    ret = (ret != 0) ? ret : desc.Append(info.layers, sizeof(info.layers));
    ret = (ret != 0) ? ret : desc.Append(info.superPodId);
    ret = (ret != 0) ? ret : desc.Append(info.serverId);
    ret = (ret != 0) ? ret : desc.Append(info.devicePhyId);
    ret = (ret != 0) ? ret : desc.Append(info.transportPrivateData.ip, sizeof(info.transportPrivateData.ip));
    return (ret != 0) ? ret : desc.Append(info.transportPrivateData.key.keys);
}

int ReadEntityExportInfo(const ExchangeInfoReader &reader, EntityExportInfo &info)
{
    int ret = reader.Read(info.magic);
    ret = (ret != 0) ? ret : reader.Read(info.rankId);
    ret = (ret != 0) ? ret : reader.Read(info.phyDevId);
    ret = (ret != 0) ? ret : reader.Read(info.nic, sizeof(info.nic));
    ret = (ret != 0) ? ret : reader.Read(info.layerCount);
    ret = (ret != 0) ? ret : reader.Read(info.layers, sizeof(info.layers));
    ret = (ret != 0) ? ret : reader.Read(info.superPodId);
    ret = (ret != 0) ? ret : reader.Read(info.serverId);
    ret = (ret != 0) ? ret : reader.Read(info.devicePhyId);
    ret = (ret != 0) ? ret : reader.Read(info.transportPrivateData.ip, sizeof(info.transportPrivateData.ip));
    return (ret != 0) ? ret : reader.Read(info.transportPrivateData.key.keys, MAX_TRANSPORT_KEY_WORDS);
}

int AppendSliceTransportKey(ExchangeInfoWriter &desc, const SliceExportTransportKey &key)
{
    int ret = desc.Append(key.rankId);
    ret = (ret != 0) ? ret : desc.Append(key.reserved, sizeof(key.reserved));
    ret = (ret != 0) ? ret : desc.Append(key.address);
    return (ret != 0) ? ret : desc.Append(key.key.keys);
}

int ReadSliceTransportKey(const ExchangeInfoReader &reader, SliceExportTransportKey &key)
{
    int ret = reader.Read(key.rankId);
    ret = (ret != 0) ? ret : reader.Read(key.reserved, sizeof(key.reserved));
    ret = (ret != 0) ? ret : reader.Read(key.address);
    return (ret != 0) ? ret : reader.Read(key.key.keys, MAX_TRANSPORT_KEY_WORDS);
}
} // namespace

// entityExchangeInfo = entityInfo + segmentInfo
int32_t MemEntityDefault::ExportEntityExchangeInfo(ExchangeInfoWriter &desc, uint32_t flags) noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_NOT_INITIALIZED;
    }

    EntityExportInfo exportInfo{};
    exportInfo.rankId = options_.rankId;
    std::vector<hybm_data_op_type> layerProtocols;
    if (tagManager_ != nullptr) {
        layerProtocols = tagManager_->GetLayersForRank(options_.rankId);
    }
    if (layerProtocols.size() > EXPORT_INFO_MAX_LAYER_COUNT) {
        BM_LOG_ERROR("protocol layer count:" << layerProtocols.size() << " exceeds limit:"
                                             << EXPORT_INFO_MAX_LAYER_COUNT << " rank:" << options_.rankId);
        return BM_INVALID_PARAM;
    }
    exportInfo.layerCount = static_cast<uint32_t>(layerProtocols.size());
    std::copy(layerProtocols.begin(), layerProtocols.end(), exportInfo.layers);
    MemSegment::GetLocalPhysicalInfo(exportInfo.superPodId, exportInfo.serverId, exportInfo.devicePhyId);
    // phyDevId is the node-unique device id, used by a proxy to connect only to same-device peers.
    exportInfo.phyDevId = LocalPhyDevId();
    if (providerServer_ != nullptr) {
        const auto &url = providerServer_->GetControlUrl();
        if (url.size() >= sizeof(exportInfo.nic)) {
            BM_LOG_ERROR("provider control url(" << url << ") too long.");
            return BM_ERROR;
        }
        std::copy_n(url.c_str(), url.size(), exportInfo.nic);
        BM_LOG_DEBUG("provider control url:" << url);
    } else if (transportManager_ != nullptr) {
        auto &nic = transportManager_->GetNic();
        if (nic.size() >= sizeof(exportInfo.nic)) {
            BM_LOG_ERROR("transport get nic(" << nic << ") too long.");
            return BM_ERROR;
        }
        size_t copyLen = std::min(nic.size(), sizeof(exportInfo.nic));
        std::copy_n(nic.c_str(), copyLen, exportInfo.nic);
        const auto privateData = transportManager_->GetPrivateData();
        exportInfo.transportPrivateData = privateData;
        BM_LOG_DEBUG("transport get nic:" << nic << ", ip: " << privateData.ip);
    }
    auto ret = AppendEntityExportInfo(desc, exportInfo);
    if (ret != 0) {
        BM_LOG_ERROR("export entity exchange info failed: " << ret);
        return BM_ERROR;
    }

    if (hbmSegment_ == nullptr) {
        if (options_.scene == HYBM_SCENE_TRANS) {
            BM_LOG_ERROR("hbm segment is null, failed to export segment info in trans scene");
            return BM_ERROR;
        } else {
            BM_LOG_DEBUG("hbm segment is null, skip export segment info");
            return BM_OK;
        }
    }

    std::string segInfo;
    ret = hbmSegment_->Export(segInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("failed to export segment info in trans scene, ret: " << ret);
        return BM_ERROR;
    }
    ret = desc.Append(segInfo.data(), segInfo.size());
    if (ret != 0) {
        BM_LOG_ERROR("export to string wrong size: " << segInfo.size());
        return BM_ERROR;
    }
    return BM_OK;
}

// sliceExchangeInfo = segmentInfo + transportInfo
int32_t MemEntityDefault::ExportSliceExchangeInfo(hybm_mem_slice_t slice, ExchangeInfoWriter &desc,
                                                  uint32_t flags) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(initialized_, "the entity is not initialized", BM_NOT_INITIALIZED);
    if (flags & HYBM_FLAG_EXPORT_ENTITY) {
        return ExportEntityExchangeInfo(desc, flags);
    }

    uint64_t exportMagic = 0;
    std::string info;
    MemSlicePtr realSlice;
    std::shared_ptr<MemSegment> currentSegment;
    if (hbmSegment_ != nullptr) {
        realSlice = hbmSegment_->GetMemSlice(slice, true);
        currentSegment = hbmSegment_;
    }
    if (realSlice == nullptr && dramSegment_ != nullptr) {
        realSlice = dramSegment_->GetMemSlice(slice);
        currentSegment = dramSegment_;
    }
    if (realSlice == nullptr) {
        BM_LOG_ERROR("cannot find input slice for export.");
        return BM_INVALID_PARAM;
    }

    auto ret = currentSegment->Export(realSlice, info);
    if (ret != 0) {
        BM_LOG_ERROR("export to string failed: " << ret);
        return ret;
    }
    // 导出块首 8 字节即段内写入的 magic（与 Import 端 demux 约定一致），据此填充 transport key；
    // asymmetric 段统一写 USER_MEM magic，池内 HBM/DRAM 段写各自 magic，无需按介质分支
    if (info.size() >= sizeof(exportMagic)) {
        std::memcpy(&exportMagic, info.data(), sizeof(exportMagic));
    }
    ret = desc.Append(info.data(), info.size());
    if (ret != 0) {
        BM_LOG_ERROR("export to string wrong size: " << info.size());
        return BM_ERROR;
    }

    SliceExportTransportKey transportKey{exportMagic, options_.rankId, realSlice->gva_};
    if (transportManager_ != nullptr && !(options_.bmDataOpType & HYBM_DOP_TYPE_AIV_SDMA)) {
        if (realSlice->size_ > 0) {
            ret = transportManager_->QueryMemoryKey(realSlice->vAddress_, transportKey.key);
            if (ret != 0) {
                BM_LOG_WARN("query memory key failed, export zero key. addr: 0x" << std::hex << realSlice->vAddress_
                                                                                 << std::dec << ", ret: " << ret);
                transportKey.key = {};
            } else {
                const uint64_t sliceGva = realSlice->gva_;
                if (sliceGva != 0) {
                    // trans scene: the slice already holds the paired GVA, so overwrite the address inside the key
                    // directly. A peer-side reverse HVA lookup would otherwise yield an "owner GVA" differing from
                    // the local rebuilt GVA sent by the data plane. Non-trans segments have gva_ == 0, a no-op there.
                    transportManager_->UpdateMemoryKey(transportKey.key,
                                                       reinterpret_cast<void *>(static_cast<uintptr_t>(sliceGva)));
                }
            }
        }
        ret = AppendSliceTransportKey(desc, transportKey);
        if (ret != 0) {
            BM_LOG_ERROR("append transport key failed: " << ret);
            return ret;
        }
    }

    if (options_.scene != HYBM_SCENE_TRANS) {
        BM_LOG_DEBUG("Success to export slice rankId:" << transportKey.rankId << " addr:" << transportKey.address
                                                       << " key:" << transportKey.key);
    } else {
        BM_LOG_DEBUG("Success to export slice rankId:" << options_.rankId << " addr:" << realSlice->vAddress_);
    }
    return BM_OK;
}

int32_t MemEntityDefault::ImportForSegment(const ExchangeInfoReader desc[], uint32_t count, void *addresses[],
                                           uint32_t overrideRank, std::vector<ImportEntryKind> &entryKinds) noexcept
{
    entryKinds.assign(count, ImportEntryKind::IgnoredProxy);

    if (desc[0].LeftBytes() == 0) {
        BM_LOG_DEBUG("no segment need import.");
        return BM_OK;
    }

    std::vector<std::string> dramInfos;
    std::vector<std::string> userSegInfos;
    std::vector<uint32_t> dramIndex;
    std::vector<uint32_t> userSegIndex;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t magic;
        if (desc[i].Test(magic) < 0) {
            BM_LOG_ERROR("left import data no magic size. idx:" << i);
            return BM_OK;
        }

        char tmp[UNIFIED_EXCHANGE_SEG_INFO_SIZE];
        if (IsDramSlice(magic)) {
            desc[i].Read(reinterpret_cast<void *>(tmp), UNIFIED_EXCHANGE_SEG_INFO_SIZE);
            dramInfos.emplace_back(tmp, UNIFIED_EXCHANGE_SEG_INFO_SIZE);
            dramIndex.emplace_back(i);
        } else if (IsHbmSlice(magic) || IsUserSlice(magic)) {
            desc[i].Read(reinterpret_cast<void *>(tmp), UNIFIED_EXCHANGE_SEG_INFO_SIZE);
            userSegInfos.emplace_back(tmp, UNIFIED_EXCHANGE_SEG_INFO_SIZE);
            userSegIndex.emplace_back(i);
        }
    }

    if (!dramInfos.empty()) {
        std::vector<void *> dramAddrs(dramInfos.size(), nullptr);
        BM_ASSERT_LOG_AND_RETURN(dramSegment_ != nullptr, "dramSegment is nullptr", BM_ERROR);
        std::vector<ImportEntryKind> dramResults(dramInfos.size(), ImportEntryKind::PeerSlice);
        auto ret = dramSegment_->Import(dramInfos, dramAddrs.data(), overrideRank, dramResults);
        if (ret != BM_OK) {
            BM_LOG_ERROR("dram segment import infos failed: " << ret);
            return ret;
        }
        if (dramResults.size() != dramInfos.size()) {
            BM_LOG_ERROR("dram import results size mismatch, results: " << dramResults.size()
                                                                        << ", infos: " << dramInfos.size());
            return BM_ERROR;
        }
        for (uint32_t i = 0; i < dramInfos.size(); i++) {
            entryKinds[dramIndex[i]] = dramResults[i];
            if (addresses != nullptr) {
                addresses[dramIndex[i]] = dramAddrs[i];
            }
        }
    }

    if (!userSegInfos.empty()) {
        std::vector<void *> userSegAddrs(userSegInfos.size(), nullptr);
        BM_ASSERT_LOG_AND_RETURN(hbmSegment_ != nullptr, "hbmSegment is nullptr", BM_ERROR);
        std::vector<ImportEntryKind> userSegResults(userSegInfos.size(), ImportEntryKind::PeerSlice);
        auto ret = hbmSegment_->Import(userSegInfos, userSegAddrs.data(), overrideRank, userSegResults);
        if (ret != BM_OK) {
            // P2P 未就绪是可重试的瞬时状态，根因已在 segment 层记 WARN，这里不重复打印
            if (ret != BM_P2P_NOT_READY) {
                BM_LOG_ERROR("hbm segment import infos failed: " << ret);
            }
            return ret;
        }
        if (userSegResults.size() != userSegInfos.size()) {
            BM_LOG_ERROR("hbm import results size mismatch, results: " << userSegResults.size()
                                                                       << ", infos: " << userSegInfos.size());
            return BM_ERROR;
        }
        for (uint32_t i = 0; i < userSegInfos.size(); i++) {
            entryKinds[userSegIndex[i]] = userSegResults[i];
            if (addresses != nullptr) {
                addresses[userSegIndex[i]] = userSegAddrs[i];
            }
        }
    }
    return BM_OK;
}

int32_t MemEntityDefault::ImportSliceExchangeInfo(const ExchangeInfoReader desc[], uint32_t count, void *addresses[],
                                                  uint32_t flags) noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_NOT_INITIALIZED;
    }

    auto ret = SetThreadAclDevice();
    if (ret != BM_OK) {
        return BM_ERROR;
    }

    if (desc == nullptr) {
        BM_LOG_ERROR("the input desc is nullptr.");
        return BM_ERROR;
    }

    // trans 场景 rank 覆盖：flags 中解码本地分配的覆盖 rank，0 表示不覆盖
    const uint32_t overrideRank =
        HYBM_IMPORT_RANK_ENABLED(flags) ? HYBM_IMPORT_RANK_DECODE(flags) : HYBM_IMPORT_RANK_INVALID;
    std::vector<ImportEntryKind> entryKinds;

    ret = ImportForSegment(desc, count, addresses, overrideRank, entryKinds);
    if (ret != BM_OK) {
        return ret;
    }

    auto connSegment = std::dynamic_pointer_cast<HybmConnBasedSegment>(dramSegment_);
    if (connSegment != nullptr && connSegment->IsHostProxy() && transportManager_ != nullptr) {
        transport::HybmTransPrepareOptions prepareOptions;
        for (const auto &item : connSegment->GetImportedProviders()) {
            if (item.first == connSegment->GetLocalProviderRank()) {
                continue;
            }
            auto rankIt = importedRanks_.find(item.first);
            if (rankIt == importedRanks_.end() || rankIt->second.nic[0] == '\0') {
                BM_LOG_ERROR("provider entity or control URL missing, providerRank:" << item.first);
                return BM_NOT_INITIALIZED;
            }
            transport::TransportRankPrepareInfo info;
            info.isProvider = true;
            info.providerControlUrl = std::string(rankIt->second.nic);
            info.providerGvaBase = connSegment->GetProviderGvaBase(item.first);
            prepareOptions.options[item.first] = std::move(info);
        }
        if (!prepareOptions.options.empty()) {
            ret = transportManager_->Prepare(prepareOptions);
            if (ret != BM_OK) {
                BM_LOG_ERROR("prepare proxy links failed: " << ret);
                return ret;
            }
        }
    }

    // transport要在segment之后import
    if (transportManager_ != nullptr && !(options_.bmDataOpType & HYBM_DOP_TYPE_AIV_SDMA)) {
        ret = ImportForTransportPrecheck(desc, count, entryKinds, addresses, overrideRank);
        if (ret != BM_OK) {
            return ret;
        }

        ret = ImportForTransport();
        if (ret != BM_OK) {
            return ret;
        }
    }

    return BM_OK;
}

int32_t MemEntityDefault::ImportForLayerProtocols()
{
    const auto localLayers = tagManager_->GetLayersForRank(options_.rankId);
    const uint32_t localCount = static_cast<uint32_t>(localLayers.size());
    for (const auto &item : importedRanks_) {
        const auto rankId = item.first;
        const auto &info = item.second;
        if (info.layerCount != localCount) {
            BM_LOG_ERROR("tag info template mismatch, local rank:" << options_.rankId << " localLayerCount:"
                                                                   << localCount << " peer rank:" << rankId
                                                                   << " peerLayerCount:" << info.layerCount);
            return BM_INVALID_PARAM;
        }
        if (info.layerCount == 0U) {
            continue;
        }
        if (info.layerCount > EXPORT_INFO_MAX_LAYER_COUNT) {
            BM_LOG_ERROR("peer layer count:" << info.layerCount << " exceeds limit:" << EXPORT_INFO_MAX_LAYER_COUNT
                                             << " local rank:" << options_.rankId << " peer rank:" << rankId);
            return BM_INVALID_PARAM;
        }
        tagManager_->SetRankTopology(rankId, info.superPodId, info.serverId, info.devicePhyId);
        const std::vector<hybm_data_op_type> layers(info.layers, info.layers + info.layerCount);
        auto ret = tagManager_->AddRankLayers(rankId, layers);
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to add rank layers, rankId:" << rankId << " layerCount:" << info.layerCount);
            return ret;
        }
    }
    return BM_OK;
}

int32_t MemEntityDefault::ConnectTransport(const uint32_t *ranks, uint32_t count, uint32_t flags) noexcept
{
    (void)flags;
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized");
        return BM_NOT_INITIALIZED;
    }
    if (transportManager_ == nullptr || (options_.bmDataOpType & HYBM_DOP_TYPE_AIV_SDMA)) {
        return BM_OK;
    }
    auto conn = std::dynamic_pointer_cast<HybmConnBasedSegment>(dramSegment_);
    const auto providers =
        conn != nullptr ? conn->GetImportedProviders() : std::unordered_map<uint32_t, HostExportInfo>{};
    for (uint32_t i = 0; i < count; ++i) {
        if (conn != nullptr && conn->IsHostProxy() &&
            (ranks[i] == conn->GetLocalProviderRank() || providers.find(ranks[i]) == providers.end())) {
            continue;
        }
        auto ret = transportManager_->ConnectRank(ranks[i]);
        if (dataOperator_ != nullptr) {
            const auto connectedProtocols = transportManager_->GetConnectedProtocols(ranks[i]);
            dataOperator_->CacheRankProtocols(ranks[i], connectedProtocols);
        }
        if (ret != BM_OK) {
            BM_LOG_ERROR("Failed to connect transport, rankId:" << ranks[i] << ", ret:" << ret);
            return ret;
        }
    }
    return BM_OK;
}

int32_t MemEntityDefault::ImportForTransportManager()
{
    if (transportManager_ == nullptr || (options_.bmDataOpType & HYBM_DOP_TYPE_AIV_SDMA)) {
        BM_LOG_DEBUG("no transport, no need import.");
        return BM_OK;
    }
    int32_t ret = BM_ERROR;
    transport::HybmTransPrepareOptions prepareOptions;
    for (const auto &item : importedRanks_) {
        auto &info = item.second;
        // Peers without a transport (e.g. a bare provider that only owns shared memory) export an empty nic;
        // they cannot be connected over the network and must be skipped.
        if (info.nic[0] == '\0') {
            continue;
        }
        // A proxy only connects (device RDMA) to peers on the same node-unique phyDevId.
        if (options_.clusterRole == HYBM_CLUSTER_ROLE_PROXY && info.phyDevId != LocalPhyDevId()) {
            continue;
        }
        transport::TransportRankPrepareInfo prepareInfo;
        prepareInfo.nic = info.nic;
        prepareInfo.privateData = info.transportPrivateData;
        prepareOptions.options.emplace(info.rankId, std::move(prepareInfo));
    }

    {
        std::lock_guard<std::mutex> lock(transportMutex_);
        if (transportPrepared_) {
            ret = transportManager_->UpdateRankOptions(prepareOptions);
        } else {
            ret = transportManager_->Prepare(prepareOptions);
            if (ret != BM_OK) {
                BM_LOG_ERROR("Failed to prepare transport connect data, ret: " << ret);
                return ret;
            }
            transportPrepared_ = true;
        }
    }

    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "Failed to Connect transport: " << ret, ret);
    return 0;
}

int32_t MemEntityDefault::ImportEntityExchangeInfo(const ExchangeInfoReader desc[], uint32_t count,
                                                   uint32_t flags) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(initialized_, "initialized_ = " << initialized_, BM_NOT_INITIALIZED);
    BM_ASSERT_LOG_AND_RETURN(desc != nullptr, "desc is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(SetThreadAclDevice() == BM_OK, "SetThreadAclDevice failed", BM_ERROR);

    // trans 场景 rank 覆盖：flags 使能时用导入方本地按对端分配的 rank 替换导出端 entity rank，
    // 无效值（UINT32_MAX）表示不覆盖
    const uint32_t overrideRank =
        HYBM_IMPORT_RANK_ENABLED(flags) ? HYBM_IMPORT_RANK_DECODE(flags) : HYBM_IMPORT_RANK_INVALID;

    std::vector<EntityExportInfo> deserializedInfos(count);
    for (auto i = 0U; i < count; i++) {
        auto ret = ReadEntityExportInfo(desc[i], deserializedInfos[i]);
        if (ret != 0) {
            BM_LOG_ERROR("deserialize imported info(" << i << ") failed.");
            return BM_INVALID_PARAM;
        }
        if (overrideRank != HYBM_IMPORT_RANK_INVALID) {
            deserializedInfos[i].rankId = overrideRank;
        }
    }
    {
        std::unique_lock<std::mutex> uniqueLock{importMutex_};
        for (auto &deserializedInfo : deserializedInfos) {
            importedRanks_[deserializedInfo.rankId] = deserializedInfo;
        }
    }
    BM_ASSERT_LOG_AND_RETURN(ImportForLayerProtocols() == BM_OK, "Failed import for layer protocols", BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(ImportForTransportManager() == BM_OK, "Failed import for transport manager", BM_ERROR);

    if (hbmSegment_ == nullptr) {
        if (options_.scene == HYBM_SCENE_TRANS) {
            BM_LOG_ERROR("hbm segment is null, failed to import segment info in trans scene");
            return BM_ERROR;
        }
        return BM_OK;
    }

    std::vector<std::string> infos;
    for (auto i = 0U; i < count; i++) {
        if (desc[i].LeftBytes() > 0) {
            infos.emplace_back(desc[i].LeftToString());
        }
    }
    if (infos.size() > 0) {
        std::vector<ImportEntryKind> importResults(infos.size(), ImportEntryKind::PeerSlice);
        auto ret = hbmSegment_->Import(infos, nullptr, overrideRank, importResults);
        if (ret != BM_OK) {
            BM_LOG_ERROR("failed to import segment info, ret: " << ret);
            return BM_ERROR;
        }
    }
    return BM_OK;
}

int32_t MemEntityDefault::SetExtraContext(const void *context, uint32_t size) noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_NOT_INITIALIZED;
    }

    BM_ASSERT_LOG_AND_RETURN(context != nullptr, "context is nullptr", BM_INVALID_PARAM);
    if (size > HYBM_DEVICE_USER_CONTEXT_PRE_SIZE) {
        BM_LOG_ERROR("set extra context failed, context size is too large: " << size << " limit: "
                                                                             << HYBM_DEVICE_USER_CONTEXT_PRE_SIZE);
        return BM_INVALID_PARAM;
    }

    uint64_t addr = HYBM_DEVICE_USER_CONTEXT_ADDR + id_ * HYBM_DEVICE_USER_CONTEXT_PRE_SIZE;
    auto ret = VendorApi::Api()->Memcpy((void *)addr, HYBM_DEVICE_USER_CONTEXT_PRE_SIZE, context, size,
                                        ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("memcpy user context failed, ret: " << ret);
        return BM_ERROR;
    }

    return UpdateHybmDeviceInfo(size);
}

void MemEntityDefault::Unmap() noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return;
    }

    if (hbmSegment_ != nullptr) {
        hbmSegment_->Unmap();
    }
    if (dramSegment_ != nullptr) {
        dramSegment_->Unmap();
    }
}

int32_t MemEntityDefault::Mmap() noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_NOT_INITIALIZED;
    }

    // in trans scene, two segement work togather, but hbm do not need to mmap, and wrongly mmap will make dram mmap
    // fail
    if (hbmSegment_ != nullptr && options_.scene != HYBM_SCENE_TRANS) {
        auto ret = hbmSegment_->Mmap();
        if (ret != BM_OK) {
            if (ret == BM_P2P_NOT_READY) {
                BM_LOG_WARN("hbmSegment_->Mmap() not ready, entityId: " << id_ << ", will retry later");
            } else {
                BM_LOG_ERROR("hbmSegment_->Mmap() failed, ret: " << ret << " entityId: " << id_);
            }
            return ret;
        }
    }

    if (dramSegment_ != nullptr) {
        auto ret = dramSegment_->Mmap();
        if (ret != BM_OK) {
            BM_LOG_ERROR("dramSegment_->Mmap() failed, ret: " << ret << " entityId: " << id_);
            return ret;
        }
    }

    return BM_OK;
}

int32_t MemEntityDefault::UnmapRank(uint32_t rankId) noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized");
        return BM_NOT_INITIALIZED;
    }
    if (transportManager_ != nullptr) {
        std::vector<uint32_t> ranks = {rankId};
        auto ret = transportManager_->RemoveRanks(ranks);
        if (ret != BM_OK) {
            BM_LOG_WARN("transport remove ranks failed for rank " << rankId << " ret: " << ret);
        }
    }
    if (hbmSegment_ != nullptr) {
        auto ret = hbmSegment_->RemoveImported({rankId});
        if (ret != BM_OK) {
            BM_LOG_WARN("hbm unmap rank " << rankId << " failed: " << ret);
        }
    }
    if (dramSegment_ != nullptr) {
        auto ret = dramSegment_->RemoveImported({rankId});
        if (ret != BM_OK) {
            BM_LOG_WARN("dram unmap rank " << rankId << " failed: " << ret);
        }
    }
    if (tagManager_ != nullptr) {
        tagManager_->RemoveRankInfo(rankId);
    }
    {
        std::unique_lock<std::mutex> lock{importMutex_};
        importedRanks_.erase(rankId);
        importedMemories_.erase(rankId);
    }
    return BM_OK;
}

int32_t MemEntityDefault::RemoveImported(const std::vector<uint32_t> &ranks) noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_NOT_INITIALIZED;
    }

    if (providerServer_ != nullptr) {
        const auto ret = providerServer_->RemoveRanks(ranks);
        if (ret != BM_OK) {
            return ret;
        }
    }
    if (transportManager_ != nullptr) {
        auto ret = transportManager_->RemoveRanks(ranks);
        if (ret != BM_OK) {
            auto conn = std::dynamic_pointer_cast<HybmConnBasedSegment>(dramSegment_);
            if (conn != nullptr && conn->IsHostProxy()) {
                BM_LOG_ERROR("unable to remove provider transport ranks : " << ret);
                return ret;
            }
            BM_LOG_WARN("unable to transport remove ranks : " << ret);
        }
    }

    if (hbmSegment_ != nullptr) {
        auto ret = hbmSegment_->RemoveImported(ranks);
        if (ret != BM_OK) {
            return ret;
        }
    }

    if (dramSegment_ != nullptr) {
        auto ret = dramSegment_->RemoveImported(ranks);
        if (ret != BM_OK) {
            return ret;
        }
    }

    if (dataOperator_ != nullptr) {
        dataOperator_->CleanUp();
    }

    if (tagManager_ != nullptr) {
        for (auto rank : ranks) {
            tagManager_->RemoveRankInfo(rank);
        }
    }

    {
        std::unique_lock<std::mutex> uniqueLock{importMutex_};
        for (auto rank : ranks) {
            importedRanks_.erase(rank);
            importedMemories_.erase(rank);
        }
    }

    return BM_OK;
}

// host combo 组合下的本地地址判据：只有已知本地类别（本机普通内存或本机 provider 共享内存映射区）
// 才允许走本地共享内存拷贝；未知归属（含窗口内池外空洞）一律按远端处理，避免跨机读写误落本机内存。
static bool IsLocalAddressScope(HybmConnBasedSegment::AddressScope scope)
{
    return scope == HybmConnBasedSegment::AddressScope::LOCAL_HOST ||
           scope == HybmConnBasedSegment::AddressScope::LOCAL_PROVIDER_SHM;
}

// The TRANS HBM segment may cover both HBM and DRAM rank windows.
// Exclude HBM-only windows before classifying DRAM through its mapped range.
// Unknown holes and remote windows must stay on the device path.
bool MemEntityDefault::IsLocalCopyAddress(void *addr, const HybmConnBasedSegment &segment) noexcept
{
    const uint64_t va = reinterpret_cast<uint64_t>(addr);
    uint32_t hbmRank = 0;
    uint32_t dramRank = 0;
    if (hbmSegment_ != nullptr && hbmSegment_->FindRemoteRankByVa(va, hbmRank) &&
        !segment.FindRemoteRankByVa(va, dramRank)) {
        return false;
    }
    return IsLocalAddressScope(segment.GetAddressScope(va));
}

int32_t MemEntityDefault::RemoveAllImported() noexcept
{
    BM_ASSERT_LOG_AND_RETURN(initialized_, "the object is not initialized", BM_NOT_INITIALIZED);
    std::set<uint32_t> ranks;
    {
        std::lock_guard<std::mutex> lock(importMutex_);
        for (const auto &peer : importedRanks_) {
            ranks.insert(peer.first);
        }
        for (const auto &peer : importedMemories_) {
            ranks.insert(peer.first);
        }
    }
    ranks.erase(options_.rankId);
    int32_t firstError = BM_OK;
    for (auto rank : ranks) {
        auto ret = RemoveImported({rank});
        if (ret != BM_OK) {
            BM_LOG_WARN("remove imported rank failed, rank=" << rank << " ret=" << ret);
            if (firstError == BM_OK) {
                firstError = ret;
            }
        }
    }
    return firstError;
}

int32_t MemEntityDefault::CopyData(hybm_copy_params &params, hybm_data_copy_direction direction, void *stream,
                                   uint32_t flags) noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_NOT_INITIALIZED;
    }
    auto ret = SetThreadAclDevice();
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "ret = " << ret, BM_ERROR);

    ret = BM_OK;
    std::pair<uint32_t, uint32_t> p2pInfo;
    ret = LocateAddrAndRank(params.src, params.dest, p2pInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("failed to locate addr and rank, ret:" << ret << ", src:" << VaToStr(params.src) << ", dest:"
                                                            << VaToStr(params.dest) << ", size:" << params.dataSize);
        return ret;
    }
    ExtOptions options{};
    options.flags = flags;
    options.stream = stream;
    options.srcRankId = p2pInfo.first;
    options.destRankId = p2pInfo.second;
    auto connSegment = std::dynamic_pointer_cast<HybmConnBasedSegment>(dramSegment_);
    if (connSegment != nullptr && connSegment->IsHostCombo()) {
        const bool local =
            IsLocalCopyAddress(params.src, *connSegment) && IsLocalCopyAddress(params.dest, *connSegment);
        options.hostShmPolicy = local ? HostShmPolicy::PREFER : HostShmPolicy::EXCLUDE;
    }

    if (dataOperator_ == nullptr) {
        BM_LOG_ERROR("Data copy failed, dataOperator_ is null, src:"
                     << VaToStr(params.src) << ", dest:" << VaToStr(params.dest) << ", size:" << params.dataSize);
        return BM_ERROR;
    }
    ret = dataOperator_->DataCopy(params, direction, options);
    if (ret != BM_OK) {
        BM_LOG_ERROR("failed to copy data ret:" << ret << ", src:" << VaToStr(params.src)
                                                << ", dest:" << VaToStr(params.dest) << ", size:" << params.dataSize);
    }
    return ret;
}

int32_t MemEntityDefault::BatchCopyData(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                        void *stream, uint32_t flags) noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_NOT_INITIALIZED;
    }
    auto ret = SetThreadAclDevice();
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "ret = " << ret, BM_ERROR);

    ret = BM_ERROR;
    if (dataOperator_ == nullptr) {
        BM_LOG_ERROR("Data copy failed, dataOperator_ is null.");
        return ret;
    }

    ExtOptions localOptions{};
    localOptions.stream = stream;
    localOptions.flags = flags;
    localOptions.hostShmPolicy = HostShmPolicy::PREFER;
    ExtOptions remoteOptions{};
    remoteOptions.hostShmPolicy = HostShmPolicy::EXCLUDE;
    remoteOptions.stream = stream;
    remoteOptions.flags = flags;
    auto connSegment = std::dynamic_pointer_cast<HybmConnBasedSegment>(dramSegment_);
    const bool hostCombo = connSegment != nullptr && connSegment->IsHostCombo();
    // 将所有地址按 srcRank - dstRank 分组，并按本地/远端 provider 拆成两组。
    TP_TRACE_BEGIN(TP_HYBM_BATCH_BUILD_GROUP_MAP);
    for (uint32_t i = 0; i < params.batchSize; ++i) {
        std::pair<uint32_t, uint32_t> p2pInfo;
        ret = LocateAddrAndRank(params.sources[i], params.destinations[i], p2pInfo);
        if (ret != BM_OK) {
            TP_TRACE_END(TP_HYBM_BATCH_BUILD_GROUP_MAP, ret);
            BM_LOG_ERROR("failed to locate addr and rank, ret:"
                         << ret << ", index:" << i << ", src:" << VaToStr(params.sources[i])
                         << ", dest:" << VaToStr(params.destinations[i]) << ", size:" << params.dataSizes[i]);
            return ret;
        }
        bool local = true;
        if (hostCombo) {
            // 与 CopyData 同判据：device VA 与未知归属（含窗口内池外空洞）一律按远端处理。
            local = IsLocalCopyAddress(params.sources[i], *connSegment) &&
                    IsLocalCopyAddress(params.destinations[i], *connSegment);
        }
        if (local) {
            localOptions.groupMap[p2pInfo].push_back(i);
        } else {
            remoteOptions.groupMap[p2pInfo].push_back(i);
        }
    }
    TP_TRACE_END(TP_HYBM_BATCH_BUILD_GROUP_MAP, BM_OK);

    BM_LOG_DEBUG("BatchDataCopy: localRank=" << options_.rankId << ", batchSize=" << params.batchSize << ", direction="
                                             << direction << ", localGroups=" << localOptions.groupMap.size()
                                             << ", remoteGroups=" << remoteOptions.groupMap.size());
    TP_TRACE_BEGIN(TP_HYBM_BATCH_DISPATCH_DATA_OP);
    if (!localOptions.groupMap.empty()) {
        ret = dataOperator_->BatchDataCopy(params, direction, localOptions);
        if (ret != BM_OK) {
            TP_TRACE_END(TP_HYBM_BATCH_DISPATCH_DATA_OP, ret);
            BM_LOG_ERROR("local batch data copy failed, ret: " << ret);
            return ret;
        }
    }
    if (!remoteOptions.groupMap.empty()) {
        ret = dataOperator_->BatchDataCopy(params, direction, remoteOptions);
        if (ret != BM_OK) {
            TP_TRACE_END(TP_HYBM_BATCH_DISPATCH_DATA_OP, ret);
            BM_LOG_ERROR("remote batch data copy failed, ret: " << ret);
            return ret;
        }
    }
    ret = BM_OK;
    TP_TRACE_END(TP_HYBM_BATCH_DISPATCH_DATA_OP, ret);
    return ret;
}

int32_t MemEntityDefault::PrepareDeviceCopy(uint32_t rankId, uint64_t poolBase, uint64_t poolSize) noexcept
{
    if (transportManager_ == nullptr || !initialized_) {
        BM_LOG_ERROR("device copy entity not initialized, rankId: " << options_.rankId);
        return BM_NOT_INITIALIZED;
    }

    return transportManager_->PrepareDeviceCopy(rankId, poolBase, poolSize);
}

int32_t MemEntityDefault::DeviceCopy(const hybm_device_copy_params &params) noexcept
{
    if (transportManager_ == nullptr || !initialized_) {
        BM_LOG_ERROR("device copy entity not initialized, rankId: " << options_.rankId);
        return BM_NOT_INITIALIZED;
    }

    return transportManager_->DeviceCopy(params);
}

int32_t MemEntityDefault::Wait() noexcept
{
    if (!initialized_) {
        BM_LOG_ERROR("the object is not initialized, please check whether Initialize is called.");
        return BM_NOT_INITIALIZED;
    }
    if (dataOperator_ == nullptr) {
        // provider 进程不创建数据面算子，也就没有待完成任务可等。
        return BM_OK;
    }
    return dataOperator_->Wait(0);
}

int MemEntityDefault::CheckOptions(const hybm_options *options) noexcept
{
    if (options == nullptr) {
        BM_LOG_ERROR("initialize with nullptr.");
        return BM_INVALID_PARAM;
    }

    if (options->rankId >= options->rankCount) {
        BM_LOG_ERROR("local rank id: " << options->rankId << " invalid, total is " << options->rankCount);
        return BM_INVALID_PARAM;
    }

    if (options->clusterRole < HYBM_CLUSTER_ROLE_DEFAULT || options->clusterRole >= HYBM_CLUSTER_ROLE_BUTT) {
        BM_LOG_ERROR("invalid cluster role: " << options->clusterRole);
        return BM_INVALID_PARAM;
    }

    constexpr uint32_t deviceNetworkProtocolMask = HYBM_DOP_TYPE_DEVICE_TRANSPORT_MASK;
    const uint32_t enabledDeviceNetworkProtocols = options->bmDataOpType & deviceNetworkProtocolMask;
    // 只允许启用一种协议
    if (enabledDeviceNetworkProtocols != 0U &&
        (enabledDeviceNetworkProtocols & (enabledDeviceNetworkProtocols - 1U)) != 0U) {
        BM_LOG_ERROR("device network protocols conflict: rankId=" << options->rankId
                                                                  << " bmDataOpType=" << options->bmDataOpType);
        return BM_INVALID_PARAM;
    }

    const bool hasHostShm = (options->bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) != 0;
    if (hasHostShm) {
        if ((options->memType & HYBM_MEM_TYPE_DEVICE) != 0 || options->deviceVASpace != 0 ||
            options->maxHBMSize != 0U) {
            BM_LOG_ERROR("HOST_SHM op type only supports DRAM shared memory without HBM");
            return BM_INVALID_PARAM;
        }
        // 纯 HOST_SHM 模式要求非零本地地址空间，组合模式允许 proxy 不提供本地内存。
        if (enabledDeviceNetworkProtocols == 0U && options->hostVASpace == 0U) {
            BM_LOG_ERROR("HOST_SHM op type requires non-zero host VASpace");
            return BM_INVALID_PARAM;
        }
        constexpr uint32_t forbidden =
            HYBM_DOP_TYPE_SDMA | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA | HYBM_DOP_TYPE_HOST_TCP;
        if ((options->bmDataOpType & forbidden) != 0U) {
            BM_LOG_ERROR("HOST_SHM cannot mix with SDMA or host RDMA/URMA/TCP, bmDataOpType=" << options->bmDataOpType);
            return BM_INVALID_PARAM;
        }
    }
    if ((options->flags & HYBM_FLAG_CREATE_WITH_SHM) != 0 && options->dramShmFd < 0) {
        BM_LOG_ERROR("local rank id: " << options->rankId << ", create with share memory flag set but fd: "
                                       << options->dramShmFd << " invalid.");
        return BM_INVALID_PARAM;
    }

    return BM_OK;
}

int MemEntityDefault::LoadExtendLibrary() noexcept
{
    constexpr uint32_t deviceInterMask = HYBM_DOP_TYPE_DEVICE_TRANSPORT_MASK;
    if ((options_.bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) != 0 && (options_.bmDataOpType & deviceInterMask) == 0U) {
        BM_LOG_DEBUG("HOST_SHM-only data operator selected, skip loading transport extend libraries.");
        return BM_OK;
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) {
        auto ret = BM_OK;
        if (OpenDccSelected(options_)) {
#ifdef BUILD_OPENDCC
            ret = DlOpenDccApi::Load();
#else
            BM_LOG_ERROR("openDCC is not built");
            return BM_NOT_SUPPORTED;
#endif
        } else {
            ret = DlApi::LoadExtendLibrary(DlApiExtendLibraryType::DL_EXT_LIB_DEVICE_RDMA);
        }
        if (ret != 0) {
            BM_LOG_ERROR("LoadExtendLibrary for DEVICE RDMA failed: " << ret);
            return ret;
        }
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_AIV_SDMA) {
        auto ret = DlApi::LoadExtendLibrary(DlApiExtendLibraryType::DL_EXT_LIB_DEVICE_SDMA);
        if (ret != 0) {
            BM_LOG_ERROR("LoadExtendLibrary for DEVICE SDMA failed: " << ret);
            return ret;
        }
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_URMA) {
        auto ret = DlApi::LoadExtendLibrary(DlApiExtendLibraryType::DL_EXT_LIB_DEVICE_URMA);
        if (ret != 0) {
            BM_LOG_ERROR("LoadExtendLibrary for DEVICE URMA failed: " << ret);
            return ret;
        }
    }

    if (options_.scene == HYBM_SCENE_OFFLOAD && options_.bmDataOpType == HYBM_DOP_TYPE_HOST_URMA) {
        auto ret = DlApi::LoadExtendLibrary(DlApiExtendLibraryType::DL_EXT_LIB_DEVICE_URMA);
        if (ret != BM_OK) {
            BM_LOG_ERROR("LoadExtendLibrary for Host URMA offload failed: " << ret);
            return ret;
        }
    } else if (options_.bmDataOpType & (HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA | HYBM_DOP_TYPE_HOST_TCP)) {
        auto ret = DlApi::LoadExtendLibrary(DlApiExtendLibraryType::DL_EXT_LIB_HOST_RDMA);
        if (ret != 0) {
            BM_LOG_ERROR("LoadExtendLibrary for HOST RDMA failed: " << ret);
            return ret;
        }
    }

    return BM_OK;
}

int MemEntityDefault::UpdateHybmDeviceInfo(uint32_t extCtxSize) noexcept
{
    if (options_.bmType != HYBM_TYPE_AI_CORE_INITIATE) {
        return BM_OK;
    }

    HybmDeviceMeta info;
    auto addr = HYBM_DEVICE_META_ADDR + HYBM_DEVICE_GLOBAL_META_SIZE + id_ * HYBM_DEVICE_PRE_META_SIZE;

    SetHybmDeviceInfo(info);
    info.extraContextSize = extCtxSize;
    auto ret = VendorApi::Api()->Memcpy((void *)addr, HYBM_LARGE_PAGE_SIZE, &info, sizeof(HybmDeviceMeta),
                                        ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR("update hybm info memory failed, ret: " << ret);
        return BM_ERROR;
    }
    return BM_OK;
}

void MemEntityDefault::SetHybmDeviceInfo(HybmDeviceMeta &info)
{
    info.entityId = id_;
    info.rankId = options_.rankId;
    info.rankSize = options_.rankCount;
    info.symmetricSize = options_.maxHBMSize;
    info.extraContextSize = 0;
    if (transportManager_ != nullptr) {
        info.qpInfoAddress = (uint64_t)(ptrdiff_t)transportManager_->GetQpInfo();
        info.sdmaWorkSpace = transportManager_->GetSdmaWorkSpaceAddr();
    } else {
        info.qpInfoAddress = 0UL;
        info.sdmaWorkSpace = 0UL;
    }
}

int32_t MemEntityDefault::ImportForTransportPrecheck(const ExchangeInfoReader desc[], uint32_t &count,
                                                     const std::vector<ImportEntryKind> &types, void *addresses[],
                                                     uint32_t overrideRank)
{
    int ret = BM_OK;
    SliceExportTransportKey transportKey;
    for (auto i = 0U; i < count; i++) {
        if (types[i] != ImportEntryKind::PeerSlice) {
            if (addresses != nullptr) {
                addresses[i] = nullptr;
            }
            continue;
        }

        ret = ReadSliceTransportKey(desc[i], transportKey);
        if (ret != BM_OK) {
            BM_LOG_ERROR("read info for transport failed: " << ret);
            return ret;
        }
        if (transportKey.key.Empty()) {
            continue;
        }

        // trans scene overwrites the transport key address with the address the caller will use for copies.
        const bool overwriteKeyAddr = options_.scene == HYBM_SCENE_TRANS;
        if (addresses != nullptr && overwriteKeyAddr) {
            transportManager_->UpdateMemoryKey(transportKey.key, addresses[i]);
        }

        // trans 场景 rank 覆盖：与 entity/segment 导入路径使用同一本地覆盖 rank，
        // 保证 ImportForTransport 中 importedMemories_ 与 importedRanks_ 的 rank key 对齐
        const uint32_t rankId = (overrideRank != HYBM_IMPORT_RANK_INVALID) ? overrideRank : transportKey.rankId;
        {
            std::unique_lock<std::mutex> uniqueLock{importMutex_};
            importedMemories_[rankId].insert(transportKey.key);
        }
        BM_LOG_DEBUG("Success to import slice rankId:" << transportKey.rankId << " addr:" << std::hex
                                                       << transportKey.address);
    }
    return BM_OK;
}

int32_t MemEntityDefault::ImportForTransport() noexcept
{
    int ret = BM_OK;
    transport::HybmTransPrepareOptions transOptions;
    std::unique_lock<std::mutex> uniqueLock{importMutex_};
    for (auto &rank : importedRanks_) {
        // Peers without a transport (e.g. a bare provider) export an empty nic and cannot be connected.
        if (rank.second.nic[0] == '\0') {
            continue;
        }
        // A proxy only connects (device RDMA) to peers on the same node-unique phyDevId.
        if (options_.clusterRole == HYBM_CLUSTER_ROLE_PROXY && rank.second.phyDevId != LocalPhyDevId()) {
            continue;
        }

        transOptions.options[rank.first].nic = rank.second.nic;
        transOptions.options[rank.first].privateData = rank.second.transportPrivateData;
        BM_LOG_DEBUG("ImportForTransport rankid:" << rank.first);
    }
    for (auto &mr : importedMemories_) {
        auto pos = transOptions.options.find(mr.first);
        if (pos != transOptions.options.end()) {
            for (auto &key : mr.second) {
                pos->second.memKeys.emplace_back(key);
            }
        }
    }
    uniqueLock.unlock();
    if (transportManager_ != nullptr) {
        {
            std::lock_guard<std::mutex> lock(transportMutex_);
            if (!transportPrepared_) {
                ret = transportManager_->Prepare(transOptions);
                transportPrepared_ = true;
            } else {
                ret = transportManager_->UpdateRankOptions(transOptions);
            }
        }
        if (ret != 0) {
            BM_LOG_ERROR("ImportForTransport failed: " << ret);
            return ret;
        }
    }
    return BM_OK;
}

Result MemEntityDefault::LocateAddrAndRank(void *&src, void *&dest, std::pair<uint32_t, uint32_t> &p2pInfo) noexcept
{
    // va -> remote rank: resolve by the GVA layout of this entity's segments (gva<->rank is computed by the
    // segment itself, see MemSegment::FindRemoteRankByVa). A miss means local memory, fall back to options_.rankId.
    auto resolveRank = [this](uint64_t va) {
        uint32_t rank = 0;
        if ((hbmSegment_ != nullptr && hbmSegment_->FindRemoteRankByVa(va, rank)) ||
            (dramSegment_ != nullptr && dramSegment_->FindRemoteRankByVa(va, rank))) {
            auto conn = std::dynamic_pointer_cast<HybmConnBasedSegment>(dramSegment_);
            if (conn != nullptr && conn->IsHostProxy() &&
                conn->GetAddressScope(va) == HybmConnBasedSegment::AddressScope::LOCAL_PROVIDER_SHM) {
                return options_.rankId;
            }
            return rank;
        }
        return options_.rankId;
    };

    p2pInfo.first = resolveRank(reinterpret_cast<uint64_t>(src));
    p2pInfo.second = resolveRank(reinterpret_cast<uint64_t>(dest));

    BM_LOG_DEBUG("LocateAddrAndRank, srcRank: " << p2pInfo.first << ", destRank: " << p2pInfo.second);
    return BM_OK;
}

Result MemEntityDefault::InitSegment()
{
    BM_LOG_DEBUG("Initialize segment with type: " << std::hex << options_.memType);
    if (options_.memType & HYBM_MEM_TYPE_DEVICE) {
        auto ret = InitHbmSegment();
        if (ret != BM_OK) {
            BM_LOG_ERROR("InitHbmSegment() failed: " << ret);
            return ret;
        }
    }

    if (options_.memType & HYBM_MEM_TYPE_HOST) {
        auto ret = InitDramSegment();
        if (ret != BM_OK) {
            BM_LOG_ERROR("InitDramSegment() failed: " << ret);
            return ret;
        }
    }

    return BM_OK;
}

Result MemEntityDefault::InitHbmSegment()
{
    if (options_.maxHBMSize == 0) {
        BM_LOG_INFO("Hbm rank space is zero.");
        return BM_OK;
    }

    MemSegmentOptions segmentOptions;
    if (options_.scene != HYBM_SCENE_TRANS) {
        segmentOptions.size = options_.deviceVASpace;
        segmentOptions.maxSize = options_.maxHBMSize;
        segmentOptions.segType = HYBM_MST_HBM;
        BM_LOG_TRACE("create entity global unified memory space.");
    } else {
        segmentOptions.size = options_.deviceVASpace;
        segmentOptions.maxSize = options_.maxHBMSize;
        segmentOptions.segType = HYBM_MST_ASYMMETRIC;
        BM_LOG_TRACE("create entity user defined memory space.");
    }
    segmentOptions.devId = static_cast<uint16_t>(options_.devId);
    segmentOptions.rankId = options_.rankId;
    segmentOptions.rankCnt = options_.rankCount;
    segmentOptions.dataOpType = options_.bmDataOpType;
    segmentOptions.flags = options_.flags;
    segmentOptions.clusterRole = options_.clusterRole;
    segmentOptions.enable56BitsGva = options_.enable56BitsGva;
    segmentOptions.scene = options_.scene;
    constexpr auto transportMask = HYBM_DOP_TYPE_DEVICE_TRANSPORT_MASK | HYBM_DOP_TYPE_HOST_RDMA;
    if ((options_.bmDataOpType & transportMask) != 0U && (options_.bmDataOpType & HYBM_DOP_TYPE_SDMA) == 0U) {
        segmentOptions.shared = false;
    }

    hbmSegment_ = MemSegment::Create(segmentOptions, id_);
    if (hbmSegment_ == nullptr) {
        BM_LOG_ERROR("Failed to create hbm segment");
        return BM_ERROR;
    }
    return BM_OK;
}

Result MemEntityDefault::InitDramSegment()
{
    if (options_.maxDRAMSize == 0) {
        BM_LOG_INFO("Dram rank space is zero.");
        return BM_OK;
    }

    MemSegmentOptions segmentOptions;
    segmentOptions.size = options_.hostVASpace;
    segmentOptions.maxSize = options_.maxDRAMSize;
    segmentOptions.devId = static_cast<uint16_t>(options_.devId);
    segmentOptions.segType = HYBM_MST_DRAM;
    segmentOptions.rankId = options_.rankId;
    segmentOptions.rankCnt = options_.rankCount;
    segmentOptions.dataOpType = options_.bmDataOpType;
    segmentOptions.flags = options_.flags;
    segmentOptions.clusterRole = options_.clusterRole;
    segmentOptions.shmFd = options_.dramShmFd;
    segmentOptions.enable56BitsGva = options_.enable56BitsGva;
    segmentOptions.scene = options_.scene;
    constexpr auto deviceTransportMask = HYBM_DOP_TYPE_DEVICE_TRANSPORT_MASK;
    if ((options_.bmDataOpType & deviceTransportMask) != 0U && (options_.bmDataOpType & HYBM_DOP_TYPE_SDMA) == 0U) {
        segmentOptions.shared = false;
    }
    dramSegment_ = MemSegment::Create(segmentOptions, id_);
    if (dramSegment_ == nullptr) {
        BM_LOG_ERROR("Failed to create dram segment");
        return BM_ERROR;
    }
    if (options_.scene != HYBM_SCENE_TRANS && (options_.bmDataOpType & HYBM_DOP_TYPE_SDMA) != 0U &&
        (options_.bmDataOpType & deviceTransportMask) == 0U && !dramSegment_->CheckSdmaReaches(options_.rankId)) {
        BM_LOG_ERROR("dram segment does not support sdma in current environment, rankId: " << options_.rankId);
        return BM_ERROR;
    }
    return BM_OK;
}

Result MemEntityDefault::InitTransManager()
{
    constexpr uint32_t hostComboDeviceMask = HYBM_DOP_TYPE_DEVICE_TRANSPORT_MASK;
    if ((options_.bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) != 0) {
        if ((options_.bmDataOpType & hostComboDeviceMask) == 0U) {
            BM_LOG_DEBUG("HOST_SHM-only data operator selected, skip transport manager initialization.");
            return BM_OK;
        }
        transport::TransportOptions proxyOptions{};
        proxyOptions.rankId = options_.rankId;
        proxyOptions.rankCount = options_.rankCount;
        proxyOptions.deviceId = options_.devId;
        proxyOptions.protocol = options_.bmDataOpType;
        proxyOptions.scene = options_.scene;
        proxyOptions.initialType = options_.bmType;
        proxyOptions.nic = options_.transUrl;
        proxyOptions.tlsOption = options_.tlsOption;

        if (ResolveHostComboRole(options_.clusterRole, options_.hostVASpace) == HYBM_CLUSTER_ROLE_PROVIDER) {
            if ((options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_URMA) != 0U) {
                providerServer_ = std::make_shared<transport::device::UrmaProviderControlServer>();
            } else {
                providerServer_ = std::make_shared<transport::device::RdmaProviderControlServer>();
            }
            const auto providerDevices = ProviderDeviceIds();
            if (providerDevices.empty()) {
                BM_LOG_ERROR("provider has no device to bind, check MEMFABRIC_PROVIDER_DEVICES and the visible NPUs.");
                return BM_ERROR;
            }
            if (!providerServer_->Init({providerDevices, proxyOptions})) {
                BM_LOG_ERROR("provider control server init failed.");
                return BM_ERROR;
            }
            return BM_OK;
        }

        transportManager_ = transport::TransportManager::Create(transport::TT_COMPOSE, tagManager_);
        BM_ASSERT_LOG_AND_RETURN(transportManager_ != nullptr, "Failed to create compose transport manager.", BM_ERROR);
        proxyOptions.isProxy = true;
        auto ret = transportManager_->OpenDevice(proxyOptions);
        if (ret != BM_OK) {
            BM_LOG_ERROR("proxy transport open device failed: " << ret);
            return ret;
        }
        return BM_OK;
    }

    const bool localAicpuSdma = options_.bmType == HYBM_TYPE_AICPU_INITIATE && options_.scene == HYBM_SCENE_OFFLOAD &&
                                options_.bmDataOpType == HYBM_DOP_TYPE_AIV_SDMA;
    if (options_.rankCount <= 1 && !localAicpuSdma) {
        BM_LOG_INFO("rank total count : " << options_.rankCount << ", no transport.");
        return BM_OK;
    }

    auto hostDataOpMask = HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA | HYBM_DOP_TYPE_HOST_TCP;
    auto composeDataOpMask =
        HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_AIV_SDMA | HYBM_DOP_TYPE_DEVICE_URMA | hostDataOpMask;
    if ((options_.bmDataOpType & composeDataOpMask) == 0) {
        BM_LOG_DEBUG("NO RDMA/URMA Data Operator transport skip init.");
        return BM_OK;
    }
    if (options_.bmDataOpType & HYBM_DOP_TYPE_AIV_SDMA) {
        transportManager_ = transport::TransportManager::Create(transport::TT_SDMA, tagManager_);
    } else {
        transportManager_ = transport::TransportManager::Create(transport::TT_COMPOSE, tagManager_);
    }
    BM_ASSERT_LOG_AND_RETURN(transportManager_ != nullptr, "Failed to create transportManager.", BM_ERROR);

    transport::TransportOptions options{};
    options.rankId = options_.rankId;
    options.rankCount = options_.rankCount;
    options.useOpenDcc = OpenDccSelected(options_);
    if (options.useOpenDcc) {
        options.deviceId = HybmGetInitedLogicDeviceId();
    }
    options.protocol = options_.bmDataOpType;
    options.scene = options_.scene;
    options.initialType = options_.bmType;
    options.nic = options_.transUrl;
    options.tlsOption = options_.tlsOption;
    auto ret = transportManager_->OpenDevice(options);
    if (ret != 0) {
        BM_LOG_ERROR("Failed to open device, ret: " << ret << " rankId: " << options_.rankId << " protocol: "
                                                    << options_.bmDataOpType << " nic: " << options.nic);
        transportManager_ = nullptr;
    }
    return ret;
}

Result MemEntityDefault::InitDataOperator()
{
    // AI_CORE驱动不走这里的dateOperator
    if (options_.bmType == HYBM_TYPE_AI_CORE_INITIATE) {
        BM_LOG_INFO("Type is ai core, not need init data operator.");
        return BM_OK;
    }

    constexpr uint32_t deviceInterMask = HYBM_DOP_TYPE_DEVICE_TRANSPORT_MASK;
    if ((options_.bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) != 0 && (options_.bmDataOpType & deviceInterMask) != 0U &&
        options_.hostVASpace > 0U) {
        BM_LOG_INFO("provider does not create a data operator.");
        return BM_OK;
    }

    auto dataOptions = options_;
    const bool sdmaConfigured = (dataOptions.bmDataOpType & HYBM_DOP_TYPE_SDMA) != 0U;
    const bool checkDramSdma = sdmaConfigured && hbmSegment_ == nullptr && dramSegment_ != nullptr;
    if (checkDramSdma && !dramSegment_->CheckSdmaReaches(options_.rankId)) {
        BM_LOG_WARN(
            "Disabling configured SDMA because the DRAM segment is not reachable via SDMA, rankId=" << options_.rankId);
        dataOptions.bmDataOpType = static_cast<hybm_data_op_type>(dataOptions.bmDataOpType & ~HYBM_DOP_TYPE_SDMA);
    }
    dataOperator_ = std::make_shared<HostComposeDataOp>(dataOptions, transportManager_, tagManager_);
    auto ret = dataOperator_->Initialize();
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to init data operator ret:" << ret);
        return ret;
    }
    // imported gva 的连接域解析：多进程实体同址共存（多 store 组窗口号撞车）时，
    // 全局台账的同 gva 行无法区分连接，注入本实体导入记录的解析器供数据面优先使用
    dataOperator_->SetImportedGvaResolver([this](uint64_t gva) -> uint64_t {
        uint64_t lva = 0;
        if (hbmSegment_ != nullptr && hbmSegment_->ResolveRemoteGva(gva, lva)) {
            return lva;
        }
        if (dramSegment_ != nullptr && dramSegment_->ResolveRemoteGva(gva, lva)) {
            return lva;
        }
        return 0;
    });
    return BM_OK;
}

bool MemEntityDefault::SdmaReaches(uint32_t remoteRank) const noexcept
{
    if (hbmSegment_ != nullptr) {
        return hbmSegment_->CheckSdmaReaches(remoteRank);
    }

    if (dramSegment_ != nullptr) {
        return dramSegment_->CheckSdmaReaches(remoteRank);
    }

    return false;
}

bool MemEntityDefault::QueryDataLinkStates(std::vector<std::pair<uint32_t, bool>> &states) const
{
    if (OpenDccSelected(options_)) {
        const auto compose = std::dynamic_pointer_cast<transport::ComposeTransportManager>(transportManager_);
        const auto device = compose == nullptr ? nullptr : compose->GetTransport(HYBM_DOP_TYPE_DEVICE_RDMA);
        for (uint32_t rank = 0; rank < options_.rankCount; ++rank) {
            if (rank != options_.rankId) {
                states.emplace_back(rank, device != nullptr && !device->GetConnectedProtocols(rank).empty());
            }
        }
        return true;
    }
    if (providerServer_ != nullptr) {
        const auto peers = providerServer_->GetPeerRanks();
        for (auto rank : peers) {
            states.emplace_back(rank, !providerServer_->GetConnectedProtocols(rank).empty());
        }
        return true;
    }
    const auto conn = std::dynamic_pointer_cast<HybmConnBasedSegment>(dramSegment_);
    if (conn == nullptr || !conn->IsHostProxy()) {
        return false;
    }
    const auto compose = std::dynamic_pointer_cast<transport::ComposeTransportManager>(transportManager_);
    const auto localProvider = conn->GetLocalProviderRank();
    for (const auto &[rank, info] : conn->GetImportedProviders()) {
        if (rank == localProvider || compose == nullptr) {
            states.emplace_back(rank, CanReachDataOperators(rank) != 0);
            continue;
        }
        // The peer may have removed its QP while our control link was down.
        const auto protocols = compose->GetConnectedProtocols(rank);
        const bool connected = std::any_of(protocols.begin(), protocols.end(), [&](uint32_t protocol) {
            const auto device = compose->GetTransport(protocol);
            return device != nullptr && !device->GetConnectedProtocols(rank).empty();
        });
        states.emplace_back(rank, connected);
    }
    return true;
}

hybm_data_op_type MemEntityDefault::CanReachDataOperators(uint32_t remoteRank) const noexcept
{
    if (providerServer_ != nullptr) {
        const auto protocols = providerServer_->GetConnectedProtocols(remoteRank);
        uint32_t reachable = 0U;
        for (auto protocol : protocols) {
            reachable |= protocol;
        }
        return static_cast<hybm_data_op_type>(reachable);
    }
    auto conn = std::dynamic_pointer_cast<HybmConnBasedSegment>(dramSegment_);
    if (conn != nullptr && conn->IsHostProxy()) {
        if (remoteRank == conn->GetLocalProviderRank()) {
            const auto scope = conn->GetAddressScope(conn->GetProviderGvaBase(remoteRank));
            return scope == HybmConnBasedSegment::AddressScope::LOCAL_PROVIDER_SHM ? HYBM_DOP_TYPE_HOST_SHM
                                                                                   : static_cast<hybm_data_op_type>(0U);
        }
        if (!conn->HasImportedProvider(remoteRank) || transportManager_ == nullptr) {
            return static_cast<hybm_data_op_type>(0U);
        }
        const auto protocols = transportManager_->GetConnectedProtocols(remoteRank);
        uint32_t reachable = 0U;
        for (const auto protocol : protocols) {
            reachable |= protocol;
        }
        return static_cast<hybm_data_op_type>(reachable);
    }
    uint32_t supportDataOp = 0U;
    if ((options_.bmDataOpType & HYBM_DOP_TYPE_SDMA) && SdmaReaches(remoteRank)) {
        supportDataOp |= (HYBM_DOP_TYPE_MTE | HYBM_DOP_TYPE_SDMA);
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) {
        supportDataOp |= HYBM_DOP_TYPE_DEVICE_RDMA;
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_URMA) {
        supportDataOp |= HYBM_DOP_TYPE_DEVICE_URMA;
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_HOST_RDMA) {
        supportDataOp |= HYBM_DOP_TYPE_HOST_RDMA;
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) {
        supportDataOp |= HYBM_DOP_TYPE_HOST_SHM;
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_HOST_URMA) {
        supportDataOp |= HYBM_DOP_TYPE_HOST_URMA;
    }
    return static_cast<hybm_data_op_type>(supportDataOp);
}

void *MemEntityDefault::GetReservedMemoryPtr(hybm_mem_type memType) noexcept
{
    if (memType == HYBM_MEM_TYPE_DEVICE) {
        return hbmGva_;
    }

    if (memType == HYBM_MEM_TYPE_HOST) {
        return dramGva_;
    }

    return nullptr;
}

int32_t MemEntityDefault::SetThreadAclDevice()
{
#if !defined(ASCEND_NPU)
    return BM_OK;
#endif
    if (options_.clusterRole == HYBM_CLUSTER_ROLE_PROVIDER) {
        return BM_OK;
    }
    // Other runtimes can change this thread's ACL device between transfers.
    // Recheck the current device before reusing any device-bound resources.
    const auto deviceId = HybmGetInitDeviceId();
    const auto ret = VendorApi::Api()->SetDevice(deviceId);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "aclrtSetDevice failed, deviceId: " << deviceId << " ret: " << ret, ret);
    return BM_OK;
}

void MemEntityDefault::ReleaseResources()
{
    const bool standaloneUrma = (options_.bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) != 0U &&
                                (options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_URMA) != 0U;
    if (!initialized_ && !standaloneUrma) {
        return;
    }
    const bool proxyMode = (options_.bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) != 0U && transportManager_ != nullptr;
    if (proxyMode) {
        const auto ret = transportManager_->CloseDevice();
        if (ret != BM_OK) {
            BM_LOG_ERROR("provider proxy close failed, retaining registered backing, ret=" << ret);
            RetainUnreleasedTransport({nullptr, std::move(dramSegment_), std::move(transportManager_),
                                       std::move(dataOperator_), std::move(hbmSegment_),
                                       std::move(managedAllocations_)});
            initialized_ = false;
            return;
        }
    }
    dataOperator_.reset();
    if (providerServer_ != nullptr) {
        providerServer_->Stop();
        if (!providerServer_->CleanupComplete()) {
            BM_LOG_ERROR("provider cleanup incomplete, retaining registered backing");
            RetainUnreleasedTransport(
                {std::move(providerServer_), std::move(dramSegment_), nullptr, nullptr, nullptr, {}});
        }
        providerServer_.reset();
    }
    if (!proxyMode && transportManager_ != nullptr) {
        transportManager_->CloseDevice();
    }
    // Imported mappings are tracked separately from local slices and can survive
    // UnReserveMemorySpace(). Clear them before dropping segment objects so the
    // same process can initialize again without stale VA records.
    if (initialized_) {
        Unmap();
    }
    UnReserveMemorySpace();
    importedRanks_.clear();
    importedMemories_.clear();
    hbmGva_ = nullptr;
    dramGva_ = nullptr;
    hbmSegment_.reset();
    dramSegment_.reset();
    if (transportManager_ != nullptr) {
        transportManager_.reset();
    }
    tagManager_.reset();
    transportPrepared_ = false;
    initialized_ = false;
    decltype(managedAllocations_) allocations;
    {
        std::lock_guard<std::mutex> lock(managedAllocationsMutex_);
        allocations.swap(managedAllocations_);
    }
}
} // namespace mf
} // namespace ock
