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

#include <fcntl.h>
#include <linux/mempolicy.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <string>
#include <vector>

#define private   public
#define protected public
#include "hybm_conn_based_segment.h"
#include "hybm_shm_util.h"
#undef private
#undef protected

#include "hybm_ex_info_transfer.h"
#include "hybm_va_manager.h"
#include "vendor/ascend/dl_hal_api.h"
#include "mf_scope_guard.h"

using namespace ock::mf;

class HybmConnBasedSegmentTest : public testing::Test {
protected:
    static MemSegmentOptions MakeOptions(uint32_t rankCnt = 2U, uint32_t rankId = 1U)
    {
        MemSegmentOptions options{};
        options.segType = HYBM_MST_DRAM;
        options.maxSize = HYBM_LARGE_PAGE_SIZE;
        options.size = HYBM_LARGE_PAGE_SIZE;
        options.rankCnt = rankCnt;
        options.rankId = rankId;
        options.dataOpType = HYBM_DOP_TYPE_SDMA;
        return options;
    }

    static MemSlicePtr MakeHostSlice(uint16_t index, uint64_t gva, uint64_t hva, uint64_t size = HYBM_LARGE_PAGE_SIZE)
    {
        return std::make_shared<MemSlice>(index, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, gva, hva, size);
    }

    void AddHostVaInfo(uint64_t gva, uint64_t hva, uint64_t size, uint32_t rankId)
    {
        BaseAllocatedGvaInfo baseInfo{};
        baseInfo.va[HVM_GVA] = gva;
        baseInfo.va[HVM_HVA] = hva;
        baseInfo.size = size;
        baseInfo.memType = HYBM_MEM_TYPE_HOST;
        ASSERT_EQ(HybmVaManager::GetInstance().AddVaInfo(baseInfo), BM_OK);
    }

    void SetUp() override
    {
        GlobalMockObject::reset();
        HybmVaManager::GetInstance().ClearAll();
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
        HybmVaManager::GetInstance().ClearAll();
    }
};

/**
 * ValidateOptions_ChecksTypeAlignAndOverflow
 *  - 验证 DRAM 段的类型、页对齐和总大小溢出约束。
 */
TEST_F(HybmConnBasedSegmentTest, ValidateOptions_ChecksTypeAlignAndOverflow)
{
    auto options = MakeOptions();

    HybmConnBasedSegment segment(options, 0);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    options.segType = HYBM_MST_HBM;
    HybmConnBasedSegment badType(options, 0);
    EXPECT_EQ(badType.ValidateOptions(), BM_INVALID_PARAM);

    options = MakeOptions();
    options.maxSize = HYBM_LARGE_PAGE_SIZE / 2UL;
    HybmConnBasedSegment badAlign(options, 0);
    EXPECT_EQ(badAlign.ValidateOptions(), BM_INVALID_PARAM);

    options = MakeOptions();
    options.maxSize = (1ULL << 63U);
    options.rankCnt = 2U;
    HybmConnBasedSegment overflow(options, 0);
    EXPECT_EQ(overflow.ValidateOptions(), BM_INVALID_PARAM);
}

/**
 * ReserveAndUnreserveMemorySpace_ManageAddressWindow
 *  - 验证预留地址窗口成功，重复预留被拦截，并且可通过 UnReserve 回收。
 */
TEST_F(HybmConnBasedSegmentTest, ReserveAndUnreserveMemorySpace_ManageAddressWindow)
{
    auto options = MakeOptions(2U, 1U);
    HybmConnBasedSegment segment(options, 0);

    void *address = nullptr;
    ASSERT_EQ(segment.ReserveMemorySpace(&address), BM_OK);
    EXPECT_EQ(address, segment.globalVirtualAddress_);
    EXPECT_EQ(segment.totalVirtualSize_, options.rankCnt * options.maxSize);
    EXPECT_EQ(segment.localVirtualBase_, segment.globalVirtualAddress_ + options.maxSize * options.rankId);
    EXPECT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 1U);

    EXPECT_EQ(segment.ReserveMemorySpace(&address), BM_NOT_INITIALIZED);

    EXPECT_EQ(segment.UnReserveMemorySpace(), BM_OK);
    EXPECT_EQ(segment.globalVirtualAddress_, nullptr);
    EXPECT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 0U);
}

