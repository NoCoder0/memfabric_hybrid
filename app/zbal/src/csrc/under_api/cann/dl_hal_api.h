/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef ZBAL_DL_HAL_API_H
#define ZBAL_DL_HAL_API_H

#include <cstdint>

namespace zbal {
namespace operators {

#define SQCQ_QUERY_INFO_LENGTH  8
#define SQCQ_CONFIG_INFO_LENGTH 8

enum drvSqCqType { DRV_NORMAL_TYPE = 0 };

enum drvSqCqPropType {
    DRV_SQCQ_PROP_SQ_STATUS = 0x0,
    DRV_SQCQ_PROP_SQ_HEAD = 1,
    DRV_SQCQ_PROP_SQ_TAIL = 2,
    DRV_SQCQ_PROP_SQ_REG_BASE = 5,
    DRV_SQCQ_PROP_SQ_BASE = 6,
    DRV_SQCQ_PROP_SQ_DEPTH = 7,
};

struct halSqCqQueryInfo {
    uint32_t type, tsId, sqId, cqId, prop;
    uint32_t value[SQCQ_QUERY_INFO_LENGTH];
};

struct halSqCqConfigInfo {
    uint32_t type, tsId, sqId, cqId, prop;
    uint32_t value[SQCQ_CONFIG_INFO_LENGTH];
};

using HalSqCqQueryFunc = int (*)(uint32_t devId, struct halSqCqQueryInfo *info);
using HalSqCqConfigFunc = int (*)(uint32_t devId, struct halSqCqConfigInfo *info);

class DlHalApi {
public:
    static bool Load();
    static int HalSqCqQuery(uint32_t devId, struct halSqCqQueryInfo *info);
    static int HalSqCqConfig(uint32_t devId, struct halSqCqConfigInfo *info);

private:
    static void *libHandle_;
    static HalSqCqQueryFunc pHalSqCqQuery_;
    static HalSqCqConfigFunc pHalSqCqConfig_;
};

} // namespace operators
} // namespace zbal
#endif
