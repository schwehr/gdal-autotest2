// Copyright 2014 Google LLC
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
//
// Tests gcore/gdal_misc.cpp.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdal_misc.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_gcp.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kVsiMemDir[] = "/vsimem/gdal_misc_test";

TEST(GdalMiscTest, GdalDataTypeUnion) {
  struct DataTypeUnionCase {
    GDALDataType a;
    GDALDataType b;
    GDALDataType expected;
  };
  // Tests a subset of type pairs.
  constexpr DataTypeUnionCase kCases[] = {
      {.a = GDT_Unknown, .b = GDT_Unknown, .expected = GDT_Unknown},
      {.a = GDT_Unknown, .b = GDT_Byte, .expected = GDT_Byte},
      {.a = GDT_Byte, .b = GDT_Unknown, .expected = GDT_Byte},
      {.a = GDT_Byte, .b = GDT_TypeCount, .expected = GDT_Unknown},
      {.a = GDT_TypeCount, .b = GDT_Byte, .expected = GDT_Unknown},
      {.a = GDT_Unknown, .b = GDT_UInt16, .expected = GDT_UInt16},
      {.a = GDT_Unknown, .b = GDT_Int16, .expected = GDT_Int16},
      {.a = GDT_Unknown, .b = GDT_UInt32, .expected = GDT_UInt32},
      {.a = GDT_Unknown, .b = GDT_Int32, .expected = GDT_Int32},
      {.a = GDT_Unknown, .b = GDT_Float32, .expected = GDT_Float32},
      {.a = GDT_Unknown, .b = GDT_Float64, .expected = GDT_Float64},
      {.a = GDT_Unknown, .b = GDT_CInt16, .expected = GDT_CInt16},
      {.a = GDT_Unknown, .b = GDT_CInt32, .expected = GDT_CInt32},
      {.a = GDT_Unknown, .b = GDT_CFloat32, .expected = GDT_CFloat32},
      {.a = GDT_Unknown, .b = GDT_CFloat64, .expected = GDT_CFloat64},
      {.a = GDT_Byte, .b = GDT_UInt16, .expected = GDT_UInt16},
      {.a = GDT_Byte, .b = GDT_Int16, .expected = GDT_Int16},
      {.a = GDT_Int16, .b = GDT_Byte, .expected = GDT_Int16},
      {.a = GDT_Byte, .b = GDT_Int8, .expected = GDT_Int16},
      {.a = GDT_Int8, .b = GDT_Byte, .expected = GDT_Int16},
      {.a = GDT_Int16, .b = GDT_UInt16, .expected = GDT_Int32},
      {.a = GDT_Int16, .b = GDT_UInt32, .expected = GDT_Int64},
      {.a = GDT_UInt32, .b = GDT_Int16, .expected = GDT_Int64},
      {.a = GDT_UInt32, .b = GDT_CInt16, .expected = GDT_CFloat64},
      {.a = GDT_Int32, .b = GDT_CInt16, .expected = GDT_CInt32},
      {.a = GDT_Float32, .b = GDT_CInt16, .expected = GDT_CFloat32},
      {.a = GDT_Float32, .b = GDT_CInt32, .expected = GDT_CFloat64},
      {.a = GDT_Float64, .b = GDT_CInt32, .expected = GDT_CFloat64},
      {.a = GDT_CInt16, .b = GDT_UInt32, .expected = GDT_CFloat64},
      {.a = GDT_CInt16, .b = GDT_Int32, .expected = GDT_CInt32},
      {.a = GDT_CInt16, .b = GDT_CFloat32, .expected = GDT_CFloat32},
      {.a = GDT_CInt16, .b = GDT_CFloat64, .expected = GDT_CFloat64},
      {.a = GDT_CInt32, .b = GDT_Byte, .expected = GDT_CInt32},
      {.a = GDT_CInt32, .b = GDT_UInt16, .expected = GDT_CInt32},
      {.a = GDT_CInt32, .b = GDT_Int16, .expected = GDT_CInt32},
      {.a = GDT_CInt32, .b = GDT_UInt32, .expected = GDT_CFloat64},
      {.a = GDT_CInt32, .b = GDT_Int32, .expected = GDT_CInt32},
      {.a = GDT_CInt32, .b = GDT_Float32, .expected = GDT_CFloat64},
      {.a = GDT_CInt32, .b = GDT_CInt16, .expected = GDT_CInt32},
      {.a = GDT_CInt32, .b = GDT_CFloat32, .expected = GDT_CFloat64},
      {.a = GDT_CInt32, .b = GDT_CFloat64, .expected = GDT_CFloat64},
      {.a = GDT_CFloat32, .b = GDT_Byte, .expected = GDT_CFloat32},
      {.a = GDT_CFloat32, .b = GDT_UInt16, .expected = GDT_CFloat32},
      {.a = GDT_CFloat32, .b = GDT_Int16, .expected = GDT_CFloat32},
      {.a = GDT_CFloat32, .b = GDT_UInt32, .expected = GDT_CFloat64},
      {.a = GDT_CFloat32, .b = GDT_Int32, .expected = GDT_CFloat64},
      {.a = GDT_CFloat32, .b = GDT_Float32, .expected = GDT_CFloat32},
      {.a = GDT_CFloat32, .b = GDT_Float64, .expected = GDT_CFloat64},
      {.a = GDT_CFloat32, .b = GDT_CInt16, .expected = GDT_CFloat32},
      {.a = GDT_CFloat32, .b = GDT_CInt32, .expected = GDT_CFloat64},
      {.a = GDT_CFloat64, .b = GDT_Int32, .expected = GDT_CFloat64},
      {.a = GDT_CFloat64, .b = GDT_Float32, .expected = GDT_CFloat64},
      {.a = GDT_CFloat64, .b = GDT_Float64, .expected = GDT_CFloat64},
      {.a = GDT_CFloat64, .b = GDT_CInt16, .expected = GDT_CFloat64},
      {.a = GDT_CFloat64, .b = GDT_CInt32, .expected = GDT_CFloat64},
      {.a = GDT_CFloat64, .b = GDT_CFloat32, .expected = GDT_CFloat64},
      // For Float16 / RFC 100.
      {.a = GDT_Int64, .b = GDT_Float16, .expected = GDT_Float64},
      {.a = GDT_UInt64, .b = GDT_Float16, .expected = GDT_Float64},
      {.a = GDT_Float16, .b = GDT_CInt32, .expected = GDT_CFloat64},
      {.a = GDT_CInt16, .b = GDT_CFloat16, .expected = GDT_CFloat32},
      {.a = GDT_CFloat16, .b = GDT_Byte, .expected = GDT_CFloat32},
      {.a = GDT_CFloat16, .b = GDT_UInt16, .expected = GDT_CFloat32},
      {.a = GDT_CFloat16, .b = GDT_Int16, .expected = GDT_CFloat32},
      {.a = GDT_CFloat16, .b = GDT_UInt32, .expected = GDT_CFloat64},
      {.a = GDT_CFloat16, .b = GDT_Int32, .expected = GDT_CFloat64},
      {.a = GDT_CFloat16, .b = GDT_Float32, .expected = GDT_CFloat32},
      {.a = GDT_CFloat16, .b = GDT_CInt16, .expected = GDT_CFloat32},
      {.a = GDT_CFloat16, .b = GDT_CInt32, .expected = GDT_CFloat64},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(c.expected, GDALDataTypeUnion(c.a, c.b));
  }
}

