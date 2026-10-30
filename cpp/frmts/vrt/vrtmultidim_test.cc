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

#include <climits>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

class VRTMultidimTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { GDALAllRegister(); }

  void TearDown() override {
    for (const auto& filename : to_unlink_) {
      VSIUnlink(filename.c_str());
    }
    to_unlink_.clear();
  }

  void TrackFile(const std::string& filename) {
    to_unlink_.push_back(filename);
  }

  void CreateGeoTiff(const std::string& filename, int xsize, int ysize,
                     int bands, GDALDataType dt, const double* gt,
                     const char* srs_wkt,
                     const std::vector<std::vector<double>>& band_data) {
    TrackFile(filename);
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(poDriver, nullptr);

    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), xsize, ysize, bands, dt, nullptr));
    ASSERT_NE(poDS, nullptr);

    if (gt != nullptr) {
      EXPECT_EQ(poDS->SetGeoTransform(const_cast<double*>(gt)), CE_None);
    }
    if (srs_wkt != nullptr) {
      OGRSpatialReference srs;
      srs.importFromWkt(srs_wkt);
      EXPECT_EQ(poDS->SetSpatialRef(&srs), CE_None);
    }

    for (int b = 1; b <= bands; ++b) {
      auto* poBand = poDS->GetRasterBand(b);
      if (b - 1 < static_cast<int>(band_data.size())) {
        const auto& data = band_data[b - 1];
        if (!data.empty()) {
          EXPECT_EQ(poBand->RasterIO(GF_Write, 0, 0, xsize, ysize,
                                     const_cast<double*>(data.data()), xsize,
                                     ysize, GDT_Float64, 0, 0),
                    CE_None);
        }
      }
    }
  }

 private:
  std::vector<std::string> to_unlink_;
};

// =========================================================================
// Programmatic Creation of VRT MultiDimensional Dataset, Groups, Dimensions
// =========================================================================

TEST_F(VRTMultidimTest, ProgrammaticGroupAndDimensionCreation) {
  const std::string vrt_path = "/vsimem/prog_multidim.vrt";
  TrackFile(vrt_path);

  auto poDS =
      VRTDataset::CreateVRTMultiDimensional(vrt_path.c_str(), nullptr, nullptr);
  ASSERT_NE(poDS, nullptr);

  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);
  EXPECT_EQ(poRoot->GetName(), "/");
  EXPECT_EQ(poRoot->GetFullName(), "/");

  auto poRootVRT = poDS->GetRootVRTGroup();
  ASSERT_NE(poRootVRT, nullptr);
  EXPECT_EQ(poRootVRT->GetRootGroup(), poRootVRT.get());
  EXPECT_EQ(poRootVRT->GetRootGroupSharedPtr(), poRootVRT);

  // Create SubGroups
  auto poSubGroup = poRoot->CreateGroup("subgroup1");
  ASSERT_NE(poSubGroup, nullptr);
  EXPECT_EQ(poSubGroup->GetName(), "subgroup1");
  EXPECT_EQ(poSubGroup->GetFullName(), "/subgroup1");

  auto poSubGroup2 = poRoot->CreateGroup("subgroup2");
  ASSERT_NE(poSubGroup2, nullptr);

  auto groupNames = poRoot->GetGroupNames();
  EXPECT_EQ(groupNames.size(), 2);

  auto poOpenedSub = poRoot->OpenGroup("subgroup1");
  ASSERT_NE(poOpenedSub, nullptr);
  EXPECT_EQ(poOpenedSub->GetName(), "subgroup1");

  // Create Dimensions
  auto poDimX = poRoot->CreateDimension("dimX", "TYPE_X", "DIR_EAST", 10);
  ASSERT_NE(poDimX, nullptr);
  EXPECT_EQ(poDimX->GetName(), "dimX");
  EXPECT_EQ(poDimX->GetFullName(), "/dimX");
  EXPECT_EQ(poDimX->GetType(), "TYPE_X");
  EXPECT_EQ(poDimX->GetDirection(), "DIR_EAST");
  EXPECT_EQ(poDimX->GetSize(), 10);

  auto poDimY = poRoot->CreateDimension("dimY", "TYPE_Y", "DIR_NORTH", 20);
  ASSERT_NE(poDimY, nullptr);

  auto dims = poRoot->GetDimensions();
  EXPECT_EQ(dims.size(), 2);

  auto poDimFound = poRootVRT->GetDimensionFromFullName("/dimX", true);
  ASSERT_NE(poDimFound, nullptr);
  EXPECT_EQ(poDimFound->GetName(), "dimX");

  // Non-existent group or dimension
  EXPECT_EQ(poRoot->OpenGroup("non_existent"), nullptr);
  EXPECT_EQ(poRootVRT->GetDimensionFromFullName("/non_existent", false),
            nullptr);
}

// =========================================================================
// Attributes: String, Numeric, Multi-Value
// =========================================================================

TEST_F(VRTMultidimTest, AttributesCreationAndIO) {
  const std::string vrt_path = "/vsimem/attr_test.vrt";
  TrackFile(vrt_path);

  auto poDS =
      VRTDataset::CreateVRTMultiDimensional(vrt_path.c_str(), nullptr, nullptr);
  ASSERT_NE(poDS, nullptr);
  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);

  // String Attribute
  auto poAttrStr = poRoot->CreateAttribute(
      "author", {}, GDALExtendedDataType::CreateString());
  ASSERT_NE(poAttrStr, nullptr);
  EXPECT_EQ(poAttrStr->GetName(), "author");
  EXPECT_EQ(poAttrStr->GetFullName(), "/_GLOBAL_/author");

  const char* pszAuthor = "GDAL Team";
  EXPECT_TRUE(poAttrStr->Write(pszAuthor));
  const char* pszReadAuthor = poAttrStr->ReadAsString();
  ASSERT_NE(pszReadAuthor, nullptr);
  EXPECT_STREQ(pszReadAuthor, "GDAL Team");

  // Numeric Attribute (Int32 scalar)
  auto poAttrInt = poRoot->CreateAttribute(
      "version", {}, GDALExtendedDataType::Create(GDT_Int32));
  ASSERT_NE(poAttrInt, nullptr);
  int nVal = 42;
  EXPECT_TRUE(poAttrInt->Write(static_cast<double>(nVal)));
  int nReadVal = poAttrInt->ReadAsInt();
  EXPECT_EQ(nReadVal, 42);

  // Array of numeric values
  std::vector<GUInt64> dims = {3};
  auto poAttrVec = poRoot->CreateAttribute(
      "values", dims, GDALExtendedDataType::Create(GDT_Float64));
  ASSERT_NE(poAttrVec, nullptr);
  std::vector<double> vals = {1.5, 2.5, 3.5};
  EXPECT_TRUE(poAttrVec->Write(vals.data(), vals.size()));
  auto readVals = poAttrVec->ReadAsDoubleArray();
  ASSERT_EQ(readVals.size(), 3);
  EXPECT_DOUBLE_EQ(readVals[0], 1.5);
  EXPECT_DOUBLE_EQ(readVals[1], 2.5);
  EXPECT_DOUBLE_EQ(readVals[2], 3.5);

  // GetAttributes list
  auto attrs = poRoot->GetAttributes();
  EXPECT_EQ(attrs.size(), 3);
  EXPECT_NE(poRoot->GetAttribute("author"), nullptr);
  EXPECT_NE(poRoot->GetAttribute("version"), nullptr);
  EXPECT_NE(poRoot->GetAttribute("values"), nullptr);
  EXPECT_EQ(poRoot->GetAttribute("non_existent"), nullptr);
}

// =========================================================================
// Arrays: InlinedValues & Indexing Variables
// =========================================================================

TEST_F(VRTMultidimTest, ArrayWithInlineValues) {
  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"dimX\" size=\"5\" indexingVariable=\"varX\"/>\n"
      "    <Attribute name=\"test_attr\">\n"
      "      <DataType>String</DataType>\n"
      "      <Value>attr_val</Value>\n"
      "    </Attribute>\n"
      "    <Array name=\"varX\">\n"
      "      <DataType>Int32</DataType>\n"
      "      <DimensionRef ref=\"dimX\"/>\n"
      "      <NoDataValue>-9999</NoDataValue>\n"
      "      <Unit>metre</Unit>\n"
      "      <Offset>10</Offset>\n"
      "      <Scale>2</Scale>\n"
      "      <InlineValues>10 20 30 40 50</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poDS, nullptr);

  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);

  auto poVRTDS = dynamic_cast<VRTDataset*>(poDS.get());
  ASSERT_NE(poVRTDS, nullptr);
  auto poRootVRT = poVRTDS->GetRootVRTGroup();
  ASSERT_NE(poRootVRT, nullptr);

  auto poDimX = poRootVRT->GetDimension("dimX");
  ASSERT_NE(poDimX, nullptr);
  EXPECT_EQ(poDimX->GetSize(), 5);

  auto poVarX = poRoot->OpenMDArray("varX");
  ASSERT_NE(poVarX, nullptr);
  EXPECT_EQ(poVarX->GetDimensionCount(), 1);
  EXPECT_EQ(poVarX->GetUnit(), "metre");
  EXPECT_DOUBLE_EQ(poVarX->GetOffset(), 10.0);
  EXPECT_DOUBLE_EQ(poVarX->GetScale(), 2.0);

  // Check indexing variable
  auto poIdxVar = poDimX->GetIndexingVariable();
  ASSERT_NE(poIdxVar, nullptr);
  EXPECT_EQ(poIdxVar->GetName(), "varX");

  // Read array values
  std::vector<GUInt64> start = {0};
  std::vector<size_t> count = {5};
  std::vector<GInt64> step = {1};
  std::vector<GPtrDiff_t> stride = {1};
  std::vector<int> data(5);
  EXPECT_TRUE(
      poVarX->Read(start.data(), count.data(), step.data(), stride.data(),
                   GDALExtendedDataType::Create(GDT_Int32), data.data()));
  EXPECT_EQ(data[0], 10);
  EXPECT_EQ(data[1], 20);
  EXPECT_EQ(data[2], 30);
  EXPECT_EQ(data[3], 40);
  EXPECT_EQ(data[4], 50);

  // Check nodata
  auto pRawNoData = poVarX->GetRawNoDataValue();
  ASSERT_NE(pRawNoData, nullptr);
  EXPECT_EQ(*reinterpret_cast<const int*>(pRawNoData), -9999);
}

