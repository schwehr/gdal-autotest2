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

#include "third_party/geos/include/geos/noding/IntersectionFinderAdder.h"

#include <cmath>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/noding/MCIndexNoder.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using ::geos::algorithm::LineIntersector;
using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::PrecisionModel;
using ::geos::noding::IntersectionFinderAdder;
using ::geos::noding::MCIndexNoder;
using ::geos::noding::NodedSegmentString;
using ::geos::noding::SegmentString;
using ::testing::DoubleEq;
using ::testing::IsEmpty;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

std::unique_ptr<NodedSegmentString> MakeNodedXY(
    std::initializer_list<CoordinateXY> coords, const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<NodedSegmentString>(seq.release(), false, false,
                                              context);
}

std::unique_ptr<NodedSegmentString> MakeNodedXYZ(
    std::initializer_list<Coordinate> coords, const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(coords);
  return std::make_unique<NodedSegmentString>(seq.release(), true, false,
                                              context);
}

void DeleteSegmentStringVector(SegmentString::NonConstVect* vec) {
  if (vec == nullptr) {
    return;
  }
  for (SegmentString* ss : *vec) {
    delete ss;
  }
  delete vec;
}

TEST(IntersectionFinderAdderTest, InitialStateIsDoneAndAppendsToVector) {
  LineIntersector li;
  Coordinate preExisting(99.0, 99.0);
  std::vector<Coordinate> intersections{preExisting};

  IntersectionFinderAdder adder(li, intersections);
  EXPECT_FALSE(adder.isDone());
  EXPECT_EQ(&adder.getInteriorIntersections(), &intersections);
  ASSERT_THAT(adder.getInteriorIntersections(), SizeIs(1));
  EXPECT_EQ(adder.getInteriorIntersections()[0], preExisting);

  auto ss0 = MakeNodedXY({{0.0, 0.0}, {4.0, 4.0}});
  auto ss1 = MakeNodedXY({{0.0, 4.0}, {4.0, 0.0}});
  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  EXPECT_FALSE(adder.isDone());
  ASSERT_THAT(adder.getInteriorIntersections(), SizeIs(2));
  EXPECT_EQ(adder.getInteriorIntersections()[0], preExisting);
  EXPECT_EQ(adder.getInteriorIntersections()[1], Coordinate(2.0, 2.0));
}

TEST(IntersectionFinderAdderTest, SkipsSelfComparisonSameSegment) {
  LineIntersector li;
  std::vector<Coordinate> intersections;
  IntersectionFinderAdder adder(li, intersections);

  auto ss = MakeNodedXY({{0.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}});
  adder.processIntersections(ss.get(), 0, ss.get(), 0);
  adder.processIntersections(ss.get(), 1, ss.get(), 1);

  EXPECT_THAT(intersections, IsEmpty());
  EXPECT_FALSE(ss->hasNodes());
}

TEST(IntersectionFinderAdderTest, DisjointSegmentsProduceNoIntersections) {
  LineIntersector li;
  std::vector<Coordinate> intersections;
  IntersectionFinderAdder adder(li, intersections);

  auto ss0 = MakeNodedXY({{0.0, 0.0}, {3.0, 0.0}});
  auto ss1 = MakeNodedXY({{0.0, 1.0}, {3.0, 1.0}});
  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  EXPECT_THAT(intersections, IsEmpty());
  EXPECT_FALSE(ss0->hasNodes());
  EXPECT_FALSE(ss1->hasNodes());
}

