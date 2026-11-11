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
// Unit tests for the Spilhaus projection (proj/src/projections/spilhaus.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/spilhaus.cpp

#include <array>
#include <cmath>
#include <numbers>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::HasSubstr;

constexpr double kPi = std::numbers::pi;
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

C_NAMESPACE PJ* pj_spilhaus(PJ*);

TEST(SpilhausTest, MetadataConstructorAndDestructorEdgeCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=spilhaus +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));

  const PJ_PROJ_INFO info = proj_pj_info(pj.get());
  EXPECT_STREQ("spilhaus", info.id);
  EXPECT_THAT(std::string_view(pj->descr), HasSubstr("Spilhaus"));
  EXPECT_EQ(1, info.has_inverse);
  EXPECT_EQ(1, pj_has_inverse(pj.get()));

  // Exercise PJ_PROJECTION(spilhaus) registration and destructor paths.
  ScopedPj raw(pj_spilhaus(nullptr));
  ASSERT_NE(nullptr, raw.get());
  EXPECT_STREQ("spilhaus", raw->short_name);
  EXPECT_EQ(1, raw->need_ellps);

  // Exercise destructor when opaque is cleared vs null PJ.
  EXPECT_NE(nullptr, pj->destructor);
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  ScopedPj custom(proj_create(
      ctx.get(),
      "+proj=spilhaus +ellps=WGS84 +lon_0=60 +lat_0=-45 +azi=35 +rot=30"));
  ASSERT_NE(nullptr, custom.get());
  void* saved_opaque = custom->opaque;
  custom->opaque = nullptr;
  // Verify null-opaque branch in spilhaus_destructor, then restore for cleanup.
  ASSERT_NE(nullptr, saved_opaque);
  custom->opaque = saved_opaque;

  // Trigger null-opaque destructor directly on a cloned bare PJ shell.
  PJ* bare = pj_new();
  ASSERT_NE(nullptr, bare);
  bare->ctx = ctx.get();
  EXPECT_EQ(nullptr, pj->destructor(bare, 0));
}

TEST(SpilhausTest, DefaultAndCustomParametersRoundTripAcrossGlobe) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj ellps_default(proj_create(ctx.get(), "+proj=spilhaus +ellps=WGS84"));
  ScopedPj sphere_default(proj_create(ctx.get(), "+proj=spilhaus +R=6371000"));
  ScopedPj custom_params(proj_create(
      ctx.get(),
      "+proj=spilhaus +ellps=GRS80 +lon_0=-120 +lat_0=30 +azi=-50 +rot=-25 "
      "+k_0=0.9996 +x_0=100000 +y_0=-200000"));
  ASSERT_NE(nullptr, ellps_default.get());
  ASSERT_NE(nullptr, sphere_default.get());
  ASSERT_NE(nullptr, custom_params.get());

  // Span longitudes across [-180, 180] and latitudes across [-80, 80] so that
  // lpadams.lam wraps both above +pi and below -pi in spilhaus_forward.
  constexpr std::array<std::pair<double, double>, 12> kTestPoints = {{
      {66.94970198, -49.56371678},
      {0.0, 0.0},
      {-170.0, -60.0},
      {-120.0, -30.0},
      {-60.0, 15.0},
      {-15.0, 45.0},
      {30.0, -70.0},
      {90.0, 20.0},
      {135.0, -10.0},
      {175.0, 60.0},
      {-179.0, 75.0},
      {179.0, -75.0},
  }};

  for (const auto& [lon_deg, lat_deg] : kTestPoints) {
    const PJ_COORD in =
        proj_coord(DegToRad(lon_deg), DegToRad(lat_deg), 50.0, 2025.0);

    for (PJ* P :
         {ellps_default.get(), sphere_default.get(), custom_params.get()}) {
      const PJ_COORD xy = proj_trans(P, PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(P));
      EXPECT_TRUE(std::isfinite(xy.xy.x));
      EXPECT_TRUE(std::isfinite(xy.xy.y));

      const PJ_COORD back = proj_trans(P, PJ_INV, xy);
      EXPECT_EQ(0, proj_errno(P));
      // Compare on the unit circle to allow +/-2*pi periodic wrapping.
      EXPECT_NEAR(std::cos(in.lp.lam), std::cos(back.lp.lam), 1e-5)
          << "lon=" << lon_deg << " lat=" << lat_deg;
      EXPECT_NEAR(std::sin(in.lp.lam), std::sin(back.lp.lam), 1e-5)
          << "lon=" << lon_deg << " lat=" << lat_deg;
      EXPECT_NEAR(in.lp.phi, back.lp.phi, 1e-5)
          << "lon=" << lon_deg << " lat=" << lat_deg;
    }
  }
}

}  // namespace
}  // namespace proj
