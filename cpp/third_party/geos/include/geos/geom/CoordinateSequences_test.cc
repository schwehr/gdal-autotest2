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

// Unit tests for third_party/geos/include/geos/geom/CoordinateSequences.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/geom/CoordinateSequences.h

#include "third_party/geos/include/geos/geom/CoordinateSequences.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateMutator;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateSequences;
using ::geos::geom::CoordinateType;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYM;
using ::geos::geom::CoordinateXYZM;
using ::geos::geom::Ordinate;

template <typename T>
constexpr CoordinateType CoordinateTypeOf() {
  if constexpr (std::is_same_v<T, CoordinateXY>) {
    return CoordinateType::XY;
  } else if constexpr (std::is_same_v<T, Coordinate>) {
    return CoordinateType::XYZ;
  } else if constexpr (std::is_same_v<T, CoordinateXYM>) {
    return CoordinateType::XYM;
  } else {
    static_assert(std::is_same_v<T, CoordinateXYZM>);
    return CoordinateType::XYZM;
  }
}

CoordinateSequence MakeSequence(CoordinateType kind, double base) {
  switch (kind) {
    case CoordinateType::XY: {
      CoordinateSequence seq = CoordinateSequence::XY(2);
      seq.setAt(CoordinateXY(base, base + 1.0), 0);
      seq.setAt(CoordinateXY(base + 10.0, base + 11.0), 1);
      return seq;
    }
    case CoordinateType::XYZ: {
      CoordinateSequence seq = CoordinateSequence::XYZ(2);
      seq.setAt(Coordinate(base, base + 1.0, base + 2.0), 0);
      seq.setAt(Coordinate(base + 10.0, base + 11.0, base + 12.0), 1);
      return seq;
    }
    case CoordinateType::XYM: {
      CoordinateSequence seq = CoordinateSequence::XYM(2);
      seq.setAt(CoordinateXYM(base, base + 1.0, base + 3.0), 0);
      seq.setAt(CoordinateXYM(base + 10.0, base + 11.0, base + 13.0), 1);
      return seq;
    }
    case CoordinateType::XYZM: {
      CoordinateSequence seq = CoordinateSequence::XYZM(2);
      seq.setAt(CoordinateXYZM(base, base + 1.0, base + 2.0, base + 3.0), 0);
      seq.setAt(
          CoordinateXYZM(base + 10.0, base + 11.0, base + 12.0, base + 13.0),
          1);
      return seq;
    }
  }
  return CoordinateSequence();
}

struct DispatchRecorder {
  int call_count = 0;
  std::vector<std::pair<CoordinateType, CoordinateType>> dispatched_pairs;

  template <typename T1, typename T2>
  void operator()() {
    ++call_count;
    dispatched_pairs.emplace_back(CoordinateTypeOf<T1>(),
                                  CoordinateTypeOf<T2>());
  }
};

struct MoveOnlyArgsFunctor {
  std::unique_ptr<int> state = std::make_unique<int>(0);
  int received_tag = 0;
  std::string received_label;
  double max_xy_diff = 0.0;
  std::size_t nan_z_count = 0;
  std::size_t nan_m_count = 0;

  MoveOnlyArgsFunctor() = default;
  MoveOnlyArgsFunctor(const MoveOnlyArgsFunctor&) = delete;
  MoveOnlyArgsFunctor& operator=(const MoveOnlyArgsFunctor&) = delete;
  MoveOnlyArgsFunctor(MoveOnlyArgsFunctor&&) = default;
  MoveOnlyArgsFunctor& operator=(MoveOnlyArgsFunctor&&) = default;

  template <typename T1, typename T2>
  void operator()(const CoordinateSequence* s1, const CoordinateSequence* s2,
                  int tag, std::string label, std::vector<double>* out_xs) {
    ++(*state);
    received_tag = tag;
    received_label = std::move(label);

    const std::size_t n = std::min(s1->size(), s2->size());
    for (std::size_t i = 0; i < n; ++i) {
      const T1& c1 = s1->getAt<T1>(i);
      const T2& c2 = s2->getAt<T2>(i);
      out_xs->push_back(c1.x + c2.x);
      max_xy_diff = std::max(max_xy_diff, std::abs(c1.x - c2.x));
      max_xy_diff = std::max(max_xy_diff, std::abs(c1.y - c2.y));
      if (std::isnan(c1.template get<Ordinate::Z>())) {
        ++nan_z_count;
      }
      if (std::isnan(c2.template get<Ordinate::Z>())) {
        ++nan_z_count;
      }
      if (std::isnan(c1.template get<Ordinate::M>())) {
        ++nan_m_count;
      }
      if (std::isnan(c2.template get<Ordinate::M>())) {
        ++nan_m_count;
      }
    }
  }
};

