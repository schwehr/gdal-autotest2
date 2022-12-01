// Tests gcore/gdal_misc.cpp.
//
// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gunit.h"
#include "autotest2/cpp/util/error_handler.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"

namespace {

TEST(GdalMiscTest, GdalDataTypeUnion) {
  // Tests a subset of type pairs.
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_Unknown));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_Byte));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_UInt16));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_Int16));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_UInt32));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_Int32));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_Float32));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_Float64));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_CInt16));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_CInt32));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_CFloat32));
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnion(GDT_Unknown, GDT_CFloat64));
  EXPECT_EQ(GDT_UInt16, GDALDataTypeUnion(GDT_Byte, GDT_UInt16));
  EXPECT_EQ(GDT_Int16, GDALDataTypeUnion(GDT_Byte, GDT_Int16));
  EXPECT_EQ(GDT_Int32, GDALDataTypeUnion(GDT_Int16, GDT_UInt16));
  EXPECT_EQ(GDT_Float64, GDALDataTypeUnion(GDT_Int16, GDT_UInt32));
  EXPECT_EQ(GDT_Float64, GDALDataTypeUnion(GDT_UInt32, GDT_Int16));
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
}

TEST(GdalMiscTest, GDALDataTypeUnionWithValue) {
  // The 3rd arg is complex.
  EXPECT_EQ(GDT_Unknown, GDALDataTypeUnionWithValue(GDT_Unknown, 1.0, FALSE));
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
  EXPECT_EQ(GDT_Float64,
            GDALDataTypeUnionWithValue(GDT_Byte, -2147483649, FALSE));
  EXPECT_EQ(GDT_UInt32,
            GDALDataTypeUnionWithValue(GDT_Byte, 4294967295, FALSE));
  EXPECT_EQ(GDT_Float64,
            GDALDataTypeUnionWithValue(GDT_Byte, 4294967296, FALSE));
  // TODO(schwehr): More tests
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
  EXPECT_EQ(GDT_Float64, GDALFindDataType(33, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(63, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(64, FALSE, FALSE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(65, FALSE, FALSE, FALSE));

  // Signed int real.
  EXPECT_EQ(GDT_Int16, GDALFindDataType(0, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int16, GDALFindDataType(8, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int16, GDALFindDataType(9, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int16, GDALFindDataType(16, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int32, GDALFindDataType(17, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Int32, GDALFindDataType(32, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(33, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(63, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(64, TRUE, FALSE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(65, TRUE, FALSE, FALSE));

  // Unsigned floating point real
  // TODO(schwehr): What?
  EXPECT_EQ(GDT_Float64, GDALFindDataType(0, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(8, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(9, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(16, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(17, FALSE, TRUE, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataType(32, FALSE, TRUE, FALSE));
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
  // TODO(schwehr): And there was much sadness.
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(0, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(8, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(9, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(16, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(17, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(32, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(33, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(63, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(64, FALSE, TRUE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(65, FALSE, TRUE, TRUE));

  // Signed and floating point and complex
  // TODO(schwehr): And there was much sadness.
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
  EXPECT_EQ(GDT_CInt32, GDALFindDataType(32, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(33, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(63, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(64, FALSE, FALSE, TRUE));
  EXPECT_EQ(GDT_CFloat64, GDALFindDataType(65, FALSE, FALSE, TRUE));
}

TEST(GdalMiscTest, GDALFindDataTypeForValue) {
  EXPECT_EQ(GDT_Byte, GDALFindDataTypeForValue(1, FALSE));
  EXPECT_EQ(GDT_Int16, GDALFindDataTypeForValue(-1, FALSE));
  EXPECT_EQ(GDT_UInt16, GDALFindDataTypeForValue(20000, FALSE));
  EXPECT_EQ(GDT_Int32, GDALFindDataTypeForValue(-40000, FALSE));
  EXPECT_EQ(GDT_UInt32, GDALFindDataTypeForValue(3000000000, FALSE));
  EXPECT_EQ(GDT_Float64, GDALFindDataTypeForValue(0.1, FALSE));

  EXPECT_EQ(GDT_CFloat64, GDALFindDataTypeForValue(0.1, TRUE));
}

TEST(GdalMiscTest, GDALGetDataTypeSizeBytes) {
  EXPECT_EQ(0, GDALGetDataTypeSizeBytes(GDT_Unknown));

  EXPECT_EQ(1, GDALGetDataTypeSizeBytes(GDT_Byte));

  EXPECT_EQ(2, GDALGetDataTypeSizeBytes(GDT_UInt16));
  EXPECT_EQ(2, GDALGetDataTypeSizeBytes(GDT_Int16));

  EXPECT_EQ(4, GDALGetDataTypeSizeBytes(GDT_UInt32));
  EXPECT_EQ(4, GDALGetDataTypeSizeBytes(GDT_Int32));
  EXPECT_EQ(4, GDALGetDataTypeSizeBytes(GDT_Float32));
  EXPECT_EQ(4, GDALGetDataTypeSizeBytes(GDT_CInt16));

  EXPECT_EQ(8, GDALGetDataTypeSizeBytes(GDT_Float64));
  EXPECT_EQ(8, GDALGetDataTypeSizeBytes(GDT_CInt32));
  EXPECT_EQ(8, GDALGetDataTypeSizeBytes(GDT_CFloat32));

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

// TODO(schwehr): Test .

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
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_UInt16));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Int32));
  EXPECT_FALSE(GDALDataTypeIsComplex(GDT_Float32));
  EXPECT_TRUE(GDALDataTypeIsComplex(GDT_CInt16));
  EXPECT_TRUE(GDALDataTypeIsComplex(GDT_CInt32));
  EXPECT_TRUE(GDALDataTypeIsComplex(GDT_CFloat32));
  EXPECT_TRUE(GDALDataTypeIsComplex(GDT_CFloat64));
}

TEST(GdalMiscTest, GdalDataTypeIsInteger) {
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_Unknown));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_Byte));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_UInt16));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_Int16));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_UInt32));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_Int32));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_Float32));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_Float64));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_CInt16));
  EXPECT_TRUE(GDALDataTypeIsInteger(GDT_CInt32));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_CFloat32));
  EXPECT_FALSE(GDALDataTypeIsInteger(GDT_CFloat64));
}

TEST(GdalMiscTest, GdalGetDataTypeName) {
  EXPECT_STREQ("Unknown", GDALGetDataTypeName(GDT_Unknown));
  EXPECT_STREQ("Byte", GDALGetDataTypeName(GDT_Byte));
  EXPECT_STREQ("CInt16", GDALGetDataTypeName(GDT_CInt16));
  EXPECT_STREQ("CFloat64", GDALGetDataTypeName(GDT_CFloat64));
}

// TODO(schwehr): Test GDALGetDataTypeByName.

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

  // TODO(schwehr): nan and inf.
}

TEST(GdalMiscTest, GDALGetNonComplexDataType) {
  EXPECT_EQ(GDT_Unknown, GDALGetNonComplexDataType(GDT_Unknown));
  EXPECT_EQ(GDT_Byte, GDALGetNonComplexDataType(GDT_Byte));
  EXPECT_EQ(GDT_UInt16, GDALGetNonComplexDataType(GDT_UInt16));
  EXPECT_EQ(GDT_Int16, GDALGetNonComplexDataType(GDT_Int16));
  EXPECT_EQ(GDT_UInt32, GDALGetNonComplexDataType(GDT_UInt32));
  EXPECT_EQ(GDT_Int32, GDALGetNonComplexDataType(GDT_Int32));
  EXPECT_EQ(GDT_Float32, GDALGetNonComplexDataType(GDT_Float32));
  EXPECT_EQ(GDT_Float64, GDALGetNonComplexDataType(GDT_Float64));

  EXPECT_EQ(GDT_Int16, GDALGetNonComplexDataType(GDT_CInt16));
  EXPECT_EQ(GDT_Int32, GDALGetNonComplexDataType(GDT_CInt32));
  EXPECT_EQ(GDT_Float32, GDALGetNonComplexDataType(GDT_CFloat32));
  EXPECT_EQ(GDT_Float64, GDALGetNonComplexDataType(GDT_CFloat64));
}

TEST(GdalMiscTest, GDALGetAsyncStatusTypeByName) {
  EXPECT_EQ(GARIO_PENDING,  GDALGetAsyncStatusTypeByName("PENDING"));
  EXPECT_EQ(GARIO_UPDATE,  GDALGetAsyncStatusTypeByName("UPDATE"));
  EXPECT_EQ(GARIO_ERROR,  GDALGetAsyncStatusTypeByName("ERROR"));
  EXPECT_EQ(GARIO_COMPLETE,  GDALGetAsyncStatusTypeByName("COMPLETE"));
  EXPECT_EQ(GARIO_ERROR,  GDALGetAsyncStatusTypeByName("Garbage"));
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
  EXPECT_STREQ("Unknown", GDALGetPaletteInterpretationName(
                              static_cast<GDALPaletteInterp>(9999)));
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
  EXPECT_STREQ("Unknown", GDALGetColorInterpretationName(
                              static_cast<GDALColorInterp>(9999)));
}

TEST(GdalMiscTest, GDALGetColorInterpretationByName) {
  EXPECT_EQ(GCI_Undefined, GDALGetColorInterpretationByName("Undefined"));
  EXPECT_EQ(GCI_GrayIndex, GDALGetColorInterpretationByName("gray"));
  EXPECT_EQ(GCI_Undefined, GDALGetColorInterpretationByName("Does not exist"));
}

// TODO(schwehr): Test GDALGetRandomRasterSample.
// TODO(schwehr): Test GDALInitGCPs.
// TODO(schwehr): Test GDALDeinitGCPs.
// TODO(schwehr): Test GDALDuplicateGCPs.
// TODO(schwehr): Test GDALFindAssociatedFile.
// TODO(schwehr): Test GDALLoadOziMapFile.
// TODO(schwehr): Test GDALReadOziMapFile.
// TODO(schwehr): Test GDALLoadTabFile.
// TODO(schwehr): Test GDALReadTabFile.
// TODO(schwehr): Test GDALReadTabFile2.
// TODO(schwehr): Test GDALLoadWorldFile.
// TODO(schwehr): Test GDALReadWorldFile.
// TODO(schwehr): Test GDALReadWorldFile2.
// TODO(schwehr): Test GDALWriteWorldFile.
// TODO(schwehr): Test GDALVersionInfo.
// TODO(schwehr): Test GDALCheckVersion.
// TODO(schwehr): Test GDALDecToDMS.
// TODO(schwehr): Test GDALPackedDMSToDec.
// TODO(schwehr): Test GDALDecToPackedDMS.
// TODO(schwehr): Test GDALGCPsToGeoTransform.
// TODO(schwehr): Test GDALComposeGeoTransforms.
// TODO(schwehr): Test GDALGeneralCmdLineProcessor.
// TODO(schwehr): Test GDALExtractRPCInfo.
// TODO(schwehr): Test GDALFindAssociatedAuxFile.
// TODO(schwehr): Test GDALCheckDatasetDimensions.
// TODO(schwehr): Test GDALCheckBandCount.
// TODO(schwehr): Test GDALSerializeGCPListToXML.
// TODO(schwehr): Test GDALDeserializeGCPListFromXML.
// TODO(schwehr): Test GDALSerializeOpenOptionsToXML()
// TODO(schwehr): Test GDALDeserializeOpenOptionsFromXML()
// TODO(schwehr): Test GDALRasterIOGetResampleAlg()
// TODO(schwehr): Test GDALRasterIOGetResampleAlgStr()
// TODO(schwehr): Test GDALRasterIOExtraArgSetResampleAlg()

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
