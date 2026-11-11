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

// Unit tests for third_party/geos/src/noding/ScaledNoder.cpp and
// third_party/geos/include/geos/noding/ScaledNoder.h.

#include "third_party/geos/include/geos/noding/ScaledNoder.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/algorithm/LineIntersector.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/IntersectionAdder.h"
#include "third_party/geos/include/geos/noding/MCIndexNoder.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/Noder.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/noding/SimpleNoder.h"
#include "third_party/geos/include/geos/noding/snapround/MCIndexSnapRounder.h"

namespace geos_google {
namespace {

using geos::algorithm::LineIntersector;
using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::geom::PrecisionModel;
using geos::noding::BasicSegmentString;
using geos::noding::IntersectionAdder;
using geos::noding::MCIndexNoder;
using geos::noding::NodedSegmentString;
using geos::noding::Noder;
using geos::noding::ScaledNoder;
using geos::noding::SegmentString;
using geos::noding::SimpleNoder;
using geos::noding::snapround::MCIndexSnapRounder;

NodedSegmentString* MakeSegmentString(std::initializer_list<CoordinateXY> pts,
                                      const void* context = nullptr) {
  auto seq = std::make_unique<CoordinateSequence>(pts);
  return new NodedSegmentString(seq.release(), false, false, context);
}

void DeleteSegmentStrings(std::vector<SegmentString*>& segs) {
  for (SegmentString* ss : segs) {
    delete ss;
  }
  segs.clear();
}

void DeleteSegmentStringVector(std::vector<SegmentString*>* segs) {
  if (segs == nullptr) {
    return;
  }
  DeleteSegmentStrings(*segs);
  delete segs;
}

class RecordingNoder final : public Noder {
 public:
  struct Snapshot {
    std::unique_ptr<CoordinateSequence> coords;
    const void* data;
    bool construct_z;
    bool construct_m;
  };

  void computeNodes(std::vector<SegmentString*>* input_seg_str) override {
    snapshots_.clear();
    if (input_seg_str == nullptr) {
      return;
    }
    for (const SegmentString* ss : *input_seg_str) {
      const auto* nss = dynamic_cast<const NodedSegmentString*>(ss);
      snapshots_.push_back({
          ss->getCoordinates()->clone(),
          ss->getData(),
          nss != nullptr ? nss->getNodeList().getConstructZ()
                         : ss->getCoordinates()->hasZ(),
          nss != nullptr ? nss->getNodeList().getConstructM()
                         : ss->getCoordinates()->hasM(),
      });
    }
  }

  std::vector<SegmentString*>* getNodedSubstrings() const override {
    auto* out = new std::vector<SegmentString*>();
    out->reserve(snapshots_.size());
    for (const auto& snap : snapshots_) {
      out->push_back(new NodedSegmentString(snap.coords->clone().release(),
                                            snap.construct_z, snap.construct_m,
                                            snap.data));
    }
    return out;
  }

  const std::vector<Snapshot>& snapshots() const { return snapshots_; }

