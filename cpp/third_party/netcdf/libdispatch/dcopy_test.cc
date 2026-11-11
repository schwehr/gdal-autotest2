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

// Unit tests for //third_party/netcdf/libdispatch/dcopy.c.
//
// Covers nc_copy_var() across all 11 primitive types, scalars, 2D dimension-ID
// remapping, unlimited record copying (empty and non-empty), cross-format
// error rejections, and nc_copy_att() across atomic types, NC_STRING,
// NC_NETCDF4_CLASSIC attribute-order preservation, user-defined types (OPAQUE,
// ENUM, VLEN, nested COMPOUND), type-comparison mismatches, and hierarchical
// group-tree type search.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

// Upstream dcopy.c leaves `int target_attid` uninitialized when calling
// nc_inq_attid() for NC_FORMAT_NETCDF4_CLASSIC, and nc4_get_att_ptrs() passes
// `*attnum` by value into nc4_find_grp_att(), tripping MSan.
#if defined(MEMORY_SANITIZER)
#define NETCDF_DCOPY_TEST_MSAN 1
#elif defined(__has_feature)
#if __has_feature(memory_sanitizer)
#define NETCDF_DCOPY_TEST_MSAN 1
#endif
#endif

namespace netcdf {
namespace {

using ::testing::ElementsAre;

struct InnerPair {
  int32_t left;
  int32_t right;
};
static_assert(sizeof(InnerPair) == 8);

struct ComplexSample {
  int32_t id;
  InnerPair pair;
  int32_t grid[2][3];
  int32_t mode;
};
static_assert(sizeof(ComplexSample) == 40);

int CreateInMemoryDataset(const char* name, int mode, ScopedNcFile* out) {
  return nc_create_mem(name, mode, 16384, out->ptr());
}

TEST(DcopyTest, CopyVarHandlesAllPrimitiveTypesScalarsAndDimRemapping) {
  ScopedNcFile src;
  ScopedNcFile dst;
  ASSERT_EQ(NC_NOERR, CreateInMemoryDataset("dcopy_var_src.nc", NC_CDF5, &src));
  ASSERT_EQ(NC_NOERR, CreateInMemoryDataset("dcopy_var_dst.nc", NC_CDF5, &dst));

  // Define source dims as [rows, cols, rec] and dest dims in swapped order
  // [cols, rows, rec] so dimids differ between src and dst.
  int src_rows = -1;
  int src_cols = -1;
  int src_rec = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(src.get(), "rows", 2, &src_rows));
  ASSERT_EQ(NC_NOERR, nc_def_dim(src.get(), "cols", 3, &src_cols));
  ASSERT_EQ(NC_NOERR, nc_def_dim(src.get(), "rec", NC_UNLIMITED, &src_rec));

  int dst_cols = -1;
  int dst_rows = -1;
  int dst_rec = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(dst.get(), "cols", 3, &dst_cols));
  ASSERT_EQ(NC_NOERR, nc_def_dim(dst.get(), "rows", 2, &dst_rows));
  ASSERT_EQ(NC_NOERR, nc_def_dim(dst.get(), "rec", NC_UNLIMITED, &dst_rec));

