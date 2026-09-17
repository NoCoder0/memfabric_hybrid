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
#include "hybm_common_include.h"
#include "hybm_mem_common.h"

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

    AllocatedGvaInfo() = default;
    AllocatedGvaInfo(BaseAllocatedGvaInfo b, uint32_t localRankId) : base{b}, localRankId(localRankId) {}

    AllocatedGvaInfo(BaseAllocatedGvaInfo b, uint32_t localRankId, uint32_t importedRankId)
        : base{b}, localRankId(localRankId), importedRankId(importedRankId)
    {}

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
        static HybmVaManager instance;
        return instance;
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
                                    bool enable56BitsGva = false, bool isTrans = false);
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

private:
    HybmVaManager() = default;

    ~HybmVaManager() = default;

    std::pair<bool, AllocatedGvaInfo> CheckOverlap(uint64_t va, uint64_t size, uint32_t type);

    uint64_t AllocReserveLvaInner(uint32_t localRankId, uint64_t size, uint32_t type);

    std::pair<uint64_t, bool> FindFreeSpace(uint64_t start, uint64_t end, uint64_t size, uint32_t type);

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

/* 一段地址范围缓存：把"同一段内连续地址"的重复查询从 shared_lock+红黑树(O(logN)) 降成内存比较。
   背景：批量拷贝按 iov 逐个查归属/转换（每轮 600 iov × 2 个地址 × 2 类查询 = 2400 次），
   而一批地址通常集中在同一个段里（本地段 / 对端导入段各一个），绝大多数查询是重复遍历同一棵树。
   用法约束：
     - 一个实例只服务"一个地址流"（如 sources 一个、destinations 一个），混用会因交替换段而全部未命中；
     - 生命周期就是一次批量拷贝，因此不存在跨调用失效问题（段在拷贝过程中被摘除本来就是既有竞态）。
   未命中时回表 HybmVaManager::FindAllocByVa（公开 API，管理器内部不加锁改动）。 */
class VaRangeCache {
public:
    /* 查 gva 的归属 rank：命中缓存直接返回；未命中回表，回表失败返回 false（调用方按本地 rank 处理） */
    bool RankByGva(uint64_t gva, uint32_t &rank)
    {
        if (!Contains(gva, HVM_GVA)) {
            auto [info, found] = HybmVaManager::GetInstance().FindAllocByVa(gva, HVM_GVA);
            if (!found) {
                Reset();
                return false;
            }
            Fill(info, HVM_GVA, HVM_BUTT);
        }
        rank = info_.RankId();
        return true;
    }

    /* va 从 inType 转到 outType（如 GVA->HVA）：命中缓存按段内线性关系直接算，返回 0 表示不可转 */
    uint64_t Transform(uint64_t va, uint32_t inType, uint32_t outType)
    {
        if (inType == outType) {
            return va;
        }
        if (inType_ != inType || outType_ != outType || !InRange(va)) {
            auto [info, found] = HybmVaManager::GetInstance().FindAllocByVa(va, inType);
            if (!found || info.base.va[outType] == 0) {
                Reset();
                return 0;
            }
            Fill(info, inType, outType);
        }
        return outBase_ + (va - inBase_);
    }

    /* 热路径专用：GVA -> HVA（两个类型都是编译期常量）。
       每地址只有「1 次减 + 1 次无符号比较」判区间、「1 次减 + 1 次加」做变换，
       不再做类型比较，也不再用运行期下标去 info_.base.va[] 里取基址。 */
    uint64_t GvaToHva(uint64_t va)
    {
        if (inType_ == HVM_GVA && outType_ == HVM_HVA && (va - start_) < size_) {
            return outBase_ + (va - inBase_);
        }
        return Transform(va, HVM_GVA, HVM_HVA);
    }

    /* 热循环批量版本：把段状态直接写进调用方的局部变量（而不是让调用方每次回读本对象成员）。
       原因是调用方循环里会写 sources[]/destinations[] 这类指针数组，编译器无法证明这些写不会
       别名到本对象，于是每次写后都要重新载入成员 —— 状态放到局部变量后就能常驻寄存器。
       命中：写入缓存的段区间与两个基址；未命中：回表刷新后写入；回表失败：size 写 0（调用方据此保持原地址）。 */
    void ResolveGvaToHva(uint64_t va, uint64_t &start, uint64_t &size, uint64_t &inBase, uint64_t &outBase)
    {
        if (inType_ != HVM_GVA || outType_ != HVM_HVA || (va - start_) >= size_) {
            auto [info, found] = HybmVaManager::GetInstance().FindAllocByVa(va, HVM_GVA);
            if (!found || info.base.va[HVM_HVA] == 0) {
                Reset();
                size = 0;
                return;
            }
            Fill(info, HVM_GVA, HVM_HVA);
        }
        start = start_;
        size = size_;
        inBase = inBase_;
        outBase = outBase_;
    }

private:
    void Fill(const AllocatedGvaInfo &info, uint32_t inType, uint32_t outType)
    {
        info_ = info;
        inType_ = inType;
        outType_ = outType;
        start_ = info.base.va[inType];
        size_ = info.base.size;
        inBase_ = start_;
        /* outType == HVM_BUTT 表示只关心 rank（RankByGva 用），此时不取输出基址 */
        outBase_ = (outType < HVM_BUTT) ? info.base.va[outType] : 0;
    }

    void Reset()
    {
        size_ = 0;
        inType_ = HVM_BUTT;
        outType_ = HVM_BUTT;
    }

    /* 单条无符号比较判区间：size_ == 0（未填充）时恒为 false */
    bool InRange(uint64_t addr) const
    {
        return (addr - start_) < size_;
    }

    bool Contains(uint64_t addr, uint32_t type) const
    {
        return inType_ == type && InRange(addr);
    }

    AllocatedGvaInfo info_{};
    uint64_t start_ = 0;
    uint64_t size_ = 0;
    uint64_t inBase_ = 0;
    uint64_t outBase_ = 0;
    uint32_t inType_ = HVM_BUTT;
    uint32_t outType_ = HVM_BUTT;
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
