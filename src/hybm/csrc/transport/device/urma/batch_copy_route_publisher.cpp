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

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "batch_copy_route_publisher.h"
#include "dl_acl_api.h"
#include "hybm_define.h"
#include "hybm_logger.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

namespace {
constexpr uint64_t BATCH_COPY_COMPLETION_ADDR = HYBM_BATCH_COPY_META_ADDR + BATCH_COPY_COMPLETION_OFFSET;
constexpr urma::UrmaMemTag BATCH_COPY_COMPLETION_MEM_TAG = BATCH_COPY_COMPLETION_ADDR;

class BatchCopyRouteOwnerRegistry final {
public:
    static Result Acquire(uint32_t userDeviceId, const BatchCopyRoutePublisher *owner)
    {
        std::lock_guard<std::mutex> guard(GetMutex());
        auto &owners = GetOwners();
        const auto iter = owners.find(userDeviceId);
        if (iter != owners.end()) {
            BM_LOG_ERROR("BatchCopy route owner already exists, userDeviceId: " << userDeviceId);
            return BM_BUSY;
        }
        try {
            owners.emplace(userDeviceId, owner);
        } catch (...) {
            BM_LOG_ERROR("allocate BatchCopy route owner failed, userDeviceId: " << userDeviceId);
            return BM_MALLOC_FAILED;
        }
        return BM_OK;
    }

    static void Release(uint32_t userDeviceId, const BatchCopyRoutePublisher *owner)
    {
        std::lock_guard<std::mutex> guard(GetMutex());
        auto &owners = GetOwners();
        const auto iter = owners.find(userDeviceId);
        if (iter != owners.end() && iter->second == owner) {
            owners.erase(iter);
        }
    }

private:
    static std::mutex &GetMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    static std::unordered_map<uint32_t, const BatchCopyRoutePublisher *> &GetOwners()
    {
        static std::unordered_map<uint32_t, const BatchCopyRoutePublisher *> owners;
        return owners;
    }
};

Result CopyHostToDevice(uint32_t userDeviceId, uint64_t destination, const void *source, size_t size,
                        const char *operation)
{
    const auto ret =
        DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(destination), size, source, size, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR(operation << " failed, userDeviceId: " << userDeviceId << " ret: " << ret << " dst: 0x" << std::hex
                               << destination << std::dec << " size: " << size);
    }
    return ret;
}

void LogRouteTable(uint32_t userDeviceId, const BatchCopyRouteTable &table)
{
    BM_LOG_INFO("BatchCopy route table, userDeviceId: "
                << userDeviceId << " magic: 0x" << std::hex << table.header.magic << std::dec
                << " peerCount: " << table.header.peerCount << " rangeCount: " << table.header.rangeCount);
    for (uint16_t peerIndex = 0; peerIndex < table.header.peerCount; ++peerIndex) {
        const auto &peer = table.peers[peerIndex];
        BM_LOG_INFO("BatchCopy route peer, userDeviceId: " << userDeviceId << " peerIndex: " << peerIndex
                                                           << " thread: " << peer.thread << " channel: " << peer.channel
                                                           << " remoteFlagAddr: 0x" << std::hex << peer.remoteFlagAddr
                                                           << std::dec);
    }
    for (uint16_t rangeIndex = 0; rangeIndex < table.header.rangeCount; ++rangeIndex) {
        const auto &range = table.ranges[rangeIndex];
        BM_LOG_INFO("BatchCopy route range, userDeviceId: "
                    << userDeviceId << " rangeIndex: " << rangeIndex << " peerIndex: " << range.peerIndex
                    << " srcGvaBegin: 0x" << std::hex << range.srcGvaBegin << " srcGvaEnd: 0x" << range.srcGvaEnd
                    << " hcommVaBegin: 0x" << range.hcommVaBegin << std::dec);
    }
}
} // namespace

BatchCopyRoutePublisher::BatchCopyRoutePublisher(uint32_t userDeviceId, const urma::UrmaEndpointHandle &localEndpoint,
                                                 urma::HcommTransportManager &hcommManager)
    : userDeviceId_(userDeviceId), localEndpoint_(localEndpoint), hcommManager_(hcommManager)
{}

BatchCopyRoutePublisher::~BatchCopyRoutePublisher()
{
    const auto ret = Clear();
    if (ret != BM_OK) {
        BM_LOG_ERROR("clear BatchCopy route in destructor failed, ret: " << ret << " userDeviceId: " << userDeviceId_);
    }
}

