// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/util/string.h"

#include <string>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

TEST(StringUtilTest, EndsWithString) {
  EXPECT_TRUE(geos::util::endsWith("hello world", "world"));
  EXPECT_TRUE(geos::util::endsWith("abc", "abc"));
  EXPECT_TRUE(geos::util::endsWith("abc", ""));
  EXPECT_TRUE(geos::util::endsWith("", ""));

  EXPECT_FALSE(geos::util::endsWith("hello world", "hello"));
  EXPECT_FALSE(geos::util::endsWith("abc", "C"));
  EXPECT_FALSE(geos::util::endsWith("ab", "cb"));
  EXPECT_FALSE(geos::util::endsWith("ab", "abc"));
  EXPECT_FALSE(geos::util::endsWith("", "a"));
}

TEST(StringUtilTest, EndsWithChar) {
  EXPECT_FALSE(geos::util::endsWith("", 'a'));
  EXPECT_FALSE(geos::util::endsWith("", '\0'));

  EXPECT_TRUE(geos::util::endsWith("a", 'a'));
  EXPECT_TRUE(geos::util::endsWith("abc", 'c'));

  EXPECT_FALSE(geos::util::endsWith("a", 'b'));
  EXPECT_FALSE(geos::util::endsWith("abc", 'a'));
  EXPECT_FALSE(geos::util::endsWith("abc", 'C'));
}

TEST(StringUtilTest, StartsWithString) {
  EXPECT_TRUE(geos::util::startsWith("hello world", "hello"));
  EXPECT_TRUE(geos::util::startsWith("abc", "abc"));
  EXPECT_TRUE(geos::util::startsWith("abc", ""));
  EXPECT_TRUE(geos::util::startsWith("", ""));

  EXPECT_FALSE(geos::util::startsWith("hello world", "world"));
  EXPECT_FALSE(geos::util::startsWith("abc", "A"));
  EXPECT_FALSE(geos::util::startsWith("ab", "ac"));
  EXPECT_FALSE(geos::util::startsWith("ab", "abc"));
  EXPECT_FALSE(geos::util::startsWith("", "a"));
}

TEST(StringUtilTest, StartsWithChar) {
  // Upstream geos::util::startsWith(const std::string&, char) checks
  // `if (s.empty() == 0)` (inverted emptiness check), so non-empty strings
  // return false early and empty strings compare s[0] (the trailing '\0').
  EXPECT_FALSE(geos::util::startsWith("", 'a'));
  EXPECT_TRUE(geos::util::startsWith("", '\0'));
  EXPECT_FALSE(geos::util::startsWith("abc", 'a'));
  EXPECT_FALSE(geos::util::startsWith("abc", 'b'));
}

TEST(StringUtilTest, ToUpper) {
  std::string empty;
  geos::util::toUpper(empty);
  EXPECT_EQ(empty, "");

  std::string lower = "abcdef";
  geos::util::toUpper(lower);
  EXPECT_EQ(lower, "ABCDEF");

  std::string mixed = "Hello, World! 123_z";
  geos::util::toUpper(mixed);
  EXPECT_EQ(mixed, "HELLO, WORLD! 123_Z");

  std::string upper = "GEOS";
  geos::util::toUpper(upper);
  EXPECT_EQ(upper, "GEOS");
}

}  // namespace
}  // namespace geos_google
