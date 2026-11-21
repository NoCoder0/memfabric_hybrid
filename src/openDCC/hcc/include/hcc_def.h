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
/**
 * @file hcc_def.h
 * @brief Common definitions shared by the public HCC APIs, such as handle types, error codes and macros.
 */
#ifndef MF_OPENDCC_HCC_DEF_H
#define MF_OPENDCC_HCC_DEF_H

#include "genetty_def.h"

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *hcc_t;                                   /* stands for a Host Centric Communicator */
typedef void *hcc_tunnel_t;                            /* one tunnel for data read/write/write_with_imm */
typedef void *hcc_server_t;                            /* stands for a host centric communicator with server/client */
typedef genetty_memory_segment_t hcc_memory_segment_t; /* memory segment */
typedef hcc_memory_segment_t hcc_mem_segment_t;        /* memory segment, kept for compatibility */
typedef genetty_memory_info_t hcc_memory_info_t;       /* serialized memory info */
typedef genetty_link_info_t hcc_link_info_t;           /* link info*/

typedef enum {
    TM_ONE_2_ONE = 0,
    TM_MANY_2_MANY,
} hcc_mode_t;

typedef struct {
    hcc_mode_t mode; /* mode one2one or many2many */
    uint32_t flags;  /* options flags, reserved for now */
} hcc_options_t;

typedef struct {
    char ip[64];
    uint16_t port;
    uint32_t flags;
} hcc_server_init_options_t;

typedef struct {
    hcc_options_t hcc_options;
} hcc_server_options_t;

typedef struct {
} hcc_tunnel_options_t;

#define HCC_DEVICE_ERROR GEN_DEVICE_ERROR

#define HCC_OK              (0)
#define HCC_ERROR           (-1)
#define HCC_INVALID_PARAM   (-1100)
#define HCC_NOT_INITIALIZED (-1101)
#define HCC_MALLOC_FAILED   (-1102)
#define HCC_NOT_FOUND       (-1003)
#define HCC_POST_FAILED     (-1104)
#define HCC_START_FAILED    (-1105)
#define HCC_CONNECT_FAILED  (-1106)

#ifdef __cplusplus
}
#endif

#endif // MF_OPENDCC_HCC_DEF_H
