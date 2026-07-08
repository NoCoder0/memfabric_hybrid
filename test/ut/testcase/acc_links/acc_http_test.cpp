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

#include <mockcpp/mokc.h>
#include <mockcpp/mockcpp.hpp>
#include <gtest/gtest.h>
#include <cstring>
#include <string>

#define protected public
#define private   public
#include "acc_tcp_worker.h"
#include "acc_tcp_link.h"
#include "acc_tcp_link_complex_default.h"
#include "acc_includes.h"
#include "acc_tcp_server.h"
#include "acc_tcp_server_default.h"
#include "acc_tcp_listener.h"
#include "acc_http_link_default.h"
#include "acc_http_server.h"
#include "acc_http_server_default.h"
#include "acc_http_request_context.h"
#undef private
#undef protected

namespace {
using namespace ock::acc;

const int HTTP_PORT = 8200;
const int LINK_SEND_QUEUE_SIZE = 100;
const int WORKER_COUNT = 1;
const int MOCK_FD = 100;
constexpr size_t TEST_CONTENT_LEN = 100;
constexpr uint32_t CONTENT_LEN_42 = 42;
constexpr uint32_t BODY_LEN_9 = 9;
constexpr uint32_t BODY_LEN_4 = 4;
constexpr uint32_t EMPTY_BODY_LEN = 0;
constexpr int EXPECTED_CRLF_POS = 14;
constexpr uint32_t HTTP_HANDLERS_COUNT = 2;
constexpr uint32_t HTTP_HEADERS_COUNT = 2;
constexpr const char *REASON_NOT_FOUND = "Not Found";
constexpr const char *REASON_METHOD_NOT_ALLOWED = "Method Not Allowed";
constexpr const char *REASON_CONNECT_NOT_SUPPORTED = "CONNECT not supported";
constexpr size_t SMALL_MAX_BODY_SIZE = 10;

/* multimap has no operator[]; helper to fetch first value for a key in tests. */
std::string FirstHeader(const std::multimap<std::string, std::string> &m, const std::string &key)
{
    auto it = m.find(key);
    return it != m.end() ? it->second : "";
}

class AccHttpTest : public testing::Test {
public:
    void SetUp() override
    {
        mServer = AccTcpServer::Create();
        ASSERT_TRUE(mServer.Get() != nullptr);
    }

    void TearDown() override
    {
        mServer->Stop();
    }

    AccTcpServerPtr mServer;
};

TEST_F(AccHttpTest, test_http_create_server)
{
    auto httpServer = AccHttpServer::Create();
    ASSERT_TRUE(httpServer.Get() != nullptr);
}

TEST_F(AccHttpTest, test_http_create_server_default_tcp_binary)
{
    auto server = AccTcpServer::Create();
    ASSERT_TRUE(server.Get() != nullptr);
    auto tcpServer = dynamic_cast<AccTcpServerDefault *>(server.Get());
    ASSERT_TRUE(tcpServer != nullptr);
    auto httpServer = dynamic_cast<AccHttpServer *>(server.Get());
    ASSERT_TRUE(httpServer == nullptr);
}

TEST_F(AccHttpTest, test_http_link_reset_state)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->method_ = "GET";
    httpLink->uri_ = "/test";
    httpLink->httpVersion_ = "HTTP/1.1";
    httpLink->recvBuf_ = "some leftover data";
    httpLink->contentLength_ = TEST_CONTENT_LEN;
    httpLink->httpState_ = AccHttpParseState::READ_BODY;

    httpLink->ResetHttpState();

    ASSERT_EQ(httpLink->httpState_, AccHttpParseState::READ_REQUEST_LINE);
    ASSERT_TRUE(httpLink->recvBuf_.empty());
    ASSERT_TRUE(httpLink->method_.empty());
    ASSERT_TRUE(httpLink->uri_.empty());
    ASSERT_TRUE(httpLink->httpVersion_.empty());
    ASSERT_EQ(httpLink->contentLength_, 0U);
}

TEST_F(AccHttpTest, test_http_link_parse_request_line_valid)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->recvBuf_ = "GET /hello HTTP/1.1\r\n";
    auto ret = httpLink->ParseRequestLine();
    ASSERT_EQ(ret, ACC_OK);
    ASSERT_EQ(httpLink->method_, "GET");
    ASSERT_EQ(httpLink->uri_, "/hello");
    ASSERT_EQ(httpLink->path_, "/hello");
    ASSERT_EQ(httpLink->queryString_, "");
    ASSERT_EQ(httpLink->httpVersion_, "HTTP/1.1");
    ASSERT_EQ(httpLink->httpState_, AccHttpParseState::READ_HEADERS);
}

