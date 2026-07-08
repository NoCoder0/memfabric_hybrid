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

#ifndef ACC_LINKS_ACC_HTTP_REQUEST_CONTEXT_H
#define ACC_LINKS_ACC_HTTP_REQUEST_CONTEXT_H

#include <map>
#include <string>

#include "acc_tcp_request_context.h"

namespace ock {
namespace acc {

/**
 * @brief HTTP methods
 */
enum class AccHttpMethod : uint8_t {
    GET,     /**< GET method */
    HEAD,    /**< HEAD method */
    POST,    /**< POST method */
    PUT,     /**< PUT method */
    DELETE,  /**< DELETE method */
    CONNECT, /**< CONNECT method */
    OPTIONS, /**< OPTIONS method */
    TRACE,   /**< TRACE method */
    PATCH,   /**< PATCH method */
};

/**
 * @brief Convert AccHttpMethod to its string representation
 *
 * @param method     [in] HTTP method enum value
 * @return const reference to the method string (e.g. "GET", "POST")
 */
inline const std::string &AccHttpMethodToString(AccHttpMethod method)
{
    static const std::string names[] = {
        "GET", "HEAD", "POST", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE", "PATCH",
    };
    auto idx = static_cast<uint8_t>(method);
    static const std::string unknown = "UNKNOWN";
    return idx < sizeof(names) / sizeof(names[0]) ? names[idx] : unknown;
}

/**
 * @brief Common HTTP status codes
 */
enum class AccHttpStatusCode : uint16_t {
    CONTINUE = 100,               /**< 100 Continue */
    SWITCHING_PROTOCOLS = 101,    /**< 101 Switching Protocols */
    OK = 200,                     /**< 200 OK */
    CREATED = 201,                /**< 201 Created */
    ACCEPTED = 202,               /**< 202 Accepted */
    NO_CONTENT = 204,             /**< 204 No Content */
    MOVED_PERMANENTLY = 301,      /**< 301 Moved Permanently */
    FOUND = 302,                  /**< 302 Found */
    NOT_MODIFIED = 304,           /**< 304 Not Modified */
    BAD_REQUEST = 400,            /**< 400 Bad Request */
    UNAUTHORIZED = 401,           /**< 401 Unauthorized */
    FORBIDDEN = 403,              /**< 403 Forbidden */
    NOT_FOUND = 404,              /**< 404 Not Found */
    METHOD_NOT_ALLOWED = 405,     /**< 405 Method Not Allowed */
    REQUEST_TIMEOUT = 408,        /**< 408 Request Timeout */
    CONFLICT = 409,               /**< 409 Conflict */
    GONE = 410,                   /**< 410 Gone */
    LENGTH_REQUIRED = 411,        /**< 411 Length Required */
    PAYLOAD_TOO_LARGE = 413,      /**< 413 Payload Too Large */
    UNSUPPORTED_MEDIA_TYPE = 415, /**< 415 Unsupported Media Type */
    TOO_MANY_REQUESTS = 429,      /**< 429 Too Many Requests */
    INTERNAL_SERVER_ERROR = 500,  /**< 500 Internal Server Error */
    NOT_IMPLEMENTED = 501,        /**< 501 Not Implemented */
    BAD_GATEWAY = 502,            /**< 502 Bad Gateway */
    SERVICE_UNAVAILABLE = 503,    /**< 503 Service Unavailable */
    GATEWAY_TIMEOUT = 504,        /**< 504 Gateway Timeout */
};

/**
 * @brief Look up the standard status text for an HTTP status code
 *
 * @param code [in] numeric HTTP status code
 * @return const reference to the status text (e.g. "OK"), "Unknown" if not found
 */
const std::string &AccHttpStatusText(uint16_t code);

/**
 * @brief Look up the standard status text for an AccHttpStatusCode enum value
 */
inline const std::string &AccHttpStatusText(AccHttpStatusCode code)
{
    return AccHttpStatusText(static_cast<uint16_t>(code));
}

/**
 * @brief Common HTTP header names as compile-time string constants
 */
struct AccHttpHeaderName {
    static constexpr const char *HOST = "Host";
    static constexpr const char *CONTENT_TYPE = "Content-Type";
    static constexpr const char *CONTENT_LENGTH = "Content-Length";
    static constexpr const char *CONNECTION = "Connection";
    static constexpr const char *TRANSFER_ENCODING = "Transfer-Encoding";
    static constexpr const char *CONTENT_ENCODING = "Content-Encoding";
    static constexpr const char *ACCEPT = "Accept";
    static constexpr const char *ACCEPT_ENCODING = "Accept-Encoding";
    static constexpr const char *AUTHORIZATION = "Authorization";
    static constexpr const char *COOKIE = "Cookie";
    static constexpr const char *SET_COOKIE = "Set-Cookie";
    static constexpr const char *CACHE_CONTROL = "Cache-Control";
    static constexpr const char *LOCATION = "Location";
    static constexpr const char *USER_AGENT = "User-Agent";
    static constexpr const char *SERVER = "Server";
    static constexpr const char *DATE = "Date";
    static constexpr const char *UPGRADE = "Upgrade";
    static constexpr const char *ORIGIN = "Origin";
    static constexpr const char *REFERER = "Referer";
    static constexpr const char *KEEP_ALIVE = "Keep-Alive";
};

/**
 * @brief HTTP request context passed to registered handlers.
 *
 * The context is created automatically by AccHttpServer when a complete
 * HTTP request is received. It provides access to the request method,
 * URI, version, headers, body, and the underlying link. Use Reply() to
 * send an HTTP response back to the client.
 */
class ACC_API AccHttpRequestContext : public AccReferable {
public:
    /**
     * @brief Construct an HTTP request context
     *
     * @param link [in] the underlying HTTP link (already holds method/uri/version/headers)
     * @param body [in] request body data buffer
     */
    AccHttpRequestContext(const AccTcpLinkComplexPtr &link, const AccDataBufferPtr &body);

