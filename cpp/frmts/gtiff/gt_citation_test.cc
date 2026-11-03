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
// Tests GeoTIFF ESRI/IMAGINE citation translation, parsing, SRS unit/datum
// extraction, and UTM/StatePlane citation helpers (gt_citation.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gt_citation.cpp

#include "third_party/gdal/frmts/gtiff/gt_citation.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/ogr/ogr_proj_p.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr short kKvUserDefined = 32767;

struct CitationListDeleter {
  void operator()(char** p) const {
    if (p != nullptr) {
      for (int i = 0; i < nCitationNameTypes; ++i) {
        CPLFree(p[i]);
      }
      CPLFree(p);
    }
  }
};

using CitationListPtr = std::unique_ptr<char*, CitationListDeleter>;

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

  ~MemGTIF() {
    if (gtif != nullptr) GTIFFree(gtif);
    if (tif != nullptr) TIFFClose(tif);
    if (fp != nullptr) VSIFCloseL(fp);
    VSIUnlink(path.c_str());
  }
};

class GTCitationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GTiffOneTimeInit();
    LibgeotiffOneTimeInit();
    CPLErrorReset();
  }

  void TearDown() override { CPLErrorReset(); }
};

TEST_F(GTCitationTest, ImagineCitationTranslationAllBranches) {
  EXPECT_EQ(nullptr, ImagineCitationTranslation(nullptr, PCSCitationGeoKey));

  char non_imagine[] = "Regular Citation String";
  EXPECT_EQ(nullptr,
            ImagineCitationTranslation(non_imagine, PCSCitationGeoKey));

  // 1. Imagine string without '$' -> p == nullptr, still extracts inline keys.
  {
    char s[] =
        "IMAGINE GeoTIFF Support\nNAD = NAD83\nDatum = North American   \n"
        "Ellipsoid = GRS 1980\nUnits = meters   ";
    std::unique_ptr<char, CplFreeDeleter> res(
        ImagineCitationTranslation(s, PCSCitationGeoKey));
    ASSERT_NE(nullptr, res);
    EXPECT_THAT(res.get(), HasSubstr("NAD = NAD83|"));
    EXPECT_THAT(res.get(), HasSubstr("Datum = North American|"));
    EXPECT_THAT(res.get(), HasSubstr("Ellipsoid = GRS 1980|"));
    EXPECT_THAT(res.get(), HasSubstr("LUnits = meters|"));
  }

  // 2. Imagine with '$' and '\n', PCSCitationGeoKey WITH "Projection = " and
  // trailing spaces (p1 stops at first '\n', so "Projection = " must be on the
  // first line after "$Id$\n").
  {
    char s[] =
        "IMAGINE GeoTIFF Support\n$Id$\nProjection = Transverse Mercator   \n"
        "Units = feet\n";
    std::unique_ptr<char, CplFreeDeleter> res(
        ImagineCitationTranslation(s, PCSCitationGeoKey));
    ASSERT_NE(nullptr, res);
    EXPECT_THAT(res.get(), HasSubstr("PRJ Name = Transverse Mercator|"));
    EXPECT_THAT(res.get(), HasSubstr("LUnits = feet|"));
  }

  // 3. PCSCitationGeoKey WITHOUT "Projection = " (with "Projection Name = ").
  {
    char s[] =
        "IMAGINE GeoTIFF Support\n$Id$\nProjection Name = MyPCS   \n"
        "Datum = WGS84";
    std::unique_ptr<char, CplFreeDeleter> res(
        ImagineCitationTranslation(s, PCSCitationGeoKey));
    ASSERT_NE(nullptr, res);
    EXPECT_THAT(res.get(), HasSubstr("PCS Name = MyPCS|"));
  }

  // 4. GTCitationGeoKey without '\n' after '$', inline key termination.
  {
    char s[] =
        "IMAGINE GeoTIFF Support $MyGTCit   NAD = 27 Units = us_survey_feet";
    std::unique_ptr<char, CplFreeDeleter> res(
        ImagineCitationTranslation(s, GTCitationGeoKey));
    ASSERT_NE(nullptr, res);
    EXPECT_THAT(res.get(), HasSubstr("NAD = 27|"));
    EXPECT_THAT(res.get(), HasSubstr("LUnits = us_survey_feet|"));
  }

  // 5. GeogCitationGeoKey without "Unable to" vs with "Unable to", plus default
  // keyID (VerticalCitationGeoKey).
  {
    char s1[] = "IMAGINE GeoTIFF Support\n$Id$\nWGS 84   \nEllipsoid = WGS84";
    std::unique_ptr<char, CplFreeDeleter> res1(
        ImagineCitationTranslation(s1, GeogCitationGeoKey));
    ASSERT_NE(nullptr, res1);
    EXPECT_THAT(res1.get(), HasSubstr("GCS Name = WGS 84|"));

    char s2[] = "IMAGINE GeoTIFF Support\n$Id$\nUnable to match EPSG\n";
    std::unique_ptr<char, CplFreeDeleter> res2(
        ImagineCitationTranslation(s2, GeogCitationGeoKey));
    EXPECT_EQ(nullptr, res2);

    char s3[] = "IMAGINE GeoTIFF Support\n$Id$\nSomeVert   \nUnits = meters";
    std::unique_ptr<char, CplFreeDeleter> res3(
        ImagineCitationTranslation(s3, VerticalCitationGeoKey));
    ASSERT_NE(nullptr, res3);
    EXPECT_THAT(res3.get(), HasSubstr("LUnits = meters|"));
  }
}

