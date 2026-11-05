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

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_featurestyle.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

// ============================================================================
// OGRStyleTool::GetRGBFromString and GetSpecificId
// ============================================================================

TEST(OGRFeatureStyleTest, GetRGBFromString) {
  int r = 0, g = 0, b = 0, a = 255;

  // #RRGGBB
  EXPECT_TRUE(OGRStyleTool::GetRGBFromString("#FF0000", r, g, b, a));
  EXPECT_EQ(r, 255);
  EXPECT_EQ(g, 0);
  EXPECT_EQ(b, 0);
  EXPECT_EQ(a, 255);

  EXPECT_TRUE(OGRStyleTool::GetRGBFromString("#00FF00", r, g, b, a));
  EXPECT_EQ(r, 0);
  EXPECT_EQ(g, 255);
  EXPECT_EQ(b, 0);

  EXPECT_TRUE(OGRStyleTool::GetRGBFromString("#0000FF", r, g, b, a));
  EXPECT_EQ(r, 0);
  EXPECT_EQ(g, 0);
  EXPECT_EQ(b, 255);

  // #RRGGBBAA
  EXPECT_TRUE(OGRStyleTool::GetRGBFromString("#11223344", r, g, b, a));
  EXPECT_EQ(r, 0x11);
  EXPECT_EQ(g, 0x22);
  EXPECT_EQ(b, 0x33);
  EXPECT_EQ(a, 0x44);

  // Invalid cases
  EXPECT_FALSE(OGRStyleTool::GetRGBFromString(nullptr, r, g, b, a));
  EXPECT_FALSE(OGRStyleTool::GetRGBFromString("", r, g, b, a));
  EXPECT_FALSE(
      OGRStyleTool::GetRGBFromString("FF0000", r, g, b, a));  // missing #
  EXPECT_FALSE(OGRStyleTool::GetRGBFromString("#12", r, g, b, a));
  EXPECT_FALSE(
      OGRStyleTool::GetRGBFromString("#GG0000", r, g, b, a));  // invalid hex
}

TEST(OGRFeatureStyleTest, GetSpecificId) {
  EXPECT_EQ(OGRStyleTool::GetSpecificId("ogr-pen-123", "ogr-pen"), 123);
  EXPECT_EQ(OGRStyleTool::GetSpecificId("ogr-pen", "ogr-pen"), 0);
  EXPECT_EQ(OGRStyleTool::GetSpecificId("ogr-pen-456", nullptr), 456);
  EXPECT_EQ(OGRStyleTool::GetSpecificId(nullptr, "ogr-pen"), -1);
  EXPECT_EQ(OGRStyleTool::GetSpecificId("other123", "ogr-pen"), -1);
}

// ============================================================================
// OGRStyleTool Unit and Scale Computations
// ============================================================================

TEST(OGRFeatureStyleTest, StyleToolUnits) {
  OGRStylePen pen;
  EXPECT_EQ(pen.GetType(), OGRSTCPen);

  pen.SetUnit(OGRSTUMM, 1.0);
  EXPECT_EQ(pen.GetUnit(), OGRSTUMM);

  pen.SetUnit(OGRSTUPixel, 1.0);
  EXPECT_EQ(pen.GetUnit(), OGRSTUPixel);

  pen.SetUnit(OGRSTUPoints, 1.0);
  EXPECT_EQ(pen.GetUnit(), OGRSTUPoints);

  pen.SetUnit(OGRSTUCM, 1.0);
  EXPECT_EQ(pen.GetUnit(), OGRSTUCM);

  pen.SetUnit(OGRSTUInches, 1.0);
  EXPECT_EQ(pen.GetUnit(), OGRSTUInches);

  pen.SetUnit(OGRSTUGround, 2.0);
  EXPECT_EQ(pen.GetUnit(), OGRSTUGround);

  // ComputeWithUnit (double and int)
  pen.SetUnit(OGRSTUMM, 1.0);
  EXPECT_NEAR(pen.ComputeWithUnit(10.0, OGRSTUMM), 10.0, 1e-5);
  EXPECT_NEAR(pen.ComputeWithUnit(1.0, OGRSTUCM), 10.0, 1e-5);
  EXPECT_NEAR(pen.ComputeWithUnit(1.0, OGRSTUInches), 1000.0 / 39.37, 1e-5);
  EXPECT_GT(pen.ComputeWithUnit(10.0, OGRSTUPoints), 0.0);
  EXPECT_GT(pen.ComputeWithUnit(10.0, OGRSTUPixel), 0.0);

  EXPECT_EQ(pen.ComputeWithUnit(10, OGRSTUMM), 10);
  EXPECT_EQ(pen.ComputeWithUnit(1, OGRSTUCM), 10);

  // Ground unit
  pen.SetUnit(OGRSTUGround, 1000.0);
  EXPECT_GT(pen.ComputeWithUnit(1.0, OGRSTUGround), 0.0);
  EXPECT_GT(pen.ComputeWithUnit(1, OGRSTUGround), 0);

  // SetInternalInputUnitFromParam
  char paramGround[] = "10g";
  pen.SetInternalInputUnitFromParam(paramGround);
  char paramPx[] = "10px";
  pen.SetInternalInputUnitFromParam(paramPx);
  char paramPt[] = "10pt";
  pen.SetInternalInputUnitFromParam(paramPt);
  char paramMm[] = "10mm";
  pen.SetInternalInputUnitFromParam(paramMm);
  char paramCm[] = "10cm";
  pen.SetInternalInputUnitFromParam(paramCm);
  char paramIn[] = "10in";
  pen.SetInternalInputUnitFromParam(paramIn);
  char paramNoUnit[] = "10";
  pen.SetInternalInputUnitFromParam(paramNoUnit);
  pen.SetInternalInputUnitFromParam(nullptr);

  // Modified / Parsed flags
  EXPECT_FALSE(pen.IsStyleModified());
  pen.StyleModified();
  EXPECT_TRUE(pen.IsStyleModified());

  EXPECT_FALSE(pen.IsStyleParsed());
  pen.StyleParsed();
  EXPECT_TRUE(pen.IsStyleParsed());
}

// ============================================================================
// OGRStylePen
// ============================================================================

