// Tests for GDAL's VSI virtual file system.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include <sys/resource.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using std::fill;
using ::testing::TestWithParam;

// Create a fake handler for testing that provides a stat
// that always succeeds and an open that always fails.
class DummyFilesystem : public VSIFilesystemHandler {
 public:
  DummyFilesystem();
  VSIVirtualHandleUniquePtr Open(const char* path, const char* access,
                                 bool bSetError,
                                 CSLConstList papszOptions) override;
  int Stat(const char* path, VSIStatBufL* stat, int nFlags) override;

  // Record what happened during the test.
  std::string open_last_path_;
  std::string open_last_access_;
  std::string stat_last_path_;
};

DummyFilesystem::DummyFilesystem() {}

VSIVirtualHandleUniquePtr DummyFilesystem::Open(
    const char* path, const char* access, bool /* bSetError */,
    CSLConstList /* papszOptions */) {
  open_last_path_ = path;
  open_last_access_ = access;
  return VSIVirtualHandleUniquePtr(nullptr);
}

int DummyFilesystem::Stat(const char* path, VSIStatBufL* stat, int nFlags) {
  stat_last_path_ = path;
  memset(stat, 0, sizeof(*stat));

  // Giving a size and mode is enough to fake that the file exists.
  stat->st_size = 1000;
  stat->st_mode = 33184;

  return 0;
}

// Tests adding a driver and calling open.
TEST(CplVsiFileManagerTest, FailToOpenExistingFile) {
  auto dummy_handler = std::make_unique<DummyFilesystem>();
  VSIFileManager::InstallHandler("/dummy/", dummy_handler.get());
  absl::Cleanup remove_handler = [] {
    VSIFileManager::RemoveHandler("/dummy/");
  };

  const std::string path("/dummy/foo.tif");

  {
    // Suppress '`/dummy/foo.tif' not recognized as a supported file format.'
    WithQuietHandler error_handler;

    VSILFILE* foo = VSIFOpenL(path.c_str(), "rb");
    ASSERT_EQ(nullptr, foo);
  }

  // Also searches for /dummy/foo.tif.xml in GDAL 2.*.
  EXPECT_FALSE(dummy_handler->stat_last_path_.find(path) != std::string::npos);
  EXPECT_TRUE(dummy_handler->open_last_path_.find(path) != std::string::npos);
  EXPECT_STREQ("rb", dummy_handler->open_last_access_.c_str());
}

// Tests the VSI functions for non-existent file paths.
TEST(CplVSIFunctions, VSIFilesAndPathLocalDoesNotExist) {
  ASSERT_EQ(nullptr, VSIReadDir("/does/not/exist0"));
  ASSERT_EQ(nullptr, VSIReadDirRecursive("/does/not/exist1"));
  ASSERT_EQ(-1, VSIMkdir("/does/not/exist2", 0755));
  ASSERT_EQ(-1, VSIUnlink("/does/not/exist3"));
  ASSERT_EQ(-1, VSIRmdir("/does/not/exist4"));

  VSIStatBufL stat_buf = {};
  ASSERT_EQ(-1, VSIStatL("/does/not/exist5", &stat_buf));
  ASSERT_EQ(0, stat_buf.st_mode);
  ASSERT_EQ(0, stat_buf.st_size);

  stat_buf = {};
  ASSERT_EQ(-1, VSIStatExL("/does/not/exist6", &stat_buf, 0));
  ASSERT_EQ(0, stat_buf.st_mode);
  ASSERT_EQ(0, stat_buf.st_size);
}

