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
// Tests integer array operations and XDR serialization (mfhdf/src/iarray.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/iarray.c

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
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

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Ge;
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

class IarrayTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = ncopts;
    saved_ncerr_ = ncerr;
    ncopts = 0;
    ncerr = NC_NOERR;
  }

  void TearDown() override {
    HEclear();
    ncopts = saved_ncopts_;
    ncerr = saved_ncerr_;
  }

 private:
  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
};

TEST_F(IarrayTest, NewIarrayHandlesZeroCount) {
  NC_iarray* arr = NC_new_iarray(0, nullptr);
  ASSERT_THAT(arr, NotNull());
  absl::Cleanup free_arr = [arr] {
    EXPECT_THAT(NC_free_iarray(arr), Eq(SUCCEED));
  };

  EXPECT_THAT(arr->count, Eq(0u));
  EXPECT_THAT(arr->values, IsNull());
}

TEST_F(IarrayTest, NewIarrayCopiesValues) {
  constexpr std::array<int, 3> kValues = {10, -20, 30};
  NC_iarray* arr =
      NC_new_iarray(static_cast<unsigned>(kValues.size()), kValues.data());
  ASSERT_THAT(arr, NotNull());
  absl::Cleanup free_arr = [arr] {
    EXPECT_THAT(NC_free_iarray(arr), Eq(SUCCEED));
  };

  EXPECT_THAT(arr->count, Eq(3u));
  ASSERT_THAT(arr->values, NotNull());
  const std::array<int, 3> actual{arr->values[0], arr->values[1],
                                  arr->values[2]};
  EXPECT_THAT(actual, ElementsAre(10, -20, 30));
}

TEST_F(IarrayTest, NewIarrayAllocatesWithoutCopyingWhenValuesNull) {
  NC_iarray* arr = NC_new_iarray(4, nullptr);
  ASSERT_THAT(arr, NotNull());
  absl::Cleanup free_arr = [arr] {
    EXPECT_THAT(NC_free_iarray(arr), Eq(SUCCEED));
  };

  EXPECT_THAT(arr->count, Eq(4u));
  EXPECT_THAT(arr->values, NotNull());
}

TEST_F(IarrayTest, FreeIarrayAcceptsNull) {
  EXPECT_THAT(NC_free_iarray(nullptr), Eq(SUCCEED));
}

TEST_F(IarrayTest, XlenIarrayCalculatesSizes) {
  EXPECT_THAT(NC_xlen_iarray(nullptr), Eq(4));

  NC_iarray* arr0 = NC_new_iarray(0, nullptr);
  ASSERT_THAT(arr0, NotNull());
  absl::Cleanup free_arr0 = [arr0] { (void)NC_free_iarray(arr0); };
  EXPECT_THAT(NC_xlen_iarray(arr0), Eq(4));

  constexpr std::array<int, 5> kVals = {1, 2, 3, 4, 5};
  NC_iarray* arr5 =
      NC_new_iarray(static_cast<unsigned>(kVals.size()), kVals.data());
  ASSERT_THAT(arr5, NotNull());
  absl::Cleanup free_arr5 = [arr5] { (void)NC_free_iarray(arr5); };
  EXPECT_THAT(NC_xlen_iarray(arr5), Eq(24));  // 4 + 5 * 4
}

