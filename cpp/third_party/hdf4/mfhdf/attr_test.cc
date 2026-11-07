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
// Tests NetCDF-2 attribute management routines (mfhdf/src/attr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/attr.c

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"

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
using ::testing::Gt;
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

class AttrTest : public ::testing::Test {
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

TEST_F(AttrTest, NewAttrAndFreeAttrLifecycle) {
  EXPECT_THAT(NC_free_attr(nullptr), Eq(SUCCEED));

  constexpr char kValue[] = "sample_value";
  NC_attr* attr = NC_new_attr("units", NC_CHAR,
                              static_cast<unsigned>(sizeof(kValue)), kValue);
  ASSERT_THAT(attr, NotNull());
  ASSERT_THAT(attr->name, NotNull());
  EXPECT_THAT(attr->name->values, StrEq("units"));
  ASSERT_THAT(attr->data, NotNull());
  EXPECT_THAT(attr->data->type, Eq(NC_CHAR));
  EXPECT_THAT(attr->HDFtype, Eq(DFNT_CHAR));

  EXPECT_THAT(NC_free_attr(attr), Eq(SUCCEED));

  // Name exceeding H4_MAX_NC_NAME fails in NC_new_string -> alloc_err.
  // Pins upstream leak: NC_new_attr allocates NC_attr before failing in
  // NC_new_string without freeing the 32-byte struct.
  {
    absl::LeakCheckDisabler disabler;
    const std::string too_long_name(H4_MAX_NC_NAME + 10, 'a');
    EXPECT_THAT(NC_new_attr(too_long_name.c_str(), NC_CHAR, 1, "x"), IsNull());
  }
}

TEST_F(AttrTest, NCcktypeValidatesUserTypes) {
  EXPECT_THAT(NCcktype(NC_BYTE), Eq(TRUE));
  EXPECT_THAT(NCcktype(NC_CHAR), Eq(TRUE));
  EXPECT_THAT(NCcktype(NC_SHORT), Eq(TRUE));
  EXPECT_THAT(NCcktype(NC_LONG), Eq(TRUE));
  EXPECT_THAT(NCcktype(NC_FLOAT), Eq(TRUE));
  EXPECT_THAT(NCcktype(NC_DOUBLE), Eq(TRUE));

  EXPECT_THAT(NCcktype(static_cast<nc_type>(999)), Eq(FALSE));
  EXPECT_THAT(ncerr, Eq(NC_EBADTYPE));
}

TEST_F(AttrTest, XlenAttrCalculatesSizes) {
  NC_attr* null_attr = nullptr;
  EXPECT_THAT(NC_xlen_attr(&null_attr), Eq(4));

  constexpr char kVal[] = "123";
  NC_attr* attr = NC_new_attr("test_attr", NC_CHAR, 3, kVal);
  ASSERT_THAT(attr, NotNull());
  absl::Cleanup free_att = [attr] {
    EXPECT_THAT(NC_free_attr(attr), Eq(SUCCEED));
  };

  // 4 + 12 (aligned "test_attr") + 4 + 4 + 4 (aligned "123") = 28 bytes.
  EXPECT_THAT(NC_xlen_attr(&attr), Eq(28));
}

TEST_F(AttrTest, XdrAttrRoundTripAndDirectDecode) {
  const std::string file_path = GetTempFilePath("attr_test.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // 1. Manually write name string + data array so decode produces populated
  // attr
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

    // Name string
    NC_string* name = NC_new_string(5, "valid");
    EXPECT_THAT(xdr_NC_string(&xdrs, &name), Eq(TRUE));
    EXPECT_THAT(NC_free_string(name), Eq(SUCCEED));

    // Data array: type = NC_LONG, count = 2, values = {100, 200}
    int type = NC_LONG;
    unsigned count = 2;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &type), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &count), Eq(TRUE));
    int l0 = 100, l1 = 200;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &l0), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_int(&xdrs, &l1), Eq(TRUE));
  }

  // 2. Decode back via xdr_NC_attr
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

    NC_attr* dec_attr = nullptr;
    EXPECT_THAT(xdr_NC_attr(&xdrs, &dec_attr), Eq(TRUE));
    ASSERT_THAT(dec_attr, NotNull());
    ASSERT_THAT(dec_attr->name, NotNull());
    EXPECT_THAT(dec_attr->name->values, StrEq("valid"));
    ASSERT_THAT(dec_attr->data, NotNull());
    EXPECT_THAT(dec_attr->data->type, Eq(NC_LONG));
    EXPECT_THAT(dec_attr->data->count, Eq(2u));
    EXPECT_THAT(reinterpret_cast<int32_t*>(dec_attr->data->values)[0], Eq(100));

    // XDR_FREE frees attr
    xdrs.x_op = XDR_FREE;
    EXPECT_THAT(xdr_NC_attr(&xdrs, &dec_attr), Eq(TRUE));
  }
}

