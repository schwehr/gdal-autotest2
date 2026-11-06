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
// Tests low-level rasterization routines (alg/llrasterize.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/llrasterize.cpp

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdal_alg_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

struct PointRecord {
  int y = 0;
  int x = 0;
  double variant = 0.0;

  bool operator==(const PointRecord&) const = default;
};

struct ScanlineRecord {
  int y = 0;
  int xstart = 0;
  int xend = 0;
  double variant = 0.0;

  bool operator==(const ScanlineRecord&) const = default;
};

struct TestTracker {
  std::vector<PointRecord> points;
  std::vector<ScanlineRecord> scanlines;
};

void RecordPoint(GDALRasterizeInfo* cb_data, int y, int x, double variant) {
  if (cb_data != nullptr && cb_data->pabyChunkBuf != nullptr) {
    auto* tracker = reinterpret_cast<TestTracker*>(cb_data->pabyChunkBuf);
    tracker->points.push_back({.y = y, .x = x, .variant = variant});
  }
}

void RecordScanline(GDALRasterizeInfo* cb_data, int y, int xstart, int xend,
                    double variant) {
  if (cb_data != nullptr && cb_data->pabyChunkBuf != nullptr) {
    auto* tracker = reinterpret_cast<TestTracker*>(cb_data->pabyChunkBuf);
    tracker->scanlines.push_back(
        {.y = y, .xstart = xstart, .xend = xend, .variant = variant});
  }
}

GDALRasterizeInfo MakeTrackerInfo(
    TestTracker* tracker, GDALBurnValueSrc burn_source = GBV_UserBurnValue) {
  GDALRasterizeInfo info{};
  info.pabyChunkBuf = reinterpret_cast<unsigned char*>(tracker);
  info.eBurnValueSource = burn_source;
  return info;
}

class RasterBuffer {
 public:
  RasterBuffer(int width, int height, int bands = 1,
               GDALRasterMergeAlg merge_alg = GRMA_Replace,
               GDALBurnValueSrc burn_src = GBV_UserBurnValue)
      : width_(width),
        height_(height),
        bands_(bands),
        data_(width * height * bands, 0.0),
        burn_values_(bands, 100.0) {
    info_.pabyChunkBuf = reinterpret_cast<unsigned char*>(data_.data());
    info_.nXSize = width;
    info_.nYSize = height;
    info_.nBands = bands;
    info_.eType = GDT_Float64;
    info_.nPixelSpace = static_cast<int>(sizeof(double));
    info_.nLineSpace = width * sizeof(double);
    info_.nBandSpace = width * height * sizeof(double);
    info_.eBurnValueType = GDT_Float64;
    info_.burnValues.double_values = burn_values_.data();
    info_.eBurnValueSource = burn_src;
    info_.eMergeAlg = merge_alg;
    info_.bFillSetVisitedPoints = (merge_alg == GRMA_Add);
    if (info_.bFillSetVisitedPoints) {
      visited_points_ = std::make_unique<std::set<uint64_t>>();
      info_.poSetVisitedPoints = visited_points_.get();
    } else {
      info_.poSetVisitedPoints = nullptr;
    }
  }

  void SetBurnValues(const std::vector<double>& values) {
    burn_values_ = values;
    info_.burnValues.double_values = burn_values_.data();
  }

  GDALRasterizeInfo* Info() { return &info_; }

  double GetPixel(int x, int y, int band = 0) const {
    if (x < 0 || x >= width_ || y < 0 || y >= height_ || band < 0 ||
        band >= bands_) {
      return -1.0;
    }
    return data_[band * width_ * height_ + y * width_ + x];
  }

  static void BurnPoint(GDALRasterizeInfo* cb_data, int y, int x,
                        double variant) {
    if (cb_data == nullptr || cb_data->pabyChunkBuf == nullptr) return;
    if (cb_data->bFillSetVisitedPoints &&
        cb_data->poSetVisitedPoints != nullptr) {
      uint64_t key =
          (static_cast<uint64_t>(y) << 32) | static_cast<uint32_t>(x);
      if (!cb_data->poSetVisitedPoints->insert(key).second) {
        return;
      }
    }
    for (int b = 0; b < cb_data->nBands; ++b) {
      double burn_val = (cb_data->burnValues.double_values != nullptr)
                            ? cb_data->burnValues.double_values[b]
                            : 0.0;
      if (cb_data->eBurnValueSource != GBV_UserBurnValue) {
        burn_val += variant;
      }
      auto* ptr = reinterpret_cast<double*>(
          cb_data->pabyChunkBuf + b * cb_data->nBandSpace +
          y * cb_data->nLineSpace + x * cb_data->nPixelSpace);
      if (cb_data->eMergeAlg == GRMA_Add) {
        *ptr += burn_val;
      } else {
        *ptr = burn_val;
      }
    }
  }

  static void BurnScanline(GDALRasterizeInfo* cb_data, int y, int xstart,
                           int xend, double variant) {
    for (int x = xstart; x <= xend; ++x) {
      BurnPoint(cb_data, y, x, variant);
    }
  }

 private:
  int width_;
  int height_;
  int bands_;
  std::vector<double> data_;
  std::vector<double> burn_values_;
  std::unique_ptr<std::set<uint64_t>> visited_points_;
  GDALRasterizeInfo info_{};
};

// ============================================================================
// GDALdllImagePoint Tests
// ============================================================================

TEST(GdalDllImagePointTest, EmptyPartCountReturnsEarly) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  GDALdllImagePoint(10, 10, 0, nullptr, nullptr, nullptr, nullptr, RecordPoint,
                    &info);

  EXPECT_THAT(tracker.points, IsEmpty());
}

