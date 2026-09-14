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

#include <cstring>
#include <mockcpp/mockcpp.hpp>
#include <string>
#include <vector>

// 白盒测试惯用手法（与仓库其他 UT 一致）：通过宏绕过 private/protected 访问控制。
// G.NAM.03 要求宏名全大写，但改名将破坏关键字替换语义，属工具误报，保持原样。
#define private   public
#define protected public
#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "hybm_asymmetric_mem_segment.h"
#include "hybm_def.h"
#include "hybm_define.h"
#include "hybm_dev_legacy_segment.h"
#include "hybm_mem_segment.h"
#undef private
#undef protected

#include "hybm_ex_info_transfer.h"
#include "hybm_va_manager.h"

using namespace ock::mf;

namespace {
constexpr uint64_t TEST_GVA_BASE = 0x400000000000ULL;   // 自造的段基址（不与 HBM 地址区间重叠）
constexpr uint64_t TEST_HBM_ADDR = HYBM_HBM_START_ADDR; // 0x100000000000, 落在 HBM 区间内
constexpr uint64_t TEST_HOST_ADDR = 0x00007F1234000000ULL;
constexpr uint64_t TEST_SIZE = 4096ULL;
constexpr uint64_t TEST_TOTAL_SIZE = 1ULL << 30;
constexpr uint64_t TEST_FAKE_DVA = 0x600012340000ULL;
constexpr uint32_t TEST_LOGIC_DEVICE_ID = 5;
constexpr int TEST_ENTITY_ID = 200; // G.CNS.02: 段构造的 entityId 参数具名化
// G.EXP.15: 整型测试地址到指针的转换集中于此（memcpy 转换），避免散落的 reinterpret_cast
inline void *TestAddr(uint64_t addr)
{
    void *ptr = nullptr;
    std::memcpy(&ptr, &addr, sizeof(ptr));
    return ptr;
}

int g_halHostRegisterCount = 0;
int g_halHostUnregisterCount = 0;
int g_ipcWhitelistCount = 0;

// 以下 stub 的签名（含 void*/char* 参数与 strncpy 的写入语义）必须与被 mock 的
// 函数指针 typedef 完全一致（dl_acl_api.h/dl_hal_api.h），无法改用 std::string 等类型。
int HalHostRegisterStub(void *addr, uint64_t size, uint32_t flags, uint32_t devId, void **output)
{
    (void)addr;
    (void)size;
    (void)flags;
    (void)devId;
    g_halHostRegisterCount++;
    *output = TestAddr(TEST_FAKE_DVA);
    return 0;
}

int HalHostUnregisterCountStub(void *addr, uint32_t devId, uint32_t flag)
{
    (void)addr;
    (void)devId;
    (void)flag;
    g_halHostUnregisterCount++;
    return 0;
}

// 必须写入 IPC name，否则 HBM 注册的 RegisterSlice name 为空，会被当成 DRAM 条目跳过
int RtIpcSetMemoryNameStub(const void *ptr, uint64_t byteCount, char *name, uint32_t len)
{
    (void)ptr;
    (void)byteCount;
    if (name != nullptr && len > sizeof("hbm0")) {
        std::memcpy(name, "hbm0", sizeof("hbm0")); // 含结尾 NUL 共 5 字节
    }
    return 0;
}

int RtSetIpcMemorySuperPodPidCountStub(const char *name, uint32_t sdid, int32_t pid[], int32_t num)
{
    (void)name;
    (void)sdid;
    (void)pid;
    (void)num;
    g_ipcWhitelistCount++;
    return 0;
}

// DrvMemGetAttribute 打桩：g_drvMemAttrRet 控制查询成败（非 0 = 失败，模拟 OPTIONAL 符号未加载），
// g_drvMemAttrType 为返回的属性 memType；默认查询失败 → 回退地址段判断，用例按需改写全局值
int g_drvMemAttrType = 0;
int g_drvMemAttrRet = 1;
int DrvMemGetAttributeStub(DVdeviceptr vptr, DVattribute *attr)
{
    (void)vptr;
    if (attr != nullptr) {
        attr->memType = static_cast<uint32_t>(g_drvMemAttrType);
    }
    return g_drvMemAttrRet;
}

// MemSegment 静态成员进入本 suite 时的原始值（TearDown 恢复，避免污染同一可执行文件中的其他 suite）
bool g_staticMembersSaved = false;
AscendSocType g_savedSocType = AscendSocType::ASCEND_910B;
uint32_t g_savedLogicDeviceId = 0U;
uint32_t g_savedDeviceId = 0U;
int g_savedDevicePhyId = -1;
} // namespace

