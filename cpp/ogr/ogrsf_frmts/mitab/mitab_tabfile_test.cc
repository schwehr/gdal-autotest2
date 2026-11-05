// Copyright 2026 Google LLC
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#include <cmath>
#include <cstdint>
#include <memory>
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

void CopyVsiFile(const std::string& src, const std::string& dst) {
  vsi_l_offset nLength = 0;
  GByte* pData = VSIGetMemFileBuffer(src.c_str(), &nLength, FALSE);
  if (pData != nullptr && nLength > 0) {
    VSILFILE* fp = VSIFileFromMemBuffer(dst.c_str(), pData, nLength, FALSE);
    VSIFCloseL(fp);
  }
}

TEST(TABFileTest, UnopenedAndErrorCases) {
  TABFile tab(nullptr);
  EXPECT_EQ(tab.GetFileClass(), TABFC_TABFile);
  EXPECT_STREQ(tab.GetTableName(), "");
  EXPECT_EQ(tab.GetLayerDefn(), nullptr);
  EXPECT_EQ(tab.GetNativeFieldType(0), TABFUnknown);
  EXPECT_FALSE(tab.IsFieldIndexed(0));
  EXPECT_FALSE(tab.IsFieldUnique(0));
  EXPECT_EQ(tab.GetFieldIndexNumber(0), 0);
  EXPECT_EQ(tab.GetINDFileRef(), nullptr);
  EXPECT_EQ(tab.GetMAPFileRef(), nullptr);
  EXPECT_EQ(tab.GetVersion(), 300);
  EXPECT_EQ(tab.GetSpatialRef(), nullptr);

  EXPECT_EQ(tab.SetBounds(0, 0, 10, 10), -1);
  double xmin = 0;
  double ymin = 0;
  double xmax = 0;
  double ymax = 0;
  EXPECT_EQ(tab.GetBounds(xmin, ymin, xmax, ymax, FALSE), -1);
  OGREnvelope env;
  EXPECT_EQ(tab.IGetExtent(0, &env, false), OGRERR_FAILURE);
  int pts = 0;
  int lines = 0;
  int regs = 0;
  int txts = 0;
  EXPECT_EQ(tab.GetFeatureCountByType(pts, lines, regs, txts, FALSE), -1);
  EXPECT_EQ(tab.AddFieldNative("f", TABFChar, 10), -1);
  EXPECT_EQ(tab.SetFeatureDefn(nullptr), -1);
  EXPECT_EQ(tab.SetFieldIndexed(0), -1);
  EXPECT_EQ(tab.SetSpatialRef(nullptr), -1);
  EXPECT_EQ(tab.SetMIFCoordSys("CoordSys Earth"), -1);
  EXPECT_EQ(tab.SetProjInfo(nullptr), -1);
  EXPECT_EQ(tab.SetQuickSpatialIndexMode(TRUE), -1);
  EXPECT_EQ(tab.CreateFeature(nullptr), OGRERR_FAILURE);
  EXPECT_EQ(tab.WriteFeature(nullptr), -1);
  EXPECT_EQ(tab.GetNextFeatureId(-1), -1);
  EXPECT_EQ(tab.GetNextFeatureId_Spatial(-1), -1);
  EXPECT_EQ(tab.GetFeatureRef(1), nullptr);
  EXPECT_EQ(tab.ISetFeature(nullptr), OGRERR_FAILURE);
  EXPECT_EQ(tab.DeleteFeature(1), OGRERR_FAILURE);
  EXPECT_EQ(tab.DeleteField(0), OGRERR_FAILURE);
  EXPECT_EQ(tab.ReorderFields(nullptr), OGRERR_FAILURE);
  EXPECT_EQ(tab.AlterFieldDefn(0, nullptr, 0), OGRERR_FAILURE);
  EXPECT_EQ(tab.SyncToDisk(), OGRERR_NONE);
  EXPECT_EQ(tab.SetMetadataItem("DESCRIPTION", "ro_desc"), CE_None);

  // Invalid extensions (must call Close() after failed Open to avoid leaks)
  EXPECT_EQ(tab.Open("/vsimem/bad.txt", TABRead, FALSE), -1);
  tab.Close();
  EXPECT_EQ(tab.Open("/vsimem/bad.txt", TABRead, TRUE), -1);
  tab.Close();
  EXPECT_EQ(tab.Open("/vsimem/bad.txt", TABWrite, FALSE), -1);
  tab.Close();
  EXPECT_EQ(tab.Open("/vsimem/bad.txt", TABWrite, TRUE), -1);
  tab.Close();

  // Non-existent file
  EXPECT_EQ(tab.Open("/vsimem/no_such.tab", TABRead, FALSE), -1);
  tab.Close();
  EXPECT_EQ(tab.Open("/vsimem/no_such.tab", TABRead, TRUE), -1);
  tab.Close();

  // Invalid block size
  EXPECT_EQ(tab.Open("/vsimem/badblk.tab", TABWrite, FALSE, 777, nullptr), -1);
  tab.Close();

  // Already open
  const std::string test_tab = "/vsimem/already_open.tab";
  const std::string test_dat = "/vsimem/already_open.dat";
  const std::string test_map = "/vsimem/already_open.map";
  const std::string test_id = "/vsimem/already_open.id";
  absl::Cleanup cleanup = [&]() {
    CleanVsiFiles({test_tab, test_dat, test_map, test_id});
  };
  EXPECT_EQ(tab.Open(test_tab.c_str(), TABWrite), 0);
  EXPECT_EQ(tab.Open(test_tab.c_str(), TABWrite), -1);
  EXPECT_EQ(tab.Close(), 0);
}

