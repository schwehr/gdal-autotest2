// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for tar virtual file system (VSI) files.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_tar.cpp

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/log/check.h"
#include "third_party/absl/log/die_if_null.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr int kSuccess = 0;

// TODO(schwehr): Move CplVsiLFileCloser to utils.
class CplVsiLFileCloser {
 public:
  CplVsiLFileCloser(VSILFILE* file) : file_(ABSL_DIE_IF_NULL(file)) {}
  ~CplVsiLFileCloser() { CHECK_EQ(kSuccess, VSIFCloseL(file_)); }

 private:
  VSILFILE* file_;
};

const std::filesystem::path kTestDataPath(
    "google3/third_party/gdal/autotest2/cpp/port/testdata/tar");

std::filesystem::path GetTestFilePath(const std::string_view filename) {
  return std::filesystem::path(::testing::SrcDir()) / kTestDataPath / filename;
}

TEST(CplVsiTarTest, ReadEmpty) {
  const auto filepath = GetTestFilePath("empty.tar");
  const std::string vsi_path = "/vsitar/" + std::string(filepath);

  EXPECT_EQ(kSuccess, access(filepath.c_str(), F_OK));
  char** string_list = VSIReadDir(vsi_path.c_str());
  auto entries = CslToVector(string_list);
  CSLDestroy(string_list);
  EXPECT_TRUE(entries.empty());
}

TEST(CplVsiTarTest, ReadVariety) {
  const auto filepath = GetTestFilePath("variety.tar");
  const std::string vsi_path = "/vsitar/" + std::string(filepath);

  EXPECT_EQ(kSuccess, access(filepath.c_str(), F_OK));
  char** string_list = VSIReadDir(vsi_path.c_str());
  auto entries = CslToVector(string_list);
  CSLDestroy(string_list);
  ASSERT_EQ(21, entries.size());
  EXPECT_EQ("1", entries[0]);
  EXPECT_EQ("10k-ff.gz", entries[1]);
  EXPECT_EQ("10k-ff.tar", entries[2]);
  EXPECT_EQ("10k-ff.tar.gz", entries[3]);
  EXPECT_EQ("all-bits", entries[4]);
  EXPECT_EQ("all-bits-dir", entries[5]);
  EXPECT_EQ("all-perm", entries[6]);
  // EXPECT_EQ("anyfile", entries[7]);
  EXPECT_EQ("baz.zip", entries[7]);
  EXPECT_EQ("bin", entries[8]);
  EXPECT_EQ("CAPS", entries[9]);
  EXPECT_EQ("empty.zip", entries[10]);
  EXPECT_EQ("foo.gz", entries[11]);
  EXPECT_EQ("group-sticky", entries[12]);
  EXPECT_EQ("owner-read", entries[13]);
  // Looses all the puctuation.  Should be: Punc!@#$%^&*()-=_+[]{};':",<>
  EXPECT_EQ("Punc", entries[14]);
  EXPECT_EQ("qux.zip", entries[15]);
  EXPECT_EQ("sticky", entries[16]);
  EXPECT_EQ("sticky-dir", entries[17]);
  EXPECT_EQ("sticky-permission", entries[18]);
  EXPECT_EQ("z", entries[19]);
  EXPECT_EQ(".dotfile", entries[20]);

  const std::string long_path =
      vsi_path + "/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20";
  string_list = VSIReadDir(long_path.c_str());
  entries = CslToVector(string_list);
  CSLDestroy(string_list);
  ASSERT_EQ(1, entries.size());
  EXPECT_EQ("ok", entries[0]);
}

