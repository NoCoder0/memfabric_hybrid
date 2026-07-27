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

#ifndef MF_HYBRID_BATCH_COPY_ROUTE_PUBLISHER_H
#define MF_HYBRID_BATCH_COPY_ROUTE_PUBLISHER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "hybm_batch_copy_route.h"
#include "urma/hcomm_transport_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

struct BatchCopySourceRange {
    uint64_t srcGvaBegin{0};
    uint64_t srcGvaEnd{0};
    uint64_t hcommVaBegin{0};
};

struct BatchCopyRouteSource {
    uint32_t peerRank{0};
    urma::HcommThreadHandle thread{0};
    urma::HcommChannelHandle channel{0};
    uint64_t remoteFlagAddr{0};
    std::vector<BatchCopySourceRange> ranges{};
};

class BatchCopyRoutePublisher final {
public:
    BatchCopyRoutePublisher(uint32_t userDeviceId, const urma::UrmaEndpointHandle &localEndpoint,
                            urma::HcommTransportManager &hcommManager);
    ~BatchCopyRoutePublisher();

    BatchCopyRoutePublisher(const BatchCopyRoutePublisher &) = delete;
    BatchCopyRoutePublisher &operator=(const BatchCopyRoutePublisher &) = delete;

    Result Publish(const std::vector<BatchCopyRouteSource> &sources);
    Result Clear();
    bool IsPublished() const;

private:
    using SourceRangeArray = std::array<BatchCopySourceRange, BATCH_COPY_MAX_RANGE_COUNT>;

    Result ValidateSources(const std::vector<BatchCopyRouteSource> &sources) const;
    Result ValidatePeer(const std::vector<BatchCopyRouteSource> &sources, size_t peerIndex) const;
    Result CollectRanges(const BatchCopyRouteSource &source, SourceRangeArray &ranges, size_t &rangeCount) const;
    Result ValidateSortedRanges(SourceRangeArray &ranges, size_t rangeCount) const;
    void BuildRouteImage(const std::vector<BatchCopyRouteSource> &sources, BatchCopyRouteTable &table) const;
    Result AcquireOwner();
    void ReleaseOwner();
    Result ClearMagic();
    Result ClearCompletionArea();
    Result RegisterCompletionArea();
    Result UnregisterCompletionArea();
    Result WriteRouteImage(const BatchCopyRouteTable &table);
    Result PublishMagic();
    Result PublishRouteImage(const std::vector<BatchCopyRouteSource> &sources);
    void RollbackPublish();

    uint32_t userDeviceId_{0};
    urma::UrmaEndpointHandle localEndpoint_{nullptr};
    urma::HcommTransportManager &hcommManager_;
    HcommMemHandle completionHandle_{nullptr};
    bool ownerAcquired_{false};
    bool published_{false};
};

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_BATCH_COPY_ROUTE_PUBLISHER_H
