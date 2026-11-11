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

// Unit tests for third_party/geos/src/noding/SegmentExtractingNoder.cpp and
// third_party/geos/include/geos/noding/SegmentExtractingNoder.h.

#include "third_party/geos/include/geos/noding/SegmentExtractingNoder.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::noding::BasicSegmentString;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentExtractingNoder;
using geos::noding::SegmentString;

void DeleteOwnedSubstrings(std::vector<SegmentString*>* segs) {
  if (segs == nullptr) {
    return;
  }
  for (SegmentString* ss : *segs) {
    delete ss;
  }
  delete segs;
}

TEST(SegmentExtractingNoderTest, GetNodedSubstringsIsNullBeforeComputeNodes) {
  SegmentExtractingNoder noder;
  EXPECT_EQ(noder.getNodedSubstrings(), nullptr);
}

TEST(SegmentExtractingNoderTest, EmptyInputProducesEmptySubstrings) {
  SegmentExtractingNoder noder;
  std::vector<SegmentString*> input;

  noder.computeNodes(&input);
  std::vector<SegmentString*>* out = noder.getNodedSubstrings();
  absl::Cleanup cleanup = [&] { DeleteOwnedSubstrings(out); };

  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(out->empty());
}

TEST(SegmentExtractingNoderTest, ExtractsMultiSegmentPolylineAndPreservesData) {
  SegmentExtractingNoder noder;
  int ctx_polyline = 101;
  int ctx_single_point = 202;

  auto poly_seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {1.0, 2.0}, {4.0, 2.0}, {4.0, -1.0}});
  auto single_pt_seq = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{9.0, 9.0}});

  NodedSegmentString poly_ss(poly_seq.release(), false, false, &ctx_polyline);
  NodedSegmentString single_pt_ss(single_pt_seq.release(), false, false,
                                  &ctx_single_point);

  std::vector<SegmentString*> input = {&poly_ss, &single_pt_ss};
  noder.computeNodes(&input);

  std::vector<SegmentString*>* out = noder.getNodedSubstrings();
  absl::Cleanup cleanup = [&] { DeleteOwnedSubstrings(out); };

  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 3u);

  const double expected_coords[3][4] = {
      {0.0, 0.0, 1.0, 2.0},
      {1.0, 2.0, 4.0, 2.0},
      {4.0, 2.0, 4.0, -1.0},
  };
  for (std::size_t i = 0; i < 3; ++i) {
    const SegmentString* seg = (*out)[i];
    EXPECT_NE(dynamic_cast<const NodedSegmentString*>(seg), nullptr);
    EXPECT_EQ(seg->getData(), &ctx_polyline);
    const CoordinateSequence* cs = seg->getCoordinates();
    ASSERT_EQ(cs->size(), 2u);
    EXPECT_FALSE(cs->hasZ());
    EXPECT_FALSE(cs->hasM());
    EXPECT_DOUBLE_EQ(cs->getX(0), expected_coords[i][0]);
    EXPECT_DOUBLE_EQ(cs->getY(0), expected_coords[i][1]);
    EXPECT_DOUBLE_EQ(cs->getX(1), expected_coords[i][2]);
    EXPECT_DOUBLE_EQ(cs->getY(1), expected_coords[i][3]);
  }
}

TEST(SegmentExtractingNoderTest, DoesNotSplitCrossingSegments) {
  SegmentExtractingNoder noder;
  int ctx_a = 1;
  int ctx_b = 2;

  auto seq_a = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0.0, 0.0}, {4.0, 4.0}});
  auto seq_b = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXY>{{0.0, 4.0}, {4.0, 0.0}});

  NodedSegmentString ss_a(seq_a.release(), false, false, &ctx_a);
  NodedSegmentString ss_b(seq_b.release(), false, false, &ctx_b);

  std::vector<SegmentString*> input = {&ss_a, &ss_b};
  noder.computeNodes(&input);

  std::vector<SegmentString*>* out = noder.getNodedSubstrings();
  absl::Cleanup cleanup = [&] { DeleteOwnedSubstrings(out); };

  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ((*out)[0]->getData(), &ctx_a);
  EXPECT_EQ((*out)[1]->getData(), &ctx_b);
  EXPECT_DOUBLE_EQ((*out)[0]->getCoordinates()->getX(0), 0.0);
  EXPECT_DOUBLE_EQ((*out)[0]->getCoordinates()->getX(1), 4.0);
  EXPECT_DOUBLE_EQ((*out)[1]->getCoordinates()->getY(0), 4.0);
  EXPECT_DOUBLE_EQ((*out)[1]->getCoordinates()->getY(1), 0.0);
}

