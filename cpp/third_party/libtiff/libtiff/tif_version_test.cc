// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for libtiff/tif_version.c (TIFFGetVersion and version macros).

#include <cstring>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffvers.h"

namespace libtiff {
namespace {

TEST(TifVersionTest, ReturnsConfiguredVersionString) {
  const char* version = TIFFGetVersion();
  ASSERT_NE(version, nullptr);
  EXPECT_EQ(std::string_view(version), std::string_view(TIFFLIB_VERSION_STR));
  EXPECT_NE(std::string_view(version).find("LIBTIFF, Version "),
            std::string_view::npos);
  // Google-patched version date in tiffvers.h.
  EXPECT_GE(TIFFLIB_VERSION, 20240912);
}

TEST(TifVersionTest, PointerIsStableAcrossCalls) {
  const char* first = TIFFGetVersion();
  const char* second = TIFFGetVersion();
  EXPECT_EQ(first, second);
  EXPECT_GT(std::strlen(first), 0U);
}

}  // namespace
}  // namespace libtiff
