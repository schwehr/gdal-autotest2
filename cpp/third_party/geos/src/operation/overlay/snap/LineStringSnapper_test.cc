// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/operation/overlay/snap/LineStringSnapper.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/util/GEOSException.h"
#include "third_party/geos/include/geos/util/Interrupt.h"

namespace geos_google {

struct FindSnapForVertexTag {
  using type = geos::geom::Coordinate::ConstVect::const_iterator (
      geos::operation::overlay::snap::LineStringSnapper::*)(
      const geos::geom::Coordinate&, const geos::geom::Coordinate::ConstVect&);
};

template <typename Tag, typename Tag::type M>
struct LineStringSnapperAccessor {
  friend typename Tag::type GetLineStringSnapperFn(Tag) { return M; }
};

typename FindSnapForVertexTag::type GetLineStringSnapperFn(
    FindSnapForVertexTag);
template struct LineStringSnapperAccessor<
    FindSnapForVertexTag,
    &geos::operation::overlay::snap::LineStringSnapper::findSnapForVertex>;

namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::operation::overlay::snap::LineStringSnapper;

std::unique_ptr<CoordinateSequence> RunSnap(
    std::initializer_list<Coordinate> src_coords,
    const std::vector<Coordinate>& snap_pts, double tolerance,
    bool allow_snapping_to_source_vertices = false) {
  CoordinateSequence src_seq(src_coords);
  Coordinate::ConstVect snap_ptrs;
  snap_ptrs.reserve(snap_pts.size());
  for (const auto& pt : snap_pts) {
    snap_ptrs.push_back(&pt);
  }
  LineStringSnapper snapper(src_seq, tolerance);
  snapper.setAllowSnappingToSourceVertices(allow_snapping_to_source_vertices);
  return snapper.snapTo(snap_ptrs);
}

void ExpectCoordsNear(const CoordinateSequence& actual,
                      std::initializer_list<Coordinate> expected,
                      double tol = 1e-12) {
  ASSERT_EQ(actual.size(), expected.size());
  std::size_t i = 0;
  for (const auto& exp : expected) {
    EXPECT_NEAR(actual[i].x, exp.x, tol) << "at index " << i;
    EXPECT_NEAR(actual[i].y, exp.y, tol) << "at index " << i;
    ++i;
  }
}

TEST(LineStringSnapperTest, EmptyAndSinglePointSequences) {
  auto empty_res = RunSnap({}, {Coordinate(1, 1)}, 1.0);
  EXPECT_TRUE(empty_res->isEmpty());

  auto single_snapped =
      RunSnap({Coordinate(1.0, 1.0)}, {Coordinate(1.05, 1.0)}, 0.1);
  ExpectCoordsNear(*single_snapped, {Coordinate(1.05, 1.0)});

  auto single_unsnapped =
      RunSnap({Coordinate(1.0, 1.0)}, {Coordinate(2.0, 2.0)}, 0.1);
  ExpectCoordsNear(*single_unsnapped, {Coordinate(1.0, 1.0)});

  // A 2-point degenerate closed sequence syncs both endpoints.
  auto two_pt_closed = RunSnap({Coordinate(0.0, 0.0), Coordinate(0.0, 0.0)},
                               {Coordinate(0.05, 0.0)}, 0.2);
  ExpectCoordsNear(*two_pt_closed,
                   {Coordinate(0.05, 0.0), Coordinate(0.05, 0.0)});
}

TEST(LineStringSnapperTest, EmptySnapPointsLeavesSequenceUnchanged) {
  auto open_res = RunSnap({Coordinate(0, 0), Coordinate(10, 0)}, {}, 1.0);
  ExpectCoordsNear(*open_res, {Coordinate(0, 0), Coordinate(10, 0)});

  auto closed_res = RunSnap(
      {Coordinate(0, 0), Coordinate(1, 0), Coordinate(0, 1), Coordinate(0, 0)},
      {}, 1.0);
  ExpectCoordsNear(*closed_res, {Coordinate(0, 0), Coordinate(1, 0),
                                 Coordinate(0, 1), Coordinate(0, 0)});
}

TEST(LineStringSnapperTest, VertexSnappingToleranceAndClosestCandidate) {
  // Distance equal to snapTolerance does not snap vertex or segment.
  auto equal_rejected =
      RunSnap({Coordinate(0, 0), Coordinate(10, 0)}, {Coordinate(0, 0.5)}, 0.5);
  ExpectCoordsNear(*equal_rejected, {Coordinate(0, 0), Coordinate(10, 0)});

  // Distance strictly less than snapTolerance snaps; opposite-side point at
  // exact snapTolerance stays outside tolerance even after segment tilt.
  auto boundary_res = RunSnap({Coordinate(0, 0), Coordinate(10, 0)},
                              {Coordinate(0, -0.5), Coordinate(10, 0.49)}, 0.5);
  ExpectCoordsNear(*boundary_res, {Coordinate(0, 0), Coordinate(10, 0.49)});

  // Zero and negative tolerances never snap anything.
  auto zero_tol = RunSnap({Coordinate(0, 0), Coordinate(10, 0)},
                          {Coordinate(0, 0), Coordinate(5, 0)}, 0.0);
  ExpectCoordsNear(*zero_tol, {Coordinate(0, 0), Coordinate(10, 0)});

  auto neg_tol = RunSnap({Coordinate(0, 0), Coordinate(10, 0)},
                         {Coordinate(0, 0), Coordinate(5, 0)}, -1.0);
  ExpectCoordsNear(*neg_tol, {Coordinate(0, 0), Coordinate(10, 0)});

  // Exact vertex match (dist == 0.0) returns immediately even if another
  // vertex is also within snapTolerance.
  auto exact_match_res =
      RunSnap({Coordinate(0.05, 0), Coordinate(0, 0), Coordinate(10, 0)},
              {Coordinate(0, 0)}, 0.2);
  ExpectCoordsNear(*exact_match_res,
                   {Coordinate(0.05, 0), Coordinate(0, 0), Coordinate(10, 0)});

  // Closest vertex wins regardless of vertex ordering:
  // First vertex closer than second (second skipped via dist >= minDist).
  auto first_closer =
      RunSnap({Coordinate(0.02, 0), Coordinate(0.08, 0), Coordinate(10, 0)},
              {Coordinate(0, 0)}, 0.2);
  ExpectCoordsNear(*first_closer,
                   {Coordinate(0, 0), Coordinate(0.08, 0), Coordinate(10, 0)});

  // Second vertex closer than first (updates candidate and minDist).
  auto second_closer =
      RunSnap({Coordinate(0.08, 0), Coordinate(0.02, 0), Coordinate(10, 0)},
              {Coordinate(0, 0)}, 0.2);
  ExpectCoordsNear(*second_closer,
                   {Coordinate(0.08, 0), Coordinate(0, 0), Coordinate(10, 0)});
}

TEST(LineStringSnapperTest,
     ClosedRingSyncsStartAndEndVertexOnlyWhenStartSnaps) {
  // Snapping first vertex updates both index 0 and closing index 4.
  auto start_snapped =
      RunSnap({Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
               Coordinate(0, 10), Coordinate(0, 0)},
              {Coordinate(0.05, 0.05)}, 0.2);
  ExpectCoordsNear(*start_snapped, {Coordinate(0.05, 0.05), Coordinate(10, 0),
                                    Coordinate(10, 10), Coordinate(0, 10),
                                    Coordinate(0.05, 0.05)});

  // Snapping an interior vertex leaves closing point untouched.
  auto mid_snapped =
      RunSnap({Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
               Coordinate(0, 10), Coordinate(0, 0)},
              {Coordinate(10.05, 0.05)}, 0.2);
  ExpectCoordsNear(*mid_snapped,
                   {Coordinate(0, 0), Coordinate(10.05, 0.05),
                    Coordinate(10, 10), Coordinate(0, 10), Coordinate(0, 0)});

  // Snapping start vertex of an open line updates only index 0.
  auto open_start_snapped = RunSnap({Coordinate(0, 0), Coordinate(10, 0)},
                                    {Coordinate(0.05, 0.05)}, 0.2);
  ExpectCoordsNear(*open_start_snapped,
                   {Coordinate(0.05, 0.05), Coordinate(10, 0)});
}

TEST(LineStringSnapperTest, SegmentSnappingInteriorExactAndRejections) {
  // Interior projection factor (0 < pf < 1) cracks the segment.
  auto interior_res = RunSnap({Coordinate(0, 0), Coordinate(10, 0)},
                              {Coordinate(5, 0.05)}, 0.2);
  ExpectCoordsNear(*interior_res,
                   {Coordinate(0, 0), Coordinate(5, 0.05), Coordinate(10, 0)});

  // Exact collinear point (dist == 0.0) returns immediately on first segment
  // while leaving vertices beyond snapTolerance untouched.
  auto collinear_res =
      RunSnap({Coordinate(0, 0), Coordinate(10, 0), Coordinate(5, 0.5)},
              {Coordinate(5, 0)}, 0.2);
  ExpectCoordsNear(*collinear_res, {Coordinate(0, 0), Coordinate(5, 0),
                                    Coordinate(10, 0), Coordinate(5, 0.5)});

  // Exact envelope squared-distance boundary (dist^2 == minDist^2) rejects.
  auto env_boundary_rejected =
      RunSnap({Coordinate(0, 0), Coordinate(10, 0)}, {Coordinate(5, 0.5)}, 0.5);
  ExpectCoordsNear(*env_boundary_rejected,
                   {Coordinate(0, 0), Coordinate(10, 0)});

  // Envelope check rejects distant segment; diagonal segment passes envelope
  // check for (0, 10) but is rejected by segment distance >= minDist.
  auto diag_rejected =
      RunSnap({Coordinate(0, 0), Coordinate(10, 10), Coordinate(20, 20)},
              {Coordinate(0, 10)}, 1.0);
  ExpectCoordsNear(*diag_rejected,
                   {Coordinate(0, 0), Coordinate(10, 10), Coordinate(20, 20)});

  // When two segments are within tolerance, closer segment receives the snap.
  auto closer_seg = RunSnap({Coordinate(0, 0.15), Coordinate(10, 0.15),
                             Coordinate(10, 0.05), Coordinate(0, 0.05)},
                            {Coordinate(5, 0)}, 0.5);
  ExpectCoordsNear(*closer_seg, {Coordinate(0, 0.15), Coordinate(10, 0.15),
                                 Coordinate(10, 0.05), Coordinate(5, 0),
                                 Coordinate(0, 0.05)});
}

TEST(LineStringSnapperTest, AllowSnappingToSourceVerticesControlsSegmentMatch) {
  // Default (allow=false) aborts segment seek when snapPt matches a vertex.
  auto disallowed = RunSnap(
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(0, 0.05)},
      {Coordinate(0, 0)}, 0.2, /*allow_snapping_to_source_vertices=*/false);
  ExpectCoordsNear(*disallowed,
                   {Coordinate(0, 0), Coordinate(10, 0), Coordinate(0, 0.05)});