TEST(GdalMiscTest, GDALDataTypeUnionWithValue) {
  struct UnionWithValueCase {
    GDALDataType dt;
    double value;
    int is_complex;
    GDALDataType expected;
  };
  constexpr UnionWithValueCase kCases[] = {
      {.dt = GDT_Unknown,
       .value = 1.0,
       .is_complex = FALSE,
       .expected = GDT_Byte},
      {.dt = GDT_Byte, .value = 1.0, .is_complex = FALSE, .expected = GDT_Byte},
      {.dt = GDT_Byte,
       .value = -1.0,
       .is_complex = FALSE,
       .expected = GDT_Int16},
      {.dt = GDT_Byte,
       .value = 255.0,
       .is_complex = FALSE,
       .expected = GDT_Byte},
      {.dt = GDT_Byte,
       .value = 256.0,
       .is_complex = FALSE,
       .expected = GDT_UInt16},
      {.dt = GDT_Byte,
       .value = 32767.0,
       .is_complex = FALSE,
       .expected = GDT_UInt16},
      {.dt = GDT_Byte,
       .value = 32768.0,
       .is_complex = FALSE,
       .expected = GDT_UInt16},
      {.dt = GDT_Byte,
       .value = -32768.0,
       .is_complex = FALSE,
       .expected = GDT_Int16},
      {.dt = GDT_Byte,
       .value = -32769.0,
       .is_complex = FALSE,
       .expected = GDT_Int32},
      {.dt = GDT_Byte,
       .value = 65535.0,
       .is_complex = FALSE,
       .expected = GDT_UInt16},
      {.dt = GDT_Byte,
       .value = 65536.0,
       .is_complex = FALSE,
       .expected = GDT_UInt32},
      {.dt = GDT_Byte,
       .value = 2147483647.0,
       .is_complex = FALSE,
       .expected = GDT_UInt32},
      {.dt = GDT_Byte,
       .value = -2147483648.0,
       .is_complex = FALSE,
       .expected = GDT_Int32},
      {.dt = GDT_Byte,
       .value = -2147483649.0,
       .is_complex = FALSE,
       .expected = GDT_Int64},
      {.dt = GDT_Byte,
       .value = 4294967295.0,
       .is_complex = FALSE,
       .expected = GDT_UInt32},
      {.dt = GDT_Byte,
       .value = 4294967296.0,
       .is_complex = FALSE,
       .expected = GDT_UInt64},
      {.dt = GDT_Float32,
       .value = std::numeric_limits<double>::quiet_NaN(),
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.dt = GDT_Float32,
       .value = std::numeric_limits<double>::infinity(),
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.dt = GDT_Float32,
       .value = -std::numeric_limits<double>::infinity(),
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.dt = GDT_Float64,
       .value = -99999.9876,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      {.dt = GDT_Float64,
       .value = std::numeric_limits<double>::quiet_NaN(),
       .is_complex = FALSE,
       .expected = GDT_Float64},
      {.dt = GDT_Float64,
       .value = std::numeric_limits<double>::infinity(),
       .is_complex = FALSE,
       .expected = GDT_Float64},
      {.dt = GDT_Float64,
       .value = -std::numeric_limits<double>::infinity(),
       .is_complex = FALSE,
       .expected = GDT_Float64},
      // Additional branches: GDT_Float16, complex, non-exact float32.
      {.dt = GDT_Float16,
       .value = 1.5,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.dt = GDT_Byte,
       .value = 0.5,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.dt = GDT_Byte,
       .value = 0.1,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      {.dt = GDT_Byte,
       .value = 1.0,
       .is_complex = TRUE,
       .expected = GDT_CInt32},
      {.dt = GDT_CInt16,
       .value = 0.5,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(c.expected,
              GDALDataTypeUnionWithValue(c.dt, c.value, c.is_complex));
  }
}

TEST(GdalMiscTest, GDALFindDataType) {
  struct FindDataTypeCase {
    int bits;
    int is_signed;
    int is_floating;
    int is_complex;
    GDALDataType expected;
  };
  constexpr FindDataTypeCase kCases[] = {
      // Unsigned int real.
      {.bits = -1,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Byte},
      {.bits = 0,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Byte},
      {.bits = 8,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Byte},
      {.bits = 9,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_UInt16},
      {.bits = 16,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_UInt16},
      {.bits = 17,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_UInt32},
      {.bits = 32,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_UInt32},
      {.bits = 33,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_UInt64},
      {.bits = 63,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_UInt64},
      {.bits = 64,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_UInt64},
      {.bits = 65,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      // Signed int real.
      {.bits = 0,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Int8},
      {.bits = 8,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Int8},
      {.bits = 9,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Int16},
      {.bits = 16,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Int16},
      {.bits = 17,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Int32},
      {.bits = 32,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Int32},
      {.bits = 33,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Int64},
      {.bits = 63,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Int64},
      {.bits = 64,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Int64},
      {.bits = 65,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      // Unsigned floating point real.
      {.bits = 0,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 8,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 9,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 16,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 17,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 32,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 33,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      {.bits = 63,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      {.bits = 64,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      {.bits = 65,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      // Signed floating point real.
      {.bits = 0,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 8,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 9,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 16,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 17,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 32,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float32},
      {.bits = 33,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      {.bits = 63,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      {.bits = 64,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      {.bits = 65,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = FALSE,
       .expected = GDT_Float64},
      // Unsigned and floating point and complex.
      {.bits = 0,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 8,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 9,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 16,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 17,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 32,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 33,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 63,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 64,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 65,
       .is_signed = FALSE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      // Signed and floating point and complex.
      {.bits = -1,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 0,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 8,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 9,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 16,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 17,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 32,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat32},
      {.bits = 33,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 63,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 64,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 65,
       .is_signed = TRUE,
       .is_floating = TRUE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      // Signed and int and complex.
      {.bits = -1,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt16},
      {.bits = 0,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt16},
      {.bits = 8,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt16},
      {.bits = 9,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt16},
      {.bits = 16,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt16},
      {.bits = 17,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt32},
      {.bits = 32,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt32},
      {.bits = 33,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 63,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 64,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 65,
       .is_signed = TRUE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      // Unsigned and int and complex.
      // TODO(schwehr): I want a pony.
      {.bits = 0,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt32},
      {.bits = 8,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt32},
      {.bits = 9,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt32},
      {.bits = 16,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt32},
      {.bits = 17,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CInt32},
      {.bits = 32,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 33,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 63,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 64,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
      {.bits = 65,
       .is_signed = FALSE,
       .is_floating = FALSE,
       .is_complex = TRUE,
       .expected = GDT_CFloat64},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(c.expected, GDALFindDataType(c.bits, c.is_signed, c.is_floating,
                                           c.is_complex));
  }
}

TEST(GdalMiscTest, GDALFindDataTypeForValue) {
  struct FindDataTypeForValueCase {
    double value;
    int is_complex;
    GDALDataType expected;
  };
  constexpr FindDataTypeForValueCase kCases[] = {
      {.value = 1.0, .is_complex = FALSE, .expected = GDT_Byte},
      {.value = -1.0, .is_complex = FALSE, .expected = GDT_Int8},
      {.value = -200.0, .is_complex = FALSE, .expected = GDT_Int16},
      {.value = 20000.0, .is_complex = FALSE, .expected = GDT_UInt16},
      {.value = 40000.0, .is_complex = FALSE, .expected = GDT_UInt16},
      {.value = -40000.0, .is_complex = FALSE, .expected = GDT_Int32},
      {.value = 3000000000.0, .is_complex = FALSE, .expected = GDT_UInt32},
      {.value = 5000000000.0, .is_complex = FALSE, .expected = GDT_UInt64},
      {.value = -5000000000.0, .is_complex = FALSE, .expected = GDT_Int64},
      {.value = 0.5, .is_complex = FALSE, .expected = GDT_Float32},
      {.value = 0.1, .is_complex = FALSE, .expected = GDT_Float64},
      {.value = 1e30, .is_complex = FALSE, .expected = GDT_Float64},
      {.value = -1e30, .is_complex = FALSE, .expected = GDT_Float64},
      {.value = 0.5, .is_complex = TRUE, .expected = GDT_CFloat32},
      {.value = 0.1, .is_complex = TRUE, .expected = GDT_CFloat64},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(c.expected, GDALFindDataTypeForValue(c.value, c.is_complex));
  }
}

struct DataTypeIntCase {
  GDALDataType dt;
  int expected;
};

struct DataTypeBoolCase {
  GDALDataType dt;
  bool expected;
};

TEST(GdalMiscTest, GDALGetDataTypeSizeBytes) {
  constexpr DataTypeIntCase kCases[] = {
      {.dt = GDT_Unknown, .expected = 0},  {.dt = GDT_TypeCount, .expected = 0},
      {.dt = GDT_Byte, .expected = 1},     {.dt = GDT_Int8, .expected = 1},
      {.dt = GDT_UInt16, .expected = 2},   {.dt = GDT_Int16, .expected = 2},
      {.dt = GDT_Float16, .expected = 2},  {.dt = GDT_UInt32, .expected = 4},
      {.dt = GDT_Int32, .expected = 4},    {.dt = GDT_Float32, .expected = 4},
      {.dt = GDT_CInt16, .expected = 4},   {.dt = GDT_CFloat16, .expected = 4},
      {.dt = GDT_Float64, .expected = 8},  {.dt = GDT_CInt32, .expected = 8},
      {.dt = GDT_CFloat32, .expected = 8}, {.dt = GDT_UInt64, .expected = 8},
      {.dt = GDT_Int64, .expected = 8},    {.dt = GDT_CFloat64, .expected = 16},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(c.expected, GDALGetDataTypeSizeBytes(c.dt));
  }
}

TEST(GdalMiscTest, GDALGetDataTypeSizeBits) {
  constexpr DataTypeIntCase kCases[] = {
      {.dt = GDT_Unknown, .expected = 0},
      {.dt = GDT_Byte, .expected = 1 * 8},
      {.dt = GDT_UInt16, .expected = 2 * 8},
      {.dt = GDT_Int16, .expected = 2 * 8},
      {.dt = GDT_UInt32, .expected = 4 * 8},
      {.dt = GDT_Int32, .expected = 4 * 8},
      {.dt = GDT_Float32, .expected = 4 * 8},
      {.dt = GDT_CInt16, .expected = 4 * 8},
      {.dt = GDT_Float64, .expected = 8 * 8},
      {.dt = GDT_CInt32, .expected = 8 * 8},
      {.dt = GDT_CFloat32, .expected = 8 * 8},
      {.dt = GDT_CFloat64, .expected = 16 * 8},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(c.expected, GDALGetDataTypeSizeBits(c.dt));
  }
}

TEST(GdalMiscTest, GdalDataTypeSize) {
  // Tests getting the size in bits.
  constexpr DataTypeIntCase kCases[] = {
      {.dt = GDT_Unknown, .expected = 0},
      {.dt = GDT_Byte, .expected = 8},
      {.dt = GDT_UInt16, .expected = 16},
      {.dt = GDT_Int32, .expected = 32},
      {.dt = GDT_CInt16, .expected = 32},
      {.dt = GDT_CInt32, .expected = 64},
      {.dt = GDT_Float32, .expected = 32},
      {.dt = GDT_Float64, .expected = 64},
      {.dt = GDT_CFloat32, .expected = 64},
      {.dt = GDT_CFloat64, .expected = 128},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(c.expected, GDALGetDataTypeSize(c.dt));
  }
}

TEST(GdalMiscTest, GdalDataTypeIsComplex) {
  constexpr DataTypeBoolCase kCases[] = {
      {.dt = GDT_Unknown, .expected = false},
      {.dt = GDT_Byte, .expected = false},
      {.dt = GDT_Int8, .expected = false},
      {.dt = GDT_UInt16, .expected = false},
      {.dt = GDT_Int16, .expected = false},
      {.dt = GDT_Int32, .expected = false},
      {.dt = GDT_UInt32, .expected = false},
      {.dt = GDT_Int64, .expected = false},
      {.dt = GDT_UInt64, .expected = false},
      {.dt = GDT_Float16, .expected = false},
      {.dt = GDT_Float32, .expected = false},
      {.dt = GDT_Float64, .expected = false},
      {.dt = GDT_CInt16, .expected = true},
      {.dt = GDT_CInt32, .expected = true},
      {.dt = GDT_CFloat16, .expected = true},
      {.dt = GDT_CFloat32, .expected = true},
      {.dt = GDT_CFloat64, .expected = true},
      {.dt = GDT_TypeCount, .expected = false},
  };
  for (const auto& c : kCases) {
    if (c.expected) {
      EXPECT_TRUE(GDALDataTypeIsComplex(c.dt));
    } else {
      EXPECT_FALSE(GDALDataTypeIsComplex(c.dt));
    }
  }
}

TEST(GdalMiscTest, GdalDataTypeIsFloating) {
  constexpr DataTypeBoolCase kCases[] = {
      {.dt = GDT_Unknown, .expected = false},
      {.dt = GDT_Byte, .expected = false},
      {.dt = GDT_Int8, .expected = false},
      {.dt = GDT_UInt16, .expected = false},
      {.dt = GDT_Int16, .expected = false},
      {.dt = GDT_UInt32, .expected = false},
      {.dt = GDT_Int32, .expected = false},
      {.dt = GDT_UInt64, .expected = false},
      {.dt = GDT_Int64, .expected = false},
      {.dt = GDT_Float16, .expected = true},
      {.dt = GDT_Float32, .expected = true},
      {.dt = GDT_Float64, .expected = true},
      {.dt = GDT_CInt16, .expected = false},
      {.dt = GDT_CInt32, .expected = false},
      {.dt = GDT_CFloat16, .expected = true},
      {.dt = GDT_CFloat32, .expected = true},
      {.dt = GDT_CFloat64, .expected = true},
      {.dt = GDT_TypeCount, .expected = false},
  };
  for (const auto& c : kCases) {
    if (c.expected) {
      EXPECT_TRUE(GDALDataTypeIsFloating(c.dt));
    } else {
      EXPECT_FALSE(GDALDataTypeIsFloating(c.dt));
    }
  }
}

TEST(GdalMiscTest, GdalDataTypeIsInteger) {
  constexpr DataTypeBoolCase kCases[] = {
      {.dt = GDT_Unknown, .expected = false},
      {.dt = GDT_Byte, .expected = true},
      {.dt = GDT_Int8, .expected = true},
      {.dt = GDT_UInt16, .expected = true},
      {.dt = GDT_Int16, .expected = true},
      {.dt = GDT_UInt32, .expected = true},
      {.dt = GDT_Int32, .expected = true},
      {.dt = GDT_UInt64, .expected = true},
      {.dt = GDT_Int64, .expected = true},
      {.dt = GDT_Float16, .expected = false},
      {.dt = GDT_Float32, .expected = false},
      {.dt = GDT_Float64, .expected = false},
      {.dt = GDT_CInt16, .expected = true},
      {.dt = GDT_CInt32, .expected = true},
      {.dt = GDT_CFloat16, .expected = false},
      {.dt = GDT_CFloat32, .expected = false},
      {.dt = GDT_CFloat64, .expected = false},
      {.dt = GDT_TypeCount, .expected = false},
  };
  for (const auto& c : kCases) {
    if (c.expected) {
      EXPECT_TRUE(GDALDataTypeIsInteger(c.dt));
    } else {
      EXPECT_FALSE(GDALDataTypeIsInteger(c.dt));
    }
  }
}

TEST(GdalMiscTest, GdalDataTypeIsSigned) {
  constexpr DataTypeBoolCase kCases[] = {
      {.dt = GDT_Unknown, .expected = false},
      {.dt = GDT_Byte, .expected = false},
      {.dt = GDT_Int8, .expected = true},
      {.dt = GDT_UInt16, .expected = false},
      {.dt = GDT_Int16, .expected = true},
      {.dt = GDT_UInt32, .expected = false},
      {.dt = GDT_Int32, .expected = true},
      {.dt = GDT_UInt64, .expected = false},
      {.dt = GDT_Int64, .expected = true},
      {.dt = GDT_Float16, .expected = true},
      {.dt = GDT_Float32, .expected = true},
      {.dt = GDT_Float64, .expected = true},
      {.dt = GDT_CInt16, .expected = true},
      {.dt = GDT_CInt32, .expected = true},
      {.dt = GDT_CFloat16, .expected = true},
      {.dt = GDT_CFloat32, .expected = true},
      {.dt = GDT_CFloat64, .expected = true},
      {.dt = GDT_TypeCount, .expected = false},
  };
  for (const auto& c : kCases) {
    if (c.expected) {
      EXPECT_TRUE(GDALDataTypeIsSigned(c.dt));
    } else {
      EXPECT_FALSE(GDALDataTypeIsSigned(c.dt));
    }
  }
}

TEST(GdalMiscTest, GDALDataTypeIsConversionLossy) {
  struct LossyCase {
    GDALDataType from;
    GDALDataType to;
    bool expected;
  };
  constexpr LossyCase kCases[] = {
      // Complex to non-complex is lossy.
      {.from = GDT_CFloat32, .to = GDT_Float32, .expected = true},
      {.from = GDT_CInt16, .to = GDT_Int16, .expected = true},
      // To integer:
      {.from = GDT_Float32, .to = GDT_Int32, .expected = true},
      {.from = GDT_Int16, .to = GDT_UInt16, .expected = true},
      {.from = GDT_UInt32, .to = GDT_UInt16, .expected = true},
      {.from = GDT_UInt16, .to = GDT_Int16, .expected = true},
      {.from = GDT_Byte, .to = GDT_Int16, .expected = false},
      {.from = GDT_Byte, .to = GDT_UInt16, .expected = false},
      // To Float16:
      {.from = GDT_Int16, .to = GDT_Float16, .expected = true},
      {.from = GDT_UInt16, .to = GDT_Float16, .expected = true},
      {.from = GDT_Int32, .to = GDT_Float16, .expected = true},
      {.from = GDT_UInt32, .to = GDT_Float16, .expected = true},
      {.from = GDT_Int64, .to = GDT_Float16, .expected = true},
      {.from = GDT_UInt64, .to = GDT_Float16, .expected = true},
      {.from = GDT_Float32, .to = GDT_Float16, .expected = true},
      {.from = GDT_Float64, .to = GDT_Float16, .expected = true},
      {.from = GDT_Byte, .to = GDT_Float16, .expected = false},
      // To Float32:
      {.from = GDT_Int32, .to = GDT_Float32, .expected = true},
      {.from = GDT_UInt32, .to = GDT_Float32, .expected = true},
      {.from = GDT_Int64, .to = GDT_Float32, .expected = true},
      {.from = GDT_UInt64, .to = GDT_Float32, .expected = true},
      {.from = GDT_Float64, .to = GDT_Float32, .expected = true},
      {.from = GDT_Int16, .to = GDT_Float32, .expected = false},
      // To Float64:
      {.from = GDT_Int64, .to = GDT_Float64, .expected = true},
      {.from = GDT_UInt64, .to = GDT_Float64, .expected = true},
      {.from = GDT_Int32, .to = GDT_Float64, .expected = false},
  };
  for (const auto& c : kCases) {
    if (c.expected) {
      EXPECT_TRUE(GDALDataTypeIsConversionLossy(c.from, c.to));
    } else {
      EXPECT_FALSE(GDALDataTypeIsConversionLossy(c.from, c.to));
    }
  }
}

TEST(GdalMiscTest, GdalGetDataTypeNameAndByName) {
  struct DataTypeNameCase {
    GDALDataType dt;
    const char* name;
  };
  constexpr DataTypeNameCase kCases[] = {
      {.dt = GDT_Unknown, .name = "Unknown"},
      {.dt = GDT_Byte, .name = "Byte"},
      {.dt = GDT_Int8, .name = "Int8"},
      {.dt = GDT_UInt16, .name = "UInt16"},
      {.dt = GDT_Int16, .name = "Int16"},
      {.dt = GDT_UInt32, .name = "UInt32"},
      {.dt = GDT_Int32, .name = "Int32"},
      {.dt = GDT_UInt64, .name = "UInt64"},
      {.dt = GDT_Int64, .name = "Int64"},
      {.dt = GDT_Float16, .name = "Float16"},
      {.dt = GDT_Float32, .name = "Float32"},
      {.dt = GDT_Float64, .name = "Float64"},
      {.dt = GDT_CInt16, .name = "CInt16"},
      {.dt = GDT_CInt32, .name = "CInt32"},
      {.dt = GDT_CFloat16, .name = "CFloat16"},
      {.dt = GDT_CFloat32, .name = "CFloat32"},
      {.dt = GDT_CFloat64, .name = "CFloat64"},
  };
  for (const auto& c : kCases) {
    EXPECT_STREQ(c.name, GDALGetDataTypeName(c.dt));
  }
  EXPECT_EQ(nullptr, GDALGetDataTypeName(GDT_TypeCount));

  for (int i = 1; i < GDT_TypeCount; ++i) {
    const auto eType = static_cast<GDALDataType>(i);
    EXPECT_EQ(eType, GDALGetDataTypeByName(GDALGetDataTypeName(eType)));
  }
  EXPECT_EQ(GDT_Unknown, GDALGetDataTypeByName("NotAType"));

  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(GDT_Unknown, GDALGetDataTypeByName(nullptr));
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();
}

// Based on autotest/cpp/test_gdal.cpp.
TEST(GdalMiscTest, GdalAdjustValueToDataType) {
  // GDAL uses ints as bools for its C API.
  int bClamped = FALSE;
  int bRounded = FALSE;

  EXPECT_DOUBLE_EQ(
      255.0, GDALAdjustValueToDataType(GDT_Byte, 255.0, nullptr, nullptr));

  struct AdjustCase {
    GDALDataType dt;
    double input;
    double expected;
    int expect_clamped;
    std::optional<int> expect_rounded;
  };
  const AdjustCase kCases[] = {
      {.dt = GDT_Byte,
       .input = 255.0,
       .expected = 255.0,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      {.dt = GDT_Byte,
       .input = 254.4,
       .expected = 254.0,
       .expect_clamped = FALSE,
       .expect_rounded = TRUE},
      {.dt = GDT_Byte,
       .input = -1.0,
       .expected = 0.0,
       .expect_clamped = TRUE,
       .expect_rounded = FALSE},
      {.dt = GDT_Byte,
       .input = 256.0,
       .expected = 255.0,
       .expect_clamped = TRUE,
       .expect_rounded = FALSE},
      // Int8
      {.dt = GDT_Int8,
       .input = -200.0,
       .expected = -128.0,
       .expect_clamped = TRUE,
       .expect_rounded = std::nullopt},
      {.dt = GDT_Int8,
       .input = 200.0,
       .expected = 127.0,
       .expect_clamped = TRUE,
       .expect_rounded = std::nullopt},
      // UInt16
      {.dt = GDT_UInt16,
       .input = 65535.0,
       .expected = 65535.0,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      {.dt = GDT_UInt16,
       .input = 65534.4,
       .expected = 65534.0,
       .expect_clamped = FALSE,
       .expect_rounded = TRUE},
      {.dt = GDT_UInt16,
       .input = -1.0,
       .expected = 0.0,
       .expect_clamped = TRUE,
       .expect_rounded = FALSE},
      {.dt = GDT_UInt16,
       .input = 65536.0,
       .expected = 65535.0,
       .expect_clamped = TRUE,
       .expect_rounded = FALSE},
      // Int16
      {.dt = GDT_Int16,
       .input = -32768.0,
       .expected = -32768.0,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      {.dt = GDT_Int16,
       .input = 32767.0,
       .expected = 32767.0,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      {.dt = GDT_Int16,
       .input = -32767.4,
       .expected = -32767.0,
       .expect_clamped = FALSE,
       .expect_rounded = TRUE},
      {.dt = GDT_Int16,
       .input = 32766.4,
       .expected = 32766.0,
       .expect_clamped = FALSE,
       .expect_rounded = TRUE},
      {.dt = GDT_Int16,
       .input = -32769.0,
       .expected = -32768.0,
       .expect_clamped = TRUE,
       .expect_rounded = FALSE},
      {.dt = GDT_Int16,
       .input = 32768.0,
       .expected = 32767.0,
       .expect_clamped = TRUE,
       .expect_rounded = FALSE},
      // UInt32
      {.dt = GDT_UInt32,
       .input = 10000000.0,
       .expected = 10000000.0,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      {.dt = GDT_UInt32,
       .input = 10000000.4,
       .expected = 10000000.0,
       .expect_clamped = FALSE,
       .expect_rounded = TRUE},
      {.dt = GDT_UInt32,
       .input = -1.0,
       .expected = 0.0,
       .expect_clamped = TRUE,
       .expect_rounded = FALSE},
      // Int32
      {.dt = GDT_Int32,
       .input = -10000000.0,
       .expected = -10000000.0,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      {.dt = GDT_Int32,
       .input = 10000000.0,
       .expected = 10000000.0,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      // Int64 / UInt64
      {.dt = GDT_Int64,
       .input = 100.0,
       .expected = 100.0,
       .expect_clamped = FALSE,
       .expect_rounded = std::nullopt},
      {.dt = GDT_UInt64,
       .input = 100.0,
       .expected = 100.0,
       .expect_clamped = FALSE,
       .expect_rounded = std::nullopt},
      // Float16
      {.dt = GDT_Float16,
       .input = 1.5,
       .expected = 1.5,
       .expect_clamped = FALSE,
       .expect_rounded = std::nullopt},
      {.dt = GDT_Float16,
       .input = -1e10,
       .expected = static_cast<double>(cpl::NumericLimits<GFloat16>::lowest()),
       .expect_clamped = TRUE,
       .expect_rounded = std::nullopt},
      {.dt = GDT_Float16,
       .input = 1e10,
       .expected = static_cast<double>(cpl::NumericLimits<GFloat16>::max()),
       .expect_clamped = TRUE,
       .expect_rounded = std::nullopt},
      // Float32
      {.dt = GDT_Float32,
       .input = 1e300,
       .expected = std::numeric_limits<float>::max(),
       .expect_clamped = TRUE,
       .expect_rounded = FALSE},
      {.dt = GDT_Float32,
       .input = -1e300,
       .expected = std::numeric_limits<float>::lowest(),
       .expect_clamped = TRUE,
       .expect_rounded = FALSE},
      // Float64
      {.dt = GDT_Float64,
       .input = 0.0,
       .expected = 0.0,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      {.dt = GDT_Float64,
       .input = 1e-50,
       .expected = 1e-50,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      {.dt = GDT_Float64,
       .input = -1e40,
       .expected = -1e40,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      {.dt = GDT_Float64,
       .input = 1e40,
       .expected = 1e40,
       .expect_clamped = FALSE,
       .expect_rounded = FALSE},
      // Complex
      {.dt = GDT_CInt16,
       .input = 1.0,
       .expected = 1.0,
       .expect_clamped = FALSE,
       .expect_rounded = std::nullopt},
  };
  for (const auto& c : kCases) {
    EXPECT_DOUBLE_EQ(c.expected, GDALAdjustValueToDataType(
                                     c.dt, c.input, &bClamped, &bRounded));
    EXPECT_EQ(c.expect_clamped, bClamped);
    if (c.expect_rounded.has_value()) {
      EXPECT_EQ(*c.expect_rounded, bRounded);
    }
  }

  EXPECT_TRUE(std::isnan(GDALAdjustValueToDataType(
      GDT_Float16, std::numeric_limits<double>::quiet_NaN(), &bClamped,
      &bRounded)));

  EXPECT_NEAR(
      1.23, GDALAdjustValueToDataType(GDT_Float32, 1.23, &bClamped, &bRounded),
      0.000001);
  EXPECT_EQ(FALSE, bClamped);
  EXPECT_EQ(FALSE, bRounded);

  EXPECT_TRUE(std::isinf(GDALAdjustValueToDataType(
      GDT_Float32, std::numeric_limits<double>::infinity(), &bClamped,
      &bRounded)));
}

TEST(GdalMiscTest, GDALIsValueExactAsAndInRangeOf) {
  for (int i = GDT_Unknown; i <= GDT_TypeCount; ++i) {
    const auto eDT = static_cast<GDALDataType>(i);
    EXPECT_TRUE(GDALIsValueExactAs(1.0, eDT));
    EXPECT_TRUE(GDALIsValueInRangeOf(1.0, eDT));
  }
  EXPECT_FALSE(GDALIsValueExactAs(1.5, GDT_Byte));
  EXPECT_TRUE(GDALIsValueInRangeOf(1.5, GDT_Byte));
  EXPECT_FALSE(GDALIsValueInRangeOf(300.0, GDT_Byte));
}

TEST(GdalMiscTest, GDALGetNonComplexDataType) {
  struct NonComplexCase {
    GDALDataType input;
    GDALDataType expected;
  };
  constexpr NonComplexCase kCases[] = {
      {.input = GDT_Unknown, .expected = GDT_Unknown},
      {.input = GDT_Byte, .expected = GDT_Byte},
      {.input = GDT_Int8, .expected = GDT_Int8},
      {.input = GDT_UInt16, .expected = GDT_UInt16},
      {.input = GDT_Int16, .expected = GDT_Int16},
      {.input = GDT_UInt32, .expected = GDT_UInt32},
      {.input = GDT_Int32, .expected = GDT_Int32},
      {.input = GDT_UInt64, .expected = GDT_UInt64},
      {.input = GDT_Int64, .expected = GDT_Int64},
      {.input = GDT_Float16, .expected = GDT_Float16},
      {.input = GDT_Float32, .expected = GDT_Float32},
      {.input = GDT_Float64, .expected = GDT_Float64},
      {.input = GDT_CInt16, .expected = GDT_Int16},
      {.input = GDT_CInt32, .expected = GDT_Int32},
      {.input = GDT_CFloat16, .expected = GDT_Float16},
      {.input = GDT_CFloat32, .expected = GDT_Float32},
      {.input = GDT_CFloat64, .expected = GDT_Float64},
      {.input = GDT_TypeCount, .expected = GDT_TypeCount},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(c.expected, GDALGetNonComplexDataType(c.input));
  }
}

TEST(GdalMiscTest, GDALGetAsyncStatusTypeByName) {
  struct AsyncStatusByNameCase {
    const char* name;
    GDALAsyncStatusType expected;
  };
  constexpr AsyncStatusByNameCase kCases[] = {
      {.name = "PENDING", .expected = GARIO_PENDING},
      {.name = "UPDATE", .expected = GARIO_UPDATE},
      {.name = "ERROR", .expected = GARIO_ERROR},
      {.name = "COMPLETE", .expected = GARIO_COMPLETE},
      {.name = "Garbage", .expected = GARIO_ERROR},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(c.expected, GDALGetAsyncStatusTypeByName(c.name));
  }

  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(GARIO_ERROR, GDALGetAsyncStatusTypeByName(nullptr));
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST(GdalMiscTest, GDALGetAsyncStatusTypeName) {
  struct AsyncStatusNameCase {
    GDALAsyncStatusType status;
    const char* name;
  };
  constexpr AsyncStatusNameCase kCases[] = {
      {.status = GARIO_PENDING, .name = "PENDING"},
      {.status = GARIO_UPDATE, .name = "UPDATE"},
      {.status = GARIO_ERROR, .name = "ERROR"},
      {.status = GARIO_COMPLETE, .name = "COMPLETE"},
  };
  for (const auto& c : kCases) {
    EXPECT_STREQ(c.name, GDALGetAsyncStatusTypeName(c.status));
  }
  EXPECT_EQ(nullptr, GDALGetAsyncStatusTypeName(GARIO_TypeCount));
}

TEST(GdalMiscTest, GDALGetPaletteInterpretationName) {
  struct PaletteNameCase {
    GDALPaletteInterp interp;
    const char* name;
  };
  constexpr PaletteNameCase kCases[] = {
      {.interp = GPI_Gray, .name = "Gray"},
      {.interp = GPI_RGB, .name = "RGB"},
      {.interp = GPI_CMYK, .name = "CMYK"},
      {.interp = GPI_HLS, .name = "HLS"},
  };
  for (const auto& c : kCases) {
    EXPECT_STREQ(c.name, GDALGetPaletteInterpretationName(c.interp));
  }
#if !defined(UNDEFINED_BEHAVIOR_SANITIZER)
  EXPECT_STREQ("Unknown", GDALGetPaletteInterpretationName(
                              static_cast<GDALPaletteInterp>(9999)));
#endif
}

TEST(GdalMiscTest, GDALGetColorInterpretationName) {
  struct ColorInterpNameCase {
    GDALColorInterp interp;
    const char* name;
  };
  constexpr ColorInterpNameCase kCases[] = {
      {.interp = GCI_Undefined, .name = "Undefined"},
      {.interp = GCI_GrayIndex, .name = "Gray"},
      {.interp = GCI_PaletteIndex, .name = "Palette"},
      {.interp = GCI_RedBand, .name = "Red"},
      {.interp = GCI_GreenBand, .name = "Green"},
      {.interp = GCI_BlueBand, .name = "Blue"},
      {.interp = GCI_AlphaBand, .name = "Alpha"},
      {.interp = GCI_HueBand, .name = "Hue"},
      {.interp = GCI_SaturationBand, .name = "Saturation"},
      {.interp = GCI_LightnessBand, .name = "Lightness"},
      {.interp = GCI_CyanBand, .name = "Cyan"},
      {.interp = GCI_MagentaBand, .name = "Magenta"},
      {.interp = GCI_YellowBand, .name = "Yellow"},
      {.interp = GCI_BlackBand, .name = "Black"},
      {.interp = GCI_YCbCr_YBand, .name = "YCbCr_Y"},
      {.interp = GCI_YCbCr_CbBand, .name = "YCbCr_Cb"},
      {.interp = GCI_YCbCr_CrBand, .name = "YCbCr_Cr"},
      {.interp = GCI_PanBand, .name = "Pan"},
      {.interp = GCI_CoastalBand, .name = "Coastal"},
      {.interp = GCI_RedEdgeBand, .name = "RedEdge"},
      {.interp = GCI_NIRBand, .name = "NIR"},
      {.interp = GCI_SWIRBand, .name = "SWIR"},
      {.interp = GCI_MWIRBand, .name = "MWIR"},
      {.interp = GCI_LWIRBand, .name = "LWIR"},
      {.interp = GCI_TIRBand, .name = "TIR"},
      {.interp = GCI_OtherIRBand, .name = "OtherIR"},
      {.interp = GCI_IR_Reserved_1, .name = "IR_Reserved_1"},
      {.interp = GCI_IR_Reserved_2, .name = "IR_Reserved_2"},
      {.interp = GCI_IR_Reserved_3, .name = "IR_Reserved_3"},
      {.interp = GCI_IR_Reserved_4, .name = "IR_Reserved_4"},
      {.interp = GCI_SAR_Ka_Band, .name = "SAR_Ka"},
      {.interp = GCI_SAR_K_Band, .name = "SAR_K"},
      {.interp = GCI_SAR_Ku_Band, .name = "SAR_Ku"},
      {.interp = GCI_SAR_X_Band, .name = "SAR_X"},
      {.interp = GCI_SAR_C_Band, .name = "SAR_C"},
      {.interp = GCI_SAR_S_Band, .name = "SAR_S"},
      {.interp = GCI_SAR_L_Band, .name = "SAR_L"},
      {.interp = GCI_SAR_P_Band, .name = "SAR_P"},
      {.interp = GCI_SAR_Reserved_1, .name = "SAR_Reserved_1"},
      {.interp = GCI_SAR_Reserved_2, .name = "SAR_Reserved_2"},
  };
  for (const auto& c : kCases) {
    EXPECT_STREQ(c.name, GDALGetColorInterpretationName(c.interp));
  }
#if !defined(UNDEFINED_BEHAVIOR_SANITIZER)
  // Undefined behavior sanitizer gives:
  // runtime error: load of value 9999, which is not a valid value for type
  // 'GDALColorInterp'
  EXPECT_STREQ("Undefined", GDALGetColorInterpretationName(
                                static_cast<GDALColorInterp>(9999)));
#endif
}

TEST(GdalMiscTest, GDALGetColorInterpretationByNameAndSTAC) {
  struct ColorInterpByNameCase {
    const char* name;
    GDALColorInterp expected;
  };
  constexpr ColorInterpByNameCase kNameCases[] = {
      {.name = "Undefined", .expected = GCI_Undefined},
      {.name = "gray", .expected = GCI_GrayIndex},
      {.name = "grey", .expected = GCI_GrayIndex},
      {.name = "Does not exist", .expected = GCI_Undefined},
  };
  for (const auto& c : kNameCases) {
    EXPECT_EQ(c.expected, GDALGetColorInterpretationByName(c.name));
  }

  for (int i = 0; i <= GCI_Max; ++i) {
    const auto eInterp = static_cast<GDALColorInterp>(i);
    EXPECT_EQ(eInterp, GDALGetColorInterpretationByName(
                           GDALGetColorInterpretationName(eInterp)));
  }

  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(GCI_Undefined, GDALGetColorInterpretationByName(nullptr));
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();

  constexpr ColorInterpByNameCase kStacCases[] = {
      {.name = "pan", .expected = GCI_PanBand},
      {.name = "green05", .expected = GCI_GreenBand},
      {.name = "swir16", .expected = GCI_SWIRBand},
      {.name = "nonexistent", .expected = GCI_Undefined},
  };
  for (const auto& c : kStacCases) {
    EXPECT_EQ(c.expected, GDALGetColorInterpFromSTACCommonName(c.name));
  }

  EXPECT_STREQ("pan", GDALGetSTACCommonNameFromColorInterp(GCI_PanBand));
  EXPECT_EQ(nullptr, GDALGetSTACCommonNameFromColorInterp(GCI_SWIRBand));
  EXPECT_EQ(nullptr, GDALGetSTACCommonNameFromColorInterp(GCI_Undefined));
}

TEST(GdalMiscTest, GDALGetRandomRasterSample) {
  GDALRegister_MEM();
  GDALRegister_GTiff();
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kVsiMemDir); };

  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(0, GDALGetRandomRasterSample(nullptr, 10, nullptr));
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();

  GDALDriver* poMemDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDrv);

  for (int i = 1; i < GDT_TypeCount; ++i) {
    const auto eDT = static_cast<GDALDataType>(i);
    GDALDatasetUniquePtr poDS(poMemDrv->Create("", 10, 10, 1, eDT, nullptr));
    ASSERT_NE(nullptr, poDS);
    GDALRasterBand* poBand = poDS->GetRasterBand(1);
    ASSERT_NE(nullptr, poBand);
    EXPECT_EQ(CE_None, poBand->Fill(5.0, 2.0));
    if (eDT == GDT_Byte) {
      EXPECT_EQ(CE_None, poBand->SetNoDataValue(5.0));
    }
    std::array<float, 20> afSamples{};
    const int nGot = GDALGetRandomRasterSample(GDALRasterBand::ToHandle(poBand),
                                               10, afSamples.data());
    if (eDT == GDT_Byte) {
      EXPECT_EQ(0, nGot);
    } else {
      EXPECT_GT(nGot, 0);
    }
  }

  // Test multi-block tiled GTiff with partial edge blocks.
  GDALDriver* poGTiffDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poGTiffDrv);
  CPLStringList aosCreationOpts;
  aosCreationOpts.SetNameValue("TILED", "YES");
  aosCreationOpts.SetNameValue("BLOCKXSIZE", "16");
  aosCreationOpts.SetNameValue("BLOCKYSIZE", "16");
  constexpr char kTiledFile[] =
      "/vsimem/gdal_misc_test/random_sample_tiled.tif";
  GDALDatasetUniquePtr poDS(poGTiffDrv->Create(kTiledFile, 50, 50, 1, GDT_Byte,
                                               aosCreationOpts.List()));
  ASSERT_NE(nullptr, poDS);
  GDALRasterBand* poBand = poDS->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);
  EXPECT_EQ(CE_None, poBand->Fill(42));
  std::array<float, 100> afSamples{};
  EXPECT_GT(GDALGetRandomRasterSample(GDALRasterBand::ToHandle(poBand), 50,
                                      afSamples.data()),
            0);
  poDS.reset();
}

TEST(GdalMiscTest, GCPFunctions) {
  gdal::GCP gcp1("id1", "info1", 1.0, 2.0, 3.0, 4.0, 5.0);
  EXPECT_STREQ("id1", gcp1.Id());
  EXPECT_STREQ("info1", gcp1.Info());
  EXPECT_DOUBLE_EQ(1.0, gcp1.Pixel());
  EXPECT_DOUBLE_EQ(2.0, gcp1.Line());
  EXPECT_DOUBLE_EQ(3.0, gcp1.X());
  EXPECT_DOUBLE_EQ(4.0, gcp1.Y());
  EXPECT_DOUBLE_EQ(5.0, gcp1.Z());

  gdal::GCP gcpNull(nullptr, nullptr);
  EXPECT_STREQ("", gcpNull.Id());
  EXPECT_STREQ("", gcpNull.Info());
  gcpNull.SetId(nullptr);
  gcpNull.SetInfo(nullptr);
  gcpNull.SetId("id2");
  gcpNull.SetInfo("info2");
  EXPECT_STREQ("id2", gcpNull.Id());
  EXPECT_STREQ("info2", gcpNull.Info());

  gdal::GCP gcpCopy(gcp1);
  gdal::GCP gcpFromC(*gcp1.c_ptr());
  gcpCopy = gcpNull;
  EXPECT_STREQ("id2", gcpCopy.Id());

  gdal::GCP gcpMove(std::move(gcpFromC));
  EXPECT_STREQ("id1", gcpMove.Id());
  gcpCopy = std::move(gcpMove);
  EXPECT_STREQ("id1", gcpCopy.Id());

  std::vector<gdal::GCP> emptyVec;
  EXPECT_EQ(nullptr, gdal::GCP::c_ptr(emptyVec));
  std::vector<gdal::GCP> vec = {gcp1, gcpNull};
  EXPECT_NE(nullptr, gdal::GCP::c_ptr(vec));
  auto vec2 = gdal::GCP::fromC(gdal::GCP::c_ptr(vec), 2);
  EXPECT_EQ(2u, vec2.size());

  GDALInitGCPs(0, nullptr);
  GDALDeinitGCPs(0, nullptr);

  CPLErrorReset();
  {
    WithQuietHandler quiet;
    GDALInitGCPs(1, nullptr);
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    GDALDeinitGCPs(1, nullptr);
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();

  std::array<GDAL_GCP, 2> rawGCPs{};
  GDALInitGCPs(2, rawGCPs.data());
  absl::Cleanup deinit_raw = [&rawGCPs] { GDALDeinitGCPs(2, rawGCPs.data()); };
  CPLFree(rawGCPs[0].pszId);
  rawGCPs[0].pszId = CPLStrdup("raw0");
  rawGCPs[0].dfGCPX = 10.0;

  GDAL_GCP* dupGCPs = GDALDuplicateGCPs(2, rawGCPs.data());
  ASSERT_NE(nullptr, dupGCPs);
  absl::Cleanup free_dup = [dupGCPs] {
    GDALDeinitGCPs(2, dupGCPs);
    CPLFree(dupGCPs);
  };
  EXPECT_STREQ("raw0", dupGCPs[0].pszId);
  EXPECT_DOUBLE_EQ(10.0, dupGCPs[0].dfGCPX);
}

TEST(GdalMiscTest, GDALFindAssociatedFile) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kVsiMemDir); };

  constexpr char kBase[] = "/vsimem/gdal_misc_test/assoc_base.tif";
  constexpr char kUpper[] = "/vsimem/gdal_misc_test/assoc_base.RPB";
  constexpr char kLower[] = "/vsimem/gdal_misc_test/assoc_base2.rpb";
  VsiMemTempWrapper upper_file(kUpper, "");
  VsiMemTempWrapper lower_file(kLower, "");

  // Lowercase ext requested, uppercase exists on disk.
  EXPECT_STREQ(kUpper,
               GDALFindAssociatedFile(kBase, "rpb", nullptr, 0).c_str());
  // Uppercase ext requested, lowercase exists on disk.
  EXPECT_STREQ(kLower,
               GDALFindAssociatedFile("/vsimem/gdal_misc_test/assoc_base2.tif",
                                      "RPB", nullptr, 0)
                   .c_str());
  // Exact match.
  EXPECT_STREQ(kUpper,
               GDALFindAssociatedFile(kBase, "RPB", nullptr, 0).c_str());
  // Not found.
  EXPECT_STREQ("",
               GDALFindAssociatedFile("/vsimem/gdal_misc_test/nonexistent.tif",
                                      "rpb", nullptr, 0)
                   .c_str());

  // With sibling files.
  CPLStringList aosSiblings;
  aosSiblings.AddString("assoc_base.rpb");
  EXPECT_STREQ(
      "/vsimem/gdal_misc_test/assoc_base.rpb",
      GDALFindAssociatedFile(kBase, "rpb", aosSiblings.List(), 0).c_str());
  EXPECT_STREQ(
      "", GDALFindAssociatedFile(kBase, "imd", aosSiblings.List(), 0).c_str());
}

TEST(GdalMiscTest, OziMapFile) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kVsiMemDir); };

  std::array<double, 6> adfGT{};
  char* pszWKT = nullptr;
  int nGCPCount = 0;
  GDAL_GCP* pasGCPs = nullptr;

  constexpr char kNoSuchMap[] = "/vsimem/gdal_misc_test/no_such.map";
  const auto ExpectNullOzi = [](auto&& fn) {
    CPLErrorReset();
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(fn());
    }
    EXPECT_NE(CE_None, CPLGetLastErrorType());
  };
  ExpectNullOzi([&] {
    return GDALLoadOziMapFile(nullptr, adfGT.data(), &pszWKT, &nGCPCount,
                              &pasGCPs);
  });
  ExpectNullOzi([&] {
    return GDALLoadOziMapFile(kNoSuchMap, nullptr, &pszWKT, &nGCPCount,
                              &pasGCPs);
  });
  ExpectNullOzi([&] {
    return GDALLoadOziMapFile(kNoSuchMap, adfGT.data(), &pszWKT, nullptr,
                              &pasGCPs);
  });
  ExpectNullOzi([&] {
    return GDALLoadOziMapFile(kNoSuchMap, adfGT.data(), &pszWKT, &nGCPCount,
                              nullptr);
  });
  CPLErrorReset();

  EXPECT_FALSE(GDALLoadOziMapFile(kNoSuchMap, adfGT.data(), &pszWKT, &nGCPCount,
                                  &pasGCPs));
  EXPECT_FALSE(GDALReadOziMapFile("/vsimem/gdal_misc_test/no_such.tif",
                                  adfGT.data(), &pszWKT, &nGCPCount, &pasGCPs));

  // Invalid header.
  {
    constexpr char kBadMap[] = "/vsimem/gdal_misc_test/bad_ozi.map";
    VsiMemTempWrapper bad_map(kBadMap, "Not an Ozi file\nLine2\n");
    CPLErrorReset();
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(GDALLoadOziMapFile(kBadMap, adfGT.data(), &pszWKT,
                                      &nGCPCount, &pasGCPs));
    }
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Valid header, but 0 points.
  {
    constexpr char kNoPtsMap[] = "/vsimem/gdal_misc_test/nopts_ozi.map";
    VsiMemTempWrapper no_pts_map(kNoPtsMap,
                                 R"ozi(OziExplorer Map Data File Version 2.2
Test
test.tif
1 ,Map Code,
WGS 84,WGS 84,   0.0000,   0.0000,WGS 84
MSF, 0.001
Map Projection,Latitude/Longitude,PolyCal,No,AutoCalOnly,No,BSBUseWPX,No
)ozi");
    char* raw_wkt = nullptr;
    EXPECT_FALSE(GDALLoadOziMapFile(kNoPtsMap, adfGT.data(), &raw_wkt,
                                    &nGCPCount, &pasGCPs));
    std::unique_ptr<char, CplFreeDeleter> wkt(raw_wkt);
  }

  // Valid Ozi with 2 geographic points (forms geotransform).
  {
    constexpr char kGeoMap[] = "/vsimem/gdal_misc_test/geo_ozi.map";
    VsiMemTempWrapper geo_map(kGeoMap,
                              R"ozi(OziExplorer Map Data File Version 2.2
Test
test.tif
1 ,Map Code,
WGS 84,WGS 84,   0.0000,   0.0000,WGS 84
MSF, 2.0
Map Projection,Latitude/Longitude,PolyCal,No,AutoCalOnly,No,BSBUseWPX,No
Projection Setup,     0.000000000,     0.000000000,     1.000000000,       0.00,       0.00,,,,,
Point01,xy,    0,    0,in, deg,  45,  0.0,S, 120,  0.0,W, grid,   ,   ,   ,N
Point02,xy,  100,  100,in, deg,  46, 30.0,S, 119, 30.0,W, grid,   ,   ,   ,N
)ozi");
    char* raw_wkt = nullptr;
    EXPECT_TRUE(GDALReadOziMapFile("/vsimem/gdal_misc_test/geo_ozi.tif",
                                   adfGT.data(), &raw_wkt, &nGCPCount,
                                   &pasGCPs));
    std::unique_ptr<char, CplFreeDeleter> wkt(raw_wkt);
    EXPECT_NE(nullptr, wkt);
    EXPECT_EQ(0, nGCPCount);
  }

  // Valid Ozi with uppercase .MAP extension and 4 non-affine Cartesian points
  // (returns GCPs).
  {
    constexpr char kCartMap[] = "/vsimem/gdal_misc_test/cart_ozi.MAP";
    VsiMemTempWrapper cart_map(kCartMap,
                               R"ozi(OziExplorer Map Data File Version 2.2
Test
test.tif
1 ,Map Code,
WGS 84,WGS 84,   0.0000,   0.0000,WGS 84
Map Projection,(UTM) Universal Transverse Mercator,PolyCal,No,AutoCalOnly,No,BSBUseWPX,No
Projection Setup,     0.000000000,     0.000000000,     1.000000000,       0.00,       0.00,,,,,
Point01,xy,    0,    0,in, deg,    ,     ,N,    ,     ,E, grid, 32, 500000, 4500000,N
Point02,xy,  100,    0,in, deg,    ,     ,N,    ,     ,E, grid, 32, 501000, 4500000,N
Point03,xy,  100,  100,in, deg,    ,     ,N,    ,     ,E, grid, 32, 505000, 4490000,N
Point04,xy,    0,  100,in, deg,    ,     ,N,    ,     ,E, grid, 32, 500000, 4499000,N
)ozi");
    char* raw_wkt = nullptr;
    GDAL_GCP* raw_gcps = nullptr;
    EXPECT_TRUE(GDALReadOziMapFile("/vsimem/gdal_misc_test/cart_ozi.tif",
                                   adfGT.data(), &raw_wkt, &nGCPCount,
                                   &raw_gcps));
    std::unique_ptr<char, CplFreeDeleter> wkt(raw_wkt);
    absl::Cleanup free_gcps = [nGCPCount, raw_gcps] {
      GDALDeinitGCPs(nGCPCount, raw_gcps);
      CPLFree(raw_gcps);
    };
    EXPECT_EQ(4, nGCPCount);
    EXPECT_NE(nullptr, raw_gcps);
  }
}

TEST(GdalMiscTest, TabFile) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kVsiMemDir); };

  std::array<double, 6> adfGT{};
  char* pszWKT = nullptr;
  int nGCPCount = 0;
  GDAL_GCP* pasGCPs = nullptr;
  char* pszTabOut = nullptr;

  EXPECT_FALSE(GDALLoadTabFile("/vsimem/gdal_misc_test/no_such.tab",
                               adfGT.data(), &pszWKT, &nGCPCount, &pasGCPs));
  EXPECT_FALSE(GDALReadTabFile("/vsimem/gdal_misc_test/no_such.tif",
                               adfGT.data(), &pszWKT, &nGCPCount, &pasGCPs));
  EXPECT_FALSE(GDALReadTabFile2(
      "/vsisubfile/0_10,/vsimem/gdal_misc_test/no_such.tif", adfGT.data(),
      &pszWKT, &nGCPCount, &pasGCPs, nullptr, &pszTabOut));

  // Non-RASTER type tab file.
  {
    constexpr char kNativeTab[] = "/vsimem/gdal_misc_test/native.tab";
    VsiMemTempWrapper native_tab(kNativeTab,
                                 R"tab(!table
!version 300
Definition Table
  Type NATIVE
)tab");
    EXPECT_FALSE(GDALLoadTabFile(kNativeTab, adfGT.data(), &pszWKT, &nGCPCount,
                                 &pasGCPs));
  }

  // RASTER type with 0 GCPs.
  {
    constexpr char kEmptyTab[] = "/vsimem/gdal_misc_test/empty.tab";
    VsiMemTempWrapper empty_tab(kEmptyTab,
                                R"tab(!table
!version 300
Definition Table
  File "empty.tif"
  Type "RASTER"
)tab");
    EXPECT_FALSE(GDALLoadTabFile(kEmptyTab, adfGT.data(), &pszWKT, &nGCPCount,
                                 &pasGCPs));
  }

  // RASTER type with projected CoordSys + Units "degree" and 4 corner GCPs.
  {
    constexpr char kValidTab[] = "/vsimem/gdal_misc_test/valid.tab";
    constexpr char kValidTif[] = "/vsimem/gdal_misc_test/valid.tif";
    VsiMemTempWrapper valid_tab(kValidTab,
                                R"tab(!table
!version 300
Definition Table
  File "valid.tif"
  Type "RASTER"
  (10.0, 50.0) (0, 0) Label "Pt 1",
  (20.0, 50.0) (100, 0) Label "Pt 2",
  (20.0, 40.0) (100, 100) Label "Pt 3",
  (10.0, 40.0) (0, 100) Label "Pt 4"
  CoordSys Earth Projection 8, 104, "m", 15, 0, 0.9996, 500000, 0
  Units "degree"
)tab");
    char* raw_wkt = nullptr;
    char* raw_tab_out = nullptr;
    EXPECT_TRUE(GDALReadTabFile2(kValidTif, adfGT.data(), &raw_wkt, &nGCPCount,
                                 &pasGCPs, nullptr, &raw_tab_out));
    std::unique_ptr<char, CplFreeDeleter> wkt(raw_wkt);
    std::unique_ptr<char, CplFreeDeleter> tab_out(raw_tab_out);
    EXPECT_STREQ(kValidTab, tab_out.get());
    EXPECT_NE(nullptr, wkt);
    EXPECT_EQ(0, nGCPCount);

    // Test with sibling files.
    CPLStringList aosSiblings;
    aosSiblings.AddString("valid.tab");
    char* raw_sib_wkt = nullptr;
    char* raw_sib_tab = nullptr;
    EXPECT_TRUE(GDALReadTabFile2(kValidTif, adfGT.data(), &raw_sib_wkt,
                                 &nGCPCount, &pasGCPs, aosSiblings.List(),
                                 &raw_sib_tab));
    std::unique_ptr<char, CplFreeDeleter> sib_wkt(raw_sib_wkt);
    std::unique_ptr<char, CplFreeDeleter> sib_tab(raw_sib_tab);
    EXPECT_STREQ(kValidTab, sib_tab.get());

    CPLStringList aosOtherSiblings;
    aosOtherSiblings.AddString("other.tab");
    EXPECT_FALSE(GDALReadTabFile2(kValidTif, adfGT.data(), &pszWKT, &nGCPCount,
                                  &pasGCPs, aosOtherSiblings.List(),
                                  &pszTabOut));
  }

  // Uppercase .TAB with non-affine GCPs.
  {
    constexpr char kUpperTab[] = "/vsimem/gdal_misc_test/upper.TAB";
    VsiMemTempWrapper upper_tab(kUpperTab,
                                R"tab(!table
!version 300
Definition Table
  File "upper.tif"
  Type "RASTER"
  (10.0, 50.0) (0, 0) Label "Pt 1",
  (20.0, 50.0) (100, 0) Label "Pt 2",
  (25.0, 35.0) (100, 100) Label "Pt 3",
  (10.0, 40.0) (0, 100) Label "Pt 4"
)tab");
    char* raw_wkt = nullptr;
    GDAL_GCP* raw_gcps = nullptr;
    EXPECT_TRUE(GDALReadTabFile("/vsimem/gdal_misc_test/upper.tif",
                                adfGT.data(), &raw_wkt, &nGCPCount, &raw_gcps));
    std::unique_ptr<char, CplFreeDeleter> wkt(raw_wkt);
    absl::Cleanup free_gcps = [nGCPCount, raw_gcps] {
      GDALDeinitGCPs(nGCPCount, raw_gcps);
      CPLFree(raw_gcps);
    };
    EXPECT_EQ(4, nGCPCount);
    ASSERT_NE(nullptr, raw_gcps);
  }
}

TEST(GdalMiscTest, WorldFile) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kVsiMemDir); };

  std::array<double, 6> adfGT = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
  std::array<double, 6> adfGTRead{};

  const auto ExpectNullWorld = [](auto&& fn) {
    CPLErrorReset();
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(fn());
    }
    EXPECT_NE(CE_None, CPLGetLastErrorType());
  };
  ExpectNullWorld([&] { return GDALLoadWorldFile(nullptr, adfGTRead.data()); });
  ExpectNullWorld([&] {
    return GDALLoadWorldFile("/vsimem/gdal_misc_test/no.wld", nullptr);
  });
  ExpectNullWorld(
      [&] { return GDALReadWorldFile(nullptr, "wld", adfGTRead.data()); });
  ExpectNullWorld([&] {
    return GDALReadWorldFile("/vsimem/gdal_misc_test/no.tif", "wld", nullptr);
  });
  ExpectNullWorld(
      [&] { return GDALWriteWorldFile(nullptr, "wld", adfGT.data()); });
  ExpectNullWorld([&] {
    return GDALWriteWorldFile("/vsimem/gdal_misc_test/no.tif", nullptr,
                              adfGT.data());
  });
  ExpectNullWorld([&] {
    return GDALWriteWorldFile("/vsimem/gdal_misc_test/no.tif", "wld", nullptr);
  });
  CPLErrorReset();

  EXPECT_FALSE(GDALLoadWorldFile("/vsimem/gdal_misc_test/nonexistent.wld",
                                 adfGTRead.data()));
  EXPECT_FALSE(
      GDALReadWorldFile("/vsisubfile/0_10,/vsimem/gdal_misc_test/a.tif", "wld",
                        adfGTRead.data()));
  EXPECT_FALSE(GDALReadWorldFile("/vsimem/gdal_misc_test/a.x", nullptr,
                                 adfGTRead.data()));

  // Corrupt world file.
  {
    constexpr char kCorrupt[] = "/vsimem/gdal_misc_test/corrupt.wld";
    VsiMemTempWrapper corrupt_world(kCorrupt,
                                    "\n0.0\n0.0\n0.0\n0.0\n10.0\n20.0\n");
    EXPECT_FALSE(GDALLoadWorldFile(kCorrupt, adfGTRead.data()));
  }

  // Write and read back with Windows-style extension (.tfw), Unix-style
  // (.tifw), leading dot, sibling files, and uppercase extension.
  constexpr char kBase[] = "/vsimem/gdal_misc_test/world_test.tif";
  constexpr char kTfw[] = "/vsimem/gdal_misc_test/world_test.tfw";
  EXPECT_TRUE(GDALWriteWorldFile(kBase, "tfw", adfGT.data()));
  EXPECT_TRUE(GDALReadWorldFile(kBase, nullptr, adfGTRead.data()));
  for (size_t i = 0; i < adfGT.size(); ++i) {
    EXPECT_NEAR(adfGT[i], adfGTRead[i], 1e-7);
  }
  EXPECT_TRUE(GDALReadWorldFile(kBase, ".tfw", adfGTRead.data()));

  GDALGeoTransform gtObj;
  CPLStringList aosSiblings;
  aosSiblings.AddString("world_test.tfw");
  {
    char* raw_world_out = nullptr;
    EXPECT_TRUE(GDALReadWorldFile2(kBase, "tfw", gtObj, aosSiblings.List(),
                                   &raw_world_out));
    std::unique_ptr<char, CplFreeDeleter> world_out(raw_world_out);
    EXPECT_STREQ(kTfw, world_out.get());
  }

  CPLStringList aosEmptySiblings;
  aosEmptySiblings.AddString("other.tfw");
  {
    char* raw_world_out = nullptr;
    EXPECT_FALSE(GDALReadWorldFile2(kBase, "tfw", gtObj,
                                    aosEmptySiblings.List(), &raw_world_out));
    std::unique_ptr<char, CplFreeDeleter> world_out(raw_world_out);
  }
  EXPECT_EQ(0, VSIUnlink(kTfw));

  // Unix-style extension (.tifw) and uppercase (.WLD).
  EXPECT_TRUE(GDALWriteWorldFile(kBase, "tifw", adfGT.data()));
  EXPECT_TRUE(GDALReadWorldFile(kBase, nullptr, adfGTRead.data()));
  EXPECT_EQ(0, VSIUnlink("/vsimem/gdal_misc_test/world_test.tifw"));

  constexpr char kWld[] = "/vsimem/gdal_misc_test/world_test.WLD";
  EXPECT_TRUE(GDALWriteWorldFile(kBase, "WLD", adfGT.data()));
  {
    char* raw_world_out = nullptr;
    EXPECT_TRUE(GDALReadWorldFile2(kBase, "wld", adfGTRead.data(), nullptr,
                                   &raw_world_out));
    std::unique_ptr<char, CplFreeDeleter> world_out(raw_world_out);
    EXPECT_STREQ(kWld, world_out.get());
  }
  EXPECT_EQ(0, VSIUnlink(kWld));
}

TEST(GdalMiscTest, VersionInfoAndCheckVersion) {
  constexpr const char* kRequests[] = {
      nullptr,
      "VERSION_NUM",
      "RELEASE_DATE",
      "RELEASE_NAME",
      "RELEASE_NICKNAME",
      "--version",
      "BUILD_INFO",
      "LICENSE",
      // Second call hits TLS cache.
      "LICENSE",
  };
  for (const char* req : kRequests) {
    EXPECT_NE(nullptr, GDALVersionInfo(req));
  }

  EXPECT_TRUE(
      GDALCheckVersion(GDAL_VERSION_MAJOR, GDAL_VERSION_MINOR, "unit_test"));
  EXPECT_FALSE(
      GDALCheckVersion(GDAL_VERSION_MAJOR + 1, GDAL_VERSION_MINOR, nullptr));

  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(GDALCheckVersion(GDAL_VERSION_MAJOR + 1, GDAL_VERSION_MINOR,
                                  "unit_test"));
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST(GdalMiscTest, DMSConversions) {
  const char* pszDMS = GDALDecToDMS(45.5, "Long", 2);
  EXPECT_NE(nullptr, pszDMS);
  const double dfPacked = GDALDecToPackedDMS(45.5);
  EXPECT_NEAR(45.5, GDALPackedDMSToDec(dfPacked), 1e-6);
}

TEST(GdalMiscTest, GDALGCPsToGeoTransformAndCompose) {
  std::array<double, 6> adfGT{};
  std::array<GDAL_GCP, 5> asGCPs{};
  GDALInitGCPs(static_cast<int>(asGCPs.size()), asGCPs.data());
  absl::Cleanup deinit_gcps = [&asGCPs] {
    GDALDeinitGCPs(static_cast<int>(asGCPs.size()), asGCPs.data());
  };

  // < 2 GCPs
  EXPECT_FALSE(GDALGCPsToGeoTransform(1, asGCPs.data(), adfGT.data(), TRUE));

  // 2 GCPs: degenerate (same pixel)
  asGCPs[0].dfGCPPixel = 0.0;
  asGCPs[0].dfGCPLine = 0.0;
  asGCPs[1].dfGCPPixel = 0.0;
  asGCPs[1].dfGCPLine = 10.0;
  EXPECT_FALSE(GDALGCPsToGeoTransform(2, asGCPs.data(), adfGT.data(), TRUE));

  // 2 GCPs: valid
  asGCPs[1].dfGCPPixel = 10.0;
  asGCPs[1].dfGCPLine = 10.0;
  asGCPs[0].dfGCPX = 100.0;
  asGCPs[0].dfGCPY = 200.0;
  asGCPs[1].dfGCPX = 120.0;
  asGCPs[1].dfGCPY = 180.0;
  EXPECT_TRUE(GDALGCPsToGeoTransform(2, asGCPs.data(), adfGT.data(), FALSE));
  EXPECT_DOUBLE_EQ(100.0, adfGT[0]);
  EXPECT_DOUBLE_EQ(2.0, adfGT[1]);
  EXPECT_DOUBLE_EQ(-2.0, adfGT[5]);

  // 4 GCPs: special axis-aligned TL, TR, BR, BL
  asGCPs[0].dfGCPPixel = 0.0;
  asGCPs[0].dfGCPLine = 0.0;
  asGCPs[0].dfGCPX = 100.0;
  asGCPs[0].dfGCPY = 200.0;
  asGCPs[1].dfGCPPixel = 10.0;
  asGCPs[1].dfGCPLine = 0.0;
  asGCPs[1].dfGCPX = 120.0;
  asGCPs[1].dfGCPY = 200.0;
  asGCPs[2].dfGCPPixel = 10.0;
  asGCPs[2].dfGCPLine = 10.0;
  asGCPs[2].dfGCPX = 120.0;
  asGCPs[2].dfGCPY = 180.0;
  asGCPs[3].dfGCPPixel = 0.0;
  asGCPs[3].dfGCPLine = 10.0;
  asGCPs[3].dfGCPX = 100.0;
  asGCPs[3].dfGCPY = 180.0;
  EXPECT_TRUE(GDALGCPsToGeoTransform(4, asGCPs.data(), adfGT.data(), FALSE));

  // 3 GCPs: degenerate (all same line)
  asGCPs[2].dfGCPLine = 0.0;
  EXPECT_FALSE(GDALGCPsToGeoTransform(3, asGCPs.data(), adfGT.data(), TRUE));

  // 3 GCPs: collinear in pixel/line space -> divisor == 0
  asGCPs[0].dfGCPPixel = 0.0;
  asGCPs[0].dfGCPLine = 0.0;
  asGCPs[0].dfGCPX = 0.0;
  asGCPs[0].dfGCPY = 0.0;
  asGCPs[1].dfGCPPixel = 5.0;
  asGCPs[1].dfGCPLine = 5.0;
  asGCPs[1].dfGCPX = 5.0;
  asGCPs[1].dfGCPY = 5.0;
  asGCPs[2].dfGCPPixel = 10.0;
  asGCPs[2].dfGCPLine = 10.0;
  asGCPs[2].dfGCPX = 10.0;
  asGCPs[2].dfGCPY = 10.0;
  EXPECT_FALSE(GDALGCPsToGeoTransform(3, asGCPs.data(), adfGT.data(), TRUE));

  // 5 GCPs: exact fit in general least squares
  asGCPs[0].dfGCPPixel = 0.0;
  asGCPs[0].dfGCPLine = 0.0;
  asGCPs[0].dfGCPX = 100.0;
  asGCPs[0].dfGCPY = 200.0;
  asGCPs[1].dfGCPPixel = 10.0;
  asGCPs[1].dfGCPLine = 0.0;
  asGCPs[1].dfGCPX = 120.0;
  asGCPs[1].dfGCPY = 200.0;
  asGCPs[2].dfGCPPixel = 10.0;
  asGCPs[2].dfGCPLine = 10.0;
  asGCPs[2].dfGCPX = 120.0;
  asGCPs[2].dfGCPY = 180.0;
  asGCPs[3].dfGCPPixel = 0.0;
  asGCPs[3].dfGCPLine = 10.0;
  asGCPs[3].dfGCPX = 100.0;
  asGCPs[3].dfGCPY = 180.0;
  asGCPs[4].dfGCPPixel = 5.0;
  asGCPs[4].dfGCPLine = 5.0;
  asGCPs[4].dfGCPX = 110.0;
  asGCPs[4].dfGCPY = 190.0;
  EXPECT_TRUE(GDALGCPsToGeoTransform(5, asGCPs.data(), adfGT.data(), FALSE));

  // Perturb 5th point so exact fit fails, approx fit succeeds.
  asGCPs[4].dfGCPX = 150.0;
  EXPECT_FALSE(GDALGCPsToGeoTransform(5, asGCPs.data(), adfGT.data(), FALSE));
  EXPECT_TRUE(GDALGCPsToGeoTransform(5, asGCPs.data(), adfGT.data(), TRUE));

  {
    CPLConfigOptionSetter oSetter("GDAL_GCPS_TO_GEOTRANSFORM_APPROX_OK", "YES",
                                  false);
    EXPECT_TRUE(GDALGCPsToGeoTransform(5, asGCPs.data(), adfGT.data(), FALSE));
  }

  // ComposeGeoTransforms
  constexpr std::array<double, 6> gt1 = {10.0, 2.0, 0.0, 20.0, 0.0, -2.0};
  constexpr std::array<double, 6> gt2 = {5.0, 0.5, 0.0, -10.0, 0.0, -0.5};
  std::array<double, 6> gtOut{};
  GDALComposeGeoTransforms(gt1.data(), gt2.data(), gtOut.data());
  EXPECT_DOUBLE_EQ(10.0, gtOut[0]);
  EXPECT_DOUBLE_EQ(1.0, gtOut[1]);
  EXPECT_DOUBLE_EQ(-20.0, gtOut[3]);
  EXPECT_DOUBLE_EQ(1.0, gtOut[5]);
}

TEST(GdalMiscTest, DriverListAndGeneralCmdLineProcessor) {
  GDALRegister_MEM();
  GDALRegister_GTiff();
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kVsiMemDir); };

  EXPECT_FALSE(GDALPrintDriverList(0, false).empty());
  EXPECT_FALSE(GDALPrintDriverList(0, true).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_VECTOR, false).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_VECTOR, true).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_MULTIDIM_RASTER, false).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_MULTIDIM_RASTER, true).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_GNM, false).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_GNM, true).empty());

  const auto RunCmdLine = [](std::initializer_list<const char*> args,
                             int nOptions = 0) {
    CPLStringList aosList;
    for (const char* arg : args) {
      aosList.AddString(arg);
    }
    char** papszArgv = aosList.StealList();
    char** papszOrig = papszArgv;
    absl::Cleanup free_argv = [&papszArgv, papszOrig] {
      if (papszArgv != papszOrig) {
        CSLDestroy(papszOrig);
      }
      CSLDestroy(papszArgv);
    };
    return GDALGeneralCmdLineProcessor(static_cast<int>(args.size()),
                                       &papszArgv, nOptions);
  };

  EXPECT_EQ(0, RunCmdLine({"prog", "--version"}));
  EXPECT_EQ(0, RunCmdLine({"prog", "--build"}));
  EXPECT_EQ(0, RunCmdLine({"prog", "--license"}));
  EXPECT_EQ(0, RunCmdLine({"prog", "--help-general"}));
  EXPECT_EQ(0, RunCmdLine({"prog", "--formats"}));
  EXPECT_EQ(0, RunCmdLine({"prog", "--formats", "-json"}));
  EXPECT_EQ(0, RunCmdLine({"prog", "--format", "GTiff"}, GDAL_OF_RASTER));
  EXPECT_EQ(0, RunCmdLine({"prog", "--format", "GTiff"}, GDAL_OF_VECTOR));
  EXPECT_EQ(0, RunCmdLine({"prog", "--format", "MEM"},
                          GDAL_OF_RASTER | GDAL_OF_VECTOR));

  // --config, --debug, --locale, and pass-through arguments.
  {
    absl::Cleanup clear_config = [] {
      CPLSetConfigOption("FOO_OPT", nullptr);
      CPLSetConfigOption("FOO_OPT2", nullptr);
      CPLSetConfigOption("CPL_DEBUG", nullptr);
    };
    EXPECT_EQ(2,
              RunCmdLine({"prog", "--config", "FOO_OPT", "BAR_VAL", "--config",
                          "FOO_OPT2=BAR_VAL2", "--config", "CPL_DEBUG", "OFF",
                          "--debug", "OFF", "--locale", "C", "custom_arg"}));
    EXPECT_STREQ("BAR_VAL", CPLGetConfigOption("FOO_OPT", ""));
    EXPECT_STREQ("BAR_VAL2", CPLGetConfigOption("FOO_OPT2", ""));
  }

  // --mempreload
  {
    constexpr char kPreloadDir[] = "/vsimem/gdal_misc_test/preload_dir";
    constexpr char kPreloadSubDir[] =
        "/vsimem/gdal_misc_test/preload_dir/subdir";
    constexpr char kPreloadFile[] =
        "/vsimem/gdal_misc_test/preload_dir/file1.txt";
    constexpr char kPreloadedFile[] = "/vsimem/file1.txt";
    absl::Cleanup unlink_preloaded = [kPreloadedFile] {
      VSIUnlink(kPreloadedFile);
    };
    EXPECT_EQ(0, VSIMkdir(kVsiMemDir, 0755));
    EXPECT_EQ(0, VSIMkdir(kPreloadDir, 0755));
    EXPECT_EQ(0, VSIMkdir(kPreloadSubDir, 0755));
    VsiMemTempWrapper preload_file(kPreloadFile, "");
    EXPECT_EQ(1, RunCmdLine({"prog", "--mempreload", kPreloadDir}));
    VSIStatBufL sStat{};
    EXPECT_EQ(0, VSIStatL(kPreloadedFile, &sStat));
  }

  // --optfile
  {
    absl::Cleanup clear_optfile_key = [] {
      CPLSetConfigOption("OPTFILE_KEY", nullptr);
    };
    constexpr char kOptFile[] = "/vsimem/gdal_misc_test/test_optfile.txt";
    VsiMemTempWrapper opt_file(kOptFile,
                               "# comment line\n"
                               "\n"
                               "--config OPTFILE_KEY=OPTFILE_VAL\n"
                               "arg_from_optfile\n");
    EXPECT_EQ(2, RunCmdLine({"prog", "--optfile", kOptFile}));
    EXPECT_STREQ("OPTFILE_VAL", CPLGetConfigOption("OPTFILE_KEY", ""));
  }

  // Nested --optfile triggers warning and does not recurse.
  {
    constexpr char kNestedOpt[] = "/vsimem/gdal_misc_test/nested_optfile.txt";
    VsiMemTempWrapper nested_opt(kNestedOpt, "--optfile other.txt\n");

    const auto ExpectCmdResult = [&](std::initializer_list<const char*> args,
                                     int expected_rc, CPLErr expected_err) {
      CPLErrorReset();
      {
        WithQuietHandler quiet;
        EXPECT_EQ(expected_rc, RunCmdLine(args));
      }
      EXPECT_EQ(expected_err, CPLGetLastErrorType());
    };

    ExpectCmdResult({"prog", "--optfile", kNestedOpt}, 3, CE_Warning);

    // Error cases:
    ExpectCmdResult({"prog", "--debug"}, -1, CE_Failure);
    ExpectCmdResult({"prog", "--config"}, -1, CE_Failure);
    ExpectCmdResult({"prog", "--config", "ONLY_KEY"}, -1, CE_Failure);
    ExpectCmdResult({"prog", "--mempreload"}, -1, CE_Failure);
    ExpectCmdResult(
        {"prog", "--mempreload", "/vsimem/gdal_misc_test/no_such_dir"}, -1,
        CE_Failure);
    ExpectCmdResult({"prog", "--optfile"}, -1, CE_Failure);
    ExpectCmdResult(
        {"prog", "--optfile", "/vsimem/gdal_misc_test/no_such_opt.txt"}, -1,
        CE_Failure);
    ExpectCmdResult({"prog", "--format"}, -1, CE_Failure);
    ExpectCmdResult({"prog", "--format", "NOT_A_DRIVER"}, -1, CE_Failure);
    CPLErrorReset();
  }
}

// Expose legacy V1 ABI signature of GDALExtractRPCInfo in gcore/gdal_misc.cpp;
// keep in sync with third_party/gdal/gcore/gdal.h and gcore/gdal_misc.cpp.
#undef GDALExtractRPCInfo
extern "C" int CPL_STDCALL GDALExtractRPCInfo(CSLConstList, GDALRPCInfoV1*);

TEST(GdalMiscTest, ExtractRPCInfo) {
  GDALRPCInfoV1 sRPC1{};
  GDALRPCInfoV2 sRPC2{};
  EXPECT_FALSE(GDALExtractRPCInfo(nullptr, &sRPC1));
  EXPECT_FALSE(GDALExtractRPCInfoV2(nullptr, &sRPC2));

  CPLStringList aosMD;
  aosMD.SetNameValue("LINE_NUM_COEFF", "1 2 3");
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(GDALExtractRPCInfoV2(aosMD.List(), &sRPC2));
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();

  constexpr char kTwentyVals[] =
      "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20";
  aosMD.SetNameValue("LINE_NUM_COEFF", kTwentyVals);
  aosMD.SetNameValue("LINE_DEN_COEFF", kTwentyVals);
  aosMD.SetNameValue("SAMP_NUM_COEFF", kTwentyVals);
  aosMD.SetNameValue("SAMP_DEN_COEFF", "1 2 3");  // wrong count (3 != 20)
  aosMD.SetNameValue("LINE_OFF", "100.5");
  aosMD.SetNameValue("SAMP_OFF", "200.5");
  EXPECT_TRUE(GDALExtractRPCInfo(aosMD.List(), &sRPC1));
  EXPECT_TRUE(GDALExtractRPCInfoV2(aosMD.List(), &sRPC2));
  EXPECT_DOUBLE_EQ(100.5, sRPC2.dfLINE_OFF);
  EXPECT_DOUBLE_EQ(20.0, sRPC2.adfLINE_NUM_COEFF[19]);
  EXPECT_DOUBLE_EQ(0.0, sRPC2.adfSAMP_DEN_COEFF[0]);
}

TEST(GdalMiscTest, GDALFindAssociatedAuxFile) {
  GDALRegister_HFA();
  GDALRegister_MEM();
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kVsiMemDir); };

  EXPECT_EQ(nullptr,
            GDALFindAssociatedAuxFile("test.aux", GA_ReadOnly, nullptr));
  EXPECT_EQ(nullptr, GDALFindAssociatedAuxFile("", GA_ReadOnly, nullptr));
  EXPECT_EQ(nullptr,
            GDALFindAssociatedAuxFile("/vsimem/gdal_misc_test/no_aux.tif",
                                      GA_ReadOnly, nullptr));

  GDALDriver* poHFADrv = GetGDALDriverManager()->GetDriverByName("HFA");
  GDALDriver* poMemDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poHFADrv);
  ASSERT_NE(nullptr, poMemDrv);

  // 1. HFA file without HFA_DEPENDENT_FILE -> ignored.
  {
    GDALDatasetUniquePtr poHFA(poHFADrv->Create(
        "/vsimem/gdal_misc_test/aux1.aux", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poHFA);
  }
  EXPECT_EQ(nullptr,
            GDALFindAssociatedAuxFile("/vsimem/gdal_misc_test/aux1.tif",
                                      GA_ReadOnly, nullptr));

  // 2. HFA file with AUX=YES and DEPENDENT_FILE matching, uppercase .AUX.
  {
    CPLStringList aosOpts;
    aosOpts.SetNameValue("AUX", "YES");
    aosOpts.SetNameValue("DEPENDENT_FILE", "aux2.tif");
    GDALDatasetUniquePtr poHFA(
        poHFADrv->Create("/vsimem/gdal_misc_test/aux2.AUX", 10, 10, 1, GDT_Byte,
                         aosOpts.List()));
    ASSERT_NE(nullptr, poHFA);
  }
  GDALDatasetUniquePtr poDepMatch(
      poMemDrv->Create("", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, poDepMatch);
  poDepMatch->MarkAsShared();
  {
    GDALDatasetUniquePtr poFound(GDALFindAssociatedAuxFile(
        "/vsimem/gdal_misc_test/aux2.tif", GA_ReadOnly, poDepMatch.get()));
    EXPECT_NE(nullptr, poFound);
  }

  // Mismatched dimensions -> ignored.
  GDALDatasetUniquePtr poDepMismatch(
      poMemDrv->Create("", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, poDepMismatch);
  EXPECT_EQ(nullptr,
            GDALFindAssociatedAuxFile("/vsimem/gdal_misc_test/aux2.tif",
                                      GA_ReadOnly, poDepMismatch.get()));

  // 3. Appended .tif.AUX with non-existent vs existing other dependent file.
  constexpr char kOtherDep[] = "/vsimem/gdal_misc_test/other_dep.tif";
  {
    CPLStringList aosOpts;
    aosOpts.SetNameValue("AUX", "YES");
    aosOpts.SetNameValue("DEPENDENT_FILE", kOtherDep);
    GDALDatasetUniquePtr poHFA(
        poHFADrv->Create("/vsimem/gdal_misc_test/aux3.tif.AUX", 10, 10, 1,
                         GDT_Byte, aosOpts.List()));
    ASSERT_NE(nullptr, poHFA);
  }
  // When kOtherDep does NOT exist, it uses the .aux file anyway.
  {
    GDALDatasetUniquePtr poFound(GDALFindAssociatedAuxFile(
        "/vsimem/gdal_misc_test/aux3.tif", GA_ReadOnly, poDepMatch.get()));
    EXPECT_NE(nullptr, poFound);
  }

  // When kOtherDep DOES exist, it ignores the .aux file.
  {
    VsiMemTempWrapper other_dep_file(kOtherDep, "");
    EXPECT_EQ(nullptr,
              GDALFindAssociatedAuxFile("/vsimem/gdal_misc_test/aux3.tif",
                                        GA_ReadOnly, nullptr));
  }
}

TEST(GdalMiscTest, CheckDatasetDimensionsAndBandCount) {
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(GDALCheckDatasetDimensions(0, 10));
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(GDALCheckDatasetDimensions(10, -1));
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();
  EXPECT_TRUE(GDALCheckDatasetDimensions(10, 10));
  EXPECT_EQ(CE_None, CPLGetLastErrorType());

  const auto ExpectInvalidBandCount = [](int bands, int allow_zero) {
    CPLErrorReset();
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(GDALCheckBandCount(bands, allow_zero));
    }
    EXPECT_NE(CE_None, CPLGetLastErrorType());
  };

  ExpectInvalidBandCount(-1, TRUE);
  ExpectInvalidBandCount(0, FALSE);
  CPLErrorReset();
  EXPECT_TRUE(GDALCheckBandCount(0, TRUE));
  EXPECT_TRUE(GDALCheckBandCount(3, FALSE));
  EXPECT_EQ(CE_None, CPLGetLastErrorType());

  ExpectInvalidBandCount(70000, FALSE);
  {
    CPLConfigOptionSetter oSetter("GDAL_MAX_BAND_COUNT", "10", false);
    ExpectInvalidBandCount(11, FALSE);
    CPLErrorReset();
    EXPECT_TRUE(GDALCheckBandCount(10, FALSE));
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }
  CPLErrorReset();
}

TEST(GdalMiscTest, SerializeDeserializeGCPListXML) {
  OGRSpatialReference oSRS;
  oSRS.SetWellKnownGeogCS("WGS84");
  oSRS.SetDataAxisToSRSAxisMapping({2, 1});

  std::vector<gdal::GCP> asGCPs;
  asGCPs.emplace_back("1", "corner", 0.5, 1.5, 10.0, 20.0, 30.0);
  asGCPs.emplace_back("2", "", 10.5, 11.5, 15.0, 25.0, 0.0);

  CPLXMLTreeCloser psRoot(CPLCreateXMLNode(nullptr, CXT_Element, "PAMDataset"));
  GDALSerializeGCPListToXML(psRoot.get(), asGCPs, &oSRS);

  CPLXMLNode* psGCPList = CPLGetXMLNode(psRoot.get(), "GCPList");
  ASSERT_NE(nullptr, psGCPList);

  // Add a non-GCP child and invalid GCP nodes to test error recovery.
  CPLCreateXMLNode(psGCPList, CXT_Element, "NotAGCP");
  CPLXMLNode* psBadGCP1 = CPLCreateXMLNode(psGCPList, CXT_Element, "GCP");
  CPLSetXMLValue(psBadGCP1, "#Pixel", "not_a_number");
  CPLSetXMLValue(psBadGCP1, "#Z", "not_a_number");

  std::vector<gdal::GCP> asOutGCPs;
  OGRSpatialReference* rawOutSRS = nullptr;
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    GDALDeserializeGCPListFromXML(psGCPList, asOutGCPs, &rawOutSRS);
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();

  std::unique_ptr<OGRSpatialReference> poOutSRS(rawOutSRS);
  ASSERT_NE(nullptr, poOutSRS);
  EXPECT_THAT(poOutSRS->GetDataAxisToSRSAxisMapping(), ElementsAre(2, 1));

  ASSERT_EQ(2u, asOutGCPs.size());
  EXPECT_STREQ("1", asOutGCPs[0].Id());
  EXPECT_STREQ("corner", asOutGCPs[0].Info());
  EXPECT_DOUBLE_EQ(30.0, asOutGCPs[0].Z());
  EXPECT_DOUBLE_EQ(0.0, asOutGCPs[1].Z());
}

TEST(GdalMiscTest, SerializeDeserializeOpenOptionsXML) {
  CPLXMLTreeCloser psRoot(CPLCreateXMLNode(nullptr, CXT_Element, "Root"));
  GDALSerializeOpenOptionsToXML(psRoot.get(), nullptr);
  EXPECT_EQ(nullptr, GDALDeserializeOpenOptionsFromXML(psRoot.get()));

  CPLStringList aosOpts;
  aosOpts.SetNameValue("KEY1", "VAL1");
  aosOpts.SetNameValue("KEY2", "VAL2");
  GDALSerializeOpenOptionsToXML(psRoot.get(), aosOpts.List());

  StringListPtr papszOut(GDALDeserializeOpenOptionsFromXML(psRoot.get()),
                         CSLDestroy);
  ASSERT_NE(nullptr, papszOut);
  EXPECT_STREQ("VAL1", CSLFetchNameValue(papszOut.get(), "KEY1"));
  EXPECT_STREQ("VAL2", CSLFetchNameValue(papszOut.get(), "KEY2"));
}

TEST(GdalMiscTest, ResampleAlgFunctions) {
  struct ResampleFromNameCase {
    const char* name;
    GDALRIOResampleAlg expected;
  };
  constexpr ResampleFromNameCase kFromNameCases[] = {
      {.name = "NEAR", .expected = GRIORA_NearestNeighbour},
      {.name = "BILINEAR", .expected = GRIORA_Bilinear},
      {.name = "CUBIC", .expected = GRIORA_Cubic},
      {.name = "CUBICSPLINE", .expected = GRIORA_CubicSpline},
      {.name = "LANCZOS", .expected = GRIORA_Lanczos},
      {.name = "AVERAGE", .expected = GRIORA_Average},
      {.name = "RMS", .expected = GRIORA_RMS},
      {.name = "MODE", .expected = GRIORA_Mode},
      {.name = "GAUSS", .expected = GRIORA_Gauss},
  };
  for (const auto& c : kFromNameCases) {
    EXPECT_EQ(c.expected, GDALRasterIOGetResampleAlg(c.name));
  }

  CPLErrorReset();
  {
    WithQuietHandler quiet;
    EXPECT_EQ(GRIORA_NearestNeighbour,
              GDALRasterIOGetResampleAlg("INVALID_ALG"));
  }
  EXPECT_NE(CE_None, CPLGetLastErrorType());
  CPLErrorReset();

  struct ResampleToNameCase {
    GDALRIOResampleAlg alg;
    const char* name;
  };
  constexpr ResampleToNameCase kToNameCases[] = {
      {.alg = GRIORA_NearestNeighbour, .name = "NearestNeighbour"},
      {.alg = GRIORA_Bilinear, .name = "Bilinear"},
      {.alg = GRIORA_Cubic, .name = "Cubic"},
      {.alg = GRIORA_CubicSpline, .name = "CubicSpline"},
      {.alg = GRIORA_Lanczos, .name = "Lanczos"},
      {.alg = GRIORA_Average, .name = "Average"},
      {.alg = GRIORA_RMS, .name = "RMS"},
      {.alg = GRIORA_Mode, .name = "Mode"},
      {.alg = GRIORA_Gauss, .name = "Gauss"},
  };
  for (const auto& c : kToNameCases) {
    EXPECT_STREQ(c.name, GDALRasterIOGetResampleAlg(c.alg));
  }

  GDALRasterIOExtraArg sArg;
  INIT_RASTERIO_EXTRA_ARG(sArg);
  {
    CPLConfigOptionSetter oSetter("GDAL_RASTERIO_RESAMPLING", "BILINEAR",
                                  false);
    GDALRasterIOExtraArgSetResampleAlg(&sArg, 10, 10, 20, 20);
    EXPECT_EQ(GRIORA_Bilinear, sArg.eResampleAlg);
  }
}

TEST(GdalMiscTest, NoDataHelpers) {
  GDALRegister_MEM();
  EXPECT_TRUE(GDALCanReliablyUseSiblingFileList("/vsimem/test.tif"));

  constexpr float kMaxFloat = std::numeric_limits<float>::max();
  EXPECT_DOUBLE_EQ(static_cast<double>(kMaxFloat),
                   GDALAdjustNoDataCloseToFloatMax(
                       static_cast<double>(kMaxFloat) * (1.0 - 1e-12)));
  EXPECT_DOUBLE_EQ(static_cast<double>(-kMaxFloat),
                   GDALAdjustNoDataCloseToFloatMax(
                       static_cast<double>(-kMaxFloat) * (1.0 - 1e-12)));
  EXPECT_DOUBLE_EQ(42.0, GDALAdjustNoDataCloseToFloatMax(42.0));

  // GDALGetNoDataValueCastToDouble
  EXPECT_DOUBLE_EQ(123.0,
                   GDALGetNoDataValueCastToDouble(static_cast<int64_t>(123)));
  EXPECT_DOUBLE_EQ(123.0,
                   GDALGetNoDataValueCastToDouble(static_cast<uint64_t>(123)));

  CPLErrorReset();
  {
    WithQuietHandler quiet;
    GDALGetNoDataValueCastToDouble(static_cast<int64_t>((1LL << 60) + 1));
  }
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    GDALGetNoDataValueCastToDouble(static_cast<uint64_t>((1ULL << 60) + 1));
  }
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  CPLErrorReset();

  // GDALCopyNoDataValue
  GDALDriver* poMemDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDrv);
  const auto MakeBand = [&](GDALDataType eDT) {
    return GDALDatasetUniquePtr(poMemDrv->Create("", 2, 2, 1, eDT, nullptr));
  };

  {
    auto poSrc = MakeBand(GDT_Int64);
    auto poDstI64 = MakeBand(GDT_Int64);
    auto poDstU64 = MakeBand(GDT_UInt64);
    auto poDstByte = MakeBand(GDT_Byte);
    ASSERT_NE(nullptr, poSrc);
    ASSERT_NE(nullptr, poDstI64);
    ASSERT_NE(nullptr, poDstU64);
    ASSERT_NE(nullptr, poDstByte);
    bool bCannot = false;
    // No nodata set on source.
    EXPECT_FALSE(GDALCopyNoDataValue(poDstI64->GetRasterBand(1),
                                     poSrc->GetRasterBand(1), &bCannot));
    EXPECT_TRUE(bCannot);

    EXPECT_EQ(CE_None, poSrc->GetRasterBand(1)->SetNoDataValueAsInt64(100));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstI64->GetRasterBand(1),
                                    poSrc->GetRasterBand(1), &bCannot));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstU64->GetRasterBand(1),
                                    poSrc->GetRasterBand(1), &bCannot));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstByte->GetRasterBand(1),
                                    poSrc->GetRasterBand(1), &bCannot));

    EXPECT_EQ(CE_None, poSrc->GetRasterBand(1)->SetNoDataValueAsInt64(-5));
    EXPECT_FALSE(GDALCopyNoDataValue(poDstU64->GetRasterBand(1),
                                     poSrc->GetRasterBand(1), &bCannot));
  }
  {
    auto poSrc = MakeBand(GDT_UInt64);
    auto poDstU64 = MakeBand(GDT_UInt64);
    auto poDstI64 = MakeBand(GDT_Int64);
    auto poDstByte = MakeBand(GDT_Byte);
    ASSERT_NE(nullptr, poSrc);
    ASSERT_NE(nullptr, poDstU64);
    ASSERT_NE(nullptr, poDstI64);
    ASSERT_NE(nullptr, poDstByte);
    EXPECT_EQ(CE_None, poSrc->GetRasterBand(1)->SetNoDataValueAsUInt64(200));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstU64->GetRasterBand(1),
                                    poSrc->GetRasterBand(1)));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstI64->GetRasterBand(1),
                                    poSrc->GetRasterBand(1)));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstByte->GetRasterBand(1),
                                    poSrc->GetRasterBand(1)));
  }
  {
    auto poSrc = MakeBand(GDT_Float64);
    auto poDstI64 = MakeBand(GDT_Int64);
    auto poDstU64 = MakeBand(GDT_UInt64);
    auto poDstF32 = MakeBand(GDT_Float32);
    ASSERT_NE(nullptr, poSrc);
    ASSERT_NE(nullptr, poDstI64);
    ASSERT_NE(nullptr, poDstU64);
    ASSERT_NE(nullptr, poDstF32);
    EXPECT_EQ(CE_None, poSrc->GetRasterBand(1)->SetNoDataValue(50.0));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstI64->GetRasterBand(1),
                                    poSrc->GetRasterBand(1)));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstU64->GetRasterBand(1),
                                    poSrc->GetRasterBand(1)));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstF32->GetRasterBand(1),
                                    poSrc->GetRasterBand(1)));
  }

  // GDALGetNoDataReplacementValue for all types (in-range, max, out-of-range).
  constexpr std::array<GDALDataType, 11> kTypes = {
      GDT_Byte,   GDT_Int8,  GDT_UInt16,  GDT_Int16,   GDT_UInt32, GDT_Int32,
      GDT_UInt64, GDT_Int64, GDT_Float16, GDT_Float32, GDT_Float64};
  for (GDALDataType eDT : kTypes) {
    EXPECT_NE(0.0, GDALGetNoDataReplacementValue(eDT, 0.0));
    if (eDT != GDT_Float64) {
      EXPECT_DOUBLE_EQ(0.0, GDALGetNoDataReplacementValue(
                                eDT, std::numeric_limits<double>::infinity()));
    }
  }

  struct ReplacementMaxCase {
    GDALDataType dt;
    double input;
    double expected;
  };
  constexpr ReplacementMaxCase kExactMaxCases[] = {
      {.dt = GDT_Byte, .input = 255.0, .expected = 254.0},
      {.dt = GDT_Int8, .input = 127.0, .expected = 126.0},
      {.dt = GDT_UInt16, .input = 65535.0, .expected = 65534.0},
      {.dt = GDT_Int16, .input = 32767.0, .expected = 32766.0},
      {.dt = GDT_UInt32, .input = 4294967295.0, .expected = 4294967294.0},
      {.dt = GDT_Int32, .input = 2147483647.0, .expected = 2147483646.0},
  };
  for (const auto& c : kExactMaxCases) {
    EXPECT_DOUBLE_EQ(c.expected, GDALGetNoDataReplacementValue(c.dt, c.input));
  }

  EXPECT_GT(GDALGetNoDataReplacementValue(
                GDT_UInt64,
                static_cast<double>(std::numeric_limits<uint64_t>::max())),
            0.0);
  EXPECT_GT(
      GDALGetNoDataReplacementValue(
          GDT_Int64, static_cast<double>(std::numeric_limits<int64_t>::max())),
      0.0);
  EXPECT_GT(GDALGetNoDataReplacementValue(
                GDT_Float16,
                static_cast<double>(cpl::NumericLimits<GFloat16>::max())),
            0.0);
  EXPECT_GT(
      GDALGetNoDataReplacementValue(
          GDT_Float32, static_cast<double>(std::numeric_limits<float>::max())),
      0.0);
  EXPECT_GT(GDALGetNoDataReplacementValue(GDT_Float64,
                                          std::numeric_limits<double>::max()),
            0.0);
}

