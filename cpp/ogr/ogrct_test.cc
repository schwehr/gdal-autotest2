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

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_proj_p.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/ogr/ogrct_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::DoubleNear;
using ::testing::NotNull;

class OgrCtEnvironment : public ::testing::Environment {
 public:
  void TearDown() override {
    OSRCTCleanCache();
    OSRCleanup();
  }
};

testing::Environment* const kCtEnv =
    testing::AddGlobalTestEnvironment(new OgrCtEnvironment);

TEST(OgrCtTest, OptionsConstructorsAndGetters) {
  OGRCoordinateTransformationOptions options;
  EXPECT_TRUE(options.SetAreaOfInterest(-10.0, 40.0, 10.0, 50.0));
  options.SetCoordinateOperation("+proj=pipeline +step +proj=unitconvert",
                                 false);
  options.SetSourceCenterLong(0.0);
  options.SetTargetCenterLong(10.0);
  options.SetDesiredAccuracy(0.1);
  options.SetBallparkAllowed(true);
  options.SetOnlyBest(false);

  // Copy constructor
  OGRCoordinateTransformationOptions copy(options);
  // Copy assignment
  OGRCoordinateTransformationOptions copy2;
  copy2 = options;

  // Move constructor
  OGRCoordinateTransformationOptions moved(std::move(copy));
  // Move assignment
  OGRCoordinateTransformationOptions moved2;
  moved2 = std::move(copy2);

  // Self-assignment
  options = *&options;
}

TEST(OgrCtTest, OptionsCApi) {
  OGRCoordinateTransformationOptionsH hOptions =
      OCTNewCoordinateTransformationOptions();
  ASSERT_THAT(hOptions, NotNull());

  EXPECT_TRUE(OCTCoordinateTransformationOptionsSetAreaOfInterest(
      hOptions, -120.0, 30.0, -110.0, 40.0));
  EXPECT_TRUE(OCTCoordinateTransformationOptionsSetOperation(
      hOptions, "+proj=pipeline +step +proj=unitconvert", false));
  EXPECT_TRUE(
      OCTCoordinateTransformationOptionsSetDesiredAccuracy(hOptions, 0.5));
  EXPECT_TRUE(
      OCTCoordinateTransformationOptionsSetBallparkAllowed(hOptions, true));
  EXPECT_TRUE(OCTCoordinateTransformationOptionsSetOnlyBest(hOptions, false));

  OCTDestroyCoordinateTransformationOptions(hOptions);
}

TEST(OgrCtTest, CreateInvalidTransformations) {
  WithQuietHandler quiet;

  // Null source and target without coordinate operation
  EXPECT_EQ(nullptr, OGRCreateCoordinateTransformation(nullptr, nullptr));

  OGRSpatialReference srsWGS84;
  srsWGS84.SetWellKnownGeogCS("WGS84");

  EXPECT_EQ(nullptr, OGRCreateCoordinateTransformation(&srsWGS84, nullptr));
  EXPECT_EQ(nullptr, OGRCreateCoordinateTransformation(nullptr, &srsWGS84));

  // Invalid coordinate operation
  OGRCoordinateTransformationOptions options;
  options.SetCoordinateOperation("invalid_operation_pipeline", false);
  EXPECT_EQ(nullptr,
            OGRCreateCoordinateTransformation(nullptr, nullptr, options));
}

TEST(OgrCtTest, UserSpecifiedCoordinateOperation) {
  OGRCoordinateTransformationOptions options;
  options.SetCoordinateOperation(
      "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad", false);

  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(nullptr, nullptr, options));
  ASSERT_THAT(ct, NotNull());

  double x = 180.0;
  double y = 90.0;
  int bSuccess = FALSE;
  EXPECT_TRUE(ct->Transform(1, &x, &y, nullptr, nullptr, &bSuccess));
  EXPECT_TRUE(bSuccess);
  EXPECT_NEAR(x, M_PI, 1e-7);
  EXPECT_NEAR(y, M_PI / 2.0, 1e-7);
}

TEST(OgrCtTest, WebMercatorToWgs84AndInverse) {
  OGRSpatialReference srs3857;
  srs3857.importFromEPSG(3857);
  OGRSpatialReference srs4326;
  srs4326.importFromEPSG(4326);

  // 3857 -> 4326
  auto ctForward = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srs3857, &srs4326));
  ASSERT_THAT(ctForward, NotNull());

  double x = 0.0;
  double y = 0.0;
  int bSuccess = FALSE;
  EXPECT_TRUE(ctForward->Transform(1, &x, &y, nullptr, nullptr, &bSuccess));
  EXPECT_TRUE(bSuccess);
  EXPECT_NEAR(x, 0.0, 1e-6);
  EXPECT_NEAR(y, 0.0, 1e-6);

  // 4326 -> 3857
  auto ctReverse = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srs4326, &srs3857));
  ASSERT_THAT(ctReverse, NotNull());

  double lat = 0.0;
  double lon = 0.0;
  EXPECT_TRUE(ctReverse->Transform(1, &lat, &lon, nullptr, nullptr, &bSuccess));
  EXPECT_TRUE(bSuccess);
  EXPECT_NEAR(lat, 0.0, 1e-6);
  EXPECT_NEAR(lon, 0.0, 1e-6);

  // Clone & Inverse
  auto clone = std::unique_ptr<OGRCoordinateTransformation>(ctForward->Clone());
  ASSERT_THAT(clone, NotNull());

  auto inverse =
      std::unique_ptr<OGRCoordinateTransformation>(ctForward->GetInverse());
  ASSERT_THAT(inverse, NotNull());
}

TEST(OgrCtTest, TransformVariations) {
  OGRSpatialReference srsWGS84;
  srsWGS84.SetWellKnownGeogCS("WGS84");
  srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srsUTM;
  srsUTM.importFromEPSG(32631);  // UTM zone 31N
  srsUTM.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
  ASSERT_THAT(ct, NotNull());

  // 2D transform
  double x2d = 3.0;
  double y2d = 50.0;
  EXPECT_TRUE(ct->Transform(1, &x2d, &y2d));

  // 3D transform
  double x3d = 3.0;
  double y3d = 50.0;
  double z3d = 100.0;
  EXPECT_TRUE(ct->Transform(1, &x3d, &y3d, &z3d));
  EXPECT_DOUBLE_EQ(100.0, z3d);

  // 4D transform
  double x4d = 3.0;
  double y4d = 50.0;
  double z4d = 50.0;
  double m4d = 2020.0;
  int bSuccess = FALSE;
  EXPECT_TRUE(ct->Transform(1, &x4d, &y4d, &z4d, &m4d, &bSuccess));
  EXPECT_TRUE(bSuccess);

  // TransformWithErrorCodes with fresh input
  x4d = 3.0;
  y4d = 50.0;
  z4d = 50.0;
  m4d = 2020.0;
  int errCode = -1;
  EXPECT_TRUE(ct->TransformWithErrorCodes(1, &x4d, &y4d, &z4d, &m4d, &errCode));
  EXPECT_EQ(0, errCode);

  // Zero count with non-null success buffer
  EXPECT_TRUE(ct->Transform(0, nullptr, nullptr, nullptr, &bSuccess));
  EXPECT_TRUE(ct->TransformWithErrorCodes(0, nullptr, nullptr, nullptr, nullptr,
                                          nullptr));
}

