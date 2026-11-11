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

// Unit tests for //third_party/netcdf/libdispatch/dcompound.c (compound types).
//
// Covers all 14 nc_*_compound* functions, dispatch/format validation, invalid
// ncid/type/field/name errors, scalar and multi-dimensional array fields,
// nested compound and string fields (with RAII nc_reclaim_data before close),
// and in-memory persistence round-trips.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dcompound.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

struct InnerCoord {
  int16_t x;
  double y;
};

struct TelemetrySample {
  uint8_t tag;
  int32_t matrix[2][3];
  InnerCoord coord;
};

struct LabelledItem {
  int32_t id;
  char* label;
};

TEST(DcompoundTest, RejectsInvalidNcidOnAllFunctions) {
  nc_type tid = NC_NAT;
  constexpr int kDims[2] = {2, 3};
  char name[NC_MAX_NAME + 1]{};
  size_t size = 0;
  size_t nfields = 0;
  size_t offset = 0;
  nc_type ftype = NC_NAT;
  int ndims = 0;
  int dims[NC_MAX_VAR_DIMS]{};
  int field_id = -1;

  EXPECT_EQ(NC_EBADID, nc_def_compound(-1, 8, "rec", &tid));
  EXPECT_EQ(NC_EBADID,
            nc_insert_compound(-1, NC_FIRSTUSERTYPEID, "a", 0, NC_INT));
  EXPECT_EQ(NC_EBADID, nc_insert_array_compound(-1, NC_FIRSTUSERTYPEID, "arr",
                                                0, NC_INT, 2, kDims));
  EXPECT_EQ(NC_EBADID,
            nc_inq_compound(-1, NC_FIRSTUSERTYPEID, name, &size, &nfields));
  EXPECT_EQ(NC_EBADID, nc_inq_compound_name(-1, NC_FIRSTUSERTYPEID, name));
  EXPECT_EQ(NC_EBADID, nc_inq_compound_size(-1, NC_FIRSTUSERTYPEID, &size));
  EXPECT_EQ(NC_EBADID,
            nc_inq_compound_nfields(-1, NC_FIRSTUSERTYPEID, &nfields));
  EXPECT_EQ(NC_EBADID, nc_inq_compound_field(-1, NC_FIRSTUSERTYPEID, 0, name,
                                             &offset, &ftype, &ndims, dims));
  EXPECT_EQ(NC_EBADID,
            nc_inq_compound_fieldname(-1, NC_FIRSTUSERTYPEID, 0, name));
  EXPECT_EQ(NC_EBADID,
            nc_inq_compound_fieldoffset(-1, NC_FIRSTUSERTYPEID, 0, &offset));
  EXPECT_EQ(NC_EBADID,
            nc_inq_compound_fieldtype(-1, NC_FIRSTUSERTYPEID, 0, &ftype));
  EXPECT_EQ(NC_EBADID,
            nc_inq_compound_fieldndims(-1, NC_FIRSTUSERTYPEID, 0, &ndims));
  EXPECT_EQ(NC_EBADID,
            nc_inq_compound_fielddim_sizes(-1, NC_FIRSTUSERTYPEID, 0, dims));
  EXPECT_EQ(NC_EBADID,
            nc_inq_compound_fieldindex(-1, NC_FIRSTUSERTYPEID, "a", &field_id));
}

