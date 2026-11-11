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

#include "third_party/geos/include/geos/noding/BoundaryChainNoder.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::noding::BasicSegmentString;
using geos::noding::BoundaryChainNoder;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentString;
using testing::UnorderedElementsAre;

// BoundaryChainNoder::substring returns BasicSegmentString wrapping a raw
// CoordinateSequence pointer that BasicSegmentString does not own.
struct NodedResultDeleter {
  void operator()(std::vector<SegmentString*>* list) const {
    if (list == nullptr) {
      return;
    }
    for (SegmentString* ss : *list) {
      if (auto* bss = dynamic_cast<BasicSegmentString*>(ss)) {
        CoordinateSequence* coords = bss->getCoordinates();
        delete bss;
        delete coords;
      } else {
        delete ss;
      }
    }
    delete list;
  }
};

using ScopedSegmentStrings =
    std::unique_ptr<std::vector<SegmentString*>, NodedResultDeleter>;

class BoundaryNoderInput {
 public:
  void AddBasicXY(std::initializer_list<CoordinateXY> coords,
                  const void* context = nullptr) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    AddBasicSeq(std::move(seq), context);
  }

  void AddBasicSeq(std::unique_ptr<CoordinateSequence> seq,
                   const void* context = nullptr) {
    CoordinateSequence* seq_ptr = seq.get();
    owned_seqs_.push_back(std::move(seq));
    auto ss = std::make_unique<BasicSegmentString>(seq_ptr, context);
    raw_strings_.push_back(ss.get());
    owned_strings_.push_back(std::move(ss));
  }

  void AddNodedSeq(std::unique_ptr<CoordinateSequence> seq, bool construct_z,
                   bool construct_m, const void* context = nullptr) {
    auto ss = std::make_unique<NodedSegmentString>(seq.release(), construct_z,
                                                   construct_m, context);
    raw_strings_.push_back(ss.get());
    owned_strings_.push_back(std::move(ss));
  }

  std::vector<SegmentString*>* ptrs() { return &raw_strings_; }

 private:
  // Destroy SegmentStrings before externally owned CoordinateSequences.
  std::vector<std::unique_ptr<CoordinateSequence>> owned_seqs_;
  std::vector<std::unique_ptr<SegmentString>> owned_strings_;
  std::vector<SegmentString*> raw_strings_;
};

ScopedSegmentStrings RunNoder(BoundaryNoderInput& input,
                              BoundaryChainNoder& noder) {
  noder.computeNodes(input.ptrs());
  return ScopedSegmentStrings(noder.getNodedSubstrings());
}

std::string ChainToString(const SegmentString* ss) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < ss->size(); ++i) {
    if (i > 0) {
      oss << "->";
    }
    const auto& pt = ss->getCoordinate<CoordinateXY>(i);
    oss << "(" << pt.x << "," << pt.y << ")";
  }
  return oss.str();
}

std::vector<std::string> ChainsToStrings(const std::vector<SegmentString*>& s) {
  std::vector<std::string> out;
  out.reserve(s.size());
  for (const SegmentString* ss : s) {
    out.push_back(ChainToString(ss));
  }
  return out;
}

TEST(BoundaryChainNoderTest, NullBeforeComputeNodes) {
  BoundaryChainNoder noder;
  EXPECT_EQ(noder.getNodedSubstrings(), nullptr);
}

TEST(BoundaryChainNoderTest, EmptyInputProducesEmptyResult) {
  BoundaryNoderInput input;
  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->empty());
}

TEST(BoundaryChainNoderTest, SinglePointSegmentStringProducesEmptyResult) {
  BoundaryNoderInput input;
  input.AddBasicXY({CoordinateXY(1.0, 2.0)});
  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->empty());
}

TEST(BoundaryChainNoderTest, SingleRingPreservesBoundaryAndContext) {
  int context_tag = 7;
  BoundaryNoderInput input;
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
                    CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0),
                    CoordinateXY(0.0, 0.0)},
                   &context_tag);

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  ASSERT_EQ(result->size(), 1u);

  const SegmentString* chain = (*result)[0];
  EXPECT_EQ(chain->getData(), &context_tag);
  EXPECT_NE(dynamic_cast<const NodedSegmentString*>(chain), nullptr);
  EXPECT_FALSE(chain->getCoordinates()->hasZ());
  EXPECT_FALSE(chain->getCoordinates()->hasM());
  EXPECT_EQ(ChainToString(chain), "(0,0)->(10,0)->(10,10)->(0,10)->(0,0)");
}