class HybmAsymmetricSegmentTest : public testing::Test {
public:
    void SetUp() override
    {
        GlobalMockObject::reset();
        HybmVaManager::GetInstance().ClearAll();
        g_halHostRegisterCount = 0;
        g_halHostUnregisterCount = 0;
        g_ipcWhitelistCount = 0;
        // 判型查询默认"不可用"（回退地址段判断）；HAL 判型用例改写 g_drvMemAttrType/g_drvMemAttrRet
        g_drvMemAttrType = 0;
        g_drvMemAttrRet = 1;
        MOCKER(&ock::mf::DlHalApi::DrvMemGetAttribute).stubs().will(invoke(DrvMemGetAttributeStub));
        // 保存进入本 suite 时的 MemSegment 静态成员值，TearDown 恢复，避免污染同一可执行文件中的其他 suite
        if (!g_staticMembersSaved) {
            g_savedSocType = MemSegment::socType_;
            g_savedLogicDeviceId = MemSegment::logicDeviceId_;
            g_savedDeviceId = MemSegment::deviceId_;
            g_savedDevicePhyId = MemSegment::devicePhyId_;
            g_staticMembersSaved = true;
        }
    }

    void TearDown() override
    {
        MemSegment::socType_ = g_savedSocType;
        MemSegment::logicDeviceId_ = g_savedLogicDeviceId;
        MemSegment::deviceId_ = g_savedDeviceId;
        MemSegment::devicePhyId_ = g_savedDevicePhyId;
        GlobalMockObject::verify();
        GlobalMockObject::reset();
        HybmVaManager::GetInstance().ClearAll();
    }

protected:
    static MemSegmentOptions MakeOptions(hybm_data_op_type opType, AscendSocType socType)
    {
        MemSegmentOptions options{};
        options.segType = HYBM_MST_ASYMMETRIC;
        options.maxSize = HYBM_LARGE_PAGE_SIZE;
        options.rankCnt = 1;
        options.rankId = 0;
        options.dataOpType = opType;
        MemSegment::socType_ = socType;
        MemSegment::logicDeviceId_ = TEST_LOGIC_DEVICE_ID;
        MemSegment::devicePhyId_ = TEST_LOGIC_DEVICE_ID;
        MemSegment::deviceId_ = 0;
        return options;
    }

    // 直接填充段基址字段，绕开依赖全局 VaManager 预留流程的 ReserveMemorySpace
    static void InitSegmentBase(AsymmetricMemSegment &segment)
    {
        segment.globalVirtualAddress_ = reinterpret_cast<uint8_t *>(TEST_GVA_BASE);
        segment.lvaBase_ = segment.globalVirtualAddress_;
        segment.totalVirtualSize_ = TEST_TOTAL_SIZE;
        segment.allocatedSize_ = 0;
    }
};