TEST(CoordinateSequencesTest, DispatchesAllCoordinateSequenceKindPairs) {
  constexpr std::array<CoordinateType, 4> kKinds = {
      CoordinateType::XY,
      CoordinateType::XYZ,
      CoordinateType::XYM,
      CoordinateType::XYZM,
  };

  // Note: CoordinateSequence.cpp defines GEOS_COORDSEQ_PADZ, so XY sequences
  // are backed by stride 3 (CoordinateType::XYZ) and XYM sequences are backed
  // by stride 4 (CoordinateType::XYZM). The 12 XY/XYM switch cases in
  // CoordinateSequences::binaryDispatch are therefore unreachable under PADZ.
  EXPECT_EQ(MakeSequence(CoordinateType::XY, 0.0).getCoordinateType(),
            CoordinateType::XYZ);
  EXPECT_EQ(MakeSequence(CoordinateType::XYZ, 0.0).getCoordinateType(),
            CoordinateType::XYZ);
  EXPECT_EQ(MakeSequence(CoordinateType::XYM, 0.0).getCoordinateType(),
            CoordinateType::XYZM);
  EXPECT_EQ(MakeSequence(CoordinateType::XYZM, 0.0).getCoordinateType(),
            CoordinateType::XYZM);

  for (CoordinateType kind1 : kKinds) {
    CoordinateSequence seq1 = MakeSequence(kind1, 1.0);
    for (CoordinateType kind2 : kKinds) {
      CoordinateSequence seq2 = MakeSequence(kind2, 2.0);
      DispatchRecorder recorder;
      CoordinateSequences::binaryDispatch(seq1, seq2, recorder);

      EXPECT_EQ(recorder.call_count, 1);
      ASSERT_EQ(recorder.dispatched_pairs.size(), 1u);
      EXPECT_EQ(recorder.dispatched_pairs[0].first, seq1.getCoordinateType());
      EXPECT_EQ(recorder.dispatched_pairs[0].second, seq2.getCoordinateType());
    }
  }
}

TEST(CoordinateSequencesTest, DispatchesAllConstructorsAndEmptySequences) {
  std::vector<CoordinateSequence> sequences;
  sequences.emplace_back();
  sequences.emplace_back(0, 2);
  sequences.emplace_back(0, 3);
  sequences.emplace_back(0, 4);
  sequences.push_back(CoordinateSequence::XY(0));
  sequences.push_back(CoordinateSequence::XYZ(0));
  sequences.push_back(CoordinateSequence::XYM(0));
  sequences.push_back(CoordinateSequence::XYZM(0));
  sequences.emplace_back(2, 2);
  sequences.emplace_back(2, 3);
  sequences.emplace_back(2, 4);
  sequences.push_back(CoordinateSequence{CoordinateXY(1.0, 2.0)});
  sequences.push_back(CoordinateSequence{Coordinate(1.0, 2.0, 3.0)});
  sequences.push_back(CoordinateSequence{CoordinateXYM(1.0, 2.0, 4.0)});
  sequences.push_back(CoordinateSequence{CoordinateXYZM(1.0, 2.0, 3.0, 4.0)});

  for (const auto& s1 : sequences) {
    for (const auto& s2 : sequences) {
      DispatchRecorder recorder;
      CoordinateSequences::binaryDispatch(s1, s2, recorder);
      EXPECT_EQ(recorder.call_count, 1);
      ASSERT_EQ(recorder.dispatched_pairs.size(), 1u);
      EXPECT_EQ(recorder.dispatched_pairs[0].first, s1.getCoordinateType());
      EXPECT_EQ(recorder.dispatched_pairs[0].second, s2.getCoordinateType());
    }
  }
}