 private:
  std::vector<Snapshot> snapshots_;
};

TEST(ScaledNoderTest, IsIntegerPrecisionReflectsScaleFactor) {
  RecordingNoder inner;

  ScaledNoder unit_default(inner, 1.0);
  EXPECT_TRUE(unit_default.isIntegerPrecision());

  ScaledNoder unit_with_offset(inner, 1.0, 50.0, -25.0);
  EXPECT_TRUE(unit_with_offset.isIntegerPrecision());

  ScaledNoder upscaled(inner, 100.0);
  EXPECT_FALSE(upscaled.isIntegerPrecision());

  ScaledNoder downscaled(inner, 0.25, 1.0, 2.0);
  EXPECT_FALSE(downscaled.isIntegerPrecision());

  ScaledNoder negative_scale(inner, -1.0);
  EXPECT_FALSE(negative_scale.isIntegerPrecision());
}

TEST(ScaledNoderTest, UnitScaleSkipsScalingAndRescalingEvenWithOffsets) {
  RecordingNoder inner;
  ScaledNoder scaled(inner, 1.0, 100.0, -50.0);

  int tag = 7;
  // Consecutive duplicate coordinates must also be preserved because scale()
  // is skipped when scaleFactor == 1.0.
  std::vector<SegmentString*> input = {
      MakeSegmentString({{1.25, 2.75}, {1.25, 2.75}, {3.6, -4.4}}, &tag),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  scaled.computeNodes(&input);
  ASSERT_EQ(inner.snapshots().size(), 1u);
  const CoordinateSequence* seen = inner.snapshots()[0].coords.get();
  ASSERT_EQ(seen->size(), 3u);
  EXPECT_DOUBLE_EQ(seen->getX(0), 1.25);
  EXPECT_DOUBLE_EQ(seen->getY(0), 2.75);
  EXPECT_DOUBLE_EQ(seen->getX(1), 1.25);
  EXPECT_DOUBLE_EQ(seen->getY(1), 2.75);
  EXPECT_DOUBLE_EQ(seen->getX(2), 3.6);
  EXPECT_DOUBLE_EQ(seen->getY(2), -4.4);
  EXPECT_EQ(inner.snapshots()[0].data, &tag);

  std::vector<SegmentString*>* out = scaled.getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  ASSERT_EQ((*out)[0]->getCoordinates()->size(), 3u);
  EXPECT_DOUBLE_EQ((*out)[0]->getCoordinates()->getX(0), 1.25);
  EXPECT_DOUBLE_EQ((*out)[0]->getCoordinates()->getY(0), 2.75);
  EXPECT_DOUBLE_EQ((*out)[0]->getCoordinates()->getX(1), 1.25);
  EXPECT_DOUBLE_EQ((*out)[0]->getCoordinates()->getY(1), 2.75);
  EXPECT_DOUBLE_EQ((*out)[0]->getCoordinates()->getX(2), 3.6);
  EXPECT_DOUBLE_EQ((*out)[0]->getCoordinates()->getY(2), -4.4);
  EXPECT_EQ((*out)[0]->getData(), &tag);
}

TEST(ScaledNoderTest, ScalesRoundsWithOffsetsAndRescalesSubstrings) {
  RecordingNoder inner;
  // Scale = 10, offset = (1.0, 2.0):
  // Non-halfway inputs avoid binary floating-point tie sensitivity:
  // (1.06, 2.06) -> (0.6, 0.6) -> (1, 1) -> rescaled (1.1, 2.1)
  // (0.96, 1.96) -> (-0.4, -0.4) -> (0, 0) -> rescaled (1.0, 2.0)
  // (0.84, 1.84) -> (-1.6, -1.6) -> (-2, -2) -> rescaled (0.8, 1.8)
  // (1.24, 2.26) -> (2.4, 2.6) -> (2, 3) -> rescaled (1.2, 2.3)
  ScaledNoder scaled(inner, 10.0, 1.0, 2.0);
  EXPECT_FALSE(scaled.isIntegerPrecision());

  int tag = 42;
  std::vector<SegmentString*> input = {
      MakeSegmentString(
          {{1.06, 2.06}, {0.96, 1.96}, {0.84, 1.84}, {1.24, 2.26}}, &tag),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  scaled.computeNodes(&input);

  ASSERT_EQ(inner.snapshots().size(), 1u);
  const CoordinateSequence* seen = inner.snapshots()[0].coords.get();
  ASSERT_EQ(seen->size(), 4u);
  EXPECT_DOUBLE_EQ(seen->getX(0), 1.0);
  EXPECT_DOUBLE_EQ(seen->getY(0), 1.0);
  EXPECT_DOUBLE_EQ(seen->getX(1), 0.0);
  EXPECT_DOUBLE_EQ(seen->getY(1), 0.0);
  EXPECT_DOUBLE_EQ(seen->getX(2), -2.0);
  EXPECT_DOUBLE_EQ(seen->getY(2), -2.0);
  EXPECT_DOUBLE_EQ(seen->getX(3), 2.0);
  EXPECT_DOUBLE_EQ(seen->getY(3), 3.0);

  std::vector<SegmentString*>* out = scaled.getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  const CoordinateSequence* rescaled = (*out)[0]->getCoordinates();
  ASSERT_EQ(rescaled->size(), 4u);
  EXPECT_NEAR(rescaled->getX(0), 1.1, 1e-12);
  EXPECT_NEAR(rescaled->getY(0), 2.1, 1e-12);
  EXPECT_NEAR(rescaled->getX(1), 1.0, 1e-12);
  EXPECT_NEAR(rescaled->getY(1), 2.0, 1e-12);
  EXPECT_NEAR(rescaled->getX(2), 0.8, 1e-12);
  EXPECT_NEAR(rescaled->getY(2), 1.8, 1e-12);
  EXPECT_NEAR(rescaled->getX(3), 1.2, 1e-12);
  EXPECT_NEAR(rescaled->getY(3), 2.3, 1e-12);
  EXPECT_EQ((*out)[0]->getData(), &tag);
}

TEST(ScaledNoderTest, UsesJavaMathRoundHalfwayTieBreakingWithBinaryExactScale) {
  RecordingNoder inner;
  // Scale = 2.0, offset = (1.0, 2.0) yields exact IEEE-754 half-integers:
  // (1.25, 2.25) -> (0.5, 0.5) -> java_math_round -> (1, 1) -> (1.5, 2.5)
  // (0.75, 1.75) -> (-0.5, -0.5) -> java_math_round -> (0, 0) -> (1.0, 2.0)
  // (1.75, 2.75) -> (1.5, 1.5) -> java_math_round -> (2, 2) -> (2.0, 3.0)
  // (0.25, 1.25) -> (-1.5, -1.5) -> java_math_round -> (-1, -1) -> (0.5, 1.5)
  ScaledNoder scaled(inner, 2.0, 1.0, 2.0);

  std::vector<SegmentString*> input = {
      MakeSegmentString(
          {{1.25, 2.25}, {0.75, 1.75}, {1.75, 2.75}, {0.25, 1.25}}),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  scaled.computeNodes(&input);

  ASSERT_EQ(inner.snapshots().size(), 1u);
  const CoordinateSequence* seen = inner.snapshots()[0].coords.get();
  ASSERT_EQ(seen->size(), 4u);
  EXPECT_DOUBLE_EQ(seen->getX(0), 1.0);
  EXPECT_DOUBLE_EQ(seen->getY(0), 1.0);
  EXPECT_DOUBLE_EQ(seen->getX(1), 0.0);
  EXPECT_DOUBLE_EQ(seen->getY(1), 0.0);
  EXPECT_DOUBLE_EQ(seen->getX(2), 2.0);
  EXPECT_DOUBLE_EQ(seen->getY(2), 2.0);
  EXPECT_DOUBLE_EQ(seen->getX(3), -1.0);
  EXPECT_DOUBLE_EQ(seen->getY(3), -1.0);

  std::vector<SegmentString*>* out = scaled.getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 1u);
  const CoordinateSequence* rescaled = (*out)[0]->getCoordinates();
  ASSERT_EQ(rescaled->size(), 4u);
  EXPECT_DOUBLE_EQ(rescaled->getX(0), 1.5);
  EXPECT_DOUBLE_EQ(rescaled->getY(0), 2.5);
  EXPECT_DOUBLE_EQ(rescaled->getX(1), 1.0);
  EXPECT_DOUBLE_EQ(rescaled->getY(1), 2.0);
  EXPECT_DOUBLE_EQ(rescaled->getX(2), 2.0);
  EXPECT_DOUBLE_EQ(rescaled->getY(2), 3.0);
  EXPECT_DOUBLE_EQ(rescaled->getX(3), 0.5);
  EXPECT_DOUBLE_EQ(rescaled->getY(3), 1.5);
}

TEST(ScaledNoderTest, CollapsesRepeatedPointsAfterScalingAndPreservesData) {
  LineIntersector li;
  IntersectionAdder adder(li);
  MCIndexNoder mc_noder(&adder);
  ScaledNoder scaled(mc_noder, 10.0, 1.0, 2.0);

  int tag_collapse = 11;
  int tag_cross = 22;
  // First two points of segment 0 scale to (0, 0), triggering repeated-point
  // removal and replacement of input[0].
  std::vector<SegmentString*> input = {
      MakeSegmentString({{1.01, 2.01}, {1.04, 2.04}, {2.0, 3.0}},
                        &tag_collapse),
      MakeSegmentString({{1.0, 3.0}, {2.0, 2.0}}, &tag_cross),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  scaled.computeNodes(&input);

  // Replaced SegmentString in input[0] now has 2 points in scaled space and
  // retains tag_collapse.
  ASSERT_EQ(input[0]->size(), 2u);
  EXPECT_EQ(input[0]->getData(), &tag_collapse);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getX(1), 10.0);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getY(1), 10.0);

  std::vector<SegmentString*>* out = scaled.getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 4u);

  int count_collapse_tag = 0;
  int count_cross_tag = 0;
  bool saw_intersection_node = false;
  for (const SegmentString* ss : *out) {
    if (ss->getData() == &tag_collapse) {
      ++count_collapse_tag;
    } else if (ss->getData() == &tag_cross) {
      ++count_cross_tag;
    }
    const CoordinateSequence* cs = ss->getCoordinates();
    for (std::size_t i = 0; i < cs->size(); ++i) {
      if (std::abs(cs->getX(i) - 1.5) < 1e-12 &&
          std::abs(cs->getY(i) - 2.5) < 1e-12) {
        saw_intersection_node = true;
      }
    }
  }
  EXPECT_EQ(count_collapse_tag, 2);
  EXPECT_EQ(count_cross_tag, 2);
  EXPECT_TRUE(saw_intersection_node);
}

TEST(ScaledNoderTest, CollapsesMultipleConsecutiveAndAllRepeatedPoints) {
  RecordingNoder inner;
  ScaledNoder scaled(inner, 10.0, 1.0, 2.0);

  int tag_all = 1;
  int tag_mid = 2;
  int tag_keep = 3;
  std::vector<SegmentString*> input = {
      MakeSegmentString({{1.01, 2.01}, {1.02, 2.02}, {0.98, 1.98}}, &tag_all),
      MakeSegmentString({{1.0, 2.0}, {1.51, 2.51}, {1.52, 2.52}, {2.0, 3.0}},
                        &tag_mid),
      MakeSegmentString({{3.0, 4.0}, {4.0, 5.0}}, &tag_keep),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  scaled.computeNodes(&input);

  ASSERT_EQ(inner.snapshots().size(), 3u);
  ASSERT_EQ(inner.snapshots()[0].coords->size(), 1u);
  EXPECT_DOUBLE_EQ(inner.snapshots()[0].coords->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(inner.snapshots()[0].coords->getY(0), 0.0);
  EXPECT_EQ(inner.snapshots()[0].data, &tag_all);

  ASSERT_EQ(inner.snapshots()[1].coords->size(), 3u);
  EXPECT_DOUBLE_EQ(inner.snapshots()[1].coords->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(inner.snapshots()[1].coords->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(inner.snapshots()[1].coords->getX(1), 5.0);
  EXPECT_DOUBLE_EQ(inner.snapshots()[1].coords->getY(1), 5.0);
  EXPECT_DOUBLE_EQ(inner.snapshots()[1].coords->getX(2), 10.0);
  EXPECT_DOUBLE_EQ(inner.snapshots()[1].coords->getY(2), 10.0);
  EXPECT_EQ(inner.snapshots()[1].data, &tag_mid);

  ASSERT_EQ(inner.snapshots()[2].coords->size(), 2u);
  EXPECT_DOUBLE_EQ(inner.snapshots()[2].coords->getX(0), 20.0);
  EXPECT_DOUBLE_EQ(inner.snapshots()[2].coords->getY(0), 20.0);
  EXPECT_DOUBLE_EQ(inner.snapshots()[2].coords->getX(1), 30.0);
  EXPECT_DOUBLE_EQ(inner.snapshots()[2].coords->getY(1), 30.0);
  EXPECT_EQ(inner.snapshots()[2].data, &tag_keep);

  std::vector<SegmentString*>* out = scaled.getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 3u);
  EXPECT_NEAR((*out)[0]->getCoordinates()->getX(0), 1.0, 1e-12);
  EXPECT_NEAR((*out)[0]->getCoordinates()->getY(0), 2.0, 1e-12);
  EXPECT_NEAR((*out)[1]->getCoordinates()->getX(1), 1.5, 1e-12);
  EXPECT_NEAR((*out)[1]->getCoordinates()->getY(1), 2.5, 1e-12);
  EXPECT_NEAR((*out)[2]->getCoordinates()->getX(1), 4.0, 1e-12);
  EXPECT_NEAR((*out)[2]->getCoordinates()->getY(1), 5.0, 1e-12);
}

TEST(ScaledNoderTest, CollapsesRepeatedPointsOnBasicSegmentString) {
  RecordingNoder inner;
  ScaledNoder scaled(inner, 10.0, 1.0, 2.0);

  int tag = 99;
  auto raw_seq =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {1.01, 2.01}, {1.04, 2.04}, {2.0, 3.0}});
  // BasicSegmentString does not own raw_seq, so raw_seq outlives input;
  // when ScaledNoder collapses points, it deletes the BasicSegmentString and
  // installs an owning NodedSegmentString.
  std::vector<SegmentString*> input = {
      new BasicSegmentString(raw_seq.get(), &tag),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  scaled.computeNodes(&input);

  ASSERT_NE(dynamic_cast<NodedSegmentString*>(input[0]), nullptr);
  ASSERT_EQ(input[0]->size(), 2u);
  EXPECT_EQ(input[0]->getData(), &tag);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getX(1), 10.0);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getY(1), 10.0);
}

TEST(ScaledNoderTest, NodesCrossingSegmentsWithSimpleNoderAndOffset) {
  LineIntersector li;
  IntersectionAdder adder(li);
  SimpleNoder simple_noder(&adder);
  ScaledNoder scaled(simple_noder, 100.0, 10.0, 20.0);

  int ctx_a = 1;
  int ctx_b = 2;
  std::vector<SegmentString*> input = {
      MakeSegmentString({{10.00, 20.00}, {10.10, 20.10}}, &ctx_a),
      MakeSegmentString({{10.00, 20.10}, {10.10, 20.00}}, &ctx_b),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  scaled.computeNodes(&input);

  // Inputs are scaled in-place to [0..10] x [0..10] before rescaling.
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getX(0), 0.0);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getX(1), 10.0);
  EXPECT_DOUBLE_EQ(input[0]->getCoordinates()->getY(1), 10.0);

