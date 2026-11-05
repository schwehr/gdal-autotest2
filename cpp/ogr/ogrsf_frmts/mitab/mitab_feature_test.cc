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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_utils.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

// Test helper subclass to expose protected methods of TABFeature.
class TestTABFeature : public TABFeature {
 public:
  using TABFeature::TABFeature;
  using TABFeature::UpdateMBR;
  using TABFeature::ValidateCoordType;
};

// Helper to create an in-memory TABMAPFile opened for writing.
std::unique_ptr<TABMAPFile> CreateMemoryMapFile(const std::string& basename,
                                                double minX = -1000.0,
                                                double minY = -1000.0,
                                                double maxX = 1000.0,
                                                double maxY = 1000.0) {
  const std::string mapPath = "/vsimem/" + basename + ".map";
  auto mapFile = std::make_unique<TABMAPFile>("");
  if (mapFile->Open(mapPath.c_str(), "w") != 0) {
    return nullptr;
  }
  mapFile->SetCoordsysBounds(minX, minY, maxX, maxY);
  return mapFile;
}

// Helper to write a feature to a map file and commit it.
int WriteFeatureToMap(TABMAPFile* mapFile, TABFeature* feat, int id) {
  CPLErrorReset();
  const TABGeomType geomType = feat->ValidateMapInfoType(mapFile);
  std::unique_ptr<TABMAPObjHdr> objHdr(TABMAPObjHdr::NewObj(geomType, id));
  if (!objHdr) {
    return -1;
  }
  feat->GetIntMBR(objHdr->m_nMinX, objHdr->m_nMinY, objHdr->m_nMaxX,
                  objHdr->m_nMaxY);
  if (mapFile->PrepareNewObj(objHdr.get()) != 0) {
    return -1;
  }
  if (feat->WriteGeometryToMAPFile(mapFile, objHdr.get()) != 0) {
    return -1;
  }
  return mapFile->CommitNewObj(objHdr.get());
}

// Helper to read a feature from a map file.
int ReadFeatureFromMap(TABMAPFile* mapFile, TABFeature* feat, int id,
                       GBool bCoordDataOnly = FALSE) {
  CPLErrorReset();
  if (mapFile->MoveToObjId(id) != 0) {
    return -1;
  }
  std::unique_ptr<TABMAPObjHdr> objHdr(
      TABMAPObjHdr::NewObj(mapFile->GetCurObjType(), mapFile->GetCurObjId()));
  if (!objHdr) {
    return -1;
  }
  if (objHdr->ReadObj(mapFile->GetCurObjBlock()) != 0) {
    return -1;
  }
  return feat->ReadGeometryFromMAPFile(mapFile, objHdr.get(), bCoordDataOnly);
}

TEST(MitabFeatureTest, PenStyleMethods) {
  ITABFeaturePen pen;
  EXPECT_EQ(pen.GetPenPattern(), 2);
  EXPECT_EQ(pen.GetPenColor(), 0);
  EXPECT_EQ(pen.GetPenWidthPixel(), 1);
  EXPECT_DOUBLE_EQ(pen.GetPenWidthPoint(), 0.0);
  EXPECT_EQ(pen.GetPenWidthMIF(), 1);

  pen.SetPenPattern(5);
  EXPECT_EQ(pen.GetPenPattern(), 5);

  pen.SetPenColor(0x123456);
  EXPECT_EQ(pen.GetPenColor(), 0x123456);

  pen.SetPenWidthPixel(4);
  EXPECT_EQ(pen.GetPenWidthPixel(), 4);
  EXPECT_EQ(pen.GetPenWidthMIF(), 4);

  // Clamp pixel width:
  pen.SetPenWidthPixel(0);
  EXPECT_EQ(pen.GetPenWidthPixel(), 1);
  pen.SetPenWidthPixel(10);
  EXPECT_EQ(pen.GetPenWidthPixel(), 7);

  pen.SetPenWidthPoint(2.5);
  EXPECT_DOUBLE_EQ(pen.GetPenWidthPoint(), 2.5);
  EXPECT_EQ(pen.GetPenWidthPixel(), 1);
  EXPECT_EQ(pen.GetPenWidthMIF(), 35);  // 10 + 25

  pen.SetPenWidthMIF(5);  // <= 10 -> pixel width
  EXPECT_EQ(pen.GetPenWidthPixel(), 5);
  EXPECT_DOUBLE_EQ(pen.GetPenWidthPoint(), 0.0);

  pen.SetPenWidthMIF(25);  // > 10 -> point width = (25-10)/10 = 1.5
  EXPECT_DOUBLE_EQ(pen.GetPenWidthPoint(), 1.5);
  EXPECT_EQ(pen.GetPenWidthPixel(), 0);

  // Test GetPenStyleString for all patterns 1..26 with both pixel and point
  // width.
  for (int pat = 1; pat <= 26; ++pat) {
    pen.SetPenPattern(pat);
    pen.SetPenWidthPixel(2);
    const char* str_px = pen.GetPenStyleString();
    EXPECT_THAT(str_px, NotNull());

    pen.SetPenWidthPoint(3.0);
    const char* str_pt = pen.GetPenStyleString();
    EXPECT_THAT(str_pt, NotNull());
  }

  // SetPenFromStyleString tests.
  pen.SetPenFromStyleString(nullptr);
  pen.SetPenFromStyleString("");
  pen.SetPenFromStyleString("BRUSH(fc:#123456)");  // No pen part.

  pen.SetPenFromStyleString("PEN(w:2px,c:#abcdef,id:\"mapinfo-pen-7\")");
  EXPECT_EQ(pen.GetPenPattern(), 7);
  EXPECT_EQ(pen.GetPenColor(), 0xabcdef);
  EXPECT_EQ(pen.GetPenWidthPixel(), 2);

  // ogr-pen styles 0..7 and unknown 99.
  for (int i = 0; i <= 7; ++i) {
    std::string s = CPLSPrintf("PEN(w:1.5pt,c:#001122,id:\"ogr-pen-%d\")", i);
    pen.SetPenFromStyleString(s.c_str());
    EXPECT_EQ(pen.GetPenColor(), 0x001122);
    EXPECT_DOUBLE_EQ(pen.GetPenWidthPoint(), 1.5);
  }

  pen.SetPenFromStyleString(
      "PEN(w:2.0mm,c:#334455,id:\"ogr-pen-99\",p:\"1 1px\")");
  EXPECT_EQ(pen.GetPenColor(), 0x334455);

  pen.SetPenFromStyleString("PEN(w:1.0cm,c:#556677,p:\"12 2px\")");
  EXPECT_EQ(pen.GetPenColor(), 0x556677);

  pen.SetPenFromStyleString("PEN(w:0.5in,c:#778899,id:\"custom-pen\")");
  EXPECT_EQ(pen.GetPenColor(), 0x778899);

  const char* kPenPatterns[] = {"1 1",
                                "2 1",
                                "3 1",
                                "6 1",
                                "12 2",
                                "24 4",
                                "4 3",
                                "1 4",
                                "4 6",
                                "6 4",
                                "12 12",
                                "8 2 1 2",
                                "12 1 1 1",
                                "12 1 3 1",
                                "24 6 4 6",
                                "24 3 3 3 3 3",
                                "24 3 3 3 3 3 3 3",
                                "6 3 1 3 1 3",
                                "12 2 1 2 1 2",
                                "12 2 1 2 1 2 1 2",
                                "4 1 1 1",
                                "4 1 1 1 1",
                                "1 2 1 2"};
  for (const char* pat : kPenPatterns) {
    std::string s = CPLSPrintf("PEN(p:\"%spx\")", pat);
    pen.SetPenFromStyleString(s.c_str());
  }

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  pen.DumpPenDef(fp);
  EXPECT_THAT(pen.GetPenDefRef(), NotNull());
}

