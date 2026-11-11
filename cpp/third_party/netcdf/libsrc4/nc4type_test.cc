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

// Unit tests for //third_party/netcdf/libsrc4/nc4type.c.
//
// Exercises user-defined type enumeration and inquiry, compound field metadata
// and normalized index lookup, enum member and identifier resolution across all
// integral base types, relative/FQN/recursive nc_inq_typeid(), fixed-size vs
// variable-sized propagation, and internal nc4type.c helpers via nc4type_shim.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/libsrc4/nc4type_shim.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAre;

// Constructs an invalid group sub-ID on an otherwise open NetCDF-4 file handle
// so dispatch passes NC_check_id() and exercises nc4_find_* error returns in
// nc4type.c.
int MakeInvalidGroupSubId(int root_ncid) {
  return (root_ncid & 0x7fff0000) | 0x7fff;
}

// Closes an in-memory dataset via nc_close_memio(), releasing ScopedNcFile
// only on success so failed assertions keep RAII cleanup active.
int CloseMemio(ScopedNcFile& nc, ScopedMemio& memio) {
  const int stat = nc_close_memio(nc.get(), memio.get());
  if (stat == NC_NOERR) {
    nc.release();
  }
  return stat;
}

struct InnerFixed {
  std::int32_t code;
  double weight;
};

struct MidFixed {
  std::int32_t tag;
  std::int16_t flag;
  std::uint8_t blob[12];
  InnerFixed inner;
};

struct OuterVarsized {
  std::uint8_t tag;
  std::int32_t matrix[2][3];
  InnerFixed inner;
  char* label;
  nc_vlen_t seq;
};

struct NestedVarsized {
  std::int32_t seq_no;
  OuterVarsized payload;
};

