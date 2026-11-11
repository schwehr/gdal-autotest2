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

// Unit tests for //third_party/netcdf/libdispatch/dattput.c.
//
// Covers all 15 nc_put_att* dispatch entry points, atomic/string/user-defined
// types, type conversion and NC_ERANGE detection, classic/CDF-5/netCDF-4 define
// and overwrite rules, NC_FillValue checks, memio persistence, and errors.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dattput.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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

template <typename T, std::size_t N>
class ScopedReclaimedArray {
 public:
  ScopedReclaimedArray(int ncid, nc_type xtype) : ncid_(ncid), xtype_(xtype) {}

  ~ScopedReclaimedArray() {
    if (ncid_ >= 0 && xtype_ > 0) {
      EXPECT_EQ(NC_NOERR,
                nc_reclaim_data(ncid_, xtype_, items_.data(), items_.size()));
    }
  }

  ScopedReclaimedArray(const ScopedReclaimedArray&) = delete;
  ScopedReclaimedArray& operator=(const ScopedReclaimedArray&) = delete;

  T* data() { return items_.data(); }
  const T& operator[](std::size_t index) const { return items_[index]; }

 private:
  int ncid_ = -1;
  nc_type xtype_ = NC_NAT;
  std::array<T, N> items_{};
};

TEST(DattputTest, RejectsInvalidAndClosedNcidForAllPutFunctions) {
  auto check_bad_id = [](int bad_ncid) {
    const char* str = "abc";
    constexpr char kTxt[] = "txt";
    constexpr signed char kSc = -1;
    constexpr unsigned char kUc = 1;
    constexpr short kSh = -2;
    constexpr int kInt = -3;
    constexpr long kLng = -4;
    constexpr float kFlt = 1.5F;
    constexpr double kDbl = 2.5;
    constexpr unsigned short kUsh = 2;
    constexpr unsigned int kUi = 3;
    constexpr long long kLl = -5;
    constexpr unsigned long long kUll = 5;

    EXPECT_EQ(NC_EBADID, nc_put_att_string(bad_ncid, NC_GLOBAL, "s", 1, &str));
    EXPECT_EQ(NC_EBADID, nc_put_att_text(bad_ncid, NC_GLOBAL, "t", 3, kTxt));
    EXPECT_EQ(NC_EBADID, nc_put_att(bad_ncid, 0, "g", NC_INT, 1, &kInt));
    EXPECT_EQ(NC_EBADID,
              nc_put_att_schar(bad_ncid, NC_GLOBAL, "sc", NC_BYTE, 1, &kSc));
    EXPECT_EQ(NC_EBADID,
              nc_put_att_uchar(bad_ncid, NC_GLOBAL, "uc", NC_BYTE, 1, &kUc));
    EXPECT_EQ(NC_EBADID,
              nc_put_att_short(bad_ncid, NC_GLOBAL, "sh", NC_SHORT, 1, &kSh));
    EXPECT_EQ(NC_EBADID,
              nc_put_att_int(bad_ncid, NC_GLOBAL, "in", NC_INT, 1, &kInt));
    EXPECT_EQ(NC_EBADID,
              nc_put_att_long(bad_ncid, NC_GLOBAL, "ln", NC_INT, 1, &kLng));
    EXPECT_EQ(NC_EBADID,
              nc_put_att_float(bad_ncid, NC_GLOBAL, "fl", NC_FLOAT, 1, &kFlt));
    EXPECT_EQ(NC_EBADID, nc_put_att_double(bad_ncid, NC_GLOBAL, "db", NC_DOUBLE,
                                           1, &kDbl));
    EXPECT_EQ(NC_EBADID,
              nc_put_att_ubyte(bad_ncid, NC_GLOBAL, "ub", NC_UBYTE, 1, &kUc));
    EXPECT_EQ(NC_EBADID, nc_put_att_ushort(bad_ncid, NC_GLOBAL, "us", NC_USHORT,
                                           1, &kUsh));
    EXPECT_EQ(NC_EBADID,
              nc_put_att_uint(bad_ncid, NC_GLOBAL, "ui", NC_UINT, 1, &kUi));
    EXPECT_EQ(NC_EBADID, nc_put_att_longlong(bad_ncid, NC_GLOBAL, "ll",
                                             NC_INT64, 1, &kLl));
    EXPECT_EQ(NC_EBADID, nc_put_att_ulonglong(bad_ncid, NC_GLOBAL, "ul",
                                              NC_UINT64, 1, &kUll));
  };

  check_bad_id(-1);
  check_bad_id(999999);

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create("dattput_closed.nc", NC_DISKLESS, nc.ptr()));
  const int closed_ncid = nc.get();
  ASSERT_EQ(NC_NOERR, nc.close());

  check_bad_id(closed_ncid);
}