  std::vector<SegmentString*>* out = scaled.getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 4u);

  // Every substring should end or start at the rescaled midpoint
  // (10.05, 20.05).
  bool saw_midpoint = false;
  for (const SegmentString* ss : *out) {
    const CoordinateSequence* cs = ss->getCoordinates();
    ASSERT_EQ(cs->size(), 2u);
    if ((std::abs(cs->getX(0) - 10.05) < 1e-12 &&
         std::abs(cs->getY(0) - 20.05) < 1e-12) ||
        (std::abs(cs->getX(1) - 10.05) < 1e-12 &&
         std::abs(cs->getY(1) - 20.05) < 1e-12)) {
      saw_midpoint = true;
    }
  }
  EXPECT_TRUE(saw_midpoint);
}

TEST(ScaledNoderTest, SnapRoundingWithMCIndexSnapRounderAndOffset) {
  PrecisionModel integer_pm(PrecisionModel::FIXED);
  MCIndexSnapRounder snap_rounder(integer_pm);
  ScaledNoder scaled(snap_rounder, 100.0, 1000.0, -500.0);

  std::vector<SegmentString*> input = {
      MakeSegmentString({{1000.00, -500.00}, {1000.10, -499.90}}),
      MakeSegmentString({{1000.00, -499.90}, {1000.09, -500.00}}),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  scaled.computeNodes(&input);
  std::vector<SegmentString*>* out = scaled.getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };

  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 4u);

  // All resulting coordinates must lie on the 0.01 grid near (1000, -500).
  for (const SegmentString* ss : *out) {
    const CoordinateSequence* cs = ss->getCoordinates();
    for (std::size_t i = 0; i < cs->size(); ++i) {
      double x = cs->getX(i);
      double y = cs->getY(i);
      EXPECT_NEAR(x, std::round(x * 100.0) / 100.0, 1e-11);
      EXPECT_NEAR(y, std::round(y * 100.0) / 100.0, 1e-11);
      EXPECT_GE(x, 1000.0 - 1e-12);
      EXPECT_LE(x, 1000.10 + 1e-12);
      EXPECT_GE(y, -500.0 - 1e-12);
      EXPECT_LE(y, -499.90 + 1e-12);
    }
  }
}