TEST_F(AttrTest, XdrAttrDecodeTruncatedFails) {
  const std::string file_path = GetTempFilePath("attr_trunc.xdr");
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

  NC_attr* attr = nullptr;
  EXPECT_THAT(xdr_NC_attr(&xdrs, &attr), Eq(FALSE));
  if (attr != nullptr) {
    std::free(attr);
  }
}

TEST_F(AttrTest, XdrAttrEncodeOperations) {
  const std::string file_path = GetTempFilePath("attr_encode.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  constexpr char kVal[] = "hello";
  NC_attr* attr = NC_new_attr("greeting", NC_CHAR, 5, kVal);
  ASSERT_THAT(attr, NotNull());
  absl::Cleanup free_attr = [attr] {
    EXPECT_THAT(NC_free_attr(attr), Eq(SUCCEED));
  };

  // 1. Encode to O_RDWR stream
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

    EXPECT_THAT(xdr_NC_attr(&xdrs, &attr), Eq(TRUE));
  }

  // 2. Encode to O_RDONLY stream fails
  {
    const int fd = open(file_path.c_str(), O_RDONLY);
    ASSERT_THAT(fd, Ne(-1));
    XDR xdrs{};
    const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_ENCODE);
    if (xdrs.x_private == nullptr) {
      close(fd);
    }
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    ASSERT_THAT(create_rc, Ge(0));

    EXPECT_THAT(xdr_NC_attr(&xdrs, &attr), Eq(FALSE));
  }
}

TEST_F(AttrTest, FindAttrReturnsNullWhenArrayPointerIsNull) {
  NC_array* null_arr = nullptr;
  EXPECT_THAT(NC_findattr(&null_arr, "units"), IsNull());
}