Result BatchCopyRoutePublisher::ValidatePeer(const std::vector<BatchCopyRouteSource> &sources, size_t peerIndex) const
{
    const auto &source = sources[peerIndex];
    if (source.thread == 0 || source.channel == 0 || source.remoteFlagAddr == 0 || source.ranges.empty() ||
        source.ranges.size() > BATCH_COPY_MAX_RANGE_PER_PEER) {
        BM_LOG_ERROR("invalid BatchCopy peer source, userDeviceId: "
                     << userDeviceId_ << " peerRank: " << source.peerRank << " peerIndex: " << peerIndex
                     << " thread: " << source.thread << " channel: " << source.channel << " remoteFlagAddr: 0x"
                     << std::hex << source.remoteFlagAddr << std::dec << " rangeCount: " << source.ranges.size());
        return BM_INVALID_PARAM;
    }
    for (size_t previous = 0; previous < peerIndex; ++previous) {
        if (sources[previous].peerRank == source.peerRank) {
            BM_LOG_ERROR("duplicate BatchCopy peer rank, userDeviceId: " << userDeviceId_
                                                                         << " peerRank: " << source.peerRank);
            return BM_INVALID_PARAM;
        }
    }
    return BM_OK;
}

Result BatchCopyRoutePublisher::CollectRanges(const BatchCopyRouteSource &source, SourceRangeArray &ranges,
                                              size_t &rangeCount) const
{
    for (const auto &range : source.ranges) {
        if (range.srcGvaBegin == 0 || range.srcGvaBegin >= range.srcGvaEnd || range.hcommVaBegin == 0 ||
            rangeCount >= ranges.size()) {
            BM_LOG_ERROR("invalid BatchCopy source range, userDeviceId: "
                         << userDeviceId_ << " peerRank: " << source.peerRank << " srcGvaBegin: 0x" << std::hex
                         << range.srcGvaBegin << " srcGvaEnd: 0x" << range.srcGvaEnd << " hcommVaBegin: 0x"
                         << range.hcommVaBegin << std::dec);
            return BM_INVALID_PARAM;
        }
        const auto length = range.srcGvaEnd - range.srcGvaBegin;
        if (range.hcommVaBegin > std::numeric_limits<uint64_t>::max() - length) {
            BM_LOG_ERROR("BatchCopy HCOMM range overflows, userDeviceId: "
                         << userDeviceId_ << " peerRank: " << source.peerRank << " hcommVaBegin: 0x" << std::hex
                         << range.hcommVaBegin << " length: 0x" << length << std::dec);
            return BM_INVALID_PARAM;
        }
        ranges[rangeCount++] = range;
    }
    return BM_OK;
}

Result BatchCopyRoutePublisher::ValidateSortedRanges(SourceRangeArray &ranges, size_t rangeCount) const
{
    std::sort(ranges.begin(), ranges.begin() + rangeCount,
              [](const auto &left, const auto &right) { return left.srcGvaBegin < right.srcGvaBegin; });
    for (size_t index = 1; index < rangeCount; ++index) {
        if (ranges[index].srcGvaBegin < ranges[index - 1].srcGvaEnd) {
            BM_LOG_ERROR("overlapping BatchCopy source ranges, userDeviceId: "
                         << userDeviceId_ << " srcGvaBegin: 0x" << std::hex << ranges[index].srcGvaBegin
                         << " previousEnd: 0x" << ranges[index - 1].srcGvaEnd << std::dec);
            return BM_INVALID_PARAM;
        }
    }
    return BM_OK;
}

Result BatchCopyRoutePublisher::ValidateSources(const std::vector<BatchCopyRouteSource> &sources) const
{
    if (sources.empty() || sources.size() > BATCH_COPY_MAX_PEER_COUNT) {
        BM_LOG_ERROR("invalid BatchCopy peer count: " << sources.size() << " userDeviceId: " << userDeviceId_);
        return BM_INVALID_PARAM;
    }
    SourceRangeArray ranges{};
    size_t rangeCount = 0;
    for (size_t peerIndex = 0; peerIndex < sources.size(); ++peerIndex) {
        auto ret = ValidatePeer(sources, peerIndex);
        if (ret != BM_OK) {
            return ret;
        }
        ret = CollectRanges(sources[peerIndex], ranges, rangeCount);
        if (ret != BM_OK) {
            return ret;
        }
    }
    return ValidateSortedRanges(ranges, rangeCount);
}

