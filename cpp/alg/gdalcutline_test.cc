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
// Tests cutline and blend mask generator (alg/gdalcutline.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalcutline.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalwarper.h

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdalwarper.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

struct WarpOptionsDeleter {
  void operator()(GDALWarpOptions* psOptions) const {
    if (psOptions != nullptr) {
      GDALDestroyWarpOptions(psOptions);
    }
  }
};
using WarpOptionsUniquePtr =
    std::unique_ptr<GDALWarpOptions, WarpOptionsDeleter>;

class GdalcutlineTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

TEST_F(GdalcutlineTest, ValidationInvalidSizes) {
  WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
  ASSERT_NE(nullptr, warp_options);

  int validity_flag = -999;
  float mask_val = 1.0f;

  // 1. nXSize < 1 returns CE_None and preserves validity flag default.
  EXPECT_EQ(CE_None, GDALWarpCutlineMaskerEx(
                         warp_options.get(), /*nBandCount=*/1, GDT_Float32,
                         /*nXOff=*/0, /*nYOff=*/0,
                         /*nXSize=*/0, /*nYSize=*/10,
                         /*ppImageData=*/nullptr,
                         /*bMaskIsFloat=*/TRUE, &mask_val, &validity_flag));
  EXPECT_EQ(GCMVF_PARTIAL_INTERSECTION, validity_flag);

  // 2. nYSize < 1 returns CE_None.
  validity_flag = -999;
  EXPECT_EQ(CE_None, GDALWarpCutlineMaskerEx(
                         warp_options.get(), /*nBandCount=*/1, GDT_Float32,
                         /*nXOff=*/0, /*nYOff=*/0,
                         /*nXSize=*/10, /*nYSize=*/-1,
                         /*ppImageData=*/nullptr,
                         /*bMaskIsFloat=*/TRUE, &mask_val, &validity_flag));
  EXPECT_EQ(GCMVF_PARTIAL_INTERSECTION, validity_flag);
}

TEST_F(GdalcutlineTest, ValidationMaskNotFloat) {
  WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
  ASSERT_NE(nullptr, warp_options);

  int validity_flag = -1;
  float mask_val = 1.0f;

  // Mask must be float (bMaskIsFloat == TRUE); otherwise CE_Failure.
  EXPECT_EQ(CE_Failure, GDALWarpCutlineMaskerEx(
                            warp_options.get(), /*nBandCount=*/1, GDT_Float32,
                            /*nXOff=*/0, /*nYOff=*/0,
                            /*nXSize=*/10, /*nYSize=*/10,
                            /*ppImageData=*/nullptr,
                            /*bMaskIsFloat=*/FALSE, &mask_val, &validity_flag));
}

TEST_F(GdalcutlineTest, ValidationNullOptions) {
  int validity_flag = -1;
  float mask_val = 1.0f;

  // Null pMaskFuncArg returns CE_Failure.
  EXPECT_EQ(CE_Failure, GDALWarpCutlineMaskerEx(
                            /*pMaskFuncArg=*/nullptr, /*nBandCount=*/1,
                            GDT_Float32, /*nXOff=*/0, /*nYOff=*/0,
                            /*nXSize=*/10, /*nYSize=*/10,
                            /*ppImageData=*/nullptr,
                            /*bMaskIsFloat=*/TRUE, &mask_val, &validity_flag));
}

TEST_F(GdalcutlineTest, ValidationNullCutline) {
  WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
  ASSERT_NE(nullptr, warp_options);
  warp_options->hCutline = nullptr;

  int validity_flag = -1;
  float mask_val = 1.0f;

  // Null hCutline in options returns CE_Failure.
  EXPECT_EQ(CE_Failure, GDALWarpCutlineMaskerEx(
                            warp_options.get(), /*nBandCount=*/1, GDT_Float32,
                            /*nXOff=*/0, /*nYOff=*/0,
                            /*nXSize=*/10, /*nYSize=*/10,
                            /*ppImageData=*/nullptr,
                            /*bMaskIsFloat=*/TRUE, &mask_val, &validity_flag));
}

