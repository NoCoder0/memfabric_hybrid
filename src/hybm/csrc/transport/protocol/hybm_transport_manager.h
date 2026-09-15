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

#ifndef MF_HYBRID_HYBM_TRANSPORT_MANAGER_H
#define MF_HYBRID_HYBM_TRANSPORT_MANAGER_H

#include <memory>
#include <set>
#include "hybm_data_operator.h"
#include "hybm_types.h"
#include "hybm_transport_common.h"
#include "hybm_entity_tag_info.h"

namespace ock {
namespace mf {
namespace transport {

using RankGroupMap = decltype(ExtOptions::groupMap);

class TransportManager {
public:
    static std::shared_ptr<TransportManager> Create(TransportType type, HybmEntityTagInfoPtr tagManager = nullptr);

public:
    TransportManager() = default;

    virtual ~TransportManager() = default;

    /*
     * 1、本地IP（NIC、Device）
     * @return 0 if successful
     */
    virtual Result OpenDevice(const TransportOptions &options) = 0;

    virtual Result CloseDevice() = 0;

    /*
     * 2、注册内存
     * @return 0 if successful
     */
    virtual Result RegisterMemoryRegion(const TransportMemoryRegion &mr) = 0;

    virtual Result UnregisterMemoryRegion(uint64_t addr) = 0;

    virtual bool QueryHasRegistered(uint64_t addr, uint64_t size) = 0;

    virtual Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) = 0;

    virtual void UpdateMemoryKey(TransportMemoryKey &key, void *addr) = 0;

    /*
     * 3、建链前的准备工作
     * @return 0 if successful
     */
    virtual Result Prepare(const HybmTransPrepareOptions &options) = 0;

    /*
     * 建链完成状态，删除一部分节点
     */
    virtual Result RemoveRanks(const std::vector<uint32_t> &removedRanks) = 0;

    /*
     * 单rank建链，用于OnEstablishConnection逐rank建链
     * @return 0 if successful
     */
    virtual Result ConnectRank(uint32_t rankId)
    {
        return BM_OK;
    }

    /*
     * 建链完成后，更新rank配置信息，可以新增rank或减少rank
     */
    virtual Result UpdateRankOptions(const HybmTransPrepareOptions &options) = 0;

    /**
     * 查询
     */
    virtual const std::string &GetNic() const = 0; // X

    virtual const TransportPrivateData GetPrivateData() const = 0;

    virtual const void *GetQpInfo() const;

    /*
     * 获取SDMA workspace地址
     * @return 0 if successful
     */
    virtual uint64_t GetSdmaWorkSpaceAddr() const;

    /**
      * rdma单边传输
      */
    virtual Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) = 0;

    virtual Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) = 0;

    virtual Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) = 0;

    virtual Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) = 0;

    virtual Result Synchronize(uint32_t rankId) = 0;

    virtual Result SynchronizeRanks(const std::set<uint32_t> &rankIds)
    {
        Result firstError = BM_OK;
        for (uint32_t rankId : rankIds) {
            const auto ret = Synchronize(rankId);
            if (ret != BM_OK && firstError == BM_OK) {
                firstError = ret;
            }
        }
        return firstError;
    }

    virtual Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) = 0;

    virtual Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) = 0;

    // batchRanks is populated with all effective remote ranks only when the slice kernel succeeds as a whole.
    virtual Result TransferRemoteBatchAsync(const hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                            const RankGroupMap &groupMap, std::vector<uint32_t> &localIndices,
                                            RankGroupMap &unregisteredGroups, std::set<uint32_t> &batchRanks);
};

using TransManagerPtr = std::shared_ptr<TransportManager>;
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_TRANSPORT_MANAGER_H
