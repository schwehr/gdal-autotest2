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

// Unit tests for third_party/geos/include/geos/triangulate/tri/TriList.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - Uses isolated GeometryFactory::create() instances rather than
//   getDefaultInstance().
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/triangulate/tri/TriList.h

#include "third_party/geos/include/geos/triangulate/tri/TriList.h"

#include <array>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/hull/HullTri.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/triangulate/tri/Tri.h"
#include "third_party/geos/include/geos/triangulate/tri/TriangulationBuilder.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::algorithm::hull::HullTri;
using ::geos::geom::Coordinate;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::GEOS_GEOMETRYCOLLECTION;
using ::geos::geom::GEOS_POLYGON;
using ::geos::triangulate::tri::Tri;
using ::geos::triangulate::tri::TriangulationBuilder;
using ::geos::triangulate::tri::TriList;
using ::geos::util::IllegalArgumentException;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

TEST(TriListTest, EmptyStateConstAndMutableAccessors) {
  TriList<Tri> list;
  const TriList<Tri>& const_list = list;

  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.size(), 0u);
  EXPECT_TRUE(const_list.empty());
  EXPECT_EQ(const_list.size(), 0u);
  EXPECT_EQ(list.begin(), list.end());
  EXPECT_EQ(const_list.begin(), const_list.end());
  EXPECT_TRUE(list.getTris().empty());
  EXPECT_DOUBLE_EQ(list.area(), 0.0);
  EXPECT_DOUBLE_EQ(list.length(), 0.0);
  EXPECT_NO_THROW(list.validate());

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Geometry> geom = const_list.toGeometry(factory.get());
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  EXPECT_TRUE(geom->isEmpty());
  EXPECT_EQ(geom->getNumGeometries(), 0u);

  std::ostringstream oss;
  oss << list;
  const std::string formatted = oss.str();
  EXPECT_EQ(formatted, "TRILIST [0] ()");
}

TEST(TriListTest, AddCoordinatesAndArrayPreservesOrderAndIteration) {
  TriList<Tri> list;
  list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0), Coordinate(1.0, 0.0));
  std::array<Coordinate, 3> corners = {
      Coordinate(0.0, 1.0),
      Coordinate(1.0, 1.0),
      Coordinate(1.0, 0.0),
  };
  list.add(corners);

  ASSERT_FALSE(list.empty());
  ASSERT_EQ(list.size(), 2u);
  ASSERT_EQ(list.getTris().size(), 2u);
  EXPECT_EQ(list[0], list.getTris()[0]);
  EXPECT_EQ(list[1], list.getTris()[1]);

  EXPECT_EQ(list[0]->getCoordinate(0), Coordinate(0.0, 0.0));
  EXPECT_EQ(list[0]->getCoordinate(1), Coordinate(0.0, 1.0));
  EXPECT_EQ(list[0]->getCoordinate(2), Coordinate(1.0, 0.0));
  EXPECT_EQ(list[1]->getCoordinate(0), Coordinate(0.0, 1.0));
  EXPECT_EQ(list[1]->getCoordinate(1), Coordinate(1.0, 1.0));
  EXPECT_EQ(list[1]->getCoordinate(2), Coordinate(1.0, 0.0));

  std::vector<Tri*> mutable_visited;
  for (Tri* tri : list) {
    mutable_visited.push_back(tri);
  }
  EXPECT_THAT(mutable_visited, ElementsAre(list[0], list[1]));

  const TriList<Tri>& const_list = list;
  std::vector<const Tri*> const_visited;
  for (const Tri* tri : const_list) {
    const_visited.push_back(tri);
  }
  EXPECT_THAT(const_visited, ElementsAre(list[0], list[1]));
}

TEST(TriListTest, AreaAndLengthSumAcrossTriangles) {
  TriList<Tri> list;
  // Two 3-4-5 right triangles (area 6, perimeter 12 each).
  list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 4.0), Coordinate(3.0, 0.0));
  list.add(Coordinate(0.0, 4.0), Coordinate(3.0, 4.0), Coordinate(3.0, 0.0));
  // Plus a 6-8-10 right triangle (area 24, perimeter 24).
  list.add(Coordinate(10.0, 0.0), Coordinate(10.0, 8.0), Coordinate(16.0, 0.0));

  EXPECT_DOUBLE_EQ(list.area(), 36.0);
  EXPECT_DOUBLE_EQ(list.length(), 48.0);
}