  const int dim_2d[2] = {src_rows, src_cols};
  const int dim_rec[1] = {src_rec};
  int v_byte = -1;
  int v_char = -1;
  int v_short = -1;
  int v_int = -1;
  int v_float = -1;
  int v_double = -1;
  int v_ubyte = -1;
  int v_ushort = -1;
  int v_uint = -1;
  int v_int64 = -1;
  int v_uint64 = -1;
  int v_scalar = -1;
  int v_unlim = -1;

  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_byte", NC_BYTE, 2, dim_2d, &v_byte));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_char", NC_CHAR, 2, dim_2d, &v_char));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_short", NC_SHORT, 2, dim_2d, &v_short));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_int", NC_INT, 2, dim_2d, &v_int));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_float", NC_FLOAT, 2, dim_2d, &v_float));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_double", NC_DOUBLE, 2, dim_2d, &v_double));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_ubyte", NC_UBYTE, 2, dim_2d, &v_ubyte));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_ushort", NC_USHORT, 2, dim_2d, &v_ushort));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_uint", NC_UINT, 2, dim_2d, &v_uint));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_int64", NC_INT64, 2, dim_2d, &v_int64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_uint64", NC_UINT64, 2, dim_2d, &v_uint64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_scalar", NC_INT, 0, nullptr, &v_scalar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v_unlim", NC_DOUBLE, 1, dim_rec, &v_unlim));

  // Attach variable attributes so nc_copy_var() copies metadata too.
  constexpr char kUnits[] = "kelvin";
  constexpr double kScale = 0.5;
  ASSERT_EQ(NC_NOERR, nc_put_att_text(src.get(), v_int, "units",
                                      sizeof(kUnits) - 1, kUnits));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(src.get(), v_int, "scale_factor",
                                        NC_DOUBLE, 1, &kScale));
  ASSERT_EQ(NC_NOERR, nc_enddef(src.get()));

  constexpr std::array<signed char, 6> kByteIn{-120, -1, 0, 1, 42, 127};
  constexpr std::array<char, 6> kCharIn{'a', 'b', 'c', 'x', 'y', 'z'};
  constexpr std::array<short, 6> kShortIn{-30000, -2, 0, 7, 1024, 30000};
  constexpr std::array<int, 6> kIntIn{-100000, -1, 0, 42, 999, 1000000};
  constexpr std::array<float, 6> kFloatIn{-1.5F, 0.0F,  1.25F,
                                          2.5F,  3.75F, 4.5F};
  constexpr std::array<double, 6> kDoubleIn{-9.5, 0.0, 1.125, 2.25, 3.5, 8.75};
  constexpr std::array<unsigned char, 6> kUbyteIn{0, 1, 127, 128, 200, 254};
  constexpr std::array<unsigned short, 6> kUshortIn{0,     1,     500,
                                                    32768, 50000, 65534};
  constexpr std::array<unsigned int, 6> kUintIn{
      0U, 1U, 65536U, 100000U, 2200000000U, 4000000000U};
  constexpr std::array<long long, 6> kInt64In{
      -5000000000LL, -1LL, 0LL, 1LL, 42LL, 9000000000LL};
  constexpr std::array<unsigned long long, 6> kUint64In{
      0ULL, 1ULL, 100ULL, 5000000000ULL, 10000000000ULL, 18000000000ULL};
  constexpr int kScalarIn = 777;
  constexpr std::array<double, 3> kUnlimIn{10.5, 20.5, 30.5};

  ASSERT_EQ(NC_NOERR, nc_put_var_schar(src.get(), v_byte, kByteIn.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_text(src.get(), v_char, kCharIn.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_short(src.get(), v_short, kShortIn.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(src.get(), v_int, kIntIn.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(src.get(), v_float, kFloatIn.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(src.get(), v_double, kDoubleIn.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_uchar(src.get(), v_ubyte, kUbyteIn.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_ushort(src.get(), v_ushort, kUshortIn.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_uint(src.get(), v_uint, kUintIn.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_longlong(src.get(), v_int64, kInt64In.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_var_ulonglong(src.get(), v_uint64, kUint64In.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(src.get(), v_scalar, &kScalarIn));
  constexpr size_t kStart[1] = {0};
  constexpr size_t kCount[1] = {kUnlimIn.size()};
  ASSERT_EQ(NC_NOERR, nc_put_vara_double(src.get(), v_unlim, kStart, kCount,
                                         kUnlimIn.data()));

  // First copy enters while dst is in define mode (NC_EINDEFINE); subsequent
  // copies enter data mode and hit nc_redef() == NC_NOERR.
  for (const int varid :
       {v_byte, v_char, v_short, v_int, v_float, v_double, v_ubyte, v_ushort,
        v_uint, v_int64, v_uint64, v_scalar, v_unlim}) {
    ASSERT_EQ(NC_NOERR, nc_copy_var(src.get(), varid, dst.get()));
  }

  auto inq_dst_id = [&](const char* name) {
    int id = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_varid(dst.get(), name, &id)) << name;
    return id;
  };

  std::vector<signed char> byte_out(6, 0);
  ASSERT_EQ(NC_NOERR,
            nc_get_var_schar(dst.get(), inq_dst_id("v_byte"), byte_out.data()));
  EXPECT_EQ(std::vector<signed char>(kByteIn.begin(), kByteIn.end()), byte_out);

  std::vector<char> char_out(6, '\0');
  ASSERT_EQ(NC_NOERR,
            nc_get_var_text(dst.get(), inq_dst_id("v_char"), char_out.data()));
  EXPECT_EQ(std::vector<char>(kCharIn.begin(), kCharIn.end()), char_out);

  std::vector<short> short_out(6, 0);
  ASSERT_EQ(NC_NOERR, nc_get_var_short(dst.get(), inq_dst_id("v_short"),
                                       short_out.data()));
  EXPECT_EQ(std::vector<short>(kShortIn.begin(), kShortIn.end()), short_out);

  const int dst_vint = inq_dst_id("v_int");
  std::vector<int> int_out(6, 0);
  ASSERT_EQ(NC_NOERR, nc_get_var_int(dst.get(), dst_vint, int_out.data()));
  EXPECT_EQ(std::vector<int>(kIntIn.begin(), kIntIn.end()), int_out);

  // Verify remapped dimids and copied attributes on v_int.
  int dst_dimids[2]{-1, -1};
  ASSERT_EQ(NC_NOERR, nc_inq_vardimid(dst.get(), dst_vint, dst_dimids));
  EXPECT_EQ(dst_rows, dst_dimids[0]);
  EXPECT_EQ(dst_cols, dst_dimids[1]);
  char units_out[NC_MAX_NAME + 1]{};
  ASSERT_EQ(NC_NOERR, nc_get_att_text(dst.get(), dst_vint, "units", units_out));
  EXPECT_STREQ("kelvin", units_out);
  double scale_out = 0.0;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_double(dst.get(), dst_vint, "scale_factor", &scale_out));
  EXPECT_DOUBLE_EQ(0.5, scale_out);

  std::vector<float> float_out(6, 0.0F);
  ASSERT_EQ(NC_NOERR, nc_get_var_float(dst.get(), inq_dst_id("v_float"),
                                       float_out.data()));
  EXPECT_EQ(std::vector<float>(kFloatIn.begin(), kFloatIn.end()), float_out);

  std::vector<double> double_out(6, 0.0);
  ASSERT_EQ(NC_NOERR, nc_get_var_double(dst.get(), inq_dst_id("v_double"),
                                        double_out.data()));
  EXPECT_EQ(std::vector<double>(kDoubleIn.begin(), kDoubleIn.end()),
            double_out);

  std::vector<unsigned char> ubyte_out(6, 0);
  ASSERT_EQ(NC_NOERR, nc_get_var_uchar(dst.get(), inq_dst_id("v_ubyte"),
                                       ubyte_out.data()));
  EXPECT_EQ(std::vector<unsigned char>(kUbyteIn.begin(), kUbyteIn.end()),
            ubyte_out);

  std::vector<unsigned short> ushort_out(6, 0);
  ASSERT_EQ(NC_NOERR, nc_get_var_ushort(dst.get(), inq_dst_id("v_ushort"),
                                        ushort_out.data()));
  EXPECT_EQ(std::vector<unsigned short>(kUshortIn.begin(), kUshortIn.end()),
            ushort_out);

  std::vector<unsigned int> uint_out(6, 0U);
  ASSERT_EQ(NC_NOERR,
            nc_get_var_uint(dst.get(), inq_dst_id("v_uint"), uint_out.data()));
  EXPECT_EQ(std::vector<unsigned int>(kUintIn.begin(), kUintIn.end()),
            uint_out);

  std::vector<long long> int64_out(6, 0LL);
  ASSERT_EQ(NC_NOERR, nc_get_var_longlong(dst.get(), inq_dst_id("v_int64"),
                                          int64_out.data()));
  EXPECT_EQ(std::vector<long long>(kInt64In.begin(), kInt64In.end()),
            int64_out);

  std::vector<unsigned long long> uint64_out(6, 0ULL);
  ASSERT_EQ(NC_NOERR, nc_get_var_ulonglong(dst.get(), inq_dst_id("v_uint64"),
                                           uint64_out.data()));
  EXPECT_EQ(std::vector<unsigned long long>(kUint64In.begin(), kUint64In.end()),
            uint64_out);

  int scalar_out = 0;
  ASSERT_EQ(NC_NOERR,
            nc_get_var_int(dst.get(), inq_dst_id("v_scalar"), &scalar_out));
  EXPECT_EQ(kScalarIn, scalar_out);

  std::vector<double> unlim_out(3, 0.0);
  ASSERT_EQ(NC_NOERR, nc_get_var_double(dst.get(), inq_dst_id("v_unlim"),
                                        unlim_out.data()));
  EXPECT_EQ(std::vector<double>(kUnlimIn.begin(), kUnlimIn.end()), unlim_out);
}

TEST(DcopyTest, CopyVarHandlesZeroRecordUnlimitedVarAndCrossFormatCopy) {
  ScopedNcFile nc4_src;
  ScopedNcFile classic_dst;
  ASSERT_EQ(NC_NOERR,
            CreateInMemoryDataset("dcopy_nc4_empty.nc", NC_NETCDF4, &nc4_src));
  ASSERT_EQ(NC_NOERR,
            CreateInMemoryDataset("dcopy_classic_dst.nc", 0, &classic_dst));

  int src_rec = -1;
  int dst_rec = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc4_src.get(), "rec", NC_UNLIMITED, &src_rec));
  ASSERT_EQ(NC_NOERR,
            nc_def_dim(classic_dst.get(), "rec", NC_UNLIMITED, &dst_rec));

  int empty_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc4_src.get(), "empty_series", NC_FLOAT, 1,
                                 &src_rec, &empty_var));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc4_src.get()));

  // Source unlimited dimension has length 0: hits the early exit after def_var.
  ASSERT_EQ(NC_NOERR, nc_copy_var(nc4_src.get(), empty_var, classic_dst.get()));

  int dst_var = -1;
  size_t rec_len = 99;
  ASSERT_EQ(NC_NOERR,
            nc_inq_varid(classic_dst.get(), "empty_series", &dst_var));
  ASSERT_EQ(NC_NOERR, nc_inq_dimlen(classic_dst.get(), dst_rec, &rec_len));
  EXPECT_EQ(size_t{0}, rec_len);
}

TEST(DcopyTest,
     CopyVarRejectsInvalidIdsMissingDimsIncompatibleTypesAndReadOnly) {
  ScopedNcFile nc4_src;
  ASSERT_EQ(NC_NOERR,
            CreateInMemoryDataset("dcopy_err_src.nc", NC_NETCDF4, &nc4_src));

  int d_len = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc4_src.get(), "x", 2, &d_len));

  int v_int = -1;
  int v_i64 = -1;
  int v_str = -1;
  int v_with_opaque_att = -1;
  nc_type op_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc4_src.get(), 4, "op4", &op_type));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc4_src.get(), "v_int", NC_INT, 1, &d_len, &v_int));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc4_src.get(), "v_i64", NC_INT64, 1, &d_len, &v_i64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc4_src.get(), "v_str", NC_STRING, 1, &d_len, &v_str));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc4_src.get(), "v_att_fail", NC_INT, 1, &d_len,
                                 &v_with_opaque_att));

  constexpr std::array<uint8_t, 4> kOpVal{1, 2, 3, 4};
  ASSERT_EQ(NC_NOERR, nc_put_att(nc4_src.get(), v_with_opaque_att, "op_att",
                                 op_type, 1, kOpVal.data()));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc4_src.get()));
  constexpr std::array<int, 2> kInts{11, 22};
  constexpr std::array<long long, 2> kI64s{100LL, 200LL};
  std::array<const char*, 2> kStrs{"alpha", "beta"};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(nc4_src.get(), v_int, kInts.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_longlong(nc4_src.get(), v_i64, kI64s.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_string(nc4_src.get(), v_str, kStrs.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_var_int(nc4_src.get(), v_with_opaque_att, kInts.data()));
  ASSERT_EQ(NC_NOERR, nc_sync(nc4_src.get()));

  // 1. Bad ncid_in / varid_in / ncid_out.
  ScopedNcFile empty_dst;
  ASSERT_EQ(NC_NOERR, CreateInMemoryDataset("dcopy_empty_dst.nc", NC_NETCDF4,
                                            &empty_dst));
  EXPECT_EQ(NC_EBADID, nc_copy_var(-1, v_int, empty_dst.get()));
  EXPECT_EQ(NC_ENOTVAR, nc_copy_var(nc4_src.get(), 99, empty_dst.get()));
  EXPECT_EQ(NC_EBADDIM, nc_copy_var(nc4_src.get(), v_int, empty_dst.get()));

  int dst_dim = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(empty_dst.get(), "x", 2, &dst_dim));
  EXPECT_EQ(NC_EBADID, nc_copy_var(nc4_src.get(), v_int, -1));

  // 2. NC_STRING variable copy hits default NC_EBADTYPE branch in switch.
  EXPECT_EQ(NC_EBADTYPE, nc_copy_var(nc4_src.get(), v_str, empty_dst.get()));

  // 3. Copying var with unresolvable user-defined attribute fails in att loop.
  EXPECT_EQ(NC_EBADTYPE,
            nc_copy_var(nc4_src.get(), v_with_opaque_att, empty_dst.get()));

  // 4. Duplicate variable name in dst fails nc_def_var with NC_ENAMEINUSE.
  int dup_var = -1;
  ASSERT_EQ(NC_NOERR, nc_enddef(empty_dst.get()));
  ASSERT_EQ(NC_NOERR, nc_redef(empty_dst.get()));
  ASSERT_EQ(NC_NOERR, nc_def_var(empty_dst.get(), "v_int", NC_INT, 1, &dst_dim,
                                 &dup_var));
  EXPECT_EQ(NC_ENAMEINUSE, nc_copy_var(nc4_src.get(), v_int, empty_dst.get()));

  // 5. NetCDF-4 extended type (xtype > NC_DOUBLE) rejected by classic,
  // 64-bit offset, and CDF-5 destinations.
  for (const int classic_mode : {0, NC_64BIT_OFFSET, NC_CDF5}) {
    SCOPED_TRACE(classic_mode);
    ScopedNcFile dst_classic;
    ASSERT_EQ(NC_NOERR, CreateInMemoryDataset("dcopy_reject_ext.nc",
                                              classic_mode, &dst_classic));
    int cdim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(dst_classic.get(), "x", 2, &cdim));
    EXPECT_EQ(NC_ENOTNC4, nc_copy_var(nc4_src.get(), v_i64, dst_classic.get()));
  }

  // 6. Read-only destination fails nc_redef with NC_EPERM.
  ScopedMemio ro_mem;
  {
    ScopedNcFile rw_tmp;
    ASSERT_EQ(NC_NOERR,
              CreateInMemoryDataset("dcopy_ro_tmp.nc", NC_NETCDF4, &rw_tmp));
    int d = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(rw_tmp.get(), "x", 2, &d));
    ASSERT_EQ(NC_NOERR, nc_close_memio(rw_tmp.release(), ro_mem.get()));
  }
  ScopedNcFile ro_dst;
  ASSERT_EQ(NC_NOERR, nc_open_mem("dcopy_ro_dst.nc", NC_NOWRITE, ro_mem.size(),
                                  ro_mem.get()->memory, ro_dst.ptr()));
  EXPECT_EQ(NC_EPERM, nc_copy_var(nc4_src.get(), v_int, ro_dst.get()));
}

