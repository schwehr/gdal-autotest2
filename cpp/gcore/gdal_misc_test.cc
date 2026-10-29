// Tests gcore/gdal_misc.cpp.
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

TEST(GdalMiscTest, GdalDataTypeUnion) {
  // Tests a subset of type pairs.
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_Unknown));
  EXPECT_EQ(GDT_Byte, GDALDataTypeUnion(GDT_Unknown, GDT_Byte));
  EXPECT_EQ(GDT_Byte, GDALDataTypeUnion(GDT_Byte, GDT_Unknown));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Byte, GDT_TypeCount));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_TypeCount, GDT_Byte));
  EXPECT_EQ(GDT_UInt16, GDALDataTypeUnion(GDT_Unknown, GDT_UInt16));
  EXPECT_EQ(GDT_Int16, GDALDataTypeUnion(GDT_Unknown, GDT_Int16));
  EXPECT_EQ(GDT_UInt32, GDALDataTypeUnion(GDT_Unknown, GDT_UInt32));
  EXPECT_EQ(GDT_Int32, GDALDataTypeUnion(GDT_Unknown, GDT_Int32));
  EXPECT_EQ(GDT_Float32, GDALDataTypeUnion(GDT_Unknown, GDT_Float32));
  EXPECT_EQ(GDT_Float64, GDALDataTypeUnion(GDT_Unknown, GDT_Float64));
  EXPECT_EQ(GDT_CInt16, GDALDataTypeUnion(GDT_Unknown, GDT_CInt16));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_Unknown, GDT_CInt32));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_Unknown, GDT_CFloat32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_Unknown, GDT_CFloat64));
  EXPECT_EQ(GDT_UInt16, GDALDataTypeUnion(GDT_Byte, GDT_UInt16));
  EXPECT_EQ(GDT_Int16, GDALDataTypeUnion(GDT_Byte, GDT_Int16));
  EXPECT_EQ(GDT_Int16, GDALDataTypeUnion(GDT_Int16, GDT_Byte));
  EXPECT_EQ(GDT_Int16, GDALDataTypeUnion(GDT_Byte, GDT_Int8));
  EXPECT_EQ(GDT_Int16, GDALDataTypeUnion(GDT_Int8, GDT_Byte));
  EXPECT_EQ(GDT_Int32, GDALDataTypeUnion(GDT_Int16, GDT_UInt16));
  EXPECT_EQ(GDT_Int64, GDALDataTypeUnion(GDT_Int16, GDT_UInt32));
  EXPECT_EQ(GDT_Int64, GDALDataTypeUnion(GDT_UInt32, GDT_Int16));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_UInt32, GDT_CInt16));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_Int32, GDT_CInt16));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_Float32, GDT_CInt16));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_Float32, GDT_CInt32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_Float64, GDT_CInt32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CInt16, GDT_UInt32));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt16, GDT_Int32));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CInt16, GDT_CFloat32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CInt16, GDT_CFloat64));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_Byte));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_UInt16));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_Int16));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CInt32, GDT_UInt32));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_Int32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CInt32, GDT_Float32));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_CInt16));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CInt32, GDT_CFloat32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CInt32, GDT_CFloat64));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_Byte));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_UInt16));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_Int16));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat32, GDT_UInt32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat32, GDT_Int32));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_Float32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat32, GDT_Float64));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_CInt16));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat32, GDT_CInt32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_Int32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_Float32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_Float64));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_CInt16));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_CInt32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_CFloat32));

  // For Float16 / RFC 100.
  EXPECT_EQ(GDALDataTypeUnion(GDT_Int64, GDT_Float16), GDT_Float64);
  EXPECT_EQ(GDALDataTypeUnion(GDT_UInt64, GDT_Float16), GDT_Float64);
  EXPECT_EQ(GDALDataTypeUnion(GDT_Float16, GDT_CInt32), GDT_CFloat64);
  EXPECT_EQ(GDALDataTypeUnion(GDT_CInt16, GDT_CFloat16), GDT_CFloat32);
  EXPECT_EQ(GDALDataTypeUnion(GDT_CFloat16, GDT_Byte), GDT_CFloat32);
  EXPECT_EQ(GDALDataTypeUnion(GDT_CFloat16, GDT_UInt16), GDT_CFloat32);
  EXPECT_EQ(GDALDataTypeUnion(GDT_CFloat16, GDT_Int16), GDT_CFloat32);
  EXPECT_EQ(GDALDataTypeUnion(GDT_CFloat16, GDT_UInt32), GDT_CFloat64);
  EXPECT_EQ(GDALDataTypeUnion(GDT_CFloat16, GDT_Int32), GDT_CFloat64);
  EXPECT_EQ(GDALDataTypeUnion(GDT_CFloat16, GDT_Float32), GDT_CFloat32);
  EXPECT_EQ(GDALDataTypeUnion(GDT_CFloat16, GDT_CInt16), GDT_CFloat32);
  EXPECT_EQ(GDALDataTypeUnion(GDT_CFloat16, GDT_CInt32), GDT_CFloat64);
}

TEST(GdalMiscTest, GDALDataTypeUnionWithValue) {
  // The 3rd arg is complex.
  EXPECT_EQ(GDT_Byte, GDALDataTypeUnionWithValue(GDT_Unknown, 1.0, FALSE));
  EXPECT_EQ(GDT_Byte, GDALDataTypeUnionWithValue(GDT_Byte, 1.0, FALSE));
  EXPECT_EQ(GDT_Int16, GDALDataTypeUnionWithValue(GDT_Byte, -1.0, FALSE));
  EXPECT_EQ(GDT_Byte, GDALDataTypeUnionWithValue(GDT_Byte, 255.0, FALSE));
  EXPECT_EQ(GDT_UInt16, GDALDataTypeUnionWithValue(GDT_Byte, 256.0, FALSE));
  EXPECT_EQ(GDT_UInt16, GDALDataTypeUnionWithValue(GDT_Byte, 32767.0, FALSE));
  EXPECT_EQ(GDT_UInt16, GDALDataTypeUnionWithValue(GDT_Byte, 32768.0, FALSE));
  EXPECT_EQ(GDT_Int16, GDALDataTypeUnionWithValue(GDT_Byte, -32768.0, FALSE));
  EXPECT_EQ(GDT_Int32, GDALDataTypeUnionWithValue(GDT_Byte, -32769.0, FALSE));
  EXPECT_EQ(GDT_UInt16, GDALDataTypeUnionWithValue(GDT_Byte, 65535.0, FALSE));
  EXPECT_EQ(GDT_UInt32, GDALDataTypeUnionWithValue(GDT_Byte, 65536.0, FALSE));
  EXPECT_EQ(GDT_UInt32,
            GDALDataTypeUnionWithValue(GDT_Byte, 2147483647, FALSE));
  EXPECT_EQ(GDT_Int32,
            GDALDataTypeUnionWithValue(GDT_Byte, -2147483648, FALSE));
  EXPECT_EQ(GDT_Int64,
            GDALDataTypeUnionWithValue(GDT_Byte, -2147483649, FALSE));
  EXPECT_EQ(GDT_UInt32,
            GDALDataTypeUnionWithValue(GDT_Byte, 4294967295, FALSE));
  EXPECT_EQ(GDT_UInt64,
            GDALDataTypeUnionWithValue(GDT_Byte, 4294967296, FALSE));

  EXPECT_EQ(GDALDataTypeUnionWithValue(
                GDT_Float32, cpl::NumericLimits<double>::quiet_NaN(), false),
            GDT_Float32);
  EXPECT_EQ(GDALDataTypeUnionWithValue(
                GDT_Float32, -cpl::NumericLimits<double>::infinity(), false),
            GDT_Float32);
  EXPECT_EQ(GDALDataTypeUnionWithValue(
                GDT_Float32, -cpl::NumericLimits<double>::infinity(), false),
            GDT_Float32);
  EXPECT_EQ(GDALDataTypeUnionWithValue(GDT_Float64, -99999.9876, false),
            GDT_Float64);
  EXPECT_EQ(GDALDataTypeUnionWithValue(
                GDT_Float64, cpl::NumericLimits<double>::quiet_NaN(), false),
            GDT_Float64);
  EXPECT_EQ(GDALDataTypeUnionWithValue(
                GDT_Float64, -cpl::NumericLimits<double>::infinity(), false),
            GDT_Float64);
  EXPECT_EQ(GDALDataTypeUnionWithValue(
                GDT_Float64, -cpl::NumericLimits<double>::infinity(), false),
            GDT_Float64);

  // Additional branches: GDT_Float16, complex, non-exact float32.
  EXPECT_EQ(GDT_Float32, GDALDataTypeUnionWithValue(GDT_Float16, 1.5, FALSE));
  EXPECT_EQ(GDT_Float32, GDALDataTypeUnionWithValue(GDT_Byte, 0.5, FALSE));
  EXPECT_EQ(GDT_Float64, GDALDataTypeUnionWithValue(GDT_Byte, 0.1, FALSE));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnionWithValue(GDT_Byte, 1.0, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnionWithValue(GDT_CInt16, 0.5, TRUE));
}