TEST(ScaledNoderTest, DownscalesWithFractionalScaleFactor) {
  LineIntersector li;
  IntersectionAdder adder(li);
  MCIndexNoder mc_noder(&adder);
  ScaledNoder scaled(mc_noder, 0.5);
  EXPECT_FALSE(scaled.isIntegerPrecision());

  std::vector<SegmentString*> input = {
      MakeSegmentString({{0.0, 0.0}, {4.0, 4.0}}),
      MakeSegmentString({{0.0, 4.0}, {4.0, 0.0}}),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  scaled.computeNodes(&input);
  std::vector<SegmentString*>* out = scaled.getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };

  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 4u);
  EXPECT_NEAR((*out)[0]->getCoordinates()->getX(1), 2.0, 1e-12);
  EXPECT_NEAR((*out)[0]->getCoordinates()->getY(1), 2.0, 1e-12);
}

TEST(ScaledNoderTest, EmptyInputProducesEmptyOutput) {
  LineIntersector li;
  IntersectionAdder adder(li);
  MCIndexNoder mc_noder(&adder);
  ScaledNoder scaled(mc_noder, 10.0);

  std::vector<SegmentString*> empty_input;
  scaled.computeNodes(&empty_input);
  std::vector<SegmentString*>* out = scaled.getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };

  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(out->empty());

  RecordingNoder recording_inner;
  ScaledNoder unit_scaled(recording_inner, 1.0);
  std::vector<SegmentString*> unit_empty;
  unit_scaled.computeNodes(&unit_empty);
  std::vector<SegmentString*>* unit_out = unit_scaled.getNodedSubstrings();
  absl::Cleanup cleanup_unit_out = [&] { DeleteSegmentStringVector(unit_out); };

  ASSERT_NE(unit_out, nullptr);
  EXPECT_TRUE(unit_out->empty());
}