TEST_F(GdalcutlineTest, ValidationNonPolygonGeometry) {
  // 1. Point geometry triggers CPLError and CE_Failure.
  {
    WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
    ASSERT_NE(nullptr, warp_options);

    OGRGeometry* geom = nullptr;
    ASSERT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt("POINT(5 5)", nullptr, &geom));
    warp_options->hCutline = geom;

    int validity_flag = -1;
    float mask_val = 1.0f;

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALWarpCutlineMaskerEx(warp_options.get(), /*nBandCount=*/1,
                                      GDT_Float32, /*nXOff=*/0, /*nYOff=*/0,
                                      /*nXSize=*/10, /*nYSize=*/10,
                                      /*ppImageData=*/nullptr,
                                      /*bMaskIsFloat=*/TRUE, &mask_val,
                                      &validity_flag));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cutline should be a polygon or a multipolygon"));
  }

  // 2. LineString geometry triggers CPLError and CE_Failure.
  {
    WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
    ASSERT_NE(nullptr, warp_options);

    OGRGeometry* geom = nullptr;
    ASSERT_EQ(OGRERR_NONE, OGRGeometryFactory::createFromWkt(
                               "LINESTRING(0 0, 10 10)", nullptr, &geom));
    warp_options->hCutline = geom;

    int validity_flag = -1;
    float mask_val = 1.0f;

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALWarpCutlineMaskerEx(warp_options.get(), /*nBandCount=*/1,
                                      GDT_Float32, /*nXOff=*/0, /*nYOff=*/0,
                                      /*nXSize=*/10, /*nYSize=*/10,
                                      /*ppImageData=*/nullptr,
                                      /*bMaskIsFloat=*/TRUE, &mask_val,
                                      &validity_flag));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cutline should be a polygon or a multipolygon"));
  }
}

