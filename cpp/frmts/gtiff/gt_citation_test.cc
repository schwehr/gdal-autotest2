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

#include "third_party/gdal/frmts/gtiff/gt_citation.h"

#include <cstring>
#include <map>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr short KvUserDefined = 32767;

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
    }
  }

  ~MemGTIF() {
    if (gtif) GTIFFree(gtif);
    if (tif) TIFFClose(tif);
    if (fp) VSIFCloseL(fp);
    VSIUnlink(path.c_str());
  }
};

class GTCitationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    // Resolve a GDAL_DATA file once so the /vsimem/gdal_data lookup path is
    // initialized before ImportFromESRIStatePlaneWKT is exercised.
    (void)CPLFindFile("gdal", "esri_StatePlane_extra.wkt");
    GTiffOneTimeInit();
    LibgeotiffOneTimeInit();
  }
};

TEST_F(GTCitationTest, ImagineCitationTranslationAllBranches) {
  EXPECT_EQ(ImagineCitationTranslation(nullptr, PCSCitationGeoKey), nullptr);

  char non_imagine[] = "Regular Citation String";
  EXPECT_EQ(ImagineCitationTranslation(non_imagine, PCSCitationGeoKey),
            nullptr);

  // Imagine string without '$' -> p == nullptr, but still extracts inline keys.
  {
    char s[] =
        "IMAGINE GeoTIFF Support\nNAD = NAD83\nDatum = North American   \n"
        "Ellipsoid = GRS 1980\nUnits = meters   ";
    char* res = ImagineCitationTranslation(s, PCSCitationGeoKey);
    ASSERT_NE(res, nullptr);
    EXPECT_NE(std::strstr(res, "NAD = NAD83|"), nullptr);
    EXPECT_NE(std::strstr(res, "Datum = North American|"), nullptr);
    EXPECT_NE(std::strstr(res, "Ellipsoid = GRS 1980|"), nullptr);
    EXPECT_NE(std::strstr(res, "LUnits = meters|"), nullptr);
    CPLFree(res);
  }

  // Imagine with '$' and '\n', PCSCitationGeoKey WITH "Projection = " and
  // trailing spaces (p1 stops at first '\n', so "Projection = " must be on the
  // first line after "$Id$\n"!).
  {
    char s[] =
        "IMAGINE GeoTIFF Support\n$Id$\nProjection = Transverse Mercator   \n"
        "Units = feet\n";
    char* res = ImagineCitationTranslation(s, PCSCitationGeoKey);
    ASSERT_NE(res, nullptr);
    EXPECT_NE(std::strstr(res, "PRJ Name = Transverse Mercator|"), nullptr);
    EXPECT_NE(std::strstr(res, "LUnits = feet|"), nullptr);
    CPLFree(res);
  }

  // PCSCitationGeoKey WITHOUT "Projection = " (with "Projection Name = ").
  {
    char s[] =
        "IMAGINE GeoTIFF Support\n$Id$\nProjection Name = MyPCS   \n"
        "Datum = WGS84";
    char* res = ImagineCitationTranslation(s, PCSCitationGeoKey);
    ASSERT_NE(res, nullptr);
    EXPECT_NE(std::strstr(res, "PCS Name = MyPCS|"), nullptr);
    CPLFree(res);
  }

  // GTCitationGeoKey without '\n' after '$', inline key termination.
  {
    char s[] =
        "IMAGINE GeoTIFF Support $MyGTCit   NAD = 27 Units = us_survey_feet";
    char* res = ImagineCitationTranslation(s, GTCitationGeoKey);
    ASSERT_NE(res, nullptr);
    EXPECT_NE(std::strstr(res, "NAD = 27|"), nullptr);
    EXPECT_NE(std::strstr(res, "LUnits = us_survey_feet|"), nullptr);
    CPLFree(res);
  }

  // GeogCitationGeoKey without "Unable to" vs with "Unable to", plus default
  // keyID (VerticalCitationGeoKey).
  {
    char s1[] = "IMAGINE GeoTIFF Support\n$Id$\nWGS 84   \nEllipsoid = WGS84";
    char* res1 = ImagineCitationTranslation(s1, GeogCitationGeoKey);
    ASSERT_NE(res1, nullptr);
    EXPECT_NE(std::strstr(res1, "GCS Name = WGS 84|"), nullptr);
    CPLFree(res1);

    char s2[] = "IMAGINE GeoTIFF Support\n$Id$\nUnable to match EPSG\n";
    char* res2 = ImagineCitationTranslation(s2, GeogCitationGeoKey);
    EXPECT_EQ(res2, nullptr);

    char s3[] = "IMAGINE GeoTIFF Support\n$Id$\nSomeVert   \nUnits = meters";
    char* res3 = ImagineCitationTranslation(s3, VerticalCitationGeoKey);
    ASSERT_NE(res3, nullptr);
    EXPECT_NE(std::strstr(res3, "LUnits = meters|"), nullptr);
    CPLFree(res3);
  }
}

