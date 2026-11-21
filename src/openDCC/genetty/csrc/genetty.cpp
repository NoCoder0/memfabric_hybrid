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
#include "genetty.h"

#include <cstdio>
#include <cstdlib>

#include "genetty_vendor_type.h"
#include "genetty_info_types.h"
#include "common/genetty_common_includes.h"
#include "verbs/genetty_verbs_backend.h"
#include "urma/genetty_urma_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace ock::openDCC;

static std::mutex GENETTY_MUTEX;
static bool GENETTY_INITED = false;

#define GEN_API_CHECK_INIT_RETURN(RET)                   \
    do {                                                 \
        std::lock_guard<std::mutex> lock(GENETTY_MUTEX); \
        if (!GENETTY_INITED) {                           \
            GEN_LOG_ERROR("Genetty is not inited");      \
            return RET;                                  \
        }                                                \
    } while (0)

int32_t GENETTY_API genetty_init(PROTOCOL prot, const char *vendor, uint32_t flags)
{
    /* step1: validate params */
    GEN_ASSERT_MSG_RETURN(prot != 0, "Genetty init failed, prot is 0", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(vendor != nullptr, "Genetty init failed, vendor is null", GEN_INVALID_PARAM);

    VendorType vendorType = GetVendorType(vendor);
    GEN_ASSERT_MSG_RETURN(vendorType != VT_INVALID, "Genetty init failed, vendor(" << vendor << ") is not supported",
                          GEN_INVALID_PARAM);

    /* step2: create genetty options */
    GenettyBackendOptions options;
    options.protocol = prot;
    options.vendor = vendorType;

    std::lock_guard<std::mutex> lock(GENETTY_MUTEX);
    if (GENETTY_INITED) {
        GEN_LOG_DEBUG("Genetty already inited");
        return GEN_OK;
    }

    /* step3: init the backends which serve the given protocol */
    Result result = verbs::VerbsBackend::Instance().Initialize(options);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Genetty init failed, as init verbs backend failed, result(" << result << "), prot(" << prot
                                                                                   << ")");
        return result;
    }

    result = urma::UrmaBackend::Instance().Initialize(options);
    if (UNLIKELY(result != GEN_OK)) {
        verbs::VerbsBackend::Instance().Destroy();
        GEN_LOG_ERROR("Genetty init failed, as init urma backend failed, result(" << result << "), prot(" << prot
                                                                                  << ")");
        return result;
    }

    /* mark initialized */
    GENETTY_INITED = true;

    GEN_LOG_DEBUG("Genetty init successfully, prot(" << prot << "), flags(" << flags << ")");
    return GEN_OK;
}

void GENETTY_API genetty_set_log_func(void (*logger)(int32_t level, const char *file))
{
    (void)logger;
}

void GENETTY_API genetty_set_log_level(int32_t level)
{
    (void)level;
}

void GENETTY_API genetty_uninit(uint32_t flags)
{
    (void)flags; /* the flags is reserved for now */

    std::lock_guard<std::mutex> lock(GENETTY_MUTEX);
    if (!GENETTY_INITED) {
        GEN_LOG_DEBUG("Genetty not inited");
        return;
    }

    /* step1: destroy backends */
    verbs::VerbsBackend::Instance().Destroy();
    urma::UrmaBackend::Instance().Destroy();

    /* step2: mark uninited */
    GENETTY_INITED = false;
}

