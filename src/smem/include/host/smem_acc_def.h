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
#ifndef MEMFABRIC_SMEM_ACC_DEF_H
#define MEMFABRIC_SMEM_ACC_DEF_H

#include <stddef.h>
#include <stdint.h>

#include "smem_tls_options.h"

typedef void *smem_acc_server_t;
typedef void *smem_acc_link_t;
typedef void *smem_acc_http_request_t;

typedef struct {
    const char *listen_ip;
    uint16_t listen_port;
    uint16_t worker_count;
    uint16_t link_send_queue_size;
    int16_t magic;
    int16_t version;
    uint8_t enable_listener;
} smem_acc_tcp_server_options_t;

typedef struct {
    const char *listen_ip;
    uint16_t listen_port;
    uint16_t worker_count;
    size_t max_body_size;
} smem_acc_http_server_options_t;

typedef struct {
    int16_t magic;
    int16_t version;
    uint8_t reconnect;
    uint64_t rank_id;
} smem_acc_connect_request_t;

typedef struct {
    int16_t type;
    int16_t result;
    uint32_t body_len;
    uint32_t seq_no;
} smem_acc_message_t;

typedef struct {
    int16_t type;
    int16_t result;
    uint32_t body_len;
    uint32_t seq_no;
    uint32_t crc;
} smem_acc_message_v2_t;

/**
 * @brief Handle an incoming TCP connection.
 *
 * @param context          [in] user context supplied when registering the handler
 * @param request          [in] borrowed handshake information, valid only during the callback
 * @param link             [in] borrowed link handle, retain it before keeping it beyond the callback
 * @return 0 if accepted, or an ACC error code on failure
 */
typedef int32_t (*smem_acc_new_link_cb)(void *context, const smem_acc_connect_request_t *request, smem_acc_link_t link);

/**
 * @brief Handle a received TCP message.
 *
 * @param context          [in] user context supplied when registering the handler
 * @param message          [in] borrowed message header, valid only during the callback
 * @param data             [in] borrowed payload, valid only during the callback
 * @param data_len         [in] payload length in bytes
 * @param link             [in] borrowed link handle, retain it before keeping it beyond the callback
 * @return 0 if successful, or an ACC error code on failure
 */
typedef int32_t (*smem_acc_request_cb)(void *context, const smem_acc_message_t *message, const void *data,
                                       uint32_t data_len, smem_acc_link_t link);

/**
 * @brief Handle completion of a TCP send operation.
 *
 * @param context          [in] user context supplied when registering the handler
 * @param result           [in] send result, 0:sent, 1:timeout, 2:link broken
 * @param message          [in] borrowed message header, valid only during the callback
 * @return 0 if successful, or an ACC error code on failure
 */
typedef int32_t (*smem_acc_sent_cb)(void *context, int32_t result, const smem_acc_message_t *message);

/**
 * @brief Handle a received TCP message with an extended header.
 *
 * @param context          [in] user context supplied when registering the handler
 * @param message          [in] borrowed header including the CRC field, valid only during the callback
 * @param data             [in] borrowed payload, valid only during the callback
 * @param data_len         [in] payload length in bytes
 * @param link             [in] borrowed link handle, retain it before keeping it beyond the callback
 * @return 0 if successful, or an ACC error code on failure
 */
typedef int32_t (*smem_acc_request_cb_v2)(void *context, const smem_acc_message_v2_t *message, const void *data,
                                          uint32_t data_len, smem_acc_link_t link);

/**
 * @brief Handle completion of a TCP send operation with local callback data.
 *
 * @param context          [in] user context supplied when registering the handler
 * @param result           [in] send result, 0:sent, 1:timeout, 2:link broken
 * @param message          [in] borrowed extended header, valid only during the callback
 * @param callback_data    [in] borrowed local data supplied by smem_acc_link_send_v2, or NULL if absent
 * @param callback_data_len [in] callback data length in bytes; the data is valid only during the callback
 * @return 0 if successful, or an ACC error code on failure
 */
typedef int32_t (*smem_acc_sent_cb_v2)(void *context, int32_t result, const smem_acc_message_v2_t *message,
                                       const void *callback_data, uint32_t callback_data_len);

/**
 * @brief Handle a broken TCP link.
 *
 * @param context          [in] user context supplied when registering the handler
 * @param link             [in] borrowed link handle, retain it before keeping it beyond the callback
 * @return 0 if successful, or an ACC error code on failure
 */
typedef int32_t (*smem_acc_link_broken_cb)(void *context, smem_acc_link_t link);

/**
 * @brief Handle an HTTP request.
 *
 * Read request data and send a response with the smem_acc_http_request_* APIs during this callback.
 *
 * @param context          [in] user context supplied when registering the handler
 * @param request          [in] borrowed request handle, valid only during the callback; do not free it
 * @return 0 if successful, or an ACC error code on failure
 */
typedef int32_t (*smem_acc_http_cb)(void *context, smem_acc_http_request_t request);

/**
 * @brief Visit one HTTP request parameter.
 *
 * @param context          [in] user context supplied to smem_acc_http_request_visit_params
 * @param name             [in] borrowed null-terminated name, valid only during the callback
 * @param value            [in] borrowed null-terminated value, valid only during the callback
 */
typedef void (*smem_acc_http_param_cb)(void *context, const char *name, const char *value);

/**
 * @brief Decrypt a TLS private key password.
 *
 * @param context          [in] user context supplied when registering the handler
 * @param cipher_text      [in] encrypted private key password
 * @param cipher_text_len  [in] encrypted text length in bytes
 * @param plain_text       [out] caller-provided buffer for the decrypted password
 * @param plain_text_len   [in] available output buffer size in bytes
 * @return 0 if successful, or an error code on failure
 */
typedef int32_t (*smem_decrypt_cb)(void *context, const char *cipher_text, size_t cipher_text_len, uint8_t *plain_text,
                                   size_t plain_text_len);

#endif // MEMFABRIC_SMEM_ACC_DEF_H
