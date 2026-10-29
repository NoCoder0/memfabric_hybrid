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
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

#include "hybm_data_op_sdma.h"
#include "hybm_data_op_host_shm.h"
#include "hybm_data_op_host_rdma.h"
#include "hybm_data_op_device_rdma.h"
#include "hybm_data_op_device_urma.h"

class HybmDataOperatorTest : public testing::Test {
public:
    void SetUp() override
    {
        ock::mf::HybmVaManager::GetInstance().ClearAll();
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
        ock::mf::HybmVaManager::GetInstance().ClearAll();
    };
};

TEST_F(HybmDataOperatorTest, update_gva_space_test)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_BUTT, 1, 1, 1);
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_DEVICE, 1, 0, 1);
}

TEST_F(HybmDataOperatorTest, clean_up_test)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::CleanUp();
}

TEST_F(HybmDataOperatorTest, update_gva_space_unknown_type)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_BUTT, 0, 0, 0);
}

TEST_F(HybmDataOperatorTest, update_gva_space_device_type)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_DEVICE, 0x1000, 0x100000, 2); // 2
}

TEST_F(HybmDataOperatorTest, update_gva_space_host_type)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_HOST, 0x2000, 0x200000, 4); // 4
}

TEST_F(HybmDataOperatorTest, update_gva_space_zero_size)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_DEVICE, 0x1000, 0, 2); // 2
}

TEST_F(HybmDataOperatorTest, RemovedGlobalRecordFailsWhileOtherRecordsRemain)
{
    using namespace ock::mf;
    auto &manager = HybmVaManager::GetInstance();
    constexpr uint64_t gva = HYBM_TRANS_GVA_START_ADDR;
    constexpr uint64_t size = 4096ULL;
    constexpr uint64_t mappedVa = 0x200000000ULL;
    ASSERT_EQ(manager.AddVaInfo({gva, mappedVa, mappedVa, size, HYBM_MEM_TYPE_HOST}, false, AddressOwnership::Imported),
              BM_OK);
    ASSERT_EQ(manager.AddVaInfo({gva + size, 0ULL, 0ULL, size, HYBM_MEM_TYPE_HOST}, false, AddressOwnership::Imported),
              BM_OK);
    DataOperatorPtr operators[] = {std::make_shared<HostDataOpSDMA>(), std::make_shared<HostDataOpHostShm>(0U),
                                   std::make_shared<HostDataOpRDMA>(0U, nullptr),
                                   std::make_shared<DataOpDeviceRDMA>(0U, nullptr),
                                   std::make_shared<DataOpDeviceURMA>(0U, nullptr)};
    uint8_t local[16U]{};
    for (const auto &op : operators) {
        void *src = local;
        void *dest = reinterpret_cast<void *>(gva);
        ASSERT_EQ(op->TryTransformVa(src, dest, HYBM_LOCAL_HOST_TO_GLOBAL_HOST), BM_OK);
        EXPECT_EQ(src, local);
        EXPECT_EQ(dest, reinterpret_cast<void *>(mappedVa));
    }
    manager.RemoveOneVaInfo(gva, mappedVa, mappedVa);
    for (const auto &op : operators) {
        void *src = local;
        void *dest = reinterpret_cast<void *>(gva);
        EXPECT_EQ(op->TryTransformVa(src, dest, HYBM_LOCAL_HOST_TO_GLOBAL_HOST), BM_GVA_NOT_FOUND);
        EXPECT_EQ(dest, reinterpret_cast<void *>(gva));
        src = dest;
        dest = local;
        EXPECT_EQ(op->TryTransformVa(src, dest, HYBM_GLOBAL_HOST_TO_LOCAL_HOST), BM_GVA_NOT_FOUND);
    }
}

TEST_F(HybmDataOperatorTest, LocalEndpointIsNotLookedUpAndSdmaRetainsEntityResolver)
{
    using namespace ock::mf;
    constexpr uint64_t gva = HYBM_TRANS_GVA_START_ADDR;
    constexpr uint64_t dva = 0x200000000ULL;
    uint32_t lookups = 0;
    HostDataOpSDMA op;
    op.SetImportedGvaResolver([&](uint64_t va) {
        ++lookups;
        EXPECT_EQ(va, gva);
        return dva;
    });
    uint8_t local[16]{};
    void *src = local;
    void *dest = reinterpret_cast<void *>(gva);
    ASSERT_EQ(op.TryTransformVa(src, dest, HYBM_LOCAL_HOST_TO_GLOBAL_DEVICE), BM_OK);
    EXPECT_EQ(src, local);
    EXPECT_EQ(dest, reinterpret_cast<void *>(dva));
    EXPECT_EQ(lookups, 1U);
}

TEST_F(HybmDataOperatorTest, RemoteGvaWithoutLocalMappingKeepsOriginalAddress)
{
    using namespace ock::mf;
    auto &manager = HybmVaManager::GetInstance();
    constexpr uint64_t gva = HYBM_TRANS_GVA_START_ADDR;
    constexpr uint64_t size = 4096ULL;
    ASSERT_EQ(manager.AddVaInfo({gva, 0ULL, 0ULL, size, HYBM_MEM_TYPE_HOST}, false, AddressOwnership::Imported), BM_OK);
    DataOperatorPtr operators[] = {std::make_shared<HostDataOpRDMA>(0U, nullptr),
                                   std::make_shared<DataOpDeviceRDMA>(0U, nullptr),
                                   std::make_shared<DataOpDeviceURMA>(0U, nullptr)};
    uint8_t local[16U]{};
    for (const auto &op : operators) {
        void *src = local;
        void *dest = reinterpret_cast<void *>(gva);
        ASSERT_EQ(op->TryTransformVa(src, dest, HYBM_LOCAL_HOST_TO_GLOBAL_HOST), BM_OK);
        EXPECT_EQ(src, local);
        EXPECT_EQ(dest, reinterpret_cast<void *>(gva));
        src = dest;
        dest = local;
        EXPECT_EQ(op->TryTransformVa(src, dest, HYBM_GLOBAL_HOST_TO_LOCAL_HOST), BM_OK);
        EXPECT_EQ(src, reinterpret_cast<void *>(gva));
    }
}

TEST_F(HybmDataOperatorTest, MissingGlobalStopsCopyBeforeSubmission)
{
    using namespace ock::mf;
    DataOperatorPtr operators[] = {std::make_shared<DataOpDeviceRDMA>(0, nullptr),
                                   std::make_shared<DataOpDeviceURMA>(0, nullptr)};
    uint8_t local[16]{};
    void *sources[] = {local};
    void *destinations[] = {reinterpret_cast<void *>(HYBM_TRANS_GVA_START_ADDR)};
    uint64_t sizes[] = {sizeof(local)};
    ExtOptions options{};
    options.destRankId = 1;
    for (const auto &op : operators) {
        hybm_copy_params params{sources[0], destinations[0], sizes[0]};
        EXPECT_EQ(op->DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_GVA_NOT_FOUND);
    }
    hybm_batch_copy_params batch{sources, destinations, sizes, 1};
    EXPECT_EQ(operators[0]->BatchDataCopy(batch, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_GVA_NOT_FOUND);
    HostDataOpSDMA sdma;
    hybm_copy_params single{sources[0], destinations[0], sizes[0]};
    EXPECT_EQ(sdma.DataCopyAsync(single, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_GVA_NOT_FOUND);
    EXPECT_EQ(sdma.BatchDataCopy(batch, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, options), BM_GVA_NOT_FOUND);
}