int32_t GENETTY_API genetty_get_devices_info(PROTOCOL prot, uint16_t *device_count, genetty_device_info_t **device_info)
{
    /* step1: check param */
    GEN_ASSERT_MSG_RETURN(device_count != nullptr, "Get devices info failed, as count is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(device_info != nullptr, "Get devices info failed, as device info is null", GEN_INVALID_PARAM);

    *device_count = 0;
    *device_info = nullptr;

    /* step2: check inited, dont hold lock here */
    if (UNLIKELY(!GENETTY_INITED)) {
        GEN_LOG_ERROR("Get devices info failed, as genetty is not inited");
        return GEN_NOT_INITIALIZED;
    }

    /* step3: define std::vector<GenettyDeviceInfo> */
    std::vector<GenettyDeviceInfo> infos;

    /* step4: loop all backend based on prot, and aggregate to std::vector */
    if ((prot & (GEN_PROT_HOST_RDMA | GEN_PROT_HOST_URMA)) == 0) {
        GEN_LOG_ERROR("Get devices info failed, as protocol(" << prot << ") is not supported");
        return GEN_INVALID_PARAM;
    }

    if ((prot & GEN_PROT_HOST_RDMA) != 0) {
        auto result = verbs::VerbsBackend::Instance().GetDevicesInfo(infos);
        if (UNLIKELY(result != GEN_OK)) {
            GEN_LOG_ERROR("Get devices info failed, as get verbs devices info failed, result(" << result << ")");
        }
    }

    if ((prot & GEN_PROT_HOST_URMA) != 0) {
        auto result = urma::UrmaBackend::Instance().GetDevicesInfo(infos);
        if (UNLIKELY(result != GEN_OK)) {
            GEN_LOG_ERROR("Get devices info failed, as get urma devices info failed, result(" << result << ")");
        }
    }

    /* step5: malloc memory for device info based on the size of vector */
    /* don't malloc device info if vector is 0 and return GEN_OK */
    if (infos.empty()) {
        return GEN_OK;
    }

    if (UNLIKELY(infos.size() > UINT16_MAX)) {
        GEN_LOG_ERROR("Get devices info failed, as device count(" << infos.size() << ") is out of range");
        return GEN_ERROR;
    }

    auto buffer = static_cast<genetty_device_info_t *>(malloc(infos.size() * sizeof(genetty_device_info_t)));
    if (UNLIKELY(buffer == nullptr)) {
        GEN_LOG_ERROR("Get devices info failed, as malloc " << infos.size() << " device info failed");
        return GEN_MALLOC_FAILED;
    }

    /* step6: fill device_info */
    for (size_t i = 0; i < infos.size(); i++) {
        buffer[i].device_id = infos[i].deviceId;
        buffer[i].port_id = infos[i].portId;
        buffer[i].active = infos[i].active ? 1 : 0;
        snprintf(buffer[i].device_name, sizeof(buffer[i].device_name), "%s", infos[i].deviceName.c_str());
        snprintf(buffer[i].ip, sizeof(buffer[i].ip), "%s", infos[i].ip.c_str());
        buffer[i].type = infos[i].type;
    }

    /* step7: assign device_count and device info */
    *device_count = static_cast<uint16_t>(infos.size());
    *device_info = buffer;

    GEN_LOG_DEBUG("Get devices info finished, protocol(" << prot << "), device count(" << *device_count << ")");
    return GEN_OK;
}

void GENETTY_API genetty_device_info_free(genetty_device_info_t *info)
{
    /* step1: check param */
    if (info == nullptr) {
        GEN_LOG_DEBUG("Free device info skipped, as info is null");
        return;
    }

    /* step2: free memory */
    free(info);
}

genetty_context_t GENETTY_API genetty_context_get_by_ip(genetty_type_t type, char *ipMask, uint32_t flags)
{
    /* step1: check params */
    GEN_ASSERT_MSG_RETURN(ipMask != nullptr, "Get context failed, as ip mask is null, type(" << type << ")", nullptr);

    /* step2: check init or not */
    GEN_API_CHECK_INIT_RETURN(nullptr);

    /* step3: construct GenettyCtxGetOptions */
    GenettyCtxGetOptions options;
    options.genettyType = type;
    options.type = GCCP_BY_IP;
    options.ipMask = ipMask;
    options.flags = flags;

    /* step4: call different backend based on type to get context */
    genetty_context_t context = nullptr;
    Result result = GEN_ERROR;
    if (type == HOST_RDMA_GENETTY || type == DEVICE_RDMA_GENETTY) {
        result = verbs::VerbsBackend::Instance().GetContext(options, &context);
    } else if (type == HOST_URMA_GENETTY) {
        result = urma::UrmaBackend::Instance().GetContext(options, &context);
    } else {
        GEN_LOG_INFO("Get context failed, as type(" << type << ") is not supported, ipMask(" << ipMask << ")");
        return nullptr;
    }

    /* step5: return if get context ok */
    if (result == GEN_OK && context != nullptr) {
        GEN_LOG_DEBUG("Get context(" << context << ") by ip(" << ipMask << ") finished, type(" << type << ")");
        return context;
    }

    GEN_LOG_INFO("Get context ip failed, result(" << result << "), type(" << type << "), ip mask(" << ipMask << ")");
    return nullptr;
}

genetty_context_t GENETTY_API genetty_context_get_by_policy(genetty_type_t type, CONTEXT_POLICY policy, uint32_t flags)
{
    /* step1: check params */
    GEN_ASSERT_MSG_RETURN(
        (policy & ~(GEN_CTX_POLICY_NUMA_AFFINITY | GEN_CTX_POLICY_DEVICE_BALANCED)) == 0,
        "Get context failed, as policy(" << CtxPolicyStr(policy) << ") is invalid, type(" << type << ")", nullptr);

    /* step2: check init or not */
    GEN_API_CHECK_INIT_RETURN(nullptr);

    /* step3: construct GenettyCtxGetOptions */
    GenettyCtxGetOptions options;
    options.genettyType = type;
    options.type = GCCP_AUTO;
    options.flags = flags;
    options.whichAutoPolicy = policy;

    /* step4: call different backend based on type to get context */
    genetty_context_t context = nullptr;
    Result result = GEN_ERROR;
    if (type == HOST_RDMA_GENETTY || type == DEVICE_RDMA_GENETTY) {
        result = verbs::VerbsBackend::Instance().GetContext(options, &context);
    } else if (type == HOST_URMA_GENETTY) {
        result = urma::UrmaBackend::Instance().GetContext(options, &context);
    } else {
        GEN_LOG_INFO("Get context failed, as type(" << type << ") is not supported, policy(" << CtxPolicyStr(policy)
                                                    << ")");
        return nullptr;
    }

    /* step5: return if get context ok */
    if (result == GEN_OK && context != nullptr) {
        GEN_LOG_DEBUG("Get context(" << context << ") by policy(" << CtxPolicyStr(policy) << ") finished, type(" << type
                                     << ")");
        return context;
    }

    GEN_LOG_INFO("Get context policy failed, result(" << result << "), type(" << type << "), policy("
                                                      << CtxPolicyStr(policy) << ")");
    return nullptr;
}

genetty_context_t GENETTY_API genetty_context_get_by_id(genetty_type_t type, uint16_t device_id, uint32_t flags)
{
    /* step1: check params */
    GEN_ASSERT_MSG_RETURN(device_id != UINT16_MAX, "Get context failed, as device id is invalid, type(" << type << ")",
                          nullptr);

    /* step2: check init or not */
    GEN_API_CHECK_INIT_RETURN(nullptr);

    /* step3: construct GenettyCtxGetOptions */
    GenettyCtxGetOptions options;
    options.genettyType = type;
    options.type = GCCP_BY_DEVICE_ID;
    options.deviceId = device_id;
    options.flags = flags;

    /* step4: call different backend based on type to get context */
    genetty_context_t context = nullptr;
    Result result = GEN_ERROR;
    if (type == HOST_RDMA_GENETTY || type == DEVICE_RDMA_GENETTY) {
        result = verbs::VerbsBackend::Instance().GetContext(options, &context);
    } else if (type == HOST_URMA_GENETTY) {
        result = urma::UrmaBackend::Instance().GetContext(options, &context);
    } else {
        GEN_LOG_INFO("Get context failed, as type(" << type << ") is not supported, device id(" << device_id << ")");
        return nullptr;
    }

    /* step5: return if get context ok */
    if (result == GEN_OK && context != nullptr) {
        GEN_LOG_DEBUG("Get context(" << context << ") by device id(" << device_id << ") finished, type(" << type
                                     << ")");
        return context;
    }

    GEN_LOG_INFO("Get context device id failed, result(" << result << "), type(" << type << "), device id(" << device_id
                                                         << ")");
    return nullptr;
}

void GENETTY_API genetty_context_return(genetty_context_t context, uint32_t flags)
{
    /* step1: check param */
    GEN_ASSERT_MSG_NO_RETURN(context != nullptr, "Return context failed, as context is null");

    /* step2: check init */
    GEN_API_CHECK_INIT_RETURN();

    /* step3: convert to GenettyContext type */
    auto genettyCtx = reinterpret_cast<GenettyContext *>(context);

    /* step4: get type and call related backend to do release */
    auto type = genettyCtx->Type();
    if (type == HOST_RDMA_GENETTY || type == DEVICE_RDMA_GENETTY) {
        verbs::VerbsBackend::Instance().ReturnContext(&context, flags);
        return;
    }

    if (type == HOST_URMA_GENETTY) {
        urma::UrmaBackend::Instance().ReturnContext(&context, flags);
        return;
    }

    GEN_LOG_ERROR("Return context failed, as type(" << type << ") is not supported");
}

void GENETTY_API genetty_cq_options_init(genetty_cq_options_t *options)
{
    GEN_ASSERT_MSG_NO_RETURN(options != nullptr, "Invalid param, options is null");

    options->context = nullptr;
    options->event_fd = UINT32_MAX;
    options->queue_depth = 16384;
    options->flags = 0;
    options->lock_free = 0;
    options->preferred_ip_mask[0] = '\0';
}

genetty_cq_t GENETTY_API genetty_cq_create(genetty_cq_options_t *options)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(options != nullptr, "Create cq failed, as options is null", nullptr);

    /* step2: check inited */
    GEN_API_CHECK_INIT_RETURN(nullptr);

    /* step3: create the cq by the backend which owns the context */
    GEN_ASSERT_MSG_RETURN(options->context != nullptr, "Create cq failed, as context is null", nullptr);

    auto context = reinterpret_cast<GenettyContext *>(options->context);
    if (context->Type() == HOST_RDMA_GENETTY || context->Type() == DEVICE_RDMA_GENETTY) {
        return verbs::VerbsBackend::Instance().GenettyCqCreate(options);
    }

    if (context->Type() == HOST_URMA_GENETTY) {
        return urma::UrmaBackend::Instance().GenettyCqCreate(options);
    }

    GEN_LOG_ERROR("Create cq failed, as context type(" << context->Type() << ") is not supported");
    return nullptr;
}

