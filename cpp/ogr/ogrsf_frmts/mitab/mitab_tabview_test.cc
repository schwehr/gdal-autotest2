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

#include <climits>
#include <cstdio>
#include <cstring>
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
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_utils.h"

namespace {

void CleanVsiFiles(const std::vector<std::string>& files) {
  for (const auto& file : files) {
    VSIUnlink(file.c_str());
  }
}

TEST(TABViewTest, UnopenedAndErrorCases) {
  TABView tab(nullptr);

  EXPECT_EQ(tab.GetLayerDefn(), nullptr);
  EXPECT_EQ(tab.GetNativeFieldType(0), TABFUnknown);
  EXPECT_EQ(tab.AddFieldNative("f", TABFInteger), -1);
  EXPECT_EQ(tab.SetFeatureDefn(nullptr), -1);
  EXPECT_EQ(tab.SetFieldIndexed(0), -1);
  EXPECT_FALSE(tab.IsFieldIndexed(0));
  EXPECT_FALSE(tab.IsFieldUnique(0));

  double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
  EXPECT_EQ(tab.GetBounds(xmin, ymin, xmax, ymax), -1);
  OGREnvelope env;
  EXPECT_EQ(tab.IGetExtent(0, &env, false), OGRERR_FAILURE);

  int np = 0, nl = 0, nr = 0, nt = 0;
  EXPECT_EQ(tab.GetFeatureCountByType(np, nl, nr, nt), -1);
  EXPECT_EQ(tab.GetSpatialRef(), nullptr);

  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  EXPECT_EQ(tab.SetSpatialRef(&srs), -1);
  EXPECT_EQ(tab.SetBounds(-180, -90, 180, 90), -1);
  EXPECT_EQ(tab.GetNextFeatureId(-1), -1);
  EXPECT_EQ(tab.GetFeatureRef(1), nullptr);

  auto* poDummyDefn = new OGRFeatureDefn("dummy");
  poDummyDefn->Reference();
  {
    TABPoint pt(poDummyDefn);
    EXPECT_EQ(tab.CreateFeature(&pt), OGRERR_UNSUPPORTED_OPERATION);
  }
  poDummyDefn->Release();
  EXPECT_EQ(tab.SetQuickSpatialIndexMode(TRUE), -1);
  EXPECT_EQ(tab.SetCharset("Neutral"), 0);

  // Capabilities
  EXPECT_TRUE(tab.TestCapability(OLCRandomRead));
  EXPECT_TRUE(tab.TestCapability(OLCSequentialWrite));
  EXPECT_FALSE(tab.TestCapability(OLCRandomWrite));
  EXPECT_TRUE(tab.TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(tab.TestCapability(OLCFastSpatialFilter));
  EXPECT_TRUE(tab.TestCapability(OLCFastGetExtent));
  EXPECT_FALSE(tab.TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(tab.TestCapability("UNKNOWN"));

  // Unsupported access mode
  EXPECT_NE(tab.Open("/vsimem/view.tab", TABReadWrite), 0);

  // Non-existent file
  EXPECT_NE(tab.Open("/vsimem/nonexistent.tab", TABRead, FALSE), 0);
  EXPECT_NE(tab.Open("/vsimem/nonexistent.tab", TABRead, TRUE), 0);

  // Missing "create view" keyword
  const std::string noview_tab = "/vsimem/noview.tab";
  absl::Cleanup cleanup_noview = [&]() { CleanVsiFiles({noview_tab}); };
  {
    const char* content = "!table\n!version 100\n!charset Neutral\n";
    VSILFILE* fp = VSIFOpenL(noview_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    EXPECT_NE(tab.Open(noview_tab.c_str(), TABRead, FALSE), 0);
    tab.Close();
    EXPECT_NE(tab.Open(noview_tab.c_str(), TABRead, TRUE), 0);
    tab.Close();
  }

  // Missing Fields in Select
  const std::string nofields_tab = "/vsimem/nofields.tab";
  absl::Cleanup cleanup_nofields = [&]() { CleanVsiFiles({nofields_tab}); };
  {
    const char* content =
        "!table\n!version 100\n!charset Neutral\n"
        "Open Table \"t1\" Hide\nOpen Table \"t2\" Hide\n"
        "Create View v As\nWhere t1.id=t2.id\n";
    VSILFILE* fp = VSIFOpenL(nofields_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    EXPECT_NE(tab.Open(nofields_tab.c_str(), TABRead, FALSE), 0);
    EXPECT_NE(tab.Open(nofields_tab.c_str(), TABRead, TRUE), 0);
  }

  // Missing Where line
  const std::string nowhere_tab = "/vsimem/nowhere.tab";
  absl::Cleanup cleanup_nowhere = [&]() { CleanVsiFiles({nowhere_tab}); };
  {
    const char* content =
        "!table\n!version 100\n!charset Neutral\n"
        "Open Table \"t1\" Hide\nOpen Table \"t2\" Hide\n"
        "Create View v As\nSelect f1\n";
    VSILFILE* fp = VSIFOpenL(nowhere_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    EXPECT_NE(tab.Open(nowhere_tab.c_str(), TABRead, FALSE), 0);
    EXPECT_NE(tab.Open(nowhere_tab.c_str(), TABRead, TRUE), 0);
  }

  // Invalid Where clause (not 5 tokens)
  const std::string badwhere_tab = "/vsimem/badwhere.tab";
  absl::Cleanup cleanup_badwhere = [&]() { CleanVsiFiles({badwhere_tab}); };
  {
    const char* content =
        "!table\n!version 100\n!charset Neutral\n"
        "Open Table \"t1\" Hide\nOpen Table \"t2\" Hide\n"
        "Create View v As\nSelect f1\nWhere t1.id=t2.id=extra\n";
    VSILFILE* fp = VSIFOpenL(badwhere_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    EXPECT_NE(tab.Open(badwhere_tab.c_str(), TABRead, FALSE), 0);
    EXPECT_NE(tab.Open(badwhere_tab.c_str(), TABRead, TRUE), 0);
  }

  // Wrong number of Open Table entries (e.g. 1 table)
  const std::string onetab_view = "/vsimem/onetab.tab";
  absl::Cleanup cleanup_onetab = [&]() { CleanVsiFiles({onetab_view}); };
  {
    const char* content =
        "!table\n!version 100\n!charset Neutral\n"
        "Open Table \"t1\" Hide\n"
        "Create View v As\nSelect f1\nWhere t1.id=t2.id\n";
    VSILFILE* fp = VSIFOpenL(onetab_view.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    EXPECT_NE(tab.Open(onetab_view.c_str(), TABRead, FALSE), 0);
    EXPECT_NE(tab.Open(onetab_view.c_str(), TABRead, TRUE), 0);
  }

  // Subtable missing
  const std::string missingsub_view = "/vsimem/missingsub.tab";
  absl::Cleanup cleanup_missingsub = [&]() {
    CleanVsiFiles({missingsub_view});
  };
  {
    const char* content =
        "!table\n!version 100\n!charset Neutral\n"
        "Open Table \"nosuch1.tab\" Hide\nOpen Table \"nosuch2.tab\" Hide\n"
        "Create View v As\nSelect f1\nWhere nosuch1.id=nosuch2.id\n";
    VSILFILE* fp = VSIFOpenL(missingsub_view.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    EXPECT_NE(tab.Open(missingsub_view.c_str(), TABRead, FALSE), 0);
    EXPECT_NE(tab.Open(missingsub_view.c_str(), TABRead, TRUE), 0);
  }
}

TEST(TABViewTest, ReadJoinedViewAndFeatures) {
  const std::string base1 = "/vsimem/sub1";
  const std::string sub1_tab = base1 + ".tab";
  const std::string sub1_dat = base1 + ".dat";
  const std::string sub1_map = base1 + ".map";
  const std::string sub1_id = base1 + ".id";
  const std::string sub1_ind = base1 + ".ind";

  const std::string base2 = "/vsimem/sub2";
  const std::string sub2_tab = base2 + ".tab";
  const std::string sub2_dat = base2 + ".dat";
  const std::string sub2_map = base2 + ".map";
  const std::string sub2_id = base2 + ".id";
  const std::string sub2_ind = base2 + ".ind";

  const std::string view_tab = "/vsimem/joined_view.tab";
  const std::string view_wildcard_tab = "/vsimem/wildcard_view.tab";
  const std::string view_char_tab = "/vsimem/char_view.tab";
  const std::string view_dec_tab = "/vsimem/dec_view.tab";
  const std::string view_int_tab = "/vsimem/int_view.tab";

  absl::Cleanup cleanup = [&]() {
    CleanVsiFiles({sub1_tab, sub1_dat, sub1_map, sub1_id, sub1_ind, sub2_tab,
                   sub2_dat, sub2_map, sub2_id, sub2_ind, view_tab,
                   view_wildcard_tab, view_char_tab, view_dec_tab,
                   view_int_tab});
  };

  // 1. Create main sub1 table
  {
    TABFile tab1(nullptr);
    EXPECT_EQ(tab1.Open(sub1_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(tab1.AddFieldNative("id_main", TABFInteger, 10, 0, TRUE), 0);
    EXPECT_EQ(tab1.AddFieldNative("char_k", TABFChar, 10, 0, TRUE), 0);
    EXPECT_EQ(tab1.AddFieldNative("dec_k", TABFDecimal, 8, 2, TRUE), 0);
    EXPECT_EQ(tab1.AddFieldNative("int_k", TABFSmallInt, 5, 0, TRUE), 0);
    EXPECT_EQ(tab1.AddFieldNative("main_val", TABFChar, 20), 0);
    EXPECT_EQ(tab1.SetBounds(-100, -100, 100, 100), 0);

    for (int i = 1; i <= 3; ++i) {
      TABPoint pt(tab1.GetLayerDefn());
      pt.SetField(0, i * 10);
      pt.SetField(1, CPLSPrintf("KEY_%d", i));
      pt.SetField(2, i * 1.25);
      pt.SetField(3, i);
      pt.SetField(4, CPLSPrintf("MainVal_%d", i));
      OGRPoint p(i * 10.0, i * 10.0);
      pt.SetGeometry(&p);
      EXPECT_EQ(tab1.CreateFeature(&pt), OGRERR_NONE);
    }
    EXPECT_EQ(tab1.Close(), 0);
  }

  // 2. Create related sub2 table
  {
    TABFile tab2(nullptr);
    EXPECT_EQ(tab2.Open(sub2_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(tab2.AddFieldNative("id_rel", TABFInteger, 10, 0, TRUE), 0);
    EXPECT_EQ(tab2.AddFieldNative("char_rel", TABFChar, 10, 0, TRUE), 0);
    EXPECT_EQ(tab2.AddFieldNative("dec_rel", TABFDecimal, 8, 2, TRUE), 0);
    EXPECT_EQ(tab2.AddFieldNative("int_rel", TABFSmallInt, 5, 0, TRUE), 0);
    EXPECT_EQ(tab2.AddFieldNative("rel_val", TABFChar, 20), 0);
    EXPECT_EQ(tab2.SetBounds(-100, -100, 100, 100), 0);

    // Write records matching sub1 keys
    for (int i = 1; i <= 3; ++i) {
      TABFeature feat(tab2.GetLayerDefn());
      feat.SetField(0, i * 10);
      feat.SetField(1, CPLSPrintf("KEY_%d", i));
      feat.SetField(2, i * 1.25);
      feat.SetField(3, i);
      feat.SetField(4, CPLSPrintf("RelVal_%d", i));
      EXPECT_EQ(tab2.CreateFeature(&feat), OGRERR_NONE);
    }
    EXPECT_EQ(tab2.Close(), 0);
  }

  // 3. Write join_view.tab
  {
    const char* content =
        "!table\n"
        "!version 100\n"
        "!charset Neutral\n"
        "\n"
        "Open Table \"sub1.tab\" Hide\n"
        "Open Table \"sub2.tab\" Hide\n"
        "\n"
        "Create View joined_view As\n"
        "Select id_main, char_k, main_val, rel_val, non_existent_fld\n"
        "From sub2, sub1\n"
        "Where sub2.id_rel=sub1.id_main\n";
    VSILFILE* fp = VSIFOpenL(view_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);
  }

  // 4. Test reading through TABView
  {
    TABView tab(nullptr);
    EXPECT_EQ(tab.Open(view_tab.c_str(), TABRead), 0);
    // Double open error
    EXPECT_EQ(tab.Open(view_tab.c_str(), TABRead), -1);

    const OGRFeatureDefn* poDefn = tab.GetLayerDefn();
    ASSERT_NE(poDefn, nullptr);
    EXPECT_EQ(poDefn->GetFieldCount(), 4);

    double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    EXPECT_EQ(tab.GetBounds(xmin, ymin, xmax, ymax), 0);
    EXPECT_NEAR(xmin, -100, 1e-3);
    EXPECT_NEAR(xmax, 100, 1e-3);

    OGREnvelope env;
    EXPECT_EQ(tab.GetExtent(&env), OGRERR_NONE);

    int np = 0, nl = 0, nr = 0, nt = 0;
    EXPECT_EQ(tab.GetFeatureCountByType(np, nl, nr, nt), 0);
    EXPECT_EQ(np, 3);

    EXPECT_NE(tab.GetSpatialRef(), nullptr);
    EXPECT_EQ(tab.GetNativeFieldType(0), TABFInteger);
    EXPECT_EQ(tab.GetNativeFieldType(3), TABFChar);
    EXPECT_EQ(tab.GetNativeFieldType(99), TABFUnknown);
    EXPECT_FALSE(tab.IsFieldUnique(0));
    EXPECT_TRUE(tab.IsFieldUnique(3));

    // Sequential traversal
    GIntBig nFid = -1;
    int count = 0;
    while ((nFid = tab.GetNextFeatureId(nFid)) != -1) {
      TABFeature* poFeat = tab.GetFeatureRef(nFid);
      ASSERT_NE(poFeat, nullptr);
      EXPECT_EQ(poFeat->GetFID(), nFid);
      EXPECT_EQ(poFeat->GetFieldAsInteger(0), nFid * 10);
      EXPECT_STREQ(poFeat->GetFieldAsString(3),
                   CPLSPrintf("RelVal_%d", static_cast<int>(nFid)));
      count++;
    }
    EXPECT_EQ(count, 3);

    // Random access
    TABFeature* f2 = tab.GetFeatureRef(2);
    ASSERT_NE(f2, nullptr);
    EXPECT_EQ(f2->GetFieldAsInteger("id_main"), 20);
    EXPECT_STREQ(f2->GetFieldAsString("rel_val"), "RelVal_2");

    EXPECT_EQ(tab.GetFeatureRef(999), nullptr);
    EXPECT_EQ(tab.GetFeatureRef(static_cast<GIntBig>(1) << 35), nullptr);

    // Spatial filter
    tab.SetSpatialFilterRect(5, 5, 15, 15);
    EXPECT_FALSE(tab.TestCapability(OLCFastFeatureCount));
    tab.SetSpatialFilter(nullptr);
    EXPECT_TRUE(tab.TestCapability(OLCFastFeatureCount));

#ifdef DEBUG
    tab.Dump(nullptr);
#endif

    EXPECT_EQ(tab.Close(), 0);
  }

  // 5. Wildcard view Select *
  {
    const char* content =
        "!table\n"
        "!version 100\n"
        "!charset Neutral\n"
        "\n"
        "Open Table \"sub1.tab\" Hide\n"
        "Open Table \"sub2.tab\" Hide\n"
        "\n"
        "Create View wildcard_view As\n"
        "Select *\n"
        "From sub2, sub1\n"
        "Where sub2.id_rel=sub1.id_main\n";
    VSILFILE* fp = VSIFOpenL(view_wildcard_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    TABView tab(nullptr);
    EXPECT_EQ(tab.Open(view_wildcard_tab.c_str(), TABRead), 0);
    EXPECT_GT(tab.GetLayerDefn()->GetFieldCount(), 4);
    EXPECT_NE(tab.GetFeatureRef(1), nullptr);
    EXPECT_EQ(tab.Close(), 0);
  }

  // 6. View joining on TABFChar key
  {
    const char* content =
        "!table\n"
        "!version 100\n"
        "!charset Neutral\n"
        "\n"
        "Open Table \"sub1.tab\" Hide\n"
        "Open Table \"sub2.tab\" Hide\n"
        "\n"
        "Create View char_view As\n"
        "Select id_main, char_k, rel_val\n"
        "From sub2, sub1\n"
        "Where sub2.char_rel=sub1.char_k\n";
    VSILFILE* fp = VSIFOpenL(view_char_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    TABView tab(nullptr);
    EXPECT_EQ(tab.Open(view_char_tab.c_str(), TABRead), 0);
    TABFeature* f = tab.GetFeatureRef(1);
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(f->GetFieldAsString("rel_val"), "RelVal_1");
    EXPECT_EQ(tab.Close(), 0);
  }

  // 7. View joining on TABFDecimal key
  {
    const char* content =
        "!table\n"
        "!version 100\n"
        "!charset Neutral\n"
        "\n"
        "Open Table \"sub1.tab\" Hide\n"
        "Open Table \"sub2.tab\" Hide\n"
        "\n"
        "Create View dec_view As\n"
        "Select id_main, dec_k, rel_val\n"
        "From sub2, sub1\n"
        "Where sub2.dec_rel=sub1.dec_k\n";
    VSILFILE* fp = VSIFOpenL(view_dec_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    TABView tab(nullptr);
    EXPECT_EQ(tab.Open(view_dec_tab.c_str(), TABRead), 0);
    TABFeature* f = tab.GetFeatureRef(2);
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(f->GetFieldAsString("rel_val"), "RelVal_2");
    EXPECT_EQ(tab.Close(), 0);
  }

  // 8. View joining on TABFSmallInt key
  {
    const char* content =
        "!table\n"
        "!version 100\n"
        "!charset Neutral\n"
        "\n"
        "Open Table \"sub1.tab\" Hide\n"
        "Open Table \"sub2.tab\" Hide\n"
        "\n"
        "Create View int_view As\n"
        "Select id_main, int_k, rel_val\n"
        "From sub2, sub1\n"
        "Where sub2.int_rel=sub1.int_k\n";
    VSILFILE* fp = VSIFOpenL(view_int_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    TABView tab(nullptr);
    EXPECT_EQ(tab.Open(view_int_tab.c_str(), TABRead), 0);
    TABFeature* f = tab.GetFeatureRef(3);
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(f->GetFieldAsString("rel_val"), "RelVal_3");
    EXPECT_EQ(tab.Close(), 0);
  }
}

TEST(TABViewTest, OpenForWriteAndRelationOperations) {
  const std::string writeview_tab = "/vsimem/writeview.tab";
  const std::string writeview1_tab = "/vsimem/writeview1.tab";
  const std::string writeview1_dat = "/vsimem/writeview1.dat";
  const std::string writeview1_map = "/vsimem/writeview1.map";
  const std::string writeview1_id = "/vsimem/writeview1.id";
  const std::string writeview2_tab = "/vsimem/writeview2.tab";
  const std::string writeview2_dat = "/vsimem/writeview2.dat";

  absl::Cleanup cleanup = [&]() {
    CleanVsiFiles({writeview_tab, writeview1_tab, writeview1_dat,
                   writeview1_map, writeview1_id, writeview2_tab,
                   writeview2_dat});
  };

  {
    TABView tab(nullptr);
    EXPECT_EQ(tab.Open(writeview_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(tab.SetCharset("Neutral"), 0);
    EXPECT_EQ(tab.SetBounds(-180, -90, 180, 90), 0);

    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    EXPECT_EQ(tab.SetSpatialRef(&srs), 0);

    EXPECT_EQ(tab.SetQuickSpatialIndexMode(TRUE), 0);
    EXPECT_EQ(tab.SetQuickSpatialIndexMode(FALSE), 0);

    // SetFeatureDefn when 0 fields exist
    auto* poEmptyDefn = new OGRFeatureDefn("empty");
    poEmptyDefn->Reference();
    EXPECT_EQ(tab.SetFeatureDefn(poEmptyDefn), 0);
    poEmptyDefn->Dereference();

    // Pre-create a field named "MI_Refnum" to exercise the collision loop
    EXPECT_EQ(tab.AddFieldNative("MI_Refnum", TABFInteger, 10, 0, FALSE, FALSE),
              0);

    // Add main table fields
    EXPECT_EQ(tab.AddFieldNative("m_int", TABFInteger, 10, 0, FALSE, FALSE), 0);
    EXPECT_EQ(tab.AddFieldNative("m_char", TABFChar, 20, 0, FALSE, FALSE), 0);

    // Add unique/related table fields
    EXPECT_EQ(tab.AddFieldNative("u_int", TABFInteger, 10, 0, FALSE, TRUE), 0);
    EXPECT_EQ(tab.AddFieldNative("u_char", TABFChar, 20, 0, FALSE, TRUE), 0);

    // Check inspection methods
    EXPECT_EQ(tab.GetNativeFieldType(0), TABFInteger);
    EXPECT_EQ(tab.GetNativeFieldType(1), TABFInteger);
    EXPECT_EQ(tab.GetNativeFieldType(3), TABFInteger);
    EXPECT_FALSE(tab.IsFieldUnique(0));
    EXPECT_TRUE(tab.IsFieldUnique(3));
    EXPECT_TRUE(tab.IsFieldUnique(4));

    // Index field
    EXPECT_EQ(tab.SetFieldIndexed(1), 0);
    EXPECT_TRUE(tab.IsFieldIndexed(1));
    EXPECT_EQ(tab.SetFieldIndexed(3), 0);
    EXPECT_TRUE(tab.IsFieldIndexed(3));

    EXPECT_EQ(tab.Close(), 0);
  }

  // Reopen the written view in TABRead mode
  {
    TABView tab(nullptr);
    EXPECT_EQ(tab.Open(writeview_tab.c_str(), TABRead), 0);
    EXPECT_EQ(tab.GetLayerDefn()->GetFieldCount(), 5);
    EXPECT_EQ(tab.Close(), 0);
  }

  // Write feature test where only unique fields are added,
  // so m_panMainTableFieldMap[0] == -1 and no leaked feature allocations occur.
  const std::string wf_tab = "/vsimem/wf.tab";
  const std::string wf1_tab = "/vsimem/wf1.tab";
  const std::string wf1_dat = "/vsimem/wf1.dat";
  const std::string wf1_map = "/vsimem/wf1.map";
  const std::string wf1_id = "/vsimem/wf1.id";
  const std::string wf2_tab = "/vsimem/wf2.tab";
  const std::string wf2_dat = "/vsimem/wf2.dat";

  absl::Cleanup cleanup_wf = [&]() {
    CleanVsiFiles(
        {wf_tab, wf1_tab, wf1_dat, wf1_map, wf1_id, wf2_tab, wf2_dat});
  };

  {
    TABView tab(nullptr);
    EXPECT_EQ(tab.Open(wf_tab.c_str(), TABWrite), 0);
    EXPECT_EQ(tab.AddFieldNative("u_int", TABFInteger, 10, 0, FALSE, TRUE), 0);
    TABPoint pt(tab.GetLayerDefn());
    OGRPoint p(10, 20);
    pt.SetGeometry(&p);
    EXPECT_EQ(tab.CreateFeature(&pt), OGRERR_FAILURE);
    EXPECT_EQ(tab.Close(), 0);
  }
}

TEST(TABViewTest, TABRelationDirectUnitTests) {
  TABRelation rel;

  EXPECT_EQ(rel.GetFeature(1), nullptr);
  EXPECT_EQ(rel.GetNativeFieldType(0), TABFUnknown);
  EXPECT_EQ(rel.AddFieldNative("f", TABFInteger), -1);
  EXPECT_FALSE(rel.IsFieldIndexed(0));
  EXPECT_EQ(rel.SetFieldIndexed(0), -1);
  EXPECT_FALSE(rel.IsFieldUnique(0));

  auto* poFeatDefn = new OGRFeatureDefn("feat");
  poFeatDefn->Reference();
  {
    TABFeature feat(poFeatDefn);
    EXPECT_EQ(rel.WriteFeature(&feat, 10), -1);
  }
  poFeatDefn->Release();

  EXPECT_EQ(rel.Init("v", nullptr, nullptr, "f1", "f2", nullptr), -1);

  auto* poDefn = new OGRFeatureDefn("custom");
  poDefn->Reference();
  EXPECT_EQ(rel.SetFeatureDefn(poDefn), 0);
  EXPECT_EQ(rel.GetFeatureDefn(), poDefn);
  poDefn->Dereference();
}

}  // namespace