TEST_F(AccHttpTest, test_http_link_parse_request_line_with_query)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->recvBuf_ = "POST /api/data?key=val&a=b HTTP/1.1\r\n";
    auto ret = httpLink->ParseRequestLine();
    ASSERT_EQ(ret, ACC_OK);
    ASSERT_EQ(httpLink->method_, "POST");
    ASSERT_EQ(httpLink->uri_, "/api/data?key=val&a=b");
    ASSERT_EQ(httpLink->path_, "/api/data");
    ASSERT_EQ(httpLink->queryString_, "key=val&a=b");
    ASSERT_EQ(httpLink->httpVersion_, "HTTP/1.1");
}

TEST_F(AccHttpTest, test_http_link_parse_request_line_invalid)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->recvBuf_ = "INVALID_LINE_NO_CRLF";
    auto ret = httpLink->ParseRequestLine();
    ASSERT_EQ(ret, ACC_LINK_EAGAIN);

    httpLink->recvBuf_ = "INVALID_NO_SPACES\r\n";
    ret = httpLink->ParseRequestLine();
    ASSERT_EQ(ret, ACC_LINK_MSG_INVALID);
}

TEST_F(AccHttpTest, test_http_link_parse_request_line_unknown_method)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->recvBuf_ = "FOO /hello HTTP/1.1\r\n";
    auto ret = httpLink->ParseRequestLine();
    ASSERT_EQ(ret, ACC_LINK_MSG_INVALID);
}

TEST_F(AccHttpTest, test_http_link_parse_headers)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->httpState_ = AccHttpParseState::READ_HEADERS;
    httpLink->recvBuf_ = "Host: localhost:8200\r\nContent-Type: application/json\r\nContent-Length: 42\r\n\r\n";

    auto ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_OK);
    ASSERT_EQ(FirstHeader(httpLink->headers_, "host"), "localhost:8200");
    ASSERT_EQ(FirstHeader(httpLink->headers_, "content-type"), "application/json");
    ASSERT_EQ(FirstHeader(httpLink->headers_, "content-length"), "42");
    ASSERT_EQ(httpLink->contentLength_, CONTENT_LEN_42);
    ASSERT_EQ(httpLink->httpState_, AccHttpParseState::READ_BODY);
}

TEST_F(AccHttpTest, test_http_link_parse_headers_no_body)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->httpState_ = AccHttpParseState::READ_HEADERS;
    httpLink->recvBuf_ = "Host: localhost:8200\r\n\r\n";

    auto ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_OK);
    ASSERT_EQ(httpLink->contentLength_, 0U);
    ASSERT_EQ(httpLink->httpState_, AccHttpParseState::COMPLETE);
}

TEST_F(AccHttpTest, test_http_link_parse_headers_eagain)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->httpState_ = AccHttpParseState::READ_HEADERS;
    httpLink->recvBuf_ = "Host: localhost:8200\r\nCont";

    auto ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_LINK_EAGAIN);
}

TEST_F(AccHttpTest, test_http_link_parse_headers_reject_chunked)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->httpState_ = AccHttpParseState::READ_HEADERS;
    httpLink->method_ = "POST";
    httpLink->recvBuf_ = "Transfer-Encoding: chunked\r\n\r\n";

    auto ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_LINK_MSG_INVALID);
    ASSERT_TRUE(httpLink->needClose_);
}

TEST_F(AccHttpTest, test_http_find_line_end)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->recvBuf_ = "GET / HTTP/1.1\r\n";
    int pos = httpLink->FindLineEnd();
    ASSERT_EQ(pos, EXPECTED_CRLF_POS);

    httpLink->recvBuf_ = "no crlf here";
    pos = httpLink->FindLineEnd();
    ASSERT_EQ(pos, -1);
}

TEST_F(AccHttpTest, test_http_server_register_handler)
{
    auto server = AccHttpServer::Create();
    ASSERT_TRUE(server.Get() != nullptr);
    auto httpServer = dynamic_cast<AccHttpServerDefault *>(server.Get());
    ASSERT_TRUE(httpServer != nullptr);
    bool handlerCalled = false;
    httpServer->RegisterHttpHandler(AccHttpMethod::GET, "/test", [&handlerCalled](AccHttpRequestContext &) {
        handlerCalled = true;
        return ACC_OK;
    });

    httpServer->RegisterHttpHandler(AccHttpMethod::POST, "/api", [](AccHttpRequestContext &) { return ACC_OK; });

    ASSERT_EQ(httpServer->httpHandlers_.size(), HTTP_HANDLERS_COUNT);
    ASSERT_TRUE(httpServer->httpHandlers_.find("GET:/test") != httpServer->httpHandlers_.end());
    ASSERT_TRUE(httpServer->httpHandlers_.find("POST:/api") != httpServer->httpHandlers_.end());
}

