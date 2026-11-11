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
// Unit tests for PROJ internal plumbing helpers: I/O unit queries, 2D/3D
// approximate transform dispatch, context assignment/inheritance, string
// trimming/quoting, and context errno setters (proj/src/internal.cpp).
// Note: in google3 default non-GCE builds, proj.db is embedded in memory and
// network access is disabled; null-context tests restore pj_get_default_ctx()
// and thread-local errno on scope exit.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/internal.cpp

#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <numbers>
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
constexpr double kSphereRadiusM = 6378137.0;

constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }

TEST(InternalTest, LeftAndRightIoUnitsMapClassicAndHonorInverted) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Classic projected CRS reports RADIANS left and PROJECTED right.
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, merc.get());
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_left(merc.get()));
  EXPECT_EQ(PJ_IO_UNITS_PROJECTED, pj_right(merc.get()));

  // Inverting swaps left and right while still mapping CLASSIC -> PROJECTED.
  merc->inverted = 1;
  EXPECT_EQ(PJ_IO_UNITS_PROJECTED, pj_left(merc.get()));
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_right(merc.get()));

  // 2. Cartesian conversion preserves CARTESIAN vs RADIANS across inversion.
  ScopedPj cart(proj_create(ctx.get(), "+proj=cart +ellps=WGS84"));
  ASSERT_NE(nullptr, cart.get());
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_left(cart.get()));
  EXPECT_EQ(PJ_IO_UNITS_CARTESIAN, pj_right(cart.get()));
  cart->inverted = 1;
  EXPECT_EQ(PJ_IO_UNITS_CARTESIAN, pj_left(cart.get()));
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_right(cart.get()));

  // 3. No-op and angular unitconvert report WHATEVER and DEGREES/RADIANS.
  ScopedPj noop(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, noop.get());
  EXPECT_EQ(PJ_IO_UNITS_WHATEVER, pj_left(noop.get()));
  EXPECT_EQ(PJ_IO_UNITS_WHATEVER, pj_right(noop.get()));

  ScopedPj deg2rad(
      proj_create(ctx.get(), "+proj=unitconvert +xy_in=deg +xy_out=rad"));
  ASSERT_NE(nullptr, deg2rad.get());
  EXPECT_EQ(PJ_IO_UNITS_DEGREES, pj_left(deg2rad.get()));
  EXPECT_EQ(PJ_IO_UNITS_RADIANS, pj_right(deg2rad.get()));
}

TEST(InternalTest, ProjCoordErrorSetsAllFourComponentsToHugeVal) {
  const PJ_COORD err = proj_coord_error();
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(HUGE_VAL, err.v[i]) << "component=" << i;
    EXPECT_TRUE(std::isinf(err.v[i])) << "component=" << i;
  }
  EXPECT_EQ(HUGE_VAL, err.xyzt.x);
  EXPECT_EQ(HUGE_VAL, err.xyzt.y);
  EXPECT_EQ(HUGE_VAL, err.xyzt.z);
  EXPECT_EQ(HUGE_VAL, err.xyzt.t);
}

