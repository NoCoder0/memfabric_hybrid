/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include <cstdint>
#include <cstring>

#include "dl_hal_api_def.h"

constexpr int32_t RETURN_OK = 0;
constexpr const char *STUB_CPU_LIST = "0-3";

extern "C" {
int32_t dcmiv2_init()
{
    return RETURN_OK;
}

int32_t dcmiv2_get_affinity_cpu_info_by_dev_id(int32_t deviceId, char *cpuList, int32_t *length)
{
    (void)deviceId;
    if (cpuList == nullptr || length == nullptr || *length <= 0) {
        return -1;
    }
    const size_t copySize =
        sizeof(STUB_CPU_LIST) < static_cast<size_t>(*length) ? sizeof(STUB_CPU_LIST) : static_cast<size_t>(*length);
    std::strncpy(cpuList, STUB_CPU_LIST, copySize - 1U);
    cpuList[copySize - 1U] = '\0';
    *length = static_cast<int32_t>(std::strlen(cpuList));
    return RETURN_OK;
}

int dcmiv2_get_urma_device_cnt(int npuId, unsigned int *devCnt)
{
    (void)npuId;
    if (devCnt == nullptr) {
        return -1;
    }
    *devCnt = 0U;
    return RETURN_OK;
}

int dcmiv2_get_eid_list_by_urma_dev_index(int npuId, int urmaDevIndex, dcmi_urma_eid_info *eidList, int *eidCnt)
{
    (void)npuId;
    (void)urmaDevIndex;
    (void)eidList;
    if (eidCnt == nullptr) {
        return -1;
    }
    *eidCnt = 0;
    return RETURN_OK;
}

int dcmiv2_get_device_pcie_info(int npuId, dcmi_pcie_info_all *pcieInfo)
{
    (void)npuId;
    if (pcieInfo == nullptr) {
        return -1;
    }
    (void)memset(pcieInfo, 0, sizeof(dcmi_pcie_info_all));
    return RETURN_OK;
}
}
