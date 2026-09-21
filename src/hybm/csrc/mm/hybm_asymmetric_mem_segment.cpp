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

#include <algorithm>
#include <cstring>

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
    auto gvaInfo = HybmVaManager::GetInstance().AllocReserveGva(
        options_.rankId, totalVirtualSize_, 0, HYBM_MEM_TYPE_DEVICE, options_.enable56BitsGva, options_.scene);
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
            (void)UserMemDestroyName(name.c_str());
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
               attr.memType == DV_MEM_LOCK_DEV_DVPP || attr.memType == DV_MEM_LOCK_HOST;
    }
    BM_LOG_DEBUG("DrvMemGetAttribute unavailable, fallback to address check: va=" << VaToStr(va));
#endif
    return IsHbmAddr(va);
}
} // namespace

namespace {
// 共享名类型提取：空名/未知首字节返回 0（调用方按 no-op 处理）
inline uint16_t UserMemNameType(const char *name)
{
    return name == nullptr ? 0U : static_cast<uint16_t>(static_cast<uint8_t>(name[0]));
}

inline uint16_t UserMemNameType(const std::string &name)
{
    return name.empty() ? 0U : static_cast<uint16_t>(static_cast<uint8_t>(name[0]));
}

// 日志用共享名预览：IPC 按跳过类型字节的 C 字符串打印；VMM share_handle 为二进制（可含 \0），
// 仅打印类型标识与前 8 字节十六进制，避免二进制数据污染日志
std::string UserMemNamePreview(const char *name)
{
    if (name == nullptr) {
        return "<null>";
    }
    if (UserMemNameType(name) != USER_HBM_NAME_TYPE_VMM) {
        return std::string(name + 1);
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string preview = "vmm:";
    for (size_t i = 1; i <= 8; ++i) {
        const auto byte = static_cast<uint8_t>(name[i]);
        preview.push_back(kHex[byte >> 4]);
        preview.push_back(kHex[byte & 0xF]);
    }
    return preview;
}

std::string UserMemNamePreview(const std::string &name)
{
    return UserMemNamePreview(name.c_str());
}

// 释放 VMM handle 的静默封装：失败仅告警（引用泄漏风险），不打断调用方主流程
void ReleaseVmmHandleQuietly(drv_mem_handle_t *handle)
{
    if (handle == nullptr) {
        return;
    }
    const auto ret = DlHalApi::HalMemRelease(handle);
    if (ret != 0) {
        BM_LOG_WARN("HalMemRelease failed, handle may leak: ret=" << ret);
    }
}
} // namespace

Result AsymmetricMemSegment::RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    if (addr == nullptr || size == 0) {
        BM_LOG_ERROR("input address parameter is invalid, rankId=" << options_.rankId);
        return BM_INVALID_PARAM;
    }

    // 同一已注册区间内的重复注册复用既有 slice（首次调用注册生效，后续跳过），
    // 避免 offload 池多段注册触发 2MiB 对齐重叠与重复导出；对端按首个锚点+偏移寻址
    const auto va = reinterpret_cast<uint64_t>(addr);
    for (auto &it : registerSlices_) {
        const auto &exist = it.second.slice;
        const auto base = exist->vAddress_;
        if (va >= base && va + size <= base + exist->size_) {
            BM_LOG_INFO("fake register, reuse existing slice: rankId=" << options_.rankId << " addr=" << VaToStr(va)
                                                                       << " size=" << size << " sliceIdx=" << it.first);
            slice = exist;
            return BM_OK;
        }
    }

    //offload 池内存（GVM 窗口）首次注册时，向 VaManager 查分配时登记的整段区间，
    // 以整个 allocation 的 [base, size) 注册（单分配=单 handle=单锚点）；后续落在该区间内的
    // 注册由上方复用逻辑跳过
    uint64_t regBase = va;
    uint64_t regSize = size;
    const uint64_t gvmStart = socType_ == AscendSocType::ASCEND_950 ? HYBM_GVM_START_ADDR_A5 : HYBM_GVM_START_ADDR;
    if (va >= gvmStart && va < gvmStart + HYBM_GVM_MAX_POOL_SIZE) {
        // 分配记录 HVA/DVA=allocAddr；导入记录 HVA=0、DVA=lva——按 DVA 查询对两种 rank 均命中
        auto [allocInfo, found] = HybmVaManager::GetInstance().FindAllocByVa(va, HVM_DVA);
        if (found && allocInfo.base.va[HVM_DVA] != 0) {
            regBase = allocInfo.base.va[HVM_DVA];
            regSize = allocInfo.base.size;
            BM_LOG_INFO("expand registration to alloc interval: rankId=" << options_.rankId << " addr=" << VaToStr(va)
                                                                         << " size=" << size << " regBase="
                                                                         << VaToStr(regBase) << " regSize=" << regSize);
        }
    }

