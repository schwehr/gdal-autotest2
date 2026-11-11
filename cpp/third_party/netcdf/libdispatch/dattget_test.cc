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

// Unit tests for //third_party/netcdf/libdispatch/dattget.c.
//
// Covers all 15 nc_get_att* dispatch entry points, nc_get_att() inquiry error
// propagation, sentinel-guarded readback across classic/64-bit/CDF-5/netCDF-4
// formats, strings/vlens/compounds with proper reclamation order, type
// conversions, classic byte-signedness semantics, and error paths.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dattget.c

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

TEST(DattgetTest, RejectsInvalidAndClosedNcidForAllGetFunctions) {
  auto check_bad_id = [](int bad_ncid) {
    int generic_val = -1;
    char text_buf[8]{};
    signed char schar_val = 0;
    unsigned char uchar_val = 0;
    short short_val = 0;
    int int_val = 0;
    long long_val = 0;
    float float_val = 0.0F;
    double double_val = 0.0;
    unsigned char ubyte_val = 0;
    unsigned short ushort_val = 0;
    unsigned int uint_val = 0;
    long long ll_val = 0;
    unsigned long long ull_val = 0;
    char* str_val = nullptr;

    EXPECT_EQ(NC_EBADID, nc_get_att(bad_ncid, NC_GLOBAL, "a", &generic_val));
    EXPECT_EQ(NC_EBADID, nc_get_att(bad_ncid, 0, "a", &generic_val));
    EXPECT_EQ(NC_EBADID, nc_get_att_text(bad_ncid, NC_GLOBAL, "a", text_buf));
    EXPECT_EQ(NC_EBADID,
              nc_get_att_schar(bad_ncid, NC_GLOBAL, "a", &schar_val));
    EXPECT_EQ(NC_EBADID,
              nc_get_att_uchar(bad_ncid, NC_GLOBAL, "a", &uchar_val));
    EXPECT_EQ(NC_EBADID,
              nc_get_att_short(bad_ncid, NC_GLOBAL, "a", &short_val));
    EXPECT_EQ(NC_EBADID, nc_get_att_int(bad_ncid, NC_GLOBAL, "a", &int_val));
    EXPECT_EQ(NC_EBADID, nc_get_att_long(bad_ncid, NC_GLOBAL, "a", &long_val));
    EXPECT_EQ(NC_EBADID,
              nc_get_att_float(bad_ncid, NC_GLOBAL, "a", &float_val));
    EXPECT_EQ(NC_EBADID,
              nc_get_att_double(bad_ncid, NC_GLOBAL, "a", &double_val));
    EXPECT_EQ(NC_EBADID,
              nc_get_att_ubyte(bad_ncid, NC_GLOBAL, "a", &ubyte_val));
    EXPECT_EQ(NC_EBADID,
              nc_get_att_ushort(bad_ncid, NC_GLOBAL, "a", &ushort_val));
    EXPECT_EQ(NC_EBADID, nc_get_att_uint(bad_ncid, NC_GLOBAL, "a", &uint_val));
    EXPECT_EQ(NC_EBADID,
              nc_get_att_longlong(bad_ncid, NC_GLOBAL, "a", &ll_val));
    EXPECT_EQ(NC_EBADID,
              nc_get_att_ulonglong(bad_ncid, NC_GLOBAL, "a", &ull_val));
    EXPECT_EQ(NC_EBADID, nc_get_att_string(bad_ncid, NC_GLOBAL, "a", &str_val));
  };

  check_bad_id(-1);
  check_bad_id(999999);

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create("dattget_closed.nc", NC_DISKLESS, nc.ptr()));
  constexpr int kVal = 7;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(nc.get(), NC_GLOBAL, "a", NC_INT, 1, &kVal));
  const int closed_ncid = nc.get();
  ASSERT_EQ(NC_NOERR, nc.close());

  check_bad_id(closed_ncid);
}

TEST(DattgetTest, GenericGetAttPropagatesInqAtttypeErrors) {
  for (const int cmode : {NC_DISKLESS, NC_NETCDF4 | NC_DISKLESS}) {
    SCOPED_TRACE(cmode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattget_gen_err.nc", cmode, nc.ptr()));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));
    constexpr int kVal = 42;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), varid, "present", NC_INT, 1, &kVal));

    std::vector<int> sentinel_buf(2, -777);
    EXPECT_EQ(NC_ENOTATT,
              nc_get_att(nc.get(), NC_GLOBAL, "missing", sentinel_buf.data()));
    EXPECT_EQ(NC_ENOTATT,
              nc_get_att(nc.get(), varid, "missing", sentinel_buf.data()));
    EXPECT_EQ(NC_ENOTVAR,
              nc_get_att(nc.get(), 99, "present", sentinel_buf.data()));
    EXPECT_EQ(NC_ENOTVAR,
              nc_get_att(nc.get(), -2, "present", sentinel_buf.data()));
    EXPECT_EQ(NC_EBADNAME,
              nc_get_att(nc.get(), varid, nullptr, sentinel_buf.data()));
    EXPECT_THAT(sentinel_buf, ::testing::ElementsAre(-777, -777));
  }
}

