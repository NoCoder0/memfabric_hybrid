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
/**
 * @file hcc_server_messages.h
 * @brief Internal message definitions used by the HCC server.
 */
#ifndef MF_OPENDCC_HCC_SERVER_MESSAGES_H
#define MF_OPENDCC_HCC_SERVER_MESSAGES_H

namespace ock {
namespace openDCC {

enum HccSvrReqOp {
    HS_OP_CONNECT_REQ = 0,
    HS_OP_DISCONNECT_REQ,
    HS_OP_EXCHANGE_LINK_INFO_REQ,
};

enum HccSvrRespOp {
    HS_OP_CONNECT_RESP = 0,
    HS_OP_DISCONNECT_RESP,
    HS_OP_EXCHANGE_LINK_INFO_RESP,
};

} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_HCC_SERVER_MESSAGES_H
