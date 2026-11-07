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

#include <cstdint>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/vg_priv.h"

namespace hdf4 {
namespace {

TEST(VparseTest, SingleToken) {
  int32_t count = 0;
  char** tokens = nullptr;

  ASSERT_EQ(SUCCEED, scanattrs("single_field", &count, &tokens));
  EXPECT_EQ(count, 1);
  ASSERT_NE(tokens, nullptr);
  EXPECT_STREQ(tokens[0], "single_field");
  EXPECT_EQ(tokens[1], nullptr);
}

TEST(VparseTest, MultipleTokensWithSpaces) {
  int32_t count = 0;
  char** tokens = nullptr;

  ASSERT_EQ(SUCCEED,
            scanattrs("field_x,  field_y,   field_z", &count, &tokens));
  EXPECT_EQ(count, 3);
  ASSERT_NE(tokens, nullptr);
  EXPECT_STREQ(tokens[0], "field_x");
  EXPECT_STREQ(tokens[1], "field_y");
  EXPECT_STREQ(tokens[2], "field_z");
  EXPECT_EQ(tokens[3], nullptr);
}

TEST(VparseTest, BufferGrowth) {
  int32_t count = 0;
  char** tokens = nullptr;

  // First allocate a small buffer.
  ASSERT_EQ(SUCCEED, scanattrs("a", &count, &tokens));
  EXPECT_EQ(count, 1);
  EXPECT_STREQ(tokens[0], "a");

  // Now parse a much longer string, exercising buffer reallocation.
  const std::string long_attrs =
      "first_very_long_field_name, second_very_long_field_name, "
      "third_very_long_field_name, fourth_very_long_field_name";
  ASSERT_EQ(SUCCEED, scanattrs(long_attrs.c_str(), &count, &tokens));
  EXPECT_EQ(count, 4);
  EXPECT_STREQ(tokens[0], "first_very_long_field_name");
  EXPECT_STREQ(tokens[1], "second_very_long_field_name");
  EXPECT_STREQ(tokens[2], "third_very_long_field_name");
  EXPECT_STREQ(tokens[3], "fourth_very_long_field_name");
}

TEST(VparseTest, FieldNameTruncation) {
  int32_t count = 0;
  char** tokens = nullptr;

  const std::string long1(FIELDNAMELENMAX + 20, 'A');
  const std::string long2(FIELDNAMELENMAX + 30, 'B');
  const std::string input = long1 + "," + long2;

  ASSERT_EQ(SUCCEED, scanattrs(input.c_str(), &count, &tokens));
  EXPECT_EQ(count, 2);
  ASSERT_NE(tokens, nullptr);

  // Both tokens should be truncated to FIELDNAMELENMAX.
  EXPECT_EQ(strlen(tokens[0]), FIELDNAMELENMAX);
  EXPECT_EQ(tokens[0], long1.substr(0, FIELDNAMELENMAX));

  EXPECT_EQ(strlen(tokens[1]), FIELDNAMELENMAX);
  EXPECT_EQ(tokens[1], long2.substr(0, FIELDNAMELENMAX));
}

TEST(VparseTest, InvalidSyntax) {
  int32_t count = 0;
  char** tokens = nullptr;

  // Empty string.
  EXPECT_EQ(FAIL, scanattrs("", &count, &tokens));

  // Leading comma.
  EXPECT_EQ(FAIL, scanattrs(",field", &count, &tokens));

  // Trailing comma.
  EXPECT_EQ(FAIL, scanattrs("field,", &count, &tokens));

  // Trailing comma followed by spaces.
  EXPECT_EQ(FAIL, scanattrs("field,   ", &count, &tokens));

  // Consecutive commas.
  EXPECT_EQ(FAIL, scanattrs("f1,,f2", &count, &tokens));

  // Comma with only spaces between.
  EXPECT_EQ(FAIL, scanattrs("f1, ,f2", &count, &tokens));
}

TEST(VparseTest, Shutdown) {
  int32_t count = 0;
  char** tokens = nullptr;

  // Ensure buffer is allocated.
  ASSERT_EQ(SUCCEED, scanattrs("test_field", &count, &tokens));

  // First shutdown frees buffer.
  EXPECT_EQ(SUCCEED, VPparse_shutdown());

  // Second shutdown when already NULL should succeed gracefully.
  EXPECT_EQ(SUCCEED, VPparse_shutdown());
}

}  // namespace
}  // namespace hdf4
