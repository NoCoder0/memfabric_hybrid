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
#include <new>
#include <array>
#include <algorithm>
#include <utility>

#include "hybm_logger.h"
#include "hybm_data_op_factory.h"
#include "dl_acl_api.h"
#include "mf_env_define.h"
#include "mf_env_util.h"
#include "hybm_compose_data_op.h"
#include "hybm_mem_segment.h"
#include "compose_transport_manager.h"

namespace ock {
namespace mf {
HostComposeDataOp::HostComposeDataOp(hybm_options options, transport::TransManagerPtr tm,
                                     HybmEntityTagInfoPtr tag) noexcept
    : options_{std::move(options)}, transport_{std::move(tm)}, entityTagInfo_{std::move(tag)}
{}

HostComposeDataOp::~HostComposeDataOp() noexcept {}

Result HostComposeDataOp::Initialize() noexcept
{
    // AI_CORE驱动不走这里的dataOperator
    if (options_.bmType == HYBM_TYPE_AI_CORE_INITIATE) {
        return BM_OK;
    }

    for (auto &dataOperator : dataOperators_) {
        dataOperator = nullptr;
    }

    const auto composeTransportManager = std::dynamic_pointer_cast<transport::ComposeTransportManager>(transport_);
    const auto getTransport = [&composeTransportManager](uint32_t opTypes) {
        return composeTransportManager != nullptr ? composeTransportManager->GetTransport(opTypes) : nullptr;
    };

    if (options_.bmDataOpType & HYBM_DOP_TYPE_SDMA) {
        sdmaDataOperator_ = DataOperatorFactory::CreateSdmaDataOperator();
        auto ret = sdmaDataOperator_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("SDMA data operator init failed, ret: " << ret << " rankId: " << options_.rankId
                                                                 << " bmDataOpType: " << options_.bmDataOpType);
            sdmaDataOperator_ = nullptr;
            return ret;
        }
    }

    // Device data operator: DEVICE_RDMA 用 DataOpDeviceRDMA（老 native RDMA 与新 HCOMM 共用，
    // 传输无关，经 TransportManager 基类虚接口）；URMA 走 develop 原生 DataOpDeviceURMA。
    if (options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) {
        auto deviceTransportManager = getTransport(HYBM_DOP_TYPE_DEVICE_RDMA);
        if (transport_ != nullptr && deviceTransportManager == nullptr) {
            BM_LOG_ERROR("Device RDMA transport not found, rankId:" << options_.rankId
                                                                    << ", bmDataOpType:" << options_.bmDataOpType);
            return BM_ERROR;
        }
        devRdmaDataOperator_ = DataOperatorFactory::CreateDevRdmaDataOperator(options_.rankId, deviceTransportManager);
        auto ret = devRdmaDataOperator_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("Device data operator init failed, ret:" << ret);
            sdmaDataOperator_ = nullptr;
            devRdmaDataOperator_ = nullptr;
            return ret;
        }
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_URMA) {
        auto deviceTransportManager = getTransport(HYBM_DOP_TYPE_DEVICE_URMA);
        if (transport_ != nullptr && deviceTransportManager == nullptr) {
            BM_LOG_ERROR("Device URMA transport not found, rankId:" << options_.rankId
                                                                    << ", bmDataOpType:" << options_.bmDataOpType);
            return BM_ERROR;
        }
        devUrmaDataOperator_ = DataOperatorFactory::CreateDevUrmaDataOperator(options_.rankId, deviceTransportManager);
        auto ret = devUrmaDataOperator_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("Device URMA data operator init failed, ret:" << ret);
            sdmaDataOperator_ = nullptr;
            devRdmaDataOperator_ = nullptr;
            devUrmaDataOperator_ = nullptr;
            return ret;
        }
    }

    if (options_.bmDataOpType & (HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA | HYBM_DOP_TYPE_HOST_TCP)) {
        const uint32_t hostProtocol = HYBM_DOP_TYPE_HOST_TCP | HYBM_DOP_TYPE_HOST_RDMA | HYBM_DOP_TYPE_HOST_URMA;
        auto hostTransportManager = getTransport(hostProtocol);
        if (transport_ != nullptr && hostTransportManager == nullptr) {
            BM_LOG_ERROR("Host transport not found, rankId:" << options_.rankId
                                                             << ", bmDataOpType:" << options_.bmDataOpType);
            return BM_ERROR;
        }
        hostRdmaDataOperator_ = DataOperatorFactory::CreateHostRdmaDataOperator(options_.rankId, hostTransportManager);
        auto ret = hostRdmaDataOperator_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("Host RDMA data operator init failed, ret:" << ret);
            sdmaDataOperator_ = nullptr;
            devRdmaDataOperator_ = nullptr;
            hostRdmaDataOperator_ = nullptr;
            return ret;
        }
    }

    if (options_.bmDataOpType & HYBM_DOP_TYPE_HOST_SHM) {
        hostShmDataOperator_ = DataOperatorFactory::CreateHostShmDataOperator(options_.rankId);
        auto ret = hostShmDataOperator_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("Host shm data operator init failed, ret:" << ret);
            sdmaDataOperator_ = nullptr;
            devRdmaDataOperator_ = nullptr;
            hostRdmaDataOperator_ = nullptr;
            hostShmDataOperator_ = nullptr;
            return ret;
        }
    }

    dataOperators_[HYBM_DOP_TYPE_SDMA] = sdmaDataOperator_;
    dataOperators_[HYBM_DOP_TYPE_DEVICE_RDMA] = devRdmaDataOperator_;
    dataOperators_[HYBM_DOP_TYPE_DEVICE_URMA] = devUrmaDataOperator_;
    dataOperators_[HYBM_DOP_TYPE_HOST_RDMA] = hostRdmaDataOperator_;
    dataOperators_[HYBM_DOP_TYPE_HOST_TCP] = hostRdmaDataOperator_;
    dataOperators_[HYBM_DOP_TYPE_HOST_URMA] = hostRdmaDataOperator_;
    dataOperators_[HYBM_DOP_TYPE_HOST_SHM] = hostShmDataOperator_;

    return BM_OK;
}