TEST(GdalMiscTest, GDALFindDataType) {
  // Unsigned int real.
  EXPECT_EQ(GDT_Byte, GDALFindDataType(-1, FALSE, FALSE, FALSE));

  EXPECT_EQ(GDT_Byte, GDALFindDataType(0, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_Byte, GDALFindDataType(8, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_UInt16, GDALFindDataType(9, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_UInt16, GDALFindDataType(16, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_UInt32, GDALFindDataType(17, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_UInt32, GDALFindDataType(32, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_UInt64, GDALFindDataType(33, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_UInt64, GDALFindDataType(63, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_UInt64, GDALFindDataType(64, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(65, FALSE, FALSE, FALSE));

  // Signed int real.
  EXPECT_EQ(GDT_Int8, GDALFindDataType(0, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int8, GDALFindDataType(8, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int16, GDALFindDataType(9, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int16, GDALFindDataType(16, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int32, GDALFindDataType(17, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int32, GDALFindDataType(32, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int64, GDALFindDataType(33, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int64, GDALFindDataType(63, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int64, GDALFindDataType(64, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(65, TRUE, FALSE, FALSE));

  // Unsigned floating point real
  EXPECT_EQ(GDT_Float32, GDALFindDataType(0, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataType(8, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataType(9, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataType(16, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataType(17, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataType(32, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(33, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(63, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(64, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(65, FALSE, TRUE, FALSE));

  // Signed floating point real.
  EXPECT_EQ(GDT_Float32, GDALFindDataType(0, TRUE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataType(8, TRUE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataType(9, TRUE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataType(16, TRUE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataType(17, TRUE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataType(32, TRUE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(33, TRUE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(63, TRUE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(64, TRUE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(65, TRUE, TRUE, FALSE));

  // Unsigned and floating point and complex.
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(0, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(8, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(9, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(16, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(17, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(32, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(33, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(63, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(64, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(65, FALSE, TRUE, TRUE));

  // Signed and floating point and complex
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(-1, TRUE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(0, TRUE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(8, TRUE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(9, TRUE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(16, TRUE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(17, TRUE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat32, GDALFindDataType(32, TRUE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(33, TRUE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(63, TRUE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(64, TRUE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(65, TRUE, TRUE, TRUE));

  // Signed and int and complex
  EXPECT_EQ(GDT_CInt16, GDALFindDataType(-1, TRUE, FALSE, TRUE));
  EXPECT_EQ(GDT_CInt16, GDALFindDataType(0, TRUE, FALSE, TRUE));
  EXPECT_EQ(GDT_CInt16, GDALFindDataType(8, TRUE, FALSE, TRUE));
  EXPECT_EQ(GDT_CInt16, GDALFindDataType(9, TRUE, FALSE, TRUE));
  EXPECT_EQ(GDT_CInt16, GDALFindDataType(16, TRUE, FALSE, TRUE));
  EXPECT_EQ(GDT_CInt32, GDALFindDataType(17, TRUE, FALSE, TRUE));
  EXPECT_EQ(GDT_CInt32, GDALFindDataType(32, TRUE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(33, TRUE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(63, TRUE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(64, TRUE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(65, TRUE, FALSE, TRUE));

  // Unsigned and int and complex
  // TODO(schwehr): I want a pony.
  EXPECT_EQ(GDT_CInt32, GDALFindDataType(0, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CInt32, GDALFindDataType(8, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CInt32, GDALFindDataType(9, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CInt32, GDALFindDataType(16, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CInt32, GDALFindDataType(17, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(32, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(33, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(63, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(64, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(65, FALSE, FALSE, TRUE));
}

TEST(GdalMiscTest, GDALFindDataTypeForValue) {
  EXPECT_EQ(GDT_Byte, GDALFindDataTypeForValue(1, FALSE));
  EXPECT_EQ(GDT_Int8, GDALFindDataTypeForValue(-1, FALSE));
  EXPECT_EQ(GDT_Int16, GDALFindDataTypeForValue(-200, FALSE));
  EXPECT_EQ(GDT_UInt16, GDALFindDataTypeForValue(20000, FALSE));
  EXPECT_EQ(GDT_UInt16, GDALFindDataTypeForValue(40000, FALSE));
  EXPECT_EQ(GDT_Int32, GDALFindDataTypeForValue(-40000, FALSE));
  EXPECT_EQ(GDT_UInt32, GDALFindDataTypeForValue(3000000000, FALSE));
  EXPECT_EQ(GDT_UInt64, GDALFindDataTypeForValue(5000000000.0, FALSE));
  EXPECT_EQ(GDT_Int64, GDALFindDataTypeForValue(-5000000000.0, FALSE));
  EXPECT_EQ(GDT_Float32, GDALFindDataTypeForValue(0.5, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataTypeForValue(0.1, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataTypeForValue(1e30, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataTypeForValue(-1e30, FALSE));

  EXPECT_EQ(GDT_CFloat32, GDALFindDataTypeForValue(0.5, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataTypeForValue(0.1, TRUE));
}

TEST(GdalMiscTest, GDALGetDataTypeSizeBytes) {
  EXPECT_EQ(0, GDALGetDataTypeSizeBytes(GDT_Unknown));
  EXPECT_EQ(0, GDALGetDataTypeSizeBytes(GDT_TypeCount));

  EXPECT_EQ(1, GDALGetDataTypeSizeBytes(GDT_Byte));
  EXPECT_EQ(1, GDALGetDataTypeSizeBytes(GDT_Int8));

  EXPECT_EQ(2, GDALGetDataTypeSizeBytes(GDT_UInt16));
  EXPECT_EQ(2, GDALGetDataTypeSizeBytes(GDT_Int16));
  EXPECT_EQ(2, GDALGetDataTypeSizeBytes(GDT_Float16));

  EXPECT_EQ(4, GDALGetDataTypeSizeBytes(GDT_UInt32));
  EXPECT_EQ(4, GDALGetDataTypeSizeBytes(GDT_Int32));
  EXPECT_EQ(4, GDALGetDataTypeSizeBytes(GDT_Float32));
  EXPECT_EQ(4, GDALGetDataTypeSizeBytes(GDT_CInt16));
  EXPECT_EQ(4, GDALGetDataTypeSizeBytes(GDT_CFloat16));

  EXPECT_EQ(8, GDALGetDataTypeSizeBytes(GDT_Float64));
  EXPECT_EQ(8, GDALGetDataTypeSizeBytes(GDT_CInt32));
  EXPECT_EQ(8, GDALGetDataTypeSizeBytes(GDT_CFloat32));
  EXPECT_EQ(8, GDALGetDataTypeSizeBytes(GDT_UInt64));
  EXPECT_EQ(8, GDALGetDataTypeSizeBytes(GDT_Int64));

  EXPECT_EQ(16, GDALGetDataTypeSizeBytes(GDT_CFloat64));
}

TEST(GdalMiscTest, GDALGetDataTypeSizeBits) {
  EXPECT_EQ(0, GDALGetDataTypeSizeBits(GDT_Unknown));

  EXPECT_EQ(1 * 8, GDALGetDataTypeSizeBits(GDT_Byte));

  EXPECT_EQ(2 * 8, GDALGetDataTypeSizeBits(GDT_UInt16));
  EXPECT_EQ(2 * 8, GDALGetDataTypeSizeBits(GDT_Int16));

  EXPECT_EQ(4 * 8, GDALGetDataTypeSizeBits(GDT_UInt32));
  EXPECT_EQ(4 * 8, GDALGetDataTypeSizeBits(GDT_Int32));
  EXPECT_EQ(4 * 8, GDALGetDataTypeSizeBits(GDT_Float32));
  EXPECT_EQ(4 * 8, GDALGetDataTypeSizeBits(GDT_CInt16));

  EXPECT_EQ(8 * 8, GDALGetDataTypeSizeBits(GDT_Float64));
  EXPECT_EQ(8 * 8, GDALGetDataTypeSizeBits(GDT_CInt32));
  EXPECT_EQ(8 * 8, GDALGetDataTypeSizeBits(GDT_CFloat32));

  EXPECT_EQ(16 * 8, GDALGetDataTypeSizeBits(GDT_CFloat64));
}

TEST(GdalMiscTest, GdalDataTypeSize) {
  // Tests getting the size in bits.
  EXPECT_EQ(0, GDALGetDataTypeSize(GDT_Unknown));
  EXPECT_EQ(8, GDALGetDataTypeSize(GDT_Byte));
  EXPECT_EQ(16, GDALGetDataTypeSize(GDT_UInt16));
  EXPECT_EQ(32, GDALGetDataTypeSize(GDT_Int32));
  EXPECT_EQ(32, GDALGetDataTypeSize(GDT_CInt16));
  EXPECT_EQ(64, GDALGetDataTypeSize(GDT_CInt32));
  EXPECT_EQ(32, GDALGetDataTypeSize(GDT_Float32));
  EXPECT_EQ(64, GDALGetDataTypeSize(GDT_Float64));
  EXPECT_EQ(64, GDALGetDataTypeSize(GDT_CFloat32));
  EXPECT_EQ(128, GDALGetDataTypeSize(GDT_CFloat64));
}

TEST(GdalMiscTest, GdalDataTypeIsComplex) {
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Unknown));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Byte));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Int8));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_UInt16));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Int16));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Int32));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_UInt32));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Int64));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_UInt64));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Float16));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Float32));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Float64));
  EXPECT_TRUE(GDALDataTypeIsComplex(GDT_CInt16));
  EXPECT_TRUE(GDALDataTypeIsComplex(GDT_CInt32));
  EXPECT_TRUE(GDALDataTypeIsComplex(GDT_CFloat16));
  EXPECT_TRUE(GDALDataTypeIsComplex(GDT_CFloat32));
  EXPECT_TRUE(GDALDataTypeIsComplex(GDT_CFloat64));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_TypeCount));
}

TEST(GdalMiscTest, GdalDataTypeIsFloating) {
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_Unknown));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_Byte));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_Int8));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_UInt16));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_Int16));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_UInt32));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_Int32));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_UInt64));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_Int64));
  EXPECT_TRUE(GDALDataTypeIsFloating(GDT_Float16));
  EXPECT_TRUE(GDALDataTypeIsFloating(GDT_Float32));
  EXPECT_TRUE(GDALDataTypeIsFloating(GDT_Float64));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_CInt16));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_CInt32));
  EXPECT_TRUE(GDALDataTypeIsFloating(GDT_CFloat16));
  EXPECT_TRUE(GDALDataTypeIsFloating(GDT_CFloat32));
  EXPECT_TRUE(GDALDataTypeIsFloating(GDT_CFloat64));
  EXPECT_FALSE(GDALDataTypeIsFloating(GDT_TypeCount));
}

TEST(GdalMiscTest, GdalDataTypeIsInteger) {
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_Unknown));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_Byte));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_Int8));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_UInt16));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_Int16));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_UInt32));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_Int32));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_UInt64));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_Int64));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_Float16));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_Float32));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_Float64));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_CInt16));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_CInt32));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_CFloat16));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_CFloat32));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_CFloat64));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_TypeCount));
}

TEST(GdalMiscTest, GdalDataTypeIsSigned) {
  EXPECT_FALSE(GDALDataTypeIsSigned(GDT_Unknown));
  EXPECT_FALSE(GDALDataTypeIsSigned(GDT_Byte));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_Int8));
  EXPECT_FALSE(GDALDataTypeIsSigned(GDT_UInt16));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_Int16));
  EXPECT_FALSE(GDALDataTypeIsSigned(GDT_UInt32));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_Int32));
  EXPECT_FALSE(GDALDataTypeIsSigned(GDT_UInt64));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_Int64));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_Float16));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_Float32));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_Float64));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_CInt16));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_CInt32));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_CFloat16));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_CFloat32));
  EXPECT_TRUE(GDALDataTypeIsSigned(GDT_CFloat64));
  EXPECT_FALSE(GDALDataTypeIsSigned(GDT_TypeCount));
}

TEST(GdalMiscTest, GDALDataTypeIsConversionLossy) {
  // Complex to non-complex is lossy.
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_CFloat32, GDT_Float32));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_CInt16, GDT_Int16));

  // To integer:
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Float32, GDT_Int32));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Int16, GDT_UInt16));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_UInt32, GDT_UInt16));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_UInt16, GDT_Int16));
  EXPECT_FALSE(GDALDataTypeIsConversionLossy(GDT_Byte, GDT_Int16));
  EXPECT_FALSE(GDALDataTypeIsConversionLossy(GDT_Byte, GDT_UInt16));

  // To Float16:
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Int16, GDT_Float16));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_UInt16, GDT_Float16));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Int32, GDT_Float16));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_UInt32, GDT_Float16));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Int64, GDT_Float16));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_UInt64, GDT_Float16));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Float32, GDT_Float16));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Float64, GDT_Float16));
  EXPECT_FALSE(GDALDataTypeIsConversionLossy(GDT_Byte, GDT_Float16));

  // To Float32:
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Int32, GDT_Float32));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_UInt32, GDT_Float32));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Int64, GDT_Float32));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_UInt64, GDT_Float32));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Float64, GDT_Float32));
  EXPECT_FALSE(GDALDataTypeIsConversionLossy(GDT_Int16, GDT_Float32));

  // To Float64:
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_Int64, GDT_Float64));
  EXPECT_TRUE(GDALDataTypeIsConversionLossy(GDT_UInt64, GDT_Float64));
  EXPECT_FALSE(GDALDataTypeIsConversionLossy(GDT_Int32, GDT_Float64));
}

TEST(GdalMiscTest, GdalGetDataTypeNameAndByName) {
  EXPECT_STREQ("Unknown", GDALGetDataTypeName(GDT_Unknown));
  EXPECT_STREQ("Byte", GDALGetDataTypeName(GDT_Byte));
  EXPECT_STREQ("Int8", GDALGetDataTypeName(GDT_Int8));
  EXPECT_STREQ("UInt16", GDALGetDataTypeName(GDT_UInt16));
  EXPECT_STREQ("Int16", GDALGetDataTypeName(GDT_Int16));
  EXPECT_STREQ("UInt32", GDALGetDataTypeName(GDT_UInt32));
  EXPECT_STREQ("Int32", GDALGetDataTypeName(GDT_Int32));
  EXPECT_STREQ("UInt64", GDALGetDataTypeName(GDT_UInt64));
  EXPECT_STREQ("Int64", GDALGetDataTypeName(GDT_Int64));
  EXPECT_STREQ("Float16", GDALGetDataTypeName(GDT_Float16));
  EXPECT_STREQ("Float32", GDALGetDataTypeName(GDT_Float32));
  EXPECT_STREQ("Float64", GDALGetDataTypeName(GDT_Float64));
  EXPECT_STREQ("CInt16", GDALGetDataTypeName(GDT_CInt16));
  EXPECT_STREQ("CInt32", GDALGetDataTypeName(GDT_CInt32));
  EXPECT_STREQ("CFloat16", GDALGetDataTypeName(GDT_CFloat16));
  EXPECT_STREQ("CFloat32", GDALGetDataTypeName(GDT_CFloat32));
  EXPECT_STREQ("CFloat64", GDALGetDataTypeName(GDT_CFloat64));
  EXPECT_EQ(nullptr, GDALGetDataTypeName(GDT_TypeCount));

  for (int i = 1; i < GDT_TypeCount; ++i) {
    const auto eType = static_cast<GDALDataType>(i);
    EXPECT_EQ(eType, GDALGetDataTypeByName(GDALGetDataTypeName(eType)));
  }
  EXPECT_EQ(GDT_Unknown, GDALGetDataTypeByName("NotAType"));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(GDT_Unknown, GDALGetDataTypeByName(nullptr));
  CPLPopErrorHandler();
}