// =========================================================================
// Arrays: RegularlySpacedValues
// =========================================================================

TEST_F(VRTMultidimTest, ArrayWithRegularlySpacedValues) {
  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"time\" size=\"4\"/>\n"
      "    <Array name=\"time_var\">\n"
      "      <DataType>Float64</DataType>\n"
      "      <DimensionRef ref=\"time\"/>\n"
      "      <RegularlySpacedValues start=\"100.0\" increment=\"0.5\"/>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poDS, nullptr);

  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);
  auto poArray = poRoot->OpenMDArray("time_var");
  ASSERT_NE(poArray, nullptr);

  std::vector<GUInt64> start = {0};
  std::vector<size_t> count = {4};
  std::vector<GInt64> step = {1};
  std::vector<GPtrDiff_t> stride = {1};
  std::vector<double> data(4);
  EXPECT_TRUE(
      poArray->Read(start.data(), count.data(), step.data(), stride.data(),
                    GDALExtendedDataType::Create(GDT_Float64), data.data()));
  EXPECT_DOUBLE_EQ(data[0], 100.0);
  EXPECT_DOUBLE_EQ(data[1], 100.5);
  EXPECT_DOUBLE_EQ(data[2], 101.0);
  EXPECT_DOUBLE_EQ(data[3], 101.5);
}

// =========================================================================
// Arrays: SourceFromArray (Mapping raster dataset into MDArray)
// =========================================================================

TEST_F(VRTMultidimTest, ArraySourceFromArray) {
  const std::string tif_path = "/vsimem/source_backing.tif";
  const double gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  std::vector<double> band1 = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  CreateGeoTiff(tif_path, 3, 2, 1, GDT_Float64, gt, nullptr, {band1});

  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"Y\" size=\"2\"/>\n"
      "    <Dimension name=\"X\" size=\"3\"/>\n"
      "    <Array name=\"raster_array\">\n"
      "      <DataType>Float64</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <Source>\n"
      "        <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/source_backing.tif</SourceFilename>\n"
      "        <SourceBand>1</SourceBand>\n"
      "      </Source>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poDS, nullptr);

  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);
  auto poArray = poRoot->OpenMDArray("raster_array");
  ASSERT_NE(poArray, nullptr);
  EXPECT_EQ(poArray->GetDimensionCount(), 2);

  std::vector<GUInt64> start = {0, 0};
  std::vector<size_t> count = {2, 3};
  std::vector<GInt64> step = {1, 1};
  std::vector<GPtrDiff_t> stride = {3, 1};
  std::vector<double> data(6);
  EXPECT_TRUE(
      poArray->Read(start.data(), count.data(), step.data(), stride.data(),
                    GDALExtendedDataType::Create(GDT_Float64), data.data()));
  EXPECT_DOUBLE_EQ(data[0], 1.0);
  EXPECT_DOUBLE_EQ(data[1], 2.0);
  EXPECT_DOUBLE_EQ(data[2], 3.0);
  EXPECT_DOUBLE_EQ(data[3], 4.0);
  EXPECT_DOUBLE_EQ(data[4], 5.0);
  EXPECT_DOUBLE_EQ(data[5], 6.0);
}

// =========================================================================
// 2D VRT RasterBand with VRTArraySource
// =========================================================================

TEST_F(VRTMultidimTest, VRTArraySourceIn2DRasterBand) {
  // 1. Create a multidimensional VRT with a 2D array
  const std::string md_vrt = "/vsimem/md_for_raster.vrt";
  TrackFile(md_vrt);
  const std::string md_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"Y\" size=\"3\"/>\n"
      "    <Dimension name=\"X\" size=\"4\"/>\n"
      "    <Array name=\"grid\">\n"
      "      <DataType>Byte</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <InlineValues>10 11 12 13 14 15 16 17 18 19 20 21</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  VSILFILE* fp = VSIFOpenL(md_vrt.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(md_xml.c_str(), 1, md_xml.size(), fp);
  VSIFCloseL(fp);

  // 2. Create 2D VRT referencing the multidimensional array via ArraySource
  const std::string raster_xml =
      "<VRTDataset rasterXSize=\"4\" rasterYSize=\"3\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <ArraySource>\n"
      "      <SingleSourceArray>\n"
      "        <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/md_for_raster.vrt</SourceFilename>\n"
      "        <SourceArray>/grid</SourceArray>\n"
      "      </SingleSourceArray>\n"
      "    </ArraySource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";

  auto po2D = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      raster_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(po2D, nullptr);
  EXPECT_EQ(po2D->GetRasterXSize(), 4);
  EXPECT_EQ(po2D->GetRasterYSize(), 3);
  EXPECT_EQ(po2D->GetRasterCount(), 1);

  auto* poBand = po2D->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);

  std::vector<GByte> buf(12);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 4, 3, buf.data(), 4, 3, GDT_Byte, 0,
                             0, nullptr),
            CE_None);
  EXPECT_EQ(buf[0], 10);
  EXPECT_EQ(buf[1], 11);
  EXPECT_EQ(buf[11], 21);

  // Statistics / Histogram
  int bSuccess = FALSE;
  double dfMin = poBand->GetMinimum(&bSuccess);
  if (bSuccess) {
    EXPECT_GE(dfMin, 10.0);
  }
  double dfMax = poBand->GetMaximum(&bSuccess);
  if (bSuccess) {
    EXPECT_LE(dfMax, 21.0);
  }

  // Serialize 2D dataset to XML
  auto* poVRTDS = dynamic_cast<VRTDataset*>(po2D.get());
  ASSERT_NE(poVRTDS, nullptr);
  auto* psTree = poVRTDS->SerializeToXML("/vsimem/serialized_2d.vrt");
  ASSERT_NE(psTree, nullptr);
  EXPECT_NE(CPLGetXMLNode(psTree, "VRTRasterBand.ArraySource"), nullptr);
  CPLDestroyXMLNode(psTree);
}

// =========================================================================
// Derived Array Steps: View, Transpose, Resample, ExtractSlice, Pad, Cast
// =========================================================================

TEST_F(VRTMultidimTest, DerivedArraySteps) {
  const std::string base_vrt = "/vsimem/base_derived.vrt";
  TrackFile(base_vrt);
  const std::string base_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"Y\" size=\"4\"/>\n"
      "    <Dimension name=\"X\" size=\"4\"/>\n"
      "    <Array name=\"base_arr\">\n"
      "      <DataType>Int32</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <InlineValues>0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 "
      "15</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  VSILFILE* fp = VSIFOpenL(base_vrt.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(base_xml.c_str(), 1, base_xml.size(), fp);
  VSIFCloseL(fp);

  // 2D VRT with DerivedArray View step
  const std::string view_xml =
      "<VRTDataset rasterXSize=\"2\" rasterYSize=\"2\">\n"
      "  <VRTRasterBand dataType=\"Int32\" band=\"1\">\n"
      "    <ArraySource>\n"
      "      <DerivedArray>\n"
      "        <SingleSourceArray>\n"
      "          <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/base_derived.vrt</SourceFilename>\n"
      "          <SourceArray>/base_arr</SourceArray>\n"
      "        </SingleSourceArray>\n"
      "        <Step>\n"
      "          <View expr=\"[0:2, 0:2]\"/>\n"
      "        </Step>\n"
      "      </DerivedArray>\n"
      "    </ArraySource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";

  auto poDSView = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      view_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poDSView, nullptr);
  auto* poBandView = poDSView->GetRasterBand(1);
  ASSERT_NE(poBandView, nullptr);

  std::vector<int> view_data(4);
  EXPECT_EQ(poBandView->RasterIO(GF_Read, 0, 0, 2, 2, view_data.data(), 2, 2,
                                 GDT_Int32, 0, 0, nullptr),
            CE_None);
  EXPECT_EQ(view_data[0], 0);
  EXPECT_EQ(view_data[1], 1);
  EXPECT_EQ(view_data[2], 4);
  EXPECT_EQ(view_data[3], 5);

  // 2D VRT with DerivedArray Transpose step
  const std::string trans_xml =
      "<VRTDataset rasterXSize=\"4\" rasterYSize=\"4\">\n"
      "  <VRTRasterBand dataType=\"Int32\" band=\"1\">\n"
      "    <ArraySource>\n"
      "      <DerivedArray>\n"
      "        <SingleSourceArray>\n"
      "          <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/base_derived.vrt</SourceFilename>\n"
      "          <SourceArray>/base_arr</SourceArray>\n"
      "        </SingleSourceArray>\n"
      "        <Step>\n"
      "          <Transpose newOrder=\"1,0\"/>\n"
      "        </Step>\n"
      "      </DerivedArray>\n"
      "    </ArraySource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";

  auto poDSTrans = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      trans_xml.c_str(), GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));
  ASSERT_NE(poDSTrans, nullptr);
  auto* poBandTrans = poDSTrans->GetRasterBand(1);
  ASSERT_NE(poBandTrans, nullptr);

  std::vector<int> trans_data(16);
  EXPECT_EQ(poBandTrans->RasterIO(GF_Read, 0, 0, 4, 4, trans_data.data(), 4, 4,
                                  GDT_Int32, 0, 0, nullptr),
            CE_None);
  // (0, 1) in transposed = (1, 0) in original = 4
  EXPECT_EQ(trans_data[1], 4);
}