TEST(IntersectionFinderAdderTest, EndpointOnlyTouchesProduceNoInteriorNodes) {
  LineIntersector li;
  std::vector<Coordinate> intersections;
  IntersectionFinderAdder adder(li, intersections);

  // Corner intersection at shared endpoint (5, 0).
  auto cornerA = MakeNodedXY({{0.0, 0.0}, {5.0, 0.0}});
  auto cornerB = MakeNodedXY({{5.0, 0.0}, {5.0, 5.0}});
  adder.processIntersections(cornerA.get(), 0, cornerB.get(), 0);
  EXPECT_THAT(intersections, IsEmpty());
  EXPECT_FALSE(cornerA->hasNodes());
  EXPECT_FALSE(cornerB->hasNodes());

  // Collinear touching only at endpoint (5, 0).
  auto abutA = MakeNodedXY({{0.0, 0.0}, {5.0, 0.0}});
  auto abutB = MakeNodedXY({{5.0, 0.0}, {10.0, 0.0}});
  adder.processIntersections(abutA.get(), 0, abutB.get(), 0);
  EXPECT_THAT(intersections, IsEmpty());
  EXPECT_FALSE(abutA->hasNodes());
  EXPECT_FALSE(abutB->hasNodes());

  // Identical segments on two distinct SegmentStrings only intersect at
  // endpoints of both segments, so neither intersection point is interior.
  auto dupA = MakeNodedXY({{0.0, 0.0}, {5.0, 0.0}});
  auto dupB = MakeNodedXY({{0.0, 0.0}, {5.0, 0.0}});
  adder.processIntersections(dupA.get(), 0, dupB.get(), 0);
  EXPECT_THAT(intersections, IsEmpty());
  EXPECT_FALSE(dupA->hasNodes());
  EXPECT_FALSE(dupB->hasNodes());

  // Adjacent segments on the same SegmentString meeting cleanly at vertex 1.
  auto poly = MakeNodedXY({{0.0, 0.0}, {5.0, 0.0}, {5.0, 5.0}});
  adder.processIntersections(poly.get(), 0, poly.get(), 1);
  EXPECT_THAT(intersections, IsEmpty());
  EXPECT_FALSE(poly->hasNodes());
}

TEST(IntersectionFinderAdderTest,
     ProperCrossingAddsIntersectionAndSplitsEdges) {
  LineIntersector li;
  std::vector<Coordinate> intersections;
  IntersectionFinderAdder adder(li, intersections);

  int ctx0 = 1;
  int ctx1 = 2;
  auto ss0 = MakeNodedXY({{0.0, 0.0}, {10.0, 10.0}}, &ctx0);
  auto ss1 = MakeNodedXY({{0.0, 10.0}, {10.0, 0.0}}, &ctx1);

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  ASSERT_THAT(intersections, SizeIs(1));
  EXPECT_EQ(intersections[0], Coordinate(5.0, 5.0));
  EXPECT_TRUE(std::isnan(intersections[0].z));
  EXPECT_TRUE(ss0->hasNodes());
  EXPECT_TRUE(ss1->hasNodes());

  SegmentString::NonConstVect inputs{ss0.get(), ss1.get()};
  SegmentString::NonConstVect* substrings =
      NodedSegmentString::getNodedSubstrings(inputs);
  absl::Cleanup cleanup_substrings = [&] {
    DeleteSegmentStringVector(substrings);
  };

  ASSERT_THAT(*substrings, SizeIs(4));
  EXPECT_EQ((*substrings)[0]->getData(), &ctx0);
  EXPECT_EQ((*substrings)[1]->getData(), &ctx0);
  EXPECT_EQ((*substrings)[2]->getData(), &ctx1);
  EXPECT_EQ((*substrings)[3]->getData(), &ctx1);
  EXPECT_EQ((*substrings)[0]->getCoordinate(1), Coordinate(5.0, 5.0));
  EXPECT_EQ((*substrings)[1]->getCoordinate(0), Coordinate(5.0, 5.0));
  EXPECT_EQ((*substrings)[2]->getCoordinate(1), Coordinate(5.0, 5.0));
  EXPECT_EQ((*substrings)[3]->getCoordinate(0), Coordinate(5.0, 5.0));
}