void HostComposeDataOp::UnInitialize() noexcept
{
    if (hostRdmaDataOperator_ != nullptr) {
        hostRdmaDataOperator_->UnInitialize();
        hostRdmaDataOperator_ = nullptr;
    }
    if (hostShmDataOperator_ != nullptr) {
        hostShmDataOperator_->UnInitialize();
        hostShmDataOperator_ = nullptr;
    }
    if (devRdmaDataOperator_ != nullptr) {
        devRdmaDataOperator_->UnInitialize();
        devRdmaDataOperator_ = nullptr;
    }
    if (devUrmaDataOperator_ != nullptr) {
        devUrmaDataOperator_->UnInitialize();
        devUrmaDataOperator_ = nullptr;
    }
    if (sdmaDataOperator_ != nullptr) {
        sdmaDataOperator_->UnInitialize();
        sdmaDataOperator_ = nullptr;
    }

    for (auto &dataOperator : dataOperators_) {
        dataOperator = nullptr;
    }
}

Result HostComposeDataOp::DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                                   const ExtOptions &options) noexcept
{
    auto availableOps = GetPrioritedDataOperators(options);
    if (availableOps.empty()) {
        BM_LOG_ERROR("data copy from rank " << options.srcRankId << " to rank " << options.destRankId
                                            << " no data operator available");
        return BM_INVALID_PARAM;
    }

    Result result = BM_ERROR;
    for (auto &ops : availableOps) {
        BM_LOG_DEBUG("try data copy from rank " << options.srcRankId << " to rank " << options.destRankId
                                                << " with data op " << ops.first << " direction:" << direction);
        hybm_copy_params param2 = params;
        result = ops.second->DataCopy(param2, direction, options);
        if (result == BM_OK || result == BM_GVA_NOT_FOUND) {
            break;
        }

        BM_LOG_WARN("data copy from rank " << options.srcRankId << " to rank " << options.destRankId << " with data op "
                                           << ops.first << " failed " << result);
    }

    if (result != BM_OK) {
        BM_LOG_ERROR("data copy from rank " << options.srcRankId << " to rank " << options.destRankId << " failed "
                                            << result);
    }
    return result;
}

