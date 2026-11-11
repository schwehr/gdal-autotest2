// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for third_party/geos/src/noding/snapround/HotPixelIndex.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/snapround/HotPixelIndex.cpp

#include "third_party/geos/include/geos/noding/snapround/HotPixelIndex.h"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/index/kdtree/KdNode.h"
#include "third_party/geos/include/geos/index/kdtree/KdNodeVisitor.h"
#include "third_party/geos/include/geos/noding/snapround/HotPixel.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::PrecisionModel;
using geos::index::kdtree::KdNode;
using geos::index::kdtree::KdNodeVisitor;
using geos::noding::snapround::HotPixel;
using geos::noding::snapround::HotPixelIndex;
using geos::util::IllegalArgumentException;
using ::testing::UnorderedElementsAre;

class CollectingVisitor final : public KdNodeVisitor {
 public:
  explicit CollectingVisitor(std::vector<HotPixel*>& out) : out_(out) {}
  void visit(KdNode* node) override {
    out_.push_back(static_cast<HotPixel*>(node->getData()));
  }

 private:
  std::vector<HotPixel*>& out_;
};

std::vector<HotPixel*> QueryIndex(HotPixelIndex& index, const CoordinateXY& p0,
                                  const CoordinateXY& p1) {
  std::vector<HotPixel*> results;
  CollectingVisitor visitor(results);
  index.query(p0, p1, visitor);
  return results;
}

TEST(HotPixelIndexTest, AddSingleCoordinatesRoundsAndPreservesOrdinates) {
  const PrecisionModel pm(10.0);
  HotPixelIndex index(&pm);

  HotPixel* hp_xy = index.add(CoordinateXY(1.23, 4.56));
  ASSERT_NE(hp_xy, nullptr);
  EXPECT_FALSE(hp_xy->isNode());
  EXPECT_DOUBLE_EQ(hp_xy->getScaleFactor(), 10.0);
  EXPECT_DOUBLE_EQ(hp_xy->getCoordinate().x, 1.2);
  EXPECT_DOUBLE_EQ(hp_xy->getCoordinate().y, 4.6);
  EXPECT_TRUE(std::isnan(hp_xy->getCoordinate().z));
  EXPECT_TRUE(std::isnan(hp_xy->getCoordinate().m));

  // Second coordinate rounding to the same grid cell reuses the pixel and marks
  // it as a node.
  HotPixel* hp_dup = index.add(CoordinateXY(1.24, 4.58));
  EXPECT_EQ(hp_dup, hp_xy);
  EXPECT_TRUE(hp_xy->isNode());

  HotPixel* hp_xyz = index.add(Coordinate(2.04, 3.01, 7.5));
  ASSERT_NE(hp_xyz, nullptr);
  EXPECT_NE(hp_xyz, hp_xy);
  EXPECT_FALSE(hp_xyz->isNode());
  EXPECT_DOUBLE_EQ(hp_xyz->getCoordinate().x, 2.0);
  EXPECT_DOUBLE_EQ(hp_xyz->getCoordinate().y, 3.0);
  EXPECT_DOUBLE_EQ(hp_xyz->getCoordinate().z, 7.5);
  EXPECT_TRUE(std::isnan(hp_xyz->getCoordinate().m));

  HotPixel* hp_xym = index.add(CoordinateXYM(3.04, 4.01, 9.25));
  ASSERT_NE(hp_xym, nullptr);
  EXPECT_FALSE(hp_xym->isNode());
  EXPECT_TRUE(std::isnan(hp_xym->getCoordinate().z));
  EXPECT_DOUBLE_EQ(hp_xym->getCoordinate().m, 9.25);

  HotPixel* hp_xyzm = index.add(CoordinateXYZM(4.04, 5.01, 8.5, 11.25));
  ASSERT_NE(hp_xyzm, nullptr);
  EXPECT_FALSE(hp_xyzm->isNode());
  EXPECT_DOUBLE_EQ(hp_xyzm->getCoordinate().z, 8.5);
  EXPECT_DOUBLE_EQ(hp_xyzm->getCoordinate().m, 11.25);
}