  // With allow=true, segment 0 is skipped and segment 1 snaps to (0, 0).
  auto allowed = RunSnap(
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(0, 0.05)},
      {Coordinate(0, 0)}, 0.2, /*allow_snapping_to_source_vertices=*/true);
  ExpectCoordsNear(*allowed, {Coordinate(0, 0), Coordinate(10, 0),
                              Coordinate(0, 0), Coordinate(0, 0.05)});

  // Same check when snapPt matches seg.p1 (second disjunct of equals2D check).
  auto disallowed_p1 = RunSnap({Coordinate(10, 0), Coordinate(0, 0),
                                Coordinate(10, 0.05), Coordinate(0, 0.05)},
                               {Coordinate(0, 0)}, 0.2,
                               /*allow_snapping_to_source_vertices=*/false);
  ExpectCoordsNear(*disallowed_p1, {Coordinate(10, 0), Coordinate(0, 0),
                                    Coordinate(10, 0.05), Coordinate(0, 0.05)});

  // With allow=true, segment 2 matches with exact pf == 1.0 boundary.
  auto allowed_p1 = RunSnap({Coordinate(10, 0), Coordinate(0, 0),
                             Coordinate(10, 0.05), Coordinate(0, 0.05)},
                            {Coordinate(0, 0)}, 0.2,
                            /*allow_snapping_to_source_vertices=*/true);
  ExpectCoordsNear(*allowed_p1,
                   {Coordinate(10, 0), Coordinate(0, 0), Coordinate(10, 0.05),
                    Coordinate(0, 0.05), Coordinate(0, 0)});
}

