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
#include "hybm_asymmetric_mem_segment.h"

#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "hybm_ex_info_transfer.h"
#include "hybm_networks_common.h"
#include "hybm_va_manager.h"

namespace ock {
namespace mf {
constexpr uint8_t MAX_DEVICE_COUNT = 16;
AsymmetricMemSegment::AsymmetricMemSegment(const MemSegmentOptions &options, int eid) noexcept
    : HybmDevLegacySegment{options, eid}
{}

AsymmetricMemSegment::~AsymmetricMemSegment()
{
    UnReserveMemorySpace();
}

Result AsymmetricMemSegment::ValidateOptions() noexcept
{
    return BM_OK;
}

Result AsymmetricMemSegment::ReserveMemorySpace(void **address) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(address != nullptr, "address is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(options_.enable56BitsGva == true,
                             "options_.enable56BitsGva = " << options_.enable56BitsGva, BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(options_.rankId < options_.rankCnt,
                             "rank(" << options_.rankId << ") but total " << options_.rankCnt, BM_INVALID_PARAM);

    totalVirtualSize_ = options_.rankCnt * options_.maxSize;
    auto gvaInfo = HybmVaManager::GetInstance().AllocReserveGva(options_.rankId, totalVirtualSize_, 0,
                                                                HYBM_MEM_TYPE_DEVICE, options_.enable56BitsGva, true);
    BM_ASSERT_LOG_AND_RETURN(gvaInfo.va[HVM_GVA] > 0, "gvaInfo.va[HVM_GVA] = " << gvaInfo.va[HVM_GVA], BM_ERROR);
    globalVirtualAddress_ = (uint8_t *)reinterpret_cast<void *>(gvaInfo.va[HVM_GVA]);
    lvaBase_ = globalVirtualAddress_ + options_.maxSize * options_.rankId;
    unReserveDone_ = false;
    return BM_OK;
}

Result AsymmetricMemSegment::UnReserveMemorySpace() noexcept
{
    // Both ReleaseResources() and the destructor invoke this; only the first
    // call performs any release, later calls return silently.
    if (unReserveDone_.exchange(true)) {
        return BM_OK;
    }
    BM_LOG_INFO("un-reserve memory space.");
    if (!memNames_.empty() && options_.shared) {
        for (auto &name : memNames_) {
            DlAclApi::RtIpcDestroyMemoryName(name.c_str());
        }
        BM_LOG_INFO("Finish to destroy memory names.");
    } else {
        BM_LOG_INFO("Sender does not need to destroy memory names.");
    }
    memNames_.clear();
    CloseMemory();
    return BM_OK;
}

Result AsymmetricMemSegment::AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept
{
    BM_LOG_ERROR("AsymmetricMemSegment NOT SUPPORT AllocLocalMemory");
    return BM_NOT_SUPPORTED;
}

namespace {
// 用户注册内存判型：优先 HAL 属性查询（drvMemGetAttribute，A2/A3/A5 均支持，比地址段判断更准确）；
// 查询失败（pDrvMemGetAttribute 为 OPTIONAL 加载，老 HAL/无卡环境为空）或 no_xpu 构建
// （宏编译隔离）时，回退 HBM 地址段判断。属性解读与 HybmVaManager::GetLocalMemoryType 的
// ASCEND_950 路径保持一致。
bool IsDeviceMemoryAddr(uint64_t va)
{
#if defined(ASCEND_NPU)
    DVattribute attr{};
    const auto ret = DlHalApi::DrvMemGetAttribute(static_cast<DVdeviceptr>(va), &attr);
    // 打印 HAL 返回的原始 memType：A5(950) 为 SVM 统一编址架构，对 host 用户态内存也可能返回
    // SVM/LOCK 类属性（语义是"统一空间内设备可达"而非"物理介质为 HBM"），需实测确认取值域。
    // 注意 dl_hal_api_def.h 另有一套位域定义（MEM_PHY_BIT=14: MEM_TYPE_DDR/HBM），若 rawMemType
    // 命中位域形态，介质判型应改用 phy 位而非 DV_MEM_* 三值全等。
    BM_LOG_DEBUG("DrvMemGetAttribute: va=" << VaToStr(va) << " ret=" << ret
                                           << " rawMemType=" << static_cast<uint32_t>(attr.memType));
    if (ret == BM_OK) {
        return attr.memType == DV_MEM_SVM_DEVICE || attr.memType == DV_MEM_LOCK_DEV ||
               attr.memType == DV_MEM_LOCK_DEV_DVPP;
    }
    BM_LOG_DEBUG("DrvMemGetAttribute unavailable, fallback to address check: va=" << VaToStr(va));
#endif
    return IsHbmAddr(va);
}
} // namespace

Result AsymmetricMemSegment::RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    if (addr == nullptr || size == 0) {
        BM_LOG_ERROR("input address parameter is invalid, rankId=" << options_.rankId);
        return BM_INVALID_PARAM;
    }

