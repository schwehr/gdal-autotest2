// Copyright 2018 Google Inc. All Rights Reserved.
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
//
// Test UriEscape.c
//
// A = ASCII
//
// TODO(schwehr): Test the "Ex" variants of the functions.
// TODO(schwehr): Test more complete URIs.

#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "uriparser/Uri.h"
#include "uriparser/UriBase.h"

namespace {

constexpr UriBool kSpaceToPlus = URI_TRUE;
constexpr UriBool kSpaceToPercent = URI_FALSE;
constexpr UriBool kNormalizeBreaks = URI_TRUE;
constexpr UriBool kKeepBreaks = URI_FALSE;

TEST(UriEscapeATest, AllNull) {
  for (UriBool space_to_plus : {URI_FALSE, URI_TRUE}) {
    for (UriBool normalize_breaks : {URI_FALSE, URI_TRUE}) {
      std::vector<char> buf(10, '\0');
      EXPECT_EQ(nullptr,
                uriEscapeA(nullptr, nullptr, space_to_plus, normalize_breaks));
    }
  }
}

TEST(UriEscapeATest, SourceNull) {
  // nullptr for the source results in an empty output string.
  for (UriBool space_to_plus : {URI_FALSE, URI_TRUE}) {
    for (UriBool normalize_breaks : {URI_FALSE, URI_TRUE}) {
      std::vector<char> buf(10, '\0');
      EXPECT_STREQ(
          "", uriEscapeA(nullptr, &buf[0], space_to_plus, normalize_breaks));
    }
  }
}

TEST(UriEscapeATest, OutputNull) {
  for (UriBool space_to_plus : {URI_FALSE, URI_TRUE}) {
    for (UriBool normalize_breaks : {URI_FALSE, URI_TRUE}) {
      EXPECT_EQ(nullptr, uriEscapeA("string to escape", nullptr, space_to_plus,
                                    normalize_breaks));
    }
  }
}

// Verify that uriEscapeA converts the src string to the expected_result.
// uriEscapeA converts special characters to their percent equivalents.
// This function helps to hide the troubles of dealing with a C API that has
// dangerously left the size of buffers up to the caller.
// If space_to_plus is true, space characters (but not other whitespace) are
// transformed to the plus character.
// If normalize_breaks is true, then various styles of new lines are converted
// to uriparser's canonical idea of a new line, which is the DOS \r\n.
void CheckUriEscapeA(const string &src, const string &expected_result,
                     UriBool space_to_plus, UriBool normalize_breaks) {
  const char *in = src.c_str();

  const size_t max_char_size = normalize_breaks ? 6 : 3;
  const size_t buf_size = src.size() * max_char_size + 1;
  std::vector<char> buf(buf_size);
  char *out = &buf[0];
  const char *expected_last = &buf[expected_result.size()];

  const char *result = uriEscapeA(in, out, space_to_plus, normalize_breaks);
  EXPECT_EQ(expected_last, result)
      << "out: " << out << " last:" << expected_last;

  EXPECT_EQ(expected_result, out) << "For \"" << src << "\"";
}

TEST(UriEscapeATest, SameForAll) {
  // Things that all stay the same for all for UriBool combinations.
  for (UriBool space_to_plus : {URI_FALSE, URI_TRUE}) {
    for (UriBool normalize_breaks : {URI_FALSE, URI_TRUE}) {
      CheckUriEscapeA("", "", space_to_plus, normalize_breaks);
      CheckUriEscapeA("a", "a", space_to_plus, normalize_breaks);
      CheckUriEscapeA("+", "%2B", space_to_plus, normalize_breaks);
      CheckUriEscapeA("ABCEDEFGHIJKLMNOPQRSTUVWXYZ",
                      "ABCEDEFGHIJKLMNOPQRSTUVWXYZ", space_to_plus,
                      normalize_breaks);
    }
  }
}

TEST(UriEscapeATest, Spaces) {
  CheckUriEscapeA(" ", "%20", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA(" ", "+", kSpaceToPlus, kKeepBreaks);

  CheckUriEscapeA("\t", "%09", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA("\t", "%09", kSpaceToPlus, kKeepBreaks);

  CheckUriEscapeA("\v", "%0B", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA("\v", "%0B", kSpaceToPlus, kKeepBreaks);
}

TEST(UriEscapeATest, Percent) {
  // Should never do anything special.
  CheckUriEscapeA("%", "%25", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA("%2", "%252", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA("%z", "%25z", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA("%0B", "%250B", kSpaceToPlus, kKeepBreaks);
}

TEST(UriEscapeATest, Breaks) {
  // https://en.wikipedia.org/wiki/Newline
  // Unix
  CheckUriEscapeA("\n", "%0A", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA("\n", "%0D%0A", kSpaceToPercent, kNormalizeBreaks);
  // Not impacted by trailing text.
  CheckUriEscapeA("\nw", "%0Aw", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA("\nw", "%0D%0Aw", kSpaceToPercent, kNormalizeBreaks);

  // DOS
  CheckUriEscapeA("\r\n", "%0D%0A", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA("\r\n", "%0D%0A", kSpaceToPercent, kNormalizeBreaks);

  // C64 et al.
  CheckUriEscapeA("\r", "%0D", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA("\r", "%0D%0A", kSpaceToPercent, kNormalizeBreaks);

  // Acorn BBC and RISC OS
  CheckUriEscapeA("\n\r", "%0A%0D", kSpaceToPercent, kKeepBreaks);
  CheckUriEscapeA("\n\r", "%0D%0A%0D%0A", kSpaceToPercent, kNormalizeBreaks);
}

TEST(UriUnescapeInPlaceATest, Null) {
  EXPECT_EQ(nullptr, uriUnescapeInPlaceA(nullptr));
}

// Verify that uriUnescapeInPlaceA converts a percent encoded string back into
// a normal (unencoded) ASCII string.
// This function helps to hide the troubles of dealing with a C API.
// See uriUnescapeInPlaceAEx for the ability to control line breaks and space
// character handling.
void CheckUriUnescapeInPlaceA(const string &src,
                              const string &expected_result) {
  const size_t size = src.size() + 1;
  std::vector<char> buf(size, '\0');
  char *inout = &buf[0];
  snprintf(inout, size, "%s", src.c_str());
  const char *expected_last = &buf[expected_result.size()];

  const char *result = uriUnescapeInPlaceA(inout);
  EXPECT_EQ(expected_last, result)
      << "out: " << inout << " last:" << expected_last;

  EXPECT_EQ(expected_result, inout) << "For \"" << src << "\"";
}

TEST(UriUnescapeInPlaceATest, Empty) { CheckUriUnescapeInPlaceA("", ""); }

TEST(UriUnescapeInPlaceATest, Unchanged) {
  CheckUriUnescapeInPlaceA("abcdefhijklmnopqrstuvwxyz",
                           "abcdefhijklmnopqrstuvwxyz");
  CheckUriUnescapeInPlaceA("ABCEDEFGHIJKLMNOPQRSTUVWXYZ",
                           "ABCEDEFGHIJKLMNOPQRSTUVWXYZ");
  CheckUriUnescapeInPlaceA("0123456789", "0123456789");
}

TEST(UriUnescapeInPlaceATest, Breaks) {
  // Unix - does not convert.
  CheckUriUnescapeInPlaceA("%OA", "%OA");
  CheckUriUnescapeInPlaceA("\n", "\n");

  // DOS
  CheckUriUnescapeInPlaceA("%0D%0A", "\r\n");
  CheckUriUnescapeInPlaceA("\r\n", "\r\n");
  // Not impacted by trailing text.
  CheckUriUnescapeInPlaceA("%0D%0Aw", "\r\nw");

  // C64 et al.
  CheckUriUnescapeInPlaceA("%0D", "\r");

  // Acorn BBC and RISC OS
  CheckUriUnescapeInPlaceA("%0A%0D", "\n\r");
}

TEST(UriUnescapeInPlaceATest, Space) {
  CheckUriUnescapeInPlaceA(" ", " ");
  CheckUriUnescapeInPlaceA("%20", " ");
  CheckUriUnescapeInPlaceA("+", "+");

  CheckUriUnescapeInPlaceA("%09", "\t");
  CheckUriUnescapeInPlaceA("%0B", "\v");
}

TEST(UriUnescapeInPlaceATest, Percent) {
  CheckUriUnescapeInPlaceA("%", "%");
  CheckUriUnescapeInPlaceA("%2", "%2");
  CheckUriUnescapeInPlaceA("%25", "%");
  CheckUriUnescapeInPlaceA("%252", "%2");
  CheckUriUnescapeInPlaceA("%0G", "%0G");
}

}  // namespace
