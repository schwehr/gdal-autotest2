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

#include "third_party/geos/include/geos/geom/CoordinateSequenceFilter.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/geom/Polygon.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateSequenceFilter;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYM;
using ::geos::geom::CoordinateXYZM;
using ::geos::geom::Envelope;
using ::geos::geom::Geometry;
using ::geos::geom::GeometryFactory;
using ::geos::geom::LinearRing;

struct CoordinateVisit {
  const CoordinateSequence* seq;
  std::size_t index;
  std::size_t seq_size;
  bool has_z;
  bool has_m;
  double x;
  double y;
  double z;
  double m;
};

// Read-only filter that intentionally leaves filter_rw() unimplemented.
class RecordingReadOnlyFilter final : public CoordinateSequenceFilter {
 public:
  explicit RecordingReadOnlyFilter(
      std::size_t max_visits = std::numeric_limits<std::size_t>::max())
      : max_visits_(max_visits) {}

  void filter_ro(const CoordinateSequence& seq, std::size_t i) override {
    CoordinateXYZM coord;
    seq.getAt(i, coord);
    visits_.push_back(CoordinateVisit{
        .seq = &seq,
        .index = i,
        .seq_size = seq.size(),
        .has_z = seq.hasZ(),
        .has_m = seq.hasM(),
        .x = coord.x,
        .y = coord.y,
        .z = seq.getOrdinate(i, CoordinateSequence::Z),
        .m = seq.getOrdinate(i, CoordinateSequence::M),
    });
  }

  bool isDone() const override { return visits_.size() >= max_visits_; }

  bool isGeometryChanged() const override { return false; }

  const std::vector<CoordinateVisit>& visits() const { return visits_; }

 private:
  std::size_t max_visits_;
  std::vector<CoordinateVisit> visits_;
};

// Read-write filter that intentionally leaves filter_ro() unimplemented.
class TranslateWriteFilter final : public CoordinateSequenceFilter {
 public:
  TranslateWriteFilter(
      double dx, double dy, bool report_changed,
      std::size_t max_visits = std::numeric_limits<std::size_t>::max())
      : dx_(dx),
        dy_(dy),
        report_changed_(report_changed),
        max_visits_(max_visits) {}

  void filter_rw(CoordinateSequence& seq, std::size_t i) override {
    auto& coord = seq.getAt<CoordinateXY>(i);
    coord.x += dx_;
    coord.y += dy_;
    ++visit_count_;
  }

  bool isDone() const override { return visit_count_ >= max_visits_; }

  bool isGeometryChanged() const override { return report_changed_; }

  std::size_t visit_count() const { return visit_count_; }

 private:
  double dx_;
  double dy_;
  bool report_changed_;
  std::size_t max_visits_;
  std::size_t visit_count_ = 0;
};

TEST(CoordinateSequenceFilterTest, PolymorphicDestruction) {
  std::unique_ptr<CoordinateSequenceFilter> ro =
      std::make_unique<RecordingReadOnlyFilter>();
  std::unique_ptr<CoordinateSequenceFilter> rw =
      std::make_unique<TranslateWriteFilter>(1.0, 2.0, true);

  EXPECT_FALSE(ro->isDone());
  EXPECT_FALSE(ro->isGeometryChanged());
  EXPECT_FALSE(rw->isDone());
  EXPECT_TRUE(rw->isGeometryChanged());

  ro.reset();
  rw.reset();
}

