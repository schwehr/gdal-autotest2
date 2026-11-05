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

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogr_attrind.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::NotNull;

class FakeAttrIndex : public OGRAttrIndex {
 public:
  FakeAttrIndex() = default;
  ~FakeAttrIndex() override = default;

  void SetMatches(std::vector<GIntBig> fids) {
    default_fids_ = std::move(fids);
    queued_fids_.clear();
    queue_idx_ = 0;
  }

  void QueueMatches(std::vector<GIntBig> fids) {
    queued_fids_.push_back(std::move(fids));
  }

  GIntBig GetFirstMatch(OGRField* psKey) override {
    const auto& fids = GetNextFIDs();
    return fids.empty() ? OGRNullFID : fids[0];
  }

  GIntBig* GetAllMatches(OGRField* psKey) override { return nullptr; }

  GIntBig* GetAllMatches(OGRField* psKey, GIntBig* panFIDList, int* nFIDCount,
                         int* nLength) override {
    const auto& fids = GetNextFIDs();
    int cur_count = nFIDCount ? *nFIDCount : 0;
    int cur_len = nLength ? *nLength : 0;
    int needed = cur_count + static_cast<int>(fids.size()) + 1;
    if (needed > cur_len || panFIDList == nullptr) {
      cur_len = std::max(needed, cur_len * 2 + 8);
      panFIDList = static_cast<GIntBig*>(
          CPLRealloc(panFIDList, sizeof(GIntBig) * cur_len));
      if (nLength) *nLength = cur_len;
    }
    for (GIntBig fid : fids) {
      panFIDList[cur_count++] = fid;
    }
    panFIDList[cur_count] = OGRNullFID;
    if (nFIDCount) *nFIDCount = cur_count;
    return panFIDList;
  }

  OGRErr AddEntry(OGRField* psKey, GIntBig nFID) override {
    return OGRERR_NONE;
  }
  OGRErr RemoveEntry(OGRField* psKey, GIntBig nFID) override {
    return OGRERR_NONE;
  }
  OGRErr Clear() override { return OGRERR_NONE; }

 private:
  const std::vector<GIntBig>& GetNextFIDs() {
    if (queue_idx_ < queued_fids_.size()) {
      return queued_fids_[queue_idx_++];
    }
    return default_fids_;
  }

  std::vector<GIntBig> default_fids_;
  std::vector<std::vector<GIntBig>> queued_fids_;
  size_t queue_idx_ = 0;
};

class FakeLayerAttrIndex : public OGRLayerAttrIndex {
 public:
  FakeLayerAttrIndex() = default;
  ~FakeLayerAttrIndex() override = default;

  OGRErr Initialize(const char* pszIndexPath, OGRLayer*) override {
    return OGRERR_NONE;
  }
  OGRErr CreateIndex(int iField) override { return OGRERR_NONE; }
  OGRErr DropIndex(int iField) override { return OGRERR_NONE; }
  OGRErr IndexAllFeatures(int iField = -1) override { return OGRERR_NONE; }
  OGRErr AddToIndex(OGRFeature* poFeature, int iField = -1) override {
    return OGRERR_NONE;
  }
  OGRErr RemoveFromIndex(OGRFeature* poFeature) override { return OGRERR_NONE; }

  OGRAttrIndex* GetFieldIndex(int iField) override {
    auto it = indices_.find(iField);
    return it != indices_.end() ? it->second : nullptr;
  }

  void SetIndex(int iField, OGRAttrIndex* idx) { indices_[iField] = idx; }

 private:
  std::map<int, OGRAttrIndex*> indices_;
};

class FakeLayer : public OGRLayer {
 public:
  explicit FakeLayer(OGRFeatureDefn* poDefn) : poDefn_(poDefn) {}
  ~FakeLayer() override { m_poAttrIndex = nullptr; }

  void ResetReading() override {}
  OGRFeature* GetNextFeature() override { return nullptr; }
  const OGRFeatureDefn* GetLayerDefn() const override { return poDefn_; }
  OGRFeatureDefn* GetLayerDefn() { return poDefn_; }
  int TestCapability(const char* pszCap) const override {
    if (EQUAL(pszCap, OLCStringsAsUTF8)) return bUTF8_ ? TRUE : FALSE;
    return FALSE;
  }

  void SetUTF8(bool b) { bUTF8_ = b; }
  void SetFIDColumnName(std::string name) { fid_col_ = std::move(name); }
  const char* GetFIDColumn() const override { return fid_col_.c_str(); }