TEST(Nc4TypeTest, UserTypeCreationEnumerationAndInqRoundTrip) {
  // Use a temporary NetCDF-4 file so HDF5 persists creation-order tracking
  // (nc_create_mem uses H5P_DEFAULT without H5P_CRT_ORDER_TRACKED).
  TempFile temp_file("nc4type_roundtrip.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(temp_file.c_str(), NC_NETCDF4, nc.ptr()));

    nc_type blob_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 12, "blob_t", &blob_t));

    nc_type flag_enum_t = NC_NAT;
    ASSERT_EQ(NC_NOERR,
              nc_def_enum(nc.get(), NC_SHORT, "flag_enum_t", &flag_enum_t));
    std::int16_t v_off = 0;
    std::int16_t v_low = -32000;
    std::int16_t v_high = 12345;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), flag_enum_t, "OFF", &v_off));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), flag_enum_t, "LOW", &v_low));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), flag_enum_t, "HIGH", &v_high));

    nc_type int_seq_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "int_seq_t", NC_INT, &int_seq_t));

    nc_type inner_fixed_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(InnerFixed),
                                        "inner_fixed_t", &inner_fixed_t));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), inner_fixed_t, "code",
                                           offsetof(InnerFixed, code), NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), inner_fixed_t, "weight",
                                 offsetof(InnerFixed, weight), NC_DOUBLE));

    // Fixed compound containing enum, opaque, and nested fixed compound.
    nc_type mid_fixed_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(MidFixed),
                                        "mid_fixed_t", &mid_fixed_t));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), mid_fixed_t, "tag",
                                           offsetof(MidFixed, tag), NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), mid_fixed_t, "flag",
                                 offsetof(MidFixed, flag), flag_enum_t));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), mid_fixed_t, "blob",
                                           offsetof(MidFixed, blob), blob_t));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), mid_fixed_t, "inner",
                                 offsetof(MidFixed, inner), inner_fixed_t));

    // Variable-sized compound due to NC_STRING and NC_VLEN fields.
    nc_type outer_var_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(OuterVarsized),
                                        "outer_var_t", &outer_var_t));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), outer_var_t, "tag",
                                 offsetof(OuterVarsized, tag), NC_UBYTE));
    const int matrix_dims[2] = {2, 3};
    ASSERT_EQ(NC_NOERR,
              nc_insert_array_compound(nc.get(), outer_var_t, "matrix",
                                       offsetof(OuterVarsized, matrix), NC_INT,
                                       2, matrix_dims));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), outer_var_t, "inner",
                                           offsetof(OuterVarsized, inner),
                                           inner_fixed_t));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), outer_var_t, "label",
                                 offsetof(OuterVarsized, label), NC_STRING));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), outer_var_t, "seq",
                                 offsetof(OuterVarsized, seq), int_seq_t));

    // Compound nesting another variable-sized compound propagates varsized.
    nc_type nested_var_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(NestedVarsized),
                                        "nested_var_t", &nested_var_t));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), nested_var_t, "seq_no",
                                 offsetof(NestedVarsized, seq_no), NC_INT));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), nested_var_t, "payload",
                                           offsetof(NestedVarsized, payload),
                                           outer_var_t));

    // Check fixed-sizedness prior to commit.
    int is_fixed = -1;
    EXPECT_EQ(NC_NOERR, nc4type_test_inq_type_fixed_size(nc.get(), mid_fixed_t,
                                                         &is_fixed));
    EXPECT_EQ(1, is_fixed);
    EXPECT_EQ(NC_NOERR, nc4type_test_inq_type_fixed_size(nc.get(), outer_var_t,
                                                         &is_fixed));
    EXPECT_EQ(0, is_fixed);
    EXPECT_EQ(NC_NOERR, nc4type_test_inq_type_fixed_size(nc.get(), nested_var_t,
                                                         &is_fixed));
    EXPECT_EQ(0, is_fixed);

    int empty_sub = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "empty_sub", &empty_sub));

    // Persist variables so all types commit cleanly and reload on open.
    int dim_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "n", 1, &dim_id));

    int outer_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "outer_data", outer_var_t, 1,
                                   &dim_id, &outer_varid));
    int nested_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "nested_data", nested_var_t, 1,
                                   &dim_id, &nested_varid));
    int str_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "str_data", NC_STRING, 1, &dim_id,
                                   &str_varid));
    int mid_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "mid_data", mid_fixed_t, 1,
                                   &dim_id, &mid_varid));

    char label_buf[] = "alpha_record";
    std::array<std::int32_t, 3> seq_elems = {11, 22, 33};
    OuterVarsized write_outer{};
    std::memset(&write_outer, 0, sizeof(write_outer));
    write_outer.tag = 7;
    write_outer.matrix[0][0] = 1;
    write_outer.matrix[0][1] = 2;
    write_outer.matrix[0][2] = 3;
    write_outer.matrix[1][0] = 4;
    write_outer.matrix[1][1] = 5;
    write_outer.matrix[1][2] = 6;
    write_outer.inner.code = 42;
    write_outer.inner.weight = 3.5;
    write_outer.label = label_buf;
    write_outer.seq.len = seq_elems.size();
    write_outer.seq.p = seq_elems.data();

    NestedVarsized write_nested{};
    std::memset(&write_nested, 0, sizeof(write_nested));
    write_nested.seq_no = 99;
    std::memcpy(&write_nested.payload, &write_outer, sizeof(write_outer));

    MidFixed write_mid{};
    std::memset(&write_mid, 0, sizeof(write_mid));
    write_mid.tag = 5;
    write_mid.flag = v_high;
    write_mid.blob[0] = 0xAB;
    write_mid.blob[11] = 0xCD;
    write_mid.inner.code = 17;
    write_mid.inner.weight = 2.25;

    const char* str_ptr = label_buf;
    const size_t idx0 = 0;
    const size_t cnt1 = 1;
    ASSERT_EQ(NC_NOERR,
              nc_put_vara(nc.get(), outer_varid, &idx0, &cnt1, &write_outer));
    ASSERT_EQ(NC_NOERR,
              nc_put_vara(nc.get(), nested_varid, &idx0, &cnt1, &write_nested));
    ASSERT_EQ(NC_NOERR,
              nc_put_vara(nc.get(), mid_varid, &idx0, &cnt1, &write_mid));
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_string(nc.get(), str_varid, &idx0, &cnt1, &str_ptr));

    ASSERT_EQ(NC_NOERR, nc.close());
  }

  ScopedNcFile reader;
  ASSERT_EQ(NC_NOERR, nc_open(temp_file.c_str(), NC_NOWRITE, reader.ptr()));
  const int root_id = reader.get();

  int empty_sub = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_ncid(root_id, "empty_sub", &empty_sub));
  int empty_ntypes = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_typeids(empty_sub, &empty_ntypes, nullptr));
  EXPECT_EQ(0, empty_ntypes);
  EXPECT_EQ(NC_NOERR, nc_inq_typeids(empty_sub, nullptr, nullptr));

  int root_ntypes = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_typeids(root_id, &root_ntypes, nullptr));
  ASSERT_EQ(7, root_ntypes);

  std::vector<nc_type> typeids(root_ntypes, NC_NAT);
  EXPECT_EQ(NC_NOERR, nc_inq_typeids(root_id, nullptr, typeids.data()));
  int root_ntypes2 = -1;
  std::vector<nc_type> typeids2(root_ntypes, NC_NAT);
  ASSERT_EQ(NC_NOERR, nc_inq_typeids(root_id, &root_ntypes2, typeids2.data()));
  EXPECT_EQ(root_ntypes, root_ntypes2);
  EXPECT_EQ(typeids, typeids2);

  // Verify nc_inq_type() and nc_inq_user_type() on all persisted user types.
  nc_type read_blob_t = NC_NAT;
  nc_type read_enum_t = NC_NAT;
  nc_type read_vlen_t = NC_NAT;
  nc_type read_mid_t = NC_NAT;
  nc_type read_outer_t = NC_NAT;
  nc_type read_nested_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(root_id, "blob_t", &read_blob_t));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(root_id, "flag_enum_t", &read_enum_t));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(root_id, "int_seq_t", &read_vlen_t));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(root_id, "mid_fixed_t", &read_mid_t));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(root_id, "outer_var_t", &read_outer_t));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(root_id, "nested_var_t", &read_nested_t));

  char name[NC_MAX_NAME + 1]{};
  size_t size = 0;
  nc_type base_type = NC_NAT;
  size_t nfields = 0;
  int type_class = 0;

  // Opaque
  ASSERT_EQ(NC_NOERR, nc_inq_type(root_id, read_blob_t, name, &size));
  EXPECT_STREQ("blob_t", name);
  EXPECT_EQ(12u, size);
  EXPECT_EQ(NC_NOERR, nc_inq_type(root_id, read_blob_t, nullptr, nullptr));
  ASSERT_EQ(NC_NOERR, nc_inq_user_type(root_id, read_blob_t, name, &size,
                                       &base_type, &nfields, &type_class));
  EXPECT_STREQ("blob_t", name);
  EXPECT_EQ(12u, size);
  EXPECT_EQ(NC_NAT, base_type);
  EXPECT_EQ(0u, nfields);
  EXPECT_EQ(NC_OPAQUE, type_class);
  EXPECT_EQ(NC_NOERR, nc_inq_user_type(root_id, read_blob_t, nullptr, nullptr,
                                       nullptr, nullptr, nullptr));

  // Enum
  ASSERT_EQ(NC_NOERR, nc_inq_user_type(root_id, read_enum_t, name, &size,
                                       &base_type, &nfields, &type_class));
  EXPECT_STREQ("flag_enum_t", name);
  EXPECT_EQ(sizeof(std::int16_t), size);
  EXPECT_EQ(NC_SHORT, base_type);
  EXPECT_EQ(3u, nfields);
  EXPECT_EQ(NC_ENUM, type_class);

  // VLEN (size must equal sizeof(nc_vlen_t) in both nc_inq_type & user_type)
  ASSERT_EQ(NC_NOERR, nc_inq_type(root_id, read_vlen_t, name, &size));
  EXPECT_STREQ("int_seq_t", name);
  EXPECT_EQ(sizeof(nc_vlen_t), size);
  ASSERT_EQ(NC_NOERR, nc_inq_user_type(root_id, read_vlen_t, name, &size,
                                       &base_type, &nfields, &type_class));
  EXPECT_STREQ("int_seq_t", name);
  EXPECT_EQ(sizeof(nc_vlen_t), size);
  EXPECT_EQ(NC_INT, base_type);
  EXPECT_EQ(0u, nfields);
  EXPECT_EQ(NC_VLEN, type_class);

  // Compound
  ASSERT_EQ(NC_NOERR, nc_inq_user_type(root_id, read_outer_t, name, &size,
                                       &base_type, &nfields, &type_class));
  EXPECT_STREQ("outer_var_t", name);
  EXPECT_EQ(sizeof(OuterVarsized), size);
  EXPECT_EQ(NC_NAT, base_type);
  EXPECT_EQ(5u, nfields);
  EXPECT_EQ(NC_COMPOUND, type_class);

  // Check varsizedness after HDF5 reopen.
  int fixed_mid = -1;
  int fixed_outer = -1;
  int fixed_nested = -1;
  EXPECT_EQ(NC_NOERR,
            nc4type_test_inq_type_fixed_size(root_id, read_mid_t, &fixed_mid));
  EXPECT_EQ(1, fixed_mid);
  EXPECT_EQ(NC_NOERR, nc4type_test_inq_type_fixed_size(root_id, read_outer_t,
                                                       &fixed_outer));
  EXPECT_EQ(0, fixed_outer);
  EXPECT_EQ(NC_NOERR, nc4type_test_inq_type_fixed_size(root_id, read_nested_t,
                                                       &fixed_nested));
  EXPECT_EQ(0, fixed_nested);

  // Read back nested varsized and fixed data, reclaiming with nc_reclaim_data()
  // and nc_free_string() before closing reader.
  int outer_varid = -1;
  int nested_varid = -1;
  int mid_varid = -1;
  int str_varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(root_id, "outer_data", &outer_varid));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(root_id, "nested_data", &nested_varid));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(root_id, "mid_data", &mid_varid));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(root_id, "str_data", &str_varid));

  OuterVarsized read_outer{};
  std::memset(&read_outer, 0, sizeof(read_outer));
  absl::Cleanup reclaim_outer = [root_id, read_outer_t, &read_outer]() {
    EXPECT_EQ(NC_NOERR, nc_reclaim_data(root_id, read_outer_t, &read_outer, 1));
  };
  NestedVarsized read_nested{};
  std::memset(&read_nested, 0, sizeof(read_nested));
  absl::Cleanup reclaim_nested = [root_id, read_nested_t, &read_nested]() {
    EXPECT_EQ(NC_NOERR,
              nc_reclaim_data(root_id, read_nested_t, &read_nested, 1));
  };
  ScopedNcStrings read_strs(1);

  const size_t idx0 = 0;
  const size_t cnt1 = 1;
  ASSERT_EQ(NC_NOERR,
            nc_get_vara(root_id, outer_varid, &idx0, &cnt1, &read_outer));
  EXPECT_EQ(7u, read_outer.tag);
  EXPECT_EQ(6, read_outer.matrix[1][2]);
  EXPECT_EQ(42, read_outer.inner.code);
  EXPECT_DOUBLE_EQ(3.5, read_outer.inner.weight);
  ASSERT_NE(nullptr, read_outer.label);
  EXPECT_STREQ("alpha_record", read_outer.label);
  ASSERT_EQ(size_t{3}, read_outer.seq.len);
  const auto* read_seq = static_cast<const std::int32_t*>(read_outer.seq.p);
  EXPECT_EQ(11, read_seq[0]);
  EXPECT_EQ(22, read_seq[1]);
  EXPECT_EQ(33, read_seq[2]);

  ASSERT_EQ(NC_NOERR,
            nc_get_vara(root_id, nested_varid, &idx0, &cnt1, &read_nested));
  EXPECT_EQ(99, read_nested.seq_no);
  EXPECT_EQ(7u, read_nested.payload.tag);
  EXPECT_EQ(42, read_nested.payload.inner.code);
  ASSERT_NE(nullptr, read_nested.payload.label);
  EXPECT_STREQ("alpha_record", read_nested.payload.label);
  ASSERT_EQ(size_t{3}, read_nested.payload.seq.len);

  MidFixed read_mid{};
  std::memset(&read_mid, 0, sizeof(read_mid));
  ASSERT_EQ(NC_NOERR, nc_get_vara(root_id, mid_varid, &idx0, &cnt1, &read_mid));
  EXPECT_EQ(5, read_mid.tag);
  EXPECT_EQ(12345, read_mid.flag);
  EXPECT_EQ(0xAB, read_mid.blob[0]);
  EXPECT_EQ(0xCD, read_mid.blob[11]);
  EXPECT_EQ(17, read_mid.inner.code);

  ASSERT_EQ(NC_NOERR, nc_get_vara_string(root_id, str_varid, &idx0, &cnt1,
                                         read_strs.data()));
  ASSERT_NE(nullptr, read_strs[0]);
  EXPECT_STREQ("alpha_record", read_strs[0]);

  // Error checks for nc_inq_typeids / nc_inq_type / nc_inq_user_type.
  const int bad_grp_ncid = MakeInvalidGroupSubId(root_id);
  int count = -1;
  EXPECT_EQ(NC_EBADID, nc_inq_typeids(bad_grp_ncid, &count, nullptr));
  EXPECT_EQ(NC_EBADID, nc_inq_type(bad_grp_ncid, read_outer_t, name, &size));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_type(root_id, 9999, name, &size));
  EXPECT_EQ(NC_EBADID, nc_inq_user_type(bad_grp_ncid, read_outer_t, name, &size,
                                        &base_type, &nfields, &type_class));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_user_type(root_id, 9999, name, &size,
                                          &base_type, &nfields, &type_class));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_user_type(root_id, NC_INT, name, &size,
                                          &base_type, &nfields, &type_class));
}