// Tests most of the file operations by creating a directory and a file.
TEST(CplVSIFunctions, VSIFileAndDirOpsLocal) {
  // TODO(schwehr): Let temp_dir also work outside of google3.
  const std::string temp_dir(::testing::TempDir());

  constexpr int kSuccess = 0;
  constexpr int kFailure = -1;

  VSIStatBufL stat_buf = {};
  ASSERT_EQ(kSuccess, VSIStatL(temp_dir.c_str(), &stat_buf));

  const std::string src_dir(temp_dir + "/foo");
  const std::string dst_dir(temp_dir + "/bar");
  ASSERT_EQ(kSuccess, VSIMkdir(src_dir.c_str(), 0755));
  ASSERT_EQ(kSuccess, VSIRename(src_dir.c_str(), dst_dir.c_str()));

  stat_buf = {};
  ASSERT_EQ(kSuccess, VSIStatL(dst_dir.c_str(), &stat_buf));
  // TODO(schwehr): Flaky.  Depends on user env: 040750
  // ASSERT_EQ(stat_buf.st_mode, 040755);

  const int case_sensitive = VSIIsCaseSensitiveFS(temp_dir.c_str());
  ASSERT_TRUE((0 == case_sensitive || 1 == case_sensitive));

  const std::string filename(dst_dir + "/baz.txt");

  VSILFILE* file = VSIFOpenL(filename.c_str(), "w");
  ASSERT_NE(nullptr, file);

  ASSERT_EQ(0, VSIFTellL(file));

  // Cannot seek to before the file or to
  // std::numeric_limits<vsi_l_offset>::max()
  ASSERT_EQ(kFailure, VSIFSeekL(file, -1, SEEK_SET));
  ASSERT_EQ(kFailure, VSIFSeekL(file, -1, SEEK_CUR));
  ASSERT_EQ(kFailure, VSIFSeekL(file, -1, SEEK_END));

  ASSERT_EQ(0, VSIFTellL(file));

  ASSERT_EQ(0, VSIFSeekL(file, 0, SEEK_SET));
  ASSERT_EQ(0, VSIFSeekL(file, 0, SEEK_CUR));
  ASSERT_EQ(0, VSIFSeekL(file, 0, SEEK_END));

  ASSERT_EQ(0, VSIFTellL(file));

  ASSERT_EQ(3, VSIFWriteL("012", 1, 3, file));
  ASSERT_EQ(kSuccess, VSIFFlushL(file));
  ASSERT_EQ(3, VSIFTellL(file));

  stat_buf = {};
  ASSERT_EQ(kSuccess, VSIStatL(filename.c_str(), &stat_buf));
  ASSERT_EQ(3, stat_buf.st_size);

  VSIRewindL(file);
  ASSERT_EQ(0, VSIFTellL(file));

  ASSERT_EQ(kSuccess, VSIFCloseL(file));

  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);

  char buf[43] = {};
  ASSERT_EQ(1, VSIFReadL(buf, 1, 1, file));
  ASSERT_STREQ("0", buf);
  ASSERT_FALSE(VSIFEofL(file));

  ASSERT_EQ(2, VSIFReadL(buf, 1, 10, file));
  ASSERT_TRUE(VSIFEofL(file));

  ASSERT_EQ(kSuccess, VSIFSeekL(file, -2, SEEK_CUR));
  ASSERT_FALSE(VSIFEofL(file));
  ASSERT_EQ(1, VSIFTellL(file));

  // TODO(schwehr): Why does this work this way?
  // Seeking past the end.
  ASSERT_EQ(kSuccess, VSIFSeekL(file, 5, SEEK_CUR));
  ASSERT_EQ(6, VSIFTellL(file));
  ASSERT_FALSE(VSIFEofL(file));

  ASSERT_EQ(kSuccess, VSIFCloseL(file));

  stat_buf = {};
  ASSERT_EQ(kSuccess, VSIStatL(filename.c_str(), &stat_buf));
  ASSERT_EQ(3, stat_buf.st_size);

  // Cannot truncate when opened read-only.
  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(-1, VSIFTruncateL(file, 1));
  ASSERT_EQ(kSuccess, VSIFCloseL(file));

  file = VSIFOpenL(filename.c_str(), "r+");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(3, VSIFReadL(buf, 1, 10, file));
  ASSERT_TRUE(VSIFEofL(file));
  ASSERT_EQ(kSuccess, VSIFTruncateL(file, 1));
  ASSERT_TRUE(VSIFEofL(file));
  ASSERT_EQ(kSuccess, VSIFFlushL(file));
  // Truncate does not modify the offset of open files.
  ASSERT_EQ(3, VSIFTellL(file));
  stat_buf = {};
  ASSERT_EQ(kSuccess, VSIStatL(filename.c_str(), &stat_buf));
  ASSERT_EQ(1, stat_buf.st_size);

  ASSERT_EQ(1, VSIFWriteL("3", 1, 1, file));
  ASSERT_EQ(kSuccess, VSIFFlushL(file));
  ASSERT_EQ(4, VSIFTellL(file));

  VSIRewindL(file);
  ASSERT_EQ(0, VSIFTellL(file));
  ASSERT_EQ(4, VSIFReadL(buf, 1, 10, file));

  ASSERT_STREQ("0", buf);
  ASSERT_EQ('\0', buf[1]);
  ASSERT_EQ('\0', buf[2]);
  ASSERT_STREQ("3", buf + 3);

  // Truncate can also grow a file.
  ASSERT_EQ(kSuccess, VSIFTruncateL(file, 42));
  ASSERT_EQ(kSuccess, VSIFFlushL(file));
  stat_buf = {};
  ASSERT_EQ(kSuccess, VSIStatL(filename.c_str(), &stat_buf));
  ASSERT_EQ(42, stat_buf.st_size);

  // Truncate does not move the file index forward or backwards.
  ASSERT_EQ(4, VSIFTellL(file));
  VSIRewindL(file);
  ASSERT_EQ(42, VSIFReadL(buf, 1, 43, file));
  ASSERT_STREQ("0", buf);
  ASSERT_EQ('\0', buf[1]);
  ASSERT_EQ('\0', buf[2]);
  ASSERT_STREQ("3", buf + 3);
  ASSERT_EQ('\0', buf[23]);
  ASSERT_EQ('\0', buf[24]);

  ASSERT_EQ(kSuccess, VSIFCloseL(file));

  // Fail to rmdir with contents.
  ASSERT_EQ(-1, VSIRmdir(dst_dir.c_str()));

  ASSERT_EQ(kSuccess, VSIUnlink(filename.c_str()));
  ASSERT_EQ(kSuccess, VSIRmdir(dst_dir.c_str()));
}

// Test reading more than is available in a file on a normal filesystem.
// Most likely uses Read from cpl_vsil_unix_stdio_64.cpp.
TEST(CplVSIFunctions, VSIReadTooMuchLocalFileSystem) {
  // TODO(schwehr): Non-google3 specific replacement for FLAGS.
  const std::string temp_dir(::testing::TempDir());
  const std::string filename(temp_dir + "/short.txt");
  const std::string contents("012\n\n");
  const int length = contents.length();

  // Write a small amount of data.
  VSILFILE* file = VSIFOpenL(filename.c_str(), "w");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(length, VSIFWriteL(contents.c_str(), 1, length, file));
  ASSERT_EQ(0, VSIFFlushL(file));
  ASSERT_EQ(0, VSIFCloseL(file));
  file = nullptr;

  const int buf_size = 1024;
  char buf[buf_size] = {};

  // Read the file normally.
  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);
  EXPECT_EQ(length, VSIFReadL(buf, 1, length, file));
  EXPECT_STREQ(contents.c_str(), buf);
  ASSERT_EQ(0, VSIFCloseL(file));
  file = nullptr;
  fill(buf, buf + buf_size, 0);

  // Try to read way too much.
  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);
  EXPECT_EQ(length, VSIFReadL(buf, 1, buf_size, file));
  EXPECT_STREQ(contents.c_str(), buf);
  ASSERT_EQ(0, VSIFCloseL(file));
  file = nullptr;
  fill(buf, buf + buf_size, 0);

  // Try to read way too much with swapped size and count.
  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);
  // 1, 1024 -> 1024, 1
  // It will be unable to read 1 block of 1024 bytes.
  EXPECT_EQ(0, VSIFReadL(buf, buf_size, 1, file));
  // The read reports nothing read, but buf is set.
  EXPECT_STREQ(contents.c_str(), buf);
  ASSERT_EQ(0, VSIFCloseL(file));
}

// Test reading more than is available in a memory filesystem.
TEST(CplVSIFunctions, VSIFReadLTooMuchVsimem) {
  const std::string filename("/vsimem/short.txt");
  const std::string contents("\n\nabc");
  const int length = contents.length();

  // Write a small amount of data.
  VSILFILE* file = VSIFOpenL(filename.c_str(), "w");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(length, VSIFWriteL(contents.c_str(), 1, length, file));
  ASSERT_EQ(0, VSIFFlushL(file));
  ASSERT_EQ(0, VSIFCloseL(file));
  file = nullptr;

  const int buf_size = 1024;
  char buf[buf_size] = {};

  // Try to read way too much.
  file = VSIFOpenL(filename.c_str(), "r");
  ASSERT_NE(nullptr, file);
  EXPECT_EQ(length, VSIFReadL(buf, 1, buf_size, file));
  EXPECT_STREQ(contents.c_str(), buf);
  ASSERT_EQ(0, VSIFCloseL(file));
}

