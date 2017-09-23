// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "autotest2/cpp/util/cpl_cstringlist.h"

#include <vector>

#include "gmock.h"
#include "gunit.h"
#include "port/cpl_string.h"

namespace autotest2 {
namespace {

TEST(CplCStringListTest, CslToVector) {
  EXPECT_TRUE(CslToVector(nullptr).empty());

  {
    const char * const csl[] = {nullptr};
    EXPECT_TRUE(CslToVector(csl).empty());
  }

  {
    const char * const csl[] = {"a", nullptr};
    EXPECT_THAT(CslToVector(csl), testing::ElementsAre("a"));
  }

  {
    const char * const csl[] = {"a", "b", nullptr};
    EXPECT_THAT(CslToVector(csl), testing::ElementsAre("a", "b"));
  }

  {
    const char * const csl[] = {"a", "a", nullptr};
    EXPECT_THAT(CslToVector(csl), testing::ElementsAre("a", "a"));
  }
}

TEST(CplCStringListTest, VectorToCsl) {
  EXPECT_EQ(nullptr, VectorToCsl({}));

  {
    auto csl = VectorToCsl({"a"});
    EXPECT_THAT(CslToVector(csl), testing::ElementsAre("a"));
    CSLDestroy(csl);
  }

  {
    auto csl = VectorToCsl({"a", "b"});
    EXPECT_THAT(CslToVector(csl), testing::ElementsAre("a", "b"));
    CSLDestroy(csl);
  }

  {
    auto csl = VectorToCsl({"a", "a"});
    EXPECT_THAT(CslToVector(csl), testing::ElementsAre("a", "a"));
    CSLDestroy(csl);
  }
}

}  // namespace
}  // namespace autotest2