TEST_F(AccHttpTest, test_http_server_start_without_listener)
{
    AccHttpServerOptions opts;
    opts.enableListener = false;
    opts.workerCount = 1;

    auto server = AccHttpServer::Create();
    ASSERT_TRUE(server.Get() != nullptr);
    auto httpServer = dynamic_cast<AccHttpServerDefault *>(server.Get());
    ASSERT_TRUE(httpServer != nullptr);

    httpServer->RegisterLinkBrokenHandler([](const AccTcpLinkComplexPtr &) -> int32_t { return ACC_OK; });
    httpServer->RegisterHttpHandler(AccHttpMethod::GET, "/", [](AccHttpRequestContext &) -> int32_t { return ACC_OK; });

    auto result = server->Start(opts, AccTlsOption());
    ASSERT_EQ(result, ACC_OK);
    server->Stop();
}

TEST_F(AccHttpTest, test_http_request_context_getters)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->method_ = "GET";
    httpLink->uri_ = "/test?k=v";
    httpLink->path_ = "/test";
    httpLink->queryString_ = "k=v";
    httpLink->httpVersion_ = "HTTP/1.1";
    httpLink->headers_.emplace("accept", "text/html");
    httpLink->headers_.emplace("host", "localhost");

    auto body = AccDataBuffer::Create("body data", BODY_LEN_9);
    ASSERT_TRUE(body.Get() != nullptr);

    AccHttpRequestContext ctx(httpLink.Get(), body);
    ASSERT_EQ(ctx.Method(), "GET");
    ASSERT_EQ(ctx.Uri(), "/test?k=v");
    ASSERT_EQ(ctx.Path(), "/test");
    ASSERT_EQ(ctx.QueryString(), "k=v");
    ASSERT_EQ(ctx.Version(), "HTTP/1.1");
    ASSERT_EQ(ctx.Headers().size(), HTTP_HEADERS_COUNT);
    ASSERT_EQ(ctx.GetHeader("accept"), "text/html");
    ASSERT_EQ(ctx.BodyLen(), BODY_LEN_9);
    ASSERT_TRUE(ctx.BodyPtr() != nullptr);
    ASSERT_TRUE(ctx.Link().Get() != nullptr);
    ASSERT_EQ(ctx.GetParam("k"), "v");
}

TEST_F(AccHttpTest, test_http_request_context_reply)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->method_ = "GET";
    httpLink->uri_ = "/test";
    httpLink->path_ = "/test";
    httpLink->httpVersion_ = "HTTP/1.1";
    httpLink->sendingQueue_ = AccMakeRef<AccLinkedMessageQueue>(LINK_SEND_QUEUE_SIZE);
    ASSERT_TRUE(httpLink->sendingQueue_.Get() != nullptr);

    auto body = AccDataBuffer::Create("test", BODY_LEN_4);
    ASSERT_TRUE(body.Get() != nullptr);

    AccHttpRequestContext ctx(httpLink.Get(), body);
    ASSERT_EQ(ctx.Method(), "GET");
    ASSERT_EQ(ctx.Uri(), "/test");
    ASSERT_EQ(ctx.BodyLen(), BODY_LEN_4);
}

TEST_F(AccHttpTest, test_http_link_parse_full_manually)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->recvBuf_ = "POST /api/data HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: 11\r\n"
                         "\r\n"
                         "Hello World";

    auto ret = httpLink->ParseRequestLine();
    ASSERT_EQ(ret, ACC_OK);
    ASSERT_EQ(httpLink->method_, "POST");
    ASSERT_EQ(httpLink->uri_, "/api/data");
    ASSERT_EQ(httpLink->httpVersion_, "HTTP/1.1");

    ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_OK);
    ASSERT_EQ(FirstHeader(httpLink->headers_, "content-type"), "application/json");
    ASSERT_EQ(httpLink->contentLength_, 11U);
    ASSERT_EQ(httpLink->httpState_, AccHttpParseState::READ_BODY);

    ret = httpLink->ParseBody();
    ASSERT_EQ(ret, ACC_OK);
    ASSERT_EQ(httpLink->httpState_, AccHttpParseState::COMPLETE);
}

