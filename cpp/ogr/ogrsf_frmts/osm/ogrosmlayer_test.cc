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
// Tests for the OSM vector driver layers (ogrosmlayer.cpp).
//
// google3 build caveats:
// - Default osmconf.ini is located via GoogleGdalInitSupportFiles().
// - CPLDebug messages are compiled out (WITHOUT_CPLDEBUG).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/osm/ogrosmlayer.cpp
//   https://gdal.org/drivers/vector/osm.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_osm.py

#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/osm/ogr_osm.h"
#include "third_party/gdal/ogr/ogrsf_frmts/osm/osm_parser.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr std::string_view kSingleNodeOsm =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<osm version=\"0.6\">\n"
    "  <node id=\"1\" lat=\"10.0\" lon=\"20.0\"/>\n"
    "</osm>\n";

constexpr size_t kSwitchThreshold = 10000;  // SWITCH_THRESHOLD, ogrosmlayer.cpp
constexpr size_t kMaxThreshold = 100000;    // MAX_THRESHOLD, ogrosmlayer.cpp

std::unique_ptr<OGROSMDataSource> OpenDataset(
    const std::string& path, CSLConstList open_options = nullptr) {
  auto ds = std::make_unique<OGROSMDataSource>();
  ds->SetDescription(path.c_str());
  if (!ds->Open(path.c_str(), open_options)) {
    return nullptr;
  }
  return ds;
}

OGROSMLayer* GetOsmLayer(OGROSMDataSource* ds, int idx) {
  return cpl::down_cast<OGROSMLayer*>(
      static_cast<GDALDataset*>(ds)->GetLayer(idx));
}

// Note: kHardcodedZOrderSql must match ogrosmlayer.cpp:994-1006 byte-for-byte
// to exercise the hardcoded Z-order evaluation branch.
constexpr char kHardcodedZOrderSql[] =
    "SELECT (CASE [highway] WHEN 'minor' THEN 3 WHEN 'road' THEN 3 "
    "WHEN 'unclassified' THEN 3 WHEN 'residential' THEN 3 WHEN "
    "'tertiary_link' THEN 4 WHEN 'tertiary' THEN 4 WHEN "
    "'secondary_link' "
    "THEN 6 WHEN 'secondary' THEN 6 WHEN 'primary_link' THEN 7 WHEN "
    "'primary' THEN 7 WHEN 'trunk_link' THEN 8 WHEN 'trunk' THEN 8 "
    "WHEN 'motorway_link' THEN 9 WHEN 'motorway' THEN 9 ELSE 0 END) + "
    "(CASE WHEN [bridge] IN ('yes', 'true', '1') THEN 10 ELSE 0 END) + "
    "(CASE WHEN [tunnel] IN ('yes', 'true', '1') THEN -10 ELSE 0 END) "
    "+ "
    "(CASE WHEN [railway] IS NOT NULL THEN 5 ELSE 0 END) + "
    "(CASE WHEN [layer] IS NOT NULL THEN 10 * CAST([layer] AS INTEGER) "
    "ELSE 0 END)";

