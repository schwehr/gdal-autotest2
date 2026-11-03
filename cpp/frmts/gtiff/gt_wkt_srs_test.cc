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
// Tests GeoTIFF <-> OGRSpatialReference / WKT conversion, map projections,
// compound/3D CRS handling, and GeoTIFF-in-memory serialization
// (gt_wkt_srs.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gt_wkt_srs.cpp

#include "third_party/gdal/frmts/gtiff/gt_wkt_srs.h"

#include <array>
#include <cstdint>
#include <format>  // NOLINT
#include <iterator>
#include <limits>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/gtiff/gt_wkt_srs_for_gdal.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/ogr/ogr_proj_p.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr short kKvUserDefined = 32767;
constexpr short kModelTypeProjected = 1;
constexpr short kModelTypeGeographic = 2;
constexpr short kCtTransverseMercator = 1;
constexpr short kCtMercator = 7;
constexpr short kCtAlaskaConformal = 2;

struct MemGTIF {
  std::string path;
  VSILFILE* fp = nullptr;
  TIFF* tif = nullptr;
  GTIF* gtif = nullptr;

  explicit MemGTIF(const char* name) : path(name) {
    fp = VSIFOpenL(path.c_str(), "wb+");
    if (fp == nullptr) {
      ADD_FAILURE() << "VSIFOpenL failed for " << path;
      return;
    }
    tif = VSI_TIFFOpen(path.c_str(), "w+", fp);
    if (tif == nullptr) {
      ADD_FAILURE() << "VSI_TIFFOpen failed for " << path;
      return;
    }
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 1);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 1);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 1);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    uint8_t px = 0;
    TIFFWriteEncodedStrip(tif, 0, &px, 1);
    gtif = GTIFNew(tif);
    if (gtif == nullptr) {
      ADD_FAILURE() << "GTIFNew failed for " << path;
      return;
    }
    GTIFAttachPROJContext(gtif, OSRGetProjTLSContext());
  }

  OGRSpatialReference ReadBackOSR() {
    GTIFWriteKeys(gtif);
    GTIFDefn defn{};
    GTIFGetDefn(gtif, &defn);
    OGRSpatialReferenceH h = GTIFGetOGISDefnAsOSR(gtif, &defn);
    OGRSpatialReference out;
    if (h != nullptr) {
      out = *OGRSpatialReference::FromHandle(h);
      OSRDestroySpatialReference(h);
    }
    return out;
  }

  std::string ReadBackWkt() {
    GTIFWriteKeys(gtif);
    GTIFDefn defn{};
    GTIFGetDefn(gtif, &defn);
    std::unique_ptr<char, CplFreeDeleter> wkt(GTIFGetOGISDefn(gtif, &defn));
    return wkt != nullptr ? wkt.get() : "";
  }

  ~MemGTIF() {
    if (gtif != nullptr) GTIFFree(gtif);
    if (tif != nullptr) TIFFClose(tif);
    if (fp != nullptr) VSIFCloseL(fp);
    VSIUnlink(path.c_str());
  }
};

class GTWktSrsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GTiffOneTimeInit();
    LibgeotiffOneTimeInit();
    LibgeotiffOneTimeInit();  // Second call hits bOneTimeInitDone early return.
    CPLErrorReset();
  }

  void TearDown() override { CPLErrorReset(); }
};

