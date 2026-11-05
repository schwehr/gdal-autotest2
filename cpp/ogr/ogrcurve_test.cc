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
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::NotNull;

TEST(OgrCurveTest, OperatorAssignAndDimension) {
  OGRLineString ls1;
  ls1.addPoint(1, 2);
  ls1.addPoint(3, 4);
  auto* srs = new OGRSpatialReference();
  srs->SetWellKnownGeogCS("WGS84");
  ls1.assignSpatialReference(srs);
  srs->Release();

  OGRLineString ls2;
  // Assignment through OGRCurve reference
  OGRCurve& c1 = ls1;
  OGRCurve& c2 = ls2;
  c2 = c1;
  EXPECT_EQ(1, c2.getDimension());
  EXPECT_THAT(c2.getSpatialReference(), NotNull());

  // Self-assignment
  c2 = c2;
  EXPECT_EQ(1, c2.getDimension());

  OSRCleanup();
}

TEST(OgrCurveTest, GetIsClosed) {
  // 2D closed
  OGRLineString ls_closed_2d;
  ls_closed_2d.addPoint(0, 0);
  ls_closed_2d.addPoint(1, 1);
  ls_closed_2d.addPoint(0, 0);
  EXPECT_TRUE(ls_closed_2d.get_IsClosed());

  // 2D open
  OGRLineString ls_open_2d;
  ls_open_2d.addPoint(0, 0);
  ls_open_2d.addPoint(1, 1);
  EXPECT_FALSE(ls_open_2d.get_IsClosed());

  // 3D closed
  OGRLineString ls_closed_3d;
  ls_closed_3d.addPoint(0, 0, 5);
  ls_closed_3d.addPoint(1, 1, 5);
  ls_closed_3d.addPoint(0, 0, 5);
  EXPECT_TRUE(ls_closed_3d.get_IsClosed());

  // 3D open (different Z)
  OGRLineString ls_open_3d;
  ls_open_3d.addPoint(0, 0, 5);
  ls_open_3d.addPoint(1, 1, 5);
  ls_open_3d.addPoint(0, 0, 6);
  EXPECT_FALSE(ls_open_3d.get_IsClosed());

  // One 3D, one 2D point in curve
  OGRLineString ls_mixed;
  ls_mixed.set3D(TRUE);
  ls_mixed.addPoint(0, 0, 5);
  ls_mixed.set3D(FALSE);
  // Using points directly with different 3D flags:
  OGRCompoundCurve cc_mixed;
  auto part1 = std::make_unique<OGRLineString>();
  part1->addPoint(0, 0, 5);
  auto part2 = std::make_unique<OGRLineString>();
  part2->addPoint(0, 0);
  // Directly test line 84: one point 3D, other 2D
  OGRLineString ls_3d_one;
  ls_3d_one.addPoint(0, 0, 5);
  ls_3d_one.addPoint(0, 0);  // second point gets Z=0 in 3D linestring
  // For true mixed start/end point dimensionality:
  // Subclass OGRCurve to test start 3D and end 2D
  class MixedCurve : public OGRLineString {
   public:
    void StartPoint(OGRPoint* p) const override {
      p->setX(0);
      p->setY(0);
      p->setZ(5);
    }
    void EndPoint(OGRPoint* p) const override {
      p->setX(0);
      p->setY(0);
      p->flattenTo2D();
    }
  };
  MixedCurve mixed_curve;
  EXPECT_FALSE(mixed_curve.get_IsClosed());
}

TEST(OgrCurveTest, IsConvex) {
  // Empty curve (< 2 points)
  OGRLineString empty_ls;
  EXPECT_TRUE(empty_ls.IsConvex());

  // Convex closed box CW
  OGRLineString box_cw;
  box_cw.addPoint(0, 0);
  box_cw.addPoint(0, 10);
  box_cw.addPoint(10, 10);
  box_cw.addPoint(10, 0);
  box_cw.addPoint(0, 0);
  EXPECT_TRUE(box_cw.IsConvex());

  // Non-convex shape (indentation)
  OGRLineString non_convex;
  non_convex.addPoint(0, 0);
  non_convex.addPoint(0, 10);
  non_convex.addPoint(5, 5);
  non_convex.addPoint(10, 10);
  non_convex.addPoint(10, 0);
  non_convex.addPoint(0, 0);
  EXPECT_FALSE(non_convex.IsConvex());
}

