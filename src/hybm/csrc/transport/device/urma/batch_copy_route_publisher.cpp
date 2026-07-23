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
#include <array>
#include <cstring>
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
            if (iter->second == owner) {
                return BM_OK;
            }
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

Result CopyHostToDevice(uint64_t destination, const void *source, size_t size, const char *operation)
{
    const auto ret =
        DlAclApi::AclrtMemcpy(reinterpret_cast<void *>(destination), size, source, size, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != BM_OK) {
        BM_LOG_ERROR(operation << " failed, ret: " << ret << " dst: 0x" << std::hex << destination << std::dec
                               << " size: " << size);
    }
    return ret;
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

Result BatchCopyRoutePublisher::ValidateSources(const std::vector<BatchCopyRouteSource> &sources) const
{
    if (sources.empty() || sources.size() > BATCH_COPY_MAX_PEER_COUNT) {
        BM_LOG_ERROR("invalid BatchCopy peer count: " << sources.size() << " userDeviceId: " << userDeviceId_);
        return BM_INVALID_PARAM;
    }
    std::array<BatchCopySourceRange, BATCH_COPY_MAX_RANGE_COUNT> ranges{};
    size_t rangeCount = 0;
    for (size_t peerIndex = 0; peerIndex < sources.size(); ++peerIndex) {
        const auto &source = sources[peerIndex];
        if (source.thread == 0 || source.channel == 0 || source.remoteFlagAddr == 0 || source.ranges.empty() ||
            source.ranges.size() > BATCH_COPY_MAX_RANGE_PER_PEER) {
            BM_LOG_ERROR("invalid BatchCopy peer source, userDeviceId: "
                         << userDeviceId_ << " peerRank: " << source.peerRank << " peerIndex: " << peerIndex
                         << " rangeCount: " << source.ranges.size());
            return BM_INVALID_PARAM;
        }
        for (size_t previous = 0; previous < peerIndex; ++previous) {
            if (sources[previous].peerRank == source.peerRank) {
                BM_LOG_ERROR("duplicate BatchCopy peer rank, userDeviceId: " << userDeviceId_
                                                                             << " peerRank: " << source.peerRank);
                return BM_INVALID_PARAM;
            }
        }
        for (const auto &range : source.ranges) {
            if (range.begin == 0 || range.begin >= range.end || rangeCount >= ranges.size()) {
                BM_LOG_ERROR("invalid BatchCopy source range, userDeviceId: "
                             << userDeviceId_ << " peerRank: " << source.peerRank << " begin: 0x" << std::hex
                             << range.begin << " end: 0x" << range.end << std::dec);
                return BM_INVALID_PARAM;
            }
            ranges[rangeCount++] = range;
        }
    }
    std::sort(ranges.begin(), ranges.begin() + rangeCount,
              [](const auto &left, const auto &right) { return left.begin < right.begin; });
    for (size_t index = 1; index < rangeCount; ++index) {
        if (ranges[index].begin < ranges[index - 1].end) {
            BM_LOG_ERROR("overlapping BatchCopy source ranges, userDeviceId: "
                         << userDeviceId_ << " begin: 0x" << std::hex << ranges[index].begin << " previousEnd: 0x"
                         << ranges[index - 1].end << std::dec);
            return BM_INVALID_PARAM;
        }
    }
    return BM_OK;
}

Result BatchCopyRoutePublisher::BuildRouteImage(const std::vector<BatchCopyRouteSource> &sources,
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
            entry.srcGvaBegin = range.begin;
            entry.srcGvaEnd = range.end;
            entry.peerIndex = static_cast<uint16_t>(peerIndex);
        }
    }
    std::sort(table.ranges, table.ranges + rangeIndex,
              [](const auto &left, const auto &right) { return left.srcGvaBegin < right.srcGvaBegin; });
    table.header.magic = 0;
    table.header.peerCount = static_cast<uint16_t>(sources.size());
    table.header.rangeCount = static_cast<uint16_t>(rangeIndex);
    return BM_OK;
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
    return CopyHostToDevice(HYBM_BATCH_COPY_META_ADDR, &magic, sizeof(magic), "clear BatchCopy route magic");
}

Result BatchCopyRoutePublisher::ClearCompletionArea()
{
    const BatchCopyCompletionArea completion{};
    return CopyHostToDevice(BATCH_COPY_COMPLETION_ADDR, &completion, sizeof(completion),
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
    return CopyHostToDevice(HYBM_BATCH_COPY_META_ADDR, &table, sizeof(table), "write BatchCopy route image");
}

Result BatchCopyRoutePublisher::PublishMagic()
{
    const uint32_t magic = BATCH_COPY_ROUTE_MAGIC;
    return CopyHostToDevice(HYBM_BATCH_COPY_META_ADDR, &magic, sizeof(magic), "publish BatchCopy route magic");
}

Result BatchCopyRoutePublisher::PublishRouteImage(const std::vector<BatchCopyRouteSource> &sources)
{
    auto ret = ClearMagic();
    if (ret != BM_OK) {
        return ret;
    }
    ret = ClearCompletionArea();
    if (ret != BM_OK) {
        return ret;
    }
    ret = RegisterCompletionArea();
    if (ret != BM_OK) {
        return ret;
    }
    BatchCopyRouteTable table{};
    ret = BuildRouteImage(sources, table);
    if (ret != BM_OK) {
        return ret;
    }
    ret = WriteRouteImage(table);
    if (ret != BM_OK) {
        return ret;
    }
    return PublishMagic();
}

void BatchCopyRoutePublisher::RollbackPublish()
{
    const auto magicRet = ClearMagic();
    if (magicRet != BM_OK) {
        BM_LOG_ERROR("rollback BatchCopy route magic failed, ret: " << magicRet << " userDeviceId: " << userDeviceId_);
    }
    const auto unregisterRet = UnregisterCompletionArea();
    if (unregisterRet != BM_OK) {
        BM_LOG_ERROR("rollback BatchCopy completion failed, ret: " << unregisterRet
                                                                   << " userDeviceId: " << userDeviceId_);
    }
    published_ = false;
    ReleaseOwner();
}

Result BatchCopyRoutePublisher::Publish(const std::vector<BatchCopyRouteSource> &sources)
{
    if (published_) {
        return BM_OK;
    }
    auto ret = ValidateSources(sources);
    if (ret != BM_OK) {
        return ret;
    }
    ret = AcquireOwner();
    if (ret != BM_OK) {
        return ret;
    }
    ret = PublishRouteImage(sources);
    if (ret != BM_OK) {
        RollbackPublish();
        return ret;
    }
    published_ = true;
    return BM_OK;
}

Result BatchCopyRoutePublisher::Clear()
{
    Result firstError = BM_OK;
    if (ownerAcquired_) {
        firstError = ClearMagic();
    }
    const auto unregisterRet = UnregisterCompletionArea();
    if (firstError == BM_OK && unregisterRet != BM_OK) {
        firstError = unregisterRet;
    }
    published_ = false;
    ReleaseOwner();
    return firstError;
}

bool BatchCopyRoutePublisher::IsPublished() const
{
    return published_;
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