TEST(LineStringSnapperTest, SegmentSnapProjectionFactorAtLeastOneOpenLine) {
  // End segment (to == too_far) on an open line inserts old endpoint before
  // re-snapped end point.
  auto end_open = RunSnap({Coordinate(0, 0), Coordinate(10, 0)},
                          {Coordinate(10.5, 0.1), Coordinate(9.5, 0.0)}, 2.0);
  ExpectCoordsNear(*end_open, {Coordinate(0, 0), Coordinate(9.5, 0.0),
                               Coordinate(10.5, 0.1)});

  // Middle segment where nextSeg is closer to old endpoint than seg.
  auto next_closer =
      RunSnap({Coordinate(0, 0), Coordinate(10, 0), Coordinate(0, -0.5)},
              {Coordinate(10.5, 0.2), Coordinate(9.5, 0.0)}, 2.0);
  ExpectCoordsNear(*next_closer, {Coordinate(0, 0), Coordinate(10.5, 0.2),
                                  Coordinate(9.5, 0.0), Coordinate(0, -0.5)});

  // Middle segment where seg is closer to old endpoint than nextSeg.
  auto current_closer =
      RunSnap({Coordinate(0, 0), Coordinate(10, 0), Coordinate(0, 5.0)},
              {Coordinate(10.5, 0.2), Coordinate(9.5, 0.0)}, 2.0);
  ExpectCoordsNear(*current_closer,
                   {Coordinate(0, 0), Coordinate(9.5, 0.0),
                    Coordinate(10.5, 0.2), Coordinate(0, 5.0)});
}