TEST_F(IarrayTest, XdrIarrayRoundTripEncodeDecodeAndFree) {
  const std::string file_path = GetTempFilePath("iarray_test.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // 1. Encode an empty array followed by a 3-element array.
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

    NC_iarray* arr_empty = NC_new_iarray(0, nullptr);
    ASSERT_THAT(arr_empty, NotNull());
    EXPECT_THAT(xdr_NC_iarray(&xdrs, &arr_empty), Eq(TRUE));
    EXPECT_THAT(NC_free_iarray(arr_empty), Eq(SUCCEED));

    constexpr std::array<int, 3> kValues = {100, -200, 300};
    NC_iarray* arr_pop =
        NC_new_iarray(static_cast<unsigned>(kValues.size()), kValues.data());
    ASSERT_THAT(arr_pop, NotNull());
    EXPECT_THAT(xdr_NC_iarray(&xdrs, &arr_pop), Eq(TRUE));
    EXPECT_THAT(NC_free_iarray(arr_pop), Eq(SUCCEED));
  }

  // 2. Decode both back and verify.
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

    NC_iarray* dec_empty = nullptr;
    EXPECT_THAT(xdr_NC_iarray(&xdrs, &dec_empty), Eq(TRUE));
    ASSERT_THAT(dec_empty, NotNull());
    EXPECT_THAT(dec_empty->count, Eq(0u));
    EXPECT_THAT(dec_empty->values, IsNull());
    EXPECT_THAT(NC_free_iarray(dec_empty), Eq(SUCCEED));

    NC_iarray* dec_pop = nullptr;
    EXPECT_THAT(xdr_NC_iarray(&xdrs, &dec_pop), Eq(TRUE));
    ASSERT_THAT(dec_pop, NotNull());
    EXPECT_THAT(dec_pop->count, Eq(3u));
    ASSERT_THAT(dec_pop->values, NotNull());
    const std::array<int, 3> actual_dec{dec_pop->values[0], dec_pop->values[1],
                                        dec_pop->values[2]};
    EXPECT_THAT(actual_dec, ElementsAre(100, -200, 300));

    // 3. XDR_FREE frees decoded array.
    xdrs.x_op = XDR_FREE;
    EXPECT_THAT(xdr_NC_iarray(&xdrs, &dec_pop), Eq(TRUE));

    // 4. Invalid operation returns FALSE.
    constexpr enum xdr_op kBogusXdrOp = static_cast<enum xdr_op>(3);
    xdrs.x_op = kBogusXdrOp;
    NC_iarray* dummy = nullptr;
    EXPECT_THAT(xdr_NC_iarray(&xdrs, &dummy), Eq(FALSE));
  }
}

TEST_F(IarrayTest, XdrIarrayDecodeTruncatedCountFails) {
  const std::string file_path = GetTempFilePath("iarray_trunc_count.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Write only 2 bytes (cannot read 4-byte uint for count).
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

  NC_iarray* arr = nullptr;
  EXPECT_THAT(xdr_NC_iarray(&xdrs, &arr), Eq(FALSE));
  EXPECT_THAT(arr, IsNull());
}

TEST_F(IarrayTest, XdrIarrayDecodeTruncatedElementsFails) {
  const std::string file_path = GetTempFilePath("iarray_trunc_elems.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Write count = 2 (in big-endian 4-byte uint: 0, 0, 0, 2), but only 1 element
  // (4 bytes: 0, 0, 0, 42).
  {
    const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ASSERT_THAT(fd, Ne(-1));
    const uint8_t data[] = {0, 0, 0, 2, 0, 0, 0, 42};
    EXPECT_THAT(write(fd, data, sizeof(data)), Eq(8));
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

  NC_iarray* arr = nullptr;
  EXPECT_THAT(xdr_NC_iarray(&xdrs, &arr), Eq(FALSE));
  if (arr != nullptr) {
    EXPECT_THAT(NC_free_iarray(arr), Eq(SUCCEED));
  }
}

TEST_F(IarrayTest, XdrIarrayEncodeFailsOnReadOnlyStream) {
  const std::string file_path = GetTempFilePath("iarray_ro.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };
  {
    const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ASSERT_THAT(fd, Ne(-1));
    close(fd);
  }

  const int fd = open(file_path.c_str(), O_RDONLY);
  ASSERT_THAT(fd, Ne(-1));
  XDR xdrs{};
  const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_ENCODE);
  if (xdrs.x_private == nullptr) {
    close(fd);
  }
  absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
  ASSERT_THAT(create_rc, Ge(0));

  constexpr std::array<int, 2> kValues = {1, 2};
  NC_iarray* arr =
      NC_new_iarray(static_cast<unsigned>(kValues.size()), kValues.data());
  ASSERT_THAT(arr, NotNull());
  absl::Cleanup free_arr = [arr] {
    EXPECT_THAT(NC_free_iarray(arr), Eq(SUCCEED));
  };

  EXPECT_THAT(xdr_NC_iarray(&xdrs, &arr), Eq(FALSE));
}

// TODO(schwehr): Uncovered lines in iarray.c (~91.5% covered) are defensive OOM
// and XDR encode failure branches on truncated streams.

}  // namespace
}  // namespace hdf4
