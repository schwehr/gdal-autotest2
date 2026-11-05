// Copyright 2026 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_vsi.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "ogr_spatialref.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_priv.h"

namespace {

void CleanVsiFiles(const std::vector<std::string>& files) {
  for (const auto& file : files) {
    VSIUnlink(file.c_str());
  }
}

TEST(MIFFileTest, UnopenedAndErrorCases) {
  MIFFile mif(nullptr);
  EXPECT_EQ(mif.GetFileClass(), TABFC_MIFFile);
  EXPECT_STREQ(mif.GetTableName(), "");
  EXPECT_EQ(mif.GetLayerDefn(), nullptr);
  EXPECT_EQ(mif.GetNativeFieldType(0), TABFUnknown);
  EXPECT_FALSE(mif.IsFieldIndexed(0));
  EXPECT_FALSE(mif.IsFieldUnique(0));
  EXPECT_EQ(mif.GetSpatialRef(), nullptr);
  EXPECT_EQ(mif.GetProjInfo(nullptr), -1);
  EXPECT_EQ(mif.SetProjInfo(nullptr), -1);
  EXPECT_EQ(mif.SetQuickSpatialIndexMode(), -1);
  EXPECT_EQ(mif.GetVersion(), 300);

  EXPECT_EQ(mif.SetBounds(0, 0, 10, 10), -1);
  EXPECT_EQ(mif.SetFieldIndexed(0), -1);
  EXPECT_EQ(mif.CreateFeature(nullptr), OGRERR_UNSUPPORTED_OPERATION);
  EXPECT_EQ(mif.GetFeatureRef(1), nullptr);

  // In write mode, GetNextFeatureId and GetFeatureRef fail
  const std::string wmif = "/vsimem/w_err.mif";
  const std::string wmid = "/vsimem/w_err.mid";
  absl::Cleanup cleanup_wmif = [&]() { CleanVsiFiles({wmif, wmid}); };
  MIFFile mif_w(nullptr);
  EXPECT_EQ(mif_w.Open(wmif.c_str(), TABWrite, FALSE, "UTF-8"), 0);
  EXPECT_EQ(mif_w.GetNextFeatureId(-1), -1);
  EXPECT_EQ(mif_w.GetFeatureRef(1), nullptr);
  // CoordSys without bounds in write mode
  EXPECT_TRUE(mif_w.SetMIFCoordSys("Earth Projection 1, 104"));
  EXPECT_EQ(mif_w.AddFieldNative("fld", TABFChar, 10), 0);
  TABFeature wf(mif_w.GetLayerDefn());
  wf.SetField(0, "val");
  EXPECT_EQ(mif_w.CreateFeature(&wf), OGRERR_NONE);
  EXPECT_EQ(mif_w.Close(), 0);
  double xmin = 0;
  double ymin = 0;
  double xmax = 0;
  double ymax = 0;
  EXPECT_EQ(mif.GetBounds(xmin, ymin, xmax, ymax, FALSE), -1);
  OGREnvelope env;
  EXPECT_EQ(mif.IGetExtent(0, &env, false), OGRERR_FAILURE);
  int pts = 0;
  int lines = 0;
  int regs = 0;
  int txts = 0;
  EXPECT_EQ(mif.GetFeatureCountByType(pts, lines, regs, txts, FALSE), -1);
  EXPECT_EQ(mif.GetFeatureCount(FALSE), -1);

  // Invalid access mode
  EXPECT_EQ(mif.Open("/vsimem/err.mif", TABReadWrite, FALSE), -1);
  mif.Close();
  EXPECT_EQ(mif.Open("/vsimem/err.mif", TABReadWrite, TRUE), -1);
  mif.Close();

  // Invalid extensions
  EXPECT_EQ(mif.Open("/vsimem/bad.txt", TABRead, FALSE), -1);
  mif.Close();
  EXPECT_EQ(mif.Open("/vsimem/bad.txt", TABRead, TRUE), -1);
  mif.Close();
  EXPECT_EQ(mif.Open("/vsimem/bad.txt", TABWrite, FALSE), -1);
  mif.Close();
  EXPECT_EQ(mif.Open("/vsimem/bad.txt", TABWrite, TRUE), -1);
  mif.Close();

  // Non-existent file
  EXPECT_EQ(mif.Open("/vsimem/nonexistent.mif", TABRead, FALSE), -1);
  mif.Close();
  EXPECT_EQ(mif.Open("/vsimem/nonexistent.mif", TABRead, TRUE), -1);
  mif.Close();

  // Already open
  const std::string test_mif = "/vsimem/already_open.mif";
  const std::string test_mid = "/vsimem/already_open.mid";
  absl::Cleanup cleanup = [&]() { CleanVsiFiles({test_mif, test_mid}); };
  EXPECT_EQ(mif.Open(test_mif.c_str(), TABWrite), 0);
  EXPECT_EQ(mif.Open(test_mif.c_str(), TABWrite), -1);
  EXPECT_EQ(mif.Close(), 0);
}

TEST(MIFFileTest, WriteAndReadAllTypesAndAllFeatures) {
  const std::string base_path = "/vsimem/all_types";
  const std::string mif_path = base_path + ".mif";
  const std::string mid_path = base_path + ".mid";
  absl::Cleanup cleanup = [&]() { CleanVsiFiles({mif_path, mid_path}); };

  {
    MIFFile mif(nullptr);
    EXPECT_EQ(mif.Open(mif_path.c_str(), TABWrite), 0);
    EXPECT_EQ(mif.SetCharset("Neutral"), 0);
    EXPECT_EQ(mif.SetCharset("UTF-8"), 0);
    mif.SetStrictLaundering(false);

    // CoordSys setting
    EXPECT_TRUE(mif.SetMIFCoordSys(
        "CoordSys Earth Projection 1, 104 Bounds (-180, -90) (180, 90)"));
    EXPECT_TRUE(mif.SetMIFCoordSys("Earth Projection 1, 104"));

    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    EXPECT_TRUE(mif.SetSpatialRef(&srs));
    EXPECT_EQ(mif.SetBounds(-180, -90, 180, 90), 0);

    // Add all 10 field types
    EXPECT_EQ(mif.AddFieldNative("char_fld", TABFChar, 300, 0, TRUE, TRUE), 0);
    EXPECT_EQ(mif.AddFieldNative("int_fld", TABFInteger, 0, 0, TRUE, FALSE), 0);
    EXPECT_EQ(
        mif.AddFieldNative("smallint_fld", TABFSmallInt, 0, 0, FALSE, TRUE), 0);
    EXPECT_EQ(
        mif.AddFieldNative("largeint_fld", TABFLargeInt, 0, 0, FALSE, FALSE),
        0);
    EXPECT_EQ(mif.AddFieldNative("dec_fld", TABFDecimal, 0, 0, FALSE, FALSE),
              0);
    EXPECT_EQ(mif.AddFieldNative("dec2_fld", TABFDecimal, 12, 3, FALSE, FALSE),
              0);
    EXPECT_EQ(mif.AddFieldNative("float_fld", TABFFloat, 0, 0, FALSE, FALSE),
              0);
    EXPECT_EQ(mif.AddFieldNative("date_fld", TABFDate, 0, 0, FALSE, FALSE), 0);
    EXPECT_EQ(mif.AddFieldNative("time_fld", TABFTime, 0, 0, FALSE, FALSE), 0);
    EXPECT_EQ(
        mif.AddFieldNative("datetime_fld", TABFDateTime, 0, 0, FALSE, FALSE),
        0);
    EXPECT_EQ(
        mif.AddFieldNative("logical_fld", TABFLogical, 0, 0, FALSE, FALSE), 0);
    EXPECT_EQ(mif.AddFieldNative("bad_fld", TABFUnknown), -1);

    EXPECT_EQ(mif.SetFieldIndexed(1), 0);
    EXPECT_EQ(mif.SetFieldIndexed(99), -1);
    EXPECT_TRUE(mif.IsFieldIndexed(1));
    EXPECT_FALSE(mif.IsFieldIndexed(99));
    EXPECT_TRUE(mif.IsFieldUnique(0));
    EXPECT_FALSE(mif.IsFieldUnique(99));
    EXPECT_EQ(mif.GetNativeFieldType(0), TABFChar);
    EXPECT_EQ(mif.GetNativeFieldType(99), TABFUnknown);

    // TestCapability in write mode
    EXPECT_TRUE(mif.TestCapability(OLCSequentialWrite));
    EXPECT_TRUE(mif.TestCapability(OLCCreateField));
    EXPECT_TRUE(mif.TestCapability(OLCRandomRead));
    EXPECT_TRUE(mif.TestCapability(OLCStringsAsUTF8));
    EXPECT_FALSE(mif.TestCapability(OLCFastSpatialFilter));
    EXPECT_FALSE(mif.TestCapability("UNKNOWN_CAP"));

    const OGRFeatureDefn* poDefn = mif.GetLayerDefn();
    ASSERT_NE(poDefn, nullptr);

    auto PopulateFields = [&](TABFeature* feat, int id) {
      feat->SetField(0, "test_str");
      feat->SetField(1, 100 + id);
      feat->SetField(2, 10 + id);
      feat->SetField(3, static_cast<GIntBig>(10000000000LL + id));
      feat->SetField(4, 123.45);
      feat->SetField(5, 678.123);
      feat->SetField(6, 3.14159);
      feat->SetField(7, "2026/11/03");
      feat->SetField(8, "12:34:56");
      feat->SetField(9, "2026/11/03 12:34:56");
      feat->SetField(10, 1);
    };

    // 1. None geometry
    {
      TABFeature feat(poDefn);
      PopulateFields(&feat, 1);
      EXPECT_EQ(mif.CreateFeature(&feat), OGRERR_NONE);
      EXPECT_EQ(feat.GetFID(), 1);
    }

    // Now header is written; AddFieldNative and SetFeatureDefn must fail
    EXPECT_EQ(mif.AddFieldNative("after_header", TABFChar, 10), -1);
    OGRFeatureDefn dummy_defn("dummy");
    EXPECT_EQ(mif.SetFeatureDefn(&dummy_defn), -1);

    // 2. Point (with symbol 35)
    {
      TABPoint pt(poDefn);
      PopulateFields(&pt, 2);
      OGRPoint ogr_pt(10.0, 20.0);
      pt.SetGeometry(&ogr_pt);
      pt.SetSymbolNo(35);
      pt.SetSymbolColor(0xFF0000);
      pt.SetSymbolSize(12);
      EXPECT_EQ(mif.CreateFeature(&pt), OGRERR_NONE);
    }

    // 3. FontPoint
    {
      TABFontPoint fpt(poDefn);
      PopulateFields(&fpt, 3);
      OGRPoint ogr_pt(15.0, 25.0);
      fpt.SetGeometry(&ogr_pt);
      fpt.SetFontName("Arial");
      fpt.SetSymbolNo(40);
      fpt.SetSymbolSize(14);
      fpt.SetSymbolAngle(45.0);
      fpt.SetFontStyleTABValue(TABFSBold | TABFSHalo);
      EXPECT_EQ(mif.CreateFeature(&fpt), OGRERR_NONE);
    }

    // 4. CustomPoint
    {
      TABCustomPoint cpt(poDefn);
      PopulateFields(&cpt, 4);
      OGRPoint ogr_pt(18.0, 28.0);
      cpt.SetGeometry(&ogr_pt);
      cpt.SetSymbolName("custom.bmp");
      cpt.SetCustomSymbolStyle(TABCSApplyColor);
      EXPECT_EQ(mif.CreateFeature(&cpt), OGRERR_NONE);
    }

    // 5. Polyline (2-point LINE)
    {
      TABPolyline pl(poDefn);
      PopulateFields(&pl, 5);
      OGRLineString ls;
      ls.addPoint(1.0, 2.0);
      ls.addPoint(3.0, 4.0);
      pl.SetGeometry(&ls);
      EXPECT_EQ(mif.CreateFeature(&pl), OGRERR_NONE);
    }

    // 6. Polyline (smoothed PLINE with > 2 points)
    {
      TABPolyline pl(poDefn);
      PopulateFields(&pl, 6);
      OGRLineString ls;
      ls.addPoint(1.0, 2.0);
      ls.addPoint(3.0, 4.0);
      ls.addPoint(5.0, 6.0);
      ls.addPoint(7.0, 8.0);
      pl.SetGeometry(&ls);
      pl.m_bSmooth = TRUE;
      EXPECT_EQ(mif.CreateFeature(&pl), OGRERR_NONE);
    }

    // 7. Polyline (PLINE MULTIPLE)
    {
      TABPolyline pl(poDefn);
      PopulateFields(&pl, 7);
      OGRMultiLineString mls;
      auto ls1 = std::make_unique<OGRLineString>();
      ls1->addPoint(1.0, 1.0);
      ls1->addPoint(2.0, 2.0);
      auto ls2 = std::make_unique<OGRLineString>();
      ls2->addPoint(3.0, 3.0);
      ls2->addPoint(4.0, 4.0);
      mls.addGeometryDirectly(ls1.release());
      mls.addGeometryDirectly(ls2.release());
      pl.SetGeometry(&mls);
      EXPECT_EQ(mif.CreateFeature(&pl), OGRERR_NONE);
    }

    // 8. Region (single ring and SetCenter)
    {
      TABRegion reg(poDefn);
      PopulateFields(&reg, 8);
      OGRPolygon poly;
      OGRLinearRing ring;
      ring.addPoint(0.0, 0.0);
      ring.addPoint(0.0, 10.0);
      ring.addPoint(10.0, 10.0);
      ring.addPoint(10.0, 0.0);
      ring.addPoint(0.0, 0.0);
      poly.addRing(&ring);
      reg.SetGeometry(&poly);
      reg.SetCenter(5.0, 5.0);
      EXPECT_EQ(mif.CreateFeature(&reg), OGRERR_NONE);
    }

    // 9. Arc
    {
      TABArc arc(poDefn);
      PopulateFields(&arc, 9);
      arc.m_dCenterX = 50.0;
      arc.m_dCenterY = 50.0;
      arc.m_dXRadius = 10.0;
      arc.m_dYRadius = 20.0;
      arc.SetStartAngle(0.0);
      arc.SetEndAngle(90.0);
      OGRPoint pt(50.0, 50.0);
      arc.SetGeometry(&pt);
      EXPECT_EQ(mif.CreateFeature(&arc), OGRERR_NONE);
    }

    // 10. Text non-empty
    {
      TABText txt(poDefn);
      PopulateFields(&txt, 10);
      OGRPoint pt(30.0, 40.0);
      txt.SetGeometry(&pt);
      txt.SetTextString("Hello MIF");
      txt.SetTextBoxHeight(5.0);
      txt.SetTextBoxWidth(20.0);
      txt.SetTextAngle(15.0);
      txt.SetTextLineEndPoint(35.0, 45.0);
      txt.SetTextLineType(TABTLArrow);
      txt.SetTextJustification(TABTJCenter);
      txt.SetTextSpacing(TABTS1_5);
      EXPECT_EQ(mif.CreateFeature(&txt), OGRERR_NONE);
    }

    // 11. Text empty (converts to NONE on read)
    {
      TABText txt(poDefn);
      PopulateFields(&txt, 11);
      OGRPoint pt(31.0, 41.0);
      txt.SetGeometry(&pt);
      txt.SetTextString("");
      txt.SetTextBoxHeight(5.0);
      txt.SetTextBoxWidth(20.0);
      EXPECT_EQ(mif.CreateFeature(&txt), OGRERR_NONE);
    }

    // 12. Rectangle (sharp corners)
    {
      TABRectangle rect(poDefn);
      PopulateFields(&rect, 12);
      OGRPolygon poly;
      OGRLinearRing ring;
      ring.addPoint(1.0, 1.0);
      ring.addPoint(1.0, 5.0);
      ring.addPoint(5.0, 5.0);
      ring.addPoint(5.0, 1.0);
      ring.addPoint(1.0, 1.0);
      poly.addRing(&ring);
      rect.SetGeometry(&poly);
      rect.m_bRoundCorners = FALSE;
      EXPECT_EQ(mif.CreateFeature(&rect), OGRERR_NONE);
    }

    // 13. Rectangle (rounded corners)
    {
      TABRectangle rect(poDefn);
      PopulateFields(&rect, 13);
      OGRPolygon poly;
      OGRLinearRing ring;
      ring.addPoint(2.0, 2.0);
      ring.addPoint(2.0, 6.0);
      ring.addPoint(6.0, 6.0);
      ring.addPoint(6.0, 2.0);
      ring.addPoint(2.0, 2.0);
      poly.addRing(&ring);
      rect.SetGeometry(&poly);
      rect.m_bRoundCorners = TRUE;
      rect.m_dRoundXRadius = 0.5;
      rect.m_dRoundYRadius = 0.5;
      EXPECT_EQ(mif.CreateFeature(&rect), OGRERR_NONE);
    }

    // 14. Ellipse
    {
      TABEllipse ell(poDefn);
      PopulateFields(&ell, 14);
      ell.m_dCenterX = 60.0;
      ell.m_dCenterY = 70.0;
      ell.m_dXRadius = 15.0;
      ell.m_dYRadius = 25.0;
      OGRPoint pt(60.0, 70.0);
      ell.SetGeometry(&pt);
      EXPECT_EQ(mif.CreateFeature(&ell), OGRERR_NONE);
    }

    // 15. MultiPoint
    {
      TABMultiPoint mp(poDefn);
      PopulateFields(&mp, 15);
      OGRMultiPoint ogr_mp;
      OGRPoint pt1(1.0, 2.0);
      OGRPoint pt2(3.0, 4.0);
      ogr_mp.addGeometry(&pt1);
      ogr_mp.addGeometry(&pt2);
      mp.SetGeometry(&ogr_mp);
      mp.SetCenter(1.0, 2.0);
      EXPECT_EQ(mif.CreateFeature(&mp), OGRERR_NONE);
    }

    // 16. Test ICreateFeature with OGRFeature
    {
      OGRFeature ogr_f(const_cast<OGRFeatureDefn*>(poDefn));
      ogr_f.SetField(0, "ogr_feature");
      OGRPoint pt(100.0, 200.0);
      ogr_f.SetGeometry(&pt);
      EXPECT_EQ(mif.ICreateFeature(&ogr_f), OGRERR_NONE);
    }

    EXPECT_EQ(mif.Close(), 0);
  }

  // Now Reopen in TABRead mode
  {
    MIFFile mif(nullptr);
    EXPECT_EQ(mif.Open(mif_path.c_str(), TABRead), 0);
    EXPECT_GE(mif.GetVersion(), 900);
    EXPECT_STREQ(mif.GetTableName(), "all_types");
    EXPECT_NE(mif.GetSpatialRef(), nullptr);

    const OGRFeatureDefn* poDefn = mif.GetLayerDefn();
    ASSERT_NE(poDefn, nullptr);
    EXPECT_EQ(poDefn->GetFieldCount(), 11);

    EXPECT_EQ(mif.GetNativeFieldType(0), TABFChar);
    EXPECT_EQ(mif.GetNativeFieldType(1), TABFInteger);
    EXPECT_EQ(mif.GetNativeFieldType(2), TABFSmallInt);
    EXPECT_EQ(mif.GetNativeFieldType(3), TABFLargeInt);
    EXPECT_EQ(mif.GetNativeFieldType(4), TABFDecimal);
    EXPECT_EQ(mif.GetNativeFieldType(6), TABFFloat);
    EXPECT_EQ(mif.GetNativeFieldType(7), TABFDate);
    EXPECT_EQ(mif.GetNativeFieldType(8), TABFTime);
    EXPECT_EQ(mif.GetNativeFieldType(9), TABFDateTime);
    EXPECT_EQ(mif.GetNativeFieldType(10), TABFLogical);

    EXPECT_TRUE(mif.IsFieldIndexed(0));
    EXPECT_TRUE(mif.IsFieldIndexed(1));
    EXPECT_TRUE(mif.IsFieldUnique(0));
    EXPECT_TRUE(mif.IsFieldUnique(2));

    double xmin = 0;
    double ymin = 0;
    double xmax = 0;
    double ymax = 0;
    EXPECT_EQ(mif.GetBounds(xmin, ymin, xmax, ymax, FALSE), 0);
    EXPECT_EQ(mif.GetBounds(xmin, ymin, xmax, ymax, TRUE), 0);

    OGREnvelope env;
    EXPECT_EQ(mif.IGetExtent(0, &env, false), OGRERR_FAILURE);
    EXPECT_EQ(mif.IGetExtent(0, &env, true), OGRERR_NONE);
    EXPECT_TRUE(mif.TestCapability(OLCFastGetExtent));
    EXPECT_TRUE(mif.TestCapability(OLCFastFeatureCount));

    int pts = 0;
    int lines = 0;
    int regs = 0;
    int txts = 0;
    EXPECT_EQ(mif.GetFeatureCountByType(pts, lines, regs, txts, TRUE), 0);
    EXPECT_EQ(mif.GetFeatureCount(FALSE), 16);
    EXPECT_EQ(mif.GetFeatureCount(TRUE), 16);

    // Sequential read
    GIntBig nFid = -1;
    int count = 0;
    while ((nFid = mif.GetNextFeatureId(nFid)) != -1) {
      TABFeature* poFeat = mif.GetFeatureRef(nFid);
      ASSERT_NE(poFeat, nullptr);
      EXPECT_EQ(poFeat->GetFID(), nFid);
      count++;
    }
    EXPECT_EQ(count, 16);

    // Verify empty text feature became NONE
    mif.ResetReading();
    TABFeature* poEmptyText = mif.GetFeatureRef(11);
    ASSERT_NE(poEmptyText, nullptr);
    EXPECT_EQ(poEmptyText->GetFeatureClass(), TABFCNoGeomFeature);

    // Test GotoFeature non-sequential
    mif.ResetReading();
    EXPECT_NE(mif.GetFeatureRef(5), nullptr);
    EXPECT_NE(mif.GetFeatureRef(2),
              nullptr);  // Triggers ResetReading and advance
    EXPECT_NE(mif.GetFeatureRef(5), nullptr);
    EXPECT_EQ(mif.GetFeatureRef(0), nullptr);
    EXPECT_EQ(mif.GetFeatureRef(999), nullptr);

    // Test spatial and attribute filter
    mif.SetSpatialFilterRect(0, 0, 100, 100);
    EXPECT_GT(mif.GetFeatureCount(TRUE), 0);
    mif.SetSpatialFilter(nullptr);

    mif.SetAttributeFilter("int_fld = 102");
    EXPECT_EQ(mif.GetFeatureCount(TRUE), 1);
    mif.ResetReading();
    OGRFeature* poFiltered = mif.GetNextFeature();
    ASSERT_NE(poFiltered, nullptr);
    EXPECT_EQ(poFiltered->GetFieldAsInteger("int_fld"), 102);
    delete poFiltered;
    mif.SetAttributeFilter(nullptr);

    EXPECT_EQ(mif.Close(), 0);
  }
}

TEST(MIFFileTest, CollectionFeature) {
  const std::string col_mif = "/vsimem/col.mif";
  const std::string col_mid = "/vsimem/col.mid";
  absl::Cleanup cleanup = [&]() { CleanVsiFiles({col_mif, col_mid}); };

  {
    MIFFile mif(nullptr);
    EXPECT_EQ(mif.Open(col_mif.c_str(), TABWrite), 0);
    EXPECT_EQ(mif.AddFieldNative("id", TABFInteger), 0);

    TABCollection col(mif.GetLayerDefn());
    col.SetField(0, 1);
    auto reg = std::make_unique<TABRegion>(mif.GetLayerDefn());
    OGRPolygon poly;
    OGRLinearRing ring;
    ring.addPoint(0.0, 0.0);
    ring.addPoint(0.0, 2.0);
    ring.addPoint(2.0, 2.0);
    ring.addPoint(2.0, 0.0);
    ring.addPoint(0.0, 0.0);
    poly.addRing(&ring);
    reg->SetGeometry(&poly);

    auto pl = std::make_unique<TABPolyline>(mif.GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(5.0, 5.0);
    ls.addPoint(6.0, 6.0);
    pl->SetGeometry(&ls);

    col.SetRegionDirectly(reg.release());
    col.SetPolylineDirectly(pl.release());
    EXPECT_EQ(mif.CreateFeature(&col), OGRERR_NONE);
    EXPECT_EQ(mif.Close(), 0);
  }
}

TEST(MIFFileTest, SetFeatureDefnAndZeroFields) {
  const std::string test_mif = "/vsimem/set_defn.mif";
  const std::string test_mid = "/vsimem/set_defn.mid";
  absl::Cleanup cleanup = [&]() { CleanVsiFiles({test_mif, test_mid}); };

  MIFFile mif(nullptr);
  EXPECT_EQ(mif.Open(test_mif.c_str(), TABWrite), 0);

  OGRFeatureDefn defn("custom_defn");
  OGRFieldDefn f1("int_col", OFTInteger);
  OGRFieldDefn f2("bool_col", OFTInteger);
  f2.SetSubType(OFSTBoolean);
  OGRFieldDefn f3("real_col", OFTReal);
  OGRFieldDefn f4("str_col", OFTString);
  OGRFieldDefn f5("date_col", OFTDate);
  OGRFieldDefn f6("time_col", OFTTime);
  OGRFieldDefn f7("dt_col", OFTDateTime);
  defn.AddFieldDefn(&f1);
  defn.AddFieldDefn(&f2);
  defn.AddFieldDefn(&f3);
  defn.AddFieldDefn(&f4);
  defn.AddFieldDefn(&f5);
  defn.AddFieldDefn(&f6);
  defn.AddFieldDefn(&f7);

  EXPECT_EQ(mif.SetFeatureDefn(&defn, nullptr), 0);
  EXPECT_EQ(mif.GetLayerDefn()->GetFieldCount(), 7);

  // Write a feature to verify header output
  TABFeature feat(mif.GetLayerDefn());
  feat.SetField(0, 42);
  feat.SetField(1, 1);
  feat.SetField(2, 3.14);
  feat.SetField(3, "val");
  EXPECT_EQ(mif.CreateFeature(&feat), OGRERR_NONE);
  EXPECT_EQ(mif.Close(), 0);

  // Zero fields error in WriteMIFHeader
  const std::string zero_mif = "/vsimem/zero_fields.mif";
  const std::string zero_mid = "/vsimem/zero_fields.mid";
  absl::Cleanup cleanup_zero = [&]() { CleanVsiFiles({zero_mif, zero_mid}); };

  MIFFile mif_zero(nullptr);
  EXPECT_EQ(mif_zero.Open(zero_mif.c_str(), TABWrite), 0);
  // Closing with 0 fields attempts WriteMIFHeader and fails
  EXPECT_EQ(mif_zero.Close(), 0);
}

TEST(MIFFileTest, CraftedHeadersAndEdgeCases) {
  // 1. Uppercase extension
  const std::string upper_mif = "/vsimem/UPPER.MIF";
  const std::string upper_mid = "/vsimem/UPPER.MID";
  absl::Cleanup cleanup_upper = [&]() {
    CleanVsiFiles({upper_mif, upper_mid});
  };

  const char* mif_content =
      "Version 450\n"
      "Charset \"WindowsLatin1\"\n"
      "Delimiter \",\"\n"
      "Unique 1\n"
      "Index 2\n"
      "CoordSys Earth Projection 1, 104 Bounds (-180, -90) (180, 90)\n"
      "Transform 2.0, -3.0, 10.0, 20.0\n"
      "Columns 11\n"
      "  col1 char(20)\n"
      "  col2 integer(10)\n"
      "  col3 smallint(5)\n"
      "  col4 largeint(15)\n"
      "  col5 decimal(10, 2)\n"
      "  col6 float\n"
      "  col7 date\n"
      "  col8 time\n"
      "  col9 datetime\n"
      "  col10 logical\n"
      "  col11 integer\n"
      "Data\n"
      "Point 10.0 20.0\n"
      "    Symbol (35, 16711680, 12)\n"
      "Point 15.0 25.0\n"
      "    Symbol (40, 16711680, 14, \"Arial\", 1, 45.0)\n"
      "Point 18.0 28.0\n"
      "    Symbol (\"custom.bmp\", 16711680, 10, 1)\n"
      "Point 30.0 40.0\n";  // No symbol line

  const char* mid_content =
      "\"row1\",10,20,30,1.2,3.4,\"2026/01/01\",\"12:00:00\",\"2026/01/01 "
      "12:00:00\",T,100\n"
      "\"row2\",11,21,31,1.2,3.4,\"2026/01/01\",\"12:00:00\",\"2026/01/01 "
      "12:00:00\",F,101\n"
      "\"row3\",12,22,32,1.2,3.4,\"2026/01/01\",\"12:00:00\",\"2026/01/01 "
      "12:00:00\",T,102\n"
      "\"row4\",13,23,33,1.2,3.4,\"2026/01/01\",\"12:00:00\",\"2026/01/01 "
      "12:00:00\",F,103\n";

  VSILFILE* fp = VSIFileFromMemBuffer(
      upper_mif.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(mif_content)),
      strlen(mif_content), FALSE);
  VSIFCloseL(fp);
  fp = VSIFileFromMemBuffer(
      upper_mid.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(mid_content)),
      strlen(mid_content), FALSE);
  VSIFCloseL(fp);