void GENETTY_API genetty_cq_destroy(genetty_cq_t cq, uint32_t flags)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_NO_RETURN(cq != nullptr, "Destory cq failed, as cq is null");

    /* step2: check inited */
    GEN_API_CHECK_INIT_RETURN();

    /* step3: do destroy */
    auto result = verbs::VerbsBackend::Instance().GenettyCqDestroy(cq, flags);
    if (result == GEN_OK) {
        return;
    }

    result = urma::UrmaBackend::Instance().GenettyCqDestroy(cq, flags);
}

int32_t GENETTY_API genetty_cq_get_event_fd(genetty_cq_t cq, int32_t *fd, uint32_t flags)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(cq != nullptr, "Get cq event fd failed, as cq is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(fd != nullptr, "Get cq event fd failed, as fd is null", GEN_INVALID_PARAM);

    /* step2: check inited, dont hold lock here */
    GEN_API_CHECK_INIT_RETURN(GEN_NOT_INITIALIZED);

    /* step3: get the event fd through the cq object, only the host rdma cq is supported now */
    return reinterpret_cast<GenettyCQ *>(cq)->GetEventFd(fd, flags);
}

int32_t GENETTY_API genetty_cq_poll(genetty_cq_t cq, int32_t num, genetty_cq_wc_t *wc, uint32_t flags)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(cq != nullptr, "Poll cq failed, as cq is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(wc != nullptr, "Poll cq failed, as wc is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(num > 0, "Poll cq failed, as num is 0", GEN_INVALID_PARAM);

    /* step2: check inited, dont hold lock here */
    if (UNLIKELY(!GENETTY_INITED)) {
        GEN_LOG_ERROR("Poll cq failed, as cq is not inited");
        return GEN_NOT_INITIALIZED;
    }

    /* step3: poll the cq through the cq object, only the host rdma cq is supported now */
    return reinterpret_cast<GenettyCQ *>(cq)->Poll(num, wc, flags);
}

