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
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_utils.h"

namespace autotest2 {
namespace {

void CleanVsiFiles(const std::vector<std::string>& files) {
  for (const auto& file : files) {
    VSIUnlink(file.c_str());
  }
}

TEST(TABSeamlessTest, UnopenedAndUnsupportedOperations) {
  TABSeamless seamless(nullptr);

  EXPECT_EQ(seamless.GetFileClass(), TABFC_TABSeamless);
  EXPECT_EQ(seamless.GetLayerDefn(), nullptr);
  EXPECT_EQ(seamless.GetNativeFieldType(0), TABFUnknown);
  EXPECT_FALSE(seamless.IsFieldIndexed(0));
  EXPECT_FALSE(seamless.IsFieldUnique(0));

  double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
  EXPECT_EQ(seamless.GetBounds(xmin, ymin, xmax, ymax), -1);
  OGREnvelope env;
  EXPECT_EQ(seamless.IGetExtent(0, &env, false), OGRERR_FAILURE);

  int np = 0, nl = 0, nr = 0, nt = 0;
  EXPECT_EQ(seamless.GetFeatureCountByType(np, nl, nr, nt), -1);
  EXPECT_EQ(seamless.GetFeatureCount(FALSE), -1);
  EXPECT_EQ(seamless.GetFeatureCount(TRUE), 0);
  EXPECT_EQ(seamless.GetSpatialRef(), nullptr);
  EXPECT_EQ(seamless.GetNextFeatureId(-1), -1);
  EXPECT_EQ(seamless.GetFeatureRef(1), nullptr);

  seamless.ResetReading();

  // Unsupported modification operations
  auto* poDefn = new OGRFeatureDefn("dummy");
  poDefn->Reference();
  EXPECT_EQ(seamless.SetFeatureDefn(poDefn), -1);
  EXPECT_EQ(seamless.AddFieldNative("f", TABFInteger), -1);
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  EXPECT_EQ(seamless.SetSpatialRef(&srs), -1);
  EXPECT_EQ(seamless.SetBounds(0, 0, 10, 10), -1);
  EXPECT_EQ(seamless.SetFieldIndexed(0), -1);

  TABPoint pt(poDefn);
  EXPECT_EQ(seamless.CreateFeature(&pt), OGRERR_UNSUPPORTED_OPERATION);
  poDefn->Release();

  // Test capabilities
  EXPECT_TRUE(seamless.TestCapability(OLCRandomRead));
  EXPECT_FALSE(seamless.TestCapability(OLCSequentialWrite));
  EXPECT_FALSE(seamless.TestCapability(OLCRandomWrite));
  EXPECT_FALSE(seamless.TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(seamless.TestCapability(OLCFastSpatialFilter));
  EXPECT_TRUE(seamless.TestCapability(OLCFastGetExtent));
  EXPECT_FALSE(seamless.TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(seamless.TestCapability("UNKNOWN"));

#ifdef DEBUG
  seamless.Dump(nullptr);
#endif
}

TEST(TABSeamlessTest, OpenFailureCases) {
  TABSeamless seamless(nullptr);

  // Access mode != TABRead
  EXPECT_NE(seamless.Open("/vsimem/fake.tab", TABWrite), 0);
  EXPECT_NE(seamless.Open("/vsimem/fake.tab", TABReadWrite), 0);

  // Non-existent file
  EXPECT_NE(seamless.Open("/vsimem/nonexistent.tab", TABRead, FALSE), 0);
  EXPECT_NE(seamless.Open("/vsimem/nonexistent.tab", TABRead, TRUE), 0);

  // Valid .tab file but missing IsSeamless metadata
  const std::string not_seamless_tab = "/vsimem/not_seamless.tab";
  absl::Cleanup cleanup_not_seamless = [&]() {
    CleanVsiFiles({not_seamless_tab});
  };
  {
    const char* content = "!table\n!version 300\n!charset Neutral\n";
    VSILFILE* fp = VSIFOpenL(not_seamless_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    EXPECT_NE(seamless.Open(not_seamless_tab.c_str(), TABRead, FALSE), 0);
    EXPECT_NE(seamless.Open(not_seamless_tab.c_str(), TABRead, TRUE), 0);
  }

  // Seamless metadata present, but index TABFile open fails (missing .dat/.map)
  const std::string bad_index_tab = "/vsimem/bad_index.tab";
  absl::Cleanup cleanup_bad_index = [&]() { CleanVsiFiles({bad_index_tab}); };
  {
    const char* content =
        "!table\n"
        "!version 300\n"
        "!charset Neutral\n"
        "\n"
        "Definition Table\n"
        "  Type NATIVE Charset \"Neutral\"\n"
        "  Fields 1\n"
        "    Table Char (50) ;\n"
        "begin_metadata\n"
        "\"\\IsSeamless\" = \"TRUE\"\n"
        "end_metadata\n";
    VSILFILE* fp = VSIFOpenL(bad_index_tab.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    VSIFWriteL(content, 1, strlen(content), fp);
    VSIFCloseL(fp);

    EXPECT_NE(seamless.Open(bad_index_tab.c_str(), TABRead, FALSE), 0);
    EXPECT_NE(seamless.Open(bad_index_tab.c_str(), TABRead, TRUE), 0);
  }

  // Seamless index table exists, but missing 'Table' field
  const std::string notable_tab = "/vsimem/notable.tab";
  absl::Cleanup cleanup_notable = [&]() {
    CleanVsiFiles({notable_tab, "/vsimem/notable.dat", "/vsimem/notable.map",
                   "/vsimem/notable.id"});
  };
  {
    TABFile idx(nullptr);
    ASSERT_EQ(idx.Open(notable_tab.c_str(), TABWrite), 0);
    ASSERT_EQ(idx.AddFieldNative("Other", TABFChar, 50), 0);
    ASSERT_EQ(idx.Close(), 0);

    VSILFILE* fp = VSIFOpenL(notable_tab.c_str(), "ab");
    ASSERT_NE(fp, nullptr);
    const char* meta =
        "\nbegin_metadata\n\"\\IsSeamless\" = \"TRUE\"\nend_metadata\n";
    VSIFWriteL(meta, 1, strlen(meta), fp);
    VSIFCloseL(fp);

    EXPECT_NE(seamless.Open(notable_tab.c_str(), TABRead, FALSE), 0);
    EXPECT_NE(seamless.Open(notable_tab.c_str(), TABRead, TRUE), 0);
  }

  // Seamless index table exists with 'Table' field, but the referenced base
  // table is missing
  const std::string nobase_tab = "/vsimem/nobase.tab";
  absl::Cleanup cleanup_nobase = [&]() {
    CleanVsiFiles({nobase_tab, "/vsimem/nobase.dat", "/vsimem/nobase.map",
                   "/vsimem/nobase.id"});
  };
  {
    TABFile idx(nullptr);
    ASSERT_EQ(idx.Open(nobase_tab.c_str(), TABWrite), 0);
    ASSERT_EQ(idx.AddFieldNative("Table", TABFChar, 50), 0);
    ASSERT_EQ(idx.SetBounds(0, 0, 100, 100), 0);
    TABPoint pt(idx.GetLayerDefn());
    pt.SetField(0, "nonexistent_tile.tab");
    OGRPoint p(50, 50);
    pt.SetGeometry(&p);
    ASSERT_EQ(idx.CreateFeature(&pt), OGRERR_NONE);
    ASSERT_EQ(idx.Close(), 0);

    VSILFILE* fp = VSIFOpenL(nobase_tab.c_str(), "ab");
    ASSERT_NE(fp, nullptr);
    const char* meta =
        "\nbegin_metadata\n\"\\IsSeamless\" = \"TRUE\"\nend_metadata\n";
    VSIFWriteL(meta, 1, strlen(meta), fp);
    VSIFCloseL(fp);

    EXPECT_NE(seamless.Open(nobase_tab.c_str(), TABRead, FALSE), 0);
    EXPECT_NE(seamless.Open(nobase_tab.c_str(), TABRead, TRUE), 0);
  }
}

TEST(TABSeamlessTest, ValidSeamlessDatasetWorkflow) {
  // Directory layout in /vsimem:
  // /vsimem/seamless_test/index.tab (+ .dat, .map, .id)
  // /vsimem/seamless_test/tile1.tab (+ .dat, .map, .id)
  // /vsimem/seamless_test/sub/tile2.tab (+ .dat, .map, .id)
  const std::string dir = "/vsimem/seamless_test";
  const std::string sub_dir = dir + "/sub";

  const std::string tile1_base = dir + "/tile1";
  const std::string tile2_base = sub_dir + "/tile2";
  const std::string idx_base = dir + "/index";

  const std::vector<std::string> all_files = {
      tile1_base + ".tab", tile1_base + ".dat", tile1_base + ".map",
      tile1_base + ".id",  tile2_base + ".tab", tile2_base + ".dat",
      tile2_base + ".map", tile2_base + ".id",  idx_base + ".tab",
      idx_base + ".dat",   idx_base + ".map",   idx_base + ".id",
  };

  absl::Cleanup cleanup = [&]() {
    CleanVsiFiles(all_files);
    VSIRmdir(sub_dir.c_str());
    VSIRmdir(dir.c_str());
  };

  VSIMkdir(dir.c_str(), 0755);
  VSIMkdir(sub_dir.c_str(), 0755);

  // 1. Create Tile 1
  {
    TABFile tile1(nullptr);
    ASSERT_EQ(tile1.Open((tile1_base + ".tab").c_str(), TABWrite), 0);
    ASSERT_EQ(tile1.AddFieldNative("val", TABFInteger, 10), 0);
    ASSERT_EQ(tile1.AddFieldNative("name", TABFChar, 20), 0);
    ASSERT_EQ(tile1.SetBounds(0, 0, 100, 100), 0);

    for (int i = 1; i <= 2; ++i) {
      TABPoint pt(tile1.GetLayerDefn());
      pt.SetField(0, i * 10);
      pt.SetField(1, CPLSPrintf("T1_%d", i));
      OGRPoint p(i * 10.0, i * 10.0);
      pt.SetGeometry(&p);
      ASSERT_EQ(tile1.CreateFeature(&pt), OGRERR_NONE);
    }
    ASSERT_EQ(tile1.Close(), 0);
  }

  // 2. Create Tile 2 (in sub-directory)
  {
    TABFile tile2(nullptr);
    ASSERT_EQ(tile2.Open((tile2_base + ".tab").c_str(), TABWrite), 0);
    ASSERT_EQ(tile2.AddFieldNative("val", TABFInteger, 10), 0);
    ASSERT_EQ(tile2.AddFieldNative("name", TABFChar, 20), 0);
    ASSERT_EQ(tile2.SetBounds(100, 100, 200, 200), 0);

    for (int i = 1; i <= 2; ++i) {
      TABPoint pt(tile2.GetLayerDefn());
      pt.SetField(0, 100 + i * 10);
      pt.SetField(1, CPLSPrintf("T2_%d", i));
      OGRPoint p(100.0 + i * 10.0, 100.0 + i * 10.0);
      pt.SetGeometry(&p);
      ASSERT_EQ(tile2.CreateFeature(&pt), OGRERR_NONE);
    }
    ASSERT_EQ(tile2.Close(), 0);
  }

  // 3. Create Index TAB file with "Table" field and rectangles for each tile
  {
    TABFile idx(nullptr);
    ASSERT_EQ(idx.Open((idx_base + ".tab").c_str(), TABWrite), 0);
    ASSERT_EQ(idx.AddFieldNative("Table", TABFChar, 60), 0);
    ASSERT_EQ(idx.SetBounds(0, 0, 200, 200), 0);

    // Feature 1: tile1 (relative name "tile1.tab")
    {
      TABRegion reg(idx.GetLayerDefn());
      reg.SetField(0, "tile1.tab");
      OGRPolygon poly;
      OGRLinearRing ring;
      ring.addPoint(0, 0);
      ring.addPoint(100, 0);
      ring.addPoint(100, 100);
      ring.addPoint(0, 100);
      ring.addPoint(0, 0);
      poly.addRing(&ring);
      reg.SetGeometry(&poly);
      ASSERT_EQ(idx.CreateFeature(&reg), OGRERR_NONE);
    }

    // Feature 2: tile2 with Windows-style backslash "sub\\tile2.tab" to test
    // path normalization
    {
      TABRegion reg(idx.GetLayerDefn());
      reg.SetField(0, "sub\\tile2.tab");
      OGRPolygon poly;
      OGRLinearRing ring;
      ring.addPoint(100, 100);
      ring.addPoint(200, 100);
      ring.addPoint(200, 200);
      ring.addPoint(100, 200);
      ring.addPoint(100, 100);
      poly.addRing(&ring);
      reg.SetGeometry(&poly);
      ASSERT_EQ(idx.CreateFeature(&reg), OGRERR_NONE);
    }

    ASSERT_EQ(idx.Close(), 0);

    // Append "\IsSeamless" = "TRUE" metadata to index.tab
    VSILFILE* fp = VSIFOpenL((idx_base + ".tab").c_str(), "ab");
    ASSERT_NE(fp, nullptr);
    const char* meta =
        "\nbegin_metadata\n\"\\IsSeamless\" = \"TRUE\"\nend_metadata\n";
    VSIFWriteL(meta, 1, strlen(meta), fp);
    VSIFCloseL(fp);
  }

  // 4. Test reading through TABSeamless
  {
    TABSeamless seamless(nullptr);
    ASSERT_EQ(seamless.Open((idx_base + ".tab").c_str(), TABRead), 0);

    // Opening already open file should fail
    EXPECT_NE(seamless.Open((idx_base + ".tab").c_str(), TABRead), 0);

    const OGRFeatureDefn* poDefn = seamless.GetLayerDefn();
    ASSERT_NE(poDefn, nullptr);
    EXPECT_EQ(poDefn->GetFieldCount(), 2);
    EXPECT_EQ(seamless.GetNativeFieldType(0), TABFInteger);
    EXPECT_EQ(seamless.GetNativeFieldType(1), TABFChar);
    EXPECT_EQ(seamless.GetNativeFieldType(99), TABFUnknown);
    EXPECT_FALSE(seamless.IsFieldIndexed(0));
    EXPECT_FALSE(seamless.IsFieldUnique(0));

    double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    EXPECT_EQ(seamless.GetBounds(xmin, ymin, xmax, ymax), 0);
    EXPECT_NEAR(xmin, 0, 1e-3);
    EXPECT_NEAR(xmax, 200, 1e-3);

    OGREnvelope env;
    EXPECT_EQ(seamless.IGetExtent(0, &env, false), OGRERR_NONE);
    EXPECT_NEAR(env.MinX, 0, 1e-3);
    EXPECT_NEAR(env.MaxX, 200, 1e-3);

    EXPECT_NE(seamless.GetSpatialRef(), nullptr);
    EXPECT_EQ(seamless.GetFeatureCount(FALSE), -1);
    EXPECT_EQ(seamless.GetFeatureCount(TRUE), 4);
    int np = 0, nl = 0, nr = 0, nt = 0;
    EXPECT_EQ(seamless.GetFeatureCountByType(np, nl, nr, nt), -1);

#ifdef DEBUG
    seamless.Dump(nullptr);
#endif

    // Sequential reading across tiles
    GIntBig nFid = -1;
    int count = 0;
    std::vector<GIntBig> fids;
    while ((nFid = seamless.GetNextFeatureId(nFid)) != -1) {
      fids.push_back(nFid);
      TABFeature* poFeat = seamless.GetFeatureRef(nFid);
      ASSERT_NE(poFeat, nullptr);
      EXPECT_EQ(poFeat->GetFID(), nFid);
      count++;
    }
    EXPECT_EQ(count, 4);
    ASSERT_EQ(fids.size(), 4);

    // Repeated GetFeatureRef on the same ID (cached in m_poCurFeature)
    TABFeature* f_cached = seamless.GetFeatureRef(fids[3]);
    ASSERT_NE(f_cached, nullptr);
    EXPECT_EQ(f_cached->GetFID(), fids[3]);

    // Random access: switch back to Tile 1, then Tile 2
    TABFeature* f_t1 = seamless.GetFeatureRef(fids[0]);
    ASSERT_NE(f_t1, nullptr);
    EXPECT_EQ(f_t1->GetFID(), fids[0]);
    EXPECT_EQ(f_t1->GetFieldAsInteger("val"), 10);
    EXPECT_STREQ(f_t1->GetFieldAsString("name"), "T1_1");

    TABFeature* f_t2 = seamless.GetFeatureRef(fids[2]);
    ASSERT_NE(f_t2, nullptr);
    EXPECT_EQ(f_t2->GetFID(), fids[2]);
    EXPECT_EQ(f_t2->GetFieldAsInteger("val"), 110);
    EXPECT_STREQ(f_t2->GetFieldAsString("name"), "T2_1");

    // Invalid FIDs
    EXPECT_EQ(seamless.GetFeatureRef(-1), nullptr);
    EXPECT_EQ(seamless.GetFeatureRef((static_cast<GIntBig>(2) << 32) + 999),
              nullptr);
    EXPECT_EQ(seamless.GetFeatureRef((static_cast<GIntBig>(1) << 32) + 99),
              nullptr);

    // Test GetNextFeature and ResetReading
    seamless.ResetReading();
    OGRFeature* poSpFeat = seamless.GetNextFeature();
    ASSERT_NE(poSpFeat, nullptr);
    EXPECT_EQ(poSpFeat->GetFieldAsInteger("val"), 10);
    delete poSpFeat;

    EXPECT_EQ(seamless.Close(), 0);
  }
}

}  // namespace
}  // namespace autotest2
