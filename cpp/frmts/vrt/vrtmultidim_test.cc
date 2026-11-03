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
//
// Tests multidimensional VRT datasets, groups, dimensions, attributes, arrays
// (inline, regularly spaced, source-backed, derived), and VRTArraySource in
// 2D bands (vrtmultidim.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtmultidim.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <climits>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/vrtmultidim_test";

GDALDatasetUniquePtr OpenMultidimVerbose(const char* filename) {
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(filename, GDAL_OF_MULTIDIM_RASTER | GDAL_OF_VERBOSE_ERROR,
                 nullptr, nullptr, nullptr)));
}

GDALDatasetUniquePtr OpenMultidimQuiet(const char* filename) {
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(filename, GDAL_OF_READONLY | GDAL_OF_MULTIDIM_RASTER, nullptr,
                 nullptr, nullptr)));
}

GDALDatasetUniquePtr OpenRasterVerbose(const char* filename) {
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(filename, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR, nullptr,
                 nullptr, nullptr)));
}

GDALDatasetUniquePtr OpenRasterQuiet(const char* filename) {
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(GDALOpenEx(
      filename, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
}

class VrtMultidimTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_VRT();
    GDALRegister_GTiff();
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  void CreateGeoTiff(const char* filename, int xsize, int ysize, int bands,
                     GDALDataType dt, const double* gt, const char* srs_wkt,
                     const std::vector<std::vector<double>>& band_data) {
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, driver);

    GDALDatasetUniquePtr ds(
        driver->Create(filename, xsize, ysize, bands, dt, nullptr));
    ASSERT_NE(nullptr, ds);

    if (gt != nullptr) {
      EXPECT_EQ(CE_None, ds->SetGeoTransform(GDALGeoTransform(gt)));
    }
    if (srs_wkt != nullptr) {
      OGRSpatialReference srs;
      EXPECT_EQ(OGRERR_NONE, srs.importFromWkt(srs_wkt));
      EXPECT_EQ(CE_None, ds->SetSpatialRef(&srs));
    }

    for (int b = 1; b <= bands; ++b) {
      GDALRasterBand* band = ds->GetRasterBand(b);
      ASSERT_NE(nullptr, band);
      if (b - 1 < static_cast<int>(band_data.size()) &&
          !band_data[b - 1].empty()) {
        std::vector<double> data = band_data[b - 1];
        EXPECT_EQ(CE_None,
                  band->RasterIO(GF_Write, 0, 0, xsize, ysize, data.data(),
                                 xsize, ysize, GDT_Float64, 0, 0));
      }
    }
    ds.reset();
  }
};

// =========================================================================
// Programmatic Creation of VRT MultiDimensional Dataset, Groups, Dimensions
// =========================================================================

TEST_F(VrtMultidimTest, ProgrammaticGroupAndDimensionCreation) {
  constexpr char kVrtPath[] = "/vsimem/vrtmultidim_test/prog_multidim.vrt";

  auto ds = VRTDataset::CreateVRTMultiDimensional(kVrtPath, nullptr, nullptr);
  ASSERT_NE(nullptr, ds);

  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  EXPECT_EQ("/", root->GetName());
  EXPECT_EQ("/", root->GetFullName());

  auto root_vrt = ds->GetRootVRTGroup();
  ASSERT_NE(nullptr, root_vrt);
  EXPECT_EQ(root_vrt.get(), root_vrt->GetRootGroup());
  EXPECT_EQ(root_vrt, root_vrt->GetRootGroupSharedPtr());

  // 1. Create SubGroups
  auto subgroup1 = root->CreateGroup("subgroup1");
  ASSERT_NE(nullptr, subgroup1);
  EXPECT_EQ("subgroup1", subgroup1->GetName());
  EXPECT_EQ("/subgroup1", subgroup1->GetFullName());

  auto subgroup2 = root->CreateGroup("subgroup2");
  ASSERT_NE(nullptr, subgroup2);

  EXPECT_EQ(2, root->GetGroupNames().size());

  auto opened_sub = root->OpenGroup("subgroup1");
  ASSERT_NE(nullptr, opened_sub);
  EXPECT_EQ("subgroup1", opened_sub->GetName());

  // 2. Create Dimensions
  auto dim_x = root->CreateDimension("dimX", "TYPE_X", "DIR_EAST", 10);
  ASSERT_NE(nullptr, dim_x);
  EXPECT_EQ("dimX", dim_x->GetName());
  EXPECT_EQ("/dimX", dim_x->GetFullName());
  EXPECT_EQ("TYPE_X", dim_x->GetType());
  EXPECT_EQ("DIR_EAST", dim_x->GetDirection());
  EXPECT_EQ(10, dim_x->GetSize());

  auto dim_y = root->CreateDimension("dimY", "TYPE_Y", "DIR_NORTH", 20);
  ASSERT_NE(nullptr, dim_y);
  EXPECT_EQ(2, root->GetDimensions().size());

  auto dim_found = root_vrt->GetDimensionFromFullName("/dimX", true);
  ASSERT_NE(nullptr, dim_found);
  EXPECT_EQ("dimX", dim_found->GetName());

  // 3. Non-existent group or dimension
  EXPECT_EQ(nullptr, root->OpenGroup("non_existent"));
  EXPECT_EQ(nullptr,
            root_vrt->GetDimensionFromFullName("/non_existent", false));
}

// =========================================================================
// Attributes: String, Numeric, Multi-Value
// =========================================================================

TEST_F(VrtMultidimTest, AttributesCreationAndIO) {
  constexpr char kVrtPath[] = "/vsimem/vrtmultidim_test/attr_test.vrt";

  auto ds = VRTDataset::CreateVRTMultiDimensional(kVrtPath, nullptr, nullptr);
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  // 1. String Attribute
  auto attr_str =
      root->CreateAttribute("author", {}, GDALExtendedDataType::CreateString());
  ASSERT_NE(nullptr, attr_str);
  EXPECT_EQ("author", attr_str->GetName());
  EXPECT_EQ("/_GLOBAL_/author", attr_str->GetFullName());

  EXPECT_TRUE(attr_str->Write("GDAL Team"));
  const char* read_author = attr_str->ReadAsString();
  ASSERT_NE(nullptr, read_author);
  EXPECT_STREQ("GDAL Team", read_author);

  // 2. Numeric Attribute (Int32 scalar)
  auto attr_int = root->CreateAttribute(
      "version", {}, GDALExtendedDataType::Create(GDT_Int32));
  ASSERT_NE(nullptr, attr_int);
  EXPECT_TRUE(attr_int->Write(42.0));
  EXPECT_EQ(42, attr_int->ReadAsInt());

  // 3. Array of numeric values
  const std::vector<GUInt64> dims = {3};
  auto attr_vec = root->CreateAttribute(
      "values", dims, GDALExtendedDataType::Create(GDT_Float64));
  ASSERT_NE(nullptr, attr_vec);
  const std::vector<double> vals = {1.5, 2.5, 3.5};
  EXPECT_TRUE(attr_vec->Write(vals.data(), vals.size()));
  EXPECT_THAT(attr_vec->ReadAsDoubleArray(), ElementsAre(1.5, 2.5, 3.5));

  // 4. GetAttributes list
  EXPECT_EQ(3, root->GetAttributes().size());
  EXPECT_NE(nullptr, root->GetAttribute("author"));
  EXPECT_NE(nullptr, root->GetAttribute("version"));
  EXPECT_NE(nullptr, root->GetAttribute("values"));
  EXPECT_EQ(nullptr, root->GetAttribute("non_existent"));
}

// =========================================================================
// Arrays: InlinedValues & Indexing Variables
// =========================================================================

TEST_F(VrtMultidimTest, ArrayWithInlineValues) {
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="dimX" size="5" indexingVariable="varX"/>
    <Attribute name="test_attr">
      <DataType>String</DataType>
      <Value>attr_val</Value>
    </Attribute>
    <Array name="varX">
      <DataType>Int32</DataType>
      <DimensionRef ref="dimX"/>
      <NoDataValue>-9999</NoDataValue>
      <Unit>metre</Unit>
      <Offset>10</Offset>
      <Scale>2</Scale>
      <InlineValues>10 20 30 40 50</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
  ASSERT_NE(nullptr, ds);

  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto* vrt_ds = dynamic_cast<VRTDataset*>(ds.get());
  ASSERT_NE(nullptr, vrt_ds);
  auto root_vrt = vrt_ds->GetRootVRTGroup();
  ASSERT_NE(nullptr, root_vrt);

  auto dim_x = root_vrt->GetDimension("dimX");
  ASSERT_NE(nullptr, dim_x);
  EXPECT_EQ(5, dim_x->GetSize());

  auto var_x = root->OpenMDArray("varX");
  ASSERT_NE(nullptr, var_x);
  EXPECT_EQ(1, var_x->GetDimensionCount());
  EXPECT_EQ("metre", var_x->GetUnit());
  EXPECT_DOUBLE_EQ(10.0, var_x->GetOffset());
  EXPECT_DOUBLE_EQ(2.0, var_x->GetScale());

  // Check indexing variable
  auto idx_var = dim_x->GetIndexingVariable();
  ASSERT_NE(nullptr, idx_var);
  EXPECT_EQ("varX", idx_var->GetName());

  // Read array values
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {5};
  const std::vector<GInt64> step = {1};
  const std::vector<GPtrDiff_t> stride = {1};
  std::vector<int> data(5);
  EXPECT_TRUE(
      var_x->Read(start.data(), count.data(), step.data(), stride.data(),
                  GDALExtendedDataType::Create(GDT_Int32), data.data()));
  EXPECT_THAT(data, ElementsAre(10, 20, 30, 40, 50));

  // Check nodata
  const void* raw_nodata = var_x->GetRawNoDataValue();
  ASSERT_NE(nullptr, raw_nodata);
  EXPECT_EQ(-9999, *static_cast<const int*>(raw_nodata));
}

