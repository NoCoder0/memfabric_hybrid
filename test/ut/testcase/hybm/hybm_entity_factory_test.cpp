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

#define private   public
#define protected public
#include "hybm_entity_factory.h"
#undef private
#undef protected

using namespace ock::mf;

class HybmEntityFactoryTest : public testing::Test {
public:
    void SetUp() override
    {
        ClearFactory();
        auto ret = hybm_init(0, 0);
        EXPECT_EQ(ret, BM_OK);
    }

    void TearDown() override
    {
        ClearFactory();
        hybm_uninit();
    }

private:
    static void ClearFactory()
    {
        auto &factory = MemEntityFactory::Instance();
        std::lock_guard<std::mutex> guard(factory.enginesMutex_);
        for (auto &engine : factory.engines_) {
            engine.second->UnReserveMemorySpace();
            engine.second->UnInitialize();
        }
        factory.engines_.clear();
        factory.enginesFromAddress_.clear();
    }
};

TEST_F(HybmEntityFactoryTest, GetOrCreateEngine_SameIdReturnsSame)
{
    auto &factory = MemEntityFactory::Instance();
    auto engine1 = factory.GetOrCreateEngine(1, 0);
    EXPECT_NE(engine1, nullptr);
    EXPECT_EQ(factory.engines_.size(), 1U);

    auto engine2 = factory.GetOrCreateEngine(1, 0);
    EXPECT_EQ(engine1, engine2);
    EXPECT_EQ(factory.engines_.size(), 1U);

    auto engine3 = factory.GetOrCreateEngine(2, 0);
    EXPECT_NE(engine3, nullptr);
    EXPECT_NE(engine1, engine3);
    EXPECT_EQ(factory.engines_.size(), 2U);
}

TEST_F(HybmEntityFactoryTest, FindEngineByPtr)
{
    auto &factory = MemEntityFactory::Instance();
    auto engine = factory.GetOrCreateEngine(1, 0);
    EXPECT_EQ(factory.FindEngineByPtr(engine.get()), engine);
    EXPECT_EQ(factory.FindEngineByPtr(reinterpret_cast<hybm_entity_t>(0x1234)), nullptr);
}

TEST_F(HybmEntityFactoryTest, RemoveEngine)
{
    auto &factory = MemEntityFactory::Instance();
    auto engine = factory.GetOrCreateEngine(1, 0);
    EXPECT_TRUE(factory.RemoveEngine(engine.get()));
    EXPECT_EQ(factory.engines_.size(), 0U);
    EXPECT_EQ(factory.FindEngineByPtr(engine.get()), nullptr);

    EXPECT_FALSE(factory.RemoveEngine(reinterpret_cast<hybm_entity_t>(0x1234)));
}

TEST_F(HybmEntityFactoryTest, MultipleEngines_Scenario)
{
    auto &factory = MemEntityFactory::Instance();
    auto e1 = factory.GetOrCreateEngine(1, 0);
    auto e2 = factory.GetOrCreateEngine(2, 0);
    auto e3 = factory.GetOrCreateEngine(3, 0);
    EXPECT_EQ(factory.engines_.size(), 3U);

    EXPECT_EQ(factory.FindEngineByPtr(e1.get()), e1);
    EXPECT_EQ(factory.FindEngineByPtr(e2.get()), e2);
    EXPECT_EQ(factory.FindEngineByPtr(e3.get()), e3);

    EXPECT_TRUE(factory.RemoveEngine(e2.get()));
    EXPECT_EQ(factory.engines_.size(), 2U);
    EXPECT_EQ(factory.FindEngineByPtr(e2.get()), nullptr);
    EXPECT_EQ(factory.FindEngineByPtr(e1.get()), e1);
}

TEST_F(HybmEntityFactoryTest, InternalStaleEnginesFromAddress)
{
    auto &factory = MemEntityFactory::Instance();
    auto engine = factory.GetOrCreateEngine(1, 0);
    auto ptr = engine.get();
    {
        std::lock_guard<std::mutex> guard(factory.enginesMutex_);
        factory.engines_.clear();
    }
    EXPECT_EQ(factory.FindEngineByPtr(ptr), nullptr);

    // RemoveEngine should still clean up the stale enginesFromAddress_ entry
    EXPECT_TRUE(factory.RemoveEngine(ptr));
    EXPECT_EQ(factory.enginesFromAddress_.size(), 0U);
}
