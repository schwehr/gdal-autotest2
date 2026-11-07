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
// Tests NetCDF-2 dimension management routines (mfhdf/src/dim.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/dim.c

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
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

class DimTest : public ::testing::Test {
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
    HEclear();
  }

 private:
  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
};

TEST_F(DimTest, NewDimAndFreeDimLifecycleAndRefCount) {
  EXPECT_THAT(NC_free_dim(nullptr), Eq(SUCCEED));

  NC_dim* dim = NC_new_dim("lat", 64);
  ASSERT_THAT(dim, NotNull());
  EXPECT_THAT(dim->count, Eq(1));
  EXPECT_THAT(dim->size, Eq(64));
  ASSERT_THAT(dim->name, NotNull());
  EXPECT_THAT(dim->name->values, StrEq("lat"));

  // Refcount decrement when count > 1
  dim->count = 2;
  EXPECT_THAT(NC_free_dim(dim), Eq(SUCCEED));
  EXPECT_THAT(dim->count, Eq(1));

  // Actual free when count == 1
  EXPECT_THAT(NC_free_dim(dim), Eq(SUCCEED));
}

TEST_F(DimTest, XlenDimCalculatesSizes) {
  NC_dim* null_dim = nullptr;
  EXPECT_THAT(NC_xlen_dim(&null_dim), Eq(4));

  NC_dim* dim = NC_new_dim("dim_four", 100);
  ASSERT_THAT(dim, NotNull());
  absl::Cleanup free_dim = [dim] {
    EXPECT_THAT(NC_free_dim(dim), Eq(SUCCEED));
  };

  // 4 + NC_xlen_string("dim_four" len 8 -> 4 + 8 = 12, padded to 12) = 16
  EXPECT_THAT(NC_xlen_dim(&dim), Eq(16));
}

TEST_F(DimTest, XdrDimRoundTrip) {
  const std::string file_path = GetTempFilePath("dim_test.xdr");
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

    NC_dim* dim = NC_new_dim("level", 32);
    ASSERT_THAT(dim, NotNull());
    EXPECT_THAT(xdr_NC_dim(&xdrs, &dim), Eq(TRUE));
    EXPECT_THAT(NC_free_dim(dim), Eq(SUCCEED));
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

    NC_dim* dec_dim = nullptr;
    EXPECT_THAT(xdr_NC_dim(&xdrs, &dec_dim), Eq(TRUE));
    ASSERT_THAT(dec_dim, NotNull());
    EXPECT_THAT(dec_dim->size, Eq(32));
    ASSERT_THAT(dec_dim->name, NotNull());
    EXPECT_THAT(dec_dim->name->values, StrEq("level"));

    // XDR_FREE frees decoded dim
    xdrs.x_op = XDR_FREE;
    EXPECT_THAT(xdr_NC_dim(&xdrs, &dec_dim), Eq(TRUE));
  }
}

TEST_F(DimTest, XdrDimDecodeTruncatedFails) {
  const std::string file_path = GetTempFilePath("dim_trunc.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Write only 2 bytes
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

  NC_dim* dim = nullptr;
  EXPECT_THAT(xdr_NC_dim(&xdrs, &dim), Eq(FALSE));
  if (dim != nullptr) {
    std::free(dim);
  }
}

TEST_F(DimTest, DimDefAndInquireViaFileHandle) {
  const std::string file_path = GetTempFilePath("dim_file.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  // 1. Invalid size < 0
  EXPECT_THAT(ncdimdef(cdfid, "bad_dim", -5), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // 2. Define first dimension (fixed size)
  const int dim0 = ncdimdef(cdfid, "lat", 180);
  EXPECT_THAT(dim0, Eq(0));

  // 3. Define second dimension (NC_UNLIMITED == 0)
  const int dim1 = ncdimdef(cdfid, "time", NC_UNLIMITED);
  EXPECT_THAT(dim1, Eq(1));

  // 4. Duplicate name fails with NC_ENAMEINUSE
  EXPECT_THAT(ncdimdef(cdfid, "lat", 90), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_ENAMEINUSE));

  // 5. Second unlimited dimension fails with NC_EUNLIMIT
  EXPECT_THAT(ncdimdef(cdfid, "time2", NC_UNLIMITED), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EUNLIMIT));

  // Exceeding maximum dimensions triggers NC_EMAXDIMS
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());
  {
    const unsigned orig_count = handle->dims->count;
    absl::Cleanup restore_count = [handle, orig_count] {
      handle->dims->count = orig_count;
    };
    handle->dims->count = H4_MAX_NC_DIMS;
    EXPECT_THAT(ncdimdef(cdfid, "excess_dim", 10), Eq(-1));
    EXPECT_THAT(ncerr, Eq(NC_EMAXDIMS));
  }

  // 6. NC_dimid lookup
  char lat_name[] = "lat";
  char time_name[] = "time";
  char missing_name[] = "nonexistent";
  EXPECT_THAT(NC_dimid(handle, lat_name), Eq(0));
  EXPECT_THAT(NC_dimid(handle, time_name), Eq(1));
  EXPECT_THAT(NC_dimid(handle, missing_name), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADDIM));

  // 7. ncdiminq valid queries
  char name_buf[H4_MAX_NC_NAME + 1] = {0};
  long size_val = -1;
  EXPECT_THAT(ncdiminq(cdfid, 0, name_buf, &size_val), Eq(0));
  EXPECT_THAT(std::string_view(name_buf), Eq("lat"));
  EXPECT_THAT(size_val, Eq(180));

  // Query unlimited dimension size (returns handle->numrecs)
  EXPECT_THAT(ncdiminq(cdfid, 1, nullptr, &size_val), Eq(1));
  EXPECT_THAT(size_val, Eq(0));

  // Query with null sizep
  EXPECT_THAT(ncdiminq(cdfid, 0, name_buf, nullptr), Eq(0));

  // 8. ncdiminq invalid dimid
  EXPECT_THAT(ncdiminq(cdfid, 99, name_buf, &size_val), Eq(-1));
  EXPECT_THAT(ncdiminq(-1, 0, name_buf, &size_val), Eq(-1));

  // 9. Cannot define dim when not in define mode
  EXPECT_THAT(ncendef(cdfid), Eq(0));
  EXPECT_THAT(ncdimdef(cdfid, "extra", 10), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_ENOTINDEFINE));
}

TEST_F(DimTest, InquireOnHandleWithNullDimsReturnsFailure) {
  const std::string file_path = GetTempFilePath("dim_empty.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  // File has no dimensions defined
  long size_val = -1;
  EXPECT_THAT(ncdiminq(cdfid, 0, nullptr, &size_val), Eq(-1));
}

// TODO(schwehr): Uncovered lines in dim.c (~90% covered) are defensive OOM
// (NC_new_dim alloc failure) and XDR alloc/encode failure branches.

}  // namespace
}  // namespace hdf4