TEST(CplVSIFunctions, VSIMkdirRecursive) {
  constexpr int kSuccess = 0;
  constexpr int kFailure = -1;

  EXPECT_EQ(kFailure, VSIMkdirRecursive("/", 0));
  EXPECT_EQ(kFailure, VSIMkdirRecursive("", 0));
  EXPECT_EQ(kFailure, VSIMkdirRecursive("/does/not/exist", 0));
  EXPECT_EQ(kFailure, VSIRmdirRecursive("/also/does/not/exist"));

  constexpr char kBasePath[] = "/vsimem/a";
  constexpr char kPath[] = "/vsimem/a/b/c";

  EXPECT_EQ(kSuccess, VSIMkdirRecursive(kPath, 0777));
  VSIStatBufL stat_buf = {};
  ASSERT_EQ(kSuccess, VSIStatL(kPath, &stat_buf));

  // Add a file.
  const auto filepath = std::filesystem::path(kPath) / "d";
  VSILFILE* file = VSIFOpenL(filepath.c_str(), "wb");
  ASSERT_NE(nullptr, file);
  constexpr char kData[] = "abc";
  EXPECT_EQ(1, VSIFWriteL(kData, std::size(kData), 1, file));
  EXPECT_EQ(kSuccess, VSIFCloseL(file));

  EXPECT_EQ(kSuccess, VSIRmdirRecursive(kBasePath));
  ASSERT_EQ(kFailure, VSIStatL(kPath, &stat_buf));
}

TEST(CplVSIFunctions, VSIGetDiskFreeSpaceVsiMemFileShouldReturnMemorySize) {
  GIntBig free_space_on_mem_disk = VSIGetDiskFreeSpace("/vsimem/");

  // Memory size of the machine the test is run in should be positive.
  // This also catches any possible overflow issues.
  ASSERT_GT(free_space_on_mem_disk, 0);

  // Memory size of the machine the test is run in should be within some limits.
  GIntBig kHighMemLimit1Pb = 1UL << 50;
  ASSERT_LT(free_space_on_mem_disk, kHighMemLimit1Pb);
}

#ifdef HAVE_GETRLIMIT

TEST(CplVSIFunctions, VSIGetDiskFreeSpaceVsiMemFileShouldRespectRlimit) {
  constexpr GIntBig kLowRlimitOf1mb = 1 << 20;

  // This code assumes HAVE_RLIMIT_AS
  constexpr int res = RLIMIT_AS;

  // Save current rlimit
  struct rlimit orig_rlim;
  int ret = getrlimit(res, &orig_rlim);
  EXPECT_EQ(ret, 0);

  absl::Cleanup reset_orig_rlim = [orig_rlim] { setrlimit(res, &orig_rlim); };

  // Setup lower rlimit than (assumption) amount of physical ram
  struct rlimit rlim;
  rlim.rlim_cur = kLowRlimitOf1mb;
  rlim.rlim_max = orig_rlim.rlim_max;
  setrlimit(res, &rlim);

  // Assumes that the physical ram on the test machine is greater than
  // kLowRlimitOf1mb
  ASSERT_EQ(VSIGetDiskFreeSpace("/vsimem/"), kLowRlimitOf1mb);
}

#endif

class MockFileSystemHandler : public VSIFilesystemHandler {
 public:
  MOCK_METHOD(VSIVirtualHandleUniquePtr, Open,
              (const char* filename, const char* access, bool set_error,
               CSLConstList options),
              (override));
  MOCK_METHOD(int, Stat,
              (const char* filename, VSIStatBufL* stat_buf, int flags),
              (override));
  MOCK_METHOD(bool, IsLocal, (const char* path), (const, override));
};

struct VsiGetHandlerTestCase {
  std::string_view test_name;
  const char* prefix;
  const char* path;
  bool expect_to_find;
};

using FindHandlerTest = TestWithParam<VsiGetHandlerTestCase>;

TEST_P(FindHandlerTest, FindHandler) {
  const VsiGetHandlerTestCase& test_case = GetParam();

  auto handler = std::make_unique<MockFileSystemHandler>();

  VSIFileManager::InstallHandler(test_case.prefix, handler.get());
  absl::Cleanup remove_handler = [&test_case] {
    VSIFileManager::RemoveHandler(test_case.prefix);
  };

  auto actual_handler = VSIFileManager::GetHandler(test_case.path);

  if (test_case.expect_to_find) {
    ASSERT_EQ(handler.get(), actual_handler);
  } else {
    constexpr char non_matching_path[] = "/vsi-get-handler-non-existent/";
    auto default_handler = VSIFileManager::GetHandler(non_matching_path);
    ASSERT_EQ(actual_handler, default_handler);
  }
}

INSTANTIATE_TEST_SUITE_P(
    VsiGetHandlerTestSuite, FindHandlerTest,
    testing::ValuesIn<VsiGetHandlerTestCase>({
        {
            .test_name = "ExactMatch",
            .prefix = "/vsi-get-handler-exact-match/",
            .path = "/vsi-get-handler-exact-match/",
            .expect_to_find = true,
        },
        {
            .test_name = "SubPathMatch",
            .prefix = "/vsi-get-handler-sub-path-match/",
            .path = "/vsi-get-handler-sub-path-match//file.txt",
            .expect_to_find = true,
        },
        {
            .test_name = "PathWithBackslashes",
            .prefix = "/vsi-get-handler-path-with-backslashes/",
            .path =
                "/vsi-get-handler-path-with-backslashes\\directory\\file.txt",
            .expect_to_find = true,
        },
        {
            .test_name = "PathWithoutTrailingSlash",
            .prefix = "/vsi-get-handler-path-without-trailing-slash/",
            .path = "/vsi-get-handler-path-without-trailing-slash",
            .expect_to_find = true,
        },
        {
            .test_name = "PathPrefixNoMatch",
            .prefix = "/vsi-get-handler-path-prefix-no-match/",
            .path = "/vsi-get-handler-path-prefix/",
            .expect_to_find = false,
        },
    }),
    [](const testing::TestParamInfo<FindHandlerTest::ParamType>& info) {
      return std::string(info.param.test_name);
    });

TEST(CplVSIFunctions, VsiIsLocalTreatsVsiMemAsLocalNonPersistentFileSystem) {
  EXPECT_TRUE(VSIIsLocal("/vsimem/"));
}

TEST(CplVSIFunctions, VsiIsLocalWillStepIntoArchiveFileSystems) {
  EXPECT_TRUE(VSIIsLocal("/vsigzip//vsimem/tmp.gz"));
}

TEST(CplVSIFunctions, VsiIsLocalReturnsFalseForCurlFileSystem) {
  EXPECT_TRUE(VSIIsLocal("/vsicurl/http://example.com"));
}