// =========================================================================
// Serialization, Deserialization & VRT CopyFrom
// =========================================================================

TEST_F(VRTMultidimTest, SerializationAndReopen) {
  const std::string vrt_path = "/vsimem/ser_multidim.vrt";
  TrackFile(vrt_path);

  {
    auto poDS = VRTDataset::CreateVRTMultiDimensional(vrt_path.c_str(), nullptr,
                                                      nullptr);
    ASSERT_NE(poDS, nullptr);
    auto poRoot = poDS->GetRootGroup();
    ASSERT_NE(poRoot, nullptr);

    // Dimension
    auto poDim = poRoot->CreateDimension("dim1", "", "", 10);
    ASSERT_NE(poDim, nullptr);

    // Attribute
    auto poAttr = poRoot->CreateAttribute("desc", {},
                                          GDALExtendedDataType::CreateString());
    ASSERT_NE(poAttr, nullptr);
    poAttr->Write("multidimensional test");

    // Array
    auto poArray = poRoot->CreateMDArray(
        "arr1", {poDim}, GDALExtendedDataType::Create(GDT_Float32));
    ASSERT_NE(poArray, nullptr);
    poArray->SetUnit("km");
    poArray->SetOffset(5.0);
    poArray->SetScale(0.1);
    double dfNoData = -999.0;
    poArray->SetRawNoDataValue(&dfNoData);

    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    poArray->SetSpatialRef(&srs);

    // Close to serialize to file
    poDS->Close();
  }

  // Reopen and verify
  auto poOpened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_path.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(poOpened, nullptr);
  auto poRoot = poOpened->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);

  auto poVRTDS = dynamic_cast<VRTDataset*>(poOpened.get());
  ASSERT_NE(poVRTDS, nullptr);
  auto poRootVRT = poVRTDS->GetRootVRTGroup();
  ASSERT_NE(poRootVRT, nullptr);

  auto poDim = poRootVRT->GetDimension("dim1");
  ASSERT_NE(poDim, nullptr);
  EXPECT_EQ(poDim->GetSize(), 10);

  auto poAttr = poRoot->GetAttribute("desc");
  ASSERT_NE(poAttr, nullptr);
  EXPECT_STREQ(poAttr->ReadAsString(), "multidimensional test");

  auto poArray = poRoot->OpenMDArray("arr1");
  ASSERT_NE(poArray, nullptr);
  EXPECT_EQ(poArray->GetUnit(), "km");
  EXPECT_DOUBLE_EQ(poArray->GetOffset(), 5.0);
  EXPECT_DOUBLE_EQ(poArray->GetScale(), 0.1);
  EXPECT_NE(poArray->GetSpatialRef(), nullptr);
}

// =========================================================================
// MDArray CopyFrom & Dimension SetIndexingVariable
// =========================================================================

TEST_F(VRTMultidimTest, MDArrayCopyFromAndIndexingVariable) {
  // 1. Create a source dataset with 1D regularly spaced array and a 2D array
  const std::string src_vrt = "/vsimem/copy_src.vrt";
  TrackFile(src_vrt);
  const std::string src_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"dim1\" size=\"4\"/>\n"
      "    <Dimension name=\"Y\" size=\"2\"/>\n"
      "    <Dimension name=\"X\" size=\"3\"/>\n"
      "    <Array name=\"reg1d\">\n"
      "      <DataType>Float64</DataType>\n"
      "      <DimensionRef ref=\"dim1\"/>\n"
      "      <RegularlySpacedValues start=\"10.0\" increment=\"2.5\"/>\n"
      "    </Array>\n"
      "    <Array name=\"arr2d\">\n"
      "      <DataType>Int32</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <InlineValues>1 2 3 4 5 6</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  VSILFILE* fp = VSIFOpenL(src_vrt.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(src_xml.c_str(), 1, src_xml.size(), fp);
  VSIFCloseL(fp);

  auto poSrcDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(src_vrt.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(poSrcDS, nullptr);
  auto poSrcRoot = poSrcDS->GetRootGroup();
  ASSERT_NE(poSrcRoot, nullptr);
  auto poSrc1D = poSrcRoot->OpenMDArray("reg1d");
  ASSERT_NE(poSrc1D, nullptr);
  auto poSrc2D = poSrcRoot->OpenMDArray("arr2d");
  ASSERT_NE(poSrc2D, nullptr);

  // 2. Create destination VRT
  const std::string dst_vrt = "/vsimem/copy_dst.vrt";
  TrackFile(dst_vrt);
  {
    auto poDstDS = VRTDataset::CreateVRTMultiDimensional(dst_vrt.c_str(),
                                                         nullptr, nullptr);
    ASSERT_NE(poDstDS, nullptr);
    auto poDstRoot = poDstDS->GetRootGroup();
    ASSERT_NE(poDstRoot, nullptr);
    auto poDstRootVRT = poDstDS->GetRootVRTGroup();
    ASSERT_NE(poDstRootVRT, nullptr);

    // Create dimensions
    auto poDim1 = poDstRoot->CreateDimension("dim1", "", "", 4);
    auto poDimY = poDstRoot->CreateDimension("Y", "", "", 2);
    auto poDimX = poDstRoot->CreateDimension("X", "", "", 3);

    // Create arrays
    auto poDst1D = poDstRoot->CreateMDArray(
        "copied1d", {poDim1}, GDALExtendedDataType::Create(GDT_Float64));
    ASSERT_NE(poDst1D, nullptr);
    GUInt64 nCurCost = 0;
    EXPECT_TRUE(poDst1D->CopyFrom(poSrcDS.get(), poSrc1D.get(), false, nCurCost,
                                  1000, nullptr, nullptr));

    auto poDst2D = poDstRoot->CreateMDArray(
        "copied2d", {poDimY, poDimX}, GDALExtendedDataType::Create(GDT_Int32));
    ASSERT_NE(poDst2D, nullptr);
    EXPECT_TRUE(poDst2D->CopyFrom(poSrcDS.get(), poSrc2D.get(), false, nCurCost,
                                  1000, nullptr, nullptr));

    // Array CreateAttribute
    auto poAttr = poDst2D->CreateAttribute(
        "desc", {}, GDALExtendedDataType::CreateString());
    ASSERT_NE(poAttr, nullptr);
    poAttr->Write("copied array");

    // Set indexing variable on dim1
    auto poDim1VRT = poDstRootVRT->GetDimension("dim1");
    ASSERT_NE(poDim1VRT, nullptr);
    EXPECT_TRUE(poDim1VRT->SetIndexingVariable(poDst1D));
    EXPECT_NE(poDim1VRT->GetIndexingVariable(), nullptr);

    // Set indexing variable to nullptr and restore
    EXPECT_TRUE(poDim1VRT->SetIndexingVariable(nullptr));
    EXPECT_EQ(poDim1VRT->GetIndexingVariable(), nullptr);
    EXPECT_TRUE(poDim1VRT->SetIndexingVariable(poDst1D));

    // Close to serialize to file
    poDstDS->Close();
  }

  // 3. Reopen and verify
  auto poReopened = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(dst_vrt.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(poReopened, nullptr);
  auto poRoot = poReopened->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);

  auto poArr1D = poRoot->OpenMDArray("copied1d");
  ASSERT_NE(poArr1D, nullptr);
  std::vector<GUInt64> start1 = {0};
  std::vector<size_t> count1 = {4};
  std::vector<double> vals1(4);
  EXPECT_TRUE(poArr1D->Read(start1.data(), count1.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Float64),
                            vals1.data()));
  EXPECT_DOUBLE_EQ(vals1[0], 10.0);
  EXPECT_DOUBLE_EQ(vals1[1], 12.5);
  EXPECT_DOUBLE_EQ(vals1[3], 17.5);

  auto poArr2D = poRoot->OpenMDArray("copied2d");
  ASSERT_NE(poArr2D, nullptr);
  std::vector<GUInt64> start2 = {0, 0};
  std::vector<size_t> count2 = {2, 3};
  std::vector<int> vals2(6);
  EXPECT_TRUE(poArr2D->Read(start2.data(), count2.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Int32),
                            vals2.data()));
  EXPECT_EQ(vals2[0], 1);
  EXPECT_EQ(vals2[5], 6);

  auto poAttrRead = poArr2D->GetAttribute("desc");
  ASSERT_NE(poAttrRead, nullptr);
  EXPECT_STREQ(poAttrRead->ReadAsString(), "copied array");
}

// =========================================================================
// InlinedValues Types & Serialization
// =========================================================================