TEST(GdalDllImagePointTest, SinglePointInBoundsWithVariant) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  const double x = 2.0;
  const double y = 3.0;
  const double variant = 42.0;
  const int part_size = 1;

  GDALdllImagePoint(5, 5, 1, &part_size, &x, &y, &variant, RecordPoint, &info);

  ASSERT_EQ(1, tracker.points.size());
  EXPECT_EQ(3, tracker.points[0].y);
  EXPECT_EQ(2, tracker.points[0].x);
  EXPECT_DOUBLE_EQ(42.0, tracker.points[0].variant);
}

TEST(GdalDllImagePointTest, SinglePointInBoundsWithoutVariantDefaultsToZero) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  const double x = 1.0;
  const double y = 4.0;
  const int part_size = 1;

  GDALdllImagePoint(5, 5, 1, &part_size, &x, &y, nullptr, RecordPoint, &info);

  ASSERT_EQ(1, tracker.points.size());
  EXPECT_EQ(4, tracker.points[0].y);
  EXPECT_EQ(1, tracker.points[0].x);
  EXPECT_DOUBLE_EQ(0.0, tracker.points[0].variant);
}

TEST(GdalDllImagePointTest, MultiplePointsAndFractionalFloorRounding) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  const std::vector<double> x = {1.2, 3.9, 0.0};
  const std::vector<double> y = {2.8, 0.1, 4.99};
  const std::vector<double> variants = {10.0, 20.0, 30.0};
  const std::vector<int> part_sizes = {1, 1, 1};

  GDALdllImagePoint(5, 5, 3, part_sizes.data(), x.data(), y.data(),
                    variants.data(), RecordPoint, &info);

  EXPECT_THAT(tracker.points,
              ElementsAre(PointRecord{.y = 2, .x = 1, .variant = 10.0},
                          PointRecord{.y = 0, .x = 3, .variant = 20.0},
                          PointRecord{.y = 4, .x = 0, .variant = 30.0}));
}

TEST(GdalDllImagePointTest, OutOfBoundsPointsFilteredOut) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  const std::vector<double> x = {-0.1, 5.0, 2.0, 2.0};
  const std::vector<double> y = {2.0, 2.0, -1.0, 5.0};
  const std::vector<int> part_sizes = {1, 1, 1, 1};

  GDALdllImagePoint(5, 5, 4, part_sizes.data(), x.data(), y.data(), nullptr,
                    RecordPoint, &info);

  EXPECT_THAT(tracker.points, IsEmpty());
}

TEST(GdalDllImagePointTest, MultiBandRasterBufferBurning) {
  RasterBuffer raster(4, 4, /*bands=*/2, GRMA_Replace, GBV_Z);
  raster.SetBurnValues({10.0, 20.0});

  const double x = 2.0;
  const double y = 1.0;
  const double variant = 5.0;
  const int part_size = 1;

  GDALdllImagePoint(4, 4, 1, &part_size, &x, &y, &variant,
                    RasterBuffer::BurnPoint, raster.Info());

  EXPECT_DOUBLE_EQ(15.0, raster.GetPixel(2, 1, 0));
  EXPECT_DOUBLE_EQ(25.0, raster.GetPixel(2, 1, 1));
  EXPECT_DOUBLE_EQ(0.0, raster.GetPixel(0, 0, 0));
}

// ============================================================================
// GDALdllImageLine Tests
// ============================================================================

TEST(GdalDllImageLineTest, EmptyPartCountReturnsEarly) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  GDALdllImageLine(10, 10, 0, nullptr, nullptr, nullptr, nullptr, RecordPoint,
                   &info);

  EXPECT_THAT(tracker.points, IsEmpty());
}

TEST(GdalDllImageLineTest, DegenerateSinglePointLine) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  const std::vector<double> x = {2.0, 2.0};
  const std::vector<double> y = {3.0, 3.0};
  const int part_size = 2;

  GDALdllImageLine(5, 5, 1, &part_size, x.data(), y.data(), nullptr,
                   RecordPoint, &info);

  EXPECT_THAT(tracker.points,
              ElementsAre(PointRecord{.y = 3, .x = 2, .variant = 0.0}));
}

TEST(GdalDllImageLineTest, HorizontalLinesBothDirections) {
  // Left-to-right
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.0, 4.0};
    const std::vector<double> y = {2.0, 2.0};
    const int part_size = 2;

    GDALdllImageLine(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 2, .x = 1, .variant = 0.0},
                            PointRecord{.y = 2, .x = 2, .variant = 0.0},
                            PointRecord{.y = 2, .x = 3, .variant = 0.0},
                            PointRecord{.y = 2, .x = 4, .variant = 0.0}));
  }

  // Right-to-left
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {4.0, 1.0};
    const std::vector<double> y = {2.0, 2.0};
    const int part_size = 2;

    GDALdllImageLine(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 2, .x = 4, .variant = 0.0},
                            PointRecord{.y = 2, .x = 3, .variant = 0.0},
                            PointRecord{.y = 2, .x = 2, .variant = 0.0},
                            PointRecord{.y = 2, .x = 1, .variant = 0.0}));
  }
}

TEST(GdalDllImageLineTest, VerticalLinesBothDirections) {
  // Top-to-bottom
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {2.0, 2.0};
    const std::vector<double> y = {1.0, 4.0};
    const int part_size = 2;

    GDALdllImageLine(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 1, .x = 2, .variant = 0.0},
                            PointRecord{.y = 2, .x = 2, .variant = 0.0},
                            PointRecord{.y = 3, .x = 2, .variant = 0.0},
                            PointRecord{.y = 4, .x = 2, .variant = 0.0}));
  }

  // Bottom-to-top
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {2.0, 2.0};
    const std::vector<double> y = {4.0, 1.0};
    const int part_size = 2;

    GDALdllImageLine(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 4, .x = 2, .variant = 0.0},
                            PointRecord{.y = 3, .x = 2, .variant = 0.0},
                            PointRecord{.y = 2, .x = 2, .variant = 0.0},
                            PointRecord{.y = 1, .x = 2, .variant = 0.0}));
  }
}

