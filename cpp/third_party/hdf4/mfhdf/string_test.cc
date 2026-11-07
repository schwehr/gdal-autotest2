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
// Tests counted string operations and XDR serialization (mfhdf/src/string.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/string.c

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// GoogleTest defines SUCCEED() and FAIL() as macros; undefine them before
// including HDF4 headers to avoid macro collision under --config=no_modules.
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/mfhdf/src/nc_priv.h"
#include "third_party/hdf4/mfhdf/src/hdf_xdr_priv.h"
// clang-format on

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

class StringTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = ncopts;
    saved_ncerr_ = ncerr;
    ncopts = 0;
    ncerr = NC_NOERR;
  }

  void TearDown() override {
    ncopts = saved_ncopts_;
    ncerr = saved_ncerr_;
  }

 private:
  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
};

TEST_F(StringTest, NewStringHandlesZeroLength) {
  NC_string* s = NC_new_string(0, nullptr);
  ASSERT_THAT(s, NotNull());
  absl::Cleanup free_s = [s] { EXPECT_THAT(NC_free_string(s), Eq(SUCCEED)); };

  EXPECT_THAT(s->count, Eq(0u));
  EXPECT_THAT(s->len, Eq(0u));
  EXPECT_THAT(s->hash, Eq(0u));
  EXPECT_THAT(s->values, IsNull());
}

TEST_F(StringTest, NewStringCopiesShortString) {
  constexpr char kText[] = "abc";
  NC_string* s =
      NC_new_string(static_cast<unsigned>(std::strlen(kText)), kText);
  ASSERT_THAT(s, NotNull());
  absl::Cleanup free_s = [s] { EXPECT_THAT(NC_free_string(s), Eq(SUCCEED)); };

  EXPECT_THAT(s->count, Eq(3u));
  EXPECT_THAT(s->len, Eq(3u));
  EXPECT_THAT(s->hash, Ne(0u));
  ASSERT_THAT(s->values, NotNull());
  EXPECT_THAT(s->values, StrEq(kText));
}

TEST_F(StringTest, NewStringComputesHashWithMultipleBlocksAndRemainder) {
  // 10 bytes: 2 blocks of 4 bytes (sizeof(uint32)) + 2-byte remainder.
  constexpr char kText[] = "0123456789";
  NC_string* s =
      NC_new_string(static_cast<unsigned>(std::strlen(kText)), kText);
  ASSERT_THAT(s, NotNull());
  absl::Cleanup free_s = [s] { EXPECT_THAT(NC_free_string(s), Eq(SUCCEED)); };

  EXPECT_THAT(s->count, Eq(10u));
  EXPECT_THAT(s->len, Eq(10u));
  EXPECT_THAT(s->hash, Ne(0u));
  ASSERT_THAT(s->values, NotNull());
  EXPECT_THAT(s->values, StrEq(kText));
}

TEST_F(StringTest, NewStringWithNullBufferAllocatesWithoutCopying) {
  NC_string* s = NC_new_string(8, nullptr);
  ASSERT_THAT(s, NotNull());
  absl::Cleanup free_s = [s] { EXPECT_THAT(NC_free_string(s), Eq(SUCCEED)); };

  EXPECT_THAT(s->count, Eq(8u));
  EXPECT_THAT(s->len, Eq(8u));
  EXPECT_THAT(s->hash, Eq(0u));  // compute_hash returns 0 when str == NULL.
  EXPECT_THAT(s->values, NotNull());
}

TEST_F(StringTest, NewStringExceedingMaxNameFailsWithEmaxName) {
  // Provide enough characters so NCadvise reading str[0..5] does not read out
  // of bounds.
  const std::string overlong(H4_MAX_NC_NAME + 10, 'A');
  NC_string* s =
      NC_new_string(static_cast<unsigned>(overlong.size()), overlong.c_str());

  EXPECT_THAT(s, IsNull());
  EXPECT_THAT(ncerr, Eq(NC_EMAXNAME));
}

TEST_F(StringTest, CompareStringMacroDifferentiatesContent) {
  constexpr char kStr1[] = "alpha";
  constexpr char kStr2[] = "beta";
  NC_string* s1 =
      NC_new_string(static_cast<unsigned>(std::strlen(kStr1)), kStr1);
  NC_string* s2 =
      NC_new_string(static_cast<unsigned>(std::strlen(kStr2)), kStr2);
  NC_string* s3 =
      NC_new_string(static_cast<unsigned>(std::strlen(kStr1)), kStr1);
  ASSERT_THAT(s1, NotNull());
  ASSERT_THAT(s2, NotNull());
  ASSERT_THAT(s3, NotNull());
  absl::Cleanup free_strings = [s1, s2, s3] {
    EXPECT_THAT(NC_free_string(s1), Eq(SUCCEED));
    EXPECT_THAT(NC_free_string(s2), Eq(SUCCEED));
    EXPECT_THAT(NC_free_string(s3), Eq(SUCCEED));
  };

  EXPECT_THAT(NC_compare_string(s1, s3), Eq(0));
  EXPECT_THAT(NC_compare_string(s1, s2), Ne(0));
}

TEST_F(StringTest, FreeStringAcceptsNull) {
  EXPECT_THAT(NC_free_string(nullptr), Eq(SUCCEED));
}

