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
// Unit tests for libharp area mask operations (libharp/harp-area-mask.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-area-mask.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-area-mask.h

#include <cmath>
#include <cstdlib>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

#if defined(__SANITIZE_ADDRESS__)
#define HARP_TEST_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HARP_TEST_HAS_ASAN 1
#endif
#endif

ScopedHarpSphericalPolygon MakeSquareDeg(double lat0, double lon0, double lat1,
                                         double lon1) {
  double lats[2] = {lat0, lat1};
  double lons[2] = {lon0, lon1};
  harp_spherical_polygon* raw = nullptr;
  if (harp_spherical_polygon_from_latitude_longitude_bounds(0, 2, lats, lons, 1,
                                                            &raw) != 0) {
    return nullptr;
  }
  return ScopedHarpSphericalPolygon(raw);
}

harp_spherical_point PointDeg(double lat_deg, double lon_deg) {
  harp_spherical_point p = {.lon = lon_deg, .lat = lat_deg};
  harp_spherical_point_rad_from_deg(&p);
  harp_spherical_point_check(&p);
  return p;
}

TEST(HarpAreaMaskTest, NewAddPolygonGrowPastBlockSizeAndDelete) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_area_mask_delete(nullptr);

  harp_area_mask* raw_mask = nullptr;
  ASSERT_EQ(0, harp_area_mask_new(&raw_mask));
  ScopedHarpAreaMask mask(raw_mask);
  EXPECT_EQ(0, mask->num_polygons);

  // Invalid bowtie polygon is rejected and caller retains ownership.
  double bowtie_lats[4] = {0.0, 10.0, 10.0, 0.0};
  double bowtie_lons[4] = {0.0, 10.0, 0.0, 10.0};
  harp_spherical_polygon* raw_bowtie = nullptr;
  ASSERT_EQ(0, harp_spherical_polygon_from_latitude_longitude_bounds(
                   0, 4, bowtie_lats, bowtie_lons, 0, &raw_bowtie));
  ScopedHarpSphericalPolygon bowtie(raw_bowtie);
  EXPECT_EQ(-1, harp_area_mask_add_polygon(mask.get(), bowtie.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(0, mask->num_polygons);

  // Add > BLOCK_SIZE (16) polygons to exercise realloc block growth.
  for (int i = 0; i < BLOCK_SIZE + 3; ++i) {
    double lat0 = static_cast<double>(i);
    ScopedHarpSphericalPolygon sq = MakeSquareDeg(lat0, 0.0, lat0 + 0.5, 1.0);
    ASSERT_NE(nullptr, sq);
    ASSERT_EQ(0, harp_area_mask_add_polygon(mask.get(), sq.get()));
    sq.release();
  }
  EXPECT_EQ(BLOCK_SIZE + 3, mask->num_polygons);
}

TEST(HarpAreaMaskTest, SpatialQueriesPointAndAreaPredicates) {
  harp_area_mask* raw_mask = nullptr;
  ASSERT_EQ(0, harp_area_mask_new(&raw_mask));
  ScopedHarpAreaMask mask(raw_mask);

  // Empty mask returns 0 for all queries.
  const harp_spherical_point p_in = PointDeg(5.0, 5.0);
  ScopedHarpSphericalPolygon query_small = MakeSquareDeg(2.0, 2.0, 8.0, 8.0);
  ASSERT_NE(nullptr, query_small);
  EXPECT_EQ(0, harp_area_mask_covers_point(mask.get(), &p_in));
  EXPECT_EQ(0, harp_area_mask_covers_area(mask.get(), query_small.get()));
  EXPECT_EQ(0, harp_area_mask_inside_area(mask.get(), query_small.get()));
  EXPECT_EQ(0, harp_area_mask_intersects_area(mask.get(), query_small.get()));
  EXPECT_EQ(0, harp_area_mask_intersects_area_with_fraction(
                   mask.get(), query_small.get(), 0.1));

  // Add two disjoint polygons: [0..10]x[0..10] and [20..30]x[20..30].
  ScopedHarpSphericalPolygon poly1 = MakeSquareDeg(0.0, 0.0, 10.0, 10.0);
  ScopedHarpSphericalPolygon poly2 = MakeSquareDeg(20.0, 20.0, 30.0, 30.0);
  ASSERT_NE(nullptr, poly1);
  ASSERT_NE(nullptr, poly2);
  ASSERT_EQ(0, harp_area_mask_add_polygon(mask.get(), poly1.get()));
  poly1.release();
  ASSERT_EQ(0, harp_area_mask_add_polygon(mask.get(), poly2.get()));
  poly2.release();

  // Point queries.
  const harp_spherical_point p_in2 = PointDeg(25.0, 25.0);
  const harp_spherical_point p_out = PointDeg(50.0, 50.0);
  EXPECT_EQ(1, harp_area_mask_covers_point(mask.get(), &p_in));
  EXPECT_EQ(1, harp_area_mask_covers_point(mask.get(), &p_in2));
  EXPECT_EQ(0, harp_area_mask_covers_point(mask.get(), &p_out));

  // Area coverage / inside / intersection queries.
  ScopedHarpSphericalPolygon query_big = MakeSquareDeg(-5.0, -5.0, 15.0, 15.0);
  ScopedHarpSphericalPolygon query_partial =
      MakeSquareDeg(5.0, 5.0, 15.0, 15.0);
  ScopedHarpSphericalPolygon query_disjoint =
      MakeSquareDeg(40.0, 40.0, 50.0, 50.0);
  ASSERT_NE(nullptr, query_big);
  ASSERT_NE(nullptr, query_partial);
  ASSERT_NE(nullptr, query_disjoint);

  EXPECT_EQ(1, harp_area_mask_covers_area(mask.get(), query_small.get()));
  EXPECT_EQ(0, harp_area_mask_covers_area(mask.get(), query_big.get()));

  EXPECT_EQ(1, harp_area_mask_inside_area(mask.get(), query_big.get()));
  EXPECT_EQ(0, harp_area_mask_inside_area(mask.get(), query_small.get()));

  EXPECT_EQ(1, harp_area_mask_intersects_area(mask.get(), query_partial.get()));
  EXPECT_EQ(0,
            harp_area_mask_intersects_area(mask.get(), query_disjoint.get()));

  // Overlap fraction of [0..10]x[0..10] and [5..15]x[5..15] is ~0.25.
  EXPECT_EQ(1, harp_area_mask_intersects_area_with_fraction(
                   mask.get(), query_partial.get(), 0.2));
  EXPECT_EQ(0, harp_area_mask_intersects_area_with_fraction(
                   mask.get(), query_partial.get(), 0.5));
  EXPECT_EQ(0, harp_area_mask_intersects_area_with_fraction(
                   mask.get(), query_disjoint.get(), 0.0));
}

TEST(HarpAreaMaskTest,
     ReadValidCsvWithClosingVertexTrailingCommaAndManyVertices) {
  TempFile csv("area_mask_valid.csv");

  // Construct a valid convex polygon with > BLOCK_SIZE (16) vertices on a
  // small circle around (lat=10, lon=10) to exercise point_array growth in
  // parse_polygon.
  std::ostringstream many_vertices;
  constexpr int kNumVerts = BLOCK_SIZE + 4;
  for (int i = 0; i < kNumVerts; ++i) {
    const double angle = 2.0 * std::numbers::pi * i / kNumVerts;
    const double lat = 10.0 + 2.0 * std::sin(angle);
    const double lon = 10.0 + 2.0 * std::cos(angle);
    if (i > 0) {
      many_vertices << ", ";
    }
    many_vertices << lat << ", " << lon;
  }

  std::string content;
  content += "latitude, longitude\r\n";
  // Polygon with duplicate closing point, spaces around the lat/lon comma, and
  // trailing pair-separator comma + CRLF.
  content += "  0.0 , 0.0, 0.0 , 10.0, 10.0 , 10.0, 10.0 , 0.0, 0.0 , 0.0,\r\n";
  content += many_vertices.str() + "\n";
  ASSERT_TRUE(WriteFileText(csv.path(), content));

  harp_area_mask* raw_mask = nullptr;
  ASSERT_EQ(0, harp_area_mask_read(csv.c_str(), &raw_mask))
      << harp_errno_to_string(harp_errno);
  ScopedHarpAreaMask mask(raw_mask);
  ASSERT_EQ(2, mask->num_polygons);
  EXPECT_EQ(4, mask->polygon[0]->numberofpoints);
  EXPECT_EQ(kNumVerts, mask->polygon[1]->numberofpoints);

  const harp_spherical_point inside_first = PointDeg(5.0, 5.0);
  const harp_spherical_point inside_second = PointDeg(10.0, 10.0);
  EXPECT_EQ(1, harp_area_mask_covers_point(mask.get(), &inside_first));
  EXPECT_EQ(1, harp_area_mask_covers_point(mask.get(), &inside_second));

  // Empty 0-byte file yields an empty area mask.
  TempFile empty_csv("area_mask_empty.csv");
  ASSERT_TRUE(WriteFileText(empty_csv.path(), ""));
  harp_area_mask* raw_empty = nullptr;
  ASSERT_EQ(0, harp_area_mask_read(empty_csv.c_str(), &raw_empty));
  ScopedHarpAreaMask empty_mask(raw_empty);
  EXPECT_EQ(0, empty_mask->num_polygons);
}

TEST(HarpAreaMaskTest, ReadErrorsNullMissingAndInvalidSyntax) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_area_mask* mask = nullptr;

  // 1. NULL filename.
  EXPECT_EQ(-1, harp_area_mask_read(nullptr, &mask));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // 2. Non-existent file.
  TempFile missing("does_not_exist_area_mask.csv");
  EXPECT_EQ(-1, harp_area_mask_read(missing.c_str(), &mask));
  EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);

  // 3. Invalid latitude token and non-finite latitude.
  TempFile bad_lat("bad_lat.csv");
  ASSERT_TRUE(WriteFileText(bad_lat.path(), "header\nabc, 10.0\n"));
  EXPECT_EQ(-1, harp_area_mask_read(bad_lat.c_str(), &mask));
  EXPECT_EQ(HARP_ERROR_INVALID_FORMAT, harp_errno);

  ASSERT_TRUE(WriteFileText(bad_lat.path(), "header\nNaN, 10.0\n"));
  EXPECT_EQ(-1, harp_area_mask_read(bad_lat.path().c_str(), &mask));
  EXPECT_EQ(HARP_ERROR_INVALID_FORMAT, harp_errno);

  // 4. Missing comma / longitude after latitude.
  TempFile missing_lon("missing_lon.csv");
  ASSERT_TRUE(WriteFileText(missing_lon.path(), "header\n10.0 20.0\n"));
  EXPECT_EQ(-1, harp_area_mask_read(missing_lon.c_str(), &mask));
  EXPECT_EQ(HARP_ERROR_INVALID_FORMAT, harp_errno);
  EXPECT_THAT(harp_errno_to_string(HARP_ERROR_INVALID_FORMAT),
              HasSubstr("missing longitude"));

  // 5. Invalid longitude token and non-finite longitude.
  TempFile bad_lon("bad_lon.csv");
  ASSERT_TRUE(WriteFileText(bad_lon.path(), "header\n10.0, xyz\n"));
  EXPECT_EQ(-1, harp_area_mask_read(bad_lon.c_str(), &mask));
  EXPECT_EQ(HARP_ERROR_INVALID_FORMAT, harp_errno);

  ASSERT_TRUE(WriteFileText(bad_lon.path(), "header\n10.0, inf\n"));
  EXPECT_EQ(-1, harp_area_mask_read(bad_lon.c_str(), &mask));
  EXPECT_EQ(HARP_ERROR_INVALID_FORMAT, harp_errno);

  // 6. Invalid polygon geometry (self-intersecting bowtie).
  TempFile bad_poly("bad_poly.csv");
  ASSERT_TRUE(WriteFileText(
      bad_poly.path(), "header\n0.0, 0.0, 10.0, 10.0, 10.0, 0.0, 0.0, 10.0\n"));
  EXPECT_EQ(-1, harp_area_mask_read(bad_poly.c_str(), &mask));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // 7. Directory path opens with fopen("r") and triggers ferror() in fgets().
  TempDir dir("area_mask_dir");
  EXPECT_EQ(-1, harp_area_mask_read(dir.c_str(), &mask));
  EXPECT_EQ(HARP_ERROR_FILE_READ, harp_errno);
}