TEST(BoundaryChainNoderTest, AdjacentSquaresCancelOppositeSharedEdge) {
  int tag_a = 1;
  int tag_b = 2;
  BoundaryNoderInput input;
  // Shared edge (10,0)-(10,10) is traversed upward in A and downward in B.
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
                    CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0),
                    CoordinateXY(0.0, 0.0)},
                   &tag_a);
  input.AddBasicXY({CoordinateXY(10.0, 0.0), CoordinateXY(20.0, 0.0),
                    CoordinateXY(20.0, 10.0), CoordinateXY(10.0, 10.0),
                    CoordinateXY(10.0, 0.0)},
                   &tag_b);

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  ASSERT_EQ(result->size(), 3u);
  EXPECT_THAT(ChainsToStrings(*result),
              UnorderedElementsAre("(0,0)->(10,0)", "(10,10)->(0,10)->(0,0)",
                                   "(10,0)->(20,0)->(20,10)->(10,10)"));

  for (const SegmentString* ss : *result) {
    if (ChainToString(ss) == "(10,0)->(20,0)->(20,10)->(10,10)") {
      EXPECT_EQ(ss->getData(), &tag_b);
    } else {
      EXPECT_EQ(ss->getData(), &tag_a);
    }
  }
}

TEST(BoundaryChainNoderTest, SameDirectionSharedSegmentsCancel) {
  BoundaryNoderInput input;
  // Both polygons start with segment (0,0)->(10,0) in the same direction.
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
                    CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0),
                    CoordinateXY(0.0, 0.0)});
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
                    CoordinateXY(5.0, -5.0), CoordinateXY(0.0, 0.0)});

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  EXPECT_THAT(ChainsToStrings(*result),
              UnorderedElementsAre("(10,0)->(10,10)->(0,10)->(0,0)",
                                   "(10,0)->(5,-5)->(0,0)"));
}

TEST(BoundaryChainNoderTest, IdenticalRingsCancelCompletely) {
  BoundaryNoderInput input;
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
                    CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0),
                    CoordinateXY(0.0, 0.0)});
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(0.0, 10.0),
                    CoordinateXY(10.0, 10.0), CoordinateXY(10.0, 0.0),
                    CoordinateXY(0.0, 0.0)});

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->empty());
}

TEST(BoundaryChainNoderTest, RingSplitIntoMultipleDisconnectedBoundaryChains) {
  BoundaryNoderInput input;
  // Middle square shares left edge with left square and right edge with right.
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
                    CoordinateXY(10.0, 10.0), CoordinateXY(0.0, 10.0),
                    CoordinateXY(0.0, 0.0)});
  input.AddBasicXY({CoordinateXY(10.0, 0.0), CoordinateXY(20.0, 0.0),
                    CoordinateXY(20.0, 10.0), CoordinateXY(10.0, 10.0),
                    CoordinateXY(10.0, 0.0)});
  input.AddBasicXY({CoordinateXY(20.0, 0.0), CoordinateXY(30.0, 0.0),
                    CoordinateXY(30.0, 10.0), CoordinateXY(20.0, 10.0),
                    CoordinateXY(20.0, 0.0)});

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  EXPECT_THAT(ChainsToStrings(*result),
              UnorderedElementsAre("(0,0)->(10,0)", "(10,10)->(0,10)->(0,0)",
                                   "(10,0)->(20,0)", "(20,10)->(10,10)",
                                   "(20,0)->(30,0)->(30,10)->(20,10)"));
}

TEST(BoundaryChainNoderTest, HoleFilledByInnerRingCancelsHole) {
  BoundaryNoderInput input;
  // Outer shell
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(20.0, 0.0),
                    CoordinateXY(20.0, 20.0), CoordinateXY(0.0, 20.0),
                    CoordinateXY(0.0, 0.0)});
  // Hole ring (CW)
  input.AddBasicXY({CoordinateXY(5.0, 5.0), CoordinateXY(5.0, 15.0),
                    CoordinateXY(15.0, 15.0), CoordinateXY(15.0, 5.0),
                    CoordinateXY(5.0, 5.0)});
  // Inner polygon filling the hole (CCW)
  input.AddBasicXY({CoordinateXY(5.0, 5.0), CoordinateXY(15.0, 5.0),
                    CoordinateXY(15.0, 15.0), CoordinateXY(5.0, 15.0),
                    CoordinateXY(5.0, 5.0)});

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  EXPECT_THAT(ChainsToStrings(*result),
              UnorderedElementsAre("(0,0)->(20,0)->(20,20)->(0,20)->(0,0)"));
}