TEST(OgrosmLayerTest, BasicPropertiesAndFieldManagement) {
  const std::string path = "/vsimem/ogrosm_layer/basic.osm";
  VsiMemTempWrapper mem_file(path, std::string(kSingleNodeOsm));
  auto ds = OpenDataset(path);
  ASSERT_NE(nullptr, ds);

  auto* layer = GetOsmLayer(ds.get(), 0);
  ASSERT_NE(nullptr, layer);

  EXPECT_STREQ("points", layer->GetLayerDefn()->GetName());

  // Test boolean property flags and setters
  EXPECT_TRUE(layer->HasOSMId());
  layer->SetHasOSMId(false);
  EXPECT_FALSE(layer->HasOSMId());
  layer->SetHasOSMId(true);

  EXPECT_FALSE(layer->HasVersion());
  layer->SetHasVersion(true);
  EXPECT_TRUE(layer->HasVersion());

  EXPECT_FALSE(layer->HasTimestamp());
  layer->SetHasTimestamp(true);
  EXPECT_TRUE(layer->HasTimestamp());

  EXPECT_FALSE(layer->HasUID());
  layer->SetHasUID(true);
  EXPECT_TRUE(layer->HasUID());

  EXPECT_FALSE(layer->HasUser());
  layer->SetHasUser(true);
  EXPECT_TRUE(layer->HasUser());

  EXPECT_FALSE(layer->HasChangeset());
  layer->SetHasChangeset(true);
  EXPECT_TRUE(layer->HasChangeset());

  EXPECT_TRUE(layer->HasOtherTags());
  layer->SetHasOtherTags(false);
  EXPECT_FALSE(layer->HasOtherTags());
  layer->SetHasOtherTags(true);

  EXPECT_FALSE(layer->HasAllTags());
  layer->SetHasAllTags(true);
  EXPECT_TRUE(layer->HasAllTags());

  EXPECT_TRUE(layer->IsUserInterested());
  layer->SetDeclareInterest(false);
  EXPECT_FALSE(layer->IsUserInterested());
  layer->SetDeclareInterest(true);

  // Test field addition and laundering
  layer->AddField("custom:field:name", OFTString);
  EXPECT_GE(layer->GetFieldIndex("custom:field:name"), 0);
  EXPECT_GE(layer->GetLayerDefn()->GetFieldIndex("custom_field_name"), 0);
  EXPECT_EQ(-1, layer->GetLayerDefn()->GetFieldIndex("custom:field:name"));

  layer->AddField("osm_way_id", OFTInteger64);
  EXPECT_GE(layer->GetFieldIndex("osm_way_id"), 0);

  layer->AddField("all_tags", OFTString);
  EXPECT_GE(layer->GetFieldIndex("all_tags"), 0);

  EXPECT_EQ(-1, layer->GetFieldIndex("non_existent_field"));

  // Test significant / insignificant keys
  layer->AddInsignificantKey("insig_key");
  EXPECT_FALSE(layer->IsSignificantKey("insig_key"));
  EXPECT_TRUE(layer->IsSignificantKey("sig_key"));

  layer->AddIgnoreKey("ignore_exact");
  layer->AddIgnoreKey("openGeoDB:");
  layer->AddWarnKey("warn_key");

  // ResetReading on unread layer
  layer->ResetReading();
}

