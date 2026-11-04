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

// Tests for OGRLayerWithTranslateFeature.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrlayerwithtranslatefeature.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrlayerwithtranslatefeature.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/generic/ogrlayerwithtranslatefeature.h"

#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"

namespace autotest2 {
namespace {

class TestTranslateLayer : public OGRLayerWithTranslateFeature {
 public:
  TestTranslateLayer() : feature_defn_(new OGRFeatureDefn("test_layer")) {
    feature_defn_->Reference();
  }
  ~TestTranslateLayer() override {
    if (feature_defn_) {
      feature_defn_->Release();
    }
  }

  void ResetReading() override {}
  OGRFeature* GetNextFeature() override { return nullptr; }
  const OGRFeatureDefn* GetLayerDefn() const override { return feature_defn_; }
  int TestCapability(const char*) const override { return FALSE; }

  void TranslateFeature(
      std::unique_ptr<OGRFeature> poSrcFeature,
      std::vector<std::unique_ptr<OGRFeature>>& apoOutFeatures) override {
    if (poSrcFeature != nullptr) {
      apoOutFeatures.push_back(std::move(poSrcFeature));
    }
  }

 private:
  OGRFeatureDefn* feature_defn_ = nullptr;
};

TEST(OGRLayerWithTranslateFeatureTest, BasicLifecycle) {
  auto layer = std::make_unique<TestTranslateLayer>();
  ASSERT_NE(nullptr, layer);

  auto defn = layer->GetLayerDefn();
  ASSERT_NE(nullptr, defn);

  auto feat = std::make_unique<OGRFeature>(const_cast<OGRFeatureDefn*>(defn));
  feat->SetFID(42);

  std::vector<std::unique_ptr<OGRFeature>> out_features;
  layer->TranslateFeature(std::move(feat), out_features);
  ASSERT_EQ(1, out_features.size());
  EXPECT_EQ(42, out_features[0]->GetFID());

  out_features.clear();
  layer.reset();
}

// TODO: b/335317901 - Cover remaining error branches in
// ogrlayerwithtranslatefeature.cpp.

}  // namespace
}  // namespace autotest2
