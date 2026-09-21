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

// ===== A5 VMM 共享路径打桩：各 g_*Ret 控制对应 HAL 调用成败（0 = 成功） =====
constexpr uint64_t TEST_FAKE_VMM_HANDLE = 0x900000000001ULL;
int g_retainCount = 0, g_exportCount = 0, g_transCount = 0, g_attrCount = 0, g_vmmReleaseCount = 0;
int g_importCount = 0, g_reserveCount = 0, g_mapCount = 0, g_unmapCount = 0, g_ipcOpenCount = 0, g_ipcCloseCount = 0;
int g_setAccessCount = 0;
int g_retainRet = 0, g_exportRet = 0, g_transRet = 0, g_attrRet = 0, g_importRet = 0, g_mapRet = 0;

drv_mem_handle_t *FakeVmmHandle(uint64_t value)
{
    drv_mem_handle_t *handle = nullptr;
    std::memcpy(&handle, &value, sizeof(handle));
    return handle;
}

int HalMemRetainStub(drv_mem_handle_t **handle, void *ptr)
{
    (void)ptr;
    g_retainCount++;
    *handle = FakeVmmHandle(TEST_FAKE_VMM_HANDLE);
    return g_retainRet;
}

// share_info 写入确定性二进制模式（(i*7+3) & 0xFF，天然含 \0 字节），供截断回归逐字节比对
int HalMemExportStub(drv_mem_handle_t *handle, drv_mem_handle_type type, uint64_t flags, MemShareHandle *sHandle)
{
    (void)handle;
    (void)type;
    (void)flags;
    g_exportCount++;
    if (sHandle != nullptr) {
        for (uint32_t i = 0; i < MEM_SHARE_HANDLE_LEN; i++) {
            sHandle->share_info[i] = static_cast<uint8_t>((i * 7U + 3U) & 0xFFU);
        }
    }
    return g_exportRet;
}

int HalMemTransStub(drv_mem_handle_type type, MemShareHandle *handle, uint32_t *serverId, uint64_t *shareable)
{
    (void)type;
    (void)handle;
    g_transCount++;
    if (serverId != nullptr) {
        *serverId = 1U;
    }
    if (shareable != nullptr) {
        *shareable = 0x99ULL;
    }
    return g_transRet;
}

int HalMemShareAttrStub(uint64_t handle, enum ShareHandleAttrType type, struct ShareHandleAttr attr)
{
    (void)handle;
    (void)type;
    (void)attr;
    g_attrCount++;
    return g_attrRet;
}

int HalMemReleaseCountStub(drv_mem_handle_t *handle)
{
    (void)handle;
    g_vmmReleaseCount++;
    return 0;
}

int HalMemImportStub(drv_mem_handle_type type, MemShareHandle *sHandle, uint32_t devid, drv_mem_handle_t **handle)
{
    (void)type;
    (void)sHandle;
    (void)devid;
    g_importCount++;
    *handle = FakeVmmHandle(TEST_FAKE_VMM_HANDLE + 1U);
    return g_importRet;
}

int HalMemImportUnloadStub(drv_mem_handle_type type, MemShareHandle *sHandle, uint32_t devid, drv_mem_handle_t **handle)
{
    (void)type;
    (void)sHandle;
    (void)devid;
    (void)handle;
    g_importCount++;
    return BM_UNDER_API_UNLOAD;
}

int HalMemAddressReserveOkStub(void **ptr, size_t size, size_t alignment, void *addr, uint64_t flag)
{
    (void)size;
    (void)alignment;
    (void)flag;
    g_reserveCount++;
    *ptr = addr; // 模拟驱动返回与请求一致的预留地址
    return 0;
}

int HalMemAddressReserveFailStub(void **ptr, size_t size, size_t alignment, void *addr, uint64_t flag)
{
    (void)ptr;
    (void)size;
    (void)alignment;
    (void)addr;
    (void)flag;
    g_reserveCount++;
    return -1;
}