TEST(DattputTest, WritesAllAtomicAndUserTypesInNetcdf4AndPersistsViaMemio) {
  struct RecordAttr {
    std::int32_t id;
    std::int32_t code;
    double weight;
    char* label;
  };
  static_assert(sizeof(RecordAttr) ==
                sizeof(std::int32_t) * 2 + sizeof(double) + sizeof(char*));

  constexpr nc_type kLongXtype = sizeof(long) >= 8 ? NC_INT64 : NC_INT;
  constexpr long kLongValA =
      static_cast<long>(sizeof(long) >= 8 ? -5000000000LL : -50000LL);
  constexpr long kLongValB =
      static_cast<long>(sizeof(long) >= 8 ? 5000000000LL : 50000LL);

  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dattput_nc4.nc", NC_NETCDF4, 8192, nc.ptr()));

    nc_type opaque_type = NC_NAT;
    nc_type enum_type = NC_NAT;
    nc_type vlen_type = NC_NAT;
    nc_type compound_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 4, "op4_t", &opaque_type));
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT, "state_t", &enum_type));
    constexpr int kOk = 1;
    constexpr int kFail = 2;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_type, "OK", &kOk));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_type, "FAIL", &kFail));
    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "ivl_t", NC_INT, &vlen_type));
    ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(RecordAttr), "rec_t",
                                        &compound_type));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), compound_type, "id",
                                           offsetof(RecordAttr, id), NC_INT));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), compound_type, "code",
                                           offsetof(RecordAttr, code), NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), compound_type, "weight",
                                 offsetof(RecordAttr, weight), NC_DOUBLE));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), compound_type, "label",
                                 offsetof(RecordAttr, label), NC_STRING));

    int subgrp = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "child", &subgrp));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(subgrp, "dim0", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(subgrp, "val", NC_INT, 1, &dimid, &varid));

    // All 15 nc_put_att* functions across atomic and zero-length values.
    std::array<const char*, 3> strings = {"north", "", "south"};
    constexpr char kText[] = "dataset_text";
    constexpr std::array<signed char, 2> kSchars = {-120, 120};
    constexpr std::array<unsigned char, 2> kUchars = {0, 250};
    constexpr std::array<unsigned char, 2> kUbytes = {10, 200};
    constexpr std::array<short, 2> kShorts = {-30000, 30000};
    constexpr std::array<unsigned short, 2> kUshorts = {0, 65000};
    constexpr std::array<int, 2> kInts = {-100000, 100000};
    constexpr std::array<unsigned int, 2> kUints = {1U, 4000000000U};
    const std::array<long, 2> kLongs = {kLongValA, kLongValB};
    constexpr std::array<long long, 2> kLongLongs = {-9000000000000LL,
                                                     9000000000000LL};
    constexpr std::array<unsigned long long, 2> kUlongLongs = {
        7ULL, 18000000000000000007ULL};
    constexpr std::array<float, 2> kFloats = {-1.25F, 3.5F};
    constexpr std::array<double, 2> kDoubles = {-12.5, 42.25};
    constexpr int kGenericInt = 777;

    ASSERT_EQ(NC_NOERR, nc_put_att_string(nc.get(), NC_GLOBAL, "a_string",
                                          strings.size(), strings.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_string(nc.get(), NC_GLOBAL, "a_empty_str", 0,
                                          nullptr));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "a_text",
                                        sizeof(kText) - 1, kText));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(nc.get(), NC_GLOBAL, "a_empty_txt", 0, nullptr));
    ASSERT_EQ(NC_NOERR, nc_put_att(nc.get(), NC_GLOBAL, "a_generic", NC_INT, 1,
                                   &kGenericInt));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_schar(nc.get(), NC_GLOBAL, "a_schar", NC_BYTE,
                               kSchars.size(), kSchars.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uchar(nc.get(), NC_GLOBAL, "a_uchar", NC_UBYTE,
                               kUchars.size(), kUchars.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_ubyte(nc.get(), NC_GLOBAL, "a_ubyte", NC_UBYTE,
                               kUbytes.size(), kUbytes.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_short(nc.get(), NC_GLOBAL, "a_short", NC_SHORT,
                               kShorts.size(), kShorts.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_ushort(nc.get(), NC_GLOBAL, "a_ushort", NC_USHORT,
                                kUshorts.size(), kUshorts.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "a_int", NC_INT,
                                       kInts.size(), kInts.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_uint(nc.get(), NC_GLOBAL, "a_uint", NC_UINT,
                                        kUints.size(), kUints.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_long(nc.get(), NC_GLOBAL, "a_long", kLongXtype,
                              kLongs.size(), kLongs.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_longlong(nc.get(), NC_GLOBAL, "a_ll", NC_INT64,
                                  kLongLongs.size(), kLongLongs.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "a_ull", NC_UINT64,
                                   kUlongLongs.size(), kUlongLongs.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_float(nc.get(), NC_GLOBAL, "a_float", NC_FLOAT,
                               kFloats.size(), kFloats.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_double(nc.get(), NC_GLOBAL, "a_double", NC_DOUBLE,
                                kDoubles.size(), kDoubles.data()));

    // Subgroup variable NC_FillValue and user-defined attributes via
    // nc_put_att.
    constexpr int kFillVal = -999;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(subgrp, varid, NC_FillValue, NC_INT, 1,
                                       &kFillVal));

    constexpr std::array<unsigned char, 8> kOpaque = {1, 2, 3, 4, 9, 8, 7, 6};
    constexpr std::array<int, 2> kEnumVals = {kOk, kFail};
    std::array<int, 2> vlen0 = {11, 22};
    std::array<int, 3> vlen1 = {33, 44, 55};
    const std::array<nc_vlen_t, 2> kVlens = {
        {{vlen0.size(), vlen0.data()}, {vlen1.size(), vlen1.data()}}};
    char label0[] = "row_one";
    char label1[] = "row_two";
    const std::array<RecordAttr, 2> kRecords = {
        {{1, 10, 1.5, label0}, {2, -20, -3.25, label1}}};

    ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, varid, "u_opaque", opaque_type, 2,
                                   kOpaque.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, varid, "u_enum", enum_type,
                                   kEnumVals.size(), kEnumVals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, varid, "u_vlen", vlen_type,
                                   kVlens.size(), kVlens.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, varid, "u_compound", compound_type,
                                   kRecords.size(), kRecords.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open_mem("dattput_nc4.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reopened.ptr()));

  ScopedNcStrings got_strings(3);
  ASSERT_EQ(NC_NOERR, nc_get_att_string(reopened.get(), NC_GLOBAL, "a_string",
                                        got_strings.data()));
  EXPECT_STREQ("north", got_strings[0]);
  EXPECT_STREQ("", got_strings[1]);
  EXPECT_STREQ("south", got_strings[2]);

  std::string got_text(12, '\0');
  ASSERT_EQ(NC_NOERR, nc_get_att_text(reopened.get(), NC_GLOBAL, "a_text",
                                      got_text.data()));
  EXPECT_EQ("dataset_text", got_text);

  std::vector<signed char> got_sc(2);
  std::vector<unsigned char> got_uc(2);
  std::vector<unsigned char> got_ub(2);
  std::vector<short> got_sh(2);
  std::vector<unsigned short> got_ush(2);
  std::vector<int> got_int(2);
  std::vector<unsigned int> got_ui(2);
  std::vector<long> got_lng(2);
  std::vector<long long> got_ll(2);
  std::vector<unsigned long long> got_ull(2);
  std::vector<float> got_flt(2);
  std::vector<double> got_dbl(2);
  int got_gen = 0;

  ASSERT_EQ(NC_NOERR,
            nc_get_att(reopened.get(), NC_GLOBAL, "a_generic", &got_gen));
  ASSERT_EQ(NC_NOERR, nc_get_att_schar(reopened.get(), NC_GLOBAL, "a_schar",
                                       got_sc.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_uchar(reopened.get(), NC_GLOBAL, "a_uchar",
                                       got_uc.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_ubyte(reopened.get(), NC_GLOBAL, "a_ubyte",
                                       got_ub.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_short(reopened.get(), NC_GLOBAL, "a_short",
                                       got_sh.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_ushort(reopened.get(), NC_GLOBAL, "a_ushort",
                                        got_ush.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(reopened.get(), NC_GLOBAL, "a_int", got_int.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_uint(reopened.get(), NC_GLOBAL, "a_uint",
                                      got_ui.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_long(reopened.get(), NC_GLOBAL, "a_long",
                                      got_lng.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_longlong(reopened.get(), NC_GLOBAL, "a_ll",
                                          got_ll.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_ulonglong(reopened.get(), NC_GLOBAL, "a_ull",
                                           got_ull.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_float(reopened.get(), NC_GLOBAL, "a_float",
                                       got_flt.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_double(reopened.get(), NC_GLOBAL, "a_double",
                                        got_dbl.data()));

  EXPECT_EQ(777, got_gen);
  EXPECT_THAT(got_sc, ::testing::ElementsAre(-120, 120));
  EXPECT_THAT(got_uc, ::testing::ElementsAre(0, 250));
  EXPECT_THAT(got_ub, ::testing::ElementsAre(10, 200));
  EXPECT_THAT(got_sh, ::testing::ElementsAre(-30000, 30000));
  EXPECT_THAT(got_ush, ::testing::ElementsAre(0, 65000));
  EXPECT_THAT(got_int, ::testing::ElementsAre(-100000, 100000));
  EXPECT_THAT(got_ui, ::testing::ElementsAre(1U, 4000000000U));
  EXPECT_THAT(got_lng, ::testing::ElementsAre(kLongValA, kLongValB));
  EXPECT_THAT(got_ll,
              ::testing::ElementsAre(-9000000000000LL, 9000000000000LL));
  EXPECT_THAT(got_ull, ::testing::ElementsAre(7ULL, 18000000000000000007ULL));
  EXPECT_THAT(got_flt, ::testing::ElementsAre(-1.25F, 3.5F));
  EXPECT_THAT(got_dbl, ::testing::ElementsAre(-12.5, 42.25));

  int subgrp = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(reopened.get(), "child", &subgrp));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(subgrp, "val", &varid));

  int got_fill = 0;
  ASSERT_EQ(NC_NOERR, nc_get_att_int(subgrp, varid, NC_FillValue, &got_fill));
  EXPECT_EQ(-999, got_fill);

  std::vector<unsigned char> got_opaque(8);
  std::vector<int> got_enum(2);
  ASSERT_EQ(NC_NOERR, nc_get_att(subgrp, varid, "u_opaque", got_opaque.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att(subgrp, varid, "u_enum", got_enum.data()));
  EXPECT_THAT(got_opaque, ::testing::ElementsAre(1, 2, 3, 4, 9, 8, 7, 6));
  EXPECT_THAT(got_enum, ::testing::ElementsAre(1, 2));

  ScopedNcVlens got_vlens(2);
  ASSERT_EQ(NC_NOERR, nc_get_att(subgrp, varid, "u_vlen", got_vlens.data()));
  ASSERT_EQ(size_t{2}, got_vlens[0].len);
  ASSERT_EQ(size_t{3}, got_vlens[1].len);
  const int* vp0 = static_cast<const int*>(got_vlens[0].p);
  const int* vp1 = static_cast<const int*>(got_vlens[1].p);
  EXPECT_EQ(11, vp0[0]);
  EXPECT_EQ(22, vp0[1]);
  EXPECT_EQ(33, vp1[0]);
  EXPECT_EQ(44, vp1[1]);
  EXPECT_EQ(55, vp1[2]);

  nc_type compound_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(subgrp, "rec_t", &compound_type));
  ScopedReclaimedArray<RecordAttr, 2> got_records(reopened.get(),
                                                  compound_type);
  ASSERT_EQ(NC_NOERR,
            nc_get_att(subgrp, varid, "u_compound", got_records.data()));
  EXPECT_EQ(1, got_records[0].id);
  EXPECT_EQ(10, got_records[0].code);
  EXPECT_DOUBLE_EQ(1.5, got_records[0].weight);
  EXPECT_STREQ("row_one", got_records[0].label);
  EXPECT_EQ(2, got_records[1].id);
  EXPECT_EQ(-20, got_records[1].code);
  EXPECT_DOUBLE_EQ(-3.25, got_records[1].weight);
  EXPECT_STREQ("row_two", got_records[1].label);
}

TEST(DattputTest, WritesCdf5ExtendedAndClassicFormats) {
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create("dattput_cdf5.nc", NC_CDF5 | NC_DISKLESS, nc.ptr()));

    constexpr unsigned char kUb = 250;
    constexpr unsigned short kUsh = 65000;
    constexpr unsigned int kUi = 4000000001U;
    constexpr long long kLl = -9000000000000000001LL;
    constexpr unsigned long long kUll = 18000000000000000001ULL;
    constexpr long kLng =
        static_cast<long>(sizeof(long) >= 8 ? -8000000000LL : -80000LL);
    constexpr nc_type kLongXtype = sizeof(long) >= 8 ? NC_INT64 : NC_INT;

    ASSERT_EQ(NC_NOERR,
              nc_put_att_ubyte(nc.get(), NC_GLOBAL, "ub", NC_UBYTE, 1, &kUb));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uchar(nc.get(), NC_GLOBAL, "uc", NC_UBYTE, 1, &kUb));
    ASSERT_EQ(NC_NOERR, nc_put_att_ushort(nc.get(), NC_GLOBAL, "us", NC_USHORT,
                                          1, &kUsh));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uint(nc.get(), NC_GLOBAL, "ui", NC_UINT, 1, &kUi));
    ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc.get(), NC_GLOBAL, "ll", NC_INT64,
                                            1, &kLl));
    ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "ul",
                                             NC_UINT64, 1, &kUll));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_long(nc.get(), NC_GLOBAL, "ln", kLongXtype, 1, &kLng));
    ASSERT_EQ(NC_NOERR,
              nc_put_att(nc.get(), NC_GLOBAL, "gen_u64", NC_UINT64, 1, &kUll));

    unsigned char got_ub = 0;
    unsigned short got_ush = 0;
    unsigned int got_ui = 0;
    long long got_ll = 0;
    unsigned long long got_ull = 0;
    unsigned long long got_gen = 0;
    long got_lng = 0;

    ASSERT_EQ(NC_NOERR, nc_get_att_ubyte(nc.get(), NC_GLOBAL, "ub", &got_ub));
    ASSERT_EQ(NC_NOERR, nc_get_att_ushort(nc.get(), NC_GLOBAL, "us", &got_ush));
    ASSERT_EQ(NC_NOERR, nc_get_att_uint(nc.get(), NC_GLOBAL, "ui", &got_ui));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_longlong(nc.get(), NC_GLOBAL, "ll", &got_ll));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_ulonglong(nc.get(), NC_GLOBAL, "ul", &got_ull));
    ASSERT_EQ(NC_NOERR, nc_get_att_long(nc.get(), NC_GLOBAL, "ln", &got_lng));
    ASSERT_EQ(NC_NOERR, nc_get_att(nc.get(), NC_GLOBAL, "gen_u64", &got_gen));

    EXPECT_EQ(kUb, got_ub);
    EXPECT_EQ(kUsh, got_ush);
    EXPECT_EQ(kUi, got_ui);
    EXPECT_EQ(kLl, got_ll);
    EXPECT_EQ(kUll, got_ull);
    EXPECT_EQ(kLng, got_lng);
    EXPECT_EQ(kUll, got_gen);
  }

  // Classic CDF-1 memio round-trip.
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dattput_classic.nc", NC_CLOBBER, 4096, nc.ptr()));

    constexpr signed char kSc = -42;
    constexpr unsigned char kUc = 210;
    constexpr short kSh = -321;
    constexpr int kIn = 123456;
    constexpr long kLn = -654321L;
    constexpr float kFl = 2.5F;
    constexpr double kDb = -9.75;

    ASSERT_EQ(NC_NOERR,
              nc_put_att_schar(nc.get(), NC_GLOBAL, "sc", NC_BYTE, 1, &kSc));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uchar(nc.get(), NC_GLOBAL, "uc", NC_BYTE, 1, &kUc));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_short(nc.get(), NC_GLOBAL, "sh", NC_SHORT, 1, &kSh));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), NC_GLOBAL, "in", NC_INT, 1, &kIn));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_long(nc.get(), NC_GLOBAL, "ln", NC_INT, 1, &kLn));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_float(nc.get(), NC_GLOBAL, "fl", NC_FLOAT, 1, &kFl));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_double(nc.get(), NC_GLOBAL, "db", NC_DOUBLE, 1, &kDb));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("dattput_classic.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, reopened.ptr()));

  signed char got_sc = 0;
  unsigned char got_uc = 0;
  short got_sh = 0;
  int got_in = 0;
  long got_ln = 0;
  float got_fl = 0.0F;
  double got_db = 0.0;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_schar(reopened.get(), NC_GLOBAL, "sc", &got_sc));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_uchar(reopened.get(), NC_GLOBAL, "uc", &got_uc));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_short(reopened.get(), NC_GLOBAL, "sh", &got_sh));
  ASSERT_EQ(NC_NOERR, nc_get_att_int(reopened.get(), NC_GLOBAL, "in", &got_in));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_long(reopened.get(), NC_GLOBAL, "ln", &got_ln));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_float(reopened.get(), NC_GLOBAL, "fl", &got_fl));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_double(reopened.get(), NC_GLOBAL, "db", &got_db));
  EXPECT_EQ(-42, got_sc);
  EXPECT_EQ(210, got_uc);
  EXPECT_EQ(-321, got_sh);
  EXPECT_EQ(123456, got_in);
  EXPECT_EQ(-654321L, got_ln);
  EXPECT_FLOAT_EQ(2.5F, got_fl);
  EXPECT_DOUBLE_EQ(-9.75, got_db);
}

