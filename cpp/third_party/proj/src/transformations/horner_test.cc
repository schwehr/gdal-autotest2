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
// Unit tests for 2D real and complex Horner polynomial evaluation
// (proj/src/transformations/horner.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/horner.cpp

#include <cmath>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

TEST(HornerTest, RealPolynomialDegree2ExplicitInverseForwardAndReverse) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // For degree 2 (6 coefficients per polynomial):
  // E = u0 + u1*e + u2*e^2 + n*(u3 + u4*e + u5*n)
  // N = v0 + v1*n + v2*n^2 + e*(v3 + v4*n + v5*e)
  // where e = u - fwd_origin.u, n = v - fwd_origin.v.
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=horner +deg=2 "
                          "+fwd_origin=10,20 "
                          "+fwd_u=100,2,0.01,0.5,0.02,0.03 "
                          "+fwd_v=200,3,0.04,-0.25,0.05,0.06 "
                          "+inv_origin=100,200 "
                          "+inv_u=10,0.5,0.001,-0.1,0.002,0.003 "
                          "+inv_v=20,0.25,0.004,0.2,0.005,0.006 "
                          "+range=1000"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Forward evaluation at (u=12, v=23) -> e = 2, n = 3.
  const PJ_COORD in = proj_coord(12.0, 23.0, 5.0, 2025.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  const double e = 2.0;
  const double n = 3.0;
  const double expected_e =
      100.0 + 2.0 * e + 0.01 * e * e + n * (0.5 + 0.02 * e + 0.03 * n);
  const double expected_n =
      200.0 + 3.0 * n + 0.04 * n * n + e * (-0.25 + 0.05 * n + 0.06 * e);
  EXPECT_NEAR(expected_e, fwd.uv.u, 1e-12);
  EXPECT_NEAR(expected_n, fwd.uv.v, 1e-12);

  // 2. Explicit inverse evaluation at (u=104, v=202) -> e_inv = 4, n_inv = 2.
  const PJ_COORD inv_in = proj_coord(104.0, 202.0, 0.0, 0.0);
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, inv_in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  const double ei = 4.0;
  const double ni = 2.0;
  const double expected_inv_u =
      10.0 + 0.5 * ei + 0.001 * ei * ei + ni * (-0.1 + 0.002 * ei + 0.003 * ni);
  const double expected_inv_v =
      20.0 + 0.25 * ni + 0.004 * ni * ni + ei * (0.2 + 0.005 * ni + 0.006 * ei);
  EXPECT_NEAR(expected_inv_u, inv.uv.u, 1e-12);
  EXPECT_NEAR(expected_inv_v, inv.uv.v, 1e-12);
}

TEST(HornerTest, RealPolynomialIterativeInverseRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Omit +inv_u, +inv_v, and +inv_origin so Q->has_inv == false and
  // horner_iterative_inverse_4d is used for PJ_INV.
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=horner +deg=2 "
                          "+fwd_origin=100,200 "
                          "+fwd_u=500,1.0001,1e-7,1e-4,1e-8,-1e-7 "
                          "+fwd_v=1000,0.9999,-1e-7,-1e-4,1e-8,1e-7 "
                          "+inv_tolerance=1e-10"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Forward transform from (125, 180).
  const PJ_COORD in = proj_coord(125.0, 180.0, 0.0, 0.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isinf(fwd.uv.u));

  // 2. Iterative inverse recovers original coordinates within inv_tolerance.
  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.uv.u, inv.uv.u, 1e-9);
  EXPECT_NEAR(in.uv.v, inv.uv.v, 1e-9);
}

