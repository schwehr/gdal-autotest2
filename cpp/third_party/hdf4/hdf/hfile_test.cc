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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/hdf/src/hchunks_priv.h"
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/hfile.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"
#include "third_party/hdf4/hdf/src/vg.h"
// clang-format on

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Ne;
using ::testing::NotNull;

std::string GetTempFilePath(const std::string& filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

int DummyTermFunc() { return 0; }

TEST(HfileTest, HishdfScenarios) {
  // Non-existent file
  const std::string non_existent = GetTempFilePath("hfile_nonexistent.hdf");
  std::filesystem::remove(non_existent);
  EXPECT_THAT(Hishdf(non_existent.c_str()), Eq(FALSE));

  // Empty file (0 bytes)
  const std::string empty_file = GetTempFilePath("hfile_empty.hdf");
  {
    std::ofstream ofs(empty_file, std::ios::binary);
  }
  EXPECT_THAT(Hishdf(empty_file.c_str()), Eq(FALSE));

  // Invalid magic number (4 bytes of garbage)
  const std::string corrupt_file = GetTempFilePath("hfile_corrupt.hdf");
  {
    std::ofstream ofs(corrupt_file, std::ios::binary);
    ofs.write("BAD!", 4);
  }
  EXPECT_THAT(Hishdf(corrupt_file.c_str()), Eq(FALSE));

  // Valid HDF file
  const std::string valid_file = GetTempFilePath("hfile_valid.hdf");
  const int32_t fid = Hopen(valid_file.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));

  EXPECT_THAT(Hishdf(valid_file.c_str()), Eq(TRUE));
}

TEST(HfileTest, VersionFunctions) {
  uint32_t maj = 0;
  uint32_t min = 0;
  uint32_t rel = 0;
  char str[128] = {0};

  EXPECT_THAT(Hgetlibversion(&maj, &min, &rel, str), Eq(SUCCEED));
  EXPECT_THAT(maj, Ge(4u));
  EXPECT_THAT(strlen(str), Gt(0u));

  const std::string path = GetTempFilePath("hfile_version.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  uint32_t fmaj = 0;
  uint32_t fmin = 0;
  uint32_t frel = 0;
  char fstr[128] = {0};
  EXPECT_THAT(Hgetfileversion(fid, &fmaj, &fmin, &frel, fstr), Eq(SUCCEED));
  EXPECT_THAT(fmaj, Ge(4u));
  EXPECT_THAT(strlen(fstr), Gt(0u));
  EXPECT_THAT(Hgetfileversion(fid, nullptr, nullptr, nullptr, nullptr),
              Eq(SUCCEED));
  EXPECT_THAT(Hgetfileversion(FAIL, &fmaj, &fmin, &frel, fstr), Eq(FAIL));
}

TEST(HfileTest, BasicLifecycleAndAccessModes) {
  const std::string path = GetTempFilePath("hfile_lifecycle.hdf");

  // Create file
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    EXPECT_THAT(HDvalidfid(fid), Eq(TRUE));
    EXPECT_THAT(HPisfile_in_use(path.c_str()), Eq(TRUE));

    // Inquire about open file
    char* fname = nullptr;
    int acc = 0;
    int att = 0;
    EXPECT_THAT(Hfidinquire(FAIL, &fname, &acc, &att), Eq(FAIL));
    EXPECT_THAT(Hfidinquire(fid, &fname, &acc, &att), Eq(SUCCEED));
    EXPECT_THAT(fname, NotNull());
    EXPECT_THAT(att, Eq(0));
    EXPECT_THAT(acc, Eq(DFACC_ALL));

    // Write element (tag 100, ref 1)
    const int32_t aid = Hstartwrite(fid, 100, 1, 4);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(Hfidinquire(fid, &fname, &acc, &att), Eq(SUCCEED));
    EXPECT_THAT(att, Eq(1));
    uint8_t data[4] = {1, 2, 3, 4};
    EXPECT_THAT(Hwrite(aid, 4, data), Eq(4));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
    EXPECT_THAT(HPisfile_in_use(path.c_str()), Eq(FALSE));
  }

  // Open read-only
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));

    // Reopen same file concurrently (should succeed and share filerec)
    const int32_t fid2 = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid2, Ne(FAIL));

    char* fname = nullptr;
    int acc = 0;
    int att = 0;
    EXPECT_THAT(Hfidinquire(FAIL, &fname, &acc, &att), Eq(FAIL));
    EXPECT_THAT(Hfidinquire(fid, &fname, &acc, &att), Eq(SUCCEED));
    EXPECT_THAT(fname, NotNull());
    EXPECT_THAT(att, Eq(0));

    // Read element
    const int32_t aid = Hstartread(fid, 100, 1);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(Hfidinquire(fid, &fname, &acc, &att), Eq(SUCCEED));
    EXPECT_THAT(att, Eq(1));
    uint8_t buf[4] = {0};
    EXPECT_THAT(Hread(aid, 4, buf), Eq(4));
    EXPECT_THAT(buf[0], Eq(1));
    EXPECT_THAT(buf[3], Eq(4));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

    EXPECT_THAT(Hclose(fid2), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(HfileTest, OpenAndCloseEdgeCases) {
  const std::string path = GetTempFilePath("hfile_edge_cases.hdf");
  std::filesystem::remove(path);

  // Invalid parameters to Hopen
  EXPECT_THAT(Hopen(nullptr, DFACC_READ, 0), Eq(FAIL));
  EXPECT_THAT(Hopen("", DFACC_READ, 0), Eq(FAIL));
  EXPECT_THAT(Hopen(path.c_str(), -1, 0), Eq(FAIL));
  EXPECT_THAT(Hopen(path.c_str(), DFACC_CREATE, -1), Eq(FAIL));
  std::filesystem::remove(path);
  EXPECT_THAT(Hopen(path.c_str(), DFACC_READ, 0), Eq(FAIL));

  // DFACC_WRITE on non-existent file creates it
  const int32_t fid_w = Hopen(path.c_str(), DFACC_WRITE, 16);
  ASSERT_THAT(fid_w, Ne(FAIL));
  EXPECT_THAT(Hclose(fid_w), Eq(SUCCEED));

  // Open with DFACC_READ and check Hopen(path, DFACC_CREATE) fails with
  // DFE_ALROPEN
  const int32_t fid_ro = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(fid_ro, Ne(FAIL));
  EXPECT_THAT(Hishdf(path.c_str()), Eq(TRUE));
  EXPECT_THAT(Hopen(path.c_str(), DFACC_CREATE, 0), Eq(FAIL));
  EXPECT_THAT(Hclose(fid_ro), Eq(SUCCEED));

  // Opening a corrupt/non-HDF file for read fails with DFE_NOTDFFILE
  const std::string bad_magic_path = GetTempFilePath("hfile_bad_magic.hdf");
  {
    std::ofstream ofs(bad_magic_path, std::ios::binary);
    ofs.write("BAD!", 4);
  }
  EXPECT_THAT(Hopen(bad_magic_path.c_str(), DFACC_READ, 0), Eq(FAIL));
  std::filesystem::remove(bad_magic_path);

  // Hclose with invalid fid
  EXPECT_THAT(Hclose(FAIL), Eq(FAIL));

  // Hclose while an aid is active fails with DFE_OPENAID
  const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  const int32_t aid = Hstartwrite(fid, 150, 1, 4);
  ASSERT_THAT(aid, Ne(FAIL));
  EXPECT_THAT(Hclose(fid), Eq(FAIL));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
}

TEST(HfileTest, ReopenFileUpgradingToReadWrite) {
  const std::string path = GetTempFilePath("hfile_upgrade.hdf");
  const int32_t fid_cr = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid_cr, Ne(FAIL));
  const int32_t aid = Hstartwrite(fid_cr, 100, 1, 4);
  ASSERT_THAT(aid, Ne(FAIL));
  uint8_t data[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(aid, 4, data), Eq(4));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid_cr), Eq(SUCCEED));

  // Open with DFACC_READ
  const int32_t fid_ro = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(fid_ro, Ne(FAIL));

  // Reopen concurrently with DFACC_RDWR: triggers reopening file for write
  const int32_t fid_rw = Hopen(path.c_str(), DFACC_RDWR, 0);
  ASSERT_THAT(fid_rw, Ne(FAIL));

  EXPECT_THAT(Hclose(fid_rw), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid_ro), Eq(SUCCEED));

  // Reopening for write when file has read-only disk permissions triggers
  // DFE_DENIED
  const std::string ro_path = GetTempFilePath("hfile_ro_perm.hdf");
  const int32_t fid_init = Hopen(ro_path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid_init, Ne(FAIL));
  EXPECT_THAT(Hclose(fid_init), Eq(SUCCEED));

  const int32_t fid_read = Hopen(ro_path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(fid_read, Ne(FAIL));
  std::filesystem::permissions(ro_path, std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace);
  EXPECT_THAT(Hopen(ro_path.c_str(), DFACC_RDWR, 0), Eq(FAIL));
  std::filesystem::permissions(
      ro_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);
  EXPECT_THAT(Hclose(fid_read), Eq(SUCCEED));
}