// =========================================================================
// Arrays: RegularlySpacedValues
// =========================================================================

TEST_F(VrtMultidimTest, ArrayWithRegularlySpacedValues) {
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="time" size="4"/>
    <Array name="time_var">
      <DataType>Float64</DataType>
      <DimensionRef ref="time"/>
      <RegularlySpacedValues start="100.0" increment="0.5"/>
    </Array>
  </Group>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
  ASSERT_NE(nullptr, ds);

  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  auto array = root->OpenMDArray("time_var");
  ASSERT_NE(nullptr, array);

  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {4};
  const std::vector<GInt64> step = {1};
  const std::vector<GPtrDiff_t> stride = {1};
  std::vector<double> data(4);
  EXPECT_TRUE(
      array->Read(start.data(), count.data(), step.data(), stride.data(),
                  GDALExtendedDataType::Create(GDT_Float64), data.data()));
  EXPECT_THAT(data, ElementsAre(100.0, 100.5, 101.0, 101.5));
}

// =========================================================================
// Arrays: SourceFromArray (Mapping raster dataset into MDArray)
// =========================================================================

TEST_F(VrtMultidimTest, ArraySourceFromArray) {
  constexpr char kTifPath[] = "/vsimem/vrtmultidim_test/source_backing.tif";
  constexpr double kGt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  CreateGeoTiff(kTifPath, 3, 2, 1, GDT_Float64, kGt, nullptr,
                {{1.0, 2.0, 3.0, 4.0, 5.0, 6.0}});

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="2"/>
    <Dimension name="X" size="3"/>
    <Array name="raster_array">
      <DataType>Float64</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <Source>
        <SourceFilename relativeToVRT="0">/vsimem/vrtmultidim_test/source_backing.tif</SourceFilename>
        <SourceBand>1</SourceBand>
      </Source>
    </Array>
  </Group>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
  ASSERT_NE(nullptr, ds);

  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  auto array = root->OpenMDArray("raster_array");
  ASSERT_NE(nullptr, array);
  EXPECT_EQ(2, array->GetDimensionCount());

  const std::vector<GUInt64> start = {0, 0};
  const std::vector<size_t> count = {2, 3};
  const std::vector<GInt64> step = {1, 1};
  const std::vector<GPtrDiff_t> stride = {3, 1};
  std::vector<double> data(6);
  EXPECT_TRUE(
      array->Read(start.data(), count.data(), step.data(), stride.data(),
                  GDALExtendedDataType::Create(GDT_Float64), data.data()));
  EXPECT_THAT(data, ElementsAre(1.0, 2.0, 3.0, 4.0, 5.0, 6.0));
}

// =========================================================================
// 2D VRT RasterBand with VRTArraySource
// =========================================================================

TEST_F(VrtMultidimTest, VRTArraySourceIn2DRasterBand) {
  constexpr std::string_view kMdXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="3"/>
    <Dimension name="X" size="4"/>
    <Array name="grid">
      <DataType>Byte</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>10 11 12 13 14 15 16 17 18 19 20 21</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";
  VsiMemTempWrapper md_vrt("/vsimem/vrtmultidim_test/md_for_raster.vrt",
                           std::string(kMdXml));

  constexpr char kRasterXml[] = R"vrt(
<VRTDataset rasterXSize="4" rasterYSize="3">
  <VRTRasterBand dataType="Byte" band="1">
    <ArraySource>
      <SingleSourceArray>
        <SourceFilename relativeToVRT="0">/vsimem/vrtmultidim_test/md_for_raster.vrt</SourceFilename>
        <SourceArray>/grid</SourceArray>
      </SingleSourceArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr ds_2d = OpenRasterVerbose(kRasterXml);
  ASSERT_NE(nullptr, ds_2d);
  EXPECT_EQ(4, ds_2d->GetRasterXSize());
  EXPECT_EQ(3, ds_2d->GetRasterYSize());
  EXPECT_EQ(1, ds_2d->GetRasterCount());

  GDALRasterBand* band = ds_2d->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<GByte> buf(12);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 4, 3, buf.data(), 4, 3,
                                    GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(10, buf[0]);
  EXPECT_EQ(11, buf[1]);
  EXPECT_EQ(21, buf[11]);

  int success = TRUE;
  EXPECT_DOUBLE_EQ(0.0, band->GetMinimum(&success));
  EXPECT_EQ(FALSE, success);

  success = TRUE;
  EXPECT_DOUBLE_EQ(255.0, band->GetMaximum(&success));
  EXPECT_EQ(FALSE, success);

  double min_max[2] = {};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(FALSE, min_max));
  EXPECT_DOUBLE_EQ(10.0, min_max[0]);
  EXPECT_DOUBLE_EQ(21.0, min_max[1]);

  auto* vrt_ds = dynamic_cast<VRTDataset*>(ds_2d.get());
  ASSERT_NE(nullptr, vrt_ds);
  CPLXMLTreeCloser tree(
      vrt_ds->SerializeToXML("/vsimem/vrtmultidim_test/serialized_2d.vrt"));
  ASSERT_NE(nullptr, tree.get());
  EXPECT_NE(nullptr, CPLGetXMLNode(tree.get(), "VRTRasterBand.ArraySource"));
}

// =========================================================================
// Derived Array Steps: View, Transpose
// =========================================================================

TEST_F(VrtMultidimTest, DerivedArraySteps) {
  constexpr std::string_view kBaseXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="4"/>
    <Dimension name="X" size="4"/>
    <Array name="base_arr">
      <DataType>Int32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";
  VsiMemTempWrapper base_vrt("/vsimem/vrtmultidim_test/base_derived.vrt",
                             std::string(kBaseXml));

  // 1. 2D VRT with DerivedArray View step
  {
    constexpr char kViewXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2">
  <VRTRasterBand dataType="Int32" band="1">
    <ArraySource>
      <DerivedArray>
        <SingleSourceArray>
          <SourceFilename relativeToVRT="0">/vsimem/vrtmultidim_test/base_derived.vrt</SourceFilename>
          <SourceArray>/base_arr</SourceArray>
        </SingleSourceArray>
        <Step>
          <View expr="[0:2, 0:2]"/>
        </Step>
      </DerivedArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";

    GDALDatasetUniquePtr ds_view = OpenRasterVerbose(kViewXml);
    ASSERT_NE(nullptr, ds_view);
    GDALRasterBand* band_view = ds_view->GetRasterBand(1);
    ASSERT_NE(nullptr, band_view);

    std::vector<int> view_data(4);
    EXPECT_EQ(CE_None,
              band_view->RasterIO(GF_Read, 0, 0, 2, 2, view_data.data(), 2, 2,
                                  GDT_Int32, 0, 0, nullptr));
    EXPECT_THAT(view_data, ElementsAre(0, 1, 4, 5));
  }

  // 2. 2D VRT with DerivedArray Transpose step
  {
    constexpr char kTransXml[] = R"vrt(
<VRTDataset rasterXSize="4" rasterYSize="4">
  <VRTRasterBand dataType="Int32" band="1">
    <ArraySource>
      <DerivedArray>
        <SingleSourceArray>
          <SourceFilename relativeToVRT="0">/vsimem/vrtmultidim_test/base_derived.vrt</SourceFilename>
          <SourceArray>/base_arr</SourceArray>
        </SingleSourceArray>
        <Step>
          <Transpose newOrder="1,0"/>
        </Step>
      </DerivedArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";

    GDALDatasetUniquePtr ds_trans = OpenRasterVerbose(kTransXml);
    ASSERT_NE(nullptr, ds_trans);
    GDALRasterBand* band_trans = ds_trans->GetRasterBand(1);
    ASSERT_NE(nullptr, band_trans);

    std::vector<int> trans_data(16);
    EXPECT_EQ(CE_None,
              band_trans->RasterIO(GF_Read, 0, 0, 4, 4, trans_data.data(), 4, 4,
                                   GDT_Int32, 0, 0, nullptr));
    EXPECT_EQ(4, trans_data[1]);
  }
}

// =========================================================================
// Serialization, Deserialization & VRT CopyFrom
// =========================================================================

