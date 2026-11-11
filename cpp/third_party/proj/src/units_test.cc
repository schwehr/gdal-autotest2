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
// Unit tests for built-in linear and angular unit tables (proj/src/units.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/units.cpp

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <set>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

double ParseRatioOrScalar(std::string_view expr) {
  const size_t slash = expr.find('/');
  if (slash == std::string_view::npos) {
    return pj_atof(std::string(expr).c_str());  // NOLINT(runtime/deprecated_fn)
  }
  const std::string num_str(expr.substr(0, slash));
  const std::string den_str(expr.substr(slash + 1));
  // NOLINTNEXTLINE(runtime/deprecated_fn)
  return pj_atof(num_str.c_str()) / pj_atof(den_str.c_str());
}

TEST(UnitsTest, LinearUnitAccessorsMatchAndHaveConsistentFactors) {
  const PJ_UNITS* internal_units = pj_list_linear_units();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  const PJ_UNITS* public_units = proj_list_units();
#pragma GCC diagnostic pop
  ASSERT_NE(nullptr, internal_units);
  EXPECT_EQ(internal_units, public_units);

  std::set<std::string_view> seen_ids;
  const PJ_UNITS* cur = internal_units;
  for (; cur->id != nullptr; ++cur) {
    ASSERT_NE(nullptr, cur->to_meter);
    ASSERT_NE(nullptr, cur->name);
    const std::string_view id(cur->id);
    EXPECT_FALSE(id.empty());
    EXPECT_TRUE(seen_ids.insert(id).second) << "Duplicate unit id: " << id;
    EXPECT_FALSE(std::string_view(cur->to_meter).empty()) << id;
    EXPECT_FALSE(std::string_view(cur->name).empty()) << id;
    EXPECT_GT(cur->factor, 0.0) << id;
    EXPECT_TRUE(std::isfinite(cur->factor)) << id;

    const double parsed = ParseRatioOrScalar(cur->to_meter);
    EXPECT_NEAR(parsed, cur->factor, 1e-12 * std::max(1.0, cur->factor)) << id;
  }

  // Sentinel terminator must have null strings and zero factor.
  EXPECT_EQ(nullptr, cur->id);
  EXPECT_EQ(nullptr, cur->to_meter);
  EXPECT_EQ(nullptr, cur->name);
  EXPECT_DOUBLE_EQ(0.0, cur->factor);

  EXPECT_EQ(21u, seen_ids.size());
  EXPECT_STREQ("km", internal_units[0].id);
  EXPECT_DOUBLE_EQ(1000.0, internal_units[0].factor);
  EXPECT_STREQ("m", internal_units[1].id);
  EXPECT_DOUBLE_EQ(1.0, internal_units[1].factor);
  EXPECT_STREQ("us-ft", internal_units[14].id);
  EXPECT_NEAR(1200.0 / 3937.0, internal_units[14].factor, 1e-15);
}

TEST(UnitsTest, AngularUnitAccessorsMatchAndHaveExactRadDegGradFactors) {
  const PJ_UNITS* internal_ang = pj_list_angular_units();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  const PJ_UNITS* public_ang = proj_list_angular_units();
#pragma GCC diagnostic pop
  ASSERT_NE(nullptr, internal_ang);
  EXPECT_EQ(internal_ang, public_ang);

  EXPECT_STREQ("rad", internal_ang[0].id);
  EXPECT_STREQ("1.0", internal_ang[0].to_meter);
  EXPECT_STREQ("Radian", internal_ang[0].name);
  EXPECT_DOUBLE_EQ(1.0, internal_ang[0].factor);

  EXPECT_STREQ("deg", internal_ang[1].id);
  EXPECT_STREQ("Degree", internal_ang[1].name);
  EXPECT_NEAR(std::numbers::pi / 180.0, internal_ang[1].factor, 1e-15);

  EXPECT_STREQ("grad", internal_ang[2].id);
  EXPECT_STREQ("Grad", internal_ang[2].name);
  EXPECT_NEAR(std::numbers::pi / 200.0, internal_ang[2].factor, 1e-15);

  EXPECT_EQ(nullptr, internal_ang[3].id);
  EXPECT_EQ(nullptr, internal_ang[3].to_meter);
  EXPECT_EQ(nullptr, internal_ang[3].name);
  EXPECT_DOUBLE_EQ(0.0, internal_ang[3].factor);
}

TEST(UnitsTest, LinearAndAngularUnitsApplyInProjOperations) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Linear unit scaling via +units=km vs +units=m.
  ScopedPj merc_m(proj_create(ctx.get(), "+proj=merc +R=6378137 +units=m"));
  ScopedPj merc_km(proj_create(ctx.get(), "+proj=merc +R=6378137 +units=km"));
  ASSERT_NE(nullptr, merc_m.get());
  ASSERT_NE(nullptr, merc_km.get());
  EXPECT_DOUBLE_EQ(1000.0, merc_km->to_meter);

  const PJ_COORD pt = proj_coord(proj_torad(10.0), proj_torad(20.0), 0.0, 0.0);
  const PJ_COORD out_m = proj_trans(merc_m.get(), PJ_FWD, pt);
  const PJ_COORD out_km = proj_trans(merc_km.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(merc_m.get()));
  EXPECT_EQ(0, proj_errno(merc_km.get()));
  EXPECT_NEAR(out_m.xy.x / 1000.0, out_km.xy.x, 1e-9);
  EXPECT_NEAR(out_m.xy.y / 1000.0, out_km.xy.y, 1e-9);

  // 2. Angular unit conversion via unitconvert (deg -> grad).
  ScopedPj deg_to_grad(
      proj_create(ctx.get(), "+proj=unitconvert +xy_in=deg +xy_out=grad"));
  ASSERT_NE(nullptr, deg_to_grad.get());
  const PJ_COORD grad_out =
      proj_trans(deg_to_grad.get(), PJ_FWD, proj_coord(90.0, 45.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(deg_to_grad.get()));
  EXPECT_NEAR(100.0, grad_out.xy.x, 1e-12);
  EXPECT_NEAR(50.0, grad_out.xy.y, 1e-12);

  // 3. Unknown unit keyword is rejected cleanly.
  ScopedPj bad_unit(
      proj_create(ctx.get(), "+proj=merc +R=6378137 +units=no_such_unit"));
  EXPECT_EQ(nullptr, bad_unit.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

}  // namespace
}  // namespace proj
