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

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class TestMultiCurve : public OGRMultiCurve {
 public:
  using OGRMultiCurve::isCompatibleSubType;
};

TEST(OgrMultiCurveTest, BasicProperties) {
  TestMultiCurve mc;
  EXPECT_STREQ("MULTICURVE", mc.getGeometryName());
  EXPECT_EQ(1, mc.getDimension());
  EXPECT_EQ(wkbMultiCurve, mc.getGeometryType());

  mc.set3D(TRUE);
  EXPECT_EQ(wkbMultiCurveZ, mc.getGeometryType());

  mc.setMeasured(TRUE);
  EXPECT_EQ(wkbMultiCurveZM, mc.getGeometryType());

  mc.set3D(FALSE);
  EXPECT_EQ(wkbMultiCurveM, mc.getGeometryType());

  EXPECT_TRUE(mc.isCompatibleSubType(wkbLineString));
  EXPECT_TRUE(mc.isCompatibleSubType(wkbCircularString));
  EXPECT_TRUE(mc.isCompatibleSubType(wkbCompoundCurve));
  EXPECT_FALSE(mc.isCompatibleSubType(wkbPolygon));
}

TEST(OgrMultiCurveTest, HasCurveGeometry) {
  OGRMultiCurve mc;
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  mc.addGeometry(ls.get());

  // Without looking for non-linear, always returns true
  EXPECT_TRUE(mc.hasCurveGeometry(FALSE));
  // With only linear geometries, looking for non-linear returns false
  EXPECT_FALSE(mc.hasCurveGeometry(TRUE));

  auto cs = std::make_unique<OGRCircularString>();
  cs->addPoint(0, 0);
  cs->addPoint(1, 1);
  cs->addPoint(2, 0);
  mc.addGeometry(cs.get());
  EXPECT_TRUE(mc.hasCurveGeometry(TRUE));
}

TEST(OgrMultiCurveTest, CopyAndClone) {
  OGRMultiCurve mc;
  auto ls = std::make_unique<OGRLineString>();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  mc.addGeometry(ls.get());

  // Copy constructor
  OGRMultiCurve mc_copy(mc);
  EXPECT_EQ(1, mc_copy.getNumGeometries());

  // Assignment operator
  OGRMultiCurve mc_assigned;
  mc_assigned = mc;
  EXPECT_EQ(1, mc_assigned.getNumGeometries());

  // Self assignment
  const auto* ptr = &mc_assigned;
  mc_assigned = *ptr;
  EXPECT_EQ(1, mc_assigned.getNumGeometries());

  // Clone
  std::unique_ptr<OGRMultiCurve> mc_cloned(mc.clone());
  ASSERT_NE(nullptr, mc_cloned);
  EXPECT_EQ(1, mc_cloned->getNumGeometries());
}

TEST(OgrMultiCurveTest, ImportExportWkt) {
  const char* pszWkt = "MULTICURVE ((0 0,1 1))";
  OGRMultiCurve mc;
  EXPECT_EQ(OGRERR_NONE, mc.importFromWkt(&pszWkt));
  EXPECT_EQ(1, mc.getNumGeometries());

  OGRWktOptions opts;
  OGRErr err = OGRERR_NONE;
  std::string exported = mc.exportToWkt(opts, &err);
  EXPECT_EQ(OGRERR_NONE, err);
  EXPECT_EQ("MULTICURVE ((0 0,1 1))", exported);
}

TEST(OgrMultiCurveTest, CastToMultiLineString) {
  // Successful cast (contains only OGRLineString)
  {
    auto* mc = new OGRMultiCurve();
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    mc->addGeometry(ls.get());

    OGRMultiLineString* mls = OGRMultiCurve::CastToMultiLineString(mc);
    ASSERT_NE(nullptr, mls);
    EXPECT_STREQ("MULTILINESTRING", mls->getGeometryName());
    delete mls;
  }

  // Failed cast (contains non-linear curve like OGRCircularString)
  {
    auto* mc = new OGRMultiCurve();
    auto cs = std::make_unique<OGRCircularString>();
    cs->addPoint(0, 0);
    cs->addPoint(1, 1);
    cs->addPoint(2, 0);
    mc->addGeometry(cs.get());

    OGRMultiLineString* mls = OGRMultiCurve::CastToMultiLineString(mc);
    EXPECT_EQ(nullptr, mls);
  }
}

}  // namespace
}  // namespace autotest2
