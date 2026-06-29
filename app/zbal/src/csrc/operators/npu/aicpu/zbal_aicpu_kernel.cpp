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

#include <cstdint>

#include "executor/zbal_aicpu_defines.h"
#include "executor/zbal_aicpu_workspace.h"
#include "executor/engine/sdma/zbal_aicpu_channel.h"
#include "executor/engine/sdma/zbal_aicpu_sdma_sqe_context.h"
#include "executor/zbal_aicpu_comm_op.h"
#include "executor/zbal_aicpu_debug.h"
#include "dl_hal_api.h"

volatile AicpuDebugHeader *g_debugBuf = nullptr;

static void InitContext(AicpuInitContext &ctx, volatile uint8_t *workspace)
{
    const volatile AicpuInitContext *initCtx =
        reinterpret_cast<const volatile AicpuInitContext *>(workspace + ZBAL_AICPU_INIT_CTX_OFFSET);

    ctx.rankId = initCtx->rankId;
    ctx.rankNum = initCtx->rankNum;
    ctx.localDeviceMemSize = initCtx->localDeviceMemSize;
    ctx.exchangeGva = initCtx->exchangeGva;
}

extern "C" uint32_t ZBALAicpuDispatcherEntry(void *args)
{
    AicpuWorkDesc *workDesc = static_cast<AicpuWorkDesc *>(args);
    volatile uint8_t *workspace = reinterpret_cast<volatile uint8_t *>(workDesc->sdmaWorkspaceGva);

    if (workspace == nullptr) {
        return 1;
    }

    /* Per-op runtime config from host (host filled via GetCommOpConfig). */
    uint32_t numCores = workDesc->numCores;
    uint32_t numChPerCore = workDesc->numChPerCore;
    /* Defensive clamps — must stay within compile-time workspace bounds. */
    if (numCores == 0 || numCores > ZBAL_AICPU_MAX_NUM_CORES) {
        numCores = ZBAL_AICPU_MAX_NUM_CORES;
    }
    if (numChPerCore == 0 || numChPerCore > ZBAL_AICPU_MAX_CH_PER_CORE) {
        numChPerCore = ZBAL_AICPU_MAX_CH_PER_CORE;
    }

    /* CoreId: atomic counter with modulo */
    uint32_t coreId = 0;
    volatile uint32_t *counter = AicpuWorkspace::CoreIdCounter(workspace);
    coreId = __atomic_fetch_add(const_cast<uint32_t *>(counter), 1, __ATOMIC_RELAXED) % numCores;
    if (coreId == 0) {
        g_debugBuf = reinterpret_cast<volatile AicpuDebugHeader *>(workspace + AicpuWorkspace::K_DEBUG_BUF_OFF);
        g_debugBuf->magic = AICPU_DEBUG_MAGIC;
        g_debugBuf->count = 0;
        g_debugBuf->seq = 0;
        g_debugBuf->reserved = 0;
    }

    AICPU_DBG(TAG_ENTRY, coreId, numCores);

    if (!DlHalApi::Load()) {
        AICPU_DBG(TAG_ENTRY, ERR_HAL_LOAD_FAILED, coreId);
        return 1;
    }

    /* Multi-channel per core from SMEM — array sized to MAX, kernel uses first numChPerCore slots. */
    volatile stars_channel_info_t *channels[ZBAL_AICPU_MAX_CH_PER_CORE];
    Channel::GetChannels(channels, coreId, numChPerCore);

    SqeLocalRingBuffer ringBufs[ZBAL_AICPU_MAX_CH_PER_CORE];
    AicpuCoreRingbufsInit(ringBufs, workspace, coreId); /* initializes all MAX channels */

    AicpuInitContext ctx;
    InitContext(ctx, workspace);
    AICPU_DBG(TAG_INIT_CTX_RANK, ctx.rankId, ctx.rankNum);
    AICPU_DBG(TAG_UPDATE_CTX, workDesc->commType, workDesc->count);
    if (workDesc->commType == ZBAL_CMD_INIT || workDesc->commType == ZBAL_CMD_FINALIZE) {
        AICPU_DBG(TAG_RETURN, workDesc->commType, 0);
        return 0;
    }

    int ret = CommOpBase::Execute(ctx, workDesc, ringBufs, channels, numChPerCore, workspace, coreId, numCores);
    AICPU_DBG(TAG_RETURN, ret, coreId);
    return (ret == 0) ? 0 : 1;
}

/* Per-op entry points for profiling visibility — same implementation, distinct kernel names */
extern "C" uint32_t ZBALAicpuAllGather(void *args)
{
    return ZBALAicpuDispatcherEntry(args);
}
extern "C" uint32_t ZBALAicpuAllReduce(void *args)
{
    return ZBALAicpuDispatcherEntry(args);
}
extern "C" uint32_t ZBALAicpuReduceScatter(void *args)
{
    return ZBALAicpuDispatcherEntry(args);
}
extern "C" uint32_t ZBALAicpuBroadcast(void *args)
{
    return ZBALAicpuDispatcherEntry(args);
}
extern "C" uint32_t ZBALAicpuScatter(void *args)
{
    return ZBALAicpuDispatcherEntry(args);
}
extern "C" uint32_t ZBALAicpuAlltoAllV(void *args)
{
    return ZBALAicpuDispatcherEntry(args);
}
extern "C" uint32_t ZBALAicpuSend(void *args)
{
    return ZBALAicpuDispatcherEntry(args);
}
extern "C" uint32_t ZBALAicpuRecv(void *args)
{
    return ZBALAicpuDispatcherEntry(args);
}