TEST(MitabFeatureTest, BrushStyleMethods) {
  ITABFeatureBrush brush;
  EXPECT_EQ(brush.GetBrushFGColor(), 0);
  EXPECT_EQ(brush.GetBrushBGColor(), 0xffffff);
  EXPECT_EQ(brush.GetBrushPattern(), 1);
  EXPECT_FALSE(brush.GetBrushTransparent());

  brush.SetBrushFGColor(0x112233);
  EXPECT_EQ(brush.GetBrushFGColor(), 0x112233);

  brush.SetBrushBGColor(0x445566);
  EXPECT_EQ(brush.GetBrushBGColor(), 0x445566);

  brush.SetBrushPattern(5);
  EXPECT_EQ(brush.GetBrushPattern(), 5);

  brush.SetBrushTransparent(true);
  EXPECT_TRUE(brush.GetBrushTransparent());

  // GetBrushStyleString with transparent true/false across patterns 1..8 and 2.
  for (int pat : {1, 2, 3, 4, 5, 6, 7, 8, 12}) {
    brush.SetBrushPattern(pat);
    brush.SetBrushTransparent(false);
    EXPECT_THAT(brush.GetBrushStyleString(), NotNull());
    brush.SetBrushTransparent(true);
    EXPECT_THAT(brush.GetBrushStyleString(), NotNull());
  }

  // SetBrushFromStyleString tests.
  brush.SetBrushFromStyleString(nullptr);
  brush.SetBrushFromStyleString("");
  brush.SetBrushFromStyleString("PEN(w:1px)");  // No brush part.

  brush.SetBrushTransparent(false);
  brush.SetBrushFromStyleString(
      "BRUSH(fc:#abcdef,bc:#123456,id:\"mapinfo-brush-4\")");
  EXPECT_EQ(brush.GetBrushFGColor(), 0xabcdef);
  EXPECT_EQ(brush.GetBrushBGColor(), 0x123456);
  EXPECT_EQ(brush.GetBrushPattern(), 4);
  EXPECT_FALSE(brush.GetBrushTransparent());

  brush.SetBrushFromStyleString("BRUSH(fc:#fedcba,id:\"mapinfo-brush-2\")");
  EXPECT_EQ(brush.GetBrushFGColor(), 0xfedcba);
  EXPECT_TRUE(brush.GetBrushTransparent());

  // 8-digit hex ending in 00 alpha sets transparent as well.
  brush.SetBrushFromStyleString(
      "BRUSH(fc:#abcdef,bc:#12345600,id:\"mapinfo-brush-2\")");
  EXPECT_TRUE(brush.GetBrushTransparent());

  brush.SetBrushFromStyleString("BRUSH(fc:#12345600)");
  brush.SetBrushFromStyleString("BRUSH(fc:#12345678)");
  brush.SetBrushFromStyleString("BRUSH(fc:#123456,bc:#fedcba78)");

  for (int i = 0; i <= 7; ++i) {
    std::string s = CPLSPrintf("BRUSH(fc:#111111,id:\"ogr-brush-%d\")", i);
    brush.SetBrushFromStyleString(s.c_str());
  }
  brush.SetBrushFromStyleString("BRUSH(fc:#222222,id:\"ogr-brush-99\")");

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  brush.DumpBrushDef(fp);
  EXPECT_THAT(brush.GetBrushDefRef(), NotNull());
}

TEST(MitabFeatureTest, FontStyleMethods) {
  ITABFeatureFont font;
  EXPECT_STREQ(font.GetFontNameRef(), "Arial");

  font.SetFontName("Courier");
  EXPECT_STREQ(font.GetFontNameRef(), "Courier");

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  font.DumpFontDef(fp);
  EXPECT_THAT(font.GetFontDefRef(), NotNull());
}

TEST(MitabFeatureTest, SymbolStyleMethods) {
  ITABFeatureSymbol sym;
  EXPECT_EQ(sym.GetSymbolNo(), 35);
  EXPECT_EQ(sym.GetSymbolSize(), 12);
  EXPECT_EQ(sym.GetSymbolColor(), 0);

  sym.SetSymbolNo(40);
  EXPECT_EQ(sym.GetSymbolNo(), 40);

  sym.SetSymbolSize(16);
  EXPECT_EQ(sym.GetSymbolSize(), 16);

  sym.SetSymbolColor(0x998877);
  EXPECT_EQ(sym.GetSymbolColor(), 0x998877);

  EXPECT_THAT(sym.GetSymbolStyleString(), NotNull());

  for (int sNo = 31; sNo <= 68; ++sNo) {
    sym.SetSymbolNo(sNo);
    EXPECT_THAT(sym.GetSymbolStyleString(), NotNull());
  }

  sym.SetSymbolFromStyleString(nullptr);
  sym.SetSymbolFromStyleString("");
  sym.SetSymbolFromStyleString("PEN(w:1px)");

  sym.SetSymbolFromStyleString(
      "SYMBOL(c:#123456,s:14pt,id:\"mapinfo-sym-36\")");
  EXPECT_EQ(sym.GetSymbolColor(), 0x123456);
  EXPECT_EQ(sym.GetSymbolSize(), 14);
  EXPECT_EQ(sym.GetSymbolNo(), 36);

  for (int i = 0; i <= 10; ++i) {
    std::string s = CPLSPrintf("SYMBOL(s:10px,id:\"ogr-sym-%d\")", i);
    sym.SetSymbolFromStyleString(s.c_str());
  }

  sym.SetSymbolFromStyleString("SYMBOL(s:2.5mm,id:\"custom-sym\")");
  sym.SetSymbolFromStyleString("SYMBOL(s:1.0cm,id:\"custom-sym\")");
  sym.SetSymbolFromStyleString("SYMBOL(s:0.5in,id:\"custom-sym\")");

  // GetSymbolFeatureClass tests.
  EXPECT_EQ(ITABFeatureSymbol::GetSymbolFeatureClass(nullptr), TABFCPoint);
  EXPECT_EQ(ITABFeatureSymbol::GetSymbolFeatureClass(""), TABFCPoint);
  EXPECT_EQ(ITABFeatureSymbol::GetSymbolFeatureClass("PEN(w:1px)"), TABFCPoint);
  EXPECT_EQ(
      ITABFeatureSymbol::GetSymbolFeatureClass("SYMBOL(id:\"font-sym-12\")"),
      TABFCFontPoint);
  EXPECT_EQ(ITABFeatureSymbol::GetSymbolFeatureClass(
                "SYMBOL(id:\"mapinfo-custom-sym-2-test.bmp\")"),
            TABFCCustomPoint);
  EXPECT_EQ(
      ITABFeatureSymbol::GetSymbolFeatureClass("SYMBOL(id:\"mapinfo-sym-35\")"),
      TABFCPoint);

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  sym.DumpSymbolDef(fp);
  EXPECT_THAT(sym.GetSymbolDefRef(), NotNull());
}

