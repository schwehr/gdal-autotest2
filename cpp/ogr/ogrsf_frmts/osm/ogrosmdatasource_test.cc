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
// Tests for the OSM vector driver datasource (ogrosmdatasource.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/osm/ogrosmdatasource.cpp
//   https://gdal.org/drivers/vector/osm.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_osm.py
//
// Google3 build caveats:
//   - Default osmconf.ini discovery relies on GoogleGdalInitSupportFiles()
//     (EMBED_RESOURCE_FILES is not enabled in google3/third_party/gdal/BUILD).
//   - Built with HAVE_EXPAT, HAVE_GEOS, and SQLite OGRSQLiteVFS enabled, and
//     WITHOUT_CPLDEBUG defined.
//   - White-box tests instantiate OGROSMDataSource directly without
//     registering the global OSM driver.

#include <filesystem>  // NOLINT
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/osm/ogr_osm.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

OGROSMLayer* GetOsmLayer(OGROSMDataSource* ds, int idx) {
  const OGRLayer* layer = ds->GetLayer(idx);
  return layer != nullptr
             ? cpl::down_cast<OGROSMLayer*>(const_cast<OGRLayer*>(layer))
             : nullptr;
}

int CountFeaturesInLayer(OGRLayer* layer) {
  layer->ResetReading();
  int count = 0;
  while (true) {
    OGRFeatureUniquePtr f(layer->GetNextFeature());
    if (!f) break;
    count++;
  }
  return count;
}

int CountFeaturesInDataset(OGROSMDataSource* ds) {
  ds->ResetReading();
  int count = 0;
  OGRLayer* belonging_layer = nullptr;
  while (true) {
    OGRFeatureUniquePtr f(
        ds->GetNextFeature(&belonging_layer, nullptr, nullptr, nullptr));
    if (!f) break;
    count++;
  }
  return count;
}

TEST(OgrOsmDataSourceTest, OpenNonExistentFails) {
  WithQuietHandler quiet;
  OGROSMDataSource ds;
  EXPECT_FALSE(ds.Open("/vsimem/does_not_exist_ds.osm", nullptr));
}

TEST(OgrOsmDataSourceTest, OpenNonExistentConfigFileFails) {
  WithQuietHandler quiet;
  const std::string osm_path = "/vsimem/test_bad_conf.osm";
  const std::string xml =
      "<osm version='0.6'><node id='1' lat='1' lon='2'/></osm>";
  VsiMemTempWrapper wrapper(osm_path, xml);

  CPLStringList options;
  options.AddNameValue("CONFIG_FILE", "/vsimem/nonexistent_conf.ini");

  OGROSMDataSource ds;
  EXPECT_FALSE(ds.Open(osm_path.c_str(), options.List()));
}

TEST(OgrOsmDataSourceTest, ParseConfAllOptionsAndTagsFormats) {
  const std::string conf_path = "/vsimem/full_conf.ini";
  const std::string conf_content =
      "# Comment line\n"
      "[unknown_layer]\n"
      "some_key=val\n"
      "[general]\n"
      "closed_ways_are_polygons=area,building,landuse,leisure\n"
      "report_all_tags=yes\n"
      "report_all_nodes=yes\n"
      "report_all_ways=yes\n"
      "attribute_name_laundering=no\n"
      "tags_format=json\n"
      "[points]\n"
      "osm_id=yes\n"
      "osm_version=yes\n"
      "osm_timestamp=yes\n"
      "osm_uid=yes\n"
      "osm_user=yes\n"
      "osm_changeset=yes\n"
      "other_tags=yes\n"
      "all_tags=yes\n"
      "attributes=name,barrier,highway,created_by\n"
      "unsignificant=source,created_by\n"
      "insignificant=fixme\n"
      "ignore=tiger:tlid,note\n"
      "computed_attributes=c_int,c_int64,c_real,c_str,c_dt,c_bad\n"
      "c_int_type=Integer\n"
      "c_int_sql=\"SELECT 10\"\n"
      "c_int64_type=Integer64\n"
      "c_real_type=Real\n"
      "c_str_type=String\n"
      "c_dt_type=DateTime\n"
      "c_bad_type=UnknownType\n"
      "highway_type=String\n"
      "undeclared_type=Integer\n"
      "undeclared_sql=\"SELECT 1\"\n"
      "[lines]\n"
      "osm_id=no\n"
      "osm_version=no\n"
      "osm_timestamp=no\n"
      "osm_uid=no\n"
      "osm_user=no\n"
      "osm_changeset=no\n"
      "other_tags=no\n"
      "all_tags=no\n"
      "[multipolygons]\n"
      "osm_id=yes\n"
      "osm_version=yes\n"
      "osm_timestamp=yes\n"
      "osm_uid=yes\n"
      "osm_user=yes\n"
      "osm_changeset=yes\n"
      "other_tags=yes\n"
      "[multilinestrings]\n"
      "other_tags=yes\n"
      "[other_relations]\n"
      "other_tags=yes\n";
  VsiMemTempWrapper conf_wrapper(conf_path, conf_content);

  const std::string osm_path = "/vsimem/test_full_conf.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <node id='1' lat='10.0' lon='20.0' version='1' changeset='100'\n"
      "        user='alice' uid='1' timestamp='2020-01-01T00:00:00Z'>\n"
      "    <tag k='name' v='Node 1'/>\n"
      "    <tag k='highway' v='bus_stop'/>\n"
      "    <tag k='extra_k' v='extra_v'/>\n"
      "  </node>\n"
      "  <node id='2' lat='10.0' lon='20.1'/>\n"
      "  <node id='3' lat='10.1' lon='20.1'/>\n"
      "  <node id='4' lat='10.1' lon='20.0'/>\n"
      "  <!-- Closed way with metadata (tests CompressWay with psInfo != "
      "nullptr) -->\n"
      "  <way id='10' version='2' changeset='500' uid='10' user='bob' "
      "timestamp='2020-01-01T00:00:00Z'>\n"
      "    <nd ref='1'/><nd ref='2'/><nd ref='3'/><nd ref='4'/><nd ref='1'/>\n"
      "    <tag k='building' v='yes'/>\n"
      "    <tag k='name' v='Building 10'/>\n"
      "  </way>\n"
      "  <!-- Closed way with invalid timestamp string -->\n"
      "  <way id='11' timestamp='invalid-date-string'>\n"
      "    <nd ref='1'/><nd ref='2'/><nd ref='3'/><nd ref='4'/><nd ref='1'/>\n"
      "    <tag k='building' v='yes'/>\n"
      "  </way>\n"
      "  <!-- Open way with highway tag -->\n"
      "  <way id='12'>\n"
      "    <nd ref='1'/><nd ref='2'/>\n"
      "    <tag k='highway' v='primary'/>\n"
      "  </way>\n"
      "  <relation id='100'>\n"
      "    <member type='way' ref='12' role='outer'/>\n"
      "    <tag k='type' v='multipolygon'/>\n"
      "  </relation>\n"
      "</osm>\n";
  VsiMemTempWrapper osm_wrapper(osm_path, xml);

  CPLStringList options;
  options.AddNameValue("CONFIG_FILE", conf_path.c_str());
  options.AddNameValue("TAGS_FORMAT", "HSTORE");

  WithQuietHandler quiet;
  OGROSMDataSource ds;
  ASSERT_TRUE(ds.Open(osm_path.c_str(), options.List()));

  EXPECT_EQ(5, ds.GetLayerCount());
  OGROSMLayer* poPoints = GetOsmLayer(&ds, 0);
  ASSERT_NE(nullptr, poPoints);

  EXPECT_NE(-1, poPoints->GetLayerDefn()->GetFieldIndex("osm_id"));
  EXPECT_NE(-1, poPoints->GetLayerDefn()->GetFieldIndex("osm_version"));
  EXPECT_NE(-1, poPoints->GetLayerDefn()->GetFieldIndex("osm_timestamp"));
  EXPECT_NE(-1, poPoints->GetLayerDefn()->GetFieldIndex("osm_uid"));
  EXPECT_NE(-1, poPoints->GetLayerDefn()->GetFieldIndex("osm_user"));
  EXPECT_NE(-1, poPoints->GetLayerDefn()->GetFieldIndex("osm_changeset"));
  EXPECT_NE(-1, poPoints->GetLayerDefn()->GetFieldIndex("all_tags"));
  EXPECT_NE(-1, poPoints->GetLayerDefn()->GetFieldIndex("c_int"));

  // Check lines layer has no osm_id
  OGROSMLayer* poLines = GetOsmLayer(&ds, 1);
  ASSERT_NE(nullptr, poLines);
  EXPECT_EQ(-1, poLines->GetLayerDefn()->GetFieldIndex("osm_id"));

  // Check multipolygons layer has osm_way_id
  OGROSMLayer* poMultiPoly = GetOsmLayer(&ds, 3);
  ASSERT_NE(nullptr, poMultiPoly);
  EXPECT_NE(-1, poMultiPoly->GetLayerDefn()->GetFieldIndex("osm_way_id"));

  EXPECT_EQ(4, CountFeaturesInLayer(poPoints));
  ds.ResetReading();
  EXPECT_GE(CountFeaturesInLayer(poMultiPoly), 2);
}