  void SetIndex(OGRLayerAttrIndex* poIndex) { m_poAttrIndex = poIndex; }

 private:
  OGRFeatureDefn* poDefn_;
  bool bUTF8_ = false;
  std::string fid_col_;
};

struct FeatureDefnReleaser {
  void operator()(OGRFeatureDefn* d) const {
    if (d) d->Release();
  }
};
using OGRFeatureDefnUniquePtr =
    std::unique_ptr<OGRFeatureDefn, FeatureDefnReleaser>;

OGRFeatureDefnUniquePtr CreateComprehensiveDefn() {
  auto poDefn = OGRFeatureDefnUniquePtr(new OGRFeatureDefn("comprehensive"));
  poDefn->Reference();

  OGRFieldDefn f_int("f_int", OFTInteger);
  poDefn->AddFieldDefn(&f_int);

  OGRFieldDefn f_bool("f_bool", OFTInteger);
  f_bool.SetSubType(OFSTBoolean);
  poDefn->AddFieldDefn(&f_bool);

  OGRFieldDefn f_int64("f_int64", OFTInteger64);
  poDefn->AddFieldDefn(&f_int64);

  OGRFieldDefn f_real("f_real", OFTReal);
  poDefn->AddFieldDefn(&f_real);

  OGRFieldDefn f_str("f_str", OFTString);
  poDefn->AddFieldDefn(&f_str);

  OGRFieldDefn f_date("f_date", OFTDate);
  poDefn->AddFieldDefn(&f_date);

  OGRFieldDefn f_time("f_time", OFTTime);
  poDefn->AddFieldDefn(&f_time);

  OGRFieldDefn f_datetime("f_datetime", OFTDateTime);
  poDefn->AddFieldDefn(&f_datetime);

  OGRFieldDefn f_bin("f_bin", OFTBinary);
  poDefn->AddFieldDefn(&f_bin);

  OGRGeomFieldDefn g_named("geom1", wkbPoint);
  poDefn->AddGeomFieldDefn(&g_named);

  OGRGeomFieldDefn g_unnamed("", wkbPolygon);
  poDefn->AddGeomFieldDefn(&g_unnamed);

  return poDefn;
}

TEST(OgrFeatureQueryTest, CompileAndRecompileAndInvalid) {
  WithQuietHandler quiet_handler;
  auto poDefn = CreateComprehensiveDefn();

  OGRFeatureQuery query;
  // Invalid expression
  EXPECT_EQ(OGRERR_CORRUPT_DATA,
            query.Compile(poDefn.get(), "INVALID SQL SYNTAX HERE !!!"));

  // Re-compiling on same query object (tests freeing previous pSWQExpr)
  EXPECT_EQ(OGRERR_NONE, query.Compile(poDefn.get(), "f_int = 10"));
  EXPECT_EQ(OGRERR_NONE, query.Compile(poDefn.get(), "f_str = 'hello'"));
}

TEST(OgrFeatureQueryTest, CompileWithLayerVariants) {
  auto poDefn = CreateComprehensiveDefn();

  FakeLayer layer(poDefn.get());
  layer.SetUTF8(true);
  layer.SetFIDColumnName("my_custom_fid");
  layer.SetMetadataItem(OLMD_FID64, "YES");

  OGRFeatureQuery query;
  EXPECT_EQ(OGRERR_NONE,
            query.Compile(&layer, "my_custom_fid = 100 AND f_bool = 1"));

  // Layer without FID64 metadata
  FakeLayer layer32(poDefn.get());
  layer32.SetFIDColumnName("my_custom_fid32");
  EXPECT_EQ(OGRERR_NONE,
            query.Compile(&layer32, "my_custom_fid32 = 100 AND f_int64 = 1"));

  // Layer with empty / "FID" column name
  FakeLayer layer_fid(poDefn.get());
  layer_fid.SetFIDColumnName("FID");
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer_fid, "FID = 50"));

  FakeLayer layer_empty(poDefn.get());
  layer_empty.SetFIDColumnName("");
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer_empty, "FID = 50"));
}