TEST(OgrCtTest, TransformBounds) {
  OGRSpatialReference srsWGS84;
  srsWGS84.SetWellKnownGeogCS("WGS84");
  srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srs3857;
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsWGS84, &srs3857));
  ASSERT_THAT(ct, NotNull());

  double outXMin = 0.0;
  double outYMin = 0.0;
  double outXMax = 0.0;
  double outYMax = 0.0;

  EXPECT_TRUE(ct->TransformBounds(-10.0, 40.0, 10.0, 50.0, &outXMin, &outYMin,
                                  &outXMax, &outYMax, 21));
  EXPECT_LT(outXMin, outXMax);
  EXPECT_LT(outYMin, outYMax);

  // Densify = 0
  EXPECT_TRUE(ct->TransformBounds(-10.0, 40.0, 10.0, 50.0, &outXMin, &outYMin,
                                  &outXMax, &outYMax, 0));
}

TEST(OgrCtTest, CApiFunctions) {
  OGRSpatialReference srsWGS84;
  srsWGS84.SetWellKnownGeogCS("WGS84");
  srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srs3857;
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRCoordinateTransformationH hCT =
      OCTNewCoordinateTransformation(OGRSpatialReference::ToHandle(&srsWGS84),
                                     OGRSpatialReference::ToHandle(&srs3857));
  ASSERT_THAT(hCT, NotNull());

  // GetSourceCS & GetTargetCS
  EXPECT_THAT(OCTGetSourceCS(hCT), NotNull());
  EXPECT_THAT(OCTGetTargetCS(hCT), NotNull());

  // Clone
  OGRCoordinateTransformationH hClone = OCTClone(hCT);
  ASSERT_THAT(hClone, NotNull());

  // Inverse
  OGRCoordinateTransformationH hInverse = OCTGetInverse(hCT);
  ASSERT_THAT(hInverse, NotNull());

  // OCTTransform
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  EXPECT_TRUE(OCTTransform(hCT, 1, &x, &y, &z));

  // OCTTransformEx
  int bSuccess = FALSE;
  EXPECT_TRUE(OCTTransformEx(hCT, 1, &x, &y, &z, &bSuccess));
  EXPECT_TRUE(bSuccess);

  // OCTTransform4D
  double m = 0.0;
  EXPECT_TRUE(OCTTransform4D(hCT, 1, &x, &y, &z, &m, &bSuccess));

  // OCTTransform4DWithErrorCodes
  int errCode = -1;
  EXPECT_TRUE(OCTTransform4DWithErrorCodes(hCT, 1, &x, &y, &z, &m, &errCode));
  EXPECT_EQ(0, errCode);

  // OCTTransformBounds
  double outXMin = 0.0;
  double outYMin = 0.0;
  double outXMax = 0.0;
  double outYMax = 0.0;
  EXPECT_TRUE(OCTTransformBounds(hCT, -5.0, -5.0, 5.0, 5.0, &outXMin, &outYMin,
                                 &outXMax, &outYMax, 5));

  OCTDestroyCoordinateTransformation(hClone);
  OCTDestroyCoordinateTransformation(hInverse);
  OCTDestroyCoordinateTransformation(hCT);

  // Statistics dump
  OGRCTDumpStatistics();
}

TEST(OgrCtTest, DifferentOperationsUsed) {
  WithQuietHandler quiet;

  // Passing non-OGRProjCT or nullptr
  OGRProjCTDifferentOperationsStart(nullptr);
  OGRProjCTDifferentOperationsStop(nullptr);
  EXPECT_FALSE(OGRProjCTDifferentOperationsUsed(nullptr));

  OGRSpatialReference srsNAD27;
  srsNAD27.importFromEPSG(4267);
  OGRSpatialReference srsWGS84;
  srsWGS84.importFromEPSG(4326);

  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsNAD27, &srsWGS84));
  if (ct) {
    OGRProjCTDifferentOperationsStart(ct.get());
    double x[2] = {40.0, 45.0};
    double y[2] = {-100.0, -70.0};
    ct->Transform(2, x, y);
    OGRProjCTDifferentOperationsStop(ct.get());
    CPL_IGNORE_RET_VAL(OGRProjCTDifferentOperationsUsed(ct.get()));
  }
}

TEST(OgrCtTest, ConfigOptionsAndCache) {
  OSRCTCleanCache();

  CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", "YES");
  CPLSetConfigOption("OGR_CT_FORCE_TRADITIONAL_GIS_ORDER", "YES");
  CPLSetConfigOption("OGR_CT_OP_SELECTION", "BEST_ACCURACY");

  auto cleanup = absl::MakeCleanup([] {
    CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", nullptr);
    CPLSetConfigOption("OGR_CT_FORCE_TRADITIONAL_GIS_ORDER", nullptr);
    CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr);
    OSRCTCleanCache();
  });

  OGRSpatialReference srsWGS84;
  srsWGS84.SetWellKnownGeogCS("WGS84");
  OGRSpatialReference srsUTM;
  srsUTM.importFromEPSG(32632);

  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
  ASSERT_THAT(ct, NotNull());

  // Second lookup will find from cache if destroyed via DestroyCT
  OGRCoordinateTransformation::DestroyCT(ct.release());

  auto ct2 = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
  ASSERT_THAT(ct2, NotNull());
}