TEST(TriListTest, RemoveDeletesOnlyMatchingPointerFromVector) {
  TriList<Tri> list;
  list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 4.0), Coordinate(3.0, 0.0));
  list.add(Coordinate(0.0, 4.0), Coordinate(3.0, 4.0), Coordinate(3.0, 0.0));
  list.add(Coordinate(10.0, 0.0), Coordinate(10.0, 8.0), Coordinate(16.0, 0.0));

  Tri* t0 = list[0];
  Tri* t1 = list[1];
  Tri* t2 = list[2];

  // Removing nullptr or a Tri owned by another list is a no-op.
  TriList<Tri> other_list;
  other_list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0),
                 Coordinate(1.0, 0.0));
  list.remove(nullptr);
  list.remove(other_list[0]);
  EXPECT_THAT(list.getTris(), ElementsAre(t0, t1, t2));

  // Removing the middle element preserves remaining elements and order.
  list.remove(t1);
  EXPECT_THAT(list.getTris(), ElementsAre(t0, t2));
  EXPECT_EQ(list.size(), 2u);
  EXPECT_DOUBLE_EQ(list.area(), 30.0);
  EXPECT_DOUBLE_EQ(list.length(), 36.0);

  // Removing an already-removed pointer is a no-op.
  list.remove(t1);
  EXPECT_THAT(list.getTris(), ElementsAre(t0, t2));

  list.remove(t0);
  list.remove(t2);
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.size(), 0u);
  EXPECT_DOUBLE_EQ(list.area(), 0.0);
  EXPECT_DOUBLE_EQ(list.length(), 0.0);
}

TEST(TriListTest, PointerStabilityAcrossDequeGrowth) {
  TriList<Tri> list;
  Tri* first = nullptr;
  Tri* mid = nullptr;
  for (int i = 0; i < 200; ++i) {
    const double x = static_cast<double>(i);
    list.add(Coordinate(x, 0.0), Coordinate(x, 1.0), Coordinate(x + 1.0, 0.0));
    if (i == 0) {
      first = list[0];
    } else if (i == 100) {
      mid = list[100];
    }
  }

  ASSERT_EQ(list.size(), 200u);
  EXPECT_EQ(list[0], first);
  EXPECT_EQ(list[100], mid);
  EXPECT_EQ(first->getCoordinate(0), Coordinate(0.0, 0.0));
  EXPECT_EQ(mid->getCoordinate(0), Coordinate(100.0, 0.0));
}

TEST(TriListTest, DegreeCountsMatchingVerticesIn2D) {
  TriList<Tri> list;
  const Coordinate center(1.0, 1.0);
  // Four CW triangles sharing the center vertex (1, 1).
  list.add(center, Coordinate(0.0, 0.0), Coordinate(0.0, 2.0));
  list.add(center, Coordinate(0.0, 2.0), Coordinate(2.0, 2.0));
  list.add(center, Coordinate(2.0, 2.0), Coordinate(2.0, 0.0));
  list.add(center, Coordinate(2.0, 0.0), Coordinate(0.0, 0.0));

  EXPECT_EQ(list.degree(list[0], 0), 4u);
  // Corner (0, 2) belongs to triangles 0 and 1.
  EXPECT_EQ(list.degree(list[0], 2), 2u);
  // Corner (0, 0) belongs to triangles 0 and 3.
  EXPECT_EQ(list.degree(list[0], 1), 2u);

  // Add a triangle sharing (0, 0) with a different Z ordinate; degree uses 2D.
  list.add(Coordinate(0.0, 0.0, 42.0), Coordinate(-1.0, 0.0),
           Coordinate(-1.0, 1.0));
  EXPECT_EQ(list.degree(list[0], 1), 3u);
  EXPECT_EQ(list.degree(list[4], 1), 1u);

  // External Tri with disjoint vertices has degree 0.
  Tri disjoint(Coordinate(50.0, 50.0), Coordinate(50.0, 51.0),
               Coordinate(51.0, 50.0));
  EXPECT_EQ(list.degree(&disjoint, 0), 0u);

  // Invalid vertex indices propagate IllegalArgumentException from Tri.
  EXPECT_THROW(list.degree(list[0], -1), IllegalArgumentException);
  EXPECT_THROW(list.degree(list[0], 3), IllegalArgumentException);
}

