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

// Unit tests for third_party/geos/src/operation/overlayng/OverlayEdge.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/overlayng/OverlayEdge.cpp

#include "third_party/geos/include/geos/operation/overlayng/OverlayEdge.h"

#include <memory>
#include <sstream>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/geom/Position.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/operation/overlayng/MaximalEdgeRing.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayEdgeRing.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayGraph.h"
#include "third_party/geos/include/geos/operation/overlayng/OverlayLabel.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::geom::Position;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::operation::overlayng::MaximalEdgeRing;
using geos::operation::overlayng::OverlayEdge;
using geos::operation::overlayng::OverlayEdgeRing;
using geos::operation::overlayng::OverlayGraph;
using geos::operation::overlayng::OverlayLabel;

CoordinateXYZM MakeXY(double x, double y) {
  return CoordinateXYZM(x, y, DoubleNotANumber, DoubleNotANumber);
}

TEST(OverlayEdgeTest, AccessorsAndSymmetricDirections) {
  CoordinateSequence pts{
      CoordinateXYZM(0.0, 0.0, 10.0, 1.0),
      CoordinateXYZM(5.0, 2.0, 20.0, 2.0),
      CoordinateXYZM(10.0, 0.0, 30.0, 3.0),
  };
  OverlayLabel label(0, Location::EXTERIOR, Location::INTERIOR, false);

  OverlayEdge fwd(CoordinateXYZM(0.0, 0.0, 10.0, 1.0),
                  CoordinateXYZM(5.0, 2.0, 20.0, 2.0), true, &label, &pts);
  OverlayEdge rev(CoordinateXYZM(10.0, 0.0, 30.0, 3.0),
                  CoordinateXYZM(5.0, 2.0, 20.0, 2.0), false, &label, &pts);
  fwd.link(&rev);

  EXPECT_TRUE(fwd.isForward());
  EXPECT_FALSE(rev.isForward());
  EXPECT_EQ(fwd.symOE(), &rev);
  EXPECT_EQ(rev.symOE(), &fwd);
  EXPECT_EQ(fwd.getLabel(), &label);
  EXPECT_EQ(rev.getLabel(), &label);
  EXPECT_EQ(fwd.getCoordinatesRO(), &pts);

  EXPECT_TRUE(
      fwd.getCoordinate().equals4D(CoordinateXYZM(0.0, 0.0, 10.0, 1.0)));
  EXPECT_TRUE(fwd.directionPt().equals4D(CoordinateXYZM(5.0, 2.0, 20.0, 2.0)));
  EXPECT_TRUE(fwd.dest().equals4D(CoordinateXYZM(10.0, 0.0, 30.0, 3.0)));
  EXPECT_TRUE(
      rev.getCoordinate().equals4D(CoordinateXYZM(10.0, 0.0, 30.0, 3.0)));
  EXPECT_TRUE(rev.directionPt().equals4D(CoordinateXYZM(5.0, 2.0, 20.0, 2.0)));
  EXPECT_TRUE(rev.dest().equals4D(CoordinateXYZM(0.0, 0.0, 10.0, 1.0)));

  EXPECT_EQ(fwd.getLocation(0, Position::LEFT), Location::EXTERIOR);
  EXPECT_EQ(fwd.getLocation(0, Position::RIGHT), Location::INTERIOR);
  EXPECT_EQ(rev.getLocation(0, Position::LEFT), Location::INTERIOR);
  EXPECT_EQ(rev.getLocation(0, Position::RIGHT), Location::EXTERIOR);

  std::unique_ptr<CoordinateSequence> copied = fwd.getCoordinates();
  ASSERT_NE(copied, nullptr);
  EXPECT_TRUE(copied->equalsIdentical(pts));
  EXPECT_NE(copied.get(), &pts);
}

