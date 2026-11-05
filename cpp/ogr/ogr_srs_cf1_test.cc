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

#include "third_party/gdal/ogr/ogr_srs_cf1.h"

#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::DoubleNear;
using ::testing::Eq;
using ::testing::Ne;
using ::testing::NotNull;

class OgrSrsCf1Test : public ::testing::Test {
 protected:
  void TearDown() override { OSRCleanup(); }
};

TEST_F(OgrSrsCf1Test, CApiNullHandles) {
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE, OSRImportFromCF1(nullptr, nullptr, nullptr));
    EXPECT_EQ(OGRERR_FAILURE,
              OSRExportToCF1(nullptr, nullptr, nullptr, nullptr, nullptr));
  }
}

TEST_F(OgrSrsCf1Test, ImportWktPriority) {
  OGRSpatialReference srs;
  // spatial_ref key present
  CPLStringList kv;
  kv.AddNameValue(NCDF_SPATIAL_REF, SRS_WKT_WGS84_LAT_LONG);
  EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  EXPECT_TRUE(srs.IsGeographic());

  // crs_wkt key present
  CPLStringList kv2;
  kv2.AddNameValue(NCDF_CRS_WKT, SRS_WKT_WGS84_LAT_LONG);
  OGRSpatialReference srs2;
  EXPECT_EQ(OGRERR_NONE, srs2.importFromCF1(kv2.List(), nullptr));
  EXPECT_TRUE(srs2.IsGeographic());

  // Missing grid mapping name and no pole longitude
  CPLStringList kv3;
  kv3.AddNameValue("other_param", "123");
  OGRSpatialReference srs3;
  EXPECT_EQ(OGRERR_FAILURE, srs3.importFromCF1(kv3.List(), nullptr));

  // Missing grid mapping name but has grid_north_pole_longitude -> defaults to
  // rotated_pole
  CPLStringList kv4;
  kv4.AddNameValue(CF_PP_GRID_NORTH_POLE_LONGITUDE, "10.0");
  kv4.AddNameValue(CF_PP_GRID_NORTH_POLE_LATITUDE, "50.0");
  kv4.AddNameValue(CF_PP_NORTH_POLE_GRID_LONGITUDE, "0.0");
  OGRSpatialReference srs4;
  EXPECT_EQ(OGRERR_NONE, srs4.importFromCF1(kv4.List(), nullptr));
  EXPECT_TRUE(srs4.IsDerivedGeographic());
}

TEST_F(OgrSrsCf1Test, ImportDatumEllipsoidVariants) {
  // Sphere with earth_radius only
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LATITUDE_LONGITUDE);
    kv.AddNameValue(CF_PP_EARTH_RADIUS, "6371000.0");
    kv.AddNameValue(CF_PRIME_MERIDIAN_NAME, "Greenwich");
    kv.AddNameValue(CF_PP_LONG_PRIME_MERIDIAN, "0.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    EXPECT_DOUBLE_EQ(6371000.0, srs.GetSemiMajor());
    EXPECT_DOUBLE_EQ(0.0, srs.GetInvFlattening());
  }

  // Spheroid with semi_major_axis and semi_minor_axis
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LATITUDE_LONGITUDE);
    kv.AddNameValue(CF_PP_SEMI_MAJOR_AXIS, "6378137.0");
    kv.AddNameValue(CF_PP_SEMI_MINOR_AXIS, "6356752.314245179");
    kv.AddNameValue(CF_REFERENCE_ELLIPSOID_NAME, "WGS 84");
    kv.AddNameValue(CF_HORIZONTAL_DATUM_NAME, "WGS 1984");
    kv.AddNameValue(CF_GEOGRAPHIC_CRS_NAME, "WGS 84");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    EXPECT_NEAR(6378137.0, srs.GetSemiMajor(), 1e-4);
    EXPECT_NEAR(298.257223563, srs.GetInvFlattening(), 1e-4);
  }

  // Spheroid with earth_radius and semi_minor_axis (no semi_major)
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LATITUDE_LONGITUDE);
    kv.AddNameValue(CF_PP_EARTH_RADIUS, "6378137.0");
    kv.AddNameValue(CF_PP_SEMI_MINOR_AXIS, "6356752.314245179");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    EXPECT_NEAR(6378137.0, srs.GetSemiMajor(), 1e-4);
  }

  // Spheroid with semi_major and inverse_flattening
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LATITUDE_LONGITUDE);
    kv.AddNameValue(CF_PP_SEMI_MAJOR_AXIS, "6378137.0");
    kv.AddNameValue(CF_PP_INVERSE_FLATTENING, "298.257223563");
    kv.AddNameValue(CF_PP_LONG_PRIME_MERIDIAN,
                    "2.33722917");  // Paris PM, no name -> "unknown"
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    EXPECT_NEAR(298.257223563, srs.GetInvFlattening(), 1e-6);
  }

  // Legacy spherical_earth_radius_meters
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LATITUDE_LONGITUDE);
    kv.AddNameValue(CF_PP_EARTH_RADIUS_OLD, "6370000.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    EXPECT_DOUBLE_EQ(6370000.0, srs.GetSemiMajor());
  }
}

