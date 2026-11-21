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
#ifndef OPEN_DCC_TOOLS_ODCC_CMD_HELP_H
#define OPEN_DCC_TOOLS_ODCC_CMD_HELP_H

#include <new>

#include "odcc_cmd.h"
#include "odcc_cmd_list.h"

namespace ock {
namespace openDCC {
class SubCommandHelp : public SubCommand {
public:
    SubCommandHelp(const std::string &name, const ParamMap &params) noexcept : SubCommand(name, params) {}

protected:
    int DoPrintHelp() noexcept override
    {
        return DoExecute();
    }

    int DoParamByRule() noexcept override
    {
        return 0;
    }

    int DoInitialize() noexcept override
    {
        return 0;
    }

    int DoExecute() noexcept override
    {
        std::cout << "Usage: " << PROGRAM_NAME << " <sub command> [params]" << std::endl << std::endl;

        const auto &subCommands = AllSubCommands();
        if (!subCommands.empty()) {
            std::cout << "Supported sub commands:" << std::endl;
            for (const auto &item : subCommands) {
                std::cout << "  " << std::left << std::setw(16) << item.name << item.helpStr << std::endl;
            }
            std::cout << std::endl;
        }

        std::cout << "Supported options:" << std::endl;
        std::cout << "  " << std::left << std::setw(16) << SUB_CMD_MINUS_H << "print the help tips" << std::endl;

        return 0;
    }
};

inline SubCommand *CreateHelp(const ParamMap &params)
{
    return new (std::nothrow) SubCommandHelp(SUB_CMD_MINUS_H, params);
}
} // namespace openDCC
} // namespace ock

#endif // OPEN_DCC_TOOLS_ODCC_CMD_HELP_H