TEST(OgrFeatureQueryTest, EvaluateVariousFieldTypes) {
  auto poDefn = CreateComprehensiveDefn();

  OGRFeature feature(poDefn.get());
  feature.SetFID(42);
  feature.SetField("f_int", 123);
  feature.SetField("f_bool", 1);
  feature.SetField("f_int64", static_cast<GIntBig>(9876543210123LL));
  feature.SetField("f_real", 3.14159);
  feature.SetField("f_str", "test_val");
  feature.SetField("f_date", "2026/11/04");
  feature.SetField("f_time", "12:34:56");
  feature.SetField("f_datetime", "2026/11/04 12:34:56");

  OGRPoint pt(10.0, 20.0);
  feature.SetGeometry(&pt);
  feature.SetGeomField(poDefn->GetGeomFieldIndex("geom1"), &pt);

  feature.SetStyleString("PEN(c:#FF0000)");

  OGRFeatureQuery query;
  EXPECT_FALSE(query.Evaluate(&feature));  // pSWQExpr == nullptr

  // Matching integer and boolean
  EXPECT_EQ(OGRERR_NONE,
            query.Compile(poDefn.get(), "f_int = 123 AND f_bool = 1"));
  EXPECT_TRUE(query.Evaluate(&feature));

  // Non-matching int
  EXPECT_EQ(OGRERR_NONE, query.Compile(poDefn.get(), "f_int = 999"));
  EXPECT_FALSE(query.Evaluate(&feature));

  // Matching int64 and real
  EXPECT_EQ(
      OGRERR_NONE,
      query.Compile(poDefn.get(), "f_int64 = 9876543210123 AND f_real > 3.0"));
  EXPECT_TRUE(query.Evaluate(&feature));

  // Timestamp fields
  EXPECT_EQ(OGRERR_NONE,
            query.Compile(poDefn.get(),
                          "f_date = '2026/11/04' AND f_time = '12:34:56'"));
  EXPECT_TRUE(query.Evaluate(&feature));

  // String field
  EXPECT_EQ(OGRERR_NONE, query.Compile(poDefn.get(), "f_str = 'test_val'"));
  EXPECT_TRUE(query.Evaluate(&feature));

  // Special fields: FID, OGR_STYLE, OGR_GEOM_WKT
  EXPECT_EQ(OGRERR_NONE,
            query.Compile(poDefn.get(),
                          "FID = 42 AND OGR_STYLE = 'PEN(c:#FF0000)' AND "
                          "OGR_GEOM_WKT = 'POINT (10 20)'"));
  EXPECT_TRUE(query.Evaluate(&feature));

  // Geometry field (named)
  EXPECT_EQ(OGRERR_NONE, query.Compile(poDefn.get(), "geom1 IS NOT NULL"));
  EXPECT_TRUE(query.Evaluate(&feature));

  // Unset / null field
  EXPECT_EQ(OGRERR_NONE, query.Compile(poDefn.get(), "f_bin IS NULL"));
  EXPECT_TRUE(query.Evaluate(&feature));
}

TEST(OgrFeatureQueryTest, EvaluateCustomFIDColumn) {
  auto poDefn = CreateComprehensiveDefn();

  FakeLayer layer(poDefn.get());
  layer.SetFIDColumnName("custom_fid");
  layer.SetMetadataItem(OLMD_FID64, "YES");

  OGRFeature feature(poDefn.get());
  feature.SetFID(999);

  OGRFeatureQuery query;
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "custom_fid = 999"));
  EXPECT_TRUE(query.Evaluate(&feature));

  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "custom_fid = 888"));
  EXPECT_FALSE(query.Evaluate(&feature));
}

TEST(OgrFeatureQueryTest, CanUseIndex) {
  auto poDefn = CreateComprehensiveDefn();

  FakeLayer layer(poDefn.get());
  OGRFeatureQuery query;

  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int = 10"));
  // No index on layer
  EXPECT_FALSE(query.CanUseIndex(&layer));

  FakeLayerAttrIndex layer_index;
  FakeAttrIndex attr_index_int;
  layer_index.SetIndex(poDefn->GetFieldIndex("f_int"), &attr_index_int);
  layer.SetIndex(&layer_index);

  // Indexed field with EQ
  EXPECT_TRUE(query.CanUseIndex(&layer));

  // Indexed field with IN
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int IN (1, 2, 3)"));
  EXPECT_TRUE(query.CanUseIndex(&layer));

  // Unindexed field (f_real)
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_real = 3.14"));
  EXPECT_FALSE(query.CanUseIndex(&layer));

  // Compound OR / AND where both or one side is indexed
  FakeAttrIndex attr_index_real;
  layer_index.SetIndex(poDefn->GetFieldIndex("f_real"), &attr_index_real);

  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int = 10 OR f_real = 3.14"));
  EXPECT_TRUE(query.CanUseIndex(&layer));

  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int = 10 AND f_real = 3.14"));
  EXPECT_TRUE(query.CanUseIndex(&layer));

  // Compound where one side is unindexed
  EXPECT_EQ(OGRERR_NONE,
            query.Compile(&layer, "f_int = 10 AND f_str = 'hello'"));
  EXPECT_FALSE(query.CanUseIndex(&layer));

  // Unsupported operation (e.g. > or !=)
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int > 5"));
  EXPECT_FALSE(query.CanUseIndex(&layer));

  // Non-operation or empty query
  OGRFeatureQuery empty_query;
  EXPECT_FALSE(empty_query.CanUseIndex(&layer));
}