TEST(HotPixelIndexTest, AddRoundedMarksDuplicateAsNodeAndKeepsOriginal) {
  const PrecisionModel pm(10.0);
  HotPixelIndex index(&pm);

  // addRounded inserts the coordinate directly without applying
  // pm->makePrecise.
  HotPixel* first = index.addRounded(CoordinateXYZM(10.23, 20.47, 3.0, 4.0));
  ASSERT_NE(first, nullptr);
  EXPECT_FALSE(first->isNode());
  EXPECT_DOUBLE_EQ(first->getCoordinate().x, 10.23);
  EXPECT_DOUBLE_EQ(first->getCoordinate().y, 20.47);
  EXPECT_DOUBLE_EQ(first->getScaleFactor(), 10.0);

  HotPixel* second = index.addRounded(CoordinateXYZM(10.23, 20.47, 99.0, 88.0));
  EXPECT_EQ(second, first);
  EXPECT_TRUE(first->isNode());
  EXPECT_DOUBLE_EQ(first->getCoordinate().z, 3.0);
  EXPECT_DOUBLE_EQ(first->getCoordinate().m, 4.0);
}

TEST(HotPixelIndexTest, AddCoordinateSequenceSupportsEmptyAndAllDimensions) {
  const PrecisionModel pm(10.0);
  HotPixelIndex index(&pm);

  const CoordinateSequence empty_seq(0, false, false);
  index.add(&empty_seq);
  EXPECT_TRUE(QueryIndex(index, CoordinateXY(-100.0, -100.0),
                         CoordinateXY(100.0, 100.0))
                  .empty());

  const CoordinateSequence xy_seq = {CoordinateXY(1.02, 2.03),
                                     CoordinateXY(1.04, 2.01)};
  index.add(&xy_seq);
  auto xy_hits =
      QueryIndex(index, CoordinateXY(0.5, 1.5), CoordinateXY(1.5, 2.5));
  ASSERT_EQ(xy_hits.size(), 1);
  EXPECT_TRUE(xy_hits[0]->isNode());
  EXPECT_DOUBLE_EQ(xy_hits[0]->getCoordinate().x, 1.0);
  EXPECT_DOUBLE_EQ(xy_hits[0]->getCoordinate().y, 2.0);
  EXPECT_TRUE(std::isnan(xy_hits[0]->getCoordinate().z));
  EXPECT_TRUE(std::isnan(xy_hits[0]->getCoordinate().m));

  const CoordinateSequence xyz_seq = {Coordinate(5.01, 6.02, 12.0),
                                      Coordinate(5.41, 6.42, 14.0),
                                      Coordinate(5.03, 6.04, 15.0)};
  index.add(&xyz_seq);
  auto xyz_hits =
      QueryIndex(index, CoordinateXY(4.8, 5.8), CoordinateXY(5.6, 6.6));
  ASSERT_EQ(xyz_hits.size(), 2);
  for (HotPixel* hp : xyz_hits) {
    if (hp->getCoordinate().x == 5.0) {
      EXPECT_TRUE(hp->isNode());
    } else {
      EXPECT_DOUBLE_EQ(hp->getCoordinate().x, 5.4);
      EXPECT_FALSE(hp->isNode());
      EXPECT_DOUBLE_EQ(hp->getCoordinate().z, 14.0);
    }
    EXPECT_TRUE(std::isnan(hp->getCoordinate().m));
  }

  const CoordinateSequence xym_seq = {CoordinateXYM(8.01, 9.02, 3.5),
                                      CoordinateXYM(8.41, 9.42, 4.5)};
  index.add(&xym_seq);
  auto xym_hits =
      QueryIndex(index, CoordinateXY(7.8, 8.8), CoordinateXY(8.6, 9.6));
  ASSERT_EQ(xym_hits.size(), 2);
  for (HotPixel* hp : xym_hits) {
    EXPECT_FALSE(hp->isNode());
    EXPECT_TRUE(std::isnan(hp->getCoordinate().z));
    if (hp->getCoordinate().x == 8.0) {
      EXPECT_DOUBLE_EQ(hp->getCoordinate().m, 3.5);
    } else {
      EXPECT_DOUBLE_EQ(hp->getCoordinate().x, 8.4);
      EXPECT_DOUBLE_EQ(hp->getCoordinate().m, 4.5);
    }
  }

  const CoordinateSequence xyzm_seq = {CoordinateXYZM(11.01, 12.02, 4.5, 6.5),
                                       CoordinateXYZM(11.03, 12.04, 7.5, 8.5)};
  index.add(&xyzm_seq);
  auto xyzm_hits =
      QueryIndex(index, CoordinateXY(10.8, 11.8), CoordinateXY(11.2, 12.2));
  ASSERT_EQ(xyzm_hits.size(), 1);
  EXPECT_TRUE(xyzm_hits[0]->isNode());
  EXPECT_FALSE(std::isnan(xyzm_hits[0]->getCoordinate().z));
  EXPECT_FALSE(std::isnan(xyzm_hits[0]->getCoordinate().m));
}