TEST(CoordinateSequenceFilterTest, ReadOnlyFilterVisitsAllLineStringVertices) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto line = factory->createLineString(CoordinateSequence{
      Coordinate(1.0, 2.0, 10.0),
      Coordinate(3.0, 4.0, 20.0),
      Coordinate(5.0, 6.0, 30.0),
  });

  RecordingReadOnlyFilter filter;
  line->apply_ro(filter);

  ASSERT_EQ(filter.visits().size(), 3u);
  for (std::size_t i = 0; i < filter.visits().size(); ++i) {
    const auto& visit = filter.visits()[i];
    EXPECT_EQ(visit.seq, line->getCoordinatesRO());
    EXPECT_EQ(visit.index, i);
    EXPECT_EQ(visit.seq_size, 3u);
    EXPECT_TRUE(visit.has_z);
    EXPECT_FALSE(visit.has_m);
    EXPECT_TRUE(std::isnan(visit.m));
  }
  EXPECT_DOUBLE_EQ(filter.visits()[0].x, 1.0);
  EXPECT_DOUBLE_EQ(filter.visits()[0].y, 2.0);
  EXPECT_DOUBLE_EQ(filter.visits()[0].z, 10.0);
  EXPECT_DOUBLE_EQ(filter.visits()[1].x, 3.0);
  EXPECT_DOUBLE_EQ(filter.visits()[1].y, 4.0);
  EXPECT_DOUBLE_EQ(filter.visits()[1].z, 20.0);
  EXPECT_DOUBLE_EQ(filter.visits()[2].x, 5.0);
  EXPECT_DOUBLE_EQ(filter.visits()[2].y, 6.0);
  EXPECT_DOUBLE_EQ(filter.visits()[2].z, 30.0);
  EXPECT_FALSE(filter.isDone());
}

TEST(CoordinateSequenceFilterTest, ReadOnlyFilterHandlesEmptyGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::vector<std::unique_ptr<Geometry>> empty_geoms;
  empty_geoms.push_back(factory->createPoint());
  empty_geoms.push_back(factory->createLineString());
  empty_geoms.push_back(factory->createPolygon());
  empty_geoms.push_back(factory->createGeometryCollection());

  RecordingReadOnlyFilter filter;
  for (const auto& geom : empty_geoms) {
    geom->apply_ro(filter);
  }
  EXPECT_TRUE(filter.visits().empty());
}

TEST(CoordinateSequenceFilterTest, ReadOnlyFilterReadsZAndMPoints) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto pt_xyzm = factory->createPoint(CoordinateXYZM(1.5, 2.5, 3.5, 4.5));
  auto pt_xym = factory->createPoint(CoordinateXYM(7.0, 8.0, 9.0));

  RecordingReadOnlyFilter filter;
  pt_xyzm->apply_ro(filter);
  pt_xym->apply_ro(filter);

  ASSERT_EQ(filter.visits().size(), 2u);
  EXPECT_TRUE(filter.visits()[0].has_z);
  EXPECT_TRUE(filter.visits()[0].has_m);
  EXPECT_DOUBLE_EQ(filter.visits()[0].x, 1.5);
  EXPECT_DOUBLE_EQ(filter.visits()[0].y, 2.5);
  EXPECT_DOUBLE_EQ(filter.visits()[0].z, 3.5);
  EXPECT_DOUBLE_EQ(filter.visits()[0].m, 4.5);

  EXPECT_FALSE(filter.visits()[1].has_z);
  EXPECT_TRUE(filter.visits()[1].has_m);
  EXPECT_DOUBLE_EQ(filter.visits()[1].x, 7.0);
  EXPECT_DOUBLE_EQ(filter.visits()[1].y, 8.0);
  EXPECT_TRUE(std::isnan(filter.visits()[1].z));
  EXPECT_DOUBLE_EQ(filter.visits()[1].m, 9.0);
}

