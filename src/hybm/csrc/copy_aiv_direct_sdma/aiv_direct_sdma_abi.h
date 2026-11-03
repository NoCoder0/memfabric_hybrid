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
// A3 STARS ABI, derived from the Mulan PSL v2 ZBAL implementation in this repository.
#pragma once
#include <cstdint>
#include <cstddef>
constexpr uint32_t AIV_DIRECT_SDMA_ABI_VERSION = 1;
constexpr uint64_t AIV_DIRECT_SDMA_QUEUE_INFO_OFFSET = 64;
constexpr uint64_t AIV_DIRECT_SDMA_NOTIFY_ID_OFFSET = 14ULL * 1024;
constexpr uint64_t AIV_DIRECT_SDMA_DOORBELL_TAIL_OFFSET = 8;
constexpr uint32_t AIV_DIRECT_SDMA_CONSUMED_STRIDE = 64;
constexpr uint8_t AIV_DIRECT_SDMA_DATA_KERNEL_CREDIT = 240;
constexpr uint8_t AIV_DIRECT_SDMA_NOTIFY_KERNEL_CREDIT = 254;
struct StarsQueueInfo {
    uint32_t sq_head;
    uint32_t sq_tail;
    uint64_t sq_base;     // SQ缓冲区基地址
    uint64_t sq_reg_base; // SQ寄存器基地址
    uint32_t sq_depth;
    uint32_t sq_id;
    uint32_t cq_id;
    uint32_t logic_cq_id;
    uint64_t cqe_addr;
    uint32_t report_cqe_num;
    uint32_t stream_id;
    uint32_t dev_id;
    uint8_t reserved[4]; // 对齐到64字节
};

struct stars_sqe_header_t {
    uint8_t type : 6;
    uint16_t res1 : 10;
    uint16_t block_dim;
    uint16_t rt_streamid;
    uint16_t task_id;
};

struct stars_sdma_sqe_t {
    stars_sqe_header_t header;
    /**** 8 bytpes ****/

    uint32_t res3;
    /* *******12 bytes********* */

    uint16_t res4;
    uint8_t kernel_credit;
    uint8_t ptr_mode : 1;
    uint8_t res5 : 7;
    /**** 16 bytpes ****/

    uint32_t opcode : 8;
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
    /**** 20 bytpes ****/

    uint16_t src_streamid;
    uint16_t src_sub_streamid;
    /**** 24 bytpes ****/

    uint16_t dst_streamid;
    uint16_t dst_sub_streamid;

    uint32_t length;
    /**** 32 bytpes ****/

    uint32_t src_addr_low;
    uint32_t src_addr_high;
    uint32_t dst_addr_low;
    uint32_t dst_addr_high;
    /**** 48 bytpes ****/

    uint8_t link_type;
    uint8_t resvered[3];
    uint32_t reslast[3];
    /**** 64 bytpes ****/
};

struct stars_notify_sqe_t {
    stars_sqe_header_t header;
    /**** 8 bytpes ****/

    uint32_t notify_id : 13;
    uint32_t res2 : 19;

    uint16_t res3;
    uint8_t kernel_credit;
    uint8_t res4;
    /**** 16 bytpes ****/

    uint32_t timeout;
    uint32_t res5[11];
    /**** 64 bytpes ****/
};

static_assert(sizeof(StarsQueueInfo) == 64, "STARS queue ABI");
static_assert(sizeof(stars_sdma_sqe_t) == 64, "A3 data SQE ABI");
static_assert(sizeof(stars_notify_sqe_t) == 64, "A3 notify SQE ABI");
struct AivDirectSdmaDescriptor {
    uint64_t src, dst, bytes;
};
static_assert(sizeof(AivDirectSdmaDescriptor) == 24, "descriptor ABI");

static_assert(offsetof(StarsQueueInfo, sq_base) == 8, "STARS SQ base offset");
static_assert(offsetof(StarsQueueInfo, sq_reg_base) == 16, "STARS doorbell offset");
static_assert(offsetof(stars_sdma_sqe_t, length) == 28, "A3 length offset");
