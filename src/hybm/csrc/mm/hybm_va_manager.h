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
#ifndef MEM_FABRIC_HYBRID_HYBM_VA_MANAGER_H
#define MEM_FABRIC_HYBRID_HYBM_VA_MANAGER_H

#include <map>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <iomanip>
#include <cstring>
#include "hybm_common_include.h"
#include "hybm_mem_common.h"
#include "dl_hal_api_def.h"

namespace ock {
namespace mf {
enum HybmVaMgrType : uint32_t {
    HVM_GVA = 0,
    HVM_DVA = 1, // device va
    HVM_HVA = 2, // host va
    HVM_BUTT
};

enum AddrType { GLOBAL_DEVICE = 0, GLOBAL_HOST, LOCAL_DEVICE, LOCAL_HOST, ADDRESS_CATEGORY_BUTT };

inline std::ostream &operator<<(std::ostream &os, hybm_mem_type obj)
{
    switch (obj) {
        case HYBM_MEM_TYPE_DEVICE:
            return os << "DEVICE";
        case HYBM_MEM_TYPE_HOST:
            return os << "HOST";
        case HYBM_MEM_TYPE_BUTT:
            return os << "BUTT";
        default:
            return os << "UNKNOWN(" << static_cast<unsigned>(obj) << ")";
    }
}

inline std::ostream &operator<<(std::ostream &os, hybm_type obj)
{
    switch (obj) {
        case HYBM_TYPE_AI_CORE_INITIATE:
            return os << "AI_CORE_INITIATE";
        case HYBM_TYPE_HOST_INITIATE:
            return os << "HOST_INITIATE";
        case HYBM_TYPE_AICPU_INITIATE:
            return os << "AICPU_INITIATE";
        case HYBM_TYPE_BUTT:
            return os << "BUTT";
        default:
            return os << "UNKNOWN(" << static_cast<unsigned>(obj) << ")";
    }
}

inline std::ostream &operator<<(std::ostream &os, hybm_scene obj)
{
    switch (obj) {
        case HYBM_SCENE_DEFAULT:
            return os << "DEFAULT";
        case HYBM_SCENE_TRANS:
            return os << "TRANS";
        case HYBM_SCENE_SHM:
            return os << "SHM";
        case HYBM_SCENE_BUTT:
            return os << "BUTT";
        default:
            return os << "UNKNOWN(" << static_cast<unsigned>(obj) << ")";
    }
}

inline std::ostream &operator<<(std::ostream &os, hybm_role_type obj)
{
    switch (obj) {
        case HYBM_ROLE_PEER:
            return os << "PEER";
        case HYBM_ROLE_SENDER:
            return os << "SENDER";
        case HYBM_ROLE_RECEIVER:
            return os << "RECEIVER";
        case HYBM_ROLE_BUTT:
            return os << "BUTT";
        default:
            return os << "UNKNOWN(" << static_cast<unsigned>(obj) << ")";
    }
}

inline std::ostream &operator<<(std::ostream &os, const hybm_tls_config &obj)
{
    os << "TlsConfig{enable: " << obj.tlsEnable;
    if (obj.tlsEnable) {
        os << ", caPath: " << obj.caPath << ", crlPath: " << obj.crlPath << ", certPath: " << obj.certPath
           << ", keyPath: " << obj.keyPath;
    }
    os << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const hybm_options &obj)
{
    os << "deviceId: " << obj.devId << ", rankId: " << obj.rankId << ", rankCount: " << obj.rankCount
       << ", maxHBMSize: " << obj.maxHBMSize << ", maxDRAMSize: " << obj.maxDRAMSize
       << ", deviceVASpace: " << obj.deviceVASpace << ", hostVASpace: " << obj.hostVASpace << ", bmType: " << obj.bmType
       << ", memType: " << obj.memType << ", bmDataOpType: " << obj.bmDataOpType << ", scene: " << obj.scene
       << ", enable56BitsGva: " << obj.enable56BitsGva << ", role: " << obj.role << ", flags: " << obj.flags
       << ", dramShmFd: " << obj.dramShmFd << ", transUrl: " << obj.transUrl << ", tag: " << obj.tag
       << ", tagOpInfo: " << obj.tagOpInfo << ", tlsOption: " << obj.tlsOption;
    return os;
}

template<typename T>
std::string VaToStr(T v)
{
    uint64_t v64 = 0;
    if constexpr (std::is_pointer_v<T>) {
        v64 = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(v));
    } else {
        v64 = static_cast<uint64_t>(v);
    }
    std::stringstream ss;
    ss << "0x" << std::hex << v64;
    return ss.str();
}

// Reserved virtual address ranges; GVA ranges of all types must not overlap.
struct ReservedGvaInfo {
    uint64_t va[HVM_BUTT]{};                   // >0
    uint64_t size{};                           // >0
    hybm_mem_type memType{HYBM_MEM_TYPE_BUTT}; // Must be set
    uint32_t localRankId{};                    // Must be set >=0; currently only one localRankId per process
    friend std::ostream &operator<<(std::ostream &os, const ReservedGvaInfo &obj)
    {
        os << "ReservedGvaInfo{va: " << VaToStr(obj.va[HVM_GVA]) << " " << VaToStr(obj.va[HVM_DVA]) << " "
           << VaToStr(obj.va[HVM_DVA]) << ", size: " << VaToStr(obj.size) << ", memType: " << obj.memType
           << ", localRankId: " << obj.localRankId << "}";
        return os;
    }