TEST(DcompoundTest, RejectsClassicAndClassicModelFormats) {
  ScopedNcFile classic_nc;
  ASSERT_EQ(NC_NOERR, nc_create("dcmpd_classic.nc", NC_DISKLESS | NC_CLOBBER,
                                classic_nc.ptr()));

  nc_type tid = NC_NAT;
  constexpr int kDims[1] = {4};
  char name[NC_MAX_NAME + 1]{};
  size_t size = 0;
  size_t nfields = 0;
  size_t offset = 0;
  nc_type ftype = NC_NAT;
  int ndims = 0;
  int dims[NC_MAX_VAR_DIMS]{};
  int field_id = -1;

  EXPECT_EQ(NC_ENOTNC4, nc_def_compound(classic_nc, 8, "rec", &tid));
  EXPECT_EQ(NC_ENOTNC4,
            nc_insert_compound(classic_nc, NC_FIRSTUSERTYPEID, "a", 0, NC_INT));
  EXPECT_EQ(NC_ENOTNC4, nc_insert_array_compound(classic_nc, NC_FIRSTUSERTYPEID,
                                                 "arr", 0, NC_INT, 1, kDims));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_compound(classic_nc, NC_FIRSTUSERTYPEID, name,
                                        &size, &nfields));
  EXPECT_EQ(NC_ENOTNC4,
            nc_inq_compound_name(classic_nc, NC_FIRSTUSERTYPEID, name));
  EXPECT_EQ(NC_ENOTNC4,
            nc_inq_compound_size(classic_nc, NC_FIRSTUSERTYPEID, &size));
  EXPECT_EQ(NC_ENOTNC4,
            nc_inq_compound_nfields(classic_nc, NC_FIRSTUSERTYPEID, &nfields));
  EXPECT_EQ(NC_ENOTNC4,
            nc_inq_compound_field(classic_nc, NC_FIRSTUSERTYPEID, 0, name,
                                  &offset, &ftype, &ndims, dims));
  EXPECT_EQ(NC_ENOTNC4,
            nc_inq_compound_fieldname(classic_nc, NC_FIRSTUSERTYPEID, 0, name));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_compound_fieldoffset(
                            classic_nc, NC_FIRSTUSERTYPEID, 0, &offset));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_compound_fieldtype(
                            classic_nc, NC_FIRSTUSERTYPEID, 0, &ftype));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_compound_fieldndims(
                            classic_nc, NC_FIRSTUSERTYPEID, 0, &ndims));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_compound_fielddim_sizes(
                            classic_nc, NC_FIRSTUSERTYPEID, 0, dims));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_compound_fieldindex(
                            classic_nc, NC_FIRSTUSERTYPEID, "a", &field_id));

  ScopedNcFile strict_nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("dcmpd_strict.nc",
                      NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS | NC_CLOBBER,
                      strict_nc.ptr()));
  EXPECT_EQ(NC_ESTRICTNC3, nc_def_compound(strict_nc, 8, "rec", &tid));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_compound(strict_nc, NC_FIRSTUSERTYPEID, name,
                                         &size, &nfields));
}

