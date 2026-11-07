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

// Tests linked-block special data elements (hdf/src/hblocks.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/hblocks.c
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/test/blocks.c

#include <climits>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#ifdef FAIL
#undef FAIL
#endif
#ifdef SUCCEED
#undef SUCCEED
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/hdf/src/hdf_priv.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"
#include "third_party/hdf4/hdf/src/hproto.h"
// clang-format on

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Ne;

struct BlockRecord {
  uint16_t ref;
};

struct LinkTable {
  uint16_t nextref;
  LinkTable* next;
  BlockRecord* block_list;
};

struct LinkInfo {
  int attached;
  int32_t length;
  int32_t first_length;
  int32_t block_length;
  int32_t number_blocks;
  uint16_t link_ref;
  LinkTable* link;
  LinkTable* last_link;
};

std::string GetTempFilePath(const std::string& filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

TEST(HblocksTest, InvalidArgumentsHLcreate) {
  const std::string path = GetTempFilePath("hblocks_inv_create.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Invalid file ID
  EXPECT_THAT(HLcreate(FAIL, 1000, 1, 64, 4), Eq(FAIL));

  // Negative block length
  EXPECT_THAT(HLcreate(fid, 1000, 1, -1, 4), Eq(FAIL));

  // Negative number of blocks
  EXPECT_THAT(HLcreate(fid, 1000, 1, 64, -1), Eq(FAIL));

  // Special tag not allowed as source tag
  EXPECT_THAT(HLcreate(fid, static_cast<uint16_t>(0x8000 | 1000), 1, 64, 4),
              Eq(FAIL));

  // Create a valid linked block element
  const int32_t aid = HLcreate(fid, 1000, 1, 64, 4);
  ASSERT_THAT(aid, Ne(FAIL));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HblocksTest, ReadOnlyFileHLcreateFails) {
  const std::string path = GetTempFilePath("hblocks_ro_create.hdf");
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  EXPECT_THAT(HLcreate(fid, 1000, 1, 64, 4), Eq(FAIL));
}

TEST(HblocksTest, InvalidArgumentsHLconvert) {
  const std::string path = GetTempFilePath("hblocks_inv_convert.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Invalid AID
  EXPECT_THAT(HLconvert(FAIL, 64, 4), Eq(FAIL));
  EXPECT_THAT(HLconvert(99999, 64, 4), Eq(FAIL));

  // Negative block length (fails before accessing access_rec)
  {
    const int32_t aid = Hstartwrite(fid, 1000, 1, 100);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(HLconvert(aid, -1, 4), Eq(FAIL));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }

  // Negative number of blocks (fails before accessing access_rec)
  {
    const int32_t aid = Hstartwrite(fid, 1000, 2, 100);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(HLconvert(aid, 64, -1), Eq(FAIL));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }

  // Successful conversion
  {
    const int32_t aid = Hstartwrite(fid, 1000, 3, 100);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(HLconvert(aid, 64, 4), Eq(SUCCEED));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }

  // Converting an already-special AID must fail (DFE_CANTMOD).
  // Upstream HLconvert releases access_rec without detaching DD or decrementing
  // attach count, and leaves info->link leaked. Clean up manually.
  const int32_t special_aid = HLcreate(fid, 1000, 4, 64, 4);
  ASSERT_THAT(special_aid, Ne(FAIL));
  auto* arec = static_cast<accrec_t*>(HAatom_object(special_aid));
  ASSERT_THAT(arec, Ne(nullptr));
  auto* info = static_cast<LinkInfo*>(arec->special_info);
  const int32_t ddid = arec->ddid;
  auto* frec = static_cast<filerec_t*>(HAatom_object(fid));
  ASSERT_THAT(frec, Ne(nullptr));

  // Free info->link before HLconvert frees info (to avoid upstream leak)
  if (info != nullptr && info->link != nullptr) {
    free(info->link->block_list);
    free(info->link);
    info->link = nullptr;
  }

  EXPECT_THAT(HLconvert(special_aid, 64, 4), Eq(FAIL));
  HAremove_atom(special_aid);
  HTPendaccess(ddid);
  frec->attach--;
}

TEST(HblocksTest, ReadOnlyAidHLconvertFails) {
  const std::string path = GetTempFilePath("hblocks_ro_convert.hdf");
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    const int32_t aid = Hstartwrite(fid, 1000, 1, 10);
    ASSERT_THAT(aid, Ne(FAIL));
    uint8_t buf[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    EXPECT_THAT(Hwrite(aid, sizeof(buf), buf), Eq(sizeof(buf)));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid = Hstartread(fid, 1000, 1);
  ASSERT_THAT(aid, Ne(FAIL));
  auto* arec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_THAT(arec, Ne(nullptr));
  const int32_t ddid = arec->ddid;
  auto* frec = static_cast<filerec_t*>(HAatom_object(fid));
  ASSERT_THAT(frec, Ne(nullptr));

  // Cannot convert read-only AID
  EXPECT_THAT(HLconvert(aid, 64, 4), Eq(FAIL));
  HAremove_atom(aid);
  HTPendaccess(ddid);
  frec->attach--;
}

TEST(HblocksTest, BlockInfoGetSetInq) {
  const std::string path = GetTempFilePath("hblocks_info.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Invalid AID queries
  int32_t len = 0;
  int32_t flen = 0;
  int32_t blen = 0;
  int32_t nblocks = 0;
  EXPECT_THAT(HDinqblockinfo(FAIL, &len, &flen, &blen, &nblocks), Eq(FAIL));
  EXPECT_THAT(HLgetblockinfo(FAIL, &blen, &nblocks), Eq(FAIL));
  EXPECT_THAT(HLsetblockinfo(FAIL, 64, 4), Eq(FAIL));

  // Non-special AID queries
  const int32_t reg_aid = Hstartwrite(fid, 2000, 1, 50);
  ASSERT_THAT(reg_aid, Ne(FAIL));
  absl::Cleanup close_reg = [reg_aid] {
    EXPECT_THAT(Hendaccess(reg_aid), Eq(SUCCEED));
  };

  EXPECT_THAT(HDinqblockinfo(reg_aid, &len, &flen, &blen, &nblocks), Eq(FAIL));

  // HLsetblockinfo invalid args on regular AID
  EXPECT_THAT(HLsetblockinfo(reg_aid, 0, 4), Eq(FAIL));
  EXPECT_THAT(HLsetblockinfo(reg_aid, -2, 4), Eq(FAIL));
  EXPECT_THAT(HLsetblockinfo(reg_aid, 64, 0), Eq(FAIL));
  EXPECT_THAT(HLsetblockinfo(reg_aid, 64, -2), Eq(FAIL));

  // HLsetblockinfo valid modifications on regular AID
  EXPECT_THAT(HLsetblockinfo(reg_aid, 128, 8), Eq(SUCCEED));
  int32_t query_bs = 0;
  int32_t query_nb = 0;
  EXPECT_THAT(HLgetblockinfo(reg_aid, &query_bs, &query_nb), Eq(SUCCEED));
  EXPECT_THAT(query_bs, Eq(128));
  EXPECT_THAT(query_nb, Eq(8));

  // Partial updates with -1 (no change)
  EXPECT_THAT(HLsetblockinfo(reg_aid, 256, -1), Eq(SUCCEED));
  EXPECT_THAT(HLgetblockinfo(reg_aid, &query_bs, nullptr), Eq(SUCCEED));
  EXPECT_THAT(query_bs, Eq(256));
  EXPECT_THAT(HLgetblockinfo(reg_aid, nullptr, &query_nb), Eq(SUCCEED));
  EXPECT_THAT(query_nb, Eq(8));

  EXPECT_THAT(HLsetblockinfo(reg_aid, -1, 16), Eq(SUCCEED));
  EXPECT_THAT(HLgetblockinfo(reg_aid, &query_bs, &query_nb), Eq(SUCCEED));
  EXPECT_THAT(query_bs, Eq(256));
  EXPECT_THAT(query_nb, Eq(16));

  // Create linked-block element
  const int32_t link_aid = HLcreate(fid, 2000, 2, 64, 4);
  ASSERT_THAT(link_aid, Ne(FAIL));
  absl::Cleanup close_link = [link_aid] {
    EXPECT_THAT(Hendaccess(link_aid), Eq(SUCCEED));
  };

  // HDinqblockinfo with non-null and null pointers
  EXPECT_THAT(HDinqblockinfo(link_aid, &len, &flen, &blen, &nblocks),
              Eq(SUCCEED));
  EXPECT_THAT(len, Eq(0));
  EXPECT_THAT(flen, Eq(64));
  EXPECT_THAT(blen, Eq(64));
  EXPECT_THAT(nblocks, Eq(4));

  EXPECT_THAT(HDinqblockinfo(link_aid, nullptr, nullptr, nullptr, nullptr),
              Eq(SUCCEED));

  // HLsetblockinfo on linked element does not alter info
  EXPECT_THAT(HLsetblockinfo(link_aid, 512, 32), Eq(SUCCEED));
}

TEST(HblocksTest, BasicCreateWriteReadInquire) {
  const std::string path = GetTempFilePath("hblocks_basic.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid = HLcreate(fid, 1000, 1, 32, 2);
  ASSERT_THAT(aid, Ne(FAIL));

  // Write 20 bytes
  std::vector<uint8_t> wdata(20);
  for (size_t i = 0; i < wdata.size(); ++i) {
    wdata[i] = static_cast<uint8_t>(i + 1);
  }
  EXPECT_THAT(Hwrite(aid, wdata.size(), wdata.data()), Eq(wdata.size()));

  // Inquire on active write AID
  int32_t q_fid = 0;
  uint16_t q_tag = 0;
  uint16_t q_ref = 0;
  int32_t q_len = 0;
  int32_t q_off = -1;
  int32_t q_pos = -1;
  int16_t q_acc = 0;
  int16_t q_sp = 0;
  EXPECT_THAT(Hinquire(aid, &q_fid, &q_tag, &q_ref, &q_len, &q_off, &q_pos,
                       &q_acc, &q_sp),
              Eq(SUCCEED));
  EXPECT_THAT(q_fid, Eq(fid));
  EXPECT_THAT(q_len, Eq(20));
  EXPECT_THAT(q_off, Eq(0));
  EXPECT_THAT(q_pos, Eq(20));
  EXPECT_THAT(q_sp, Eq(SPECIAL_LINKED));

  // Hinquire with all null pointers
  EXPECT_THAT(Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr),
              Eq(SUCCEED));

  // HLPinfo direct check
  auto* arec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_THAT(arec, Ne(nullptr));
  sp_info_block_t info_blk{};
  EXPECT_THAT(HLPinfo(arec, &info_blk), Eq(SUCCEED));
  EXPECT_THAT(info_blk.key, Eq(SPECIAL_LINKED));
  EXPECT_THAT(info_blk.first_len, Eq(32));
  EXPECT_THAT(info_blk.block_len, Eq(32));
  EXPECT_THAT(info_blk.nblocks, Eq(2));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Reopen for reading
  const int32_t raid = Hstartread(fid, 1000, 1);
  ASSERT_THAT(raid, Ne(FAIL));
  absl::Cleanup close_raid = [raid] {
    EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));
  };

  // Length 0 reads entire element
  std::vector<uint8_t> rdata(20, 0);
  EXPECT_THAT(Hread(raid, 0, rdata.data()), Eq(20));
  EXPECT_THAT(rdata, Eq(wdata));
}

TEST(HblocksTest, MultiBlockAndMultiTableSpanning) {
  const std::string path = GetTempFilePath("hblocks_multitable.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));

  // Block length = 16, 2 blocks per table (32 bytes per table).
  // Writing 100 bytes will span across 4 linked block tables!
  const int32_t aid = HLcreate(fid, 1000, 1, 16, 2);
  ASSERT_THAT(aid, Ne(FAIL));

  std::vector<uint8_t> wdata(100);
  for (size_t i = 0; i < wdata.size(); ++i) {
    wdata[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
  }

  // Write in chunks aligned to block boundaries for intermediate blocks:
  // 32 bytes (blocks 0 & 1), 32 bytes (blocks 2 & 3), 36 bytes (blocks 4, 5, &
  // part of 6)
  EXPECT_THAT(Hwrite(aid, 32, wdata.data()), Eq(32));
  EXPECT_THAT(Hwrite(aid, 32, wdata.data() + 32), Eq(32));
  EXPECT_THAT(Hwrite(aid, 36, wdata.data() + 64), Eq(36));

  // Overwrite existing data across table boundaries (exercises lines 1267-1268
  // and 1362-1363)
  EXPECT_THAT(Hseek(aid, 16, DF_START), Eq(SUCCEED));
  std::vector<uint8_t> owdata(32, 0xEE);
  EXPECT_THAT(Hwrite(aid, 32, owdata.data()), Eq(32));
  std::fill(wdata.begin() + 16, wdata.begin() + 48, 0xEE);

  // Overwrite with seeking directly into Table 1 (exercises lines 1267-1268)
  EXPECT_THAT(Hseek(aid, 32, DF_START), Eq(SUCCEED));
  std::vector<uint8_t> owdata2(16, 0xDD);
  EXPECT_THAT(Hwrite(aid, 16, owdata2.data()), Eq(16));
  std::fill(wdata.begin() + 32, wdata.begin() + 48, 0xDD);

  // Invalid write lengths
  EXPECT_THAT(Hwrite(aid, 0, wdata.data()), Eq(FAIL));
  EXPECT_THAT(Hwrite(aid, -5, wdata.data()), Eq(FAIL));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Reopen and read back in small irregular chunks across boundaries
  const int32_t raid = Hstartread(fid, 1000, 1);
  ASSERT_THAT(raid, Ne(FAIL));

  std::vector<uint8_t> rdata(100, 0);
  // Read first 25 bytes
  EXPECT_THAT(Hread(raid, 25, rdata.data()), Eq(25));
  // Read next 50 bytes (spanning table boundaries)
  EXPECT_THAT(Hread(raid, 50, rdata.data() + 25), Eq(50));
  // Read remaining 25 bytes
  EXPECT_THAT(Hread(raid, 25, rdata.data() + 75), Eq(25));
  EXPECT_THAT(rdata, Eq(wdata));

  // Negative read length
  EXPECT_THAT(Hread(raid, -1, rdata.data()), Eq(FAIL));

  EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
}

TEST(HblocksTest, SparseBlockAllocationAndZeroFill) {
  const std::string path = GetTempFilePath("hblocks_sparse.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Block length = 16, 2 blocks per table
  const int32_t aid = HLcreate(fid, 1000, 1, 16, 2);
  ASSERT_THAT(aid, Ne(FAIL));

  // Write block 0 (16 bytes)
  std::vector<uint8_t> b0(16, 0xAA);
  EXPECT_THAT(Hwrite(aid, 16, b0.data()), Eq(16));

  // Seek to offset 48, skipping block 1 (table 0) and block 2 (table 1),
  // landing in block 3 (table 1)
  EXPECT_THAT(Hseek(aid, 48, DF_START), Eq(SUCCEED));

  // Write 10 bytes in block 3
  std::vector<uint8_t> b3(10, 0xBB);
  EXPECT_THAT(Hwrite(aid, 10, b3.data()), Eq(10));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Read back all 58 bytes: blocks 1 and 2 must be zero-filled
  const int32_t raid = Hstartread(fid, 1000, 1);
  ASSERT_THAT(raid, Ne(FAIL));

  std::vector<uint8_t> rdata(58, 0xFF);
  EXPECT_THAT(Hread(raid, 58, rdata.data()), Eq(58));

  for (int i = 0; i < 16; ++i) {
    EXPECT_THAT(rdata[i], Eq(0xAA));
  }
  for (int i = 16; i < 48; ++i) {
    EXPECT_THAT(rdata[i], Eq(0x00));
  }
  for (int i = 48; i < 58; ++i) {
    EXPECT_THAT(rdata[i], Eq(0xBB));
  }
  EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));

  // Fill in the sparse gap in block 1: write at offset 16 (block 1 boundary)
  // Fill full block (16 bytes) so intermediate block length is complete
  const int32_t waid = Hstartwrite(fid, 1000, 1, 0);
  ASSERT_THAT(waid, Ne(FAIL));
  EXPECT_THAT(Hseek(waid, 16, DF_START), Eq(SUCCEED));
  std::vector<uint8_t> gap_data(16, 0xCC);
  EXPECT_THAT(Hwrite(waid, 16, gap_data.data()), Eq(16));
  EXPECT_THAT(Hendaccess(waid), Eq(SUCCEED));

  // Verify the gap was filled without altering other blocks
  const int32_t raid2 = Hstartread(fid, 1000, 1);
  ASSERT_THAT(raid2, Ne(FAIL));
  EXPECT_THAT(Hread(raid2, 58, rdata.data()), Eq(58));
  for (int i = 0; i < 16; ++i) {
    EXPECT_THAT(rdata[i], Eq(0xAA));
  }
  for (int i = 16; i < 32; ++i) {
    EXPECT_THAT(rdata[i], Eq(0xCC));
  }
  for (int i = 32; i < 48; ++i) {
    EXPECT_THAT(rdata[i], Eq(0x00));
  }
  for (int i = 48; i < 58; ++i) {
    EXPECT_THAT(rdata[i], Eq(0xBB));
  }
  EXPECT_THAT(Hendaccess(raid2), Eq(SUCCEED));
}

TEST(HblocksTest, SeekOriginsAndBoundaries) {
  const std::string path = GetTempFilePath("hblocks_seek.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid = HLcreate(fid, 1000, 1, 32, 2);
  ASSERT_THAT(aid, Ne(FAIL));

  std::vector<uint8_t> wdata(40, 0x11);
  EXPECT_THAT(Hwrite(aid, 40, wdata.data()), Eq(40));

  // DF_START
  EXPECT_THAT(Hseek(aid, 10, DF_START), Eq(SUCCEED));
  int32_t pos = 0;
  EXPECT_THAT(Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr, &pos,
                       nullptr, nullptr),
              Eq(SUCCEED));
  EXPECT_THAT(pos, Eq(10));

  // DF_CURRENT positive
  EXPECT_THAT(Hseek(aid, 5, DF_CURRENT), Eq(SUCCEED));
  EXPECT_THAT(Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr, &pos,
                       nullptr, nullptr),
              Eq(SUCCEED));
  EXPECT_THAT(pos, Eq(15));

  // DF_CURRENT negative
  EXPECT_THAT(Hseek(aid, -3, DF_CURRENT), Eq(SUCCEED));
  EXPECT_THAT(Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr, &pos,
                       nullptr, nullptr),
              Eq(SUCCEED));
  EXPECT_THAT(pos, Eq(12));

  // DF_END (length is 40)
  EXPECT_THAT(Hseek(aid, 0, DF_END), Eq(SUCCEED));
  EXPECT_THAT(Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr, &pos,
                       nullptr, nullptr),
              Eq(SUCCEED));
  EXPECT_THAT(pos, Eq(40));

  // DF_END negative offset
  EXPECT_THAT(Hseek(aid, -8, DF_END), Eq(SUCCEED));
  EXPECT_THAT(Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr, &pos,
                       nullptr, nullptr),
              Eq(SUCCEED));
  EXPECT_THAT(pos, Eq(32));

  // Negative offsets beyond start must fail (DFE_RANGE)
  EXPECT_THAT(Hseek(aid, -1, DF_START), Eq(FAIL));
  EXPECT_THAT(Hseek(aid, -50, DF_CURRENT), Eq(FAIL));
  EXPECT_THAT(Hseek(aid, -50, DF_END), Eq(FAIL));

  // Read clamping when posn + length > total_length (exercises HLPread line
  // 1096)
  EXPECT_THAT(Hseek(aid, 30, DF_START), Eq(SUCCEED));
  uint8_t clamp_buf[20] = {0};
  EXPECT_THAT(Hread(aid, 20, clamp_buf), Eq(10));
  for (int i = 0; i < 10; ++i) {
    EXPECT_THAT(clamp_buf[i], Eq(0x11));
  }

  // Seeking past end is allowed
  EXPECT_THAT(Hseek(aid, 100, DF_START), Eq(SUCCEED));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HblocksTest, MultipleAIDsAndSharedSpecialInfo) {
  const std::string path = GetTempFilePath("hblocks_shared_aid.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid = HLcreate(fid, 1000, 1, 32, 2);
  ASSERT_THAT(aid, Ne(FAIL));
  std::vector<uint8_t> wdata = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_THAT(Hwrite(aid, wdata.size(), wdata.data()), Eq(wdata.size()));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Open first read AID
  const int32_t raid1 = Hstartread(fid, 1000, 1);
  ASSERT_THAT(raid1, Ne(FAIL));

  // Open second read AID on the exact same tag/ref:
  // Exercises HIgetspinfo reuse and attached++ in HLIstaccess!
  const int32_t raid2 = Hstartread(fid, 1000, 1);
  ASSERT_THAT(raid2, Ne(FAIL));

  // Close raid1:
  // Exercises HLPcloseAID with attached decrementing to 1 (not freed yet).
  EXPECT_THAT(Hendaccess(raid1), Eq(SUCCEED));

  // raid2 is still valid and readable
  std::vector<uint8_t> rdata(8, 0);
  EXPECT_THAT(Hread(raid2, 8, rdata.data()), Eq(8));
  EXPECT_THAT(rdata, Eq(wdata));

  // Close raid2:
  // Exercises HLPcloseAID with attached reaching 0 and freeing all structures.
  EXPECT_THAT(Hendaccess(raid2), Eq(SUCCEED));
}

TEST(HblocksTest, HnextreadReusesAIDAndClosesOldSpecial) {
  const std::string path = GetTempFilePath("hblocks_nextread.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Write two linked block elements
  const int32_t aid1 = HLcreate(fid, 1000, 1, 32, 2);
  ASSERT_THAT(aid1, Ne(FAIL));
  uint8_t d1[4] = {1, 1, 1, 1};
  EXPECT_THAT(Hwrite(aid1, 4, d1), Eq(4));
  EXPECT_THAT(Hendaccess(aid1), Eq(SUCCEED));

  const int32_t aid2 = HLcreate(fid, 1000, 2, 32, 2);
  ASSERT_THAT(aid2, Ne(FAIL));
  uint8_t d2[4] = {2, 2, 2, 2};
  EXPECT_THAT(Hwrite(aid2, 4, d2), Eq(4));
  EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));

  // Open read on first element
  const int32_t raid = Hstartread(fid, 1000, 1);
  ASSERT_THAT(raid, Ne(FAIL));
  absl::Cleanup close_raid = [raid] {
    EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));
  };

  // Move to next element using Hnextread: reuses raid and calls HLPcloseAID
  EXPECT_THAT(Hnextread(raid, 1000, DFREF_WILDCARD, DF_CURRENT), Eq(SUCCEED));

  uint16_t tag = 0;
  uint16_t ref = 0;
  int16_t sp = 0;
  EXPECT_THAT(Hinquire(raid, nullptr, &tag, &ref, nullptr, nullptr, nullptr,
                       nullptr, &sp),
              Eq(SUCCEED));
  EXPECT_THAT(BASETAG(tag), Eq(1000));
  EXPECT_THAT(ref, Eq(2));
  EXPECT_THAT(sp, Eq(SPECIAL_LINKED));

  uint8_t rbuf[4] = {0};
  EXPECT_THAT(Hread(raid, 4, rbuf), Eq(4));
  EXPECT_THAT(std::memcmp(rbuf, d2, 4), Eq(0));
}