TEST(OgrOsmDataSourceTest, TagsFormatInvalidWarning) {
  WithQuietHandler quiet;
  const std::string osm_path = "/vsimem/test_bad_tags_fmt.osm";
  const std::string xml =
      "<osm version='0.6'><node id='1' lat='1' lon='2'/></osm>";
  VsiMemTempWrapper wrapper(osm_path, xml);

  CPLStringList options;
  options.AddNameValue("TAGS_FORMAT", "INVALID_FORMAT_XYZ");

  OGROSMDataSource ds;
  CPLErrorReset();
  EXPECT_EQ(TRUE, ds.Open(osm_path.c_str(), options.List()));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid value for TAGS_FORMAT"));
}

TEST(OgrOsmDataSourceTest, SqliteNodeIndexingMode) {
  // Test USE_CUSTOM_INDEXING=NO (SQLite indexing for nodes)
  const std::string osm_path = "/vsimem/sqlite_indexing.osm";
  std::string xml = "<osm version='0.6'>\n";
  // Add 250 nodes to test batch lookups
  for (int i = 1; i <= 250; ++i) {
    xml.append("  <node id='")
        .append(std::to_string(i))
        .append("' lat='")
        .append(std::to_string(10.0 + i * 0.001))
        .append("' lon='")
        .append(std::to_string(20.0 + i * 0.001))
        .append("'><tag k='name' v='n")
        .append(std::to_string(i))
        .append("'/></node>\n");
  }
  // Way referencing these nodes plus a missing node (9999)
  xml.append("  <way id='300'>\n");
  for (int i = 1; i <= 250; ++i) {
    xml.append("    <nd ref='").append(std::to_string(i)).append("'/>\n");
  }
  xml.append("    <nd ref='9999'/>\n");  // Missing node
  xml.append("    <tag k='highway' v='primary'/>\n");
  xml.append("  </way>\n");
  xml.append("</osm>\n");

  VsiMemTempWrapper wrapper(osm_path, xml);
  CPLStringList options;
  options.AddNameValue("USE_CUSTOM_INDEXING", "NO");

  OGROSMDataSource ds;
  ds.SetDescription(osm_path.c_str());
  ASSERT_TRUE(ds.Open(osm_path.c_str(), options.List()));
  OGROSMLayer* lines = GetOsmLayer(&ds, 1);
  ASSERT_NE(nullptr, lines);
  EXPECT_EQ(1, CountFeaturesInLayer(lines));
}

TEST(OgrOsmDataSourceTest, CustomNodeIndexingUncompressedMultipleBuckets) {
  // Test USE_CUSTOM_INDEXING=YES, COMPRESS_NODES=NO
  // Node IDs spanning multiple sectors (64 nodes per sector) and multiple
  // buckets (65536)
  const std::string osm_path = "/vsimem/custom_uncompressed.osm";
  std::string xml = "<osm version='0.6'>\n";
  // Sector 0 (nodes 1..64)
  for (int i = 1; i <= 65; ++i) {
    xml.append("  <node id='")
        .append(std::to_string(i))
        .append("' lat='")
        .append(std::to_string(1.0 + i * 0.01))
        .append("' lon='")
        .append(std::to_string(2.0 + i * 0.01))
        .append("'><tag k='name' v='p'/></node>\n");
  }
  // Bucket 1 (node ID 70000 > 65536)
  xml.append(
      "  <node id='70000' lat='5.0' lon='6.0'><tag k='name' v='b1'/></node>\n");
  // Way referencing nodes across sectors and buckets
  xml.append(
      "  <way id='100'>\n"
      "    <nd ref='1'/>\n"
      "    <nd ref='65'/>\n"
      "    <nd ref='70000'/>\n"
      "    <tag k='highway' v='secondary'/>\n"
      "  </way>\n");
  xml.append("</osm>\n");

  VsiMemTempWrapper wrapper(osm_path, xml);
  CPLStringList options;
  options.AddNameValue("USE_CUSTOM_INDEXING", "YES");
  options.AddNameValue("COMPRESS_NODES", "NO");

  OGROSMDataSource ds;
  ASSERT_TRUE(ds.Open(osm_path.c_str(), options.List()));
  OGROSMLayer* lines = GetOsmLayer(&ds, 1);
  ASSERT_NE(nullptr, lines);
  EXPECT_EQ(1, CountFeaturesInLayer(lines));
}

TEST(OgrOsmDataSourceTest, CustomNodeIndexingCompressedWithFallback) {
  // Test USE_CUSTOM_INDEXING=YES, COMPRESS_NODES=YES
  // Include 64 consecutive nodes with wildly alternating coords so delta varint
  // exceeds 504 bytes, triggering the uncompressed sector fallback in
  // FlushCurrentSectorCompressedCase!
  const std::string osm_path = "/vsimem/custom_compressed_fallback.osm";
  std::string xml = "<osm version='0.6'>\n";

  // Normal compressed sector (nodes 1..63)
  for (int i = 1; i <= 63; ++i) {
    xml.append("  <node id='")
        .append(std::to_string(i))
        .append("' lat='")
        .append(std::to_string(10.0 + i * 0.0001))
        .append("' lon='")
        .append(std::to_string(20.0 + i * 0.0001))
        .append("'><tag k='name' v='n'/></node>\n");
  }

  // Sector starting at 1024: 64 nodes (1024..1087) with wildly alternating
  // lat/lon
  for (int i = 1024; i < 1024 + 64; ++i) {
    double lat = (i % 2 == 0) ? -89.9999999 : 89.9999999;
    double lon = (i % 2 == 0) ? -179.9999999 : 179.9999999;
    xml.append("  <node id='")
        .append(std::to_string(i))
        .append("' lat='")
        .append(std::to_string(lat))
        .append("' lon='")
        .append(std::to_string(lon))
        .append("'><tag k='name' v='alt'/></node>\n");
  }

  // Way referencing nodes from both sectors
  xml.append(
      "  <way id='200'>\n"
      "    <nd ref='1'/>\n"
      "    <nd ref='2'/>\n"
      "    <nd ref='1024'/>\n"
      "    <nd ref='1025'/>\n"
      "    <tag k='highway' v='motorway'/>\n"
      "  </way>\n");
  xml.append("</osm>\n");

  VsiMemTempWrapper wrapper(osm_path, xml);
  CPLStringList options;
  options.AddNameValue("USE_CUSTOM_INDEXING", "YES");
  options.AddNameValue("COMPRESS_NODES", "YES");

  OGROSMDataSource ds;
  ASSERT_TRUE(ds.Open(osm_path.c_str(), options.List()));
  OGROSMLayer* lines = GetOsmLayer(&ds, 1);
  ASSERT_NE(nullptr, lines);
  EXPECT_EQ(1, CountFeaturesInLayer(lines));
}