TEST(MitabFeatureTest, FeatureFactoryAndBaseMethods) {
  auto* defn = new OGRFeatureDefn("test_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  // Test all geom types in CreateFromMapInfoType.
  const int geomTypes[] = {
      TAB_GEOM_NONE,
      TAB_GEOM_SYMBOL,
      TAB_GEOM_SYMBOL_C,
      TAB_GEOM_FONTSYMBOL,
      TAB_GEOM_FONTSYMBOL_C,
      TAB_GEOM_CUSTOMSYMBOL,
      TAB_GEOM_CUSTOMSYMBOL_C,
      TAB_GEOM_LINE,
      TAB_GEOM_LINE_C,
      TAB_GEOM_PLINE,
      TAB_GEOM_PLINE_C,
      TAB_GEOM_MULTIPLINE,
      TAB_GEOM_MULTIPLINE_C,
      TAB_GEOM_V450_MULTIPLINE,
      TAB_GEOM_V450_MULTIPLINE_C,
      TAB_GEOM_V800_MULTIPLINE,
      TAB_GEOM_V800_MULTIPLINE_C,
      TAB_GEOM_ARC,
      TAB_GEOM_ARC_C,
      TAB_GEOM_REGION,
      TAB_GEOM_REGION_C,
      TAB_GEOM_V450_REGION,
      TAB_GEOM_V450_REGION_C,
      TAB_GEOM_V800_REGION,
      TAB_GEOM_V800_REGION_C,
      TAB_GEOM_RECT,
      TAB_GEOM_RECT_C,
      TAB_GEOM_ROUNDRECT,
      TAB_GEOM_ROUNDRECT_C,
      TAB_GEOM_ELLIPSE,
      TAB_GEOM_ELLIPSE_C,
      TAB_GEOM_TEXT,
      TAB_GEOM_TEXT_C,
      TAB_GEOM_MULTIPOINT,
      TAB_GEOM_MULTIPOINT_C,
      TAB_GEOM_V800_MULTIPOINT,
      TAB_GEOM_V800_MULTIPOINT_C,
      TAB_GEOM_COLLECTION,
      TAB_GEOM_COLLECTION_C,
      TAB_GEOM_V800_COLLECTION,
      TAB_GEOM_V800_COLLECTION_C,
      999,  // Unknown type produces warning and returns TABFeature.
  };

  for (int t : geomTypes) {
    CPLErrorReset();
    TABFeature* f = TABFeature::CreateFromMapInfoType(t, defn);
    ASSERT_THAT(f, NotNull());
    delete f;
  }

  // Base TABFeature methods using TestTABFeature subclass.
  TestTABFeature feat(defn);
  EXPECT_EQ(feat.GetMapInfoType(), TAB_GEOM_NONE);

  feat.SetMBR(10.0, 20.0, 30.0, 40.0);
  double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
  feat.GetMBR(xmin, ymin, xmax, ymax);
  EXPECT_DOUBLE_EQ(xmin, 10.0);
  EXPECT_DOUBLE_EQ(ymin, 20.0);
  EXPECT_DOUBLE_EQ(xmax, 30.0);
  EXPECT_DOUBLE_EQ(ymax, 40.0);

  feat.SetIntMBR(100, 200, 300, 400);
  GInt32 nXMin = 0, nYMin = 0, nXMax = 0, nYMax = 0;
  feat.GetIntMBR(nXMin, nYMin, nXMax, nYMax);
  EXPECT_EQ(nXMin, 100);
  EXPECT_EQ(nYMin, 200);
  EXPECT_EQ(nXMax, 300);
  EXPECT_EQ(nYMax, 400);

  EXPECT_EQ(feat.UpdateMBR(nullptr), -1);  // No geometry.

  auto pt = std::make_unique<OGRPoint>(15.0, 25.0);
  feat.SetGeometryDirectly(pt.release());
  EXPECT_EQ(feat.UpdateMBR(nullptr), 0);
  feat.GetMBR(xmin, ymin, xmax, ymax);
  EXPECT_DOUBLE_EQ(xmin, 15.0);
  EXPECT_DOUBLE_EQ(ymin, 25.0);

  // ValidateCoordType with memory MAP file.
  const std::string base = "base_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };
  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  EXPECT_TRUE(feat.ValidateCoordType(mapFile.get()));

  // Test uncompressed branch in ValidateCoordType (span >= 65535 int units).
  auto lineLarge = std::make_unique<OGRLineString>();
  lineLarge->addPoint(-500.0, -500.0);
  lineLarge->addPoint(500.0, 500.0);
  feat.SetGeometryDirectly(lineLarge.release());
  EXPECT_FALSE(feat.ValidateCoordType(mapFile.get()));

  // ForceCoordTypeAndOrigin tests.
  feat.ForceCoordTypeAndOrigin(TAB_GEOM_LINE, TRUE, 0, 0, 10, 20, 30, 40);
  EXPECT_EQ(feat.GetMapInfoType(), TAB_GEOM_LINE_C);
  feat.ForceCoordTypeAndOrigin(TAB_GEOM_LINE_C, FALSE, 0, 0, 10, 20, 30, 40);
  EXPECT_EQ(feat.GetMapInfoType(), TAB_GEOM_LINE);

  // Base Read/Write geometry on TABFeature (returns 0).
  EXPECT_EQ(feat.ReadGeometryFromMAPFile(mapFile.get(), nullptr), 0);
  EXPECT_EQ(feat.WriteGeometryToMAPFile(mapFile.get(), nullptr), 0);

  // Cloning.
  TABFeature* cloneSame = feat.CloneTABFeature(nullptr);
  ASSERT_THAT(cloneSame, NotNull());
  delete cloneSame;

  auto* defn2 = new OGRFeatureDefn("diff_layer");
  defn2->Reference();
  absl::Cleanup defn2_cleanup = [defn2] { defn2->Release(); };
  TABFeature* cloneDiff = feat.CloneTABFeature(defn2);
  ASSERT_THAT(cloneDiff, NotNull());
  delete cloneDiff;

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  feat.DumpMID(fp);
  feat.DumpMIF(fp);
}

TEST(MitabFeatureTest, DatRecordReadWriteAllTypesAndFIDHack) {
  const std::string datPath = "/vsimem/rec_test.dat";
  const std::string indPath = "/vsimem/rec_test.ind";
  absl::Cleanup dat_clean = [&] {
    VSIUnlink(datPath.c_str());
    VSIUnlink(indPath.c_str());
  };

  auto* defn = new OGRFeatureDefn("rec_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  OGRFieldDefn fld_char("f_char", OFTString);
  defn->AddFieldDefn(&fld_char);
  OGRFieldDefn fld_dec("f_dec", OFTReal);
  fld_dec.SetWidth(10);
  fld_dec.SetPrecision(2);
  defn->AddFieldDefn(&fld_dec);
  OGRFieldDefn fld_int("f_int", OFTInteger);
  defn->AddFieldDefn(&fld_int);
  OGRFieldDefn fld_sint("f_sint", OFTInteger);
  defn->AddFieldDefn(&fld_sint);
  OGRFieldDefn fld_lint("f_lint", OFTInteger64);
  defn->AddFieldDefn(&fld_lint);
  OGRFieldDefn fld_flt("f_flt", OFTReal);
  defn->AddFieldDefn(&fld_flt);
  OGRFieldDefn fld_log("f_log", OFTInteger);
  fld_log.SetSubType(OFSTBoolean);
  defn->AddFieldDefn(&fld_log);
  OGRFieldDefn fld_date("f_date", OFTDate);
  defn->AddFieldDefn(&fld_date);
  OGRFieldDefn fld_time("f_time", OFTTime);
  defn->AddFieldDefn(&fld_time);
  OGRFieldDefn fld_dt("f_dt", OFTDateTime);
  defn->AddFieldDefn(&fld_dt);

  TABDATFile datFile("");
  ASSERT_EQ(0, datFile.Open(datPath.c_str(), "w"));
  EXPECT_EQ(0, datFile.AddField("f_char", TABFChar, 20));
  EXPECT_EQ(0, datFile.AddField("f_dec", TABFDecimal, 10, 2));
  EXPECT_EQ(0, datFile.AddField("f_int", TABFInteger, 10));
  EXPECT_EQ(0, datFile.AddField("f_sint", TABFSmallInt, 5));
  EXPECT_EQ(0, datFile.AddField("f_lint", TABFLargeInt, 18));
  EXPECT_EQ(0, datFile.AddField("f_flt", TABFFloat, 12));
  EXPECT_EQ(0, datFile.AddField("f_log", TABFLogical, 1));
  EXPECT_EQ(0, datFile.AddField("f_date", TABFDate, 10));
  EXPECT_EQ(0, datFile.AddField("f_time", TABFTime, 9));
  EXPECT_EQ(0, datFile.AddField("f_dt", TABFDateTime, 19));

  TABINDFile indFile;
  ASSERT_EQ(0, indFile.Open(indPath.c_str(), "w"));
  int indexNo = indFile.CreateIndex(TABFInteger, 10);
  int panIndexNo[10] = {0, 0, indexNo, 0, 0, 0, 0, 0, 0, 0};

  // Record 1: fully set fields.
  TABPoint feat1(defn);
  feat1.SetField(0, "TestString");
  feat1.SetField(1, 123.45);
  feat1.SetField(2, 42);
  feat1.SetField(3, 100);
  feat1.SetField(4, static_cast<GIntBig>(10000000000LL));
  feat1.SetField(5, 3.14159);
  feat1.SetField(6, 1);
  feat1.SetField(7, "2026/11/03");
  feat1.SetField(8, "12:34:56");
  feat1.SetField(9, "2026/11/03 12:34:56");

  ASSERT_THAT(datFile.GetRecordBlock(1), NotNull());
  EXPECT_EQ(0, feat1.WriteRecordToDATFile(&datFile, &indFile, panIndexNo));

  // Record 2: unset/null dates/times to hit null date branches.
  TABPoint feat2(defn);
  feat2.SetField(0, "NullDates");
  feat2.SetField(1, 0.0);
  feat2.SetField(2, 43);
  feat2.SetField(3, 0);
  feat2.SetField(4, static_cast<GIntBig>(0));
  feat2.SetField(5, 0.0);
  feat2.SetField(6, 0);
  // fields 7, 8, 9 unset
  ASSERT_THAT(datFile.GetRecordBlock(2), NotNull());
  EXPECT_EQ(0, feat2.WriteRecordToDATFile(&datFile, &indFile, panIndexNo));

  datFile.Close();
  indFile.Close();

  // Read back records and verify.
  ASSERT_EQ(0, datFile.Open(datPath.c_str(), "r"));
  EXPECT_EQ(0, datFile.ValidateFieldInfoFromTAB(0, "f_char", TABFChar, 20, 0));
  EXPECT_EQ(0,
            datFile.ValidateFieldInfoFromTAB(1, "f_dec", TABFDecimal, 10, 2));
  EXPECT_EQ(0,
            datFile.ValidateFieldInfoFromTAB(2, "f_int", TABFInteger, 10, 0));
  EXPECT_EQ(0,
            datFile.ValidateFieldInfoFromTAB(3, "f_sint", TABFSmallInt, 5, 0));
  EXPECT_EQ(0,
            datFile.ValidateFieldInfoFromTAB(4, "f_lint", TABFLargeInt, 18, 0));
  EXPECT_EQ(0, datFile.ValidateFieldInfoFromTAB(5, "f_flt", TABFFloat, 12, 0));
  EXPECT_EQ(0, datFile.ValidateFieldInfoFromTAB(6, "f_log", TABFLogical, 1, 0));
  EXPECT_EQ(0, datFile.ValidateFieldInfoFromTAB(7, "f_date", TABFDate, 10, 0));
  EXPECT_EQ(0, datFile.ValidateFieldInfoFromTAB(8, "f_time", TABFTime, 9, 0));
  EXPECT_EQ(0,
            datFile.ValidateFieldInfoFromTAB(9, "f_dt", TABFDateTime, 19, 0));
  TABPoint featRead(defn);

  ASSERT_THAT(datFile.GetRecordBlock(1), NotNull());
  EXPECT_EQ(0, featRead.ReadRecordFromDATFile(&datFile));
  EXPECT_STREQ(featRead.GetFieldAsString(0), "TestString");
  EXPECT_DOUBLE_EQ(featRead.GetFieldAsDouble(1), 123.45);
  EXPECT_EQ(featRead.GetFieldAsInteger(2), 42);
  EXPECT_EQ(featRead.GetFieldAsInteger(3), 100);
  EXPECT_EQ(featRead.GetFieldAsInteger64(4), 10000000000LL);
  EXPECT_EQ(featRead.GetFieldAsInteger(6), 1);

  ASSERT_THAT(datFile.GetRecordBlock(2), NotNull());
  EXPECT_EQ(0, featRead.ReadRecordFromDATFile(&datFile));
  EXPECT_STREQ(featRead.GetFieldAsString(0), "NullDates");
  EXPECT_EQ(featRead.GetFieldAsInteger(2), 43);

  datFile.Close();

  // Test the FID hack branch: iField == 0 >= GetDefnRef()->GetFieldCount()
  auto* emptyDefn = new OGRFeatureDefn("empty");
  emptyDefn->Reference();
  absl::Cleanup empty_cleanup = [emptyDefn] { emptyDefn->Release(); };

  const std::string fidDatPath = "/vsimem/fid_hack.dat";
  absl::Cleanup fid_clean = [&] { VSIUnlink(fidDatPath.c_str()); };

  TABDATFile fidDat("");
  ASSERT_EQ(0, fidDat.Open(fidDatPath.c_str(), "w"));
  EXPECT_EQ(0, fidDat.AddField("FID", TABFInteger, 10));

  TABFeature fidFeat(emptyDefn);
  fidFeat.SetFID(99);
  ASSERT_THAT(fidDat.GetRecordBlock(1), NotNull());
  EXPECT_EQ(0, fidFeat.WriteRecordToDATFile(&fidDat, nullptr, nullptr));
  fidDat.Close();

  ASSERT_EQ(0, fidDat.Open(fidDatPath.c_str(), "r"));
  EXPECT_EQ(0, fidDat.ValidateFieldInfoFromTAB(0, "FID", TABFInteger, 10, 0));
  ASSERT_THAT(fidDat.GetRecordBlock(1), NotNull());
  EXPECT_EQ(0, fidFeat.ReadRecordFromDATFile(&fidDat));
  EXPECT_EQ(fidFeat.GetFID(), 99);
  fidDat.Close();
}

TEST(MitabFeatureTest, PointClassesReadWriteAndStyles) {
  const std::string base = "pt_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  auto* defn = new OGRFeatureDefn("pt_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  // TABPoint: compressed and uncompressed.
  TABPoint ptCompr(defn);
  EXPECT_THAT(ptCompr.GetStyleString(), NotNull());
  auto p1 = std::make_unique<OGRPoint>(1.0, 2.0);
  ptCompr.SetGeometryDirectly(p1.release());
  EXPECT_EQ(ptCompr.ValidateMapInfoType(mapFile.get()), TAB_GEOM_SYMBOL);
  EXPECT_DOUBLE_EQ(ptCompr.GetX(), 1.0);
  EXPECT_DOUBLE_EQ(ptCompr.GetY(), 2.0);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &ptCompr, 1));

  TABPoint ptUncompr(defn);
  auto p2 = std::make_unique<OGRPoint>(500.0, 500.0);
  ptUncompr.SetGeometryDirectly(p2.release());
  // Force uncompressed:
  ptUncompr.SetIntMBR(-100000000, -100000000, 100000000, 100000000);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &ptUncompr, 2));

  // TABPoint with invalid geometry.
  TABPoint ptBad(defn);
  auto polyBad = std::make_unique<OGRPolygon>();
  ptBad.SetGeometryDirectly(polyBad.release());
  EXPECT_EQ(ptBad.ValidateMapInfoType(mapFile.get()), TAB_GEOM_NONE);
  CPLErrorReset();

  // TABFontPoint.
  TABFontPoint fontPt(defn);
  auto pf = std::make_unique<OGRPoint>(3.0, 4.0);
  fontPt.SetGeometryDirectly(pf.release());
  fontPt.SetFontName("MapInfo Symbols");
  fontPt.SetSymbolNo(50);
  fontPt.SetSymbolAngle(45.0);
  fontPt.ToggleFontStyle(TABFSBold, TRUE);
  EXPECT_TRUE(fontPt.QueryFontStyle(TABFSBold));
  fontPt.ToggleFontStyle(TABFSBold, FALSE);
  EXPECT_FALSE(fontPt.QueryFontStyle(TABFSBold));

  fontPt.SetFontStyleMIFValue(16);
  EXPECT_EQ(fontPt.GetFontStyleMIFValue(), 16);

  fontPt.SetSymbolAngle(-30.0);
  EXPECT_DOUBLE_EQ(fontPt.GetSymbolAngle(), 330.0);
  fontPt.SetSymbolAngle(400.0);
  EXPECT_DOUBLE_EQ(fontPt.GetSymbolAngle(), 40.0);

  EXPECT_THAT(fontPt.GetStyleString(), NotNull());
  fontPt.SetFontStyleTABValue(16);
  EXPECT_THAT(fontPt.GetSymbolStyleString(30.0), NotNull());
  fontPt.SetFontStyleTABValue(512);
  EXPECT_THAT(fontPt.GetSymbolStyleString(30.0), NotNull());
  fontPt.SetFontStyleTABValue(0);
  EXPECT_THAT(fontPt.GetSymbolStyleString(30.0), NotNull());
  fontPt.SetSymbolFromStyleString(
      "SYMBOL(id:\"font-sym-42\",f:\"Courier\",s:12pt,c:#112233)");
  EXPECT_EQ(fontPt.ValidateMapInfoType(mapFile.get()), TAB_GEOM_FONTSYMBOL);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &fontPt, 3));

  // TABCustomPoint.
  TABCustomPoint customPt(defn);
  auto pc = std::make_unique<OGRPoint>(5.0, 6.0);
  customPt.SetGeometryDirectly(pc.release());
  customPt.SetSymbolName("my_custom.bmp");
  customPt.SetCustomSymbolStyle(0x02);  // Apply color.
  EXPECT_EQ(customPt.GetCustomSymbolStyle(), 0x02);
  EXPECT_THAT(customPt.GetStyleString(), NotNull());
  customPt.SetCustomSymbolStyle(0x01);  // Without color.
  EXPECT_THAT(customPt.GetStyleString(), NotNull());
  customPt.SetCustomSymbolStyle(0);
  EXPECT_THAT(customPt.GetSymbolStyleString(15.0), NotNull());
  customPt.SetCustomSymbolStyle(2);
  EXPECT_THAT(customPt.GetSymbolStyleString(15.0), NotNull());
  customPt.SetSymbolFromStyleString(
      "SYMBOL(id:\"mapinfo-custom-sym-3-my_icon.bmp\",s:14pt)");

  EXPECT_EQ(customPt.ValidateMapInfoType(mapFile.get()), TAB_GEOM_CUSTOMSYMBOL);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &customPt, 4));

  mapFile->Close();

  // Read back features.
  ASSERT_EQ(0, mapFile->Open(mapPath.c_str(), "r"));

  TABPoint readPt(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readPt, 1));
  EXPECT_NEAR(readPt.GetX(), 1.0, 1e-4);
  EXPECT_NEAR(readPt.GetY(), 2.0, 1e-4);

  // bCoordBlockDataOnly branch.
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readPt, 1, TRUE));

  TABFontPoint readFontPt(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readFontPt, 3));
  EXPECT_STREQ(readFontPt.GetFontNameRef(), "Courier");

  TABCustomPoint readCustomPt(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readCustomPt, 4));
  EXPECT_STREQ(readCustomPt.GetSymbolNameRef(), "my_icon.bmp");

  mapFile->Close();

  // Cloning and dump tests.
  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };

  TABFeature* clonePt = ptCompr.CloneTABFeature(nullptr);
  ASSERT_THAT(clonePt, NotNull());
  clonePt->DumpMIF(fp);
  delete clonePt;

  TABFeature* cloneFont = fontPt.CloneTABFeature(nullptr);
  ASSERT_THAT(cloneFont, NotNull());
  cloneFont->DumpMIF(fp);
  delete cloneFont;

  TABFeature* cloneCustom = customPt.CloneTABFeature(nullptr);
  ASSERT_THAT(cloneCustom, NotNull());
  cloneCustom->DumpMIF(fp);
  delete cloneCustom;
}