void BatchCopyRoutePublisher::BuildRouteImage(const std::vector<BatchCopyRouteSource> &sources,
                                              BatchCopyRouteTable &table) const
{
    std::memset(static_cast<void *>(&table), 0, sizeof(table));
    size_t rangeIndex = 0;
    for (size_t peerIndex = 0; peerIndex < sources.size(); ++peerIndex) {
        const auto &source = sources[peerIndex];
        table.peers[peerIndex].thread = source.thread;
        table.peers[peerIndex].channel = source.channel;
        table.peers[peerIndex].remoteFlagAddr = source.remoteFlagAddr;
        for (const auto &range : source.ranges) {
            auto &entry = table.ranges[rangeIndex++];
            entry.srcGvaBegin = range.srcGvaBegin;
            entry.srcGvaEnd = range.srcGvaEnd;
            entry.hcommVaBegin = range.hcommVaBegin;
            entry.peerIndex = static_cast<uint16_t>(peerIndex);
        }
    }
    std::sort(table.ranges, table.ranges + rangeIndex,
              [](const auto &left, const auto &right) { return left.srcGvaBegin < right.srcGvaBegin; });
    table.header.peerCount = static_cast<uint16_t>(sources.size());
    table.header.rangeCount = static_cast<uint16_t>(rangeIndex);
}

Result BatchCopyRoutePublisher::AcquireOwner()
{
    const auto ret = BatchCopyRouteOwnerRegistry::Acquire(userDeviceId_, this);
    if (ret == BM_OK) {
        ownerAcquired_ = true;
    }
    return ret;
}

void BatchCopyRoutePublisher::ReleaseOwner()
{
    if (!ownerAcquired_) {
        return;
    }
    BatchCopyRouteOwnerRegistry::Release(userDeviceId_, this);
    ownerAcquired_ = false;
}

Result BatchCopyRoutePublisher::ClearMagic()
{
    const uint32_t magic = 0;
    return CopyHostToDevice(userDeviceId_, HYBM_BATCH_COPY_META_ADDR, &magic, sizeof(magic),
                            "clear BatchCopy route magic");
}

Result BatchCopyRoutePublisher::ClearCompletionArea()
{
    const BatchCopyCompletionArea completion{};
    return CopyHostToDevice(userDeviceId_, BATCH_COPY_COMPLETION_ADDR, &completion, sizeof(completion),
                            "clear BatchCopy completion area");
}

Result BatchCopyRoutePublisher::RegisterCompletionArea()
{
    if (localEndpoint_ == nullptr) {
        BM_LOG_ERROR("register BatchCopy completion failed, local endpoint is null, userDeviceId: " << userDeviceId_);
        return BM_NOT_INITIALIZED;
    }
    const urma::UrmaCommMem memory{BATCH_COPY_COMPLETION_ADDR, sizeof(BatchCopyCompletionArea),
                                   urma::UrmaMemoryType::DEVICE_HBM};
    const auto ret =
        hcommManager_.HcommMemReg(localEndpoint_, BATCH_COPY_COMPLETION_MEM_TAG, memory, &completionHandle_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("register BatchCopy completion failed, ret: " << ret << " userDeviceId: " << userDeviceId_
                                                                   << " addr: 0x" << std::hex
                                                                   << BATCH_COPY_COMPLETION_ADDR << std::dec);
    }
    return ret;
}

Result BatchCopyRoutePublisher::UnregisterCompletionArea()
{
    if (completionHandle_ == nullptr) {
        return BM_OK;
    }
    const auto ret = hcommManager_.HcommMemUnreg(localEndpoint_, completionHandle_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("unregister BatchCopy completion failed, ret: " << ret << " userDeviceId: " << userDeviceId_
                                                                     << " handle: " << completionHandle_);
        return ret;
    }
    completionHandle_ = nullptr;
    return BM_OK;
}

Result BatchCopyRoutePublisher::WriteRouteImage(const BatchCopyRouteTable &table)
{
    if (table.header.magic != 0) {
        BM_LOG_ERROR("BatchCopy route image magic must be zero, userDeviceId: " << userDeviceId_
                                                                                << " magic: " << table.header.magic);
        return BM_INVALID_PARAM;
    }
    return CopyHostToDevice(userDeviceId_, HYBM_BATCH_COPY_META_ADDR, &table, sizeof(table),
                            "write BatchCopy route image");
}

Result BatchCopyRoutePublisher::PublishMagic()
{
    const uint32_t magic = BATCH_COPY_ROUTE_MAGIC;
    return CopyHostToDevice(userDeviceId_, HYBM_BATCH_COPY_META_ADDR, &magic, sizeof(magic),
                            "publish BatchCopy route magic");
}