    if (allocatedSize_ + regSize > totalVirtualSize_) {
        BM_LOG_ERROR("gva space exhausted: rankId=" << options_.rankId << " allocated=" << allocatedSize_
                                                    << " request=" << regSize << " total=" << totalVirtualSize_);
        return BM_ERROR;
    }

    if (IsDeviceMemoryAddr(reinterpret_cast<uint64_t>(regBase))) {
        return RegisterDeviceMemory(reinterpret_cast<const void *>(regBase), regSize, slice);
    }
    return RegisterHostMemory(reinterpret_cast<const void *>(regBase), regSize, slice);
}

Result AsymmetricMemSegment::RegisterDeviceMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    char name[USER_HBM_NAME_MAX_LEN]{};
    Result ret = BM_OK;
    if (options_.shared) {
        ret = UserMemExportName(addr, size, name);
        if (ret != BM_OK) {
            // 根因日志已在 UserMemExportName 内记录
            return ret;
        }
    }
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto &remoteDev : importedDeviceInfo_) {
        if (!options_.shared ||
            !CanSdmaReaches(remoteDev.second.superPodId, remoteDev.second.serverId, remoteDev.second.devicePhyId)) {
            continue;
        }
        ret = UserMemSetSuperPodPid(name, remoteDev.second.sdid, remoteDev.second.pid);
        if (ret != BM_OK) {
            // 根因日志已在 UserMemSetSuperPodPid 内记录
            (void)UserMemDestroyName(name);
            return BM_DL_FUNCTION_FAILED;
        }
    }

    uint64_t gva = reinterpret_cast<uint64_t>(lvaBase_) + allocatedSize_;
    slice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM, gva,
                                       reinterpret_cast<uint64_t>(addr), size);
    // host_rdma: write DVA/HVA maps (bare device addr) to match ConnBasedSegment's va-table layout;
    // sdma/device_rdma: keep onlyGva=true to preserve the IPC/VMM-based sharing mechanism.
    const bool onlyGva = (options_.dataOpType & HYBM_DOP_TYPE_HOST_RDMA) == 0U;
    ret = HybmVaManager::GetInstance().AddVaInfo({gva, slice->vAddress_, slice->vAddress_, size, HYBM_MEM_TYPE_DEVICE},
                                                 options_.rankId, onlyGva);
    if (ret != 0) {
        BM_LOG_ERROR("AddVaInfo failed: rankId=" << options_.rankId << " gva=" << VaToStr(gva) << " size=" << size
                                                 << " ret=" << ret);
        if (options_.shared) {
            (void)UserMemDestroyName(name);
        }
        slice = nullptr;
        return ret;
    }

    // VMM share_handle 为二进制（可含 \0），std::string 必须定长构造，禁止隐式 strlen 截断
    const std::string shareName = options_.shared ? std::string(name, USER_HBM_NAME_MAX_LEN) : std::string{};
    memNames_.emplace_back(shareName);
    registerSlices_.emplace(slice->index_, RegisterSlice{slice, shareName});
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
        ret = UserMemDestroyName(pos->second.name.c_str());
        if (ret != BM_OK) {
            // 根因日志已在 UserMemDestroyName 内记录
            return ret;
        }
        // 名称已即时销毁，需从 UnReserveMemorySpace 的待销毁列表移除，避免重复销毁
        auto nameIt = std::find(memNames_.begin(), memNames_.end(), pos->second.name);
        if (nameIt != memNames_.end()) {
            memNames_.erase(nameIt);
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
    // HBM shared 携带带类型标记的定长共享名（129B，VMM share_handle 可含 \0，必须整段拷贝防截断）；
    // DRAM/非 shared 为空名，info.name 保持全零
    if (!regSlice.name.empty()) {
        std::memcpy(info.name, regSlice.name.data(), USER_HBM_NAME_MAX_LEN);
    }
    auto ret = LiteralExInfoTranslater<UserSliceExportInfo>{}.Serialize(info, exInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("export user slice failed: rankId=" << options_.rankId << " index=" << regSlice.slice->index_
                                                         << " ret=" << ret);
        return BM_ERROR;
    }

    BM_LOG_DEBUG("export user slice success. type:" << info.segmentType << " addr:" << VaToStr(info.address) << " size:"
                                                    << VaToStr(info.size) << " name:" << UserMemNamePreview(info.name)
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
        // 按登记类型回滚（IPC close / VMM unmap+release）；未登记或空名条目为 no-op，
        // DRAM 与 GVA-only 条目不会被误传给 RtIpcCloseMemory
        UserMemUnImportName(slice->index_);
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
        HybmVaManager::GetInstance().RemoveOneVaInfo(remoteSlice->gva_);
        auto rIt = remoteSlices_.find(remoteSlice->index_);
        if (rIt == remoteSlices_.end()) {
            continue;
        }
        if (options_.shared) {
            // 按登记类型解除映射（IPC close / VMM unmap+release）；未实际打开的条目 vAddress_=0 为 no-op
            UserMemUnImportName(remoteSlice->index_);
        }
        BM_LOG_INFO("RemoveSliceInfo, rankId=" << rankId << ", remoteSlice->index_=" << remoteSlice->index_
                                               << ", slice name " << UserMemNamePreview(rIt->second.name));
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
                continue; // DRAM/非 shared 注册内存无共享名；VMM 已设 NO_WLIST 免白名单，helper 内 no-op
            }
            ret = UserMemSetSuperPodPid(it.second.name.c_str(), deviceInfo.sdid, deviceInfo.pid);
            if (ret != BM_OK) {
                // 根因日志已在 UserMemSetSuperPodPid 内记录
                return ret;
            }
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

        // IPC 名打开映射；VMM share_handle 导入 + 预留 LVA + 映射（根因日志均在 helper 内记录）
        ret = UserMemImportAndMapName(&address, sliceInfo);
        if (ret != BM_OK) {
            return ret;
        }
    } else if (options_.dataOpType &
               (HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_DEVICE_URMA | HYBM_DOP_TYPE_DEVICE_UBOE)) {
        address = nullptr;
    }

    remoteSlice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM,
                                             sliceInfo.gvaOffset + reinterpret_cast<uint64_t>(globalVirtualAddress_),
                                             reinterpret_cast<uint64_t>(address), sliceInfo.size);
    rankToRemoteSlices_[sliceInfo.rankId].push_back(remoteSlice);
    // name 定长构造：VMM share_handle 为二进制，隐式 strlen 会在首个 \0 截断导致 find/erase 失配
    const std::string shareName(sliceInfo.name, USER_HBM_NAME_MAX_LEN);
    remoteSlices_.emplace(remoteSlice->index_, RegisterSlice{remoteSlice, shareName});
    importedSliceInfo_.emplace(shareName, sliceInfo);
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
        for (auto &it : remoteSlices_) {
            // 按登记类型解除映射（IPC close / VMM unmap+release）
            UserMemUnImportName(it.first);
        }
        remoteSlices_.clear();
    }
    for (auto &it : registerSlices_) {
        if (it.second.hostMapped) {
            // 段级销毁兜底：未逐个 ReleaseSliceMemory 的 hostMapped DRAM slice 需对称解除设备映射
            ReleaseHostMapping(it.second.slice->vAddress_);
        }
        HybmVaManager::GetInstance().RemoveOneVaInfo(it.second.slice->gva_);
    }
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