TEST(HfileTest, ReadWriteSeekTellHinquire) {
  const std::string path = GetTempFilePath("hfile_rw_seek.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Hstartwrite with negative length on a new element fails in Hsetlength
  EXPECT_THAT(Hstartwrite(fid, 200, 9999, -1), Eq(FAIL));

  const int32_t aid = Hstartwrite(fid, 200, 1, 10);
  ASSERT_THAT(aid, Ne(FAIL));

  // Hwrite invalid args
  EXPECT_THAT(Hwrite(FAIL, 5, "hello"), Eq(FAIL));
  EXPECT_THAT(Hwrite(aid, 5, nullptr), Eq(FAIL));
  EXPECT_THAT(Hwrite(aid, -1, "hello"), Eq(FAIL));
  EXPECT_THAT(Hwrite(aid, 0, "hello"), Eq(FAIL));

  uint8_t data[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  EXPECT_THAT(Hwrite(aid, 10, data), Eq(10));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Double Hendaccess fails
  EXPECT_THAT(Hendaccess(aid), Eq(FAIL));

  const int32_t raid = Hstartread(fid, 200, 1);
  ASSERT_THAT(raid, Ne(FAIL));

  // Hinquire
  int32 f_id = 0, len = 0, off = 0, pos = 0;
  uint16 tag = 0, ref = 0;
  int16 acc = 0, sp = 0;
  EXPECT_THAT(Hinquire(raid, &f_id, &tag, &ref, &len, &off, &pos, &acc, &sp),
              Eq(SUCCEED));
  EXPECT_THAT(f_id, Eq(fid));
  EXPECT_THAT(tag, Eq(200));
  EXPECT_THAT(ref, Eq(1));
  EXPECT_THAT(len, Eq(10));
  EXPECT_THAT(pos, Eq(0));
  EXPECT_THAT(sp, Eq(0));
  EXPECT_THAT(Hinquire(FAIL, nullptr, nullptr, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr),
              Eq(FAIL));
  EXPECT_THAT(Hinquire(raid, nullptr, nullptr, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr),
              Eq(SUCCEED));

  // Cannot write to read-only access element
  EXPECT_THAT(Hwrite(raid, 1, data), Eq(FAIL));

  // Hread invalid args
  uint8_t rbuf[10] = {0};
  EXPECT_THAT(Hread(FAIL, 5, rbuf), Eq(FAIL));
  EXPECT_THAT(Hread(raid, 5, nullptr), Eq(FAIL));
  EXPECT_THAT(Hread(raid, -1, rbuf), Eq(FAIL));

  // Read partially
  EXPECT_THAT(Hread(raid, 4, rbuf), Eq(4));
  EXPECT_THAT(rbuf[0], Eq(0));
  EXPECT_THAT(rbuf[3], Eq(3));
  EXPECT_THAT(Htell(raid), Eq(4));

  // Hseek
  EXPECT_THAT(Hseek(raid, 2, DF_START), Eq(SUCCEED));
  EXPECT_THAT(Htell(raid), Eq(2));
  // Seeking to current position bypasses position update (offset == posn)
  EXPECT_THAT(Hseek(raid, 2, DF_START), Eq(SUCCEED));
  EXPECT_THAT(Hseek(raid, 0, DF_CURRENT), Eq(SUCCEED));
  EXPECT_THAT(Hseek(raid, 3, DF_CURRENT), Eq(SUCCEED));
  EXPECT_THAT(Htell(raid), Eq(5));
  EXPECT_THAT(Hseek(raid, -2, DF_END), Eq(SUCCEED));
  EXPECT_THAT(Htell(raid), Eq(8));
  EXPECT_THAT(Hseek(raid, 0, DF_END), Eq(SUCCEED));
  EXPECT_THAT(Htell(raid), Eq(10));

  // Read to end with length 0
  EXPECT_THAT(Hseek(raid, 6, DF_START), Eq(SUCCEED));
  uint8_t rem[4] = {0};
  EXPECT_THAT(Hread(raid, 0, rem), Eq(4));
  EXPECT_THAT(rem[0], Eq(6));
  EXPECT_THAT(rem[3], Eq(9));

  // Invalid seek positions
  EXPECT_THAT(Hseek(FAIL, 0, DF_START), Eq(FAIL));
  EXPECT_THAT(Hseek(raid, 0, 999), Eq(FAIL));
  EXPECT_THAT(Hseek(raid, -1, DF_START), Eq(FAIL));
  EXPECT_THAT(Hseek(raid, 20, DF_START), Eq(FAIL));
  EXPECT_THAT(Hseek(raid, -20, DF_CURRENT), Eq(FAIL));
  EXPECT_THAT(Hseek(raid, -20, DF_END), Eq(FAIL));
  EXPECT_THAT(Htell(FAIL), Eq(FAIL));

  EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));
}