TEST_F(AccHttpTest, test_http_server_handle_http_request_with_registered_handler)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->method_ = "GET";
    httpLink->uri_ = "/hello";
    httpLink->path_ = "/hello";
    httpLink->httpVersion_ = "HTTP/1.1";
    httpLink->data_ = AccDataBuffer::Create("", EMPTY_BODY_LEN);
    ASSERT_TRUE(httpLink->data_.Get() != nullptr);

    auto server = AccHttpServer::Create();
    ASSERT_TRUE(server.Get() != nullptr);
    auto httpServer = dynamic_cast<AccHttpServerDefault *>(server.Get());
    ASSERT_TRUE(httpServer != nullptr);

    bool handlerInvoked = false;
    httpServer->RegisterHttpHandler(
        AccHttpMethod::GET, "/hello", [&handlerInvoked](AccHttpRequestContext &ctx) -> int32_t {
            handlerInvoked = true;
            if (ctx.Method() != "GET" || ctx.Uri() != "/hello" || ctx.Version() != "HTTP/1.1") {
                return ACC_ERROR;
            }
            return ACC_OK;
        });

    AccTcpRequestContext reqCtx(AccMsgHeader{}, httpLink->data_, httpLink.Get());
    auto result = httpServer->HandleHttpRequest(reqCtx);
    ASSERT_EQ(result, ACC_OK);
    ASSERT_TRUE(handlerInvoked);
}

TEST_F(AccHttpTest, test_http_server_handle_http_request_no_handler)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->method_ = "GET";
    httpLink->uri_ = "/notfound";
    httpLink->path_ = "/notfound";
    httpLink->httpVersion_ = "HTTP/1.1";
    httpLink->data_ = AccDataBuffer::Create("", EMPTY_BODY_LEN);
    httpLink->sendingQueue_ = AccMakeRef<AccLinkedMessageQueue>(LINK_SEND_QUEUE_SIZE);
    ASSERT_TRUE(httpLink->data_.Get() != nullptr);
    ASSERT_TRUE(httpLink->sendingQueue_.Get() != nullptr);

    auto server = AccHttpServer::Create();
    auto httpServer = dynamic_cast<AccHttpServerDefault *>(server.Get());

    httpServer->RegisterHttpHandler(AccHttpMethod::GET, "/hello",
                                    [](AccHttpRequestContext &) -> int32_t { return ACC_OK; });

    AccTcpRequestContext reqCtx(AccMsgHeader{}, httpLink->data_, httpLink.Get());
    auto result = httpServer->HandleHttpRequest(reqCtx);
    ASSERT_EQ(result, ACC_LINK_MSG_INVALID);
}

TEST_F(AccHttpTest, test_http_server_handle_http_request_method_not_allowed)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    /* path /admin exists for GET, but request is DELETE -> 405 */
    httpLink->method_ = "DELETE";
    httpLink->uri_ = "/admin";
    httpLink->path_ = "/admin";
    httpLink->httpVersion_ = "HTTP/1.1";
    httpLink->data_ = AccDataBuffer::Create("", EMPTY_BODY_LEN);
    httpLink->sendingQueue_ = AccMakeRef<AccLinkedMessageQueue>(LINK_SEND_QUEUE_SIZE);

    auto server = AccHttpServer::Create();
    auto httpServer = dynamic_cast<AccHttpServerDefault *>(server.Get());

    httpServer->RegisterHttpHandler(AccHttpMethod::GET, "/admin",
                                    [](AccHttpRequestContext &) -> int32_t { return ACC_OK; });

    AccTcpRequestContext reqCtx(AccMsgHeader{}, httpLink->data_, httpLink.Get());
    auto result = httpServer->HandleHttpRequest(reqCtx);
    ASSERT_EQ(result, ACC_LINK_MSG_INVALID);
}

TEST_F(AccHttpTest, test_http_pipelining)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->recvBuf_ = "GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n"
                         "GET /b HTTP/1.1\r\nHost: localhost\r\n\r\n";

    auto ret = httpLink->ParseHttpMessage();
    ASSERT_EQ(ret, ACC_LINK_MSG_READY);
    ASSERT_EQ(httpLink->path_, "/a");

    httpLink->ResetForNextRequest();

    ret = httpLink->ParseHttpMessage();
    ASSERT_EQ(ret, ACC_LINK_MSG_READY);
    ASSERT_EQ(httpLink->path_, "/b");
}

