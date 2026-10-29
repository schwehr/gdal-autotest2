
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
//
// Tests for gzipped virtual file system (VSI) files.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_gzip.cpp

#include <sys/types.h>

#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/log/check.h"
#include "third_party/absl/log/die_if_null.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

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
    "google3/third_party/gdal/autotest2/cpp/port/testdata/gzip");

std::filesystem::path GetTestFilePath(const std::string_view filename) {
  return std::filesystem::path(::testing::SrcDir()) / kTestDataPath / filename;
}

constexpr char kFilename[] = "count.gz";

constexpr off_t kCompressedSize = 67;
constexpr off_t kUncompressedSize = 125;

// The raw bytes of testdata/gzip/count.gz (kCompressedSize bytes, inflating to
// kUncompressedSize bytes).
constexpr uint8_t kCountGz[] = {
    0x1f, 0x8b, 0x08, 0x08, 0x27, 0x82, 0xda, 0x58, 0x02, 0x03, 'c',  'o',
    'u',  'n',  't',  0x00, 0x4d, 0xc6, 0xab, 0x01, 0x00, 0x20, 0x08, 0x40,
    0xc1, 0xce, 0x34, 0x80, 0xfc, 0xdc, 0x47, 0xab, 0xfb, 0x47, 0xeb, 0xbb,
    0x74, 0xaa, 0xe6, 0x2b, 0xb2, 0x7a, 0xb6, 0x18, 0xee, 0xf8, 0xc2, 0x03,
    0x4f, 0xbc, 0xf0, 0xc6, 0x07, 0xdf, 0xf8, 0x7d, 0x47, 0xe4, 0x03, 0x9c,
    0xa1, 0xb4, 0x30, 0x7d, 0x00, 0x00, 0x00};
static_assert(static_cast<off_t>(sizeof(kCountGz)) == kCompressedSize);

std::string CountGzAsString() {
  return std::string(reinterpret_cast<const char*>(kCountGz), sizeof(kCountGz));
}

TEST(CplVsiGzipTest, Read) {
  const auto filepath = GetTestFilePath(kFilename);

  constexpr int kBufSize = 256;

  // Read raw.
  {
    char buf[kBufSize] = {};
    VSILFILE* file = VSIFOpenL(filepath.c_str(), "r");
    ASSERT_NE(nullptr, file);
    CplVsiLFileCloser closer(file);
    EXPECT_EQ(kCompressedSize, VSIFReadL(buf, 1, kBufSize - 1, file));

    // The gzip file contains the original filename.
    EXPECT_THAT(buf, testing::ContainsRegex("count"));
  }

  // Read uncompressed.
  {
    const std::string vsipath = "/vsigzip/" + std::string(filepath);
    char buf[kBufSize] = {};
    VSILFILE* file = VSIFOpenL(vsipath.c_str(), "r");
    ASSERT_NE(nullptr, file) << vsipath;
    CplVsiLFileCloser closer(file);
    EXPECT_EQ(kUncompressedSize, VSIFReadL(buf, 1, kBufSize - 1, file));

    // Look for some text in the original file.
    EXPECT_THAT(buf, testing::ContainsRegex("end"));
  }

  // Read from vsimem.
  const char kFilename[] = "/vsimem/a";
  autotest2::VsiMemTempWrapper wrapper(kFilename, CountGzAsString());

  // Read raw.
  {
    char buf[kBufSize] = {};
    VSILFILE* file = VSIFOpenL(kFilename, "r");
    ASSERT_NE(nullptr, file);
    CplVsiLFileCloser closer(file);
    EXPECT_EQ(kCompressedSize, VSIFReadL(buf, 1, kBufSize - 1, file));

    // The gzip file contains the original filename.
    EXPECT_THAT(buf, testing::ContainsRegex("count"));
  }

  // Read uncompressed.
  {
    // Note the double slash: //
    // A single slash between the two vsi paths does not work:
    //   /vsigzip/vsimem/a
    char buf[kBufSize] = {};
    VSILFILE* file = VSIFOpenL("/vsigzip//vsimem/a", "r");
    ASSERT_NE(nullptr, file);
    CplVsiLFileCloser closer(file);
    EXPECT_EQ(kUncompressedSize, VSIFReadL(buf, 1, kBufSize - 1, file));

    // Look for some text in the original file.
    EXPECT_THAT(buf, testing::ContainsRegex("end"));
  }
}

// TODO(schwehr): Test opening an empty file that claims to be a gzip.
// TODO(schwehr): Test non-empty files that claims to be a gzip, but is not.
// TODO(schwehr): Test corrupt gzip file(s).

