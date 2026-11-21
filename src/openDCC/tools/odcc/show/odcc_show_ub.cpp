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
#include "odcc_show_ub.h"

#include <iostream>

#include "urma/genetty_host_urma_wrapper.h"

namespace ock {
namespace openDCC {
void SubCommandShowUb::SetRules() noexcept
{
    paramRules_[SHOW_UB_PARAM_NAME] =
        Param(SHOW_UB_PARAM_NAME, PDT_STR, false, "", "", "device name of the ub device, empty means all");
    paramRules_[SHOW_UB_PARAM_DETAIL] = Param(SHOW_UB_PARAM_DETAIL, PDT_STR_ENUM, false, SHOW_UB_DETAIL_SHORT,
                                              SHOW_UB_DETAIL_ENUMS, "detail level of the output");

    examples_.push_back(std::string(PROGRAM_NAME) + " " + SUB_CMD_SHOW_UB);
    examples_.push_back(std::string(PROGRAM_NAME) + " " + SUB_CMD_SHOW_UB + " --name=udma0 --detail=whole");
}

int SubCommandShowUb::DoInitialize() noexcept
{
    /* load liburma, register the logger and init the urma library */
    auto result = urma::UrmaUtil::UrmaInit();
    if (result != GEN_OK) {
        std::cout << "Initialize urma failed, result(" << result << ")" << std::endl;
        return -1;
    }

    /* list all the host ub devices of this machine */
    urma::UrmaDevice::Init();

    nameFilter_ = paramRules_[SHOW_UB_PARAM_NAME].strRule.value;
    detailWhole_ = paramRules_[SHOW_UB_PARAM_DETAIL].strRule.value == SHOW_UB_DETAIL_WHOLE;
    return 0;
}

int SubCommandShowUb::DoExecute() noexcept
{
    const auto &devices = urma::UrmaDevice::AllDevices();
    if (devices.empty()) {
        std::cout << "No ub device found on this machine" << std::endl;
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
        std::cout << "No ub device matched with name(" << nameFilter_ << ")" << std::endl;
        return 0;
    }

    std::cout << "Total " << matched << " ub device(s) found" << std::endl;
    return 0;
}
} // namespace openDCC
} // namespace ock
