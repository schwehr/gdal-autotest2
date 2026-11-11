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

#include "third_party/geos/include/geos/coverage/CoverageEdge.h"

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineSegment.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/MultiLineString.h"
#include "third_party/geos/include/geos/util/IllegalStateException.h"

namespace geos_google {
namespace {

using geos::coverage::CoverageEdge;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::GeometryFactory;
using geos::geom::LineSegment;
using geos::geom::LineString;
using geos::geom::MultiLineString;

std::unique_ptr<CoordinateSequence> MakeSequence(
    std::initializer_list<Coordinate> coords) {
  return std::make_unique<CoordinateSequence>(coords);
}

void ExpectSequenceEquals(const CoordinateSequence& actual,
                          std::initializer_list<Coordinate> expected) {
  ASSERT_EQ(actual.size(), expected.size());
  std::size_t idx = 0;
  for (const Coordinate& exp : expected) {
    EXPECT_EQ(actual.getAt(idx), exp) << "at index " << idx;
    ++idx;
  }
}

TEST(CoverageEdgeTest, ConstructorHandlesValidAndNullSequences) {
  auto seq =
      MakeSequence({Coordinate(1, 2), Coordinate(3, 4), Coordinate(5, 6)});
  CoverageEdge free_edge(std::move(seq), /*isFreeRing=*/true);
  EXPECT_TRUE(free_edge.isFreeRing());
  EXPECT_EQ(free_edge.getRingCount(), 0U);
  free_edge.incRingCount();
  free_edge.incRingCount();
  EXPECT_EQ(free_edge.getRingCount(), 2U);
  EXPECT_EQ(free_edge.getStartCoordinate(), Coordinate(1, 2));
  EXPECT_EQ(free_edge.getEndCoordinate(), Coordinate(5, 6));
  ExpectSequenceEquals(*free_edge.getCoordinates(),
                       {Coordinate(1, 2), Coordinate(3, 4), Coordinate(5, 6)});

  CoverageEdge null_edge(nullptr, /*isFreeRing=*/false);
  EXPECT_FALSE(null_edge.isFreeRing());
  EXPECT_EQ(null_edge.getRingCount(), 0U);
  ASSERT_NE(null_edge.getCoordinates(), nullptr);
  EXPECT_TRUE(null_edge.getCoordinates()->isEmpty());

  auto replacement = MakeSequence({Coordinate(7, 8), Coordinate(9, 10)});
  null_edge.setCoordinates(replacement.get());
  replacement->setAt(Coordinate(99, 99), 0);
  EXPECT_EQ(null_edge.getStartCoordinate(), Coordinate(7, 8));
  EXPECT_EQ(null_edge.getEndCoordinate(), Coordinate(9, 10));
}

TEST(CoverageEdgeTest, CreateEdgeFromFullRing) {
  auto ring =
      MakeSequence({Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
                    Coordinate(0, 10), Coordinate(0, 0)});
  std::unique_ptr<CoverageEdge> edge = CoverageEdge::createEdge(*ring);
  ASSERT_NE(edge, nullptr);
  EXPECT_TRUE(edge->isFreeRing());
  EXPECT_EQ(edge->getRingCount(), 0U);
  EXPECT_EQ(edge->getStartCoordinate(), Coordinate(0, 0));
  EXPECT_EQ(edge->getEndCoordinate(), Coordinate(0, 0));
  ExpectSequenceEquals(*edge->getCoordinates(),
                       {Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
                        Coordinate(0, 10), Coordinate(0, 0)});
}

TEST(CoverageEdgeTest, CreateEdgeSectionWithoutWrap) {
  auto ring =
      MakeSequence({Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
                    Coordinate(0, 10), Coordinate(0, 0)});
  std::unique_ptr<CoverageEdge> edge = CoverageEdge::createEdge(*ring, 1, 3);
  ASSERT_NE(edge, nullptr);
  EXPECT_FALSE(edge->isFreeRing());
  EXPECT_EQ(edge->getStartCoordinate(), Coordinate(10, 0));
  EXPECT_EQ(edge->getEndCoordinate(), Coordinate(0, 10));
  ExpectSequenceEquals(
      *edge->getCoordinates(),
      {Coordinate(10, 0), Coordinate(10, 10), Coordinate(0, 10)});
}

TEST(CoverageEdgeTest, CreateEdgeSectionWithWrapAround) {
  auto ring =
      MakeSequence({Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
                    Coordinate(0, 10), Coordinate(0, 0)});

  std::unique_ptr<CoverageEdge> wrapped = CoverageEdge::createEdge(*ring, 3, 1);
  EXPECT_FALSE(wrapped->isFreeRing());
  ExpectSequenceEquals(
      *wrapped->getCoordinates(),
      {Coordinate(0, 10), Coordinate(0, 0), Coordinate(10, 0)});

  std::unique_ptr<CoverageEdge> from_closure =
      CoverageEdge::createEdge(*ring, 4, 2);
  ExpectSequenceEquals(
      *from_closure->getCoordinates(),
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10)});