TEST(HblocksTest, ConvertExistingDataViaHLcreate) {
  const std::string path = GetTempFilePath("hblocks_convert_exist.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Write a normal element with 15 bytes
  const std::string text = "InitialData1234";
  EXPECT_THAT(
      Hputelement(fid, 1100, 1, reinterpret_cast<const uint8_t*>(text.data()),
                  text.size()),
      Eq(static_cast<int32_t>(text.size())));

  // Promote existing element to linked blocks via HLcreate
  const int32_t aid = HLcreate(fid, 1100, 1, 32, 2);
  ASSERT_THAT(aid, Ne(FAIL));

  int32_t len = 0;
  int32_t flen = 0;
  int32_t blen = 0;
  int32_t nblocks = 0;
  EXPECT_THAT(HDinqblockinfo(aid, &len, &flen, &blen, &nblocks), Eq(SUCCEED));
  EXPECT_THAT(len, Eq(15));
  EXPECT_THAT(flen, Eq(15));
  EXPECT_THAT(blen, Eq(32));
  EXPECT_THAT(nblocks, Eq(2));

  // Seek to end and append 10 more bytes
  EXPECT_THAT(Hseek(aid, 0, DF_END), Eq(SUCCEED));
  const std::string more = "Appended99";
  EXPECT_THAT(
      Hwrite(aid, more.size(), reinterpret_cast<const uint8_t*>(more.data())),
      Eq(more.size()));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Verify full content
  const int32_t raid = Hstartread(fid, 1100, 1);
  ASSERT_THAT(raid, Ne(FAIL));
  absl::Cleanup close_raid = [raid] {
    EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));
  };

  std::vector<uint8_t> rdata(25, 0);
  EXPECT_THAT(Hread(raid, 25, rdata.data()), Eq(25));
  const std::string expected = text + more;
  EXPECT_THAT(std::memcmp(rdata.data(), expected.data(), 25), Eq(0));
}

