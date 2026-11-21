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
#ifndef OPEN_DCC_TOOLS_ODCC_COMMON_H
#define OPEN_DCC_TOOLS_ODCC_COMMON_H

#include <cstdint>
#include <map>
#include <string>

namespace ock {
namespace openDCC {
using ParamMap = std::map<std::string, std::string>;

constexpr char PROGRAM_NAME[] = "odcc";

#define SUB_CMD_MINUS_H "-h"

struct SubCmd {
    std::string name;
    std::string helpStr;
};

enum ParamDataType : int8_t {
    PDT_NONE = 0,
    PDT_INT64,
    PDT_STR,
    PDT_STR_ENUM,

    PDT_COUNT
};

/* the rule of one parameter of a sub command */
struct Param {
    std::string name;
    ParamDataType dataType = PDT_NONE;
    bool required = false;
    std::string help;

    struct {
        int64_t defaultValue = 0;
        int64_t min = 0;
        int64_t max = 0;
        int64_t value = 0;
    } int64Rule;

    struct {
        std::string defaultValue;
        std::string allEnum;
        std::string value;
    } strRule;

    Param() = default;

    Param(const std::string &n, ParamDataType t, bool r, int64_t defaultValue, int64_t minValue, int64_t maxValue,
          const std::string &h) noexcept
        : name(n), dataType(t), required(r), help(h)
    {
        int64Rule.defaultValue = defaultValue;
        int64Rule.min = minValue;
        int64Rule.max = maxValue;
        int64Rule.value = defaultValue;
    }

    Param(const std::string &n, ParamDataType t, bool r, const std::string &defaultValue, const std::string &enums,
          const std::string &h) noexcept
        : name(n), dataType(t), required(r), help(h)
    {
        strRule.defaultValue = defaultValue;
        strRule.allEnum = enums;
        strRule.value = defaultValue;
    }

    bool SetValueFromString(const std::string &src) noexcept
    {
        switch (dataType) {
            case PDT_INT64: {
                try {
                    int64Rule.value = std::stoll(src);
                } catch (...) {
                    return false;
                }
                return true;
            }
            case PDT_STR:
            case PDT_STR_ENUM:
                strRule.value = src;
                return true;
            default:
                return false;
        }
    }

    bool IsValueValid() const noexcept
    {
        switch (dataType) {
            case PDT_INT64:
                return int64Rule.value >= int64Rule.min && int64Rule.value <= int64Rule.max;
            case PDT_STR:
                return !strRule.value.empty();
            case PDT_STR_ENUM:
                /* wrap with '|' so that the value must be one of the enums, which are separated by '|' */
                return ("|" + strRule.allEnum + "|").find("|" + strRule.value + "|") != std::string::npos;
            default:
                return false;
        }
    }

    std::string RequiredError() const noexcept
    {
        return "param '" + name + "' is required, " + TypeString();
    }

    std::string InvalidError() const noexcept
    {
        return "invalid value for param '" + name + "', " + TypeString();
    }

    std::string HelpString() const noexcept
    {
        return std::string(required ? "required, " : "optional, ") + TypeString() + (help.empty() ? "" : ", " + help);
    }

private:
    std::string TypeString() const noexcept
    {
        switch (dataType) {
            case PDT_INT64:
                return "type is number, range [" + std::to_string(int64Rule.min) + "-" +
                       std::to_string(int64Rule.max) + "]";
            case PDT_STR:
                return "type is str, which should not be empty";
            case PDT_STR_ENUM:
                return "type is str, which should be one of '" + strRule.allEnum + "'";
            default:
                return "";
        }
    }
};

using ParamRuleMap = std::map<std::string, Param>;
} // namespace openDCC
} // namespace ock

#endif // OPEN_DCC_TOOLS_ODCC_COMMON_H