    /**
     * @brief Get the HTTP method
     *
     * @return method string
     */
    const std::string &Method() const
    {
        return method_;
    }

    /**
     * @brief Get the request URI
     *
     * @return URI string
     */
    const std::string &Uri() const
    {
        return uri_;
    }

    /**
     * @brief Get the request path (URI without query string)
     *
     * @return path string
     */
    const std::string &Path() const
    {
        return path_;
    }

    /**
     * @brief Get the raw query string from the URI
     *
     * @return query string, empty if none
     */
    const std::string &QueryString() const
    {
        return queryString_;
    }

    /**
     * @brief Get a single query parameter value by key
     *
     * @param key [in] parameter name
     * @return value, empty string if not found
     */
    std::string GetParam(const std::string &key) const;

    /**
     * @brief Parse and return all query parameters
     *
     * @return multimap of key-value pairs
     */
    std::multimap<std::string, std::string> Params() const;

    /**
     * @brief Get the HTTP version
     *
     * @return version string
     */
    const std::string &Version() const
    {
        return version_;
    }

    /**
     * @brief Get the request headers (keys are lowercase)
     *
     * @return multimap of header key-value pairs, preserving duplicates
     */
    const std::multimap<std::string, std::string> &Headers() const
    {
        return headers_;
    }

    /**
     * @brief Get the first value of a header by lowercase key
     *
     * @param key [in] lowercase header name
     * @return value, empty string if not found
     */
    std::string GetHeader(const std::string &key) const
    {
        auto it = headers_.find(key);
        return it != headers_.end() ? it->second : "";
    }

    /**
     * @brief Get the request body buffer
     *
     * @return shared pointer to body data buffer
     */
    const AccDataBufferPtr &Body() const
    {
        return body_;
    }

    /**
     * @brief Get a pointer to the request body data
     *
     * @return void pointer to body data, nullptr if no body
     */
    void *BodyPtr() const
    {
        return body_.Get() ? body_->DataPtrVoid() : nullptr;
    }

    /**
     * @brief Get the length of the request body
     *
     * @return body length in bytes
     */
    uint32_t BodyLen() const
    {
        return body_.Get() ? body_->DataLen() : 0;
    }

    /**
     * @brief Get the underlying TCP link
     *
     * @return shared pointer to the link
     */
    const AccTcpLinkComplexPtr &Link() const
    {
        return link_;
    }

    /**
     * @brief Send an HTTP response (with explicit status code and text)
     *
     * @param statusCode   [in] HTTP status code (e.g. 200, 404)
     * @param statusText   [in] HTTP status text (e.g. "OK", "Not Found")
     * @param contentType  [in] Content-Type header value
     * @param body         [in] response body data buffer
     * @param extraHeaders [in] optional extra response headers
     * @return ACC_OK if the response is queued successfully
     */
    Result Reply(int16_t statusCode, const std::string &statusText, const std::string &contentType,
                 const AccDataBufferPtr &body, const std::map<std::string, std::string> &extraHeaders = {});

    /**
     * @brief Send an HTTP response with a string body (with explicit status code and text)
     *
     * @param statusCode   [in] HTTP status code
     * @param statusText   [in] HTTP status text
     * @param contentType  [in] Content-Type header value
     * @param body         [in] response body string
     * @param extraHeaders [in] optional extra response headers
     * @return ACC_OK if the response is queued successfully
     */
    Result Reply(int16_t statusCode, const std::string &statusText, const std::string &contentType,
                 const std::string &body, const std::map<std::string, std::string> &extraHeaders = {});

    /**
     * @brief Send an HTTP response using AccHttpStatusCode enum
     *
     * @param statusCode   [in] HTTP status code enum value
     * @param contentType  [in] Content-Type header value
     * @param body         [in] response body data buffer
     * @param extraHeaders [in] optional extra response headers
     * @return ACC_OK if the response is queued successfully
     */
    Result Reply(AccHttpStatusCode statusCode, const std::string &contentType, const AccDataBufferPtr &body,
                 const std::map<std::string, std::string> &extraHeaders = {});

    /**
     * @brief Send an HTTP response using AccHttpStatusCode enum with a string body
     *
     * @param statusCode   [in] HTTP status code enum value
     * @param contentType  [in] Content-Type header value
     * @param body         [in] response body string
     * @param extraHeaders [in] optional extra response headers
     * @return ACC_OK if the response is queued successfully
     */
    Result Reply(AccHttpStatusCode statusCode, const std::string &contentType, const std::string &body,
                 const std::map<std::string, std::string> &extraHeaders = {});

private:
    std::string method_;
    std::string uri_;
    std::string path_;
    std::string queryString_;
    std::string version_;
    std::multimap<std::string, std::string> headers_;
    AccDataBufferPtr body_;
    AccTcpLinkComplexPtr link_;
};

} // namespace acc
} // namespace ock

#endif // ACC_LINKS_ACC_HTTP_REQUEST_CONTEXT_H
