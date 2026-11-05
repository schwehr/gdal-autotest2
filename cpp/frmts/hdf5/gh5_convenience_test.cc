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

// Unit tests for HDF5 convenience attribute and datatype helpers.
// See also:
//   https://gdal.org/drivers/raster/hdf5.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hdf5/gh5_convenience.cpp

#include "third_party/gdal/frmts/hdf5/gh5_convenience.h"

#include <cstdint>
#include <span>  // NOLINT
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/hdf5/hdf5_test_util.h"
#include "third_party/gdal/frmts/hdf5/hdf5_api.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/hdf5/src/H5Apublic.h"
#include "third_party/hdf5/src/H5Ppublic.h"
#include "third_party/hdf5/src/H5Spublic.h"
#include "third_party/hdf5/src/H5Tpublic.h"
#include "third_party/hdf5/src/H5version.h"

namespace autotest2 {
namespace {

TEST(Gh5ConvenienceTest, FetchStringAttribute) {
  ScopedVsimemFile mem_file("fetch_string.h5");
  ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
  ASSERT_GE(file_id.get(), 0);

  ScopedH5Dataspace scalar_space(H5Screate(H5S_SCALAR));
  ASSERT_GE(scalar_space.get(), 0);

  // 1. Fixed-length NUL-padded string attribute (scalar space).
  ASSERT_TRUE(WriteScalarStringAttr(
      file_id, "fixed_str", std::string_view("hello\0\0\0", 8),
      H5T_STR_NULLPAD));

  // 2. Fixed-length full-width string attribute (1D size=1, no NUL).
  {
    const char buf[5] = {'a', 'b', 'c', 'd', 'e'};
    ASSERT_TRUE(Write1DFixedStringAttr(file_id, "full_fixed_str", 5, buf));
  }

  // 3. Variable-length string attribute (non-null).
  {
    ScopedH5Datatype vstr_type(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(vstr_type, H5T_VARIABLE), 0);
    ScopedH5Attribute attr(
        H5Acreate(file_id, "vlen_str", vstr_type, scalar_space, H5P_DEFAULT));
    ASSERT_GE(attr.get(), 0);
    const char* val = "variable_length_value";
    ASSERT_GE(H5Awrite(attr, vstr_type, &val), 0);
  }

  // 4. Variable-length string attribute with null pointer (unwritten).
  {
    ScopedH5Datatype vstr_type(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(vstr_type, H5T_VARIABLE), 0);
    ScopedH5Attribute attr(H5Acreate(file_id, "vlen_null_str", vstr_type,
                                     scalar_space, H5P_DEFAULT));
    ASSERT_GE(attr.get(), 0);
  }

  // 5. Multi-element string attribute (1D size=2 -> rejected).
  {
    const char buf[8] = "abcdefg";
    ASSERT_TRUE(Write1DFixedStringAttr(file_id, "array_str", 4, buf));
  }

  // 6. Numeric attribute (unsupported for string conversion).
  ASSERT_TRUE(WriteScalarAttr<int>(file_id, "int_attr", H5T_NATIVE_INT, 42));

  // 7. Verify GH5_FetchAttribute string overloads and error paths.
  CPLString result = "stale";
  EXPECT_FALSE(GH5_FetchAttribute(file_id, "missing_attr", result, false));
  EXPECT_EQ("stale", result);

  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(GH5_FetchAttribute(file_id, "missing_attr", result, true));
    EXPECT_TRUE(result.empty());
  }

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "fixed_str", result, true));
  EXPECT_EQ("hello", result);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "full_fixed_str", result, true));
  EXPECT_EQ("abcde", result);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "vlen_str", result, true));
  EXPECT_EQ("variable_length_value", result);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "vlen_null_str", result, true));
  EXPECT_TRUE(result.empty());

  EXPECT_FALSE(GH5_FetchAttribute(file_id, "array_str", result, true));
  EXPECT_FALSE(GH5_FetchAttribute(file_id, "int_attr", result, false));

  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(GH5_FetchAttribute(file_id, "int_attr", result, true));
  }
}