TEST(OgrOsmDataSourceTest, NonSequentialNodeIdsHashedLookup) {
  // Test non-increasing node IDs (node 500 then node 100), negative node ID
  // (-5), and collision chain in FindNode (ids with same id % 3145739)
  const std::string osm_path = "/vsimem/non_seq_nodes.osm";
  std::string xml =
      "<osm version='0.6'>\n"
      "  <node id='-5' lat='1' lon='1'><tag k='name' v='neg'/></node>\n"
      "  <node id='500' lat='10' lon='20'><tag k='name' v='n500'/></node>\n"
      "  <node id='100' lat='11' lon='21'><tag k='name' v='n100'/></node>\n"
      "  <node id='1' lat='12' lon='22'><tag k='name' v='n1'/></node>\n"
      "  <node id='3145740' lat='13' lon='23'><tag k='name' "
      "v='n_coll'/></node>\n"  // 1 + 3145739
      "  <way id='300'>\n"
      "    <nd ref='100'/>\n"
      "    <nd ref='500'/>\n"
      "    <nd ref='1'/>\n"
      "    <nd ref='3145740'/>\n"
      "    <tag k='highway' v='residential'/>\n"
      "  </way>\n"
      "</osm>\n";

  VsiMemTempWrapper wrapper(osm_path, xml);
  WithQuietHandler quiet;
  OGROSMDataSource ds;
  ASSERT_TRUE(ds.Open(osm_path.c_str(), nullptr));
  OGROSMLayer* lines = GetOsmLayer(&ds, 1);
  ASSERT_NE(nullptr, lines);
  EXPECT_EQ(1, CountFeaturesInLayer(lines));
}

TEST(OgrOsmDataSourceTest, RelationsMultipolygonBoundaryInnerOuterAndHoles) {
  // Multipolygon relation with:
  // - single closed outer way
  // - multiple chained open outer ways (forward and backward)
  // - inner ring (hole)
  // - standalone closed polygon suppression via polygons_standalone table
  const std::string osm_path = "/vsimem/relations_mp.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <!-- Standalone polygon way -->\n"
      "  <node id='1' lat='1' lon='1'/>\n"
      "  <node id='2' lat='1' lon='10'/>\n"
      "  <node id='3' lat='10' lon='10'/>\n"
      "  <node id='4' lat='10' lon='1'/>\n"
      "  <way id='10'>\n"
      "    <nd ref='1'/><nd ref='2'/><nd ref='3'/><nd ref='4'/><nd ref='1'/>\n"
      "    <tag k='building' v='yes'/>\n"
      "  </way>\n"
      "\n"
      "  <!-- Outer ring formed by 2 open ways, plus 1 inner ring -->\n"
      "  <node id='11' lat='20' lon='20'/>\n"
      "  <node id='12' lat='20' lon='30'/>\n"
      "  <node id='13' lat='30' lon='30'/>\n"
      "  <node id='14' lat='30' lon='20'/>\n"
      "  <way id='21'>\n"
      "    <nd ref='11'/><nd ref='12'/><nd ref='13'/>\n"
      "  </way>\n"
      "  <way id='22'>\n"
      "    <!-- reversed order: 11 to 14 to 13 -->\n"
      "    <nd ref='11'/><nd ref='14'/><nd ref='13'/>\n"
      "  </way>\n"
      "  <!-- Inner ring -->\n"
      "  <node id='15' lat='22' lon='22'/>\n"
      "  <node id='16' lat='22' lon='28'/>\n"
      "  <node id='17' lat='28' lon='28'/>\n"
      "  <node id='18' lat='28' lon='22'/>\n"
      "  <way id='23'>\n"
      "    <nd ref='15'/><nd ref='16'/><nd ref='17'/><nd ref='18'/><nd "
      "ref='15'/>\n"
      "  </way>\n"
      "\n"
      "  <relation id='100'>\n"
      "    <member type='way' ref='21' role='outer'/>\n"
      "    <member type='way' ref='22' role='outer'/>\n"
      "    <member type='way' ref='23' role='inner'/>\n"
      "    <tag k='type' v='multipolygon'/>\n"
      "    <tag k='landuse' v='forest'/>\n"
      "  </relation>\n"
      "\n"
      "  <!-- Standalone polygon way not in any relation (triggers "
      "ProcessPolygonsStandalone) -->\n"
      "  <node id='50' lat='50' lon='50'/>\n"
      "  <node id='51' lat='50' lon='51'/>\n"
      "  <node id='52' lat='51' lon='51'/>\n"
      "  <node id='53' lat='51' lon='50'/>\n"
      "  <way id='500'>\n"
      "    <nd ref='50'/><nd ref='51'/><nd ref='52'/><nd ref='53'/><nd "
      "ref='50'/>\n"
      "    <tag k='building' v='warehouse'/>\n"
      "    <tag k='name' v='Warehouse'/>\n"
      "  </way>\n"
      "\n"
      "  <!-- Ways testing area=no, highway=platform (key=value), area=yes, "
      "and long key -->\n"
      "  <way id='501'>\n"
      "    <nd ref='50'/><nd ref='51'/><nd ref='52'/><nd ref='53'/><nd "
      "ref='50'/>\n"
      "    <tag k='area' v='no'/>\n"
      "  </way>\n"
      "  <way id='502'>\n"
      "    <nd ref='50'/><nd ref='51'/><nd ref='52'/><nd ref='53'/><nd "
      "ref='50'/>\n"
      "    <tag k='area' v='yes'/>\n"
      "  </way>\n"
      "  <way id='503'>\n"
      "    <nd ref='50'/><nd ref='51'/><nd ref='52'/><nd ref='53'/><nd "
      "ref='50'/>\n"
      "    <tag k='this_key_name_is_way_too_long_for_polygon_check' v='val'/>\n"
      "  </way>\n"
      "  <way id='504'>\n"
      "    <nd ref='50'/><nd ref='51'/><nd ref='52'/><nd ref='53'/><nd "
      "ref='50'/>\n"
      "    <tag k='highway' v='platform'/>\n"
      "  </way>\n"
      "\n"
      "  <!-- Relation with single closed outer way and tag inheritance -->\n"
      "  <relation id='101'>\n"
      "    <member type='way' ref='10' role='outer'/>\n"
      "    <tag k='type' v='multipolygon'/>\n"
      "  </relation>\n"
      "</osm>\n";

  VsiMemTempWrapper wrapper(osm_path, xml);
  OGROSMDataSource ds;
  ASSERT_TRUE(ds.Open(osm_path.c_str(), nullptr));

  OGROSMLayer* mp = GetOsmLayer(&ds, 3);
  ASSERT_NE(nullptr, mp);
  int count = CountFeaturesInLayer(mp);
  EXPECT_GE(count, 3);
}

