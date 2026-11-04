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

// Tests for OGRMILayerAttrIndex and OGRMIAttrIndex.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogr_miattrind.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogr_miattrind.cpp

#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogr_attrind.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(OGRMIAttrIndexTest, FullLifecycle) {
  // Note: ogr_miattrind.cpp uses stdio VSIFOpen/VSIStat (not VSIFOpenL),
  // requiring a real scratch filesystem directory per AGENTS.md §6.2.
  std::string temp_dir =
      std::filesystem::path(::testing::TempDir()) / "miattrind_test";
  std::filesystem::create_directories(temp_dir);
  auto cleanup_temp =
      absl::MakeCleanup([&]() { std::filesystem::remove_all(temp_dir); });

  std::string idm_path =
      (std::filesystem::path(temp_dir) / "test.idm").string();

  auto mem_layer = std::make_unique<OGRMemLayer>("mi_test", nullptr, wkbPoint);

  OGRFieldDefn f_int("f_int", OFTInteger);
  mem_layer->CreateField(&f_int);

  OGRFieldDefn f_real("f_real", OFTReal);
  mem_layer->CreateField(&f_real);

  OGRFieldDefn f_str10("f_str10", OFTString);
  f_str10.SetWidth(10);
  mem_layer->CreateField(&f_str10);

  OGRFieldDefn f_str0("f_str0", OFTString);
  f_str0.SetWidth(0);  // Should default to 20
  mem_layer->CreateField(&f_str0);

  OGRFieldDefn f_i64("f_i64", OFTInteger64);
  mem_layer->CreateField(&f_i64);

  OGRFieldDefn f_unsupported("f_unsupported", OFTIntegerList);
  mem_layer->CreateField(&f_unsupported);

  OGRFieldDefn f_int2("f_int2", OFTInteger);
  mem_layer->CreateField(&f_int2);

  // Add multiple features with same key 100 to test GetAllMatches buffer growth
  for (int i = 0; i < 6; ++i) {
    auto feat = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    feat->SetFID(i + 1);
    feat->SetField(0, 100);
    feat->SetField(1, 1.5);
    feat->SetField(2, "match");
    feat->SetField(3, "str_default");
    feat->SetField(4, static_cast<GIntBig>(1000));
    ASSERT_EQ(OGRERR_NONE, mem_layer->CreateFeature(feat.get()));
  }

  std::unique_ptr<OGRLayerAttrIndex> layer_idx(OGRCreateDefaultLayerIndex());
  ASSERT_NE(nullptr, layer_idx);

  EXPECT_EQ(OGRERR_NONE,
            layer_idx->Initialize(idm_path.c_str(), mem_layer.get()));
  // Calling Initialize again with the same layer returns OGRERR_NONE
  // immediately
  EXPECT_EQ(OGRERR_NONE,
            layer_idx->Initialize(idm_path.c_str(), mem_layer.get()));

  // Create indexes
  EXPECT_EQ(OGRERR_NONE, layer_idx->CreateIndex(0));
  // Duplicate index error
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE, layer_idx->CreateIndex(0));
    EXPECT_EQ(OGRERR_FAILURE, layer_idx->CreateIndex(5));  // unsupported list
  }
  EXPECT_EQ(OGRERR_NONE, layer_idx->CreateIndex(1));
  EXPECT_EQ(OGRERR_NONE, layer_idx->CreateIndex(2));
  EXPECT_EQ(OGRERR_NONE, layer_idx->CreateIndex(3));

  // IndexAllFeatures
  EXPECT_EQ(OGRERR_NONE, layer_idx->IndexAllFeatures(-1));

  // Add feature with FID >= INT_MAX to trigger IndexAllFeatures failure
  {
    auto huge_fid_feat =
        std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    huge_fid_feat->SetFID(static_cast<GIntBig>(INT_MAX) + 5);
    huge_fid_feat->SetField(0, 100);
    ASSERT_EQ(OGRERR_NONE, mem_layer->CreateFeature(huge_fid_feat.get()));
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE, layer_idx->IndexAllFeatures(0));
  }

  // AddToIndex error with OGRNullFID
  {
    auto null_fid_feat =
        std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
    null_fid_feat->SetFID(OGRNullFID);
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE, layer_idx->AddToIndex(null_fid_feat.get(), 0));
  }

  // Query index
  OGRAttrIndex* attr_idx = layer_idx->GetFieldIndex(0);
  ASSERT_NE(nullptr, attr_idx);
  EXPECT_EQ(nullptr, layer_idx->GetFieldIndex(-1));
  EXPECT_EQ(nullptr, layer_idx->GetFieldIndex(5));

  OGRField sKey;
  sKey.Integer = 100;
  GIntBig first_match = attr_idx->GetFirstMatch(&sKey);
  EXPECT_GE(first_match, 1);
  EXPECT_LE(first_match, 6);

  OGRField missing_key;
  missing_key.Integer = 99999;
  EXPECT_EQ(OGRNullFID, attr_idx->GetFirstMatch(&missing_key));

  // GetAllMatches with pre-allocated and nullptr lists
  int nFIDCount = 0;
  int nLength = 0;
  GIntBig* matches =
      attr_idx->GetAllMatches(&sKey, nullptr, &nFIDCount, &nLength);
  ASSERT_NE(nullptr, matches);
  EXPECT_GE(nFIDCount, 6);
  CPLFree(matches);

  GIntBig* matches_simple = attr_idx->GetAllMatches(&sKey);
  ASSERT_NE(nullptr, matches_simple);
  CPLFree(matches_simple);

  // AddEntry & RemoveEntry
  EXPECT_EQ(OGRERR_NONE, attr_idx->AddEntry(&sKey, 999));
  EXPECT_EQ(OGRERR_FAILURE, attr_idx->AddEntry(nullptr, 999));
  EXPECT_EQ(OGRERR_FAILURE,
            attr_idx->AddEntry(&sKey, static_cast<GIntBig>(INT_MAX) + 1));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, attr_idx->RemoveEntry(&sKey, 999));
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION, attr_idx->Clear());

  // AddToIndex with specific field target and with null/unset field
  auto feat_partial = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
  feat_partial->SetFID(7);
  EXPECT_EQ(OGRERR_NONE, layer_idx->AddToIndex(feat_partial.get(), 1));
  EXPECT_EQ(OGRERR_NONE, layer_idx->AddToIndex(feat_partial.get(), -1));

  // RemoveFromIndex
  auto feat_to_remove = std::make_unique<OGRFeature>(mem_layer->GetLayerDefn());
  feat_to_remove->SetFID(6);
  feat_to_remove->SetField(0, 100);
  EXPECT_EQ(OGRERR_UNSUPPORTED_OPERATION,
            layer_idx->RemoveFromIndex(feat_to_remove.get()));

  // DropIndex on non-indexed field
  {
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE, layer_idx->DropIndex(5));
  }
  EXPECT_EQ(OGRERR_NONE, layer_idx->DropIndex(1));
  EXPECT_EQ(OGRERR_NONE, layer_idx->DropIndex(2));
  EXPECT_EQ(OGRERR_NONE, layer_idx->DropIndex(3));

  // Destroy layer_idx to flush index to disk
  layer_idx.reset();

  // Test loading existing index from disk with a second layer_idx instance
  {
    std::unique_ptr<OGRLayerAttrIndex> layer_idx2(OGRCreateDefaultLayerIndex());
    ASSERT_NE(nullptr, layer_idx2);
    EXPECT_EQ(OGRERR_NONE,
              layer_idx2->Initialize(idm_path.c_str(), mem_layer.get()));
    OGRAttrIndex* ai = layer_idx2->GetFieldIndex(0);
    ASSERT_NE(nullptr, ai);
    GIntBig match = ai->GetFirstMatch(&sKey);
    EXPECT_GE(match, 1);
    EXPECT_LE(match, 999);

    // Create index on previously unindexed field 6 while bINDAsReadOnly is
    // TRUE: exercises reopening .ind as r+ (read-write)
    EXPECT_EQ(OGRERR_NONE, layer_idx2->CreateIndex(6));
    EXPECT_EQ(OGRERR_NONE, layer_idx2->DropIndex(6));

    // Test raw XML with OFTInteger64 field and corrupt entry while index file
    // is intact
    {
      std::string ind_path =
          (std::filesystem::path(temp_dir) / "test.ind").string();
      std::string xml =
          "<OGRMILayerAttrIndex>"
          "<MIIDFilename>" +
          ind_path +
          "</MIIDFilename>"
          "<OGRMIAttrIndex>"
          "<FieldIndex>4</FieldIndex>"
          "<FieldName>f_i64</FieldName>"
          "<IndexIndex>0</IndexIndex>"
          "</OGRMIAttrIndex>"
          "<OGRMIAttrIndex>"
          "<FieldIndex>-1</FieldIndex>"
          "</OGRMIAttrIndex>"
          "</OGRMILayerAttrIndex>";

      std::unique_ptr<OGRLayerAttrIndex> layer_idx3(
          OGRCreateDefaultLayerIndex());
      ASSERT_NE(nullptr, layer_idx3);
      {
        WithQuietHandler quiet_handler;
        EXPECT_EQ(OGRERR_NONE,
                  layer_idx3->Initialize(xml.c_str(), mem_layer.get()));
      }
      OGRAttrIndex* ai64 = layer_idx3->GetFieldIndex(4);
      ASSERT_NE(nullptr, ai64);
      OGRField key64;
      key64.Integer64 = 100;
      EXPECT_EQ(OGRNullFID, ai64->GetFirstMatch(&key64));
      key64.Integer64 = (1LL << 40);
      {
        WithQuietHandler quiet_handler;
        EXPECT_EQ(OGRNullFID, ai64->GetFirstMatch(&key64));
      }

      // Drop last index in layer_idx2, which deletes test.ind
      EXPECT_EQ(OGRERR_NONE, layer_idx2->DropIndex(0));
      layer_idx2.reset();

      // Now layer_idx3 attempts CreateIndex on read-only index whose ind file
      // was unlinked
      {
        WithQuietHandler quiet_handler;
        EXPECT_EQ(OGRERR_FAILURE, layer_idx3->CreateIndex(0));
      }
    }
  }
}