TEST(GdalMiscTest, GDALGetCompressionFormatForJPEG) {
  constexpr std::array<GByte, 4> kNotJpeg = {0x00, 0x01, 0x02, 0x03};
  EXPECT_EQ("",
            GDALGetCompressionFormatForJPEG(kNotJpeg.data(), kNotJpeg.size()));

  const auto MakeJpegBuf = [](GByte sofMarker, GByte numComponents,
                              GByte subsampling, int adobeTransform) {
    std::vector<GByte> buf = {0xFF, 0xD8};  // SOI
    if (adobeTransform >= 0) {
      // APP14 Adobe marker (0xFF, 0xEE, length=14, "Adobe" + 7 bytes)
      buf.insert(buf.end(),
                 {0xFF, 0xEE, 0x00, 14, 'A', 'd', 'o', 'b', 'e', 0, 0, 0, 0, 0,
                  0, static_cast<GByte>(adobeTransform)});
    }
    const size_t sofLen = 8 + 3 * numComponents;
    buf.insert(buf.end(), {0xFF, sofMarker, static_cast<GByte>(sofLen >> 8),
                           static_cast<GByte>(sofLen & 0xFF), 8, 0, 16, 0, 16,
                           numComponents});
    for (GByte i = 1; i <= numComponents; ++i) {
      buf.push_back(i);
      buf.push_back(i == 1 ? subsampling : 0x11);
      buf.push_back(0);
    }
    // SOS marker
    buf.insert(buf.end(), {0xFF, 0xDA, 0x00, 0x04, 0x00, 0x00});
    return buf;
  };

  constexpr std::array<GByte, 13> kSofMarkers = {0xC0, 0xC1, 0xC2, 0xC3, 0xC5,
                                                 0xC6, 0xC7, 0xC9, 0xCA, 0xCB,
                                                 0xCD, 0xCE, 0xCF};
  for (GByte sof : kSofMarkers) {
    const auto buf = MakeJpegBuf(sof, 1, 0x11, -1);
    EXPECT_THAT(GDALGetCompressionFormatForJPEG(buf.data(), buf.size()),
                HasSubstr("JPEG;frame_type=SOF"));
  }

  struct JpegColorCase {
    GByte num_components;
    GByte subsampling;
    int adobe_transform;
    const char* expected_substr;
  };
  constexpr JpegColorCase kColorCases[] = {
      // 3 components with 4:4:4, 4:2:0, 4:2:2 and Adobe transforms.
      {.num_components = 3,
       .subsampling = 0x11,
       .adobe_transform = 0,
       .expected_substr = "subsampling=4:4:4;colorspace=RGB"},
      {.num_components = 3,
       .subsampling = 0x22,
       .adobe_transform = 1,
       .expected_substr = "subsampling=4:2:0;colorspace=YCbCr"},
      {.num_components = 3,
       .subsampling = 0x21,
       .adobe_transform = -1,
       .expected_substr = "subsampling=4:2:2;colorspace=YCbCr"},
      // 4 components with CMYK and YCCK.
      {.num_components = 4,
       .subsampling = 0x11,
       .adobe_transform = 0,
       .expected_substr = "colorspace=CMYK"},
      {.num_components = 4,
       .subsampling = 0x11,
       .adobe_transform = 2,
       .expected_substr = "colorspace=YCCK"},
      {.num_components = 4,
       .subsampling = 0x11,
       .adobe_transform = -1,
       .expected_substr = "colorspace=CMYK"},
  };
  for (const auto& c : kColorCases) {
    const auto buf =
        MakeJpegBuf(0xC0, c.num_components, c.subsampling, c.adobe_transform);
    EXPECT_THAT(GDALGetCompressionFormatForJPEG(buf.data(), buf.size()),
                HasSubstr(c.expected_substr));
  }
}