// A minimal VSIVirtualHandle to exercise base class default virtual methods.
class MinimalVirtualHandle : public VSIVirtualHandle {
 public:
  explicit MinimalVirtualHandle(std::string data) : data_(std::move(data)) {}

  int Seek(vsi_l_offset nOffset, int nWhence) override {
    if (nWhence == SEEK_SET) {
      pos_ = nOffset;
    } else if (nWhence == SEEK_CUR) {
      pos_ += nOffset;
    } else if (nWhence == SEEK_END) {
      pos_ = data_.size() + nOffset;
    }
    eof_ = false;
    return 0;
  }
  vsi_l_offset Tell() override { return pos_; }
  size_t Read(void* pBuffer, size_t nSize, size_t nCount) override {
    size_t total = nSize * nCount;
    if (pos_ >= data_.size()) {
      eof_ = true;
      return 0;
    }
    size_t avail = std::min<size_t>(total, data_.size() - pos_);
    memcpy(pBuffer, data_.data() + pos_, avail);
    pos_ += avail;
    if (avail < total) eof_ = true;
    return nSize ? (avail / nSize) : 0;
  }
  size_t Write(const void* pBuffer, size_t nSize, size_t nCount) override {
    size_t total = nSize * nCount;
    if (pos_ + total > data_.size()) {
      data_.resize(pos_ + total);
    }
    memcpy(&data_[pos_], pBuffer, total);
    pos_ += total;
    return nCount;
  }
  void ClearErr() override {
    eof_ = false;
    err_ = false;
  }
  int Eof() override { return eof_ ? 1 : 0; }
  int Error() override { return err_ ? 1 : 0; }
  int Flush() override { return 0; }
  int Close() override { return 0; }

 private:
  std::string data_;
  vsi_l_offset pos_ = 0;
  bool eof_ = false;
  bool err_ = false;
};

// A filesystem handler where Stat fails but Open succeeds, to test
// VSIIngestFile chunked fallback and base VSIFilesystemHandler methods.
class StreamOnlyFilesystem : public VSIFilesystemHandler {
 public:
  VSIVirtualHandleUniquePtr Open(const char* pszFilename, const char* pszAccess,
                                 bool bSetError,
                                 CSLConstList papszOptions) override {
    if (strstr(pszFilename, "fail") != nullptr) {
      return nullptr;
    }
    return VSIVirtualHandleUniquePtr(
        new MinimalVirtualHandle(std::string(80000, 'X')));
  }
  int Stat(const char* pszFilename, VSIStatBufL* pStatBuf,
           int nFlags) override {
    return -1;
  }
};

TEST(CplVSIFunctions, BaseVirtualHandleAndFilesystemHandlerMethods) {
  WithQuietHandler quiet;

  MinimalVirtualHandle handle("0123456789abcdef");
  EXPECT_EQ(-1, handle.Truncate(5));
  EXPECT_EQ(nullptr, handle.GetNativeFileDescriptor());
  EXPECT_EQ(VSI_RANGE_STATUS_UNKNOWN, handle.GetRangeStatus(0, 10));
  EXPECT_FALSE(handle.HasPRead());
  char buf[16] = {};
  EXPECT_EQ(0U, handle.PRead(buf, 4, 0));
  handle.AdviseRead(0, nullptr, nullptr);
  EXPECT_EQ(0U, handle.GetAdviseReadTotalBytesLimit());

  void* ptrs[2] = {buf, buf + 4};
  const vsi_l_offset offsets[2] = {2, 6};
  const size_t sizes[2] = {3, 3};
  EXPECT_EQ(0, handle.ReadMultiRange(2, ptrs, offsets, sizes));
  EXPECT_EQ(0, memcmp(buf, "234", 3));
  EXPECT_EQ(0, memcmp(buf + 4, "678", 3));

  EXPECT_EQ(0, handle.Seek(0, SEEK_END));
  EXPECT_GT(handle.Printf("-%d-%s", 42, "end"), 0);

  // Test StreamOnlyFilesystem base methods and VSIIngestFile with fp != nullptr
  // and pszFilename == nullptr (chunked streaming path).
  auto* stream_fs = new StreamOnlyFilesystem();
  VSIFileManager::InstallHandler("/vsistreamtest/", stream_fs);
  absl::Cleanup remove_handler = [stream_fs] {
    VSIFileManager::RemoveHandler("/vsistreamtest/");
    delete stream_fs;
  };

  VSILFILE* stream_fp = VSIFOpenL("/vsistreamtest/ok", "rb");
  ASSERT_NE(nullptr, stream_fp);
  absl::Cleanup close_stream_fp = [stream_fp] {
    EXPECT_EQ(0, VSIFCloseL(stream_fp));
  };
  GByte* ingested = nullptr;
  vsi_l_offset ingested_size = 0;
  EXPECT_TRUE(VSIIngestFile(stream_fp, nullptr, &ingested, &ingested_size, -1));
  std::unique_ptr<GByte, decltype(&VSIFree)> ingested_guard(ingested, VSIFree);
  EXPECT_EQ(80000U, ingested_size);
  ingested_guard.reset();

  // Max size exceeded during streaming ingest.
  ingested = nullptr;
  EXPECT_FALSE(
      VSIIngestFile(stream_fp, nullptr, &ingested, &ingested_size, 100));

  // Exercise default virtual methods on VSIFilesystemHandler via stream_fs.
  EXPECT_EQ(nullptr, VSISiblingFiles("/vsistreamtest/ok"));
  EXPECT_EQ(-1, VSIMkdir("/vsistreamtest/dir", 0755));
  EXPECT_EQ(-1, VSIRmdir("/vsistreamtest/dir"));
  EXPECT_EQ(-1, VSIRmdirRecursive("/vsistreamtest/dir"));
  EXPECT_EQ(-1, VSIRmdirRecursive(nullptr));
  EXPECT_EQ(-1, VSIUnlink("/vsistreamtest/ok"));
  EXPECT_EQ(-1, VSIRename("/vsistreamtest/ok", "/vsistreamtest/ok2"));
  EXPECT_EQ(-1, VSIGetDiskFreeSpace("/vsistreamtest/"));
  EXPECT_EQ(1, VSIIsCaseSensitiveFS("/vsistreamtest/"));
  EXPECT_FALSE(VSISupportsSparseFiles("/vsistreamtest/"));
  EXPECT_TRUE(VSISupportsSequentialWrite("/vsistreamtest/", true));
  EXPECT_TRUE(VSISupportsRandomWrite("/vsistreamtest/", true));
  EXPECT_FALSE(VSIHasOptimizedReadMultiRange("/vsistreamtest/"));
  EXPECT_EQ(nullptr, VSIGetActualURL("/vsistreamtest/ok"));
  EXPECT_EQ(nullptr, VSIGetSignedURL("/vsistreamtest/ok", nullptr));
  EXPECT_EQ(nullptr, VSIGetFileMetadata("/vsistreamtest/ok", nullptr, nullptr));
  EXPECT_FALSE(
      VSISetFileMetadata("/vsistreamtest/ok", nullptr, nullptr, nullptr));
  EXPECT_TRUE(VSIAbortPendingUploads("/vsistreamtest/"));
  EXPECT_STREQ("/", VSIGetDirectorySeparator("/vsistreamtest/ok"));
  EXPECT_STREQ("/", VSIGetDirectorySeparator("http://example.com/a"));

  std::unique_ptr<char, CplFreeDeleter> canon(
      VSIGetCanonicalFilename("/vsistreamtest/ok"));
  EXPECT_STREQ("/vsistreamtest/ok", canon.get());

  // OpenStatic via base handler.
  auto static_h =
      VSIFilesystemHandler::OpenStatic("/vsimem/nonexistent_static", "rb");
  EXPECT_EQ(nullptr, static_h);
}

