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
// Unit tests for complex Horner polynomial and derivative evaluators
// pj_zpoly1() and pj_zpolyd1() (proj/src/zpoly1.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/zpoly1.cpp

#include <cmath>
#include <complex>
#include <cstddef>
#include <iterator>
#include <numbers>
#include <span>  // NOLINT(build/c++20)

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

// Evaluates P(z) = sum_{k=0}^{n} C[k] * z^(k+1), where coeffs has n+1 entries.
std::complex<double> EvalPolyOracle(std::complex<double> z,
                                    std::span<const COMPLEX> coeffs) {
  std::complex<double> sum(0.0, 0.0);
  std::complex<double> z_pow = z;
  for (const COMPLEX& c : coeffs) {
    sum += std::complex<double>(c.r, c.i) * z_pow;
    z_pow *= z;
  }
  return sum;
}

// Evaluates P'(z) = sum_{k=0}^{n} (k+1) * C[k] * z^k.
std::complex<double> EvalDerivOracle(std::complex<double> z,
                                     std::span<const COMPLEX> coeffs) {
  std::complex<double> sum(0.0, 0.0);
  std::complex<double> z_pow(1.0, 0.0);
  for (size_t k = 0; k < coeffs.size(); ++k) {
    sum += static_cast<double>(k + 1) *
           std::complex<double>(coeffs[k].r, coeffs[k].i) * z_pow;
    z_pow *= z;
  }
  return sum;
}

TEST(Zpoly1Test, ZeroInputReturnsZeroValueAndLinearTermAsDerivative) {
  constexpr COMPLEX kCoeffs[] = {
      {.r = 1.5, .i = -2.25},
      {.r = -0.5, .i = 0.75},
      {.r = 0.25, .i = -0.125},
  };
  constexpr COMPLEX kZero = {.r = 0.0, .i = 0.0};

  // Degree parameter n = 2 (array length n + 1 = 3).
  const COMPLEX val1 = pj_zpoly1(kZero, kCoeffs, 1);
  EXPECT_DOUBLE_EQ(0.0, val1.r);
  EXPECT_DOUBLE_EQ(0.0, val1.i);

  const COMPLEX val2 = pj_zpoly1(kZero, kCoeffs, 2);
  EXPECT_DOUBLE_EQ(0.0, val2.r);
  EXPECT_DOUBLE_EQ(0.0, val2.i);

  COMPLEX der = {.r = 0.0, .i = 0.0};
  const COMPLEX val_d = pj_zpolyd1(kZero, kCoeffs, 2, &der);
  EXPECT_DOUBLE_EQ(0.0, val_d.r);
  EXPECT_DOUBLE_EQ(0.0, val_d.i);
  EXPECT_DOUBLE_EQ(kCoeffs[0].r, der.r);
  EXPECT_DOUBLE_EQ(kCoeffs[0].i, der.i);
}

TEST(Zpoly1Test, Zpoly1MatchesComplexOracleForLinearAndHigherDegrees) {
  // 1. Degenerate n = 0 evaluates C[0] * z directly without entering the loop.
  constexpr COMPLEX kSingle[] = {{.r = 2.5, .i = -1.25}};
  constexpr COMPLEX kZInputs[] = {
      {.r = 1.5, .i = 0.0},
      {.r = 0.0, .i = -2.0},
      {.r = 0.35, .i = -0.45},
      {.r = -1.1, .i = 0.8},
  };
  for (const COMPLEX& z : kZInputs) {
    const COMPLEX actual = pj_zpoly1(z, kSingle, 0);
    const std::complex<double> expected = EvalPolyOracle({z.r, z.i}, kSingle);
    EXPECT_NEAR(expected.real(), actual.r, 1e-14);
    EXPECT_NEAR(expected.imag(), actual.i, 1e-14);
  }

  // 2. Higher-degree complex polynomial (n = 5, 6 coefficients).
  constexpr COMPLEX kCoeffs[] = {
      {.r = 0.7557853228, .i = 0.0},
      {.r = 0.249204646, .i = 0.003371507},
      {.r = -0.001541739, .i = 0.041058560},
      {.r = -0.10162907, .i = 0.01727609},
      {.r = -0.26623489, .i = -0.36249218},
      {.r = -0.6870983, .i = -1.1651967},
  };
  constexpr int kDegreeN = static_cast<int>(std::size(kCoeffs)) - 1;

  for (const COMPLEX& z : kZInputs) {
    const COMPLEX actual = pj_zpoly1(z, kCoeffs, kDegreeN);
    const std::complex<double> expected = EvalPolyOracle({z.r, z.i}, kCoeffs);
    EXPECT_NEAR(expected.real(), actual.r, 1e-12);
    EXPECT_NEAR(expected.imag(), actual.i, 1e-12);
  }
}

