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

// Unit tests for //third_party/netcdf/libdispatch/dattinq.c.
//
// Covers nc_inq_att(), nc_inq_attid(), nc_inq_attname(), nc_inq_natts(),
// nc_inq_atttype(), and nc_inq_attlen() across classic/64-bit/CDF-5/netCDF-4
// formats, optional null output pointers, groups, user-defined types, UTF-8
// normalization, and error handling.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dattinq.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

TEST(DattinqTest, RejectsInvalidAndClosedNcid) {
  auto check_bad_id = [](int bad_ncid) {
    nc_type xtype = NC_NAT;
    size_t len = 123;
    int attid = -1;
    int natts = -1;
    char name[NC_MAX_NAME + 1]{};

    EXPECT_EQ(NC_EBADID, nc_inq_att(bad_ncid, NC_GLOBAL, "a", &xtype, &len));
    EXPECT_EQ(NC_EBADID, nc_inq_att(bad_ncid, 0, "a", &xtype, &len));
    EXPECT_EQ(NC_EBADID, nc_inq_attid(bad_ncid, NC_GLOBAL, "a", &attid));
    EXPECT_EQ(NC_EBADID, nc_inq_attid(bad_ncid, 0, "a", &attid));
    EXPECT_EQ(NC_EBADID, nc_inq_attname(bad_ncid, NC_GLOBAL, 0, name));
    EXPECT_EQ(NC_EBADID, nc_inq_attname(bad_ncid, 0, 0, name));
    EXPECT_EQ(NC_EBADID, nc_inq_natts(bad_ncid, &natts));
    EXPECT_EQ(NC_EBADID, nc_inq_natts(bad_ncid, nullptr));
    EXPECT_EQ(NC_EBADID, nc_inq_atttype(bad_ncid, NC_GLOBAL, "a", &xtype));
    EXPECT_EQ(NC_EBADID, nc_inq_atttype(bad_ncid, 0, "a", &xtype));
    EXPECT_EQ(NC_EBADID, nc_inq_attlen(bad_ncid, NC_GLOBAL, "a", &len));
    EXPECT_EQ(NC_EBADID, nc_inq_attlen(bad_ncid, 0, "a", &len));
  };

  check_bad_id(-1);
  check_bad_id(999999);

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create("dattinq_closed.nc", NC_DISKLESS, nc.ptr()));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "a", 2, "ok"));
  const int closed_ncid = nc.get();
  ASSERT_EQ(NC_NOERR, nc.close());

  check_bad_id(closed_ncid);
}

