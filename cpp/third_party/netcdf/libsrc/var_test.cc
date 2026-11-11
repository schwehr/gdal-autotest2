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

// Unit tests for //third_party/netcdf/libsrc/var.c.
//
// Exercises classic-model (CDF-1, CDF-2, CDF-5) variable creation, shape and
// vlen calculations, type sizes across all classic/CDF-5 numeric types,
// lookups, inquiries, renaming, fill-value definitions, UTF-8 normalization,
// dynamic array growth/duplication, and internal var.c helpers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/include/nc3dispatch.h"
#include "third_party/netcdf/include/nc3internal.h"
size_t ncx_szof(nc_type type);
}  // extern "C"

namespace netcdf {
namespace {

TEST(VarTest, InternalPrimitiveHelpersAllocationOverflowAndLookup) {
  free_NC_var(nullptr);

  // Overflow guards in new_x_NC_var().
  EXPECT_EQ(nullptr, new_x_NC_var(nullptr, std::numeric_limits<size_t>::max()));
  EXPECT_EQ(nullptr, new_x_NC_var(nullptr, std::numeric_limits<size_t>::max() /
                                                   sizeof(size_t) +
                                               1U));

  // Direct new_x_NC_var() for ndims == 0 and ndims > 0.
  NC_string* s0 = new_NC_string(6, "scalar");
  ASSERT_NE(s0, nullptr);
  NC_var* scalar_var = new_x_NC_var(s0, 0);
  ASSERT_NE(scalar_var, nullptr);
  EXPECT_EQ(nullptr, scalar_var->dimids);
  EXPECT_EQ(nullptr, scalar_var->shape);
  EXPECT_EQ(nullptr, scalar_var->dsizes);
  free_NC_var(scalar_var);

  NC_string* s1 = new_NC_string(5, "array");
  ASSERT_NE(s1, nullptr);
  std::unique_ptr<NC_var, decltype(&free_NC_var)> arr_var(new_x_NC_var(s1, 1),
                                                          &free_NC_var);
  ASSERT_NE(arr_var, nullptr);
  arr_var->type = NC_INT;
  arr_var->dimids[0] = 1;
  // NULL dims triggers false branch of (dims != NULL ? dims->nelems : 1).
  EXPECT_EQ(NC_EBADDIM, NC_var_shape(arr_var.get(), nullptr));

  // Empty NC_vararray helpers.
  NC_vararray empty_vars{};
  free_NC_vararrayV0(&empty_vars);
  free_NC_vararrayV(&empty_vars);
  EXPECT_EQ(-1, NC_findvar(&empty_vars, "missing", nullptr));

  NC_vararray dup_empty{};
  ASSERT_EQ(NC_NOERR, dup_NC_vararrayV(&dup_empty, &empty_vars));
  EXPECT_EQ(size_t{0}, dup_empty.nelems);
  free_NC_vararrayV(&dup_empty);
}

TEST(VarTest, ExternalTypeSizesAndVlenChecks) {
  struct TypeSizeCase {
    nc_type type;
    size_t expected_xsz;
  };
  constexpr TypeSizeCase kCases[] = {
      {NC_BYTE, 1}, {NC_CHAR, 1},  {NC_UBYTE, 1},  {NC_SHORT, 2},
      {NC_INT, 4},  {NC_FLOAT, 4}, {NC_DOUBLE, 8}, {NC_USHORT, 2},
      {NC_UINT, 4}, {NC_INT64, 8}, {NC_UINT64, 8},
  };
  for (const TypeSizeCase& tc : kCases) {
    EXPECT_EQ(tc.expected_xsz, ncx_szof(tc.type)) << "nc_type=" << tc.type;
  }

  // Exercise all NC_check_vlen() guard and loop paths directly.
  NC_var v{};
  v.xsz = 4;
  v.ndims = 1;
  v.shape = nullptr;
  EXPECT_EQ(0, NC_check_vlen(&v, 1024));

  size_t fixed_shape[2] = {10, 20};
  v.ndims = 2;
  v.shape = fixed_shape;
  v.xsz = 0;
  EXPECT_EQ(0, NC_check_vlen(&v, 1024));

  v.xsz = 4;
  EXPECT_EQ(1, NC_check_vlen(&v, 800));
  EXPECT_EQ(0, NC_check_vlen(&v, 799));

  // Record variable starts loop at index 1, ignoring NC_UNLIMITED at index 0.
  size_t rec_shape[2] = {NC_UNLIMITED, 25};
  v.shape = rec_shape;
  v.xsz = 4;
  EXPECT_EQ(1, NC_check_vlen(&v, 100));
  EXPECT_EQ(0, NC_check_vlen(&v, 99));

  // Classic endef() catches non-last oversized fixed variable with NC_EVARSIZE.
  if constexpr (sizeof(size_t) > 4) {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create("var_vlen_err.nc", NC_DISKLESS | NC_CLOBBER, nc.ptr()));
    int big_dim = -1;
    constexpr size_t kBigDimLen =
        (static_cast<size_t>(std::numeric_limits<int32_t>::max()) / 4U) + 2U;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "big", kBigDimLen, &big_dim));
    int v0 = -1;
    int v1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "huge_first", NC_INT, 1, &big_dim, &v0));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "tail_scalar", NC_INT, 0, nullptr, &v1));
    EXPECT_EQ(NC_EVARSIZE, nc_enddef(nc));
    EXPECT_EQ(NC_NOERR, nc.abort());
  }
}