TEST(CoordinateSequencesTest, ForwardsArgumentsAndMutatesFunctorByReference) {
  CoordinateSequence seq_xyz = MakeSequence(CoordinateType::XYZ, 10.0);
  CoordinateSequence seq_xyzm = MakeSequence(CoordinateType::XYZM, 13.5);
  CoordinateSequence seq_xy = MakeSequence(CoordinateType::XY, 1.0);
  CoordinateSequence seq_xym = MakeSequence(CoordinateType::XYM, 4.0);

  {
    MoveOnlyArgsFunctor functor;
    std::vector<double> out_xs;
    CoordinateSequences::binaryDispatch(seq_xyz, seq_xyzm, functor, &seq_xyz,
                                        &seq_xyzm, 7, std::string("xyz-xyzm"),
                                        &out_xs);
    EXPECT_EQ(*functor.state, 1);
    EXPECT_EQ(functor.received_tag, 7);
    EXPECT_EQ(functor.received_label, "xyz-xyzm");
    ASSERT_EQ(out_xs.size(), 2u);
    EXPECT_DOUBLE_EQ(out_xs[0], 23.5);
    EXPECT_DOUBLE_EQ(out_xs[1], 43.5);
    EXPECT_DOUBLE_EQ(functor.max_xy_diff, 3.5);
    EXPECT_EQ(functor.nan_z_count, 0u);
    EXPECT_EQ(functor.nan_m_count, 2u);
  }

  {
    MoveOnlyArgsFunctor functor;
    std::vector<double> out_xs;
    CoordinateSequences::binaryDispatch(seq_xy, seq_xym, functor, &seq_xy,
                                        &seq_xym, 9, std::string("xy-xym"),
                                        &out_xs);
    EXPECT_EQ(*functor.state, 1);
    EXPECT_EQ(functor.received_tag, 9);
    EXPECT_EQ(functor.received_label, "xy-xym");
    ASSERT_EQ(out_xs.size(), 2u);
    EXPECT_DOUBLE_EQ(out_xs[0], 5.0);
    EXPECT_DOUBLE_EQ(out_xs[1], 25.0);
    EXPECT_DOUBLE_EQ(functor.max_xy_diff, 3.0);
    // Both XY and XYM pad missing Z with NaN; XY also lacks M.
    EXPECT_EQ(functor.nan_z_count, 4u);
    EXPECT_EQ(functor.nan_m_count, 2u);
  }
}

struct ConstDispatchRecorder {
  mutable int call_count = 0;
  mutable std::pair<CoordinateType, CoordinateType> last_pair{
      CoordinateType::XY, CoordinateType::XY};

  template <typename T1, typename T2>
  void operator()(int& out_sum, int delta) const {
    ++call_count;
    last_pair = {CoordinateTypeOf<T1>(), CoordinateTypeOf<T2>()};
    out_sum += delta;
  }
};

struct SetZFilter : public CoordinateMutator<SetZFilter> {
  void filter(CoordinateXY*) const {}
  void filter(Coordinate* c) const { c->z = 99.0; }
  void filter(CoordinateXYM*) const {}
  void filter(CoordinateXYZM* c) const { c->z = 99.0; }
};