TEST(OgrCurveTest, CastingFunctions) {
  // CastToCompoundCurve with LineString
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    auto* cc = OGRCurve::CastToCompoundCurve(ls.release());
    ASSERT_THAT(cc, NotNull());
    EXPECT_EQ(1, cc->getNumCurves());
    delete cc;
  }

  // CastToCompoundCurve with empty LineString
  {
    auto* empty_ls = new OGRLineString();
    auto* cc = OGRCurve::CastToCompoundCurve(empty_ls);
    ASSERT_THAT(cc, NotNull());
    EXPECT_EQ(0, cc->getNumCurves());
    delete empty_ls;
    delete cc;
  }

  // CastToLineString
  {
    auto ring = std::make_unique<OGRLinearRing>();
    ring->addPoint(0, 0);
    ring->addPoint(1, 0);
    ring->addPoint(0, 0);
    auto* ls = OGRCurve::CastToLineString(ring.release());
    ASSERT_THAT(ls, NotNull());
    delete ls;
  }

  // CastToLinearRing
  {
    auto ls = std::make_unique<OGRLineString>();
    ls->addPoint(0, 0);
    ls->addPoint(1, 0);
    ls->addPoint(0, 0);
    auto* ring = OGRCurve::CastToLinearRing(ls.release());
    ASSERT_THAT(ring, NotNull());
    delete ring;
  }
}

class ExposeCurveMethods : public OGRLineString {
 public:
  using OGRCurve::ContainsPoint;
  using OGRCurve::IntersectsPoint;
};

TEST(OgrCurveTest, ContainsPointAndIntersectsPoint) {
  ExposeCurveMethods curve;
  OGRPoint pt(1, 0.5);
  EXPECT_EQ(-1, curve.ContainsPoint(&pt));
  EXPECT_EQ(-1, curve.IntersectsPoint(&pt));
}

TEST(OgrCurveTest, PointIteratorDestroy) {
  OGRLineString ls;
  ls.addPoint(1, 2);
  OGRPointIterator* iter = ls.getPointIterator();
  ASSERT_THAT(iter, NotNull());
  OGRPoint pt;
  EXPECT_TRUE(iter->getNextPoint(&pt));
  EXPECT_DOUBLE_EQ(1.0, pt.getX());
  EXPECT_DOUBLE_EQ(2.0, pt.getY());
  EXPECT_FALSE(iter->getNextPoint(&pt));
  OGRPointIterator::destroy(iter);
}

TEST(OgrCurveTest, SimpleCurveIteratorsAndPointMutation) {
  OGRLineString ls;
  ls.addPoint(0, 0);
  ls.addPoint(10, 10);

  // Iterate and mutate XY
  for (auto&& pt : ls) {
    pt.setX(pt.getX() + 5.0);
    pt.setY(pt.getY() + 15.0);
  }
  EXPECT_DOUBLE_EQ(5.0, ls.getX(0));
  EXPECT_DOUBLE_EQ(15.0, ls.getY(0));
  EXPECT_DOUBLE_EQ(15.0, ls.getX(1));
  EXPECT_DOUBLE_EQ(25.0, ls.getY(1));

  // Mutate Z and M
  auto it = ls.begin();
  (*it).setZ(50.0);
  (*it).setM(100.0);
  ++it;
  EXPECT_TRUE(ls.Is3D());
  EXPECT_TRUE(ls.IsMeasured());
  EXPECT_DOUBLE_EQ(50.0, ls.getZ(0));
  EXPECT_DOUBLE_EQ(100.0, ls.getM(0));

  // ConstIterator
  const auto& const_ls = ls;
  int count = 0;
  for (const auto& pt : const_ls) {
    count++;
    EXPECT_GT(pt.getX(), 0.0);
  }
  EXPECT_EQ(2, count);
}

