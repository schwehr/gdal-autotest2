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
// Unit tests for PJ_COORD constructors, I/O unit inspection, degree/radian
// conversions, and DMS string formatting/parsing (proj/src/coordinates.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/coordinates.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(CoordinatesTest, ProjCoordInitializesAllUnionComponents) {
  const PJ_COORD c = proj_coord(1.25, -2.5, 3.75, 2026.5);
  EXPECT_DOUBLE_EQ(1.25, c.v[0]);
  EXPECT_DOUBLE_EQ(-2.5, c.v[1]);
  EXPECT_DOUBLE_EQ(3.75, c.v[2]);
  EXPECT_DOUBLE_EQ(2026.5, c.v[3]);
  EXPECT_DOUBLE_EQ(1.25, c.xyzt.x);
  EXPECT_DOUBLE_EQ(-2.5, c.xyzt.y);
  EXPECT_DOUBLE_EQ(3.75, c.xyzt.z);
  EXPECT_DOUBLE_EQ(2026.5, c.xyzt.t);
  EXPECT_DOUBLE_EQ(1.25, c.lpz.lam);
  EXPECT_DOUBLE_EQ(-2.5, c.lpz.phi);
  EXPECT_DOUBLE_EQ(3.75, c.lpz.z);
}

TEST(CoordinatesTest, OppositeDirectionNegatesDirectionValues) {
  EXPECT_EQ(PJ_INV, pj_opposite_direction(PJ_FWD));
  EXPECT_EQ(PJ_FWD, pj_opposite_direction(PJ_INV));
  EXPECT_EQ(PJ_IDENT, pj_opposite_direction(PJ_IDENT));
}

