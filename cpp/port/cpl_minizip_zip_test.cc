// Copyright 2026 Google LLC
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
// Tests for the minizip-based zip writer.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_minizip_zip.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_minizip_zip.cpp

#include "third_party/gdal/port/cpl_minizip_zip.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_minizip_ioapi.h"
#include "third_party/gdal/port/cpl_minizip_unzip.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

void AppendU16(std::vector<uint8_t>& buf, uint16_t val) {
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

void AppendU32(std::vector<uint8_t>& buf, uint32_t val) {
  buf.push_back(static_cast<uint8_t>(val & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

void AppendU64(std::vector<uint8_t>& buf, uint64_t val) {
  AppendU32(buf, static_cast<uint32_t>(val & 0xFFFFFFFFULL));
  AppendU32(buf, static_cast<uint32_t>((val >> 32) & 0xFFFFFFFFULL));
}

// Closes a still-open low-level zipFile handle and removes the archive when a
// test exits early (e.g. on an ASSERT failure). Tests that close the handle
// themselves must reset `zf` to nullptr afterwards.
struct ScopedZipFile {
  explicit ScopedZipFile(const std::string& path) : zip_path(path) {}
  ~ScopedZipFile() {
    if (zf != nullptr) cpl_zipClose(zf, nullptr);
    VSIUnlink(zip_path.c_str());
  }

  const std::string zip_path;
  zipFile zf = nullptr;
};

// Closes a still-open high-level CPL zip handle and removes the archive when
// a test exits early. Tests that close the handle themselves must reset
// `hZip` to nullptr afterwards.
struct ScopedCplZip {
  explicit ScopedCplZip(const std::string& path) : zip_path(path) {}
  ~ScopedCplZip() {
    if (hZip != nullptr) CPLCloseZip(hZip);
    VSIUnlink(zip_path.c_str());
  }

  const std::string zip_path;
  void* hZip = nullptr;
};

// Exercises use_cpl_io = 0 (custom zlib_filefunc_def via
// cpl_fill_fopen_filefunc) and use_cpl_io = 1 (pzlib_filefunc_def == nullptr),
// including >16KB writes that flush Z_BUFSIZE buffers in both deflated and
// stored/raw modes.
TEST(CplMinizipZipTest, LowLevelZipOpenWriteAndBufferFlushes) {
  ScopedZipFile zip("/vsimem/test_lowlevel_zip.zip");
  zlib_filefunc_def filefunc;
  cpl_fill_fopen_filefunc(&filefunc);

  // 1. Open with explicit filefunc (use_cpl_io = 0)
  zip.zf = cpl_zipOpen2(zip.zip_path.c_str(), APPEND_STATUS_CREATE, nullptr,
                        &filefunc);
  ASSERT_NE(zip.zf, nullptr);
  zipFile zf = zip.zf;

  // Parameter validation errors
  EXPECT_EQ(cpl_zipOpenNewFileInZip(nullptr, "a.txt", nullptr, nullptr, 0,
                                    nullptr, 0, nullptr, Z_DEFLATED, 6),
            ZIP_PARAMERROR);
  EXPECT_EQ(cpl_zipOpenNewFileInZip(zf, "a.txt", nullptr, nullptr, 0, nullptr,
                                    0, nullptr, 99, 6),
            ZIP_PARAMERROR);
  EXPECT_EQ(
      cpl_zipOpenNewFileInZip3(zf, "a.txt", nullptr, nullptr, 0, nullptr, 0,
                               nullptr, Z_DEFLATED, 6, 0, 99, DEF_MEM_LEVEL,
                               Z_DEFAULT_STRATEGY, nullptr, 0, false, true),
      Z_STREAM_ERROR);
  EXPECT_EQ(
      cpl_zipOpenNewFileInZip3(zf, "a.txt", nullptr, nullptr, 0, nullptr, 0,
                               nullptr, Z_DEFLATED, 6, 0, -MAX_WBITS, 99,
                               Z_DEFAULT_STRATEGY, nullptr, 0, false, true),
      Z_STREAM_ERROR);
  EXPECT_EQ(
      cpl_zipOpenNewFileInZip3(zf, "a.txt", nullptr, nullptr, 0, nullptr, 0,
                               nullptr, Z_DEFLATED, 6, 0, -MAX_WBITS,
                               DEF_MEM_LEVEL, 99, nullptr, 0, false, true),
      Z_STREAM_ERROR);
  EXPECT_EQ(cpl_zipOpenNewFileInZip3(zf, "a.txt", nullptr, nullptr, 0, nullptr,
                                     0, nullptr, Z_DEFLATED, 6, 0, -MAX_WBITS,
                                     DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY,
                                     "password", 0, false, true),
            ZIP_PARAMERROR);
  EXPECT_EQ(cpl_zipWriteInFileInZip(nullptr, "a", 1), ZIP_PARAMERROR);
  EXPECT_EQ(cpl_zipWriteInFileInZip(zf, "a", 1), ZIP_PARAMERROR);
  EXPECT_EQ(cpl_zipCloseFileInZip(nullptr), ZIP_PARAMERROR);
  EXPECT_EQ(cpl_zipCloseFileInZip(zf), ZIP_PARAMERROR);
  EXPECT_EQ(cpl_zipClose(nullptr, nullptr), ZIP_PARAMERROR);

  // File 1: Deflated (use_cpl_io = 0) with incompressible data > Z_BUFSIZE
  // (16384) so deflate() fills buffered_data and calls zip64FlushWriteBuffer.
  zip_fileinfo zfi1 = {};
  zfi1.tmz_date.tm_year = 2025;  // > 1980 branch
  zfi1.tmz_date.tm_mon = 6;
  zfi1.tmz_date.tm_mday = 10;
  const uint8_t extra_local[] = {0x11, 0x22, 0x02, 0x00, 0x33, 0x44};
  const uint8_t extra_global[] = {0x55, 0x66, 0x02, 0x00, 0x77, 0x88};
  ASSERT_EQ(cpl_zipOpenNewFileInZip2(zf, "deflated_stream.bin", &zfi1,
                                     extra_local, sizeof(extra_local),
                                     extra_global, sizeof(extra_global),
                                     "entry comment", Z_DEFLATED, 1, 0),
            ZIP_OK);
  std::vector<uint8_t> random_like(40000);
  uint32_t seed = 0x12345678;
  for (size_t i = 0; i < random_like.size(); ++i) {
    seed = seed * 1664525U + 1013904223U;
    random_like[i] = static_cast<uint8_t>(seed >> 24);
  }
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, random_like.data(), random_like.size()),
            ZIP_OK);

  // File 2: Open without closing File 1 (triggers auto-close of File 1),
  // stored mode (method = 0) with > Z_BUFSIZE (16384) bytes to trigger
  // zip64FlushWriteBuffer in the raw/stored copy loop.
  zip_fileinfo zfi2 = {};
  zfi2.tmz_date.tm_year = 120;  // > 80 && <= 1980 branch
  zfi2.tmz_date.tm_mon = 1;
  zfi2.tmz_date.tm_mday = 1;
  ASSERT_EQ(cpl_zipOpenNewFileInZip(zf, nullptr, &zfi2, nullptr, 0, nullptr, 0,
                                    nullptr, 0, 0),
            ZIP_OK);
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, random_like.data(), random_like.size()),
            ZIP_OK);
  ASSERT_EQ(cpl_zipCloseFileInZip(zf), ZIP_OK);

  // File 3: Explicit dosDate != 0, level 2, raw=1 with fake 64-bit size in
  // cpl_zipCloseFileInZipRaw (with bZip64=true so it writes 64-bit sizes in
  // local & central directory extra fields).
  zip_fileinfo zfi3 = {};
  zfi3.dosDate = 0x58B03800;
  ASSERT_EQ(cpl_zipOpenNewFileInZip3(
                zf, "raw_zip64.bin", &zfi3, nullptr, 0, nullptr, 0, nullptr,
                Z_DEFLATED, 2, 1, -MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY,
                nullptr, 0, true, true),
            ZIP_OK);
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, "rawbytes", 8), ZIP_OK);
  EXPECT_EQ(cpl_zipCloseFileInZipRaw(zf, 0x100000005ULL, 0xAABBCCDDUL), ZIP_OK);

  // File 4: Raw mode with bZip64=false and uncompressed_size >= 0xffffffff
  // triggers ZIP_BADZIPFILE.
  zip_fileinfo zfi4 = {};
  zfi4.tmz_date.tm_year = 10;  // <= 80 branch
  ASSERT_EQ(cpl_zipOpenNewFileInZip3(
                zf, "raw_overflow.bin", &zfi4, nullptr, 0, nullptr, 0, nullptr,
                Z_DEFLATED, 8, 1, -MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY,
                nullptr, 0, false, true),
            ZIP_OK);
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, "rawbytes", 8), ZIP_OK);
  EXPECT_EQ(cpl_zipCloseFileInZipRaw(zf, 0x100000005ULL, 0x11223344UL),
            ZIP_BADZIPFILE);

  // File 5: Many entries with long comments to exceed SIZEDATA_INDATABLOCK
  // (4080) and allocate multiple linkedlist_datablock_internal blocks, leave
  // last one open when calling cpl_zipClose.
  const std::string long_comment(300, 'M');
  for (int i = 0; i < 20; ++i) {
    const std::string name = CPLSPrintf("entry_%02d.txt", i);
    ASSERT_EQ(
        cpl_zipOpenNewFileInZip(zf, name.c_str(), nullptr, nullptr, 0, nullptr,
                                0, long_comment.c_str(), Z_DEFLATED, 6),
        ZIP_OK);
    ASSERT_EQ(cpl_zipWriteInFileInZip(zf, "hello", 5), ZIP_OK);
    if (i < 19) {
      ASSERT_EQ(cpl_zipCloseFileInZip(zf), ZIP_OK);
    }
  }

  // Global comment > 1024 bytes so subsequent APPEND_STATUS_ADDINZIP searches
  // multiple chunks in zip64local_SearchCentralDir.
  const std::string global_comment(1500, 'G');
  zip.zf = nullptr;
  ASSERT_EQ(cpl_zipClose(zf, global_comment.c_str()), ZIP_OK);

  // 2. Re-open with APPEND_STATUS_ADDINZIP and default CPL IO (pzlib = nullptr)
  // Exercises LoadCentralDirectoryRecord for standard ZIP and preserves
  // globalcomment when cpl_zipClose is called with nullptr.
  zip.zf = cpl_zipOpen(zip.zip_path.c_str(), APPEND_STATUS_ADDINZIP);
  ASSERT_NE(zip.zf, nullptr);
  zf = zip.zf;
  ASSERT_EQ(cpl_zipOpenNewFileInZip(zf, "appended.txt", nullptr, nullptr, 0,
                                    nullptr, 0, nullptr, Z_DEFLATED, 9),
            ZIP_OK);
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, "appended data", 13), ZIP_OK);
  ASSERT_EQ(cpl_zipCloseFileInZip(zf), ZIP_OK);
  zip.zf = nullptr;
  ASSERT_EQ(cpl_zipClose(zf, nullptr), ZIP_OK);

  // 3. Re-open with APPEND_STATUS_CREATEAFTER
  zip.zf = cpl_zipOpen(zip.zip_path.c_str(), APPEND_STATUS_CREATEAFTER);
  ASSERT_NE(zip.zf, nullptr);
  zf = zip.zf;
  ASSERT_EQ(cpl_zipOpenNewFileInZip(zf, "after.txt", nullptr, nullptr, 0,
                                    nullptr, 0, nullptr, 0, 0),
            ZIP_OK);
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, "after", 5), ZIP_OK);
  ASSERT_EQ(cpl_zipCloseFileInZip(zf), ZIP_OK);
  zip.zf = nullptr;
  ASSERT_EQ(cpl_zipClose(zf, "short comment"), ZIP_OK);
}

