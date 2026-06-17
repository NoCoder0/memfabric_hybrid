/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef ZBAL_AICPU_SDMA_SQE_H
#define ZBAL_AICPU_SDMA_SQE_H

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"

/* SQE type identifiers */
constexpr uint8_t AC_SQE_TYPE_SDMA = 11;
constexpr uint8_t AC_SQE_TYPE_NOTIFY_RECORD = 6;
constexpr uint8_t AC_SQE_TYPE_NOTIFY_WAIT = 7;

constexpr uint8_t AC_KERNEL_CREDIT_DEFAULT = 240U;
constexpr uint8_t AC_KERNEL_CREDIT_NOTIFY = 254U;
constexpr uint8_t AC_QOS_DEFAULT = 6;
constexpr uint8_t AC_LINK_TYPE_RESERVED = 255U;
constexpr uint32_t AC_ADDR_HIGH_SHIFT = 32U;
constexpr uint32_t AC_NOTIFY_SQE_RES5_COUNT = 11U;

/* ================================================================
 * SQE header (8 bytes, identical to stars_sqe_header_t in AIV)
 * ================================================================ */
struct AicpuSqeHeader {
    uint8_t type : 6; /* SQE type: 11=SDMA, 6=notify-record, 7=notify-wait */
    uint16_t res1 : 10;
    uint16_t block_dim;   /* always 0 for single-block */
    uint16_t rt_streamid; /* hardware stream ID from channel info */
    uint16_t task_id;     /* task sequence number */
};

/* ================================================================
 * SDMA Memcpy SQE (64 bytes packed, matches stars_sdma_sqe_t)
 *
 * Byte layout:
 *   [0-7]   header
 *   [8-11]  res3
 *   [12-13] res4
 *   [14]    kernel_credit = 240
 *   [15]    ptr_mode
 *   [16-19] opcode(8) ie2(1) sssv(1) dssv(1) sns(1) dns(1) qos(4) ...
 *   [20-21] src_streamid
 *   [22-23] src_sub_streamid
 *   [24-25] dst_streamid
 *   [26-27] dst_sub_streamid
 *   [28-31] length
 *   [32-35] src_addr_low
 *   [36-39] src_addr_high
 *   [40-43] dst_addr_low
 *   [44-47] dst_addr_high
 *   [48]    link_type
 *   [49-51] reserved
 *   [52-63] reslast[3]
 * ================================================================ */
struct AicpuStarsSdmaSqe {
    AicpuSqeHeader header; /* offset 0 */

    uint32_t res3;         /* offset 8 */
    uint16_t res4;         /* offset 12 */
    uint8_t kernel_credit; /* offset 14 */
    uint8_t ptr_mode : 1;  /* offset 15 */
    uint8_t res5 : 7;

    uint32_t opcode : 8; /* offset 16 */
    uint32_t ie2 : 1;
    uint32_t sssv : 1;
    uint32_t dssv : 1;
    uint32_t sns : 1;
    uint32_t dns : 1;
    uint32_t qos : 4;
    uint32_t sro : 1;
    uint32_t dro : 1;
    uint32_t partid : 8;
    uint32_t mpam : 1;
    uint32_t res6 : 4;

    uint16_t src_streamid;     /* offset 20 */
    uint16_t src_sub_streamid; /* offset 22 */
    uint16_t dst_streamid;     /* offset 24 */
    uint16_t dst_sub_streamid; /* offset 26 */

    uint32_t length; /* offset 28 */

    uint32_t src_addr_low;  /* offset 32 */
    uint32_t src_addr_high; /* offset 36 */
    uint32_t dst_addr_low;  /* offset 40 */
    uint32_t dst_addr_high; /* offset 44 */

    uint8_t link_type;   /* offset 48 */
    uint8_t reserved[3]; /* offset 49 */
    uint32_t reslast[3]; /* offset 52 */
};

/* ================================================================
 * Notify SQE (64 bytes packed, matches stars_notify_sqe_t)
 *
 * Used for inner-chip stream synchronization:
 *   type=6: notify-record (signal that stream reached this point)
 *   type=7: notify-wait   (block until notify_id is recorded)
 * ================================================================ */
struct AicpuStarsNotifySqe {
    AicpuSqeHeader header; /* offset 0 */

    uint32_t notify_id : 13; /* offset 8 */
    uint32_t res2 : 19;