TEST_F(StringTest, ReStringModifiesExistingStringWithinAllocatedBuffer) {
  constexpr char kInitial[] = "0123456789";
  NC_string* s =
      NC_new_string(static_cast<unsigned>(std::strlen(kInitial)), kInitial);
  ASSERT_THAT(s, NotNull());
  absl::Cleanup free_s = [s] { EXPECT_THAT(NC_free_string(s), Eq(SUCCEED)); };

  // Shrink string to 4 characters.
  constexpr char kShorter[] = "zero";
  NC_string* result =
      NC_re_string(s, static_cast<unsigned>(std::strlen(kShorter)), kShorter);
  EXPECT_THAT(result, Eq(s));
  EXPECT_THAT(s->count, Eq(10u));  // Allocated capacity remains 10.
  EXPECT_THAT(s->len, Eq(4u));
  EXPECT_THAT(s->values, StrEq(kShorter));

  // Passing null string returns null.
  EXPECT_THAT(NC_re_string(s, 4, nullptr), IsNull());

  // Attempting to grow beyond count fails with NC_ENOTINDEFINE.
  const std::string too_long(15, 'x');
  EXPECT_THAT(
      NC_re_string(s, static_cast<unsigned>(too_long.size()), too_long.c_str()),
      IsNull());
  EXPECT_THAT(ncerr, Eq(NC_ENOTINDEFINE));
}

TEST_F(StringTest, XlenStringCalculatesPaddedSizes) {
  EXPECT_THAT(NC_xlen_string(nullptr), Eq(4));

  NC_string* s0 = NC_new_string(0, nullptr);
  EXPECT_THAT(NC_xlen_string(s0), Eq(4));
  EXPECT_THAT(NC_free_string(s0), Eq(SUCCEED));

  NC_string* s1 = NC_new_string(1, "x");
  EXPECT_THAT(NC_xlen_string(s1), Eq(8));  // 4 + 1 = 5 -> rounded up to 8
  EXPECT_THAT(NC_free_string(s1), Eq(SUCCEED));

  NC_string* s4 = NC_new_string(4, "four");
  EXPECT_THAT(NC_xlen_string(s4), Eq(8));  // 4 + 4 = 8 -> multiple of 4
  EXPECT_THAT(NC_free_string(s4), Eq(SUCCEED));

  NC_string* s5 = NC_new_string(5, "hello");
  EXPECT_THAT(NC_xlen_string(s5), Eq(12));  // 4 + 5 = 9 -> rounded up to 12
  EXPECT_THAT(NC_free_string(s5), Eq(SUCCEED));
}

TEST_F(StringTest, XdrStringRoundTripAndNullHandling) {
  const std::string file_path = GetTempFilePath("string_test.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // 1. Encode a null string followed by a populated string.
  {
    const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    ASSERT_THAT(fd, Ne(-1));
    XDR xdrs{};
    const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDWR, XDR_ENCODE);
    if (xdrs.x_private == nullptr) {
      close(fd);
    }
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    ASSERT_THAT(create_rc, Ge(0));

    NC_string* null_str = nullptr;
    EXPECT_THAT(xdr_NC_string(&xdrs, &null_str), Eq(TRUE));

    constexpr char kPayload[] = "testing_xdr";
    NC_string* pop_str =
        NC_new_string(static_cast<unsigned>(std::strlen(kPayload)), kPayload);
    ASSERT_THAT(pop_str, NotNull());
    EXPECT_THAT(xdr_NC_string(&xdrs, &pop_str), Eq(TRUE));
    EXPECT_THAT(NC_free_string(pop_str), Eq(SUCCEED));
  }

  // 2. Decode back and verify.
  {
    const int fd = open(file_path.c_str(), O_RDONLY);
    ASSERT_THAT(fd, Ne(-1));
    XDR xdrs{};
    const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_DECODE);
    if (xdrs.x_private == nullptr) {
      close(fd);
    }
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    ASSERT_THAT(create_rc, Ge(0));

    NC_string* decoded_null = nullptr;
    EXPECT_THAT(xdr_NC_string(&xdrs, &decoded_null), Eq(TRUE));
    EXPECT_THAT(decoded_null, IsNull());

    NC_string* decoded_pop = nullptr;
    EXPECT_THAT(xdr_NC_string(&xdrs, &decoded_pop), Eq(TRUE));
    ASSERT_THAT(decoded_pop, NotNull());
    EXPECT_THAT(decoded_pop->count, Eq(11u));
    EXPECT_THAT(decoded_pop->len, Eq(11u));
    EXPECT_THAT(decoded_pop->values, StrEq("testing_xdr"));

    // 3. XDR_FREE frees the decoded string.
    xdrs.x_op = XDR_FREE;
    EXPECT_THAT(xdr_NC_string(&xdrs, &decoded_pop), Eq(TRUE));

    // 4. Invalid operation returns FALSE.
    constexpr enum xdr_op kBogusXdrOp = static_cast<enum xdr_op>(3);
    xdrs.x_op = kBogusXdrOp;
    EXPECT_THAT(xdr_NC_string(&xdrs, &decoded_null), Eq(FALSE));
  }
}

TEST_F(StringTest, XdrStringDecodeTruncatedStreamFails) {
  const std::string file_path = GetTempFilePath("string_trunc.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Write only 2 bytes (cannot read 4-byte uint for length).
  {
    const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ASSERT_THAT(fd, Ne(-1));
    const uint8_t partial[] = {0, 0};
    EXPECT_THAT(write(fd, partial, sizeof(partial)), Eq(2));
    close(fd);
  }

  const int fd = open(file_path.c_str(), O_RDONLY);
  ASSERT_THAT(fd, Ne(-1));
  XDR xdrs{};
  const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_DECODE);
  if (xdrs.x_private == nullptr) {
    close(fd);
  }
  absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
  ASSERT_THAT(create_rc, Ge(0));

  NC_string* s = nullptr;
  EXPECT_THAT(xdr_NC_string(&xdrs, &s), Eq(FALSE));
  EXPECT_THAT(s, IsNull());
}

// TODO(schwehr): Uncovered lines in string.c (~93.1% covered) are defensive OOM
// and XDR encode failure paths.

}  // namespace
}  // namespace hdf4