TEST(LineStringSnapperTest, SegmentSnapProjectionFactorAtLeastOneClosedRing) {
  // Closing segment (to == too_far) with pf >= 1.0 syncs start vertex and
  // keeps the ring closed.
  auto closed_end = RunSnap(
      {Coordinate(0.9, 0.0), Coordinate(1.0, 0.0), Coordinate(1.0, 1.0),
       Coordinate(0.0, 1.0), Coordinate(0.9, 0.0)},
      {Coordinate(1.0, 0.0)}, 0.5, /*allow_snapping_to_source_vertices=*/true);
  ExpectCoordsNear(*closed_end, {Coordinate(1.0, 0.0), Coordinate(1.0, 0.0),
                                 Coordinate(1.0, 1.0), Coordinate(0.0, 1.0),
                                 Coordinate(0.9, 0.0), Coordinate(1.0, 0.0)});
}

TEST(LineStringSnapperTest, SegmentSnapProjectionFactorAtMostZeroOpenLine) {
  // First segment (segpos == begin) on an open line inserts old start point
  // after re-snapped start point.
  auto start_open = RunSnap({Coordinate(0, 0), Coordinate(10, 0)},
                            {Coordinate(-0.5, 0.1), Coordinate(0.5, 0.0)}, 2.0);
  ExpectCoordsNear(*start_open, {Coordinate(-0.5, 0.1), Coordinate(0.5, 0.0),
                                 Coordinate(10, 0)});

  // Interior segment with pf <= 0.0 where seg is closer to old start point
  // than prevSeg.
  auto mid_current_closer =
      RunSnap({Coordinate(0, 0), Coordinate(0.5, 0.0), Coordinate(10, 0)},
              {Coordinate(0, 0)}, 2.0,
              /*allow_snapping_to_source_vertices=*/true);
  ExpectCoordsNear(*mid_current_closer,
                   {Coordinate(0, 0), Coordinate(0, 0), Coordinate(0.5, 0.0),
                    Coordinate(10, 0)});

  // Exact pf == 0.0 boundary on first segment.
  auto exact_pf_zero = RunSnap({Coordinate(0, 0.05), Coordinate(10, 0.05),
                                Coordinate(0, 0), Coordinate(10, 0)},
                               {Coordinate(0, 0)}, 0.2,
                               /*allow_snapping_to_source_vertices=*/true);
  ExpectCoordsNear(*exact_pf_zero,
                   {Coordinate(0, 0), Coordinate(0, 0.05), Coordinate(10, 0.05),
                    Coordinate(0, 0), Coordinate(10, 0)});
}

