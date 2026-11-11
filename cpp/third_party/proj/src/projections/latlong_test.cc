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
// Unit tests for geodetic latlong/longlat/latlon/lonlat pass-through operations
// across 2D, 3D, and 4D entry points (proj/src/projections/latlong.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/latlong.cpp

#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

struct AliasCase {
  const char* id;
  const char* expected_description;
};

constexpr AliasCase kAliases[] = {
    {.id = "latlong", .expected_description = "Lat/long (Geodetic alias)"},
    {.id = "longlat", .expected_description = "Lat/long (Geodetic alias)"},
    {.id = "latlon", .expected_description = "Lat/long (Geodetic alias)"},
    {.id = "lonlat", .expected_description = "Lat/long (Geodetic)"},
};

TEST(LatlongTest, AllFourAliasesConfigureSetupFieldsAndFunctionPointers) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const auto& alias : kAliases) {
    // Pass non-zero +x_0 and +y_0 to verify latlong_setup() resets x0=0, y0=0.
    const std::string def =
        std::string("+proj=") + alias.id + " +x_0=500000 +y_0=100000";
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get()) << alias.id;
    EXPECT_EQ(0, proj_errno(pj.get()));

    EXPECT_EQ(1, pj->is_latlong);
    EXPECT_EQ(0.0, pj->x0);
    EXPECT_EQ(0.0, pj->y0);
    EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj->left);
    EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj->right);
    EXPECT_NE(nullptr, pj->fwd);
    EXPECT_NE(nullptr, pj->inv);
    EXPECT_NE(nullptr, pj->fwd3d);
    EXPECT_NE(nullptr, pj->inv3d);
    EXPECT_NE(nullptr, pj->fwd4d);
    EXPECT_NE(nullptr, pj->inv4d);
    EXPECT_EQ(1, pj_has_inverse(pj.get()));
    EXPECT_EQ(1, proj_angular_input(pj.get(), PJ_FWD));
    EXPECT_EQ(1, proj_angular_output(pj.get(), PJ_FWD));

    const PJ_PROJ_INFO info = proj_pj_info(pj.get());
    EXPECT_STREQ(alias.id, info.id);
    EXPECT_NE(std::string_view(pj->descr).find(alias.expected_description),
              std::string_view::npos);
    EXPECT_EQ(1, info.has_inverse);
  }
}

TEST(LatlongTest, Direct2DForwardAndInversePreserveRadiansExactly) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const auto& alias : kAliases) {
    const std::string def = std::string("+proj=") + alias.id;
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get()) << alias.id;

    const PJ_LP lp_in = {.lam = DegToRad(-122.4194), .phi = DegToRad(37.7749)};

    // 1. Exercise latlong_forward via pj_fwd (and pj->fwd directly).
    const PJ_XY xy = pj_fwd(lp_in, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lp_in.lam, xy.x, 1e-15);
    EXPECT_NEAR(lp_in.phi, xy.y, 1e-15);

    const PJ_XY xy_direct = pj->fwd(lp_in, pj.get());
    EXPECT_NEAR(lp_in.lam, xy_direct.x, 1e-15);
    EXPECT_NEAR(lp_in.phi, xy_direct.y, 1e-15);

    // 2. Exercise latlong_inverse via pj_inv (and pj->inv directly).
    const PJ_LP lp_out = pj_inv(xy, pj.get());
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_NEAR(lp_in.lam, lp_out.lam, 1e-15);
    EXPECT_NEAR(lp_in.phi, lp_out.phi, 1e-15);

    const PJ_LP lp_direct = pj->inv(xy, pj.get());
    EXPECT_NEAR(lp_in.lam, lp_direct.lam, 1e-15);
    EXPECT_NEAR(lp_in.phi, lp_direct.phi, 1e-15);
  }
}