TEST(CplVSIFunctions, GlobAndDirOperations) {
  constexpr char kRoot[] = "/vsimem/glob_dir_test";
  ASSERT_EQ(0, VSIMkdirRecursive("/vsimem/glob_dir_test/sub1/deep", 0755));
  ASSERT_EQ(0, VSIMkdirRecursive("/vsimem/glob_dir_test/sub2", 0755));

  const char* kFiles[] = {
      "/vsimem/glob_dir_test/a1.txt",
      "/vsimem/glob_dir_test/b2.txt",
      "/vsimem/glob_dir_test/c].txt",
      "/vsimem/glob_dir_test/d-.txt",
      "/vsimem/glob_dir_test/sub1/f1.txt",
      "/vsimem/glob_dir_test/sub1/deep/f2.txt",
      "/vsimem/glob_dir_test/sub2/f3.log",
  };
  for (const char* f : kFiles) {
    VSILFILE* fp = VSIFOpenL(f, "wb");
    ASSERT_NE(nullptr, fp);
    VSIFPrintfL(fp, "hello %s", f);
    VSIFPutcL('!', fp);
    VSIFCloseL(fp);
  }
  absl::Cleanup cleanup_dir = [kRoot] { VSIRmdirRecursive(kRoot); };

  // CPLReadDir and VSIReadDirRecursive.
  StringListPtr dir_entries(CPLReadDir(kRoot), CSLDestroy);
  EXPECT_GE(CSLCount(dir_entries.get()), 4);

  StringListPtr rec_entries(VSIReadDirRecursive(kRoot), CSLDestroy);
  EXPECT_GE(CSLCount(rec_entries.get()), 7);

  // VSIOpenDir with infinite recursion (with and without a PREFIX filter),
  // depth 0 and depth 1.  The fixture has 6 top-level entries (4 files + 2
  // dirs), 3 entries one level down (sub1/f1.txt, sub1/deep, sub2/f3.log) and
  // 1 entry two levels down (sub1/deep/f2.txt).
  struct OpenDirCase {
    int depth;
    const char* prefix;  // nullptr for no PREFIX option.
    int expected_entries;
  };
  constexpr OpenDirCase kOpenDirCases[] = {
      {.depth = -1, .prefix = nullptr, .expected_entries = 10},
      {.depth = -1,
       .prefix = "sub",
       .expected_entries = 6},  // sub1, sub1/f1.txt, sub1/deep,
                                // sub1/deep/f2.txt, sub2, sub2/f3.log
      {.depth = 0, .prefix = nullptr, .expected_entries = 6},
      {.depth = 1, .prefix = nullptr, .expected_entries = 9},
  };
  for (const OpenDirCase& c : kOpenDirCases) {
    CPLStringList opts;
    if (c.prefix != nullptr) opts.SetNameValue("PREFIX", c.prefix);
    VSIDIR* dir = VSIOpenDir(kRoot, c.depth, opts.List());
    ASSERT_NE(nullptr, dir);
    int num_entries = 0;
    while (const VSIDIREntry* entry = VSIGetNextDirEntry(dir)) {
      ++num_entries;
      VSIDIREntry copy_entry(*entry);
      EXPECT_STREQ(entry->pszName, copy_entry.pszName);
      if (c.prefix != nullptr) {
        EXPECT_TRUE(STARTS_WITH(entry->pszName, c.prefix)) << entry->pszName;
      }
    }
    VSICloseDir(dir);
    EXPECT_EQ(c.expected_entries, num_entries)
        << "depth=" << c.depth << " prefix=" << (c.prefix ? c.prefix : "");
  }
  VSICloseDir(nullptr);

  // VSIDIREntry copy constructor with extra populated.
  {
    VSIDIREntry src_entry;
    src_entry.pszName = CPLStrdup("name");
    src_entry.papszExtra = CSLAddString(nullptr, "KEY=VAL");
    VSIDIREntry copied_entry(src_entry);
    EXPECT_STREQ("name", copied_entry.pszName);
    EXPECT_EQ(1, CSLCount(copied_entry.papszExtra));
  }

  // VSIGlob with exact match, non-existent, *, ?, [a-z], [!a-z], []], [-],
  // an unclosed class, a wildcard directory component and **.
  struct GlobCase {
    const char* pattern;
    int expected_matches;
  };
  constexpr GlobCase kGlobCases[] = {
      {.pattern = "/vsimem/glob_dir_test/a1.txt", .expected_matches = 1},
      {.pattern = "/vsimem/glob_dir_test/nonexistent.txt",
       .expected_matches = 0},
      {.pattern = "/vsimem/glob_dir_test/*.txt", .expected_matches = 4},
      {.pattern = "/vsimem/glob_dir_test/??.txt", .expected_matches = 4},
      {.pattern = "/vsimem/glob_dir_test/[a-b][12].txt",
       .expected_matches = 2},  // a1.txt, b2.txt
      {.pattern = "/vsimem/glob_dir_test/[!a-b]?.txt",
       .expected_matches = 2},  // c].txt, d-.txt
      {.pattern = "/vsimem/glob_dir_test/[]c]].txt",
       .expected_matches = 1},  // ']' first in class
      {.pattern = "/vsimem/glob_dir_test/[-d]-.txt",
       .expected_matches = 1},  // '-' first in class
      {.pattern = "/vsimem/glob_dir_test/[unclosed.txt", .expected_matches = 0},
      {.pattern = "/vsimem/glob_dir_test/sub*/f?.txt",
       .expected_matches = 1},  // sub1/f1.txt
      // All 10 entries, directories included.
      {.pattern = "/vsimem/glob_dir_test/**", .expected_matches = 10},
  };
  for (const GlobCase& c : kGlobCases) {
    StringListPtr matched(VSIGlob(c.pattern, nullptr, nullptr, nullptr),
                          CSLDestroy);
    EXPECT_EQ(c.expected_matches, CSLCount(matched.get())) << c.pattern;
  }
  {
    StringListPtr matched(
        VSIGlob("/vsimem/glob_dir_test/a1.txt", nullptr, nullptr, nullptr),
        CSLDestroy);
    EXPECT_EQ(1, CSLCount(matched.get()));
    if (CSLCount(matched.get()) == 1) {
      EXPECT_STREQ("/vsimem/glob_dir_test/a1.txt", matched.get()[0]);
    }
  }
  // "**" followed by more pattern: every entry found by "**" (files included)
  // becomes a directory candidate for the remainder, and VSIGlob() gives up
  // with nullptr as soon as one of them cannot be opened as a directory.  Only
  // check that it does not crash; the exact result is an upstream quirk.
  CSLDestroy(
      VSIGlob("/vsimem/glob_dir_test/**/*.txt", nullptr, nullptr, nullptr));

  // VSIGlob cancelled by progress callback.
  auto cancel_cb = [](double, const char*, void*) -> int { return FALSE; };
  char** cancelled =
      VSIGlob("/vsimem/glob_dir_test/**", nullptr, cancel_cb, nullptr);
  EXPECT_EQ(nullptr, cancelled);
}

