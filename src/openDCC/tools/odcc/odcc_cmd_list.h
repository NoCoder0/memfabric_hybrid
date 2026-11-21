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
#ifndef OPEN_DCC_TOOLS_ODCC_CMD_LIST_H
#define OPEN_DCC_TOOLS_ODCC_CMD_LIST_H

#include <vector>

#include "odcc_common.h"

namespace ock {
namespace openDCC {
constexpr char SUB_CMD_SHOW_ROCE[] = "show_roce";
constexpr char SUB_CMD_SHOW_UB[] = "show_ub";
constexpr char SUB_CMD_PP[] = "pp";

/* all sub commands which are shown by the help command, '-h' is an option rather than a sub command */
inline const std::vector<SubCmd> &AllSubCommands() noexcept
{
    static const std::vector<SubCmd> COMMANDS = {
        {SUB_CMD_SHOW_ROCE, "list all the host roce devices on this machine"},
        {SUB_CMD_SHOW_UB, "list all the host ub devices on this machine"},
        {SUB_CMD_PP, "ping-pong test between a server and a client"},
    };

    return COMMANDS;
}
} // namespace openDCC
} // namespace ock

#endif // OPEN_DCC_TOOLS_ODCC_CMD_LIST_H