    [[nodiscard]] bool Contains(const uint64_t addr) const
    {
        return addr >= va[HVM_GVA] && addr < va[HVM_GVA] + size;
    }
};
constexpr uint32_t INVALID_RANK_ID = std::numeric_limits<uint32_t>::max();

// share handle 字符串：仅保存 MemShareHandle::share_info 本体（二进制安全，
// 恒为 MEM_SHARE_HANDLE_LEN 字节，禁止按 c_str/strlen 解析），登记/查询接口与
// 存储均不再直接使用驱动结构体；未登记时为空串
using ShareHandleStr = std::string;

inline ShareHandleStr ToShareInfo(const MemShareHandle &handle)
{
    return ShareHandleStr(reinterpret_cast<const char *>(handle.share_info), MEM_SHARE_HANDLE_LEN);
}
struct BaseAllocatedGvaInfo {
    uint64_t va[HVM_BUTT]{};
    uint64_t size{};                             // >0
    hybm_mem_type memType{HYBM_MEM_TYPE_DEVICE}; // Must be set
};
// Actually allocated memory segments. LVA is an address directly accessible by the XPU, which may equal GVA.
// For the current segment, lva == gva.
// For registered memory: the user provides LVA, and the registered address becomes GVA.
// For imported memory: lva = 0.
struct AllocatedGvaInfo {
    BaseAllocatedGvaInfo base;
    uint32_t localRankId{INVALID_RANK_ID};    // Must be set >=0
    uint32_t importedRankId{INVALID_RANK_ID}; // can be set >=0
    ShareHandleStr shareHandle;               // share_info（二进制安全字符串），未登记时为空串

    AllocatedGvaInfo() = default;
    AllocatedGvaInfo(BaseAllocatedGvaInfo b, uint32_t localRankId) : base{b}, localRankId(localRankId) {}

    AllocatedGvaInfo(BaseAllocatedGvaInfo b, uint32_t localRankId, uint32_t importedRankId)
        : base{b}, localRankId(localRankId), importedRankId(importedRankId)
    {}

    // 单独保存的 share_info（二进制安全字符串），与本记录一一对应（登记地址必为
    // 记录起始 VA），随记录一起增删；未登记时为空串
    void SetShareHandle(const ShareHandleStr &shareInfo)
    {
        shareHandle = shareInfo;
    }

    [[nodiscard]] bool GetShareHandle(ShareHandleStr &shareInfo) const
    {
        // 未登记判定与旧定长数组语义一致：空串或全 0 字节串均视为未登记
        if (shareHandle.empty() || shareHandle.find_first_not_of('\0') == std::string::npos) {
            return false;
        }
        shareInfo = shareHandle;
        return true;
    }

