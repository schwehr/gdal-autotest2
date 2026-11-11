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

// Clean-room C++20 unit tests for //third_party/netcdf/libhdf5/hdf5var.c
// (variable definition, storage/filter/fill/endian/quantize properties,
// variable renaming, strided/hyperslab/scalar/fill I/O, fixed-length HDF5
// string adaptation, and per-variable chunk cache management).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/netcdf/google/libhdf5/hdf5var_shim.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_f.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr int kBadGrpMask = 0x0FFF;

int BogusGrpId(int ncid) { return ncid | kBadGrpMask; }

struct CustomCompound {
  int32_t a;
  double b;
};

TEST(Hdf5VarTest, DefVarValidatesInputsAndModeTransitions) {
  TempFile file("hdf5var_def_err.nc");

  {
    ScopedNcFile ncid;
    ASSERT_EQ(NC_NOERR,
              nc_create(file.c_str(), NC_NETCDF4 | NC_CLOBBER, ncid.ptr()));

    int dimx = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 4, &dimx));

    // Invalid group ID within an open file passes dispatch checks and hits
    // nc4_find_grp_h5 inside NC4_def_var.
    int unused_varid = -1;
    EXPECT_EQ(NC_EBADID, nc_def_var(BogusGrpId(ncid), "v_badgrp", NC_INT, 1,
                                    &dimx, &unused_varid));

    // Rank > H5S_MAX_RANK (32) fails with NC_EMAXDIMS; negative rank hits
    // X_INT_MAX unsigned check with NC_EINVAL.
    std::array<int, 33> too_many_dims{};
    too_many_dims.fill(dimx);
    EXPECT_EQ(NC_EMAXDIMS, nc_def_var(ncid, "too_many_dims", NC_INT,
                                      static_cast<int>(too_many_dims.size()),
                                      too_many_dims.data(), &unused_varid));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var(ncid, "neg_dims", NC_INT, -1, nullptr, &unused_varid));

    // Name validation and length bounds.
    EXPECT_EQ(NC_EBADNAME,
              nc_def_var(ncid, "", NC_INT, 0, nullptr, &unused_varid));
    EXPECT_EQ(NC_EBADNAME,
              nc_def_var(ncid, "bad/slash", NC_INT, 0, nullptr, &unused_varid));
    EXPECT_EQ(NC_EBADNAME,
              nc_def_var(ncid, "trailing ", NC_INT, 0, nullptr, &unused_varid));
    const std::string over_max_name(NC_MAX_NAME + 1, 'n');
    EXPECT_EQ(NC_EMAXNAME, nc_def_var(ncid, over_max_name.c_str(), NC_INT, 0,
                                      nullptr, &unused_varid));

    // Type validation: NC_NAT and unknown >NC_STRING type return NC_EBADTYPE;
    // negative atomic ID hits nc4_get_typelen_mem and returns NC_EINVAL.
    EXPECT_EQ(NC_EBADTYPE,
              nc_def_var(ncid, "bad_nat", NC_NAT, 0, nullptr, &unused_varid));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var(ncid, "bad_neg_type", -5, 0, nullptr, &unused_varid));
    EXPECT_EQ(NC_EBADTYPE,
              nc_def_var(ncid, "bad_user_type", NC_FIRSTUSERTYPEID + 500, 0,
                         nullptr, &unused_varid));

    // Non-scalar requires non-null valid dimids.
    EXPECT_EQ(NC_EINVAL,
              nc_def_var(ncid, "null_dims", NC_INT, 1, nullptr, &unused_varid));
    const int bad_dimid = 999;
    EXPECT_EQ(NC_EBADDIM, nc_def_var(ncid, "missing_dim", NC_INT, 1, &bad_dimid,
                                     &unused_varid));

    // Duplicate names against existing variable, child group, and user type.
    int v0 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v0", NC_INT, 1, &dimx, &v0));
    int sub_ncid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(ncid, "subgrp", &sub_ncid));
    nc_type opq_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(ncid, 8, "opqt", &opq_type));

    EXPECT_EQ(NC_ENAMEINUSE,
              nc_def_var(ncid, "v0", NC_INT, 0, nullptr, &unused_varid));
    EXPECT_EQ(NC_ENAMEINUSE,
              nc_def_var(ncid, "subgrp", NC_INT, 0, nullptr, &unused_varid));
    EXPECT_EQ(NC_ENAMEINUSE,
              nc_def_var(ncid, "opqt", NC_INT, 0, nullptr, &unused_varid));

    // Exiting define mode and defining another variable on a standard netCDF-4
    // file automatically re-enters define mode. Passing varidp == nullptr
    // works.
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    EXPECT_EQ(NC_NOERR,
              nc_def_var(ncid, "auto_redef_var", NC_INT, 0, nullptr, nullptr));
    int queried_id = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_varid(ncid, "auto_redef_var", &queried_id));
    ASSERT_EQ(NC_NOERR, ncid.close());
  }

  // Opening read-only rejects nc_def_var with NC_EPERM (via NC4_redef).
  {
    ScopedNcFile ro_ncid;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro_ncid.ptr()));
    int unused_varid = -1;
    EXPECT_EQ(NC_EPERM,
              nc_def_var(ro_ncid, "ro_new", NC_INT, 0, nullptr, &unused_varid));
  }

  // Strict NC_CLASSIC_MODEL rejects non-classic types and data-mode defs.
  {
    ScopedNcFile classic_ncid;
    ASSERT_EQ(NC_NOERR, nc_create("def_var_classic.nc",
                                  NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS,
                                  classic_ncid.ptr()));
    int cdim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic_ncid, "c", 2, &cdim));
    int unused_varid = -1;
    EXPECT_EQ(NC_ESTRICTNC3, nc_def_var(classic_ncid, "strict_ubyte", NC_UBYTE,
                                        1, &cdim, &unused_varid));
    EXPECT_EQ(NC_ESTRICTNC3, nc_def_var(classic_ncid, "strict_str", NC_STRING,
                                        1, &cdim, &unused_varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(classic_ncid));
    EXPECT_EQ(NC_ENOTINDEFINE, nc_def_var(classic_ncid, "late_classic", NC_INT,
                                          1, &cdim, &unused_varid));
  }
}

TEST(Hdf5VarTest, DefVarSupportsAllAtomicAndUserTypesAndGlobalNoFill) {
  ScopedMemio memio;
  {
    ScopedNcFile ncid;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("all_types.nc", NC_NETCDF4, 4096, ncid.ptr()));

    int fixed_dim = -1;
    int unlim_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "fixed", 4, &fixed_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "unlim", NC_UNLIMITED, &unlim_dim));

    constexpr std::array<nc_type, 12> kAtomicTypes = {
        NC_BYTE,  NC_CHAR,   NC_SHORT, NC_INT,   NC_FLOAT,  NC_DOUBLE,
        NC_UBYTE, NC_USHORT, NC_UINT,  NC_INT64, NC_UINT64, NC_STRING,
    };
    for (size_t i = 0; i < kAtomicTypes.size(); ++i) {
      SCOPED_TRACE(i);
      const std::string vname = absl::StrFormat("atomic_%zu", i);
      int vid = -1;
      EXPECT_EQ(NC_NOERR, nc_def_var(ncid, vname.c_str(), kAtomicTypes[i], 1,
                                     &fixed_dim, &vid));
    }

    // Setting file-level NC_NOFILL propagates to newly created numeric vars,
    // while NC_STRING retains default fill mode (nc_type_class == NC_STRING).
    int old_fill_mode = -1;
    ASSERT_EQ(NC_NOERR, nc_set_fill(ncid, NC_NOFILL, &old_fill_mode));
    int nofill_int_vid = -1;
    int str_fill_vid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "global_nofill_int", NC_INT, 1,
                                   &fixed_dim, &nofill_int_vid));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "global_nofill_str", NC_STRING, 1,
                                   &fixed_dim, &str_fill_vid));
    int no_fill_out = -1;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_fill(ncid, nofill_int_vid, &no_fill_out, nullptr));
    EXPECT_EQ(NC_NOFILL, no_fill_out);
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_fill(ncid, str_fill_vid, &no_fill_out, nullptr));
    EXPECT_EQ(NC_FILL, no_fill_out);
    ASSERT_EQ(NC_NOERR, nc_set_fill(ncid, NC_FILL, &old_fill_mode));

    // Unlimited dimension automatically selects NC_CHUNKED storage.
    int unlim_vid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "unlim_var", NC_INT, 1, &unlim_dim, &unlim_vid));
    int storage_out = -1;
    size_t chunk_out = 0;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_chunking(ncid, unlim_vid, &storage_out, &chunk_out));
    EXPECT_EQ(NC_CHUNKED, storage_out);

    // User-defined types (compound, vlen, enum, opaque).
    nc_type cmp_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_compound(ncid, sizeof(CustomCompound), "cmp_t",
                                        &cmp_type));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(ncid, cmp_type, "a",
                                 offsetof(CustomCompound, a), NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(ncid, cmp_type, "b",
                                 offsetof(CustomCompound, b), NC_DOUBLE));
    nc_type vlen_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_vlen(ncid, "vlen_t", NC_INT, &vlen_type));
    nc_type enum_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(ncid, NC_INT, "enum_t", &enum_type));
    int32_t enum_val = 1;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(ncid, enum_type, "ONE", &enum_val));
    nc_type opaque_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(ncid, 4, "opq_t", &opaque_type));

    int cmp_vid = -1;
    int vlen_vid = -1;
    int enum_vid = -1;
    int opq_vid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "cmp_var", cmp_type, 1, &fixed_dim, &cmp_vid));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "vlen_var", vlen_type, 1, &fixed_dim,
                                   &vlen_vid));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "enum_var", enum_type, 1, &fixed_dim,
                                   &enum_vid));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "opq_var", opaque_type, 1, &fixed_dim,
                                   &opq_vid));

    // Coordinate variable defined in same group as dimension.
    int coord_vid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "fixed", NC_INT, 1, &fixed_dim, &coord_vid));
    const std::array<int32_t, 4> coord_vals = {10, 20, 30, 40};
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, coord_vid, coord_vals.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(ncid.release(), memio.get()));
  }

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open_mem("all_types.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro.ptr()));
  int coord_vid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "fixed", &coord_vid));
  std::vector<int32_t> read_back(4, -1);
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, coord_vid, read_back.data()));
  EXPECT_THAT(read_back, ElementsAre(10, 20, 30, 40));
}