TEST(Nc4TypeTest, CompoundFieldAndFieldIndexQueries) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create_mem("nc4type_compound_fields.nc", NC_NETCDF4,
                                    4096, nc.ptr()));

  struct SampleRecord {
    std::int32_t scalar_i;
    double grid[2][3];
    float temp;
  };

  nc_type cmp_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(SampleRecord),
                                      "sample_t", &cmp_t));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), cmp_t, "scalar_i",
                               offsetof(SampleRecord, scalar_i), NC_INT));
  const int grid_dims[2] = {2, 3};
  ASSERT_EQ(NC_NOERR, nc_insert_array_compound(nc.get(), cmp_t, "grid",
                                               offsetof(SampleRecord, grid),
                                               NC_DOUBLE, 2, grid_dims));
  // UTF-8 NFC "caf\u00e9" field name.
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), cmp_t, "caf\xC3\xA9",
                               offsetof(SampleRecord, temp), NC_FLOAT));

  nc_type enum_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT, "non_cmp_enum", &enum_t));
  std::int32_t non_cmp_val = 1;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_t, "ONLY", &non_cmp_val));

  // Query scalar field (all pointers non-null and all nullable pointers null).
  char field_name[NC_MAX_NAME + 1]{};
  size_t offset = 999;
  nc_type field_type = NC_NAT;
  int ndims = -1;
  int dim_sizes[4] = {-1, -1, -1, -1};
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_field(nc.get(), cmp_t, 0, field_name, &offset,
                                  &field_type, &ndims, dim_sizes));
  EXPECT_STREQ("scalar_i", field_name);
  EXPECT_EQ(offsetof(SampleRecord, scalar_i), offset);
  EXPECT_EQ(NC_INT, field_type);
  EXPECT_EQ(0, ndims);
  EXPECT_EQ(NC_NOERR,
            nc_inq_compound_field(nc.get(), cmp_t, 0, nullptr, nullptr, nullptr,
                                  nullptr, nullptr));

  // Query 2D array field via individual accessors too.
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldname(nc.get(), cmp_t, 1, field_name));
  EXPECT_STREQ("grid", field_name);
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fieldoffset(nc.get(), cmp_t, 1, &offset));
  EXPECT_EQ(offsetof(SampleRecord, grid), offset);
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldtype(nc.get(), cmp_t, 1, &field_type));
  EXPECT_EQ(NC_DOUBLE, field_type);
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fieldndims(nc.get(), cmp_t, 1, &ndims));
  EXPECT_EQ(2, ndims);
  std::array<int, 2> dims_out = {-1, -1};
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fielddim_sizes(nc.get(), cmp_t, 1,
                                                     dims_out.data()));
  EXPECT_THAT(dims_out, ElementsAre(2, 3));

  // Lookup by field name, iterating past non-matching fields and using NFD.
  int field_id = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldindex(nc.get(), cmp_t, "scalar_i", &field_id));
  EXPECT_EQ(0, field_id);
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fieldindex(nc.get(), cmp_t,
                                                 "cafe\xCC\x81", &field_id));
  EXPECT_EQ(2, field_id);
  EXPECT_EQ(NC_NOERR,
            nc_inq_compound_fieldindex(nc.get(), cmp_t, "grid", nullptr));

  // Error handling for nc_inq_compound_field and nc_inq_compound_fieldindex.
  const int bad_grp_ncid = MakeInvalidGroupSubId(nc.get());
  EXPECT_EQ(NC_EBADID,
            nc_inq_compound_field(bad_grp_ncid, cmp_t, 0, field_name, &offset,
                                  &field_type, &ndims, dim_sizes));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_compound_field(nc.get(), 9999, 0, field_name, &offset,
                                  &field_type, &ndims, dim_sizes));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_compound_field(nc.get(), NC_INT, 0, field_name, &offset,
                                  &field_type, &ndims, dim_sizes));
  EXPECT_EQ(NC_EBADFIELD,
            nc_inq_compound_field(nc.get(), cmp_t, -1, field_name, &offset,
                                  &field_type, &ndims, dim_sizes));
  EXPECT_EQ(NC_EBADFIELD,
            nc_inq_compound_field(nc.get(), cmp_t, 99, field_name, &offset,
                                  &field_type, &ndims, dim_sizes));

  EXPECT_EQ(NC_EBADID, nc_inq_compound_fieldindex(bad_grp_ncid, cmp_t,
                                                  "scalar_i", &field_id));
  EXPECT_EQ(NC_EINVAL,
            nc_inq_compound_fieldindex(nc.get(), -1, "scalar_i", &field_id));
  EXPECT_EQ(NC_EBADTYPID,
            nc_inq_compound_fieldindex(nc.get(), 9999, "scalar_i", &field_id));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_compound_fieldindex(nc.get(), NC_INT,
                                                    "scalar_i", &field_id));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_compound_fieldindex(nc.get(), enum_t,
                                                    "scalar_i", &field_id));
  const std::string too_long(NC_MAX_NAME + 1, 'x');
  EXPECT_EQ(NC_EMAXNAME, nc_inq_compound_fieldindex(
                             nc.get(), cmp_t, too_long.c_str(), &field_id));
  EXPECT_EQ(NC_EBADFIELD,
            nc_inq_compound_fieldindex(nc.get(), cmp_t, "missing", &field_id));

  ScopedMemio memio;
  ASSERT_EQ(NC_NOERR, CloseMemio(nc, memio));
}