TEST(SegmentExtractingNoderTest, HonorsNodedSegmentStringZMConstructionFlags) {
  SegmentExtractingNoder noder;

  auto seq_xyz =
      std::make_unique<CoordinateSequence>(std::initializer_list<Coordinate>{
          {0.0, 0.0, 10.0}, {1.0, 1.0, 20.0}, {2.0, 0.0, 30.0}});
  auto seq_xym = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYM>{{0.0, 0.0, 1.5}, {3.0, 4.0, 2.5}});
  auto seq_xyzm = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYZM>{{1.0, 2.0, 3.0, 4.0},
                                            {5.0, 6.0, 7.0, 8.0}});
  auto seq_drop_z = std::make_unique<CoordinateSequence>(
      std::initializer_list<Coordinate>{{0.0, 0.0, 99.0}, {1.0, 0.0, 88.0}});

  NodedSegmentString ss_xyz(seq_xyz.release(), true, false, nullptr);
  NodedSegmentString ss_xym(seq_xym.release(), false, true, nullptr);
  NodedSegmentString ss_xyzm(seq_xyzm.release(), true, true, nullptr);
  // Explicitly request constructZ = false despite XYZ input sequence.
  NodedSegmentString ss_drop_z(seq_drop_z.release(), false, false, nullptr);

  std::vector<SegmentString*> input = {&ss_xyz, &ss_xym, &ss_xyzm, &ss_drop_z};
  noder.computeNodes(&input);

  std::vector<SegmentString*>* out = noder.getNodedSubstrings();
  absl::Cleanup cleanup = [&] { DeleteOwnedSubstrings(out); };

  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 5u);

  // Two XYZ segments from ss_xyz.
  const CoordinateSequence* seg0 = (*out)[0]->getCoordinates();
  EXPECT_TRUE(seg0->hasZ());
  EXPECT_FALSE(seg0->hasM());
  EXPECT_DOUBLE_EQ(seg0->getOrdinate(0, CoordinateSequence::Z), 10.0);
  EXPECT_DOUBLE_EQ(seg0->getOrdinate(1, CoordinateSequence::Z), 20.0);

  const CoordinateSequence* seg1 = (*out)[1]->getCoordinates();
  EXPECT_TRUE(seg1->hasZ());
  EXPECT_FALSE(seg1->hasM());
  EXPECT_DOUBLE_EQ(seg1->getOrdinate(0, CoordinateSequence::Z), 20.0);
  EXPECT_DOUBLE_EQ(seg1->getOrdinate(1, CoordinateSequence::Z), 30.0);

  // One XYM segment from ss_xym.
  const CoordinateSequence* seg2 = (*out)[2]->getCoordinates();
  EXPECT_FALSE(seg2->hasZ());
  EXPECT_TRUE(seg2->hasM());
  EXPECT_TRUE(std::isnan(seg2->getOrdinate(0, CoordinateSequence::Z)));
  CoordinateXYM xym0;
  CoordinateXYM xym1;
  seg2->getAt(0, xym0);
  seg2->getAt(1, xym1);
  EXPECT_DOUBLE_EQ(xym0.m, 1.5);
  EXPECT_DOUBLE_EQ(xym1.m, 2.5);

  // One XYZM segment from ss_xyzm.
  const CoordinateSequence* seg3 = (*out)[3]->getCoordinates();
  EXPECT_TRUE(seg3->hasZ());
  EXPECT_TRUE(seg3->hasM());
  EXPECT_DOUBLE_EQ(seg3->getOrdinate(0, CoordinateSequence::Z), 3.0);
  EXPECT_DOUBLE_EQ(seg3->getOrdinate(0, CoordinateSequence::M), 4.0);
  EXPECT_DOUBLE_EQ(seg3->getOrdinate(1, CoordinateSequence::Z), 7.0);
  EXPECT_DOUBLE_EQ(seg3->getOrdinate(1, CoordinateSequence::M), 8.0);

  // One segment where constructZ=false dropped Z.
  const CoordinateSequence* seg4 = (*out)[4]->getCoordinates();
  EXPECT_FALSE(seg4->hasZ());
  EXPECT_FALSE(seg4->hasM());
  EXPECT_TRUE(std::isnan(seg4->getOrdinate(0, CoordinateSequence::Z)));
}

