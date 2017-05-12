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

// TODO(schwehr): Test GDALFindDataTypeForValue.
// TODO(schwehr): Test GDALGetDataTypeSizeBytes.
// TODO(schwehr): Test GDALGetDataTypeSizeBits.

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

// TODO(schwehr): Test GDALGetAsyncStatusTypeByName.
// TODO(schwehr): Test GDALGetAsyncStatusTypeName.
// TODO(schwehr): Test GDALGetPaletteInterpretationName.
// TODO(schwehr): Test GDALGetColorInterpretationName.
// TODO(schwehr): Test GDALGetColorInterpretationByName.
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

}  // namespace
