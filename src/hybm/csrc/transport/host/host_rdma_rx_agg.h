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

#ifndef MF_HYBRID_HOST_RDMA_RX_AGG_H
#define MF_HYBRID_HOST_RDMA_RX_AGG_H

#include <cstdint>

namespace ock {
namespace mf {
namespace transport {
namespace host {

/*
 * host rdma 独有的"收端连续接收 + 本地 scatter"工具。
 *
 * 背景：
 *   RDMA 单边写对收端(被写方)没有任何完成事件；host rdma 批量路径(ChannelPutV/GetV)
 *   也没有"对端写完后通知本端"的能力。而"远端把 600 个 1KB 写进本端一段连续 staging、
 *   本端收到后再 scatter 到离散目标"的流程，收端必须先感知"整批已写完"。
 *
 * 本模块提供(仅 host 收端侧使用，不进入公共 TransportManager/公共头)：
 *   1) 等待"对端写完成"：轮询 staging 尾部一个 8B 完成槽(flag)，约定由对端在整批写完后
 *      单发一次 8B 单边写填入序号；
 *   2) 完成后把连续 staging 按 1KB/块 scatter 到本地离散目标(纯 host memcpy)。
 *
 * 内存地址约定：本模块只认"本端已注册内存的 host VA"(由调用方通过
 *   smem_bm_gva_to_va / hybm 地址转换得到)，不处理任何地址交换。
 */

/*
 * 收端：等待对端把整批数据写完成。
 * @param flagHostVa  完成槽的 host VA(本端内存，由对端单边写)
 * @param expectSeq   期望的完成序号(对端每轮 +1；首轮 1)
 * @param timeoutUs   超时(us)，<=0 表示无限等待
 * @return 0 成功；-1 参数非法；-2 超时
 */
int WaitWriteDone(uint64_t flagHostVa, uint64_t expectSeq, int64_t timeoutUs);

/*
 * 收端：把连续 staging scatter 到离散目标(纯本机 memcpy，不经过网络)。
 * @param stagingHostVa  连续 staging 起始 host VA；第 i 块位于 staging + i*blockSize
 * @param blockSize      单块字节数(收端按此切块，对端须按同一切法写入)
 * @param targetHostVas  目标 host VA 数组(长度 count，离散摆放)
 * @param blockCount     块数
 * @return 0 成功；-1 参数非法
 */
int ScatterContiguous(uint64_t stagingHostVa, uint64_t blockSize, const uint64_t *targetHostVas,
                      uint32_t blockCount);

} // namespace host
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HOST_RDMA_RX_AGG_H