/**
 * Register_HostAddr_CreatesHostSlice
 *  - host 地址注册成功：slice memType=HOST、gva=lvaBase_+allocatedSize_；
 *  - VaManager GVA/HVA 表有 HOST 条目；
 *  - 设备 IPC 接口 RtIpcSetMemoryName 未被调用。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_HostAddr_CreatesHostSlice)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_HOST_RDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlAclApi::RtIpcSetMemoryName).expects(never());

    MemSlicePtr slice;
    auto ret = segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, slice);
    EXPECT_EQ(ret, BM_OK);
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->memType_, HYBM_MEM_TYPE_HOST);
    EXPECT_EQ(slice->gva_, TEST_GVA_BASE);
    EXPECT_EQ(slice->vAddress_, TEST_HOST_ADDR);
    EXPECT_EQ(segment.registerSlices_.size(), 1U);

    auto gvaHit = HybmVaManager::GetInstance().FindAllocByVa(TEST_GVA_BASE, HVM_GVA);
    EXPECT_TRUE(gvaHit.second);
    EXPECT_EQ(gvaHit.first.base.memType, HYBM_MEM_TYPE_HOST);
    auto hvaHit = HybmVaManager::GetInstance().FindAllocByVa(TEST_HOST_ADDR, HVM_HVA);
    EXPECT_TRUE(hvaHit.second);
}

/**
 * Register_HostAddr_RejectedWithoutCapability
 *  - 纯设备侧 op（无 host 类传输、无 SDMA）：host DRAM 无数据面通道，能力校验拒绝。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_HostAddr_RejectedWithoutCapability)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_DEVICE_RDMA, ASCEND_910B);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);

    MemSlicePtr slice;
    auto ret = segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, slice);
    EXPECT_EQ(ret, BM_NOT_SUPPORTED);
    EXPECT_EQ(slice, nullptr);
    EXPECT_TRUE(segment.registerSlices_.empty());
}

/**
 * Register_HostAddr_NoOpType_Allowed
 *  - dataOpType=0（未指定传输方式，如 RECEIVER 注册流程）：放行；
 *  - 不触发 HalHostRegister（NeedHostDeviceMapping 为 false），仅登记 GVA。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_HostAddr_NoOpType_Allowed)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_DEFAULT, ASCEND_910B);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlHalApi::HalHostRegister).expects(never());

    MemSlicePtr slice;
    auto ret = segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, slice);
    EXPECT_EQ(ret, BM_OK);
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->memType_, HYBM_MEM_TYPE_HOST);
    EXPECT_EQ(slice->gva_, TEST_GVA_BASE);
    EXPECT_EQ(segment.registerSlices_.size(), 1U);
}

/**
 * Register_HostAddr_WithDeviceRdma_MapsHost
 *  - dataOpType 含 DEVICE_RDMA：HalHostRegister 被调用，返回假 dva，VaManager DVA 表命中。
 *  - 能力校验需同时含 HOST 类 op 才放行，故组合 DEVICE_RDMA | HOST_RDMA。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_HostAddr_WithDeviceRdma_MapsHost)
{
    auto options =
        MakeOptions(static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA), ASCEND_910B);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlHalApi::HalHostRegister).stubs().will(invoke(HalHostRegisterStub));

    MemSlicePtr slice;
    auto ret = segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, slice);
    EXPECT_EQ(ret, BM_OK);
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(g_halHostRegisterCount, 1);

    auto dvaHit = HybmVaManager::GetInstance().FindAllocByVa(TEST_FAKE_DVA, HVM_DVA);
    EXPECT_TRUE(dvaHit.second);
    EXPECT_EQ(dvaHit.first.base.memType, HYBM_MEM_TYPE_HOST);
}

/**
 * Register_HostAddr_Sdma_Rejected
 *  - 纯 SDMA：SDMA 引擎不支持访问外部注册 DRAM（评审结论），注册入口直接拒绝，
 *    不触发 HalHostRegister，不产生任何登记。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_HostAddr_Sdma_Rejected)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlHalApi::HalHostRegister).expects(never());

    MemSlicePtr slice;
    auto ret = segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, slice);
    EXPECT_EQ(ret, BM_NOT_SUPPORTED);
    EXPECT_EQ(slice, nullptr);
    EXPECT_TRUE(segment.registerSlices_.empty());
}

/**
 * Register_HalAttr_DeviceType_WinsOverAddress
 *  - HAL 属性查询成功且 memType 为设备类型：即使地址不在 HBM 地址段内，也走设备注册路径
 *    （RtIpcSetMemoryName 建名，slice memType=DEVICE，不触发 HalHostRegister）。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_HalAttr_DeviceType_WinsOverAddress)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_HOST_RDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlAclApi::RtIpcSetMemoryName).stubs().will(invoke(RtIpcSetMemoryNameStub));
    MOCKER(&ock::mf::DlHalApi::HalHostRegister).expects(never());

    g_drvMemAttrRet = 0;                  // 查询成功
    g_drvMemAttrType = DV_MEM_SVM_DEVICE; // HAL 判定为设备内存

    MemSlicePtr slice;
    auto ret = segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, slice);
    EXPECT_EQ(ret, BM_OK);
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->memType_, HYBM_MEM_TYPE_DEVICE);
}

/**
 * Register_HalAttr_HostType_WinsOverAddress
 *  - HAL 属性查询成功且 memType 非设备类型：即使地址落在 HBM 地址段内，也走 host 注册路径
 *    （slice memType=HOST，不触发 RtIpcSetMemoryName）。
 *  - dataOpType 组合 DEVICE_RDMA 使 NeedHostDeviceMapping() 为 true，可断言 HalHostRegister 建立设备映射。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_HalAttr_HostType_WinsOverAddress)
{
    auto options =
        MakeOptions(static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA), ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlAclApi::RtIpcSetMemoryName).expects(never());
    MOCKER(&ock::mf::DlHalApi::HalHostRegister).stubs().will(invoke(HalHostRegisterStub));

    g_drvMemAttrRet = 0;  // 查询成功
    g_drvMemAttrType = 0; // HAL 判定为非设备（host）内存

    MemSlicePtr slice;
    auto ret = segment.RegisterMemory(reinterpret_cast<void *>(TEST_HBM_ADDR), TEST_SIZE, slice);
    EXPECT_EQ(ret, BM_OK);
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->memType_, HYBM_MEM_TYPE_HOST);
    EXPECT_EQ(g_halHostRegisterCount, 1);
}

/**
 * Register_GvaExhausted_Rejected
 *  - allocatedSize_ 达到 totalVirtualSize_ 后继续注册被拒绝。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_GvaExhausted_Rejected)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_HOST_RDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    segment.allocatedSize_ = segment.totalVirtualSize_;

    MemSlicePtr slice;
    auto ret = segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, slice);
    EXPECT_EQ(ret, BM_ERROR);
    EXPECT_EQ(slice, nullptr);
}

/**
 * Release_HostSlice_UnmapsDevice
 *  - hostMapped=true 的 slice 释放时 HalHostUnregisterEx 被调用且仅一次，VaManager 条目同步移除。
 */