TEST(DattputTest, ConvertsCompatibleTypesAndDetectsRangeOverflows) {
  for (const int cmode :
       {NC_DISKLESS, NC_CDF5 | NC_DISKLESS, NC_NETCDF4 | NC_DISKLESS}) {
    SCOPED_TRACE(cmode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattput_conv.nc", cmode, nc.ptr()));

    constexpr signed char kSc = -12;
    constexpr unsigned char kUc = 77;
    constexpr short kSh = 345;
    constexpr unsigned short kUsh = 600;
    constexpr int kInt = 1024;
    constexpr unsigned int kUi = 2048;
    constexpr long kLng = 4096;
    constexpr long long kLl = -8192;
    constexpr unsigned long long kUll = 16384;
    constexpr float kFlt = -15.0F;
    constexpr double kDbl = 25.0;

    // Cross-type writes into NC_INT / NC_DOUBLE / NC_FLOAT / NC_SHORT.
    ASSERT_EQ(NC_NOERR, nc_put_att_schar(nc.get(), NC_GLOBAL, "sc_to_int",
                                         NC_INT, 1, &kSc));
    ASSERT_EQ(NC_NOERR, nc_put_att_uchar(nc.get(), NC_GLOBAL, "uc_to_short",
                                         NC_SHORT, 1, &kUc));
    ASSERT_EQ(NC_NOERR, nc_put_att_ubyte(nc.get(), NC_GLOBAL, "ub_to_double",
                                         NC_DOUBLE, 1, &kUc));
    ASSERT_EQ(NC_NOERR, nc_put_att_short(nc.get(), NC_GLOBAL, "sh_to_double",
                                         NC_DOUBLE, 1, &kSh));
    ASSERT_EQ(NC_NOERR, nc_put_att_ushort(nc.get(), NC_GLOBAL, "us_to_int",
                                          NC_INT, 1, &kUsh));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "in_to_float",
                                       NC_FLOAT, 1, &kInt));
    ASSERT_EQ(NC_NOERR, nc_put_att_uint(nc.get(), NC_GLOBAL, "ui_to_double",
                                        NC_DOUBLE, 1, &kUi));
    ASSERT_EQ(NC_NOERR, nc_put_att_long(nc.get(), NC_GLOBAL, "ln_to_double",
                                        NC_DOUBLE, 1, &kLng));
    ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc.get(), NC_GLOBAL, "ll_to_int",
                                            NC_INT, 1, &kLl));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "ul_to_double",
                                   NC_DOUBLE, 1, &kUll));
    ASSERT_EQ(NC_NOERR, nc_put_att_float(nc.get(), NC_GLOBAL, "fl_to_int",
                                         NC_INT, 1, &kFlt));
    ASSERT_EQ(NC_NOERR, nc_put_att_double(nc.get(), NC_GLOBAL, "db_to_short",
                                          NC_SHORT, 1, &kDbl));

    int got_i = 0;
    short got_s = 0;
    double got_d = 0.0;
    float got_f = 0.0F;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(nc.get(), NC_GLOBAL, "sc_to_int", &got_i));
    EXPECT_EQ(-12, got_i);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_short(nc.get(), NC_GLOBAL, "uc_to_short", &got_s));
    EXPECT_EQ(77, got_s);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_double(nc.get(), NC_GLOBAL, "ub_to_double", &got_d));
    EXPECT_DOUBLE_EQ(77.0, got_d);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_double(nc.get(), NC_GLOBAL, "sh_to_double", &got_d));
    EXPECT_DOUBLE_EQ(345.0, got_d);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(nc.get(), NC_GLOBAL, "us_to_int", &got_i));
    EXPECT_EQ(600, got_i);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_float(nc.get(), NC_GLOBAL, "in_to_float", &got_f));
    EXPECT_FLOAT_EQ(1024.0F, got_f);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_double(nc.get(), NC_GLOBAL, "ui_to_double", &got_d));
    EXPECT_DOUBLE_EQ(2048.0, got_d);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_double(nc.get(), NC_GLOBAL, "ln_to_double", &got_d));
    EXPECT_DOUBLE_EQ(4096.0, got_d);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(nc.get(), NC_GLOBAL, "ll_to_int", &got_i));
    EXPECT_EQ(-8192, got_i);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_double(nc.get(), NC_GLOBAL, "ul_to_double", &got_d));
    EXPECT_DOUBLE_EQ(16384.0, got_d);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(nc.get(), NC_GLOBAL, "fl_to_int", &got_i));
    EXPECT_EQ(-15, got_i);
    ASSERT_EQ(NC_NOERR,
              nc_get_att_short(nc.get(), NC_GLOBAL, "db_to_short", &got_s));
    EXPECT_EQ(25, got_s);

    // Range overflows. Using integer-to-integer narrowing avoids upstream
    // UBSan float-cast-overflow traps when ERANGE_FILL is undefined.
    constexpr int kTooBigForByte = 300;
    constexpr int kTooBigForShort = 40000;
    constexpr long long kTooBigForInt = 1LL << 40;
    EXPECT_EQ(NC_ERANGE, nc_put_att_int(nc.get(), NC_GLOBAL, "ovf_b", NC_BYTE,
                                        1, &kTooBigForByte));
    EXPECT_EQ(NC_ERANGE, nc_put_att_int(nc.get(), NC_GLOBAL, "ovf_s", NC_SHORT,
                                        1, &kTooBigForShort));
    EXPECT_EQ(NC_ERANGE, nc_put_att_longlong(nc.get(), NC_GLOBAL, "ovf_i",
                                             NC_INT, 1, &kTooBigForInt));
    if constexpr (sizeof(long) > sizeof(std::int32_t)) {
      constexpr long kBigLong = static_cast<long>(1LL << 40);
      EXPECT_EQ(NC_ERANGE, nc_put_att_long(nc.get(), NC_GLOBAL, "ovf_l", NC_INT,
                                           1, &kBigLong));
    }

    // Classic CDF-1 permits unsigned char 200 into NC_BYTE as raw 8-bit data,
    // whereas CDF-5 and netCDF-4 treat NC_BYTE strictly as signed (-128..127).
    constexpr unsigned char kHighUbyte = 200;
    const int ub_to_b_stat = nc_put_att_ubyte(nc.get(), NC_GLOBAL, "ub_to_b",
                                              NC_BYTE, 1, &kHighUbyte);
    if (cmode & (NC_CDF5 | NC_NETCDF4)) {
      EXPECT_EQ(NC_ERANGE, ub_to_b_stat);
      constexpr int kNeg = -5;
      EXPECT_EQ(NC_ERANGE, nc_put_att_int(nc.get(), NC_GLOBAL, "neg_ub",
                                          NC_UBYTE, 1, &kNeg));
      EXPECT_EQ(NC_ERANGE, nc_put_att_int(nc.get(), NC_GLOBAL, "neg_us",
                                          NC_USHORT, 1, &kNeg));
      EXPECT_EQ(NC_ERANGE, nc_put_att_int(nc.get(), NC_GLOBAL, "neg_ui",
                                          NC_UINT, 1, &kNeg));
    } else {
      EXPECT_EQ(NC_NOERR, ub_to_b_stat);
    }
  }
}