TEST(HotPixelIndexTest, AddCoordinateVectorDeduplicatesRoundedPoints) {
  const PrecisionModel pm(10.0);
  HotPixelIndex index(&pm);

  const std::vector<Coordinate> empty_pts;
  index.add(empty_pts);
  EXPECT_TRUE(
      QueryIndex(index, CoordinateXY(-10.0, -10.0), CoordinateXY(10.0, 10.0))
          .empty());

  const std::vector<Coordinate> pts = {
      Coordinate(1.01, 1.02, 10.0),
      Coordinate(1.04, 0.98, 20.0),  // Also rounds to (1.0, 1.0).
      Coordinate(3.01, 4.02, 30.0),
  };
  index.add(pts);

  auto hits = QueryIndex(index, CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 5.0));
  ASSERT_EQ(hits.size(), 2);

  std::vector<std::pair<double, bool>> summary;
  for (HotPixel* hp : hits) {
    summary.emplace_back(hp->getCoordinate().x, hp->isNode());
  }
  EXPECT_THAT(summary,
              UnorderedElementsAre(std::pair<double, bool>(1.0, true),
                                   std::pair<double, bool>(3.0, false)));
}

TEST(HotPixelIndexTest, AddNodesMarksAllInsertedPixelsAsNodes) {
  const PrecisionModel pm(1.0);
  HotPixelIndex index(&pm);

  const CoordinateSequence empty_seq(0, false, false);
  const std::vector<Coordinate> empty_vec;
  index.addNodes(&empty_seq);
  index.addNodes(empty_vec);
  EXPECT_TRUE(
      QueryIndex(index, CoordinateXY(-10.0, -10.0), CoordinateXY(10.0, 10.0))
          .empty());

  HotPixel* pre_existing = index.add(CoordinateXY(9.2, 10.1));
  ASSERT_NE(pre_existing, nullptr);
  EXPECT_FALSE(pre_existing->isNode());

  const CoordinateSequence xy_seq = {CoordinateXY(1.2, 2.3)};
  const CoordinateSequence xyz_seq = {Coordinate(3.2, 4.1, 15.0)};
  const CoordinateSequence xym_seq = {CoordinateXYM(5.2, 6.1, 25.0)};
  const CoordinateSequence xyzm_seq = {CoordinateXYZM(7.2, 8.3, 5.0, 6.0),
                                       CoordinateXYZM(9.1, 10.2, 9.0, 10.0)};
  index.addNodes(&xy_seq);
  index.addNodes(&xyz_seq);
  index.addNodes(&xym_seq);
  index.addNodes(&xyzm_seq);

  const std::vector<Coordinate> vec = {Coordinate(11.1, 12.2, 9.0)};
  index.addNodes(vec);

  EXPECT_TRUE(pre_existing->isNode());

  auto hits =
      QueryIndex(index, CoordinateXY(0.0, 0.0), CoordinateXY(15.0, 15.0));
  ASSERT_EQ(hits.size(), 6);

  std::vector<double> xs;
  for (HotPixel* hp : hits) {
    EXPECT_TRUE(hp->isNode());
    xs.push_back(hp->getCoordinate().x);
    if (hp->getCoordinate().x == 9.0) {
      EXPECT_EQ(hp, pre_existing);
    }
  }
  EXPECT_THAT(xs, UnorderedElementsAre(1.0, 3.0, 5.0, 7.0, 9.0, 11.0));
}

TEST(HotPixelIndexTest, QueryExpandsEnvelopeByReciprocalScaleFactor) {
  // scaleFactor = 4.0 -> expansion is exactly 0.25 in binary floating point.
  const PrecisionModel pm(4.0);
  HotPixelIndex index(&pm);

  EXPECT_TRUE(QueryIndex(index, CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 5.0))
                  .empty());

  HotPixel* interior = index.add(CoordinateXY(3.0, 2.0));
  HotPixel* corner_ll = index.add(CoordinateXY(1.75, 1.75));
  HotPixel* corner_ur = index.add(CoordinateXY(4.25, 2.25));
  // Just outside [1.75, 4.25] x [1.75, 2.25].
  index.add(CoordinateXY(1.5, 2.0));
  index.add(CoordinateXY(4.5, 2.0));
  index.add(CoordinateXY(3.0, 1.5));
  index.add(CoordinateXY(3.0, 2.5));

  auto forward =
      QueryIndex(index, CoordinateXY(2.0, 2.0), CoordinateXY(4.0, 2.0));
  EXPECT_THAT(forward, UnorderedElementsAre(interior, corner_ll, corner_ur));

  auto reversed =
      QueryIndex(index, CoordinateXY(4.0, 2.0), CoordinateXY(2.0, 2.0));
  EXPECT_THAT(reversed, UnorderedElementsAre(interior, corner_ll, corner_ur));

  // Point query at (2.0, 2.0) expands to [1.75, 2.25] x [1.75, 2.25].
  auto point_hits =
      QueryIndex(index, CoordinateXY(2.0, 2.0), CoordinateXY(2.0, 2.0));
  EXPECT_THAT(point_hits, UnorderedElementsAre(corner_ll));
}