TEST(HornerTest, ComplexPolynomialExplicitInverseAndAxisNegation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // For complex polynomial of degree 2 (6 coefficients c0..c5):
  // Let Z = n + i*e (after subtracting origin and applying uneg/vneg).
  // W = N + i*E = (c0 + i*c1) + (c2 + i*c3)*Z + (c4 + i*c5)*Z^2.
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=horner +deg=2 +uneg +vneg "
                          "+fwd_origin=10,20 "
                          "+fwd_c=100,200,1.5,0.5,0.01,-0.02 "
                          "+inv_origin=100,200 "
                          "+inv_c=20,10,0.8,-0.2,0.005,0.001 "
                          "+range=500"));
  ASSERT_NE(nullptr, pj.get());

  // 1. Forward at (u=12, v=23): with +uneg +vneg, e = -(12-10) = -2,
  // n = -(23-20) = -3.
  // Z = -3 - 2i, Z^2 = (9 - 4) + i*(12) = 5 + 12i.
  // (c2 + i*c3)*Z = (1.5 + 0.5i)*(-3 - 2i) = (-4.5 + 1.0) + i*(-3.0 - 1.5)
  //               = -3.5 - 4.5i.
  // (c4 + i*c5)*Z^2 = (0.01 - 0.02i)*(5 + 12i) = (0.05 + 0.24) + i*(0.12 -
  // 0.10)
  //                 = 0.29 + 0.02i.
  // N = 100 - 3.5 + 0.29 = 96.79, E = 200 - 4.5 + 0.02 = 195.52.
  const PJ_COORD fwd =
      proj_trans(pj.get(), PJ_FWD, proj_coord(12.0, 23.0, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(195.52, fwd.uv.u, 1e-12);
  EXPECT_NEAR(96.79, fwd.uv.v, 1e-12);

  // 2. Explicit inverse at (u=100, v=200): e = 0, n = 0 -> E = c1 = 10, N = c0
  // = 20.
  const PJ_COORD inv =
      proj_trans(pj.get(), PJ_INV, proj_coord(100.0, 200.0, 0, 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(10.0, inv.uv.u, 1e-12);
  EXPECT_NEAR(20.0, inv.uv.v, 1e-12);
}

TEST(HornerTest, ComplexPolynomialIterativeInverseWithUnegAndVnegRoundTrip) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // Complex polynomial of degree 2 without inv_c or inv_origin, exercising
  // complex_horner_iterative_inverse_4d and uneg/vneg sign restoration.
  ScopedPj pj(proj_create(ctx.get(),
                          "+proj=horner +deg=2 +uneg +vneg "
                          "+fwd_origin=50,60 "
                          "+fwd_c=1000,2000,1.0,0.001,1e-7,-1e-7 "
                          "+inv_tolerance=1e-10"));
  ASSERT_NE(nullptr, pj.get());

  const PJ_COORD in = proj_coord(70.0, 45.0, 0.0, 0.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_FALSE(std::isinf(fwd.uv.u));

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.uv.u, inv.uv.u, 1e-9);
  EXPECT_NEAR(in.uv.v, inv.uv.v, 1e-9);
}

TEST(HornerTest, OutOfRangeCoordinatesFailInRealAndComplexModes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Real polynomial with explicit inverse and +range=100.
  ScopedPj pj_real_exp(proj_create(
      ctx.get(),
      "+proj=horner +deg=1 +fwd_origin=0,0 +fwd_u=0,1,0 +fwd_v=0,1,0 "
      "+inv_origin=0,0 +inv_u=0,1,0 +inv_v=0,1,0 +range=100"));
  ASSERT_NE(nullptr, pj_real_exp.get());

  // |e| > 100 in forward:
  const PJ_COORD bad_e =
      proj_trans(pj_real_exp.get(), PJ_FWD, proj_coord(101.0, 0.0, 0, 0));
  EXPECT_EQ(HUGE_VAL, bad_e.uv.u);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_real_exp.get()));
  proj_errno_reset(pj_real_exp.get());

  // |n| > 100 in explicit inverse:
  const PJ_COORD bad_n =
      proj_trans(pj_real_exp.get(), PJ_INV, proj_coord(0.0, -101.0, 0, 0));
  EXPECT_EQ(HUGE_VAL, bad_n.uv.u);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_real_exp.get()));

  // 2. Real polynomial with iterative inverse and default range (500000).
  ScopedPj pj_real_iter(proj_create(
      ctx.get(),
      "+proj=horner +deg=1 +fwd_origin=0,0 +fwd_u=0,1,0 +fwd_v=0,1,0"));
  ASSERT_NE(nullptr, pj_real_iter.get());
  const PJ_COORD bad_iter =
      proj_trans(pj_real_iter.get(), PJ_INV, proj_coord(500001.0, 0.0, 0, 0));
  EXPECT_EQ(HUGE_VAL, bad_iter.uv.u);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_real_iter.get()));

  // 3. Complex polynomial with explicit and iterative inverse and +range=50.
  ScopedPj pj_cplx_exp(
      proj_create(ctx.get(),
                  "+proj=horner +deg=1 +fwd_origin=0,0 +fwd_c=0,0,1,0 "
                  "+inv_origin=0,0 +inv_c=0,0,1,0 +range=50"));
  ASSERT_NE(nullptr, pj_cplx_exp.get());
  const PJ_COORD bad_cplx_fwd =
      proj_trans(pj_cplx_exp.get(), PJ_FWD, proj_coord(0.0, 51.0, 0, 0));
  EXPECT_EQ(HUGE_VAL, bad_cplx_fwd.uv.u);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_cplx_exp.get()));

  ScopedPj pj_cplx_iter(proj_create(
      ctx.get(),
      "+proj=horner +deg=1 +fwd_origin=0,0 +fwd_c=0,0,1,0 +range=50"));
  ASSERT_NE(nullptr, pj_cplx_iter.get());
  const PJ_COORD bad_cplx_iter =
      proj_trans(pj_cplx_iter.get(), PJ_INV, proj_coord(51.0, 0.0, 0, 0));
  EXPECT_EQ(HUGE_VAL, bad_cplx_iter.uv.u);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
            proj_errno(pj_cplx_iter.get()));
}

