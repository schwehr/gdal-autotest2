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
// Tests for minizip unzip support.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_minizip_unzip.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_minizip_unzip.cpp

#include "third_party/gdal/port/cpl_minizip_unzip.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_minizip_ioapi.h"
#include "third_party/gdal/port/cpl_minizip_zip.h"
#include "third_party/gdal/port/cpl_vsi.h"

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

void AppendBytes(std::vector<uint8_t>& buf, const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  buf.insert(buf.end(), p, p + len);
}

TEST(CplMinizipUnzipTest, StringFileNameCompare) {
  // Case sensitive (1)
  EXPECT_EQ(cpl_unzStringFileNameCompare("abc", "abc", 1), 0);
  EXPECT_NE(cpl_unzStringFileNameCompare("abc", "ABC", 1), 0);
  EXPECT_LT(cpl_unzStringFileNameCompare("abc", "abd", 1), 0);
  EXPECT_GT(cpl_unzStringFileNameCompare("abd", "abc", 1), 0);

  // Case insensitive (2) -> exercises strcmpcasenosensitive_internal
  EXPECT_EQ(cpl_unzStringFileNameCompare("aBcDeF", "AbCdEf", 2), 0);
  EXPECT_LT(cpl_unzStringFileNameCompare("abc", "abcd", 2), 0);
  EXPECT_GT(cpl_unzStringFileNameCompare("abcd", "abc", 2), 0);
  EXPECT_LT(cpl_unzStringFileNameCompare("abC", "abD", 2), 0);
  EXPECT_GT(cpl_unzStringFileNameCompare("abD", "abC", 2), 0);

  // Default OS sensitivity (0)
  EXPECT_EQ(cpl_unzStringFileNameCompare("same.txt", "same.txt", 0), 0);
}

