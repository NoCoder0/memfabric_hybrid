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
#include "dl_api.h"
#include "vendor/ascend/dl_hal_api.h"
#include "vendor/ascend/dl_hccp_api.h"
#include "vendor/ascend/dl_hccl_api.h"
#include "dl_hcom_api.h"
#include "dl_op_api.h"
#include "vendor/ascend/dl_hcomm_api.h"
#include "vendor/vendor_api.h"
#include "dl_hybm_copy_extend.h"
#include "dl_hybm_aiv_direct_sdma.h"

namespace ock {
namespace mf {

Result DlApi::LoadLibrary(const uint32_t gvaVersion)
{
    auto result = VendorApi::LoadLibrary();
    if (result != BM_OK) {
        return result;
    }
#if defined(ASCEND_NPU)
    result = DlHalApi::LoadLibrary(gvaVersion);
    if (result != BM_OK) {
        VendorApi::CleanupLibrary();
        return result;
    }

    result = DlHccpApi::LoadLibrary();
    if (result != BM_OK) {
        DlHalApi::CleanupLibrary();
        VendorApi::CleanupLibrary();
        return result;
    }

    result = DlHcclApi::LoadLibrary();
    if (result != BM_OK) {
        DlHccpApi::CleanupLibrary();
        DlHalApi::CleanupLibrary();
        VendorApi::CleanupLibrary();
        return result;
    }

    (void)DlHybmExtendApi::TryLoadLibrary();
    (void)DlHybmAivDirectSdmaApi::TryLoadLibrary();
#endif

    DlHcomApi::LoadLibrary();
    return BM_OK;
}

void DlApi::CleanupLibrary()
{
    DlOpApi::CleanupLibrary();
    VendorApi::CleanupLibrary();
    DlHcclApi::CleanupLibrary();
    DlHccpApi::CleanupLibrary();
    DlHalApi::CleanupLibrary();
    DlHcomApi::CleanupLibrary();
    DlHcommApi::CleanupLibrary();
    DlHybmExtendApi::CleanupLibrary();
    DlHybmAivDirectSdmaApi::CleanupLibrary();
}

Result DlApi::LoadExtendLibrary(DlApiExtendLibraryType libraryType)
{
    if (libraryType == DlApiExtendLibraryType::DL_EXT_LIB_DEVICE_RDMA) {
        return DlHccpApi::LoadLibrary();
    }

    if (libraryType == DlApiExtendLibraryType::DL_EXT_LIB_DEVICE_SDMA) {
        auto result = VendorApi::LoadLibrary();
        if (result != BM_OK) {
            return result;
        }
        result = DlOpApi::LoadLibrary();
        if (result != BM_OK) {
            return result;
        }
    }

    if (libraryType == DlApiExtendLibraryType::DL_EXT_LIB_HOST_RDMA) {
        return DlHcomApi::LoadLibrary();
    }

    if (libraryType == DlApiExtendLibraryType::DL_EXT_LIB_DEVICE_URMA) {
        auto result = VendorApi::LoadLibrary();
        if (result != BM_OK) {
            return result;
        }
        result = DlHcommApi::LoadLibrary();
        if (result != BM_OK) {
            return result;
        }
    }

    return BM_OK;
}
} // namespace mf
} // namespace ock
