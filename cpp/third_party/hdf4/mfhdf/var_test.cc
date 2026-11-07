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
// Tests NetCDF-2 variable management routines (mfhdf/src/var.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/var.c

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

class VarTest : public ::testing::Test {
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

TEST_F(VarTest, NewVarAndFreeVarLifecycle) {
  EXPECT_THAT(NC_free_var(nullptr), Eq(SUCCEED));

  constexpr int kDims[] = {0, 1};
  NC_var* var = NC_new_var("temperature", NC_FLOAT, 2, kDims);
  ASSERT_THAT(var, NotNull());
  ASSERT_THAT(var->name, NotNull());
  EXPECT_THAT(var->name->values, StrEq("temperature"));
  ASSERT_THAT(var->assoc, NotNull());
  EXPECT_THAT(var->assoc->count, Eq(2u));
  EXPECT_THAT(var->type, Eq(NC_FLOAT));
  EXPECT_THAT(var->szof, Eq(sizeof(float)));
  EXPECT_THAT(var->HDFtype, Eq(DFNT_FLOAT32));

  EXPECT_THAT(NC_free_var(var), Eq(SUCCEED));

  // Name exceeding H4_MAX_NC_NAME fails in NC_new_string -> alloc_err.
  // Pins upstream leak: NC_new_var calloc's NC_var before failing in
  // NC_new_string without freeing the struct.
  {
    absl::LeakCheckDisabler disabler;
    const std::string too_long_name(H4_MAX_NC_NAME + 10, 'a');
    EXPECT_THAT(NC_new_var(too_long_name.c_str(), NC_FLOAT, 1, kDims),
                IsNull());
  }
}

TEST_F(VarTest, VarShapeComputesSizesAndValidatesDimensions) {
  NC cdf{};
  cdf.file_type = HDF_FILE;

  // 1. Scalar variable (assoc->count == 0)
  {
    NC_var* scalar_var = NC_new_var("scalar", NC_DOUBLE, 0, nullptr);
    ASSERT_THAT(scalar_var, NotNull());
    scalar_var->cdf = &cdf;
    absl::Cleanup free_var = [scalar_var] {
      EXPECT_THAT(NC_free_var(scalar_var), Eq(SUCCEED));
    };

    EXPECT_THAT(NC_var_shape(scalar_var, nullptr), Eq(0));
    EXPECT_THAT(scalar_var->len, Eq(sizeof(double)));
  }

  // 2. Fixed multi-dimensional variable
  {
    NC_dim* dim0 = NC_new_dim("dim0", 10);
    ASSERT_THAT(dim0, NotNull());
    NC_dim* dim1 = NC_new_dim("dim1", 20);
    ASSERT_THAT(dim1, NotNull());

    NC_dim* dims_arr[] = {dim0, dim1};
    NC_array* dims = NC_new_array(NC_DIMENSION, 2, dims_arr);
    ASSERT_THAT(dims, NotNull());
    absl::Cleanup free_dims = [dims] {
      EXPECT_THAT(NC_free_array(dims), Eq(SUCCEED));
    };

    constexpr int kDimIndices[] = {0, 1};
    NC_var* var = NC_new_var("matrix", NC_LONG, 2, kDimIndices);
    ASSERT_THAT(var, NotNull());
    var->cdf = &cdf;
    absl::Cleanup free_var = [var] {
      EXPECT_THAT(NC_free_var(var), Eq(SUCCEED));
    };

    EXPECT_THAT(NC_var_shape(var, dims), Eq(2));
    EXPECT_THAT(var->shape[0], Eq(10u));
    EXPECT_THAT(var->shape[1], Eq(20u));
    EXPECT_THAT(var->dsizes[0], Eq(20 * sizeof(int32_t)));
    EXPECT_THAT(var->dsizes[1], Eq(sizeof(int32_t)));
    EXPECT_THAT(var->len, Eq(10 * 20 * sizeof(int32_t)));

    // Recompilation reuses/frees existing shape & dsizes
    EXPECT_THAT(NC_var_shape(var, dims), Eq(2));
  }

  // 3. Unlimited record variable at dimension index 0
  {
    NC_dim* unlim_dim = NC_new_dim("time", NC_UNLIMITED);
    ASSERT_THAT(unlim_dim, NotNull());
    NC_dim* fixed_dim = NC_new_dim("space", 5);
    ASSERT_THAT(fixed_dim, NotNull());

    NC_dim* dims_arr[] = {unlim_dim, fixed_dim};
    NC_array* dims = NC_new_array(NC_DIMENSION, 2, dims_arr);
    ASSERT_THAT(dims, NotNull());
    absl::Cleanup free_dims = [dims] {
      EXPECT_THAT(NC_free_array(dims), Eq(SUCCEED));
    };

    constexpr int kDimIndices[] = {0, 1};
    NC_var* rec_var = NC_new_var("record_var", NC_SHORT, 2, kDimIndices);
    ASSERT_THAT(rec_var, NotNull());
    rec_var->cdf = &cdf;
    absl::Cleanup free_var = [rec_var] {
      EXPECT_THAT(NC_free_var(rec_var), Eq(SUCCEED));
    };

    EXPECT_THAT(NC_var_shape(rec_var, dims), Eq(2));
    EXPECT_THAT(rec_var->shape[0],
                Eq(static_cast<unsigned long>(NC_UNLIMITED)));
    EXPECT_THAT(rec_var->shape[1], Eq(5u));
    EXPECT_THAT(rec_var->len, Eq(5 * sizeof(int16_t)));
  }

  // 4. Invalid dimension errors
  {
    NC_dim* dim0 = NC_new_dim("dim0", 10);
    ASSERT_THAT(dim0, NotNull());
    NC_dim* unlim_dim = NC_new_dim("unlim", NC_UNLIMITED);
    ASSERT_THAT(unlim_dim, NotNull());

    NC_dim* dims_arr[] = {dim0, unlim_dim};
    NC_array* dims = NC_new_array(NC_DIMENSION, 2, dims_arr);
    ASSERT_THAT(dims, NotNull());
    absl::Cleanup free_dims = [dims] {
      EXPECT_THAT(NC_free_array(dims), Eq(SUCCEED));
    };

    // Negative dimension ID
    constexpr int kBadDimsNeg[] = {-1};
    NC_var* var_neg = NC_new_var("bad_neg", NC_LONG, 1, kBadDimsNeg);
    ASSERT_THAT(var_neg, NotNull());
    var_neg->cdf = &cdf;
    absl::Cleanup free_neg = [var_neg] {
      EXPECT_THAT(NC_free_var(var_neg), Eq(SUCCEED));
    };
    EXPECT_THAT(NC_var_shape(var_neg, dims), Eq(-1));
    EXPECT_THAT(ncerr, Eq(NC_EBADDIM));

    // Out-of-bounds dimension ID
    constexpr int kBadDimsOob[] = {99};
    NC_var* var_oob = NC_new_var("bad_oob", NC_LONG, 1, kBadDimsOob);
    ASSERT_THAT(var_oob, NotNull());
    var_oob->cdf = &cdf;
    absl::Cleanup free_oob = [var_oob] {
      EXPECT_THAT(NC_free_var(var_oob), Eq(SUCCEED));
    };
    EXPECT_THAT(NC_var_shape(var_oob, dims), Eq(-1));
    EXPECT_THAT(ncerr, Eq(NC_EBADDIM));

    // Unlimited dimension not at index 0 (dims_arr[1] is unlim)
    constexpr int kBadUnlimPos[] = {0, 1};
    NC_var* var_unlim = NC_new_var("bad_unlim", NC_LONG, 2, kBadUnlimPos);
    ASSERT_THAT(var_unlim, NotNull());
    var_unlim->cdf = &cdf;
    absl::Cleanup free_unlim = [var_unlim] {
      EXPECT_THAT(NC_free_var(var_unlim), Eq(SUCCEED));
    };
    EXPECT_THAT(NC_var_shape(var_unlim, dims), Eq(-1));
    EXPECT_THAT(ncerr, Eq(NC_EUNLIMPOS));
  }

  // 5. 4-byte padding round-up for netCDF files (file_type != HDF_FILE)
  {
    NC netcdf_handle{};
    netcdf_handle.file_type = netCDF_FILE;

    NC_dim* dim3 = NC_new_dim("dim3", 3);
    ASSERT_THAT(dim3, NotNull());

    NC_dim* dims_arr[] = {dim3};
    NC_array* dims = NC_new_array(NC_DIMENSION, 1, dims_arr);
    ASSERT_THAT(dims, NotNull());
    absl::Cleanup free_dims = [dims] {
      EXPECT_THAT(NC_free_array(dims), Eq(SUCCEED));
    };

    constexpr int kDimIdx[] = {0};

    // NC_BYTE with len=3 rounded up to 4
    NC_var* byte_var = NC_new_var("b", NC_BYTE, 1, kDimIdx);
    ASSERT_THAT(byte_var, NotNull());
    byte_var->cdf = &netcdf_handle;
    absl::Cleanup free_b = [byte_var] {
      EXPECT_THAT(NC_free_var(byte_var), Eq(SUCCEED));
    };
    EXPECT_THAT(NC_var_shape(byte_var, dims), Eq(1));
    EXPECT_THAT(byte_var->len, Eq(4u));

    // NC_CHAR with len=3 rounded up to 4
    NC_var* char_var = NC_new_var("c", NC_CHAR, 1, kDimIdx);
    ASSERT_THAT(char_var, NotNull());
    char_var->cdf = &netcdf_handle;
    absl::Cleanup free_c = [char_var] {
      EXPECT_THAT(NC_free_var(char_var), Eq(SUCCEED));
    };
    EXPECT_THAT(NC_var_shape(char_var, dims), Eq(1));
    EXPECT_THAT(char_var->len, Eq(4u));

    // NC_SHORT with len=6 rounded up to 8
    NC_var* short_var = NC_new_var("s", NC_SHORT, 1, kDimIdx);
    ASSERT_THAT(short_var, NotNull());
    short_var->cdf = &netcdf_handle;
    absl::Cleanup free_s = [short_var] {
      EXPECT_THAT(NC_free_var(short_var), Eq(SUCCEED));
    };
    EXPECT_THAT(NC_var_shape(short_var, dims), Eq(1));
    EXPECT_THAT(short_var->len, Eq(8u));

    // NC_LONG (multiple of 4, hits default: break)
    NC_var* long_var = NC_new_var("l", NC_LONG, 1, kDimIdx);
    ASSERT_THAT(long_var, NotNull());
    long_var->cdf = &netcdf_handle;
    absl::Cleanup free_l = [long_var] {
      EXPECT_THAT(NC_free_var(long_var), Eq(SUCCEED));
    };
    EXPECT_THAT(NC_var_shape(long_var, dims), Eq(1));
    EXPECT_THAT(long_var->len, Eq(12u));
  }
}

TEST_F(VarTest, NcVarDefAndInquireAndLookup) {
  const std::string file_path = GetTempFilePath("var_test.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  // 1. Argument validations on ncvardef
  constexpr int kDimsZero[] = {0};
  EXPECT_THAT(ncvardef(-1, "v", NC_LONG, 1, kDimsZero), Eq(-1));

  EXPECT_THAT(ncvardef(cdfid, "v", static_cast<nc_type>(999), 1, kDimsZero),
              Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADTYPE));

  EXPECT_THAT(ncvardef(cdfid, "v", NC_LONG, -1, kDimsZero), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // ndims > 0 but handle->dims is NULL
  EXPECT_THAT(ncvardef(cdfid, "v", NC_LONG, 1, kDimsZero), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // Define dimensions
  const int dim0 = ncdimdef(cdfid, "time", NC_UNLIMITED);
  ASSERT_THAT(dim0, Ge(0));
  const int dim1 = ncdimdef(cdfid, "x", 10);
  ASSERT_THAT(dim1, Ge(0));

  // ndims > dims->count (2)
  constexpr int kTooManyDims[] = {0, 1, 0};
  EXPECT_THAT(ncvardef(cdfid, "v", NC_LONG, 3, kTooManyDims), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // Lookup before any variable defined (handle->vars == NULL returns -1 without
  // setting ncerr)
  EXPECT_THAT(ncvarid(cdfid, "pressure"), Eq(-1));

  // 2. Define first variable (handle->vars == NULL branch)
  const int v0_dims[] = {dim0, dim1};
  const int v0 = ncvardef(cdfid, "pressure", NC_FLOAT, 2, v0_dims);
  EXPECT_THAT(v0, Eq(0));

  // 3. Define second variable (NC_incr_array branch)
  const int v1_dims[] = {dim1};
  const int v1 = ncvardef(cdfid, "temperature", NC_FLOAT, 1, v1_dims);
  EXPECT_THAT(v1, Eq(1));

  // 4. Duplicate name fails
  EXPECT_THAT(ncvardef(cdfid, "pressure", NC_FLOAT, 1, v1_dims), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_ENAMEINUSE));

  // 5. Exceed maximum variables
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());
  const unsigned orig_vcount = handle->vars->count;
  handle->vars->count = H4_MAX_NC_VARS;
  EXPECT_THAT(ncvardef(cdfid, "excess", NC_FLOAT, 1, v1_dims), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EMAXVARS));
  handle->vars->count = orig_vcount;

  // 6. NC_var_shape failure inside ncvardef unwinds newly added variable
  const int bad_indices[] = {-1};
  EXPECT_THAT(ncvardef(cdfid, "bad_shape", NC_FLOAT, 1, bad_indices), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADDIM));
  EXPECT_THAT(handle->vars->count, Eq(2u));