TEST_F(VRTMultidimTest, InlinedValuesTypesAndSerialization) {
  const std::string vrt_path = "/vsimem/inline_types.vrt";
  TrackFile(vrt_path);

  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"dim4\" size=\"4\"/>\n"
      "    <Dimension name=\"dim2\" size=\"2\"/>\n"
      "    <Array name=\"const_arr\">\n"
      "      <DataType>Float32</DataType>\n"
      "      <DimensionRef ref=\"dim4\"/>\n"
      "      <ConstantValue>42.5</ConstantValue>\n"
      "    </Array>\n"
      "    <Array name=\"str_arr\">\n"
      "      <DataType>String</DataType>\n"
      "      <DimensionRef ref=\"dim2\"/>\n"
      "      <InlineValuesWithValueElement>\n"
      "        <Value>hello</Value>\n"
      "        <NullValue/>\n"
      "      </InlineValuesWithValueElement>\n"
      "    </Array>\n"
      "    <Array name=\"sub_arr\">\n"
      "      <DataType>Int32</DataType>\n"
      "      <DimensionRef ref=\"dim4\"/>\n"
      "      <InlineValues offset=\"1\" count=\"2\">100 200</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  VSILFILE* fp = VSIFOpenL(vrt_path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(vrt_xml.c_str(), 1, vrt_xml.size(), fp);
  VSIFCloseL(fp);

  auto poDS = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      vrt_path.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(poDS, nullptr);
  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);

  // 1. ConstantValue
  auto poConst = poRoot->OpenMDArray("const_arr");
  ASSERT_NE(poConst, nullptr);
  std::vector<GUInt64> start4 = {0};
  std::vector<size_t> count4 = {4};
  std::vector<float> fVals(4);
  EXPECT_TRUE(poConst->Read(start4.data(), count4.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Float32),
                            fVals.data()));
  EXPECT_FLOAT_EQ(fVals[0], 42.5f);
  EXPECT_FLOAT_EQ(fVals[3], 42.5f);

  // 2. String Array
  auto poStr = poRoot->OpenMDArray("str_arr");
  ASSERT_NE(poStr, nullptr);
  std::vector<GUInt64> start2 = {0};
  std::vector<size_t> count2 = {2};
  std::vector<char*> strVals(2, nullptr);
  EXPECT_TRUE(poStr->Read(start2.data(), count2.data(), nullptr, nullptr,
                          GDALExtendedDataType::CreateString(),
                          strVals.data()));
  ASSERT_NE(strVals[0], nullptr);
  EXPECT_STREQ(strVals[0], "hello");
  EXPECT_EQ(strVals[1], nullptr);
  CPLFree(strVals[0]);

  // 3. Sub-window array
  auto poSub = poRoot->OpenMDArray("sub_arr");
  ASSERT_NE(poSub, nullptr);
  std::vector<int> subVals(4, -1);
  EXPECT_TRUE(poSub->Read(start4.data(), count4.data(), nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Int32),
                          subVals.data()));
  EXPECT_EQ(subVals[1], 100);
  EXPECT_EQ(subVals[2], 200);

  // 4. Test SerializeToXML
  auto* poVRTDS = dynamic_cast<VRTDataset*>(poDS.get());
  ASSERT_NE(poVRTDS, nullptr);
  auto* psTree = poVRTDS->SerializeToXML(nullptr);
  ASSERT_NE(psTree, nullptr);
  CPLDestroyXMLNode(psTree);
}

// =========================================================================
// MDArray Source From Array Advanced
// =========================================================================

TEST_F(VRTMultidimTest, MDArraySourceFromArrayAdvanced) {
  // Base 2D array
  const std::string base_vrt = "/vsimem/source_adv_base.vrt";
  TrackFile(base_vrt);
  const std::string base_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"Y\" size=\"4\"/>\n"
      "    <Dimension name=\"X\" size=\"4\"/>\n"
      "    <Array name=\"grid\">\n"
      "      <DataType>Int32</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <InlineValues>0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 "
      "15</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  VSILFILE* fp = VSIFOpenL(base_vrt.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(base_xml.c_str(), 1, base_xml.size(), fp);
  VSIFCloseL(fp);

  // VRT with Source referencing the multidim array, with SourceTranspose,
  // SourceView, SourceSlab, DestSlab
  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"dimY\" size=\"3\"/>\n"
      "    <Dimension name=\"dimX\" size=\"3\"/>\n"
      "    <Array name=\"mapped\">\n"
      "      <DataType>Int32</DataType>\n"
      "      <DimensionRef ref=\"dimY\"/>\n"
      "      <DimensionRef ref=\"dimX\"/>\n"
      "      <Source>\n"
      "        <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/source_adv_base.vrt</SourceFilename>\n"
      "        <SourceArray>/grid</SourceArray>\n"
      "        <SourceTranspose>1,0</SourceTranspose>\n"
      "        <SourceView>[0:3, 0:3]</SourceView>\n"
      "        <SourceSlab offset=\"0,0\" count=\"2,2\" step=\"1,1\"/>\n"
      "        <DestSlab offset=\"1,1\"/>\n"
      "      </Source>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(poDS, nullptr);
  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);
  auto poArray = poRoot->OpenMDArray("mapped");
  ASSERT_NE(poArray, nullptr);

  std::vector<GUInt64> start = {0, 0};
  std::vector<size_t> count = {3, 3};
  std::vector<int> data(9, -1);
  EXPECT_TRUE(poArray->Read(start.data(), count.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Int32),
                            data.data()));
  // (1,1) in dest corresponds to (0,0) in source: grid(0,0) = 0
  EXPECT_EQ(data[1 * 3 + 1], 0);

  // Serialize to XML
  auto* poVRTDS = dynamic_cast<VRTDataset*>(poDS.get());
  ASSERT_NE(poVRTDS, nullptr);
  auto* psTree = poVRTDS->SerializeToXML(nullptr);
  ASSERT_NE(psTree, nullptr);
  auto* psSourceNode = CPLGetXMLNode(psTree, "Group.Array.Source");
  ASSERT_NE(psSourceNode, nullptr);
  EXPECT_NE(CPLGetXMLNode(psSourceNode, "SourceTranspose"), nullptr);
  EXPECT_NE(CPLGetXMLNode(psSourceNode, "SourceView"), nullptr);
  EXPECT_NE(CPLGetXMLNode(psSourceNode, "SourceSlab"), nullptr);
  EXPECT_NE(CPLGetXMLNode(psSourceNode, "DestSlab"), nullptr);
  CPLDestroyXMLNode(psTree);
}

// =========================================================================
// Derived Array Additional Steps (GetUnscaled, GetMask, Resample)
// =========================================================================

TEST_F(VRTMultidimTest, DerivedArrayAdditionalSteps) {
  // Base 2D array
  const std::string base_vrt = "/vsimem/derived_more_base.vrt";
  TrackFile(base_vrt);
  const std::string base_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"Y\" size=\"4\"/>\n"
      "    <Dimension name=\"X\" size=\"4\"/>\n"
      "    <Array name=\"grid\">\n"
      "      <DataType>Float32</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <Offset>10</Offset>\n"
      "      <Scale>2</Scale>\n"
      "      <NoDataValue>-999</NoDataValue>\n"
      "      <InlineValues>0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 "
      "15</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  VSILFILE* fp = VSIFOpenL(base_vrt.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(base_xml.c_str(), 1, base_xml.size(), fp);
  VSIFCloseL(fp);

  // 1. GetUnscaled step
  {
    const std::string unscaled_xml =
        "<VRTDataset rasterXSize=\"4\" rasterYSize=\"4\">\n"
        "  <VRTRasterBand dataType=\"Float32\" band=\"1\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename "
        "relativeToVRT=\"0\">/vsimem/derived_more_base.vrt</SourceFilename>\n"
        "          <SourceArray>/grid</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <GetUnscaled/>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";

    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(unscaled_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poDS, nullptr);
    auto* poBand = poDS->GetRasterBand(1);
    ASSERT_NE(poBand, nullptr);
    std::vector<float> buf(16);
    EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 4, 4, buf.data(), 4, 4,
                               GDT_Float32, 0, 0, nullptr),
              CE_None);
    // unscaled: val * scale + offset = 0 * 2 + 10 = 10.0
    EXPECT_FLOAT_EQ(buf[0], 10.0f);
  }

  // 2. GetMask step
  {
    const std::string mask_xml =
        "<VRTDataset rasterXSize=\"4\" rasterYSize=\"4\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename "
        "relativeToVRT=\"0\">/vsimem/derived_more_base.vrt</SourceFilename>\n"
        "          <SourceArray>/grid</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <GetMask/>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";

    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(mask_xml.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poDS, nullptr);
    auto* poBand = poDS->GetRasterBand(1);
    ASSERT_NE(poBand, nullptr);
    std::vector<GByte> buf(16);
    EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 4, 4, buf.data(), 4, 4, GDT_Byte,
                               0, 0, nullptr),
              CE_None);
    EXPECT_EQ(buf[0], 1);
  }

  // 3. Resample step
  {
    const std::string resample_xml =
        "<VRTDataset rasterXSize=\"2\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Float32\" band=\"1\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename "
        "relativeToVRT=\"0\">/vsimem/derived_more_base.vrt</SourceFilename>\n"
        "          <SourceArray>/grid</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <Resample>\n"
        "            <Dimension name=\"Y\" size=\"2\"/>\n"
        "            <Dimension name=\"X\" size=\"2\"/>\n"
        "            <ResampleAlg>NEAR</ResampleAlg>\n"
        "            <Option name=\"SRC_METHOD\">NO_GEOTRANSFORM</Option>\n"
        "          </Resample>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";

    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(resample_xml.c_str(), GDAL_OF_RASTER));
    CPLPopErrorHandler();
    // Exercises VRTDerivedArrayCreate Resample step parsing.
  }
}

// =========================================================================
// MDArray IRead Variations (Strides, Negative Step, Type Conversion)
// =========================================================================

