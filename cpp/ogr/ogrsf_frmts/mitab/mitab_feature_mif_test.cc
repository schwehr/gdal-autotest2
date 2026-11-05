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
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_utils.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(MitabFeatureMifTest, MidRecordReadWriteAllFieldTypesAndEdgeCases) {
  auto* defn = new OGRFeatureDefn("test_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  OGRFieldDefn fld_str("str_fld", OFTString);
  defn->AddFieldDefn(&fld_str);

  OGRFieldDefn fld_bool("bool_fld", OFTInteger);
  fld_bool.SetSubType(OFSTBoolean);
  defn->AddFieldDefn(&fld_bool);

  OGRFieldDefn fld_int("int_fld", OFTInteger);
  defn->AddFieldDefn(&fld_int);

  OGRFieldDefn fld_real("real_fld", OFTReal);
  defn->AddFieldDefn(&fld_real);

  OGRFieldDefn fld_date("date_fld", OFTDate);
  defn->AddFieldDefn(&fld_date);

  OGRFieldDefn fld_time("time_fld", OFTTime);
  defn->AddFieldDefn(&fld_time);

  OGRFieldDefn fld_datetime("datetime_fld", OFTDateTime);
  defn->AddFieldDefn(&fld_datetime);

  constexpr char kMidPath[] = "/vsimem/mitab_feature_mif_test/record.mid";
  absl::Cleanup vsi_cleanup = [kMidPath] { VSIUnlink(kMidPath); };

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMidPath, "wt"));
    fp.SetEncoding("ISO-8859-1");

    TABFeature feat(defn);
    feat.SetField(0, "Line with \"quotes\" and \nnewline and \xc3\xa9 (é)");
    feat.SetField(1, 1);
    feat.SetField(2, 42);
    feat.SetField(3, 3.1415);
    feat.SetField(4, 2023, 11, 23, 0, 0, 0.0f, 0);
    feat.SetField(5, 0, 0, 0, 14, 30, 45.123f, 0);
    feat.SetField(6, 2023, 11, 23, 14, 30, 45.123f, 0);

    EXPECT_EQ(0, feat.WriteRecordToMIDFile(&fp));

    // Second feature with null/unset date, time, datetime and false bool.
    TABFeature feat2(defn);
    feat2.SetField(0, "Simple");
    feat2.SetField(1, 0);
    feat2.SetField(2, -100);
    feat2.SetField(3, 0.0);
    EXPECT_EQ(0, feat2.WriteRecordToMIDFile(&fp));

    fp.Close();
  }

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMidPath, "rt"));
    fp.SetEncoding("ISO-8859-1");

    TABFeature feat(defn);
    EXPECT_EQ(0, feat.ReadRecordFromMIDFile(&fp));
    EXPECT_THAT(feat.GetFieldAsString(0), HasSubstr("quotes"));
    EXPECT_THAT(feat.GetFieldAsString(0), HasSubstr("newline"));
    EXPECT_EQ(1, feat.GetFieldAsInteger(1));
    EXPECT_EQ(42, feat.GetFieldAsInteger(2));
    EXPECT_NEAR(3.1415, feat.GetFieldAsDouble(3), 1e-4);

    int year = 0, month = 0, day = 0, hour = 0, min = 0, tz = 0;
    float sec = 0.0f;
    feat.GetFieldAsDateTime(4, &year, &month, &day, &hour, &min, &sec, &tz);
    EXPECT_EQ(2023, year);
    EXPECT_EQ(11, month);
    EXPECT_EQ(23, day);

    feat.GetFieldAsDateTime(5, &year, &month, &day, &hour, &min, &sec, &tz);
    EXPECT_EQ(14, hour);
    EXPECT_EQ(30, min);
    EXPECT_NEAR(45.123f, sec, 1e-2f);

    feat.GetFieldAsDateTime(6, &year, &month, &day, &hour, &min, &sec, &tz);
    EXPECT_EQ(2023, year);
    EXPECT_EQ(11, month);
    EXPECT_EQ(23, day);
    EXPECT_EQ(14, hour);
    EXPECT_EQ(30, min);
    EXPECT_NEAR(45.123f, sec, 1e-2f);

    TABFeature feat2(defn);
    EXPECT_EQ(0, feat2.ReadRecordFromMIDFile(&fp));
    EXPECT_STREQ("Simple", feat2.GetFieldAsString(0));
    EXPECT_EQ(0, feat2.GetFieldAsInteger(1));

    fp.Close();
  }

  // Test boolean sub-type variations ('T', 't', 'Y', 'y', '1', 'F', '0') using
  // TAB delimiter.
  constexpr char kBoolMidPath[] = "/vsimem/mitab_feature_mif_test/bool.mid";
  absl::Cleanup bool_cleanup = [kBoolMidPath] { VSIUnlink(kBoolMidPath); };
  {
    VsiMemTempWrapper wrapper(
        kBoolMidPath,
        "\"s\"\tT\t1\t1.0\t20200101\t120000000\t20200101120000000\n"
        "\"s\"\tt\t1\t1.0\t20200101\t120000000\t20200101120000000\n"
        "\"s\"\tY\t1\t1.0\t20200101\t120000000\t20200101120000000\n"
        "\"s\"\ty\t1\t1.0\t20200101\t120000000\t20200101120000000\n"
        "\"s\"\t1\t1\t1.0\t20200101\t120000000\t20200101120000000\n"
        "\"s\"\tF\t1\t1.0\t20200101\t120000000\t20200101120000000\n"
        "\"s\"\t0\t1\t1.0\t20200101\t120000000\t20200101120000000\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kBoolMidPath, "rt"));
    TABFeature feat(defn);
    for (int expected : {1, 1, 1, 1, 1, 0, 0}) {
      EXPECT_EQ(0, feat.ReadRecordFromMIDFile(&fp));
      EXPECT_EQ(expected, feat.GetFieldAsInteger(1));
    }

    // EOF error
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, feat.ReadRecordFromMIDFile(&fp));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    fp.Close();
  }

  // Single field defn reading a blank line treats it as empty string
  {
    auto* single_defn = new OGRFeatureDefn("single");
    single_defn->Reference();
    absl::Cleanup single_cleanup = [single_defn] { single_defn->Release(); };
    OGRFieldDefn f1("f1", OFTString);
    single_defn->AddFieldDefn(&f1);

    constexpr char kBlankPath[] = "/vsimem/mitab_feature_mif_test/blank.mid";
    absl::Cleanup blank_cleanup = [kBlankPath] { VSIUnlink(kBlankPath); };
    VsiMemTempWrapper wrapper(kBlankPath, "\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kBlankPath, "rt"));
    TABFeature feat(single_defn);
    EXPECT_EQ(0, feat.ReadRecordFromMIDFile(&fp));
    EXPECT_STREQ("", feat.GetFieldAsString(0));
    fp.Close();
  }

  // Fewer tokens than fields returns -1
  {
    constexpr char kShortPath[] = "/vsimem/mitab_feature_mif_test/short.mid";
    absl::Cleanup short_cleanup = [kShortPath] { VSIUnlink(kShortPath); };
    VsiMemTempWrapper wrapper(kShortPath, "\"only_one_field\"\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kShortPath, "rt"));
    TABFeature feat(defn);
    EXPECT_EQ(-1, feat.ReadRecordFromMIDFile(&fp));
    fp.Close();
  }
}

TEST(MitabFeatureMifTest, TABFeatureAndDebugFeatureMIF) {
  auto* defn = new OGRFeatureDefn("test_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  constexpr char kMifPath[] = "/vsimem/mitab_feature_mif_test/base_feat.mif";
  absl::Cleanup mif_cleanup = [kMifPath] { VSIUnlink(kMifPath); };

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));
    TABFeature feat(defn);
    EXPECT_EQ(0, feat.WriteGeometryToMIFFile(&fp));

    TABDebugFeature debug_feat(defn);
    EXPECT_EQ(-1, debug_feat.WriteGeometryToMIFFile(&fp));
    fp.Close();
  }

  {
    VsiMemTempWrapper wrapper(kMifPath,
                              "NONE\n"
                              "# comment\n"
                              "Point 10.0 20.0\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "rt"));
    TABFeature feat(defn);
    EXPECT_EQ(0, feat.ReadGeometryFromMIFFile(&fp));

    TABDebugFeature debug_feat(defn);
    EXPECT_EQ(0, debug_feat.ReadGeometryFromMIFFile(&fp));
    fp.Close();
  }
}

