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
#ifndef MF_GENETTY_MEMORY_INFO_H
#define MF_GENETTY_MEMORY_INFO_H

#include "genetty_vendor_type.h"
#include "common/genetty_common_includes.h"

namespace ock {
namespace openDCC {

constexpr uint16_t GEN_INFO_VERSION_CONST = 1;

constexpr char GEN_INFO_VERSION[] = "openDCC.version";
constexpr char GEN_INFO_MEM_TYPE[] = "openDCC.mem.type";
constexpr char GEN_INFO_MEM_ADDR[] = "openDCC.mem.addr";
constexpr char GEN_INFO_MEM_SIZE[] = "openDCC.mem.size";
constexpr char GEN_INFO_MEM_RKEY[] = "openDCC.mem.rkey";
constexpr char GEN_INFO_DEVICE_TYPE[] = "openDCC.device.type";
constexpr char GEN_INFO_DEVICE_VENDOR[] = "openDCC.device.vendor";

/**
 * @brief Information of a registered memory region, which is exported to the peer so that the peer can
 *        access this memory region remotely
 */
class MemoryInfo {
public:
    ~MemoryInfo() = default;

    uint64_t Address() const noexcept;
    void Address(uint64_t address) noexcept;

    uint64_t Size() const noexcept;
    void Size(uint64_t size) noexcept;

    uint16_t Version() const noexcept;
    void Version(uint16_t version) noexcept;

    genetty_mem_type_t MemType() const noexcept;
    void MemType(genetty_mem_type_t memType) noexcept;

    uint32_t Rkey() const noexcept;
    void Rkey(uint32_t rkey) noexcept;

    uint32_t Lkey() const noexcept;
    void Lkey(uint32_t lkey) noexcept;

    bool operator<(const MemoryInfo &other) const noexcept;

    /**
     * @brief Export the local memory information, the given struct is filled by this call
     *
     * @param info             [out] memory information to be filled
     * @param flags            [in] optional flags
     * @return GEN_OK if successful, others if failed
     */
    Result Export(genetty_memory_info_t *info, uint32_t flags) noexcept;

    /**
     * @brief Import the memory information exported by the peer, and make this memory region accessible
     *
     * @param info             [in] memory information exported by the peer
     * @param flags            [in] optional flags
     * @return GEN_OK if successful, others if failed
     */
    Result Import(genetty_memory_info_t *info, uint32_t flags) noexcept;

protected:
    uint64_t address_ = 0;                      /* virtual address of memory region */
    uint64_t size_ = 0;                         /* size of the memory region */
    uint16_t version_ = GEN_INFO_VERSION_CONST; /* version */
    genetty_mem_type_t type_ = MT_INVALID;      /* memory type */
    uint32_t rkey_ = 0;                         /* remote key got from the peer memory info */
    uint32_t lkey_ = 0;                         /* local key got after registered */
};

ALWAYS_INLINE uint64_t MemoryInfo::Address() const noexcept
{
    return address_;
}

ALWAYS_INLINE void MemoryInfo::Address(uint64_t address) noexcept
{
    address_ = address;
}

ALWAYS_INLINE uint64_t MemoryInfo::Size() const noexcept
{
    return size_;
}

ALWAYS_INLINE void MemoryInfo::Size(uint64_t size) noexcept
{
    size_ = size;
}

ALWAYS_INLINE uint16_t MemoryInfo::Version() const noexcept
{
    return version_;
}

ALWAYS_INLINE void MemoryInfo::Version(uint16_t version) noexcept
{
    version_ = version;
}

ALWAYS_INLINE genetty_mem_type_t MemoryInfo::MemType() const noexcept
{
    return type_;
}

ALWAYS_INLINE void MemoryInfo::MemType(genetty_mem_type_t memType) noexcept
{
    type_ = memType;
}

ALWAYS_INLINE uint32_t MemoryInfo::Rkey() const noexcept
{
    return rkey_;
}

ALWAYS_INLINE void MemoryInfo::Rkey(uint32_t rkey) noexcept
{
    rkey_ = rkey;
}

ALWAYS_INLINE uint32_t MemoryInfo::Lkey() const noexcept
{
    return lkey_;
}

ALWAYS_INLINE void MemoryInfo::Lkey(uint32_t lkey) noexcept
{
    lkey_ = lkey;
}

ALWAYS_INLINE bool MemoryInfo::operator<(const MemoryInfo &other) const noexcept
{
    if (type_ != other.type_) {
        return type_ < other.type_;
    }

    if (address_ != other.address_) {
        return address_ < other.address_;
    }

    return size_ < other.size_;
}

/**
 * @brief Information exchanged between two peers to establish the connection, such as the queue pair
 *        information of the local side
 */
class LinkInfo {
public:
    virtual ~LinkInfo() = default;

    uint16_t Version() const noexcept;
    void Version(uint16_t version) noexcept;

    genetty_type_t Type() const noexcept;
    void Type(genetty_type_t type) noexcept;

    VendorType Vendor() const noexcept;
    void Vendor(VendorType vendor) noexcept;

    /**
     * @brief Export the local exchange information, the given struct is filled by this call
     *
     * @param info             [out] exchange information to be filled
     * @param flags            [in] optional flags
     * @return GEN_OK if successful, others if failed
     */
    virtual Result Export(genetty_link_info_t *info, uint32_t flags) noexcept = 0;

    /**
     * @brief Import the exchange information exported by the peer, and make the connection ready
     *
     * @param info             [in] exchange information exported by the peer
     * @param flags            [in] optional flags
     * @return GEN_OK if successful, others if failed
     */
    virtual Result Import(genetty_link_info_t *info, uint32_t flags) noexcept = 0;

protected:
    uint16_t version_ = GEN_INFO_VERSION_CONST; /* version */
    genetty_type_t type_ = GEN_TYPE_INVALID;    /* genetty type */
    VendorType vendor_ = VT_INVALID;            /* vendor */
};

ALWAYS_INLINE uint16_t LinkInfo::Version() const noexcept
{
    return version_;
}

ALWAYS_INLINE void LinkInfo::Version(uint16_t version) noexcept
{
    version_ = version;
}

ALWAYS_INLINE genetty_type_t LinkInfo::Type() const noexcept
{
    return type_;
}

ALWAYS_INLINE void LinkInfo::Type(genetty_type_t type) noexcept
{
    type_ = type;
}

ALWAYS_INLINE VendorType LinkInfo::Vendor() const noexcept
{
    return vendor_;
}

ALWAYS_INLINE void LinkInfo::Vendor(VendorType vendor) noexcept
{
    vendor_ = vendor;
}
} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_MEMORY_INFO_H