int32_t GENETTY_API genetty_cq_rearm(genetty_cq_t cq, int32_t solicited_only, uint32_t flags)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(cq != nullptr, "Rearm cq failed, as cq is null", GEN_INVALID_PARAM);

    /* step2: check inited, dont hold lock here */
    if (UNLIKELY(!GENETTY_INITED)) {
        GEN_LOG_ERROR("Rearm cq failed, as cq is not inited");
        return GEN_NOT_INITIALIZED;
    }

    /* step3: rearm the cq through the cq object, only the host rdma cq is supported now */
    return reinterpret_cast<GenettyCQ *>(cq)->Rearm(solicited_only, flags);
}

int32_t GENETTY_API genetty_cq_ack(genetty_cq_t cq, int32_t event_count, uint32_t flags)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(cq != nullptr, "Ack cq failed, as cq is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(event_count >= 0, "Ack cq failed, as event count is negative", GEN_INVALID_PARAM);

    /* step2: check inited, dont hold lock here */
    if (UNLIKELY(!GENETTY_INITED)) {
        GEN_LOG_ERROR("Ack cq failed, as cq is not inited");
        return GEN_NOT_INITIALIZED;
    }

    /* step3: ack the cq through the cq object, only the host rdma cq is supported now */
    return reinterpret_cast<GenettyCQ *>(cq)->Ack(event_count, flags);
}

