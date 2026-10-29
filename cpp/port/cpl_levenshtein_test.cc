// Tests for cpl_levenshtein:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_levenshtein.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_levenshtein.cpp
//
// Copyright 2026 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/port/cpl_levenshtein.h"

#include <cstddef>
#include <limits>
#include <string>

#include "testing/base/public/gunit.h"

namespace autotest2 {
namespace {

TEST(CplLevenshteinTest, EmptyAndBasicDistances) {
  EXPECT_EQ(0u, CPLLevenshteinDistance("", "", false));
  EXPECT_EQ(4u, CPLLevenshteinDistance("", "gdal", false));
  EXPECT_EQ(4u, CPLLevenshteinDistance("gdal", "", false));
  EXPECT_EQ(0u, CPLLevenshteinDistance("gdal", "gdal", false));
  EXPECT_EQ(3u, CPLLevenshteinDistance("kitten", "sitting", false));
}

TEST(CplLevenshteinTest, TranspositionAllowed) {
  // Without transposition: "ca" -> "ac" costs 2.
  EXPECT_EQ(2u, CPLLevenshteinDistance("ca", "ac", false));
  // With transposition: "ca" -> "ac" costs 1.
  EXPECT_EQ(1u, CPLLevenshteinDistance("ca", "ac", true));
  EXPECT_EQ(1u, CPLLevenshteinDistance("abcd", "abdc", true));
}

TEST(CplLevenshteinTest, VeryLongStrings) {
  const std::string long_a(32768, 'a');
  const std::string long_b(32768, 'b');
  EXPECT_EQ(0u, CPLLevenshteinDistance(long_a.c_str(), long_a.c_str(), false));
  EXPECT_EQ(std::numeric_limits<size_t>::max(),
            CPLLevenshteinDistance(long_a.c_str(), long_b.c_str(), false));
}

}  // namespace
}  // namespace autotest2
