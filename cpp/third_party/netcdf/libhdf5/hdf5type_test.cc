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

// Unit tests for //third_party/netcdf/libhdf5/hdf5type.c.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

namespace netcdf {
namespace {

TEST(Hdf5TypeTest, InqTypeEqualHandlesAtomicMixedUncommittedAndErrors) {
  ScopedNcFile nc1;
  ASSERT_EQ(NC_NOERR,
            nc_create("type_equal_1.nc", NC_NETCDF4 | NC_DISKLESS, nc1.ptr()));
  ScopedNcFile nc2;
  ASSERT_EQ(NC_NOERR,
            nc_create("type_equal_2.nc", NC_NETCDF4 | NC_DISKLESS, nc2.ptr()));

  // Null equalp short-circuits before checking type IDs.
  EXPECT_EQ(NC_NOERR, nc_inq_type_equal(nc1, NC_NAT, nc2, -1, nullptr));

  // Invalid/NAT atomic IDs fail with NC_EINVAL.
  int equal = -1;
  EXPECT_EQ(NC_EINVAL, nc_inq_type_equal(nc1, NC_NAT, nc2, NC_INT, &equal));
  EXPECT_EQ(NC_EINVAL, nc_inq_type_equal(nc1, -1, nc2, NC_INT, &equal));
  EXPECT_EQ(NC_EINVAL, nc_inq_type_equal(nc1, NC_INT, nc2, NC_NAT, &equal));
  EXPECT_EQ(NC_EINVAL, nc_inq_type_equal(nc1, NC_INT, nc2, -2, &equal));

  // Atomic vs atomic comparisons.
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, NC_INT, nc2, NC_INT, &equal));
  EXPECT_EQ(equal, 1);
  ASSERT_EQ(NC_NOERR,
            nc_inq_type_equal(nc1, NC_STRING, nc2, NC_STRING, &equal));
  EXPECT_EQ(equal, 1);
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, NC_INT, nc2, NC_DOUBLE, &equal));
  EXPECT_EQ(equal, 0);

  // Define matching and distinct user-defined types in both files.
  nc_type opq1 = NC_NAT;
  nc_type opq2 = NC_NAT;
  nc_type opq_other = NC_NAT;
  nc_type enum1 = NC_NAT;
  nc_type enum2 = NC_NAT;
  nc_type vlen1 = NC_NAT;
  nc_type vlen2 = NC_NAT;
  nc_type cmp1 = NC_NAT;
  nc_type cmp2 = NC_NAT;

  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc1, 4, "blob4", &opq1));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc2, 4, "blob4", &opq2));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc2, 8, "blob8", &opq_other));

  ASSERT_EQ(NC_NOERR, nc_def_enum(nc1, NC_INT, "color", &enum1));
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc2, NC_INT, "color", &enum2));
  const int kRed = 1;
  const int kBlue = 2;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc1, enum1, "red", &kRed));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc1, enum1, "blue", &kBlue));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc2, enum2, "red", &kRed));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc2, enum2, "blue", &kBlue));

  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc1, "ints_vlen", NC_INT, &vlen1));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc2, "ints_vlen", NC_INT, &vlen2));

  struct Pair {
    int a;
    double b;
  };
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc1, sizeof(Pair), "pair_t", &cmp1));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc1, cmp1, "a",
                                         NC_COMPOUND_OFFSET(Pair, a), NC_INT));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc1, cmp1, "b", NC_COMPOUND_OFFSET(Pair, b),
                               NC_DOUBLE));

  ASSERT_EQ(NC_NOERR, nc_def_compound(nc2, sizeof(Pair), "pair_t", &cmp2));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc2, cmp2, "a",
                                         NC_COMPOUND_OFFSET(Pair, a), NC_INT));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc2, cmp2, "b", NC_COMPOUND_OFFSET(Pair, b),
                               NC_DOUBLE));

  // Atomic mixed with user-defined type in either argument order.
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, NC_INT, nc1, opq1, &equal));
  EXPECT_EQ(equal, 0);
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, opq1, nc1, NC_STRING, &equal));
  EXPECT_EQ(equal, 0);

  // Unknown group/file parts or unknown type IDs.
  EXPECT_EQ(NC_EBADID, nc_inq_type_equal(nc1 | 0x7f, opq1, nc2, opq2, &equal));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_type_equal(nc1, 9999, nc2, opq2, &equal));
  EXPECT_EQ(NC_EBADID, nc_inq_type_equal(nc1, opq1, nc2 | 0x7f, opq2, &equal));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_type_equal(nc1, opq1, nc2, 9999, &equal));

  // Uncommitted HDF5 types have native_hdf_typeid == 0 -> H5Tequal fails.
  EXPECT_EQ(NC_EHDFERR, nc_inq_type_equal(nc1, opq1, nc2, opq2, &equal));

  // Commit nc1 only: comparing committed nc1 with uncommitted nc2 still fails.
  ASSERT_EQ(NC_NOERR, nc_enddef(nc1));
  EXPECT_EQ(NC_EHDFERR, nc_inq_type_equal(nc1, opq1, nc2, opq2, &equal));

  // Commit nc2 as well: cross-file structural comparisons succeed.
  ASSERT_EQ(NC_NOERR, nc_enddef(nc2));
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, opq1, nc1, opq1, &equal));
  EXPECT_EQ(equal, 1);
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, opq1, nc2, opq2, &equal));
  EXPECT_EQ(equal, 1);
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, opq1, nc2, opq_other, &equal));
  EXPECT_EQ(equal, 0);
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, enum1, nc2, enum2, &equal));
  EXPECT_EQ(equal, 1);
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, vlen1, nc2, vlen2, &equal));
  EXPECT_EQ(equal, 1);
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, cmp1, nc2, cmp2, &equal));
  EXPECT_EQ(equal, 1);
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1, cmp1, nc2, opq2, &equal));
  EXPECT_EQ(equal, 0);
}

