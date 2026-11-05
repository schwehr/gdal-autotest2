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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/ogr/ogrsf_frmts/shape/shape_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shapelib/shapefil.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shp_vsi.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::NotNull;

TEST(ShpTreeTest, CheckBoundsOverlap2DAnd3D) {
  double min1[4] = {0.0, 0.0, 0.0, 0.0};
  double max1[4] = {10.0, 10.0, 10.0, 0.0};

  double min2[4] = {5.0, 5.0, 5.0, 0.0};
  double max2[4] = {15.0, 15.0, 15.0, 0.0};

  // 2D overlap
  EXPECT_TRUE(SHPCheckBoundsOverlap(min1, max1, min2, max2, 2));
  // 3D overlap
  EXPECT_TRUE(SHPCheckBoundsOverlap(min1, max1, min2, max2, 3));

  // Disjoint in X
  double min_disjoint_x[4] = {20.0, 5.0, 5.0, 0.0};
  double max_disjoint_x[4] = {30.0, 15.0, 15.0, 0.0};
  EXPECT_FALSE(
      SHPCheckBoundsOverlap(min1, max1, min_disjoint_x, max_disjoint_x, 2));

  // Disjoint in Y
  double min_disjoint_y[4] = {5.0, 20.0, 5.0, 0.0};
  double max_disjoint_y[4] = {15.0, 30.0, 15.0, 0.0};
  EXPECT_FALSE(
      SHPCheckBoundsOverlap(min1, max1, min_disjoint_y, max_disjoint_y, 2));

  // Overlap in 2D, disjoint in Z
  double min_disjoint_z[4] = {5.0, 5.0, 20.0, 0.0};
  double max_disjoint_z[4] = {15.0, 15.0, 30.0, 0.0};
  EXPECT_TRUE(
      SHPCheckBoundsOverlap(min1, max1, min_disjoint_z, max_disjoint_z, 2));
  EXPECT_FALSE(
      SHPCheckBoundsOverlap(min1, max1, min_disjoint_z, max_disjoint_z, 3));
}