TEST(DattputTest, EnforcesDefineModeOverwriteAndFillValueValidation) {
  // Classic and NC_NETCDF4|NC_CLASSIC_MODEL define/data mode overwrite rules.
  for (const int cmode :
       {NC_DISKLESS, NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS}) {
    SCOPED_TRACE(cmode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattput_modes.nc", cmode, nc.ptr()));

    constexpr std::array<int, 3> kInit = {1, 2, 3};
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "g", NC_INT,
                                       kInit.size(), kInit.data()));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    // Same size or smaller overwrite succeeds in data mode.
    constexpr std::array<int, 3> kSame = {4, 5, 6};
    constexpr std::array<short, 2> kSmaller = {7, 8};
    EXPECT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "g", NC_INT,
                                       kSame.size(), kSame.data()));
    EXPECT_EQ(NC_NOERR, nc_put_att_short(nc.get(), NC_GLOBAL, "g", NC_SHORT,
                                         kSmaller.size(), kSmaller.data()));

    // Expanding attribute or creating a new attribute requires define mode.
    constexpr std::array<double, 4> kBigger = {1.0, 2.0, 3.0, 4.0};
    constexpr int kExtra = 9;
    EXPECT_EQ(NC_ENOTINDEFINE,
              nc_put_att_double(nc.get(), NC_GLOBAL, "g", NC_DOUBLE,
                                kBigger.size(), kBigger.data()));
    EXPECT_EQ(NC_ENOTINDEFINE,
              nc_put_att_int(nc.get(), NC_GLOBAL, "added", NC_INT, 1, &kExtra));

    ASSERT_EQ(NC_NOERR, nc_redef(nc.get()));
    EXPECT_EQ(NC_NOERR, nc_put_att_double(nc.get(), NC_GLOBAL, "g", NC_DOUBLE,
                                          kBigger.size(), kBigger.data()));
    EXPECT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), NC_GLOBAL, "added", NC_INT, 1, &kExtra));

    // Classic and classic-model reject extended/string types.
    const char* str = "disallowed";
    constexpr unsigned long long kUll = 42ULL;
    const int expected_err =
        (cmode & NC_CLASSIC_MODEL) ? NC_ESTRICTNC3 : NC_EBADTYPE;
    EXPECT_EQ(expected_err,
              nc_put_att_string(nc.get(), NC_GLOBAL, "bad_s", 1, &str));
    EXPECT_EQ(expected_err, nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "bad_u64",
                                                 NC_UINT64, 1, &kUll));
  }

  // NetCDF-4 NC_FillValue length, type, and pre-write timing checks.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattput_fill_len.nc",
                                  NC_NETCDF4 | NC_DISKLESS, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));
    constexpr std::array<int, 2> kTwoFills = {-1, -2};
    EXPECT_EQ(NC_EINVAL, nc_put_att_int(nc.get(), varid, NC_FillValue, NC_INT,
                                        kTwoFills.size(), kTwoFills.data()));
    EXPECT_EQ(NC_NOERR, nc_del_att(nc.get(), varid, NC_FillValue));
    EXPECT_EQ(NC_NOERR, nc.close());
  }

  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattput_fill_type.nc",
                                  NC_NETCDF4 | NC_DISKLESS, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));
    constexpr double kWrongTypeFill = -1.0;
    EXPECT_EQ(NC_EBADTYPE, nc_put_att_double(nc.get(), varid, NC_FillValue,
                                             NC_DOUBLE, 1, &kWrongTypeFill));
    EXPECT_EQ(NC_NOERR, nc_del_att(nc.get(), varid, NC_FillValue));
    EXPECT_EQ(NC_NOERR, nc.close());
  }

  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattput_fill_late.nc",
                                  NC_NETCDF4 | NC_DISKLESS, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    constexpr std::array<int, 2> kData = {10, 20};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), varid, kData.data()));

    constexpr int kLateFill = -9;
    EXPECT_EQ(NC_ELATEFILL, nc_put_att_int(nc.get(), varid, NC_FillValue,
                                           NC_INT, 1, &kLateFill));
    EXPECT_EQ(NC_NOERR, nc.abort());
  }

  // Replacing NC_FillValue before writing data and global NC_FillValue succeed.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattput_fill_ok.nc",
                                  NC_NETCDF4 | NC_DISKLESS, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));

    constexpr int kFill1 = -11;
    constexpr int kFill2 = -22;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), varid, NC_FillValue, NC_INT, 1,
                                       &kFill1));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), varid, NC_FillValue, NC_INT, 1,
                                       &kFill2));
    EXPECT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, NC_FillValue,
                                       NC_INT, 1, &kFill1));

    int got_fill = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(nc.get(), varid, NC_FillValue, &got_fill));
    EXPECT_EQ(kFill2, got_fill);
  }
}

