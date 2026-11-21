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
#ifndef MF_OPENDCC_ODCC_SHOW_UB_H
#define MF_OPENDCC_ODCC_SHOW_UB_H

#include <new>
#include <string>

#include "odcc_cmd.h"
#include "odcc_cmd_list.h"

namespace ock {
namespace openDCC {
constexpr char SHOW_UB_PARAM_NAME[] = "name";
constexpr char SHOW_UB_PARAM_DETAIL[] = "detail";
constexpr char SHOW_UB_DETAIL_SHORT[] = "short";
constexpr char SHOW_UB_DETAIL_WHOLE[] = "whole";
constexpr char SHOW_UB_DETAIL_ENUMS[] = "short|whole";

class SubCommandShowUb : public SubCommand {
public:
    SubCommandShowUb(const std::string &name, const ParamMap &params) noexcept : SubCommand(name, params) {}

protected:
    void SetRules() noexcept override;

    int DoInitialize() noexcept override;

    int DoExecute() noexcept override;

private:
    std::string nameFilter_;   /* device name to be filtered, empty means all devices */
    bool detailWhole_ = false; /* print the whole information or the short one */
};

inline SubCommand *CreateShowUb(const ParamMap &params)
{
    return new (std::nothrow) SubCommandShowUb(SUB_CMD_SHOW_UB, params);
}
} // namespace openDCC
} // namespace ock

#endif // MF_OPENDCC_ODCC_SHOW_UB_H
