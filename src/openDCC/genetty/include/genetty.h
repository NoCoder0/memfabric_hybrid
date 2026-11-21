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
#ifndef MF_GENETTY_H
#define MF_GENETTY_H

#include "genetty_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the library of genetty
 *
 * @param prot             [in] protocol to be initialized, see the GEN_PROT_* macros in genetty_def.h
 * @param vendor           [in] vendor of the device to be used, e.g. "ascend"
 * @param flags            [in] optional flags, should be 0 currently
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_init(PROTOCOL prot, const char *vendor, uint32_t flags);

/**
 * @brief Set a custom logger callback for genetty
 *
 * The callback receives the log level and the log message. If it is not set, genetty uses the default
 * unified logger.
 *
 * @param logger           [in] logger callback, level is the log level and file carries the log message,
 *                         can be null to use the default logger
 */
void genetty_set_log_func(void(*logger)(int32_t level, const char *msg));

/**
 * @brief Set the log level of genetty, logs below this level are not output
 *
 * @param level            [in] log level, 0:debug 1:info 2:warn 3:error
 */
void genetty_set_log_level(int32_t level);

/**
 * @brief Un-initialize the library of genetty, all resources are released
 *
 * @param flags            [in] optional flags, should be 0 currently
 */
void genetty_uninit(uint32_t flags);

/**
 * @brief Get the information of all the devices which serve the given protocol
 *
 * @param prot             [in] protocol of the devices, see the GEN_PROT_* macros in genetty_def.h
 * @param device_count     [out] number of the devices found
 * @param device_info      [out] array of the device information, its data is allocated by this call and
 *                         should be freed by genetty_device_info_free
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_get_devices_info(PROTOCOL prot, uint16_t *device_count, genetty_device_info_t **device_info);

/**
 * @brief Free the device information allocated by genetty_get_devices_info
 *
 * @param info             [in] device information to be freed, can be null
 */
void genetty_device_info_free(genetty_device_info_t *info);

/**
 * @brief Get a context by the given ip and mask, the context can be used to create the cq
 *
 * @param type             [in] genetty type of the context, see genetty_type_t in genetty_def.h
 * @param ipMask           [in] ip with mask of the device, e.g. "127.0.0.1/24"
 * @param flags            [in] optional flags, should be 0 currently
 * @return the context if successful, nullptr if failed
 */
genetty_context_t genetty_context_get_by_ip(genetty_type_t type, char *ipMask, uint32_t flags);

/**
 * @brief Get a context by the internal policy, which selects the best device automatically
 *
 * @param type             [in] genetty type of the context, see genetty_type_t in genetty_def.h
 * @param policy           [in] context select policy, GEN_CTX_POLICY_NUMA_AFFINITY | GEN_CTX_POLICY_DEVICE_BALANCED
 * @param flags            [in] optional flags, should be 0 currently
 * @return the context if successful, nullptr if failed
 */
genetty_context_t genetty_context_get_by_policy(genetty_type_t type, CONTEXT_POLICY policy, uint32_t flags);

/**
 * @brief Get a context by the given device id
 *
 * @param type             [in] genetty type of the context, see genetty_type_t in genetty_def.h
 * @param device_id        [in] logical device id for DEVICE_RDMA_GENETTY; otherwise see genetty_device_info_t
 * @param flags            [in] optional flags, should be 0 currently
 * @return the context if successful, nullptr if failed
 */
genetty_context_t genetty_context_get_by_id(genetty_type_t type, uint16_t device_id, uint32_t flags);

/**
 * @brief Return the context created by genetty_context_get_by_ip/policy/id back to the library
 *
 * @param context          [in] the context to be returned
 * @param flags            [in] optional flags, should be 0 currently
 */
void genetty_context_return(genetty_context_t context, uint32_t flags);

/**
 * @brief Set the value of cq options to default value
 *
 * @param options          [in] options to be set
 */
void genetty_cq_options_init(genetty_cq_options_t *options);

/**
 * @brief Create completion queue with options
 *
 * DEVICE_RDMA_GENETTY currently provides a polling CQ bound to one QP. Event notification and
 * device address export are not supported for this CQ.
 *
 * @param options          [in] options to create completion queue
 * @return the handle of the cq if successful, nullptr if failed
 */
genetty_cq_t genetty_cq_create(genetty_cq_options_t *options);

/**
 * @brief Destroy the completion queue created by genetty_cq_create
 *
 * @param cq               [in] cq to be destroyed
 * @param flags            [in] optional flags, should be 0 currently
 */