TEST(IntersectionFinderAdderTest, TIntersectionAddsNodeToSplitSegment) {
  LineIntersector li;
  std::vector<Coordinate> intersections;
  IntersectionFinderAdder adder(li, intersections);

  auto horiz = MakeNodedXY({{0.0, 0.0}, {10.0, 0.0}});
  auto stem = MakeNodedXY({{4.0, 0.0}, {4.0, 6.0}});

  adder.processIntersections(horiz.get(), 0, stem.get(), 0);

  ASSERT_THAT(intersections, SizeIs(1));
  EXPECT_EQ(intersections[0], Coordinate(4.0, 0.0));
  EXPECT_TRUE(horiz->hasNodes());
  EXPECT_TRUE(stem->hasNodes());

  SegmentString::NonConstVect inputs{horiz.get(), stem.get()};
  SegmentString::NonConstVect* substrings =
      NodedSegmentString::getNodedSubstrings(inputs);
  absl::Cleanup cleanup_substrings = [&] {
    DeleteSegmentStringVector(substrings);
  };

  ASSERT_THAT(*substrings, SizeIs(3));
  EXPECT_EQ((*substrings)[0]->getCoordinate(0), Coordinate(0.0, 0.0));
  EXPECT_EQ((*substrings)[0]->getCoordinate(1), Coordinate(4.0, 0.0));
  EXPECT_EQ((*substrings)[1]->getCoordinate(0), Coordinate(4.0, 0.0));
  EXPECT_EQ((*substrings)[1]->getCoordinate(1), Coordinate(10.0, 0.0));
  EXPECT_EQ((*substrings)[2]->getCoordinate(0), Coordinate(4.0, 0.0));
  EXPECT_EQ((*substrings)[2]->getCoordinate(1), Coordinate(4.0, 6.0));
}

TEST(IntersectionFinderAdderTest, CollinearPartialOverlapRecordsBothPoints) {
  LineIntersector li;
  std::vector<Coordinate> intersections;
  IntersectionFinderAdder adder(li, intersections);

  auto ss0 = MakeNodedXY({{0.0, 0.0}, {10.0, 0.0}});
  auto ss1 = MakeNodedXY({{4.0, 0.0}, {14.0, 0.0}});

  adder.processIntersections(ss0.get(), 0, ss1.get(), 0);

  EXPECT_THAT(intersections, UnorderedElementsAre(Coordinate(4.0, 0.0),
                                                  Coordinate(10.0, 0.0)));
  EXPECT_TRUE(ss0->hasNodes());
  EXPECT_TRUE(ss1->hasNodes());

  SegmentString::NonConstVect inputs{ss0.get(), ss1.get()};
  SegmentString::NonConstVect* substrings =
      NodedSegmentString::getNodedSubstrings(inputs);
  absl::Cleanup cleanup_substrings = [&] {
    DeleteSegmentStringVector(substrings);
  };

  ASSERT_THAT(*substrings, SizeIs(4));
}

TEST(IntersectionFinderAdderTest, CollinearContainmentSplitsOuterSegment) {
  LineIntersector li;
  std::vector<Coordinate> intersections;
  IntersectionFinderAdder adder(li, intersections);

  auto outer = MakeNodedXY({{0.0, 0.0}, {10.0, 0.0}});
  auto inner = MakeNodedXY({{2.0, 0.0}, {7.0, 0.0}});

  adder.processIntersections(outer.get(), 0, inner.get(), 0);

  EXPECT_THAT(intersections,
              UnorderedElementsAre(Coordinate(2.0, 0.0), Coordinate(7.0, 0.0)));

  SegmentString::NonConstVect inputs{outer.get(), inner.get()};
  SegmentString::NonConstVect* substrings =
      NodedSegmentString::getNodedSubstrings(inputs);
  absl::Cleanup cleanup_substrings = [&] {
    DeleteSegmentStringVector(substrings);
  };

  // Outer splits into 3 pieces (0-2, 2-7, 7-10) and inner remains 1 piece.
  ASSERT_THAT(*substrings, SizeIs(4));
}

