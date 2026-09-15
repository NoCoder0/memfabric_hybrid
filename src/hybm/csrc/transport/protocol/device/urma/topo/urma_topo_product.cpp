/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "urma_topo_product.h"

#include "hybm_gva_version.h"
#include "hybm_logger.h"
#include "urma_topo_product_server.h"
#include "urma_topo_product_pod.h"

namespace ock {
namespace mf {
namespace transport {
UrmaTopoProductPtr UrmaTopoProduct::GetUrmaTopoProduct(int32_t deviceId, int32_t logicId, int32_t phyId,
                                                       uint32_t mainboardId)
{
    UrmaTopoProductPtr res = nullptr;
    switch (mainboardId) {
        case MAIN_BOARD_ID_SERVER_TYPE1:
        case MAIN_BOARD_ID_SERVER_8PMESH:
        case MAIN_BOARD_ID_SERVER_8PMESH_UBOE:
        case MAIN_BOARD_ID_SERVER_8PMESH_NOSP:
        case MAIN_BOARD_ID_SERVER_8PMESH_NOSP_UBOE:
        case MAIN_BOARD_ID_SERVER_350L:
        case MAIN_BOARD_ID_POD_FLEX:
        case MAIN_BOARD_ID_POD_FLEX_RTP:
            return std::make_shared<UrmaTopoProductServer>(deviceId, logicId, phyId, mainboardId);
        case MAIN_BOARD_ID_POD:
        case MAIN_BOARD_ID_POD_2D:
            return std::make_shared<UrmaTopoProductPod>(deviceId, logicId, phyId, mainboardId);
        default:
            BM_LOG_ERROR("TopoManager: unknown board id: " << mainboardId);
            return nullptr;
    }
}

Result UrmaTopoProduct::GetTopoFilePath(uint32_t mainboardId, uint32_t spodType, std::string &topoFilePath)
{
    std::string driverPath;
    HalGvaGetDriverInstallPath(driverPath);
    std::string fileName;
    switch (mainboardId) {
        case MAIN_BOARD_ID_SERVER_TYPE1:
        case MAIN_BOARD_ID_SERVER_8PMESH:
        case MAIN_BOARD_ID_SERVER_8PMESH_UBOE:
        case MAIN_BOARD_ID_SERVER_8PMESH_NOSP:
        case MAIN_BOARD_ID_SERVER_8PMESH_NOSP_UBOE:
            fileName = (spodType == TOPO_TYPE_SERVER_16FM) ? "driver/topo/950/atlas_850_2.json"
                                                           : "driver/topo/950/atlas_850_1.json";
            break;
        case MAIN_BOARD_ID_SERVER_350L:
            fileName = "driver/topo/950/atlas_850_3.json";
            break;
        case MAIN_BOARD_ID_POD:
        case MAIN_BOARD_ID_POD_2D:
            fileName = "driver/topo/950/atlas_950_1.json";
            break;
        case MAIN_BOARD_ID_POD_FLEX:
        case MAIN_BOARD_ID_POD_FLEX_RTP:
            fileName = "driver/topo/950/atlas_950_2.json";
            break;
        default:
            BM_LOG_ERROR("GetTopoFilePath: unknown mainboardId=" << mainboardId);
            return BM_INVALID_PARAM;
    }
    topoFilePath = driverPath + "/" + fileName;
    return BM_OK;
}
} // namespace transport
} // namespace mf
} // namespace ock