TEST(DattinqTest, InquiresGlobalAndVarAttributesAcrossFormatsAndNullOutputs) {
  struct FormatCase {
    const char* label;
    int cmode;
  };
  constexpr FormatCase kCases[] = {
      {"classic", NC_CLOBBER}, {"offset64", NC_64BIT_OFFSET},
      {"cdf5", NC_CDF5},       {"nc4_classic", NC_NETCDF4 | NC_CLASSIC_MODEL},
      {"netcdf4", NC_NETCDF4},
  };

  for (const FormatCase& tc : kCases) {
    SCOPED_TRACE(tc.label);
    ScopedMemio memio;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("dattinq_fmt.nc", tc.cmode, 4096, nc.ptr()));

      // Verify empty dataset and ignored nullptr output pointer in
      // nc_inq_natts.
      int natts = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_natts(nc.get(), &natts));
      EXPECT_EQ(0, natts);
      EXPECT_EQ(NC_NOERR, nc_inq_natts(nc.get(), nullptr));

      int dimid = -1;
      int varid = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 4, &dimid));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(nc.get(), "temp", NC_FLOAT, 1, &dimid, &varid));

      int var_natts = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_varnatts(nc.get(), varid, &var_natts));
      EXPECT_EQ(0, var_natts);

      constexpr std::array<signed char, 2> kFlags = {-128, 127};
      constexpr std::array<double, 3> kCoeffs = {1.5, 0.0, -2.5};
      constexpr char kUnits[] = "kelvin";
      constexpr std::array<float, 2> kRange = {200.0F, 350.0F};

      ASSERT_EQ(NC_NOERR,
                nc_put_att_text(nc.get(), NC_GLOBAL, "empty-text", 0, nullptr));
      ASSERT_EQ(NC_NOERR,
                nc_put_att_schar(nc.get(), NC_GLOBAL, "flag.bytes", NC_BYTE,
                                 kFlags.size(), kFlags.data()));
      ASSERT_EQ(NC_NOERR,
                nc_put_att_double(nc.get(), NC_GLOBAL, "coeffs_d", NC_DOUBLE,
                                  kCoeffs.size(), kCoeffs.data()));

      ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), varid, "units",
                                          sizeof(kUnits) - 1, kUnits));
      ASSERT_EQ(NC_NOERR,
                nc_put_att_float(nc.get(), varid, "valid_range", NC_FLOAT,
                                 kRange.size(), kRange.data()));

      // Verify attribute inquiry functions before closing.
      EXPECT_EQ(NC_NOERR, nc_inq_natts(nc.get(), nullptr));
      ASSERT_EQ(NC_NOERR, nc_inq_natts(nc.get(), &natts));
      EXPECT_EQ(3, natts);

      nc_type xtype = NC_NAT;
      size_t len = 999;
      ASSERT_EQ(NC_NOERR,
                nc_inq_att(nc.get(), NC_GLOBAL, "empty-text", &xtype, &len));
      EXPECT_EQ(NC_CHAR, xtype);
      EXPECT_EQ(size_t{0}, len);

      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
    }

    ScopedNcFile reopened;
    ASSERT_EQ(NC_NOERR, nc_open_mem("dattinq_fmt.nc", NC_NOWRITE, memio.size(),
                                    memio.get()->memory, reopened.ptr()));

    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened.get(), "temp", &varid));

    int natts = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_natts(reopened.get(), nullptr));
    ASSERT_EQ(NC_NOERR, nc_inq_natts(reopened.get(), &natts));
    EXPECT_EQ(3, natts);

    std::vector<std::string> global_names;
    for (int i = 0; i < natts; ++i) {
      char name_buf[NC_MAX_NAME + 1]{};
      ASSERT_EQ(NC_NOERR,
                nc_inq_attname(reopened.get(), NC_GLOBAL, i, name_buf));
      global_names.emplace_back(name_buf);

      int attid = -1;
      ASSERT_EQ(NC_NOERR,
                nc_inq_attid(reopened.get(), NC_GLOBAL, name_buf, &attid));
      EXPECT_EQ(i, attid);
      EXPECT_EQ(NC_NOERR,
                nc_inq_attid(reopened.get(), NC_GLOBAL, name_buf, nullptr));
    }
    EXPECT_THAT(global_names,
                ::testing::ElementsAre("empty-text", "flag.bytes", "coeffs_d"));

    nc_type xtype = NC_NAT;
    size_t len = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_att(reopened.get(), NC_GLOBAL, "flag.bytes",
                                   &xtype, &len));
    EXPECT_EQ(NC_BYTE, xtype);
    EXPECT_EQ(size_t{2}, len);

    // Optional null pointer combinations for nc_inq_att / atttype / attlen.
    xtype = NC_NAT;
    len = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_att(reopened.get(), NC_GLOBAL, "coeffs_d",
                                   &xtype, nullptr));
    EXPECT_EQ(NC_DOUBLE, xtype);
    EXPECT_EQ(NC_NOERR,
              nc_inq_att(reopened.get(), NC_GLOBAL, "coeffs_d", nullptr, &len));
    EXPECT_EQ(size_t{3}, len);
    EXPECT_EQ(NC_NOERR, nc_inq_att(reopened.get(), NC_GLOBAL, "coeffs_d",
                                   nullptr, nullptr));

    xtype = NC_NAT;
    len = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_atttype(reopened.get(), varid, "units", &xtype));
    ASSERT_EQ(NC_NOERR, nc_inq_attlen(reopened.get(), varid, "units", &len));
    EXPECT_EQ(NC_CHAR, xtype);
    EXPECT_EQ(size_t{6}, len);
    EXPECT_EQ(NC_NOERR,
              nc_inq_atttype(reopened.get(), varid, "units", nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_attlen(reopened.get(), varid, "units", nullptr));

    int var_attid = -1;
    char var_att_name[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR,
              nc_inq_attid(reopened.get(), varid, "valid_range", &var_attid));
    EXPECT_EQ(1, var_attid);
    ASSERT_EQ(NC_NOERR, nc_inq_attname(reopened.get(), varid, 1, var_att_name));
    EXPECT_STREQ("valid_range", var_att_name);
    ASSERT_EQ(NC_NOERR,
              nc_inq_att(reopened.get(), varid, "valid_range", &xtype, &len));
    EXPECT_EQ(NC_FLOAT, xtype);
    EXPECT_EQ(size_t{2}, len);
  }
}