TEST(TABFileTest, WriteReadAllFieldTypesMetadataAndFeatures) {
  const std::string base_path = "/vsimem/full";
  const std::string tab_path = base_path + ".tab";
  const std::string dat_path = base_path + ".dat";
  const std::string map_path = base_path + ".map";
  const std::string id_path = base_path + ".id";
  const std::string ind_path = base_path + ".ind";
  absl::Cleanup cleanup = [&]() {
    CleanVsiFiles({tab_path, dat_path, map_path, id_path, ind_path});
  };

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(tab_path.c_str(), TABWrite, FALSE, 512, "UTF-8"), 0);

    // Description metadata with quotes, newlines, and length capping (> 508)
    std::string long_desc(600, 'A');
    EXPECT_EQ(tab.SetMetadataItem("DESCRIPTION", long_desc.c_str()), CE_None);
    EXPECT_EQ(
        tab.SetMetadataItem("DESCRIPTION", "Line1\nLine2\rLine3\"quoted\"end"),
        CE_None);
    EXPECT_EQ(tab.SetMetadataItem("OTHER_KEY", "val", "OTHER_DOMAIN"), CE_None);

    EXPECT_EQ(tab.SetQuickSpatialIndexMode(TRUE), 0);
    EXPECT_EQ(tab.SetQuickSpatialIndexMode(FALSE), 0);
    tab.SetStrictLaundering(false);
    EXPECT_EQ(tab.SetCharset("UTF-8"), 0);

    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    EXPECT_EQ(tab.SetSpatialRef(&srs), 0);

    EXPECT_EQ(
        tab.SetMIFCoordSys(
            "CoordSys Earth Projection 1, 104 Bounds (-180, -90) (180, 90)"),
        0);

    TABProjInfo sProjInfo;
    EXPECT_EQ(tab.GetProjInfo(&sProjInfo), 0);
    EXPECT_EQ(tab.SetProjInfo(&sProjInfo), 0);
    EXPECT_EQ(tab.SetBounds(-180, -90, 180, 90), 0);

    // Add all 10 native types
    EXPECT_EQ(tab.AddFieldNative("char_fld", TABFChar, 300, 0, TRUE, FALSE), 0);
    EXPECT_EQ(tab.AddFieldNative("int_fld", TABFInteger, 8, 0, TRUE, FALSE), 0);
    EXPECT_EQ(
        tab.AddFieldNative("smallint_fld", TABFSmallInt, 4, 0, FALSE, FALSE),
        0);
    EXPECT_EQ(
        tab.AddFieldNative("largeint_fld", TABFLargeInt, 0, 0, FALSE, FALSE),
        0);
    EXPECT_EQ(tab.AddFieldNative("dec_fld", TABFDecimal, 0, 0, FALSE, FALSE),
              0);
    EXPECT_EQ(tab.AddFieldNative("dec2_fld", TABFDecimal, 10, 2, FALSE, FALSE),
              0);
    EXPECT_EQ(tab.AddFieldNative("float_fld", TABFFloat, 0, 0, FALSE, FALSE),
              0);
    EXPECT_EQ(tab.AddFieldNative("date_fld", TABFDate, 0, 0, FALSE, FALSE), 0);
    EXPECT_EQ(tab.AddFieldNative("time_fld", TABFTime, 0, 0, FALSE, FALSE), 0);
    EXPECT_EQ(
        tab.AddFieldNative("datetime_fld", TABFDateTime, 0, 0, FALSE, FALSE),
        0);
    EXPECT_EQ(
        tab.AddFieldNative("logical_fld", TABFLogical, 0, 0, FALSE, FALSE), 0);
    EXPECT_EQ(tab.AddFieldNative("bad_fld", TABFUnknown), -1);

    // Index field 2 (smallint_fld)
    EXPECT_EQ(tab.SetFieldIndexed(2), 0);
    EXPECT_EQ(tab.SetFieldIndexed(2), 0);  // Already indexed -> returns 0
    EXPECT_EQ(tab.SetFieldIndexed(99), -1);
    EXPECT_TRUE(tab.IsFieldIndexed(2));
    EXPECT_FALSE(tab.IsFieldIndexed(99));

    const OGRFeatureDefn* poDefn = tab.GetLayerDefn();
    ASSERT_NE(poDefn, nullptr);

    auto PopulateFields = [&](TABFeature* feat, int id) {
      feat->SetField(0, "test_str");
      feat->SetField(1, 100 + id);
      feat->SetField(2, 10 + id);
      feat->SetField(3, static_cast<GIntBig>(10000000000LL + id));
      feat->SetField(4, 123.45);
      feat->SetField(5, 678.12);
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
      EXPECT_EQ(tab.CreateFeature(&feat), OGRERR_NONE);
      EXPECT_EQ(feat.GetFID(), 1);
    }

    // Now that first feature is written, bounds and proj cannot be reset
    EXPECT_EQ(tab.SetBounds(0, 0, 10, 10), -1);
    EXPECT_EQ(tab.SetMIFCoordSys("CoordSys Earth"), -1);
    EXPECT_EQ(tab.SetProjInfo(&sProjInfo), -1);

    // 2. Point
    {
      TABPoint pt(poDefn);
      PopulateFields(&pt, 2);
      OGRPoint ogr_pt(10.0, 20.0);
      pt.SetGeometry(&ogr_pt);
      pt.SetSymbolNo(35);
      pt.SetSymbolColor(0xFF0000);
      pt.SetSymbolSize(12);
      EXPECT_EQ(tab.CreateFeature(&pt), OGRERR_NONE);
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
      EXPECT_EQ(tab.CreateFeature(&fpt), OGRERR_NONE);
    }

    // 4. CustomPoint
    {
      TABCustomPoint cpt(poDefn);
      PopulateFields(&cpt, 4);
      OGRPoint ogr_pt(18.0, 28.0);
      cpt.SetGeometry(&ogr_pt);
      cpt.SetSymbolName("custom.bmp");
      cpt.SetCustomSymbolStyle(TABCSApplyColor);
      EXPECT_EQ(tab.CreateFeature(&cpt), OGRERR_NONE);
    }

    // 5. Polyline (2-point LINE)
    {
      TABPolyline pl(poDefn);
      PopulateFields(&pl, 5);
      OGRLineString ls;
      ls.addPoint(1.0, 2.0);
      ls.addPoint(3.0, 4.0);
      pl.SetGeometry(&ls);
      EXPECT_EQ(tab.CreateFeature(&pl), OGRERR_NONE);
    }

    // 6. Polyline (smoothed PLINE)
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
      EXPECT_EQ(tab.CreateFeature(&pl), OGRERR_NONE);
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
      EXPECT_EQ(tab.CreateFeature(&pl), OGRERR_NONE);
    }

    // 8. Region
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
      EXPECT_EQ(tab.CreateFeature(&reg), OGRERR_NONE);
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
      EXPECT_EQ(tab.CreateFeature(&arc), OGRERR_NONE);
    }

    // 10. Text
    {
      TABText txt(poDefn);
      PopulateFields(&txt, 10);
      OGRPoint pt(30.0, 40.0);
      txt.SetGeometry(&pt);
      txt.SetTextString("Hello TAB");
      txt.SetTextBoxHeight(5.0);
      txt.SetTextBoxWidth(20.0);
      txt.SetTextAngle(15.0);
      txt.SetTextLineEndPoint(35.0, 45.0);
      txt.SetTextLineType(TABTLArrow);
      txt.SetTextJustification(TABTJCenter);
      txt.SetTextSpacing(TABTS1_5);
      EXPECT_EQ(tab.CreateFeature(&txt), OGRERR_NONE);
    }

    // 11. Rectangle (sharp)
    {
      TABRectangle rect(poDefn);
      PopulateFields(&rect, 11);
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
      EXPECT_EQ(tab.CreateFeature(&rect), OGRERR_NONE);
    }

    // 12. Rectangle (rounded)
    {
      TABRectangle rect(poDefn);
      PopulateFields(&rect, 12);
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
      EXPECT_EQ(tab.CreateFeature(&rect), OGRERR_NONE);
    }

    // 13. Ellipse
    {
      TABEllipse ell(poDefn);
      PopulateFields(&ell, 13);
      ell.m_dCenterX = 60.0;
      ell.m_dCenterY = 70.0;
      ell.m_dXRadius = 15.0;
      ell.m_dYRadius = 25.0;
      OGRPoint pt(60.0, 70.0);
      ell.SetGeometry(&pt);
      EXPECT_EQ(tab.CreateFeature(&ell), OGRERR_NONE);
    }

    // 14. MultiPoint
    {
      TABMultiPoint mp(poDefn);
      PopulateFields(&mp, 14);
      OGRMultiPoint ogr_mp;
      OGRPoint pt1(1.0, 2.0);
      OGRPoint pt2(3.0, 4.0);
      ogr_mp.addGeometry(&pt1);
      ogr_mp.addGeometry(&pt2);
      mp.SetGeometry(&ogr_mp);
      mp.SetCenter(1.0, 2.0);
      EXPECT_EQ(tab.CreateFeature(&mp), OGRERR_NONE);
    }

    // 15. Collection
    {
      TABCollection col(poDefn);
      PopulateFields(&col, 15);
      auto reg = std::make_unique<TABRegion>(poDefn);
      OGRPolygon poly;
      OGRLinearRing ring;
      ring.addPoint(0.0, 0.0);
      ring.addPoint(0.0, 2.0);
      ring.addPoint(2.0, 2.0);
      ring.addPoint(2.0, 0.0);
      ring.addPoint(0.0, 0.0);
      poly.addRing(&ring);
      reg->SetGeometry(&poly);

      auto pl = std::make_unique<TABPolyline>(poDefn);
      OGRLineString ls;
      ls.addPoint(5.0, 5.0);
      ls.addPoint(6.0, 6.0);
      pl->SetGeometry(&ls);

      col.SetRegionDirectly(reg.release());
      col.SetPolylineDirectly(pl.release());
      EXPECT_EQ(tab.CreateFeature(&col), OGRERR_NONE);
    }

    // 16. OGRFeature via ICreateFeature
    {
      OGRFeature ogr_f(const_cast<OGRFeatureDefn*>(poDefn));
      ogr_f.SetField(0, "ogr_feature");
      OGRPoint pt(100.0, 200.0);
      ogr_f.SetGeometry(&pt);
      EXPECT_EQ(tab.ICreateFeature(&ogr_f), OGRERR_NONE);
    }

    EXPECT_EQ(tab.SyncToDisk(), OGRERR_NONE);
    EXPECT_EQ(tab.Close(), 0);
  }

  // Reopen in TABRead mode using .map path (tests conversion to .tab)
  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(map_path.c_str(), TABRead), 0);
    EXPECT_GE(tab.GetVersion(), 900);
    EXPECT_STREQ(tab.GetTableName(), "full");
    EXPECT_NE(tab.GetSpatialRef(), nullptr);
    EXPECT_NE(tab.GetINDFileRef(), nullptr);
    EXPECT_NE(tab.GetMAPFileRef(), nullptr);

    TABProjInfo sProjInfo;
    EXPECT_EQ(tab.GetProjInfo(&sProjInfo), 0);

    const OGRFeatureDefn* poDefn = tab.GetLayerDefn();
    ASSERT_NE(poDefn, nullptr);
    EXPECT_EQ(poDefn->GetFieldCount(), 11);

    EXPECT_EQ(tab.GetNativeFieldType(0), TABFChar);
    EXPECT_EQ(tab.GetNativeFieldType(1), TABFInteger);
    EXPECT_EQ(tab.GetNativeFieldType(2), TABFSmallInt);
    EXPECT_EQ(tab.GetNativeFieldType(3), TABFLargeInt);
    EXPECT_EQ(tab.GetNativeFieldType(4), TABFDecimal);
    EXPECT_EQ(tab.GetNativeFieldType(6), TABFFloat);
    EXPECT_EQ(tab.GetNativeFieldType(7), TABFDate);
    EXPECT_EQ(tab.GetNativeFieldType(8), TABFTime);
    EXPECT_EQ(tab.GetNativeFieldType(9), TABFDateTime);
    EXPECT_EQ(tab.GetNativeFieldType(10), TABFLogical);

    EXPECT_TRUE(tab.IsFieldIndexed(0));
    EXPECT_TRUE(tab.IsFieldIndexed(1));
    EXPECT_TRUE(tab.IsFieldIndexed(2));
    EXPECT_GT(tab.GetFieldIndexNumber(0), 0);

    double xmin = 0;
    double ymin = 0;
    double xmax = 0;
    double ymax = 0;
    EXPECT_EQ(tab.GetBounds(xmin, ymin, xmax, ymax, FALSE), 0);
    EXPECT_EQ(tab.GetBounds(xmin, ymin, xmax, ymax, TRUE), 0);

    OGREnvelope env;
    EXPECT_EQ(tab.IGetExtent(0, &env, false), OGRERR_NONE);
    EXPECT_EQ(tab.IGetExtent(0, &env, true), OGRERR_NONE);

    int pts = 0;
    int lines = 0;
    int regs = 0;
    int txts = 0;
    EXPECT_EQ(tab.GetFeatureCountByType(pts, lines, regs, txts, FALSE), 0);
    EXPECT_EQ(tab.GetFeatureCountByType(pts, lines, regs, txts, TRUE), 0);
    EXPECT_EQ(tab.GetFeatureCount(FALSE), 16);
    EXPECT_EQ(tab.GetFeatureCount(TRUE), 16);

    // Sequential read
    GIntBig nFid = -1;
    int count = 0;
    while ((nFid = tab.GetNextFeatureId(nFid)) != -1) {
      TABFeature* poFeat = tab.GetFeatureRef(nFid);
      ASSERT_NE(poFeat, nullptr);
      EXPECT_EQ(poFeat->GetFID(), nFid);
      count++;
    }
    EXPECT_EQ(count, 16);

    // Spatial filter index traversal
    tab.SetSpatialFilterRect(0, 0, 50, 50);
    tab.ResetReading();
    int spatial_count = 0;
    OGRFeature* poSpatialFeat = nullptr;
    while ((poSpatialFeat = tab.GetNextFeature()) != nullptr) {
      spatial_count++;
      delete poSpatialFeat;
    }
    EXPECT_GT(spatial_count, 0);
    tab.SetSpatialFilter(nullptr);

    // Attribute index traversal
    tab.SetAttributeFilter("int_fld = 102");
    tab.ResetReading();
    OGRFeature* poAttrFeat = tab.GetNextFeature();
    ASSERT_NE(poAttrFeat, nullptr);
    EXPECT_EQ(poAttrFeat->GetFieldAsInteger("int_fld"), 102);
    delete poAttrFeat;

    // Attribute filter with 0 matches
    tab.SetAttributeFilter("int_fld = -99999");
    tab.ResetReading();
    EXPECT_EQ(tab.GetNextFeature(), nullptr);
    tab.SetAttributeFilter(nullptr);

    // Non-sequential GetFeatureRef
    tab.ResetReading();
    EXPECT_NE(tab.GetFeatureRef(5), nullptr);
    EXPECT_NE(tab.GetFeatureRef(2), nullptr);
    EXPECT_EQ(tab.GetFeatureRef(0), nullptr);
    EXPECT_EQ(tab.GetFeatureRef(999), nullptr);

    EXPECT_EQ(tab.Close(), 0);
  }
}

