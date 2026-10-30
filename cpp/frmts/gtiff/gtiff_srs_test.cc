// Copyright 2016 Google Inc. All Rights Reserved.
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
// Tests geotiff raster driver Spatial Reference System (SRS) handling.
//
// The geotiff driver uses logic separate from the rest of GDAL to handle SRS
// support.
//
// See also:
//   https://gdal.org/drivers/raster/gtiff.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/tiff_srs.py

#include <stddef.h>

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"

using std::to_string;
using std::unique_ptr;

namespace autotest2 {
namespace {

const char kTestData[] =
    "/google3/third_party/gdal/autotest2/cpp/frmts/gtiff/testdata/";

class GtiffSrsTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_GTiff(); }
};

TEST_F(GtiffSrsTest, EpsgMany) {
  for (const int epsg :
       {2000, 2012, 2050, 2143, 2220, 3625, 3857, 4326, 32360, 32517, 32750}) {
    const std::string filepath = ::testing::SrcDir() + std::string(kTestData) +
                                 "epsg" + to_string(epsg) + ".tif";

    unique_ptr<GDALDataset> src(static_cast<GDALDataset *>(
        GDALOpenEx(filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));

    EXPECT_STREQ("", CPLGetLastErrorMsg());
    EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
    ASSERT_NE(nullptr, src);

    std::string projection(src->GetProjectionRef());

    OGRSpatialReference expected_srs;
    ASSERT_EQ(OGRERR_NONE, expected_srs.importFromEPSG(epsg))
        << "Failed to load epsg: " << epsg;

    char *expected_wkt = nullptr;
    expected_srs.exportToWkt(&expected_wkt);
    ASSERT_NE(nullptr, expected_wkt);

    EXPECT_STREQ(expected_wkt, projection.c_str())
        << "WKT1 mismatch for EPSG: " << epsg << "\n"
        << "See https://spatialreference.org/ref/epsg/" << epsg << "/";
    CPLFree(expected_wkt);
  }
}

// Rework of Python Grads test, but without Python so that debugging is easier:
// autotest2/gcore/tiff_read_test.py
TEST_F(GtiffSrsTest, Grads) {
  const std::string filepath =
      ::testing::SrcDir() + std::string(kTestData) + "test_gf.tif";

  unique_ptr<GDALDataset> src(static_cast<GDALDataset *>(
      GDALOpenEx(filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));

  std::string projection(src->GetProjectionRef());

  OGRSpatialReference src_srs(projection.c_str());
  OGRSpatialReference expected_srs(R"wkt(
      PROJCS["NTF (Paris) / Lambert zone II",
         GEOGCS["NTF (Paris)",
             DATUM["Nouvelle_Triangulation_Francaise_Paris",
                 SPHEROID["Clarke 1880 (IGN)",6378249.2,293.466021293627,
                     AUTHORITY["EPSG","7011"]],
                 TOWGS84[-168,-60,320,0,0,0,0],
                 AUTHORITY["EPSG","6807"]],
             PRIMEM["Paris",2.33722916999999,
                 AUTHORITY["EPSG","8903"]],
             UNIT["grad",0.0157079632679489,
                 AUTHORITY["EPSG","9105"]],
             AUTHORITY["EPSG","4807"]],
         PROJECTION["Lambert_Conformal_Conic_1SP"],
         PARAMETER["latitude_of_origin",52],
         PARAMETER["central_meridian",0],
         PARAMETER["scale_factor",0.99987742],
         PARAMETER["false_easting",600000],
         PARAMETER["false_northing",2200000],
         UNIT["metre",1,
             AUTHORITY["EPSG","9001"]],
         AXIS["Easting",EAST],
         AXIS["Northing",NORTH],
         AUTHORITY["EPSG","27572"]]
      )wkt");

  EXPECT_THAT(src_srs, IsSameAs(expected_srs));
}

}  // namespace
}  // namespace autotest2
