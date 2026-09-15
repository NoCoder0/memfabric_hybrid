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

// ABI used by HybmBatchRead/HybmBatchWrite and non-URMA transports. Keep this layout unchanged.
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

// Multi-rank batch ABI used by HybmBatchTransfer.
struct HybmBatchTransferParam {
    // Rank 级参数
    uint32_t rank_num;                    // 本次操作涉及的远端 Rank 数量
    uint32_t *rank_id_list;               // 远端 Rank ID 列表
    uint32_t *rank_start_idx_list;        // 各 Rank 在 IO 列表中的起始下标
    uint32_t *rank_list_num_list;         // 各 Rank 对应的 IO 数量
    ock::mf::ThreadHandle *thread_list;   // 各 Rank 对应的 HCOMM 线程
    ock::mf::ChannelHandle *channel_list; // 各 Rank 对应的 HCOMM 通道

    // IO 级参数
    uint32_t total_list_num;                         // 本次操作包含的 IO 总数
    ock::mf::HcommBatchTransferDesc *transfer_descs; // Host 已完成地址转换和读写描述符构造
    // 每个 Rank 对应一个完成 marker；为空时仅下发数据。
    ock::mf::HcommBatchTransferDesc *marker_descs;

    // 根据 rank_id_list[r] 获取指定 Rank；该 Rank 的第 j 个 IO 下标为：
    // rank_start_idx_list[r] + j，其中 0 <= j < rank_list_num_list[r]
};

extern "C" {
int32_t HybmBatchWrite(HybmOneSideOpParam *param);
int32_t HybmBatchRead(HybmOneSideOpParam *param);
int32_t HybmBatchTransfer(HybmBatchTransferParam *param);
}

#endif