TEST(OgrCtTest, TransformBoundsPolesAndAntimeridian) {
  // 1. UPS North to WGS84 with traditional GIS order (lon, lat)
  {
    OGRSpatialReference srsUPSNorth;
    srsUPSNorth.importFromEPSG(5041);
    OGRSpatialReference srsWGS84LonLat;
    srsWGS84LonLat.importFromEPSG(4326);
    srsWGS84LonLat.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsUPSNorth, &srsWGS84LonLat));
    ASSERT_THAT(ct, NotNull());

    double outXMin = 0;
    double outYMin = 0;
    double outXMax = 0;
    double outYMax = 0;
    // Bounds enclosing the North Pole (2000000, 2000000)
    EXPECT_TRUE(ct->TransformBounds(1900000.0, 1900000.0, 2100000.0, 2100000.0,
                                    &outXMin, &outYMin, &outXMax, &outYMax,
                                    21));
    EXPECT_DOUBLE_EQ(-180.0, outXMin);
    EXPECT_DOUBLE_EQ(180.0, outXMax);
    EXPECT_DOUBLE_EQ(90.0, outYMax);

    // Bounds not enclosing pole
    EXPECT_TRUE(ct->TransformBounds(2100000.0, 2100000.0, 2200000.0, 2200000.0,
                                    &outXMin, &outYMin, &outXMax, &outYMax,
                                    21));
  }

  // 2. UPS North to WGS84 with EPSG order (lat, lon)
  {
    OGRSpatialReference srsUPSNorth;
    srsUPSNorth.importFromEPSG(5041);
    OGRSpatialReference srsWGS84LatLon;
    srsWGS84LatLon.importFromEPSG(4326);
    srsWGS84LatLon.SetAxisMappingStrategy(OAMS_AUTHORITY_COMPLIANT);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsUPSNorth, &srsWGS84LatLon));
    ASSERT_THAT(ct, NotNull());

    double outXMin = 0;
    double outYMin = 0;
    double outXMax = 0;
    double outYMax = 0;
    EXPECT_TRUE(ct->TransformBounds(1900000.0, 1900000.0, 2100000.0, 2100000.0,
                                    &outXMin, &outYMin, &outXMax, &outYMax,
                                    21));
    EXPECT_DOUBLE_EQ(90.0, outXMax);
    EXPECT_DOUBLE_EQ(-180.0, outYMin);
    EXPECT_DOUBLE_EQ(180.0, outYMax);

    // Bounds not enclosing pole in EPSG order
    EXPECT_TRUE(ct->TransformBounds(2100000.0, 2100000.0, 2200000.0, 2200000.0,
                                    &outXMin, &outYMin, &outXMax, &outYMax,
                                    21));
  }

  // 3. UPS South to WGS84 with traditional GIS order (lon, lat)
  {
    OGRSpatialReference srsUPSSouth;
    srsUPSSouth.importFromEPSG(5042);
    OGRSpatialReference srsWGS84LonLat;
    srsWGS84LonLat.importFromEPSG(4326);
    srsWGS84LonLat.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsUPSSouth, &srsWGS84LonLat));
    ASSERT_THAT(ct, NotNull());

    double outXMin = 0;
    double outYMin = 0;
    double outXMax = 0;
    double outYMax = 0;
    // Bounds enclosing the South Pole (2000000, 2000000)
    EXPECT_TRUE(ct->TransformBounds(1900000.0, 1900000.0, 2100000.0, 2100000.0,
                                    &outXMin, &outYMin, &outXMax, &outYMax,
                                    21));
    EXPECT_DOUBLE_EQ(-180.0, outXMin);
    EXPECT_DOUBLE_EQ(180.0, outXMax);
    EXPECT_DOUBLE_EQ(-90.0, outYMin);
  }

  // 4. UPS South to WGS84 with EPSG order (lat, lon)
  {
    OGRSpatialReference srsUPSSouth;
    srsUPSSouth.importFromEPSG(5042);
    OGRSpatialReference srsWGS84LatLon;
    srsWGS84LatLon.importFromEPSG(4326);
    srsWGS84LatLon.SetAxisMappingStrategy(OAMS_AUTHORITY_COMPLIANT);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsUPSSouth, &srsWGS84LatLon));
    ASSERT_THAT(ct, NotNull());

    double outXMin = 0;
    double outYMin = 0;
    double outXMax = 0;
    double outYMax = 0;
    EXPECT_TRUE(ct->TransformBounds(1900000.0, 1900000.0, 2100000.0, 2100000.0,
                                    &outXMin, &outYMin, &outXMax, &outYMax,
                                    21));
    EXPECT_DOUBLE_EQ(-90.0, outXMin);
    EXPECT_DOUBLE_EQ(-180.0, outYMin);
    EXPECT_DOUBLE_EQ(180.0, outYMax);
  }
}

TEST(OgrCtTest, DetectWebMercatorProj4Fallback) {
  OGRSpatialReference srsMerc;
  srsMerc.importFromProj4(
      "+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0 +lon_0=0.0 +x_0=0.0 "
      "+y_0=0 +k=1.0 +units=m +nadgrids=@null +wktext +no_defs");
  OGRSpatialReference srsWgs;
  srsWgs.importFromProj4("+proj=longlat +datum=WGS84 +no_defs");

  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsMerc, &srsWgs));
  ASSERT_THAT(ct, NotNull());

  double x = 1000.0;
  double y = 1000.0;
  EXPECT_TRUE(ct->Transform(1, &x, &y));
}

TEST(OgrCtTest, TargetLongitudeWrapping) {
  // Wrap target longitude at 180 degrees
  OGRCoordinateTransformationOptions options;
  options.SetTargetCenterLong(180.0);

  OGRSpatialReference srsUTM;
  srsUTM.importFromEPSG(32631);
  srsUTM.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srsWGS84;
  srsWGS84.importFromEPSG(4326);
  srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsUTM, &srsWGS84, options));
  ASSERT_THAT(ct, NotNull());

  double x = 500000.0;
  double y = 5000000.0;
  EXPECT_TRUE(ct->Transform(1, &x, &y));
  // Since longitude was ~3 deg, wrapping around 180 deg keeps it in [0, 360]
  EXPECT_GE(x, 0.0);
  EXPECT_LE(x, 360.0);

  // Now with EPSG order (m_eTargetFirstAxisOrient == OAO_North)
  srsWGS84.SetAxisMappingStrategy(OAMS_AUTHORITY_COMPLIANT);
  auto ctNorth = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsUTM, &srsWGS84, options));
  ASSERT_THAT(ctNorth, NotNull());
  x = 500000.0;
  y = 5000000.0;
  EXPECT_TRUE(ctNorth->Transform(1, &x, &y));
}

TEST(OgrCtTest, CandidateOperationSelectionAndRegionalBBoxes) {
  OGRSpatialReference srsNAD27;
  srsNAD27.importFromEPSG(4267);
  srsNAD27.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srsWGS84;
  srsWGS84.importFromEPSG(4326);
  srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // Strategy::FIRST_MATCHING
  {
    CPLSetConfigOption("OGR_CT_OP_SELECTION", "FIRST_MATCHING");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsNAD27, &srsWGS84));
    ASSERT_THAT(ct, NotNull());

    double x[3] = {-100.0, -117.0, -75.0};
    double y[3] = {40.0, 34.0, 45.0};
    int bSuccess[3] = {0, 0, 0};
    EXPECT_TRUE(ct->Transform(3, x, y, nullptr, nullptr, bSuccess));
    EXPECT_TRUE(bSuccess[0]);
    EXPECT_TRUE(bSuccess[1]);
    EXPECT_TRUE(bSuccess[2]);
  }

  // Strategy::BEST_ACCURACY
  {
    CPLSetConfigOption("OGR_CT_OP_SELECTION", "BEST_ACCURACY");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });

    OGRCoordinateTransformationOptions options;
    options.SetBallparkAllowed(true);
    options.SetOnlyBest(false);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsNAD27, &srsWGS84, options));
    ASSERT_THAT(ct, NotNull());

    double x[2] = {-100.0, -120.0};
    double y[2] = {40.0, 38.0};
    int bSuccess[2] = {0, 0};
    EXPECT_TRUE(ct->Transform(2, x, y, nullptr, nullptr, bSuccess));
    EXPECT_TRUE(bSuccess[0]);
    EXPECT_TRUE(bSuccess[1]);
  }
}

TEST(OgrCtTest, CoordinateEpochsAndDynamicCRS) {
  OGRSpatialReference srsITRF2014;
  srsITRF2014.importFromEPSG(9000);  // ITRF2014
  srsITRF2014.SetCoordinateEpoch(2020.0);

  OGRSpatialReference srsWGS84;
  srsWGS84.importFromEPSG(4326);
  srsWGS84.SetCoordinateEpoch(2010.0);

  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsITRF2014, &srsWGS84));
  if (ct) {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double t = 2020.0;
    int bSuccess = FALSE;
    ct->Transform(1, &x, &y, &z, &t, &bSuccess);
  }
}

TEST(OgrCtTest, NoTransformIdentity) {
  OGRSpatialReference srs1;
  srs1.importFromEPSG(4326);
  srs1.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srs2;
  srs2.importFromEPSG(4326);
  srs2.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srs1, &srs2));
  ASSERT_THAT(ct, NotNull());

  double x = 12.34;
  double y = 56.78;
  double z = 100.0;
  double t = 2020.0;
  int errCode = -1;
  EXPECT_TRUE(ct->TransformWithErrorCodes(1, &x, &y, &z, &t, &errCode));
  EXPECT_EQ(0, errCode);
  EXPECT_DOUBLE_EQ(12.34, x);
  EXPECT_DOUBLE_EQ(56.78, y);
  EXPECT_DOUBLE_EQ(100.0, z);
  EXPECT_DOUBLE_EQ(2020.0, t);
}

