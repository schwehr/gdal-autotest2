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
// Unit tests for the Transverse Mercator Zoned Grid System projection
// (proj/src/projections/tmerczoned.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/tmerczoned.cpp

#include <array>
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

TEST(TmerczonedTest, EllipsoidalUtmStyleZonesRoundTripAndMatchUtmWithPrefix) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // IOGP Guidance Note 7-2 section 3.2.3.2: initial longitude -180 deg,
  // zone width 6 deg, scale factor 0.9996, false easting 500000 m.
  ScopedPj zoned(proj_create(
      ctx.get(),
      "+proj=tmerczoned +lon_0=-180 +width=6 +k_0=0.9996 +x_0=500000 "
      "+ellps=WGS84"));
  ASSERT_NE(nullptr, zoned.get());
  EXPECT_EQ(1, pj_has_inverse(zoned.get()));

  struct ZoneCase {
    double lon_deg;
    double lat_deg;
    int expected_zone;
  };
  constexpr ZoneCase kCases[] = {
      // Zone 1: [-180, -174), central meridian -177 deg (lp.lam >= 0 relative
      // to lon_0 = -180 after adjlon).
      {.lon_deg = -177.0, .lat_deg = 0.0, .expected_zone = 1},
      {.lon_deg = -175.5, .lat_deg = 45.0, .expected_zone = 1},
      // Exact zone boundary at -174 deg biases into Zone 2 via +eps.
      {.lon_deg = -174.0, .lat_deg = 20.0, .expected_zone = 2},
      // Zone 30: [-6, 0), central meridian -3 deg.
      {.lon_deg = -3.0, .lat_deg = 51.5, .expected_zone = 30},
      // Zone 31: [0, 6), central meridian +3 deg. Because lon - lon_0 = +183
      // deg wraps to -177 deg (< 0) in adjlon, this exercises the
      // `if (lp.lam < 0) lp.lam += 2 * M_PI;` branch in tmerczoned_forward.
      {.lon_deg = 3.0, .lat_deg = 0.0, .expected_zone = 31},
      {.lon_deg = 12.0, .lat_deg = 55.0, .expected_zone = 33},
      // Zone 60: [174, 180), central meridian +177 deg.
      {.lon_deg = 177.0, .lat_deg = -33.0, .expected_zone = 60},
  };

  for (const auto& tc : kCases) {
    const double lon_rad = DegToRad(tc.lon_deg);
    const double lat_rad = DegToRad(tc.lat_deg);

    // 1. Forward transform in tmerczoned.
    const PJ_COORD fwd =
        proj_trans(zoned.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(zoned.get()));

    // Verify the zone prefix encoded in millions of meters.
    const int computed_zone =
        static_cast<int>(std::floor(fwd.xy.x / 1'000'000.0));
    EXPECT_EQ(tc.expected_zone, computed_zone);

    // At the central meridian on the equator, easting is Z * 1e6 + 500000 and
    // northing is 0.
    if (tc.lat_deg == 0.0 &&
        std::abs(tc.lon_deg - (-180.0 + (tc.expected_zone - 0.5) * 6.0)) <
            1e-12) {
      EXPECT_NEAR(tc.expected_zone * 1'000'000.0 + 500'000.0, fwd.xy.x, 1e-6);
      EXPECT_NEAR(0.0, fwd.xy.y, 1e-9);
    }

    // 2. Inverse transform back to geodetic (lon, lat) in radians.
    const PJ_COORD inv = proj_trans(zoned.get(), PJ_INV, fwd);
    EXPECT_EQ(0, proj_errno(zoned.get()));
    EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-11);
    EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-11);
  }
}

TEST(TmerczonedTest, SphericalAndCustomZoneWidthRoundTripAndArrayTransform) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 3-degree Gauss-Kruger style zones starting at 0 deg on a sphere.
  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=tmerczoned +lon_0=0 +width=3 +k_0=1.0 +x_0=500000 +R=6371000"));
  ASSERT_NE(nullptr, pj.get());

  auto coords = std::to_array<PJ_COORD>({
      proj_coord(DegToRad(1.5), DegToRad(30.0), 100.0, 2026.0),
      proj_coord(DegToRad(4.5), DegToRad(-15.0), 250.0, 2026.5),
      proj_coord(DegToRad(-1.5), DegToRad(50.0), 0.0, 2027.0),
  });
  const auto orig = coords;

  // 1. Batch forward transform.
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  // Zone 1 central meridian 1.5 deg -> easting 1,500,000 m.
  EXPECT_NEAR(1'500'000.0, coords[0].xy.x, 1e-5);
  // Zone 2 central meridian 4.5 deg -> easting 2,500,000 m.
  EXPECT_NEAR(2'500'000.0, coords[1].xy.x, 1e-5);
  // -1.5 deg wraps to +358.5 deg (Zone 120, central meridian 358.5 deg).
  EXPECT_NEAR(120'500'000.0, coords[2].xy.x, 1e-4);

  // 2. Batch inverse transform.
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  for (size_t i = 0; i < coords.size(); ++i) {
    EXPECT_NEAR(orig[i].lpz.lam, coords[i].lpz.lam, 1e-11);
    EXPECT_NEAR(orig[i].lpz.phi, coords[i].lpz.phi, 1e-11);
    EXPECT_NEAR(orig[i].lpz.z, coords[i].lpz.z, 1e-9);
    EXPECT_DOUBLE_EQ(orig[i].xyzt.t, coords[i].xyzt.t);
  }
}

TEST(TmerczonedTest, FactorsAndDestructorNullGuards) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj pj(proj_create(
      ctx.get(),
      "+proj=tmerczoned +lon_0=-180 +width=6 +k_0=0.9996 +x_0=500000 "
      "+ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  // At the central meridian of Zone 31 (+3 deg), meridional and parallel scale
  // equal k_0 = 0.9996 and meridian convergence is 0 on the equator.
  const PJ_FACTORS factors =
      proj_factors(pj.get(), proj_coord(DegToRad(3.0), 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(0.9996, factors.meridional_scale, 1e-7);
  EXPECT_NEAR(0.9996, factors.parallel_scale, 1e-7);
  EXPECT_NEAR(0.0, factors.meridian_convergence, 1e-9);

  // Exercise tmerczoned_destructor defensive guards for nullptr and null
  // opaque.
  ASSERT_NE(nullptr, pj->destructor);
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  PJ* raw = pj_new();
  ASSERT_NE(nullptr, raw);
  raw->ctx = ctx.get();
  EXPECT_EQ(nullptr, raw->opaque);
  EXPECT_EQ(nullptr, pj->destructor(raw, 0));
}

}  // namespace
}  // namespace proj