Result HostComposeDataOp::BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                        const ExtOptions &options) noexcept
{
    if (options.groupMap.empty()) {
        BM_LOG_ERROR("multi-rank batch copy has empty groupMap, batchSize: " << params.batchSize);
        return BM_INVALID_PARAM;
    }

    if (AllSupportSdma(options)) {
        return sdmaDataOperator_->BatchDataCopy(params, direction, options);
    }
    if (AllPreferDeviceUrma(options)) {
        return devUrmaDataOperator_->BatchDataCopy(params, direction, options);
    }

    ExtOptions urmaOptions{options.srcRankId, options.destRankId, options.stream, options.flags, {}};
    ExtOptions otherOptions{options.srcRankId, options.destRankId, options.stream, options.flags, {}};
    try {
        for (const auto &[p2pInfo, indices] : options.groupMap) {
            ExtOptions copyOptions{p2pInfo.first, p2pInfo.second, options.stream, options.flags, {}};
            auto availableOps = GetPrioritedDataOperators(copyOptions);
            if (availableOps.empty()) {
                BM_LOG_ERROR("batch data copy from rank " << p2pInfo.first << " to rank " << p2pInfo.second
                                                          << " no data operator available");
                return BM_INVALID_PARAM;
            }
            auto &targetGroupMap =
                availableOps.front().second == devUrmaDataOperator_ ? urmaOptions.groupMap : otherOptions.groupMap;
            targetGroupMap.emplace(p2pInfo, indices);
        }
    } catch (const std::bad_alloc &) {
        BM_LOG_ERROR("batch data copy split groupMap allocation failed, batchSize: "
                     << params.batchSize << " groupNum: " << options.groupMap.size());
        return BM_MALLOC_FAILED;
    }

    if (!urmaOptions.groupMap.empty()) {
        auto ret = devUrmaDataOperator_->BatchDataCopy(params, direction, urmaOptions);
        if (ret != BM_OK) {
            BM_LOG_ERROR("multi-rank batch copy failed, ret: " << ret << " rankNum: " << urmaOptions.groupMap.size());
            return ret;
        }
    }
    return otherOptions.groupMap.empty() ? BM_OK : BatchDataCopyByGroup(params, direction, otherOptions);
}

//原始的单rank BatchDataCopy
Result HostComposeDataOp::BatchDataCopyByGroup(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                               const ExtOptions &options) noexcept
{
    for (const auto &[p2pInfo, indices] : options.groupMap) {
        ExtOptions copyOptions{};
        copyOptions.srcRankId = p2pInfo.first;
        copyOptions.destRankId = p2pInfo.second;
        copyOptions.stream = options.stream;
        copyOptions.flags = options.flags;
        auto availableOps = GetPrioritedDataOperators(copyOptions);
        if (availableOps.empty()) {
            BM_LOG_ERROR("batch data copy from rank " << p2pInfo.first << " to rank " << p2pInfo.second
                                                      << " no data operator available");
            return BM_INVALID_PARAM;
        }

        const uint32_t groupSize = static_cast<uint32_t>(indices.size());
        std::vector<void *> sources(groupSize);
        std::vector<void *> destinations(groupSize);
        std::vector<size_t> dataSizes(groupSize);
        for (uint32_t i = 0; i < groupSize; ++i) {
            const uint32_t index = indices[i];
            sources[i] = params.sources[index];
            destinations[i] = params.destinations[index];
            dataSizes[i] = params.dataSizes[index];
        }
        hybm_batch_copy_params copyParams{sources.data(), destinations.data(), dataSizes.data(), groupSize};
        Result ret = BM_ERROR;
        for (auto &ops : availableOps) {
            ret = ops.second->BatchDataCopy(copyParams, direction, copyOptions);
            if (ret == BM_OK) {
                break;
            }
            BM_LOG_WARN("try batch data operator failed, srcRank:" << p2pInfo.first << " destRank:" << p2pInfo.second
                                                                   << " opType:" << ops.first
                                                                   << " direction:" << direction << " ret:" << ret);
        }
        if (ret != BM_OK) {
            BM_LOG_ERROR("data batch copy failed, ret: " << ret << " srcRank: " << p2pInfo.first
                                                         << " destRank: " << p2pInfo.second);
            return ret;
        }
    }
    return BM_OK;
}