int32_t GENETTY_API genetty_cq_get_device_access_info(genetty_cq_t cq, uint32_t flags,
                                                      genetty_cq_device_access_info_t *info)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(cq != nullptr, "Get cq device access info failed, as cq is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Get cq device access info failed, as info is null", GEN_INVALID_PARAM);

    /* step2: check inited, dont hold lock here */
    GEN_API_CHECK_INIT_RETURN(GEN_NOT_INITIALIZED);

    /* step3: get the info through the cq object */
    return reinterpret_cast<GenettyCQ *>(cq)->GetDeviceAccessInfo(flags, info);
}

void GENETTY_API genetty_options_init(genetty_options_t *options)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_NO_RETURN(options != nullptr, "Invalid param, as options is null");

    options->cq_handle = nullptr;
    options->sq_depth = 4096;
    options->rq_depth = 4096;
    options->lock_free = 0;
    options->flags = 0;
    options->service_level = 0;
    options->traffic_class = 0;
}

genetty_t GENETTY_API genetty_create(genetty_options_t *options)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(options != nullptr, "Create genetty failed, as options is null", nullptr);

    /* step2: check inited */
    GEN_API_CHECK_INIT_RETURN(nullptr);

    /* step3: create the genetty by the backend which owns the cq */
    GEN_ASSERT_MSG_RETURN(options->cq_handle != nullptr, "Create genetty failed, as cq handle is null", nullptr);

    auto cq = reinterpret_cast<GenettyCQ *>(options->cq_handle);
    if (cq->Type() == HOST_RDMA_GENETTY || cq->Type() == DEVICE_RDMA_GENETTY) {
        return verbs::VerbsBackend::Instance().GenettyCreate(options);
    }

    if (cq->Type() == HOST_URMA_GENETTY) {
        return urma::UrmaBackend::Instance().GenettyCreate(options);
    }

    GEN_LOG_ERROR("Create genetty failed, as cq type(" << cq->Type() << ") is not supported");
    return nullptr;
}

void GENETTY_API genetty_destroy(genetty_t handle, uint32_t flags)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_NO_RETURN(handle != nullptr, "Destory genetty failed, as handle is null");

    /* step2: check inited */
    GEN_API_CHECK_INIT_RETURN();

    /* step3: do destroy */
    auto result = verbs::VerbsBackend::Instance().GenettyDestroy(handle, flags);
    if (result == GEN_OK) {
        return;
    }

    urma::UrmaBackend::Instance().GenettyDestroy(handle, flags);
}

int32_t GENETTY_API genetty_export(genetty_t handle, uint32_t flags, genetty_link_info_t *info)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(handle != nullptr, "Export genetty failed, as handle is null", GEN_INVALID_PARAM);

    /* step2: check inited */
    GEN_API_CHECK_INIT_RETURN(GEN_INVALID_PARAM);

    /* step3: export link info */
    auto genetty = reinterpret_cast<Genetty *>(handle);
    return genetty->Export(flags, info);
}