TEST_F(GTCitationTest, CitationStringParseAndSetLinearUnitCitation) {
  EXPECT_EQ(CitationStringParse(nullptr, PCSCitationGeoKey), nullptr);

  char empty_str[] = "";
  EXPECT_EQ(CitationStringParse(empty_str, GeogCitationGeoKey), nullptr);

  char one_char[] = "x";
  EXPECT_EQ(CitationStringParse(one_char, GeogCitationGeoKey), nullptr);

  // Plain string >= 2 chars: GeogCitationGeoKey sets CitGcsName; PCS returns
  // nullptr.
  {
    char plain[] = "MyCustomGCS";
    char** gcs_ret = CitationStringParse(plain, GeogCitationGeoKey);
    ASSERT_NE(gcs_ret, nullptr);
    EXPECT_STREQ(gcs_ret[CitGcsName], "MyCustomGCS");
    for (int i = 0; i < nCitationNameTypes; ++i) CPLFree(gcs_ret[i]);
    CPLFree(gcs_ret);

    EXPECT_EQ(CitationStringParse(plain, PCSCitationGeoKey), nullptr);
  }

  // Full pipe-delimited string with all 8 keys (plus duplicate key ignored).
  {
    char full[] =
        "PCS Name = P1|PCS Name = P2|PRJ Name = TM|LUnits = m|GCS Name = G1|"
        "Datum = D1|Ellipsoid = E1|Primem = PM1|AUnits = rad";
    char** ret = CitationStringParse(full, PCSCitationGeoKey);
    ASSERT_NE(ret, nullptr);
    EXPECT_STREQ(ret[CitPcsName], "P1");
    EXPECT_STREQ(ret[CitProjectionName], "TM");
    EXPECT_STREQ(ret[CitLUnitsName], "m");
    EXPECT_STREQ(ret[CitGcsName], "G1");
    EXPECT_STREQ(ret[CitDatumName], "D1");
    EXPECT_STREQ(ret[CitEllipsoidName], "E1");
    EXPECT_STREQ(ret[CitPrimemName], "PM1");
    EXPECT_STREQ(ret[CitAUnitsName], "rad");
    for (int i = 0; i < nCitationNameTypes; ++i) CPLFree(ret[i]);
    CPLFree(ret);
  }

  // SetLinearUnitCitation: empty, existing without '|', existing with '|'.
  std::map<geokey_t, std::string> ascii_keys;
  SetLinearUnitCitation(ascii_keys, "Foot_US");
  EXPECT_EQ(ascii_keys[PCSCitationGeoKey], "LUnits = Foot_US");

  SetLinearUnitCitation(ascii_keys, "meters");
  EXPECT_EQ(ascii_keys[PCSCitationGeoKey], "LUnits = Foot_US|LUnits = meters|");

  SetLinearUnitCitation(ascii_keys, "yard");
  EXPECT_EQ(ascii_keys[PCSCitationGeoKey],
            "LUnits = Foot_US|LUnits = meters|LUnits = yard|");
}

