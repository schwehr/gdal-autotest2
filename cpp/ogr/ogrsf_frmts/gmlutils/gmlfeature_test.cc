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
// Tests for GMLFeature (ogr/ogrsf_frmts/gmlutils/gmlfeature.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gmlutils/gmlfeature.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gml.py

#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlfeature.h"

#include <cstdio>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_minixml.h"

namespace autotest2 {
namespace {

TEST(GmlFeatureTest, BasicAndFid) {
  GMLFeatureClass fc("test_class");
  GMLFeature feature(&fc);

  EXPECT_EQ(&fc, feature.GetClass());
  EXPECT_EQ(nullptr, feature.GetFID());
  EXPECT_EQ(0, feature.GetGeometryCount());
  EXPECT_EQ(nullptr, feature.GetGeometryRef(-1));
  EXPECT_EQ(nullptr, feature.GetGeometryRef(0));
  EXPECT_EQ(nullptr, feature.GetProperty(-1));
  EXPECT_EQ(nullptr, feature.GetProperty(0));
  EXPECT_EQ(nullptr, feature.GetBoundedByGeometry());

  feature.SetFID("fid_1");
  EXPECT_STREQ("fid_1", feature.GetFID());

  feature.SetFID("fid_2");
  EXPECT_STREQ("fid_2", feature.GetFID());

  feature.SetFID(nullptr);
  EXPECT_EQ(nullptr, feature.GetFID());
}

TEST(GmlFeatureTest, PropertyLifecycleAndReallocation) {
  GMLFeatureClass fc("test_class");
  auto prop0 = std::make_unique<GMLPropertyDefn>("prop0", "elem0");
  fc.AddProperty(prop0.release());

  GMLFeature feature(&fc);

  // 1. Set property on index 0 with 1 subproperty.
  feature.SetPropertyDirectly(0, CPLStrdup("val0_1"));
  const GMLProperty* p0 = feature.GetProperty(0);
  ASSERT_NE(nullptr, p0);
  EXPECT_EQ(1, p0->nSubProperties);
  EXPECT_STREQ("val0_1", p0->papszSubProperties[0]);

  // 2. Add more properties to class to trigger reallocation in
  // SetPropertyDirectly.
  auto prop1 = std::make_unique<GMLPropertyDefn>("prop1", "elem1");
  auto prop2 = std::make_unique<GMLPropertyDefn>("prop2", "elem2");
  fc.AddProperty(prop1.release());
  fc.AddProperty(prop2.release());

  // 3. Set property at index 2 (requires reallocating m_pasProperties).
  feature.SetPropertyDirectly(2, CPLStrdup("val2_1"));
  const GMLProperty* p2 = feature.GetProperty(2);
  ASSERT_NE(nullptr, p2);
  EXPECT_EQ(1, p2->nSubProperties);
  EXPECT_STREQ("val2_1", p2->papszSubProperties[0]);

  // Re-verify p0 after reallocation.
  p0 = feature.GetProperty(0);
  ASSERT_NE(nullptr, p0);
  EXPECT_EQ(1, p0->nSubProperties);
  EXPECT_STREQ("val0_1", p0->papszSubProperties[0]);

  // Verify index 1 has 0 subproperties.
  const GMLProperty* p1 = feature.GetProperty(1);
  ASSERT_NE(nullptr, p1);
  EXPECT_EQ(0, p1->nSubProperties);

  // 4. Add second and third subproperties to index 2 (nSubProperties == 1 -> 2
  // -> 3).
  feature.SetPropertyDirectly(2, CPLStrdup("val2_2"));
  EXPECT_EQ(2, p2->nSubProperties);
  EXPECT_STREQ("val2_1", p2->papszSubProperties[0]);
  EXPECT_STREQ("val2_2", p2->papszSubProperties[1]);

  feature.SetPropertyDirectly(2, CPLStrdup("val2_3"));
  EXPECT_EQ(3, p2->nSubProperties);
  EXPECT_STREQ("val2_1", p2->papszSubProperties[0]);
  EXPECT_STREQ("val2_2", p2->papszSubProperties[1]);
  EXPECT_STREQ("val2_3", p2->papszSubProperties[2]);

  // 5. Add property 3 to class and set property 3 to test reallocation when
  // existing properties have both <= 1 and > 1 subproperties.
  auto prop3 = std::make_unique<GMLPropertyDefn>("prop3", "elem3");
  fc.AddProperty(prop3.release());
  feature.SetPropertyDirectly(3, CPLStrdup("val3_1"));

  const GMLProperty* p3 = feature.GetProperty(3);
  ASSERT_NE(nullptr, p3);
  EXPECT_EQ(1, p3->nSubProperties);
  EXPECT_STREQ("val3_1", p3->papszSubProperties[0]);

  // Destruction cleans up: p0 (1 subprop), p1 (0 subprop), p2 (3 subprops), p3
  // (1 subprop).
}

TEST(GmlFeatureTest, SetGeometryDirectlySingle) {
  GMLFeatureClass fc("test_class");
  GMLFeature feature(&fc);

  CPLXMLNode* geom1 = CPLCreateXMLNode(nullptr, CXT_Element, "Point");
  feature.SetGeometryDirectly(geom1);
  EXPECT_EQ(1, feature.GetGeometryCount());
  EXPECT_EQ(geom1, feature.GetGeometryRef(0));

  CPLXMLNode* geom2 = CPLCreateXMLNode(nullptr, CXT_Element, "LineString");
  feature.SetGeometryDirectly(geom2);  // Destroys geom1.
  EXPECT_EQ(1, feature.GetGeometryCount());
  EXPECT_EQ(geom2, feature.GetGeometryRef(0));
}

TEST(GmlFeatureTest, SetGeometryDirectlyIndexed) {
  GMLFeatureClass fc("test_class");
  GMLFeature feature(&fc);

  // 1. nIdx == 0 when count <= 1 delegates to single-arg SetGeometryDirectly.
  CPLXMLNode* geom0 = CPLCreateXMLNode(nullptr, CXT_Element, "Point");
  feature.SetGeometryDirectly(0, geom0);
  EXPECT_EQ(1, feature.GetGeometryCount());
  EXPECT_EQ(geom0, feature.GetGeometryRef(0));

  // 2. nIdx > 0 when count <= 1 allocates heap array and sets at nIdx.
  CPLXMLNode* geom2 = CPLCreateXMLNode(nullptr, CXT_Element, "Polygon");
  feature.SetGeometryDirectly(2, geom2);
  EXPECT_EQ(3, feature.GetGeometryCount());
  EXPECT_EQ(geom0, feature.GetGeometryRef(0));
  EXPECT_EQ(nullptr, feature.GetGeometryRef(1));
  EXPECT_EQ(geom2, feature.GetGeometryRef(2));

  // 3. Overwrite existing geometry at index 2.
  CPLXMLNode* geom2_new = CPLCreateXMLNode(nullptr, CXT_Element, "MultiPoint");
  feature.SetGeometryDirectly(2, geom2_new);
  EXPECT_EQ(3, feature.GetGeometryCount());
  EXPECT_EQ(geom2_new, feature.GetGeometryRef(2));

  // 4. Overwrite at index 0 when count > 1.
  CPLXMLNode* geom0_new =
      CPLCreateXMLNode(nullptr, CXT_Element, "MultiLineString");
  feature.SetGeometryDirectly(0, geom0_new);
  EXPECT_EQ(3, feature.GetGeometryCount());
  EXPECT_EQ(geom0_new, feature.GetGeometryRef(0));

  // 5. Expand beyond current count.
  CPLXMLNode* geom4 = CPLCreateXMLNode(nullptr, CXT_Element, "MultiPolygon");
  feature.SetGeometryDirectly(4, geom4);
  EXPECT_EQ(5, feature.GetGeometryCount());
  EXPECT_EQ(geom4, feature.GetGeometryRef(4));

  EXPECT_EQ(nullptr, feature.GetGeometryRef(-1));
  EXPECT_EQ(nullptr, feature.GetGeometryRef(5));
}

TEST(GmlFeatureTest, AddGeometryMultiple) {
  GMLFeatureClass fc("test_class");
  GMLFeature feature(&fc);

  CPLXMLNode* geom0 = CPLCreateXMLNode(nullptr, CXT_Element, "Geom0");
  CPLXMLNode* geom1 = CPLCreateXMLNode(nullptr, CXT_Element, "Geom1");
  CPLXMLNode* geom2 = CPLCreateXMLNode(nullptr, CXT_Element, "Geom2");

  feature.AddGeometry(geom0);
  EXPECT_EQ(1, feature.GetGeometryCount());
  EXPECT_EQ(geom0, feature.GetGeometryRef(0));

  feature.AddGeometry(geom1);
  EXPECT_EQ(2, feature.GetGeometryCount());
  EXPECT_EQ(geom0, feature.GetGeometryRef(0));
  EXPECT_EQ(geom1, feature.GetGeometryRef(1));

  feature.AddGeometry(geom2);
  EXPECT_EQ(3, feature.GetGeometryCount());
  EXPECT_EQ(geom0, feature.GetGeometryRef(0));
  EXPECT_EQ(geom1, feature.GetGeometryRef(1));
  EXPECT_EQ(geom2, feature.GetGeometryRef(2));

  const CPLXMLNode* const* list = feature.GetGeometryList();
  ASSERT_NE(nullptr, list);
  EXPECT_EQ(geom0, list[0]);
  EXPECT_EQ(geom1, list[1]);
  EXPECT_EQ(geom2, list[2]);
  EXPECT_EQ(nullptr, list[3]);
}

TEST(GmlFeatureTest, BoundedByGeometry) {
  GMLFeatureClass fc("test_class");
  GMLFeature feature(&fc);

  CPLXMLNode* bbox1 = CPLCreateXMLNode(nullptr, CXT_Element, "Box");
  feature.SetBoundedByGeometry(bbox1);
  EXPECT_EQ(bbox1, feature.GetBoundedByGeometry());

  CPLXMLNode* bbox2 = CPLCreateXMLNode(nullptr, CXT_Element, "Envelope");
  feature.SetBoundedByGeometry(bbox2);  // Destroys bbox1.
  EXPECT_EQ(bbox2, feature.GetBoundedByGeometry());
}

TEST(GmlFeatureTest, Dump) {
  GMLFeatureClass fc("test_class");
  auto prop0 = std::make_unique<GMLPropertyDefn>("prop0", "elem0");
  auto prop1 = std::make_unique<GMLPropertyDefn>("prop1", "elem1");
  fc.AddProperty(prop0.release());
  fc.AddProperty(prop1.release());

  GMLFeature feature(&fc);
  feature.SetFID("test_fid");
  feature.SetPropertyDirectly(0, CPLStrdup("val0_single"));
  feature.SetPropertyDirectly(1, CPLStrdup("val1_multi1"));
  feature.SetPropertyDirectly(1, CPLStrdup("val1_multi2"));

  CPLXMLNode* geom = CPLCreateXMLNode(nullptr, CXT_Element, "gml:Point");
  feature.SetGeometryDirectly(geom);

  // Upstream quirk: GMLFeature::Dump(FILE* fp) ignores fp and always prints
  // directly to stdout using printf.
  testing::internal::CaptureStdout();
  feature.Dump(stdout);
  std::string output = testing::internal::GetCapturedStdout();
  EXPECT_THAT(output, testing::HasSubstr("GMLFeature(test_class):"));
  EXPECT_THAT(output, testing::HasSubstr("  FID = test_fid"));
  EXPECT_THAT(output, testing::HasSubstr("  prop0 = val0_single"));
  EXPECT_THAT(output, testing::HasSubstr("  prop1 = val1_multi1, val1_multi2"));
  EXPECT_THAT(output, testing::HasSubstr("<gml:Point />"));

  // Dump without FID.
  testing::internal::CaptureStdout();
  GMLFeature feature_no_fid(&fc);
  feature_no_fid.Dump(stdout);
  std::string output_no_fid = testing::internal::GetCapturedStdout();
  EXPECT_THAT(output_no_fid, testing::HasSubstr("GMLFeature(test_class):"));
  EXPECT_THAT(output_no_fid, testing::Not(testing::HasSubstr("FID =")));
}

// TODO: b/335317901 - GMLFeature achieves 100% line coverage. Track any
// upstream additions to GMLFeature methods.

}  // namespace
}  // namespace autotest2
