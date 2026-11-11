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

// Unit tests for //third_party/netcdf/libdispatch/dstring.c (NC_check_name,
// new_NC_string, set_NC_string, and free_NC_string).
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dstring.c

#include <cstddef>
#include <limits>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
#include "third_party/netcdf/include/nc.h"
}  // extern "C"

namespace netcdf {
namespace {

using ScopedNcString = std::unique_ptr<NC_string, decltype(&free_NC_string)>;

ScopedNcString MakeNcString(size_t slen, const char* str) {
  return ScopedNcString(new_NC_string(slen, str), &free_NC_string);
}

TEST(DstringTest, CheckNameAcceptsValidAsciiAndMultibyteNames) {
  constexpr const char* kValidNames[] = {
      "A",
      "Z",
      "a",
      "z",
      "0",
      "9",
      "_",
      "var_1",
      "temp with spaces",
      "name-with.punct:and~tilde",
      "\xC3\xA9",                 // Leading 2-byte UTF-8
      "\xC3\xA9_tail",            // Leading 2-byte UTF-8 + ASCII
      "\xE2\x82\xAC_eur",         // Leading 3-byte UTF-8
      "\xF0\x9F\x98\x80_emo",     // Leading 4-byte UTF-8
      "v_\xC3\xA9_\xE2\x82\xAC",  // Embedded 2-byte and 3-byte UTF-8
      "v_\xF0\x9F\x98\x80",       // Embedded/trailing 4-byte UTF-8
  };

  for (const char* name : kValidNames) {
    SCOPED_TRACE(name);
    EXPECT_EQ(NC_NOERR, NC_check_name(name));
  }

  // Exact NC_MAX_NAME byte length is accepted.
  const std::string max_len_name(static_cast<size_t>(NC_MAX_NAME), 'x');
  EXPECT_EQ(NC_NOERR, NC_check_name(max_len_name.c_str()));
}

TEST(DstringTest, CheckNameRejectsInvalidSyntaxUtf8SpacesAndOverlongNames) {
  constexpr const char* kBadNames[] = {
      "",           // Empty name
      "/root",      // Leading slash
      "a/b",        // Embedded slash
      "abc/",       // Trailing slash
      "-dash",      // Disallowed leading punctuation
      " space",     // Disallowed leading space
      ".dot",       // Disallowed leading dot
      "@at",        // Disallowed leading symbol
      "\x01_ctrl",  // Leading control byte
      "a\x01"
      "b",     // Embedded control byte
      "a\tb",  // Embedded tab (< ' ')
      "a\x7F"
      "b",             // Embedded DEL (0x7F)
      "abc ",          // Trailing space
      "a  ",           // Multiple trailing spaces
      "\x80",          // Invalid UTF-8 start
      "a\xFF",         // Invalid UTF-8 inside name
      "\xED\xA0\x80",  // Surrogate UTF-8
      "a\xC3",         // Truncated UTF-8 inside name
  };

  for (const char* name : kBadNames) {
    EXPECT_EQ(NC_EBADNAME, NC_check_name(name));
  }

  const std::string overlong_ascii(static_cast<size_t>(NC_MAX_NAME + 1), 'a');
  EXPECT_EQ(NC_EMAXNAME, NC_check_name(overlong_ascii.c_str()));

  // Multibyte UTF-8 pushing total byte count over NC_MAX_NAME.
  const std::string overlong_utf8 =
      std::string(static_cast<size_t>(NC_MAX_NAME - 1), 'a') + "\xC3\xA9";
  EXPECT_EQ(NC_EMAXNAME, NC_check_name(overlong_utf8.c_str()));
}

TEST(DstringTest, NewAndFreeNcStringHandleSizesAndOverflow) {
  // Null pointer is a safe no-op for free_NC_string.
  free_NC_string(nullptr);

  // Overflow guard prevents wrapped allocation sizes.
  EXPECT_EQ(nullptr, new_NC_string(std::numeric_limits<size_t>::max(), "x"));
  EXPECT_EQ(nullptr,
            new_NC_string(std::numeric_limits<size_t>::max() - 16, nullptr));

  // Zero-length allocations with null and empty initializer strings.
  {
    ScopedNcString s_null = MakeNcString(0, nullptr);
    ASSERT_NE(nullptr, s_null.get());
    EXPECT_EQ(size_t{0}, s_null->nchars);
    EXPECT_STREQ("", s_null->cp);

    ScopedNcString s_empty = MakeNcString(0, "");
    ASSERT_NE(nullptr, s_empty.get());
    EXPECT_STREQ("", s_empty->cp);
  }

  // Non-empty allocation with NULL string leaves zeroed buffer.
  {
    ScopedNcString s = MakeNcString(8, nullptr);
    ASSERT_NE(nullptr, s.get());
    EXPECT_GE(s->nchars, size_t{8});
    EXPECT_STREQ("", s->cp);
  }

  // Non-empty allocation copies contents and NUL-terminates.
  {
    ScopedNcString s = MakeNcString(5, "hello");
    ASSERT_NE(nullptr, s.get());
    EXPECT_EQ(size_t{5}, s->nchars);
    EXPECT_STREQ("hello", s->cp);
  }
}

TEST(DstringTest, SetNcStringUpdatesWithinCapacityAndRejectsTooLong) {
  ScopedNcString s = MakeNcString(5, "alpha");
  ASSERT_NE(nullptr, s.get());
  ASSERT_EQ(size_t{5}, s->nchars);

  // Too-long replacement fails without modifying capacity or stored text.
  EXPECT_EQ(NC_ENOTINDEFINE, set_NC_string(s.get(), "toolong"));
  EXPECT_STREQ("alpha", s->cp);

  // Shorter replacement updates string and preserves original nchars.
  EXPECT_EQ(NC_NOERR, set_NC_string(s.get(), "xy"));
  EXPECT_EQ(size_t{5}, s->nchars);
  EXPECT_STREQ("xy", s->cp);

  // Exact capacity replacement succeeds and stays NUL-terminated.
  EXPECT_EQ(NC_NOERR, set_NC_string(s.get(), "12345"));
  EXPECT_EQ(size_t{5}, s->nchars);
  EXPECT_STREQ("12345", s->cp);
}

}  // namespace
}  // namespace netcdf