TEST(CplMinizipUnzipTest, StandardZipReadAndNavigation) {
  const std::string zip_path = "/vsimem/test_std_unzip.zip";
  zlib_filefunc_def filefunc;
  cpl_fill_fopen_filefunc(&filefunc);

  zipFile zf =
      cpl_zipOpen2(zip_path.c_str(), APPEND_STATUS_CREATE, nullptr, &filefunc);
  ASSERT_NE(zf, nullptr);
  // Make sure an early ASSERT failure does not leak the handles or the vsimem
  // file into other tests in this binary.
  unzFile uf = nullptr;
  absl::Cleanup cleanup = [&zf, &uf, &zip_path] {
    if (zf != nullptr) cpl_zipClose(zf, nullptr);
    if (uf != nullptr) cpl_unzClose(uf);
    VSIUnlink(zip_path.c_str());
  };

  // File 1: stored (method 0) with local extra field
  zip_fileinfo zfi = {};
  zfi.tmz_date.tm_sec = 10;
  zfi.tmz_date.tm_min = 20;
  zfi.tmz_date.tm_hour = 14;
  zfi.tmz_date.tm_mday = 15;
  zfi.tmz_date.tm_mon = 5;
  zfi.tmz_date.tm_year = 2024;
  const uint8_t local_extra[] = {0x42, 0x42, 0x02, 0x00, 0xAA, 0xBB};
  ASSERT_EQ(cpl_zipOpenNewFileInZip(zf, "stored.txt", &zfi, local_extra,
                                    sizeof(local_extra), nullptr, 0,
                                    "file comment", 0, 0),
            ZIP_OK);
  const std::string content1 = "Stored file content 12345";
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, content1.data(), content1.size()),
            ZIP_OK);
  ASSERT_EQ(cpl_zipCloseFileInZip(zf), ZIP_OK);

  // File 2: deflated level 1 (fast -> flag bits 6)
  ASSERT_EQ(cpl_zipOpenNewFileInZip(zf, "deflate1.txt", &zfi, nullptr, 0,
                                    nullptr, 0, nullptr, Z_DEFLATED, 1),
            ZIP_OK);
  const std::string content2(20000, 'A');  // > UNZ_BUFSIZE (16384)
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, content2.data(), content2.size()),
            ZIP_OK);
  ASSERT_EQ(cpl_zipCloseFileInZip(zf), ZIP_OK);

  // File 3: deflated level 2 (flag bits 4)
  ASSERT_EQ(cpl_zipOpenNewFileInZip(zf, "deflate2.txt", &zfi, nullptr, 0,
                                    nullptr, 0, nullptr, Z_DEFLATED, 2),
            ZIP_OK);
  const std::string content3 = "Deflate level 2 data";
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, content3.data(), content3.size()),
            ZIP_OK);
  ASSERT_EQ(cpl_zipCloseFileInZip(zf), ZIP_OK);

  // File 4: deflated level 9 (best -> flag bits 2)
  ASSERT_EQ(cpl_zipOpenNewFileInZip(zf, "deflate9.txt", &zfi, nullptr, 0,
                                    nullptr, 0, nullptr, Z_DEFLATED, 9),
            ZIP_OK);
  const std::string content4 = "Deflate level 9 data";
  ASSERT_EQ(cpl_zipWriteInFileInZip(zf, content4.data(), content4.size()),
            ZIP_OK);
  ASSERT_EQ(cpl_zipCloseFileInZip(zf), ZIP_OK);

  // Global comment > 1024 bytes (BUFREADCOMMENT) to exercise multi-chunk search
  const std::string global_comment(1500, 'C');
  // cpl_zipClose frees the handle even on failure.
  const int zip_close_result = cpl_zipClose(zf, global_comment.c_str());
  zf = nullptr;
  ASSERT_EQ(zip_close_result, ZIP_OK);

  // Open with cpl_unzOpen2 and explicit filefunc
  uf = cpl_unzOpen2(zip_path.c_str(), &filefunc);
  ASSERT_NE(uf, nullptr);

  // Global info & comment
  unz_global_info gi = {};
  EXPECT_EQ(cpl_unzGetGlobalInfo(nullptr, &gi), UNZ_PARAMERROR);
  ASSERT_EQ(cpl_unzGetGlobalInfo(uf, &gi), UNZ_OK);
  EXPECT_EQ(gi.number_entry, 4U);
  EXPECT_EQ(gi.size_comment, global_comment.size());

  EXPECT_EQ(cpl_unzGetGlobalComment(nullptr, nullptr, 0), UNZ_PARAMERROR);
  char small_comment[16] = {};
  EXPECT_EQ(cpl_unzGetGlobalComment(uf, small_comment, sizeof(small_comment)),
            static_cast<int>(sizeof(small_comment)));
  std::vector<char> full_comment(global_comment.size() + 10, 0);
  EXPECT_EQ(
      cpl_unzGetGlobalComment(uf, full_comment.data(), full_comment.size()),
      static_cast<int>(global_comment.size()));
  EXPECT_STREQ(full_comment.data(), global_comment.c_str());

  // File 1 info & reading
  unz_file_info fi = {};
  char filename[256] = {};
  EXPECT_EQ(cpl_unzGetCurrentFileInfo(nullptr, &fi, filename, sizeof(filename),
                                      nullptr, 0, nullptr, 0),
            UNZ_PARAMERROR);
  ASSERT_EQ(cpl_unzGetCurrentFileInfo(uf, &fi, filename, sizeof(filename),
                                      nullptr, 0, nullptr, 0),
            UNZ_OK);
  EXPECT_STREQ(filename, "stored.txt");
  EXPECT_EQ(fi.compression_method, 0U);
  EXPECT_EQ(fi.uncompressed_size, content1.size());

  // Truncated filename buffer in cpl_unzGetCurrentFileInfo
  char tiny_name[4] = {};
  ASSERT_EQ(cpl_unzGetCurrentFileInfo(uf, &fi, tiny_name, sizeof(tiny_name),
                                      nullptr, 0, nullptr, 0),
            UNZ_OK);

  // Local header helpers
  uLong64 local_hdr_pos = 0;
  EXPECT_EQ(cpl_unzGetLocalHeaderPos(nullptr, &local_hdr_pos), UNZ_PARAMERROR);
  ASSERT_EQ(cpl_unzGetLocalHeaderPos(uf, &local_hdr_pos), UNZ_OK);
  unz_file_info local_fi = {};
  uLong64 pos_data = 0;
  char local_fname[64] = {};
  EXPECT_EQ(cpl_unzCurrentFileInfoFromLocalHeader(
                nullptr, local_hdr_pos, &local_fi, local_fname,
                sizeof(local_fname), &pos_data),
            UNZ_PARAMERROR);
  ASSERT_EQ(cpl_unzCurrentFileInfoFromLocalHeader(
                uf, local_hdr_pos, &local_fi, local_fname, sizeof(local_fname),
                &pos_data),
            UNZ_OK);
  EXPECT_EQ(local_fi.uncompressed_size, content1.size());
  EXPECT_GT(pos_data, local_hdr_pos);

  // Password open fails because NOUNCRYPT is compiled
  EXPECT_EQ(cpl_unzOpenCurrentFilePassword(uf, "secret"), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzOpenCurrentFile(nullptr), UNZ_PARAMERROR);

  // Before opening current file, stream functions return error
  EXPECT_EQ(cpl_unzGetCurrentFileZStreamPos(nullptr), 0U);
  EXPECT_EQ(cpl_unzGetCurrentFileZStreamPos(uf), 0U);
  EXPECT_EQ(cpl_unztell(nullptr), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unztell(uf), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzeof(nullptr), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzeof(uf), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzGetLocalExtrafield(nullptr, nullptr, 0), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzGetLocalExtrafield(uf, nullptr, 0), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzReadCurrentFile(nullptr, filename, 1), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzReadCurrentFile(uf, filename, 1), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzCloseCurrentFile(nullptr), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzCloseCurrentFile(uf), UNZ_PARAMERROR);

  // Open current file (stored)
  ASSERT_EQ(cpl_unzOpenCurrentFile(uf), UNZ_OK);
  EXPECT_GT(cpl_unzGetCurrentFileZStreamPos(uf), 0U);
  EXPECT_EQ(cpl_unztell(uf), 0);
  EXPECT_EQ(cpl_unzeof(uf), 0);

  // Local extra field (includes 6-byte local_extra plus 20-byte Zip64 field)
  EXPECT_GE(cpl_unzGetLocalExtrafield(uf, nullptr, 0),
            static_cast<int>(sizeof(local_extra)));
  uint8_t extra_buf[16] = {};
  EXPECT_EQ(cpl_unzGetLocalExtrafield(uf, extra_buf, 0), 0);
  EXPECT_EQ(cpl_unzGetLocalExtrafield(uf, extra_buf, 3), 3);
  EXPECT_EQ(cpl_unzGetLocalExtrafield(uf, extra_buf, sizeof(local_extra)),
            static_cast<int>(sizeof(local_extra)));
  EXPECT_EQ(memcmp(extra_buf, local_extra, sizeof(local_extra)), 0);

  // Read len=0, then partial, then rest
  char read_buf[64] = {};
  EXPECT_EQ(cpl_unzReadCurrentFile(uf, read_buf, 0), 0);
  EXPECT_EQ(cpl_unzReadCurrentFile(uf, read_buf, 6), 6);
  EXPECT_EQ(cpl_unztell(uf), 6);
  EXPECT_EQ(cpl_unzReadCurrentFile(uf, read_buf + 6, sizeof(read_buf) - 6),
            static_cast<int>(content1.size() - 6));
  EXPECT_EQ(std::string(read_buf, content1.size()), content1);
  EXPECT_EQ(cpl_unzeof(uf), 1);
  EXPECT_EQ(cpl_unzReadCurrentFile(uf, read_buf, sizeof(read_buf)), UNZ_EOF);
  ASSERT_EQ(cpl_unzCloseCurrentFile(uf), UNZ_OK);

  // Save file 1 pos
  unz_file_pos pos1 = {};
  EXPECT_EQ(cpl_unzGetFilePos(nullptr, &pos1), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzGetFilePos(uf, nullptr), UNZ_PARAMERROR);
  ASSERT_EQ(cpl_unzGetFilePos(uf, &pos1), UNZ_OK);
  uLong64 offset1 = cpl_unzGetOffset(uf);
  EXPECT_GT(offset1, 0U);
  EXPECT_EQ(cpl_unzGetOffset(nullptr), 0U);

  // Move to File 2 (deflated level 1, > 16KB)
  EXPECT_EQ(cpl_unzGoToNextFile(nullptr), UNZ_PARAMERROR);
  ASSERT_EQ(cpl_unzGoToNextFile(uf), UNZ_OK);
  int method = 0;
  int level = 0;
  ASSERT_EQ(cpl_unzOpenCurrentFile2(uf, &method, &level, 0), UNZ_OK);
  EXPECT_EQ(method, Z_DEFLATED);
  EXPECT_EQ(level, 1);
  // Re-opening current file without closing first triggers auto-close
  ASSERT_EQ(cpl_unzOpenCurrentFile2(uf, &method, &level, 0), UNZ_OK);
  std::vector<char> big_read(content2.size() + 100);
  EXPECT_EQ(cpl_unzReadCurrentFile(uf, big_read.data(), big_read.size()),
            static_cast<int>(content2.size()));
  EXPECT_EQ(std::string(big_read.data(), content2.size()), content2);
  EXPECT_EQ(cpl_unzReadCurrentFile(uf, big_read.data(), 10), 0);
  ASSERT_EQ(cpl_unzCloseCurrentFile(uf), UNZ_OK);

  // Move to File 3 (deflated level 2), read in raw mode
  ASSERT_EQ(cpl_unzGoToNextFile(uf), UNZ_OK);
  ASSERT_EQ(cpl_unzOpenCurrentFile2(uf, &method, &level, 1), UNZ_OK);
  EXPECT_EQ(method, Z_DEFLATED);
  EXPECT_EQ(level, 2);
  ASSERT_EQ(
      cpl_unzGetCurrentFileInfo(uf, &fi, nullptr, 0, nullptr, 0, nullptr, 0),
      UNZ_OK);
  std::vector<char> raw_buf(fi.compressed_size + 10);
  EXPECT_EQ(cpl_unzReadCurrentFile(uf, raw_buf.data(), 2), 2);
  EXPECT_EQ(cpl_unzReadCurrentFile(uf, raw_buf.data() + 2, raw_buf.size() - 2),
            static_cast<int>(fi.compressed_size - 2));
  EXPECT_EQ(cpl_unzReadCurrentFile(uf, raw_buf.data(), 10), UNZ_EOF);
  ASSERT_EQ(cpl_unzCloseCurrentFile(uf), UNZ_OK);

  // Move to File 4 (deflated level 9)
  ASSERT_EQ(cpl_unzGoToNextFile(uf), UNZ_OK);
  ASSERT_EQ(cpl_unzOpenCurrentFile2(uf, &method, &level, 0), UNZ_OK);
  EXPECT_EQ(method, Z_DEFLATED);
  EXPECT_EQ(level, 9);
  // End of list
  EXPECT_EQ(cpl_unzGoToNextFile(uf), UNZ_END_OF_LIST_OF_FILE);

  // LocateFile
  EXPECT_EQ(cpl_unzLocateFile(nullptr, "stored.txt", 1), UNZ_PARAMERROR);
  const std::string too_long_name(300, 'x');
  EXPECT_EQ(cpl_unzLocateFile(uf, too_long_name.c_str(), 1), UNZ_PARAMERROR);
  ASSERT_EQ(cpl_unzLocateFile(uf, "STORED.TXT", 2), UNZ_OK);
  // Missing file restores previous position ("stored.txt")
  EXPECT_EQ(cpl_unzLocateFile(uf, "nonexistent.txt", 1),
            UNZ_END_OF_LIST_OF_FILE);
  ASSERT_EQ(cpl_unzGetCurrentFileInfo(uf, &fi, filename, sizeof(filename),
                                      nullptr, 0, nullptr, 0),
            UNZ_OK);
  EXPECT_STREQ(filename, "stored.txt");

  // GoToFilePos and SetOffset
  EXPECT_EQ(cpl_unzGoToFilePos(nullptr, &pos1), UNZ_PARAMERROR);
  EXPECT_EQ(cpl_unzGoToFilePos(uf, nullptr), UNZ_PARAMERROR);
  ASSERT_EQ(cpl_unzGoToFilePos(uf, &pos1), UNZ_OK);

  EXPECT_EQ(cpl_unzSetOffset(nullptr, offset1), UNZ_PARAMERROR);
  ASSERT_EQ(cpl_unzSetOffset(uf, offset1), UNZ_OK);
  // After SetOffset, num_file == gi.number_entry, so GetOffset returns 0
  EXPECT_EQ(cpl_unzGetOffset(uf), 0U);

  // Leave a file open so that cpl_unzClose has to close the current file.
  ASSERT_EQ(cpl_unzOpenCurrentFile(uf), UNZ_OK);
  EXPECT_EQ(cpl_unzClose(nullptr), UNZ_PARAMERROR);
  // cpl_unzClose frees the handle; the cleanup only unlinks the file.
  const int unz_close_result = cpl_unzClose(uf);
  uf = nullptr;
  EXPECT_EQ(unz_close_result, UNZ_OK);
}