TEST_F(HybmConnBasedSegmentTest, FailedProviderImportPreservesReservedWindow)
{
    HybmConnBasedSegment segment(MakeOptions(1U, 0U), 0);
    void *base = nullptr;
    ASSERT_EQ(segment.ReserveMemorySpace(&base), BM_OK);
    HybmConnBasedSegment::ProviderShmFile file;
    file.fd = memfd_create("short-provider-pool", MFD_CLOEXEC);
    ASSERT_GE(file.fd, 0);
    file.mapSize = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    ASSERT_EQ(ftruncate(file.fd, file.mapSize), 0);
    HostExportInfo info{};
    info.gva = reinterpret_cast<uint64_t>(base);
    info.size = file.mapSize + 1;
    EXPECT_EQ(segment.MapLocalProviderShmInternal(info, file), BM_ERROR);
    void *mapped = mmap(base, file.mapSize, PROT_NONE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_EQ(mapped, MAP_FAILED);
    EXPECT_EQ(errno, EEXIST);
}

/**
 * LvaShmReservePhysicalMemory_TouchesEachLargePage
 *  - 验证按大页步长触碰物理页，并补写最后一个字节。
 */
TEST_F(HybmConnBasedSegmentTest, LvaShmReservePhysicalMemory_TouchesEachLargePage)
{
    std::vector<uint8_t> buffer(HYBM_LARGE_PAGE_SIZE * 2UL, 0x5AU);

    HybmConnBasedSegment::LvaShmReservePhysicalMemory(buffer.data(), buffer.size());

    EXPECT_EQ(buffer.front(), 0U);
    EXPECT_EQ(buffer[HYBM_LARGE_PAGE_SIZE], 0U);
    EXPECT_EQ(buffer.back(), 0U);
    EXPECT_EQ(buffer[1UL], 0x5AU);
}

/**
 * AllocLocalMemory_MapsOneSliceAndTracksVa
 *  - 验证本地分配会映射 slice、登记 VA，并更新内部计数。
 */
TEST_F(HybmConnBasedSegmentTest, AllocLocalMemory_MapsOneSliceAndTracksVa)
{
    auto options = MakeOptions(2U, 1U);
    HybmConnBasedSegment segment(options, 0);

    void *address = nullptr;
    ASSERT_EQ(segment.ReserveMemorySpace(&address), BM_OK);

    MemSlicePtr slice;
    ASSERT_EQ(segment.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice), BM_OK);
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->index_, 0U);
    EXPECT_EQ(slice->gva_,
              reinterpret_cast<uint64_t>(segment.globalVirtualAddress_) + options.maxSize * options.rankId);
    EXPECT_EQ(slice->vAddress_, reinterpret_cast<uint64_t>(segment.localVirtualBase_));
    EXPECT_EQ(segment.allocatedSize_, HYBM_LARGE_PAGE_SIZE);
    EXPECT_EQ(segment.slices_.count(slice->index_), 1U);
    EXPECT_EQ(HybmVaManager::GetInstance().GetAllocCount(), 1U);

    MemSlicePtr invalidSlice;
    EXPECT_EQ(segment.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE / 2UL, invalidSlice), BM_INVALID_PARAM);

    segment.FreeMemory();
}

/**
 * Export
 *  - 验证段级 Export、slice Export、序列化字段和 exportMap_ 缓存命中。
 */
TEST_F(HybmConnBasedSegmentTest, Export)
{
    auto options = MakeOptions(2U, 1U);

    HybmConnBasedSegment segment(options, 0);
    EXPECT_EQ(segment.ValidateOptions(), BM_OK);

    std::string segmentExInfo;
    EXPECT_EQ(segment.Export(segmentExInfo), BM_OK);

    constexpr uint16_t sliceIndex = 3U;
    constexpr uint64_t gva = 0x100000000ULL;
    constexpr uint64_t hva = 0x200000000ULL;
    auto slice = MakeHostSlice(sliceIndex, gva, hva);
    segment.slices_.emplace(slice->index_, MemSliceStatus(slice));
    AddHostVaInfo(gva, hva, slice->size_, options.rankId);

    std::string exInfo;
    ASSERT_EQ(segment.Export(slice, exInfo), BM_OK);
    ASSERT_EQ(segment.exportMap_.count(sliceIndex), 1U);
    EXPECT_EQ(segment.exportMap_.at(sliceIndex), exInfo);

    HostExportInfo exportedInfo{};
    ASSERT_EQ(LiteralExInfoTranslater<HostExportInfo>{}.Deserialize(exInfo, exportedInfo), 0);
    EXPECT_EQ(exportedInfo.magic, DRAM_SLICE_EXPORT_INFO_MAGIC);
    EXPECT_EQ(exportedInfo.version, EXPORT_INFO_VERSION);
    EXPECT_EQ(exportedInfo.gva, gva);
    EXPECT_EQ(exportedInfo.sliceIndex, sliceIndex);
    EXPECT_EQ(exportedInfo.rankId, options.rankId);
    EXPECT_EQ(exportedInfo.size, HYBM_LARGE_PAGE_SIZE);
    EXPECT_EQ(exportedInfo.pageTblType, MEM_PT_TYPE_SVM);
    EXPECT_EQ(exportedInfo.memSegType, HYBM_MST_DRAM);
    EXPECT_EQ(exportedInfo.exchangeType, HYBM_INFO_EXG_IN_NODE);

    std::string cachedExInfo = "cache_miss";
    EXPECT_EQ(segment.Export(slice, cachedExInfo), BM_OK);
    EXPECT_EQ(cachedExInfo, exInfo);
}

TEST_F(HybmConnBasedSegmentTest, Export_AfterRereserveUsesCurrentSlice)
{
    auto options = MakeOptions();
    options.maxSize = options.size = HYBM_LARGE_PAGE_SIZE * 2UL;
    HybmConnBasedSegment segment(options, 0);

    for (auto size : {HYBM_LARGE_PAGE_SIZE, HYBM_LARGE_PAGE_SIZE * 2UL}) {
        void *address = nullptr;
        ASSERT_EQ(segment.ReserveMemorySpace(&address), BM_OK);
        MemSlicePtr slice;
        ASSERT_EQ(segment.AllocLocalMemory(size, slice), BM_OK);
        ASSERT_EQ(slice->index_, 0U);

        std::string exInfo;
        ASSERT_EQ(segment.Export(slice, exInfo), BM_OK);
        HostExportInfo info{};
        ASSERT_EQ(LiteralExInfoTranslater<HostExportInfo>{}.Deserialize(exInfo, info), BM_OK);
        EXPECT_EQ(info.size, size);
        EXPECT_EQ(info.gva, slice->gva_);
        ASSERT_EQ(segment.UnReserveMemorySpace(), BM_OK);
    }
}

/**
 * Import_StoresDeserializedHostExportInfo
 *  - 验证导入信息会被反序列化后落入 imports_。
 */
