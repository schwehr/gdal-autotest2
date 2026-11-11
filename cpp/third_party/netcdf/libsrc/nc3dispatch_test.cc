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

// Unit tests for //third_party/netcdf/libsrc/nc3dispatch.c.

#include <cstddef>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
#include "third_party/netcdf/include/nc3dispatch.h"
int nc_var_par_access(int ncid, int varid, int par_access);
}  // extern "C"
// clang-format on

namespace netcdf {
namespace {

TEST(Nc3DispatchTest, DispatchTableAndLifecycleHooksAreConsistent) {
  ASSERT_EQ(NC_NOERR, nc_initialize());
  ASSERT_NE(nullptr, NC3_dispatch_table);
  EXPECT_EQ(NC_FORMATX_NC3, NC3_dispatch_table->model);
  EXPECT_EQ(NC_DISPATCH_VERSION, NC3_dispatch_table->dispatch_version);

  EXPECT_EQ(NC_NOERR, NC3_finalize());
  EXPECT_EQ(NC_NOERR, NC3_initialize());
  ASSERT_NE(nullptr, NC3_dispatch_table);
  EXPECT_EQ(NC_FORMATX_NC3, NC3_dispatch_table->model);
}

TEST(Nc3DispatchTest, InqVarAllHandlesAllArgumentsNullsUnsupportedAndBadVar) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc3d_varall.nc", NC_CLOBBER, 0, file.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "x", 3, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "v", NC_INT, 1, &dimid, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

  // Exercise libdispatch wrappers that reach NC3_inq_var_all.
  int shuffle = -1;
  int deflate = -1;
  int deflate_level = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(file.get(), varid, &shuffle, &deflate,
                                         &deflate_level));
  EXPECT_EQ(0, shuffle);
  EXPECT_EQ(0, deflate);
  EXPECT_EQ(0, deflate_level);

  int fletcher32 = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(file.get(), varid, &fletcher32));
  EXPECT_EQ(0, fletcher32);

  int storage = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking(file.get(), varid, &storage, nullptr));
  EXPECT_EQ(NC_CONTIGUOUS, storage);

  int endian = 0;
  EXPECT_EQ(NC_ENOTNC4, nc_inq_var_endian(file.get(), varid, &endian));

  int no_fill = -1;
  int fill_val = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_fill(file.get(), varid, &no_fill, &fill_val));

  // Direct calls hit every parameter branch inside NC3_inq_var_all.
  shuffle = -1;
  deflate = -1;
  fletcher32 = -1;
  storage = -1;
  char name[NC_MAX_NAME + 1] = {};
  nc_type xtype = NC_NAT;
  int ndims = 0;
  int dims[NC_MAX_VAR_DIMS] = {};
  int natts = 0;
  ASSERT_EQ(NC_NOERR,
            NCDISPATCH_inq_var_all(
                file.get(), varid, name, &xtype, &ndims, dims, &natts, &shuffle,
                &deflate, nullptr, &fletcher32, &storage, nullptr, &no_fill,
                &fill_val, nullptr, nullptr, nullptr, nullptr));
  EXPECT_STREQ("v", name);
  EXPECT_EQ(NC_INT, xtype);
  EXPECT_EQ(1, ndims);
  EXPECT_EQ(0, shuffle);
  EXPECT_EQ(0, deflate);
  EXPECT_EQ(0, fletcher32);
  EXPECT_EQ(NC_CONTIGUOUS, storage);

  // All optional NC3_inq_var_all pointers nullptr.
  EXPECT_EQ(NC_NOERR,
            NC3_dispatch_table->inq_var_all(
                file.get(), varid, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr));

  // Invalid varid returns NC_ENOTVAR from NC3_inq_var.
  EXPECT_EQ(NC_ENOTVAR,
            NC3_dispatch_table->inq_var_all(
                file.get(), 999, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr));

  // Each netCDF-4-only pointer triggers NC_ENOTNC4 individually.
  unsigned int filter_id = 0;
  size_t nparams = 0;
  unsigned int params[2] = {};
  EXPECT_EQ(NC_ENOTNC4,
            NC3_dispatch_table->inq_var_all(
                file.get(), varid, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, &endian, nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_ENOTNC4,
            NC3_dispatch_table->inq_var_all(
                file.get(), varid, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, &filter_id, nullptr, nullptr));
  EXPECT_EQ(NC_ENOTNC4,
            NC3_dispatch_table->inq_var_all(
                file.get(), varid, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, &nparams, nullptr));
  EXPECT_EQ(NC_ENOTNC4,
            NC3_dispatch_table->inq_var_all(
                file.get(), varid, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, params));

  ASSERT_EQ(NC_NOERR, file.abort());
}