// ====================== IPC/VMM 双路径共享 helper 实现 ======================

Result AsymmetricMemSegment::UserMemExportName(const void *ptr, uint64_t len, char *name) noexcept
{
    //   A3：HBM 地址窗口内的用户态设备内存（torch/acl 直接分配，LOCK_DEV 型，
    //   驱动不支持 HalMemRetainAllocationHandle）走 IPC 名路径；窗口外的 host 内存
    //   （offload 池等，GVM 窗口 0x28 开头，可 retain+export）走 VMM share_handle 路径。
    //   注意 A3 驱动对 host 用户态内存的 DrvMemGetAttribute 也返回 LOCK_DEV(0x10)，
    //   属性查询无法区分介质，只能按地址窗口判型。
    const bool useVmm = !IsHbmAddr(reinterpret_cast<uint64_t>(ptr));
    name[0] = static_cast<char>(useVmm ? USER_HBM_NAME_TYPE_VMM : USER_HBM_NAME_TYPE_IPC);
    if (!useVmm) {
        auto ret = DlAclApi::RtIpcSetMemoryName(ptr, len, name + 1, DEVICE_SHM_NAME_SIZE + 1);
        if (ret != BM_OK) {
            BM_LOG_ERROR("set memory name failed: rankId=" << options_.rankId
                                                           << " addr=" << VaToStr(reinterpret_cast<uint64_t>(ptr))
                                                           << " size=" << len << " ret=" << ret);
            return BM_DL_FUNCTION_FAILED;
        }
        return BM_OK;
    }
    return ExportVmmShareName(ptr, name);
}