  // 7. Add an attribute to v0 to test ncvarinq with attrs != NULL
  const float scale_val = 1.5f;
  EXPECT_THAT(ncattput(cdfid, v0, "scale", NC_FLOAT, 1, &scale_val), Eq(0));

  // 8. ncvarid lookups
  EXPECT_THAT(ncvarid(-1, "pressure"), Eq(-1));
  EXPECT_THAT(ncvarid(cdfid, "pressure"), Eq(0));
  EXPECT_THAT(ncvarid(cdfid, "temperature"), Eq(1));
  EXPECT_THAT(ncvarid(cdfid, "nonexistent"), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_ENOTVAR));

  // 9. NC_hlookupvar
  EXPECT_THAT(NC_hlookupvar(handle, NC_GLOBAL), IsNull());
  EXPECT_THAT(NC_hlookupvar(handle, -1), IsNull());
  EXPECT_THAT(ncerr, Eq(NC_ENOTVAR));
  EXPECT_THAT(NC_hlookupvar(handle, 99), IsNull());
  EXPECT_THAT(ncerr, Eq(NC_ENOTVAR));
  EXPECT_THAT(NC_hlookupvar(handle, 0), NotNull());

  // 10. ncvarinq
  EXPECT_THAT(ncvarinq(-1, 0, nullptr, nullptr, nullptr, nullptr, nullptr),
              Eq(-1));

  char vname[H4_MAX_NC_NAME + 1]{};
  nc_type vtype{};
  int vndims = 0;
  int vdims[H4_MAX_VAR_DIMS]{};
  int vnattrs = 0;

  // Inquiry on v0 (has 1 attribute)
  EXPECT_THAT(ncvarinq(cdfid, 0, vname, &vtype, &vndims, vdims, &vnattrs),
              Eq(0));
  EXPECT_THAT(vname, StrEq("pressure"));
  EXPECT_THAT(vtype, Eq(NC_FLOAT));
  EXPECT_THAT(vndims, Eq(2));
  EXPECT_THAT(vdims[0], Eq(dim0));
  EXPECT_THAT(vdims[1], Eq(dim1));
  EXPECT_THAT(vnattrs, Eq(1));

  // Inquiry on v1 (has 0 attributes, attrs == NULL)
  EXPECT_THAT(ncvarinq(cdfid, 1, nullptr, nullptr, nullptr, nullptr, &vnattrs),
              Eq(1));
  EXPECT_THAT(vnattrs, Eq(0));

  // 11. In data mode: ncvardef fails
  EXPECT_THAT(ncendef(cdfid), Eq(0));
  EXPECT_THAT(ncvardef(cdfid, "late", NC_FLOAT, 1, v1_dims), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_ENOTINDEFINE));
}