TEST(TABFileTest, ZeroFieldsDummyFIDAndSetFeatureDefn) {
  const std::string nofields_tab = "/vsimem/nofields.tab";
  const std::string nofields_dat = "/vsimem/nofields.dat";
  const std::string nofields_map = "/vsimem/nofields.map";
  const std::string nofields_id = "/vsimem/nofields.id";
  absl::Cleanup cleanup_nofields = [&]() {
    CleanVsiFiles({nofields_tab, nofields_dat, nofields_map, nofields_id});
  };

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(nofields_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(tab.SetBounds(-100, -100, 100, 100), 0);

    // Writing a feature without any fields triggers dummy FID column creation
    TABPoint pt(tab.GetLayerDefn());
    OGRPoint p(10, 20);
    pt.SetGeometry(&p);
    EXPECT_EQ(tab.CreateFeature(&pt), OGRERR_NONE);
    EXPECT_EQ(tab.Close(), 0);
  }

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(nofields_tab.c_str(), TABRead), 0);
    EXPECT_EQ(tab.GetFeatureCount(FALSE), 1);
    EXPECT_EQ(tab.GetLayerDefn()->GetFieldCount(), 1);
    EXPECT_NE(tab.GetFeatureRef(1), nullptr);
    EXPECT_EQ(tab.Close(), 0);
  }

  // SetFeatureDefn with paeMapInfoNativeFieldTypes == nullptr
  const std::string setdefn_tab = "/vsimem/setdefn.tab";
  const std::string setdefn_dat = "/vsimem/setdefn.dat";
  const std::string setdefn_map = "/vsimem/setdefn.map";
  const std::string setdefn_id = "/vsimem/setdefn.id";
  absl::Cleanup cleanup_setdefn = [&]() {
    CleanVsiFiles({setdefn_tab, setdefn_dat, setdefn_map, setdefn_id});
  };

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(setdefn_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(tab.SetBounds(-100, -100, 100, 100), 0);

    auto* poDefn = new OGRFeatureDefn("heap_defn");
    OGRFieldDefn f1("int_col", OFTInteger);
    OGRFieldDefn f2("bool_col", OFTInteger);
    f2.SetSubType(OFSTBoolean);
    OGRFieldDefn f3("real_dec", OFTReal);
    f3.SetWidth(10);
    f3.SetPrecision(2);
    OGRFieldDefn f4("real_float", OFTReal);
    OGRFieldDefn f5("date_col", OFTDate);
    OGRFieldDefn f6("time_col", OFTTime);
    OGRFieldDefn f7("dt_col", OFTDateTime);
    OGRFieldDefn f8("str_col", OFTString);
    poDefn->AddFieldDefn(&f1);
    poDefn->AddFieldDefn(&f2);
    poDefn->AddFieldDefn(&f3);
    poDefn->AddFieldDefn(&f4);
    poDefn->AddFieldDefn(&f5);
    poDefn->AddFieldDefn(&f6);
    poDefn->AddFieldDefn(&f7);
    poDefn->AddFieldDefn(&f8);

    EXPECT_EQ(tab.SetFeatureDefn(poDefn, nullptr), 0);
    EXPECT_EQ(tab.GetLayerDefn()->GetFieldCount(), 8);

    TABFeature feat(tab.GetLayerDefn());
    feat.SetField(0, 10);
    feat.SetField(1, 1);
    feat.SetField(2, 12.34);
    feat.SetField(3, 5.67);
    feat.SetField(7, "val");
    EXPECT_EQ(tab.CreateFeature(&feat), OGRERR_NONE);
    EXPECT_EQ(tab.Close(), 0);
  }

  // SetFeatureDefn with explicit native types
  const std::string setdefn2_tab = "/vsimem/setdefn2.tab";
  const std::string setdefn2_dat = "/vsimem/setdefn2.dat";
  const std::string setdefn2_map = "/vsimem/setdefn2.map";
  const std::string setdefn2_id = "/vsimem/setdefn2.id";
  absl::Cleanup cleanup_setdefn2 = [&]() {
    CleanVsiFiles({setdefn2_tab, setdefn2_dat, setdefn2_map, setdefn2_id});
  };

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(setdefn2_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(tab.SetBounds(-100, -100, 100, 100), 0);

    auto* poDefn = new OGRFeatureDefn("heap_defn2");
    OGRFieldDefn f1("c1", OFTString);
    OGRFieldDefn f2("c2", OFTInteger);
    poDefn->AddFieldDefn(&f1);
    poDefn->AddFieldDefn(&f2);

    TABFieldType nativeTypes[] = {TABFChar, TABFLargeInt};
    EXPECT_EQ(tab.SetFeatureDefn(poDefn, nativeTypes), 0);
    EXPECT_EQ(tab.GetLayerDefn()->GetFieldCount(), 2);
    EXPECT_EQ(tab.Close(), 0);
  }
}