TEST(InternalTest, Approx2DTransHandlesNullFwdInvIdentAndInverted) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  const PJ_COORD input =
      proj_coord(DegToRad(12.0), DegToRad(45.0), 321.5, 2024.25);

  // 1. Null PJ returns the input coordinate untouched.
  const PJ_COORD null_res = pj_approx_2D_trans(nullptr, PJ_FWD, input);
  for (int i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(input.v[i], null_res.v[i]);
  }

  // 2. Normal PJ: PJ_IDENT leaves input untouched; PJ_FWD/PJ_INV round-trip
  //    2D coordinates while leaving z and t unmodified.
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +R=6378137"));
  ASSERT_NE(nullptr, merc.get());

  const PJ_COORD ident_res = pj_approx_2D_trans(merc.get(), PJ_IDENT, input);
  EXPECT_EQ(0, proj_errno(merc.get()));
  for (int i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(input.v[i], ident_res.v[i]);
  }

  const PJ_COORD fwd = pj_approx_2D_trans(merc.get(), PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(merc.get()));
  const double expected_x = kSphereRadiusM * DegToRad(12.0);
  const double expected_y =
      kSphereRadiusM * std::log(std::tan(kPi / 4.0 + DegToRad(45.0) / 2.0));
  EXPECT_NEAR(expected_x, fwd.xy.x, 1e-6);
  EXPECT_NEAR(expected_y, fwd.xy.y, 1e-6);
  EXPECT_DOUBLE_EQ(input.xyzt.z, fwd.xyzt.z);
  EXPECT_DOUBLE_EQ(input.xyzt.t, fwd.xyzt.t);

  const PJ_COORD inv = pj_approx_2D_trans(merc.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_NEAR(input.lp.lam, inv.lp.lam, 1e-12);
  EXPECT_NEAR(input.lp.phi, inv.lp.phi, 1e-12);
  EXPECT_DOUBLE_EQ(input.xyzt.z, inv.xyzt.z);
  EXPECT_DOUBLE_EQ(input.xyzt.t, inv.xyzt.t);

  // 3. Inverted PJ flips PJ_FWD <-> PJ_INV while preserving PJ_IDENT.
  merc->inverted = 1;
  const PJ_COORD inv_via_fwd = pj_approx_2D_trans(merc.get(), PJ_FWD, fwd);
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_NEAR(input.lp.lam, inv_via_fwd.lp.lam, 1e-12);
  EXPECT_NEAR(input.lp.phi, inv_via_fwd.lp.phi, 1e-12);

  const PJ_COORD fwd_via_inv = pj_approx_2D_trans(merc.get(), PJ_INV, input);
  EXPECT_EQ(0, proj_errno(merc.get()));
  EXPECT_NEAR(expected_x, fwd_via_inv.xy.x, 1e-6);
  EXPECT_NEAR(expected_y, fwd_via_inv.xy.y, 1e-6);

  const PJ_COORD inverted_ident =
      pj_approx_2D_trans(merc.get(), PJ_IDENT, input);
  for (int i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(input.v[i], inverted_ident.v[i]);
  }

  // 4. Forward-only projection reports HUGE_VAL + PROJ_ERR_OTHER_NO_INVERSE_OP,
  //    and invalid latitude reports HUGE_VAL +
  //    PROJ_ERR_COORD_TRANSFM_INVALID_COORD.
  ScopedPj airy(proj_create(ctx.get(), "+proj=airy +R=6371000"));
  ASSERT_NE(nullptr, airy.get());
  const PJ_COORD no_inv = pj_approx_2D_trans(airy.get(), PJ_INV, input);
  EXPECT_EQ(HUGE_VAL, no_inv.xyzt.x);
  EXPECT_TRUE(std::isinf(no_inv.xyzt.x));
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(airy.get()));
  proj_errno_reset(airy.get());

  merc->inverted = 0;
  const PJ_COORD bad_lat =
      pj_approx_2D_trans(merc.get(), PJ_FWD, proj_coord(0.0, kPi, 0.0, 0.0));
  EXPECT_EQ(HUGE_VAL, bad_lat.xyzt.x);
  EXPECT_TRUE(std::isinf(bad_lat.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_INVALID_COORD, proj_errno(merc.get()));
  proj_errno_reset(merc.get());
}

