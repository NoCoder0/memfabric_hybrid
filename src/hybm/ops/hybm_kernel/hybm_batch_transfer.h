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

#ifndef MF_HYBM_OPS_HYBM_KERNEL_HYBM_BATCH_TRANSFER_H
#define MF_HYBM_OPS_HYBM_KERNEL_HYBM_BATCH_TRANSFER_H

#include <cstdint>
#include "dl_hcomm_api.h"

struct HybmOneSideOpParam {
    ock::mf::ThreadHandle thread;
    ock::mf::ChannelHandle channel;
    uint32_t list_num;
    void **dst_buf_addr_list;
    void **src_buf_addr_list;
    uint64_t *len_list;
    uint64_t remote_flag_addr;
    uint64_t local_flag_addr;
    uint32_t flag_size;
};

extern "C" {
int32_t HybmBatchWrite(HybmOneSideOpParam *param);
int32_t HybmBatchRead(HybmOneSideOpParam *param);
}

#endif
