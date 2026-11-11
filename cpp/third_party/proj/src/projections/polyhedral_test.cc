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
// Unit tests for the Snyder equal-area polyhedral projections: tsea
// (Tetrahedral), dsea (Dodecahedral), isea (Icosahedral), and ivea
// (Icosahedral Vertex) (proj/src/projections/polyhedral.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/polyhedral.cpp

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(PolyhedralTest, MetadataAndParameterValidationErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. All four projections expose inverse transforms and angular->linear I/O.
  struct MetaCase {
    const char* id;
    const char* proj_str;
  };
  constexpr MetaCase kMetaCases[] = {
      {.id = "tsea", .proj_str = "+proj=tsea +R=6371000"},
      {.id = "dsea", .proj_str = "+proj=dsea +R=6371000"},
      {.id = "isea", .proj_str = "+proj=isea +R=6371000"},
      {.id = "ivea", .proj_str = "+proj=ivea +R=6371000"},
  };
  for (const auto& mc : kMetaCases) {
    ScopedPj pj(proj_create(ctx.get(), mc.proj_str));
    ASSERT_NE(nullptr, pj.get()) << mc.id;
    const PJ_PROJ_INFO info = proj_pj_info(pj.get());
    EXPECT_STREQ(mc.id, info.id);
    EXPECT_EQ(1, info.has_inverse) << mc.id;
    EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD)) << mc.id;
    EXPECT_EQ(0, proj_angular_output(pj.get(), PJ_FWD)) << mc.id;
    EXPECT_EQ(nullptr, pj->destructor(nullptr, 0)) << mc.id;
  }

  // 2. Invalid +net / +orient parameters fail creation with ILLEGAL_ARG_VALUE.
  struct InvalidParamCase {
    const char* proj_str;
    const char* expected_log;
  };
  constexpr InvalidParamCase kInvalidCases[] = {
      {.proj_str = "+proj=tsea +net=invalid_net",
       .expected_log = "invalid +net"},
      {.proj_str = "+proj=dsea +net=invalid_net",
       .expected_log = "invalid +net"},
      {.proj_str = "+proj=isea +orient=invalid_orient",
       .expected_log = "invalid +orient"},
      {.proj_str = "+proj=ivea +orient=invalid_orient",
       .expected_log = "invalid +orient"},
      // NaN orient_lat causes lonlat_to_cart(lat_0, lon_0) to produce NaN so
      // find_triangle fails inside apply_polyhedral_params.
      {.proj_str = "+proj=tsea +R=1 +lat_0=0 +orient_lat=nan",
       .expected_log = "outside the polyhedron"},
  };
  for (const auto& tc : kInvalidCases) {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    ScopedPj bad(proj_create(ctx.get(), tc.proj_str));
    EXPECT_EQ(nullptr, bad.get()) << tc.proj_str;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << tc.proj_str;
    EXPECT_TRUE(capture.ContainsMessage(tc.expected_log)) << tc.proj_str;
    proj_context_errno_set(ctx.get(), 0);
  }
}