TEST(Hdf5VarTest, DefVarHandlesSecretNamesAndPostEnddefCoordReplacement) {
  // Scalar variable with exact NC_MAX_NAME matching a dimension exceeds
  // NC_MAX_NAME once NON_COORD_PREPEND is added -> NC_EMAXNAME.
  // NC4_def_var appends the NC_VAR_INFO_T to grp->vars before
  // give_var_secret_name fails, so enter redef first and abort() to discard
  // the failed metadata cleanly without calling sync_netcdf4_file.
  {
    ScopedNcFile bad_ncid;
    ASSERT_EQ(NC_NOERR, nc_create_mem("secret_max_err.nc", NC_NETCDF4, 4096,
                                      bad_ncid.ptr()));
    int max_dim = -1;
    const std::string max_len_name(NC_MAX_NAME, 'm');
    ASSERT_EQ(NC_NOERR,
              nc_def_dim(bad_ncid, max_len_name.c_str(), 2, &max_dim));
    ASSERT_EQ(NC_NOERR, nc_enddef(bad_ncid));
    ASSERT_EQ(NC_NOERR, nc_redef(bad_ncid));
    int unused_varid = -1;
    EXPECT_EQ(NC_EMAXNAME, nc_def_var(bad_ncid, max_len_name.c_str(), NC_INT, 0,
                                      nullptr, &unused_varid));
    EXPECT_EQ(NC_NOERR, bad_ncid.abort());
  }

  TempFile file("hdf5var_secret_and_coord.nc");

  {
    ScopedNcFile ncid;
    ASSERT_EQ(NC_NOERR,
              nc_create(file.c_str(), NC_NETCDF4 | NC_CLOBBER, ncid.ptr()));

    int lat_dim = -1;
    int lon_dim = -1;
    int coord_later_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lat", 2, &lat_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lon", 3, &lon_dim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "coord_later", 3, &coord_later_dim));

    // Non-coordinate 2D variable sharing "lat" name, with "lon" as dim[0],
    // triggers give_var_secret_name.
    const std::array<int, 2> flipped_dims = {lon_dim, lat_dim};
    int non_coord_2d = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lat", NC_INT, 2, flipped_dims.data(),
                                   &non_coord_2d));

    // Non-coordinate scalar variable sharing "lon" name triggers
    // give_var_secret_name via !var->ndims.
    int non_coord_scalar = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "lon", NC_INT, 0, nullptr, &non_coord_scalar));

    // Child group variable sharing parent dimension's name uses parent dim
    // (dim_grp != grp -> not a coordinate variable, no secret name needed).
    int sub_ncid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(ncid, "sub", &sub_ncid));
    int child_var = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(sub_ncid, "lat", NC_INT, 1, &lat_dim, &child_var));

    // Variable using coord_later BEFORE a coordinate var exists so enddef
    // creates and attaches a DIM_WITHOUT_VARIABLE dataset for coord_later.
    int obs_vid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "obs", NC_INT, 1, &coord_later_dim, &obs_vid));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

    const std::array<int32_t, 6> lat_vals = {1, 2, 3, 4, 5, 6};
    const int32_t scalar_val = 77;
    const std::array<int32_t, 2> child_vals = {88, 99};
    const std::array<int32_t, 3> obs_vals = {10, 20, 30};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, non_coord_2d, lat_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, non_coord_scalar, &scalar_val));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sub_ncid, child_var, child_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, obs_vid, obs_vals.data()));

    // Defining coordinate variable "coord_later" after enddef unlinks the
    // existing DIM_WITHOUT_VARIABLE dataset after detaching it from "obs".
    int coord_later_vid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "coord_later", NC_DOUBLE, 1,
                                   &coord_later_dim, &coord_later_vid));
    const std::array<double, 3> coord_vals = {1.5, 2.5, 3.5};
    ASSERT_EQ(NC_NOERR,
              nc_put_var_double(ncid, coord_later_vid, coord_vals.data()));
    ASSERT_EQ(NC_NOERR, ncid.close());
  }

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));

  int vid = -1;
  std::vector<int32_t> out2d(6, -1);
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "lat", &vid));
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, vid, out2d.data()));
  EXPECT_THAT(out2d, ElementsAre(1, 2, 3, 4, 5, 6));

  int32_t scalar_out = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "lon", &vid));
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, vid, &scalar_out));
  EXPECT_EQ(77, scalar_out);

  int sub_ro = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(ro, "sub", &sub_ro));
  std::vector<int32_t> child_out(2, -1);
  ASSERT_EQ(NC_NOERR, nc_inq_varid(sub_ro, "lat", &vid));
  ASSERT_EQ(NC_NOERR, nc_get_var_int(sub_ro, vid, child_out.data()));
  EXPECT_THAT(child_out, ElementsAre(88, 99));

  std::vector<double> coord_out(3, 0.0);
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "coord_later", &vid));
  ASSERT_EQ(NC_NOERR, nc_get_var_double(ro, vid, coord_out.data()));
  EXPECT_THAT(coord_out, ElementsAre(1.5, 2.5, 3.5));

  std::vector<int32_t> obs_out(3, -1);
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "obs", &vid));
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, vid, obs_out.data()));
  EXPECT_THAT(obs_out, ElementsAre(10, 20, 30));
}