Result AsymmetricMemSegment::ExportVmmShareName(const void *ptr, char *name) noexcept
{
    const auto addrStr = VaToStr(reinterpret_cast<uint64_t>(ptr));
    ShareHandleStr shareInfo;
    // 本地已登记该地址的 share handle 时直接复用并跳过 retain/export：SHARED 池仅分配 rank
    // 能 export 成功，其他 rank 的同 VA 是导入映射（句柄在导入时登记），对其 export 会报错
    if (HybmVaManager::GetInstance().GetHandle(ptr, shareInfo) == 0) {
        BM_LOG_INFO("reuse registered share handle, skip export: rankId=" << options_.rankId << " addr=" << addrStr);
        std::memcpy(name + 1, shareInfo.data(), shareInfo.size());
        return BM_OK;
    }
    drv_mem_handle_t *handle = nullptr;
    auto ret = DlHalApi::HalMemRetainAllocationHandle(&handle, const_cast<void *>(ptr));
    if (ret != BM_OK || handle == nullptr) {
        BM_LOG_ERROR("HalMemRetainAllocationHandle failed: rankId=" << options_.rankId << " addr=" << addrStr
                                                                    << " ret=" << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    // halMemExport 的 flags 实测必须传 0：传 ACL 的 DISABLE_PID_VALIDATION(0x1) 返回 65534 被拒，
    // 该 flag 语义仅在 ACL 层 aclrtMemExportToShareableHandleV2 支持；免白名单当前以
    // SetAttribute 降级（WARN）换取链路验证，白名单拦截时再切 ACL 层 export 链路
    MemShareHandle shareHandle{}; // 驱动出参，HalMemExport 成功后填充
    ret = DlHalApi::HalMemExport(handle, MEM_HANDLE_TYPE_FABRIC, 0, &shareHandle);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemExport failed: rankId=" << options_.rankId << " addr=" << addrStr << " ret=" << ret);
        ReleaseVmmHandleQuietly(handle);
        return BM_DL_FUNCTION_FAILED;
    }
    // 实测：export 后立即 release 会导致同 allocation 的后续 slice retain/export 全部 PARA_ERROR，
    // share_handle 疑似依赖 retain 引用存活，故 retain 引用保持不释放（销毁对称待段级统一处理）；
    // trans/setAttr 失败路径仍对称释放
    uint64_t shareable = 0U;
    uint32_t sId = 0;
    ret = DlHalApi::HalMemTransShareableHandle(MEM_HANDLE_TYPE_FABRIC, &shareHandle, &sId, &shareable);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemTransShareableHandle failed: rankId=" << options_.rankId << " addr=" << addrStr
                                                                  << " ret=" << ret);
        ReleaseVmmHandleQuietly(handle);
        return BM_ERROR;
    }
    BM_LOG_INFO("trans shareable ok: rankId=" << options_.rankId << " shareable=0x" << std::hex << shareable
                                              << " serverId=" << std::dec << sId);
    struct ShareHandleAttr attr = {.enableFlag = SHR_HANDLE_NO_WLIST_ENABLE, .rsv = {0}};
    ret = DlHalApi::HalMemShareHandleSetAttribute(shareable, SHR_HANDLE_ATTR_NO_WLIST_IN_SERVER, attr);
    if (ret != BM_OK) {
        // A5 上 ACL 物理分配来源的 shareable 可能不被 HAL 属性接口接受（实测 PARA_ERROR）；
        // 免白名单已由 export flag 承担，此处降级为告警不阻断；若对端 import 被白名单拒绝再回退本步
        BM_LOG_WARN("HalMemShareHandleSetAttribute degraded: rankId=" << options_.rankId << " shareable=0x" << std::hex
                                                                      << shareable << " ret=" << std::dec << ret);
    }
    std::memcpy(name + 1, shareHandle.share_info, MEM_SHARE_HANDLE_LEN);
    return BM_OK;
}

