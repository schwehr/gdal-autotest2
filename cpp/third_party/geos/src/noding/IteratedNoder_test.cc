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

#include "third_party/geos/include/geos/noding/IteratedNoder.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;
using ::geos::geom::PrecisionModel;
using ::geos::noding::IteratedNoder;
using ::geos::noding::NodedSegmentString;
using ::geos::noding::SegmentString;
using ::geos::util::TopologyException;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::SizeIs;

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

bool ContainsVertex2D(const SegmentString::NonConstVect& strings,
                      const CoordinateXY& target) {
  for (const SegmentString* ss : strings) {
    for (std::size_t i = 0; i < ss->size(); ++i) {
      if (ss->getCoordinate(i).equals2D(target)) {
        return true;
      }
    }
  }
  return false;
}

TEST(IteratedNoderTest, EmptyInputConvergesImmediately) {
  PrecisionModel pm;
  IteratedNoder noder(&pm);

  SegmentString::NonConstVect empty;
  noder.computeNodes(&empty);

  SegmentString::NonConstVect* noded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };

  ASSERT_NE(noded, nullptr);
  EXPECT_THAT(*noded, IsEmpty());
}

TEST(IteratedNoderTest, DisjointAndEndpointOnlyStringsConvergeInOnePass) {
  IteratedNoder noder(nullptr);

  int ctx0 = 10;
  int ctx1 = 20;
  int ctx2 = 30;
  auto s0 = MakeNodedXY({{0.0, 0.0}, {5.0, 0.0}}, &ctx0);
  auto s1 = MakeNodedXY({{5.0, 0.0}, {5.0, 5.0}}, &ctx1);
  auto s2 = MakeNodedXY({{10.0, 10.0}, {15.0, 10.0}}, &ctx2);

  SegmentString::NonConstVect inputs{s0.get(), s1.get(), s2.get()};
  noder.computeNodes(&inputs);

  SegmentString::NonConstVect* noded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };

  ASSERT_THAT(*noded, SizeIs(3));
  EXPECT_EQ((*noded)[0]->getData(), &ctx0);
  EXPECT_EQ((*noded)[1]->getData(), &ctx1);
  EXPECT_EQ((*noded)[2]->getData(), &ctx2);
}

TEST(IteratedNoderTest, ProperCrossAndGridSplitAllSegments) {
  PrecisionModel pm;
  IteratedNoder noder(&pm);

  int ctxA = 1;
  int ctxB = 2;
  auto a = MakeNodedXY({{0.0, 0.0}, {10.0, 10.0}}, &ctxA);
  auto b = MakeNodedXY({{0.0, 10.0}, {10.0, 0.0}}, &ctxB);

  SegmentString::NonConstVect crossInputs{a.get(), b.get()};
  noder.computeNodes(&crossInputs);

  SegmentString::NonConstVect* crossNoded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_cross = [&] { DeleteSegmentStringVector(crossNoded); };

  ASSERT_THAT(*crossNoded, SizeIs(4));
  EXPECT_EQ((*crossNoded)[0]->getData(), &ctxA);
  EXPECT_EQ((*crossNoded)[1]->getData(), &ctxA);
  EXPECT_EQ((*crossNoded)[2]->getData(), &ctxB);
  EXPECT_EQ((*crossNoded)[3]->getData(), &ctxB);
  EXPECT_TRUE(ContainsVertex2D(*crossNoded, CoordinateXY(5.0, 5.0)));

  // 2x2 grid: 4 lines each intersected twice -> 12 substrings.
  auto h1 = MakeNodedXY({{0.0, 2.0}, {8.0, 2.0}});
  auto h2 = MakeNodedXY({{0.0, 6.0}, {8.0, 6.0}});
  auto v1 = MakeNodedXY({{2.0, 0.0}, {2.0, 8.0}});
  auto v2 = MakeNodedXY({{6.0, 0.0}, {6.0, 8.0}});

  SegmentString::NonConstVect gridInputs{h1.get(), h2.get(), v1.get(),
                                         v2.get()};
  noder.computeNodes(&gridInputs);

  SegmentString::NonConstVect* gridNoded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_grid = [&] { DeleteSegmentStringVector(gridNoded); };
  EXPECT_THAT(*gridNoded, SizeIs(12));
}