TEST(Gh5ConvenienceTest, FetchDoubleAttribute) {
  ScopedVsimemFile mem_file("fetch_double.h5");
  ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
  ASSERT_GE(file_id.get(), 0);

  // 1. Populate scalar attributes for every supported HDF5 numeric type.
  ASSERT_TRUE(
      WriteScalarAttr<char>(file_id, "char_attr", H5T_NATIVE_CHAR, 'A'));
  ASSERT_TRUE(WriteScalarAttr<signed char>(file_id, "schar_attr",
                                           H5T_NATIVE_SCHAR, -12));
  ASSERT_TRUE(WriteScalarAttr<unsigned char>(file_id, "uchar_attr",
                                             H5T_NATIVE_UCHAR, 210));
  ASSERT_TRUE(
      WriteScalarAttr<short>(file_id, "short_attr", H5T_NATIVE_SHORT, -1234));
  ASSERT_TRUE(WriteScalarAttr<unsigned short>(file_id, "ushort_attr",
                                              H5T_NATIVE_USHORT, 54321));
  ASSERT_TRUE(
      WriteScalarAttr<int>(file_id, "int_attr", H5T_NATIVE_INT, -100000));
  ASSERT_TRUE(WriteScalarAttr<unsigned int>(file_id, "uint_attr",
                                            H5T_NATIVE_UINT, 3000000000U));
  ASSERT_TRUE(WriteScalarAttr<int64_t>(file_id, "int64_exact", H5T_NATIVE_INT64,
                                       -1234567890123LL));
  ASSERT_TRUE(WriteScalarAttr<int64_t>(file_id, "int64_lossy", H5T_NATIVE_INT64,
                                       (1LL << 53) + 1));
  ASSERT_TRUE(WriteScalarAttr<uint64_t>(file_id, "uint64_exact",
                                        H5T_NATIVE_UINT64, 1234567890123ULL));
  ASSERT_TRUE(WriteScalarAttr<uint64_t>(file_id, "uint64_lossy",
                                        H5T_NATIVE_UINT64, (1ULL << 53) + 1));
  ASSERT_TRUE(
      WriteScalarAttr<float>(file_id, "float_attr", H5T_NATIVE_FLOAT, 3.25f));
  ASSERT_TRUE(WriteScalarAttr<double>(file_id, "double_attr", H5T_NATIVE_DOUBLE,
                                      2.718281828459045));
#ifdef HDF5_HAVE_FLOAT16
  ASSERT_TRUE(WriteScalarAttr<_Float16>(file_id, "float16_attr",
                                        H5T_NATIVE_FLOAT16,
                                        static_cast<_Float16>(1.5f)));
#endif

  // 2. Multi-element numeric attribute (rejected since count != 1) and string.
  const double vals[2] = {1.0, 2.0};
  ASSERT_TRUE(
      Write1DAttr<double>(file_id, "multi_double", H5T_NATIVE_DOUBLE, vals));
  ASSERT_TRUE(WriteScalarStringAttr(file_id, "str_attr", "abc"));

  // 3. Verify error branches and double conversions (including > 2^53).
  double val = 999.0;
  EXPECT_FALSE(GH5_FetchAttribute(file_id, "no_such_attr", val, false));
  EXPECT_EQ(999.0, val);

  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(GH5_FetchAttribute(file_id, "no_such_attr", val, true));
    EXPECT_EQ(0.0, val);
  }

  EXPECT_FALSE(GH5_FetchAttribute(file_id, "multi_double", val, false));
  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(GH5_FetchAttribute(file_id, "multi_double", val, true));
  }

  EXPECT_FALSE(GH5_FetchAttribute(file_id, "str_attr", val, false));
  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(GH5_FetchAttribute(file_id, "str_attr", val, true));
  }

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "char_attr", val, true));
  EXPECT_EQ(static_cast<double>('A'), val);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "schar_attr", val, true));
  EXPECT_EQ(-12.0, val);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "uchar_attr", val, true));
  EXPECT_EQ(210.0, val);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "short_attr", val, true));
  EXPECT_EQ(-1234.0, val);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "ushort_attr", val, true));
  EXPECT_EQ(54321.0, val);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "int_attr", val, true));
  EXPECT_EQ(-100000.0, val);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "uint_attr", val, true));
  EXPECT_EQ(3000000000.0, val);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "int64_exact", val, true));
  EXPECT_EQ(-1234567890123.0, val);

  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_TRUE(GH5_FetchAttribute(file_id, "int64_lossy", val, true));
  }
  EXPECT_EQ(static_cast<double>((1LL << 53) + 1), val);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "uint64_exact", val, true));
  EXPECT_EQ(1234567890123.0, val);

  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_TRUE(GH5_FetchAttribute(file_id, "uint64_lossy", val, true));
  }
  EXPECT_EQ(static_cast<double>((1ULL << 53) + 1), val);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "float_attr", val, true));
  EXPECT_EQ(3.25, val);

  EXPECT_TRUE(GH5_FetchAttribute(file_id, "double_attr", val, true));
  EXPECT_DOUBLE_EQ(2.718281828459045, val);

#ifdef HDF5_HAVE_FLOAT16
  EXPECT_TRUE(GH5_FetchAttribute(file_id, "float16_attr", val, true));
  EXPECT_EQ(1.5, val);
#endif
}

