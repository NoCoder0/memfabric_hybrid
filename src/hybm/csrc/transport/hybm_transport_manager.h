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
#include "hybm_types.h"
#include "hybm_transport_common.h"
#include "hybm_entity_tag_info.h"

namespace ock {
namespace mf {
namespace transport {

class TransportManager {
public:
    static std::shared_ptr<TransportManager> Create(TransportType type, HybmEntityTagInfoPtr tagManager = nullptr);
    static std::shared_ptr<TransportManager> Create(HybmGvaVersion version);

public:
    TransportManager() = default;

    virtual ~TransportManager() = default;

    /*
     * 1、本地IP（NIC、Device）
     * @return 0 if successful
     */
    virtual Result OpenDevice(const TransportOptions &options) = 0;

    virtual Result CloseDevice() = 0;

    virtual Result ConnectWithOptions(const HybmTransPrepareOptions &options);

    /*
     * 2、注册内存
     * @return 0 if successful
     */
    virtual Result RegisterMemoryRegion(const TransportMemoryRegion &mr) = 0;

    virtual Result UnregisterMemoryRegion(uint64_t addr) = 0;

    virtual bool QueryHasRegistered(uint64_t addr, uint64_t size) = 0;

    virtual Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) = 0;

    // Link/endpoint count per rank. Default 1 (single link); multi-link transports override.
    virtual uint32_t GetLinkCount() const
    {
        return 1;
    }

    // 所有 link(ep) 是否都就绪（已配置且已连接）。默认 true：单链路没有"多链路齐备"的概念。
    virtual bool AllLinksReady(uint32_t rankId) const
    {
        (void)rankId;
        return true;
    }

    // Query memory key registered on a specific link(ep). Default falls back to QueryMemoryKey(ep0).
    virtual Result QueryMemoryKeyByEp(uint64_t addr, uint32_t ep, TransportMemoryKey &key)
    {
        return QueryMemoryKey(addr, key);
    }

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
     * 4、建链
     * @return 0 if successful
     */
    virtual Result Connect() = 0;

    /*
     * 单rank建链，用于OnEstablishConnection逐rank建链
     * @return 0 if successful
     */
    virtual Result ConnectRank(uint32_t rankId)
    {
        return BM_OK;
    }

    /*
     * 异步建链
     * @return 0 if successful
     */
    virtual Result AsyncConnect() = 0;

    /*
     * 等待异步建链完成
     * @return 0 if successful
     */
    virtual Result WaitForConnected(int64_t timeoutNs) = 0;

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

    virtual Result Remove(const std::vector<uint32_t> &removeList);

    virtual Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) = 0;

    // 在【指定 link(ep)】上提交一段 iov（只提交、不等待完成）。多链路下"每链路一份进度水位"需要它。
    // 默认不支持，返回错误。
    virtual Result SubmitWriteBatchOnEp(uint32_t rankId, uint32_t ep, const CopyDescriptor &descriptor, size_t begin,
                                        size_t end)
    {
        (void)rankId;
        (void)ep;
        (void)descriptor;
        (void)begin;
        (void)end;
        return BM_ERROR;
    }

    // 在【指定 link(ep)】上写一小段（只提交、不等待完成）。默认回落到 ep0 的 WriteRemoteAsync。
    virtual Result WriteRemoteAsyncOnEp(uint32_t rankId, uint32_t ep, uint64_t lAddr, uint64_t rAddr, uint64_t size)
    {
        (void)ep;
        return WriteRemoteAsync(rankId, lAddr, rAddr, size);
    }

    virtual Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) = 0;

protected:
    bool connected_{false};
};

using TransManagerPtr = std::shared_ptr<TransportManager>;
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_TRANSPORT_MANAGER_H