TEST(Nc3DispatchTest, VarParAccessReturnsNoErrDirectly) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("nc3d_par.nc", NC_CLOBBER, 0, file.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "x", 2, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "v", NC_INT, 1, &dimid, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

  constexpr int kIndependent = 0;
  constexpr int kCollective = 1;
  EXPECT_EQ(NC_ENOPAR, nc_var_par_access(file.get(), varid, kIndependent));
  EXPECT_EQ(NC_NOERR, NC3_dispatch_table->var_par_access(file.get(), varid,
                                                         kIndependent));
  EXPECT_EQ(NC_NOERR,
            NC3_dispatch_table->var_par_access(file.get(), varid, kCollective));
  ASSERT_EQ(NC_NOERR, file.abort());
}

TEST(Nc3DispatchTest, InqVaridsDimidsUnlimdimsCoverAllPaths) {
  // Empty dataset: 0 dims, 0 vars, no unlimited dim.
  {
    ScopedNcFile empty;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nc3d_empty.nc", NC_CLOBBER, 0, empty.ptr()));
    ASSERT_EQ(NC_NOERR, nc_enddef(empty.get()));

    int nunlim = -1;
    int unlim_id = 99;
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(empty.get(), &nunlim, &unlim_id));
    EXPECT_EQ(0, nunlim);
    EXPECT_EQ(NC_NOERR, nc_inq_unlimdims(empty.get(), nullptr, nullptr));

    int nvars = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varids(empty.get(), &nvars, nullptr));
    EXPECT_EQ(0, nvars);

    int ndims = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimids(empty.get(), &ndims, nullptr, 0));
    EXPECT_EQ(0, ndims);
    ASSERT_EQ(NC_NOERR, empty.abort());
  }

  // Dataset with 2 dims (one unlimited) and 2 variables.
  {
    ScopedNcFile populated;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nc3d_pop.nc", NC_CLOBBER, 0, populated.ptr()));
    int xdim = -1;
    int recdim = -1;
    int v0 = -1;
    int v1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(populated.get(), "x", 4, &xdim));
    ASSERT_EQ(NC_NOERR,
              nc_def_dim(populated.get(), "rec", NC_UNLIMITED, &recdim));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(populated.get(), "v0", NC_INT, 1, &xdim, &v0));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(populated.get(), "v1", NC_FLOAT, 1, &recdim, &v1));
    ASSERT_EQ(NC_NOERR, nc_enddef(populated.get()));

    int nunlim = -1;
    int unlim_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_unlimdims(populated.get(), &nunlim, &unlim_id));
    EXPECT_EQ(1, nunlim);
    EXPECT_EQ(recdim, unlim_id);
    EXPECT_EQ(NC_NOERR, nc_inq_unlimdims(populated.get(), nullptr, nullptr));

    int nvars = -1;
    int varids[2] = {-1, -1};
    ASSERT_EQ(NC_NOERR, nc_inq_varids(populated.get(), &nvars, varids));
    EXPECT_EQ(2, nvars);
    EXPECT_THAT(varids, ::testing::ElementsAre(0, 1));
    int varids_no_count[2] = {-1, -1};
    ASSERT_EQ(NC_NOERR,
              nc_inq_varids(populated.get(), nullptr, varids_no_count));
    EXPECT_THAT(varids_no_count, ::testing::ElementsAre(0, 1));
    ASSERT_EQ(NC_NOERR, nc_inq_varids(populated.get(), &nvars, nullptr));

    int ndims = -1;
    int dimids[2] = {-1, -1};
    ASSERT_EQ(NC_NOERR, nc_inq_dimids(populated.get(), &ndims, dimids, 0));
    EXPECT_EQ(2, ndims);
    EXPECT_THAT(dimids, ::testing::ElementsAre(0, 1));
    int dimids_parents[2] = {-1, -1};
    ASSERT_EQ(NC_NOERR,
              nc_inq_dimids(populated.get(), nullptr, dimids_parents, 1));
    EXPECT_THAT(dimids_parents, ::testing::ElementsAre(0, 1));
    ASSERT_EQ(NC_NOERR, nc_inq_dimids(populated.get(), &ndims, nullptr, 0));

    ASSERT_EQ(NC_NOERR, populated.abort());
  }

  // Bad ncid reaches NC3_inq_unlimdims / NC3_inq_varids / NC3_inq_dimids error
  // returns directly through the dispatch table.
  int count = 0;
  int ids[2] = {};
  EXPECT_EQ(NC_EBADID, NC3_dispatch_table->inq_unlimdims(-1, &count, ids));
  EXPECT_EQ(NC_EBADID, NC3_dispatch_table->inq_varids(-1, &count, ids));
  EXPECT_EQ(NC_EBADID, NC3_dispatch_table->inq_dimids(-1, &count, ids, 0));
}