TEST(OGRFeatureStyleTest, StylePen) {
  OGRStylePen pen;
  EXPECT_EQ(pen.GetType(), OGRSTCPen);

  GBool bDefault = FALSE;

  // Defaults
  pen.Color(bDefault);
  EXPECT_TRUE(bDefault);

  // Setters
  pen.SetColor("#00FF00FF");
  EXPECT_STREQ(pen.Color(bDefault), "#00FF00FF");
  EXPECT_FALSE(bDefault);

  pen.SetWidth(2.5);
  EXPECT_NEAR(pen.Width(bDefault), 2.5, 1e-5);
  EXPECT_FALSE(bDefault);

  OGRSTUnitId rawUnit = OGRSTUMM;
  EXPECT_NEAR(pen.RawWidth(rawUnit, bDefault), 2.5, 1e-5);

  pen.SetPattern("5 2");
  EXPECT_STREQ(pen.Pattern(bDefault), "5 2");

  pen.SetId("pen_id");
  EXPECT_STREQ(pen.Id(bDefault), "pen_id");

  pen.SetPerpendicularOffset(1.2);
  EXPECT_NEAR(pen.PerpendicularOffset(bDefault), 1.2, 1e-5);

  pen.SetCap("round");
  EXPECT_STREQ(pen.Cap(bDefault), "round");

  pen.SetJoin("mitre");
  EXPECT_STREQ(pen.Join(bDefault), "mitre");

  pen.SetPriority(7);
  EXPECT_EQ(pen.Priority(bDefault), 7);

  const char* pszStyle = pen.GetStyleString();
  ASSERT_NE(pszStyle, nullptr);
  EXPECT_NE(strstr(pszStyle, "PEN("), nullptr);
  EXPECT_NE(strstr(pszStyle, "c:#00FF00FF"), nullptr);

  // Parse existing style string
  OGRStylePen pen2;
  pen2.SetStyleString(
      "PEN(c:#FF0000,w:3.0mm,p:\"2 2\",id:\"p1\",dp:0.5,cap:butt,j:bevel,l:3)");
  EXPECT_STREQ(pen2.Color(bDefault), "#FF0000");
  EXPECT_NEAR(pen2.Width(bDefault), 3.0, 1e-5);
  EXPECT_STREQ(pen2.Pattern(bDefault), "2 2");
  EXPECT_STREQ(pen2.Id(bDefault), "p1");
  EXPECT_NEAR(pen2.PerpendicularOffset(bDefault), 0.5, 1e-5);
  EXPECT_STREQ(pen2.Cap(bDefault), "butt");
  EXPECT_STREQ(pen2.Join(bDefault), "bevel");
  EXPECT_EQ(pen2.Priority(bDefault), 3);
}

// ============================================================================
// OGRStyleBrush
// ============================================================================

TEST(OGRFeatureStyleTest, StyleBrush) {
  OGRStyleBrush brush;
  EXPECT_EQ(brush.GetType(), OGRSTCBrush);

  GBool bDefault = FALSE;

  brush.SetForeColor("#FF0000");
  EXPECT_STREQ(brush.ForeColor(bDefault), "#FF0000");

  brush.SetBackColor("#0000FF");
  EXPECT_STREQ(brush.BackColor(bDefault), "#0000FF");

  brush.SetId("cross_hatch");
  EXPECT_STREQ(brush.Id(bDefault), "cross_hatch");

  brush.SetAngle(45.0);
  EXPECT_NEAR(brush.Angle(bDefault), 45.0, 1e-5);

  brush.SetSize(12.0);
  EXPECT_NEAR(brush.Size(bDefault), 12.0, 1e-5);

  brush.SetSpacingX(3.0);
  EXPECT_NEAR(brush.SpacingX(bDefault), 3.0, 1e-5);

  brush.SetSpacingY(4.0);
  EXPECT_NEAR(brush.SpacingY(bDefault), 4.0, 1e-5);

  brush.SetPriority(2);
  EXPECT_EQ(brush.Priority(bDefault), 2);

  const char* pszStyle = brush.GetStyleString();
  ASSERT_NE(pszStyle, nullptr);
  EXPECT_NE(strstr(pszStyle, "BRUSH("), nullptr);

  // Parse existing brush string
  OGRStyleBrush brush2;
  brush2.SetStyleString(
      "BRUSH(fc:#112233,bc:#445566,id:\"b1\",a:90,s:5,dx:1,dy:2,l:4)");
  EXPECT_STREQ(brush2.ForeColor(bDefault), "#112233");
  EXPECT_STREQ(brush2.BackColor(bDefault), "#445566");
  EXPECT_STREQ(brush2.Id(bDefault), "b1");
  EXPECT_NEAR(brush2.Angle(bDefault), 90.0, 1e-5);
  EXPECT_NEAR(brush2.Size(bDefault), 5.0, 1e-5);
  EXPECT_NEAR(brush2.SpacingX(bDefault), 1.0, 1e-5);
  EXPECT_NEAR(brush2.SpacingY(bDefault), 2.0, 1e-5);
  EXPECT_EQ(brush2.Priority(bDefault), 4);
}

// ============================================================================
// OGRStyleSymbol
// ============================================================================

TEST(OGRFeatureStyleTest, StyleSymbol) {
  OGRStyleSymbol sym;
  EXPECT_EQ(sym.GetType(), OGRSTCSymbol);

  GBool bDefault = FALSE;

  sym.SetId("star");
  EXPECT_STREQ(sym.Id(bDefault), "star");

  sym.SetAngle(180.0);
  EXPECT_NEAR(sym.Angle(bDefault), 180.0, 1e-5);

  sym.SetColor("#FFFF00");
  EXPECT_STREQ(sym.Color(bDefault), "#FFFF00");

  sym.SetSize(8.0);
  EXPECT_NEAR(sym.Size(bDefault), 8.0, 1e-5);

  sym.SetSpacingX(2.0);
  EXPECT_NEAR(sym.SpacingX(bDefault), 2.0, 1e-5);

  sym.SetSpacingY(3.0);
  EXPECT_NEAR(sym.SpacingY(bDefault), 3.0, 1e-5);

  sym.SetStep(1.5);
  EXPECT_NEAR(sym.Step(bDefault), 1.5, 1e-5);

  sym.SetOffset(0.8);
  EXPECT_NEAR(sym.Offset(bDefault), 0.8, 1e-5);

  sym.SetPerp(0.5);
  EXPECT_NEAR(sym.Perp(bDefault), 0.5, 1e-5);

  sym.SetPriority(1);
  EXPECT_EQ(sym.Priority(bDefault), 1);

  sym.SetFontName("Wingdings");
  EXPECT_STREQ(sym.FontName(bDefault), "Wingdings");

  sym.SetOColor("#000000");
  EXPECT_STREQ(sym.OColor(bDefault), "#000000");

  const char* pszStyle = sym.GetStyleString();
  ASSERT_NE(pszStyle, nullptr);
  EXPECT_NE(strstr(pszStyle, "SYMBOL("), nullptr);

  // Parse existing symbol string
  OGRStyleSymbol sym2;
  sym2.SetStyleString(
      "SYMBOL(id:\"circle\",a:30,c:#00FFFF,s:10,dx:1,dy:1,ds:2,dp:3,di:4,l:5,f:"
      "\"Arial\",o:#123456)");
  EXPECT_STREQ(sym2.Id(bDefault), "circle");
  EXPECT_NEAR(sym2.Angle(bDefault), 30.0, 1e-5);
  EXPECT_STREQ(sym2.Color(bDefault), "#00FFFF");
  EXPECT_NEAR(sym2.Size(bDefault), 10.0, 1e-5);
  EXPECT_NEAR(sym2.SpacingX(bDefault), 1.0, 1e-5);
  EXPECT_NEAR(sym2.SpacingY(bDefault), 1.0, 1e-5);
  EXPECT_NEAR(sym2.Step(bDefault), 2.0, 1e-5);
  EXPECT_NEAR(sym2.Offset(bDefault), 4.0, 1e-5);
  EXPECT_NEAR(sym2.Perp(bDefault), 3.0, 1e-5);
  EXPECT_EQ(sym2.Priority(bDefault), 5);
  EXPECT_STREQ(sym2.FontName(bDefault), "Arial");
  EXPECT_STREQ(sym2.OColor(bDefault), "#123456");
}