  {
    MIFFile mif(nullptr);
    EXPECT_EQ(mif.Open(upper_mif.c_str(), TABRead), 0);
    EXPECT_GE(mif.GetVersion(), 450);
    EXPECT_TRUE(mif.IsFieldUnique(0));
    EXPECT_TRUE(mif.IsFieldIndexed(1));

    EXPECT_EQ(mif.GetLayerDefn()->GetGeomType(), wkbUnknown);

    TABFeature* f1 = mif.GetFeatureRef(1);
    ASSERT_NE(f1, nullptr);
    EXPECT_EQ(f1->GetFeatureClass(), TABFCPoint);

    TABFeature* f2 = mif.GetFeatureRef(2);
    ASSERT_NE(f2, nullptr);
    EXPECT_EQ(f2->GetFeatureClass(), TABFCFontPoint);

    TABFeature* f3 = mif.GetFeatureRef(3);
    ASSERT_NE(f3, nullptr);
    EXPECT_EQ(f3->GetFeatureClass(), TABFCCustomPoint);

    TABFeature* f4 = mif.GetFeatureRef(4);
    ASSERT_NE(f4, nullptr);
    EXPECT_EQ(f4->GetFeatureClass(), TABFCPoint);

    EXPECT_EQ(mif.Close(), 0);
  }