TEST(PolyhedralTest, SphereAndEllipsoidRoundTripsAcrossAllFamiliesAndNets) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr const char* kDefs[] = {
      "+proj=tsea +R=6371000",
      "+proj=tsea +ellps=WGS84",
      "+proj=tsea +net=tsea +R=6371000",
      "+proj=tsea +net=triangle +R=6371000",
      "+proj=tsea +dual +ellps=WGS84",
      "+proj=dsea +R=6371000",
      "+proj=dsea +ellps=WGS84",
      "+proj=dsea +net=dsea +R=6371000",
      "+proj=dsea +net=a5 +R=6371000",
      "+proj=dsea +net=crescent +R=6371000",
      "+proj=dsea +net=flower +R=6371000",
      "+proj=dsea +dual +R=6371000",
      "+proj=dsea +dual +ellps=WGS84",
      "+proj=isea +R=6371000",
      "+proj=isea +ellps=WGS84",
      "+proj=isea +orient=isea +R=6371000",
      "+proj=isea +orient=pole +R=6371000",
      "+proj=isea +dual +R=6371000",
      "+proj=isea +dual +ellps=WGS84",
      "+proj=ivea +R=6371000",
      "+proj=ivea +ellps=WGS84",
      "+proj=ivea +orient=isea +R=6371000",
      "+proj=ivea +orient=pole +ellps=WGS84",
  };

  struct GlobePoint {
    double lon_deg;
    double lat_deg;
  };
  constexpr GlobePoint kPoints[] = {
      {.lon_deg = 0.0, .lat_deg = 0.0},
      {.lon_deg = 25.0, .lat_deg = 45.0},
      {.lon_deg = -110.0, .lat_deg = 35.0},
      {.lon_deg = 140.0, .lat_deg = -30.0},
      {.lon_deg = -55.0, .lat_deg = -65.0},
      {.lon_deg = 0.0, .lat_deg = 90.0},
      {.lon_deg = 0.0, .lat_deg = -90.0},
  };

  for (const char* def : kDefs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;

    for (const auto& pt : kPoints) {
      const double lon_rad = DegToRad(pt.lon_deg);
      const double lat_rad = DegToRad(pt.lat_deg);

      // 1. Forward: (lon, lat) -> planar (x, y) in meters.
      const PJ_COORD fwd =
          proj_trans(pj.get(), PJ_FWD, proj_coord(lon_rad, lat_rad, 0.0, 0.0));
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_FALSE(std::isinf(fwd.xy.x)) << def;
      EXPECT_FALSE(std::isinf(fwd.xy.y)) << def;

      // 2. Inverse: planar (x, y) -> (lon, lat) in radians.
      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get())) << def;
      EXPECT_NEAR(lat_rad, inv.lp.phi, 1e-7) << def << " lat=" << pt.lat_deg;
      if (std::abs(pt.lat_deg) < 89.9) {
        EXPECT_NEAR(lon_rad, inv.lp.lam, 1e-7)
            << def << " lon=" << pt.lon_deg << " lat=" << pt.lat_deg;
      }
    }
  }
}

TEST(PolyhedralTest, IveaMatchesDseaDualAndCustomOrientationAnchors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. +proj=ivea is defined as +proj=dsea +dual (icosahedron dual).
  ScopedPj ivea_sph(proj_create(ctx.get(), "+proj=ivea +R=6371000"));
  ScopedPj dsea_dual_sph(proj_create(ctx.get(), "+proj=dsea +dual +R=6371000"));
  ScopedPj ivea_ell(proj_create(ctx.get(), "+proj=ivea +ellps=WGS84"));
  ScopedPj dsea_dual_ell(
      proj_create(ctx.get(), "+proj=dsea +dual +ellps=WGS84"));
  ASSERT_NE(nullptr, ivea_sph.get());
  ASSERT_NE(nullptr, dsea_dual_sph.get());
  ASSERT_NE(nullptr, ivea_ell.get());
  ASSERT_NE(nullptr, dsea_dual_ell.get());

  const PJ_COORD sample = proj_coord(DegToRad(-73.0), DegToRad(41.0), 0.0, 0.0);
  const PJ_COORD fwd_ivea_sph = proj_trans(ivea_sph.get(), PJ_FWD, sample);
  const PJ_COORD fwd_ddual_sph =
      proj_trans(dsea_dual_sph.get(), PJ_FWD, sample);
  EXPECT_EQ(0, proj_errno(ivea_sph.get()));
  EXPECT_EQ(0, proj_errno(dsea_dual_sph.get()));
  EXPECT_NEAR(fwd_ddual_sph.xy.x, fwd_ivea_sph.xy.x, 1e-9);
  EXPECT_NEAR(fwd_ddual_sph.xy.y, fwd_ivea_sph.xy.y, 1e-9);

  const PJ_COORD fwd_ivea_ell = proj_trans(ivea_ell.get(), PJ_FWD, sample);
  const PJ_COORD fwd_ddual_ell =
      proj_trans(dsea_dual_ell.get(), PJ_FWD, sample);
  EXPECT_EQ(0, proj_errno(ivea_ell.get()));
  EXPECT_EQ(0, proj_errno(dsea_dual_ell.get()));
  EXPECT_NEAR(fwd_ddual_ell.xy.x, fwd_ivea_ell.xy.x, 1e-9);
  EXPECT_NEAR(fwd_ddual_ell.xy.y, fwd_ivea_ell.xy.y, 1e-9);

  // 2. Explicit +orient_lat/+orient_lon/+azi/+lat_0/+lon_0 places the anchor
  // point at (0, 0) on both sphere and ellipsoid.
  ScopedPj anchored_ell(proj_create(
      ctx.get(),
      "+proj=isea +ellps=WGS84 +orient_lat=50 +orient_lon=10 +azi=15 "
      "+lat_0=30 +lon_0=-20"));
  ASSERT_NE(nullptr, anchored_ell.get());
  const PJ_COORD anchor_fwd =
      proj_trans(anchored_ell.get(), PJ_FWD,
                 proj_coord(DegToRad(-20.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(anchored_ell.get()));
  EXPECT_NEAR(0.0, anchor_fwd.xy.x, 1e-7);
  EXPECT_NEAR(0.0, anchor_fwd.xy.y, 1e-7);
}

TEST(PolyhedralTest, DomainErrorsOnOutOfNetInverseAndNanForward) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Out-of-net inverse coordinates trigger barycentric rejection and return
  // HUGE_VAL + PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  constexpr const char* kUnitProjs[] = {
      "+proj=tsea +R=1",
      "+proj=dsea +R=1",
      "+proj=isea +R=1",
      "+proj=ivea +R=1",
  };
  for (const char* def : kUnitProjs) {
    ScopedPj pj(proj_create(ctx.get(), def));
    ASSERT_NE(nullptr, pj.get()) << def;

    const PJ_COORD inv_out =
        proj_trans(pj.get(), PJ_INV, proj_coord(10.0, 10.0, 0.0, 0.0));
    EXPECT_TRUE(std::isinf(inv_out.lp.lam)) << def;
    EXPECT_TRUE(std::isinf(inv_out.lp.phi)) << def;
    EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
              proj_errno(pj.get()))
        << def;
    proj_errno_reset(pj.get());

    // 2. NaN input passed to pj_fwd triggers find_triangle(Q, v) < 0 in
    // polyhedral_fwd(), returning HUGE_VAL + OUTSIDE_PROJECTION_DOMAIN.
    const double kNan = std::numeric_limits<double>::quiet_NaN();
    const PJ_COORD fwd_nan =
        proj_trans(pj.get(), PJ_FWD, proj_coord(kNan, 0.0, 0.0, 0.0));
    EXPECT_TRUE(std::isnan(fwd_nan.xy.x) || std::isinf(fwd_nan.xy.x)) << def;
    EXPECT_TRUE(std::isnan(fwd_nan.xy.y) || std::isinf(fwd_nan.xy.y)) << def;
    proj_errno_reset(pj.get());
  }
}