TEST(DattinqTest, InquiresNetcdf4SubgroupsExtendedAndUserDefinedTypes) {
  struct SamplePair {
    std::int32_t id;
    std::int32_t tag;
    double value;
  };
  static_assert(sizeof(SamplePair) ==
                sizeof(std::int32_t) * 2 + sizeof(double));

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("dattinq_nc4.nc", NC_NETCDF4 | NC_DISKLESS, nc.ptr()));

  nc_type opaque_type = NC_NAT;
  nc_type enum_type = NC_NAT;
  nc_type vlen_type = NC_NAT;
  nc_type compound_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 4, "opaque4_t", &opaque_type));
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT, "mode_enum_t", &enum_type));
  constexpr int kIdle = 0;
  constexpr int kActive = 1;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_type, "IDLE", &kIdle));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_type, "ACTIVE", &kActive));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "ivec_t", NC_INT, &vlen_type));
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(SamplePair), "pair_t",
                                      &compound_type));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), compound_type, "id",
                                         offsetof(SamplePair, id), NC_INT));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), compound_type, "tag",
                                         offsetof(SamplePair, tag), NC_INT));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), compound_type, "value",
                               offsetof(SamplePair, value), NC_DOUBLE));

  int subgrp = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "obs", &subgrp));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(subgrp, "station", 2, &dimid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(subgrp, "depth", NC_DOUBLE, 1, &dimid, &varid));

  // Root global attributes.
  std::array<const char*, 2> tags = {"air", "sea"};
  constexpr unsigned long long kEpoch = 1700000000ULL;
  ASSERT_EQ(NC_NOERR, nc_put_att_string(nc.get(), NC_GLOBAL, "tags",
                                        tags.size(), tags.data()));
  ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "epoch",
                                           NC_UINT64, 1, &kEpoch));

  // Subgroup global attributes of user-defined types.
  constexpr std::array<unsigned char, 8> kOpaqueData = {1, 2, 3, 4, 5, 6, 7, 8};
  constexpr std::array<int, 2> kEnumVals = {kIdle, kActive};
  std::array<int, 2> vlen_a = {10, 20};
  std::array<int, 3> vlen_b = {30, 40, 50};
  const std::array<nc_vlen_t, 2> kVlenVals = {
      {{vlen_a.size(), vlen_a.data()}, {vlen_b.size(), vlen_b.data()}}};
  constexpr SamplePair kPair = {7, 9, 12.5};

  ASSERT_EQ(NC_NOERR, nc_put_att_text(subgrp, NC_GLOBAL, "site", 3, "lax"));
  ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, NC_GLOBAL, "opaque_id", opaque_type, 2,
                                 kOpaqueData.data()));
  ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, NC_GLOBAL, "status", enum_type,
                                 kEnumVals.size(), kEnumVals.data()));
  ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, NC_GLOBAL, "chunks", vlen_type,
                                 kVlenVals.size(), kVlenVals.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_att(subgrp, NC_GLOBAL, "pair", compound_type, 1, &kPair));

  // Verify independent global attribute counts for root vs subgroup.
  int root_natts = -1;
  int sub_natts = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_natts(nc.get(), &root_natts));
  ASSERT_EQ(NC_NOERR, nc_inq_natts(subgrp, &sub_natts));
  EXPECT_EQ(2, root_natts);
  EXPECT_EQ(5, sub_natts);
  EXPECT_EQ(NC_NOERR, nc_inq_natts(subgrp, nullptr));

  struct ExpectedAttr {
    int ncid;
    const char* name;
    nc_type xtype;
    size_t len;
    int id;
  };
  const ExpectedAttr kExpected[] = {
      {nc.get(), "tags", NC_STRING, 2, 0},
      {nc.get(), "epoch", NC_UINT64, 1, 1},
      {subgrp, "site", NC_CHAR, 3, 0},
      {subgrp, "opaque_id", opaque_type, 2, 1},
      {subgrp, "status", enum_type, 2, 2},
      {subgrp, "chunks", vlen_type, 2, 3},
      {subgrp, "pair", compound_type, 1, 4},
  };
  for (const ExpectedAttr& exp : kExpected) {
    nc_type got_type = NC_NAT;
    nc_type got_only_type = NC_NAT;
    size_t got_len = 0;
    size_t got_only_len = 0;
    int got_id = -1;
    char got_name[NC_MAX_NAME + 1]{};

    ASSERT_EQ(NC_NOERR,
              nc_inq_att(exp.ncid, NC_GLOBAL, exp.name, &got_type, &got_len));
    ASSERT_EQ(NC_NOERR,
              nc_inq_atttype(exp.ncid, NC_GLOBAL, exp.name, &got_only_type));
    ASSERT_EQ(NC_NOERR,
              nc_inq_attlen(exp.ncid, NC_GLOBAL, exp.name, &got_only_len));
    ASSERT_EQ(NC_NOERR, nc_inq_attid(exp.ncid, NC_GLOBAL, exp.name, &got_id));
    ASSERT_EQ(NC_NOERR, nc_inq_attname(exp.ncid, NC_GLOBAL, exp.id, got_name));

    EXPECT_EQ(exp.xtype, got_type);
    EXPECT_EQ(exp.xtype, got_only_type);
    EXPECT_EQ(exp.len, got_len);
    EXPECT_EQ(exp.len, got_only_len);
    EXPECT_EQ(exp.id, got_id);
    EXPECT_STREQ(exp.name, got_name);
  }

  // Deleting a variable attribute updates remaining attnum ordering.
  constexpr int kVal = 1;
  ASSERT_EQ(NC_NOERR, nc_put_att_int(subgrp, varid, "v0", NC_INT, 1, &kVal));
  ASSERT_EQ(NC_NOERR, nc_put_att_int(subgrp, varid, "v1", NC_INT, 1, &kVal));
  ASSERT_EQ(NC_NOERR, nc_put_att_int(subgrp, varid, "v2", NC_INT, 1, &kVal));
  ASSERT_EQ(NC_NOERR, nc_del_att(subgrp, varid, "v1"));

  int v2_id = -1;
  char att1_name[NC_MAX_NAME + 1]{};
  ASSERT_EQ(NC_NOERR, nc_inq_attid(subgrp, varid, "v2", &v2_id));
  EXPECT_EQ(1, v2_id);
  ASSERT_EQ(NC_NOERR, nc_inq_attname(subgrp, varid, 1, att1_name));
  EXPECT_STREQ("v2", att1_name);
}

