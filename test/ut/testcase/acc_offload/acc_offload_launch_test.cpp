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

#include <dlfcn.h>
#include <libgen.h>
#include <unistd.h>

#include <limits>
#include <string>

#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

// 先包含完整标准库链（acc_offload_launch.h -> logger -> sstream 等），
// 再用 private->public hack 暴露私有静态方法，避免宏污染标准头。
#include "acc_offload_define.h"
#include "acc_offload_logger.h"
#include "mf_file_util.h"

#define private public
#include "acc_offload_launch.h"
#undef private

using namespace ock::offload;

namespace {

constexpr int32_t DLADDR_FAILED = 0;
constexpr size_t MAX_PATH_BUF = 4096;

struct DlInfoSetter {
    static int SetAndReturn(Dl_info *info, const char *fname, int ret)
    {
        info->dli_fname = fname;
        info->dli_fbase = nullptr;
        info->dli_sname = nullptr;
        info->dli_saddr = nullptr;
        return ret;
    }
};

// mockcpp 的 invoke 只接受函数指针，mock dladdr 时用全局变量向 stub 传参。
const char *gMockDliFname = nullptr;
std::string gMockDirForLib;
int gMockDladdrRet = 1;

int MockDladdr(const void *, Dl_info *info)
{
    return DlInfoSetter::SetAndReturn(info, gMockDliFname, gMockDladdrRet);
}

int MockDladdrWithDir(const void *, Dl_info *info)
{
    return DlInfoSetter::SetAndReturn(info, gMockDirForLib.c_str(), gMockDladdrRet);
}

// glibc 将 dladdr 声明为 noexcept，mockcpp 的 API hook 模板不支持 noexcept 函数类型，
// 这里抹掉 noexcept 重新注册（函数地址不变，hook 机制不受影响）。
using DladdrApiFn = int (*)(const void *, Dl_info *);

#define MOCK_DLADDR() MOCKCPP_NS::mockAPI("dladdr", reinterpret_cast<DladdrApiFn>(&dladdr))

std::string DirNameOf(const std::string &path)
{
    std::string buf = path;
    char *dup = strdup(buf.c_str());
    if (dup == nullptr) {
        return "";
    }
    char *dir = dirname(dup);
    std::string result = dir;
    free(dup);
    return result;
}

// dladdr 返回的是加载时使用的路径（可能是相对路径），断言前统一归一化。
std::string RealPathOf(const std::string &path)
{
    char resolved[MAX_PATH_BUF] = {0};
    if (realpath(path.c_str(), resolved) == nullptr) {
        return path;
    }
    return std::string(resolved);
}

} // namespace

class AccOffloadLaunchTest : public testing::Test {
public:
    static void SetUpTestCase() {}

    static void TearDownTestCase()
    {
        GlobalMockObject::reset();
    }

    void SetUp() override {}

    void TearDown() override
    {
        GlobalMockObject::reset();
    }
};

// GetSelfLibDir 在 UT 可执行文件内解析：dladdr 定位到测试二进制自身，
// 返回目录应与测试二进制所在目录一致（生产同款 dladdr + strrchr 链路）。
TEST_F(AccOffloadLaunchTest, GetSelfLibDir_FromMainBinary_ReturnsExeDir)
{
    std::string exePath(MAX_PATH_BUF, '\0');
    ssize_t len = readlink("/proc/self/exe", exePath.data(), exePath.size() - 1);
    ASSERT_GT(len, 0);

    exePath.resize(static_cast<size_t>(len));
    std::string exeDir = RealPathOf(DirNameOf(exePath));

    std::string libDir = AccOffloadLaunchApi::GetSelfLibDir();
    EXPECT_FALSE(libDir.empty());
    EXPECT_EQ(RealPathOf(libDir), exeDir);
}

// GetSelfLibDir 在共享库内解析（生产场景）：代码链入 helper so 后 dlopen 加载，
// dladdr 应解析出 helper so 的真实路径，目录与其一致。
TEST_F(AccOffloadLaunchTest, GetSelfLibDir_FromSharedLib_ReturnsSoDir)
{
    const char *helperSo = ACC_OFFLOAD_UT_HELPER_SO;
    void *handle = dlopen(helperSo, RTLD_NOW);
    ASSERT_NE(handle, nullptr) << "dlopen " << helperSo << " failed: " << dlerror();

    using GetSelfLibDirFn = int (*)(char *, unsigned int);
    auto getSelfLibDir = reinterpret_cast<GetSelfLibDirFn>(dlsym(handle, "AccOffloadLaunchUtGetSelfLibDir"));
    ASSERT_NE(getSelfLibDir, nullptr) << "dlsym failed: " << dlerror();

    Dl_info info{};
    ASSERT_NE(dladdr(reinterpret_cast<void *>(getSelfLibDir), &info), 0);
    ASSERT_NE(info.dli_fname, nullptr);

    std::string soDir = RealPathOf(DirNameOf(info.dli_fname));

    char dirBuf[MAX_PATH_BUF] = {0};
    ASSERT_EQ(getSelfLibDir(dirBuf, sizeof(dirBuf)), 0);
    EXPECT_EQ(RealPathOf(dirBuf), soDir);

    dlclose(handle);
}