TEST(HblocksTest, ConvertEmptyElementViaHLcreate) {
  const std::string path = GetTempFilePath("hblocks_convert_empty.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Create an unwritten element (has DD entry but INVALID_OFFSET / length)
  const int32_t waid = Hstartaccess(fid, 1200, 1, DFACC_WRITE);
  ASSERT_THAT(waid, Ne(FAIL));
  EXPECT_THAT(Hendaccess(waid), Eq(SUCCEED));

  // HLcreate on this empty DD entry exercises HTPdelete and data_id = FAIL
  // branch
  const int32_t aid = HLcreate(fid, 1200, 1, 32, 2);
  ASSERT_THAT(aid, Ne(FAIL));

  int32_t len = 0;
  int32_t flen = 0;
  EXPECT_THAT(HDinqblockinfo(aid, &len, &flen, nullptr, nullptr), Eq(SUCCEED));
  EXPECT_THAT(len, Eq(0));
  EXPECT_THAT(flen, Eq(32));

  uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_THAT(Hwrite(aid, sizeof(data), data), Eq(sizeof(data)));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  const int32_t raid = Hstartread(fid, 1200, 1);
  ASSERT_THAT(raid, Ne(FAIL));
  absl::Cleanup close_raid = [raid] {
    EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));
  };
  uint8_t rdata[10] = {0};
  EXPECT_THAT(Hread(raid, 10, rdata), Eq(10));
  EXPECT_THAT(std::memcmp(rdata, data, 10), Eq(0));
}

