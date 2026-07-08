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

#include <unordered_map>

#include "acc_http_link_default.h"
#include "acc_http_request_context.h"

namespace ock {
namespace acc {

namespace {

/* hex digit offset for letters a-f/A-F */
constexpr int HEX_BASE = 10;
/* bit shift count per hex digit */
constexpr int HEX_BIT_SHIFT = 4;
/* number of hex digits after % in percent-encoding */
constexpr size_t URL_PCT_HEX_DIGITS = 2;

/* lookup table mapping HTTP status codes to their standard text phrases */
const std::unordered_map<AccHttpStatusCode, std::string> &StatusTextMap()
{
    static const std::unordered_map<AccHttpStatusCode, std::string> m = {
        {AccHttpStatusCode::CONTINUE, "Continue"},
        {AccHttpStatusCode::SWITCHING_PROTOCOLS, "Switching Protocols"},
        {AccHttpStatusCode::OK, "OK"},
        {AccHttpStatusCode::CREATED, "Created"},
        {AccHttpStatusCode::ACCEPTED, "Accepted"},
        {AccHttpStatusCode::NO_CONTENT, "No Content"},
        {AccHttpStatusCode::MOVED_PERMANENTLY, "Moved Permanently"},
        {AccHttpStatusCode::FOUND, "Found"},
        {AccHttpStatusCode::NOT_MODIFIED, "Not Modified"},
        {AccHttpStatusCode::BAD_REQUEST, "Bad Request"},
        {AccHttpStatusCode::UNAUTHORIZED, "Unauthorized"},
        {AccHttpStatusCode::FORBIDDEN, "Forbidden"},
        {AccHttpStatusCode::NOT_FOUND, "Not Found"},
        {AccHttpStatusCode::METHOD_NOT_ALLOWED, "Method Not Allowed"},
        {AccHttpStatusCode::REQUEST_TIMEOUT, "Request Timeout"},
        {AccHttpStatusCode::CONFLICT, "Conflict"},
        {AccHttpStatusCode::GONE, "Gone"},
        {AccHttpStatusCode::LENGTH_REQUIRED, "Length Required"},
        {AccHttpStatusCode::PAYLOAD_TOO_LARGE, "Payload Too Large"},
        {AccHttpStatusCode::UNSUPPORTED_MEDIA_TYPE, "Unsupported Media Type"},
        {AccHttpStatusCode::TOO_MANY_REQUESTS, "Too Many Requests"},
        {AccHttpStatusCode::INTERNAL_SERVER_ERROR, "Internal Server Error"},
        {AccHttpStatusCode::NOT_IMPLEMENTED, "Not Implemented"},
        {AccHttpStatusCode::BAD_GATEWAY, "Bad Gateway"},
        {AccHttpStatusCode::SERVICE_UNAVAILABLE, "Service Unavailable"},
        {AccHttpStatusCode::GATEWAY_TIMEOUT, "Gateway Timeout"},
    };
    return m;
}

int HexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + HEX_BASE;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + HEX_BASE;
    }
    return -1;
}

std::string UrlDecode(const std::string &s, size_t start, size_t end)
{
    std::string out;
    out.reserve(end - start);
    for (size_t i = start; i < end; i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + URL_PCT_HEX_DIGITS < end) {
            int hi = HexValue(s[i + 1]);
            int lo = HexValue(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<unsigned char>((hi << HEX_BIT_SHIFT) | lo);
                i += URL_PCT_HEX_DIGITS;
            } else {
                out += s[i];
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

const std::string &AccHttpStatusText(uint16_t code)
{
    static const std::string unknown = "Unknown";
    auto &m = StatusTextMap();
    auto it = m.find(static_cast<AccHttpStatusCode>(code));
    return it != m.end() ? it->second : unknown;
}

AccHttpRequestContext::AccHttpRequestContext(const AccTcpLinkComplexPtr &link, const AccDataBufferPtr &body)
    : body_(body), link_(link)
{
    auto httpLink = AccConvert<AccTcpLinkComplex, AccHttpLinkDefault>(link_);
    if (httpLink.Get() != nullptr) {
        method_ = httpLink->method_;
        uri_ = httpLink->uri_;
        path_ = httpLink->path_;
        queryString_ = httpLink->queryString_;
        version_ = httpLink->httpVersion_;
        headers_ = httpLink->headers_;
    }
}

/* Parse query string into key-value pairs.
 * Note: URI fragment (the part after '#') is never sent to the server per
 * RFC 3986 §3.5, so it is not handled here; any '#' inside the query string
 * must be percent-encoded by the client as %23. */
static std::multimap<std::string, std::string> ParseQueryString(const std::string &qs)
{
    std::multimap<std::string, std::string> result;
    size_t pos = 0;
    while (pos < qs.size()) {
        auto amp = qs.find('&', pos);
        if (amp == std::string::npos) {
            amp = qs.size();
        }
        if (amp > pos) {
            auto eq = qs.find('=', pos);
            if (eq == std::string::npos || eq > amp) {
                result.emplace(UrlDecode(qs, pos, amp), "");
            } else {
                std::string key = UrlDecode(qs, pos, eq);
                std::string val = UrlDecode(qs, eq + 1, amp);
                result.emplace(std::move(key), std::move(val));
            }
        }
        pos = amp + 1;
    }
    return result;
}

std::string AccHttpRequestContext::GetParam(const std::string &key) const
{
    auto m = ParseQueryString(queryString_);
    auto it = m.find(key);
    return it != m.end() ? it->second : "";
}

std::multimap<std::string, std::string> AccHttpRequestContext::Params() const
{
    return ParseQueryString(queryString_);
}

Result AccHttpRequestContext::Reply(int16_t statusCode, const std::string &statusText, const std::string &contentType,
                                    const AccDataBufferPtr &body,
                                    const std::map<std::string, std::string> &extraHeaders)
{
    auto httpLink = AccConvert<AccTcpLinkComplex, AccHttpLinkDefault>(link_);
    if (httpLink.Get() == nullptr) {
        LOG_ERROR("Reply failed: link is not AccHttpLinkDefault, linkId=" << link_->Id());
        return ACC_ERROR;
    }
    return httpLink->SendHttpResponse(statusCode, statusText, contentType, body, extraHeaders);
}

/* wrap the string body in AccDataBuffer and delegate to the AccDataBufferPtr overload */
Result AccHttpRequestContext::Reply(int16_t statusCode, const std::string &statusText, const std::string &contentType,
                                    const std::string &body, const std::map<std::string, std::string> &extraHeaders)
{
    auto bodyBuf = AccDataBuffer::Create(body.data(), static_cast<uint32_t>(body.size()));
    if (bodyBuf.Get() == nullptr) {
        LOG_ERROR("Failed to allocate buffer for string body in Reply, bodySize=" << body.size()
                                                                                  << ", statusCode=" << statusCode);
        return ACC_MALLOC_FAIL;
    }
    return Reply(statusCode, statusText, contentType, bodyBuf, extraHeaders);
}

/* look up the status text from StatusTextMap() and delegate to the int16_t overload */
Result AccHttpRequestContext::Reply(AccHttpStatusCode statusCode, const std::string &contentType,
                                    const AccDataBufferPtr &body,
                                    const std::map<std::string, std::string> &extraHeaders)
{
    auto code = static_cast<uint16_t>(statusCode);
    return Reply(static_cast<int16_t>(code), AccHttpStatusText(code), contentType, body, extraHeaders);
}

/* wrap the string body and delegate to the AccHttpStatusCode + AccDataBufferPtr overload */
Result AccHttpRequestContext::Reply(AccHttpStatusCode statusCode, const std::string &contentType,
                                    const std::string &body, const std::map<std::string, std::string> &extraHeaders)
{
    auto bodyBuf = AccDataBuffer::Create(body.data(), static_cast<uint32_t>(body.size()));
    if (bodyBuf.Get() == nullptr) {
        LOG_ERROR("Failed to allocate buffer for string body in Reply, bodySize=" << body.size() << ", statusCode="
                                                                                  << static_cast<uint16_t>(statusCode));
        return ACC_MALLOC_FAIL;
    }
    return Reply(statusCode, contentType, bodyBuf, extraHeaders);
}

} // namespace acc
} // namespace ock