#if !defined(HARP_TEST_HAS_ASAN)
TEST(HarpAreaMaskTest, ReadErrorsEmptyLineAndOversizedLineInSubprocess) {
  // read_area_mask() in libharp/harp-area-mask.c returns -1 without calling
  // harp_area_mask_delete(area_mask) on empty lines and lines of length ==
  // HARP_CSV_LINE_LENGTH. Run in a child process that exits with 0 so
  // coverage is flushed while avoiding HeapLeakChecker false positives in the
  // parent; guarded under ASan where LSan also checks child processes.
  TempFile empty_line("empty_line_sub.csv");
  ASSERT_TRUE(WriteFileText(empty_line.path(), "header\n\n0,0,0,1,1,1\n"));
  TempFile long_line("long_line_sub.csv");
  const std::string oversized(HARP_CSV_LINE_LENGTH, ' ');
  ASSERT_TRUE(WriteFileText(long_line.path(), oversized + "\n"));

  EXPECT_EXIT(
      {
        harp_area_mask* mask = nullptr;
        if (harp_area_mask_read(empty_line.c_str(), &mask) != -1 ||
            harp_errno != HARP_ERROR_INVALID_ARGUMENT) {
          std::exit(1);
        }
        if (harp_area_mask_read(long_line.c_str(), &mask) != -1 ||
            harp_errno != HARP_ERROR_INVALID_ARGUMENT) {
          std::exit(2);
        }
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace stcorp_harp