    for (auto &it : registerSlices_) {
        if (it.second.slice->vAddress_ == reinterpret_cast<uint64_t>(addr)) {
            BM_LOG_ERROR("addr has registered: rankId=" << options_.rankId
                                                        << " addr=" << VaToStr(reinterpret_cast<uint64_t>(addr)));
            return BM_ERROR;
        }
    }

    if (allocatedSize_ + size > totalVirtualSize_) {
        BM_LOG_ERROR("gva space exhausted: rankId=" << options_.rankId << " allocated=" << allocatedSize_
                                                    << " request=" << size << " total=" << totalVirtualSize_);
        return BM_ERROR;
    }

    if (IsDeviceMemoryAddr(reinterpret_cast<uint64_t>(addr))) {
        return RegisterDeviceMemory(addr, size, slice);
    }
    return RegisterHostMemory(addr, size, slice);
}

Result AsymmetricMemSegment::RegisterDeviceMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    char name[DEVICE_SHM_NAME_SIZE + 1U]{};
    Result ret = BM_OK;
    if (options_.shared) {
        ret = DlAclApi::RtIpcSetMemoryName(addr, size, name, sizeof(name));
        if (ret != 0) {
            BM_LOG_ERROR("set memory name failed: rankId=" << options_.rankId
                                                           << " addr=" << VaToStr(reinterpret_cast<uint64_t>(addr))
                                                           << " size=" << size << " ret=" << ret);
            return BM_DL_FUNCTION_FAILED;
        }
    }
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto &remoteDev : importedDeviceInfo_) {
        if (!options_.shared ||
            !CanSdmaReaches(remoteDev.second.superPodId, remoteDev.second.serverId, remoteDev.second.devicePhyId)) {
            continue;
        }
        ret = DlAclApi::RtSetIpcMemorySuperPodPid(name, remoteDev.second.sdid, (int *)&remoteDev.second.pid, 1);
        if (ret != 0) {
            BM_LOG_ERROR("set shm(" << name << ") for sdid=" << remoteDev.second.sdid << " pid=" << remoteDev.second.pid
                                    << " failed: " << ret);
            DlAclApi::RtIpcDestroyMemoryName(name);
            return BM_DL_FUNCTION_FAILED;
        }
        BM_LOG_INFO("set shm(" << name << ") for sdid=" << remoteDev.second.sdid << " pid=" << remoteDev.second.pid
                               << " success.");
    }

    uint64_t gva = reinterpret_cast<uint64_t>(lvaBase_) + allocatedSize_;
    slice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM, gva,
                                       reinterpret_cast<uint64_t>(addr), size);
    // host_rdma: write DVA/HVA maps (bare device addr) to match ConnBasedSegment's va-table layout;
    // sdma/device_rdma: keep onlyGva=true to preserve the IPC-based sharing mechanism.
    const bool onlyGva = (options_.dataOpType & HYBM_DOP_TYPE_HOST_RDMA) == 0U;
    ret = HybmVaManager::GetInstance().AddVaInfo({gva, slice->vAddress_, slice->vAddress_, size, HYBM_MEM_TYPE_DEVICE},
                                                 options_.rankId, onlyGva);
    if (ret != 0) {
        BM_LOG_ERROR("AddVaInfo failed: rankId=" << options_.rankId << " gva=" << VaToStr(gva) << " size=" << size
                                                 << " ret=" << ret);
        if (options_.shared) {
            DlAclApi::RtIpcDestroyMemoryName(name);
        }
        slice = nullptr;
        return ret;
    }

    memNames_.emplace_back(name);
    registerSlices_.emplace(slice->index_, RegisterSlice{slice, name});
    allocatedSize_ += size;
    uniqueLock.unlock();
    return BM_OK;
}