TEST(OgrCtTest, TransformErrorsAndOutOfBounds) {
  WithQuietHandler quiet;

  OGRSpatialReference srsWGS84;
  srsWGS84.SetWellKnownGeogCS("WGS84");
  srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  OGRSpatialReference srs3857;
  srs3857.importFromEPSG(3857);
  srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  auto ct = std::unique_ptr<OGRCoordinateTransformation>(
      OGRCreateCoordinateTransformation(&srsWGS84, &srs3857));
  ASSERT_THAT(ct, NotNull());

  // Latitude beyond WebMercator valid range (e.g. 89.9 deg or 95 deg)
  double x = 0.0;
  double y = 95.0;
  int errCode = 0;
  ct->TransformWithErrorCodes(1, &x, &y, nullptr, nullptr, &errCode);
  EXPECT_NE(0, errCode);

  int bSuccess = TRUE;
  x = 0.0;
  y = 95.0;
  ct->Transform(1, &x, &y, nullptr, nullptr, &bSuccess);
  EXPECT_FALSE(bSuccess);
}

TEST(OgrCtTest, MoreTransformBoundsAndFeatures) {
  WithQuietHandler quiet;

  // 1. TransformBounds with bNoTransform
  {
    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsWGS84));
    ASSERT_THAT(ct, NotNull());
    double outXMin = 0;
    double outYMin = 0;
    double outXMax = 0;
    double outYMax = 0;
    EXPECT_TRUE(ct->TransformBounds(10.0, 20.0, 30.0, 40.0, &outXMin, &outYMin,
                                    &outXMax, &outYMax, 0));
    EXPECT_DOUBLE_EQ(10.0, outXMin);
    EXPECT_DOUBLE_EQ(20.0, outYMin);
    EXPECT_DOUBLE_EQ(30.0, outXMax);
    EXPECT_DOUBLE_EQ(40.0, outYMax);
  }

  // 2. TransformBounds crossing antimeridian (antimeridian_min/max)
  {
    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srs3857, &srsWGS84));
    ASSERT_THAT(ct, NotNull());

    // Project bounds that span antimeridian: near 170 deg lon to -170 deg lon
    double outXMin = 0;
    double outYMin = 0;
    double outXMax = 0;
    double outYMax = 0;
    ct->TransformBounds(18000000.0, -1000000.0, 20000000.0, 1000000.0, &outXMin,
                        &outYMin, &outXMax, &outYMax, 21);
  }

  // 3. TryAtPlusMinus180 in TransformBounds: WGS84 wide bounds to UTM (dfLon0
  // != 0)
  {
    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);  // UTM 31N, lon_0 = 3
    srsUTM.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
    ASSERT_THAT(ct, NotNull());

    double outXMin = 0;
    double outYMin = 0;
    double outXMax = 0;
    double outYMax = 0;
    ct->TransformBounds(-180.0, -60.0, 180.0, 60.0, &outXMin, &outYMin,
                        &outXMax, &outYMax, 21);
  }

  // 4. Source Longitude Wrapping (bSourceWrap)
  {
    OGRCoordinateTransformationOptions options;
    options.SetSourceCenterLong(180.0);

    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);
    srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);
    srsUTM.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM, options));
    ASSERT_THAT(ct, NotNull());

    double x = 3.0;
    double y = 50.0;
    EXPECT_TRUE(ct->Transform(1, &x, &y));

    // And with OAO_North (EPSG compliant lat/long)
    srsWGS84.SetAxisMappingStrategy(OAMS_AUTHORITY_COMPLIANT);
    auto ctNorth = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM, options));
    ASSERT_THAT(ctNorth, NotNull());
    x = 50.0;
    y = 3.0;
    EXPECT_TRUE(ctNorth->Transform(1, &x, &y));
  }

  // 5. Non-finite coordinates in TransformWithErrorCodes (NaN / Inf)
  {
    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srs3857));
    ASSERT_THAT(ct, NotNull());

    double x = std::numeric_limits<double>::quiet_NaN();
    double y = 0.0;
    int errCode = 0;
    EXPECT_FALSE(
        ct->TransformWithErrorCodes(1, &x, &y, nullptr, nullptr, &errCode));
    EXPECT_NE(0, errCode);
  }

  // 6. Proj4 WebMercator with towgs84 in destination
  {
    OGRSpatialReference srsMerc;
    srsMerc.importFromProj4(
        "+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0 +lon_0=0.0 +x_0=0.0 "
        "+y_0=0 +k=1.0 +units=m +nadgrids=@null +wktext +no_defs");
    OGRSpatialReference srsWgs;
    srsWgs.importFromProj4(
        "+proj=longlat +ellps=WGS84 +towgs84=0,0,0,0,0,0,0 +no_defs");

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsMerc, &srsWgs));
    ASSERT_THAT(ct, NotNull());
  }

  // 7. CENTER_LONG config option
  {
    CPLSetConfigOption("CENTER_LONG", "180.0");
    auto cleanup =
        absl::MakeCleanup([] { CPLSetConfigOption("CENTER_LONG", nullptr); });

    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
    ASSERT_THAT(ct, NotNull());
  }

  // 8. OCTNewCoordinateTransformationEx C API
  {
    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);

    OGRCoordinateTransformationOptionsH hOptions =
        OCTNewCoordinateTransformationOptions();
    OCTCoordinateTransformationOptionsSetAreaOfInterest(hOptions, -10.0, 40.0,
                                                        10.0, 50.0);

    OGRCoordinateTransformationH hCT = OCTNewCoordinateTransformationEx(
        OGRSpatialReference::ToHandle(&srsWGS84),
        OGRSpatialReference::ToHandle(&srs3857), hOptions);
    ASSERT_THAT(hCT, NotNull());

    OCTDestroyCoordinateTransformation(hCT);
    OCTDestroyCoordinateTransformationOptions(hOptions);
  }

  // 9. Area of interest with Strategy::PROJ
  {
    OGRCoordinateTransformationOptions options;
    options.SetAreaOfInterest(-10.0, 40.0, 10.0, 50.0);

    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM, options));
    ASSERT_THAT(ct, NotNull());
  }

  // 10. Area of interest with Strategy::BEST_ACCURACY
  {
    CPLSetConfigOption("OGR_CT_OP_SELECTION", "BEST_ACCURACY");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });

    OGRCoordinateTransformationOptions options;
    options.SetAreaOfInterest(-100.0, 30.0, -90.0, 40.0);

    OGRSpatialReference srsNAD27;
    srsNAD27.importFromEPSG(4267);
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsNAD27, &srsWGS84, options));
    ASSERT_THAT(ct, NotNull());
  }

  // 11. Regional datum transformation without world transformation (OSGB36 to
  // WGS84)
  {
    CPLSetConfigOption("OGR_CT_OP_SELECTION", "BEST_ACCURACY");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });

    OGRSpatialReference srsOSGB;
    srsOSGB.importFromEPSG(4277);
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsOSGB, &srsWGS84));
    ASSERT_THAT(ct, NotNull());

    double x = -1.0;
    double y = 52.0;
    EXPECT_TRUE(ct->Transform(1, &x, &y));
  }

  // 12. CHECK_WITH_INVERT_PROJ with out-of-domain point
  {
    CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", "YES");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", nullptr); });

    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference srsMerc;
    srsMerc.importFromEPSG(3857);
    srsMerc.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsMerc));
    ASSERT_THAT(ct, NotNull());

    double x = 0.0;
    double y = 89.9;
    int errCode = 0;
    ct->TransformWithErrorCodes(1, &x, &y, nullptr, nullptr, &errCode);
  }
}

