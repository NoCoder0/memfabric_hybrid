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
#ifndef OPEN_DCC_TOOLS_ODCC_CMD_H
#define OPEN_DCC_TOOLS_ODCC_CMD_H

#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "odcc_cmd_parser.h"
#include "odcc_common.h"

namespace ock {
namespace openDCC {
/******************************/
/* sub command base           */
/******************************/
class SubCommand {
public:
    SubCommand(const std::string &name, const ParamMap &params) noexcept : name_(name), params_(params) {}
    virtual ~SubCommand() = default;

    const std::string &Name() const noexcept
    {
        return name_;
    }

    /**
     * @brief Execute this sub command, every sub command goes the same pipeline
     *
     * @return 0 if successful, others if failed
     */
    int Execute() noexcept
    {
        /* step1: setup the parameter rules of this sub command */
        SetRules();

        /* print help when '-h' is set, no matter which sub command it is */
        if (params_.find(SUB_CMD_MINUS_H) != params_.end()) {
            return DoPrintHelp();
        }

        /* step2: convert and verify the parameters by the rules */
        auto verified = DoParamByRule();
        if (verified != 0) {
            std::cout << std::endl;
            DoPrintHelp();
            return verified;
        }

        /* step3: initialize the sub command */
        auto inited = DoInitialize();
        if (inited != 0) {
            std::cout << "Initialize sub command '" << name_ << "' failed" << std::endl;
            return inited;
        }

        /* step4: run the sub command */
        return DoExecute();
    }

protected:
    virtual void SetRules() noexcept {}

    virtual int DoPrintHelp() noexcept
    {
        std::cout << "Params for sub command '" << name_ << "':" << std::endl;
        for (const auto &rule : paramRules_) {
            std::cout << "  --" << std::left << std::setw(16) << rule.first << "  " << rule.second.HelpString()
                      << std::endl;
        }
        std::cout << std::endl;

        std::cout << "Example:" << std::endl;
        for (const auto &item : examples_) {
            std::cout << "  " << item << std::endl;
        }

        return 0;
    }

    virtual int DoParamByRule() noexcept
    {
        for (auto &rule : paramRules_) {
            auto iter = params_.find(rule.first);

            /* the parameter is absent, report an error if it is required */
            if (iter == params_.end()) {
                if (rule.second.required) {
                    std::cout << "Execute sub command '" << name_ << "' failed, as " << rule.second.RequiredError()
                              << std::endl;
                    return -1;
                }

                continue;
            }

            /* the parameter is set, but it can not be converted or does not meet the rule */
            if (!rule.second.SetValueFromString(iter->second) || !rule.second.IsValueValid()) {
                std::cout << "Execute sub command '" << name_ << "' failed, as " << rule.second.InvalidError()
                          << std::endl;
                return -1;
            }

            params_.erase(iter);
        }

        /* any parameter left is not for this sub command */
        if (!params_.empty()) {
            std::cout << "Execute sub command '" << name_ << "' failed, as the following param is not for it:"
                      << std::endl;
            for (const auto &param : params_) {
                std::cout << "  --" << param.first << "=" << param.second << std::endl;
            }
            std::cout << std::endl;
            return -1;
        }

        return 0;
    }

protected:
    virtual int DoInitialize() noexcept = 0;

    virtual int DoExecute() noexcept = 0;

protected:
    std::string name_;
    ParamMap params_;
    ParamRuleMap paramRules_;
    std::vector<std::string> examples_;
};

/******************************/
/* sub command registry       */
/******************************/
using SubCommandCreateFunc = SubCommand *(*)(const ParamMap &params);

class SubCommandRegistry {
public:
    static SubCommandRegistry &Instance() noexcept
    {
        static SubCommandRegistry registry;
        return registry;
    }

    /**
     * @brief Get the create function of the given sub command
     *
     * @param name         [in] name of the sub command
     * @return the create function, or nullptr when not registered
     */
    SubCommandCreateFunc GetCommandCreateFunc(const std::string &name) const noexcept
    {
        auto iter = subCommands_.find(name);
        if (iter == subCommands_.end()) {
            return nullptr;
        }

        return iter->second;
    }

    /**
     * @brief Register one sub command to this registry
     *
     * @param name         [in] name of the sub command
     * @param func         [in] create function of the sub command
     */
    void RegisterSubCommand(const std::string &name, SubCommandCreateFunc func) noexcept
    {
        if (name.empty() || func == nullptr) {
            return;
        }

        subCommands_[name] = func;
    }

    /**
     * @brief Register all the sub commands
     */
    void RegisterAll() noexcept;

private:
    std::map<std::string, SubCommandCreateFunc> subCommands_;
};
} // namespace openDCC
} // namespace ock

#endif // OPEN_DCC_TOOLS_ODCC_CMD_H
