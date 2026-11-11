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
// Unit tests for built-in PROJ datum and prime meridian tables
// (proj/src/datums.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/datums.cpp

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

TEST(DatumsTest, GetDatumsRefReturnsValidTableAndEllipsoidsExist) {
  const PJ_DATUMS* datums = pj_get_datums_ref();
  ASSERT_NE(nullptr, datums);

  // Collect all valid built-in ellipsoid IDs from ellps.cpp for cross-check.
  std::set<std::string_view> valid_ellps;
  const PJ_ELLPS* ellps = proj_list_ellps();
  ASSERT_NE(nullptr, ellps);
  for (const PJ_ELLPS* e = ellps; e->id != nullptr; ++e) {
    valid_ellps.insert(e->id);
  }

  std::vector<std::string_view> ids;
  const PJ_DATUMS* cur = datums;
  for (; cur->id != nullptr; ++cur) {
    ASSERT_NE(nullptr, cur->defn);
    ASSERT_NE(nullptr, cur->ellipse_id);
    ASSERT_NE(nullptr, cur->comments);
    EXPECT_FALSE(std::string_view(cur->id).empty());
    EXPECT_FALSE(std::string_view(cur->defn).empty());
    EXPECT_TRUE(valid_ellps.contains(cur->ellipse_id))
        << "Unknown ellipse_id " << cur->ellipse_id << " for datum " << cur->id;
    ids.emplace_back(cur->id);
  }

  // Sentinel entry must have all fields null.
  EXPECT_EQ(nullptr, cur->id);
  EXPECT_EQ(nullptr, cur->defn);
  EXPECT_EQ(nullptr, cur->ellipse_id);
  EXPECT_EQ(nullptr, cur->comments);

  EXPECT_EQ(10u, ids.size());
  EXPECT_STREQ("WGS84", datums[0].id);
  EXPECT_STREQ("towgs84=0,0,0", datums[0].defn);
  EXPECT_STREQ("WGS84", datums[0].ellipse_id);
  EXPECT_STREQ("", datums[0].comments);

  EXPECT_STREQ("NAD27", datums[3].id);
  EXPECT_STREQ("clrk66", datums[3].ellipse_id);

  EXPECT_STREQ("OSGB36", datums[9].id);
  EXPECT_STREQ("airy", datums[9].ellipse_id);
}

TEST(DatumsTest, Towgs84DatumsInstantiateAndUnknownDatumFails) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  struct DatumCase {
    std::string_view id;
    int expected_type;
  };
  constexpr DatumCase kCases[] = {
      {.id = "WGS84", .expected_type = PJD_WGS84},
      {.id = "GGRS87", .expected_type = PJD_3PARAM},
      {.id = "NAD83", .expected_type = PJD_WGS84},
      {.id = "carthage", .expected_type = PJD_3PARAM},
      {.id = "hermannskogel", .expected_type = PJD_7PARAM},
      {.id = "ire65", .expected_type = PJD_7PARAM},
      {.id = "nzgd49", .expected_type = PJD_7PARAM},
      {.id = "OSGB36", .expected_type = PJD_7PARAM},
  };

  for (const auto& tc : kCases) {
    const std::string def = "+proj=longlat +datum=" + std::string(tc.id);
    ScopedPj pj(proj_create(ctx.get(), def.c_str()));
    ASSERT_NE(nullptr, pj.get()) << def;
    EXPECT_EQ(0, proj_errno(pj.get())) << def;
    EXPECT_EQ(tc.expected_type, pj->datum_type) << def;
  }

  ScopedPj bad(proj_create(ctx.get(), "+proj=longlat +datum=no_such_datum"));
  EXPECT_EQ(nullptr, bad.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

TEST(DatumsTest, ListPrimeMeridiansReturnsValidDmsOffsetsAndSentinel) {
  const PJ_PRIME_MERIDIANS* pms = proj_list_prime_meridians();
  ASSERT_NE(nullptr, pms);

  std::vector<std::string_view> ids;
  const PJ_PRIME_MERIDIANS* cur = pms;
  for (; cur->id != nullptr; ++cur) {
    ASSERT_NE(nullptr, cur->defn);
    EXPECT_FALSE(std::string_view(cur->id).empty());
    char* endptr = nullptr;
    const double rad = proj_dmstor(cur->defn, &endptr);
    ASSERT_NE(nullptr, endptr) << cur->id;
    EXPECT_EQ('\0', *endptr) << cur->id << " defn=" << cur->defn;
    if (std::string_view(cur->id) == "greenwich") {
      EXPECT_DOUBLE_EQ(0.0, rad);
    } else {
      EXPECT_NE(0.0, rad) << cur->id;
    }
    ids.emplace_back(cur->id);
  }

  EXPECT_EQ(nullptr, cur->id);
  EXPECT_EQ(nullptr, cur->defn);
  EXPECT_EQ(14u, ids.size());
  EXPECT_STREQ("greenwich", pms[0].id);
  EXPECT_STREQ("0dE", pms[0].defn);
  EXPECT_STREQ("paris", pms[2].id);
  EXPECT_STREQ("2d20'14.025\"E", pms[2].defn);
  EXPECT_STREQ("copenhagen", pms[13].id);
}

TEST(DatumsTest, BuiltInPrimeMeridiansApplyInProjCreate) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  ScopedPj paris(
      proj_create(ctx.get(), "+proj=longlat +pm=paris +ellps=WGS84"));
  ASSERT_NE(nullptr, paris.get());
  EXPECT_EQ(0, proj_errno(paris.get()));
  EXPECT_NEAR(proj_dmstor("2d20'14.025\"E", nullptr), paris->from_greenwich,
              1e-15);

  ScopedPj ferro(
      proj_create(ctx.get(), "+proj=longlat +pm=ferro +ellps=WGS84"));
  ASSERT_NE(nullptr, ferro.get());
  EXPECT_EQ(0, proj_errno(ferro.get()));
  EXPECT_NEAR(proj_dmstor("17d40'W", nullptr), ferro->from_greenwich, 1e-15);

  ScopedPj bad_pm(
      proj_create(ctx.get(), "+proj=longlat +pm=no_such_pm +ellps=WGS84"));
  EXPECT_EQ(nullptr, bad_pm.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_context_errno(ctx.get()));
}

}  // namespace
}  // namespace proj