TEST(IteratedNoderTest, NonProperInteriorIntersectionsConverge) {
  PrecisionModel pm;
  IteratedNoder noder(&pm);

  // T-intersection: interior to horiz, endpoint of stem (not proper).
  auto horiz = MakeNodedXY({{0.0, 0.0}, {10.0, 0.0}});
  auto stem = MakeNodedXY({{4.0, 0.0}, {4.0, 5.0}});
  SegmentString::NonConstVect tInputs{horiz.get(), stem.get()};
  noder.computeNodes(&tInputs);

  SegmentString::NonConstVect* tNoded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_t = [&] { DeleteSegmentStringVector(tNoded); };
  ASSERT_THAT(*tNoded, SizeIs(3));
  EXPECT_TRUE(ContainsVertex2D(*tNoded, CoordinateXY(4.0, 0.0)));

  // Collinear partial overlap: not proper, splits both strings at interior
  // overlap endpoints and converges on the second iteration.
  auto c0 = MakeNodedXY({{0.0, 0.0}, {10.0, 0.0}});
  auto c1 = MakeNodedXY({{4.0, 0.0}, {14.0, 0.0}});
  SegmentString::NonConstVect colInputs{c0.get(), c1.get()};
  noder.computeNodes(&colInputs);

  SegmentString::NonConstVect* colNoded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_col = [&] { DeleteSegmentStringVector(colNoded); };
  ASSERT_THAT(*colNoded, SizeIs(4));
  EXPECT_TRUE(ContainsVertex2D(*colNoded, CoordinateXY(4.0, 0.0)));
  EXPECT_TRUE(ContainsVertex2D(*colNoded, CoordinateXY(10.0, 0.0)));
}

TEST(IteratedNoderTest, SelfIntersectingPolylineNodesCorrectly) {
  PrecisionModel pm;
  IteratedNoder noder(&pm);

  auto bowtie = MakeNodedXY({{0.0, 0.0}, {4.0, 4.0}, {4.0, 0.0}, {0.0, 4.0}});
  SegmentString::NonConstVect inputs{bowtie.get()};
  noder.computeNodes(&inputs);

  SegmentString::NonConstVect* noded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };

  ASSERT_THAT(*noded, SizeIs(3));
  EXPECT_TRUE(ContainsVertex2D(*noded, CoordinateXY(2.0, 2.0)));
}

TEST(IteratedNoderTest, FixedPrecisionMultiIterationCascadeConverges) {
  // Under PrecisionModel(1.0), rounding each intersection pushes the left
  // split of S0 upward so it intersects the next obstacle in the following
  // iteration:
  //   Iter 1: S0(y=20x) x B1 -> (20.4, 408) rounds to (20, 408)
  //   Iter 2: S1(y=20.4x) x B2 -> (16.296, 332.444) rounds to (16, 332)
  //   Iter 3: S2(y=20.75x) x B3 -> (12.267, 254.533) rounds to (12, 255)
  //   Iter 4: 0 interior intersections -> converges.
  PrecisionModel pm(1.0);
  IteratedNoder noder(&pm);

  auto s0 = MakeNodedXY({{0.0, 0.0}, {50.0, 1000.0}});
  auto b1 = MakeNodedXY({{19.0, 408.0}, {22.0, 408.0}});
  auto b2 = MakeNodedXY({{16.0, 328.0}, {17.0, 343.0}});
  auto b3 = MakeNodedXY({{12.0, 250.0}, {13.0, 267.0}});

  SegmentString::NonConstVect inputs{s0.get(), b1.get(), b2.get(), b3.get()};
  noder.computeNodes(&inputs);

  SegmentString::NonConstVect* noded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };

  ASSERT_THAT(*noded, SizeIs(10));
  EXPECT_TRUE(ContainsVertex2D(*noded, CoordinateXY(20.0, 408.0)));
  EXPECT_TRUE(ContainsVertex2D(*noded, CoordinateXY(16.0, 332.0)));
  EXPECT_TRUE(ContainsVertex2D(*noded, CoordinateXY(12.0, 255.0)));
}

TEST(IteratedNoderTest,
     ThrowsTopologyExceptionWhenDefaultMaxIterationsExceeded) {
  // Six-stage cascade keeps creating 1 intersection per iteration across
  // iterations 1..6, exceeding default MAX_ITER (5) on iteration 6.
  PrecisionModel pm(1.0);
  IteratedNoder noder(&pm);

  auto s0 = MakeNodedXY({{0.0, 0.0}, {50.0, 1000.0}});
  auto b1 = MakeNodedXY({{19.0, 408.0}, {22.0, 408.0}});
  auto b2 = MakeNodedXY({{16.0, 328.0}, {17.0, 343.0}});
  auto b3 = MakeNodedXY({{12.0, 250.0}, {13.0, 267.0}});
  auto b4 = MakeNodedXY({{9.0, 193.0}, {10.0, 210.0}});
  auto b5 = MakeNodedXY({{6.0, 135.0}, {7.0, 152.0}});
  auto b6 = MakeNodedXY({{3.0, 71.0}, {4.0, 91.0}});

  SegmentString::NonConstVect inputs{s0.get(), b1.get(), b2.get(), b3.get(),
                                     b4.get(), b5.get(), b6.get()};
  try {
    noder.computeNodes(&inputs);
    FAIL() << "Expected TopologyException";
  } catch (const TopologyException& e) {
    EXPECT_THAT(
        e.what(),
        HasSubstr("Iterated noding failed to converge after 6 iterations"));
    EXPECT_THAT(e.what(), HasSubstr("(near"));
  }
}