TEST(IntersectionFinderAdderTest, SelfIntersectionOnSameSegmentString) {
  LineIntersector li;
  std::vector<Coordinate> intersections;
  IntersectionFinderAdder adder(li, intersections);

  // Non-adjacent segments (0 and 2) cross at (2, 2).
  auto bowtie = MakeNodedXY({{0.0, 0.0}, {4.0, 4.0}, {4.0, 0.0}, {0.0, 4.0}});
  adder.processIntersections(bowtie.get(), 0, bowtie.get(), 2);

  ASSERT_THAT(intersections, SizeIs(1));
  EXPECT_EQ(intersections[0], Coordinate(2.0, 2.0));
  EXPECT_TRUE(bowtie->hasNodes());

  SegmentString::NonConstVect inputs{bowtie.get()};
  SegmentString::NonConstVect* substrings =
      NodedSegmentString::getNodedSubstrings(inputs);
  absl::Cleanup cleanup_substrings = [&] {
    DeleteSegmentStringVector(substrings);
  };
  // Bowtie splits at (2, 2) on segments 0 and 2 into 3 substrings:
  // (0,0)-(2,2), (2,2)-(4,4)-(4,0)-(2,2), and (2,2)-(0,4).
  ASSERT_THAT(*substrings, SizeIs(3));
  EXPECT_EQ((*substrings)[0]->size(), 2u);
  EXPECT_EQ((*substrings)[1]->size(), 4u);
  EXPECT_EQ((*substrings)[2]->size(), 2u);

  // Adjacent segments folding back along the same line overlap collinearly
  // along [2, 6]; because (2, 0) is interior to segment 0, both endpoints
  // of the collinear intersection are recorded.
  intersections.clear();
  auto folded = MakeNodedXY({{0.0, 0.0}, {6.0, 0.0}, {2.0, 0.0}});
  adder.processIntersections(folded.get(), 0, folded.get(), 1);
  EXPECT_THAT(intersections,
              UnorderedElementsAre(Coordinate(2.0, 0.0), Coordinate(6.0, 0.0)));
  EXPECT_TRUE(folded->hasNodes());
}

TEST(IntersectionFinderAdderTest, FixedPrecisionAndZInterpolation) {
  PrecisionModel fixedPm(1.0);
  LineIntersector fixedLi(&fixedPm);
  std::vector<Coordinate> fixedInts;
  IntersectionFinderAdder fixedAdder(fixedLi, fixedInts);

  auto s0 = MakeNodedXY({{0.0, 0.0}, {10.0, 3.0}});
  auto s1 = MakeNodedXY({{5.0, 0.0}, {5.0, 5.0}});
  fixedAdder.processIntersections(s0.get(), 0, s1.get(), 0);

  ASSERT_THAT(fixedInts, SizeIs(1));
  EXPECT_EQ(fixedInts[0], Coordinate(5.0, 2.0));

  LineIntersector xyzLi;
  std::vector<Coordinate> xyzInts;
  IntersectionFinderAdder xyzAdder(xyzLi, xyzInts);

  auto z0 = MakeNodedXYZ({{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}});
  auto z1 = MakeNodedXYZ({{0.0, 10.0, 20.0}, {10.0, 0.0, 20.0}});
  xyzAdder.processIntersections(z0.get(), 0, z1.get(), 0);

  ASSERT_THAT(xyzInts, SizeIs(1));
  EXPECT_THAT(xyzInts[0].x, DoubleEq(5.0));
  EXPECT_THAT(xyzInts[0].y, DoubleEq(5.0));
  EXPECT_THAT(xyzInts[0].z, DoubleEq(12.5));
}

TEST(IntersectionFinderAdderTest, IntegrationWithMCIndexNoder) {
  LineIntersector li;
  std::vector<Coordinate> intersections;
  IntersectionFinderAdder adder(li, intersections);

  auto diag = MakeNodedXY({{0.0, 0.0}, {10.0, 10.0}});
  auto cross = MakeNodedXY({{0.0, 10.0}, {10.0, 0.0}});
  auto horiz = MakeNodedXY({{0.0, 2.0}, {10.0, 2.0}});

  SegmentString::NonConstVect inputs{diag.get(), cross.get(), horiz.get()};
  MCIndexNoder noder(&adder);
  noder.computeNodes(&inputs);

  EXPECT_FALSE(adder.isDone());
  EXPECT_THAT(adder.getInteriorIntersections(),
              UnorderedElementsAre(Coordinate(5.0, 5.0), Coordinate(2.0, 2.0),
                                   Coordinate(8.0, 2.0)));

  SegmentString::NonConstVect* noded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };
  // diag split into 3, cross split into 3, horiz split into 3 = 9 substrings.
  EXPECT_THAT(*noded, SizeIs(9));
}

}  // namespace
}  // namespace geos_google