Result AsymmetricMemSegment::CheckHostRegisterAllowed() const noexcept
{
    constexpr auto hostTransOps =
        HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA | HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_SHM;
    // dataOpType=0（未指定传输方式，如 RECEIVER 角色的 smem_trans 注册）：NeedHostDeviceMapping()
    // 为 false，不做设备映射，仅登记 GVA，保持兼容放行
    if (options_.dataOpType == 0U) {
        return BM_OK;
    }
    // 含 host 类传输：数据面走 host 侧 transport，放行。
    // 纯 SDMA/设备类 op 一律拒绝（评审结论）：SDMA 引擎不支持访问外部注册 DRAM，
    // 放行会导致数据面在无映射 GVA 上失败，必须在注册入口显式报错
    if ((options_.dataOpType & hostTransOps) != 0U) {
        return BM_OK;
    }
    // 错误返回路径必须记录根因日志（代码规范）：rankId/socType/dataOpType 为关键参数
    BM_LOG_ERROR("register dram not supported: rankId=" << options_.rankId << " socType=" << static_cast<int>(socType_)
                                                        << " dataOpType=" << options_.dataOpType);
    return BM_NOT_SUPPORTED;
}

bool AsymmetricMemSegment::NeedHostDeviceMapping() const noexcept
{
    // 仅设备侧连接类 op 需要把 host DRAM 映射进设备地址空间；
    // SDMA 引擎不支持访问外部注册 DRAM（评审结论），纯 SDMA 注册已在 CheckHostRegisterAllowed 拒绝
    return (options_.dataOpType &
            (HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_DEVICE_URMA | HYBM_DOP_TYPE_DEVICE_UBOE)) != 0U;
}

Result AsymmetricMemSegment::RegisterHostMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    auto ret = CheckHostRegisterAllowed();
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "ret = " << ret, ret);

    uint64_t hostVa = reinterpret_cast<uint64_t>(addr);
    uint64_t gva = reinterpret_cast<uint64_t>(lvaBase_) + allocatedSize_;
    uint64_t dva = 0;
    bool hostMapped = false;
    if (NeedHostDeviceMapping()) {
        void *output = nullptr;
        auto halRet =
            DlHalApi::HalHostRegister(const_cast<void *>(addr), size, HOST_MEM_MAP_DEV, logicDeviceId_, &output);
        if (halRet != 0) {
            BM_LOG_ERROR("HalHostRegister failed: rankId=" << options_.rankId << " deviceId=" << logicDeviceId_
                                                           << " addr=" << VaToStr(hostVa) << " size=" << size
                                                           << " ret=" << halRet);
            return BM_DL_FUNCTION_FAILED;
        }
        dva = reinterpret_cast<uint64_t>(output);
        hostMapped = true;
    }

    slice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, gva, hostVa, size);
    ret = HybmVaManager::GetInstance().AddVaInfo({gva, dva, hostVa, size, HYBM_MEM_TYPE_HOST}, options_.rankId);
    if (ret != 0) {
        BM_LOG_ERROR("AddVaInfo failed: rankId=" << options_.rankId << " gva=" << VaToStr(gva) << " size=" << size
                                                 << " ret=" << ret);
        if (hostMapped) {
            ReleaseHostMapping(hostVa);
        }
        slice = nullptr;
        return ret;
    }

    std::unique_lock<std::mutex> uniqueLock{mutex_};
    registerSlices_.emplace(slice->index_, RegisterSlice{slice, "", hostMapped});
    allocatedSize_ += size;
    uniqueLock.unlock();
    BM_LOG_INFO("register dram memory: rankId=" << options_.rankId << " gva=" << VaToStr(gva) << " hostVa="
                                                << VaToStr(hostVa) << " dva=" << VaToStr(dva) << " size=" << size);
    return BM_OK;
}

void AsymmetricMemSegment::ReleaseHostMapping(uint64_t hostVa) noexcept
{
    auto ret = DlHalApi::HalHostUnregisterEx(reinterpret_cast<void *>(hostVa), logicDeviceId_, HOST_MEM_MAP_DEV);
    if (ret != 0) {
        BM_LOG_ERROR("HalHostUnregisterEx failed: rankId=" << options_.rankId << " deviceId=" << logicDeviceId_
                                                           << " hostVa=" << VaToStr(hostVa) << " ret=" << ret);
    }
}