TEST_F(OgrSrsCf1Test, ImportStandardParallelsFormats) {
  // Brace array syntax: "{20.0,30.0}"
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_AEA);
    kv.AddNameValue(CF_PP_STD_PARALLEL, "{20.0,30.0}");
    kv.AddNameValue(CF_PP_LONG_CENTRAL_MERIDIAN, "10.0");
    kv.AddNameValue(CF_PP_LAT_PROJ_ORIGIN, "25.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    EXPECT_TRUE(srs.IsProjected());
  }

  // Comma and space separated syntax: "20.0, 30.0" and "20.0 30.0"
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_AEA);
    kv.AddNameValue(CF_PP_STD_PARALLEL, "20.0, 30.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    EXPECT_TRUE(srs.IsProjected());
  }
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_AEA);
    kv.AddNameValue(CF_PP_STD_PARALLEL, "20.0 30.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    EXPECT_TRUE(srs.IsProjected());
  }

  // Single standard parallel with warning: "25.0"
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_AEA);
    kv.AddNameValue(CF_PP_STD_PARALLEL, "25.0");
    OGRSpatialReference srs;
    {
      WithQuietHandler quiet;
      EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    }
  }

  // Empty string in NCDFTokenizeArray
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_AEA);
    kv.AddNameValue(CF_PP_STD_PARALLEL, "");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }

  // Fallback to standard_parallel_1 and standard_parallel_2
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_AEA);
    kv.AddNameValue(CF_PP_STD_PARALLEL_1, "20.0");
    kv.AddNameValue(CF_PP_STD_PARALLEL_2, "30.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    EXPECT_TRUE(srs.IsProjected());
  }
}