// Test how stat behaves outside and inside of gzipped file.
TEST(CplVsiGzipTest, Stat) {
  const auto filepath = GetTestFilePath(kFilename);

  VSIStatBufL stat;
  ASSERT_EQ(kSuccess, VSIStatExL(filepath.c_str(), &stat, VSI_STAT_SIZE_FLAG));
  EXPECT_EQ(kCompressedSize, stat.st_size);

  const std::string vsipath = "/vsigzip/" + std::string(filepath);
  ASSERT_EQ(kSuccess, VSIStatExL(vsipath.c_str(), &stat, VSI_STAT_SIZE_FLAG))
      << vsipath;
  EXPECT_EQ(kUncompressedSize, stat.st_size) << vsipath;
}

TEST(CplVsiGzipTest, GzipWriteSingleAndMultiThreadedAndDirectWritable) {
  WithQuietHandler quiet;

  // Prepare 140 KB of pseudo-random + structured data so compressed size > 64KB
  // (Z_BUFSIZE), exercising snapshot creation and multi-chunk deflate.
  std::vector<uint8_t> payload(140000);
  uint32_t rng = 0x12345678U;
  for (size_t i = 0; i < payload.size(); ++i) {
    rng = rng * 1664525U + 1013904223U;
    payload[i] = static_cast<uint8_t>((rng >> 16) & 0xFF);
  }

  // Invalid mode "w+" on /vsigzip/.
  EXPECT_EQ(nullptr, VSIFOpenL("/vsigzip//vsimem/bad_mode.gz", "w+"));

  // 1. Single-threaded write via /vsigzip/.
  const char* kStGz = "/vsimem/st_write.gz";
  const char* kStVsiGz = "/vsigzip//vsimem/st_write.gz";
  {
    CPLConfigOptionSetter st_threads("GDAL_NUM_THREADS", "1", false);
    VSILFILE* fp = VSIFOpenL(kStVsiGz, "wb1");
    ASSERT_NE(nullptr, fp);
    EXPECT_EQ(0U, VSIFTellL(fp));
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_CUR));
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_END));
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));
    EXPECT_EQ(-1, VSIFSeekL(fp, 10, SEEK_SET));
    char dummy[4] = {};
    EXPECT_EQ(0U, VSIFReadL(dummy, 1, 4, fp));
    EXPECT_EQ(0, VSIFEofL(fp));
    EXPECT_EQ(0, VSIFErrorL(fp));
    VSIFClearErrL(fp);

    EXPECT_EQ(payload.size(),
              VSIFWriteL(payload.data(), 1, payload.size(), fp));
    EXPECT_EQ(payload.size(), VSIFTellL(fp));
    EXPECT_EQ(0, VSIFFlushL(fp));
    EXPECT_EQ(0, VSIFCloseL(fp));
  }

  // Read back and seek using snapshots + test Duplicate() via second open.
  {
    VSILFILE* fp1 = VSIFOpenL(kStVsiGz, "rb");
    ASSERT_NE(nullptr, fp1);

    // Read full file to populate snapshots across >64KB compressed stream.
    std::vector<uint8_t> read_back(payload.size());
    EXPECT_EQ(payload.size(),
              VSIFReadL(read_back.data(), 1, read_back.size(), fp1));
    EXPECT_EQ(0, memcmp(read_back.data(), payload.data(), payload.size()));

    // Read past EOF.
    uint8_t extra = 0;
    EXPECT_EQ(0U, VSIFReadL(&extra, 1, 1, fp1));
    EXPECT_NE(0, VSIFEofL(fp1));

    // Seek backwards into middle (uses snapshots) and read.
    EXPECT_EQ(0, VSIFSeekL(fp1, 50000, SEEK_SET));
    EXPECT_EQ(50000U, VSIFTellL(fp1));
    EXPECT_EQ(0, VSIFEofL(fp1));
    std::vector<uint8_t> mid(1000);
    EXPECT_EQ(1000U, VSIFReadL(mid.data(), 1, 1000, fp1));
    EXPECT_EQ(0, memcmp(mid.data(), payload.data() + 50000, 1000));

    // Seek forward with SEEK_CUR.
    EXPECT_EQ(0, VSIFSeekL(fp1, 5000, SEEK_CUR));
    EXPECT_EQ(56000U, VSIFTellL(fp1));
    EXPECT_EQ(100U, VSIFReadL(mid.data(), 1, 100, fp1));
    EXPECT_EQ(0, memcmp(mid.data(), payload.data() + 56000, 100));

    // Seek to end with SEEK_END.
    EXPECT_EQ(0, VSIFSeekL(fp1, 0, SEEK_END));
    EXPECT_EQ(payload.size(), VSIFTellL(fp1));

    // Unsupported Write and Flush on read-only handle.
    EXPECT_EQ(0U, VSIFWriteL("x", 1, 1, fp1));
    EXPECT_EQ(0, VSIFFlushL(fp1));
    EXPECT_EQ(0, VSIFErrorL(fp1));
    VSIFClearErrL(fp1);

    // Open a second handle while cached -> triggers Duplicate() with snapshots.
    VSILFILE* fp2 = VSIFOpenL(kStVsiGz, "rb");
    ASSERT_NE(nullptr, fp2);
    EXPECT_EQ(0, VSIFSeekL(fp2, 70000, SEEK_SET));
    EXPECT_EQ(100U, VSIFReadL(mid.data(), 1, 100, fp2));
    EXPECT_EQ(0, memcmp(mid.data(), payload.data() + 70000, 100));
    EXPECT_EQ(0, VSIFCloseL(fp2));
    EXPECT_EQ(0, VSIFCloseL(fp1));
  }

  // Test VSIReadDir, VSIGetFileMetadata, options, capabilities on /vsigzip/.
  char** gz_dir = VSIReadDir(kStVsiGz);
  EXPECT_EQ(0, CSLCount(gz_dir));
  CSLDestroy(gz_dir);
  EXPECT_NE(nullptr, VSIGetFileSystemOptions("/vsigzip/"));
  EXPECT_FALSE(VSISupportsSequentialWrite("/vsigzip/", true));
  EXPECT_FALSE(VSISupportsRandomWrite("/vsigzip/", true));
  VSIUnlink(kStGz);

  // 2. Multi-threaded write via /vsigzip/ (VSIGZipWriteHandleMT).
  const char* kMtGz = "/vsimem/mt_write.gz";
  const char* kMtVsiGz = "/vsigzip//vsimem/mt_write.gz";
  {
    CPLConfigOptionSetter mt_threads("GDAL_NUM_THREADS", "2", false);
    CPLConfigOptionSetter mt_chunk("CPL_VSIL_DEFLATE_CHUNK_SIZE", "16K", false);
    VSILFILE* fp = VSIFOpenL(kMtVsiGz, "wb");
    ASSERT_NE(nullptr, fp);
    EXPECT_EQ(0U, VSIFTellL(fp));
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_CUR));
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_END));
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));
    EXPECT_EQ(-1, VSIFSeekL(fp, 10, SEEK_SET));
    char dummy[4] = {};
    EXPECT_EQ(0U, VSIFReadL(dummy, 1, 4, fp));
    EXPECT_EQ(0, VSIFFlushL(fp));
    EXPECT_EQ(0, VSIFEofL(fp));
    EXPECT_EQ(0, VSIFErrorL(fp));
    VSIFClearErrL(fp);

    // Write in multiple calls and one large call exceeding
    // nChunks * nChunkSize.
    EXPECT_EQ(10000U, VSIFWriteL(payload.data(), 1, 10000, fp));
    EXPECT_EQ(payload.size() - 10000U, VSIFWriteL(payload.data() + 10000, 1,
                                                  payload.size() - 10000, fp));
    EXPECT_EQ(payload.size(), VSIFTellL(fp));
    EXPECT_EQ(0, VSIFCloseL(fp));
  }
  {
    VSILFILE* fp = VSIFOpenL(kMtVsiGz, "rb");
    ASSERT_NE(nullptr, fp);
    std::vector<uint8_t> read_back(payload.size());
    EXPECT_EQ(payload.size(),
              VSIFReadL(read_back.data(), 1, read_back.size(), fp));
    EXPECT_EQ(0, memcmp(read_back.data(), payload.data(), payload.size()));
    EXPECT_EQ(0, VSIFCloseL(fp));
  }
  VSIUnlink(kMtGz);

  // 3. Direct calls to VSICreateGZipWritable (ZLIB, RAW_DEFLATE, GZIP with
  // SOZIP index size 4 and 8, single-threaded and multi-threaded).
  for (int deflate_type : {CPL_DEFLATE_TYPE_GZIP, CPL_DEFLATE_TYPE_ZLIB,
                           CPL_DEFLATE_TYPE_RAW_DEFLATE}) {
    for (int threads : {1, 2}) {
      for (size_t elt_size : {static_cast<size_t>(4), static_cast<size_t>(8)}) {
        // VSILFILE is a typedef of VSIVirtualHandle.
        VSIVirtualHandle* base = VSIFOpenL("/vsimem/direct_writable.bin", "wb");
        ASSERT_NE(nullptr, base);
        std::vector<uint8_t> sozip_index;
        // bAutoCloseBaseHandle=true: `writable` owns `base` from here on.
        std::unique_ptr<VSIVirtualHandle> writable(VSICreateGZipWritable(
            base, deflate_type, true, threads, 16384, elt_size, &sozip_index));
        ASSERT_NE(nullptr, writable);
        EXPECT_EQ(1U, writable->Write(payload.data(), 40000, 1));
        EXPECT_EQ(0, writable->Close());
        writable.reset();
        VSIUnlink("/vsimem/direct_writable.bin");
      }
    }
  }
}

