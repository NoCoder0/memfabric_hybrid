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
#include "acc_offload.h"

#include <iomanip>
#include <limits>

#include "acc_offload_entry_manager.h"
#include "acc_offload_define.h"
#include "acc_offload_launch.h"
#include "hybm_def.h"

using namespace ock::offload;

OFFLOAD_API int32_t offload_init(const offload_config_t &config)
{
    return AccOffloadEntryManager::Instance().Initialize(config);
}

OFFLOAD_API void offload_uninit()
{
    AccOffloadEntryManager::Instance().UnInitialize();
}

OFFLOAD_API uint64_t offload_malloc(uint64_t size, uint64_t flags)
{
    (void)flags;
    auto ptr = AccOffloadEntryManager::Instance().MallocHost(size);
    if (ptr == nullptr) {
        OFFLOAD_LOG_ERROR("offload_malloc failed, size:" << size);
        return 0;
    }

    return reinterpret_cast<uint64_t>(ptr);
}

OFFLOAD_API void offload_free(uint64_t ptr, uint64_t flags)
{
    (void)flags;
    AccOffloadEntryManager::Instance().FreeHost(reinterpret_cast<void *>(ptr));
}

OFFLOAD_API int32_t offload_sparse_copy(uint64_t srcPtr, uint64_t dstPtr, uint64_t lenPtr, uint64_t sizePtr,
                                        uint16_t deviceId)
{
    auto srcPtrs = reinterpret_cast<uint64_t *>(srcPtr);
    auto dstPtrs = reinterpret_cast<uint64_t *>(dstPtr);
    auto lenPtrs = reinterpret_cast<uint32_t *>(lenPtr);
    auto sizePtr_ = reinterpret_cast<uint32_t *>(sizePtr);

    return AccOffloadEntryManager::Instance().SparseCopy(srcPtrs, dstPtrs, lenPtrs, sizePtr_, deviceId);
}

OFFLOAD_API int32_t offload_sparse_copy_urma(uint64_t srcPtrs, uint64_t dstPtrs, uint64_t lenPtrs, uint32_t listNum,
                                             uint16_t deviceId)
{
    if (srcPtrs == 0U || dstPtrs == 0U || lenPtrs == 0U || listNum == 0U ||
        deviceId == std::numeric_limits<uint16_t>::max()) {
        OFFLOAD_LOG_ERROR("invalid sparse_copy_urma arguments, src: 0x"
                          << std::hex << srcPtrs << " dst: 0x" << dstPtrs << " len: 0x" << lenPtrs << std::dec
                          << " listNum: " << listNum << " deviceId: " << deviceId);
        return BM_INVALID_PARAM;
    }

    const auto loadRet = AccOffloadLaunchApi::TryLoadLibrary();
    if (loadRet != OFFLOAD_OK) {
        OFFLOAD_LOG_ERROR("sparse_copy_urma launcher load failed, deviceId: " << deviceId << " ret: " << loadRet);
        return BM_DL_FUNCTION_FAILED;
    }

    const auto ret = AccOffloadLaunchApi::AccOffloadSparseCopyUrma(srcPtrs, dstPtrs, lenPtrs, listNum, deviceId);
    if (ret == OFFLOAD_UNLOAD) {
        OFFLOAD_LOG_ERROR("sparse_copy_urma launcher symbol is unavailable, deviceId: " << deviceId);
        return BM_NOT_INITIALIZED;
    }
    if (ret != BM_OK) {
        OFFLOAD_LOG_ERROR("sparse_copy_urma failed, deviceId: " << deviceId << " listNum: " << listNum
                                                                << " ret: " << ret);
    }
    return ret;
}

OFFLOAD_API int32_t offload_group_pack_copy(uint64_t srcPtr, uint64_t dstPtr, uint64_t lenPtr,
                                            uint64_t numLocalExpertPtr, uint64_t groupListPtr,
                                            uint64_t packedGroupListPtr, uint16_t deviceId)
{
    auto srcPtrs = reinterpret_cast<uint64_t *>(srcPtr);
    auto dstPtrs = reinterpret_cast<uint64_t *>(dstPtr);
    auto lenPtrs = reinterpret_cast<uint32_t *>(lenPtr);
    auto numLocalExpertPtrs = reinterpret_cast<uint32_t *>(numLocalExpertPtr);
    auto groupList = reinterpret_cast<int64_t *>(groupListPtr);
    auto packedGroupList = reinterpret_cast<int64_t *>(packedGroupListPtr);

    return AccOffloadEntryManager::Instance().GroupPackCopy(srcPtrs, dstPtrs, lenPtrs, numLocalExpertPtrs, groupList,
                                                            packedGroupList, deviceId);
}
