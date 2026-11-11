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

// Unit tests for //third_party/netcdf/libdispatch/dnotnc4.c.
//
// Covers all NC_NOTNC4_* and NC_NOOP_* dispatch stubs directly, verifies
// NC_Dispatch table signature compatibility, and tests classic-dataset
// public API dispatch into dnotnc4.c.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dnotnc4.c

#include <array>
#include <cstddef>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
}

namespace netcdf {
namespace {

TEST(Dnotnc4Test, QuantizeAndFilterStubsReturnExpectedCodes) {
  int quantize_mode = -1;
  int nsd = -1;
  EXPECT_EQ(NC_ENOTNC4,
            NC_NOTNC4_def_var_quantize(0, 0, NC_QUANTIZE_BITGROOM, 3));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_var_quantize(0, 0, &quantize_mode, &nsd));
  EXPECT_EQ(-1, quantize_mode);
  EXPECT_EQ(-1, nsd);

  constexpr unsigned int kParams[1] = {4};
  size_t nfilters = 99;
  unsigned int filter_ids[2] = {11, 22};
  size_t nparams = 77;
  unsigned int out_params[2] = {33, 44};

  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_def_var_filter(0, 0, 1, 1, kParams));
  EXPECT_EQ(NC_ENOTNC4,
            NC_NOTNC4_inq_var_filter_ids(0, 0, &nfilters, filter_ids));
  EXPECT_EQ(99u, nfilters);
  EXPECT_EQ(11u, filter_ids[0]);

  EXPECT_EQ(NC_ENOTNC4,
            NC_NOTNC4_inq_var_filter_info(0, 0, 1, &nparams, out_params));
  EXPECT_EQ(77u, nparams);
  EXPECT_EQ(33u, out_params[0]);

  EXPECT_EQ(NC_NOERR, NC_NOOP_inq_var_filter_ids(0, 0, &nfilters, filter_ids));
  EXPECT_EQ(0u, nfilters);
  EXPECT_EQ(NC_NOERR, NC_NOOP_inq_var_filter_ids(0, 0, nullptr, nullptr));

  EXPECT_EQ(NC_ENOFILTER,
            NC_NOOP_inq_var_filter_info(0, 0, 1, &nparams, out_params));
  EXPECT_EQ(NC_ENOFILTER, NC_NOOP_inq_filter_avail(0, 1));
}

TEST(Dnotnc4Test, GroupAndUserTypeStubsReturnNotNc4) {
  int grp_id = -1;
  nc_type type_id = -1;
  int field_id = -1;
  char name[NC_MAX_NAME + 1] = "untouched";
  size_t size_val = 123;
  size_t offset = 456;
  nc_type base_type = -1;
  int ndims = -1;
  std::array<int, 2> dim_sizes = {5, 6};
  size_t nfields = 7;
  int type_class = -1;
  long long enum_val = 42;
  int out_enum_val = -99;
  nc_vlen_t vlen{};
  int vlen_data = 88;
  size_t vlen_len = 9;

  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_def_grp(0, "subgrp", &grp_id));
  EXPECT_EQ(-1, grp_id);
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_rename_grp(0, "renamed"));

  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_def_compound(0, 16, "cmpd", &type_id));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_insert_compound(0, type_id, "f0", 0, NC_INT));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_insert_array_compound(
                            0, type_id, "f1", 4, NC_INT, 2, dim_sizes.data()));
  EXPECT_EQ(NC_ENOTNC4,
            NC_NOTNC4_inq_compound_field(0, type_id, 0, name, &offset,
                                         &base_type, &ndims, dim_sizes.data()));
  EXPECT_STREQ("untouched", name);
  EXPECT_EQ(NC_ENOTNC4,
            NC_NOTNC4_inq_compound_fieldindex(0, type_id, "f0", &field_id));

  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_def_opaque(0, 8, "op", &type_id));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_def_vlen(0, "vl", NC_INT, &type_id));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_def_enum(0, NC_INT, "en", &type_id));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_enum_ident(0, type_id, enum_val, name));
  EXPECT_EQ(NC_ENOTNC4,
            NC_NOTNC4_inq_enum_member(0, type_id, 0, name, &out_enum_val));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_insert_enum(0, type_id, "A", &enum_val));
  EXPECT_EQ(NC_ENOTNC4,
            NC_NOTNC4_put_vlen_element(0, type_id, &vlen, 1, &vlen_data));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_get_vlen_element(0, type_id, &vlen, &vlen_len,
                                                   &vlen_data));

  EXPECT_EQ(NC_ENOTNC4,
            NC_NOTNC4_inq_user_type(0, type_id, name, &size_val, &base_type,
                                    &nfields, &type_class));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_typeid(0, "int", &type_id));
  EXPECT_EQ(-1, type_id);
}

TEST(Dnotnc4Test, StorageAndGroupInquiryStubsReturnExpectedCodes) {
  size_t cache_size = 1024;
  size_t cache_nelems = 16;
  float cache_preemption = 0.5F;
  constexpr size_t kChunkSizes[2] = {4, 4};

  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_set_var_chunk_cache(
                            0, 0, cache_size, cache_nelems, cache_preemption));
  EXPECT_EQ(NC_ENOTNC4,
            NC_NOTNC4_get_var_chunk_cache(0, 0, &cache_size, &cache_nelems,
                                          &cache_preemption));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_def_var_deflate(0, 0, 1, 1, 5));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_def_var_fletcher32(0, 0, NC_FLETCHER32));
  // Unlike other stubs in dnotnc4.c, NC_NOTNC4_def_var_chunking returns
  // NC_EPERM.
  EXPECT_EQ(NC_EPERM,
            NC_NOTNC4_def_var_chunking(0, 0, NC_CHUNKED, kChunkSizes));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_def_var_endian(0, 0, NC_ENDIAN_LITTLE));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_var_par_access(0, 0, 0));

  int grp_ncid = -1;
  int count = -1;
  int ids[4] = {-1, -1, -1, -1};
  char name[NC_MAX_NAME + 1] = "keep";
  size_t full_len = 55;

  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_ncid(0, "sub", &grp_ncid));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_grps(0, &count, ids));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_grpname(0, name));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_grpname_full(0, &full_len, name));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_grp_parent(0, &grp_ncid));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_grp_full_ncid(0, "/", &grp_ncid));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_varids(0, &count, ids));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_dimids(0, &count, ids, 0));
  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC4_inq_typeids(0, &count, ids));
  EXPECT_STREQ("keep", name);
  EXPECT_EQ(55u, full_len);
}

