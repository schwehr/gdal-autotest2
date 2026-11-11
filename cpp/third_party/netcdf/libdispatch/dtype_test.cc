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

// Unit tests for //third_party/netcdf/libdispatch/dtype.c
// (`nc_inq_type_equal`, `nc_inq_typeid`, and `nc_inq_user_type`).
//
// Uses Tier 1 in-memory datasets across classic and netCDF-4/HDF5 backends.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dtype.c

#include <cstddef>
#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

struct PairSample {
  int32_t id;
  double val;
};

TEST(DtypeTest, InvalidAndClosedNcidReturnBadId) {
  int equal = -1;
  nc_type typeid_out = NC_NAT;

  EXPECT_EQ(NC_EBADID, nc_inq_type_equal(-1, NC_INT, -1, NC_INT, &equal));
  EXPECT_EQ(NC_EBADID, nc_inq_typeid(-1, "int", &typeid_out));
  EXPECT_EQ(NC_EBADID, nc_inq_user_type(-1, NC_INT, nullptr, nullptr, nullptr,
                                        nullptr, nullptr));

  // Verify closed handle is also rejected.
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dtype_closed.nc", NC_NETCDF4, 1024, nc.ptr()));
  const int stale_id = nc.get();
  ASSERT_EQ(NC_NOERR, nc.close());
  EXPECT_EQ(NC_EBADID,
            nc_inq_type_equal(stale_id, NC_INT, stale_id, NC_INT, &equal));
  EXPECT_EQ(NC_EBADID, nc_inq_typeid(stale_id, "int", &typeid_out));
  EXPECT_EQ(NC_EBADID, nc_inq_user_type(stale_id, NC_INT, nullptr, nullptr,
                                        nullptr, nullptr, nullptr));
}

TEST(DtypeTest, ClassicDispatchAtomicAndUnsupportedUserTypeApis) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dtype_classic.nc", NC_CLOBBER, 1024, nc.ptr()));

  int equal = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_type_equal(nc.get(), NC_INT, nc.get(), NC_INT, &equal));
  EXPECT_EQ(1, equal);

  ASSERT_EQ(NC_NOERR,
            nc_inq_type_equal(nc.get(), NC_INT, nc.get(), NC_DOUBLE, &equal));
  EXPECT_EQ(0, equal);

  EXPECT_EQ(NC_NOERR,
            nc_inq_type_equal(nc.get(), NC_INT, nc.get(), NC_INT, nullptr));
  EXPECT_EQ(NC_EINVAL,
            nc_inq_type_equal(nc.get(), NC_NAT, nc.get(), NC_INT, &equal));

  // Atomic type names succeed on classic files, while unknown names fail with
  // NC_ENOTNC4.
  nc_type tid = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc.get(), "int", &tid));
  EXPECT_EQ(NC_INT, tid);
  EXPECT_EQ(NC_ENOTNC4, nc_inq_typeid(nc.get(), "missing_type", &tid));

  EXPECT_EQ(NC_ENOTNC4, nc_inq_user_type(nc.get(), NC_INT, nullptr, nullptr,
                                         nullptr, nullptr, nullptr));
}