TEST_F(OgrSrsCf1Test, ImportProjectionsAndUnits) {
  // Transverse Mercator with units
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_TM);
    kv.AddNameValue(CF_PP_SCALE_FACTOR_MERIDIAN, "0.9996");
    kv.AddNameValue(CF_PP_LONG_CENTRAL_MERIDIAN, "-123.0");
    kv.AddNameValue(CF_PP_LAT_PROJ_ORIGIN, "0.0");
    kv.AddNameValue(CF_PP_FALSE_EASTING, "500000.0");
    kv.AddNameValue(CF_PP_FALSE_NORTHING, "0.0");
    kv.AddNameValue(CF_PROJECTED_CRS_NAME, "UTM Zone 10N");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), "metre"));
    EXPECT_TRUE(srs.IsProjected());
  }

  // Units tests: km, US_survey_foot, US_survey_feet, meter, unknown
  for (const char* unit :
       {"m", "meter", "km", "US_survey_foot", "US_survey_feet"}) {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_TM);
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), unit));
  }
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_TM);
    OGRSpatialReference srs;
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), "furlong"));
  }

  // Cylindrical Equal Area (CEA and LCEA)
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_CEA);
    kv.AddNameValue(CF_PP_STD_PARALLEL, "30.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }
  {
    // LCEA without standard_parallel -> error
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LCEA);
    OGRSpatialReference srs;
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }

  // LAEA with and without WGS84 datum
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LAEA);
    kv.AddNameValue(CF_HORIZONTAL_DATUM_NAME, "WGS_1984");
    kv.AddNameValue(CF_PP_EARTH_RADIUS, "6378137.0");
    kv.AddNameValue(CF_PP_INVERSE_FLATTENING, "298.257223563");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
    EXPECT_STREQ("LAEA (WGS84)", srs.GetAttrValue("PROJCS"));
  }

  // Azimuthal Equidistant
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_AE);
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }

  // Lambert Conformal Conic: 2SP, 1SP with scale, 1SP without scale (equal and
  // unequal parallels)
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LCC);
    kv.AddNameValue(CF_PP_STD_PARALLEL, "{30.0,60.0}");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LCC);
    kv.AddNameValue(CF_PP_SCALE_FACTOR_ORIGIN, "0.99");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }
  {
    // 1SP without scale factor, std_parallel == center_lat
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LCC);
    kv.AddNameValue(CF_PP_LAT_PROJ_ORIGIN, "45.0");
    kv.AddNameValue(CF_PP_STD_PARALLEL, "45.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }
  {
    // 1SP without scale factor, std_parallel != center_lat -> Snyder eq.
    // warning
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_LCC);
    kv.AddNameValue(CF_PP_LAT_PROJ_ORIGIN, "45.0");
    kv.AddNameValue(CF_PP_STD_PARALLEL, "25.0");
    OGRSpatialReference srs;
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }

  // Mercator: 1SP and 2SP
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_MERCATOR);
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_MERCATOR);
    kv.AddNameValue(CF_PP_STD_PARALLEL, "45.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }

  // Orthographic & Stereographic
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_ORTHOGRAPHIC);
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_STEREO);
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }

  // Polar Stereographic: Variant B (with std_parallel) and Variant A (with
  // +/-90 and invalid)
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_POLAR_STEREO);
    kv.AddNameValue(CF_PP_STD_PARALLEL, "71.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_POLAR_STEREO);
    kv.AddNameValue(CF_PP_LAT_PROJ_ORIGIN, "-90.0");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_POLAR_STEREO);
    kv.AddNameValue(CF_PP_LAT_PROJ_ORIGIN, "45.0");  // Invalid, clamped to 90
    OGRSpatialReference srs;
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }

  // Geostationary: sweep_angle_axis = x and y
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_GEOS);
    kv.AddNameValue(CF_PP_SWEEP_ANGLE_AXIS, "x");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }
  {
    CPLStringList kv;
    kv.AddNameValue(CF_GRD_MAPPING_NAME, CF_PT_GEOS);
    kv.AddNameValue(CF_PP_SWEEP_ANGLE_AXIS, "y");
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromCF1(kv.List(), nullptr));
  }
}

