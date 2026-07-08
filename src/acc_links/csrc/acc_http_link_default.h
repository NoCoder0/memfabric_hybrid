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

#ifndef ACC_LINKS_ACC_HTTP_LINK_DEFAULT_H
#define ACC_LINKS_ACC_HTTP_LINK_DEFAULT_H

#include <ctime>
#include <map>
#include <string>

#include "acc_tcp_link_default.h"
#include "acc_tcp_link_complex_default.h"
#include "acc_http_request_context.h"

namespace ock {
namespace acc {

/* valid HTTP status code range (RFC 7230 §3.1.2) */
constexpr int16_t HTTP_STATUS_CODE_MIN = 100;
constexpr int16_t HTTP_STATUS_CODE_MAX = 599;
/* informational (1xx) status codes end before 200 */
constexpr int16_t HTTP_STATUS_CODE_1XX_END = 200;
/* status codes that never carry a body */
constexpr int16_t HTTP_STATUS_NO_CONTENT = static_cast<int16_t>(AccHttpStatusCode::NO_CONTENT);
constexpr int16_t HTTP_STATUS_NOT_MODIFIED = static_cast<int16_t>(AccHttpStatusCode::NOT_MODIFIED);

/* max size of HTTP request header section (request line + headers) */
constexpr uint32_t HTTP_MAX_HEADER_SIZE = 8192;
/* keep-alive idle timeout in seconds */
constexpr uint32_t HTTP_KEEPALIVE_TIMEOUT_S = 60;
/* recv buffer size used per PollInRecv call */
constexpr uint32_t HTTP_RECV_BUF_SIZE = 4096;
/* accept-thread poll timeout (ms) */
constexpr int32_t HTTP_LISTENER_POLL_TIMEOUT_MS = 500;
/* length of CRLF (\r\n) */
constexpr size_t CRLF_LEN = 2;
/* length of "HTTP/" prefix in version string */
constexpr size_t HTTP_PREFIX_LEN = 5;

/**
 * @brief HTTP request parsing state machine states
 */
enum class AccHttpParseState : uint8_t {
    READ_REQUEST_LINE, /**< parsing "METHOD /path HTTP/1.1\r\n" */
    READ_HEADERS,      /**< parsing header lines until \r\n */
    READ_BODY,         /**< reading body according to Content-Length */
    COMPLETE,          /**< full request received, ready for dispatch */
};

/**
 * @brief Small helper to parse a single request-line "METHOD SP uri SP version".
 *
 * Constructed with the raw line (without trailing CRLF); call Valid() before
 * accessing Method()/Uri()/Version(). Splits the uri into path and query string.
 */
struct RequestLineParser {
    std::string method{};
    std::string uri{};
    std::string path{};
    std::string queryString{};
    std::string version{};
    bool valid = false;

    explicit RequestLineParser(const std::string &line)
    {
        auto pos1 = line.find(' ');
        if (pos1 == std::string::npos) {
            return;
        }
        auto pos2 = line.rfind(' ');
        if (pos2 == std::string::npos || pos2 == pos1) {
            return;
        }
        method = line.substr(0, pos1);
        uri = line.substr(pos1 + 1, pos2 - pos1 - 1);
        version = line.substr(pos2 + 1);
        auto qpos = uri.find('?');
        if (qpos != std::string::npos) {
            path = uri.substr(0, qpos);
            queryString = uri.substr(qpos + 1);
        } else {
            path = uri;
        }
        valid = !method.empty() && !uri.empty() && !version.empty();
    }
};

class AccHttpLinkDefault : public AccTcpLinkDefault {
public:
    AccHttpLinkDefault(int fd, std::string ipPort, uint32_t id, SSL *ssl = nullptr)
        : AccTcpLinkDefault(fd, std::move(ipPort), id, ssl)
    {}

    AccHttpLinkDefault(int fd, std::string ipPort, SSL *ssl = nullptr)
        : AccTcpLinkDefault(fd, std::move(ipPort), AccTcpLinkDefault::NewId(), ssl)
    {}

    ~AccHttpLinkDefault() override
    {
        UnInitialize();
    }

    void ResetHttpState();

    bool IsIdleExpired();
    Result Initialize(uint16_t sendQueueCap, int32_t workIndex, AccTcpWorker *worker) override;
    void UnInitialize() override;

