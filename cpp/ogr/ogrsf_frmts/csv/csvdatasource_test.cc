// Copyright 2017 Google Inc. All Rights Reserved.
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

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/csv/ogr_csv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

TEST(CsvDatasourceTest, MultiGeomNoOptions) {
  // Test reloading saved multi-geometry (RFC41) from CSV driver.
  const char kCsv[] = "WKT,id,_WKTgeom2\n\"POINT (1 2)\",1,\"POINT (3 4)\"\n";
  const char kFilename[] = "/vsimem/a.csv";
  const std::string data(kCsv, CPL_ARRAYSIZE(kCsv));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data);
  auto open_info =
      std::make_unique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  auto dataset = std::make_unique<OGRCSVDataSource>();
  EXPECT_TRUE(dataset->Open(kFilename, FALSE, FALSE, nullptr, false));
  ASSERT_EQ(1, dataset->GetLayerCount());
  OGRLayer *layer = dataset->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(wkbUnknown, layer->GetGeomType());
  ASSERT_EQ(1, layer->GetFeatureCount());

  OGRFeatureDefn *feature_defn = layer->GetLayerDefn();
  ASSERT_NE(nullptr, feature_defn);
  EXPECT_EQ(3, feature_defn->GetFieldCount());
  EXPECT_EQ(2, feature_defn->GetGeomFieldCount());
  EXPECT_STREQ("WKT", feature_defn->GetFieldDefn(0)->GetNameRef());
  EXPECT_STREQ("id", feature_defn->GetFieldDefn(1)->GetNameRef());
  EXPECT_STREQ("_WKTgeom2", feature_defn->GetFieldDefn(2)->GetNameRef());

  EXPECT_STREQ("", feature_defn->GetGeomFieldDefn(0)->GetNameRef());
  EXPECT_STREQ("geom__WKTgeom2",
               feature_defn->GetGeomFieldDefn(1)->GetNameRef());

  auto feature = absl::WrapUnique(layer->GetNextFeature());
  EXPECT_STREQ("POINT (1 2)", feature->GetFieldAsString("WKT"));
  EXPECT_STREQ("1", feature->GetFieldAsString("id"));
  EXPECT_STREQ("POINT (3 4)", feature->GetFieldAsString("_WKTgeom2"));

  {
    // The match to the "WKT" string field.
    auto geom = feature->GetGeomFieldRef("");
    OGRPoint point(1, 2);
    EXPECT_TRUE(point.Equals(geom));
  }

  {
    auto geom = feature->GetGeomFieldRef("geom__WKTgeom2");
    OGRPoint point(3, 4);
    EXPECT_TRUE(point.Equals(geom));
  }
}

TEST(CsvDatasourceTest, MultiGeom) {
  // Test reloading saved multi-geometry (RFC41) from CSV driver.
  // Sets KEEP_GEOM_COLUMNS=NO
  const char kCsv[] = "WKT,id,_WKTgeom2\n\"POINT (1 2)\",1,\"POINT (3 4)\"\n";
  const char kFilename[] = "/vsimem/a.csv";
  const std::string data(kCsv, CPL_ARRAYSIZE(kCsv));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data);
  auto open_info =
      std::make_unique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  auto dataset = std::make_unique<OGRCSVDataSource>();

  // Do not keep non-geometry fields that go with the geometry columns.
  auto options =
      VectorToCsl(std::vector<std::string>({"KEEP_GEOM_COLUMNS=NO"}));
  auto csl_cleaner = absl::MakeCleanup([options] { CSLDestroy(options); });
  EXPECT_TRUE(dataset->Open(kFilename, FALSE, FALSE, options, false));

  ASSERT_EQ(1, dataset->GetLayerCount());
  OGRLayer *layer = dataset->GetLayer(0);
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(wkbUnknown, layer->GetGeomType());
  ASSERT_EQ(1, layer->GetFeatureCount());

  OGRFeatureDefn *feature_defn = layer->GetLayerDefn();
  ASSERT_NE(nullptr, feature_defn);
  EXPECT_EQ(1, feature_defn->GetFieldCount());
  EXPECT_EQ(2, feature_defn->GetGeomFieldCount());
  EXPECT_STREQ("id", feature_defn->GetFieldDefn(0)->GetNameRef());

  EXPECT_STREQ("", feature_defn->GetGeomFieldDefn(0)->GetNameRef());
  EXPECT_STREQ("geom__WKTgeom2",
               feature_defn->GetGeomFieldDefn(1)->GetNameRef());

  auto feature = absl::WrapUnique(layer->GetNextFeature());
  EXPECT_STREQ("1", feature->GetFieldAsString("id"));

  {
    auto geom = feature->GetGeomFieldRef("");
    OGRPoint point(1, 2);
    EXPECT_TRUE(point.Equals(geom));
  }

  {
    auto geom = feature->GetGeomFieldRef("geom__WKTgeom2");
    OGRPoint point(3, 4);
    EXPECT_TRUE(point.Equals(geom));
  }
}

}  // namespace
}  // namespace autotest2
