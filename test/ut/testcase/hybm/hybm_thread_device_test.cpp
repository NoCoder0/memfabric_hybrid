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

#include <gtest/gtest.h>

#define private public
#include "hybm_entity_default.h"
#include "dl_acl_api.h"
#undef private

namespace {
using namespace ock::mf;

constexpr int32_t INIT_DEVICE_ID = 0;
constexpr int32_t OTHER_DEVICE_ID = 1;
thread_local int32_t currentDevice = OTHER_DEVICE_ID;
thread_local int32_t setDeviceCalls = 0;
thread_local int32_t setDeviceResult = BM_OK;

struct AclDeviceFunctionsGuard {
    aclrtGetDeviceFunc oldGetDevice{DlAclApi::pAclrtGetDevice};
    aclrtSetDeviceFunc oldSetDevice{DlAclApi::pAclrtSetDevice};

    AclDeviceFunctionsGuard()
    {
        currentDevice = OTHER_DEVICE_ID;
        setDeviceCalls = 0;
        setDeviceResult = BM_OK;
        DlAclApi::pAclrtGetDevice = [](int32_t *device) {
            *device = currentDevice;
            return BM_OK;
        };
        DlAclApi::pAclrtSetDevice = [](int32_t device) {
            ++setDeviceCalls;
            if (setDeviceResult == BM_OK) {
                currentDevice = device;
            }
            return setDeviceResult;
        };
    }

    ~AclDeviceFunctionsGuard()
    {
        DlAclApi::pAclrtGetDevice = oldGetDevice;
        DlAclApi::pAclrtSetDevice = oldSetDevice;
    }
};

class HybmThreadDeviceTest : public testing::Test {
public:
    void SetUp() override
    {
        ASSERT_EQ(hybm_init(INIT_DEVICE_ID, 0), BM_OK);
    }

    void TearDown() override
    {
        hybm_uninit();
    }
};

TEST_F(HybmThreadDeviceTest, RestoresDeviceAfterExternalSwitch)
{
    AclDeviceFunctionsGuard guard;
    MemEntityDefault entity(INIT_DEVICE_ID);
    ASSERT_EQ(entity.SetThreadAclDevice(), BM_OK);
    ASSERT_EQ(currentDevice, INIT_DEVICE_ID);
    ASSERT_EQ(setDeviceCalls, 1);

    // Simulate another runtime releasing an event and restoring its cached device.
    currentDevice = OTHER_DEVICE_ID;
    ASSERT_EQ(entity.SetThreadAclDevice(), BM_OK);
    EXPECT_EQ(currentDevice, INIT_DEVICE_ID);
    EXPECT_EQ(setDeviceCalls, 2);

    ASSERT_EQ(entity.SetThreadAclDevice(), BM_OK);
    EXPECT_EQ(setDeviceCalls, 2);
}

TEST_F(HybmThreadDeviceTest, PropagatesRebindFailureAfterSuccess)
{
    AclDeviceFunctionsGuard guard;
    MemEntityDefault entity(INIT_DEVICE_ID);
    ASSERT_EQ(entity.SetThreadAclDevice(), BM_OK);
    currentDevice = OTHER_DEVICE_ID;
    setDeviceResult = BM_ERROR;

    EXPECT_EQ(entity.SetThreadAclDevice(), BM_ERROR);
    EXPECT_EQ(currentDevice, OTHER_DEVICE_ID);
    EXPECT_EQ(setDeviceCalls, 2);

    setDeviceResult = BM_OK;
    EXPECT_EQ(entity.SetThreadAclDevice(), BM_OK);
    EXPECT_EQ(currentDevice, INIT_DEVICE_ID);
}
} // namespace