TEST(DcompoundTest, ValidatesArgumentsClassesFieldsAndCommitLock) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dcmpd_validate.nc", NC_NETCDF4, 0, nc.ptr()));

  nc_type cmpd_type = NC_NAT;
  EXPECT_EQ(NC_EINVAL, nc_def_compound(nc, 0, "zero_size", &cmpd_type));
  EXPECT_EQ(NC_EBADNAME, nc_def_compound(nc, 8, "bad/cmpd", &cmpd_type));
  const std::string too_long(NC_MAX_NAME + 1, 'c');
  EXPECT_EQ(NC_EMAXNAME, nc_def_compound(nc, 8, too_long.c_str(), &cmpd_type));

  // Optional null typeidp works and name can be resolved later.
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(nc, sizeof(InnerCoord), "inner", nullptr));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc, "inner", &cmpd_type));
  EXPECT_EQ(NC_ENAMEINUSE, nc_def_compound(nc, 8, "inner", &cmpd_type));

  // Invalid type / non-compound user type checks across compound APIs.
  nc_type opaque_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "op4", &opaque_type));

  constexpr int kDims[2] = {2, 3};
  EXPECT_EQ(NC_EBADTYPE, nc_insert_compound(nc, NC_INT, "f", 0, NC_INT));
  EXPECT_EQ(NC_EBADTYPID, nc_insert_compound(nc, 99999, "f", 0, NC_INT));
  EXPECT_EQ(NC_EBADTYPE, nc_insert_compound(nc, opaque_type, "f", 0, NC_INT));
  EXPECT_EQ(NC_EBADNAME,
            nc_insert_compound(nc, cmpd_type, "bad/field", 0, NC_INT));

  EXPECT_EQ(NC_EBADTYPE,
            nc_insert_array_compound(nc, NC_INT, "arr", 0, NC_INT, 2, kDims));
  EXPECT_EQ(NC_EBADTYPID,
            nc_insert_array_compound(nc, 99999, "arr", 0, NC_INT, 2, kDims));
  EXPECT_EQ(NC_EBADTYPE, nc_insert_array_compound(nc, opaque_type, "arr", 0,
                                                  NC_INT, 2, kDims));
  EXPECT_EQ(NC_EBADNAME, nc_insert_array_compound(nc, cmpd_type, "bad/arr", 0,
                                                  NC_INT, 2, kDims));

  char name[NC_MAX_NAME + 1]{};
  size_t size = 0;
  size_t nfields = 0;
  size_t offset = 0;
  nc_type ftype = NC_NAT;
  int ndims = 0;
  int dims[NC_MAX_VAR_DIMS]{};
  int field_id = -1;

  // nc_inq_compound rejects bad typeid (NC_EBADTYPE) and non-compound user type
  // (class != NC_COMPOUND -> NC_EBADTYPE).
  EXPECT_EQ(NC_EBADTYPE, nc_inq_compound(nc, NC_INT, name, &size, &nfields));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_compound(nc, 99999, name, &size, &nfields));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_compound(nc, opaque_type, name, &size, &nfields));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_compound_name(nc, opaque_type, name));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_compound_size(nc, opaque_type, &size));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_compound_nfields(nc, opaque_type, &nfields));

  // Populate compound fields and test field inquiry error cases.
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc, cmpd_type, "x",
                               NC_COMPOUND_OFFSET(InnerCoord, x), NC_SHORT));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc, cmpd_type, "y",
                               NC_COMPOUND_OFFSET(InnerCoord, y), NC_DOUBLE));

  EXPECT_EQ(NC_EBADTYPE, nc_inq_compound_field(nc, 99999, 0, name, &offset,
                                               &ftype, &ndims, dims));
  EXPECT_EQ(NC_EBADFIELD, nc_inq_compound_field(nc, cmpd_type, -1, name,
                                                &offset, &ftype, &ndims, dims));
  EXPECT_EQ(NC_EBADFIELD, nc_inq_compound_field(nc, cmpd_type, 2, name, &offset,
                                                &ftype, &ndims, dims));
  EXPECT_EQ(NC_EBADFIELD, nc_inq_compound_fieldname(nc, cmpd_type, 2, name));
  EXPECT_EQ(NC_EBADFIELD,
            nc_inq_compound_fieldoffset(nc, cmpd_type, 2, &offset));
  EXPECT_EQ(NC_EBADFIELD, nc_inq_compound_fieldtype(nc, cmpd_type, 2, &ftype));
  EXPECT_EQ(NC_EBADFIELD, nc_inq_compound_fieldndims(nc, cmpd_type, 2, &ndims));
  EXPECT_EQ(NC_EBADFIELD,
            nc_inq_compound_fielddim_sizes(nc, cmpd_type, 2, dims));

  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_compound_fieldindex(nc, opaque_type, "x", &field_id));
  EXPECT_EQ(NC_EBADTYPID,
            nc_inq_compound_fieldindex(nc, 99999, "x", &field_id));
  EXPECT_EQ(NC_EBADFIELD,
            nc_inq_compound_fieldindex(nc, cmpd_type, "missing", &field_id));
  EXPECT_EQ(NC_NOERR, nc_inq_compound_fieldindex(nc, cmpd_type, "y", nullptr));

  // Null optional out-parameters on inquiry APIs succeed cleanly.
  EXPECT_EQ(NC_NOERR,
            nc_inq_compound(nc, cmpd_type, nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_NOERR, nc_inq_compound_field(nc, cmpd_type, 0, nullptr, nullptr,
                                            nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_NOERR,
            nc_inq_compound_fielddim_sizes(nc, cmpd_type, 0, nullptr));

  // Committed compound rejects additional fields with NC_ETYPDEFINED.
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  EXPECT_EQ(NC_ETYPDEFINED,
            nc_insert_compound(nc, cmpd_type, "late", 0, NC_INT));
  EXPECT_EQ(NC_ETYPDEFINED, nc_insert_array_compound(nc, cmpd_type, "late_arr",
                                                     0, NC_INT, 2, kDims));
}

TEST(DcompoundTest, NestedAndArrayFieldsRoundTripAndReopenInMemory) {
  std::array<TelemetrySample, 2> input{};
  input[0].tag = 7;
  input[0].matrix[0][0] = 10;
  input[0].matrix[0][1] = 20;
  input[0].matrix[0][2] = 30;
  input[0].matrix[1][0] = 40;
  input[0].matrix[1][1] = 50;
  input[0].matrix[1][2] = 60;
  input[0].coord.x = -12;
  input[0].coord.y = 3.25;

  input[1].tag = 99;
  input[1].matrix[0][0] = -1;
  input[1].matrix[0][1] = -2;
  input[1].matrix[0][2] = -3;
  input[1].matrix[1][0] = -4;
  input[1].matrix[1][1] = -5;
  input[1].matrix[1][2] = -6;
  input[1].coord.x = 300;
  input[1].coord.y = -7.5;

  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create_mem("dcmpd_rt.nc", NC_NETCDF4, 0, nc.ptr()));

    nc_type inner_type = NC_NAT;
    ASSERT_EQ(NC_NOERR,
              nc_def_compound(nc, sizeof(InnerCoord), "coord_t", &inner_type));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, inner_type, "x",
                                 NC_COMPOUND_OFFSET(InnerCoord, x), NC_SHORT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, inner_type, "y",
                                 NC_COMPOUND_OFFSET(InnerCoord, y), NC_DOUBLE));

    nc_type outer_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_compound(nc, sizeof(TelemetrySample),
                                        "telemetry_t", &outer_type));
    ASSERT_EQ(
        NC_NOERR,
        nc_insert_compound(nc, outer_type, "tag",
                           NC_COMPOUND_OFFSET(TelemetrySample, tag), NC_UBYTE));
    constexpr int kMatDims[2] = {2, 3};
    ASSERT_EQ(NC_NOERR, nc_insert_array_compound(
                            nc, outer_type, "matrix",
                            NC_COMPOUND_OFFSET(TelemetrySample, matrix), NC_INT,
                            2, kMatDims));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc, outer_type, "coord",
                                 NC_COMPOUND_OFFSET(TelemetrySample, coord),
                                 inner_type));

    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "samples", input.size(), &dimid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "records", outer_type, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_put_att(nc, NC_GLOBAL, "reference_sample",
                                   outer_type, 1, &input[0]));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, varid, input.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("dcmpd_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));

  // Read-only file rejects new compound definitions with NC_EPERM.
  nc_type dummy_tid = NC_NAT;
  EXPECT_EQ(NC_EPERM, nc_def_compound(ro_nc, 4, "forbidden", &dummy_tid));

  nc_type outer_type = NC_NAT;
  nc_type inner_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(ro_nc, "telemetry_t", &outer_type));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(ro_nc, "coord_t", &inner_type));

  char cname[NC_MAX_NAME + 1]{};
  size_t csize = 0;
  size_t cnfields = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound(ro_nc, outer_type, cname, &csize, &cnfields));
  EXPECT_STREQ("telemetry_t", cname);
  EXPECT_EQ(sizeof(TelemetrySample), csize);
  EXPECT_EQ(3U, cnfields);

  std::memset(cname, 0, sizeof(cname));
  csize = 0;
  cnfields = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_compound_name(ro_nc, outer_type, cname));
  EXPECT_STREQ("telemetry_t", cname);
  ASSERT_EQ(NC_NOERR, nc_inq_compound_size(ro_nc, outer_type, &csize));
  EXPECT_EQ(sizeof(TelemetrySample), csize);
  ASSERT_EQ(NC_NOERR, nc_inq_compound_nfields(ro_nc, outer_type, &cnfields));
  EXPECT_EQ(3U, cnfields);

  // Check every field via nc_inq_compound_field and the single-attribute APIs.
  char fname[NC_MAX_NAME + 1]{};
  size_t offset = 999;
  nc_type ftype = NC_NAT;
  int ndims = -1;
  int dim_sizes[NC_MAX_VAR_DIMS]{};
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_field(ro_nc, outer_type, 1, fname, &offset, &ftype,
                                  &ndims, dim_sizes));
  EXPECT_STREQ("matrix", fname);
  EXPECT_EQ(NC_COMPOUND_OFFSET(TelemetrySample, matrix), offset);
  EXPECT_EQ(NC_INT, ftype);
  EXPECT_EQ(2, ndims);
  EXPECT_EQ(2, dim_sizes[0]);
  EXPECT_EQ(3, dim_sizes[1]);

  std::memset(fname, 0, sizeof(fname));
  offset = 999;
  ftype = NC_NAT;
  ndims = -1;
  std::memset(dim_sizes, 0, sizeof(dim_sizes));
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fieldname(ro_nc, outer_type, 2, fname));
  EXPECT_STREQ("coord", fname);
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fieldoffset(ro_nc, outer_type, 2, &offset));
  EXPECT_EQ(NC_COMPOUND_OFFSET(TelemetrySample, coord), offset);
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fieldtype(ro_nc, outer_type, 2, &ftype));
  int equal_inner = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_type_equal(ro_nc, inner_type, ro_nc, ftype, &equal_inner));
  EXPECT_EQ(1, equal_inner);
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fieldndims(ro_nc, outer_type, 2, &ndims));
  EXPECT_EQ(0, ndims);
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_fielddim_sizes(ro_nc, outer_type, 1, dim_sizes));
  EXPECT_EQ(2, dim_sizes[0]);
  EXPECT_EQ(3, dim_sizes[1]);

  int field_idx = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fieldindex(ro_nc, outer_type, "matrix",
                                                 &field_idx));
  EXPECT_EQ(1, field_idx);

  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "records", &varid));
  std::array<TelemetrySample, 2> output{};
  ASSERT_EQ(NC_NOERR, nc_get_var(ro_nc, varid, output.data()));
  for (size_t i = 0; i < input.size(); ++i) {
    EXPECT_EQ(input[i].tag, output[i].tag);
    EXPECT_EQ(input[i].coord.x, output[i].coord.x);
    EXPECT_DOUBLE_EQ(input[i].coord.y, output[i].coord.y);
    for (int r = 0; r < 2; ++r) {
      for (int c = 0; c < 3; ++c) {
        EXPECT_EQ(input[i].matrix[r][c], output[i].matrix[r][c]);
      }
    }
  }

  TelemetrySample att_out{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att(ro_nc, NC_GLOBAL, "reference_sample", &att_out));
  EXPECT_EQ(input[0].tag, att_out.tag);
  EXPECT_EQ(input[0].coord.x, att_out.coord.x);
  EXPECT_DOUBLE_EQ(input[0].coord.y, att_out.coord.y);
}