TEST(LineStringSnapperTest, SegmentSnapProjectionFactorAtMostZeroClosedRing) {
  // First segment (segpos == begin) on a closed ring with pf <= 0.0 syncs end
  // vertex and inserts old start point into prevSeg when prevSeg is closer.
  auto closed_start =
      RunSnap({Coordinate(0, 0), Coordinate(5.0, 0.5), Coordinate(2.0, -0.2),
               Coordinate(0, 0)},
              {Coordinate(-0.5, 0.2), Coordinate(0.5, 0.0)}, 2.0);
  ExpectCoordsNear(*closed_start, {Coordinate(-0.5, 0.2), Coordinate(5.0, 0.5),
                                   Coordinate(2.0, -0.2), Coordinate(0.5, 0.0),
                                   Coordinate(-0.5, 0.2)});

  // First segment on a closed ring with pf <= 0.0 inserts old start point
  // into current segment when prevSeg is not closer.
  auto closed_start_current_closer =
      RunSnap({Coordinate(0, 0), Coordinate(2.0, -0.2), Coordinate(5.0, 0.5),
               Coordinate(0, 0)},
              {Coordinate(-0.5, 0.2), Coordinate(0.5, 0.0)}, 2.0);
  ExpectCoordsNear(
      *closed_start_current_closer,
      {Coordinate(-0.5, 0.2), Coordinate(0.5, 0.0), Coordinate(2.0, -0.2),
       Coordinate(5.0, 0.5), Coordinate(-0.5, 0.2)});
}

TEST(LineStringSnapperTest, SequentialSnapPointsOnClosedRing) {
  auto res = RunSnap({Coordinate(0, 0), Coordinate(10, 0), Coordinate(10, 10),
                      Coordinate(0, 0)},
                     {Coordinate(0.1, 0), Coordinate(0.2, 0)}, 0.25);
  ExpectCoordsNear(*res,
                   {Coordinate(0.1, 0), Coordinate(0.2, 0), Coordinate(10, 0),
                    Coordinate(10, 10), Coordinate(0.1, 0)});
}

