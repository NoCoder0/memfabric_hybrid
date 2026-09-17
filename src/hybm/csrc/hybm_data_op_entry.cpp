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
#include <type_traits>
#include <iomanip>
#include "hybm_logger.h"
#include "mf_num_util.h"
#include "hybm_entity_factory.h"
#include "hybm_data_op.h"
#include "hybm_va_manager.h"

using namespace ock::mf;

HYBM_API int32_t hybm_data_copy(hybm_entity_t e, hybm_copy_params *params, hybm_data_copy_direction direction,
                                void *stream, uint32_t flags)
{
    BM_ASSERT_LOG_AND_RETURN(e != nullptr, "e is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params != nullptr, "params is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->src != nullptr, "params->src is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->dest != nullptr, "params->dest is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->dataSize != 0, "params->dataSize = " << params->dataSize, BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(direction < HYBM_DATA_COPY_DIRECTION_BUTT, "direction = " << direction, BM_INVALID_PARAM);
    BM_LOG_DEBUG("Src: " << VaToInfo(params->src) << ", dest: " << VaToInfo(params->dest) << " flag:" << VaToStr(flags)
                         << " direction:" << direction);

    auto &vaMgr = ock::mf::HybmVaManager::GetInstance();
    uint8_t srcMask = vaMgr.ClassifyAddressMask(reinterpret_cast<uint64_t>(params->src));
    uint8_t dstMask = vaMgr.ClassifyAddressMask(reinterpret_cast<uint64_t>(params->dest));
    uint8_t except = srcMask | (dstMask << 4);

    if (direction == HYBM_DATA_COPY_DIRECTION_AUTO) {
        direction = static_cast<hybm_data_copy_direction>(HybmVaManager::directionLut[except]);
        if (direction >= HYBM_DATA_COPY_DIRECTION_AUTO) {
            BM_LOG_ERROR("Failed to auto infer copy direction, src:" << std::hex << params->src
                                                                     << ", dest:" << params->dest);
            return BM_INVALID_PARAM;
        }
    } else if ((HybmVaManager::dirMask[direction] & except) != HybmVaManager::dirMask[direction]) {
        BM_LOG_ERROR("Direction mismatch: specified=" << static_cast<int>(direction)
                                                      << " except:" << static_cast<int>(except)
                                                      << " src=" << params->src << " dest=" << params->dest);
        return BM_INVALID_PARAM;
    }

    auto entity = MemEntityFactory::Instance().FindEngineByPtr(e);
    BM_ASSERT_LOG_AND_RETURN(entity != nullptr, "entity is nullptr", BM_INVALID_PARAM);

    return entity->CopyData(*params, direction, stream, flags);
}

HYBM_API int32_t hybm_wait(hybm_entity_t e)
{
    if (e == nullptr) {
        BM_LOG_ERROR("input parameter invalid, e: 0x" << std::hex << e);
        return BM_INVALID_PARAM;
    }
    auto entity = MemEntityFactory::Instance().FindEngineByPtr(e);
    BM_ASSERT_LOG_AND_RETURN(entity != nullptr, "entity is nullptr", BM_INVALID_PARAM);
    return entity->Wait();
}

static int32_t BatchCopyByAutoGroup(MemEntity *entity, const hybm_batch_copy_params *params, void *stream,
                                    uint32_t flags)
{
    auto &vaMgr = ock::mf::HybmVaManager::GetInstance();
    /* 快路径（仅当这一批恰好同段时生效，属于"廉价特例"）：段 + 段内 memType/imported 决定地址类型
       掩码、掩码决定方向，因此若后续 iov 都还落在首 iov 的那两个段内，整批可沿用同一方向 ——
       免掉逐 iov 两次 ClassifyAddressMask（各含 shared_lock + 红黑树查询，600 iov 约 1200 次，
       实测约 50us），也免掉 map 分组与 3 个 batchSize 元 vector 的拷贝。

       注意：真实场景一批 IO 未必同段、甚至方向都不同（smem_bm 最外层按方向分组正是为此），
       所以这里**不是假设**，而是"命中则快、不命中则原样退回"：任一 iov 越出这两个段就整体走下面的
       原分组路径，行为与改动前完全一致。多段/多方向批次目前仍要逐 iov 分类（后续可按
       alloc 段 / reserved 段 / HBM 段三级范围缓存来降低，见 TODO）。 */
    if (params->batchSize != 0 && params->sources[0] != nullptr && params->destinations[0] != nullptr) {
        const uint64_t firstSrc = reinterpret_cast<uint64_t>(params->sources[0]);
        const uint64_t firstDst = reinterpret_cast<uint64_t>(params->destinations[0]);
        auto [srcInfo, srcFound] = vaMgr.FindAllocByVa(firstSrc, HVM_GVA);
        auto [dstInfo, dstFound] = vaMgr.FindAllocByVa(firstDst, HVM_GVA);
        if (srcFound && dstFound) {
            const uint64_t srcBegin = srcInfo.base.va[HVM_GVA];
            const uint64_t srcEnd = srcBegin + srcInfo.base.size;
            const uint64_t dstBegin = dstInfo.base.va[HVM_GVA];
            const uint64_t dstEnd = dstBegin + dstInfo.base.size;
            bool sameSegments = true;
            for (uint32_t i = 1; i < params->batchSize; ++i) {
                auto src = reinterpret_cast<uint64_t>(params->sources[i]);
                auto dst = reinterpret_cast<uint64_t>(params->destinations[i]);
                if (src < srcBegin || src >= srcEnd || dst < dstBegin || dst >= dstEnd) {
                    sameSegments = false;
                    break;
                }
            }
            if (sameSegments) {
                uint8_t srcMask = vaMgr.ClassifyAddressMask(firstSrc);
                uint8_t dstMask = vaMgr.ClassifyAddressMask(firstDst);
                auto dir = static_cast<hybm_data_copy_direction>(HybmVaManager::directionLut[srcMask | (dstMask << 4)]);
                if (dir < HYBM_DATA_COPY_DIRECTION_AUTO) {
                    /* 单方向：原始数组本身已按序构成整组，直接透传（progress 参数原样转发） */
                    hybm_batch_copy_params directParams = *params;
                    return entity->BatchCopyData(directParams, dir, stream, flags);
                }
            }
        }
    }

    std::map<hybm_data_copy_direction, std::vector<uint32_t>> groups;
    for (uint32_t i = 0; i < params->batchSize; i++) {
        if (params->sources[i] == nullptr || params->destinations[i] == nullptr) {
            BM_LOG_ERROR("input copy address is invalid, source or dest is nullptr, index:" << i);
            return BM_INVALID_PARAM;
        }
        uint8_t srcMask = vaMgr.ClassifyAddressMask(reinterpret_cast<uint64_t>(params->sources[i]));
        uint8_t dstMask = vaMgr.ClassifyAddressMask(reinterpret_cast<uint64_t>(params->destinations[i]));
        uint8_t except = srcMask | (dstMask << 4);
        auto dir = static_cast<hybm_data_copy_direction>(HybmVaManager::directionLut[except]);
        if (dir >= HYBM_DATA_COPY_DIRECTION_AUTO) {
            BM_LOG_ERROR("failed to auto infer copy direction, index: "
                         << i << ", src: " << std::hex << params->sources[i] << ", dest: " << params->destinations[i]);
            return BM_INVALID_PARAM;
        }
        groups[dir].push_back(i);
    }
    // Progress notification follows the original element order, so it is only forwarded when all
    // elements fall into a single direction group (the supported usage).
    const bool forwardProgress = (groups.size() == 1U) && (params->progressInterval != 0U);
    if (!forwardProgress && params->progressInterval != 0U) {
        BM_LOG_WARN("batch copy progress ignored, elements span " << groups.size() << " direction groups");
    }
    for (auto &[dir, indices] : groups) {
        std::vector<void *> subSrc;
        std::vector<void *> subDst;
        std::vector<uint64_t> subSizes;
        subSrc.reserve(indices.size());
        subDst.reserve(indices.size());
        subSizes.reserve(indices.size());
        for (auto idx : indices) {
            subSrc.push_back(params->sources[idx]);
            subDst.push_back(params->destinations[idx]);
            subSizes.push_back(params->dataSizes[idx]);
        }
        // Progress notification follows the original element order, so it is only forwarded when all
        // elements fall into a single direction group (the supported usage).
        const bool forwardProgress = (groups.size() == 1U) && (params->progressInterval != 0U);
        if (!forwardProgress && params->progressInterval != 0U) {
            BM_LOG_WARN("batch copy progress ignored, elements span " << groups.size() << " direction groups");
        }
        hybm_batch_copy_params subParams = {subSrc.data(),
                                            subDst.data(),
                                            subSizes.data(),
                                            static_cast<uint32_t>(indices.size()),
                                            forwardProgress ? params->progressSrc : nullptr,
                                            forwardProgress ? params->progressDest : nullptr,
                                            forwardProgress ? params->progressBase : 0ULL,
                                            forwardProgress ? params->progressInterval : 0U};
        auto ret = entity->BatchCopyData(subParams, dir, stream, flags);
        if (ret != BM_OK) {
            BM_LOG_ERROR("batch copy data failed, direction: " << dir << ", batchSize: " << indices.size()
                                                               << ", ret: " << ret);
            return ret;
        }
    }
    return BM_OK;
}

HYBM_API int32_t hybm_data_batch_copy(hybm_entity_t e, hybm_batch_copy_params *params,
                                      hybm_data_copy_direction direction, void *stream, uint32_t flags)
{
    BM_ASSERT_LOG_AND_RETURN(e != nullptr, "e is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params != nullptr, "params is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->sources != nullptr, "params->sources is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->destinations != nullptr, "params->destinations is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->dataSizes != nullptr, "params->dataSizes is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->batchSize != 0, "params->batchSize = " << params->batchSize, BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(direction < HYBM_DATA_COPY_DIRECTION_BUTT, "direction = " << direction, BM_INVALID_PARAM);
    BM_LOG_DEBUG("Src[0]: " << VaToInfo(params->sources[0]) << ", dest[0]: " << VaToInfo(params->destinations[0])
                            << " flag:" << VaToStr(flags) << " direction:" << direction);

    auto entity = (MemEntity *)e;

    if (direction == HYBM_DATA_COPY_DIRECTION_AUTO) {
        return BatchCopyByAutoGroup(entity, params, stream, flags);
    }

    for (uint32_t i = 0; i < params->batchSize; i++) {
        if (params->sources[i] == nullptr || params->destinations[i] == nullptr) {
            BM_LOG_ERROR("input copy address is invalid, source or dest is nullptr, index:" << i);
            return BM_INVALID_PARAM;
        }

        auto &vaMgr = HybmVaManager::GetInstance();
        uint8_t srcMask = vaMgr.ClassifyAddressMask(reinterpret_cast<uint64_t>(params->sources[i]));
        uint8_t dstMask = vaMgr.ClassifyAddressMask(reinterpret_cast<uint64_t>(params->destinations[i]));
        uint8_t except = srcMask | (dstMask << 4);

        if ((HybmVaManager::dirMask[direction] & except) != HybmVaManager::dirMask[direction]) {
            BM_LOG_ERROR("Direction mismatch at index "
                         << i << ": dir=" << static_cast<int>(direction) << " except:" << static_cast<int>(except)
                         << " src=" << params->sources[i] << " dest=" << params->destinations[i]);
            return BM_INVALID_PARAM;
        }
    }
    return entity->BatchCopyData(*params, direction, stream, flags);
}

HYBM_API int32_t hybm_data_quant_copy(hybm_entity_t e, hybm_quant_copy_params *params)
{
    BM_ASSERT_LOG_AND_RETURN(e != nullptr, "e is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params != nullptr, "params is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->sources != nullptr, "params->sources is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->destinations != nullptr, "params->destinations is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->dataSizes != nullptr, "params->dataSizes is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->batchSize != 0, "params->batchSize = " << params->batchSize, BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->scale != nullptr, "params->scale is nullptr", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(params->offset != nullptr, "params->offset is nullptr", BM_INVALID_PARAM);
    BM_VALIDATE_RETURN(params->unitNum <= 32U * KB, "unit is " << params->unitNum << " large than 32K",
                       BM_INVALID_PARAM);

    uint32_t unitSize = params->unitNum * 2;
    for (uint32_t i = 0; i < params->batchSize; i++) {
        BM_VALIDATE_RETURN(params->dataSizes[i] % unitSize == 0,
                           "dataSize:" << params->dataSizes[i] << " is not a multiple of unitSize:" << unitSize,
                           BM_INVALID_PARAM);
    }

    auto entity = MemEntityFactory::Instance().FindEngineByPtr(e);
    BM_ASSERT_LOG_AND_RETURN(entity != nullptr, "entity is nullptr", BM_INVALID_PARAM);
    return entity->QuantCopy(*params);
}
