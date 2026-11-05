// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr const char* kTestDir = "/vsimem/mitab_mapcoordblock_test";

class TABMAPCoordBlockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CPLErrorReset();
    VSIMkdir(kTestDir, 0777);
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
    CPLErrorReset();
  }
};

TEST_F(TABMAPCoordBlockTest, UninitializedBlockBehavior) {
  TABMAPCoordBlock block(TABRead);
  EXPECT_EQ(TABMAP_COORD_BLOCK, block.GetBlockClass());
  EXPECT_EQ(1, block.GetNumBlocksInChain());
  EXPECT_EQ(0, block.GetNextCoordBlock());
  EXPECT_EQ(0, block.GetTotalDataSize());
  EXPECT_EQ(0, block.GetFeatureDataSize());

  // Commit on uninitialized block fails.
  EXPECT_EQ(-1, block.CommitToFile());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Write on read block fails.
  const GByte dummy[4] = {0};
  EXPECT_EQ(-1, block.WriteBytes(4, dummy));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(TABMAPCoordBlockTest, InitBlockFromDataErrors) {
  TABMAPCoordBlock block(TABRead);

  // Buffer with wrong block type at non-zero offset.
  GByte buf[512] = {0};
  buf[0] = static_cast<GByte>(TABMAP_INDEX_BLOCK);  // Wrong type (expected 2)
  EXPECT_EQ(-1, block.InitBlockFromData(buf, 512, 512, TRUE, nullptr, 512));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Buffer with correct block type but invalid m_numDataBytes (< 0)
  buf[0] = static_cast<GByte>(TABMAP_COORD_BLOCK);
  // Bytes 2-3 are m_numDataBytes in big-endian or native?
  // mitab ReadInt16 is little-endian (or big-endian? mitab uses CPL_LSB
  // usually, but rawbinblock ReadInt16: let's check: mitab ReadInt16 in
  // rawbinblock is little endian: buf[0x02] | (buf[0x03] << 8) if signed
  // int16). Let's set 0xFFFF (-1).
  buf[2] = 0xFF;
  buf[3] = 0xFF;
  EXPECT_EQ(-1, block.InitBlockFromData(buf, 512, 512, TRUE, nullptr, 512));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Buffer with m_numDataBytes + 8 > nBlockSize (e.g. 505 + 8 = 513 > 512)
  buf[2] = static_cast<GByte>(505 & 0xFF);
  buf[3] = static_cast<GByte>((505 >> 8) & 0xFF);
  EXPECT_EQ(-1, block.InitBlockFromData(buf, 512, 512, TRUE, nullptr, 512));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(TABMAPCoordBlockTest, InitNewBlockAndCommitLifecycle) {
  const std::string path = std::string(kTestDir) + "/lifecycle.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  TABMAPCoordBlock block(TABReadWrite);
  // Remember: non-zero offset!
  EXPECT_EQ(0, block.InitNewBlock(fp, 512, 512));
  EXPECT_EQ(0, block.GetNextCoordBlock());
  EXPECT_EQ(0, block.GetTotalDataSize());
  EXPECT_EQ(0, block.GetFeatureDataSize());

  // Commit unmodified block.
  EXPECT_EQ(0, block.CommitToFile());

  // Set next coord block address and check modified.
  block.SetNextCoordBlock(1024);
  EXPECT_EQ(1024, block.GetNextCoordBlock());

  // Commit modified block.
  EXPECT_EQ(0, block.CommitToFile());

  // Test SeekEnd, Feature size, and MBR tracking.
  block.StartNewFeature();
  EXPECT_EQ(0, block.GetFeatureDataSize());
  GInt32 minX = 0, minY = 0, maxX = 0, maxY = 0;
  block.GetFeatureMBR(minX, minY, maxX, maxY);
  EXPECT_EQ(1000000000, minX);
  EXPECT_EQ(-1000000000, maxX);

  block.SeekEnd();
  block.ResetTotalDataSize();
  EXPECT_EQ(0, block.GetTotalDataSize());

  VSIFCloseL(fp);
}

TEST_F(TABMAPCoordBlockTest, WriteAndReadIntCoordsUncompressedAndCompressed) {
  const std::string path = std::string(kTestDir) + "/coords.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  TABMAPCoordBlock block(TABReadWrite);
  ASSERT_EQ(0, block.InitNewBlock(fp, 512, 512));

  block.StartNewFeature();
  // Uncompressed coordinates.
  EXPECT_EQ(0, block.WriteIntCoord(100, 200, FALSE));
  EXPECT_EQ(0, block.WriteIntCoord(50, 400, FALSE));

  GInt32 minX = 0, minY = 0, maxX = 0, maxY = 0;
  block.GetFeatureMBR(minX, minY, maxX, maxY);
  EXPECT_EQ(50, minX);
  EXPECT_EQ(200, minY);
  EXPECT_EQ(100, maxX);
  EXPECT_EQ(400, maxY);
  EXPECT_EQ(16, block.GetFeatureDataSize());

  // Compressed coordinates with origin.
  block.SetComprCoordOrigin(1000, 2000);
  EXPECT_EQ(0, block.WriteIntCoord(1050, 2075, TRUE));

  // Commit and read back.
  EXPECT_EQ(0, block.CommitToFile());

  TABMAPCoordBlock readBlock(TABRead);
  ASSERT_EQ(0, readBlock.ReadFromFile(fp, 512, 512));
  readBlock.SetComprCoordOrigin(1000, 2000);

  // Position at byte 8 (first coord).
  readBlock.GotoByteInBlock(8);

  GInt32 x = 0, y = 0;
  EXPECT_EQ(0, readBlock.ReadIntCoord(FALSE, x, y));
  EXPECT_EQ(100, x);
  EXPECT_EQ(200, y);

  EXPECT_EQ(0, readBlock.ReadIntCoord(FALSE, x, y));
  EXPECT_EQ(50, x);
  EXPECT_EQ(400, y);

  EXPECT_EQ(0, readBlock.ReadIntCoord(TRUE, x, y));
  EXPECT_EQ(1050, x);
  EXPECT_EQ(2075, y);

  // Test ReadIntCoords multi-pair reader.
  readBlock.GotoByteInBlock(8);
  GInt32 panXY[4] = {0};
  EXPECT_EQ(0, readBlock.ReadIntCoords(FALSE, 2, panXY));
  EXPECT_EQ(100, panXY[0]);
  EXPECT_EQ(200, panXY[1]);
  EXPECT_EQ(50, panXY[2]);
  EXPECT_EQ(400, panXY[3]);

  // Compressed multi-pair read.
  GInt32 panCompr[2] = {0};
  EXPECT_EQ(0, readBlock.ReadIntCoords(TRUE, 1, panCompr));
  EXPECT_EQ(1050, panCompr[0]);
  EXPECT_EQ(2075, panCompr[1]);

  VSIFCloseL(fp);
}

TEST_F(TABMAPCoordBlockTest, WriteAndReadCoordSecHdrsAllVersions) {
  const std::string path = std::string(kTestDir) + "/sechdrs.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  const int versions[] = {300, 450, 800};

  for (int v : versions) {
    for (GBool bCompressed : {FALSE, TRUE}) {
      TABMAPCoordBlock writeBlock(TABReadWrite);
      ASSERT_EQ(0, writeBlock.InitNewBlock(fp, 512, 512));
      if (bCompressed) {
        writeBlock.SetComprCoordOrigin(500, 600);
      }

      const int nSectionSize = (v >= 450) ? 28 : 24;
      const int numSections = 2;
      const int totalHdrSize = nSectionSize * numSections;

      TABMAPCoordSecHdr hdrs[2];
      hdrs[0].numVertices = 10;
      hdrs[0].numHoles = 0;
      hdrs[0].nXMin = 10;
      hdrs[0].nYMin = 20;
      hdrs[0].nXMax = 30;
      hdrs[0].nYMax = 40;
      hdrs[0].nDataOffset = totalHdrSize;
      hdrs[0].nVertexOffset = 0;

      hdrs[1].numVertices = 15;
      hdrs[1].numHoles = 1;
      hdrs[1].nXMin = 50;
      hdrs[1].nYMin = 60;
      hdrs[1].nXMax = 70;
      hdrs[1].nYMax = 80;
      hdrs[1].nDataOffset = totalHdrSize + 10 * 8;
      hdrs[1].nVertexOffset = 10;

      writeBlock.GotoByteInBlock(8);
      EXPECT_EQ(
          0, writeBlock.WriteCoordSecHdrs(v, numSections, hdrs, bCompressed));
      EXPECT_EQ(0, writeBlock.CommitToFile());

      // Read back.
      TABMAPCoordBlock readBlock(TABRead);
      ASSERT_EQ(0, readBlock.ReadFromFile(fp, 512, 512));
      if (bCompressed) {
        readBlock.SetComprCoordOrigin(500, 600);
      }
      readBlock.GotoByteInBlock(8);

      TABMAPCoordSecHdr readHdrs[2];
      GInt32 totalVertices = 0;
      EXPECT_EQ(0, readBlock.ReadCoordSecHdrs(bCompressed, v, numSections,
                                              readHdrs, totalVertices));
      EXPECT_EQ(25, totalVertices);
      EXPECT_EQ(10, readHdrs[0].numVertices);
      EXPECT_EQ(0, readHdrs[0].numHoles);
      EXPECT_EQ(10, readHdrs[0].nXMin);
      EXPECT_EQ(20, readHdrs[0].nYMin);
      EXPECT_EQ(30, readHdrs[0].nXMax);
      EXPECT_EQ(40, readHdrs[0].nYMax);
      EXPECT_EQ(0, readHdrs[0].nVertexOffset);

      EXPECT_EQ(15, readHdrs[1].numVertices);
      EXPECT_EQ(1, readHdrs[1].numHoles);
      EXPECT_EQ(50, readHdrs[1].nXMin);
      EXPECT_EQ(60, readHdrs[1].nYMin);
      EXPECT_EQ(70, readHdrs[1].nXMax);
      EXPECT_EQ(80, readHdrs[1].nYMax);
      EXPECT_EQ(10, readHdrs[1].nVertexOffset);
    }
  }

  VSIFCloseL(fp);
}

TEST_F(TABMAPCoordBlockTest, ReadCoordSecHdrsErrorBranches) {
  TABMAPCoordBlock block(TABRead);

  // 1. numSections overflow
  TABMAPCoordSecHdr hdrs[1];
  GInt32 totalVertices = 0;
  EXPECT_EQ(-1, block.ReadCoordSecHdrs(FALSE, 300, INT_MAX / 20, hdrs,
                                       totalVertices));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  const std::string path = std::string(kTestDir) + "/sec_err.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  // Write invalid numVertices (< 0)
  TABMAPCoordBlock writeBlock(TABReadWrite);
  ASSERT_EQ(0, writeBlock.InitNewBlock(fp, 512, 512));
  writeBlock.GotoByteInBlock(8);
  writeBlock.WriteInt16(-5);  // negative numVertices for V300
  writeBlock.WriteInt16(0);   // numHoles
  writeBlock.WriteInt32(0);   // minX
  writeBlock.WriteInt32(0);   // minY
  writeBlock.WriteInt32(0);   // maxX
  writeBlock.WriteInt32(0);   // maxY
  writeBlock.WriteInt32(24);  // nDataOffset
  writeBlock.CommitToFile();

  TABMAPCoordBlock readBlock(TABRead);
  ASSERT_EQ(0, readBlock.ReadFromFile(fp, 512, 512));
  readBlock.GotoByteInBlock(8);
  EXPECT_EQ(-1, readBlock.ReadCoordSecHdrs(FALSE, 300, 1, hdrs, totalVertices));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Write invalid numHoles (< 0)
  ASSERT_EQ(0, writeBlock.InitNewBlock(fp, 512, 512));
  writeBlock.GotoByteInBlock(8);
  writeBlock.WriteInt16(10);  // numVertices
  writeBlock.WriteInt16(-1);  // numHoles < 0
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(24);
  writeBlock.CommitToFile();

  ASSERT_EQ(0, readBlock.ReadFromFile(fp, 512, 512));
  readBlock.GotoByteInBlock(8);
  EXPECT_EQ(-1, readBlock.ReadCoordSecHdrs(FALSE, 300, 1, hdrs, totalVertices));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Write invalid nDataOffset < nTotalHdrSizeUncompressed
  ASSERT_EQ(0, writeBlock.InitNewBlock(fp, 512, 512));
  writeBlock.GotoByteInBlock(8);
  writeBlock.WriteInt16(10);
  writeBlock.WriteInt16(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(10);  // nDataOffset < 24!
  writeBlock.CommitToFile();

  ASSERT_EQ(0, readBlock.ReadFromFile(fp, 512, 512));
  readBlock.GotoByteInBlock(8);
  EXPECT_EQ(-1, readBlock.ReadCoordSecHdrs(FALSE, 300, 1, hdrs, totalVertices));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Write ungrouped vertices (nVertexOffset + numVertices > totalVertices)
  ASSERT_EQ(0, writeBlock.InitNewBlock(fp, 512, 512));
  writeBlock.GotoByteInBlock(8);
  writeBlock.WriteInt16(10);
  writeBlock.WriteInt16(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(0);
  writeBlock.WriteInt32(24 + 100 * 8);  // nVertexOffset = 100, but total is 10!
  writeBlock.CommitToFile();

  ASSERT_EQ(0, readBlock.ReadFromFile(fp, 512, 512));
  readBlock.GotoByteInBlock(8);
  EXPECT_EQ(-1, readBlock.ReadCoordSecHdrs(FALSE, 300, 1, hdrs, totalVertices));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  VSIFCloseL(fp);
}

TEST_F(TABMAPCoordBlockTest, ChainedBlocksWriteAndReadAcrossBlocks) {
  const std::string path = std::string(kTestDir) + "/chained.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  TABBinBlockManager mgr;
  mgr.SetName("COORD");
  mgr.SetBlockSize(512);
  // Allocate block 0 as header so first coord block is 512.
  mgr.AllocNewBlock("HEADER");

  TABMAPCoordBlock block(TABReadWrite);
  const int firstBlockOffset = mgr.AllocNewBlock("COORD");
  EXPECT_EQ(512, firstBlockOffset);
  ASSERT_EQ(0, block.InitNewBlock(fp, 512, firstBlockOffset));
  block.SetMAPBlockManagerRef(&mgr);

  // 1. Write chunk that fits in block (e.g. 400 bytes, free space is 504 - 400
  // = 104)
  std::vector<GByte> chunk1(400, 'A');
  EXPECT_EQ(0, block.WriteBytes(400, chunk1.data()));
  EXPECT_EQ(1, block.GetNumBlocksInChain());

  // 2. Write chunk that does not fit in remaining space (e.g. 200 bytes) but <=
  // 504 bytes. This will allocate a new block and link it!
  std::vector<GByte> chunk2(200, 'B');
  EXPECT_EQ(0, block.WriteBytes(200, chunk2.data()));
  EXPECT_EQ(2, block.GetNumBlocksInChain());
  const int secondBlockOffset = block.GetNextCoordBlock();
  // In the newly allocated block, block is at the new block offset.
  EXPECT_EQ(0, secondBlockOffset);  // Currently active block is the new block,
                                    // so its m_nNextCoordBlock is 0.

  // 3. Write a chunk larger than an entire block (> 504 bytes, e.g. 1100
  // bytes). This exercises the multi-block split loop (lines 762-786).
  std::vector<GByte> chunk3(1100, 'C');
  EXPECT_EQ(0, block.WriteBytes(1100, chunk3.data()));
  EXPECT_GE(block.GetNumBlocksInChain(), 4);

  EXPECT_EQ(0, block.CommitToFile());

  // 4. Test reloading existing next block when m_nNextCoordBlock != 0 in
  // TABReadWrite mode. Load block 1 from offset 512.
  TABMAPCoordBlock reloadBlock(TABReadWrite);
  ASSERT_EQ(0, reloadBlock.ReadFromFile(fp, 512, 512));
  reloadBlock.SetMAPBlockManagerRef(&mgr);
  EXPECT_NE(0, reloadBlock.GetNextCoordBlock());
  // Move to end of block 1
  reloadBlock.GotoByteInBlock(512);
  // Write a small buffer; since (m_nBlockSize - m_nCurPos) < nBytes and
  // m_nNextCoordBlock != 0, it commits current block and reloads the next
  // block!
  const GByte tiny[10] = {'Z', 'Z', 'Z', 'Z', 'Z', 'Z', 'Z', 'Z', 'Z', 'Z'};
  EXPECT_EQ(0, reloadBlock.WriteBytes(10, tiny));

  // 5. Test ReadBytes chained behavior across blocks.
  TABMAPCoordBlock chainRead(TABRead);
  ASSERT_EQ(0, chainRead.ReadFromFile(fp, 512, 512));
  EXPECT_EQ(1, chainRead.GetNumBlocksInChain());

  // Read the 400 'A' bytes
  std::vector<GByte> readBuf1(400);
  chainRead.GotoByteInBlock(8);
  EXPECT_EQ(0, chainRead.ReadBytes(400, readBuf1.data()));
  EXPECT_EQ(chunk1, readBuf1);

  // Now we are at offset 408 (which is the end of block 1's data,
  // m_numDataBytes = 400). Reading 10 bytes will trigger the block transition
  // (lines 655-669) to block 2!
  std::vector<GByte> readBuf2(10);
  EXPECT_EQ(0, chainRead.ReadBytes(10, readBuf2.data()));
  EXPECT_EQ(2, chainRead.GetNumBlocksInChain());

  // Test overlapping ReadBytes across block boundary (lines 671-687).
  // Position chainRead 5 bytes before the end of the current block.
  chainRead.GotoByteInBlock(512 - 5);
  std::vector<GByte> overlapBuf(20);
  // This will read 5 bytes from current block, advance to next, and read 15
  // from next!
  EXPECT_EQ(0, chainRead.ReadBytes(20, overlapBuf.data()));

  VSIFCloseL(fp);
}

TEST_F(TABMAPCoordBlockTest, DumpBlock) {
#ifdef DEBUG
  TABMAPCoordBlock block(TABRead);
  block.Dump(nullptr);

  const std::string path = std::string(kTestDir) + "/dump.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);
  block.InitNewBlock(fp, 512, 512);
  block.Dump(nullptr);
  VSIFCloseL(fp);
#endif
}

}  // namespace
}  // namespace autotest2