TEST(GdalDllImageLineTest, DiagonalLines45DegreesAllDirections) {
  // Octant 1 diagonal (dx > 0, dy > 0)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.0, 3.0};
    const std::vector<double> y = {1.0, 3.0};
    const int part_size = 2;

    GDALdllImageLine(5, 5, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 1, .x = 1, .variant = 0.0},
                            PointRecord{.y = 2, .x = 2, .variant = 0.0},
                            PointRecord{.y = 3, .x = 3, .variant = 0.0}));
  }

  // Octant 4 diagonal (dx < 0, dy > 0)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {3.0, 1.0};
    const std::vector<double> y = {1.0, 3.0};
    const int part_size = 2;

    GDALdllImageLine(5, 5, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 1, .x = 3, .variant = 0.0},
                            PointRecord{.y = 2, .x = 2, .variant = 0.0},
                            PointRecord{.y = 3, .x = 1, .variant = 0.0}));
  }

  // Octant 5 diagonal (dx < 0, dy < 0)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {3.0, 1.0};
    const std::vector<double> y = {3.0, 1.0};
    const int part_size = 2;

    GDALdllImageLine(5, 5, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 3, .x = 3, .variant = 0.0},
                            PointRecord{.y = 2, .x = 2, .variant = 0.0},
                            PointRecord{.y = 1, .x = 1, .variant = 0.0}));
  }

  // Octant 8 diagonal (dx > 0, dy < 0)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.0, 3.0};
    const std::vector<double> y = {3.0, 1.0};
    const int part_size = 2;

    GDALdllImageLine(5, 5, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 3, .x = 1, .variant = 0.0},
                            PointRecord{.y = 2, .x = 2, .variant = 0.0},
                            PointRecord{.y = 1, .x = 3, .variant = 0.0}));
  }
}

TEST(GdalDllImageLineTest, GentleSlopesExercisesErrorAccumulator) {
  // Gentle slope with dx=5, dy=2 (positive dx and dy)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {0.0, 5.0};
    const std::vector<double> y = {0.0, 2.0};
    const int part_size = 2;

    GDALdllImageLine(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 0, .x = 0, .variant = 0.0},
                            PointRecord{.y = 0, .x = 1, .variant = 0.0},
                            PointRecord{.y = 1, .x = 2, .variant = 0.0},
                            PointRecord{.y = 1, .x = 3, .variant = 0.0},
                            PointRecord{.y = 2, .x = 4, .variant = 0.0},
                            PointRecord{.y = 2, .x = 5, .variant = 0.0}));
  }

  // Gentle slope with dx=5, dy=2 (negative dy: y from 2 down to 0)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {0.0, 5.0};
    const std::vector<double> y = {2.0, 0.0};
    const int part_size = 2;

    GDALdllImageLine(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 2, .x = 0, .variant = 0.0},
                            PointRecord{.y = 2, .x = 1, .variant = 0.0},
                            PointRecord{.y = 1, .x = 2, .variant = 0.0},
                            PointRecord{.y = 1, .x = 3, .variant = 0.0},
                            PointRecord{.y = 0, .x = 4, .variant = 0.0},
                            PointRecord{.y = 0, .x = 5, .variant = 0.0}));
  }
}

TEST(GdalDllImageLineTest, SteepSlopesExercisesErrorAccumulator) {
  // Steep slope with dx=2, dy=5 (positive dx and dy)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {0.0, 2.0};
    const std::vector<double> y = {0.0, 5.0};
    const int part_size = 2;

    GDALdllImageLine(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 0, .x = 0, .variant = 0.0},
                            PointRecord{.y = 1, .x = 0, .variant = 0.0},
                            PointRecord{.y = 2, .x = 1, .variant = 0.0},
                            PointRecord{.y = 3, .x = 1, .variant = 0.0},
                            PointRecord{.y = 4, .x = 2, .variant = 0.0},
                            PointRecord{.y = 5, .x = 2, .variant = 0.0}));
  }

  // Steep slope with dx=2, dy=5 (negative dx: x from 2 down to 0)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {2.0, 0.0};
    const std::vector<double> y = {0.0, 5.0};
    const int part_size = 2;

    GDALdllImageLine(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 0, .x = 2, .variant = 0.0},
                            PointRecord{.y = 1, .x = 2, .variant = 0.0},
                            PointRecord{.y = 2, .x = 1, .variant = 0.0},
                            PointRecord{.y = 3, .x = 1, .variant = 0.0},
                            PointRecord{.y = 4, .x = 0, .variant = 0.0},
                            PointRecord{.y = 5, .x = 0, .variant = 0.0}));
  }
}

TEST(GdalDllImageLineTest, MultiSegmentIntermediatePointAvoidanceInAddMode) {
  // Gentle slope multi-segment line
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

    const std::vector<double> x = {1.0, 3.0, 5.0};
    const std::vector<double> y = {1.0, 1.0, 1.0};
    const int part_size = 3;

    GDALdllImageLine(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    // Intermediate point (3, 1) must be burned exactly once, not twice.
    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 1, .x = 1, .variant = 0.0},
                            PointRecord{.y = 1, .x = 2, .variant = 0.0},
                            PointRecord{.y = 1, .x = 3, .variant = 0.0},
                            PointRecord{.y = 1, .x = 4, .variant = 0.0},
                            PointRecord{.y = 1, .x = 5, .variant = 0.0}));
  }

  // Steep slope multi-segment line
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

    const std::vector<double> x = {1.0, 1.0, 1.0};
    const std::vector<double> y = {1.0, 3.0, 5.0};
    const int part_size = 3;

    GDALdllImageLine(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                     RecordPoint, &info);

    // Intermediate point (1, 3) must be burned exactly once, not twice.
    EXPECT_THAT(tracker.points,
                ElementsAre(PointRecord{.y = 1, .x = 1, .variant = 0.0},
                            PointRecord{.y = 2, .x = 1, .variant = 0.0},
                            PointRecord{.y = 3, .x = 1, .variant = 0.0},
                            PointRecord{.y = 4, .x = 1, .variant = 0.0},
                            PointRecord{.y = 5, .x = 1, .variant = 0.0}));
  }
}