template <typename IntT>
void VerifyEnumBaseType(int ncid, nc_type base_xtype, const char* type_name,
                        IntT first_val, IntT second_val) {
  nc_type enum_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(ncid, base_xtype, type_name, &enum_t));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(ncid, enum_t, "FIRST", &first_val));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(ncid, enum_t, "SECOND", &second_val));

  char ident[NC_MAX_NAME + 1]{};
  IntT val_out = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_enum_member(ncid, enum_t, 1, ident, &val_out));
  EXPECT_STREQ("SECOND", ident);
  EXPECT_EQ(second_val, val_out);
  EXPECT_EQ(NC_NOERR, nc_inq_enum_member(ncid, enum_t, 0, nullptr, nullptr));

  ASSERT_EQ(NC_NOERR,
            nc_inq_enum_ident(ncid, enum_t, static_cast<long long>(second_val),
                              ident));
  EXPECT_STREQ("SECOND", ident);
  EXPECT_EQ(NC_NOERR,
            nc_inq_enum_ident(ncid, enum_t, static_cast<long long>(first_val),
                              nullptr));

  // Unmatched non-zero value returns NC_EINVAL.
  EXPECT_EQ(NC_EINVAL, nc_inq_enum_ident(ncid, enum_t, 7777LL, ident));

  // Unmatched zero value returns special "_UNDEFINED" identifier.
  if (first_val != 0 && second_val != 0) {
    ASSERT_EQ(NC_NOERR, nc_inq_enum_ident(ncid, enum_t, 0LL, ident));
    EXPECT_STREQ(NC_UNDEFINED_ENUM_IDENT, ident);
  }
}

