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
// Unit tests for PROJ's static operation registry and proj_list_operations()
// (proj/src/list.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/list.cpp

#include <cstddef>
#include <set>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

TEST(ListTest, ProjListOperationsReturnsConsistentNullTerminatedTable) {
  const PJ_OPERATIONS* ops = proj_list_operations();
  ASSERT_NE(nullptr, ops);
  EXPECT_EQ(ops, proj_list_operations());

  std::set<std::string_view> seen_ids;
  size_t count = 0;
  for (const PJ_OPERATIONS* cur = ops; cur->id != nullptr; ++cur) {
    const std::string_view id(cur->id);
    EXPECT_FALSE(id.empty()) << "index=" << count;
    EXPECT_NE(nullptr, cur->proj) << "id=" << id;
    ASSERT_NE(nullptr, cur->descr) << "id=" << id;
    ASSERT_NE(nullptr, *cur->descr) << "id=" << id;
    EXPECT_FALSE(std::string_view(*cur->descr).empty()) << "id=" << id;
    EXPECT_TRUE(seen_ids.insert(id).second) << "duplicate id=" << id;
    ++count;
  }

  // Verify the sentinel terminator clears all three fields.
  EXPECT_EQ(nullptr, ops[count].id);
  EXPECT_EQ(nullptr, ops[count].proj);
  EXPECT_EQ(nullptr, ops[count].descr);

  // PROJ 9.9 registers nearly 200 operations in pj_list.h.
  EXPECT_GT(count, 150u);
}

TEST(ListTest, RegistryIncludesCoreProjectionsConversionsAndAliases) {
  const PJ_OPERATIONS* ops = proj_list_operations();
  ASSERT_NE(nullptr, ops);

  std::set<std::string_view> ids;
  for (const PJ_OPERATIONS* cur = ops; cur->id != nullptr; ++cur) {
    ids.insert(cur->id);
  }

  constexpr std::string_view kExpectedIds[] = {
      "aea",      "axisswap",   "cart",     "deformation", "defmodel",
      "eqc",      "geocent",    "helmert",  "hgridshift",  "laea",
      "latlon",   "latlong",    "lcc",      "lonlat",      "longlat",
      "merc",     "molodensky", "noop",     "nzmg",        "omerc",
      "pipeline", "stere",      "tinshift", "tmerc",       "unitconvert",
      "utm",      "vgridshift", "webmerc",
  };
  for (const std::string_view expected : kExpectedIds) {
    EXPECT_TRUE(ids.contains(expected)) << "missing operation: " << expected;
  }
}

TEST(ListTest, EntryPointsAllocatePjWithMatchingShortNameAndDescription) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  const PJ_OPERATIONS* ops = proj_list_operations();
  ASSERT_NE(nullptr, ops);

  for (const PJ_OPERATIONS* cur = ops; cur->id != nullptr; ++cur) {
    ScopedPj pj(cur->proj(nullptr));
    ASSERT_NE(nullptr, pj.get()) << "id=" << cur->id;
    // Bind local context before ScopedPj destroys pj so PJ_DEFAULT_CTX is
    // never touched.
    pj->ctx = ctx.get();

    EXPECT_STREQ(cur->id, pj->short_name);
    EXPECT_STREQ(*cur->descr, pj->descr);
  }
}

}  // namespace
}  // namespace proj
