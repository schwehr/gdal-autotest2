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
// Unit tests for PROJ parameter list construction, lookup, typed extraction,
// and +t_final parsing (proj/src/param.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/param.cpp

#include <cmath>
#include <ctime>
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

class ScopedParalist {
 public:
  ScopedParalist(PJ_CONTEXT* ctx, paralist* head) noexcept
      : ctx_(ctx), head_(head) {}

  ~ScopedParalist() {
    if (head_ != nullptr) {
      free_params(ctx_, head_, 0);
    }
  }

  ScopedParalist(const ScopedParalist&) = delete;
  ScopedParalist& operator=(const ScopedParalist&) = delete;

  void Append(paralist* node) noexcept {
    if (head_ == nullptr) {
      head_ = node;
      return;
    }
    paralist* tail = head_;
    while (tail->next != nullptr) {
      tail = tail->next;
    }
    tail->next = node;
  }

  paralist* get() const noexcept { return head_; }
  paralist* release() noexcept {
    paralist* out = head_;
    head_ = nullptr;
    return out;
  }

 private:
  PJ_CONTEXT* ctx_;
  paralist* head_;
};

double ComputeExpectedYearFraction(std::time_t timestamp) {
  std::tm date{};
  localtime_r(&timestamp, &date);
  const int year = date.tm_year + 1900;
  const int days_in_year =
      (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
  return 1900.0 + date.tm_year +
         date.tm_yday / static_cast<double>(days_in_year);
}

TEST(ParamTest, MkparamAndMkparamWsParseTokensWhitespaceAndQuotes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. pj_mkparam strips leading '+' and preserves strings without '+'.
  ScopedParalist with_plus(ctx.get(), pj_mkparam("+proj=merc"));
  ASSERT_NE(nullptr, with_plus.get());
  EXPECT_EQ(0, with_plus.get()->used);
  EXPECT_EQ(nullptr, with_plus.get()->next);
  EXPECT_STREQ("proj=merc", with_plus.get()->param);

  ScopedParalist without_plus(ctx.get(), pj_mkparam("ellps=WGS84"));
  ASSERT_NE(nullptr, without_plus.get());
  EXPECT_STREQ("ellps=WGS84", without_plus.get()->param);

  ScopedParalist empty_param(ctx.get(), pj_mkparam(""));
  ASSERT_NE(nullptr, empty_param.get());
  EXPECT_STREQ("", empty_param.get()->param);

  // 2. pj_mkparam_ws handles nullptr, whitespace, '+', quotes, and "" escapes.
  const char* untouched_next = "sentinel";
  EXPECT_EQ(nullptr, pj_mkparam_ws(nullptr, &untouched_next));
  EXPECT_STREQ("sentinel", untouched_next);

  const char* input1 = " \t +proj=tmerc +zone=32";
  const char* next1 = nullptr;
  ScopedParalist ws_plus(ctx.get(), pj_mkparam_ws(input1, &next1));
  ASSERT_NE(nullptr, ws_plus.get());
  EXPECT_STREQ("proj=tmerc", ws_plus.get()->param);
  EXPECT_STREQ(" +zone=32", next1);

  const char* input2 = "  title=\"\"alpha \"\"beta\"\" gamma\"   tail";
  const char* next2 = nullptr;
  ScopedParalist ws_quoted(ctx.get(), pj_mkparam_ws(input2, &next2));
  ASSERT_NE(nullptr, ws_quoted.get());
  EXPECT_STREQ("title=\"\"alpha \"\"beta\"\" gamma\"", ws_quoted.get()->param);
  EXPECT_STREQ("   tail", next2);

  // Unterminated quote consumes to NUL and handles next_str == nullptr.
  ScopedParalist ws_unterminated(
      ctx.get(), pj_mkparam_ws("desc=\"\"unterminated text", nullptr));
  ASSERT_NE(nullptr, ws_unterminated.get());
  EXPECT_STREQ("desc=\"\"unterminated text", ws_unterminated.get()->param);
}

TEST(ParamTest, ParamExistsMatchesExactNamesStripsEqualsAndStopsOnStep) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  EXPECT_EQ(nullptr, pj_param_exists(nullptr, "proj"));

  ScopedParalist list(ctx.get(), pj_mkparam("+proj=merc"));
  ASSERT_NE(nullptr, list.get());
  list.Append(pj_mkparam("+lat_0=45"));
  list.Append(pj_mkparam("+over"));
  list.Append(pj_mkparam("+step"));

  // Partial key prefix must not match longer key.
  EXPECT_EQ(nullptr, pj_param_exists(list.get(), "lat"));

  // Query containing '=value' still matches key name before '=' and sets used.
  paralist* found_lat0 = pj_param_exists(list.get(), "lat_0=999");
  ASSERT_NE(nullptr, found_lat0);
  EXPECT_EQ(1, found_lat0->used);
  EXPECT_STREQ("lat_0=45", found_lat0->param);

  paralist* found_flag = pj_param_exists(list.get(), "over");
  ASSERT_NE(nullptr, found_flag);
  EXPECT_EQ(1, found_flag->used);
  EXPECT_STREQ("over", found_flag->param);

  // Searching for "step" returns non-null only if the current element is
  // "step", terminating search immediately after the first non-"step" node.
  EXPECT_EQ(nullptr, pj_param_exists(list.get(), "step"));
  ScopedParalist step_first(ctx.get(), pj_mkparam("+step"));
  ASSERT_NE(nullptr, step_first.get());
  EXPECT_EQ(step_first.get(), pj_param_exists(step_first.get(), "step"));
  EXPECT_EQ(1, step_first.get()->used);
}