TEST(TABFileTest, ReadWriteModeSchemaAndFeatureMutations) {
  const std::string rw_base = "/vsimem/rw";
  const std::string rw_tab = rw_base + ".tab";
  const std::string rw_dat = rw_base + ".dat";
  const std::string rw_map = rw_base + ".map";
  const std::string rw_id = rw_base + ".id";
  const std::string rw_ind = rw_base + ".ind";
  absl::Cleanup cleanup = [&]() {
    CleanVsiFiles({rw_tab, rw_dat, rw_map, rw_id, rw_ind});
  };

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(rw_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(tab.AddFieldNative("str_col", TABFChar, 20), 0);
    EXPECT_EQ(tab.AddFieldNative("int_col", TABFInteger, 10, 0, TRUE), 0);
    EXPECT_EQ(tab.AddFieldNative("drop_col", TABFSmallInt, 5), 0);
    EXPECT_EQ(tab.SetBounds(-100, -100, 100, 100), 0);
    EXPECT_EQ(tab.Close(), 0);
  }

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(rw_tab.c_str(), TABReadWrite), 0);

    // Capabilities in TABReadWrite
    EXPECT_TRUE(tab.TestCapability(OLCRandomRead));
    EXPECT_TRUE(tab.TestCapability(OLCSequentialWrite));
    EXPECT_TRUE(tab.TestCapability(OLCRandomWrite));
    EXPECT_TRUE(tab.TestCapability(OLCDeleteFeature));
    EXPECT_TRUE(tab.TestCapability(OLCFastFeatureCount));
    EXPECT_TRUE(tab.TestCapability(OLCFastSpatialFilter));
    EXPECT_TRUE(tab.TestCapability(OLCFastGetExtent));
    EXPECT_TRUE(tab.TestCapability(OLCCreateField));
    EXPECT_TRUE(tab.TestCapability(OLCDeleteField));
    EXPECT_TRUE(tab.TestCapability(OLCReorderFields));
    EXPECT_TRUE(tab.TestCapability(OLCAlterFieldDefn));
    EXPECT_FALSE(tab.TestCapability("UNKNOWN"));

    // Write 3 features
    for (int i = 1; i <= 3; ++i) {
      TABPoint pt(tab.GetLayerDefn());
      pt.SetField(0, "initial");
      pt.SetField(1, i * 10);
      pt.SetField(2, i);
      OGRPoint p(i * 10.0, i * 10.0);
      pt.SetGeometry(&p);
      EXPECT_EQ(tab.CreateFeature(&pt), OGRERR_NONE);
    }

    // ISetFeature tests
    std::unique_ptr<OGRFeature> f1(tab.GetFeature(1));
    ASSERT_NE(f1, nullptr);

    // 1. Unmodified feature -> fast path OGRERR_NONE
    EXPECT_EQ(tab.ISetFeature(f1.get()), OGRERR_NONE);

    // 2. Attribute-only change -> in-place DAT rewrite
    f1->SetField("str_col", "modified_attr");
    EXPECT_EQ(tab.ISetFeature(f1.get()), OGRERR_NONE);

    // 3. Geometry change -> MAP rewrite
    OGRPoint new_p(25.0, 35.0);
    f1->SetGeometry(&new_p);
    EXPECT_EQ(tab.ISetFeature(f1.get()), OGRERR_NONE);

    // 4. ISetFeature error cases
    f1->SetFID(OGRNullFID);
    EXPECT_EQ(tab.ISetFeature(f1.get()), OGRERR_FAILURE);
    f1->SetFID(0);
    EXPECT_EQ(tab.ISetFeature(f1.get()), OGRERR_NON_EXISTING_FEATURE);
    f1->SetFID(999);
    EXPECT_EQ(tab.ISetFeature(f1.get()), OGRERR_NON_EXISTING_FEATURE);
    f1->SetFID(1);
    OGRMultiPoint mp;
    f1->SetGeometry(&mp);
    EXPECT_EQ(tab.ISetFeature(f1.get()), OGRERR_FAILURE);

    // DeleteFeature tests
    EXPECT_EQ(tab.DeleteFeature(2), OGRERR_NONE);
    EXPECT_EQ(tab.DeleteFeature(2), OGRERR_NON_EXISTING_FEATURE);
    EXPECT_EQ(tab.DeleteFeature(999), OGRERR_NON_EXISTING_FEATURE);

    // CreateFeature reusing deleted FID 2
    {
      TABPoint reused_pt(tab.GetLayerDefn());
      reused_pt.SetFID(2);
      reused_pt.SetField(0, "reused_slot");
      reused_pt.SetField(1, 20);
      reused_pt.SetField(2, 2);
      OGRPoint p2(20.0, 20.0);
      reused_pt.SetGeometry(&p2);
      EXPECT_EQ(tab.CreateFeature(&reused_pt), OGRERR_NONE);
    }

    // CreateFeature with non-deleted FID 1 -> failure
    {
      TABPoint bad_f(tab.GetLayerDefn());
      bad_f.SetFID(1);
      EXPECT_EQ(tab.CreateFeature(&bad_f), OGRERR_FAILURE);
      bad_f.SetFID(999);
      EXPECT_EQ(tab.CreateFeature(&bad_f), OGRERR_FAILURE);
    }

    // Reset f1 before modifying schema since modifying OGRFeatureDefn in place
    // would corrupt f1's field types during its destruction.
    f1.reset();

    // Schema mutations in TABReadWrite after features exist
    EXPECT_EQ(tab.AddFieldNative("added_col", TABFInteger, 10, 0, FALSE), 0);

    OGRFieldDefn altered_defn("new_str_col", OFTString);
    altered_defn.SetWidth(30);
    EXPECT_EQ(tab.AlterFieldDefn(0, &altered_defn, ALTER_ALL_FLAG),
              OGRERR_NONE);
    EXPECT_EQ(tab.AlterFieldDefn(99, &altered_defn, ALTER_ALL_FLAG),
              OGRERR_FAILURE);

    int panMap[] = {1, 0, 2, 3};
    EXPECT_EQ(tab.ReorderFields(panMap), OGRERR_NONE);

    EXPECT_EQ(tab.DeleteField(2), OGRERR_NONE);
    EXPECT_EQ(tab.DeleteField(99), OGRERR_FAILURE);
    CPLErrorReset();

    EXPECT_EQ(tab.SyncToDisk(), OGRERR_NONE);
    EXPECT_EQ(tab.Close(), 0);
  }

  // Reopen existing dataset in TABReadWrite
  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(rw_tab.c_str(), TABReadWrite), 0);
    EXPECT_EQ(tab.GetFeatureCount(FALSE), 3);
    EXPECT_EQ(tab.Close(), 0);
  }
}

