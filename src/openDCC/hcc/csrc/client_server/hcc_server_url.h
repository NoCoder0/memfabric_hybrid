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
#ifndef MF_OPENDCC_HCC_SERVER_URL_H
#define MF_OPENDCC_HCC_SERVER_URL_H

#include "hcc_types.h"

#include <map>
#include <string>

namespace ock {
namespace openDCC {
class HccServerUrl {
public:
    explicit HccServerUrl(const std::string &url) noexcept : rawUrl_(url) {}
    HccServerUrl() noexcept = default;

    Result Deserialize() noexcept;
    Result Serialize() noexcept;

    bool GetValueByKey(const std::string &key, std::string &value) noexcept;
    void PutKeyValue(const std::string &key, const std::string &value) noexcept;

    const std::string &GetRawUrl() const noexcept;
    void SetRawUrl(const std::string &rawUrl) noexcept;

    const std::string &GetIp() const noexcept;
    void SetIp(const std::string &ip) noexcept;

    uint16_t GetPort() const noexcept;
    void SetPort(uint16_t port) noexcept;

    const std::map<std::string, std::string> &GetKv() const noexcept;
    void SetKv(const std::map<std::string, std::string> &kv) noexcept;

private:
    static Result ParsePort(const std::string &text, uint16_t &port) noexcept;
    static Result ParseQuery(const std::string &query, std::map<std::string, std::string> &kv) noexcept;

    std::string rawUrl_;                    /* raw url */
    std::string ip_;                        /* ip */
    uint16_t port_ = 0;                     /* port */
    std::map<std::string, std::string> kv_; /* query key and value */
};

ALWAYS_INLINE const std::string &HccServerUrl::GetRawUrl() const noexcept
{
    return rawUrl_;
}

ALWAYS_INLINE void HccServerUrl::SetRawUrl(const std::string &rawUrl) noexcept
{
    rawUrl_ = rawUrl;
}

ALWAYS_INLINE const std::string &HccServerUrl::GetIp() const noexcept
{
    return ip_;
}

ALWAYS_INLINE void HccServerUrl::SetIp(const std::string &ip) noexcept
{
    ip_ = ip;
}

ALWAYS_INLINE uint16_t HccServerUrl::GetPort() const noexcept
{
    return port_;
}

ALWAYS_INLINE void HccServerUrl::SetPort(uint16_t port) noexcept
{
    port_ = port;
}

ALWAYS_INLINE const std::map<std::string, std::string> &HccServerUrl::GetKv() const noexcept
{
    return kv_;
}

ALWAYS_INLINE void HccServerUrl::SetKv(const std::map<std::string, std::string> &kv) noexcept
{
    kv_ = kv;
}
} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_HCC_SERVER_URL_H