TEST(DcopyTest, CopyAttPreservesOrderInNetcdf4ClassicAndHandlesAtomicCases) {
  ScopedNcFile src;
  ASSERT_EQ(NC_NOERR,
            CreateInMemoryDataset("dcopy_att_src.nc", NC_NETCDF4, &src));

  int src_dim = -1;
  int src_v0 = -1;
  int src_v1 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(src.get(), "x", 1, &src_dim));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v0", NC_INT, 1, &src_dim, &src_v0));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(src.get(), "v1", NC_INT, 1, &src_dim, &src_v1));

  constexpr std::array<int, 2> kUpdatedA1{101, 202};
  constexpr int kValA3 = 303;
  constexpr int kValLast = 404;
  std::array<const char*, 2> kStringAtt{"hello", "world"};
  ASSERT_EQ(NC_NOERR, nc_put_att_int(src.get(), src_v0, "a1", NC_INT, 2,
                                     kUpdatedA1.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(src.get(), src_v0, "a3", NC_INT, 1, &kValA3));
  ASSERT_EQ(NC_NOERR, nc_put_att_int(src.get(), NC_GLOBAL, "g_int", NC_INT, 1,
                                     &kValLast));
  ASSERT_EQ(NC_NOERR, nc_put_att_string(src.get(), src_v0, "s_att",
                                        kStringAtt.size(), kStringAtt.data()));

  // Same file, same var -> immediate NC_NOERR no-op.
  EXPECT_EQ(NC_NOERR, nc_copy_att(src.get(), src_v0, "a1", src.get(), src_v0));
  // Same file, different var and global -> var.
  ASSERT_EQ(NC_NOERR,
            nc_copy_att(src.get(), NC_GLOBAL, "g_int", src.get(), src_v1));
  ASSERT_EQ(NC_NOERR,
            nc_copy_att(src.get(), src_v0, "s_att", src.get(), src_v1));

#if !defined(NETCDF_DCOPY_TEST_MSAN)
  // NC_NETCDF4_CLASSIC destination preserves attribute index order.
  ScopedMemio nc4c_mem;
  {
    ScopedNcFile dst;
    ASSERT_EQ(NC_NOERR,
              CreateInMemoryDataset("dcopy_att_nc4c.nc",
                                    NC_NETCDF4 | NC_CLASSIC_MODEL, &dst));
    int dst_dim = -1;
    int dst_v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(dst.get(), "x", 1, &dst_dim));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(dst.get(), "v", NC_INT, 1, &dst_dim, &dst_v));

    constexpr int kOrig0 = 0;
    constexpr int kOrig1 = 1;
    constexpr int kOrig2 = 2;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(dst.get(), dst_v, "a0", NC_INT, 1, &kOrig0));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(dst.get(), dst_v, "a1", NC_INT, 1, &kOrig1));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(dst.get(), dst_v, "a2", NC_INT, 1, &kOrig2));

    // New attribute (NC_ENOTATT in target).
    ASSERT_EQ(NC_NOERR, nc_copy_att(src.get(), src_v0, "a3", dst.get(), dst_v));
    // Overwrite last attribute (target_attid == target_natts - 1).
    ASSERT_EQ(NC_NOERR, nc_copy_att(src.get(), src_v0, "a3", dst.get(), dst_v));
    // Overwrite middle attribute "a1" -> exercises full order-preservation
    // loop.
    ASSERT_EQ(NC_NOERR, nc_copy_att(src.get(), src_v0, "a1", dst.get(), dst_v));

    // Invalid varid_out in NC_NETCDF4_CLASSIC exercises non-NC_ENOTATT return.
    EXPECT_EQ(NC_ENOTVAR, nc_copy_att(src.get(), src_v0, "a3", dst.get(), 99));

    for (int i = 0; i < 4; ++i) {
      char att_name[NC_MAX_NAME + 1]{};
      ASSERT_EQ(NC_NOERR, nc_inq_attname(dst.get(), dst_v, i, att_name));
      EXPECT_EQ(absl::StrFormat("a%d", i), att_name);
    }
    std::vector<int> a1_out(2, -1);
    ASSERT_EQ(NC_NOERR, nc_get_att_int(dst.get(), dst_v, "a1", a1_out.data()));
    EXPECT_THAT(a1_out, ElementsAre(101, 202));

    ASSERT_EQ(NC_NOERR, nc_enddef(dst.get()));
    ASSERT_EQ(NC_NOERR, nc_close_memio(dst.release(), nc4c_mem.get()));
  }

  // Read-only NC_NETCDF4_CLASSIC fails in both loop branches (index 0 and 1).
  ScopedNcFile ro_nc4c;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("dcopy_nc4c_ro.nc", NC_NOWRITE, nc4c_mem.size(),
                        nc4c_mem.get()->memory, ro_nc4c.ptr()));
  int ro_v = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc4c.get(), "v", &ro_v));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(src.get(), src_v0, "a0", NC_INT, 1, &kValA3));
  EXPECT_EQ(NC_EPERM,
            nc_copy_att(src.get(), src_v0, "a0", ro_nc4c.get(), ro_v));
  EXPECT_EQ(NC_EPERM,
            nc_copy_att(src.get(), src_v0, "a1", ro_nc4c.get(), ro_v));