TEST(OgrOsmDataSourceTest, RelationsMultiLineStringAndGeometryCollection) {
  // Relation type=multilinestring (lines) and type=site (other_relations)
  const std::string osm_path = "/vsimem/relations_other.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <node id='1' lat='0.5' lon='0.5'/>\n"
      "  <node id='2' lat='1' lon='1'/>\n"
      "  <node id='3' lat='2' lon='2'/>\n"
      "  <way id='10'>\n"
      "    <nd ref='1'/><nd ref='2'/>\n"
      "    <tag k='highway' v='path'/>\n"
      "  </way>\n"
      "  <way id='20'>\n"
      "    <nd ref='2'/><nd ref='3'/>\n"
      "    <tag k='highway' v='path'/>\n"
      "  </way>\n"
      "  <!-- Area way for site relation -->\n"
      "  <node id='4' lat='3' lon='3'/>\n"
      "  <node id='5' lat='3' lon='4'/>\n"
      "  <node id='6' lat='4' lon='4'/>\n"
      "  <way id='30'>\n"
      "    <nd ref='4'/><nd ref='5'/><nd ref='6'/><nd ref='4'/>\n"
      "    <tag k='building' v='yes'/>\n"
      "  </way>\n"
      "  <relation id='100'>\n"
      "    <member type='way' ref='10' role='route'/>\n"
      "    <member type='way' ref='20' role='route'/>\n"
      "    <tag k='type' v='route'/>\n"
      "    <tag k='route' v='hiking'/>\n"
      "  </relation>\n"
      "  <relation id='200'>\n"
      "    <member type='node' ref='1' role='center'/>\n"
      "    <member type='way' ref='10' role='boundary'/>\n"
      "    <member type='way' ref='30' role='building'/>\n"
      "    <member type='way' ref='10' role='subarea'/>\n"
      "    <tag k='type' v='site'/>\n"
      "    <tag k='name' v='My Site'/>\n"
      "  </relation>\n"
      "</osm>\n";

  VsiMemTempWrapper wrapper(osm_path, xml);
  {
    OGROSMDataSource ds;
    ASSERT_TRUE(ds.Open(osm_path.c_str(), nullptr));
    OGROSMLayer* mls = GetOsmLayer(&ds, 2);
    ASSERT_NE(nullptr, mls);
    EXPECT_EQ(1, CountFeaturesInLayer(mls));
  }

  {
    OGROSMDataSource ds;
    ASSERT_TRUE(ds.Open(osm_path.c_str(), nullptr));
    OGROSMLayer* other = GetOsmLayer(&ds, 4);
    ASSERT_NE(nullptr, other);
    EXPECT_EQ(1, CountFeaturesInLayer(other));
  }
}

TEST(OgrOsmDataSourceTest, ExecuteSqlSpecialCommandsAndDecorators) {
  const std::string osm_path = "/vsimem/exec_sql.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <bounds minlat='1.0' minlon='2.0' maxlat='3.0' maxlon='4.0'/>\n"
      "  <node id='1' lat='1.5' lon='2.5'><tag k='name' v='n1'/></node>\n"
      "  <node id='2' lat='2.0' lon='3.0'><tag k='name' v='n2'/></node>\n"
      "</osm>\n";
  VsiMemTempWrapper wrapper(osm_path, xml);

  OGROSMDataSource ds;
  ds.SetDescription(osm_path.c_str());
  ASSERT_TRUE(ds.Open(osm_path.c_str(), nullptr));

  // 1. GetBytesRead()
  {
    OGRLayer* bytes_layer = ds.ExecuteSQL("GetBytesRead()", nullptr, nullptr);
    ASSERT_NE(nullptr, bytes_layer);
    absl::Cleanup cleaner = [&ds, bytes_layer] {
      ds.ReleaseResultSet(bytes_layer);
    };
    EXPECT_STREQ("SELECT", bytes_layer->GetLayerDefn()->GetName());
    EXPECT_FALSE(bytes_layer->TestCapability("RandomCap"));
    OGRFeatureUniquePtr feat1(bytes_layer->GetNextFeature());
    ASSERT_NE(nullptr, feat1);
    EXPECT_GT(feat1->GetFieldAsString(0)[0], '\0');
    EXPECT_EQ(nullptr, bytes_layer->GetNextFeature());
    bytes_layer->ResetReading();
    OGRFeatureUniquePtr feat2(bytes_layer->GetNextFeature());
    ASSERT_NE(nullptr, feat2);
  }

  // 2. SHOW config_file_path
  {
    OGRLayer* conf_layer =
        ds.ExecuteSQL("SHOW config_file_path", nullptr, nullptr);
    ASSERT_NE(nullptr, conf_layer);
    absl::Cleanup cleaner = [&ds, conf_layer] {
      ds.ReleaseResultSet(conf_layer);
    };
    OGRFeatureUniquePtr feat_conf(conf_layer->GetNextFeature());
    ASSERT_NE(nullptr, feat_conf);
    EXPECT_THAT(feat_conf->GetFieldAsString(0), HasSubstr("osmconf.ini"));
  }

  // 3. SET interest_layers = points
  OGRLayer* set_pts =
      ds.ExecuteSQL("SET interest_layers = points", nullptr, nullptr);
  EXPECT_EQ(nullptr, set_pts);
  EXPECT_TRUE(GetOsmLayer(&ds, 0)->IsUserInterested());
  EXPECT_FALSE(GetOsmLayer(&ds, 1)->IsUserInterested());

  // 4. SET interest_layers = lines
  OGRLayer* set_lines =
      ds.ExecuteSQL("SET interest_layers = lines", nullptr, nullptr);
  EXPECT_EQ(nullptr, set_lines);
  EXPECT_FALSE(GetOsmLayer(&ds, 0)->IsUserInterested());
  EXPECT_TRUE(GetOsmLayer(&ds, 1)->IsUserInterested());

  // 5. SELECT * FROM points (OGRSQL)
  {
    OGRLayer* select_pts =
        ds.ExecuteSQL("SELECT * FROM points", nullptr, nullptr);
    ASSERT_NE(nullptr, select_pts);
    absl::Cleanup cleaner = [&ds, select_pts] {
      ds.ReleaseResultSet(select_pts);
    };
    // Attempting second query without ReleaseResultSet fails
    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr,
                ds.ExecuteSQL("SELECT * FROM points", nullptr, nullptr));
    }
    EXPECT_EQ(2, CountFeaturesInLayer(select_pts));
  }

  // 6. SELECT COUNT(*) FROM points (SQLite dialect with
  // OGROSMResultLayerDecorator)
  // Drain the process-global oListDSToBeOpened entry queued by
  // OGROSMResultLayerDecorator::GetFeatureCount() even on early ASSERT_* exit
  // (also covers GetInterestLayersForDSName).
  absl::Cleanup drain_list = [&osm_path] {
    OGROSMDataSource ds_drain;
    ds_drain.SetDescription(osm_path.c_str());
    EXPECT_EQ(TRUE, ds_drain.Open(osm_path.c_str(), nullptr));
  };
  {
    OGRLayer* count_pts =
        ds.ExecuteSQL("SELECT COUNT(*) FROM points", nullptr, "SQLITE");
    ASSERT_NE(nullptr, count_pts);
    absl::Cleanup cleaner = [&ds, count_pts] {
      ds.ReleaseResultSet(count_pts);
    };
    EXPECT_EQ(1, count_pts->GetFeatureCount(TRUE));
  }
}

TEST(OgrOsmDataSourceTest, DatasetGetNextFeatureInterleavedAndCancellation) {
  const std::string osm_path = "/vsimem/interleaved.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <node id='1' lat='1' lon='1'><tag k='name' v='n1'/></node>\n"
      "  <node id='2' lat='2' lon='2'><tag k='name' v='n2'/></node>\n"
      "  <way id='10'><nd ref='1'/><nd ref='2'/><tag k='highway' "
      "v='track'/></way>\n"
      "</osm>\n";
  VsiMemTempWrapper wrapper(osm_path, xml);

  CPLStringList options;
  options.AddNameValue("INTERLEAVED_READING", "YES");

  OGROSMDataSource ds;
  ASSERT_TRUE(ds.Open(osm_path.c_str(), options.List()));
  EXPECT_EQ(1, ds.IsInterleavedReading());

  // Count using dataset GetNextFeature
  int total = CountFeaturesInDataset(&ds);
  EXPECT_EQ(3, total);

  // Test progress function cancellation
  ds.ResetReading();
  auto CancelProgress = [](double /* dfComplete */,
                           const char* /* pszMessage */,
                           void* /* pData */) -> int {
    return FALSE;  // cancel
  };
  OGRLayer* lyr = nullptr;
  OGRFeatureUniquePtr f(
      ds.GetNextFeature(&lyr, nullptr, CancelProgress, nullptr));
  EXPECT_EQ(nullptr, f);
  EXPECT_EQ(nullptr, lyr);
}

