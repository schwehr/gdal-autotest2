// Copyright 2017 Google LLC
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

#include <memory>
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "ogr_spatialref.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab.h"

namespace autotest2 {
namespace {

void CreateTestTabFile(const char* pszPath) {
  TABFile tab(nullptr);
  ASSERT_EQ(tab.Open(pszPath, TABWrite), 0);
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  tab.SetSpatialRef(&srs);
  tab.SetBounds(-180, -90, 180, 90);
  tab.AddFieldNative("id", TABFInteger, 10);
  tab.AddFieldNative("name", TABFChar, 20);

  TABPoint pt(tab.GetLayerDefn());
  pt.SetField(0, 1);
  pt.SetField(1, "one");
  pt.SetGeometryDirectly(new OGRPoint(10, 20));
  EXPECT_EQ(tab.CreateFeature(&pt), OGRERR_NONE);

  TABPoint pt2(tab.GetLayerDefn());
  pt2.SetField(0, 2);
  pt2.SetField(1, "two");
  pt2.SetGeometryDirectly(new OGRPoint(50, 60));
  EXPECT_EQ(tab.CreateFeature(&pt2), OGRERR_NONE);

  tab.Close();
}

void CreateTestMifFile(const char* pszMifPath, const char* pszMidPath) {
  constexpr const char kMifContent[] =
      "Version 300\n"
      "Charset \"Neutral\"\n"
      "Delimiter \"\\t\"\n"
      "CoordSys Earth Projection 1, 104\n"
      "Columns 2\n"
      "  id Integer\n"
      "  name Char(20)\n"
      "Data\n"
      "\n"
      "Point 10 20\n"
      "Point 50 60\n";
  constexpr const char kMidContent[] =
      "1\tone\n"
      "2\ttwo\n";

  VSIFCloseL(VSIFileFromMemBuffer(
      pszMifPath, reinterpret_cast<GByte*>(const_cast<char*>(kMifContent)),
      sizeof(kMifContent) - 1, FALSE));
  VSIFCloseL(VSIFileFromMemBuffer(
      pszMidPath, reinterpret_cast<GByte*>(const_cast<char*>(kMidContent)),
      sizeof(kMidContent) - 1, FALSE));
}

TEST(MitabImapinfofileTest, OpenCompatibilityLayer) {
  constexpr const char kTabPath[] = "/vsimem/imapinfo_test/compat.tab";
  CreateTestTabFile(kTabPath);
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/imapinfo_test"); };

  TABFile tab(nullptr);
  // Valid read mode "r"
  EXPECT_EQ(tab.Open(kTabPath, "r"), 0);
  tab.Close();

  // Valid read mode "read"
  EXPECT_EQ(tab.Open(kTabPath, "read"), 0);
  tab.Close();

  // Valid write mode "w"
  constexpr const char kTabWritePath[] = "/vsimem/imapinfo_test/compat_w.tab";
  EXPECT_EQ(tab.Open(kTabWritePath, "w"), 0);
  tab.Close();

  // Invalid mode "x"
  {
    WithQuietHandler quiet;
    EXPECT_EQ(tab.Open(kTabPath, "xyz"), -1);
  }
}

TEST(MitabImapinfofileTest, SmartOpenErrorsAndEdgeCases) {
  WithQuietHandler quiet;

  // Null or short filename
  EXPECT_EQ(IMapInfoFile::SmartOpen(nullptr, nullptr, FALSE, TRUE), nullptr);
  EXPECT_EQ(IMapInfoFile::SmartOpen(nullptr, ".tab", FALSE, TRUE), nullptr);
  EXPECT_EQ(IMapInfoFile::SmartOpen(nullptr, nullptr, FALSE, FALSE), nullptr);

  // Non-existent MIF/MID
  EXPECT_EQ(
      IMapInfoFile::SmartOpen(nullptr, "/vsimem/nonexistent.mif", FALSE, TRUE),
      nullptr);
  EXPECT_EQ(
      IMapInfoFile::SmartOpen(nullptr, "/vsimem/nonexistent.mif", FALSE, FALSE),
      nullptr);
  EXPECT_EQ(
      IMapInfoFile::SmartOpen(nullptr, "/vsimem/nonexistent.mid", FALSE, TRUE),
      nullptr);

  // Non-existent TAB
  EXPECT_EQ(
      IMapInfoFile::SmartOpen(nullptr, "/vsimem/nonexistent.tab", FALSE, TRUE),
      nullptr);
  EXPECT_EQ(
      IMapInfoFile::SmartOpen(nullptr, "/vsimem/nonexistent.tab", FALSE, FALSE),
      nullptr);

  // TAB header with "Fields" but missing data files (.dat/.map)
  constexpr const char kBadTabPath[] = "/vsimem/imapinfo_test/bad_open.tab";
  constexpr const char kBadTabContent[] =
      "!table\n"
      "!version 300\n"
      "!charset Neutral\n"
      "\n"
      "Definition Table\n"
      "  Type NATIVE Charset \"Neutral\"\n"
      "  Fields 1\n"
      "    id Integer ;\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kBadTabPath, reinterpret_cast<GByte*>(const_cast<char*>(kBadTabContent)),
      sizeof(kBadTabContent) - 1, FALSE));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/imapinfo_test"); };