TEST(ParamTest, PjParamExtractsAllTypesValidatesErrorsAndHandlesMissing) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Missing parameter / null paralist defaults across all 6 type codes.
  EXPECT_EQ(0, pj_param(ctx.get(), nullptr, "tmissing").i);
  EXPECT_EQ(0, pj_param(ctx.get(), nullptr, "bmissing").i);
  EXPECT_EQ(0, pj_param(ctx.get(), nullptr, "imissing").i);
  EXPECT_DOUBLE_EQ(0.0, pj_param(ctx.get(), nullptr, "dmissing").f);
  EXPECT_DOUBLE_EQ(0.0, pj_param(ctx.get(), nullptr, "rmissing").f);
  EXPECT_EQ(nullptr, pj_param(ctx.get(), nullptr, "smissing").s);

  // 2. Build parameter list covering every valid and invalid type case.
  ScopedParalist list(ctx.get(), pj_mkparam("+zone=32"));
  ASSERT_NE(nullptr, list.get());
  list.Append(pj_mkparam("+bad_int_alpha=32x"));
  list.Append(pj_mkparam("+bad_int_neg=-5"));
  list.Append(pj_mkparam("+scale=0.9996"));
  list.Append(pj_mkparam("+lon_0=15d30'"));
  list.Append(pj_mkparam("+datum=WGS84"));
  list.Append(pj_mkparam("+bare_flag"));
  list.Append(pj_mkparam("+bool_t=t"));
  list.Append(pj_mkparam("+bool_T=T"));
  list.Append(pj_mkparam("+bool_f=f"));
  list.Append(pj_mkparam("+bool_F=F"));
  list.Append(pj_mkparam("+bool_bad=yes"));

  // Presence ('t').
  EXPECT_EQ(1, pj_param(ctx.get(), list.get(), "tzone").i);
  EXPECT_EQ(0, pj_param(ctx.get(), list.get(), "tnonexistent").i);

  // Integer ('i'): valid vs non-digit rejection.
  EXPECT_EQ(32, pj_param(ctx.get(), list.get(), "izone").i);
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  EXPECT_EQ(0, pj_param(ctx.get(), list.get(), "ibad_int_alpha").i);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  EXPECT_EQ(0, pj_param(ctx.get(), list.get(), "ibad_int_neg").i);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  // Floating-point ('d') and DMS->radians ('r').
  EXPECT_NEAR(0.9996, pj_param(ctx.get(), list.get(), "dscale").f, 1e-12);
  EXPECT_NEAR(DegToRad(15.5), pj_param(ctx.get(), list.get(), "rlon_0").f,
              1e-12);

  // String ('s'): keyed value and bare flag pointing to NUL terminator.
  EXPECT_STREQ("WGS84", pj_param(ctx.get(), list.get(), "sdatum").s);
  EXPECT_STREQ("", pj_param(ctx.get(), list.get(), "sbare_flag").s);

  // Boolean ('b'): t/T/bare -> 1, f/F -> 0, invalid -> 0 + errno.
  EXPECT_EQ(1, pj_param(ctx.get(), list.get(), "bbare_flag").i);
  EXPECT_EQ(1, pj_param(ctx.get(), list.get(), "bbool_t").i);
  EXPECT_EQ(1, pj_param(ctx.get(), list.get(), "bbool_T").i);
  EXPECT_EQ(0, pj_param(ctx.get(), list.get(), "bbool_f").i);
  EXPECT_EQ(0, pj_param(ctx.get(), list.get(), "bbool_F").i);
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  EXPECT_EQ(0, pj_param(ctx.get(), list.get(), "bbool_bad").i);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  // 3. Passing ctx == nullptr falls back cleanly to pj_get_default_ctx().
  absl::Cleanup reset_default_ctx = [] { proj_errno_reset(nullptr); };
  EXPECT_EQ(32, pj_param(nullptr, list.get(), "izone").i);
  EXPECT_EQ(0, proj_context_errno(nullptr));
}