TEST_F(GTCitationTest, CitationStringParseAndSetLinearUnitCitation) {
  EXPECT_EQ(nullptr, CitationStringParse(nullptr, PCSCitationGeoKey));

  char empty_str[] = "";
  EXPECT_EQ(nullptr, CitationStringParse(empty_str, GeogCitationGeoKey));

  char one_char[] = "x";
  EXPECT_EQ(nullptr, CitationStringParse(one_char, GeogCitationGeoKey));

  // 1. Plain string >= 2 chars: GeogCitationGeoKey sets CitGcsName; PCS
  // returns nullptr.
  {
    char plain[] = "MyCustomGCS";
    CitationListPtr gcs_ret(CitationStringParse(plain, GeogCitationGeoKey));
    ASSERT_NE(nullptr, gcs_ret);
    EXPECT_STREQ("MyCustomGCS", gcs_ret.get()[CitGcsName]);

    EXPECT_EQ(nullptr, CitationStringParse(plain, PCSCitationGeoKey));
  }

  // 2. Full pipe-delimited string with all 8 keys (plus duplicate ignored).
  {
    char full[] =
        "PCS Name = P1|PCS Name = P2|PRJ Name = TM|LUnits = m|GCS Name = G1|"
        "Datum = D1|Ellipsoid = E1|Primem = PM1|AUnits = rad";
    CitationListPtr ret(CitationStringParse(full, PCSCitationGeoKey));
    ASSERT_NE(nullptr, ret);
    EXPECT_STREQ("P1", ret.get()[CitPcsName]);
    EXPECT_STREQ("TM", ret.get()[CitProjectionName]);
    EXPECT_STREQ("m", ret.get()[CitLUnitsName]);
    EXPECT_STREQ("G1", ret.get()[CitGcsName]);
    EXPECT_STREQ("D1", ret.get()[CitDatumName]);
    EXPECT_STREQ("E1", ret.get()[CitEllipsoidName]);
    EXPECT_STREQ("PM1", ret.get()[CitPrimemName]);
    EXPECT_STREQ("rad", ret.get()[CitAUnitsName]);
  }

  // 3. SetLinearUnitCitation: empty, existing without '|', existing with '|'.
  std::map<geokey_t, std::string> ascii_keys;
  SetLinearUnitCitation(ascii_keys, "Foot_US");
  EXPECT_EQ("LUnits = Foot_US", ascii_keys[PCSCitationGeoKey]);

  SetLinearUnitCitation(ascii_keys, "meters");
  EXPECT_EQ("LUnits = Foot_US|LUnits = meters|", ascii_keys[PCSCitationGeoKey]);

  SetLinearUnitCitation(ascii_keys, "yard");
  EXPECT_EQ("LUnits = Foot_US|LUnits = meters|LUnits = yard|",
            ascii_keys[PCSCitationGeoKey]);
}

