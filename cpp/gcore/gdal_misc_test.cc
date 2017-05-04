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
  EXPECT_EQ(GDT_Int16, GDALDataTypeUnion(GDT_Int16, GDT_UInt16));
  EXPECT_EQ(GDT_Int32, GDALDataTypeUnion(GDT_Int16, GDT_UInt32));
  EXPECT_EQ(GDT_Int32, GDALDataTypeUnion(GDT_UInt32, GDT_Int16));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_UInt32, GDT_CInt16));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_Int32, GDT_CInt16));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_Float32, GDT_CInt16));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_Float32, GDT_CInt32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_Float64, GDT_CInt32));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt16, GDT_UInt32));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt16, GDT_Int32));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CInt16, GDT_CFloat32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CInt16, GDT_CFloat64));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_Byte));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_UInt16));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_Int16));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_UInt32));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_Int32));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CInt32, GDT_Float32));
  EXPECT_EQ(GDT_CInt32, GDALDataTypeUnion(GDT_CInt32, GDT_CInt16));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CInt32, GDT_CFloat32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CInt32, GDT_CFloat64));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_Byte));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_UInt16));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_Int16));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_UInt32));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_Int32));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_Float32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat32, GDT_Float64));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_CInt16));
  EXPECT_EQ(GDT_CFloat32, GDALDataTypeUnion(GDT_CFloat32, GDT_CInt32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_Int32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_Float32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_Float64));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_CInt16));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_CInt32));
  EXPECT_EQ(GDT_CFloat64, GDALDataTypeUnion(GDT_CFloat64, GDT_CFloat32));
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
