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

#include <limits.h>

#include "gmock.h"
#include "gunit.h"
#include "third_party/absl/strings/match.h"
#include "third_party/absl/strings/string_view_utils.h"
#include "port/cpl_conv.h"

namespace autotest2 {
namespace {

TEST(CPLGetExecPathTest, BufferTooSmall) {
  char buf[1] = {};
  EXPECT_FALSE(CPLGetExecPath(buf, 0));
}

TEST(CPLGetExecPathTest, Basic) {
  const char kTestName[] = "cpl_getexecpath_test";
  char buf[PATH_MAX + 1] = {};
  EXPECT_TRUE(CPLGetExecPath(buf, PATH_MAX));
  // On a CAS filesystem, the result will look like:
  // /build/cas/668/66816bbffceb8b1de618d84f436ff590
  if (!absl::StartsWith(buf, "/build/cas")) {
    EXPECT_THAT(buf, testing::EndsWith(kTestName));
  }
}

}  // namespace
}  // namespace autotest2