TEST(MitabFeatureMifTest, TABPointFontPointCustomPointMIF) {
  auto* defn = new OGRFeatureDefn("test_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  constexpr char kMifPath[] = "/vsimem/mitab_feature_mif_test/points.mif";
  absl::Cleanup mif_cleanup = [kMifPath] { VSIUnlink(kMifPath); };

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));

    // 1. TABPoint
    TABPoint pt(defn);
    auto geom_pt = std::make_unique<OGRPoint>(10.5, 20.5);
    ASSERT_EQ(OGRERR_NONE, pt.SetGeometryDirectly(geom_pt.release()));
    pt.SetSymbolNo(35);
    pt.SetSymbolColor(255);
    pt.SetSymbolSize(12);
    EXPECT_EQ(0, pt.WriteGeometryToMIFFile(&fp));

    // 2. TABFontPoint
    TABFontPoint fpt(defn);
    auto geom_fpt = std::make_unique<OGRPoint>(30.5, 40.5);
    ASSERT_EQ(OGRERR_NONE, fpt.SetGeometryDirectly(geom_fpt.release()));
    fpt.SetSymbolNo(45);
    fpt.SetSymbolColor(128);
    fpt.SetSymbolSize(16);
    fpt.SetFontName("Wingdings");
    fpt.SetFontStyleMIFValue(1);
    fpt.SetSymbolAngle(45.0);
    EXPECT_EQ(0, fpt.WriteGeometryToMIFFile(&fp));

    // 3. TABCustomPoint
    TABCustomPoint cpt(defn);
    auto geom_cpt = std::make_unique<OGRPoint>(50.5, 60.5);
    ASSERT_EQ(OGRERR_NONE, cpt.SetGeometryDirectly(geom_cpt.release()));
    cpt.SetFontName("custom_icon.bmp");
    cpt.SetSymbolColor(64);
    cpt.SetSymbolSize(24);
    cpt.SetCustomSymbolStyle(2);
    EXPECT_EQ(0, cpt.WriteGeometryToMIFFile(&fp));

    fp.Close();
  }

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "rt"));

    // Read TABPoint
    const char* line = fp.GetLine();
    fp.SaveLine(line);
    fp.GetLine();
    TABPoint pt(defn);
    EXPECT_EQ(0, pt.ReadGeometryFromMIFFile(&fp));
    ASSERT_NE(nullptr, pt.GetGeometryRef());
    OGRPoint* p = pt.GetGeometryRef()->toPoint();
    ASSERT_NE(nullptr, p);
    EXPECT_DOUBLE_EQ(10.5, p->getX());
    EXPECT_DOUBLE_EQ(20.5, p->getY());
    EXPECT_EQ(35, pt.GetSymbolNo());
    EXPECT_EQ(255, pt.GetSymbolColor());
    EXPECT_EQ(12, pt.GetSymbolSize());

    // Read TABFontPoint
    line = fp.GetLastLine();
    fp.SaveLine(line);
    fp.GetLine();
    TABFontPoint fpt(defn);
    EXPECT_EQ(0, fpt.ReadGeometryFromMIFFile(&fp));
    ASSERT_NE(nullptr, fpt.GetGeometryRef());
    p = fpt.GetGeometryRef()->toPoint();
    ASSERT_NE(nullptr, p);
    EXPECT_DOUBLE_EQ(30.5, p->getX());
    EXPECT_DOUBLE_EQ(40.5, p->getY());
    EXPECT_EQ(45, fpt.GetSymbolNo());
    EXPECT_EQ(128, fpt.GetSymbolColor());
    EXPECT_EQ(16, fpt.GetSymbolSize());
    EXPECT_STREQ("Wingdings", fpt.GetFontNameRef());
    EXPECT_DOUBLE_EQ(45.0, fpt.GetSymbolAngle());

    // Read TABCustomPoint
    line = fp.GetLastLine();
    fp.SaveLine(line);
    fp.GetLine();
    TABCustomPoint cpt(defn);
    EXPECT_EQ(0, cpt.ReadGeometryFromMIFFile(&fp));
    ASSERT_NE(nullptr, cpt.GetGeometryRef());
    p = cpt.GetGeometryRef()->toPoint();
    ASSERT_NE(nullptr, p);
    EXPECT_DOUBLE_EQ(50.5, p->getX());
    EXPECT_DOUBLE_EQ(60.5, p->getY());
    EXPECT_STREQ("custom_icon.bmp", cpt.GetFontNameRef());
    EXPECT_EQ(64, cpt.GetSymbolColor());
    EXPECT_EQ(24, cpt.GetSymbolSize());
    EXPECT_EQ(2, cpt.GetCustomSymbolStyle());

    fp.Close();
  }

  // Error cases: missing geometry on write
  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));
    WithQuietHandler quiet;
    CPLErrorReset();

    TABPoint pt(defn);
    EXPECT_EQ(-1, pt.WriteGeometryToMIFFile(&fp));

    TABFontPoint fpt(defn);
    EXPECT_EQ(-1, fpt.WriteGeometryToMIFFile(&fp));

    TABCustomPoint cpt(defn);
    EXPECT_EQ(-1, cpt.WriteGeometryToMIFFile(&fp));

    CPLErrorReset();
    fp.Close();
  }

  // Error cases: bad tokens on read
  {
    constexpr char kBadPath[] = "/vsimem/mitab_feature_mif_test/bad_pt.mif";
    absl::Cleanup bad_cleanup = [kBadPath] { VSIUnlink(kBadPath); };
    VsiMemTempWrapper wrapper(kBadPath, "Point 10.0\nSymbol (1)\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kBadPath, "rt"));
    const char* line = fp.GetLine();
    fp.SaveLine(line);
    fp.GetLine();

    TABPoint pt(defn);
    EXPECT_EQ(-1, pt.ReadGeometryFromMIFFile(&fp));

    TABFontPoint fpt(defn);
    EXPECT_EQ(-1, fpt.ReadGeometryFromMIFFile(&fp));

    TABCustomPoint cpt(defn);
    EXPECT_EQ(-1, cpt.ReadGeometryFromMIFFile(&fp));
    fp.Close();
  }

  // Error cases: bad symbol line on read for FontPoint and CustomPoint
  {
    constexpr char kBadSymPath[] = "/vsimem/mitab_feature_mif_test/badsym.mif";
    absl::Cleanup bad_cleanup = [kBadSymPath] { VSIUnlink(kBadSymPath); };
    VsiMemTempWrapper wrapper(kBadSymPath,
                              "Point 10.0 20.0\n"
                              "Symbol (1, 2)\n"
                              "Point 10.0 20.0\n"
                              "Symbol (1, 2)\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kBadSymPath, "rt"));
    const char* line = fp.GetLine();
    fp.SaveLine(line);
    fp.GetLine();

    TABFontPoint fpt(defn);
    EXPECT_EQ(-1, fpt.ReadGeometryFromMIFFile(&fp));

    line = fp.GetLine();
    fp.SaveLine(line);
    fp.GetLine();

    TABCustomPoint cpt(defn);
    EXPECT_EQ(-1, cpt.ReadGeometryFromMIFFile(&fp));
    fp.Close();
  }
}

TEST(MitabFeatureMifTest, TABPolylineMIF) {
  auto* defn = new OGRFeatureDefn("test_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  constexpr char kMifPath[] = "/vsimem/mitab_feature_mif_test/pline.mif";
  absl::Cleanup mif_cleanup = [kMifPath] { VSIUnlink(kMifPath); };

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));

    // 1. Two-point Line
    TABPolyline line(defn);
    auto geom_line = std::make_unique<OGRLineString>();
    geom_line->addPoint(1.0, 2.0);
    geom_line->addPoint(3.0, 4.0);
    ASSERT_EQ(OGRERR_NONE, line.SetGeometryDirectly(geom_line.release()));
    line.SetPenWidthMIF(2);
    line.SetPenPattern(3);
    line.SetPenColor(255);
    EXPECT_EQ(0, line.WriteGeometryToMIFFile(&fp));

    // 2. Polyline with 3 points and Smooth
    TABPolyline pline(defn);
    auto geom_pline = std::make_unique<OGRLineString>();
    geom_pline->addPoint(10.0, 10.0);
    geom_pline->addPoint(20.0, 30.0);
    geom_pline->addPoint(30.0, 10.0);
    ASSERT_EQ(OGRERR_NONE, pline.SetGeometryDirectly(geom_pline.release()));
    pline.SetPenWidthMIF(1);
    pline.SetPenPattern(2);
    pline.SetPenColor(128);
    pline.m_bSmooth = TRUE;
    EXPECT_EQ(0, pline.WriteGeometryToMIFFile(&fp));

    // 3. MultiPolyline with 2 lines
    TABPolyline mline(defn);
    auto geom_mline = std::make_unique<OGRMultiLineString>();
    auto part1 = std::make_unique<OGRLineString>();
    part1->addPoint(100.0, 100.0);
    part1->addPoint(110.0, 110.0);
    auto part2 = std::make_unique<OGRLineString>();
    part2->addPoint(200.0, 200.0);
    part2->addPoint(210.0, 210.0);
    geom_mline->addGeometryDirectly(part1.release());
    geom_mline->addGeometryDirectly(part2.release());
    ASSERT_EQ(OGRERR_NONE, mline.SetGeometryDirectly(geom_mline.release()));
    mline.SetPenWidthMIF(4);
    mline.SetPenPattern(1);
    mline.SetPenColor(0);
    EXPECT_EQ(0, mline.WriteGeometryToMIFFile(&fp));

    fp.Close();
  }

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "rt"));

    // Read Line
    ASSERT_NE(nullptr, fp.GetLine());
    TABPolyline line(defn);
    EXPECT_EQ(0, line.ReadGeometryFromMIFFile(&fp));
    OGRGeometry* geom = line.GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbLineString, wkbFlatten(geom->getGeometryType()));
    EXPECT_EQ(2, geom->toLineString()->getNumPoints());
    EXPECT_EQ(2, line.GetPenWidthMIF());
    EXPECT_EQ(3, line.GetPenPattern());
    EXPECT_EQ(255, line.GetPenColor());

    // Read Pline
    TABPolyline pline(defn);
    EXPECT_EQ(0, pline.ReadGeometryFromMIFFile(&fp));
    geom = pline.GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbLineString, wkbFlatten(geom->getGeometryType()));
    EXPECT_EQ(3, geom->toLineString()->getNumPoints());
    EXPECT_EQ(TRUE, pline.m_bSmooth);

    // Read MultiPline
    TABPolyline mline(defn);
    EXPECT_EQ(0, mline.ReadGeometryFromMIFFile(&fp));
    geom = mline.GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbMultiLineString, wkbFlatten(geom->getGeometryType()));
    EXPECT_EQ(2, geom->toMultiLineString()->getNumGeometries());

    fp.Close();
  }

  // Read variations for PLINE: 1 token, 3 tokens, 4 tokens
  {
    constexpr char kVarPath[] = "/vsimem/mitab_feature_mif_test/pline_var.mif";
    absl::Cleanup var_cleanup = [kVarPath] { VSIUnlink(kVarPath); };
    VsiMemTempWrapper wrapper(kVarPath,
                              "Pline\n"
                              "2\n"
                              "1.0 1.0\n"
                              "2.0 2.0\n"
                              "PLINE MULTIPLE 2\n"
                              "2\n"
                              "10.0 10.0\n"
                              "11.0 11.0\n"
                              "2\n"
                              "20.0 20.0\n"
                              "21.0 21.0\n"
                              "PLINE MULTIPLE 1 2\n"
                              "30.0 30.0\n"
                              "31.0 31.0\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kVarPath, "rt"));

    ASSERT_NE(nullptr, fp.GetLine());
    TABPolyline p1(defn);
    EXPECT_EQ(0, p1.ReadGeometryFromMIFFile(&fp));
    EXPECT_EQ(2, p1.GetGeometryRef()->toLineString()->getNumPoints());

    TABPolyline p2(defn);
    EXPECT_EQ(0, p2.ReadGeometryFromMIFFile(&fp));
    EXPECT_EQ(2, p2.GetGeometryRef()->toMultiLineString()->getNumGeometries());

    TABPolyline p3(defn);
    EXPECT_EQ(0, p3.ReadGeometryFromMIFFile(&fp));
    EXPECT_EQ(1, p3.GetGeometryRef()->toMultiLineString()->getNumGeometries());

    fp.Close();
  }

  // Error branches on write: invalid geometry
  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));
    WithQuietHandler quiet;
    CPLErrorReset();

    TABPolyline bad_geom(defn);
    EXPECT_EQ(0, bad_geom.WriteGeometryToMIFFile(&fp));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    bad_geom.SetGeometryDirectly(new OGRPoint(1, 2));
    EXPECT_EQ(0, bad_geom.WriteGeometryToMIFFile(&fp));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    fp.Close();
  }

  // Error branches on read: bad token counts, < 2 vertices, unexpected EOF
  {
    WithQuietHandler quiet;

    auto check_read_err = [defn](const char* content) {
      constexpr char kPath[] =
          "/vsimem/mitab_feature_mif_test/pline_single_err.mif";
      VsiMemTempWrapper wrapper(kPath, content);
      MIDDATAFile fp("");
      if (fp.Open(kPath, "rt") == 0 && fp.GetLine() != nullptr) {
        CPLErrorReset();
        TABPolyline p(defn);
        EXPECT_EQ(-1, p.ReadGeometryFromMIFFile(&fp));
        CPLErrorReset();
        fp.Close();
      }
    };

    check_read_err("Line 1 2 3\n");
    check_read_err("PLINE NOT_MULTIPLE 2\n");
    check_read_err("PLINE NOT_MULTIPLE 2 3\n");
    check_read_err("Pline 1\n1.0 1.0\n");
    check_read_err("PLINE MULTIPLE 1\n1\n1.0 1.0\n");
    check_read_err("Pline 2\n1.0\n");
  }
}