TEST(GdalDllImageLineTest, MultiPartLineRasterization) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  const std::vector<double> x = {0.0, 2.0, 0.0, 2.0};
  const std::vector<double> y = {0.0, 0.0, 3.0, 3.0};
  const std::vector<int> part_sizes = {2, 2};

  GDALdllImageLine(5, 5, 2, part_sizes.data(), x.data(), y.data(), nullptr,
                   RecordPoint, &info);

  EXPECT_THAT(tracker.points,
              ElementsAre(PointRecord{.y = 0, .x = 0, .variant = 0.0},
                          PointRecord{.y = 0, .x = 1, .variant = 0.0},
                          PointRecord{.y = 0, .x = 2, .variant = 0.0},
                          PointRecord{.y = 3, .x = 0, .variant = 0.0},
                          PointRecord{.y = 3, .x = 1, .variant = 0.0},
                          PointRecord{.y = 3, .x = 2, .variant = 0.0}));
}

TEST(GdalDllImageLineTest, VariantInterpolationWithZSourceAndUserSource) {
  // Gentle slope with GBV_Z source
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker, GBV_Z);

    const std::vector<double> x = {0.0, 4.0};
    const std::vector<double> y = {1.0, 1.0};
    const std::vector<double> variants = {10.0, 30.0};
    const int part_size = 2;

    GDALdllImageLine(5, 5, 1, &part_size, x.data(), y.data(), variants.data(),
                     RecordPoint, &info);

    ASSERT_EQ(5, tracker.points.size());
    EXPECT_DOUBLE_EQ(10.0, tracker.points[0].variant);
    EXPECT_DOUBLE_EQ(15.0, tracker.points[1].variant);
    EXPECT_DOUBLE_EQ(20.0, tracker.points[2].variant);
    EXPECT_DOUBLE_EQ(25.0, tracker.points[3].variant);
    EXPECT_DOUBLE_EQ(30.0, tracker.points[4].variant);
  }

  // Steep slope with GBV_Z source
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker, GBV_Z);

    const std::vector<double> x = {1.0, 1.0};
    const std::vector<double> y = {0.0, 4.0};
    const std::vector<double> variants = {10.0, 30.0};
    const int part_size = 2;

    GDALdllImageLine(5, 5, 1, &part_size, x.data(), y.data(), variants.data(),
                     RecordPoint, &info);

    ASSERT_EQ(5, tracker.points.size());
    EXPECT_DOUBLE_EQ(10.0, tracker.points[0].variant);
    EXPECT_DOUBLE_EQ(15.0, tracker.points[1].variant);
    EXPECT_DOUBLE_EQ(20.0, tracker.points[2].variant);
    EXPECT_DOUBLE_EQ(25.0, tracker.points[3].variant);
    EXPECT_DOUBLE_EQ(30.0, tracker.points[4].variant);
  }

  // GBV_UserBurnValue ignores padfVariant
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker, GBV_UserBurnValue);

    const std::vector<double> x = {0.0, 2.0};
    const std::vector<double> y = {1.0, 1.0};
    const std::vector<double> variants = {10.0, 30.0};
    const int part_size = 2;

    GDALdllImageLine(5, 5, 1, &part_size, x.data(), y.data(), variants.data(),
                     RecordPoint, &info);

    ASSERT_EQ(3, tracker.points.size());
    EXPECT_DOUBLE_EQ(0.0, tracker.points[0].variant);
    EXPECT_DOUBLE_EQ(0.0, tracker.points[1].variant);
    EXPECT_DOUBLE_EQ(0.0, tracker.points[2].variant);
  }
}

TEST(GdalDllImageLineTest, OutOfBoundsLineClipping) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  // Line crossing image from negative x to beyond width
  const std::vector<double> x = {-2.0, 6.0};
  const std::vector<double> y = {1.0, 1.0};
  const int part_size = 2;

  GDALdllImageLine(4, 4, 1, &part_size, x.data(), y.data(), nullptr,
                   RecordPoint, &info);

  EXPECT_THAT(tracker.points,
              ElementsAre(PointRecord{.y = 1, .x = 0, .variant = 0.0},
                          PointRecord{.y = 1, .x = 1, .variant = 0.0},
                          PointRecord{.y = 1, .x = 2, .variant = 0.0},
                          PointRecord{.y = 1, .x = 3, .variant = 0.0}));
}

// ============================================================================
// GDALdllImageLineAllTouched Tests
// ============================================================================

TEST(GdalDllImageLineAllTouchedTest, EmptyPartCountReturnsEarly) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  GDALdllImageLineAllTouched(10, 10, 0, nullptr, nullptr, nullptr, nullptr,
                             RecordPoint, &info, false, false);

  EXPECT_THAT(tracker.points, IsEmpty());
}