TEST(InternalTest, Approx3DTransHandlesNullFwdInvIdentAndInverted) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  const PJ_COORD input =
      proj_coord(DegToRad(12.0), DegToRad(55.0), 432.1, 2025.5);

  // 1. Null PJ returns the input coordinate untouched.
  const PJ_COORD null_res = pj_approx_3D_trans(nullptr, PJ_FWD, input);
  for (int i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(input.v[i], null_res.v[i]);
  }

  // 2. 3D geocentric conversion transforms (lon, lat, h) <-> (X, Y, Z) while
  //    leaving t unmodified.
  ScopedPj cart(proj_create(ctx.get(), "+proj=cart +ellps=WGS84"));
  ASSERT_NE(nullptr, cart.get());

  const PJ_COORD ident_res = pj_approx_3D_trans(cart.get(), PJ_IDENT, input);
  EXPECT_EQ(0, proj_errno(cart.get()));
  for (int i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(input.v[i], ident_res.v[i]);
  }

  const PJ_COORD fwd = pj_approx_3D_trans(cart.get(), PJ_FWD, input);
  EXPECT_EQ(0, proj_errno(cart.get()));
  const PJ_XYZ ref_xyz = pj_fwd3d(input.lpz, cart.get());
  EXPECT_NEAR(ref_xyz.x, fwd.xyz.x, 1e-6);
  EXPECT_NEAR(ref_xyz.y, fwd.xyz.y, 1e-6);
  EXPECT_NEAR(ref_xyz.z, fwd.xyz.z, 1e-6);
  EXPECT_DOUBLE_EQ(input.xyzt.t, fwd.xyzt.t);

  const PJ_COORD inv = pj_approx_3D_trans(cart.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(cart.get()));
  EXPECT_NEAR(input.lpz.lam, inv.lpz.lam, 1e-12);
  EXPECT_NEAR(input.lpz.phi, inv.lpz.phi, 1e-12);
  EXPECT_NEAR(input.lpz.z, inv.lpz.z, 1e-4);
  EXPECT_DOUBLE_EQ(input.xyzt.t, inv.xyzt.t);

  // 3. Inverted PJ flips PJ_FWD <-> PJ_INV while preserving PJ_IDENT.
  cart->inverted = 1;
  const PJ_COORD inv_via_fwd = pj_approx_3D_trans(cart.get(), PJ_FWD, fwd);
  EXPECT_EQ(0, proj_errno(cart.get()));
  EXPECT_NEAR(input.lpz.lam, inv_via_fwd.lpz.lam, 1e-12);
  EXPECT_NEAR(input.lpz.phi, inv_via_fwd.lpz.phi, 1e-12);
  EXPECT_NEAR(input.lpz.z, inv_via_fwd.lpz.z, 1e-4);

  const PJ_COORD fwd_via_inv = pj_approx_3D_trans(cart.get(), PJ_INV, input);
  EXPECT_EQ(0, proj_errno(cart.get()));
  EXPECT_NEAR(ref_xyz.x, fwd_via_inv.xyz.x, 1e-6);
  EXPECT_NEAR(ref_xyz.y, fwd_via_inv.xyz.y, 1e-6);
  EXPECT_NEAR(ref_xyz.z, fwd_via_inv.xyz.z, 1e-6);

  const PJ_COORD inverted_ident =
      pj_approx_3D_trans(cart.get(), PJ_IDENT, input);
  for (int i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(input.v[i], inverted_ident.v[i]);
  }

  // 4. Forward-only projection reports HUGE_VAL + PROJ_ERR_OTHER_NO_INVERSE_OP.
  ScopedPj airy(proj_create(ctx.get(), "+proj=airy +R=6371000"));
  ASSERT_NE(nullptr, airy.get());
  const PJ_COORD no_inv = pj_approx_3D_trans(airy.get(), PJ_INV, input);
  EXPECT_EQ(HUGE_VAL, no_inv.xyzt.x);
  EXPECT_TRUE(std::isinf(no_inv.xyzt.x));
  EXPECT_EQ(PROJ_ERR_OTHER_NO_INVERSE_OP, proj_errno(airy.get()));
  proj_errno_reset(airy.get());
}