TEST(Zpoly1Test, Zpolyd1MatchesValueDerivativeOracleAndFiniteDifferences) {
  constexpr COMPLEX kCoeffs[] = {
      {.r = 1.25, .i = -0.5},
      {.r = -0.75, .i = 1.5},
      {.r = 0.4, .i = -0.3},
      {.r = -0.15, .i = 0.2},
  };

  constexpr COMPLEX kZInputs[] = {
      {.r = 0.8, .i = 0.0},
      {.r = 0.0, .i = 0.65},
      {.r = -0.45, .i = 0.55},
      {.r = 1.15, .i = -0.7},
  };

  // Exercise both n = 1 (if (first) branch only) and n = 2, 3 (enters else).
  for (const int n : {1, 2, 3}) {
    const std::span<const COMPLEX> sub_coeffs(kCoeffs,
                                              static_cast<size_t>(n) + 1);
    for (const COMPLEX& z : kZInputs) {
      COMPLEX der = {.r = 0.0, .i = 0.0};
      const COMPLEX actual = pj_zpolyd1(z, kCoeffs, n, &der);
      const COMPLEX value_only = pj_zpoly1(z, kCoeffs, n);

      EXPECT_NEAR(value_only.r, actual.r, 1e-14) << "n=" << n;
      EXPECT_NEAR(value_only.i, actual.i, 1e-14) << "n=" << n;

      const std::complex<double> expected_val =
          EvalPolyOracle({z.r, z.i}, sub_coeffs);
      const std::complex<double> expected_der =
          EvalDerivOracle({z.r, z.i}, sub_coeffs);
      EXPECT_NEAR(expected_val.real(), actual.r, 1e-13) << "n=" << n;
      EXPECT_NEAR(expected_val.imag(), actual.i, 1e-13) << "n=" << n;
      EXPECT_NEAR(expected_der.real(), der.r, 1e-13) << "n=" << n;
      EXPECT_NEAR(expected_der.imag(), der.i, 1e-13) << "n=" << n;

      // Verify Cauchy-Riemann complex derivative via real and imaginary steps.
      constexpr double kH = 1e-6;
      const COMPLEX p_plus_r = pj_zpoly1({.r = z.r + kH, .i = z.i}, kCoeffs, n);
      const COMPLEX p_minus_r =
          pj_zpoly1({.r = z.r - kH, .i = z.i}, kCoeffs, n);
      EXPECT_NEAR((p_plus_r.r - p_minus_r.r) / (2.0 * kH), der.r, 1e-8);
      EXPECT_NEAR((p_plus_r.i - p_minus_r.i) / (2.0 * kH), der.i, 1e-8);

      const COMPLEX p_plus_i = pj_zpoly1({.r = z.r, .i = z.i + kH}, kCoeffs, n);
      const COMPLEX p_minus_i =
          pj_zpoly1({.r = z.r, .i = z.i - kH}, kCoeffs, n);
      // dP/d(iy) = i * P'(z) => P'(z) = (dP/dy) / i.
      EXPECT_NEAR((p_plus_i.i - p_minus_i.i) / (2.0 * kH), der.r, 1e-8);
      EXPECT_NEAR(-(p_plus_i.r - p_minus_i.r) / (2.0 * kH), der.i, 1e-8);
    }
  }
}

TEST(Zpoly1Test, NzmgForwardAndInverseRoundTripViaComplexPolynomials) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj nzmg(proj_create(ctx.get(), "+proj=nzmg"));
  ASSERT_NE(nullptr, nzmg.get());

  // 1. Forward NZMG calls pj_zpoly1() on degree-5 complex series.
  const PJ_COORD wellington =
      proj_coord(DegToRad(174.7762), DegToRad(-41.2865), 0.0, 0.0);
  const PJ_COORD xy = proj_trans(nzmg.get(), PJ_FWD, wellington);
  EXPECT_EQ(0, proj_errno(nzmg.get()));
  EXPECT_FALSE(std::isinf(xy.xy.x));

  // 2. Inverse NZMG solves via Newton iteration calling pj_zpolyd1().
  const PJ_COORD roundtrip = proj_trans(nzmg.get(), PJ_INV, xy);
  EXPECT_EQ(0, proj_errno(nzmg.get()));
  EXPECT_NEAR(wellington.lp.lam, roundtrip.lp.lam, 1e-10);
  EXPECT_NEAR(wellington.lp.phi, roundtrip.lp.phi, 1e-10);
}

}  // namespace
}  // namespace proj