TEST(MitabFeatureMifTest, TABRegionMIF) {
  auto* defn = new OGRFeatureDefn("test_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  constexpr char kMifPath[] = "/vsimem/mitab_feature_mif_test/region.mif";
  absl::Cleanup mif_cleanup = [kMifPath] { VSIUnlink(kMifPath); };

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));

    // 1. Single-ring Region with Pen, transparent Brush, and Center
    TABRegion reg1(defn);
    auto poly1 = std::make_unique<OGRPolygon>();
    auto ring1 = std::make_unique<OGRLinearRing>();
    ring1->addPoint(0.0, 0.0);
    ring1->addPoint(10.0, 0.0);
    ring1->addPoint(10.0, 10.0);
    ring1->addPoint(0.0, 10.0);
    ring1->addPoint(0.0, 0.0);
    poly1->addRingDirectly(ring1.release());
    ASSERT_EQ(OGRERR_NONE, reg1.SetGeometryDirectly(poly1.release()));
    reg1.SetPenWidthMIF(1);
    reg1.SetPenPattern(2);
    reg1.SetPenColor(100);
    reg1.SetBrushPattern(1);
    reg1.SetBrushFGColor(200);
    reg1.SetBrushTransparent(TRUE);
    reg1.SetCenter(5.0, 5.0);
    EXPECT_EQ(0, reg1.WriteGeometryToMIFFile(&fp));

    // 2. Region with outer + inner ring and non-transparent Brush
    TABRegion reg2(defn);
    auto poly2 = std::make_unique<OGRPolygon>();
    auto outer = std::make_unique<OGRLinearRing>();
    outer->addPoint(0.0, 0.0);
    outer->addPoint(100.0, 0.0);
    outer->addPoint(100.0, 100.0);
    outer->addPoint(0.0, 100.0);
    outer->addPoint(0.0, 0.0);
    auto inner = std::make_unique<OGRLinearRing>();
    inner->addPoint(20.0, 20.0);
    inner->addPoint(80.0, 20.0);
    inner->addPoint(80.0, 80.0);
    inner->addPoint(20.0, 80.0);
    inner->addPoint(20.0, 20.0);
    poly2->addRingDirectly(outer.release());
    poly2->addRingDirectly(inner.release());
    ASSERT_EQ(OGRERR_NONE, reg2.SetGeometryDirectly(poly2.release()));
    reg2.SetBrushPattern(2);
    reg2.SetBrushFGColor(10);
    reg2.SetBrushBGColor(20);
    reg2.SetBrushTransparent(FALSE);
    EXPECT_EQ(0, reg2.WriteGeometryToMIFFile(&fp));

    fp.Close();
  }

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "rt"));

    // Read reg1
    ASSERT_NE(nullptr, fp.GetLine());
    TABRegion reg1(defn);
    EXPECT_EQ(0, reg1.ReadGeometryFromMIFFile(&fp));
    OGRGeometry* geom = reg1.GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbPolygon, wkbFlatten(geom->getGeometryType()));
    EXPECT_EQ(1, reg1.GetPenWidthMIF());
    EXPECT_EQ(1, reg1.GetBrushPattern());
    EXPECT_EQ(TRUE, reg1.GetBrushTransparent());
    double cx = 0.0, cy = 0.0;
    EXPECT_EQ(0, reg1.GetCenter(cx, cy));
    EXPECT_DOUBLE_EQ(5.0, cx);
    EXPECT_DOUBLE_EQ(5.0, cy);

    // Read reg2
    TABRegion reg2(defn);
    EXPECT_EQ(0, reg2.ReadGeometryFromMIFFile(&fp));
    geom = reg2.GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbPolygon, wkbFlatten(geom->getGeometryType()));
    EXPECT_EQ(2, geom->toPolygon()->getNumInteriorRings() + 1);
    EXPECT_EQ(2, reg2.GetBrushPattern());
    EXPECT_EQ(FALSE, reg2.GetBrushTransparent());
    EXPECT_EQ(10, reg2.GetBrushFGColor());
    EXPECT_EQ(20, reg2.GetBrushBGColor());

    fp.Close();
  }

  // Multi-section polygon organizePolygons
  {
    constexpr char kSelfIntPath[] =
        "/vsimem/mitab_feature_mif_test/self_int.mif";
    absl::Cleanup self_cleanup = [kSelfIntPath] { VSIUnlink(kSelfIntPath); };
    VsiMemTempWrapper wrapper(kSelfIntPath,
                              "Region 2\n"
                              "4\n"
                              "0 0\n"
                              "10 0\n"
                              "10 10\n"
                              "0 10\n"
                              "4\n"
                              "20 20\n"
                              "30 20\n"
                              "30 30\n"
                              "20 30\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kSelfIntPath, "rt"));
    ASSERT_NE(nullptr, fp.GetLine());

    TABRegion reg(defn);
    EXPECT_EQ(0, reg.ReadGeometryFromMIFFile(&fp));
    ASSERT_NE(nullptr, reg.GetGeometryRef());
    fp.Close();
  }

  // Error branches on write: invalid geometry
  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));
    WithQuietHandler quiet;
    CPLErrorReset();

    TABRegion bad_geom(defn);
    EXPECT_EQ(-1, bad_geom.WriteGeometryToMIFFile(&fp));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    fp.Close();
  }

  // Error branches on read: negative sections, < 2 vertices, incomplete coords
  {
    WithQuietHandler quiet;

    auto check_reg_err = [defn](const char* content) {
      constexpr char kPath[] =
          "/vsimem/mitab_feature_mif_test/reg_single_err.mif";
      VsiMemTempWrapper wrapper(kPath, content);
      MIDDATAFile fp("");
      if (fp.Open(kPath, "rt") == 0 && fp.GetLine() != nullptr) {
        CPLErrorReset();
        TABRegion r(defn);
        EXPECT_EQ(-1, r.ReadGeometryFromMIFFile(&fp));
        CPLErrorReset();
        fp.Close();
      }
    };

    check_reg_err("Region -5\n");
    check_reg_err("Region 1\n1\n0 0\n");
    check_reg_err("Region 1\n3\n0 0\n1\n");
  }
}

