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
// Tests for GPX datasource (ogrgpxdatasource.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gpx/ogrgpxdatasource.cpp
//   https://gdal.org/drivers/vector/gpx.html

#include <cstdio>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpx/ogr_gpx.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

size_t StdoutCaptureWrite(const void* ptr, size_t size, size_t nmemb,
                          FILE* stream) {
  auto* out = reinterpret_cast<std::string*>(stream);
  if (out != nullptr && ptr != nullptr) {
    out->append(static_cast<const char*>(ptr), size * nmemb);
  }
  return nmemb;
}

TEST(OgrGpxDataSourceTest, CapabilitiesAndLayersAccess) {
  OGRGPXDataSource ds;

  // 1. Verify TestCapability returns expected booleans.
  EXPECT_TRUE(ds.TestCapability(ODsCCreateLayer));
  EXPECT_FALSE(ds.TestCapability(ODsCDeleteLayer));
  EXPECT_TRUE(ds.TestCapability(ODsCZGeometries));
  EXPECT_FALSE(ds.TestCapability("UnknownCapability"));

  // 2. Layer count and indexing when empty.
  EXPECT_EQ(0, ds.GetLayerCount());
  EXPECT_EQ(nullptr, ds.GetLayer(-1));
  EXPECT_EQ(nullptr, ds.GetLayer(0));
  EXPECT_EQ(nullptr, ds.GetLayer(10));
}

TEST(OgrGpxDataSourceTest, CreateLayers) {
  constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/create_layers.gpx";
  absl::Cleanup cleanup_file = [kPath] { VSIUnlink(kPath); };

  OGRGPXDataSource ds;
  ASSERT_TRUE(ds.Create(kPath, nullptr));

  // 1. Unsupported or invalid geometry types.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, ds.ICreateLayer("invalid_none", nullptr, nullptr));

    OGRGeomFieldDefn poly_defn("poly", wkbPolygon);
    EXPECT_EQ(nullptr, ds.ICreateLayer("invalid_poly", &poly_defn, nullptr));

    OGRGeomFieldDefn unknown_defn("unknown", wkbUnknown);
    EXPECT_EQ(nullptr,
              ds.ICreateLayer("invalid_unknown", &unknown_defn, nullptr));
  }

  // 2. Point layers: waypoints, route_points, track_points, and custom names.
  OGRGeomFieldDefn pt_defn("geom", wkbPoint);
  OGRLayer* wpt_layer = ds.ICreateLayer("waypoints", &pt_defn, nullptr);
  ASSERT_NE(nullptr, wpt_layer);
  EXPECT_STREQ("waypoints", wpt_layer->GetName());

  OGRLayer* rtept_layer = ds.ICreateLayer("route_points", &pt_defn, nullptr);
  ASSERT_NE(nullptr, rtept_layer);
  EXPECT_STREQ("route_points", rtept_layer->GetName());

  OGRLayer* trkpt_layer = ds.ICreateLayer("track_points", &pt_defn, nullptr);
  ASSERT_NE(nullptr, trkpt_layer);
  EXPECT_STREQ("track_points", trkpt_layer->GetName());

  OGRLayer* custom_pt_layer = ds.ICreateLayer("my_points", &pt_defn, nullptr);
  ASSERT_NE(nullptr, custom_pt_layer);

  OGRGeomFieldDefn pt25d_defn("geom", wkbPoint25D);
  OGRLayer* wpt25d_layer =
      ds.ICreateLayer("waypoints_25d", &pt25d_defn, nullptr);
  ASSERT_NE(nullptr, wpt25d_layer);

  // 3. LineString layers: default is route, FORCE_GPX_TRACK=YES makes it track.
  OGRGeomFieldDefn line_defn("geom", wkbLineString);
  OGRLayer* route_layer = ds.ICreateLayer("routes", &line_defn, nullptr);
  ASSERT_NE(nullptr, route_layer);

  const char* const track_opts[] = {"FORCE_GPX_TRACK=YES", nullptr};
  OGRLayer* forced_track_layer =
      ds.ICreateLayer("forced_track", &line_defn, track_opts);
  ASSERT_NE(nullptr, forced_track_layer);

  // 4. MultiLineString layers: default is track, FORCE_GPX_ROUTE=YES makes it
  // route.
  OGRGeomFieldDefn multiline_defn("geom", wkbMultiLineString);
  OGRLayer* track_layer = ds.ICreateLayer("tracks", &multiline_defn, nullptr);
  ASSERT_NE(nullptr, track_layer);

  const char* const route_opts[] = {"FORCE_GPX_ROUTE=YES", nullptr};
  OGRLayer* forced_route_layer =
      ds.ICreateLayer("forced_route", &multiline_defn, route_opts);
  ASSERT_NE(nullptr, forced_route_layer);

  EXPECT_EQ(9, ds.GetLayerCount());
  EXPECT_NE(nullptr, ds.GetLayer(0));
  EXPECT_NE(nullptr, ds.GetLayer(8));
  EXPECT_EQ(nullptr, ds.GetLayer(9));
}

