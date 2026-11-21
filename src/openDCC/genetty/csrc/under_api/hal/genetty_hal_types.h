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
#ifndef MF_GENETTY_HAL_TYPES_H
#define MF_GENETTY_HAL_TYPES_H
#include <cstdint>
namespace ock {
namespace openDCC {
namespace verbs {
constexpr uint32_t SQCQ_RTS_INFO_LENGTH = 5;
constexpr uint32_t SQCQ_RESV_LENGTH = 8;
constexpr uint32_t RESOURCEID_RESV_LENGTH = 8;
constexpr uint32_t RESOURCE_CONFIG_VALUE_LENGTH = 7;

enum DrvSqCqType {
    DRV_NORMAL_TYPE = 0,
    DRV_LOGIC_TYPE = 2,
};

enum DrvSqCqPropType {
    DRV_SQCQ_PROP_SQ_STATUS = 0x0,
    DRV_SQCQ_PROP_SQ_HEAD = 0x1,
    DRV_SQCQ_PROP_SQ_TAIL = 0x2,
    DRV_SQCQ_PROP_SQ_DISABLE_TO_ENABLE = 0x3,
    DRV_SQCQ_PROP_SQ_CQE_STATUS = 0x4,
    DRV_SQCQ_PROP_SQ_REG_BASE = 0x5,
    DRV_SQCQ_PROP_SQ_BASE = 0x6,
    DRV_SQCQ_PROP_SQ_DEPTH = 0x7,
    DRV_SQCQ_PROP_SQ_PAUSE = 0x8,
};

enum DrvIdType {
    DRV_STREAM_ID = 0,
};

struct halSqCqInputInfo {
    uint32_t type;
    uint32_t tsId;
    uint32_t sqeSize;
    uint32_t cqeSize;
    uint32_t sqeDepth;
    uint32_t cqeDepth;
    uint32_t grpId;
    uint32_t flag;
    uint32_t cqId;
    uint32_t sqId;
    uint32_t info[SQCQ_RTS_INFO_LENGTH];
    uint32_t ext_info_len;
    void *ext_info;
    uint32_t res[SQCQ_RESV_LENGTH - 3];
};

struct halSqCqOutputInfo {
    uint32_t sqId;
    uint32_t cqId;
    unsigned long long queueVAddr;
    uint32_t flag;
    uint32_t res[SQCQ_RESV_LENGTH - 3];
};

struct halSqCqFreeInfo {
    uint32_t type;
    uint32_t tsId;
    uint32_t sqId;
    uint32_t cqId;
    uint32_t flag;
    uint32_t res[SQCQ_RESV_LENGTH];
};

struct halResourceIdInputInfo {
    uint32_t type;
    uint32_t tsId;
    uint32_t resourceId;
    uint32_t res[RESOURCEID_RESV_LENGTH];
};

struct halResourceIdOutputInfo {
    uint32_t resourceId;
    uint32_t res[RESOURCEID_RESV_LENGTH];
};

struct halResourceConfigInfo {
    uint32_t prop;
    uint32_t value[RESOURCE_CONFIG_VALUE_LENGTH];
};

enum DrvResourceConfigType {
    DRV_STREAM_BIND_LOGIC_CQ = 0x0,
};

struct halTaskSendInfo {
    uint32_t type;
    uint32_t tsId;
    uint32_t sqId;
    int32_t timeout;
    uint8_t *sqe_addr;
    uint32_t sqe_num;
    uint32_t pos;
    uint32_t res[SQCQ_RESV_LENGTH];
};

struct halSqCqQueryInfo {
    uint32_t type;
    uint32_t tsId;
    uint32_t sqId;
    uint32_t cqId;
    uint32_t prop;
    uint32_t value[8];
};

struct rtStarsSqeHeader_t {
    uint8_t type : 6;
    uint8_t l1_lock : 1;
    uint8_t l1_unlock : 1;
    uint8_t ie : 2;
    uint8_t pre_p : 2;
    uint8_t post_p : 2;
    uint8_t wr_cqe : 1;
    uint8_t reserved : 1;
    uint16_t block_dim;
    uint16_t rt_stream_id;
    uint16_t task_id;
};

#pragma pack(push)
#pragma pack(1)
struct rtStarsWriteValueSqe_t {
    rtStarsSqeHeader_t header;
    uint32_t res3;
    uint32_t res4 : 16;
    uint32_t kernel_credit : 8;
    uint32_t res5 : 8;
    uint32_t write_addr_low;
    uint32_t write_addr_high : 17;
    uint32_t res6 : 3;
    uint32_t awsize : 3;
    uint32_t snoop : 1;
    uint32_t awcache : 4;
    uint32_t awprot : 3;
    uint32_t va : 1;
    uint32_t res7;
    uint32_t sub_type;
    uint32_t write_value_part0;
    uint32_t write_value_part1;
    uint32_t write_value_part2;
    uint32_t write_value_part3;
    uint32_t write_value_part4;
    uint32_t write_value_part5;
    uint32_t write_value_part6;
    uint32_t write_value_part7;
};
#pragma pack(pop)

union rtStarsSqe_t {
    rtStarsWriteValueSqe_t writeValueSqe;
};

using halSqCqAllocateFunc = int (*)(uint32_t, halSqCqInputInfo *, halSqCqOutputInfo *);
using halSqCqFreeFunc = int (*)(uint32_t, halSqCqFreeInfo *);
using halResourceIdAllocFunc = int (*)(uint32_t, halResourceIdInputInfo *, halResourceIdOutputInfo *);
using halResourceIdFreeFunc = int (*)(uint32_t, halResourceIdInputInfo *);
using halResourceConfigFunc = int (*)(uint32_t, halResourceIdInputInfo *, halResourceConfigInfo *);
using halSqTaskSendFunc = int (*)(uint32_t, halTaskSendInfo *);
using halSqCqQueryFunc = int (*)(uint32_t, halSqCqQueryInfo *);
using halSqCqConfigFunc = int (*)(uint32_t, halSqCqQueryInfo *);

} // namespace verbs
} // namespace openDCC
} // namespace ock
#endif // MF_GENETTY_HAL_TYPES_H
