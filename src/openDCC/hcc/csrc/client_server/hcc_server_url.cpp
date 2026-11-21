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
#include "hcc_server_url.h"

#include "common/genetty_functions.h"
#include "common/hcc_logger.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <exception>
#include <utility>

namespace ock {
namespace openDCC {
constexpr char HCC_URL_PROTOCOL[] = "hcc://";

Result HccServerUrl::ParsePort(const std::string &text, uint16_t &port) noexcept
{
    if (text.empty()) {
        HCC_LOG_ERROR("Parse url port(" << text << ") failed, port is empty");
        return HCC_INVALID_PARAM;
    }

    try {
        size_t pos = 0;
        int value = std::stoi(text, &pos);
        if (pos != text.size() || value < 0 || value > UINT16_MAX) {
            HCC_LOG_ERROR("Parse url port(" << text << ") failed, port is out of range");
            return HCC_INVALID_PARAM;
        }

        port = static_cast<uint16_t>(value);
    } catch (const std::exception &e) {
        HCC_LOG_ERROR("Parse url port(" << text << ") failed, port is invalid, error(" << e.what() << ")");
        return HCC_INVALID_PARAM;
    }

    return HCC_OK;
}

Result HccServerUrl::ParseQuery(const std::string &query, std::map<std::string, std::string> &kv) noexcept
{
    size_t start = 0;
    while (start < query.size()) {
        size_t ampPos = query.find('&', start);
        std::string pair = query.substr(start, ampPos == std::string::npos ? std::string::npos : ampPos - start);
        size_t equalPos = pair.find('=');
        if (equalPos == std::string::npos || equalPos == 0) {
            HCC_LOG_ERROR("Parse url query(" << query << ") failed, pair(" << pair << ") is invalid");
            return HCC_INVALID_PARAM;
        }

        std::string key = Func::Trim(pair.substr(0, equalPos));
        std::string value = Func::Trim(pair.substr(equalPos + 1));
        if (key.empty()) {
            HCC_LOG_ERROR("Parse url query(" << query << ") failed, key of pair(" << pair << ") is empty");
            return HCC_INVALID_PARAM;
        }
        kv[key] = value;

        if (ampPos == std::string::npos) {
            break;
        }
        start = ampPos + 1;
    }

    return HCC_OK;
}

Result HccServerUrl::Deserialize() noexcept
{
    /* note: the format url is hcc://ip:port?key=value&key1=value1 */

    /* step1: do trim, using the function genetty_functions.h */
    std::string url = Func::Trim(rawUrl_);
    if (url.empty()) {
        HCC_LOG_ERROR("Deserialize url(" << rawUrl_ << ") failed, url is empty");
        return HCC_INVALID_PARAM;
    }

    /* step2: extract protocol hcc:// and verify */
    constexpr size_t protocolLen = sizeof(HCC_URL_PROTOCOL) - 1;
    if (url.compare(0, protocolLen, HCC_URL_PROTOCOL) != 0) {
        HCC_LOG_ERROR("Deserialize url(" << url << ") failed, protocol is invalid");
        return HCC_INVALID_PARAM;
    }

    /* step3: extract ip and port */
    size_t queryPos = url.find('?', protocolLen);
    std::string hostPort =
        url.substr(protocolLen, queryPos == std::string::npos ? std::string::npos : queryPos - protocolLen);
    size_t colonPos = hostPort.rfind(':');
    if (colonPos == std::string::npos || colonPos == 0 || colonPos + 1 >= hostPort.size()) {
        HCC_LOG_ERROR("Deserialize url(" << url << ") failed, ip or port is invalid");
        return HCC_INVALID_PARAM;
    }

    uint16_t port = 0;
    Result result = ParsePort(hostPort.substr(colonPos + 1), port);
    if (result != HCC_OK) {
        return result;
    }

    /* step4: extract keys and values */
    std::map<std::string, std::string> kv;
    if (queryPos != std::string::npos) {
        result = ParseQuery(url.substr(queryPos + 1), kv);
        if (result != HCC_OK) {
            return result;
        }
    }

    /* step5: assign to member variables */
    rawUrl_ = url;
    ip_ = hostPort.substr(0, colonPos);
    port_ = port;
    kv_ = std::move(kv);
    return HCC_OK;
}

Result HccServerUrl::Serialize() noexcept
{
    /* step1: check member variables if they are valid or not */
    if (ip_.empty() || port_ == 0 || ip_.find_first_of(":?&=") != std::string::npos) {
        HCC_LOG_ERROR("Serialize url(" << ip_ << ":" << port_ << ") failed, ip or port is invalid");
        return HCC_INVALID_PARAM;
    }

    for (const auto &item : kv_) {
        if (item.first.empty() || item.first.find_first_of("&=") != std::string::npos ||
            item.second.find('&') != std::string::npos) {
            HCC_LOG_ERROR("Serialize url(" << ip_ << ":" << port_ << ") failed, key(" << item.first << ") or value("
                                           << item.second << ") is invalid");
            return HCC_INVALID_PARAM;
        }
    }

    /* step2: serialize protocol */
    std::string url = HCC_URL_PROTOCOL;

    /* step3: serialize ip and port */
    url.append(ip_).append(":").append(std::to_string(port_));

    /* step4: serialize key and value */
    url += '?';
    if (!kv_.empty()) {
        for (const auto &item : kv_) {
            url.append(item.first).append("=").append(item.second).append("&");
        }
    }

    /* step5: remove the last character */
    if (!url.empty()) {
        url.pop_back();
    }

    /* step6: assign to rawUrl_ */
    rawUrl_ = std::move(url);
    return HCC_OK;
}

bool HccServerUrl::GetValueByKey(const std::string &key, std::string &value) noexcept
{
    auto iter = kv_.find(key);
    if (iter == kv_.end()) {
        return false;
    }

    value = iter->second;
    return true;
}

void HccServerUrl::PutKeyValue(const std::string &key, const std::string &value) noexcept
{
    kv_[key] = value;
}
} // namespace openDCC
} // namespace ock