TEST_F(GTCitationTest, SetGeogCSCitationAndGetGeogCSFromCitation) {
  MemGTIF m("/vsimem/gtcit_geog.tif");

  std::map<geokey_t, std::string> keys;
  OGRSpatialReference srs;
  srs.SetGeogCS("MyGeog", "MyDatum", "MyEllipsoid", 6378137.0, 298.257223563,
                "Paris", 2.33722917, "grad", 0.015707963267948967);

  // 1. Missing GeogCitationGeoKey -> early return.
  SetGeogCSCitation(m.gtif, keys, &srs, "grad", KvUserDefined, KvUserDefined);
  EXPECT_TRUE(keys.empty());

  // 2. Empty GeogCitationGeoKey -> early return.
  keys[GeogCitationGeoKey] = "";
  SetGeogCSCitation(m.gtif, keys, &srs, "grad", KvUserDefined, KvUserDefined);
  EXPECT_EQ(keys[GeogCitationGeoKey], "");

  // 3. Without "GCS Name = " prefix, KvUserDefined datum & spheroid, non-Degree
  // angular unit -> rewrites all fields and sets GeogPrimeMeridianLongGeoKey!
  keys[GeogCitationGeoKey] = "MyGeog";
  SetGeogCSCitation(m.gtif, keys, &srs, "grad", KvUserDefined, KvUserDefined);
  EXPECT_NE(keys[GeogCitationGeoKey].find("GCS Name = MyGeog|Datum = MyDatum|"),
            std::string::npos);
  EXPECT_NE(keys[GeogCitationGeoKey].find("Ellipsoid = MyEllipsoid|"),
            std::string::npos);
  EXPECT_NE(keys[GeogCitationGeoKey].find("Primem = Paris|AUnits = grad|"),
            std::string::npos);

  // 4. Already starts with "GCS Name = ", Degree unit, known datum/spheroid,
  // trailing '|'.
  keys[GeogCitationGeoKey] = "GCS Name = Existing|";
  SetGeogCSCitation(m.gtif, keys, &srs, "Degree", 6326, 7030);
  EXPECT_NE(
      keys[GeogCitationGeoKey].find("GCS Name = Existing||Primem = Paris|"),
      std::string::npos);

  // 5. Empty SRS (no PRIMEM, Degree, known datum/spheroid) -> bRewrite false!
  OGRSpatialReference empty_srs;
  keys[GeogCitationGeoKey] = "Unchanged";
  SetGeogCSCitation(m.gtif, keys, &empty_srs, "Degree", 6326, 7030);
  EXPECT_EQ(keys[GeogCitationGeoKey], "Unchanged");

  // 6. GetGeogCSFromCitation: Imagine + pipe-delimited all 5 fields.
  char buf[256];
  std::snprintf(
      buf, sizeof(buf), "%s",
      "GCS Name = G1|Datum = D1|Ellipsoid = E1|Primem = P1|AUnits = A1|");
  char *geog = nullptr, *datum = nullptr, *pm = nullptr, *sph = nullptr,
       *au = nullptr;
  GetGeogCSFromCitation(buf, sizeof(buf), GeogCitationGeoKey, &geog, &datum,
                        &pm, &sph, &au);
  EXPECT_STREQ(geog, "G1");
  EXPECT_STREQ(datum, "D1");
  EXPECT_STREQ(sph, "E1");
  EXPECT_STREQ(pm, "P1");
  EXPECT_STREQ(au, "A1");
  CPLFree(geog);
  CPLFree(datum);
  CPLFree(pm);
  CPLFree(sph);
  CPLFree(au);

  std::snprintf(buf, sizeof(buf), "%s",
                "IMAGINE GeoTIFF Support\n$Id$\nMyImgGCS\nDatum = ImgD");
  GetGeogCSFromCitation(buf, sizeof(buf), GeogCitationGeoKey, &geog, &datum,
                        &pm, &sph, &au);
  EXPECT_STREQ(geog, "MyImgGCS");
  EXPECT_STREQ(datum, "ImgD");
  CPLFree(geog);
  CPLFree(datum);
  CPLFree(pm);
  CPLFree(sph);
  CPLFree(au);
}