TEST(CoordinatesTest, AngularAndDegreeInputOutputClassifyOperators) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Classic projection: RADIANS in, PROJECTED out (swapped when inverted).
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, merc.get());
  EXPECT_EQ(1, proj_angular_input(merc.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_input(merc.get(), PJ_INV));
  EXPECT_EQ(0, proj_angular_output(merc.get(), PJ_FWD));
  EXPECT_EQ(1, proj_angular_output(merc.get(), PJ_INV));
  EXPECT_EQ(0, proj_degree_input(merc.get(), PJ_FWD));
  EXPECT_EQ(0, proj_degree_input(merc.get(), PJ_INV));
  EXPECT_EQ(0, proj_degree_output(merc.get(), PJ_FWD));
  EXPECT_EQ(0, proj_degree_output(merc.get(), PJ_INV));

  merc->inverted = 1;
  EXPECT_EQ(0, proj_angular_input(merc.get(), PJ_FWD));
  EXPECT_EQ(1, proj_angular_input(merc.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(merc.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(merc.get(), PJ_INV));

  // 2. Unit conversion deg -> rad reports DEGREES on left and RADIANS on right.
  ScopedPj deg2rad(
      proj_create(ctx.get(), "+proj=unitconvert +xy_in=deg +xy_out=rad"));
  ASSERT_NE(nullptr, deg2rad.get());
  EXPECT_EQ(0, proj_angular_input(deg2rad.get(), PJ_FWD));
  EXPECT_EQ(1, proj_angular_input(deg2rad.get(), PJ_INV));
  EXPECT_EQ(1, proj_angular_output(deg2rad.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(deg2rad.get(), PJ_INV));
  EXPECT_EQ(1, proj_degree_input(deg2rad.get(), PJ_FWD));
  EXPECT_EQ(0, proj_degree_input(deg2rad.get(), PJ_INV));
  EXPECT_EQ(0, proj_degree_output(deg2rad.get(), PJ_FWD));
  EXPECT_EQ(1, proj_degree_output(deg2rad.get(), PJ_INV));

  // 3. No-op conversion returns 0 for both angular and degree queries.
  ScopedPj noop(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, noop.get());
  EXPECT_EQ(0, proj_angular_input(noop.get(), PJ_FWD));
  EXPECT_EQ(0, proj_angular_output(noop.get(), PJ_FWD));
  EXPECT_EQ(0, proj_degree_input(noop.get(), PJ_FWD));
  EXPECT_EQ(0, proj_degree_output(noop.get(), PJ_FWD));

  // 4. CRS-to-CRS pipeline EPSG:4326 -> EPSG:32632 takes degree input in FWD
  // and produces degree output in INV.
  ScopedPj epsg_to_utm(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:32632", nullptr));
  ASSERT_NE(nullptr, epsg_to_utm.get());
  EXPECT_EQ(1, proj_degree_input(epsg_to_utm.get(), PJ_FWD));
  EXPECT_EQ(0, proj_degree_input(epsg_to_utm.get(), PJ_INV));
  EXPECT_EQ(0, proj_degree_output(epsg_to_utm.get(), PJ_FWD));
  EXPECT_EQ(1, proj_degree_output(epsg_to_utm.get(), PJ_INV));
}

TEST(CoordinatesTest, ToradTodegRoundTripExactAngles) {
  constexpr double kAnglesDeg[] = {-360.0, -180.0, -90.0, -45.0, 0.0,
                                   45.0,   90.0,   180.0, 360.0};
  for (const double deg : kAnglesDeg) {
    const double rad = proj_torad(deg);
    EXPECT_DOUBLE_EQ(DegToRad(deg), rad);
    EXPECT_DOUBLE_EQ(deg, proj_todeg(rad));
  }
}

TEST(CoordinatesTest, DmstorAndRtodmsFormatAndParseDmsStrings) {
  // proj_dmstor uses pj_get_default_ctx() and set_rtodms sets static formatting
  // globals; restore both cleanly at scope exit.
  absl::Cleanup restore_globals = [] {
    proj_errno_reset(nullptr);
    set_rtodms(3, 0);
  };

  char* endptr = nullptr;
  EXPECT_NEAR(DegToRad(45.0 + 30.0 / 60.0 + 15.0 / 3600.0),
              proj_dmstor(" 45d30'15\"N rest", &endptr), 1e-14);
  ASSERT_NE(nullptr, endptr);
  EXPECT_STREQ(" rest", endptr);

  EXPECT_NEAR(-DegToRad(122.0 + 15.0 / 60.0 + 30.5 / 3600.0),
              proj_dmstor("122d15'30.5\"W", nullptr), 1e-14);
  EXPECT_NEAR(-DegToRad(12.5),
              proj_dmstor("-12\xc2\xb0"
                          "30'",
                          nullptr),
              1e-14);
  EXPECT_NEAR(1.2345, proj_dmstor("1.2345r", nullptr), 1e-14);

  // Invalid DMS unit ordering returns HUGE_VAL and sets error on default ctx.
  EXPECT_EQ(HUGE_VAL, proj_dmstor("10'5d", nullptr));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_context_errno(nullptr));
  proj_errno_reset(nullptr);

  char buf[64]{};
  // proj_rtodms is marked PROJ_DEPRECATED; test the wrapper directly.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  EXPECT_EQ(buf, proj_rtodms(buf, kPi / 4.0, 'N', 'S'));
  EXPECT_STREQ("45dN", buf);
  EXPECT_EQ(buf, proj_rtodms(buf, -kPi / 4.0, 'N', 'S'));
  EXPECT_STREQ("45dS", buf);
#pragma GCC diagnostic pop

  EXPECT_EQ(buf, proj_rtodms2(buf, sizeof(buf), -DegToRad(12.5), 0, 0));
  EXPECT_STREQ("-12d30'", buf);
  EXPECT_EQ(buf, proj_rtodms2(buf, sizeof(buf),
                              DegToRad(12.0 + 34.0 / 60.0 + 56.789 / 3600.0),
                              'E', 'W'));
  EXPECT_STREQ("12d34'56.789\"E", buf);

  // Truncation edge case when buffer length is 1 byte for negative value.
  char tiny[2] = {'x', 'y'};
  EXPECT_EQ(tiny, proj_rtodms2(tiny, 1, -0.5, 0, 0));
  EXPECT_STREQ("", tiny);
}

}  // namespace
}  // namespace proj
