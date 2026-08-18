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

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "local_dram_validation_role.h"

using namespace ock::mf;

namespace {
constexpr const char *kRoleEnv = "MF_LOCAL_DRAM_VALIDATION_ROLE";
constexpr const char *kHostEidEnv = "MF_HOST_URMA_EID";

class EnvVarGuard {
public:
    explicit EnvVarGuard(const char *name) : name_(name)
    {
        const char *value = std::getenv(name);
        if (value != nullptr) {
            wasSet_ = true;
            value_ = value;
        }
    }

    ~EnvVarGuard()
    {
        if (wasSet_) {
            (void)setenv(name_.c_str(), value_.c_str(), 1);
        } else {
            (void)unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    bool wasSet_{false};
    std::string value_;
};
} // namespace

TEST(LocalDramValidationRoleTest, UnsetRoleReturnsDevice)
{
    EnvVarGuard roleGuard(kRoleEnv);
    ASSERT_EQ(0, unsetenv(kRoleEnv));

    EXPECT_EQ(GetLocalDramValidationRole(7U, HYBM_DOP_TYPE_DEVICE_RDMA), LocalDramValidationRole::DEVICE);
}

TEST(LocalDramValidationRoleTest, HostRoleAcceptsAnyRank)
{
    EnvVarGuard roleGuard(kRoleEnv);
    EnvVarGuard eidGuard(kHostEidEnv);
    ASSERT_EQ(0, setenv(kRoleEnv, "host", 1));
    ASSERT_EQ(0, setenv(kHostEidEnv, "00000000003f050000100000df08eb01", 1));

    for (uint32_t rankId : {0U, 1U, 7U}) {
        EXPECT_EQ(GetLocalDramValidationRole(rankId, HYBM_DOP_TYPE_HOST_DEVICE_URMA), LocalDramValidationRole::HOST);
    }
}

TEST(LocalDramValidationRoleTest, WrongRoleReturnsInvalid)
{
    EnvVarGuard roleGuard(kRoleEnv);
    EnvVarGuard eidGuard(kHostEidEnv);
    ASSERT_EQ(0, setenv(kRoleEnv, "device", 1));
    ASSERT_EQ(0, setenv(kHostEidEnv, "00000000003f050000100000df08eb01", 1));

    EXPECT_EQ(GetLocalDramValidationRole(1U, HYBM_DOP_TYPE_HOST_DEVICE_URMA), LocalDramValidationRole::INVALID);
}

TEST(LocalDramValidationRoleTest, WrongProtocolReturnsInvalid)
{
    EnvVarGuard roleGuard(kRoleEnv);
    EnvVarGuard eidGuard(kHostEidEnv);
    ASSERT_EQ(0, setenv(kRoleEnv, "host", 1));
    ASSERT_EQ(0, setenv(kHostEidEnv, "00000000003f050000100000df08eb01", 1));

    EXPECT_EQ(GetLocalDramValidationRole(1U, HYBM_DOP_TYPE_DEVICE_RDMA), LocalDramValidationRole::INVALID);
}

TEST(LocalDramValidationRoleTest, MissingHostEidReturnsInvalid)
{
    EnvVarGuard roleGuard(kRoleEnv);
    EnvVarGuard eidGuard(kHostEidEnv);
    ASSERT_EQ(0, setenv(kRoleEnv, "host", 1));
    ASSERT_EQ(0, unsetenv(kHostEidEnv));

    EXPECT_EQ(GetLocalDramValidationRole(1U, HYBM_DOP_TYPE_HOST_DEVICE_URMA), LocalDramValidationRole::INVALID);
}