TEST_F(GTCitationTest, SetCitationToSRSAllBranches) {
  MemGTIF m("/vsimem/gtcit_setsrs.tif");
  GTIFKeySet(m.gtif, ProjLinearUnitSizeGeoKey, TYPE_DOUBLE, 1, 0.5);

  // 1. Empty SRS: the function first resets *linearUnitIsSet to FALSE (the
  // SRS has no linear unit), then the Imagine string translates to
  // "PRJ Name = Mercator|LUnits = yards|". "yards" is a known unit in
  // apszUnitMap, so SetLinearUnits is applied and lu_set ends up TRUE again.
  // Returns FALSE because no "PCS Name = " is present.
  {
    OGRSpatialReference srs;
    OGRBoolean lu_set = TRUE;
    char cit[256] =
        "IMAGINE GeoTIFF Support\n$Id$\nProjection = Mercator\nUnits = yards";
    EXPECT_FALSE(SetCitationToSRS(m.gtif, cit, sizeof(cit), PCSCitationGeoKey,
                                  &srs, &lu_set));
    EXPECT_TRUE(lu_set);
  }

  // 2. SRS with existing linear unit (linearUnitIsSet = TRUE), custom unit not
  // in apszUnitMap ("furlong" -> reads ProjLinearUnitSizeGeoKey = 0.5 from
  // hGTIF!), and CitPcsName set.
  {
    OGRSpatialReference srs;
    srs.SetProjCS("InitialPCS");
    srs.SetLinearUnits("metre", 1.0);
    OGRBoolean lu_set = FALSE;
    char cit[256] = "PCS Name = UpdatedPCS|LUnits = furlong|";
    EXPECT_TRUE(SetCitationToSRS(m.gtif, cit, sizeof(cit), PCSCitationGeoKey,
                                 &srs, &lu_set));
    EXPECT_TRUE(lu_set);
    EXPECT_DOUBLE_EQ(srs.GetLinearUnits(nullptr), 0.5);
  }

  // 3. GTCitationGeoKey without "PCS Name = ":
  // (a) empty SRS & "Projected Coordinates" -> skips SetNode, returns TRUE
  // (b) empty SRS & normal name -> sets PROJCS
  // (c) "unnamed" PROJCS -> replaces "unnamed" with szCTString
  {
    OGRSpatialReference srs1;
    OGRBoolean lu = FALSE;
    char cit1[128] = "Projected Coordinates";
    EXPECT_TRUE(SetCitationToSRS(m.gtif, cit1, sizeof(cit1), GTCitationGeoKey,
                                 &srs1, &lu));

    OGRSpatialReference srs2;
    char cit2[128] = "CustomGTCitationName";
    EXPECT_TRUE(SetCitationToSRS(m.gtif, cit2, sizeof(cit2), GTCitationGeoKey,
                                 &srs2, &lu));
    EXPECT_STREQ(srs2.GetAttrValue("PROJCS"), "CustomGTCitationName");

    OGRSpatialReference srs3;
    srs3.SetNode("PROJCS", "unnamed");
    char cit3[128] = "NamedFromGT";
    EXPECT_TRUE(SetCitationToSRS(m.gtif, cit3, sizeof(cit3), GTCitationGeoKey,
                                 &srs3, &lu));
    EXPECT_STREQ(srs3.GetAttrValue("PROJCS"), "NamedFromGT");
  }
}

