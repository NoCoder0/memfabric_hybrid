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
#include "genetty_verbs_device_ts.h"
#include "under_api/hal/genetty_hal_types.h"
#include <chrono>
#include <thread>
#include <sys/syscall.h>
#include <unistd.h>
#include "under_api/acl/genetty_dl_acl_api.h"
namespace ock {
namespace openDCC {
namespace verbs {
namespace {
constexpr uint32_t TS_SQ_DEPTH = 2048;
constexpr uint32_t TS_LOGIC_CQ_DEPTH = 4096;
constexpr uint32_t TS_CQE_SIZE = 32;
constexpr uint32_t TS_CLEANUP_TIMEOUT_MS = 1000;
constexpr int32_t TS_SEND_TIMEOUT_MS = 100;
constexpr uint8_t TS_WRITE_VALUE_TYPE = 8;
constexpr uint8_t TS_KERNEL_CREDIT = 254;
constexpr uint32_t TS_WRITE_SIZE_64 = 3;
constexpr uint32_t TS_RDMA_DB_SUBTYPE = 2;
constexpr uint32_t TS_ADDR_HIGH_MASK = 0x1ffff;
constexpr uint64_t TS_ROCEE_BASE = 0x2000000000ULL;
constexpr uint64_t TS_ROCEE_DB_OFFSET = 0x230ULL;
constexpr uint64_t TS_CHIP_OFFSET = 0x80000000000ULL;
constexpr uint64_t TS_DIE_OFFSET = 0x10000000000ULL;
constexpr uint64_t TS_FLAT_BASE = 0x200000000000ULL;
constexpr uint64_t TS_FLAT_CHIP_OFFSET = 0x20000000000ULL;
constexpr int32_t TS_INFO_CHIP = 18;
constexpr int32_t TS_INFO_DIE = 19;
constexpr int32_t TS_INFO_ADDR_MODE = 30;
constexpr uint32_t TS_SQ_FLAGS = 1U << 1;
static_assert(sizeof(rtStarsSqe_t) == 64, "Invalid TS SQE layout");
template<typename T>
T LoadHalSymbol(void *handle, const char *name, const char *alternate)
{
    auto fn = reinterpret_cast<T>(dlsym(handle, name));
    return fn != nullptr ? fn : reinterpret_cast<T>(dlsym(handle, alternate));
}
} // namespace

struct DeviceTsWriter::Impl {
    void *hal = nullptr;
    halSqCqAllocateFunc allocate = nullptr;
    halSqCqFreeFunc freeQueue = nullptr;
    halResourceIdAllocFunc allocateId = nullptr;
    halResourceIdFreeFunc freeId = nullptr;
    halResourceConfigFunc configure = nullptr;
    halSqTaskSendFunc send = nullptr;
    halSqCqQueryFunc query = nullptr;
    uint32_t userId = 0;
    uint32_t logicId = 0;
    uint32_t stream = 0;
    uint32_t sq = 0;
    uint32_t cq = 0;
    uint32_t logicCq = 0;
    uint32_t depth = TS_SQ_DEPTH;
    uint32_t tail = 0;
    uint32_t task = 0;
    uint64_t dbAddress = 0;
    bool hasStream = false;
    bool hasQueue = false;
    bool hasLogicCq = false;
    bool inFlight = false;
    bool initialized = false;
    rtStarsSqe_t sqe{};
    Result Load() noexcept;
    Result QueryAddress() noexcept;
    Result AllocateStream() noexcept;
    Result AllocateQueue() noexcept;
    Result AllocateLogicCq() noexcept;
    Result WaitIdle(uint32_t timeoutMs) noexcept;
    Result Release() noexcept;
};

Result DeviceTsWriter::Impl::Load() noexcept
{
    const char *libraries[] = {"libascend_hal.so", "/usr/local/Ascend/driver/lib64/driver/libascend_hal.so"};
    for (const auto *library : libraries) {
        hal = dlopen(library, RTLD_NOW | RTLD_NODELETE);
        if (hal == nullptr) {
            continue;
        }
        allocate = LoadHalSymbol<halSqCqAllocateFunc>(hal, "halSqCqAllocate", "hal_sq_cq_allocate");
        freeQueue = LoadHalSymbol<halSqCqFreeFunc>(hal, "halSqCqFree", "hal_sq_cq_free");
        allocateId = LoadHalSymbol<halResourceIdAllocFunc>(hal, "halResourceIdAlloc", "hal_resource_id_alloc");
        freeId = LoadHalSymbol<halResourceIdFreeFunc>(hal, "halResourceIdFree", "hal_resource_id_free");
        configure = LoadHalSymbol<halResourceConfigFunc>(hal, "halResourceConfig", "hal_resource_config");
        send = LoadHalSymbol<halSqTaskSendFunc>(hal, "halSqTaskSend", "hal_sq_task_send");
        query = LoadHalSymbol<halSqCqQueryFunc>(hal, "halSqCqQuery", "hal_sq_cq_query");
        if (allocate && freeQueue && allocateId && freeId && configure && send && query) {
            return GEN_OK;
        }
        dlclose(hal);
        hal = nullptr;
    }
    GEN_LOG_ERROR("Load TS HAL APIs failed");
    return GEN_LOAD_LIBRARY_FAILED;
}

Result DeviceTsWriter::Impl::QueryAddress() noexcept
{
    auto runtime = dlopen("libruntime.so", RTLD_NOW | RTLD_NODELETE);
    GEN_ASSERT_MSG_RETURN(runtime != nullptr, "Open runtime for TS topology failed", GEN_LOAD_LIBRARY_FAILED);
    using GetInfo = int (*)(uint32_t, int32_t, int32_t, int64_t *);
    auto getInfo = reinterpret_cast<GetInfo>(dlsym(runtime, "rtGetDeviceInfo"));
    int64_t chip = 0, die = 0, mode = 0;
    bool valid = getInfo != nullptr && getInfo(userId, 0, TS_INFO_CHIP, &chip) == 0 &&
                 getInfo(userId, 0, TS_INFO_DIE, &die) == 0 && getInfo(userId, 0, TS_INFO_ADDR_MODE, &mode) == 0;
    dlclose(runtime);
    GEN_ASSERT_MSG_RETURN(valid && chip >= 0 && die >= 0, "Query TS topology failed, userDeviceId(" << userId << ")",
                          GEN_DEVICE_ERROR);
    dbAddress = TS_ROCEE_BASE + TS_ROCEE_DB_OFFSET + (mode != 0 ? TS_FLAT_BASE : 0) +
                (mode != 0 ? TS_FLAT_CHIP_OFFSET : TS_CHIP_OFFSET) * static_cast<uint64_t>(chip) +
                TS_DIE_OFFSET * static_cast<uint64_t>(die);
    return GEN_OK;
}

Result DeviceTsWriter::Impl::AllocateStream() noexcept
{
    halResourceIdInputInfo input{};
    halResourceIdOutputInfo output{};
    input.type = DRV_STREAM_ID;
    auto result = allocateId(logicId, &input, &output);
    GEN_ASSERT_MSG_RETURN(result == 0, "Allocate TS stream failed, result(" << result << ")", GEN_DEVICE_ERROR);
    stream = output.resourceId;
    hasStream = true;
    GEN_ASSERT_MSG_RETURN(stream <= UINT16_MAX, "TS stream id exceeds SQE field width(" << stream << ")",
                          GEN_DEVICE_ERROR);
    return GEN_OK;
}

Result DeviceTsWriter::Impl::AllocateQueue() noexcept
{
    halSqCqInputInfo input{};
    halSqCqOutputInfo output{};
    input.type = DRV_NORMAL_TYPE;
    input.sqeSize = sizeof(sqe);
    input.cqeSize = TS_CQE_SIZE;
    input.sqeDepth = TS_SQ_DEPTH;
    input.cqeDepth = TS_SQ_DEPTH;
    input.info[0] = stream;
    input.info[2] = TS_SQ_FLAGS;
    input.info[3] = 1;
    input.info[4] = UINT32_MAX;
    auto result = allocate(logicId, &input, &output);
    GEN_ASSERT_MSG_RETURN(result == 0, "Allocate TS SQ/CQ failed, result(" << result << ")", GEN_DEVICE_ERROR);
    sq = output.sqId;
    cq = output.cqId;
    hasQueue = true;
    halSqCqQueryInfo info{};
    info.type = DRV_NORMAL_TYPE;
    info.sqId = sq;
    info.prop = DRV_SQCQ_PROP_SQ_DEPTH;
    result = query(logicId, &info);
    GEN_ASSERT_MSG_RETURN(result == 0 && info.value[0] > 1 && info.value[0] <= UINT16_MAX,
                          "Query TS SQ depth failed, result(" << result << ")", GEN_DEVICE_ERROR);
    depth = info.value[0];
    info.prop = DRV_SQCQ_PROP_SQ_TAIL;
    result = query(logicId, &info);
    GEN_ASSERT_MSG_RETURN(result == 0 && info.value[0] < depth, "Query TS SQ tail failed, result(" << result << ")",
                          GEN_DEVICE_ERROR);
    tail = info.value[0];
    return GEN_OK;
}

Result DeviceTsWriter::Impl::AllocateLogicCq() noexcept
{
    halSqCqInputInfo input{};
    halSqCqOutputInfo output{};
    input.type = DRV_LOGIC_TYPE;
    input.cqeSize = TS_CQE_SIZE;
    input.cqeDepth = TS_LOGIC_CQ_DEPTH;
    input.cqId = UINT16_MAX;
    input.info[0] = stream;
    input.info[1] = static_cast<uint32_t>(syscall(SYS_gettid));
    auto result = allocate(logicId, &input, &output);
    GEN_ASSERT_MSG_RETURN(result == 0, "Allocate TS logic CQ failed, result(" << result << ")", GEN_DEVICE_ERROR);
    logicCq = output.cqId;
    hasLogicCq = true;
    halResourceIdInputInfo resource{};
    halResourceConfigInfo config{};
    resource.type = DRV_STREAM_ID;
    resource.resourceId = stream;
    config.prop = DRV_STREAM_BIND_LOGIC_CQ;
    config.value[0] = logicCq;
    result = configure(logicId, &resource, &config);
    GEN_ASSERT_MSG_RETURN(result == 0, "Bind TS logic CQ failed, result(" << result << ")", GEN_DEVICE_ERROR);
    return GEN_OK;
}

Result DeviceTsWriter::Impl::WaitIdle(uint32_t timeoutMs) noexcept
{
    if (!inFlight) {
        return GEN_OK;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        halSqCqQueryInfo info{};
        info.type = DRV_NORMAL_TYPE;
        info.sqId = sq;
        info.prop = DRV_SQCQ_PROP_SQ_HEAD;
        auto result = query(logicId, &info);
        GEN_ASSERT_MSG_RETURN(result == 0, "Query TS SQ head failed, result(" << result << ")", GEN_DEVICE_ERROR);
        if (info.value[0] == tail) {
            inFlight = false;
            return GEN_OK;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    GEN_LOG_ERROR("Wait TS SQ head timed out, sq(" << sq << "), tail(" << tail << ")");
    return GEN_ERROR;
}

Result DeviceTsWriter::Impl::Release() noexcept
{
    auto result = WaitIdle(TS_CLEANUP_TIMEOUT_MS);
    if (result != GEN_OK) {
        return result;
    }
    if (hasQueue) {
        halSqCqFreeInfo info{};
        info.type = DRV_NORMAL_TYPE;
        info.sqId = sq;
        info.cqId = cq;
        result = freeQueue(logicId, &info);
        GEN_ASSERT_MSG_RETURN(result == 0, "Free TS SQ/CQ failed, result(" << result << ")", GEN_DEVICE_ERROR);
        hasQueue = false;
    }
    if (hasStream) {
        halResourceIdInputInfo info{};
        info.type = DRV_STREAM_ID;
        info.resourceId = stream;
        result = freeId(logicId, &info);
        GEN_ASSERT_MSG_RETURN(result == 0, "Free TS stream failed, result(" << result << ")", GEN_DEVICE_ERROR);
        hasStream = false;
    }
    if (hasLogicCq) {
        halSqCqFreeInfo info{};
        info.type = DRV_LOGIC_TYPE;
        info.cqId = logicCq;
        result = freeQueue(logicId, &info);
        GEN_ASSERT_MSG_RETURN(result == 0, "Free TS logic CQ failed, result(" << result << ")", GEN_DEVICE_ERROR);
        hasLogicCq = false;
    }
    if (hal != nullptr) {
        dlclose(hal);
        hal = nullptr;
    }
    initialized = false;
    return GEN_OK;
}

DeviceTsWriter::DeviceTsWriter() : impl_(new(std::nothrow) Impl) {}
DeviceTsWriter::~DeviceTsWriter()
{
    (void)Destroy();
}

Result DeviceTsWriter::Initialize(uint32_t userDeviceId, uint32_t logicDeviceId) noexcept
{
    GEN_ASSERT_MSG_RETURN(impl_ != nullptr, "Allocate TS writer failed", GEN_MALLOC_FAILED);
    if (impl_->initialized) {
        return GEN_OK;
    }
    if (impl_->hal != nullptr && impl_->Release() != GEN_OK) {
        return GEN_DEVICE_ERROR;
    }
    impl_->userId = userDeviceId;
    impl_->logicId = logicDeviceId;
    auto result = AclApi::AclrtSetDevice(static_cast<int32_t>(userDeviceId));
    GEN_ASSERT_MSG_RETURN(result == 0, "Set device for TS failed, result(" << result << ")", GEN_DEVICE_ERROR);
    if ((result = impl_->Load()) != GEN_OK || (result = impl_->QueryAddress()) != GEN_OK ||
        (result = impl_->AllocateStream()) != GEN_OK || (result = impl_->AllocateQueue()) != GEN_OK ||
        (result = impl_->AllocateLogicCq()) != GEN_OK) {
        return result;
    }
    impl_->initialized = true;
    return GEN_OK;
}

Result DeviceTsWriter::Submit(uint64_t dbInfo, uint32_t timeoutMs) noexcept
{
    GEN_ASSERT_MSG_RETURN(impl_ != nullptr && impl_->initialized, "TS writer is not initialized", GEN_NOT_INITIALIZED);
    auto &state = *impl_;
    auto result = state.WaitIdle(timeoutMs);
    if (result != GEN_OK) {
        return result;
    }
    result = AclApi::AclrtSetDevice(static_cast<int32_t>(state.userId));
    GEN_ASSERT_MSG_RETURN(result == 0, "Set device for TS submission failed", GEN_DEVICE_ERROR);
    state.sqe = {};
    auto &sqe = state.sqe.writeValueSqe;
    sqe.header.type = TS_WRITE_VALUE_TYPE;
    sqe.header.rt_stream_id = static_cast<uint16_t>(state.stream);
    sqe.header.task_id = static_cast<uint16_t>(state.task++ % state.depth);
    sqe.kernel_credit = TS_KERNEL_CREDIT;
    sqe.awsize = TS_WRITE_SIZE_64;
    sqe.write_addr_low = static_cast<uint32_t>(state.dbAddress);
    sqe.write_addr_high = static_cast<uint32_t>(state.dbAddress >> 32) & TS_ADDR_HIGH_MASK;
    sqe.sub_type = TS_RDMA_DB_SUBTYPE;
    sqe.write_value_part0 = static_cast<uint32_t>(dbInfo);
    sqe.write_value_part1 = static_cast<uint32_t>(dbInfo >> 32);
    halTaskSendInfo info{};
    info.type = DRV_NORMAL_TYPE;
    info.sqId = state.sq;
    info.timeout = TS_SEND_TIMEOUT_MS;
    info.sqe_addr = reinterpret_cast<uint8_t *>(&state.sqe);
    info.sqe_num = 1;
    state.tail = (state.tail + 1) % state.depth;
    state.inFlight = true;
    result = state.send(state.logicId, &info);
    GEN_ASSERT_MSG_RETURN(result == 0, "Submit TS doorbell failed, result(" << result << ")", GEN_POST_FAILED);
    return state.WaitIdle(timeoutMs);
}

Result DeviceTsWriter::Destroy() noexcept
{
    return impl_ != nullptr ? impl_->Release() : GEN_OK;
}
} // namespace verbs
} // namespace openDCC
} // namespace ock