int HalMemAddressReserveMismatchStub(void **ptr, size_t size, size_t alignment, void *addr, uint64_t flag)
{
    (void)size;
    (void)alignment;
    (void)flag;
    g_reserveCount++;
    *ptr = TestAddr(TEST_GVA_BASE); // 与请求的预留地址不一致，应被视为失败
    return 0;
}

int HalMemMapStub(void *ptr, size_t size, size_t offset, drv_mem_handle_t *handle, uint64_t flag)
{
    (void)ptr;
    (void)size;
    (void)offset;
    (void)handle;
    (void)flag;
    g_mapCount++;
    return g_mapRet;
}

int HalMemUnmapCountStub(void *ptr)
{
    (void)ptr;
    g_unmapCount++;
    return 0;
}

// HalMemSetAccess 打桩：import 映射后的本地设备 RW 授权（成功路径必经）
int HalMemSetAccessStub(void *ptr, size_t size, struct drv_mem_access_desc *desc, size_t count)
{
    (void)ptr;
    (void)size;
    (void)desc;
    (void)count;
    g_setAccessCount++;
    return 0;
}

int RtIpcOpenMemoryStub(void **ptr, const char *name)
{
    (void)name;
    g_ipcOpenCount++;
    *ptr = TestAddr(TEST_FAKE_DVA);
    return 0;
}

int RtIpcCloseMemoryCountStub(const void *ptr)
{
    (void)ptr;
    g_ipcCloseCount++;
    return 0;
}

// 构造 VMM 类型导出信息：name+1 填充确定性二进制模式（(i*7+3) & 0xFF，天然含 \0）
UserSliceExportInfo MakeVmmSliceInfo(uint64_t gvaOffset, uint32_t rankId)
{
    UserSliceExportInfo info{};
    info.segmentType = SEGMENT_TYPE_USER_DEV;
    info.gvaOffset = gvaOffset;
    info.size = TEST_SIZE;
    info.rankId = rankId;
    info.devicePhyId = TEST_LOGIC_DEVICE_ID; // 与本机相同，EnsurePeerAccess 直接命中
    info.name[0] = static_cast<char>(USER_HBM_NAME_TYPE_VMM);
    // gvaOffset 折叠进每个字节：保证同 rank 不同 offset 的 name 唯一（VMM 导入以 name 为 handle key）
    const auto salt = static_cast<uint8_t>((gvaOffset >> 24U) ^ (gvaOffset >> 16U) ^ (gvaOffset >> 8U) ^ gvaOffset);
    for (uint32_t i = 1; i < USER_HBM_NAME_MAX_LEN; i++) {
        info.name[i] = static_cast<char>((i * 7U + 3U + salt) & 0xFFU);
    }
    return info;
}

UserSliceExportInfo MakeIpcSliceInfo(uint64_t gvaOffset, uint32_t rankId)
{
    UserSliceExportInfo info{};
    info.segmentType = SEGMENT_TYPE_USER_DEV;
    info.gvaOffset = gvaOffset;
    info.size = TEST_SIZE;
    info.rankId = rankId;
    info.devicePhyId = TEST_LOGIC_DEVICE_ID;
    info.name[0] = static_cast<char>(USER_HBM_NAME_TYPE_IPC);
    std::memcpy(info.name + 1, "ipchbm", sizeof("ipchbm"));
    return info;
}

void MockVmmExportPath()
{
    MOCKER(&ock::mf::DlHalApi::HalMemRetainAllocationHandle).stubs().will(invoke(HalMemRetainStub));
    MOCKER(&ock::mf::DlHalApi::HalMemExport).stubs().will(invoke(HalMemExportStub));
    MOCKER(&ock::mf::DlHalApi::HalMemTransShareableHandle).stubs().will(invoke(HalMemTransStub));
    MOCKER(&ock::mf::DlHalApi::HalMemShareHandleSetAttribute).stubs().will(invoke(HalMemShareAttrStub));
    MOCKER(&ock::mf::DlHalApi::HalMemRelease).stubs().will(invoke(HalMemReleaseCountStub));
}