TEST(ShpTreeTest, CreateTreeFromShapefileAndSearch) {
  constexpr const char* kShpPath = "/vsimem/shptree_test.shp";
  constexpr const char* kShxPath = "/vsimem/shptree_test.shx";
  constexpr const char* kQixPath = "/vsimem/shptree_test.qix";

  auto cleanup = absl::MakeCleanup([kShpPath, kShxPath, kQixPath]() {
    VSIUnlink(kShpPath);
    VSIUnlink(kShxPath);
    VSIUnlink(kQixPath);
  });

  SHPHandle hSHP = SHPCreateLL(kShpPath, SHPT_POLYGON, VSI_SHP_GetHook(0));
  ASSERT_THAT(hSHP, NotNull());

  // Add 40 polygon features spread over quadrants
  for (int i = 0; i < 40; ++i) {
    double baseX = (i % 2 == 0) ? 10.0 : 60.0;
    double baseY = (i % 4 < 2) ? 10.0 : 60.0;
    double padfX[5] = {baseX, baseX + 5.0, baseX + 5.0, baseX, baseX};
    double padfY[5] = {baseY, baseY, baseY + 5.0, baseY + 5.0, baseY};
    int nParts = 1;
    int panPartStart[1] = {0};

    SHPObject* psObj =
        SHPCreateObject(SHPT_POLYGON, i, nParts, panPartStart, nullptr, 5,
                        padfX, padfY, nullptr, nullptr);
    ASSERT_THAT(psObj, NotNull());
    EXPECT_GE(SHPWriteObject(hSHP, -1, psObj), 0);
    SHPDestroyObject(psObj);
  }

  // Create tree with depth = 4
  SHPTree* psTree = SHPCreateTree(hSHP, 2, 4, nullptr, nullptr);
  ASSERT_THAT(psTree, NotNull());

  // Trim extra nodes
  SHPTreeTrimExtraNodes(psTree);

  // Search full extent
  double fullMin[4] = {0.0, 0.0, 0.0, 0.0};
  double fullMax[4] = {100.0, 100.0, 0.0, 0.0};
  int nHits = 0;
  int* panHits = SHPTreeFindLikelyShapes(psTree, fullMin, fullMax, &nHits);
  EXPECT_EQ(nHits, 40);
  free(panHits);

  // Search single quadrant
  double quadMin[4] = {5.0, 5.0, 0.0, 0.0};
  double quadMax[4] = {20.0, 20.0, 0.0, 0.0};
  nHits = 0;
  panHits = SHPTreeFindLikelyShapes(psTree, quadMin, quadMax, &nHits);
  EXPECT_GT(nHits, 0);
  EXPECT_LE(nHits, 40);
  free(panHits);

  // Search disjoint region
  double disjMin[4] = {200.0, 200.0, 0.0, 0.0};
  double disjMax[4] = {300.0, 300.0, 0.0, 0.0};
  nHits = -1;
  panHits = SHPTreeFindLikelyShapes(psTree, disjMin, disjMax, &nHits);
  EXPECT_EQ(nHits, 0);
  if (panHits != nullptr) {
    free(panHits);
  }

  // Test auto tree depth estimation (nMaxDepth = 0)
  SHPTree* psAutoTree = SHPCreateTree(hSHP, 2, 0, nullptr, nullptr);
  ASSERT_THAT(psAutoTree, NotNull());
  EXPECT_GT(psAutoTree->nMaxDepth, 0);
  SHPDestroyTree(psAutoTree);

  // Test 3D and 4D trees with bounds containment
  SHPTree* psTree3D = SHPCreateTree(hSHP, 3, 2, nullptr, nullptr);
  ASSERT_THAT(psTree3D, NotNull());
  SHPDestroyTree(psTree3D);

  SHPTree* psTree4D = SHPCreateTree(hSHP, 4, 2, nullptr, nullptr);
  ASSERT_THAT(psTree4D, NotNull());
  SHPDestroyTree(psTree4D);

  // Test SHPTreeAddShapeId with extra object
  double addX[5] = {1.0, 2.0, 2.0, 1.0, 1.0};
  double addY[5] = {1.0, 1.0, 2.0, 2.0, 1.0};
  int partStart[1] = {0};
  SHPObject* extraObj = SHPCreateObject(SHPT_POLYGON, 40, 1, partStart, nullptr,
                                        5, addX, addY, nullptr, nullptr);
  EXPECT_TRUE(SHPTreeAddShapeId(psTree, extraObj));
  SHPDestroyObject(extraObj);

  // Write tree to disk quadtree (.qix)
  EXPECT_TRUE(SHPWriteTree(psTree, kQixPath));

  SHPDestroyTree(psTree);
  SHPClose(hSHP);

  // Open and search disk tree with SHPSearchDiskTreeEx
  SHPTreeDiskHandle hDiskTree = SHPOpenDiskTree(kQixPath, nullptr);
  ASSERT_THAT(hDiskTree, NotNull());

  nHits = 0;
  panHits = SHPSearchDiskTreeEx(hDiskTree, fullMin, fullMax, &nHits);
  EXPECT_GE(nHits, 40);
  free(panHits);

  // Search single quadrant
  nHits = 0;
  panHits = SHPSearchDiskTreeEx(hDiskTree, quadMin, quadMax, &nHits);
  EXPECT_GT(nHits, 0);
  free(panHits);

  SHPCloseDiskTree(hDiskTree);

  // Test SHPSearchDiskTree with libc FILE* using fmemopen on the /vsimem/
  // buffer
  vsi_l_offset nDataLen = 0;
  GByte* pabyData = VSIGetMemFileBuffer(kQixPath, &nDataLen, FALSE);
  ASSERT_THAT(pabyData, NotNull());
  ASSERT_GT(nDataLen, 0);

  FILE* fpMem = fmemopen(pabyData, static_cast<size_t>(nDataLen), "rb");
  ASSERT_THAT(fpMem, NotNull());
  nHits = 0;
  panHits = SHPSearchDiskTree(fpMem, fullMin, fullMax, &nHits);
  EXPECT_GE(nHits, 40);
  free(panHits);
  fclose(fpMem);
}

