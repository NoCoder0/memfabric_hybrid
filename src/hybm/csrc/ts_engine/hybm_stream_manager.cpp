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

#include <thread>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include "dl_acl_api.h"
#include "hybm_logger.h"
#include "hybm_stream_manager.h"

namespace ock {
namespace mf {
static std::shared_mutex g_allThreadStreamMutex;
static std::unordered_map<int64_t, HybmStreamPtr> g_allThreadStreams;
HybmStreamPtr HybmStreamManager::GetThreadHybmStream(uint32_t devId)
{
    const auto thisId = Func::GetCurTid();
    {
        std::shared_lock lock(g_allThreadStreamMutex);
        auto it = g_allThreadStreams.find(thisId);
        if (it != g_allThreadStreams.end()) {
            return it->second;
        }
    }
    HybmStreamPtr hybmStream_ = nullptr;
    {
        std::unique_lock lock(g_allThreadStreamMutex);
        auto it = g_allThreadStreams.find(thisId);
        if (it != g_allThreadStreams.end()) {
            return it->second;
        }
        hybmStream_ = std::make_shared<HybmStream>(devId, 0, 0);
        auto ret = hybmStream_->Initialize();
        if (ret != BM_OK) {
            BM_LOG_ERROR("HybmStream init failed: " << ret);
            hybmStream_->Destroy();
            return nullptr;
        }
        g_allThreadStreams[thisId] = hybmStream_;
    }
    return hybmStream_;
}

void HybmStreamManager::DestroyAllThreadHybmStream()
{
    std::unique_lock<std::shared_mutex> lock_guard(g_allThreadStreamMutex);
    g_allThreadStreams.clear();
}

namespace {
// tid -> stream：供故障线程跨线程中止本线程的 stream。
// 独立于 device_urma 的 mutex_，避免与被阻塞在 notify 等待中的数据线程争用。
std::shared_mutex g_threadAclStreamMutex;
std::unordered_map<int64_t, void *> g_threadAclStreams;
} // namespace

// 线程退出时兜底销毁本线程 ACL stream；销毁前先从注册表摘除，避免故障线程 abort 到已销毁句柄。
class AclrtStream {
public:
    explicit AclrtStream(void *stream) : stream_(stream) {}
    ~AclrtStream()
    {
        if (stream_ == nullptr) {
            return;
        }
        HybmStreamManager::UnregisterThreadAclStream(Func::GetCurTid(), stream_);
        (void)DlAclApi::AclrtDestroyStream(stream_);
    }

private:
    void *stream_;
};

int HybmStreamManager::SetDevice(uint32_t devId)
{
    auto ret = DlAclApi::AclrtSetDevice(devId);
    if (ret != 0) {
        BM_LOG_ERROR("aclrtSetDevice failed: " << ret);
        return BM_ERROR;
    }
    return BM_OK;
}

void *HybmStreamManager::GetThreadAclStream()
{
    static thread_local void *stream_ = nullptr;
    if (stream_ != nullptr) {
        return stream_;
    }
#if defined(ASCEND_NPU)
    static uint32_t ACL_STREAM_FAST_LAUNCH = 1U;
    static uint32_t ACL_STREAM_FAST_SYNC = 2U;
    auto ret = DlAclApi::AclrtCreateStreamWithConfig(&stream_, 0, ACL_STREAM_FAST_LAUNCH | ACL_STREAM_FAST_SYNC);
    if (ret != 0) {
        BM_LOG_ERROR("create stream failed: " << ret);
        return nullptr;
    }
    {
        // 登记到全局表，供故障线程在对端退出时跨线程中止
        std::unique_lock<std::shared_mutex> lock(g_threadAclStreamMutex);
        g_threadAclStreams[Func::GetCurTid()] = stream_;
    }
#endif
    static thread_local AclrtStream aclrtStream_(stream_);
    return stream_;
}

void HybmStreamManager::AbortAllThreadAclStreams()
{
    std::vector<void *> streams;
    {
        std::shared_lock<std::shared_mutex> lock(g_threadAclStreamMutex);
        streams.reserve(g_threadAclStreams.size());
        for (const auto &item : g_threadAclStreams) {
            streams.push_back(item.second);
        }
    }
    for (auto *stream : streams) {
        if (stream == nullptr) {
            continue;
        }
        auto ret = DlAclApi::AclrtStreamAbort(stream);
        if (ret == BM_UNDER_API_UNLOAD) {
            BM_LOG_WARN("AclrtStreamAbort unavailable on this platform, skip abort, stream: " << stream);
            continue;
        }
        if (ret != 0) {
            BM_LOG_ERROR("AclrtStreamAbort failed, stream: " << stream << " ret: " << ret);
            continue;
        }
        BM_LOG_WARN("AclrtStreamAbort done for peer fault, stream: " << stream);
    }
}

void HybmStreamManager::UnregisterThreadAclStream(int64_t tid, void *stream)
{
    std::unique_lock<std::shared_mutex> lock(g_threadAclStreamMutex);
    auto it = g_threadAclStreams.find(tid);
    if (it != g_threadAclStreams.end() && it->second == stream) {
        g_threadAclStreams.erase(it);
    }
}
} // namespace mf
} // namespace ock
