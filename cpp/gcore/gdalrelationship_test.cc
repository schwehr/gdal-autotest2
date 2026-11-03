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
// Tests table relationship C API (gcore/gdalrelationship.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalrelationship.cpp

#include <array>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;

TEST(GdalRelationshipTest, CreateGetSetAndDestroy) {
  // Null pointer checks on creation and getters.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALRelationshipCreate(nullptr, "left", "right",
                                              GRC_ONE_TO_MANY));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALRelationshipCreate("rel", nullptr, "right", GRC_ONE_TO_MANY));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALRelationshipCreate("rel", "left", nullptr, GRC_ONE_TO_MANY));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
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
    CPLErrorReset();
  }

  GDALDestroyRelationship(nullptr);

  // Valid relationship lifecycle.
  GDALRelationshipH rel = GDALRelationshipCreate(
      "building_furniture", "buildings", "furniture", GRC_MANY_TO_MANY);
  ASSERT_NE(nullptr, rel);
  absl::Cleanup destroy_rel = [rel] { GDALDestroyRelationship(rel); };

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
  constexpr std::array<const char*, 3> kLeftFields = {"bldg_id", "zone_id",
                                                      nullptr};
  static_assert(kLeftFields.back() == nullptr);
  GDALRelationshipSetLeftTableFields(rel, kLeftFields.data());
  StringListPtr got_left(GDALRelationshipGetLeftTableFields(rel), CSLDestroy);
  ASSERT_NE(nullptr, got_left);
  EXPECT_THAT(CslToVector(got_left.get()), ElementsAre("bldg_id", "zone_id"));

  constexpr std::array<const char*, 2> kRightFields = {"furn_id", nullptr};
  static_assert(kRightFields.back() == nullptr);
  GDALRelationshipSetRightTableFields(rel, kRightFields.data());
  StringListPtr got_right(GDALRelationshipGetRightTableFields(rel), CSLDestroy);
  ASSERT_NE(nullptr, got_right);
  EXPECT_THAT(CslToVector(got_right.get()), ElementsAre("furn_id"));

  // Left and right mapping table fields.
  constexpr std::array<const char*, 2> kLeftMapFields = {"map_bldg_id",
                                                         nullptr};
  static_assert(kLeftMapFields.back() == nullptr);
  GDALRelationshipSetLeftMappingTableFields(rel, kLeftMapFields.data());
  StringListPtr got_left_map(GDALRelationshipGetLeftMappingTableFields(rel),
                             CSLDestroy);
  ASSERT_NE(nullptr, got_left_map);
  EXPECT_THAT(CslToVector(got_left_map.get()), ElementsAre("map_bldg_id"));

  constexpr std::array<const char*, 2> kRightMapFields = {"map_furn_id",
                                                          nullptr};
  static_assert(kRightMapFields.back() == nullptr);
  GDALRelationshipSetRightMappingTableFields(rel, kRightMapFields.data());
  StringListPtr got_right_map(GDALRelationshipGetRightMappingTableFields(rel),
                              CSLDestroy);
  ASSERT_NE(nullptr, got_right_map);
  EXPECT_THAT(CslToVector(got_right_map.get()), ElementsAre("map_furn_id"));

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
}

}  // namespace
}  // namespace autotest2