TEST(InternalTest, HasInverseChecksInvertedAndAllDimensionCallbacks) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. 2D invertible projection (inv != nullptr).
  ScopedPj merc(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, merc.get());
  ASSERT_NE(nullptr, merc->inv);
  EXPECT_EQ(1, pj_has_inverse(merc.get()));

  // 2. 3D-only conversion (inv == nullptr, inv3d != nullptr; fwd3d != nullptr).
  ScopedPj swap3d(proj_create(ctx.get(), "+proj=axisswap +order=3,2,1"));
  ASSERT_NE(nullptr, swap3d.get());
  ASSERT_EQ(nullptr, swap3d->inv);
  ASSERT_NE(nullptr, swap3d->inv3d);
  ASSERT_EQ(nullptr, swap3d->fwd);
  ASSERT_NE(nullptr, swap3d->fwd3d);
  EXPECT_EQ(1, pj_has_inverse(swap3d.get()));
  swap3d->inverted = 1;
  EXPECT_EQ(1, pj_has_inverse(swap3d.get()));

  // 3. 4D-only conversion (inv == inv3d == nullptr, inv4d != nullptr).
  ScopedPj noop(proj_create(ctx.get(), "+proj=noop"));
  ASSERT_NE(nullptr, noop.get());
  ASSERT_EQ(nullptr, noop->inv);
  ASSERT_EQ(nullptr, noop->inv3d);
  ASSERT_NE(nullptr, noop->inv4d);
  ASSERT_EQ(nullptr, noop->fwd);
  ASSERT_EQ(nullptr, noop->fwd3d);
  ASSERT_NE(nullptr, noop->fwd4d);
  EXPECT_EQ(1, pj_has_inverse(noop.get()));
  noop->inverted = 1;
  EXPECT_EQ(1, pj_has_inverse(noop.get()));

  // 4. Forward-only projection has no inverse until inverted.
  ScopedPj airy(proj_create(ctx.get(), "+proj=airy +R=6371000"));
  ASSERT_NE(nullptr, airy.get());
  ASSERT_NE(nullptr, airy->fwd);
  ASSERT_EQ(nullptr, airy->inv);
  EXPECT_EQ(0, pj_has_inverse(airy.get()));
  airy->inverted = 1;
  EXPECT_EQ(1, pj_has_inverse(airy.get()));

  // 5. Bare PJ with no forward or inverse callbacks reports 0 even if inverted.
  ScopedPj empty(pj_new());
  ASSERT_NE(nullptr, empty.get());
  empty->ctx = ctx.get();
  EXPECT_EQ(0, pj_has_inverse(empty.get()));
  empty->inverted = 1;
  EXPECT_EQ(0, pj_has_inverse(empty.get()));
}

TEST(InternalTest, ContextSetAndInheritAssignNonNullAndDefaultContexts) {
  ScopedContext ctx_a = MakeContext();
  ScopedContext ctx_b = MakeContext();
  ASSERT_NE(nullptr, ctx_a.get());
  ASSERT_NE(nullptr, ctx_b.get());

  ScopedPj parent(proj_create(ctx_a.get(), "+proj=merc +ellps=WGS84"));
  ScopedPj child(proj_create(ctx_a.get(), "+proj=cart +ellps=WGS84"));
  ASSERT_NE(nullptr, parent.get());
  ASSERT_NE(nullptr, child.get());

  absl::Cleanup restore_child_ctx = [&] {
    proj_context_set(child.get(), ctx_a.get());
  };

  // 1. Explicit non-null context assignment.
  proj_context_set(child.get(), ctx_b.get());
  EXPECT_EQ(ctx_b.get(), pj_get_ctx(child.get()));

  // 2. Null context falls back to the process default context.
  proj_context_set(child.get(), nullptr);
  EXPECT_EQ(pj_get_default_ctx(), pj_get_ctx(child.get()));

  // 3. Inheriting from parent restores parent's context.
  proj_context_inherit(parent.get(), child.get());
  EXPECT_EQ(ctx_a.get(), pj_get_ctx(child.get()));

  // 4. Inheriting from nullptr falls back to the process default context.
  proj_context_inherit(nullptr, child.get());
  EXPECT_EQ(pj_get_default_ctx(), pj_get_ctx(child.get()));
}

