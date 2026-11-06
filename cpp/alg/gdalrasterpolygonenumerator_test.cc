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
// Tests raster polygon enumerator (alg/gdalrasterpolygonenumerator.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalrasterpolygonenumerator.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/gdal_alg_priv.h

#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(GdalRasterPolygonEnumeratorTest, ConstructorAndClear) {
  // 1. Verify 4-connectedness constructor initialization.
  GDALRasterPolygonEnumerator enumerator_4(4);
  EXPECT_EQ(4, enumerator_4.nConnectedness);
  EXPECT_EQ(nullptr, enumerator_4.panPolyIdMap);
  EXPECT_EQ(nullptr, enumerator_4.panPolyValue);
  EXPECT_EQ(0, enumerator_4.nNextPolygonId);
  EXPECT_EQ(0, enumerator_4.nPolyAlloc);

  // 2. Verify 8-connectedness constructor initialization.
  GDALRasterPolygonEnumerator enumerator_8(8);
  EXPECT_EQ(8, enumerator_8.nConnectedness);
  EXPECT_EQ(nullptr, enumerator_8.panPolyIdMap);
  EXPECT_EQ(nullptr, enumerator_8.panPolyValue);
  EXPECT_EQ(0, enumerator_8.nNextPolygonId);
  EXPECT_EQ(0, enumerator_8.nPolyAlloc);

  // 3. Verify Clear() on empty enumerator is safe and idempotent.
  enumerator_4.Clear();
  EXPECT_EQ(nullptr, enumerator_4.panPolyIdMap);
  EXPECT_EQ(nullptr, enumerator_4.panPolyValue);
  EXPECT_EQ(0, enumerator_4.nNextPolygonId);
  EXPECT_EQ(0, enumerator_4.nPolyAlloc);
}

TEST(GdalRasterPolygonEnumeratorTest, FirstLineInt64) {
  GDALRasterPolygonEnumerator enumerator(4);
  constexpr int kXSize = 6;
  std::vector<std::int64_t> line = {GP_NODATA_MARKER, 10, 10, 20,
                                    GP_NODATA_MARKER, 30};
  std::vector<GInt32> ids(kXSize, -99);

  // 1. Process the very first line (panLastLineVal == nullptr).
  EXPECT_TRUE(enumerator.ProcessLine(/*panLastLineVal=*/nullptr, line.data(),
                                     /*panLastLineId=*/nullptr, ids.data(),
                                     kXSize));

  // 2. Verify assigned polygon IDs: nodata -> -1, consecutive identical ->
  // same, different -> new ID.
  EXPECT_EQ(-1, ids[0]);
  EXPECT_EQ(0, ids[1]);
  EXPECT_EQ(0, ids[2]);
  EXPECT_EQ(1, ids[3]);
  EXPECT_EQ(-1, ids[4]);
  EXPECT_EQ(2, ids[5]);

  // 3. Verify internal polygon tracking.
  EXPECT_EQ(3, enumerator.nNextPolygonId);
  ASSERT_NE(nullptr, enumerator.panPolyValue);
  EXPECT_EQ(10, enumerator.panPolyValue[0]);
  EXPECT_EQ(20, enumerator.panPolyValue[1]);
  EXPECT_EQ(30, enumerator.panPolyValue[2]);

  ASSERT_NE(nullptr, enumerator.panPolyIdMap);
  EXPECT_EQ(0, enumerator.panPolyIdMap[0]);
  EXPECT_EQ(1, enumerator.panPolyIdMap[1]);
  EXPECT_EQ(2, enumerator.panPolyIdMap[2]);
}