TEST(OgrOsmDataSourceTest, CapabilitiesExtentAndLayerLookup) {
  const std::string osm_path = "/vsimem/caps_extent.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <bounds minlat='10.0' minlon='20.0' maxlat='30.0' maxlon='40.0'/>\n"
      "  <node id='1' lat='15' lon='25'><tag k='name' v='pt'/></node>\n"
      "</osm>\n";
  VsiMemTempWrapper wrapper(osm_path, xml);

  OGROSMDataSource ds;
  ASSERT_TRUE(ds.Open(osm_path.c_str(), nullptr));

  EXPECT_TRUE(ds.TestCapability(ODsCRandomLayerRead));
  EXPECT_FALSE(ds.TestCapability("UnknownCapability"));

  EXPECT_EQ(nullptr, ds.GetLayer(-1));
  EXPECT_EQ(nullptr, ds.GetLayer(10));
  EXPECT_NE(nullptr, ds.GetLayer(0));

  OGREnvelope env;
  EXPECT_EQ(OGRERR_NONE, ds.GetNativeExtent(&env));
  EXPECT_DOUBLE_EQ(20.0, env.MinX);
  EXPECT_DOUBLE_EQ(10.0, env.MinY);
  EXPECT_DOUBLE_EQ(40.0, env.MaxX);
  EXPECT_DOUBLE_EQ(30.0, env.MaxY);
}

