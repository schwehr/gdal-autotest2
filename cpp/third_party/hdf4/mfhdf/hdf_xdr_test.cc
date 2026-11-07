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
// Tests POSIX buffered XDR serialization routines (mfhdf/src/hdf_xdr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/hdf_xdr.c

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

class HdfXdrTest : public ::testing::Test {
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

TEST_F(HdfXdrTest, IntAndUIntRoundTripAndOperations) {
  const std::string file_path = GetTempFilePath("hdf_xdr_int.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

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

    int val_i = -123456789;
    unsigned val_u = 3000000000u;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &val_i), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &val_u), Eq(TRUE));
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

    int dec_i = 0;
    unsigned dec_u = 0;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &dec_i), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &dec_u), Eq(TRUE));
    EXPECT_THAT(dec_i, Eq(-123456789));
    EXPECT_THAT(dec_u, Eq(3000000000u));

    // XDR_FREE operation returns TRUE
    xdrs.x_op = XDR_FREE;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &dec_i), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &dec_u), Eq(TRUE));

    // Invalid op returns FALSE
    constexpr enum xdr_op kBogusXdrOp = static_cast<enum xdr_op>(3);
    xdrs.x_op = kBogusXdrOp;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &dec_i), Eq(FALSE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &dec_u), Eq(FALSE));
  }
}

TEST_F(HdfXdrTest, FloatAndDoubleRoundTripAndOperations) {
  const std::string file_path = GetTempFilePath("hdf_xdr_float.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

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

    float val_f = 3.14159f;
    double val_d = -2.718281828459045;
    EXPECT_THAT(hdf_xdr_float(&xdrs, &val_f), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_double(&xdrs, &val_d), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_double(&xdrs, nullptr), Eq(FALSE));
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

    float dec_f = 0.0f;
    double dec_d = 0.0;
    EXPECT_THAT(hdf_xdr_float(&xdrs, &dec_f), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_double(&xdrs, &dec_d), Eq(TRUE));
    EXPECT_THAT(dec_f, Eq(3.14159f));
    EXPECT_THAT(dec_d, Eq(-2.718281828459045));
    EXPECT_THAT(hdf_xdr_double(&xdrs, nullptr), Eq(FALSE));

    // XDR_FREE operation returns TRUE
    xdrs.x_op = XDR_FREE;
    EXPECT_THAT(hdf_xdr_float(&xdrs, &dec_f), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_double(&xdrs, &dec_d), Eq(TRUE));

    // Invalid op returns FALSE
    constexpr enum xdr_op kBogusXdrOp = static_cast<enum xdr_op>(3);
    xdrs.x_op = kBogusXdrOp;
    EXPECT_THAT(hdf_xdr_float(&xdrs, &dec_f), Eq(FALSE));
    EXPECT_THAT(hdf_xdr_double(&xdrs, &dec_d), Eq(FALSE));
  }
}

TEST_F(HdfXdrTest, OpaqueOperationsBothAlignedAndPadded) {
  const std::string file_path = GetTempFilePath("hdf_xdr_opaque.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Zero-length returns TRUE immediately.
  {
    XDR xdrs{};
    EXPECT_THAT(hdf_xdr_opaque(&xdrs, nullptr, 0), Eq(TRUE));
  }

  // Encode aligned (4 bytes) and non-aligned (5 bytes, requiring 3 bytes
  // padding)
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

    char aligned[4] = {'a', 'b', 'c', 'd'};
    char padded[5] = {'1', '2', '3', '4', '5'};
    EXPECT_THAT(hdf_xdr_opaque(&xdrs, aligned, 4), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_opaque(&xdrs, padded, 5), Eq(TRUE));
  }

  // Decode back
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

    char dec_aligned[4] = {0};
    char dec_padded[5] = {0};
    EXPECT_THAT(hdf_xdr_opaque(&xdrs, dec_aligned, 4), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_opaque(&xdrs, dec_padded, 5), Eq(TRUE));
    EXPECT_THAT(std::string_view(dec_aligned, 4), Eq("abcd"));
    EXPECT_THAT(std::string_view(dec_padded, 5), Eq("12345"));

    // XDR_FREE returns TRUE
    xdrs.x_op = XDR_FREE;
    EXPECT_THAT(hdf_xdr_opaque(&xdrs, dec_aligned, 4), Eq(TRUE));

    // Invalid op returns FALSE
    constexpr enum xdr_op kBogusXdrOp = static_cast<enum xdr_op>(3);
    xdrs.x_op = kBogusXdrOp;
    EXPECT_THAT(hdf_xdr_opaque(&xdrs, dec_aligned, 4), Eq(FALSE));
  }
}