TEST(OgrGpxDataSourceTest, OpenValidationsAndVersions) {
  // 1. Opening with update access should fail.
  {
    constexpr char kDummyPath[] = "/vsimem/ogrgpxdatasource_test/update.gpx";
    VsiMemTempWrapper dummy(kDummyPath, "<gpx version=\"1.1\"></gpx>");
    GDALOpenInfo update_info(kDummyPath, GDAL_OF_UPDATE, nullptr);
    OGRGPXDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&update_info));
  }

  // 2. Non-existent file.
  {
    GDALOpenInfo non_existent_info(
        "/vsimem/ogrgpxdatasource_test/does_not_exist.gpx", GDAL_OF_READONLY,
        nullptr);
    OGRGPXDataSource ds;
    EXPECT_FALSE(ds.Open(&non_existent_info));
  }

  // 3. Root element is not <gpx>.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/not_gpx.xml";
    VsiMemTempWrapper not_gpx(kPath, "<not_gpx><child/></not_gpx>");
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    EXPECT_FALSE(ds.Open(&info));
  }

  // 4. Version 1.0 file.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/v10.gpx";
    VsiMemTempWrapper v10(kPath, "<gpx version=\"1.0\"></gpx>");
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    EXPECT_TRUE(ds.Open(&info));
    EXPECT_STREQ("1.0", ds.GetVersion());
    EXPECT_EQ(5, ds.GetLayerCount());
  }

  // 5. Version 1.1 file.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/v11.gpx";
    VsiMemTempWrapper v11(kPath, "<gpx version=\"1.1\"></gpx>");
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    EXPECT_TRUE(ds.Open(&info));
    EXPECT_STREQ("1.1", ds.GetVersion());
  }

  // 6. Missing version defaults to 1.1 with a warning.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/no_ver.gpx";
    VsiMemTempWrapper no_ver(kPath, "<gpx></gpx>");
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    WithQuietHandler quiet;
    EXPECT_TRUE(ds.Open(&info));
    EXPECT_STREQ("1.1", ds.GetVersion());
  }

  // 7. Unknown version (e.g. 2.0) issues warning and retains version string.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/v20.gpx";
    VsiMemTempWrapper v20(kPath, "<gpx version=\"2.0\"></gpx>");
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    WithQuietHandler quiet;
    EXPECT_TRUE(ds.Open(&info));
    EXPECT_STREQ("2.0", ds.GetVersion());
  }

  // 8. Extension namespace in <gpx> attributes.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/ext_ns.gpx";
    VsiMemTempWrapper ext_ns(
        kPath,
        "<gpx version=\"1.1\" xmlns:ogr=\"http://osgeo.org/gdal\"></gpx>");
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    EXPECT_TRUE(ds.Open(&info));
    EXPECT_TRUE(ds.GetUseExtensions());
  }

  // 9. Extensions detected at depth 2 without xmlns:ogr attribute.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/ext_elem.gpx";
    VsiMemTempWrapper ext_elem(kPath,
                               "<gpx version=\"1.1\">\n"
                               "  <wpt lat=\"1.0\" lon=\"2.0\">\n"
                               "    <extensions><foo>bar</foo></extensions>\n"
                               "  </wpt>\n"
                               "</gpx>");
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    EXPECT_TRUE(ds.Open(&info));
    EXPECT_TRUE(ds.GetUseExtensions());
  }
}