TEST(BoundaryChainNoderTest,
     NodesChainAtInteriorVerticesMatchingOtherEndpoints) {
  int tag_main = 10;
  int tag_left = 20;
  int tag_right = 30;
  BoundaryNoderInput input;
  // Main polygon has interior vertices (3,0) and (7,0) on its bottom edge.
  input.AddBasicXY(
      {CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 0.0), CoordinateXY(7.0, 0.0),
       CoordinateXY(10.0, 0.0), CoordinateXY(10.0, 10.0),
       CoordinateXY(0.0, 10.0), CoordinateXY(0.0, 0.0)},
      &tag_main);
  // Triangles touching only at endpoints (3,0) and (7,0).
  input.AddBasicXY({CoordinateXY(3.0, 0.0), CoordinateXY(1.0, -4.0),
                    CoordinateXY(4.0, -4.0), CoordinateXY(3.0, 0.0)},
                   &tag_left);
  input.AddBasicXY({CoordinateXY(7.0, 0.0), CoordinateXY(6.0, -4.0),
                    CoordinateXY(9.0, -4.0), CoordinateXY(7.0, 0.0)},
                   &tag_right);

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  EXPECT_THAT(ChainsToStrings(*result),
              UnorderedElementsAre("(0,0)->(3,0)", "(3,0)->(7,0)",
                                   "(7,0)->(10,0)->(10,10)->(0,10)->(0,0)",
                                   "(3,0)->(1,-4)->(4,-4)->(3,0)",
                                   "(7,0)->(6,-4)->(9,-4)->(7,0)"));

  int main_count = 0;
  for (const SegmentString* ss : *result) {
    if (ss->getData() == &tag_main) {
      ++main_count;
      EXPECT_NE(dynamic_cast<const BasicSegmentString*>(ss), nullptr);
    } else {
      EXPECT_TRUE(ss->getData() == &tag_left || ss->getData() == &tag_right);
      EXPECT_NE(dynamic_cast<const NodedSegmentString*>(ss), nullptr);
    }
  }
  EXPECT_EQ(main_count, 3);
}

TEST(BoundaryChainNoderTest, NodesChainsAtSharedInteriorVertex) {
  int tag_left = 100;
  int tag_right = 200;
  BoundaryNoderInput input;
  // Two rings whose start/end vertices are distinct ((0,0) and (10,0)),
  // touching only at interior vertex (5,5).
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(4.0, 0.0),
                    CoordinateXY(5.0, 5.0), CoordinateXY(4.0, 10.0),
                    CoordinateXY(0.0, 10.0), CoordinateXY(0.0, 0.0)},
                   &tag_left);
  input.AddBasicXY({CoordinateXY(10.0, 0.0), CoordinateXY(10.0, 10.0),
                    CoordinateXY(6.0, 10.0), CoordinateXY(5.0, 5.0),
                    CoordinateXY(6.0, 0.0), CoordinateXY(10.0, 0.0)},
                   &tag_right);

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  EXPECT_THAT(ChainsToStrings(*result),
              UnorderedElementsAre(
                  "(0,0)->(4,0)->(5,5)", "(5,5)->(4,10)->(0,10)->(0,0)",
                  "(10,0)->(10,10)->(6,10)->(5,5)", "(5,5)->(6,0)->(10,0)"));
}

TEST(BoundaryChainNoderTest, PreservesXYZCoordinatesAndNodedInput) {
  BoundaryNoderInput input;
  auto xyz = std::make_unique<CoordinateSequence>(CoordinateSequence::XYZ(0));
  xyz->add(Coordinate(0.0, 0.0, 1.5));
  xyz->add(Coordinate(10.0, 0.0, 2.5));
  xyz->add(Coordinate(0.0, 10.0, 3.5));
  xyz->add(Coordinate(0.0, 0.0, 1.5));
  input.AddNodedSeq(std::move(xyz), /*construct_z=*/true,
                    /*construct_m=*/false);

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  ASSERT_EQ(result->size(), 1u);

  const CoordinateSequence* out = (*result)[0]->getCoordinates();
  EXPECT_TRUE(out->hasZ());
  EXPECT_FALSE(out->hasM());
  ASSERT_EQ(out->size(), 4u);
  EXPECT_DOUBLE_EQ(
      out->getOrdinate(0, static_cast<std::size_t>(CoordinateSequence::Z)),
      1.5);
  EXPECT_DOUBLE_EQ(
      out->getOrdinate(1, static_cast<std::size_t>(CoordinateSequence::Z)),
      2.5);
  EXPECT_DOUBLE_EQ(
      out->getOrdinate(2, static_cast<std::size_t>(CoordinateSequence::Z)),
      3.5);
  EXPECT_DOUBLE_EQ(
      out->getOrdinate(3, static_cast<std::size_t>(CoordinateSequence::Z)),
      1.5);
}