TEST_F(HdfXdrTest, CountedBytesRoundTripAndAllocations) {
  const std::string file_path = GetTempFilePath("hdf_xdr_bytes.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Encode zero-length bytes, followed by populated bytes
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

    unsigned size0 = 0;
    char* ptr0 = nullptr;
    EXPECT_THAT(hdf_xdr_bytes(&xdrs, &ptr0, &size0, 10), Eq(TRUE));

    char payload[] = "hello_world";
    char* ptr_payload = payload;
    unsigned size_payload = static_cast<unsigned>(sizeof(payload));
    EXPECT_THAT(hdf_xdr_bytes(&xdrs, &ptr_payload, &size_payload, 100),
                Eq(TRUE));

    // nodesize > maxsize fails
    EXPECT_THAT(hdf_xdr_bytes(&xdrs, &ptr_payload, &size_payload, 5),
                Eq(FALSE));
  }

  // Decode back
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

    // 1. Decode zero length
    unsigned dec_size0 = 99;
    char* dec_ptr0 = nullptr;
    EXPECT_THAT(hdf_xdr_bytes(&xdrs, &dec_ptr0, &dec_size0, 10), Eq(TRUE));
    EXPECT_THAT(dec_size0, Eq(0u));
    EXPECT_THAT(dec_ptr0, IsNull());

    // 2. Decode with *cpp == nullptr -> allocates buffer
    unsigned dec_size_pop = 0;
    char* dec_ptr_pop = nullptr;
    EXPECT_THAT(hdf_xdr_bytes(&xdrs, &dec_ptr_pop, &dec_size_pop, 100),
                Eq(TRUE));
    EXPECT_THAT(dec_size_pop, Eq(12u));
    ASSERT_THAT(dec_ptr_pop, NotNull());
    EXPECT_THAT(std::string_view(dec_ptr_pop), Eq("hello_world"));

    // 3. XDR_FREE frees *cpp and sets to null
    xdrs.x_op = XDR_FREE;
    EXPECT_THAT(hdf_xdr_bytes(&xdrs, &dec_ptr_pop, &dec_size_pop, 100),
                Eq(TRUE));
    EXPECT_THAT(dec_ptr_pop, IsNull());

    // 4. Invalid op returns FALSE
    constexpr enum xdr_op kBogusXdrOp = static_cast<enum xdr_op>(3);
    xdrs.x_op = kBogusXdrOp;
    EXPECT_THAT(hdf_xdr_bytes(&xdrs, &dec_ptr_pop, &dec_size_pop, 100),
                Eq(FALSE));
  }
}

TEST_F(HdfXdrTest, MultiPageBufferedIoAndSeek) {
  const std::string file_path = GetTempFilePath("hdf_xdr_multipage.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // 25,000 bytes spans 4 pages (8192 bytes each: page 0, 1, 2, 3)
  constexpr unsigned kTotalBytes = 25000;
  std::vector<char> write_buf(kTotalBytes);
  for (unsigned i = 0; i < kTotalBytes; ++i) {
    write_buf[i] = static_cast<char>((i * 7 + 13) % 256);
  }

  // Write multi-page
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

    EXPECT_THAT(hdf_xdr_putbytes(&xdrs, nullptr, 0), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_putbytes(&xdrs, write_buf.data(), kTotalBytes),
                Eq(TRUE));
    EXPECT_THAT(hdf_xdr_getpos(&xdrs), Eq(kTotalBytes));

    // Seek back across pages while dirty (forces dirty page flush)
    EXPECT_THAT(hdf_xdr_setpos(&xdrs, 500), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_getpos(&xdrs), Eq(500u));
    char patch[4] = {'Z', 'Z', 'Z', 'Z'};
    EXPECT_THAT(hdf_xdr_putbytes(&xdrs, patch, 4), Eq(TRUE));
    std::memcpy(&write_buf[500], patch, 4);

    // Seek ahead to page 2 (around 18,000 bytes)
    EXPECT_THAT(hdf_xdr_setpos(&xdrs, 18000), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_getpos(&xdrs), Eq(18000u));
    EXPECT_THAT(hdf_xdr_putbytes(&xdrs, patch, 4), Eq(TRUE));
    std::memcpy(&write_buf[18000], patch, 4);
  }

  // Read back multi-page and verify
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

    EXPECT_THAT(hdf_xdr_getbytes(&xdrs, nullptr, 0), Eq(TRUE));

    std::vector<char> read_buf(kTotalBytes, 0);
    EXPECT_THAT(hdf_xdr_getbytes(&xdrs, read_buf.data(), kTotalBytes),
                Eq(TRUE));
    EXPECT_THAT(read_buf, Eq(write_buf));

    // Seek to arbitrary pages and read isolated blocks
    EXPECT_THAT(hdf_xdr_setpos(&xdrs, 500), Eq(TRUE));
    char patch_read[4] = {0};
    EXPECT_THAT(hdf_xdr_getbytes(&xdrs, patch_read, 4), Eq(TRUE));
    EXPECT_THAT(std::string_view(patch_read, 4), Eq("ZZZZ"));

    EXPECT_THAT(hdf_xdr_setpos(&xdrs, 18000), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_getbytes(&xdrs, patch_read, 4), Eq(TRUE));
    EXPECT_THAT(std::string_view(patch_read, 4), Eq("ZZZZ"));
  }
}

