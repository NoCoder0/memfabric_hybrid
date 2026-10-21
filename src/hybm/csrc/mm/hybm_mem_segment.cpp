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
#include "hybm_mem_segment.h"

#include <fstream>
#include <sstream>

#include "dl_acl_api.h"
#include "hybm_asymmetric_mem_segment.h"
#include "hybm_common_include.h"
#include "hybm_conn_based_segment.h"
#include "hybm_dev_legacy_segment.h"
#include "hybm_gva.h"
#include "hybm.h"
#include "hybm_host_shm_segment.h"
#include "hybm_types.h"
#include "hybm_va_manager.h"
#include "hybm_vmm_based_segment.h"

namespace ock {
namespace mf {

constexpr uint32_t ASC910B_CONN_RANKS = 8U;

namespace {
constexpr uint64_t GIB = 1ULL << 30;

struct AllocationResources {
    MemSegmentPtr segment;
    void *context{nullptr};
    bool initialized{false};
};

struct HostAllocation {
    std::unique_ptr<AllocationResources> resources{std::make_unique<AllocationResources>()};
    uint64_t size{0};
    void *address{nullptr};

    ~HostAllocation()
    {
        if (!resources->initialized) {
            return;
        }
        if (resources->context == nullptr) {
            hybm_uninit();
            return;
        }
        void *previous = nullptr;
        (void)DlAclApi::AclrtGetCurrentContext(&previous);
        const auto ret = DlAclApi::AclrtSetCurrentContext(resources->context);
        if (ret != BM_OK) {
            BM_LOG_ERROR("aclrtSetCurrentContext failed during allocation cleanup, ret: "
                         << ret << " size_bytes: " << size << "; retaining resources until process exit");
            (void)resources.release();
            return;
        }
        resources->segment.reset();
        if (previous != nullptr && previous != resources->context) {
            const auto restoreRet = DlAclApi::AclrtSetCurrentContext(previous);
            if (restoreRet != BM_OK) {
                BM_LOG_ERROR("aclrtSetCurrentContext restore failed, ret: " << restoreRet);
            }
        }
        hybm_uninit();
    }
};

std::mutex allocationMutex;
std::map<uintptr_t, std::shared_ptr<HostAllocation>> allocations;

int32_t AllocateHostSegment(uint16_t deviceId, uint64_t size, HostAllocation &allocation)
{
    MemSegmentOptions options{};
    options.devId = deviceId;
    options.segType = HYBM_MST_DRAM;
    options.dataOpType = HYBM_DOP_TYPE_SDMA;
    options.infoExType = HYBM_INFO_EXG_CROSS_NODE_SDMA;
    options.rankCnt = 1;
    options.rankId = 0;
    options.maxSize = ((size + GIB - 1) / GIB) * GIB;
    options.size = options.maxSize;
    options.flags = HYBM_FLAG_DRAM_MAP_HOST_VA;
    allocation.resources->segment = MemSegment::Create(options, -1);
    auto &segment = allocation.resources->segment;
    BM_VALIDATE_RETURN(segment != nullptr, "Failed to create Fabric Host segment, deviceId: " << deviceId, BM_ERROR);
    void *gva = nullptr;
    auto ret = segment->ReserveMemorySpace(&gva);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "ReserveMemorySpace failed, deviceId: " << deviceId);
    MemSlicePtr slice;
    ret = segment->AllocLocalMemory(options.maxSize, slice);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "AllocLocalMemory failed, size_bytes: " << options.maxSize);
    // Export once while the allocation's physical handle is available. TRANS reuses the recorded handle.
    std::string info;
    ret = segment->Export(slice, info);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "Export Fabric Host allocation failed, size_bytes: " << options.maxSize);
    allocation.address = reinterpret_cast<void *>(slice->vAddress_);
    allocation.size = size;
    return BM_OK;
}
} // namespace

