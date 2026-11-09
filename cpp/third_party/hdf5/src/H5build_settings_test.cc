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
// Tests embedded build configuration string contents and consistency with
// compiled HDF5 version and feature settings
// (third_party/hdf5/src/H5build_settings.c). (LF=0 in default google3 non-MPI
// Linux build: H5build_settings.c defines only the const char
// H5build_settings[] data array with no executable statements.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5build_settings.c

#ifndef NDEBUG
#define H5BUILD_SETTINGS_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#ifdef H5BUILD_SETTINGS_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5BUILD_SETTINGS_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <cstring>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

TEST(H5BuildSettingsTest, ExportsNonEmptyNullTerminatedSummary) {
  ASSERT_NE(H5build_settings, nullptr);
  const size_t len = std::strlen(H5build_settings);
  EXPECT_GT(len, 100U);

  const std::string_view settings(H5build_settings, len);
  EXPECT_THAT(settings, HasSubstr("SUMMARY OF THE HDF5 CONFIGURATION"));
  EXPECT_THAT(settings, HasSubstr("General Information:"));
  EXPECT_THAT(settings, HasSubstr("Compiling Options:"));
  EXPECT_THAT(settings, HasSubstr("Linking Options:"));
  EXPECT_THAT(settings, HasSubstr("Languages:"));
  EXPECT_THAT(settings, HasSubstr("Features:"));
}

TEST(H5BuildSettingsTest, MatchesLibraryVersionAndCompiledBuildFeatures) {
  unsigned major = 0;
  unsigned minor = 0;
  unsigned release = 0;
  ASSERT_GE(H5get_libversion(&major, &minor, &release), 0);
  EXPECT_EQ(major, static_cast<unsigned>(H5_VERS_MAJOR));
  EXPECT_EQ(minor, static_cast<unsigned>(H5_VERS_MINOR));
  EXPECT_EQ(release, static_cast<unsigned>(H5_VERS_RELEASE));

  bool is_threadsafe = false;
  ASSERT_GE(H5is_library_threadsafe(&is_threadsafe), 0);
  EXPECT_TRUE(is_threadsafe);

  const std::string_view settings(H5build_settings);
  const std::string expected_version =
      "HDF5 Version: " + std::to_string(major) + "." + std::to_string(minor) +
      "." + std::to_string(release);
  EXPECT_THAT(settings, HasSubstr(expected_version));
  EXPECT_THAT(settings, HasSubstr("Parallel HDF5: OFF"));
  EXPECT_THAT(settings, HasSubstr("Threadsafety: on"));
  EXPECT_THAT(settings, HasSubstr("With deprecated public symbols: ON"));
  EXPECT_THAT(settings, HasSubstr("Default API mapping: v200"));
}

}  // namespace
}  // namespace hdf5
