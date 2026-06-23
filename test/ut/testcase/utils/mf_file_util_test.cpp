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

#include "mf_file_util.h"

using namespace ock::mf;

class MFFileUtilTest : public testing::Test {
public:
    static void SetUpTestCase() {}

    static void TearDownTestCase()
    {
        GlobalMockObject::reset();
    }

    void SetUp() override {}

    void TearDown() override {}
};

TEST_F(MFFileUtilTest, GetFileSize_1)
{
    std::string path1 = "/etc/group";
    size_t size1 = FileUtil::GetFileSize(path1);
    EXPECT_TRUE(size1 >= 0);

    std::string path2 = "/etc/group111222";
    size_t size2 = FileUtil::GetFileSize(path2);
    EXPECT_EQ(size2, 0);

    MOCKER(fseek).stubs().will(returnValue(-1));
    size_t size3 = FileUtil::GetFileSize(path1);
    EXPECT_TRUE(size3 == 0);

    MOCKER(fopen).stubs().will(returnValue(static_cast<FILE *>(nullptr)));
    size_t size4 = FileUtil::GetFileSize(path1);
    EXPECT_TRUE(size4 == 0);
}

TEST_F(MFFileUtilTest, IsFile_1)
{
    std::string path1 = "/etc/group";
    EXPECT_TRUE(ock::mf::FileUtil::IsFile(path1));

    std::string path2 = "/etc/group111222";
    EXPECT_FALSE(ock::mf::FileUtil::IsFile(path2));
}

TEST_F(MFFileUtilTest, IsDir_1)
{
    std::string path1 = "/etc/";
    EXPECT_TRUE(ock::mf::FileUtil::IsDir(path1));

    std::string path2 = "/etc/group111222";
    EXPECT_FALSE(ock::mf::FileUtil::IsDir(path2));
}

TEST_F(MFFileUtilTest, CheckFileSize_1)
{
    std::string path1 = "/etc/group";
    const uint32_t max_size = 10 * 1024 * 1024;
    EXPECT_TRUE(ock::mf::FileUtil::CheckFileSize(path1, max_size));

    std::string path2 = "/etc/group111222";
    EXPECT_FALSE(ock::mf::FileUtil::CheckFileSize(path2, max_size));
}

TEST_F(MFFileUtilTest, Exist_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::Exist("/nonexistent_path_12345"));
}

TEST_F(MFFileUtilTest, Exist_ExistingPath_ReturnsTrue)
{
    EXPECT_TRUE(FileUtil::Exist("/etc"));
}

TEST_F(MFFileUtilTest, Readable_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::Readable("/nonexistent_path_12345"));
}

TEST_F(MFFileUtilTest, Writable_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::Writable("/nonexistent_path_12345"));
}

TEST_F(MFFileUtilTest, MakeDir_NewDir_CreatesAndRemoves)
{
    std::string tmpDir = "/tmp/mf_ut_dir_" + std::to_string(getpid());
    EXPECT_TRUE(FileUtil::MakeDir(tmpDir, 0755)); // 0755
    EXPECT_TRUE(FileUtil::Exist(tmpDir));
    EXPECT_TRUE(FileUtil::IsDir(tmpDir));
    EXPECT_TRUE(FileUtil::Remove(tmpDir));
    EXPECT_FALSE(FileUtil::Exist(tmpDir));
}

TEST_F(MFFileUtilTest, IsSymlink_RegularFile_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::IsSymlink("/etc/group"));
}

TEST_F(MFFileUtilTest, RealPath_NormalizePath)
{
    std::string path = "/etc/../etc/group";
    EXPECT_TRUE(FileUtil::Realpath(path));
    EXPECT_FALSE(path.empty());
}

TEST_F(MFFileUtilTest, CheckFileIsREG_NonExistent_ReturnsFalse)
{
    std::string path = "/nonexistent";
    EXPECT_FALSE(FileUtil::CheckFileIsREG(path));
}