TEST(DcompoundTest, StringFieldCompoundReclaimedBeforeClosingFile) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dcmpd_strings.nc", NC_NETCDF4, 0, nc.ptr()));

  nc_type item_type = NC_NAT;
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(nc, sizeof(LabelledItem), "item_t", &item_type));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc, item_type, "id",
                               NC_COMPOUND_OFFSET(LabelledItem, id), NC_INT));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(
                          nc, item_type, "label",
                          NC_COMPOUND_OFFSET(LabelledItem, label), NC_STRING));

  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "items", 2, &dimid));
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "table", item_type, 1, &dimid, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  char label0[] = "alpha";
  char label1[] = "beta";
  std::array<LabelledItem, 2> in_rows{};
  in_rows[0].id = 101;
  in_rows[0].label = label0;
  in_rows[1].id = 202;
  in_rows[1].label = label1;
  ASSERT_EQ(NC_NOERR, nc_put_var(nc, varid, in_rows.data()));

  std::vector<LabelledItem> out_rows(2, LabelledItem{});
  absl::Cleanup reclaim_guard([&]() {
    if (nc.valid()) {
      nc_reclaim_data(nc, item_type, out_rows.data(), out_rows.size());
    }
  });
  ASSERT_EQ(NC_NOERR, nc_get_var(nc, varid, out_rows.data()));
  EXPECT_EQ(101, out_rows[0].id);
  EXPECT_STREQ("alpha", out_rows[0].label);
  EXPECT_EQ(202, out_rows[1].id);
  EXPECT_STREQ("beta", out_rows[1].label);
}

}  // namespace
}  // namespace netcdf
