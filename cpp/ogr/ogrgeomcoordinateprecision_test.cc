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

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_geomcoordinateprecision.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

TEST(OgrGeomCoordinatePrecisionTest, CApiNullHandling) {
  WithQuietHandler quiet_handler;

  EXPECT_DOUBLE_EQ(0.0, OGRGeomCoordinatePrecisionGetXYResolution(nullptr));
  EXPECT_DOUBLE_EQ(0.0, OGRGeomCoordinatePrecisionGetZResolution(nullptr));
  EXPECT_DOUBLE_EQ(0.0, OGRGeomCoordinatePrecisionGetMResolution(nullptr));
  EXPECT_EQ(nullptr, OGRGeomCoordinatePrecisionGetFormats(nullptr));
  EXPECT_EQ(nullptr, OGRGeomCoordinatePrecisionGetFormatSpecificOptions(
                         nullptr, "OpenFileGDB"));

  OGRGeomCoordinatePrecisionSetFormatSpecificOptions(nullptr, "OpenFileGDB",
                                                     nullptr);
  OGRGeomCoordinatePrecisionSet(nullptr, 0.001, 0.001, 0.001);
  OGRGeomCoordinatePrecisionSetFromMeter(nullptr, nullptr, 0.001, 0.001, 0.001);

  OGRGeomCoordinatePrecisionDestroy(nullptr);
}

TEST(OgrGeomCoordinatePrecisionTest, CApiLifecycleAndAccessors) {
  OGRGeomCoordinatePrecisionH h = OGRGeomCoordinatePrecisionCreate();
  ASSERT_NE(nullptr, h);

  EXPECT_DOUBLE_EQ(OGR_GEOM_COORD_PRECISION_UNKNOWN,
                   OGRGeomCoordinatePrecisionGetXYResolution(h));
  EXPECT_DOUBLE_EQ(OGR_GEOM_COORD_PRECISION_UNKNOWN,
                   OGRGeomCoordinatePrecisionGetZResolution(h));
  EXPECT_DOUBLE_EQ(OGR_GEOM_COORD_PRECISION_UNKNOWN,
                   OGRGeomCoordinatePrecisionGetMResolution(h));

  OGRGeomCoordinatePrecisionSet(h, 0.01, 0.02, 0.03);
  EXPECT_DOUBLE_EQ(0.01, OGRGeomCoordinatePrecisionGetXYResolution(h));
  EXPECT_DOUBLE_EQ(0.02, OGRGeomCoordinatePrecisionGetZResolution(h));
  EXPECT_DOUBLE_EQ(0.03, OGRGeomCoordinatePrecisionGetMResolution(h));

  // Format options
  CPLStringList options;
  options.AddString("KEY=VALUE");
  OGRGeomCoordinatePrecisionSetFormatSpecificOptions(h, "FileGeodatabase",
                                                     options.List());

  char** formats = OGRGeomCoordinatePrecisionGetFormats(h);
  ASSERT_NE(nullptr, formats);
  EXPECT_STREQ("FileGeodatabase", formats[0]);
  CSLDestroy(formats);

  CSLConstList fetched =
      OGRGeomCoordinatePrecisionGetFormatSpecificOptions(h, "FileGeodatabase");
  ASSERT_NE(nullptr, fetched);
  EXPECT_STREQ("KEY=VALUE", fetched[0]);

  EXPECT_EQ(nullptr, OGRGeomCoordinatePrecisionGetFormatSpecificOptions(
                         h, "NonExistentFormat"));

  // SetFromMeter C API
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  OGRGeomCoordinatePrecisionSetFromMeter(h, OGRSpatialReference::ToHandle(&srs),
                                         1.0, 1.0, 0.1);
  EXPECT_LT(OGRGeomCoordinatePrecisionGetXYResolution(h), 1.0);

  OGRGeomCoordinatePrecisionDestroy(h);
}

TEST(OgrGeomCoordinatePrecisionTest, SetFromMeterCpp) {
  OGRGeomCoordinatePrecision prec;

  // Geographic SRS
  OGRSpatialReference geog_srs;
  geog_srs.importFromEPSG(4326);
  prec.SetFromMeter(&geog_srs, 10.0, 5.0, 0.1);
  EXPECT_NEAR(10.0 / (6378137.0 * M_PI / 180.0), prec.dfXYResolution, 1e-10);
  EXPECT_DOUBLE_EQ(5.0, prec.dfZResolution);
  EXPECT_DOUBLE_EQ(0.1, prec.dfMResolution);

  // Projected SRS
  OGRSpatialReference proj_srs;
  proj_srs.importFromEPSG(3857);
  prec.SetFromMeter(&proj_srs, 1.0, 2.0, 3.0);
  EXPECT_DOUBLE_EQ(1.0, prec.dfXYResolution);
  EXPECT_DOUBLE_EQ(2.0, prec.dfZResolution);

  // 3D SRS with EPSG:4979 (WGS 84 3D geographic)
  OGRSpatialReference srs_3d;
  srs_3d.importFromEPSG(4979);
  prec.SetFromMeter(&srs_3d, 1.0, 1.0, 0.0);
  EXPECT_EQ(3, srs_3d.GetAxesCount());

  // Null SRS
  prec.SetFromMeter(nullptr, 2.0, 3.0, 4.0);
  EXPECT_DOUBLE_EQ(2.0, prec.dfXYResolution);
  EXPECT_DOUBLE_EQ(3.0, prec.dfZResolution);
  EXPECT_DOUBLE_EQ(4.0, prec.dfMResolution);
}

TEST(OgrGeomCoordinatePrecisionTest, ConvertToOtherSRS) {
  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.001;
  prec.dfZResolution = 0.002;
  prec.dfMResolution = 0.003;
  prec.oFormatSpecificOptions["Format"] = CPLStringList();

  // Both null SRS: preserves format options
  OGRGeomCoordinatePrecision converted =
      prec.ConvertToOtherSRS(nullptr, nullptr);
  EXPECT_FALSE(converted.oFormatSpecificOptions.empty());

  // Same SRS: preserves format options
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  converted = prec.ConvertToOtherSRS(&srs, &srs);
  EXPECT_FALSE(converted.oFormatSpecificOptions.empty());

  // Different SRS: clears format options
  OGRSpatialReference dst_srs;
  dst_srs.importFromEPSG(3857);
  converted = prec.ConvertToOtherSRS(&srs, &dst_srs);
  EXPECT_TRUE(converted.oFormatSpecificOptions.empty());
}

TEST(OgrGeomCoordinatePrecisionTest, ResolutionToPrecision) {
  EXPECT_EQ(3, OGRGeomCoordinatePrecision::ResolutionToPrecision(0.001));
  EXPECT_EQ(2, OGRGeomCoordinatePrecision::ResolutionToPrecision(0.01));
  EXPECT_EQ(0, OGRGeomCoordinatePrecision::ResolutionToPrecision(1.0));
  EXPECT_EQ(0, OGRGeomCoordinatePrecision::ResolutionToPrecision(10.0));
}

}  // namespace
}  // namespace autotest2
