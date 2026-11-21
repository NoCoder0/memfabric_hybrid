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
#ifndef MF_GENETTY_LOGGER_H
#define MF_GENETTY_LOGGER_H

#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include "mf_out_logger.h"

namespace ock {
namespace openDCC {
#define GEN_LOG_DEBUG(ARGS)      MF_OUT_LOG("[openDCC ", ock::mf::DEBUG_LEVEL, ARGS)
#define GEN_LOG_INFO(ARGS)       MF_OUT_LOG("[openDCC ", ock::mf::INFO_LEVEL, ARGS)
#define GEN_LOG_WARN(ARGS)       MF_OUT_LOG("[openDCC ", ock::mf::WARN_LEVEL, ARGS)
#define GEN_LOG_WARN_LIMIT(ARGS) MF_OUT_LOG_LIMIT("[openDCC ", ock::mf::WARN_LEVEL, ARGS)
#define GEN_LOG_ERROR(ARGS)      MF_OUT_LOG("[openDCC ", ock::mf::ERROR_LEVEL, ARGS)
#define GEN_LOG_TRACE(ARGS)      MF_OUT_LOG("[openDCC ", ock::mf::TRACE_LEVEL, ARGS)

#define GEN_LOG_ALARM(CODE, ARGS)       MF_ALARM_LOG("[openDCC ", CODE, ARGS)
#define GEN_LOG_ALARM_LIMIT(CODE, ARGS) MF_ALARM_LOG_LIMIT("[openDCC ", CODE, ARGS)
#define GEN_LOG_RESUME(CODE)            MF_RESUME_LOG(CODE)

#define GEN_ASSERT(ARGS)                         \
    do {                                         \
        if (__builtin_expect(!(ARGS), 0) != 0) { \
            GEN_LOG_ERROR("Assert " << #ARGS);   \
            return;                              \
        }                                        \
    } while (0)

#define GEN_ASSERT_RETURN(ARGS, RET)             \
    do {                                         \
        if (__builtin_expect(!(ARGS), 0) != 0) { \
            GEN_LOG_ERROR("Assert " << #ARGS);   \
            return RET;                          \
        }                                        \
    } while (0)

#define GEN_ASSERT_MSG_NO_RETURN(ARGS, msg)      \
    do {                                         \
        if (__builtin_expect(!(ARGS), 0) != 0) { \
            GEN_LOG_ERROR(msg);                  \
            return;                              \
        }                                        \
    } while (0)

#define GEN_ASSERT_MSG_RETURN(ARGS, msg, RET)    \
    do {                                         \
        if (__builtin_expect(!(ARGS), 0) != 0) { \
            GEN_LOG_ERROR(msg);                  \
            return RET;                          \
        }                                        \
    } while (0)

} // namespace openDCC
} // namespace ock

#endif // MF_GENETTY_LOGGER_H
