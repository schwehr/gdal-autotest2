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

#include <cmath>
#include <cstring>
#include <memory>
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

constexpr const char* kTestDir = "/vsimem/mitab_mapindexblock_test";

class TABMAPIndexBlockTest : public ::testing::Test {
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

TEST_F(TABMAPIndexBlockTest, UninitializedAndBasicOperations) {
  TABMAPIndexBlock block(TABRead);
  EXPECT_EQ(TABMAP_INDEX_BLOCK, block.GetBlockClass());
  EXPECT_EQ(1, block.GetCurMaxDepth());
  EXPECT_EQ(0, block.GetNumEntries());
  EXPECT_EQ(nullptr, block.GetEntry(-1));
  EXPECT_EQ(nullptr, block.GetEntry(0));

  // Commit on uninitialized block fails.
  EXPECT_EQ(-1, block.CommitToFile());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // InitBlockFromData with wrong block type at non-zero offset.
  GByte badTypeBuf[512] = {0};
  badTypeBuf[0] =
      static_cast<GByte>(TABMAP_COORD_BLOCK);  // Wrong type (expected 1)
  EXPECT_EQ(-1,
            block.InitBlockFromData(badTypeBuf, 512, 512, TRUE, nullptr, 512));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Test MBR getters and setters.
  block.SetMBR(10, 20, 30, 40);
  GInt32 minX = 0, minY = 0, maxX = 0, maxY = 0;
  block.GetMBR(minX, minY, maxX, maxY);
  EXPECT_EQ(10, minX);
  EXPECT_EQ(20, minY);
  EXPECT_EQ(30, maxX);
  EXPECT_EQ(40, maxY);
}

TEST_F(TABMAPIndexBlockTest, InsertAndReadEntriesLifecycle) {
  const std::string path = std::string(kTestDir) + "/entries.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  // 1. InsertEntry on read-only block fails.
  TABMAPIndexBlock readOnlyBlock(TABRead);
  ASSERT_EQ(0, readOnlyBlock.InitNewBlock(fp, 512, 512));
  EXPECT_EQ(-1, readOnlyBlock.InsertEntry(0, 0, 10, 10, 100));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 2. Insert entries on read/write block.
  TABMAPIndexBlock block(TABReadWrite);
  ASSERT_EQ(0, block.InitNewBlock(fp, 512, 512));
  const int maxEntries = block.GetNumFreeEntries();
  EXPECT_EQ(25, maxEntries);  // (512 - 4) / 20 = 25

  EXPECT_EQ(0, block.InsertEntry(10, 20, 100, 200, 1000));
  EXPECT_EQ(1, block.GetNumEntries());
  EXPECT_EQ(24, block.GetNumFreeEntries());

  EXPECT_EQ(0, block.InsertEntry(5, 15, 50, 150, 2000));
  EXPECT_EQ(2, block.GetNumEntries());

  TABMAPIndexEntry* entry0 = block.GetEntry(0);
  ASSERT_NE(nullptr, entry0);
  EXPECT_EQ(10, entry0->XMin);
  EXPECT_EQ(20, entry0->YMin);
  EXPECT_EQ(100, entry0->XMax);
  EXPECT_EQ(200, entry0->YMax);
  EXPECT_EQ(1000, entry0->nBlockPtr);

  // Commit to file.
  EXPECT_EQ(0, block.CommitToFile());

  // Reload from file and verify ReadAllEntries.
  TABMAPIndexBlock reloadBlock(TABRead);
  ASSERT_EQ(0, reloadBlock.ReadFromFile(fp, 512, 512));
  EXPECT_EQ(2, reloadBlock.GetNumEntries());

  TABMAPIndexEntry* rentry1 = reloadBlock.GetEntry(1);
  ASSERT_NE(nullptr, rentry1);
  EXPECT_EQ(5, rentry1->XMin);
  EXPECT_EQ(15, rentry1->YMin);
  EXPECT_EQ(50, rentry1->XMax);
  EXPECT_EQ(150, rentry1->YMax);
  EXPECT_EQ(2000, rentry1->nBlockPtr);

  // Fill up block to max entries to test InsertEntry full error.
  for (int i = 2; i < maxEntries; ++i) {
    EXPECT_EQ(0, block.InsertEntry(i, i, i + 10, i + 10, 3000 + i));
  }
  EXPECT_EQ(0, block.GetNumFreeEntries());
  EXPECT_EQ(-1, block.InsertEntry(999, 999, 1000, 1000, 9999));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  VSIFCloseL(fp);
}

TEST_F(TABMAPIndexBlockTest, StaticAlgorithmsComputeAreaDiffAndPickSeeds) {
  // 1. ComputeAreaDiff: contained vs non-contained
  // Contained: node (0,0)-(100,100), entry (10,10)-(20,20)
  double diffContained =
      TABMAPIndexBlock::ComputeAreaDiff(0, 0, 100, 100, 10, 10, 20, 20);
  EXPECT_LT(diffContained, 0.0);

  // Non-contained: node (0,0)-(100,100), entry (50,50)-(150,150)
  double diffExpanded =
      TABMAPIndexBlock::ComputeAreaDiff(0, 0, 100, 100, 50, 50, 150, 150);
  EXPECT_GT(diffExpanded, 0.0);

  // 2. PickSeedsForSplit
  // Test case where dX > dY
  TABMAPIndexEntry entriesX[3];
  entriesX[0] = {0, 0, 10, 10, 100};
  entriesX[1] = {100, 0, 110, 10, 200};
  entriesX[2] = {50, 0, 60, 10, 300};
  int seed1 = -1, seed2 = -1;
  EXPECT_EQ(0, TABMAPIndexBlock::PickSeedsForSplit(entriesX, 3, -1, 55, 5, 65,
                                                   15, seed1, seed2));
  EXPECT_NE(-1, seed1);
  EXPECT_NE(-1, seed2);
  EXPECT_NE(seed1, seed2);

  // Test case where dY >= dX
  TABMAPIndexEntry entriesY[3];
  entriesY[0] = {0, 0, 10, 10, 100};
  entriesY[1] = {0, 100, 10, 110, 200};
  entriesY[2] = {0, 50, 10, 60, 300};
  EXPECT_EQ(0, TABMAPIndexBlock::PickSeedsForSplit(entriesY, 3, -1, 5, 55, 15,
                                                   65, seed1, seed2));
  EXPECT_NE(-1, seed1);
  EXPECT_NE(-1, seed2);
  EXPECT_NE(seed1, seed2);

  // Test case where seed1 == seed2 (identical MBRs)
  TABMAPIndexEntry entriesSame[2];
  entriesSame[0] = {10, 10, 20, 20, 100};
  entriesSame[1] = {10, 10, 20, 20, 200};
  EXPECT_EQ(0, TABMAPIndexBlock::PickSeedsForSplit(entriesSame, 2, 1, 10, 10,
                                                   20, 20, seed1, seed2));
  EXPECT_NE(-1, seed1);
  EXPECT_NE(-1, seed2);
  EXPECT_NE(seed1, seed2);

  // Test case where seed1 == seed2 and nSrcCurChildIndex == -1
  EXPECT_EQ(0, TABMAPIndexBlock::PickSeedsForSplit(entriesSame, 2, -1, 10, 10,
                                                   20, 20, seed1, seed2));
  EXPECT_NE(seed1, seed2);

  // Test seed swap branch: nSeed1 != nSrcCurChildIndex && (dAreaDiff1 >
  // dAreaDiff2 || nSeed2 == nSrcCurChildIndex)
  EXPECT_EQ(0, TABMAPIndexBlock::PickSeedsForSplit(entriesX, 3, 0, 95, 0, 105,
                                                   10, seed1, seed2));
}

TEST_F(TABMAPIndexBlockTest, ChooseSubEntryAndLeafOperations) {
  const std::string path = std::string(kTestDir) + "/subentry.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  TABMAPIndexBlock block(TABReadWrite);
  ASSERT_EQ(0, block.InitNewBlock(fp, 512, 512));

  // ChooseSubEntry on empty block
  EXPECT_EQ(-1, block.ChooseSubEntryForInsert(0, 0, 10, 10));

  // Insert entry 0: (0, 0)-(100, 100)
  // Insert entry 1: (200, 200)-(300, 300)
  block.InsertEntry(0, 0, 100, 100, 1000);
  block.InsertEntry(200, 200, 300, 300, 2000);

  // Candidate inside entry 0
  EXPECT_EQ(0, block.ChooseSubEntryForInsert(10, 10, 20, 20));
  // Candidate closer to entry 1
  EXPECT_EQ(1, block.ChooseSubEntryForInsert(210, 210, 250, 250));

  // ChooseLeafForInsert at leaf level returns the block ptr of the best entry
  EXPECT_EQ(1000, block.ChooseLeafForInsert(10, 10, 20, 20));

  // GetCurLeafEntryMBR found and not found
  GInt32 minX = 0, minY = 0, maxX = 0, maxY = 0;
  EXPECT_EQ(0, block.GetCurLeafEntryMBR(1000, minX, minY, maxX, maxY));
  EXPECT_EQ(0, minX);
  EXPECT_EQ(100, maxX);

  EXPECT_EQ(-1, block.GetCurLeafEntryMBR(9999, minX, minY, maxX, maxY));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // UpdateLeafEntry unchanged and changed
  EXPECT_EQ(0, block.UpdateLeafEntry(1000, 0, 0, 100, 100));  // unchanged
  EXPECT_EQ(0, block.UpdateLeafEntry(1000, 0, 0, 150, 150));  // changed MBR
  EXPECT_EQ(0, block.GetCurLeafEntryMBR(1000, minX, minY, maxX, maxY));
  EXPECT_EQ(150, maxX);

  // UpdateLeafEntry not found
  EXPECT_EQ(-1, block.UpdateLeafEntry(9999, 0, 0, 10, 10));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  VSIFCloseL(fp);
}

TEST_F(TABMAPIndexBlockTest, MultiLevelTreeSplittingAndAddEntry) {
  const std::string path = std::string(kTestDir) + "/multilevel.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  TABBinBlockManager mgr;
  mgr.SetName("INDEX");
  mgr.SetBlockSize(512);
  // Allocate block 0 as header so root index block is at offset 512.
  mgr.AllocNewBlock("HEADER");

  TABMAPIndexBlock rootBlock(TABReadWrite);
  const int rootOffset = mgr.AllocNewBlock("INDEX");
  EXPECT_EQ(512, rootOffset);
  ASSERT_EQ(0, rootBlock.InitNewBlock(fp, 512, rootOffset));
  rootBlock.SetMAPBlockManagerRef(&mgr);

  // AddEntry on read-only block fails
  TABMAPIndexBlock roBlock(TABRead);
  EXPECT_EQ(-1, roBlock.AddEntry(0, 0, 10, 10, 100));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Insert 60 entries to force SplitRootNode and child SplitNode!
  // Max entries per block is 25.
  // 60 entries will require a root node with multiple children.
  for (int i = 0; i < 60; ++i) {
    GInt32 x1 = i * 10;
    GInt32 y1 = i * 10;
    GInt32 x2 = x1 + 8;
    GInt32 y2 = y1 + 8;
    GInt32 blockPtr = 10000 + i * 512;
    EXPECT_EQ(0, rootBlock.AddEntry(x1, y1, x2, y2, blockPtr));
  }

  // Tree depth must now be at least 2!
  EXPECT_GE(rootBlock.GetCurMaxDepth(), 2);

  // ChooseLeafForInsert now traverses down into children
  GInt32 targetPtr = rootBlock.ChooseLeafForInsert(10, 10, 18, 18);
  EXPECT_NE(-1, targetPtr);

  // GetCurLeafEntryMBR via child chain
  GInt32 minX = 0, minY = 0, maxX = 0, maxY = 0;
  EXPECT_EQ(0, rootBlock.GetCurLeafEntryMBR(targetPtr, minX, minY, maxX, maxY));

  // UpdateLeafEntry via child chain
  EXPECT_EQ(
      0, rootBlock.UpdateLeafEntry(targetPtr, minX, minY, maxX + 2, maxY + 2));

  // UnsetCurChild and Commit
  rootBlock.UnsetCurChild();
  EXPECT_EQ(0, rootBlock.CommitToFile());

  VSIFCloseL(fp);
}

TEST_F(TABMAPIndexBlockTest, DumpBlock) {
#ifdef DEBUG
  TABMAPIndexBlock block(TABRead);
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