TEST_F(GdalcutlineTest, NoIntersectionOutsideEnvelope) {
  WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
  ASSERT_NE(nullptr, warp_options);

  // Polygon at [100, 200] x [100, 200].
  OGRGeometry* geom = nullptr;
  ASSERT_EQ(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(
                "POLYGON((100 100, 200 100, 200 200, 100 200, 100 100))",
                nullptr, &geom));
  warp_options->hCutline = geom;
  warp_options->dfCutlineBlendDist = 0.0;

  constexpr int kXSize = 10;
  constexpr int kYSize = 10;
  std::vector<float> mask(kXSize * kYSize, 1.0f);

  // 1. Chunk is to the left and top: nXOff=0, nYOff=0.
  int validity_flag = -1;
  EXPECT_EQ(CE_None,
            GDALWarpCutlineMaskerEx(
                warp_options.get(), /*nBandCount=*/1, GDT_Float32, /*nXOff=*/0,
                /*nYOff=*/0, kXSize, kYSize, /*ppImageData=*/nullptr,
                /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
  EXPECT_EQ(GCMVF_NO_INTERSECTION, validity_flag);
  for (int i = 0; i < kXSize * kYSize; i++) {
    EXPECT_FLOAT_EQ(0.0f, mask[i]);
  }

  // 2. Chunk is to the right: nXOff=300, nYOff=150.
  std::fill(mask.begin(), mask.end(), 1.0f);
  validity_flag = -1;
  EXPECT_EQ(CE_None, GDALWarpCutlineMaskerEx(
                         warp_options.get(), /*nBandCount=*/1, GDT_Float32,
                         /*nXOff=*/300, /*nYOff=*/150, kXSize, kYSize,
                         /*ppImageData=*/nullptr,
                         /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
  EXPECT_EQ(GCMVF_NO_INTERSECTION, validity_flag);
  for (int i = 0; i < kXSize * kYSize; i++) {
    EXPECT_FLOAT_EQ(0.0f, mask[i]);
  }

  // 3. Chunk is below: nXOff=150, nYOff=300.
  std::fill(mask.begin(), mask.end(), 1.0f);
  validity_flag = -1;
  EXPECT_EQ(CE_None, GDALWarpCutlineMaskerEx(
                         warp_options.get(), /*nBandCount=*/1, GDT_Float32,
                         /*nXOff=*/150, /*nYOff=*/300, kXSize, kYSize,
                         /*ppImageData=*/nullptr,
                         /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
  EXPECT_EQ(GCMVF_NO_INTERSECTION, validity_flag);
  for (int i = 0; i < kXSize * kYSize; i++) {
    EXPECT_FLOAT_EQ(0.0f, mask[i]);
  }

  // 4. Chunk is above: nXOff=150, nYOff=0.
  std::fill(mask.begin(), mask.end(), 1.0f);
  validity_flag = -1;
  EXPECT_EQ(CE_None, GDALWarpCutlineMaskerEx(
                         warp_options.get(), /*nBandCount=*/1, GDT_Float32,
                         /*nXOff=*/150, /*nYOff=*/0, kXSize, kYSize,
                         /*ppImageData=*/nullptr,
                         /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
  EXPECT_EQ(GCMVF_NO_INTERSECTION, validity_flag);
  for (int i = 0; i < kXSize * kYSize; i++) {
    EXPECT_FLOAT_EQ(0.0f, mask[i]);
  }
}

TEST_F(GdalcutlineTest, ChunkFullyWithinCutline) {
  WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
  ASSERT_NE(nullptr, warp_options);

  // Large cutline polygon enclosing [-50, 150] x [-50, 150].
  OGRGeometry* geom = nullptr;
  ASSERT_EQ(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(
                "POLYGON((-50 -50, 150 -50, 150 150, -50 150, -50 -50))",
                nullptr, &geom));
  warp_options->hCutline = geom;
  warp_options->dfCutlineBlendDist = 0.0;

  // Chunk [10, 30] x [10, 30] is completely inside the cutline polygon.
  constexpr int kXSize = 20;
  constexpr int kYSize = 20;
  std::vector<float> mask(kXSize * kYSize, 1.0f);
  int validity_flag = -1;

  EXPECT_EQ(CE_None,
            GDALWarpCutlineMaskerEx(
                warp_options.get(), /*nBandCount=*/1, GDT_Float32, /*nXOff=*/10,
                /*nYOff=*/10, kXSize, kYSize, /*ppImageData=*/nullptr,
                /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
  EXPECT_EQ(GCMVF_CHUNK_FULLY_WITHIN_CUTLINE, validity_flag);
  // In GCMVF_CHUNK_FULLY_WITHIN_CUTLINE, mask buffer is retained as valid.
  for (int i = 0; i < kXSize * kYSize; i++) {
    EXPECT_FLOAT_EQ(1.0f, mask[i]);
  }
}

TEST_F(GdalcutlineTest, PartialIntersectionZeroBlend) {
  WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
  ASSERT_NE(nullptr, warp_options);

  // Cutline covering x in [5, 15], y in [0, 10].
  OGRGeometry* geom = nullptr;
  ASSERT_EQ(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(
                "POLYGON((5 0, 15 0, 15 10, 5 10, 5 0))", nullptr, &geom));
  warp_options->hCutline = geom;
  warp_options->dfCutlineBlendDist = 0.0;

  constexpr int kXSize = 10;
  constexpr int kYSize = 10;
  std::vector<float> mask(kXSize * kYSize, 1.0f);
  int validity_flag = -1;

  // 1. Without CUTLINE_ALL_TOUCHED option (default false).
  EXPECT_EQ(CE_None,
            GDALWarpCutlineMaskerEx(
                warp_options.get(), /*nBandCount=*/1, GDT_Float32, /*nXOff=*/0,
                /*nYOff=*/0, kXSize, kYSize, /*ppImageData=*/nullptr,
                /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
  EXPECT_EQ(GCMVF_PARTIAL_INTERSECTION, validity_flag);

  // Pixels with x < 5 are outside polygon -> 0.0f.
  // Pixels with x >= 5 are inside polygon -> remain 1.0f.
  for (int y = 0; y < kYSize; y++) {
    for (int x = 0; x < kXSize; x++) {
      const float expected = (x >= 5) ? 1.0f : 0.0f;
      EXPECT_FLOAT_EQ(expected, mask[y * kXSize + x]);
    }
  }

  // 2. With CUTLINE_ALL_TOUCHED=TRUE.
  warp_options->papszWarpOptions = CSLSetNameValue(
      warp_options->papszWarpOptions, "CUTLINE_ALL_TOUCHED", "TRUE");
  std::fill(mask.begin(), mask.end(), 1.0f);
  validity_flag = -1;
  EXPECT_EQ(CE_None,
            GDALWarpCutlineMaskerEx(
                warp_options.get(), /*nBandCount=*/1, GDT_Float32, /*nXOff=*/0,
                /*nYOff=*/0, kXSize, kYSize, /*ppImageData=*/nullptr,
                /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
  EXPECT_EQ(GCMVF_PARTIAL_INTERSECTION, validity_flag);

  // 3. Test GDALWarpCutlineMasker (non-Ex variant without pnValidityFlag).
  std::fill(mask.begin(), mask.end(), 1.0f);
  EXPECT_EQ(CE_None,
            GDALWarpCutlineMasker(warp_options.get(), /*nBandCount=*/1,
                                  GDT_Float32, /*nXOff=*/0, /*nYOff=*/0, kXSize,
                                  kYSize, /*ppImageData=*/nullptr,
                                  /*bMaskIsFloat=*/TRUE, mask.data()));
  for (int y = 0; y < kYSize; y++) {
    for (int x = 0; x < kXSize; x++) {
      const float expected = (x >= 5) ? 1.0f : 0.0f;
      EXPECT_FLOAT_EQ(expected, mask[y * kXSize + x]);
    }
  }
}

TEST_F(GdalcutlineTest, MultiPolygonCutline) {
  WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
  ASSERT_NE(nullptr, warp_options);

  // Cutline as MULTIPOLYGON.
  OGRGeometry* geom = nullptr;
  ASSERT_EQ(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(
                "MULTIPOLYGON(((2 2, 8 2, 8 8, 2 8, 2 2)))", nullptr, &geom));
  warp_options->hCutline = geom;
  warp_options->dfCutlineBlendDist = 0.0;

  constexpr int kXSize = 10;
  constexpr int kYSize = 10;
  std::vector<float> mask(kXSize * kYSize, 1.0f);
  int validity_flag = -1;

  EXPECT_EQ(CE_None,
            GDALWarpCutlineMaskerEx(
                warp_options.get(), /*nBandCount=*/1, GDT_Float32, /*nXOff=*/0,
                /*nYOff=*/0, kXSize, kYSize, /*ppImageData=*/nullptr,
                /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
  EXPECT_EQ(GCMVF_PARTIAL_INTERSECTION, validity_flag);

  // Pixels strictly inside [2, 8) x [2, 8) should be 1.0f; outside should be
  // 0.0f.
  EXPECT_FLOAT_EQ(0.0f, mask[0]);
  EXPECT_FLOAT_EQ(1.0f, mask[5 * kXSize + 5]);
}

TEST_F(GdalcutlineTest, PartialIntersectionWithBlendDistance) {
  WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
  ASSERT_NE(nullptr, warp_options);

  // Cutline covering x in [5, 15], y in [0, 10].
  OGRGeometry* geom = nullptr;
  ASSERT_EQ(OGRERR_NONE,
            OGRGeometryFactory::createFromWkt(
                "POLYGON((5 0, 15 0, 15 10, 5 10, 5 0))", nullptr, &geom));
  warp_options->hCutline = geom;
  warp_options->dfCutlineBlendDist = 2.0;

  constexpr int kXSize = 10;
  constexpr int kYSize = 10;
  std::vector<float> mask(kXSize * kYSize, 1.0f);
  int validity_flag = -1;

  EXPECT_EQ(CE_None,
            GDALWarpCutlineMaskerEx(
                warp_options.get(), /*nBandCount=*/1, GDT_Float32, /*nXOff=*/0,
                /*nYOff=*/0, kXSize, kYSize, /*ppImageData=*/nullptr,
                /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
  EXPECT_EQ(GCMVF_PARTIAL_INTERSECTION, validity_flag);

  // Along y = 5:
  // x = 0, 1, 2: outside blend distance (> 2.0 away) -> 0.0f.
  EXPECT_FLOAT_EQ(0.0f, mask[5 * kXSize + 0]);
  EXPECT_FLOAT_EQ(0.0f, mask[5 * kXSize + 1]);
  EXPECT_FLOAT_EQ(0.0f, mask[5 * kXSize + 2]);

  // x = 4: outside polygon, distance to edge (5) is 0.5 <= 2.0.
  // Ratio is in (0.0, 0.5].
  EXPECT_GT(mask[5 * kXSize + 4], 0.0f);
  EXPECT_LE(mask[5 * kXSize + 4], 0.5f);

  // x = 5: inside polygon, distance to edge is 0.5 <= 2.0.
  // Ratio is in [0.5, 1.0).
  EXPECT_GE(mask[5 * kXSize + 5], 0.5f);
  EXPECT_LT(mask[5 * kXSize + 5], 1.0f);

  // x = 8, 9: inside polygon, far from edge (> 2.0 away) -> 1.0f.
  EXPECT_FLOAT_EQ(1.0f, mask[5 * kXSize + 8]);
  EXPECT_FLOAT_EQ(1.0f, mask[5 * kXSize + 9]);
}

TEST_F(GdalcutlineTest, BlendMaskGeneratorBranches) {
  // 1. Polygon envelope overlaps chunk + blend distance, but polygon itself
  // does not intersect oClipRect (line !poPolygon->Intersects(&oClipRect)).
  {
    WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
    ASSERT_NE(nullptr, warp_options);

    // Diagonal polygon whose envelope is [0, 30] x [0, 30].
    OGRGeometry* geom = nullptr;
    ASSERT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(
                  "POLYGON((0 20, 20 0, 30 0, 0 30, 0 20))", nullptr, &geom));
    warp_options->hCutline = geom;
    warp_options->dfCutlineBlendDist = 1.0;

    // Chunk [0, 5] x [0, 5]. oClipRect is [-2, 7] x [-2, 7].
    // Maximum x + y on oClipRect is 14 < 20, so polygon does not intersect
    // oClipRect.
    constexpr int kXSize = 5;
    constexpr int kYSize = 5;
    std::vector<float> mask(kXSize * kYSize, 1.0f);
    int validity_flag = -1;

    EXPECT_EQ(CE_None, GDALWarpCutlineMaskerEx(
                           warp_options.get(), /*nBandCount=*/1, GDT_Float32,
                           /*nXOff=*/0, /*nYOff=*/0, kXSize, kYSize,
                           /*ppImageData=*/nullptr,
                           /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
    // When poPolygon does not intersect oClipRect, mask is zeroed out.
    for (int i = 0; i < kXSize * kYSize; i++) {
      EXPECT_FLOAT_EQ(0.0f, mask[i]);
    }
  }

  // 2. Large chunk triggering dfLastDist > dfBlendDist + 1.5 distance skip
  // optimization.
  {
    WarpOptionsUniquePtr warp_options(GDALCreateWarpOptions());
    ASSERT_NE(nullptr, warp_options);

    OGRGeometry* geom = nullptr;
    ASSERT_EQ(OGRERR_NONE,
              OGRGeometryFactory::createFromWkt(
                  "POLYGON((0 0, 20 0, 20 20, 0 20, 0 0))", nullptr, &geom));
    warp_options->hCutline = geom;
    warp_options->dfCutlineBlendDist = 2.0;

    constexpr int kXSize = 20;
    constexpr int kYSize = 20;
    std::vector<float> mask(kXSize * kYSize, 1.0f);
    int validity_flag = -1;

    EXPECT_EQ(CE_None, GDALWarpCutlineMaskerEx(
                           warp_options.get(), /*nBandCount=*/1, GDT_Float32,
                           /*nXOff=*/0, /*nYOff=*/0, kXSize, kYSize,
                           /*ppImageData=*/nullptr,
                           /*bMaskIsFloat=*/TRUE, mask.data(), &validity_flag));
    EXPECT_EQ(GCMVF_PARTIAL_INTERSECTION, validity_flag);
    EXPECT_FLOAT_EQ(1.0f, mask[10 * kXSize + 10]);
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - #ifndef HAVE_GEOS stub is not compiled in google3.