TEST(OgrosmLayerTest, SetFieldsFromTagsWithHstore) {
  const std::string path = "/vsimem/ogrosm_layer/hstore_tags.osm";
  VsiMemTempWrapper mem_file(path, std::string(kSingleNodeOsm));
  auto ds = OpenDataset(path);
  ASSERT_NE(nullptr, ds);

  auto* layer = GetOsmLayer(ds.get(), 0);
  ASSERT_NE(nullptr, layer);

  layer->SetHasVersion(true);
  layer->SetHasTimestamp(true);
  layer->SetHasUID(true);
  layer->SetHasUser(true);
  layer->SetHasChangeset(true);
  layer->SetHasOtherTags(true);
  layer->SetHasAllTags(false);

  layer->AddField("osm_version", OFTInteger);
  layer->AddField("osm_timestamp", OFTDateTime);
  layer->AddField("osm_uid", OFTInteger);
  layer->AddField("osm_user", OFTString);
  layer->AddField("osm_changeset", OFTInteger64);
  layer->AddField("known_key", OFTString);

  layer->AddIgnoreKey("ignored_tag");
  layer->AddIgnoreKey("prefix:");
  layer->AddWarnKey("warn_tag");

  // 1. Valid string ISO timestamp and HSTORE escaping
  // Note: AddInOtherOrAllTags temporarily NUL-terminates keys at ':' in-place,
  // so keys containing ':' must be mutable char[].
  auto feat1 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  char key_known[] = "known_key";
  char key_other[] = "other_quote";
  char key_ignored[] = "ignored_tag";
  char key_prefix[] = "prefix:sub";
  char key_other_prefix[] = "other:sub";
  char key_warn[] = "warn_tag";

  OSMTag tags1[] = {
      {key_known, "known_val"},
      {key_other, "val\"with\\quotes"},
      {key_ignored, "ignore_me"},
      {key_prefix, "prefix_val"},
      {key_other_prefix, "other_prefix_val"},
      {key_warn, "warn_val"},
  };

  OSMInfo info1{};
  info1.nVersion = 3;
  info1.nUID = 101;
  info1.pszUserSID = "alice";
  info1.nChangeset = 99999;
  info1.bTimeStampIsStr = 1;
  info1.ts.pszTimeStamp = "2023-08-15T10:20:30Z";

  layer->SetFieldsFromTags(feat1.get(), 1001, /*bIsWayID=*/false,
                           std::size(tags1), tags1, &info1);

  EXPECT_EQ(1001, feat1->GetFieldAsInteger64("osm_id"));
  EXPECT_EQ(3, feat1->GetFieldAsInteger("osm_version"));
  EXPECT_EQ(101, feat1->GetFieldAsInteger("osm_uid"));
  EXPECT_STREQ("alice", feat1->GetFieldAsString("osm_user"));
  EXPECT_EQ(99999, feat1->GetFieldAsInteger64("osm_changeset"));
  EXPECT_STREQ("known_val", feat1->GetFieldAsString("known_key"));
  EXPECT_STREQ("2023/08/15 10:20:30+00",
               feat1->GetFieldAsString("osm_timestamp"));

  const char* other_tags1 = feat1->GetFieldAsString("other_tags");
  ASSERT_NE(nullptr, other_tags1);
  EXPECT_THAT(other_tags1, HasSubstr(R"("other_quote"=>"val\"with\\quotes")"));

  // 2. Invalid string timestamp
  auto feat2 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  info1.ts.pszTimeStamp = "not-a-valid-timestamp";
  layer->SetFieldsFromTags(feat2.get(), 1002, /*bIsWayID=*/false, 0, nullptr,
                           &info1);
  EXPECT_FALSE(feat2->IsFieldSetAndNotNull(
      layer->GetLayerDefn()->GetFieldIndex("osm_timestamp")));

  // 3. Fallback to FID when bIsWayID is true but osm_way_id is not present
  auto feat3 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  layer->SetFieldsFromTags(feat3.get(), 1003, /*bIsWayID=*/true, 0, nullptr,
                           &info1);
  EXPECT_EQ(1003, feat3->GetFID());
}

TEST(OgrosmLayerTest, SetFieldsFromTagsWithJson) {
  const std::string path = "/vsimem/ogrosm_layer/json_tags.osm";
  VsiMemTempWrapper mem_file(path, std::string(kSingleNodeOsm));
  const char* const open_options[] = {"TAGS_FORMAT=JSON", nullptr};
  auto ds = OpenDataset(path, open_options);
  ASSERT_NE(nullptr, ds);

  auto* layer = GetOsmLayer(ds.get(), 0);
  ASSERT_NE(nullptr, layer);

  layer->SetHasTimestamp(true);
  layer->SetHasOtherTags(true);
  layer->SetHasAllTags(true);
  layer->AddField("osm_timestamp", OFTDateTime);
  layer->AddField("osm_way_id", OFTInteger64);
  layer->AddField("all_tags", OFTString);

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  OSMTag tags[] = {
      {"json_special",
       "line1\nline2\r\ttab\\slash\"quote\x01"
       "ctrl"},
  };
  OSMInfo info{};
  info.bTimeStampIsStr = 0;
  info.ts.nTimeStamp = 1692094830;

  layer->SetFieldsFromTags(feat.get(), 2001, /*bIsWayID=*/true, std::size(tags),
                           tags, &info);

  EXPECT_EQ(2001, feat->GetFieldAsInteger64("osm_way_id"));
  EXPECT_STREQ("2023/08/15 10:20:30", feat->GetFieldAsString("osm_timestamp"));

  const char* all_tags = feat->GetFieldAsString("all_tags");
  ASSERT_NE(nullptr, all_tags);
  EXPECT_THAT(all_tags, HasSubstr(R"(\n)"));
  EXPECT_THAT(all_tags, HasSubstr(R"(\r)"));
  EXPECT_THAT(all_tags, HasSubstr(R"(\t)"));
  EXPECT_THAT(all_tags, HasSubstr(R"(\")"));
  EXPECT_THAT(all_tags, HasSubstr(R"(\\)"));
  EXPECT_THAT(all_tags, HasSubstr(R"(\u0001)"));
}

TEST(OgrosmLayerTest, ComputedAttributesHardcodedZOrder) {
  const std::string path = "/vsimem/ogrosm_layer/zorder.osm";
  VsiMemTempWrapper mem_file(path, std::string(kSingleNodeOsm));
  auto ds = OpenDataset(path);
  ASSERT_NE(nullptr, ds);

  // 1. On other_relations, explicitly add highway, bridge, tunnel, railway,
  // layer before adding computed attribute so anIndexToBind[0..4] >= 0.
  auto* rel_layer = GetOsmLayer(ds.get(), 4);
  ASSERT_NE(nullptr, rel_layer);
  rel_layer->AddField("highway", OFTString);
  rel_layer->AddField("bridge", OFTString);
  rel_layer->AddField("tunnel", OFTString);
  rel_layer->AddField("railway", OFTString);
  rel_layer->AddField("layer", OFTString);
  rel_layer->AddComputedAttribute("z_order", OFTInteger, kHardcodedZOrderSql);

  struct TestCase {
    const char* highway;
    const char* bridge;
    const char* tunnel;
    const char* railway;
    const char* layer_str;
    int expected_z;
  };

  const TestCase test_cases[] = {
      {"motorway", "yes", nullptr, nullptr, "1", 9 + 10 + 10},
      {"motorway_link", nullptr, "yes", nullptr, nullptr, 9 - 10},
      {"trunk", nullptr, nullptr, nullptr, nullptr, 8},
      {"trunk_link", nullptr, nullptr, nullptr, nullptr, 8},
      {"primary", "true", nullptr, nullptr, nullptr, 7 + 10},
      {"primary_link", nullptr, nullptr, nullptr, nullptr, 7},
      {"secondary", "1", nullptr, nullptr, nullptr, 6 + 10},
      {"secondary_link", nullptr, nullptr, nullptr, nullptr, 6},
      {"tertiary", nullptr, "true", nullptr, nullptr, 4 - 10},
      {"tertiary_link", nullptr, nullptr, nullptr, nullptr, 4},
      {"residential", nullptr, "1", nullptr, nullptr, 3 - 10},
      {"unclassified", nullptr, nullptr, nullptr, nullptr, 3},
      {"road", nullptr, nullptr, nullptr, nullptr, 3},
      {"minor", nullptr, nullptr, nullptr, nullptr, 3},
      {"footway", nullptr, nullptr, nullptr, nullptr, 0},
      {nullptr, nullptr, nullptr, "rail", nullptr, 5},
      {nullptr, nullptr, nullptr, nullptr, "-1", -10},
  };

  for (const auto& tc : test_cases) {
    auto feat = std::make_unique<OGRFeature>(rel_layer->GetLayerDefn());
    if (tc.highway) feat->SetField("highway", tc.highway);
    if (tc.bridge) feat->SetField("bridge", tc.bridge);
    if (tc.tunnel) feat->SetField("tunnel", tc.tunnel);
    if (tc.railway) feat->SetField("railway", tc.railway);
    if (tc.layer_str) feat->SetField("layer", tc.layer_str);

    rel_layer->SetFieldsFromTags(feat.get(), 1, false, 0, nullptr, nullptr);
    EXPECT_EQ(tc.expected_z, feat->GetFieldAsInteger("z_order"));
  }

  // 2. On multilinestrings, highway, bridge, tunnel, railway, layer are NOT
  // added. This exercises anIndexToBind[0..4] < 0 calling GetValueOfTag for all
  // 5 components.
  auto* mls_layer = GetOsmLayer(ds.get(), 2);
  ASSERT_NE(nullptr, mls_layer);
  mls_layer->AddComputedAttribute("z_order", OFTInteger, kHardcodedZOrderSql);

  auto mls_feat = std::make_unique<OGRFeature>(mls_layer->GetLayerDefn());
  OSMTag raw_tags[] = {
      {"highway", "primary"}, {"bridge", "yes"}, {"tunnel", "1"},
      {"railway", "rail"},    {"layer", "2"},
  };
  mls_layer->SetFieldsFromTags(mls_feat.get(), 10, false, std::size(raw_tags),
                               raw_tags, nullptr);
  EXPECT_EQ(7 + 10 - 10 + 5 + 20, mls_feat->GetFieldAsInteger("z_order"));
}

TEST(OgrosmLayerTest, ComputedAttributesCustomSql) {
  const std::string path = "/vsimem/ogrosm_layer/custom_sql.osm";
  VsiMemTempWrapper mem_file(path, std::string(kSingleNodeOsm));
  auto ds = OpenDataset(path);
  ASSERT_NE(nullptr, ds);

  auto* layer = GetOsmLayer(ds.get(), 0);
  ASSERT_NE(nullptr, layer);

  layer->AddField("int_fld", OFTInteger);
  layer->AddField("int64_fld", OFTInteger64);
  layer->AddField("real_fld", OFTReal);
  layer->AddField("str_fld", OFTString);

  // Duplicate attribute name error
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    layer->AddComputedAttribute("int_fld", OFTInteger, "SELECT 1");
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  // Invalid SQL error
  CPLErrorReset();
  {
    WithQuietHandler quiet;
    layer->AddComputedAttribute("bad_sql", OFTInteger,
                                "SELECT ??? FROM invalid");
  }
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(-1, layer->GetLayerDefn()->GetFieldIndex("bad_sql"));

  // Valid computed attributes
  layer->AddComputedAttribute("sum_int", OFTInteger, "SELECT [int_fld] + 10");
  layer->AddComputedAttribute("calc_real", OFTReal, "SELECT [real_fld] * 2.5");
  layer->AddComputedAttribute("concat_str", OFTString,
                              "SELECT [str_fld] || '_suffix'");
  layer->AddComputedAttribute("from_tag", OFTString,
                              "SELECT [custom_tag] || '_tagged'");
  layer->AddComputedAttribute("bracket_escape", OFTString,
                              "SELECT '\\[bracket\\]'");

  auto feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat->SetField("int_fld", 5);
  feat->SetField("int64_fld", 10000000000LL);
  feat->SetField("real_fld", 4.0);
  feat->SetField("str_fld", "prefix");

  OSMTag tags[] = {
      {"custom_tag", "tag_value"},
  };

  layer->SetFieldsFromTags(feat.get(), 1, false, std::size(tags), tags,
                           nullptr);

  EXPECT_EQ(15, feat->GetFieldAsInteger("sum_int"));
  EXPECT_DOUBLE_EQ(10.0, feat->GetFieldAsDouble("calc_real"));
  EXPECT_STREQ("prefix_suffix", feat->GetFieldAsString("concat_str"));
  EXPECT_STREQ("tag_value_tagged", feat->GetFieldAsString("from_tag"));
  EXPECT_STREQ("[bracket]", feat->GetFieldAsString("bracket_escape"));

  // Also test with unset/null fields
  auto null_feat = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  layer->SetFieldsFromTags(null_feat.get(), 2, false, 0, nullptr, nullptr);
  EXPECT_FALSE(null_feat->IsFieldSetAndNotNull(
      layer->GetLayerDefn()->GetFieldIndex("sum_int")));
}

TEST(OgrosmLayerTest, AddFeatureFilteringAndThreshold) {
  const std::string path = "/vsimem/ogrosm_layer/filtering.osm";
  VsiMemTempWrapper mem_file(path, std::string(kSingleNodeOsm));
  auto ds = OpenDataset(path);
  ASSERT_NE(nullptr, ds);

  auto* layer = GetOsmLayer(ds.get(), 0);
  ASSERT_NE(nullptr, layer);

  // 1. User not interested
  layer->SetDeclareInterest(false);
  auto feat1 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  bool filtered_out = false;
  EXPECT_TRUE(
      layer->AddFeature(std::move(feat1), /*bAttrFilterAlreadyEvaluated=*/false,
                        &filtered_out, /*bCheckFeatureThreshold=*/true));
  EXPECT_TRUE(filtered_out);
  layer->SetDeclareInterest(true);

  // 2. Spatial filter rejection and acceptance
  EXPECT_EQ(nullptr, layer->GetSpatialFilterEnvelope());
  layer->SetSpatialFilterRect(0.0, 0.0, 5.0, 5.0);
  ASSERT_NE(nullptr, layer->GetSpatialFilterEnvelope());

  auto feat_outside = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat_outside->SetGeometryDirectly(new OGRPoint(10.0, 10.0));
  filtered_out = false;
  EXPECT_TRUE(layer->AddFeature(std::move(feat_outside), false, &filtered_out));
  EXPECT_TRUE(filtered_out);

  auto feat_inside = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat_inside->SetGeometryDirectly(new OGRPoint(2.0, 2.0));
  filtered_out = false;
  EXPECT_TRUE(layer->AddFeature(std::move(feat_inside), false, &filtered_out));
  EXPECT_FALSE(filtered_out);
  layer->SetSpatialFilter(nullptr);
  EXPECT_EQ(nullptr, layer->GetSpatialFilterEnvelope());

  // 3. Attribute filter rejection and acceptance
  ASSERT_EQ(OGRERR_NONE, layer->SetAttributeFilter("osm_id = '42'"));
  EXPECT_TRUE(layer->HasAttributeFilter());

  auto feat_attr_reject = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat_attr_reject->SetField("osm_id", "1");
  EXPECT_FALSE(layer->EvaluateAttributeFilter(feat_attr_reject.get()));
  filtered_out = false;
  EXPECT_TRUE(
      layer->AddFeature(std::move(feat_attr_reject), false, &filtered_out));
  EXPECT_TRUE(filtered_out);

  auto feat_attr_accept = std::make_unique<OGRFeature>(layer->GetLayerDefn());
  feat_attr_accept->SetField("osm_id", "42");
  EXPECT_TRUE(layer->EvaluateAttributeFilter(feat_attr_accept.get()));
  filtered_out = false;
  EXPECT_TRUE(
      layer->AddFeature(std::move(feat_attr_accept), false, &filtered_out));
  EXPECT_FALSE(filtered_out);

  ASSERT_EQ(OGRERR_NONE, layer->SetAttributeFilter(nullptr));
  EXPECT_FALSE(layer->HasAttributeFilter());

  // 4. ForceResetReading cleans up buffered features
  layer->ForceResetReading();

  // 5. Exceeding MAX_THRESHOLD (100,000)
  for (int i = 0; i <= static_cast<int>(kMaxThreshold); ++i) {
    auto f = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    layer->AddFeature(std::move(f), false, nullptr,
                      /*bCheckFeatureThreshold=*/false);
  }

  // 100,001st feature (with check=true and size 100,001 > 100000) emits warning
  // and returns false
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto f_overflow1 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    EXPECT_FALSE(layer->AddFeature(std::move(f_overflow1), false, nullptr,
                                   /*bCheckFeatureThreshold=*/true));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too many features"));

    // Subsequent feature suppresses repeated warning and returns false
    CPLErrorReset();
    auto f_overflow2 = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    EXPECT_FALSE(layer->AddFeature(std::move(f_overflow2), false, nullptr,
                                   /*bCheckFeatureThreshold=*/true));
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }

  layer->ForceResetReading();
}