bool MemSegment::deviceInfoReady_{false};
int MemSegment::deviceId_{-1};
int MemSegment::logicDeviceId_{-1};
int MemSegment::devicePhyId_{-1};
uint32_t MemSegment::pid_{0};
uint32_t MemSegment::sdid_{0};
uint32_t MemSegment::serverId_{0};
uint32_t MemSegment::superPodId_{0};
std::string MemSegment::sysBoolId_{};
uint32_t MemSegment::bootIdHead_{0};
AscendSocType MemSegment::socType_{AscendSocType::ASCEND_UNKNOWN};
bool MemSegment::atforkRegistered_{false};

void MemSegment::ResetDeviceInfoInChild() noexcept
{
    MemSegment::deviceInfoReady_ = false;
    MemSegment::deviceId_ = -1;
    MemSegment::logicDeviceId_ = -1;
    MemSegment::devicePhyId_ = -1;
    MemSegment::pid_ = 0;
    MemSegment::sdid_ = 0;
    MemSegment::serverId_ = 0;
    MemSegment::superPodId_ = 0;
    MemSegment::sysBoolId_.clear();
    MemSegment::bootIdHead_ = 0;
    MemSegment::socType_ = ock::mf::AscendSocType::ASCEND_UNKNOWN;
}

MemSegmentPtr MemSegment::Create(const MemSegmentOptions &options, int entityId)
{
    if (options.rankId >= options.rankCnt) {
        BM_LOG_ERROR("rank(" << options.rankId << ") but total " << options.rankCnt);
        return nullptr;
    }

    auto ret = MemSegment::InitDeviceInfo(options.devId);
    if (ret != BM_OK) {
        BM_LOG_ERROR("MemSegment::InitDeviceInfo failed: " << ret);
        return nullptr;
    }
    ret = HybmVaManager::GetInstance().Initialize(socType_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HybmVaManager Initialize failed: " << ret);
        return nullptr;
    }
    MemSegmentPtr tmpSeg;

    // Hal not support 1 GB huge pages in A5. This is a temporary solution and will be deleted in the formal solution.
    if (options.flags & HYBM_FLAG_OFFLOAD_GIANT_PAGE) {
        if (socType_ == AscendSocType::ASCEND_910C) {
            tmpSeg = std::make_shared<HybmVmmBasedSegment>(options, entityId);
        } else if (socType_ == AscendSocType::ASCEND_950) {
            tmpSeg = std::make_shared<HybmConnBasedSegment>(options, entityId);
        } else {
            BM_LOG_ERROR("Invalid soc type use offload giant page");
        }
        return tmpSeg;
    }

    switch (options.segType) {
        case HYBM_MST_HBM:
            if (HybmGetGvaVersion() == HYBM_GVA_V4 || socType_ == AscendSocType::ASCEND_950) {
                tmpSeg = std::make_shared<HybmVmmBasedSegment>(options, entityId);
            } else {
                tmpSeg = std::make_shared<HybmDevLegacySegment>(options, entityId);
            }
            break;
        case HYBM_MST_DRAM: {
            constexpr auto DEVICE_TRANSPORT_MASK =
                HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_DEVICE_URMA | HYBM_DOP_TYPE_DEVICE_UBOE;
            const bool isSdmaMixedTransport =
                (options.dataOpType & DEVICE_TRANSPORT_MASK) != 0U && (options.dataOpType & HYBM_DOP_TYPE_SDMA) != 0U;
            const bool vmmSupport = HybmGetGvaVersion() == HYBM_GVA_V4 &&
                                    (socType_ == AscendSocType::ASCEND_910C || socType_ == AscendSocType::ASCEND_950) &&
                                    options.shmFd < 0 && !(options.dataOpType & HYBM_DOP_TYPE_HOST_TCP) &&
                                    (!(options.dataOpType & DEVICE_TRANSPORT_MASK) || isSdmaMixedTransport);
            // When host shared memory op type is set, use dedicated host shm segment.
            if ((options.dataOpType & HYBM_DOP_TYPE_HOST_SHM) != 0) {
                tmpSeg = std::make_shared<HybmHostShmSegment>(options, entityId);
            } else if (vmmSupport) {
                tmpSeg = std::make_shared<HybmVmmBasedSegment>(options, entityId);
            } else {
                tmpSeg = std::make_shared<HybmConnBasedSegment>(options, entityId);
            }
            break;
        }
        case HYBM_MST_ASYMMETRIC:
            tmpSeg = std::make_shared<AsymmetricMemSegment>(options, entityId);
            break;
        default:
            BM_LOG_ERROR("Invalid memory seg type " << int(options.segType));
    }
    return tmpSeg;
}

