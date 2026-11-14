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

#include "urma_provider_protocol.h"
#include "hybm_common_include.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include <vector>
#include <cstring>
namespace ock::mf::transport::device {
namespace {
constexpr uint32_t MAGIC = 0x55524D50U;
constexpr uint16_t VERSION = 1U;
constexpr size_t HEADER_SIZE = 16U;
constexpr size_t HELLO_PREFIX_SIZE = 16U;
constexpr size_t OFFER_PREFIX_SIZE = 38U;
constexpr size_t MAX_KEY_BYTES = TRANS_KEY_MAX_WORDS * sizeof(uint64_t);
constexpr size_t MAX_PAYLOAD = 2U * MAX_KEY_BYTES + 64U;
void Append(std::vector<uint8_t> &out, uint64_t value, size_t bytes)
{
    for (size_t i = bytes; i != 0; --i)
        out.push_back(static_cast<uint8_t>(value >> ((i - 1U) * 8U)));
}
uint64_t Read(const uint8_t *data, size_t bytes)
{
    uint64_t result = 0;
    for (size_t i = 0; i < bytes; ++i)
        result = (result << 8U) | data[i];
    return result;
}
Result Invalid(const char *reason)
{
    BM_LOG_ERROR("URMA provider control frame invalid: " << reason);
    return BM_INVALID_PARAM;
}
} // namespace
ProviderDeadline UrmaProviderDeadline()
{
    auto seconds = MfEnvUtil::GetOptionalUintOrDefault(env::MF_QP_READY_CHECK_TIMEOUT_BASE, 100);
    return std::chrono::steady_clock::now() + std::chrono::seconds(seconds) + std::chrono::milliseconds(5100);
}
Result SendUrmaProviderMessage(int fd, const UrmaProviderMessage &message, ProviderDeadline deadline)
{
    std::vector<uint8_t> payload;
    auto type = static_cast<uint16_t>(message.type);
    if (message.type < UrmaProviderMessageType::HELLO || message.type > UrmaProviderMessageType::QUIESCED)
        return Invalid("message type");
    auto privateBytes = message.privateData.key.ByteSize();
    auto keyBytes = message.memoryKey.ByteSize();
    if (privateBytes > MAX_KEY_BYTES || keyBytes > MAX_KEY_BYTES)
        return Invalid("descriptor capacity");
    if (message.status == BM_OK &&
        (message.type == UrmaProviderMessageType::HELLO || message.type == UrmaProviderMessageType::OFFER)) {
        Append(payload, message.rank, 4);
        Append(payload, message.phyId, 4);
        Append(payload, message.protocols, 4);
        if (message.type == UrmaProviderMessageType::OFFER) {
            Append(payload, message.gvaBase, 8);
            Append(payload, message.poolSize, 8);
        }
        Append(payload, privateBytes, 4);
        if (message.type == UrmaProviderMessageType::OFFER) {
            Append(payload, keyBytes, 4);
            Append(payload, message.channelPort, 2);
        }
        if (privateBytes != 0)
            payload.insert(payload.end(), message.privateData.key.ByteData(),
                           message.privateData.key.ByteData() + privateBytes);
        if (message.type == UrmaProviderMessageType::OFFER && keyBytes != 0)
            payload.insert(payload.end(), message.memoryKey.ByteData(), message.memoryKey.ByteData() + keyBytes);
    }
    std::vector<uint8_t> header;
    Append(header, MAGIC, 4);
    Append(header, VERSION, 2);
    Append(header, type, 2);
    Append(header, static_cast<uint32_t>(message.status), 4);
    Append(header, payload.size(), 4);
    if (!provider_control::SendAll(fd, header.data(), header.size(), deadline) ||
        (!payload.empty() && !provider_control::SendAll(fd, payload.data(), payload.size(), deadline))) {
        BM_LOG_ERROR("URMA provider send failed, fd=" << fd << " type=" << type);
        return BM_ERROR;
    }
    return BM_OK;
}
Result ReceiveUrmaProviderMessage(int fd, UrmaProviderMessage &message, ProviderDeadline deadline)
{
    uint8_t header[HEADER_SIZE]{};
    if (!provider_control::RecvAll(fd, header, sizeof(header), deadline)) {
        BM_LOG_ERROR("URMA provider receive header failed, fd=" << fd);
        return BM_ERROR;
    }
    auto type = static_cast<UrmaProviderMessageType>(Read(header + 6, 2));
    auto length = Read(header + 12, 4);
    if (Read(header, 4) != MAGIC || Read(header + 4, 2) != VERSION || type < UrmaProviderMessageType::HELLO ||
        type > UrmaProviderMessageType::QUIESCED || length > MAX_PAYLOAD)
        return Invalid("header");
    message = {};
    message.type = type;
    message.status = static_cast<int32_t>(static_cast<uint32_t>(Read(header + 8, 4)));
    const bool hasDescriptor = type == UrmaProviderMessageType::HELLO || type == UrmaProviderMessageType::OFFER;
    if ((!hasDescriptor || message.status != BM_OK) && length != 0)
        return Invalid("unexpected payload");
    if (length == 0) {
        if (hasDescriptor && message.status == BM_OK)
            return Invalid("missing descriptor");
        return BM_OK;
    }
    auto prefix = type == UrmaProviderMessageType::HELLO ? HELLO_PREFIX_SIZE : OFFER_PREFIX_SIZE;
    if (length < prefix)
        return Invalid("truncated prefix");
    std::vector<uint8_t> payload(length);
    if (!provider_control::RecvAll(fd, payload.data(), payload.size(), deadline)) {
        BM_LOG_ERROR("URMA provider receive payload failed, fd=" << fd);
        return BM_ERROR;
    }
    auto data = payload.data();
    message.rank = static_cast<uint32_t>(Read(data, 4));
    message.phyId = static_cast<uint32_t>(Read(data + 4, 4));
    message.protocols = static_cast<uint32_t>(Read(data + 8, 4));
    size_t privateBytes = Read(data + (type == UrmaProviderMessageType::HELLO ? 12U : 28U), 4);
    size_t keyBytes = type == UrmaProviderMessageType::HELLO ? 0U : Read(data + 32, 4);
    if (privateBytes == 0 || privateBytes > MAX_KEY_BYTES || keyBytes > MAX_KEY_BYTES ||
        privateBytes % sizeof(uint64_t) != 0 || keyBytes % sizeof(uint64_t) != 0 ||
        privateBytes + keyBytes != length - prefix || (type == UrmaProviderMessageType::OFFER && keyBytes == 0))
        return Invalid("descriptor lengths");
    message.privateData.key.ResizeBytes(privateBytes);
    std::memcpy(message.privateData.key.ByteData(), data + prefix, privateBytes);
    message.memoryKey.ResizeBytes(keyBytes);
    if (keyBytes != 0)
        std::memcpy(message.memoryKey.ByteData(), data + prefix + privateBytes, keyBytes);
    if (type == UrmaProviderMessageType::OFFER) {
        message.gvaBase = Read(data + 12, 8);
        message.poolSize = Read(data + 20, 8);
        message.channelPort = static_cast<uint16_t>(Read(data + 36, 2));
    }
    return BM_OK;
}
} // namespace ock::mf::transport::device
