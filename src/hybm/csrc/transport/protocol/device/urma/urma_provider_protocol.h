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
#include "provider_control_io.h"
#include "hybm_transport_common.h"
#include "hybm_types.h"
namespace ock::mf::transport::device {
enum class UrmaProviderMessageType : uint16_t { HELLO = 1, OFFER, READY, ACK, DRAIN, QUIESCED };
struct UrmaProviderMessage {
    UrmaProviderMessageType type{UrmaProviderMessageType::HELLO};
    Result status{BM_OK};
    uint32_t rank{0};
    uint32_t phyId{0};
    uint32_t protocols{HYBM_DOP_TYPE_DEVICE_URMA};
    uint64_t gvaBase{0};
    uint64_t poolSize{0};
    uint16_t channelPort{0};
    TransportPrivateData privateData{};
    TransportMemoryKey memoryKey;
};
ProviderDeadline UrmaProviderDeadline();
Result SendUrmaProviderMessage(int fd, const UrmaProviderMessage &message, ProviderDeadline deadline);
Result ReceiveUrmaProviderMessage(int fd, UrmaProviderMessage &message, ProviderDeadline deadline);
} // namespace ock::mf::transport::device
