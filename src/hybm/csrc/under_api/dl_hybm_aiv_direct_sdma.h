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
#ifndef MEMFABRIC_HYBRID_DL_HYBM_AIV_DIRECT_SDMA_H
#define MEMFABRIC_HYBRID_DL_HYBM_AIV_DIRECT_SDMA_H

#include "hybm_common_include.h"

namespace ock {
namespace mf {

using hybmAivDirectSdmaLaunchFunc = int (*)(void *, void *, uint32_t, void *, uint32_t, void *);
using hybmAivDirectSdmaAbiVersionFunc = uint32_t (*)();

class DlHybmAivDirectSdmaApi {
public:
    static Result TryLoadLibrary();
    static void CleanupLibrary();

    static inline Result HybmAivDirectSdmaLaunch(void *workspace, void *descriptors, uint32_t count, void *consumed,
                                                 uint32_t aivCount, void *stream)
    {
        if (pHybmAivDirectSdmaLaunch == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        return pHybmAivDirectSdmaLaunch(workspace, descriptors, count, consumed, aivCount, stream);
    }

private:
    static std::mutex gMutex;
    static bool gLoaded;
    static void *libHandle;
    static const char *gHybmAivDirectSdmaLibName;

    static hybmAivDirectSdmaLaunchFunc pHybmAivDirectSdmaLaunch;
};
} // namespace mf
} // namespace ock
#endif // MEMFABRIC_HYBRID_DL_HYBM_AIV_DIRECT_SDMA_H