TEST(HfileTest, ByteIOFunctions) {
  const std::string path = GetTempFilePath("hfile_byteio.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid = Hstartwrite(fid, 300, 1, 3);
  ASSERT_THAT(aid, Ne(FAIL));
  EXPECT_THAT(HDputc('A', aid), Eq('A'));
  EXPECT_THAT(HDputc('B', aid), Eq('B'));
  EXPECT_THAT(HDputc('C', aid), Eq('C'));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  const int32_t raid = Hstartread(fid, 300, 1);
  ASSERT_THAT(raid, Ne(FAIL));
  EXPECT_THAT(HDgetc(raid), Eq('A'));
  EXPECT_THAT(HDgetc(raid), Eq('B'));
  EXPECT_THAT(HDgetc(raid), Eq('C'));
  // At EOF, Hread returns 0 so HDgetc returns (int)(uint8)FAIL == 255.
  EXPECT_THAT(HDgetc(raid), Eq(255));
  EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));

  // Invalid aid returns FAIL (-1).
  EXPECT_THAT(HDgetc(FAIL), Eq(FAIL));
  EXPECT_THAT(HDputc('X', FAIL), Eq(FAIL));
}

TEST(HfileTest, DirectElementManipulation) {
  const std::string path = GetTempFilePath("hfile_direct_elem.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  uint8_t data[5] = {10, 20, 30, 40, 50};
  EXPECT_THAT(Hputelement(fid, 400, 1, data, 5), Eq(5));

  uint8_t read_buf[5] = {0};
  EXPECT_THAT(Hgetelement(fid, 400, 1, read_buf), Eq(5));
  EXPECT_THAT(read_buf[0], Eq(10));
  EXPECT_THAT(read_buf[4], Eq(50));

  EXPECT_THAT(Hlength(fid, 400, 1), Eq(5));
  EXPECT_THAT(Hoffset(fid, 400, 1), Gt(0));
  EXPECT_THAT(Hexist(fid, 400, 1), Eq(SUCCEED));
  EXPECT_THAT(Hexist(fid, 400, 999), Eq(FAIL));

  // HDcheck_empty on non-special element with data
  int empty_non_special = -1;
  EXPECT_THAT(HDcheck_empty(fid, 400, 1, &empty_non_special), Eq(SUCCEED));
  EXPECT_THAT(empty_non_special, Eq(FALSE));

  // Invalid argument handling
  EXPECT_THAT(Hputelement(FAIL, 400, 1, data, 5), Eq(FAIL));
  EXPECT_THAT(Hputelement(fid, 400, 1, data, -1), Eq(FAIL));
  EXPECT_THAT(Hputelement(fid, 400, 2, nullptr, 5), Eq(FAIL));
  EXPECT_THAT(Hgetelement(FAIL, 400, 1, read_buf), Eq(FAIL));
  EXPECT_THAT(Hgetelement(fid, 400, 999, read_buf), Eq(FAIL));
  EXPECT_THAT(Hgetelement(fid, 400, 1, nullptr), Eq(FAIL));
  EXPECT_THAT(Hlength(FAIL, 400, 1), Eq(FAIL));
  EXPECT_THAT(Hlength(fid, 400, 999), Eq(FAIL));
  EXPECT_THAT(Hoffset(FAIL, 400, 1), Eq(FAIL));
  EXPECT_THAT(Hoffset(fid, 400, 999), Eq(FAIL));
  EXPECT_THAT(Hexist(FAIL, 400, 1), Eq(FAIL));
}

TEST(HfileTest, AppendableAndTruncate) {
  const std::string path = GetTempFilePath("hfile_append.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid = Hstartwrite(fid, 500, 1, 4);
  ASSERT_THAT(aid, Ne(FAIL));
  uint8_t data[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(aid, 4, data), Eq(4));

  // The element at the end of the file is appendable.
  EXPECT_THAT(HPisappendable(aid), Eq(SUCCEED));
  EXPECT_THAT(Happendable(aid), Eq(SUCCEED));

  // Write additional data to appendable element
  uint8_t more[2] = {5, 6};
  EXPECT_THAT(Hwrite(aid, 2, more), Eq(2));

  // Htrunc returns the new truncated length.
  EXPECT_THAT(Htrunc(aid, 100), Eq(FAIL));  // trunc_len >= data_len fails
  EXPECT_THAT(Htrunc(aid, 3), Eq(3));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Hsetlength works on a new element before any write.
  const int32_t aid2 = Hstartaccess(fid, 501, 1, DFACC_WRITE);
  ASSERT_THAT(aid2, Ne(FAIL));
  EXPECT_THAT(Hsetlength(aid2, 8), Eq(SUCCEED));
  EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));

  // Hsetlength fails on already-written element (new_elem == FALSE)
  const int32_t aid3 = Hstartwrite(fid, 502, 1, 4);
  ASSERT_THAT(aid3, Ne(FAIL));
  EXPECT_THAT(Hwrite(aid3, 4, data), Eq(4));
  EXPECT_THAT(Hsetlength(aid3, 5), Eq(FAIL));
  EXPECT_THAT(Hendaccess(aid3), Eq(SUCCEED));

  // Internal error branches via tampered access_rec
  const int32_t taid = Hstartwrite(fid, 503, 1, 4);
  ASSERT_THAT(taid, Ne(FAIL));
  accrec_t* tacc = static_cast<accrec_t*>(HAatom_object(taid));
  ASSERT_THAT(tacc, NotNull());

  const int32_t saved_fid = tacc->file_id;
  tacc->file_id = FAIL;
  EXPECT_THAT(HPisappendable(taid), Eq(FAIL));
  EXPECT_THAT(Hsetlength(taid, 10), Eq(FAIL));
  tacc->file_id = saved_fid;

  const atom_t saved_ddid = tacc->ddid;
  tacc->ddid = FAIL;
  EXPECT_THAT(Hseek(taid, 0, DF_START), Eq(FAIL));
  uint8_t dummy[4] = {0};
  EXPECT_THAT(Hwrite(taid, 4, dummy), Eq(FAIL));
  EXPECT_THAT(Htrunc(taid, 2), Eq(FAIL));
  tacc->ddid = saved_ddid;

  EXPECT_THAT(Hendaccess(taid), Eq(SUCCEED));

  // Hnextread with invalid file_id in accrec
  const int32_t naid = Hstartread(fid, 500, 1);
  ASSERT_THAT(naid, Ne(FAIL));
  accrec_t* nacc = static_cast<accrec_t*>(HAatom_object(naid));
  ASSERT_THAT(nacc, NotNull());
  nacc->file_id = FAIL;
  EXPECT_THAT(Hnextread(naid, 500, 1, DF_START), Eq(FAIL));
  nacc->file_id = saved_fid;
  EXPECT_THAT(Hendaccess(naid), Eq(SUCCEED));

  // Invalid arguments
  EXPECT_THAT(Happendable(FAIL), Eq(FAIL));
  EXPECT_THAT(HPisappendable(FAIL), Eq(FAIL));
  EXPECT_THAT(Hsetlength(FAIL, 5), Eq(FAIL));
  EXPECT_THAT(Htrunc(FAIL, 1), Eq(FAIL));
}