void GENETTY_API genetty_link_info_free(genetty_link_info_t *info)
{
    if (info == nullptr) {
        GEN_LOG_DEBUG("Invalid param, info is null");
        return;
    }

    if (info->data == nullptr) {
        GEN_LOG_DEBUG("Invalid param, info->data is null");
        return;
    }

    free(info->data);
}

int32_t GENETTY_API genetty_import(genetty_t handle, genetty_link_info_t *info, uint32_t flags)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(handle != nullptr, "Import genetty failed, as handle is null", GEN_INVALID_PARAM);

    /* step2: check inited */
    GEN_API_CHECK_INIT_RETURN(GEN_NOT_INITIALIZED);

    /* step3: import link info */
    auto genetty = reinterpret_cast<Genetty *>(handle);
    return genetty->Import(flags, info);
}

genetty_target_t GENETTY_API genetty_import_m2m(genetty_t handle, genetty_link_info_t *info, uint32_t flags)
{
    (void)info;

    GEN_LOG_ERROR("Import m2m is not implemented yet, handle(" << handle << "), flags(" << flags << ")");
    return nullptr;
}

int32_t GENETTY_API genetty_get_device_access_info(genetty_t handle, uint32_t flags, genetty_device_access_info_t *info)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(handle != nullptr, "Get genetty device access info failed, as handle is null",
                          GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Get genetty device access info failed, as info is null", GEN_INVALID_PARAM);

    /* step2: check inited, dont hold lock here */
    GEN_API_CHECK_INIT_RETURN(GEN_NOT_INITIALIZED);

    /* step3: get the info through the genetty object */
    return reinterpret_cast<Genetty *>(handle)->GetDeviceAccessInfo(flags, info);
}