TEST(Nc3DispatchTest, GroupAndMetadataInquiriesMatchClassicBehavior) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("nc3d_grp.nc", NC_CLOBBER, 0, file.ptr()));
  ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

  int grpid = -1;
  EXPECT_EQ(NC_ENOTNC4, nc_def_grp(file.get(), "child", &grpid));
  EXPECT_EQ(NC_ENOTNC4, nc_rename_grp(file.get(), "renamed"));

  ASSERT_EQ(NC_NOERR, nc_inq_ncid(file.get(), "/", &grpid));
  EXPECT_EQ(file.get(), grpid);
  EXPECT_EQ(NC_NOERR, NC3_dispatch_table->inq_ncid(file.get(), "/", nullptr));

  int numgrps = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grps(file.get(), &numgrps, &grpid));
  EXPECT_EQ(0, numgrps);
  EXPECT_EQ(NC_NOERR, nc_inq_grps(file.get(), nullptr, nullptr));

  char grpname[NC_MAX_NAME + 1] = {};
  ASSERT_EQ(NC_NOERR, nc_inq_grpname(file.get(), grpname));
  EXPECT_STREQ("/", grpname);
  EXPECT_EQ(NC_NOERR, NC3_dispatch_table->inq_grpname(file.get(), nullptr));

  size_t full_len = 0;
  char fullname[NC_MAX_NAME + 1] = {};
  ASSERT_EQ(NC_NOERR, nc_inq_grpname_full(file.get(), &full_len, fullname));
  EXPECT_EQ(1U, full_len);
  EXPECT_STREQ("/", fullname);

  size_t only_len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_grpname_len(file.get(), &only_len));
  EXPECT_EQ(1U, only_len);
  EXPECT_EQ(NC_NOERR, nc_inq_grpname_full(file.get(), nullptr, nullptr));

  EXPECT_EQ(NC_ENOGRP, nc_inq_grp_parent(file.get(), &grpid));
  EXPECT_EQ(NC_ENOGRP, nc_inq_grp_full_ncid(file.get(), "/", &grpid));
  EXPECT_EQ(NC_NOERR, nc_show_metadata(file.get()));

  ASSERT_EQ(NC_NOERR, file.abort());
}

TEST(Nc3DispatchTest, TypeInquiriesAndTypeEqualityCoverAllBranches) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc3d_types.nc", NC_CLOBBER, 0, file.ptr()));
  ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

  struct TypeNameCase {
    const char* name;
    nc_type expected;
  };
  constexpr TypeNameCase kAtomicTypes[] = {
      {"undefined", NC_NAT}, {"byte", NC_BYTE}, {"char", NC_CHAR},
      {"short", NC_SHORT},   {"int", NC_INT},   {"float", NC_FLOAT},
      {"double", NC_DOUBLE},
  };
  for (const TypeNameCase& tc : kAtomicTypes) {
    nc_type tid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_typeid(file.get(), tc.name, &tid)) << tc.name;
    EXPECT_EQ(tc.expected, tid) << tc.name;
  }
  EXPECT_EQ(NC_NOERR,
            NC3_dispatch_table->inq_typeid(file.get(), "int", nullptr));
  nc_type unknown_tid = -1;
  EXPECT_EQ(NC_ENOTNC4, nc_inq_typeid(file.get(), "uint64", &unknown_tid));

  int ntypes = -1;
  int typeids[2] = {};
  ASSERT_EQ(NC_NOERR, nc_inq_typeids(file.get(), &ntypes, typeids));
  EXPECT_EQ(0, ntypes);
  EXPECT_EQ(NC_NOERR, nc_inq_typeids(file.get(), nullptr, nullptr));

  char uname[NC_MAX_NAME + 1] = {};
  size_t usize = 0;
  nc_type ubase = NC_NAT;
  size_t unfields = 0;
  int uclass = 0;
  EXPECT_EQ(NC_ENOTNC4, nc_inq_user_type(file.get(), NC_INT, uname, &usize,
                                         &ubase, &unfields, &uclass));

  // Cover all branches of NC3_inq_type_equal.
  EXPECT_EQ(NC_NOERR,
            nc_inq_type_equal(file.get(), NC_INT, file.get(), NC_INT, nullptr));
  int eq = -1;
  EXPECT_EQ(NC_EINVAL,
            nc_inq_type_equal(file.get(), NC_NAT, file.get(), NC_INT, &eq));
  EXPECT_EQ(NC_EINVAL,
            nc_inq_type_equal(file.get(), NC_INT, file.get(), -1, &eq));

  eq = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(file.get(), NC_INT, file.get(),
                                        NC_STRING + 1, &eq));
  EXPECT_EQ(0, eq);

  eq = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(file.get(), NC_STRING + 1, file.get(),
                                        NC_INT, &eq));
  EXPECT_EQ(0, eq);

  eq = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_type_equal(file.get(), NC_INT, file.get(), NC_INT, &eq));
  EXPECT_EQ(1, eq);

  eq = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_type_equal(file.get(), NC_INT, file.get(), NC_DOUBLE, &eq));
  EXPECT_EQ(0, eq);

  // Types > ATOMICTYPEMAX3 (e.g., NC_STRING and > NC_STRING) fall through.
  eq = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_type_equal(file.get(), NC_STRING, file.get(), NC_INT, &eq));
  EXPECT_EQ(0, eq);
  eq = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(file.get(), NC_STRING + 1, file.get(),
                                        NC_STRING + 1, &eq));
  EXPECT_EQ(0, eq);

  ASSERT_EQ(NC_NOERR, file.abort());
}