Result AsymmetricMemSegment::EnsurePeerAccess(uint32_t remotePhyId) noexcept
{
    if (remotePhyId == static_cast<uint32_t>(devicePhyId_) || enablePeerDevices_.test(remotePhyId)) {
        return BM_OK;
    }
    auto ret = EnableRemotePeerAccess(static_cast<int32_t>(remotePhyId));
    if (ret != BM_OK) {
        return ret;
    }
    enablePeerDevices_.set(remotePhyId);
    BM_LOG_DEBUG("enable peer access for : " << remotePhyId);
    return BM_OK;
}

Result AsymmetricMemSegment::ReleaseSliceMemory(const MemSlicePtr &slice) noexcept
{
    if (slice == nullptr) {
        BM_LOG_ERROR("input slice is nullptr.");
        return BM_INVALID_PARAM;
    }

    auto pos = registerSlices_.find(slice->index_);
    if (pos == registerSlices_.end()) {
        BM_LOG_ERROR("release slice : " << slice->index_ << " not exist.");
        return BM_INVALID_PARAM;
    }

    Result ret = BM_OK;
    if (pos->second.slice->memType_ == HYBM_MEM_TYPE_HOST) {
        if (pos->second.hostMapped) {
            ReleaseHostMapping(pos->second.slice->vAddress_);
        }
        // DRAM 的 AddVaInfo 登记 GVA/DVA/HVA 三表，释放时即时移除，防止 HalHostUnregisterEx 后 DVA 悬挂
        HybmVaManager::GetInstance().RemoveOneVaInfo(pos->second.slice->gva_);
    } else if (options_.shared) {
        ret = DlAclApi::RtIpcDestroyMemoryName(pos->second.name.c_str());
        if (ret != 0) {
            BM_LOG_ERROR("destroy memory name failed: rankId=" << options_.rankId << " name=" << pos->second.name
                                                               << " ret=" << ret);
            return BM_DL_FUNCTION_FAILED;
        }
    }

    registerSlices_.erase(pos);
    return ret;
}

Result AsymmetricMemSegment::Export(std::string &exInfo) noexcept
{
    HbmExportDeviceInfo info;
    info.devicePhyId = devicePhyId_;
    info.rankId = options_.rankId;
    info.pid = HybmDevLegacySegment::pid_;
    HybmDevLegacySegment::GetDeviceInfo(info.sdid, info.serverId, info.superPodId);
    auto ret = LiteralExInfoTranslater<HbmExportDeviceInfo>{}.Serialize(info, exInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("export info failed: " << ret);
        return BM_ERROR;
    }

    BM_LOG_DEBUG("export device info(sdid=" << sdid_ << " pid=" << pid_ << " rank=" << info.rankId
                                            << " deviceId=" << devicePhyId_ << ")");
    return BM_OK;
}

Result AsymmetricMemSegment::Export(const MemSlicePtr &slice, std::string &exInfo) noexcept
{
    auto pos = registerSlices_.find(slice->index_);
    if (pos == registerSlices_.end()) {
        BM_LOG_ERROR("export slice not exist: index=" << slice->index_);
        return BM_INVALID_PARAM;
    }
    return ExportSlice(pos->second, exInfo);
}