void MockVmmImportPath()
{
    MOCKER(&ock::mf::DlHalApi::HalMemImport).stubs().will(invoke(HalMemImportStub));
    MOCKER(&ock::mf::DlHalApi::HalMemAddressReserve).stubs().will(invoke(HalMemAddressReserveOkStub));
    MOCKER(&ock::mf::DlHalApi::HalMemMap).stubs().will(invoke(HalMemMapStub));
    MOCKER(&ock::mf::DlHalApi::HalMemSetAccess).stubs().will(invoke(HalMemSetAccessStub));
    MOCKER(&ock::mf::DlHalApi::HalMemRelease).stubs().will(invoke(HalMemReleaseCountStub));
    MOCKER(&ock::mf::DlHalApi::HalMemUnmap).stubs().will(invoke(HalMemUnmapCountStub));
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
        g_halHostRegisterCount = g_halHostUnregisterCount = g_ipcWhitelistCount = 0;
        g_retainCount = g_exportCount = g_transCount = g_attrCount = g_vmmReleaseCount = 0;
        g_importCount = g_reserveCount = g_mapCount = g_unmapCount = g_ipcOpenCount = g_ipcCloseCount = 0;
        g_setAccessCount = 0;
        g_retainRet = g_exportRet = g_transRet = g_attrRet = g_importRet = g_mapRet = 0;
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
 *    （slice memType=DEVICE，不触发 HalHostRegister）。
 *  - 导出名类型与 HAL 判型无关，按地址窗口路由：窗口外地址走 VMM share_handle。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_HalAttr_DeviceType_WinsOverAddress)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_HOST_RDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MockVmmExportPath();
    MOCKER(&ock::mf::DlHalApi::HalHostRegister).expects(never());

    g_drvMemAttrRet = 0;                  // 查询成功
    g_drvMemAttrType = DV_MEM_SVM_DEVICE; // HAL 判定为设备内存

    MemSlicePtr slice;
    auto ret = segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, slice);
    EXPECT_EQ(ret, BM_OK);
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->memType_, HYBM_MEM_TYPE_DEVICE);
    const auto &reg = segment.registerSlices_.at(slice->index_);
    EXPECT_EQ(static_cast<uint8_t>(reg.name[0]), USER_HBM_NAME_TYPE_VMM);
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
 * Register_DupAddr_FakeRegister_ReuseSlice
 *  - 同一区间重复注册（HBM/DRAM 各一次）→ 复用既有 slice 返回 BM_OK，
 *    不新增登记、allocatedSize_ 不重复累加（d2rh 整池注册语义）。
 */
TEST_F(HybmAsymmetricSegmentTest, Register_DupAddr_FakeRegister_ReuseSlice)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_HOST_RDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlAclApi::RtIpcSetMemoryName).stubs().will(invoke(RtIpcSetMemoryNameStub));

    MemSlicePtr hbmSlice;
    ASSERT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HBM_ADDR), TEST_SIZE, hbmSlice), BM_OK);
    MemSlicePtr dupHbmSlice;
    EXPECT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HBM_ADDR), TEST_SIZE, dupHbmSlice), BM_OK);
    EXPECT_EQ(dupHbmSlice, hbmSlice);

    MemSlicePtr hostSlice;
    EXPECT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, hostSlice), BM_OK);
    MemSlicePtr dupHostSlice;
    EXPECT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(TEST_HOST_ADDR), TEST_SIZE, dupHostSlice), BM_OK);
    EXPECT_EQ(dupHostSlice, hostSlice);

    EXPECT_EQ(segment.registerSlices_.size(), 2U);
    EXPECT_EQ(segment.allocatedSize_, 2 * TEST_SIZE);
}

// ====================== A5 VMM 共享路径：导入侧 ======================

/**
 * ImportSlice_A5_VmmMap
 *  - VMM name 导入：import/reserve/map 全链路成功；vmmNameToHandle_ 登记且 key 为 129B 定长名；
 *  - slice vAddress_ 为预留 LVA，addresses 返回 gva。
 */