TEST(MitabFeatureTest, PolylineMethodsAndMapFile) {
  const std::string base = "pline_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  auto* defn = new OGRFeatureDefn("pline_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  // 1. Two-point line -> TAB_GEOM_LINE.
  TABPolyline lineFeat(defn);
  auto line2 = std::make_unique<OGRLineString>();
  line2->addPoint(0.0, 0.0);
  line2->addPoint(0.01, 0.01);
  lineFeat.SetGeometryDirectly(line2.release());
  EXPECT_EQ(lineFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_LINE);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &lineFeat, 1));

  // 2. Two-point line with TwoPointLineAsPolyline -> TAB_GEOM_PLINE_C.
  lineFeat.TwoPointLineAsPolyline(TRUE);
  EXPECT_TRUE(lineFeat.TwoPointLineAsPolyline());
  EXPECT_EQ(lineFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_PLINE_C);
  lineFeat.m_bSmooth = TRUE;
  EXPECT_TRUE(lineFeat.m_bSmooth);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &lineFeat, 2));

  // 3. Multi-point PLINE (>2 vertices).
  TABPolyline plineFeat(defn);
  auto lineMulti = std::make_unique<OGRLineString>();
  lineMulti->addPoint(0.0, 0.0);
  lineMulti->addPoint(0.005, 0.005);
  lineMulti->addPoint(0.01, 0.0);
  plineFeat.SetGeometryDirectly(lineMulti.release());
  EXPECT_EQ(plineFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_PLINE_C);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &plineFeat, 3));

  // 4. MultiLineString -> MULTIPLINE_C.
  TABPolyline mlineFeat(defn);
  auto mls = std::make_unique<OGRMultiLineString>();
  auto part1 = std::make_unique<OGRLineString>();
  part1->addPoint(0.0, 0.0);
  part1->addPoint(0.01, 0.01);
  auto part2 = std::make_unique<OGRLineString>();
  part2->addPoint(0.02, 0.02);
  part2->addPoint(0.03, 0.03);
  mls->addGeometryDirectly(part1.release());
  mls->addGeometryDirectly(part2.release());
  mlineFeat.SetGeometryDirectly(mls.release());
  EXPECT_EQ(mlineFeat.ValidateMapInfoType(mapFile.get()),
            TAB_GEOM_MULTIPLINE_C);
  EXPECT_EQ(mlineFeat.GetNumParts(), 2);
  EXPECT_THAT(mlineFeat.GetPartRef(0), NotNull());
  EXPECT_THAT(mlineFeat.GetPartRef(1), NotNull());
  EXPECT_EQ(mlineFeat.GetPartRef(2), nullptr);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &mlineFeat, 4));

  // 5. Uncompressed LINE (delta >= 65535).
  TABPolyline lineUnc(defn);
  auto lineLarge = std::make_unique<OGRLineString>();
  lineLarge->addPoint(-500.0, -500.0);
  lineLarge->addPoint(500.0, 500.0);
  lineUnc.SetGeometryDirectly(lineLarge.release());
  EXPECT_EQ(lineUnc.ValidateMapInfoType(mapFile.get()), TAB_GEOM_LINE);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &lineUnc, 5));

  // 6. Center.
  double cx = 0, cy = 0;
  EXPECT_EQ(0, lineFeat.GetCenter(cx, cy));
  lineFeat.SetCenter(5.0, 6.0);
  EXPECT_EQ(0, lineFeat.GetCenter(cx, cy));
  EXPECT_DOUBLE_EQ(cx, 5.0);
  EXPECT_DOUBLE_EQ(cy, 6.0);

  // 7. Invalid geometry.
  TABPolyline plineBad(defn);
  auto ptBad = std::make_unique<OGRPoint>(1.0, 2.0);
  plineBad.SetGeometryDirectly(ptBad.release());
  EXPECT_EQ(plineBad.ValidateMapInfoType(mapFile.get()), TAB_GEOM_NONE);
  CPLErrorReset();

  mapFile->Close();

  // Read back features.
  ASSERT_EQ(0, mapFile->Open(mapPath.c_str(), "r"));

  TABPolyline readLine(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readLine, 1));
  EXPECT_EQ(readLine.GetNumParts(), 1);
  EXPECT_THAT(readLine.GetPartRef(0), NotNull());
  EXPECT_EQ(readLine.GetPartRef(1), nullptr);

  TABPolyline readPline(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readPline, 2));
  EXPECT_TRUE(readPline.m_bSmooth);

  TABPolyline readMline(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readMline, 4));
  EXPECT_EQ(readMline.GetNumParts(), 2);

  // Test bCoordBlockDataOnly.
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readMline, 4, TRUE));

  mapFile->Close();

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  lineFeat.DumpMIF(fp);
  mlineFeat.DumpMIF(fp);
  plineBad.DumpMIF(fp);
  CPLErrorReset();
  EXPECT_THAT(lineFeat.GetStyleString(), NotNull());

  TABFeature* clonePline = lineFeat.CloneTABFeature(nullptr);
  ASSERT_THAT(clonePline, NotNull());
  delete clonePline;
}