TEST(ParamDeathTest, PjParamInvalidRequestTypeExits) {
  EXPECT_EXIT(
      {
        ScopedContext ctx = MakeContext();
        if (ctx.get() != nullptr) {
          pj_param(ctx.get(), nullptr, "xfoo");
        }
      },
      ::testing::ExitedWithCode(1), "invalid request to pj_param, fatal");
}

TEST(ParamTest, ParseTFinalHandlesAbsentNumericInvalidAndNow) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Absent +t_final returns 0.0.
  ScopedPj absent(pj_new());
  ASSERT_NE(nullptr, absent.get());
  absent->ctx = ctx.get();
  EXPECT_DOUBLE_EQ(0.0, pj_parse_t_final(absent.get()));

  // 2. Numeric +t_final returns the parsed floating-point year.
  ScopedParalist num_param(ctx.get(), pj_mkparam("+t_final=2030.25"));
  ASSERT_NE(nullptr, num_param.get());
  ScopedPj numeric(pj_new());
  ASSERT_NE(nullptr, numeric.get());
  numeric->ctx = ctx.get();
  numeric->params = num_param.release();
  EXPECT_DOUBLE_EQ(2030.25, pj_parse_t_final(numeric.get()));

  // 3. Non-numeric non-"now" value returns 0.0.
  ScopedParalist bad_param(ctx.get(), pj_mkparam("+t_final=later"));
  ASSERT_NE(nullptr, bad_param.get());
  ScopedPj invalid(pj_new());
  ASSERT_NE(nullptr, invalid.get());
  invalid->ctx = ctx.get();
  invalid->params = bad_param.release();
  EXPECT_DOUBLE_EQ(0.0, pj_parse_t_final(invalid.get()));

  // 4. Bare +t_final flag (empty value) returns 0.0.
  ScopedParalist bare_param(ctx.get(), pj_mkparam("+t_final"));
  ASSERT_NE(nullptr, bare_param.get());
  ScopedPj bare(pj_new());
  ASSERT_NE(nullptr, bare.get());
  bare->ctx = ctx.get();
  bare->params = bare_param.release();
  EXPECT_DOUBLE_EQ(0.0, pj_parse_t_final(bare.get()));

  // 5. +t_final=now returns current local year + day-of-year fraction.
  ScopedParalist now_param(ctx.get(), pj_mkparam("+t_final=now"));
  ASSERT_NE(nullptr, now_param.get());
  ScopedPj current(pj_new());
  ASSERT_NE(nullptr, current.get());
  current->ctx = ctx.get();
  current->params = now_param.release();

  const std::time_t before = std::time(nullptr);
  const double parsed_now = pj_parse_t_final(current.get());
  const std::time_t after = std::time(nullptr);
  const double min_expected = ComputeExpectedYearFraction(before);
  const double max_expected = ComputeExpectedYearFraction(after);
  EXPECT_GE(parsed_now, min_expected - 1e-9);
  EXPECT_LE(parsed_now, max_expected + 1e-9);
}

}  // namespace
}  // namespace proj

// Coverage notes (proj/src/param.cpp):
// - Unreachable OOM allocation checks after malloc/calloc in pj_mkparam (line
//   17-24 false branch) and pj_mkparam_ws (line 62-63) are omitted.
// - Null guard on t_final_str (line 233 `!t_final_str`) cannot be triggered
//   when `tt_final` succeeded, because `"s" + key` returns non-null.