TEST(Hdf5VarTest, DefVarExtraFiltersChunkingFillAndEndianValidation) {
  ScopedMemio memio;
  {
    ScopedNcFile ncid;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("extra_settings.nc", NC_NETCDF4, 8192, ncid.ptr()));

    int d4 = -1;
    int d6 = -1;
    int d_unlim = -1;
    int d_huge = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d4", 4, &d4));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d6", 6, &d6));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dunlim", NC_UNLIMITED, &d_unlim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dhuge", 16385, &d_huge));

    const std::array<int, 2> dims2 = {d4, d6};
    int v_scalar = -1;
    int v_flt = -1;
    int v_unlim = -1;
    int v_def_chunk = -1;
    int v_ints_chunk = -1;
    int v_compact_ok = -1;
    int v_compact_huge = -1;
    int v_fill = -1;
    int v_fill_null = -1;
    int v_str = -1;
    int v_char = -1;
    int v_big_i32 = -1;
    int v_big_f64 = -1;

    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_scalar", NC_INT, 0, nullptr, &v_scalar));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_flt", NC_INT, 2, dims2.data(), &v_flt));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_unlim", NC_INT, 1, &d_unlim, &v_unlim));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_def_chunk", NC_INT, 2, dims2.data(),
                                   &v_def_chunk));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_ints_chunk", NC_INT, 2,
                                   dims2.data(), &v_ints_chunk));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_compact_ok", NC_INT, 2,
                                   dims2.data(), &v_compact_ok));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_compact_huge", NC_INT, 1, &d_huge,
                                   &v_compact_huge));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_fill", NC_INT, 1, &d4, &v_fill));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_fill_null", NC_INT, 1, &d4, &v_fill_null));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_str", NC_STRING, 1, &d4, &v_str));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_char", NC_CHAR, 1, &d4, &v_char));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_big_i32", NC_INT, 1, &d4, &v_big_i32));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_big_f64", NC_DOUBLE, 1, &d4, &v_big_f64));

    // Invalid group ID and invalid varid errors across nc_def_var_* APIs.
    EXPECT_EQ(NC_EBADID, nc_def_var_deflate(BogusGrpId(ncid), v_flt, 0, 1, 3));
    EXPECT_EQ(NC_EBADID,
              nc_def_var_fletcher32(BogusGrpId(ncid), v_flt, NC_FLETCHER32));
    EXPECT_EQ(NC_EBADID, nc_def_var_chunking(BogusGrpId(ncid), v_flt,
                                             NC_CONTIGUOUS, nullptr));
    EXPECT_EQ(NC_EBADID, nc_def_var_chunking_ints(BogusGrpId(ncid), v_flt,
                                                  NC_CONTIGUOUS, nullptr));
    EXPECT_EQ(NC_EBADID,
              nc_def_var_fill(BogusGrpId(ncid), v_fill, NC_NOFILL, nullptr));
    EXPECT_EQ(NC_EBADID,
              nc_def_var_endian(BogusGrpId(ncid), v_flt, NC_ENDIAN_LITTLE));

    EXPECT_EQ(NC_ENOTVAR, nc_def_var_deflate(ncid, 99, 0, 1, 3));
    EXPECT_EQ(NC_ENOTVAR, nc_def_var_fletcher32(ncid, 99, NC_FLETCHER32));
    EXPECT_EQ(NC_ENOTVAR,
              nc_def_var_chunking_ints(ncid, 99, NC_CONTIGUOUS, nullptr));

    // Scalar variable filter/chunking rules.
    EXPECT_EQ(NC_EINVAL, nc_def_var_deflate(ncid, v_scalar, NC_SHUFFLE, 0, 0));
    EXPECT_EQ(NC_EINVAL, nc_def_var_fletcher32(ncid, v_scalar, NC_FLETCHER32));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_chunking(ncid, v_scalar, NC_CHUNKED, nullptr));
    EXPECT_EQ(NC_NOERR,
              nc_def_var_chunking_ints(ncid, v_scalar, NC_CONTIGUOUS, nullptr));

    // Shuffle + deflate_level==0 skips deflate; invalid levels fail NC_EINVAL
    // before deflate is registered; valid call adds deflate; deflate==0 skips
    // deflate filter updates while updating shuffle.
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(ncid, v_flt, NC_SHUFFLE, 1, 0));
    int shuf_out = -1;
    int defl_out = -1;
    int level_out = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(ncid, v_flt, &shuf_out, &defl_out,
                                           &level_out));
    EXPECT_EQ(NC_SHUFFLE, shuf_out);
    EXPECT_EQ(0, defl_out);

    EXPECT_EQ(NC_EINVAL, nc_def_var_deflate(ncid, v_flt, NC_NOSHUFFLE, 1, 10));
    EXPECT_EQ(NC_EINVAL, nc_def_var_deflate(ncid, v_flt, NC_SHUFFLE, 1, -1));
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(ncid, v_flt, NC_SHUFFLE, 1, 5));
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(ncid, v_flt, NC_NOSHUFFLE, 0, 5));

    // Fletcher32 twice (absent -> present) and NC_NOCHECKSUM (ignored cleanly).
    ASSERT_EQ(NC_NOERR, nc_def_var_fletcher32(ncid, v_flt, NC_FLETCHER32));
    ASSERT_EQ(NC_NOERR, nc_def_var_fletcher32(ncid, v_flt, NC_FLETCHER32));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fletcher32(ncid, v_compact_ok, NC_NOCHECKSUM));

    // Cannot set NC_CONTIGUOUS on filtered or unlimited vars.
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_chunking(ncid, v_flt, NC_CONTIGUOUS, nullptr));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_chunking(ncid, v_unlim, NC_CONTIGUOUS, nullptr));

    // Bad chunksizes: total > NC_MAX_UINT triggers NC_EBADCHUNK in
    // nc4_check_chunksizes; fixed chunk > dim len triggers NC_EBADCHUNK
    // directly.
    const size_t oversize_chunk = static_cast<size_t>(NC_MAX_UINT / 2);
    EXPECT_EQ(NC_EBADCHUNK,
              nc_def_var_chunking(ncid, v_unlim, NC_CHUNKED, &oversize_chunk));
    const std::array<size_t, 2> too_wide_chunks = {5, 2};
    EXPECT_EQ(NC_EBADCHUNK, nc_def_var_chunking(ncid, v_def_chunk, NC_CHUNKED,
                                                too_wide_chunks.data()));

    // Toggle non-scalar storage between NC_CHUNKED (with nullptr keeping
    // defaults) and NC_CONTIGUOUS, and exercise NC_NOFILL when no fill_value
    // was ever stored on v_fill_null.
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(ncid, v_def_chunk, NC_CHUNKED, nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(ncid, v_def_chunk, NC_CONTIGUOUS, nullptr));
    int storage_def = -1;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_chunking(ncid, v_def_chunk, &storage_def, nullptr));
    EXPECT_EQ(NC_CONTIGUOUS, storage_def);
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(ncid, v_def_chunk, NC_CHUNKED, nullptr));

    // Fortran *_ints chunking wrapper.
    std::array<int, 2> int_chunks = {2, 3};
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking_ints(ncid, v_ints_chunk, NC_CHUNKED,
                                                 int_chunks.data()));
    int storage_ints = -1;
    std::array<int, 2> chunks_ints_out = {-1, -1};
    ASSERT_EQ(NC_NOERR,
              nc_inq_var_chunking_ints(ncid, v_ints_chunk, &storage_ints,
                                       chunks_ints_out.data()));
    EXPECT_EQ(NC_CHUNKED, storage_ints);
    EXPECT_THAT(chunks_ints_out, ElementsAre(2, 3));

    // Compact storage: <= 64 KB succeeds, > 64 KB returns NC_EVARSIZE.
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(ncid, v_compact_ok, NC_COMPACT, nullptr));
    EXPECT_EQ(NC_EVARSIZE,
              nc_def_var_chunking(ncid, v_compact_huge, NC_COMPACT, nullptr));

    // Fill mode & fill value creation, overwrite, reclaim, and NC_STRING error.
    EXPECT_EQ(NC_EINVAL, nc_def_var_fill(ncid, v_str, NC_NOFILL, nullptr));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_fill_null, NC_FILL, nullptr));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_fill_null, NC_NOFILL, nullptr));
    const int32_t fv1 = 111;
    const int32_t fv2 = 222;
    const int32_t fv3 = 333;
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_fill, NC_FILL, &fv1));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_fill, NC_FILL, &fv2));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_fill, NC_NOFILL, nullptr));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_fill, NC_FILL, &fv3));

    // Endianness across all 10 integer/float atomic types and rejected types.
    constexpr std::array<nc_type, 10> kEndianTypes = {
        NC_BYTE,  NC_SHORT,  NC_INT,  NC_FLOAT, NC_DOUBLE,
        NC_UBYTE, NC_USHORT, NC_UINT, NC_INT64, NC_UINT64,
    };
    for (size_t i = 0; i < kEndianTypes.size(); ++i) {
      SCOPED_TRACE(i);
      const std::string ename = absl::StrFormat("end_%zu", i);
      int evar = -1;
      ASSERT_EQ(NC_NOERR, nc_def_var(ncid, ename.c_str(), kEndianTypes[i], 1,
                                     &d4, &evar));
      EXPECT_EQ(NC_NOERR, nc_def_var_endian(ncid, evar, NC_ENDIAN_LITTLE));
      EXPECT_EQ(NC_NOERR, nc_def_var_endian(ncid, evar, NC_ENDIAN_BIG));
      EXPECT_EQ(NC_NOERR, nc_def_var_endian(ncid, evar, NC_ENDIAN_NATIVE));
    }
    EXPECT_EQ(NC_EINVAL, nc_def_var_endian(ncid, v_char, NC_ENDIAN_BIG));
    EXPECT_EQ(NC_EINVAL, nc_def_var_endian(ncid, v_str, NC_ENDIAN_BIG));

    nc_type enum_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(ncid, NC_INT, "end_enum_t", &enum_type));
    int32_t enum_zero = 0;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(ncid, enum_type, "ZERO", &enum_zero));
    int v_enum = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_enum", enum_type, 1, &d4, &v_enum));
    EXPECT_EQ(NC_EINVAL, nc_def_var_endian(ncid, v_enum, NC_ENDIAN_BIG));

    ASSERT_EQ(NC_NOERR, nc_def_var_endian(ncid, v_big_i32, NC_ENDIAN_BIG));
    ASSERT_EQ(NC_NOERR, nc_def_var_endian(ncid, v_big_f64, NC_ENDIAN_BIG));

    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

    // Writing after nc_enddef triggers NC_ELATEDEF for metadata-altering calls.
    EXPECT_EQ(NC_ELATEDEF, nc_def_var_deflate(ncid, v_flt, NC_SHUFFLE, 1, 2));
    EXPECT_EQ(NC_ELATEDEF, nc_def_var_fletcher32(ncid, v_flt, NC_FLETCHER32));
    EXPECT_EQ(NC_ELATEDEF,
              nc_def_var_chunking(ncid, v_def_chunk, NC_CONTIGUOUS, nullptr));
    EXPECT_EQ(NC_ELATEDEF, nc_def_var_fill(ncid, v_fill, NC_FILL, &fv1));
    EXPECT_EQ(NC_ELATEDEF,
              nc_def_var_endian(ncid, v_big_i32, NC_ENDIAN_LITTLE));

    const std::array<int32_t, 4> big_i32_vals = {0x12345678, -42, 0, 999};
    const std::array<double, 4> big_f64_vals = {3.25, -1.5, 0.0, 128.75};
    std::array<int32_t, 24> compact_vals{};
    compact_vals.fill(7);
    ASSERT_EQ(NC_NOERR,
              nc_put_var_int(ncid, v_compact_ok, compact_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v_big_i32, big_i32_vals.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_var_double(ncid, v_big_f64, big_f64_vals.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(ncid.release(), memio.get()));
  }

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open_mem("extra_settings.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro.ptr()));

  // Read-only file returns NC_EPERM on def_var_extra setters.
  int v_big_i32 = -1;
  int v_big_f64 = -1;
  int v_fill = -1;
  int v_compact_ok = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v_big_i32", &v_big_i32));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v_big_f64", &v_big_f64));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v_fill", &v_fill));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v_compact_ok", &v_compact_ok));

  EXPECT_EQ(NC_EPERM, nc_def_var_endian(ro, v_big_i32, NC_ENDIAN_LITTLE));
  EXPECT_EQ(NC_EPERM, nc_def_var_fill(ro, v_fill, NC_NOFILL, nullptr));

  int endian_out = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_endian(ro, v_big_i32, &endian_out));
  EXPECT_EQ(NC_ENDIAN_BIG, endian_out);
  std::vector<int32_t> i32_out(4, 0);
  ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, v_big_i32, i32_out.data()));
  EXPECT_THAT(i32_out, ElementsAre(0x12345678, -42, 0, 999));

  std::vector<double> f64_out(4, 0.0);
  ASSERT_EQ(NC_NOERR, nc_get_var_double(ro, v_big_f64, f64_out.data()));
  EXPECT_THAT(f64_out, ElementsAre(3.25, -1.5, 0.0, 128.75));

  int compact_storage = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking(ro, v_compact_ok, &compact_storage, nullptr));
  EXPECT_EQ(NC_COMPACT, compact_storage);

  int no_fill_out = -1;
  int32_t fv_out = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_fill(ro, v_fill, &no_fill_out, &fv_out));
  EXPECT_EQ(NC_FILL, no_fill_out);
  EXPECT_EQ(333, fv_out);
}