TEST(HblocksTest, ConvertViaHLconvertWithSeeking) {
  const std::string path = GetTempFilePath("hblocks_hlconvert_seek.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid = Hstartwrite(fid, 1300, 1, 50);
  ASSERT_THAT(aid, Ne(FAIL));

  // Write 20 bytes
  std::vector<uint8_t> d1(20, 0x12);
  EXPECT_THAT(Hwrite(aid, 20, d1.data()), Eq(20));

  // Seek to offset 10 (old_posn > 0)
  EXPECT_THAT(Hseek(aid, 10, DF_START), Eq(SUCCEED));

  // HLconvert with old_posn > 0 exercises Hseek(aid, old_posn, DF_START)
  EXPECT_THAT(HLconvert(aid, 16, 2), Eq(SUCCEED));

  int32_t pos = 0;
  EXPECT_THAT(Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr, &pos,
                       nullptr, nullptr),
              Eq(SUCCEED));
  EXPECT_THAT(pos, Eq(10));

  // Write 15 bytes starting at offset 10
  std::vector<uint8_t> d2(15, 0x34);
  EXPECT_THAT(Hwrite(aid, 15, d2.data()), Eq(15));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Read back all 25 bytes
  const int32_t raid = Hstartread(fid, 1300, 1);
  ASSERT_THAT(raid, Ne(FAIL));
  absl::Cleanup close_raid = [raid] {
    EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));
  };

  std::vector<uint8_t> rdata(25, 0);
  EXPECT_THAT(Hread(raid, 25, rdata.data()), Eq(25));
  for (int i = 0; i < 10; ++i) {
    EXPECT_THAT(rdata[i], Eq(0x12));
  }
  for (int i = 10; i < 25; ++i) {
    EXPECT_THAT(rdata[i], Eq(0x34));
  }
}