int32_t GENETTY_API genetty_memory_register(genetty_context_t ctx, genetty_memory_segment_t *segment, uint32_t flags)
{
    /* step1: check param */
    GEN_ASSERT_MSG_RETURN(ctx != nullptr, "Register memory failed, as context is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(segment != nullptr, "Register memory failed, as segment is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(segment->type > MT_INVALID && segment->type < MT_COUNT,
                          "Register memory failed, as mem type(" << segment->type << ") is invalid", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(segment->address != 0, "Register memory failed, as address is 0", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(segment->size != 0, "Register memory failed, as size is 0", GEN_INVALID_PARAM);

    /* step2: check init */
    GEN_API_CHECK_INIT_RETURN(GEN_NOT_INITIALIZED);

    /* step3: convert to GenettyContext */
    auto genettyCtx = reinterpret_cast<GenettyContext *>(ctx);

    /* step4: register the memory by the backend */
    auto result = genettyCtx->RegisterMemory(segment, flags);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Register memory failed, as backend register failed, result("
                      << result << "), mem type(" << segment->type << "), address(" << segment->address << "), size("
                      << segment->size << ")");
        return result;
    }

    GEN_LOG_DEBUG("Register memory finished, mem type(" << segment->type << "), address(" << segment->address
                                                        << "), size(" << segment->size << "), lkey(" << segment->lkey
                                                        << ")");
    return GEN_OK;
}

int32_t GENETTY_API genetty_memory_unregister(genetty_context_t ctx, genetty_memory_segment_t *segment, uint32_t flags)
{
    /* step1: check param */
    GEN_ASSERT_MSG_RETURN(ctx != nullptr, "Unregister memory failed, as context is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(segment != nullptr, "Unregister memory failed, as segment is null", GEN_INVALID_PARAM);

    /* step2: check init */
    GEN_API_CHECK_INIT_RETURN(GEN_NOT_INITIALIZED);

    /* step3: convert to GenettyContext */
    auto genettyCtx = reinterpret_cast<GenettyContext *>(ctx);

    /* step4: unregister the memory by the backend */
    auto result = genettyCtx->UnRegisterMemory(segment, flags);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Unregister memory failed, as backend unregister failed, result("
                      << result << "), mem type(" << segment->type << "), address(" << segment->address << ")");
        return result;
    }

    GEN_LOG_DEBUG("Unregister memory finished, mem type(" << segment->type << "), address(" << segment->address << ")");
    return 0;
}

void GENETTY_API genetty_memory_free_info(genetty_memory_info_t *info)
{
    if (info == nullptr) {
        GEN_LOG_DEBUG("Invalid param, info is null");
        return;
    }

    if (info->data == nullptr) {
        GEN_LOG_DEBUG("Invalid param, info->data is null");
        return;
    }

    free(info->data);
}

int32_t GENETTY_API genetty_memory_import_with_info(genetty_context_t handle, genetty_memory_info_t *info,
                                                    uint32_t flags)
{
    /* step1: check param */
    GEN_ASSERT_MSG_RETURN(handle != nullptr, "Import memory failed, as context is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(info != nullptr, "Import memory failed, as info is null", GEN_INVALID_PARAM);

    /* step2: check init */
    GEN_API_CHECK_INIT_RETURN(GEN_NOT_INITIALIZED);

    /* step3: decode the memory info into the memory segment */
    genetty_memory_segment_t segment{};
    auto result = GenettyBackend::MemInfoDecode(info, &segment, flags);
    if (UNLIKELY(result != GEN_OK)) {
        GEN_LOG_ERROR("Import memory failed, as decode memory info failed, result(" << result << "), length("
                                                                                    << info->length << ")");
        return result;
    }

    /* step4: import the segment by the context */
    auto genettyCtx = reinterpret_cast<GenettyContext *>(handle);
    return genettyCtx->ImportMemory(&segment, flags);
}

int32_t GENETTY_API genetty_memory_import(genetty_context_t context, genetty_memory_segment_t *segment, uint32_t flags)
{
    /* step1: check param */
    GEN_ASSERT_MSG_RETURN(context != nullptr, "Import memory failed, as context is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(segment != nullptr, "Import memory failed, as segment is null", GEN_INVALID_PARAM);

    /* step2: check init */
    GEN_API_CHECK_INIT_RETURN(GEN_NOT_INITIALIZED);

    /* step3: convert to GenettyContext and import the segment by the backend */
    auto genettyCtx = reinterpret_cast<GenettyContext *>(context);
    return genettyCtx->ImportMemory(segment, flags);
}

int32_t GENETTY_API genetty_memory_info_encode(genetty_memory_segment_t *segment, uint32_t flags,
                                               genetty_memory_info_t *info)
{
    return GenettyBackend::MemInfoEncode(segment, flags, info);
}

int32_t GENETTY_API genetty_memory_info_decode(genetty_memory_info_t *info, genetty_memory_segment_t *segment,
                                               uint32_t flags)
{
    return GenettyBackend::MemInfoDecode(info, segment, flags);
}

int32_t GENETTY_API genetty_batch_post(genetty_t handle, genetty_batch_op_data_t *op_data, uint32_t batchSize,
                                       genetty_batch_op_options_t *options)
{
    /* step1: check the parameters */
    GEN_ASSERT_MSG_RETURN(handle != nullptr, "Batch post failed, as handle is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(op_data != nullptr, "Batch post failed, as op data is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(options != nullptr, "Batch post failed, as options is null", GEN_INVALID_PARAM);
    GEN_ASSERT_MSG_RETURN(batchSize != 0, "Batch post failed, as batch size is 0", GEN_INVALID_PARAM);

    /* step2: post the batch through the genetty object */
    auto genetty = reinterpret_cast<Genetty *>(handle);
    return genetty->BatchPost(op_data, batchSize, options);
}

int32_t GENETTY_API genetty_batch_post_m2m(genetty_t handle, genetty_target_t target_handle,
                                           genetty_batch_op_data_t *op_data, uint32_t batchSize,
                                           genetty_batch_op_options_t *options)
{
    (void)op_data;
    (void)options;

    GEN_LOG_ERROR("Batch post m2m is not implemented yet, handle(" << handle << "), target(" << target_handle
                                                                   << "), batch size(" << batchSize << ")");
    return GEN_ERROR;
}

#ifdef __cplusplus
}
#endif