TEST_F(AccHttpTest, test_http_split_body)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->httpState_ = AccHttpParseState::READ_HEADERS;
    httpLink->recvBuf_ = "Content-Length: 5\r\n\r\nHel";

    auto ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_OK);
    ASSERT_EQ(httpLink->httpState_, AccHttpParseState::READ_BODY);

    ret = httpLink->ParseBody();
    ASSERT_EQ(ret, ACC_LINK_EAGAIN);

    httpLink->recvBuf_ += "lo";
    ret = httpLink->ParseBody();
    ASSERT_EQ(ret, ACC_OK);
    ASSERT_EQ(httpLink->httpState_, AccHttpParseState::COMPLETE);
}

TEST_F(AccHttpTest, test_http_bad_request_invalid_version_prefix)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->recvBuf_ = "GET /hello HTCP/1.1\r\n";
    auto ret = httpLink->ParseRequestLine();
    ASSERT_EQ(ret, ACC_LINK_MSG_INVALID);
    ASSERT_TRUE(httpLink->needClose_);
}

TEST_F(AccHttpTest, test_http_bad_request_unsupported_version)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->recvBuf_ = "GET /hello HTTP/2.0\r\n";
    auto ret = httpLink->ParseRequestLine();
    ASSERT_EQ(ret, ACC_LINK_MSG_INVALID);
    ASSERT_TRUE(httpLink->needClose_);
}

TEST_F(AccHttpTest, test_http_bad_request_duplicate_content_length)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->httpState_ = AccHttpParseState::READ_HEADERS;
    httpLink->recvBuf_ = "Content-Length: 10\r\nContent-Length: 20\r\n\r\n";

    auto ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_LINK_MSG_INVALID);
    ASSERT_TRUE(httpLink->needClose_);
}

TEST_F(AccHttpTest, test_http_bad_request_content_length_too_large)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->maxBodySize_ = SMALL_MAX_BODY_SIZE;
    httpLink->httpState_ = AccHttpParseState::READ_HEADERS;
    httpLink->recvBuf_ = "Content-Length: 100\r\n\r\n";

    auto ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_LINK_MSG_INVALID);
    ASSERT_TRUE(httpLink->needClose_);
}

TEST_F(AccHttpTest, test_http_bad_request_content_length_invalid)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->httpState_ = AccHttpParseState::READ_HEADERS;
    httpLink->recvBuf_ = "Content-Length: abc\r\n\r\n";

    auto ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_LINK_MSG_INVALID);
    ASSERT_TRUE(httpLink->needClose_);
}

TEST_F(AccHttpTest, test_http_bad_request_header_missing_colon)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->httpState_ = AccHttpParseState::READ_HEADERS;
    httpLink->recvBuf_ = "Host localhost\r\n\r\n";

    auto ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_LINK_MSG_INVALID);
    ASSERT_TRUE(httpLink->needClose_);
}

TEST_F(AccHttpTest, test_http_bad_request_connect_method)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->method_ = "CONNECT";
    httpLink->uri_ = "/target";
    httpLink->path_ = "/target";
    httpLink->httpVersion_ = "HTTP/1.1";
    httpLink->data_ = AccDataBuffer::Create("", EMPTY_BODY_LEN);
    httpLink->sendingQueue_ = AccMakeRef<AccLinkedMessageQueue>(LINK_SEND_QUEUE_SIZE);
    ASSERT_TRUE(httpLink->data_.Get() != nullptr);
    ASSERT_TRUE(httpLink->sendingQueue_.Get() != nullptr);

    auto server = AccHttpServer::Create();
    auto httpServer = dynamic_cast<AccHttpServerDefault *>(server.Get());

    AccTcpRequestContext reqCtx(AccMsgHeader{}, httpLink->data_, httpLink.Get());
    auto result = httpServer->HandleHttpRequest(reqCtx);
    ASSERT_EQ(result, ACC_LINK_MSG_INVALID);
    ASSERT_TRUE(httpLink->needClose_);
}

TEST_F(AccHttpTest, test_http_bad_request_headers_too_large)
{
    std::string ipPort = "127.0.0.1:8200";
    int fd = MOCK_FD;
    AccHttpLinkDefaultPtr httpLink = AccMakeRef<AccHttpLinkDefault>(fd, ipPort, AccTcpLinkDefault::NewId());
    ASSERT_TRUE(httpLink.Get() != nullptr);

    httpLink->httpState_ = AccHttpParseState::READ_HEADERS;
    /* oversized header line without trailing CRLF so FindLineEnd returns -1 */
    std::string oversized(HTTP_MAX_HEADER_SIZE + 1, 'A');
    httpLink->recvBuf_ = "X-Large: " + oversized;

    auto ret = httpLink->ParseHeaders();
    ASSERT_EQ(ret, ACC_LINK_MSG_INVALID);
    ASSERT_TRUE(httpLink->needClose_);
}
} // namespace