TEST_F(HybmConnBasedSegmentTest, Import_StoresDeserializedHostExportInfo)
{
    HybmConnBasedSegment segment(MakeOptions(), 0);

    HostExportInfo info{};
    info.gva = 0x300000000ULL;
    info.sliceIndex = 2U;
    info.rankId = 0;
    info.size = HYBM_LARGE_PAGE_SIZE;

    std::string exInfo;
    ASSERT_EQ(LiteralExInfoTranslater<HostExportInfo>{}.Serialize(info, exInfo), BM_OK);

    void *addresses[1U] = {nullptr};
    std::vector<ImportEntryKind> entryKinds(1U);
    ASSERT_EQ(segment.Import({exInfo}, addresses, HYBM_IMPORT_RANK_INVALID, entryKinds), BM_OK);
    ASSERT_EQ(segment.imports_.size(), 1U);
    EXPECT_EQ(segment.imports_[0].gva, info.gva);
    EXPECT_EQ(segment.imports_[0].sliceIndex, info.sliceIndex);
    EXPECT_EQ(segment.imports_[0].rankId, info.rankId);
    EXPECT_EQ(segment.imports_[0].size, info.size);
}

/**
 * MmapAndUnmap_RegisterAndClearImportedGva
 *  - 验证只为远端 rank 建立 VA 映射，并在 Unmap 时清空登记。
 */
TEST_F(HybmConnBasedSegmentTest, MmapAndUnmap_RegisterAndClearImportedGva)
{
    auto options = MakeOptions(3U, 1U);
    HybmConnBasedSegment segment(options, 0);
    // gva is always base + rank*maxSize + delta, so a real base address is required and the address must fall
    // inside the window of its rank
    std::vector<uint8_t> buffer(options.rankCnt * options.maxSize);
    segment.globalVirtualAddress_ = buffer.data();

    HostExportInfo local{};
    local.gva = reinterpret_cast<uint64_t>(buffer.data()) + options.maxSize * options.rankId;
    local.rankId = options.rankId;
    local.size = HYBM_LARGE_PAGE_SIZE;

    HostExportInfo remote{};
    remote.gva = reinterpret_cast<uint64_t>(buffer.data()) + options.maxSize * 2UL;
    remote.rankId = 2U;
    remote.size = HYBM_LARGE_PAGE_SIZE;

    segment.imports_.push_back(local);
    segment.imports_.push_back(remote);

    ASSERT_EQ(segment.Mmap(), BM_OK);
    EXPECT_TRUE(segment.imports_.empty());
    EXPECT_EQ(segment.mappedGvaMem_.size(), 1U);
    EXPECT_EQ(*segment.mappedGvaMem_.begin(), remote.gva);

    auto found = HybmVaManager::GetInstance().FindAllocByVa(remote.gva, HVM_GVA);
    ASSERT_TRUE(found.second);
    EXPECT_TRUE(found.first.IsImported());
    uint32_t remoteRank = 0;
    EXPECT_TRUE(segment.FindRemoteRankByVa(remote.gva, remoteRank));
    EXPECT_EQ(remoteRank, remote.rankId);

    ASSERT_EQ(segment.Unmap(), BM_OK);
    EXPECT_TRUE(segment.mappedGvaMem_.empty());
    EXPECT_FALSE(HybmVaManager::GetInstance().FindAllocByVa(remote.gva, HVM_GVA).second);
}

/**
 * GetMemSlice_ValidatesIdBeforeReturningSlice
 *  - 验证索引命中后还会校验 slice id，避免错误对象冒用。
 */
TEST_F(HybmConnBasedSegmentTest, GetMemSlice_ValidatesIdBeforeReturningSlice)
{
    HybmConnBasedSegment segment(MakeOptions(), 0);

    auto slice = MakeHostSlice(7U, 0x1000ULL, 0x2000ULL);
    segment.slices_.emplace(slice->index_, MemSliceStatus(slice));

    EXPECT_EQ(segment.GetMemSlice(slice->ConvertToId(), false), slice);

    auto fakeSameIndex = MakeHostSlice(slice->index_, 0x3000ULL, 0x4000ULL);
    EXPECT_EQ(segment.GetMemSlice(fakeSameIndex->ConvertToId(), true), nullptr);
    EXPECT_EQ(segment.GetMemSlice(nullptr, true), nullptr);
}

/**
 * MemoryInRange_ChecksBeginAndEndBounds
 *  - 验证范围判断既检查起点，也检查尾部是否越界。
 */
TEST_F(HybmConnBasedSegmentTest, MemoryInRange_ChecksBeginAndEndBounds)
{
    HybmConnBasedSegment segment(MakeOptions(), 0);
    std::vector<uint8_t> buffer(HYBM_LARGE_PAGE_SIZE * 2UL);

    segment.globalVirtualAddress_ = buffer.data();
    segment.totalVirtualSize_ = buffer.size();

    EXPECT_TRUE(segment.MemoryInRange(buffer.data(), buffer.size()));
    EXPECT_TRUE(segment.MemoryInRange(buffer.data() + 128UL, HYBM_LARGE_PAGE_SIZE));
    EXPECT_FALSE(segment.MemoryInRange(buffer.data() + HYBM_LARGE_PAGE_SIZE, HYBM_LARGE_PAGE_SIZE + 1UL));

    segment.globalVirtualAddress_ = nullptr;
    segment.totalVirtualSize_ = 0;
}

/**
 * FreeMemory_ReleasesSlicesAndReservedWindow
 *  - 验证释放时会清空 slices_、VA 登记和预留地址窗口。
 */