// ============================================================================
// OGRStyleLabel
// ============================================================================

TEST(OGRFeatureStyleTest, StyleLabel) {
  OGRStyleLabel label;
  EXPECT_EQ(label.GetType(), OGRSTCLabel);

  GBool bDefault = FALSE;

  label.SetFontName("Helvetica");
  EXPECT_STREQ(label.FontName(bDefault), "Helvetica");

  label.SetSize(14.0);
  EXPECT_NEAR(label.Size(bDefault), 14.0, 1e-5);

  label.SetTextString("Hello World");
  EXPECT_STREQ(label.TextString(bDefault), "Hello World");

  label.SetAngle(15.0);
  EXPECT_NEAR(label.Angle(bDefault), 15.0, 1e-5);

  label.SetForColor("#333333");
  EXPECT_STREQ(label.ForeColor(bDefault), "#333333");

  label.SetBackColor("#EEEEEE");
  EXPECT_STREQ(label.BackColor(bDefault), "#EEEEEE");

  label.SetPlacement("line");
  EXPECT_STREQ(label.Placement(bDefault), "line");

  label.SetAnchor(5);
  EXPECT_EQ(label.Anchor(bDefault), 5);

  label.SetSpacingX(1.0);
  EXPECT_NEAR(label.SpacingX(bDefault), 1.0, 1e-5);

  label.SetSpacingY(1.5);
  EXPECT_NEAR(label.SpacingY(bDefault), 1.5, 1e-5);

  label.SetPerp(2.0);
  EXPECT_NEAR(label.Perp(bDefault), 2.0, 1e-5);

  label.SetBold(TRUE);
  EXPECT_TRUE(label.Bold(bDefault));

  label.SetItalic(TRUE);
  EXPECT_TRUE(label.Italic(bDefault));

  label.SetUnderline(TRUE);
  EXPECT_TRUE(label.Underline(bDefault));

  label.SetStrikeout(TRUE);
  EXPECT_TRUE(label.Strikeout(bDefault));

  label.SetPriority(8);
  EXPECT_EQ(label.Priority(bDefault), 8);

  label.SetStretch(1.25);
  EXPECT_NEAR(label.Stretch(bDefault), 1.25, 1e-5);

  label.SetShadowColor("#111111");
  EXPECT_STREQ(label.ShadowColor(bDefault), "#111111");

  label.SetOutlineColor("#999999");
  EXPECT_STREQ(label.OutlineColor(bDefault), "#999999");

  const char* pszStyle = label.GetStyleString();
  ASSERT_NE(pszStyle, nullptr);
  EXPECT_NE(strstr(pszStyle, "LABEL("), nullptr);

  // Parse existing label string
  OGRStyleLabel label2;
  label2.SetStyleString(
      "LABEL(f:\"Courier\",s:10,t:\"Sample\",a:45,c:#000000,b:#FFFFFF,m:p,p:1,"
      "dx:0,dy:0,dp:0,bo:0,it:0,un:0,st:0,l:1,w:1.0,h:#222222,o:#444444)");
  EXPECT_STREQ(label2.FontName(bDefault), "Courier");
  EXPECT_NEAR(label2.Size(bDefault), 10.0, 1e-5);
  EXPECT_STREQ(label2.TextString(bDefault), "Sample");
  EXPECT_NEAR(label2.Angle(bDefault), 45.0, 1e-5);
  EXPECT_STREQ(label2.ForeColor(bDefault), "#000000");
  EXPECT_STREQ(label2.BackColor(bDefault), "#FFFFFF");
  EXPECT_STREQ(label2.Placement(bDefault), "p");
  EXPECT_EQ(label2.Anchor(bDefault), 1);
  EXPECT_FALSE(label2.Bold(bDefault));
  EXPECT_FALSE(label2.Italic(bDefault));
  EXPECT_FALSE(label2.Underline(bDefault));
  EXPECT_FALSE(label2.Strikeout(bDefault));
  EXPECT_EQ(label2.Priority(bDefault), 1);
  EXPECT_NEAR(label2.Stretch(bDefault), 1.0, 1e-5);
  EXPECT_STREQ(label2.ShadowColor(bDefault), "#222222");
  EXPECT_STREQ(label2.OutlineColor(bDefault), "#444444");
}

// ============================================================================
// OGRStyleTable
// ============================================================================