TEST_F(VarTest, NcComputeShapesCalculatesRecordSizes) {
  NC handle{};
  handle.file_type = HDF_FILE;

  // 1. vars == NULL returns 0
  EXPECT_THAT(NC_computeshapes(&handle), Eq(0));
  EXPECT_THAT(handle.begin_rec, Eq(0));
  EXPECT_THAT(handle.recsize, Eq(0));

  // Define 1 unlimited dim and 1 fixed dim
  NC_dim* unlim_dim = NC_new_dim("rec", NC_UNLIMITED);
  ASSERT_THAT(unlim_dim, NotNull());

  NC_dim* fixed_dim = NC_new_dim("len", 4);
  ASSERT_THAT(fixed_dim, NotNull());

  NC_dim* dims_arr[] = {unlim_dim, fixed_dim};
  NC_array* dims = NC_new_array(NC_DIMENSION, 2, dims_arr);
  ASSERT_THAT(dims, NotNull());
  absl::Cleanup free_dims = [dims] {
    EXPECT_THAT(NC_free_array(dims), Eq(SUCCEED));
  };
  handle.dims = dims;

  // 2. Exactly one record variable: recsize is packed to *first->dsizes
  constexpr int kRecDims[] = {0, 1};
  NC_var* rec_v0 = NC_new_var("r0", NC_LONG, 2, kRecDims);
  ASSERT_THAT(rec_v0, NotNull());
  rec_v0->cdf = &handle;

  NC_var* vars_one[] = {rec_v0};
  NC_array* vars_arr = NC_new_array(NC_VARIABLE, 1, vars_one);
  ASSERT_THAT(vars_arr, NotNull());
  absl::Cleanup free_vars = [vars_arr] {
    EXPECT_THAT(NC_free_array(vars_arr), Eq(SUCCEED));
  };
  handle.vars = vars_arr;

  EXPECT_THAT(NC_computeshapes(&handle), Eq(1));
  EXPECT_THAT(handle.recsize, Eq(4 * sizeof(int32_t)));

  // 3. Two record variables: grow vars_arr using NC_incr_array
  NC_var* rec_v1 = NC_new_var("r1", NC_SHORT, 2, kRecDims);
  ASSERT_THAT(rec_v1, NotNull());
  rec_v1->cdf = &handle;
  ASSERT_THAT(NC_incr_array(vars_arr, reinterpret_cast<uint8_t*>(&rec_v1)),
              NotNull());

  EXPECT_THAT(NC_computeshapes(&handle), Eq(2));
  EXPECT_THAT(handle.recsize, Eq(4 * sizeof(int32_t) + 4 * sizeof(int16_t)));

  // 4. Invalid variable with negative dim index causes NC_computeshapes to fail
  constexpr int kBadDims[] = {-1};
  NC_var* bad_v = NC_new_var("bad", NC_LONG, 1, kBadDims);
  ASSERT_THAT(bad_v, NotNull());
  bad_v->cdf = &handle;
  ASSERT_THAT(NC_incr_array(vars_arr, reinterpret_cast<uint8_t*>(&bad_v)),
              NotNull());

  EXPECT_THAT(NC_computeshapes(&handle), Eq(-1));
}