TEST(OgrCurveTest, CurveConstIteratorAndMoves) {
  // Empty curve iteration using OGRCurve::begin()
  const OGRCircularString empty_cs;
  const OGRCurve& empty_ref = empty_cs;
  EXPECT_FALSE(empty_ref.begin() != empty_ref.end());

  // Non-empty curve iteration using OGRCurve::begin()
  OGRCircularString cs;
  cs.addPoint(1, 2);
  cs.addPoint(3, 4);
  cs.addPoint(5, 6);
  const OGRCurve& curve_ref = cs;

  int count = 0;
  for (const auto& pt : curve_ref) {
    count++;
    EXPECT_GT(pt.getX(), 0.0);
  }
  EXPECT_EQ(3, count);

  // Move constructor
  auto it1 = curve_ref.begin();
  auto it2(std::move(it1));
  EXPECT_TRUE(it2 != curve_ref.end());
  EXPECT_DOUBLE_EQ(1.0, (*it2).getX());

  // Move assignment
  auto it3 = curve_ref.end();
  it3 = std::move(it2);
  EXPECT_TRUE(it3 != curve_ref.end());
  EXPECT_DOUBLE_EQ(1.0, (*it3).getX());
}

TEST(OgrCurveTest, IsClockwiseVariants) {
  // Less than 3 points -> TRUE
  OGRLineString ls_short;
  ls_short.addPoint(0, 0);
  ls_short.addPoint(1, 1);
  EXPECT_TRUE(ls_short.OGRCurve::isClockwise());

  // Standard CW ring
  OGRLineString cw;
  cw.addPoint(0, 0);
  cw.addPoint(0, 10);
  cw.addPoint(10, 10);
  cw.addPoint(10, 0);
  cw.addPoint(0, 0);
  EXPECT_TRUE(cw.OGRCurve::isClockwise());

  // Standard CCW ring
  OGRLineString ccw;
  ccw.addPoint(0, 0);
  ccw.addPoint(10, 0);
  ccw.addPoint(10, 10);
  ccw.addPoint(0, 10);
  ccw.addPoint(0, 0);
  EXPECT_FALSE(ccw.OGRCurve::isClockwise());

  // Lowest-rightmost vertex is at index 0 (v == 0)
  OGRLineString cw_v0;
  cw_v0.addPoint(10, 0);  // lowest-rightmost at index 0
  cw_v0.addPoint(0, 0);
  cw_v0.addPoint(0, 10);
  cw_v0.addPoint(10, 10);
  cw_v0.addPoint(10, 0);
  EXPECT_TRUE(cw_v0.OGRCurve::isClockwise());

  // Lowest-rightmost vertex near end (v + 1 >= nPointCount - 1)
  OGRLineString cw_vend;
  cw_vend.addPoint(0, 0);
  cw_vend.addPoint(0, 10);
  cw_vend.addPoint(10, 10);
  cw_vend.addPoint(10, 0);  // lowest-rightmost at index 3 of 5
  cw_vend.addPoint(0, 0);
  EXPECT_TRUE(cw_vend.OGRCurve::isClockwise());

  // Duplicate lowest-rightmost vertex triggering fallback (bUseFallback = true)
  OGRLineString cw_dup;
  cw_dup.addPoint(0, 10);
  cw_dup.addPoint(10, 10);
  cw_dup.addPoint(10, 0);
  cw_dup.addPoint(10, 0);  // duplicate lowest rightmost
  cw_dup.addPoint(0, 0);
  cw_dup.addPoint(0, 10);
  EXPECT_TRUE(cw_dup.OGRCurve::isClockwise());

  // Near duplicate adjacent points within EPSILON triggering fallback
  OGRLineString cw_eps;
  cw_eps.addPoint(0, 0);
  cw_eps.addPoint(0, 10);
  cw_eps.addPoint(10, 10);
  cw_eps.addPoint(10.0000001, 0.0000001);  // within 1e-5
  cw_eps.addPoint(10.0, 0.0);
  cw_eps.addPoint(0, 0);
  EXPECT_TRUE(cw_eps.OGRCurve::isClockwise());
}

}  // namespace
}  // namespace autotest2