TEST_F(VRTMultidimTest, MDArrayIReadVariations) {
  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"Y\" size=\"3\"/>\n"
      "    <Dimension name=\"X\" size=\"4\"/>\n"
      "    <Array name=\"grid\">\n"
      "      <DataType>Int32</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <InlineValues>0 1 2 3 10 11 12 13 20 21 22 23</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(poDS, nullptr);
  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);
  auto poArray = poRoot->OpenMDArray("grid");
  ASSERT_NE(poArray, nullptr);

  // 1. Read with step > 1 (e.g. step Y=2, step X=2)
  std::vector<GUInt64> start = {0, 0};
  std::vector<size_t> count = {2, 2};
  std::vector<GInt64> step = {2, 2};
  std::vector<GPtrDiff_t> stride = {2, 1};
  std::vector<int> step_vals(4);
  EXPECT_TRUE(
      poArray->Read(start.data(), count.data(), step.data(), stride.data(),
                    GDALExtendedDataType::Create(GDT_Int32), step_vals.data()));
  EXPECT_EQ(step_vals[0], 0);   // (0,0)
  EXPECT_EQ(step_vals[1], 2);   // (0,2)
  EXPECT_EQ(step_vals[2], 20);  // (2,0)
  EXPECT_EQ(step_vals[3], 22);  // (2,2)

  // 2. Read with data type conversion (Int32 to Float64)
  std::vector<double> dbl_vals(4);
  EXPECT_TRUE(poArray->Read(
      start.data(), count.data(), step.data(), stride.data(),
      GDALExtendedDataType::Create(GDT_Float64), dbl_vals.data()));
  EXPECT_DOUBLE_EQ(dbl_vals[0], 0.0);
  EXPECT_DOUBLE_EQ(dbl_vals[3], 22.0);

  // 3. Read with negative step (reversed order: start from row 2, step -1)
  std::vector<GUInt64> rev_start = {2, 0};
  std::vector<size_t> rev_count = {3, 1};
  std::vector<GInt64> rev_step = {-1, 1};
  std::vector<GPtrDiff_t> rev_stride = {1, 1};
  std::vector<int> rev_vals(3);
  EXPECT_TRUE(poArray->Read(
      rev_start.data(), rev_count.data(), rev_step.data(), rev_stride.data(),
      GDALExtendedDataType::Create(GDT_Int32), rev_vals.data()));
  EXPECT_EQ(rev_vals[0], 20);  // row 2
  EXPECT_EQ(rev_vals[1], 10);  // row 1
  EXPECT_EQ(rev_vals[2], 0);   // row 0
}

// =========================================================================
// Additional Error Paths
// =========================================================================

TEST_F(VRTMultidimTest, AdditionalErrorPaths) {
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // Note: the <SingleSourceArray> "array not found" case and the <DerivedArray>
  // <Step> error cases (View without expr, Transpose without newOrder, Grid
  // without GridOptions, unknown step) are covered by
  // SingleSourceArrayAndDerivedArraySteps, which creates its own source file.

  // 1. Source having both SourceArray and SourceBand
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension name=\"dim1\" size=\"2\"/>\n"
        "    <Array name=\"arr\">\n"
        "      <DataType>Byte</DataType>\n"
        "      <DimensionRef ref=\"dim1\"/>\n"
        "      <Source>\n"
        "        <SourceFilename>/vsimem/dummy.tif</SourceFilename>\n"
        "        <SourceArray>/arr</SourceArray>\n"
        "        <SourceBand>1</SourceBand>\n"
        "      </Source>\n"
        "    </Array>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER)),
              nullptr);
  }

  // 2. Source with invalid SourceSlab and DestSlab counts
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension name=\"dim1\" size=\"2\"/>\n"
        "    <Array name=\"arr\">\n"
        "      <DataType>Byte</DataType>\n"
        "      <DimensionRef ref=\"dim1\"/>\n"
        "      <Source>\n"
        "        <SourceFilename>/vsimem/dummy.tif</SourceFilename>\n"
        "        <SourceBand>1</SourceBand>\n"
        "        <SourceSlab offset=\"0,0\" count=\"1\" step=\"1\"/>\n"
        "      </Source>\n"
        "    </Array>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER)),
              nullptr);
  }

  CPLPopErrorHandler();
}

// =========================================================================
// Nested Groups, Relative & Absolute Dimension Full Name Lookup
// =========================================================================

TEST_F(VRTMultidimTest, NestedGroupsAndDimensions) {
  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Group name=\"sub1\">\n"
      "      <Dimension name=\"dim_sub\" size=\"3\"/>\n"
      "      <Array name=\"arr_sub\">\n"
      "        <DataType>Byte</DataType>\n"
      "        <DimensionRef ref=\"dim_sub\"/>\n"
      "        <InlineValues>10 20 30</InlineValues>\n"
      "      </Array>\n"
      "    </Group>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(poDS, nullptr);
  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);

  auto poSubGroup = poRoot->OpenGroup("sub1");
  ASSERT_NE(poSubGroup, nullptr);
  auto poVRTSubGroup = std::dynamic_pointer_cast<VRTGroup>(poSubGroup);
  ASSERT_NE(poVRTSubGroup, nullptr);

  // 1. Valid lookups
  auto poDim = poVRTSubGroup->GetDimensionFromFullName("dim_sub", true);
  ASSERT_NE(poDim, nullptr);
  EXPECT_EQ(poDim->GetName(), "dim_sub");

  auto poDimAbs =
      poVRTSubGroup->GetDimensionFromFullName("/sub1/dim_sub", true);
  ASSERT_NE(poDimAbs, nullptr);
  EXPECT_EQ(poDimAbs->GetName(), "dim_sub");

  // 2. Error lookups with bEmitError = true
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  EXPECT_EQ(poVRTSubGroup->GetDimensionFromFullName("non_existent", true),
            nullptr);
  EXPECT_EQ(
      poVRTSubGroup->GetDimensionFromFullName("/non_existent_group/d", true),
      nullptr);
  EXPECT_EQ(
      poVRTSubGroup->GetDimensionFromFullName("/sub1/non_existent_dim", true),
      nullptr);

  // 3. Indexing variable from another group with full path
  auto poArrSub = poSubGroup->OpenMDArray("arr_sub");
  ASSERT_NE(poArrSub, nullptr);
  EXPECT_TRUE(poDim->SetIndexingVariable(poArrSub));
  EXPECT_NE(poDim->GetIndexingVariable(), nullptr);

  CPLPopErrorHandler();
}

// =========================================================================
// Array element directly inside ArraySource (testing VRTMDArray::Create)
// =========================================================================

TEST_F(VRTMultidimTest, ArrayInsideArraySource) {
  const std::string raster_xml =
      "<VRTDataset rasterXSize=\"2\" rasterYSize=\"2\">\n"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
      "    <ArraySource>\n"
      "      <Array name=\"inline_arr\">\n"
      "        <DataType>Byte</DataType>\n"
      "        <Dimension name=\"Y\" size=\"2\"/>\n"
      "        <Dimension name=\"X\" size=\"2\"/>\n"
      "        <BlockSize>2,2</BlockSize>\n"
      "        <InlineValues>5 6 7 8</InlineValues>\n"
      "      </Array>\n"
      "    </ArraySource>\n"
      "  </VRTRasterBand>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(raster_xml.c_str(), GDAL_OF_RASTER));
  ASSERT_NE(poDS, nullptr);
  auto* poBand = poDS->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);

  std::vector<GByte> buf(4);
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 2, 2, buf.data(), 2, 2, GDT_Byte, 0,
                             0, nullptr),
            CE_None);
  EXPECT_EQ(buf[0], 5);
  EXPECT_EQ(buf[1], 6);
  EXPECT_EQ(buf[2], 7);
  EXPECT_EQ(buf[3], 8);
}

// =========================================================================
// SourceFromArray Read Negative Step, Caching & Relative Path
// =========================================================================

TEST_F(VRTMultidimTest, SourceFromArrayNegativeStepAndCache) {
  const std::string base_vrt = "/vsimem/cache_base.vrt";
  TrackFile(base_vrt);
  const std::string base_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"Y\" size=\"2\"/>\n"
      "    <Dimension name=\"X\" size=\"2\"/>\n"
      "    <Array name=\"grid\">\n"
      "      <DataType>Int32</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <InlineValues>10 20 30 40</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  VSILFILE* fp = VSIFOpenL(base_vrt.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(base_xml.c_str(), 1, base_xml.size(), fp);
  VSIFCloseL(fp);

  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"Y\" size=\"2\"/>\n"
      "    <Dimension name=\"X\" size=\"2\"/>\n"
      "    <Array name=\"mapped\">\n"
      "      <DataType>Int32</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <Source>\n"
      "        <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/cache_base.vrt</SourceFilename>\n"
      "        <SourceArray>/grid</SourceArray>\n"
      "      </Source>\n"
      "    </Array>\n"
      "    <Array name=\"mapped2\">\n"
      "      <DataType>Int32</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <Source>\n"
      "        <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/cache_base.vrt</SourceFilename>\n"
      "        <SourceArray>/grid</SourceArray>\n"
      "      </Source>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(poDS, nullptr);
  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);

  auto poArr1 = poRoot->OpenMDArray("mapped");
  ASSERT_NE(poArr1, nullptr);
  auto poArr2 = poRoot->OpenMDArray("mapped2");
  ASSERT_NE(poArr2, nullptr);

  // Read with negative step: start at (1,1), step (-1, -1), count (2, 2)
  std::vector<GUInt64> start = {1, 1};
  std::vector<size_t> count = {2, 2};
  std::vector<GInt64> step = {-1, -1};
  std::vector<GPtrDiff_t> stride = {2, 1};
  std::vector<int> vals(4);
  EXPECT_TRUE(
      poArr1->Read(start.data(), count.data(), step.data(), stride.data(),
                   GDALExtendedDataType::Create(GDT_Int32), vals.data()));
  EXPECT_EQ(vals[0], 40);  // (1,1)
  EXPECT_EQ(vals[1], 30);  // (1,0)
  EXPECT_EQ(vals[2], 20);  // (0,1)
  EXPECT_EQ(vals[3], 10);  // (0,0)

  // Read second array (hits source dataset cache)
  std::vector<int> vals2(4);
  std::vector<GUInt64> start_fwd = {0, 0};
  std::vector<GInt64> step_fwd = {1, 1};
  EXPECT_TRUE(poArr2->Read(
      start_fwd.data(), count.data(), step_fwd.data(), stride.data(),
      GDALExtendedDataType::Create(GDT_Int32), vals2.data()));
  EXPECT_EQ(vals2[0], 10);
  EXPECT_EQ(vals2[3], 40);
}