#endif  // !NETCDF_DCOPY_TEST_MSAN

  // NC_STRING copy to another NC_NETCDF4 file succeeds and reclaims cleanly;
  // copy to Classic fails put_att while still reclaiming NC_STRING heap data.
  ScopedNcFile nc4_dst;
  ASSERT_EQ(NC_NOERR,
            CreateInMemoryDataset("dcopy_str_dst.nc", NC_NETCDF4, &nc4_dst));
  ASSERT_EQ(NC_NOERR,
            nc_copy_att(src.get(), src_v0, "s_att", nc4_dst.get(), NC_GLOBAL));
  ScopedNcStrings str_out(2);
  ASSERT_EQ(NC_NOERR, nc_get_att_string(nc4_dst.get(), NC_GLOBAL, "s_att",
                                        str_out.data()));
  EXPECT_STREQ("hello", str_out[0]);
  EXPECT_STREQ("world", str_out[1]);

  ScopedNcFile classic_dst;
  ASSERT_EQ(NC_NOERR,
            CreateInMemoryDataset("dcopy_str_classic.nc", 0, &classic_dst));
  EXPECT_EQ(NC_EBADTYPE, nc_copy_att(src.get(), src_v0, "s_att",
                                     classic_dst.get(), NC_GLOBAL));

  // General error paths for nc_copy_att().
  EXPECT_EQ(NC_EBADID, nc_copy_att(src.get(), src_v0, "a1", -1, NC_GLOBAL));
  EXPECT_EQ(NC_EBADID, nc_copy_att(-1, src_v0, "a1", nc4_dst.get(), NC_GLOBAL));
  EXPECT_EQ(NC_ENOTATT, nc_copy_att(src.get(), src_v0, "no_such_att",
                                    nc4_dst.get(), NC_GLOBAL));
}