TEST(HfileTest, AppendableNotAtEofTriggersConversion) {
  const std::string path = GetTempFilePath("hfile_append_conv.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid1 = Hstartwrite(fid, 510, 1, 4);
  ASSERT_THAT(aid1, Ne(FAIL));
  uint8_t data[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(aid1, 4, data), Eq(4));
  EXPECT_THAT(Happendable(aid1), Eq(SUCCEED));

  // Write second element so aid1 is no longer at EOF
  const int32_t aid2 = Hstartwrite(fid, 511, 1, 4);
  ASSERT_THAT(aid2, Ne(FAIL));
  EXPECT_THAT(Hwrite(aid2, 4, data), Eq(4));
  EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));

  // aid1 is not at EOF now
  EXPECT_THAT(HPisappendable(aid1), Eq(FAIL));

  // If HLconvert fails, Hwrite sets appendable=FALSE and returns DFE_BADSEEK
  accrec_t* acc1 = static_cast<accrec_t*>(HAatom_object(aid1));
  ASSERT_THAT(acc1, NotNull());
  acc1->block_size = -1;
  uint8_t extra[10] = {0};
  EXPECT_THAT(Hwrite(aid1, 10, extra), Eq(FAIL));
  EXPECT_THAT(acc1->appendable, Eq(FALSE));

  // Restore and test successful promotion to linked block
  acc1->appendable = TRUE;
  acc1->block_size = 16;
  acc1->num_blocks = 2;
  EXPECT_THAT(Hwrite(aid1, 10, extra), Eq(10));
  EXPECT_THAT(Hendaccess(aid1), Eq(SUCCEED));
}

TEST(HfileTest, SeekAppendableNotAtEofTriggersConversion) {
  const std::string path = GetTempFilePath("hfile_seek_append_conv.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid1 = Hstartwrite(fid, 520, 1, 4);
  ASSERT_THAT(aid1, Ne(FAIL));
  uint8_t data[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(aid1, 4, data), Eq(4));
  EXPECT_THAT(Happendable(aid1), Eq(SUCCEED));

  // Write second element so aid1 is no longer at EOF
  const int32_t aid2 = Hstartwrite(fid, 521, 1, 4);
  ASSERT_THAT(aid2, Ne(FAIL));
  EXPECT_THAT(Hwrite(aid2, 4, data), Eq(4));
  EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));

  // If HLconvert fails during Hseek, appendable is cleared and returns FAIL
  accrec_t* acc1 = static_cast<accrec_t*>(HAatom_object(aid1));
  ASSERT_THAT(acc1, NotNull());
  acc1->block_size = -1;
  EXPECT_THAT(Hseek(aid1, 10, DF_START), Eq(FAIL));
  EXPECT_THAT(acc1->appendable, Eq(FALSE));

  // Restore and test successful seek with conversion
  acc1->appendable = TRUE;
  acc1->block_size = 16;
  acc1->num_blocks = 2;
  EXPECT_THAT(Hseek(aid1, 10, DF_START), Eq(SUCCEED));
  EXPECT_THAT(Htell(aid1), Eq(10));
  EXPECT_THAT(Hendaccess(aid1), Eq(SUCCEED));
}