void genetty_cq_destroy(genetty_cq_t cq, uint32_t flags);

/**
 * @brief Get the event fd of the completion queue, which can be used to wait for the completion events
 *
 * @param cq               [in] the completion queue created by genetty_cq_create
 * @param fd               [out] the event fd of the completion queue
 * @param flags            [in] optional flags, should be 0 currently
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_cq_get_event_fd(genetty_cq_t cq, int32_t *fd, uint32_t flags);

/**
 * @brief Poll the completions of the completion queue, non-blocking
 *
 * @param cq               [in] the completion queue created by genetty_cq_create
 * @param num              [in] max number of the completions to be polled
 * @param wc               [out] array to store the polled completions
 * @param flags            [in] optional flags, should be 0 currently
 * @return the number of the polled completions if successful, negative error code if failed
 */
int32_t genetty_cq_poll(genetty_cq_t cq, int32_t num, genetty_cq_wc_t *wc, uint32_t flags);

/**
 * @brief Poll the completions of the completion queue with the event mechanism, which waits for the completion
 *        events until the given timeout is expired
 *
 * @param cq               [in] the completion queue created by genetty_cq_create
 * @param num              [in] max number of the completions to be polled
 * @param wc               [out] array to store the polled completions
 * @param timeoutMs        [in] timeout of the waiting in milliseconds
 * @param flags            [in] optional flags, should be 0 currently
 * @return the number of the polled completions if successful, negative error code if failed
 */
int32_t genetty_cq_event_poll(genetty_cq_t cq, int32_t num, genetty_cq_wc_t *wc, int32_t timeoutMs, uint32_t flags);

/**
 * @brief Rearm the completion queue, so that the next completion event can be notified
 *
 * @param cq               [in] the completion queue created by genetty_cq_create
 * @param solicited_only   [in] only notify the solicited completions or not
 * @param flags            [in] optional flags, should be 0 currently
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_cq_rearm(genetty_cq_t cq, int32_t solicited_only, uint32_t flags);

/**
 * @brief Ack the completion events of the completion queue
 *
 * @param cq               [in] the completion queue created by genetty_cq_create
 * @param event_count      [in] number of the events to be acked
 * @param flags            [in] optional flags, should be 0 currently
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_cq_ack(genetty_cq_t cq, int32_t event_count, uint32_t flags);

/**
 * @brief Get the information of the completion queue which can be accessed by the device, for the device
 *        centric communication
 *
 * @param cq               [in] the completion queue created by genetty_cq_create
 * @param flags            [in] optional flags, should be 0 currently
 * @param info             [out] device access information of the completion queue
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_cq_get_device_access_info(genetty_cq_t cq, uint32_t flags, genetty_cq_device_access_info_t *info);

/**
 * @brief Set the genetty options to default value
 *
 * @param options          [in] options to be set
 */
void genetty_options_init(genetty_options_t *options);

/**
 * @brief Create genetty with options
 *
 * @param options          [in] options to create genetty
 * @return the handle of the genetty if successful, nullptr if failed
 */
genetty_t genetty_create(genetty_options_t *options);

/**
 * @brief Destroy the genetty created by genetty_create
 *
 * @param handle           [in] the handle of genetty
 * @param flags            [in] optional flags, should be 0 currently
 */
void genetty_destroy(genetty_t handle, uint32_t flags);

/**
 * @brief Export the local link info, which should be sent to the peer to establish the connection
 *
 * @param handle           [in] the handle of genetty
 * @param flags            [in] optional flags, should be 0 currently
 * @param info             [out] link info exported, its data is allocated by this call and should be freed
 *                         by genetty_link_info_free
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_export(genetty_t handle, uint32_t flags, genetty_link_info_t *info);

/**
 * @brief Free the data contained by the link info exported by genetty_export
 *
 * @param info             [in] info to be freed, can be null
 */
void genetty_link_info_free(genetty_link_info_t *info);

/**
 * @brief Import the link info exported by the peer, then this genetty is ready to be used
 *
 * @param handle           [in] the handle of genetty
 * @param info             [in] link info imported, which is exported by the peer
 * @param flags            [in] optional flags, should be 0 currently
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_import(genetty_t handle, genetty_link_info_t *info, uint32_t flags);

/**
 * @brief Import the link info exported by the peer for many-to-many mode, then the target is ready to be used
 *
 * @param handle           [in] the handle of genetty
 * @param info             [in] link info imported, which is exported by the peer
 * @param flags            [in] optional flags, should be 0 currently
 * @return the imported target if successful, nullptr if failed
 */
