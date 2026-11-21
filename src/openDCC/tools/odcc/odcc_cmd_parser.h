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
#ifndef OPEN_DCC_TOOLS_ODCC_CMD_PARSER_H
#define OPEN_DCC_TOOLS_ODCC_CMD_PARSER_H

#include "odcc_common.h"

namespace ock {
namespace openDCC {
class CmdParser {
public:
    CmdParser(int argc, char *argv[]) noexcept
    {
        Parse(argc, argv);
    }

    const std::string &SubCommand() const noexcept
    {
        return subCmd_;
    }

    const ParamMap &Params() const noexcept
    {
        return params_;
    }

private:
    void Parse(int argc, char *argv[]) noexcept
    {
        /* nothing to do, print the help tips */
        if (argc <= 1) {
            subCmd_ = SUB_CMD_MINUS_H;
            return;
        }

        subCmd_ = argv[1];
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];

            if (arg == SUB_CMD_MINUS_H) {
                params_[SUB_CMD_MINUS_H] = "";
                continue;
            }

            /* only the form of '--key=value' is supported */
            if (arg.substr(0, 2) != "--") {
                continue;
            }

            auto pos = arg.find('=');
            if (pos == std::string::npos) {
                continue;
            }

            params_[arg.substr(2, pos - 2)] = arg.substr(pos + 1);
        }
    }

private:
    std::string subCmd_;
    ParamMap params_;
};
} // namespace openDCC
} // namespace ock

#endif // OPEN_DCC_TOOLS_ODCC_CMD_PARSER_H