  std::unique_ptr<CoverageEdge> rotated_full =
      CoverageEdge::createEdge(*ring, 2, 2);
  ExpectSequenceEquals(*rotated_full->getCoordinates(),
                       {Coordinate(10, 10), Coordinate(0, 10), Coordinate(0, 0),
                        Coordinate(10, 0), Coordinate(10, 10)});

  std::unique_ptr<CoverageEdge> zero_to_zero =
      CoverageEdge::createEdge(*ring, 0, 0);
  ExpectSequenceEquals(*zero_to_zero->getCoordinates(),
                       {Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
                        Coordinate(0, 10), Coordinate(0, 0)});
}

TEST(CoverageEdgeTest, ToLineStringAndCreateLines) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto ring =
      MakeSequence({Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
                    Coordinate(0, 10), Coordinate(0, 0)});
  std::unique_ptr<CoverageEdge> edge1 = CoverageEdge::createEdge(*ring, 0, 2);
  std::unique_ptr<CoverageEdge> edge2 = CoverageEdge::createEdge(*ring, 2, 4);

  std::unique_ptr<LineString> ls = edge1->toLineString(factory.get());
  ASSERT_NE(ls, nullptr);
  EXPECT_EQ(ls->getNumPoints(), 3U);
  ExpectSequenceEquals(
      *ls->getCoordinatesRO(),
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10)});

  std::vector<CoverageEdge*> empty_edges;
  std::unique_ptr<MultiLineString> empty_mls =
      CoverageEdge::createLines(empty_edges, factory.get());
  ASSERT_NE(empty_mls, nullptr);
  EXPECT_TRUE(empty_mls->isEmpty());
  EXPECT_EQ(empty_mls->getNumGeometries(), 0U);

  std::vector<CoverageEdge*> edges = {edge1.get(), edge2.get()};
  std::unique_ptr<MultiLineString> mls =
      CoverageEdge::createLines(edges, factory.get());
  ASSERT_NE(mls, nullptr);
  ASSERT_EQ(mls->getNumGeometries(), 2U);
  ExpectSequenceEquals(
      *mls->getGeometryN(0)->getCoordinatesRO(),
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10)});
  ExpectSequenceEquals(
      *mls->getGeometryN(1)->getCoordinatesRO(),
      {Coordinate(10, 10), Coordinate(0, 10), Coordinate(0, 0)});
}

TEST(CoverageEdgeTest, KeyForRingIsOrientationAndStartInvariant) {
  auto max_at_start =
      MakeSequence({Coordinate(10, 10), Coordinate(0, 0), Coordinate(10, 0),
                    Coordinate(0, 10), Coordinate(10, 10)});
  LineSegment start_key = CoverageEdge::key(*max_at_start);
  EXPECT_EQ(start_key.p0, Coordinate(10, 10));
  EXPECT_EQ(start_key.p1, Coordinate(0, 0));

  auto ccw =
      MakeSequence({Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
                    Coordinate(0, 10), Coordinate(0, 0)});
  auto cw =
      MakeSequence({Coordinate(0, 0), Coordinate(0, 10), Coordinate(10, 10),
                    Coordinate(10, 0), Coordinate(0, 0)});
  auto rotated =
      MakeSequence({Coordinate(10, 0), Coordinate(10, 10), Coordinate(0, 10),
                    Coordinate(0, 0), Coordinate(10, 0)});

  LineSegment key_ccw = CoverageEdge::key(*ccw);
  LineSegment key_cw = CoverageEdge::key(*cw);
  LineSegment key_rotated = CoverageEdge::key(*rotated);

  EXPECT_EQ(key_ccw.p0, Coordinate(10, 10));
  EXPECT_EQ(key_ccw.p1, Coordinate(0, 10));
  EXPECT_TRUE(key_ccw == key_cw);
  EXPECT_TRUE(key_ccw == key_rotated);
}

