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
#include "odcc_show_roce.h"

#include <iostream>

#include "verbs/genetty_verbs_host_wrapper.h"

namespace ock {
namespace openDCC {
void SubCommandShowRoce::SetRules() noexcept
{
    paramRules_[SHOW_ROCE_PARAM_NAME] =
        Param(SHOW_ROCE_PARAM_NAME, PDT_STR, false, "", "", "device name of the roce device, empty means all");
    paramRules_[SHOW_ROCE_PARAM_DETAIL] = Param(SHOW_ROCE_PARAM_DETAIL, PDT_STR_ENUM, false, SHOW_ROCE_DETAIL_SHORT,
                                                SHOW_ROCE_DETAIL_ENUMS, "detail level of the output");

    examples_.push_back(std::string(PROGRAM_NAME) + " " + SUB_CMD_SHOW_ROCE);
    examples_.push_back(std::string(PROGRAM_NAME) + " " + SUB_CMD_SHOW_ROCE + " --name=mlx5_0 --detail=whole");
}

int SubCommandShowRoce::DoInitialize() noexcept
{
    /* load libibverbs and list all the roce devices of this machine */
    auto result = verbs::VerbsDevice::Init();
    if (result != GEN_OK) {
        std::cout << "Initialize roce devices failed, result: " << result << std::endl;
        return -1;
    }

    nameFilter_ = paramRules_[SHOW_ROCE_PARAM_NAME].strRule.value;
    detailWhole_ = paramRules_[SHOW_ROCE_PARAM_DETAIL].strRule.value == SHOW_ROCE_DETAIL_WHOLE;
    return 0;
}

int SubCommandShowRoce::DoExecute() noexcept
{
    const auto &devices = verbs::VerbsDevice::AllDevices();
    if (devices.empty()) {
        std::cout << "No roce device found on this machine" << std::endl;
        return 0;
    }

    uint32_t matched = 0;
    for (const auto &item : devices) {
        const auto &device = item.second;
        if (device == nullptr) {
            continue;
        }

        if (!nameFilter_.empty() && device->DeviceName() != nameFilter_) {
            continue;
        }

        std::cout << device->ToString(detailWhole_, "  ", "\n") << std::endl;
        matched++;
    }

    if (matched == 0) {
        std::cout << "No roce device matched with name(" << nameFilter_ << ")" << std::endl;
        return 0;
    }

    std::cout << "Total " << matched << " roce device(s) found" << std::endl;
    return 0;
}
} // namespace openDCC
} // namespace ock
