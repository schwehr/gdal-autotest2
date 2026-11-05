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

#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/ogr/ogrsf_frmts/shape/shape_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shapelib/shapefil.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shp_vsi.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::NotNull;

struct ShapeDesc {
  uint8_t minX;
  uint8_t minY;
  uint8_t maxX;
  uint8_t maxY;
  int32_t shapeId;  // 1-based in file
};

struct BinDesc {
  int32_t binId;
  std::vector<ShapeDesc> shapes;
};

struct NodeDesc {
  int32_t binStart;
  int32_t shapeCount;
};

std::vector<unsigned char> BuildSbnFile(double minX, double minY, double maxX,
                                        double maxY, int32_t totalShapeCount,
                                        const std::vector<NodeDesc>& nodes,
                                        const std::vector<BinDesc>& bins,
                                        uint8_t sigByte3 = 0x0A) {
  std::vector<unsigned char> buffer(108, 0);

  // Magic signature
  buffer[0] = 0x00;
  buffer[1] = 0x00;
  buffer[2] = 0x27;
  buffer[3] = sigByte3;
  buffer[4] = 0xFF;
  buffer[5] = 0xFF;
  buffer[6] = 0xFE;
  buffer[7] = 0x70;

  WriteMSBInt(buffer.data() + 28, totalShapeCount);
  WriteMSBDouble(buffer.data() + 32, minX);
  WriteMSBDouble(buffer.data() + 40, minY);
  WriteMSBDouble(buffer.data() + 48, maxX);
  WriteMSBDouble(buffer.data() + 56, maxY);

  if (totalShapeCount == 0) {
    return buffer;
  }

  WriteMSBInt(buffer.data() + 100, 1);  // first bin id
  int32_t nodeDescSizeWords = static_cast<int32_t>(nodes.size() * 4);
  WriteMSBInt(buffer.data() + 104, nodeDescSizeWords);

  // Append node descriptors (8 bytes each)
  for (const auto& node : nodes) {
    size_t offset = buffer.size();
    buffer.resize(offset + 8);
    WriteMSBInt(buffer.data() + offset, node.binStart);
    WriteMSBInt(buffer.data() + offset + 4, node.shapeCount);
  }

  // Append bins
  for (const auto& bin : bins) {
    size_t binOffset = buffer.size();
    int32_t binSizeWords = static_cast<int32_t>(bin.shapes.size() * 4);
    buffer.resize(binOffset + 8 + bin.shapes.size() * 8);
    WriteMSBInt(buffer.data() + binOffset, bin.binId);
    WriteMSBInt(buffer.data() + binOffset + 4, binSizeWords);

    unsigned char* pShape = buffer.data() + binOffset + 8;
    for (const auto& s : bin.shapes) {
      pShape[0] = s.minX;
      pShape[1] = s.minY;
      pShape[2] = s.maxX;
      pShape[3] = s.maxY;
      WriteMSBInt(pShape + 4, s.shapeId);
      pShape += 8;
    }
  }

  return buffer;
}