TEST(GdalDllImageLineAllTouchedTest, OffTargetSegmentsSkipped) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
  const int part_size = 2;

  // dfY < 0.0 && dfYEnd < 0.0
  {
    const std::vector<double> x = {2.0, 4.0};
    const std::vector<double> y = {-5.0, -1.0};
    GDALdllImageLineAllTouched(10, 10, 1, &part_size, x.data(), y.data(),
                               nullptr, RecordPoint, &info, false, false);
    EXPECT_THAT(tracker.points, IsEmpty());
  }

  // dfY > nRasterYSize && dfYEnd > nRasterYSize
  {
    const std::vector<double> x = {2.0, 4.0};
    const std::vector<double> y = {15.0, 20.0};
    GDALdllImageLineAllTouched(10, 10, 1, &part_size, x.data(), y.data(),
                               nullptr, RecordPoint, &info, false, false);
    EXPECT_THAT(tracker.points, IsEmpty());
  }

  // dfX < 0.0 && dfXEnd < 0.0
  {
    const std::vector<double> x = {-5.0, -1.0};
    const std::vector<double> y = {2.0, 4.0};
    GDALdllImageLineAllTouched(10, 10, 1, &part_size, x.data(), y.data(),
                               nullptr, RecordPoint, &info, false, false);
    EXPECT_THAT(tracker.points, IsEmpty());
  }

  // dfX > nRasterXSize && dfXEnd > nRasterXSize
  {
    const std::vector<double> x = {15.0, 20.0};
    const std::vector<double> y = {2.0, 4.0};
    GDALdllImageLineAllTouched(10, 10, 1, &part_size, x.data(), y.data(),
                               nullptr, RecordPoint, &info, false, false);
    EXPECT_THAT(tracker.points, IsEmpty());
  }
}

TEST(GdalDllImageLineAllTouchedTest, VerticalLinesDirectionAndVariants) {
  // Top-to-bottom
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker, GBV_Z);
    const std::vector<double> x = {2.5, 2.5};
    const std::vector<double> y = {1.0, 4.0};
    const std::vector<double> variants = {10.0, 40.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(),
                               variants.data(), RecordPoint, &info, false,
                               false);

    ASSERT_EQ(3, tracker.points.size());
    EXPECT_EQ(1, tracker.points[0].y);
    EXPECT_EQ(2, tracker.points[0].x);
    EXPECT_DOUBLE_EQ(10.0, tracker.points[0].variant);
    EXPECT_EQ(2, tracker.points[1].y);
    EXPECT_EQ(2, tracker.points[1].x);
    EXPECT_DOUBLE_EQ(20.0, tracker.points[1].variant);
    EXPECT_EQ(3, tracker.points[2].y);
    EXPECT_EQ(2, tracker.points[2].x);
    EXPECT_DOUBLE_EQ(30.0, tracker.points[2].variant);
  }

  // Bottom-to-top (swapped dfY)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {2.5, 2.5};
    const std::vector<double> y = {4.0, 1.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    ASSERT_EQ(3, tracker.points.size());
    EXPECT_EQ(1, tracker.points[0].y);
    EXPECT_EQ(2, tracker.points[1].y);
    EXPECT_EQ(3, tracker.points[2].y);
  }

  // Single-point vertical line (dfY == dfYEnd)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {2.5, 2.5};
    const std::vector<double> y = {1.5, 1.5};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    ASSERT_EQ(1, tracker.points.size());
    EXPECT_EQ(1, tracker.points[0].y);
    EXPECT_EQ(2, tracker.points[0].x);
  }
}

TEST(GdalDllImageLineAllTouchedTest, VerticalLineIntersectOnly) {
  // Integer X coordinate: skipped when bIntersectOnly is true
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {3.0, 3.0};
    const std::vector<double> y = {1.0, 4.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false,
                               /*bIntersectOnly=*/true);

    EXPECT_THAT(tracker.points, IsEmpty());
  }

  // Non-integer X coordinate: not skipped when bIntersectOnly is true
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {3.5, 3.5};
    const std::vector<double> y = {1.0, 4.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false,
                               /*bIntersectOnly=*/true);

    EXPECT_FALSE(tracker.points.empty());
  }
}

TEST(GdalDllImageLineAllTouchedTest, VerticalLineClippingAndOutOfBounds) {
  // iX out of bounds
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    // x near raster border such that dfXEnd is out of bounds
    const std::vector<double> x = {6.0 - 0.005, 6.0 + 0.002};
    const std::vector<double> y = {1.0, 4.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    EXPECT_THAT(tracker.points, IsEmpty());
  }

  // iY clipped to 0 and iYEnd clipped to nRasterYSize - 1
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {2.5, 2.5};
    const std::vector<double> y = {-2.0, 8.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(4, 4, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    ASSERT_EQ(4, tracker.points.size());
    EXPECT_EQ(0, tracker.points[0].y);
    EXPECT_EQ(1, tracker.points[1].y);
    EXPECT_EQ(2, tracker.points[2].y);
    EXPECT_EQ(3, tracker.points[3].y);
  }
}

TEST(GdalDllImageLineAllTouchedTest, VerticalLineAvoidBurningSamePoints) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  // Two segments sharing vertex at y=3.0
  const std::vector<double> x = {2.5, 2.5, 2.5};
  const std::vector<double> y = {1.0, 3.0, 5.0};
  const int part_size = 3;

  GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                             RecordPoint, &info,
                             /*bAvoidBurningSamePoints=*/true, false);

  // Each pixel along the line should be burned once
  EXPECT_THAT(tracker.points,
              ElementsAre(PointRecord{.y = 1, .x = 2, .variant = 0.0},
                          PointRecord{.y = 2, .x = 2, .variant = 0.0},
                          PointRecord{.y = 3, .x = 2, .variant = 0.0},
                          PointRecord{.y = 4, .x = 2, .variant = 0.0}));
}