TEST(CplVsiTarTest, ReadVarietyFileContents) {
  const auto filepath = GetTestFilePath("variety.tar");

  const std::string vsi_path = "/vsitar/" + std::string(filepath) + "/z";

  constexpr size_t kBufSize = 10;
  constexpr size_t kFileSize = 4;
  char buf[kBufSize] = {};
  VSILFILE* file = VSIFOpenL(vsi_path.c_str(), "rb");
  ASSERT_NE(nullptr, file);
  CplVsiLFileCloser closer(file);
  EXPECT_EQ(kFileSize, VSIFReadL(buf, 1, kBufSize - 1, file));
  EXPECT_STREQ("abc\n", buf);
}

// TODO(schwehr): Test the stat of the files in variety.tar.

TEST(CplVsiTarTest, ReadTarGz) {
  const auto filepath = GetTestFilePath("bar.tar.gz");
  // No need to tell it about the gzip compression.
  const std::string vsi_path = "/vsitar/" + std::string(filepath);

  EXPECT_EQ(kSuccess, access(filepath.c_str(), F_OK));
  char** string_list = VSIReadDir(vsi_path.c_str());
  auto entries = CslToVector(string_list);
  CSLDestroy(string_list);
  ASSERT_EQ(1, entries.size());
  EXPECT_EQ("bar", entries[0]);

  const std::string bar_path = "/vsitar/" + std::string(filepath / "bar");

  constexpr size_t kBufSize = 10;
  constexpr size_t kFileSize = 4;
  char buf[kBufSize] = {};
  VSILFILE* file = VSIFOpenL(vsi_path.c_str(), "rb");
  ASSERT_NE(nullptr, file);
  CplVsiLFileCloser closer(file);
  EXPECT_EQ(kFileSize, VSIFReadL(buf, 1, kBufSize - 1, file));
  EXPECT_STREQ("foo\n", buf);
}

std::string MakeTarHeader(std::string_view name, std::string_view size_octal,
                          std::string_view mtime_octal, char typeflag,
                          std::string_view prefix = "") {
  std::string hdr(512, '\0');
  std::memcpy(&hdr[0], name.data(), std::min<size_t>(name.size(), 100));
  std::memcpy(&hdr[100], "0000644", 7);
  std::memcpy(&hdr[108], "0001000", 7);
  std::memcpy(&hdr[116], "0001000", 7);
  std::memcpy(&hdr[124], size_octal.data(),
              std::min<size_t>(size_octal.size(), 11));
  std::memcpy(&hdr[136], mtime_octal.data(),
              std::min<size_t>(mtime_octal.size(), 11));
  hdr[156] = typeflag;
  if (!prefix.empty()) {
    std::memcpy(&hdr[257], "ustar\0", 6);
    std::memcpy(&hdr[345], prefix.data(), std::min<size_t>(prefix.size(), 155));
  }
  return hdr;
}

TEST(CplVsiTarTest, LongLinkAndUstarPrefix) {
  // 1) GNU LongLink ('L') entry followed by actual file entry.
  const std::string long_name = "dir_long/subdir/long_filename_entry.txt";
  const std::string long_name_padded =
      long_name + std::string(512 - long_name.size(), '\0');
  char size_buf[12] = {};
  snprintf(size_buf, sizeof(size_buf), "%011o",
           static_cast<unsigned int>(long_name.size() + 1));
  std::string tar_data =
      MakeTarHeader("././@LongLink", size_buf, "00000000000", 'L') +
      long_name_padded;

  const std::string content1 = "hello";
  std::string content1_padded =
      content1 + std::string(512 - content1.size(), '\0');
  tar_data += MakeTarHeader("ignored", "00000000005", "00000000001", '0') +
              content1_padded;

  // 2) UStar prefix entry.
  tar_data += MakeTarHeader("leaf.txt", "00000000005", "00000000001", '0',
                            "ustar_prefix") +
              content1_padded;

  // End of archive (two 512-byte zero blocks).
  tar_data += std::string(1024, '\0');

  const std::string mem_tar = "/vsimem/custom_long_ustar.tar";
  VsiMemTempWrapper wrapper(mem_tar, tar_data);

  // Open file via LongLink name.
  const std::string path1 = "/vsitar/" + mem_tar + "/" + long_name;
  VSILFILE* fp1 = VSIFOpenL(path1.c_str(), "rb");
  ASSERT_NE(nullptr, fp1);
  char buf[16] = {};
  EXPECT_EQ(5, VSIFReadL(buf, 1, 5, fp1));
  EXPECT_STREQ("hello", buf);
  EXPECT_EQ(0, VSIFCloseL(fp1));

  // Open file via UStar prefix name.
  const std::string path2 = "/vsitar/" + mem_tar + "/ustar_prefix/leaf.txt";
  VSILFILE* fp2 = VSIFOpenL(path2.c_str(), "rb");
  ASSERT_NE(nullptr, fp2);
  std::memset(buf, 0, sizeof(buf));
  EXPECT_EQ(5, VSIFReadL(buf, 1, 5, fp2));
  EXPECT_STREQ("hello", buf);
  EXPECT_EQ(0, VSIFCloseL(fp2));
}