TEST(CoordinateSequencesTest, DispatchesMutatedClonedAndAliasedSequences) {
  CoordinateSequence seq = CoordinateSequence::XYZM(1);
  EXPECT_EQ(seq.getCoordinateType(), CoordinateType::XYZM);

  // setPoints resets backing storage to stride-3 XYZ without M.
  seq.setPoints(std::vector<Coordinate>{Coordinate(1.0, 2.0, 3.0),
                                        Coordinate(4.0, 5.0, 6.0)});
  EXPECT_EQ(seq.getCoordinateType(), CoordinateType::XYZ);
  {
    DispatchRecorder recorder;
    CoordinateSequences::binaryDispatch(seq, seq, recorder);
    EXPECT_EQ(recorder.call_count, 1);
    ASSERT_EQ(recorder.dispatched_pairs.size(), 1u);
    EXPECT_EQ(recorder.dispatched_pairs[0],
              std::make_pair(CoordinateType::XYZ, CoordinateType::XYZ));
  }

  CoordinateSequence xy_via_setpoints = CoordinateSequence::XYZM(1);
  xy_via_setpoints.setPoints(std::vector<CoordinateXY>{CoordinateXY(7.0, 8.0)});
  EXPECT_EQ(xy_via_setpoints.getCoordinateType(), CoordinateType::XYZ);

  // apply_rw resets m_hasdim/m_hasz without altering the backing coordinate
  // type.
  SetZFilter set_z;
  xy_via_setpoints.apply_rw(&set_z);
  EXPECT_TRUE(xy_via_setpoints.hasZ());
  EXPECT_EQ(xy_via_setpoints.getCoordinateType(), CoordinateType::XYZ);

  CoordinateSequence uninit_xy(2, false, false, false);
  CoordinateSequence uninit_xym(2, false, true, false);
  CoordinateSequence uninit_xyz(2, true, false, false);
  CoordinateSequence uninit_xyzm(2, true, true, false);
  EXPECT_EQ(uninit_xy.getCoordinateType(), CoordinateType::XYZ);
  EXPECT_EQ(uninit_xym.getCoordinateType(), CoordinateType::XYZM);
  EXPECT_EQ(uninit_xyz.getCoordinateType(), CoordinateType::XYZ);
  EXPECT_EQ(uninit_xyzm.getCoordinateType(), CoordinateType::XYZM);

  std::unique_ptr<CoordinateSequence> cloned_xym = uninit_xym.clone();
  ASSERT_NE(cloned_xym, nullptr);
  CoordinateSequence copied(*cloned_xym);
  CoordinateSequence moved(std::move(copied));
  EXPECT_EQ(moved.getCoordinateType(), CoordinateType::XYZM);

  DispatchRecorder recorder;
  CoordinateSequences::binaryDispatch(xy_via_setpoints, moved, recorder);
  CoordinateSequences::binaryDispatch(moved, moved, recorder);
  CoordinateSequences::binaryDispatch(uninit_xy, uninit_xyz, recorder);
  CoordinateSequences::binaryDispatch(uninit_xyz, uninit_xyzm, recorder);
  ASSERT_EQ(recorder.dispatched_pairs.size(), 4u);
  EXPECT_EQ(recorder.dispatched_pairs[0],
            std::make_pair(CoordinateType::XYZ, CoordinateType::XYZM));
  EXPECT_EQ(recorder.dispatched_pairs[1],
            std::make_pair(CoordinateType::XYZM, CoordinateType::XYZM));
  EXPECT_EQ(recorder.dispatched_pairs[2],
            std::make_pair(CoordinateType::XYZ, CoordinateType::XYZ));
  EXPECT_EQ(recorder.dispatched_pairs[3],
            std::make_pair(CoordinateType::XYZ, CoordinateType::XYZM));
}

TEST(CoordinateSequencesTest,
     SupportsConstFunctorTemplateLambdaAndReferenceWrapperArgs) {
  const CoordinateSequence seq_xy = MakeSequence(CoordinateType::XY, 1.0);
  const CoordinateSequence seq_xym = MakeSequence(CoordinateType::XYM, 4.0);

  const ConstDispatchRecorder const_recorder;
  int accumulated = 10;
  CoordinateSequences::binaryDispatch(seq_xy, seq_xym, const_recorder,
                                      std::ref(accumulated), 15);
  EXPECT_EQ(const_recorder.call_count, 1);
  EXPECT_EQ(const_recorder.last_pair,
            std::make_pair(CoordinateType::XYZ, CoordinateType::XYZM));
  EXPECT_EQ(accumulated, 25);

  int observed_count = 0;
  CoordinateType first_type = CoordinateType::XY;
  CoordinateType second_type = CoordinateType::XY;
  auto templated_lambda = [&]<typename T1, typename T2>(
                              const CoordinateSequence* s1,
                              const CoordinateSequence* s2) {
    ++observed_count;
    first_type = CoordinateTypeOf<T1>();
    second_type = CoordinateTypeOf<T2>();
    ASSERT_EQ(s1->size(), 2u);
    ASSERT_EQ(s2->size(), 2u);
    EXPECT_DOUBLE_EQ(s1->getAt<T1>(0).x, 4.0);
    EXPECT_TRUE(std::isnan(s1->getAt<T1>(0).template get<Ordinate::Z>()));
    EXPECT_DOUBLE_EQ(s1->getAt<T1>(0).template get<Ordinate::M>(), 7.0);
    EXPECT_DOUBLE_EQ(s2->getAt<T2>(1).x, 11.0);
    EXPECT_TRUE(std::isnan(s2->getAt<T2>(1).template get<Ordinate::Z>()));
    EXPECT_TRUE(std::isnan(s2->getAt<T2>(1).template get<Ordinate::M>()));
  };
  CoordinateSequences::binaryDispatch(seq_xym, seq_xy, templated_lambda,
                                      &seq_xym, &seq_xy);
  EXPECT_EQ(observed_count, 1);
  EXPECT_EQ(first_type, CoordinateType::XYZM);
  EXPECT_EQ(second_type, CoordinateType::XYZ);
}