TEST(CoordinateSequenceFilterTest, ReadOnlyFilterShortCircuitsWithIsDone) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto line = factory->createLineString(CoordinateSequence{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 1.0),
      CoordinateXY(2.0, 2.0),
      CoordinateXY(3.0, 3.0),
  });

  RecordingReadOnlyFilter line_filter(/*max_visits=*/2);
  line->apply_ro(line_filter);
  EXPECT_TRUE(line_filter.isDone());
  ASSERT_EQ(line_filter.visits().size(), 2u);
  EXPECT_EQ(line_filter.visits()[0].index, 0u);
  EXPECT_EQ(line_filter.visits()[1].index, 1u);

  auto shell = factory->createLinearRing(CoordinateSequence{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(10.0, 10.0),
      CoordinateXY(0.0, 10.0),
      CoordinateXY(0.0, 0.0),
  });
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory->createLinearRing(CoordinateSequence{
      CoordinateXY(2.0, 2.0),
      CoordinateXY(4.0, 2.0),
      CoordinateXY(4.0, 4.0),
      CoordinateXY(2.0, 4.0),
      CoordinateXY(2.0, 2.0),
  }));
  auto polygon = factory->createPolygon(std::move(shell), std::move(holes));

  RecordingReadOnlyFilter poly_shell_stop(/*max_visits=*/3);
  polygon->apply_ro(poly_shell_stop);
  EXPECT_TRUE(poly_shell_stop.isDone());
  ASSERT_EQ(poly_shell_stop.visits().size(), 3u);
  EXPECT_EQ(poly_shell_stop.visits()[2].seq,
            polygon->getExteriorRing()->getCoordinatesRO());

  RecordingReadOnlyFilter poly_hole_stop(/*max_visits=*/7);
  polygon->apply_ro(poly_hole_stop);
  EXPECT_TRUE(poly_hole_stop.isDone());
  ASSERT_EQ(poly_hole_stop.visits().size(), 7u);
  EXPECT_EQ(poly_hole_stop.visits()[5].seq,
            polygon->getInteriorRingN(0)->getCoordinatesRO());
  EXPECT_EQ(poly_hole_stop.visits()[5].index, 0u);
  EXPECT_EQ(poly_hole_stop.visits()[6].index, 1u);

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(factory->createPoint(CoordinateXY(5.0, 6.0)));
  geoms.push_back(line->clone());
  auto collection = factory->createGeometryCollection(std::move(geoms));

  RecordingReadOnlyFilter collection_filter(/*max_visits=*/1);
  collection->apply_ro(collection_filter);
  EXPECT_TRUE(collection_filter.isDone());
  ASSERT_EQ(collection_filter.visits().size(), 1u);
  EXPECT_DOUBLE_EQ(collection_filter.visits()[0].x, 5.0);
  EXPECT_DOUBLE_EQ(collection_filter.visits()[0].y, 6.0);
}

TEST(CoordinateSequenceFilterTest, ReadWriteFilterMutatesAndUpdatesEnvelopes) {
  GeometryFactory::Ptr factory = GeometryFactory::create();

  auto point = factory->createPoint(CoordinateXY(1.0, 2.0));
  TranslateWriteFilter point_filter(10.0, -5.0, /*report_changed=*/true);
  point->apply_rw(point_filter);
  EXPECT_EQ(point_filter.visit_count(), 1u);
  EXPECT_DOUBLE_EQ(point->getX(), 11.0);
  EXPECT_DOUBLE_EQ(point->getY(), -3.0);
  EXPECT_DOUBLE_EQ(point->getEnvelopeInternal()->getMinX(), 11.0);
  EXPECT_DOUBLE_EQ(point->getEnvelopeInternal()->getMaxY(), -3.0);

  auto shell = factory->createLinearRing(CoordinateSequence{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(10.0, 10.0),
      CoordinateXY(0.0, 10.0),
      CoordinateXY(0.0, 0.0),
  });
  std::vector<std::unique_ptr<LinearRing>> holes;
  holes.push_back(factory->createLinearRing(CoordinateSequence{
      CoordinateXY(2.0, 2.0),
      CoordinateXY(4.0, 2.0),
      CoordinateXY(4.0, 4.0),
      CoordinateXY(2.0, 4.0),
      CoordinateXY(2.0, 2.0),
  }));
  auto polygon = factory->createPolygon(std::move(shell), std::move(holes));
  TranslateWriteFilter poly_filter(1.0, 2.0, /*report_changed=*/true);
  polygon->apply_rw(poly_filter);
  EXPECT_EQ(poly_filter.visit_count(), 10u);
  EXPECT_DOUBLE_EQ(polygon->getEnvelopeInternal()->getMinX(), 1.0);
  EXPECT_DOUBLE_EQ(polygon->getEnvelopeInternal()->getMaxX(), 11.0);
  EXPECT_DOUBLE_EQ(polygon->getEnvelopeInternal()->getMinY(), 2.0);
  EXPECT_DOUBLE_EQ(polygon->getEnvelopeInternal()->getMaxY(), 12.0);
  EXPECT_DOUBLE_EQ(polygon->getInteriorRingN(0)->getCoordinateN(1).x, 5.0);
  EXPECT_DOUBLE_EQ(polygon->getInteriorRingN(0)->getCoordinateN(1).y, 4.0);

  std::vector<std::unique_ptr<Geometry>> geoms;
  geoms.push_back(factory->createPoint(CoordinateXY(1.0, 1.0)));
  geoms.push_back(factory->createLineString(CoordinateSequence{
      CoordinateXY(2.0, 2.0),
      CoordinateXY(3.0, 4.0),
  }));
  auto collection = factory->createGeometryCollection(std::move(geoms));
  TranslateWriteFilter gc_filter(10.0, 20.0, /*report_changed=*/true);
  collection->apply_rw(gc_filter);
  EXPECT_EQ(gc_filter.visit_count(), 3u);
  const Envelope* gc_env = collection->getEnvelopeInternal();
  EXPECT_DOUBLE_EQ(gc_env->getMinX(), 11.0);
  EXPECT_DOUBLE_EQ(gc_env->getMaxX(), 13.0);
  EXPECT_DOUBLE_EQ(gc_env->getMinY(), 21.0);
  EXPECT_DOUBLE_EQ(gc_env->getMaxY(), 24.0);
}