TEST(Nc4TypeTest, EnumMemberAndIdentAcrossAllIntegerBaseTypes) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc4type_enums.nc", NC_NETCDF4, 4096, nc.ptr()));
  // Transition through enddef -> redef so nc_abort() discards uncommitted
  // invalid types cleanly in RAM without touching filesystem.
  ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));
  ASSERT_EQ(NC_NOERR, nc_redef(nc.get()));
  absl::Cleanup abort_on_exit = [&nc]() {
    if (nc.valid()) {
      EXPECT_EQ(NC_NOERR, nc.abort());
    }
  };

  // Note: NC_BYTE uses non-negative values in [0, 127] so x86_64 signed char
  // and ARM64 unsigned char evaluate identically inside *(char*)enum->value.
  VerifyEnumBaseType<signed char>(nc.get(), NC_BYTE, "e_byte", 1, 127);
  VerifyEnumBaseType<std::uint8_t>(nc.get(), NC_UBYTE, "e_ubyte", 2, 200);
  VerifyEnumBaseType<std::int16_t>(nc.get(), NC_SHORT, "e_short", -32000,
                                   12345);
  VerifyEnumBaseType<std::uint16_t>(nc.get(), NC_USHORT, "e_ushort", 3, 65000);
  VerifyEnumBaseType<std::int32_t>(nc.get(), NC_INT, "e_int", -100000, 42);
  VerifyEnumBaseType<std::uint32_t>(nc.get(), NC_UINT, "e_uint", 7U,
                                    3000000000U);
  VerifyEnumBaseType<std::int64_t>(nc.get(), NC_INT64, "e_int64", -5000000000LL,
                                   123456789012LL);
  VerifyEnumBaseType<std::uint64_t>(nc.get(), NC_UINT64, "e_uint64", 9ULL,
                                    5000000000ULL);

  nc_type valid_enum = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc.get(), "e_int", &valid_enum));
  nc_type opaque_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 4, "not_enum_t", &opaque_t));

  // Uncommitted float-based enum triggers default switch branch NC_EINVAL in
  // NC4_inq_enum_ident().
  nc_type float_enum_t = NC_NAT;
  ASSERT_EQ(NC_NOERR,
            nc_def_enum(nc.get(), NC_FLOAT, "e_float_invalid", &float_enum_t));
  float float_val = 1.0f;
  ASSERT_EQ(NC_NOERR,
            nc_insert_enum(nc.get(), float_enum_t, "ONE", &float_val));

  // Compound with bogus field type triggers nc4_find_type error branch in
  // NC4_recheck_varsize().
  nc_type broken_cmp_t = NC_NAT;
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(nc.get(), 8, "broken_cmp_t", &broken_cmp_t));
  EXPECT_EQ(NC_EBADTYPID,
            nc_insert_compound(nc.get(), broken_cmp_t, "bad_fld", 0, 9999));

  const int bad_grp_ncid = MakeInvalidGroupSubId(nc.get());
  char ident[NC_MAX_NAME + 1]{};
  std::int32_t val = 0;

  EXPECT_EQ(NC_EBADID,
            nc_inq_enum_member(bad_grp_ncid, valid_enum, 0, ident, &val));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_enum_member(nc.get(), 9999, 0, ident, &val));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_enum_member(nc.get(), opaque_t, 0, ident, &val));
  EXPECT_EQ(NC_EINVAL,
            nc_inq_enum_member(nc.get(), valid_enum, -1, ident, &val));
  EXPECT_EQ(NC_EINVAL,
            nc_inq_enum_member(nc.get(), valid_enum, 99, ident, &val));

  EXPECT_EQ(NC_EBADID, nc_inq_enum_ident(bad_grp_ncid, valid_enum, 42, ident));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_enum_ident(nc.get(), 9999, 42, ident));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_enum_ident(nc.get(), opaque_t, 42, ident));
  EXPECT_EQ(NC_EINVAL, nc_inq_enum_ident(nc.get(), float_enum_t, 1, ident));

  ASSERT_EQ(NC_NOERR, nc_abort(nc.release()));
}