TEST(OgrFeatureQueryTest, EvaluateAgainstIndicesEqualityAndIn) {
  auto poDefn = CreateComprehensiveDefn();

  FakeLayer layer(poDefn.get());
  FakeLayerAttrIndex layer_index;
  FakeAttrIndex idx_int;
  FakeAttrIndex idx_int64;
  FakeAttrIndex idx_real;
  FakeAttrIndex idx_str;

  layer_index.SetIndex(poDefn->GetFieldIndex("f_int"), &idx_int);
  layer_index.SetIndex(poDefn->GetFieldIndex("f_int64"), &idx_int64);
  layer_index.SetIndex(poDefn->GetFieldIndex("f_real"), &idx_real);
  layer_index.SetIndex(poDefn->GetFieldIndex("f_str"), &idx_str);
  layer.SetIndex(&layer_index);

  OGRFeatureQuery query;

  // Layer without index returns nullptr
  FakeLayer layer_no_index(poDefn.get());
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer_no_index, "f_int = 1"));
  OGRErr err = OGRERR_NONE;
  EXPECT_EQ(nullptr, query.EvaluateAgainstIndices(&layer_no_index, &err));
  EXPECT_EQ(OGRERR_NONE, err);

  // EQ on f_int (integer constant and float constant)
  idx_int.SetMatches({20, 10});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int = 1"));
  GIntBig* fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  EXPECT_EQ(fids[0], 10);
  EXPECT_EQ(fids[1], 20);
  EXPECT_EQ(fids[2], OGRNullFID);
  CPLFree(fids);

  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int = 1.0"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  CPLFree(fids);

  // EQ on f_int64 (int constant and float constant)
  idx_int64.SetMatches({100});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int64 = 1000"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  EXPECT_EQ(fids[0], 100);
  EXPECT_EQ(fids[1], OGRNullFID);
  CPLFree(fids);

  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int64 = 1000.0"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  CPLFree(fids);

  // EQ on f_real
  idx_real.SetMatches({50});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_real = 3.14"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  EXPECT_EQ(fids[0], 50);
  CPLFree(fids);

  // EQ on f_str
  idx_str.SetMatches({77});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_str = 'value'"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  EXPECT_EQ(fids[0], 77);
  CPLFree(fids);

  // IN on f_int (with float and int)
  idx_int.SetMatches({});
  idx_int.QueueMatches({5});
  idx_int.QueueMatches({30});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int IN (1, 2.0)"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  EXPECT_EQ(fids[0], 5);
  EXPECT_EQ(fids[1], 30);
  EXPECT_EQ(fids[2], OGRNullFID);
  CPLFree(fids);

  // IN on f_int64 (with float and int)
  idx_int64.SetMatches({40});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int64 IN (10, 20.0)"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  CPLFree(fids);

  // IN on f_real
  idx_real.SetMatches({60});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_real IN (1.1, 2.2)"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  CPLFree(fids);

  // IN on f_str
  idx_str.SetMatches({80});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_str IN ('a', 'b')"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  CPLFree(fids);

  // Unindexed field returns nullptr
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_bin = 1"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  EXPECT_EQ(nullptr, fids);
}