TEST_F(VrtMultidimTest, SerializationAndReopen) {
  constexpr char kVrtPath[] = "/vsimem/vrtmultidim_test/ser_multidim.vrt";

  {
    auto ds = VRTDataset::CreateVRTMultiDimensional(kVrtPath, nullptr, nullptr);
    ASSERT_NE(nullptr, ds);
    auto root = ds->GetRootGroup();
    ASSERT_NE(nullptr, root);

    auto dim = root->CreateDimension("dim1", "", "", 10);
    ASSERT_NE(nullptr, dim);

    auto attr =
        root->CreateAttribute("desc", {}, GDALExtendedDataType::CreateString());
    ASSERT_NE(nullptr, attr);
    EXPECT_TRUE(attr->Write("multidimensional test"));

    auto array = root->CreateMDArray("arr1", {dim},
                                     GDALExtendedDataType::Create(GDT_Float32));
    ASSERT_NE(nullptr, array);
    EXPECT_TRUE(array->SetUnit("km"));
    EXPECT_TRUE(array->SetOffset(5.0));
    EXPECT_TRUE(array->SetScale(0.1));
    double nodata = -999.0;
    EXPECT_TRUE(array->SetRawNoDataValue(&nodata));

    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS("WGS84"));
    EXPECT_TRUE(array->SetSpatialRef(&srs));

    EXPECT_EQ(CE_None, ds->Close());
  }

  GDALDatasetUniquePtr reopened = OpenMultidimVerbose(kVrtPath);
  ASSERT_NE(nullptr, reopened);
  auto root = reopened->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto* vrt_ds = dynamic_cast<VRTDataset*>(reopened.get());
  ASSERT_NE(nullptr, vrt_ds);
  auto root_vrt = vrt_ds->GetRootVRTGroup();
  ASSERT_NE(nullptr, root_vrt);

  auto dim = root_vrt->GetDimension("dim1");
  ASSERT_NE(nullptr, dim);
  EXPECT_EQ(10, dim->GetSize());

  auto attr = root->GetAttribute("desc");
  ASSERT_NE(nullptr, attr);
  EXPECT_STREQ("multidimensional test", attr->ReadAsString());

  auto array = root->OpenMDArray("arr1");
  ASSERT_NE(nullptr, array);
  EXPECT_EQ("km", array->GetUnit());
  EXPECT_DOUBLE_EQ(5.0, array->GetOffset());
  EXPECT_DOUBLE_EQ(0.1, array->GetScale());
  ASSERT_NE(nullptr, array->GetSpatialRef());
  OGRSpatialReference expected_srs;
  ASSERT_EQ(OGRERR_NONE, expected_srs.SetWellKnownGeogCS("WGS84"));
  EXPECT_THAT(*array->GetSpatialRef(), IsSameAs(expected_srs));
}

// =========================================================================
// MDArray CopyFrom & Dimension SetIndexingVariable
// =========================================================================

TEST_F(VrtMultidimTest, MDArrayCopyFromAndIndexingVariable) {
  constexpr std::string_view kSrcXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="dim1" size="4"/>
    <Dimension name="Y" size="2"/>
    <Dimension name="X" size="3"/>
    <Array name="reg1d">
      <DataType>Float64</DataType>
      <DimensionRef ref="dim1"/>
      <RegularlySpacedValues start="10.0" increment="2.5"/>
    </Array>
    <Array name="arr2d">
      <DataType>Int32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>1 2 3 4 5 6</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";
  constexpr char kSrcVrtPath[] = "/vsimem/vrtmultidim_test/copy_src.vrt";
  VsiMemTempWrapper src_vrt(kSrcVrtPath, std::string(kSrcXml));

  GDALDatasetUniquePtr src_ds = OpenMultidimVerbose(kSrcVrtPath);
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);
  auto src_1d = src_root->OpenMDArray("reg1d");
  ASSERT_NE(nullptr, src_1d);
  auto src_2d = src_root->OpenMDArray("arr2d");
  ASSERT_NE(nullptr, src_2d);

  constexpr char kDstVrtPath[] = "/vsimem/vrtmultidim_test/copy_dst.vrt";
  {
    auto dst_ds =
        VRTDataset::CreateVRTMultiDimensional(kDstVrtPath, nullptr, nullptr);
    ASSERT_NE(nullptr, dst_ds);
    auto dst_root = dst_ds->GetRootGroup();
    ASSERT_NE(nullptr, dst_root);
    auto dst_root_vrt = dst_ds->GetRootVRTGroup();
    ASSERT_NE(nullptr, dst_root_vrt);

    auto dim1 = dst_root->CreateDimension("dim1", "", "", 4);
    auto dim_y = dst_root->CreateDimension("Y", "", "", 2);
    auto dim_x = dst_root->CreateDimension("X", "", "", 3);

    auto dst_1d = dst_root->CreateMDArray(
        "copied1d", {dim1}, GDALExtendedDataType::Create(GDT_Float64));
    ASSERT_NE(nullptr, dst_1d);
    GUInt64 cur_cost = 0;
    EXPECT_TRUE(dst_1d->CopyFrom(src_ds.get(), src_1d.get(), false, cur_cost,
                                 1000, nullptr, nullptr));

    auto dst_2d = dst_root->CreateMDArray(
        "copied2d", {dim_y, dim_x}, GDALExtendedDataType::Create(GDT_Int32));
    ASSERT_NE(nullptr, dst_2d);
    EXPECT_TRUE(dst_2d->CopyFrom(src_ds.get(), src_2d.get(), false, cur_cost,
                                 1000, nullptr, nullptr));

    auto attr = dst_2d->CreateAttribute("desc", {},
                                        GDALExtendedDataType::CreateString());
    ASSERT_NE(nullptr, attr);
    EXPECT_TRUE(attr->Write("copied array"));

    auto dim1_vrt = dst_root_vrt->GetDimension("dim1");
    ASSERT_NE(nullptr, dim1_vrt);
    EXPECT_TRUE(dim1_vrt->SetIndexingVariable(dst_1d));
    EXPECT_NE(nullptr, dim1_vrt->GetIndexingVariable());

    EXPECT_TRUE(dim1_vrt->SetIndexingVariable(nullptr));
    EXPECT_EQ(nullptr, dim1_vrt->GetIndexingVariable());
    EXPECT_TRUE(dim1_vrt->SetIndexingVariable(dst_1d));

    EXPECT_EQ(CE_None, dst_ds->Close());
  }

  GDALDatasetUniquePtr reopened = OpenMultidimVerbose(kDstVrtPath);
  ASSERT_NE(nullptr, reopened);
  auto root = reopened->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto arr_1d = root->OpenMDArray("copied1d");
  ASSERT_NE(nullptr, arr_1d);
  const std::vector<GUInt64> start1 = {0};
  const std::vector<size_t> count1 = {4};
  std::vector<double> vals1(4);
  EXPECT_TRUE(arr_1d->Read(start1.data(), count1.data(), nullptr, nullptr,
                           GDALExtendedDataType::Create(GDT_Float64),
                           vals1.data()));
  EXPECT_DOUBLE_EQ(10.0, vals1[0]);
  EXPECT_DOUBLE_EQ(12.5, vals1[1]);
  EXPECT_DOUBLE_EQ(17.5, vals1[3]);

  auto arr_2d = root->OpenMDArray("copied2d");
  ASSERT_NE(nullptr, arr_2d);
  const std::vector<GUInt64> start2 = {0, 0};
  const std::vector<size_t> count2 = {2, 3};
  std::vector<int> vals2(6);
  EXPECT_TRUE(arr_2d->Read(start2.data(), count2.data(), nullptr, nullptr,
                           GDALExtendedDataType::Create(GDT_Int32),
                           vals2.data()));
  EXPECT_EQ(1, vals2[0]);
  EXPECT_EQ(6, vals2[5]);

  auto attr_read = arr_2d->GetAttribute("desc");
  ASSERT_NE(nullptr, attr_read);
  EXPECT_STREQ("copied array", attr_read->ReadAsString());
}

// =========================================================================
// InlinedValues Types & Serialization
// =========================================================================