TEST(OGRFeatureStyleTest, StyleTableBasic) {
  OGRStyleTable table;

  EXPECT_EQ(table.IsExist("non_existent"), -1);
  EXPECT_EQ(table.Find("non_existent"), nullptr);

  EXPECT_TRUE(table.AddStyle("style1", "PEN(c:#FF0000)"));
  EXPECT_TRUE(table.AddStyle("style2", "BRUSH(fc:#00FF00)"));
  EXPECT_GE(table.IsExist("style1"), 0);
  EXPECT_GE(table.IsExist("style2"), 0);
  EXPECT_STREQ(table.Find("style1"), "PEN(c:#FF0000)");
  EXPECT_STREQ(table.Find("style2"), "BRUSH(fc:#00FF00)");

  // Duplicate AddStyle returns FALSE
  EXPECT_FALSE(table.AddStyle("style1", "PEN(c:#0000FF)"));

  // Find style
  EXPECT_STREQ(table.Find("style1"), "PEN(c:#FF0000)");
  EXPECT_EQ(table.Find("non_existent"), nullptr);

  // GetStyleName (returns style name given style string)
  EXPECT_STREQ(table.GetStyleName("PEN(c:#FF0000)"), "style1");
  EXPECT_EQ(table.GetStyleName("PEN(c:#999999)"), nullptr);

  // ModifyStyle (modifies existing or adds if not present)
  EXPECT_TRUE(table.ModifyStyle("style1", "PEN(c:#0000FF,w:2px)"));
  EXPECT_STREQ(table.Find("style1"), "PEN(c:#0000FF,w:2px)");
  EXPECT_TRUE(table.ModifyStyle("extra", "PEN(c:#000000)"));
  EXPECT_TRUE(table.RemoveStyle("extra"));

  // Reading iteration
  table.ResetStyleStringReading();
  const char* s1 = table.GetNextStyle();
  ASSERT_NE(s1, nullptr);
  const char* name1 = table.GetLastStyleName();
  ASSERT_NE(name1, nullptr);

  const char* s2 = table.GetNextStyle();
  ASSERT_NE(s2, nullptr);

  EXPECT_EQ(table.GetNextStyle(), nullptr);

  // Clone
  std::unique_ptr<OGRStyleTable> cloned(table.Clone());
  ASSERT_NE(cloned, nullptr);
  EXPECT_GE(cloned->IsExist("style1"), 0);
  EXPECT_GE(cloned->IsExist("style2"), 0);

  // RemoveStyle
  EXPECT_TRUE(table.RemoveStyle("style1"));
  EXPECT_EQ(table.IsExist("style1"), -1);
  EXPECT_FALSE(table.RemoveStyle("style1"));

  // Clear
  table.Clear();
  EXPECT_EQ(table.IsExist("style2"), -1);
}

TEST(OGRFeatureStyleTest, StyleTableFileIO) {
  constexpr const char* kFilename = "/vsimem/test_style_table.txt";
  auto cleanup = absl::MakeCleanup([]() { VSIUnlink(kFilename); });

  OGRStyleTable table;
  EXPECT_TRUE(table.AddStyle("road", "PEN(c:#000000,w:2px)"));
  EXPECT_TRUE(table.AddStyle("water", "BRUSH(fc:#0000FF)"));

  // Print to file
  FILE* fpPrint = tmpfile();
  ASSERT_NE(fpPrint, nullptr);
  table.Print(fpPrint);
  fclose(fpPrint);

  // Save to file
  EXPECT_TRUE(table.SaveStyleTable(kFilename));

  // Load from file
  OGRStyleTable loadedTable;
  EXPECT_TRUE(loadedTable.LoadStyleTable(kFilename));
  EXPECT_GE(loadedTable.IsExist("road"), 0);
  EXPECT_GE(loadedTable.IsExist("water"), 0);
  EXPECT_STREQ(loadedTable.Find("road"), "PEN(c:#000000,w:2px)");
  EXPECT_STREQ(loadedTable.Find("water"), "BRUSH(fc:#0000FF)");

  // Load non-existent file
  {
    WithQuietHandler holder;
    EXPECT_FALSE(loadedTable.LoadStyleTable("/vsimem/does_not_exist_123.txt"));
  }
}

// ============================================================================
// OGRStyleMgr
// ============================================================================

TEST(OGRFeatureStyleTest, StyleMgrBasic) {
  OGRStyleTable table;
  table.AddStyle("my_pen", "PEN(c:#FF0000,w:1px)");

  OGRStyleMgr mgr(&table);
  EXPECT_EQ(mgr.GetDataSetStyleTable(), &table);

  EXPECT_TRUE(mgr.InitStyleString("PEN(c:#00FF00);BRUSH(fc:#0000FF)"));
  EXPECT_EQ(mgr.GetPartCount(), 2);

  std::unique_ptr<OGRStyleTool> part0(mgr.GetPart(0));
  ASSERT_NE(part0, nullptr);
  EXPECT_EQ(part0->GetType(), OGRSTCPen);

  std::unique_ptr<OGRStyleTool> part1(mgr.GetPart(1));
  ASSERT_NE(part1, nullptr);
  EXPECT_EQ(part1->GetType(), OGRSTCBrush);

  EXPECT_EQ(mgr.GetPart(2), nullptr);
  EXPECT_EQ(mgr.GetPart(-1), nullptr);

  // AddPart with string
  EXPECT_TRUE(mgr.AddPart("SYMBOL(id:\"star\")"));
  EXPECT_EQ(mgr.GetPartCount(), 3);

  // AddPart with OGRStyleTool
  OGRStyleLabel label;
  label.SetTextString("City");
  EXPECT_TRUE(mgr.AddPart(&label));
  EXPECT_EQ(mgr.GetPartCount(), 4);

  // AddStyle
  EXPECT_TRUE(mgr.AddStyle("new_style", "PEN(c:#AABBCC)"));
  EXPECT_TRUE(table.IsExist("new_style"));
  EXPECT_STREQ(mgr.GetStyleByName("new_style"), "PEN(c:#AABBCC)");
  EXPECT_EQ(mgr.GetStyleByName("unknown"), nullptr);

  // GetStyleString
  const char* styleStr = mgr.GetStyleString();
  ASSERT_NE(styleStr, nullptr);
  EXPECT_NE(strstr(styleStr, "PEN("), nullptr);
  EXPECT_NE(strstr(styleStr, "BRUSH("), nullptr);
  EXPECT_NE(strstr(styleStr, "SYMBOL("), nullptr);
  EXPECT_NE(strstr(styleStr, "LABEL("), nullptr);
}

TEST(OGRFeatureStyleTest, StyleMgrWithFeature) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("test_layer");
  poDefn->Reference();
  auto cleanup = absl::MakeCleanup([poDefn]() { poDefn->Release(); });
  OGRFeature feat(poDefn);

  OGRStyleTable table;
  table.AddStyle("@road_style", "PEN(c:#123456,w:2px)");

  OGRStyleMgr mgr(&table);

  // SetFeatureStyleString with style string directly
  EXPECT_TRUE(
      mgr.SetFeatureStyleString(&feat, "PEN(c:#FF0000)", /*bNoMatching=*/TRUE));
  EXPECT_STREQ(feat.GetStyleString(), "PEN(c:#FF0000)");

  // InitFromFeature
  EXPECT_STREQ(mgr.InitFromFeature(&feat), "PEN(c:#FF0000)");
  EXPECT_STREQ(mgr.GetStyleString(&feat), "PEN(c:#FF0000)");

  // SetFeatureStyleString with matching in style table
  EXPECT_TRUE(mgr.SetFeatureStyleString(&feat, "PEN(c:#123456,w:2px)",
                                        /*bNoMatching=*/FALSE));
  EXPECT_STREQ(feat.GetStyleString(), "@road_style");

  // InitFromFeature when feature has style name referencing table
  EXPECT_STREQ(mgr.InitFromFeature(&feat), "PEN(c:#123456,w:2px)");
}