TEST(OGRMIAttrIndexTest, RawXmlInitialization) {
  auto mem_layer = std::make_unique<OGRMemLayer>("raw_xml", nullptr, wkbPoint);
  OGRFieldDefn fld("id", OFTInteger);
  mem_layer->CreateField(&fld);

  // Index creation failure when .ind file cannot be created
  {
    std::unique_ptr<OGRLayerAttrIndex> layer_idx(OGRCreateDefaultLayerIndex());
    ASSERT_NE(nullptr, layer_idx);
    EXPECT_EQ(OGRERR_NONE,
              layer_idx->Initialize("/nonexistent_dir_xyz/test.idm",
                                    mem_layer.get()));
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE, layer_idx->CreateIndex(0));
  }

  // Missing ind file
  const char* raw_xml_bad_ind =
      "<OGRMILayerAttrIndex>"
      "<MIIDFilename>/vsimem/nonexistent.ind</MIIDFilename>"
      "<OGRMIAttrIndex>"
      "<FieldIndex>0</FieldIndex>"
      "<FieldName>id</FieldName>"
      "<IndexIndex>0</IndexIndex>"
      "</OGRMIAttrIndex>"
      "</OGRMILayerAttrIndex>";

  {
    std::unique_ptr<OGRLayerAttrIndex> layer_idx(OGRCreateDefaultLayerIndex());
    ASSERT_NE(nullptr, layer_idx);
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE,
              layer_idx->Initialize(raw_xml_bad_ind, mem_layer.get()));
  }

  // Invalid XML syntax (starts with <OGRMILayerAttrIndex> but has unclosed tag)
  {
    std::unique_ptr<OGRLayerAttrIndex> layer_idx(OGRCreateDefaultLayerIndex());
    ASSERT_NE(nullptr, layer_idx);
    WithQuietHandler quiet_handler;
    EXPECT_EQ(OGRERR_FAILURE,
              layer_idx->Initialize("<OGRMILayerAttrIndex><unclosed",
                                    mem_layer.get()));
  }
}

// TODO: b/335317901 - Cover remaining error branches in ogr_miattrind.cpp.

}  // namespace
}  // namespace autotest2