  EXPECT_EQ(IMapInfoFile::SmartOpen(nullptr, kBadTabPath, FALSE, TRUE),
            nullptr);
  EXPECT_EQ(IMapInfoFile::SmartOpen(nullptr, kBadTabPath, FALSE, FALSE),
            nullptr);
}

TEST(MitabImapinfofileTest, SmartOpenValidDatasets) {
  constexpr const char kMifPath[] = "/vsimem/imapinfo_test/smart.mif";
  constexpr const char kMidPath[] = "/vsimem/imapinfo_test/smart.mid";
  CreateTestMifFile(kMifPath, kMidPath);

  constexpr const char kTabPath[] = "/vsimem/imapinfo_test/smart.tab";
  CreateTestTabFile(kTabPath);

  constexpr const char kViewPath[] = "/vsimem/imapinfo_test/smart_view.tab";
  const std::string viewContent =
      "!table\n"
      "!version 300\n"
      "!charset Neutral\n"
      "\n"
      "create view smart_view as\n"
      "Select * from smart\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kViewPath,
      reinterpret_cast<GByte*>(const_cast<char*>(viewContent.data())),
      viewContent.size(), FALSE));

  constexpr const char kSeamlessPath[] =
      "/vsimem/imapinfo_test/smart_seamless.tab";
  const std::string seamlessContent =
      "!table\n"
      "!version 300\n"
      "!charset Neutral\n"
      "\n"
      "Section Table\n"
      "  \"\\IsSeamless\" = \"TRUE\"\n"
      "Fields 1\n"
      "  Table Char(50) ;\n";
  VSIFCloseL(VSIFileFromMemBuffer(
      kSeamlessPath,
      reinterpret_cast<GByte*>(const_cast<char*>(seamlessContent.data())),
      seamlessContent.size(), FALSE));

  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/imapinfo_test"); };

  // SmartOpen MIF
  std::unique_ptr<IMapInfoFile> mif(
      IMapInfoFile::SmartOpen(nullptr, kMifPath, FALSE, FALSE));
  ASSERT_NE(mif, nullptr);
  EXPECT_EQ(mif->GetFileClass(), TABFC_MIFFile);
  mif->Close();

  // SmartOpen MID
  std::unique_ptr<IMapInfoFile> mid(
      IMapInfoFile::SmartOpen(nullptr, kMidPath, FALSE, FALSE));
  ASSERT_NE(mid, nullptr);
  EXPECT_EQ(mid->GetFileClass(), TABFC_MIFFile);
  mid->Close();

  // SmartOpen TAB
  std::unique_ptr<IMapInfoFile> tab(
      IMapInfoFile::SmartOpen(nullptr, kTabPath, FALSE, FALSE));
  ASSERT_NE(tab, nullptr);
  EXPECT_EQ(tab->GetFileClass(), TABFC_TABFile);
  tab->Close();

  // SmartOpen TABView (fails Open because view target doesn't fully exist as
  // relation, but exercises branch)
  {
    WithQuietHandler quiet;
    std::unique_ptr<IMapInfoFile> view(
        IMapInfoFile::SmartOpen(nullptr, kViewPath, FALSE, TRUE));
    if (view) view->Close();
  }

  // SmartOpen TABSeamless (fails Open because index table lacks records, but
  // exercises branch)
  {
    WithQuietHandler quiet;
    std::unique_ptr<IMapInfoFile> seamless(
        IMapInfoFile::SmartOpen(nullptr, kSeamlessPath, FALSE, TRUE));
    if (seamless) seamless->Close();
  }
}