TEST(OGRFeatureStyleTest, StyleMgrCreateStyleToolFromStyleString) {
  std::unique_ptr<OGRStyleTool> pen(
      OGRStyleMgr::CreateStyleToolFromStyleString("PEN(c:#FF0000)"));
  ASSERT_NE(pen, nullptr);
  EXPECT_EQ(pen->GetType(), OGRSTCPen);

  std::unique_ptr<OGRStyleTool> brush(
      OGRStyleMgr::CreateStyleToolFromStyleString("BRUSH(fc:#00FF00)"));
  ASSERT_NE(brush, nullptr);
  EXPECT_EQ(brush->GetType(), OGRSTCBrush);

  std::unique_ptr<OGRStyleTool> sym(
      OGRStyleMgr::CreateStyleToolFromStyleString("SYMBOL(id:\"star\")"));
  ASSERT_NE(sym, nullptr);
  EXPECT_EQ(sym->GetType(), OGRSTCSymbol);

  std::unique_ptr<OGRStyleTool> label(
      OGRStyleMgr::CreateStyleToolFromStyleString("LABEL(t:\"Hello\")"));
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->GetType(), OGRSTCLabel);

  EXPECT_EQ(OGRStyleMgr::CreateStyleToolFromStyleString("UNKNOWN(foo:bar)"),
            nullptr);
  EXPECT_EQ(OGRStyleMgr::CreateStyleToolFromStyleString(nullptr), nullptr);
  EXPECT_EQ(OGRStyleMgr::CreateStyleToolFromStyleString(""), nullptr);
}

// ============================================================================
// C API Coverage: OGR_SM_*, OGR_STBL_*, OGR_ST_*
// ============================================================================

TEST(OGRFeatureStyleTest, CAPICoverage) {
  // Style Table C API
  OGRStyleTableH hSTBL = OGR_STBL_Create();
  ASSERT_NE(hSTBL, nullptr);

  EXPECT_TRUE(OGR_STBL_AddStyle(hSTBL, "pen1", "PEN(c:#FF0000)"));
  EXPECT_STREQ(OGR_STBL_Find(hSTBL, "pen1"), "PEN(c:#FF0000)");

  OGR_STBL_ResetStyleStringReading(hSTBL);
  EXPECT_STREQ(OGR_STBL_GetNextStyle(hSTBL), "PEN(c:#FF0000)");
  EXPECT_STREQ(OGR_STBL_GetLastStyleName(hSTBL), "pen1");

  constexpr const char* kTablePath = "/vsimem/capi_styles.txt";
  auto cleanup = absl::MakeCleanup([]() { VSIUnlink(kTablePath); });

  EXPECT_TRUE(OGR_STBL_SaveStyleTable(hSTBL, kTablePath));

  OGRStyleTableH hLoaded = OGR_STBL_Create();
  EXPECT_TRUE(OGR_STBL_LoadStyleTable(hLoaded, kTablePath));
  EXPECT_STREQ(OGR_STBL_Find(hLoaded, "pen1"), "PEN(c:#FF0000)");
  OGR_STBL_Destroy(hLoaded);

  // Style Manager C API
  OGRStyleMgrH hSM = OGR_SM_Create(hSTBL);
  ASSERT_NE(hSM, nullptr);

  EXPECT_TRUE(OGR_SM_InitStyleString(hSM, "PEN(c:#00FF00);BRUSH(fc:#0000FF)"));
  EXPECT_EQ(OGR_SM_GetPartCount(hSM, nullptr), 2);

  OGRStyleToolH hPart0 = OGR_SM_GetPart(hSM, 0, nullptr);
  ASSERT_NE(hPart0, nullptr);
  EXPECT_EQ(OGR_ST_GetType(hPart0), OGRSTCPen);

  EXPECT_TRUE(OGR_SM_AddStyle(hSM, "brush1", "BRUSH(fc:#123456)"));

  OGRStyleToolH hSym = OGR_ST_Create(OGRSTCSymbol);
  ASSERT_NE(hSym, nullptr);
  EXPECT_EQ(OGR_ST_GetType(hSym), OGRSTCSymbol);
  OGR_ST_SetParamStr(hSym, OGRSTSymbolId, "sym1");
  EXPECT_TRUE(OGR_SM_AddPart(hSM, hSym));
  OGR_ST_Destroy(hSym);

  // Feature with Style Manager C API
  OGRFeatureDefnH hDefn = OGR_FD_Create("layer");
  OGR_FD_Reference(hDefn);
  OGRFeatureH hFeat = OGR_F_Create(hDefn);

  OGR_F_SetStyleString(hFeat, "PEN(c:#AABBCC)");
  EXPECT_STREQ(OGR_SM_InitFromFeature(hSM, hFeat), "PEN(c:#AABBCC)");

  OGR_F_Destroy(hFeat);
  OGR_FD_Release(hDefn);

  OGR_ST_Destroy(hPart0);
  OGR_SM_Destroy(hSM);
  OGR_STBL_Destroy(hSTBL);
}