TEST(OgrFeatureQueryTest, EvaluateAgainstIndicesCompoundOrAndAnd) {
  auto poDefn = CreateComprehensiveDefn();

  FakeLayer layer(poDefn.get());
  FakeLayerAttrIndex layer_index;
  FakeAttrIndex idx1;
  FakeAttrIndex idx2;

  layer_index.SetIndex(poDefn->GetFieldIndex("f_int"), &idx1);
  layer_index.SetIndex(poDefn->GetFieldIndex("f_real"), &idx2);
  layer.SetIndex(&layer_index);

  OGRFeatureQuery query;
  OGRErr err = OGRERR_NONE;

  // Interleaved arrays to hit all branches of OGRORGIntBigArray:
  // List1: { 1, 2, 5, 8, 9, 15 }
  // List2: { 3, 4, 5, 6, 7, 10, 11 }
  // Tests:
  // - nVal1 < nVal2 with next val <= nVal2 (1, 2 vs 3)
  // - nVal1 < nVal2 else branch
  // - nVal1 == nVal2 (5 == 5)
  // - nVal1 > nVal2 with next val <= nVal1 (6, 7 vs 8)
  // - nVal1 > nVal2 else branch
  // - trailing elements in list1 (15) and list2
  idx1.SetMatches({1, 2, 5, 8, 9, 15});
  idx2.SetMatches({3, 4, 5, 6, 7, 10, 11});

  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int = 1 OR f_real = 1.0"));
  GIntBig* fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  EXPECT_EQ(fids[0], 1);
  EXPECT_EQ(fids[1], 2);
  EXPECT_EQ(fids[2], 3);
  EXPECT_EQ(fids[3], 4);
  EXPECT_EQ(fids[4], 5);
  EXPECT_EQ(fids[5], 6);
  EXPECT_EQ(fids[6], 7);
  EXPECT_EQ(fids[7], 8);
  EXPECT_EQ(fids[8], 9);
  EXPECT_EQ(fids[9], 10);
  EXPECT_EQ(fids[10], 11);
  EXPECT_EQ(fids[11], 15);
  EXPECT_EQ(fids[12], OGRNullFID);
  CPLFree(fids);

  // Test trailing elements in list2 for OR
  idx1.SetMatches({1});
  idx2.SetMatches({2, 3});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int = 1 OR f_real = 1.0"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  EXPECT_EQ(fids[0], 1);
  EXPECT_EQ(fids[1], 2);
  EXPECT_EQ(fids[2], 3);
  EXPECT_EQ(fids[3], OGRNullFID);
  CPLFree(fids);

  // Test OGRANDGIntBigArray with matching and non-matching elements
  idx1.SetMatches({1, 2, 5, 8, 9, 12});
  idx2.SetMatches({2, 4, 5, 7, 9, 15});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int = 1 AND f_real = 1.0"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  EXPECT_EQ(fids[0], 2);
  EXPECT_EQ(fids[1], 5);
  EXPECT_EQ(fids[2], 9);
  EXPECT_EQ(fids[3], OGRNullFID);
  CPLFree(fids);

  // AND where one side has no match
  idx1.SetMatches({1, 2});
  idx2.SetMatches({3, 4});
  EXPECT_EQ(OGRERR_NONE, query.Compile(&layer, "f_int = 1 AND f_real = 1.0"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  ASSERT_THAT(fids, NotNull());
  EXPECT_EQ(fids[0], OGRNullFID);
  CPLFree(fids);

  // OR where one side fails to evaluate against index (e.g. unindexed field)
  EXPECT_EQ(OGRERR_NONE,
            query.Compile(&layer, "f_int = 1 OR f_str = 'unindexed'"));
  fids = query.EvaluateAgainstIndices(&layer, &err);
  EXPECT_EQ(nullptr, fids);
}

TEST(OgrFeatureQueryTest, GetUsedFields) {
  auto poDefn = CreateComprehensiveDefn();

  OGRFeatureQuery query;
  EXPECT_EQ(nullptr, query.GetUsedFields());  // Null expr

  FakeLayer layer(poDefn.get());
  layer.SetFIDColumnName("custom_fid");
  layer.SetMetadataItem(OLMD_FID64, "YES");

  EXPECT_EQ(
      OGRERR_NONE,
      query.Compile(&layer,
                    "f_int = 1 AND (f_real > 2.0 OR f_str = 'val') AND "
                    "FID = 10 AND OGR_GEOMETRY = 'PT' AND OGR_STYLE = 's' "
                    "AND OGR_GEOM_WKT = 'w' AND OGR_GEOM_AREA > 0 AND "
                    "custom_fid = 99"));

  char** fields = query.GetUsedFields();
  ASSERT_THAT(fields, NotNull());
  EXPECT_NE(-1, CSLFindString(fields, "f_int"));
  EXPECT_NE(-1, CSLFindString(fields, "f_real"));
  EXPECT_NE(-1, CSLFindString(fields, "f_str"));
  EXPECT_NE(-1, CSLFindString(fields, "FID"));
  EXPECT_NE(-1, CSLFindString(fields, "OGR_GEOMETRY"));
  EXPECT_NE(-1, CSLFindString(fields, "OGR_STYLE"));
  EXPECT_NE(-1, CSLFindString(fields, "OGR_GEOM_WKT"));
  EXPECT_NE(-1, CSLFindString(fields, "OGR_GEOM_AREA"));
  CSLDestroy(fields);
}

}  // namespace
}  // namespace autotest2