TEST(DtypeTest, Netcdf4UserDefinedAndAtomicTypesWorkAcrossFilesAndGroups) {
  ScopedNcFile nc1;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dtype_nc4_a.nc", NC_NETCDF4, 4096, nc1.ptr()));

  int subgrpid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc1.get(), "subgrp", &subgrpid));

  nc_type compound_id = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc1.get(), sizeof(PairSample),
                                      "pair_type", &compound_id));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc1.get(), compound_id, "id",
                                         offsetof(PairSample, id), NC_INT));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc1.get(), compound_id, "val",
                                         offsetof(PairSample, val), NC_DOUBLE));

  nc_type enum_id = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc1.get(), NC_SHORT, "flag_enum", &enum_id));
  const int16_t flag_on = 1;
  const int16_t flag_off = 0;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc1.get(), enum_id, "OFF", &flag_off));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc1.get(), enum_id, "ON", &flag_on));

  nc_type vlen_id = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc1.get(), "float_vlen", NC_FLOAT, &vlen_id));

  nc_type opaque_id = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc1.get(), 12, "blob_opaque", &opaque_id));

  nc_type sub_compound_id = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_compound(subgrpid, sizeof(int32_t), "sub_rec",
                                      &sub_compound_id));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(subgrpid, sub_compound_id, "x", 0, NC_INT));

  // Uncommitted user-defined types return NC_EHDFERR before nc_enddef().
  int equal = -1;
  EXPECT_EQ(NC_EHDFERR, nc_inq_type_equal(nc1.get(), compound_id, nc1.get(),
                                          compound_id, &equal));

  ASSERT_EQ(NC_NOERR, nc_enddef(nc1.get()));

  // Second file with an identical compound definition to test cross-file equal.
  ScopedNcFile nc2;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dtype_nc4_b.nc", NC_NETCDF4, 4096, nc2.ptr()));
  nc_type compound2_id = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc2.get(), sizeof(PairSample),
                                      "pair_type", &compound2_id));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc2.get(), compound2_id, "id",
                                         offsetof(PairSample, id), NC_INT));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc2.get(), compound2_id, "val",
                                         offsetof(PairSample, val), NC_DOUBLE));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc2.get()));

  // Committed equality checks: same type, cross-file equivalent, distinct,
  // atomic vs user type, and invalid inputs.
  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1.get(), compound_id, nc1.get(),
                                        compound_id, &equal));
  EXPECT_EQ(1, equal);

  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1.get(), compound_id, nc2.get(),
                                        compound2_id, &equal));
  EXPECT_EQ(1, equal);

  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1.get(), compound_id, nc1.get(),
                                        opaque_id, &equal));
  EXPECT_EQ(0, equal);

  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1.get(), NC_INT, nc1.get(),
                                        compound_id, &equal));
  EXPECT_EQ(0, equal);

  ASSERT_EQ(NC_NOERR, nc_inq_type_equal(nc1.get(), NC_DOUBLE, nc1.get(),
                                        NC_DOUBLE, &equal));
  EXPECT_EQ(1, equal);

  ASSERT_EQ(NC_NOERR,
            nc_inq_type_equal(nc1.get(), NC_BYTE, nc1.get(), NC_INT64, &equal));
  EXPECT_EQ(0, equal);

  EXPECT_EQ(NC_EBADTYPE, nc_inq_type_equal(nc1.get(), 99999, nc1.get(),
                                           compound_id, &equal));
  EXPECT_EQ(NC_EINVAL,
            nc_inq_type_equal(nc1.get(), NC_NAT, nc1.get(), NC_INT, &equal));

  // nc_inq_typeid checks: atomic, root from root/subgroup, subgroup from root,
  // FQN, malformed relative path with slash, and missing type.
  nc_type queried_id = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc1.get(), "double", &queried_id));
  EXPECT_EQ(NC_DOUBLE, queried_id);

  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc1.get(), "pair_type", &queried_id));
  EXPECT_EQ(compound_id, queried_id);

  ASSERT_EQ(NC_NOERR, nc_inq_typeid(subgrpid, "pair_type", &queried_id));
  EXPECT_EQ(compound_id, queried_id);

  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc1.get(), "sub_rec", &queried_id));
  EXPECT_EQ(sub_compound_id, queried_id);

  EXPECT_EQ(NC_NOERR, nc_inq_typeid(nc1.get(), "/subgrp/sub_rec", &queried_id));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_typeid(nc1.get(), "/subgrp/missing", &queried_id));
  EXPECT_EQ(NC_EINVAL, nc_inq_typeid(nc1.get(), "bad/path", &queried_id));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_typeid(nc1.get(), "nonexistent", &queried_id));

  // nc_inq_user_type checks for compound, enum, vlen, opaque, null outs, and
  // bad typeid.
  char name[NC_MAX_NAME + 1]{};
  size_t size = 0;
  nc_type base_type = NC_NAT;
  size_t nfields = 0;
  int type_class = 0;

  ASSERT_EQ(NC_NOERR, nc_inq_user_type(nc1.get(), compound_id, name, &size,
                                       &base_type, &nfields, &type_class));
  EXPECT_STREQ("pair_type", name);
  EXPECT_EQ(sizeof(PairSample), size);
  EXPECT_EQ(NC_NAT, base_type);
  EXPECT_EQ(size_t{2}, nfields);
  EXPECT_EQ(NC_COMPOUND, type_class);

  ASSERT_EQ(NC_NOERR, nc_inq_user_type(nc1.get(), enum_id, name, &size,
                                       &base_type, &nfields, &type_class));
  EXPECT_STREQ("flag_enum", name);
  EXPECT_EQ(sizeof(int16_t), size);
  EXPECT_EQ(NC_SHORT, base_type);
  EXPECT_EQ(size_t{2}, nfields);
  EXPECT_EQ(NC_ENUM, type_class);

  ASSERT_EQ(NC_NOERR, nc_inq_user_type(nc1.get(), vlen_id, name, &size,
                                       &base_type, &nfields, &type_class));
  EXPECT_STREQ("float_vlen", name);
  EXPECT_EQ(sizeof(nc_vlen_t), size);
  EXPECT_EQ(NC_FLOAT, base_type);
  EXPECT_EQ(size_t{0}, nfields);
  EXPECT_EQ(NC_VLEN, type_class);

  ASSERT_EQ(NC_NOERR, nc_inq_user_type(nc1.get(), opaque_id, name, &size,
                                       &base_type, &nfields, &type_class));
  EXPECT_STREQ("blob_opaque", name);
  EXPECT_EQ(size_t{12}, size);
  EXPECT_EQ(NC_NAT, base_type);
  EXPECT_EQ(size_t{0}, nfields);
  EXPECT_EQ(NC_OPAQUE, type_class);

  EXPECT_EQ(NC_NOERR, nc_inq_user_type(nc1.get(), compound_id, nullptr, nullptr,
                                       nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_user_type(nc1.get(), 99999, name, &size,
                                          &base_type, &nfields, &type_class));
}

}  // namespace
}  // namespace netcdf