TEST_F(GTWktSrsTest, AllProjectionMethodsRoundTripAndMercatorDefaultEdgeCases) {
  // 1. Table of 28 projection setters (27 methods known to the GeoTIFF writer
  // plus Bonne as an unknown one) with custom user-defined GeogCS so
  // nPCS == kKvUserDefined.
  using ProjSetter = void (*)(OGRSpatialReference&);
  constexpr auto kSetters = std::to_array<ProjSetter>({
      [](OGRSpatialReference& s) { s.SetTM(10.0, 15.0, 0.99, 1000.0, 2000.0); },
      [](OGRSpatialReference& s) { s.SetTMSO(-25.0, 27.0, 1.0, 0.0, 0.0); },
      [](OGRSpatialReference& s) {
        s.SetMercator(0.0, 10.0, 0.999, 500.0, 1000.0);
      },
      [](OGRSpatialReference& s) {
        s.SetMercator2SP(30.0, 0.0, 10.0, 500.0, 1000.0);
      },
      [](OGRSpatialReference& s) {
        s.SetOS(52.0, 5.0, 0.9999, 155000.0, 463000.0);
      },
      [](OGRSpatialReference& s) {
        s.SetStereographic(45.0, 10.0, 1.0, 0.0, 0.0);
      },
      [](OGRSpatialReference& s) {
        s.SetHOM(45.0, 7.0, 30.0, 30.0, 0.9999, 200000.0, 600000.0);
      },
      [](OGRSpatialReference& s) {
        s.SetHOMAC(4.0, 115.0, 53.3158, 53.1301, 0.99984, 590476.0, 442857.0);
      },
      [](OGRSpatialReference& s) {
        s.SetLOM(-18.9, 46.437, 18.9, 0.9995, 400000.0, 800000.0);
      },
      [](OGRSpatialReference& s) {
        s.SetEC(30.0, 60.0, 45.0, -96.0, 0.0, 0.0);
      },
      [](OGRSpatialReference& s) { s.SetCS(52.418, 13.627, 40000.0, 10000.0); },
      [](OGRSpatialReference& s) {
        s.SetPolyconic(0.0, -54.0, 5000000.0, 10000000.0);
      },
      [](OGRSpatialReference& s) { s.SetAE(45.0, 10.0, 0.0, 0.0); },
      [](OGRSpatialReference& s) { s.SetMC(0.0, 10.0, 0.0, 0.0); },
      [](OGRSpatialReference& s) {
        s.SetEquirectangular2(0.0, 10.0, 30.0, 0.0, 0.0);
      },
      [](OGRSpatialReference& s) { s.SetGnomonic(45.0, 10.0, 0.0, 0.0); },
      [](OGRSpatialReference& s) {
        s.SetLAEA(52.0, 10.0, 4321000.0, 3210000.0);
      },
      [](OGRSpatialReference& s) { s.SetOrthographic(45.0, 10.0, 0.0, 0.0); },
      [](OGRSpatialReference& s) { s.SetRobinson(10.0, 0.0, 0.0); },
      [](OGRSpatialReference& s) { s.SetSinusoidal(10.0, 0.0, 0.0); },
      [](OGRSpatialReference& s) { s.SetVDG(10.0, 0.0, 0.0); },
      [](OGRSpatialReference& s) { s.SetPS(-71.0, 0.0, 1.0, 0.0, 0.0); },
      [](OGRSpatialReference& s) {
        s.SetLCC(33.0, 45.0, 39.0, -96.0, 100.0, 200.0);
      },
      [](OGRSpatialReference& s) {
        s.SetLCC1SP(49.5, 2.337, 0.999877, 600000.0, 200000.0);
      },
      [](OGRSpatialReference& s) {
        s.SetACEA(29.5, 45.5, 23.0, -96.0, 0.0, 0.0);
      },
      [](OGRSpatialReference& s) {
        s.SetNZMG(-41.0, 173.0, 2510000.0, 6023150.0);
      },
      [](OGRSpatialReference& s) { s.SetCEA(30.0, -96.0, 0.0, 0.0); },
      // Unknown projection (Bonne) -> writes ESRI PE string.
      [](OGRSpatialReference& s) { s.SetBonne(45.0, 10.0, 0.0, 0.0); },
  });

  for (size_t i = 0; i < kSetters.size(); ++i) {
    const std::string path = std::format("/vsimem/gtwkt_proj_{}.tif", i);
    MemGTIF m(path.c_str());

    OGRSpatialReference srs;
    srs.SetProjCS("CustomPCS");
    srs.SetGeogCS("CustomGeog", "CustomDatum", "CustomSpheroid", 6378137.0,
                  298.257223563);
    kSetters[i](srs);
    // Test feet linear units on LCC 2SP (i==22) and TM (i==0).
    if (i == 0 || i == 22) {
      srs.SetLinearUnits(SRS_UL_FOOT, CPLAtof(SRS_UL_FOOT_CONV));
    }

    {
      WithQuietHandler quiet;
      EXPECT_NE(FALSE, GTIFSetFromOGISDefnEx(
                           m.gtif, OGRSpatialReference::ToHandle(&srs),
                           GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_AUTO))
          << "index " << i;
      CPLErrorReset();
    }
    EXPECT_FALSE(m.ReadBackWkt().empty()) << "index " << i;
  }

  // 2. Mercator with both ProjStdParallel1GeoKey and ProjScaleAtNatOriginGeoKey
  // + ProjectionGeoKey == 1024, plus unsupported kCtAlaskaConformal demoted to
  // LOCAL_CS.
  {
    MemGTIF m1("/vsimem/gtwkt_merc_both.tif");
    GTIFKeySet(m1.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kModelTypeProjected);
    GTIFKeySet(m1.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, kKvUserDefined);
    GTIFKeySet(m1.gtif, ProjectionGeoKey, TYPE_SHORT, 1, 1024);
    GTIFKeySet(m1.gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1, kCtMercator);
    GTIFKeySet(m1.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    GTIFKeySet(m1.gtif, ProjStdParallel1GeoKey, TYPE_DOUBLE, 1, 0.0);
    GTIFKeySet(m1.gtif, ProjScaleAtNatOriginGeoKey, TYPE_DOUBLE, 1, 1.0);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_NE(FALSE, m1.ReadBackOSR().IsProjected());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(
          CPLGetLastErrorMsg(),
          ::testing::HasSubstr(
              "Mercator projection should not define both StdParallel1 and "
              "ScaleAtNatOrigin"));
      CPLErrorReset();
    }

    MemGTIF m2("/vsimem/gtwkt_ct_default.tif");
    GTIFKeySet(m2.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kModelTypeProjected);
    GTIFKeySet(m2.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, kKvUserDefined);
    GTIFKeySet(m2.gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
               kCtAlaskaConformal);
    GTIFKeySet(m2.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    CPLErrorReset();
    EXPECT_NE(FALSE, m2.ReadBackOSR().IsLocal());
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }
}

TEST_F(GTWktSrsTest, LocalGeocentricImagineCitationsUTMAndDatumUnitsVariants) {
  // 1. GTIFSetFromOGISDefn invalid WKT -> FALSE; valid WKT -> TRUE.
  {
    MemGTIF m("/vsimem/gtwkt_c_api.tif");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(FALSE, GTIFSetFromOGISDefn(m.gtif, "NOT_VALID_WKT[[["));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
    EXPECT_NE(FALSE, GTIFSetFromOGISDefn(m.gtif, SRS_WKT_WGS84_LAT_LONG));
  }

  // 2. Empty GeoKeys (nKeyCount == 0) -> empty OGRSpatialReference.
  {
    MemGTIF m("/vsimem/gtwkt_empty_keys.tif");
    EXPECT_TRUE(m.ReadBackOSR().IsEmpty());
  }

  // 3. LOCAL_CS + Imagine citation cleanup + Compound LOCAL_CS with
  // VerticalCitationGeoKey and manual VertCS.
  {
    MemGTIF m1("/vsimem/gtwkt_local_img.tif");
    GTIFKeySet(m1.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kKvUserDefined);
    GTIFKeySet(
        m1.gtif, GTCitationGeoKey, TYPE_ASCII, 0,
        "IMAGINE GeoTIFF Support\nCopyright 1991 $Date$\nLocalGrid\nLine2");
    GTIFKeySet(m1.gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1, 9002);
    GTIFKeySet(m1.gtif, VerticalUnitsGeoKey, TYPE_SHORT, 1, 9003);
    GTIFKeySet(m1.gtif, VerticalDatumGeoKey, TYPE_SHORT, 1, kKvUserDefined);
    GTIFKeySet(m1.gtif, VerticalCitationGeoKey, TYPE_ASCII, 0,
               "VCS Name = NAVD88 height|Extra");
    OGRSpatialReference out1 = m1.ReadBackOSR();
    EXPECT_NE(FALSE, out1.IsCompound());

    MemGTIF m2("/vsimem/gtwkt_local_write.tif");
    EXPECT_NE(FALSE, GTIFSetFromOGISDefnEx(
                         m2.gtif, OGRSpatialReference::ToHandle(&out1),
                         GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_0));
    EXPECT_FALSE(m2.ReadBackWkt().empty());

    MemGTIF m3("/vsimem/gtwkt_local_vdatum.tif");
    GTIFKeySet(m3.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kKvUserDefined);
    GTIFKeySet(m3.gtif, GeogCitationGeoKey, TYPE_ASCII, 0, "LocalFromGeog");
    GTIFKeySet(m3.gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1, kKvUserDefined);
    GTIFKeySet(m3.gtif, ProjLinearUnitSizeGeoKey, TYPE_DOUBLE, 1, 2.0);
    GTIFKeySet(m3.gtif, VerticalUnitsGeoKey, TYPE_SHORT, 1, 9001);
    GTIFKeySet(m3.gtif, VerticalDatumGeoKey, TYPE_SHORT, 1, 5103);
    WithQuietHandler quiet;
    EXPECT_NE(FALSE, m3.ReadBackOSR().IsCompound());
    CPLErrorReset();
  }

  // 4. ModelTypeGeocentric (known & user-defined linear units).
  {
    OGRSpatialReference geoc;
    geoc.SetGeocCS("MyGeocentric");
    geoc.SetWellKnownGeogCS("WGS84");
    geoc.SetLinearUnits("CustomLink", 0.201168);

    MemGTIF m("/vsimem/gtwkt_geoc.tif");
    WithQuietHandler quiet;
    EXPECT_NE(FALSE, GTIFSetFromOGISDefnEx(
                         m.gtif, OGRSpatialReference::ToHandle(&geoc),
                         GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_0));
    EXPECT_NE(FALSE, m.ReadBackOSR().IsGeocentric());
    CPLErrorReset();
  }

  // 5. UTM Zone special-casing on Write (NAD83, NAD27, WGS84 N & S, WGS72).
  constexpr const char* kUtmDatums[] = {"NAD83", "NAD27", "WGS84", "WGS72"};
  for (const char* datum : kUtmDatums) {
    for (int north : {TRUE, FALSE}) {
      if (!EQUAL(datum, "WGS84") && !north) continue;
      OGRSpatialReference utm;
      utm.SetProjCS("CustomUTM");
      utm.SetWellKnownGeogCS(datum);
      utm.SetUTM(10, north);
      const std::string p =
          std::format("/vsimem/gtwkt_utm_{}_{}.tif", datum, north);
      MemGTIF m(p.c_str());
      EXPECT_NE(FALSE, GTIFSetFromOGISDefnEx(
                           m.gtif, OGRSpatialReference::ToHandle(&utm),
                           GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_0));
      EXPECT_NE(FALSE, m.ReadBackOSR().IsProjected());
    }
  }

  // 6. OGCDatumName2EPSGDatumCode + WKTMassageDatum + custom angular units.
  {
    constexpr const char* kWktMgi =
        "GEOGCS[\"CustomMGI\",DATUM[\"Militar_Geographische_Institute\","
        "SPHEROID[\"Bessel 1841\",6377397.155,299.1528128]],"
        "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";
    MemGTIF m_mgi("/vsimem/gtwkt_mgi.tif");
    EXPECT_NE(FALSE, GTIFSetFromOGISDefn(m_mgi.gtif, kWktMgi));

    struct AngUnitCase {
      const char* name;
      double rad;
    };
    constexpr AngUnitCase kAngUnits[] = {
        {.name = "arc-second", .rad = 4.84813681109536e-06},
        {.name = "arc-minute", .rad = 0.0002908882086657216},
        {.name = "grad", .rad = 0.015707963267948967},
        {.name = "gon", .rad = 0.015707963267948967},
        {.name = "radian", .rad = 1.0},
        {.name = "my_angle", .rad = 0.02},
    };
    for (size_t i = 0; i < std::size(kAngUnits); ++i) {
      OGRSpatialReference gsrs;
      gsrs.SetGeogCS("CustomGCS", "TotallyUnknownDatum_123", "CustomSph",
                     6371000.0, (i % 2 == 0) ? 0.0 : 298.3, "Paris", 2.337,
                     kAngUnits[i].name, kAngUnits[i].rad);
      if (i == 2) {
        gsrs.SetProjCS("GradLCC");
        gsrs.SetLCC1SP(52.0, 0.0, 0.99987742, 600000.0, 2200000.0);
        gsrs.SetLinearUnits("CustomMeter", 1.05);
      }
      const std::string p = std::format("/vsimem/gtwkt_ang_{}.tif", i);
      MemGTIF m(p.c_str());
      WithQuietHandler quiet;
      EXPECT_NE(FALSE, GTIFSetFromOGISDefnEx(
                           m.gtif, OGRSpatialReference::ToHandle(&gsrs),
                           GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_0));
      EXPECT_FALSE(m.ReadBackWkt().empty());
      CPLErrorReset();
    }
  }
}

TEST_F(GTWktSrsTest, Compound3DESRIPEConfigOptionsAndEdgeCases) {
  // 1. EPSG:3857 with GEOTIFF_KEYS_ESRI_PE.
  {
    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    MemGTIF m("/vsimem/gtwkt_3857_esri.tif");
    EXPECT_NE(FALSE, GTIFSetFromOGISDefnEx(
                         m.gtif, OGRSpatialReference::ToHandle(&srs3857),
                         GEOTIFF_KEYS_ESRI_PE, GEOTIFF_VERSION_1_0));
    EXPECT_NE(FALSE, m.ReadBackOSR().IsProjected());
  }

  // 2. Geographic 3D (EPSG:4979 & EPSG:4937) & Compound CRS (EPSG:5498).
  constexpr int kCrsCodes[] = {4979, 4937, 5498};
  constexpr GeoTIFFVersionEnum kVersions[] = {
      GEOTIFF_VERSION_1_0, GEOTIFF_VERSION_1_1, GEOTIFF_VERSION_AUTO};
  for (int code : kCrsCodes) {
    for (GeoTIFFVersionEnum ver : kVersions) {
      OGRSpatialReference srs;
      srs.importFromEPSG(code);
      srs.SetCoordinateEpoch(2020.5);
      const std::string p = std::format("/vsimem/gtwkt_3d_{}_{}.tif", code,
                                        static_cast<int>(ver));
      MemGTIF m(p.c_str());
      EXPECT_NE(FALSE, GTIFSetFromOGISDefnEx(
                           m.gtif, OGRSpatialReference::ToHandle(&srs),
                           GEOTIFF_KEYS_STANDARD, ver));
      OGRSpatialReference out = m.ReadBackOSR();
      EXPECT_FALSE(out.IsEmpty());
      EXPECT_NEAR(2020.5, out.GetCoordinateEpoch(), 1e-4);
    }
  }

  // 3. GeoTIFF 1.0 legacy vertical codes and private vertical code > 32767.
  constexpr int kLegacyVCodes[] = {5103, 5171, 5030, 32768, 4326};
  for (int vcode : kLegacyVCodes) {
    const std::string p = std::format("/vsimem/gtwkt_legacy_v_{}.tif", vcode);
    MemGTIF m(p.c_str());
    GTIFKeySet(m.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kModelTypeGeographic);
    GTIFKeySet(m.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    GTIFKeySet(m.gtif, VerticalCSTypeGeoKey, TYPE_SHORT, 1, vcode);
    GTIFKeySet(m.gtif, VerticalUnitsGeoKey, TYPE_SHORT, 1, 9001);
    WithQuietHandler quiet;
    EXPECT_FALSE(m.ReadBackOSR().IsEmpty());
    CPLErrorReset();
  }

  // 4. Custom Compound CRS without EPSG vertical code in GeoTIFF 1.1.
  {
    OGRSpatialReference horiz;
    OGRSpatialReference vert;
    OGRSpatialReference compd;
    horiz.importFromEPSG(32611);
    vert.SetVertCS("MyCustomVert", "MyCustomVDatum", 2005);
    vert.SetLinearUnits(SRS_UL_FOOT, CPLAtof(SRS_UL_FOOT_CONV));
    compd.SetCompoundCS("CustomCompd", &horiz, &vert);

    MemGTIF m("/vsimem/gtwkt_custom_compd_11.tif");
    WithQuietHandler quiet;
    EXPECT_NE(FALSE, GTIFSetFromOGISDefnEx(
                         m.gtif, OGRSpatialReference::ToHandle(&compd),
                         GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_1));
    EXPECT_NE(FALSE, m.ReadBackOSR().IsCompound());
    CPLErrorReset();
  }

  // 5. Projected 3D CRS demotion on write.
  {
    OGRSpatialReference p3d;
    p3d.importFromEPSG(32611);
    p3d.PromoteTo3D(nullptr);
    MemGTIF m("/vsimem/gtwkt_proj3d.tif");
    EXPECT_NE(FALSE, GTIFSetFromOGISDefnEx(
                         m.gtif, OGRSpatialReference::ToHandle(&p3d),
                         GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_1));
    EXPECT_FALSE(m.ReadBackOSR().IsEmpty());
  }

  // 6. GTIFF_SRS_SOURCE ("", "EPSG", "GEOKEYS") on inconsistent GeogCS &
  // ProjCS.
  constexpr const char* kSrsSources[] = {"", "EPSG", "GEOKEYS"};
  for (const char* src_opt : kSrsSources) {
    WithQuietHandler quiet;
    CPLConfigOptionSetter cfg("GTIFF_SRS_SOURCE",
                              src_opt[0] != '\0' ? src_opt : nullptr,
                              /*bSetOnlyIfUndefined=*/false);
    const std::string p1 =
        std::format("/vsimem/gtwkt_inc_geog_{}.tif", src_opt);
    MemGTIF m1(p1.c_str());
    GTIFKeySet(m1.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kModelTypeGeographic);
    GTIFKeySet(m1.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    GTIFKeySet(m1.gtif, GeogSemiMajorAxisGeoKey, TYPE_DOUBLE, 1, 6370000.0);
    EXPECT_FALSE(m1.ReadBackOSR().IsEmpty());

    const std::string p2 =
        std::format("/vsimem/gtwkt_inc_proj_{}.tif", src_opt);
    MemGTIF m2(p2.c_str());
    GTIFKeySet(m2.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kModelTypeProjected);
    GTIFKeySet(m2.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, 32611);
    GTIFKeySet(m2.gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
               kCtTransverseMercator);
    GTIFKeySet(m2.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    GTIFKeySet(m2.gtif, ProjNatOriginLongGeoKey, TYPE_DOUBLE, 1, -117.0);
    GTIFKeySet(m2.gtif, ProjScaleAtNatOriginGeoKey, TYPE_DOUBLE, 1, 0.9996);
    GTIFKeySet(m2.gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1, 12345.0);
    EXPECT_FALSE(m2.ReadBackOSR().IsEmpty());
    CPLErrorReset();
  }

  // 7. GTIFF_LINEAR_UNITS=BROKEN + Fast-path PCS with overridden linear unit.
  {
    WithQuietHandler quiet;
    CPLConfigOptionSetter broken("GTIFF_LINEAR_UNITS", "BROKEN",
                                 /*bSetOnlyIfUndefined=*/false);
    MemGTIF m1("/vsimem/gtwkt_broken_lu.tif");
    GTIFKeySet(m1.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kModelTypeProjected);
    GTIFKeySet(m1.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, kKvUserDefined);
    GTIFKeySet(m1.gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
               kCtTransverseMercator);
    GTIFKeySet(m1.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    GTIFKeySet(m1.gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1, 9002);
    GTIFKeySet(m1.gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1, 1000.0);
    GTIFKeySet(m1.gtif, ProjFalseNorthingGeoKey, TYPE_DOUBLE, 1, 2000.0);
    GTIFKeySet(m1.gtif, ProjFalseOriginEastingGeoKey, TYPE_DOUBLE, 1, 3000.0);
    GTIFKeySet(m1.gtif, ProjFalseOriginNorthingGeoKey, TYPE_DOUBLE, 1, 4000.0);
    GTIFKeySet(m1.gtif, ProjCenterEastingGeoKey, TYPE_DOUBLE, 1, 5000.0);
    GTIFKeySet(m1.gtif, ProjCenterNorthingGeoKey, TYPE_DOUBLE, 1, 6000.0);
    EXPECT_NE(FALSE, m1.ReadBackOSR().IsProjected());

    MemGTIF m2("/vsimem/gtwkt_pcs_foot_override.tif");
    GTIFKeySet(m2.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kModelTypeProjected);
    GTIFKeySet(m2.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, 32611);
    GTIFKeySet(m2.gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1, 9002);
    EXPECT_NE(FALSE, m2.ReadBackOSR().IsProjected());
    CPLErrorReset();
  }

  // 8. TOWGS84 (3-param and 7-param) with GTIFF_WRITE_TOWGS84=YES &
  // OSR_STRIP_TOWGS84=NO.
  {
    CPLConfigOptionSetter wt("GTIFF_WRITE_TOWGS84", "YES",
                             /*bSetOnlyIfUndefined=*/false);
    CPLConfigOptionSetter st("OSR_STRIP_TOWGS84", "NO",
                             /*bSetOnlyIfUndefined=*/false);
    for (int seven : {0, 1}) {
      OGRSpatialReference srs;
      srs.SetGeogCS("CustomT", "CustomDT", "CustomET", 6378137.0,
                    298.257223563);
      if (seven) {
        srs.SetTOWGS84(1.0, 2.0, 3.0, 0.1, 0.2, 0.3, 0.4);
      } else {
        srs.SetTOWGS84(10.0, 20.0, 30.0);
      }
      const std::string p = std::format("/vsimem/gtwkt_towgs84_{}.tif", seven);
      MemGTIF m(p.c_str());
      EXPECT_NE(FALSE, GTIFSetFromOGISDefnEx(
                           m.gtif, OGRSpatialReference::ToHandle(&srs),
                           GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_0));
      double params[7] = {};
      EXPECT_EQ(OGRERR_NONE, m.ReadBackOSR().GetTOWGS84(params, 7));
    }
  }

  // 9. ESRI underscore morphing, INF inv flattening, special Web Mercator
  // names.
  constexpr const char* kPcsCitations[] = {
      "WGS_1984_Web_Mercator", "Google Maps Global Mercator",
      "WGS_1984_UTM_Zone_11N", "WGS 84 / UTM zone 11N"};
  for (size_t i = 0; i < std::size(kPcsCitations); ++i) {
    const char* pcs_cit = kPcsCitations[i];
    SCOPED_TRACE(pcs_cit);
    const std::string p = std::format("/vsimem/gtwkt_special_{}.tif", i);
    MemGTIF m(p.c_str());
    GTIFKeySet(m.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kModelTypeProjected);
    GTIFKeySet(m.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, kKvUserDefined);
    GTIFKeySet(m.gtif, GTCitationGeoKey, TYPE_ASCII, 0, pcs_cit);
    GTIFKeySet(m.gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
               kCtTransverseMercator);
    GTIFKeySet(
        m.gtif, GeogCitationGeoKey, TYPE_ASCII, 0,
        "GCS Name = GCS_WGS_1984|Datum = D_WGS_1984|Ellipsoid = WGS_1984|");
    GTIFKeySet(m.gtif, GeogSemiMajorAxisGeoKey, TYPE_DOUBLE, 1, 6378137.0);
    GTIFKeySet(m.gtif, GeogInvFlatteningGeoKey, TYPE_DOUBLE, 1, 298.257223563);
    GTIFKeySet(m.gtif, ProjNatOriginLongGeoKey, TYPE_DOUBLE, 1, -117.0);
    GTIFKeySet(m.gtif, ProjScaleAtNatOriginGeoKey, TYPE_DOUBLE, 1, 0.9996);
    GTIFKeySet(m.gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1, 500000.0);
    GTIFKeySet(m.gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1, 9001);
    EXPECT_FALSE(m.ReadBackOSR().IsEmpty());
  }

  // 10. INFINITY GeogInvFlatteningGeoKey + 0 GeogAngularUnitSizeGeoKey warning.
  {
    MemGTIF m("/vsimem/gtwkt_inf_invf.tif");
    GTIFKeySet(m.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, kModelTypeGeographic);
    GTIFKeySet(m.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, kKvUserDefined);
    GTIFKeySet(m.gtif, GeogCitationGeoKey, TYPE_ASCII, 0,
               "GCS Name = CustomSph|Ellipsoid = Sph|AUnits = bad|");
    GTIFKeySet(m.gtif, GeogSemiMajorAxisGeoKey, TYPE_DOUBLE, 1, 6371000.0);
    GTIFKeySet(m.gtif, GeogInvFlatteningGeoKey, TYPE_DOUBLE, 1,
               std::numeric_limits<double>::infinity());
    GTIFKeySet(m.gtif, GeogAngularUnitSizeGeoKey, TYPE_DOUBLE, 1, 0.0);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(FALSE, m.ReadBackOSR().IsGeographic());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(GTWktSrsTest, MemBufFunctionsAllGeorefModes) {
  // 1. Corrupt buffer -> CE_Failure.
  {
    unsigned char bad[4] = {'B', 'A', 'D', '!'};
    char* raw_wkt = nullptr;
    double gt[6] = {};
    int ngcp = 0;
    GDAL_GCP* gcps = nullptr;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GTIFWktFromMemBuf(4, bad, &raw_wkt, gt, &ngcp, &gcps));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    std::unique_ptr<char, CplFreeDeleter> wkt(raw_wkt);
    EXPECT_EQ(nullptr, wkt);
  }

  // 2. GTIFMemBufFromWkt + GTIFWktFromMemBuf: north-up geotransform.
  {
    constexpr double kGtIn[6] = {100.0, 2.0, 0.0, 500.0, 0.0, -2.0};
    int sz = 0;
    unsigned char* raw_buf = nullptr;
    ASSERT_EQ(CE_None, GTIFMemBufFromWkt(SRS_WKT_WGS84_LAT_LONG, kGtIn, 0,
                                         nullptr, &sz, &raw_buf));
    std::unique_ptr<unsigned char, decltype(&VSIFree)> buf(raw_buf, VSIFree);
    ASSERT_GT(sz, 0);
    ASSERT_NE(nullptr, buf);

    char* raw_wkt = nullptr;
    double gt_out[6] = {};
    int ngcp = 0;
    GDAL_GCP* gcps = nullptr;
    EXPECT_EQ(CE_None,
              GTIFWktFromMemBuf(sz, buf.get(), &raw_wkt, gt_out, &ngcp, &gcps));
    std::unique_ptr<char, CplFreeDeleter> wkt_out(raw_wkt);
    EXPECT_NE(nullptr, wkt_out);
    EXPECT_DOUBLE_EQ(100.0, gt_out[0]);
  }

  // 3. GTIFMemBufFromSRS + GTIFWktFromMemBufEx:
  // (a) North-up + bPixelIsPoint=TRUE + RPC metadata.
  {
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    constexpr double kGtIn[6] = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
    constexpr const char* kCoeffs = "1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0";
    CPLStringList rpc;
    rpc.SetNameValue("LINE_OFF", "10");
    rpc.SetNameValue("SAMP_OFF", "10");
    rpc.SetNameValue("LAT_OFF", "30");
    rpc.SetNameValue("LONG_OFF", "-120");
    rpc.SetNameValue("HEIGHT_OFF", "50");
    rpc.SetNameValue("LINE_SCALE", "10");
    rpc.SetNameValue("SAMP_SCALE", "10");
    rpc.SetNameValue("LAT_SCALE", "1");
    rpc.SetNameValue("LONG_SCALE", "1");
    rpc.SetNameValue("HEIGHT_SCALE", "100");
    rpc.SetNameValue("LINE_NUM_COEFF", kCoeffs);
    rpc.SetNameValue("LINE_DEN_COEFF", kCoeffs);
    rpc.SetNameValue("SAMP_NUM_COEFF", kCoeffs);
    rpc.SetNameValue("SAMP_DEN_COEFF", kCoeffs);

    int sz = 0;
    unsigned char* raw_buf = nullptr;
    ASSERT_EQ(CE_None, GTIFMemBufFromSRS(OGRSpatialReference::ToHandle(&srs),
                                         kGtIn, 0, nullptr, &sz, &raw_buf,
                                         /*bPixelIsPoint=*/TRUE, rpc.List()));
    std::unique_ptr<unsigned char, decltype(&VSIFree)> buf(raw_buf, VSIFree);

    OGRSpatialReferenceH out_srs = nullptr;
    double gt_out[6] = {};
    int ngcp = 0;
    GDAL_GCP* gcps = nullptr;
    int pip = FALSE;
    char** raw_rpc_out = nullptr;
    const CPLErr err = GTIFWktFromMemBufEx(sz, buf.get(), &out_srs, gt_out,
                                           &ngcp, &gcps, &pip, &raw_rpc_out);
    absl::Cleanup destroy_srs = [out_srs]() {
      if (out_srs != nullptr) OSRDestroySpatialReference(out_srs);
    };
    CPLStringList rpc_out(raw_rpc_out, /*bTakeOwnership=*/TRUE);
    EXPECT_EQ(CE_None, err);
    EXPECT_NE(FALSE, pip);
    EXPECT_DOUBLE_EQ(10.0, gt_out[0]);
    EXPECT_FALSE(rpc_out.empty());
  }

  // (b) Rotated geotransform (TIFFTAG_GEOTRANSMATRIX) + bPixelIsPoint=TRUE.
  {
    OGRSpatialReference srs;
    srs.importFromEPSG(32611);
    constexpr double kGtRot[6] = {1000.0, 1.0, 0.25, 2000.0, 0.25, -1.0};
    int sz = 0;
    unsigned char* raw_buf = nullptr;
    ASSERT_EQ(CE_None, GTIFMemBufFromSRS(OGRSpatialReference::ToHandle(&srs),
                                         kGtRot, 0, nullptr, &sz, &raw_buf,
                                         /*bPixelIsPoint=*/TRUE, nullptr));
    std::unique_ptr<unsigned char, decltype(&VSIFree)> buf(raw_buf, VSIFree);

    OGRSpatialReferenceH out_srs = nullptr;
    double gt_out[6] = {};
    int ngcp = 0;
    GDAL_GCP* gcps = nullptr;
    int pip = FALSE;
    const CPLErr err = GTIFWktFromMemBufEx(sz, buf.get(), &out_srs, gt_out,
                                           &ngcp, &gcps, &pip, nullptr);
    absl::Cleanup destroy_srs = [out_srs]() {
      if (out_srs != nullptr) OSRDestroySpatialReference(out_srs);
    };
    EXPECT_EQ(CE_None, err);
    EXPECT_NEAR(1000.625, gt_out[0], 1e-6);
    EXPECT_NEAR(0.25, gt_out[2], 1e-6);
  }

  // (c) GCPs (nGCPCount = 2 with identity GT).
  {
    GDAL_GCP gcps_in[2];
    GDALInitGCPs(2, gcps_in);
    absl::Cleanup clean_in = [&]() { GDALDeinitGCPs(2, gcps_in); };
    gcps_in[0].dfGCPPixel = 0.0;
    gcps_in[0].dfGCPLine = 0.0;
    gcps_in[0].dfGCPX = 10.0;
    gcps_in[0].dfGCPY = 20.0;
    gcps_in[1].dfGCPPixel = 10.0;
    gcps_in[1].dfGCPLine = 10.0;
    gcps_in[1].dfGCPX = 30.0;
    gcps_in[1].dfGCPY = 40.0;
    constexpr double kGtId[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    int sz = 0;
    unsigned char* raw_buf = nullptr;
    ASSERT_EQ(CE_None, GTIFMemBufFromWkt(SRS_WKT_WGS84_LAT_LONG, kGtId, 2,
                                         gcps_in, &sz, &raw_buf));
    std::unique_ptr<unsigned char, decltype(&VSIFree)> buf(raw_buf, VSIFree);

    char* raw_wkt = nullptr;
    double gt_out[6] = {};
    int ngcp_out = 0;
    GDAL_GCP* gcps_out = nullptr;
    const CPLErr err = GTIFWktFromMemBuf(sz, buf.get(), &raw_wkt, gt_out,
                                         &ngcp_out, &gcps_out);
    std::unique_ptr<char, CplFreeDeleter> wkt_out(raw_wkt);
    absl::Cleanup clean_out = [&]() {
      if (gcps_out != nullptr) {
        GDALDeinitGCPs(ngcp_out, gcps_out);
        CPLFree(gcps_out);
      }
    };
    EXPECT_EQ(CE_None, err);
    EXPECT_EQ(2, ngcp_out);
  }
}

// TODO: b/335317901 - Cover remaining defensive PROJ error branches and rare
// 1D/vertical WKT2 edge cases in gt_wkt_srs.cpp.

}  // namespace
}  // namespace autotest2