    [[nodiscard]] bool Contains(uint64_t addr, uint32_t t) const
    {
        return t < HVM_BUTT && addr >= base.va[t] && addr < base.va[t] + base.size;
    }

    [[nodiscard]] uint32_t RankId() const noexcept
    {
        return (importedRankId != INVALID_RANK_ID) ? importedRankId : localRankId;
    }

    friend std::ostream &operator<<(std::ostream &os, const AllocatedGvaInfo &obj)
    {
        os << "AllocatedGvaInfo{gva: " << VaToStr(obj.base.va[HVM_GVA]) << ", size: " << VaToStr(obj.base.size)
           << ", localRankId: " << obj.localRankId << ", importRankId: " << obj.importedRankId
           << ", memType: " << obj.base.memType << ", deviceVa: " << VaToStr(obj.base.va[HVM_DVA])
           << ", hostVa: " << VaToStr(obj.base.va[HVM_HVA]) << "}";
        return os;
    }

    [[nodiscard]] std::string ToString() const
    {
        auto type = base.memType == hybm_mem_type::HYBM_MEM_TYPE_HOST ? "H" : "D";
        auto remote = base.va[HVM_DVA] > 0 or base.va[HVM_HVA] > 0 ? "L" : "R";
        std::stringstream os;
        os << "{gva: " << VaToStr(base.va[HVM_GVA]) << ", ";
        os << remote << type << "(";
        os << localRankId << "), deviceVa: " << VaToStr(base.va[HVM_DVA]);
        os << ", hostVa: " << VaToStr(base.va[HVM_HVA]) << "}";
        return os.str();
    }
};

inline bool operator==(const AllocatedGvaInfo &lhs, const AllocatedGvaInfo &rhs)
{
    return lhs.base.va[HVM_GVA] == rhs.base.va[HVM_GVA] && lhs.base.va[HVM_DVA] == rhs.base.va[HVM_DVA] &&
           lhs.base.va[HVM_HVA] == rhs.base.va[HVM_HVA] && lhs.base.size == rhs.base.size &&
           lhs.RankId() == rhs.RankId();
}

inline bool operator!=(const AllocatedGvaInfo &lhs, const AllocatedGvaInfo &rhs)
{
    return !(lhs == rhs);
}

/*
 * Unified address query result — 查 GVA map，返回命中信息
 */
struct AddrQueryResult {
    bool inAllocGva{false};
    hybm_mem_type memType{HYBM_MEM_TYPE_BUTT};
    uint32_t importedRankId{INVALID_RANK_ID};
};

/*
 * Virtual address management and maintenance.
 * Address types include DRAM and HBM. There are two kinds of ranges: AllocatedGvaInfo and ReservedGvaInfo.
 * Memory segments of the same type must not overlap.
 * LVA is an address in the current process that the XPU can directly access, and it may be equal to GVA.
 */
class HybmVaManager {
public:
    HybmVaManager(const HybmVaManager &) = delete;
    HybmVaManager &operator=(const HybmVaManager &) = delete;

    static HybmVaManager &GetInstance()
    {
        // 设定为进程生命周期，以确保在其他单例销毁期间仍保持可用。
        static HybmVaManager *const instance = new HybmVaManager();
        return *instance;
    }
    Result Initialize(AscendSocType socType) noexcept;

    // 地址类型比特位定义，用于 ClassifyAddressMask
    static constexpr uint8_t BIT_LOCAL_HOST = 1 << 0;
    static constexpr uint8_t BIT_GLOBAL_HOST = 1 << 1;
    static constexpr uint8_t BIT_LOCAL_DEVICE = 1 << 2;
    static constexpr uint8_t BIT_GLOBAL_DEVICE = 1 << 3;
    static constexpr int BIT_LUT_SIZE = 256;