TEST(MitabFeatureTest, RegionMethodsAndMapFile) {
  const std::string base = "reg_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  auto* defn = new OGRFeatureDefn("reg_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  // 1. Single polygon with interior hole -> REGION_C.
  TABRegion regFeat(defn);
  auto poly = std::make_unique<OGRPolygon>();
  auto outer = std::make_unique<OGRLinearRing>();
  outer->addPoint(0.0, 0.0);
  outer->addPoint(0.02, 0.0);
  outer->addPoint(0.02, 0.02);
  outer->addPoint(0.0, 0.02);
  outer->addPoint(0.0, 0.0);
  auto inner = std::make_unique<OGRLinearRing>();
  inner->addPoint(0.005, 0.005);
  inner->addPoint(0.015, 0.005);
  inner->addPoint(0.015, 0.015);
  inner->addPoint(0.005, 0.015);
  inner->addPoint(0.005, 0.005);
  poly->addRingDirectly(outer.release());
  poly->addRingDirectly(inner.release());
  regFeat.SetGeometryDirectly(poly.release());

  EXPECT_EQ(regFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_REGION_C);
  EXPECT_EQ(regFeat.GetNumRings(), 2);
  EXPECT_FALSE(regFeat.IsInteriorRing(0));
  EXPECT_TRUE(regFeat.IsInteriorRing(1));
  EXPECT_FALSE(regFeat.IsInteriorRing(2));
  EXPECT_THAT(regFeat.GetRingRef(0), NotNull());
  EXPECT_THAT(regFeat.GetRingRef(1), NotNull());
  EXPECT_EQ(regFeat.GetRingRef(2), nullptr);

  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &regFeat, 1));

  // 2. MultiPolygon.
  TABRegion mregFeat(defn);
  auto mp = std::make_unique<OGRMultiPolygon>();
  auto p1 = std::make_unique<OGRPolygon>();
  auto r1 = std::make_unique<OGRLinearRing>();
  r1->addPoint(0.0, 0.0);
  r1->addPoint(0.01, 0.0);
  r1->addPoint(0.01, 0.01);
  r1->addPoint(0.0, 0.01);
  r1->addPoint(0.0, 0.0);
  p1->addRingDirectly(r1.release());
  auto p2 = std::make_unique<OGRPolygon>();
  auto r2 = std::make_unique<OGRLinearRing>();
  r2->addPoint(0.02, 0.02);
  r2->addPoint(0.03, 0.02);
  r2->addPoint(0.03, 0.03);
  r2->addPoint(0.02, 0.03);
  r2->addPoint(0.02, 0.02);
  p2->addRingDirectly(r2.release());
  mp->addGeometryDirectly(p1.release());
  mp->addGeometryDirectly(p2.release());
  mregFeat.SetGeometryDirectly(mp.release());

  EXPECT_EQ(mregFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_REGION_C);
  EXPECT_EQ(mregFeat.GetNumRings(), 2);
  EXPECT_FALSE(mregFeat.IsInteriorRing(0));
  EXPECT_FALSE(mregFeat.IsInteriorRing(1));
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &mregFeat, 2));

  // 3. Uncompressed REGION.
  TABRegion regUnc(defn);
  auto pLarge = std::make_unique<OGRPolygon>();
  auto rLarge = std::make_unique<OGRLinearRing>();
  rLarge->addPoint(-500.0, -500.0);
  rLarge->addPoint(500.0, -500.0);
  rLarge->addPoint(500.0, 500.0);
  rLarge->addPoint(-500.0, 500.0);
  rLarge->addPoint(-500.0, -500.0);
  pLarge->addRingDirectly(rLarge.release());
  regUnc.SetGeometryDirectly(pLarge.release());
  EXPECT_EQ(regUnc.ValidateMapInfoType(mapFile.get()), TAB_GEOM_REGION);
  double ucx = 0, ucy = 0;
  EXPECT_EQ(0, regUnc.GetCenter(ucx, ucy));
  EXPECT_DOUBLE_EQ(ucx, 0.0);
  EXPECT_DOUBLE_EQ(ucy, 0.0);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &regUnc, 3));

  // Center.
  double cx = 0, cy = 0;
  EXPECT_EQ(0, regFeat.GetCenter(cx, cy));
  regFeat.SetCenter(1.0, 2.0);
  EXPECT_EQ(0, regFeat.GetCenter(cx, cy));
  EXPECT_DOUBLE_EQ(cx, 1.0);
  EXPECT_DOUBLE_EQ(cy, 2.0);

  // Invalid geometry.
  TABRegion regBad(defn);
  auto ptBad = std::make_unique<OGRPoint>(1.0, 2.0);
  regBad.SetGeometryDirectly(ptBad.release());
  EXPECT_EQ(regBad.ValidateMapInfoType(mapFile.get()), TAB_GEOM_NONE);
  CPLErrorReset();

  mapFile->Close();

  // Read back.
  ASSERT_EQ(0, mapFile->Open(mapPath.c_str(), "r"));

  TABRegion readReg(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readReg, 1));
  EXPECT_EQ(readReg.GetNumRings(), 2);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readReg, 1, TRUE));

  TABRegion readMreg(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readMreg, 2));
  EXPECT_EQ(readMreg.GetNumRings(), 2);

  mapFile->Close();

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  regFeat.DumpMIF(fp);
  regBad.DumpMIF(fp);
  CPLErrorReset();
  EXPECT_THAT(regFeat.GetStyleString(), NotNull());

  TABFeature* cloneReg = regFeat.CloneTABFeature(nullptr);
  ASSERT_THAT(cloneReg, NotNull());
  delete cloneReg;
}