TEST(LatlongTest, Direct3DForwardAndInversePreserveLonLatAndHeight) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=latlong +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_LPZ lpz_in = {
      .lam = DegToRad(139.6917),
      .phi = DegToRad(35.6895),
      .z = 42.5,
  };

  // 1. Exercise latlong_forward_3d via pj_fwd3d and pj->fwd3d.
  const PJ_XYZ xyz = pj_fwd3d(lpz_in, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lpz_in.lam, xyz.x, 1e-15);
  EXPECT_NEAR(lpz_in.phi, xyz.y, 1e-15);
  EXPECT_NEAR(lpz_in.z, xyz.z, 1e-15);

  const PJ_XYZ xyz_direct = pj->fwd3d(lpz_in, pj.get());
  EXPECT_NEAR(lpz_in.lam, xyz_direct.x, 1e-15);
  EXPECT_NEAR(lpz_in.phi, xyz_direct.y, 1e-15);
  EXPECT_NEAR(lpz_in.z, xyz_direct.z, 1e-15);

  // 2. Exercise latlong_inverse_3d via pj_inv3d and pj->inv3d.
  const PJ_LPZ lpz_out = pj_inv3d(xyz, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(lpz_in.lam, lpz_out.lam, 1e-15);
  EXPECT_NEAR(lpz_in.phi, lpz_out.phi, 1e-15);
  EXPECT_NEAR(lpz_in.z, lpz_out.z, 1e-15);

  const PJ_LPZ lpz_direct = pj->inv3d(xyz, pj.get());
  EXPECT_NEAR(lpz_in.lam, lpz_direct.lam, 1e-15);
  EXPECT_NEAR(lpz_in.phi, lpz_direct.phi, 1e-15);
  EXPECT_NEAR(lpz_in.z, lpz_direct.z, 1e-15);
}

TEST(LatlongTest, FourDimensionalTransformAndBatchLeaveCoordinatesUnchanged) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=lonlat +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_COORD in =
      proj_coord(DegToRad(8.5417), DegToRad(47.3769), 408.0, 2026.5);

  // 1. proj_trans dispatches through latlong_forward_4d and latlong_inverse_4d.
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.lpzt.lam, fwd.lpzt.lam, 1e-15);
  EXPECT_NEAR(in.lpzt.phi, fwd.lpzt.phi, 1e-15);
  EXPECT_NEAR(in.lpzt.z, fwd.lpzt.z, 1e-15);
  EXPECT_NEAR(in.lpzt.t, fwd.lpzt.t, 1e-15);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.lpzt.lam, inv.lpzt.lam, 1e-15);
  EXPECT_NEAR(in.lpzt.phi, inv.lpzt.phi, 1e-15);
  EXPECT_NEAR(in.lpzt.z, inv.lpzt.z, 1e-15);
  EXPECT_NEAR(in.lpzt.t, inv.lpzt.t, 1e-15);

  // 2. Also invoke pj_fwd4d and pj_inv4d directly.
  PJ_COORD c4d = in;
  pj_fwd4d(c4d, pj.get());
  EXPECT_NEAR(in.lpzt.lam, c4d.lpzt.lam, 1e-15);
  pj_inv4d(c4d, pj.get());
  EXPECT_NEAR(in.lpzt.phi, c4d.lpzt.phi, 1e-15);

  // 3. Batch transform via proj_trans_array.
  std::array<PJ_COORD, 2> coords = {
      in,
      proj_coord(DegToRad(-74.006), DegToRad(40.7128), 10.0, 2025.0),
  };
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_FWD, coords.size(), coords.data()));
  ASSERT_EQ(0,
            proj_trans_array(pj.get(), PJ_INV, coords.size(), coords.data()));
  EXPECT_NEAR(DegToRad(-74.006), coords[1].lpzt.lam, 1e-15);
  EXPECT_NEAR(DegToRad(40.7128), coords[1].lpzt.phi, 1e-15);
}

}  // namespace
}  // namespace proj