TEST(InternalTest, ChompStripsCommentsPrefixPostfixWhitespaceAndSemicolons) {
  EXPECT_EQ(nullptr, pj_chomp(nullptr));

  struct ChompCase {
    std::string_view input;
    const char* expected;
  };
  constexpr ChompCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = "# full-line comment", .expected = ""},
      {.input = "   ", .expected = ""},
      {.input = " ", .expected = ""},
      {.input = ";", .expected = ""},
      {.input = ";;;\t\n ", .expected = ""},
      {.input = "proj=merc", .expected = "proj=merc"},
      {.input = "  \t; +proj=utm +zone=32 ;\n\t # utm zone 32",
       .expected = "+proj=utm +zone=32"},
      {.input = "a  b#inline comment", .expected = "a  b"},
      {.input = " ; a ; # ; ", .expected = "a"},
  };

  for (const auto& tc : kCases) {
    std::string buf(tc.input);
    char* res = pj_chomp(buf.data());
    EXPECT_EQ(buf.data(), res) << tc.input;
    EXPECT_STREQ(tc.expected, res) << tc.input;
  }
}

TEST(InternalTest, ShrinkCollapsesWhitespaceStripsPlusAndHandlesQuotes) {
  EXPECT_EQ(nullptr, pj_shrink(nullptr));

  struct ShrinkCase {
    std::string_view input;
    const char* expected;
  };
  constexpr ShrinkCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = "  ;  # only comment", .expected = ""},
      {.input = "+proj=utm +zone=32", .expected = "proj=utm zone=32"},
      {.input = "  +proj = utm ; +zone = 32  +towgs84 = 1 , 2 , 3 ; ",
       .expected = "proj=utm zone=32 towgs84=1,2,3"},
      {.input = "+a = 1.23e+08 +b = 4.5e-02",
       .expected = "a=1.23e+08 b=4.5e-02"},
      {.input = "+title=\"  a   \"\"b\"\"  c  \" +proj=merc",
       .expected = "title=\"  a   \"\"b\"\"  c  \" proj=merc"},
      {.input = "+title=\"a +b; c  d\" +over",
       .expected = "title=\"a +b; c  d\" over"},
      {.input = "= value , 2", .expected = "=value,2"},
      {.input = "foo=bar", .expected = "foo=bar"},
      {.input = "a,, b", .expected = "a,,b"},
  };

  for (const auto& tc : kCases) {
    std::string buf(tc.input);
    char* res = pj_shrink(buf.data());
    EXPECT_EQ(buf.data(), res) << tc.input;
    EXPECT_STREQ(tc.expected, res) << tc.input;
  }
}

TEST(InternalTest, TrimArgcAndArgvSplitAndUnquoteMutableBuffer) {
  // 1. Guard branches on pj_trim_argv and empty pj_trim_argc inputs.
  EXPECT_EQ(nullptr, pj_trim_argv(2, nullptr));
  std::string dummy = "proj=merc";
  EXPECT_EQ(nullptr, pj_trim_argv(0, dummy.data()));

  std::string empty_buf = "  ;  # comment only";
  EXPECT_EQ(0u, pj_trim_argc(empty_buf.data()));

  // 2. Typical multi-parameter definition with quoted spaces and "" escapes.
  std::string buf =
      " +proj = utm ; +zone = 32 +title=\"Zone \"\"32N\"\"  area\" "
      "+south # trailing comment";
  const size_t argc = pj_trim_argc(buf.data());
  ASSERT_EQ(4u, argc);
  ScopedMalloc<char*> argv(pj_trim_argv(argc, buf.data()));
  ASSERT_NE(nullptr, argv.get());
  EXPECT_STREQ("proj=utm", argv.get()[0]);
  EXPECT_STREQ("zone=32", argv.get()[1]);
  EXPECT_STREQ("title=Zone \"32N\"  area", argv.get()[2]);
  EXPECT_STREQ("south", argv.get()[3]);

  // 3. Empty quoted value unquotes cleanly to empty parameter value.
  std::string empty_quote_buf = "+title=\"\" +proj=merc";
  const size_t eq_argc = pj_trim_argc(empty_quote_buf.data());
  ASSERT_EQ(2u, eq_argc);
  ScopedMalloc<char*> eq_argv(pj_trim_argv(eq_argc, empty_quote_buf.data()));
  ASSERT_NE(nullptr, eq_argv.get());
  EXPECT_STREQ("title=", eq_argv.get()[0]);
  EXPECT_STREQ("proj=merc", eq_argv.get()[1]);

  // 4. Unterminated quote and leading "=\"..." fail unquote preconditions and
  //    are left verbatim.
  std::string unterminated = "title=\"abc";
  ASSERT_EQ(1u, pj_trim_argc(unterminated.data()));
  ScopedMalloc<char*> u_argv(pj_trim_argv(1, unterminated.data()));
  ASSERT_NE(nullptr, u_argv.get());
  EXPECT_STREQ("title=\"abc", u_argv.get()[0]);

  std::string no_key = "=\"abc\"";
  ASSERT_EQ(1u, pj_trim_argc(no_key.data()));
  ScopedMalloc<char*> nk_argv(pj_trim_argv(1, no_key.data()));
  ASSERT_NE(nullptr, nk_argv.get());
  EXPECT_STREQ("=\"abc\"", nk_argv.get()[0]);
}

