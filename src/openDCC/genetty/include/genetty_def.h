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
#ifndef MF_GENETTY_DEF_H
#define MF_GENETTY_DEF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *genetty_context_t; /* context created from device, for both Verbs@RoCE and URMA@UB etc */
typedef void *genetty_cq_t;      /* completion queue for both verbs@RoCE and URMA@UB etc */
typedef void *genetty_t;         /* stands for QP@verbs or Jetty@URMA */
typedef void *genetty_target_t;  /* stands for imported Jetty for URMA@UB, and only for many-to-many mode */

typedef uint32_t PROTOCOL;

#define GEN_PROT_HOST_RDMA             1 << 0
#define GEN_PROT_HOST_URMA             1 << 1
#define GEN_PROT_DEVICE_RDMA           1 << 2
#define GEN_PROT_DEVICE_URMA           1 << 3
#define GEN_PROT_DEVICE_INITIATED_RDMA 1 << 4
#define GEN_PROT_DEVICE_INITIATED_URMA 1 << 5

typedef uint32_t CONTEXT_POLICY;

#define GEN_CTX_POLICY_NUMA_AFFINITY   1 << 0 /* choose the affinity context according to current CPU */
#define GEN_CTX_POLICY_DEVICE_BALANCED 1 << 1 /* balancing among device(s) */

typedef enum {
    MT_INVALID = 0,
    MT_HOST_DRAM,
    MT_DEVICE_HBM,

    MT_COUNT
} genetty_mem_type_t;

typedef enum {
    GEN_TYPE_INVALID = 0,
    HOST_RDMA_GENETTY,             /* host rdma genetty */
    HOST_URMA_GENETTY,             /* host urma genetty */
    DEVICE_RDMA_GENETTY,           /* device rdma genetty */
    DEVICE_URMA_GENETTY,           /* device rdma_genetty */
    DEVICE_INITIATED_RDMA_GENETTY, /* device initated rdma genetty */
    DEVICE_INITIATED_URMA_GENETTY, /* device initated urma genetty */

    GENETTY_TYPE_COUNT,
} genetty_type_t;

typedef struct {
    uint16_t device_id;   /* logic device id */
    uint8_t port_id;      /* eid index | port number */
    uint8_t active;       /* active or not */
    char device_name[64]; /* name of device */
    char ip[64];          /* mapped ip if exsits */
    genetty_type_t type;  /* genetty type of this device, see genetty_type_t */
} genetty_device_info_t;

typedef struct {
    genetty_context_t context;   /* context created by genetty_context_create, required to create the cq */
    uint32_t event_fd;           /* external created event fd, UINT32_MAX stand of event create event internally */
    uint32_t queue_depth;        /* cq depth */
    uint32_t flags;              /* flags */
    uint8_t lock_free;           /* lock free or not, 0 by default, used for host urma */
    char preferred_ip_mask[256]; /* prefer IP and mask */
} genetty_cq_options_t;

typedef struct {
    genetty_cq_t cq_handle; /* cq handle created by 'genetty_cq_create' */
    uint32_t sq_depth;      /* send queue depth */
    uint32_t rq_depth;      /* receive queue depth */
    uint8_t lock_free;      /* lock free or not, 0 by default, used for host urma */
    uint32_t flags;         /* flags */
    union {
        uint32_t service_level; /* service level, for UB */
        uint32_t traffic_class; /* traffic class, for RoCE */
    };
} genetty_options_t;

/* info of genetty cq can be accessed by device, for device centric communication  */
typedef struct {
    uint64_t address; /* address */
} genetty_cq_device_access_info_t;

/* info of genetty can be accessed by device, for device centric communication */
typedef struct {
    uint64_t sq_address; /* address of send queue */
    uint64_t db_address; /* address of door bell */
    uint16_t sq_depth;   /* depth of send queue */
    uint16_t rq_depth;   /* depth of receive queue */
    uint32_t flags;      /* optional flags */
} genetty_device_access_info_t;

typedef struct {
    char *data;      /* sererialized data */
    uint32_t length; /* length of serialized data */
} genetty_link_info_t;

typedef struct {
    genetty_mem_type_t type; /* DRAM or HBM */
    uint64_t address;        /* virtual address */
    uint64_t size;           /* size */
    uint32_t lkey;           /* local key, got after registered */
    uint32_t rkey;           /* remote key, got from the peer memory info */
} genetty_memory_segment_t;

typedef struct {
    char *data;      /* serialized data */
    uint32_t length; /* length of serialized data */
} genetty_memory_info_t;

typedef enum {
    OP_READ = 0,
    OP_WRITE,
    OP_WRITE_IMM,

    OP_COUNT,
} genetty_op_type_t;

typedef struct {
    uint64_t usr_ctx;          /* user context */
    uint64_t imm;              /* value of imm */
    genetty_op_type_t op_type; /* read/write/write_with_imm */
    uint8_t signal;            /* trigger finish event or not */
    uint8_t kick_db;           /* kick the doorbell or not */
    uint16_t flags;            /* reserved flags */
} genetty_batch_op_options_t;

typedef struct {
    void *laddress; /* local address of data */
    uint32_t lsize; /* size of local data */
    uint32_t lkey;  /* local key */
    void *raddress; /* remote address of data */
    uint32_t rsize; /* size of remote */
    uint32_t rkey;  /* remote key */
} genetty_batch_op_data_t;

typedef enum {
    OPS_SUCCESS = 0,
    OPS_ERROR,

    OPS_COUNT
} genetty_op_status_t;

typedef struct {
    genetty_op_status_t status; /* status */
    genetty_op_type_t type;     /* op type */
    uint64_t genetty_id;        /* genetty id */
    uint64_t usr_ctx;           /* user context */
} genetty_cq_wc_t;

#define GEN_OK                  (0)
#define GEN_ERROR               (-1)
#define GEN_INVALID_PARAM       (-1000)
#define GEN_LOAD_LIBRARY_FAILED (-1001)
#define GEN_MALLOC_FAILED       (-1002)
#define GEN_DEVICE_ERROR        (-1003)
#define GEN_NOT_INITIALIZED     (-1004)
#define GEN_NOT_FOUND           (-1005)
#define GEN_POST_FAILED         (-1006)

#ifdef __cplusplus
}
#endif

#endif // MF_GENETTY_DEF_H