TEST_F(HybmAsymmetricSegmentTest, ImportSlice_A5_VmmMap)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_950);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MockVmmImportPath();

    auto sliceInfo = MakeVmmSliceInfo(0x1000000ULL, 7U);
    std::string data(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
    void *addresses[1] = {nullptr};
    EXPECT_EQ(segment.Import({data}, addresses), BM_OK);

    EXPECT_EQ(g_importCount, 1);
    EXPECT_EQ(g_reserveCount, 1);
    EXPECT_EQ(g_mapCount, 1);
    ASSERT_EQ(segment.vmmNameToHandle_.size(), 1U);
    EXPECT_NE(segment.vmmNameToHandle_.count(std::string(sliceInfo.name, USER_HBM_NAME_MAX_LEN)), 0U);

    auto remoteSlice = segment.rankToRemoteSlices_.at(7U).at(0);
    EXPECT_NE(remoteSlice->vAddress_, 0U);
    EXPECT_EQ(addresses[0], TestAddr(TEST_GVA_BASE + 0x1000000ULL));
    EXPECT_EQ(g_setAccessCount, 1); // import 映射后本地设备 RW 授权
}

/**
 * ImportSlice_A5_ReserveFail
 *  - 预留失败：imported handle 被释放，无映射残留。
 */
TEST_F(HybmAsymmetricSegmentTest, ImportSlice_A5_ReserveFail)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_950);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlHalApi::HalMemImport).stubs().will(invoke(HalMemImportStub));
    MOCKER(&ock::mf::DlHalApi::HalMemAddressReserve).stubs().will(invoke(HalMemAddressReserveFailStub));
    MOCKER(&ock::mf::DlHalApi::HalMemMap).stubs().will(invoke(HalMemMapStub));
    MOCKER(&ock::mf::DlHalApi::HalMemRelease).stubs().will(invoke(HalMemReleaseCountStub));

    auto sliceInfo = MakeVmmSliceInfo(0x1000000ULL, 7U);
    std::string data(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
    void *addresses[1] = {nullptr};
    EXPECT_EQ(segment.Import({data}, addresses), BM_ERROR);
    EXPECT_EQ(g_importCount, 1);
    EXPECT_EQ(g_vmmReleaseCount, 1); // imported handle 已释放
    EXPECT_TRUE(segment.vmmNameToHandle_.empty());
    EXPECT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 0U);
}

/**
 * ImportSlice_A5_MapFail_Rollback
 *  - map 失败：release + freeLva 均被调用，vmmNameToHandle_ 无残留。
 */
TEST_F(HybmAsymmetricSegmentTest, ImportSlice_A5_MapFail_Rollback)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_950);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MockVmmImportPath();
    g_mapRet = -1;

    auto sliceInfo = MakeVmmSliceInfo(0x1000000ULL, 7U);
    std::string data(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
    void *addresses[1] = {nullptr};
    EXPECT_EQ(segment.Import({data}, addresses), BM_DL_FUNCTION_FAILED);
    EXPECT_EQ(g_vmmReleaseCount, 1);
    EXPECT_TRUE(segment.vmmNameToHandle_.empty());
    EXPECT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 0U);
}

/**
 * ImportSlice_A5_InvalidNameType
 *  - name[0] 为未知类型值（0x3/0x0）：显式报错，不进入任何静默分支。
 */
TEST_F(HybmAsymmetricSegmentTest, ImportSlice_A5_InvalidNameType)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_950);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MockVmmImportPath();

    for (auto badType : {0x3U, 0x0U}) {
        auto sliceInfo = MakeVmmSliceInfo(0x1000000ULL, 7U);
        sliceInfo.name[0] = static_cast<char>(badType);
        std::string data(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
        void *addresses[1] = {nullptr};
        EXPECT_EQ(segment.Import({data}, addresses), BM_INVALID_PARAM);
        EXPECT_EQ(g_importCount, 0);
    }
    EXPECT_TRUE(segment.rankToRemoteSlices_.empty());
}