TEST(GdalRasterPolygonEnumeratorTest, FirstLineFloat) {
  GDALRasterPolygonEnumeratorT<float, FloatEqualityTest> enumerator(4);
  constexpr int kXSize = 6;
  std::vector<float> line = {
      static_cast<float>(GP_NODATA_MARKER), 1.5f, 1.5f, 2.5f,
      static_cast<float>(GP_NODATA_MARKER), 3.5f};
  std::vector<GInt32> ids(kXSize, -99);

  // 1. Process first line with float data type and FloatEqualityTest.
  EXPECT_TRUE(enumerator.ProcessLine(/*panLastLineVal=*/nullptr, line.data(),
                                     /*panLastLineId=*/nullptr, ids.data(),
                                     kXSize));

  EXPECT_EQ(-1, ids[0]);
  EXPECT_EQ(0, ids[1]);
  EXPECT_EQ(0, ids[2]);
  EXPECT_EQ(1, ids[3]);
  EXPECT_EQ(-1, ids[4]);
  EXPECT_EQ(2, ids[5]);

  EXPECT_EQ(3, enumerator.nNextPolygonId);
  ASSERT_NE(nullptr, enumerator.panPolyValue);
  EXPECT_FLOAT_EQ(1.5f, enumerator.panPolyValue[0]);
  EXPECT_FLOAT_EQ(2.5f, enumerator.panPolyValue[1]);
  EXPECT_FLOAT_EQ(3.5f, enumerator.panPolyValue[2]);
}

TEST(GdalRasterPolygonEnumeratorTest, ProcessLine4ConnectedMerges) {
  GDALRasterPolygonEnumerator enumerator(4);
  constexpr int kXSize = 3;

  // 1. Line 0: two disconnected regions of value 1, separated by 0.
  std::vector<std::int64_t> line0 = {1, 0, 1};
  std::vector<GInt32> id0(kXSize, -1);
  EXPECT_TRUE(enumerator.ProcessLine(nullptr, line0.data(), nullptr, id0.data(),
                                     kXSize));
  EXPECT_EQ(0, id0[0]);
  EXPECT_EQ(1, id0[1]);
  EXPECT_EQ(2, id0[2]);
  EXPECT_EQ(3, enumerator.nNextPolygonId);

  // 2. Line 1: [1, 1, 1] connects the two regions in a U-shape.
  // i=0 matches last[0] (Case C: id 0).
  // i=1 matches this[0] (Case B: id 0, last[1] is 0 so no merge).
  // i=2 matches this[1] (Case B: id 0) and matches last[2] (Case B.1: merges
  // lastLineId[2] = 2 with thisLineId[2] = 0).
  std::vector<std::int64_t> line1 = {1, 1, 1};
  std::vector<GInt32> id1(kXSize, -1);
  EXPECT_TRUE(enumerator.ProcessLine(line0.data(), line1.data(), id0.data(),
                                     id1.data(), kXSize));
  EXPECT_EQ(0, id1[0]);
  EXPECT_EQ(0, id1[1]);
  EXPECT_EQ(0, id1[2]);

  // 3. Complete merges and verify that polygon 2 maps to 0.
  enumerator.CompleteMerges();
  EXPECT_EQ(0, enumerator.panPolyIdMap[0]);
  EXPECT_EQ(0, enumerator.panPolyIdMap[2]);

  // 4. Line 2: test nodata and non-matching pixels on subsequent lines.
  std::vector<std::int64_t> line2 = {GP_NODATA_MARKER, 99, 100};
  std::vector<GInt32> id2(kXSize, -99);
  EXPECT_TRUE(enumerator.ProcessLine(line1.data(), line2.data(), id1.data(),
                                     id2.data(), kXSize));
  // i=0 is nodata -> -1.
  EXPECT_EQ(-1, id2[0]);
  // i=1 is 99 (different from prev nodata and lastLine 1) -> new polygon ID 3.
  EXPECT_EQ(3, id2[1]);
  // i=2 is 100 (different from prev 99 and lastLine 1) -> new polygon ID 4.
  EXPECT_EQ(4, id2[2]);
}

