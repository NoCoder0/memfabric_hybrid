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
#ifndef MF_GENETTY_DL_API_H
#define MF_GENETTY_DL_API_H

#include <dlfcn.h>

#include "common/genetty_common_includes.h"

namespace ock {
namespace openDCC {
#define DL_API_DECLARE(__api_name)            static __api_name##_api __api_name##_ptr;
#define DL_API_DEFINE(CLASS_NAME, __api_name) __api_name##_api CLASS_NAME::__api_name##_ptr = nullptr;
#define DL_API_SET_NULL(__api_name)           __api_name##_ptr = nullptr;

#define LOAD_LIB_URMA  1 << 0L
#define LOAD_LIB_VERBS 1 << 1L
#define LOAD_LIB_HCCP  1 << 2L

class DlApi {
public:
    static Result Load(uint32_t libraries) noexcept;
    static void UnLoad(uint32_t libraries) noexcept;

    static void *LoadSymbol(void *&handle, const char *name)
    {
        auto ptr = dlsym(handle, name);
        if (ptr == nullptr) {
            dlclose(handle);
            handle = nullptr;
            GEN_LOG_ERROR("Load " << name << " failed, " << dlerror());
        }

        return ptr;
    }
};

#define DL_LOAD_SYM(TARGET_FUNC_VAR, TARGET_FUNC_TYPE, FILE_HANDLE, SYMBOL_NAME)       \
    TARGET_FUNC_VAR = (TARGET_FUNC_TYPE)(DlApi::LoadSymbol(FILE_HANDLE, SYMBOL_NAME)); \
    if (TARGET_FUNC_VAR == nullptr) {                                                  \
        UnLoadInner();                                                                 \
        if (FILE_HANDLE != nullptr) {                                                  \
            dlclose(FILE_HANDLE);                                                      \
        }                                                                              \
        return GEN_LOAD_LIBRARY_FAILED;                                                \
    }

#define DL_API_LOAD(__api_name) DL_LOAD_SYM(__api_name##_ptr, __api_name##_api, handle, #__api_name)

} // namespace openDIC
} // namespace ock

#endif // MF_GENETTY_DL_API_H