TEST(DattputTest, RejectsInvalidArgumentsTypesNamesAndReadOnlyDatasets) {
  for (const int cmode : {NC_DISKLESS, NC_NETCDF4 | NC_DISKLESS}) {
    SCOPED_TRACE(cmode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattput_err.nc", cmode, nc.ptr()));

    constexpr int kVal = 5;
    constexpr signed char kSchar = 5;
    constexpr float kFlt = 5.0F;
    constexpr double kDbl = 5.0;

    // Invalid types and NC_CHAR / NC_STRING conversions.
    EXPECT_EQ(NC_EBADTYPE,
              nc_put_att(nc.get(), NC_GLOBAL, "nat0", NC_NAT, 1, &kVal));
    EXPECT_EQ(NC_EBADTYPE,
              nc_put_att_int(nc.get(), NC_GLOBAL, "nat1", NC_NAT, 1, &kVal));
    EXPECT_EQ(NC_ECHAR,
              nc_put_att_int(nc.get(), NC_GLOBAL, "i_to_c", NC_CHAR, 1, &kVal));
    EXPECT_EQ(NC_ECHAR, nc_put_att_schar(nc.get(), NC_GLOBAL, "sc_to_c",
                                         NC_CHAR, 1, &kSchar));
    EXPECT_EQ(NC_ECHAR, nc_put_att_double(nc.get(), NC_GLOBAL, "d_to_c",
                                          NC_CHAR, 1, &kDbl));
    if (cmode & NC_NETCDF4) {
      EXPECT_EQ(NC_ECHAR, nc_put_att_int(nc.get(), NC_GLOBAL, "i_to_s",
                                         NC_STRING, 1, &kVal));
      EXPECT_EQ(NC_ECHAR, nc_put_att_float(nc.get(), NC_GLOBAL, "f_to_s",
                                           NC_STRING, 1, &kFlt));
    }

    // Null data pointer with non-zero length and huge length > X_INT_MAX.
    EXPECT_EQ(NC_EINVAL, nc_put_att_int(nc.get(), NC_GLOBAL, "null_data",
                                        NC_INT, 1, nullptr));
    constexpr size_t kTooLong =
        static_cast<size_t>(std::numeric_limits<std::int32_t>::max()) + 1;
    EXPECT_EQ(NC_EINVAL, nc_put_att_int(nc.get(), NC_GLOBAL, "too_long", NC_INT,
                                        kTooLong, &kVal));

    // Zero length with null pointer succeeds.
    EXPECT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "zero_int", NC_INT,
                                       0, nullptr));

    // Invalid variable ID.
    EXPECT_EQ(NC_ENOTVAR,
              nc_put_att_int(nc.get(), 99, "bad_v", NC_INT, 1, &kVal));
    EXPECT_EQ(NC_ENOTVAR,
              nc_put_att_int(nc.get(), -2, "bad_v", NC_INT, 1, &kVal));

    // Invalid and overlong attribute names.
    EXPECT_EQ(NC_EBADNAME,
              nc_put_att_int(nc.get(), NC_GLOBAL, nullptr, NC_INT, 1, &kVal));
    EXPECT_EQ(NC_EBADNAME,
              nc_put_att_int(nc.get(), NC_GLOBAL, "", NC_INT, 1, &kVal));
    EXPECT_EQ(NC_EBADNAME, nc_put_att_int(nc.get(), NC_GLOBAL, "bad/slash",
                                          NC_INT, 1, &kVal));
    EXPECT_EQ(NC_EBADNAME,
              nc_put_att_int(nc.get(), NC_GLOBAL, " lead", NC_INT, 1, &kVal));
    EXPECT_EQ(NC_EBADNAME,
              nc_put_att_int(nc.get(), NC_GLOBAL, "trail ", NC_INT, 1, &kVal));

    const std::string overlong(NC_MAX_NAME + 1, 'a');
    EXPECT_EQ((cmode & NC_NETCDF4) ? NC_EBADNAME : NC_EMAXNAME,
              nc_put_att_int(nc.get(), NC_GLOBAL, overlong.c_str(), NC_INT, 1,
                             &kVal));
  }

  // Read-only NC_NOWRITE datasets reject puts with NC_EPERM.
  for (const int cmode : {NC_CLOBBER, NC_NETCDF4}) {
    SCOPED_TRACE(cmode);
    ScopedMemio memio;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("dattput_ro.nc", cmode, 4096, nc.ptr()));
      constexpr int kInit = 1;
      ASSERT_EQ(NC_NOERR,
                nc_put_att_int(nc.get(), NC_GLOBAL, "ro", NC_INT, 1, &kInit));
      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
    }

    ScopedNcFile ro_nc;
    ASSERT_EQ(NC_NOERR, nc_open_mem("dattput_ro.nc", NC_NOWRITE, memio.size(),
                                    memio.get()->memory, ro_nc.ptr()));
    constexpr int kUpdated = 2;
    EXPECT_EQ(NC_EPERM, nc_put_att_int(ro_nc.get(), NC_GLOBAL, "ro", NC_INT, 1,
                                       &kUpdated));
    EXPECT_EQ(NC_EPERM, nc_put_att_int(ro_nc.get(), NC_GLOBAL, "new_att",
                                       NC_INT, 1, &kUpdated));
  }
}

}  // namespace
}  // namespace netcdf