Result AsymmetricMemSegment::ExportSlice(const RegisterSlice &regSlice, std::string &exInfo) noexcept
{
    uint32_t sdId;
    UserSliceExportInfo info;
    info.segmentType =
        (regSlice.slice->memType_ == HYBM_MEM_TYPE_DEVICE) ? SEGMENT_TYPE_USER_DEV : SEGMENT_TYPE_USER_DRAM;
    // 多trans实例场景下,同一实例不同进程的gva_start不相同,所以仅传递offset
    info.gvaOffset = regSlice.slice->gva_ - reinterpret_cast<uint64_t>(globalVirtualAddress_);
    info.address = regSlice.slice->vAddress_;
    info.size = regSlice.slice->size_;
    info.devicePhyId = static_cast<uint32_t>(devicePhyId_);
    info.rankId = options_.rankId;
    HybmDevLegacySegment::GetDeviceInfo(sdId, info.serverId, info.superPodId);
    // HBM slice 携带 IPC name（shared 场景对端 RtIpcOpenMemory 用）；DRAM 为空串，拷贝 0 字节
    std::copy_n(regSlice.name.c_str(), std::min(regSlice.name.size(), sizeof(info.name) - 1), info.name);
    auto ret = LiteralExInfoTranslater<UserSliceExportInfo>{}.Serialize(info, exInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("export user slice failed: rankId=" << options_.rankId << " index=" << regSlice.slice->index_
                                                         << " ret=" << ret);
        return BM_ERROR;
    }

    BM_LOG_DEBUG("export user slice success. type:" << info.segmentType << " addr:" << VaToStr(info.address)
                                                    << " size:" << VaToStr(info.size) << " name:" << info.name
                                                    << " rank:" << options_.rankId);
    return BM_OK;
}

Result AsymmetricMemSegment::GetExportSliceSize(size_t &size) noexcept
{
    size = sizeof(UserSliceExportInfo);
    return BM_OK;
}

void AsymmetricMemSegment::RollbackImportedSlices(const std::vector<MemSlicePtr> &slices) noexcept
{
    if (!options_.shared) {
        return;
    }
    for (auto &slice : slices) {
        if (slice->memType_ != HYBM_MEM_TYPE_DEVICE || slice->vAddress_ == 0) {
            // 仅回滚真实打开过 IPC 映射的 DEVICE 条目；DRAM 条目及 GVA/nullptr 不能传给 RtIpcCloseMemory
            continue;
        }
        auto ret = DlAclApi::RtIpcCloseMemory(reinterpret_cast<void *>(slice->vAddress_));
        if (ret != 0) {
            BM_LOG_WARN("RtIpcCloseMemory failed: deviceId=" << devicePhyId_ << " addr=" << VaToStr(slice->vAddress_)
                                                             << " ret=" << ret);
        }
    }
}

Result AsymmetricMemSegment::Import(const std::vector<std::string> &allExInfo, void *addresses[]) noexcept
{
    if (allExInfo.empty()) {
        return BM_OK;
    }

    Result ret = BM_ERROR;
    uint32_t index = 0U;
    std::vector<MemSlicePtr> importedSlices; // 本次成功导入的远端 slice，失败时用于回滚 IPC 映射
    for (auto &info : allExInfo) {
        MemSlicePtr rms;
        auto magic = *reinterpret_cast<const uint64_t *>(info.data());
        if (magic == ENTITY_EXPORT_INFO_MAGIC) {
            ret = ImportDeviceInfo(info);
        } else if (magic == USER_MEM_SLICE_EXPORT_INFO_MAGIC) {
            ret = ImportSliceInfo(info, rms);
        } else {
            BM_LOG_ERROR("invalid import magic : " << magic);
            ret = BM_INVALID_PARAM;
        }
        if (rms != nullptr) {
            importedSlices.emplace_back(rms);
        }
        if (addresses == nullptr) {
            if (ret != BM_OK) {
                break;
            }
            // kv trans addresses is null need continue
            continue;
        }
        if (ret != BM_OK) {
            // rollback：仅针对真实打开过的 IPC 映射；addresses[] 中是 gva/nullptr，不能传给 RtIpcCloseMemory
            RollbackImportedSlices(importedSlices);
            break;
        }

        void *address = nullptr;
        if (rms != nullptr) {
            address = (void *)(ptrdiff_t)(rms->gva_);
        }
        addresses[index++] = address;
    }

    return ret;
}

Result AsymmetricMemSegment::RemoveImported(const std::vector<uint32_t> &ranks) noexcept
{
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto rankId : ranks) {
        importedDeviceInfo_.erase(rankId);
        RemoveSliceInfo(rankId);
    }
    uniqueLock.unlock();
    return BM_OK;
}

