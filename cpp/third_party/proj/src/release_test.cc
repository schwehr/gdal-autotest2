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
// Unit tests for PROJ release string and version metadata
// (proj/src/release.cpp). Note: in google3 builds, PROJ_GIT_HASH is unset so
// pj_release omits the optional git revision suffix.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/release.cpp

#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

static_assert(PROJ_VERSION_MAJOR == 9);
static_assert(PROJ_VERSION_MINOR == 9);
static_assert(PROJ_VERSION_PATCH == 0);
static_assert(PROJ_COMPUTE_VERSION(9, 9, 0) == 90900);
static_assert(PROJ_VERSION_NUMBER == 90900);
static_assert(PROJ_AT_LEAST_VERSION(9, 9, 0));
static_assert(!PROJ_AT_LEAST_VERSION(9, 9, 1));

TEST(ReleaseTest, GetReleaseReturnsStaticPjReleasePointer) {
  const char* release = pj_get_release();
  ASSERT_NE(nullptr, release);
  EXPECT_EQ(pj_release, release);
  EXPECT_EQ(release, pj_get_release());
  EXPECT_STREQ(pj_release, release);
  EXPECT_FALSE(std::string_view(release).empty());
}

TEST(ReleaseTest, ReleaseStringMatchesVersionMacrosAndReleaseDate) {
  const std::string expected_prefix = "Rel. " +
                                      std::to_string(PROJ_VERSION_MAJOR) + "." +
                                      std::to_string(PROJ_VERSION_MINOR) + "." +
                                      std::to_string(PROJ_VERSION_PATCH);
  const std::string_view release(pj_get_release());

  EXPECT_TRUE(release.starts_with(expected_prefix)) << release;
  EXPECT_TRUE(release.ends_with(", September 15th, 2026")) << release;
  // Pins PROJ 9.9.0 release notice without PROJ_GIT_HASH in google3.
  EXPECT_STREQ("Rel. 9.9.0, September 15th, 2026", pj_get_release());
}

TEST(ReleaseTest, ProjInfoExposesMatchingReleaseAndVersion) {
  // proj_info() populates process-global searchpath allocations via
  // pj_get_default_ctx(); clean up global caches on scope exit.
  absl::Cleanup cleanup = [] { proj_cleanup(); };

  const PJ_INFO info = proj_info();
  EXPECT_EQ(PROJ_VERSION_MAJOR, info.major);
  EXPECT_EQ(PROJ_VERSION_MINOR, info.minor);
  EXPECT_EQ(PROJ_VERSION_PATCH, info.patch);
  EXPECT_STREQ("9.9.0", info.version);
  EXPECT_EQ(pj_get_release(), info.release);
  EXPECT_EQ(pj_release, info.release);
  EXPECT_STREQ("Rel. 9.9.0, September 15th, 2026", info.release);
}

// Note: the #ifdef PROJ_GIT_HASH branch in release.cpp is a compile-time
// preprocessor gate not enabled in google3 builds.

}  // namespace
}  // namespace proj