TEST(GdalDllImageLineAllTouchedTest, HorizontalLinesDirectionAndVariants) {
  // Left-to-right
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker, GBV_Z);
    const std::vector<double> x = {1.0, 4.0};
    const std::vector<double> y = {2.5, 2.5};
    const std::vector<double> variants = {10.0, 40.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(),
                               variants.data(), RecordPoint, &info, false,
                               false);

    ASSERT_EQ(3, tracker.points.size());
    EXPECT_EQ(2, tracker.points[0].y);
    EXPECT_EQ(1, tracker.points[0].x);
    EXPECT_DOUBLE_EQ(10.0, tracker.points[0].variant);
    EXPECT_EQ(2, tracker.points[1].y);
    EXPECT_EQ(2, tracker.points[1].x);
    EXPECT_DOUBLE_EQ(20.0, tracker.points[1].variant);
    EXPECT_EQ(2, tracker.points[2].y);
    EXPECT_EQ(3, tracker.points[2].x);
    EXPECT_DOUBLE_EQ(30.0, tracker.points[2].variant);
  }

  // Right-to-left (swapped dfX)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {4.0, 1.0};
    const std::vector<double> y = {2.5, 2.5};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    ASSERT_EQ(3, tracker.points.size());
    EXPECT_EQ(1, tracker.points[0].x);
    EXPECT_EQ(2, tracker.points[1].x);
    EXPECT_EQ(3, tracker.points[2].x);
  }

  // Single point horizontal line (dfX == dfXEnd)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.5, 1.5};
    const std::vector<double> y = {2.5, 2.5};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    ASSERT_EQ(1, tracker.points.size());
    EXPECT_EQ(2, tracker.points[0].y);
    EXPECT_EQ(1, tracker.points[0].x);
  }
}

TEST(GdalDllImageLineAllTouchedTest, HorizontalLineIntersectOnly) {
  // Integer Y coordinate: skipped when bIntersectOnly is true
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.0, 4.0};
    const std::vector<double> y = {3.0, 3.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false,
                               /*bIntersectOnly=*/true);

    EXPECT_THAT(tracker.points, IsEmpty());
  }

  // Non-integer Y coordinate: not skipped when bIntersectOnly is true
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.0, 4.0};
    const std::vector<double> y = {3.5, 3.5};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false,
                               /*bIntersectOnly=*/true);

    EXPECT_FALSE(tracker.points.empty());
  }
}

TEST(GdalDllImageLineAllTouchedTest, HorizontalLineClippingAndOutOfBounds) {
  // iY out of bounds
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.0, 4.0};
    const std::vector<double> y = {-0.005, 0.002};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    EXPECT_THAT(tracker.points, IsEmpty());
  }

  // iX clipped to 0 and iXEnd clipped to nRasterXSize - 1
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {-2.0, 8.0};
    const std::vector<double> y = {2.5, 2.5};
    const int part_size = 2;

    GDALdllImageLineAllTouched(4, 4, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    ASSERT_EQ(4, tracker.points.size());
    EXPECT_EQ(0, tracker.points[0].x);
    EXPECT_EQ(1, tracker.points[1].x);
    EXPECT_EQ(2, tracker.points[2].x);
    EXPECT_EQ(3, tracker.points[3].x);
  }
}

TEST(GdalDllImageLineAllTouchedTest, HorizontalLineAvoidBurningSamePoints) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  // Two segments sharing vertex at x=3.0
  const std::vector<double> x = {1.0, 3.0, 5.0};
  const std::vector<double> y = {2.5, 2.5, 2.5};
  const int part_size = 3;

  GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                             RecordPoint, &info,
                             /*bAvoidBurningSamePoints=*/true, false);

  EXPECT_THAT(tracker.points,
              ElementsAre(PointRecord{.y = 2, .x = 1, .variant = 0.0},
                          PointRecord{.y = 2, .x = 2, .variant = 0.0},
                          PointRecord{.y = 2, .x = 3, .variant = 0.0},
                          PointRecord{.y = 2, .x = 4, .variant = 0.0}));
}

TEST(GdalDllImageLineAllTouchedTest, SlopedLinesSteppingAndClipping) {
  // Gentle slope inside raster
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker, GBV_Z);
    const std::vector<double> x = {1.0, 4.0};
    const std::vector<double> y = {1.2, 1.8};
    const std::vector<double> variants = {10.0, 40.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(),
                               variants.data(), RecordPoint, &info, false,
                               false);

    EXPECT_FALSE(tracker.points.empty());
  }

  // Right-to-left sloped line (tests dfX > dfXEnd swap)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {4.0, 1.0};
    const std::vector<double> y = {1.8, 1.2};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    EXPECT_FALSE(tracker.points.empty());
  }

  // X clipping: dfXEnd > nRasterXSize and dfX < 0.0
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker, GBV_Z);
    const std::vector<double> x = {-2.0, 10.0};
    const std::vector<double> y = {1.0, 3.0};
    const std::vector<double> variants = {0.0, 100.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(),
                               variants.data(), RecordPoint, &info, false,
                               false);

    for (const auto& pt : tracker.points) {
      EXPECT_GE(pt.x, 0);
      EXPECT_LT(pt.x, 6);
    }
  }

  // Y clipping with positive slope (dfY < 0 and dfYEnd >= nRasterYSize)
  // Subcase: dfXEnd <= nRasterXSize
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker, GBV_Z);
    const std::vector<double> x = {1.0, 2.0};
    const std::vector<double> y = {-2.0, 12.0};
    const std::vector<double> variants = {0.0, 100.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(),
                               variants.data(), RecordPoint, &info, false,
                               false);

    for (const auto& pt : tracker.points) {
      EXPECT_GE(pt.y, 0);
      EXPECT_LT(pt.y, 6);
    }
  }

  // Subcase: dfXEnd > nRasterXSize during bottom Y clipping
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {2.0, 8.0};
    const std::vector<double> y = {1.0, 20.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    for (const auto& pt : tracker.points) {
      EXPECT_GE(pt.x, 0);
      EXPECT_LT(pt.x, 6);
      EXPECT_GE(pt.y, 0);
      EXPECT_LT(pt.y, 6);
    }
  }

  // Y clipping with negative slope (dfY >= nRasterYSize and dfYEnd < 0.0)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker, GBV_Z);
    const std::vector<double> x = {1.0, 5.0};
    const std::vector<double> y = {10.0, -2.0};
    const std::vector<double> variants = {0.0, 100.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(),
                               variants.data(), RecordPoint, &info, false,
                               false);

    for (const auto& pt : tracker.points) {
      EXPECT_GE(pt.x, 0);
      EXPECT_LT(pt.x, 6);
      EXPECT_GE(pt.y, 0);
      EXPECT_LT(pt.y, 6);
    }
  }
}