// Exercises LoadCentralDirectoryRecord on a ZIP64 archive and triggers
// Write_Zip64EndOfCentralDirectoryRecord and
// Write_Zip64EndOfCentralDirectoryLocator in cpl_zipClose by setting
// number_entry = 0x10000 in the initial ZIP64 EOCD.
TEST(CplMinizipZipTest, Zip64AppendAndEocd64Write) {
  std::vector<uint8_t> zip64_bytes;
  // Write a minimal dummy 4-byte payload before central dir so cd_offset > 0
  AppendU32(zip64_bytes, 0x04034b50);
  const uint64_t cd_offset = zip64_bytes.size();
  const uint64_t cd_size = 0;
  const uint64_t eocd64_offset = zip64_bytes.size();

  // Zip64 EOCD record (0x06064b50) with number_entry = 0x10000 (> 0xFFFF)
  AppendU32(zip64_bytes, 0x06064b50);
  AppendU64(zip64_bytes, 44);
  AppendU16(zip64_bytes, 45);
  AppendU16(zip64_bytes, 45);
  AppendU32(zip64_bytes, 0);
  AppendU32(zip64_bytes, 0);
  AppendU64(zip64_bytes, 0x10000ULL);
  AppendU64(zip64_bytes, 0x10000ULL);
  AppendU64(zip64_bytes, cd_size);
  AppendU64(zip64_bytes, cd_offset);

  // Zip64 EOCD locator (0x07064b50)
  AppendU32(zip64_bytes, 0x07064b50);
  AppendU32(zip64_bytes, 0);
  AppendU64(zip64_bytes, eocd64_offset);
  AppendU32(zip64_bytes, 1);

  // Standard EOCD record (0x06054b50) with >1024 byte comment
  AppendU32(zip64_bytes, 0x06054b50);
  AppendU16(zip64_bytes, 0);
  AppendU16(zip64_bytes, 0);
  AppendU16(zip64_bytes, 0xFFFFU);
  AppendU16(zip64_bytes, 0xFFFFU);
  AppendU32(zip64_bytes, 0);
  AppendU32(zip64_bytes, cd_offset);
  AppendU16(zip64_bytes, 1200);
  zip64_bytes.insert(zip64_bytes.end(), 1200, 'Q');

  const std::string zip_path = "/vsimem/test_zip64_append.zip";
  autotest2::VsiMemTempWrapper w(
      zip_path, std::string(reinterpret_cast<const char*>(zip64_bytes.data()),
                            zip64_bytes.size()));

  zipFile zf = cpl_zipOpen(zip_path.c_str(), APPEND_STATUS_ADDINZIP);
  ASSERT_NE(zf, nullptr);
  ASSERT_EQ(cpl_zipOpenNewFileInZip(zf, "in_zip64.txt", nullptr, nullptr, 0,
                                    nullptr, 0, nullptr, Z_DEFLATED, 6),
            ZIP_OK);
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, "zip64 payload", 13), ZIP_OK);
  ASSERT_EQ(cpl_zipCloseFileInZip(zf), ZIP_OK);
  // Because zi->number_entry > 0xFFFF, cpl_zipClose writes Zip64 EOCD and
  // Locator.
  ASSERT_EQ(cpl_zipClose(zf, nullptr), ZIP_OK);

  // Also test APPEND_STATUS_ADDINZIP on a corrupt/non-ZIP file -> returns
  // nullptr.
  const std::string bad_path = "/vsimem/not_a_zip.zip";
  autotest2::VsiMemTempWrapper w_bad(bad_path, "not a zip file");
  EXPECT_EQ(cpl_zipOpen(bad_path.c_str(), APPEND_STATUS_ADDINZIP), nullptr);
}