TEST_F(HdfXdrTest, InterleavedReadAndWriteSeekAdjustments) {
  const std::string file_path = GetTempFilePath("hdf_xdr_rw.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
  ASSERT_THAT(fd, Ne(-1));
  XDR xdrs{};
  const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDWR, XDR_DECODE);
  if (xdrs.x_private == nullptr) {
    close(fd);
  }
  absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
  ASSERT_THAT(create_rc, Ge(0));

  // 1. Write some bytes
  xdrs.x_op = XDR_ENCODE;
  std::vector<char> initial(200, 'A');
  EXPECT_THAT(hdf_xdr_putbytes(&xdrs, initial.data(), 200), Eq(TRUE));

  // 2. Seek back and read (exercises nwrote != BIOBUFSIZ seek adjustment in
  // bio_read_page)
  EXPECT_THAT(hdf_xdr_setpos(&xdrs, 50), Eq(TRUE));
  xdrs.x_op = XDR_DECODE;
  char read_buf[20] = {0};
  EXPECT_THAT(hdf_xdr_getbytes(&xdrs, read_buf, 20), Eq(TRUE));
  EXPECT_THAT(std::string_view(read_buf, 20), Eq(std::string(20, 'A')));

  // 3. Write after read (exercises nread != 0 seek adjustment in
  // bio_write_page)
  xdrs.x_op = XDR_ENCODE;
  char write_b[10] = {'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B'};
  EXPECT_THAT(hdf_xdr_putbytes(&xdrs, write_b, 10), Eq(TRUE));
}

TEST_F(HdfXdrTest, SetupNofileAndNullDestruction) {
  // 1. Setup nofile with NC_CREAT -> XDR_ENCODE
  {
    XDR xdrs{};
    hdf_xdr_setup_nofile(&xdrs, NC_CREAT);
    EXPECT_THAT(xdrs.x_op, Eq(XDR_ENCODE));
    EXPECT_THAT(xdrs.x_private, NotNull());
    hdf_xdr_destroy(&xdrs);
  }

  // 2. Setup nofile without NC_CREAT -> XDR_DECODE
  {
    XDR xdrs{};
    hdf_xdr_setup_nofile(&xdrs, 0);
    EXPECT_THAT(xdrs.x_op, Eq(XDR_DECODE));
    EXPECT_THAT(xdrs.x_private, NotNull());
    hdf_xdr_destroy(&xdrs);
  }

  // 3. Destroy with null private is safe
  {
    XDR xdrs{};
    xdrs.x_private = nullptr;
    hdf_xdr_destroy(&xdrs);
  }

  // 4. setpos with null private returns FALSE
  {
    XDR xdrs{};
    xdrs.x_private = nullptr;
    EXPECT_THAT(hdf_xdr_setpos(&xdrs, 10), Eq(FALSE));
  }
}

TEST_F(HdfXdrTest, ReadOnlyStreamWriteFails) {
  const std::string file_path = GetTempFilePath("hdf_xdr_ronly.xdr");
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

  char dummy[4] = {1, 2, 3, 4};
  EXPECT_THAT(hdf_xdr_putbytes(&xdrs, dummy, 4), Eq(FALSE));
  EXPECT_THAT(hdf_xdr_opaque(&xdrs, dummy, 4), Eq(FALSE));
}

TEST_F(HdfXdrTest, CountedBytesDecodeOpaqueFailureFreesAllocatedMemory) {
  const std::string file_path = GetTempFilePath("hdf_xdr_bytes_fail.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Write length = 100 in 4 bytes, but only 2 bytes of payload.
  {
    const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ASSERT_THAT(fd, Ne(-1));
    const uint8_t data[] = {0, 0, 0, 100, 'a', 'b'};
    EXPECT_THAT(write(fd, data, sizeof(data)), Eq(6));
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

  char* p = nullptr;
  unsigned sz = 0;
  // Allocation succeeds, but subsequent opaque read fails, triggering cleanup.
  EXPECT_THAT(hdf_xdr_bytes(&xdrs, &p, &sz, 200), Eq(FALSE));
  EXPECT_THAT(p, IsNull());
}

TEST_F(HdfXdrTest, WriteOnlyStreamMultiPageOperations) {
  const std::string file_path = GetTempFilePath("hdf_xdr_wonly.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  ASSERT_THAT(fd, Ne(-1));
  XDR xdrs{};
  // Test hdf_xdr_create with O_WRONLY mode
  const int create_rc = hdf_xdr_create(&xdrs, fd, O_WRONLY, XDR_ENCODE);
  if (xdrs.x_private == nullptr) {
    close(fd);
  }
  absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
  ASSERT_THAT(create_rc, Eq(0));

  // Write 18,000 bytes spanning 3 pages to trigger bio_read_page on O_WRONLY
  // stream
  std::vector<char> data(18000, 'W');
  EXPECT_THAT(hdf_xdr_putbytes(&xdrs, data.data(), 18000), Eq(TRUE));
}

// TODO(schwehr): Uncovered lines in hdf_xdr.c (~93.0% covered) are defensive
// OOM branches in bio_get_new and short POSIX read/write/lseek I/O failures.

}  // namespace
}  // namespace hdf4
