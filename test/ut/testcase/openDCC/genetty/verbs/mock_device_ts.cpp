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
#include "mock_device_ts.h"
#include "under_api/hal/genetty_hal_types.h"
#include <cstring>
using namespace ock::openDCC::verbs;
namespace {
constexpr uint32_t MOCK_DEPTH = 2048;
constexpr uint32_t MOCK_STREAM = 7;
constexpr uint32_t MOCK_SQ = 11;
constexpr uint32_t MOCK_CQ = 13;
constexpr uint32_t MOCK_LOGIC_CQ = 17;
int live = 0;
int sends = 0;
uint32_t tail = 0;
uint32_t head = 0;
uint64_t doorbell = 0;
bool stalled = false;
bool failFree = false;
} // namespace
extern "C" {
void MockTsReset()
{
    live = 0;
    sends = 0;
    tail = 0;
    head = 0;
    doorbell = 0;
    stalled = false;
    failFree = false;
}
void MockTsStall(bool value)
{
    stalled = value;
}
void MockTsFailFree(bool value)
{
    failFree = value;
}
int MockTsLiveResources()
{
    return live;
}
uint64_t MockTsLastDoorbell()
{
    return doorbell;
}
int MockTsSendCount()
{
    return sends;
}
int rtGetDeviceInfo(uint32_t, int32_t, int32_t, int64_t *value)
{
    *value = 0;
    return 0;
}
int halResourceIdAlloc(uint32_t, halResourceIdInputInfo *, halResourceIdOutputInfo *output)
{
    output->resourceId = MOCK_STREAM;
    ++live;
    return 0;
}
int halResourceIdFree(uint32_t, halResourceIdInputInfo *)
{
    --live;
    return 0;
}
int halResourceConfig(uint32_t, halResourceIdInputInfo *, halResourceConfigInfo *)
{
    return 0;
}
int halSqCqAllocate(uint32_t, halSqCqInputInfo *input, halSqCqOutputInfo *output)
{
    output->sqId = MOCK_SQ;
    output->cqId = input->type == DRV_NORMAL_TYPE ? MOCK_CQ : MOCK_LOGIC_CQ;
    ++live;
    return 0;
}
int halSqCqFree(uint32_t, halSqCqFreeInfo *)
{
    if (failFree) {
        return -1;
    }
    --live;
    return 0;
}
int halSqCqQuery(uint32_t, halSqCqQueryInfo *info)
{
    if (info->prop == DRV_SQCQ_PROP_SQ_DEPTH) {
        info->value[0] = MOCK_DEPTH;
    } else if (info->prop == DRV_SQCQ_PROP_SQ_TAIL) {
        info->value[0] = tail;
    } else if (info->prop == DRV_SQCQ_PROP_SQ_HEAD) {
        info->value[0] = head;
        /* The first query sees the old head, as on asynchronous TS hardware. */
        if (!stalled) {
            head = tail;
        }
    } else {
        return -1;
    }
    return 0;
}
int halSqTaskSend(uint32_t, halTaskSendInfo *info)
{
    if (info->sqe_num != 1 || info->sqId != MOCK_SQ) {
        return -1;
    }
    rtStarsSqe_t sqe{};
    memcpy(&sqe, info->sqe_addr, sizeof(sqe));
    if (sqe.writeValueSqe.header.type != 8 || sqe.writeValueSqe.header.rt_stream_id != MOCK_STREAM) {
        return -1;
    }
    doorbell = sqe.writeValueSqe.write_value_part0 | (static_cast<uint64_t>(sqe.writeValueSqe.write_value_part1) << 32);
    tail = (tail + 1) % MOCK_DEPTH;
    ++sends;
    return 0;
}
}
