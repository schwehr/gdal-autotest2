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

// Tests for OGRAttrIndex and OGRLayerAttrIndex.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogr_attrind.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogr_attrind.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/ogr_attrind.h"

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

class ConcreteAttrIndex : public OGRAttrIndex {
 public:
  ConcreteAttrIndex() = default;
  ~ConcreteAttrIndex() override = default;

  GIntBig GetFirstMatch(OGRField*) override { return OGRNullFID; }
  GIntBig* GetAllMatches(OGRField*) override { return nullptr; }
  GIntBig* GetAllMatches(OGRField*, GIntBig*, int*, int*) override {
    return nullptr;
  }
  OGRErr AddEntry(OGRField*, GIntBig) override { return OGRERR_NONE; }
  OGRErr RemoveEntry(OGRField*, GIntBig) override { return OGRERR_NONE; }
  OGRErr Clear() override { return OGRERR_NONE; }
};

class ConcreteLayerAttrIndex : public OGRLayerAttrIndex {
 public:
  ConcreteLayerAttrIndex() {
    poLayer = nullptr;
    pszIndexPath = CPLStrdup("/vsimem/dummy_path.idm");
  }
  ~ConcreteLayerAttrIndex() override = default;

  OGRErr Initialize(const char*, OGRLayer*) override { return OGRERR_NONE; }
  OGRErr CreateIndex(int) override { return OGRERR_NONE; }
  OGRErr DropIndex(int) override { return OGRERR_NONE; }
  OGRErr IndexAllFeatures(int) override { return OGRERR_NONE; }
  OGRErr AddToIndex(OGRFeature*, int) override { return OGRERR_NONE; }
  OGRErr RemoveFromIndex(OGRFeature*) override { return OGRERR_NONE; }
  OGRAttrIndex* GetFieldIndex(int) override { return nullptr; }
};

TEST(OGRAttrIndexTest, BaseAttrIndexLifecycle) {
  auto index = std::make_unique<ConcreteAttrIndex>();
  ASSERT_NE(nullptr, index);
  EXPECT_EQ(OGRNullFID, index->GetFirstMatch(nullptr));
  index.reset();
}

TEST(OGRLayerAttrIndexTest, BaseLayerAttrIndexLifecycle) {
  auto layer_index = std::make_unique<ConcreteLayerAttrIndex>();
  ASSERT_NE(nullptr, layer_index);
  EXPECT_EQ(OGRERR_NONE, layer_index->CreateIndex(0));
  // Destructor frees pszIndexPath and exercises ~OGRLayerAttrIndex().
  layer_index.reset();
}

// TODO: b/335317901 - Cover remaining error branches in ogr_attrind.cpp.

}  // namespace
}  // namespace autotest2
