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

#include "third_party/gdal/frmts/gtiff/gt_wkt_srs.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gt_wkt_srs_for_gdal.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

struct PJ_CONTEXT;
extern PJ_CONTEXT* OSRGetProjTLSContext();

namespace autotest2 {
namespace {

constexpr short KvUserDefined = 32767;
constexpr short ModelTypeProjected = 1;
constexpr short ModelTypeGeographic = 2;
constexpr short CT_TransverseMercator = 1;
constexpr short CT_Mercator = 7;
constexpr short CT_AlaskaConformal = 2;

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
    char* wkt = GTIFGetOGISDefn(gtif, &defn);
    std::string res = wkt ? wkt : "";
    CPLFree(wkt);
    return res;
  }

  ~MemGTIF() {
    if (gtif) GTIFFree(gtif);
    if (tif) TIFFClose(tif);
    if (fp) VSIFCloseL(fp);
    VSIUnlink(path.c_str());
  }
};

class GTWktSrsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    GTiffOneTimeInit();
    LibgeotiffOneTimeInit();
    LibgeotiffOneTimeInit();  // second call hits bOneTimeInitDone early return
  }
};

TEST_F(GTWktSrsTest,
       AllProjectionMethodsRoundTripAndMercatorDefaultEdgeCases) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // Table of 28 projection setters (27 methods known to the GeoTIFF writer
  // plus Bonne as an unknown one) with custom user-defined GeogCS so
  // nPCS == KvUserDefined -> exercises BOTH the writer if/else chain
  // (L2312-3139) AND the reader CTProjection switch (L1438-1596)!
  std::vector<std::function<void(OGRSpatialReference&)>> setters = {
      [](OGRSpatialReference& s) {
        // Non-UTM Transverse Mercator (scale 0.99, lat 10).
        s.SetTM(10.0, 15.0, 0.99, 1000.0, 2000.0);
      },
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
      // Unknown projection (Bonne) -> writes ESRI PE string!
      [](OGRSpatialReference& s) { s.SetBonne(45.0, 10.0, 0.0, 0.0); },
  };

  for (size_t i = 0; i < setters.size(); ++i) {
    CPLString path;
    path.Printf("/vsimem/gtwkt_proj_%zu.tif", i);
    MemGTIF m(path.c_str());

    OGRSpatialReference srs;
    srs.SetProjCS("CustomPCS");
    srs.SetGeogCS("CustomGeog", "CustomDatum", "CustomSpheroid", 6378137.0,
                  298.257223563);
    setters[i](srs);
    // Test feet linear units on LCC 2SP (i==22) to cover key 3059 with
    // ProjFalseOriginEastingGeoKey, and TM (i==0) to cover ProjFalseEasting!
    if (i == 0 || i == 22) {
      srs.SetLinearUnits(SRS_UL_FOOT, CPLAtof(SRS_UL_FOOT_CONV));
    }

    EXPECT_TRUE(
        GTIFSetFromOGISDefnEx(m.gtif, OGRSpatialReference::ToHandle(&srs),
                              GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_AUTO))
        << "index " << i;
    std::string wkt = m.ReadBackWkt();
    EXPECT_FALSE(wkt.empty()) << "index " << i;
  }

  // Mercator with BOTH ProjStdParallel1GeoKey AND ProjScaleAtNatOriginGeoKey +
  // ProjectionGeoKey == 1024 (Google Mercator extension), plus unsupported
  // CT_AlaskaConformal (29 -> default switch demotes to LOCAL_CS)!
  {
    MemGTIF m1("/vsimem/gtwkt_merc_both.tif");
    GTIFKeySet(m1.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeProjected);
    GTIFKeySet(m1.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, KvUserDefined);
    GTIFKeySet(m1.gtif, ProjectionGeoKey, TYPE_SHORT, 1, 1024);
    GTIFKeySet(m1.gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1, CT_Mercator);
    GTIFKeySet(m1.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    GTIFKeySet(m1.gtif, ProjStdParallel1GeoKey, TYPE_DOUBLE, 1, 0.0);
    GTIFKeySet(m1.gtif, ProjScaleAtNatOriginGeoKey, TYPE_DOUBLE, 1, 1.0);
    OGRSpatialReference out1 = m1.ReadBackOSR();
    EXPECT_TRUE(out1.IsProjected());

    MemGTIF m2("/vsimem/gtwkt_ct_default.tif");
    GTIFKeySet(m2.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeProjected);
    GTIFKeySet(m2.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, KvUserDefined);
    GTIFKeySet(m2.gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
               CT_AlaskaConformal);
    GTIFKeySet(m2.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    OGRSpatialReference out2 = m2.ReadBackOSR();
    EXPECT_TRUE(out2.IsLocal());
  }
}

TEST_F(GTWktSrsTest, LocalGeocentricImagineCitationsUTMAndDatumUnitsVariants) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 1. GTIFSetFromOGISDefn invalid WKT -> FALSE; valid WKT -> TRUE.
  {
    MemGTIF m("/vsimem/gtwkt_c_api.tif");
    EXPECT_FALSE(GTIFSetFromOGISDefn(m.gtif, "NOT_VALID_WKT[[["));
    EXPECT_TRUE(GTIFSetFromOGISDefn(m.gtif, SRS_WKT_WGS84_LAT_LONG));
  }

  // 2. Empty GeoKeys (nKeyCount == 0) -> empty OGRSpatialReference.
  {
    MemGTIF m("/vsimem/gtwkt_empty_keys.tif");
    OGRSpatialReference out = m.ReadBackOSR();
    EXPECT_TRUE(out.IsEmpty());
  }

  // 3. LOCAL_CS + Imagine citation cleanup ($ + \n) + Compound LOCAL_CS with
  // VerticalCitationGeoKey ("VCS Name = NAVD88 height|") and manual VertCS!
  {
    MemGTIF m1("/vsimem/gtwkt_local_img.tif");
    GTIFKeySet(m1.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, KvUserDefined);
    GTIFKeySet(
        m1.gtif, GTCitationGeoKey, TYPE_ASCII, 0,
        "IMAGINE GeoTIFF Support\nCopyright 1991 $Date$\nLocalGrid\nLine2");
    GTIFKeySet(m1.gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1, 9002);
    GTIFKeySet(m1.gtif, VerticalUnitsGeoKey, TYPE_SHORT, 1, 9003);
    GTIFKeySet(m1.gtif, VerticalDatumGeoKey, TYPE_SHORT, 1, KvUserDefined);
    GTIFKeySet(m1.gtif, VerticalCitationGeoKey, TYPE_ASCII, 0,
               "VCS Name = NAVD88 height|Extra");
    OGRSpatialReference out1 = m1.ReadBackOSR();
    EXPECT_TRUE(out1.IsCompound());

    // Write compound LOCAL_CS back via GTIFSetFromOGISDefnEx!
    MemGTIF m2("/vsimem/gtwkt_local_write.tif");
    EXPECT_TRUE(
        GTIFSetFromOGISDefnEx(m2.gtif, OGRSpatialReference::ToHandle(&out1),
                              GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_0));
    EXPECT_FALSE(m2.ReadBackWkt().empty());

    // Local CS with KvUserDefined linear unit + VerticalDatumGeoKey = 5103.
    MemGTIF m3("/vsimem/gtwkt_local_vdatum.tif");
    GTIFKeySet(m3.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, KvUserDefined);
    GTIFKeySet(m3.gtif, GeogCitationGeoKey, TYPE_ASCII, 0, "LocalFromGeog");
    GTIFKeySet(m3.gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1, KvUserDefined);
    GTIFKeySet(m3.gtif, ProjLinearUnitSizeGeoKey, TYPE_DOUBLE, 1, 2.0);
    GTIFKeySet(m3.gtif, VerticalUnitsGeoKey, TYPE_SHORT, 1, 9001);
    GTIFKeySet(m3.gtif, VerticalDatumGeoKey, TYPE_SHORT, 1, 5103);
    EXPECT_TRUE(m3.ReadBackOSR().IsCompound());
  }

  // 4. ModelTypeGeocentric (known unit 9001 & user-defined unit).
  {
    OGRSpatialReference geoc;
    geoc.SetGeocCS("MyGeocentric");
    geoc.SetWellKnownGeogCS("WGS84");
    geoc.SetLinearUnits("CustomLink", 0.201168);

    MemGTIF m("/vsimem/gtwkt_geoc.tif");
    EXPECT_TRUE(
        GTIFSetFromOGISDefnEx(m.gtif, OGRSpatialReference::ToHandle(&geoc),
                              GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_0));
    OGRSpatialReference out = m.ReadBackOSR();
    EXPECT_TRUE(out.IsGeocentric());
  }

  // 5. UTM Zone special-casing on Write (NAD83, NAD27, WGS84 N & S, WGS72)!
  for (const char* datum : {"NAD83", "NAD27", "WGS84", "WGS72"}) {
    for (int north : {TRUE, FALSE}) {
      if (!EQUAL(datum, "WGS84") && !north) continue;
      // A custom PROJCS name plus SetUTM yields no PROJCS AUTHORITY, so the
      // writer sees nPCS == KvUserDefined and takes the UTM special-casing.
      OGRSpatialReference utm;
      utm.SetProjCS("CustomUTM");
      utm.SetWellKnownGeogCS(datum);
      utm.SetUTM(10, north);
      CPLString p;
      p.Printf("/vsimem/gtwkt_utm_%s_%d.tif", datum, north);
      MemGTIF m(p.c_str());
      EXPECT_TRUE(
          GTIFSetFromOGISDefnEx(m.gtif, OGRSpatialReference::ToHandle(&utm),
                                GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_0));
      EXPECT_TRUE(m.ReadBackOSR().IsProjected());
    }
  }

  // 6. OGCDatumName2EPSGDatumCode + WKTMassageDatum + custom angular units
  // (arc-second, arc-minute, grad, gon, radian, custom) + sphere vs oblate!
  {
    const char* wkt_mgi =
        "GEOGCS[\"CustomMGI\",DATUM[\"Militar_Geographische_Institute\","
        "SPHEROID[\"Bessel 1841\",6377397.155,299.1528128]],"
        "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";
    MemGTIF m_mgi("/vsimem/gtwkt_mgi.tif");
    EXPECT_TRUE(GTIFSetFromOGISDefn(m_mgi.gtif, wkt_mgi));

    const struct {
      const char* name;
      double rad;
    } kAngUnits[] = {
        {"arc-second", 4.84813681109536e-06},
        {"arc-minute", 0.0002908882086657216},
        {"grad", 0.015707963267948967},
        {"gon", 0.015707963267948967},
        {"radian", 1.0},
        {"my_angle", 0.02},
    };
    for (size_t i = 0; i < 6; ++i) {
      OGRSpatialReference gsrs;
      // Sphere (invFlattening == 0) on even i, oblate on odd i!
      gsrs.SetGeogCS("CustomGCS", "TotallyUnknownDatum_123", "CustomSph",
                     6371000.0, (i % 2 == 0) ? 0.0 : 298.3, "Paris", 2.337,
                     kAngUnits[i].name, kAngUnits[i].rad);
      // Add projected layer on grad (i==2) to exercise angular param conversion
      // in both writer (L2271) and reader (L1421, L1878)!
      if (i == 2) {
        gsrs.SetProjCS("GradLCC");
        gsrs.SetLCC1SP(52.0, 0.0, 0.99987742, 600000.0, 2200000.0);
        gsrs.SetLinearUnits("CustomMeter", 1.05);
      }
      CPLString p;
      p.Printf("/vsimem/gtwkt_ang_%zu.tif", i);
      MemGTIF m(p.c_str());
      EXPECT_TRUE(
          GTIFSetFromOGISDefnEx(m.gtif, OGRSpatialReference::ToHandle(&gsrs),
                                GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_0));
      EXPECT_FALSE(m.ReadBackWkt().empty());
    }
  }
}