TEST(IteratedNoderTest, SetMaximumIterationsControlsAbortAndConvergence) {
  PrecisionModel pm(1.0);

  // 2-stage cascade creates 1 intersection in iter 1 and 1 in iter 2:
  // with maxIter = 1, iter 2 exceeds maxIter and aborts.
  {
    auto s0 = MakeNodedXY({{0.0, 0.0}, {50.0, 1000.0}});
    auto b1 = MakeNodedXY({{19.0, 408.0}, {22.0, 408.0}});
    auto b2 = MakeNodedXY({{16.0, 328.0}, {17.0, 343.0}});
    SegmentString::NonConstVect inputs{s0.get(), b1.get(), b2.get()};

    IteratedNoder strictNoder(&pm);
    strictNoder.setMaximumIterations(1);
    try {
      strictNoder.computeNodes(&inputs);
      FAIL() << "Expected TopologyException";
    } catch (const TopologyException& e) {
      EXPECT_THAT(
          e.what(),
          HasSubstr("Iterated noding failed to converge after 2 iterations"));
    }
  }

  // With maxIter = 2, iteration 2 is allowed (2 > 2 is false) and iter 3
  // finds 0 intersections and converges.
  {
    auto s0 = MakeNodedXY({{0.0, 0.0}, {50.0, 1000.0}});
    auto b1 = MakeNodedXY({{19.0, 408.0}, {22.0, 408.0}});
    auto b2 = MakeNodedXY({{16.0, 328.0}, {17.0, 343.0}});
    SegmentString::NonConstVect inputs{s0.get(), b1.get(), b2.get()};

    IteratedNoder exactNoder(&pm);
    exactNoder.setMaximumIterations(2);
    exactNoder.computeNodes(&inputs);
    SegmentString::NonConstVect* noded = exactNoder.getNodedSubstrings();
    absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };
    ASSERT_THAT(*noded, SizeIs(7));
    EXPECT_TRUE(ContainsVertex2D(*noded, CoordinateXY(16.0, 332.0)));
  }

  // Even when iteration count > maxIter (iter 2 > 1), if the intersection
  // count strictly decreases (iter 1: 2 intersections, iter 2: 1 intersection),
  // noding continues and converges.
  {
    auto s0 = MakeNodedXY({{0.0, 0.0}, {50.0, 1000.0}});
    auto b1 = MakeNodedXY({{19.0, 408.0}, {22.0, 408.0}});
    auto b2 = MakeNodedXY({{16.0, 328.0}, {17.0, 343.0}});
    auto extraA = MakeNodedXY({{100.0, 0.0}, {102.0, 2.0}});
    auto extraB = MakeNodedXY({{100.0, 2.0}, {102.0, 0.0}});
    SegmentString::NonConstVect decreasingInputs{s0.get(), b1.get(), b2.get(),
                                                 extraA.get(), extraB.get()};

    IteratedNoder decreasingNoder(&pm);
    decreasingNoder.setMaximumIterations(1);
    decreasingNoder.computeNodes(&decreasingInputs);
    SegmentString::NonConstVect* noded = decreasingNoder.getNodedSubstrings();
    absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };
    ASSERT_THAT(*noded, SizeIs(11));
    EXPECT_TRUE(ContainsVertex2D(*noded, CoordinateXY(16.0, 332.0)));
    EXPECT_TRUE(ContainsVertex2D(*noded, CoordinateXY(101.0, 1.0)));
  }
}

TEST(IteratedNoderTest, PreservesAndInterpolatesZCoordinates) {
  PrecisionModel pm;
  IteratedNoder noder(&pm);

  auto z0 = MakeNodedXYZ({{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}});
  auto z1 = MakeNodedXYZ({{0.0, 10.0, 20.0}, {10.0, 0.0, 20.0}});
  SegmentString::NonConstVect inputs{z0.get(), z1.get()};

  noder.computeNodes(&inputs);
  SegmentString::NonConstVect* noded = noder.getNodedSubstrings();
  absl::Cleanup cleanup_noded = [&] { DeleteSegmentStringVector(noded); };

  ASSERT_THAT(*noded, SizeIs(4));
  for (const SegmentString* ss : *noded) {
    for (std::size_t i = 0; i < ss->size(); ++i) {
      const Coordinate& c = ss->getCoordinate(i);
      EXPECT_FALSE(std::isnan(c.z));
      if (c.equals2D(CoordinateXY(5.0, 5.0))) {
        EXPECT_DOUBLE_EQ(c.z, 12.5);
      }
    }
  }
}

}  // namespace
}  // namespace geos_google