Result AsymmetricMemSegment::UserMemSetSuperPodPid(const char *name, uint32_t sdid, uint32_t pid) noexcept
{
    // 仅 IPC 名需要 superPod/pid 白名单；VMM 共享已设 NO_WLIST 免白名单，空名（DRAM/非 shared）无需处理
    if (UserMemNameType(name) != USER_HBM_NAME_TYPE_IPC) {
        return BM_OK;
    }
    int32_t pidArg = static_cast<int32_t>(pid);
    auto ret = DlAclApi::RtSetIpcMemorySuperPodPid(name + 1, sdid, &pidArg, 1);
    if (ret != BM_OK) {
        BM_LOG_ERROR("set shm(" << name + 1 << ") for sdid=" << sdid << " pid=" << pid << " failed: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    BM_LOG_INFO("set shm(" << name + 1 << ") for sdid=" << sdid << " pid=" << pid << " success.");
    return BM_OK;
}

namespace {
// reserve flags 候选（按优先级）：该驱动（25.7.0/ascendhal 7.35.23）实测 reserve 的 flags 为页型
// 标志（1=HUGE）而非旧驱动的 MEM_RSV_TYPE_REMOTE_MAP 语义，且 expectPtr 可能为空才被接受；
// 逐候选探测，失败降级。useExpect=false 时采用驱动自选地址。
struct ReserveFlagCandidate {
    uint64_t flags;
    bool useExpect;
};
const ReserveFlagCandidate kReserveCandidates[] = {
    {MEM_RSV_TYPE_REMOTE_MAP, true}, // 旧语义：REMOTE_MAP + 指定 VaManager 预留地址
    {1, true},                       // 页型(HUGE) + 指定地址
    {1, false},                      // 页型(HUGE) + 驱动自选地址（放弃 VaManager 预留）
    {0, false},
};

// 单次 reserve 尝试：成功且地址符合期望返回 lva；地址不符时释放驱动侧预留返回 0 表示换候选
uint64_t TryHalReserve(uint64_t size, uint64_t expectLva, uint64_t flags, bool useExpect)
{
    void *lva = nullptr;
    void *expect = useExpect ? reinterpret_cast<void *>(expectLva) : nullptr;
    auto ret = DlHalApi::HalMemAddressReserve(&lva, size, 0, expect, flags);
    if (ret != 0) {
        return 0;
    }
    const auto got = reinterpret_cast<uint64_t>(lva);
    if (useExpect && got != expectLva) {
        (void)DlHalApi::HalMemAddressFree(lva); // 驱动返回了非请求地址：释放该预留，尝试下一候选
        return 0;
    }
    return got;
}
} // namespace

uint64_t AsymmetricMemSegment::ReserveLva(const UserSliceExportInfo &info) noexcept
{
    auto reserved =
        HybmVaManager::GetInstance().AllocReserveLva(options_.rankId, info.size, HVM_DVA, HYBM_MEM_TYPE_DEVICE);
    const auto reservedLva = reserved.va[HVM_DVA];
    if (reservedLva == 0) {
        BM_LOG_ERROR("AllocReserveLva failed: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                                       << " size=" << info.size);
        return 0;
    }
    for (const auto &candidate : kReserveCandidates) {
        auto lva = TryHalReserve(info.size, reservedLva, candidate.flags, candidate.useExpect);
        if (lva == 0) {
            continue;
        }
        if (!candidate.useExpect) {
            // 驱动自选地址：归还 VaManager 的预留，后续映射/登记均采用驱动地址
            HybmVaManager::GetInstance().FreeReserveLva(reservedLva, HVM_DVA);
        }
        BM_LOG_INFO("reserve lva ok: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                              << " lva=" << VaToStr(lva) << " flags=0x" << std::hex << candidate.flags
                                              << " useExpect=" << std::dec << candidate.useExpect);
        return lva;
    }
    BM_LOG_ERROR("HalMemAddressReserve failed for all candidates: rankId=" << options_.rankId << " remoteRank="
                                                                           << info.rankId << " size=" << info.size
                                                                           << " reservedVa=" << VaToStr(reservedLva));
    HybmVaManager::GetInstance().FreeReserveLva(reservedLva, HVM_DVA);
    return 0;
}

Result AsymmetricMemSegment::UserMemImportAndMapName(void **ptr, const UserSliceExportInfo &info) noexcept
{
    const uint16_t type = UserMemNameType(info.name);
    if (type == USER_HBM_NAME_TYPE_IPC) {
        auto ret = DlAclApi::RtIpcOpenMemory(ptr, info.name + 1);
        if (ret != BM_OK) {
            BM_LOG_ERROR("IpcOpenMemory(" << info.name + 1 << ") failed: rankId=" << options_.rankId
                                          << " sdid=" << sdid_ << " pid=" << pid_ << " deviceId=" << devicePhyId_
                                          << " remoteDeviceId=" << info.devicePhyId << " ret=" << ret);
            return BM_DL_FUNCTION_FAILED;
        }
        BM_LOG_INFO("IpcOpenMemory(" << info.name + 1 << ") success: rankId=" << options_.rankId
                                     << " deviceId=" << devicePhyId_);
        return BM_OK;
    }
    if (type != USER_HBM_NAME_TYPE_VMM) {
        // 未知类型（含 0）：exchange 数据损坏或对端版本不匹配，必须显式报错而非静默跳过
        BM_LOG_ERROR("unknown user mem name type: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                                           << " type=" << type);
        return BM_INVALID_PARAM;
    }
    return ImportAndMapVmmName(ptr, info);
}

Result AsymmetricMemSegment::ImportAndMapVmmName(void **ptr, const UserSliceExportInfo &info) noexcept
{
    MemShareHandle shareHandle{};
    std::memcpy(shareHandle.share_info, info.name + 1, MEM_SHARE_HANDLE_LEN);
    drv_mem_handle_t *handle = nullptr;
    auto ret = DlHalApi::HalMemImport(MEM_HANDLE_TYPE_FABRIC, &shareHandle, logicDeviceId_, &handle);
    if (ret != BM_OK || handle == nullptr) {
        BM_LOG_ERROR("HalMemImport failed: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                                    << " deviceId=" << logicDeviceId_ << " ret=" << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    const auto lva = ReserveLva(info);
    if (lva == 0) {
        // 预留失败的根因已在 ReserveLva 记录；imported handle 不能泄漏
        ReleaseVmmHandleQuietly(handle);
        return BM_ERROR;
    }
    ret = DlHalApi::HalMemMap(reinterpret_cast<void *>(lva), info.size, 0, handle, 0);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemMap failed: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                                 << " lva=" << VaToStr(lva) << " size=" << info.size << " ret=" << ret);
        ReleaseVmmHandleQuietly(handle);
        HybmVaManager::GetInstance().FreeReserveLva(lva, HVM_DVA);
        return BM_DL_FUNCTION_FAILED;
    }
    // 授权本地设备对导入内存的读写权限：A5 上 import 映射默认无写权限，
    // SDMA 写会被硬件静默丢弃（实测 ret=0 但数据未落地），须显式 SetAccess
    struct drv_mem_access_desc accessDesc[1] = {};
    accessDesc[0].location.side = MEM_DEV_SIDE;
    accessDesc[0].location.id = logicDeviceId_;
    accessDesc[0].type = MEM_ACCESS_TYPE_READWRITE;
    ret = DlHalApi::HalMemSetAccess(reinterpret_cast<void *>(lva), info.size, accessDesc, 1);
    if (ret != BM_OK) {
        BM_LOG_ERROR("HalMemSetAccess failed: rankId=" << options_.rankId << " remoteRank=" << info.rankId << " lva="
                                                       << VaToStr(lva) << " size=" << info.size << " ret=" << ret);
        ReleaseVmmHandleQuietly(handle);
        HybmVaManager::GetInstance().FreeReserveLva(lva, HVM_DVA);
        return BM_DL_FUNCTION_FAILED;
    }
    vmmNameToHandle_[std::string(info.name, USER_HBM_NAME_MAX_LEN)] = handle;
    *ptr = reinterpret_cast<void *>(lva);
    BM_LOG_INFO("import vmm slice success: rankId=" << options_.rankId << " remoteRank=" << info.rankId
                                                    << " lva=" << VaToStr(lva) << " size=" << info.size);
    return BM_OK;
}

void AsymmetricMemSegment::CloseRemoteIpcMemory(const RegisterSlice &reg) noexcept
{
    // RtIpcOpenMemory 返回的地址高 16 位带标记位，close 时需掩码还原（历史行为，保持不变）
    void *address = reinterpret_cast<void *>(static_cast<ptrdiff_t>(reg.slice->vAddress_ << 16 >> 16));
    auto ret = DlAclApi::RtIpcCloseMemory(address);
    if (ret != 0) {
        BM_LOG_WARN("RtIpcCloseMemory failed: deviceId=" << devicePhyId_ << " addr=" << VaToStr(reg.slice->vAddress_)
                                                         << " ret=" << ret << ", This may affect future registration.");
    }
}

void AsymmetricMemSegment::UnmapRemoteVmmMemory(const RegisterSlice &reg) noexcept
{
    const auto lva = reg.slice->vAddress_;
    auto ret = DlHalApi::HalMemUnmap(reinterpret_cast<void *>(static_cast<ptrdiff_t>(lva)));
    if (ret != 0) {
        BM_LOG_WARN("HalMemUnmap failed: deviceId=" << devicePhyId_ << " lva=" << VaToStr(lva) << " ret=" << ret);
    }
    HybmVaManager::GetInstance().FreeReserveLva(lva, HVM_DVA);
    auto it = vmmNameToHandle_.find(reg.name);
    if (it == vmmNameToHandle_.end()) {
        BM_LOG_WARN("vmm handle not found on unimport, may leak: deviceId=" << devicePhyId_ << " lva=" << VaToStr(lva));
        return;
    }
    ReleaseVmmHandleQuietly(it->second);
    vmmNameToHandle_.erase(it);
}

void AsymmetricMemSegment::UserMemUnImportName(uint32_t sliceId) noexcept
{
    auto rIt = remoteSlices_.find(sliceId);
    if (rIt == remoteSlices_.end()) {
        return; // 未登记条目（含尚未登记的回滚场景）为 no-op
    }
    const auto &reg = rIt->second;
    if (reg.slice == nullptr || reg.slice->vAddress_ == 0) {
        return; // 未实际打开映射（不可达/非 shared/DRAM），无资源可释放
    }
    const auto type = UserMemNameType(reg.name);
    if (type == USER_HBM_NAME_TYPE_IPC) {
        CloseRemoteIpcMemory(reg);
    } else if (type == USER_HBM_NAME_TYPE_VMM) {
        UnmapRemoteVmmMemory(reg);
    }
}

Result AsymmetricMemSegment::UserMemDestroyName(const char *name) noexcept
{
    // 仅 IPC 名有可销毁的共享名；VMM 导出侧 retain 引用已在导出时释放，无资源可清理
    if (UserMemNameType(name) != USER_HBM_NAME_TYPE_IPC) {
        return BM_OK;
    }
    auto ret = DlAclApi::RtIpcDestroyMemoryName(name + 1);
    if (ret != BM_OK) {
        BM_LOG_ERROR("destroy memory name failed: rankId=" << options_.rankId << " name=" << name + 1
                                                           << " ret=" << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    return BM_OK;
}

} // namespace mf
} // namespace ock