TEST(MitabFeatureTest, RectangleMethodsAndMapFile) {
  const std::string base = "rect_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  auto* defn = new OGRFeatureDefn("rect_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  // RECT_C.
  TABRectangle rectFeat(defn);
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0.0, 0.0);
  ring->addPoint(0.01, 0.0);
  ring->addPoint(0.01, 0.01);
  ring->addPoint(0.0, 0.01);
  ring->addPoint(0.0, 0.0);
  poly->addRingDirectly(ring.release());
  rectFeat.SetGeometryDirectly(poly.release());

  EXPECT_EQ(rectFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_RECT);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &rectFeat, 1));

  // ROUNDRECT_C.
  TABRectangle roundRectFeat(defn);
  auto poly2 = std::make_unique<OGRPolygon>();
  auto ring2 = std::make_unique<OGRLinearRing>();
  ring2->addPoint(0.02, 0.02);
  ring2->addPoint(0.03, 0.02);
  ring2->addPoint(0.03, 0.03);
  ring2->addPoint(0.02, 0.03);
  ring2->addPoint(0.02, 0.02);
  poly2->addRingDirectly(ring2.release());
  roundRectFeat.SetGeometryDirectly(poly2.release());
  roundRectFeat.m_bRoundCorners = TRUE;
  roundRectFeat.m_dRoundXRadius = 0.001;
  roundRectFeat.m_dRoundYRadius = 0.001;
  EXPECT_TRUE(roundRectFeat.m_bRoundCorners);

  EXPECT_EQ(roundRectFeat.ValidateMapInfoType(mapFile.get()),
            TAB_GEOM_ROUNDRECT);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &roundRectFeat, 2));

  // Uncompressed RECT.
  TABRectangle rectUnc(defn);
  auto polyLarge = std::make_unique<OGRPolygon>();
  auto ringLarge = std::make_unique<OGRLinearRing>();
  ringLarge->addPoint(-500.0, -500.0);
  ringLarge->addPoint(500.0, -500.0);
  ringLarge->addPoint(500.0, 500.0);
  ringLarge->addPoint(-500.0, 500.0);
  ringLarge->addPoint(-500.0, -500.0);
  polyLarge->addRingDirectly(ringLarge.release());
  rectUnc.SetGeometryDirectly(polyLarge.release());
  EXPECT_EQ(rectUnc.ValidateMapInfoType(mapFile.get()), TAB_GEOM_RECT);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &rectUnc, 3));

  // Invalid geometry.
  TABRectangle rectBad(defn);
  auto ptBad = std::make_unique<OGRPoint>(1.0, 2.0);
  rectBad.SetGeometryDirectly(ptBad.release());
  EXPECT_EQ(rectBad.ValidateMapInfoType(mapFile.get()), TAB_GEOM_NONE);
  CPLErrorReset();

  mapFile->Close();

  // Read back.
  ASSERT_EQ(0, mapFile->Open(mapPath.c_str(), "r"));

  TABRectangle readRect(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readRect, 1));
  EXPECT_FALSE(readRect.m_bRoundCorners);

  TABRectangle readRoundRect(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readRoundRect, 2));
  EXPECT_TRUE(readRoundRect.m_bRoundCorners);

  mapFile->Close();

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  rectFeat.DumpMIF(fp);
  roundRectFeat.DumpMIF(fp);
  rectBad.DumpMIF(fp);
  CPLErrorReset();
  EXPECT_THAT(rectFeat.GetStyleString(), NotNull());

  TABFeature* cloneRect = rectFeat.CloneTABFeature(nullptr);
  ASSERT_THAT(cloneRect, NotNull());
  delete cloneRect;
}

TEST(MitabFeatureTest, EllipseMethodsAndMapFile) {
  const std::string base = "ell_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  auto* defn = new OGRFeatureDefn("ell_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  // 1. Polygon geometry -> ELLIPSE_C.
  TABEllipse ellFeat(defn);
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0.0, 0.0);
  ring->addPoint(0.01, 0.0);
  ring->addPoint(0.01, 0.01);
  ring->addPoint(0.0, 0.01);
  ring->addPoint(0.0, 0.0);
  poly->addRingDirectly(ring.release());
  ellFeat.SetGeometryDirectly(poly.release());

  EXPECT_EQ(ellFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_ELLIPSE);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &ellFeat, 1));

  // 2. Point geometry with radii -> ELLIPSE.
  TABEllipse ellPtFeat(defn);
  auto pt = std::make_unique<OGRPoint>(0.05, 0.05);
  ellPtFeat.SetGeometryDirectly(pt.release());
  ellPtFeat.m_dXRadius = 0.005;
  ellPtFeat.m_dYRadius = 0.003;
  EXPECT_EQ(ellPtFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_ELLIPSE);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &ellPtFeat, 2));

  // 3. Uncompressed ELLIPSE.
  TABEllipse ellUnc(defn);
  auto ptLarge = std::make_unique<OGRPoint>(0.0, 0.0);
  ellUnc.SetGeometryDirectly(ptLarge.release());
  ellUnc.m_dXRadius = 200.0;
  ellUnc.m_dYRadius = 150.0;
  EXPECT_EQ(ellUnc.ValidateMapInfoType(mapFile.get()), TAB_GEOM_ELLIPSE);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &ellUnc, 3));

  // Invalid geometry.
  TABEllipse ellBad(defn);
  auto lineBad = std::make_unique<OGRLineString>();
  ellBad.SetGeometryDirectly(lineBad.release());
  EXPECT_EQ(ellBad.ValidateMapInfoType(mapFile.get()), TAB_GEOM_NONE);
  CPLErrorReset();

  mapFile->Close();

  // Read back.
  ASSERT_EQ(0, mapFile->Open(mapPath.c_str(), "r"));

  TABEllipse readEll(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readEll, 1));
  EXPECT_NEAR(readEll.m_dXRadius, 0.005, 1e-4);

  mapFile->Close();

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  ellFeat.DumpMIF(fp);
  EXPECT_THAT(ellFeat.GetStyleString(), NotNull());

  TABFeature* cloneEll = ellFeat.CloneTABFeature(nullptr);
  ASSERT_THAT(cloneEll, NotNull());
  delete cloneEll;
}