TEST(CplVsiTarTest, ErrorConditions) {
  WithQuietHandler quiet;
  const auto filepath = GetTestFilePath("variety.tar");
  const std::string vsi_path = "/vsitar/" + std::string(filepath) + "/z";

  // Write mode is rejected.
  EXPECT_EQ(nullptr, VSIFOpenL(vsi_path.c_str(), "wb"));
  EXPECT_EQ(nullptr, VSIFOpenL(vsi_path.c_str(), "rb+"));

  // Non-existent archive and non-existent entry inside valid archive.
  EXPECT_EQ(nullptr, VSIFOpenL("/vsitar/nonexistent_dir/no.tar/file", "rb"));
  const std::string missing_in_tar =
      "/vsitar/" + std::string(filepath) + "/does_not_exist.txt";
  EXPECT_EQ(nullptr, VSIFOpenL(missing_in_tar.c_str(), "rb"));

  // Truncated header (< 512 bytes).
  {
    const std::string mem_tar = "/vsimem/truncated_hdr.tar";
    VsiMemTempWrapper wrapper(mem_tar, "short");
    EXPECT_EQ(nullptr, VSIReadDir(("/vsitar/" + mem_tar).c_str()));
  }

  // Invalid numeric field terminator.
  {
    std::string bad_hdr(512, 'X');
    const std::string mem_tar = "/vsimem/bad_term.tar";
    VsiMemTempWrapper wrapper(mem_tar, bad_hdr);
    EXPECT_EQ(nullptr, VSIReadDir(("/vsitar/" + mem_tar).c_str()));
  }

  // Invalid octal digit in file size ('0' followed by '9').
  {
    std::string bad_size =
        MakeTarHeader("file.txt", "09000000000", "00000000000", '0');
    const std::string mem_tar = "/vsimem/bad_size.tar";
    VsiMemTempWrapper wrapper(mem_tar, bad_size);
    EXPECT_EQ(nullptr, VSIReadDir(("/vsitar/" + mem_tar).c_str()));
  }

  // Invalid octal digit in mtime ('0' followed by '9').
  {
    std::string bad_mtime =
        MakeTarHeader("file.txt", "00000000000", "09000000000", '0');
    const std::string mem_tar = "/vsimem/bad_mtime.tar";
    VsiMemTempWrapper wrapper(mem_tar, bad_mtime);
    EXPECT_EQ(nullptr, VSIReadDir(("/vsitar/" + mem_tar).c_str()));
  }

  // Truncated LongLink payload.
  {
    std::string trunc_long =
        MakeTarHeader("././@LongLink", "00000000010", "00000000000", 'L') +
        "short";
    const std::string mem_tar = "/vsimem/trunc_long.tar";
    VsiMemTempWrapper wrapper(mem_tar, trunc_long);
    EXPECT_EQ(nullptr, VSIReadDir(("/vsitar/" + mem_tar).c_str()));
  }
}

}  // namespace
}  // namespace autotest2