TEST_F(OgrSrsCf1Test, ExportProjections) {
  // Albers Equal Area (ACEA)
  {
    OGRSpatialReference srs;
    srs.SetACEA(20.0, 30.0, 25.0, 10.0, 500000.0, 0.0);
    char* pszGridMappingName = nullptr;
    char** papszKeyValues = nullptr;
    char* pszUnits = nullptr;
    EXPECT_EQ(OGRERR_NONE, srs.exportToCF1(&pszGridMappingName, &papszKeyValues,
                                           &pszUnits, nullptr));
    EXPECT_STREQ(CF_PT_AEA, pszGridMappingName);
    EXPECT_THAT(CSLFetchNameValue(papszKeyValues, CF_PP_STD_PARALLEL),
                NotNull());
    EXPECT_STREQ("m", pszUnits);
    CPLFree(pszGridMappingName);
    CSLDestroy(papszKeyValues);
    CPLFree(pszUnits);
  }

  // Azimuthal Equidistant & Cylindrical Equal Area
  {
    OGRSpatialReference srs;
    srs.SetAE(40.0, 10.0, 0.0, 0.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_AE, pszName);
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }
  {
    OGRSpatialReference srs;
    srs.SetCEA(30.0, 10.0, 0.0, 0.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_LCEA, pszName);
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }

  // Geostationary satellite (default and with +sweep=x extension)
  {
    OGRSpatialReference srs;
    srs.SetGEOS(0.0, 35785831.0, 0.0, 0.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ("y", CSLFetchNameValue(papszKV, CF_PP_SWEEP_ANGLE_AXIS));
    CPLFree(pszName);
    CSLDestroy(papszKV);

    srs.SetExtension(srs.GetRoot()->GetValue(), "PROJ4", "+proj=geos +sweep=x");
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ("x", CSLFetchNameValue(papszKV, CF_PP_SWEEP_ANGLE_AXIS));
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }

  // Lambert Conformal Conic (2SP and 1SP variants)
  {
    OGRSpatialReference srs;
    srs.SetLCC(30.0, 60.0, 45.0, 10.0, 0.0, 0.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_LCC, pszName);
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }
  {
    // LCC 1SP with scale_factor == 1.0 (copies standard_parallel_1 from
    // latitude_of_origin)
    OGRSpatialReference srs;
    srs.SetLCC1SP(45.0, 10.0, 1.0, 0.0, 0.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_LCC, pszName);
    EXPECT_THAT(CSLFetchNameValue(papszKV, CF_PP_STD_PARALLEL), NotNull());
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }
  {
    // LCC 1SP with scale_factor != 1.0 -> logs error and writes scale_factor
    OGRSpatialReference srs;
    srs.SetLCC1SP(45.0, 10.0, 0.99, 0.0, 0.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    {
      WithQuietHandler quiet;
      EXPECT_EQ(OGRERR_NONE,
                srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    }
    EXPECT_STREQ(CF_PT_LCC, pszName);
    EXPECT_THAT(CSLFetchNameValue(papszKV, CF_PP_SCALE_FACTOR_ORIGIN),
                NotNull());
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }

  // LAEA, Mercator (1SP & 2SP), Orthographic, Stereographic, TM
  {
    OGRSpatialReference srs;
    srs.SetLAEA(50.0, 10.0, 0.0, 0.0);
    char* pszName = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, nullptr, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_LAEA, pszName);
    CPLFree(pszName);
  }
  {
    OGRSpatialReference srs;
    srs.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0);
    char* pszName = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, nullptr, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_MERCATOR, pszName);
    CPLFree(pszName);
  }
  {
    OGRSpatialReference srs;
    srs.SetMercator2SP(45.0, 0.0, 0.0, 0.0, 0.0);
    char* pszName = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, nullptr, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_MERCATOR, pszName);
    CPLFree(pszName);
  }
  {
    OGRSpatialReference srs;
    srs.SetOrthographic(40.0, 10.0, 0.0, 0.0);
    char* pszName = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, nullptr, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_ORTHOGRAPHIC, pszName);
    CPLFree(pszName);
  }
  {
    OGRSpatialReference srs;
    srs.SetStereographic(40.0, 10.0, 1.0, 0.0, 0.0);
    char* pszName = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, nullptr, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_STEREO, pszName);
    CPLFree(pszName);
  }
  {
    OGRSpatialReference srs;
    srs.SetTM(0.0, 0.0, 0.9996, 500000.0, 0.0);
    char* pszName = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, nullptr, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_TM, pszName);
    CPLFree(pszName);
  }

  // Polar Stereographic: Variant A (scale factor) and Variant B (latitude of
  // origin, no scale factor)
  {
    OGRSpatialReference srs;
    srs.SetPS(90.0, 0.0, 0.994, 0.0, 0.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_POLAR_STEREO, pszName);
    EXPECT_THAT(CSLFetchNameValue(papszKV, CF_PP_SCALE_FACTOR_ORIGIN),
                NotNull());
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }
  {
    // Variant B: delete scale_factor parameter from PROJCS
    OGRSpatialReference srs;
    srs.SetPS(71.0, 0.0, 1.0, 0.0, 0.0);
    auto* poProjCS = srs.GetAttrNode("PROJCS");
    for (int i = 0; i < poProjCS->GetChildCount(); ++i) {
      if (EQUAL(poProjCS->GetChild(i)->GetValue(), "PARAMETER") &&
          EQUAL(poProjCS->GetChild(i)->GetChild(0)->GetValue(),
                SRS_PP_SCALE_FACTOR)) {
        poProjCS->DestroyChild(i);
        break;
      }
    }
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ(CF_PT_POLAR_STEREO, pszName);
    EXPECT_THAT(CSLFetchNameValue(papszKV, CF_PP_STD_PARALLEL), NotNull());
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }
}