TEST_F(HybmAsymmetricSegmentTest, Release_HostSlice_UnmapsDevice)
{
    auto options =
        MakeOptions(static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_DEVICE_RDMA | HYBM_DOP_TYPE_HOST_RDMA), ASCEND_910B);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlHalApi::HalHostRegister).stubs().will(invoke(HalHostRegisterStub));
    MOCKER(&ock::mf::DlHalApi::HalHostUnregisterEx).stubs().will(invoke(HalHostUnregisterCountStub));

    MemSlicePtr slice;
    ASSERT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, slice), BM_OK);
    ASSERT_TRUE(segment.registerSlices_.at(slice->index_).hostMapped);

    EXPECT_EQ(segment.ReleaseSliceMemory(slice), BM_OK);
    EXPECT_EQ(g_halHostUnregisterCount, 1);
    EXPECT_TRUE(segment.registerSlices_.empty());
    EXPECT_FALSE(HybmVaManager::GetInstance().FindAllocByVa(TEST_GVA_BASE, HVM_GVA).second);
}

/**
 * Export_HostSlice_UsesUserMemMagic
 *  - 导出 DRAM slice：反序列化 magic == USER_MEM_SLICE_EXPORT_INFO_MAGIC、
 *    segmentType == SEGMENT_TYPE_USER_DRAM，gvaOffset/address/size 正确。
 */