TEST(ShpTreeTest, CreateTreeWithoutShapefileAndDiskTree) {
  constexpr const char* kQixPath = "/vsimem/tree_no_shp.qix";

  auto cleanup = absl::MakeCleanup([kQixPath]() { VSIUnlink(kQixPath); });

  double bMin[4] = {0.0, 0.0, 0.0, 0.0};
  double bMax[4] = {100.0, 100.0, 0.0, 0.0};

  // Create tree with hSHP = nullptr and explicit bounds
  SHPTree* psTree = SHPCreateTree(nullptr, 2, 4, bMin, bMax);
  ASSERT_THAT(psTree, NotNull());

  // Add 30 objects across quadrants to trigger tree node subdivision
  for (int i = 0; i < 30; ++i) {
    double x = (i * 3) % 95 + 1.0;
    double y = (i * 7) % 95 + 1.0;
    double padfX[5] = {x, x + 2.0, x + 2.0, x, x};
    double padfY[5] = {y, y, y + 2.0, y + 2.0, y};
    int panParts[1] = {0};
    SHPObject* obj = SHPCreateObject(SHPT_POLYGON, i, 1, panParts, nullptr, 5,
                                     padfX, padfY, nullptr, nullptr);
    EXPECT_TRUE(SHPTreeAddShapeId(psTree, obj));
    SHPDestroyObject(obj);
  }

  // Write tree with SHPWriteTreeLL
  EXPECT_TRUE(SHPWriteTreeLL(psTree, kQixPath, VSI_SHP_GetHook(0)));
  SHPDestroyTree(psTree);

  // Search disk tree
  SHPTreeDiskHandle hDisk = SHPOpenDiskTree(kQixPath, VSI_SHP_GetHook(0));
  ASSERT_THAT(hDisk, NotNull());
  int nHits = 0;
  int* panHits = SHPSearchDiskTreeEx(hDisk, bMin, bMax, &nHits);
  EXPECT_EQ(nHits, 30);
  free(panHits);

  // Search outside bounds (0 results)
  double outMin[4] = {200.0, 200.0, 0.0, 0.0};
  double outMax[4] = {300.0, 300.0, 0.0, 0.0};
  nHits = -1;
  int* panZeroHits = SHPSearchDiskTreeEx(hDisk, outMin, outMax, &nHits);
  EXPECT_EQ(nHits, 0);
  EXPECT_THAT(panZeroHits, NotNull());
  free(panZeroHits);

  SHPCloseDiskTree(hDisk);

  // Test tree with in-memory shape objects
  SHPTree* psTreeMem = SHPCreateTree(nullptr, 2, 1, bMin, bMax);
  ASSERT_THAT(psTreeMem, NotNull());
  psTreeMem->psRoot->nShapeCount = 1;
  psTreeMem->psRoot->panShapeIds = static_cast<int*>(calloc(1, sizeof(int)));
  psTreeMem->psRoot->papsShapeObj =
      static_cast<SHPObject**>(calloc(1, sizeof(SHPObject*)));
  double ptX = 10.0;
  double ptY = 10.0;
  SHPObject* memObj = SHPCreateSimpleObject(SHPT_POINT, 1, &ptX, &ptY, nullptr);
  psTreeMem->psRoot->papsShapeObj[0] = memObj;
  // Adding another shape to a node with papsShapeObj != nullptr reallocs it
  SHPObject* memObj2 =
      SHPCreateSimpleObject(SHPT_POINT, 1, &ptX, &ptY, nullptr);
  EXPECT_TRUE(SHPTreeAddShapeId(psTreeMem, memObj2));
  SHPDestroyObject(memObj2);
  SHPDestroyTree(psTreeMem);
}