TEST(Hdf5TypeTest, AddUserTypeValidatesArgumentsModeAndDuplicates) {
  // Classic-model netCDF-4 rejects all user-defined types.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("classic_types.nc",
                                  NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS,
                                  classic.ptr()));
    nc_type tid = NC_NAT;
    EXPECT_EQ(NC_ESTRICTNC3,
              nc_def_compound(classic, sizeof(int), "cmp", &tid));
    EXPECT_EQ(NC_ESTRICTNC3, nc_def_opaque(classic, 4, "opq", &tid));
    EXPECT_EQ(NC_ESTRICTNC3, nc_def_vlen(classic, "vl", NC_INT, &tid));
    EXPECT_EQ(NC_ESTRICTNC3, nc_def_enum(classic, NC_INT, "en", &tid));
  }

  ScopedMemio saved_mem;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("add_user_type.nc", NC_NETCDF4, 4096, nc.ptr()));
    nc_type tid = NC_NAT;

    // Name checks via nc4_check_name.
    const std::string too_long_name(NC_MAX_NAME + 1, 't');
    EXPECT_EQ(NC_EINVAL, nc_def_opaque(nc, 4, nullptr, &tid));
    EXPECT_EQ(NC_EBADNAME, nc_def_opaque(nc, 4, "", &tid));
    EXPECT_EQ(NC_EBADNAME, nc_def_opaque(nc, 4, "bad/name", &tid));
    EXPECT_EQ(NC_EMAXNAME, nc_def_opaque(nc, 4, too_long_name.c_str(), &tid));

    // Bad group mask and invalid sizes.
    EXPECT_EQ(NC_EBADID, nc_def_opaque(nc | 0x7f, 4, "bad_grp", &tid));
    EXPECT_EQ(NC_EINVAL, nc_def_compound(nc, 0, "zero_cmp", &tid));
    EXPECT_EQ(NC_EINVAL, nc_def_opaque(nc, 0, "zero_opq", &tid));

    // Invalid enum base type triggers nc4_get_typelen_mem error.
    EXPECT_EQ(NC_EINVAL, nc_def_enum(nc, -1, "neg_base_enum", &tid));
    EXPECT_EQ(NC_EBADTYPE, nc_def_enum(nc, NC_NAT, "nat_base_enum", &tid));
    EXPECT_EQ(NC_EBADTYPID, nc_def_enum(nc, 9999, "bad_base_enum", &tid));

    // Duplicate names across types, variables, and subgroups fail.
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "dup_name", &tid));
    EXPECT_EQ(NC_ENAMEINUSE, nc_def_opaque(nc, 4, "dup_name", &tid));

    int subgrp = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "existing_grp", &subgrp));
    EXPECT_EQ(NC_ENAMEINUSE, nc_def_vlen(nc, "existing_grp", NC_INT, &tid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "existing_var", NC_INT, 0, nullptr, &varid));
    EXPECT_EQ(NC_ENAMEINUSE, nc_def_enum(nc, NC_INT, "existing_var", &tid));

    // Auto-redef from data mode on a writable file succeeds, and nullptr
    // typeidp is tolerated.
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 8, "auto_redef_opq", nullptr));
    nc_type looked_up = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc, "auto_redef_opq", &looked_up));
    EXPECT_GT(looked_up, NC_STRING);

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), saved_mem.get()));
  }

  // Reopen read-only: calling nc_def_* triggers NC4_redef -> NC_EPERM.
  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("add_user_type_ro.nc", NC_NOWRITE, saved_mem.size(),
                        saved_mem.get()->memory, ro_nc.ptr()));
  nc_type ro_tid = NC_NAT;
  EXPECT_EQ(NC_EPERM, nc_def_opaque(ro_nc, 4, "cannot_write", &ro_tid));
}