void AsymmetricMemSegment::RemoveSliceInfo(const uint32_t rankId) noexcept
{
    // Clear Imported SliceInfo
    auto it = rankToRemoteSlices_.find(rankId);
    if (it == rankToRemoteSlices_.end()) {
        return;
    }
    auto &remoteSliceVec = it->second;
    for (auto &remoteSlice : remoteSliceVec) {
        registerAddrs_.erase(reinterpret_cast<void *>(static_cast<ptrdiff_t>(remoteSlice->vAddress_)));
        HybmVaManager::GetInstance().RemoveOneVaInfo(remoteSlice->gva_);
        auto rIt = remoteSlices_.find(remoteSlice->index_);
        if (rIt == remoteSlices_.end()) {
            continue;
        }
        auto sIt = importedSliceInfo_.find(rIt->second.name);
        if (sIt == importedSliceInfo_.end()) {
            remoteSlices_.erase(remoteSlice->index_);
            continue;
        }
        auto &sliceInfo = sIt->second;
        if (options_.shared && CanSdmaReaches(sliceInfo.superPodId, sliceInfo.serverId, sliceInfo.devicePhyId)) {
            void *address = reinterpret_cast<void *>(static_cast<ptrdiff_t>(remoteSlice->vAddress_ << 16 >> 16));
            BM_LOG_INFO("RtIpcCloseMemory start address="
                        << address
                        << ", vAddress_ = " << reinterpret_cast<void *>(static_cast<ptrdiff_t>(remoteSlice->vAddress_))
                        << ", deviceId=" << devicePhyId_ << ", sliceInfo.devicePhyId=" << sliceInfo.devicePhyId
                        << ", sliceInfo.rankId=" << sliceInfo.rankId);
            auto ret = DlAclApi::RtIpcCloseMemory(address);
            if (ret != 0) {
                BM_LOG_WARN("Unable to close memory, address="
                            << address << ", vAddress_"
                            << reinterpret_cast<void *>(static_cast<ptrdiff_t>(remoteSlice->vAddress_))
                            << ", deviceId=" << devicePhyId_ << ", sliceInfo.devicePhyId=" << sliceInfo.devicePhyId
                            << ", sliceInfo.rankId=" << sliceInfo.rankId << ", ret:" << ret
                            << ", This may affect future memory registration.");
            }
        }
        BM_LOG_INFO("RemoveSliceInfo, rankId=" << rankId << ", remoteSlice->index_=" << remoteSlice->index_
                                               << ",slice name " << rIt->second.name);
        importedSliceInfo_.erase(rIt->second.name);
        remoteSlices_.erase(remoteSlice->index_);
    }
    rankToRemoteSlices_.erase(rankId);
}

Result AsymmetricMemSegment::Mmap() noexcept
{
    BM_LOG_ERROR("AsymmetricMemSegment NOT SUPPORT Mmap");
    return BM_NOT_SUPPORTED;
}

MemSlicePtr AsymmetricMemSegment::GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept
{
    MemSlicePtr target;
    auto index = MemSlice::GetIndexFrom(slice);
    auto pos = registerSlices_.find(index);
    if (pos != registerSlices_.end()) {
        target = pos->second.slice;
    } else if ((pos = remoteSlices_.find(index)) != remoteSlices_.end()) {
        target = pos->second.slice;
    } else {
        if (quiet) {
            BM_LOG_DEBUG("cannot get slice: " << slice);
        } else {
            BM_LOG_ERROR("cannot get slice: " << slice);
        }
        return nullptr;
    }

    if (!target->ValidateId(slice)) {
        return nullptr;
    }

    return target;
}

Result AsymmetricMemSegment::Unmap() noexcept
{
    BM_LOG_INFO("AsymmetricMemSegment NOT SUPPORT Unmap");
    return BM_NOT_SUPPORTED;
}