struct NonVoidCommonTypeFunctor {
  int invocations = 0;
  CoordinateType last_common_type = CoordinateType::XY;

  template <typename T1, typename T2>
  bool operator()(const CoordinateSequence* s1, const CoordinateSequence* s2) {
    ++invocations;
    last_common_type = CoordinateTypeOf<std::common_type_t<T1, T2>>();
    return !s1->isEmpty() && !s2->isEmpty();
  }
};

TEST(CoordinateSequencesTest,
     SupportsNonVoidFunctorExplicitReferenceArgsAndInPlaceOps) {
  CoordinateSequence xy_seq = CoordinateSequence::XY(0);
  xy_seq.add(1.0, 2.0);
  xy_seq.add(3.0, 4.0);
  xy_seq.closeRing();
  xy_seq.reverse();
  xy_seq.sort();
  xy_seq.pop_back();
  xy_seq.resize(4);

  CoordinateSequence xym_seq = CoordinateSequence::XYM(0);
  xym_seq.add(CoordinateXYM(1.0, 2.0, 5.0));
  xym_seq.add(Coordinate(3.0, 4.0, 9.0));
  xym_seq.closeRing();
  xym_seq.reverse();
  xym_seq.sort();
  xym_seq.pop_back();

  NonVoidCommonTypeFunctor common_functor;
  CoordinateSequences::binaryDispatch(xy_seq, xy_seq, common_functor, &xy_seq,
                                      &xy_seq);
  EXPECT_EQ(common_functor.invocations, 1);
  EXPECT_EQ(common_functor.last_common_type, CoordinateType::XYZ);

  CoordinateSequences::binaryDispatch(xy_seq, xym_seq, common_functor, &xy_seq,
                                      &xym_seq);
  EXPECT_EQ(common_functor.invocations, 2);
  EXPECT_EQ(common_functor.last_common_type, CoordinateType::XYZM);

  CoordinateSequences::binaryDispatch(xym_seq, xy_seq, common_functor, &xym_seq,
                                      &xy_seq);
  EXPECT_EQ(common_functor.invocations, 3);
  EXPECT_EQ(common_functor.last_common_type, CoordinateType::XYZM);

  CoordinateSequences::binaryDispatch(xym_seq, xym_seq, common_functor,
                                      &xym_seq, &xym_seq);
  EXPECT_EQ(common_functor.invocations, 4);
  EXPECT_EQ(common_functor.last_common_type, CoordinateType::XYZM);

  const ConstDispatchRecorder const_recorder;
  int ref_total = 3;
  CoordinateSequences::binaryDispatch<const ConstDispatchRecorder, int&, int>(
      xy_seq, xym_seq, const_recorder, ref_total, 7);
  EXPECT_EQ(const_recorder.call_count, 1);
  EXPECT_EQ(const_recorder.last_pair,
            std::make_pair(CoordinateType::XYZ, CoordinateType::XYZM));
  EXPECT_EQ(ref_total, 10);

  xy_seq.clear();
  xym_seq.clear();
  CoordinateSequences::binaryDispatch(xy_seq, xym_seq, common_functor, &xy_seq,
                                      &xym_seq);
  EXPECT_EQ(common_functor.invocations, 5);
  EXPECT_EQ(common_functor.last_common_type, CoordinateType::XYZM);
}

}  // namespace
}  // namespace geos_google