TEST(MitabImapinfofileTest, GetNextFeatureAndGetFeature) {
  constexpr const char kTabPath[] = "/vsimem/imapinfo_test/features.tab";
  CreateTestTabFile(kTabPath);
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/imapinfo_test"); };

  TABFile tab(nullptr);
  ASSERT_EQ(tab.Open(kTabPath, TABRead), 0);

  // Sequential reading via GetNextFeature
  OGRFeature* f1 = tab.GetNextFeature();
  ASSERT_NE(f1, nullptr);
  EXPECT_EQ(f1->GetFID(), 1);
  EXPECT_EQ(f1->GetFieldAsInteger(0), 1);
  EXPECT_NE(f1->GetGeometryRef(), nullptr);
  delete f1;

  OGRFeature* f2 = tab.GetNextFeature();
  ASSERT_NE(f2, nullptr);
  EXPECT_EQ(f2->GetFID(), 2);
  EXPECT_EQ(f2->GetFieldAsInteger(0), 2);
  delete f2;

  EXPECT_EQ(tab.GetNextFeature(), nullptr);

  // ResetReading and test with Spatial Filter
  tab.ResetReading();
  tab.SetSpatialFilterRect(0, 0, 20, 30);
  OGRFeature* sf1 = tab.GetNextFeature();
  ASSERT_NE(sf1, nullptr);
  EXPECT_EQ(sf1->GetFID(), 1);
  delete sf1;
  EXPECT_EQ(tab.GetNextFeature(), nullptr);

  // ResetReading and test with Attribute Filter
  tab.ResetReading();
  tab.SetSpatialFilter(nullptr);
  tab.SetAttributeFilter("id = 2");
  OGRFeature* af1 = tab.GetNextFeature();
  ASSERT_NE(af1, nullptr);
  EXPECT_EQ(af1->GetFID(), 2);
  delete af1;
  EXPECT_EQ(tab.GetNextFeature(), nullptr);
  tab.SetAttributeFilter(nullptr);

  // Random access via GetFeature
  OGRFeature* gf1 = tab.GetFeature(1);
  ASSERT_NE(gf1, nullptr);
  EXPECT_EQ(gf1->GetFID(), 1);
  delete gf1;

  OGRFeature* gf2 = tab.GetFeature(2);
  ASSERT_NE(gf2, nullptr);
  EXPECT_EQ(gf2->GetFID(), 2);
  delete gf2;

  EXPECT_EQ(tab.GetFeature(999), nullptr);

  tab.Close();
}