TEST(Hdf5TypeTest, CompoundInsertValidationNestedTypesAndRoundTrip) {
  struct InnerFixed {
    int32_t id;
    std::array<double, 2> vec;
  };
  struct InnerVar {
    int32_t tag;
    char* label;
  };
  struct OuterMixed {
    int32_t seq;
    std::array<std::array<float, 2>, 2> grid;
    std::array<unsigned char, 4> raw_token;
    int16_t mode;
    InnerFixed fixed_sub;
    InnerVar var_sub;
    nc_vlen_t vlen_ints;
    char* note;
  };

  // Use an on-disk NetCDF-4 file so HDF5 tracks link creation order
  // (H5P_CRT_ORDER_TRACKED) and reads nested field types like "token4"
  // before "outer_mixed" even when "token4" sorts later alphabetically.
  TempFile file("compound_roundtrip.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(file.c_str(), NC_NETCDF4 | NC_CLOBBER, nc.ptr()));

    nc_type tok_tid = NC_NAT;
    nc_type mode_tid = NC_NAT;
    nc_type vlen_tid = NC_NAT;
    nc_type inner_fixed_tid = NC_NAT;
    nc_type inner_var_tid = NC_NAT;
    nc_type outer_tid = NC_NAT;

    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "token4", &tok_tid));
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_SHORT, "mode_t", &mode_tid));
    const int16_t kIdle = 0;
    const int16_t kActive = 7;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, mode_tid, "IDLE", &kIdle));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, mode_tid, "ACTIVE", &kActive));
    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc, "int_list", NC_INT, &vlen_tid));

    ASSERT_EQ(NC_NOERR, nc_def_compound(nc, sizeof(InnerFixed), "inner_fixed",
                                        &inner_fixed_tid));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, inner_fixed_tid, "id",
                                 NC_COMPOUND_OFFSET(InnerFixed, id), NC_INT));
    constexpr std::array<int, 1> kVecDims = {2};
    ASSERT_EQ(NC_NOERR,
              nc_insert_array_compound(nc, inner_fixed_tid, "vec",
                                       NC_COMPOUND_OFFSET(InnerFixed, vec),
                                       NC_DOUBLE, 1, kVecDims.data()));

    ASSERT_EQ(NC_NOERR, nc_def_compound(nc, sizeof(InnerVar), "inner_var",
                                        &inner_var_tid));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, inner_var_tid, "tag",
                                 NC_COMPOUND_OFFSET(InnerVar, tag), NC_INT));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc, inner_var_tid, "label",
                                           NC_COMPOUND_OFFSET(InnerVar, label),
                                           NC_STRING));

    ASSERT_EQ(NC_NOERR, nc_def_compound(nc, sizeof(OuterMixed), "outer_mixed",
                                        &outer_tid));

    // Validate error checks on insert_compound / insert_array_compound.
    const std::string too_long(NC_MAX_NAME + 1, 'f');
    EXPECT_EQ(NC_EINVAL, nc_insert_compound(nc, outer_tid, nullptr, 0, NC_INT));
    EXPECT_EQ(NC_EBADNAME, nc_insert_compound(nc, outer_tid, "", 0, NC_INT));
    EXPECT_EQ(NC_EMAXNAME,
              nc_insert_compound(nc, outer_tid, too_long.c_str(), 0, NC_INT));
    EXPECT_EQ(NC_EBADID,
              nc_insert_compound(nc | 0x7f, outer_tid, "ok", 0, NC_INT));
    EXPECT_EQ(NC_EINVAL, nc_insert_compound(nc, -1, "ok", 0, NC_INT));
    EXPECT_EQ(NC_EBADTYPID, nc_insert_compound(nc, 9999, "ok", 0, NC_INT));
    EXPECT_EQ(NC_EBADTYPE, nc_insert_compound(nc, NC_INT, "ok", 0, NC_INT));
    EXPECT_EQ(NC_EBADTYPE, nc_insert_compound(nc, tok_tid, "ok", 0, NC_INT));
    EXPECT_EQ(NC_EBADTYPE, nc_insert_compound(nc, mode_tid, "ok", 0, NC_INT));
    EXPECT_EQ(NC_EBADTYPE, nc_insert_compound(nc, vlen_tid, "ok", 0, NC_INT));

    // Insert fixed, array, nested fixed, nested variable, vlen, and string
    // fields covering all branches of NC4_recheck_varsize.
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, outer_tid, "seq",
                                 NC_COMPOUND_OFFSET(OuterMixed, seq), NC_INT));
    constexpr std::array<int, 2> kGridDims = {2, 2};
    ASSERT_EQ(NC_NOERR,
              nc_insert_array_compound(nc, outer_tid, "grid",
                                       NC_COMPOUND_OFFSET(OuterMixed, grid),
                                       NC_FLOAT, 2, kGridDims.data()));
    ASSERT_EQ(
        NC_NOERR,
        nc_insert_compound(nc, outer_tid, "raw_token",
                           NC_COMPOUND_OFFSET(OuterMixed, raw_token), tok_tid));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc, outer_tid, "mode",
                                           NC_COMPOUND_OFFSET(OuterMixed, mode),
                                           mode_tid));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, outer_tid, "fixed_sub",
                                 NC_COMPOUND_OFFSET(OuterMixed, fixed_sub),
                                 inner_fixed_tid));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, outer_tid, "var_sub",
                                 NC_COMPOUND_OFFSET(OuterMixed, var_sub),
                                 inner_var_tid));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, outer_tid, "vlen_ints",
                                 NC_COMPOUND_OFFSET(OuterMixed, vlen_ints),
                                 vlen_tid));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc, outer_tid, "note",
                                           NC_COMPOUND_OFFSET(OuterMixed, note),
                                           NC_STRING));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 1, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "records", outer_tid, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // Inserting into an already committed compound fails with NC_ETYPDEFINED.
    EXPECT_EQ(NC_ETYPDEFINED,
              nc_insert_compound(nc, outer_tid, "late", 0, NC_INT));
    EXPECT_EQ(NC_ETYPDEFINED,
              nc_insert_array_compound(nc, outer_tid, "late_arr", 0, NC_INT, 1,
                                       kVecDims.data()));

    std::array<int32_t, 3> vlen_payload = {100, 200, 300};
    std::string label_buf = "nested";
    std::string note_buf = "outer";
    OuterMixed write_rec{};
    std::memset(&write_rec, 0, sizeof(write_rec));
    write_rec.seq = 42;
    write_rec.grid = {{{1.0F, 2.0F}, {3.0F, 4.0F}}};
    write_rec.raw_token = {0xAA, 0xBB, 0xCC, 0xDD};
    write_rec.mode = kActive;
    write_rec.fixed_sub.id = 9;
    write_rec.fixed_sub.vec = {1.25, 2.5};
    write_rec.var_sub.tag = 77;
    write_rec.var_sub.label = label_buf.data();
    write_rec.vlen_ints.len = vlen_payload.size();
    write_rec.vlen_ints.p = vlen_payload.data();
    write_rec.note = note_buf.data();

    ASSERT_EQ(NC_NOERR, nc_put_var(nc, varid, &write_rec));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro_nc.ptr()));
  nc_type ro_outer_tid = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(ro_nc, "outer_mixed", &ro_outer_tid));
  int ro_varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "records", &ro_varid));

  OuterMixed read_rec{};
  auto reclaim_guard = absl::MakeCleanup([&]() {
    EXPECT_EQ(NC_NOERR, nc_reclaim_data(ro_nc, ro_outer_tid, &read_rec, 1));
  });
  ASSERT_EQ(NC_NOERR, nc_get_var(ro_nc, ro_varid, &read_rec));
  EXPECT_EQ(read_rec.seq, 42);
  EXPECT_FLOAT_EQ(read_rec.grid[1][1], 4.0F);
  EXPECT_EQ(read_rec.raw_token,
            (std::array<unsigned char, 4>{0xAA, 0xBB, 0xCC, 0xDD}));
  EXPECT_EQ(read_rec.mode, 7);
  EXPECT_EQ(read_rec.fixed_sub.id, 9);
  EXPECT_DOUBLE_EQ(read_rec.fixed_sub.vec[1], 2.5);
  EXPECT_EQ(read_rec.var_sub.tag, 77);
  EXPECT_STREQ(read_rec.var_sub.label, "nested");
  ASSERT_EQ(read_rec.vlen_ints.len, 3U);
  const auto* read_ints = static_cast<const int32_t*>(read_rec.vlen_ints.p);
  EXPECT_EQ(read_ints[0], 100);
  EXPECT_EQ(read_ints[1], 200);
  EXPECT_EQ(read_ints[2], 300);
  EXPECT_STREQ(read_rec.note, "outer");
}