TEST(OgrGpxDataSourceTest, OpenMetadataParsing) {
  constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/metadata.gpx";
  constexpr char kGpx[] =
      "<gpx version=\"1.1\">\n"
      "  <metadata>\n"
      "    <name>TestDataset</name>\n"
      "    <desc>Dataset description</desc>\n"
      "    <time>2026-11-03T12:00:00Z</time>\n"
      "    <keywords>sample,gpx</keywords>\n"
      "    <author>\n"
      "      <name>John Doe</name>\n"
      "      <email id=\"john\" domain=\"example.com\"/>\n"
      "      <link href=\"http://author.example.com\">\n"
      "        <text>Author Homepage</text>\n"
      "        <type>text/html</type>\n"
      "      </link>\n"
      "    </author>\n"
      "    <copyright author=\"CopyHolder\">\n"
      "      <year>2026</year>\n"
      "      <license>Apache-2.0</license>\n"
      "    </copyright>\n"
      "    <link href=\"http://link1.org\">\n"
      "      <text>Link1 Text</text>\n"
      "      <type>Link1 Type</type>\n"
      "    </link>\n"
      "    <link href=\"http://link2.org\">\n"
      "      <text>Link2 Text</text>\n"
      "      <type>Link2 Type</type>\n"
      "    </link>\n"
      "  </metadata>\n"
      "</gpx>";
  VsiMemTempWrapper wrapper(kPath, kGpx);

  GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
  OGRGPXDataSource ds;
  ASSERT_TRUE(ds.Open(&info));

  EXPECT_STREQ("TestDataset", ds.GetMetadataItem("NAME"));
  EXPECT_STREQ("Dataset description", ds.GetMetadataItem("DESCRIPTION"));
  EXPECT_STREQ("2026-11-03T12:00:00Z", ds.GetMetadataItem("TIME"));
  EXPECT_STREQ("sample,gpx", ds.GetMetadataItem("KEYWORDS"));
  EXPECT_STREQ("John Doe", ds.GetMetadataItem("AUTHOR_NAME"));
  EXPECT_STREQ("john@example.com", ds.GetMetadataItem("AUTHOR_EMAIL"));
  EXPECT_STREQ("http://author.example.com",
               ds.GetMetadataItem("AUTHOR_LINK_HREF"));
  EXPECT_STREQ("Author Homepage", ds.GetMetadataItem("AUTHOR_LINK_TEXT"));
  EXPECT_STREQ("text/html", ds.GetMetadataItem("AUTHOR_LINK_TYPE"));
  EXPECT_STREQ("CopyHolder", ds.GetMetadataItem("COPYRIGHT_AUTHOR"));
  EXPECT_STREQ("2026", ds.GetMetadataItem("COPYRIGHT_YEAR"));
  EXPECT_STREQ("Apache-2.0", ds.GetMetadataItem("COPYRIGHT_LICENSE"));
  EXPECT_STREQ("http://link1.org", ds.GetMetadataItem("LINK_1_HREF"));
  EXPECT_STREQ("Link1 Text", ds.GetMetadataItem("LINK_1_TEXT"));
  EXPECT_STREQ("Link1 Type", ds.GetMetadataItem("LINK_1_TYPE"));
  EXPECT_STREQ("http://link2.org", ds.GetMetadataItem("LINK_2_HREF"));
  EXPECT_STREQ("Link2 Text", ds.GetMetadataItem("LINK_2_TEXT"));
  EXPECT_STREQ("Link2 Type", ds.GetMetadataItem("LINK_2_TYPE"));
}