TEST(MitabImapinfofileTest, GetTABTypeAndCreateField) {
  TABFieldType eType;
  int nWidth = 0;
  int nPrec = 0;

  // OFTInteger default width
  {
    OGRFieldDefn f("f_int", OFTInteger);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFInteger);
    EXPECT_EQ(nWidth, 12);
  }

  // OFTInteger explicit width
  {
    OGRFieldDefn f("f_int5", OFTInteger);
    f.SetWidth(5);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFInteger);
    EXPECT_EQ(nWidth, 5);
  }

  // OFTInteger with OFSTBoolean
  {
    OGRFieldDefn f("f_bool", OFTInteger);
    f.SetSubType(OFSTBoolean);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFLogical);
    EXPECT_EQ(nWidth, 1);
  }

  // OFTInteger64 default and explicit width
  {
    OGRFieldDefn f("f_int64", OFTInteger64);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFLargeInt);
    EXPECT_EQ(nWidth, 20);

    f.SetWidth(15);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(nWidth, 15);
  }

  // OFTReal default (float)
  {
    OGRFieldDefn f("f_real", OFTReal);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFFloat);
    EXPECT_EQ(nWidth, 32);
  }

  // OFTReal decimal normal
  {
    OGRFieldDefn f("f_dec", OFTReal);
    f.SetWidth(10);
    f.SetPrecision(2);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFDecimal);
    EXPECT_EQ(nWidth, 10);
    EXPECT_EQ(nPrec, 2);
  }

  // OFTReal decimal limits (clamping nWidth > 20 and nPrecision > 16)
  {
    OGRFieldDefn f("f_dec_clamp", OFTReal);
    f.SetWidth(25);
    f.SetPrecision(20);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFDecimal);
    EXPECT_EQ(nWidth, 20);
    EXPECT_EQ(nPrec, 16);
  }

  // OFTReal decimal limits (nWidth - nPrecision < 2)
  {
    OGRFieldDefn f("f_dec_tight", OFTReal);
    f.SetWidth(10);
    f.SetPrecision(9);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFDecimal);
    EXPECT_EQ(nWidth, 10);
    EXPECT_EQ(nPrec, 8);
  }

  // OFTDate
  {
    OGRFieldDefn f("f_date", OFTDate);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFDate);
    EXPECT_EQ(nWidth, 10);
  }

  // OFTTime
  {
    OGRFieldDefn f("f_time", OFTTime);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFTime);
    EXPECT_EQ(nWidth, 9);
  }

  // OFTDateTime
  {
    OGRFieldDefn f("f_datetime", OFTDateTime);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFDateTime);
    EXPECT_EQ(nWidth, 19);
  }

  // OFTString default and clamped
  {
    OGRFieldDefn f("f_str", OFTString);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(eType, TABFChar);
    EXPECT_EQ(nWidth, 254);

    f.SetWidth(50);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(nWidth, 50);

    f.SetWidth(300);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), 0);
    EXPECT_EQ(nWidth, 254);
  }

  // Unsupported type (OFTBinary)
  {
    WithQuietHandler quiet;
    OGRFieldDefn f("f_bin", OFTBinary);
    EXPECT_EQ(IMapInfoFile::GetTABType(&f, &eType, &nWidth, &nPrec), -1);
  }

  // Test CreateField on a writable TABFile
  constexpr const char kTabPath[] = "/vsimem/imapinfo_test/create_field.tab";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/imapinfo_test"); };

  TABFile tab(nullptr);
  ASSERT_EQ(tab.Open(kTabPath, TABWrite), 0);
  OGRFieldDefn validField("valid_col", OFTInteger);
  EXPECT_EQ(tab.CreateField(&validField), OGRERR_NONE);

  {
    WithQuietHandler quiet;
    OGRFieldDefn badField("bad_col", OFTBinary);
    EXPECT_EQ(tab.CreateField(&badField), OGRERR_FAILURE);
  }
  tab.Close();
}