TEST_F(HybmConnBasedSegmentTest, FreeMemory_ReleasesSlicesAndReservedWindow)
{
    auto options = MakeOptions(2U, 0);
    HybmConnBasedSegment segment(options, 0);

    void *address = nullptr;
    ASSERT_EQ(segment.ReserveMemorySpace(&address), BM_OK);

    MemSlicePtr slice;
    ASSERT_EQ(segment.AllocLocalMemory(HYBM_LARGE_PAGE_SIZE, slice), BM_OK);
    ASSERT_EQ(HybmVaManager::GetInstance().GetAllocCount(), 1U);
    ASSERT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 1U);

    segment.FreeMemory();

    EXPECT_TRUE(segment.slices_.empty());
    EXPECT_EQ(segment.globalVirtualAddress_, nullptr);
    EXPECT_EQ(segment.localVirtualBase_, nullptr);
    EXPECT_EQ(HybmVaManager::GetInstance().GetAllocCount(), 0U);
    EXPECT_EQ(HybmVaManager::GetInstance().GetReservedCount(), 0U);
}

/**
 * PrepareShareMemoryFd_ExpandsBackingFileToRequestedSize
 *  - 验证共享内存 fd 会被扩容到 options.size。
 */
TEST_F(HybmConnBasedSegmentTest, PrepareShareMemoryFd_ExpandsBackingFileToRequestedSize)
{
    char path[] = "/tmp/hybm_conn_based_segment_ut_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(unlink(path), 0);

    auto options = MakeOptions();
    options.shmFd = fd;
    options.size = HYBM_LARGE_PAGE_SIZE;
    HybmConnBasedSegment segment(options, 0);

    struct stat fileStat {};
    ASSERT_EQ(fstat(fd, &fileStat), 0);
    EXPECT_EQ(fileStat.st_size, 0);

    EXPECT_EQ(segment.PrepareShareMemoryFd(), BM_OK);

    ASSERT_EQ(fstat(fd, &fileStat), 0);
    EXPECT_EQ(fileStat.st_size, static_cast<off_t>(options.size));

    close(fd);
}

/**
 * MapSlice_SizeZeroReturnsOk
 *  - 验证零长度 slice 直接返回成功，不触发实际映射。
 */
TEST_F(HybmConnBasedSegmentTest, MapSlice_SizeZeroReturnsOk)
{
    HybmConnBasedSegment segment(MakeOptions(), 0);
    void *mapped = reinterpret_cast<void *>(0x1234ULL);
    auto method = MemAllocMethod::MMAP;

    EXPECT_EQ(segment.MapSlice(mapped, nullptr, 0, 0, 0x1000ULL, method), BM_OK);
    EXPECT_EQ(mapped, reinterpret_cast<void *>(0x1234ULL));
}

/**
 * RemoveImported_RemovesSpecifiedRanksFromMapAndImports
 *  - 验证只清理指定 rank 的 imports_ 和 mappedGvaMem_。
 */
TEST_F(HybmConnBasedSegmentTest, RemoveImported_RemovesSpecifiedRanksFromMapAndImports)
{
    auto options = MakeOptions(4U, 0);
    HybmConnBasedSegment segment(options, 0);
    std::vector<uint8_t> buffer(options.rankCnt * options.maxSize);

    segment.globalVirtualAddress_ = buffer.data();

    auto rank1Gva = reinterpret_cast<uint64_t>(buffer.data() + options.maxSize);
    auto rank2Gva = reinterpret_cast<uint64_t>(buffer.data() + options.maxSize * 2UL);

    BaseAllocatedGvaInfo rank1Info{};
    rank1Info.va[HVM_GVA] = rank1Gva;
    rank1Info.size = HYBM_LARGE_PAGE_SIZE;
    rank1Info.memType = HYBM_MEM_TYPE_HOST;
    ASSERT_EQ(HybmVaManager::GetInstance().AddVaInfo(rank1Info, false, AddressOwnership::Imported), BM_OK);

    BaseAllocatedGvaInfo rank2Info{};
    rank2Info.va[HVM_GVA] = rank2Gva;
    rank2Info.size = HYBM_LARGE_PAGE_SIZE;
    rank2Info.memType = HYBM_MEM_TYPE_HOST;
    ASSERT_EQ(HybmVaManager::GetInstance().AddVaInfo(rank2Info, false, AddressOwnership::Imported), BM_OK);

    HostExportInfo import1{};
    import1.gva = rank1Gva;
    import1.rankId = 1U;
    import1.size = HYBM_LARGE_PAGE_SIZE;

    HostExportInfo import2{};
    import2.gva = rank2Gva;
    import2.rankId = 2U;
    import2.size = HYBM_LARGE_PAGE_SIZE;

    segment.imports_.push_back(import1);
    segment.imports_.push_back(import2);
    segment.mappedGvaMem_.insert(rank1Gva);
    segment.mappedGvaMem_.insert(rank2Gva);

    ASSERT_EQ(segment.RemoveImported({1U}), BM_OK);
    EXPECT_EQ(segment.imports_.size(), 1U);
    EXPECT_EQ(segment.imports_[0].rankId, 2U);
    EXPECT_EQ(segment.mappedGvaMem_.count(rank1Gva), 0U);
    EXPECT_EQ(segment.mappedGvaMem_.count(rank2Gva), 1U);
    EXPECT_FALSE(HybmVaManager::GetInstance().FindAllocByVa(rank1Gva, HVM_GVA).second);
    EXPECT_TRUE(HybmVaManager::GetInstance().FindAllocByVa(rank2Gva, HVM_GVA).second);

    EXPECT_EQ(segment.RemoveImported({options.rankCnt}), BM_INVALID_PARAM);

    segment.globalVirtualAddress_ = nullptr;
}

