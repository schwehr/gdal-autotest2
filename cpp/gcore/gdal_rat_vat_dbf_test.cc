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
// Tests loading ESRI .vat.dbf raster attribute tables
// (gdal_rat_vat_dbf.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdal_rat_vat_dbf.cpp

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kSuiteDir[] = "/vsimem/gdal_rat_vat_dbf_test";

class GdalRatVatDbfTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterOGRShape(); }
};

TEST_F(GdalRatVatDbfTest, LoadNonExistentReturnsNull) {
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(
      nullptr,
      GDALLoadVATDBF("/vsimem/gdal_rat_vat_dbf_test/does_not_exist.vat.dbf"));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalRatVatDbfTest, LoadIntValueAndAllFieldTypes) {
  GDALDriver* shp_driver =
      GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
  ASSERT_NE(nullptr, shp_driver);

  constexpr char kDbfPath[] = "/vsimem/gdal_rat_vat_dbf_test/test_int_vat.dbf";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kSuiteDir); };
  {
    GDALDatasetUniquePtr ds(
        shp_driver->Create(kDbfPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->CreateLayer("vat", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, layer);

    OGRFieldDefn f_value("VALUE", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_value));

    OGRFieldDefn f_count("COUNT", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_count));

    OGRFieldDefn f_class("CLASS_NAME", OFTString);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_class));

    OGRFieldDefn f_red("RED", OFTReal);
    f_red.SetWidth(10);
    f_red.SetPrecision(4);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_red));

    OGRFieldDefn f_green("GREEN", OFTReal);
    f_green.SetWidth(10);
    f_green.SetPrecision(4);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_green));

    OGRFieldDefn f_blue("BLUE", OFTReal);
    f_blue.SetWidth(10);
    f_blue.SetPrecision(4);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_blue));

    OGRFieldDefn f_real("GENERIC_R", OFTReal);
    f_real.SetWidth(10);
    f_real.SetPrecision(3);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_real));

    OGRFieldDefn f_int("GENERIC_I", OFTInteger);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_int));

    OGRFieldDefn f_bool("GENERIC_B", OFTInteger);
    f_bool.SetSubType(OFSTBoolean);
    f_bool.SetWidth(1);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_bool));

    OGRFieldDefn f_date("GENERIC_D", OFTDate);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_date));

    OGRFieldDefn f_str("GENERIC_S", OFTString);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_str));

    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetField("VALUE", 10);
    feat->SetField("COUNT", 42);
    feat->SetField("CLASS_NAME", "Forest");
    feat->SetField("RED", 1.0);
    feat->SetField("GREEN", 0.5);
    feat->SetField("BLUE", 0.0);
    feat->SetField("GENERIC_R", 3.25);
    feat->SetField("GENERIC_I", 99);
    feat->SetField("GENERIC_B", 1);
    feat->SetField("GENERIC_D", 2025, 6, 15, 0, 0, 0.0f, 0);
    feat->SetField("GENERIC_S", "misc");
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  auto rat = GDALLoadVATDBF(kDbfPath);
  ASSERT_NE(nullptr, rat);
  EXPECT_EQ(11, rat->GetColumnCount());
  EXPECT_EQ(1, rat->GetRowCount());

  EXPECT_EQ(GFT_Integer, rat->GetTypeOfCol(0));
  EXPECT_EQ(GFU_MinMax, rat->GetUsageOfCol(0));
  EXPECT_EQ(10, rat->GetValueAsInt(0, 0));

  EXPECT_EQ(GFU_PixelCount, rat->GetUsageOfCol(1));
  EXPECT_EQ(42, rat->GetValueAsInt(0, 1));

  EXPECT_EQ(GFU_Name, rat->GetUsageOfCol(2));
  EXPECT_STREQ("Forest", rat->GetValueAsString(0, 2));

  EXPECT_EQ(GFU_Red, rat->GetUsageOfCol(3));
  EXPECT_EQ(255, rat->GetValueAsInt(0, 3));

  EXPECT_EQ(GFU_Green, rat->GetUsageOfCol(4));
  EXPECT_EQ(128, rat->GetValueAsInt(0, 4));

  EXPECT_EQ(GFU_Blue, rat->GetUsageOfCol(5));
  EXPECT_EQ(0, rat->GetValueAsInt(0, 5));

  EXPECT_EQ(GFT_Real, rat->GetTypeOfCol(6));
  EXPECT_NEAR(3.25, rat->GetValueAsDouble(0, 6), 1e-4);

  EXPECT_EQ(GFT_Integer, rat->GetTypeOfCol(7));
  EXPECT_EQ(99, rat->GetValueAsInt(0, 7));

  EXPECT_EQ(GFT_Boolean, rat->GetTypeOfCol(8));
  EXPECT_TRUE(rat->GetValueAsBoolean(0, 8));

  EXPECT_EQ(GFT_DateTime, rat->GetTypeOfCol(9));
  GDALRATDateTime dt = rat->GetValueAsDateTime(0, 9);
  EXPECT_TRUE(dt.bIsValid);
  EXPECT_EQ(2025, dt.nYear);
  EXPECT_EQ(6, dt.nMonth);
  EXPECT_EQ(15, dt.nDay);

  EXPECT_EQ(GFT_String, rat->GetTypeOfCol(10));
  EXPECT_STREQ("misc", rat->GetValueAsString(0, 10));
}

TEST_F(GdalRatVatDbfTest, LoadRealValueField) {
  GDALDriver* shp_driver =
      GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
  ASSERT_NE(nullptr, shp_driver);

  constexpr char kDbfPath[] = "/vsimem/gdal_rat_vat_dbf_test/test_real_vat.dbf";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kSuiteDir); };
  {
    GDALDatasetUniquePtr ds(
        shp_driver->Create(kDbfPath, 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, ds);
    OGRLayer* layer = ds->CreateLayer("vat", nullptr, wkbNone, nullptr);
    ASSERT_NE(nullptr, layer);

    OGRFieldDefn f_value("VALUE", OFTReal);
    f_value.SetWidth(10);
    f_value.SetPrecision(3);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_value));

    OGRFieldDefn f_name("NAME", OFTString);
    ASSERT_EQ(OGRERR_NONE, layer->CreateField(&f_name));

    auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    feat->SetField("VALUE", 1.5);
    feat->SetField("NAME", "Water");
    ASSERT_EQ(OGRERR_NONE, layer->CreateFeature(feat.get()));
  }

  auto rat = GDALLoadVATDBF(kDbfPath);
  ASSERT_NE(nullptr, rat);
  EXPECT_EQ(GFT_Real, rat->GetTypeOfCol(0));
  EXPECT_EQ(GFU_MinMax, rat->GetUsageOfCol(0));
  EXPECT_NEAR(1.5, rat->GetValueAsDouble(0, 0), 1e-4);
  EXPECT_EQ(GFU_Name, rat->GetUsageOfCol(1));
  EXPECT_STREQ("Water", rat->GetValueAsString(0, 1));
}

}  // namespace
}  // namespace autotest2