TEST(GdalRasterPolygonEnumeratorTest, ProcessLine8ConnectedMatchesAndMerges) {
  // 1. Case D (matches lastLine[i-1] in 8-connected mode, no merge).
  {
    GDALRasterPolygonEnumerator enumerator(8);
    constexpr int kXSize = 2;
    std::vector<std::int64_t> line0 = {5, 0};
    std::vector<GInt32> id0(kXSize, -1);
    EXPECT_TRUE(enumerator.ProcessLine(nullptr, line0.data(), nullptr,
                                       id0.data(), kXSize));
    EXPECT_EQ(0, id0[0]);
    EXPECT_EQ(1, id0[1]);

    std::vector<std::int64_t> line1 = {0, 5};
    std::vector<GInt32> id1(kXSize, -1);
    EXPECT_TRUE(enumerator.ProcessLine(line0.data(), line1.data(), id0.data(),
                                       id1.data(), kXSize));
    // i=0 matches last[0] == 0 (id 1).
    EXPECT_EQ(1, id1[0]);
    // i=1 is 5; matches last[0] (diagonal up-left, Case D).
    EXPECT_EQ(0, id1[1]);
  }

  // 2. Case E (matches lastLine[i+1] in 8-connected mode).
  {
    GDALRasterPolygonEnumerator enumerator(8);
    constexpr int kXSize = 2;
    std::vector<std::int64_t> line0 = {0, 6};
    std::vector<GInt32> id0(kXSize, -1);
    EXPECT_TRUE(enumerator.ProcessLine(nullptr, line0.data(), nullptr,
                                       id0.data(), kXSize));
    EXPECT_EQ(0, id0[0]);
    EXPECT_EQ(1, id0[1]);

    std::vector<std::int64_t> line1 = {6, 0};
    std::vector<GInt32> id1(kXSize, -1);
    EXPECT_TRUE(enumerator.ProcessLine(line0.data(), line1.data(), id0.data(),
                                       id1.data(), kXSize));
    // i=0 is 6; doesn't match last[0] (0), but matches last[1] (diagonal
    // up-right, Case E).
    EXPECT_EQ(1, id1[0]);
    // i=1 is 0; matches last[0] (diagonal up-left, Case D).
    EXPECT_EQ(0, id1[1]);
  }

  // 3. Case D with D.1 (matches lastLine[i-1] AND merges with lastLine[i+1]).
  {
    GDALRasterPolygonEnumerator enumerator(8);
    constexpr int kXSize = 3;
    std::vector<std::int64_t> line0 = {7, 0, 7};
    std::vector<GInt32> id0(kXSize, -1);
    EXPECT_TRUE(enumerator.ProcessLine(nullptr, line0.data(), nullptr,
                                       id0.data(), kXSize));
    EXPECT_EQ(0, id0[0]);
    EXPECT_EQ(1, id0[1]);
    EXPECT_EQ(2, id0[2]);

    std::vector<std::int64_t> line1 = {0, 7, 0};
    std::vector<GInt32> id1(kXSize, -1);
    EXPECT_TRUE(enumerator.ProcessLine(line0.data(), line1.data(), id0.data(),
                                       id1.data(), kXSize));
    // i=1 matches last[0] (Case D: id 0) AND matches last[2] (Subcase D.1:
    // merges lastLineId[2] with id 0).
    EXPECT_EQ(0, id1[1]);

    enumerator.CompleteMerges();
    EXPECT_EQ(0, enumerator.panPolyIdMap[0]);
    EXPECT_EQ(0, enumerator.panPolyIdMap[2]);
  }

  // 4. Case B.3 (matches thisLine[i-1] AND merges with lastLine[i+1]).
  {
    GDALRasterPolygonEnumerator enumerator(8);
    constexpr int kXSize = 4;
    std::vector<std::int64_t> line0 = {8, 0, 0, 8};
    std::vector<GInt32> id0(kXSize, -1);
    EXPECT_TRUE(enumerator.ProcessLine(nullptr, line0.data(), nullptr,
                                       id0.data(), kXSize));
    EXPECT_EQ(0, id0[0]);
    EXPECT_EQ(1, id0[1]);
    EXPECT_EQ(2, id0[3]);

    std::vector<std::int64_t> line1 = {0, 8, 8, 0};
    std::vector<GInt32> id1(kXSize, -1);
    EXPECT_TRUE(enumerator.ProcessLine(line0.data(), line1.data(), id0.data(),
                                       id1.data(), kXSize));
    // i=1 matches last[0] (Case D: id 0).
    EXPECT_EQ(0, id1[1]);
    // i=2 matches this[1] (Case B: id 0) and merges with last[3] (Subcase
    // B.3).
    EXPECT_EQ(0, id1[2]);

    enumerator.CompleteMerges();
    EXPECT_EQ(0, enumerator.panPolyIdMap[0]);
    EXPECT_EQ(0, enumerator.panPolyIdMap[2]);
  }

  // 5. Case B.2 (matches thisLine[i-1] AND merges with lastLine[i-1]).
  // Tested using FloatEqualityTest where |f1 - f2| <= 10 ULPs, |f2 - f3| <= 10
  // ULPs, but |f1 - f3| > 10 ULPs.
  {
    GDALRasterPolygonEnumeratorT<float, FloatEqualityTest> enumerator(8);
    constexpr int kXSize = 2;

    const float f1 = 100.0f;
    const uint32_t u1 = std::bit_cast<uint32_t>(f1);
    const float f2 = std::bit_cast<float>(u1 + 5);
    const float f3 = std::bit_cast<float>(u1 + 12);

    FloatEqualityTest eq;
    EXPECT_TRUE(eq(f1, f2));
    EXPECT_TRUE(eq(f2, f3));
    EXPECT_FALSE(eq(f1, f3));

    std::vector<float> line0 = {f1, 0.0f};
    std::vector<GInt32> id0(kXSize, -1);
    EXPECT_TRUE(enumerator.ProcessLine(nullptr, line0.data(), nullptr,
                                       id0.data(), kXSize));
    EXPECT_EQ(0, id0[0]);

    std::vector<float> line1 = {f3, f2};
    std::vector<GInt32> id1(kXSize, -1);
    EXPECT_TRUE(enumerator.ProcessLine(line0.data(), line1.data(), id0.data(),
                                       id1.data(), kXSize));
    // i=0 is f3 != f1 -> allocates new polygon (id 2).
    EXPECT_EQ(2, id1[0]);
    // i=1 is f2 == f3 (Case B: id 2), and f2 == f1 (last[0]), triggering
    // Subcase B.2: merges lastLineId[0] (0) with thisLineId[1] (2).
    EXPECT_EQ(2, id1[1]);

    enumerator.CompleteMerges();
    EXPECT_EQ(2, enumerator.panPolyIdMap[0]);
    EXPECT_EQ(2, enumerator.panPolyIdMap[2]);
  }
}