TEST(LineStringSnapperTest, SnapperReuseAndZPreservation) {
  CoordinateSequence src(
      {Coordinate(0, 0), Coordinate(10, 0), Coordinate(0, 0.05)});
  Coordinate snap_pt(0, 0);
  Coordinate::ConstVect snap_ptrs = {&snap_pt};

  LineStringSnapper snapper(src, 0.2);
  auto first = snapper.snapTo(snap_ptrs);
  ExpectCoordsNear(*first,
                   {Coordinate(0, 0), Coordinate(10, 0), Coordinate(0, 0.05)});

  snapper.setAllowSnappingToSourceVertices(true);
  auto second = snapper.snapTo(snap_ptrs);
  ExpectCoordsNear(*second, {Coordinate(0, 0), Coordinate(10, 0),
                             Coordinate(0, 0), Coordinate(0, 0.05)});

  snapper.setAllowSnappingToSourceVertices(false);
  auto third = snapper.snapTo(snap_ptrs);
  ExpectCoordsNear(*third,
                   {Coordinate(0, 0), Coordinate(10, 0), Coordinate(0, 0.05)});

  // Source sequence remains unmodified.
  ExpectCoordsNear(src,
                   {Coordinate(0, 0), Coordinate(10, 0), Coordinate(0, 0.05)});

  // Z values from snap points are copied onto snapped/inserted vertices,
  // while 2D snap points leave Z as NaN.
  auto res_z = RunSnap({Coordinate(0, 0, 10), Coordinate(10, 0, 20)},
                       {Coordinate(0.05, 0, 99), Coordinate(5, 0.05, 77)}, 0.2);
  ExpectCoordsNear(
      *res_z, {Coordinate(0.05, 0), Coordinate(5, 0.05), Coordinate(10, 0)});
  EXPECT_DOUBLE_EQ((*res_z)[0].z, 99.0);
  EXPECT_DOUBLE_EQ((*res_z)[1].z, 77.0);
  EXPECT_DOUBLE_EQ((*res_z)[2].z, 20.0);

  auto res_2d_snap = RunSnap({Coordinate(0, 0, 10), Coordinate(10, 0, 20)},
                             {Coordinate(0.05, 0)}, 0.2);
  ExpectCoordsNear(*res_2d_snap, {Coordinate(0.05, 0), Coordinate(10, 0)});
  EXPECT_TRUE(std::isnan((*res_2d_snap)[0].z));
  EXPECT_DOUBLE_EQ((*res_2d_snap)[1].z, 20.0);
}

TEST(LineStringSnapperTest, InterruptAbortsSnapping) {
  absl::Cleanup cancel_guard([] { geos::util::Interrupt::cancel(); });
  geos::util::Interrupt::request();
  EXPECT_THROW(
      RunSnap({Coordinate(0, 0), Coordinate(10, 0)}, {Coordinate(5, 0.1)}, 1.0),
      geos::util::GEOSException);

  // Empty snap points still triggers interrupt check inside snapSegments.
  geos::util::Interrupt::request();
  EXPECT_THROW(RunSnap({Coordinate(0, 0), Coordinate(10, 0)}, {}, 1.0),
               geos::util::GEOSException);

  // Empty source sequence exits early before interrupt check.
  geos::util::Interrupt::request();
  auto empty_res = RunSnap({}, {Coordinate(1, 1)}, 1.0);
  EXPECT_TRUE(empty_res->isEmpty());
  geos::util::Interrupt::cancel();
}

TEST(LineStringSnapperTest, FindSnapForVertexCoversEqualClosestAndMissPaths) {
  CoordinateSequence src_seq({Coordinate(0.0, 0.0), Coordinate(10.0, 0.0)});
  LineStringSnapper snapper(src_seq, 1.0);
  auto find_fn = GetLineStringSnapperFn(FindSnapForVertexTag{});

  Coordinate far(5.0, 5.0);
  Coordinate near_loose(0.6, 0.0);
  Coordinate near_tight(0.25, 0.0);
  Coordinate exact(0.0, 0.0);

  Coordinate::ConstVect closest_ptrs{&far, &near_loose, &near_tight};
  auto it_closest = (snapper.*find_fn)(Coordinate(0.0, 0.0), closest_ptrs);
  ASSERT_TRUE(it_closest != closest_ptrs.cend());
  EXPECT_EQ(*it_closest, &near_tight);

  Coordinate::ConstVect exact_ptrs{&near_tight, &exact};
  auto it_exact = (snapper.*find_fn)(Coordinate(0.0, 0.0), exact_ptrs);
  EXPECT_TRUE(it_exact == exact_ptrs.cend());

  Coordinate::ConstVect miss_ptrs{&far};
  auto it_miss = (snapper.*find_fn)(Coordinate(0.0, 0.0), miss_ptrs);
  EXPECT_TRUE(it_miss == miss_ptrs.cend());
}

}  // namespace
}  // namespace geos_google