TEST_F(AccOffloadLaunchTest, GetSelfLibDir_DladdrFailed_ReturnsEmpty)
{
    MOCK_DLADDR().stubs().will(returnValue(DLADDR_FAILED));
    EXPECT_TRUE(AccOffloadLaunchApi::GetSelfLibDir().empty());
}

TEST_F(AccOffloadLaunchTest, GetSelfLibDir_FnameNullptr_ReturnsEmpty)
{
    gMockDliFname = nullptr;
    gMockDladdrRet = 1;
    MOCK_DLADDR().stubs().will(invoke(MockDladdr));
    EXPECT_TRUE(AccOffloadLaunchApi::GetSelfLibDir().empty());
}

TEST_F(AccOffloadLaunchTest, GetSelfLibDir_FnameEmpty_ReturnsEmpty)
{
    gMockDliFname = "";
    gMockDladdrRet = 1;
    MOCK_DLADDR().stubs().will(invoke(MockDladdr));
    EXPECT_TRUE(AccOffloadLaunchApi::GetSelfLibDir().empty());
}

TEST_F(AccOffloadLaunchTest, GetSelfLibDir_NoSlash_ReturnsEmpty)
{
    gMockDliFname = "libmf_hybm_accoffload.so";
    gMockDladdrRet = 1;
    MOCK_DLADDR().stubs().will(invoke(MockDladdr));
    EXPECT_TRUE(AccOffloadLaunchApi::GetSelfLibDir().empty());
}

// 根路径场景："/lib.so" 的父目录是 "/" 本身，应返回 "/" 而非空串。
TEST_F(AccOffloadLaunchTest, GetSelfLibDir_RootPath_ReturnsRoot)
{
    gMockDliFname = "/lib.so";
    gMockDladdrRet = 1;
    MOCK_DLADDR().stubs().will(invoke(MockDladdr));
    std::string libDir = AccOffloadLaunchApi::GetSelfLibDir();
    EXPECT_EQ(libDir, "/");
}

// TryLoadLibrary：dladdr 失败导致无法定位库目录，返回 OFFLOAD_ERROR。
TEST_F(AccOffloadLaunchTest, TryLoadLibrary_DladdrFailed_ReturnsError)
{
    AccOffloadLaunchApi::gLoaded = false;
    MOCK_DLADDR().stubs().will(returnValue(DLADDR_FAILED));

    EXPECT_EQ(AccOffloadLaunchApi::TryLoadLibrary(), OFFLOAD_ERROR);
    EXPECT_FALSE(AccOffloadLaunchApi::gLoaded);
}

// TryLoadLibrary：定位到的目录中不存在目标 so，LibraryRealPath 失败，返回 OFFLOAD_ERROR。
TEST_F(AccOffloadLaunchTest, TryLoadLibrary_LibNotInDir_ReturnsError)
{
    AccOffloadLaunchApi::gLoaded = false;

    std::string exePath(MAX_PATH_BUF, '\0');
    ssize_t len = readlink("/proc/self/exe", exePath.data(), exePath.size() - 1);
    ASSERT_GT(len, 0);
    exePath.resize(static_cast<size_t>(len));
    std::string exeDir = DirNameOf(exePath);
    gMockDirForLib = exeDir;
    gMockDladdrRet = 1;

    MOCK_DLADDR().stubs().will(invoke(MockDladdrWithDir));

    // exeDir 目录下不会有 libmf_hybm_accoffload.so，LibraryRealPath 返回 false
    EXPECT_EQ(AccOffloadLaunchApi::TryLoadLibrary(), OFFLOAD_ERROR);
    EXPECT_FALSE(AccOffloadLaunchApi::gLoaded);
}

// 未加载成功时，算子入口应直接返回 OFFLOAD_UNLOAD，不崩溃。
TEST_F(AccOffloadLaunchTest, SparseCopy_NotLoaded_ReturnsUnload)
{
    AccOffloadLaunchApi::gLoaded = false;
    AccOffloadLaunchApi::pAccOffloadSparseCopy = nullptr;

    uint64_t *src = nullptr;
    uint32_t len = 0;
    EXPECT_EQ(AccOffloadLaunchApi::AccOffloadSparseCopy(src, src, &len, &len, 0), OFFLOAD_UNLOAD);
}

TEST_F(AccOffloadLaunchTest, GroupPackCopy_NotLoaded_ReturnsUnload)
{
    AccOffloadLaunchApi::gLoaded = false;
    AccOffloadLaunchApi::pAccOffloadGroupPackCopy = nullptr;

    uint64_t *src = nullptr;
    uint32_t len = 0;
    int64_t groupList = 0;
    EXPECT_EQ(AccOffloadLaunchApi::AccOffloadGroupPackCopy(src, src, &len, &len, &groupList, &groupList, 0),
              OFFLOAD_UNLOAD);
}