// =========================================================================
// Array Fill with NoData (arrays without sources)
// =========================================================================

TEST_F(VRTMultidimTest, VRTMDArrayFillNoData) {
  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"dim2\" size=\"2\"/>\n"
      "    <Array name=\"empty_num\">\n"
      "      <DataType>Float32</DataType>\n"
      "      <DimensionRef ref=\"dim2\"/>\n"
      "      <NoDataValue>-999</NoDataValue>\n"
      "    </Array>\n"
      "    <Array name=\"empty_str\">\n"
      "      <DataType>String</DataType>\n"
      "      <DimensionRef ref=\"dim2\"/>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  auto poDS = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(poDS, nullptr);
  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);

  // 1. Numeric empty array fills with NoData
  auto poNum = poRoot->OpenMDArray("empty_num");
  ASSERT_NE(poNum, nullptr);
  std::vector<GUInt64> start = {0};
  std::vector<size_t> count = {2};
  std::vector<float> fVals(2, 0.0f);
  EXPECT_TRUE(poNum->Read(start.data(), count.data(), nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Float32),
                          fVals.data()));
  EXPECT_FLOAT_EQ(fVals[0], -999.0f);
  EXPECT_FLOAT_EQ(fVals[1], -999.0f);

  // 2. String empty array fills with nullptr/empty strings
  auto poStr = poRoot->OpenMDArray("empty_str");
  ASSERT_NE(poStr, nullptr);
  std::vector<char*> strVals(2, nullptr);
  EXPECT_TRUE(poStr->Read(start.data(), count.data(), nullptr, nullptr,
                          GDALExtendedDataType::CreateString(),
                          strVals.data()));
  EXPECT_EQ(strVals[0], nullptr);
  EXPECT_EQ(strVals[1], nullptr);
}

// =========================================================================
// Programmatic MDArray Creation Options & Errors
// =========================================================================

TEST_F(VRTMultidimTest, CreateMDArrayOptionsAndErrors) {
  const std::string vrt_path = "/vsimem/create_options.vrt";
  TrackFile(vrt_path);

  auto poDS =
      VRTDataset::CreateVRTMultiDimensional(vrt_path.c_str(), nullptr, nullptr);
  ASSERT_NE(poDS, nullptr);
  auto poRoot = poDS->GetRootGroup();
  ASSERT_NE(poRoot, nullptr);

  auto poDimY = poRoot->CreateDimension("Y", "", "", 4);
  auto poDimX = poRoot->CreateDimension("X", "", "", 4);

  // 1. Create with BLOCKSIZE option
  const char* papszBlockOptions[] = {"BLOCKSIZE=2,2", nullptr};
  auto poArr1 = poRoot->CreateMDArray("arr_block", {poDimY, poDimX},
                                      GDALExtendedDataType::Create(GDT_Int32),
                                      papszBlockOptions);
  ASSERT_NE(poArr1, nullptr);
  auto blockSize = poArr1->GetBlockSize();
  ASSERT_EQ(blockSize.size(), 2);
  EXPECT_EQ(blockSize[0], 2);
  EXPECT_EQ(blockSize[1], 2);

  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 2. Duplicate array name
  EXPECT_EQ(
      poRoot->CreateMDArray("arr_block", {poDimY, poDimX},
                            GDALExtendedDataType::Create(GDT_Int32), nullptr),
      nullptr);

  // 3. Invalid BLOCKSIZE count (1 value instead of 2)
  const char* papszBadBlockOptions[] = {"BLOCKSIZE=2", nullptr};
  EXPECT_EQ(poRoot->CreateMDArray("arr_bad_block", {poDimY, poDimX},
                                  GDALExtendedDataType::Create(GDT_Int32),
                                  papszBadBlockOptions),
            nullptr);

  // 4. CreateDimension with empty name or duplicate name
  EXPECT_EQ(poRoot->CreateDimension("", "", "", 10), nullptr);
  EXPECT_EQ(poRoot->CreateDimension("Y", "", "", 4), nullptr);

  // 5. CreateAttribute duplicate on group
  EXPECT_NE(poRoot->CreateAttribute("attr1", {},
                                    GDALExtendedDataType::CreateString()),
            nullptr);
  EXPECT_EQ(poRoot->CreateAttribute("attr1", {},
                                    GDALExtendedDataType::CreateString()),
            nullptr);

  // 6. CreateAttribute duplicate on array
  EXPECT_NE(poArr1->CreateAttribute("arr_attr1", {},
                                    GDALExtendedDataType::CreateString()),
            nullptr);
  EXPECT_EQ(poArr1->CreateAttribute("arr_attr1", {},
                                    GDALExtendedDataType::CreateString()),
            nullptr);

  CPLPopErrorHandler();
}

// =========================================================================
// VRTParseArraySource Direct & DerivedArray Error Steps
// =========================================================================

TEST_F(VRTMultidimTest, VRTParseArraySourceDirectAndDerivedErrors) {
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. VRTParseArraySource with non-ArraySource element name
  {
    CPLXMLNode node;
    memset(&node, 0, sizeof(node));
    node.eType = CXT_Element;
    char szWrongName[] = "InvalidTag";
    node.pszValue = szWrongName;
    VRTMapSharedResources mapShared;
    EXPECT_EQ(VRTParseArraySource(&node, nullptr, mapShared), nullptr);
  }

  // Note: the <Resample> with invalid <SRS> error case is covered by
  // SingleSourceArrayAndDerivedArraySteps.

  // 2. Resample without Dimension elements: the parser fills apoNewDims with
  // null entries (one per source dimension) before calling GetResampled().
  {
    // Source multidim VRT with a 2D array, so that <SingleSourceArray> resolves
    // and the <Step> parsing loop is actually entered.
    const std::string base_vrt = "/vsimem/parse_array_source_base.vrt";
    TrackFile(base_vrt);
    const std::string base_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension name=\"Y\" size=\"4\"/>\n"
        "    <Dimension name=\"X\" size=\"4\"/>\n"
        "    <Array name=\"grid\">\n"
        "      <DataType>Int32</DataType>\n"
        "      <DimensionRef ref=\"Y\"/>\n"
        "      <DimensionRef ref=\"X\"/>\n"
        "      <InlineValues>0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 "
        "15</InlineValues>\n"
        "    </Array>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    VSILFILE* fp = VSIFOpenL(base_vrt.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(base_xml.c_str(), 1, base_xml.size(), fp);
    VSIFCloseL(fp);

    const std::string no_dims =
        "<VRTDataset rasterXSize=\"2\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Int32\" band=\"1\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename>" +
        base_vrt +
        "</SourceFilename>\n"
        "          <SourceArray>/grid</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <Resample>\n"
        "            <Option name=\"SRC_METHOD\">NO_GEOTRANSFORM</Option>\n"
        "          </Resample>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(no_dims.c_str(), GDAL_OF_RASTER));
  }

  // 3. Missing name on Group
  {
    const std::string group_missing_name =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Group/>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(GDALDataset::Open(
                  group_missing_name.c_str(), GDAL_OF_MULTIDIM_RASTER)),
              nullptr);
  }

  CPLPopErrorHandler();
}

// =========================================================================
// Error Branches & Malformed XML
// =========================================================================

TEST_F(VRTMultidimTest, XMLInitErrorBranches) {
  CPLErrorReset();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Group missing name
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group/>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER)),
              nullptr);
  }

  // 2. Dimension missing name or size
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension/>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER)),
              nullptr);
  }

  // 3. Array missing name or DataType
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Array>\n"
        "      <DataType>Int32</DataType>\n"
        "    </Array>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER)),
              nullptr);
  }

  // 4. Attribute missing name
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Attribute type=\"String\"/>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER)),
              nullptr);
  }

  // 5. ArraySource with missing SingleSourceArray
  {
    const std::string vrt_xml =
        "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\">\n"
        "    <ArraySource/>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }

  CPLPopErrorHandler();
}