/**
 * RegisterMemory_AddsDeviceSliceIntoSegment
 *  - 验证 RegisterMemory 会登记一块现成地址并写入 slices_。
 */
TEST_F(HybmConnBasedSegmentTest, RegisterMemory_AddsDeviceSliceIntoSegment)
{
    HybmVaManager::GetInstance().ClearAll();
    ASSERT_EQ(HybmVaManager::GetInstance().Initialize(AscendSocType::ASCEND_910C), BM_OK);
    HybmConnBasedSegment segment(MakeOptions(), 0);
    auto *deviceAddr = reinterpret_cast<void *>(HYBM_HBM_START_ADDR + HYBM_LARGE_PAGE_SIZE);

    MemSlicePtr slice;
    ASSERT_EQ(segment.RegisterMemory(deviceAddr, 4096ULL, slice), BM_OK);
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->memType_, HYBM_MEM_TYPE_DEVICE);
    EXPECT_EQ(segment.slices_.count(slice->index_), 1U);

    auto found = HybmVaManager::GetInstance().FindAllocByVa(reinterpret_cast<uint64_t>(deviceAddr), HVM_HVA);
    ASSERT_TRUE(found.second);
    EXPECT_EQ(found.first.base.va[HVM_HVA], reinterpret_cast<uint64_t>(deviceAddr));
}

/**
 * ReleaseSliceMemory_RemovesRegisteredSliceAndVaInfo
 *  - 验证释放已登记 slice 时会同步清理 slices_ 和 VA 管理器。
 */
TEST_F(HybmConnBasedSegmentTest, ReleaseSliceMemory_RemovesRegisteredSliceAndVaInfo)
{
    HybmVaManager::GetInstance().ClearAll();
    ASSERT_EQ(HybmVaManager::GetInstance().Initialize(AscendSocType::ASCEND_910C), BM_OK);
    HybmConnBasedSegment segment(MakeOptions(), 0);
    auto *deviceAddr = reinterpret_cast<void *>(HYBM_HBM_START_ADDR + HYBM_LARGE_PAGE_SIZE * 2UL);

    MemSlicePtr slice;
    ASSERT_EQ(segment.RegisterMemory(deviceAddr, 4096ULL, slice), BM_OK);
    ASSERT_TRUE(HybmVaManager::GetInstance().FindAllocByVa(reinterpret_cast<uint64_t>(deviceAddr), HVM_HVA).second);

    EXPECT_EQ(segment.ReleaseSliceMemory(slice), BM_OK);
    EXPECT_TRUE(segment.slices_.empty());
    EXPECT_FALSE(HybmVaManager::GetInstance().FindAllocByVa(reinterpret_cast<uint64_t>(deviceAddr), HVM_HVA).second);
    EXPECT_EQ(segment.ReleaseSliceMemory(slice), BM_INVALID_PARAM);
}

TEST_F(HybmConnBasedSegmentTest, ReleaseRegisteredHostMemory_UnregistersDeviceForProviderAndProxy)
{
    ASSERT_EQ(HybmVaManager::GetInstance().Initialize(AscendSocType::ASCEND_910C), BM_OK);
    constexpr uint64_t hostAddress = 0x7F1234000000ULL;
    constexpr uint64_t bufferSize = 4096ULL;
    constexpr uint32_t deviceId = 7;
    constexpr uint32_t registrationCount = 4;
    MOCKER(&DlHalApi::HalHostRegister).expects(exactly(registrationCount)).will(returnValue(0));
    MOCKER(&DlHalApi::HalHostUnregisterEx)
        .expects(exactly(registrationCount))
        .with(eq(reinterpret_cast<void *>(hostAddress)), eq(deviceId), eq(static_cast<uint32_t>(HOST_MEM_MAP_DEV)))
        .will(returnValue(0));
    auto options = MakeOptions();
    options.dataOpType = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_HOST_SHM | HYBM_DOP_TYPE_DEVICE_URMA);
    for (auto role : {HYBM_CLUSTER_ROLE_PROVIDER, HYBM_CLUSTER_ROLE_PROXY}) {
        HybmConnBasedSegment segment(options, 0);
        segment.clusterRole_ = role;
        segment.logicDeviceId_ = deviceId;
        for (uint32_t attempt = 0; attempt < 2; ++attempt) {
            MemSlicePtr slice;
            ASSERT_EQ(segment.RegisterMemory(reinterpret_cast<void *>(hostAddress), bufferSize, slice), BM_OK);
            ASSERT_EQ(segment.ReleaseSliceMemory(slice), BM_OK);
            EXPECT_TRUE(segment.slices_.empty());
            EXPECT_FALSE(HybmVaManager::GetInstance().FindAllocByVa(hostAddress, HVM_HVA).second);
        }
    }
}

TEST_F(HybmConnBasedSegmentTest, ReleaseProviderPoolAndProxyDeviceSlice_DoesNotUnregisterHost)
{
    MOCKER(&DlHalApi::HalHostUnregisterEx).expects(never());
    auto options = MakeOptions();
    options.dataOpType = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_HOST_SHM | HYBM_DOP_TYPE_DEVICE_URMA);
    HybmConnBasedSegment provider(options, 0);
    provider.clusterRole_ = HYBM_CLUSTER_ROLE_PROVIDER;
    auto pool = MakeHostSlice(0, HYBM_LARGE_PAGE_SIZE, HYBM_LARGE_PAGE_SIZE);
    provider.slices_.emplace(pool->index_, pool);
    EXPECT_EQ(provider.ReleaseSliceMemory(pool), BM_OK);
    HybmConnBasedSegment proxy(options, 0);
    proxy.clusterRole_ = HYBM_CLUSTER_ROLE_PROXY;
    auto deviceSlice = std::make_shared<MemSlice>(0, HYBM_MEM_TYPE_DEVICE, MEM_PT_TYPE_SVM, 0, HYBM_HBM_START_ADDR,
                                                  HYBM_LARGE_PAGE_SIZE);
    proxy.slices_.emplace(deviceSlice->index_, deviceSlice);
    EXPECT_EQ(proxy.ReleaseSliceMemory(deviceSlice), BM_OK);
}

