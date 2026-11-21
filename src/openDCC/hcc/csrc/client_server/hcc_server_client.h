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
#ifndef MF_OPENDCC_HCC_SERVER_CLIENT_H
#define MF_OPENDCC_HCC_SERVER_CLIENT_H

#include "acc_def.h"
#include "hcc_server_types.h"
#include "hcc_server_socket.h"

#include <map>
#include <mutex>

namespace ock {
namespace openDCC {
class HccServerClient final : public Referable {
public:
    static HccServerClient &Instance() noexcept
    {
        static HccServerClient instance;
        return instance;
    }

public:
    HccServerClient() noexcept = default;
    ~HccServerClient() override
    {
        Stop();
    }

    Result Start(const TcpServerOptions &options) noexcept;
    void Stop() noexcept;

    Result ConnectAsync(const ExternalConnectionHandlerPtr& h) noexcept;

    Result RegisterCommunicator(const HccCommunicatorPtr &communicator) noexcept;
    void UnregisterCommunicator(const HccCommunicatorPtr &communicator) noexcept;

private:


private:
    TcpServerPtr socketServer_;
    std::mutex mutex_;
    bool started_ = false;
    TcpServerOptions options_;
    std::map<int, ExternalConnectionHandler> issuedConnects_;
    std::map<uint64_t, HccCommunicatorPtr> communicators_;
};
using HccServerClientPtr = Ref<HccServerClient>;

} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_HCC_SERVER_CLIENT_H