TEST(OverlayEdgeTest, GetCoordinatesOrientedForwardAndBackward) {
  CoordinateSequence pts{
      CoordinateXYZM(0.0, 0.0, 1.0, 11.0),
      CoordinateXYZM(4.0, 3.0, 2.0, 22.0),
      CoordinateXYZM(8.0, 0.0, 3.0, 33.0),
  };
  OverlayLabel label(0);

  OverlayEdge fwd(CoordinateXYZM(0.0, 0.0, 1.0, 11.0),
                  CoordinateXYZM(4.0, 3.0, 2.0, 22.0), true, &label, &pts);
  OverlayEdge rev(CoordinateXYZM(8.0, 0.0, 3.0, 33.0),
                  CoordinateXYZM(4.0, 3.0, 2.0, 22.0), false, &label, &pts);
  fwd.link(&rev);

  std::unique_ptr<CoordinateSequence> fwdSeq = fwd.getCoordinatesOriented();
  ASSERT_NE(fwdSeq, nullptr);
  EXPECT_TRUE(fwdSeq->equalsIdentical(pts));

  std::unique_ptr<CoordinateSequence> revSeq = rev.getCoordinatesOriented();
  ASSERT_NE(revSeq, nullptr);
  CoordinateSequence expectedRev{
      CoordinateXYZM(8.0, 0.0, 3.0, 33.0),
      CoordinateXYZM(4.0, 3.0, 2.0, 22.0),
      CoordinateXYZM(0.0, 0.0, 1.0, 11.0),
  };
  EXPECT_TRUE(revSeq->equalsIdentical(expectedRev));
}

TEST(OverlayEdgeTest, AddCoordinatesForwardEmptyAndNonEmptyTargets) {
  CoordinateSequence pts3{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(3.0, 4.0),
      CoordinateXY(10.0, 0.0),
  };
  OverlayLabel label(0);
  OverlayEdge fwd3(MakeXY(0.0, 0.0), MakeXY(3.0, 4.0), true, &label, &pts3);
  OverlayEdge rev3(MakeXY(10.0, 0.0), MakeXY(3.0, 4.0), false, &label, &pts3);
  fwd3.link(&rev3);

  // Empty target skips index 0 so ring-building can close on the end.
  CoordinateSequence fromEmpty(0u, false, false);
  fwd3.addCoordinates(&fromEmpty);
  CoordinateSequence expectedEmpty{
      CoordinateXY(3.0, 4.0),
      CoordinateXY(10.0, 0.0),
  };
  EXPECT_TRUE(fromEmpty.equalsIdentical(expectedEmpty));

  // Non-empty target matching orig deduplicates the connecting vertex.
  CoordinateSequence matchingEnd{CoordinateXY(0.0, 0.0)};
  fwd3.addCoordinates(&matchingEnd);
  EXPECT_TRUE(matchingEnd.equalsIdentical(pts3));

  // Non-empty target with a distinct last coordinate retains all vertices.
  CoordinateSequence distinctEnd{CoordinateXY(-1.0, -1.0)};
  fwd3.addCoordinates(&distinctEnd);
  CoordinateSequence expectedDistinct{
      CoordinateXY(-1.0, -1.0),
      CoordinateXY(0.0, 0.0),
      CoordinateXY(3.0, 4.0),
      CoordinateXY(10.0, 0.0),
  };
  EXPECT_TRUE(distinctEnd.equalsIdentical(expectedDistinct));

  // 2-point forward edge on empty vs non-empty targets.
  CoordinateSequence pts2{CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 0.0)};
  OverlayEdge fwd2(MakeXY(0.0, 0.0), MakeXY(5.0, 0.0), true, &label, &pts2);
  OverlayEdge rev2(MakeXY(5.0, 0.0), MakeXY(0.0, 0.0), false, &label, &pts2);
  fwd2.link(&rev2);

  CoordinateSequence empty2(0u, false, false);
  fwd2.addCoordinates(&empty2);
  CoordinateSequence expected2{CoordinateXY(5.0, 0.0)};
  EXPECT_TRUE(empty2.equalsIdentical(expected2));
}

