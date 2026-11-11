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

// Unit tests for //third_party/netcdf/libdispatch/dv2i.c (NetCDF V2 API and
// record-level convenience helpers).
//
// Covers nc_advise / ncopts / ncerr handling, dataset lifecycle, dimensions,
// variables (scalar/1D/2D, single/array/strided/mapped I/O), attributes, and
// both V2 (`ncrec*`) and V3-internal (`nc_*_rec`) record APIs across valid,
// null-pointer, and error paths.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dv2i.c

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

extern "C" {
#include "third_party/netcdf/include/nc3internal.h"
}  // extern "C"

namespace netcdf {
namespace {

using ::testing::ElementsAre;
using ::testing::ExitedWithCode;

TEST(Dv2iTest, AdviseSetsNcerrAndHonorsVerboseAndFatalOpts) {
  EXPECT_EQ(NC_FATAL | NC_VERBOSE, ncopts);
  EXPECT_EQ(NC_NOERR, ncerr);

  {
    ScopedNcV2Opts opts(0);
    nc_advise("ok_op", NC_NOERR, "x=%d", 1);
    EXPECT_EQ(NC_NOERR, ncerr);

    nc_advise("err_op", NC_EBADID, "id=%d", -1);
    EXPECT_EQ(NC_EBADID, ncerr);

    // Positive errno codes map to NC_SYSERR via NC_ISSYSERR().
    nc_advise("sys_op", ENOENT, "path=%s", "missing");
    EXPECT_EQ(NC_SYSERR, ncerr);

    // NC_FATAL with NC_NOERR must not exit.
    ncopts = NC_FATAL;
    nc_advise("fatal_ok", NC_NOERR, "ok=%d", 2);
    EXPECT_EQ(NC_NOERR, ncerr);
  }

  // Verify NC_VERBOSE emits formatted diagnostics to stderr only when set.
  constexpr std::string_view kChildArgs[] = {"dv2i_advise"};
  const CliResult verbose_res = RunCliInForkedChild(
      [](int /*argc*/, char** /*argv*/) -> int {
        ScopedNcV2Opts child_opts(0);
        nc_advise("silent_err", NC_EINVAL, "ignored=%d", 1);

        ncopts = NC_VERBOSE;
        nc_advise("verbose_ok", NC_NOERR, "val=%d", 7);
        nc_advise("verbose_err", NC_EBADID, "id=%d", -1);
        return 0;
      },
      kChildArgs);
  ASSERT_TRUE(verbose_res.ok()) << verbose_res.harness_error;
  EXPECT_EQ(0, verbose_res.exit_code);
  EXPECT_EQ(
      "verbose_ok: val=7\n"
      "verbose_err: id=-1: NetCDF: Not a valid ID\n",
      verbose_res.stderr_output);

  // Verify NC_FATAL calls exit(ncopts) in isolated threadsafe death-test
  // children.
  struct DeathTestStyleGuard {
    std::string prev = GTEST_FLAG_GET(death_test_style);
    DeathTestStyleGuard() { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
    ~DeathTestStyleGuard() { GTEST_FLAG_SET(death_test_style, prev); }
  } style_guard;

  EXPECT_EXIT(
      {
        ncopts = NC_FATAL | NC_VERBOSE;
        nc_advise("fatal_err", NC_EINVAL, "arg=%d", 99);
      },
      ExitedWithCode(NC_FATAL | NC_VERBOSE),
      "fatal_err: arg=99: NetCDF: Invalid argument");

  EXPECT_EXIT(
      {
        ncopts = NC_FATAL;
        ncclose(-1);
      },
      ExitedWithCode(NC_FATAL), "");
}

TEST(Dv2iTest, FileLifecycleCreateOpenRedefEndefSyncCloseAndAbort) {
  ScopedNcV2Opts opts(0);
  TempFile nc_file("dv2i_lifecycle.nc");

  // Create on disk, define schema, test define-mode vs data-mode constraints,
  // write data, sync, redef+abort rollback, then reopen and verify persistence.
  {
    ScopedNcFile nc(nccreate(nc_file.c_str(), NC_CLOBBER));
    ASSERT_GE(nc.get(), 0);

    const int dimid = ncdimdef(nc.get(), "x", 3L);
    ASSERT_GE(dimid, 0);
    const int varid = ncvardef(nc.get(), "v", NC_INT, 1, &dimid);
    ASSERT_GE(varid, 0);
    EXPECT_EQ(0, ncattput(nc.get(), NC_GLOBAL, "title", NC_CHAR, 4, "demo"));

    // Cannot sync or re-enter define mode while already in define mode.
    EXPECT_EQ(NC2_ERR, ncsync(nc.get()));
    EXPECT_EQ(NC_EINDEFINE, ncerr);
    EXPECT_EQ(NC2_ERR, ncredef(nc.get()));
    EXPECT_EQ(NC_EINDEFINE, ncerr);

    EXPECT_EQ(0, ncendef(nc.get()));

    // Already in data mode -> ncendef fails.
    EXPECT_EQ(NC2_ERR, ncendef(nc.get()));
    EXPECT_EQ(NC_ENOTINDEFINE, ncerr);

    const std::array<int32_t, 3> values = {10, 20, 30};
    const long start = 0;
    const long count = 3;
    EXPECT_EQ(0, ncvarput(nc.get(), varid, &start, &count, values.data()));
    EXPECT_EQ(0, ncsync(nc.get()));

    // Enter redef, stage a transient attribute, and roll it back via ncabort.
    EXPECT_EQ(0, ncredef(nc.get()));
    EXPECT_EQ(0, ncattput(nc.get(), NC_GLOBAL, "transient", NC_INT, 1,
                          values.data()));
    const int abort_rc = ncabort(nc.get());
    nc.release();
    EXPECT_EQ(0, abort_rc);
  }

  // Creating with NC_NOCLOBBER over an existing file must fail.
  EXPECT_EQ(NC2_ERR, nccreate(nc_file.c_str(), NC_NOCLOBBER));
  EXPECT_EQ(NC_EEXIST, ncerr);

  // Reopen read-only and verify rolled-back state and read-only protections.
  {
    ScopedNcFile nc(ncopen(nc_file.c_str(), NC_NOWRITE));
    ASSERT_GE(nc.get(), 0);

    nc_type att_type = NC_NAT;
    int att_len = -1;
    EXPECT_EQ(NC2_ERR,
              ncattinq(nc.get(), NC_GLOBAL, "transient", &att_type, &att_len));
    EXPECT_EQ(NC_ENOTATT, ncerr);
    EXPECT_EQ(1, ncattinq(nc.get(), NC_GLOBAL, "title", &att_type, &att_len));
    EXPECT_EQ(NC_CHAR, att_type);
    EXPECT_EQ(4, att_len);

    const int varid = ncvarid(nc.get(), "v");
    ASSERT_GE(varid, 0);
    std::vector<int32_t> read_back(3, -999);
    const long start = 0;
    const long count = 3;
    EXPECT_EQ(0, ncvarget(nc.get(), varid, &start, &count, read_back.data()));
    EXPECT_THAT(read_back, ElementsAre(10, 20, 30));

    EXPECT_EQ(NC2_ERR, ncredef(nc.get()));
    EXPECT_EQ(NC_EPERM, ncerr);

    const int close_rc = ncclose(nc.get());
    nc.release();
    EXPECT_EQ(0, close_rc);
  }

  // Opening a missing file maps ENOENT -> NC_SYSERR.
  TempDir missing_dir("dv2i_missing_dir");
  EXPECT_EQ(NC2_ERR,
            ncopen(missing_dir.child("nonexistent.nc").c_str(), NC_NOWRITE));
  EXPECT_EQ(NC_SYSERR, ncerr);

  // Invalid ncid checks for lifecycle and inquiry functions.
  EXPECT_EQ(NC2_ERR, ncsync(-1));
  EXPECT_EQ(NC_EBADID, ncerr);
  EXPECT_EQ(NC2_ERR, ncclose(-1));
  EXPECT_EQ(NC_EBADID, ncerr);
  EXPECT_EQ(NC2_ERR, ncabort(-1));
  EXPECT_EQ(NC_EBADID, ncerr);

  int ndims = -1;
  int nvars = -1;
  int natts = -1;
  int recdim = -1;
  EXPECT_EQ(NC2_ERR, ncinquire(-1, &ndims, &nvars, &natts, &recdim));
  EXPECT_EQ(NC_EBADID, ncerr);
}

TEST(Dv2iTest, InquireFillModeAndDimensionOperations) {
  ScopedNcV2Opts opts(0);
  ScopedNcFile nc(nccreate("dv2i_dims.nc", NC_CLOBBER | NC_DISKLESS));
  ASSERT_GE(nc.get(), 0);

  // Fill mode transitions and error checks.
  EXPECT_EQ(NC_FILL, ncsetfill(nc.get(), NC_NOFILL));
  EXPECT_EQ(NC_NOFILL, ncsetfill(nc.get(), NC_FILL));
  EXPECT_EQ(NC2_ERR, ncsetfill(nc.get(), 0x7777));
  EXPECT_EQ(NC_EINVAL, ncerr);
  EXPECT_EQ(NC2_ERR, ncsetfill(-1, NC_FILL));
  EXPECT_EQ(NC_EBADID, ncerr);

  // Negative dimension size fails before reaching nc_def_dim.
  EXPECT_EQ(NC2_ERR, ncdimdef(nc.get(), "neg", -1L));
  EXPECT_EQ(NC_EDIMSIZE, ncerr);

  const int unlim_id = ncdimdef(nc.get(), "time", NC_UNLIMITED);
  ASSERT_GE(unlim_id, 0);
  const int lat_id = ncdimdef(nc.get(), "lat", 4L);
  ASSERT_GE(lat_id, 0);

  // Duplicate dimension name fails.
  EXPECT_EQ(NC2_ERR, ncdimdef(nc.get(), "lat", 2L));
  EXPECT_EQ(NC_ENAMEINUSE, ncerr);

  // ncdimid success and error.
  EXPECT_EQ(lat_id, ncdimid(nc.get(), "lat"));
  EXPECT_EQ(NC2_ERR, ncdimid(nc.get(), "no_such_dim"));
  EXPECT_EQ(NC_EBADDIM, ncerr);

  // ncdiminq with non-NULL pointers, NULL pointers, and invalid dimid.
  char dim_name[NC_MAX_NAME + 1]{};
  long dim_len = -1;
  EXPECT_EQ(lat_id, ncdiminq(nc.get(), lat_id, dim_name, &dim_len));
  EXPECT_STREQ("lat", dim_name);
  EXPECT_EQ(4L, dim_len);
  EXPECT_EQ(unlim_id, ncdiminq(nc.get(), unlim_id, nullptr, nullptr));
  EXPECT_EQ(NC2_ERR, ncdiminq(nc.get(), 99, dim_name, &dim_len));
  EXPECT_EQ(NC_EBADDIM, ncerr);

  // ncdimrename success and error.
  EXPECT_EQ(lat_id, ncdimrename(nc.get(), lat_id, "latitude"));
  EXPECT_EQ(lat_id, ncdimid(nc.get(), "latitude"));
  EXPECT_EQ(NC2_ERR, ncdimrename(nc.get(), 99, "bad"));
  EXPECT_EQ(NC_EBADDIM, ncerr);

  // ncinquire returns ncid on success; test both non-NULL and NULL out-args.
  const int v_id = ncvardef(nc.get(), "temp", NC_FLOAT, 1, &lat_id);
  ASSERT_GE(v_id, 0);
  EXPECT_EQ(0, ncattput(nc.get(), NC_GLOBAL, "g", NC_INT, 1, &lat_id));

  int ndims = -1;
  int nvars = -1;
  int natts = -1;
  int recdim = -1;
  EXPECT_EQ(nc.get(), ncinquire(nc.get(), &ndims, &nvars, &natts, &recdim));
  EXPECT_EQ(2, ndims);
  EXPECT_EQ(1, nvars);
  EXPECT_EQ(1, natts);
  EXPECT_EQ(unlim_id, recdim);
  EXPECT_EQ(nc.get(), ncinquire(nc.get(), nullptr, nullptr, nullptr, nullptr));
}

TEST(Dv2iTest, VariableDefinitionInquiryRenameAndAllAccessModes) {
  ScopedNcV2Opts opts(0);
  ScopedNcFile nc(nccreate("dv2i_vars.nc", NC_CLOBBER | NC_DISKLESS));
  ASSERT_GE(nc.get(), 0);

  const int rows_dim = ncdimdef(nc.get(), "rows", 3L);
  const int cols_dim = ncdimdef(nc.get(), "cols", 4L);
  ASSERT_GE(rows_dim, 0);
  ASSERT_GE(cols_dim, 0);

  const int dims_2d[2] = {rows_dim, cols_dim};
  const int v_vec = ncvardef(nc.get(), "vec", NC_DOUBLE, 1, &cols_dim);
  const int v_mat = ncvardef(nc.get(), "mat", NC_INT, 2, dims_2d);
  const int v_scalar = ncvardef(nc.get(), "scalar", NC_FLOAT, 0, nullptr);
  ASSERT_GE(v_vec, 0);
  ASSERT_GE(v_mat, 0);
  ASSERT_GE(v_scalar, 0);

  // Duplicate variable name fails.
  EXPECT_EQ(NC2_ERR, ncvardef(nc.get(), "vec", NC_INT, 1, &cols_dim));
  EXPECT_EQ(NC_ENAMEINUSE, ncerr);

  // ncvarid success and error.
  EXPECT_EQ(v_mat, ncvarid(nc.get(), "mat"));
  EXPECT_EQ(NC2_ERR, ncvarid(nc.get(), "missing_var"));
  EXPECT_EQ(NC_ENOTVAR, ncerr);

  // ncvarinq with all pointers non-NULL, all NULL, and invalid varid.
  char var_name[NC_MAX_NAME + 1]{};
  nc_type var_type = NC_NAT;
  int var_ndims = -1;
  int var_dims[NC_MAX_VAR_DIMS]{};
  int var_natts = -1;
  EXPECT_EQ(v_mat, ncvarinq(nc.get(), v_mat, var_name, &var_type, &var_ndims,
                            var_dims, &var_natts));
  EXPECT_STREQ("mat", var_name);
  EXPECT_EQ(NC_INT, var_type);
  EXPECT_EQ(2, var_ndims);
  EXPECT_EQ(rows_dim, var_dims[0]);
  EXPECT_EQ(cols_dim, var_dims[1]);
  EXPECT_EQ(0, var_natts);
  EXPECT_EQ(v_mat, ncvarinq(nc.get(), v_mat, nullptr, nullptr, nullptr, nullptr,
                            nullptr));
  EXPECT_EQ(NC2_ERR, ncvarinq(nc.get(), 99, var_name, &var_type, &var_ndims,
                              var_dims, &var_natts));
  EXPECT_EQ(NC_ENOTVAR, ncerr);

  // ncvarrename success and error.
  EXPECT_EQ(v_vec, ncvarrename(nc.get(), v_vec, "vector"));
  EXPECT_EQ(v_vec, ncvarid(nc.get(), "vector"));
  EXPECT_EQ(NC2_ERR, ncvarrename(nc.get(), 99, "nope"));
  EXPECT_EQ(NC_ENOTVAR, ncerr);

  ASSERT_EQ(0, ncendef(nc.get()));

  // Single-element I/O (ncvarput1 / ncvarget1) on scalar and 1D variables.
  const float scalar_in = 42.5F;
  float scalar_out = -999.0F;
  EXPECT_EQ(0, ncvarput1(nc.get(), v_scalar, nullptr, &scalar_in));
  EXPECT_EQ(0, ncvarget1(nc.get(), v_scalar, nullptr, &scalar_out));
  EXPECT_FLOAT_EQ(42.5F, scalar_out);

  const long idx_valid = 2;
  const double d_in = 9.25;
  double d_out = -999.0;
  EXPECT_EQ(0, ncvarput1(nc.get(), v_vec, &idx_valid, &d_in));
  EXPECT_EQ(0, ncvarget1(nc.get(), v_vec, &idx_valid, &d_out));
  EXPECT_DOUBLE_EQ(9.25, d_out);

  const long idx_oob = 99;
  EXPECT_EQ(NC2_ERR, ncvarput1(nc.get(), v_vec, &idx_oob, &d_in));
  EXPECT_EQ(NC_EINVALCOORDS, ncerr);
  EXPECT_EQ(NC2_ERR, ncvarget1(nc.get(), v_vec, &idx_oob, &d_out));
  EXPECT_EQ(NC_EINVALCOORDS, ncerr);

  // Contiguous array I/O (ncvarput / ncvarget).
  const std::array<double, 4> vec_in = {1.0, 2.0, 3.0, 4.0};
  std::vector<double> vec_out(4, -999.0);
  const long v_start = 0;
  const long v_count = 4;
  EXPECT_EQ(0, ncvarput(nc.get(), v_vec, &v_start, &v_count, vec_in.data()));
  EXPECT_EQ(0, ncvarget(nc.get(), v_vec, &v_start, &v_count, vec_out.data()));
  EXPECT_THAT(vec_out, ElementsAre(1.0, 2.0, 3.0, 4.0));

  const long v_count_oob = 99;
  EXPECT_EQ(NC2_ERR,
            ncvarput(nc.get(), v_vec, &v_start, &v_count_oob, vec_in.data()));
  EXPECT_EQ(NC_EEDGE, ncerr);
  EXPECT_EQ(NC2_ERR,
            ncvarget(nc.get(), v_vec, &v_start, &v_count_oob, vec_out.data()));
  EXPECT_EQ(NC_EEDGE, ncerr);

  // Strided I/O (ncvarputs / ncvargets): NULL stride fallback and non-NULL
  // stride.
  const std::array<double, 4> vec_base = {10.0, 20.0, 30.0, 40.0};
  std::vector<double> vec_null_stride(4, -999.0);
  EXPECT_EQ(0, ncvarputs(nc.get(), v_vec, &v_start, &v_count, nullptr,
                         vec_base.data()));
  EXPECT_EQ(0, ncvargets(nc.get(), v_vec, &v_start, &v_count, nullptr,
                         vec_null_stride.data()));
  EXPECT_THAT(vec_null_stride, ElementsAre(10.0, 20.0, 30.0, 40.0));

  const long s_count = 2;
  const long s_stride = 2;
  const std::array<double, 2> strided_in = {111.0, 333.0};
  std::vector<double> strided_out(2, -999.0);
  EXPECT_EQ(0, ncvarputs(nc.get(), v_vec, &v_start, &s_count, &s_stride,
                         strided_in.data()));
  EXPECT_EQ(0, ncvargets(nc.get(), v_vec, &v_start, &s_count, &s_stride,
                         strided_out.data()));
  EXPECT_THAT(strided_out, ElementsAre(111.0, 333.0));

  const long bad_stride = 0;
  EXPECT_EQ(NC2_ERR, ncvarputs(nc.get(), v_vec, &v_start, &s_count, &bad_stride,
                               strided_in.data()));
  EXPECT_EQ(NC_ESTRIDE, ncerr);
  EXPECT_EQ(NC2_ERR, ncvargets(nc.get(), v_vec, &v_start, &s_count, &bad_stride,
                               strided_out.data()));
  EXPECT_EQ(NC_ESTRIDE, ncerr);

  // Mapped I/O (ncvarputg / ncvargetg): NULL map fallback, transposed 3x4 byte
  // map, and error paths.
  const std::array<long, 2> m_start = {0, 0};
  const std::array<long, 2> m_count = {3, 4};
  const std::array<long, 2> m_stride = {1, 1};
  std::array<int32_t, 12> linear_in = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  std::vector<int32_t> linear_out(12, -999);
  EXPECT_EQ(0, ncvarputg(nc.get(), v_mat, m_start.data(), m_count.data(),
                         m_stride.data(), nullptr, linear_in.data()));
  EXPECT_EQ(0, ncvargetg(nc.get(), v_mat, m_start.data(), m_count.data(),
                         m_stride.data(), nullptr, linear_out.data()));
  EXPECT_THAT(linear_out, ElementsAre(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));

  // Caller buffer is transposed [col][row] (4x3): byte step along row = 4,
  // byte step along col = 3 * 4.
  int32_t transposed_in[4][3] = {
      {100, 101, 102},
      {200, 201, 202},
      {300, 301, 302},
      {400, 401, 402},
  };
  const std::array<long, 2> byte_map = {
      static_cast<long>(sizeof(int32_t)),
      static_cast<long>(3 * sizeof(int32_t)),
  };
  EXPECT_EQ(0,
            ncvarputg(nc.get(), v_mat, m_start.data(), m_count.data(),
                      m_stride.data(), byte_map.data(), &transposed_in[0][0]));

  std::vector<int32_t> mat_linear(12, -999);
  EXPECT_EQ(0, ncvarget(nc.get(), v_mat, m_start.data(), m_count.data(),
                        mat_linear.data()));
  EXPECT_THAT(mat_linear, ElementsAre(100, 200, 300, 400, 101, 201, 301, 401,
                                      102, 202, 302, 402));

  std::vector<int32_t> transposed_out(12, -999);
  EXPECT_EQ(0, ncvargetg(nc.get(), v_mat, m_start.data(), m_count.data(),
                         nullptr, byte_map.data(), transposed_out.data()));
  EXPECT_THAT(transposed_out, ElementsAre(100, 101, 102, 200, 201, 202, 300,
                                          301, 302, 400, 401, 402));

  // Error paths in ncvarputg / ncvargetg when map != nullptr:
  // 1) Invalid ncid/varid fails at nc_inq_varndims and returns status directly.
  EXPECT_EQ(NC_EBADID,
            ncvarputg(-1, v_mat, m_start.data(), m_count.data(),
                      m_stride.data(), byte_map.data(), &transposed_in[0][0]));
  EXPECT_EQ(NC_ENOTVAR,
            ncvarputg(nc.get(), 99, m_start.data(), m_count.data(),
                      m_stride.data(), byte_map.data(), &transposed_in[0][0]));
  EXPECT_EQ(NC_EBADID,
            ncvargetg(-1, v_mat, m_start.data(), m_count.data(),
                      m_stride.data(), byte_map.data(), transposed_out.data()));
  EXPECT_EQ(NC_ENOTVAR,
            ncvargetg(nc.get(), 99, m_start.data(), m_count.data(),
                      m_stride.data(), byte_map.data(), transposed_out.data()));

  // 2) Out-of-bounds count in nc_put_varm / nc_get_varm calls nc_advise -> -1.
  const std::array<long, 2> bad_count = {99, 1};
  EXPECT_EQ(NC2_ERR,
            ncvarputg(nc.get(), v_mat, m_start.data(), bad_count.data(),
                      m_stride.data(), byte_map.data(), &transposed_in[0][0]));
  EXPECT_EQ(NC_EEDGE, ncerr);
  EXPECT_EQ(NC2_ERR,
            ncvargetg(nc.get(), v_mat, m_start.data(), bad_count.data(),
                      m_stride.data(), byte_map.data(), transposed_out.data()));
  EXPECT_EQ(NC_EEDGE, ncerr);
}

TEST(Dv2iTest, AttributesPutInqGetCopyNameRenameAndDelete) {
  ScopedNcV2Opts opts(0);
  ScopedNcFile nc1(nccreate("dv2i_atts1.nc", NC_CLOBBER | NC_DISKLESS));
  ScopedNcFile nc2(nccreate("dv2i_atts2.nc", NC_CLOBBER | NC_DISKLESS));
  ASSERT_GE(nc1.get(), 0);
  ASSERT_GE(nc2.get(), 0);

  const int dimid = ncdimdef(nc1.get(), "x", 2L);
  ASSERT_GE(dimid, 0);
  const int varid = ncvardef(nc1.get(), "v", NC_INT, 1, &dimid);
  ASSERT_GE(varid, 0);

  const std::array<int32_t, 3> att_vals = {11, 22, 33};
  EXPECT_EQ(0,
            ncattput(nc1.get(), NC_GLOBAL, "history", NC_CHAR, 7, "created"));
  EXPECT_EQ(0, ncattput(nc1.get(), varid, "scale", NC_INT, 3, att_vals.data()));
  EXPECT_EQ(NC2_ERR, ncattput(-1, varid, "bad", NC_INT, 1, att_vals.data()));
  EXPECT_EQ(NC_EBADID, ncerr);

  // ncattinq returns 1 on success; verify non-NULL, NULL, and error paths.
  nc_type att_type = NC_NAT;
  int att_len = -1;
  EXPECT_EQ(1, ncattinq(nc1.get(), varid, "scale", &att_type, &att_len));
  EXPECT_EQ(NC_INT, att_type);
  EXPECT_EQ(3, att_len);
  EXPECT_EQ(1, ncattinq(nc1.get(), varid, "scale", nullptr, nullptr));
  EXPECT_EQ(NC2_ERR,
            ncattinq(nc1.get(), varid, "missing", &att_type, &att_len));
  EXPECT_EQ(NC_ENOTATT, ncerr);

  // ncattget returns 1 on success.
  std::vector<int32_t> got_vals(3, -999);
  EXPECT_EQ(1, ncattget(nc1.get(), varid, "scale", got_vals.data()));
  EXPECT_THAT(got_vals, ElementsAre(11, 22, 33));
  EXPECT_EQ(NC2_ERR, ncattget(nc1.get(), varid, "missing", got_vals.data()));
  EXPECT_EQ(NC_ENOTATT, ncerr);

  // ncattname returns attnum on success.
  char att_name[NC_MAX_NAME + 1]{};
  EXPECT_EQ(0, ncattname(nc1.get(), varid, 0, att_name));
  EXPECT_STREQ("scale", att_name);
  EXPECT_EQ(NC2_ERR, ncattname(nc1.get(), varid, 99, att_name));
  EXPECT_EQ(NC_ENOTATT, ncerr);

  // ncattcopy returns 0 on success.
  EXPECT_EQ(0,
            ncattcopy(nc1.get(), NC_GLOBAL, "history", nc2.get(), NC_GLOBAL));
  std::string copied_history(7, '\0');
  EXPECT_EQ(1,
            ncattget(nc2.get(), NC_GLOBAL, "history", copied_history.data()));
  EXPECT_EQ("created", copied_history);
  EXPECT_EQ(NC2_ERR,
            ncattcopy(nc1.get(), NC_GLOBAL, "missing", nc2.get(), NC_GLOBAL));
  EXPECT_EQ(NC_ENOTATT, ncerr);

  // ncattrename and ncattdel return 1 on success.
  EXPECT_EQ(1, ncattrename(nc1.get(), varid, "scale", "scale_factor"));
  EXPECT_EQ(NC2_ERR, ncattrename(nc1.get(), varid, "scale", "x"));
  EXPECT_EQ(NC_ENOTATT, ncerr);

  EXPECT_EQ(1, ncattdel(nc1.get(), varid, "scale_factor"));
  EXPECT_EQ(NC2_ERR, ncattdel(nc1.get(), varid, "scale_factor"));
  EXPECT_EQ(NC_ENOTATT, ncerr);
}

TEST(Dv2iTest, RecordApisInqPutGetHandleNoRecVarsMultiDimAndErrors) {
  ScopedNcV2Opts opts(0);

  // Case 1: Dataset with NO unlimited dimension.
  {
    ScopedNcFile nc(nccreate("dv2i_no_unlim.nc", NC_CLOBBER | NC_DISKLESS));
    ASSERT_GE(nc.get(), 0);
    const int d_fixed = ncdimdef(nc.get(), "x", 2L);
    ASSERT_GE(d_fixed, 0);
    ASSERT_GE(ncvardef(nc.get(), "fx", NC_INT, 1, &d_fixed), 0);
    ASSERT_EQ(0, ncendef(nc.get()));

    size_t nrv = 77;
    int rids[4]{};
    size_t rsizes[4]{};
    EXPECT_EQ(NC_NOERR, nc_inq_rec(nc.get(), &nrv, rids, rsizes));
    EXPECT_EQ(77U, nrv);  // nc_inq_rec returns early before modifying *nrv.

    int v2_nrv = -1;
    long v2_rsizes[4]{};
    EXPECT_EQ(0, ncrecinq(nc.get(), &v2_nrv, rids, v2_rsizes));
    EXPECT_EQ(0, v2_nrv);

    void* dummy_ptrs[1] = {nullptr};
    EXPECT_EQ(NC_NOERR, nc_put_rec(nc.get(), 0, dummy_ptrs));
    EXPECT_EQ(NC_NOERR, nc_get_rec(nc.get(), 0, dummy_ptrs));
  }

  // Case 2: Dataset WITH unlimited dimension, but zero record variables.
  {
    ScopedNcFile nc(nccreate("dv2i_zero_recvars.nc", NC_CLOBBER | NC_DISKLESS));
    ASSERT_GE(nc.get(), 0);
    ASSERT_GE(ncdimdef(nc.get(), "rec", NC_UNLIMITED), 0);
    const int d_fixed = ncdimdef(nc.get(), "x", 2L);
    ASSERT_GE(d_fixed, 0);
    ASSERT_GE(ncvardef(nc.get(), "fx", NC_INT, 1, &d_fixed), 0);
    ASSERT_EQ(0, ncendef(nc.get()));

    size_t nrv = 77;
    EXPECT_EQ(NC_NOERR, nc_inq_rec(nc.get(), &nrv, nullptr, nullptr));
    EXPECT_EQ(0U, nrv);
    EXPECT_EQ(0, ncrecinq(nc.get(), nullptr, nullptr, nullptr));

    void* dummy_ptrs[1] = {nullptr};
    EXPECT_EQ(NC_NOERR, nc_put_rec(nc.get(), 0, dummy_ptrs));
    EXPECT_EQ(NC_NOERR, nc_get_rec(nc.get(), 0, dummy_ptrs));
  }

  // Case 3: Mixed scalar, fixed, 1D record, 2D record, and 3D record variables.
  // Keeping nrvars (3) >= max_ndims (3) ensures dv2i.c initializes start[0..2].
  TempFile rec_file("dv2i_records.nc");
  int r1_id = -1;
  int r2_id = -1;
  int r3_id = -1;
  {
    ScopedNcFile nc(nccreate(rec_file.c_str(), NC_CLOBBER));
    ASSERT_GE(nc.get(), 0);

    const int d_rec = ncdimdef(nc.get(), "rec", NC_UNLIMITED);
    const int d_y = ncdimdef(nc.get(), "y", 2L);
    const int d_x = ncdimdef(nc.get(), "x", 3L);
    ASSERT_GE(d_rec, 0);
    ASSERT_GE(d_y, 0);
    ASSERT_GE(d_x, 0);

    // Non-record variables exercise numrecvars() skipping scalar and fixed
    // vars.
    ASSERT_GE(ncvardef(nc.get(), "fixed_var", NC_INT, 1, &d_x), 0);
    ASSERT_GE(ncvardef(nc.get(), "scalar_var", NC_INT, 0, nullptr), 0);

    const int dims_2d[2] = {d_rec, d_x};
    const int dims_3d[3] = {d_rec, d_y, d_x};
    r1_id = ncvardef(nc.get(), "r1", NC_INT, 1, &d_rec);
    r2_id = ncvardef(nc.get(), "r2", NC_DOUBLE, 2, dims_2d);
    r3_id = ncvardef(nc.get(), "r3", NC_SHORT, 3, dims_3d);
    ASSERT_GE(r1_id, 0);
    ASSERT_GE(r2_id, 0);
    ASSERT_GE(r3_id, 0);
    ASSERT_EQ(0, ncendef(nc.get()));

    size_t nrv = 0;
    std::array<int, 3> rids = {-1, -1, -1};
    std::array<size_t, 3> rsizes = {0, 0, 0};
    EXPECT_EQ(NC_NOERR, nc_inq_rec(nc.get(), &nrv, rids.data(), rsizes.data()));
    EXPECT_EQ(3U, nrv);
    EXPECT_THAT(rids, ElementsAre(r1_id, r2_id, r3_id));
    EXPECT_THAT(rsizes, ElementsAre(sizeof(int32_t), 3 * sizeof(double),
                                    2 * 3 * sizeof(int16_t)));
    EXPECT_EQ(NC_NOERR, nc_inq_rec(nc.get(), nullptr, nullptr, nullptr));

    int v2_nrv = -1;
    std::array<int, 3> v2_rids = {-1, -1, -1};
    std::array<long, 3> v2_rsizes = {0, 0, 0};
    EXPECT_EQ(3, ncrecinq(nc.get(), &v2_nrv, v2_rids.data(), v2_rsizes.data()));
    EXPECT_EQ(3, v2_nrv);
    EXPECT_THAT(v2_rids, ElementsAre(r1_id, r2_id, r3_id));
    EXPECT_THAT(v2_rsizes,
                ElementsAre(static_cast<long>(sizeof(int32_t)),
                            static_cast<long>(3 * sizeof(double)),
                            static_cast<long>(2 * 3 * sizeof(int16_t))));
    EXPECT_EQ(3, ncrecinq(nc.get(), nullptr, nullptr, nullptr));

    // Write record 0 across all three record variables via nc_put_rec.
    int32_t r1_rec0 = 100;
    std::array<double, 3> r2_rec0 = {1.5, 2.5, 3.5};
    std::array<int16_t, 6> r3_rec0 = {1, 2, 3, 4, 5, 6};
    void* rec0_ptrs[3] = {&r1_rec0, r2_rec0.data(), r3_rec0.data()};
    EXPECT_EQ(NC_NOERR, nc_put_rec(nc.get(), 0, rec0_ptrs));

    // Write record 1 using ncrecput in two passes with NULL entries skipped.
    int32_t r1_rec1 = 200;
    std::array<double, 3> r2_rec1 = {4.5, 5.5, 6.5};
    std::array<int16_t, 6> r3_rec1 = {10, 20, 30, 40, 50, 60};
    void* rec1_part_a[3] = {&r1_rec1, nullptr, r3_rec1.data()};
    void* rec1_part_b[3] = {nullptr, r2_rec1.data(), nullptr};
    EXPECT_EQ(0, ncrecput(nc.get(), 1L, rec1_part_a));
    EXPECT_EQ(0, ncrecput(nc.get(), 1L, rec1_part_b));

    const int close_rc = ncclose(nc.get());
    nc.release();
    EXPECT_EQ(0, close_rc);
  }

  // Reopen read-only to verify record reads and write/bounds error paths.
  {
    ScopedNcFile nc(ncopen(rec_file.c_str(), NC_NOWRITE));
    ASSERT_GE(nc.get(), 0);

    int32_t r1_out = -999;
    std::vector<double> r2_out(3, -999.0);
    std::vector<int16_t> r3_out(6, -999);
    void* rec0_out_ptrs[3] = {&r1_out, r2_out.data(), r3_out.data()};
    EXPECT_EQ(NC_NOERR, nc_get_rec(nc.get(), 0, rec0_out_ptrs));
    EXPECT_EQ(100, r1_out);
    EXPECT_THAT(r2_out, ElementsAre(1.5, 2.5, 3.5));
    EXPECT_THAT(r3_out, ElementsAre(1, 2, 3, 4, 5, 6));

    // Read record 1 via ncrecget in partial NULL passes.
    r1_out = -999;
    r2_out.assign(3, -999.0);
    r3_out.assign(6, -999);
    void* rec1_out_a[3] = {&r1_out, nullptr, r3_out.data()};
    void* rec1_out_b[3] = {nullptr, r2_out.data(), nullptr};
    EXPECT_EQ(0, ncrecget(nc.get(), 1L, rec1_out_a));
    EXPECT_EQ(0, ncrecget(nc.get(), 1L, rec1_out_b));
    EXPECT_EQ(200, r1_out);
    EXPECT_THAT(r2_out, ElementsAre(4.5, 5.5, 6.5));
    EXPECT_THAT(r3_out, ElementsAre(10, 20, 30, 40, 50, 60));

    // Writing to read-only dataset fails with NC_EPERM.
    int32_t write_val = 7;
    void* ro_put_ptrs[3] = {&write_val, nullptr, nullptr};
    EXPECT_EQ(NC_EPERM, nc_put_rec(nc.get(), 0, ro_put_ptrs));
    EXPECT_EQ(NC2_ERR, ncrecput(nc.get(), 0L, ro_put_ptrs));
    EXPECT_EQ(NC_EPERM, ncerr);

    // Reading beyond current record count fails with NC_EINVALCOORDS.
    EXPECT_EQ(NC_EINVALCOORDS, nc_get_rec(nc.get(), 99, rec0_out_ptrs));
    EXPECT_EQ(NC2_ERR, ncrecget(nc.get(), 99L, rec0_out_ptrs));
    EXPECT_EQ(NC_EINVALCOORDS, ncerr);
  }

  // Invalid ncid checks for all record functions.
  size_t bad_nrv = 0;
  int bad_ids[1]{};
  size_t bad_sizes[1]{};
  long bad_v2_sizes[1]{};
  void* bad_ptrs[1] = {nullptr};
  EXPECT_EQ(NC_EBADID, nc_inq_rec(-1, &bad_nrv, bad_ids, bad_sizes));
  EXPECT_EQ(NC_EBADID, nc_put_rec(-1, 0, bad_ptrs));
  EXPECT_EQ(NC_EBADID, nc_get_rec(-1, 0, bad_ptrs));
  EXPECT_EQ(NC2_ERR, ncrecinq(-1, nullptr, bad_ids, bad_v2_sizes));
  EXPECT_EQ(NC_EBADID, ncerr);
  EXPECT_EQ(NC2_ERR, ncrecput(-1, 0L, bad_ptrs));
  EXPECT_EQ(NC_EBADID, ncerr);
  EXPECT_EQ(NC2_ERR, ncrecget(-1, 0L, bad_ptrs));
  EXPECT_EQ(NC_EBADID, ncerr);
}

}  // namespace
}  // namespace netcdf