TEST_F(GTCitationTest, SetGeogCSCitationAndGetGeogCSFromCitation) {
  MemGTIF m("/vsimem/gtcit_geog.tif");

  std::map<geokey_t, std::string> keys;
  OGRSpatialReference srs;
  srs.SetGeogCS("MyGeog", "MyDatum", "MyEllipsoid", 6378137.0, 298.257223563,
                "Paris", 2.33722917, "grad", 0.015707963267948967);

  // 1. Missing GeogCitationGeoKey -> early return.
  SetGeogCSCitation(m.gtif, keys, &srs, "grad", kKvUserDefined, kKvUserDefined);
  EXPECT_TRUE(keys.empty());

  // 2. Empty GeogCitationGeoKey -> early return.
  keys[GeogCitationGeoKey] = "";
  SetGeogCSCitation(m.gtif, keys, &srs, "grad", kKvUserDefined, kKvUserDefined);
  EXPECT_EQ("", keys[GeogCitationGeoKey]);

  // 3. Without "GCS Name = " prefix, kKvUserDefined datum & spheroid,
  // non-Degree angular unit -> rewrites all fields and sets
  // GeogPrimeMeridianLongGeoKey.
  keys[GeogCitationGeoKey] = "MyGeog";
  SetGeogCSCitation(m.gtif, keys, &srs, "grad", kKvUserDefined, kKvUserDefined);
  EXPECT_THAT(keys[GeogCitationGeoKey],
              HasSubstr("GCS Name = MyGeog|Datum = MyDatum|"));
  EXPECT_THAT(keys[GeogCitationGeoKey], HasSubstr("Ellipsoid = MyEllipsoid|"));
  EXPECT_THAT(keys[GeogCitationGeoKey],
              HasSubstr("Primem = Paris|AUnits = grad|"));

  // 4. Already starts with "GCS Name = ", Degree unit, known datum/spheroid,
  // trailing '|'.
  keys[GeogCitationGeoKey] = "GCS Name = Existing|";
  SetGeogCSCitation(m.gtif, keys, &srs, "Degree", 6326, 7030);
  EXPECT_THAT(keys[GeogCitationGeoKey],
              HasSubstr("GCS Name = Existing||Primem = Paris|"));

  // 5. Empty SRS (no PRIMEM, Degree, known datum/spheroid) -> bRewrite false.
  OGRSpatialReference empty_srs;
  keys[GeogCitationGeoKey] = "Unchanged";
  SetGeogCSCitation(m.gtif, keys, &empty_srs, "Degree", 6326, 7030);
  EXPECT_EQ("Unchanged", keys[GeogCitationGeoKey]);

  // 6. GetGeogCSFromCitation: pipe-delimited all 5 fields & Imagine format.
  {
    std::string buf =
        "GCS Name = G1|Datum = D1|Ellipsoid = E1|Primem = P1|AUnits = A1|";
    char* raw_geog = nullptr;
    char* raw_datum = nullptr;
    char* raw_pm = nullptr;
    char* raw_sph = nullptr;
    char* raw_au = nullptr;
    GetGeogCSFromCitation(buf.data(), static_cast<int>(buf.size() + 1),
                          GeogCitationGeoKey, &raw_geog, &raw_datum, &raw_pm,
                          &raw_sph, &raw_au);
    std::unique_ptr<char, CplFreeDeleter> geog(raw_geog);
    std::unique_ptr<char, CplFreeDeleter> datum(raw_datum);
    std::unique_ptr<char, CplFreeDeleter> pm(raw_pm);
    std::unique_ptr<char, CplFreeDeleter> sph(raw_sph);
    std::unique_ptr<char, CplFreeDeleter> au(raw_au);
    EXPECT_STREQ("G1", geog.get());
    EXPECT_STREQ("D1", datum.get());
    EXPECT_STREQ("E1", sph.get());
    EXPECT_STREQ("P1", pm.get());
    EXPECT_STREQ("A1", au.get());
  }

  {
    std::string buf = "IMAGINE GeoTIFF Support\n$Id$\nMyImgGCS\nDatum = ImgD";
    char* raw_geog = nullptr;
    char* raw_datum = nullptr;
    char* raw_pm = nullptr;
    char* raw_sph = nullptr;
    char* raw_au = nullptr;
    GetGeogCSFromCitation(buf.data(), static_cast<int>(buf.size() + 1),
                          GeogCitationGeoKey, &raw_geog, &raw_datum, &raw_pm,
                          &raw_sph, &raw_au);
    std::unique_ptr<char, CplFreeDeleter> geog(raw_geog);
    std::unique_ptr<char, CplFreeDeleter> datum(raw_datum);
    std::unique_ptr<char, CplFreeDeleter> pm(raw_pm);
    std::unique_ptr<char, CplFreeDeleter> sph(raw_sph);
    std::unique_ptr<char, CplFreeDeleter> au(raw_au);
    EXPECT_STREQ("MyImgGCS", geog.get());
    EXPECT_STREQ("ImgD", datum.get());
  }
}