/**
 * ImportSlice_A5_MultiSliceSameRank
 *  - 同 rank 多 slice 依次导入：各自独立 reserve/map，handle 登记数量正确。
 */
TEST_F(HybmAsymmetricSegmentTest, ImportSlice_A5_MultiSliceSameRank)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_950);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MockVmmImportPath();

    auto info1 = MakeVmmSliceInfo(0x1000000ULL, 7U);
    auto info2 = MakeVmmSliceInfo(0x2000000ULL, 7U);
    std::string data1(reinterpret_cast<const char *>(&info1), sizeof(info1));
    std::string data2(reinterpret_cast<const char *>(&info2), sizeof(info2));
    void *addresses[2] = {nullptr, nullptr};
    EXPECT_EQ(segment.Import({data1, data2}, addresses), BM_OK);

    EXPECT_EQ(segment.vmmNameToHandle_.size(), 2U);
    EXPECT_EQ(segment.rankToRemoteSlices_.at(7U).size(), 2U);
    EXPECT_EQ(addresses[0], TestAddr(TEST_GVA_BASE + 0x1000000ULL));
    EXPECT_EQ(addresses[1], TestAddr(TEST_GVA_BASE + 0x2000000ULL));
}

// ====================== A5 VMM 共享路径：释放与生命周期 ======================

/**
 * RemoveImported_A5_VmmUnimport
 *  - 移除远端 rank：unmap/release 被调用，vmmNameToHandle_ 清空，LVA 预留归还。
 */
TEST_F(HybmAsymmetricSegmentTest, RemoveImported_A5_VmmUnimport)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_950);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MockVmmImportPath();

    auto sliceInfo = MakeVmmSliceInfo(0x1000000ULL, 7U);
    std::string data(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
    void *addresses[1] = {nullptr};
    ASSERT_EQ(segment.Import({data}, addresses), BM_OK);

    EXPECT_EQ(segment.RemoveImported({7U}), BM_OK);
    EXPECT_EQ(g_unmapCount, 1);
    EXPECT_EQ(g_vmmReleaseCount, 1);
    EXPECT_TRUE(segment.vmmNameToHandle_.empty());
    EXPECT_TRUE(segment.remoteSlices_.empty());
    EXPECT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 0U);
}

/**
 * RemoveImported_A5_Rejoin
 *  - 移除后同 rank 重新导入：二次成功，import/reserve 各两次、unmap/release 各一次，无泄漏。
 */
TEST_F(HybmAsymmetricSegmentTest, RemoveImported_A5_Rejoin)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_950);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MockVmmImportPath();

    auto sliceInfo = MakeVmmSliceInfo(0x1000000ULL, 7U);
    std::string data(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
    void *addresses[1] = {nullptr};
    ASSERT_EQ(segment.Import({data}, addresses), BM_OK);
    ASSERT_EQ(segment.RemoveImported({7U}), BM_OK);

    ASSERT_EQ(segment.Import({data}, addresses), BM_OK);
    EXPECT_EQ(g_importCount, 2);
    EXPECT_EQ(g_reserveCount, 2);
    EXPECT_EQ(g_unmapCount, 1);
    EXPECT_EQ(g_vmmReleaseCount, 1);
    EXPECT_EQ(segment.vmmNameToHandle_.size(), 1U);
}

/**
 * CloseMemory_A5_MixedTypes
 *  - 混合导入（VMM + IPC + DRAM）后段级 CloseMemory：三条释放路径互不误伤。
 */
