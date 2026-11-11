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
// Unit tests for PROJ ellipsoid parameter parsing, calculation, inheritance,
// and spherification conversions (proj/src/ell_set.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/ell_set.cpp

#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <span>  // NOLINT(build/c++20)
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kHalfPi = kPi / 2.0;

constexpr std::array<std::string_view, 4> Params(
    std::string_view p0, std::string_view p1 = {}, std::string_view p2 = {},
    std::string_view p3 = {}) noexcept {
  return {{p0, p1, p2, p3}};
}

// RAII owner for a paralist built via pj_mkparam that frees nodes directly
// without disturbing ctx->last_errno. Empty views are ignored so fixed-width
// std::array tables can omit unused trailing slots.
class ScopedParalist {
 public:
  explicit ScopedParalist(std::span<const std::string_view> tokens) {
    paralist* tail = nullptr;
    for (const std::string_view tok : tokens) {
      if (tok.empty()) {
        continue;
      }
      const std::string str(tok);
      paralist* node = pj_mkparam(str.c_str());
      if (node == nullptr) {
        Reset();
        return;
      }
      if (head_ == nullptr) {
        head_ = node;
      } else {
        tail->next = node;
      }
      tail = node;
    }
  }

  ~ScopedParalist() { Reset(); }

  ScopedParalist(const ScopedParalist&) = delete;
  ScopedParalist& operator=(const ScopedParalist&) = delete;

  paralist* get() const noexcept { return head_; }

 private:
  void Reset() noexcept {
    while (head_ != nullptr) {
      paralist* next = head_->next;
      std::free(head_);
      head_ = next;
    }
  }

  paralist* head_ = nullptr;
};

void FreeEllipsoidDefs(PJ* pj) noexcept {
  std::free(pj->def_size);
  pj->def_size = nullptr;
  std::free(pj->def_shape);
  pj->def_shape = nullptr;
  std::free(pj->def_spherification);
  pj->def_spherification = nullptr;
  std::free(pj->def_ellps);
  pj->def_ellps = nullptr;
}

