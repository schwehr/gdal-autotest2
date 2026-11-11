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
// Unit tests for built-in PROJ ellipsoid definitions (proj/src/ellps.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/ellps.cpp

#include <set>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

TEST(EllpsTest, ListEllpsReturnsExpectedTableAndSentinel) {
  const PJ_ELLPS* ellps = proj_list_ellps();
  ASSERT_NE(nullptr, ellps);

  std::set<std::string_view> seen_ids;
  const PJ_ELLPS* cur = ellps;
  for (; cur->id != nullptr; ++cur) {
    ASSERT_NE(nullptr, cur->major);
    ASSERT_NE(nullptr, cur->ell);
    ASSERT_NE(nullptr, cur->name);

    const std::string_view id(cur->id);
    const std::string_view major(cur->major);
    const std::string_view ell(cur->ell);
    const std::string_view name(cur->name);

    EXPECT_FALSE(id.empty());
    EXPECT_TRUE(seen_ids.insert(id).second) << "Duplicate ellipsoid id: " << id;
    EXPECT_TRUE(major.starts_with("a=")) << id << " major=" << major;
    EXPECT_TRUE(ell.starts_with("rf=") || ell.starts_with("b="))
        << id << " ell=" << ell;
    EXPECT_FALSE(name.empty()) << id;
  }

  // Sentinel terminator must have all fields null.
  EXPECT_EQ(nullptr, cur->id);
  EXPECT_EQ(nullptr, cur->major);
  EXPECT_EQ(nullptr, cur->ell);
  EXPECT_EQ(nullptr, cur->name);

  EXPECT_EQ(46u, seen_ids.size());
  EXPECT_TRUE(seen_ids.contains("WGS84"));
  EXPECT_TRUE(seen_ids.contains("GRS80"));
  EXPECT_TRUE(seen_ids.contains("clrk66"));
  EXPECT_TRUE(seen_ids.contains("sphere"));

  // Spot-check first and final entries.
  EXPECT_STREQ("MERIT", ellps[0].id);
  EXPECT_STREQ("a=6378137.0", ellps[0].major);
  EXPECT_STREQ("rf=298.257", ellps[0].ell);

  EXPECT_STREQ("sphere", ellps[45].id);
  EXPECT_STREQ("a=6370997.0", ellps[45].major);
  EXPECT_STREQ("b=6370997.0", ellps[45].ell);
}

TEST(EllpsTest, AllBuiltInEllipsoidsInstantiateAndUnknownEllipsoidFails) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  const PJ_ELLPS* ellps = proj_list_ellps();
  ASSERT_NE(nullptr, ellps);

  for (const PJ_ELLPS* cur = ellps; cur->id != nullptr; ++cur) {
    const std::string def = "+proj=merc +ellps=" + std::string(cur->id);
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get()) << def;
    EXPECT_EQ(0, proj_errno(pj.get())) << def;

    // NOLINTNEXTLINE(runtime/deprecated_fn)
    const double expected_a = pj_atof(cur->major + 2);  // Skip "a=".
    EXPECT_NEAR(expected_a, pj->a, 1e-6) << cur->id;
    if (std::string_view(cur->id) == "sphere") {
      EXPECT_DOUBLE_EQ(0.0, pj->es);
    } else {
      EXPECT_GT(pj->es, 0.0) << cur->id;
    }
  }

  ScopedPj bad(proj_create(ctx.get(), "+proj=merc +ellps=no_such_ellps"));
  EXPECT_EQ(nullptr, bad.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

}  // namespace
}  // namespace proj
