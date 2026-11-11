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
// Unit tests for 2D/3D Euclidean and ellipsoidal/spherical geodesic distance
// and direct/inverse geodesic helpers (proj/src/dist.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/dist.cpp

#include <cmath>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(DistTest, NullGeodGuardReturnsErrorCoordsAndHugeVal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // A VerticalCRS without horizontal/geodetic datum leaves P->geod == nullptr.
  ScopedPj no_geod(proj_create(ctx.get(), "EPSG:3855"));
  ASSERT_NE(nullptr, no_geod.get());
  ASSERT_EQ(nullptr, no_geod->geod);

  const PJ_COORD a = proj_coord(0.0, 0.0, 0.0, 0.0);
  const PJ_COORD b = proj_coord(DegToRad(1.0), DegToRad(1.0), 0.0, 0.0);

  const PJ_COORD geod_err = proj_geod(no_geod.get(), a, b);
  EXPECT_EQ(HUGE_VAL, geod_err.v[0]);
  EXPECT_EQ(HUGE_VAL, geod_err.v[1]);
  EXPECT_EQ(HUGE_VAL, geod_err.v[2]);
  EXPECT_EQ(HUGE_VAL, geod_err.v[3]);

  const PJ_COORD direct_err = proj_geod_direct(no_geod.get(), a, 0.0, 1000.0);
  EXPECT_EQ(HUGE_VAL, direct_err.v[0]);
  EXPECT_EQ(HUGE_VAL, direct_err.v[1]);
  EXPECT_EQ(HUGE_VAL, direct_err.v[2]);
  EXPECT_EQ(HUGE_VAL, direct_err.v[3]);

  EXPECT_EQ(HUGE_VAL, proj_lp_dist(no_geod.get(), a, b));
  EXPECT_EQ(HUGE_VAL, proj_lpz_dist(no_geod.get(), a, b));
}

TEST(DistTest, GeodAndGeodDirectEllipsoidalAndSphericalRoundTrips) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj wgs84(proj_create(ctx.get(), "+proj=longlat +ellps=WGS84"));
  ASSERT_NE(nullptr, wgs84.get());

  // Note: proj_geod() leaves c.v[3] uninitialized; read only .geod.{s,a1,a2}
  // where s is in meters and a1/a2 are returned in degrees, whereas
  // proj_geod_direct() takes and returns angles in radians.

  // 1. Coincident points produce zero distance.
  const PJ_COORD pt_eq = proj_coord(DegToRad(10.0), DegToRad(45.0), 0.0, 0.0);
  const PJ_COORD g_zero = proj_geod(wgs84.get(), pt_eq, pt_eq);
  EXPECT_DOUBLE_EQ(0.0, g_zero.geod.s);

  // 2. Due-north meridian segment at 2 deg E from 49N to 50N.
  const PJ_COORD meridian_a =
      proj_coord(DegToRad(2.0), DegToRad(49.0), 0.0, 0.0);
  const PJ_COORD meridian_b =
      proj_coord(DegToRad(2.0), DegToRad(50.0), 0.0, 0.0);
  const PJ_COORD g_north = proj_geod(wgs84.get(), meridian_a, meridian_b);
  EXPECT_NEAR(111219.409, g_north.geod.s, 1e-3);
  EXPECT_NEAR(0.0, g_north.geod.a1, 1e-9);
  EXPECT_NEAR(0.0, g_north.geod.a2, 1e-9);

  // 3. Equator eastward segment (a1 == a2 == 90 deg) distinguishes degrees
  // output of proj_geod() from radians input/output of proj_geod_direct().
  const PJ_COORD eq_start = proj_coord(0.0, 0.0, 0.0, 0.0);
  const PJ_COORD eq_end = proj_coord(DegToRad(10.0), 0.0, 0.0, 0.0);
  const PJ_COORD g_east = proj_geod(wgs84.get(), eq_start, eq_end);
  EXPECT_NEAR(6378137.0 * DegToRad(10.0), g_east.geod.s, 1e-4);
  EXPECT_NEAR(90.0, g_east.geod.a1, 1e-9);
  EXPECT_NEAR(90.0, g_east.geod.a2, 1e-9);

  // 4. Oblique Paris -> New York segment and inverse/direct round-trip.
  const PJ_COORD paris =
      proj_coord(DegToRad(2.3522), DegToRad(48.8566), 0.0, 0.0);
  const PJ_COORD nyc =
      proj_coord(DegToRad(-74.0060), DegToRad(40.7128), 0.0, 0.0);
  const PJ_COORD g_paris_nyc = proj_geod(wgs84.get(), paris, nyc);
  EXPECT_NEAR(5852935.292, g_paris_nyc.geod.s, 1.0);
  EXPECT_NEAR(-68.173917, g_paris_nyc.geod.a1, 1e-3);
  EXPECT_NEAR(-126.275955, g_paris_nyc.geod.a2, 1e-3);

  const PJ_COORD direct_nyc = proj_geod_direct(
      wgs84.get(), paris, proj_torad(g_paris_nyc.geod.a1), g_paris_nyc.geod.s);
  EXPECT_NEAR(nyc.lpz.lam, direct_nyc.lpz.lam, 1e-11);
  EXPECT_NEAR(nyc.lpz.phi, direct_nyc.lpz.phi, 1e-11);
  EXPECT_NEAR(proj_torad(g_paris_nyc.geod.a2), direct_nyc.v[2], 1e-11);
  EXPECT_DOUBLE_EQ(0.0, direct_nyc.v[3]);

  // 5. Spherical antimeridian crossing and quarter-meridian arc.
  constexpr double kRadius = 6371000.0;
  ScopedPj sphere(proj_create(ctx.get(), "+proj=longlat +R=6371000"));
  ASSERT_NE(nullptr, sphere.get());
  const PJ_COORD near_dateline =
      proj_coord(DegToRad(179.0), DegToRad(0.0), 0.0, 0.0);
  const PJ_COORD crossed =
      proj_geod_direct(sphere.get(), near_dateline, kPi / 2.0, 300000.0);
  EXPECT_LT(crossed.lpz.lam, -kPi / 2.0);
  EXPECT_NEAR(0.0, crossed.lpz.phi, 1e-12);
  EXPECT_NEAR(kRadius * (kPi / 2.0),
              proj_lp_dist(sphere.get(), eq_start,
                           proj_coord(0.0, kPi / 2.0, 0.0, 0.0)),
              1e-6);
}

