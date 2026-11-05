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
// Tests for GMLReadState (gmlreadstate.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gml/gmlreadstate.cpp

#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gml/gmlreaderp.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlfeature.h"

namespace autotest2 {
namespace {

TEST(GMLReadStateTest, DefaultState) {
  GMLReadState state;
  EXPECT_STREQ("", state.GetLastComponent());
  EXPECT_EQ(0, state.GetLastComponentLen());
  EXPECT_TRUE(state.osPath.empty());
  EXPECT_EQ(nullptr, state.m_poFeature);
  EXPECT_EQ(nullptr, state.m_poParentState);
}

TEST(GMLReadStateTest, PushPathDefaultLenAndPop) {
  GMLReadState state;

  // 1. Push single component with default nLen = -1.
  state.PushPath("FeatureCollection");
  EXPECT_STREQ("FeatureCollection", state.GetLastComponent());
  EXPECT_EQ(17, state.GetLastComponentLen());
  EXPECT_EQ("FeatureCollection", state.osPath);

  // 2. Push second component.
  state.PushPath("featureMember");
  EXPECT_STREQ("featureMember", state.GetLastComponent());
  EXPECT_EQ(13, state.GetLastComponentLen());
  EXPECT_EQ("FeatureCollection|featureMember", state.osPath);

  // 3. Pop second component.
  state.PopPath();
  EXPECT_STREQ("FeatureCollection", state.GetLastComponent());
  EXPECT_EQ(17, state.GetLastComponentLen());
  EXPECT_EQ("FeatureCollection", state.osPath);

  // 4. Pop first component back to empty.
  state.PopPath();
  EXPECT_STREQ("", state.GetLastComponent());
  EXPECT_EQ(0, state.GetLastComponentLen());
  EXPECT_TRUE(state.osPath.empty());
}

TEST(GMLReadStateTest, PushPathExplicitLenAndMultiplePop) {
  GMLReadState state;

  // 1. Push first component (vector size grows; pushes full string).
  state.PushPath("root");
  EXPECT_STREQ("root", state.GetLastComponent());
  EXPECT_EQ(4, state.GetLastComponentLen());
  EXPECT_EQ("root", state.osPath);

  // 2. Push second component.
  state.PushPath("child");
  EXPECT_STREQ("child", state.GetLastComponent());
  EXPECT_EQ(5, state.GetLastComponentLen());
  EXPECT_EQ("root|child", state.osPath);

  // 3. Push third component.
  state.PushPath("leaf");
  EXPECT_STREQ("leaf", state.GetLastComponent());
  EXPECT_EQ(4, state.GetLastComponentLen());
  EXPECT_EQ("root|child|leaf", state.osPath);

  // 4. Pop all components one by one.
  state.PopPath();
  EXPECT_EQ("root|child", state.osPath);
  state.PopPath();
  EXPECT_EQ("root", state.osPath);
  state.PopPath();
  EXPECT_TRUE(state.osPath.empty());
}

TEST(GMLReadStateTest, ResetAndReuseComponents) {
  GMLReadState state;

  // Populate two components so aosPathComponents capacity/size is 2.
  state.PushPath("first");
  state.PushPath("second");
  EXPECT_EQ("first|second", state.osPath);

  // Reset clears osPath and resets m_nPathLength to 0 while keeping capacity.
  state.Reset();
  EXPECT_TRUE(state.osPath.empty());
  EXPECT_STREQ("", state.GetLastComponent());
  EXPECT_EQ(0, state.GetLastComponentLen());

  // Push with nLen >= 0 when m_nPathLength < aosPathComponents.size() (lines
  // 48-54).
  constexpr std::string_view kNewFirst = "alpha_extra";
  state.PushPath(kNewFirst.data(), 5);  // "alpha"
  EXPECT_STREQ("alpha", state.GetLastComponent());
  EXPECT_EQ("alpha", state.osPath);

  // Push with nLen < 0 when m_nPathLength < aosPathComponents.size() (lines
  // 55-59).
  state.PushPath("beta");
  EXPECT_STREQ("beta", state.GetLastComponent());
  EXPECT_EQ("alpha|beta", state.osPath);

  // Push beyond previous size (lines 61-65).
  state.PushPath("gamma");
  EXPECT_STREQ("gamma", state.GetLastComponent());
  EXPECT_EQ("alpha|beta|gamma", state.osPath);
}

TEST(GMLReadStateTest, ResetClearsPointers) {
  GMLReadState state;
  GMLFeature feature(nullptr);
  GMLReadState parent;

  state.m_poFeature = &feature;
  state.m_poParentState = &parent;
  state.PushPath("element");

  state.Reset();
  EXPECT_EQ(nullptr, state.m_poFeature);
  EXPECT_EQ(nullptr, state.m_poParentState);
  EXPECT_TRUE(state.osPath.empty());
}

}  // namespace
}  // namespace autotest2