TEST(CoordinateSequenceFilterTest,
     ReadWriteFilterShortCircuitsAndRespectsIsGeometryChangedFalse) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  auto line = factory->createLineString(CoordinateSequence{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 1.0),
      CoordinateXY(2.0, 2.0),
      CoordinateXY(3.0, 3.0),
  });

  TranslateWriteFilter partial_filter(5.0, 5.0, /*report_changed=*/true,
                                      /*max_visits=*/2);
  line->apply_rw(partial_filter);
  EXPECT_TRUE(partial_filter.isDone());
  EXPECT_EQ(partial_filter.visit_count(), 2u);
  EXPECT_DOUBLE_EQ(line->getCoordinateN(0).x, 5.0);
  EXPECT_DOUBLE_EQ(line->getCoordinateN(1).x, 6.0);
  EXPECT_DOUBLE_EQ(line->getCoordinateN(2).x, 2.0);
  EXPECT_DOUBLE_EQ(line->getCoordinateN(3).x, 3.0);

  auto line2 = factory->createLineString(CoordinateSequence{
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 1.0),
  });
  ASSERT_DOUBLE_EQ(line2->getEnvelopeInternal()->getMaxX(), 1.0);

  TranslateWriteFilter unnotified_filter(100.0, 100.0,
                                         /*report_changed=*/false);
  line2->apply_rw(unnotified_filter);
  EXPECT_DOUBLE_EQ(line2->getCoordinateN(0).x, 100.0);
  EXPECT_DOUBLE_EQ(line2->getCoordinateN(1).x, 101.0);
  // Cached envelope remains stale because isGeometryChanged() returned false.
  EXPECT_DOUBLE_EQ(line2->getEnvelopeInternal()->getMaxX(), 1.0);
}

TEST(CoordinateSequenceFilterDeathTest, DefaultUnimplementedDirectionAborts) {
  CoordinateSequence seq{Coordinate(1.0, 2.0, 3.0)};

  RecordingReadOnlyFilter ro_filter;
  CoordinateSequenceFilter& base_ro = ro_filter;
  EXPECT_DEBUG_DEATH(base_ro.filter_rw(seq, 0), "");

  TranslateWriteFilter rw_filter(1.0, 1.0, /*report_changed=*/true);
  CoordinateSequenceFilter& base_rw = rw_filter;
  EXPECT_DEBUG_DEATH(base_rw.filter_ro(seq, 0), "");
}

}  // namespace
}  // namespace geos_google