TEST(GdalDllImageLineAllTouchedTest, SlopedLineSteppingClampingEpsilons) {
  // Steep positive slope starting near top of pixel to hit (dfStepY < 1e-9)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.0, 2.0};
    const std::vector<double> y = {1.9999999999999, 5.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    EXPECT_FALSE(tracker.points.empty());
  }

  // Steep negative slope starting near bottom of pixel to hit (dfStepY > -1e-9)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.0, 2.0};
    const std::vector<double> y = {4.0000000000001, 1.0};
    const int part_size = 2;

    GDALdllImageLineAllTouched(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                               RecordPoint, &info, false, false);

    EXPECT_FALSE(tracker.points.empty());
  }
}

TEST(GdalDllImageLineAllTouchedTest, SlopedLineAvoidBurningSamePoints) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  // Multi-segment sloped line
  const std::vector<double> x = {1.2, 3.5, 5.8};
  const std::vector<double> y = {1.2, 3.5, 5.8};
  const int part_size = 3;

  GDALdllImageLineAllTouched(7, 7, 1, &part_size, x.data(), y.data(), nullptr,
                             RecordPoint, &info,
                             /*bAvoidBurningSamePoints=*/true, false);

  std::set<std::pair<int, int>> unique_points;
  for (const auto& pt : tracker.points) {
    auto res = unique_points.insert({pt.y, pt.x});
    EXPECT_TRUE(res.second);  // Point should not have been burned twice
  }
}

// ============================================================================
// GDALdllImageFilledPolygon Tests
// ============================================================================

TEST(GdalDllImageFilledPolygonTest, EmptyPartCountReturnsEarly) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  GDALdllImageFilledPolygon(10, 10, 0, nullptr, nullptr, nullptr, nullptr,
                            RecordScanline, &info, false);

  EXPECT_THAT(tracker.scanlines, IsEmpty());
}

TEST(GdalDllImageFilledPolygonTest, TriangleInBoundsWithAndWithoutVariant) {
  // Without variant
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.0, 5.0, 3.0};
    const std::vector<double> y = {1.0, 1.0, 4.0};
    const int part_size = 3;

    GDALdllImageFilledPolygon(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                              RecordScanline, &info, false);

    EXPECT_FALSE(tracker.scanlines.empty());
    for (const auto& sl : tracker.scanlines) {
      EXPECT_DOUBLE_EQ(0.0, sl.variant);
    }
  }

  // With variant
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {1.0, 5.0, 3.0};
    const std::vector<double> y = {1.0, 1.0, 4.0};
    const double variant = 77.0;
    const int part_size = 3;

    GDALdllImageFilledPolygon(6, 6, 1, &part_size, x.data(), y.data(), &variant,
                              RecordScanline, &info, false);

    EXPECT_FALSE(tracker.scanlines.empty());
    for (const auto& sl : tracker.scanlines) {
      EXPECT_DOUBLE_EQ(77.0, sl.variant);
    }
  }
}

TEST(GdalDllImageFilledPolygonTest, YBoundingBoxClipping) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  // Polygon extending above and below raster bounds (miny < 0, maxy >=
  // nRasterYSize)
  const std::vector<double> x = {1.0, 5.0, 3.0};
  const std::vector<double> y = {-3.0, -3.0, 10.0};
  const int part_size = 3;

  GDALdllImageFilledPolygon(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                            RecordScanline, &info, false);

  for (const auto& sl : tracker.scanlines) {
    EXPECT_GE(sl.y, 0);
    EXPECT_LT(sl.y, 6);
  }
}

TEST(GdalDllImageFilledPolygonTest, XBoundingBoxClipping) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  // Polygon completely to right of raster (polyInts[i] > maxx)
  {
    const std::vector<double> x = {12.0, 16.0, 14.0};
    const std::vector<double> y = {1.0, 1.0, 4.0};
    const int part_size = 3;

    GDALdllImageFilledPolygon(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                              RecordScanline, &info, false);

    EXPECT_THAT(tracker.scanlines, IsEmpty());
  }

  // Polygon completely to left of raster (polyInts[i + 1] <= minx)
  {
    const std::vector<double> x = {-8.0, -2.0, -5.0};
    const std::vector<double> y = {1.0, 1.0, 4.0};
    const int part_size = 3;

    GDALdllImageFilledPolygon(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                              RecordScanline, &info, false);

    EXPECT_THAT(tracker.scanlines, IsEmpty());
  }
}

