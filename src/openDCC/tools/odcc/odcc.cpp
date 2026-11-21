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
#include <iostream>
#include <memory>

#include "odcc_cmd.h"

int main(int argc, char *argv[])
{
    using namespace ock::openDCC;

    /* initial all sub commands */
    SubCommandRegistry::Instance().RegisterAll();

    /* parse the command line */
    CmdParser parser(argc, argv);

    auto func = SubCommandRegistry::Instance().GetCommandCreateFunc(parser.SubCommand());
    if (func == nullptr) {
        std::cout << "Invalid sub command(" << parser.SubCommand() << ")" << std::endl << std::endl;
        func = SubCommandRegistry::Instance().GetCommandCreateFunc(SUB_CMD_MINUS_H);
    }

    if (func == nullptr) {
        std::cout << "No sub command registered, please check the build" << std::endl;
        return -1;
    }

    std::unique_ptr<SubCommand> subCmd(func(parser.Params()));
    if (subCmd == nullptr) {
        std::cout << "Create sub command failed, probably out of memory" << std::endl;
        return -1;
    }

    return subCmd->Execute();
}
