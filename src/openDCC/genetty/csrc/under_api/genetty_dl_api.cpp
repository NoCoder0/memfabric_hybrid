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

#include "genetty_dl_api.h"

#include "genetty_def.h"
#include "common/genetty_logger.h"
#include "under_api/genetty_dl_api.h"
#include "under_api/urma/genetty_dl_urma_api.h"
#include "under_api/verbs/genetty_dl_verbs_api.h"

#include <cstdint>

namespace ock {
namespace openDCC {
Result DlApi::Load(uint32_t libraries) noexcept
{
    GEN_LOG_DEBUG("enter, libraries(" << libraries << ")");
    Result result = GEN_OK;

    /* load verbs api */
    if ((libraries & LOAD_LIB_VERBS) == LOAD_LIB_VERBS) {
        result = VerbsApi::Load();
        GEN_LOG_DEBUG("finished verbs api loading, result(" << result << ")");
        if (result != GEN_OK) {
            goto ERROR;
        }
    }

    /* load verbs api */
    if ((libraries & LOAD_LIB_URMA) == LOAD_LIB_URMA) {
        result = UrmaApi::Load();
        GEN_LOG_DEBUG("finished urma api loading, result(" << result << ")");
        if (result != GEN_OK) {
            goto ERROR;
        }
    }

    goto OK;

ERROR:
    VerbsApi::UnLoad();

    UrmaApi::UnLoad();

OK:
    GEN_LOG_DEBUG("leave, libraries(" << libraries << ")");
    return result;
}

void DlApi::UnLoad(uint32_t libraries) noexcept
{
    VerbsApi::UnLoad();

    UrmaApi::UnLoad();
}
} // namespace openDIC
} // namespace ock