genetty_target_t genetty_import_m2m(genetty_t handle, genetty_link_info_t *info, uint32_t flags);

/**
 * @brief Get the information of the genetty which can be accessed by the device, for the device centric
 *        communication, such as the address of the send queue and the door bell
 *
 * @param handle           [in] the handle of genetty
 * @param flags            [in] optional flags, should be 0 currently
 * @param info             [out] device access information of the genetty
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_get_device_access_info(genetty_t handle, uint32_t flags, genetty_device_access_info_t *info);

/**
 * @brief Register the local memory, the key is filled into the segment by this call
 *
 * @param ctx              [in] context created by genetty_context_create, which serves the memory
 * @param segment          [in,out] memory segment to be registered, the rkey/lkey are filled by this call
 * @param flags            [in] optional flags, should be 0 currently
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_memory_register(genetty_context_t ctx, genetty_memory_segment_t *segment, uint32_t flags);

/**
 * @brief Unregister the local memory registered by genetty_memory_register
 *
 * @param ctx              [in] context created by genetty_context_create, which serves the memory
 * @param segment          [in] memory segment registered before
 * @param flags            [in] optional flags, should be 0 currently
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_memory_unregister(genetty_context_t ctx, genetty_memory_segment_t *segment, uint32_t flags);

/**
 * @brief Import the memory segment of the peer, then the memory region can be accessed remotely
 *
 * @param context          [in] context created by genetty_context_get_by_*, which serves the memory
 * @param segment          [in] memory segment of the peer, carrying the remote address and keys
 * @param flags            [in] optional flags, should be 0 currently
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_memory_import(genetty_context_t context, genetty_memory_segment_t *segment, uint32_t flags);

/**
 * @brief Encode the memory segment into the serialized memory info, which should be sent to the peer to decode
 *
 * @param segment          [in] memory segment to be encoded
 * @param flags            [in] optional flags, should be 0 currently
 * @param info             [out] serialized memory info, its data is allocated by this call and should be freed
 *                         by genetty_memory_free_info
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_memory_info_encode(genetty_memory_segment_t *segment, uint32_t flags, genetty_memory_info_t *info);

/**
 * @brief Decode the serialized memory info exported by the peer into the memory segment
 *
 * @param info             [in] serialized memory info exported by the peer
 * @param segment          [out] memory segment decoded
 * @param flags            [in] optional flags, should be 0 currently
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_memory_info_decode(genetty_memory_info_t *info, genetty_memory_segment_t *segment, uint32_t flags);

/**
 * @brief Free the data contained by the memory info
 *
 * @param info             [in] info to be freed, can be null
 */
void genetty_memory_free_info(genetty_memory_info_t *info);

/**
 * @brief Import the memory info exported by the peer, then the memory region can be accessed remotely
 *
 * @param handle           [in] context created by genetty_context_get_by_*, which serves the memory
 * @param info             [in] memory info imported, which is exported by the peer
 * @param flags            [in] optional flags, should be 0 currently
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_memory_import_with_info(genetty_context_t handle, genetty_memory_info_t *info, uint32_t flags);

/**
 * @brief Post a batch of read or write operations to this genetty
 *
 * DEVICE_RDMA_GENETTY currently accepts one signaled OP_READ or OP_WRITE with kick_db set to 1.
 * Poll its completion before posting the next operation. The caller must keep both registered
 * memory ranges valid until completion and destroy the QP before unregistering its memory.
 *
 * @param handle           [in] the handle of genetty
 * @param op_data          [in] array of the operations, each item carries the local and remote address
 * @param batchSize        [in] count of the operations in op_data
 * @param options          [in] options of this batch, such as the immediate value and the signal flag
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_batch_post(genetty_t handle, genetty_batch_op_data_t *op_data, uint32_t batchSize,
                           genetty_batch_op_options_t *options);

/**
 * @brief Post a batch of read or write operations to the target in many-to-many mode
 *
 * @param handle           [in] the handle of genetty
 * @param target_handle    [in] the target imported by genetty_import_m2m
 * @param op_data          [in] array of the operations, each item carries the local and remote address
 * @param batchSize        [in] count of the operations in op_data
 * @param options          [in] options of this batch, such as the immediate value and the signal flag
 * @return 0 if successful, negative error code if failed
 */
int32_t genetty_batch_post_m2m(genetty_t handle, genetty_target_t target_handle, genetty_batch_op_data_t *op_data,
                               uint32_t batchSize, genetty_batch_op_options_t *options);

#ifdef __cplusplus
}
#endif

#endif // MF_GENETTY_H