/**
 * GetExportSliceSize_ReturnsHostExportInfoSize
 *  - 验证导出描述大小与 HostExportInfo 结构体保持一致。
 */
TEST_F(HybmConnBasedSegmentTest, GetExportSliceSize_ReturnsHostExportInfoSize)
{
    HybmConnBasedSegment segment(MakeOptions(), 0);
    size_t size = 0;

    EXPECT_EQ(segment.GetExportSliceSize(size), BM_OK);
    EXPECT_EQ(size, sizeof(HostExportInfo));
}

TEST_F(HybmConnBasedSegmentTest, UnReserveMemorySpace_NoInit_NoCrash)
{
    HybmConnBasedSegment seg(MakeOptions(), 0);
    auto ret = seg.UnReserveMemorySpace();
    EXPECT_EQ(ret, BM_OK);
}

TEST_F(HybmConnBasedSegmentTest, CheckSdmaReaches_Default_ReturnsFalse)
{
    HybmConnBasedSegment seg(MakeOptions(), 0);
    EXPECT_FALSE(seg.CheckSdmaReaches(0));
}

TEST_F(HybmConnBasedSegmentTest, MemoryInRange_NullAddr_ReturnsFalse)
{
    HybmConnBasedSegment seg(MakeOptions(), 0);
    EXPECT_FALSE(seg.MemoryInRange(nullptr, 1));
}

TEST_F(HybmConnBasedSegmentTest, Export_Empty_ReturnsOk)
{
    HybmConnBasedSegment seg(MakeOptions(), 0);
    std::string info;
    EXPECT_EQ(seg.Export(info), BM_OK);
}

TEST_F(HybmConnBasedSegmentTest, Export_NullSlice_ReturnsInvalidParam)
{
    HybmConnBasedSegment seg(MakeOptions(), 0);
    MemSlicePtr nullSlice;
    std::string info;
    auto ret = seg.Export(nullSlice, info);
    EXPECT_NE(ret, BM_OK);
}

namespace {
constexpr size_t NUMA_TEST_MAX_NODES = 1024;
using NumaTestMask = std::array<unsigned long, NUMA_TEST_MAX_NODES / (8 * sizeof(unsigned long))>;
int observedProviderPolicy;
NumaTestMask observedProviderMask{};
bool failProviderMmap;

void *ObserveProviderMmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (fd != 123 || length != HYBM_LARGE_PAGE_SIZE) {
        return reinterpret_cast<void *>(syscall(SYS_mmap, address, length, prot, flags, fd, offset));
    }
    observedProviderMask.fill(0);
    EXPECT_EQ(syscall(SYS_get_mempolicy, &observedProviderPolicy, observedProviderMask.data(), NUMA_TEST_MAX_NODES,
                      nullptr, 0),
              0);
    if (failProviderMmap) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return address;
}
} // namespace

class HybmProviderNumaPolicyTest : public HybmConnBasedSegmentTest {
protected:
    void SetUp() override
    {
        HybmConnBasedSegmentTest::SetUp();
        if (syscall(SYS_get_mempolicy, &savedPolicy_, savedMask_.data(), NUMA_TEST_MAX_NODES, nullptr, 0) != 0 ||
            syscall(SYS_get_mempolicy, nullptr, allowedMask_.data(), NUMA_TEST_MAX_NODES, nullptr,
                    MPOL_F_MEMS_ALLOWED) != 0) {
            GTEST_SKIP() << "NUMA policy syscalls unavailable";
        }
        size_t nodes = 0;
        for (auto word : allowedMask_) {
            nodes += __builtin_popcountl(word);
        }
        if (nodes < 2) {
            GTEST_SKIP() << "Requires two allowed memory nodes";
        }
        if (syscall(SYS_set_mempolicy, MPOL_DEFAULT, nullptr, 0) != 0) {
            GTEST_SKIP() << "Cannot set thread memory policy";
        }
        restorePolicy_ = true;
        observedProviderPolicy = -1;
        failProviderMmap = false;
        MOCKCPP_NS::mockAPI("TryHugetlbfsAvailable", reinterpret_cast<bool (*)()>(&TryHugetlbfsAvailable))
            .stubs()
            .will(returnValue(false));
        MOCKCPP_NS::mockAPI("OpenAndLockLocalShm",
                            reinterpret_cast<ock::mf::Result (*)(HybmConnBasedSegment *, const std::string &, int &)>(
                                &HybmConnBasedSegment::OpenAndLockLocalShm))
            .stubs()
            .will(invoke(+[](HybmConnBasedSegment *, const std::string &, int &fd) {
                fd = 123;
                return BM_OK;
            }));
        auto truncateCall = static_cast<int (*)(int, off_t)>(ftruncate);
        auto closeCall = static_cast<int (*)(int)>(close);
        auto unlinkCall = static_cast<int (*)(const char *)>(unlink);
        auto unmapCall = static_cast<int (*)(void *, size_t)>(munmap);
        auto mapCall = static_cast<void *(*)(void *, size_t, int, int, int, off_t)>(mmap);
        MOCKER(truncateCall).stubs().will(returnValue(0));
        MOCKER(closeCall).stubs().will(
            invoke(+[](int fd) { return fd == 123 ? 0 : static_cast<int>(syscall(SYS_close, fd)); }));
        MOCKER(unlinkCall).stubs().will(returnValue(0));
        MOCKER(unmapCall).stubs().will(returnValue(0));
        MOCKER(mapCall).stubs().will(invoke(ObserveProviderMmap));
    }