Result BatchCopyRoutePublisher::PublishRouteImage(const std::vector<BatchCopyRouteSource> &sources)
{
    auto ret = ClearMagic();
    if (ret != BM_OK) {
        BM_LOG_ERROR("prepare BatchCopy route image failed while clearing magic, userDeviceId: " << userDeviceId_
                                                                                                 << " ret: " << ret);
        return ret;
    }
    ret = ClearCompletionArea();
    if (ret != BM_OK) {
        BM_LOG_ERROR("prepare BatchCopy route image failed while clearing completion area, userDeviceId: "
                     << userDeviceId_ << " ret: " << ret);
        return ret;
    }
    ret = RegisterCompletionArea();
    if (ret != BM_OK) {
        BM_LOG_ERROR("prepare BatchCopy route image failed while registering completion area, userDeviceId: "
                     << userDeviceId_ << " ret: " << ret);
        return ret;
    }
    BatchCopyRouteTable table{};
    BuildRouteImage(sources, table);
    ret = WriteRouteImage(table);
    if (ret != BM_OK) {
        BM_LOG_ERROR("prepare BatchCopy route image failed while writing route table, userDeviceId: "
                     << userDeviceId_ << " peerCount: " << sources.size() << " ret: " << ret);
        return ret;
    }
    LogRouteTable(userDeviceId_, table);
    ret = PublishMagic();
    if (ret != BM_OK) {
        BM_LOG_ERROR("publish BatchCopy route image failed while publishing magic, userDeviceId: "
                     << userDeviceId_ << " peerCount: " << sources.size() << " ret: " << ret);
    }
    return ret;
}

void BatchCopyRoutePublisher::RollbackPublish()
{
    published_ = false;
    const auto ret = Clear();
    if (ret != BM_OK) {
        BM_LOG_ERROR("rollback BatchCopy route failed, ret: " << ret << " userDeviceId: " << userDeviceId_);
    }
}

Result BatchCopyRoutePublisher::Publish(const std::vector<BatchCopyRouteSource> &sources)
{
    if (published_) {
        return BM_OK;
    }
    if (localEndpoint_ == nullptr) {
        BM_LOG_ERROR("publish BatchCopy route failed, local endpoint is null, userDeviceId: " << userDeviceId_);
        return BM_NOT_INITIALIZED;
    }
    if (ownerAcquired_ || completionHandle_ != nullptr) {
        BM_LOG_ERROR("BatchCopy route has pending cleanup, userDeviceId: " << userDeviceId_ << " completionHandle: "
                                                                           << completionHandle_);
        return BM_BUSY;
    }
    auto ret = ValidateSources(sources);
    if (ret != BM_OK) {
        BM_LOG_ERROR("publish BatchCopy route validation failed, userDeviceId: " << userDeviceId_ << " peerCount: "
                                                                                 << sources.size() << " ret: " << ret);
        return ret;
    }
    ret = AcquireOwner();
    if (ret != BM_OK) {
        BM_LOG_ERROR("acquire BatchCopy route owner failed, userDeviceId: " << userDeviceId_ << " peerCount: "
                                                                            << sources.size() << " ret: " << ret);
        return ret;
    }
    ret = PublishRouteImage(sources);
    if (ret != BM_OK) {
        BM_LOG_ERROR("publish BatchCopy route image failed, userDeviceId: " << userDeviceId_ << " peerCount: "
                                                                            << sources.size() << " ret: " << ret);
        RollbackPublish();
        return ret;
    }
    published_ = true;
    return BM_OK;
}

Result BatchCopyRoutePublisher::Clear()
{
    if (!ownerAcquired_) {
        published_ = false;
        if (completionHandle_ == nullptr) {
            return BM_OK;
        }
        BM_LOG_ERROR("BatchCopy completion exists without route owner, userDeviceId: "
                     << userDeviceId_ << " completionHandle: " << completionHandle_);
        return BM_ERROR;
    }
    auto ret = ClearMagic();
    if (ret != BM_OK) {
        BM_LOG_ERROR("clear BatchCopy route failed while clearing magic, userDeviceId: " << userDeviceId_
                                                                                         << " ret: " << ret);
        return ret;
    }
    published_ = false;
    ret = UnregisterCompletionArea();
    if (ret != BM_OK) {
        BM_LOG_ERROR("clear BatchCopy route failed while unregistering completion area, userDeviceId: "
                     << userDeviceId_ << " ret: " << ret);
        return ret;
    }
    ReleaseOwner();
    return BM_OK;
}

bool BatchCopyRoutePublisher::IsPublished() const
{
    return published_;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