TEST(ShapeTestUtilTest, WriteMsbAndLsbHelpers) {
  unsigned char b4[4] = {};

  WriteMSBInt(b4, 0x12345678);
  EXPECT_THAT(b4, ElementsAre(0x12, 0x34, 0x56, 0x78));

  WriteMSBInt(b4, -1);
  EXPECT_THAT(b4, ElementsAre(0xFF, 0xFF, 0xFF, 0xFF));

  WriteMSBInt(b4, 0);
  EXPECT_THAT(b4, ElementsAre(0x00, 0x00, 0x00, 0x00));

  WriteLSBInt(b4, 0x12345678);
  EXPECT_THAT(b4, ElementsAre(0x78, 0x56, 0x34, 0x12));

  WriteLSBInt(b4, -1);
  EXPECT_THAT(b4, ElementsAre(0xFF, 0xFF, 0xFF, 0xFF));

  WriteLSBInt(b4, 0);
  EXPECT_THAT(b4, ElementsAre(0x00, 0x00, 0x00, 0x00));

  unsigned char b8[8] = {};

  // Test all 8 distinct bytes for MSB and LSB double.
  WriteMSBDouble(b8, std::bit_cast<double>(uint64_t{0x0102030405060708ULL}));
  EXPECT_THAT(b8, ElementsAre(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08));

  WriteLSBDouble(b8, std::bit_cast<double>(uint64_t{0x0102030405060708ULL}));
  EXPECT_THAT(b8, ElementsAre(0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01));

  WriteMSBDouble(b8, 1.0);
  EXPECT_THAT(b8, ElementsAre(0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));

  WriteMSBDouble(b8, -2.0);
  EXPECT_THAT(b8, ElementsAre(0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));

  // Test 0.0 after non-zero double to ensure all bytes are actively zeroed.
  WriteMSBDouble(b8, 0.0);
  EXPECT_THAT(b8, ElementsAre(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
}

TEST(SbnSearchTest, ValidMultiLevelTreeAndSearches) {
  constexpr const char* kSbnPath = "/vsimem/valid_multilevel.sbn";
  auto cleanup = absl::MakeCleanup([]() { VSIUnlink(kSbnPath); });

  // Total 25 shapes: nMaxDepth = 3 (nodes 0..6)
  // Node 0 (root, depth 0): binStart = 3, 5 shapes
  // Node 1 (depth 1, x split right): binStart = 2, 5 shapes
  // Node 2 (depth 1, x split left): empty (binStart = 0)
  // Node 3 (depth 2, y split top of Node 1): binStart = 4, 5 shapes
  // Node 4 (depth 2, y split bottom of Node 1): binStart = 5, 5 shapes
  // Node 5 (depth 2, y split top of Node 2): binStart = 6, 5 shapes
  // Node 6 (depth 2, y split bottom of Node 2): empty
  std::vector<NodeDesc> nodes = {
      {3, 5},  // Node 0 (unsorted binStart relative to Node 1 tests qsort)
      {2, 5},  // Node 1
      {0, 0},  // Node 2
      {4, 5},  // Node 3
      {5, 5},  // Node 4
      {6, 5},  // Node 5
      {0, 0},  // Node 6
  };

  std::vector<BinDesc> bins;
  // Bin 2: for Node 1 (has non-monotone minX/minY to test bounding box
  // expansion)
  bins.push_back(
      {2,
       {
           {140, 30, 150, 40, 1},
           {130, 20, 145, 35, 2},  // smaller minX and minY than shape 1
           {150, 40, 160, 50, 3},
           {160, 50, 170, 60, 4},
           {170, 60, 180, 70, 5},
       }});
  // Bin 3: for Node 0
  bins.push_back({3,
                  {
                      {10, 10, 20, 20, 6},
                      {20, 20, 30, 30, 7},
                      {30, 30, 40, 40, 8},
                      {40, 40, 50, 50, 9},
                      {50, 50, 60, 60, 10},
                  }});
  // Bin 4: for Node 3
  bins.push_back({4,
                  {
                      {130, 130, 140, 140, 11},
                      {140, 140, 150, 150, 12},
                      {150, 150, 160, 160, 13},
                      {160, 160, 170, 170, 14},
                      {170, 170, 180, 180, 15},
                  }});
  // Bin 5: for Node 4
  bins.push_back({5,
                  {
                      {130, 10, 140, 20, 16},
                      {140, 20, 150, 30, 17},
                      {150, 30, 160, 40, 18},
                      {160, 40, 170, 50, 19},
                      {170, 50, 180, 60, 20},
                  }});
  // Bin 6: for Node 5
  bins.push_back({6,
                  {
                      {10, 130, 20, 140, 21},
                      {20, 140, 30, 150, 22},
                      {30, 150, 40, 160, 23},
                      {40, 160, 50, 170, 24},
                      {50, 170, 60, 180, 25},
                  }});

  auto data = BuildSbnFile(0.0, 0.0, 1000.0, 1000.0, 25, nodes, bins, 0x0A);
  VSIFCloseL(VSIFileFromMemBuffer(kSbnPath, data.data(), data.size(), FALSE));

  SBNSearchHandle hSBN = SBNOpenDiskTree(kSbnPath, VSI_SHP_GetHook(0));
  ASSERT_THAT(hSBN, NotNull());

  // Search full extent with SBNSearchDiskTree
  double bMin[2] = {0.0, 0.0};
  double bMax[2] = {1000.0, 1000.0};
  int nHits = 0;
  int* panHits = SBNSearchDiskTree(hSBN, bMin, bMax, &nHits);
  EXPECT_EQ(nHits, 25);
  ASSERT_THAT(panHits, NotNull());
  SBNSearchFreeIds(panHits);

  // Search again: hits CACHE (pabyShapeDesc != nullptr)
  nHits = 0;
  panHits = SBNSearchDiskTree(hSBN, bMin, bMax, &nHits);
  EXPECT_EQ(nHits, 25);
  ASSERT_THAT(panHits, NotNull());
  SBNSearchFreeIds(panHits);

  // Search sub-quadrants exercising x and y splits in SBNSearchDiskInternal
  double leftMin[2] = {0.0, 0.0};
  double leftMax[2] = {400.0, 400.0};
  nHits = 0;
  panHits = SBNSearchDiskTree(hSBN, leftMin, leftMax, &nHits);
  EXPECT_GT(nHits, 0);
  EXPECT_LT(nHits, 25);
  ASSERT_THAT(panHits, NotNull());
  SBNSearchFreeIds(panHits);

  double rightTopMin[2] = {500.0, 500.0};
  double rightTopMax[2] = {1000.0, 1000.0};
  nHits = 0;
  panHits = SBNSearchDiskTree(hSBN, rightTopMin, rightTopMax, &nHits);
  EXPECT_GT(nHits, 0);
  EXPECT_LT(nHits, 25);
  ASSERT_THAT(panHits, NotNull());
  SBNSearchFreeIds(panHits);

  // Non-intersecting query within extent
  double nonInterMin[2] = {900.0, 900.0};
  double nonInterMax[2] = {950.0, 950.0};
  nHits = -1;
  panHits = SBNSearchDiskTree(hSBN, nonInterMin, nonInterMax, &nHits);
  EXPECT_EQ(nHits, 0);
  ASSERT_THAT(panHits, NotNull());
  SBNSearchFreeIds(panHits);

  // Out of extent bounds checks
  double outMin1[2] = {1100.0, 0.0};
  double outMax1[2] = {1200.0, 1000.0};
  EXPECT_EQ(SBNSearchDiskTree(hSBN, outMin1, outMax1, &nHits), nullptr);

  double outMin2[2] = {-200.0, -200.0};
  double outMax2[2] = {-100.0, -100.0};
  EXPECT_EQ(SBNSearchDiskTree(hSBN, outMin2, outMax2, &nHits), nullptr);

  // Inverted bounds (min > max)
  double invMin[2] = {500.0, 500.0};
  double invMax[2] = {100.0, 100.0};
  EXPECT_EQ(SBNSearchDiskTree(hSBN, invMin, invMax, &nHits), nullptr);

  // Partial out-of-bounds queries covering coordinate clamping branches
  double bndMin1[2] = {-50.0, 500.0};
  double bndMax1[2] = {500.0, 600.0};
  panHits = SBNSearchDiskTree(hSBN, bndMin1, bndMax1, &nHits);
  if (panHits) SBNSearchFreeIds(panHits);

  double bndMin2[2] = {500.0, 500.0};
  double bndMax2[2] = {1500.0, 600.0};
  panHits = SBNSearchDiskTree(hSBN, bndMin2, bndMax2, &nHits);
  if (panHits) SBNSearchFreeIds(panHits);

  double bndMin3[2] = {500.0, -50.0};
  double bndMax3[2] = {600.0, 500.0};
  panHits = SBNSearchDiskTree(hSBN, bndMin3, bndMax3, &nHits);
  if (panHits) SBNSearchFreeIds(panHits);

  double bndMin4[2] = {500.0, 500.0};
  double bndMax4[2] = {600.0, 1500.0};
  panHits = SBNSearchDiskTree(hSBN, bndMin4, bndMax4, &nHits);
  if (panHits) SBNSearchFreeIds(panHits);

  // Integer search direct API
  nHits = 0;
  panHits = SBNSearchDiskTreeInteger(hSBN, 0, 0, 255, 255, &nHits);
  EXPECT_EQ(nHits, 25);
  ASSERT_THAT(panHits, NotNull());
  SBNSearchFreeIds(panHits);

  // Integer search boundary conditions
  EXPECT_EQ(SBNSearchDiskTreeInteger(hSBN, 100, 100, 50, 50, &nHits), nullptr);
  EXPECT_EQ(SBNSearchDiskTreeInteger(hSBN, -10, -10, -5, -5, &nHits), nullptr);
  EXPECT_EQ(SBNSearchDiskTreeInteger(hSBN, 300, 300, 400, 400, &nHits),
            nullptr);

  // Clamped negative/exceeding integer coordinates
  nHits = 0;
  panHits = SBNSearchDiskTreeInteger(hSBN, -50, -50, 300, 300, &nHits);
  EXPECT_EQ(nHits, 25);
  ASSERT_THAT(panHits, NotNull());
  SBNSearchFreeIds(panHits);

  SBNCloseDiskTree(hSBN);
}

TEST(SbnSearchTest, ZeroExtentAndEmptyIndex) {
  WithQuietHandler quiet_handler;

  // Zero-extent index (dfMinX == dfMaxX && dfMinY == dfMaxY)
  constexpr const char* kZeroExtentPath = "/vsimem/zero_extent.sbn";
  auto cleanup1 = absl::MakeCleanup([]() { VSIUnlink(kZeroExtentPath); });
  std::vector<NodeDesc> nodes = {{2, 1}};
  std::vector<BinDesc> bins = {{2, {{0, 0, 255, 255, 1}}}};
  auto dataZero = BuildSbnFile(100.0, 100.0, 100.0, 100.0, 1, nodes, bins);
  VSIFCloseL(VSIFileFromMemBuffer(kZeroExtentPath, dataZero.data(),
                                  dataZero.size(), FALSE));

  SBNSearchHandle hZero = SBNOpenDiskTree(kZeroExtentPath, nullptr);
  ASSERT_THAT(hZero, NotNull());
  double bMin[2] = {100.0, 100.0};
  double bMax[2] = {100.0, 100.0};
  int nHits = 0;
  int* panHits = SBNSearchDiskTree(hZero, bMin, bMax, &nHits);
  EXPECT_EQ(nHits, 1);
  ASSERT_THAT(panHits, NotNull());
  SBNSearchFreeIds(panHits);
  SBNCloseDiskTree(hZero);

  // Empty index (nShapeCount == 0)
  constexpr const char* kEmptyPath = "/vsimem/empty.sbn";
  auto cleanup2 = absl::MakeCleanup([]() { VSIUnlink(kEmptyPath); });
  auto dataEmpty = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 0, {}, {});
  VSIFCloseL(VSIFileFromMemBuffer(kEmptyPath, dataEmpty.data(),
                                  dataEmpty.size(), FALSE));

  SBNSearchHandle hEmpty = SBNOpenDiskTree(kEmptyPath, nullptr);
  ASSERT_THAT(hEmpty, NotNull());
  EXPECT_EQ(SBNSearchDiskTree(hEmpty, bMin, bMax, &nHits), nullptr);
  EXPECT_EQ(SBNSearchDiskTreeInteger(hEmpty, 0, 0, 255, 255, &nHits), nullptr);
  SBNCloseDiskTree(hEmpty);

  // Alternate signature byte 0x0D
  constexpr const char* kAltSigPath = "/vsimem/alt_sig.sbn";
  auto cleanup3 = absl::MakeCleanup([]() { VSIUnlink(kAltSigPath); });
  auto dataAlt = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, nodes, bins, 0x0D);
  VSIFCloseL(
      VSIFileFromMemBuffer(kAltSigPath, dataAlt.data(), dataAlt.size(), FALSE));

  SBNSearchHandle hAlt = SBNOpenDiskTree(kAltSigPath, nullptr);
  ASSERT_THAT(hAlt, NotNull());
  SBNCloseDiskTree(hAlt);

  // Close null handle is no-op
  SBNCloseDiskTree(nullptr);
}