TEST(CplMinizipUnzipTest, Zip64AndExtraFieldsAndUnicodePath) {
  // Craft a valid ZIP64 archive in memory containing files with:
  // 1) 0x0001 ZIP64 extra field (64-bit uncompressed, compressed, offset,
  //    and 32-bit disk_num_start) + truncated filename read (lSeek != 0)
  // 2) 0x7075 Unicode Path extra field (version 2 -> ignored, version 1 with
  //    bad CRC -> ignored, version 1 with valid CRC -> used, plus small buffer)
  // 3) Unknown extra field (0x9999) + non-UTF8 CP437 filename (flag bit 11 = 0)
  std::vector<uint8_t> zip_bytes;

  const std::string data = "Hello";
  const uint32_t data_crc =
      crc32(0, reinterpret_cast<const Bytef*>(data.data()), data.size());

  // Local file header 1 at offset 0
  const std::string fname1 = "file1_long_name.txt";
  AppendU32(zip_bytes, 0x04034b50);  // local magic
  AppendU16(zip_bytes, 45);          // version needed
  AppendU16(zip_bytes, 1 << 11);     // flag (UTF-8)
  AppendU16(zip_bytes, 0);           // method (store)
  AppendU32(zip_bytes, 0);           // dosDate (month=0 edge case)
  AppendU32(zip_bytes, data_crc);
  AppendU32(zip_bytes, data.size());
  AppendU32(zip_bytes, data.size());
  AppendU16(zip_bytes, fname1.size());
  AppendU16(zip_bytes, 0);  // local extra len
  AppendBytes(zip_bytes, fname1.data(), fname1.size());
  AppendBytes(zip_bytes, data.data(), data.size());

  // Local file header 2
  const uint64_t offset2 = zip_bytes.size();
  const std::string fname2 = "ascii.txt";
  AppendU32(zip_bytes, 0x04034b50);
  AppendU16(zip_bytes, 20);
  AppendU16(zip_bytes, 0);  // flag bit 11 unset
  AppendU16(zip_bytes, 0);
  AppendU32(zip_bytes, 0x58B03800);  // non-zero month dosDate
  AppendU32(zip_bytes, data_crc);
  AppendU32(zip_bytes, data.size());
  AppendU32(zip_bytes, data.size());
  AppendU16(zip_bytes, fname2.size());
  AppendU16(zip_bytes, 0);
  AppendBytes(zip_bytes, fname2.data(), fname2.size());
  AppendBytes(zip_bytes, data.data(), data.size());

  // Local file header 3 (CP437 filename without 0x7075, e.g. 0x82 = 'é')
  const uint64_t offset3 = zip_bytes.size();
  const std::string fname3 = "\x82.txt";
  AppendU32(zip_bytes, 0x04034b50);
  AppendU16(zip_bytes, 20);
  AppendU16(zip_bytes, 0);  // flag bit 11 unset -> triggers CPLRecode
  AppendU16(zip_bytes, 0);
  AppendU32(zip_bytes, 0x58B03800);
  AppendU32(zip_bytes, data_crc);
  AppendU32(zip_bytes, data.size());
  AppendU32(zip_bytes, data.size());
  AppendU16(zip_bytes, fname3.size());
  AppendU16(zip_bytes, 0);
  AppendBytes(zip_bytes, fname3.data(), fname3.size());
  AppendBytes(zip_bytes, data.data(), data.size());

  // Central Directory
  const uint64_t cd_offset = zip_bytes.size();

  // CD Entry 1: 0x0001 ZIP64 extra field with all 4 fields maxed in 32/16-bit
  std::vector<uint8_t> extra1;
  AppendU16(extra1, 0x0001);
  AppendU16(extra1, 8 + 8 + 8 + 4);
  AppendU64(extra1, data.size());  // uncompressed_size
  AppendU64(extra1, data.size());  // compressed_size
  AppendU64(extra1, 0);            // offset_curfile
  AppendU32(extra1, 0);            // disk_num_start

  AppendU32(zip_bytes, 0x02014b50);
  AppendU16(zip_bytes, 45);
  AppendU16(zip_bytes, 45);
  AppendU16(zip_bytes, 1 << 11);
  AppendU16(zip_bytes, 0);
  AppendU32(zip_bytes, 0);
  AppendU32(zip_bytes, data_crc);
  AppendU32(zip_bytes, 0xFFFFFFFFU);  // triggers 64-bit compressed_size read
  AppendU32(zip_bytes, 0xFFFFFFFFU);  // triggers 64-bit uncompressed_size read
  AppendU16(zip_bytes, fname1.size());
  AppendU16(zip_bytes, extra1.size());
  AppendU16(zip_bytes, 0);
  AppendU16(zip_bytes, 0xFFFFU);  // triggers 32-bit disk_num_start read
  AppendU16(zip_bytes, 0);
  AppendU32(zip_bytes, 0);
  AppendU32(zip_bytes, 0xFFFFFFFFU);  // triggers 64-bit offset_curfile read
  AppendBytes(zip_bytes, fname1.data(), fname1.size());
  AppendBytes(zip_bytes, extra1.data(), extra1.size());

  // CD Entry 2: 0x7075 extra fields (v2, v1 bad CRC, v1 good CRC)
  const uint32_t fname2_crc =
      crc32(0, reinterpret_cast<const Bytef*>(fname2.data()), fname2.size());
  const std::string utf8_name2 = "unicode_name_long.txt";
  std::vector<uint8_t> extra2;
  // 0x7075 with version 2 (ignored)
  AppendU16(extra2, 0x7075);
  AppendU16(extra2, 6);
  extra2.push_back(2);
  AppendU32(extra2, fname2_crc);
  extra2.push_back('x');
  // 0x7075 with version 1 and mismatched CRC (ignored)
  AppendU16(extra2, 0x7075);
  AppendU16(extra2, 6);
  extra2.push_back(1);
  AppendU32(extra2, fname2_crc ^ 0xFFFFFFFFU);
  extra2.push_back('y');
  // 0x7075 with version 1 and matching CRC
  AppendU16(extra2, 0x7075);
  AppendU16(extra2, 1 + 4 + utf8_name2.size());
  extra2.push_back(1);
  AppendU32(extra2, fname2_crc);
  AppendBytes(extra2, utf8_name2.data(), utf8_name2.size());

  AppendU32(zip_bytes, 0x02014b50);
  AppendU16(zip_bytes, 20);
  AppendU16(zip_bytes, 20);
  AppendU16(zip_bytes, 0);
  AppendU16(zip_bytes, 0);
  AppendU32(zip_bytes, 0x58B03800);
  AppendU32(zip_bytes, data_crc);
  AppendU32(zip_bytes, data.size());
  AppendU32(zip_bytes, data.size());
  AppendU16(zip_bytes, fname2.size());
  AppendU16(zip_bytes, extra2.size());
  AppendU16(zip_bytes, 0);
  AppendU16(zip_bytes, 0);
  AppendU16(zip_bytes, 0);
  AppendU32(zip_bytes, 0);
  AppendU32(zip_bytes, offset2);
  AppendBytes(zip_bytes, fname2.data(), fname2.size());
  AppendBytes(zip_bytes, extra2.data(), extra2.size());

  // CD Entry 3: Unknown extra field 0x9999 + CP437 filename
  std::vector<uint8_t> extra3;
  AppendU16(extra3, 0x9999);
  AppendU16(extra3, 4);
  AppendU32(extra3, 0x12345678);

  AppendU32(zip_bytes, 0x02014b50);
  AppendU16(zip_bytes, 20);
  AppendU16(zip_bytes, 20);
  AppendU16(zip_bytes, 0);
  AppendU16(zip_bytes, 0);
  AppendU32(zip_bytes, 0x58B03800);
  AppendU32(zip_bytes, data_crc);
  AppendU32(zip_bytes, data.size());
  AppendU32(zip_bytes, data.size());
  AppendU16(zip_bytes, fname3.size());
  AppendU16(zip_bytes, extra3.size());
  AppendU16(zip_bytes, 0);
  AppendU16(zip_bytes, 0);
  AppendU16(zip_bytes, 0);
  AppendU32(zip_bytes, 0);
  AppendU32(zip_bytes, offset3);
  AppendBytes(zip_bytes, fname3.data(), fname3.size());
  AppendBytes(zip_bytes, extra3.data(), extra3.size());

  const uint64_t cd_size = zip_bytes.size() - cd_offset;
  const uint64_t eocd64_offset = zip_bytes.size();

  // Zip64 End of Central Directory Record (0x06064b50)
  AppendU32(zip_bytes, 0x06064b50);
  AppendU64(zip_bytes, 44);  // size of record
  AppendU16(zip_bytes, 45);
  AppendU16(zip_bytes, 45);
  AppendU32(zip_bytes, 0);  // number_disk
  AppendU32(zip_bytes, 0);  // number_disk_with_CD
  AppendU64(zip_bytes, 3);  // number_entry on disk
  AppendU64(zip_bytes, 3);  // number_entry_CD
  AppendU64(zip_bytes, cd_size);
  AppendU64(zip_bytes, cd_offset);

  // Zip64 End of Central Directory Locator (0x07064b50)
  AppendU32(zip_bytes, 0x07064b50);
  AppendU32(zip_bytes, 0);  // disk with zip64 EOCD
  AppendU64(zip_bytes, eocd64_offset);
  AppendU32(zip_bytes, 1);  // total disks

  // End of Central Directory Record (0x06054b50) + >1024 byte comment
  AppendU32(zip_bytes, 0x06054b50);
  AppendU16(zip_bytes, 0);
  AppendU16(zip_bytes, 0);
  AppendU16(zip_bytes, 3);
  AppendU16(zip_bytes, 3);
  AppendU32(zip_bytes, cd_size);
  AppendU32(zip_bytes, 0xFFFFFFFFU);
  AppendU16(zip_bytes, 1200);
  zip_bytes.insert(zip_bytes.end(), 1200, 'Z');

  const std::string zip_path = "/vsimem/test_zip64_extra.zip";
  autotest2::VsiMemTempWrapper wrapper(
      zip_path, std::string(reinterpret_cast<const char*>(zip_bytes.data()),
                            zip_bytes.size()));

  unzFile uf = cpl_unzOpen(zip_path.c_str());
  ASSERT_NE(uf, nullptr);

  // Entry 1: test truncated filename buffer (so lSeek > 0 before extra field!)
  unz_file_info fi = {};
  char small_buf[5] = {};
  ASSERT_EQ(cpl_unzGetCurrentFileInfo(uf, &fi, small_buf, sizeof(small_buf),
                                      nullptr, 0, nullptr, 0),
            UNZ_OK);
  EXPECT_EQ(fi.uncompressed_size, data.size());
  EXPECT_EQ(fi.compressed_size, data.size());
  EXPECT_EQ(fi.disk_num_start, 0U);

  // Entry 2: 0x7075 Unicode Path with large buffer and with smaller buffer
  // (where file_info.size_filename <= fileNameBufferSize < utf8Size)
  ASSERT_EQ(cpl_unzGoToNextFile(uf), UNZ_OK);
  char full_buf[64] = {};
  ASSERT_EQ(cpl_unzGetCurrentFileInfo(uf, &fi, full_buf, sizeof(full_buf),
                                      nullptr, 0, nullptr, 0),
            UNZ_OK);
  EXPECT_STREQ(full_buf, utf8_name2.c_str());

  char mid_buf[12] = {};  // >= fname2.size() (9), < utf8_name2.size() (21)
  ASSERT_EQ(cpl_unzGetCurrentFileInfo(uf, &fi, mid_buf, sizeof(mid_buf),
                                      nullptr, 0, nullptr, 0),
            UNZ_OK);

  // Entry 3: Unknown extra field + CP437 recoding to UTF-8 ("é.txt")
  ASSERT_EQ(cpl_unzGoToNextFile(uf), UNZ_OK);
  ASSERT_EQ(cpl_unzGetCurrentFileInfo(uf, &fi, full_buf, sizeof(full_buf),
                                      nullptr, 0, nullptr, 0),
            UNZ_OK);
  EXPECT_STREQ(full_buf, "\xC3\xA9.txt");

  ASSERT_EQ(cpl_unzClose(uf), UNZ_OK);
}

