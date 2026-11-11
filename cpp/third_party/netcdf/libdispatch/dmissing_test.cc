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

// Unit tests for //third_party/netcdf/libdispatch/dmissing.c.
//
// In google3's config.h, HAVE_STRDUP, HAVE_STRNDUP, and HAVE_MEMMOVE are 1
// while HAVE_STRLCAT is undefined, so nc_strlcat() is the sole compiled-in
// function in dmissing.c.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dmissing.c

#include <array>
#include <cstddef>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
size_t nc_strlcat(char* dst, const char* src, size_t dsize);
}  // extern "C"

namespace netcdf {
namespace {

TEST(DmissingTest, AppendsWithinCapacityWithoutTruncation) {
  std::array<char, 16> buf{};
  buf[0] = 'n';
  buf[1] = 'e';
  buf[2] = 't';
  buf[3] = '\0';
  buf[15] = 'Z';

  const size_t total = nc_strlcat(buf.data(), "cdf4", 15);
  EXPECT_EQ(7U, total);
  EXPECT_STREQ("netcdf4", buf.data());
  EXPECT_EQ('Z', buf[15]);
}

TEST(DmissingTest, HandlesExactFitAndOneByteTruncationBoundary) {
  // Exact fit: dlen(3) + slen(2) == dsize(6) - 1.
  std::array<char, 8> exact_buf{'a', 'b', 'c', '\0', 'X', 'X', 'G', 'G'};
  const size_t exact_len = nc_strlcat(exact_buf.data(), "de", 6);
  EXPECT_EQ(5U, exact_len);
  EXPECT_STREQ("abcde", exact_buf.data());
  EXPECT_EQ('G', exact_buf[6]);

  // Off-by-one overflow: dlen(3) + slen(3) == dsize(6) -> truncated by 1 char.
  std::array<char, 8> trunc_buf{'a', 'b', 'c', '\0', 'X', 'X', 'G', 'G'};
  const size_t trunc_len = nc_strlcat(trunc_buf.data(), "def", 6);
  EXPECT_EQ(6U, trunc_len);
  EXPECT_STREQ("abcde", trunc_buf.data());
  EXPECT_EQ('G', trunc_buf[6]);
}

TEST(DmissingTest, TruncatesLongSourceAndReportsFullDesiredLength) {
  std::array<char, 10> buf{'h', 'i', '\0', '?', '?', '?', '#', '#', '#', '#'};
  const size_t result = nc_strlcat(buf.data(), "_world_long", 6);

  EXPECT_EQ(13U, result);
  EXPECT_STREQ("hi_wo", buf.data());
  EXPECT_EQ('\0', buf[5]);
  // Bytes past dsize must remain unmodified.
  EXPECT_EQ('#', buf[6]);
  EXPECT_EQ('#', buf[9]);
}

TEST(DmissingTest, HandlesEmptyInputsAndMinimalBufferSizes) {
  // Empty dst + non-empty src with dsize == 1 writes only trailing NUL.
  std::array<char, 4> one_byte{'\0', 'Q', 'Q', 'Q'};
  EXPECT_EQ(3U, nc_strlcat(one_byte.data(), "xyz", 1));
  EXPECT_STREQ("", one_byte.data());
  EXPECT_EQ('Q', one_byte[1]);

  // Empty dst + empty src.
  std::array<char, 4> both_empty{'\0', 'Q', 'Q', 'Q'};
  EXPECT_EQ(0U, nc_strlcat(both_empty.data(), "", 1));
  EXPECT_STREQ("", both_empty.data());

  // Non-empty dst + empty src leaves dst unchanged.
  std::array<char, 8> empty_src{'a', 'b', 'c', '\0', 'Q', 'Q', 'Q', 'Q'};
  EXPECT_EQ(3U, nc_strlcat(empty_src.data(), "", 8));
  EXPECT_STREQ("abc", empty_src.data());

  // Empty dst + non-empty src within capacity.
  std::array<char, 8> empty_dst{'\0', '?', '?', '?', '?', '?', '?', '?'};
  EXPECT_EQ(4U, nc_strlcat(empty_dst.data(), "test", sizeof(empty_dst)));
  EXPECT_STREQ("test", empty_dst.data());
}

TEST(DmissingTest, ReturnsDsizePlusSlenWhenDstLacksNullWithinDsize) {
  // dsize == 0 triggers n-- == 0 immediately without dereferencing dst.
  std::array<char, 4> zero_buf{'A', 'B', 'C', '\0'};
  EXPECT_EQ(4U, nc_strlcat(zero_buf.data(), "1234", 0));
  EXPECT_EQ((std::array<char, 4>{'A', 'B', 'C', '\0'}), zero_buf);

  // dst has no NUL within first dsize bytes -> stops scanning dst at dsize.
  std::array<char, 8> unterminated{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
  const size_t ret = nc_strlcat(unterminated.data(), "xy", 4);
  EXPECT_EQ(6U, ret);
  EXPECT_EQ('A', unterminated[0]);
  EXPECT_EQ('D', unterminated[3]);
  EXPECT_EQ('E', unterminated[4]);

  // strlen(dst) == dsize boundary (NUL sits at index dsize).
  std::array<char, 8> exact_dst{'a', 'b', 'c', 'd', '\0', 'X', 'Y', 'Z'};
  EXPECT_EQ(7U, nc_strlcat(exact_dst.data(), "123", 4));
  EXPECT_STREQ("abcd", exact_dst.data());
  EXPECT_EQ('X', exact_dst[5]);
}

TEST(DmissingTest, SupportsChainedAppendsPreservingTailGuardBytes) {
  std::array<char, 16> buf{};
  buf.back() = '!';

  EXPECT_EQ(5U, nc_strlcat(buf.data(), "alpha", 12));
  EXPECT_EQ(6U, nc_strlcat(buf.data(), "-", 12));
  EXPECT_EQ(10U, nc_strlcat(buf.data(), "beta", 12));
  EXPECT_STREQ("alpha-beta", buf.data());
  EXPECT_EQ(14U, nc_strlcat(buf.data(), "-end", 12));
  EXPECT_EQ(std::string_view("alpha-beta-"), std::string_view(buf.data()));
  EXPECT_EQ('!', buf.back());
}

}  // namespace
}  // namespace netcdf