TEST_F(VrtMultidimTest, InlinedValuesTypesAndSerialization) {
  constexpr std::string_view kVrtXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="dim4" size="4"/>
    <Dimension name="dim2" size="2"/>
    <Array name="const_arr">
      <DataType>Float32</DataType>
      <DimensionRef ref="dim4"/>
      <ConstantValue>42.5</ConstantValue>
    </Array>
    <Array name="str_arr">
      <DataType>String</DataType>
      <DimensionRef ref="dim2"/>
      <InlineValuesWithValueElement>
        <Value>hello</Value>
        <NullValue/>
      </InlineValuesWithValueElement>
    </Array>
    <Array name="sub_arr">
      <DataType>Int32</DataType>
      <DimensionRef ref="dim4"/>
      <InlineValues offset="1" count="2">100 200</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";

  constexpr char kVrtPath[] = "/vsimem/vrtmultidim_test/inline_types.vrt";
  VsiMemTempWrapper vrt_file(kVrtPath, std::string(kVrtXml));

  GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
      GDALOpenEx(kVrtPath, GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  // 1. ConstantValue
  auto const_arr = root->OpenMDArray("const_arr");
  ASSERT_NE(nullptr, const_arr);
  const std::vector<GUInt64> start4 = {0};
  const std::vector<size_t> count4 = {4};
  std::vector<float> f_vals(4);
  EXPECT_TRUE(const_arr->Read(start4.data(), count4.data(), nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_Float32),
                              f_vals.data()));
  EXPECT_FLOAT_EQ(42.5f, f_vals[0]);
  EXPECT_FLOAT_EQ(42.5f, f_vals[3]);

  // 2. String Array
  auto str_arr = root->OpenMDArray("str_arr");
  ASSERT_NE(nullptr, str_arr);
  const std::vector<GUInt64> start2 = {0};
  const std::vector<size_t> count2 = {2};
  std::vector<char*> str_vals(2, nullptr);
  EXPECT_TRUE(str_arr->Read(start2.data(), count2.data(), nullptr, nullptr,
                            GDALExtendedDataType::CreateString(),
                            str_vals.data()));
  std::unique_ptr<char, CplFreeDeleter> owned_str(str_vals[0]);
  std::unique_ptr<char, CplFreeDeleter> owned_null_str(str_vals[1]);
  ASSERT_NE(nullptr, owned_str);
  EXPECT_STREQ("hello", owned_str.get());
  EXPECT_EQ(nullptr, owned_null_str);

  // 3. Sub-window array
  auto sub_arr = root->OpenMDArray("sub_arr");
  ASSERT_NE(nullptr, sub_arr);
  std::vector<int> sub_vals(4, -1);
  EXPECT_TRUE(sub_arr->Read(start4.data(), count4.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Int32),
                            sub_vals.data()));
  EXPECT_EQ(100, sub_vals[1]);
  EXPECT_EQ(200, sub_vals[2]);

  // 4. Test SerializeToXML
  auto* vrt_ds = dynamic_cast<VRTDataset*>(ds.get());
  ASSERT_NE(nullptr, vrt_ds);
  CPLXMLTreeCloser tree(vrt_ds->SerializeToXML(nullptr));
  ASSERT_NE(nullptr, tree.get());
}

// =========================================================================
// MDArray Source From Array Advanced
// =========================================================================

TEST_F(VrtMultidimTest, MDArraySourceFromArrayAdvanced) {
  constexpr std::string_view kBaseXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="4"/>
    <Dimension name="X" size="4"/>
    <Array name="grid">
      <DataType>Int32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";
  VsiMemTempWrapper base_vrt("/vsimem/vrtmultidim_test/source_adv_base.vrt",
                             std::string(kBaseXml));

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="dimY" size="3"/>
    <Dimension name="dimX" size="3"/>
    <Array name="mapped">
      <DataType>Int32</DataType>
      <DimensionRef ref="dimY"/>
      <DimensionRef ref="dimX"/>
      <Source>
        <SourceFilename relativeToVRT="0">/vsimem/vrtmultidim_test/source_adv_base.vrt</SourceFilename>
        <SourceArray>/grid</SourceArray>
        <SourceTranspose>1,0</SourceTranspose>
        <SourceView>[0:3, 0:3]</SourceView>
        <SourceSlab offset="0,0" count="2,2" step="1,1"/>
        <DestSlab offset="1,1"/>
      </Source>
    </Array>
  </Group>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  auto array = root->OpenMDArray("mapped");
  ASSERT_NE(nullptr, array);

  const std::vector<GUInt64> start = {0, 0};
  const std::vector<size_t> count = {3, 3};
  std::vector<int> data(9, -1);
  EXPECT_TRUE(array->Read(start.data(), count.data(), nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Int32),
                          data.data()));
  EXPECT_EQ(0, data[1 * 3 + 1]);

  auto* vrt_ds = dynamic_cast<VRTDataset*>(ds.get());
  ASSERT_NE(nullptr, vrt_ds);
  CPLXMLTreeCloser tree(vrt_ds->SerializeToXML(nullptr));
  ASSERT_NE(nullptr, tree.get());
  CPLXMLNode* src_node = CPLGetXMLNode(tree.get(), "Group.Array.Source");
  ASSERT_NE(nullptr, src_node);
  EXPECT_NE(nullptr, CPLGetXMLNode(src_node, "SourceTranspose"));
  EXPECT_NE(nullptr, CPLGetXMLNode(src_node, "SourceView"));
  EXPECT_NE(nullptr, CPLGetXMLNode(src_node, "SourceSlab"));
  EXPECT_NE(nullptr, CPLGetXMLNode(src_node, "DestSlab"));
}

// =========================================================================
// Derived Array Additional Steps (GetUnscaled, GetMask, Resample)
// =========================================================================

TEST_F(VrtMultidimTest, DerivedArrayAdditionalSteps) {
  constexpr std::string_view kBaseXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="4"/>
    <Dimension name="X" size="4"/>
    <Array name="grid">
      <DataType>Float32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <Offset>10</Offset>
      <Scale>2</Scale>
      <NoDataValue>-999</NoDataValue>
      <InlineValues>0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";
  VsiMemTempWrapper base_vrt("/vsimem/vrtmultidim_test/derived_more_base.vrt",
                             std::string(kBaseXml));

  // 1. GetUnscaled step
  {
    constexpr char kUnscaledXml[] = R"vrt(
<VRTDataset rasterXSize="4" rasterYSize="4">
  <VRTRasterBand dataType="Float32" band="1">
    <ArraySource>
      <DerivedArray>
        <SingleSourceArray>
          <SourceFilename relativeToVRT="0">/vsimem/vrtmultidim_test/derived_more_base.vrt</SourceFilename>
          <SourceArray>/grid</SourceArray>
        </SingleSourceArray>
        <Step>
          <GetUnscaled/>
        </Step>
      </DerivedArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";

    GDALDatasetUniquePtr ds = OpenRasterVerbose(kUnscaledXml);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    std::vector<float> buf(16);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 4, 4, buf.data(), 4, 4,
                                      GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(10.0f, buf[0]);
  }

  // 2. GetMask step
  {
    constexpr char kMaskXml[] = R"vrt(
<VRTDataset rasterXSize="4" rasterYSize="4">
  <VRTRasterBand dataType="Byte" band="1">
    <ArraySource>
      <DerivedArray>
        <SingleSourceArray>
          <SourceFilename relativeToVRT="0">/vsimem/vrtmultidim_test/derived_more_base.vrt</SourceFilename>
          <SourceArray>/grid</SourceArray>
        </SingleSourceArray>
        <Step>
          <GetMask/>
        </Step>
      </DerivedArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";

    GDALDatasetUniquePtr ds = OpenRasterVerbose(kMaskXml);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    std::vector<GByte> buf(16);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 4, 4, buf.data(), 4, 4,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(1, buf[0]);
  }

  // 3. Resample step parsing
  {
    constexpr char kResampleXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2">
  <VRTRasterBand dataType="Float32" band="1">
    <ArraySource>
      <DerivedArray>
        <SingleSourceArray>
          <SourceFilename relativeToVRT="0">/vsimem/vrtmultidim_test/derived_more_base.vrt</SourceFilename>
          <SourceArray>/grid</SourceArray>
        </SingleSourceArray>
        <Step>
          <Resample>
            <Dimension name="Y" size="2"/>
            <Dimension name="X" size="2"/>
            <ResampleAlg>NEAR</ResampleAlg>
            <Option name="SRC_METHOD">NO_GEOTRANSFORM</Option>
          </Resample>
        </Step>
      </DerivedArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";

    WithQuietHandler quiet;
    CPLErrorReset();
    CPL_IGNORE_RET_VAL(OpenRasterQuiet(kResampleXml));
    CPLErrorReset();
  }
}

// =========================================================================
// MDArray IRead Variations (Strides, Negative Step, Type Conversion)
// =========================================================================

TEST_F(VrtMultidimTest, MDArrayIReadVariations) {
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="3"/>
    <Dimension name="X" size="4"/>
    <Array name="grid">
      <DataType>Int32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>0 1 2 3 10 11 12 13 20 21 22 23</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  auto array = root->OpenMDArray("grid");
  ASSERT_NE(nullptr, array);

  // 1. Read with step > 1
  const std::vector<GUInt64> start = {0, 0};
  const std::vector<size_t> count = {2, 2};
  const std::vector<GInt64> step = {2, 2};
  const std::vector<GPtrDiff_t> stride = {2, 1};
  std::vector<int> step_vals(4);
  EXPECT_TRUE(
      array->Read(start.data(), count.data(), step.data(), stride.data(),
                  GDALExtendedDataType::Create(GDT_Int32), step_vals.data()));
  EXPECT_THAT(step_vals, ElementsAre(0, 2, 20, 22));

  // 2. Read with data type conversion (Int32 to Float64)
  std::vector<double> dbl_vals(4);
  EXPECT_TRUE(
      array->Read(start.data(), count.data(), step.data(), stride.data(),
                  GDALExtendedDataType::Create(GDT_Float64), dbl_vals.data()));
  EXPECT_DOUBLE_EQ(0.0, dbl_vals[0]);
  EXPECT_DOUBLE_EQ(22.0, dbl_vals[3]);

  // 3. Read with negative step
  const std::vector<GUInt64> rev_start = {2, 0};
  const std::vector<size_t> rev_count = {3, 1};
  const std::vector<GInt64> rev_step = {-1, 1};
  const std::vector<GPtrDiff_t> rev_stride = {1, 1};
  std::vector<int> rev_vals(3);
  EXPECT_TRUE(array->Read(
      rev_start.data(), rev_count.data(), rev_step.data(), rev_stride.data(),
      GDALExtendedDataType::Create(GDT_Int32), rev_vals.data()));
  EXPECT_THAT(rev_vals, ElementsAre(20, 10, 0));
}