    void TearDown() override
    {
        if (restorePolicy_) {
            EXPECT_EQ(syscall(SYS_set_mempolicy, savedPolicy_,
                              savedPolicy_ == MPOL_DEFAULT ? nullptr : savedMask_.data(),
                              savedPolicy_ == MPOL_DEFAULT ? 0 : NUMA_TEST_MAX_NODES),
                      0);
        }
        HybmConnBasedSegmentTest::TearDown();
    }

    ock::mf::Result MapProvider(hybm_data_op_type transport = HYBM_DOP_TYPE_DEVICE_RDMA, uint32_t flags = 0)
    {
        auto options = MakeOptions(1, 0);
        options.dataOpType = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_HOST_SHM | transport);
        options.flags = flags;
        HybmConnBasedSegment segment(options, 0);
        segment.clusterRole_ = HYBM_CLUSTER_ROLE_PROVIDER;
        segment.localVirtualBase_ = reinterpret_cast<uint8_t *>(0x400000000000ULL);
        return segment.MapProviderShm();
    }

    int CurrentPolicy()
    {
        int policy = -1;
        EXPECT_EQ(syscall(SYS_get_mempolicy, &policy, nullptr, 0, nullptr, 0), 0);
        return policy;
    }

    NumaTestMask allowedMask_{};

private:
    int savedPolicy_{MPOL_DEFAULT};
    NumaTestMask savedMask_{};
    bool restorePolicy_{false};
};

TEST_F(HybmProviderNumaPolicyTest, ProviderInterleavesPoolAndRestoresThreadPolicy)
{
    EXPECT_EQ(MapProvider(), BM_OK);
    EXPECT_EQ(observedProviderPolicy, MPOL_INTERLEAVE);
    EXPECT_EQ(observedProviderMask, allowedMask_);
    EXPECT_EQ(CurrentPolicy(), MPOL_DEFAULT);
}

TEST_F(HybmProviderNumaPolicyTest, FailedMappingRestoresThreadPolicy)
{
    failProviderMmap = true;
    EXPECT_EQ(MapProvider(), BM_ERROR);
    EXPECT_EQ(observedProviderPolicy, MPOL_INTERLEAVE);
    EXPECT_EQ(CurrentPolicy(), MPOL_DEFAULT);
}

TEST_F(HybmProviderNumaPolicyTest, PerformanceModeKeepsOriginalAllocation)
{
    EXPECT_EQ(MapProvider(HYBM_DOP_TYPE_DEVICE_RDMA, 1U << HYBM_PERFORMANCE_MODE_FLAG_INDEX), BM_OK);
    EXPECT_EQ(observedProviderPolicy, MPOL_DEFAULT);
    EXPECT_EQ(CurrentPolicy(), MPOL_DEFAULT);
}

TEST_F(HybmProviderNumaPolicyTest, ExplicitBindingIsPreserved)
{
    NumaTestMask bound{};
    for (size_t i = 0; i < allowedMask_.size(); ++i) {
        if (allowedMask_[i] != 0) {
            bound[i] = allowedMask_[i] & (0UL - allowedMask_[i]);
            break;
        }
    }
    ASSERT_EQ(syscall(SYS_set_mempolicy, MPOL_BIND, bound.data(), NUMA_TEST_MAX_NODES), 0);
    EXPECT_EQ(MapProvider(), BM_OK);
    EXPECT_EQ(observedProviderPolicy, MPOL_BIND);
    EXPECT_EQ(observedProviderMask, bound);
    EXPECT_EQ(CurrentPolicy(), MPOL_BIND);
}

TEST_F(HybmProviderNumaPolicyTest, UrmaAllocationIsUnchanged)
{
    EXPECT_EQ(MapProvider(HYBM_DOP_TYPE_DEVICE_URMA), BM_OK);
    EXPECT_EQ(observedProviderPolicy, MPOL_DEFAULT);
    EXPECT_EQ(CurrentPolicy(), MPOL_DEFAULT);
}

namespace {
std::string providerPoolTestPath;

void UseTemporaryProviderPoolPath(const std::string &path)
{
    providerPoolTestPath = path;
    MOCKCPP_NS::mockAPI("GetProviderShmFilePath", reinterpret_cast<std::string (*)(const HybmConnBasedSegment *, bool)>(
                                                      &HybmConnBasedSegment::GetProviderShmFilePath))
        .stubs()
        .will(invoke(
            +[](const HybmConnBasedSegment *, bool huge) { return providerPoolTestPath + (huge ? ".huge" : ""); }));
    MOCKCPP_NS::mockAPI("TryHugetlbfsAvailable", reinterpret_cast<bool (*)()>(&TryHugetlbfsAvailable))
        .stubs()
        .will(returnValue(false));
}
} // namespace