TEST(VarTest, DirectNc3DispatchValidationLookupAndNullPointers) {
  int varid = -1;
  char name_buf[NC_MAX_NAME + 1]{};
  nc_type xtype = NC_NAT;
  int ndims = -1;
  int dimids[NC_MAX_VAR_DIMS]{};
  int natts = -1;
  int no_fill = -1;
  int fill_int = 0;

  EXPECT_EQ(NC_EBADID, NC3_def_var(-1, "v", NC_INT, 0, nullptr, &varid));
  EXPECT_EQ(NC_EBADID, NC3_inq_varid(-1, "v", &varid));
  EXPECT_EQ(NC_EBADID, NC3_inq_var(-1, 0, name_buf, &xtype, &ndims, dimids,
                                   &natts, &no_fill, &fill_int));
  EXPECT_EQ(NC_EBADID, NC3_rename_var(-1, 0, "v2"));
  EXPECT_EQ(NC_EBADID, NC3_def_var_fill(-1, 0, NC_FILL, &fill_int));

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("var_dispatch.nc", NC_DISKLESS | NC_CLOBBER, nc.ptr()));

  NC* base_nc = nullptr;
  ASSERT_EQ(NC_NOERR, NC_check_id(nc, &base_nc));
  ASSERT_NE(base_nc, nullptr);
  NC3_INFO* nc3 = NC3_DATA(base_nc);
  ASSERT_NE(nc3, nullptr);

  // NC_lookupvar() error branches on empty varray.
  NC_var* found_var = nullptr;
  EXPECT_EQ(NC_EGLOBAL, NC_lookupvar(nc3, NC_GLOBAL, &found_var));
  EXPECT_EQ(NC_ENOTVAR, NC_lookupvar(nc3, 0, nullptr));
  EXPECT_EQ(NC_ENOTVAR, NC_lookupvar(nc3, 0, &found_var));
  EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(nc, "absent", &varid));
  EXPECT_EQ(NC_ENOTVAR,
            nc_inq_var(nc, 0, name_buf, &xtype, &ndims, dimids, &natts));

  int unlim_dim = -1;
  int fixed_dim = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "time", NC_UNLIMITED, &unlim_dim));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 3, &fixed_dim));

  // Validation errors in NC3_def_var().
  EXPECT_EQ(NC_EBADNAME, nc_def_var(nc, "", NC_INT, 0, nullptr, &varid));
  EXPECT_EQ(NC_EBADNAME, nc_def_var(nc, "a/b", NC_INT, 0, nullptr, &varid));
  const std::string over_max_name(NC_MAX_NAME + 1, 'v');
  EXPECT_EQ(NC_EMAXNAME,
            nc_def_var(nc, over_max_name.c_str(), NC_INT, 0, nullptr, &varid));
  EXPECT_EQ(NC_EBADTYPE, nc_def_var(nc, "t0", NC_NAT, 0, nullptr, &varid));
  EXPECT_EQ(NC_EBADTYPE, nc_def_var(nc, "t1", NC_STRING, 0, nullptr, &varid));
  EXPECT_EQ(NC_EBADTYPE, nc_def_var(nc, "t2", NC_UINT64, 0, nullptr, &varid));
  EXPECT_EQ(NC_EMAXDIMS, nc_def_var(nc, "many_dims", NC_INT,
                                    NC_MAX_VAR_DIMS + 1, nullptr, &varid));
  EXPECT_EQ(NC_EINVAL, nc_def_var(nc, "neg_dims", NC_INT, -1, nullptr, &varid));

  int bad_neg_dim = -1;
  int bad_oob_dim = 5;
  int bad_unlim_pos[2] = {fixed_dim, unlim_dim};
  EXPECT_EQ(NC_EBADDIM,
            nc_def_var(nc, "neg_dim", NC_INT, 1, &bad_neg_dim, &varid));
  EXPECT_EQ(NC_EBADDIM,
            nc_def_var(nc, "oob_dim", NC_INT, 1, &bad_oob_dim, &varid));
  EXPECT_EQ(NC_EUNLIMPOS,
            nc_def_var(nc, "bad_pos", NC_INT, 2, bad_unlim_pos, &varid));

  // Define with varidp == NULL and valid fixed + record dims.
  ASSERT_EQ(NC_NOERR,
            NC3_def_var(nc, "out_ignored", NC_CHAR, 1, &fixed_dim, nullptr));
  // Direct NC_findvar() with varpp == NULL and invalid UTF-8 name.
  EXPECT_EQ(0, NC_findvar(&nc3->vars, "out_ignored", nullptr));
  EXPECT_EQ(-1, NC_findvar(&nc3->vars, "\xFF\xFE", &found_var));
  EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(nc, "\xFF\xFE", &varid));

  // Define record var + fill check with NULL output pointers.
  int rec_dims[2] = {unlim_dim, fixed_dim};
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "rec_var", NC_INT, 2, rec_dims, &varid));
  EXPECT_EQ(1, varid);

  // Check NC3_inq_var with all NULL pointers and non-NULL pointers.
  ASSERT_EQ(NC_NOERR, NC3_inq_var(nc, varid, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr));
  ASSERT_EQ(NC_NOERR, NC3_inq_var(nc, varid, name_buf, &xtype, &ndims, dimids,
                                  &natts, &no_fill, &fill_int));
  EXPECT_STREQ("rec_var", name_buf);
  EXPECT_EQ(NC_INT, xtype);
  EXPECT_EQ(2, ndims);
  EXPECT_EQ(unlim_dim, dimids[0]);
  EXPECT_EQ(fixed_dim, dimids[1]);
  EXPECT_EQ(0, natts);
  EXPECT_EQ(0, no_fill);
  EXPECT_EQ(NC_FILL_INT, fill_int);

  // Switch file fill mode to NC_NOFILL and verify newly defined var picks it
  // up.
  int old_fill = -1;
  ASSERT_EQ(NC_NOERR, nc_set_fill(nc, NC_NOFILL, &old_fill));
  EXPECT_EQ(NC_FILL, old_fill);
  int nofill_varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "after_nofill", NC_FLOAT, 0, nullptr,
                                 &nofill_varid));
  ASSERT_EQ(NC_NOERR, nc_inq_var_fill(nc, nofill_varid, &no_fill, nullptr));
  EXPECT_EQ(1, no_fill);

  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  EXPECT_EQ(NC_ENOTINDEFINE,
            nc_def_var(nc, "late_var", NC_INT, 0, nullptr, &varid));
  EXPECT_EQ(NC_NOERR, nc.close());
}