    // 方向掩码表：每个方向的 (src_bit | dst_bit<<4)
    static constexpr uint8_t dirMask[HYBM_DATA_COPY_DIRECTION_BUTT] = {
        BIT_LOCAL_HOST | (BIT_GLOBAL_HOST << 4),      // 0 H2GH
        BIT_LOCAL_HOST | (BIT_GLOBAL_DEVICE << 4),    // 1 H2GD
        BIT_LOCAL_DEVICE | (BIT_GLOBAL_HOST << 4),    // 2 D2GH
        BIT_LOCAL_DEVICE | (BIT_GLOBAL_DEVICE << 4),  // 3 D2GD
        BIT_GLOBAL_HOST | (BIT_GLOBAL_HOST << 4),     // 4 GH2GH
        BIT_GLOBAL_HOST | (BIT_GLOBAL_DEVICE << 4),   // 5 GH2GD
        BIT_GLOBAL_HOST | (BIT_LOCAL_HOST << 4),      // 6 GH2LH
        BIT_GLOBAL_HOST | (BIT_LOCAL_DEVICE << 4),    // 7 GH2LD
        BIT_GLOBAL_DEVICE | (BIT_GLOBAL_HOST << 4),   // 8 GD2GH
        BIT_GLOBAL_DEVICE | (BIT_GLOBAL_DEVICE << 4), // 9 GD2GD
        BIT_GLOBAL_DEVICE | (BIT_LOCAL_HOST << 4),    // 10 GD2LH
        BIT_GLOBAL_DEVICE | (BIT_LOCAL_DEVICE << 4),  // 11 GD2LD
        0xFF,                                         // 12 AUTO sentinel
    };

    // 方向查表 LUT：except(8bit) → direction，hybm_init 时初始化
    static uint8_t directionLut[BIT_LUT_SIZE];
    static void InitDirectionLut();

    Result AddVaInfoFromExternal(const BaseAllocatedGvaInfo &baseInfo, uint32_t localRankId);
    Result AddVaInfoFromExternal(const BaseAllocatedGvaInfo &baseInfo, uint32_t localRankId, uint32_t importedRankId);
    Result AddVaInfo(const BaseAllocatedGvaInfo &baseInfo, uint32_t localRankId, bool onlyGva = false);
    Result AddVaInfo(const AllocatedGvaInfo &info, bool onlyGva = false);
    void RemoveOneVaInfo(uint64_t va, uint32_t type = HVM_GVA);

    // Returns 0 if not found.
    uint64_t TransformVa(uint64_t va, uint32_t inputType, uint32_t outputType);
    std::pair<AllocatedGvaInfo, bool> FindAllocByVa(uint64_t va, uint32_t type = HVM_GVA) const;

    hybm_mem_type GetGvaMemType(uint64_t gva); // Supports both LVA and GVA
    std::pair<uint32_t, bool> GetRankByGva(uint64_t gva);
    // Checks if 'va' is within any AllocatedGvaInfo range (either LVA or GVA).
    bool IsValidAddr(uint64_t va);

    hybm_data_copy_direction InferCopyDirection(uint64_t srcVa, uint64_t dstVa);

    // 返回地址类型掩码（可多 bit 组合），用于 bitmask 方向校验
    uint8_t ClassifyAddressMask(const uint64_t va);
    Result GetLocalMemoryType(uint64_t va, hybm_mem_type &memType) const noexcept;

    // =============ReservedGvaInfo Management==============================
    ReservedGvaInfo AllocReserveGva(uint32_t localRankId, uint64_t size, uint64_t localSize, hybm_mem_type memType,
                                    bool enable56BitsGva = false, hybm_scene scene = HYBM_SCENE_DEFAULT);
    ReservedGvaInfo AllocReserveLva(uint32_t localRankId, uint64_t size, uint32_t type, hybm_mem_type memType);
    void FreeReserveGva(uint64_t addr);
    void FreeReserveLva(uint64_t addr, uint32_t type);

    void DumpReservedGvaInfo() const;
    void DumpAllocatedGvaInfo() const;

    size_t GetAllocCount() const;
    size_t GetReservedCount() const;
    void ClearAll();

    AddrType ClassifyAddress(const uint64_t va);

    // 查 GVA map：地址在 GVA 范围内 → GVA 命中
    AddrQueryResult QueryAddr(const uint64_t va) const
    {
        AddrQueryResult result;
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (allocatedMap_[HVM_GVA].empty()) {
            return result;
        }
        auto it = allocatedMap_[HVM_GVA].upper_bound(va);
        if (it != allocatedMap_[HVM_GVA].begin()) {
            --it;
        }
        if (it->second.Contains(va, HVM_GVA)) {
            auto &info = it->second;
            result.inAllocGva = true;
            result.memType = info.base.memType;
            result.importedRankId = info.importedRankId;
        }
        return result;
    }