TEST_F(GTWktSrsTest, Compound3DESRIPEConfigOptionsAndEdgeCases) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 1. EPSG:3857 with GEOTIFF_KEYS_ESRI_PE -> writes ESRI PE String +
  // WGS_1984_Web_Mercator_Auxiliary_Sphere, then reads back via L604-621!
  {
    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    MemGTIF m("/vsimem/gtwkt_3857_esri.tif");
    EXPECT_TRUE(
        GTIFSetFromOGISDefnEx(m.gtif, OGRSpatialReference::ToHandle(&srs3857),
                              GEOTIFF_KEYS_ESRI_PE, GEOTIFF_VERSION_1_0));
    OGRSpatialReference out = m.ReadBackOSR();
    EXPECT_TRUE(out.IsProjected());
  }

  // 2. Geographic 3D (EPSG:4979 & EPSG:4937) in GeoTIFF 1.0 vs 1.1 +
  // CoordinateEpochGeoKey + Compound CRS (EPSG:5498 = NAD83 + NAVD88)!
  for (int code : {4979, 4937, 5498}) {
    for (GeoTIFFVersionEnum ver :
         {GEOTIFF_VERSION_1_0, GEOTIFF_VERSION_1_1, GEOTIFF_VERSION_AUTO}) {
      OGRSpatialReference srs;
      srs.importFromEPSG(code);
      srs.SetCoordinateEpoch(2020.5);
      CPLString p;
      p.Printf("/vsimem/gtwkt_3d_%d_%d.tif", code, static_cast<int>(ver));
      MemGTIF m(p.c_str());
      EXPECT_TRUE(GTIFSetFromOGISDefnEx(m.gtif,
                                        OGRSpatialReference::ToHandle(&srs),
                                        GEOTIFF_KEYS_STANDARD, ver));
      OGRSpatialReference out = m.ReadBackOSR();
      EXPECT_FALSE(out.IsEmpty());
      EXPECT_NEAR(out.GetCoordinateEpoch(), 2020.5, 1e-4);
    }
  }

  // 3. GeoTIFF 1.0 legacy vertical codes (5103 -> 5703, 5171 -> 5773,
  // 5030 -> 4979) and private vertical code > 32767 warning!
  for (int vcode : {5103, 5171, 5030, 32768, 4326}) {
    CPLString p;
    p.Printf("/vsimem/gtwkt_legacy_v_%d.tif", vcode);
    MemGTIF m(p.c_str());
    GTIFKeySet(m.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeGeographic);
    GTIFKeySet(m.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    GTIFKeySet(m.gtif, VerticalCSTypeGeoKey, TYPE_SHORT, 1, vcode);
    GTIFKeySet(m.gtif, VerticalUnitsGeoKey, TYPE_SHORT, 1, 9001);
    EXPECT_FALSE(m.ReadBackOSR().IsEmpty());
  }

  // 4. Custom Compound CRS without EPSG vertical code in GeoTIFF 1.1!
  {
    OGRSpatialReference horiz, vert, compd;
    horiz.importFromEPSG(32611);
    vert.SetVertCS("MyCustomVert", "MyCustomVDatum", 2005);
    vert.SetLinearUnits(SRS_UL_FOOT, CPLAtof(SRS_UL_FOOT_CONV));
    compd.SetCompoundCS("CustomCompd", &horiz, &vert);

    MemGTIF m("/vsimem/gtwkt_custom_compd_11.tif");
    EXPECT_TRUE(
        GTIFSetFromOGISDefnEx(m.gtif, OGRSpatialReference::ToHandle(&compd),
                              GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_1));
    EXPECT_TRUE(m.ReadBackOSR().IsCompound());
  }

  // 5. Projected 3D CRS demotion on write (L2247)!
  {
    OGRSpatialReference p3d;
    p3d.importFromEPSG(32611);
    p3d.PromoteTo3D(nullptr);
    MemGTIF m("/vsimem/gtwkt_proj3d.tif");
    EXPECT_TRUE(
        GTIFSetFromOGISDefnEx(m.gtif, OGRSpatialReference::ToHandle(&p3d),
                              GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_1));
    EXPECT_FALSE(m.ReadBackOSR().IsEmpty());
  }

  // 6. GTIFF_SRS_SOURCE ("", "EPSG", "GEOKEYS") on inconsistent GeogCS &
  // inconsistent ProjCS, plus GTIFF_LINEAR_UNITS=BROKEN, plus TOWGS84 3 & 7
  // params, plus ESRI underscore name morphing & special Web Mercator names!
  for (const char* src_opt : {"", "EPSG", "GEOKEYS"}) {
    CPLConfigOptionSetter cfg("GTIFF_SRS_SOURCE",
                              src_opt[0] ? src_opt : nullptr, false);
    // Inconsistent GeogCS (4326 with wrong SemiMajor = 6370000).
    CPLString p1;
    p1.Printf("/vsimem/gtwkt_inc_geog_%s.tif", src_opt);
    MemGTIF m1(p1.c_str());
    GTIFKeySet(m1.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeGeographic);
    GTIFKeySet(m1.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    GTIFKeySet(m1.gtif, GeogSemiMajorAxisGeoKey, TYPE_DOUBLE, 1, 6370000.0);
    EXPECT_FALSE(m1.ReadBackOSR().IsEmpty());

    // Inconsistent ProjCS (32611 with wrong FalseEasting = 12345).
    CPLString p2;
    p2.Printf("/vsimem/gtwkt_inc_proj_%s.tif", src_opt);
    MemGTIF m2(p2.c_str());
    GTIFKeySet(m2.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeProjected);
    GTIFKeySet(m2.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, 32611);
    GTIFKeySet(m2.gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
               CT_TransverseMercator);
    GTIFKeySet(m2.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    GTIFKeySet(m2.gtif, ProjNatOriginLongGeoKey, TYPE_DOUBLE, 1, -117.0);
    GTIFKeySet(m2.gtif, ProjScaleAtNatOriginGeoKey, TYPE_DOUBLE, 1, 0.9996);
    GTIFKeySet(m2.gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1, 12345.0);
    EXPECT_FALSE(m2.ReadBackOSR().IsEmpty());
  }

  // GTIFF_LINEAR_UNITS=BROKEN + Fast-path PCS with overridden linear unit!
  {
    CPLConfigOptionSetter broken("GTIFF_LINEAR_UNITS", "BROKEN", false);
    MemGTIF m1("/vsimem/gtwkt_broken_lu.tif");
    GTIFKeySet(m1.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeProjected);
    GTIFKeySet(m1.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, KvUserDefined);
    GTIFKeySet(m1.gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
               CT_TransverseMercator);
    GTIFKeySet(m1.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, 4326);
    GTIFKeySet(m1.gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1, 9002);
    GTIFKeySet(m1.gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1, 1000.0);
    GTIFKeySet(m1.gtif, ProjFalseNorthingGeoKey, TYPE_DOUBLE, 1, 2000.0);
    GTIFKeySet(m1.gtif, ProjFalseOriginEastingGeoKey, TYPE_DOUBLE, 1, 3000.0);
    GTIFKeySet(m1.gtif, ProjFalseOriginNorthingGeoKey, TYPE_DOUBLE, 1, 4000.0);
    GTIFKeySet(m1.gtif, ProjCenterEastingGeoKey, TYPE_DOUBLE, 1, 5000.0);
    GTIFKeySet(m1.gtif, ProjCenterNorthingGeoKey, TYPE_DOUBLE, 1, 6000.0);
    EXPECT_TRUE(m1.ReadBackOSR().IsProjected());

    MemGTIF m2("/vsimem/gtwkt_pcs_foot_override.tif");
    GTIFKeySet(m2.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeProjected);
    GTIFKeySet(m2.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, 32611);
    GTIFKeySet(m2.gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1, 9002);
    EXPECT_TRUE(m2.ReadBackOSR().IsProjected());
  }

  // TOWGS84 (3-param and 7-param) with GTIFF_WRITE_TOWGS84=YES &
  // OSR_STRIP_TOWGS84=NO.
  {
    CPLConfigOptionSetter wt("GTIFF_WRITE_TOWGS84", "YES", false);
    CPLConfigOptionSetter st("OSR_STRIP_TOWGS84", "NO", false);
    for (int seven : {0, 1}) {
      OGRSpatialReference srs;
      srs.SetGeogCS("CustomT", "CustomDT", "CustomET", 6378137.0,
                    298.257223563);
      if (seven) {
        srs.SetTOWGS84(1.0, 2.0, 3.0, 0.1, 0.2, 0.3, 0.4);
      } else {
        srs.SetTOWGS84(10.0, 20.0, 30.0);
      }
      CPLString p;
      p.Printf("/vsimem/gtwkt_towgs84_%d.tif", seven);
      MemGTIF m(p.c_str());
      EXPECT_TRUE(
          GTIFSetFromOGISDefnEx(m.gtif, OGRSpatialReference::ToHandle(&srs),
                                GEOTIFF_KEYS_STANDARD, GEOTIFF_VERSION_1_0));
      double params[7] = {};
      EXPECT_EQ(m.ReadBackOSR().GetTOWGS84(params, 7), OGRERR_NONE);
    }
  }

  // ESRI underscore morphing, INF inv flattening, WGS_1984_Web_Mercator,
  // Google Maps Global Mercator, and DB name lookup!
  const char* const kPcsCitations[] = {
      "WGS_1984_Web_Mercator", "Google Maps Global Mercator",
      "WGS_1984_UTM_Zone_11N", "WGS 84 / UTM zone 11N"};
  for (size_t i = 0; i < sizeof(kPcsCitations) / sizeof(kPcsCitations[0]);
       ++i) {
    const char* pcs_cit = kPcsCitations[i];
    SCOPED_TRACE(pcs_cit);
    CPLString p;
    p.Printf("/vsimem/gtwkt_special_%zu.tif", i);
    MemGTIF m(p.c_str());
    GTIFKeySet(m.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeProjected);
    GTIFKeySet(m.gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, KvUserDefined);
    GTIFKeySet(m.gtif, GTCitationGeoKey, TYPE_ASCII, 0, pcs_cit);
    GTIFKeySet(m.gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
               CT_TransverseMercator);
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

  // INFINITY GeogInvFlatteningGeoKey + 0 GeogAngularUnitSizeGeoKey warning!
  {
    MemGTIF m("/vsimem/gtwkt_inf_invf.tif");
    GTIFKeySet(m.gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeGeographic);
    GTIFKeySet(m.gtif, GeographicTypeGeoKey, TYPE_SHORT, 1, KvUserDefined);
    GTIFKeySet(m.gtif, GeogCitationGeoKey, TYPE_ASCII, 0,
               "GCS Name = CustomSph|Ellipsoid = Sph|AUnits = bad|");
    GTIFKeySet(m.gtif, GeogSemiMajorAxisGeoKey, TYPE_DOUBLE, 1, 6371000.0);
    GTIFKeySet(m.gtif, GeogInvFlatteningGeoKey, TYPE_DOUBLE, 1,
               std::numeric_limits<double>::infinity());
    GTIFKeySet(m.gtif, GeogAngularUnitSizeGeoKey, TYPE_DOUBLE, 1, 0.0);
    EXPECT_TRUE(m.ReadBackOSR().IsGeographic());
  }
}

TEST_F(GTWktSrsTest, MemBufFunctionsAllGeorefModes) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 1. Corrupt buffer -> CE_Failure!
  {
    unsigned char bad[4] = {'B', 'A', 'D', '!'};
    char* wkt = nullptr;
    double gt[6] = {};
    int ngcp = 0;
    GDAL_GCP* gcps = nullptr;
    EXPECT_EQ(GTIFWktFromMemBuf(4, bad, &wkt, gt, &ngcp, &gcps), CE_Failure);
  }

  // 2. GTIFMemBufFromWkt + GTIFWktFromMemBuf: north-up geotransform!
  {
    const double gt_in[6] = {100.0, 2.0, 0.0, 500.0, 0.0, -2.0};
    int sz = 0;
    unsigned char* buf = nullptr;
    ASSERT_EQ(
        GTIFMemBufFromWkt(SRS_WKT_WGS84_LAT_LONG, gt_in, 0, nullptr, &sz, &buf),
        CE_None);
    ASSERT_GT(sz, 0);
    ASSERT_NE(buf, nullptr);

    char* wkt_out = nullptr;
    double gt_out[6] = {};
    int ngcp = 0;
    GDAL_GCP* gcps = nullptr;
    EXPECT_EQ(GTIFWktFromMemBuf(sz, buf, &wkt_out, gt_out, &ngcp, &gcps),
              CE_None);
    EXPECT_NE(wkt_out, nullptr);
    EXPECT_DOUBLE_EQ(gt_out[0], 100.0);
    CPLFree(wkt_out);
    CPLFree(buf);
  }

  // 3. GTIFMemBufFromSRS + GTIFWktFromMemBufEx:
  // (a) North-up + bPixelIsPoint=TRUE + RPC metadata!
  {
    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    const double gt_in[6] = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
    CPLString coeffs = "1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0";
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
    rpc.SetNameValue("LINE_NUM_COEFF", coeffs.c_str());
    rpc.SetNameValue("LINE_DEN_COEFF", coeffs.c_str());
    rpc.SetNameValue("SAMP_NUM_COEFF", coeffs.c_str());
    rpc.SetNameValue("SAMP_DEN_COEFF", coeffs.c_str());

    int sz = 0;
    unsigned char* buf = nullptr;
    ASSERT_EQ(GTIFMemBufFromSRS(OGRSpatialReference::ToHandle(&srs), gt_in, 0,
                                nullptr, &sz, &buf, /*bPixelIsPoint=*/TRUE,
                                rpc.List()),
              CE_None);

    OGRSpatialReferenceH out_srs = nullptr;
    double gt_out[6] = {};
    int ngcp = 0;
    GDAL_GCP* gcps = nullptr;
    int pip = FALSE;
    char** rpc_out = nullptr;
    EXPECT_EQ(GTIFWktFromMemBufEx(sz, buf, &out_srs, gt_out, &ngcp, &gcps, &pip,
                                  &rpc_out),
              CE_None);
    EXPECT_TRUE(pip);
    EXPECT_DOUBLE_EQ(gt_out[0], 10.0);
    EXPECT_NE(rpc_out, nullptr);
    OSRDestroySpatialReference(out_srs);
    CSLDestroy(rpc_out);
    CPLFree(buf);
  }

  // (b) Rotated geotransform (`TIFFTAG_GEOTRANSMATRIX`) + bPixelIsPoint=TRUE!
  {
    OGRSpatialReference srs;
    srs.importFromEPSG(32611);
    const double gt_rot[6] = {1000.0, 1.0, 0.25, 2000.0, 0.25, -1.0};
    int sz = 0;
    unsigned char* buf = nullptr;
    ASSERT_EQ(
        GTIFMemBufFromSRS(OGRSpatialReference::ToHandle(&srs), gt_rot, 0,
                          nullptr, &sz, &buf, /*bPixelIsPoint=*/TRUE, nullptr),
        CE_None);

    OGRSpatialReferenceH out_srs = nullptr;
    double gt_out[6] = {};
    int ngcp = 0;
    GDAL_GCP* gcps = nullptr;
    int pip = FALSE;
    EXPECT_EQ(GTIFWktFromMemBufEx(sz, buf, &out_srs, gt_out, &ngcp, &gcps, &pip,
                                  nullptr),
              CE_None);
    // This pins an upstream asymmetry: for bPixelIsPoint=TRUE,
    // GTIFMemBufFromSRS shifts adfMatrix[3]/[7] by half a pixel
    // (gt[1] + gt[2]) * 0.5 = (1.0 + 0.25) * 0.5 = 0.625 before writing
    // TIFFTAG_GEOTRANSMATRIX, but GTIFWktFromMemBufEx only undoes that shift
    // on the tiepoint path and copies adfMatrix[3] back verbatim here. The
    // origin therefore reads back as 1000.625 rather than 1000.0.
    EXPECT_NEAR(gt_out[0], 1000.625, 1e-6);
    EXPECT_NEAR(gt_out[2], 0.25, 1e-6);
    OSRDestroySpatialReference(out_srs);
    CPLFree(buf);
  }

  // (c) GCPs (`nGCPCount = 2` with identity GT)!
  {
    GDAL_GCP gcps_in[2];
    GDALInitGCPs(2, gcps_in);
    gcps_in[0].dfGCPPixel = 0.0;
    gcps_in[0].dfGCPLine = 0.0;
    gcps_in[0].dfGCPX = 10.0;
    gcps_in[0].dfGCPY = 20.0;
    gcps_in[1].dfGCPPixel = 10.0;
    gcps_in[1].dfGCPLine = 10.0;
    gcps_in[1].dfGCPX = 30.0;
    gcps_in[1].dfGCPY = 40.0;
    const double gt_id[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    int sz = 0;
    unsigned char* buf = nullptr;
    ASSERT_EQ(
        GTIFMemBufFromWkt(SRS_WKT_WGS84_LAT_LONG, gt_id, 2, gcps_in, &sz, &buf),
        CE_None);
    GDALDeinitGCPs(2, gcps_in);

    char* wkt_out = nullptr;
    double gt_out[6] = {};
    int ngcp_out = 0;
    GDAL_GCP* gcps_out = nullptr;
    EXPECT_EQ(
        GTIFWktFromMemBuf(sz, buf, &wkt_out, gt_out, &ngcp_out, &gcps_out),
        CE_None);
    EXPECT_EQ(ngcp_out, 2);
    GDALDeinitGCPs(ngcp_out, gcps_out);
    CPLFree(gcps_out);
    CPLFree(wkt_out);
    CPLFree(buf);
  }
}

}  // namespace
}  // namespace autotest2