TEST(MitabFeatureMifTest, TABRectangleAndEllipseMIF) {
  auto* defn = new OGRFeatureDefn("test_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  constexpr char kMifPath[] = "/vsimem/mitab_feature_mif_test/rect_ell.mif";
  absl::Cleanup mif_cleanup = [kMifPath] { VSIUnlink(kMifPath); };

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));

    // 1. Rect
    TABRectangle rect(defn);
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(10.0, 20.0);
    ring->addPoint(30.0, 20.0);
    ring->addPoint(30.0, 40.0);
    ring->addPoint(10.0, 40.0);
    ring->addPoint(10.0, 20.0);
    poly->addRingDirectly(ring.release());
    ASSERT_EQ(OGRERR_NONE, rect.SetGeometryDirectly(poly.release()));
    rect.SetPenWidthMIF(1);
    rect.SetPenPattern(2);
    rect.SetPenColor(255);
    rect.SetBrushPattern(1);
    rect.SetBrushFGColor(100);
    rect.SetBrushTransparent(TRUE);
    EXPECT_EQ(0, rect.WriteGeometryToMIFFile(&fp));

    // 2. Roundrect
    TABRectangle roundrect(defn);
    auto rpoly = std::make_unique<OGRPolygon>();
    auto rring = std::make_unique<OGRLinearRing>();
    rring->addPoint(50.0, 60.0);
    rring->addPoint(70.0, 60.0);
    rring->addPoint(70.0, 80.0);
    rring->addPoint(50.0, 80.0);
    rring->addPoint(50.0, 60.0);
    rpoly->addRingDirectly(rring.release());
    ASSERT_EQ(OGRERR_NONE, roundrect.SetGeometryDirectly(rpoly.release()));
    roundrect.m_bRoundCorners = TRUE;
    roundrect.m_dRoundXRadius = 2.0;
    roundrect.m_dRoundYRadius = 2.0;
    roundrect.SetBrushPattern(2);
    roundrect.SetBrushFGColor(10);
    roundrect.SetBrushBGColor(20);
    roundrect.SetBrushTransparent(FALSE);
    EXPECT_EQ(0, roundrect.WriteGeometryToMIFFile(&fp));

    // 3. Ellipse (with OGRPolygon)
    TABEllipse ell(defn);
    auto epoly = std::make_unique<OGRPolygon>();
    auto ering = std::make_unique<OGRLinearRing>();
    ering->addPoint(0.0, 0.0);
    ering->addPoint(10.0, 0.0);
    ering->addPoint(10.0, 20.0);
    ering->addPoint(0.0, 20.0);
    ering->addPoint(0.0, 0.0);
    epoly->addRingDirectly(ering.release());
    ASSERT_EQ(OGRERR_NONE, ell.SetGeometryDirectly(epoly.release()));
    ell.SetPenWidthMIF(2);
    ell.SetPenPattern(1);
    ell.SetPenColor(50);
    ell.SetBrushPattern(1);
    ell.SetBrushFGColor(150);
    ell.SetBrushTransparent(TRUE);
    EXPECT_EQ(0, ell.WriteGeometryToMIFFile(&fp));

    // 4. Ellipse with OGRPoint
    TABEllipse ell_pt(defn);
    auto epoint = std::make_unique<OGRPoint>(15.0, 25.0);
    ASSERT_EQ(OGRERR_NONE, ell_pt.SetGeometryDirectly(epoint.release()));
    ell_pt.SetBrushPattern(3);
    ell_pt.SetBrushFGColor(10);
    ell_pt.SetBrushBGColor(20);
    ell_pt.SetBrushTransparent(FALSE);
    EXPECT_EQ(0, ell_pt.WriteGeometryToMIFFile(&fp));

    fp.Close();
  }

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "rt"));

    // Read rect
    ASSERT_NE(nullptr, fp.GetLine());
    TABRectangle rect(defn);
    EXPECT_EQ(0, rect.ReadGeometryFromMIFFile(&fp));
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    rect.GetMBR(minx, miny, maxx, maxy);
    EXPECT_DOUBLE_EQ(10.0, minx);
    EXPECT_DOUBLE_EQ(20.0, miny);
    EXPECT_DOUBLE_EQ(30.0, maxx);
    EXPECT_DOUBLE_EQ(40.0, maxy);
    EXPECT_EQ(FALSE, rect.m_bRoundCorners);

    // Read roundrect
    TABRectangle roundrect(defn);
    EXPECT_EQ(0, roundrect.ReadGeometryFromMIFFile(&fp));
    roundrect.GetMBR(minx, miny, maxx, maxy);
    EXPECT_DOUBLE_EQ(50.0, minx);
    EXPECT_DOUBLE_EQ(60.0, miny);
    EXPECT_DOUBLE_EQ(70.0, maxx);
    EXPECT_DOUBLE_EQ(80.0, maxy);
    EXPECT_EQ(TRUE, roundrect.m_bRoundCorners);
    EXPECT_DOUBLE_EQ(2.0, roundrect.m_dRoundXRadius);

    // Read ellipse
    TABEllipse ell(defn);
    EXPECT_EQ(0, ell.ReadGeometryFromMIFFile(&fp));
    ell.GetMBR(minx, miny, maxx, maxy);
    EXPECT_DOUBLE_EQ(0.0, minx);
    EXPECT_DOUBLE_EQ(0.0, miny);
    EXPECT_DOUBLE_EQ(10.0, maxx);
    EXPECT_DOUBLE_EQ(20.0, maxy);

    // Read ellipse point
    TABEllipse ell_pt(defn);
    EXPECT_EQ(0, ell_pt.ReadGeometryFromMIFFile(&fp));
    ell_pt.GetMBR(minx, miny, maxx, maxy);
    EXPECT_DOUBLE_EQ(15.0, minx);
    EXPECT_DOUBLE_EQ(25.0, miny);

    fp.Close();
  }

  // Roundrect radius on next line (5 tokens on line 1, 1 token on line 2)
  {
    constexpr char kRrPath[] = "/vsimem/mitab_feature_mif_test/rr_split.mif";
    absl::Cleanup rr_cleanup = [kRrPath] { VSIUnlink(kRrPath); };
    VsiMemTempWrapper wrapper(kRrPath,
                              "Roundrect 10 20 30 40\n"
                              "4\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kRrPath, "rt"));
    ASSERT_NE(nullptr, fp.GetLine());

    TABRectangle rr(defn);
    EXPECT_EQ(0, rr.ReadGeometryFromMIFFile(&fp));
    EXPECT_EQ(TRUE, rr.m_bRoundCorners);
    EXPECT_DOUBLE_EQ(2.0, rr.m_dRoundXRadius);
    fp.Close();
  }

  // Error branches: missing geometry on write, < 5 tokens on read
  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));
    WithQuietHandler quiet;
    CPLErrorReset();

    TABRectangle r_empty(defn);
    EXPECT_EQ(-1, r_empty.WriteGeometryToMIFFile(&fp));
    TABEllipse e_empty(defn);
    EXPECT_EQ(-1, e_empty.WriteGeometryToMIFFile(&fp));
    CPLErrorReset();
    fp.Close();

    constexpr char kShortPath[] = "/vsimem/mitab_feature_mif_test/short_re.mif";
    absl::Cleanup short_cleanup = [kShortPath] { VSIUnlink(kShortPath); };
    VsiMemTempWrapper wrapper(kShortPath,
                              "Rect 1 2 3\n"
                              "Ellipse 1 2 3 4 5 6\n");
    ASSERT_EQ(0, fp.Open(kShortPath, "rt"));
    ASSERT_NE(nullptr, fp.GetLine());
    EXPECT_EQ(-1, r_empty.ReadGeometryFromMIFFile(&fp));
    ASSERT_NE(nullptr, fp.GetLine());
    EXPECT_EQ(-1, e_empty.ReadGeometryFromMIFFile(&fp));
    fp.Close();
  }
}

