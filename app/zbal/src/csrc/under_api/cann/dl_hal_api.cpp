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
#include "dl_hal_api.h"
#include <dlfcn.h>

namespace zbal {
namespace operators {

void *DlHalApi::libHandle_ = nullptr;
HalSqCqQueryFunc DlHalApi::pHalSqCqQuery_ = nullptr;
HalSqCqConfigFunc DlHalApi::pHalSqCqConfig_ = nullptr;

bool DlHalApi::Load()
{
    if (libHandle_ != nullptr) {
        return true;
    }
    libHandle_ = dlopen("libascend_hal.so", RTLD_NOW | RTLD_GLOBAL);
    if (libHandle_ == nullptr) {
        return false;
    }
    pHalSqCqQuery_ = reinterpret_cast<HalSqCqQueryFunc>(dlsym(libHandle_, "halSqCqQuery"));
    pHalSqCqConfig_ = reinterpret_cast<HalSqCqConfigFunc>(dlsym(libHandle_, "halSqCqConfig"));
    if (pHalSqCqQuery_ == nullptr || pHalSqCqConfig_ == nullptr) {
        dlclose(libHandle_);
        libHandle_ = nullptr;
        return false;
    }
    return true;
}

int DlHalApi::HalSqCqQuery(uint32_t devId, struct halSqCqQueryInfo *info)
{
    if (pHalSqCqQuery_ == nullptr) {
        return -1;
    }
    return pHalSqCqQuery_(devId, info);
}

int DlHalApi::HalSqCqConfig(uint32_t devId, struct halSqCqConfigInfo *info)
{
    if (pHalSqCqConfig_ == nullptr) {
        return -1;
    }
    return pHalSqCqConfig_(devId, info);
}

} // namespace operators
} // namespace zbal