// =========================================================================
// Additional Error Paths
// =========================================================================

TEST_F(VrtMultidimTest, AdditionalErrorPaths) {
  WithQuietHandler quiet;

  // 1. Source having both SourceArray and SourceBand
  constexpr char kBothArrayAndBand[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="dim1" size="2"/>
    <Array name="arr">
      <DataType>Byte</DataType>
      <DimensionRef ref="dim1"/>
      <Source>
        <SourceFilename>/vsimem/vrtmultidim_test/dummy.tif</SourceFilename>
        <SourceArray>/arr</SourceArray>
        <SourceBand>1</SourceBand>
      </Source>
    </Array>
  </Group>
</VRTDataset>)vrt";
  CPLErrorReset();
  EXPECT_EQ(nullptr, OpenMultidimQuiet(kBothArrayAndBand));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // 2. Source with invalid SourceSlab and DestSlab counts
  constexpr char kBadSlab[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="dim1" size="2"/>
    <Array name="arr">
      <DataType>Byte</DataType>
      <DimensionRef ref="dim1"/>
      <Source>
        <SourceFilename>/vsimem/vrtmultidim_test/dummy.tif</SourceFilename>
        <SourceBand>1</SourceBand>
        <SourceSlab offset="0,0" count="1" step="1"/>
      </Source>
    </Array>
  </Group>
</VRTDataset>)vrt";
  CPLErrorReset();
  EXPECT_EQ(nullptr, OpenMultidimQuiet(kBadSlab));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// =========================================================================
// Nested Groups, Relative & Absolute Dimension Full Name Lookup
// =========================================================================

TEST_F(VrtMultidimTest, NestedGroupsAndDimensions) {
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Group name="sub1">
      <Dimension name="dim_sub" size="3"/>
      <Array name="arr_sub">
        <DataType>Byte</DataType>
        <DimensionRef ref="dim_sub"/>
        <InlineValues>10 20 30</InlineValues>
      </Array>
    </Group>
  </Group>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto subgroup = root->OpenGroup("sub1");
  ASSERT_NE(nullptr, subgroup);
  auto vrt_subgroup = std::dynamic_pointer_cast<VRTGroup>(subgroup);
  ASSERT_NE(nullptr, vrt_subgroup);

  // 1. Valid lookups
  auto dim = vrt_subgroup->GetDimensionFromFullName("dim_sub", true);
  ASSERT_NE(nullptr, dim);
  EXPECT_EQ("dim_sub", dim->GetName());

  auto dim_abs = vrt_subgroup->GetDimensionFromFullName("/sub1/dim_sub", true);
  ASSERT_NE(nullptr, dim_abs);
  EXPECT_EQ("dim_sub", dim_abs->GetName());

  // 2. Error lookups with bEmitError = true
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              vrt_subgroup->GetDimensionFromFullName("non_existent", true));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(nullptr, vrt_subgroup->GetDimensionFromFullName(
                           "/non_existent_group/d", true));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(nullptr, vrt_subgroup->GetDimensionFromFullName(
                           "/sub1/non_existent_dim", true));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. Indexing variable from another group with full path
  auto arr_sub = subgroup->OpenMDArray("arr_sub");
  ASSERT_NE(nullptr, arr_sub);
  EXPECT_TRUE(dim->SetIndexingVariable(arr_sub));
  EXPECT_NE(nullptr, dim->GetIndexingVariable());
}

// =========================================================================
// Array element directly inside ArraySource (testing VRTMDArray::Create)
// =========================================================================

TEST_F(VrtMultidimTest, ArrayInsideArraySource) {
  constexpr char kRasterXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2">
  <VRTRasterBand dataType="Byte" band="1">
    <ArraySource>
      <Array name="inline_arr">
        <DataType>Byte</DataType>
        <Dimension name="Y" size="2"/>
        <Dimension name="X" size="2"/>
        <BlockSize>2,2</BlockSize>
        <InlineValues>5 6 7 8</InlineValues>
      </Array>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr ds = OpenRasterVerbose(kRasterXml);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  std::vector<GByte> buf(4);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 2, 2, buf.data(), 2, 2,
                                    GDT_Byte, 0, 0, nullptr));
  EXPECT_THAT(buf, ElementsAre(5, 6, 7, 8));
}

// =========================================================================
// SourceFromArray Read Negative Step, Caching & Relative Path
// =========================================================================

TEST_F(VrtMultidimTest, SourceFromArrayNegativeStepAndCache) {
  constexpr std::string_view kBaseXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="2"/>
    <Dimension name="X" size="2"/>
    <Array name="grid">
      <DataType>Int32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>10 20 30 40</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";
  VsiMemTempWrapper base_vrt("/vsimem/vrtmultidim_test/cache_base.vrt",
                             std::string(kBaseXml));

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="2"/>
    <Dimension name="X" size="2"/>
    <Array name="mapped">
      <DataType>Int32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <Source>
        <SourceFilename relativeToVRT="0">/vsimem/vrtmultidim_test/cache_base.vrt</SourceFilename>
        <SourceArray>/grid</SourceArray>
      </Source>
    </Array>
    <Array name="mapped2">
      <DataType>Int32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <Source>
        <SourceFilename relativeToVRT="0">/vsimem/vrtmultidim_test/cache_base.vrt</SourceFilename>
        <SourceArray>/grid</SourceArray>
      </Source>
    </Array>
  </Group>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto arr1 = root->OpenMDArray("mapped");
  ASSERT_NE(nullptr, arr1);
  auto arr2 = root->OpenMDArray("mapped2");
  ASSERT_NE(nullptr, arr2);

  // Read with negative step
  const std::vector<GUInt64> start = {1, 1};
  const std::vector<size_t> count = {2, 2};
  const std::vector<GInt64> step = {-1, -1};
  const std::vector<GPtrDiff_t> stride = {2, 1};
  std::vector<int> vals(4);
  EXPECT_TRUE(arr1->Read(start.data(), count.data(), step.data(), stride.data(),
                         GDALExtendedDataType::Create(GDT_Int32), vals.data()));
  EXPECT_THAT(vals, ElementsAre(40, 30, 20, 10));

  // Read second array (hits source dataset cache)
  std::vector<int> vals2(4);
  const std::vector<GUInt64> start_fwd = {0, 0};
  const std::vector<GInt64> step_fwd = {1, 1};
  EXPECT_TRUE(arr2->Read(start_fwd.data(), count.data(), step_fwd.data(),
                         stride.data(), GDALExtendedDataType::Create(GDT_Int32),
                         vals2.data()));
  EXPECT_EQ(10, vals2[0]);
  EXPECT_EQ(40, vals2[3]);
}

// =========================================================================
// Array Fill with NoData (arrays without sources)
// =========================================================================

TEST_F(VrtMultidimTest, VRTMDArrayFillNoData) {
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="dim2" size="2"/>
    <Array name="empty_num">
      <DataType>Float32</DataType>
      <DimensionRef ref="dim2"/>
      <NoDataValue>-999</NoDataValue>
    </Array>
    <Array name="empty_str">
      <DataType>String</DataType>
      <DimensionRef ref="dim2"/>
    </Array>
  </Group>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto num_arr = root->OpenMDArray("empty_num");
  ASSERT_NE(nullptr, num_arr);
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {2};
  std::vector<float> f_vals(2, 0.0f);
  EXPECT_TRUE(num_arr->Read(start.data(), count.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Float32),
                            f_vals.data()));
  EXPECT_FLOAT_EQ(-999.0f, f_vals[0]);
  EXPECT_FLOAT_EQ(-999.0f, f_vals[1]);

  auto str_arr = root->OpenMDArray("empty_str");
  ASSERT_NE(nullptr, str_arr);
  std::vector<char*> str_vals(2, nullptr);
  EXPECT_TRUE(str_arr->Read(start.data(), count.data(), nullptr, nullptr,
                            GDALExtendedDataType::CreateString(),
                            str_vals.data()));
  EXPECT_EQ(nullptr, str_vals[0]);
  EXPECT_EQ(nullptr, str_vals[1]);
}

// =========================================================================
// Programmatic MDArray Creation Options & Errors
// =========================================================================