TEST_F(HybmAsymmetricSegmentTest, CloseMemory_A5_MixedTypes)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_950);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MockVmmImportPath();
    MOCKER(&ock::mf::DlAclApi::RtIpcOpenMemory).stubs().will(invoke(RtIpcOpenMemoryStub));
    MOCKER(&ock::mf::DlAclApi::RtIpcCloseMemory).stubs().will(invoke(RtIpcCloseMemoryCountStub));

    auto vmmInfo = MakeVmmSliceInfo(0x1000000ULL, 7U);
    auto ipcInfo = MakeIpcSliceInfo(0x2000000ULL, 8U);
    UserSliceExportInfo dramInfo{};
    dramInfo.segmentType = SEGMENT_TYPE_USER_DRAM;
    dramInfo.gvaOffset = 0x3000000ULL;
    dramInfo.size = TEST_SIZE;
    dramInfo.rankId = 9U;
    dramInfo.devicePhyId = TEST_LOGIC_DEVICE_ID;
    std::string vmmData(reinterpret_cast<const char *>(&vmmInfo), sizeof(vmmInfo));
    std::string ipcData(reinterpret_cast<const char *>(&ipcInfo), sizeof(ipcInfo));
    std::string dramData(reinterpret_cast<const char *>(&dramInfo), sizeof(dramInfo));
    void *addresses[3] = {nullptr, nullptr, nullptr};
    ASSERT_EQ(segment.Import({vmmData, ipcData, dramData}, addresses), BM_OK);
    ASSERT_EQ(g_ipcOpenCount, 1);

    segment.CloseMemory();
    EXPECT_EQ(g_unmapCount, 1);      // VMM unmap
    EXPECT_EQ(g_vmmReleaseCount, 1); // VMM handle release
    EXPECT_EQ(g_ipcCloseCount, 1);   // IPC close
    EXPECT_TRUE(segment.vmmNameToHandle_.empty());
    EXPECT_TRUE(segment.remoteSlices_.empty());
}

/**
 * UnImport_UnregisteredOrRepeat_NoCrash
 *  - 未登记 sliceId 为 no-op；同一 slice 重复 unimport：二次进入 handle 缺失分支仅告警，
 *    不 erase 无效迭代器（防 UB 回归），VMM 登记保持一致。
 */
TEST_F(HybmAsymmetricSegmentTest, UnImport_UnregisteredOrRepeat_NoCrash)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_950);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MockVmmImportPath();

    segment.UserMemUnImportName(9999U); // 未登记 sliceId：no-op

    auto sliceInfo = MakeVmmSliceInfo(0x1000000ULL, 7U);
    std::string data(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
    void *addresses[1] = {nullptr};
    ASSERT_EQ(segment.Import({data}, addresses), BM_OK);
    ASSERT_EQ(segment.remoteSlices_.size(), 1U);
    const auto sliceId = segment.remoteSlices_.begin()->first;

    segment.UserMemUnImportName(sliceId);
    EXPECT_EQ(g_unmapCount, 1);
    EXPECT_TRUE(segment.vmmNameToHandle_.empty());
    segment.UserMemUnImportName(sliceId); // 重复 unimport：handle 缺失分支，仅告警不 erase(end())
    EXPECT_EQ(g_unmapCount, 2);
    EXPECT_EQ(g_vmmReleaseCount, 1);
}

// ====================== A5 VMM 共享路径：跨 SoC ======================

/**
 * Import_CrossSoc_A3ReceivesVmmName
 *  - 旧 SoC 收到 VMM 类型 name：HalMemImport 返回符号未加载错误，错误上抛不 crash。
 */
TEST_F(HybmAsymmetricSegmentTest, Import_CrossSoc_A3ReceivesVmmName)
{
    auto options = MakeOptions(HYBM_DOP_TYPE_SDMA, ASCEND_910C);
    AsymmetricMemSegment segment(options, TEST_ENTITY_ID);
    InitSegmentBase(segment);
    MOCKER(&ock::mf::DlHalApi::HalMemImport).stubs().will(invoke(HalMemImportUnloadStub));

    auto sliceInfo = MakeVmmSliceInfo(0x1000000ULL, 7U);
    std::string data(reinterpret_cast<const char *>(&sliceInfo), sizeof(sliceInfo));
    void *addresses[1] = {nullptr};
    EXPECT_EQ(segment.Import({data}, addresses), BM_DL_FUNCTION_FAILED);
    EXPECT_TRUE(segment.rankToRemoteSlices_.empty());
}