TEST(OGRFeatureStyleTest, StyleToolCAPICoverage) {
  OGRStyleToolH hTool = OGR_ST_Create(OGRSTCPen);
  ASSERT_NE(hTool, nullptr);

  EXPECT_EQ(OGR_ST_GetType(hTool), OGRSTCPen);

  OGR_ST_SetUnit(hTool, OGRSTUMM, 1.0);
  EXPECT_EQ(OGR_ST_GetUnit(hTool), OGRSTUMM);

  // Set and get parameters
  OGR_ST_SetParamStr(hTool, OGRSTPenColor, "#FF0000");
  int bNull = FALSE;
  EXPECT_STREQ(OGR_ST_GetParamStr(hTool, OGRSTPenColor, &bNull), "#FF0000");
  EXPECT_FALSE(bNull);

  OGR_ST_SetParamDbl(hTool, OGRSTPenWidth, 3.5);
  EXPECT_NEAR(OGR_ST_GetParamDbl(hTool, OGRSTPenWidth, &bNull), 3.5, 1e-5);
  EXPECT_FALSE(bNull);

  OGR_ST_SetParamNum(hTool, OGRSTPenPriority, 4);
  EXPECT_EQ(OGR_ST_GetParamNum(hTool, OGRSTPenPriority, &bNull), 4);
  EXPECT_FALSE(bNull);

  const char* pszStyle = OGR_ST_GetStyleString(hTool);
  ASSERT_NE(pszStyle, nullptr);
  EXPECT_NE(strstr(pszStyle, "PEN("), nullptr);

  int r = 0, g = 0, b = 0, a = 255;
  EXPECT_TRUE(OGR_ST_GetRGBFromString(hTool, "#00FF00", &r, &g, &b, &a));
  EXPECT_EQ(r, 0);
  EXPECT_EQ(g, 255);
  EXPECT_EQ(b, 0);

  OGR_ST_Destroy(hTool);

  // Brush C API tool
  OGRStyleToolH hBrush = OGR_ST_Create(OGRSTCBrush);
  ASSERT_NE(hBrush, nullptr);
  OGR_ST_SetParamStr(hBrush, OGRSTBrushFColor, "#112233");
  OGR_ST_SetParamDbl(hBrush, OGRSTBrushAngle, 45.0);
  OGR_ST_SetParamNum(hBrush, OGRSTBrushPriority, 2);
  EXPECT_STREQ(OGR_ST_GetParamStr(hBrush, OGRSTBrushFColor, &bNull), "#112233");
  EXPECT_NEAR(OGR_ST_GetParamDbl(hBrush, OGRSTBrushAngle, &bNull), 45.0, 1e-5);
  EXPECT_EQ(OGR_ST_GetParamNum(hBrush, OGRSTBrushPriority, &bNull), 2);
  OGR_ST_Destroy(hBrush);

  // Symbol C API tool
  OGRStyleToolH hSymbol = OGR_ST_Create(OGRSTCSymbol);
  ASSERT_NE(hSymbol, nullptr);
  OGR_ST_SetParamStr(hSymbol, OGRSTSymbolId, "sym-cross");
  OGR_ST_SetParamDbl(hSymbol, OGRSTSymbolSize, 6.0);
  OGR_ST_SetParamNum(hSymbol, OGRSTSymbolPriority, 1);
  EXPECT_STREQ(OGR_ST_GetParamStr(hSymbol, OGRSTSymbolId, &bNull), "sym-cross");
  EXPECT_NEAR(OGR_ST_GetParamDbl(hSymbol, OGRSTSymbolSize, &bNull), 6.0, 1e-5);
  EXPECT_EQ(OGR_ST_GetParamNum(hSymbol, OGRSTSymbolPriority, &bNull), 1);
  OGR_ST_Destroy(hSymbol);

  // Label C API tool
  OGRStyleToolH hLabel = OGR_ST_Create(OGRSTCLabel);
  ASSERT_NE(hLabel, nullptr);
  OGR_ST_SetParamStr(hLabel, OGRSTLabelTextString, "Title");
  OGR_ST_SetParamDbl(hLabel, OGRSTLabelSize, 12.0);
  OGR_ST_SetParamNum(hLabel, OGRSTLabelBold, 1);
  EXPECT_STREQ(OGR_ST_GetParamStr(hLabel, OGRSTLabelTextString, &bNull),
               "Title");
  EXPECT_NEAR(OGR_ST_GetParamDbl(hLabel, OGRSTLabelSize, &bNull), 12.0, 1e-5);
  EXPECT_EQ(OGR_ST_GetParamNum(hLabel, OGRSTLabelBold, &bNull), 1);
  OGR_ST_Destroy(hLabel);
}

TEST(OGRFeatureStyleTest, NullPointerSafety) {
  WithQuietHandler holder;

  // C API null pointer handling
  OGRStyleMgrH hSMNullTable = OGR_SM_Create(nullptr);
  ASSERT_NE(hSMNullTable, nullptr);
  OGR_SM_Destroy(hSMNullTable);

  OGR_SM_Destroy(nullptr);
  EXPECT_EQ(OGR_SM_InitFromFeature(nullptr, nullptr), nullptr);
  EXPECT_FALSE(OGR_SM_InitStyleString(nullptr, nullptr));
  EXPECT_EQ(OGR_SM_GetPartCount(nullptr, nullptr), 0);
  EXPECT_EQ(OGR_SM_GetPart(nullptr, 0, nullptr), nullptr);
  EXPECT_FALSE(OGR_SM_AddPart(nullptr, nullptr));
  EXPECT_FALSE(OGR_SM_AddStyle(nullptr, "s", "p"));

  OGR_STBL_Destroy(nullptr);
  EXPECT_FALSE(OGR_STBL_AddStyle(nullptr, "s", "p"));
  EXPECT_FALSE(OGR_STBL_SaveStyleTable(nullptr, "/vsimem/dummy.txt"));
  EXPECT_FALSE(OGR_STBL_LoadStyleTable(nullptr, "/vsimem/dummy.txt"));
  EXPECT_EQ(OGR_STBL_Find(nullptr, "p"), nullptr);
  OGR_STBL_ResetStyleStringReading(nullptr);
  EXPECT_EQ(OGR_STBL_GetNextStyle(nullptr), nullptr);
  EXPECT_EQ(OGR_STBL_GetLastStyleName(nullptr), nullptr);

  OGR_ST_Destroy(nullptr);
  EXPECT_EQ(OGR_ST_GetType(nullptr), OGRSTCNone);
  EXPECT_EQ(OGR_ST_GetUnit(nullptr), OGRSTUGround);
  OGR_ST_SetUnit(nullptr, OGRSTUMM, 1.0);
  int bNull = FALSE;
  EXPECT_STREQ(OGR_ST_GetParamStr(nullptr, 0, &bNull), "");
  EXPECT_EQ(OGR_ST_GetParamNum(nullptr, 0, &bNull), 0);
  EXPECT_EQ(OGR_ST_GetParamDbl(nullptr, 0, &bNull), 0.0);
  OGR_ST_SetParamStr(nullptr, 0, "val");
  OGR_ST_SetParamNum(nullptr, 0, 1);
  OGR_ST_SetParamDbl(nullptr, 0, 1.0);
  EXPECT_STREQ(OGR_ST_GetStyleString(nullptr), "");
  int r = 0, g = 0, b = 0, a = 0;
  EXPECT_FALSE(OGR_ST_GetRGBFromString(nullptr, "#FFF", &r, &g, &b, &a));
}

class DummyStyleTool : public OGRStyleTool {
 public:
  DummyStyleTool() : OGRStyleTool(OGRSTCNone) {}
  GBool Parse() override {
    static const OGRStyleParamId params[] = {{0, "x", FALSE, OGRSTypeString}};
    static OGRStyleValue values[1];
    return OGRStyleTool::Parse(params, values, 1);
  }
  const char* GetStyleString() override { return ""; }

