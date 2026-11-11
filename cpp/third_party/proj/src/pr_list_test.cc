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
// Unit tests for projection parameter list formatting and expanded PROJ
// string reconstruction (proj/src/pr_list.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/pr_list.cpp

#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

std::string CapturePrList(PJ* pj) {
  testing::internal::CaptureStdout();
  pj_pr_list(pj);
  return testing::internal::GetCapturedStdout();
}

TEST(PrListTest, AllUsedMultiLineWrappingAndEmptyParamsBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Classic projection where all specified parameters are consumed.
  {
    ScopedPj merc(proj_create(ctx.get(), "+proj=merc +ellps=GRS80"));
    ASSERT_NE(nullptr, merc.get());
    const std::string output = CapturePrList(merc.get());
    EXPECT_THAT(output, HasSubstr("#Mercator\n#"));
    EXPECT_THAT(output, HasSubstr("+proj=merc"));
    EXPECT_THAT(output, HasSubstr("+ellps=GRS80"));
    EXPECT_THAT(output, Not(HasSubstr("NOT used")));
  }

  // 2. Projection with unused parameters, a parameter retaining a leading '+',
  // and line lengths exceeding LINE_LEN (72) to trigger "\n#" wrapping in both
  // the used and unused parameter sections.
  {
    ScopedPj rich(proj_create(
        ctx.get(),
        "+proj=merc +ellps=WGS84 +lon_0=10 +lat_ts=15 +x_0=100000 "
        "+y_0=200000 +k_0=0.9996 +unused_alpha=12345678901234567890 "
        "+unused_beta=12345678901234567890 ++prefixed=1 "
        "+unused_gamma=12345678901234567890"));
    ASSERT_NE(nullptr, rich.get());
    const std::string output = CapturePrList(rich.get());
    EXPECT_THAT(output, HasSubstr("#--- following specified but NOT used\n"));
    EXPECT_THAT(output, HasSubstr("+unused_alpha=12345678901234567890"));
    EXPECT_THAT(output, HasSubstr("+unused_gamma=12345678901234567890"));
    // "++prefixed=1" parses to "+prefixed=1" in paralist so '+' is not doubled.
    EXPECT_THAT(output, HasSubstr(" +prefixed=1"));
    EXPECT_THAT(output, Not(HasSubstr("++prefixed=1")));
    EXPECT_THAT(output, HasSubstr("\n# "));
  }

  // 3. Bare PJ with single-line and empty descriptions and no parameters
  // exercises the non-newline description loop and n == 1 exit branch.
  {
    ScopedPj empty(pj_new());
    ASSERT_NE(nullptr, empty.get());
    empty->ctx = ctx.get();
    empty->descr = "SingleLineDescription";
    EXPECT_EQ("#SingleLineDescription\n#", CapturePrList(empty.get()));

    empty->descr = "";
    EXPECT_EQ("#\n#", CapturePrList(empty.get()));
  }
}

TEST(PrListTest, GetDefFormatsShortGrowsBufferAndSkipsUnusedParams) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Short initial parameter (+R=1) fits within initial def_max=10 without
  // reallocating; subsequent parameters grow def_max; unused params are
  // omitted.
  ScopedPj merc(proj_create(
      ctx.get(), "+R=1 +proj=merc +lat_ts=15 +unused_param=ignored"));
  ASSERT_NE(nullptr, merc.get());

  ScopedMalloc<char> def0(pj_get_def(merc.get(), 0));
  ASSERT_NE(nullptr, def0.get());
  EXPECT_THAT(def0.get(), HasSubstr(" +R=1"));
  EXPECT_THAT(def0.get(), HasSubstr(" +proj=merc"));
  EXPECT_THAT(def0.get(), HasSubstr(" +lat_ts=15"));
  EXPECT_THAT(def0.get(), Not(HasSubstr("unused_param")));

  // Options argument is currently unused; passing non-zero matches options=0.
  ScopedMalloc<char> def1(pj_get_def(merc.get(), 1));
  ASSERT_NE(nullptr, def1.get());
  EXPECT_STREQ(def0.get(), def1.get());

  // 2. Empty parameter list yields an empty heap-allocated string.
  ScopedPj empty(pj_new());
  ASSERT_NE(nullptr, empty.get());
  empty->ctx = ctx.get();
  ScopedMalloc<char> empty_def(pj_get_def(empty.get(), 0));
  ASSERT_NE(nullptr, empty_def.get());
  EXPECT_STREQ("", empty_def.get());
}

}  // namespace
}  // namespace proj