TEST(HotPixelIndexTest, NegativeScaleSpecifiesGridSizeAndMaintainsStability) {
  // Negative scale -10.0 specifies gridSize = 10.0, scale = 0.1.
  const PrecisionModel pm_coarse(-10.0);
  HotPixelIndex coarse(&pm_coarse);
  HotPixel* hp1 = coarse.add(CoordinateXY(14.0, 26.0));
  HotPixel* hp2 = coarse.add(CoordinateXY(20.0, 40.0));
  coarse.add(CoordinateXY(30.0, 30.0));

  EXPECT_DOUBLE_EQ(hp1->getCoordinate().x, 10.0);
  EXPECT_DOUBLE_EQ(hp1->getCoordinate().y, 30.0);
  auto coarse_hits =
      QueryIndex(coarse, CoordinateXY(10.0, 30.0), CoordinateXY(10.0, 30.0));
  EXPECT_THAT(coarse_hits, UnorderedElementsAre(hp1, hp2));

  // Add enough distinct points to span multiple std::deque blocks and confirm
  // pointers returned early remain valid.
  const PrecisionModel pm_unit(PrecisionModel::FIXED);
  HotPixelIndex large(&pm_unit);
  std::vector<HotPixel*> kept;
  kept.reserve(2000);
  for (std::size_t i = 0; i < 2000; ++i) {
    kept.push_back(large.add(CoordinateXY(static_cast<double>(i), 0.0)));
  }
  for (std::size_t i = 0; i < 2000; ++i) {
    EXPECT_DOUBLE_EQ(kept[i]->getCoordinate().x, static_cast<double>(i));
    EXPECT_FALSE(kept[i]->isNode());
  }
}

TEST(HotPixelIndexTest, ZeroScalePrecisionModelThrowsWhenAddingHotPixel) {
  // Default-constructed PrecisionModel() and PrecisionModel(0.0) both have
  // scale == 0.0, which HotPixel rejects with IllegalArgumentException.
  const PrecisionModel pm_default;
  HotPixelIndex index_default(&pm_default);
  EXPECT_THROW(index_default.add(CoordinateXY(1.0, 2.0)),
               IllegalArgumentException);
  EXPECT_THROW(index_default.addRounded(CoordinateXYZM(1.0, 2.0, 3.0, 4.0)),
               IllegalArgumentException);

  const PrecisionModel pm_zero(0.0);
  HotPixelIndex index_zero(&pm_zero);
  EXPECT_THROW(index_zero.add(CoordinateXY(1.0, 2.0)),
               IllegalArgumentException);
}

TEST(HotPixelIndexTest, ExplicitFloatingPrecisionModelsUseUnitScale) {
  // PrecisionModel(Type) initializes scale == 1.0.
  const PrecisionModel pm_floating(PrecisionModel::FLOATING);
  HotPixelIndex index_floating(&pm_floating);
  HotPixel* hp = index_floating.add(CoordinateXY(1.2345, 2.6789));
  ASSERT_NE(hp, nullptr);
  EXPECT_DOUBLE_EQ(hp->getScaleFactor(), 1.0);
  EXPECT_DOUBLE_EQ(hp->getCoordinate().x, 1.2345);
  EXPECT_DOUBLE_EQ(hp->getCoordinate().y, 2.6789);
  EXPECT_THAT(QueryIndex(index_floating, CoordinateXY(0.5, 2.0),
                         CoordinateXY(0.5, 2.0)),
              UnorderedElementsAre(hp));

  const PrecisionModel pm_single(PrecisionModel::FLOATING_SINGLE);
  HotPixelIndex index_single(&pm_single);
  const double raw_x = 1.123456789;
  const double raw_y = 2.987654321;
  HotPixel* hp_single = index_single.add(CoordinateXY(raw_x, raw_y));
  ASSERT_NE(hp_single, nullptr);
  EXPECT_DOUBLE_EQ(hp_single->getScaleFactor(), 1.0);
  EXPECT_DOUBLE_EQ(hp_single->getCoordinate().x,
                   static_cast<double>(static_cast<float>(raw_x)));
  EXPECT_DOUBLE_EQ(hp_single->getCoordinate().y,
                   static_cast<double>(static_cast<float>(raw_y)));
}

}  // namespace
}  // namespace geos_google