Result AsymmetricMemSegment::ImportDeviceInfo(const std::string &info) noexcept
{
    HbmExportDeviceInfo deviceInfo;
    LiteralExInfoTranslater<HbmExportDeviceInfo> translator;
    auto ret = translator.Deserialize(info, deviceInfo);
    if (ret != 0) {
        BM_LOG_ERROR("deserialize device info failed: " << ret);
        return ret;
    }

    if (deviceInfo.devicePhyId >= MAX_DEVICE_COUNT) {
        BM_LOG_ERROR("Invalid deviceInfo device id: " << deviceInfo.devicePhyId
                                                      << ", max: " << static_cast<int>(MAX_DEVICE_COUNT));
        return BM_ERROR;
    }

    ret = EnsurePeerAccess(deviceInfo.devicePhyId);
    if (ret != BM_OK) {
        return ret;
    }
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    if (options_.shared) {
        for (auto &it : registerSlices_) {
            if (it.second.name.empty()) {
                continue; // DRAM 注册内存无 IPC name，无需设置设备白名单
            }
            ret =
                DlAclApi::RtSetIpcMemorySuperPodPid(it.second.name.c_str(), deviceInfo.sdid, (int *)&deviceInfo.pid, 1);
            if (ret != 0) {
                BM_LOG_ERROR("RtSetIpcMemorySuperPodPid failed: name=" << it.second.name << " sdid=" << deviceInfo.sdid
                                                                       << " pid=" << deviceInfo.pid << " rankId="
                                                                       << deviceInfo.rankId << " ret=" << ret);
                return BM_DL_FUNCTION_FAILED;
            }
            BM_LOG_DEBUG("set whitelist for shm(" << it.second.name << ") sdid=" << deviceInfo.sdid
                                                  << " pid=" << deviceInfo.pid << " rank=" << deviceInfo.rankId
                                                  << " devId=" << deviceInfo.devicePhyId);
        }
    }

    importedDeviceInfo_.emplace(deviceInfo.rankId, deviceInfo);
    uniqueLock.unlock();
    return BM_OK;
}

Result AsymmetricMemSegment::ImportSliceInfo(const std::string &info, MemSlicePtr &remoteSlice) noexcept
{
    UserSliceExportInfo sliceInfo;
    LiteralExInfoTranslater<UserSliceExportInfo> translator;
    auto ret = translator.Deserialize(info, sliceInfo);
    if (ret != 0) {
        BM_LOG_ERROR("deserialize user slice info failed: " << ret);
        return ret;
    }

    if (sliceInfo.devicePhyId >= MAX_DEVICE_COUNT) {
        BM_LOG_ERROR("Invalid sliceInfo device id: " << sliceInfo.devicePhyId
                                                     << ", max: " << static_cast<int>(MAX_DEVICE_COUNT));
        return BM_ERROR;
    }

    // 介质判型：segmentType 由导出端按 slice 实际内存类型写入
    if (sliceInfo.segmentType == SEGMENT_TYPE_USER_DRAM) {
        return ImportDramSlice(sliceInfo, remoteSlice);
    }
    return ImportHbmSlice(sliceInfo, remoteSlice);
}

Result AsymmetricMemSegment::ImportHbmSlice(const UserSliceExportInfo &sliceInfo, MemSlicePtr &remoteSlice) noexcept
{
    Result ret = BM_OK;
    void *address = nullptr;
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    if (options_.shared && CanSdmaReaches(sliceInfo.superPodId, sliceInfo.serverId, sliceInfo.devicePhyId)) {
        ret = EnsurePeerAccess(sliceInfo.devicePhyId);
        if (ret != BM_OK) {
            return ret;
        }

        ret = DlAclApi::RtIpcOpenMemory(&address, sliceInfo.name);
        if (ret != 0) {
            BM_LOG_ERROR("IpcOpenMemory(" << sliceInfo.name << ") failed:" << ret << ",sdid=" << sdid_
                                          << ", pid=" << pid_ << ", deviceId=" << devicePhyId_
                                          << ", sliceInfo.devicePhyId=" << sliceInfo.devicePhyId);
            return BM_DL_FUNCTION_FAILED;
        }
        BM_LOG_INFO("IpcOpenMemory(" << sliceInfo.name << ") success, sdid=" << sdid_ << ", pid=" << pid_
                                     << ", deviceId=" << devicePhyId_
                                     << ", sliceInfo.devicePhyId=" << sliceInfo.devicePhyId);
        registerAddrs_.insert(address);
    } else if (options_.dataOpType &
               (HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_DEVICE_URMA | HYBM_DOP_TYPE_DEVICE_UBOE)) {
        address = nullptr;
    }

    remoteSlice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM,
                                             sliceInfo.gvaOffset + reinterpret_cast<uint64_t>(globalVirtualAddress_),
                                             reinterpret_cast<uint64_t>(address), sliceInfo.size);
    rankToRemoteSlices_[sliceInfo.rankId].push_back(remoteSlice);
    remoteSlices_.emplace(remoteSlice->index_, RegisterSlice{remoteSlice, sliceInfo.name});
    importedSliceInfo_.emplace(sliceInfo.name, sliceInfo);
    uniqueLock.unlock();
    ret = HybmVaManager::GetInstance().AddVaInfoFromExternal(
        {remoteSlice->gva_, remoteSlice->vAddress_, 0, remoteSlice->size_, HYBM_MEM_TYPE_DEVICE}, options_.rankId,
        sliceInfo.rankId);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK, "ret = " << ret, ret);
    return BM_OK;
}