TEST(TABFileTest, ParseTABFileEdgeCasesDBFAndTableWithoutMAP) {
  // 1. Table without MAP and ID files (attribute-only table)
  const std::string nomap_base = "/vsimem/nomap";
  const std::string nomap_tab = nomap_base + ".tab";
  const std::string nomap_dat = nomap_base + ".dat";
  absl::Cleanup cleanup_nomap = [&]() {
    CleanVsiFiles({nomap_tab, nomap_dat});
  };

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(nomap_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(tab.AddFieldNative("attr", TABFChar, 20), 0);
    EXPECT_EQ(tab.SetBounds(-100, -100, 100, 100), 0);
    TABFeature feat(tab.GetLayerDefn());
    feat.SetField(0, "hello");
    EXPECT_EQ(tab.CreateFeature(&feat), OGRERR_NONE);
    EXPECT_EQ(tab.Close(), 0);
  }

  // Delete the .map and .id files so only .tab and .dat remain
  VSIUnlink((nomap_base + ".map").c_str());
  VSIUnlink((nomap_base + ".id").c_str());

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(nomap_tab.c_str(), TABRead), 0);
    EXPECT_EQ(tab.GetLayerDefn()->GetGeomType(), wkbNone);
    EXPECT_EQ(tab.GetSpatialRef(), nullptr);
    OGREnvelope env;
    EXPECT_EQ(tab.IGetExtent(0, &env, false), OGRERR_FAILURE);

    TABFeature* f = tab.GetFeatureRef(1);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->GetFeatureClass(), TABFCNoGeomFeature);
    EXPECT_EQ(tab.Close(), 0);
  }

  // 2. Type DBF table
  const std::string dbf_base = "/vsimem/dbf_tbl";
  const std::string dbf_tab = dbf_base + ".tab";
  const std::string dbf_dbf = dbf_base + ".dbf";
  const std::string dbf_map = dbf_base + ".map";
  const std::string dbf_id = dbf_base + ".id";
  absl::Cleanup cleanup_dbf = [&]() {
    CleanVsiFiles({dbf_tab, dbf_dbf, dbf_map, dbf_id});
  };

  // Create a base table with TABWrite
  const std::string base_tab = "/vsimem/dbf_src.tab";
  const std::string base_dat = "/vsimem/dbf_src.dat";
  const std::string base_map = "/vsimem/dbf_src.map";
  const std::string base_id = "/vsimem/dbf_src.id";
  absl::Cleanup cleanup_src = [&]() {
    CleanVsiFiles({base_tab, base_dat, base_map, base_id});
  };

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(base_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(tab.AddFieldNative("name", TABFChar, 20), 0);
    EXPECT_EQ(tab.SetBounds(-100, -100, 100, 100), 0);
    TABPoint pt(tab.GetLayerDefn());
    pt.SetField(0, "dbf_test");
    OGRPoint p(10, 20);
    pt.SetGeometry(&p);
    EXPECT_EQ(tab.CreateFeature(&pt), OGRERR_NONE);
    EXPECT_EQ(tab.Close(), 0);
  }

  // Copy dat to dbf, map to map, id to id
  CopyVsiFile(base_dat, dbf_dbf);
  CopyVsiFile(base_map, dbf_map);
  CopyVsiFile(base_id, dbf_id);

  const char* dbf_tab_content =
      "!table\n"
      "!version 300\n"
      "!edit_version 300\n"
      "!charset Neutral\n"
      "\n"
      "Definition Table\n"
      "  Type DBF Charset \"Neutral\"\n"
      "  Fields 1\n"
      "    name Char (20) ;\n";

  VSILFILE* fp = VSIFileFromMemBuffer(
      dbf_tab.c_str(),
      reinterpret_cast<GByte*>(const_cast<char*>(dbf_tab_content)),
      strlen(dbf_tab_content), FALSE);
  VSIFCloseL(fp);

  {
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(dbf_tab.c_str(), TABRead), 0);
    EXPECT_EQ(tab.GetFeatureCount(FALSE), 1);
    TABFeature* f = tab.GetFeatureRef(1);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->GetFeatureClass(), TABFCPoint);
    EXPECT_EQ(tab.Close(), 0);
  }

  // 3. Unsupported table types
  auto TestUnsupportedType = [](const char* type_name) {
    std::string tab_file =
        std::string("/vsimem/unsupported_") + type_name + ".tab";
    absl::Cleanup cleanup_unsupported = [&]() { CleanVsiFiles({tab_file}); };
    std::string content =
        std::string("!table\n!version 300\nDefinition Table\n  Type ") +
        type_name + "\n  Fields 1\n    f1 Char(10);\n";
    VSILFILE* f = VSIFileFromMemBuffer(
        tab_file.c_str(),
        reinterpret_cast<GByte*>(const_cast<char*>(content.c_str())),
        content.size(), FALSE);
    VSIFCloseL(f);

    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(tab_file.c_str(), TABRead, FALSE), -1);
    tab.Close();
    EXPECT_EQ(tab.Open(tab_file.c_str(), TABRead, TRUE), -1);
    tab.Close();
  };

  TestUnsupportedType("ACCESS");
  TestUnsupportedType("WMS");
  TestUnsupportedType("WFS");
  TestUnsupportedType("ASCII");

  // 4. Corrupt table files
  {
    const std::string corrupt_tab = "/vsimem/corrupt_fields.tab";
    absl::Cleanup cleanup_corrupt = [&]() { CleanVsiFiles({corrupt_tab}); };
    const char* corrupt_content =
        "!table\n!version 300\nDefinition Table\n  Type NATIVE\n  Fields 0\n";
    fp = VSIFileFromMemBuffer(
        corrupt_tab.c_str(),
        reinterpret_cast<GByte*>(const_cast<char*>(corrupt_content)),
        strlen(corrupt_content), FALSE);
    VSIFCloseL(fp);

    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(corrupt_tab.c_str(), TABRead, FALSE), -1);
    tab.Close();
    EXPECT_EQ(tab.Open(corrupt_tab.c_str(), TABRead, TRUE), -1);
    tab.Close();
  }
}

