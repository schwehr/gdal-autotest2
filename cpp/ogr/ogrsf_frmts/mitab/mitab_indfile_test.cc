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

constexpr const char* kTestDir = "/vsimem/mitab_indfile_test";

class TABINDFileTest : public ::testing::Test {
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

TEST_F(TABINDFileTest, OpenAndCloseErrors) {
  TABINDFile ind;

  // 1. Invalid access mode
  EXPECT_EQ(-1, ind.Open("/vsimem/dummy.ind", "invalid"));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 2. Non-existent file with bTestOpenNoError = TRUE vs FALSE
  EXPECT_EQ(
      -1, ind.Open(std::string(std::string(kTestDir) + "/nonexist.ind").c_str(),
                   "r", TRUE));
  EXPECT_EQ(CE_None, CPLGetLastErrorType());

  EXPECT_EQ(
      -1, ind.Open(std::string(std::string(kTestDir) + "/nonexist.ind").c_str(),
                   "r", FALSE));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 3. Extension replacement (.dat -> .ind)
  const std::string datPath = std::string(kTestDir) + "/test.dat";
  EXPECT_EQ(0, ind.Open(datPath.c_str(), "w"));
  // Double open fails
  EXPECT_EQ(-1, ind.Open(datPath.c_str(), "w"));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
  EXPECT_EQ(0, ind.Close());

  // 4. Corrupt magic cookie
  const std::string corruptPath = std::string(kTestDir) + "/corrupt.ind";
  VSILFILE* fp = VSIFOpenL(corruptPath.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  GByte headerBuf[512] = {0};
  GUInt32 badCookie = 12345678;
  memcpy(headerBuf, &badCookie, 4);
  VSIFWriteL(headerBuf, 1, 512, fp);
  VSIFCloseL(fp);

  EXPECT_EQ(-1, ind.Open(corruptPath.c_str(), "r"));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 5. Invalid numIndexes (< 1 or > 29)
  fp = VSIFOpenL(corruptPath.c_str(), "wb");
  ASSERT_NE(nullptr, fp);
  GUInt32 goodCookie = 24242424;
  memcpy(headerBuf, &goodCookie, 4);
  GInt16 badNumIdx = 35;
  memcpy(headerBuf + 12, &badNumIdx, 2);
  VSIFWriteL(headerBuf, 1, 512, fp);
  VSIFCloseL(fp);

  EXPECT_EQ(-1, ind.Open(corruptPath.c_str(), "r"));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 6. Operations on closed file fail validation
  EXPECT_EQ(-1, ind.SetIndexFieldType(1, TABFInteger));
  EXPECT_EQ(-1, ind.SetIndexUnique(1, TRUE));
  EXPECT_EQ(nullptr, ind.BuildKey(1, 100));
  EXPECT_EQ(nullptr, ind.BuildKey(1, "abc"));
  EXPECT_EQ(nullptr, ind.BuildKey(1, 123.45));
  EXPECT_EQ(nullptr, ind.BuildKey(1, static_cast<GInt64>(123456789)));
  EXPECT_EQ(-1, ind.FindFirst(1, nullptr));
  EXPECT_EQ(-1, ind.FindNext(1, nullptr));
  EXPECT_EQ(-1, ind.AddEntry(1, nullptr, 1));

  // 7. Invalid index number on opened file (lines 463-466)
  ASSERT_EQ(
      0, ind.Open(std::string(std::string(kTestDir) + "/valid_num.ind").c_str(),
                  "w"));
  EXPECT_EQ(1, ind.CreateIndex(TABFInteger, 0));
  EXPECT_EQ(-1, ind.SetIndexFieldType(99, TABFInteger));  // Out of range index
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
  ind.Close();
}

TEST_F(TABINDFileTest, CreateIndexesAndBuildKeys) {
  const std::string path = std::string(kTestDir) + "/all_keys.ind";
  TABINDFile ind;
  ASSERT_EQ(0, ind.Open(path.c_str(), "w"));

  // CreateIndex for supported types
  int idxInt = ind.CreateIndex(TABFInteger, 0);
  EXPECT_EQ(1, idxInt);
  int idxSmall = ind.CreateIndex(TABFSmallInt, 0);
  EXPECT_EQ(2, idxSmall);
  int idxLarge = ind.CreateIndex(TABFLargeInt, 0);
  EXPECT_EQ(3, idxLarge);
  int idxFloat = ind.CreateIndex(TABFFloat, 0);
  EXPECT_EQ(4, idxFloat);
  int idxDec = ind.CreateIndex(TABFDecimal, 0);
  EXPECT_EQ(5, idxDec);
  int idxDate = ind.CreateIndex(TABFDate, 0);
  EXPECT_EQ(6, idxDate);
  int idxTime = ind.CreateIndex(TABFTime, 0);
  EXPECT_EQ(7, idxTime);
  int idxLogic = ind.CreateIndex(TABFLogical, 0);
  EXPECT_EQ(8, idxLogic);
  int idxChar1 = ind.CreateIndex(TABFChar, 1);
  EXPECT_EQ(9, idxChar1);
  int idxChar10 = ind.CreateIndex(TABFChar, 10);
  EXPECT_EQ(10, idxChar10);
  int idxCharClamp = ind.CreateIndex(TABFChar, 200);  // Clamped to 128
  EXPECT_EQ(11, idxCharClamp);

  // DateTime not supported
  EXPECT_EQ(-1, ind.CreateIndex(TABFDateTime, 0));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Test SetIndexFieldType and SetIndexUnique
  EXPECT_EQ(0, ind.SetIndexFieldType(idxInt, TABFInteger));
  EXPECT_EQ(
      -1, ind.SetIndexFieldType(idxInt, TABFSmallInt));  // Mismatch key length
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
  EXPECT_EQ(0, ind.SetIndexUnique(idxInt, TRUE));

  // Test BuildKey(GInt32):
  // 1 byte (idxChar1)
  GByte* key1 = ind.BuildKey(idxChar1, 10);
  ASSERT_NE(nullptr, key1);
  EXPECT_EQ(static_cast<GByte>((10 & 0xff) + 0x80), key1[0]);

  // 2 bytes (idxSmall)
  GByte* key2 = ind.BuildKey(idxSmall, 500);
  ASSERT_NE(nullptr, key2);

  // 4 bytes (idxInt)
  GByte* key4 = ind.BuildKey(idxInt, 123456);
  ASSERT_NE(nullptr, key4);

  // Switch default branch: BuildKey(GInt32) on 8-byte key length (idxFloat)
  EXPECT_NE(nullptr, ind.BuildKey(idxFloat, 100));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Test BuildKey(const char *)
  GByte* keyStr = ind.BuildKey(idxChar10, "Hello");
  ASSERT_NE(nullptr, keyStr);
  EXPECT_EQ('H', keyStr[0]);
  EXPECT_EQ('E', keyStr[1]);
  EXPECT_EQ('L', keyStr[2]);
  EXPECT_EQ('L', keyStr[3]);
  EXPECT_EQ('O', keyStr[4]);
  EXPECT_EQ('\0', keyStr[5]);

  // Test BuildKey(double)
  GByte* keyDbl = ind.BuildKey(idxFloat, 3.14159);
  ASSERT_NE(nullptr, keyDbl);

  // Test BuildKey(GInt64)
  GByte* keyI64 = ind.BuildKey(idxLarge, static_cast<GInt64>(987654321012LL));
  ASSERT_NE(nullptr, keyI64);

  // Add more indexes to reach 29 limit
  for (int i = 12; i <= 29; ++i) {
    int idx = ind.CreateIndex(TABFInteger, 0);
    EXPECT_EQ(i, idx);
  }
  // 30th index fails limit
  EXPECT_EQ(-1, ind.CreateIndex(TABFInteger, 0));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(0, ind.Close());
}

TEST_F(TABINDFileTest, BTreeSplittingAndSearching) {
  const std::string path = std::string(kTestDir) + "/btree.ind";
  TABINDFile ind;
  ASSERT_EQ(0, ind.Open(path.c_str(), "w"));

  // Create an index with TABFChar width 60 (max 7 entries per 512-byte node).
  int idx = ind.CreateIndex(TABFChar, 60);
  EXPECT_EQ(1, idx);

  // Insert 60 entries:
  // We'll insert in mixed order, including duplicate keys, to trigger all split
  // branches! First, insert 20 ascending entries: "KEY_00", "KEY_02", ...
  for (int i = 0; i < 20; ++i) {
    char keyStr[32];
    snprintf(keyStr, sizeof(keyStr), "KEY_%02d", i * 2);
    GByte* pKey = ind.BuildKey(idx, keyStr);
    EXPECT_EQ(0, ind.AddEntry(idx, pKey, 100 + i));
  }

  // Second, insert 20 descending/odd entries: "KEY_39", "KEY_37", ...
  for (int i = 19; i >= 0; --i) {
    char keyStr[32];
    snprintf(keyStr, sizeof(keyStr), "KEY_%02d", i * 2 + 1);
    GByte* pKey = ind.BuildKey(idx, keyStr);
    EXPECT_EQ(0, ind.AddEntry(idx, pKey, 200 + i));
  }

  // Third, insert 20 duplicate entries: "KEY_10", "KEY_20", etc.
  for (int i = 0; i < 20; ++i) {
    char keyStr[32];
    snprintf(keyStr, sizeof(keyStr), "KEY_%02d", (i % 10) * 2);
    GByte* pKey = ind.BuildKey(idx, keyStr);
    EXPECT_EQ(0, ind.AddEntry(idx, pKey, 300 + i));
  }

  EXPECT_EQ(0, ind.Close());

  // Reopen in "r+" (TABReadWrite) mode to test ReadHeader and searching
  ASSERT_EQ(0, ind.Open(path.c_str(), "r+"));

  // 1. FindFirst for existing key with duplicates ("KEY_00")
  GByte* searchKey = ind.BuildKey(idx, "KEY_00");
  GInt32 rec = ind.FindFirst(idx, searchKey);
  EXPECT_GT(rec, 0);

  // FindNext returns duplicates until 0
  int dupCount = 1;
  while ((rec = ind.FindNext(idx, searchKey)) > 0) {
    dupCount++;
  }
  EXPECT_GE(dupCount, 2);

  // 2. FindFirst for single key ("KEY_01")
  searchKey = ind.BuildKey(idx, "KEY_01");
  rec = ind.FindFirst(idx, searchKey);
  EXPECT_GT(rec, 0);

  // 3. FindFirst for key smaller than min ("KEY_!")
  searchKey = ind.BuildKey(idx, "KEY_!");
  EXPECT_EQ(0, ind.FindFirst(idx, searchKey));

  // 4. FindFirst for key larger than max ("KEY_ZZ")
  searchKey = ind.BuildKey(idx, "KEY_ZZ");
  EXPECT_EQ(0, ind.FindFirst(idx, searchKey));

  // 5. FindFirst for missing key in middle ("KEY_99")
  searchKey = ind.BuildKey(idx, "KEY_99");
  EXPECT_EQ(0, ind.FindFirst(idx, searchKey));

  EXPECT_EQ(0, ind.Close());
}

TEST_F(TABINDFileTest, DirectTABINDNodeErrorBranches) {
  // Test uninitialized node methods
  TABINDNode node(TABRead);
  GByte dummyKey[10] = {0};
  EXPECT_EQ(-1, node.FindFirst(dummyKey));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(-1, node.FindNext(dummyKey));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(-1, node.CommitToFile());
  EXPECT_EQ(-1, node.SetPrevNodePtr(512));
  EXPECT_EQ(-1, node.SetNextNodePtr(1024));
  EXPECT_EQ(nullptr, node.GetNodeKey());
  EXPECT_EQ(-1, node.SetFieldType(TABFInteger));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Node pointer checks in FindFirst
  const std::string path = std::string(kTestDir) + "/node_err.ind";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  TABBinBlockManager mgr;
  mgr.SetName("IND");
  mgr.SetBlockSize(512);

  TABINDNode rootNode(TABReadWrite);
  ASSERT_EQ(0, rootNode.InitNode(fp, 0, 4, 2, FALSE, &mgr));
  GByte key[4] = {1, 2, 3, 4};
  EXPECT_EQ(0, rootNode.AddEntry(key, 513, TRUE));

  // SetPrev/NextNodePtr noop
  EXPECT_EQ(0, rootNode.SetPrevNodePtr(0));  // already 0
  EXPECT_EQ(0, rootNode.SetNextNodePtr(0));  // already 0

  // FindFirst on rootNode will detect unaligned child pointer (nChildNodePtr %
  // 512 != 0)
  EXPECT_EQ(-1, rootNode.FindFirst(key));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // FindFirst cycle detection: child pointer points to cycleNode itself!
  TABINDNode cycleNode(TABReadWrite);
  ASSERT_EQ(0, cycleNode.InitNode(fp, 0, 4, 2, FALSE, &mgr));
  EXPECT_EQ(0, cycleNode.AddEntry(key, cycleNode.GetNodeBlockPtr(), TRUE));
  EXPECT_EQ(-1, cycleNode.FindFirst(key));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  VSIFCloseL(fp);
}

TEST_F(TABINDFileTest, DumpBlock) {
#ifdef DEBUG
  TABINDFile ind;
  ind.Dump(nullptr);

  const std::string path = std::string(kTestDir) + "/dump.ind";
  ind.Open(path.c_str(), "w");
  ind.CreateIndex(TABFInteger, 0);
  ind.Dump(nullptr);
  ind.Close();

  TABINDNode node(TABRead);
  node.Dump(nullptr);
#endif
}

}  // namespace
}  // namespace autotest2