  // 2. Uniform line geometry check
  const std::string lines_mif = "/vsimem/lines.mif";
  const std::string lines_mid = "/vsimem/lines.mid";
  absl::Cleanup cleanup_lines = [&]() {
    CleanVsiFiles({lines_mif, lines_mid});
  };

  const char* lines_mif_content =
      "Version 300\n"
      "Columns 1\n"
      "  name char(10)\n"
      "Data\n"
      "Line 1.0 2.0 3.0 4.0\n"
      "Pline 3\n"
      "1.0 1.0\n"
      "2.0 2.0\n"
      "3.0 3.0\n";
  const char* lines_mid_content = "\"l1\"\n\"l2\"\n";

  fp = VSIFileFromMemBuffer(
      lines_mif.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(lines_mif_content)),
      strlen(lines_mif_content), FALSE);
  VSIFCloseL(fp);
  fp = VSIFileFromMemBuffer(
      lines_mid.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(lines_mid_content)),
      strlen(lines_mid_content), FALSE);
  VSIFCloseL(fp);

  {
    MIFFile mif(nullptr);
    EXPECT_EQ(mif.Open(lines_mif.c_str(), TABRead), 0);
    EXPECT_EQ(mif.GetLayerDefn()->GetGeomType(), wkbUnknown);
    EXPECT_EQ(mif.Close(), 0);
  }

  // 3. Zero columns (valid MIF with no attributes)
  const std::string zero_col_mif = "/vsimem/zero_cols.mif";
  absl::Cleanup cleanup_zero_col = [&]() { CleanVsiFiles({zero_col_mif}); };
  const char* zero_col_content =
      "Version 300\n"
      "Columns 0\n"
      "Data\n"
      "Point 1.0 2.0\n";
  fp = VSIFileFromMemBuffer(
      zero_col_mif.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(zero_col_content)),
      strlen(zero_col_content), FALSE);
  VSIFCloseL(fp);

  {
    MIFFile mif(nullptr);
    EXPECT_EQ(mif.Open(zero_col_mif.c_str(), TABRead), 0);
    EXPECT_EQ(mif.GetLayerDefn()->GetFieldCount(), 0);
    TABFeature* f = mif.GetFeatureRef(1);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(mif.Close(), 0);
  }

  // 4. Missing MID file when COLUMNS > 0 (handled gracefully in TABRead)
  const std::string no_mid_mif = "/vsimem/no_mid.mif";
  absl::Cleanup cleanup_no_mid = [&]() { CleanVsiFiles({no_mid_mif}); };
  const char* no_mid_content =
      "Version 300\n"
      "Columns 1\n"
      "  col1 char(10)\n"
      "Data\n"
      "Point 1.0 2.0\n";
  fp = VSIFileFromMemBuffer(
      no_mid_mif.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(no_mid_content)),
      strlen(no_mid_content), FALSE);
  VSIFCloseL(fp);

  {
    MIFFile mif(nullptr);
    // Should succeed opening MIF, MID is nullptr
    EXPECT_EQ(mif.Open(no_mid_mif.c_str(), TABRead), 0);
    TABFeature* f = mif.GetFeatureRef(1);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(mif.Close(), 0);
  }

  // 5. Corrupt headers
  const std::string corrupt_mif = "/vsimem/corrupt.mif";
  absl::Cleanup cleanup_corrupt = [&]() { CleanVsiFiles({corrupt_mif}); };
  const char* corrupt_content =
      "Version 300\n"
      "UnknownKeyword 123\n";
  fp = VSIFileFromMemBuffer(
      corrupt_mif.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(corrupt_content)),
      strlen(corrupt_content), FALSE);
  VSIFCloseL(fp);

  {
    MIFFile mif(nullptr);
    EXPECT_EQ(mif.Open(corrupt_mif.c_str(), TABRead, TRUE), -1);
  }

  // 6. Bad column type in MIF header
  const std::string bad_col_mif = "/vsimem/bad_col.mif";
  absl::Cleanup cleanup_bad_col = [&]() { CleanVsiFiles({bad_col_mif}); };
  const char* bad_col_content =
      "Version 300\n"
      "CoordSys\n"
      "  Earth Projection 1, 104\n"
      "  Bounds (-180, -90) (180, 90)\n"
      "Columns 1\n"
      "  col1 unknown_bad_type\n"
      "Data\n";
  fp = VSIFileFromMemBuffer(
      bad_col_mif.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(bad_col_content)),
      strlen(bad_col_content), FALSE);
  VSIFCloseL(fp);

  {
    MIFFile mif(nullptr);
    EXPECT_EQ(mif.Open(bad_col_mif.c_str(), TABRead), -1);
  }
}

}  // namespace
