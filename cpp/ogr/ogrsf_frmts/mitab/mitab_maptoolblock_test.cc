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

#include <array>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(MitabMapToolBlockTest, InitBlockErrors) {
  TABMAPToolBlock block(TABRead);

  // Invalid block type in buffer (must use non-zero offset so block type is
  // read from buffer)
  std::vector<GByte> buf(512, 0);
  buf[0] = TABMAP_INDEX_BLOCK;  // Expected TABMAP_TOOL_BLOCK (5)
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(
        -1, block.InitBlockFromData(buf.data(), 512, 512, TRUE, nullptr, 512));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Invalid data size (> block size)
  buf[0] = TABMAP_TOOL_BLOCK;
  buf[2] = 0xFF;
  buf[3] = 0x7F;  // 32767 > 512
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(
        -1, block.InitBlockFromData(buf.data(), 512, 512, TRUE, nullptr, 512));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Self-referencing next block
  buf[2] = 0;
  buf[3] = 0;
  buf[4] = 0;
  buf[5] = 0x04;  // next = 1024
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(
        -1, block.InitBlockFromData(buf.data(), 512, 512, TRUE, nullptr, 1024));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST(MitabMapToolBlockTest, WriteReadChainedBlocks) {
  CPLErrorReset();
  constexpr char kPath[] = "/vsimem/maptool_test/tools.bin";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/maptool_test"); };
  VSIMkdir("/vsimem/maptool_test", 0777);

  VSILFILE* fp = VSIFOpenL(kPath, "w+b");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  // Write 512-byte dummy block at offset 0 (header block space)
  std::vector<GByte> dummy_header(512, 0);
  ASSERT_EQ(1, VSIFWriteL(dummy_header.data(), 512, 1, fp));

  TABBinBlockManager mgr;
  mgr.SetBlockSize(512);
  mgr.SetLastPtr(512);

  // Write mode starting at offset 512
  {
    TABMAPToolBlock write_block(TABWrite);
    EXPECT_EQ(0, write_block.InitNewBlock(fp, 512, 512));
    write_block.SetMAPBlockManagerRef(&mgr);
    EXPECT_EQ(TABMAP_TOOL_BLOCK, write_block.GetBlockClass());
    EXPECT_EQ(1, write_block.GetNumBlocksInChain());

    // Write enough bytes to fill first block (512 - 8 header = 504 bytes
    // available) Writing 500 bytes first
    std::vector<GByte> payload1(500, 0xAA);
    EXPECT_EQ(0, write_block.WriteBytes(500, payload1.data()));

    // CheckAvailableSpace for pen (11 bytes) -> doesn't fit in 4 bytes left, so
    // allocates block 2
    EXPECT_EQ(0, write_block.CheckAvailableSpace(TABMAP_TOOL_PEN));
    EXPECT_EQ(2, write_block.GetNumBlocksInChain());

    // CheckAvailableSpace for brush (13), font (37), symbol (13)
    EXPECT_EQ(0, write_block.CheckAvailableSpace(TABMAP_TOOL_BRUSH));
    EXPECT_EQ(0, write_block.CheckAvailableSpace(TABMAP_TOOL_FONT));
    EXPECT_EQ(0, write_block.CheckAvailableSpace(TABMAP_TOOL_SYMBOL));

    // Write second payload into block 2
    std::vector<GByte> payload2(300, 0xBB);
    EXPECT_EQ(0, write_block.WriteBytes(300, payload2.data()));

    EXPECT_EQ(0, write_block.CommitToFile());
  }

  VSIFFlushL(fp);

  // Read mode starting at offset 512
  {
    TABMAPToolBlock read_block(TABRead);
    EXPECT_EQ(0, read_block.ReadFromFile(fp, 512, 512));
    EXPECT_FALSE(read_block.EndOfChain());

    // Read back first 500 bytes
    std::vector<GByte> read_buf1(500, 0);
    EXPECT_EQ(0, read_block.ReadBytes(500, read_buf1.data()));
    EXPECT_EQ(0xAA, read_buf1[0]);
    EXPECT_EQ(0xAA, read_buf1[499]);

    // Read back next 300 bytes across chain
    std::vector<GByte> read_buf2(300, 0);
    EXPECT_EQ(0, read_block.ReadBytes(300, read_buf2.data()));
    EXPECT_EQ(0xBB, read_buf2[0]);
    EXPECT_EQ(0xBB, read_buf2[299]);

    EXPECT_TRUE(read_block.EndOfChain());
  }
}