TEST(ScaledNoderTest, PreservesZAndMWhenNoPointsCollapse) {
  RecordingNoder inner;
  ScaledNoder scaled(inner, 10.0, 1.0, 2.0);

  auto seq_xyz = std::make_unique<CoordinateSequence>(
      std::initializer_list<Coordinate>{{1.0, 2.0, 5.5}, {2.0, 3.0, 9.5}});
  auto seq_xym = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYM>{{1.0, 2.0, 12.5}, {3.0, 4.0, 25.0}});
  auto seq_xyzm = std::make_unique<CoordinateSequence>(
      std::initializer_list<CoordinateXYZM>{{1.0, 2.0, 3.0, 7.0},
                                            {2.0, 2.0, 4.0, 8.0}});
  std::vector<SegmentString*> input = {
      new NodedSegmentString(seq_xyz.release(), true, false, nullptr),
      new NodedSegmentString(seq_xym.release(), false, true, nullptr),
      new NodedSegmentString(seq_xyzm.release(), true, true, nullptr),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  scaled.computeNodes(&input);
  std::vector<SegmentString*>* out = scaled.getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };

  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 3u);

  const CoordinateSequence* xyz_out = (*out)[0]->getCoordinates();
  EXPECT_TRUE(xyz_out->hasZ());
  EXPECT_FALSE(xyz_out->hasM());
  EXPECT_DOUBLE_EQ(xyz_out->getOrdinate(0, CoordinateSequence::Z), 5.5);
  EXPECT_DOUBLE_EQ(xyz_out->getOrdinate(1, CoordinateSequence::Z), 9.5);

  const CoordinateSequence* xym_out = (*out)[1]->getCoordinates();
  EXPECT_FALSE(xym_out->hasZ());
  EXPECT_TRUE(xym_out->hasM());
  EXPECT_TRUE(std::isnan(xym_out->getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_DOUBLE_EQ(xym_out->getOrdinate(0, CoordinateSequence::M), 12.5);
  EXPECT_DOUBLE_EQ(xym_out->getOrdinate(1, CoordinateSequence::M), 25.0);
  CoordinateXYM out_pt0;
  xym_out->getAt(0, out_pt0);
  EXPECT_NEAR(out_pt0.x, 1.0, 1e-12);
  EXPECT_NEAR(out_pt0.y, 2.0, 1e-12);
  EXPECT_DOUBLE_EQ(out_pt0.m, 12.5);

  const CoordinateSequence* xyzm_out = (*out)[2]->getCoordinates();
  EXPECT_TRUE(xyzm_out->hasZ());
  EXPECT_TRUE(xyzm_out->hasM());
  EXPECT_DOUBLE_EQ(xyzm_out->getOrdinate(0, CoordinateSequence::Z), 3.0);
  EXPECT_DOUBLE_EQ(xyzm_out->getOrdinate(0, CoordinateSequence::M), 7.0);
  EXPECT_DOUBLE_EQ(xyzm_out->getOrdinate(1, CoordinateSequence::Z), 4.0);
  EXPECT_DOUBLE_EQ(xyzm_out->getOrdinate(1, CoordinateSequence::M), 8.0);
}

TEST(ScaledNoderTest, PolymorphicNoderLifecycleAndRescaling) {
  LineIntersector li;
  IntersectionAdder adder(li);
  MCIndexNoder mc_noder(&adder);
  std::unique_ptr<Noder> noder =
      std::make_unique<ScaledNoder>(mc_noder, 10.0, 5.0, -5.0);

  std::vector<SegmentString*> input = {
      MakeSegmentString({{5.0, -5.0}, {6.0, -4.0}}),
      MakeSegmentString({{5.0, -4.0}, {6.0, -5.0}}),
  };
  absl::Cleanup cleanup_input = [&] { DeleteSegmentStrings(input); };

  noder->computeNodes(&input);
  std::vector<SegmentString*>* out = noder->getNodedSubstrings();
  absl::Cleanup cleanup_out = [&] { DeleteSegmentStringVector(out); };

  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->size(), 4u);
  noder.reset();
}

}  // namespace
}  // namespace geos_google