TEST(Dnotnc4Test, DispatchTableSlotsAcceptAllStubs) {
  NC_Dispatch table{};
  table.def_var_quantize = NC_NOTNC4_def_var_quantize;
  table.inq_var_quantize = NC_NOTNC4_inq_var_quantize;
  table.def_var_filter = NC_NOTNC4_def_var_filter;
  table.inq_var_filter_ids = NC_NOOP_inq_var_filter_ids;
  table.inq_var_filter_info = NC_NOOP_inq_var_filter_info;
  table.inq_filter_avail = NC_NOOP_inq_filter_avail;
  table.def_grp = NC_NOTNC4_def_grp;
  table.rename_grp = NC_NOTNC4_rename_grp;
  table.def_compound = NC_NOTNC4_def_compound;
  table.insert_compound = NC_NOTNC4_insert_compound;
  table.insert_array_compound = NC_NOTNC4_insert_array_compound;
  table.inq_compound_field = NC_NOTNC4_inq_compound_field;
  table.inq_compound_fieldindex = NC_NOTNC4_inq_compound_fieldindex;
  table.def_opaque = NC_NOTNC4_def_opaque;
  table.def_vlen = NC_NOTNC4_def_vlen;
  table.def_enum = NC_NOTNC4_def_enum;
  table.inq_enum_ident = NC_NOTNC4_inq_enum_ident;
  table.inq_enum_member = NC_NOTNC4_inq_enum_member;
  table.insert_enum = NC_NOTNC4_insert_enum;
  table.put_vlen_element = NC_NOTNC4_put_vlen_element;
  table.get_vlen_element = NC_NOTNC4_get_vlen_element;
  table.set_var_chunk_cache = NC_NOTNC4_set_var_chunk_cache;
  table.get_var_chunk_cache = NC_NOTNC4_get_var_chunk_cache;
  table.def_var_deflate = NC_NOTNC4_def_var_deflate;
  table.def_var_fletcher32 = NC_NOTNC4_def_var_fletcher32;
  table.def_var_chunking = NC_NOTNC4_def_var_chunking;
  table.def_var_endian = NC_NOTNC4_def_var_endian;
  table.var_par_access = NC_NOTNC4_var_par_access;
  table.inq_ncid = NC_NOTNC4_inq_ncid;
  table.inq_grps = NC_NOTNC4_inq_grps;
  table.inq_grpname = NC_NOTNC4_inq_grpname;
  table.inq_grpname_full = NC_NOTNC4_inq_grpname_full;
  table.inq_grp_parent = NC_NOTNC4_inq_grp_parent;
  table.inq_grp_full_ncid = NC_NOTNC4_inq_grp_full_ncid;
  table.inq_varids = NC_NOTNC4_inq_varids;
  table.inq_dimids = NC_NOTNC4_inq_dimids;
  table.inq_typeids = NC_NOTNC4_inq_typeids;
  table.inq_user_type = NC_NOTNC4_inq_user_type;
  table.inq_typeid = NC_NOTNC4_inq_typeid;

  size_t nfilters = 42;
  EXPECT_EQ(NC_NOERR, table.inq_var_filter_ids(0, 0, &nfilters, nullptr));
  EXPECT_EQ(0u, nfilters);
  EXPECT_EQ(NC_ENOFILTER, table.inq_var_filter_info(0, 0, 1, nullptr, nullptr));
  EXPECT_EQ(NC_ENOFILTER, table.inq_filter_avail(0, 1));
  EXPECT_EQ(NC_EPERM, table.def_var_chunking(0, 0, NC_CONTIGUOUS, nullptr));
}

TEST(Dnotnc4Test, ClassicDatasetDispatchesQuantizeAndFilterNoops) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create("dnotnc4_classic.nc", NC_CLOBBER | NC_DISKLESS,
                                nc.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 4, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_FLOAT, 1, &dimid, &varid));

  EXPECT_EQ(NC_ENOTNC4,
            nc_def_var_quantize(nc.get(), varid, NC_QUANTIZE_BITGROOM, 3));
  int qmode = -1;
  int nsd = -1;
  EXPECT_EQ(NC_ENOTNC4, nc_inq_var_quantize(nc.get(), varid, &qmode, &nsd));

  size_t nfilters = 123;
  EXPECT_EQ(NC_NOERR,
            nc_inq_var_filter_ids(nc.get(), varid, &nfilters, nullptr));
  EXPECT_EQ(0u, nfilters);
  EXPECT_EQ(NC_NOERR, nc_inq_var_filter_ids(nc.get(), varid, nullptr, nullptr));
  EXPECT_EQ(NC_ENOFILTER,
            nc_inq_var_filter_info(nc.get(), varid, 1, nullptr, nullptr));
  EXPECT_EQ(NC_ENOFILTER, nc_inq_filter_avail(nc.get(), 1));

  EXPECT_EQ(NC_NOERR, nc.close());
}

}  // namespace
}  // namespace netcdf