TEST_F(GTCitationTest, CheckCitationKeyForStatePlaneUTMAndCheckUTM) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // Null arg guards.
  CheckUTM(nullptr, "foo");
  GTIFDefn defn{};
  CheckUTM(&defn, nullptr);
  EXPECT_FALSE(
      CheckCitationKeyForStatePlaneUTM(nullptr, &defn, nullptr, nullptr));

  // Direct CheckUTM tests:
  // 1. With '|' after Datum and UTM Zone -> sets ProjCode = 16017!
  defn.ProjCode = 0;
  CheckUTM(&defn, "PCS Name = UTM Zone 17N|Datum = PSAD56|");
  EXPECT_EQ(defn.ProjCode, 16017);
  // Calling again when ProjCode already matches 16017 -> false branch of !=.
  CheckUTM(&defn, "PCS Name = UTM Zone 17N|Datum = PSAD56|");
  EXPECT_EQ(defn.ProjCode, 16017);

  // 2. Without '|' after Datum or UTM Zone. "Datum = PSAD56" alone has no
  // "UTM Zone " so the table lookup is skipped; "UTM Zone 18S" alone has an
  // empty datum name which never matches "PSAD56". ProjCode stays 0.
  defn.ProjCode = 0;
  CheckUTM(&defn, "Datum = PSAD56");
  EXPECT_EQ(defn.ProjCode, 0);
  CheckUTM(&defn, "UTM Zone 18S");
  EXPECT_EQ(defn.ProjCode, 0);

  // CheckCitationKeyForStatePlaneUTM:
  // A. PCS != KvUserDefined -> early FALSE.
  {
    MemGTIF m("/vsimem/gtcit_sp0.tif");
    OGRSpatialReference srs;
    OGRBoolean lu = FALSE;
    defn.PCS = 32617;
    EXPECT_FALSE(CheckCitationKeyForStatePlaneUTM(m.gtif, &defn, &srs, &lu));
  }

  // B. GTCitationGeoKey ESRI StatePlane name with us_survey_feet,
  // international_feet, and meters!
  auto test_gt_sp = [&](const char* path, const char* gt_cit, bool expect_ok) {
    MemGTIF m(path);
    GTIFKeySet(m.gtif, GTCitationGeoKey, TYPE_ASCII, 0, gt_cit);
    GTIFWriteKeys(m.gtif);
    OGRSpatialReference srs;
    OGRBoolean lu = FALSE;
    GTIFDefn d{};
    d.PCS = KvUserDefined;
    d.UOMLength = KvUserDefined;
    EXPECT_EQ(CheckCitationKeyForStatePlaneUTM(m.gtif, &d, &srs, &lu) != FALSE,
              expect_ok);
  };

  // Note: the `false` expectations for "meters" and "us_survey_feet" pin the
  // current production behavior. CheckCitationKeyForStatePlaneUTM compares the
  // lower-cased PROJCS|UNIT name against the substrings "meter",
  // "us_survey_feet" and "foot_us", but PROJ 9's WKT1_GDAL names those units
  // "metre" and "US survey foot", so neither matches and `done` stays false.
  // Only "international_feet" matches (via the "foot" substring) and returns
  // true. If upstream fixes that comparison, these two expectations must flip.
  test_gt_sp("/vsimem/gtcit_sp_m.tif",
             "IMAGINE\nProjection Name = "
             "NAD_1983_StatePlane_Alabama_East_FIPS_0101\"\nUnits = meters",
             false);
  test_gt_sp(
      "/vsimem/gtcit_sp_usft.tif",
      "IMAGINE\nProjection Name = "
      "NAD_1983_StatePlane_Alabama_East_FIPS_0101_Feet\"\nus survey feet",
      false);
  test_gt_sp("/vsimem/gtcit_sp_intl.tif",
             "IMAGINE\nProjection Name = "
             "NAD_1983_StatePlane_Arizona_East_FIPS_0201_Feet_"
             "Intl\"\ninternational linear_feet",
             true);
  // Unit mismatch (meters citation on Feet CS) -> done == false, falls through.
  test_gt_sp("/vsimem/gtcit_sp_mismatch.tif",
             "IMAGINE\nProjection Name = "
             "NAD_1983_StatePlane_Alabama_East_FIPS_0101_Feet\"\nmeter",
             false);

  // C. hasUnits == false fallback via psDefn->UOMLength (9003 US survey foot,
  // 9002 international foot, 9001 meter, KvUserDefined) + PCSCitationGeoKey
  // State Plane Zone (negative zone, NAD83, NAD27, HARN, and UTM Zone)!
  auto test_pcs_sp = [&](const char* path, short uom, const char* pcs_cit,
                         bool expect_ok) {
    MemGTIF m(path);
    GTIFKeySet(m.gtif, PCSCitationGeoKey, TYPE_ASCII, 0, pcs_cit);
    GTIFWriteKeys(m.gtif);
    OGRSpatialReference srs;
    OGRBoolean lu = FALSE;
    GTIFDefn d{};
    d.PCS = KvUserDefined;
    d.UOMLength = uom;
    EXPECT_EQ(CheckCitationKeyForStatePlaneUTM(m.gtif, &d, &srs, &lu) != FALSE,
              expect_ok);
  };

  test_pcs_sp("/vsimem/gtcit_pcssp1.tif", 9003,
              "PCS Name = State Plane Zone -101|Datum = NAD83|", true);
  test_pcs_sp("/vsimem/gtcit_pcssp2.tif", 9003,
              "PCS Name = State Plane Zone 101|Datum = NAD27|", true);
  test_pcs_sp("/vsimem/gtcit_pcssp3.tif", 9002,
              "PCS Name = State Plane Zone 201|", true);
  test_pcs_sp("/vsimem/gtcit_pcssp4.tif", 9001,
              "PCS Name = State Plane Zone 99999|", false);
  test_pcs_sp("/vsimem/gtcit_pcsutm.tif", KvUserDefined,
              "PCS Name = UTM Zone 17N|Datum = PSAD56|", false);
}

}  // namespace
}  // namespace autotest2