TEST_F(HybmConnBasedSegmentTest, ReplacedInodeRemapsNewPoolDespitePreopenedOldDescriptor)
{
    char path[] = "/tmp/mf-provider-inode-XXXXXX";
    int original = mkstemp(path);
    ASSERT_GE(original, 0);
    auto guard = make_scope_guard(original, [path = std::string(path)](int fd) {
        close(fd);
        unlink(path.c_str());
        unlink((path + ".replacement").c_str());
    });
    const auto size = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    ASSERT_EQ(ftruncate(original, size), 0);
    char oldByte = 'A';
    ASSERT_EQ(pwrite(original, &oldByte, 1, 0), 1);
    UseTemporaryProviderPoolPath(path);
    auto options = MakeOptions(2, 1);
    options.size = 0;
    options.dataOpType = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_HOST_SHM | HYBM_DOP_TYPE_DEVICE_RDMA);
    HybmConnBasedSegment segment(options, 0);
    auto identityGuard = make_scope_guard(segment.machineId_, [](MachineId saved) { MemSegment::machineId_ = saved; });
    segment.machineId_ = MachineId{1, 2};
    void *base = nullptr;
    ASSERT_EQ(segment.ReserveMemorySpace(&base), BM_OK);
    HostExportInfo info{};
    info.gva = reinterpret_cast<uint64_t>(base);
    info.size = size;
    info.rankId = 0;
    info.machineId = segment.machineId_;
    info.memSegType = HYBM_DRAM_PROVIDER;
    std::vector<ImportEntryKind> kinds(1);
    HybmConnBasedSegment::ProviderShmFile first;
    ASSERT_EQ(segment.PreOpenLocalProviderShm({info}, first), BM_OK);
    ASSERT_EQ(segment.ImportHostProvider(0, info, kinds, first), BM_OK);
    ASSERT_EQ(*segment.localShmBase_, static_cast<uint8_t>('A'));
    EXPECT_EQ(first.fd, -1);
    const auto oldInode = segment.providerShmIno_;

    HybmConnBasedSegment::ProviderShmFile preopened;
    ASSERT_EQ(segment.OpenLocalProviderShm(preopened, 0), BM_OK);
    ASSERT_EQ(preopened.ino, oldInode);
    const auto replacement = std::string(path) + ".replacement";
    int fresh = open(replacement.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    ASSERT_GE(fresh, 0);
    auto freshGuard = make_scope_guard(fresh, [](int fd) { close(fd); });
    ASSERT_EQ(ftruncate(fresh, size), 0);
    char newByte = 'B';
    ASSERT_EQ(pwrite(fresh, &newByte, 1, 0), 1);
    ASSERT_EQ(rename(replacement.c_str(), path), 0);
    ASSERT_EQ(segment.ImportHostProvider(0, info, kinds, preopened), BM_OK);
    EXPECT_NE(segment.providerShmIno_, oldInode);
    EXPECT_EQ(*segment.localShmBase_, static_cast<uint8_t>('B'));
    EXPECT_EQ(preopened.fd, -1);
    EXPECT_EQ(kinds[0], ImportEntryKind::ProviderPool);
    EXPECT_EQ(segment.GetAddressScope(info.gva), HybmConnBasedSegment::AddressScope::LOCAL_PROVIDER_SHM);
    EXPECT_EQ(segment.GetAddressScope(info.gva + size), HybmConnBasedSegment::AddressScope::UNKNOWN);
    HybmConnBasedSegment::ProviderShmFile duplicate;
    EXPECT_EQ(segment.ImportHostProvider(0, info, kinds, duplicate), BM_OK);
    EXPECT_EQ(*segment.localShmBase_, static_cast<uint8_t>('B'));
    auto conflicting = info;
    conflicting.size--;
    EXPECT_EQ(segment.ImportHostProvider(0, conflicting, kinds, duplicate), BM_INVALID_PARAM);
    EXPECT_EQ(segment.TeardownLocalProviderShm(), BM_OK);
    EXPECT_FALSE(segment.LocalProviderMapped());
    EXPECT_EQ(segment.importedShmFd_, -1);
}

TEST_F(HybmConnBasedSegmentTest, ProviderPoolFilesDistinguishNotReadyLiveAndStalePools)
{
    char path[] = "/tmp/mf-provider-lock-XXXXXX";
    int original = mkstemp(path);
    ASSERT_GE(original, 0);
    auto guard = make_scope_guard(original, [path = std::string(path)](int fd) {
        close(fd);
        unlink(path.c_str());
    });
    UseTemporaryProviderPoolPath(path);
    HybmConnBasedSegment segment(MakeOptions(), 0);
    HybmConnBasedSegment::ProviderShmFile file;
    EXPECT_EQ(segment.OpenProviderShm(path, file), BM_NOT_INITIALIZED);
    ASSERT_EQ(ftruncate(original, HYBM_LARGE_PAGE_SIZE * 2), 0);
    ASSERT_EQ(segment.OpenProviderShmWithRetry(path, file), BM_OK);
    EXPECT_EQ(file.mapSize, HYBM_LARGE_PAGE_SIZE);
    close(file.fd);
    file.fd = -1;
    int acquired = -1;
    EXPECT_EQ(segment.OpenAndLockLocalShm(path, acquired), BM_NOT_INITIALIZED);
    ASSERT_EQ(flock(original, LOCK_EX | LOCK_NB), 0);
    EXPECT_FALSE(segment.CleanupStaleLocalShm(path));
    ASSERT_EQ(flock(original, LOCK_UN), 0);
    EXPECT_TRUE(segment.CleanupStaleLocalShm(path));
    EXPECT_EQ(access(path, F_OK), -1);
    EXPECT_TRUE(segment.CleanupStaleLocalShm(path));
    EXPECT_EQ(segment.OpenAndLockLocalShm(path, acquired), BM_OK);
    ASSERT_GE(acquired, 0);
    EXPECT_FALSE(segment.CleanupStaleLocalShm(path));
    close(acquired);
    EXPECT_TRUE(segment.CleanupStaleLocalShm(path));
}