TEST(DcopyTest, CopyAttResolvesUserTypesAcrossGroupHierarchy) {
  ScopedNcFile src;
  ScopedNcFile dst;
  ASSERT_EQ(NC_NOERR,
            CreateInMemoryDataset("dcopy_utype_src.nc", NC_NETCDF4, &src));
  ASSERT_EQ(NC_NOERR,
            CreateInMemoryDataset("dcopy_utype_dst.nc", NC_NETCDF4, &dst));

  // Build user-defined types in src (at root).
  nc_type src_enum = NC_NAT;
  nc_type src_opaque = NC_NAT;
  nc_type src_vlen_int = NC_NAT;
  nc_type src_inner = NC_NAT;
  nc_type src_complex = NC_NAT;

  ASSERT_EQ(NC_NOERR, nc_def_enum(src.get(), NC_INT, "status_e", &src_enum));
  constexpr int32_t kCold = 0;
  constexpr int32_t kWarm = 1;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(src.get(), src_enum, "COLD", &kCold));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(src.get(), src_enum, "WARM", &kWarm));

  ASSERT_EQ(NC_NOERR, nc_def_opaque(src.get(), 6, "tag_opaque", &src_opaque));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(src.get(), "vlen_i", NC_INT, &src_vlen_int));

  ASSERT_EQ(NC_NOERR, nc_def_compound(src.get(), sizeof(InnerPair), "inner_c",
                                      &src_inner));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(src.get(), src_inner, "left",
                                         offsetof(InnerPair, left), NC_INT));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(src.get(), src_inner, "right",
                                         offsetof(InnerPair, right), NC_INT));

  ASSERT_EQ(NC_NOERR, nc_def_compound(src.get(), sizeof(ComplexSample),
                                      "complex_c", &src_complex));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(src.get(), src_complex, "id",
                                         offsetof(ComplexSample, id), NC_INT));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(src.get(), src_complex, "pair",
                               offsetof(ComplexSample, pair), src_inner));
  constexpr int kGridDims[2] = {2, 3};
  ASSERT_EQ(NC_NOERR, nc_insert_array_compound(src.get(), src_complex, "grid",
                                               offsetof(ComplexSample, grid),
                                               NC_INT, 2, kGridDims));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(src.get(), src_complex, "mode",
                               offsetof(ComplexSample, mode), src_enum));

  // Place matching types in different levels of dst's group hierarchy:
  // - dst_target gets tag_opaque directly (hits local searchgroup)
  // - dst (root) gets status_e, vlen_i (hits parent walk-up)
  // - dst/branch/deep gets inner_c and complex_c (hits root BFS
  // searchgrouptree)
  int dst_target = -1;
  int dst_branch = -1;
  int dst_deep = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(dst.get(), "target", &dst_target));
  ASSERT_EQ(NC_NOERR, nc_def_grp(dst.get(), "branch", &dst_branch));
  ASSERT_EQ(NC_NOERR, nc_def_grp(dst_branch, "deep", &dst_deep));

  nc_type dst_enum = NC_NAT;
  nc_type dst_opaque = NC_NAT;
  nc_type dst_vlen_int = NC_NAT;
  nc_type dst_inner = NC_NAT;
  nc_type dst_complex = NC_NAT;

  ASSERT_EQ(NC_NOERR, nc_def_enum(dst.get(), NC_INT, "status_e", &dst_enum));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(dst.get(), dst_enum, "COLD", &kCold));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(dst.get(), dst_enum, "WARM", &kWarm));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(dst.get(), "vlen_i", NC_INT, &dst_vlen_int));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(dst_target, 6, "tag_opaque", &dst_opaque));

  ASSERT_EQ(NC_NOERR, nc_def_compound(dst_deep, sizeof(InnerPair), "inner_c",
                                      &dst_inner));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(dst_deep, dst_inner, "left",
                                         offsetof(InnerPair, left), NC_INT));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(dst_deep, dst_inner, "right",
                                         offsetof(InnerPair, right), NC_INT));
  ASSERT_EQ(NC_NOERR, nc_def_compound(dst_deep, sizeof(ComplexSample),
                                      "complex_c", &dst_complex));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(dst_deep, dst_complex, "id",
                                         offsetof(ComplexSample, id), NC_INT));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(dst_deep, dst_complex, "pair",
                               offsetof(ComplexSample, pair), dst_inner));
  ASSERT_EQ(NC_NOERR, nc_insert_array_compound(dst_deep, dst_complex, "grid",
                                               offsetof(ComplexSample, grid),
                                               NC_INT, 2, kGridDims));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(dst_deep, dst_complex, "mode",
                               offsetof(ComplexSample, mode), dst_enum));

  // Populate attributes on src.
  constexpr std::array<uint8_t, 6> kOpaqueVal{1, 2, 3, 4, 5, 6};
  constexpr int32_t kEnumVal = kWarm;
  std::array<int32_t, 3> vlen_i_elems{7, 8, 9};
  nc_vlen_t vlen_i_val{vlen_i_elems.size(), vlen_i_elems.data()};
  constexpr ComplexSample kComplexVal{
      42, {10, 20}, {{1, 2, 3}, {4, 5, 6}}, kWarm};

  ASSERT_EQ(NC_NOERR, nc_put_att(src.get(), NC_GLOBAL, "att_opaque", src_opaque,
                                 1, kOpaqueVal.data()));
  ASSERT_EQ(NC_NOERR, nc_put_att(src.get(), NC_GLOBAL, "att_enum", src_enum, 1,
                                 &kEnumVal));
  ASSERT_EQ(NC_NOERR, nc_put_att(src.get(), NC_GLOBAL, "att_vlen_i",
                                 src_vlen_int, 1, &vlen_i_val));
  ASSERT_EQ(NC_NOERR, nc_put_att(src.get(), NC_GLOBAL, "att_complex",
                                 src_complex, 1, &kComplexVal));
  ASSERT_EQ(NC_NOERR, nc_enddef(src.get()));
  ASSERT_EQ(NC_NOERR, nc_enddef(dst.get()));

  ASSERT_EQ(NC_NOERR, nc_copy_att(src.get(), NC_GLOBAL, "att_opaque",
                                  dst_target, NC_GLOBAL));
  ASSERT_EQ(NC_NOERR, nc_copy_att(src.get(), NC_GLOBAL, "att_enum", dst_target,
                                  NC_GLOBAL));
  ASSERT_EQ(NC_NOERR, nc_copy_att(src.get(), NC_GLOBAL, "att_vlen_i",
                                  dst_target, NC_GLOBAL));
  ASSERT_EQ(NC_NOERR, nc_copy_att(src.get(), NC_GLOBAL, "att_complex",
                                  dst_target, NC_GLOBAL));

  std::array<uint8_t, 6> opaque_out{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att(dst_target, NC_GLOBAL, "att_opaque", opaque_out.data()));
  EXPECT_EQ(kOpaqueVal, opaque_out);

  int32_t enum_out = -1;
  ASSERT_EQ(NC_NOERR, nc_get_att(dst_target, NC_GLOBAL, "att_enum", &enum_out));
  EXPECT_EQ(kWarm, enum_out);

  ScopedNcVlens vlen_i_out(1);
  ASSERT_EQ(NC_NOERR,
            nc_get_att(dst_target, NC_GLOBAL, "att_vlen_i", vlen_i_out.data()));
  ASSERT_EQ(size_t{3}, vlen_i_out[0].len);
  const int32_t* vptr = static_cast<const int32_t*>(vlen_i_out[0].p);
  EXPECT_THAT(std::vector<int32_t>(vptr, vptr + 3), ElementsAre(7, 8, 9));

  ComplexSample complex_out{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att(dst_target, NC_GLOBAL, "att_complex", &complex_out));
  EXPECT_EQ(kComplexVal.id, complex_out.id);
  EXPECT_EQ(kComplexVal.pair.left, complex_out.pair.left);
  EXPECT_EQ(kComplexVal.pair.right, complex_out.pair.right);
  EXPECT_EQ(kComplexVal.grid[1][2], complex_out.grid[1][2]);
  EXPECT_EQ(kComplexVal.mode, complex_out.mode);

  // Also test NC_VLEN over user-defined base type with aligned base type IDs.
  ScopedNcFile src_vlen;
  ScopedNcFile dst_vlen;
  ASSERT_EQ(NC_NOERR, CreateInMemoryDataset("dcopy_vlen_user_src.nc",
                                            NC_NETCDF4, &src_vlen));
  ASSERT_EQ(NC_NOERR, CreateInMemoryDataset("dcopy_vlen_user_dst.nc",
                                            NC_NETCDF4, &dst_vlen));
  nc_type se = NC_NAT;
  nc_type sv = NC_NAT;
  nc_type de = NC_NAT;
  nc_type dv = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(src_vlen.get(), NC_INT, "ue", &se));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(src_vlen.get(), se, "COLD", &kCold));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(src_vlen.get(), se, "WARM", &kWarm));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(src_vlen.get(), "ve", se, &sv));
  ASSERT_EQ(NC_NOERR, nc_def_enum(dst_vlen.get(), NC_INT, "ue", &de));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(dst_vlen.get(), de, "COLD", &kCold));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(dst_vlen.get(), de, "WARM", &kWarm));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(dst_vlen.get(), "ve", de, &dv));
  std::array<int32_t, 2> vlen_e_elems{kCold, kWarm};
  nc_vlen_t vlen_e_val{vlen_e_elems.size(), vlen_e_elems.data()};
  ASSERT_EQ(NC_NOERR, nc_put_att(src_vlen.get(), NC_GLOBAL, "att_ve", sv, 1,
                                 &vlen_e_val));
  ASSERT_EQ(NC_NOERR, nc_enddef(src_vlen.get()));
  ASSERT_EQ(NC_NOERR, nc_enddef(dst_vlen.get()));
  ASSERT_EQ(NC_NOERR, nc_copy_att(src_vlen.get(), NC_GLOBAL, "att_ve",
                                  dst_vlen.get(), NC_GLOBAL));
  ScopedNcVlens ve_out(1);
  ASSERT_EQ(NC_NOERR,
            nc_get_att(dst_vlen.get(), NC_GLOBAL, "att_ve", ve_out.data()));
  ASSERT_EQ(size_t{2}, ve_out[0].len);
  const int32_t* ve_ptr = static_cast<const int32_t*>(ve_out[0].p);
  EXPECT_THAT(std::vector<int32_t>(ve_ptr, ve_ptr + 2),
              ElementsAre(kCold, kWarm));
}