TEST(Hdf5VarTest, DefVarQuantizeValidationAndRoundTrip) {
  ScopedMemio memio;
  {
    ScopedNcFile ncid;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("quantize.nc", NC_NETCDF4, 8192, ncid.ptr()));

    int d4 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "n", 4, &d4));

    int v_int = -1;
    int v_flt_bg = -1;
    int v_flt_gbr = -1;
    int v_flt_br = -1;
    int v_flt_reset = -1;
    int v_scalar_flt = -1;
    int v_dbl_bg = -1;
    int v_dbl_gbr = -1;
    int v_dbl_br = -1;

    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_int", NC_INT, 1, &d4, &v_int));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_flt_bg", NC_FLOAT, 1, &d4, &v_flt_bg));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_flt_gbr", NC_FLOAT, 1, &d4, &v_flt_gbr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_flt_br", NC_FLOAT, 1, &d4, &v_flt_br));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_flt_reset", NC_FLOAT, 1, &d4, &v_flt_reset));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_scalar_flt", NC_FLOAT, 0, nullptr,
                                   &v_scalar_flt));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_dbl_bg", NC_DOUBLE, 1, &d4, &v_dbl_bg));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_dbl_gbr", NC_DOUBLE, 1, &d4, &v_dbl_gbr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_dbl_br", NC_DOUBLE, 1, &d4, &v_dbl_br));

    // Error paths for def_var_quantize and inq_var_quantize.
    EXPECT_EQ(NC_EBADID, nc_def_var_quantize(BogusGrpId(ncid), v_flt_bg,
                                             NC_QUANTIZE_BITGROOM, 3));
    EXPECT_EQ(NC_EBADID, nc_inq_var_quantize(BogusGrpId(ncid), v_flt_bg,
                                             nullptr, nullptr));
    EXPECT_EQ(NC_ENOTVAR,
              nc_def_var_quantize(ncid, 99, NC_QUANTIZE_BITGROOM, 3));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_var_quantize(ncid, 99, nullptr, nullptr));

    EXPECT_EQ(NC_EINVAL, nc_def_var_quantize(ncid, v_flt_bg, 99, 3));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_quantize(ncid, v_int, NC_QUANTIZE_BITGROOM, 3));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_quantize(ncid, v_flt_bg, NC_QUANTIZE_BITGROOM, 0));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_quantize(ncid, v_flt_bg, NC_QUANTIZE_BITGROOM, -2));

    // Upper bounds for NSD (BitGroom / GranularBR) and NSB (BitRound).
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_quantize(ncid, v_flt_bg, NC_QUANTIZE_BITGROOM,
                                  NC_QUANTIZE_MAX_FLOAT_NSD + 1));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_quantize(ncid, v_flt_gbr, NC_QUANTIZE_GRANULARBR,
                                  NC_QUANTIZE_MAX_FLOAT_NSD + 1));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_quantize(ncid, v_flt_br, NC_QUANTIZE_BITROUND,
                                  NC_QUANTIZE_MAX_FLOAT_NSB + 1));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_quantize(ncid, v_dbl_bg, NC_QUANTIZE_BITGROOM,
                                  NC_QUANTIZE_MAX_DOUBLE_NSD + 1));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_quantize(ncid, v_dbl_gbr, NC_QUANTIZE_GRANULARBR,
                                  NC_QUANTIZE_MAX_DOUBLE_NSD + 1));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_quantize(ncid, v_dbl_br, NC_QUANTIZE_BITROUND,
                                  NC_QUANTIZE_MAX_DOUBLE_NSB + 1));

    // Valid quantization configurations and reset to NC_NOQUANTIZE.
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_flt_bg, NC_QUANTIZE_BITGROOM, 3));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_flt_gbr, NC_QUANTIZE_GRANULARBR, 3));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_flt_br, NC_QUANTIZE_BITROUND, 10));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_flt_reset, NC_QUANTIZE_BITGROOM, 3));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_flt_reset, NC_NOQUANTIZE, 99));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_scalar_flt, NC_QUANTIZE_BITGROOM, 3));

    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_dbl_bg, NC_QUANTIZE_BITGROOM, 5));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_dbl_gbr, NC_QUANTIZE_GRANULARBR, 5));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, v_dbl_br, NC_QUANTIZE_BITROUND, 20));

    // Verify nc_inq_var_quantize with both pointers and null pointers.
    int qmode = -1;
    int qnsd = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ncid, v_flt_reset, &qmode, &qnsd));
    EXPECT_EQ(NC_NOQUANTIZE, qmode);
    EXPECT_EQ(0, qnsd);

    qmode = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ncid, v_dbl_br, &qmode, nullptr));
    EXPECT_EQ(NC_QUANTIZE_BITROUND, qmode);
    qnsd = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ncid, v_dbl_br, nullptr, &qnsd));
    EXPECT_EQ(20, qnsd);

    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    EXPECT_EQ(NC_ELATEDEF,
              nc_def_var_quantize(ncid, v_flt_bg, NC_QUANTIZE_BITGROOM, 4));

    const std::array<float, 4> fvals = {0.0F, 3.1415927F, -2.7182817F,
                                        1234.5679F};
    const std::array<double, 4> dvals = {0.0, 3.141592653589793,
                                         -2.718281828459045, 12345.678901234};
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_flt_bg, fvals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_flt_gbr, fvals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_flt_br, fvals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_flt_reset, fvals.data()));
    const float scalar_f_in = 3.1415927F;
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_scalar_flt, &scalar_f_in));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, v_dbl_bg, dvals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, v_dbl_gbr, dvals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, v_dbl_br, dvals.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(ncid.release(), memio.get()));
  }

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open_mem("quantize.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro.ptr()));

  int vid = -1;
  int qmode = -1;
  int qnsd = -1;

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v_flt_bg", &vid));
  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ro, vid, &qmode, &qnsd));
  EXPECT_EQ(NC_QUANTIZE_BITGROOM, qmode);
  EXPECT_EQ(3, qnsd);
  std::vector<float> fout(4, 0.0F);
  ASSERT_EQ(NC_NOERR, nc_get_var_float(ro, vid, fout.data()));
  EXPECT_FLOAT_EQ(0.0F, fout[0]);
  EXPECT_NEAR(3.14F, fout[1], 0.05F);
  EXPECT_NEAR(-2.72F, fout[2], 0.05F);

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v_dbl_gbr", &vid));
  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ro, vid, &qmode, &qnsd));
  EXPECT_EQ(NC_QUANTIZE_GRANULARBR, qmode);
  EXPECT_EQ(5, qnsd);
  std::vector<double> dout(4, 0.0);
  ASSERT_EQ(NC_NOERR, nc_get_var_double(ro, vid, dout.data()));
  EXPECT_NEAR(3.14159, dout[1], 1e-3);

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v_flt_reset", &vid));
  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ro, vid, &qmode, &qnsd));
  EXPECT_EQ(NC_NOQUANTIZE, qmode);
  EXPECT_EQ(0, qnsd);

  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v_scalar_flt", &vid));
  float scalar_f_out = 0.0F;
  ASSERT_EQ(NC_NOERR, nc_get_var_float(ro, vid, &scalar_f_out));
  EXPECT_NEAR(3.14F, scalar_f_out, 0.05F);

  // Read-only file rejects nc_def_var_quantize with NC_EPERM.
  EXPECT_EQ(NC_EPERM, nc_def_var_quantize(ro, vid, NC_QUANTIZE_BITGROOM, 3));
}