TEST_F(GTCitationTest, SetCitationToSRSAllBranches) {
  MemGTIF m("/vsimem/gtcit_setsrs.tif");
  GTIFKeySet(m.gtif, ProjLinearUnitSizeGeoKey, TYPE_DOUBLE, 1, 0.5);

  // 1. Empty SRS: resets *linearUnitIsSet to FALSE, then Imagine string sets
  // known unit "yards" so SetLinearUnits sets lu_set = TRUE. Returns FALSE
  // because no "PCS Name = " is present.
  {
    OGRSpatialReference srs;
    OGRBoolean lu_set = TRUE;
    char cit[256] =
        "IMAGINE GeoTIFF Support\n$Id$\nProjection = Mercator\nUnits = yards";
    EXPECT_EQ(FALSE, SetCitationToSRS(m.gtif, cit, sizeof(cit),
                                      PCSCitationGeoKey, &srs, &lu_set));
    EXPECT_EQ(TRUE, lu_set);
  }

  // 2. SRS with existing linear unit, custom unit not in apszUnitMap ("furlong"
  // -> reads ProjLinearUnitSizeGeoKey = 0.5 from hGTIF), and CitPcsName set.
  {
    OGRSpatialReference srs;
    srs.SetProjCS("InitialPCS");
    srs.SetLinearUnits("metre", 1.0);
    OGRBoolean lu_set = FALSE;
    char cit[256] = "PCS Name = UpdatedPCS|LUnits = furlong|";
    EXPECT_EQ(TRUE, SetCitationToSRS(m.gtif, cit, sizeof(cit),
                                     PCSCitationGeoKey, &srs, &lu_set));
    EXPECT_EQ(TRUE, lu_set);
    EXPECT_DOUBLE_EQ(0.5, srs.GetLinearUnits(nullptr));
  }

  // 3. GTCitationGeoKey without "PCS Name = ".
  {
    OGRSpatialReference srs1;
    OGRBoolean lu = FALSE;
    char cit1[128] = "Projected Coordinates";
    EXPECT_EQ(TRUE, SetCitationToSRS(m.gtif, cit1, sizeof(cit1),
                                     GTCitationGeoKey, &srs1, &lu));

    OGRSpatialReference srs2;
    char cit2[128] = "CustomGTCitationName";
    EXPECT_EQ(TRUE, SetCitationToSRS(m.gtif, cit2, sizeof(cit2),
                                     GTCitationGeoKey, &srs2, &lu));
    EXPECT_STREQ("CustomGTCitationName", srs2.GetAttrValue("PROJCS"));

    OGRSpatialReference srs3;
    srs3.SetNode("PROJCS", "unnamed");
    char cit3[128] = "NamedFromGT";
    EXPECT_EQ(TRUE, SetCitationToSRS(m.gtif, cit3, sizeof(cit3),
                                     GTCitationGeoKey, &srs3, &lu));
    EXPECT_STREQ("NamedFromGT", srs3.GetAttrValue("PROJCS"));
  }
}