    AccLinkedMessageNode *TakeAwayMessages() override;
    Result EnqueueFront(AccLinkedMessageNode *node) override;
    AccLinkedMessageNode *DequeueFront() override;

    void SetMaxBodySize(size_t sz)
    {
        maxBodySize_ = sz;
    }

protected:
    bool HasBufferedRequest() const override;
    bool HasPendingCleanup() const override
    {
        return needClose_;
    }
    Result HandlePollIn() noexcept override;
    Result HandlePollOut(AccMsgHeader &header, AccDataBufferPtr &cbCtx) noexcept override;

private:
    Result ParseRequestLine();
    /* validate httpVersion_ format and reject unsupported versions */
    Result ValidateHttpVersion();
    Result ParseHeaders();
    Result ParseBody();
    Result AppendRecvData();
    int FindLineEnd() const;

    /* finalize headers after the empty-line terminator: parse Content-Length,
     * check Connection/Transfer-Encoding/Expect, transition to READ_BODY or COMPLETE */
    Result FinalizeHeaders();
    /* parse Content-Length header, reject duplicates/overflow; return ACC_OK or error */
    Result ParseContentLength();
    /* parse a single header line (folding, colon split, OWS trim, store) */
    Result ParseHeaderLine(const std::string &line);

    /* reset parse state for the next keep-alive request */
    void ResetForNextRequest();
    /* parse HTTP message through request-line/headers/body states; return ACC_LINK_MSG_READY
     * when a full message is buffered in data_ */
    Result ParseHttpMessage();

    /* write the remaining payload (data) of a dequeued send node; on full send
     * delete the node and close if needed, returning the send result code */
    Result WritePayload(AccLinkedMessageNode *oneMsg);

    /* build the HTTP response header section string (status line + headers + CRLF) */
    std::string BuildResponseHeader(int16_t statusCode, const std::string &statusText,
                                    const std::string &contentType,
                                    const AccDataBufferPtr &body,
                                    const std::map<std::string, std::string> &extraHeaders,
                                    bool noBodyForStatus, bool noBody);
    /* enqueue header and body nodes into sendingQueue_, returns ACC_OK or error */
    Result EnqueueResponseNodes(const AccDataBufferPtr &headerBuf, const AccDataBufferPtr &body, bool noBodyForStatus,
                                bool noBody, int16_t statusCode);

    void SendErrorResponse(int16_t statusCode, const std::string &statusText);
    void SendErrorResponse(AccHttpStatusCode statusCode)
    {
        SendErrorResponse(static_cast<int16_t>(statusCode), AccHttpStatusText(statusCode));
    }

    Result SendHttpResponse(int16_t statusCode, const std::string &statusText, const std::string &contentType,
                            const AccDataBufferPtr &body, const std::map<std::string, std::string> &extraHeaders = {},
                            bool noBody = false);

    static std::string ToLower(const std::string &s);
    static bool IsKnownMethod(const std::string &method);
    AccHttpParseState httpState_ = AccHttpParseState::READ_REQUEST_LINE;
    std::string recvBuf_;
    std::string method_;
    std::string uri_;         /* full raw URI (with query string) */
    std::string path_;        /* URI path only, e.g. /api/data */
    std::string queryString_; /* URI query string only, e.g. key=val&a=b */
    std::string httpVersion_;
    std::string prevHeaderKey_; /* for obsolete line folding */
    std::string prevHeaderVal_;
    std::multimap<std::string, std::string> headers_; /* keys normalized to lowercase */
    size_t contentLength_ = 0;
    bool needClose_ = false;                          /* send Connection: close after response */
    time_t lastActivity_{0};                          /* last IO activity timestamp */
    size_t maxBodySize_ = HTTP_DEFAULT_MAX_BODY_SIZE; /* per-link body size limit */

    AccLinkedMessageQueuePtr sendingQueue_{nullptr};

    friend class AccHttpServerDefault;
    friend class AccHttpRequestContext;
};

using AccHttpLinkDefaultPtr = AccRef<AccHttpLinkDefault>;

} // namespace acc
} // namespace ock

#endif // ACC_LINKS_ACC_HTTP_LINK_DEFAULT_H