TEST(ShpTreeTest, DiskTreeErrorsAndByteSwap) {
  WithQuietHandler quiet_handler;
  // Non-existent tree
  EXPECT_EQ(SHPOpenDiskTree("/vsimem/does_not_exist.qix", nullptr), nullptr);

  // Corrupt header < 16 bytes
  constexpr const char* kCorruptQix = "/vsimem/corrupt.qix";
  unsigned char shortHeader[8] = {'S', 'Q', 'I', 'X', 0, 0, 0, 0};
  VSIFCloseL(VSIFileFromMemBuffer(kCorruptQix, shortHeader, sizeof(shortHeader),
                                  FALSE));
  auto cleanup = absl::MakeCleanup([kCorruptQix]() { VSIUnlink(kCorruptQix); });
  SHPTreeDiskHandle hCorrupt = SHPOpenDiskTree(kCorruptQix, nullptr);
  ASSERT_THAT(hCorrupt, NotNull());
  int nHitsCorrupt = 0;
  double bMinCorrupt[4] = {0, 0, 0, 0};
  double bMaxCorrupt[4] = {10, 10, 0, 0};
  EXPECT_EQ(
      SHPSearchDiskTreeEx(hCorrupt, bMinCorrupt, bMaxCorrupt, &nHitsCorrupt),
      nullptr);
  SHPCloseDiskTree(hCorrupt);

  // Bad magic header (16 bytes, but not SQT)
  unsigned char badMagic[16] = {'B', 'A', 'D', '!', 0};
  constexpr const char* kBadMagicQix = "/vsimem/bad_magic.qix";
  VSIFCloseL(
      VSIFileFromMemBuffer(kBadMagicQix, badMagic, sizeof(badMagic), FALSE));
  auto cleanup2 =
      absl::MakeCleanup([kBadMagicQix]() { VSIUnlink(kBadMagicQix); });
  SHPTreeDiskHandle hBad = SHPOpenDiskTree(kBadMagicQix, nullptr);
  EXPECT_THAT(hBad, NotNull());
  int nHits = 0;
  double bMin[4] = {0, 0, 0, 0};
  double bMax[4] = {10, 10, 0, 0};
  EXPECT_EQ(SHPSearchDiskTreeEx(hBad, bMin, bMax, &nHits), nullptr);
  SHPCloseDiskTree(hBad);

  // Valid big-endian disk tree header and node
  constexpr const char* kValidSwappedQix = "/vsimem/valid_swapped.qix";
  std::vector<unsigned char> validSwapped(64, 0);
  validSwapped[0] = 'S';
  validSwapped[1] = 'Q';
  validSwapped[2] = 'T';
  validSwapped[3] = 2;                              // big-endian
  WriteMSBInt(validSwapped.data() + 16, 0);         // offset
  WriteMSBDouble(validSwapped.data() + 20, 0.0);    // minX
  WriteMSBDouble(validSwapped.data() + 28, 0.0);    // minY
  WriteMSBDouble(validSwapped.data() + 36, 100.0);  // maxX
  WriteMSBDouble(validSwapped.data() + 44, 100.0);  // maxY
  WriteMSBInt(validSwapped.data() + 52, 1);         // numshapes = 1
  WriteMSBInt(validSwapped.data() + 56, 42);        // shape ID = 42
  WriteMSBInt(validSwapped.data() + 60, 0);         // numsubnodes = 0
  VSIFCloseL(VSIFileFromMemBuffer(kValidSwappedQix, validSwapped.data(),
                                  validSwapped.size(), FALSE));
  auto cleanup3 =
      absl::MakeCleanup([kValidSwappedQix]() { VSIUnlink(kValidSwappedQix); });
  SHPTreeDiskHandle hValidSwapped = SHPOpenDiskTree(kValidSwappedQix, nullptr);
  ASSERT_THAT(hValidSwapped, NotNull());
  nHits = 0;
  int* panSwappedHits = SHPSearchDiskTreeEx(hValidSwapped, bMin, bMax, &nHits);
  EXPECT_EQ(nHits, 1);
  ASSERT_THAT(panSwappedHits, NotNull());
  EXPECT_EQ(panSwappedHits[0], 42);
  free(panSwappedHits);
  SHPCloseDiskTree(hValidSwapped);

  // Error case: offset > INT_MAX - sizeof(int)
  constexpr const char* kBadOffsetQix = "/vsimem/bad_offset.qix";
  std::vector<unsigned char> badOffset(64, 0);
  badOffset[0] = 'S';
  badOffset[1] = 'Q';
  badOffset[2] = 'T';
  badOffset[3] = 1;                        // little-endian
  memset(badOffset.data() + 16, 0xFF, 4);  // offset = 0xFFFFFFFF
  VSIFCloseL(VSIFileFromMemBuffer(kBadOffsetQix, badOffset.data(),
                                  badOffset.size(), FALSE));
  auto cleanupBadOffset =
      absl::MakeCleanup([kBadOffsetQix]() { VSIUnlink(kBadOffsetQix); });
  SHPTreeDiskHandle hBadOffset = SHPOpenDiskTree(kBadOffsetQix, nullptr);
  ASSERT_THAT(hBadOffset, NotNull());
  EXPECT_EQ(SHPSearchDiskTreeEx(hBadOffset, bMin, bMax, &nHits), nullptr);
  SHPCloseDiskTree(hBadOffset);

  // Error case: numshapes overflow
  constexpr const char* kBadNumShapesQix = "/vsimem/bad_numshapes.qix";
  std::vector<unsigned char> badNumShapes(64, 0);
  badNumShapes[0] = 'S';
  badNumShapes[1] = 'Q';
  badNumShapes[2] = 'T';
  badNumShapes[3] = 1;
  memset(badNumShapes.data() + 52, 0x7F, 4);  // numshapes = 0x7F7F7F7F
  VSIFCloseL(VSIFileFromMemBuffer(kBadNumShapesQix, badNumShapes.data(),
                                  badNumShapes.size(), FALSE));
  auto cleanupBadNumShapes =
      absl::MakeCleanup([kBadNumShapesQix]() { VSIUnlink(kBadNumShapesQix); });
  SHPTreeDiskHandle hBadNumShapes = SHPOpenDiskTree(kBadNumShapesQix, nullptr);
  ASSERT_THAT(hBadNumShapes, NotNull());
  EXPECT_EQ(SHPSearchDiskTreeEx(hBadNumShapes, bMin, bMax, &nHits), nullptr);
  SHPCloseDiskTree(hBadNumShapes);

  // Error case: truncated shape IDs
  constexpr const char* kTruncShapesQix = "/vsimem/trunc_shapes.qix";
  std::vector<unsigned char> truncShapes(56, 0);
  truncShapes[0] = 'S';
  truncShapes[1] = 'Q';
  truncShapes[2] = 'T';
  truncShapes[3] = 1;
  // bounds overlap bMin/bMax
  WriteLSBDouble(truncShapes.data() + 36, 100.0);
  WriteLSBDouble(truncShapes.data() + 44, 100.0);
  WriteLSBInt(truncShapes.data() + 52, 5);  // expects 5 shapes, but file ends
  VSIFCloseL(VSIFileFromMemBuffer(kTruncShapesQix, truncShapes.data(),
                                  truncShapes.size(), FALSE));
  auto cleanupTruncShapes =
      absl::MakeCleanup([kTruncShapesQix]() { VSIUnlink(kTruncShapesQix); });
  SHPTreeDiskHandle hTruncShapes = SHPOpenDiskTree(kTruncShapesQix, nullptr);
  ASSERT_THAT(hTruncShapes, NotNull());
  EXPECT_EQ(SHPSearchDiskTreeEx(hTruncShapes, bMin, bMax, &nHits), nullptr);
  SHPCloseDiskTree(hTruncShapes);

  // Error case: truncated subnode count
  constexpr const char* kTruncSubnodesQix = "/vsimem/trunc_subnodes.qix";
  std::vector<unsigned char> truncSubnodes(60, 0);
  truncSubnodes[0] = 'S';
  truncSubnodes[1] = 'Q';
  truncSubnodes[2] = 'T';
  truncSubnodes[3] = 1;
  WriteLSBDouble(truncSubnodes.data() + 36, 100.0);
  WriteLSBDouble(truncSubnodes.data() + 44, 100.0);
  WriteLSBInt(truncSubnodes.data() + 52, 1);  // 1 shape
  // shape ID = 0 at 56..59, but file ends before numsubnodes
  VSIFCloseL(VSIFileFromMemBuffer(kTruncSubnodesQix, truncSubnodes.data(),
                                  truncSubnodes.size(), FALSE));
  auto cleanupTruncSubnodes = absl::MakeCleanup(
      [kTruncSubnodesQix]() { VSIUnlink(kTruncSubnodesQix); });
  SHPTreeDiskHandle hTruncSubnodes =
      SHPOpenDiskTree(kTruncSubnodesQix, nullptr);
  ASSERT_THAT(hTruncSubnodes, NotNull());
  EXPECT_EQ(SHPSearchDiskTreeEx(hTruncSubnodes, bMin, bMax, &nHits), nullptr);
  SHPCloseDiskTree(hTruncSubnodes);
}

}  // namespace
}  // namespace autotest2