TEST(SbnSearchTest, CorruptHeadersAndErrorPaths) {
  WithQuietHandler quiet_handler;

  // Non-existent file
  EXPECT_EQ(SBNOpenDiskTree("/vsimem/does_not_exist.sbn", nullptr), nullptr);

  // Truncated header < 108 bytes
  constexpr const char* kShortPath = "/vsimem/short.sbn";
  unsigned char shortBuf[50] = {0};
  VSIFCloseL(
      VSIFileFromMemBuffer(kShortPath, shortBuf, sizeof(shortBuf), FALSE));
  auto cleanup1 = absl::MakeCleanup([]() { VSIUnlink(kShortPath); });
  EXPECT_EQ(SBNOpenDiskTree(kShortPath, nullptr), nullptr);

  // Bad magic bytes
  constexpr const char* kBadMagicPath = "/vsimem/bad_magic.sbn";
  unsigned char badMagic[108] = {0};
  VSIFCloseL(
      VSIFileFromMemBuffer(kBadMagicPath, badMagic, sizeof(badMagic), FALSE));
  auto cleanup2 = absl::MakeCleanup([]() { VSIUnlink(kBadMagicPath); });
  EXPECT_EQ(SBNOpenDiskTree(kBadMagicPath, nullptr), nullptr);

  // Invalid extent in header (min > max)
  constexpr const char* kBadExtentPath = "/vsimem/bad_extent.sbn";
  auto dataBadExtent = BuildSbnFile(100.0, 0.0, 50.0, 100.0, 1, {{2, 1}},
                                    {{2, {{0, 0, 10, 10, 1}}}});
  VSIFCloseL(VSIFileFromMemBuffer(kBadExtentPath, dataBadExtent.data(),
                                  dataBadExtent.size(), FALSE));
  auto cleanup3 = absl::MakeCleanup([]() { VSIUnlink(kBadExtentPath); });
  EXPECT_EQ(SBNOpenDiskTree(kBadExtentPath, nullptr), nullptr);

  // Invalid shape count (< 0 or > 256000000)
  constexpr const char* kBadCountPath = "/vsimem/bad_count.sbn";
  auto dataBadCount = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, {{2, 1}},
                                   {{2, {{0, 0, 10, 10, 1}}}});
  WriteMSBInt(dataBadCount.data() + 28, -5);
  VSIFCloseL(VSIFileFromMemBuffer(kBadCountPath, dataBadCount.data(),
                                  dataBadCount.size(), FALSE));
  auto cleanup4 = absl::MakeCleanup([]() { VSIUnlink(kBadCountPath); });
  EXPECT_EQ(SBNOpenDiskTree(kBadCountPath, nullptr), nullptr);

  // First bin ID != 1 at offset 100
  constexpr const char* kBadFirstBinPath = "/vsimem/bad_first_bin.sbn";
  auto dataBadFirstBin = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, {{2, 1}},
                                      {{2, {{0, 0, 10, 10, 1}}}});
  WriteMSBInt(dataBadFirstBin.data() + 100, 99);
  VSIFCloseL(VSIFileFromMemBuffer(kBadFirstBinPath, dataBadFirstBin.data(),
                                  dataBadFirstBin.size(), FALSE));
  auto cleanup5 = absl::MakeCleanup([]() { VSIUnlink(kBadFirstBinPath); });
  EXPECT_EQ(SBNOpenDiskTree(kBadFirstBinPath, nullptr), nullptr);

  // Invalid node descriptor size in words (not multiple of 4, or count >
  // maxNodes)
  constexpr const char* kBadDescSizePath = "/vsimem/bad_desc_size.sbn";
  auto dataBadDescSize = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, {{2, 1}},
                                      {{2, {{0, 0, 10, 10, 1}}}});
  WriteMSBInt(dataBadDescSize.data() + 104, 7);  // not multiple of 4
  VSIFCloseL(VSIFileFromMemBuffer(kBadDescSizePath, dataBadDescSize.data(),
                                  dataBadDescSize.size(), FALSE));
  auto cleanup6 = absl::MakeCleanup([]() { VSIUnlink(kBadDescSizePath); });
  EXPECT_EQ(SBNOpenDiskTree(kBadDescSizePath, nullptr), nullptr);

  // Truncated node descriptors
  constexpr const char* kTruncDescPath = "/vsimem/trunc_desc.sbn";
  auto dataTruncDesc = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, {{2, 1}},
                                    {{2, {{0, 0, 10, 10, 1}}}});
  dataTruncDesc.resize(110);  // truncates node descriptor
  VSIFCloseL(VSIFileFromMemBuffer(kTruncDescPath, dataTruncDesc.data(),
                                  dataTruncDesc.size(), FALSE));
  auto cleanup7 = absl::MakeCleanup([]() { VSIUnlink(kTruncDescPath); });
  EXPECT_EQ(SBNOpenDiskTree(kTruncDescPath, nullptr), nullptr);

  // Inconsistent shape count in bin (nBinStart > 0 && nNodeShapeCount == 0)
  constexpr const char* kInconsistBinPath = "/vsimem/inconsist_bin.sbn";
  auto dataInconsist = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, {{2, 0}},
                                    {{2, {{0, 0, 10, 10, 1}}}});
  VSIFCloseL(VSIFileFromMemBuffer(kInconsistBinPath, dataInconsist.data(),
                                  dataInconsist.size(), FALSE));
  auto cleanup8 = absl::MakeCleanup([]() { VSIUnlink(kInconsistBinPath); });
  EXPECT_EQ(SBNOpenDiskTree(kInconsistBinPath, nullptr), nullptr);

  // All nodes empty (nBinStart == 0)
  constexpr const char* kAllEmptyPath = "/vsimem/all_empty.sbn";
  auto dataAllEmpty = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, {{0, 0}}, {});
  VSIFCloseL(VSIFileFromMemBuffer(kAllEmptyPath, dataAllEmpty.data(),
                                  dataAllEmpty.size(), FALSE));
  auto cleanup9 = absl::MakeCleanup([]() { VSIUnlink(kAllEmptyPath); });
  EXPECT_EQ(SBNOpenDiskTree(kAllEmptyPath, nullptr), nullptr);

  // First referenced bin is not 2 (e.g. 3)
  constexpr const char* kFirstNotTwoPath = "/vsimem/first_not_two.sbn";
  auto dataFirstNotTwo = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, {{3, 1}},
                                      {{3, {{0, 0, 10, 10, 1}}}});
  VSIFCloseL(VSIFileFromMemBuffer(kFirstNotTwoPath, dataFirstNotTwo.data(),
                                  dataFirstNotTwo.size(), FALSE));
  auto cleanup10 = absl::MakeCleanup([]() { VSIUnlink(kFirstNotTwoPath); });
  EXPECT_EQ(SBNOpenDiskTree(kFirstNotTwoPath, nullptr), nullptr);

  // Duplicate nBinStart across nodes
  constexpr const char* kDupBinStartPath = "/vsimem/dup_bin_start.sbn";
  auto dataDup = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 2, {{2, 1}, {2, 1}},
                              {{2, {{0, 0, 10, 10, 1}}}});
  VSIFCloseL(VSIFileFromMemBuffer(kDupBinStartPath, dataDup.data(),
                                  dataDup.size(), FALSE));
  auto cleanup11 = absl::MakeCleanup([]() { VSIUnlink(kDupBinStartPath); });
  EXPECT_EQ(SBNOpenDiskTree(kDupBinStartPath, nullptr), nullptr);

  // Unexpected bin ID in bin loop (binId != nExpectedBinId)
  constexpr const char* kBadBinIdPath = "/vsimem/bad_bin_id.sbn";
  auto dataBadBinId = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, {{2, 1}},
                                   {{5, {{0, 0, 10, 10, 1}}}});
  VSIFCloseL(VSIFileFromMemBuffer(kBadBinIdPath, dataBadBinId.data(),
                                  dataBadBinId.size(), FALSE));
  auto cleanup12 = absl::MakeCleanup([]() { VSIUnlink(kBadBinIdPath); });
  EXPECT_EQ(SBNOpenDiskTree(kBadBinIdPath, nullptr), nullptr);

  // Unexpected bin size (e.g. 0 or > 100 * 4)
  constexpr const char* kBadBinSizePath = "/vsimem/bad_bin_size.sbn";
  auto dataBadBinSize =
      BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, {{2, 1}}, {{2, {}}});
  VSIFCloseL(VSIFileFromMemBuffer(kBadBinSizePath, dataBadBinSize.data(),
                                  dataBadBinSize.size(), FALSE));
  auto cleanup13 = absl::MakeCleanup([]() { VSIUnlink(kBadBinSizePath); });
  EXPECT_EQ(SBNOpenDiskTree(kBadBinSizePath, nullptr), nullptr);

  // Missing bin (EOF before all referenced bin starts are found)
  constexpr const char* kMissingBinPath = "/vsimem/missing_bin.sbn";
  auto dataMissingBin = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 2, {{2, 1}, {3, 1}},
                                     {{2, {{0, 0, 10, 10, 1}}}});
  VSIFCloseL(VSIFileFromMemBuffer(kMissingBinPath, dataMissingBin.data(),
                                  dataMissingBin.size(), FALSE));
  auto cleanup14 = absl::MakeCleanup([]() { VSIUnlink(kMissingBinPath); });
  EXPECT_EQ(SBNOpenDiskTree(kMissingBinPath, nullptr), nullptr);

  // Truncated shape read during search (file truncated after opening)
  constexpr const char* kTruncSearchPath = "/vsimem/trunc_search.sbn";
  auto dataTruncSearch = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 1, {{2, 1}},
                                      {{2, {{0, 0, 10, 10, 1}}}});
  VSIFCloseL(VSIFileFromMemBuffer(kTruncSearchPath, dataTruncSearch.data(),
                                  dataTruncSearch.size(), FALSE));
  auto cleanup15 = absl::MakeCleanup([]() { VSIUnlink(kTruncSearchPath); });
  SBNSearchHandle hTruncSearch = SBNOpenDiskTree(kTruncSearchPath, nullptr);
  ASSERT_THAT(hTruncSearch, NotNull());
  // Truncate file so shape read fails
  VSILFILE* fpTrunc = VSIFOpenL(kTruncSearchPath, "r+b");
  ASSERT_THAT(fpTrunc, NotNull());
  EXPECT_EQ(VSIFTruncateL(fpTrunc, 116), 0);
  VSIFCloseL(fpTrunc);
  int nHits = -1;
  double bMin[2] = {0.0, 0.0};
  double bMax[2] = {10.0, 10.0};
  EXPECT_EQ(SBNSearchDiskTree(hTruncSearch, bMin, bMax, &nHits), nullptr);
  SBNCloseDiskTree(hTruncSearch);

  // Inconsistent shape count for node at search time (bin has fewer shapes than
  // node declared)
  constexpr const char* kNodeShapeMismatchPath =
      "/vsimem/node_shape_mismatch.sbn";
  // Node says 5 shapes, but bin 2 only contains 1 shape
  auto dataMismatch = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 5, {{2, 5}},
                                   {{2, {{0, 0, 10, 10, 1}}}});
  VSIFCloseL(VSIFileFromMemBuffer(kNodeShapeMismatchPath, dataMismatch.data(),
                                  dataMismatch.size(), FALSE));
  auto cleanup16 =
      absl::MakeCleanup([]() { VSIUnlink(kNodeShapeMismatchPath); });
  SBNSearchHandle hMismatch = SBNOpenDiskTree(kNodeShapeMismatchPath, nullptr);
  ASSERT_THAT(hMismatch, NotNull());
  nHits = -1;
  EXPECT_EQ(SBNSearchDiskTree(hMismatch, bMin, bMax, &nHits), nullptr);
  SBNCloseDiskTree(hMismatch);

  // Inconsistent shape count in bin at search time (two bins where sum > node
  // shapeCount)
  constexpr const char* kBinOverflowPath = "/vsimem/bin_overflow.sbn";
  auto dataOverflow =
      BuildSbnFile(0.0, 0.0, 10.0, 10.0, 5, {{2, 5}},
                   {{2, {{0, 0, 5, 5, 1}, {0, 0, 5, 5, 2}, {0, 0, 5, 5, 3}}},
                    {3, {{0, 0, 5, 5, 4}, {0, 0, 5, 5, 5}, {0, 0, 5, 5, 6}}}});
  VSIFCloseL(VSIFileFromMemBuffer(kBinOverflowPath, dataOverflow.data(),
                                  dataOverflow.size(), FALSE));
  auto cleanup17 = absl::MakeCleanup([]() { VSIUnlink(kBinOverflowPath); });
  SBNSearchHandle hOverflow = SBNOpenDiskTree(kBinOverflowPath, nullptr);
  ASSERT_THAT(hOverflow, NotNull());
  nHits = -1;
  EXPECT_EQ(SBNSearchDiskTree(hOverflow, bMin, bMax, &nHits), nullptr);
  SBNCloseDiskTree(hOverflow);

  // Deep tree (depth >= 8) using non-cached search path (pabyBinShape =
  // abyBinShape)
  constexpr const char* kDeepPath = "/vsimem/deep.sbn";
  // nShapeCount = 2041 causes nMaxDepth = 9
  std::vector<NodeDesc> deepNodes(256, {0, 0});
  deepNodes[255] = {2, 1};  // Node 255 at depth 8
  auto dataDeep = BuildSbnFile(0.0, 0.0, 10.0, 10.0, 2041, deepNodes,
                               {{2, {{0, 0, 255, 255, 1}}}});
  VSIFCloseL(
      VSIFileFromMemBuffer(kDeepPath, dataDeep.data(), dataDeep.size(), FALSE));
  auto cleanup18 = absl::MakeCleanup([]() { VSIUnlink(kDeepPath); });
  SBNSearchHandle hDeep = SBNOpenDiskTree(kDeepPath, nullptr);
  ASSERT_THAT(hDeep, NotNull());
  nHits = 0;
  int* panDeepHits = SBNSearchDiskTree(hDeep, bMin, bMax, &nHits);
  EXPECT_EQ(nHits, 1);
  if (panDeepHits) SBNSearchFreeIds(panDeepHits);
  SBNCloseDiskTree(hDeep);
}

}  // namespace
}  // namespace autotest2