TEST(GdalMiscTest, CacheDirectoryExistAndGeoTransformApply) {
  GDALRegister_GTiff();
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kVsiMemDir); };

  const std::filesystem::path temp_root(::testing::TempDir());
  {
    const std::string cache_dir = (temp_root / "my_gdal_cache").string();
    CPLConfigOptionSetter oSetter("GDAL_CACHE_DIRECTORY", cache_dir.c_str(),
                                  false);
    EXPECT_EQ(cache_dir, GDALGetCacheDirectory());
  }
  {
    const std::string xdg_cache = (temp_root / "xdg_cache").string();
    CPLConfigOptionSetter oSetter("XDG_CACHE_HOME", xdg_cache.c_str(), false);
    EXPECT_EQ((std::filesystem::path(xdg_cache) / "gdal").string(),
              GDALGetCacheDirectory());
  }
  {
    const std::string my_home = (temp_root / "my_home").string();
    CPLConfigOptionSetter oSetter("HOME", my_home.c_str(), false);
    EXPECT_EQ((std::filesystem::path(my_home) / ".gdal").string(),
              GDALGetCacheDirectory());
  }
  {
    const char* pszOrigHome = getenv("HOME");
    std::optional<std::string> saved_home;
    if (pszOrigHome != nullptr) {
      saved_home = pszOrigHome;
      unsetenv("HOME");
    }
    absl::Cleanup restore_home = [&saved_home] {
      if (saved_home.has_value()) {
        setenv("HOME", saved_home->c_str(), 1);
      }
    };
    const std::string cpl_tmp = (temp_root / "cpl_tmp").string();
    CPLConfigOptionSetter oSetter1("CPL_TMPDIR", cpl_tmp.c_str(), false);
    CPLConfigOptionSetter oSetter2("USERNAME", "testuser", false);
    EXPECT_EQ((std::filesystem::path(cpl_tmp) / ".gdal_testuser").string(),
              GDALGetCacheDirectory());
  }

  // GDALDoesFileOrDatasetExist
  const char* pszType = nullptr;
  GDALDriver* poDrv = nullptr;
  EXPECT_FALSE(GDALDoesFileOrDatasetExist(
      "/vsimem/gdal_misc_test/does_not_exist.bin", &pszType, &poDrv));

  constexpr char kExistDir[] = "/vsimem/gdal_misc_test/test_exist_dir";
  ASSERT_EQ(0, VSIMkdir(kVsiMemDir, 0755));
  ASSERT_EQ(0, VSIMkdir(kExistDir, 0755));
  EXPECT_TRUE(GDALDoesFileOrDatasetExist(kExistDir, &pszType, &poDrv));
  EXPECT_STREQ("Directory", pszType);

  {
    constexpr char kPlainFile[] = "/vsimem/gdal_misc_test/test_plain.bin";
    VsiMemTempWrapper plain_file(kPlainFile, "");
    EXPECT_TRUE(GDALDoesFileOrDatasetExist(kPlainFile, &pszType, &poDrv));
    EXPECT_STREQ("File", pszType);
  }

  GDALDriver* poGTiff = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poGTiff);
  constexpr char kTestDs[] = "/vsimem/gdal_misc_test/test_ds.tif";
  {
    GDALDatasetUniquePtr poDS(
        poGTiff->Create(kTestDs, 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDS);
  }
  EXPECT_TRUE(GDALDoesFileOrDatasetExist(kTestDs, &pszType, &poDrv));
  EXPECT_STREQ("Dataset", pszType);
  EXPECT_EQ(poGTiff, poDrv);

  // GDALGeoTransform::Apply(OGREnvelope, GDALRasterWindow) and vice versa.
  GDALGeoTransform gtAligned(100.0, 2.0, 0.0, 200.0, 0.0, -2.0);
  GDALRasterWindow win{10, 20, 30, 40};
  OGREnvelope env;
  EXPECT_TRUE(gtAligned.Apply(win, env));
  EXPECT_DOUBLE_EQ(120.0, env.MinX);
  EXPECT_DOUBLE_EQ(180.0, env.MaxX);
  EXPECT_DOUBLE_EQ(80.0, env.MinY);
  EXPECT_DOUBLE_EQ(160.0, env.MaxY);

  GDALGeoTransform gtInv;
  ASSERT_TRUE(gtAligned.GetInverse(gtInv));
  GDALRasterWindow winOut{};
  EXPECT_TRUE(gtInv.Apply(env, winOut));
  EXPECT_EQ(10, winOut.nXOff);
  EXPECT_EQ(20, winOut.nYOff);
  EXPECT_EQ(30, winOut.nXSize);
  EXPECT_EQ(40, winOut.nYSize);

  GDALGeoTransform gtRotated(100.0, 2.0, 0.5, 200.0, 0.5, -2.0);
  EXPECT_FALSE(gtRotated.Apply(win, env));
  EXPECT_FALSE(gtRotated.Apply(env, winOut));
}

TEST(GdalMiscTest, GDALCanFileAcceptSidecarFile) {
  // Do not call with nullptr.
  struct SidecarCase {
    const char* filename;
    int expected;
  };
  constexpr SidecarCase kCases[] = {
      {.filename = "", .expected = TRUE},
      {.filename = " ", .expected = TRUE},
      {.filename = "\r\n\t\v", .expected = TRUE},
      {.filename = "/vsicurl/", .expected = TRUE},
      {.filename = "/vsicurl/a", .expected = TRUE},
      {.filename = "/vsicurl/?", .expected = FALSE},
      {.filename = "/vsicurl/a/?/b", .expected = FALSE},
      {.filename = "c/vsicurl/?", .expected = FALSE},
      {.filename = "d/vsicurl/a/?/b", .expected = FALSE},
      {.filename = "?/vsicurl/e", .expected = FALSE},
      {.filename = "/vsisubfile/", .expected = FALSE},
      {.filename = "a/vsisubfile/", .expected = TRUE},
      {.filename = "a/vsisubfile/?/vsicurl/e", .expected = FALSE},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(c.expected, GDALCanFileAcceptSidecarFile(c.filename));
  }
}

}  // namespace
}  // namespace autotest2
