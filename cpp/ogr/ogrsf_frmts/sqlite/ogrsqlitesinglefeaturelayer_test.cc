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
// Tests for OGRSQLiteSingleFeatureLayer (ogrsqlitesinglefeaturelayer.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqlitesinglefeaturelayer.cpp

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogrsqlitebase.h"

namespace autotest2 {
namespace {

TEST(OGRSQLiteSingleFeatureLayerTest, IntegerFeatureLayer) {
  auto poLayer = std::make_unique<OGRSQLiteSingleFeatureLayer>("int_field", 42);

  const OGRFeatureDefn* poDefn = poLayer->GetLayerDefn();
  ASSERT_NE(nullptr, poDefn);
  EXPECT_STREQ("SELECT", poDefn->GetName());
  ASSERT_EQ(1, poDefn->GetFieldCount());
  EXPECT_STREQ("int_field", poDefn->GetFieldDefn(0)->GetNameRef());
  EXPECT_EQ(OFTInteger, poDefn->GetFieldDefn(0)->GetType());

  EXPECT_FALSE(poLayer->TestCapability(OLCFastFeatureCount));
  EXPECT_FALSE(poLayer->TestCapability("UnknownCapability"));

  // First GetNextFeature returns the single feature.
  std::unique_ptr<OGRFeature> poFeature1(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeature1);
  EXPECT_EQ(42, poFeature1->GetFieldAsInteger(0));
  EXPECT_EQ(0, poFeature1->GetFID());

  // Second GetNextFeature returns nullptr.
  EXPECT_EQ(nullptr, poLayer->GetNextFeature());

  // ResetReading resets the iterator.
  poLayer->ResetReading();
  std::unique_ptr<OGRFeature> poFeature2(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeature2);
  EXPECT_EQ(42, poFeature2->GetFieldAsInteger(0));
  EXPECT_EQ(nullptr, poLayer->GetNextFeature());
}

TEST(OGRSQLiteSingleFeatureLayerTest, StringFeatureLayer) {
  auto poLayer =
      std::make_unique<OGRSQLiteSingleFeatureLayer>("str_field", "hello");

  const OGRFeatureDefn* poDefn = poLayer->GetLayerDefn();
  ASSERT_NE(nullptr, poDefn);
  EXPECT_STREQ("SELECT", poDefn->GetName());
  ASSERT_EQ(1, poDefn->GetFieldCount());
  EXPECT_STREQ("str_field", poDefn->GetFieldDefn(0)->GetNameRef());
  EXPECT_EQ(OFTString, poDefn->GetFieldDefn(0)->GetType());

  std::unique_ptr<OGRFeature> poFeature(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeature);
  EXPECT_STREQ("hello", poFeature->GetFieldAsString(0));
  EXPECT_EQ(0, poFeature->GetFID());

  EXPECT_EQ(nullptr, poLayer->GetNextFeature());
}

// TODO: b/335317901 - Cover OGRSQLiteSingleFeatureLayer feature mutation error
// paths if extended.

}  // namespace
}  // namespace autotest2