TEST(HfileTest, TagAndRefUtilities) {
  const std::string path = GetTempFilePath("hfile_tags.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Tag helper macros
  EXPECT_FALSE(SPECIALTAG(600));
  const uint16_t sp_tag = MKSPECIALTAG(600);
  EXPECT_TRUE(SPECIALTAG(sp_tag));
  EXPECT_THAT(BASETAG(sp_tag), Eq(600));

  // Reference numbers
  const uint16_t ref1 = Hnewref(fid);
  EXPECT_THAT(ref1, Gt(0));
  const uint16_t ref2 = Htagnewref(fid, 600);
  EXPECT_THAT(ref2, Gt(0));

  EXPECT_THAT(Hnumber(fid, 600), Eq(0));
  uint8_t d[2] = {1, 2};
  EXPECT_THAT(Hputelement(fid, 600, ref1, d, 2), Eq(2));
  EXPECT_THAT(Hputelement(fid, 600, ref2, d, 2), Eq(2));
  EXPECT_THAT(Hnumber(fid, 600), Eq(2));
  EXPECT_THAT(Hnumber(fid, DFTAG_WILDCARD), Ge(2));

  // Invalid tag/ref args
  EXPECT_THAT(Hnewref(FAIL), Eq(0));
  EXPECT_THAT(Htagnewref(FAIL, 600), Eq(0));
  EXPECT_THAT(Hnumber(FAIL, 600), Eq(FAIL));
}

TEST(HfileTest, HnextreadAndSearch) {
  const std::string path = GetTempFilePath("hfile_nextread.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  uint8_t d1[2] = {1, 1};
  uint8_t d2[2] = {2, 2};
  EXPECT_THAT(Hputelement(fid, 700, 1, d1, 2), Eq(2));
  EXPECT_THAT(Hputelement(fid, 700, 2, d2, 2), Eq(2));

  // Wildcard start read
  const int32_t aid = Hstartread(fid, DFTAG_WILDCARD, DFREF_WILDCARD);
  ASSERT_THAT(aid, Ne(FAIL));

  // Move forward with Hnextread
  EXPECT_THAT(Hnextread(aid, 700, 2, DF_CURRENT), Eq(SUCCEED));
  uint8_t buf[2] = {0};
  EXPECT_THAT(Hread(aid, 2, buf), Eq(2));
  EXPECT_THAT(buf[0], Eq(2));

  // Seek back to start with Hnextread
  EXPECT_THAT(Hnextread(aid, 700, 1, DF_START), Eq(SUCCEED));
  EXPECT_THAT(Hread(aid, 2, buf), Eq(2));
  EXPECT_THAT(buf[0], Eq(1));

  // Invalid Hnextread calls
  EXPECT_THAT(Hnextread(FAIL, 700, 1, DF_START), Eq(FAIL));
  EXPECT_THAT(Hnextread(aid, 700, 1, 999), Eq(FAIL));
  EXPECT_THAT(Hnextread(aid, 9999, 9999, DF_CURRENT), Eq(FAIL));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HfileTest, SpecialLinkedBlockElement) {
  const std::string path = GetTempFilePath("hfile_special_linked.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Create linked block element: tag 800, ref 1, block length 16, 2 blocks
  const int32_t laid = HLcreate(fid, 800, 1, 16, 2);
  ASSERT_THAT(laid, Ne(FAIL));
  uint8_t write_buf[32] = {0};
  for (int i = 0; i < 32; ++i) write_buf[i] = static_cast<uint8_t>(i);
  EXPECT_THAT(Hwrite(laid, 32, write_buf), Eq(32));
  EXPECT_THAT(Hendaccess(laid), Eq(SUCCEED));

  // Start read on the special element
  const int32_t raid = Hstartread(fid, 800, 1);
  ASSERT_THAT(raid, Ne(FAIL));

  // Inquire on special element
  int32 f_id = 0, len = 0, off = 0, pos = 0;
  uint16 tag = 0, ref = 0;
  int16 acc = 0, sp = 0;
  EXPECT_THAT(Hinquire(raid, &f_id, &tag, &ref, &len, &off, &pos, &acc, &sp),
              Eq(SUCCEED));
  EXPECT_THAT(BASETAG(tag), Eq(800));
  EXPECT_THAT(tag, Eq(MKSPECIALTAG(800)));
  EXPECT_THAT(ref, Eq(1));
  EXPECT_THAT(len, Eq(32));
  EXPECT_THAT(sp, Ne(0));

  // Seek and tell on special element
  EXPECT_THAT(Hseek(raid, 10, DF_START), Eq(SUCCEED));
  EXPECT_THAT(Htell(raid), Eq(10));
  EXPECT_THAT(Hseek(raid, 5, DF_CURRENT), Eq(SUCCEED));
  EXPECT_THAT(Htell(raid), Eq(15));
  EXPECT_THAT(Hseek(raid, -5, DF_END), Eq(SUCCEED));
  EXPECT_THAT(Htell(raid), Eq(27));

  // Read from special element
  uint8_t read_buf[5] = {0};
  EXPECT_THAT(Hread(raid, 5, read_buf), Eq(5));
  EXPECT_THAT(read_buf[0], Eq(27));

  // HDget_special_info on special element
  sp_info_block_t info = {};
  EXPECT_THAT(HDget_special_info(raid, &info), Eq(SUCCEED));

  // Hsetaccesstype on special element calls HXPsetaccesstype (returns FAIL)
  EXPECT_THAT(Hsetaccesstype(raid, DFACC_PARALLEL), Eq(FAIL));

  // Open second reader to test HIgetspinfo reusing existing special info
  const int32_t raid2 = Hstartread(fid, 800, 1);
  ASSERT_THAT(raid2, Ne(FAIL));
  accrec_t* racc2 = static_cast<accrec_t*>(HAatom_object(raid2));
  ASSERT_THAT(racc2, NotNull());
  EXPECT_THAT(HIgetspinfo(racc2), NotNull());
  EXPECT_THAT(Hendaccess(raid2), Eq(SUCCEED));

  // Hnextread on special element (triggers lines 750-768)
  EXPECT_THAT(Hnextread(raid, 800, 1, DF_START), Eq(SUCCEED));

  EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));

  // Write access to existing special element
  const int32_t waid = Hstartaccess(fid, 800, 1, DFACC_WRITE);
  ASSERT_THAT(waid, Ne(FAIL));
  EXPECT_THAT(Hseek(waid, 0, DF_START), Eq(SUCCEED));
  uint8_t new_val = 99;
  EXPECT_THAT(Hwrite(waid, 1, &new_val), Eq(1));
  EXPECT_THAT(Hendaccess(waid), Eq(SUCCEED));

  // HDcheck_empty on linked block element is not supported (leaves empty
  // unchanged)
  int empty = -1;
  EXPECT_THAT(HDcheck_empty(fid, 800, 1, &empty), Eq(SUCCEED));
  EXPECT_THAT(empty, Eq(-1));
}

TEST(HfileTest, SpecialElementTypesAndCheckEmpty) {
  const std::string path = GetTempFilePath("hfile_special_headers.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  EXPECT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup close_file = [fid] {
    EXPECT_THAT(Vend(fid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  };

  // 1. SPECIAL_COMP with length > 0
  const uint16_t tag_comp = 601;
  const uint16_t ref_comp = Hnewref(fid);
  const int32_t aid_comp =
      Hstartaccess(fid, MKSPECIALTAG(tag_comp), ref_comp, DFACC_WRITE);
  ASSERT_THAT(aid_comp, Ne(FAIL));
  uint8_t comp_hdr[16] = {0};
  uint8_t* p = comp_hdr;
  INT16ENCODE(p, SPECIAL_COMP);
  INT16ENCODE(p, 1);    // version
  INT32ENCODE(p, 100);  // length = 100
  EXPECT_THAT(Hwrite(aid_comp, 16, comp_hdr), Eq(16));
  EXPECT_THAT(Hendaccess(aid_comp), Eq(SUCCEED));

  int empty_comp = -1;
  EXPECT_THAT(HDcheck_empty(fid, tag_comp, ref_comp, &empty_comp), Eq(SUCCEED));
  EXPECT_THAT(empty_comp, Eq(FALSE));

  // 2. SPECIAL_COMP with length == 0 (empty)
  const uint16_t tag_comp_zero = 602;
  const uint16_t ref_comp_zero = Hnewref(fid);
  const int32_t aid_comp_zero = Hstartaccess(fid, MKSPECIALTAG(tag_comp_zero),
                                             ref_comp_zero, DFACC_WRITE);
  ASSERT_THAT(aid_comp_zero, Ne(FAIL));
  uint8_t comp_zero_hdr[16] = {0};
  p = comp_zero_hdr;
  INT16ENCODE(p, SPECIAL_COMP);
  INT16ENCODE(p, 1);
  INT32ENCODE(p, 0);  // length = 0
  EXPECT_THAT(Hwrite(aid_comp_zero, 16, comp_zero_hdr), Eq(16));
  EXPECT_THAT(Hendaccess(aid_comp_zero), Eq(SUCCEED));

  int empty_zero = -1;
  EXPECT_THAT(HDcheck_empty(fid, tag_comp_zero, ref_comp_zero, &empty_zero),
              Eq(SUCCEED));
  EXPECT_THAT(empty_zero, Eq(TRUE));

  // 3. Regular element without data (empty SDS)
  const uint16_t tag_empty = 603;
  const uint16_t ref_empty = Hnewref(fid);
  const int32_t aid_empty =
      Hstartaccess(fid, tag_empty, ref_empty, DFACC_WRITE);
  ASSERT_THAT(aid_empty, Ne(FAIL));
  EXPECT_THAT(Hendaccess(aid_empty), Eq(SUCCEED));

  int empty_no_data = -1;
  EXPECT_THAT(HDcheck_empty(fid, tag_empty, ref_empty, &empty_no_data),
              Eq(SUCCEED));
  EXPECT_THAT(empty_no_data, Eq(TRUE));

  // Invalid file_id to HDcheck_empty
  EXPECT_THAT(HDcheck_empty(FAIL, tag_empty, ref_empty, &empty_no_data),
              Eq(FAIL));

  // 4. SPECIAL_EXT header
  const uint16_t tag_ext = 604;
  const uint16_t ref_ext = Hnewref(fid);
  const int32_t aid_ext =
      Hstartaccess(fid, MKSPECIALTAG(tag_ext), ref_ext, DFACC_WRITE);
  ASSERT_THAT(aid_ext, Ne(FAIL));
  uint8_t ext_hdr[16] = {0};
  p = ext_hdr;
  INT16ENCODE(p, SPECIAL_EXT);
  EXPECT_THAT(Hwrite(aid_ext, 16, ext_hdr), Eq(16));
  EXPECT_THAT(Hendaccess(aid_ext), Eq(SUCCEED));

  int empty_ext = -1;
  EXPECT_THAT(HDcheck_empty(fid, tag_ext, ref_ext, &empty_ext), Eq(SUCCEED));

  // 5. SPECIAL_COMPRAS header
  const uint16_t tag_cr = 605;
  const uint16_t ref_cr = Hnewref(fid);
  const int32_t aid_cr =
      Hstartaccess(fid, MKSPECIALTAG(tag_cr), ref_cr, DFACC_WRITE);
  ASSERT_THAT(aid_cr, Ne(FAIL));
  uint8_t cr_hdr[16] = {0};
  p = cr_hdr;
  INT16ENCODE(p, SPECIAL_COMPRAS);
  EXPECT_THAT(Hwrite(aid_cr, 16, cr_hdr), Eq(16));
  EXPECT_THAT(Hendaccess(aid_cr), Eq(SUCCEED));

  // 6. Test HPread_drec directly
  filerec_t* file_rec = static_cast<filerec_t*>(HAatom_object(fid));
  ASSERT_THAT(file_rec, NotNull());
  atom_t data_id = HTPselect(file_rec, tag_comp, ref_comp);
  ASSERT_THAT(data_id, Ne(FAIL));
  uint8_t* drec_buf = nullptr;
  int32 drec_len = HPread_drec(fid, data_id, &drec_buf);
  EXPECT_THAT(drec_len, Gt(0));
  free(drec_buf);
  EXPECT_THAT(HTPendaccess(data_id), Eq(SUCCEED));

  // 7. Chunked element HDcheck_empty (empty -> not empty) and Hnextread
  // SPECIAL_CHUNKED
  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = static_cast<int32_t>(sizeof(uint8_t)),
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill_val = 0;
  const uint16_t tag_chk = 606;
  const uint16_t ref_chk = Hnewref(fid);
  const int32_t caid = HMCcreate(fid, tag_chk, ref_chk, 1, 1, &fill_val, &chk);
  ASSERT_THAT(caid, Ne(FAIL));
  EXPECT_THAT(Hendaccess(caid), Eq(SUCCEED));

  int empty_chunked = -1;
  EXPECT_THAT(HDcheck_empty(fid, tag_chk, ref_chk, &empty_chunked),
              Eq(SUCCEED));
  EXPECT_THAT(empty_chunked, Eq(TRUE));

  // Write a chunk
  const int32_t cwaid = Hstartaccess(fid, tag_chk, ref_chk, DFACC_WRITE);
  ASSERT_THAT(cwaid, Ne(FAIL));
  uint8_t cdata[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(cwaid, 4, cdata), Eq(4));
  EXPECT_THAT(Hendaccess(cwaid), Eq(SUCCEED));

  EXPECT_THAT(HDcheck_empty(fid, tag_chk, ref_chk, &empty_chunked),
              Eq(SUCCEED));
  EXPECT_THAT(empty_chunked, Eq(FALSE));

  const int32_t craid = Hstartread(fid, tag_chk, ref_chk);
  ASSERT_THAT(craid, Ne(FAIL));
  EXPECT_THAT(Hnextread(craid, tag_chk, ref_chk, DF_START), Eq(SUCCEED));
  EXPECT_THAT(Hendaccess(craid), Eq(SUCCEED));

  // 8. External element HDset_special_info and Hnextread SPECIAL_EXT
  const std::string ext_file = GetTempFilePath("hfile_ext_target.bin");
  const int32_t ext_aid = HXcreate(fid, 607, 1, ext_file.c_str(), 0, 16);
  ASSERT_THAT(ext_aid, Ne(FAIL));
  uint8_t ext_data[16] = {0};
  EXPECT_THAT(Hwrite(ext_aid, 16, ext_data), Eq(16));
  EXPECT_THAT(Hendaccess(ext_aid), Eq(SUCCEED));

  const int32_t ext_raid = Hstartread(fid, 607, 1);
  ASSERT_THAT(ext_raid, Ne(FAIL));
  sp_info_block_t sp_info = {};
  EXPECT_THAT(HDget_special_info(ext_raid, &sp_info), Eq(SUCCEED));
  char new_ext_path[] = "hfile_ext_renamed.bin";
  sp_info.path = new_ext_path;
  EXPECT_THAT(HDset_special_info(ext_raid, &sp_info), Eq(SUCCEED));
  EXPECT_THAT(Hnextread(ext_raid, 607, 1, DF_START), Eq(SUCCEED));
  EXPECT_THAT(Hendaccess(ext_raid), Eq(SUCCEED));

  // 9. Compressed element Hnextread SPECIAL_COMP
  model_info minfo = {};
  comp_info cinfo = {};
  cinfo.deflate_setting.level = 6;
  const int32_t cmp_aid = HCcreate(fid, 608, 1, COMP_MODEL_STDIO, &minfo,
                                   COMP_CODE_DEFLATE, &cinfo);
  ASSERT_THAT(cmp_aid, Ne(FAIL));
  uint8_t cmp_data[16] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(cmp_aid, 16, cmp_data), Eq(16));
  EXPECT_THAT(Hendaccess(cmp_aid), Eq(SUCCEED));

  const int32_t cmp_raid = Hstartread(fid, 608, 1);
  ASSERT_THAT(cmp_raid, Ne(FAIL));
  EXPECT_THAT(Hnextread(cmp_raid, DFTAG_WILDCARD, DFREF_WILDCARD, DF_START),
              Eq(SUCCEED));
  EXPECT_THAT(Hendaccess(cmp_raid), Eq(SUCCEED));
}

TEST(HfileTest, FileWithoutVersionTagTriggersFallbackAndUpgrade) {
  const std::string path = GetTempFilePath("hfile_no_version.hdf");

  // Create file and delete version tag
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    EXPECT_THAT(Hdeldd(fid, DFTAG_VERSION, 1), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Reopen with DFACC_READ: triggers HIread_version failure fallback
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    uint32_t fmaj = 999, fmin = 999, frel = 999;
    char fstr[128] = "dummy";
    EXPECT_THAT(Hgetfileversion(fid, &fmaj, &fmin, &frel, fstr), Eq(SUCCEED));
    EXPECT_THAT(fmaj, Eq(0u));
    EXPECT_THAT(fmin, Eq(0u));
    EXPECT_THAT(frel, Eq(0u));
    EXPECT_THAT(strlen(fstr), Eq(0u));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Open with DFACC_WRITE and write an element: triggers HIcheckfileversion
  // newver branch and HIupdate_version upon Hclose
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_WRITE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t aid = Hstartwrite(fid, 100, 1, 4);
    ASSERT_THAT(aid, Ne(FAIL));
    uint8_t data[4] = {1, 2, 3, 4};
    EXPECT_THAT(Hwrite(aid, 4, data), Eq(4));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Reopen with DFACC_READ: version should now be restored!
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    uint32_t fmaj = 0;
    EXPECT_THAT(Hgetfileversion(fid, &fmaj, nullptr, nullptr, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(fmaj, Ge(4u));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }
}

TEST(HfileTest, LowLevelInternalRoutines) {
  const std::string path = GetTempFilePath("hfile_lowlevel.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  filerec_t* file_rec = static_cast<filerec_t*>(HAatom_object(fid));
  ASSERT_THAT(file_rec, NotNull());

  // HPcompare_filerec_path permutations
  EXPECT_THAT(HPcompare_filerec_path(nullptr, nullptr), Eq(FALSE));
  EXPECT_THAT(HPcompare_filerec_path(file_rec, nullptr), Eq(FALSE));
  EXPECT_THAT(HPcompare_filerec_path(nullptr, path.c_str()), Eq(FALSE));
  EXPECT_THAT(HPcompare_filerec_path(file_rec, "wrong_path"), Eq(FALSE));
  EXPECT_THAT(HPcompare_filerec_path(file_rec, path.c_str()), Eq(TRUE));
  filerec_t bad_frec = *file_rec;
  bad_frec.refcount = 0;
  EXPECT_THAT(HPcompare_filerec_path(&bad_frec, path.c_str()), Eq(FALSE));

  // HPcompare_accrec_tagref permutations
  const int32_t aid1 = Hstartwrite(fid, 200, 1, 4);
  ASSERT_THAT(aid1, Ne(FAIL));
  const int32_t aid2 = Hstartwrite(fid, 200, 2, 4);
  ASSERT_THAT(aid2, Ne(FAIL));
  accrec_t* acc1 = static_cast<accrec_t*>(HAatom_object(aid1));
  accrec_t* acc2 = static_cast<accrec_t*>(HAatom_object(aid2));
  ASSERT_THAT(acc1, NotNull());
  ASSERT_THAT(acc2, NotNull());

  // rec1 == rec2 returns FALSE
  EXPECT_THAT(HPcompare_accrec_tagref(acc1, acc1), Eq(FALSE));
  // different refs returns FALSE
  EXPECT_THAT(HPcompare_accrec_tagref(acc1, acc2), Eq(FALSE));
  accrec_t bad_acc = *acc1;
  bad_acc.ddid = FAIL;
  EXPECT_THAT(HPcompare_accrec_tagref(&bad_acc, acc2), Eq(FALSE));
  EXPECT_THAT(HPcompare_accrec_tagref(acc1, &bad_acc), Eq(FALSE));

  EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));
  EXPECT_THAT(Hendaccess(aid1), Eq(SUCCEED));

  // Write an element and open two readers to compare matching tag/ref
  const int32_t wr_aid = Hstartwrite(fid, 201, 1, 4);
  ASSERT_THAT(wr_aid, Ne(FAIL));
  uint8_t dummy_d[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(wr_aid, 4, dummy_d), Eq(4));
  EXPECT_THAT(Hendaccess(wr_aid), Eq(SUCCEED));

  const int32_t raid1 = Hstartread(fid, 201, 1);
  ASSERT_THAT(raid1, Ne(FAIL));
  const int32_t raid2 = Hstartread(fid, 201, 1);
  ASSERT_THAT(raid2, Ne(FAIL));
  accrec_t* racc1 = static_cast<accrec_t*>(HAatom_object(raid1));
  accrec_t* racc2 = static_cast<accrec_t*>(HAatom_object(raid2));
  ASSERT_THAT(racc1, NotNull());
  ASSERT_THAT(racc2, NotNull());
  EXPECT_THAT(HPcompare_accrec_tagref(racc1, racc2), Eq(TRUE));

  // HIgetspinfo on racc1
  EXPECT_THAT(HIgetspinfo(racc1), Eq(nullptr));

  // HDget_special_info / HDset_special_info on non-special element
  sp_info_block_t info = {};
  EXPECT_THAT(HDget_special_info(FAIL, &info), Eq(FAIL));
  EXPECT_THAT(HDget_special_info(raid1, nullptr), Eq(FAIL));
  EXPECT_THAT(HDget_special_info(raid1, &info), Eq(FAIL));
  EXPECT_THAT(info.key, Eq(FAIL));
  EXPECT_THAT(HDset_special_info(FAIL, &info), Eq(FAIL));
  EXPECT_THAT(HDset_special_info(raid1, nullptr), Eq(FAIL));
  EXPECT_THAT(HDset_special_info(raid1, &info), Eq(FAIL));

  EXPECT_THAT(Hendaccess(raid2), Eq(SUCCEED));
  EXPECT_THAT(Hendaccess(raid1), Eq(SUCCEED));

  // HPgetdiskblock & HPfreediskblock
  EXPECT_THAT(HPgetdiskblock(nullptr, 16, FALSE), Eq(FAIL));
  EXPECT_THAT(HPgetdiskblock(file_rec, -1, FALSE), Eq(FAIL));
  EXPECT_THAT(HPgetdiskblock(file_rec, 0, FALSE), Eq(file_rec->f_end_off));
  const int32_t off = HPgetdiskblock(file_rec, 16, TRUE);
  EXPECT_THAT(off, Gt(0));
  EXPECT_THAT(HPfreediskblock(file_rec, off, 16), Eq(SUCCEED));

  // HP_write, HPseek, HP_read operation switching
  uint8_t wdata8[8] = {11, 22, 33, 44, 55, 66, 77, 88};
  EXPECT_THAT(HPseek(file_rec, off), Eq(SUCCEED));
  EXPECT_THAT(HP_write(file_rec, wdata8, 8), Eq(SUCCEED));
  EXPECT_THAT(HPseek(file_rec, off), Eq(SUCCEED));
  EXPECT_THAT(HP_write(file_rec, wdata8, 4), Eq(SUCCEED));
  // Calling HP_read immediately after HP_write triggers operation switch (lines
  // 3162-3165)
  uint8_t rdata[4] = {0};
  EXPECT_THAT(HP_read(file_rec, rdata, 4), Eq(SUCCEED));
  EXPECT_THAT(rdata[0], Eq(55));
  // Calling HP_write immediately after HP_read triggers operation switch (lines
  // 3254-3257)
  EXPECT_THAT(HP_write(file_rec, wdata8, 4), Eq(SUCCEED));

  // HIget_access_rec, HIrelease_accrec_node, Hshutdown
  accrec_t* a = HIget_access_rec();
  ASSERT_THAT(a, NotNull());
  HIrelease_accrec_node(a);
  accrec_t* b = HIget_access_rec();
  ASSERT_THAT(b, Eq(a));
  HIrelease_accrec_node(b);
  EXPECT_THAT(Hshutdown(), Eq(SUCCEED));
}

TEST(HfileTest, CacheAndFileExtend) {
  const std::string path = GetTempFilePath("hfile_cache_extend.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));

  // Enable caching on this file
  EXPECT_THAT(Hcache(fid, 1), Eq(SUCCEED));

  // Allocate space via HPgetdiskblock while caching is active -> marks
  // FILE_END_DIRTY
  filerec_t* file_rec = static_cast<filerec_t*>(HAatom_object(fid));
  ASSERT_THAT(file_rec, NotNull());
  const int32_t off = HPgetdiskblock(file_rec, 64, FALSE);
  EXPECT_THAT(off, Gt(0));

  // Syncing the file triggers HIextend_file!
  EXPECT_THAT(Hsync(fid), Eq(SUCCEED));

  // Turn caching off -> triggers HIsync
  EXPECT_THAT(Hcache(fid, 0), Eq(SUCCEED));

  // CACHE_ALL_FILES
  EXPECT_THAT(Hcache(CACHE_ALL_FILES, 1), Eq(SUCCEED));
  EXPECT_THAT(Hcache(CACHE_ALL_FILES, 0), Eq(SUCCEED));
  EXPECT_THAT(Hcache(CACHE_ALL_FILES, 1), Eq(SUCCEED));

  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
}

TEST(HfileTest, HgetntinfoAllTypes) {
  hdf_ntinfo_t info = {};

  const int32_t types[] = {
      DFNT_UCHAR8, DFNT_CHAR8,   DFNT_FLOAT32, DFNT_FLOAT64, DFNT_FLOAT128,
      DFNT_INT8,   DFNT_UINT8,   DFNT_INT16,   DFNT_UINT16,  DFNT_INT32,
      DFNT_UINT32, DFNT_INT64,   DFNT_UINT64,  DFNT_INT128,  DFNT_UINT128,
      DFNT_CHAR16, DFNT_UCHAR16,
  };

  for (int32_t t : types) {
    EXPECT_THAT(Hgetntinfo(t, &info), Eq(SUCCEED));
    EXPECT_THAT(strlen(info.type_name), Gt(0u));
    EXPECT_THAT(std::string_view(info.byte_order), Eq("bigEndian"));
  }

  // Little-endian
  EXPECT_THAT(Hgetntinfo(DFNT_INT32 | DFNT_LITEND, &info), Eq(SUCCEED));
  EXPECT_THAT(std::string_view(info.type_name), Eq("int32"));
  EXPECT_THAT(std::string_view(info.byte_order), Eq("littleEndian"));

  // Native flag
  EXPECT_THAT(Hgetntinfo(DFNT_FLOAT32 | DFNT_NATIVE, &info), Eq(SUCCEED));
  EXPECT_THAT(std::string_view(info.type_name), Eq("float32"));

  // Unknown numbertype fails
  EXPECT_THAT(Hgetntinfo(99999, &info), Eq(FAIL));
}

TEST(HfileTest, HiCloseStdio) {
  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  EXPECT_THAT(hi_close_stdio(&fp), Eq(SUCCEED));
  EXPECT_THAT(fp, Eq(nullptr));
}

TEST(HfileTest, RegisterTermFuncAndMisc) {
  EXPECT_THAT(HPregister_term_func(DummyTermFunc), Eq(SUCCEED));
  EXPECT_THAT(HDdont_atexit(), Eq(SUCCEED));

  // Hsetaccesstype
  EXPECT_THAT(Hsetaccesstype(FAIL, DFACC_DEFAULT), Eq(FAIL));

  const std::string path = GetTempFilePath("hfile_misc.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid = Hstartwrite(fid, 900, 1, 4);
  ASSERT_THAT(aid, Ne(FAIL));
  EXPECT_THAT(Hsetaccesstype(aid, 9999), Eq(FAIL));
  EXPECT_THAT(Hsetaccesstype(aid, DFACC_DEFAULT), Eq(SUCCEED));
  EXPECT_THAT(Hsetaccesstype(aid, DFACC_SERIAL), Eq(FAIL));
  EXPECT_THAT(Hsetaccesstype(aid, DFACC_PARALLEL), Eq(SUCCEED));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // HDvalidfid
  EXPECT_THAT(HDvalidfid(fid), Eq(TRUE));
  EXPECT_THAT(HDvalidfid(FAIL), Eq(FALSE));
  EXPECT_THAT(HDvalidfid(99999), Eq(FALSE));

  // HDerr closes the file and returns FAIL
  const std::string err_path = GetTempFilePath("hfile_hderr.hdf");
  const int32_t err_fid = Hopen(err_path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(err_fid, Ne(FAIL));
  EXPECT_THAT(HDerr(err_fid), Eq(FAIL));
  EXPECT_THAT(HDvalidfid(err_fid), Eq(FALSE));

  // HPisfile_in_use
  EXPECT_THAT(HPisfile_in_use(nullptr), Eq(FALSE));
  EXPECT_THAT(HPisfile_in_use("non_existent_file"), Eq(FALSE));
}

TEST(HfileTest, AdditionalReachableBranchesAndErrors) {
  // 1. Hopen cannot create file in non-existent directory (lines 353-355)
  EXPECT_THAT(Hopen("/non_existent_dir_12345/test.hdf", DFACC_CREATE, 0),
              Eq(FAIL));

  const std::string path = GetTempFilePath("hfile_branches.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Write an element
  const int32_t wr_aid = Hstartwrite(fid, 901, 1, 4);
  ASSERT_THAT(wr_aid, Ne(FAIL));
  uint8_t data[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(wr_aid, 4, data), Eq(4));
  EXPECT_THAT(Hendaccess(wr_aid), Eq(SUCCEED));

  // Open with DFACC_APPENDABLE flag (lines 869-870)
  const int32_t app_aid =
      Hstartaccess(fid, 902, 1, DFACC_WRITE | DFACC_APPENDABLE);
  ASSERT_THAT(app_aid, Ne(FAIL));
  accrec_t* app_acc = static_cast<accrec_t*>(HAatom_object(app_aid));
  ASSERT_THAT(app_acc, NotNull());
  EXPECT_THAT(app_acc->appendable, Eq(TRUE));
  EXPECT_THAT(Hendaccess(app_aid), Eq(SUCCEED));

  // 2. Hread on new element fails with DFE_READERROR (line 1263)
  const int32_t new_aid = Hstartaccess(fid, 903, 1, DFACC_WRITE);
  ASSERT_THAT(new_aid, Ne(FAIL));
  uint8_t rdata[4] = {0};
  EXPECT_THAT(Hread(new_aid, 4, rdata), Eq(FAIL));

  // Hwrite with bad file_id in accrec (line 1350)
  accrec_t* new_acc = static_cast<accrec_t*>(HAatom_object(new_aid));
  ASSERT_THAT(new_acc, NotNull());
  const int32_t saved_fid = new_acc->file_id;
  new_acc->file_id = FAIL;
  EXPECT_THAT(Hwrite(new_aid, 4, data), Eq(FAIL));
  new_acc->file_id = saved_fid;
  EXPECT_THAT(Hendaccess(new_aid), Eq(SUCCEED));

  // 3. Hread with bad file_rec and bad ddid (lines 1274, 1282)
  const int32_t rd_aid = Hstartread(fid, 901, 1);
  ASSERT_THAT(rd_aid, Ne(FAIL));
  accrec_t* rd_acc = static_cast<accrec_t*>(HAatom_object(rd_aid));
  ASSERT_THAT(rd_acc, NotNull());
  rd_acc->file_id = FAIL;
  EXPECT_THAT(Hread(rd_aid, 4, rdata), Eq(FAIL));
  rd_acc->file_id = saved_fid;

  const atom_t saved_ddid = rd_acc->ddid;
  rd_acc->ddid = FAIL;
  EXPECT_THAT(Hread(rd_aid, 4, rdata), Eq(FAIL));
  rd_acc->ddid = saved_ddid;
  EXPECT_THAT(Hendaccess(rd_aid), Eq(SUCCEED));

  // 4. Open read-only file and try Hstartaccess with DFACC_WRITE (line 860)
  const std::string ro_path = GetTempFilePath("hfile_ro_branch.hdf");
  const int32_t fid_ro_cr = Hopen(ro_path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid_ro_cr, Ne(FAIL));
  EXPECT_THAT(Hclose(fid_ro_cr), Eq(SUCCEED));

  const int32_t fid_ro = Hopen(ro_path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(fid_ro, Ne(FAIL));
  EXPECT_THAT(Hstartaccess(fid_ro, 901, 1, DFACC_WRITE), Eq(FAIL));
  EXPECT_THAT(Hclose(fid_ro), Eq(SUCCEED));
}

}  // namespace
}  // namespace hdf4