TEST(Nc3DispatchTest, Netcdf4EnhancedStubsReturnNotNc4) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc3d_nc4stubs.nc", NC_CLOBBER, 0, file.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "x", 2, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "v", NC_INT, 1, &dimid, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

  nc_type utype = -1;
  int fieldid = -1;
  char name[NC_MAX_NAME + 1] = {};
  size_t offset = 0;
  nc_type ftype = NC_NAT;
  int ndims = 0;
  int dims[4] = {};
  EXPECT_EQ(NC_ENOTNC4, nc_def_compound(file.get(), 8, "c", &utype));
  EXPECT_EQ(NC_ENOTNC4, nc_insert_compound(file.get(), 100, "f", 0, NC_INT));
  EXPECT_EQ(NC_ENOTNC4, nc_insert_array_compound(file.get(), 100, "fa", 0,
                                                 NC_INT, 1, &ndims));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_compound_field(file.get(), 100, 0, name, &offset,
                                              &ftype, &ndims, dims));
  EXPECT_EQ(NC_ENOTNC4,
            nc_inq_compound_fieldindex(file.get(), 100, "f", &fieldid));

  EXPECT_EQ(NC_ENOTNC4, nc_def_opaque(file.get(), 4, "opq", &utype));
  EXPECT_EQ(NC_ENOTNC4, nc_def_vlen(file.get(), "vl", NC_INT, &utype));

  int enum_val = 1;
  long long enum_ll = 1;
  EXPECT_EQ(NC_ENOTNC4, nc_def_enum(file.get(), NC_INT, "en", &utype));
  EXPECT_EQ(NC_ENOTNC4, nc_insert_enum(file.get(), 100, "ONE", &enum_val));
  EXPECT_EQ(NC_ENOTNC4,
            nc_inq_enum_member(file.get(), 100, 0, name, &enum_val));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_enum_ident(file.get(), 100, enum_ll, name));

  nc_vlen_t vlen_elem{};
  size_t vlen_len = 0;
  int vdata[2] = {1, 2};
  EXPECT_EQ(NC_ENOTNC4,
            nc_put_vlen_element(file.get(), 100, &vlen_elem, 2, vdata));
  EXPECT_EQ(NC_ENOTNC4,
            nc_get_vlen_element(file.get(), 100, &vlen_elem, &vlen_len, vdata));

  size_t csize = 0;
  size_t nelems = 0;
  float preempt = 0.0F;
  EXPECT_EQ(NC_ENOTNC4,
            nc_set_var_chunk_cache(file.get(), varid, 1024, 16, 0.5F));
  EXPECT_EQ(NC_ENOTNC4, nc_get_var_chunk_cache(file.get(), varid, &csize,
                                               &nelems, &preempt));

  const size_t chunk = 2;
  const unsigned int param = 1;
  EXPECT_EQ(NC_ENOTNC4, nc_def_var_deflate(file.get(), varid, 1, 1, 3));
  EXPECT_EQ(NC_ENOTNC4,
            nc_def_var_fletcher32(file.get(), varid, NC_FLETCHER32));
  EXPECT_EQ(NC_ENOTNC4,
            nc_def_var_chunking(file.get(), varid, NC_CHUNKED, &chunk));
  EXPECT_EQ(NC_ENOTNC4, nc_def_var_endian(file.get(), varid, NC_ENDIAN_BIG));
  EXPECT_EQ(NC_ENOTNC4, nc_def_var_filter(file.get(), varid, 32001, 1, &param));

  int qmode = 0;
  int qnsd = 0;
  EXPECT_EQ(NC_ENOTNC4,
            nc_def_var_quantize(file.get(), varid, NC_QUANTIZE_BITGROOM, 3));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_var_quantize(file.get(), varid, &qmode, &qnsd));

  ASSERT_EQ(NC_NOERR, file.abort());
}