TEST(Hdf5VarTest, RenameVarCoversValidationOrderingCoordsAndLazyMeta) {
  // Error checks & classic-model data-mode name-length restriction.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("rename_classic.nc",
                                  NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS,
                                  classic.ptr()));
    int d = -1;
    int v_alpha = -1;
    int v_beta = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic, "d", 2, &d));
    ASSERT_EQ(NC_NOERR, nc_def_var(classic, "alpha", NC_INT, 1, &d, &v_alpha));
    ASSERT_EQ(NC_NOERR, nc_def_var(classic, "beta", NC_INT, 1, &d, &v_beta));

    EXPECT_EQ(NC_EINVAL, nc_rename_var(classic, v_alpha, nullptr));
    EXPECT_EQ(NC_EBADID, nc_rename_var(BogusGrpId(classic), v_alpha, "gamma"));
    const std::string too_long(NC_MAX_NAME + 1, 'r');
    EXPECT_EQ(NC_EMAXNAME, nc_rename_var(classic, v_alpha, too_long.c_str()));
    EXPECT_EQ(NC_EBADNAME, nc_rename_var(classic, v_alpha, ""));
    EXPECT_EQ(NC_EBADNAME, nc_rename_var(classic, v_alpha, "bad/name"));
    EXPECT_EQ(NC_ENOTVAR, nc_rename_var(classic, 99, "gamma"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_var(classic, v_alpha, "alpha"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_var(classic, v_alpha, "beta"));

    const std::array<int32_t, 2> data = {11, 22};
    ASSERT_EQ(NC_NOERR, nc_enddef(classic));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(classic, v_alpha, data.data()));

    // Classic model in data mode forbids longer new names, permits shorter.
    EXPECT_EQ(NC_ENOTINDEFINE, nc_rename_var(classic, v_alpha, "alpha_longer"));
    EXPECT_EQ(NC_NOERR, nc_rename_var(classic, v_alpha, "alp"));
    char name_buf[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR, nc_inq_varname(classic, v_alpha, name_buf));
    EXPECT_STREQ("alp", name_buf);
  }

  // Pre-enddef rename of v(d1) to another dim "d2" creates dim_wo_var and uses
  // a secret HDF5 dataset name.
  {
    TempFile pre_file("hdf5var_rename_pre_enddef.nc");
    {
      ScopedNcFile ncid;
      ASSERT_EQ(NC_NOERR, nc_create(pre_file.c_str(), NC_NETCDF4 | NC_CLOBBER,
                                    ncid.ptr()));
      int d1 = -1;
      int d2 = -1;
      int v1 = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d1", 2, &d1));
      ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d2", 3, &d2));
      ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v1", NC_INT, 1, &d1, &v1));
      ASSERT_EQ(NC_NOERR, nc_rename_var(ncid, v1, "d2"));
      ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
      const std::array<int32_t, 2> vals = {7, 8};
      ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v1, vals.data()));
      ASSERT_EQ(NC_NOERR, ncid.close());
    }
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(pre_file.c_str(), NC_NOWRITE, ro.ptr()));
    int vid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "d2", &vid));
    int ndims = -1;
    int dimid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_inq_var(ro, vid, nullptr, nullptr, &ndims, &dimid, nullptr));
    EXPECT_EQ(1, ndims);
    char dname[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR, nc_inq_dimname(ro, dimid, dname));
    EXPECT_STREQ("d1", dname);
    std::vector<int32_t> out(2, -1);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, vid, out.data()));
    EXPECT_THAT(out, ElementsAre(7, 8));
  }

  // (a) Post-enddef regular and scalar renames preserve HDF5 creation order
  // via NC_TEMP_NAME, and reopening NC_WRITE + renaming varid 1 immediately
  // hits the lazy !var->meta_read path before nc_inq_var*.
  TempFile order_file("hdf5var_rename_order.nc");
  {
    ScopedNcFile ncid;
    ASSERT_EQ(NC_NOERR, nc_create(order_file.c_str(), NC_NETCDF4 | NC_CLOBBER,
                                  ncid.ptr()));
    int d1 = -1;
    int v0 = -1;
    int v1 = -1;
    int v2 = -1;
    int s0 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d1", 3, &d1));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v0", NC_INT, 1, &d1, &v0));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v1", NC_INT, 1, &d1, &v1));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v2", NC_INT, 1, &d1, &v2));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "s0", NC_INT, 0, nullptr, &s0));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

    const std::array<int32_t, 3> a0 = {1, 2, 3};
    const std::array<int32_t, 3> a1 = {4, 5, 6};
    const std::array<int32_t, 3> a2 = {7, 8, 9};
    const int32_t sv = 42;
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v0, a0.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v1, a1.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v2, a2.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, s0, &sv));

    ASSERT_EQ(NC_NOERR, nc_rename_var(ncid, v0, "v0_r"));
    ASSERT_EQ(NC_NOERR, ncid.close());
  }

  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(order_file.c_str(), NC_WRITE, rw.ptr()));
    // Rename varid 1 BEFORE inquiring/reading any var (!var->meta_read path)
    // and shift v2/s0 through NC_TEMP_NAME; also rename scalar varid 3.
    ASSERT_EQ(NC_NOERR, nc_rename_var(rw, 1, "v1_r"));
    ASSERT_EQ(NC_NOERR, nc_rename_var(rw, 3, "s0_r"));
    ASSERT_EQ(NC_NOERR, rw.close());
  }

  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(order_file.c_str(), NC_NOWRITE, ro.ptr()));
    EXPECT_EQ(NC_EPERM, nc_rename_var(ro, 0, "denied"));

    std::array<char, NC_MAX_NAME + 1> n0{};
    std::array<char, NC_MAX_NAME + 1> n1{};
    std::array<char, NC_MAX_NAME + 1> n2{};
    std::array<char, NC_MAX_NAME + 1> n3{};
    ASSERT_EQ(NC_NOERR, nc_inq_varname(ro, 0, n0.data()));
    ASSERT_EQ(NC_NOERR, nc_inq_varname(ro, 1, n1.data()));
    ASSERT_EQ(NC_NOERR, nc_inq_varname(ro, 2, n2.data()));
    ASSERT_EQ(NC_NOERR, nc_inq_varname(ro, 3, n3.data()));
    EXPECT_STREQ("v0_r", n0.data());
    EXPECT_STREQ("v1_r", n1.data());
    EXPECT_STREQ("v2", n2.data());
    EXPECT_STREQ("s0_r", n3.data());

    std::vector<int32_t> o0(3, -1);
    std::vector<int32_t> o1(3, -1);
    std::vector<int32_t> o2(3, -1);
    int32_t os = -1;
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, 0, o0.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, 1, o1.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, 2, o2.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, 3, &os));
    EXPECT_THAT(o0, ElementsAre(1, 2, 3));
    EXPECT_THAT(o1, ElementsAre(4, 5, 6));
    EXPECT_THAT(o2, ElementsAre(7, 8, 9));
    EXPECT_EQ(42, os);
  }

  // (b) Post-enddef coordinate-var reform & break plus subgroup rename to
  // parent dimension's name (dim_grp != grp -> not a coordinate variable).
  TempFile coord_file("hdf5var_rename_coord.nc");
  {
    ScopedNcFile ncid;
    ASSERT_EQ(NC_NOERR, nc_create(coord_file.c_str(), NC_NETCDF4 | NC_CLOBBER,
                                  ncid.ptr()));
    int d1 = -1;
    int v0 = -1;
    int v1 = -1;
    int sub = -1;
    int sub_v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d1", 3, &d1));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v0", NC_INT, 1, &d1, &v0));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v1", NC_INT, 1, &d1, &v1));
    ASSERT_EQ(NC_NOERR, nc_def_grp(ncid, "sub", &sub));
    ASSERT_EQ(NC_NOERR, nc_def_var(sub, "sub_v", NC_INT, 1, &d1, &sub_v));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

    const std::array<int32_t, 3> a1 = {4, 5, 6};
    const std::array<int32_t, 3> asub = {10, 11, 12};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v1, a1.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(sub, sub_v, asub.data()));

    // Reform coord var post-enddef: deletes d1's DIM_WITHOUT_VARIABLE dataset,
    // detaches scales, and renames v0 -> "d1". Sync recreates the H5DS
    // dataset before writing v0's values.
    ASSERT_EQ(NC_NOERR, nc_rename_var(ncid, v0, "d1"));
    ASSERT_EQ(NC_NOERR, nc_sync(ncid));
    const std::array<int32_t, 3> a0 = {1, 2, 3};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v0, a0.data()));
    ASSERT_EQ(NC_NOERR, ncid.close());
  }

  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(coord_file.c_str(), NC_WRITE, rw.ptr()));
    int d1_id = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "d1", &d1_id));
    // nc_inq_varid leaves meta_read == NC_FALSE; renaming breaks coord var.
    ASSERT_EQ(NC_NOERR, nc_rename_var(rw, d1_id, "not_d1"));

    int sub = -1;
    int sub_v = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(rw, "sub", &sub));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sub, "sub_v", &sub_v));
    ASSERT_EQ(NC_NOERR, nc_rename_var(sub, sub_v, "d1"));
    ASSERT_EQ(NC_NOERR, rw.close());
  }

  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(coord_file.c_str(), NC_NOWRITE, ro.ptr()));
    int vid_not_d1 = -1;
    int vid_v1 = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "not_d1", &vid_not_d1));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v1", &vid_v1));
    std::vector<int32_t> o0(3, -1);
    std::vector<int32_t> o1(3, -1);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, vid_not_d1, o0.data()));
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, vid_v1, o1.data()));
    EXPECT_THAT(o0, ElementsAre(1, 2, 3));
    EXPECT_THAT(o1, ElementsAre(4, 5, 6));

    int sub = -1;
    int sub_v = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(ro, "sub", &sub));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(sub, "d1", &sub_v));
    std::vector<int32_t> osub(3, -1);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(sub, sub_v, osub.data()));
    EXPECT_THAT(osub, ElementsAre(10, 11, 12));
  }

  // (c) Post-creation rename to a different newly defined dimension name uses
  // give_var_secret_name + alt_name on an already-created HDF5 dataset.
  TempFile secret_post_file("hdf5var_rename_secret_post.nc");
  {
    ScopedNcFile ncid;
    ASSERT_EQ(NC_NOERR, nc_create(secret_post_file.c_str(),
                                  NC_NETCDF4 | NC_CLOBBER, ncid.ptr()));
    int d1 = -1;
    int v1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d1", 2, &d1));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v1", NC_INT, 1, &d1, &v1));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    const std::array<int32_t, 2> vals = {15, 25};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v1, vals.data()));

    ASSERT_EQ(NC_NOERR, nc_redef(ncid));
    int d2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d2", 3, &d2));
    ASSERT_EQ(NC_NOERR, nc_rename_var(ncid, v1, "d2"));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    ASSERT_EQ(NC_NOERR, ncid.close());
  }

  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR,
              nc_open(secret_post_file.c_str(), NC_NOWRITE, ro.ptr()));
    int vid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "d2", &vid));
    std::vector<int32_t> out(2, -1);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, vid, out.data()));
    EXPECT_THAT(out, ElementsAre(15, 25));
  }
}

