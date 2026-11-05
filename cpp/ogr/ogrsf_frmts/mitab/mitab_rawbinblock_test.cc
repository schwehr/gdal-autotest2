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
#include <cstdint>
#include <cstring>
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

TEST(MitabRawBinBlockTest, BasicReadWriteInMemory) {
  TABRawBinBlock block(TABReadWrite, TRUE);
  EXPECT_EQ(0, block.InitNewBlock(nullptr, 512, 0));
  EXPECT_EQ(TAB_RAWBIN_BLOCK, block.GetBlockClass());
  EXPECT_EQ(0, block.GetStartAddress());

  // Test write operations
  EXPECT_EQ(0, block.WriteByte(0x42));
  EXPECT_EQ(0, block.WriteInt16(1234));
  EXPECT_EQ(0, block.WriteInt32(567890));
  EXPECT_EQ(0, block.WriteInt64(9876543210123LL));
  EXPECT_EQ(0, block.WriteFloat(3.14159f));
  EXPECT_EQ(0, block.WriteDouble(2.718281828459));
  EXPECT_EQ(0, block.WriteZeros(4));
  EXPECT_EQ(0, block.WritePaddedString(10, "hello"));

  // Check positions
  EXPECT_EQ(0, block.GotoByteInBlock(0));
  EXPECT_EQ(0, block.GetCurAddress());

  // Test read operations
  EXPECT_EQ(0x42, block.ReadByte());
  EXPECT_EQ(1234, block.ReadInt16());
  EXPECT_EQ(567890, block.ReadInt32());
  EXPECT_EQ(9876543210123LL, block.ReadInt64());
  EXPECT_FLOAT_EQ(3.14159f, block.ReadFloat());
  EXPECT_DOUBLE_EQ(2.718281828459, block.ReadDouble());

  std::array<GByte, 4> zeros = {1, 1, 1, 1};
  EXPECT_EQ(0, block.ReadBytes(4, zeros.data()));
  EXPECT_EQ(0, zeros[0]);
  EXPECT_EQ(0, zeros[1]);
  EXPECT_EQ(0, zeros[2]);
  EXPECT_EQ(0, zeros[3]);

  std::array<char, 11> padded_str{};
  EXPECT_EQ(0,
            block.ReadBytes(10, reinterpret_cast<GByte*>(padded_str.data())));
  EXPECT_STREQ("hello     ", padded_str.data());

  // GotoByteRel
  EXPECT_EQ(0, block.GotoByteRel(-10));
  EXPECT_EQ(0x68, block.ReadByte());  // 'h'
}