TEST(VarTest, ArrayGrowthRedefDupUtf8MaxNameAndAllNumericTypes) {
  // CDF-5 enables all 11 classic+CDF-5 types plus >INT64_MAX dimension check.
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create("var_cdf5_types.nc",
                                NC_DISKLESS | NC_CLOBBER | NC_CDF5, nc.ptr()));

  // Empty redef hits ref->nelems == 0 path in dup_NC_vararrayV().
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  ASSERT_EQ(NC_NOERR, nc_redef(nc));

  int d_three = -1;
  int d_four = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "three", 3, &d_three));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "four", 4, &d_four));

  // Verify NC_ERANGE in NC_var_shape() when product turns negative on large
  // CDF-5 dimension sizes (> OFF_T_MAX) in an isolated dataset.
  if constexpr (sizeof(size_t) > 4) {
    ScopedNcFile erange_nc;
    ASSERT_EQ(NC_NOERR,
              nc_create("var_cdf5_erange.nc",
                        NC_DISKLESS | NC_CLOBBER | NC_CDF5, erange_nc.ptr()));
    int erange_d0 = -1;
    int huge_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(erange_nc, "small", 3, &erange_d0));
    constexpr size_t kOverOffMax = static_cast<size_t>(
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 5ULL);
    ASSERT_EQ(NC_NOERR,
              nc_def_dim(erange_nc, "over_off_max", kOverOffMax, &huge_dim));
    int erange_dims[2] = {erange_d0, huge_dim};
    int erange_id = -1;
    EXPECT_EQ(NC_ERANGE, nc_def_var(erange_nc, "erange_var", NC_BYTE, 2,
                                    erange_dims, &erange_id));
    EXPECT_EQ(NC_NOERR, erange_nc.abort());
  }

  // Define variables covering all CDF-5 types and > NC_ARRAY_GROWBY (4) slots,
  // including scalars, 1D/2D arrays (with len%4 > 0 and len%4 == 0),
  // attributes, UTF-8 NFD names, and an NC_MAX_NAME name.
  constexpr std::array<nc_type, 11> kAllTypes = {
      NC_BYTE, NC_CHAR,  NC_UBYTE,  NC_SHORT, NC_USHORT, NC_INT,
      NC_UINT, NC_FLOAT, NC_DOUBLE, NC_INT64, NC_UINT64};
  for (size_t i = 0; i < kAllTypes.size(); ++i) {
    int vid = -1;
    const std::string vname = "typed_var_" + std::to_string(i);
    if (i % 3 == 0) {
      ASSERT_EQ(NC_NOERR,
                nc_def_var(nc, vname.c_str(), kAllTypes[i], 0, nullptr, &vid));
    } else if (i % 3 == 1) {
      ASSERT_EQ(NC_NOERR,
                nc_def_var(nc, vname.c_str(), kAllTypes[i], 1, &d_three, &vid));
    } else {
      int pair[2] = {d_three, d_four};
      ASSERT_EQ(NC_NOERR,
                nc_def_var(nc, vname.c_str(), kAllTypes[i], 2, pair, &vid));
    }
    const int tag = static_cast<int>(i);
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc, vid, "tag", NC_INT, 1, &tag));
  }

  // NFD vs NFC duplicate rejection and lookup.
  const char kNfdVar[] = "tempe\xCC\x81";
  const char kNfcVar[] = "temp\xC3\xA9";
  int utf8_varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, kNfdVar, NC_DOUBLE, 1, &d_four, &utf8_varid));
  int dup_id = -1;
  EXPECT_EQ(NC_ENAMEINUSE,
            nc_def_var(nc, kNfcVar, NC_DOUBLE, 0, nullptr, &dup_id));
  int queried = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(nc, kNfcVar, &queried));
  EXPECT_EQ(utf8_varid, queried);

  // NC_MAX_NAME length variable exercises copy_len == NC_MAX_NAME.
  const std::string max_var_name(NC_MAX_NAME, 'z');
  int max_vid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, max_var_name.c_str(), NC_SHORT, 1,
                                 &d_three, &max_vid));
  char name_out[NC_MAX_NAME + 1]{};
  ASSERT_EQ(NC_NOERR, nc_inq_varname(nc, max_vid, name_out));
  EXPECT_EQ(max_var_name, name_out);

  // Trigger populated dup_NC_vararrayV() via nc_redef() on non-empty vars.
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  ASSERT_EQ(NC_NOERR, nc_redef(nc));
  int extra_vid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "extra_redef", NC_INT, 0, nullptr, &extra_vid));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  // Verify scalar & array variables duplicated intact.
  nc_type qtype = NC_NAT;
  int qndims = -1;
  int qnatts = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var(nc, 0, nullptr, &qtype, &qndims, nullptr, &qnatts));
  EXPECT_EQ(NC_BYTE, qtype);
  EXPECT_EQ(0, qndims);
  EXPECT_EQ(1, qnatts);
  EXPECT_EQ(NC_NOERR, nc.close());
}