TEST(MitabFeatureTest, ArcMethodsAndMapFile) {
  const std::string base = "arc_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  auto* defn = new OGRFeatureDefn("arc_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  // 1. LineString geometry.
  TABArc arcFeat(defn);
  auto line = std::make_unique<OGRLineString>();
  line->addPoint(0.0, 0.0);
  line->addPoint(0.01, 0.01);
  arcFeat.SetGeometryDirectly(line.release());
  arcFeat.SetStartAngle(45.0);
  arcFeat.SetEndAngle(135.0);
  EXPECT_DOUBLE_EQ(arcFeat.GetStartAngle(), 45.0);
  EXPECT_DOUBLE_EQ(arcFeat.GetEndAngle(), 135.0);

  EXPECT_EQ(arcFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_ARC);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &arcFeat, 1));

  // 2. Point geometry with radii and endAngle < startAngle.
  TABArc arcPtFeat(defn);
  auto pt = std::make_unique<OGRPoint>(0.05, 0.05);
  arcPtFeat.SetGeometryDirectly(pt.release());
  arcPtFeat.m_dXRadius = 0.01;
  arcPtFeat.m_dYRadius = 0.01;
  arcPtFeat.SetStartAngle(315.0);
  arcPtFeat.SetEndAngle(45.0);
  EXPECT_EQ(arcPtFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_ARC);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &arcPtFeat, 2));

  // Invalid geometry.
  TABArc arcBad(defn);
  auto polyBad = std::make_unique<OGRPolygon>();
  arcBad.SetGeometryDirectly(polyBad.release());
  EXPECT_EQ(arcBad.ValidateMapInfoType(mapFile.get()), TAB_GEOM_NONE);
  CPLErrorReset();

  mapFile->Close();

  // Read back.
  ASSERT_EQ(0, mapFile->Open(mapPath.c_str(), "r"));

  TABArc readArc(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readArc, 1));
  EXPECT_NEAR(readArc.GetStartAngle(), 45.0, 1.0);
  EXPECT_NEAR(readArc.GetEndAngle(), 135.0, 1.0);

  // bCoordBlockDataOnly branch.
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readArc, 1, TRUE));

  mapFile->Close();

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  arcFeat.DumpMIF(fp);
  EXPECT_THAT(arcFeat.GetStyleString(), NotNull());

  TABFeature* cloneArc = arcFeat.CloneTABFeature(nullptr);
  ASSERT_THAT(cloneArc, NotNull());
  delete cloneArc;
}

TEST(MitabFeatureTest, TextMethodsAndMapFile) {
  const std::string base = "txt_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  auto* defn = new OGRFeatureDefn("txt_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  TABText txtFeat(defn);
  auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
  txtFeat.SetGeometryDirectly(pt.release());
  txtFeat.SetTextString("Sample Text");
  EXPECT_STREQ(txtFeat.GetTextString(), "Sample Text");

  txtFeat.SetTextAngle(45.0);
  EXPECT_DOUBLE_EQ(txtFeat.GetTextAngle(), 45.0);

  txtFeat.SetTextBoxHeight(0.01);
  EXPECT_DOUBLE_EQ(txtFeat.GetTextBoxHeight(), 0.01);
  txtFeat.SetTextBoxWidth(0.05);
  EXPECT_DOUBLE_EQ(txtFeat.GetTextBoxWidth(), 0.05);

  txtFeat.SetFontFGColor(0x112233);
  EXPECT_EQ(txtFeat.GetFontFGColor(), 0x112233);
  txtFeat.SetFontBGColor(0x445566);
  EXPECT_EQ(txtFeat.GetFontBGColor(), 0x445566);
  txtFeat.SetFontOColor(0x778899);
  EXPECT_EQ(txtFeat.GetFontOColor(), 0x778899);
  txtFeat.SetFontSColor(0x8899aa);
  EXPECT_EQ(txtFeat.GetFontSColor(), 0x8899aa);

  txtFeat.SetTextJustification(TABTJCenter);
  EXPECT_EQ(txtFeat.GetTextJustification(), TABTJCenter);

  txtFeat.SetTextSpacing(TABTS1_5);
  EXPECT_EQ(txtFeat.GetTextSpacing(), TABTS1_5);

  txtFeat.SetTextLineType(TABTLSimple);
  EXPECT_EQ(txtFeat.GetTextLineType(), TABTLSimple);
  double lx = 0, ly = 0;
  txtFeat.GetTextLineEndPoint(lx, ly);
  EXPECT_NEAR(lx, 1.014, 0.05);
  EXPECT_NEAR(ly, 2.021, 0.05);
  txtFeat.SetTextLineEndPoint(1.5, 2.5);
  txtFeat.GetTextLineEndPoint(lx, ly);
  EXPECT_DOUBLE_EQ(lx, 1.5);
  EXPECT_DOUBLE_EQ(ly, 2.5);

  txtFeat.ToggleFontStyle(TABFSBold, TRUE);
  EXPECT_TRUE(txtFeat.IsFontBold());
  txtFeat.ToggleFontStyle(TABFSItalic, TRUE);
  EXPECT_TRUE(txtFeat.IsFontItalic());
  txtFeat.ToggleFontStyle(TABFSUnderline, TRUE);
  EXPECT_TRUE(txtFeat.IsFontUnderline());

  EXPECT_EQ(txtFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_TEXT);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &txtFeat, 1));

  // Text across all 4 angle quadrants.
  double angles[] = {30.0, 120.0, 210.0, 300.0, -45.0};
  for (int i = 0; i < 5; ++i) {
    TABText txtAng(defn);
    auto p = std::make_unique<OGRPoint>(10.0 + i, 20.0 + i);
    txtAng.SetGeometryDirectly(p.release());
    txtAng.SetTextString("AngleText");
    txtAng.SetTextAngle(angles[i]);
    txtAng.SetTextLineType(TABTLArrow);
    txtAng.SetTextLineEndPoint(10.0 + i + 0.1, 20.0 + i + 0.1);
    EXPECT_EQ(txtAng.ValidateMapInfoType(mapFile.get()), TAB_GEOM_TEXT);
    EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &txtAng, 2 + i));
  }

  // SetLabelFromStyleString tests.
  txtFeat.SetLabelFromStyleString(
      "LABEL(t:\"Line1\\nLine2\",s:12pt,f:\"Arial\",bo:1,it:1,un:1,c:#112233,"
      "b:#445566,o:#778899,a:30,p:1)");
  EXPECT_STREQ(txtFeat.GetTextString(), "Line1\\nLine2");
  EXPECT_STREQ(txtFeat.GetFontNameRef(), "Arial");
  EXPECT_EQ(txtFeat.GetTextJustification(), TABTJLeft);

  txtFeat.SetLabelFromStyleString("LABEL(p:2)");
  EXPECT_EQ(txtFeat.GetTextJustification(), TABTJCenter);
  txtFeat.SetLabelFromStyleString("LABEL(p:3)");
  EXPECT_EQ(txtFeat.GetTextJustification(), TABTJRight);

  // Invalid geometry.
  TABText txtBad(defn);
  auto lineBad = std::make_unique<OGRLineString>();
  txtBad.SetGeometryDirectly(lineBad.release());
  EXPECT_EQ(txtBad.ValidateMapInfoType(mapFile.get()), TAB_GEOM_NONE);
  CPLErrorReset();

  mapFile->Close();

  // Read back.
  ASSERT_EQ(0, mapFile->Open(mapPath.c_str(), "r"));

  TABText readTxt(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readTxt, 1));
  EXPECT_STREQ(readTxt.GetTextString(), "Sample Text");
  EXPECT_EQ(readTxt.GetTextLineType(), TABTLSimple);

  for (int i = 0; i < 5; ++i) {
    TABText readAng(defn);
    EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readAng, 2 + i));
    EXPECT_EQ(readAng.GetTextLineType(), TABTLArrow);
  }

  mapFile->Close();

  TABText styleTxt(defn);
  styleTxt.SetTextString("Hello \"World\"\\nLine2");
  styleTxt.SetTextBoxHeight(10.0);
  styleTxt.SetTextSpacing(TABTS1_5);
  styleTxt.SetTextJustification(TABTJCenter);
  styleTxt.ToggleFontStyle(TABFSAllCaps, TRUE);
  styleTxt.ToggleFontStyle(TABFSExpanded, TRUE);
  EXPECT_THAT(styleTxt.GetStyleString(), NotNull());

  TABText styleTxt2(defn);
  styleTxt2.SetTextString("Line1\nLine2");
  styleTxt2.SetTextBoxHeight(10.0);
  styleTxt2.SetTextSpacing(TABTSDouble);
  styleTxt2.SetTextJustification(TABTJLeft);
  EXPECT_THAT(styleTxt2.GetStyleString(), NotNull());
  EXPECT_GE(styleTxt2.GetFontStyleMIFValue(), 0);
  styleTxt2.SetFontStyleMIFValue(1, TRUE);
  styleTxt2.SetLabelFromStyleString(
      "PEN(w:1px);LABEL(c:#11223344,b:#55667788,o:#99aabbcc)");

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  txtFeat.DumpMIF(fp);
  EXPECT_THAT(txtFeat.GetStyleString(), NotNull());

  TABFeature* cloneTxt = txtFeat.CloneTABFeature(nullptr);
  ASSERT_THAT(cloneTxt, NotNull());
  delete cloneTxt;
}