TEST(Hdf5VarTest, PutAndGetVarsCoverBoundsConversionsStridesFillAndScalars) {
  // Classic-model define-mode I/O block & BYTE/UBYTE ERANGE suppression vs
  // standard netCDF-4 ERANGE enforcement.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("classic_byte_erange.nc",
                                  NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS,
                                  classic.ptr()));
    int d2 = -1;
    int v_byte = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic, "d2", 2, &d2));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic, "v_byte", NC_BYTE, 1, &d2, &v_byte));

    // In define mode under NC_CLASSIC_MODEL, read/write fail with NC_EINDEFINE.
    const std::array<signed char, 2> sbytes = {10, -5};
    std::array<signed char, 2> sbytes_out = {0, 0};
    EXPECT_EQ(NC_EINDEFINE, nc_put_var_schar(classic, v_byte, sbytes.data()));
    EXPECT_EQ(NC_EINDEFINE,
              nc_get_var_schar(classic, v_byte, sbytes_out.data()));

    ASSERT_EQ(NC_NOERR, nc_enddef(classic));

    // Classic model ignores NC_ERANGE between NC_BYTE and NC_UBYTE on put/get.
    const std::array<unsigned char, 2> ubytes = {200, 250};
    EXPECT_EQ(NC_NOERR, nc_put_var_ubyte(classic, v_byte, ubytes.data()));
    std::vector<unsigned char> ubytes_out(2, 0);
    EXPECT_EQ(NC_NOERR, nc_get_var_ubyte(classic, v_byte, ubytes_out.data()));
    EXPECT_THAT(ubytes_out, ElementsAre(200, 250));
  }

  // Standard netCDF-4: bounds, NULLs, strides, auto-enddef, conversions,
  // scalar, user types, unlimited extensions, and numeric/string/vlen fills.
  ScopedMemio memio;
  {
    ScopedNcFile ncid;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("vars_io.nc", NC_NETCDF4, 16384, ncid.ptr()));

    int d_fix = -1;
    int d_col = -1;
    int d_unlim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "fix", 4, &d_fix));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "col", 3, &d_col));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "unlim", NC_UNLIMITED, &d_unlim));

    int v_int = -1;
    int v_char = -1;
    int v_byte = -1;
    int v_ubyte = -1;
    int v_scalar = -1;
    int v_driver = -1;
    int v_num = -1;
    int v_str_fill = -1;
    int v_str_def = -1;
    int v_vlen = -1;
    int v_mat = -1;
    int v_cmp = -1;
    int v_opq = -1;

    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_int", NC_INT, 1, &d_fix, &v_int));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_char", NC_CHAR, 1, &d_fix, &v_char));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_byte", NC_BYTE, 1, &d_fix, &v_byte));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_ubyte", NC_UBYTE, 1, &d_fix, &v_ubyte));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_scalar", NC_INT, 0, nullptr, &v_scalar));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_driver", NC_INT, 1, &d_unlim, &v_driver));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_num", NC_INT, 1, &d_unlim, &v_num));
    const int32_t num_fv = -777;
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_num, NC_FILL, &num_fv));

    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_str_fill", NC_STRING, 1, &d_unlim,
                                   &v_str_fill));
    const char* str_fv_tmp = "TEMP_FILL";
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fill(ncid, v_str_fill, NC_FILL, &str_fv_tmp));
    const char* str_fv = "MISSING";
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_str_fill, NC_FILL, &str_fv));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_str_def", NC_STRING, 1, &d_unlim,
                                   &v_str_def));

    nc_type vlen_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_vlen(ncid, "ivlen_t", NC_INT, &vlen_t));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_vlen", vlen_t, 1, &d_unlim, &v_vlen));

    const std::array<int, 2> mat_dims = {d_unlim, d_col};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "v_mat", NC_INT, 2, mat_dims.data(), &v_mat));
    const int32_t mat_fv = -55;
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, v_mat, NC_FILL, &mat_fv));

    nc_type cmp_t = NC_NAT;
    ASSERT_EQ(NC_NOERR,
              nc_def_compound(ncid, sizeof(CustomCompound), "io_cmp", &cmp_t));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(ncid, cmp_t, "a", offsetof(CustomCompound, a),
                                 NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(ncid, cmp_t, "b", offsetof(CustomCompound, b),
                                 NC_DOUBLE));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_cmp", cmp_t, 1, &d_fix, &v_cmp));

    nc_type opq_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(ncid, 4, "io_opq", &opq_t));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_opq", opq_t, 1, &d_fix, &v_opq));

    // Non-classic NC_NETCDF4 write while in define mode automatically calls
    // nc4_enddef_netcdf4_file inside check_for_vara.
    const std::array<int32_t, 4> init_ints = {100, 200, -300, 400};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v_int, init_ints.data()));

    // Error paths in NC4_put_vars / NC4_get_vars.
    const size_t z0 = 0;
    const size_t c1 = 1;
    int32_t dummy_i = 0;
    EXPECT_EQ(NC_EBADID,
              nc_put_vara_int(BogusGrpId(ncid), v_int, &z0, &c1, &dummy_i));
    EXPECT_EQ(NC_EBADID,
              nc_get_vara_int(BogusGrpId(ncid), v_int, &z0, &c1, &dummy_i));
    EXPECT_EQ(NC_ENOTVAR, nc_put_vara_int(ncid, 99, &z0, &c1, &dummy_i));
    EXPECT_EQ(NC_ENOTVAR, nc_get_vara_int(ncid, 99, &z0, &c1, &dummy_i));

    // NC_ECHAR conversions rejected.
    char cbuf[4]{'a', 'b', 'c', 'd'};
    EXPECT_EQ(NC_ECHAR, nc_put_vara_int(ncid, v_char, &z0, &c1, &dummy_i));
    EXPECT_EQ(NC_ECHAR, nc_get_vara_int(ncid, v_char, &z0, &c1, &dummy_i));
    EXPECT_EQ(NC_ECHAR, nc_put_vara_text(ncid, v_int, &z0, &c1, cbuf));
    EXPECT_EQ(NC_ECHAR, nc_get_vara_text(ncid, v_int, &z0, &c1, cbuf));

    // Non-positive strides rejected with NC_ESTRIDE.
    const ptrdiff_t zero_stride = 0;
    const ptrdiff_t neg_stride = -2;
    EXPECT_EQ(NC_ESTRIDE,
              nc_put_vars_int(ncid, v_int, &z0, &c1, &zero_stride, &dummy_i));
    EXPECT_EQ(NC_ESTRIDE,
              nc_get_vars_int(ncid, v_int, &z0, &c1, &neg_stride, &dummy_i));

    // NULL data rejected on non-zero read, allowed on zero-count read/write
    // (both native and type-converting paths where len == 0).
    EXPECT_EQ(NC_EINVAL, nc_get_vara_int(ncid, v_int, &z0, &c1, nullptr));
    EXPECT_EQ(NC_EINVAL, nc_get_var_int(ncid, v_scalar, nullptr));
    const size_t c0 = 0;
    const size_t start_at_end = 4;
    EXPECT_EQ(NC_NOERR,
              nc_put_vara_int(ncid, v_int, &start_at_end, &c0, nullptr));
    EXPECT_EQ(NC_NOERR,
              nc_get_vara_int(ncid, v_int, &start_at_end, &c0, nullptr));
    double dummy_d = 0.0;
    EXPECT_EQ(NC_NOERR,
              nc_put_vara_double(ncid, v_int, &start_at_end, &c0, &dummy_d));
    EXPECT_EQ(NC_NOERR,
              nc_get_vara_double(ncid, v_int, &start_at_end, &c0, &dummy_d));

    // Fixed-dim bounds checks on put and get.
    const size_t start_gt = 5;
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_put_vara_int(ncid, v_int, &start_gt, &c1, &dummy_i));
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_put_vara_int(ncid, v_int, &start_at_end, &c1, &dummy_i));
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_vara_int(ncid, v_int, &start_gt, &c1, &dummy_i));
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_vara_int(ncid, v_int, &start_at_end, &c1, &dummy_i));

    const size_t start2 = 2;
    const size_t count3 = 3;
    const std::array<int32_t, 3> three_ints = {1, 2, 3};
    std::array<int32_t, 3> three_out = {};
    EXPECT_EQ(NC_EEDGE, nc_put_vara_int(ncid, v_int, &start2, &count3,
                                        three_ints.data()));
    EXPECT_EQ(NC_EEDGE,
              nc_get_vara_int(ncid, v_int, &start2, &count3, three_out.data()));

    // Type conversion & NC_ERANGE on plain NC_NETCDF4 (including BYTE/UBYTE).
    const std::array<unsigned char, 4> uvals = {10, 200, 5, 255};
    EXPECT_EQ(NC_ERANGE, nc_put_var_ubyte(ncid, v_byte, uvals.data()));
    const std::array<signed char, 4> svals = {-5, 10, 20, -1};
    EXPECT_EQ(NC_ERANGE, nc_put_var_schar(ncid, v_ubyte, svals.data()));

    const std::array<long long, 4> overflow_ll = {1LL, 3000000000LL, 3LL, 4LL};
    EXPECT_EQ(NC_ERANGE, nc_put_var_longlong(ncid, v_int, overflow_ll.data()));
    const std::array<double, 4> in_range_dbl = {1.0, 2.0, 3.0, 4.0};
    ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, v_int, in_range_dbl.data()));
    // Restore clean values in v_int and verify converted & strided reads.
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v_int, init_ints.data()));
    std::array<signed char, 4> erange_out = {};
    EXPECT_EQ(NC_ERANGE, nc_get_var_schar(ncid, v_int, erange_out.data()));
    const ptrdiff_t stride2 = 2;
    const size_t count2 = 2;
    std::vector<double> strided_dbl(2, 0.0);
    ASSERT_EQ(NC_NOERR, nc_get_vars_double(ncid, v_int, &z0, &count2, &stride2,
                                           strided_dbl.data()));
    EXPECT_THAT(strided_dbl, ElementsAre(100.0, -300.0));

    // Scalar converted write/read and scalar ERANGE.
    const double scalar_dbl_in = 300.0;
    ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, v_scalar, &scalar_dbl_in));
    double scalar_dbl_out = 0.0;
    ASSERT_EQ(NC_NOERR, nc_get_var_double(ncid, v_scalar, &scalar_dbl_out));
    EXPECT_DOUBLE_EQ(300.0, scalar_dbl_out);
    signed char scalar_schar_out = 0;
    EXPECT_EQ(NC_ERANGE, nc_get_var_schar(ncid, v_scalar, &scalar_schar_out));

    // User-defined types via nc_put_vara / nc_get_vara (hits mem_nc_type >=
    // NC_FIRSTUSERTYPEID handling in NC4_put_vars).
    const std::array<size_t, 1> ustart = {0};
    const std::array<size_t, 1> ucount = {2};
    const std::array<CustomCompound, 2> cmp_in = {{{1, 1.5}, {2, 2.5}}};
    ASSERT_EQ(NC_NOERR, nc_put_vara(ncid, v_cmp, ustart.data(), ucount.data(),
                                    cmp_in.data()));
    std::array<CustomCompound, 2> cmp_out{};
    ASSERT_EQ(NC_NOERR, nc_get_vara(ncid, v_cmp, ustart.data(), ucount.data(),
                                    cmp_out.data()));
    EXPECT_EQ(cmp_out[0].a, 1);
    EXPECT_DOUBLE_EQ(cmp_out[1].b, 2.5);

    const std::array<uint8_t, 8> opq_in = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_EQ(NC_NOERR, nc_put_vara(ncid, v_opq, ustart.data(), ucount.data(),
                                    opq_in.data()));
    std::vector<uint8_t> opq_out(8, 0);
    ASSERT_EQ(NC_NOERR, nc_get_vara(ncid, v_opq, ustart.data(), ucount.data(),
                                    opq_out.data()));
    EXPECT_THAT(opq_out, ElementsAre(1, 2, 3, 4, 5, 6, 7, 8));

    // Unlimited-dimension extension (vara, vars, non-extending subset,
    // zero-count write) and fill reads beyond dataset fdims.
    const std::array<int32_t, 3> drv_strided = {10, 30, 50};
    const size_t s1 = 1;
    const size_t c3 = 3;
    ASSERT_EQ(NC_NOERR, nc_put_vars_int(ncid, v_driver, &s1, &c3, &stride2,
                                        drv_strided.data()));
    // Write within existing fdims[0] == 6 (no extension).
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_int(ncid, v_driver, &z0, &c1, &drv_strided[0]));
    // Zero-count write at start == 10 does not extend dim length.
    const size_t s10 = 10;
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(ncid, v_driver, &s10, &c0, nullptr));
    // Extend unlim dim length to 9 via v_driver.
    const std::array<int32_t, 3> drv_tail = {70, 80, 90};
    const size_t s6 = 6;
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_int(ncid, v_driver, &s6, &c3, drv_tail.data()));
    size_t ulen = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(ncid, d_unlim, &ulen));
    EXPECT_EQ(9U, ulen);

    // Populate shorter fdims on v_num (4), v_str_fill (2), v_str_def (1),
    // v_vlen (2), and 2D v_mat (2x3).
    const std::array<int32_t, 4> num_in = {10, 20, 30, 40};
    const size_t c4 = 4;
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(ncid, v_num, &z0, &c4, num_in.data()));

    std::array<const char*, 2> str_in = {"s0", "s1"};
    ASSERT_EQ(NC_NOERR, nc_put_vara_string(ncid, v_str_fill, &z0, &count2,
                                           str_in.data()));
    const char* str_def_in = "first";
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_string(ncid, v_str_def, &z0, &c1, &str_def_in));

    std::array<int32_t, 2> ve0 = {11, 12};
    std::array<int32_t, 1> ve1 = {21};
    const std::array<nc_vlen_t, 2> vlen_in = {{
        {ve0.size(), ve0.data()},
        {ve1.size(), ve1.data()},
    }};
    ASSERT_EQ(NC_NOERR,
              nc_put_vara(ncid, v_vlen, &z0, &count2, vlen_in.data()));

    const std::array<size_t, 2> mat_start = {0, 0};
    const std::array<size_t, 2> mat_count = {2, 3};
    const std::array<int32_t, 6> mat_in = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(ncid, v_mat, mat_start.data(),
                                        mat_count.data(), mat_in.data()));

    // Unlimited-dim get bounds errors against ulen (9).
    const size_t s9 = 9;
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_vara_int(ncid, v_num, &s10, &c1, &dummy_i));
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_vara_int(ncid, v_num, &s9, &c1, &dummy_i));
    EXPECT_EQ(NC_NOERR, nc_get_vara_int(ncid, v_num, &s9, &c0, nullptr));
    EXPECT_EQ(NC_EEDGE,
              nc_get_vara_int(ncid, v_num, &s6, &c4, three_out.data()));

    // All-fill unlimited read (start >= fdims): native int and converted
    // double.
    const size_t s5 = 5;
    std::vector<int32_t> fill_only_i(3, 0);
    ASSERT_EQ(NC_NOERR,
              nc_get_vara_int(ncid, v_num, &s5, &c3, fill_only_i.data()));
    EXPECT_THAT(fill_only_i, ElementsAre(-777, -777, -777));

    std::vector<double> fill_only_d(3, 0.0);
    ASSERT_EQ(NC_NOERR,
              nc_get_vara_double(ncid, v_num, &s5, &c3, fill_only_d.data()));
    EXPECT_THAT(fill_only_d, ElementsAre(-777.0, -777.0, -777.0));

    // Partial-fill unstrided read (start=2, count=5 -> [30, 40, -777*3]).
    const size_t c5 = 5;
    std::vector<int32_t> partial_i(5, 0);
    ASSERT_EQ(NC_NOERR,
              nc_get_vara_int(ncid, v_num, &start2, &c5, partial_i.data()));
    EXPECT_THAT(partial_i, ElementsAre(30, 40, -777, -777, -777));

    // Partial-fill strided reads verifying ceiling-division math in HDF5 var:
    // fdims=4, ulen=9, stride=2:
    //   start=1, count=4 -> indices 1,3 real + 5,7 fill
    //   start=0, count=4 -> indices 0,2 real + 4,6 fill
    std::vector<int32_t> strided_fill_odd(4, 0);
    ASSERT_EQ(NC_NOERR, nc_get_vars_int(ncid, v_num, &s1, &c4, &stride2,
                                        strided_fill_odd.data()));
    EXPECT_THAT(strided_fill_odd, ElementsAre(20, 40, -777, -777));

    std::vector<int32_t> strided_fill_even(4, 0);
    ASSERT_EQ(NC_NOERR, nc_get_vars_int(ncid, v_num, &z0, &c4, &stride2,
                                        strided_fill_even.data()));
    EXPECT_THAT(strided_fill_even, ElementsAre(10, 30, -777, -777));

    // Unlimited NC_STRING partial-fill reads (custom fill & default fill).
    {
      ScopedNcStrings s_fill_out(4);
      ASSERT_EQ(NC_NOERR, nc_get_vara_string(ncid, v_str_fill, &z0, &c4,
                                             s_fill_out.data()));
      EXPECT_STREQ("s0", s_fill_out[0]);
      EXPECT_STREQ("s1", s_fill_out[1]);
      EXPECT_STREQ("MISSING", s_fill_out[2]);
      EXPECT_STREQ("MISSING", s_fill_out[3]);

      ScopedNcStrings s_def_out(3);
      ASSERT_EQ(NC_NOERR, nc_get_vara_string(ncid, v_str_def, &z0, &c3,
                                             s_def_out.data()));
      EXPECT_STREQ("first", s_def_out[0]);
      EXPECT_STREQ("", s_def_out[1]);
      EXPECT_STREQ("", s_def_out[2]);
    }

    // Unlimited NC_VLEN partial-fill read; reclaim BEFORE closing ncid.
    {
      std::vector<nc_vlen_t> vlen_out(4, nc_vlen_t{});
      absl::Cleanup reclaim = [&]() {
        EXPECT_EQ(NC_NOERR, nc_reclaim_data(ncid, vlen_t, vlen_out.data(),
                                            vlen_out.size()));
      };
      ASSERT_EQ(NC_NOERR, nc_get_vara(ncid, v_vlen, &z0, &c4, vlen_out.data()));
      ASSERT_EQ(2U, vlen_out[0].len);
      EXPECT_EQ(11, static_cast<const int32_t*>(vlen_out[0].p)[0]);
      EXPECT_EQ(12, static_cast<const int32_t*>(vlen_out[0].p)[1]);
      ASSERT_EQ(1U, vlen_out[1].len);
      EXPECT_EQ(21, static_cast<const int32_t*>(vlen_out[1].p)[0]);
      EXPECT_EQ(0U, vlen_out[2].len);
      EXPECT_EQ(0U, vlen_out[3].len);
    }

    // 2D partial-fill read (row 1 real + rows 2..3 filled with -55).
    const std::array<size_t, 2> r_start = {1, 0};
    const std::array<size_t, 2> r_count = {3, 3};
    std::vector<int32_t> mat_out(9, 0);
    ASSERT_EQ(NC_NOERR, nc_get_vara_int(ncid, v_mat, r_start.data(),
                                        r_count.data(), mat_out.data()));
    EXPECT_THAT(mat_out, ElementsAre(4, 5, 6, -55, -55, -55, -55, -55, -55));

    ASSERT_EQ(NC_NOERR, nc_close_memio(ncid.release(), memio.get()));
  }
}