TEST(MitabRawBinBlockTest, BoundsAndErrors) {
  TABRawBinBlock block(TABRead, TRUE);
  EXPECT_EQ(0, block.InitNewBlock(nullptr, 512, 0));

  // GotoByteInBlock out of range
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.GotoByteInBlock(-1));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(-1, block.GotoByteInBlock(513));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Write in read-only mode
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.WriteByte(0x10));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Read beyond block size
  block.GotoByteInBlock(510);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::array<GByte, 10> buf{};
    EXPECT_EQ(-1, block.ReadBytes(10, buf.data()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Commit with null fp or uninitialized buffer
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.CommitToFile());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST(MitabRawBinBlockTest, FileReadWriteAndCommit) {
  constexpr char kPath[] = "/vsimem/rawbinblock_test/test.bin";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/rawbinblock_test"); };
  VSIMkdir("/vsimem/rawbinblock_test", 0777);

  VSILFILE* fp = VSIFOpenL(kPath, "w+b");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  TABRawBinBlock write_block(TABReadWrite, TRUE);
  EXPECT_EQ(0, write_block.InitNewBlock(fp, 512, 0));
  EXPECT_EQ(0, write_block.WriteInt32(123456));
  EXPECT_EQ(0, write_block.WritePaddedString(20, "BlockContent"));
  EXPECT_EQ(0, write_block.CommitToFile());

  // Second block at offset 512 committed as deleted
  TABRawBinBlock del_block(TABReadWrite, TRUE);
  EXPECT_EQ(0, del_block.InitNewBlock(fp, 512, 512));
  EXPECT_EQ(0, del_block.CommitAsDeleted(1024));

  // Flush to ensure file is on disk
  VSIFFlushL(fp);

  // Read first block back
  TABRawBinBlock read_block(TABRead, TRUE);
  EXPECT_EQ(0, read_block.ReadFromFile(fp, 0, 512));
  EXPECT_EQ(123456, read_block.ReadInt32());
  std::array<char, 21> str_buf{};
  EXPECT_EQ(0,
            read_block.ReadBytes(20, reinterpret_cast<GByte*>(str_buf.data())));
  EXPECT_STREQ("BlockContent        ", str_buf.data());

  // Read second block back
  TABRawBinBlock read_del_block(TABRead, TRUE);
  EXPECT_EQ(0, read_del_block.ReadFromFile(fp, 512, 512));
  EXPECT_EQ(TABMAP_GARB_BLOCK, read_del_block.ReadByte());

  // Test GotoByteInFile
  read_block.SetFirstBlockPtr(0);
  EXPECT_EQ(0, read_block.GotoByteInFile(10, /*bForceReadFromFile=*/FALSE));
  EXPECT_EQ(10, read_block.GetCurAddress());
  EXPECT_EQ(0, read_block.GotoByteInFile(516, /*bForceReadFromFile=*/TRUE,
                                         /*bOffsetIsEndOfData=*/TRUE));
}

TEST(MitabRawBinBlockTest, TABCreateMAPBlockFromFile) {
  constexpr char kPath[] = "/vsimem/rawbinblock_test/map_blocks.bin";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/rawbinblock_test"); };
  VSIMkdir("/vsimem/rawbinblock_test", 0777);

  VSILFILE* fp = VSIFOpenL(kPath, "w+b");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  // Write valid header block (type 0, 1024 bytes) at offset 0
  TABMAPHeaderBlock header(TABWrite);
  EXPECT_EQ(0, header.InitNewBlock(fp, 1024, 0));
  EXPECT_EQ(0, header.CommitToFile());

  // Write blocks 1 to 5 starting at offset 1024
  EXPECT_EQ(0, VSIFSeekL(fp, 1024, SEEK_SET));
  std::vector<GByte> buf(3072, 0);
  // Block 1 at 1024: Index block (type 1)
  buf[0] = TABMAP_INDEX_BLOCK;
  // Block 2 at 1536: Object block (type 2)
  buf[512] = TABMAP_OBJECT_BLOCK;
  // Block 3 at 2048: Coord block (type 3)
  buf[1024] = TABMAP_COORD_BLOCK;
  // Block 4 at 2560: Garbage block (type 4)
  buf[1536] = TABMAP_GARB_BLOCK;
  // Block 5 at 3072: Tool block (type 5)
  buf[2048] = TABMAP_TOOL_BLOCK;
  // Block 6 at 3584: Unknown type (type 99)
  buf[2560] = 99;

  ASSERT_EQ(3072, VSIFWriteL(buf.data(), 1, 3072, fp));
  VSIFFlushL(fp);

  // Read and verify block classes
  std::unique_ptr<TABRawBinBlock> b0(
      TABCreateMAPBlockFromFile(fp, 0, 1024, FALSE, TABRead));
  ASSERT_NE(nullptr, b0);
  EXPECT_EQ(TABMAP_HEADER_BLOCK, b0->GetBlockClass());

  std::unique_ptr<TABRawBinBlock> b1(
      TABCreateMAPBlockFromFile(fp, 1024, 512, FALSE, TABRead));
  ASSERT_NE(nullptr, b1);
  EXPECT_EQ(TABMAP_INDEX_BLOCK, b1->GetBlockClass());

  std::unique_ptr<TABRawBinBlock> b2(
      TABCreateMAPBlockFromFile(fp, 1536, 512, FALSE, TABRead));
  ASSERT_NE(nullptr, b2);
  EXPECT_EQ(TABMAP_OBJECT_BLOCK, b2->GetBlockClass());

  std::unique_ptr<TABRawBinBlock> b3(
      TABCreateMAPBlockFromFile(fp, 2048, 512, FALSE, TABRead));
  ASSERT_NE(nullptr, b3);
  EXPECT_EQ(TABMAP_COORD_BLOCK, b3->GetBlockClass());

  std::unique_ptr<TABRawBinBlock> b4(
      TABCreateMAPBlockFromFile(fp, 2560, 512, FALSE, TABRead));
  ASSERT_NE(nullptr, b4);
  EXPECT_EQ(TAB_RAWBIN_BLOCK, b4->GetBlockClass());

  std::unique_ptr<TABRawBinBlock> b5(
      TABCreateMAPBlockFromFile(fp, 3072, 512, FALSE, TABRead));
  ASSERT_NE(nullptr, b5);
  EXPECT_EQ(TABMAP_TOOL_BLOCK, b5->GetBlockClass());

  // Unknown block type returns TABRawBinBlock
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<TABRawBinBlock> b6(
        TABCreateMAPBlockFromFile(fp, 3584, 512, FALSE, TABRead));
    ASSERT_NE(nullptr, b6);
    EXPECT_EQ(TAB_RAWBIN_BLOCK, b6->GetBlockClass());
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Reading past EOF
  EXPECT_EQ(nullptr, TABCreateMAPBlockFromFile(fp, 10000, 512, FALSE, TABRead));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              TABCreateMAPBlockFromFile(fp, 10000, 512, TRUE, TABRead));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST(MitabRawBinBlockTest, TABBinBlockManager) {
  TABBinBlockManager mgr;
  mgr.SetName("TestManager");
  mgr.SetBlockSize(512);
  EXPECT_EQ(512, mgr.GetBlockSize());

  // Allocation
  GInt32 b1 = mgr.AllocNewBlock("test1");
  EXPECT_EQ(0, b1);
  GInt32 b2 = mgr.AllocNewBlock("test2");
  EXPECT_EQ(512, b2);

  // Garbage block queue
  mgr.PushGarbageBlockAsFirst(1024);
  mgr.PushGarbageBlockAsLast(2048);
  EXPECT_EQ(1024, mgr.GetFirstGarbageBlock());
  EXPECT_EQ(1024, mgr.PopGarbageBlock());
  EXPECT_EQ(2048, mgr.PopGarbageBlock());
  EXPECT_EQ(0, mgr.PopGarbageBlock());

  // Reset
  mgr.Reset();
  EXPECT_EQ(0, mgr.PopGarbageBlock());
}

TEST(MitabRawBinBlockTest, DumpBytesAndInt64) {
  FILE* fp_null = fopen("/dev/null", "w");
  ASSERT_NE(nullptr, fp_null);
  TABRawBinBlock::DumpBytes(0x12345678, 10, fp_null);
  EXPECT_EQ(0, fclose(fp_null));

  TABRawBinBlock block(TABWrite);
  std::vector<GByte> buf(512, 0);
  EXPECT_EQ(0, block.InitBlockFromData(buf.data(), 512, 8, TRUE, nullptr, 512));
  EXPECT_EQ(0, block.GotoByteInBlock(0));
  EXPECT_EQ(0, block.WriteInt64(0x1122334455667788LL));
  EXPECT_EQ(0, block.GotoByteInBlock(0));
  EXPECT_EQ(0x1122334455667788LL, block.ReadInt64());

  // ReadBytes with null destination moves read pointer
  EXPECT_EQ(0, block.GotoByteInBlock(0));
  EXPECT_EQ(0, block.ReadBytes(8, nullptr));
  EXPECT_EQ(8, block.GetCurAddress() - block.GetStartAddress());

  // Unused bytes query
  EXPECT_EQ(504, block.GetNumUnusedBytes());
  EXPECT_EQ(512 + 8, block.GetFirstUnusedByteOffset());
  // When block is full
  TABRawBinBlock full_block(TABWrite);
  EXPECT_EQ(0, full_block.InitBlockFromData(buf.data(), 512, 512, TRUE, nullptr,
                                            512));
  EXPECT_EQ(0, full_block.GetNumUnusedBytes());
  EXPECT_EQ(-1, full_block.GetFirstUnusedByteOffset());
}

TEST(MitabRawBinBlockTest, BlockTypeAndGotoErrors) {
  // Uninitialized block GetBlockType
  {
    TABRawBinBlock uninit(TABRead);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, uninit.GetBlockType());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Valid block GetBlockType (line 390)
  {
    std::vector<GByte> buf(512, 0);
    buf[0] = TABMAP_TOOL_BLOCK;
    TABRawBinBlock block(TABRead);
    EXPECT_EQ(
        0, block.InitBlockFromData(buf.data(), 512, 512, TRUE, nullptr, 512));
    EXPECT_EQ(TABMAP_TOOL_BLOCK, block.GetBlockType());
  }

  // Unsupported block type (> TABMAP_LAST_VALID_BLOCK_TYPE)
  {
    std::vector<GByte> buf(512, 0);
    buf[0] = 99;
    TABRawBinBlock block(TABRead);
    EXPECT_EQ(
        0, block.InitBlockFromData(buf.data(), 512, 512, TRUE, nullptr, 512));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.GetBlockType());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // GotoByteInBlock out of range
  {
    std::vector<GByte> buf(512, 0);
    TABRawBinBlock block(TABRead);
    EXPECT_EQ(
        0, block.InitBlockFromData(buf.data(), 512, 100, TRUE, nullptr, 512));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.GotoByteInBlock(150));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST(MitabRawBinBlockTest, ReadFromFileAndInitErrors) {
  TABRawBinBlock block(TABRead);
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(-1, block.ReadFromFile(nullptr, 0, 512));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  constexpr char kPath[] = "/vsimem/rawbin_err/test.bin";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/rawbin_err"); };
  VSIMkdir("/vsimem/rawbin_err", 0777);

  VsiMemTempWrapper file(kPath, "short");
  VSILFILE* fp = VSIFOpenL(kPath, "rb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  EXPECT_EQ(-1, block.ReadFromFile(fp, 0, 0));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Hard block size fails if file doesn't have 512 bytes
  TABRawBinBlock hard_block(TABRead, TRUE);
  EXPECT_EQ(-1, hard_block.ReadFromFile(fp, 0, 512));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST(MitabRawBinBlockTest, CommitPaddingAndGotoByteInFile) {
  constexpr char kPath[] = "/vsimem/rawbin_pad/test.bin";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/rawbin_pad"); };
  VSIMkdir("/vsimem/rawbin_pad", 0777);

  VSILFILE* fp = VSIFOpenL(kPath, "w+b");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  // CommitToFile unmodified returns 0
  {
    TABRawBinBlock block(TABWrite);
    EXPECT_EQ(0, block.InitNewBlock(fp, 512, 0));
    block.SetModifiedFlag(FALSE);
    EXPECT_EQ(0, block.CommitToFile());
  }

  // CommitAsDeleted
  {
    TABRawBinBlock uninit(TABWrite);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, uninit.CommitAsDeleted(1024));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    TABRawBinBlock block(TABWrite);
    EXPECT_EQ(0, block.InitNewBlock(fp, 512, 512));
    EXPECT_EQ(0, block.CommitAsDeleted(1024));
  }

  // CommitToFile with padding beyond EOF
  {
    TABRawBinBlock block(TABWrite);
    EXPECT_EQ(0, block.InitNewBlock(fp, 512, 1024));
    EXPECT_EQ(0, block.WriteByte(0xFF));
    EXPECT_EQ(0, block.CommitToFile());
  }

  // GotoByteInFile in TABWrite mode crossing blocks
  {
    TABRawBinBlock block(TABWrite);
    EXPECT_EQ(0, block.InitNewBlock(fp, 512, 0));
    EXPECT_EQ(0, block.GotoByteInFile(1024));
    EXPECT_EQ(1024, block.GetStartAddress());
    EXPECT_EQ(0, block.GotoByteInFile(512));
    EXPECT_EQ(512, block.GetStartAddress());

    // Negative offset
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.GotoByteInFile(-1));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // GotoByteInFile in TABReadWrite mode
  {
    TABRawBinBlock block(TABReadWrite);
    EXPECT_EQ(0, block.InitNewBlock(fp, 512, 0));
    EXPECT_EQ(0, block.WriteZeros(512));
    EXPECT_EQ(0, block.CommitToFile());

    // bOffsetIsEndOfData = TRUE and nOffset % 512 == 0
    EXPECT_EQ(0, block.GotoByteInFile(512, FALSE, TRUE));
    EXPECT_EQ(0, block.GotoByteInFile(1024, FALSE, TRUE));

    // Jump within file size with bForceReadFromFile = FALSE
    EXPECT_EQ(0, block.GotoByteInFile(100, FALSE, FALSE));

    // Jump across blocks with bForceReadFromFile = TRUE
    EXPECT_EQ(0, block.GotoByteInFile(1024, TRUE, FALSE));
  }
}

TEST(MitabRawBinBlockTest, GarbageBlockManagerExhaustive) {
  TABBinBlockManager mgr;
  mgr.SetName("GarbageMgr");
  mgr.SetBlockSize(512);

  // Push on empty list
  mgr.PushGarbageBlockAsLast(1024);
  // Push on non-empty list
  mgr.PushGarbageBlockAsLast(2048);
  mgr.PushGarbageBlockAsFirst(512);
  mgr.PushGarbageBlockAsFirst(256);

  EXPECT_EQ(256, mgr.GetFirstGarbageBlock());

  // AllocNewBlock recycles garbage block
  EXPECT_EQ(256, mgr.AllocNewBlock("recycle"));

  // Pop remaining
  EXPECT_EQ(512, mgr.PopGarbageBlock());
  EXPECT_EQ(1024, mgr.PopGarbageBlock());
  EXPECT_EQ(2048, mgr.PopGarbageBlock());
  EXPECT_EQ(0, mgr.PopGarbageBlock());

  // Push and Reset
  mgr.PushGarbageBlockAsFirst(1024);
  mgr.PushGarbageBlockAsFirst(2048);
  mgr.Reset();
  EXPECT_EQ(0, mgr.GetFirstGarbageBlock());
}

}  // namespace
}  // namespace autotest2