  using OGRStyleTool::GetParamDbl;
  using OGRStyleTool::GetParamNum;
  using OGRStyleTool::GetParamStr;
  using OGRStyleTool::GetRawParamDbl;
  using OGRStyleTool::SetParamDbl;
  using OGRStyleTool::SetParamNum;
  using OGRStyleTool::SetParamStr;
};

TEST(OGRFeatureStyleTest, AdditionalCoverage) {
  WithQuietHandler holder;

  // 1. OGR_STBL_Create
  OGRStyleTableH hSTBL = OGR_STBL_Create();
  ASSERT_NE(hSTBL, nullptr);
  OGR_STBL_Destroy(hSTBL);

  // 2. OGR_ST_Create(OGRSTCNone) returns nullptr
  EXPECT_EQ(OGR_ST_Create(OGRSTCNone), nullptr);

  // 3. DummyStyleTool with C API
  DummyStyleTool dummy;
  OGRStyleToolH hDummy = reinterpret_cast<OGRStyleToolH>(&dummy);
  int bNull = FALSE;
  EXPECT_STREQ(OGR_ST_GetParamStr(hDummy, 0, &bNull), "");
  EXPECT_EQ(OGR_ST_GetParamNum(hDummy, 0, &bNull), 0);
  EXPECT_NEAR(OGR_ST_GetParamDbl(hDummy, 0, &bNull), 0.0, 1e-5);
  OGR_ST_SetParamStr(hDummy, 0, "val");
  OGR_ST_SetParamNum(hDummy, 0, 1);
  OGR_ST_SetParamDbl(hDummy, 0, 1.0);

  // 4. Undetermined type in Parse
  dummy.SetStyleString("DUMMY(x:1)");
  EXPECT_FALSE(dummy.Parse());

  // 5. OGRStyleTool::ComputeWithUnit all unit combinations
  OGRStylePen pen;
  const OGRSTUnitId units[] = {OGRSTUGround, OGRSTUPixel, OGRSTUPoints,
                               OGRSTUMM,     OGRSTUCM,    OGRSTUInches};
  for (auto outUnit : units) {
    pen.SetUnit(outUnit, 2.0);
    for (auto inUnit : units) {
      EXPECT_GE(pen.ComputeWithUnit(10.0, inUnit), 0.0);
      EXPECT_GE(pen.ComputeWithUnit(10, inUnit), 0);
    }
  }

  // 6. Parsing error branches
  GBool bDefault = FALSE;
  {
    OGRStylePen p;
    p.SetStyleString("invalid_no_parens");
    EXPECT_EQ(p.Color(bDefault), nullptr);
  }
  {
    OGRStylePen p;
    p.SetStyleString("PEN(c:#FF0000:bad:extra)");
    EXPECT_EQ(p.Color(bDefault), nullptr);
  }
  {
    OGRStyleBrush brush;
    brush.SetStyleString("PEN(c:#000)");
    EXPECT_EQ(brush.ForeColor(bDefault), nullptr);
  }
  {
    OGRStyleSymbol sym;
    sym.SetStyleString("PEN(c:#000)");
    EXPECT_EQ(sym.Id(bDefault), nullptr);
  }
  {
    OGRStyleLabel label;
    label.SetStyleString("PEN(c:#000)");
    EXPECT_EQ(label.FontName(bDefault), nullptr);
  }
  {
    OGRStylePen p;
    p.SetStyleString("BRUSH(fc:#000)");
    EXPECT_EQ(p.Color(bDefault), nullptr);
  }

  // 7. Georeferenced units serialization in GetStyleString
  {
    OGRStylePen p;
    p.SetUnit(OGRSTUMM, 1.0);
    p.SetStyleString("PEN(w:10g,dp:5px)");
    p.SetWidth(15.0);
    EXPECT_NE(p.GetStyleString(), nullptr);
  }
  {
    OGRStylePen p;
    p.SetStyleString("PEN(w:10pt,dp:5cm)");
    p.SetWidth(15.0);
    EXPECT_NE(p.GetStyleString(), nullptr);
  }
  {
    OGRStylePen p;
    p.SetStyleString("PEN(w:10in)");
    p.SetWidth(15.0);
    EXPECT_NE(p.GetStyleString(), nullptr);
  }

  // 8. OGRStyleMgr edge cases
  OGRStyleMgr mgr;
  EXPECT_EQ(mgr.InitFromFeature(nullptr), nullptr);
  EXPECT_FALSE(mgr.InitStyleString("@nonexistent_style"));
  EXPECT_TRUE(mgr.InitStyleString(nullptr));
  EXPECT_EQ(mgr.GetStyleName(nullptr), nullptr);
  EXPECT_EQ(mgr.GetStyleByName(nullptr), nullptr);
  EXPECT_FALSE(mgr.AddStyle(nullptr, nullptr));
  EXPECT_EQ(mgr.GetPartCount(nullptr), 0);
  EXPECT_EQ(mgr.GetPart(-1, nullptr), nullptr);
  EXPECT_FALSE(mgr.AddPart(static_cast<const char*>(nullptr)));
  EXPECT_FALSE(mgr.AddPart(static_cast<OGRStyleTool*>(nullptr)));

  OGRStylePen uninitPen;
  EXPECT_FALSE(mgr.AddPart(&uninitPen));

  // AddPart when m_pszStyleString is null
  OGRStyleMgr mgr2;
  EXPECT_TRUE(mgr2.AddPart("PEN(c:#FF0000)"));
  EXPECT_STREQ(mgr2.GetStyleString(), "PEN(c:#FF0000)");

  OGRStyleMgr mgr3;
  pen.SetColor("#00FF00");
  ASSERT_NE(pen.GetStyleString(), nullptr);
  EXPECT_TRUE(mgr3.AddPart(&pen));
  EXPECT_NE(mgr3.GetStyleString(), nullptr);

  // SetFeatureStyleString branches
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("Test");
  poDefn->Reference();
  OGRFeature feat(poDefn);
  EXPECT_FALSE(mgr.SetFeatureStyleString(nullptr, nullptr, FALSE));
  EXPECT_TRUE(
      mgr.SetFeatureStyleString(&feat, nullptr, FALSE));  // empty mgr -> ""
  EXPECT_STREQ(feat.GetStyleString(), "");

  OGRStyleMgr mgrWithStyle;
  mgrWithStyle.InitStyleString("PEN(c:#123456)");
  EXPECT_TRUE(
      mgrWithStyle.SetFeatureStyleString(&feat, "PEN(c:#123456)", FALSE));
  EXPECT_STREQ(feat.GetStyleString(), "PEN(c:#123456)");
  poDefn->Release();

  // 9. OGRStyleTable edge cases
  OGRStyleTable table;
  EXPECT_FALSE(table.SaveStyleTable(nullptr));
  EXPECT_FALSE(table.SaveStyleTable("/nonexistent_dir_12345/dummy.txt"));
  EXPECT_FALSE(table.LoadStyleTable(nullptr));
  EXPECT_FALSE(table.AddStyle(nullptr, "PEN()"));
  EXPECT_FALSE(table.ModifyStyle(nullptr, "PEN()"));
  EXPECT_EQ(table.Find(nullptr), nullptr);
  EXPECT_EQ(table.IsExist(nullptr), -1);

  // 10. OGR_ST_GetStyleString with different tool types
  {
    OGRStyleBrush brush;
    brush.SetForeColor("#112233");
    brush.GetStyleString();
    EXPECT_NE(OGR_ST_GetStyleString(reinterpret_cast<OGRStyleToolH>(&brush)),
              nullptr);
  }
  {
    OGRStyleSymbol sym;
    sym.SetId("x");
    sym.GetStyleString();
    EXPECT_NE(OGR_ST_GetStyleString(reinterpret_cast<OGRStyleToolH>(&sym)),
              nullptr);
  }
  {
    OGRStyleLabel label;
    label.SetTextString("y");
    label.GetStyleString();
    EXPECT_NE(OGR_ST_GetStyleString(reinterpret_cast<OGRStyleToolH>(&label)),
              nullptr);
  }

  // 11. Direct SetParam/GetParam tests with different parameter types
  OGRStyleParamId sParamStr = {0, "s", FALSE, OGRSTypeString};
  OGRStyleParamId sParamDbl = {1, "d", FALSE, OGRSTypeDouble};
  OGRStyleParamId sParamInt = {2, "i", FALSE, OGRSTypeInteger};
  OGRStyleParamId sParamBool = {3, "b", FALSE, OGRSTypeBoolean};
  OGRStyleParamId sParamUnused = {-1, "u", FALSE, OGRSTypeUnused};
  OGRStyleParamId sParamGeorefDbl = {4, "gd", TRUE, OGRSTypeDouble};
  OGRStyleParamId sParamGeorefInt = {5, "gi", TRUE, OGRSTypeInteger};
  OGRStyleParamId sParamGeorefStr = {6, "gs", TRUE, OGRSTypeString};

  OGRStyleValue sVal{};
  dummy.SetParamStr(sParamStr, sVal, "test");
  CPLFree(sVal.pszValue);
  sVal.pszValue = nullptr;
  dummy.SetParamStr(sParamDbl, sVal, "1.5");
  dummy.SetParamStr(sParamInt, sVal, "42");
  dummy.SetParamStr(sParamBool, sVal, "1");
  dummy.SetParamStr(sParamUnused, sVal, "none");
  EXPECT_FALSE(sVal.bValid);

  dummy.SetParamNum(sParamStr, sVal, 10);
  CPLFree(sVal.pszValue);
  sVal.pszValue = nullptr;
  dummy.SetParamNum(sParamDbl, sVal, 20);
  dummy.SetParamNum(sParamInt, sVal, 30);
  dummy.SetParamNum(sParamBool, sVal, 1);
  dummy.SetParamNum(sParamUnused, sVal, 0);
  EXPECT_FALSE(sVal.bValid);

  dummy.SetParamDbl(sParamStr, sVal, 10.5);
  CPLFree(sVal.pszValue);
  sVal.pszValue = nullptr;
  dummy.SetParamDbl(sParamDbl, sVal, 20.5);
  dummy.SetParamDbl(sParamInt, sVal, 30.5);
  dummy.SetParamDbl(sParamBool, sVal, 1.0);
  dummy.SetParamDbl(sParamUnused, sVal, 0.0);
  EXPECT_FALSE(sVal.bValid);

  GBool bValNull = FALSE;
  OGRSTUnitId eUnit = OGRSTUGround;
  EXPECT_EQ(dummy.GetRawParamDbl(sParamDbl, sVal, eUnit, bValNull), 0.0);
  EXPECT_EQ(dummy.GetRawParamDbl(sParamStr, sVal, eUnit, bValNull), 0.0);

  sVal.bValid = TRUE;
  sVal.dfValue = 25.0;
  sVal.nValue = 42;
  sVal.pszValue = CPLStrdup("test");
  EXPECT_NEAR(dummy.GetRawParamDbl(sParamDbl, sVal, eUnit, bValNull), 25.0,
              1e-5);

  EXPECT_NE(dummy.GetParamStr(sParamStr, sVal, bValNull), nullptr);
  EXPECT_NE(dummy.GetParamStr(sParamDbl, sVal, bValNull), nullptr);
  EXPECT_NE(dummy.GetParamStr(sParamInt, sVal, bValNull), nullptr);
  EXPECT_NE(dummy.GetParamStr(sParamBool, sVal, bValNull), nullptr);
  EXPECT_EQ(dummy.GetParamStr(sParamUnused, sVal, bValNull), nullptr);
  EXPECT_NE(dummy.GetParamStr(sParamGeorefDbl, sVal, bValNull), nullptr);
  EXPECT_NE(dummy.GetParamStr(sParamGeorefInt, sVal, bValNull), nullptr);

  EXPECT_NEAR(dummy.GetParamDbl(sParamDbl, sVal, bValNull), 25.0, 1e-5);
  EXPECT_NEAR(dummy.GetParamDbl(sParamInt, sVal, bValNull), 42.0, 1e-5);
  EXPECT_NEAR(dummy.GetParamDbl(sParamBool, sVal, bValNull), 1.0, 1e-5);
  EXPECT_NEAR(dummy.GetParamDbl(sParamUnused, sVal, bValNull), 0.0, 1e-5);
  EXPECT_NEAR(dummy.GetParamDbl(sParamGeorefDbl, sVal, bValNull), 25.0, 1e-5);
  EXPECT_NEAR(dummy.GetParamDbl(sParamGeorefInt, sVal, bValNull), 42.0, 1e-5);
  CPLFree(sVal.pszValue);
  sVal.pszValue = CPLStrdup("12.34");
  EXPECT_NEAR(dummy.GetParamDbl(sParamGeorefStr, sVal, bValNull), 12.34, 1e-5);
  EXPECT_NEAR(dummy.GetParamDbl(sParamStr, sVal, bValNull), 12.34, 1e-5);
  CPLFree(sVal.pszValue);
  sVal.pszValue = nullptr;
}

}  // namespace
}  // namespace autotest2