    uint16_t res3;         /* offset 12 */
    uint8_t kernel_credit; /* offset 14 */
    uint8_t res4;          /* offset 15 */

    uint32_t timeout;                        /* offset 16 */
    uint32_t res5[AC_NOTIFY_SQE_RES5_COUNT]; /* offset 20..60 */
};

/*
 * Fill an SDMA memcpy SQE for local copy (same device).
 * Mirrors zbal_fill_sdma_sqe() in zbal_kernel_sdma_data_op.h.
 */
inline void AicpuFillMemcpySqe(AicpuStarsSdmaSqe *sqe, uint32_t streamId, uint64_t src, uint64_t dst, uint32_t len,
                               uint8_t opCode = 0)
{
    sqe->header.type = AC_SQE_TYPE_SDMA;
    sqe->header.block_dim = 0;
    sqe->header.rt_streamid = static_cast<uint16_t>(streamId);
    sqe->header.task_id = 0;

    sqe->kernel_credit = AC_KERNEL_CREDIT_DEFAULT;
    sqe->ptr_mode = 0;

    sqe->opcode = opCode; /* 0=memcpy, 1=sum, 2=max, 3=min */
    sqe->ie2 = 0;
    sqe->sssv = 1U;
    sqe->dssv = 1U;
    sqe->sns = 1U;
    sqe->dns = 1U;
    sqe->qos = AC_QOS_DEFAULT;
    sqe->sro = 1U;
    sqe->dro = 1U;
    sqe->partid = 0U;
    sqe->mpam = 0;

    sqe->src_streamid = 0;
    sqe->src_sub_streamid = 0;
    sqe->dst_streamid = 0;
    sqe->dst_sub_streamid = 0;

    sqe->length = len;

    sqe->src_addr_low = static_cast<uint32_t>(src & 0xFFFFFFFFU);
    sqe->src_addr_high = static_cast<uint32_t>((src >> AC_ADDR_HIGH_SHIFT) & 0xFFFFFFFFU);
    sqe->dst_addr_low = static_cast<uint32_t>(dst & 0xFFFFFFFFU);
    sqe->dst_addr_high = static_cast<uint32_t>((dst >> AC_ADDR_HIGH_SHIFT) & 0xFFFFFFFFU);

    sqe->link_type = AC_LINK_TYPE_RESERVED;
}

/*
 * Fill a Notify-Record SQE (type=6).
 */
inline void AicpuFillNotifyRecordSqe(AicpuStarsNotifySqe *sqe, uint32_t streamId, uint32_t notifyId)
{
    sqe->header.type = AC_SQE_TYPE_NOTIFY_RECORD;
    sqe->header.block_dim = 0;
    sqe->header.rt_streamid = static_cast<uint16_t>(streamId);
    sqe->header.task_id = 0;

    sqe->notify_id = notifyId;
    sqe->kernel_credit = AC_KERNEL_CREDIT_NOTIFY;

    sqe->res2 = 0;
    sqe->res3 = 0;
    sqe->res4 = 0;
    sqe->timeout = 0;
    for (auto &v : sqe->res5) {
        v = 0;
    }
}

/*
 * Fill a Notify-Wait SQE (type=7).
 */
inline void AicpuFillNotifyWaitSqe(AicpuStarsNotifySqe *sqe, uint32_t streamId, uint32_t notifyId)
{
    sqe->header.type = AC_SQE_TYPE_NOTIFY_WAIT;
    sqe->header.block_dim = 0;
    sqe->header.rt_streamid = static_cast<uint16_t>(streamId);
    sqe->header.task_id = 0;

    sqe->notify_id = notifyId;
    sqe->kernel_credit = AC_KERNEL_CREDIT_NOTIFY;

    sqe->res2 = 0;
    sqe->res3 = 0;
    sqe->res4 = 0;
    sqe->timeout = 0;
    for (auto &v : sqe->res5) {
        v = 0;
    }
}

/* Build a flag SQE: 8B SDMA copy src→dst. Used for completion detection. */
inline void AicpuBuildFlagSqe(volatile uint8_t *sqe, uint64_t srcAddr, uint64_t dstAddr, uint32_t len)
{
    AicpuFillMemcpySqe(reinterpret_cast<AicpuStarsSdmaSqe *>(const_cast<uint8_t *>(sqe)), 0, srcAddr, dstAddr, len);
}

#endif /* ZBAL_AICPU_SDMA_SQE_H */