TEST(OgrosmLayerTest, SetAttributeFilterVariations) {
  const std::string path = "/vsimem/ogrosm_layer/attr_filter.osm";
  VsiMemTempWrapper mem_file(path, std::string(kSingleNodeOsm));
  const char* const open_options[] = {"INTERLEAVED_READING=YES", nullptr};
  auto ds = OpenDataset(path, open_options);
  ASSERT_NE(nullptr, ds);

  auto* layer = GetOsmLayer(ds.get(), 0);
  ASSERT_NE(nullptr, layer);

  // Null when already null
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter(nullptr));

  // Setting identical filter returns early
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("osm_id = '1'"));
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("osm_id = '1'"));

  // Syntax error
  {
    WithQuietHandler quiet;
    EXPECT_NE(OGRERR_NONE, layer->SetAttributeFilter("INVALID SQL ==="));
  }

  // Filter on buffered features when m_nFeatureArrayIndex > 0
  EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter(nullptr));
  for (int id : {10, 20, 30}) {
    auto f = std::make_unique<OGRFeature>(layer->GetLayerDefn());
    f->SetField("osm_id", std::to_string(id).c_str());
    layer->AddFeature(std::move(f), false);
  }

  // Read 1 feature so m_nFeatureArrayIndex is 1
  OGRFeatureUniquePtr f_read(layer->GetNextFeature());
  ASSERT_NE(nullptr, f_read);
  EXPECT_STREQ("10", f_read->GetFieldAsString("osm_id"));

  // Set filter when reading has already started: emits warning
  {
    WithQuietHandler quiet;
    EXPECT_EQ(OGRERR_NONE, layer->SetAttributeFilter("osm_id = '30'"));
  }

  // Next features from buffer are read in sequence
  OGRFeatureUniquePtr f2(layer->GetNextFeature());
  ASSERT_NE(nullptr, f2);
  EXPECT_STREQ("20", f2->GetFieldAsString("osm_id"));

  OGRFeatureUniquePtr f3(layer->GetNextFeature());
  ASSERT_NE(nullptr, f3);
  EXPECT_STREQ("30", f3->GetFieldAsString("osm_id"));
  EXPECT_EQ(nullptr, layer->GetNextFeature());

  // ResetReading after reading
  layer->ResetReading();
}