TEST(Hdf5TypeTest, CompoundInsertInvalidFieldTypeFailsRecheckVarsize) {
  TempFile file("invalid_field_type.nc");
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create(file.c_str(), NC_NETCDF4 | NC_CLOBBER, nc.ptr()));
  // End define mode first then redef so nc_abort() cleans internal state
  // without relying on filesystem remove() length limits.
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  ASSERT_EQ(NC_NOERR, nc_redef(nc));

  nc_type cmp_tid = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc, sizeof(int), "bad_cmp", &cmp_tid));
  EXPECT_EQ(NC_EBADTYPID,
            nc_insert_compound(nc, cmp_tid, "bad_field", 0, 9999));
  ASSERT_EQ(NC_NOERR, nc.abort());
}

TEST(Hdf5TypeTest, EnumInsertValidationBaseTypesAndRoundTrip) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("enum_types.nc", NC_NETCDF4, 4096, nc.ptr()));

    nc_type enum_i8 = NC_NAT;
    nc_type enum_u8 = NC_NAT;
    nc_type enum_i16 = NC_NAT;
    nc_type enum_u16 = NC_NAT;
    nc_type enum_i32 = NC_NAT;
    nc_type enum_u32 = NC_NAT;
    nc_type enum_i64 = NC_NAT;
    nc_type enum_u64 = NC_NAT;
    nc_type opq_tid = NC_NAT;
    nc_type cmp_tid = NC_NAT;

    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_BYTE, "e_i8", &enum_i8));
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_UBYTE, "e_u8", &enum_u8));
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_SHORT, "e_i16", &enum_i16));
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_USHORT, "e_u16", &enum_u16));
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_INT, "e_i32", &enum_i32));
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_UINT, "e_u32", &enum_u32));
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_INT64, "e_i64", &enum_i64));
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_UINT64, "e_u64", &enum_u64));
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "opq", &opq_tid));
    ASSERT_EQ(NC_NOERR, nc_def_compound(nc, sizeof(int), "cmp", &cmp_tid));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc, cmp_tid, "x", 0, NC_INT));

    const int32_t kVal32 = 1;
    const std::string too_long(NC_MAX_NAME + 1, 'e');
    EXPECT_EQ(NC_EINVAL, nc_insert_enum(nc, enum_i32, nullptr, &kVal32));
    EXPECT_EQ(NC_EBADNAME, nc_insert_enum(nc, enum_i32, "", &kVal32));
    EXPECT_EQ(NC_EMAXNAME,
              nc_insert_enum(nc, enum_i32, too_long.c_str(), &kVal32));
    EXPECT_EQ(NC_EBADID, nc_insert_enum(nc | 0x7f, enum_i32, "OK", &kVal32));
    EXPECT_EQ(NC_EINVAL, nc_insert_enum(nc, -1, "OK", &kVal32));
    EXPECT_EQ(NC_EBADTYPID, nc_insert_enum(nc, 9999, "OK", &kVal32));
    EXPECT_EQ(NC_EBADTYPE, nc_insert_enum(nc, NC_INT, "OK", &kVal32));
    EXPECT_EQ(NC_EBADTYPE, nc_insert_enum(nc, opq_tid, "OK", &kVal32));
    EXPECT_EQ(NC_EBADTYPE, nc_insert_enum(nc, cmp_tid, "OK", &kVal32));

    const signed char v_i8 = -5;
    const unsigned char v_u8 = 250;
    const short v_i16 = -300;
    const unsigned short v_u16 = 60000;
    const int v_i32_a = 10;
    const int v_i32_b = 20;
    const unsigned int v_u32 = 4000000000U;
    const long long v_i64 = -900000000000LL;
    const unsigned long long v_u64 = 1200000000000ULL;

    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_i8, "NEG5", &v_i8));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_u8, "U250", &v_u8));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_i16, "I16", &v_i16));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_u16, "U16", &v_u16));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_i32, "ALPHA", &v_i32_a));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_i32, "BETA", &v_i32_b));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_u32, "U32", &v_u32));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_i64, "I64", &v_i64));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_u64, "U64", &v_u64));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "ev", enum_i32, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // Committed enum rejects additional members.
    const int v_late = 30;
    EXPECT_EQ(NC_ETYPDEFINED, nc_insert_enum(nc, enum_i32, "GAMMA", &v_late));

    const std::array<int, 2> kWriteVals = {v_i32_a, v_i32_b};
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, varid, kWriteVals.data()));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("enum_types_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));
  nc_type ro_enum = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(ro_nc, "e_i32", &ro_enum));

  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "ev", &varid));
  std::array<int, 2> got{};
  ASSERT_EQ(NC_NOERR, nc_get_var(ro_nc, varid, got.data()));
  EXPECT_EQ(got, (std::array<int, 2>{10, 20}));

  std::array<char, NC_MAX_NAME + 1> ident{};
  ASSERT_EQ(NC_NOERR, nc_inq_enum_ident(ro_nc, ro_enum, 20, ident.data()));
  EXPECT_STREQ(ident.data(), "BETA");
}

