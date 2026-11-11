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

#include "third_party/geos/include/geos/simplify/RingHullIndex.h"

#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/simplify/RingHull.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Envelope;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::simplify::RingHull;
using geos::simplify::RingHullIndex;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

std::unique_ptr<LinearRing> MakeBoxRing(const GeometryFactory& factory,
                                        double min_x, double max_x,
                                        double min_y, double max_y) {
  auto seq = std::make_unique<CoordinateSequence>();
  seq->reserve(5);
  seq->add(CoordinateXY(min_x, min_y));
  seq->add(CoordinateXY(min_x, max_y));
  seq->add(CoordinateXY(max_x, max_y));
  seq->add(CoordinateXY(max_x, min_y));
  seq->add(CoordinateXY(min_x, min_y));
  return factory.createLinearRing(std::move(seq));
}

TEST(RingHullIndexTest, DefaultStateAndEnabledToggle) {
  RingHullIndex index;

  EXPECT_TRUE(index.enabled());
  EXPECT_EQ(index.size(), 0u);
  EXPECT_THAT(index.query(Envelope(0.0, 10.0, 0.0, 10.0)), IsEmpty());

  index.enabled(false);
  EXPECT_FALSE(index.enabled());

  index.enabled(true);
  EXPECT_TRUE(index.enabled());
}

TEST(RingHullIndexTest, AddIncreasesSizeEvenWhenDisabled) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> ring1 = MakeBoxRing(*factory, 0.0, 4.0, 0.0, 4.0);
  std::unique_ptr<LinearRing> ring2 =
      MakeBoxRing(*factory, 10.0, 14.0, 0.0, 4.0);

  RingHull hull1(ring1.get(), true);
  RingHull hull2(ring2.get(), false);

  RingHullIndex index;
  index.enabled(false);
  index.add(&hull1);
  EXPECT_EQ(index.size(), 1u);

  index.enabled(true);
  index.add(&hull2);
  EXPECT_EQ(index.size(), 2u);

  EXPECT_THAT(index.query(Envelope(1.0, 2.0, 1.0, 2.0)), ElementsAre(&hull1));
  EXPECT_THAT(index.query(Envelope(11.0, 12.0, 1.0, 2.0)), ElementsAre(&hull2));
}

TEST(RingHullIndexTest, QueryFiltersByEnvelopeIntersection) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  std::unique_ptr<LinearRing> ring_a =
      MakeBoxRing(*factory, 0.0, 10.0, 0.0, 10.0);
  std::unique_ptr<LinearRing> ring_b =
      MakeBoxRing(*factory, 20.0, 30.0, 0.0, 10.0);
  std::unique_ptr<LinearRing> ring_c =
      MakeBoxRing(*factory, 5.0, 25.0, 5.0, 15.0);

  RingHull hull_a(ring_a.get(), true);
  RingHull hull_b(ring_b.get(), true);
  RingHull hull_c(ring_c.get(), true);

  RingHullIndex index;
  index.add(&hull_a);
  index.add(&hull_b);
  index.add(&hull_c);
  EXPECT_EQ(index.size(), 3u);

  // Null and disjoint envelopes match nothing.
  EXPECT_THAT(index.query(Envelope()), IsEmpty());
  EXPECT_THAT(index.query(Envelope(100.0, 110.0, 100.0, 110.0)), IsEmpty());

  // Interior queries matching single, pair, or all hulls.
  EXPECT_THAT(index.query(Envelope(1.0, 2.0, 1.0, 2.0)), ElementsAre(&hull_a));
  EXPECT_THAT(index.query(Envelope(22.0, 24.0, 1.0, 2.0)),
              ElementsAre(&hull_b));
  EXPECT_THAT(index.query(Envelope(7.0, 8.0, 7.0, 8.0)),
              ElementsAre(&hull_a, &hull_c));
  EXPECT_THAT(index.query(Envelope(0.0, 30.0, 0.0, 15.0)),
              ElementsAre(&hull_a, &hull_b, &hull_c));

  // Boundary-touching edge and single-point envelope queries.
  EXPECT_THAT(index.query(Envelope(10.0, 15.0, 0.0, 4.0)),
              ElementsAre(&hull_a));
  EXPECT_THAT(index.query(Envelope(CoordinateXY(20.0, 10.0))),
              ElementsAre(&hull_b, &hull_c));
}

}  // namespace
}  // namespace geos_google