TEST_F(GTCitationTest, CheckCitationKeyForStatePlaneUTMAndCheckUTM) {
  // 1. Null arg guards.
  CheckUTM(nullptr, "foo");
  GTIFDefn defn{};
  CheckUTM(&defn, nullptr);
  EXPECT_EQ(FALSE,
            CheckCitationKeyForStatePlaneUTM(nullptr, &defn, nullptr, nullptr));

  // 2. Direct CheckUTM tests:
  // With '|' after Datum and UTM Zone -> sets ProjCode = 16017.
  defn.ProjCode = 0;
  CheckUTM(&defn, "PCS Name = UTM Zone 17N|Datum = PSAD56|");
  EXPECT_EQ(16017, defn.ProjCode);
  // Calling again when ProjCode already matches 16017 -> false branch of !=.
  CheckUTM(&defn, "PCS Name = UTM Zone 17N|Datum = PSAD56|");
  EXPECT_EQ(16017, defn.ProjCode);

  // Calling without the matching "PCS Name = UTM Zone ...|" or "Datum = ...|"
  // key leaves ProjCode == 0.
  defn.ProjCode = 0;
  CheckUTM(&defn, "Datum = PSAD56");
  EXPECT_EQ(0, defn.ProjCode);
  CheckUTM(&defn, "UTM Zone 18S");
  EXPECT_EQ(0, defn.ProjCode);

  // 3. CheckCitationKeyForStatePlaneUTM:
  // A. PCS != kKvUserDefined -> early FALSE.
  {
    MemGTIF m("/vsimem/gtcit_sp0.tif");
    OGRSpatialReference srs;
    OGRBoolean lu = FALSE;
    defn.PCS = 32617;
    EXPECT_EQ(FALSE,
              CheckCitationKeyForStatePlaneUTM(m.gtif, &defn, &srs, &lu));
  }

  // B. GTCitationGeoKey ESRI StatePlane name with us_survey_feet,
  // international_feet, and meters.
  // TODO: b/335317901 - "meters" and "us_survey_feet" pin current production
  // behavior where CheckCitationKeyForStatePlaneUTM expects
  // "meters"/"us_survey_feet" while PROJ 9 WKT1_GDAL outputs
  // "metre"/"US survey foot".
  struct GtSpCase {
    const char* path;
    const char* citation;
    OGRBoolean expect_ok;
  };
  constexpr GtSpCase kGtSpCases[] = {
      {.path = "/vsimem/gtcit_sp_m.tif",
       .citation = "IMAGINE\nProjection Name = "
                   "NAD_1983_StatePlane_Alabama_East_FIPS_0101\"\nUnits = "
                   "meters",
       .expect_ok = FALSE},
      {.path = "/vsimem/gtcit_sp_usft.tif",
       .citation = "IMAGINE\nProjection Name = "
                   "NAD_1983_StatePlane_Alabama_East_FIPS_0101_Feet\"\nus "
                   "survey feet",
       .expect_ok = FALSE},
      {.path = "/vsimem/gtcit_sp_intl.tif",
       .citation = "IMAGINE\nProjection Name = "
                   "NAD_1983_StatePlane_Arizona_East_FIPS_0201_Feet_"
                   "Intl\"\ninternational linear_feet",
       .expect_ok = TRUE},
      {.path = "/vsimem/gtcit_sp_mismatch.tif",
       .citation = "IMAGINE\nProjection Name = "
                   "NAD_1983_StatePlane_Alabama_East_FIPS_0101_Feet\"\nmeter",
       .expect_ok = FALSE},
  };
  for (const GtSpCase& tc : kGtSpCases) {
    SCOPED_TRACE(tc.path);
    MemGTIF m(tc.path);
    GTIFKeySet(m.gtif, GTCitationGeoKey, TYPE_ASCII, 0, tc.citation);
    GTIFWriteKeys(m.gtif);
    OGRSpatialReference srs;
    OGRBoolean lu = FALSE;
    GTIFDefn d{};
    d.PCS = kKvUserDefined;
    d.UOMLength = kKvUserDefined;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(tc.expect_ok,
              CheckCitationKeyForStatePlaneUTM(m.gtif, &d, &srs, &lu));
    CPLErrorReset();
  }

  // C. hasUnits == false fallback via psDefn->UOMLength + PCSCitationGeoKey.
  struct PcsSpCase {
    const char* path;
    short uom;
    const char* citation;
    OGRBoolean expect_ok;
  };
  constexpr PcsSpCase kPcsSpCases[] = {
      {.path = "/vsimem/gtcit_pcssp1.tif",
       .uom = 9003,
       .citation = "PCS Name = State Plane Zone -101|Datum = NAD83|",
       .expect_ok = TRUE},
      {.path = "/vsimem/gtcit_pcssp2.tif",
       .uom = 9003,
       .citation = "PCS Name = State Plane Zone 101|Datum = NAD27|",
       .expect_ok = TRUE},
      {.path = "/vsimem/gtcit_pcssp3.tif",
       .uom = 9002,
       .citation = "PCS Name = State Plane Zone 201|",
       .expect_ok = TRUE},
      {.path = "/vsimem/gtcit_pcssp4.tif",
       .uom = 9001,
       .citation = "PCS Name = State Plane Zone 99999|",
       .expect_ok = FALSE},
      {.path = "/vsimem/gtcit_pcsutm.tif",
       .uom = kKvUserDefined,
       .citation = "PCS Name = UTM Zone 17N|Datum = PSAD56|",
       .expect_ok = FALSE},
  };
  for (const PcsSpCase& tc : kPcsSpCases) {
    SCOPED_TRACE(tc.path);
    MemGTIF m(tc.path);
    GTIFKeySet(m.gtif, PCSCitationGeoKey, TYPE_ASCII, 0, tc.citation);
    GTIFWriteKeys(m.gtif);
    OGRSpatialReference srs;
    OGRBoolean lu = FALSE;
    GTIFDefn d{};
    d.PCS = kKvUserDefined;
    d.UOMLength = tc.uom;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(tc.expect_ok,
              CheckCitationKeyForStatePlaneUTM(m.gtif, &d, &srs, &lu));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
