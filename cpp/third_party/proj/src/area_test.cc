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
// Unit tests for PJ_AREA lifecycle, bounding-box/name setters, and integration
// with proj_create_crs_to_crs() (proj/src/area.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/area.cpp

#include <cmath>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

TEST(AreaTest, CreateInitializesDefaultFieldsAndDestroyHandlesNull) {
  // 1. Destroying a null PJ_AREA* is a safe no-op.
  proj_area_destroy(nullptr);

  // 2. Newly created PJ_AREA has an unset bounding box, zero bounds, and an
  // empty name.
  ScopedArea area(proj_area_create());
  ASSERT_NE(nullptr, area.get());
  EXPECT_FALSE(area->bbox_set);
  EXPECT_DOUBLE_EQ(0.0, area->west_lon_degree);
  EXPECT_DOUBLE_EQ(0.0, area->south_lat_degree);
  EXPECT_DOUBLE_EQ(0.0, area->east_lon_degree);
  EXPECT_DOUBLE_EQ(0.0, area->north_lat_degree);
  EXPECT_TRUE(area->name.empty());

  // 3. Direct create/destroy lifecycle is leak-free.
  PJ_AREA* raw = proj_area_create();
  ASSERT_NE(nullptr, raw);
  proj_area_destroy(raw);
}

TEST(AreaTest, SetBboxAndNameUpdateAndOverwriteAreaFields) {
  ScopedArea area(proj_area_create());
  ASSERT_NE(nullptr, area.get());

  struct BboxCase {
    double west;
    double south;
    double east;
    double north;
    const char* name;
  };
  constexpr BboxCase kCases[] = {
      {.west = 8.0,
       .south = 54.5,
       .east = 15.5,
       .north = 57.8,
       .name = "Denmark"},
      {.west = -180.0,
       .south = -90.0,
       .east = 180.0,
       .north = 90.0,
       .name = "World"},
      // Antimeridian-crossing bbox and long string exceeding SSO capacity.
      {.west = 170.0,
       .south = -48.0,
       .east = -175.0,
       .north = -34.0,
       .name =
           "South Pacific region crossing the 180-degree antimeridian line"},
      // Overwriting with an empty name clears any prior name cleanly.
      {.west = -10.0, .south = 35.0, .east = 5.0, .north = 44.0, .name = ""},
  };

  for (const auto& tc : kCases) {
    proj_area_set_bbox(area.get(), tc.west, tc.south, tc.east, tc.north);
    proj_area_set_name(area.get(), tc.name);

    EXPECT_TRUE(area->bbox_set);
    EXPECT_DOUBLE_EQ(tc.west, area->west_lon_degree);
    EXPECT_DOUBLE_EQ(tc.south, area->south_lat_degree);
    EXPECT_DOUBLE_EQ(tc.east, area->east_lon_degree);
    EXPECT_DOUBLE_EQ(tc.north, area->north_lat_degree);
    EXPECT_EQ(std::string(tc.name), area->name);
  }
}

TEST(AreaTest, ProjCreateCrsToCrsAcceptsUnsetAndConfiguredAreas) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedArea area(proj_area_create());
  ASSERT_NE(nullptr, area.get());

  // 1. Passing an area without a configured bbox succeeds cleanly.
  ScopedPj unset_op(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:3857", area.get()));
  ASSERT_NE(nullptr, unset_op.get());
  EXPECT_EQ(0, proj_errno(unset_op.get()));

  // 2. Configure bbox + name for Denmark and create WGS84 -> WGS84 UTM 32N.
  proj_area_set_bbox(area.get(), 8.0, 54.5, 15.5, 57.8);
  proj_area_set_name(area.get(), "Denmark");

  ScopedPj denmark_op(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:32632", area.get()));
  ASSERT_NE(nullptr, denmark_op.get());
  EXPECT_EQ(0, proj_errno(denmark_op.get()));

  // Round-trip Copenhagen in EPSG:4326 authority axis order (lat, lon deg).
  const PJ_COORD copenhagen_deg = proj_coord(55.6761, 12.5683, 0.0, 0.0);
  const PJ_COORD utm = proj_trans(denmark_op.get(), PJ_FWD, copenhagen_deg);
  EXPECT_EQ(0, proj_errno(denmark_op.get()));
  EXPECT_FALSE(std::isinf(utm.xy.x));

  const PJ_COORD roundtrip = proj_trans(denmark_op.get(), PJ_INV, utm);
  EXPECT_EQ(0, proj_errno(denmark_op.get()));
  EXPECT_NEAR(copenhagen_deg.xy.x, roundtrip.xy.x, 1e-9);
  EXPECT_NEAR(copenhagen_deg.xy.y, roundtrip.xy.y, 1e-9);
}

}  // namespace
}  // namespace proj
