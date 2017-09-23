// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests mitab_imapinfofile.cc.
//
// See also:
//   http://www.gdal.org/drv_mitab.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_mitab.py

#include <memory>
#include <string>

#include "logging.h"
#include "file/base/path.h"
#include "googletest.h"
#include "gunit.h"
#include "ogr/ogrsf_frmts/mitab/mitab.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_feature.h"
#include "ogr/ogr_geometry.h"
#include "ogr/ogr_spatialref.h"

namespace autotest2 {
namespace {

const char kTestData[] = "cpp/ogr/ogrsf_frmts/mitab/testdata/";

TEST(MitabImapinfofileTest, SmartOpenDoesNotExist) {
  IMapInfoFile *info = IMapInfoFile::SmartOpen("does_not_exist", false, true);
  EXPECT_EQ(nullptr, info);
}

TEST(MitabImapinfofileTest, SmartOpenMid2) {
  const string filepath = file::JoinPath(
      FLAGS_test_srcdir, kTestData, "empty_first_field_with_tab_delimiter.mid");
  LOG(INFO) << "Trying: " << filepath;
  std::unique_ptr<IMapInfoFile> info(
      IMapInfoFile::SmartOpen(filepath.c_str(), false, false));
  ASSERT_NE(nullptr, info);
  EXPECT_EQ(TABFC_MIFFile, info->GetFileClass());
  EXPECT_STREQ("empty_first_field_with_tab_delimiter", info->GetTableName());
  EXPECT_EQ(-1, info->GetFeatureCount(false));
  EXPECT_EQ(1, info->GetFeatureCount(true));
  std::unique_ptr<OGRFeature> feature(info->GetNextFeature());
  ASSERT_NE(nullptr, feature);
  feature.reset(info->GetNextFeature());
  ASSERT_EQ(nullptr, feature);
  info->ResetReading();
  feature.reset(info->GetNextFeature());
  ASSERT_NE(nullptr, feature);
}

TEST(MitabImapinfofileTest, SmartOpenTabWgs84) {
  const string filepath = file::JoinPath(
      FLAGS_test_srcdir, kTestData, "point-wgs84/point.tab");
  std::unique_ptr<IMapInfoFile> info(
      IMapInfoFile::SmartOpen(filepath.c_str(), false, false));
  ASSERT_NE(nullptr, info);
  EXPECT_EQ(TABFC_TABFile, info->GetFileClass());
  EXPECT_STREQ("point", info->GetTableName());

  OGRSpatialReference *srs = info->GetSpatialRef();
  ASSERT_NE(nullptr, srs);
  EXPECT_EQ(OGRERR_NONE, srs->Validate());
  OGRSpatialReference srs_wgs84;
  srs_wgs84.importFromEPSG(4326);
  EXPECT_TRUE(srs_wgs84.IsSame(srs));
}

TEST(MitabImapinfofileTest, SmartOpenTabPoint) {
  const string filepath = file::JoinPath(
      FLAGS_test_srcdir, kTestData, "point-wgs84/point.tab");
  std::unique_ptr<IMapInfoFile> info(
      IMapInfoFile::SmartOpen(filepath.c_str(), false, false));
  ASSERT_NE(nullptr, info);

  // Check the single point.
  std::unique_ptr<OGRFeature> feature(info->GetNextFeature());
  ASSERT_NE(nullptr, feature);
  EXPECT_EQ(1, feature->GetFID());
  OGRGeometry *geometry = feature->GetGeometryRef();
  ASSERT_NE(nullptr, geometry);
  auto point = dynamic_cast<OGRPoint *>(geometry);
  ASSERT_NE(nullptr, point);
  EXPECT_NEAR(0.0, point->getX(), 0.000001);
  EXPECT_NEAR(1.0, point->getY(), 0.000001);

  feature.reset(info->GetNextFeature());
  ASSERT_EQ(nullptr, feature);
  info->ResetReading();
  feature.reset(info->GetNextFeature());
  ASSERT_NE(nullptr, feature);
}

}  // namespace
}  // namespace autotest2