TEST(OgrosmLayerTest, CapabilitiesAndExtent) {
  // Test with dataset without bounds
  const std::string path_nobounds = "/vsimem/ogrosm_layer/nobounds.osm";
  const std::string xml_nobounds =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<osm version=\"0.6\">\n"
      "  <node id=\"1\" lat=\"10.0\" lon=\"20.0\"/>\n"
      "</osm>\n";
  VsiMemTempWrapper mem_file_nobounds(path_nobounds, xml_nobounds);
  auto ds_nobounds = OpenDataset(path_nobounds);
  ASSERT_NE(nullptr, ds_nobounds);

  auto* layer_nobounds = GetOsmLayer(ds_nobounds.get(), 0);
  ASSERT_NE(nullptr, layer_nobounds);

  EXPECT_FALSE(layer_nobounds->TestCapability(OLCFastGetExtent));
  EXPECT_FALSE(layer_nobounds->TestCapability(OLCStringsAsUTF8));
  EXPECT_FALSE(layer_nobounds->TestCapability(OLCRandomRead));

  OGREnvelope env;
  EXPECT_NE(OGRERR_NONE, layer_nobounds->GetExtent(&env, false));

  // Test with dataset with bounds
  const std::string path_bounds = "/vsimem/ogrosm_layer/bounds.osm";
  const std::string xml_bounds =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<osm version=\"0.6\">\n"
      "  <bounds minlon=\"10.0\" minlat=\"20.0\" maxlon=\"15.0\" "
      "maxlat=\"25.0\"/>\n"
      "  <node id=\"1\" lat=\"21.0\" lon=\"11.0\"/>\n"
      "</osm>\n";
  VsiMemTempWrapper mem_file_bounds(path_bounds, xml_bounds);
  auto ds_bounds = OpenDataset(path_bounds);
  ASSERT_NE(nullptr, ds_bounds);

  auto* layer_bounds = GetOsmLayer(ds_bounds.get(), 0);
  ASSERT_NE(nullptr, layer_bounds);

  EXPECT_TRUE(layer_bounds->TestCapability(OLCFastGetExtent));
  EXPECT_EQ(OGRERR_NONE, layer_bounds->GetExtent(&env, false));
  EXPECT_DOUBLE_EQ(10.0, env.MinX);
  EXPECT_DOUBLE_EQ(20.0, env.MinY);
  EXPECT_DOUBLE_EQ(15.0, env.MaxX);
  EXPECT_DOUBLE_EQ(25.0, env.MaxY);
}