TEST(PolyhedralTest, EqualAreaFactorsLinearUnitsBatchAndSpecialValues) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. proj_factors at interior points of face triangles confirms equal-area
  // mapping (areal_scale == 1.0) on both sphere and WGS84 ellipsoid.
  struct FactorCase {
    const char* proj_str;
    double lon_deg;
    double lat_deg;
  };
  constexpr FactorCase kFactorCases[] = {
      {.proj_str = "+proj=tsea +R=6371000", .lon_deg = 0.0, .lat_deg = 20.0},
      {.proj_str = "+proj=tsea +ellps=WGS84", .lon_deg = 0.0, .lat_deg = 20.0},
      {.proj_str = "+proj=dsea +R=6371000", .lon_deg = 0.0, .lat_deg = 80.0},
      {.proj_str = "+proj=isea +orient=pole +R=6371000",
       .lon_deg = 10.0,
       .lat_deg = 5.0},
      {.proj_str = "+proj=ivea +orient=pole +ellps=WGS84",
       .lon_deg = 10.0,
       .lat_deg = 5.0},
  };
  for (const auto& fc : kFactorCases) {
    ScopedPj pj(proj_create(ctx.get(), fc.proj_str));
    ASSERT_NE(nullptr, pj.get()) << fc.proj_str;
    const PJ_FACTORS fac = proj_factors(
        pj.get(),
        proj_coord(DegToRad(fc.lon_deg), DegToRad(fc.lat_deg), 0.0, 0.0));
    EXPECT_EQ(0, proj_errno(pj.get())) << fc.proj_str;
    EXPECT_GT(fac.meridional_scale, 0.0) << fc.proj_str;
    EXPECT_GT(fac.parallel_scale, 0.0) << fc.proj_str;
    EXPECT_NEAR(1.0, fac.areal_scale, 1e-4) << fc.proj_str;
  }

  // 2. False easting/northing, +units=km (scaling x, y, and z from meters to
  // kilometers via fr_meter / vfr_meter), and 4D (z, t) round-trip.
  ScopedPj isea_km(proj_create(
      ctx.get(), "+proj=isea +ellps=WGS84 +x_0=100000 +y_0=200000 +units=km"));
  ASSERT_NE(nullptr, isea_km.get());
  const PJ_COORD pt_4d =
      proj_coord(DegToRad(15.0), DegToRad(35.0), 456.25, 2026.5);
  const PJ_COORD fwd_km = proj_trans(isea_km.get(), PJ_FWD, pt_4d);
  EXPECT_EQ(0, proj_errno(isea_km.get()));
  EXPECT_NEAR(0.45625, fwd_km.xyzt.z, 1e-12);
  EXPECT_NEAR(2026.5, fwd_km.xyzt.t, 1e-12);
  const PJ_COORD inv_km = proj_trans(isea_km.get(), PJ_INV, fwd_km);
  EXPECT_EQ(0, proj_errno(isea_km.get()));
  EXPECT_NEAR(pt_4d.lpzt.lam, inv_km.lpzt.lam, 1e-10);
  EXPECT_NEAR(pt_4d.lpzt.phi, inv_km.lpzt.phi, 1e-10);
  EXPECT_NEAR(456.25, inv_km.lpzt.z, 1e-12);
  EXPECT_NEAR(2026.5, inv_km.lpzt.t, 1e-12);

  // 3. Batch transforms via proj_trans_array (0 == success) and
  // proj_trans_generic.
  ScopedPj dsea(proj_create(ctx.get(), "+proj=dsea +ellps=WGS84"));
  ASSERT_NE(nullptr, dsea.get());
  std::array<PJ_COORD, 2> batch = {
      proj_coord(DegToRad(-70.0), DegToRad(40.0), 0.0, 0.0),
      proj_coord(DegToRad(120.0), DegToRad(-25.0), 0.0, 0.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(dsea.get(), PJ_FWD, batch.size(), batch.data()));
  ASSERT_EQ(0,
            proj_trans_array(dsea.get(), PJ_INV, batch.size(), batch.data()));
  EXPECT_NEAR(DegToRad(-70.0), batch[0].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(40.0), batch[0].lp.phi, 1e-10);
  EXPECT_NEAR(DegToRad(120.0), batch[1].lp.lam, 1e-10);
  EXPECT_NEAR(DegToRad(-25.0), batch[1].lp.phi, 1e-10);

  std::array<double, 2> gx = {DegToRad(-10.0), DegToRad(80.0)};
  std::array<double, 2> gy = {DegToRad(50.0), DegToRad(-40.0)};
  EXPECT_EQ(2u,
            proj_trans_generic(dsea.get(), PJ_FWD, gx.data(), sizeof(double),
                               gx.size(), gy.data(), sizeof(double), gy.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(dsea.get()));
  EXPECT_EQ(2u,
            proj_trans_generic(dsea.get(), PJ_INV, gx.data(), sizeof(double),
                               gx.size(), gy.data(), sizeof(double), gy.size(),
                               nullptr, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(0, proj_errno(dsea.get()));
  EXPECT_NEAR(DegToRad(-10.0), gx[0], 1e-10);
  EXPECT_NEAR(DegToRad(50.0), gy[0], 1e-10);
  EXPECT_NEAR(DegToRad(80.0), gx[1], 1e-10);
  EXPECT_NEAR(DegToRad(-40.0), gy[1], 1e-10);

  // 4. HUGE_VAL forward input returns HUGE_VAL cleanly.
  ScopedLogSuppressor suppress_logs(ctx.get());
  const PJ_COORD inf_res =
      proj_trans(dsea.get(), PJ_FWD, proj_coord(HUGE_VAL, 0.0, 0.0, 0.0));
  EXPECT_TRUE(std::isinf(inf_res.xy.x));
  proj_errno_reset(dsea.get());
}

// TODO(schwehr): Unreachable lines in proj/src/projections/polyhedral.cpp
// without fault injection or upstream code changes:
// - OOM null checks on calloc/pj_authalic_lat_compute_coeffs in
//   polyhedral_setup(), PJ_PROJECTION(tsea), dodecahedron_family_setup(), and
//   icosahedron_family_setup() (lines 100-101, 121-122, 157-158, 198-199).
// - polyhedral_destructor() P->opaque == nullptr fallback (lines 89-90).

}  // namespace
}  // namespace proj