TEST(BoundaryChainNoderTest, PreservesXYMAndXYZMCoordinates) {
  {
    BoundaryNoderInput input;
    auto xym = std::make_unique<CoordinateSequence>(CoordinateSequence::XYM(0));
    xym->add(CoordinateXYM(0.0, 0.0, 10.0));
    xym->add(CoordinateXYM(4.0, 0.0, 20.0));
    xym->add(CoordinateXYM(0.0, 4.0, 30.0));
    xym->add(CoordinateXYM(0.0, 0.0, 10.0));
    input.AddBasicSeq(std::move(xym));

    BoundaryChainNoder noder;
    ScopedSegmentStrings result = RunNoder(input, noder);
    ASSERT_EQ(result->size(), 1u);

    const CoordinateSequence* out = (*result)[0]->getCoordinates();
    EXPECT_FALSE(out->hasZ());
    EXPECT_TRUE(out->hasM());
    ASSERT_EQ(out->size(), 4u);

    CoordinateXYM c0;
    CoordinateXYM c1;
    out->getAt(0, c0);
    out->getAt(1, c1);
    EXPECT_DOUBLE_EQ(c0.m, 10.0);
    EXPECT_DOUBLE_EQ(c1.m, 20.0);
    EXPECT_DOUBLE_EQ(
        out->getOrdinate(2, static_cast<std::size_t>(CoordinateSequence::M)),
        30.0);
  }

  {
    BoundaryNoderInput input;
    auto xyzm =
        std::make_unique<CoordinateSequence>(CoordinateSequence::XYZM(0));
    xyzm->add(CoordinateXYZM(0.0, 0.0, 1.0, 100.0));
    xyzm->add(CoordinateXYZM(5.0, 0.0, 2.0, 200.0));
    xyzm->add(CoordinateXYZM(0.0, 5.0, 3.0, 300.0));
    xyzm->add(CoordinateXYZM(0.0, 0.0, 1.0, 100.0));
    input.AddBasicSeq(std::move(xyzm));

    BoundaryChainNoder noder;
    ScopedSegmentStrings result = RunNoder(input, noder);
    ASSERT_EQ(result->size(), 1u);

    const CoordinateSequence* out = (*result)[0]->getCoordinates();
    EXPECT_TRUE(out->hasZ());
    EXPECT_TRUE(out->hasM());
    EXPECT_DOUBLE_EQ(
        out->getOrdinate(1, static_cast<std::size_t>(CoordinateSequence::Z)),
        2.0);
    EXPECT_DOUBLE_EQ(
        out->getOrdinate(1, static_cast<std::size_t>(CoordinateSequence::M)),
        200.0);
  }
}

TEST(BoundaryChainNoderTest, MixedXYAndXYZPromotesOutputDimension) {
  BoundaryNoderInput input;
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 0.0),
                    CoordinateXY(0.0, 5.0), CoordinateXY(0.0, 0.0)});
  auto xyz = std::make_unique<CoordinateSequence>(CoordinateSequence::XYZ(0));
  xyz->add(Coordinate(10.0, 0.0, 9.0));
  xyz->add(Coordinate(15.0, 0.0, 8.0));
  xyz->add(Coordinate(10.0, 5.0, 7.0));
  xyz->add(Coordinate(10.0, 0.0, 9.0));
  input.AddBasicSeq(std::move(xyz));

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  ASSERT_EQ(result->size(), 2u);
  for (const SegmentString* ss : *result) {
    const CoordinateSequence* out = ss->getCoordinates();
    EXPECT_TRUE(out->hasZ());
    if (out->getX(0) == 0.0) {
      EXPECT_TRUE(std::isnan(out->getOrdinate(
          0, static_cast<std::size_t>(CoordinateSequence::Z))));
    } else {
      EXPECT_DOUBLE_EQ(
          out->getOrdinate(0, static_cast<std::size_t>(CoordinateSequence::Z)),
          9.0);
    }
  }
}

TEST(BoundaryChainNoderTest, NaNEndpointDoesNotFailNodeSearch) {
  BoundaryNoderInput input;
  auto seq = std::make_unique<CoordinateSequence>(CoordinateSequence::XY(0));
  seq->add(CoordinateXY(0.0, 0.0));
  seq->add(CoordinateXY(DoubleNotANumber, DoubleNotANumber));
  input.AddBasicSeq(std::move(seq));

  BoundaryChainNoder noder;
  ScopedSegmentStrings result = RunNoder(input, noder);
  ASSERT_EQ(result->size(), 1u);
  ASSERT_EQ((*result)[0]->size(), 2u);
  EXPECT_TRUE((*result)[0]->getCoordinate<CoordinateXY>(0).equals2D(
      CoordinateXY(0.0, 0.0)));
  EXPECT_TRUE(std::isnan((*result)[0]->getCoordinate<CoordinateXY>(1).x));
  EXPECT_TRUE(std::isnan((*result)[0]->getCoordinate<CoordinateXY>(1).y));
}

}  // namespace
}  // namespace geos_google