    // =============Share info 登记/复用====================================
    // share_info 不再独立建表，直接内嵌在对应的 AllocatedGvaInfo 记录中：
    // 登记地址必为记录起始 VA（slice 导出记录 DVA=HVA=slice 起始，导入记录 DVA=lva），
    // 记录删除（RemoveOneVaInfo/ClearAll）时随之失效，避免悬挂残留；
    // 全程使用二进制安全的字符串 ShareHandleStr，驱动结构体 MemShareHandle 经 ToShareInfo 转换后进入
    Result AddHandle(const void *ptr, const ShareHandleStr &shareInfo);
    Result GetHandle(const void *ptr, ShareHandleStr &shareInfo) const;

private:
    HybmVaManager() = default;

    ~HybmVaManager() = default;

    std::pair<bool, AllocatedGvaInfo> CheckOverlap(uint64_t va, uint64_t size, uint32_t type);

    uint64_t AllocReserveLvaInner(uint32_t localRankId, uint64_t size, uint32_t type,
                                  hybm_scene scene = HYBM_SCENE_DEFAULT);

    std::pair<uint64_t, bool> FindFreeSpace(uint64_t start, uint64_t end, uint64_t size, uint32_t type);

    // 按 HVA 优先、DVA 回退的顺序查找包含 addr 的记录（调用方须已持有 mutex_）；
    // 导入映射（如 SHARED 池非分配 rank）只登记 DVA 表，故需回退 DVA 查找
    const AllocatedGvaInfo *FindAllocRecordLocked(uint64_t addr) const;
    AllocatedGvaInfo *FindAllocRecordLocked(uint64_t addr);

private:
    mutable std::shared_mutex mutex_{};
    AscendSocType soc_ = AscendSocType::ASCEND_UNKNOWN;

    std::map<uint64_t, AllocatedGvaInfo> allocatedMap_[HVM_BUTT]{}; // map<va, allocInfo>
    std::map<uint64_t, ReservedGvaInfo> reservedMap_[HVM_BUTT]{};   // map<va, reserveInfo>  (HVM_HVA not used now)

    static constexpr hybm_data_copy_direction COPY_DIRECTION_TABLE[ADDRESS_CATEGORY_BUTT][ADDRESS_CATEGORY_BUTT] = {
        {HYBM_GLOBAL_DEVICE_TO_GLOBAL_DEVICE, HYBM_GLOBAL_DEVICE_TO_GLOBAL_HOST, HYBM_GLOBAL_DEVICE_TO_LOCAL_DEVICE,
         HYBM_GLOBAL_DEVICE_TO_LOCAL_HOST},
        {HYBM_GLOBAL_HOST_TO_GLOBAL_DEVICE, HYBM_GLOBAL_HOST_TO_GLOBAL_HOST, HYBM_GLOBAL_HOST_TO_LOCAL_DEVICE,
         HYBM_GLOBAL_HOST_TO_LOCAL_HOST},
        {HYBM_LOCAL_DEVICE_TO_GLOBAL_DEVICE, HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST, HYBM_DATA_COPY_DIRECTION_BUTT,
         HYBM_LOCAL_DEVICE_TO_GLOBAL_HOST},
        {HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE,
         HYBM_DATA_COPY_DIRECTION_BUTT},
    };
};

template<typename T>
std::string VaToInfo(T v)
{
    uint64_t v64 = 0;
    if constexpr (std::is_pointer_v<T>) {
        v64 = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(v));
    } else {
        v64 = static_cast<uint64_t>(v);
    }

    for (uint32_t i = 0; i < HVM_BUTT; i++) {
        auto info = HybmVaManager::GetInstance().FindAllocByVa(v64, i);
        if (info.second) {
            return "[va:" + VaToStr(v64) + ",info:" + info.first.ToString() + "]";
        }
    }
    return VaToStr(v64);
}
} // namespace mf
} // namespace ock

#endif
