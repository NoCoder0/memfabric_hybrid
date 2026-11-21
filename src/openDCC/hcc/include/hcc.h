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
 * @file hcc.h
 * @brief Public header of the HCC module.
 */
#ifndef MF_OPENDCC_HCC_H
#define MF_OPENDCC_HCC_H

#include "hcc_def.h"
#include "hcc_p2p.h"
#include "hcc_cs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set a custom logger callback for HCC (Host Centric Communicator)
 *
 * @param logger           [in] logger callback, level is the log level and msg carries the log message
 */
void hcc_set_log_func(void (*logger)(int32_t level, const char *msg));

/**
 * @brief Set the log level of HCC (Host Centric Communicator), logs below this level are not output
 *
 * @param level            [in] log level, 0:debug 1:info 2:warn 3:error
 */
void hcc_set_log_level(int32_t level);

/**
 * @brief Encode the memory segment into serialized memory info, which should be sent to the peer to decode
 *
 * @param segment          [in] memory segment to be encoded
 * @param flags            [in] optional flags, should be 0 currently
 * @param info             [out] serialized memory info, its data is allocated by this call and should be freed
 *                         by genetty_memory_free_info
 * @return HCC_OK if successful, negative error code if failed
 */
int32_t hcc_memory_info_encode(hcc_memory_segment_t *segment, uint32_t flags, hcc_memory_info_t *info);

/**
 * @brief Decode the serialized memory info exported by the peer into the memory segment
 *
 * @param info             [in] serialized memory info exported by the peer
 * @param segment          [out] memory segment decoded
 * @param flags            [in] optional flags, should be 0 currently
 * @return HCC_OK if successful, negative error code if failed
 */
int32_t hcc_memory_info_decode(hcc_memory_info_t *info, hcc_memory_segment_t *segment, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif // MF_OPENDCC_HCC_H