TEST(OverlayEdgeTest, AddCoordinatesBackwardEmptyAndNonEmptyAndPreservesZ) {
  CoordinateSequence pts3{
      Coordinate(0.0, 0.0, 10.0),
      Coordinate(3.0, 4.0, 20.0),
      Coordinate(10.0, 0.0, 30.0),
  };
  OverlayLabel label(0);
  OverlayEdge fwd3(CoordinateXYZM(0.0, 0.0, 10.0, DoubleNotANumber),
                   CoordinateXYZM(3.0, 4.0, 20.0, DoubleNotANumber), true,
                   &label, &pts3);
  OverlayEdge rev3(CoordinateXYZM(10.0, 0.0, 30.0, DoubleNotANumber),
                   CoordinateXYZM(3.0, 4.0, 20.0, DoubleNotANumber), false,
                   &label, &pts3);
  fwd3.link(&rev3);

  CoordinateSequence fromEmpty(0u, true, false);
  rev3.addCoordinates(&fromEmpty);
  CoordinateSequence expectedEmpty{
      Coordinate(3.0, 4.0, 20.0),
      Coordinate(0.0, 0.0, 10.0),
  };
  EXPECT_TRUE(fromEmpty.equalsIdentical(expectedEmpty));

  CoordinateSequence matchingEnd{Coordinate(10.0, 0.0, 30.0)};
  rev3.addCoordinates(&matchingEnd);
  CoordinateSequence expectedMatched{
      Coordinate(10.0, 0.0, 30.0),
      Coordinate(3.0, 4.0, 20.0),
      Coordinate(0.0, 0.0, 10.0),
  };
  EXPECT_TRUE(matchingEnd.equalsIdentical(expectedMatched));

  CoordinateSequence distinctEnd{Coordinate(99.0, 99.0, 99.0)};
  rev3.addCoordinates(&distinctEnd);
  CoordinateSequence expectedDistinct{
      Coordinate(99.0, 99.0, 99.0),
      Coordinate(10.0, 0.0, 30.0),
      Coordinate(3.0, 4.0, 20.0),
      Coordinate(0.0, 0.0, 10.0),
  };
  EXPECT_TRUE(distinctEnd.equalsIdentical(expectedDistinct));

  // 2-point backward edge on empty target.
  CoordinateSequence pts2{CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 0.0)};
  OverlayEdge fwd2(MakeXY(0.0, 0.0), MakeXY(5.0, 0.0), true, &label, &pts2);
  OverlayEdge rev2(MakeXY(5.0, 0.0), MakeXY(0.0, 0.0), false, &label, &pts2);
  fwd2.link(&rev2);

  CoordinateSequence empty2(0u, false, false);
  rev2.addCoordinates(&empty2);
  CoordinateSequence expected2{CoordinateXY(0.0, 0.0)};
  EXPECT_TRUE(empty2.equalsIdentical(expected2));
}

TEST(OverlayEdgeTest, ResultAreaAndLineMarksAndSymbols) {
  CoordinateSequence pts{CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)};
  OverlayLabel label(0, Location::EXTERIOR, Location::INTERIOR, false);
  OverlayEdge fwd(MakeXY(0.0, 0.0), MakeXY(10.0, 0.0), true, &label, &pts);
  OverlayEdge rev(MakeXY(10.0, 0.0), MakeXY(0.0, 0.0), false, &label, &pts);
  fwd.link(&rev);

  EXPECT_FALSE(fwd.isInResultArea());
  EXPECT_FALSE(fwd.isInResultLine());
  EXPECT_FALSE(fwd.isInResult());
  EXPECT_FALSE(fwd.isInResultEither());
  EXPECT_FALSE(fwd.isInResultAreaBoth());
  EXPECT_EQ(fwd.resultSymbol(), "");

  fwd.markInResultArea();
  EXPECT_TRUE(fwd.isInResultArea());
  EXPECT_FALSE(rev.isInResultArea());
  EXPECT_TRUE(fwd.isInResult());
  EXPECT_FALSE(rev.isInResult());
  EXPECT_TRUE(fwd.isInResultEither());
  EXPECT_TRUE(rev.isInResultEither());
  EXPECT_FALSE(fwd.isInResultAreaBoth());
  EXPECT_EQ(fwd.resultSymbol(), " resA");
  EXPECT_EQ(rev.resultSymbol(), "");

  fwd.markInResultAreaBoth();
  EXPECT_TRUE(fwd.isInResultArea());
  EXPECT_TRUE(rev.isInResultArea());
  EXPECT_TRUE(fwd.isInResultAreaBoth());
  EXPECT_TRUE(rev.isInResultAreaBoth());

  fwd.unmarkFromResultAreaBoth();
  EXPECT_FALSE(fwd.isInResultArea());
  EXPECT_FALSE(rev.isInResultArea());
  EXPECT_FALSE(fwd.isInResultAreaBoth());
  EXPECT_FALSE(fwd.isInResultEither());
  EXPECT_EQ(fwd.resultSymbol(), "");

  fwd.markInResultLine();
  EXPECT_TRUE(fwd.isInResultLine());
  EXPECT_TRUE(rev.isInResultLine());
  EXPECT_TRUE(fwd.isInResult());
  EXPECT_TRUE(rev.isInResult());
  EXPECT_TRUE(fwd.isInResultEither());
  EXPECT_EQ(fwd.resultSymbol(), " resL");
  EXPECT_EQ(rev.resultSymbol(), " resL");
}