TEST(Hdf5VarTest, RawHdf5FixtureExercisesFixedLengthStringsAndMissingFilters) {
  TempFile file("raw_hdf5_var_fixture.h5");
  ASSERT_EQ(0, NetcdfGoogleCreateRawHdf5VarFixture(file.c_str()));

  // Read fixed-length HDF5 strings and verify unregistered filter returns
  // NC_ENOFILTER on get.
  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));

    int v_str1d = -1;
    int v_str_scalar = -1;
    int v_flt = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "fixed_str_1d", &v_str1d));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "fixed_str_scalar", &v_str_scalar));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "missing_filter_var", &v_flt));

    nc_type str_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_inq_vartype(ro, v_str1d, &str_t));
    EXPECT_EQ(NC_STRING, str_t);

    ScopedNcStrings all_strs(3);
    ASSERT_EQ(NC_NOERR, nc_get_var_string(ro, v_str1d, all_strs.data()));
    EXPECT_STREQ("alpha", all_strs[0]);
    EXPECT_STREQ("bravo", all_strs[1]);
    EXPECT_STREQ("12345678", all_strs[2]);

    ScopedNcStrings sub_strs(2);
    const size_t start = 1;
    const size_t count = 2;
    ASSERT_EQ(NC_NOERR,
              nc_get_vara_string(ro, v_str1d, &start, &count, sub_strs.data()));
    EXPECT_STREQ("bravo", sub_strs[0]);
    EXPECT_STREQ("12345678", sub_strs[1]);

    ScopedNcStrings scalar_str(1);
    ASSERT_EQ(NC_NOERR, nc_get_var_string(ro, v_str_scalar, scalar_str.data()));
    EXPECT_STREQ("scalar8!", scalar_str[0]);

    std::vector<int32_t> flt_out(4, -1);
    EXPECT_EQ(NC_ENOFILTER, nc_get_var_int(ro, v_flt, flt_out.data()));
  }

  // Reopen read-write and verify unregistered filter returns NC_ENOFILTER on
  // put as well.
  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_WRITE, rw.ptr()));
    int v_flt = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(rw, "missing_filter_var", &v_flt));
    const std::array<int32_t, 4> flt_in = {1, 2, 3, 4};
    EXPECT_EQ(NC_ENOFILTER, nc_put_var_int(rw, v_flt, flt_in.data()));
    EXPECT_EQ(NC_NOERR, rw.close());
  }
}