TEST_F(HybmAsymmetricSegmentTest, Export_HostSlice_UsesUserMemMagic)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_HOST_RDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);

    MemSlicePtr slice;
    ASSERT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, slice), BM_OK);

    std::string exInfo;
    ASSERT_EQ(segment.Export(slice, exInfo), BM_OK);
    ASSERT_EQ(exInfo.size(), sizeof(UserSliceExportInfo));

    UserSliceExportInfo dramInfo;
    ASSERT_EQ(LiteralExInfoTranslater<UserSliceExportInfo>{}.Deserialize(exInfo, dramInfo), 0);
    EXPECT_EQ(dramInfo.magic, USER_MEM_SLICE_EXPORT_INFO_MAGIC);
    EXPECT_EQ(dramInfo.segmentType, SEGMENT_TYPE_USER_DRAM);
    EXPECT_EQ(dramInfo.gvaOffset, slice->gva_ - TEST_GVA_BASE);
    EXPECT_EQ(dramInfo.address, TEST_HOST_ADDR);
    EXPECT_EQ(dramInfo.size, TEST_SIZE);
    EXPECT_EQ(dramInfo.rankId, 0U);
}

/**
 * Import_UserDramInfo_CreatesRemoteHostSlice
 *  - 构造 192B DRAM 导出信息导入：远端 slice HOST / gva=gvaOffset+base / vAddress_=0；
 *  - addresses 返回 gva；VaManager 记录 importedRankId；不写 importedSliceInfo_。
 */
TEST_F(HybmAsymmetricSegmentTest, Import_UserDramInfo_CreatesRemoteHostSlice)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_HOST_RDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);

    UserSliceExportInfo sliceInfo{};
    sliceInfo.segmentType = SEGMENT_TYPE_USER_DRAM;
    sliceInfo.gvaOffset = 0x1000000ULL;
    sliceInfo.size = TEST_SIZE;
    sliceInfo.rankId = 7U;
    sliceInfo.devicePhyId = 3U;
    std::string info(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
    std::vector<std::string> infos = {info};

    void *addresses[1] = {nullptr};
    auto ret = segment.Import(infos, addresses);
    EXPECT_EQ(ret, BM_OK);
    EXPECT_EQ(addresses[0], TestAddr(TEST_GVA_BASE + 0x1000000ULL));

    ASSERT_EQ(segment.rankToRemoteSlices_.count(7U), 1U);
    auto remoteSlice = segment.rankToRemoteSlices_.at(7U).at(0);
    EXPECT_EQ(remoteSlice->memType_, HYBM_MEM_TYPE_HOST);
    EXPECT_EQ(remoteSlice->gva_, TEST_GVA_BASE + 0x1000000ULL);
    EXPECT_EQ(remoteSlice->vAddress_, 0U);
    EXPECT_TRUE(segment.importedSliceInfo_.empty());

    auto hit = HybmVaManager::GetInstance().FindAllocByVa(TEST_GVA_BASE + 0x1000000ULL, HVM_GVA);
    EXPECT_TRUE(hit.second);
    EXPECT_EQ(hit.first.importedRankId, 7U);
}

/**
 * Import_Rollback_SkipsDramGvaAddresses
 *  - 混合导入：DRAM slice 成功（addresses 记录 GVA）+ invalid magic 失败触发回滚；
 *  - 回滚只针对真实打开过 IPC 映射的 DEVICE 条目，DRAM 的 GVA 绝不传给 RtIpcCloseMemory。
 */