TEST(OgrosmLayerTest, InterleavedReadingAndLayerSwitching) {
  const std::string path = "/vsimem/ogrosm_layer/interleaved.osm";
  const std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<osm version=\"0.6\">\n"
      "  <node id=\"1\" lat=\"10.0\" lon=\"20.0\">\n"
      "    <tag k=\"name\" v=\"pt1\"/>\n"
      "  </node>\n"
      "  <way id=\"10\">\n"
      "    <nd ref=\"1\"/>\n"
      "    <nd ref=\"1\"/>\n"
      "    <tag k=\"highway\" v=\"residential\"/>\n"
      "  </way>\n"
      "</osm>\n";
  VsiMemTempWrapper mem_file(path, xml);

  const char* const open_options[] = {"INTERLEAVED_READING=YES", nullptr};
  auto ds = OpenDataset(path, open_options);
  ASSERT_NE(nullptr, ds);

  auto* points = GetOsmLayer(ds.get(), 0);
  auto* lines = GetOsmLayer(ds.get(), 1);
  ASSERT_NE(nullptr, points);
  ASSERT_NE(nullptr, lines);

  OGROSMLayer* new_cur_layer = nullptr;

  // 1. Initial call with *ppoNewCurLayer == nullptr sets *ppoNewCurLayer = this
  OGRFeatureUniquePtr f1(
      points->MyGetNextFeature(&new_cur_layer, nullptr, nullptr));
  ASSERT_NE(nullptr, f1);
  EXPECT_EQ(points, new_cur_layer);

  // 2. Call with *ppoNewCurLayer == lines (!= points) returns nullptr
  // immediately
  new_cur_layer = lines;
  EXPECT_EQ(nullptr,
            points->MyGetNextFeature(&new_cur_layer, nullptr, nullptr));
  EXPECT_EQ(lines, new_cur_layer);

  // 3. Switch when lines has > 10,000 features
  points->ForceResetReading();
  for (int i = 0; i < static_cast<int>(kSwitchThreshold) + 1; ++i) {
    auto f = std::make_unique<OGRFeature>(lines->GetLayerDefn());
    lines->AddFeature(std::move(f), false, nullptr,
                      /*bCheckFeatureThreshold=*/false);
  }
  new_cur_layer = points;
  EXPECT_EQ(nullptr,
            points->MyGetNextFeature(&new_cur_layer, nullptr, nullptr));
  EXPECT_EQ(lines, new_cur_layer);

  lines->ForceResetReading();

  // ResetReading in interleaved mode
  points->ResetReading();
}