Result HostComposeDataOp::DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                                        const ExtOptions &options) noexcept
{
    auto availableOps = GetPrioritedDataOperators(options);
    if (availableOps.empty()) {
        BM_LOG_ERROR("data copy async from rank " << options.srcRankId << " to rank " << options.destRankId
                                                  << " no data operator available");
        return BM_INVALID_PARAM;
    }

    Result result = BM_ERROR;
    for (auto &ops : availableOps) {
        BM_LOG_DEBUG("try data copy async from rank " << options.srcRankId << " to rank " << options.destRankId
                                                      << " with data op " << ops.first << " direction:" << direction);
        result = ops.second->DataCopyAsync(params, direction, options);
        if (result == BM_OK || result == BM_GVA_NOT_FOUND) {
            break;
        }

        BM_LOG_ERROR("data copy async from rank " << options.srcRankId << " to rank " << options.destRankId
                                                  << " with data op " << ops.first << " failed " << result);
    }
    return result;
}

Result HostComposeDataOp::Wait(int32_t waitId) noexcept
{
    /*
     * Note: Currently, only SDMA supports asynchronous operations; we only perform the wait for the SDMA Data Operator.
     * Subsequent consideration involves using the 3 bits in the wait ID to indicate which data operator is being used.
     */
    if (sdmaDataOperator_ != nullptr) {
        return sdmaDataOperator_->Wait(waitId);
    }
    // For non-SDMA paths (e.g. device_rdma via HCOMM), all operations are
    // synchronous (WriteRemote includes Synchronize → AclrtSynchronizeStream).
    // Nothing to wait for, return success.
    return BM_OK;
}

bool HostComposeDataOp::AllSupportSdma(const ExtOptions &options) noexcept
{
    if (sdmaDataOperator_ == nullptr) {
        return false;
    }

    for (auto &[p2pInfo, indices] : options.groupMap) {
        auto opTypes = entityTagInfo_->GetRank2RankOpType(p2pInfo.first, p2pInfo.second);
        if (!(opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA))) {
            return false;
        }
    }
    return true;
}

bool HostComposeDataOp::AllPreferDeviceUrma(const ExtOptions &options) noexcept
{
    if (devUrmaDataOperator_ == nullptr) {
        return false;
    }

    for (const auto &[p2pInfo, indices] : options.groupMap) {
        (void)indices;
        ExtOptions copyOptions{p2pInfo.first, p2pInfo.second, options.stream, options.flags, {}};
        auto availableOps = GetPrioritedDataOperators(copyOptions);
        if (availableOps.empty() || availableOps.front().second != devUrmaDataOperator_) {
            return false;
        }
    }
    return true;
}

HostComposeDataOp::DataOperators HostComposeDataOp::GetPrioritedDataOperators(const ExtOptions &options) noexcept
{
    HostComposeDataOp::DataOperators dataOperators;

    if (options.destRankId == options_.rankId || options.srcRankId == options_.rankId) {
        const uint32_t remoteRankId = options.destRankId == options_.rankId ? options.srcRankId : options.destRankId;
        hybm_data_op_type protocols[EXPORT_INFO_MAX_LAYER_COUNT] = {};
        {
            std::shared_lock<std::shared_mutex> lock(rankProtocolsMutex_);
            if (remoteRankId < RANK_MAX) {
                for (size_t layer = 0; layer < EXPORT_INFO_MAX_LAYER_COUNT; ++layer) {
                    protocols[layer] = rankProtocols_[remoteRankId][layer];
                }
            }
        }

        if (protocols[0] != HYBM_DOP_TYPE_DEFAULT) {
            for (auto opType : protocols) {
                if (opType == HYBM_DOP_TYPE_DEFAULT) {
                    break;
                }
                auto dataOperator = FindOperatorByType(opType);
                if (dataOperator != nullptr) {
                    dataOperators.emplace_back(opType, std::move(dataOperator));
                }
            }
            return dataOperators;
        }
    }

    auto opTypes = entityTagInfo_->GetRank2RankOpType(options.srcRankId, options.destRankId);
    if (sdmaDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_SDMA)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_SDMA, sdmaDataOperator_);
    }

    if (devRdmaDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_DEVICE_RDMA)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_DEVICE_RDMA, devRdmaDataOperator_);
    }
    if (devUrmaDataOperator_ != nullptr) {
        if ((opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_DEVICE_URMA)) != 0U) {
            dataOperators.emplace_back(HYBM_DOP_TYPE_DEVICE_URMA, devUrmaDataOperator_);
        }
    }

    if (hostRdmaDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_RDMA)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_HOST_RDMA, hostRdmaDataOperator_);
    }

    if (hostRdmaDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_URMA)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_HOST_URMA, hostRdmaDataOperator_);
    }

    if (hostRdmaDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_TCP)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_HOST_TCP, hostRdmaDataOperator_);
    }

    if (hostShmDataOperator_ != nullptr && (opTypes & static_cast<uint32_t>(HYBM_DOP_TYPE_HOST_SHM)) != 0U) {
        dataOperators.emplace_back(HYBM_DOP_TYPE_HOST_SHM, hostShmDataOperator_);
    }
    return dataOperators;
}