TEST(CplVsiGzipTest, GzipHeaderFlagsConcatenatedCorruptAndProperties) {
  WithQuietHandler quiet;

  // Craft a .gz with EXTRA_FIELD (0x04), ORIG_NAME (0x08), COMMENT (0x10),
  // HEAD_CRC (0x02), followed by a second concatenated .gz member, using
  // kCountGz as the base valid member.

  // Build member 1 with flags = 0x1E (EXTRA | ORIG_NAME | COMMENT | HEAD_CRC).
  std::string fancy_gz;
  fancy_gz.append("\x1f\x8b\x08\x1e\x00\x00\x00\x00\x00\x03", 10);
  // EXTRA_FIELD: len = 3, "xyz"
  fancy_gz.append("\x03\x00xyz", 5);
  // ORIG_NAME: "name\0"
  fancy_gz.append("name\0", 5);
  // COMMENT: "comment\0"
  fancy_gz.append("comment\0", 8);
  // HEAD_CRC: 2 bytes
  fancy_gz.append("\x12\x34", 2);
  // Deflate body + trailer from kCountGz (starting at offset 16):
  fancy_gz.append(reinterpret_cast<const char*>(kCountGz + 16),
                  sizeof(kCountGz) - 16);
  // Append a second complete .gz member (concatenated gzip file!).
  fancy_gz.append(CountGzAsString());

  {
    autotest2::VsiMemTempWrapper wrap("/vsimem/fancy_concat.gz", fancy_gz);
    VSILFILE* fp = VSIFOpenL("/vsigzip//vsimem/fancy_concat.gz", "rb");
    ASSERT_NE(nullptr, fp);
    char buf[512] = {};
    EXPECT_EQ(250U, VSIFReadL(buf, 1, sizeof(buf), fp));
    EXPECT_EQ(0, VSIFCloseL(fp));
  }

  // Test .gz.properties generation and reading on a local filesystem file.
  const std::string local_gz =
      std::string(::testing::TempDir()) + "/prop_test.gz";
  const std::string local_prop = local_gz + ".properties";
  {
    VSILFILE* fp = VSIFOpenL(local_gz.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    VSIFWriteL(kCountGz, 1, sizeof(kCountGz), fp);
    VSIFCloseL(fp);
  }
  {
    CPLConfigOptionSetter write_props("CPL_VSIL_GZIP_WRITE_PROPERTIES", "YES",
                                      false);
    std::string vsi_local_gz = "/vsigzip/" + local_gz;
    VSIStatBufL st = {};
    EXPECT_EQ(0, VSIStatL(vsi_local_gz.c_str(), &st));
    EXPECT_EQ(125, st.st_size);
    // Open a different .gz to evict the 1-entry poArchivedFH cache in
    // VSIGZipFilesystemHandler, then re-stat local_gz so it reads .properties!
    autotest2::VsiMemTempWrapper wrap2("/vsimem/evict_cache.gz",
                                       CountGzAsString());
    EXPECT_EQ(0, VSIStatL("/vsigzip//vsimem/evict_cache.gz", &st));
    EXPECT_EQ(0, VSIStatL(vsi_local_gz.c_str(), &st));
    EXPECT_EQ(125, st.st_size);
  }
  VSIUnlink(local_gz.c_str());
  VSIUnlink(local_prop.c_str());

  // Corrupt / invalid .gz files.
  // 1. Empty file.
  {
    autotest2::VsiMemTempWrapper w("/vsimem/empty.gz", "");
    EXPECT_EQ(nullptr, VSIFOpenL("/vsigzip//vsimem/empty.gz", "rb"));
  }
  // 2. Non-gzip file.
  {
    autotest2::VsiMemTempWrapper w("/vsimem/not_gz.gz", "plain text not gzip");
    EXPECT_EQ(nullptr, VSIFOpenL("/vsigzip//vsimem/not_gz.gz", "rb"));
  }
  // 3. Reserved flags set in header.
  {
    std::string bad_flags = CountGzAsString();
    bad_flags[3] = static_cast<char>(0xE0);
    autotest2::VsiMemTempWrapper w("/vsimem/bad_flags.gz", bad_flags);
    VSILFILE* fp = VSIFOpenL("/vsigzip//vsimem/bad_flags.gz", "rb");
    ASSERT_NE(nullptr, fp);
    char buf[16] = {};
    EXPECT_EQ(0U, VSIFReadL(buf, 1, sizeof(buf), fp));
    EXPECT_NE(0, VSIFErrorL(fp));
    VSIFCloseL(fp);
  }
  // 4. Corrupt deflate stream body. The gzip header (first 16 bytes) is intact
  // so Open() succeeds; the first body byte 0xFF selects the reserved deflate
  // block type 3, so inflate() fails with Z_DATA_ERROR on the first Read().
  {
    std::string bad_body = CountGzAsString();
    for (size_t i = 16; i < 30; ++i) bad_body[i] = '\xFF';
    autotest2::VsiMemTempWrapper w("/vsimem/bad_body.gz", bad_body);
    VSILFILE* fp = VSIFOpenL("/vsigzip//vsimem/bad_body.gz", "rb");
    ASSERT_NE(nullptr, fp);
    char buf[256] = {};
    EXPECT_EQ(0U, VSIFReadL(buf, 1, sizeof(buf), fp));
    EXPECT_NE(0, VSIFErrorL(fp));
    VSIFCloseL(fp);
  }
  // 5. Corrupt CRC32 trailer.
  {
    std::string bad_crc = CountGzAsString();
    bad_crc[bad_crc.size() - 6] ^= 0xFF;
    autotest2::VsiMemTempWrapper w("/vsimem/bad_crc.gz", bad_crc);
    VSILFILE* fp = VSIFOpenL("/vsigzip//vsimem/bad_crc.gz", "rb");
    ASSERT_NE(nullptr, fp);
    char buf[256] = {};
    VSIFReadL(buf, 1, sizeof(buf), fp);
    EXPECT_NE(0, VSIFErrorL(fp));
    VSIFCloseL(fp);
  }
  // Evict VSIGZipFilesystemHandler::poHandleLastGZipFile with
  // CPL_VSIL_GZIP_SAVE_INFO=NO so no cached VSIGZipHandle remains at exit.
  {
    CPLConfigOptionSetter no_save("CPL_VSIL_GZIP_SAVE_INFO", "NO", false);
    autotest2::VsiMemTempWrapper w("/vsimem/clear_last_gz.gz",
                                   CountGzAsString());
    VSILFILE* fp = VSIFOpenL("/vsigzip//vsimem/clear_last_gz.gz", "rb");
    ASSERT_NE(nullptr, fp);
    VSIFCloseL(fp);
  }
}

TEST(CplVsiGzipTest, VsiZipReadWriteCopyAndErrors) {
  WithQuietHandler quiet;

  EXPECT_NE(nullptr, VSIGetFileSystemOptions("/vsizip/"));
  EXPECT_FALSE(VSISupportsSequentialWrite("/vsizip/", true));
  EXPECT_FALSE(VSISupportsRandomWrite("/vsizip/", true));

  const char* kZipPath = "/vsimem/test_archive.zip";
  const char* kVsiZip = "/vsizip//vsimem/test_archive.zip";

  // Helper source file for VSICopyFile into zip.
  {
    VSILFILE* fp = VSIFOpenL("/vsimem/zip_copy_src.txt", "wb");
    ASSERT_NE(nullptr, fp);
    VSIFWriteL("copied_into_zip_content", 1, 23, fp);
    VSIFCloseL(fp);
  }

  // 1. Open main zip handle for writing and test subfile creation & errors.
  VSILFILE* fp_zip = VSIFOpenL(kVsiZip, "wb");
  ASSERT_NE(nullptr, fp_zip);
  // Writing directly to main zip handle fails.
  EXPECT_EQ(0U, VSIFWriteL("x", 1, 1, fp_zip));

  // Open first child in zip.
  VSILFILE* fp_c1 =
      VSIFOpenL("/vsizip//vsimem/test_archive.zip/dir/child1.txt", "wb");
  ASSERT_NE(nullptr, fp_c1);

  // Error cases while zip/child is open for writing:
  EXPECT_EQ(nullptr,
            VSIFOpenL("/vsizip//vsimem/test_archive.zip/child2.txt", "wb"));
  EXPECT_EQ(nullptr,
            VSIFOpenL("/vsizip//vsimem/test_archive.zip/dir/child1.txt", "r+"));
  EXPECT_EQ(nullptr,
            VSIFOpenL("/vsizip//vsimem/test_archive.zip/dir/child1.txt", "rb"));
  VSIStatBufL st = {};
  EXPECT_EQ(-1,
            VSIStatL("/vsizip//vsimem/test_archive.zip/dir/child1.txt", &st));
  EXPECT_EQ(nullptr, VSIReadDir(kVsiZip));
  EXPECT_EQ(-1, VSICopyFile("/vsimem/zip_copy_src.txt",
                            "/vsizip//vsimem/test_archive.zip/fail_copy.txt",
                            nullptr, -1, nullptr, nullptr, nullptr));

  // Test VSIZipWriteHandle methods on fp_c1.
  EXPECT_EQ(0U, VSIFTellL(fp_c1));
  EXPECT_EQ(0, VSIFSeekL(fp_c1, 0, SEEK_CUR));
  EXPECT_EQ(0, VSIFSeekL(fp_c1, 0, SEEK_END));
  EXPECT_EQ(0, VSIFSeekL(fp_c1, 0, SEEK_SET));
  EXPECT_EQ(-1, VSIFSeekL(fp_c1, 5, SEEK_SET));
  char buf[32] = {};
  EXPECT_EQ(0U, VSIFReadL(buf, 1, 4, fp_c1));
  EXPECT_EQ(0, VSIFFlushL(fp_c1));
  EXPECT_EQ(0, VSIFEofL(fp_c1));
  EXPECT_EQ(0, VSIFErrorL(fp_c1));
  VSIFClearErrL(fp_c1);

  EXPECT_EQ(11U, VSIFWriteL("hello_zip_1", 1, 11, fp_c1));
  EXPECT_EQ(11U, VSIFTellL(fp_c1));
  EXPECT_EQ(0, VSIFCloseL(fp_c1));

  // Create directory entry inside open zip.
  VSILFILE* fp_dir =
      VSIFOpenL("/vsizip//vsimem/test_archive.zip/subdir/", "wb");
  ASSERT_NE(nullptr, fp_dir);
  EXPECT_EQ(0, VSIFCloseL(fp_dir));

  // VSICopyFile into open zip when no child is currently being written.
  EXPECT_EQ(0, VSICopyFile("/vsimem/zip_copy_src.txt",
                           "/vsizip//vsimem/test_archive.zip/copied1.txt",
                           nullptr, -1, nullptr, nullptr, nullptr));

  // Open a child and close parent zip handle while child is still open.
  VSILFILE* fp_c2 =
      VSIFOpenL("/vsizip//vsimem/test_archive.zip/child2.txt", "wb");
  ASSERT_NE(nullptr, fp_c2);
  EXPECT_EQ(7U, VSIFWriteL("child_2", 1, 7, fp_c2));
  EXPECT_EQ(0, VSIFCloseL(fp_zip));
  EXPECT_EQ(0, VSIFCloseL(fp_c2));

  // 2. Append to existing zip via direct subfile open (bAutoDeleteParent) and
  // via VSICopyFile when zip is not open in oMapZipWriteHandles.
  VSILFILE* fp_direct =
      VSIFOpenL("/vsizip//vsimem/test_archive.zip/direct_append.txt", "wb");
  ASSERT_NE(nullptr, fp_direct);
  EXPECT_EQ(13U, VSIFWriteL("direct_append", 1, 13, fp_direct));
  EXPECT_EQ(0, VSIFCloseL(fp_direct));

  EXPECT_EQ(0, VSICopyFile("/vsimem/zip_copy_src.txt",
                           "/vsizip//vsimem/test_archive.zip/copied2.txt",
                           nullptr, -1, nullptr, nullptr, nullptr));
  // VSICopyFile with missing filename inside zip fails.
  EXPECT_EQ(-1, VSICopyFile("/vsimem/zip_copy_src.txt", kVsiZip, nullptr, -1,
                            nullptr, nullptr, nullptr));

  // 3. Read directory, Stat, and read files from the zip archive.
  char** entries = VSIReadDirRecursive(kVsiZip);
  EXPECT_GE(CSLCount(entries), 5);
  CSLDestroy(entries);

  EXPECT_EQ(0,
            VSIStatL("/vsizip//vsimem/test_archive.zip/dir/child1.txt", &st));
  EXPECT_EQ(11, st.st_size);

  VSILFILE* fp_read =
      VSIFOpenL("/vsizip//vsimem/test_archive.zip/dir/child1.txt", "rb");
  ASSERT_NE(nullptr, fp_read);
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(11U, VSIFReadL(buf, 1, sizeof(buf), fp_read));
  EXPECT_STREQ("hello_zip_1", buf);
  EXPECT_EQ(0, VSIFCloseL(fp_read));

  VSIUnlink("/vsimem/zip_copy_src.txt");
  VSIUnlink(kZipPath);
}

TEST(CplVsiGzipTest, VsiZipStoredTransparentAndSozip) {
  WithQuietHandler quiet;

  const char* kSozipArchive = "/vsimem/sozip_and_stored.zip";
  std::vector<uint8_t> raw_data(100000);
  for (size_t i = 0; i < raw_data.size(); ++i) {
    raw_data[i] = static_cast<uint8_t>((i * 31 + 17) & 0xFF);
  }
  {
    VSILFILE* fp_src = VSIFOpenL("/vsimem/sozip_src.bin", "wb");
    ASSERT_NE(nullptr, fp_src);
    VSIFWriteL(raw_data.data(), 1, raw_data.size(), fp_src);
    VSIFCloseL(fp_src);
  }

  // Create a zip with:
  // 1) "stored.bin" (COMPRESSED=NO -> method 0 -> m_transparent = 1)
  // 2) "sozip_st.bin" (SOZIP_ENABLED=YES, chunk size 16384, single-threaded)
  // 3) "sozip_mt.bin" (SOZIP_ENABLED=YES, chunk size 16384, 2 threads)
  void* hZip = CPLCreateZip(kSozipArchive, nullptr);
  ASSERT_NE(nullptr, hZip);

  char* stored_opts[] = {const_cast<char*>("COMPRESSED=NO"), nullptr};
  ASSERT_EQ(CE_None, CPLCreateFileInZip(hZip, "stored.bin", stored_opts));
  ASSERT_EQ(CE_None, CPLWriteFileInZip(hZip, raw_data.data(),
                                       static_cast<int>(raw_data.size())));
  ASSERT_EQ(CE_None, CPLCloseFileInZip(hZip));

  const char* sozip_st_opts[] = {"SOZIP_ENABLED=YES", "SOZIP_CHUNK_SIZE=16384",
                                 "SOZIP_MIN_FILE_SIZE=100",
                                 "GDAL_NUM_THREADS=1", nullptr};
  ASSERT_EQ(CE_None,
            CPLAddFileInZip(hZip, "sozip_st.bin", "/vsimem/sozip_src.bin",
                            nullptr, sozip_st_opts, nullptr, nullptr));

  const char* sozip_mt_opts[] = {"SOZIP_ENABLED=YES", "SOZIP_CHUNK_SIZE=16384",
                                 "SOZIP_MIN_FILE_SIZE=100",
                                 "GDAL_NUM_THREADS=2", nullptr};
  ASSERT_EQ(CE_None,
            CPLAddFileInZip(hZip, "sozip_mt.bin", "/vsimem/sozip_src.bin",
                            nullptr, sozip_mt_opts, nullptr, nullptr));

  ASSERT_EQ(CE_None, CPLCloseZip(hZip));
  VSIUnlink("/vsimem/sozip_src.bin");

  // 1. Test reading and seeking in "stored.bin" (transparent mode in
  // VSIGZipHandle).
  {
    VSILFILE* fp =
        VSIFOpenL("/vsizip//vsimem/sozip_and_stored.zip/stored.bin", "rb");
    ASSERT_NE(nullptr, fp);
    std::vector<uint8_t> buf(500);
    EXPECT_EQ(0, VSIFSeekL(fp, 50000, SEEK_SET));
    EXPECT_EQ(50000U, VSIFTellL(fp));
    EXPECT_EQ(500U, VSIFReadL(buf.data(), 1, 500, fp));
    EXPECT_EQ(0, memcmp(buf.data(), raw_data.data() + 50000, 500));

    // Seek backwards in transparent mode.
    EXPECT_EQ(0, VSIFSeekL(fp, 1000, SEEK_SET));
    EXPECT_EQ(500U, VSIFReadL(buf.data(), 1, 500, fp));
    EXPECT_EQ(0, memcmp(buf.data(), raw_data.data() + 1000, 500));

    // Seek forward relative and SEEK_END.
    EXPECT_EQ(0, VSIFSeekL(fp, 500, SEEK_CUR));
    EXPECT_EQ(2000U, VSIFTellL(fp));
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_END));
    EXPECT_EQ(raw_data.size(), VSIFTellL(fp));
    EXPECT_EQ(0, VSIFCloseL(fp));
  }

  // 2. Test GetFileMetadata and reading/seeking in SOZIP files
  // (VSISOZipHandle).
  for (const char* subfile :
       {"/vsizip//vsimem/sozip_and_stored.zip/sozip_st.bin",
        "/vsizip//vsimem/sozip_and_stored.zip/sozip_mt.bin"}) {
    char** md_zip = VSIGetFileMetadata(subfile, "ZIP", nullptr);
    ASSERT_NE(nullptr, md_zip);
    EXPECT_STREQ("YES", CSLFetchNameValue(md_zip, "SOZIP_VALID"));
    CSLDestroy(md_zip);

    EXPECT_EQ(nullptr, VSIGetFileMetadata(subfile, "OTHER", nullptr));
    char** md_def = VSIGetFileMetadata(subfile, nullptr, nullptr);
    CSLDestroy(md_def);

    VSILFILE* fp = VSIFOpenL(subfile, "rb");
    ASSERT_NE(nullptr, fp);
    std::vector<uint8_t> buf(40000);
    // Seek to middle chunk and read across chunk boundaries.
    EXPECT_EQ(0, VSIFSeekL(fp, 20000, SEEK_SET));
    EXPECT_EQ(20000U, VSIFTellL(fp));
    EXPECT_EQ(40000U, VSIFReadL(buf.data(), 1, 40000, fp));
    EXPECT_EQ(0, memcmp(buf.data(), raw_data.data() + 20000, 40000));

    // Seek to end and read past EOF.
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_END));
    EXPECT_EQ(raw_data.size(), VSIFTellL(fp));
    uint8_t one = 0;
    EXPECT_EQ(0U, VSIFReadL(&one, 1, 1, fp));
    EXPECT_NE(0, VSIFEofL(fp));
    VSIFClearErrL(fp);
    EXPECT_EQ(0, VSIFErrorL(fp));
    EXPECT_EQ(0, VSIFCloseL(fp));
  }

  VSIUnlink(kSozipArchive);
}