TEST(CplMinizipUnzipTest, CorruptAndErrorConditions) {
  // 1. Non-existent file
  EXPECT_EQ(cpl_unzOpen("/vsimem/does_not_exist_at_all.zip"), nullptr);

  // 2. File with no EOCD signature
  {
    const std::string bad_zip = "/vsimem/no_eocd.zip";
    autotest2::VsiMemTempWrapper w(bad_zip, std::string(100, 'X'));
    EXPECT_EQ(cpl_unzOpen(bad_zip.c_str()), nullptr);
  }

  // 3. Standard EOCD with number_disk != 0 and central_pos < offset + size
  {
    std::vector<uint8_t> bytes;
    AppendU32(bytes, 0x06054b50);
    AppendU16(bytes, 1);  // number_disk != 0 -> UNZ_BADZIPFILE
    AppendU16(bytes, 0);
    AppendU16(bytes, 0);
    AppendU16(bytes, 0);
    AppendU32(bytes, 100);  // size_central_dir > central_pos
    AppendU32(bytes, 100);  // offset_central_dir
    AppendU16(bytes, 0);
    const std::string bad_zip = "/vsimem/bad_eocd.zip";
    autotest2::VsiMemTempWrapper w(
        bad_zip,
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    EXPECT_EQ(cpl_unzOpen(bad_zip.c_str()), nullptr);
  }

  // 4. ZIP64 Locator edge cases (disk != 0, total_disks == 2, bad EOCD64 sig,
  //    and EOCD64 number_disk != 0)
  {
    std::vector<uint8_t> bytes;
    AppendU32(bytes, 0x06064b50);
    AppendU64(bytes, 44);
    AppendU16(bytes, 45);
    AppendU16(bytes, 45);
    AppendU32(bytes, 1);  // number_disk = 1 -> UNZ_BADZIPFILE
    AppendU32(bytes, 0);
    AppendU64(bytes, 0);
    AppendU64(bytes, 0);
    AppendU64(bytes, 0);
    AppendU64(bytes, 0);

    AppendU32(bytes, 0x07064b50);
    AppendU32(bytes, 0);
    AppendU64(bytes, 0);  // offset of EOCD64
    AppendU32(bytes, 1);

    const std::string bad_zip64 = "/vsimem/bad_eocd64.zip";
    autotest2::VsiMemTempWrapper w(
        bad_zip64,
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    EXPECT_EQ(cpl_unzOpen(bad_zip64.c_str()), nullptr);
  }

  // 5. Corrupt local file headers & CRC mismatch & corrupt deflate stream
  {
    std::vector<uint8_t> bytes;
    const std::string fname = "a.txt";
    const std::string payload = "hello";
    const uint32_t good_crc = crc32(
        0, reinterpret_cast<const Bytef*>(payload.data()), payload.size());

    // Local header 0: unsupported compression method (99)
    const uint64_t off0 = bytes.size();
    AppendU32(bytes, 0x04034b50);
    AppendU16(bytes, 20);
    AppendU16(bytes, 0);
    AppendU16(bytes, 99);  // unsupported method
    AppendU32(bytes, 0);
    AppendU32(bytes, good_crc);
    AppendU32(bytes, payload.size());
    AppendU32(bytes, payload.size());
    AppendU16(bytes, fname.size());
    AppendU16(bytes, 0);
    AppendBytes(bytes, fname.data(), fname.size());
    AppendBytes(bytes, payload.data(), payload.size());

    // Local header 1: stored file with corrupted byte -> UNZ_CRCERROR on close
    const uint64_t off1 = bytes.size();
    AppendU32(bytes, 0x04034b50);
    AppendU16(bytes, 20);
    AppendU16(bytes, 0);
    AppendU16(bytes, 0);
    AppendU32(bytes, 0);
    AppendU32(bytes, good_crc);
    AppendU32(bytes, payload.size());
    AppendU32(bytes, payload.size());
    AppendU16(bytes, fname.size());
    AppendU16(bytes, 0);
    AppendBytes(bytes, fname.data(), fname.size());
    AppendBytes(bytes, "hellX", 5);  // bad payload for CRC error

    // Local header 2: deflated file with invalid deflate bitstream ->
    // Z_DATA_ERROR
    const uint64_t off2 = bytes.size();
    AppendU32(bytes, 0x04034b50);
    AppendU16(bytes, 20);
    AppendU16(bytes, 0);
    AppendU16(bytes, Z_DEFLATED);
    AppendU32(bytes, 0);
    AppendU32(bytes, good_crc);
    AppendU32(bytes, payload.size());
    AppendU32(bytes, payload.size());
    AppendU16(bytes, fname.size());
    AppendU16(bytes, 0);
    AppendBytes(bytes, fname.data(), fname.size());
    const uint8_t bad_deflate[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    AppendBytes(bytes, bad_deflate, sizeof(bad_deflate));

    // Local header 3: bad local magic + mismatched fields
    const uint64_t off3 = bytes.size();
    AppendU32(bytes, 0xDEADBEEF);  // bad local magic
    AppendU16(bytes, 20);
    AppendU16(bytes, 0);
    AppendU16(bytes, 0);  // local method 0 != CD method 8
    AppendU32(bytes, 0);
    AppendU32(bytes, good_crc ^ 1);        // mismatched CRC
    AppendU32(bytes, payload.size() + 1);  // mismatched comp size
    AppendU32(bytes, payload.size() + 1);  // mismatched uncomp size
    AppendU16(bytes, fname.size() + 1);    // mismatched fname size
    AppendU16(bytes, 0);

    const uint64_t cd_off = bytes.size();
    auto add_cd = [&](uint64_t local_off, uint16_t method) {
      AppendU32(bytes, 0x02014b50);
      AppendU16(bytes, 20);
      AppendU16(bytes, 20);
      AppendU16(bytes, 0);
      AppendU16(bytes, method);
      AppendU32(bytes, 0);
      AppendU32(bytes, good_crc);
      AppendU32(bytes, payload.size());
      AppendU32(bytes, payload.size());
      AppendU16(bytes, fname.size());
      AppendU16(bytes, 0);
      AppendU16(bytes, 0);
      AppendU16(bytes, 0);
      AppendU16(bytes, 0);
      AppendU32(bytes, 0);
      AppendU32(bytes, local_off);
      AppendBytes(bytes, fname.data(), fname.size());
    };
    add_cd(off0, 99);
    add_cd(off1, 0);
    add_cd(off2, Z_DEFLATED);
    add_cd(off3, Z_DEFLATED);

    const uint64_t cd_sz = bytes.size() - cd_off;
    AppendU32(bytes, 0x06054b50);
    AppendU16(bytes, 0);
    AppendU16(bytes, 0);
    AppendU16(bytes, 4);
    AppendU16(bytes, 4);
    AppendU32(bytes, cd_sz);
    AppendU32(bytes, cd_off);
    AppendU16(bytes, 0);

    const std::string corrupt_zip = "/vsimem/corrupt_entries.zip";
    autotest2::VsiMemTempWrapper w(
        corrupt_zip,
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));

    unzFile uf = cpl_unzOpen(corrupt_zip.c_str());
    ASSERT_NE(uf, nullptr);

    // Entry 0: unsupported compression method (99)
    {
      WithQuietHandler suppress;
      EXPECT_EQ(cpl_unzOpenCurrentFile(uf), UNZ_BADZIPFILE);
      unz_file_info local_fi = {};
      EXPECT_EQ(cpl_unzCurrentFileInfoFromLocalHeader(uf, off0, &local_fi,
                                                      nullptr, 0, nullptr),
                UNZ_BADZIPFILE);
    }

    // Entry 1: stored file with bad CRC -> UNZ_CRCERROR on close
    ASSERT_EQ(cpl_unzGoToNextFile(uf), UNZ_OK);
    ASSERT_EQ(cpl_unzOpenCurrentFile(uf), UNZ_OK);
    char buf[16] = {};
    EXPECT_EQ(cpl_unzReadCurrentFile(uf, buf, sizeof(buf)), 5);
    EXPECT_EQ(cpl_unzCloseCurrentFile(uf), UNZ_CRCERROR);

    // Entry 2: invalid deflate stream -> Z_DATA_ERROR on read
    ASSERT_EQ(cpl_unzGoToNextFile(uf), UNZ_OK);
    ASSERT_EQ(cpl_unzOpenCurrentFile(uf), UNZ_OK);
    EXPECT_LT(cpl_unzReadCurrentFile(uf, buf, sizeof(buf)), 0);
    EXPECT_EQ(cpl_unzCloseCurrentFile(uf), UNZ_OK);

    // Entry 3: bad local header magic & mismatched fields
    ASSERT_EQ(cpl_unzGoToNextFile(uf), UNZ_OK);
    EXPECT_EQ(cpl_unzOpenCurrentFile(uf), UNZ_BADZIPFILE);
    unz_file_info local_fi = {};
    EXPECT_EQ(cpl_unzCurrentFileInfoFromLocalHeader(uf, off3, &local_fi,
                                                    nullptr, 0, nullptr),
              UNZ_BADZIPFILE);

    // SetOffset to invalid central directory magic (off3 has 0xDEADBEEF)
    EXPECT_EQ(cpl_unzSetOffset(uf, off3), UNZ_BADZIPFILE);
    // Now current_file_ok == 0; test functions that check !s->current_file_ok
    EXPECT_EQ(cpl_unzGoToNextFile(uf), UNZ_END_OF_LIST_OF_FILE);
    EXPECT_EQ(cpl_unzLocateFile(uf, "a.txt", 1), UNZ_END_OF_LIST_OF_FILE);
    unz_file_pos pos = {};
    EXPECT_EQ(cpl_unzGetFilePos(uf, &pos), UNZ_END_OF_LIST_OF_FILE);
    EXPECT_EQ(cpl_unzOpenCurrentFile(uf), UNZ_PARAMERROR);
    EXPECT_EQ(cpl_unzGetOffset(uf), 0U);

    ASSERT_EQ(cpl_unzClose(uf), UNZ_OK);
  }
}

}  // namespace
