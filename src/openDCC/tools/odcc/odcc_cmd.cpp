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
#include "odcc_cmd.h"
#include "odcc_cmd_help.h"
#include "odcc_cmd_list.h"

#include "pingpong_genetty/odcc_pp_genetty.h"
#include "show/odcc_show_roce.h"
#include "show/odcc_show_ub.h"

namespace ock {
namespace openDCC {
void SubCommandRegistry::RegisterAll() noexcept
{
    RegisterSubCommand(SUB_CMD_MINUS_H, CreateHelp);
    RegisterSubCommand(SUB_CMD_SHOW_ROCE, CreateShowRoce);
    RegisterSubCommand(SUB_CMD_SHOW_UB, CreateShowUb);
    RegisterSubCommand(SUB_CMD_PP, CreatePp);
}
} // namespace openDCC
} // namespace ock