TEST(TABFileTest, AdditionalCoverageEdges) {
  // 1. Version 100 table with Description and Neutral charset
  const std::string v100_base = "/vsimem/v100";
  const std::string v100_tab = v100_base + ".tab";
  const std::string v100_dat = v100_base + ".dat";
  absl::Cleanup cleanup_v100 = [&]() { CleanVsiFiles({v100_tab, v100_dat}); };
  {
    TABFile wtab(nullptr);
    EXPECT_EQ(wtab.Open(v100_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(wtab.AddFieldNative("col1", TABFSmallInt, 5), 0);
    EXPECT_EQ(wtab.AddFieldNative("li1", TABFLargeInt, 0), 0);
    EXPECT_EQ(wtab.AddFieldNative("li2", TABFLargeInt, 18), 0);
    EXPECT_EQ(wtab.SetBounds(-10, -10, 10, 10), 0);
    EXPECT_EQ(wtab.SetMetadataItem("DESCRIPTION", "unescaped_desc", ""),
              CE_None);
    EXPECT_EQ(wtab.SetMetadataItem("custom_k", "custom_v", "CUSTOM"), CE_None);
    EXPECT_EQ(wtab.Close(), 0);

    const char* v100_content =
        "!table\n"
        "!version 100\n"
        "!charset Neutral\n"
        "\n"
        "Definition Table\n"
        "  Type NATIVE Charset \"Neutral\"\n"
        "  Description \"unescaped_desc\"\n"
        "  Fields 3\n"
        "    col1 SmallInt (5) ;\n"
        "    li1 LargeInt ;\n"
        "    li2 LargeInt (18) ;\n";
    VSILFILE* fp = VSIFOpenL(v100_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(v100_content, 1, strlen(v100_content), fp);
    VSIFCloseL(fp);

    TABFile rtab(nullptr);
    EXPECT_EQ(rtab.Open(v100_tab.c_str(), TABRead), 0);
    EXPECT_STREQ(rtab.GetMetadataItem("DESCRIPTION", ""), "unescaped_desc");
    EXPECT_EQ(rtab.Close(), 0);
  }

  // 2. Corrupt field definition in TAB file
  const std::string badfield_tab = "/vsimem/badfield.tab";
  absl::Cleanup cleanup_badfield = [&]() { CleanVsiFiles({badfield_tab}); };
  {
    const char* content =
        "!table\n"
        "!version 300\n"
        "!charset Neutral\n"
        "\n"
        "Definition Table\n"
        "  Type NATIVE Charset \"Neutral\"\n"
        "  Fields 1\n"
        "    bad_col UNKNOWN_TYPE ;\n";
    VSILFILE* fp = VSIFOpenL(badfield_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    TABFile tab(nullptr);
    EXPECT_NE(tab.Open(badfield_tab.c_str(), TABRead, FALSE), 0);
    tab.Close();
    EXPECT_NE(tab.Open(badfield_tab.c_str(), TABRead, TRUE), 0);
    tab.Close();
  }

  // 3. Missing DAT file with bTestOpenNoError = TRUE
  const std::string nodat_tab = "/vsimem/nodat.tab";
  absl::Cleanup cleanup_nodat = [&]() { CleanVsiFiles({nodat_tab}); };
  {
    const char* content =
        "!table\n"
        "!version 300\n"
        "!charset Neutral\n"
        "\n"
        "Definition Table\n"
        "  Type NATIVE Charset \"Neutral\"\n"
        "  Fields 1\n"
        "    f1 Integer ;\n";
    VSILFILE* fp = VSIFOpenL(nodat_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(nodat_tab.c_str(), TABRead, TRUE), -1);
    tab.Close();
  }

  // 4. Only polyline features -> geometry type is wkbLineString
  const std::string pline_base = "/vsimem/pline_only";
  const std::string pline_tab = pline_base + ".tab";
  const std::string pline_dat = pline_base + ".dat";
  const std::string pline_map = pline_base + ".map";
  const std::string pline_id = pline_base + ".id";
  absl::Cleanup cleanup_pline = [&]() {
    CleanVsiFiles({pline_tab, pline_dat, pline_map, pline_id});
  };
  {
    TABFile wtab(nullptr);
    EXPECT_EQ(wtab.Open(pline_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(wtab.AddFieldNative("f1", TABFInteger, 10), 0);
    EXPECT_EQ(wtab.SetBounds(-100, -100, 100, 100), 0);
    TABPolyline pl(wtab.GetLayerDefn());
    OGRLineString ls;
    ls.addPoint(0, 0);
    ls.addPoint(10, 10);
    pl.SetGeometry(&ls);
    EXPECT_EQ(wtab.CreateFeature(&pl), OGRERR_NONE);
    EXPECT_EQ(wtab.Close(), 0);

    TABFile rtab(nullptr);
    EXPECT_EQ(rtab.Open(pline_tab.c_str(), TABRead), 0);
    EXPECT_EQ(rtab.GetGeomType(), wkbLineString);
    EXPECT_EQ(rtab.Close(), 0);
  }

  // 5. Invalid geometry CreateFeature & SetSpatialRef(nullptr)
  const std::string invgeom_base = "/vsimem/invgeom";
  const std::string invgeom_tab = invgeom_base + ".tab";
  const std::string invgeom_dat = invgeom_base + ".dat";
  const std::string invgeom_map = invgeom_base + ".map";
  const std::string invgeom_id = invgeom_base + ".id";
  absl::Cleanup cleanup_invgeom = [&]() {
    CleanVsiFiles({invgeom_tab, invgeom_dat, invgeom_map, invgeom_id});
  };
  {
    TABFile wtab(nullptr);
    EXPECT_EQ(wtab.Open(invgeom_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(wtab.AddFieldNative("f1", TABFInteger, 10), 0);
    EXPECT_EQ(wtab.SetBounds(-100, -100, 100, 100), 0);

    EXPECT_EQ(wtab.SetSpatialRef(nullptr), -1);

    TABPoint pt(wtab.GetLayerDefn());
    EXPECT_EQ(wtab.CreateFeature(&pt), OGRERR_FAILURE);

    EXPECT_EQ(wtab.Close(), 0);
  }

  // 6. Invalid field count (Fields 5000)
  const std::string toomany_tab = "/vsimem/toomany.tab";
  absl::Cleanup cleanup_toomany = [&]() { CleanVsiFiles({toomany_tab}); };
  {
    const char* content =
        "!table\n!version 300\nDefinition Table\n  Type NATIVE\n  Fields "
        "5000\n";
    VSILFILE* fp = VSIFOpenL(toomany_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);
    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(toomany_tab.c_str(), TABRead), -1);
    tab.Close();
  }

  // 7. Uppercase DAT and corrupt MAP
  const std::string upper_tab = "/vsimem/UPPER.TAB";
  const std::string upper_dat = "/vsimem/UPPER.DAT";
  const std::string upper_map = "/vsimem/UPPER.MAP";
  absl::Cleanup cleanup_upper = [&]() {
    CleanVsiFiles({upper_tab, upper_dat, upper_map});
  };
  {
    // Write valid DAT file with 1 column and 0 records
    TABDATFile dat("");
    EXPECT_EQ(dat.Open(upper_dat.c_str(), TABWrite), 0);
    EXPECT_EQ(dat.AddField("col1", TABFInteger, 10, 0), 0);
    EXPECT_EQ(dat.Close(), 0);

    const char* content =
        "!table\n!version 300\nDefinition Table\n  Type NATIVE\n  Fields 1\n   "
        " "
        "col1 Integer ;\n";
    VSILFILE* fp = VSIFOpenL(upper_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    // Corrupt MAP file (10 zero bytes)
    char bad_bytes[10] = {0};
    fp = VSIFOpenL(upper_map.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(bad_bytes, 1, sizeof(bad_bytes), fp);
    VSIFCloseL(fp);

    TABFile tab(nullptr);
    EXPECT_EQ(tab.Open(upper_tab.c_str(), TABRead, FALSE), -1);
    tab.Close();
    EXPECT_EQ(tab.Open(upper_tab.c_str(), TABRead, TRUE), -1);
    tab.Close();
  }

#ifdef DEBUG
  {
    TABFile tab(nullptr);
    tab.Dump(nullptr);
  }
#endif
}

}  // namespace
