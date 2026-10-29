// Copyright 2026 Google Inc. All Rights Reserved.
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
// Tests gcore/gdalrelationship.cpp.

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace {

TEST(GdalrelationshipTest, CreateGetSetAndDestroy) {
  // Null pointer checks on creation and getters.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr,
            GDALRelationshipCreate(nullptr, "left", "right", GRC_ONE_TO_MANY));
  EXPECT_EQ(nullptr,
            GDALRelationshipCreate("rel", nullptr, "right", GRC_ONE_TO_MANY));
  EXPECT_EQ(nullptr,
            GDALRelationshipCreate("rel", "left", nullptr, GRC_ONE_TO_MANY));
  EXPECT_EQ(nullptr, GDALRelationshipGetName(nullptr));
  EXPECT_EQ(nullptr, GDALRelationshipGetLeftTableName(nullptr));
  EXPECT_EQ(nullptr, GDALRelationshipGetRightTableName(nullptr));
  EXPECT_EQ(nullptr, GDALRelationshipGetMappingTableName(nullptr));
  EXPECT_EQ(nullptr, GDALRelationshipGetLeftTableFields(nullptr));
  EXPECT_EQ(nullptr, GDALRelationshipGetRightTableFields(nullptr));
  EXPECT_EQ(nullptr, GDALRelationshipGetLeftMappingTableFields(nullptr));
  EXPECT_EQ(nullptr, GDALRelationshipGetRightMappingTableFields(nullptr));
  EXPECT_EQ(nullptr, GDALRelationshipGetForwardPathLabel(nullptr));
  EXPECT_EQ(nullptr, GDALRelationshipGetBackwardPathLabel(nullptr));
  EXPECT_EQ(nullptr, GDALRelationshipGetRelatedTableType(nullptr));
  CPLPopErrorHandler();

  GDALDestroyRelationship(nullptr);

  // Valid relationship lifecycle.
  GDALRelationshipH rel = GDALRelationshipCreate(
      "building_furniture", "buildings", "furniture", GRC_MANY_TO_MANY);
  ASSERT_NE(nullptr, rel);

  EXPECT_STREQ("building_furniture", GDALRelationshipGetName(rel));
  EXPECT_EQ(GRC_MANY_TO_MANY, GDALRelationshipGetCardinality(rel));
  EXPECT_STREQ("buildings", GDALRelationshipGetLeftTableName(rel));
  EXPECT_STREQ("furniture", GDALRelationshipGetRightTableName(rel));

  // Mapping table name.
  EXPECT_STREQ("", GDALRelationshipGetMappingTableName(rel));
  GDALRelationshipSetMappingTableName(rel, "building_furniture_map");
  EXPECT_STREQ("building_furniture_map",
               GDALRelationshipGetMappingTableName(rel));

  // Left and right table fields.
  const char* left_fields[] = {"bldg_id", "zone_id", nullptr};
  GDALRelationshipSetLeftTableFields(rel, left_fields);
  char** got_left = GDALRelationshipGetLeftTableFields(rel);
  ASSERT_NE(nullptr, got_left);
  EXPECT_EQ(2, CSLCount(got_left));
  EXPECT_STREQ("bldg_id", got_left[0]);
  EXPECT_STREQ("zone_id", got_left[1]);
  CSLDestroy(got_left);

  const char* right_fields[] = {"furn_id", nullptr};
  GDALRelationshipSetRightTableFields(rel, right_fields);
  char** got_right = GDALRelationshipGetRightTableFields(rel);
  ASSERT_NE(nullptr, got_right);
  EXPECT_EQ(1, CSLCount(got_right));
  EXPECT_STREQ("furn_id", got_right[0]);
  CSLDestroy(got_right);

  // Left and right mapping table fields.
  const char* left_map_fields[] = {"map_bldg_id", nullptr};
  GDALRelationshipSetLeftMappingTableFields(rel, left_map_fields);
  char** got_left_map = GDALRelationshipGetLeftMappingTableFields(rel);
  ASSERT_NE(nullptr, got_left_map);
  EXPECT_EQ(1, CSLCount(got_left_map));
  EXPECT_STREQ("map_bldg_id", got_left_map[0]);
  CSLDestroy(got_left_map);

  const char* right_map_fields[] = {"map_furn_id", nullptr};
  GDALRelationshipSetRightMappingTableFields(rel, right_map_fields);
  char** got_right_map = GDALRelationshipGetRightMappingTableFields(rel);
  ASSERT_NE(nullptr, got_right_map);
  EXPECT_EQ(1, CSLCount(got_right_map));
  EXPECT_STREQ("map_furn_id", got_right_map[0]);
  CSLDestroy(got_right_map);

  // Type.
  EXPECT_EQ(GRT_ASSOCIATION, GDALRelationshipGetType(rel));
  GDALRelationshipSetType(rel, GRT_COMPOSITE);
  EXPECT_EQ(GRT_COMPOSITE, GDALRelationshipGetType(rel));

  // Forward and backward path labels.
  EXPECT_STREQ("", GDALRelationshipGetForwardPathLabel(rel));
  GDALRelationshipSetForwardPathLabel(rel, "contains");
  EXPECT_STREQ("contains", GDALRelationshipGetForwardPathLabel(rel));

  EXPECT_STREQ("", GDALRelationshipGetBackwardPathLabel(rel));
  GDALRelationshipSetBackwardPathLabel(rel, "is located within");
  EXPECT_STREQ("is located within", GDALRelationshipGetBackwardPathLabel(rel));

  // Related table type.
  EXPECT_STREQ("", GDALRelationshipGetRelatedTableType(rel));
  GDALRelationshipSetRelatedTableType(rel, "attributes");
  EXPECT_STREQ("attributes", GDALRelationshipGetRelatedTableType(rel));

  GDALDestroyRelationship(rel);
}

}  // namespace