TEST_F(VRTMultidimTest, SingleSourceArrayAndDerivedArraySteps) {
  const std::string src_md_file = "/vsimem/src_md.vrt";
  TrackFile(src_md_file);
  const std::string src_md_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"Y\" size=\"2\"/>\n"
      "    <Dimension name=\"X\" size=\"3\"/>\n"
      "    <Dimension name=\"Z\" size=\"2\"/>\n"
      "    <Array name=\"var\">\n"
      "      <DataType>Byte</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <ConstantValue>42</ConstantValue>\n"
      "    </Array>\n"
      "    <Array name=\"var_unscaled\">\n"
      "      <DataType>Byte</DataType>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <ConstantValue>10</ConstantValue>\n"
      "      <Offset>10</Offset>\n"
      "      <Scale>2</Scale>\n"
      "    </Array>\n"
      "    <Array name=\"var_3d\">\n"
      "      <DataType>Byte</DataType>\n"
      "      <DimensionRef ref=\"Z\"/>\n"
      "      <DimensionRef ref=\"Y\"/>\n"
      "      <DimensionRef ref=\"X\"/>\n"
      "      <ConstantValue>1</ConstantValue>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      src_md_file.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(src_md_xml.data())),
      src_md_xml.size(), FALSE));

  // 1. ArraySource with SingleSourceArray
  {
    const std::string test_vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <SingleSourceArray>\n"
        "        <SourceFilename "
        "relativeToVRT=\"1\">src_md.vrt</SourceFilename>\n"
        "        <SourceArray>/var</SourceArray>\n"
        "      </SingleSourceArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    const std::string test_vrt_file = "/vsimem/test_array_src.vrt";
    TrackFile(test_vrt_file);
    VSIFCloseL(VSIFileFromMemBuffer(
        test_vrt_file.c_str(),
        reinterpret_cast<GByte*>(const_cast<char*>(test_vrt.data())),
        test_vrt.size(), FALSE));

    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(test_vrt_file.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poDS, nullptr);
    auto poBand = poDS->GetRasterBand(1);
    ASSERT_NE(poBand, nullptr);

    int bSuccess = 0;
    poBand->GetMinimum(&bSuccess);
    poBand->GetMaximum(&bSuccess);

    GUIntBig panHistogram[10] = {0};
    EXPECT_EQ(
        poBand->GetHistogram(0, 255, 10, panHistogram, 0, 1, nullptr, nullptr),
        CE_None);

    std::vector<GByte> buf(6);
    EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 3, 2, buf.data(), 3, 2, GDT_Byte,
                               0, 0, nullptr),
              CE_None);
    EXPECT_EQ(buf[0], 42);

    auto poSourcedBand = dynamic_cast<VRTSourcedRasterBand*>(poBand);
    ASSERT_NE(poSourcedBand, nullptr);

    char** papszVRTXML = poDS->GetMetadata("xml:VRT");
    ASSERT_NE(papszVRTXML, nullptr);
    EXPECT_NE(strstr(papszVRTXML[0], "ArraySource"), nullptr);
  }

  // 2. SingleSourceArray error conditions
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    // Missing SourceFilename
    const std::string bad_vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <SingleSourceArray>\n"
        "        <SourceArray>/var</SourceArray>\n"
        "      </SingleSourceArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(bad_vrt.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  {
    // Missing SourceArray
    const std::string bad_vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <SingleSourceArray>\n"
        "        <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "      </SingleSourceArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(bad_vrt.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  {
    // Array dimension != 2 (var_3d)
    const std::string bad_vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <SingleSourceArray>\n"
        "        <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "        <SourceArray>/var_3d</SourceArray>\n"
        "      </SingleSourceArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(bad_vrt.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  {
    // Array not found
    const std::string bad_vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <SingleSourceArray>\n"
        "        <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "        <SourceArray>/not_found</SourceArray>\n"
        "      </SingleSourceArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(bad_vrt.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  CPLPopErrorHandler();

  // 3. DerivedArray with <GetUnscaled/>
  {
    const std::string vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Float64\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "          <SourceArray>/var_unscaled</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <GetUnscaled/>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(poDS, nullptr);
    auto poBand = poDS->GetRasterBand(1);
    ASSERT_NE(poBand, nullptr);
    std::vector<double> buf(6);
    EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 3, 2, buf.data(), 3, 2,
                               GDT_Float64, 0, 0, nullptr),
              CE_None);
    EXPECT_DOUBLE_EQ(buf[0], 30.0);
  }

  // 4. DerivedArray with <Resample> and <Option>
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    const std::string vrt =
        "<VRTDataset rasterXSize=\"6\" rasterYSize=\"4\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "          <SourceArray>/var</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <Resample>\n"
        "            <Dimension name=\"dimY\" size=\"4\"/>\n"
        "            <Dimension name=\"dimX\" size=\"6\"/>\n"
        "            <ResampleAlg>NEAR</ResampleAlg>\n"
        "            <SRS>EPSG:4326</SRS>\n"
        "            <Option name=\"OVERSAMPLING\">1.0</Option>\n"
        "          </Resample>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt.c_str(), GDAL_OF_RASTER));
    if (poDS) {
      auto poBand = poDS->GetRasterBand(1);
      if (poBand) {
        std::vector<GByte> buf(24);
        CPL_IGNORE_RET_VAL(poBand->RasterIO(GF_Read, 0, 0, 6, 4, buf.data(), 6,
                                            4, GDT_Byte, 0, 0, nullptr));
      }
    }
  }
  CPLPopErrorHandler();

  // 5. DerivedArray errors
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    // Missing expr in View
    const std::string vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "          <SourceArray>/var</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <View/>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  {
    // Missing newOrder in Transpose
    const std::string vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "          <SourceArray>/var</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <Transpose/>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  {
    // Invalid SRS in Resample
    const std::string vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "          <SourceArray>/var</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <Resample>\n"
        "            <SRS>INVALID_SRS_XYZ</SRS>\n"
        "          </Resample>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  {
    // Missing GridOptions in Grid
    const std::string vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "          <SourceArray>/var</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <Grid/>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  {
    // Unknown Step element
    const std::string vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "          <SourceArray>/var</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <UnknownSubElement/>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  {
    // Missing name attribute in Option
    const std::string vrt =
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename>/vsimem/src_md.vrt</SourceFilename>\n"
        "          <SourceArray>/var</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <GetMask>\n"
        "            <Option>no_name</Option>\n"
        "          </GetMask>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(vrt.c_str(), GDAL_OF_RASTER)),
              nullptr);
  }
  CPLPopErrorHandler();
}