TEST(GdalRasterPolygonEnumeratorTest, AllocationGrowth) {
  GDALRasterPolygonEnumerator enumerator(4);
  constexpr int kCount = 25;
  std::vector<std::int64_t> line(kCount);
  std::vector<GInt32> ids(kCount, -1);
  for (int i = 0; i < kCount; i++) {
    line[i] = i + 1;
  }

  // Initial allocation starts at 0, grows to 20, and then expands to 60.
  EXPECT_TRUE(enumerator.ProcessLine(nullptr, line.data(), nullptr, ids.data(),
                                     kCount));
  EXPECT_GE(enumerator.nPolyAlloc, 25);
  EXPECT_EQ(25, enumerator.nNextPolygonId);

  for (int i = 0; i < kCount; i++) {
    EXPECT_EQ(i, ids[i]);
    EXPECT_EQ(i + 1, enumerator.panPolyValue[i]);
    EXPECT_EQ(i, enumerator.panPolyIdMap[i]);
  }
}

TEST(GdalRasterPolygonEnumeratorTest, MultiHopChainMergeAndCompleteMerges) {
  GDALRasterPolygonEnumerator enumerator(4);

  // Allocate 5 IDs using an initial line.
  std::vector<std::int64_t> init_line = {10, 20, 30, 40, 50};
  std::vector<GInt32> init_ids(5, -1);
  EXPECT_TRUE(enumerator.ProcessLine(nullptr, init_line.data(), nullptr,
                                     init_ids.data(), 5));

  // Configure multi-hop chain: 4 -> 3 -> 2 -> 1 -> 0.
  enumerator.panPolyIdMap[4] = 3;
  enumerator.panPolyIdMap[3] = 2;
  enumerator.panPolyIdMap[2] = 1;
  enumerator.panPolyIdMap[1] = 0;
  enumerator.panPolyIdMap[0] = 0;

  // CompleteMerges flattens multi-hop chains to canonical roots.
  enumerator.CompleteMerges();
  EXPECT_EQ(0, enumerator.panPolyIdMap[4]);
  EXPECT_EQ(0, enumerator.panPolyIdMap[3]);
  EXPECT_EQ(0, enumerator.panPolyIdMap[2]);
  EXPECT_EQ(0, enumerator.panPolyIdMap[1]);
  EXPECT_EQ(0, enumerator.panPolyIdMap[0]);
}

