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
// Tests short integer array XDR serialization (mfhdf/src/sharray.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/sharray.c

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

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
using ::testing::Ne;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

class SharrayTest : public ::testing::Test {
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

TEST_F(SharrayTest, XdrShortsZeroCountReturnsTrue) {
  XDR xdrs{};
  EXPECT_THAT(xdr_shorts(&xdrs, nullptr, 0), Eq(TRUE));
}

TEST_F(SharrayTest, XdrShortsEvenCountRoundTrip) {
  const std::string file_path = GetTempFilePath("sharray_even.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Includes negative values, boundary values, zero, and positive values
  // to exercise sign bit decoding: `if (*cp & 0x80) *sp -= 0x8000;`.
  std::array<short, 4> original = {-32768, -1, 0, 32767};

  // Encode
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

    EXPECT_THAT(xdr_shorts(&xdrs, original.data(),
                           static_cast<unsigned>(original.size())),
                Eq(TRUE));
  }

  // Decode
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

    std::array<short, 4> decoded = {0, 0, 0, 0};
    EXPECT_THAT(xdr_shorts(&xdrs, decoded.data(),
                           static_cast<unsigned>(decoded.size())),
                Eq(TRUE));
    EXPECT_THAT(decoded, ElementsAre(-32768, -1, 0, 32767));
  }
}

TEST_F(SharrayTest, XdrShortsOddCountRoundTrip) {
  const std::string file_path = GetTempFilePath("sharray_odd.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  std::array<short, 5> original = {10, -20, 30, -40, 50};

  // Encode: must use O_RDWR because xdr_NCvshort switches to decode to read
  // the adjoining short before seeking back and writing the encoded short.
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

    EXPECT_THAT(xdr_shorts(&xdrs, original.data(),
                           static_cast<unsigned>(original.size())),
                Eq(TRUE));
  }

  // Decode
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

    std::array<short, 5> decoded = {0, 0, 0, 0, 0};
    EXPECT_THAT(xdr_shorts(&xdrs, decoded.data(),
                           static_cast<unsigned>(decoded.size())),
                Eq(TRUE));
    EXPECT_THAT(decoded, ElementsAre(10, -20, 30, -40, 50));
  }
}

TEST_F(SharrayTest, XdrShortsMultiBlockCountRoundTrip) {
  const std::string file_path = GetTempFilePath("sharray_multiblock.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // NC_NSHRTS_PER is 4096. Use 4100 to trigger the while loop (> 4096)
  // followed by the remaining count (4).
  constexpr unsigned kCount = 4100;
  std::vector<short> original(kCount);
  for (unsigned i = 0; i < kCount; ++i) {
    original[i] = static_cast<short>(i % 30000 - 15000);
  }

  // Encode
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

    EXPECT_THAT(xdr_shorts(&xdrs, original.data(), kCount), Eq(TRUE));
  }

  // Decode
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

    std::vector<short> decoded(kCount, 0);
    EXPECT_THAT(xdr_shorts(&xdrs, decoded.data(), kCount), Eq(TRUE));
    EXPECT_THAT(decoded, Eq(original));
  }
}

TEST_F(SharrayTest, XdrShortsEncodeFailsOnReadOnlyStream) {
  const std::string file_path = GetTempFilePath("sharray_ro.xdr");
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

  // 1. Failure in multi-block loop (> 4096).
  std::vector<short> large_buf(4100, 1);
  EXPECT_THAT(xdr_shorts(&xdrs, large_buf.data(), 4100), Eq(FALSE));

  // 2. Failure in even small block (<= 4096).
  std::array<short, 4> small_buf = {1, 2, 3, 4};
  EXPECT_THAT(xdr_shorts(&xdrs, small_buf.data(), 4), Eq(FALSE));

  // 3. Failure in odd remainder (calling xdr_NCvshort).
  std::array<short, 1> odd_buf = {42};
  EXPECT_THAT(xdr_shorts(&xdrs, odd_buf.data(), 1), Eq(FALSE));
}

TEST_F(SharrayTest, XdrShortsDecodeTruncatedStreamFails) {
  const std::string file_path = GetTempFilePath("sharray_trunc.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Write only 2 bytes (cannot read 4 shorts = 8 bytes).
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

  std::array<short, 4> decoded = {0, 0, 0, 0};
  EXPECT_THAT(xdr_shorts(&xdrs, decoded.data(), 4), Eq(FALSE));
}

// TODO(schwehr): sharray.c has 100% isolated line coverage (45/45 lines).

}  // namespace
}  // namespace hdf4