TEST(Nc4TypeTest, InqTypeidAtomicRelativeFullyQualifiedAndRecursive) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc4type_inq_typeid.nc", NC_NETCDF4, 4096, nc.ptr()));

  struct AtomicNameCase {
    const char* name;
    nc_type expected;
  };
  constexpr AtomicNameCase kAtomicCases[] = {
      {"none", NC_NAT},      {"byte", NC_BYTE},   {"char", NC_CHAR},
      {"short", NC_SHORT},   {"int", NC_INT},     {"float", NC_FLOAT},
      {"double", NC_DOUBLE}, {"ubyte", NC_UBYTE}, {"ushort", NC_USHORT},
      {"uint", NC_UINT},     {"int64", NC_INT64}, {"uint64", NC_UINT64},
      {"string", NC_STRING},
  };
  for (const auto& tc : kAtomicCases) {
    SCOPED_TRACE(tc.name);
    nc_type got = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc.get(), tc.name, &got));
    EXPECT_EQ(tc.expected, got);
  }
  EXPECT_EQ(NC_NOERR, nc_inq_typeid(nc.get(), "int", nullptr));

  int grp_a = -1;
  int grp_nested = -1;
  int grp_b = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "grp_a", &grp_a));
  ASSERT_EQ(NC_NOERR, nc_def_grp(grp_a, "nested", &grp_nested));
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "grp_b", &grp_b));

  nc_type root_only_t = NC_NAT;
  nc_type root_shadow_t = NC_NAT;
  nc_type a_t = NC_NAT;
  nc_type a_shadow_t = NC_NAT;
  nc_type nested_t = NC_NAT;
  nc_type utf8_t = NC_NAT;
  nc_type b_t = NC_NAT;

  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 4, "root_only_t", &root_only_t));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 4, "shadow_t", &root_shadow_t));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(grp_a, 8, "a_t", &a_t));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(grp_a, 8, "shadow_t", &a_shadow_t));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(grp_nested, 12, "nested_t", &nested_t));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(grp_nested, 16, "caf\xC3\xA9_t", &utf8_t));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(grp_b, 20, "b_t", &b_t));

  nc_type got = NC_NAT;
  // Local match, shadowed local preference, and parent-chain fallback.
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(grp_a, "a_t", &got));
  EXPECT_EQ(a_t, got);
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(grp_a, "shadow_t", &got));
  EXPECT_EQ(a_shadow_t, got);
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(grp_nested, "root_only_t", &got));
  EXPECT_EQ(root_only_t, got);
  EXPECT_EQ(NC_NOERR, nc_inq_typeid(grp_a, "a_t", nullptr));

  // Recursive search from root finds descendant and sibling types plus NFD.
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(grp_b, "nested_t", &got));
  EXPECT_EQ(nested_t, got);
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(grp_a, "b_t", &got));
  EXPECT_EQ(b_t, got);
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(grp_b, "cafe\xCC\x81_t", &got));
  EXPECT_EQ(utf8_t, got);
  EXPECT_EQ(NC_NOERR, nc_inq_typeid(grp_b, "nested_t", nullptr));

  // Fully-qualified paths.
  EXPECT_EQ(NC_NOERR, nc_inq_typeid(grp_nested, "/grp_a/a_t", &got));
  EXPECT_EQ(NC_NOERR, nc_inq_typeid(nc.get(), "/grp_a/nested/nested_t", &got));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_typeid(nc.get(), "/grp_a/b_t", &got));
  EXPECT_EQ(NC_ENOGRP, nc_inq_typeid(nc.get(), "/missing_grp/a_t", &got));
  EXPECT_EQ(NC_EINVAL, nc_inq_typeid(nc.get(), "/", &got));
  EXPECT_EQ(NC_EINVAL, nc_inq_typeid(nc.get(), "/root_only_t", &got));

  // Relative slashes, bad group ncid, oversized name, and missing type name.
  EXPECT_EQ(NC_EINVAL, nc_inq_typeid(nc.get(), "grp_a/a_t", &got));
  const int bad_grp_ncid = MakeInvalidGroupSubId(nc.get());
  EXPECT_EQ(NC_EBADID, nc_inq_typeid(bad_grp_ncid, "root_only_t", &got));
  const std::string too_long(NC_MAX_NAME + 1, 'z');
  EXPECT_EQ(NC_EMAXNAME, nc_inq_typeid(nc.get(), too_long.c_str(), &got));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_typeid(grp_a, "no_such_type", &got));

  ScopedMemio memio;
  ASSERT_EQ(NC_NOERR, CloseMemio(nc, memio));
}