TEST(OgrGpxDataSourceTest, OpenErrorAndBoundaryPaths) {
  // 1. Short malformed XML (< PARSER_BUF_SIZE) containing <?xml and <gpx.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/bad_xml_short.gpx";
    VsiMemTempWrapper bad(kPath, "<?xml version=\"1.0\"?><gpx><unclosed");
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&info));
  }

  // 2. Long malformed XML (>= PARSER_BUF_SIZE bytes in first buffer) containing
  // <?xml and <gpx.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/bad_xml_long.gpx";
    std::string long_bad =
        "<?xml version=\"1.0\"?><gpx version=\"1.1\"><unclosed";
    long_bad.append(PARSER_BUF_SIZE * 2, ' ');
    VsiMemTempWrapper bad(kPath, long_bad);
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&info));
  }

  // 3. Million-laugh pattern check (m_nDataHandlerCounter >= PARSER_BUF_SIZE).
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/million_laugh.gpx";
    constexpr char kLaughs[] =
        "<!DOCTYPE gpx [\n"
        "  <!ENTITY a \"x\">\n"
        "  <!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">\n"
        "  <!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">\n"
        "  <!ENTITY d \"&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;\">\n"
        "  <!ENTITY e \"&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;\">\n"
        "]>\n"
        "<gpx version=\"1.1\"><metadata><desc>&e;</desc></metadata></gpx>";
    VsiMemTempWrapper bad(kPath, kLaughs);
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Open(&info));
  }

  // 4. Large file without extensions (> 1024 * 1024 bytes) breaks early and
  // succeeds.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/large_file.gpx";
    std::string large = "<gpx version=\"1.1\">";
    large.append(1024 * 1024 + 100, ' ');
    large.append("</gpx>");
    VsiMemTempWrapper file(kPath, large);
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    EXPECT_TRUE(ds.Open(&info));
  }

  // 5. 50 chunks of spaces before <gpx> hits nCount == 50 break and fails
  // silently.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/count_50.gpx";
    std::string count_50 = "<?xml version=\"1.0\"?>";
    count_50.append(55 * PARSER_BUF_SIZE, ' ');
    VsiMemTempWrapper file(kPath, count_50);
    GDALOpenInfo info(kPath, GDAL_OF_READONLY, nullptr);
    OGRGPXDataSource ds;
    EXPECT_FALSE(ds.Open(&info));
  }
}