TEST(DattgetTest, ReadsAllAtomicStringAndUserTypesFromMemioNetcdf4) {
  struct SampleRecord {
    std::int32_t seq;
    std::int32_t flags;
    double score;
    char* tag;
  };
  static_assert(sizeof(SampleRecord) ==
                sizeof(std::int32_t) * 2 + sizeof(double) + sizeof(char*));

  constexpr nc_type kLongXtype = sizeof(long) >= 8 ? NC_INT64 : NC_INT;
  constexpr long kLongA =
      static_cast<long>(sizeof(long) >= 8 ? -4000000001LL : -40001LL);
  constexpr long kLongB =
      static_cast<long>(sizeof(long) >= 8 ? 4000000002LL : 40002LL);

  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dattget_nc4.nc", NC_NETCDF4, 8192, nc.ptr()));

    nc_type opaque_type = NC_NAT;
    nc_type enum_type = NC_NAT;
    nc_type vlen_type = NC_NAT;
    nc_type compound_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 3, "op3_t", &opaque_type));
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT, "flag_t", &enum_type));
    constexpr int kZero = 0;
    constexpr int kOne = 1;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_type, "OFF", &kZero));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_type, "ON", &kOne));
    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "vl_int_t", NC_INT, &vlen_type));
    ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(SampleRecord), "rec_t",
                                        &compound_type));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), compound_type, "seq",
                                 offsetof(SampleRecord, seq), NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), compound_type, "flags",
                                 offsetof(SampleRecord, flags), NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), compound_type, "score",
                                 offsetof(SampleRecord, score), NC_DOUBLE));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc.get(), compound_type, "tag",
                                 offsetof(SampleRecord, tag), NC_STRING));

    int subgrp = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "sensor", &subgrp));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(subgrp, "n", 2, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(subgrp, "reading", NC_INT, 1, &dimid, &varid));

    constexpr char kText[] = "hello";
    constexpr std::array<signed char, 3> kSchar = {-128, 0, 127};
    constexpr std::array<unsigned char, 3> kUchar = {0, 128, 255};
    constexpr std::array<unsigned char, 3> kUbyte = {1, 200, 254};
    constexpr std::array<short, 3> kShort = {-32768, 7, 32767};
    constexpr std::array<unsigned short, 3> kUshort = {0, 40000, 65535};
    constexpr std::array<int, 3> kInt = {-2000000, 0, 2000000};
    constexpr std::array<unsigned int, 3> kUint = {0U, 3000000000U,
                                                   4294967295U};
    const std::array<long, 2> kLong = {kLongA, kLongB};
    constexpr std::array<long long, 2> kLl = {-9000000000000000000LL,
                                              9000000000000000000LL};
    constexpr std::array<unsigned long long, 2> kUll = {
        0ULL, 18446744073709551615ULL};
    constexpr std::array<float, 3> kFlt = {-1.5F, 0.0F, 3.25F};
    constexpr std::array<double, 3> kDbl = {-25.5, 0.0, 128.125};
    std::array<const char*, 3> strs = {"first", "", "third"};

    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), NC_GLOBAL, "t",
                                        sizeof(kText) - 1, kText));
    ASSERT_EQ(NC_NOERR, nc_put_att_schar(nc.get(), NC_GLOBAL, "sc", NC_BYTE,
                                         kSchar.size(), kSchar.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_uchar(nc.get(), NC_GLOBAL, "uc", NC_UBYTE,
                                         kUchar.size(), kUchar.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_ubyte(nc.get(), NC_GLOBAL, "ub", NC_UBYTE,
                                         kUbyte.size(), kUbyte.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_short(nc.get(), NC_GLOBAL, "sh", NC_SHORT,
                                         kShort.size(), kShort.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_ushort(nc.get(), NC_GLOBAL, "us", NC_USHORT,
                                          kUshort.size(), kUshort.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "in", NC_INT,
                                       kInt.size(), kInt.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_uint(nc.get(), NC_GLOBAL, "ui", NC_UINT,
                                        kUint.size(), kUint.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_long(nc.get(), NC_GLOBAL, "ln", kLongXtype,
                                        kLong.size(), kLong.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc.get(), NC_GLOBAL, "ll", NC_INT64,
                                            kLl.size(), kLl.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "ul", NC_UINT64,
                                   kUll.size(), kUll.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_float(nc.get(), NC_GLOBAL, "fl", NC_FLOAT,
                                         kFlt.size(), kFlt.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_double(nc.get(), NC_GLOBAL, "db", NC_DOUBLE,
                                          kDbl.size(), kDbl.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_string(nc.get(), NC_GLOBAL, "st",
                                          strs.size(), strs.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "zero_int", NC_INT,
                                       0, nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_string(nc.get(), NC_GLOBAL, "zero_str", 0, nullptr));

    // Subgroup global, variable NC_FillValue, and user-defined attributes.
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(subgrp, NC_GLOBAL, "grp_lbl", 3, "lab"));
    constexpr int kFill = -555;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(subgrp, varid, NC_FillValue, NC_INT, 1, &kFill));

    constexpr std::array<unsigned char, 6> kOpaque = {10, 20, 30, 40, 50, 60};
    constexpr std::array<int, 2> kEnum = {kZero, kOne};
    std::array<int, 1> v0 = {9};
    std::array<int, 3> v1 = {4, 5, 6};
    const std::array<nc_vlen_t, 2> kVlens = {
        {{v0.size(), v0.data()}, {v1.size(), v1.data()}}};
    char tag0[] = "alpha";
    char tag1[] = "beta";
    const std::array<SampleRecord, 2> kRecords = {
        {{1, 2, 1.5, tag0}, {3, 4, -2.5, tag1}}};

    ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, varid, "u_op", opaque_type, 2,
                                   kOpaque.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, varid, "u_en", enum_type,
                                   kEnum.size(), kEnum.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, varid, "u_vl", vlen_type,
                                   kVlens.size(), kVlens.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att(subgrp, varid, "u_cp", compound_type,
                                   kRecords.size(), kRecords.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open_mem("dattget_nc4.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reopened.ptr()));

  // Verify typed getters into sentinel-prefilled buffers.
  std::string got_t(5, 'X');
  std::vector<signed char> got_sc(3, 1);
  std::vector<unsigned char> got_uc(3, 1);
  std::vector<unsigned char> got_ub(3, 1);
  std::vector<short> got_sh(3, -1);
  std::vector<unsigned short> got_us(3, 1);
  std::vector<int> got_in(3, -999);
  std::vector<unsigned int> got_ui(3, 999);
  std::vector<long> got_ln(2, -999L);
  std::vector<long long> got_ll(2, -999LL);
  std::vector<unsigned long long> got_ul(2, 999ULL);
  std::vector<float> got_fl(3, -999.0F);
  std::vector<double> got_db(3, -999.0);
  ScopedNcStrings got_st(3);

  ASSERT_EQ(NC_NOERR,
            nc_get_att_text(reopened.get(), NC_GLOBAL, "t", got_t.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_schar(reopened.get(), NC_GLOBAL, "sc", got_sc.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_uchar(reopened.get(), NC_GLOBAL, "uc", got_uc.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_ubyte(reopened.get(), NC_GLOBAL, "ub", got_ub.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_short(reopened.get(), NC_GLOBAL, "sh", got_sh.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_ushort(reopened.get(), NC_GLOBAL, "us", got_us.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(reopened.get(), NC_GLOBAL, "in", got_in.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_uint(reopened.get(), NC_GLOBAL, "ui", got_ui.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_long(reopened.get(), NC_GLOBAL, "ln", got_ln.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_longlong(reopened.get(), NC_GLOBAL, "ll",
                                          got_ll.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_ulonglong(reopened.get(), NC_GLOBAL, "ul",
                                           got_ul.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_float(reopened.get(), NC_GLOBAL, "fl", got_fl.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_double(reopened.get(), NC_GLOBAL, "db", got_db.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_string(reopened.get(), NC_GLOBAL, "st", got_st.data()));

  EXPECT_EQ("hello", got_t);
  EXPECT_THAT(got_sc, ::testing::ElementsAre(-128, 0, 127));
  EXPECT_THAT(got_uc, ::testing::ElementsAre(0, 128, 255));
  EXPECT_THAT(got_ub, ::testing::ElementsAre(1, 200, 254));
  EXPECT_THAT(got_sh, ::testing::ElementsAre(-32768, 7, 32767));
  EXPECT_THAT(got_us, ::testing::ElementsAre(0, 40000, 65535));
  EXPECT_THAT(got_in, ::testing::ElementsAre(-2000000, 0, 2000000));
  EXPECT_THAT(got_ui, ::testing::ElementsAre(0U, 3000000000U, 4294967295U));
  EXPECT_THAT(got_ln, ::testing::ElementsAre(kLongA, kLongB));
  EXPECT_THAT(got_ll, ::testing::ElementsAre(-9000000000000000000LL,
                                             9000000000000000000LL));
  EXPECT_THAT(got_ul, ::testing::ElementsAre(0ULL, 18446744073709551615ULL));
  EXPECT_THAT(got_fl, ::testing::ElementsAre(-1.5F, 0.0F, 3.25F));
  EXPECT_THAT(got_db, ::testing::ElementsAre(-25.5, 0.0, 128.125));
  EXPECT_STREQ("first", got_st[0]);
  EXPECT_STREQ("", got_st[1]);
  EXPECT_STREQ("third", got_st[2]);

  // Verify generic nc_get_att works on atomic and NC_STRING attributes too.
  std::vector<int> got_in_generic(3, -999);
  ScopedNcStrings got_st_generic(3);
  ASSERT_EQ(NC_NOERR,
            nc_get_att(reopened.get(), NC_GLOBAL, "in", got_in_generic.data()));
  ASSERT_EQ(NC_NOERR,
            nc_get_att(reopened.get(), NC_GLOBAL, "st", got_st_generic.data()));
  EXPECT_THAT(got_in_generic, ::testing::ElementsAre(-2000000, 0, 2000000));
  EXPECT_STREQ("first", got_st_generic[0]);
  EXPECT_STREQ("", got_st_generic[1]);
  EXPECT_STREQ("third", got_st_generic[2]);

  // Zero-length attribute reads leave sentinels untouched and accept nullptr.
  int zero_sentinel = 12345;
  EXPECT_EQ(NC_NOERR, nc_get_att_int(reopened.get(), NC_GLOBAL, "zero_int",
                                     &zero_sentinel));
  EXPECT_EQ(12345, zero_sentinel);
  EXPECT_EQ(NC_NOERR,
            nc_get_att(reopened.get(), NC_GLOBAL, "zero_int", &zero_sentinel));
  EXPECT_EQ(12345, zero_sentinel);
  EXPECT_EQ(NC_NOERR,
            nc_get_att_int(reopened.get(), NC_GLOBAL, "zero_int", nullptr));
  EXPECT_EQ(NC_NOERR,
            nc_get_att_string(reopened.get(), NC_GLOBAL, "zero_str", nullptr));

  // Verify subgroup global, NC_FillValue, and user-defined attributes.
  int subgrp = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(reopened.get(), "sensor", &subgrp));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(subgrp, "reading", &varid));

  std::string got_lbl(3, 'Z');
  int got_fill = 0;
  std::vector<unsigned char> got_opaque(6, 0xAA);
  std::vector<int> got_enum(2, -1);
  ASSERT_EQ(NC_NOERR,
            nc_get_att_text(subgrp, NC_GLOBAL, "grp_lbl", got_lbl.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att_int(subgrp, varid, NC_FillValue, &got_fill));
  ASSERT_EQ(NC_NOERR, nc_get_att(subgrp, varid, "u_op", got_opaque.data()));
  ASSERT_EQ(NC_NOERR, nc_get_att(subgrp, varid, "u_en", got_enum.data()));
  EXPECT_EQ("lab", got_lbl);
  EXPECT_EQ(-555, got_fill);
  EXPECT_THAT(got_opaque, ::testing::ElementsAre(10, 20, 30, 40, 50, 60));
  EXPECT_THAT(got_enum, ::testing::ElementsAre(0, 1));

  ScopedNcVlens got_vlens(2);
  ASSERT_EQ(NC_NOERR, nc_get_att(subgrp, varid, "u_vl", got_vlens.data()));
  ASSERT_EQ(std::size_t{1}, got_vlens[0].len);
  ASSERT_EQ(std::size_t{3}, got_vlens[1].len);
  EXPECT_EQ(9, static_cast<const int*>(got_vlens[0].p)[0]);
  const int* v1_out = static_cast<const int*>(got_vlens[1].p);
  EXPECT_EQ(4, v1_out[0]);
  EXPECT_EQ(5, v1_out[1]);
  EXPECT_EQ(6, v1_out[2]);

  nc_type compound_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(subgrp, "rec_t", &compound_type));
  ScopedReclaimedArray<SampleRecord, 2> got_records(reopened.get(),
                                                    compound_type);
  ASSERT_EQ(NC_NOERR, nc_get_att(subgrp, varid, "u_cp", got_records.data()));
  EXPECT_EQ(1, got_records[0].seq);
  EXPECT_EQ(2, got_records[0].flags);
  EXPECT_DOUBLE_EQ(1.5, got_records[0].score);
  EXPECT_STREQ("alpha", got_records[0].tag);
  EXPECT_EQ(3, got_records[1].seq);
  EXPECT_EQ(4, got_records[1].flags);
  EXPECT_DOUBLE_EQ(-2.5, got_records[1].score);
  EXPECT_STREQ("beta", got_records[1].tag);
}

TEST(DattgetTest, ReadsCdf5ExtendedAndClassicFormats) {
  {
    ScopedMemio memio;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("dattget_cdf5.nc", NC_CDF5, 4096, nc.ptr()));

      constexpr unsigned char kUb = 251;
      constexpr unsigned short kUs = 60000;
      constexpr unsigned int kUi = 3900000000U;
      constexpr long long kLl = -7000000000000000000LL;
      constexpr unsigned long long kUl = 17000000000000000000ULL;
      ASSERT_EQ(NC_NOERR,
                nc_put_att_ubyte(nc.get(), NC_GLOBAL, "ub", NC_UBYTE, 1, &kUb));
      ASSERT_EQ(NC_NOERR, nc_put_att_ushort(nc.get(), NC_GLOBAL, "us",
                                            NC_USHORT, 1, &kUs));
      ASSERT_EQ(NC_NOERR,
                nc_put_att_uint(nc.get(), NC_GLOBAL, "ui", NC_UINT, 1, &kUi));
      ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc.get(), NC_GLOBAL, "ll",
                                              NC_INT64, 1, &kLl));
      ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "ul",
                                               NC_UINT64, 1, &kUl));
      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
    }

    ScopedNcFile reopened;
    ASSERT_EQ(NC_NOERR, nc_open_mem("dattget_cdf5.nc", NC_NOWRITE, memio.size(),
                                    memio.get()->memory, reopened.ptr()));

    unsigned char got_ub = 0;
    unsigned short got_us = 0;
    unsigned int got_ui = 0;
    long long got_ll = 0;
    unsigned long long got_ul = 0;
    unsigned long long got_gen_ul = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_ubyte(reopened.get(), NC_GLOBAL, "ub", &got_ub));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_ushort(reopened.get(), NC_GLOBAL, "us", &got_us));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_uint(reopened.get(), NC_GLOBAL, "ui", &got_ui));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_longlong(reopened.get(), NC_GLOBAL, "ll", &got_ll));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_ulonglong(reopened.get(), NC_GLOBAL, "ul", &got_ul));
    ASSERT_EQ(NC_NOERR,
              nc_get_att(reopened.get(), NC_GLOBAL, "ul", &got_gen_ul));
    EXPECT_EQ(251, got_ub);
    EXPECT_EQ(60000, got_us);
    EXPECT_EQ(3900000000U, got_ui);
    EXPECT_EQ(-7000000000000000000LL, got_ll);
    EXPECT_EQ(17000000000000000000ULL, got_ul);
    EXPECT_EQ(17000000000000000000ULL, got_gen_ul);
  }

  // Classic CDF-1 and 64-bit offset round-trip.
  for (const int cmode : {NC_DISKLESS, NC_64BIT_OFFSET | NC_DISKLESS}) {
    SCOPED_TRACE(cmode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattget_classic.nc", cmode, nc.ptr()));

    constexpr std::array<signed char, 2> kB = {-10, 20};
    constexpr std::array<short, 2> kS = {-300, 400};
    constexpr std::array<int, 2> kI = {-50000, 60000};
    constexpr std::array<float, 2> kF = {-0.5F, 2.25F};
    constexpr std::array<double, 2> kD = {-3.5, 4.75};
    constexpr char kT[] = "cls";
    ASSERT_EQ(NC_NOERR, nc_put_att_schar(nc.get(), NC_GLOBAL, "b", NC_BYTE,
                                         kB.size(), kB.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_short(nc.get(), NC_GLOBAL, "s", NC_SHORT,
                                         kS.size(), kS.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "i", NC_INT,
                                       kI.size(), kI.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_float(nc.get(), NC_GLOBAL, "f", NC_FLOAT,
                                         kF.size(), kF.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_double(nc.get(), NC_GLOBAL, "d", NC_DOUBLE,
                                          kD.size(), kD.data()));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(nc.get(), NC_GLOBAL, "t", sizeof(kT) - 1, kT));

    std::vector<signed char> gb(2);
    std::vector<short> gs(2);
    std::vector<int> gi(2);
    std::vector<long> gl(2);
    std::vector<float> gf(2);
    std::vector<double> gd(2);
    std::string gt(3, '\0');
    ASSERT_EQ(NC_NOERR, nc_get_att(nc.get(), NC_GLOBAL, "b", gb.data()));
    ASSERT_EQ(NC_NOERR, nc_get_att_short(nc.get(), NC_GLOBAL, "s", gs.data()));
    ASSERT_EQ(NC_NOERR, nc_get_att_int(nc.get(), NC_GLOBAL, "i", gi.data()));
    ASSERT_EQ(NC_NOERR, nc_get_att_long(nc.get(), NC_GLOBAL, "i", gl.data()));
    ASSERT_EQ(NC_NOERR, nc_get_att_float(nc.get(), NC_GLOBAL, "f", gf.data()));
    ASSERT_EQ(NC_NOERR, nc_get_att_double(nc.get(), NC_GLOBAL, "d", gd.data()));
    ASSERT_EQ(NC_NOERR, nc_get_att_text(nc.get(), NC_GLOBAL, "t", gt.data()));

    EXPECT_THAT(gb, ::testing::ElementsAre(-10, 20));
    EXPECT_THAT(gs, ::testing::ElementsAre(-300, 400));
    EXPECT_THAT(gi, ::testing::ElementsAre(-50000, 60000));
    EXPECT_THAT(gl, ::testing::ElementsAre(-50000L, 60000L));
    EXPECT_THAT(gf, ::testing::ElementsAre(-0.5F, 2.25F));
    EXPECT_THAT(gd, ::testing::ElementsAre(-3.5, 4.75));
    EXPECT_EQ("cls", gt);
  }
}

TEST(DattgetTest, ConvertsNumericTypesOnReadAndChecksRangeErrors) {
  struct FormatCase {
    const char* label;
    int cmode;
    bool unsigned_byte_strict;
  };
  constexpr FormatCase kCases[] = {
      {"classic", NC_DISKLESS, false},
      {"offset64", NC_64BIT_OFFSET | NC_DISKLESS, false},
      {"cdf5", NC_CDF5 | NC_DISKLESS, true},
      {"nc4_classic", NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS, false},
      {"netcdf4", NC_NETCDF4 | NC_DISKLESS, true},
  };

  for (const FormatCase& tc : kCases) {
    SCOPED_TRACE(tc.label);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattget_conv.nc", tc.cmode, nc.ptr()));

    constexpr std::array<signed char, 2> kSignedByte = {-5, 127};
    constexpr std::array<int, 2> kInts = {-5, 40000};
    constexpr int kThousand = 1000;
    constexpr double kHugeDbl = 1e12;
    constexpr std::array<double, 2> kOverIntDbl = {-2147483648.5, 2147483647.5};
    constexpr float kOverShortFlt = 32767.5F;
    constexpr std::array<float, 2> kFloats = {-2.5F, 8.25F};

    ASSERT_EQ(NC_NOERR,
              nc_put_att_schar(nc.get(), NC_GLOBAL, "sbyte", NC_BYTE,
                               kSignedByte.size(), kSignedByte.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "ints", NC_INT,
                                       kInts.size(), kInts.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "k1000", NC_INT, 1,
                                       &kThousand));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_double(nc.get(), NC_GLOBAL, "over_int_d", NC_DOUBLE,
                                kOverIntDbl.size(), kOverIntDbl.data()));
    ASSERT_EQ(NC_NOERR, nc_put_att_float(nc.get(), NC_GLOBAL, "over_short_f",
                                         NC_FLOAT, 1, &kOverShortFlt));
    if (!(tc.cmode & NC_NETCDF4)) {
      ASSERT_EQ(NC_NOERR, nc_put_att_double(nc.get(), NC_GLOBAL, "huge_d",
                                            NC_DOUBLE, 1, &kHugeDbl));
    }
    ASSERT_EQ(NC_NOERR, nc_put_att_float(nc.get(), NC_GLOBAL, "flts", NC_FLOAT,
                                         kFloats.size(), kFloats.data()));

    // Compatible widening conversions across getters.
    std::vector<short> b_as_short(2);
    std::vector<int> b_as_int(2);
    std::vector<long> b_as_long(2);
    std::vector<long long> b_as_ll(2);
    std::vector<float> b_as_flt(2);
    std::vector<double> f_as_dbl(2);
    ASSERT_EQ(NC_NOERR, nc_get_att_short(nc.get(), NC_GLOBAL, "sbyte",
                                         b_as_short.data()));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(nc.get(), NC_GLOBAL, "sbyte", b_as_int.data()));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_long(nc.get(), NC_GLOBAL, "sbyte", b_as_long.data()));
    ASSERT_EQ(NC_NOERR, nc_get_att_longlong(nc.get(), NC_GLOBAL, "sbyte",
                                            b_as_ll.data()));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_float(nc.get(), NC_GLOBAL, "sbyte", b_as_flt.data()));
    ASSERT_EQ(NC_NOERR,
              nc_get_att_double(nc.get(), NC_GLOBAL, "flts", f_as_dbl.data()));
    EXPECT_THAT(b_as_short, ::testing::ElementsAre(-5, 127));
    EXPECT_THAT(b_as_int, ::testing::ElementsAre(-5, 127));
    EXPECT_THAT(b_as_long, ::testing::ElementsAre(-5L, 127L));
    EXPECT_THAT(b_as_ll, ::testing::ElementsAre(-5LL, 127LL));
    EXPECT_THAT(b_as_flt, ::testing::ElementsAre(-5.0F, 127.0F));
    EXPECT_THAT(f_as_dbl, ::testing::ElementsAre(-2.5, 8.25));

    // Classic/64-bit/nc4_classic accept raw unsigned read of NC_BYTE (-5 ->
    // 251), whereas CDF-5 and netCDF-4 report NC_ERANGE.
    std::vector<unsigned char> b_as_ub(2, 0);
    std::vector<unsigned char> b_as_uc(2, 0);
    const int ub_stat =
        nc_get_att_ubyte(nc.get(), NC_GLOBAL, "sbyte", b_as_ub.data());
    const int uc_stat =
        nc_get_att_uchar(nc.get(), NC_GLOBAL, "sbyte", b_as_uc.data());
    if (tc.unsigned_byte_strict) {
      EXPECT_EQ(NC_ERANGE, ub_stat);
      EXPECT_EQ(NC_ERANGE, uc_stat);
    } else {
      EXPECT_EQ(NC_NOERR, ub_stat);
      EXPECT_EQ(NC_NOERR, uc_stat);
      EXPECT_THAT(b_as_ub, ::testing::ElementsAre(251, 127));
      EXPECT_THAT(b_as_uc, ::testing::ElementsAre(251, 127));
    }

    // Overflow and negative-to-unsigned NC_ERANGE checks.
    std::vector<short> tmp_s(2);
    std::vector<unsigned short> tmp_us(2);
    std::vector<unsigned int> tmp_ui(2);
    std::vector<unsigned long long> tmp_ull(2);
    std::vector<int> tmp_i_pair(2);
    signed char tmp_sc = 0;
    unsigned char tmp_uc = 0;
    unsigned char tmp_ub = 0;
    short tmp_sh = 0;
    int tmp_i = 0;

    EXPECT_EQ(NC_ERANGE,
              nc_get_att_short(nc.get(), NC_GLOBAL, "ints", tmp_s.data()));
    EXPECT_EQ(NC_ERANGE,
              nc_get_att_ushort(nc.get(), NC_GLOBAL, "ints", tmp_us.data()));
    EXPECT_EQ(NC_ERANGE,
              nc_get_att_uint(nc.get(), NC_GLOBAL, "ints", tmp_ui.data()));
    EXPECT_EQ(NC_ERANGE, nc_get_att_ulonglong(nc.get(), NC_GLOBAL, "ints",
                                              tmp_ull.data()));
    EXPECT_EQ(NC_ERANGE,
              nc_get_att_schar(nc.get(), NC_GLOBAL, "k1000", &tmp_sc));
    EXPECT_EQ(NC_ERANGE,
              nc_get_att_uchar(nc.get(), NC_GLOBAL, "k1000", &tmp_uc));
    EXPECT_EQ(NC_ERANGE,
              nc_get_att_ubyte(nc.get(), NC_GLOBAL, "k1000", &tmp_ub));
    // Fractional values just outside [INT_MIN, INT_MAX] / [SHRT_MIN, SHRT_MAX]
    // trigger NC_ERANGE while their truncated integer parts remain
    // representable, avoiding float-cast-overflow UB in netCDF-4
    // nc4_convert_type().
    EXPECT_EQ(NC_ERANGE, nc_get_att_int(nc.get(), NC_GLOBAL, "over_int_d",
                                        tmp_i_pair.data()));
    EXPECT_EQ(NC_ERANGE,
              nc_get_att_short(nc.get(), NC_GLOBAL, "over_short_f", &tmp_sh));
    if (!(tc.cmode & NC_NETCDF4)) {
      // Classic/64-bit/CDF-5 ncx_get_double_int() returns NC_ERANGE before
      // casting, whereas netCDF-4 nc4_convert_type() casts even on overflow.
      EXPECT_EQ(NC_ERANGE,
                nc_get_att_int(nc.get(), NC_GLOBAL, "huge_d", &tmp_i));
    }

    if (tc.cmode & (NC_CDF5 | NC_NETCDF4)) {
      if (!(tc.cmode & NC_CLASSIC_MODEL)) {
        constexpr unsigned long long kMaxU64 =
            std::numeric_limits<unsigned long long>::max();
        constexpr long long kBigI64 = 5000000000LL;
        ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "max_u64",
                                                 NC_UINT64, 1, &kMaxU64));
        ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc.get(), NC_GLOBAL, "big_i64",
                                                NC_INT64, 1, &kBigI64));
        long long tmp_ll = 0;
        EXPECT_EQ(NC_ERANGE,
                  nc_get_att_longlong(nc.get(), NC_GLOBAL, "max_u64", &tmp_ll));
        EXPECT_EQ(NC_ERANGE,
                  nc_get_att_int(nc.get(), NC_GLOBAL, "big_i64", &tmp_i));
      }
    }
  }
}

TEST(DattgetTest, RejectsInvalidConversionsMissingAttrsBadVarsAndBadNames) {
  for (const int cmode : {NC_DISKLESS, NC_NETCDF4 | NC_DISKLESS}) {
    SCOPED_TRACE(cmode);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("dattget_err.nc", cmode, nc.ptr()));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));

    constexpr char kText[] = "hi";
    constexpr std::array<int, 2> kNums = {1, 2};
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), varid, "txt", 2, kText));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc.get(), varid, "num", NC_INT, 2, kNums.data()));

    // NC_CHAR <-> numeric mismatches return NC_ECHAR.
    std::vector<signed char> sc(2);
    std::vector<unsigned char> uc(2);
    std::vector<unsigned char> ub(2);
    std::vector<short> sh(2);
    std::vector<unsigned short> us(2);
    std::vector<int> iv(2);
    std::vector<unsigned int> ui(2);
    std::vector<long> lv(2);
    std::vector<long long> ll(2);
    std::vector<unsigned long long> ul(2);
    std::vector<float> fl(2);
    std::vector<double> db(2);
    std::string txt_out(2, '\0');

    EXPECT_EQ(NC_ECHAR, nc_get_att_schar(nc.get(), varid, "txt", sc.data()));
    EXPECT_EQ(NC_ECHAR, nc_get_att_uchar(nc.get(), varid, "txt", uc.data()));
    EXPECT_EQ(NC_ECHAR, nc_get_att_ubyte(nc.get(), varid, "txt", ub.data()));
    EXPECT_EQ(NC_ECHAR, nc_get_att_short(nc.get(), varid, "txt", sh.data()));
    EXPECT_EQ(NC_ECHAR, nc_get_att_ushort(nc.get(), varid, "txt", us.data()));
    EXPECT_EQ(NC_ECHAR, nc_get_att_int(nc.get(), varid, "txt", iv.data()));
    EXPECT_EQ(NC_ECHAR, nc_get_att_uint(nc.get(), varid, "txt", ui.data()));
    EXPECT_EQ(NC_ECHAR, nc_get_att_long(nc.get(), varid, "txt", lv.data()));
    EXPECT_EQ(NC_ECHAR, nc_get_att_longlong(nc.get(), varid, "txt", ll.data()));
    EXPECT_EQ(NC_ECHAR,
              nc_get_att_ulonglong(nc.get(), varid, "txt", ul.data()));
    EXPECT_EQ(NC_ECHAR, nc_get_att_float(nc.get(), varid, "txt", fl.data()));
    EXPECT_EQ(NC_ECHAR, nc_get_att_double(nc.get(), varid, "txt", db.data()));
    EXPECT_EQ(NC_ECHAR,
              nc_get_att_text(nc.get(), varid, "num", txt_out.data()));

    ScopedNcStrings str_out(2);
    EXPECT_EQ(NC_ECHAR,
              nc_get_att_string(nc.get(), varid, "txt", str_out.data()));
    EXPECT_EQ(NC_EBADTYPE,
              nc_get_att_string(nc.get(), varid, "num", str_out.data()));

    if (cmode & NC_NETCDF4) {
      const char* str = "strval";
      ASSERT_EQ(NC_NOERR, nc_put_att_string(nc.get(), varid, "str", 1, &str));
      char text_from_str[8]{};
      int int_from_str = 0;
      EXPECT_EQ(NC_ECHAR,
                nc_get_att_text(nc.get(), varid, "str", text_from_str));
      EXPECT_EQ(NC_EBADTYPE,
                nc_get_att_int(nc.get(), varid, "str", &int_from_str));
    }

    // Missing attribute, invalid varid, and null name across typed getters.
    EXPECT_EQ(NC_ENOTATT,
              nc_get_att_int(nc.get(), varid, "missing", iv.data()));
    EXPECT_EQ(NC_ENOTATT,
              nc_get_att_text(nc.get(), NC_GLOBAL, "missing", txt_out.data()));
    EXPECT_EQ(NC_ENOTVAR, nc_get_att_int(nc.get(), 99, "num", iv.data()));
    EXPECT_EQ(NC_ENOTVAR, nc_get_att_int(nc.get(), -2, "num", iv.data()));
    EXPECT_EQ(NC_EBADNAME, nc_get_att_int(nc.get(), varid, nullptr, iv.data()));
    EXPECT_EQ(NC_EBADNAME,
              nc_get_att_double(nc.get(), varid, nullptr, db.data()));
  }
}

}  // namespace
}  // namespace netcdf