TEST(VarTest, RenameVarDefineDataReadOnlyHsyncAndDefVarFill) {
  TempFile disk_file("var_rename_fill.nc");

  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(disk_file.c_str(), NC_CLOBBER, nc.ptr()));
    int d = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 4, &d));

    int v0 = -1;
    int v1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "a", NC_INT, 1, &d, &v0));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "second_var_name", NC_INT, 0, nullptr, &v1));

    // def_var_fill(): NC_NOFILL, NC_FILL with NULL fill_value, explicit
    // fill_value created then overwritten via NC3_del_att() + NC3_put_att().
    EXPECT_EQ(NC_EGLOBAL, nc_def_var_fill(nc, NC_GLOBAL, NC_FILL, &v0));
    EXPECT_EQ(NC_ENOTVAR, NC3_def_var_fill(nc, NC_GLOBAL, NC_FILL, &v0));
    EXPECT_EQ(NC_ENOTVAR, nc_def_var_fill(nc, -2, NC_FILL, &v0));
    EXPECT_EQ(NC_ENOTVAR, nc_def_var_fill(nc, 99, NC_FILL, &v0));

    int custom_fill = -777;
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(nc, v0, NC_NOFILL, &custom_fill));
    int no_fill_out = -1;
    int fill_out = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_var_fill(nc, v0, &no_fill_out, &fill_out));
    EXPECT_EQ(1, no_fill_out);
    EXPECT_EQ(NC_FILL_INT, fill_out);

    ASSERT_EQ(NC_NOERR, nc_def_var_fill(nc, v0, NC_FILL, nullptr));
    ASSERT_EQ(NC_NOERR, nc_inq_var_fill(nc, v0, &no_fill_out, &fill_out));
    EXPECT_EQ(0, no_fill_out);
    EXPECT_EQ(NC_FILL_INT, fill_out);

    ASSERT_EQ(NC_NOERR, nc_def_var_fill(nc, v0, NC_FILL, &custom_fill));
    ASSERT_EQ(NC_NOERR, nc_inq_var_fill(nc, v0, &no_fill_out, &fill_out));
    EXPECT_EQ(0, no_fill_out);
    EXPECT_EQ(-777, fill_out);

    custom_fill = -888;
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(nc, v0, NC_FILL, &custom_fill));
    ASSERT_EQ(NC_NOERR, nc_inq_var_fill(nc, v0, &no_fill_out, &fill_out));
    EXPECT_EQ(-888, fill_out);

    fill_out = 0;
    no_fill_out = -1;
    ASSERT_EQ(NC_NOERR, NC3_inq_var(nc, v0, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, &no_fill_out, &fill_out));
    EXPECT_EQ(0, no_fill_out);
    EXPECT_EQ(-888, fill_out);

    // Rename validation & define-mode longer rename (including UTF-8 NFD).
    EXPECT_EQ(NC_EBADNAME, nc_rename_var(nc, v0, ""));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_var(nc, v0, "second_var_name"));
    EXPECT_EQ(NC_EGLOBAL, nc_rename_var(nc, NC_GLOBAL, "valid_rename"));
    EXPECT_EQ(NC_ENOTVAR, nc_rename_var(nc, 42, "valid_rename"));

    ASSERT_EQ(NC_NOERR, nc_rename_var(nc, v0, "renamed_cafe\xCC\x81"));
    int found = -1;
    EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(nc, "a", &found));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(nc, "renamed_caf\xC3\xA9", &found));
    EXPECT_EQ(v0, found);

    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // Data mode: def_var_fill and longer rename rejected with NC_ENOTINDEFINE.
    EXPECT_EQ(NC_ENOTINDEFINE, nc_def_var_fill(nc, v0, NC_FILL, &custom_fill));
    EXPECT_EQ(NC_ENOTINDEFINE,
              nc_rename_var(nc, v1, "much_longer_than_second_var_name"));

    // Data mode: shorter rename succeeds and updates hashmap.
    ASSERT_EQ(NC_NOERR, nc_rename_var(nc, v1, "v1_short"));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(nc, "second_var_name", &found));
    ASSERT_EQ(NC_NOERR, nc_inq_varid(nc, "v1_short", &found));
    EXPECT_EQ(v1, found);

    // Enable NC_HSYNC on NC3_INFO to exercise header sync path in data mode.
    NC* base_nc = nullptr;
    ASSERT_EQ(NC_NOERR, NC_check_id(nc, &base_nc));
    ASSERT_NE(base_nc, nullptr);
    NC3_INFO* nc3 = NC3_DATA(base_nc);
    ASSERT_NE(nc3, nullptr);
    nc3->state |= NC_HSYNC;

    ASSERT_EQ(NC_NOERR, nc_rename_var(nc, v1, "v1"));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen read-only: verify persistence and NC_EPERM checks.
  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open(disk_file.c_str(), NC_NOWRITE, ro.ptr()));
  int v1_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "v1", &v1_id));
  EXPECT_EQ(1, v1_id);

  int dummy_fill = 0;
  EXPECT_EQ(NC_EPERM, nc_rename_var(ro, v1_id, "x"));
  EXPECT_EQ(NC_EPERM, nc_def_var_fill(ro, v1_id, NC_FILL, &dummy_fill));
  EXPECT_EQ(NC_NOERR, ro.close());
}

}  // namespace
}  // namespace netcdf