TEST_F(VrtMultidimTest, CreateMDArrayOptionsAndErrors) {
  constexpr char kVrtPath[] = "/vsimem/vrtmultidim_test/create_options.vrt";

  auto ds = VRTDataset::CreateVRTMultiDimensional(kVrtPath, nullptr, nullptr);
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim_y = root->CreateDimension("Y", "", "", 4);
  auto dim_x = root->CreateDimension("X", "", "", 4);

  // 1. Create with BLOCKSIZE option
  CPLStringList block_options;
  block_options.AddString("BLOCKSIZE=2,2");
  auto arr1 = root->CreateMDArray("arr_block", {dim_y, dim_x},
                                  GDALExtendedDataType::Create(GDT_Int32),
                                  block_options.List());
  ASSERT_NE(nullptr, arr1);
  EXPECT_THAT(arr1->GetBlockSize(), ElementsAre(2, 2));

  WithQuietHandler quiet;

  // 2. Duplicate array name
  CPLErrorReset();
  EXPECT_EQ(nullptr, root->CreateMDArray(
                         "arr_block", {dim_y, dim_x},
                         GDALExtendedDataType::Create(GDT_Int32), nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // 3. Invalid BLOCKSIZE count
  CPLStringList bad_block_options;
  bad_block_options.AddString("BLOCKSIZE=2");
  CPLErrorReset();
  EXPECT_EQ(nullptr,
            root->CreateMDArray("arr_bad_block", {dim_y, dim_x},
                                GDALExtendedDataType::Create(GDT_Int32),
                                bad_block_options.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // 4. CreateDimension with empty name or duplicate name
  CPLErrorReset();
  EXPECT_EQ(nullptr, root->CreateDimension("", "", "", 10));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(nullptr, root->CreateDimension("Y", "", "", 4));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // 5. CreateAttribute duplicate on group
  EXPECT_NE(nullptr, root->CreateAttribute(
                         "attr1", {}, GDALExtendedDataType::CreateString()));
  CPLErrorReset();
  EXPECT_EQ(nullptr, root->CreateAttribute(
                         "attr1", {}, GDALExtendedDataType::CreateString()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // 6. CreateAttribute duplicate on array
  EXPECT_NE(nullptr,
            arr1->CreateAttribute("arr_attr1", {},
                                  GDALExtendedDataType::CreateString()));
  CPLErrorReset();
  EXPECT_EQ(nullptr,
            arr1->CreateAttribute("arr_attr1", {},
                                  GDALExtendedDataType::CreateString()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

// =========================================================================
// VRTParseArraySource Direct & DerivedArray Error Steps
// =========================================================================

TEST_F(VrtMultidimTest, VRTParseArraySourceDirectAndDerivedErrors) {
  WithQuietHandler quiet;

  // 1. VRTParseArraySource with non-ArraySource element name
  {
    CPLXMLTreeCloser wrong_node(
        CPLCreateXMLNode(nullptr, CXT_Element, "InvalidTag"));
    VRTMapSharedResources map_shared;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              VRTParseArraySource(wrong_node.get(), nullptr, map_shared));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. Resample without Dimension elements
  {
    constexpr std::string_view kBaseXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="4"/>
    <Dimension name="X" size="4"/>
    <Array name="grid">
      <DataType>Int32</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <InlineValues>0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";
    VsiMemTempWrapper base_vrt(
        "/vsimem/vrtmultidim_test/parse_array_source_base.vrt",
        std::string(kBaseXml));

    constexpr char kNoDimsXml[] = R"vrt(
<VRTDataset rasterXSize="2" rasterYSize="2">
  <VRTRasterBand dataType="Int32" band="1">
    <ArraySource>
      <DerivedArray>
        <SingleSourceArray>
          <SourceFilename>/vsimem/vrtmultidim_test/parse_array_source_base.vrt</SourceFilename>
          <SourceArray>/grid</SourceArray>
        </SingleSourceArray>
        <Step>
          <Resample>
            <Option name="SRC_METHOD">NO_GEOTRANSFORM</Option>
          </Resample>
        </Step>
      </DerivedArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";
    CPLErrorReset();
    CPL_IGNORE_RET_VAL(OpenRasterQuiet(kNoDimsXml));
    CPLErrorReset();
  }

  // 3. Missing name on Group
  {
    constexpr char kGroupMissingName[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Group/>
  </Group>
</VRTDataset>)vrt";
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenMultidimQuiet(kGroupMissingName));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  CPLErrorReset();
}

// =========================================================================
// Error Branches & Malformed XML
// =========================================================================

TEST_F(VrtMultidimTest, XMLInitErrorBranches) {
  WithQuietHandler quiet;

  constexpr std::string_view kBadMultidimXmls[] = {
      // 1. Group missing name
      "<VRTDataset><Group/></VRTDataset>",
      // 2. Dimension missing name or size
      "<VRTDataset><Group name=\"/\"><Dimension/></Group></VRTDataset>",
      // 3. Array missing name or DataType
      "<VRTDataset><Group "
      "name=\"/\"><Array><DataType>Int32</DataType></Array></Group></"
      "VRTDataset>",
      // 4. Attribute missing name
      "<VRTDataset><Group name=\"/\"><Attribute "
      "type=\"String\"/></Group></VRTDataset>",
  };

  for (size_t i = 0; i < std::size(kBadMultidimXmls); ++i) {
    SCOPED_TRACE(i);
    CPLErrorReset();
    std::string xml(kBadMultidimXmls[i]);
    EXPECT_EQ(nullptr, OpenMultidimQuiet(xml.c_str()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 5. ArraySource with missing SingleSourceArray
  constexpr char kMissingSingleSource[] = R"vrt(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1">
    <ArraySource/>
  </VRTRasterBand>
</VRTDataset>)vrt";
  CPLErrorReset();
  EXPECT_EQ(nullptr, OpenRasterQuiet(kMissingSingleSource));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(VrtMultidimTest, SingleSourceArrayAndDerivedArraySteps) {
  constexpr std::string_view kSrcMdXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="2"/>
    <Dimension name="X" size="3"/>
    <Dimension name="Z" size="2"/>
    <Array name="var">
      <DataType>Byte</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <ConstantValue>42</ConstantValue>
    </Array>
    <Array name="var_unscaled">
      <DataType>Byte</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <ConstantValue>10</ConstantValue>
      <Offset>10</Offset>
      <Scale>2</Scale>
    </Array>
    <Array name="var_3d">
      <DataType>Byte</DataType>
      <DimensionRef ref="Z"/>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <ConstantValue>1</ConstantValue>
    </Array>
  </Group>
</VRTDataset>)vrt";
  VsiMemTempWrapper src_md_file("/vsimem/vrtmultidim_test/src_md.vrt",
                                std::string(kSrcMdXml));

  // 1. ArraySource with SingleSourceArray
  {
    constexpr std::string_view kTestVrt = R"vrt(
<VRTDataset rasterXSize="3" rasterYSize="2">
  <VRTRasterBand dataType="Byte" band="1" subClass="VRTSourcedRasterBand">
    <ArraySource>
      <SingleSourceArray>
        <SourceFilename relativeToVRT="1">src_md.vrt</SourceFilename>
        <SourceArray>/var</SourceArray>
      </SingleSourceArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";
    constexpr char kTestVrtPath[] =
        "/vsimem/vrtmultidim_test/test_array_src.vrt";
    VsiMemTempWrapper test_vrt_file(kTestVrtPath, std::string(kTestVrt));

    GDALDatasetUniquePtr ds = OpenRasterVerbose(kTestVrtPath);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);

    int success = 0;
    band->GetMinimum(&success);
    band->GetMaximum(&success);

    GUIntBig histogram[10] = {};
    EXPECT_EQ(CE_None, band->GetHistogram(0, 255, 10, histogram, 0, 1, nullptr,
                                          nullptr));

    std::vector<GByte> buf(6);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 3, 2, buf.data(), 3, 2,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(42, buf[0]);

    auto* sourced_band = dynamic_cast<VRTSourcedRasterBand*>(band);
    ASSERT_NE(nullptr, sourced_band);

    char** vrt_xml_list = ds->GetMetadata("xml:VRT");
    ASSERT_NE(nullptr, vrt_xml_list);
    ASSERT_NE(nullptr, vrt_xml_list[0]);
    EXPECT_THAT(vrt_xml_list[0], HasSubstr("ArraySource"));
  }

  // 2. SingleSourceArray error conditions
  {
    WithQuietHandler quiet;
    constexpr std::string_view kBadSingleSources[] = {
        // Missing SourceFilename
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\"><VRTRasterBand "
        "dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"><ArraySource><SingleSourceArray><"
        "SourceArray>/var</SourceArray></SingleSourceArray></ArraySource></"
        "VRTRasterBand></VRTDataset>",
        // Missing SourceArray
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\"><VRTRasterBand "
        "dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"><ArraySource><SingleSourceArray><"
        "SourceFilename>/vsimem/vrtmultidim_test/src_md.vrt</SourceFilename></"
        "SingleSourceArray></ArraySource></VRTRasterBand></VRTDataset>",
        // Array dimension != 2 (var_3d)
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\"><VRTRasterBand "
        "dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"><ArraySource><SingleSourceArray><"
        "SourceFilename>/vsimem/vrtmultidim_test/src_md.vrt</"
        "SourceFilename><SourceArray>/var_3d</SourceArray></"
        "SingleSourceArray></ArraySource></VRTRasterBand></VRTDataset>",
        // Array not found
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\"><VRTRasterBand "
        "dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"><ArraySource><SingleSourceArray><"
        "SourceFilename>/vsimem/vrtmultidim_test/src_md.vrt</"
        "SourceFilename><SourceArray>/not_found</SourceArray></"
        "SingleSourceArray></ArraySource></VRTRasterBand></VRTDataset>",
    };
    for (size_t i = 0; i < std::size(kBadSingleSources); ++i) {
      SCOPED_TRACE(i);
      CPLErrorReset();
      std::string xml(kBadSingleSources[i]);
      EXPECT_EQ(nullptr, OpenRasterQuiet(xml.c_str()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
    CPLErrorReset();
  }

  // 3. DerivedArray with <GetUnscaled/>
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="3" rasterYSize="2">
  <VRTRasterBand dataType="Float64" band="1" subClass="VRTSourcedRasterBand">
    <ArraySource>
      <DerivedArray>
        <SingleSourceArray>
          <SourceFilename>/vsimem/vrtmultidim_test/src_md.vrt</SourceFilename>
          <SourceArray>/var_unscaled</SourceArray>
        </SingleSourceArray>
        <Step>
          <GetUnscaled/>
        </Step>
      </DerivedArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr ds = OpenRasterVerbose(kVrtXml);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    std::vector<double> buf(6);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 3, 2, buf.data(), 3, 2,
                                      GDT_Float64, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(30.0, buf[0]);
  }

  // 4. DerivedArray with <Resample> and <Option>
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="6" rasterYSize="4">
  <VRTRasterBand dataType="Byte" band="1" subClass="VRTSourcedRasterBand">
    <ArraySource>
      <DerivedArray>
        <SingleSourceArray>
          <SourceFilename>/vsimem/vrtmultidim_test/src_md.vrt</SourceFilename>
          <SourceArray>/var</SourceArray>
        </SingleSourceArray>
        <Step>
          <Resample>
            <Dimension name="dimY" size="4"/>
            <Dimension name="dimX" size="6"/>
            <ResampleAlg>NEAR</ResampleAlg>
            <SRS>EPSG:4326</SRS>
            <Option name="OVERSAMPLING">1.0</Option>
          </Resample>
        </Step>
      </DerivedArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr ds = OpenRasterQuiet(kVrtXml);
    if (ds != nullptr) {
      GDALRasterBand* band = ds->GetRasterBand(1);
      if (band != nullptr) {
        std::vector<GByte> buf(24);
        CPL_IGNORE_RET_VAL(band->RasterIO(GF_Read, 0, 0, 6, 4, buf.data(), 6, 4,
                                          GDT_Byte, 0, 0, nullptr));
      }
    }
    CPLErrorReset();
  }

  // 5. DerivedArray errors
  {
    WithQuietHandler quiet;
    constexpr std::string_view kBadDerivedXmls[] = {
        // Missing expr in View
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\"><VRTRasterBand "
        "dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"><ArraySource><DerivedArray><"
        "SingleSourceArray><SourceFilename>/vsimem/vrtmultidim_test/"
        "src_md.vrt</SourceFilename><SourceArray>/var</SourceArray></"
        "SingleSourceArray><Step><View/></Step></DerivedArray></ArraySource></"
        "VRTRasterBand></VRTDataset>",
        // Missing newOrder in Transpose
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\"><VRTRasterBand "
        "dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"><ArraySource><DerivedArray><"
        "SingleSourceArray><SourceFilename>/vsimem/vrtmultidim_test/"
        "src_md.vrt</SourceFilename><SourceArray>/var</SourceArray></"
        "SingleSourceArray><Step><Transpose/></Step></DerivedArray></"
        "ArraySource></VRTRasterBand></VRTDataset>",
        // Invalid SRS in Resample
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\"><VRTRasterBand "
        "dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"><ArraySource><DerivedArray><"
        "SingleSourceArray><SourceFilename>/vsimem/vrtmultidim_test/"
        "src_md.vrt</SourceFilename><SourceArray>/var</SourceArray></"
        "SingleSourceArray><Step><Resample><SRS>INVALID_SRS_XYZ</SRS></"
        "Resample></Step></DerivedArray></ArraySource></VRTRasterBand></"
        "VRTDataset>",
        // Missing GridOptions in Grid
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\"><VRTRasterBand "
        "dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"><ArraySource><DerivedArray><"
        "SingleSourceArray><SourceFilename>/vsimem/vrtmultidim_test/"
        "src_md.vrt</SourceFilename><SourceArray>/var</SourceArray></"
        "SingleSourceArray><Step><Grid/></Step></DerivedArray></ArraySource></"
        "VRTRasterBand></VRTDataset>",
        // Unknown Step element
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\"><VRTRasterBand "
        "dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"><ArraySource><DerivedArray><"
        "SingleSourceArray><SourceFilename>/vsimem/vrtmultidim_test/"
        "src_md.vrt</SourceFilename><SourceArray>/var</SourceArray></"
        "SingleSourceArray><Step><UnknownSubElement/></Step></DerivedArray></"
        "ArraySource></VRTRasterBand></VRTDataset>",
        // Missing name attribute in Option
        "<VRTDataset rasterXSize=\"3\" rasterYSize=\"2\"><VRTRasterBand "
        "dataType=\"Byte\" band=\"1\" "
        "subClass=\"VRTSourcedRasterBand\"><ArraySource><DerivedArray><"
        "SingleSourceArray><SourceFilename>/vsimem/vrtmultidim_test/"
        "src_md.vrt</SourceFilename><SourceArray>/var</SourceArray></"
        "SingleSourceArray><Step><GetMask><Option>no_name</Option></GetMask></"
        "Step></DerivedArray></ArraySource></VRTRasterBand></VRTDataset>",
    };
    for (size_t i = 0; i < std::size(kBadDerivedXmls); ++i) {
      SCOPED_TRACE(i);
      CPLErrorReset();
      std::string xml(kBadDerivedXmls[i]);
      EXPECT_EQ(nullptr, OpenRasterQuiet(xml.c_str()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
    CPLErrorReset();
  }
}

TEST_F(VrtMultidimTest, AdditionalCoverageAndEdgeCases) {
  // 1. VRTDimension Set/Get IndexingVariable across groups
  {
    constexpr char kVrtFile[] = "/vsimem/vrtmultidim_test/dim_test.vrt";
    auto ds = std::unique_ptr<VRTDataset>(
        VRTDataset::CreateVRTMultiDimensional(kVrtFile, nullptr, nullptr));
    ASSERT_NE(nullptr, ds);
    auto root = ds->GetRootVRTGroup();
    ASSERT_NE(nullptr, root);
    auto sub = root->CreateGroup("sub");
    ASSERT_NE(nullptr, sub);
    auto dim = root->CreateDimension("dim1", "", "", 2);
    ASSERT_NE(nullptr, dim);
    auto var_sub = sub->CreateMDArray("var_sub", {dim},
                                      GDALExtendedDataType::Create(GDT_Int32));
    ASSERT_NE(nullptr, var_sub);

    EXPECT_TRUE(dim->SetIndexingVariable(var_sub));
    auto got_var = dim->GetIndexingVariable();
    ASSERT_NE(nullptr, got_var);
    EXPECT_EQ(var_sub->GetFullName(), got_var->GetFullName());

    EXPECT_TRUE(dim->SetIndexingVariable(nullptr));
    EXPECT_EQ(nullptr, dim->GetIndexingVariable());
  }

  // 2. MDArray Source referencing 2D dataset with SourceBand and
  // SourceTranspose
  constexpr char kTifFile[] = "/vsimem/vrtmultidim_test/src2d.tif";
  CreateGeoTiff(kTifFile, 4, 4, 1, GDT_Byte, nullptr, nullptr,
                {std::vector<double>(16, 100.0)});

  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="4"/>
    <Dimension name="X" size="4"/>
    <Array name="arr">
      <DataType>Byte</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <Source>
        <SourceFilename>/vsimem/vrtmultidim_test/src2d.tif</SourceFilename>
        <SourceBand>1</SourceBand>
        <SourceTranspose>1,0</SourceTranspose>
      </Source>
    </Array>
  </Group>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
    ASSERT_NE(nullptr, ds);
    auto rg = ds->GetRootGroup();
    ASSERT_NE(nullptr, rg);
    auto arr = rg->OpenMDArray("arr");
    ASSERT_NE(nullptr, arr);
    std::vector<GByte> vals(16);
    const std::vector<GUInt64> start = {0, 0};
    const std::vector<size_t> count = {4, 4};
    EXPECT_TRUE(arr->Read(start.data(), count.data(), nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Byte), vals.data()));
    EXPECT_EQ(100, vals[0]);
  }

  // 2b. SourceView with resample=true
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="4"/>
    <Dimension name="X" size="4"/>
    <Array name="arr_resample">
      <DataType>Byte</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <Source>
        <SourceFilename>/vsimem/vrtmultidim_test/src2d.tif</SourceFilename>
        <SourceBand>1</SourceBand>
        <SourceView>resample=true</SourceView>
      </Source>
    </Array>
  </Group>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr ds = OpenMultidimQuiet(kVrtXml);
    if (ds != nullptr) {
      auto rg = ds->GetRootGroup();
      if (rg != nullptr) {
        auto arr = rg->OpenMDArray("arr_resample");
        if (arr != nullptr) {
          std::vector<GByte> vals(16);
          const std::vector<GUInt64> start = {0, 0};
          const std::vector<size_t> count = {4, 4};
          arr->Read(start.data(), count.data(), nullptr, nullptr,
                    GDALExtendedDataType::Create(GDT_Byte), vals.data());
        }
      }
    }
    CPLErrorReset();
  }

  // 3. InlineValues with invalid value count
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    constexpr char kBadVrt[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="dim" size="4"/>
    <Array name="arr">
      <DataType>Int32</DataType>
      <DimensionRef ref="dim"/>
      <InlineValues>1 2</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";
    EXPECT_EQ(nullptr, OpenMultidimQuiet(kBadVrt));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 4. Int32 array reading with non-compact stride and nodata
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="dim" size="4"/>
    <Array name="arr">
      <DataType>Int32</DataType>
      <DimensionRef ref="dim"/>
      <NoDataValue>99</NoDataValue>
      <InlineValues>1 2 3 4</InlineValues>
    </Array>
  </Group>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
    ASSERT_NE(nullptr, ds);
    auto rg = ds->GetRootGroup();
    ASSERT_NE(nullptr, rg);
    auto arr = rg->OpenMDArray("arr");
    ASSERT_NE(nullptr, arr);

    std::vector<int> buf(10, 0);
    const std::vector<GUInt64> start = {0};
    const std::vector<size_t> count = {2};
    const std::vector<GInt64> step = {1};
    const std::vector<GPtrDiff_t> stride = {2};
    EXPECT_TRUE(arr->Read(start.data(), count.data(), step.data(),
                          stride.data(),
                          GDALExtendedDataType::Create(GDT_Int32), buf.data()));
    EXPECT_EQ(1, buf[0]);
    EXPECT_EQ(2, buf[2]);
  }

  // 5. DerivedArray with <Grid> step
  {
    constexpr std::string_view kSrcMdXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="2"/>
    <Dimension name="X" size="3"/>
    <Array name="var">
      <DataType>Byte</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <ConstantValue>42</ConstantValue>
    </Array>
  </Group>
</VRTDataset>)vrt";
    VsiMemTempWrapper src_md_file("/vsimem/vrtmultidim_test/grid_step_src.vrt",
                                  std::string(kSrcMdXml));

    constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="4" rasterYSize="4">
  <VRTRasterBand dataType="Byte" band="1" subClass="VRTSourcedRasterBand">
    <ArraySource>
      <DerivedArray>
        <SingleSourceArray>
          <SourceFilename>/vsimem/vrtmultidim_test/grid_step_src.vrt</SourceFilename>
          <SourceArray>/var</SourceArray>
        </SingleSourceArray>
        <Step>
          <Grid>
            <GridOptions>invdist:power=2.0</GridOptions>
            <XArray>
              <SingleSourceArray>
                <SourceFilename>/vsimem/vrtmultidim_test/grid_step_src.vrt</SourceFilename>
                <SourceArray>/var</SourceArray>
              </SingleSourceArray>
            </XArray>
            <YArray>
              <SingleSourceArray>
                <SourceFilename>/vsimem/vrtmultidim_test/grid_step_src.vrt</SourceFilename>
                <SourceArray>/var</SourceArray>
              </SingleSourceArray>
            </YArray>
            <Option name="sample_option">val</Option>
          </Grid>
        </Step>
      </DerivedArray>
    </ArraySource>
  </VRTRasterBand>
</VRTDataset>)vrt";
    WithQuietHandler quiet;
    CPLErrorReset();
    CPL_IGNORE_RET_VAL(OpenRasterQuiet(kVrtXml));
    CPLErrorReset();
  }

  // 6. Attribute with empty list read
  {
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Attribute name="empty_attr">
      <DataType>String</DataType>
    </Attribute>
  </Group>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtXml);
    ASSERT_NE(nullptr, ds);
    auto rg = ds->GetRootGroup();
    ASSERT_NE(nullptr, rg);
    auto attr = rg->GetAttribute("empty_attr");
    ASSERT_NE(nullptr, attr);
    EXPECT_EQ(nullptr, attr->ReadAsString());
  }

  // 7. MDArray Source with SourceBand and serialize with relativetoVRT="1"
  {
    constexpr std::string_view kVrtXml = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="Y" size="4"/>
    <Dimension name="X" size="4"/>
    <Array name="arr_2d_as_md">
      <DataType>Byte</DataType>
      <DimensionRef ref="Y"/>
      <DimensionRef ref="X"/>
      <Source>
        <SourceFilename relativetoVRT="1">src2d.tif</SourceFilename>
        <SourceBand>1</SourceBand>
      </Source>
    </Array>
  </Group>
</VRTDataset>)vrt";
    constexpr char kVrtPath[] = "/vsimem/vrtmultidim_test/vrt_2d_as_md.vrt";
    VsiMemTempWrapper vrt_file(kVrtPath, std::string(kVrtXml));

    GDALDatasetUniquePtr ds = OpenMultidimVerbose(kVrtPath);
    ASSERT_NE(nullptr, ds);
    auto rg = ds->GetRootGroup();
    ASSERT_NE(nullptr, rg);
    auto arr = rg->OpenMDArray("arr_2d_as_md");
    ASSERT_NE(nullptr, arr);
    std::vector<GByte> vals(16);
    const std::vector<GUInt64> start = {0, 0};
    const std::vector<size_t> count = {4, 4};
    EXPECT_TRUE(arr->Read(start.data(), count.data(), nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Byte), vals.data()));
    EXPECT_EQ(100, vals[0]);

    auto vrt_group = std::dynamic_pointer_cast<VRTGroup>(rg);
    if (vrt_group != nullptr) {
      CPLXMLTreeCloser xml_tree(
          vrt_group->SerializeToXML("/vsimem/vrtmultidim_test"));
    }
  }

  // 8. Dimension indexingVariable not found error
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    constexpr char kVrtXml[] = R"vrt(
<VRTDataset>
  <Group name="/">
    <Dimension name="dim" size="4" indexingVariable="non_existent"/>
  </Group>
</VRTDataset>)vrt";
    GDALDatasetUniquePtr ds = OpenMultidimQuiet(kVrtXml);
    ASSERT_NE(nullptr, ds);
    auto rg = ds->GetRootGroup();
    ASSERT_NE(nullptr, rg);
    auto vrt_group = std::dynamic_pointer_cast<VRTGroup>(rg);
    ASSERT_NE(nullptr, vrt_group);
    auto dim = vrt_group->GetDimension("dim");
    ASSERT_NE(nullptr, dim);
    EXPECT_EQ(nullptr, dim->GetIndexingVariable());
    CPLErrorReset();
  }

  // 9. CreateGroup, CreateMDArray, and CreateAttribute validation errors
  {
    WithQuietHandler quiet;
    constexpr char kVrtFile1[] = "/vsimem/vrtmultidim_test/val_err1.vrt";
    constexpr char kVrtFile2[] = "/vsimem/vrtmultidim_test/val_err2.vrt";
    auto ds1 = std::unique_ptr<VRTDataset>(
        VRTDataset::CreateVRTMultiDimensional(kVrtFile1, nullptr, nullptr));
    auto ds2 = std::unique_ptr<VRTDataset>(
        VRTDataset::CreateVRTMultiDimensional(kVrtFile2, nullptr, nullptr));
    ASSERT_NE(nullptr, ds1);
    ASSERT_NE(nullptr, ds2);
    auto root1 = ds1->GetRootVRTGroup();
    auto root2 = ds2->GetRootVRTGroup();
    ASSERT_NE(nullptr, root1);
    ASSERT_NE(nullptr, root2);

    CPLErrorReset();
    EXPECT_EQ(nullptr, root1->CreateGroup(""));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    EXPECT_NE(nullptr, root1->CreateGroup("sub"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, root1->CreateGroup("sub"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(nullptr, root1->CreateMDArray(
                           "", {}, GDALExtendedDataType::Create(GDT_Int32)));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    auto dim2 = root2->CreateDimension("dim2", "", "", 10);
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              root1->CreateMDArray("arr_bad_dim", {dim2},
                                   GDALExtendedDataType::Create(GDT_Int32)));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(nullptr, root1->CreateAttribute(
                           "huge_attr", {static_cast<GUInt64>(INT_MAX) + 1},
                           GDALExtendedDataType::Create(GDT_Int32)));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive OOM and nested multi-dimensional
// array error paths in vrtmultidim.cpp.