TEST(InternalTest, DoubleQuoteStringParamAndMakeArgsRoundTrip) {
  // 1. pj_double_quote_string_param_if_needed quotes only when spaces appear.
  EXPECT_EQ("", pj_double_quote_string_param_if_needed(""));
  EXPECT_EQ("grid.tif", pj_double_quote_string_param_if_needed("grid.tif"));
  EXPECT_EQ("a\"b", pj_double_quote_string_param_if_needed("a\"b"));
  EXPECT_EQ("\"a b\"", pj_double_quote_string_param_if_needed("a b"));
  EXPECT_EQ("\"a \"\"b\"\" c\"",
            pj_double_quote_string_param_if_needed("a \"b\" c"));

  // 2. pj_make_args with argc == 0 returns an empty heap-allocated string.
  ScopedMalloc<char> empty_args(pj_make_args(0, nullptr));
  ASSERT_NE(nullptr, empty_args.get());
  EXPECT_STREQ("", empty_args.get());

  // 3. Reconstruct a PROJ string from mutable argv entries and instantiate it.
  std::array<std::string, 5> storage = {
      "+proj=utm", "zone=32", "+title=Zone \"32N\" map", "ellps=WGS84", "south",
  };
  std::array<char*, 5> argv = {
      storage[0].data(), storage[1].data(), storage[2].data(),
      storage[3].data(), storage[4].data(),
  };
  ScopedMalloc<char> made(pj_make_args(argv.size(), argv.data()));
  ASSERT_NE(nullptr, made.get());
  EXPECT_STREQ(
      "proj=utm zone=32 title=\"Zone \"\"32N\"\" map\" ellps=WGS84 south",
      made.get());

  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj from_str(proj_create(ctx.get(), made.get()));
  ASSERT_NE(nullptr, from_str.get());
  EXPECT_STREQ("utm", proj_pj_info(from_str.get()).id);

  ScopedPj from_argv(
      proj_create_argv(ctx.get(), static_cast<int>(argv.size()), argv.data()));
  ASSERT_NE(nullptr, from_argv.get());
  EXPECT_STREQ("utm", proj_pj_info(from_argv.get()).id);
}

TEST(InternalTest, ContextErrnoSetUpdatesContextAndErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  const int saved_errno = errno;
  errno = 0;
  absl::Cleanup restore_state = [saved_errno] {
    proj_context_errno_set(nullptr, 0);
    errno = saved_errno;
  };

  // 1. Non-zero error sets both context last_errno and thread-local errno.
  proj_context_errno_set(ctx.get(), PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_context_errno(ctx.get()));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, errno);

  // 2. Zero clears context last_errno without overwriting thread-local errno.
  proj_context_errno_set(ctx.get(), 0);
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, errno);

  // 3. Null context falls back to the process default context.
  errno = 0;
  proj_context_errno_set(nullptr, PROJ_ERR_INVALID_OP_WRONG_SYNTAX);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, proj_context_errno(nullptr));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_WRONG_SYNTAX, errno);
}

}  // namespace
}  // namespace proj