TEST(OgrGpxDataSourceTest, CreateOptionsAndMetadata) {
  // 1. Refuse to overwrite existing file.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/overwrite.gpx";
    VsiMemTempWrapper existing(kPath, "<gpx/>");
    OGRGPXDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Create(kPath, nullptr));
  }

  // 2. Failed create on unopenable path (parent component is a regular file).
  {
    constexpr char kBlocker[] = "/vsimem/ogrgpxdatasource_test/blocker";
    VsiMemTempWrapper blocker(kBlocker, "file");
    constexpr char kBlockedPath[] =
        "/vsimem/ogrgpxdatasource_test/blocker/child.gpx";
    OGRGPXDataSource ds;
    WithQuietHandler quiet;
    EXPECT_FALSE(ds.Create(kBlockedPath, nullptr));
  }

  // 3. Test LINEFORMAT options: CRLF, LF, and invalid.
  {
    constexpr char kPathCRLF[] = "/vsimem/ogrgpxdatasource_test/crlf.gpx";
    absl::Cleanup cleanup_crlf = [kPathCRLF] { VSIUnlink(kPathCRLF); };
    const char* const crlf_opts[] = {"LINEFORMAT=CRLF", nullptr};
    OGRGPXDataSource ds_crlf;
    EXPECT_TRUE(ds_crlf.Create(kPathCRLF, const_cast<char**>(crlf_opts)));
  }
  {
    constexpr char kPathLF[] = "/vsimem/ogrgpxdatasource_test/lf.gpx";
    absl::Cleanup cleanup_lf = [kPathLF] { VSIUnlink(kPathLF); };
    const char* const lf_opts[] = {"LINEFORMAT=LF", nullptr};
    OGRGPXDataSource ds_lf;
    EXPECT_TRUE(ds_lf.Create(kPathLF, const_cast<char**>(lf_opts)));
  }
  {
    constexpr char kPathBad[] = "/vsimem/ogrgpxdatasource_test/bad_lf.gpx";
    absl::Cleanup cleanup_bad = [kPathBad] { VSIUnlink(kPathBad); };
    const char* const bad_opts[] = {"LINEFORMAT=INVALID", nullptr};
    OGRGPXDataSource ds_bad;
    WithQuietHandler quiet;
    EXPECT_TRUE(ds_bad.Create(kPathBad, const_cast<char**>(bad_opts)));
  }

  // 4. Test GPX_USE_EXTENSIONS with default and custom namespace.
  {
    constexpr char kPathExtDefault[] =
        "/vsimem/ogrgpxdatasource_test/ext_default.gpx";
    absl::Cleanup cleanup_ext = [kPathExtDefault] {
      VSIUnlink(kPathExtDefault);
    };
    const char* const ext_opts[] = {"GPX_USE_EXTENSIONS=YES", nullptr};
    OGRGPXDataSource ds_ext;
    EXPECT_TRUE(ds_ext.Create(kPathExtDefault, const_cast<char**>(ext_opts)));
    EXPECT_TRUE(ds_ext.GetUseExtensions());
    EXPECT_EQ("ogr", ds_ext.GetExtensionsNS());
  }
  {
    constexpr char kPathExtCustom[] =
        "/vsimem/ogrgpxdatasource_test/ext_custom.gpx";
    absl::Cleanup cleanup_ext_custom = [kPathExtCustom] {
      VSIUnlink(kPathExtCustom);
    };
    const char* const ext_custom_opts[] = {
        "GPX_USE_EXTENSIONS=YES", "GPX_EXTENSIONS_NS=myprefix",
        "GPX_EXTENSIONS_NS_URL=http://example.com/ns", nullptr};
    OGRGPXDataSource ds_ext_custom;
    EXPECT_TRUE(ds_ext_custom.Create(kPathExtCustom,
                                     const_cast<char**>(ext_custom_opts)));
    EXPECT_TRUE(ds_ext_custom.GetUseExtensions());
    EXPECT_EQ("myprefix", ds_ext_custom.GetExtensionsNS());
  }

  // 5. Test CREATOR and all METADATA_* options.
  {
    constexpr char kPathFull[] = "/vsimem/ogrgpxdatasource_test/full_meta.gpx";
    absl::Cleanup cleanup_full = [kPathFull] { VSIUnlink(kPathFull); };
    const char* const full_opts[] = {
        "CREATOR=My<Custom>&App",
        "METADATA_NAME=Project Alpha",
        "METADATA_DESCRIPTION=Comprehensive & Detailed",
        "METADATA_AUTHOR_NAME=Alice Smith",
        "METADATA_AUTHOR_EMAIL=alice@example.org",
        "METADATA_AUTHOR_LINK_HREF=http://alice.org",
        "METADATA_AUTHOR_LINK_TEXT=Alice Site",
        "METADATA_AUTHOR_LINK_TYPE=text/html",
        "METADATA_COPYRIGHT_AUTHOR=ACME Corp",
        "METADATA_COPYRIGHT_YEAR=2026",
        "METADATA_COPYRIGHT_LICENSE=CC-BY-4.0",
        "METADATA_LINK_1_HREF=http://link1.org",
        "METADATA_LINK_1_TEXT=Link 1 Text",
        "METADATA_LINK_1_TYPE=text/plain",
        "METADATA_TIME=2026-11-03T09:00:00Z",
        "METADATA_KEYWORDS=tag1,tag2",
        nullptr};

    {
      OGRGPXDataSource ds_full;
      EXPECT_TRUE(ds_full.Create(kPathFull, const_cast<char**>(full_opts)));
      EXPECT_NE(nullptr, ds_full.GetOutputFP());
    }

    // Verify written XML content in file.
    vsi_l_offset nSize = 0;
    GByte* pabyData = VSIGetMemFileBuffer(kPathFull, &nSize, FALSE);
    ASSERT_NE(nullptr, pabyData);
    std::string content(reinterpret_cast<const char*>(pabyData), nSize);
    EXPECT_THAT(content, HasSubstr("creator=\"My&lt;Custom&gt;&amp;App\""));
    EXPECT_THAT(content, HasSubstr("<name>Project Alpha</name>"));
    EXPECT_THAT(content,
                HasSubstr("<desc>Comprehensive &amp; Detailed</desc>"));
    EXPECT_THAT(content, HasSubstr("<name>Alice Smith</name>"));
    EXPECT_THAT(content,
                HasSubstr("<email id=\"alice\" domain=\"example.org\"/>"));
    EXPECT_THAT(content, HasSubstr("<link href=\"http://alice.org\">"));
    EXPECT_THAT(content, HasSubstr("<text>Alice Site</text>"));
    EXPECT_THAT(content, HasSubstr("<type>text/html</type>"));
    EXPECT_THAT(content, HasSubstr("<copyright author=\"ACME Corp\">"));
    EXPECT_THAT(content, HasSubstr("<year>2026</year>"));
    EXPECT_THAT(content, HasSubstr("<license>CC-BY-4.0</license>"));
    EXPECT_THAT(content, HasSubstr("<link href=\"http://link1.org\">"));
    EXPECT_THAT(content, HasSubstr("<text>Link 1 Text</text>"));
    EXPECT_THAT(content, HasSubstr("<type>text/plain</type>"));
    EXPECT_THAT(content, HasSubstr("<time>2026-11-03T09:00:00Z</time>"));
    EXPECT_THAT(content, HasSubstr("<keywords>tag1,tag2</keywords>"));
  }
}

