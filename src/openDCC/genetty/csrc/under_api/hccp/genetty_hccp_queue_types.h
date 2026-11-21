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
#ifndef MF_GENETTY_HCCP_QUEUE_TYPES_H
#define MF_GENETTY_HCCP_QUEUE_TYPES_H
#include <cstdint>
namespace ock {
namespace openDCC {
constexpr uint32_t HCCP_GID_LENGTH = 16;
/* ABI layouts used by the HCCP extended QP and lite completion APIs. */
struct HccpQpAttr {
    unsigned int qpn;
    unsigned int udpSport;
    unsigned int psn;
    unsigned int gidIdx;
    unsigned char gid[HCCP_GID_LENGTH];
    int pathMtu;
};

struct HccpTypicalQp {
    uint32_t qpn;
    uint32_t psn;
    uint32_t gidIdx;
    uint8_t resv1[4];
    uint8_t gid[HCCP_GID_LENGTH];
    uint32_t tc;
    uint32_t sl;
    uint32_t retryCnt;
    uint32_t retryTime;
    int version;
    uint32_t reserved[32];
    uint8_t resv2[4];
};

struct HccpCqExtAttr {
    int sendCqDepth;
    int recvCqDepth;
    int sendCqCompVector;
    int recvCqCompVector;
};

enum HccpIbvQpType {
    HCCP_QPT_RC = 2,
    HCCP_QPT_UC,
    HCCP_QPT_UD,
    HCCP_QPT_RAW_PACKET = 8,
    HCCP_QPT_XRC_SEND = 9,
    HCCP_QPT_XRC_RECV,
    HCCP_QPT_DRIVER = 0xff,
};

struct HccpIbvQpCap {
    uint32_t max_send_wr;
    uint32_t max_recv_wr;
    uint32_t max_send_sge;
    uint32_t max_recv_sge;
    uint32_t max_inline_data;
};

struct HccpIbvQpInitAttr {
    void *qp_context;
    void *send_cq;
    void *recv_cq;
    void *srq;
    HccpIbvQpCap cap;
    int qp_type;
    int sq_sig_all;
};

union HccpAiDataPlaneCstmFlag {
    struct {
        uint32_t cq_cstm : 1;
        uint32_t reserved : 31;
    } bs;
    uint32_t value;
};

enum HccpQpMode {
    HCCP_QP_MODE_OP_EXT = 4,
};

struct HccpQpExtAttrs {
    int qpMode;
    HccpCqExtAttr cqAttr;
    HccpIbvQpInitAttr qpAttr;
    int version;
    int memAlign;
    uint32_t udpSport;
    union HccpAiDataPlaneCstmFlag dataPlaneFlag;
    union {
        struct {
            uint32_t useResvMem : 1;
            uint32_t reserved0 : 31;
        } bs;
        uint32_t value;
    } cstmFlag;
    uint32_t resvMemPoolId;
    uint32_t reserved[27];
};

struct HccpSgList {
    uint64_t addr;
    uint32_t len;
    uint32_t lkey;
};

struct HccpWrAuxInfo {
    uint8_t data_type;
    uint8_t reduce_type;
    uint32_t notify_offset;
};

struct HccpWrExtInfo {
    uint32_t imm_data;
    uint16_t resv;
};

struct HccpSendWrV2 {
    uint64_t wr_id;
    struct HccpSgList *buf_list;
    uint16_t buf_num;
    uint64_t dst_addr;
    uint32_t rkey;
    uint32_t op;
    int send_flag;
    union {
        struct HccpWrAuxInfo aux;
        struct HccpWrExtInfo ext;
    };
};

struct HccpWqeInfo {
    unsigned int sq_index;
    unsigned int wqe_index;
};

struct HccpDbInfo {
    unsigned int db_index;
    unsigned long db_info;
};

struct HccpSendWrRsp {
    union {
        struct HccpWqeInfo wqe_tmp;
        struct HccpDbInfo db;
    };
};

struct HccpLiteWc {
    uint64_t wr_id;
    int status;
    int opcode;
    uint32_t vendor_err;
    uint32_t byte_len;
    uint32_t qp_num;
    uint32_t wc_flags;
};

struct HccpLiteWcExt {
    uint32_t imm_data;
    uint16_t reserved[5];
    uint32_t version;
};

struct HccpLiteWcV2 {
    HccpLiteWc wc;
    HccpLiteWcExt ext;
};

enum HccpRaSendFlags {
    HCCP_RA_SEND_FENCE = 1 << 0,
    HCCP_RA_SEND_SIGNALED = 1 << 1,
    HCCP_RA_SEND_SOLICITED = 1 << 2,
    HCCP_RA_SEND_INLINE = 1 << 3,
};

} // namespace openDCC
} // namespace ock
#endif // MF_GENETTY_HCCP_QUEUE_TYPES_H