TEST(GdalDllImageFilledPolygonTest, MultiPartPolygonWithHole) {
  TestTracker tracker;
  GDALRasterizeInfo info = MakeTrackerInfo(&tracker);

  // Outer square: (1, 1) -> (7, 1) -> (7, 7) -> (1, 7)
  // Inner square (hole): (3, 3) -> (5, 3) -> (5, 5) -> (3, 5)
  const std::vector<double> x = {1.0, 7.0, 7.0, 1.0, 3.0, 5.0, 5.0, 3.0};
  const std::vector<double> y = {1.0, 1.0, 7.0, 7.0, 3.0, 3.0, 5.0, 5.0};
  const std::vector<int> part_sizes = {4, 4};

  GDALdllImageFilledPolygon(8, 8, 2, part_sizes.data(), x.data(), y.data(),
                            nullptr, RecordScanline, &info, false);

  // For y=4, there should be two separate scanline segments on either side of
  // the hole
  int count_y4 = 0;
  for (const auto& sl : tracker.scanlines) {
    if (sl.y == 4) {
      count_y4++;
    }
  }
  EXPECT_EQ(2, count_y4);
}

TEST(GdalDllImageFilledPolygonTest, HorizontalEdgesHandling) {
  // Top horizontal edge (padfX[ind1] <= padfX[ind2] at y+0.5) hits 'else
  // continue;'
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    // Vertices wound so top edge at y=2.5 goes from x=2 to x=5
    const std::vector<double> x = {2.0, 5.0, 3.5};
    const std::vector<double> y = {2.5, 2.5, 4.5};
    const int part_size = 3;

    GDALdllImageFilledPolygon(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                              RecordScanline, &info, false);

    EXPECT_FALSE(tracker.scanlines.empty());
  }

  // Bottom horizontal edge (padfX[ind1] > padfX[ind2] at y+0.5)
  // Case A: horizontal_x1 > maxx -> continue
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {10.0, 7.0, 8.5};
    const std::vector<double> y = {2.5, 2.5, 0.5};
    const int part_size = 3;

    GDALdllImageFilledPolygon(5, 5, 1, &part_size, x.data(), y.data(), nullptr,
                              RecordScanline, &info, false);

    EXPECT_THAT(tracker.scanlines, IsEmpty());
  }

  // Case B: horizontal_x2 <= minx -> continue
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {-1.0, -5.0, -3.0};
    const std::vector<double> y = {2.5, 2.5, 0.5};
    const int part_size = 3;

    GDALdllImageFilledPolygon(5, 5, 1, &part_size, x.data(), y.data(), nullptr,
                              RecordScanline, &info, false);

    EXPECT_THAT(tracker.scanlines, IsEmpty());
  }

  // Case C: In-bounds bottom horizontal edge with bAvoidBurningSamePoints =
  // false
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    const std::vector<double> x = {5.0, 2.0, 3.5};
    const std::vector<double> y = {2.5, 2.5, 0.5};
    const int part_size = 3;

    GDALdllImageFilledPolygon(6, 6, 1, &part_size, x.data(), y.data(), nullptr,
                              RecordScanline, &info, false);

    EXPECT_FALSE(tracker.scanlines.empty());
  }

  // Case D: In-bounds bottom horizontal edge with bAvoidBurningSamePoints =
  // true Subcase 1: Common segment skipped
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    // Part 1 on the left [0, 2], Part 2 on the right [4, 7] with bottom edge at
    // y=2.5
    const std::vector<double> x = {0.0, 2.0, 2.0, 0.0, 4.0, 4.0, 7.0, 7.0};
    const std::vector<double> y = {0.0, 0.0, 4.0, 4.0, 2.5, 4.0, 4.0, 2.5};
    const std::vector<int> part_sizes = {4, 4};

    GDALdllImageFilledPolygon(8, 8, 2, part_sizes.data(), x.data(), y.data(),
                              nullptr, RecordScanline, &info,
                              /*bAvoidBurningSamePoints=*/true);

    EXPECT_FALSE(tracker.scanlines.empty());
  }

  // Subcase 2: Non-matching polyInts2 segment burned (i + 1 >= ints)
  {
    TestTracker tracker;
    GDALRasterizeInfo info = MakeTrackerInfo(&tracker);
    // Polygon with bottom horizontal edge going downwards
    const std::vector<double> x = {0.0, 2.0, 2.0, 0.0, 7.0, 4.0, 5.5};
    const std::vector<double> y = {0.0, 0.0, 4.0, 4.0, 2.5, 2.5, 1.0};
    const std::vector<int> part_sizes = {4, 3};

    GDALdllImageFilledPolygon(8, 8, 2, part_sizes.data(), x.data(), y.data(),
                              nullptr, RecordScanline, &info,
                              /*bAvoidBurningSamePoints=*/true);

    EXPECT_THAT(tracker.scanlines,
                testing::Contains(ScanlineRecord{
                    .y = 2, .xstart = 4, .xend = 6, .variant = 0.0}));
  }
}

TEST(GdalDllImageFilledPolygonTest, MultiBandRasterBufferEndToEnd) {
  RasterBuffer raster(8, 8, /*bands=*/2, GRMA_Replace, GBV_Z);
  raster.SetBurnValues({10.0, 20.0});

  const std::vector<double> x = {2.0, 6.0, 6.0, 2.0};
  const std::vector<double> y = {2.0, 2.0, 6.0, 6.0};
  const double variant = 5.0;
  const int part_size = 4;

  GDALdllImageFilledPolygon(8, 8, 1, &part_size, x.data(), y.data(), &variant,
                            RasterBuffer::BurnScanline, raster.Info(), false);

  // Center of polygon at (4, 4) should be burned in both bands
  EXPECT_DOUBLE_EQ(15.0, raster.GetPixel(4, 4, 0));
  EXPECT_DOUBLE_EQ(25.0, raster.GetPixel(4, 4, 1));
  // Corner at (0, 0) should remain unburned
  EXPECT_DOUBLE_EQ(0.0, raster.GetPixel(0, 0, 0));
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Low-level rasterizer edge conditions and SSE/AVX
// branches.