TEST(MitabFeatureMifTest, TABArcMIF) {
  auto* defn = new OGRFeatureDefn("test_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  constexpr char kMifPath[] = "/vsimem/mitab_feature_mif_test/arc.mif";
  absl::Cleanup mif_cleanup = [kMifPath] { VSIUnlink(kMifPath); };

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));

    // Write Arc
    TABArc arc(defn);
    arc.m_dCenterX = 50.0;
    arc.m_dCenterY = 50.0;
    arc.m_dXRadius = 20.0;
    arc.m_dYRadius = 10.0;
    arc.SetStartAngle(45.0);
    arc.SetEndAngle(135.0);
    arc.SetPenWidthMIF(1);
    arc.SetPenPattern(2);
    arc.SetPenColor(255);
    EXPECT_EQ(0, arc.WriteGeometryToMIFFile(&fp));

    fp.Close();
  }

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "rt"));

    // Read Arc (5-token header + 2-token angle line)
    ASSERT_NE(nullptr, fp.GetLine());
    TABArc arc(defn);
    EXPECT_EQ(0, arc.ReadGeometryFromMIFFile(&fp));
    EXPECT_DOUBLE_EQ(50.0, arc.m_dCenterX);
    EXPECT_DOUBLE_EQ(50.0, arc.m_dCenterY);
    EXPECT_DOUBLE_EQ(20.0, arc.m_dXRadius);
    EXPECT_DOUBLE_EQ(10.0, arc.m_dYRadius);
    EXPECT_DOUBLE_EQ(45.0, arc.GetStartAngle());
    EXPECT_DOUBLE_EQ(135.0, arc.GetEndAngle());
    EXPECT_EQ(1, arc.GetPenWidthMIF());

    fp.Close();
  }

  // 7-token single-line Arc, endAngle < startAngle, and flipped X axis
  {
    constexpr char kArcVarPath[] = "/vsimem/mitab_feature_mif_test/arc_var.mif";
    absl::Cleanup var_cleanup = [kArcVarPath] { VSIUnlink(kArcVarPath); };
    VsiMemTempWrapper wrapper(kArcVarPath,
                              "Arc 30 40 70 60 270 90\n"
                              "Arc 30 40 70 60 45 135\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kArcVarPath, "rt"));

    // Read 7-token with endAngle < startAngle (270 to 90)
    ASSERT_NE(nullptr, fp.GetLine());
    TABArc arc1(defn);
    EXPECT_EQ(0, arc1.ReadGeometryFromMIFFile(&fp));
    EXPECT_DOUBLE_EQ(270.0, arc1.GetStartAngle());
    EXPECT_DOUBLE_EQ(90.0, arc1.GetEndAngle());

    // Read with flipped X multiplier (arc1 already consumed up to next feature
    // keyword)
    fp.SetTranslation(-1.0, 1.0, 0.0, 0.0);
    TABArc arc2(defn);
    EXPECT_EQ(0, arc2.ReadGeometryFromMIFFile(&fp));
    EXPECT_DOUBLE_EQ(315.0, arc2.GetStartAngle());
    EXPECT_DOUBLE_EQ(225.0, arc2.GetEndAngle());

    fp.Close();
  }

  // Error branches: bad tokens, fabs(end - start) >= 721
  {
    constexpr char kErrPath[] = "/vsimem/mitab_feature_mif_test/arc_err.mif";
    absl::Cleanup err_cleanup = [kErrPath] { VSIUnlink(kErrPath); };
    VsiMemTempWrapper wrapper(kErrPath,
                              "Arc 1 2 3\n"
                              "Arc 1 2 3 4 5 6 7 8\n"
                              "Arc 0 0 10 10 0 800\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kErrPath, "rt"));
    WithQuietHandler quiet;

    ASSERT_NE(nullptr, fp.GetLine());
    TABArc a(defn);
    EXPECT_EQ(-1, a.ReadGeometryFromMIFFile(&fp));

    ASSERT_NE(nullptr, fp.GetLine());
    EXPECT_EQ(-1, a.ReadGeometryFromMIFFile(&fp));

    ASSERT_NE(nullptr, fp.GetLine());
    EXPECT_EQ(-1, a.ReadGeometryFromMIFFile(&fp));

    CPLErrorReset();
    fp.Close();
  }
}

TEST(MitabFeatureMifTest, TABTextMIF) {
  auto* defn = new OGRFeatureDefn("test_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  constexpr char kMifPath[] = "/vsimem/mitab_feature_mif_test/text.mif";
  absl::Cleanup mif_cleanup = [kMifPath] { VSIUnlink(kMifPath); };

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));
    fp.SetEncoding("ISO-8859-1");

    // 1. Text with Font BG, Spacing 1.5, Justify Center, Angle, Label Line
    // Simple
    TABText t1(defn);
    t1.SetTextString("Hello\nWorld");
    t1.SetFontName("Arial");
    t1.SetFontFGColor(100);
    t1.SetFontBGColor(200);
    t1.SetFontStyleMIFValue(1, /*bBGColorSet=*/TRUE);
    t1.SetTextSpacing(TABTS1_5);
    t1.SetTextJustification(TABTJCenter);
    t1.SetTextAngle(30.0);
    t1.SetTextLineType(TABTLSimple);
    t1.SetTextLineEndPoint(100.0, 200.0);
    EXPECT_EQ(0, t1.WriteGeometryToMIFFile(&fp));

    // 2. Text with no Font BG, Spacing 2.0, Justify Right, Angle, Label Line
    // Arrow
    TABText t2(defn);
    t2.SetTextString("SingleLine");
    t2.SetFontName("Times New Roman");
    t2.SetFontFGColor(50);
    t2.SetFontStyleMIFValue(0);
    t2.SetTextSpacing(TABTSDouble);
    t2.SetTextJustification(TABTJRight);
    t2.SetTextAngle(120.0);
    t2.SetTextLineType(TABTLArrow);
    t2.SetTextLineEndPoint(300.0, 400.0);
    EXPECT_EQ(0, t2.WriteGeometryToMIFFile(&fp));

    fp.Close();
  }

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "rt"));
    fp.SetEncoding("ISO-8859-1");

    // Read t1
    ASSERT_NE(nullptr, fp.GetLine());
    TABText t1(defn);
    EXPECT_EQ(0, t1.ReadGeometryFromMIFFile(&fp));
    EXPECT_THAT(t1.GetTextString(), HasSubstr("Hello\nWorld"));
    EXPECT_STREQ("Arial", t1.GetFontNameRef());
    EXPECT_EQ(100, t1.GetFontFGColor());
    EXPECT_EQ(200, t1.GetFontBGColor());
    EXPECT_EQ(TABTS1_5, t1.GetTextSpacing());
    EXPECT_EQ(TABTJCenter, t1.GetTextJustification());
    EXPECT_DOUBLE_EQ(30.0, t1.GetTextAngle());
    EXPECT_EQ(TABTLSimple, t1.GetTextLineType());
    double ex = 0, ey = 0;
    t1.GetTextLineEndPoint(ex, ey);
    EXPECT_DOUBLE_EQ(100.0, ex);
    EXPECT_DOUBLE_EQ(200.0, ey);

    // Read t2
    TABText t2(defn);
    EXPECT_EQ(0, t2.ReadGeometryFromMIFFile(&fp));
    EXPECT_STREQ("SingleLine", t2.GetTextString());
    EXPECT_STREQ("Times New Roman", t2.GetFontNameRef());
    EXPECT_EQ(TABTSDouble, t2.GetTextSpacing());
    EXPECT_EQ(TABTJRight, t2.GetTextJustification());
    EXPECT_DOUBLE_EQ(120.0, t2.GetTextAngle());
    EXPECT_EQ(TABTLArrow, t2.GetTextLineType());
    t2.GetTextLineEndPoint(ex, ey);
    EXPECT_DOUBLE_EQ(300.0, ex);
    EXPECT_DOUBLE_EQ(400.0, ey);

    fp.Close();
  }

  // Quadrants of text angle (dSin/dCos variations: 60, 120, 210, 300)
  // and variations of Text keyword line (1 token, 1 token with box directly on
  // next line, 1 token with empty line)
  {
    constexpr char kVarPath[] = "/vsimem/mitab_feature_mif_test/text_var.mif";
    absl::Cleanup var_cleanup = [kVarPath] { VSIUnlink(kVarPath); };
    VsiMemTempWrapper wrapper(kVarPath,
                              "Text\n"
                              "\"Quad1\"\n"
                              "0 0 10 10\n"
                              "Angle 60\n"
                              "Text\n"
                              "\"Quad2\"\n"
                              "0 0 10 10\n"
                              "Angle 120\n"
                              "Text\n"
                              "\"Quad3\"\n"
                              "0 0 10 10\n"
                              "Angle 210\n"
                              "Text\n"
                              "\"Quad4\"\n"
                              "0 0 10 10\n"
                              "Angle 300\n"
                              "Text\n"
                              "0 0 10 10\n"
                              "Text\n"
                              "\n"
                              "0 0 10 10\n"
                              "Text \"EmptyBox\"\n"
                              "10 10 10 10\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kVarPath, "rt"));

    ASSERT_NE(nullptr, fp.GetLine());
    {
      TABText t(defn);
      EXPECT_EQ(0, t.ReadGeometryFromMIFFile(&fp));
      EXPECT_STREQ("Quad1", t.GetTextString());
    }

    {
      TABText t(defn);
      EXPECT_EQ(0, t.ReadGeometryFromMIFFile(&fp));
      EXPECT_STREQ("Quad2", t.GetTextString());
    }

    {
      TABText t(defn);
      EXPECT_EQ(0, t.ReadGeometryFromMIFFile(&fp));
      EXPECT_STREQ("Quad3", t.GetTextString());
    }

    {
      TABText t(defn);
      EXPECT_EQ(0, t.ReadGeometryFromMIFFile(&fp));
      EXPECT_STREQ("Quad4", t.GetTextString());
    }

    // 1-token Text with 4-token box directly on next line
    {
      TABText t(defn);
      EXPECT_EQ(0, t.ReadGeometryFromMIFFile(&fp));
    }

    // 1-token Text with blank line
    {
      TABText t(defn);
      EXPECT_EQ(0, t.ReadGeometryFromMIFFile(&fp));
    }

    // Zero-height box
    {
      TABText t(defn);
      EXPECT_EQ(0, t.ReadGeometryFromMIFFile(&fp));
      EXPECT_DOUBLE_EQ(0.0, t.GetTextBoxWidth());
    }

    fp.Close();
  }

  // Combined Spacing and Label line: "Spacing 1.5 Label Line simple x y" and
  // standalone "Label Line arrow x y"
  {
    constexpr char kCombPath[] = "/vsimem/mitab_feature_mif_test/text_comb.mif";
    absl::Cleanup comb_cleanup = [kCombPath] { VSIUnlink(kCombPath); };
    VsiMemTempWrapper wrapper(kCombPath,
                              "Text \"Comb1\"\n"
                              "0 0 10 10\n"
                              "Spacing 1.5 Label Line simple 50 60\n"
                              "Text \"Comb2\"\n"
                              "0 0 10 10\n"
                              "Label Line arrow 70 80\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kCombPath, "rt"));

    ASSERT_NE(nullptr, fp.GetLine());
    TABText t1(defn);
    EXPECT_EQ(0, t1.ReadGeometryFromMIFFile(&fp));
    EXPECT_EQ(TABTS1_5, t1.GetTextSpacing());
    EXPECT_EQ(TABTLSimple, t1.GetTextLineType());
    double x = 0, y = 0;
    t1.GetTextLineEndPoint(x, y);
    EXPECT_DOUBLE_EQ(50.0, x);
    EXPECT_DOUBLE_EQ(60.0, y);

    TABText t2(defn);
    EXPECT_EQ(0, t2.ReadGeometryFromMIFFile(&fp));
    EXPECT_EQ(TABTLArrow, t2.GetTextLineType());
    t2.GetTextLineEndPoint(x, y);
    EXPECT_DOUBLE_EQ(70.0, x);
    EXPECT_DOUBLE_EQ(80.0, y);

    fp.Close();
  }

  // Error branches: bad token count on Text line or Box line
  {
    constexpr char kErrPath[] = "/vsimem/mitab_feature_mif_test/text_err.mif";
    absl::Cleanup err_cleanup = [kErrPath] { VSIUnlink(kErrPath); };
    VsiMemTempWrapper wrapper(kErrPath,
                              "Text a b c\n"
                              "Text \"Valid\"\n"
                              "0 0 10\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kErrPath, "rt"));
    ASSERT_NE(nullptr, fp.GetLine());

    TABText t(defn);
    EXPECT_EQ(-1, t.ReadGeometryFromMIFFile(&fp));

    ASSERT_NE(nullptr, fp.GetLine());
    EXPECT_EQ(-1, t.ReadGeometryFromMIFFile(&fp));

    fp.Close();
  }
}

TEST(MitabFeatureMifTest, TABMultiPointAndCollectionMIF) {
  auto* defn = new OGRFeatureDefn("test_layer");
  defn->Reference();
  absl::Cleanup defn_cleanup = [defn] { defn->Release(); };

  constexpr char kMifPath[] = "/vsimem/mitab_feature_mif_test/mp_coll.mif";
  absl::Cleanup mif_cleanup = [kMifPath] { VSIUnlink(kMifPath); };

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));

    // 1. MultiPoint
    TABMultiPoint mp(defn);
    auto mgeom = std::make_unique<OGRMultiPoint>();
    mgeom->addGeometryDirectly(new OGRPoint(10.0, 20.0));
    mgeom->addGeometryDirectly(new OGRPoint(30.0, 40.0));
    ASSERT_EQ(OGRERR_NONE, mp.SetGeometryDirectly(mgeom.release()));
    mp.SetSymbolNo(35);
    mp.SetSymbolColor(128);
    mp.SetSymbolSize(14);
    EXPECT_EQ(0, mp.WriteGeometryToMIFFile(&fp));

    // 2. Collection (Region + Polyline + MultiPoint)
    TABCollection coll(defn);
    auto reg = std::make_unique<TABRegion>(defn);
    auto poly = std::make_unique<OGRPolygon>();
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(10, 0);
    ring->addPoint(10, 10);
    ring->addPoint(0, 10);
    ring->addPoint(0, 0);
    poly->addRingDirectly(ring.release());
    ASSERT_EQ(OGRERR_NONE, reg->SetGeometryDirectly(poly.release()));
    coll.SetRegionDirectly(reg.release());

    auto pline = std::make_unique<TABPolyline>(defn);
    auto line = std::make_unique<OGRLineString>();
    line->addPoint(20, 20);
    line->addPoint(30, 30);
    ASSERT_EQ(OGRERR_NONE, pline->SetGeometryDirectly(line.release()));
    coll.SetPolylineDirectly(pline.release());

    auto mpt = std::make_unique<TABMultiPoint>(defn);
    auto mpt_geom = std::make_unique<OGRMultiPoint>();
    mpt_geom->addGeometryDirectly(new OGRPoint(40, 40));
    mpt_geom->addGeometryDirectly(new OGRPoint(50, 50));
    ASSERT_EQ(OGRERR_NONE, mpt->SetGeometryDirectly(mpt_geom.release()));
    coll.SetMultiPointDirectly(mpt.release());

    EXPECT_EQ(0, coll.WriteGeometryToMIFFile(&fp));

    fp.Close();
  }

  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "rt"));

    // Read MultiPoint
    ASSERT_NE(nullptr, fp.GetLine());
    TABMultiPoint mp(defn);
    EXPECT_EQ(0, mp.ReadGeometryFromMIFFile(&fp));
    OGRGeometry* geom = mp.GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbMultiPoint, wkbFlatten(geom->getGeometryType()));
    EXPECT_EQ(2, geom->toMultiPoint()->getNumGeometries());
    EXPECT_EQ(35, mp.GetSymbolNo());
    EXPECT_EQ(128, mp.GetSymbolColor());
    EXPECT_EQ(14, mp.GetSymbolSize());

    // Read Collection
    TABCollection coll(defn);
    EXPECT_EQ(0, coll.ReadGeometryFromMIFFile(&fp));
    geom = coll.GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbGeometryCollection, wkbFlatten(geom->getGeometryType()));
    EXPECT_EQ(3, geom->toGeometryCollection()->getNumGeometries());
    ASSERT_NE(nullptr, coll.GetRegionRef());
    ASSERT_NE(nullptr, coll.GetPolylineRef());
    ASSERT_NE(nullptr, coll.GetMultiPointRef());

    fp.Close();
  }

  // Collection with blank line inside (blank line consumes an iteration)
  {
    constexpr char kCollBlankPath[] =
        "/vsimem/mitab_feature_mif_test/coll_blank.mif";
    absl::Cleanup blank_cleanup = [kCollBlankPath] {
      VSIUnlink(kCollBlankPath);
    };
    VsiMemTempWrapper wrapper(kCollBlankPath,
                              "COLLECTION 3\n"
                              "\n"
                              "LINE 0 0 10 10\n"
                              "\n"
                              "MULTIPOINT 1\n"
                              "20 20\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kCollBlankPath, "rt"));
    ASSERT_NE(nullptr, fp.GetLine());

    TABCollection coll(defn);
    EXPECT_EQ(0, coll.ReadGeometryFromMIFFile(&fp));
    EXPECT_EQ(
        2, coll.GetGeometryRef()->toGeometryCollection()->getNumGeometries());
    fp.Close();
  }

  // Error branches for MultiPoint: missing geom on write, bad tokens on read
  {
    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kMifPath, "wt"));
    WithQuietHandler quiet;
    CPLErrorReset();

    TABMultiPoint bad_mp(defn);
    EXPECT_EQ(0, bad_mp.WriteGeometryToMIFFile(&fp));

    TABMultiPoint bad_mp2(defn);
    bad_mp2.SetGeometryDirectly(new OGRLineString());
    EXPECT_EQ(0, bad_mp2.WriteGeometryToMIFFile(&fp));
    CPLErrorReset();
    fp.Close();

    constexpr char kBadMpPath[] = "/vsimem/mitab_feature_mif_test/bad_mp.mif";
    absl::Cleanup bad_cleanup = [kBadMpPath] { VSIUnlink(kBadMpPath); };
    VsiMemTempWrapper wrapper(kBadMpPath,
                              "MultiPoint 1 2 3\n"
                              "MultiPoint 1\n"
                              "10.0\n");

    ASSERT_EQ(0, fp.Open(kBadMpPath, "rt"));
    ASSERT_NE(nullptr, fp.GetLine());
    EXPECT_EQ(-1, bad_mp.ReadGeometryFromMIFFile(&fp));

    ASSERT_NE(nullptr, fp.GetLine());
    EXPECT_EQ(-1, bad_mp.ReadGeometryFromMIFFile(&fp));
    fp.Close();
  }

  // Error branches for Collection: unexpected EOF, invalid part keyword
  {
    constexpr char kBadCollPath[] =
        "/vsimem/mitab_feature_mif_test/bad_coll.mif";
    absl::Cleanup bad_cleanup = [kBadCollPath] { VSIUnlink(kBadCollPath); };
    VsiMemTempWrapper wrapper(kBadCollPath,
                              "COLLECTION 1 2 3\n"
                              "COLLECTION 1\n"
                              "UNKNOWN_TYPE\n"
                              "COLLECTION 2\n"
                              "LINE 0 0 10 10\n");

    MIDDATAFile fp("");
    ASSERT_EQ(0, fp.Open(kBadCollPath, "rt"));
    WithQuietHandler quiet;

    ASSERT_NE(nullptr, fp.GetLine());
    TABCollection coll(defn);
    EXPECT_EQ(-1, coll.ReadGeometryFromMIFFile(&fp));

    ASSERT_NE(nullptr, fp.GetLine());
    EXPECT_EQ(-1, coll.ReadGeometryFromMIFFile(&fp));

    ASSERT_NE(nullptr, fp.GetLine());
    EXPECT_EQ(-1, coll.ReadGeometryFromMIFFile(&fp));

    CPLErrorReset();
    fp.Close();
  }
}

}  // namespace
}  // namespace autotest2
