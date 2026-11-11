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
// Unit tests for the Hourglass equal-area projection
// (proj/src/projections/hourglass.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/projections/hourglass.cpp

#include <cmath>
#include <cstdlib>
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

// Mirror of anonymous `struct pj_hourglass_data` in hourglass.cpp.
// Upstream `PJ_PROJECTION(hourglass)` does not assign
// `P->destructor = pj_hourglass_destructor;` on the success path, so
// `pj_default_destructor` runs on `proj_destroy(P)` without freeing `Q->apa`
// when `P->e != 0`. Clearing `apa` before destruction keeps ASan/LSan clean
// both today and if upstream later sets `P->destructor`.
struct HourglassOpaqueLayout {
  int outinvalid;
  double n;
  double np1;
  double s;
  double sqrts;
  double L;
  double L_nm1_n;
  double L_1mn;
  double qp;
  double* apa;
};

void FreeHourglassApa(PJ* pj) noexcept {
  if (pj != nullptr && pj->opaque != nullptr) {
    auto* q = static_cast<HourglassOpaqueLayout*>(pj->opaque);
    std::free(q->apa);
    q->apa = nullptr;
  }
}

TEST(HourglassTest, InvalidRatioSOrExponentNRejectsCreation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture logs(ctx.get());

  // 1. s <= 0 is rejected and invokes pj_hourglass_destructor.
  for (const char* proj_str : {
           "+proj=hourglass +s=0",
           "+proj=hourglass +s=-1.5",
       }) {
    ScopedPj pj(proj_create(ctx.get(), proj_str));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
  }
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for ratio s"));

  // 2. n <= 0 is rejected and invokes pj_hourglass_destructor.
  logs.Clear();
  for (const char* proj_str : {
           "+proj=hourglass +n=0",
           "+proj=hourglass +n=-2.0",
       }) {
    ScopedPj pj(proj_create(ctx.get(), proj_str));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()));
  }
  EXPECT_TRUE(logs.ContainsMessage("Invalid value for n"));
}

TEST(HourglassTest, SphericalAndEllipsoidalForwardInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  for (const char* proj_str : {
           "+proj=hourglass +R=6371000",
           "+proj=hourglass +R=6371000 +s=1.5 +n=2.0",
           "+proj=hourglass +ellps=WGS84 +s=1.2 +n=1.5",
       }) {
    // Use pj_create_internal so PROJStringParser::createFromPROJString does not
    // create and destroy a temporary validation PJ* whose Q->apa would not be
    // freed by pj_default_destructor on the ellipsoidal path.
    ScopedPj pj(pj_create_internal(ctx.get(), proj_str));
    ASSERT_NE(nullptr, pj.get());
    absl::Cleanup free_apa = [&pj] { FreeHourglassApa(pj.get()); };
    EXPECT_EQ(1, pj_has_inverse(pj.get()));

    struct CoordCase {
      double lon_deg;
      double lat_deg;
    };
    constexpr CoordCase kCases[] = {
        {.lon_deg = 25.0, .lat_deg = 40.0},
        {.lon_deg = -60.0, .lat_deg = 20.0},
        {.lon_deg = 45.0, .lat_deg = -35.0},
        {.lon_deg = -110.0, .lat_deg = -65.0},
    };

    for (const auto& tc : kCases) {
      const PJ_COORD in =
          proj_coord(DegToRad(tc.lon_deg), DegToRad(tc.lat_deg), 0.0, 0.0);
      const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_FALSE(std::isinf(fwd.xy.x));
      EXPECT_FALSE(std::isinf(fwd.xy.y));

      const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
      EXPECT_EQ(0, proj_errno(pj.get()));
      EXPECT_NEAR(in.lp.lam, inv.lp.lam, 1e-9);
      EXPECT_NEAR(in.lp.phi, inv.lp.phi, 1e-9);
    }
  }
}

TEST(HourglassTest, InverseSingularityAtZeroAndDomainValidationBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor silence(ctx.get());

  ScopedPj pj(proj_create(ctx.get(), "+proj=hourglass +R=1"));
  ASSERT_NE(nullptr, pj.get());

  // 1. |xy.y| <= 1e-8 hits the equator singularity guard and returns (0, 0).
  const PJ_LP eq_singularity = pj_inv({.x = 0.5, .y = 0.0}, pj.get());
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_DOUBLE_EQ(0.0, eq_singularity.lam);
  EXPECT_DOUBLE_EQ(0.0, eq_singularity.phi);

  // 2. Large |xy.y| makes sinbeta > 1 and triggers
  // PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  proj_errno_reset(pj.get());
  (void)pj_inv({.x = 0.0, .y = 10.0}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));

  // 3. With default outinvalid=1, large |xy.x| yielding |lam| > pi + 1e-7
  // triggers PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN.
  proj_errno_reset(pj.get());
  (void)pj_inv({.x = 5.0, .y = 0.5}, pj.get());
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj.get()));

  // 4. With +outinvalid=0 and +over (so inv_finalize does not wrap lam with
  // adjlon), the same coordinate is accepted with |lam| > pi.
  ScopedPj pj_no_outinvalid(
      proj_create(ctx.get(), "+proj=hourglass +R=1 +outinvalid=0 +over"));
  ASSERT_NE(nullptr, pj_no_outinvalid.get());
  const PJ_LP wide_inv = pj_inv({.x = 5.0, .y = 0.5}, pj_no_outinvalid.get());
  EXPECT_EQ(0, proj_errno(pj_no_outinvalid.get()));
  EXPECT_GT(std::abs(wide_inv.lam), kPi);

  // 5. Verify proj_factors on Hourglass.
  const PJ_FACTORS factors = proj_factors(
      pj.get(), proj_coord(DegToRad(15.0), DegToRad(30.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(1.0, factors.areal_scale, 1e-5);
}

}  // namespace
}  // namespace proj