TEST(MitabFeatureTest, MultiPointMethodsAndMapFile) {
  const std::string base = "mpt_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  auto* defn = new OGRFeatureDefn("mpt_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  TABMultiPoint mptFeat(defn);
  auto mp = std::make_unique<OGRMultiPoint>();
  auto p1 = std::make_unique<OGRPoint>(1.0, 2.0);
  auto p2 = std::make_unique<OGRPoint>(1.01, 2.01);
  mp->addGeometryDirectly(p1.release());
  mp->addGeometryDirectly(p2.release());
  mptFeat.SetGeometryDirectly(mp.release());

  EXPECT_EQ(mptFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_MULTIPOINT_C);
  EXPECT_EQ(mptFeat.GetNumPoints(), 2);
  double x = 0, y = 0;
  EXPECT_EQ(0, mptFeat.GetXY(0, x, y));
  EXPECT_DOUBLE_EQ(x, 1.0);
  EXPECT_DOUBLE_EQ(y, 2.0);
  EXPECT_EQ(0, mptFeat.GetXY(5, x, y));

  double cx = 0, cy = 0;
  EXPECT_EQ(0, mptFeat.GetCenter(cx, cy));
  mptFeat.SetCenter(1.0, 2.0);

  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &mptFeat, 1));

  // Invalid geometry.
  TABMultiPoint mptBad(defn);
  EXPECT_EQ(-1, mptBad.GetXY(0, x, y));
  CPLErrorReset();
  auto lineBad = std::make_unique<OGRLineString>();
  mptBad.SetGeometryDirectly(lineBad.release());
  EXPECT_EQ(mptBad.ValidateMapInfoType(mapFile.get()), TAB_GEOM_NONE);
  CPLErrorReset();

  mapFile->Close();

  // Read back.
  ASSERT_EQ(0, mapFile->Open(mapPath.c_str(), "r"));

  TABMultiPoint readMpt(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readMpt, 1));
  EXPECT_EQ(readMpt.GetNumPoints(), 2);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readMpt, 1, TRUE));

  mapFile->Close();

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  mptFeat.DumpMIF(fp);
  EXPECT_THAT(mptFeat.GetStyleString(), NotNull());

  TABFeature* cloneMpt = mptFeat.CloneTABFeature(nullptr);
  ASSERT_THAT(cloneMpt, NotNull());
  delete cloneMpt;
}

TEST(MitabFeatureTest, CollectionMethodsAndMapFile) {
  const std::string base = "coll_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  auto* defn = new OGRFeatureDefn("coll_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  TABCollection collFeat(defn);

  auto reg = std::make_unique<TABRegion>(defn);
  auto poly = std::make_unique<OGRPolygon>();
  auto ring = std::make_unique<OGRLinearRing>();
  ring->addPoint(0.0, 0.0);
  ring->addPoint(0.01, 0.0);
  ring->addPoint(0.01, 0.01);
  ring->addPoint(0.0, 0.01);
  ring->addPoint(0.0, 0.0);
  poly->addRingDirectly(ring.release());
  reg->SetGeometryDirectly(poly.release());
  collFeat.SetRegionDirectly(reg.release());

  auto pline = std::make_unique<TABPolyline>(defn);
  auto line = std::make_unique<OGRLineString>();
  line->addPoint(0.02, 0.02);
  line->addPoint(0.03, 0.03);
  pline->SetGeometryDirectly(line.release());
  collFeat.SetPolylineDirectly(pline.release());

  auto mpt = std::make_unique<TABMultiPoint>(defn);
  auto mp = std::make_unique<OGRMultiPoint>();
  auto pt = std::make_unique<OGRPoint>(0.04, 0.04);
  mp->addGeometryDirectly(pt.release());
  mpt->SetGeometryDirectly(mp.release());
  collFeat.SetMultiPointDirectly(mpt.release());

  EXPECT_EQ(collFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_COLLECTION_C);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &collFeat, 1));

  // Uncompressed collection (ID 2).
  TABCollection collUnc(defn);
  auto regU = std::make_unique<TABRegion>(defn);
  auto polyU = std::make_unique<OGRPolygon>();
  auto ringU = std::make_unique<OGRLinearRing>();
  ringU->addPoint(-500.0, -500.0);
  ringU->addPoint(500.0, -500.0);
  ringU->addPoint(500.0, 500.0);
  ringU->addPoint(-500.0, 500.0);
  ringU->addPoint(-500.0, -500.0);
  polyU->addRingDirectly(ringU.release());
  regU->SetGeometryDirectly(polyU.release());
  collUnc.SetRegionDirectly(regU.release());
  EXPECT_EQ(collUnc.ValidateMapInfoType(mapFile.get()), TAB_GEOM_COLLECTION);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &collUnc, 2));

  // Empty collection after clearing existing pointers (ID 3).
  collFeat.SetRegionDirectly(nullptr);
  collFeat.SetPolylineDirectly(nullptr);
  collFeat.SetMultiPointDirectly(nullptr);
  EXPECT_EQ(collFeat.ValidateMapInfoType(mapFile.get()), TAB_GEOM_COLLECTION_C);
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &collFeat, 3));

  // Invalid geometry in collection.
  TABCollection collBad(defn);
  auto ptBad = std::make_unique<OGRPoint>(1.0, 2.0);
  collBad.SetGeometryDirectly(ptBad.release());
  EXPECT_EQ(collBad.ValidateMapInfoType(mapFile.get()), TAB_GEOM_NONE);
  CPLErrorReset();

  mapFile->Close();

  // Read back.
  ASSERT_EQ(0, mapFile->Open(mapPath.c_str(), "r"));

  TABCollection readColl(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readColl, 1));
  EXPECT_THAT(readColl.GetRegionRef(), NotNull());
  EXPECT_THAT(readColl.GetPolylineRef(), NotNull());
  EXPECT_THAT(readColl.GetMultiPointRef(), NotNull());

  TABCollection readCollUnc(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readCollUnc, 2));

  TABCollection readCollEmpty(defn);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &readCollEmpty, 3));

  mapFile->Close();

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  collFeat.DumpMIF(fp);
  EXPECT_THAT(collFeat.GetStyleString(), NotNull());

  TABFeature* cloneColl = collFeat.CloneTABFeature(nullptr);
  ASSERT_THAT(cloneColl, NotNull());
  delete cloneColl;
}

TEST(MitabFeatureTest, DebugFeatureMethods) {
  auto* defn = new OGRFeatureDefn("dbg_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  TABDebugFeature dbg(defn);
  EXPECT_EQ(dbg.GetMapInfoType(), TAB_GEOM_NONE);

  // WriteGeometryToMAPFile returns -1 and generates an error.
  CPLErrorReset();
  EXPECT_EQ(-1, dbg.WriteGeometryToMAPFile(nullptr, nullptr));
  EXPECT_NE(CPLGetLastErrorNo(), CPLE_None);

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, NotNull());
  absl::Cleanup fp_cleanup = [fp] { fclose(fp); };
  dbg.DumpMIF(fp);

  // Test ReadGeometryFromMAPFile on both non-coord block and coord block
  // features.
  const std::string base = "dbg_map";
  const std::string mapPath = "/vsimem/" + base + ".map";
  const std::string idPath = "/vsimem/" + base + ".id";
  absl::Cleanup map_clean = [&] {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  auto mapFile = CreateMemoryMapFile(base);
  ASSERT_THAT(mapFile, NotNull());

  // ID 1: Point (no coord block)
  TABPoint pt(defn);
  auto p = std::make_unique<OGRPoint>(1.0, 2.0);
  pt.SetGeometryDirectly(p.release());
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &pt, 1));

  // ID 2: Polyline with 3 vertices (uses coord block)
  TABPolyline pline(defn);
  auto line = std::make_unique<OGRLineString>();
  line->addPoint(1.0, 2.0);
  line->addPoint(3.0, 4.0);
  line->addPoint(5.0, 6.0);
  pline.SetGeometryDirectly(line.release());
  EXPECT_EQ(0, WriteFeatureToMap(mapFile.get(), &pline, 2));

  mapFile->Close();

  ASSERT_EQ(0, mapFile->Open(mapPath.c_str(), "r"));
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &dbg, 1));
  dbg.DumpMIF(fp);
  EXPECT_EQ(0, ReadFeatureFromMap(mapFile.get(), &dbg, 2));
  dbg.DumpMIF(fp);
  mapFile->Close();
}

}  // namespace
}  // namespace autotest2