TEST(Gh5ConvenienceTest, GetDataType) {
  struct TypeMapping {
    hid_t h5_type;
    GDALDataType expected;
  };
  const TypeMapping kMappings[] = {
      {.h5_type = H5T_NATIVE_CHAR, .expected = GDT_Byte},
      {.h5_type = H5T_NATIVE_SCHAR, .expected = GDT_Int8},
      {.h5_type = H5T_NATIVE_UCHAR, .expected = GDT_Byte},
      {.h5_type = H5T_NATIVE_SHORT, .expected = GDT_Int16},
      {.h5_type = H5T_NATIVE_USHORT, .expected = GDT_UInt16},
      {.h5_type = H5T_NATIVE_INT, .expected = GDT_Int32},
      {.h5_type = H5T_NATIVE_UINT, .expected = GDT_UInt32},
      {.h5_type = H5T_NATIVE_LONG,
       .expected = sizeof(long) == 4 ? GDT_Int32 : GDT_Unknown},
      {.h5_type = H5T_NATIVE_ULONG,
       .expected = sizeof(unsigned long) == 4 ? GDT_UInt32 : GDT_Unknown},
      {.h5_type = H5T_NATIVE_FLOAT, .expected = GDT_Float32},
      {.h5_type = H5T_NATIVE_DOUBLE, .expected = GDT_Float64},
      {.h5_type = H5T_NATIVE_LLONG, .expected = GDT_Unknown},
      {.h5_type = H5T_NATIVE_ULLONG, .expected = GDT_Unknown},
      {.h5_type = H5T_C_S1, .expected = GDT_Unknown},
  };
  for (const TypeMapping& mapping : kMappings) {
    EXPECT_EQ(mapping.expected, GH5_GetDataType(mapping.h5_type));
  }
}

TEST(Gh5ConvenienceTest, CreateAndWriteAttributes) {
  ScopedVsimemFile mem_file("create_write.h5");
  ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
  ASSERT_GE(file_id.get(), 0);

  // 1. Invalid inputs on GH5_CreateAttribute.
  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(GH5_CreateAttribute(file_id, "bad_type", H5I_INVALID_HID, 0));
    EXPECT_FALSE(GH5_CreateAttribute(file_id, "zero_str", H5T_C_S1, 0));
    EXPECT_FALSE(
        GH5_CreateAttribute(H5I_INVALID_HID, "bad_loc", H5T_NATIVE_INT, 0));
  }

  // 2. Create scalar attributes and write via string, double, and uint
  // overloads.
  ASSERT_TRUE(GH5_CreateAttribute(file_id, "str_attr", H5T_C_S1, 16));
  ASSERT_TRUE(GH5_CreateAttribute(file_id, "f32_attr", H5T_NATIVE_FLOAT));
  ASSERT_TRUE(GH5_CreateAttribute(file_id, "f64_attr", H5T_NATIVE_DOUBLE));
  ASSERT_TRUE(GH5_CreateAttribute(file_id, "i32_attr", H5T_NATIVE_INT));
  ASSERT_TRUE(GH5_CreateAttribute(file_id, "u32_attr", H5T_NATIVE_UINT));

  char str_payload[16] = "written_string";
  EXPECT_TRUE(GH5_WriteAttribute(file_id, "str_attr", str_payload));
  CPLString str_out;
  EXPECT_TRUE(GH5_FetchAttribute(file_id, "str_attr", str_out, true));
  EXPECT_EQ("written_string", str_out);

  EXPECT_TRUE(GH5_WriteAttribute(file_id, "f32_attr", 12.5));
  double dbl_out = 0.0;
  EXPECT_TRUE(GH5_FetchAttribute(file_id, "f32_attr", dbl_out, true));
  EXPECT_EQ(12.5, dbl_out);

  EXPECT_TRUE(GH5_WriteAttribute(file_id, "f64_attr", -9876.54321));
  EXPECT_TRUE(GH5_FetchAttribute(file_id, "f64_attr", dbl_out, true));
  EXPECT_DOUBLE_EQ(-9876.54321, dbl_out);

  EXPECT_TRUE(GH5_WriteAttribute(file_id, "i32_attr", 123456U));
  EXPECT_TRUE(GH5_FetchAttribute(file_id, "i32_attr", dbl_out, true));
  EXPECT_EQ(123456.0, dbl_out);

  EXPECT_TRUE(GH5_WriteAttribute(file_id, "u32_attr", 345678U));
  EXPECT_TRUE(GH5_FetchAttribute(file_id, "u32_attr", dbl_out, true));
  EXPECT_EQ(345678.0, dbl_out);

  // 3. Missing and mismatched attribute types on GH5_WriteAttribute overloads.
  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(GH5_WriteAttribute(file_id, "missing", "abc"));
    EXPECT_FALSE(GH5_WriteAttribute(file_id, "i32_attr", "not_a_string"));

    EXPECT_FALSE(GH5_WriteAttribute(file_id, "missing", 1.0));
    EXPECT_FALSE(GH5_WriteAttribute(file_id, "i32_attr", 1.0));

    EXPECT_FALSE(GH5_WriteAttribute(file_id, "missing", 1U));
    EXPECT_FALSE(GH5_WriteAttribute(file_id, "f64_attr", 1U));
  }
}

// TODO: b/335317901 - Cover HDF5 1.8 H5Acreate2 fallback when H5_USE_16_API is
// disabled.

}  // namespace
}  // namespace autotest2