TEST(CplVsiGzipTest, ZLibDeflateAndInflateFunctions) {
  WithQuietHandler quiet;

  // Prepare compressible input (2000 bytes) so 2 * compressed_size < 2000,
  // exercising the VSI_REALLOC_VERBOSE growth loop in CPLZLibInflateEx.
  std::vector<char> input(2000, 'Z');
  for (size_t i = 0; i < input.size(); i += 13) {
    input[i] = static_cast<char>(i & 0xFF);
  }

  // 1. CPLZLibDeflate with invalid compression level.
  size_t comp_size = 0;
  EXPECT_EQ(nullptr, CPLZLibDeflate(input.data(), input.size(), 999, nullptr, 0,
                                    &comp_size));
  char small_buf[4] = {};
  EXPECT_EQ(nullptr, CPLZLibDeflate(input.data(), input.size(), 999, small_buf,
                                    sizeof(small_buf), &comp_size));

  // 2. CPLZLibDeflate with auto-allocated buffer.
  void* comp_auto =
      CPLZLibDeflate(input.data(), input.size(), -1, nullptr, 0, &comp_size);
  const std::unique_ptr<void, decltype(&VSIFree)> free_comp_auto(comp_auto,
                                                                 VSIFree);
  ASSERT_NE(nullptr, comp_auto);
  ASSERT_GT(comp_size, 0U);

  // 4. CPLZLibDeflate with sufficient preallocated buffer.
  std::vector<char> comp_pre(comp_size + 64);
  size_t comp_pre_size = 0;
  EXPECT_EQ(comp_pre.data(),
            CPLZLibDeflate(input.data(), input.size(), 6, comp_pre.data(),
                           comp_pre.size(), &comp_pre_size));
  EXPECT_GT(comp_pre_size, 0U);

  // 5. CPLZLibInflate with auto-allocated buffer (triggers realloc loop).
  size_t decomp_size = 0;
  void* decomp_auto =
      CPLZLibInflate(comp_auto, comp_size, nullptr, 0, &decomp_size);
  ASSERT_NE(nullptr, decomp_auto);
  EXPECT_EQ(input.size(), decomp_size);
  EXPECT_EQ(0, memcmp(decomp_auto, input.data(), input.size()));
  VSIFree(decomp_auto);

  // 6. CPLZLibInflate with preallocated buffer (too small vs sufficient).
  EXPECT_EQ(nullptr, CPLZLibInflate(comp_auto, comp_size, small_buf,
                                    sizeof(small_buf), &decomp_size));

  std::vector<char> decomp_pre(input.size() + 16);
  EXPECT_EQ(decomp_pre.data(),
            CPLZLibInflate(comp_auto, comp_size, decomp_pre.data(),
                           decomp_pre.size(), &decomp_size));
  EXPECT_EQ(input.size(), decomp_size);
  EXPECT_EQ(0, memcmp(decomp_pre.data(), input.data(), input.size()));

  // 7. CPLZLibInflateEx with bAllowResizeOutptr = true on small VSIMalloc
  // buffer.
  void* resizable = VSIMalloc(8);
  ASSERT_NE(nullptr, resizable);
  void* resized =
      CPLZLibInflateEx(comp_auto, comp_size, resizable, 8, true, &decomp_size);
  ASSERT_NE(nullptr, resized);
  EXPECT_EQ(input.size(), decomp_size);
  EXPECT_EQ(0, memcmp(resized, input.data(), input.size()));
  VSIFree(resized);

  // 8. CPLZLibInflateEx on gzip-encapsulated data (0x1f 0x8b header).
  void* gz_inflated =
      CPLZLibInflate(kCountGz, sizeof(kCountGz), nullptr, 0, &decomp_size);
  ASSERT_NE(nullptr, gz_inflated);
  EXPECT_EQ(static_cast<size_t>(kUncompressedSize), decomp_size);
  VSIFree(gz_inflated);

  // 9. CPLZLibInflateEx on corrupt data.
  EXPECT_EQ(nullptr, CPLZLibInflate("corrupt_zlib_stream", 19, nullptr, 0,
                                    &decomp_size));
}

}  // namespace
}  // namespace autotest2
