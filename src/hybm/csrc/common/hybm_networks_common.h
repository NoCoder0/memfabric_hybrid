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

#ifndef MEM_FABRIC_HYBRID_HYBM_NETWORKS_COMMON_H
#define MEM_FABRIC_HYBRID_HYBM_NETWORKS_COMMON_H

#include <cstdint>
#include <iomanip>
#include <ostream>
#include <vector>

namespace ock {
namespace mf {

// Machine identity used to distinguish local provider pools.
struct MachineId {
    uint64_t major;
    uint64_t minor;

    constexpr MachineId() : major(0), minor(0) {}

    constexpr explicit MachineId(uint64_t mj, uint64_t mn) : major(mj), minor(mn) {}

    bool operator==(const MachineId &other) const noexcept
    {
        return major == other.major && minor == other.minor;
    }

    bool operator!=(const MachineId &other) const noexcept
    {
        return !(*this == other);
    }

    // Print the ID as zero-padded hexadecimal major-minor.
    friend std::ostream &operator<<(std::ostream &output, const MachineId &machineId)
    {
        const auto flags = output.flags();
        const auto fill = output.fill();
        output << std::hex << std::setfill('0') << std::setw(16) << machineId.major << "-" << std::setw(16)
               << machineId.minor << std::dec;
        output.flags(flags);
        output.fill(fill);
        return output;
    }
};

// Return usable IPv4 addresses in host byte order, sorted ascending.
// Place the default-route interface first when available.
std::vector<uint32_t> NetworkGetIpAddresses() noexcept;

// Cache machine-id, falling back to boot-id.
// Return true only for a successfully parsed, non-zero identity.
bool ReadMachineId(MachineId &machineId) noexcept;
} // namespace mf
} // namespace ock

#endif // MEM_FABRIC_HYBRID_HYBM_NETWORKS_COMMON_H