int32_t MemSegment::AllocateHostMemory(uint16_t deviceId, uint64_t size, void **address)
{
    BM_VALIDATE_RETURN(address != nullptr, "Output address is null", BM_INVALID_PARAM);
    *address = nullptr;
    BM_VALIDATE_RETURN(size > 0 && size <= MAX_HOST_ALLOC_SIZE,
                       "Invalid Fabric Host size_bytes: " << size << " max_size_bytes: " << MAX_HOST_ALLOC_SIZE,
                       BM_INVALID_PARAM);
    auto allocation = std::make_shared<HostAllocation>();
    auto ret = hybm_init(deviceId, 0);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "hybm_init failed, deviceId: " << deviceId);
    allocation->resources->initialized = true;
    int32_t currentDevice = -1;
    ret = DlAclApi::AclrtGetDevice(&currentDevice);
    BM_VALIDATE_RETURN(ret == BM_OK && currentDevice == deviceId,
                       "ACL device mismatch, ret: " << ret << " requested: " << deviceId
                                                    << " current: " << currentDevice,
                       BM_INVALID_PARAM);
    ret = DlAclApi::AclrtGetCurrentContext(&allocation->resources->context);
    BM_VALIDATE_RETURN(ret == BM_OK && allocation->resources->context != nullptr,
                       "aclrtGetCurrentContext failed, ret: " << ret << " deviceId: " << deviceId, BM_ERROR);
    BM_VALIDATE_RETURN(DlAclApi::GetAscendSocType() == AscendSocType::ASCEND_910C && HybmGetGvaVersion() == HYBM_GVA_V4,
                       "Fabric Host allocator requires Ascend A3", BM_ERROR);
    ret = AllocateHostSegment(deviceId, size, *allocation);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "Fabric Host allocation failed, deviceId: " << deviceId);
    {
        std::lock_guard<std::mutex> lock(allocationMutex);
        allocations.emplace(reinterpret_cast<uintptr_t>(allocation->address), allocation);
    }
    *address = allocation->address;
    return BM_OK;
}

int32_t MemSegment::FreeHostMemory(void *address)
{
    if (address == nullptr) {
        return BM_OK;
    }
    std::shared_ptr<HostAllocation> owner;
    {
        std::lock_guard<std::mutex> lock(allocationMutex);
        const auto it = allocations.find(reinterpret_cast<uintptr_t>(address));
        BM_VALIDATE_RETURN(it != allocations.end(), "Unknown Fabric Host allocation address: " << address,
                           BM_INVALID_PARAM);
        owner = std::move(it->second);
        allocations.erase(it);
    }
    // Destruction runs outside allocationMutex. TRANS may still own this allocation.
    return BM_OK;
}

int32_t MemSegment::RetainHostMemory(const void *address, uint64_t size, std::shared_ptr<void> &owner)
{
    owner.reset(); // Release a previous owner before taking the registry lock.
    const auto value = reinterpret_cast<uintptr_t>(address);
    std::lock_guard<std::mutex> lock(allocationMutex);
    auto it = allocations.upper_bound(value);
    if (it == allocations.begin()) {
        return BM_OK;
    }
    --it;
    const uint64_t physicalSize = ((it->second->size + GIB - 1) / GIB) * GIB;
    const uint64_t offset = value - it->first;
    if (offset >= physicalSize) {
        return BM_OK;
    }
    BM_VALIDATE_RETURN(size <= physicalSize - offset,
                       "Registration exceeds Fabric Host allocation, size_bytes: " << size << " remaining_bytes: "
                                                                                   << physicalSize - offset,
                       BM_INVALID_PARAM);
    owner = it->second;
    return BM_OK;
}

bool MemSegment::CheckSdmaReaches(uint32_t rankId) const noexcept
{
    return false;
}