TEST(GdalRasterPolygonEnumeratorTest, ClearReleasesMemory) {
  GDALRasterPolygonEnumerator enumerator(4);
  std::vector<std::int64_t> line = {1, 2, 3};
  std::vector<GInt32> ids(3, -1);
  EXPECT_TRUE(enumerator.ProcessLine(nullptr, line.data(), nullptr, ids.data(),
                                     /*nXSize=*/3));
  EXPECT_NE(nullptr, enumerator.panPolyIdMap);
  EXPECT_NE(nullptr, enumerator.panPolyValue);
  EXPECT_EQ(3, enumerator.nNextPolygonId);

  // Clear releases memory and resets counts.
  enumerator.Clear();
  EXPECT_EQ(nullptr, enumerator.panPolyIdMap);
  EXPECT_EQ(nullptr, enumerator.panPolyValue);
  EXPECT_EQ(0, enumerator.nNextPolygonId);
  EXPECT_EQ(0, enumerator.nPolyAlloc);

  // Can be reused immediately after Clear().
  EXPECT_TRUE(enumerator.ProcessLine(nullptr, line.data(), nullptr, ids.data(),
                                     /*nXSize=*/3));
  EXPECT_EQ(3, enumerator.nNextPolygonId);
}

TEST(GdalRasterPolygonEnumeratorTest, MaxPolygonsError) {
  // 1. Error when allocating a new polygon on the first line.
  {
    GDALRasterPolygonEnumerator enumerator(4);
    enumerator.nNextPolygonId = std::numeric_limits<int>::max();
    std::int64_t val = 42;
    GInt32 id = 0;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(enumerator.ProcessLine(nullptr, &val, nullptr, &id, 1));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("maximum number of polygons reached"));
  }

  // 2. Error when allocating a new polygon on subsequent lines.
  {
    GDALRasterPolygonEnumerator enumerator(4);
    std::int64_t last_val = 10;
    GInt32 last_id = 0;
    EXPECT_TRUE(
        enumerator.ProcessLine(nullptr, &last_val, nullptr, &last_id, 1));

    enumerator.nNextPolygonId = std::numeric_limits<int>::max();
    std::int64_t this_val = 20;  // Different value triggers NewPolygon.
    GInt32 this_id = 0;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        enumerator.ProcessLine(&last_val, &this_val, &last_id, &this_id, 1));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("maximum number of polygons reached"));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are reallocation and
// defensive guards.