TEST(Hdf5TypeTest, PutAndGetVlenElementAndUserBaseVlenRoundTrip) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("vlen_elem.nc", NC_NETCDF4, 4096, nc.ptr()));

    nc_type vl_int = NC_NAT;
    nc_type opq4 = NC_NAT;
    nc_type vl_opq = NC_NAT;
    nc_type vl_str = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc, "vl_int", NC_INT, &vl_int));
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "opq4", &opq4));
    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc, "vl_opq", opq4, &vl_opq));
    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc, "vl_str", NC_STRING, &vl_str));

    // Direct put/get vlen element calls (4-byte elements match HDF5/Fortran
    // helper).
    const std::array<int32_t, 4> kInput = {11, -22, 33, 44};
    nc_vlen_t elem{};
    ASSERT_EQ(NC_NOERR, nc_put_vlen_element(nc, vl_int, &elem, kInput.size(),
                                            kInput.data()));
    EXPECT_EQ(elem.len, kInput.size());
    EXPECT_EQ(elem.p, kInput.data());

    std::array<int32_t, 4> output{};
    size_t out_len = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_vlen_element(nc, vl_int, &elem, &out_len, output.data()));
    EXPECT_EQ(out_len, 4U);
    EXPECT_EQ(output, kInput);

    // Zero-length element with non-null storage pointer avoids memcpy(NULL).
    int32_t dummy = 0;
    nc_vlen_t empty_elem{};
    ASSERT_EQ(NC_NOERR,
              nc_put_vlen_element(nc, vl_int, &empty_elem, 0, &dummy));
    out_len = 99;
    ASSERT_EQ(NC_NOERR,
              nc_get_vlen_element(nc, vl_int, &empty_elem, &out_len, &dummy));
    EXPECT_EQ(out_len, 0U);

    int dimid = -1;
    int var_int = -1;
    int var_str = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rows", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_int", vl_int, 1, &dimid, &var_int));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v_str", vl_str, 1, &dimid, &var_str));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    std::array<int32_t, 2> row0 = {5, 6};
    std::array<int32_t, 1> row1 = {7};
    std::array<nc_vlen_t, 2> write_vlens{};
    write_vlens[0].len = row0.size();
    write_vlens[0].p = row0.data();
    write_vlens[1].len = row1.size();
    write_vlens[1].p = row1.data();
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, var_int, write_vlens.data()));

    std::array<std::string, 2> str_backing = {"one", "two"};
    std::array<char*, 2> row0_ptrs = {str_backing[0].data(),
                                      str_backing[1].data()};
    std::array<nc_vlen_t, 2> write_str_vlens{};
    write_str_vlens[0].len = 2;
    write_str_vlens[0].p = row0_ptrs.data();
    write_str_vlens[1].len = 0;
    write_str_vlens[1].p = nullptr;
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, var_str, write_str_vlens.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("vlen_elem_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));

  int ro_var_int = -1;
  int ro_var_str = -1;
  nc_type ro_vl_str = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_int", &ro_var_int));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "v_str", &ro_var_str));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(ro_nc, "vl_str", &ro_vl_str));

  ScopedNcVlens read_vlens(2);
  ASSERT_EQ(NC_NOERR, nc_get_var(ro_nc, ro_var_int, read_vlens.data()));
  ASSERT_EQ(read_vlens[0].len, 2U);
  EXPECT_EQ(static_cast<const int32_t*>(read_vlens[0].p)[0], 5);
  EXPECT_EQ(static_cast<const int32_t*>(read_vlens[0].p)[1], 6);
  ASSERT_EQ(read_vlens[1].len, 1U);
  EXPECT_EQ(static_cast<const int32_t*>(read_vlens[1].p)[0], 7);

  std::array<nc_vlen_t, 2> read_str_vlens{};
  auto reclaim_strs = absl::MakeCleanup([&]() {
    EXPECT_EQ(NC_NOERR, nc_reclaim_data(ro_nc, ro_vl_str, read_str_vlens.data(),
                                        read_str_vlens.size()));
  });
  ASSERT_EQ(NC_NOERR, nc_get_var(ro_nc, ro_var_str, read_str_vlens.data()));
  ASSERT_EQ(read_str_vlens[0].len, 2U);
  const auto* str_elems = static_cast<char* const*>(read_str_vlens[0].p);
  EXPECT_STREQ(str_elems[0], "one");
  EXPECT_STREQ(str_elems[1], "two");
  EXPECT_EQ(read_str_vlens[1].len, 0U);
}

}  // namespace
}  // namespace netcdf
