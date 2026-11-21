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
#include "genetty_host_urma_wrapper.h"

namespace ock {
namespace openDCC {
namespace urma {
namespace {
/* forward the urma logs to the genetty logger with the matched level */
void UrmaLogFunc(int level, char *message)
{
    if (message == nullptr) {
        return;
    }

    switch (level) {
        case URMA_VLOG_LEVEL_EMERG:
        case URMA_VLOG_LEVEL_ALERT:
        case URMA_VLOG_LEVEL_CRIT:
        case URMA_VLOG_LEVEL_ERR:
            GEN_LOG_ERROR("[URMA_API] " << message);
            break;
        case URMA_VLOG_LEVEL_WARNING:
            GEN_LOG_WARN("[URMA_API] " << message);
            break;
        case URMA_VLOG_LEVEL_NOTICE:
        case URMA_VLOG_LEVEL_INFO:
            GEN_LOG_INFO("[URMA_API] " << message);
            break;
        case URMA_VLOG_LEVEL_DEBUG:
            GEN_LOG_DEBUG("[URMA_API] " << message);
            break;
        default:
            break;
    }
}
} // namespace

Result UrmaUtil::UrmaInit()
{
    auto result = UrmaApi::Load();
    if (result != GEN_OK) {
        GEN_LOG_ERROR("Init urma failed, as load urma api failed, result(" << result << ")");
        return result;
    }

    UrmaSetLogger();

    auto status = UrmaApi::urma_init(nullptr);
    if (status != URMA_SUCCESS) {
        GEN_LOG_ERROR("Init urma failed, as urma_init failed, status(" << static_cast<int32_t>(status) << ")");
        return GEN_ERROR;
    }

    GEN_LOG_DEBUG("Init urma successfully");
    return GEN_OK;
}

void UrmaUtil::UrmaUnInit()
{
    if (!UrmaApi::IsLoaded()) {
        GEN_LOG_DEBUG("Uninit urma skipped, as urma api is not loaded");
        return;
    }

    auto status = UrmaApi::urma_uninit();
    if (status != URMA_SUCCESS) {
        GEN_LOG_WARN("Uninit urma failed, status(" << static_cast<int32_t>(status) << ")");
    }

    UrmaApi::UnLoad();
    GEN_LOG_DEBUG("Uninit urma successfully");
}

void UrmaUtil::UrmaSetLogger()
{
    if (!UrmaApi::IsLoaded()) {
        GEN_LOG_ERROR("Set urma logger failed, as urma api is not loaded");
        return;
    }

    auto status = UrmaApi::urma_register_log_func(UrmaLogFunc);
    if (status != URMA_SUCCESS) {
        GEN_LOG_ERROR("Set urma logger failed, as register log func failed, status(" << static_cast<int32_t>(status)
                                                                                     << ")");
        return;
    }

    GEN_LOG_DEBUG("Set urma logger successfully");
}

std::map<std::string, UrmaDevicePtr> UrmaDevice::ALL_DEVICES;
bool UrmaDevice::LOADED = false;
std::mutex UrmaDevice::MUTEX;
std::map<urma_speed_t, UrmaDeviceBandWidth> UrmaDevice::URMA_BANDWIDTHS = {
    {URMA_SP_10M, {"10Mb", 1}},     {URMA_SP_100M, {"100Mb", 1}},   {URMA_SP_1G, {"1Gb", 1}},
    {URMA_SP_2_5G, {"2.5Gb", 5}},   {URMA_SP_5G, {"5Gb", 5}},       {URMA_SP_10G, {"10Gb", 10}},
    {URMA_SP_14G, {"14Gb", 14}},    {URMA_SP_25G, {"15Gb", 25}},    {URMA_SP_40G, {"40Gb", 40}},
    {URMA_SP_50G, {"50Gb", 50}},    {URMA_SP_100G, {"100Gb", 100}}, {URMA_SP_200G, {"200Gb", 200}},
    {URMA_SP_400G, {"400Gb", 255}}, {URMA_SP_800G, {"800Gb", 255}}};

void UrmaDevice::Init() noexcept
{
    std::lock_guard<std::mutex> lock(MUTEX);
    if (LOADED) {
        GEN_LOG_DEBUG("already inited");
        return;
    }

    urma_device_t **device_list = nullptr;
    int num_devices = 0;
    device_list = UrmaApi::urma_get_device_list(&num_devices);
    if (device_list == nullptr || num_devices == 0) {
        GEN_LOG_ERROR("[URMA_API] No urma device found in the system, please use 'urma_admin show' to double check");
        return;
    }

    for (int i = 0; i < num_devices; i++) {
        auto dev = device_list[i];
        if (UNLIKELY(dev == nullptr)) {
            GEN_LOG_DEBUG("Invalid device in device list, which should not happen");
            continue;
        }

        /* get device attributes and get eid list */
        urma_device_attr_t tmp_attr;
        auto result = UrmaApi::urma_query_device(dev, &tmp_attr);
        GEN_LOG_DEBUG("urma_query_device(" << dev->name << ", " << dev->path << ") return " << result);
        if (LIKELY(result == URMA_SUCCESS)) {
            /* create new device and add into all devices */
            auto new_dev = mf::MakeRef<UrmaDevice>(dev->name, dev->path, tmp_attr);
            if (new_dev == nullptr) {
                GEN_LOG_ERROR("New object failed, probably out of memory");
                UrmaApi::urma_free_device_list(device_list);
                return;
            }

            ALL_DEVICES.emplace(dev->name, new_dev);

            /* get eids */
            uint32_t eid_count = 0;
            auto eid_list = UrmaApi::urma_get_eid_list(dev, &eid_count);

            GEN_LOG_DEBUG("urma_get_eid_list(" << dev->name << ", " << eid_count << ") return "
                                               << (eid_list != nullptr));

            if (UNLIKELY(eid_list == nullptr)) {
                GEN_LOG_DEBUG("Get eid list failed");
                continue;
            }

            for (uint32_t j = 0; j < eid_count; j++) {
                auto eid = eid_list[j];
                new_dev->eidList_.push_back(eid);
            }

            UrmaApi::urma_free_eid_list(eid_list);
        } else {
            GEN_LOG_ERROR("[URMA_API] urma_query_device() failed, errno " << errno);
        }
    }

    /* free device list */
    UrmaApi::urma_free_device_list(device_list);

    LOADED = true;
    GEN_LOG_DEBUG("urma device init finished.");
}

UrmaDevice::UrmaDevice(const std::string &name, const std::string &sys_path, const urma_device_attr_t &attr)
    : deviceName_(name), deviceSysPath_(sys_path), attributes_(attr)
{}

std::string UrmaDevice::ToString(bool whole, const std::string &prefix, const std::string &seperator) const noexcept
{
    constexpr int width = 20;
    std::ostringstream oss;
    oss << prefix << std::left << std::setw(width) << "device name: " << deviceName_ << seperator;
    oss << prefix << std::left << std::setw(width) << "sys path: " << deviceSysPath_ << seperator;
    oss << prefix << std::left << std::setw(width) << "port count: " << (uint32_t)attributes_.port_cnt << seperator;

    if (attributes_.port_cnt > 0) {
        /* bandwidth */
        auto speed = attributes_.port_attr[0].active_speed;
        auto iter = URMA_BANDWIDTHS.find(speed);
        if (iter == URMA_BANDWIDTHS.end()) {
            oss << prefix << std::left << std::setw(width) << "bandwidth: invalid" << seperator;
        } else {
            oss << prefix << std::left << std::setw(width) << "bandwidth: " << iter->second.str << seperator;
        }

        /* state */
        oss << prefix << std::left << std::setw(width) << "state: " << DeviceState2Str(attributes_.port_attr[0].state)
            << seperator;

        /* mtu */
        oss << prefix << std::left << std::setw(width)
            << "active mtu: " << DeviceMTU2Str(attributes_.port_attr[0].active_mtu) << seperator;
    }

    if (whole) {
        oss << prefix << std::left << std::setw(width) << "max jetty: " << attributes_.dev_cap.max_jetty << seperator;
        oss << prefix << std::left << std::setw(width) << "max jfs: " << attributes_.dev_cap.max_jfs << seperator;
        oss << prefix << std::left << std::setw(width) << "max jfs depth: " << attributes_.dev_cap.max_jfs_depth
            << seperator;
        oss << prefix << std::left << std::setw(width) << "max jfr: " << attributes_.dev_cap.max_jfr << seperator;
        oss << prefix << std::left << std::setw(width) << "max jfr depth: " << attributes_.dev_cap.max_jfr_depth
            << seperator;
        oss << prefix << std::left << std::setw(width) << "max jfc: " << attributes_.dev_cap.max_jfc << seperator;
        oss << prefix << std::left << std::setw(width) << "max jfc depth: " << attributes_.dev_cap.max_jfc_depth
            << seperator;
        oss << prefix << std::left << std::setw(width) << "max jfr sge: " << attributes_.dev_cap.max_jfr_sge
            << seperator;
        oss << prefix << std::left << std::setw(width) << "max jfs sge: " << attributes_.dev_cap.max_jfs_sge
            << seperator;
        oss << prefix << std::left << std::setw(width) << "max jfs rsge: " << attributes_.dev_cap.max_jfs_rsge
            << seperator;
        oss << prefix << std::left << std::setw(width) << "max msg size: " << attributes_.dev_cap.max_msg_size
            << seperator;
        oss << prefix << std::left << std::setw(width) << "max inline size: " << attributes_.dev_cap.max_jfs_inline_len
            << seperator;
        oss << prefix << std::left << std::setw(width) << "max read: " << attributes_.dev_cap.max_read_size
            << seperator;
        oss << prefix << std::left << std::setw(width) << "max write: " << attributes_.dev_cap.max_write_size
            << seperator;

        oss << prefix << std::left << std::setw(width) << "eid count: " << eidList_.size() << seperator;
        char tmp_buf[128]{};
        uint32_t eid_index = 0;
        for (auto &eid : eidList_) {
            sprintf(tmp_buf, EID_FMT, EID_ARGS(eid.eid));
            oss << prefix << std::left << std::setw(width) << " - eid " << eid_index++ << ": " << tmp_buf << seperator;
        }
    }

    return oss.str();
}

/**
 * Functions for urma context
 */
std::map<std::pair<std::string, uint32_t>, UrmaContextPtr> UrmaContext::ALL_CONTEXTS;
std::mutex UrmaContext::ALL_CONTEXTS_MUTEX;
std::atomic<uint32_t> UrmaContext::AUTO_INCREASE_JETTY_ID(0);
std::atomic<uint32_t> UrmaContext::AUTO_INCREASE_CONTEXT_ID(0);

Result UrmaContext::LookupContext(const std::string &devName, uint32_t eidIndex, UrmaContextPtr &out)
{
    auto tmp_key = std::pair<std::string, uint32_t>(devName, eidIndex);
    std::lock_guard<std::mutex> guard(ALL_CONTEXTS_MUTEX);
    auto context_iter = ALL_CONTEXTS.find(tmp_key);
    if (context_iter != ALL_CONTEXTS.end()) {
        out = context_iter->second;
        return GEN_OK;
    }

    return GEN_ERROR;
}

void UrmaContext::ClearAllContexts() noexcept
{
    std::map<std::pair<std::string, uint32_t>, UrmaContextPtr> contexts;
    {
        std::lock_guard<std::mutex> guard(ALL_CONTEXTS_MUTEX);
        contexts.swap(ALL_CONTEXTS);
    }

    if (!contexts.empty()) {
        GEN_LOG_WARN("release " << contexts.size() << " urma context(s) held by the registry");
    }
    contexts.clear();
}

Result UrmaContext::CreateContext(const std::string &devName, uint32_t eidIndex, UrmaContextPtr &out)
{
    GEN_LOG_DEBUG("enter dev(" << devName << ") eid index(" << eidIndex << ")");
    if (devName.empty()) {
        GEN_LOG_ERROR("Create urma context failed, as param 'devName' is empty");
        return GEN_INVALID_PARAM;
    }

    /* double confirm urma device is initialized */
    UrmaDevice::Init();

    /* find target device name and object */
    std::string target_dev_name;
    UrmaDevicePtr target_dev;
    auto devices = UrmaDevice::AllDevices();
    for (auto &iter : devices) {
        if (iter.first.find(devName) != std::string::npos) {
            target_dev_name = iter.first;
            target_dev = iter.second;
            break;
        }
    }

    if (target_dev_name.empty()) {
        GEN_LOG_ERROR("Not found device name matched(" << devName << ")");
        return GEN_INVALID_PARAM;
    }

    GEN_LOG_DEBUG("found device(" << target_dev_name << ")");

    /* try lookup first, return if already created */
    if (LookupContext(target_dev_name, eidIndex, out) == GEN_OK) {
        return GEN_OK;
    }

    /* not found, need to create a new one */
    auto raw_dev = UrmaApi::urma_get_device_by_name(const_cast<char *>(target_dev_name.c_str()));
    if (raw_dev == nullptr) {
        GEN_LOG_ERROR("[URMA_API] urma_get_device_by_name() failed for (" << target_dev_name << "), errno(" << errno
                                                                         << ")");
        return GEN_DEVICE_ERROR;
    }

    /* step5: create raw context */
    auto raw_context = UrmaApi::urma_create_context(raw_dev, eidIndex);
    if (raw_context == nullptr) {
        GEN_LOG_ERROR("[URMA_API] Create urma context with dev name(" << target_dev_name << ") and eid index("
                                                                      << eidIndex << ") failed, errno(" << errno
                                                                      << ")");
        return GEN_DEVICE_ERROR;
    }

    uint32_t eid_count = 0;
    auto eid_list = UrmaApi::urma_get_eid_list(raw_dev, &eid_count);
    if (eid_list == nullptr || eid_count == 0) {
        UrmaApi::urma_delete_context(raw_context);
        GEN_LOG_ERROR("Eid list is null or eid count is " << eid_count);
        return GEN_DEVICE_ERROR;
    }

    urma_eid_info_t *matched_eid_info = nullptr;
    for (uint32_t i = 0; i < eid_count; i++) {
        if (eid_list[i].eid_index == eidIndex) {
            matched_eid_info = &eid_list[i];
            break;
        }
    }

    if (matched_eid_info == nullptr) {
        UrmaApi::urma_free_eid_list(eid_list);
        UrmaApi::urma_delete_context(raw_context);
        GEN_LOG_ERROR("Eid index(" << eidIndex << ") not found in eid list (eid count is " << eid_count << ")");
        return GEN_DEVICE_ERROR;
    }

    auto eid_info = *matched_eid_info;

    /* step6: create our context */
    auto tmp_context = mf::MakeRef<UrmaContext>(raw_context, target_dev, eid_info);
    UrmaApi::urma_free_eid_list(eid_list);

    if (tmp_context == nullptr) {
        UrmaApi::urma_delete_context(raw_context);
        GEN_LOG_ERROR("Create UrmaContext object failed, probaly out of memory");
        return GEN_MALLOC_FAILED;
    }

    tmp_context->contextId_ = AUTO_INCREASE_CONTEXT_ID.fetch_add(1);

    /* step8: insert into all context */
    auto tmp_key = std::pair<std::string, uint32_t>(target_dev_name, eidIndex);
    ALL_CONTEXTS.emplace(tmp_key, tmp_context);

    /* step9: assign to out */
    out = tmp_context;

    GEN_LOG_DEBUG("created");
    return GEN_OK;
}

bool UrmaContext::IsBadPort() noexcept
{
    if (badRecord_.error_occurred_time == 0) {
        return false;
    }
    return (MonotonicTime::TimeNs() - badRecord_.error_occurred_time) < (UB_BAD_PORT_RECOVER_SECONDS * 1000000000UL);
}

void UrmaContext::RecordPortError(int16_t errorCode) noexcept
{
    badRecord_.last_error = errorCode;
    badRecord_.error_occurred_time = MonotonicTime::TimeNs();
}

uint32_t UrmaContext::NewJettyId() noexcept
{
    ++AUTO_INCREASE_JETTY_ID;
    return AUTO_INCREASE_JETTY_ID.load();
}

UrmaContext::~UrmaContext()
{
    if (rawContext_ != nullptr) {
        if (LIKELY(UrmaApi::IsLoaded())) {
            UrmaApi::urma_delete_context(rawContext_);
        }
        rawContext_ = nullptr;
    }
}

Result UrmaContext::CreateJfce(UrmaJfcePtr &out)
{
    GEN_LOG_DEBUG("enter");
    if (UNLIKELY(rawContext_ == nullptr)) {
        GEN_LOG_ERROR("Create jfce failed, as context is null");
        return GEN_ERROR;
    }

    urma_jfce_t *raw_jfce = UrmaApi::urma_create_jfce(rawContext_);
    if (raw_jfce == nullptr) {
        GEN_LOG_ERROR("[URMA_API] Create jfce failed, errno " << errno);
        return GEN_ERROR;
    }

    auto tmp = mf::MakeRef<UrmaJfce>(raw_jfce, this);
    if (tmp == nullptr) {
        GEN_LOG_ERROR("Create UrmaJfce object failed, probably out of memory");
        UrmaApi::urma_delete_jfce(raw_jfce);
        return GEN_MALLOC_FAILED;
    }

    out = tmp;

    GEN_LOG_DEBUG("created jfce with fd(" << raw_jfce->fd << ") at context(" << contextId_ << ")");
    return GEN_OK;
}

Result UrmaContext::CreateJfc(urma_jfc_cfg_t &cfg, UrmaJfcPollingType pollingType, UrmaJfcPtr &out)
{
    GEN_LOG_DEBUG("enter");
    if (UNLIKELY(rawContext_ == nullptr)) {
        GEN_LOG_ERROR("Create jfc failed, as context is null");
        return GEN_ERROR;
    }

    if (pollingType == EVENT_POLLING) {
        if (cfg.jfce == nullptr) {
            auto raw_jfce = UrmaApi::urma_create_jfce(this->rawContext_);
            if (raw_jfce == nullptr) {
                GEN_LOG_ERROR("[URMA_API] Create jfce failed, errno " << errno);
                return GEN_ERROR;
            }

            cfg.jfce = raw_jfce;
            GEN_LOG_DEBUG(cfg);
        }
        auto raw_jfce = cfg.jfce;

        auto raw_jfc = UrmaApi::urma_create_jfc(this->rawContext_, &cfg);
        if (raw_jfc == nullptr) {
            UrmaApi::urma_delete_jfce(raw_jfce);
            GEN_LOG_ERROR("[URMA_API] Create jfc failed, errno " << errno << ", " << cfg);
            return GEN_ERROR;
        }

        if (UrmaApi::urma_rearm_jfc(raw_jfc, 0) != URMA_SUCCESS) {
            UrmaApi::urma_delete_jfce(raw_jfce);
            UrmaApi::urma_delete_jfc(raw_jfc);
            GEN_LOG_ERROR("[URMA_API] create jfc failed as rearm jfc failed, errno " << errno);
            return GEN_ERROR;
        }

        // O_NONBLOCK导致裸设备urma_wait_jfc不做等待poller循环调用，待urma功能完善后放开
        /*auto tmp_ctl = fcntl(raw_jfce->fd, F_GETFL);
        if (fcntl(raw_jfce->fd, F_SETFL, static_cast<uint32_t>(tmp_ctl) | O_NONBLOCK) < 0) {
            UrmaApi::urma_delete_jfce(raw_jfce);
            UrmaApi::urma_delete_jfc(raw_jfc);
            GEN_LOG_ERROR("fcntl() set jfc fd to non-blocking failed, errno " << errno);
            return GEN_ERROR;
        }*/

        auto jfc = mf::MakeRef<UrmaJfc>(pollingType, raw_jfc, raw_jfce, this);
        if (jfc == nullptr) {
            UrmaApi::urma_delete_jfce(raw_jfce);
            UrmaApi::urma_delete_jfc(raw_jfc);
            GEN_LOG_ERROR("Create UrmaJfc object failed, probably out of memory");
            return GEN_MALLOC_FAILED;
        }

        out = jfc;
    } else {
        cfg.jfce = nullptr;
        GEN_LOG_DEBUG(cfg);

        auto raw_jfc = UrmaApi::urma_create_jfc(this->rawContext_, &cfg);
        if (raw_jfc == nullptr) {
            GEN_LOG_ERROR("[URMA_API] Create jfc failed, errno " << errno << ", " << cfg);
            return GEN_ERROR;
        }

        auto jfc = mf::MakeRef<UrmaJfc>(pollingType, raw_jfc, nullptr, this);
        if (jfc == nullptr) {
            UrmaApi::urma_delete_jfc(raw_jfc);
            GEN_LOG_ERROR("Create UrmaJfc object failed, probably out of memory");
            return GEN_MALLOC_FAILED;
        }
        out = jfc;
    }

    GEN_LOG_DEBUG("jfc created");
    return GEN_OK;
}

Result UrmaContext::CreateJfc(urma_jfc_cfg_t &cfg, UrmaJfcePtr &jfce, UrmaJfcPtr &out)
{
    GEN_LOG_DEBUG("enter");
    if (UNLIKELY(rawContext_ == nullptr)) {
        GEN_LOG_ERROR("Create jfc failed, as context is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(jfce == nullptr || jfce->rawJfce_ == nullptr)) {
        GEN_LOG_ERROR("Create jfc failed, as context is null");
        return GEN_INVALID_PARAM;
    }

    cfg.jfce = jfce->rawJfce_;
    GEN_LOG_DEBUG(cfg);

    auto raw_jfc = UrmaApi::urma_create_jfc(this->rawContext_, &cfg);
    if (raw_jfc == nullptr) {
        GEN_LOG_ERROR("[URMA_API] Create jfc failed, errno " << errno << ", " << cfg);
        return GEN_ERROR;
    }

    if (UrmaApi::urma_rearm_jfc(raw_jfc, 0) != URMA_SUCCESS) {
        UrmaApi::urma_delete_jfc(raw_jfc);
        GEN_LOG_ERROR("[URMA_API] create jfc failed as rearm jfc failed, errno " << errno);
        return GEN_ERROR;
    }

    auto jfc = mf::MakeRef<UrmaJfc>(EVENT_POLLING, raw_jfc, jfce->rawJfce_, this);
    if (jfc == nullptr) {
        UrmaApi::urma_delete_jfc(raw_jfc);
        GEN_LOG_ERROR("Create UrmaJfc object failed, probably out of memory");
        return GEN_MALLOC_FAILED;
    }

    out = jfc;

    GEN_LOG_DEBUG("jfc created");
    return GEN_OK;
}

Result UrmaContext::CreateJfs(urma_jfs_cfg_t &cfg, const UrmaJfcPtr &jfc, UrmaJfsPtr &out)
{
    GEN_LOG_DEBUG("enter");
    if (UNLIKELY(rawContext_ == nullptr)) {
        GEN_LOG_ERROR("Create jfs failed, as context is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(jfc == nullptr || jfc->rawJfc_ == nullptr)) {
        GEN_LOG_ERROR("Create jfs failed, as jfc is null");
        return GEN_ERROR;
    }

    cfg.jfc = jfc->rawJfc_;
    GEN_LOG_DEBUG(cfg);

    auto raw_jfs = UrmaApi::urma_create_jfs(this->rawContext_, &cfg);
    if (raw_jfs == nullptr) {
        GEN_LOG_ERROR("[URMA_API] Create jfs failed, errno " << errno << ", " << cfg);
        return GEN_ERROR;
    }

    auto jfs = mf::MakeRef<UrmaJfs>(raw_jfs, this, jfc);
    if (jfs == nullptr) {
        UrmaApi::urma_delete_jfs(raw_jfs);
        GEN_LOG_ERROR("Create UrmaJfs object failed, probably out of memory");
        return GEN_MALLOC_FAILED;
    }

    out = jfs;

    GEN_LOG_DEBUG("jfs created");
    return GEN_OK;
}

Result UrmaContext::CreateJfr(urma_jfr_cfg_t &cfg, const UrmaJfcPtr &jfc, UrmaJfrPtr &out)
{
    GEN_LOG_DEBUG("enter");
    if (UNLIKELY(rawContext_ == nullptr)) {
        GEN_LOG_ERROR("Create jfr failed, as context is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(jfc == nullptr || jfc->rawJfc_ == nullptr)) {
        GEN_LOG_ERROR("Create jfr failed, as jfc is null");
        return GEN_ERROR;
    }

    cfg.jfc = jfc->rawJfc_;
    GEN_LOG_DEBUG(cfg);

    auto raw_jfr = UrmaApi::urma_create_jfr(this->rawContext_, &cfg);
    if (raw_jfr == nullptr) {
        GEN_LOG_ERROR("[URMA_API] Create jfr failed, errno(" << errno << "), " << cfg);
        return GEN_ERROR;
    }

    auto jfr = mf::MakeRef<UrmaJfr>(raw_jfr, cfg.token_value.token, this, jfc);
    if (jfr == nullptr) {
        UrmaApi::urma_delete_jfr(raw_jfr);
        GEN_LOG_ERROR("Create UrmaJfr object failed, probably out of memory");
        return GEN_MALLOC_FAILED;
    }

    out = jfr;

    GEN_LOG_DEBUG("jfr created");
    return GEN_OK;
}

Result UrmaContext::CreateJetty(urma_jetty_cfg_t &cfg, urma_tp_type_t rtp_ctp_utp, const UrmaJfsPtr &jfs,
                                const UrmaJfrPtr &jfr, UrmaJettyPtr &out)
{
    GEN_LOG_DEBUG("enter");
    if (UNLIKELY(rawContext_ == nullptr)) {
        GEN_LOG_ERROR("Create jetty failed, as context is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(jfs == nullptr || jfs->rawJfs_ == nullptr || jfs->jfc_ == nullptr || jfs->jfc_->rawJfc_ == nullptr)) {
        GEN_LOG_ERROR("Create jetty failed, as jfs or it jfc is null");
        return GEN_ERROR;
    }

    if (UNLIKELY(jfr == nullptr || jfr->rawJfr_ == nullptr || jfr->jfc_ == nullptr || jfr->jfc_->rawJfc_ == nullptr)) {
        GEN_LOG_ERROR("Create jetty failed, as jfr or it jfc is null");
        return GEN_ERROR;
    }

    cfg.jfs_cfg = jfs->rawJfs_->jfs_cfg;
    cfg.shared.jfr = jfr->rawJfr_;
    cfg.shared.jfc = jfr->jfc_->rawJfc_;

    GEN_LOG_DEBUG(cfg);
    GEN_LOG_DEBUG(jfr->rawJfr_->jfr_cfg);

    auto raw_jetty = UrmaApi::urma_create_jetty(this->rawContext_, &cfg);
    if (raw_jetty == nullptr) {
        GEN_LOG_ERROR("[URMA_API] Create jetty failed, errno " << errno << ", " << cfg);
        return GEN_ERROR;
    }

    auto jetty = mf::MakeRef<UrmaJetty>(rtp_ctp_utp, raw_jetty->jetty_id.id, raw_jetty, this, jfs, jfr);
    if (jetty == nullptr) {
        UrmaApi::urma_delete_jetty(raw_jetty);
        GEN_LOG_ERROR("Create UrmaJetty object failed, probably out of memory");
        return GEN_MALLOC_FAILED;
    }

    out = jetty;

    GEN_LOG_DEBUG("jetty created");
    return GEN_OK;
}

Result UrmaContext::CreateJfsJfrJetty(const UrmaJfcPtr &jfc, uint32_t queueDepth, urma_transport_mode_t transMode,
                                      urma_tp_type_t tpType, UrmaJfsPtr &jfs, UrmaJfrPtr &jfr, UrmaJettyPtr &jetty)
{
    if (UNLIKELY(jfc == nullptr)) {
        GEN_LOG_ERROR("Create jfs/jfr/jetty failed as jfc is null");
        return GEN_INVALID_PARAM;
    }

    urma_jfs_cfg_t jfs_cfg = CreateJfsCfg(queueDepth, transMode, 0);
    if (UNLIKELY(CreateJfs(jfs_cfg, jfc, jfs) != GEN_OK)) {
        GEN_LOG_ERROR("Failed to create jfs while creating jfs/jfr/jetty");
        return GEN_ERROR;
    }

    urma_jfr_cfg_t jfr_cfg = CreateJfrCfg(queueDepth, transMode, 0, 0);
    if (UNLIKELY(CreateJfr(jfr_cfg, jfc, jfr) != GEN_OK)) {
        GEN_LOG_ERROR("Failed to create jfr while creating jfs/jfr/jetty");
        return GEN_ERROR;
    }

    urma_jetty_cfg_t jetty_cfg = CreateJettyCfg(0);
    if (UNLIKELY(CreateJetty(jetty_cfg, tpType, jfs, jfr, jetty) != GEN_OK)) {
        GEN_LOG_ERROR("Failed to create jetty while creating jfs/jfr/jetty");
        return GEN_ERROR;
    }

    return GEN_OK;
}

urma_jfc_cfg_t UrmaContext::CreateJfcCfg(uint32_t queueDepth, uint64_t userCtx)
{
    urma_jfc_cfg_t cfg{};
    bzero(&cfg, sizeof(urma_jfc_cfg_t));

    cfg.depth = queueDepth;
    cfg.user_ctx = userCtx;
    cfg.flag.bs.lock_free = 1;

    return cfg;
}

urma_jfs_cfg_t UrmaContext::CreateJfsCfg(uint32_t queueDepth, urma_transport_mode_t transMode, uint64_t userCtx,
                                         uint8_t priority)
{
    urma_jfs_cfg_t cfg{};
    bzero(&cfg, sizeof(urma_jfs_cfg_t));

    if (UNLIKELY(rawContext_ == nullptr || device_ == nullptr)) {
        GEN_LOG_ERROR("Create jfs cfg failed as raw context or device is nullptr");
        return cfg;
    }

    cfg.flag.bs.order_type = URMA_DEF_ORDER;
    cfg.trans_mode = transMode;
    cfg.depth = queueDepth;
    cfg.max_sge = device_->DeviceAttributes().dev_cap.max_jfs_sge;
    cfg.max_rsge = device_->DeviceAttributes().dev_cap.max_jfs_rsge;
    cfg.max_inline_data = device_->DeviceAttributes().dev_cap.max_jfs_inline_len;
    cfg.rnr_retry = URMA_JFS_RNR_RETRY_DEFAULT;
    cfg.err_timeout = URMA_JFS_ERROR_TIMEOUT;
    cfg.user_ctx = userCtx;
    if (priority != UINT8_MAX) {
        cfg.priority = priority;
    }

    return cfg;
}

urma_jfr_cfg_t UrmaContext::CreateJfrCfg(uint32_t queueDepth, urma_transport_mode_t transMode, uint32_t tokenValue,
                                         uint64_t userCtx)
{
    urma_jfr_cfg_t cfg{};
    bzero(&cfg, sizeof(urma_jfr_cfg_t));

    if (UNLIKELY(rawContext_ == nullptr || device_ == nullptr)) {
        GEN_LOG_ERROR("Create jfr cfg failed as raw context or device is nullptr");
        return cfg;
    }

    cfg.depth = queueDepth;
    cfg.trans_mode = transMode;
    cfg.max_sge = device_->DeviceAttributes().dev_cap.max_jfr_sge;
    cfg.min_rnr_timer = URMA_JFR_RNR_TIMER_DEFAULT;
    cfg.user_ctx = userCtx;
    cfg.flag.bs.token_policy = URMA_TOKEN_NONE;
    if (tokenValue != 0) {
        cfg.flag.bs.token_policy = URMA_TOKEN_POLICY_PLAIN_TEXT;
        cfg.token_value.token = tokenValue;
    }

    return cfg;
}

urma_jetty_cfg_t UrmaContext::CreateJettyCfg(uint64_t userCtx)
{
    urma_jetty_cfg_t cfg{};
    bzero(&cfg, sizeof(urma_jetty_cfg_t));

    cfg.flag.bs.share_jfr = 1;
    cfg.user_ctx = userCtx;

    return cfg;
}

/* jfce related functions */
UrmaJfce::~UrmaJfce() noexcept
{
    Destroy();
}

void UrmaJfce::Destroy() noexcept
{
    if (rawJfce_ != nullptr) {
        if (LIKELY(UrmaApi::IsLoaded())) {
            auto result = UrmaApi::urma_delete_jfce(rawJfce_);
            if (result != URMA_SUCCESS) {
                GEN_LOG_WARN("[URMA_API] Delete jfce, errno(" << errno << ")");
            }
        }
        rawJfce_ = nullptr;
    }
}

Result UrmaJfce::SetNonBlocking() noexcept
{
    if (UNLIKELY(rawJfce_ == nullptr)) {
        GEN_LOG_ERROR("Failed to set fd of jfce to non blocking as raw jfce is null");
        return GEN_ERROR;
    }

    auto tmp_ctl = fcntl(rawJfce_->fd, F_GETFL);
    if (fcntl(rawJfce_->fd, F_SETFL, static_cast<uint32_t>(tmp_ctl) | O_NONBLOCK) < 0) {
        GEN_LOG_ERROR("fcntl() set jfc fd to non-blocking failed, errno(" << errno << ")");
        return GEN_ERROR;
    }

    GEN_LOG_DEBUG("set fd" << rawJfce_->fd << " of jfce to nonblocking");
    return GEN_OK;
}

urma_jfce_t *UrmaJfce::GetRawJfce() noexcept
{
    return rawJfce_;
}

/* urma jfc related function */
UrmaJfc::~UrmaJfc()
{
    Destroy();
}

void UrmaJfc::DrainEventsBeforeDelete() noexcept
{
    if (rawJfc_ == nullptr || rawJfce_ == nullptr) {
        return;
    }

    /* bounded: defensive against stuck jfc */
    constexpr uint32_t MAX_DRAIN_ROUNDS = 64;
    urma_jfc_t *ev_jfc = nullptr;
    for (uint32_t i = 0; i < MAX_DRAIN_ROUNDS; i++) {
        auto cnt = UrmaApi::urma_wait_jfc(rawJfce_, 1, 0, &ev_jfc);
        if (cnt <= 0) {
            break;
        }
        Ack(1); /* per-event ack */
    }

    /* discard residual completions */
    constexpr int32_t POLL_BATCH_SIZE = 32;
    urma_cr_t crs[POLL_BATCH_SIZE];
    for (uint32_t i = 0; i < MAX_DRAIN_ROUNDS; i++) {
        auto cnt = UrmaApi::urma_poll_jfc(rawJfc_, POLL_BATCH_SIZE, crs);
        if (cnt <= 0) {
            break;
        }
    }
}

void UrmaJfc::Destroy() noexcept
{
    if (LIKELY(UrmaApi::IsLoaded())) {
        if (rawJfc_ != nullptr) {
            DrainEventsBeforeDelete();
            auto result = UrmaApi::urma_delete_jfc(rawJfc_);
            if (result != URMA_SUCCESS) {
                GEN_LOG_WARN("[URMA_API] Delete jfc, errno:" << errno);
            }
        }
    }

    rawJfc_ = nullptr;
}

/* urma jfs function */
UrmaJfs::~UrmaJfs()
{
    Destroy();
}
void UrmaJfs::Destroy() noexcept
{
    if (rawJfs_ != nullptr) {
        if (LIKELY(UrmaApi::IsLoaded())) {
            auto result = UrmaApi::urma_delete_jfs(rawJfs_);
            if (result != URMA_SUCCESS) {
                GEN_LOG_WARN("[URMA_API] Delete jfs failed, errno(" << errno << ")");
            }
        }
        rawJfs_ = nullptr;
    }
}

/* urma jfr function */
UrmaJfr::~UrmaJfr()
{
    Destroy();
}

void UrmaJfr::Destroy() noexcept
{
    if (rawJfr_ != nullptr) {
        if (LIKELY(UrmaApi::IsLoaded())) {
            auto result = UrmaApi::urma_delete_jfr(rawJfr_);
            if (result != URMA_SUCCESS) {
                GEN_LOG_WARN("[URMA_API] Delete jfr, errno(" << errno << ")");
            }
        }
        rawJfr_ = nullptr;
    }
}

/* urma jetty function */
UrmaJetty::~UrmaJetty()
{
    Destroy();
}

void UrmaJetty::Destroy() noexcept
{
    if (LIKELY(UrmaApi::IsLoaded())) {
        /* unbind if trans mode is rc */
        if (rawJetty_ != nullptr && rawJetty_->jetty_cfg.jfs_cfg.trans_mode == URMA_TM_RC &&
            rawTargetJetty_ != nullptr) {
            auto result = UrmaApi::urma_unbind_jetty(rawJetty_);
            if (result != URMA_SUCCESS) {
                GEN_LOG_WARN("[URMA_API] urma_unbind_jetty() failed, errno: %d" << errno);
            }
        }

        /* un-import target jetty if imported */
        if (rawTargetJetty_ != nullptr) {
            auto result = UrmaApi::urma_unimport_jetty(rawTargetJetty_);
            if (result != URMA_SUCCESS) {
                GEN_LOG_WARN("[URMA_API] urma_unimport_jetty() failed, errno: %d" << errno);
            }
        }

        /* delete raw jetty */
        if (rawJetty_ != nullptr) {
            auto result = UrmaApi::urma_delete_jetty(rawJetty_);
            if (result != URMA_SUCCESS) {
                GEN_LOG_WARN("[URMA_API] Delete jetty failed, errno(" << errno << ")");
            }
        }
    }

    rawTargetJetty_ = nullptr;
    rawJetty_ = nullptr;
}

Result UrmaJetty::ImportRemoteJetty(urma_jetty_id_t &jettyId, uint32_t &token) noexcept
{
    GEN_LOG_DEBUG("enter");

    /* step1: verify */
    GEN_ASSERT_RETURN(context_ != nullptr && context_->rawContext_ != nullptr, GEN_ERROR);
    GEN_ASSERT_RETURN(rawJetty_ != nullptr, GEN_ERROR);

    GEN_LOG_DEBUG(*rawJetty_);

    /* step2: create remote jetty and set related property */
    urma_rjetty_t remote_jetty{};
    remote_jetty.type = URMA_JETTY;
    remote_jetty.trans_mode = rawJetty_->jetty_cfg.jfs_cfg.trans_mode;
    remote_jetty.jetty_id = jettyId;

    urma_token_t raw_token{token};
    if (token != 0) {
        remote_jetty.flag.bs.token_policy = URMA_TOKEN_POLICY_PLAIN_TEXT;
    }
    remote_jetty.tp_type = rtp_ctp_utp_;

    /* step3: import jetty and get target jetty */
    auto target_jetty = UrmaApi::urma_import_jetty(context_->rawContext_, &remote_jetty, &raw_token);
    if (target_jetty == nullptr) {
        GEN_LOG_ERROR("Import remote jetty failed, errno " << errno << ", remote " << jettyId);
        return GEN_ERROR;
    }

    /* step4: skip bind if not rc mocde */
    if (rawJetty_->jetty_cfg.jfs_cfg.trans_mode != URMA_TM_RC) {
        rawTargetJetty_ = target_jetty;
        GEN_LOG_DEBUG("imported");
        return GEN_OK;
    }

    /* step5: bind jetty if rc */
    auto result = UrmaApi::urma_bind_jetty(rawJetty_, target_jetty);
    if (result != URMA_SUCCESS) {
        UrmaApi::urma_unimport_jetty(target_jetty);
        GEN_LOG_ERROR("Bind local jetty and remote jetty failed, errno " << errno << ", remote " << jettyId);
        return GEN_ERROR;
    }

    /* step6: set raw target jetty */
    rawTargetJetty_ = target_jetty;
    GEN_LOG_DEBUG("imported and bound");
    return GEN_OK;
}

/* ostream functions */
std::ostream &operator<<(std::ostream &os, const urma_eid_t &o)
{
    char buf[64l]{};
    snprintf(buf, sizeof(buf),
             "%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x:%2.2x%2.2x", o.raw[0],
             o.raw[1], o.raw[2], o.raw[3], o.raw[4], o.raw[5], o.raw[6], o.raw[7], o.raw[8], o.raw[9], o.raw[10],
             o.raw[11], o.raw[12], o.raw[13], o.raw[14], o.raw[15]);
    os << "eid: " << buf;
    return os;
}

std::ostream &operator<<(std::ostream &os, const urma_jetty_id_t &o)
{
    os << "jetty_id [id: " << o.id << ", uasid: " << o.uasid << ", " << o.eid << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const urma_jfc_cfg_t &o)
{
    /* level 0 members */
    os << "jfc cfg [depth: " << o.depth << ", ceqn: " << o.ceqn << ", user ctx: " << o.user_ctx;

    /* urma_jfc_flag_t flag */
    os << ", flag [lock free:" << o.flag.bs.lock_free << ", inline: " << o.flag.bs.jfc_inline
       << ", non-blocking: " << o.flag.bs.non_blocking << "]";

    /* jfce */
    os << ", jfce: " << std::hex << o.jfce << std::dec;
    if (o.jfce != nullptr) {
        os << ", jfce-fd: " << o.jfce->fd;
    }

    /* end */
    os << "]";
    return os;
}
std::ostream &operator<<(std::ostream &os, const urma_jfr_cfg_t &o)
{
    /* level 0 members */
    os << "jfr cfg [id: " << o.id << ", depth: " << o.depth << ", trans mode: " << o.trans_mode
       << ", max sge: " << (uint32_t)o.max_sge << ", min rnr timer: " << (uint32_t)o.min_rnr_timer
       << ", user ctx: " << o.user_ctx << std::hex << ", jfc: " << o.jfc << std::dec;

    /* urma_jfr_flag_t flag */
    os << ", flag [token policy: " << o.flag.bs.token_policy << ", tag matching: " << o.flag.bs.tag_matching
       << ", lock free: " << o.flag.bs.lock_free << ", order type: " << o.flag.bs.order_type
       << ", non blocking: " << o.flag.bs.non_blocking << ", ext: " << o.flag.bs.has_drv_ext << "]";

    /* end */
    os << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const urma_jfs_cfg_t &o)
{
    /* level 0 members */
    os << "jfs cfg [depth: " << o.depth << ", trans mode: " << o.trans_mode << ", priority: " << (uint32_t)o.priority
       << ", max sge: " << (uint32_t)o.max_sge << ", max rsge: " << (uint32_t)o.max_rsge
       << ", max inline data: " << o.max_inline_data << ", rnr retry: " << (uint32_t)o.rnr_retry
       << ", err timeout: " << (uint32_t)o.err_timeout << ", user ctx: " << o.user_ctx << std::hex << ", jfc: " << o.jfc
       << std::dec;

    /* urma_jfs_flag_t flag */
    os << ", flag [lock free: " << o.flag.bs.lock_free << ", error suspend: " << o.flag.bs.error_suspend
       << ", outorder completion: " << o.flag.bs.outorder_comp << ", order type: " << o.flag.bs.order_type
       << ", multi path: " << o.flag.bs.multi_path << ", ctp rc multi mode: " << o.flag.bs.ctp_rc_mul_path_mode
       << ", non blocking: " << o.flag.bs.non_blocking << ", ext: " << o.flag.bs.has_drv_ext << "]";

    /* end */
    os << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const urma_jetty_cfg_t &o)
{
    /* level 0 member */
    os << "jetty cfg [id: " << o.id << std::hex << ", jfc: " << o.shared.jfc << ", jfr: " << o.shared.jfr
       << ", jetty group: " << o.jetty_grp << ", user ctx: " << o.user_ctx << std::dec;

    /* urma_jetty_flag_t flag */
    os << ", flag [shared jfr:" << o.flag.bs.share_jfr << ", non-blocking: " << o.flag.bs.non_blocking
       << ", ext: " << o.flag.bs.has_drv_ext << "]";

    /* jfs cfg and end */
    os << ", " << o.jfs_cfg << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const urma_jetty_t &o)
{
    os << "jetty [" << o.jetty_id << ", " << o.jetty_cfg << ", handle: " << o.handle << ", urma ctx: " << std::hex
       << o.urma_ctx << std::dec << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const UrmaJetty &o)
{
    os << "jetty id: " << o.jettyId_ << std::hex << " raw_jetty: " << o.rawJetty_ << ", context: " << o.context_.Get()
       << ", jfs: " << o.jfs_.Get() << ", jfr: " << o.jfr_.Get() << std::dec;

    if (o.rawJetty_ != nullptr) {
        os << ", " << *(o.rawJetty_);
    }

    os << "]";

    return os;
}
} // namespace urma
} // namespace openDCC
} // namespace ock