TEST(DattinqTest, NormalizesUtf8NamesAndRejectsInvalidLookupArguments) {
  for (const int cmode : {NC_DISKLESS, NC_NETCDF4 | NC_DISKLESS}) {
    SCOPED_TRACE(cmode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattinq_err.nc", cmode, nc.ptr()));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));

    // Store attribute with decomposed UTF-8 (NFD: 'e' + combining acute) and
    // verify lookup works via composed UTF-8 (NFC) name.
    constexpr char kNfdName[] = "cafe\xCC\x81_temp";
    constexpr char kNfcName[] = "caf\xC3\xA9_temp";
    constexpr int kTemp = 21;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), varid, kNfdName, NC_INT, 1, &kTemp));

    nc_type xtype = NC_NAT;
    size_t len = 0;
    int attid = -1;
    char queried_name[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR, nc_inq_att(nc.get(), varid, kNfcName, &xtype, &len));
    EXPECT_EQ(NC_INT, xtype);
    EXPECT_EQ(size_t{1}, len);
    ASSERT_EQ(NC_NOERR, nc_inq_atttype(nc.get(), varid, kNfcName, &xtype));
    EXPECT_EQ(NC_INT, xtype);
    ASSERT_EQ(NC_NOERR, nc_inq_attlen(nc.get(), varid, kNfcName, &len));
    EXPECT_EQ(size_t{1}, len);
    ASSERT_EQ(NC_NOERR, nc_inq_attid(nc.get(), varid, kNfcName, &attid));
    EXPECT_EQ(0, attid);
    ASSERT_EQ(NC_NOERR, nc_inq_attname(nc.get(), varid, 0, queried_name));
    EXPECT_STREQ(kNfcName, queried_name);

    // Missing attributes.
    EXPECT_EQ(NC_ENOTATT,
              nc_inq_att(nc.get(), NC_GLOBAL, "missing", &xtype, &len));
    EXPECT_EQ(NC_ENOTATT, nc_inq_att(nc.get(), varid, "missing", &xtype, &len));
    EXPECT_EQ(NC_ENOTATT, nc_inq_attid(nc.get(), NC_GLOBAL, "missing", &attid));
    EXPECT_EQ(NC_ENOTATT, nc_inq_attid(nc.get(), varid, "missing", &attid));
    EXPECT_EQ(NC_ENOTATT, nc_inq_atttype(nc.get(), varid, "missing", &xtype));
    EXPECT_EQ(NC_ENOTATT, nc_inq_attlen(nc.get(), varid, "missing", &len));

    // Out-of-range attribute index.
    EXPECT_EQ(NC_ENOTATT, nc_inq_attname(nc.get(), NC_GLOBAL, 0, queried_name));
    EXPECT_EQ(NC_ENOTATT, nc_inq_attname(nc.get(), varid, -1, queried_name));
    EXPECT_EQ(NC_ENOTATT, nc_inq_attname(nc.get(), varid, 1, queried_name));

    // Invalid variable ID.
    EXPECT_EQ(NC_ENOTVAR, nc_inq_att(nc.get(), 99, kNfcName, &xtype, &len));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_att(nc.get(), -2, kNfcName, &xtype, &len));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_attid(nc.get(), 99, kNfcName, &attid));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_attname(nc.get(), 99, 0, queried_name));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_atttype(nc.get(), 99, kNfcName, &xtype));
    EXPECT_EQ(NC_ENOTVAR, nc_inq_attlen(nc.get(), 99, kNfcName, &len));

    // Null attribute name checks (classic NC3_inq_attid does not guard NULL
    // when attributes exist, so nc_inq_attid(nullptr) is tested on netCDF-4).
    EXPECT_EQ(NC_EBADNAME, nc_inq_att(nc.get(), varid, nullptr, &xtype, &len));
    EXPECT_EQ(NC_EBADNAME, nc_inq_atttype(nc.get(), varid, nullptr, &xtype));
    EXPECT_EQ(NC_EBADNAME, nc_inq_attlen(nc.get(), varid, nullptr, &len));
    if (cmode & NC_NETCDF4) {
      EXPECT_EQ(NC_EBADNAME, nc_inq_attid(nc.get(), varid, nullptr, &attid));
    }
  }
}

}  // namespace
}  // namespace netcdf