// Based on autotest/cpp/test_gdal.cpp.
TEST(GdalMiscTest, GdalAdjustValueToDataType) {
  // GDAL uses ints as bools for its C API.
  int bClamped = FALSE;
  int bRounded = FALSE;

  EXPECT_DOUBLE_EQ(
      255.0, GDALAdjustValueToDataType(GDT_Byte, 255.0, nullptr, nullptr));

  EXPECT_DOUBLE_EQ(
      255.0, GDALAdjustValueToDataType(GDT_Byte, 255.0, &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(
      254.0, GDALAdjustValueToDataType(GDT_Byte, 254.4, &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_TRUE(bRounded);

  EXPECT_DOUBLE_EQ(
      0.0, GDALAdjustValueToDataType(GDT_Byte, -1, &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(
      255.0, GDALAdjustValueToDataType(GDT_Byte, 256.0, &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_FALSE(bRounded);

  // Int8
  EXPECT_DOUBLE_EQ(-128.0, GDALAdjustValueToDataType(GDT_Int8, -200.0,
                                                     &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_DOUBLE_EQ(
      127.0, GDALAdjustValueToDataType(GDT_Int8, 200.0, &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);

  EXPECT_DOUBLE_EQ(65535.0, GDALAdjustValueToDataType(GDT_UInt16, 65535.0,
                                                      &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(65534.0, GDALAdjustValueToDataType(GDT_UInt16, 65534.4,
                                                      &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_TRUE(bRounded);

  EXPECT_DOUBLE_EQ(
      0.0, GDALAdjustValueToDataType(GDT_UInt16, -1, &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(65535.0, GDALAdjustValueToDataType(GDT_UInt16, 65536.0,
                                                      &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(-32768.0, GDALAdjustValueToDataType(GDT_Int16, -32768.0,
                                                       &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(32767.0, GDALAdjustValueToDataType(GDT_Int16, 32767.0,
                                                      &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(-32767.0, GDALAdjustValueToDataType(GDT_Int16, -32767.4,
                                                       &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_TRUE(bRounded);

  EXPECT_DOUBLE_EQ(32766.0, GDALAdjustValueToDataType(GDT_Int16, 32766.4,
                                                      &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_TRUE(bRounded);

  EXPECT_DOUBLE_EQ(-32768.0, GDALAdjustValueToDataType(GDT_Int16, -32769.0,
                                                       &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(32767.0, GDALAdjustValueToDataType(GDT_Int16, 32768.0,
                                                      &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(10000000.0, GDALAdjustValueToDataType(GDT_UInt32, 10000000.0,
                                                         &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(10000000.0, GDALAdjustValueToDataType(GDT_UInt32, 10000000.4,
                                                         &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_TRUE(bRounded);

  EXPECT_DOUBLE_EQ(
      0.0, GDALAdjustValueToDataType(GDT_UInt32, -1, &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(
      -10000000.0,
      GDALAdjustValueToDataType(GDT_Int32, -10000000.0, &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(10000000.0, GDALAdjustValueToDataType(GDT_Int32, 10000000.0,
                                                         &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  // Int64 / UInt64
  EXPECT_DOUBLE_EQ(
      100.0, GDALAdjustValueToDataType(GDT_Int64, 100.0, &bClamped, &bRounded));
  EXPECT_DOUBLE_EQ(100.0, GDALAdjustValueToDataType(GDT_UInt64, 100.0,
                                                    &bClamped, &bRounded));

  // Float16
  EXPECT_DOUBLE_EQ(
      1.5, GDALAdjustValueToDataType(GDT_Float16, 1.5, &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_DOUBLE_EQ(
      static_cast<double>(cpl::NumericLimits<GFloat16>::lowest()),
      GDALAdjustValueToDataType(GDT_Float16, -1e10, &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_DOUBLE_EQ(
      static_cast<double>(cpl::NumericLimits<GFloat16>::max()),
      GDALAdjustValueToDataType(GDT_Float16, 1e10, &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_TRUE(std::isnan(GDALAdjustValueToDataType(
      GDT_Float16, std::numeric_limits<double>::quiet_NaN(), &bClamped,
      &bRounded)));

  // Float32
  EXPECT_NEAR(
      1.23, GDALAdjustValueToDataType(GDT_Float32, 1.23, &bClamped, &bRounded),
      0.000001);
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(
      std::numeric_limits<float>::max(),
      GDALAdjustValueToDataType(GDT_Float32, 1e300, &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(
      std::numeric_limits<float>::lowest(),
      GDALAdjustValueToDataType(GDT_Float32, -1e300, &bClamped, &bRounded));
  EXPECT_TRUE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_TRUE(std::isinf(GDALAdjustValueToDataType(
      GDT_Float32, std::numeric_limits<double>::infinity(), &bClamped,
      &bRounded)));

  EXPECT_DOUBLE_EQ(
      0.0, GDALAdjustValueToDataType(GDT_Float64, 0.0, &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(1e-50, GDALAdjustValueToDataType(GDT_Float64, 1e-50,
                                                    &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(-1e40, GDALAdjustValueToDataType(GDT_Float64, -1e40,
                                                    &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(
      1e40, GDALAdjustValueToDataType(GDT_Float64, 1e40, &bClamped, &bRounded));
  EXPECT_FALSE(bClamped);
  EXPECT_FALSE(bRounded);

  EXPECT_DOUBLE_EQ(
      1.0, GDALAdjustValueToDataType(GDT_CInt16, 1.0, &bClamped, &bRounded));
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
  EXPECT_EQ(GDT_Unknown, GDALGetNonComplexDataType(GDT_Unknown));
  EXPECT_EQ(GDT_Byte, GDALGetNonComplexDataType(GDT_Byte));
  EXPECT_EQ(GDT_Int8, GDALGetNonComplexDataType(GDT_Int8));
  EXPECT_EQ(GDT_UInt16, GDALGetNonComplexDataType(GDT_UInt16));
  EXPECT_EQ(GDT_Int16, GDALGetNonComplexDataType(GDT_Int16));
  EXPECT_EQ(GDT_UInt32, GDALGetNonComplexDataType(GDT_UInt32));
  EXPECT_EQ(GDT_Int32, GDALGetNonComplexDataType(GDT_Int32));
  EXPECT_EQ(GDT_UInt64, GDALGetNonComplexDataType(GDT_UInt64));
  EXPECT_EQ(GDT_Int64, GDALGetNonComplexDataType(GDT_Int64));
  EXPECT_EQ(GDT_Float16, GDALGetNonComplexDataType(GDT_Float16));
  EXPECT_EQ(GDT_Float32, GDALGetNonComplexDataType(GDT_Float32));
  EXPECT_EQ(GDT_Float64, GDALGetNonComplexDataType(GDT_Float64));

  EXPECT_EQ(GDT_Int16, GDALGetNonComplexDataType(GDT_CInt16));
  EXPECT_EQ(GDT_Int32, GDALGetNonComplexDataType(GDT_CInt32));
  EXPECT_EQ(GDT_Float16, GDALGetNonComplexDataType(GDT_CFloat16));
  EXPECT_EQ(GDT_Float32, GDALGetNonComplexDataType(GDT_CFloat32));
  EXPECT_EQ(GDT_Float64, GDALGetNonComplexDataType(GDT_CFloat64));
  EXPECT_EQ(GDT_TypeCount, GDALGetNonComplexDataType(GDT_TypeCount));
}

TEST(GdalMiscTest, GDALGetAsyncStatusTypeByName) {
  EXPECT_EQ(GARIO_PENDING,  GDALGetAsyncStatusTypeByName("PENDING"));
  EXPECT_EQ(GARIO_UPDATE,  GDALGetAsyncStatusTypeByName("UPDATE"));
  EXPECT_EQ(GARIO_ERROR,  GDALGetAsyncStatusTypeByName("ERROR"));
  EXPECT_EQ(GARIO_COMPLETE,  GDALGetAsyncStatusTypeByName("COMPLETE"));
  EXPECT_EQ(GARIO_ERROR,  GDALGetAsyncStatusTypeByName("Garbage"));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(GARIO_ERROR, GDALGetAsyncStatusTypeByName(nullptr));
  CPLPopErrorHandler();
}

TEST(GdalMiscTest, GDALGetAsyncStatusTypeName) {
  EXPECT_STREQ("PENDING",  GDALGetAsyncStatusTypeName(GARIO_PENDING));
  EXPECT_STREQ("UPDATE",  GDALGetAsyncStatusTypeName(GARIO_UPDATE));
  EXPECT_STREQ("ERROR",  GDALGetAsyncStatusTypeName(GARIO_ERROR));
  EXPECT_STREQ("COMPLETE",  GDALGetAsyncStatusTypeName(GARIO_COMPLETE));
  EXPECT_EQ(nullptr,  GDALGetAsyncStatusTypeName(GARIO_TypeCount));
}

TEST(GdalMiscTest, GDALGetPaletteInterpretationName) {
  EXPECT_STREQ("Gray", GDALGetPaletteInterpretationName(GPI_Gray));
  EXPECT_STREQ("RGB", GDALGetPaletteInterpretationName(GPI_RGB));
  EXPECT_STREQ("CMYK", GDALGetPaletteInterpretationName(GPI_CMYK));
  EXPECT_STREQ("HLS", GDALGetPaletteInterpretationName(GPI_HLS));
#if !defined(UNDEFINED_BEHAVIOR_SANITIZER)
  EXPECT_STREQ("Unknown", GDALGetPaletteInterpretationName(
                              static_cast<GDALPaletteInterp>(9999)));
#endif
}

TEST(GdalMiscTest, GDALGetColorInterpretationName) {
  EXPECT_STREQ("Undefined", GDALGetColorInterpretationName(GCI_Undefined));
  EXPECT_STREQ("Gray", GDALGetColorInterpretationName(GCI_GrayIndex));
  EXPECT_STREQ("Palette", GDALGetColorInterpretationName(GCI_PaletteIndex));
  EXPECT_STREQ("Red", GDALGetColorInterpretationName(GCI_RedBand));
  EXPECT_STREQ("Green", GDALGetColorInterpretationName(GCI_GreenBand));
  EXPECT_STREQ("Blue", GDALGetColorInterpretationName(GCI_BlueBand));
  EXPECT_STREQ("Alpha", GDALGetColorInterpretationName(GCI_AlphaBand));
  EXPECT_STREQ("Hue", GDALGetColorInterpretationName(GCI_HueBand));
  EXPECT_STREQ("Saturation",
               GDALGetColorInterpretationName(GCI_SaturationBand));
  EXPECT_STREQ("Lightness", GDALGetColorInterpretationName(GCI_LightnessBand));
  EXPECT_STREQ("Cyan", GDALGetColorInterpretationName(GCI_CyanBand));
  EXPECT_STREQ("Magenta", GDALGetColorInterpretationName(GCI_MagentaBand));
  EXPECT_STREQ("Yellow", GDALGetColorInterpretationName(GCI_YellowBand));
  EXPECT_STREQ("Black", GDALGetColorInterpretationName(GCI_BlackBand));
  EXPECT_STREQ("YCbCr_Y", GDALGetColorInterpretationName(GCI_YCbCr_YBand));
  EXPECT_STREQ("YCbCr_Cb", GDALGetColorInterpretationName(GCI_YCbCr_CbBand));
  EXPECT_STREQ("YCbCr_Cr", GDALGetColorInterpretationName(GCI_YCbCr_CrBand));
  EXPECT_STREQ("Pan", GDALGetColorInterpretationName(GCI_PanBand));
  EXPECT_STREQ("Coastal", GDALGetColorInterpretationName(GCI_CoastalBand));
  EXPECT_STREQ("RedEdge", GDALGetColorInterpretationName(GCI_RedEdgeBand));
  EXPECT_STREQ("NIR", GDALGetColorInterpretationName(GCI_NIRBand));
  EXPECT_STREQ("SWIR", GDALGetColorInterpretationName(GCI_SWIRBand));
  EXPECT_STREQ("MWIR", GDALGetColorInterpretationName(GCI_MWIRBand));
  EXPECT_STREQ("LWIR", GDALGetColorInterpretationName(GCI_LWIRBand));
  EXPECT_STREQ("TIR", GDALGetColorInterpretationName(GCI_TIRBand));
  EXPECT_STREQ("OtherIR", GDALGetColorInterpretationName(GCI_OtherIRBand));
  EXPECT_STREQ("IR_Reserved_1",
               GDALGetColorInterpretationName(GCI_IR_Reserved_1));
  EXPECT_STREQ("IR_Reserved_2",
               GDALGetColorInterpretationName(GCI_IR_Reserved_2));
  EXPECT_STREQ("IR_Reserved_3",
               GDALGetColorInterpretationName(GCI_IR_Reserved_3));
  EXPECT_STREQ("IR_Reserved_4",
               GDALGetColorInterpretationName(GCI_IR_Reserved_4));
  EXPECT_STREQ("SAR_Ka", GDALGetColorInterpretationName(GCI_SAR_Ka_Band));
  EXPECT_STREQ("SAR_K", GDALGetColorInterpretationName(GCI_SAR_K_Band));
  EXPECT_STREQ("SAR_Ku", GDALGetColorInterpretationName(GCI_SAR_Ku_Band));
  EXPECT_STREQ("SAR_X", GDALGetColorInterpretationName(GCI_SAR_X_Band));
  EXPECT_STREQ("SAR_C", GDALGetColorInterpretationName(GCI_SAR_C_Band));
  EXPECT_STREQ("SAR_S", GDALGetColorInterpretationName(GCI_SAR_S_Band));
  EXPECT_STREQ("SAR_L", GDALGetColorInterpretationName(GCI_SAR_L_Band));
  EXPECT_STREQ("SAR_P", GDALGetColorInterpretationName(GCI_SAR_P_Band));
  EXPECT_STREQ("SAR_Reserved_1",
               GDALGetColorInterpretationName(GCI_SAR_Reserved_1));
  EXPECT_STREQ("SAR_Reserved_2",
               GDALGetColorInterpretationName(GCI_SAR_Reserved_2));
#if !defined(UNDEFINED_BEHAVIOR_SANITIZER)
  // Undefined behavior sanitizer gives:
  // runtime error: load of value 9999, which is not a valid value for type
  // 'GDALColorInterp'
  EXPECT_STREQ("Undefined", GDALGetColorInterpretationName(
                              static_cast<GDALColorInterp>(9999)));
#endif
}

TEST(GdalMiscTest, GDALGetColorInterpretationByNameAndSTAC) {
  EXPECT_EQ(GCI_Undefined, GDALGetColorInterpretationByName("Undefined"));
  EXPECT_EQ(GCI_GrayIndex, GDALGetColorInterpretationByName("gray"));
  EXPECT_EQ(GCI_GrayIndex, GDALGetColorInterpretationByName("grey"));
  EXPECT_EQ(GCI_Undefined, GDALGetColorInterpretationByName("Does not exist"));
  for (int i = 0; i <= GCI_Max; ++i) {
    const auto eInterp = static_cast<GDALColorInterp>(i);
    EXPECT_EQ(eInterp, GDALGetColorInterpretationByName(
                           GDALGetColorInterpretationName(eInterp)));
  }
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(GCI_Undefined, GDALGetColorInterpretationByName(nullptr));
  CPLPopErrorHandler();

  EXPECT_EQ(GCI_PanBand, GDALGetColorInterpFromSTACCommonName("pan"));
  EXPECT_EQ(GCI_GreenBand, GDALGetColorInterpFromSTACCommonName("green05"));
  EXPECT_EQ(GCI_SWIRBand, GDALGetColorInterpFromSTACCommonName("swir16"));
  EXPECT_EQ(GCI_Undefined, GDALGetColorInterpFromSTACCommonName("nonexistent"));

  EXPECT_STREQ("pan", GDALGetSTACCommonNameFromColorInterp(GCI_PanBand));
  EXPECT_EQ(nullptr, GDALGetSTACCommonNameFromColorInterp(GCI_SWIRBand));
  EXPECT_EQ(nullptr, GDALGetSTACCommonNameFromColorInterp(GCI_Undefined));
}

TEST(GdalMiscTest, GDALGetRandomRasterSample) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(0, GDALGetRandomRasterSample(nullptr, 10, nullptr));
  CPLPopErrorHandler();

  GDALDriver* poMemDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDrv);

  for (int i = 1; i < GDT_TypeCount; ++i) {
    const auto eDT = static_cast<GDALDataType>(i);
    GDALDatasetUniquePtr poDS(poMemDrv->Create("", 10, 10, 1, eDT, nullptr));
    ASSERT_NE(nullptr, poDS);
    GDALRasterBand* poBand = poDS->GetRasterBand(1);
    poBand->Fill(5.0, 2.0);
    if (eDT == GDT_Byte) {
      poBand->SetNoDataValue(5.0);
    }
    float afSamples[20] = {0.0f};
    const int nGot = GDALGetRandomRasterSample(GDALRasterBand::ToHandle(poBand),
                                               10, afSamples);
    if (eDT == GDT_Byte) {
      EXPECT_EQ(0, nGot);
    } else {
      EXPECT_GT(nGot, 0);
    }
  }

  // Test multi-block tiled GTiff with partial edge blocks.
  GDALDriver* poGTiffDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (poGTiffDrv != nullptr) {
    CPLStringList aosCreationOpts;
    aosCreationOpts.SetNameValue("TILED", "YES");
    aosCreationOpts.SetNameValue("BLOCKXSIZE", "16");
    aosCreationOpts.SetNameValue("BLOCKYSIZE", "16");
    const char* pszFile = "/vsimem/random_sample_tiled.tif";
    GDALDatasetUniquePtr poDS(poGTiffDrv->Create(pszFile, 50, 50, 1, GDT_Byte,
                                                 aosCreationOpts.List()));
    ASSERT_NE(nullptr, poDS);
    poDS->GetRasterBand(1)->Fill(42);
    float afSamples[100] = {0.0f};
    EXPECT_GT(
        GDALGetRandomRasterSample(
            GDALRasterBand::ToHandle(poDS->GetRasterBand(1)), 50, afSamples),
        0);
    poDS.reset();
    VSIUnlink(pszFile);
  }
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

  GDAL_GCP rawGCPs[2];
  GDALInitGCPs(0, nullptr);
  GDALDeinitGCPs(0, nullptr);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALInitGCPs(1, nullptr);
  GDALDeinitGCPs(1, nullptr);
  CPLPopErrorHandler();

  GDALInitGCPs(2, rawGCPs);
  CPLFree(rawGCPs[0].pszId);
  rawGCPs[0].pszId = CPLStrdup("raw0");
  rawGCPs[0].dfGCPX = 10.0;
  GDAL_GCP* dupGCPs = GDALDuplicateGCPs(2, rawGCPs);
  EXPECT_STREQ("raw0", dupGCPs[0].pszId);
  EXPECT_DOUBLE_EQ(10.0, dupGCPs[0].dfGCPX);
  GDALDeinitGCPs(2, dupGCPs);
  CPLFree(dupGCPs);
  GDALDeinitGCPs(2, rawGCPs);
}

TEST(GdalMiscTest, GDALFindAssociatedFile) {
  const char* pszBase = "/vsimem/assoc_base.tif";
  const char* pszUpper = "/vsimem/assoc_base.RPB";
  const char* pszLower = "/vsimem/assoc_base2.rpb";
  VSIFCloseL(VSIFOpenL(pszUpper, "wb"));
  VSIFCloseL(VSIFOpenL(pszLower, "wb"));

  // Lowercase ext requested, uppercase exists on disk.
  EXPECT_STREQ(pszUpper,
               GDALFindAssociatedFile(pszBase, "rpb", nullptr, 0).c_str());
  // Uppercase ext requested, lowercase exists on disk.
  EXPECT_STREQ(pszLower, GDALFindAssociatedFile("/vsimem/assoc_base2.tif",
                                                "RPB", nullptr, 0)
                             .c_str());
  // Exact match.
  EXPECT_STREQ(pszUpper,
               GDALFindAssociatedFile(pszBase, "RPB", nullptr, 0).c_str());
  // Not found.
  EXPECT_STREQ(
      "", GDALFindAssociatedFile("/vsimem/nonexistent.tif", "rpb", nullptr, 0)
              .c_str());

  // With sibling files.
  CPLStringList aosSiblings;
  aosSiblings.AddString("assoc_base.rpb");
  EXPECT_STREQ(
      "/vsimem/assoc_base.rpb",
      GDALFindAssociatedFile(pszBase, "rpb", aosSiblings.List(), 0).c_str());
  EXPECT_STREQ(
      "",
      GDALFindAssociatedFile(pszBase, "imd", aosSiblings.List(), 0).c_str());

  VSIUnlink(pszUpper);
  VSIUnlink(pszLower);
}

TEST(GdalMiscTest, OziMapFile) {
  double adfGT[6] = {0.0};
  char* pszWKT = nullptr;
  int nGCPCount = 0;
  GDAL_GCP* pasGCPs = nullptr;

  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(
      GDALLoadOziMapFile(nullptr, adfGT, &pszWKT, &nGCPCount, &pasGCPs));
  EXPECT_FALSE(GDALLoadOziMapFile("/vsimem/no_such.map", nullptr, &pszWKT,
                                  &nGCPCount, &pasGCPs));
  EXPECT_FALSE(GDALLoadOziMapFile("/vsimem/no_such.map", adfGT, &pszWKT,
                                  nullptr, &pasGCPs));
  EXPECT_FALSE(GDALLoadOziMapFile("/vsimem/no_such.map", adfGT, &pszWKT,
                                  &nGCPCount, nullptr));
  EXPECT_FALSE(GDALLoadOziMapFile("/vsimem/no_such.map", adfGT, &pszWKT,
                                  &nGCPCount, &pasGCPs));
  CPLPopErrorHandler();
  EXPECT_FALSE(GDALReadOziMapFile("/vsimem/no_such.tif", adfGT, &pszWKT,
                                  &nGCPCount, &pasGCPs));

  // Invalid header.
  const char* pszBadMap = "/vsimem/bad_ozi.map";
  const char* pszBadContent = "Not an Ozi file\nLine2\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszBadMap, reinterpret_cast<GByte*>(const_cast<char*>(pszBadContent)),
      strlen(pszBadContent), FALSE));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(
      GDALLoadOziMapFile(pszBadMap, adfGT, &pszWKT, &nGCPCount, &pasGCPs));
  CPLPopErrorHandler();
  VSIUnlink(pszBadMap);

  // Valid header, but 0 points.
  const char* pszNoPtsMap = "/vsimem/nopts_ozi.map";
  const char* pszNoPtsContent =
      "OziExplorer Map Data File Version 2.2\n"
      "Test\n"
      "test.tif\n"
      "1 ,Map Code,\n"
      "WGS 84,WGS 84,   0.0000,   0.0000,WGS 84\n"
      "MSF, 0.001\n"
      "Map "
      "Projection,Latitude/Longitude,PolyCal,No,AutoCalOnly,No,BSBUseWPX,No\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszNoPtsMap, reinterpret_cast<GByte*>(const_cast<char*>(pszNoPtsContent)),
      strlen(pszNoPtsContent), FALSE));
  EXPECT_FALSE(
      GDALLoadOziMapFile(pszNoPtsMap, adfGT, &pszWKT, &nGCPCount, &pasGCPs));
  CPLFree(pszWKT);
  pszWKT = nullptr;
  VSIUnlink(pszNoPtsMap);

  // Valid Ozi with 2 geographic points (forms geotransform).
  const char* pszGeoMap = "/vsimem/geo_ozi.map";
  const char* pszGeoContent =
      "OziExplorer Map Data File Version 2.2\n"
      "Test\n"
      "test.tif\n"
      "1 ,Map Code,\n"
      "WGS 84,WGS 84,   0.0000,   0.0000,WGS 84\n"
      "MSF, 2.0\n"
      "Map "
      "Projection,Latitude/Longitude,PolyCal,No,AutoCalOnly,No,BSBUseWPX,No\n"
      "Projection Setup,     0.000000000,     0.000000000,     1.000000000,    "
      "   0.00,       0.00,,,,,\n"
      "Point01,xy,    0,    0,in, deg,  45,  0.0,S, 120,  0.0,W, grid,   ,   , "
      "  ,N\n"
      "Point02,xy,  100,  100,in, deg,  46, 30.0,S, 119, 30.0,W, grid,   ,   , "
      "  ,N\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszGeoMap, reinterpret_cast<GByte*>(const_cast<char*>(pszGeoContent)),
      strlen(pszGeoContent), FALSE));
  EXPECT_TRUE(GDALReadOziMapFile("/vsimem/geo_ozi.tif", adfGT, &pszWKT,
                                 &nGCPCount, &pasGCPs));
  EXPECT_NE(nullptr, pszWKT);
  EXPECT_EQ(0, nGCPCount);
  CPLFree(pszWKT);
  pszWKT = nullptr;
  VSIUnlink(pszGeoMap);

  // Valid Ozi with uppercase .MAP extension and 4 non-affine Cartesian points
  // (returns GCPs).
  const char* pszCartMap = "/vsimem/cart_ozi.MAP";
  const char* pszCartContent =
      "OziExplorer Map Data File Version 2.2\n"
      "Test\n"
      "test.tif\n"
      "1 ,Map Code,\n"
      "WGS 84,WGS 84,   0.0000,   0.0000,WGS 84\n"
      "Map Projection,(UTM) Universal Transverse "
      "Mercator,PolyCal,No,AutoCalOnly,No,BSBUseWPX,No\n"
      "Projection Setup,     0.000000000,     0.000000000,     1.000000000,    "
      "   0.00,       0.00,,,,,\n"
      "Point01,xy,    0,    0,in, deg,    ,     ,N,    ,     ,E, grid, 32, "
      "500000, 4500000,N\n"
      "Point02,xy,  100,    0,in, deg,    ,     ,N,    ,     ,E, grid, 32, "
      "501000, 4500000,N\n"
      "Point03,xy,  100,  100,in, deg,    ,     ,N,    ,     ,E, grid, 32, "
      "505000, 4490000,N\n"
      "Point04,xy,    0,  100,in, deg,    ,     ,N,    ,     ,E, grid, 32, "
      "500000, 4499000,N\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszCartMap, reinterpret_cast<GByte*>(const_cast<char*>(pszCartContent)),
      strlen(pszCartContent), FALSE));
  EXPECT_TRUE(GDALReadOziMapFile("/vsimem/cart_ozi.tif", adfGT, &pszWKT,
                                 &nGCPCount, &pasGCPs));
  EXPECT_EQ(4, nGCPCount);
  EXPECT_NE(nullptr, pasGCPs);
  GDALDeinitGCPs(nGCPCount, pasGCPs);
  CPLFree(pasGCPs);
  CPLFree(pszWKT);
  VSIUnlink(pszCartMap);
}

TEST(GdalMiscTest, TabFile) {
  double adfGT[6] = {0.0};
  char* pszWKT = nullptr;
  int nGCPCount = 0;
  GDAL_GCP* pasGCPs = nullptr;
  char* pszTabOut = nullptr;

  EXPECT_FALSE(GDALLoadTabFile("/vsimem/no_such.tab", adfGT, &pszWKT,
                               &nGCPCount, &pasGCPs));
  EXPECT_FALSE(GDALReadTabFile("/vsimem/no_such.tif", adfGT, &pszWKT,
                               &nGCPCount, &pasGCPs));
  EXPECT_FALSE(GDALReadTabFile2("/vsisubfile/0_10,/vsimem/no_such.tif", adfGT,
                                &pszWKT, &nGCPCount, &pasGCPs, nullptr,
                                &pszTabOut));

  // Non-RASTER type tab file.
  const char* pszNativeTab = "/vsimem/native.tab";
  const char* pszNativeContent =
      "!table\n"
      "!version 300\n"
      "Definition Table\n"
      "  Type NATIVE\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszNativeTab,
      reinterpret_cast<GByte*>(const_cast<char*>(pszNativeContent)),
      strlen(pszNativeContent), FALSE));
  EXPECT_FALSE(
      GDALLoadTabFile(pszNativeTab, adfGT, &pszWKT, &nGCPCount, &pasGCPs));
  VSIUnlink(pszNativeTab);

  // RASTER type with 0 GCPs.
  const char* pszEmptyTab = "/vsimem/empty.tab";
  const char* pszEmptyContent =
      "!table\n"
      "!version 300\n"
      "Definition Table\n"
      "  File \"empty.tif\"\n"
      "  Type \"RASTER\"\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszEmptyTab, reinterpret_cast<GByte*>(const_cast<char*>(pszEmptyContent)),
      strlen(pszEmptyContent), FALSE));
  EXPECT_FALSE(
      GDALLoadTabFile(pszEmptyTab, adfGT, &pszWKT, &nGCPCount, &pasGCPs));
  VSIUnlink(pszEmptyTab);

  // RASTER type with projected CoordSys + Units "degree" and 4 corner GCPs.
  const char* pszValidTab = "/vsimem/valid.tab";
  const char* pszValidContent =
      "!table\n"
      "!version 300\n"
      "Definition Table\n"
      "  File \"valid.tif\"\n"
      "  Type \"RASTER\"\n"
      "  (10.0, 50.0) (0, 0) Label \"Pt 1\",\n"
      "  (20.0, 50.0) (100, 0) Label \"Pt 2\",\n"
      "  (20.0, 40.0) (100, 100) Label \"Pt 3\",\n"
      "  (10.0, 40.0) (0, 100) Label \"Pt 4\"\n"
      "  CoordSys Earth Projection 8, 104, \"m\", 15, 0, 0.9996, 500000, 0\n"
      "  Units \"degree\"\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszValidTab, reinterpret_cast<GByte*>(const_cast<char*>(pszValidContent)),
      strlen(pszValidContent), FALSE));
  EXPECT_TRUE(GDALReadTabFile2("/vsimem/valid.tif", adfGT, &pszWKT, &nGCPCount,
                               &pasGCPs, nullptr, &pszTabOut));
  EXPECT_STREQ("/vsimem/valid.tab", pszTabOut);
  EXPECT_NE(nullptr, pszWKT);
  EXPECT_EQ(0, nGCPCount);
  CPLFree(pszTabOut);
  pszTabOut = nullptr;
  CPLFree(pszWKT);
  pszWKT = nullptr;

  // Test with sibling files.
  CPLStringList aosSiblings;
  aosSiblings.AddString("valid.tab");
  EXPECT_TRUE(GDALReadTabFile2("/vsimem/valid.tif", adfGT, &pszWKT, &nGCPCount,
                               &pasGCPs, aosSiblings.List(), &pszTabOut));
  EXPECT_STREQ("/vsimem/valid.tab", pszTabOut);
  CPLFree(pszTabOut);
  pszTabOut = nullptr;
  CPLFree(pszWKT);
  pszWKT = nullptr;

  CPLStringList aosOtherSiblings;
  aosOtherSiblings.AddString("other.tab");
  EXPECT_FALSE(GDALReadTabFile2("/vsimem/valid.tif", adfGT, &pszWKT, &nGCPCount,
                                &pasGCPs, aosOtherSiblings.List(), &pszTabOut));
  VSIUnlink(pszValidTab);

  // Uppercase .TAB with non-affine GCPs.
  const char* pszUpperTab = "/vsimem/upper.TAB";
  const char* pszUpperContent =
      "!table\n"
      "!version 300\n"
      "Definition Table\n"
      "  File \"upper.tif\"\n"
      "  Type \"RASTER\"\n"
      "  (10.0, 50.0) (0, 0) Label \"Pt 1\",\n"
      "  (20.0, 50.0) (100, 0) Label \"Pt 2\",\n"
      "  (25.0, 35.0) (100, 100) Label \"Pt 3\",\n"
      "  (10.0, 40.0) (0, 100) Label \"Pt 4\"\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszUpperTab, reinterpret_cast<GByte*>(const_cast<char*>(pszUpperContent)),
      strlen(pszUpperContent), FALSE));
  EXPECT_TRUE(GDALReadTabFile("/vsimem/upper.tif", adfGT, &pszWKT, &nGCPCount,
                              &pasGCPs));
  EXPECT_EQ(4, nGCPCount);
  ASSERT_NE(nullptr, pasGCPs);
  GDALDeinitGCPs(nGCPCount, pasGCPs);
  CPLFree(pasGCPs);
  CPLFree(pszWKT);
  VSIUnlink(pszUpperTab);
}

TEST(GdalMiscTest, WorldFile) {
  double adfGT[6] = {100.0, 2.0, 0.0, 200.0, 0.0, -2.0};
  double adfGTRead[6] = {0.0};

  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(GDALLoadWorldFile(nullptr, adfGTRead));
  EXPECT_FALSE(GDALLoadWorldFile("/vsimem/no.wld", nullptr));
  EXPECT_FALSE(GDALReadWorldFile(nullptr, "wld", adfGTRead));
  EXPECT_FALSE(GDALReadWorldFile("/vsimem/no.tif", "wld", nullptr));
  EXPECT_FALSE(GDALWriteWorldFile(nullptr, "wld", adfGT));
  EXPECT_FALSE(GDALWriteWorldFile("/vsimem/no.tif", nullptr, adfGT));
  EXPECT_FALSE(GDALWriteWorldFile("/vsimem/no.tif", "wld", nullptr));
  CPLPopErrorHandler();

  EXPECT_FALSE(GDALLoadWorldFile("/vsimem/nonexistent.wld", adfGTRead));
  EXPECT_FALSE(
      GDALReadWorldFile("/vsisubfile/0_10,/vsimem/a.tif", "wld", adfGTRead));
  EXPECT_FALSE(GDALReadWorldFile("/vsimem/a.x", nullptr, adfGTRead));

  // Corrupt world file.
  const char* pszCorrupt = "/vsimem/corrupt.wld";
  const char* pszCorruptContent = "\n0.0\n0.0\n0.0\n0.0\n10.0\n20.0\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszCorrupt,
      reinterpret_cast<GByte*>(const_cast<char*>(pszCorruptContent)),
      strlen(pszCorruptContent), FALSE));
  EXPECT_FALSE(GDALLoadWorldFile(pszCorrupt, adfGTRead));
  VSIUnlink(pszCorrupt);

  // Write and read back with Windows-style extension (.tfw), Unix-style
  // (.tifw), leading dot, sibling files, and uppercase extension.
  const char* pszBase = "/vsimem/world_test.tif";
  EXPECT_TRUE(GDALWriteWorldFile(pszBase, "tfw", adfGT));
  EXPECT_TRUE(GDALReadWorldFile(pszBase, nullptr, adfGTRead));
  for (int i = 0; i < 6; ++i) {
    EXPECT_NEAR(adfGT[i], adfGTRead[i], 1e-7);
  }
  EXPECT_TRUE(GDALReadWorldFile(pszBase, ".tfw", adfGTRead));

  char* pszWorldOut = nullptr;
  GDALGeoTransform gtObj;
  CPLStringList aosSiblings;
  aosSiblings.AddString("world_test.tfw");
  EXPECT_TRUE(GDALReadWorldFile2(pszBase, "tfw", gtObj, aosSiblings.List(),
                                 &pszWorldOut));
  EXPECT_STREQ("/vsimem/world_test.tfw", pszWorldOut);
  CPLFree(pszWorldOut);
  pszWorldOut = nullptr;

  CPLStringList aosEmptySiblings;
  aosEmptySiblings.AddString("other.tfw");
  EXPECT_FALSE(GDALReadWorldFile2(pszBase, "tfw", gtObj,
                                  aosEmptySiblings.List(), &pszWorldOut));
  VSIUnlink("/vsimem/world_test.tfw");

  // Unix-style extension (.tifw) and uppercase (.WLD).
  EXPECT_TRUE(GDALWriteWorldFile(pszBase, "tifw", adfGT));
  EXPECT_TRUE(GDALReadWorldFile(pszBase, nullptr, adfGTRead));
  VSIUnlink("/vsimem/world_test.tifw");

  EXPECT_TRUE(GDALWriteWorldFile(pszBase, "WLD", adfGT));
  EXPECT_TRUE(
      GDALReadWorldFile2(pszBase, "wld", adfGTRead, nullptr, &pszWorldOut));
  EXPECT_STREQ("/vsimem/world_test.WLD", pszWorldOut);
  CPLFree(pszWorldOut);
  VSIUnlink("/vsimem/world_test.WLD");
}

TEST(GdalMiscTest, VersionInfoAndCheckVersion) {
  EXPECT_NE(nullptr, GDALVersionInfo(nullptr));
  EXPECT_NE(nullptr, GDALVersionInfo("VERSION_NUM"));
  EXPECT_NE(nullptr, GDALVersionInfo("RELEASE_DATE"));
  EXPECT_NE(nullptr, GDALVersionInfo("RELEASE_NAME"));
  EXPECT_NE(nullptr, GDALVersionInfo("RELEASE_NICKNAME"));
  EXPECT_NE(nullptr, GDALVersionInfo("--version"));
  EXPECT_NE(nullptr, GDALVersionInfo("BUILD_INFO"));
  EXPECT_NE(nullptr, GDALVersionInfo("LICENSE"));
  // Second call hits TLS cache.
  EXPECT_NE(nullptr, GDALVersionInfo("LICENSE"));

  EXPECT_TRUE(
      GDALCheckVersion(GDAL_VERSION_MAJOR, GDAL_VERSION_MINOR, "unit_test"));
  EXPECT_FALSE(
      GDALCheckVersion(GDAL_VERSION_MAJOR + 1, GDAL_VERSION_MINOR, nullptr));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(GDALCheckVersion(GDAL_VERSION_MAJOR + 1, GDAL_VERSION_MINOR,
                                "unit_test"));
  CPLPopErrorHandler();
}

TEST(GdalMiscTest, DMSConversions) {
  const char* pszDMS = GDALDecToDMS(45.5, "Long", 2);
  EXPECT_NE(nullptr, pszDMS);
  const double dfPacked = GDALDecToPackedDMS(45.5);
  EXPECT_NEAR(45.5, GDALPackedDMSToDec(dfPacked), 1e-6);
}

TEST(GdalMiscTest, GDALGCPsToGeoTransformAndCompose) {
  double adfGT[6] = {0.0};
  GDAL_GCP asGCPs[5];
  GDALInitGCPs(5, asGCPs);

  // < 2 GCPs
  EXPECT_FALSE(GDALGCPsToGeoTransform(1, asGCPs, adfGT, TRUE));

  // 2 GCPs: degenerate (same pixel)
  asGCPs[0].dfGCPPixel = 0;
  asGCPs[0].dfGCPLine = 0;
  asGCPs[1].dfGCPPixel = 0;
  asGCPs[1].dfGCPLine = 10;
  EXPECT_FALSE(GDALGCPsToGeoTransform(2, asGCPs, adfGT, TRUE));

  // 2 GCPs: valid
  asGCPs[1].dfGCPPixel = 10;
  asGCPs[1].dfGCPLine = 10;
  asGCPs[0].dfGCPX = 100.0;
  asGCPs[0].dfGCPY = 200.0;
  asGCPs[1].dfGCPX = 120.0;
  asGCPs[1].dfGCPY = 180.0;
  EXPECT_TRUE(GDALGCPsToGeoTransform(2, asGCPs, adfGT, FALSE));
  EXPECT_DOUBLE_EQ(100.0, adfGT[0]);
  EXPECT_DOUBLE_EQ(2.0, adfGT[1]);
  EXPECT_DOUBLE_EQ(-2.0, adfGT[5]);

  // 4 GCPs: special axis-aligned TL, TR, BR, BL
  asGCPs[0].dfGCPPixel = 0;
  asGCPs[0].dfGCPLine = 0;
  asGCPs[0].dfGCPX = 100.0;
  asGCPs[0].dfGCPY = 200.0;
  asGCPs[1].dfGCPPixel = 10;
  asGCPs[1].dfGCPLine = 0;
  asGCPs[1].dfGCPX = 120.0;
  asGCPs[1].dfGCPY = 200.0;
  asGCPs[2].dfGCPPixel = 10;
  asGCPs[2].dfGCPLine = 10;
  asGCPs[2].dfGCPX = 120.0;
  asGCPs[2].dfGCPY = 180.0;
  asGCPs[3].dfGCPPixel = 0;
  asGCPs[3].dfGCPLine = 10;
  asGCPs[3].dfGCPX = 100.0;
  asGCPs[3].dfGCPY = 180.0;
  EXPECT_TRUE(GDALGCPsToGeoTransform(4, asGCPs, adfGT, FALSE));

  // 3 GCPs: degenerate (all same line)
  asGCPs[2].dfGCPLine = 0;
  EXPECT_FALSE(GDALGCPsToGeoTransform(3, asGCPs, adfGT, TRUE));

  // 3 GCPs: collinear in pixel/line space -> divisor == 0
  asGCPs[0].dfGCPPixel = 0;
  asGCPs[0].dfGCPLine = 0;
  asGCPs[0].dfGCPX = 0;
  asGCPs[0].dfGCPY = 0;
  asGCPs[1].dfGCPPixel = 5;
  asGCPs[1].dfGCPLine = 5;
  asGCPs[1].dfGCPX = 5;
  asGCPs[1].dfGCPY = 5;
  asGCPs[2].dfGCPPixel = 10;
  asGCPs[2].dfGCPLine = 10;
  asGCPs[2].dfGCPX = 10;
  asGCPs[2].dfGCPY = 10;
  EXPECT_FALSE(GDALGCPsToGeoTransform(3, asGCPs, adfGT, TRUE));

  // 5 GCPs: exact fit in general least squares
  asGCPs[0].dfGCPPixel = 0;
  asGCPs[0].dfGCPLine = 0;
  asGCPs[0].dfGCPX = 100.0;
  asGCPs[0].dfGCPY = 200.0;
  asGCPs[1].dfGCPPixel = 10;
  asGCPs[1].dfGCPLine = 0;
  asGCPs[1].dfGCPX = 120.0;
  asGCPs[1].dfGCPY = 200.0;
  asGCPs[2].dfGCPPixel = 10;
  asGCPs[2].dfGCPLine = 10;
  asGCPs[2].dfGCPX = 120.0;
  asGCPs[2].dfGCPY = 180.0;
  asGCPs[3].dfGCPPixel = 0;
  asGCPs[3].dfGCPLine = 10;
  asGCPs[3].dfGCPX = 100.0;
  asGCPs[3].dfGCPY = 180.0;
  asGCPs[4].dfGCPPixel = 5;
  asGCPs[4].dfGCPLine = 5;
  asGCPs[4].dfGCPX = 110.0;
  asGCPs[4].dfGCPY = 190.0;
  EXPECT_TRUE(GDALGCPsToGeoTransform(5, asGCPs, adfGT, FALSE));

  // Perturb 5th point so exact fit fails, approx fit succeeds.
  asGCPs[4].dfGCPX = 150.0;
  EXPECT_FALSE(GDALGCPsToGeoTransform(5, asGCPs, adfGT, FALSE));
  EXPECT_TRUE(GDALGCPsToGeoTransform(5, asGCPs, adfGT, TRUE));

  {
    CPLConfigOptionSetter oSetter("GDAL_GCPS_TO_GEOTRANSFORM_APPROX_OK", "YES",
                                  false);
    EXPECT_TRUE(GDALGCPsToGeoTransform(5, asGCPs, adfGT, FALSE));
  }

  GDALDeinitGCPs(5, asGCPs);

  // ComposeGeoTransforms (including in-place)
  double gt1[6] = {10.0, 2.0, 0.0, 20.0, 0.0, -2.0};
  double gt2[6] = {5.0, 0.5, 0.0, -10.0, 0.0, -0.5};
  double gtOut[6] = {0.0};
  GDALComposeGeoTransforms(gt1, gt2, gtOut);
  EXPECT_DOUBLE_EQ(10.0, gtOut[0]);
  EXPECT_DOUBLE_EQ(1.0, gtOut[1]);
  EXPECT_DOUBLE_EQ(-20.0, gtOut[3]);
  EXPECT_DOUBLE_EQ(1.0, gtOut[5]);
}

TEST(GdalMiscTest, DriverListAndGeneralCmdLineProcessor) {
  GDALAllRegister();

  EXPECT_FALSE(GDALPrintDriverList(0, false).empty());
  EXPECT_FALSE(GDALPrintDriverList(0, true).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_VECTOR, false).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_VECTOR, true).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_MULTIDIM_RASTER, false).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_MULTIDIM_RASTER, true).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_GNM, false).empty());
  EXPECT_FALSE(GDALPrintDriverList(GDAL_OF_GNM, true).empty());

  auto RunCmdLine = [](std::vector<const char*> args, int nOptions = 0) {
    CPLStringList aosList;
    for (const char* arg : args) {
      aosList.AddString(arg);
    }
    char** papszArgv = aosList.StealList();
    char** papszOrig = papszArgv;
    int nRes = GDALGeneralCmdLineProcessor(static_cast<int>(args.size()),
                                           &papszArgv, nOptions);
    if (papszArgv != papszOrig) {
      CSLDestroy(papszOrig);
    }
    CSLDestroy(papszArgv);
    return nRes;
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
  EXPECT_EQ(2, RunCmdLine({"prog", "--config", "FOO_OPT", "BAR_VAL", "--config",
                           "FOO_OPT2=BAR_VAL2", "--config", "CPL_DEBUG", "OFF",
                           "--debug", "OFF", "--locale", "C", "custom_arg"}));
  EXPECT_STREQ("BAR_VAL", CPLGetConfigOption("FOO_OPT", ""));
  EXPECT_STREQ("BAR_VAL2", CPLGetConfigOption("FOO_OPT2", ""));
  CPLSetConfigOption("FOO_OPT", nullptr);
  CPLSetConfigOption("FOO_OPT2", nullptr);
  CPLSetConfigOption("CPL_DEBUG", nullptr);

  // --mempreload
  VSIMkdir("/vsimem/preload_dir", 0755);
  VSIMkdir("/vsimem/preload_dir/subdir", 0755);
  VSIFCloseL(VSIFOpenL("/vsimem/preload_dir/file1.txt", "wb"));
  EXPECT_EQ(1, RunCmdLine({"prog", "--mempreload", "/vsimem/preload_dir"}));
  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL("/vsimem/file1.txt", &sStat));
  VSIUnlink("/vsimem/file1.txt");
  VSIUnlink("/vsimem/preload_dir/file1.txt");
  VSIRmdir("/vsimem/preload_dir/subdir");
  VSIRmdir("/vsimem/preload_dir");

  // --optfile
  const char* pszOptFile = "/vsimem/test_optfile.txt";
  const char* pszOptContent =
      "# comment line\n"
      "\n"
      "--config OPTFILE_KEY=OPTFILE_VAL\n"
      "arg_from_optfile\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszOptFile, reinterpret_cast<GByte*>(const_cast<char*>(pszOptContent)),
      strlen(pszOptContent), FALSE));
  EXPECT_EQ(2, RunCmdLine({"prog", "--optfile", pszOptFile}));
  EXPECT_STREQ("OPTFILE_VAL", CPLGetConfigOption("OPTFILE_KEY", ""));
  CPLSetConfigOption("OPTFILE_KEY", nullptr);
  VSIUnlink(pszOptFile);

  // Nested --optfile triggers warning and does not recurse.
  const char* pszNestedOpt = "/vsimem/nested_optfile.txt";
  const char* pszNestedContent = "--optfile other.txt\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      pszNestedOpt,
      reinterpret_cast<GByte*>(const_cast<char*>(pszNestedContent)),
      strlen(pszNestedContent), FALSE));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(3, RunCmdLine({"prog", "--optfile", pszNestedOpt}));
  // Error cases:
  EXPECT_EQ(-1, RunCmdLine({"prog", "--debug"}));
  EXPECT_EQ(-1, RunCmdLine({"prog", "--config"}));
  EXPECT_EQ(-1, RunCmdLine({"prog", "--config", "ONLY_KEY"}));
  EXPECT_EQ(-1, RunCmdLine({"prog", "--mempreload"}));
  EXPECT_EQ(-1, RunCmdLine({"prog", "--mempreload", "/vsimem/no_such_dir"}));
  EXPECT_EQ(-1, RunCmdLine({"prog", "--optfile"}));
  EXPECT_EQ(-1, RunCmdLine({"prog", "--optfile", "/vsimem/no_such_opt.txt"}));
  EXPECT_EQ(-1, RunCmdLine({"prog", "--format"}));
  EXPECT_EQ(-1, RunCmdLine({"prog", "--format", "NOT_A_DRIVER"}));
  CPLPopErrorHandler();
  VSIUnlink(pszNestedOpt);
}

#undef GDALExtractRPCInfo
extern "C" int CPL_STDCALL GDALExtractRPCInfo(CSLConstList, GDALRPCInfoV1*);

TEST(GdalMiscTest, ExtractRPCInfo) {
  GDALRPCInfoV1 sRPC1;
  GDALRPCInfoV2 sRPC2;
  EXPECT_FALSE(GDALExtractRPCInfo(nullptr, &sRPC1));
  EXPECT_FALSE(GDALExtractRPCInfoV2(nullptr, &sRPC2));

  CPLStringList aosMD;
  aosMD.SetNameValue("LINE_NUM_COEFF", "1 2 3");
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(GDALExtractRPCInfoV2(aosMD.List(), &sRPC2));
  CPLPopErrorHandler();

  const char* pszTwentyVals =
      "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20";
  aosMD.SetNameValue("LINE_NUM_COEFF", pszTwentyVals);
  aosMD.SetNameValue("LINE_DEN_COEFF", pszTwentyVals);
  aosMD.SetNameValue("SAMP_NUM_COEFF", pszTwentyVals);
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
  GDALAllRegister();
  EXPECT_EQ(nullptr,
            GDALFindAssociatedAuxFile("test.aux", GA_ReadOnly, nullptr));
  EXPECT_EQ(nullptr, GDALFindAssociatedAuxFile("", GA_ReadOnly, nullptr));
  EXPECT_EQ(nullptr, GDALFindAssociatedAuxFile("/vsimem/no_aux.tif",
                                               GA_ReadOnly, nullptr));

  GDALDriver* poHFADrv = GetGDALDriverManager()->GetDriverByName("HFA");
  GDALDriver* poMemDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (poHFADrv != nullptr && poMemDrv != nullptr) {
    // 1. HFA file without HFA_DEPENDENT_FILE -> ignored.
    {
      GDALDatasetUniquePtr poHFA(
          poHFADrv->Create("/vsimem/aux1.aux", 10, 10, 1, GDT_Byte, nullptr));
    }
    EXPECT_EQ(nullptr, GDALFindAssociatedAuxFile("/vsimem/aux1.tif",
                                                 GA_ReadOnly, nullptr));
    VSIUnlink("/vsimem/aux1.aux");

    // 2. HFA file with AUX=YES and DEPENDENT_FILE matching, uppercase .AUX
    {
      CPLStringList aosOpts;
      aosOpts.SetNameValue("AUX", "YES");
      aosOpts.SetNameValue("DEPENDENT_FILE", "aux2.tif");
      GDALDatasetUniquePtr poHFA(poHFADrv->Create("/vsimem/aux2.AUX", 10, 10, 1,
                                                  GDT_Byte, aosOpts.List()));
    }
    GDALDatasetUniquePtr poDepMatch(
        poMemDrv->Create("", 10, 10, 1, GDT_Byte, nullptr));
    poDepMatch->MarkAsShared();
    GDALDataset* poFound = GDALFindAssociatedAuxFile(
        "/vsimem/aux2.tif", GA_ReadOnly, poDepMatch.get());
    EXPECT_NE(nullptr, poFound);
    GDALClose(poFound);

    // Mismatched dimensions -> ignored.
    GDALDatasetUniquePtr poDepMismatch(
        poMemDrv->Create("", 20, 20, 1, GDT_Byte, nullptr));
    EXPECT_EQ(nullptr,
              GDALFindAssociatedAuxFile("/vsimem/aux2.tif", GA_ReadOnly,
                                        poDepMismatch.get()));
    VSIUnlink("/vsimem/aux2.AUX");

    // 3. Appended .tif.AUX with non-existent vs existing other dependent file.
    {
      CPLStringList aosOpts;
      aosOpts.SetNameValue("AUX", "YES");
      aosOpts.SetNameValue("DEPENDENT_FILE", "/vsimem/other_dep.tif");
      GDALDatasetUniquePtr poHFA(poHFADrv->Create(
          "/vsimem/aux3.tif.AUX", 10, 10, 1, GDT_Byte, aosOpts.List()));
    }
    // When /vsimem/other_dep.tif does NOT exist, it uses the .aux file anyway.
    poFound = GDALFindAssociatedAuxFile("/vsimem/aux3.tif", GA_ReadOnly,
                                        poDepMatch.get());
    EXPECT_NE(nullptr, poFound);
    GDALClose(poFound);

    // When /vsimem/other_dep.tif DOES exist, it ignores the .aux file.
    VSIFCloseL(VSIFOpenL("/vsimem/other_dep.tif", "wb"));
    EXPECT_EQ(nullptr, GDALFindAssociatedAuxFile("/vsimem/aux3.tif",
                                                 GA_ReadOnly, nullptr));
    VSIUnlink("/vsimem/other_dep.tif");
    VSIUnlink("/vsimem/aux3.tif.AUX");
  }
}

TEST(GdalMiscTest, CheckDatasetDimensionsAndBandCount) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(GDALCheckDatasetDimensions(0, 10));
  EXPECT_FALSE(GDALCheckDatasetDimensions(10, -1));
  EXPECT_TRUE(GDALCheckDatasetDimensions(10, 10));

  EXPECT_FALSE(GDALCheckBandCount(-1, TRUE));
  EXPECT_FALSE(GDALCheckBandCount(0, FALSE));
  EXPECT_TRUE(GDALCheckBandCount(0, TRUE));
  EXPECT_TRUE(GDALCheckBandCount(3, FALSE));
  EXPECT_FALSE(GDALCheckBandCount(70000, FALSE));
  {
    CPLConfigOptionSetter oSetter("GDAL_MAX_BAND_COUNT", "10", false);
    EXPECT_FALSE(GDALCheckBandCount(11, FALSE));
    EXPECT_TRUE(GDALCheckBandCount(10, FALSE));
  }
  CPLPopErrorHandler();
}

TEST(GdalMiscTest, SerializeDeserializeGCPListXML) {
  OGRSpatialReference oSRS;
  oSRS.SetWellKnownGeogCS("WGS84");
  oSRS.SetDataAxisToSRSAxisMapping({2, 1});

  std::vector<gdal::GCP> asGCPs;
  asGCPs.emplace_back("1", "corner", 0.5, 1.5, 10.0, 20.0, 30.0);
  asGCPs.emplace_back("2", "", 10.5, 11.5, 15.0, 25.0, 0.0);

  CPLXMLNode* psRoot = CPLCreateXMLNode(nullptr, CXT_Element, "PAMDataset");
  GDALSerializeGCPListToXML(psRoot, asGCPs, &oSRS);

  CPLXMLNode* psGCPList = CPLGetXMLNode(psRoot, "GCPList");
  ASSERT_NE(nullptr, psGCPList);

  // Add a non-GCP child and invalid GCP nodes to test error recovery.
  CPLCreateXMLNode(psGCPList, CXT_Element, "NotAGCP");
  CPLXMLNode* psBadGCP1 = CPLCreateXMLNode(psGCPList, CXT_Element, "GCP");
  CPLSetXMLValue(psBadGCP1, "#Pixel", "not_a_number");
  CPLSetXMLValue(psBadGCP1, "#Z", "not_a_number");

  std::vector<gdal::GCP> asOutGCPs;
  OGRSpatialReference* poOutSRS = nullptr;
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALDeserializeGCPListFromXML(psGCPList, asOutGCPs, &poOutSRS);
  CPLPopErrorHandler();

  ASSERT_NE(nullptr, poOutSRS);
  EXPECT_EQ(std::vector<int>({2, 1}), poOutSRS->GetDataAxisToSRSAxisMapping());
  delete poOutSRS;

  ASSERT_EQ(2u, asOutGCPs.size());
  EXPECT_STREQ("1", asOutGCPs[0].Id());
  EXPECT_STREQ("corner", asOutGCPs[0].Info());
  EXPECT_DOUBLE_EQ(30.0, asOutGCPs[0].Z());
  EXPECT_DOUBLE_EQ(0.0, asOutGCPs[1].Z());

  CPLDestroyXMLNode(psRoot);
}

TEST(GdalMiscTest, SerializeDeserializeOpenOptionsXML) {
  CPLXMLNode* psRoot = CPLCreateXMLNode(nullptr, CXT_Element, "Root");
  GDALSerializeOpenOptionsToXML(psRoot, nullptr);
  EXPECT_EQ(nullptr, GDALDeserializeOpenOptionsFromXML(psRoot));

  CPLStringList aosOpts;
  aosOpts.SetNameValue("KEY1", "VAL1");
  aosOpts.SetNameValue("KEY2", "VAL2");
  GDALSerializeOpenOptionsToXML(psRoot, aosOpts.List());

  char** papszOut = GDALDeserializeOpenOptionsFromXML(psRoot);
  ASSERT_NE(nullptr, papszOut);
  EXPECT_STREQ("VAL1", CSLFetchNameValue(papszOut, "KEY1"));
  EXPECT_STREQ("VAL2", CSLFetchNameValue(papszOut, "KEY2"));
  CSLDestroy(papszOut);
  CPLDestroyXMLNode(psRoot);
}

TEST(GdalMiscTest, ResampleAlgFunctions) {
  EXPECT_EQ(GRIORA_NearestNeighbour, GDALRasterIOGetResampleAlg("NEAR"));
  EXPECT_EQ(GRIORA_Bilinear, GDALRasterIOGetResampleAlg("BILINEAR"));
  EXPECT_EQ(GRIORA_Cubic, GDALRasterIOGetResampleAlg("CUBIC"));
  EXPECT_EQ(GRIORA_CubicSpline, GDALRasterIOGetResampleAlg("CUBICSPLINE"));
  EXPECT_EQ(GRIORA_Lanczos, GDALRasterIOGetResampleAlg("LANCZOS"));
  EXPECT_EQ(GRIORA_Average, GDALRasterIOGetResampleAlg("AVERAGE"));
  EXPECT_EQ(GRIORA_RMS, GDALRasterIOGetResampleAlg("RMS"));
  EXPECT_EQ(GRIORA_Mode, GDALRasterIOGetResampleAlg("MODE"));
  EXPECT_EQ(GRIORA_Gauss, GDALRasterIOGetResampleAlg("GAUSS"));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(GRIORA_NearestNeighbour, GDALRasterIOGetResampleAlg("INVALID_ALG"));
  CPLPopErrorHandler();

  EXPECT_STREQ("NearestNeighbour",
               GDALRasterIOGetResampleAlg(GRIORA_NearestNeighbour));
  EXPECT_STREQ("Bilinear", GDALRasterIOGetResampleAlg(GRIORA_Bilinear));
  EXPECT_STREQ("Cubic", GDALRasterIOGetResampleAlg(GRIORA_Cubic));
  EXPECT_STREQ("CubicSpline", GDALRasterIOGetResampleAlg(GRIORA_CubicSpline));
  EXPECT_STREQ("Lanczos", GDALRasterIOGetResampleAlg(GRIORA_Lanczos));
  EXPECT_STREQ("Average", GDALRasterIOGetResampleAlg(GRIORA_Average));
  EXPECT_STREQ("RMS", GDALRasterIOGetResampleAlg(GRIORA_RMS));
  EXPECT_STREQ("Mode", GDALRasterIOGetResampleAlg(GRIORA_Mode));
  EXPECT_STREQ("Gauss", GDALRasterIOGetResampleAlg(GRIORA_Gauss));

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
  GDALAllRegister();
  EXPECT_TRUE(GDALCanReliablyUseSiblingFileList("/vsimem/test.tif"));

  const float kMaxFloat = std::numeric_limits<float>::max();
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
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALGetNoDataValueCastToDouble(static_cast<int64_t>((1LL << 60) + 1));
  GDALGetNoDataValueCastToDouble(static_cast<uint64_t>((1ULL << 60) + 1));
  CPLPopErrorHandler();

  // GDALCopyNoDataValue
  GDALDriver* poMemDrv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDrv);
  auto MakeBand = [&](GDALDataType eDT) {
    return GDALDatasetUniquePtr(poMemDrv->Create("", 2, 2, 1, eDT, nullptr));
  };

  {
    auto poSrc = MakeBand(GDT_Int64);
    auto poDstI64 = MakeBand(GDT_Int64);
    auto poDstU64 = MakeBand(GDT_UInt64);
    auto poDstByte = MakeBand(GDT_Byte);
    bool bCannot = false;
    // No nodata set on source
    EXPECT_FALSE(GDALCopyNoDataValue(poDstI64->GetRasterBand(1),
                                     poSrc->GetRasterBand(1), &bCannot));
    EXPECT_TRUE(bCannot);

    poSrc->GetRasterBand(1)->SetNoDataValueAsInt64(100);
    EXPECT_TRUE(GDALCopyNoDataValue(poDstI64->GetRasterBand(1),
                                    poSrc->GetRasterBand(1), &bCannot));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstU64->GetRasterBand(1),
                                    poSrc->GetRasterBand(1), &bCannot));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstByte->GetRasterBand(1),
                                    poSrc->GetRasterBand(1), &bCannot));

    poSrc->GetRasterBand(1)->SetNoDataValueAsInt64(-5);
    EXPECT_FALSE(GDALCopyNoDataValue(poDstU64->GetRasterBand(1),
                                     poSrc->GetRasterBand(1), &bCannot));
  }
  {
    auto poSrc = MakeBand(GDT_UInt64);
    auto poDstU64 = MakeBand(GDT_UInt64);
    auto poDstI64 = MakeBand(GDT_Int64);
    auto poDstByte = MakeBand(GDT_Byte);
    poSrc->GetRasterBand(1)->SetNoDataValueAsUInt64(200);
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
    poSrc->GetRasterBand(1)->SetNoDataValue(50.0);
    EXPECT_TRUE(GDALCopyNoDataValue(poDstI64->GetRasterBand(1),
                                    poSrc->GetRasterBand(1)));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstU64->GetRasterBand(1),
                                    poSrc->GetRasterBand(1)));
    EXPECT_TRUE(GDALCopyNoDataValue(poDstF32->GetRasterBand(1),
                                    poSrc->GetRasterBand(1)));
  }

  // GDALGetNoDataReplacementValue for all types (in-range, max, out-of-range).
  const GDALDataType aeTypes[] = {
      GDT_Byte,   GDT_Int8,  GDT_UInt16,  GDT_Int16,   GDT_UInt32, GDT_Int32,
      GDT_UInt64, GDT_Int64, GDT_Float16, GDT_Float32, GDT_Float64};
  for (GDALDataType eDT : aeTypes) {
    EXPECT_NE(0.0, GDALGetNoDataReplacementValue(eDT, 0.0));
    if (eDT != GDT_Float64) {
      EXPECT_DOUBLE_EQ(0.0, GDALGetNoDataReplacementValue(
                                eDT, std::numeric_limits<double>::infinity()));
    }
  }
  EXPECT_DOUBLE_EQ(254.0, GDALGetNoDataReplacementValue(GDT_Byte, 255.0));
  EXPECT_DOUBLE_EQ(126.0, GDALGetNoDataReplacementValue(GDT_Int8, 127.0));
  EXPECT_DOUBLE_EQ(65534.0, GDALGetNoDataReplacementValue(GDT_UInt16, 65535.0));
  EXPECT_DOUBLE_EQ(32766.0, GDALGetNoDataReplacementValue(GDT_Int16, 32767.0));
  EXPECT_DOUBLE_EQ(4294967294.0,
                   GDALGetNoDataReplacementValue(GDT_UInt32, 4294967295.0));
  EXPECT_DOUBLE_EQ(2147483646.0,
                   GDALGetNoDataReplacementValue(GDT_Int32, 2147483647.0));
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
  const GByte abyNotJpeg[] = {0x00, 0x01, 0x02, 0x03};
  EXPECT_EQ("",
            GDALGetCompressionFormatForJPEG(abyNotJpeg, sizeof(abyNotJpeg)));

  auto MakeJpegBuf = [](GByte sofMarker, GByte numComponents, GByte subsampling,
                        int adobeTransform) {
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

  for (GByte sof : {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB,
                    0xCD, 0xCE, 0xCF}) {
    auto buf = MakeJpegBuf(sof, 1, 0x11, -1);
    EXPECT_NE(std::string::npos,
              GDALGetCompressionFormatForJPEG(buf.data(), buf.size())
                  .find("JPEG;frame_type=SOF"));
  }

  // Test 3 components with 4:4:4, 4:2:0, 4:2:2 and Adobe transforms.
  auto buf444 = MakeJpegBuf(0xC0, 3, 0x11, 0);
  EXPECT_NE(std::string::npos,
            GDALGetCompressionFormatForJPEG(buf444.data(), buf444.size())
                .find("subsampling=4:4:4;colorspace=RGB"));

  auto buf420 = MakeJpegBuf(0xC0, 3, 0x22, 1);
  EXPECT_NE(std::string::npos,
            GDALGetCompressionFormatForJPEG(buf420.data(), buf420.size())
                .find("subsampling=4:2:0;colorspace=YCbCr"));

  auto buf422 = MakeJpegBuf(0xC0, 3, 0x21, -1);
  EXPECT_NE(std::string::npos,
            GDALGetCompressionFormatForJPEG(buf422.data(), buf422.size())
                .find("subsampling=4:2:2;colorspace=YCbCr"));

  // Test 4 components with CMYK and YCCK.
  auto bufCMYK1 = MakeJpegBuf(0xC0, 4, 0x11, 0);
  EXPECT_NE(std::string::npos,
            GDALGetCompressionFormatForJPEG(bufCMYK1.data(), bufCMYK1.size())
                .find("colorspace=CMYK"));
  auto bufYCCK = MakeJpegBuf(0xC0, 4, 0x11, 2);
  EXPECT_NE(std::string::npos,
            GDALGetCompressionFormatForJPEG(bufYCCK.data(), bufYCCK.size())
                .find("colorspace=YCCK"));
  auto bufCMYK2 = MakeJpegBuf(0xC0, 4, 0x11, -1);
  EXPECT_NE(std::string::npos,
            GDALGetCompressionFormatForJPEG(bufCMYK2.data(), bufCMYK2.size())
                .find("colorspace=CMYK"));
}

TEST(GdalMiscTest, CacheDirectoryExistAndGeoTransformApply) {
  GDALAllRegister();
  {
    CPLConfigOptionSetter oSetter("GDAL_CACHE_DIRECTORY", "/tmp/my_gdal_cache",
                                  false);
    EXPECT_EQ("/tmp/my_gdal_cache", GDALGetCacheDirectory());
  }
  {
    CPLConfigOptionSetter oSetter("XDG_CACHE_HOME", "/tmp/xdg_cache", false);
    EXPECT_EQ("/tmp/xdg_cache/gdal", GDALGetCacheDirectory());
  }
  {
    CPLConfigOptionSetter oSetter("HOME", "/tmp/my_home", false);
    EXPECT_EQ("/tmp/my_home/.gdal", GDALGetCacheDirectory());
  }
  const char* pszOrigHome = getenv("HOME");
  std::string osSavedHome = pszOrigHome ? pszOrigHome : "";
  unsetenv("HOME");
  {
    CPLConfigOptionSetter oSetter1("CPL_TMPDIR", "/tmp/cpl_tmp", false);
    CPLConfigOptionSetter oSetter2("USERNAME", "testuser", false);
    EXPECT_EQ("/tmp/cpl_tmp/.gdal_testuser", GDALGetCacheDirectory());
  }
  if (pszOrigHome) {
    setenv("HOME", osSavedHome.c_str(), 1);
  }

  // GDALDoesFileOrDatasetExist
  const char* pszType = nullptr;
  GDALDriver* poDrv = nullptr;
  EXPECT_FALSE(GDALDoesFileOrDatasetExist("/vsimem/does_not_exist.bin",
                                          &pszType, &poDrv));

  VSIMkdir("/vsimem/test_exist_dir", 0755);
  EXPECT_TRUE(
      GDALDoesFileOrDatasetExist("/vsimem/test_exist_dir", &pszType, &poDrv));
  EXPECT_STREQ("Directory", pszType);
  VSIRmdir("/vsimem/test_exist_dir");

  VSIFCloseL(VSIFOpenL("/vsimem/test_plain.bin", "wb"));
  EXPECT_TRUE(
      GDALDoesFileOrDatasetExist("/vsimem/test_plain.bin", &pszType, &poDrv));
  EXPECT_STREQ("File", pszType);
  VSIUnlink("/vsimem/test_plain.bin");

  GDALDriver* poGTiff = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (poGTiff != nullptr) {
    GDALDatasetUniquePtr poDS(
        poGTiff->Create("/vsimem/test_ds.tif", 4, 4, 1, GDT_Byte, nullptr));
    poDS.reset();
    EXPECT_TRUE(
        GDALDoesFileOrDatasetExist("/vsimem/test_ds.tif", &pszType, &poDrv));
    EXPECT_STREQ("Dataset", pszType);
    EXPECT_EQ(poGTiff, poDrv);
    VSIUnlink("/vsimem/test_ds.tif");
  }

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
  EXPECT_TRUE(GDALCanFileAcceptSidecarFile(""));
  EXPECT_TRUE(GDALCanFileAcceptSidecarFile(" "));
  EXPECT_TRUE(GDALCanFileAcceptSidecarFile("\r\n\t\v"));

  EXPECT_TRUE(GDALCanFileAcceptSidecarFile("/vsicurl/"));
  EXPECT_TRUE(GDALCanFileAcceptSidecarFile("/vsicurl/a"));
  EXPECT_FALSE(GDALCanFileAcceptSidecarFile("/vsicurl/?"));
  EXPECT_FALSE(GDALCanFileAcceptSidecarFile("/vsicurl/a/?/b"));
  EXPECT_FALSE(GDALCanFileAcceptSidecarFile("c/vsicurl/?"));
  EXPECT_FALSE(GDALCanFileAcceptSidecarFile("d/vsicurl/a/?/b"));
  EXPECT_FALSE(GDALCanFileAcceptSidecarFile("?/vsicurl/e"));

  EXPECT_FALSE(GDALCanFileAcceptSidecarFile("/vsisubfile/"));
  EXPECT_TRUE(GDALCanFileAcceptSidecarFile("a/vsisubfile/"));

  EXPECT_FALSE(GDALCanFileAcceptSidecarFile("a/vsisubfile/?/vsicurl/e"));
}

}  // namespace