Result AsymmetricMemSegment::ImportDramSlice(const UserSliceExportInfo &sliceInfo, MemSlicePtr &remoteSlice) noexcept
{
    Result ret = BM_OK;
    // 远端 host 内存本地不可映射，不做 RtEnableP2P/RtIpcOpenMemory，数据面走 transport MR
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    remoteSlice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM,
                                             sliceInfo.gvaOffset + reinterpret_cast<uint64_t>(globalVirtualAddress_), 0,
                                             sliceInfo.size);
    rankToRemoteSlices_[sliceInfo.rankId].push_back(remoteSlice);
    remoteSlices_.emplace(remoteSlice->index_, RegisterSlice{remoteSlice, "", false});
    uniqueLock.unlock();
    ret = HybmVaManager::GetInstance().AddVaInfoFromExternal(
        {remoteSlice->gva_, 0, 0, remoteSlice->size_, HYBM_MEM_TYPE_HOST}, options_.rankId, sliceInfo.rankId);
    BM_ASSERT_LOG_AND_RETURN(ret == BM_OK,
                             "AddVaInfoFromExternal failed: rankId=" << options_.rankId
                                                                     << " remoteRankId=" << sliceInfo.rankId << " gva="
                                                                     << VaToStr(remoteSlice->gva_) << " ret=" << ret,
                             ret);
    BM_LOG_INFO("import dram slice success: rankId=" << sliceInfo.rankId << " gva=" << VaToStr(remoteSlice->gva_)
                                                     << " size=" << VaToStr(sliceInfo.size));
    return BM_OK;
}

void AsymmetricMemSegment::CloseMemory() noexcept
{
    if (options_.shared) {
        for (auto &addr : registerAddrs_) {
            if (DlAclApi::RtIpcCloseMemory(addr) != 0) {
                BM_LOG_WARN("Unable to close memory. This may affect future memory registration.");
            }
        }
    }
    for (auto &it : registerSlices_) {
        if (it.second.hostMapped) {
            // 段级销毁兜底：未逐个 ReleaseSliceMemory 的 hostMapped DRAM slice 需对称解除设备映射
            ReleaseHostMapping(it.second.slice->vAddress_);
        }
        HybmVaManager::GetInstance().RemoveOneVaInfo(it.second.slice->gva_);
    }
    registerAddrs_.clear();
    if (globalVirtualAddress_ != nullptr) {
        HybmVaManager::GetInstance().FreeReserveGva(reinterpret_cast<uint64_t>(globalVirtualAddress_));
    }
    globalVirtualAddress_ = lvaBase_ = nullptr;
    totalVirtualSize_ = 0;
    BM_LOG_INFO("close memory finish.");
}

bool AsymmetricMemSegment::MemoryInRange(const void *begin, uint64_t size) const noexcept
{
    if (begin < globalVirtualAddress_) {
        return false;
    }

    if (reinterpret_cast<const uint8_t *>(begin) + size > globalVirtualAddress_ + totalVirtualSize_) {
        return false;
    }

    return true;
}

bool AsymmetricMemSegment::CheckSdmaReaches(uint32_t rankId) const noexcept
{
    auto pos = importedDeviceInfo_.find(rankId);
    if (pos == importedDeviceInfo_.end()) {
        return false;
    }

    uint32_t sdId;
    uint32_t serverId;
    uint32_t superPodId;
    HybmDevLegacySegment::GetDeviceInfo(sdId, serverId, superPodId);

    if (pos->second.serverId == serverId) {
        return true;
    }

    if (pos->second.superPodId == invalidSuperPodId || superPodId == invalidSuperPodId) {
        return false;
    }

    return pos->second.superPodId == superPodId;
}

} // namespace mf
} // namespace ock