TEST(OverlayEdgeTest, LinkageVisitedAndRingMembership) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  OverlayGraph graph;
  EdgeSourceInfo esi(0, 1, false);

  auto addSeg = [&](Coordinate a, Coordinate b) -> OverlayEdge* {
    auto seq = std::make_unique<CoordinateSequence>(0u, false, false);
    seq->add(a);
    seq->add(b);
    Edge edge(std::move(seq), &esi);
    return graph.addEdge(&edge);
  };

  OverlayEdge* e0 = addSeg(Coordinate(0.0, 0.0), Coordinate(0.0, 10.0));
  OverlayEdge* e1 = addSeg(Coordinate(0.0, 10.0), Coordinate(10.0, 0.0));
  OverlayEdge* e2 = addSeg(Coordinate(10.0, 0.0), Coordinate(0.0, 0.0));

  EXPECT_FALSE(e0->isResultLinked());
  EXPECT_EQ(e0->nextResult(), nullptr);
  EXPECT_FALSE(e0->isResultMaxLinked());
  EXPECT_EQ(e0->nextResultMax(), nullptr);
  EXPECT_FALSE(e0->isVisited());
  EXPECT_FALSE(e0->symOE()->isVisited());
  EXPECT_EQ(e0->getEdgeRing(), nullptr);
  EXPECT_EQ(e0->getEdgeRingMax(), nullptr);

  // Verify oNextOE() visits the other edge originating at (0, 0) (e2->symOE()).
  EXPECT_EQ(e0->oNextOE(), e2->symOE());

  e0->markVisitedBoth();
  EXPECT_TRUE(e0->isVisited());
  EXPECT_TRUE(e0->symOE()->isVisited());

  e0->setNextResult(e1);
  e1->setNextResult(e2);
  e2->setNextResult(e0);
  EXPECT_TRUE(e0->isResultLinked());
  EXPECT_EQ(e0->nextResult(), e1);

  e0->setNextResultMax(e1);
  e1->setNextResultMax(e2);
  e2->setNextResultMax(e0);
  EXPECT_TRUE(e0->isResultMaxLinked());
  EXPECT_EQ(e0->nextResultMax(), e1);

  MaximalEdgeRing maxRing(e0);
  EXPECT_EQ(e0->getEdgeRingMax(), &maxRing);
  EXPECT_EQ(e1->getEdgeRingMax(), &maxRing);
  EXPECT_EQ(e2->getEdgeRingMax(), &maxRing);

  OverlayEdgeRing edgeRing(e0, factory.get());
  EXPECT_EQ(e0->getEdgeRing(), &edgeRing);
  EXPECT_EQ(e1->getEdgeRing(), &edgeRing);
  EXPECT_EQ(e2->getEdgeRing(), &edgeRing);
}

TEST(OverlayEdgeTest, StreamInsertionFormatsTwoAndMultiPointEdges) {
  OverlayLabel label(0, Location::EXTERIOR, Location::INTERIOR, false);

  CoordinateSequence pts2{CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)};
  OverlayEdge fwd2(MakeXY(0.0, 0.0), MakeXY(10.0, 0.0), true, &label, &pts2);
  OverlayEdge rev2(MakeXY(10.0, 0.0), MakeXY(0.0, 0.0), false, &label, &pts2);
  fwd2.link(&rev2);

  {
    std::ostringstream oss;
    oss << fwd2;
    EXPECT_EQ(oss.str(), "OE( 0 0 .. 10 0 ) A:eiB/B:- / Sym: A:ieB/B:-");
  }

  fwd2.markInResultArea();
  {
    std::ostringstream oss;
    oss << fwd2;
    EXPECT_EQ(oss.str(), "OE( 0 0 .. 10 0 ) A:eiB/B:- resA / Sym: A:ieB/B:-");
  }

  CoordinateSequence pts3{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(5.0, 2.0),
      CoordinateXY(10.0, 0.0),
  };
  OverlayEdge fwd3(MakeXY(0.0, 0.0), MakeXY(5.0, 2.0), true, &label, &pts3);
  OverlayEdge rev3(MakeXY(10.0, 0.0), MakeXY(5.0, 2.0), false, &label, &pts3);
  fwd3.link(&rev3);
  fwd3.markInResultLine();

  std::ostringstream oss3;
  oss3 << fwd3;
  EXPECT_EQ(oss3.str(),
            "OE( 0 0, 5 2 .. 10 0 ) A:eiB/B:- resL / Sym: A:ieB/B:- resL");
}

}  // namespace
}  // namespace geos_google