TEST_F(OgrSrsCf1Test, ExportGenericAndDerivedAndUnits) {
  // Generic projection fallback (e.g. Cassini-Soldner, Sinusoidal,
  // HOTINE_OBLIQUE_MERCATOR)
  {
    OGRSpatialReference srs;
    srs.SetCS(0.0, 0.0, 0.0, 0.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ("cassini_soldner", pszName);
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }
  {
    // HOM: has unmapped azimuth parameter
    OGRSpatialReference srs;
    srs.SetHOM(40.0, 10.0, 45.0, 0.0, 1.0, 0.0, 0.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ("hotine_oblique_mercator", pszName);
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }

  // Derived Geographic CRS: netCDF CF convention
  {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    srs.SetDerivedGeogCRSWithPoleRotationNetCDFCFConvention("Rotated", 40.0,
                                                            10.0, 0.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ("rotated_pole", pszName);
    EXPECT_THAT(CSLFetchNameValue(papszKV, CF_PP_GRID_NORTH_POLE_LATITUDE),
                NotNull());
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }

  // Derived Geographic CRS: GRIB convention
  {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    srs.SetDerivedGeogCRSWithPoleRotationGRIBConvention("Rotated", -40.0, 10.0,
                                                        5.0);
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ("rotated_pole", pszName);
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }

  // Derived Geographic CRS: ob_tran
  {
    OGRSpatialReference srs;
    const char* wkt =
        "GEOGCRS[\"Rotated pole\",\n"
        "    BASEGEOGCRS[\"WGS 84\",\n"
        "        DATUM[\"World Geodetic System 1984\",\n"
        "            ELLIPSOID[\"WGS 84\",6378137,298.257223563]],\n"
        "        PRIMEM[\"Greenwich\",0],\n"
        "        CS[ellipsoidal,2],\n"
        "            AXIS[\"latitude\",north],\n"
        "            AXIS[\"longitude\",east],\n"
        "            UNIT[\"degree\",0.0174532925199433]],\n"
        "    DERIVINGCONVERSION[\"ob_tran\",\n"
        "        METHOD[\"PROJ ob_tran o_proj=longlat\"],\n"
        "        "
        "PARAMETER[\"lon_0\",190,ANGLEUNIT[\"degree\",0.0174532925199433]],\n"
        "        "
        "PARAMETER[\"o_lon_p\",0,ANGLEUNIT[\"degree\",0.0174532925199433]],\n"
        "        "
        "PARAMETER[\"o_lat_p\",40,ANGLEUNIT[\"degree\",0.0174532925199433]]],\n"
        "    CS[ellipsoidal,2],\n"
        "        AXIS[\"latitude\",north],\n"
        "        AXIS[\"longitude\",east],\n"
        "        UNIT[\"degree\",0.0174532925199433]]";
    EXPECT_EQ(OGRERR_NONE, srs.importFromWkt(wkt));
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ("rotated_pole", pszName);
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }

  // Derived Geographic CRS: unsupported method -> error
  {
    OGRSpatialReference srs;
    const char* wkt =
        "GEOGCRS[\"Rotated pole\",\n"
        "    BASEGEOGCRS[\"WGS 84\",\n"
        "        DATUM[\"World Geodetic System 1984\",\n"
        "            ELLIPSOID[\"WGS 84\",6378137,298.257223563]],\n"
        "        PRIMEM[\"Greenwich\",0],\n"
        "        CS[ellipsoidal,2],\n"
        "            AXIS[\"latitude\",north],\n"
        "            AXIS[\"longitude\",east],\n"
        "            UNIT[\"degree\",0.0174532925199433]],\n"
        "    DERIVINGCONVERSION[\"unsupported\",\n"
        "        METHOD[\"Unsupported_Method\"]],\n"
        "    CS[ellipsoidal,2],\n"
        "        AXIS[\"latitude\",north],\n"
        "        AXIS[\"longitude\",east],\n"
        "        UNIT[\"degree\",0.0174532925199433]]";
    EXPECT_EQ(OGRERR_NONE, srs.importFromWkt(wkt));
    char* pszName = nullptr;
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE,
              srs.exportToCF1(&pszName, nullptr, nullptr, nullptr));
    CPLFree(pszName);
  }

  // Geographic CRS export
  {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    char* pszName = nullptr;
    char** papszKV = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(&pszName, &papszKV, nullptr, nullptr));
    EXPECT_STREQ("crs", pszName);
    EXPECT_STREQ(CF_PT_LATITUDE_LONGITUDE,
                 CSLFetchNameValue(papszKV, CF_GRD_MAPPING_NAME));
    CPLFree(pszName);
    CSLDestroy(papszKV);
  }

  // Units export: km, US survey foot, custom
  {
    OGRSpatialReference srs;
    srs.SetTM(0.0, 0.0, 1.0, 0.0, 0.0);
    srs.SetLinearUnits("kilometre", 1000.0);
    char* pszUnits = nullptr;
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(nullptr, nullptr, &pszUnits, nullptr));
    EXPECT_STREQ("km", pszUnits);
    CPLFree(pszUnits);

    srs.SetLinearUnits(SRS_UL_US_FOOT, CPLAtof(SRS_UL_US_FOOT_CONV));
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(nullptr, nullptr, &pszUnits, nullptr));
    EXPECT_STREQ("US_survey_foot", pszUnits);
    CPLFree(pszUnits);

    srs.SetLinearUnits("custom", 2.5);
    EXPECT_EQ(OGRERR_NONE,
              srs.exportToCF1(nullptr, nullptr, &pszUnits, nullptr));
    EXPECT_STREQ("", pszUnits);
    CPLFree(pszUnits);
  }

  // Unknown projection -> error
  {
    OGRSpatialReference srs;
    srs.SetProjCS("Custom");
    srs.SetProjection("Unknown_Projection_Name");
    char* pszName = nullptr;
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_FAILURE,
              srs.exportToCF1(&pszName, nullptr, nullptr, nullptr));
    CPLFree(pszName);
  }
}

TEST_F(OgrSrsCf1Test, CApiRoundtrip) {
  OGRSpatialReference srs;
  srs.SetTM(0.0, 0.0, 0.9996, 500000.0, 0.0);
  char* pszName = nullptr;
  char** papszKV = nullptr;
  char* pszUnits = nullptr;
  OGRSpatialReferenceH hSRS = OGRSpatialReference::ToHandle(&srs);
  EXPECT_EQ(OGRERR_NONE,
            OSRExportToCF1(hSRS, &pszName, &papszKV, &pszUnits, nullptr));
  EXPECT_STREQ(CF_PT_TM, pszName);

  OGRSpatialReference srs2;
  OGRSpatialReferenceH hSRS2 = OGRSpatialReference::ToHandle(&srs2);
  EXPECT_EQ(OGRERR_NONE, OSRImportFromCF1(hSRS2, papszKV, pszUnits));
  EXPECT_TRUE(srs2.IsProjected());

  CPLFree(pszName);
  CSLDestroy(papszKV);
  CPLFree(pszUnits);
}

}  // namespace
}  // namespace autotest2
