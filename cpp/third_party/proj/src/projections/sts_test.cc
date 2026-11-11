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
// Unit tests for Sine-Tangent Series pseudocylindrical projections:
// Foucaut (fouc), Kavrayskiy V (kav5), Quartic Authalic (qua_aut), and
// McBryde-Thomas Flat-Polar Sine No. 1 (mbt_s) (proj/src/projections/sts.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/sts.cpp

#include <array>
#include <cmath>
#include <numbers>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

struct StsVariant {
  const char* proj_id;
  const char* expected_descr;
  double p;
  double q;
  bool tan_mode;
};

constexpr StsVariant kVariants[] = {
    {
        .proj_id = "fouc",
        .expected_descr = "Foucaut\n\tPCyl, Sph",
        .p = 2.0,
        .q = 2.0,
        .tan_mode = true,
    },
    {
        .proj_id = "kav5",
        .expected_descr = "Kavrayskiy V\n\tPCyl, Sph",
        .p = 1.50488,
        .q = 1.35439,
        .tan_mode = false,
    },
    {
        .proj_id = "qua_aut",
        .expected_descr = "Quartic Authalic\n\tPCyl, Sph",
        .p = 2.0,
        .q = 2.0,
        .tan_mode = false,
    },
    {
        .proj_id = "mbt_s",
        .expected_descr = "McBryde-Thomas Flat-Polar Sine (No. 1)\n\tPCyl, Sph",
        .p = 1.48875,
        .q = 1.36509,
        .tan_mode = false,
    },
};

TEST(StsTest, AllFourProjectionsInitializeAsSphericalWithInverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const auto& variant : kVariants) {
    const std::string def =
        std::string("+proj=") + variant.proj_id + " +ellps=WGS84";
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get()) << variant.proj_id;
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_EQ(1, pj_has_inverse(pj.get()));
    // setup() forces P->es = 0.0 (spherical).
    EXPECT_EQ(0.0, pj->es);

    const PJ_PROJ_INFO info = proj_pj_info(pj.get());
    EXPECT_STREQ(variant.proj_id, info.id);
    EXPECT_STREQ("PROJ-based coordinate operation", info.description);
    EXPECT_STREQ(variant.expected_descr, pj->descr);
    EXPECT_EQ(1, info.has_inverse);
  }
}

TEST(StsTest, ForwardMatchesClosedFormEquationsOnUnitSphere) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  const PJ_LP lp = {.lam = DegToRad(30.0), .phi = DegToRad(40.0)};

  for (const auto& variant : kVariants) {
    const std::string def = std::string("+proj=") + variant.proj_id + " +R=1";
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get()) << variant.proj_id;

    // 1. Origin maps to (0, 0).
    const PJ_XY origin = pj_fwd({.lam = 0.0, .phi = 0.0}, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(0.0, origin.x, 1e-15);
    EXPECT_NEAR(0.0, origin.y, 1e-15);

    // 2. Verify forward formula for both tan_mode (fouc) and sine mode
    // (kav5, qua_aut, mbt_s).
    const PJ_XY xy = pj_fwd(lp, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));

    const double c_x = variant.q / variant.p;
    const double c_y = variant.p;
    const double c_p = 1.0 / variant.q;
    const double scaled_phi = lp.phi * c_p;
    const double c = std::cos(scaled_phi);
    double expected_x = c_x * lp.lam * std::cos(lp.phi);
    double expected_y = c_y;
    if (variant.tan_mode) {
      expected_x *= c * c;
      expected_y *= std::tan(scaled_phi);
    } else {
      expected_x /= c;
      expected_y *= std::sin(scaled_phi);
    }
    EXPECT_NEAR(expected_x, xy.x, 1e-12) << variant.proj_id;
    EXPECT_NEAR(expected_y, xy.y, 1e-12) << variant.proj_id;
  }
}

TEST(StsTest, ForwardInverseRoundTripAcrossQuadrantsAndArrayTransform) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct SampleCoord {
    double lon_deg;
    double lat_deg;
  };
  constexpr SampleCoord kCoords[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 15.0, .lat_deg = 30.0},
      {.lon_deg = -75.0, .lat_deg = 55.0},
      {.lon_deg = 120.0, .lat_deg = -45.0},
      {.lon_deg = -165.0, .lat_deg = -70.0},
  };

  for (const auto& variant : kVariants) {
    const std::string def =
        std::string("+proj=") + variant.proj_id + " +R=6371000";
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get()) << variant.proj_id;

    for (const auto& sc : kCoords) {
      const double lam = DegToRad(sc.lon_deg);
      const double phi = DegToRad(sc.lat_deg);

      // 1. Forward projection.
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lam, phi, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_FALSE(std::isinf(fwd.xy.x));
      EXPECT_FALSE(std::isinf(fwd.xy.y));

      // 2. Inverse projection back to (lam, phi).
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(lam, inv.lp.lam, 1e-11) << variant.proj_id;
      EXPECT_NEAR(phi, inv.lp.phi, 1e-11) << variant.proj_id;
    }

    // 3. Batch transform via proj_trans_array and Distortion Factors.
    std::array<PJ_COORD, 2> batch = {
        proj_coord(DegToRad(10.0), DegToRad(20.0), 0.0, 0.0),
        proj_coord(DegToRad(-40.0), DegToRad(-35.0), 0.0, 0.0),
    };
    ASSERT_EQ(0,
              proj_trans_array(pj.get(), PJ_FWD, batch.size(), batch.data()));
    ASSERT_EQ(0,
              proj_trans_array(pj.get(), PJ_INV, batch.size(), batch.data()));
    EXPECT_NEAR(DegToRad(10.0), batch[0].lp.lam, 1e-11);
    EXPECT_NEAR(DegToRad(20.0), batch[0].lp.phi, 1e-11);

    const PJ_FACTORS factors = proj_factors(
        pj.get(), proj_coord(DegToRad(10.0), DegToRad(20.0), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_GT(factors.meridional_scale, 0.0);
    EXPECT_GT(factors.parallel_scale, 0.0);
  }
}

TEST(StsTest, SineModeInverseOutOfDomainNorthingSetsErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  // For qua_aut (tan_mode = 0, C_y = 2.0), |xy.y / C_y| > 1 triggers aasin()
  // domain failure.
  ScopedPj pj(proj_create(ctx.get(), "+proj=qua_aut +R=1"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LP inv = pj_inv({.x = 0.0, .y = 5.0}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_context_errno(ctx.get()));
  EXPECT_TRUE(std::isinf(inv.lam));
  EXPECT_TRUE(std::isinf(inv.phi));
  proj_errno_reset(pj.get());
}

}  // namespace
}  // namespace proj