TEST(DcopyTest, CopyAttRejectsMismatchedAndMissingUserTypes) {
  auto verify_copy_fails = [](const char* label, auto setup_src,
                              auto setup_dst) {
    SCOPED_TRACE(label);
    ScopedNcFile src;
    ScopedNcFile dst;
    ASSERT_EQ(NC_NOERR,
              CreateInMemoryDataset("dcopy_mm_src.nc", NC_NETCDF4, &src));
    ASSERT_EQ(NC_NOERR,
              CreateInMemoryDataset("dcopy_mm_dst.nc", NC_NETCDF4, &dst));
    ASSERT_NO_FATAL_FAILURE(setup_src(src.get()));
    ASSERT_NO_FATAL_FAILURE(setup_dst(dst.get()));
    ASSERT_EQ(NC_NOERR, nc_enddef(src.get()));
    ASSERT_EQ(NC_NOERR, nc_enddef(dst.get()));
    EXPECT_EQ(NC_EBADTYPE,
              nc_copy_att(src.get(), NC_GLOBAL, "att", dst.get(), NC_GLOBAL));
  };

  // 1. Opaque size, name, and class mismatches.
  verify_copy_fails(
      "opaque_size_mismatch",
      [](int s) {
        nc_type t = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_opaque(s, 4, "t", &t));
        constexpr std::array<uint8_t, 4> v{};
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", t, 1, v.data()));
      },
      [](int d) {
        nc_type t = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_opaque(d, 8, "t", &t));
      });
  verify_copy_fails(
      "opaque_name_mismatch",
      [](int s) {
        nc_type t = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_opaque(s, 4, "t_a", &t));
        constexpr std::array<uint8_t, 4> v{};
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", t, 1, v.data()));
      },
      [](int d) {
        nc_type t = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_opaque(d, 4, "t_b", &t));
      });
  // NetCDF-4 requires at least one member before committing an NC_ENUM type.
  verify_copy_fails(
      "opaque_vs_enum_class_mismatch",
      [](int s) {
        nc_type t = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_opaque(s, 4, "t", &t));
        constexpr std::array<uint8_t, 4> v{};
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", t, 1, v.data()));
      },
      [](int d) {
        nc_type t = NC_NAT;
        constexpr int32_t m = 0;
        ASSERT_EQ(NC_NOERR, nc_def_enum(d, NC_INT, "t", &t));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(d, t, "M", &m));
      });

  // 2. Enum mismatches: base type, nelems, member name, member value.
  verify_copy_fails(
      "enum_base_type_mismatch",
      [](int s) {
        nc_type e = NC_NAT;
        constexpr int32_t m = 1;
        ASSERT_EQ(NC_NOERR, nc_def_enum(s, NC_INT, "e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(s, e, "A", &m));
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", e, 1, &m));
      },
      [](int d) {
        nc_type e = NC_NAT;
        constexpr uint32_t m = 1;
        ASSERT_EQ(NC_NOERR, nc_def_enum(d, NC_UINT, "e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(d, e, "A", &m));
      });
  verify_copy_fails(
      "enum_nelems_mismatch",
      [](int s) {
        nc_type e = NC_NAT;
        constexpr int32_t m0 = 1;
        constexpr int32_t m1 = 2;
        ASSERT_EQ(NC_NOERR, nc_def_enum(s, NC_INT, "e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(s, e, "A", &m0));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(s, e, "B", &m1));
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", e, 1, &m0));
      },
      [](int d) {
        nc_type e = NC_NAT;
        constexpr int32_t m0 = 1;
        ASSERT_EQ(NC_NOERR, nc_def_enum(d, NC_INT, "e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(d, e, "A", &m0));
      });
  verify_copy_fails(
      "enum_member_name_mismatch",
      [](int s) {
        nc_type e = NC_NAT;
        constexpr int32_t m0 = 1;
        ASSERT_EQ(NC_NOERR, nc_def_enum(s, NC_INT, "e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(s, e, "A", &m0));
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", e, 1, &m0));
      },
      [](int d) {
        nc_type e = NC_NAT;
        constexpr int32_t m0 = 1;
        ASSERT_EQ(NC_NOERR, nc_def_enum(d, NC_INT, "e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(d, e, "OTHER", &m0));
      });
  verify_copy_fails(
      "enum_member_value_mismatch",
      [](int s) {
        nc_type e = NC_NAT;
        constexpr int32_t m0 = 1;
        ASSERT_EQ(NC_NOERR, nc_def_enum(s, NC_INT, "e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(s, e, "A", &m0));
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", e, 1, &m0));
      },
      [](int d) {
        nc_type e = NC_NAT;
        constexpr int32_t m0 = 99;
        ASSERT_EQ(NC_NOERR, nc_def_enum(d, NC_INT, "e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(d, e, "A", &m0));
      });

  // 3. Compound mismatches: nelems, ndims, dimsizes, and field type.
  verify_copy_fails(
      "compound_nelems_mismatch",
      [](int s) {
        nc_type c = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_compound(s, 8, "c", &c));
        ASSERT_EQ(NC_NOERR, nc_insert_compound(s, c, "x", 0, NC_DOUBLE));
        constexpr double v = 1.0;
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", c, 1, &v));
      },
      [](int d) {
        nc_type c = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_compound(d, 8, "c", &c));
        ASSERT_EQ(NC_NOERR, nc_insert_compound(d, c, "a", 0, NC_INT));
        ASSERT_EQ(NC_NOERR, nc_insert_compound(d, c, "b", 4, NC_INT));
      });
  verify_copy_fails(
      "compound_field_ndims_mismatch",
      [](int s) {
        nc_type c = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_compound(s, 4, "c", &c));
        ASSERT_EQ(NC_NOERR, nc_insert_compound(s, c, "x", 0, NC_INT));
        constexpr int32_t v = 7;
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", c, 1, &v));
      },
      [](int d) {
        nc_type c = NC_NAT;
        constexpr int kOne[1] = {1};
        ASSERT_EQ(NC_NOERR, nc_def_compound(d, 4, "c", &c));
        ASSERT_EQ(NC_NOERR,
                  nc_insert_array_compound(d, c, "x", 0, NC_INT, 1, kOne));
      });
  verify_copy_fails(
      "compound_field_dimsizes_mismatch",
      [](int s) {
        nc_type c = NC_NAT;
        constexpr int kShape[2] = {2, 3};
        ASSERT_EQ(NC_NOERR, nc_def_compound(s, 24, "c", &c));
        ASSERT_EQ(NC_NOERR,
                  nc_insert_array_compound(s, c, "m", 0, NC_INT, 2, kShape));
        constexpr std::array<int32_t, 6> v{};
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", c, 1, v.data()));
      },
      [](int d) {
        nc_type c = NC_NAT;
        constexpr int kShape[2] = {3, 2};
        ASSERT_EQ(NC_NOERR, nc_def_compound(d, 24, "c", &c));
        ASSERT_EQ(NC_NOERR,
                  nc_insert_array_compound(d, c, "m", 0, NC_INT, 2, kShape));
      });
  verify_copy_fails(
      "compound_field_type_mismatch",
      [](int s) {
        nc_type c = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_compound(s, 8, "c", &c));
        ASSERT_EQ(NC_NOERR, nc_insert_compound(s, c, "x", 0, NC_INT64));
        constexpr int64_t v = 9;
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", c, 1, &v));
      },
      [](int d) {
        nc_type c = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_compound(d, 8, "c", &c));
        ASSERT_EQ(NC_NOERR, nc_insert_compound(d, c, "x", 0, NC_DOUBLE));
      });

  // 4. NC_VLEN user-defined base type mismatch and missing base typeid.
  verify_copy_fails(
      "vlen_base_class_mismatch",
      [](int s) {
        nc_type e = NC_NAT;
        nc_type v = NC_NAT;
        constexpr int32_t m0 = 1;
        ASSERT_EQ(NC_NOERR, nc_def_enum(s, NC_INT, "base_e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(s, e, "A", &m0));
        ASSERT_EQ(NC_NOERR, nc_def_vlen(s, "my_vlen", e, &v));
        std::array<int32_t, 1> elem{m0};
        nc_vlen_t val{elem.size(), elem.data()};
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", v, 1, &val));
      },
      [](int d) {
        nc_type op = NC_NAT;
        nc_type v = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_opaque(d, 4, "other_t", &op));
        ASSERT_EQ(NC_NOERR, nc_def_vlen(d, "my_vlen", NC_INT, &v));
      });
  verify_copy_fails(
      "vlen_missing_base_typeid_root",
      [](int s) {
        nc_type dummy = NC_NAT;
        nc_type e = NC_NAT;
        nc_type v = NC_NAT;
        constexpr int32_t m0 = 1;
        ASSERT_EQ(NC_NOERR, nc_def_opaque(s, 4, "pad", &dummy));
        ASSERT_EQ(NC_NOERR, nc_def_enum(s, NC_INT, "base_e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(s, e, "A", &m0));
        ASSERT_EQ(NC_NOERR, nc_def_vlen(s, "my_vlen", e, &v));
        std::array<int32_t, 1> elem{m0};
        nc_vlen_t val{elem.size(), elem.data()};
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", v, 1, &val));
      },
      [](int d) {
        // Place matching-named vlen in root without base_e's typeid slot so
        // searchgroup() in NC_rec_find_nc_type() returns NC_EBADTYPE directly.
        nc_type v = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_vlen(d, "my_vlen", NC_INT, &v));
      });
  verify_copy_fails(
      "vlen_missing_base_typeid_subgroup",
      [](int s) {
        nc_type dummy = NC_NAT;
        nc_type e = NC_NAT;
        nc_type v = NC_NAT;
        constexpr int32_t m0 = 1;
        ASSERT_EQ(NC_NOERR, nc_def_opaque(s, 4, "pad", &dummy));
        ASSERT_EQ(NC_NOERR, nc_def_enum(s, NC_INT, "base_e", &e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(s, e, "A", &m0));
        ASSERT_EQ(NC_NOERR, nc_def_vlen(s, "my_vlen", e, &v));
        std::array<int32_t, 1> elem{m0};
        nc_vlen_t val{elem.size(), elem.data()};
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", v, 1, &val));
      },
      [](int d) {
        // Place matching-named vlen inside a subgroup without defining base_e's
        // typeid slot so searchgrouptree() also exercises the error return
        // path.
        int sub = -1;
        nc_type v = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_grp(d, "sub", &sub));
        ASSERT_EQ(NC_NOERR, nc_def_vlen(sub, "my_vlen", NC_INT, &v));
      });

  // 5. Compound field comparison propagating missing vlen base typeid error.
  verify_copy_fails(
      "compound_field_missing_vlen_base",
      [](int s) {
        nc_type pad1 = NC_NAT;
        nc_type pad2 = NC_NAT;
        nc_type base_e = NC_NAT;
        nc_type field_v = NC_NAT;
        nc_type cmp = NC_NAT;
        constexpr int32_t m0 = 1;
        ASSERT_EQ(NC_NOERR, nc_def_opaque(s, 4, "pad1", &pad1));
        ASSERT_EQ(NC_NOERR, nc_def_opaque(s, 4, "pad2", &pad2));
        ASSERT_EQ(NC_NOERR, nc_def_enum(s, NC_INT, "base_e", &base_e));
        ASSERT_EQ(NC_NOERR, nc_insert_enum(s, base_e, "A", &m0));
        ASSERT_EQ(NC_NOERR, nc_def_vlen(s, "fv", base_e, &field_v));
        ASSERT_EQ(NC_NOERR, nc_def_compound(s, sizeof(nc_vlen_t), "c", &cmp));
        ASSERT_EQ(NC_NOERR, nc_insert_compound(s, cmp, "fv", 0, field_v));
        std::array<int32_t, 1> elem{m0};
        nc_vlen_t val{elem.size(), elem.data()};
        ASSERT_EQ(NC_NOERR, nc_put_att(s, NC_GLOBAL, "att", cmp, 1, &val));
      },
      [](int d) {
        nc_type field_v = NC_NAT;
        nc_type cmp = NC_NAT;
        ASSERT_EQ(NC_NOERR, nc_def_vlen(d, "fv", NC_INT, &field_v));
        ASSERT_EQ(NC_NOERR, nc_def_compound(d, sizeof(nc_vlen_t), "c", &cmp));
        ASSERT_EQ(NC_NOERR, nc_insert_compound(d, cmp, "fv", 0, field_v));
      });

  // 6. Copying a user-defined attribute into a classic file fails NC_EBADTYPE.
  ScopedNcFile src;
  ScopedNcFile classic_dst;
  ASSERT_EQ(NC_NOERR,
            CreateInMemoryDataset("dcopy_classic_src.nc", NC_NETCDF4, &src));
  ASSERT_EQ(NC_NOERR,
            CreateInMemoryDataset("dcopy_classic_ut_dst.nc", 0, &classic_dst));
  nc_type op = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(src.get(), 4, "op", &op));
  constexpr std::array<uint8_t, 4> kBytes{1, 2, 3, 4};
  ASSERT_EQ(NC_NOERR,
            nc_put_att(src.get(), NC_GLOBAL, "op_att", op, 1, kBytes.data()));
  ASSERT_EQ(NC_NOERR, nc_enddef(src.get()));
  EXPECT_EQ(NC_EBADTYPE, nc_copy_att(src.get(), NC_GLOBAL, "op_att",
                                     classic_dst.get(), NC_GLOBAL));
}

}  // namespace
}  // namespace netcdf