Result MemSegment::RegisterMemCommon(const void *addr, uint64_t size, MemSlicePtr &slice)
{
    uint64_t va = reinterpret_cast<uint64_t>(addr);
    hybm_mem_type memType;
    auto ret = HybmVaManager::GetInstance().GetLocalMemoryType(va, memType);
    if (ret != BM_OK) {
        return ret;
    }
    if (memType == HYBM_MEM_TYPE_DEVICE) {
        slice = std::make_shared<MemSlice>(sliceCount_++, memType, MEM_PT_TYPE_SVM, 0, va, size);
        ret = HybmVaManager::GetInstance().AddVaInfo({0, va, va, size, memType});
        if (ret != 0) {
            BM_LOG_ERROR("add va info failed, va:" << VaToStr(va) << " ret:" << ret);
            return ret;
        }
    } else {
        void *output = nullptr;
#if defined(ASCEND_NPU)
        const bool needDeviceVa = (options_.dataOpType & (HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_DEVICE_URMA |
                                                          HYBM_DOP_TYPE_DEVICE_UBOE)) != 0U;
        if (needDeviceVa) {
            ret = DlHalApi::HalHostRegister(const_cast<void *>(addr), size, HOST_MEM_MAP_DEV, logicDeviceId_, &output);
            if (ret != 0) {
                BM_LOG_ERROR("RegisterMemory failed, size: " << size << " addr: " << std::hex << addr
                                                             << " ret: " << ret);
                return ret;
            }
        }
#endif
        auto dva = reinterpret_cast<uint64_t>(output);
        slice = std::make_shared<MemSlice>(sliceCount_++, memType, MEM_PT_TYPE_SVM, 0, va, size);
        ret = HybmVaManager::GetInstance().AddVaInfo({0, dva, va, size, memType});
        if (ret != 0) {
            BM_LOG_ERROR("add va info failed, va:" << VaToStr(va) << " ret:" << ret);
#if defined(ASCEND_NPU)
            if (needDeviceVa) {
                DlHalApi::HalHostUnregisterEx(const_cast<void *>(addr), logicDeviceId_, HOST_MEM_MAP_DEV);
            }
#endif
            return ret;
        }
    }

    return BM_OK;
}