void HostComposeDataOp::CacheRankProtocols(uint32_t rankId, const std::vector<uint32_t> &protocols)
{
    hybm_data_op_type orderedProtocols[EXPORT_INFO_MAX_LAYER_COUNT] = {};

    const auto isConnected = [&protocols](hybm_data_op_type opType) {
        const uint32_t type = static_cast<uint32_t>(opType);
        return std::any_of(protocols.begin(), protocols.end(),
                           [type](uint32_t protocol) { return (protocol & type) != 0U; });
    };

    const auto isLocalProtocol = [](hybm_data_op_type opType) {
        return opType == HYBM_DOP_TYPE_SDMA || opType == HYBM_DOP_TYPE_HOST_SHM;
    };

    const auto appendProtocol = [&](hybm_data_op_type opType) {
        if (!isLocalProtocol(opType) && !isConnected(opType)) {
            return;
        }
        for (auto &slot : orderedProtocols) {
            if (slot == opType) {
                return;
            }
            if (slot == HYBM_DOP_TYPE_DEFAULT) {
                slot = opType;
                return;
            }
        }
        BM_LOG_ERROR("Too many rank protocols, rankId:" << rankId << ", maxLayerCount:" << EXPORT_INFO_MAX_LAYER_COUNT);
    };

    const auto layers = entityTagInfo_->GetRank2RankLayers(options_.rankId, rankId);
    if (!layers.empty()) {
        for (const auto layer : layers) {
            appendProtocol(layer);
        }
    } else {
        static constexpr std::array<hybm_data_op_type, 7> kPrioritedOpTypes = {
            HYBM_DOP_TYPE_SDMA,      HYBM_DOP_TYPE_DEVICE_RDMA, HYBM_DOP_TYPE_DEVICE_URMA, HYBM_DOP_TYPE_HOST_RDMA,
            HYBM_DOP_TYPE_HOST_URMA, HYBM_DOP_TYPE_HOST_TCP,    HYBM_DOP_TYPE_HOST_SHM};
        const uint32_t opTypes = entityTagInfo_->GetRank2RankOpType(options_.rankId, rankId);
        for (auto opType : kPrioritedOpTypes) {
            if ((opTypes & static_cast<uint32_t>(opType)) != 0U) {
                appendProtocol(opType);
            }
        }
    }

    if (rankId >= RANK_MAX) {
        BM_LOG_ERROR("Invalid rank protocol cache rankId:" << rankId << ", maxRankId:" << RANK_MAX);
        return;
    }

    std::unique_lock<std::shared_mutex> lock(rankProtocolsMutex_);
    for (size_t layer = 0; layer < EXPORT_INFO_MAX_LAYER_COUNT; ++layer) {
        rankProtocols_[rankId][layer] = orderedProtocols[layer];
    }
}

DataOperatorPtr HostComposeDataOp::FindOperatorByType(hybm_data_op_type opType) const noexcept
{
    if (opType <= HYBM_DOP_TYPE_DEFAULT || opType >= HYBM_DOP_TYPE_BUTT) {
        return nullptr;
    }
    return dataOperators_[opType];
}
} // namespace mf
} // namespace ock