TEST(Nc3DispatchTest, ClassicFormatsAndDataOperationsRoundTrip) {
  struct FormatCase {
    int cmode;
    int expected_format;
  };
  constexpr FormatCase kFormats[] = {
      {NC_CLOBBER, NC_FORMAT_CLASSIC},
      {NC_CLOBBER | NC_64BIT_OFFSET, NC_FORMAT_64BIT_OFFSET},
      {NC_CLOBBER | NC_CDF5, NC_FORMAT_CDF5},
  };

  for (const FormatCase& fc : kFormats) {
    ScopedMemio saved;
    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("nc3d_fmt.nc", fc.cmode, 0, file.ptr()));
      int fmt = 0;
      int fmt_x = 0;
      int mode = 0;
      ASSERT_EQ(NC_NOERR, nc_inq_format(file.get(), &fmt));
      EXPECT_EQ(fc.expected_format, fmt);
      ASSERT_EQ(NC_NOERR, nc_inq_format_extended(file.get(), &fmt_x, &mode));
      EXPECT_EQ(NC_FORMATX_NC3, fmt_x);

      int dimid = -1;
      int varid = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "x", 4, &dimid));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(file.get(), "arr", NC_INT, 1, &dimid, &varid));
      const int fill = -999;
      ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), varid, NC_FILL, &fill));
      ASSERT_EQ(NC_NOERR,
                nc_put_att_int(file.get(), varid, "tag", NC_INT, 1, &fill));
      ASSERT_EQ(NC_NOERR, nc__enddef(file.get(), 0, 4, 0, 4));

      const size_t start[1] = {0};
      const size_t count[1] = {2};
      const ptrdiff_t stride[1] = {2};
      const ptrdiff_t imap[1] = {1};
      const int strided[2] = {10, 30};
      ASSERT_EQ(NC_NOERR, nc_put_vars_int(file.get(), varid, start, count,
                                          stride, strided));
      int got_strided[2] = {};
      ASSERT_EQ(NC_NOERR, nc_get_vars_int(file.get(), varid, start, count,
                                          stride, got_strided));
      EXPECT_THAT(got_strided, ::testing::ElementsAre(10, 30));

      const size_t single_start[1] = {1};
      const size_t single_count[1] = {1};
      const int val1 = 20;
      ASSERT_EQ(NC_NOERR, nc_put_vara_int(file.get(), varid, single_start,
                                          single_count, &val1));
      int got_val1 = 0;
      ASSERT_EQ(NC_NOERR, nc_get_vara_int(file.get(), varid, single_start,
                                          single_count, &got_val1));
      EXPECT_EQ(20, got_val1);

      const int mapped[2] = {11, 31};
      ASSERT_EQ(NC_NOERR, nc_put_varm_int(file.get(), varid, start, count,
                                          stride, imap, mapped));
      int got_mapped[2] = {};
      ASSERT_EQ(NC_NOERR, nc_get_varm_int(file.get(), varid, start, count,
                                          stride, imap, got_mapped));
      EXPECT_THAT(got_mapped, ::testing::ElementsAre(11, 31));
      ASSERT_EQ(NC_NOERR, nc_sync(file.get()));

      ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
      ASSERT_EQ(NC_NOERR, nc_rename_dim(file.get(), dimid, "x_new"));
      ASSERT_EQ(NC_NOERR, nc_rename_var(file.get(), varid, "arr_new"));
      ASSERT_EQ(NC_NOERR, nc_rename_att(file.get(), varid, "tag", "tag2"));
      ASSERT_EQ(NC_NOERR, nc_del_att(file.get(), varid, "tag2"));
      ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

      ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), saved.get()));
    }

    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open_mem("nc3d_fmt_ro.nc", NC_NOWRITE, saved.size(),
                                    saved.get()->memory, ro.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro.get(), "arr_new", &varid));
    int out[4] = {};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro.get(), varid, out));
    EXPECT_THAT(out, ::testing::ElementsAre(11, 20, 31, -999));
    ASSERT_EQ(NC_NOERR, ro.close());
  }
}

}  // namespace
}  // namespace netcdf