Result MemSegment::InitDeviceInfo(int devId)
{
#if !defined(ASCEND_NPU)
    return BM_OK;
#endif

    if (!atforkRegistered_) {
        static std::once_flag flag;
        std::call_once(flag, []() {
            if (pthread_atfork(nullptr, nullptr, []() { MemSegment::ResetDeviceInfoInChild(); }) != 0) {
                BM_LOG_ERROR("Failed to register pthread_atfork handler!");
            }
        });
        atforkRegistered_ = true;
    }

    if (deviceInfoReady_) {
        auto ret = DlAclApi::AclrtSetDevice(devId, true);
        if (ret != 0) {
            BM_LOG_ERROR("set device to:" << devId << "failed: " << ret);
            return BM_DL_FUNCTION_FAILED;
        }
        return (deviceId_ == devId ? BM_OK : BM_INVALID_PARAM);
    }

    FillSysBootIdInfo();

    deviceId_ = devId;
    auto ret = DlAclApi::AclrtSetDevice(devId, true);
    if (ret != 0) {
        BM_LOG_ERROR("set device to:" << devId << "failed: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    ret = DlAclApi::RtGetLogicDevIdByUserDevId(devId, &logicDeviceId_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("get logic deviceId failed: " << ret << " userId:" << devId);
        return BM_DL_FUNCTION_FAILED;
    }

    ret = DlAclApi::AclrtGetPhyDevIdByLogicDevId(devId, &devicePhyId_);
    if (ret != 0) {
        BM_LOG_WARN("Failed to get phy deviceId by logicDevId, fallback to logicId: user="
                    << deviceId_ << ", logic=" << logicDeviceId_ << ", ret=" << ret);
        devicePhyId_ = logicDeviceId_;
    }

    ret = DlAclApi::RtDeviceGetBareTgid(&pid_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("get bare tgid failed: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    int64_t value = 0;
    ret = DlAclApi::RtGetDeviceInfo(deviceId_, 0, INFO_TYPE_SDID, &value);
    if (ret != BM_OK) {
        BM_LOG_ERROR("get sdid failed: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    sdid_ = static_cast<uint32_t>(value);
    ret = DlAclApi::RtGetDeviceInfo(deviceId_, 0, INFO_TYPE_SERVER_ID, &value);
    if (ret != BM_OK) {
        BM_LOG_ERROR("get server id failed: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    serverId_ = static_cast<uint32_t>(value);
    BM_LOG_DEBUG("local server=0x" << std::hex << serverId_);

    ret = DlAclApi::RtGetDeviceInfo(deviceId_, 0, INFO_TYPE_SUPER_POD_ID, &value);
    if (ret != BM_OK) {
        BM_LOG_ERROR("get super pod id failed: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    superPodId_ = static_cast<uint32_t>(value);
    if (superPodId_ == invalidSuperPodId && serverId_ == invalidServerId) {
        if (bootIdHead_ != 0) {
            serverId_ = bootIdHead_;
        } else {
            auto networks = NetworkGetIpAddresses();
            if (networks.empty()) {
                BM_LOG_ERROR("get local host ip address empty.");
                return BM_ERROR;
            }
            serverId_ = networks[0];
        }
    }

    socType_ = DlAclApi::GetAscendSocType();
    BM_LOG_DEBUG("local sdid=0x" << std::hex << sdid_ << ", local server=0x" << std::hex << serverId_
                                 << ", spid=" << superPodId_);
    deviceInfoReady_ = true;

    return BM_OK;
}

bool MemSegment::CanLocalHostReaches(uint32_t superPodId, uint32_t serverId, uint32_t deviceId) noexcept
{
    if (superPodId != superPodId_ || serverId != serverId_) {
        return false;
    }
    return (socType_ != ASCEND_910B) || ((deviceId / ASC910B_CONN_RANKS) == (deviceId_ / ASC910B_CONN_RANKS));
}

bool MemSegment::CanSdmaReaches(uint32_t superPodId, uint32_t serverId, uint32_t deviceId) noexcept
{
    if (serverId == serverId_) {
        return (socType_ != ASCEND_910B) || ((deviceId / ASC910B_CONN_RANKS) == (deviceId_ / ASC910B_CONN_RANKS));
    }

    if (superPodId == invalidSuperPodId || superPodId_ == invalidSuperPodId) {
        BM_LOG_DEBUG("spid: " << superPodId << ", local: " << superPodId_ << " cannot reach.");
        return false;
    }

    return superPodId == superPodId_;
}

void MemSegment::FillSysBootIdInfo() noexcept
{
    std::string bootIdPath("/proc/sys/kernel/random/boot_id");
    std::ifstream input(bootIdPath);
    input >> sysBoolId_;

    std::stringstream ss(sysBoolId_);
    ss >> std::hex >> bootIdHead_;
    BM_LOG_DEBUG("os-boot-id: " << sysBoolId_ << ", head u32: " << std::hex << bootIdHead_);
}

Result MemSegment::EnableRemotePeerAccess(int32_t remotePhyId) noexcept
{
    if (remotePhyId < 0) {
        BM_LOG_ERROR("invalid remote phy id: " << remotePhyId);
        return BM_INVALID_PARAM;
    }
    if (remotePhyId == devicePhyId_) {
        return BM_OK;
    }

    Result ret = DlAclApi::RtEnableP2P(static_cast<uint32_t>(deviceId_), static_cast<uint32_t>(remotePhyId), 0);
    if (ret != 0) {
        BM_LOG_ERROR("enable device access failed:" << ret << " local_user:" << deviceId_
                                                    << " local_phy:" << devicePhyId_ << " remote_phy:" << remotePhyId);
        return BM_DL_FUNCTION_FAILED;
    }
    BM_LOG_DEBUG("enable device access success local_user:" << deviceId_ << " local_phy:" << devicePhyId_
                                                            << " remote_phy:" << remotePhyId);
    return BM_OK;
}
} // namespace mf
} // namespace ock