TEST(HornerTest, IterativeInverseNonConvergenceSetsCoordTransfmError) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Degenerate real polynomial with zero linear terms (Ma=Md=Mb=Mc=0) fails
  // to converge after 32 iterations in real_iterative_inverse_impl.
  ScopedPj pj_real_degen(proj_create(
      ctx.get(),
      "+proj=horner +deg=1 +fwd_origin=0,0 +fwd_u=0,0,0 +fwd_v=0,0,0"));
  ASSERT_NE(nullptr, pj_real_degen.get());
  const PJ_COORD res_real =
      proj_trans(pj_real_degen.get(), PJ_INV, proj_coord(10.0, 20.0, 0, 0));
  EXPECT_EQ(HUGE_VAL, res_real.uv.u);
  EXPECT_EQ(HUGE_VAL, res_real.uv.v);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM, proj_errno(pj_real_degen.get()));

  // 2. Degenerate complex polynomial with zero linear terms (det = 0) fails to
  // converge after 32 iterations in complex_iterative_inverse_impl.
  ScopedPj pj_cplx_degen(proj_create(
      ctx.get(), "+proj=horner +deg=1 +fwd_origin=0,0 +fwd_c=0,0,0,0"));
  ASSERT_NE(nullptr, pj_cplx_degen.get());
  const PJ_COORD res_cplx =
      proj_trans(pj_cplx_degen.get(), PJ_INV, proj_coord(10.0, 20.0, 0, 0));
  EXPECT_EQ(HUGE_VAL, res_cplx.uv.u);
  EXPECT_EQ(HUGE_VAL, res_cplx.uv.v);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM, proj_errno(pj_cplx_degen.get()));
}

TEST(HornerTest, ParameterValidationAndMalformedCoefficientErrors) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());

  struct ErrorCase {
    const char* definition;
    int expected_err;
  };
  constexpr ErrorCase kCases[] = {
      // Missing +deg=...
      {.definition = "+proj=horner +fwd_u=0,1,0 +fwd_v=0,1,0 +fwd_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Negative degree.
      {.definition =
           "+proj=horner +deg=-1 +fwd_u=0,1,0 +fwd_v=0,1,0 +fwd_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      // Unreasonably large degree (> 10000).
      {.definition =
           "+proj=horner +deg=10001 +fwd_u=0,1,0 +fwd_v=0,1,0 +fwd_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE},
      // Missing +fwd_u.
      {.definition = "+proj=horner +deg=1 +fwd_v=0,1,0 +fwd_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Malformed +fwd_u (needs 3 comma-separated values for deg=1, got 2).
      {.definition =
           "+proj=horner +deg=1 +fwd_u=0,1 +fwd_v=0,1,0 +fwd_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Missing +fwd_v.
      {.definition = "+proj=horner +deg=1 +fwd_u=0,1,0 +fwd_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // has_inv triggered by +inv_v, but +inv_u is missing.
      {.definition = "+proj=horner +deg=1 +fwd_u=0,1,0 +fwd_v=0,1,0 "
                     "+inv_v=0,1,0 +fwd_origin=0,0 +inv_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // has_inv triggered by +inv_u, but +inv_v is missing.
      {.definition = "+proj=horner +deg=1 +fwd_u=0,1,0 +fwd_v=0,1,0 "
                     "+inv_u=0,1,0 +fwd_origin=0,0 +inv_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Missing +fwd_origin.
      {.definition = "+proj=horner +deg=1 +fwd_u=0,1,0 +fwd_v=0,1,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Malformed +fwd_origin (only 1 value instead of 2).
      {.definition =
           "+proj=horner +deg=1 +fwd_u=0,1,0 +fwd_v=0,1,0 +fwd_origin=10",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Missing +inv_origin when +inv_u and +inv_v are provided.
      {.definition = "+proj=horner +deg=1 +fwd_u=0,1,0 +fwd_v=0,1,0 "
                     "+inv_u=0,1,0 +inv_v=0,1,0 +fwd_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Complex polynomial: +inv_c provided without +fwd_c.
      {.definition =
           "+proj=horner +deg=1 +inv_c=0,0,1,0 +fwd_origin=0,0 +inv_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Complex polynomial: +inv_origin provided without +inv_c.
      {.definition =
           "+proj=horner +deg=1 +fwd_c=0,0,1,0 +fwd_origin=0,0 +inv_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
      // Complex polynomial: malformed +fwd_c (needs 4 values for deg=1, got 3).
      {.definition = "+proj=horner +deg=1 +fwd_c=0,0,1 +fwd_origin=0,0",
       .expected_err = PROJ_ERR_INVALID_OP_MISSING_ARG},
  };

  for (const auto& tc : kCases) {
    proj_context_errno_set(ctx.get(), 0);
    ScopedPj pj(proj_create(ctx.get(), tc.definition));
    EXPECT_EQ(nullptr, pj.get()) << tc.definition;
    EXPECT_EQ(tc.expected_err, proj_context_errno(ctx.get())) << tc.definition;
  }
}

}  // namespace
}  // namespace proj