namespace {
class CustomCT : public OGRCoordinateTransformation {
 public:
  const OGRSpatialReference* GetSourceCS() const override { return nullptr; }
  const OGRSpatialReference* GetTargetCS() const override { return nullptr; }
  OGRCoordinateTransformation* Clone() const override { return nullptr; }
  OGRCoordinateTransformation* GetInverse() const override { return nullptr; }
  int Transform(size_t, double*, double*, double*, double*, int*) override {
    return FALSE;
  }
};
}  // namespace

TEST(OgrCtTest, ExtensiveCoverage) {
  WithQuietHandler quiet;

  // 1. SetAreaOfInterest invalid branches
  {
    OGRCoordinateTransformationOptions options;
    EXPECT_FALSE(options.SetAreaOfInterest(200.0, 0.0, 10.0, 20.0));
    EXPECT_FALSE(options.SetAreaOfInterest(0.0, -100.0, 10.0, 20.0));
    EXPECT_FALSE(options.SetAreaOfInterest(0.0, 0.0, 200.0, 20.0));
    EXPECT_FALSE(options.SetAreaOfInterest(0.0, 0.0, 10.0, 100.0));
    EXPECT_FALSE(options.SetAreaOfInterest(0.0, 50.0, 10.0, 40.0));
  }

  // 2. DestroyCT with non-OGRProjCT
  {
    OGRCoordinateTransformation::DestroyCT(new CustomCT());
  }

  // 3. Emit errors getter & setter
  {
    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srs3857));
    ASSERT_THAT(ct, NotNull());

    EXPECT_TRUE(ct->GetEmitErrors());
    ct->SetEmitErrors(false);
    EXPECT_FALSE(ct->GetEmitErrors());
    ct->SetEmitErrors(true);
    EXPECT_TRUE(ct->GetEmitErrors());

    // Generate 25 errors to hit nErrorCount == 20 suppression
    for (int i = 0; i < 25; ++i) {
      double x = 0.0;
      double y = 95.0;
      ct->Transform(1, &x, &y);
    }
    // Now with m_bEmitErrors = false
    ct->SetEmitErrors(false);
    for (int i = 0; i < 25; ++i) {
      double x = 0.0;
      double y = 95.0;
      ct->Transform(1, &x, &y);
    }
  }

  // 4. Base class TransformWithErrorCodes for count 1 and count 2
  {
    CustomCT customCt;
    double x[2] = {0, 0};
    double y[2] = {0, 0};
    double z[2] = {0, 0};
    double t[2] = {0, 0};
    int errCodes[2] = {0, 0};
    EXPECT_FALSE(customCt.TransformWithErrorCodes(1, x, y, z, t, errCodes));
    EXPECT_EQ(-1, errCodes[0]);
    EXPECT_FALSE(customCt.TransformWithErrorCodes(2, x, y, z, t, errCodes));
    EXPECT_EQ(-1, errCodes[0]);
    EXPECT_EQ(-1, errCodes[1]);
  }

  // 5. Negative axis mappings in source and target
  {
    OGRSpatialReference srsSrc;
    srsSrc.importFromEPSG(4326);
    srsSrc.SetDataAxisToSRSAxisMapping({-1, -2, -3});

    OGRSpatialReference srsDst;
    srsDst.importFromEPSG(3857);
    srsDst.SetDataAxisToSRSAxisMapping({-2, -1, -3});

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsSrc, &srsDst));
    ASSERT_THAT(ct, NotNull());

    double x = 10.0;
    double y = 20.0;
    double z = 30.0;
    EXPECT_TRUE(ct->Transform(1, &x, &y, &z));
  }

  // 6. Source and target longitude wrapping out-of-range (< -180 and > 180)
  {
    OGRCoordinateTransformationOptions options;
    options.SetSourceCenterLong(180.0);
    options.SetTargetCenterLong(180.0);

    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);
    srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsWGS84, options));
    ASSERT_THAT(ct, NotNull());

    double x[2] = {-10.0, 370.0};
    double y[2] = {10.0, 20.0};
    EXPECT_TRUE(ct->Transform(2, x, y));

    // And with OAO_North
    srsWGS84.SetAxisMappingStrategy(OAMS_AUTHORITY_COMPLIANT);
    auto ctNorth = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsWGS84, options));
    ASSERT_THAT(ctNorth, NotNull());
    double xNorth[2] = {10.0, 20.0};
    double yNorth[2] = {-10.0, 370.0};
    EXPECT_TRUE(ctNorth->Transform(2, xNorth, yNorth));
  }

  // 7. WebMercator to WGS84 optimization wrapping and invert proj check
  {
    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference srs4326;
    srs4326.importFromEPSG(4326);
    srs4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    // With CHECK_WITH_INVERT_PROJ = YES
    {
      CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", "YES");
      auto cleanup = absl::MakeCleanup(
          [] { CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", nullptr); });

      auto ct = std::unique_ptr<OGRCoordinateTransformation>(
          OGRCreateCoordinateTransformation(&srs3857, &srs4326));
      ASSERT_THAT(ct, NotNull());

      double x[4] = {30000000.0, -30000000.0, 20037508.342789244,
                     -20037508.342789244};
      double y[4] = {0, 0, 0, 0};
      int bSuccess[4] = {1, 1, 1, 1};
      ct->Transform(4, x, y, nullptr, nullptr, bSuccess);
    }

    // With CHECK_WITH_INVERT_PROJ = NO
    {
      auto ct = std::unique_ptr<OGRCoordinateTransformation>(
          OGRCreateCoordinateTransformation(&srs3857, &srs4326));
      ASSERT_THAT(ct, NotNull());

      double x[2] = {30000000.0, -30000000.0};
      double y[2] = {0, 0};
      ct->Transform(2, x, y);
    }
  }

  // 8. TransformBounds with Arctic Polar Stereographic (EPSG:3995)
  {
    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference srsPolar;
    srsPolar.importFromEPSG(3995);  // Arctic Polar Stereographic
    srsPolar.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsPolar));
    ASSERT_THAT(ct, NotNull());

    double outXMin = 0;
    double outYMin = 0;
    double outXMax = 0;
    double outYMax = 0;
    ct->TransformBounds(-180.0, 60.0, 180.0, 90.0, &outXMin, &outYMin, &outXMax,
                        &outYMax, 21);
  }

  // 9. Legacy config options OSR_USE_ETMERC and OSR_USE_APPROX_TMERC
  {
    CPLSetConfigOption("OSR_USE_ETMERC", "NO");
    CPLSetConfigOption("OSR_USE_APPROX_TMERC", "YES");
    auto cleanup = absl::MakeCleanup([] {
      CPLSetConfigOption("OSR_USE_ETMERC", nullptr);
      CPLSetConfigOption("OSR_USE_APPROX_TMERC", nullptr);
    });

    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
    ASSERT_THAT(ct, NotNull());
  }

  // 10. OSR_CT_USE_DEFAULT_EPSG_TOWGS84
  {
    CPLSetConfigOption("OSR_CT_USE_DEFAULT_EPSG_TOWGS84", "YES");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OSR_CT_USE_DEFAULT_EPSG_TOWGS84", nullptr); });

    OGRSpatialReference srsDHDN;
    srsDHDN.importFromEPSG(31468);
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsDHDN, &srsWGS84));
    ASSERT_THAT(ct, NotNull());
  }

  // 11. Strategy::PROJ options
  {
    OGRCoordinateTransformationOptions options;
    options.SetBallparkAllowed(false);
    options.SetOnlyBest(true);
    options.SetDesiredAccuracy(0.1);

    OGRSpatialReference srsWGS84;
    srsWGS84.SetWellKnownGeogCS("WGS84");
    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM, options));
    ASSERT_THAT(ct, NotNull());
  }

  // 12. LocalCS source in BEST_ACCURACY (no geodetic CRS)
  {
    CPLSetConfigOption("OGR_CT_OP_SELECTION", "BEST_ACCURACY");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });

    OGRSpatialReference srsLocal;
    srsLocal.SetLocalCS("MyLocalCS");
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);

    EXPECT_EQ(nullptr, OGRCreateCoordinateTransformation(&srsLocal, &srsWGS84));
  }

  // 13. Cache hit with identical axis mapping strategy
  {
    OSRCTCleanCache();
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);
    srsWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    srs3857.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srs3857));
    ASSERT_THAT(ct, NotNull());
    OGRCoordinateTransformation::DestroyCT(ct.release());

    auto ct2 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srs3857));
    ASSERT_THAT(ct2, NotNull());
  }

  // 14. OGR_CT_FORCE_APPROX_TMERC config option and null transform
  {
    CPLSetConfigOption("OGR_CT_FORCE_APPROX_TMERC", "YES");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_FORCE_APPROX_TMERC", nullptr); });

    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);
    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
    ASSERT_THAT(ct, NotNull());
  }
  {
    CPLSetConfigOption("OGR_CT_FORCE_APPROX_TMERC", "NO");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_FORCE_APPROX_TMERC", nullptr); });

    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);
    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
    ASSERT_THAT(ct, NotNull());
  }

  // 15. CENTER_LONG extension on GEOGCS
  {
    OGRSpatialReference srsSrc;
    srsSrc.SetWellKnownGeogCS("CRS84");
    srsSrc.SetExtension("GEOGCS", "CENTER_LONG", "120.0");

    OGRSpatialReference srsDst;
    srsDst.SetWellKnownGeogCS("CRS84");
    srsDst.SetExtension("GEOGCS", "CENTER_LONG", "-60.0");

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsSrc, &srsDst));
    ASSERT_THAT(ct, NotNull());

    double x = 10.0;
    double y = 20.0;
    EXPECT_TRUE(ct->Transform(1, &x, &y));
  }

  // 16. Longitude wrapping on source and target with both OAO_East and
  // OAO_North
  {
    // Source wrapping with CRS84 (OAO_East): x is lon
    OGRCoordinateTransformationOptions options;
    options.SetSourceCenterLong(180.0);
    options.SetTargetCenterLong(0.0);

    OGRSpatialReference srsEast;
    srsEast.SetWellKnownGeogCS("CRS84");
    auto ctEast = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsEast, &srsEast, options));
    ASSERT_THAT(ctEast, NotNull());

    double x[2] = {-10.0, 370.0};
    double y[2] = {0.0, 0.0};
    EXPECT_TRUE(ctEast->Transform(2, x, y));
  }
  {
    // Source and target wrapping with EPSG:4326 (OAO_North): y is lon
    OGRCoordinateTransformationOptions options;
    options.SetSourceCenterLong(180.0);
    options.SetTargetCenterLong(180.0);

    OGRSpatialReference srsNorth;
    srsNorth.importFromEPSG(4326);
    auto ctNorth = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsNorth, &srsNorth, options));
    ASSERT_THAT(ctNorth, NotNull());

    double x[2] = {0.0, 0.0};
    double y[2] = {-10.0, 370.0};
    EXPECT_TRUE(ctNorth->Transform(2, x, y));
  }

  // 17. Dynamic CRS with coordinate epochs
  {
    CPLSetConfigOption("OGR_CT_USE_SRS_COORDINATE_EPOCH", "YES");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_USE_SRS_COORDINATE_EPOCH", nullptr); });

    OGRSpatialReference srsDynamic;
    srsDynamic.importFromEPSG(9000);  // ITRF2014
    srsDynamic.SetCoordinateEpoch(2020.0);

    OGRSpatialReference srsStatic;
    srsStatic.importFromEPSG(4326);

    // Dynamic to static
    auto ct1 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsDynamic, &srsStatic));
    if (ct1) {
      double x = 45.0, y = 10.0, z = 0.0;
      ct1->Transform(1, &x, &y, &z);
    }

    // Static to dynamic
    auto ct2 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsStatic, &srsDynamic));
    if (ct2) {
      double x = 45.0, y = 10.0, z = 0.0;
      ct2->Transform(1, &x, &y, &z);
    }
  }

  // 18. WebMercator to WGS84 optimization edge cases
  {
    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);

    // Test with CHECK_WITH_INVERT_PROJ = YES
    {
      CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", "YES");
      auto cleanup = absl::MakeCleanup(
          [] { CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", nullptr); });
      OGRCoordinateTransformationOptions opt;
      auto ct = std::unique_ptr<OGRCoordinateTransformation>(
          OGRCreateCoordinateTransformation(&srs3857, &srsWGS84, opt));
      ASSERT_THAT(ct, NotNull());

      double x[5] = {HUGE_VAL, 25000000.0, -25000000.0, 1000000.0, 1000000.0};
      double y[5] = {HUGE_VAL, 1000000.0, 1000000.0, 1000000.0, 1000000.0};
      int errorCodes[5] = {0};
      ct->TransformWithErrorCodes(5, x, y, nullptr, nullptr, errorCodes);
    }

    // Test with CHECK_WITH_INVERT_PROJ = NO
    {
      CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", "NO");
      auto cleanup = absl::MakeCleanup(
          [] { CPLSetConfigOption("CHECK_WITH_INVERT_PROJ", nullptr); });
      OGRCoordinateTransformationOptions opt;
      auto ct = std::unique_ptr<OGRCoordinateTransformation>(
          OGRCreateCoordinateTransformation(&srs3857, &srsWGS84, opt));
      ASSERT_THAT(ct, NotNull());

      double x[5] = {HUGE_VAL, 25000000.0, -25000000.0, 1000000.0, 1000000.0};
      double y[5] = {HUGE_VAL, 1000000.0, 1000000.0, 1000000.0, 1000000.0};
      int errorCodes[5] = {0};
      ct->TransformWithErrorCodes(5, x, y, nullptr, nullptr, errorCodes);
    }

    // Swapped axes for WebMercator source (OAO_North first)
    {
      OGRSpatialReference srs3857Swapped;
      srs3857Swapped.importFromEPSG(3857);
      srs3857Swapped.SetDataAxisToSRSAxisMapping({2, 1});
      auto ct = std::unique_ptr<OGRCoordinateTransformation>(
          OGRCreateCoordinateTransformation(&srs3857Swapped, &srsWGS84));
      if (ct) {
        double x = 1000000.0, y = 1000000.0;
        ct->Transform(1, &x, &y);
      }
    }
  }

  // 19. TransformWithErrorCodes with non-finite coordinates & error limit
  // suppression
  {
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);
    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
    ASSERT_THAT(ct, NotNull());

    // Non-finite coordinate
    double x = std::numeric_limits<double>::quiet_NaN();
    double y = 10.0;
    int err = 0;
    EXPECT_FALSE(
        ct->TransformWithErrorCodes(1, &x, &y, nullptr, nullptr, &err));

    // Exceed error limit (> 20 errors)
    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    std::vector<double> xs(25, 9999999.0);
    std::vector<double> ys(25, 9999999.0);
    std::vector<int> errs(25, 0);
    ct->TransformWithErrorCodes(25, xs.data(), ys.data(), nullptr, nullptr,
                                errs.data());
  }

  // 20. TransformBounds antimeridian and polar handling
  {
    OGRSpatialReference srsCRS84;
    srsCRS84.SetWellKnownGeogCS("CRS84");

    OGRSpatialReference srsEPSG4326;
    srsEPSG4326.importFromEPSG(4326);

    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);

    // degree_input && xmax < xmin when input_lon_lat_order is true (CRS84 to
    // 3857)
    auto ctCRS84To3857 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsCRS84, &srs3857));
    ASSERT_THAT(ctCRS84To3857, NotNull());
    double outXmin, outYmin, outXmax, outYmax;
    EXPECT_TRUE(ctCRS84To3857->TransformBounds(175.0, -10.0, -175.0, 10.0,
                                               &outXmin, &outYmin, &outXmax,
                                               &outYmax, 20));

    // densify_pts invalid
    {
      CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
      EXPECT_FALSE(ctCRS84To3857->TransformBounds(
          0, 0, 10, 10, &outXmin, &outYmin, &outXmax, &outYmax, -1));
      EXPECT_FALSE(ctCRS84To3857->TransformBounds(
          0, 0, 10, 10, &outXmin, &outYmin, &outXmax, &outYmax, 20000));
    }

    // degree_input && !input_lon_lat_order && xmax < xmin (EPSG:4326 latitude
    // max < min error)
    auto ct4326To3857 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsEPSG4326, &srs3857));
    ASSERT_THAT(ct4326To3857, NotNull());
    {
      CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
      EXPECT_FALSE(ct4326To3857->TransformBounds(
          10.0, 0.0, -10.0, 10.0, &outXmin, &outYmin, &outXmax, &outYmax, 20));
      // and ymax < ymin when !input_lon_lat_order (crossing antimeridian in
      // EPSG:4326)
      EXPECT_TRUE(ct4326To3857->TransformBounds(0.0, 175.0, 10.0, -175.0,
                                                &outXmin, &outYmin, &outXmax,
                                                &outYmax, 20));
    }

    // latitude max < min when input_lon_lat_order is true (CRS84 y is lat)
    {
      CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
      EXPECT_FALSE(ctCRS84To3857->TransformBounds(
          0.0, 10.0, 10.0, -10.0, &outXmin, &outYmin, &outXmax, &outYmax, 20));
    }

    // ContainsNorthPole and ContainsSouthPole
    // Source: Polar Stereographic North (EPSG:3995) to CRS84 and EPSG:4326
    OGRSpatialReference srsNorthPole;
    srsNorthPole.importFromEPSG(3995);
    auto ctNorthCRS84 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsNorthPole, &srsCRS84));
    if (ctNorthCRS84) {
      ctNorthCRS84->TransformBounds(-100000.0, -100000.0, 100000.0, 100000.0,
                                    &outXmin, &outYmin, &outXmax, &outYmax, 20);
    }
    auto ctNorth4326 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsNorthPole, &srsEPSG4326));
    if (ctNorth4326) {
      ctNorth4326->TransformBounds(-100000.0, -100000.0, 100000.0, 100000.0,
                                   &outXmin, &outYmin, &outXmax, &outYmax, 20);
    }

    // Source: Polar Stereographic South (EPSG:3031) to CRS84 and EPSG:4326
    OGRSpatialReference srsSouthPole;
    srsSouthPole.importFromEPSG(3031);
    auto ctSouthCRS84 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsSouthPole, &srsCRS84));
    if (ctSouthCRS84) {
      ctSouthCRS84->TransformBounds(-100000.0, -100000.0, 100000.0, 100000.0,
                                    &outXmin, &outYmin, &outXmax, &outYmax, 20);
    }
    auto ctSouth4326 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsSouthPole, &srsEPSG4326));
    if (ctSouth4326) {
      ctSouth4326->TransformBounds(-100000.0, -100000.0, 100000.0, 100000.0,
                                   &outXmin, &outYmin, &outXmax, &outYmax, 20);
    }
  }

  // 21. BEST_ACCURACY strategy and regional operations
  {
    CPLSetConfigOption("OGR_CT_OP_SELECTION", "BEST_ACCURACY");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });

    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    OGRCoordinateTransformationOptions opt;

    // With desired accuracy and ballpark false
    opt.SetDesiredAccuracy(0.0000001);
    opt.SetBallparkAllowed(false);

    OGRSpatialReference srs4267, srs4326;
    srs4267.importFromEPSG(4267);
    srs4326.importFromEPSG(4326);
    OGRCreateCoordinateTransformation(&srs4267, &srs4326, opt);

    // Regional CRS to trigger !foundWorldTransformation
    // EPSG:27572 (Lambert II étendu) to EPSG:2154 (Lambert 93)
    OGRSpatialReference srs27572, srs2154;
    srs27572.importFromEPSG(27572);
    srs2154.importFromEPSG(2154);
    OGRCoordinateTransformationOptions optRegional;
    auto ctRegional = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srs27572, &srs2154, optRegional));
    EXPECT_THAT(ctRegional, NotNull());
  }

  // 22. TransformBounds with degree_output crossing antimeridian
  {
    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    OGRSpatialReference srsCRS84;
    srsCRS84.SetWellKnownGeogCS("CRS84");
    OGRSpatialReference srsEPSG4326;
    srsEPSG4326.importFromEPSG(4326);

    // Target CRS84 (output_lon_lat_order is true)
    auto ct3857ToCRS84 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srs3857, &srsCRS84));
    ASSERT_THAT(ct3857ToCRS84, NotNull());

    double xmin = 18924313.0;
    double xmax = 21150702.0;  // 190 degrees
    double ymin = -1000000.0;
    double ymax = 1000000.0;
    double outXmin, outYmin, outXmax, outYmax;
    ct3857ToCRS84->TransformBounds(xmin, ymin, xmax, ymax, &outXmin, &outYmin,
                                   &outXmax, &outYmax, 20);

    // Target EPSG:4326 (output_lon_lat_order is false)
    auto ct3857To4326 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srs3857, &srsEPSG4326));
    ASSERT_THAT(ct3857To4326, NotNull());
    ct3857To4326->TransformBounds(xmin, ymin, xmax, ymax, &outXmin, &outYmin,
                                  &outXmax, &outYmax, 20);

    // Geographic to geographic crossing antimeridian:
    OGRSpatialReference srsWGS84_GIS;
    srsWGS84_GIS.importFromEPSG(4326);
    srsWGS84_GIS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto ctGeogToCRS84 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84_GIS, &srsCRS84));
    if (ctGeogToCRS84) {
      ctGeogToCRS84->TransformBounds(170.0, -10.0, -170.0, 10.0, &outXmin,
                                     &outYmin, &outXmax, &outYmax, 20);
    }
  }

  // 23. TransformBounds failure when all points fail
  {
    OGRSpatialReference srs4326;
    srs4326.importFromEPSG(4326);
    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);  // UTM zone 31N (lon 0 to 6)

    OGRCoordinateTransformationOptions opt;
    opt.SetBallparkAllowed(false);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srs4326, &srsUTM, opt));
    if (ct) {
      CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
      double outXmin, outYmin, outXmax, outYmax;
      // Points in Antarctica far outside UTM 31N domain
      ct->TransformBounds(-80.0, -85.0, -75.0, -80.0, &outXmin, &outYmin,
                          &outXmax, &outYmax, 5);
    }
  }

  // 24. Custom Proj4 strings for WebMercator to WGS84 detection
  {
    OGRSpatialReference srsCustomMerc;
    srsCustomMerc.importFromProj4(
        "+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0  +lon_0=0.0 "
        "+x_0=0.0 +y_0=0 +k=1.0 +units=m +nadgrids=@null  +wktext +no_defs ");
    srsCustomMerc.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference srsCustomWGS84;
    srsCustomWGS84.importFromProj4("+proj=longlat  +datum=WGS84  +no_defs ");
    srsCustomWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsCustomMerc, &srsCustomWGS84));
    if (ct) {
      double x = 1000.0, y = 1000.0;
      ct->Transform(1, &x, &y);
    }

    // Another variant with +towgs84 in dest
    OGRSpatialReference srsCustomWGS84Towgs84;
    srsCustomWGS84Towgs84.importFromProj4(
        "+proj=longlat +ellps=WGS84 +towgs84=0,0,0,0,0,0,0  +no_defs ");
    srsCustomWGS84Towgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto ct2 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsCustomMerc,
                                          &srsCustomWGS84Towgs84));
    if (ct2) {
      double x = 1000.0, y = 1000.0;
      ct2->Transform(1, &x, &y);
    }

    // EPSG:3785 and EPSG:900913
    OGRSpatialReference srs3785, srs900913, srs4326_gis;
    srs3785.importFromEPSG(3785);
    srs900913.importFromEPSG(900913);
    srs4326_gis.importFromEPSG(4326);
    srs4326_gis.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto ct3785 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srs3785, &srs4326_gis));
    auto ct900913 = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srs900913, &srs4326_gis));
  }

  // 25. OGR_CT_OP_SELECTION and OSR_USE_APPROX_TMERC options
  {
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);
    OGRSpatialReference srsUTM;
    srsUTM.importFromEPSG(32631);

    // Strategy::PROJ
    {
      CPLSetConfigOption("OGR_CT_OP_SELECTION", "PROJ");
      auto cleanup = absl::MakeCleanup(
          [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });
      auto ct = std::unique_ptr<OGRCoordinateTransformation>(
          OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
      EXPECT_THAT(ct, NotNull());
    }

    // Invalid strategy warning
    {
      CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
      CPLSetConfigOption("OGR_CT_OP_SELECTION", "INVALID_STRATEGY");
      auto cleanup = absl::MakeCleanup(
          [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });
      auto ct = std::unique_ptr<OGRCoordinateTransformation>(
          OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
      EXPECT_THAT(ct, NotNull());
    }

    // OSR_USE_APPROX_TMERC = YES
    {
      CPLSetConfigOption("OSR_USE_APPROX_TMERC", "YES");
      auto cleanup = absl::MakeCleanup(
          [] { CPLSetConfigOption("OSR_USE_APPROX_TMERC", nullptr); });
      auto ct = std::unique_ptr<OGRCoordinateTransformation>(
          OGRCreateCoordinateTransformation(&srsWGS84, &srsUTM));
      EXPECT_THAT(ct, NotNull());
    }

    // Null transform with BEST_ACCURACY (proj_string[0] == '\0' -> proj=affine)
    {
      CPLSetConfigOption("OGR_CT_OP_SELECTION", "BEST_ACCURACY");
      auto cleanup = absl::MakeCleanup(
          [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });
      auto ct = std::unique_ptr<OGRCoordinateTransformation>(
          OGRCreateCoordinateTransformation(&srsWGS84, &srsWGS84));
      EXPECT_THAT(ct, NotNull());
    }
  }

  // 26. Vertical CRS in BEST_ACCURACY
  {
    CPLSetConfigOption("OGR_CT_OP_SELECTION", "BEST_ACCURACY");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });

    OGRSpatialReference srsVert;
    srsVert.importFromEPSG(5703);  // NAVD88 height (Vertical CRS)
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);

    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsVert, &srsWGS84));
  }

  // 27. Invalid CRS string in Initialize
  {
    OGRSpatialReference srsInvalid;
    srsInvalid.SetFromUserInput("+proj=invalid_projection_name_xyz");
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);

    CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(nullptr,
              OGRCreateCoordinateTransformation(&srsInvalid, &srsWGS84));
    EXPECT_EQ(nullptr,
              OGRCreateCoordinateTransformation(&srsWGS84, &srsInvalid));

    // Also test with BEST_ACCURACY to hit ListCoordinateOperations invalid
    // source/target
    CPLSetConfigOption("OGR_CT_OP_SELECTION", "BEST_ACCURACY");
    auto cleanup = absl::MakeCleanup(
        [] { CPLSetConfigOption("OGR_CT_OP_SELECTION", nullptr); });
    EXPECT_EQ(nullptr,
              OGRCreateCoordinateTransformation(&srsInvalid, &srsWGS84));
    EXPECT_EQ(nullptr,
              OGRCreateCoordinateTransformation(&srsWGS84, &srsInvalid));
  }

  // 28. TransformBounds missing source/target SRS and densify_pts < 2 for
  // degree output
  {
    OGRCoordinateTransformationOptions opt;
    opt.SetCoordinateOperation(
        "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad", false);

    // Missing source SRS
    auto ctNoSrc = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(nullptr, nullptr, opt));
    if (ctNoSrc) {
      CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
      double outXmin, outYmin, outXmax, outYmax;
      EXPECT_FALSE(ctNoSrc->TransformBounds(0, 0, 10, 10, &outXmin, &outYmin,
                                            &outXmax, &outYmax, 5));
    }

    // Missing target SRS
    OGRSpatialReference srsWGS84;
    srsWGS84.importFromEPSG(4326);
    auto ctNoDst = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsWGS84, nullptr, opt));
    if (ctNoDst) {
      CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
      double outXmin, outYmin, outXmax, outYmax;
      EXPECT_FALSE(ctNoDst->TransformBounds(0, 0, 10, 10, &outXmin, &outYmin,
                                            &outXmax, &outYmax, 5));
    }

    // densify_pts < 2 when output is degree
    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    OGRSpatialReference srsCRS84;
    srsCRS84.SetWellKnownGeogCS("CRS84");
    auto ctDegOut = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srs3857, &srsCRS84));
    if (ctDegOut) {
      CPLErrorStateBackuper oBackuper(CPLQuietErrorHandler);
      double outXmin, outYmin, outXmax, outYmax;
      EXPECT_FALSE(ctDegOut->TransformBounds(0, 0, 1000, 1000, &outXmin,
                                             &outYmin, &outXmax, &outYmax, 1));
    }
  }

  // 29. Static CRS with CoordinateEpoch set (calls HasPointMotionOperation)
  {
    OGRSpatialReference srsStaticSrc;
    srsStaticSrc.importFromEPSG(4326);
    srsStaticSrc.SetCoordinateEpoch(2021.5);

    OGRSpatialReference srsStaticDst;
    srsStaticDst.importFromEPSG(4326);
    srsStaticDst.SetCoordinateEpoch(2022.0);

    auto ct = std::unique_ptr<OGRCoordinateTransformation>(
        OGRCreateCoordinateTransformation(&srsStaticSrc, &srsStaticDst));
    EXPECT_THAT(ct, NotNull());
  }
}

}  // namespace
}  // namespace autotest2