TEST(OgrosmLayerTest, FeatureCountViaSqliteDecorator) {
  const std::string path = "/vsimem/ogrosm_layer/feat_count.osm";
  const std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<osm version=\"0.6\">\n"
      "  <node id=\"1\" lat=\"10.0\" lon=\"20.0\">\n"
      "    <tag k=\"name\" v=\"n1\"/>\n"
      "  </node>\n"
      "  <node id=\"2\" lat=\"10.1\" lon=\"20.1\">\n"
      "    <tag k=\"name\" v=\"n2\"/>\n"
      "  </node>\n"
      "</osm>\n";
  VsiMemTempWrapper mem_file(path, xml);
  auto ds = OpenDataset(path);
  ASSERT_NE(nullptr, ds);

  auto* layer = GetOsmLayer(ds.get(), 0);
  ASSERT_NE(nullptr, layer);

  // Normally returns -1
  EXPECT_EQ(-1, layer->GetFeatureCount(TRUE));

  // Drain the process-global oListDSToBeOpened entry queued by
  // OGROSMResultLayerDecorator::GetFeatureCount() even on early ASSERT_* exit
  // (also covers GetInterestLayersForDSName).
  absl::Cleanup drain_list = [&path] { EXPECT_NE(nullptr, OpenDataset(path)); };

  // When executed via SQLITE dialect SELECT COUNT(*), feature count is enabled
  {
    OGRLayer* count_layer =
        ds->ExecuteSQL("SELECT COUNT(*) FROM points", nullptr, "SQLITE");
    ASSERT_NE(nullptr, count_layer);
    absl::Cleanup count_layer_cleaner = [&ds, count_layer] {
      ds->ReleaseResultSet(count_layer);
    };
    EXPECT_EQ(1, count_layer->GetFeatureCount(TRUE));
    OGRFeatureUniquePtr f(count_layer->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(2, f->GetFieldAsInteger64(0));
  }
}

// TODO: b/335317901 - Cover AddToArray std::bad_alloc catch and
// AddComputedAttribute sqlite3_open_v2 failure.

}  // namespace
}  // namespace autotest2