TEST(HblocksTest, ConvertEmptyElementViaHLconvert) {
  const std::string path = GetTempFilePath("hblocks_hlconvert_empty.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Unwritten element with INVALID_OFFSET and INVALID_LENGTH via Hstartaccess
  const int32_t aid = Hstartaccess(fid, 1300, 2, DFACC_WRITE);
  ASSERT_THAT(aid, Ne(FAIL));

  // HLconvert exercises Hsetlength(aid, 0) and HTPinquire
  EXPECT_THAT(HLconvert(aid, 32, 2), Eq(SUCCEED));

  // Write data
  uint8_t data[8] = {9, 8, 7, 6, 5, 4, 3, 2};
  EXPECT_THAT(Hwrite(aid, sizeof(data), data), Eq(sizeof(data)));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  const int32_t raid = Hstartread(fid, 1300, 2);
  ASSERT_THAT(raid, Ne(FAIL));
  absl::Cleanup close_raid = [raid] {
    EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));
  };
  uint8_t rdata[8] = {0};
  EXPECT_THAT(Hread(raid, 8, rdata), Eq(8));
  EXPECT_THAT(std::memcmp(rdata, data, 8), Eq(0));
}

TEST(HblocksTest, HLgetdatainfo) {
  const std::string path = GetTempFilePath("hblocks_datainfo.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Create linked element: block_length = 20, 2 blocks per table.
  // Writing 45 bytes creates 3 data blocks across 2 tables:
  // Table 0: Block 0 (20B), Block 1 (20B) -> next_ref points to Table 1
  // Table 1: Block 2 (5B), Block 3 (0B, unused) -> next_ref = 0
  const int32_t aid = HLcreate(fid, 1400, 1, 20, 2);
  ASSERT_THAT(aid, Ne(FAIL));

  std::vector<uint8_t> wdata(45, 0x5A);
  EXPECT_THAT(Hwrite(aid, 45, wdata.data()), Eq(45));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Read the 16-byte special header element directly using Hstartaccess
  // (Hstartread strips the special bit via BASETAG and would read element data)
  const uint16_t special_tag = static_cast<uint16_t>(1400 | 0x4000);
  uint8_t header16[16] = {0};
  const int32_t h_aid = Hstartaccess(fid, special_tag, 1, DFACC_READ);
  ASSERT_THAT(h_aid, Ne(FAIL));
  EXPECT_THAT(Hread(h_aid, 16, header16), Eq(16));
  EXPECT_THAT(Hendaccess(h_aid), Eq(SUCCEED));

  // The 14 bytes after SPECIAL_LINKED (offset 2) are passed to HLgetdatainfo
  uint8_t* special_buf = &header16[2];

  // Test argument error: info_count == 0 while arrays are non-null
  int32_t offsets[10] = {0};
  int32_t lengths[10] = {0};
  EXPECT_THAT(HLgetdatainfo(fid, special_buf, 0, 0, offsets, lengths),
              Eq(FAIL));

  // Query all data blocks
  int count = HLgetdatainfo(fid, special_buf, 0, 10, offsets, lengths);
  EXPECT_THAT(count, Eq(3));
  EXPECT_THAT(lengths[0], Eq(20));
  EXPECT_THAT(lengths[1], Eq(20));
  EXPECT_THAT(lengths[2], Eq(5));
  for (int i = 0; i < 3; ++i) {
    EXPECT_THAT(offsets[i], Gt(0));
  }

  // Query with offsetarray == nullptr
  int32_t only_lengths[10] = {0};
  count = HLgetdatainfo(fid, special_buf, 0, 10, nullptr, only_lengths);
  EXPECT_THAT(count, Eq(3));
  EXPECT_THAT(only_lengths[2], Eq(5));

  // Query with lengtharray == nullptr
  int32_t only_offsets[10] = {0};
  count = HLgetdatainfo(fid, special_buf, 0, 10, only_offsets, nullptr);
  EXPECT_THAT(count, Eq(3));

  // Query with both null and info_count == 0 to count data blocks
  count = HLgetdatainfo(fid, special_buf, 0, 0, nullptr, nullptr);
  EXPECT_THAT(count, Eq(3));

  // Single table with multiple blocks having data (exercises line 842
  // accum_length)
  const int32_t aid2 = HLcreate(fid, 1500, 1, 20, 3);
  ASSERT_THAT(aid2, Ne(FAIL));
  std::vector<uint8_t> wdata2(55, 0x33);
  EXPECT_THAT(Hwrite(aid2, 55, wdata2.data()), Eq(55));
  EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));

  const uint16_t sp_tag2 = MKSPECIALTAG(1500);
  uint8_t hdr2[16] = {0};
  const int32_t h2_aid = Hstartaccess(fid, sp_tag2, 1, DFACC_READ);
  ASSERT_THAT(h2_aid, Ne(FAIL));
  EXPECT_THAT(Hread(h2_aid, 16, hdr2), Eq(16));
  EXPECT_THAT(Hendaccess(h2_aid), Eq(SUCCEED));

  int32_t off2[3] = {0};
  int32_t len2[3] = {0};
  EXPECT_THAT(HLgetdatainfo(fid, &hdr2[2], 0, 3, off2, len2), Eq(3));
  EXPECT_THAT(len2[0], Eq(20));
  EXPECT_THAT(len2[1], Eq(20));
  EXPECT_THAT(len2[2], Eq(15));

  // Invalid link_ref in header buffer triggers HLIgetlink read failure cleanup
  uint8_t bad_hdr[14] = {0};
  uint8_t* p = bad_hdr;
  INT32ENCODE(p, 40);
  INT32ENCODE(p, 20);
  INT32ENCODE(p, 2);
  UINT16ENCODE(p, 9999);
  EXPECT_THAT(HLgetdatainfo(fid, bad_hdr, 0, 2, off2, len2), Eq(FAIL));
}