TEST(Hdf5VarTest, VarChunkCacheAndLazyInqVarAll) {
  TempFile file("hdf5var_chunk_cache_and_inq.nc");

  {
    ScopedNcFile ncid;
    ASSERT_EQ(NC_NOERR,
              nc_create(file.c_str(), NC_NETCDF4 | NC_CLOBBER, ncid.ptr()));

    int d = -1;
    int vid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "d", 8, &d));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "cvar", NC_FLOAT, 1, &d, &vid));

    const size_t chunk = 4;
    const float fv = -99.5F;
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(ncid, vid, NC_CHUNKED, &chunk));
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(ncid, vid, NC_SHUFFLE, 1, 4));
    ASSERT_EQ(NC_NOERR, nc_def_var_fletcher32(ncid, vid, NC_FLETCHER32));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(ncid, vid, NC_FILL, &fv));
    ASSERT_EQ(NC_NOERR, nc_def_var_endian(ncid, vid, NC_ENDIAN_BIG));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(ncid, vid, NC_QUANTIZE_BITGROOM, 4));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(ncid, vid, "units", std::strlen("m"), "m"));

    // Invalid preemption, bogus grpid, and invalid varid before nc_enddef.
    EXPECT_EQ(NC_EINVAL, nc_set_var_chunk_cache(ncid, vid, 65536, 17, -0.1F));
    EXPECT_EQ(NC_EINVAL, nc_set_var_chunk_cache(ncid, vid, 65536, 17, 1.5F));
    EXPECT_EQ(NC_EBADID,
              nc_set_var_chunk_cache(BogusGrpId(ncid), vid, 65536, 17, 0.5F));
    EXPECT_EQ(NC_ENOTVAR, nc_set_var_chunk_cache(ncid, 99, 65536, 17, 0.5F));
    EXPECT_EQ(NC_EBADID, nc_get_var_chunk_cache(BogusGrpId(ncid), vid, nullptr,
                                                nullptr, nullptr));
    EXPECT_EQ(NC_ENOTVAR,
              nc_get_var_chunk_cache(ncid, 99, nullptr, nullptr, nullptr));

    // Pre-enddef chunk cache update (hdf_datasetid == 0 skips reopen).
    ASSERT_EQ(NC_NOERR,
              nc_set_var_chunk_cache(ncid, vid, 1024 * 1024, 31, 0.25F));

    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

    // Post-enddef chunk cache update (hdf_datasetid != 0 triggers
    // nc4_reopen_dataset) plus Fortran *_ints wrapper with mixed non-negative
    // and negative sentinel args.
    ASSERT_EQ(NC_NOERR,
              nc_set_var_chunk_cache(ncid, vid, 2 * 1024 * 1024, 53, 0.5F));
    size_t csize = 0;
    size_t cnelems = 0;
    float cpre = 0.0F;
    ASSERT_EQ(NC_NOERR,
              nc_get_var_chunk_cache(ncid, vid, &csize, &cnelems, &cpre));
    EXPECT_EQ(2U * 1024U * 1024U, csize);
    EXPECT_EQ(53U, cnelems);
    EXPECT_FLOAT_EQ(0.5F, cpre);

    ASSERT_EQ(NC_NOERR, nc_set_var_chunk_cache_ints(ncid, vid, 4, 101, 25));
    int isize = 0;
    int inelems = 0;
    int ipre = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_var_chunk_cache_ints(ncid, vid, &isize, &inelems, &ipre));
    EXPECT_EQ(4, isize);
    EXPECT_EQ(101, inelems);
    EXPECT_EQ(25, ipre);

    // Mixed non-negative and negative sentinels exercise each ternary branch in
    // nc_set_var_chunk_cache_ints.
    ASSERT_EQ(NC_NOERR, nc_set_var_chunk_cache_ints(ncid, vid, 4, -1, 50));
    ASSERT_EQ(NC_NOERR, nc_set_var_chunk_cache_ints(ncid, vid, -1, 53, -1));
    ASSERT_EQ(NC_NOERR, nc_set_var_chunk_cache_ints(ncid, vid, -1, -1, -1));

    const std::array<float, 8> data = {1.0F, 2.0F, 3.0F, 4.0F,
                                       5.0F, 6.0F, 7.0F, 8.0F};
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, vid, data.data()));
    ASSERT_EQ(NC_NOERR, ncid.close());
  }

  // Fresh reopen: trigger NC4_HDF5_inq_var_all lazy attribute/meta reads and
  // verify nc4_reopen_dataset also works on a reopened file.
  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));

  EXPECT_EQ(NC_EBADID, nc_inq_var(BogusGrpId(ro), 0, nullptr, nullptr, nullptr,
                                  nullptr, nullptr));
  EXPECT_EQ(NC_ENOTVAR,
            nc_inq_var(ro, 99, nullptr, nullptr, nullptr, nullptr, nullptr));

  int global_natts = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_natts(ro, &global_natts));
  EXPECT_EQ(0, global_natts);

  char name[NC_MAX_NAME + 1]{};
  nc_type xtype = NC_NAT;
  int ndims = -1;
  int dimid = -1;
  int natts = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var(ro, 0, name, &xtype, &ndims, &dimid, &natts));
  EXPECT_STREQ("cvar", name);
  EXPECT_EQ(NC_FLOAT, xtype);
  EXPECT_EQ(1, ndims);
  EXPECT_GE(natts, 2);

  int shuf = -1;
  int defl = -1;
  int lvl = -1;
  int f32 = -1;
  int stor = -1;
  size_t csz = 0;
  int nofill = -1;
  float fv = 0.0F;
  int endian = -1;
  int qmode = -1;
  int qnsd = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(ro, 0, &shuf, &defl, &lvl));
  ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(ro, 0, &f32));
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(ro, 0, &stor, &csz));
  ASSERT_EQ(NC_NOERR, nc_inq_var_fill(ro, 0, &nofill, &fv));
  ASSERT_EQ(NC_NOERR, nc_inq_var_endian(ro, 0, &endian));
  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(ro, 0, &qmode, &qnsd));

  EXPECT_EQ(NC_SHUFFLE, shuf);
  EXPECT_EQ(1, defl);
  EXPECT_EQ(4, lvl);
  EXPECT_EQ(NC_FLETCHER32, f32);
  EXPECT_EQ(NC_CHUNKED, stor);
  EXPECT_EQ(4U, csz);
  EXPECT_EQ(NC_FILL, nofill);
  EXPECT_FLOAT_EQ(-99.5F, fv);
  EXPECT_EQ(NC_ENDIAN_BIG, endian);
  EXPECT_EQ(NC_QUANTIZE_BITGROOM, qmode);
  EXPECT_EQ(4, qnsd);

  ASSERT_EQ(NC_NOERR, nc_set_var_chunk_cache(ro, 0, 512 * 1024, 19, 0.75F));
  std::vector<float> read_back(8, 0.0F);
  ASSERT_EQ(NC_NOERR, nc_get_var_float(ro, 0, read_back.data()));
  for (size_t i = 0; i < read_back.size(); ++i) {
    EXPECT_THAT(absl::StrFormat("v=%.2f", read_back[i]), HasSubstr("v="));
    EXPECT_NEAR(static_cast<float>(i + 1), read_back[i], 0.01F);
  }
}

}  // namespace
}  // namespace netcdf