TEST_F(VRTMultidimTest, AdditionalCoverageAndEdgeCases) {
  // 1. VRTDimension Set/Get IndexingVariable across groups
  {
    const std::string vrt_file = "/vsimem/dim_test.vrt";
    TrackFile(vrt_file);
    auto poDS =
        std::unique_ptr<VRTDataset>(VRTDataset::CreateVRTMultiDimensional(
            vrt_file.c_str(), nullptr, nullptr));
    ASSERT_NE(poDS, nullptr);
    auto poRoot = poDS->GetRootVRTGroup();
    ASSERT_NE(poRoot, nullptr);
    auto poSub = poRoot->CreateGroup("sub");
    ASSERT_NE(poSub, nullptr);
    auto poDim = poRoot->CreateDimension("dim1", "", "", 2);
    ASSERT_NE(poDim, nullptr);
    auto poVarSub = poSub->CreateMDArray(
        "var_sub", {poDim}, GDALExtendedDataType::Create(GDT_Int32));
    ASSERT_NE(poVarSub, nullptr);

    // Set indexing variable in another group -> full name stored
    EXPECT_TRUE(poDim->SetIndexingVariable(poVarSub));
    auto poGotVar = poDim->GetIndexingVariable();
    ASSERT_NE(poGotVar, nullptr);
    EXPECT_EQ(poGotVar->GetFullName(), poVarSub->GetFullName());

    // Reset indexing variable to null
    EXPECT_TRUE(poDim->SetIndexingVariable(nullptr));
    EXPECT_EQ(poDim->GetIndexingVariable(), nullptr);
  }

  // 2. MDArray Source referencing 2D dataset with SourceBand and
  // SourceTranspose
  {
    const std::string tif_file = "/vsimem/src2d.tif";
    TrackFile(tif_file);
    CreateGeoTiff(tif_file, 4, 4, 1, GDT_Byte, nullptr, nullptr,
                  {std::vector<double>(16, 100.0)});

    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension name=\"Y\" size=\"4\"/>\n"
        "    <Dimension name=\"X\" size=\"4\"/>\n"
        "    <Array name=\"arr\">\n"
        "      <DataType>Byte</DataType>\n"
        "      <DimensionRef ref=\"Y\"/>\n"
        "      <DimensionRef ref=\"X\"/>\n"
        "      <Source>\n"
        "        <SourceFilename>/vsimem/src2d.tif</SourceFilename>\n"
        "        <SourceBand>1</SourceBand>\n"
        "        <SourceTranspose>1,0</SourceTranspose>\n"
        "      </Source>\n"
        "    </Array>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(poDS, nullptr);
    auto poRG = poDS->GetRootGroup();
    ASSERT_NE(poRG, nullptr);
    auto poArr = poRG->OpenMDArray("arr");
    ASSERT_NE(poArr, nullptr);
    std::vector<GByte> vals(16);
    std::vector<GUInt64> start = {0, 0};
    std::vector<size_t> count = {4, 4};
    EXPECT_TRUE(poArr->Read(start.data(), count.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Byte),
                            vals.data()));
    EXPECT_EQ(vals[0], 100);
  }

  // 2b. SourceView with resample=true
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension name=\"Y\" size=\"4\"/>\n"
        "    <Dimension name=\"X\" size=\"4\"/>\n"
        "    <Array name=\"arr_resample\">\n"
        "      <DataType>Byte</DataType>\n"
        "      <DimensionRef ref=\"Y\"/>\n"
        "      <DimensionRef ref=\"X\"/>\n"
        "      <Source>\n"
        "        <SourceFilename>/vsimem/src2d.tif</SourceFilename>\n"
        "        <SourceBand>1</SourceBand>\n"
        "        <SourceView>resample=true</SourceView>\n"
        "      </Source>\n"
        "    </Array>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER));
    if (poDS) {
      auto poRG = poDS->GetRootGroup();
      if (poRG) {
        auto poArr = poRG->OpenMDArray("arr_resample");
        if (poArr) {
          std::vector<GByte> vals(16);
          std::vector<GUInt64> start = {0, 0};
          std::vector<size_t> count = {4, 4};
          poArr->Read(start.data(), count.data(), nullptr, nullptr,
                      GDALExtendedDataType::Create(GDT_Byte), vals.data());
        }
      }
    }
  }
  CPLPopErrorHandler();

  // 3. InlineValues with invalid value count
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    const std::string bad_vrt =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension name=\"dim\" size=\"4\"/>\n"
        "    <Array name=\"arr\">\n"
        "      <DataType>Int32</DataType>\n"
        "      <DimensionRef ref=\"dim\"/>\n"
        "      <InlineValues>1 2</InlineValues>\n"
        "    </Array>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    EXPECT_EQ(std::unique_ptr<GDALDataset>(
                  GDALDataset::Open(bad_vrt.c_str(), GDAL_OF_MULTIDIM_RASTER)),
              nullptr);
  }
  CPLPopErrorHandler();

  // 4. Int32 array reading with non-compact stride and nodata
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension name=\"dim\" size=\"4\"/>\n"
        "    <Array name=\"arr\">\n"
        "      <DataType>Int32</DataType>\n"
        "      <DimensionRef ref=\"dim\"/>\n"
        "      <NoDataValue>99</NoDataValue>\n"
        "      <InlineValues>1 2 3 4</InlineValues>\n"
        "    </Array>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(poDS, nullptr);
    auto poRG = poDS->GetRootGroup();
    ASSERT_NE(poRG, nullptr);
    auto poArr = poRG->OpenMDArray("arr");
    ASSERT_NE(poArr, nullptr);

    std::vector<int> buf(10, 0);
    std::vector<GUInt64> start = {0};
    std::vector<size_t> count = {2};
    std::vector<GInt64> step = {1};
    std::vector<GPtrDiff_t> stride = {2};
    EXPECT_TRUE(
        poArr->Read(start.data(), count.data(), step.data(), stride.data(),
                    GDALExtendedDataType::Create(GDT_Int32), buf.data()));
    EXPECT_EQ(buf[0], 1);
    EXPECT_EQ(buf[2], 2);
  }

  // 5. DerivedArray with <Grid> step: exercises parsing of <GridOptions>,
  // <XArray>, <YArray> and <Option> before GetGridded() is invoked.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    // Source multidim VRT with a 2D array, so that <SingleSourceArray> resolves
    // and the <Step> parsing loop is actually entered.
    const std::string src_md_file = "/vsimem/grid_step_src.vrt";
    TrackFile(src_md_file);
    const std::string src_md_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension name=\"Y\" size=\"2\"/>\n"
        "    <Dimension name=\"X\" size=\"3\"/>\n"
        "    <Array name=\"var\">\n"
        "      <DataType>Byte</DataType>\n"
        "      <DimensionRef ref=\"Y\"/>\n"
        "      <DimensionRef ref=\"X\"/>\n"
        "      <ConstantValue>42</ConstantValue>\n"
        "    </Array>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    VSILFILE* fp = VSIFOpenL(src_md_file.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(src_md_xml.c_str(), 1, src_md_xml.size(), fp);
    VSIFCloseL(fp);

    const std::string vrt =
        "<VRTDataset rasterXSize=\"4\" rasterYSize=\"4\">\n"
        "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\">\n"
        "    <ArraySource>\n"
        "      <DerivedArray>\n"
        "        <SingleSourceArray>\n"
        "          <SourceFilename>" +
        src_md_file +
        "</SourceFilename>\n"
        "          <SourceArray>/var</SourceArray>\n"
        "        </SingleSourceArray>\n"
        "        <Step>\n"
        "          <Grid>\n"
        "            <GridOptions>invdist:power=2.0</GridOptions>\n"
        "            <XArray>\n"
        "              <SingleSourceArray>\n"
        "                <SourceFilename>" +
        src_md_file +
        "</SourceFilename>\n"
        "                <SourceArray>/var</SourceArray>\n"
        "              </SingleSourceArray>\n"
        "            </XArray>\n"
        "            <YArray>\n"
        "              <SingleSourceArray>\n"
        "                <SourceFilename>" +
        src_md_file +
        "</SourceFilename>\n"
        "                <SourceArray>/var</SourceArray>\n"
        "              </SingleSourceArray>\n"
        "            </YArray>\n"
        "            <Option name=\"sample_option\">val</Option>\n"
        "          </Grid>\n"
        "        </Step>\n"
        "      </DerivedArray>\n"
        "    </ArraySource>\n"
        "  </VRTRasterBand>\n"
        "</VRTDataset>\n";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt.c_str(), GDAL_OF_RASTER));
  }
  CPLPopErrorHandler();

  // 6. Attribute with empty list read
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Attribute name=\"empty_attr\">\n"
        "      <DataType>String</DataType>\n"
        "    </Attribute>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(poDS, nullptr);
    auto poRG = poDS->GetRootGroup();
    ASSERT_NE(poRG, nullptr);
    auto poAttr = poRG->GetAttribute("empty_attr");
    ASSERT_NE(poAttr, nullptr);
    const char* pszVal = poAttr->ReadAsString();
    EXPECT_EQ(pszVal, nullptr);
  }

  // 7. MDArray Source with SourceBand and serialize with relativetoVRT="1"
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension name=\"Y\" size=\"4\"/>\n"
        "    <Dimension name=\"X\" size=\"4\"/>\n"
        "    <Array name=\"arr_2d_as_md\">\n"
        "      <DataType>Byte</DataType>\n"
        "      <DimensionRef ref=\"Y\"/>\n"
        "      <DimensionRef ref=\"X\"/>\n"
        "      <Source>\n"
        "        <SourceFilename "
        "relativetoVRT=\"1\">src2d.tif</SourceFilename>\n"
        "        <SourceBand>1</SourceBand>\n"
        "      </Source>\n"
        "    </Array>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    const std::string vrt_path = "/vsimem/vrt_2d_as_md.vrt";
    TrackFile(vrt_path);
    VSIFCloseL(VSIFileFromMemBuffer(
        vrt_path.c_str(),
        reinterpret_cast<GByte*>(const_cast<char*>(vrt_xml.data())),
        vrt_xml.size(), FALSE));
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_path.c_str(), GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(poDS, nullptr);
    auto poRG = poDS->GetRootGroup();
    ASSERT_NE(poRG, nullptr);
    auto poArr = poRG->OpenMDArray("arr_2d_as_md");
    ASSERT_NE(poArr, nullptr);
    std::vector<GByte> vals(16);
    std::vector<GUInt64> start = {0, 0};
    std::vector<size_t> count = {4, 4};
    EXPECT_TRUE(poArr->Read(start.data(), count.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Byte),
                            vals.data()));
    EXPECT_EQ(vals[0], 100);

    // Test serialization of Source with relativetoVRT
    auto poVRTGroup = std::dynamic_pointer_cast<VRTGroup>(poRG);
    if (poVRTGroup) {
      CPLXMLNode* psXML = poVRTGroup->SerializeToXML("/vsimem");
      if (psXML) {
        CPLDestroyXMLNode(psXML);
      }
    }
  }

  // 8. Dimension indexingVariable not found error
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    const std::string vrt_xml =
        "<VRTDataset>\n"
        "  <Group name=\"/\">\n"
        "    <Dimension name=\"dim\" size=\"4\" "
        "indexingVariable=\"non_existent\"/>\n"
        "  </Group>\n"
        "</VRTDataset>\n";
    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(vrt_xml.c_str(), GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(poDS, nullptr);
    auto poRG = poDS->GetRootGroup();
    ASSERT_NE(poRG, nullptr);
    auto poVRTGroup = std::dynamic_pointer_cast<VRTGroup>(poRG);
    ASSERT_NE(poVRTGroup, nullptr);
    auto poDim = poVRTGroup->GetDimension("dim");
    ASSERT_NE(poDim, nullptr);
    EXPECT_EQ(poDim->GetIndexingVariable(), nullptr);
  }
  CPLPopErrorHandler();

  // 9. CreateGroup, CreateMDArray, and CreateAttribute validation errors
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    const std::string vrt_file1 = "/vsimem/val_err1.vrt";
    const std::string vrt_file2 = "/vsimem/val_err2.vrt";
    TrackFile(vrt_file1);
    TrackFile(vrt_file2);
    auto poDS1 =
        std::unique_ptr<VRTDataset>(VRTDataset::CreateVRTMultiDimensional(
            vrt_file1.c_str(), nullptr, nullptr));
    auto poDS2 =
        std::unique_ptr<VRTDataset>(VRTDataset::CreateVRTMultiDimensional(
            vrt_file2.c_str(), nullptr, nullptr));
    ASSERT_NE(poDS1, nullptr);
    ASSERT_NE(poDS2, nullptr);
    auto poRoot1 = poDS1->GetRootVRTGroup();
    auto poRoot2 = poDS2->GetRootVRTGroup();
    ASSERT_NE(poRoot1, nullptr);
    ASSERT_NE(poRoot2, nullptr);

    // Empty group name
    EXPECT_EQ(poRoot1->CreateGroup(""), nullptr);

    // Duplicate group name
    EXPECT_NE(poRoot1->CreateGroup("sub"), nullptr);
    EXPECT_EQ(poRoot1->CreateGroup("sub"), nullptr);

    // Empty array name
    EXPECT_EQ(
        poRoot1->CreateMDArray("", {}, GDALExtendedDataType::Create(GDT_Int32)),
        nullptr);

    // Dimension from another dataset
    auto poDim2 = poRoot2->CreateDimension("dim2", "", "", 10);
    EXPECT_EQ(poRoot1->CreateMDArray("arr_bad_dim", {poDim2},
                                     GDALExtendedDataType::Create(GDT_Int32)),
              nullptr);

    // Dimension size > INT_MAX for attribute
    EXPECT_EQ(poRoot1->CreateAttribute("huge_attr",
                                       {static_cast<GUInt64>(INT_MAX) + 1},
                                       GDALExtendedDataType::Create(GDT_Int32)),
              nullptr);
  }
  CPLPopErrorHandler();
}

}  // namespace