TEST(EllSetTest, PjEllSetResolvesBuiltInsShapeModesAndOverrides) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr double kWgs84A = 6378137.0;
  constexpr double kWgs84Rf = 298.257223563;
  constexpr double kWgs84F = 1.0 / kWgs84Rf;
  constexpr double kWgs84Es = 2.0 * kWgs84F - kWgs84F * kWgs84F;

  struct EllSetCase {
    std::string_view label;
    std::array<std::string_view, 4> params;
    double expected_a;
    double expected_es;
  };
  static constexpr EllSetCase kCases[] = {
      {.label = "WGS84",
       .params = Params("+ellps=WGS84"),
       .expected_a = kWgs84A,
       .expected_es = kWgs84Es},
      {.label = "clrk66_b",
       .params = Params("+ellps=clrk66"),
       .expected_a = 6378206.4,
       .expected_es = 2.0 * ((6378206.4 - 6356583.8) / 6378206.4) -
                      ((6378206.4 - 6356583.8) / 6378206.4) *
                          ((6378206.4 - 6356583.8) / 6378206.4)},
      {.label = "sphere_equal_b",
       .params = Params("+ellps=sphere"),
       .expected_a = 6370997.0,
       .expected_es = 0.0},
      {.label = "ellps_with_size_override",
       .params = Params("+ellps=WGS84", "+a=1"),
       .expected_a = 1.0,
       .expected_es = kWgs84Es},
      {.label = "ellps_with_rf_override",
       .params = Params("+ellps=WGS84", "+rf=300"),
       .expected_a = kWgs84A,
       .expected_es = 2.0 * (1.0 / 300.0) - (1.0 / 300.0) * (1.0 / 300.0)},
      {.label = "ellps_with_f_zero_override",
       .params = Params("+ellps=WGS84", "+f=0"),
       .expected_a = kWgs84A,
       .expected_es = 0.0},
      {.label = "R_overrules_ellps_shape_and_spherification",
       .params = Params("+R=6371000", "+ellps=WGS84", "+rf=298", "+R_A"),
       .expected_a = 6371000.0,
       .expected_es = 0.0},
      {.label = "a_without_shape_defaults_to_sphere",
       .params = Params("+a=6378000"),
       .expected_a = 6378000.0,
       .expected_es = 0.0},
      {.label = "explicit_a_and_es",
       .params = Params("+a=6378137", "+es=0.0066943799901413"),
       .expected_a = kWgs84A,
       .expected_es = 0.0066943799901413},
      {.label = "explicit_a_and_e",
       .params = Params("+a=6378137", "+e=0.0818191908426215"),
       .expected_a = kWgs84A,
       .expected_es = 0.0818191908426215 * 0.0818191908426215},
      {.label = "explicit_a_and_b",
       .params = Params("+a=6378137", "+b=6356752.314245"),
       .expected_a = kWgs84A,
       .expected_es = 2.0 * ((kWgs84A - 6356752.314245) / kWgs84A) -
                      ((kWgs84A - 6356752.314245) / kWgs84A) *
                          ((kWgs84A - 6356752.314245) / kWgs84A)},
      {.label = "explicit_a_equal_b_is_sphere",
       .params = Params("+a=6378137", "+b=6378137"),
       .expected_a = kWgs84A,
       .expected_es = 0.0},
      {.label = "explicit_a_and_nonzero_f",
       .params = Params("+a=6378137", "+f=0.00335281066474748"),
       .expected_a = kWgs84A,
       .expected_es = 2.0 * 0.00335281066474748 -
                      0.00335281066474748 * 0.00335281066474748},
      {.label = "prefix_collisions_ignored",
       .params = Params("+R_unused=1", "+ellps_unused=bad", "+a=6371000"),
       .expected_a = 6371000.0,
       .expected_es = 0.0},
  };

  for (const auto& tc : kCases) {
    ScopedParalist params(tc.params);
    ASSERT_NE(nullptr, params.get()) << tc.label;
    double a = -1.0;
    double es = -1.0;
    EXPECT_EQ(0, pj_ell_set(ctx.get(), params.get(), &a, &es)) << tc.label;
    EXPECT_EQ(0, proj_context_errno(ctx.get())) << tc.label;
    EXPECT_NEAR(tc.expected_a, a, 1e-9) << tc.label;
    EXPECT_NEAR(tc.expected_es, es, 1e-14) << tc.label;
  }
}

TEST(EllSetTest, PjEllipsoidTraceLogsAndFreesPreviousDefsOnReinvocation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture trace(ctx.get(), PJ_LOG_TRACE);

  PJ pj;
  pj.ctx = ctx.get();
  absl::Cleanup free_defs = [&pj] { FreeEllipsoidDefs(&pj); };

  // 1. All four def_* tags populated and f == 0 after spherification.
  ScopedParalist all_tags(
      Params("+ellps=WGS84", "+a=6378137", "+rf=300", "+R_A"));
  pj.params = all_tags.get();
  ASSERT_EQ(0, pj_ellipsoid(&pj));
  EXPECT_STREQ("a=6378137", pj.def_size);
  EXPECT_STREQ("rf=300", pj.def_shape);
  EXPECT_STREQ("R_A", pj.def_spherification);
  EXPECT_STREQ("ellps=WGS84", pj.def_ellps);
  EXPECT_DOUBLE_EQ(0.0, pj.f);
  EXPECT_TRUE(trace.ContainsMessage("pj_ellipsoid - final:"));
  EXPECT_TRUE(trace.ContainsMessage("a=6378137 rf=300 R_A ellps=WGS84"));

  // 2. Re-invoke on same PJ with only +ellps=WGS84: frees step 1's four
  // def_* strings cleanly and logs 1 / f alongside unset def_* fields.
  trace.Clear();
  ScopedParalist only_ellps(Params("+ellps=WGS84"));
  pj.params = only_ellps.get();
  ASSERT_EQ(0, pj_ellipsoid(&pj));
  EXPECT_EQ(nullptr, pj.def_size);
  EXPECT_EQ(nullptr, pj.def_shape);
  EXPECT_EQ(nullptr, pj.def_spherification);
  EXPECT_STREQ("ellps=WGS84", pj.def_ellps);
  EXPECT_TRUE(trace.ContainsMessage("1/298.257"));

  // 3. Explicit size + shape leaves def_spherification and def_ellps nullptr.
  ScopedParalist size_shape(Params("+a=6378137", "+rf=300"));
  pj.params = size_shape.get();
  ASSERT_EQ(0, pj_ellipsoid(&pj));
  EXPECT_STREQ("a=6378137", pj.def_size);
  EXPECT_STREQ("rf=300", pj.def_shape);
  EXPECT_EQ(nullptr, pj.def_spherification);
  EXPECT_EQ(nullptr, pj.def_ellps);
}

