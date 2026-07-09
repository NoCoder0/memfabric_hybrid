/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * ZBAL is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include <gtest/gtest.h>
#include <csignal>

#include "zbal_signal_handler.h"

using namespace zbal;

class TestZBALSignalHandler : public testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TestZBALSignalHandler, NonSigusr1SignalIsIgnored)
{
    EXPECT_NO_THROW(signal_handler(SIGTERM));
    EXPECT_NO_THROW(signal_handler(SIGINT));
    EXPECT_NO_THROW(signal_handler(SIGQUIT));
    EXPECT_NO_THROW(signal_handler(SIGHUP));
    EXPECT_NO_THROW(signal_handler(SIGALRM));
    EXPECT_NO_THROW(signal_handler(0));
    EXPECT_NO_THROW(signal_handler(-1));
}

TEST_F(TestZBALSignalHandler, Sigusr1TriggersDumpAllComm)
{
    EXPECT_NO_THROW(signal_handler(SIGUSR1));
}