TEST(TriListTest, ValidateChecksClockwiseOrientationAndAdjacency) {
  TriList<Tri> valid_list;
  valid_list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0),
                 Coordinate(1.0, 0.0));
  valid_list.add(Coordinate(0.0, 1.0), Coordinate(1.0, 1.0),
                 Coordinate(1.0, 0.0));
  TriangulationBuilder::build(valid_list);
  EXPECT_NO_THROW(valid_list.validate());

  TriList<Tri> ccw_list;
  ccw_list.add(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
               Coordinate(0.0, 1.0));
  EXPECT_THROW(ccw_list.validate(), IllegalArgumentException);

  TriList<Tri> collinear_list;
  collinear_list.add(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                     Coordinate(2.0, 0.0));
  EXPECT_THROW(collinear_list.validate(), IllegalArgumentException);

  TriList<Tri> mixed_list;
  mixed_list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0),
                 Coordinate(1.0, 0.0));
  mixed_list.add(Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                 Coordinate(0.0, 1.0));
  EXPECT_THROW(mixed_list.validate(), IllegalArgumentException);
}

TEST(TriListTest, ToGeometryBuildsGeometryCollectionOfPolygons) {
  TriList<Tri> list;
  list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0), Coordinate(1.0, 0.0));
  list.add(Coordinate(0.0, 1.0), Coordinate(1.0, 1.0), Coordinate(1.0, 0.0));

  GeometryFactory::Ptr factory = GeometryFactory::create();
  const TriList<Tri>& const_list = list;
  std::unique_ptr<Geometry> collection = const_list.toGeometry(factory.get());
  ASSERT_NE(collection, nullptr);
  EXPECT_EQ(collection->getFactory(), factory.get());
  EXPECT_EQ(collection->getGeometryTypeId(), GEOS_GEOMETRYCOLLECTION);
  ASSERT_EQ(collection->getNumGeometries(), 2u);
  EXPECT_EQ(collection->getGeometryN(0)->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_EQ(collection->getGeometryN(1)->getGeometryTypeId(), GEOS_POLYGON);
  EXPECT_DOUBLE_EQ(collection->getArea(), 1.0);

  list.remove(list[0]);
  std::unique_ptr<Geometry> reduced = const_list.toGeometry(factory.get());
  ASSERT_NE(reduced, nullptr);
  EXPECT_EQ(reduced->getNumGeometries(), 1u);
  EXPECT_DOUBLE_EQ(reduced->getArea(), 0.5);
}

TEST(TriListTest, StreamInsertionFormatsAllTriangles) {
  TriList<Tri> list;
  list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 1.0), Coordinate(1.0, 0.0));
  list.add(Coordinate(0.0, 1.0), Coordinate(1.0, 1.0), Coordinate(1.0, 0.0));

  std::ostringstream oss;
  oss << list;
  EXPECT_EQ(oss.str(),
            "TRILIST [2] (  POLYGON ((0 0, 0 1, 1 0, 0 0)),\n"
            "  POLYGON ((0 1, 1 1, 1 0, 0 1)),\n"
            ")");
}

TEST(TriListTest, SupportsHullTriTemplateInstantiation) {
  TriList<HullTri> hull_list;
  hull_list.add(Coordinate(0.0, 0.0), Coordinate(0.0, 3.0),
                Coordinate(4.0, 0.0));
  hull_list.add(Coordinate(0.0, 3.0), Coordinate(4.0, 3.0),
                Coordinate(4.0, 0.0));

  ASSERT_EQ(hull_list.size(), 2u);
  EXPECT_DOUBLE_EQ(hull_list.area(), 12.0);
  EXPECT_DOUBLE_EQ(hull_list.length(), 24.0);
  EXPECT_EQ(hull_list.degree(hull_list[0], 1), 2u);
  EXPECT_NO_THROW(hull_list.validate());

  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<Geometry> geom = hull_list.toGeometry(factory.get());
  ASSERT_NE(geom, nullptr);
  EXPECT_EQ(geom->getNumGeometries(), 2u);

  hull_list[0]->remove(hull_list);
  EXPECT_EQ(hull_list.size(), 1u);

  std::ostringstream oss;
  oss << hull_list;
  EXPECT_THAT(oss.str(), HasSubstr("TRILIST [1] ("));
}

}  // namespace
}  // namespace geos_google