TEST(SegmentExtractingNoderTest, ExtractsFromBasicSegmentStringUsingSeqDims) {
  SegmentExtractingNoder noder;
  int ctx_xy = 10;
  int ctx_xyz = 20;
  int ctx_xym = 30;
  int ctx_xyzm = 40;

  CoordinateSequence seq_xy(
      {CoordinateXY{0.0, 1.0}, CoordinateXY{2.0, 3.0}, CoordinateXY{4.0, 5.0}});
  CoordinateSequence seq_xyz(
      {Coordinate{0.0, 0.0, 5.0}, Coordinate{1.0, 1.0, 6.0}});
  CoordinateSequence seq_xym(
      {CoordinateXYM{2.0, 3.0, 9.0}, CoordinateXYM{4.0, 5.0, 11.0}});
  CoordinateSequence seq_xyzm(
      {CoordinateXYZM{1.0, 1.0, 2.0, 3.0}, CoordinateXYZM{2.0, 2.0, 4.0, 6.0}});

  BasicSegmentString bss_xy(&seq_xy, &ctx_xy);
  BasicSegmentString bss_xyz(&seq_xyz, &ctx_xyz);
  BasicSegmentString bss_xym(&seq_xym, &ctx_xym);
  BasicSegmentString bss_xyzm(&seq_xyzm, &ctx_xyzm);

  std::vector<SegmentString*> input = {&bss_xy, &bss_xyz, &bss_xym, &bss_xyzm};
  noder.computeNodes(&input);

  std::vector<SegmentString*>* out = noder.getNodedSubstrings();
  absl::Cleanup cleanup = [&] { DeleteOwnedSubstrings(out); };

  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 5u);

  // First two segments come from bss_xy.
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ((*out)[i]->getData(), &ctx_xy);
    EXPECT_FALSE((*out)[i]->getCoordinates()->hasZ());
    EXPECT_FALSE((*out)[i]->getCoordinates()->hasM());
  }

  // Third segment from bss_xyz.
  const auto* nss_xyz = dynamic_cast<const NodedSegmentString*>((*out)[2]);
  ASSERT_NE(nss_xyz, nullptr);
  EXPECT_EQ(nss_xyz->getData(), &ctx_xyz);
  EXPECT_TRUE(nss_xyz->getNodeList().getConstructZ());
  EXPECT_FALSE(nss_xyz->getNodeList().getConstructM());
  EXPECT_DOUBLE_EQ(
      nss_xyz->getCoordinates()->getOrdinate(0, CoordinateSequence::Z), 5.0);
  EXPECT_DOUBLE_EQ(
      nss_xyz->getCoordinates()->getOrdinate(1, CoordinateSequence::Z), 6.0);

  // Fourth segment from bss_xym.
  const auto* nss_xym = dynamic_cast<const NodedSegmentString*>((*out)[3]);
  ASSERT_NE(nss_xym, nullptr);
  EXPECT_EQ(nss_xym->getData(), &ctx_xym);
  EXPECT_FALSE(nss_xym->getNodeList().getConstructZ());
  EXPECT_TRUE(nss_xym->getNodeList().getConstructM());
  EXPECT_DOUBLE_EQ(
      nss_xym->getCoordinates()->getOrdinate(0, CoordinateSequence::M), 9.0);
  EXPECT_DOUBLE_EQ(
      nss_xym->getCoordinates()->getOrdinate(1, CoordinateSequence::M), 11.0);

  // Fifth segment from bss_xyzm.
  const auto* nss_xyzm = dynamic_cast<const NodedSegmentString*>((*out)[4]);
  ASSERT_NE(nss_xyzm, nullptr);
  EXPECT_EQ(nss_xyzm->getData(), &ctx_xyzm);
  EXPECT_TRUE(nss_xyzm->getNodeList().getConstructZ());
  EXPECT_TRUE(nss_xyzm->getNodeList().getConstructM());
  EXPECT_DOUBLE_EQ(
      nss_xyzm->getCoordinates()->getOrdinate(0, CoordinateSequence::Z), 2.0);
  EXPECT_DOUBLE_EQ(
      nss_xyzm->getCoordinates()->getOrdinate(0, CoordinateSequence::M), 3.0);
  EXPECT_DOUBLE_EQ(
      nss_xyzm->getCoordinates()->getOrdinate(1, CoordinateSequence::Z), 4.0);
  EXPECT_DOUBLE_EQ(
      nss_xyzm->getCoordinates()->getOrdinate(1, CoordinateSequence::M), 6.0);
}

}  // namespace
}  // namespace geos_google