TEST(OgrOsmDataSourceTest, MaxTmpfileSizeOptionAndSpillover) {
  // Test invalid negative MAX_TMPFILE_SIZE option
  const std::string osm_path = "/vsimem/tmpfile_size.osm";
  const std::string xml =
      "<osm version='0.6'><node id='1' lat='1' lon='1'><tag k='name' "
      "v='n'/></node></osm>";
  VsiMemTempWrapper wrapper(osm_path, xml);

  CPLStringList options;
  options.AddNameValue("MAX_TMPFILE_SIZE", "-10");

  WithQuietHandler quiet;
  OGROSMDataSource ds;
  CPLErrorReset();
  EXPECT_EQ(TRUE, ds.Open(osm_path.c_str(), options.List()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Invalid value for OSM_MAX_TMPFILE_SIZE"));
}

TEST(OgrOsmDataSourceTest, HashCollisionsAndBinarySearchFindNode) {
  // 1. Hash collision chain test:
  // HASH_ID_FUNC is identity, HASHED_INDEXES_ARRAY_SIZE is 3145739.
  // Nodes 1, 3145740 (1+M), 6291479 (1+2M), 9437218 (1+3M) all hash to 1.
  // Node 2 has hash 2.
  const std::string osm_path = "/vsimem/hash_collisions.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <node id='1' lat='1' lon='1'/>\n"
      "  <node id='2' lat='2' lon='2'/>\n"
      "  <node id='3145740' lat='3' lon='3'/>\n"
      "  <node id='6291479' lat='4' lon='4'/>\n"
      "  <node id='9437218' lat='5' lon='5'/>\n"
      "  <!-- Way referencing colliding nodes, missing ID 12582957 (after "
      "chain) -->\n"
      "  <!-- and missing ID 3145741 (single slot 2 mismatch) -->\n"
      "  <way id='100'>\n"
      "    <nd ref='1'/><nd ref='3145740'/><nd ref='6291479'/><nd "
      "ref='9437218'/>\n"
      "    <nd ref='12582957'/><nd ref='3145741'/>\n"
      "    <tag k='highway' v='primary'/>\n"
      "  </way>\n"
      "</osm>\n";
  VsiMemTempWrapper wrapper(osm_path, xml);

  OGROSMDataSource ds;
  ASSERT_TRUE(ds.Open(osm_path.c_str(), nullptr));
  OGROSMLayer* lines = GetOsmLayer(&ds, 1);
  ASSERT_NE(nullptr, lines);
  EXPECT_EQ(1, CountFeaturesInLayer(lines));

  // 2. Binary search and consecutive node optimization:
  // Node 10 exists, nodes 11 and 9 do not exist.
  // Way has 3 refs: [10, 11, 9].
  // Since only 1 node is found in the way, m_asLonLatCache.size() < 2,
  // discarding it (exercises lines 1582-1599, 1657-1677, and 1686-1693).
  const std::string osm_bin = "/vsimem/binary_find_node.osm";
  const std::string xml_bin =
      "<osm version='0.6'>\n"
      "  <node id='10' lat='1' lon='1'/>\n"
      "  <way id='200'>\n"
      "    <nd ref='10'/><nd ref='11'/><nd ref='9'/>\n"
      "    <tag k='highway' v='track'/>\n"
      "  </way>\n"
      "</osm>\n";
  VsiMemTempWrapper wrapper_bin(osm_bin, xml_bin);

  OGROSMDataSource ds_bin;
  ASSERT_TRUE(ds_bin.Open(osm_bin.c_str(), nullptr));
  OGROSMLayer* lines_bin = GetOsmLayer(&ds_bin, 1);
  ASSERT_NE(nullptr, lines_bin);
  EXPECT_EQ(0, CountFeaturesInLayer(lines_bin));

  // 3. Error branches in IndexPointCustom:
  // a) Non-increasing node IDs:
  {
    WithQuietHandler quiet;
    const std::string osm_bad = "/vsimem/non_increasing_ids.osm";
    const std::string xml_bad =
        "<osm version='0.6'><node id='50' lat='1' lon='1'/><node id='10' "
        "lat='2' lon='2'/></osm>";
    VsiMemTempWrapper wrapper_bad(osm_bad, xml_bad);
    OGROSMDataSource ds_bad;
    ASSERT_TRUE(ds_bad.Open(osm_bad.c_str(), nullptr));
    OGROSMLayer* pts = GetOsmLayer(&ds_bad, 0);
    ASSERT_NE(nullptr, pts);
    CPLErrorReset();
    EXPECT_EQ(0, CountFeaturesInLayer(pts));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Non increasing node id"));
  }
  // b) Negative node ID:
  {
    WithQuietHandler quiet;
    const std::string osm_neg = "/vsimem/negative_id.osm";
    const std::string xml_neg =
        "<osm version='0.6'><node id='-5' lat='1' lon='1'/></osm>";
    VsiMemTempWrapper wrapper_neg(osm_neg, xml_neg);
    OGROSMDataSource ds_neg;
    ASSERT_TRUE(ds_neg.Open(osm_neg.c_str(), nullptr));
    OGROSMLayer* pts = GetOsmLayer(&ds_neg, 0);
    ASSERT_NE(nullptr, pts);
    CPLErrorReset();
    EXPECT_EQ(0, CountFeaturesInLayer(pts));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unsupported node id value"));
  }
}

TEST(OgrOsmDataSourceTest, ConfigOptionsAndFilters) {
  // Test OSM_SQLITE_CACHE config option (exercises lines 3129-3173)
  CPLConfigOptionSetter cache_setter("OSM_SQLITE_CACHE", "2",
                                     /*bSetOnlyIfUndefined=*/false);

  const std::string osm_path = "/vsimem/options_and_filters.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <node id='1' lat='1.0' lon='1.0'><tag k='name' v='Inside'/></node>\n"
      "  <node id='2' lat='50.0' lon='50.0'><tag k='name' "
      "v='Outside'/></node>\n"
      "  <node id='3' lat='1.0' lon='2.0'/>\n"
      "  <node id='4' lat='2.0' lon='2.0'/>\n"
      "  <node id='5' lat='2.0' lon='1.0'/>\n"
      "  <way id='10'>\n"
      "    <nd ref='1'/><nd ref='3'/>\n"
      "    <tag k='highway' v='primary'/>\n"
      "    <tag k='name' v='Line1'/>\n"
      "  </way>\n"
      "  <way id='20'>\n"
      "    <nd ref='1'/><nd ref='3'/><nd ref='4'/><nd ref='5'/><nd ref='1'/>\n"
      "    <tag k='building' v='yes'/>\n"
      "    <tag k='landuse' v='forest'/>\n"
      "  </way>\n"
      "  <relation id='100'>\n"
      "    <member type='way' ref='20' role='outer'/>\n"
      "    <tag k='type' v='multipolygon'/>\n"
      "    <tag k='landuse' v='forest'/>\n"
      "  </relation>\n"
      "  <relation id='101'>\n"
      "    <member type='way' ref='20' role='outer'/>\n"
      "    <tag k='type' v='multipolygon'/>\n"
      "    <tag k='landuse' v='farmland'/>\n"
      "  </relation>\n"
      "</osm>\n";
  VsiMemTempWrapper wrapper(osm_path, xml);

  CPLStringList options;
  options.AddNameValue("COMPRESS_NODES", "YES");

  OGROSMDataSource ds;
  ASSERT_TRUE(ds.Open(osm_path.c_str(), options.List()));

  // 1. Spatial filter on points: excludes node 2
  OGROSMLayer* pts = GetOsmLayer(&ds, 0);
  ASSERT_NE(nullptr, pts);
  pts->SetSpatialFilterRect(0.0, 0.0, 10.0, 10.0);
  EXPECT_EQ(1, CountFeaturesInLayer(pts));

  // 2. GetNativeExtent on dataset without bounds returns failure
  OGREnvelope env;
  EXPECT_EQ(OGRERR_FAILURE, ds.GetNativeExtent(&env));

  // 3. Early attribute filter evaluation on multipolygons (matches 1, filters
  // 1)
  ds.ResetReading();
  OGROSMLayer* mp = GetOsmLayer(&ds, 3);
  ASSERT_NE(nullptr, mp);
  ASSERT_EQ(OGRERR_NONE, mp->SetAttributeFilter("landuse = 'forest'"));
  EXPECT_EQ(1, CountFeaturesInLayer(mp));

  // 4. Test lines attribute filter with SET interest_layers = lines
  ds.ResetReading();
  OGRLayer* res =
      ds.ExecuteSQL("SET interest_layers = lines", nullptr, nullptr);
  EXPECT_EQ(nullptr, res);
  OGROSMLayer* lines = GetOsmLayer(&ds, 1);
  ASSERT_NE(nullptr, lines);
  ASSERT_EQ(OGRERR_NONE, lines->SetAttributeFilter("name = 'Line1'"));
  EXPECT_EQ(1, CountFeaturesInLayer(lines));

  // 5. Test multi-table query in ExecuteSQL OGRSQL preparse loop
  pts->SetSpatialFilter(nullptr);
  ASSERT_EQ(OGRERR_NONE, mp->SetAttributeFilter(nullptr));
  ASSERT_EQ(OGRERR_NONE, lines->SetAttributeFilter(nullptr));
  ds.ResetReading();
  OGRLayer* multi_table = ds.ExecuteSQL(
      "SELECT osm_id, name FROM points UNION ALL SELECT osm_id, name FROM "
      "lines",
      nullptr, nullptr);
  ASSERT_NE(nullptr, multi_table);
  absl::Cleanup release_multi = [&ds, multi_table] {
    ds.ReleaseResultSet(multi_table);
  };
  EXPECT_EQ(3, CountFeaturesInLayer(multi_table));

  // 6. Test MyResetReading with COMPRESS_NODES=YES
  ds.ResetReading();
}

TEST(OgrOsmDataSourceTest, WayTagsExceedingIndexedKeysAndValuesLimits) {
  // Test exceeding MAX_INDEXED_VALUES_PER_KEY and MAX_INDEXED_KEYS,
  // plus clamping MAX_COUNT_FOR_TAGS_IN_WAY (255)
  CPLConfigOptionSetter keys_setter("OSM_MAX_INDEXED_KEYS", "2",
                                    /*bSetOnlyIfUndefined=*/false);
  CPLConfigOptionSetter vals_setter("OSM_MAX_INDEXED_VALUES_PER_KEY", "2",
                                    /*bSetOnlyIfUndefined=*/false);

  const std::string osm_path = "/vsimem/many_keys_values.osm";
  std::string xml =
      "<osm version='0.6'>\n"
      "  <node id='1' lat='1' lon='1'/>\n"
      "  <node id='2' lat='1' lon='2'/>\n"
      "  <node id='3' lat='2' lon='2'/>\n"
      "  <node id='4' lat='2' lon='1'/>\n";

  // Way 101: tests exceeding MAX_INDEXED_VALUES_PER_KEY (2) and
  // MAX_INDEXED_KEYS (2)
  xml.append(
      "  <way id='101'>\n"
      "    <nd ref='1'/><nd ref='2'/><nd ref='3'/><nd ref='4'/><nd ref='1'/>\n"
      "    <tag k='building' v='yes'/>\n"
      "    <tag k='shared_key' v='v1'/>\n"
      "    <tag k='shared_key' v='v2'/>\n"
      "    <tag k='shared_key' v='v3'/>\n"
      "    <tag k='shared_key' v='v4'/>\n"
      "    <tag k='k_extra_1' v='val1'/>\n"
      "    <tag k='k_extra_2' v='val2'/>\n"
      "  </way>\n");

  // Way 102: tests clamping when tags exceed MAX_COUNT_FOR_TAGS_IN_WAY (255)
  xml.append(
      "  <way id='102'>\n"
      "    <nd ref='1'/><nd ref='2'/><nd ref='3'/><nd ref='4'/><nd ref='1'/>\n"
      "    <tag k='building' v='yes'/>\n");
  for (int i = 1; i <= 260; ++i) {
    xml.append("    <tag k='t_")
        .append(std::to_string(i))
        .append("' v='v'/>\n");
  }
  xml.append(
      "  </way>\n"
      "</osm>\n");

  VsiMemTempWrapper wrapper(osm_path, xml);
  WithQuietHandler quiet;
  OGROSMDataSource ds;
  ASSERT_TRUE(ds.Open(osm_path.c_str(), nullptr));

  OGROSMLayer* mp = GetOsmLayer(&ds, 3);
  ASSERT_NE(nullptr, mp);
  EXPECT_EQ(2, CountFeaturesInLayer(mp));
}

TEST(OgrOsmDataSourceTest, InMemoryNodesTransferToDiskOnSpillover) {
  // Test TransferToDiskIfNecesserary for m_bInMemoryNodesFile
  // When MAX_TMPFILE_SIZE=1 and USE_CUSTOM_INDEXING=YES,
  // 3 * 1 / 4 = 0 MB limit, so exceeding 1 MB in nodes file triggers transfer.
  // 2055 sectors * 512 bytes = 1,052,160 bytes > 1 MB.
  const std::string osm_path = "/vsimem/spillover_nodes.osm";
  std::string xml = "<osm version='0.6'>\n";
  for (int i = 0; i < 2055; ++i) {
    GIntBig id = 1 + static_cast<GIntBig>(i) * 64;
    xml.append("  <node id='")
        .append(std::to_string(id))
        .append("' lat='10.0' lon='20.0'/>\n");
  }
  // Way referencing two nodes
  xml.append(
      "  <way id='99999'>\n"
      "    <nd ref='1'/><nd ref='131457'/>\n"
      "    <tag k='highway' v='motorway'/>\n"
      "  </way>\n");
  // Append XML comments exceeding XML_BUFSIZE (65536) so OSM_ProcessBlock
  // returns OSM_OK when parsing nodes, allowing TransferToDiskIfNecesserary to
  // execute!
  xml.append("  <!-- ");
  xml.append(std::string(70000, 'x'));
  xml.append(" -->\n");
  xml.append(
      "  <node id='9999999' lat='1' lon='1'><tag k='name' v='end'/></node>\n");
  xml.append("</osm>\n");

  VsiMemTempWrapper wrapper(osm_path, xml);

  CPLStringList options;
  options.AddNameValue("MAX_TMPFILE_SIZE", "1");
  options.AddNameValue("USE_CUSTOM_INDEXING", "YES");
  options.AddNameValue("COMPRESS_NODES", "NO");

  // 1. Copy failure branch when CPL_TMPDIR cannot be written
  {
    const std::string missing_dir =
        (std::filesystem::path(::testing::TempDir()) /
         "missing_osm_nodes_spill" / "sub")
            .string();
    WithQuietHandler quiet;
    CPLConfigOptionSetter tmp_setter("CPL_TMPDIR", missing_dir.c_str(),
                                     /*bSetOnlyIfUndefined=*/false);
    OGROSMDataSource ds_fail;
    ASSERT_TRUE(ds_fail.Open(osm_path.c_str(), options.List()));
    OGROSMLayer* lines = GetOsmLayer(&ds_fail, 1);
    ASSERT_NE(nullptr, lines);
    CPLErrorReset();
    EXPECT_EQ(0, CountFeaturesInLayer(lines));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot copy"));
  }

  // 2. Successful spillover branch to dedicated /vsimem dir
  {
    ASSERT_EQ(0, VSIMkdir("/vsimem/ogrosmdatasource_test_nodes_spill", 0755));
    absl::Cleanup vsimem_cleanup = [] {
      VSIRmdirRecursive("/vsimem/ogrosmdatasource_test_nodes_spill");
    };
    CPLConfigOptionSetter tmp_setter(
        "CPL_TMPDIR", "/vsimem/ogrosmdatasource_test_nodes_spill",
        /*bSetOnlyIfUndefined=*/false);
    OGROSMDataSource ds;
    ASSERT_TRUE(ds.Open(osm_path.c_str(), options.List()));
    OGROSMLayer* lines = GetOsmLayer(&ds, 1);
    ASSERT_NE(nullptr, lines);
    EXPECT_EQ(1, CountFeaturesInLayer(lines));
  }
}

TEST(OgrOsmDataSourceTest, InMemorySqliteDbTransferToDiskOnSpillover) {
  // Test TransferToDiskIfNecesserary for m_bInMemoryTmpDB (lines 4150-4204).
  // When MAX_TMPFILE_SIZE=1, USE_CUSTOM_INDEXING=YES, and
  // m_bInMemoryNodesFile=true, nLimitMB = 1 * 1 / 4 = 0 MB, so exceeding 1 MB
  // in sqlite DB triggers transfer.
  const std::string osm_path = "/vsimem/spillover_db.osm";
  std::string xml = "<osm version='0.6'>\n";
  for (int i = 1; i <= 20; ++i) {
    xml.append("  <node id='")
        .append(std::to_string(i))
        .append("' lat='")
        .append(std::to_string(i))
        .append("' lon='")
        .append(std::to_string(i))
        .append("'/>\n");
  }

  // Create 1600 closed ways each with large unindexed tags so SQLite DB exceeds
  // 1 MB
  const std::string large_val(300, 'y');
  for (int w = 1; w <= 1600; ++w) {
    xml.append("  <way id='")
        .append(std::to_string(100 + w))
        .append(
            "'>\n"
            "    <nd ref='1'/><nd ref='2'/><nd ref='3'/><nd ref='1'/>\n"
            "    <tag k='building' v='yes'/>\n");
    for (int t = 1; t <= 5; ++t) {
      xml.append("    <tag k='k_")
          .append(std::to_string(w))
          .append("_")
          .append(std::to_string(t))
          .append("' v='")
          .append(large_val)
          .append("'/>\n");
    }
    xml.append("  </way>\n");
  }

  // Relation flushes ProcessWaysBatch and indexes ways into SQLite DB
  xml.append(
      "  <relation id='99999'>\n"
      "    <member type='way' ref='101' role='outer'/>\n"
      "    <tag k='type' v='multipolygon'/>\n"
      "  </relation>\n");

  // Comment exceeding XML_BUFSIZE (65536) so OSM_ProcessBlock returns OSM_OK
  xml.append("  <!-- ");
  xml.append(std::string(70000, 'z'));
  xml.append(" -->\n");
  xml.append(
      "  <node id='9999999' lat='1' lon='1'><tag k='name' v='end'/></node>\n");
  xml.append("</osm>\n");

  VsiMemTempWrapper wrapper(osm_path, xml);

  CPLStringList options;
  options.AddNameValue("MAX_TMPFILE_SIZE", "1");
  options.AddNameValue("USE_CUSTOM_INDEXING", "YES");

  CPLConfigOptionSetter keys_setter("OSM_MAX_INDEXED_KEYS", "1",
                                    /*bSetOnlyIfUndefined=*/false);
  CPLConfigOptionSetter cache_setter("OSM_SQLITE_CACHE", "1",
                                     /*bSetOnlyIfUndefined=*/false);

  // 1. Copy failure branch when CPL_TMPDIR cannot be written (lines 4163-4167)
  {
    const std::string missing_dir =
        (std::filesystem::path(::testing::TempDir()) / "missing_osm_db_spill" /
         "sub")
            .string();
    WithQuietHandler quiet;
    CPLConfigOptionSetter tmp_setter("CPL_TMPDIR", missing_dir.c_str(),
                                     /*bSetOnlyIfUndefined=*/false);
    OGROSMDataSource ds_fail;
    ASSERT_TRUE(ds_fail.Open(osm_path.c_str(), options.List()));
    OGROSMLayer* mp = GetOsmLayer(&ds_fail, 3);
    ASSERT_NE(nullptr, mp);
    CPLErrorReset();
    EXPECT_EQ(0, CountFeaturesInLayer(mp));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot copy"));
  }

  // 2. sqlite3_open_v2 failure branch when CPLCopyFile succeeds to /vsimem but
  // sqlite3_open_v2 fails under OS VFS (lines 4177-4185)
  {
    ASSERT_EQ(0, VSIMkdir("/vsimem/ogrosmdatasource_test_spill", 0755));
    absl::Cleanup vsimem_cleanup = [] {
      VSIRmdirRecursive("/vsimem/ogrosmdatasource_test_spill");
    };
    WithQuietHandler quiet;
    CPLConfigOptionSetter tmp_setter("CPL_TMPDIR",
                                     "/vsimem/ogrosmdatasource_test_spill",
                                     /*bSetOnlyIfUndefined=*/false);
    OGROSMDataSource ds_fail2;
    ASSERT_TRUE(ds_fail2.Open(osm_path.c_str(), options.List()));
    OGROSMLayer* mp = GetOsmLayer(&ds_fail2, 3);
    ASSERT_NE(nullptr, mp);
    CPLErrorReset();
    EXPECT_EQ(0, CountFeaturesInLayer(mp));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("sqlite3_open"));
  }

  // 3. Successful spillover branch to real temp directory (lines 4150-4161,
  // 4170-4176, 4189-4199)
  {
    const std::filesystem::path spill_dir =
        std::filesystem::path(::testing::TempDir()) / "osm_db_spill";
    std::filesystem::create_directories(spill_dir);
    absl::Cleanup dir_cleanup = [&spill_dir] {
      std::filesystem::remove_all(spill_dir);
    };
    CPLConfigOptionSetter tmp_setter("CPL_TMPDIR", spill_dir.string().c_str(),
                                     /*bSetOnlyIfUndefined=*/false);
    OGROSMDataSource ds;
    ASSERT_TRUE(ds.Open(osm_path.c_str(), options.List()));
    OGROSMLayer* mp = GetOsmLayer(&ds, 3);
    ASSERT_NE(nullptr, mp);
    EXPECT_GE(CountFeaturesInLayer(mp), 1);
  }
}

TEST(OgrOsmDataSourceTest, ExistingTmpFileAndParseErrorBranches) {
  // 1. Test OSM_EXISTING_TMPFILE (exercises lines 2950-2955)
  {
    const std::string missing_existing =
        (std::filesystem::path(::testing::TempDir()) / "missing_existing_dir" /
         "existing_db.sqlite")
            .string();
    CPLConfigOptionSetter file_setter("OSM_EXISTING_TMPFILE",
                                      missing_existing.c_str(),
                                      /*bSetOnlyIfUndefined=*/false);

    const std::string osm_path = "/vsimem/exist_test.osm";
    const std::string xml =
        "<osm version='0.6'><node id='1' lat='1' lon='1'><tag k='name' "
        "v='n'/></node></osm>";
    VsiMemTempWrapper wrapper(osm_path, xml);

    WithQuietHandler quiet;
    OGROSMDataSource ds;
    CPLErrorReset();
    EXPECT_FALSE(ds.Open(osm_path.c_str(), nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("sqlite3_open"));
  }

  // 2. Test OSM_ERROR in ParseNextChunk (exercises lines 4032-4038)
  {
    WithQuietHandler quiet;
    const std::string osm_err = "/vsimem/err_chunk.osm";
    const std::string xml_err =
        "<osm version='0.6'><node id='1' lat='1' lon='1'/><broken>";
    VsiMemTempWrapper wrapper_err(osm_err, xml_err);

    OGROSMDataSource ds_err;
    ASSERT_EQ(TRUE, ds_err.Open(osm_err.c_str(), nullptr));
    OGROSMLayer* pts = GetOsmLayer(&ds_err, 0);
    ASSERT_NE(nullptr, pts);
    CPLErrorReset();
    EXPECT_EQ(0, CountFeaturesInLayer(pts));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("An error occurred during the parsing"));
  }
}

TEST(OgrOsmDataSourceTest, TagsFormatConfigVariants) {
  // Test tags_format=hstore, attribute_name_laundering, and unsupported
  // tags_format in config file (lines 3510-3514 and 3523-3531)
  const std::string conf_path = "/vsimem/tags_fmt_conf.ini";
  const std::string conf_content =
      "[general]\n"
      "attribute_name_laundering=yes\n"
      "tags_format=hstore\n"
      "tags_format=invalid_fmt_xyz\n";
  VsiMemTempWrapper conf_wrapper(conf_path, conf_content);

  const std::string osm_path = "/vsimem/tags_fmt_test.osm";
  const std::string xml =
      "<osm version='0.6'><node id='1' lat='1' lon='1'/></osm>";
  VsiMemTempWrapper osm_wrapper(osm_path, xml);

  CPLStringList options;
  options.AddNameValue("CONFIG_FILE", conf_path.c_str());

  WithQuietHandler quiet;
  OGROSMDataSource ds;
  CPLErrorReset();
  EXPECT_TRUE(ds.Open(osm_path.c_str(), options.List()));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Unsupported value for tags_format"));
}

TEST(OgrOsmDataSourceTest, DatasetGetNextFeatureProgressPct) {
  // Test GetNextFeature with progress percentage and file size (lines
  // 3952-3961)
  const std::string osm_path = "/vsimem/prog_test.osm";
  const std::string xml =
      "<osm version='0.6'>\n"
      "  <node id='1' lat='1' lon='1'><tag k='name' v='p1'/></node>\n"
      "  <node id='2' lat='2' lon='2'><tag k='name' v='p2'/></node>\n"
      "</osm>\n";
  VsiMemTempWrapper wrapper(osm_path, xml);

  {
    OGROSMDataSource ds;
    ASSERT_TRUE(ds.Open(osm_path.c_str(), nullptr));

    OGRLayer* belonging = nullptr;
    double pct = 0.0;
    OGRFeatureUniquePtr f1(
        ds.GetNextFeature(&belonging, &pct, nullptr, nullptr));
    ASSERT_NE(nullptr, f1);
    EXPECT_DOUBLE_EQ(-1.0, pct);
    OGRFeatureUniquePtr f2(
        ds.GetNextFeature(&belonging, &pct, nullptr, nullptr));
    ASSERT_NE(nullptr, f2);
    EXPECT_EQ(nullptr, ds.GetNextFeature(&belonging, &pct, nullptr, nullptr));
    EXPECT_DOUBLE_EQ(1.0, pct);
  }

  // Sub-case 2: with file size initialized via SetDescription (lines 3922,
  // 3954-3956, 3996)
  {
    OGROSMDataSource ds2;
    ds2.SetDescription(osm_path.c_str());
    ASSERT_TRUE(ds2.Open(osm_path.c_str(), nullptr));

    auto ProgressCb = [](double /* dfComplete */, const char* /* pszMessage */,
                         void* /* pData */) -> int { return TRUE; };

    OGRLayer* belonging2 = nullptr;
    double pct2 = 0.0;
    OGRFeatureUniquePtr f2(
        ds2.GetNextFeature(&belonging2, &pct2, ProgressCb, nullptr));
    ASSERT_NE(nullptr, f2);
    EXPECT_GT(pct2, 0.0);
    EXPECT_LE(pct2, 1.0);
  }
}

TEST(OgrOsmDataSourceTest, DuplicatePrimaryKeysAndMissingWayInMultiLineString) {
  // 1. Test duplicate primary keys in SQLite tables when USE_CUSTOM_INDEXING=NO
  // (lines 426-429, 1572-1575, and 1757-1761)
  {
    const std::string osm_path = "/vsimem/dup_pkeys.osm";
    const std::string xml =
        "<osm version='0.6'>\n"
        "  <node id='1' lat='1.0' lon='1.0'/>\n"
        "  <node id='1' lat='2.0' lon='2.0'/>\n"
        "  <node id='2' lat='1.0' lon='2.0'/>\n"
        "  <node id='3' lat='2.0' lon='2.0'/>\n"
        "  <way id='10'>\n"
        "    <nd ref='1'/><nd ref='2'/><nd ref='3'/><nd ref='1'/>\n"
        "    <tag k='building' v='yes'/>\n"
        "  </way>\n"
        "  <way id='10'>\n"
        "    <nd ref='1'/><nd ref='2'/><nd ref='3'/><nd ref='1'/>\n"
        "    <tag k='building' v='yes'/>\n"
        "  </way>\n"
        "</osm>\n";
    VsiMemTempWrapper wrapper(osm_path, xml);

    CPLStringList options;
    options.AddNameValue("USE_CUSTOM_INDEXING", "NO");

    WithQuietHandler quiet;
    OGROSMDataSource ds;
    ASSERT_TRUE(ds.Open(osm_path.c_str(), options.List()));
    OGROSMLayer* mp = GetOsmLayer(&ds, 3);
    ASSERT_NE(nullptr, mp);
    CPLErrorReset();
    EXPECT_EQ(1, CountFeaturesInLayer(mp));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Failed inserting"));
  }

  // 2. Test relation with missing way in multilinestring (lines 2227-2243)
  {
    const std::string osm_path2 = "/vsimem/missing_way_mls.osm";
    const std::string xml2 =
        "<osm version='0.6'>\n"
        "  <node id='1' lat='1.0' lon='1.0'/>\n"
        "  <node id='2' lat='2.0' lon='2.0'/>\n"
        "  <way id='10'>\n"
        "    <nd ref='1'/><nd ref='2'/>\n"
        "    <tag k='highway' v='path'/>\n"
        "  </way>\n"
        "  <relation id='200'>\n"
        "    <member type='way' ref='10' role=''/>\n"
        "    <member type='way' ref='999999' role=''/>\n"
        "    <tag k='type' v='route'/>\n"
        "  </relation>\n"
        "</osm>\n";
    VsiMemTempWrapper wrapper2(osm_path2, xml2);

    OGROSMDataSource ds2;
    ASSERT_TRUE(ds2.Open(osm_path2.c_str(), nullptr));
    OGROSMLayer* lines = GetOsmLayer(&ds2, 1);
    ASSERT_NE(nullptr, lines);
    EXPECT_EQ(1, CountFeaturesInLayer(lines));
  }
}

// TODO: b/335317901 - Cover VSI_MALLOC_VERBOSE OOM paths, OSM_EXISTING_TMPFILE
// valid SQLite DB reuse, and FAKE_LOOKUP_NODES.

}  // namespace
}  // namespace autotest2