TEST(HblocksTest, DirectCallbacksErrorHandling) {
  // HLPendaccess with nullptr
  EXPECT_THAT(HLPendaccess(nullptr), Eq(FAIL));

  const std::string path = GetTempFilePath("hblocks_direct_cb.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // Create a regular element access record
  const int32_t reg_aid = Hstartwrite(fid, 1600, 1, 10);
  ASSERT_THAT(reg_aid, Ne(FAIL));
  absl::Cleanup close_reg = [reg_aid] {
    EXPECT_THAT(Hendaccess(reg_aid), Eq(SUCCEED));
  };
  auto* reg_arec = static_cast<accrec_t*>(HAatom_object(reg_aid));
  ASSERT_THAT(reg_arec, Ne(nullptr));

  // Calling HLPseek directly on a non-special access record must fail
  EXPECT_THAT(HLPseek(reg_arec, 0, DF_START), Eq(FAIL));

  // Calling HLPinfo directly on a non-special access record must fail
  sp_info_block_t info_blk{};
  EXPECT_THAT(HLPinfo(reg_arec, &info_blk), Eq(FAIL));

  // HDinqblockinfo on a non-special access record must fail (DFE_ARGS)
  int32_t inq_len = 0;
  EXPECT_THAT(HDinqblockinfo(reg_aid, &inq_len, nullptr, nullptr, nullptr),
              Eq(FAIL));
  EXPECT_THAT(HDinqblockinfo(FAIL, &inq_len, nullptr, nullptr, nullptr),
              Eq(FAIL));

  // Create a linked element
  const int32_t sp_aid = HLcreate(fid, 1700, 1, 32, 2);
  ASSERT_THAT(sp_aid, Ne(FAIL));
  absl::Cleanup close_sp = [sp_aid] {
    EXPECT_THAT(Hendaccess(sp_aid), Eq(SUCCEED));
  };

  // HDget_special_info on a linked block element exercises HLPinfo
  sp_info_block_t sp_info{};
  EXPECT_THAT(HDget_special_info(sp_aid, &sp_info), Eq(SUCCEED));
  EXPECT_THAT(sp_info.key, Eq(SPECIAL_LINKED));
  EXPECT_THAT(sp_info.block_len, Eq(32));
  EXPECT_THAT(sp_info.nblocks, Eq(2));

  // Invalid arguments to HDget_special_info
  EXPECT_THAT(HDget_special_info(FAIL, &sp_info), Eq(FAIL));
  EXPECT_THAT(HDget_special_info(sp_aid, nullptr), Eq(FAIL));

  // Hinquire with all non-null parameters exercises lines 1522-1537 in
  // HLPinquire
  int32_t inq_fid = 0;
  uint16_t inq_tag = 0;
  uint16_t inq_ref = 0;
  int32_t inq_length = 0;
  int32_t inq_offset = -1;
  int32_t inq_posn = -1;
  int16_t inq_access = 0;
  int16_t inq_special = 0;
  EXPECT_THAT(Hinquire(sp_aid, &inq_fid, &inq_tag, &inq_ref, &inq_length,
                       &inq_offset, &inq_posn, &inq_access, &inq_special),
              Eq(SUCCEED));
  EXPECT_THAT(inq_fid, Eq(fid));
  EXPECT_THAT(inq_offset, Eq(0));
  EXPECT_THAT(inq_special, Eq(SPECIAL_LINKED));

  // HLPinquire with invalid DD ID fails with DFE_INTERNAL
  accrec_t bad_dd_arec = *reg_arec;
  bad_dd_arec.ddid = FAIL;
  EXPECT_THAT(HLPinquire(&bad_dd_arec, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
              Eq(FAIL));

  // HLPendaccess on invalid file record fails with DFE_ARGS
  accrec_t* bad_frec_arec = HIget_access_rec();
  ASSERT_THAT(bad_frec_arec, Ne(nullptr));
  *bad_frec_arec = *reg_arec;
  bad_frec_arec->file_id = 99999;
  EXPECT_THAT(HLPendaccess(bad_frec_arec), Eq(FAIL));

  // Direct HLPstread call when special_info already exists (exercises lines
  // 620-638)
  {
    accrec_t* test_arec = HIget_access_rec();
    ASSERT_THAT(test_arec, Ne(nullptr));
    *test_arec = *reg_arec;
    test_arec->file_id = fid;
    auto* old_info = static_cast<LinkInfo*>(malloc(sizeof(LinkInfo)));
    ASSERT_THAT(old_info, Ne(nullptr));
    old_info->attached = 1;
    old_info->link = static_cast<LinkTable*>(malloc(sizeof(LinkTable)));
    ASSERT_THAT(old_info->link, Ne(nullptr));
    old_info->link->next = nullptr;
    old_info->link->block_list =
        static_cast<BlockRecord*>(malloc(sizeof(BlockRecord)));
    ASSERT_THAT(old_info->link->block_list, Ne(nullptr));
    test_arec->special_info = old_info;

    // HLPstread with BAD file record after freeing special_info
    test_arec->ddid = FAIL;
    // HLPstread will free old_info and then fail at HTPinquire
    EXPECT_THAT(HLPstread(test_arec), Eq(FAIL));
    HIrelease_accrec_node(test_arec);
  }
}

TEST(HblocksTest, AtomAndTableIntegrity) {
  const std::string path = GetTempFilePath("hblocks_atom_integrity.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // HLconvert with null atom object (line 407), HLsetblockinfo (line 1734),
  // HLgetblockinfo (line 1791)
  const int32_t null_aid = HAregister_atom(AIDGROUP, nullptr);
  ASSERT_THAT(null_aid, Ne(FAIL));
  EXPECT_THAT(HLconvert(null_aid, 64, 4), Eq(FAIL));
  EXPECT_THAT(HLsetblockinfo(null_aid, 64, 4), Eq(FAIL));
  EXPECT_THAT(HLgetblockinfo(null_aid, nullptr, nullptr), Eq(FAIL));
  EXPECT_THAT(HAremove_atom(null_aid), Eq(nullptr));

  // HLconvert with bad file record (line 412)
  accrec_t* bad_frec = HIget_access_rec();
  ASSERT_THAT(bad_frec, Ne(nullptr));
  bad_frec->file_id = 99999;
  const int32_t bad_frec_aid = HAregister_atom(AIDGROUP, bad_frec);
  ASSERT_THAT(bad_frec_aid, Ne(FAIL));
  // HLconvert calls HIrelease_accrec_node on failure
  EXPECT_THAT(HLconvert(bad_frec_aid, 64, 4), Eq(FAIL));
  EXPECT_THAT(HAremove_atom(bad_frec_aid), Ne(nullptr));

  // HLsetblockinfo error branches
  EXPECT_THAT(HLsetblockinfo(FAIL, 64, 4), Eq(FAIL));
  EXPECT_THAT(HLgetblockinfo(FAIL, nullptr, nullptr), Eq(FAIL));

  // Test HLcreate when element is already special (exercises lines 232-234)
  {
    auto* frec = static_cast<filerec_t*>(HAatom_object(fid));
    ASSERT_THAT(frec, Ne(nullptr));
    const int32_t ddid = HTPcreate(frec, 2100, 1);
    ASSERT_THAT(ddid, Ne(FAIL));
    auto* dd = static_cast<dd_t*>(HAatom_object(ddid));
    ASSERT_THAT(dd, Ne(nullptr));
    dd->tag = MKSPECIALTAG(2100);
    HTPendaccess(ddid);

    EXPECT_THAT(HLcreate(fid, 2100, 1, 64, 4), Eq(FAIL));

    // Clean up the dummy DD
    const int32_t ddid2 = HTPselect(frec, 2100, 1);
    ASSERT_THAT(ddid2, Ne(FAIL));
    HTPdelete(ddid2);
  }

  // Direct HLPstwrite on a read-only file access record fails with DFE_ARGS
  // (exercises line 609)
  {
    const std::string ro_path = GetTempFilePath("hblocks_ro_direct.hdf");
    {
      const int32_t wfid = Hopen(ro_path.c_str(), DFACC_CREATE, 0);
      ASSERT_THAT(wfid, Ne(FAIL));
      EXPECT_THAT(Hclose(wfid), Eq(SUCCEED));
    }
    const int32_t ro_fid = Hopen(ro_path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(ro_fid, Ne(FAIL));
    absl::Cleanup close_ro = [ro_fid] {
      EXPECT_THAT(Hclose(ro_fid), Eq(SUCCEED));
    };
    accrec_t* ro_arec = HIget_access_rec();
    ASSERT_THAT(ro_arec, Ne(nullptr));
    ro_arec->file_id = ro_fid;
    EXPECT_THAT(HLPstwrite(ro_arec), Eq(FAIL));
    HIrelease_accrec_node(ro_arec);
  }

  // HLconvert when ddid is FAIL (exercises line 427)
  {
    accrec_t* arec = HIget_access_rec();
    ASSERT_THAT(arec, Ne(nullptr));
    arec->file_id = fid;
    arec->ddid = FAIL;
    const int32_t fail_aid = HAregister_atom(AIDGROUP, arec);
    ASSERT_THAT(fail_aid, Ne(FAIL));
    EXPECT_THAT(HLconvert(fail_aid, 64, 4), Eq(FAIL));
    EXPECT_THAT(HAremove_atom(fail_aid), Ne(nullptr));
  }

  // HLconvert when data_tag has bit 15 set (MKSPECIALTAG fails, exercises line
  // 431)
  {
    auto* frec = static_cast<filerec_t*>(HAatom_object(fid));
    ASSERT_THAT(frec, Ne(nullptr));
    const uint16_t bad_tag = 0x8000 | 1000;
    const int32_t ddid = HTPcreate(frec, bad_tag, 99);
    ASSERT_THAT(ddid, Ne(FAIL));

    accrec_t* arec = HIget_access_rec();
    ASSERT_THAT(arec, Ne(nullptr));
    arec->file_id = fid;
    arec->ddid = ddid;
    const int32_t aid = HAregister_atom(AIDGROUP, arec);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(HLconvert(aid, 64, 4), Eq(FAIL));
    EXPECT_THAT(HAremove_atom(aid), Ne(nullptr));
    HTPendaccess(ddid);
  }

  // Special element with invalid link_ref in header: exercises line 686 and
  // HLIgetlink failure
  {
    auto* frec = static_cast<filerec_t*>(HAatom_object(fid));
    ASSERT_THAT(frec, Ne(nullptr));
    const uint16_t sp_tag = MKSPECIALTAG(2300);
    uint8_t hdr[16] = {0};
    uint8_t* p = hdr;
    UINT16ENCODE(p, SPECIAL_LINKED);
    INT32ENCODE(p, 16);
    INT32ENCODE(p, 16);
    INT32ENCODE(p, 2);
    UINT16ENCODE(p, 9999);  // nonexistent link table ref
    EXPECT_THAT(Hputelement(fid, sp_tag, 1, hdr, 16), Eq(16));

    const int32_t ddid = HTPselect(frec, sp_tag, 1);
    ASSERT_THAT(ddid, Ne(FAIL));

    accrec_t* arec = HIget_access_rec();
    ASSERT_THAT(arec, Ne(nullptr));
    arec->file_id = fid;
    arec->ddid = ddid;
    arec->special_info = nullptr;

    EXPECT_THAT(HLPstread(arec), Eq(FAIL));

    HIrelease_accrec_node(arec);
    HTPendaccess(ddid);
  }

  // Element with unallocated block 0: exercises line 697 (first_length =
  // block_length)
  {
    const int32_t aid = HLcreate(fid, 1800, 1, 32, 2);
    ASSERT_THAT(aid, Ne(FAIL));
    // Seek to block 1 and write there, leaving block 0 unallocated
    EXPECT_THAT(Hseek(aid, 32, DF_START), Eq(SUCCEED));
    uint8_t d[4] = {1, 2, 3, 4};
    EXPECT_THAT(Hwrite(aid, 4, d), Eq(4));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

    // Reopen for reading: block 0 has ref 0, hitting line 697
    const int32_t raid = Hstartread(fid, 1800, 1);
    ASSERT_THAT(raid, Ne(FAIL));
    int32_t len = 0;
    int32_t flen = 0;
    int32_t blen = 0;
    int32_t nblocks = 0;
    EXPECT_THAT(HDinqblockinfo(raid, &len, &flen, &blen, &nblocks),
                Eq(SUCCEED));
    EXPECT_THAT(flen, Eq(32));
    EXPECT_THAT(Hendaccess(raid), Eq(SUCCEED));
  }

  // Chained link table pointing to nonexistent nextref: exercises lines 705-713
  {
    auto* frec = static_cast<filerec_t*>(HAatom_object(fid));
    ASSERT_THAT(frec, Ne(nullptr));

    // Table 1 points to nonexistent nextref 7777
    const uint16_t link_ref = 7700;
    uint8_t tbl[6] = {0};
    uint8_t* tp = tbl;
    UINT16ENCODE(tp, 7777);  // nextref = 7777 (nonexistent)
    UINT16ENCODE(tp, 0);     // block 0 ref = 0
    UINT16ENCODE(tp, 0);     // block 1 ref = 0
    EXPECT_THAT(Hputelement(fid, DFTAG_LINKED, link_ref, tbl, 6), Eq(6));

    const uint16_t sp_tag = MKSPECIALTAG(2500);
    uint8_t hdr[16] = {0};
    uint8_t* p = hdr;
    UINT16ENCODE(p, SPECIAL_LINKED);
    INT32ENCODE(p, 16);
    INT32ENCODE(p, 16);
    INT32ENCODE(p, 2);
    UINT16ENCODE(p, link_ref);
    EXPECT_THAT(Hputelement(fid, sp_tag, 1, hdr, 16), Eq(16));

    const int32_t ddid = HTPselect(frec, sp_tag, 1);
    ASSERT_THAT(ddid, Ne(FAIL));

    accrec_t* arec = HIget_access_rec();
    ASSERT_THAT(arec, Ne(nullptr));
    arec->file_id = fid;
    arec->ddid = ddid;
    arec->special_info = nullptr;

    EXPECT_THAT(HLPstread(arec), Eq(FAIL));

    HIrelease_accrec_node(arec);
    HTPendaccess(ddid);
  }

  // HLgetdatainfo error paths for Hoffset and Hlength: exercises lines 820,
  // 826, 882
  {
    const uint16_t link_ref = 8800;
    uint8_t tbl[6] = {0};
    uint8_t* tp = tbl;
    UINT16ENCODE(tp, 0);     // nextref = 0
    UINT16ENCODE(tp, 9999);  // block 0 ref = 9999 (nonexistent)
    UINT16ENCODE(tp, 0);     // block 1 ref = 0
    EXPECT_THAT(Hputelement(fid, DFTAG_LINKED, link_ref, tbl, 6), Eq(6));

    uint8_t sp_buf[14] = {0};
    uint8_t* sp = sp_buf;
    INT32ENCODE(sp, 16);
    INT32ENCODE(sp, 16);
    INT32ENCODE(sp, 2);
    UINT16ENCODE(sp, link_ref);

    int32_t offset = 0;
    int32_t len = 0;
    EXPECT_THAT(HLgetdatainfo(fid, sp_buf, 0, 1, &offset, nullptr), Eq(FAIL));
    EXPECT_THAT(HLgetdatainfo(fid, sp_buf, 0, 1, nullptr, &len), Eq(FAIL));
  }
}

// TODO: b/383748289 - Increase coverage for remaining uncovered error paths in
// hblocks.c.

}  // namespace
}  // namespace hdf4