TEST(MitabImapinfofileTest, CreateTABFeatureAndICreateFeature) {
  constexpr const char kMifPath[] = "/vsimem/imapinfo_test/create_feat.mif";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/imapinfo_test"); };

  MIFFile mif(nullptr);
  ASSERT_EQ(mif.Open(kMifPath, TABWrite), 0);
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  mif.SetSpatialRef(&srs);
  mif.AddFieldNative("val", TABFInteger, 10);

  OGRLayer* poLayer = &mif;
  const OGRFeatureDefn* poDefn = mif.GetLayerDefn();

  // 1. Point without style
  {
    OGRFeature f(poDefn);
    f.SetField(0, 10);
    f.SetGeometryDirectly(new OGRPoint(1, 2));
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 2. Point with label style (TABText)
  {
    OGRFeature f(poDefn);
    f.SetField(0, 20);
    f.SetGeometryDirectly(new OGRPoint(3, 4));
    f.SetStyleString("LABEL(t:\"TestLabel\")");
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 3. Point with label style and symbol (emits warning)
  {
    WithQuietHandler quiet;
    OGRFeature f(poDefn);
    f.SetField(0, 30);
    f.SetGeometryDirectly(new OGRPoint(5, 6));
    f.SetStyleString("LABEL(t:\"Label\");SYMBOL(id:\"mapinfo-sym-34\")");
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 4. Point with font symbol style (TABFontPoint)
  {
    OGRFeature f(poDefn);
    f.SetField(0, 40);
    f.SetGeometryDirectly(new OGRPoint(7, 8));
    f.SetStyleString("SYMBOL(id:\"font-sym-35\",c:#000000,s:12pt)");
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 5. Point with custom symbol style (TABCustomPoint)
  {
    OGRFeature f(poDefn);
    f.SetField(0, 50);
    f.SetGeometryDirectly(new OGRPoint(9, 10));
    f.SetStyleString(
        "SYMBOL(id:\"mapinfo-custom-sym-1-customicon\",c:#000000,s:12pt)");
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 6. Point with standard symbol style (TABPoint)
  {
    OGRFeature f(poDefn);
    f.SetField(0, 60);
    f.SetGeometryDirectly(new OGRPoint(11, 12));
    f.SetStyleString("SYMBOL(id:\"mapinfo-sym-34\",c:#000000,s:12pt)");
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 7. Polygon with Pen and Brush styles (TABRegion)
  {
    OGRFeature f(poDefn);
    f.SetField(0, 70);
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(10, 0);
    ring->addPoint(10, 10);
    ring->addPoint(0, 10);
    ring->addPoint(0, 0);
    poly->addRingDirectly(ring.release());
    f.SetGeometryDirectly(poly.release());
    f.SetStyleString("PEN(w:1px,c:#000000);BRUSH(fc:#ff0000)");
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 8. MultiPolygon without style
  {
    OGRFeature f(poDefn);
    f.SetField(0, 80);
    auto mp = std::make_unique<OGRMultiPolygon>();
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(1, 0);
    ring->addPoint(1, 1);
    ring->addPoint(0, 1);
    ring->addPoint(0, 0);
    poly->addRingDirectly(ring.release());
    mp->addGeometryDirectly(poly.release());
    f.SetGeometryDirectly(mp.release());
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 9. LineString with Pen style (TABPolyline)
  {
    OGRFeature f(poDefn);
    f.SetField(0, 90);
    auto line = std::make_unique<OGRLineString>();
    line->addPoint(0, 0);
    line->addPoint(5, 5);
    f.SetGeometryDirectly(line.release());
    f.SetStyleString("PEN(w:2px,c:#00ff00)");
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 10. MultiLineString without style
  {
    OGRFeature f(poDefn);
    f.SetField(0, 100);
    auto ml = std::make_unique<OGRMultiLineString>();
    auto line = std::make_unique<OGRLineString>();
    line->addPoint(0, 0);
    line->addPoint(5, 5);
    ml->addGeometryDirectly(line.release());
    f.SetGeometryDirectly(ml.release());
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 11. MultiPoint (triggers recursive ICreateFeature loop)
  {
    OGRFeature f(poDefn);
    f.SetField(0, 110);
    auto mp = std::make_unique<OGRMultiPoint>();
    mp->addGeometryDirectly(new OGRPoint(1, 1));
    mp->addGeometryDirectly(new OGRPoint(2, 2));
    f.SetGeometryDirectly(mp.release());
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 12. GeometryCollection (triggers recursive ICreateFeature loop)
  {
    OGRFeature f(poDefn);
    f.SetField(0, 120);
    auto gc = std::make_unique<OGRGeometryCollection>();
    gc->addGeometryDirectly(new OGRPoint(3, 3));
    auto line = std::make_unique<OGRLineString>();
    line->addPoint(0, 0);
    line->addPoint(1, 1);
    gc->addGeometryDirectly(line.release());
    f.SetGeometryDirectly(gc.release());
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 13. None geometry
  {
    OGRFeature f(poDefn);
    f.SetField(0, 130);
    f.SetGeometry(nullptr);
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  // 14. Unsupported geometry type (wkbCircularString) -> default branch
  {
    OGRFeature f(poDefn);
    f.SetField(0, 140);
    auto circ = std::unique_ptr<OGRGeometry>(
        OGRGeometryFactory::createGeometry(wkbCircularString));
    f.SetGeometry(circ.get());
    EXPECT_EQ(poLayer->CreateFeature(&f), OGRERR_NONE);
  }

  mif.Close();
}

TEST(MitabImapinfofileTest, CharsetAndEncoding) {
  constexpr const char kTabPath[] = "/vsimem/imapinfo_test/charset.tab";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/imapinfo_test"); };

  TABFile tab(nullptr);
  ASSERT_EQ(tab.Open(kTabPath, TABWrite), 0);

  // SetCharset / GetCharset
  EXPECT_EQ(tab.SetCharset(nullptr), -1);
  EXPECT_EQ(tab.SetCharset(""), -1);
  EXPECT_EQ(tab.SetCharset("UTF-8"), 0);
  EXPECT_STREQ(tab.GetCharset(), "UTF-8");
  EXPECT_EQ(tab.SetCharset(tab.GetCharset()), 0);

  // CharsetToEncoding
  EXPECT_STREQ(IMapInfoFile::CharsetToEncoding(nullptr), "");
  EXPECT_STREQ(IMapInfoFile::CharsetToEncoding("Neutral"), "");
  EXPECT_STREQ(IMapInfoFile::CharsetToEncoding("ISO8859_1"), "ISO-8859-1");
  EXPECT_STREQ(IMapInfoFile::CharsetToEncoding("WindowsLatin1"), "CP1252");
  EXPECT_STREQ(IMapInfoFile::CharsetToEncoding("UTF-8"), "UTF-8");
  {
    WithQuietHandler quiet;
    EXPECT_STREQ(IMapInfoFile::CharsetToEncoding("Unknown_Charset"), "");
  }

  // EncodingToCharset
  EXPECT_STREQ(IMapInfoFile::EncodingToCharset(nullptr), "Neutral");
  EXPECT_STREQ(IMapInfoFile::EncodingToCharset(""), "Neutral");
  EXPECT_STREQ(IMapInfoFile::EncodingToCharset("ISO-8859-1"), "ISO8859_1");
  EXPECT_STREQ(IMapInfoFile::EncodingToCharset("CP1252"), "WindowsLatin1");
  EXPECT_STREQ(IMapInfoFile::EncodingToCharset("UTF-8"), "UTF-8");
  {
    WithQuietHandler quiet;
    EXPECT_STREQ(IMapInfoFile::EncodingToCharset("Unknown_Encoding"),
                 "Neutral");
  }

  // GetEncoding / SetEncoding
  EXPECT_STREQ(tab.GetEncoding(), "UTF-8");
  tab.SetEncoding("CP1252");
  EXPECT_STREQ(tab.GetCharset(), "WindowsLatin1");
  EXPECT_STREQ(tab.GetEncoding(), "CP1252");

  // TestUtf8Capability
  tab.SetCharset("Neutral");
  EXPECT_FALSE(tab.TestUtf8Capability());

  tab.SetCharset("UTF-8");
  tab.TestUtf8Capability();

  // SetStrictLaundering
  tab.SetStrictLaundering(true);
  tab.SetStrictLaundering(false);

  tab.Close();
}

TEST(MitabImapinfofileTest, NormalizeFieldNameCollisionsAndLength) {
  constexpr const char kMifPath[] = "/vsimem/imapinfo_test/norm.mif";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/imapinfo_test"); };

  MIFFile mif(nullptr);
  ASSERT_EQ(mif.Open(kMifPath, TABWrite), 0);
  mif.SetEncoding("UTF-8");

  // Long field name > 31 characters generates warning
  {
    WithQuietHandler quiet;
    EXPECT_EQ(mif.AddFieldNative(
                  "very_long_field_name_exceeding_thirty_one_characters",
                  TABFInteger, 10, 0, FALSE, FALSE, TRUE),
              0);
  }

  // Generate 102 duplicate field names:
  // 1. "dup_col"
  // 2..10. "dup_col_1" through "dup_col_9"
  // 11..100. "dup_col10" through "dup_col99"
  // 101. hits "Too many field names" error
  WithQuietHandler quiet;
  for (int i = 0; i < 102; i++) {
    mif.AddFieldNative("dup_col", TABFInteger, 10, 0, FALSE, FALSE, TRUE);
  }

  mif.Close();
}

}  // namespace
}  // namespace autotest2