TEST(DistTest, LpDistAndLpzDistHandle3DOffsetsPolesAndHugeVal) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj wgs84(proj_create(ctx.get(), "+proj=longlat +ellps=WGS84"));
  ASSERT_NE(nullptr, wgs84.get());

  const PJ_COORD a = proj_coord(DegToRad(2.0), DegToRad(49.0), 100.0, 0.0);
  const PJ_COORD b_same_z =
      proj_coord(DegToRad(2.0), DegToRad(50.0), 100.0, 0.0);
  const PJ_COORD b_diff_z =
      proj_coord(DegToRad(2.0), DegToRad(50.0), 10100.0, 0.0);

  const double horiz = proj_lp_dist(wgs84.get(), a, b_same_z);
  EXPECT_NEAR(111219.409, horiz, 1e-3);
  EXPECT_DOUBLE_EQ(horiz, proj_lpz_dist(wgs84.get(), a, b_same_z));
  EXPECT_NEAR(std::hypot(horiz, 10000.0),
              proj_lpz_dist(wgs84.get(), a, b_diff_z), 1e-9);

  // Coincident horizontal coordinates with pure vertical offset.
  EXPECT_DOUBLE_EQ(123.45,
                   proj_lpz_dist(wgs84.get(), proj_coord(0.1, 0.2, 100.0, 0.0),
                                 proj_coord(0.1, 0.2, 223.45, 0.0)));

  // Pole-to-pole meridian distance (~20,003.931 km on WGS84).
  const double pole_to_pole =
      proj_lp_dist(wgs84.get(), proj_coord(0.0, -kPi / 2.0, 0.0, 0.0),
                   proj_coord(0.0, kPi / 2.0, 0.0, 0.0));
  EXPECT_NEAR(20003931.4586, pole_to_pole, 1e-3);

  // HUGE_VAL longitude in either endpoint short-circuits to HUGE_VAL.
  EXPECT_EQ(HUGE_VAL,
            proj_lpz_dist(wgs84.get(), proj_coord(HUGE_VAL, 0.0, 0.0, 0.0), a));
  EXPECT_EQ(HUGE_VAL,
            proj_lpz_dist(wgs84.get(), a, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0)));
}

TEST(DistTest, XyDistAndXyzDistComputeEuclideanDistances) {
  const PJ_COORD a = proj_coord(1.0, 2.0, 3.0, 100.0);
  const PJ_COORD b = proj_coord(4.0, 6.0, 15.0, 999.0);
  const PJ_COORD neg = proj_coord(-4.0, -10.0, 3.0, 0.0);

  EXPECT_DOUBLE_EQ(0.0, proj_xy_dist(a, a));
  EXPECT_DOUBLE_EQ(0.0, proj_xyz_dist(a, a));

  // 3-4-5 2D triangle and 3-4-12-13 3D Pythagorean quadruple; t is ignored.
  EXPECT_DOUBLE_EQ(5.0, proj_xy_dist(a, b));
  EXPECT_DOUBLE_EQ(13.0, proj_xyz_dist(a, b));

  // Negative deltas (-5, -12) -> 13 in XY, 0 in Z.
  EXPECT_DOUBLE_EQ(13.0, proj_xy_dist(a, neg));
  EXPECT_DOUBLE_EQ(13.0, proj_xyz_dist(a, neg));

  // Vertical-only 3D separation.
  EXPECT_DOUBLE_EQ(42.5, proj_xyz_dist(proj_coord(10.0, 20.0, -12.5, 0.0),
                                       proj_coord(10.0, 20.0, 30.0, 0.0)));
}

}  // namespace
}  // namespace proj