TEST(EllSetTest, PjEllipsoidAndEllSetRejectInvalidParameters) {
  struct InvalidCase {
    std::string_view label;
    std::array<std::string_view, 4> params;
    int need_ellps;
    int expected_ret;
    int expected_errno;
    std::string_view expected_log;
  };
  static constexpr InvalidCase kInvalidCases[] = {
      {.label = "missing_axis_silent_when_need_ellps_0",
       .params = Params("+rf=298"),
       .need_ellps = 0,
       .expected_ret = 2,
       .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
       .expected_log = ""},
      {.label = "missing_axis_logs_when_need_ellps_1",
       .params = Params("+rf=298"),
       .need_ellps = 1,
       .expected_ret = 2,
       .expected_errno = PROJ_ERR_INVALID_OP_MISSING_ARG,
       .expected_log = "Major axis not given"},
      {.label = "ellps_short_flag_no_value",
       .params = Params("+ellps"),
       .need_ellps = 1,
       .expected_ret = 1,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for +ellps"},
      {.label = "ellps_empty_value",
       .params = Params("+ellps="),
       .need_ellps = 1,
       .expected_ret = 1,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for +ellps"},
      {.label = "ellps_unknown",
       .params = Params("+ellps=no_such_ellipsoid"),
       .need_ellps = 1,
       .expected_ret = 1,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Unrecognized value for +ellps"},
      {.label = "R_zero",
       .params = Params("+R=0"),
       .need_ellps = 1,
       .expected_ret = 1,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for major axis"},
      {.label = "R_negative",
       .params = Params("+R=-100"),
       .need_ellps = 1,
       .expected_ret = 1,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for major axis"},
      {.label = "R_inf",
       .params = Params("+R=inf"),
       .need_ellps = 1,
       .expected_ret = 1,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for major axis"},
      {.label = "a_zero",
       .params = Params("+a=0"),
       .need_ellps = 1,
       .expected_ret = 2,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for major axis"},
      {.label = "a_flag_without_equals",
       .params = Params("+a"),
       .need_ellps = 1,
       .expected_ret = 2,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for major axis"},
      {.label = "a_inf",
       .params = Params("+a=inf"),
       .need_ellps = 1,
       .expected_ret = 2,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for major axis"},
      {.label = "rf_zero",
       .params = Params("+a=1000", "+rf=0"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for rf"},
      {.label = "rf_inf",
       .params = Params("+a=1000", "+rf=inf"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for rf"},
      {.label = "rf_nan",
       .params = Params("+a=1000", "+rf=nan"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid eccentricity"},
      {.label = "f_negative",
       .params = Params("+a=1000", "+f=-0.01"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for f"},
      {.label = "f_inf",
       .params = Params("+a=1000", "+f=inf"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for f"},
      {.label = "f_above_two_negative_es",
       .params = Params("+a=1000", "+f=2.5"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid eccentricity"},
      {.label = "f_one_fails_calc_ellipsoid_params",
       .params = Params("+a=1000", "+f=1"),
       .need_ellps = 1,
       .expected_ret = 5,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid eccentricity"},
      {.label = "es_negative",
       .params = Params("+a=1000", "+es=-0.1"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for es"},
      {.label = "es_one",
       .params = Params("+a=1000", "+es=1"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for es"},
      {.label = "es_nan",
       .params = Params("+a=1000", "+es=nan"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid eccentricity"},
      {.label = "e_negative",
       .params = Params("+a=1000", "+e=-0.1"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for e"},
      {.label = "e_one",
       .params = Params("+a=1000", "+e=1"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for e"},
      {.label = "e_nan",
       .params = Params("+a=1000", "+e=nan"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid eccentricity"},
      {.label = "b_zero",
       .params = Params("+a=1000", "+b=0"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for b"},
      {.label = "b_inf",
       .params = Params("+a=1000", "+b=inf"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid value for b"},
      {.label = "b_prolate_gt_a",
       .params = Params("+a=1000", "+b=1200"),
       .need_ellps = 1,
       .expected_ret = 3,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid eccentricity"},
      {.label = "b_underflows_f_to_one",
       .params = Params("+a=6378137", "+b=1e-20"),
       .need_ellps = 1,
       .expected_ret = 5,
       .expected_errno = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected_log = "Invalid eccentricity"},
  };

  for (const auto& tc : kInvalidCases) {
    ScopedContext ctx = MakeContext();
    ASSERT_NE(nullptr, ctx.get());
    ScopedLogCapture log(ctx.get(), PJ_LOG_ERROR);
    ScopedParalist params(tc.params);
    ASSERT_NE(nullptr, params.get()) << tc.label;

    PJ pj;
    pj.ctx = ctx.get();
    pj.need_ellps = tc.need_ellps;
    pj.params = params.get();
    absl::Cleanup free_defs = [&pj] { FreeEllipsoidDefs(&pj); };

    EXPECT_EQ(tc.expected_ret, pj_ellipsoid(&pj)) << tc.label;
    EXPECT_EQ(tc.expected_errno, proj_context_errno(ctx.get())) << tc.label;
    if (tc.expected_log.empty()) {
      EXPECT_TRUE(log.entries().empty()) << tc.label;
    } else {
      EXPECT_TRUE(log.ContainsMessage(std::string(tc.expected_log)))
          << tc.label;
    }
  }

  // Verify pj_ell_set forwards failure return without overwriting outputs,
  // and proj_create rejects invalid explicit sphere radius.
  ScopedContext err_ctx = MakeContext();
  ASSERT_NE(nullptr, err_ctx.get());
  ScopedLogSuppressor suppress(err_ctx.get());
  ScopedParalist bad_params(Params("+R=0"));
  double a = 123.0;
  double es = 0.5;
  EXPECT_NE(0, pj_ell_set(err_ctx.get(), bad_params.get(), &a, &es));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(err_ctx.get()));
  EXPECT_DOUBLE_EQ(123.0, a);

  ScopedPj bad_proj(proj_create(err_ctx.get(), "+proj=merc +no_defs +R=0"));
  EXPECT_EQ(nullptr, bad_proj.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(err_ctx.get()));
}

TEST(EllSetTest, SpherificationModesAndErrorBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. All 8 spherification keywords produce spheres matching analytical radii.
  constexpr double kA = 6378137.0;
  constexpr double kF = 1.0 / 298.257223563;
  constexpr double kEs = 2.0 * kF - kF * kF;
  constexpr double kB = (1.0 - kF) * kA;

  const double lat45_sin2 = std::sin(kPi / 4.0) * std::sin(kPi / 4.0);
  const double t45 = 1.0 - kEs * lat45_sin2;
  const double lat30_sin2 = std::sin(-kPi / 6.0) * std::sin(-kPi / 6.0);
  const double t30 = 1.0 - kEs * lat30_sin2;

  struct SpherCase {
    std::string_view key;
    double phi0_rad;
    double expected_r;
  };
  const SpherCase kSpherCases[] = {
      {.key = "+R_A",
       .phi0_rad = 0.0,
       .expected_r =
           kA * (1.0 - kEs * (1.0 / 6.0 +
                              kEs * (17.0 / 360.0 + kEs * (67.0 / 3024.0))))},
      {.key = "+R_V",
       .phi0_rad = 0.0,
       .expected_r =
           kA * (1.0 - kEs * (1.0 / 6.0 +
                              kEs * (5.0 / 72.0 + kEs * (55.0 / 1296.0))))},
      {.key = "+R_a", .phi0_rad = 0.0, .expected_r = (kA + kB) / 2.0},
      {.key = "+R_g", .phi0_rad = 0.0, .expected_r = std::sqrt(kA * kB)},
      {.key = "+R_h",
       .phi0_rad = 0.0,
       .expected_r = (2.0 * kA * kB) / (kA + kB)},
      {.key = "+R_lat_a=45",
       .phi0_rad = 0.0,
       .expected_r = kA * (1.0 - kEs + t45) / (2.0 * t45 * std::sqrt(t45))},
      {.key = "+R_lat_g=-30d",
       .phi0_rad = 0.0,
       .expected_r = kA * std::sqrt(1.0 - kEs) / t30},
      {.key = "+R_C",
       .phi0_rad = kPi / 4.0,
       .expected_r = kA * std::sqrt(1.0 - kEs) / t45},
  };

  for (const auto& tc : kSpherCases) {
    ScopedParalist params(Params("+ellps=WGS84", tc.key));
    PJ pj;
    pj.ctx = ctx.get();
    pj.phi0 = tc.phi0_rad;
    pj.params = params.get();
    absl::Cleanup free_defs = [&pj] { FreeEllipsoidDefs(&pj); };

    EXPECT_EQ(0, pj_ellipsoid(&pj)) << tc.key;
    EXPECT_EQ(0, proj_context_errno(ctx.get())) << tc.key;
    EXPECT_NEAR(tc.expected_r, pj.a, 1e-6) << tc.key;
    EXPECT_NEAR(tc.expected_r, pj.b, 1e-6) << tc.key;
    EXPECT_DOUBLE_EQ(0.0, pj.es) << tc.key;
    EXPECT_DOUBLE_EQ(0.0, pj.e) << tc.key;
    EXPECT_DOUBLE_EQ(0.0, pj.f) << tc.key;
    EXPECT_DOUBLE_EQ(1.0, pj.one_es) << tc.key;
    EXPECT_TRUE(std::isinf(pj.rf)) << tc.key;
  }

  // 2. End-to-end projection check confirms +R_A radius reaches proj_trans.
  ScopedPj merc_ra(proj_create(ctx.get(), "+proj=merc +ellps=WGS84 +R_A"));
  ASSERT_NE(nullptr, merc_ra.get());
  const PJ_COORD eq_one_rad =
      proj_trans(merc_ra.get(), PJ_FWD, proj_coord(1.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(merc_ra.get()));
  EXPECT_NEAR(kSpherCases[0].expected_r, eq_one_rad.xy.x, 1e-6);

  // 3. Error branches in ellps_spherification.
  ScopedLogCapture err_log(ctx.get(), PJ_LOG_ERROR);

  struct SpherErrorCase {
    std::string_view label;
    std::array<std::string_view, 4> params;
    double phi0_rad;
    std::string_view expected_log;
  };
  static constexpr SpherErrorCase kErrorCases[] = {
      {.label = "R_lat_a_out_of_range",
       .params = Params("+ellps=WGS84", "+R_lat_a=95"),
       .phi0_rad = 0.0,
       .expected_log = "Invalid value for lat_g"},
      {.label = "R_lat_g_negative_out_of_range",
       .params = Params("+ellps=WGS84", "+R_lat_g=-95"),
       .phi0_rad = 0.0,
       .expected_log = "Invalid value for lat_g"},
      {.label = "R_lat_a_degenerate_pole_t_zero",
       .params = Params("+a=6378137", "+f=1", "+R_lat_a=90"),
       .phi0_rad = 0.0,
       .expected_log = "Invalid eccentricity"},
      {.label = "R_C_degenerate_pole_t_zero",
       .params = Params("+a=6378137", "+f=1", "+R_C"),
       .phi0_rad = kHalfPi,
       .expected_log = "Invalid eccentricity"},
      {.label = "R_g_zero_b_gives_zero_axis",
       .params = Params("+a=6378137", "+f=1", "+R_g"),
       .phi0_rad = 0.0,
       .expected_log = "Invalid or missing major axis"},
      {.label = "R_h_zero_b_gives_zero_axis",
       .params = Params("+a=6378137", "+f=1", "+R_h"),
       .phi0_rad = 0.0,
       .expected_log = "Invalid or missing major axis"},
  };

  for (const auto& tc : kErrorCases) {
    err_log.Clear();
    ScopedParalist params(tc.params);
    PJ pj;
    pj.ctx = ctx.get();
    pj.phi0 = tc.phi0_rad;
    pj.params = params.get();
    absl::Cleanup free_defs = [&pj] { FreeEllipsoidDefs(&pj); };

    EXPECT_EQ(4, pj_ellipsoid(&pj)) << tc.label;
    EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
              proj_context_errno(ctx.get()))
        << tc.label;
    EXPECT_TRUE(err_log.ContainsMessage(std::string(tc.expected_log)))
        << tc.label;
  }
}

TEST(EllSetTest, CalcEllipsoidParamsAndInheritDefCoverAllFieldsAndBounds) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Spherical calculation sets eccentricities/flattenings to 0 and inverses
  // to HUGE_VAL.
  PJ sph;
  sph.ctx = ctx.get();
  ASSERT_EQ(0, pj_calc_ellipsoid_params(&sph, 6371000.0, 0.0));
  EXPECT_DOUBLE_EQ(6371000.0, sph.a);
  EXPECT_DOUBLE_EQ(6371000.0, sph.b);
  EXPECT_DOUBLE_EQ(1.0 / 6371000.0, sph.ra);
  EXPECT_DOUBLE_EQ(1.0 / 6371000.0, sph.rb);
  EXPECT_DOUBLE_EQ(0.0, sph.e);
  EXPECT_DOUBLE_EQ(0.0, sph.alpha);
  EXPECT_DOUBLE_EQ(0.0, sph.e2);
  EXPECT_DOUBLE_EQ(0.0, sph.e3);
  EXPECT_DOUBLE_EQ(0.0, sph.f);
  EXPECT_DOUBLE_EQ(0.0, sph.f2);
  EXPECT_DOUBLE_EQ(0.0, sph.n);
  EXPECT_TRUE(std::isinf(sph.rf));
  EXPECT_TRUE(std::isinf(sph.rf2));
  EXPECT_TRUE(std::isinf(sph.rn));
  EXPECT_DOUBLE_EQ(1.0, sph.one_es);
  EXPECT_DOUBLE_EQ(1.0, sph.rone_es);

  // 2. Ellipsoidal WGS84 calculation verifies Rapp ancillary formulas.
  constexpr double kA = 6378137.0;
  constexpr double kF = 1.0 / 298.257223563;
  constexpr double kEs = 2.0 * kF - kF * kF;
  PJ wgs;
  wgs.ctx = ctx.get();
  ASSERT_EQ(0, pj_calc_ellipsoid_params(&wgs, kA, kEs));
  EXPECT_NEAR(std::sqrt(kEs), wgs.e, 1e-13);
  EXPECT_NEAR(kA * (1.0 - kF), wgs.b, 1e-6);
  EXPECT_NEAR(kF, wgs.f, 1e-13);
  EXPECT_NEAR(1.0 / kF, wgs.rf, 1e-9);
  EXPECT_GT(wgs.e2, 0.0);
  EXPECT_GT(wgs.e3, 0.0);
  EXPECT_GT(wgs.f2, 0.0);
  EXPECT_NEAR(kF / (2.0 - kF), wgs.n, 1e-13);
  EXPECT_NEAR(1.0 - kEs, wgs.one_es, 1e-13);
  EXPECT_NEAR(1.0 / (1.0 - kEs), wgs.rone_es, 1e-12);

  // 3. Failure branches: f >= 1.0 and one_es == 0.0 when preset f < 1.0.
  ScopedLogSuppressor suppress(ctx.get());
  PJ bad_f;
  bad_f.ctx = ctx.get();
  bad_f.f = 1.0;
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            pj_calc_ellipsoid_params(&bad_f, 1000.0, 0.0));

  PJ bad_one_es;
  bad_one_es.ctx = ctx.get();
  bad_one_es.f = 0.9;
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            pj_calc_ellipsoid_params(&bad_one_es, 1000.0, 1.0));

  // 4. pj_inherit_ellipsoid_def copies all 22 ellipsoid parameters cleanly.
  PJ src;
  src.a = 1.0;
  src.b = 2.0;
  src.ra = 3.0;
  src.rb = 4.0;
  src.alpha = 5.0;
  src.e = 6.0;
  src.es = 7.0;
  src.e2 = 8.0;
  src.e2s = 9.0;
  src.e3 = 10.0;
  src.e3s = 11.0;
  src.one_es = 12.0;
  src.rone_es = 13.0;
  src.f = 14.0;
  src.f2 = 15.0;
  src.n = 16.0;
  src.rf = 17.0;
  src.rf2 = 18.0;
  src.rn = 19.0;
  src.J = 20.0;
  src.es_orig = 21.0;
  src.a_orig = 22.0;

  PJ dst;
  pj_inherit_ellipsoid_def(&src, &dst);
  EXPECT_DOUBLE_EQ(1.0, dst.a);
  EXPECT_DOUBLE_EQ(2.0, dst.b);
  EXPECT_DOUBLE_EQ(3.0, dst.ra);
  EXPECT_DOUBLE_EQ(4.0, dst.rb);
  EXPECT_DOUBLE_EQ(5.0, dst.alpha);
  EXPECT_DOUBLE_EQ(6.0, dst.e);
  EXPECT_DOUBLE_EQ(7.0, dst.es);
  EXPECT_DOUBLE_EQ(8.0, dst.e2);
  EXPECT_DOUBLE_EQ(9.0, dst.e2s);
  EXPECT_DOUBLE_EQ(10.0, dst.e3);
  EXPECT_DOUBLE_EQ(11.0, dst.e3s);
  EXPECT_DOUBLE_EQ(12.0, dst.one_es);
  EXPECT_DOUBLE_EQ(13.0, dst.rone_es);
  EXPECT_DOUBLE_EQ(14.0, dst.f);
  EXPECT_DOUBLE_EQ(15.0, dst.f2);
  EXPECT_DOUBLE_EQ(16.0, dst.n);
  EXPECT_DOUBLE_EQ(17.0, dst.rf);
  EXPECT_DOUBLE_EQ(18.0, dst.rf2);
  EXPECT_DOUBLE_EQ(19.0, dst.rn);
  EXPECT_DOUBLE_EQ(20.0, dst.J);
  EXPECT_DOUBLE_EQ(21.0, dst.es_orig);
  EXPECT_DOUBLE_EQ(22.0, dst.a_orig);
}

// TODO(schwehr): Defensive OOM/unreachable checks in ell_set.cpp (lines 98,
// 168, 171-172, 186, 188, 337, 408-409, 481, 496) cannot be reached through
// built-in pj_list_ellps() entries or normal parameter lists without fault
// injection.

}  // namespace
}  // namespace proj