// Exercises CPLCreateZip, CPLCreateFileInZip, CPLWriteFileInZip,
// CPLCloseFileInZip, CPLAddFileInZip (including SOZip seek-optimized index
// generation, UTF-8 / CP437 filenames, CONTENT_TYPE, TIMESTAMP, progress).
TEST(CplMinizipZipTest, HighLevelCplZipAndSozipApi) {
  // Null / invalid checks
  EXPECT_EQ(CPLCreateFileInZip(nullptr, "a.txt", nullptr), CE_Failure);
  EXPECT_EQ(CPLWriteFileInZip(nullptr, "a", 1), CE_Failure);
  EXPECT_EQ(CPLCloseFileInZip(nullptr), CE_Failure);
  EXPECT_EQ(CPLAddFileInZip(nullptr, "a.txt", nullptr, nullptr, nullptr,
                            nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(CPLCloseZip(nullptr), CE_Failure);

  ScopedCplZip zip("/vsimem/test_highlevel_cpl.zip");
  zip.hZip = CPLCreateZip(zip.zip_path.c_str(), nullptr);
  ASSERT_NE(zip.hZip, nullptr);
  void* hZip = zip.hZip;

  // 1. File with leading slash, CONTENT_TYPE, TIMESTAMP, ZIP64=YES,
  // COMPRESSED=NO
  CPLStringList opts1;
  opts1.SetNameValue("COMPRESSED", "NO");
  opts1.SetNameValue("ZIP64", "YES");
  opts1.SetNameValue("TIMESTAMP", "1700000000");
  opts1.SetNameValue("CONTENT_TYPE", "text/plain");
  opts1.SetNameValue("COMPRESSION_LEVEL", "1");
  ASSERT_EQ(CPLCreateFileInZip(hZip, "/leading_slash.txt", opts1.List()),
            CE_None);
  ASSERT_EQ(CPLWriteFileInZip(hZip, "hello world", 11), CE_None);
  ASSERT_EQ(CPLCloseFileInZip(hZip), CE_None);

  // Duplicate filename fails
  {
    WithQuietHandler suppress;
    EXPECT_EQ(CPLCreateFileInZip(hZip, "/leading_slash.txt", nullptr),
              CE_Failure);
  }

  // 2. Non-ASCII UTF-8 filename representable in CP437 ("é.txt" =
  // "\xC3\xA9.txt"). Exercises 0x7075 Unicode Path extra field generation.
  ASSERT_EQ(CPLCreateFileInZip(hZip, "\xC3\xA9.txt", nullptr), CE_None);
  ASSERT_EQ(CPLWriteFileInZip(hZip, "accented", 8), CE_None);
  ASSERT_EQ(CPLCloseFileInZip(hZip), CE_None);

  // 3. Non-ASCII UTF-8 filename NOT representable in CP437 ("\xE4\xB8\xAD.txt")
  // Exercises GP bit 11 UTF-8 flag path.
  ASSERT_EQ(CPLCreateFileInZip(hZip, "\xE4\xB8\xAD.txt", nullptr), CE_None);
  ASSERT_EQ(CPLWriteFileInZip(hZip, "cjk", 3), CE_None);
  ASSERT_EQ(CPLCloseFileInZip(hZip), CE_None);

  // 4. CPLAddFileInZip error cases
  {
    WithQuietHandler suppress;
    EXPECT_EQ(CPLAddFileInZip(hZip, "none.txt", nullptr, nullptr, nullptr,
                              nullptr, nullptr),
              CE_Failure);
    EXPECT_EQ(CPLAddFileInZip(hZip, "none.txt", "/vsimem/no_such_input.bin",
                              nullptr, nullptr, nullptr, nullptr),
              CE_Failure);
  }

  // 5. CPLAddFileInZip with SOZip enabled (both subdir and root archive name,
  // chunk size with 'K' and default chunk size, min file size with 'K'/'M'/'G',
  // NUM_THREADS=2 and ALL_CPUS)
  const std::string input_path = "/vsimem/sozip_input.bin";
  std::string input_data(70000, 'S');  // > 32KB default SOZip chunk size
  for (size_t i = 0; i < input_data.size(); ++i) {
    input_data[i] = static_cast<char>((i * 31) & 0xFF);
  }
  autotest2::VsiMemTempWrapper w_in(input_path, input_data);

  CPLStringList sozip_opts1;
  sozip_opts1.SetNameValue("SOZIP_ENABLED", "YES");
  sozip_opts1.SetNameValue("SOZIP_CHUNK_SIZE", "16K");
  sozip_opts1.SetNameValue("SOZIP_MIN_FILE_SIZE", "1K");
  sozip_opts1.SetNameValue("NUM_THREADS", "2");
  auto progress_ok = [](double, const char*, void*) -> int { return TRUE; };
  ASSERT_EQ(CPLAddFileInZip(hZip, "subdir/seekable1.bin", input_path.c_str(),
                            nullptr, sozip_opts1.List(), progress_ok, nullptr),
            CE_None);

  // Root-level archive filename (no slash) + AUTO SOZip + default chunk size
  // (32K) + explicit VSILFILE* fpInput
  VSILFILE* fp_in = VSIFOpenL(input_path.c_str(), "rb");
  ASSERT_NE(fp_in, nullptr);
  CPLStringList sozip_opts2;
  sozip_opts2.SetNameValue("SOZIP_ENABLED", "AUTO");
  sozip_opts2.SetNameValue("SOZIP_MIN_FILE_SIZE", "10");
  sozip_opts2.SetNameValue("NUM_THREADS", "ALL_CPUS");
  const CPLErr add_err =
      CPLAddFileInZip(hZip, "seekable2.bin", nullptr, fp_in, sozip_opts2.List(),
                      progress_ok, nullptr);
  VSIFCloseL(fp_in);
  ASSERT_EQ(add_err, CE_None);

  // Test SOZIP_CHUNK_SIZE with 'M' and SOZIP_MIN_FILE_SIZE with 'G' (not seek
  // optimized because file < 1G)
  CPLStringList sozip_opts3;
  sozip_opts3.SetNameValue("SOZIP_ENABLED", "AUTO");
  sozip_opts3.SetNameValue("SOZIP_CHUNK_SIZE", "1M");
  sozip_opts3.SetNameValue("SOZIP_MIN_FILE_SIZE", "1G");
  ASSERT_EQ(CPLAddFileInZip(hZip, "regular_from_add.bin", input_path.c_str(),
                            nullptr, sozip_opts3.List(), nullptr, nullptr),
            CE_None);

  // Test progress callback cancellation in CPLAddFileInZip
  auto progress_cancel = [](double, const char*, void*) -> int {
    return FALSE;
  };
  EXPECT_EQ(CPLAddFileInZip(hZip, "cancelled.bin", input_path.c_str(), nullptr,
                            nullptr, progress_cancel, nullptr),
            CE_Failure);

  zip.hZip = nullptr;
  ASSERT_EQ(CPLCloseZip(hZip), CE_None);

  // Re-open with APPEND=TRUE in CPLCreateZip
  CPLStringList open_opts;
  open_opts.SetNameValue("APPEND", "TRUE");
  zip.hZip = CPLCreateZip(zip.zip_path.c_str(), open_opts.List());
  ASSERT_NE(zip.hZip, nullptr);
  hZip = zip.hZip;
  ASSERT_EQ(CPLCreateFileInZip(hZip, "appended_via_cpl.txt", nullptr), CE_None);
  ASSERT_EQ(CPLWriteFileInZip(hZip, "data", 4), CE_None);
  ASSERT_EQ(CPLCloseFileInZip(hZip), CE_None);
  zip.hZip = nullptr;
  ASSERT_EQ(CPLCloseZip(hZip), CE_None);

  // Verify the archive can be read back with cpl_unzOpen
  unzFile uf = cpl_unzOpen(zip.zip_path.c_str());
  ASSERT_NE(uf, nullptr);
  unz_global_info gi = {};
  const int gi_err = cpl_unzGetGlobalInfo(uf, &gi);
  EXPECT_EQ(cpl_unzClose(uf), UNZ_OK);
  ASSERT_EQ(gi_err, UNZ_OK);
  EXPECT_GE(gi.number_entry, 7U);
}

}  // namespace
}  // namespace autotest2