TEST_F(HybmAsymmetricSegmentTest, Import_Rollback_SkipsDramGvaAddresses)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_HOST_RDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlAclApi::RtIpcCloseMemory).expects(never());

    UserSliceExportInfo sliceInfo{};
    sliceInfo.segmentType = SEGMENT_TYPE_USER_DRAM;
    sliceInfo.gvaOffset = 0x1000000ULL;
    sliceInfo.size = TEST_SIZE;
    sliceInfo.rankId = 7U;
    sliceInfo.devicePhyId = 3U;
    std::string dramInfo(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));

    std::string invalidInfo(sizeof(UserSliceExportInfo), '\0'); // magic=0 → 未识别
    std::vector<std::string> infos = {dramInfo, invalidInfo};

    void *addresses[2] = {nullptr, nullptr};
    auto ret = segment.Import(infos, addresses);
    EXPECT_EQ(ret, BM_INVALID_PARAM);
    EXPECT_EQ(addresses[0], TestAddr(TEST_GVA_BASE + 0x1000000ULL)); // 成功条目仍返回 GVA
    EXPECT_EQ(segment.rankToRemoteSlices_.count(7U), 1U);            // 成功条目已登记
}

/**
 * ImportDeviceInfo_SkipsEmptyNameWhitelist
 *  - 已注册 HBM + DRAM slice 时导入远端设备信息：
 *    RtSetIpcMemorySuperPodPid 仅对 HBM slice 调用一次，DRAM（空 name）条目被跳过。
 */
TEST_F(HybmAsymmetricSegmentTest, ImportDeviceInfo_SkipsEmptyNameWhitelist)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_HOST_RDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlAclApi::RtIpcSetMemoryName).stubs().will(invoke(RtIpcSetMemoryNameStub));
    MOCKER(&ock::mf::DlAclApi::RtSetIpcMemorySuperPodPid).stubs().will(invoke(RtSetIpcMemorySuperPodPidCountStub));

    MemSlicePtr hbmSlice;
    ASSERT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HBM_ADDR), TEST_SIZE, hbmSlice), BM_OK);
    MemSlicePtr hostSlice;
    ASSERT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, hostSlice), BM_OK);
    ASSERT_EQ(segment.registerSlices_.size(), 2U);

    HbmExportDeviceInfo deviceInfo{};
    deviceInfo.magic = ENTITY_EXPORT_INFO_MAGIC;
    deviceInfo.devicePhyId = TEST_LOGIC_DEVICE_ID; // 与本机相同，跳过 P2P
    deviceInfo.rankId = 10U;
    deviceInfo.sdid = 123U;
    deviceInfo.pid = 456U;
    std::string info(reinterpret_cast<const char *>(&deviceInfo), sizeof(deviceInfo));

    EXPECT_EQ(segment.ImportDeviceInfo(info), BM_OK);
    EXPECT_EQ(g_ipcWhitelistCount, 1);
}

/**
 * Register_DupAddr_BothTypes_Rejected
 *  - 同一地址重复注册（HBM/DRAM 各一次）→ 第二次 BM_ERROR。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_DupAddr_BothTypes_Rejected)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_HOST_RDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlAclApi::RtIpcSetMemoryName).stubs().will(invoke(RtIpcSetMemoryNameStub));

    MemSlicePtr hbmSlice;
    EXPECT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HBM_ADDR), TEST_SIZE, hbmSlice), BM_OK);
    MemSlicePtr dupHbmSlice;
    EXPECT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HBM_ADDR), TEST_SIZE, dupHbmSlice), BM_ERROR);

    MemSlicePtr hostSlice;
    EXPECT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, hostSlice), BM_OK);
    MemSlicePtr dupHostSlice;
    EXPECT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, dupHostSlice), BM_ERROR);

    EXPECT_EQ(segment.registerSlices_.size(), 2U);
}