TEST(Nc4TypeTest, InternalTypeHelpersAndFixedSizeChecks) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc4type_internal.nc", NC_NETCDF4, 4096, nc.ptr()));

  nc_type enum_t = NC_NAT;
  nc_type opaque_t = NC_NAT;
  nc_type vlen_t = NC_NAT;
  nc_type cmp_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT, "cls_enum", &enum_t));
  std::int32_t e0 = 1;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_t, "ONE", &e0));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 8, "cls_opaque", &opaque_t));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "cls_vlen", NC_INT, &vlen_t));
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(nc.get(), sizeof(int), "cls_cmp", &cmp_t));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), cmp_t, "x", 0, NC_INT));

  // Direct NC4_inq_type() on atomic types.
  char name[NC_MAX_NAME + 1]{};
  size_t sz = 0;
  ASSERT_EQ(NC_NOERR, nc4type_test_inq_type(nc.get(), NC_INT, name, &sz));
  EXPECT_STREQ("int", name);
  EXPECT_EQ(sizeof(int), sz);
  ASSERT_EQ(NC_NOERR, nc4type_test_inq_type(nc.get(), NC_STRING, name, &sz));
  EXPECT_STREQ("string", name);
  ASSERT_EQ(NC_NOERR, nc4type_test_inq_type(nc.get(), NC_NAT, name, &sz));
  EXPECT_STREQ("none", name);
  EXPECT_EQ(NC_NOERR,
            nc4type_test_inq_type(nc.get(), NC_INT, nullptr, nullptr));

  // Direct NC4_inq_type_fixed_size() across atomic, user, NULL, and errors.
  int fixed = -1;
  ASSERT_EQ(NC_NOERR,
            nc4type_test_inq_type_fixed_size(nc.get(), NC_INT, &fixed));
  EXPECT_EQ(1, fixed);
  ASSERT_EQ(NC_NOERR,
            nc4type_test_inq_type_fixed_size(nc.get(), NC_STRING, &fixed));
  EXPECT_EQ(0, fixed);
  ASSERT_EQ(NC_NOERR,
            nc4type_test_inq_type_fixed_size(nc.get(), enum_t, &fixed));
  EXPECT_EQ(1, fixed);
  ASSERT_EQ(NC_NOERR,
            nc4type_test_inq_type_fixed_size(nc.get(), vlen_t, &fixed));
  EXPECT_EQ(0, fixed);
  EXPECT_EQ(NC_NOERR,
            nc4type_test_inq_type_fixed_size(nc.get(), cmp_t, nullptr));

  const int bad_grp_ncid = MakeInvalidGroupSubId(nc.get());
  EXPECT_EQ(NC_EBADID,
            nc4type_test_inq_type_fixed_size(bad_grp_ncid, cmp_t, &fixed));
  EXPECT_EQ(NC_EBADTYPID,
            nc4type_test_inq_type_fixed_size(nc.get(), 9999, &fixed));

  // Direct nc4_get_typeclass() across all switch arms + natural attr writes.
  struct ClassCase {
    nc_type xtype;
    int expected_class;
  };
  constexpr ClassCase kAtomicClassCases[] = {
      {NC_BYTE, NC_INT},     {NC_UBYTE, NC_INT},  {NC_SHORT, NC_INT},
      {NC_USHORT, NC_INT},   {NC_INT, NC_INT},    {NC_UINT, NC_INT},
      {NC_INT64, NC_INT},    {NC_UINT64, NC_INT}, {NC_FLOAT, NC_FLOAT},
      {NC_DOUBLE, NC_FLOAT}, {NC_CHAR, NC_CHAR},  {NC_STRING, NC_STRING},
  };
  for (const auto& tc : kAtomicClassCases) {
    int type_class = -1;
    ASSERT_EQ(NC_NOERR,
              nc4type_test_get_typeclass(nc.get(), tc.xtype, &type_class));
    EXPECT_EQ(tc.expected_class, type_class);
  }
  int type_class = -1;
  ASSERT_EQ(NC_NOERR,
            nc4type_test_get_typeclass(nc.get(), enum_t, &type_class));
  EXPECT_EQ(NC_ENUM, type_class);
  ASSERT_EQ(NC_NOERR,
            nc4type_test_get_typeclass(nc.get(), opaque_t, &type_class));
  EXPECT_EQ(NC_OPAQUE, type_class);
  ASSERT_EQ(NC_NOERR,
            nc4type_test_get_typeclass(nc.get(), vlen_t, &type_class));
  EXPECT_EQ(NC_VLEN, type_class);
  ASSERT_EQ(NC_NOERR, nc4type_test_get_typeclass(nc.get(), cmp_t, &type_class));
  EXPECT_EQ(NC_COMPOUND, type_class);

  EXPECT_EQ(NC_EBADID,
            nc4type_test_get_typeclass(bad_grp_ncid, NC_INT, &type_class));
  EXPECT_EQ(NC_EBADTYPE,
            nc4type_test_get_typeclass(nc.get(), NC_NAT, &type_class));
  EXPECT_EQ(NC_EBADTYPID,
            nc4type_test_get_typeclass(nc.get(), 9999, &type_class));

  // Exercise NC4_set_varsize() and NC4_var_varsized() directly across atomic
  // and user type IDs.
  int varsized = -1;
  EXPECT_EQ(NC_NOERR, nc4type_test_set_varsize(NC_INT, NC_INT, &varsized));
  EXPECT_EQ(0, varsized);
  EXPECT_EQ(NC_NOERR,
            nc4type_test_set_varsize(NC_STRING, NC_STRING, &varsized));
  EXPECT_EQ(1, varsized);
  EXPECT_EQ(NC_NOERR, nc4type_test_set_varsize(32, NC_OPAQUE, &varsized));
  EXPECT_EQ(0, varsized);
  EXPECT_EQ(NC_NOERR, nc4type_test_set_varsize(32, NC_ENUM, &varsized));
  EXPECT_EQ(0, varsized);
  EXPECT_EQ(NC_NOERR, nc4type_test_set_varsize(32, NC_VLEN, &varsized));
  EXPECT_EQ(1, varsized);
  EXPECT_EQ(NC_NOERR, nc4type_test_set_varsize(32, NC_COMPOUND, &varsized));
  EXPECT_EQ(0, varsized);

  EXPECT_EQ(0, nc4type_test_var_varsized(NC_INT, 0));
  EXPECT_EQ(1, nc4type_test_var_varsized(NC_STRING, 0));
  EXPECT_EQ(0, nc4type_test_var_varsized(32, 0));
  EXPECT_EQ(1, nc4type_test_var_varsized(32, 1));

  // Exercise global attributes on atomic types (hits nc4_get_typeclass through
  // HDF5 attr write path) and verify string attribute cleanup.
  const std::int32_t att_int = 123;
  const char att_char[] = "ok";
  const char* att_str = "str_attr";
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(nc.get(), NC_GLOBAL, "a_int", NC_INT, 1, &att_int));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(nc.get(), NC_GLOBAL, "a_char", 2, att_char));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_string(nc.get(), NC_GLOBAL, "a_str", 1, &att_str));

  ScopedMemio memio;
  ASSERT_EQ(NC_NOERR, CloseMemio(nc, memio));
  EXPECT_GT(memio.size(), size_t{0});
}

}  // namespace
}  // namespace netcdf