TEST_F(MFFileUtilTest, MakeDirRecursive_CreatesNestedDir)
{
    std::string tmpBase = "/tmp/mf_ut_recursive_" + std::to_string(getpid());
    std::string nestedDir = tmpBase + "/a/b/c";
    EXPECT_TRUE(FileUtil::MakeDirRecursive(nestedDir, 0755)); // 0755
    EXPECT_TRUE(FileUtil::Exist(nestedDir));
    EXPECT_TRUE(FileUtil::IsDir(nestedDir));
    // Cleanup
    EXPECT_TRUE(FileUtil::RemoveDirRecursive(tmpBase));
    EXPECT_FALSE(FileUtil::Exist(tmpBase));
}

TEST_F(MFFileUtilTest, MakeDirRecursive_ExistingDir_ReturnsTrue)
{
    std::string dir = "/tmp";
    EXPECT_TRUE(FileUtil::MakeDirRecursive(dir, 0755)); // 0755
}

TEST_F(MFFileUtilTest, MakeDirRecursive_EmptyPath_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::MakeDirRecursive("", 0755)); // 0755
}

TEST_F(MFFileUtilTest, RemoveDirRecursive_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::RemoveDirRecursive("/nonexistent_path_xyz_12345"));
}

TEST_F(MFFileUtilTest, RemoveDirRecursive_EmptyPath_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::RemoveDirRecursive(""));
}

TEST_F(MFFileUtilTest, IsEmptyFile_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::IsEmptyFile("/nonexistent_path_xyz_12345"));
}

TEST_F(MFFileUtilTest, GetSafePathMax_ReturnsPositive)
{
    EXPECT_GT(FileUtil::GetSafePathMax(), 0U);
}

TEST_F(MFFileUtilTest, ReadAndWritable_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::ReadAndWritable("/nonexistent_path_xyz_12345"));
}

TEST_F(MFFileUtilTest, ReadAndWritable_ExistingDir)
{
    // /tmp should be readable and writable
    EXPECT_TRUE(FileUtil::ReadAndWritable("/tmp"));
}

TEST_F(MFFileUtilTest, CloseFile_NullPtr_NoCrash)
{
    FileUtil::CloseFile(nullptr);
}

TEST_F(MFFileUtilTest, Remove_EmptyPath_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::Remove(""));
}

TEST_F(MFFileUtilTest, Remove_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::Remove("/nonexistent_path_xyz_12345"));
}

TEST_F(MFFileUtilTest, RealPath_EmptyPath_ReturnsFalse)
{
    std::string emptyPath;
    EXPECT_FALSE(FileUtil::Realpath(emptyPath));
}

TEST_F(MFFileUtilTest, RealPath_NonExistent_ReturnsFalse)
{
    std::string path = "/nonexistent_path_xyz_12345";
    EXPECT_FALSE(FileUtil::Realpath(path));
}

TEST_F(MFFileUtilTest, LibraryRealPath_NonExistent_ReturnsFalse)
{
    std::string realPath;
    EXPECT_FALSE(FileUtil::LibraryRealPath("/nonexistent_dir", "libnonexistent.so", realPath));
}

TEST_F(MFFileUtilTest, CheckFileSize_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::CheckFileSize("/nonexistent_path_xyz_12345", 1024)); // 1024
}

TEST_F(MFFileUtilTest, IsSymlink_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::IsSymlink("/nonexistent_path_xyz_12345"));
}

TEST_F(MFFileUtilTest, IsFile_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::IsFile("/nonexistent_path_xyz_12345"));
}

TEST_F(MFFileUtilTest, IsDir_NonExistent_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::IsDir("/nonexistent_path_xyz_12345"));
}

TEST_F(MFFileUtilTest, MakeDir_EmptyPath_ReturnsFalse)
{
    EXPECT_FALSE(FileUtil::MakeDir("", 0755)); // 0755
}

TEST_F(MFFileUtilTest, MakeDir_Existing_ReturnsTrue)
{
    EXPECT_TRUE(FileUtil::MakeDir("/tmp", 0755)); // 0755
}