TEST(MitabMapToolBlockTest, UninitializedCommit) {
  delete new TABMAPToolBlock(TABRead);

  TABMAPToolBlock block(TABWrite);
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(-1, block.CommitToFile());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST(MitabMapToolBlockTest, WriteBytesAutoAllocNewBlock) {
  constexpr char kPath[] = "/vsimem/maptool_test/auto_alloc.bin";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/maptool_test"); };
  VSIMkdir("/vsimem/maptool_test", 0777);

  VSILFILE* fp = VSIFOpenL(kPath, "w+b");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  std::vector<GByte> dummy(512, 0);
  ASSERT_EQ(1, VSIFWriteL(dummy.data(), 512, 1, fp));

  TABBinBlockManager mgr;
  mgr.SetBlockSize(512);
  mgr.SetLastPtr(512);

  TABMAPToolBlock write_block(TABWrite);
  EXPECT_EQ(0, write_block.InitNewBlock(fp, 512, 512));
  write_block.SetMAPBlockManagerRef(&mgr);

  // Unmodified commit returns 0 (line 170)
  write_block.SetModifiedFlag(FALSE);
  EXPECT_EQ(0, write_block.CommitToFile());

  // Write 400 bytes
  std::vector<GByte> p1(400, 1);
  EXPECT_EQ(0, write_block.WriteBytes(400, p1.data()));

  // Next write of 200 bytes exceeds remaining 104 bytes in block -> triggers
  // AllocNewBlock inside WriteBytes!
  std::vector<GByte> p2(200, 2);
  EXPECT_EQ(0, write_block.WriteBytes(200, p2.data()));
  EXPECT_EQ(2, write_block.GetNumBlocksInChain());

  EXPECT_EQ(0, write_block.CommitToFile());
}

TEST(MitabMapToolBlockTest, MaxBlocksLimitReached) {
  constexpr char kPath[] = "/vsimem/maptool_test/max_blocks.bin";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/maptool_test"); };
  VSIMkdir("/vsimem/maptool_test", 0777);

  VSILFILE* fp = VSIFOpenL(kPath, "w+b");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  std::vector<GByte> dummy(512, 0);
  ASSERT_EQ(1, VSIFWriteL(dummy.data(), 512, 1, fp));

  TABBinBlockManager mgr;
  mgr.SetBlockSize(512);
  mgr.SetLastPtr(512);

  TABMAPToolBlock write_block(TABWrite);
  EXPECT_EQ(0, write_block.InitNewBlock(fp, 512, 512));
  write_block.SetMAPBlockManagerRef(&mgr);

  // Fill initial block (512 - 8 header = 504 bytes available) so next check
  // allocates
  std::vector<GByte> p(500, 0);
  EXPECT_EQ(0, write_block.WriteBytes(500, p.data()));

  // Fill up 254 more blocks to reach 255 blocks in chain
  for (int i = 1; i < 255; ++i) {
    EXPECT_EQ(0, write_block.CheckAvailableSpace(TABMAP_TOOL_FONT));
    EXPECT_EQ(0, write_block.WriteBytes(500, p.data()));
  }
  EXPECT_EQ(255, write_block.GetNumBlocksInChain());

  // Now requesting more space hits the 255 blocks limit!
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, write_block.CheckAvailableSpace(TABMAP_TOOL_FONT));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Maximum number of 255 tool blocks reached"));
    CPLErrorReset();

    EXPECT_EQ(-1, write_block.WriteBytes(500, dummy.data()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