TEST(OgrGpxDataSourceTest, DestructorBoundsAndTrailingTags) {
  // 1. Destructor writes <bounds> when coordinates were added.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/bounds.gpx";
    absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };
    {
      OGRGPXDataSource ds;
      ASSERT_TRUE(ds.Create(kPath, nullptr));
      ds.AddCoord(-122.0, 37.0);
      ds.AddCoord(-121.5, 37.5);
    }
    vsi_l_offset nSize = 0;
    GByte* pabyData = VSIGetMemFileBuffer(kPath, &nSize, FALSE);
    ASSERT_NE(nullptr, pabyData);
    std::string content(reinterpret_cast<const char*>(pabyData), nSize);
    EXPECT_THAT(content, HasSubstr("<bounds minlat=\"37.000000000000000\""
                                   " minlon=\"-122.000000000000000\""
                                   " maxlat=\"37.500000000000000\""
                                   " maxlon=\"-121.500000000000000\"/>"));
  }

  // 2. Destructor closes open route tag if m_nLastRteId != -1.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/close_rte.gpx";
    absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };
    {
      OGRGPXDataSource ds;
      ASSERT_TRUE(ds.Create(kPath, nullptr));
      ds.m_nLastRteId = 42;
    }
    vsi_l_offset nSize = 0;
    GByte* pabyData = VSIGetMemFileBuffer(kPath, &nSize, FALSE);
    ASSERT_NE(nullptr, pabyData);
    std::string content(reinterpret_cast<const char*>(pabyData), nSize);
    EXPECT_THAT(content, HasSubstr("</rte>\n</gpx>"));
  }

  // 3. Destructor closes open track and segment tags if m_nLastTrkId != -1.
  {
    constexpr char kPath[] = "/vsimem/ogrgpxdatasource_test/close_trk.gpx";
    absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };
    {
      OGRGPXDataSource ds;
      ASSERT_TRUE(ds.Create(kPath, nullptr));
      ds.m_nLastTrkId = 7;
    }
    vsi_l_offset nSize = 0;
    GByte* pabyData = VSIGetMemFileBuffer(kPath, &nSize, FALSE);
    ASSERT_NE(nullptr, pabyData);
    std::string content(reinterpret_cast<const char*>(pabyData), nSize);
    EXPECT_THAT(content, HasSubstr("  </trkseg>\n</trk>\n</gpx>"));
  }

  // 4. Create to /dev/stdout with VSIStdoutSetRedirection (non-back-seekable
  // path).
  {
    std::string captured;
    VSIStdoutSetRedirection(StdoutCaptureWrite,
                            reinterpret_cast<FILE*>(&captured));
    absl::Cleanup reset_stdout = [] {
      VSIStdoutSetRedirection(fwrite, stdout);
    };

    {
      OGRGPXDataSource ds;
      ASSERT_TRUE(ds.Create("/dev/stdout", nullptr));
      ds.AddCoord(-10.0, 20.0);
    }

    EXPECT_THAT(captured, HasSubstr("<?xml version=\"1.0\"?>"));
    EXPECT_THAT(captured, HasSubstr("</gpx>"));
    // Non-seekable stream must not contain <bounds>.
    EXPECT_THAT(captured, Not(HasSubstr("<bounds")));
  }
}

TEST(OgrGpxDataSourceTest, GeometryTypeTracking) {
  OGRGPXDataSource ds;
  EXPECT_EQ(GPX_NONE, ds.GetLastGPXGeomTypeWritten());

  ds.SetLastGPXGeomTypeWritten(GPX_WPT);
  EXPECT_EQ(GPX_WPT, ds.GetLastGPXGeomTypeWritten());

  ds.SetLastGPXGeomTypeWritten(GPX_TRACK);
  EXPECT_EQ(GPX_TRACK, ds.GetLastGPXGeomTypeWritten());
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Line 43 in ogrgpxdatasource.cpp (~OGRGPXDataSource
// opening brace) is compiler-attributed and unexecuted in gcov.