TEST_F(AttrTest, NcAttPutLifecycleAndModifications) {
  const std::string file_path = GetTempFilePath("attr_file.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  // 1. Argument validations
  EXPECT_THAT(ncattput(-1, NC_GLOBAL, "units", NC_CHAR, 1, "K"), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));

  constexpr char kText[] = "Celsius";
  EXPECT_THAT(ncattput(cdfid, NC_GLOBAL, "units", NC_CHAR, -1, kText), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  EXPECT_THAT(
      ncattput(cdfid, NC_GLOBAL, "units", static_cast<nc_type>(999), 7, kText),
      Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADTYPE));

  EXPECT_THAT(ncattput(cdfid, 99, "units", NC_CHAR, 7, kText), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // Define dimensions and variables to test per-variable attributes
  const int dim_id = ncdimdef(cdfid, "dim0", 10);
  ASSERT_THAT(dim_id, Ge(0));
  const int dims[] = {dim_id};
  const int var0 = ncvardef(cdfid, "var0", NC_LONG, 1, dims);
  ASSERT_THAT(var0, Ge(0));
  const int var1 = ncvardef(cdfid, "var1", NC_FLOAT, 1, dims);
  ASSERT_THAT(var1, Ge(0));

  // Attach attributes to var0
  const int32_t var_scale = 10;
  EXPECT_THAT(ncattput(cdfid, var0, "scale", NC_LONG, 1, &var_scale), Eq(0));
  EXPECT_THAT(ncattput(cdfid, var0, "offset", NC_LONG, 1, &var_scale), Eq(1));

  // Name exceeding H4_MAX_NC_NAME when *ap == NULL fails in NC_new_attr and
  // leaks the partial NC_attr allocation upstream.
  const std::string too_long_name(H4_MAX_NC_NAME + 10, 'a');
  {
    absl::LeakCheckDisabler disabler;
    EXPECT_THAT(ncattput(cdfid, var1, too_long_name.c_str(), NC_CHAR, 1, "x"),
                Eq(-1));
  }

  // 2. Define first global attribute
  EXPECT_THAT(ncattput(cdfid, NC_GLOBAL, "units", NC_CHAR, 7, kText), Eq(0));

  // Name exceeding H4_MAX_NC_NAME when *ap != NULL also triggers the upstream
  // NC_new_attr allocation leak.
  {
    absl::LeakCheckDisabler disabler;
    EXPECT_THAT(
        ncattput(cdfid, NC_GLOBAL, too_long_name.c_str(), NC_CHAR, 1, "x"),
        Eq(-1));
  }

  // 3. Define second global attribute (NC_incr_array)
  const int32_t val32 = 42;
  EXPECT_THAT(ncattput(cdfid, NC_GLOBAL, "code", NC_LONG, 1, &val32), Eq(1));

  // 4. In define mode: replace existing attribute (NC_findattr match)
  constexpr char kNewUnits[] = "Kelvin";
  EXPECT_THAT(ncattput(cdfid, NC_GLOBAL, "units", NC_CHAR, 6, kNewUnits),
              Eq(1));

  // 5. Exceed maximum attributes
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());
  NC_attr* dummy_attr = NC_new_attr("existing", NC_CHAR, 1, "a");
  ASSERT_THAT(dummy_attr, NotNull());
  absl::Cleanup free_dummy = [dummy_attr] {
    EXPECT_THAT(NC_free_attr(dummy_attr), Eq(SUCCEED));
  };
  std::vector<NC_attr*> dummy_vec(H4_MAX_NC_ATTRS, dummy_attr);
  NC_array dummy_array{};
  dummy_array.type = NC_ATTRIBUTE;
  dummy_array.count = H4_MAX_NC_ATTRS;
  dummy_array.szof = sizeof(NC_attr*);
  dummy_array.values = reinterpret_cast<uint8_t*>(dummy_vec.data());

  {
    NC_array* const saved_attrs = handle->attrs;
    absl::Cleanup restore_attrs = [handle, saved_attrs] {
      handle->attrs = saved_attrs;
    };
    handle->attrs = &dummy_array;
    EXPECT_THAT(ncattput(cdfid, NC_GLOBAL, "excess_attr", NC_CHAR, 3, "foo"),
                Eq(-1));
    EXPECT_THAT(ncerr, Eq(NC_EMAXATTS));
  }

  // 6. Transition to data mode
  EXPECT_THAT(ncendef(cdfid), Eq(0));

  // 7. In data mode: modify existing attribute in-place with smaller/equal
  // count
  constexpr char kInPlaceUnits[] = "K";
  EXPECT_THAT(ncattput(cdfid, NC_GLOBAL, "units", NC_CHAR, 1, kInPlaceUnits),
              Eq(1));

  // In data mode with NC_HSYNC: syncs header to disk
  {
    const int saved_ft = handle->file_type;
    absl::Cleanup restore_flags = [handle, saved_ft] {
      handle->file_type = saved_ft;
      handle->flags &= ~NC_HSYNC;
    };
    handle->flags |= NC_HSYNC;
    EXPECT_THAT(ncattput(cdfid, NC_GLOBAL, "units", NC_CHAR, 1, "C"), Eq(1));

    // xdr_cdf failure under NC_HSYNC
    handle->file_type = 999;
    EXPECT_THAT(ncattput(cdfid, NC_GLOBAL, "units", NC_CHAR, 1, "F"), Eq(-1));
  }

  // 8. In data mode: modifying existing attribute with larger count fails
  // (cannot grow)
  constexpr char kTooLong[] = "KelvinTemperatureScale";
  EXPECT_THAT(ncattput(cdfid, NC_GLOBAL, "units", NC_CHAR, 22, kTooLong),
              Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_ENOTINDEFINE));

  // 9. In data mode: adding a brand new attribute fails (not in define mode)
  EXPECT_THAT(ncattput(cdfid, NC_GLOBAL, "brand_new", NC_CHAR, 3, "bar"),
              Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_ENOTINDEFINE));

  // In data mode on var1 (which has no attributes allocated, *ap == NULL):
  // Adding attribute fails because not in define mode
  EXPECT_THAT(ncattput(cdfid, var1, "late_attr", NC_CHAR, 1, "x"), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_ENOTINDEFINE));
}

TEST_F(AttrTest, NcAttPutFailsOnReadOnlyFile) {
  const std::string file_path = GetTempFilePath("attr_ro_file.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  {
    const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
    ASSERT_THAT(cdfid, Ge(0));
    EXPECT_THAT(ncendef(cdfid), Eq(0));
    EXPECT_THAT(ncclose(cdfid), Eq(0));
  }

  const int ro_id = ncopen(file_path.c_str(), NC_NOWRITE);
  ASSERT_THAT(ro_id, Ge(0));
  absl::Cleanup close_ro = [ro_id] { (void)ncclose(ro_id); };

  EXPECT_THAT(ncattput(ro_id, NC_GLOBAL, "units", NC_CHAR, 1, "K"), Eq(-1));
  // NC_aput returns -1 when !(handle->flags & NC_RDWR) without calling
  // NCadvise(NC_EPERM, ...), so ncerr remains NC_NOERR.
  EXPECT_THAT(ncerr, Eq(NC_NOERR));
}

// TODO(schwehr): Uncovered lines in attr.c (~91.8% covered) are defensive OOM
// and XDR allocation/encode failure paths.

}  // namespace
}  // namespace hdf4
