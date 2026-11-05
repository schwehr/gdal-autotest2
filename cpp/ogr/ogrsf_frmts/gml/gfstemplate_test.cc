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
// Tests for GFSTemplateList and GMLReader template prescanning
// (gfstemplate.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gml/gfstemplate.cpp

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gml/gmlreaderp.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlfeature.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlutils.h"

namespace autotest2 {
namespace {

TEST(GFSTemplateTest, ListSequentialAndNonSequential) {
  GFSTemplateList list;
  EXPECT_TRUE(list.HaveSequentialLayers());
  EXPECT_EQ(0, list.GetClassCount());
  EXPECT_EQ(nullptr, list.GetFirst());

  // 1. First item inserted.
  list.Update("LayerA", /*bHasGeom=*/true);
  EXPECT_TRUE(list.HaveSequentialLayers());
  EXPECT_EQ(1, list.GetClassCount());
  EXPECT_NE(nullptr, list.GetFirst());

  // 2. Same item as last item (sequential).
  list.Update("LayerA", /*bHasGeom=*/false);
  EXPECT_TRUE(list.HaveSequentialLayers());
  EXPECT_EQ(1, list.GetClassCount());

  // 3. Second item appended (still sequential).
  list.Update("LayerB", /*bHasGeom=*/true);
  EXPECT_TRUE(list.HaveSequentialLayers());
  EXPECT_EQ(2, list.GetClassCount());

  // 4. First item updated again -> triggers non-sequential layer detection.
  list.Update("LayerA", /*bHasGeom=*/true);
  EXPECT_FALSE(list.HaveSequentialLayers());
  EXPECT_EQ(2, list.GetClassCount());
}

TEST(GFSTemplateTest, PrescanForTemplateShouldLookForClassAtAnyLevel) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  // When a feature class has feature properties, AddClass sets
  // m_bLookForClassAtAnyLevel. Then PrescanForTemplate returns true early.
  auto* poClass = new GMLFeatureClass("AnyLevelClass");
  auto* poProp = new GMLPropertyDefn("rel", "rel");
  poProp->SetType(GMLPT_FeatureProperty);
  poClass->AddProperty(poProp);
  reader->AddClass(poClass);

  EXPECT_TRUE(reader->ShouldLookForClassAtAnyLevel());
  EXPECT_TRUE(reader->PrescanForTemplate());
}

TEST(GFSTemplateTest, PrescanForTemplateNullFileFails) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile("/vsimem/gfstemplate_test/nonexistent.gml");

  WithQuietHandler quiet_handler;
  EXPECT_FALSE(reader->PrescanForTemplate());
}

constexpr std::string_view kSequentialGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml">
  <ClassA>
    <gml:Point><gml:coordinates>1,2</gml:coordinates></gml:Point>
    <attr>val1</attr>
  </ClassA>
  <ClassA>
    <gml:Point><gml:coordinates>3,4</gml:coordinates></gml:Point>
    <attr>val2</attr>
  </ClassA>
  <ClassB>
    <attr>no_geom</attr>
  </ClassB>
</root>
)xml";

TEST(GFSTemplateTest, PrescanForTemplateSequentialReordersAndRemovesUnused) {
  constexpr char kPath[] = "/vsimem/gfstemplate_test/seq_features.gml";
  VsiMemTempWrapper file(kPath, std::string(kSequentialGml));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  // Register classes in reverse order: ClassB, ClassA, and UnusedClass.
  auto* class_b = new GMLFeatureClass("ClassB");
  reader->AddClass(class_b);

  auto* class_a = new GMLFeatureClass("ClassA");
  reader->AddClass(class_a);

  auto* class_unused = new GMLFeatureClass("UnusedClass");
  reader->AddClass(class_unused);

  reader->SetClassListLocked(true);

  EXPECT_TRUE(reader->PrescanForTemplate());

  // Sequential layers detected, so classes were re-ordered: ClassA first, then
  // ClassB. UnusedClass was removed.
  EXPECT_EQ(2, reader->GetClassCount());
  EXPECT_STREQ("ClassA", reader->GetClass(0)->GetName());
  EXPECT_EQ(2, reader->GetClass(0)->GetFeatureCount());
  EXPECT_EQ(1, reader->GetClass(0)->GetGeometryPropertyCount());

  EXPECT_STREQ("ClassB", reader->GetClass(1)->GetName());
  EXPECT_EQ(1, reader->GetClass(1)->GetFeatureCount());
  EXPECT_EQ(0, reader->GetClass(1)->GetGeometryPropertyCount());
}

constexpr std::string_view kNonSequentialGml = R"xml(
<root xmlns:gml="http://www.opengis.net/gml">
  <ClassA>
    <attr>val1</attr>
  </ClassA>
  <ClassB>
    <attr>val2</attr>
  </ClassB>
  <ClassA>
    <attr>val3</attr>
  </ClassA>
</root>
)xml";

TEST(GFSTemplateTest, PrescanForTemplateNonSequential) {
  constexpr char kPath[] = "/vsimem/gfstemplate_test/nonseq_features.gml";
  VsiMemTempWrapper file(kPath, std::string(kNonSequentialGml));

  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);
  reader->SetSourceFile(kPath);

  auto* class_a = new GMLFeatureClass("ClassA");
  reader->AddClass(class_a);
  auto* class_b = new GMLFeatureClass("ClassB");
  reader->AddClass(class_b);

  reader->SetClassListLocked(true);

  EXPECT_TRUE(reader->PrescanForTemplate());
  EXPECT_EQ(2, reader->GetClass(0)->GetFeatureCount());
  EXPECT_EQ(1, reader->GetClass(1)->GetFeatureCount());
}

}  // namespace
}  // namespace autotest2