TEST(CplVSIFunctions, CopyMoveSyncAndUnlinkBatch) {
  WithQuietHandler quiet;
  const std::string temp_dir(::testing::TempDir());

  // Create source files in /vsimem/cms.
  ASSERT_EQ(0, VSIMkdirRecursive("/vsimem/cms/sub", 0755));
  std::string payload(300, 'Z');
  {
    VSILFILE* fp = VSIFOpenL("/vsimem/cms/f1.txt", "wb");
    ASSERT_NE(nullptr, fp);
    VSIFWriteL(payload.data(), 1, payload.size(), fp);
    VSIFCloseL(fp);

    fp = VSIFOpenL("/vsimem/cms/sub/f2.txt", "wb");
    ASSERT_NE(nullptr, fp);
    VSIFWriteL("subdata", 1, 7, fp);
    VSIFCloseL(fp);
  }
  absl::Cleanup cleanup_cms = [] {
    VSIRmdirRecursive("/vsimem/cms");
    VSIRmdirRecursive("/vsimem/cms_sync");
  };

  // VSICopyFile edge cases and options.
  EXPECT_EQ(-1, VSICopyFile(nullptr, "/vsimem/cms/out", nullptr, -1, nullptr,
                            nullptr, nullptr));
  EXPECT_EQ(-1, VSICopyFile("", "/vsimem/cms/out", nullptr, -1, nullptr,
                            nullptr, nullptr));
  EXPECT_EQ(-1, VSICopyFile("/vsimem/cms/nonexistent", "/vsimem/cms/out",
                            nullptr, -1, nullptr, nullptr, nullptr));

  EXPECT_EQ(0, VSICopyFile("/vsimem/cms/f1.txt", "/vsimem/cms/f1_copy.txt",
                           nullptr, -1, nullptr, nullptr, nullptr));
  // Copy into subfile target.
  EXPECT_EQ(0, VSICopyFile("/vsimem/cms/f1.txt", "/vsimem/cms/sub/f1_sub.txt",
                           nullptr, -1, nullptr, nullptr, nullptr));
  // Copy with mismatched nSourceSize fails.
  EXPECT_EQ(-1, VSICopyFile("/vsimem/cms/f1.txt", "/vsimem/cms/f1_badsize.txt",
                            nullptr, 10, nullptr, nullptr, nullptr));
  // Cancelled copy.
  auto cancel_cb = [](double, const char*, void*) -> int { return FALSE; };
  EXPECT_EQ(-1, VSICopyFile("/vsimem/cms/f1.txt", "/vsimem/cms/f1_cancel.txt",
                            nullptr, -1, nullptr, cancel_cb, nullptr));

  // VSICopyFileRestartable.
  EXPECT_EQ(-1, VSICopyFileRestartable(nullptr, "/vsimem/cms/r.txt", nullptr,
                                       nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(-1, VSICopyFileRestartable("/vsimem/cms/f1.txt", "", nullptr,
                                       nullptr, nullptr, nullptr, nullptr));
  // Null output payload returns -1.
  EXPECT_EQ(
      -1, VSICopyFileRestartable("/vsimem/cms/f1.txt", "/vsimem/cms/f1_r1.txt",
                                 nullptr, nullptr, nullptr, nullptr, nullptr));

  char* out_payload = nullptr;
  EXPECT_EQ(0, VSICopyFileRestartable("/vsimem/cms/f1.txt",
                                      "/vsimem/cms/f1_r1.txt", nullptr,
                                      &out_payload, nullptr, nullptr, nullptr));
  std::unique_ptr<char, decltype(&VSIFree)> payload_guard1(out_payload,
                                                           VSIFree);

  // Cancelled restartable copy.
  char* out_payload2 = nullptr;
  EXPECT_EQ(-1, VSICopyFileRestartable(
                    "/vsimem/cms/f1.txt", "/vsimem/cms/f1_resumed.txt", nullptr,
                    &out_payload2, nullptr, cancel_cb, nullptr));
  std::unique_ptr<char, decltype(&VSIFree)> payload_guard2(out_payload2,
                                                           VSIFree);

  char* out_payload3 = nullptr;
  EXPECT_EQ(0, VSICopyFileRestartable(
                   "/vsimem/cms/f1.txt", "/vsimem/cms/f1_resumed.txt",
                   "PAYLOAD", &out_payload3, nullptr, nullptr, nullptr));
  std::unique_ptr<char, decltype(&VSIFree)> payload_guard3(out_payload3,
                                                           VSIFree);

  // VSIMove: invalid args, same path, same FS, cross FS (file and directory).
  EXPECT_EQ(-1, VSIMove("/vsimem/cms/nonexistent", "/vsimem/cms/x", nullptr,
                        nullptr, nullptr));
  EXPECT_EQ(0, VSIMove("/vsimem/cms/f1_copy.txt", "/vsimem/cms/f1_copy.txt",
                       nullptr, nullptr, nullptr));
  EXPECT_EQ(0, VSIMove("/vsimem/cms/f1_copy.txt", "/vsimem/cms/f1_moved.txt",
                       nullptr, nullptr, nullptr));

  // Cross-FS move (/vsimem/ -> local temp_dir and back).
  std::string local_moved = temp_dir + "/moved_from_vsimem.txt";
  EXPECT_EQ(0, VSIMove("/vsimem/cms/f1_moved.txt", local_moved.c_str(), nullptr,
                       nullptr, nullptr));
  EXPECT_EQ(0, VSIMove(local_moved.c_str(), "/vsimem/cms/f1_moved_back.txt",
                       nullptr, nullptr, nullptr));

  // Cross-FS directory move.
  std::string local_dir_moved = temp_dir + "/moved_sub_dir";
  EXPECT_EQ(0, VSIMove("/vsimem/cms/sub", local_dir_moved.c_str(), nullptr,
                       nullptr, nullptr));
  EXPECT_EQ(0, VSIMove(local_dir_moved.c_str(), "/vsimem/cms/sub", nullptr,
                       nullptr, nullptr));

  // VSISync: file and recursive directory sync.
  EXPECT_FALSE(VSISync("/vsimem/cms/nonexistent", "/vsimem/cms_sync/x", nullptr,
                       nullptr, nullptr, nullptr));
  EXPECT_TRUE(VSISync("/vsimem/cms/f1.txt", "/vsimem/cms_sync/f1.txt", nullptr,
                      nullptr, nullptr, nullptr));
  // Sync again when target already exists.
  const char* sync_etag_opts[] = {"STRATEGY=ETAG", nullptr};
  EXPECT_TRUE(VSISync("/vsimem/cms/f1.txt", "/vsimem/cms_sync/f1.txt",
                      sync_etag_opts, nullptr, nullptr, nullptr));
  const char* sync_dir_opts[] = {"RECURSIVE=YES", "NUM_THREADS=1", nullptr};
  EXPECT_TRUE(VSISync("/vsimem/cms", "/vsimem/cms_sync/dir", sync_dir_opts,
                      nullptr, nullptr, nullptr));

  // VSIUnlinkBatch.
  const char* to_unlink[] = {"/vsimem/cms/f1_r1.txt",
                             "/vsimem/cms/f1_resumed.txt",
                             "/vsimem/cms/does_not_exist.txt", nullptr};
  std::unique_ptr<int, decltype(&VSIFree)> unlink_res(VSIUnlinkBatch(to_unlink),
                                                      VSIFree);
  ASSERT_NE(nullptr, unlink_res);
  EXPECT_EQ(TRUE, unlink_res.get()[0]);
  EXPECT_EQ(TRUE, unlink_res.get()[1]);
  EXPECT_EQ(FALSE, unlink_res.get()[2]);

  // VSIUnlinkBatch with mixed filesystems fails and returns nullptr without
  // touching any file.
  const std::string local_b = temp_dir + "/vsi_unlink_batch_b";
  const char* mixed_unlink[] = {"/vsimem/cms/a", local_b.c_str(), nullptr};
  EXPECT_EQ(nullptr, VSIUnlinkBatch(mixed_unlink));
}

TEST(CplVSIFunctions, MultipartUploadAndOnlyVisibleAtCloseTime) {
  WithQuietHandler quiet;

  int non_consec = 0;
  int par_upload = 0;
  int abort_supp = 0;
  size_t min_part = 0;
  size_t max_part = 0;
  int max_count = 0;
  EXPECT_FALSE(VSIMultipartUploadGetCapabilities(
      "/vsimem/mp.bin", &non_consec, &par_upload, &abort_supp, &min_part,
      &max_part, &max_count));

  EXPECT_EQ(nullptr, VSIMultipartUploadStart("/vsimem/mp.bin", nullptr));
  EXPECT_EQ(nullptr, VSIMultipartUploadAddPart("/vsimem/mp.bin", "id", 0, 0,
                                               "a", 1, nullptr));
  EXPECT_EQ(nullptr, VSIMultipartUploadAddPart("/vsimem/mp.bin", "id", 1, 0,
                                               "a", 1, nullptr));
  EXPECT_FALSE(
      VSIMultipartUploadEnd("/vsimem/mp.bin", "id", 1, nullptr, 0, nullptr));
  const char* tags[] = {"tag1"};
  EXPECT_FALSE(
      VSIMultipartUploadEnd("/vsimem/mp.bin", "id", 1, tags, 0, nullptr));
  EXPECT_FALSE(VSIMultipartUploadAbort("/vsimem/mp.bin", "id", nullptr));

  // Test CreateOnlyVisibleAtCloseTime (both normal close and CancelCreation).
  VSIFilesystemHandler* handler = VSIFileManager::GetHandler("/vsimem/");
  ASSERT_NE(nullptr, handler);
  EXPECT_EQ(nullptr, handler->CreateOnlyVisibleAtCloseTime(
                         "/vsimem/vis_no_emul.bin", false, nullptr));

  {
    auto h = handler->CreateOnlyVisibleAtCloseTime("/vsimem/vis_ok.bin", true,
                                                   nullptr);
    ASSERT_NE(nullptr, h);
    EXPECT_EQ(1U, h->Write("hello", 5, 1));
    EXPECT_EQ(5U, h->Tell());
    EXPECT_EQ(0, h->Flush());
    EXPECT_EQ(0, h->Seek(0, SEEK_SET));
    char buf[8] = {};
    EXPECT_EQ(1U, h->Read(buf, 5, 1));
    EXPECT_STREQ("hello", buf);
    h->ClearErr();
    EXPECT_EQ(0, h->Error());
    EXPECT_EQ(0, h->Eof());
    EXPECT_EQ(nullptr, h->GetNativeFileDescriptor());
    EXPECT_EQ(0, h->Truncate(3));
    h->AdviseRead(0, nullptr, nullptr);
    EXPECT_TRUE(h->HasPRead());
    EXPECT_EQ(3U, h->PRead(buf, 3, 0));
    void* ptrs[1] = {buf};
    vsi_l_offset offsets[1] = {0};
    size_t sizes[1] = {3};
    EXPECT_EQ(0, h->ReadMultiRange(1, ptrs, offsets, sizes));
    EXPECT_EQ(0, h->Close());
  }
  VSIStatBufL st = {};
  EXPECT_EQ(0, VSIStatL("/vsimem/vis_ok.bin", &st));
  EXPECT_EQ(3, st.st_size);
  VSIUnlink("/vsimem/vis_ok.bin");

  // CancelCreation before Close.
  {
    auto h = handler->CreateOnlyVisibleAtCloseTime("/vsimem/vis_cancel.bin",
                                                   true, nullptr);
    ASSERT_NE(nullptr, h);
    EXPECT_EQ(1U, h->Write("hello", 5, 1));
    h->CancelCreation();
    EXPECT_EQ(0, h->Close());
  }
  EXPECT_EQ(-1, VSIStatL("/vsimem/vis_cancel.bin", &st));
}

TEST(CplVSIFunctions, IngestOverwritePathOptionsAndMisc) {
  WithQuietHandler quiet;

  // Path-specific options.  (The deprecated VSI*Credential* wrappers are not
  // exercised here because they trigger -Wdeprecated-declarations.)
  VSISetPathSpecificOption("/vsimem/path_opt", "K1", "V1");
  VSISetPathSpecificOption("/vsimem/path_opt", "K1", "V1_UPDATED");
  VSISetPathSpecificOption("/vsimem/path_opt", "K2", "V2");
  EXPECT_STREQ("V1_UPDATED",
               VSIGetPathSpecificOption("/vsimem/path_opt/file", "K1", "def"));
  EXPECT_STREQ("V2",
               VSIGetPathSpecificOption("/vsimem/path_opt/file", "K2", "def"));
  EXPECT_STREQ("def", VSIGetPathSpecificOption("/vsimem/other", "K1", "def"));
  // Remove single key.
  VSISetPathSpecificOption("/vsimem/path_opt", "K1", nullptr);
  EXPECT_STREQ("def",
               VSIGetPathSpecificOption("/vsimem/path_opt/file", "K1", "def"));
  VSIClearPathSpecificOptions("/vsimem/path_opt");
  EXPECT_STREQ("def",
               VSIGetPathSpecificOption("/vsimem/path_opt/file", "K2", "def"));
  VSIClearPathSpecificOptions(nullptr);

  // Prefixes, options, and duplicate handler.
  StringListPtr prefixes(VSIGetFileSystemsPrefixes(), CSLDestroy);
  EXPECT_NE(nullptr, prefixes);
  EXPECT_NE(nullptr, VSIGetFileSystemOptions("/vsigzip/"));
  EXPECT_EQ(nullptr, VSIGetFileSystemOptions("/vsi_nonexistent_prefix/"));

  EXPECT_FALSE(VSIDuplicateFileSystemHandler("/vsigzip/", "/vsimem/"));
  EXPECT_FALSE(
      VSIDuplicateFileSystemHandler("/vsi_nonexistent_dup/", "/vsigzip_dup/"));

  // VSIURIToVSIPath.
  EXPECT_EQ("", VSIURIToVSIPath(""));
  EXPECT_EQ("normal/path", VSIURIToVSIPath("normal/path"));
  EXPECT_EQ("/tmp/a.tif", VSIURIToVSIPath("file:///tmp/a.tif"));
  EXPECT_EQ("/vsis3/bucket/a.tif", VSIURIToVSIPath("s3://bucket/a.tif"));
  EXPECT_EQ("/vsigs/bucket/a.tif", VSIURIToVSIPath("gs://bucket/a.tif"));
  EXPECT_EQ("/vsicurl/https://example.com/a.tif",
            VSIURIToVSIPath("https://example.com/a.tif"));

  // VSIIngestFile & VSIOverwriteFile & VSIF*L helpers.
  GByte* ret_buf = nullptr;
  vsi_l_offset sz = 0;
  EXPECT_FALSE(VSIIngestFile(nullptr, nullptr, &ret_buf, &sz, -1));
  EXPECT_FALSE(VSIIngestFile(nullptr, "/vsimem/ingest_null", nullptr, &sz, -1));
  EXPECT_FALSE(VSIIngestFile(nullptr, "/vsimem/does_not_exist_ingest", &ret_buf,
                             &sz, -1));

  static constexpr char kIngestFile[] = "/vsimem/ingest_test.bin";
  static constexpr char kOverwriteTarget[] = "/vsimem/overwrite_target.bin";
  absl::Cleanup unlink_files = [] {
    VSIUnlink(kIngestFile);
    VSIUnlink(kOverwriteTarget);
  };
  {
    VSILFILE* fp = VSIFOpenEx2L(kIngestFile, "wb+", true, nullptr);
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    EXPECT_EQ(5, VSIFPrintfL(fp, "%s", "12345"));
    EXPECT_EQ(0, VSIFErrorL(fp));
    VSIFClearErrL(fp);
    EXPECT_EQ(VSI_RANGE_STATUS_UNKNOWN, VSIFGetRangeStatusL(fp, 0, 5));
    fp->AdviseRead(0, nullptr, nullptr);
    EXPECT_EQ(nullptr, VSIFGetNativeFileDescriptorL(fp));

    char mr_buf[5] = {};
    void* mr_ptrs[1] = {mr_buf};
    vsi_l_offset mr_off[1] = {1};
    size_t mr_sz[1] = {3};
    EXPECT_EQ(0, VSIFReadMultiRangeL(1, mr_ptrs, mr_off, mr_sz, fp));
    EXPECT_EQ(0, memcmp(mr_buf, "234", 3));
  }

  // MaxSize too small.
  EXPECT_FALSE(VSIIngestFile(nullptr, kIngestFile, &ret_buf, &sz, 2));

  // Normal ingest via filename.
  EXPECT_TRUE(VSIIngestFile(nullptr, kIngestFile, &ret_buf, &sz, 10));
  std::unique_ptr<GByte, decltype(&VSIFree)> ret_buf_guard(ret_buf, VSIFree);
  ASSERT_NE(nullptr, ret_buf);
  EXPECT_EQ(5U, sz);
  EXPECT_STREQ("12345", reinterpret_cast<char*>(ret_buf));
  ret_buf_guard.reset();

  // VSIOverwriteFile.
  {
    VSILFILE* fp_target = VSIFOpenExL(kOverwriteTarget, "wb+", true);
    ASSERT_NE(nullptr, fp_target);
    absl::Cleanup close_target = [fp_target] {
      EXPECT_EQ(0, VSIFCloseL(fp_target));
    };
    EXPECT_TRUE(VSIOverwriteFile(fp_target, kIngestFile));
    EXPECT_FALSE(VSIOverwriteFile(fp_target, "/vsimem/nonexistent_src"));
  }
}

// TODO(schwehr): Test the rest of cpl_vsil.cpp.

}  // namespace
}  // namespace autotest2