TEST(CoverageEdgeTest, KeyForRingSkipsRepeatedPointsAndWraps) {
  auto repeated_at_start =
      MakeSequence({Coordinate(10, 10), Coordinate(10, 10), Coordinate(0, 5),
                    Coordinate(5, 0), Coordinate(10, 10)});
  LineSegment key_start = CoverageEdge::key(*repeated_at_start);
  EXPECT_EQ(key_start.p0, Coordinate(10, 10));
  EXPECT_EQ(key_start.p1, Coordinate(0, 5));

  auto repeated_at_end = MakeSequence({Coordinate(0, 0), Coordinate(5, 0),
                                       Coordinate(10, 10), Coordinate(10, 10)});
  LineSegment key_end = CoverageEdge::key(*repeated_at_end);
  EXPECT_EQ(key_end.p0, Coordinate(10, 10));
  EXPECT_EQ(key_end.p1, Coordinate(0, 0));
}

TEST(CoverageEdgeTest, KeyForSectionForwardReverseAndWrapped) {
  auto ccw =
      MakeSequence({Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
                    Coordinate(0, 10), Coordinate(0, 0)});
  auto cw =
      MakeSequence({Coordinate(0, 0), Coordinate(0, 10), Coordinate(10, 10),
                    Coordinate(10, 0), Coordinate(0, 0)});

  LineSegment forward_key = CoverageEdge::key(*cw, 1, 3);
  LineSegment reverse_key = CoverageEdge::key(*ccw, 1, 3);
  EXPECT_EQ(forward_key.p0, Coordinate(0, 10));
  EXPECT_EQ(forward_key.p1, Coordinate(10, 10));
  EXPECT_TRUE(forward_key == reverse_key);

  LineSegment wrap_forward = CoverageEdge::key(*ccw, 4, 2);
  EXPECT_EQ(wrap_forward.p0, Coordinate(0, 0));
  EXPECT_EQ(wrap_forward.p1, Coordinate(10, 0));

  LineSegment wrap_backward = CoverageEdge::key(*ccw, 2, 0);
  EXPECT_EQ(wrap_backward.p0, Coordinate(0, 0));
  EXPECT_EQ(wrap_backward.p1, Coordinate(0, 10));

  auto repeated =
      MakeSequence({Coordinate(0, 0), Coordinate(0, 0), Coordinate(5, 2),
                    Coordinate(10, 0), Coordinate(10, 0), Coordinate(0, 0)});
  LineSegment repeated_key = CoverageEdge::key(*repeated, 0, 3);
  EXPECT_EQ(repeated_key.p0, Coordinate(0, 0));
  EXPECT_EQ(repeated_key.p1, Coordinate(5, 2));
}

TEST(CoverageEdgeTest, KeyThrowsWhenNoDistinctPoints) {
  auto all_equal = MakeSequence(
      {Coordinate(2, 3), Coordinate(2, 3), Coordinate(2, 3), Coordinate(2, 3)});
  EXPECT_THROW(CoverageEdge::key(*all_equal),
               geos::util::IllegalStateException);
  EXPECT_THROW(CoverageEdge::key(*all_equal, 0, 2),
               geos::util::IllegalStateException);

  auto single_pt = MakeSequence({Coordinate(2, 3)});
  EXPECT_THROW(CoverageEdge::key(*single_pt, 0, 0),
               geos::util::IllegalStateException);
}

}  // namespace
}  // namespace geos_google