TEST_F(VarTest, XlenVarCalculatesSerializedLength) {
  NC_var* null_var = nullptr;
  EXPECT_THAT(NC_xlen_var(&null_var), Eq(4));

  constexpr int kDims[] = {0};
  NC_var* var = NC_new_var("sample", NC_LONG, 1, kDims);
  ASSERT_THAT(var, NotNull());
  absl::Cleanup free_v = [var] { EXPECT_THAT(NC_free_var(var), Eq(SUCCEED)); };

  EXPECT_THAT(NC_xlen_var(&var), Gt(16));
}

TEST_F(VarTest, XdrVarEncodeAndDecodeRoundTrip) {
  const std::string file_path = GetTempFilePath("var_roundtrip.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  constexpr int kDims[] = {0, 1};
  NC_var* orig_var = NC_new_var("velocity", NC_FLOAT, 2, kDims);
  ASSERT_THAT(orig_var, NotNull());
  orig_var->len = 100;
  orig_var->begin = 512;
  absl::Cleanup free_orig = [orig_var] {
    EXPECT_THAT(NC_free_var(orig_var), Eq(SUCCEED));
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

    EXPECT_THAT(xdr_NC_var(&xdrs, &orig_var), Eq(TRUE));
  }

  // 2. Decode from O_RDONLY stream
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

    NC_var* dec_var = nullptr;
    EXPECT_THAT(xdr_NC_var(&xdrs, &dec_var), Eq(TRUE));
    ASSERT_THAT(dec_var, NotNull());
    ASSERT_THAT(dec_var->name, NotNull());
    EXPECT_THAT(dec_var->name->values, StrEq("velocity"));
    ASSERT_THAT(dec_var->assoc, NotNull());
    EXPECT_THAT(dec_var->assoc->count, Eq(2u));
    EXPECT_THAT(dec_var->assoc->values[0], Eq(0));
    EXPECT_THAT(dec_var->assoc->values[1], Eq(1));
    // In upstream xdr_NC_var, temp_type and temp_len are not initialized from
    // (*vpp) during XDR_ENCODE, so type and len are encoded as 0.
    EXPECT_THAT(dec_var->type, Eq(static_cast<nc_type>(0)));
    EXPECT_THAT(dec_var->len, Eq(0u));
    EXPECT_THAT(dec_var->begin, Eq(512u));
    EXPECT_THAT(dec_var->szof, Eq(0u));
    EXPECT_THAT(dec_var->HDFtype, Eq(DFNT_CHAR));

    // XDR_FREE frees decoded var
    xdrs.x_op = XDR_FREE;
    EXPECT_THAT(xdr_NC_var(&xdrs, &dec_var), Eq(TRUE));
  }

  // 3. Encode to O_RDONLY stream fails
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

    EXPECT_THAT(xdr_NC_var(&xdrs, &orig_var), Eq(FALSE));
  }
}

TEST_F(VarTest, XdrVarTruncatedDecodeFails) {
  const std::string file_path = GetTempFilePath("var_trunc.xdr");
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

  NC_var* var = nullptr;
  EXPECT_THAT(xdr_NC_var(&xdrs, &var), Eq(FALSE));
  if (var != nullptr) {
    std::free(var);
  }
}

// TODO(schwehr): Uncovered lines in var.c (~91.6% covered) are defensive OOM
// and XDR allocation/encode failure branches.

}  // namespace
}  // namespace hdf4
