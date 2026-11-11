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

// Unit tests for
// third_party/geos/include/geos/operation/cluster/DisjointOperation.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for exception propagation tests.
// - Uses isolated GeometryFactory::create() instances per test.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/cluster/DisjointOperation.h

#include "third_party/geos/include/geos/operation/cluster/DisjointOperation.h"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryCollection.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Polygon.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/cluster/EnvelopeDistanceClusterFinder.h"
#include "third_party/geos/include/geos/operation/cluster/EnvelopeIntersectsClusterFinder.h"
#include "third_party/geos/include/geos/operation/cluster/GeometryIntersectsClusterFinder.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Polygon;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::operation::cluster::DisjointOperation;
using geos::operation::cluster::EnvelopeDistanceClusterFinder;
using geos::operation::cluster::EnvelopeIntersectsClusterFinder;
using geos::operation::cluster::GeometryIntersectsClusterFinder;
using ::testing::UnorderedElementsAre;

// Cluster finder wrapper counting spatial queries so tests can observe when
// DisjointOperation delegates to the finder versus taking the fast path.
class CountingIntersectsFinder final : public EnvelopeIntersectsClusterFinder {
 public:
  std::size_t query_count() const { return query_count_; }

  void reset_count() { query_count_ = 0; }

 protected:
  const Envelope& queryEnvelope(const Geometry* a) override {
    ++query_count_;
    return EnvelopeIntersectsClusterFinder::queryEnvelope(a);
  }

 private:
  std::size_t query_count_ = 0;
};

TEST(DisjointOperationTest,
     SingleGeometryFastPathSkipsClusteringAndFlattening) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  constexpr auto kSingleGeomWkts = std::to_array<std::string_view>({
      "POINT (1 2)",
      "POINT EMPTY",
      "LINESTRING (0 0, 2 2)",
      "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))",
      "MULTIPOINT ((1 2))",
      "GEOMETRYCOLLECTION (POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0)))",
  });

  CountingIntersectsFinder finder;
  DisjointOperation op(finder);

  for (bool split_inputs : {false, true}) {
    op.setSplitInputs(split_inputs);
    for (std::string_view wkt : kSingleGeomWkts) {
      SCOPED_TRACE(wkt);
      finder.reset_count();
      Geometry::Ptr input = reader.read(std::string(wkt));

      std::size_t calls = 0;
      const Geometry* seen_arg = nullptr;
      Geometry::Ptr result =
          op.processDisjointSubsets(*input, [&](const Geometry& subset) {
            ++calls;
            seen_arg = &subset;
            return subset.clone();
          });

      EXPECT_EQ(finder.query_count(), 0);
      EXPECT_EQ(calls, 1);
      EXPECT_EQ(seen_arg, input.get());
      ASSERT_NE(result, nullptr);
      EXPECT_TRUE(result->equalsIdentical(input.get()));
    }
  }

  // A 1-element GeometryCollection containing a MultiPolygon still has
  // getNumGeometries() == 1, so it returns f(g) verbatim without splitting
  // or flattening even when setSplitInputs(true) is active.
  finder.reset_count();
  op.setSplitInputs(true);
  Geometry::Ptr nested_single = reader.read(
      "GEOMETRYCOLLECTION (MULTIPOLYGON ("
      "((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "((10 0, 11 0, 11 1, 10 1, 10 0))))");
  Geometry::Ptr result = op.processDisjointSubsets(
      *nested_single, [](const Geometry& subset) { return subset.clone(); });

  EXPECT_EQ(finder.query_count(), 0);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(result->getNumGeometries(), 1);
  EXPECT_TRUE(result->equalsIdentical(nested_single.get()));

  // Fast path returns f(g) directly without invoking
  // GeometryFlattener::flatten.
  Geometry::Ptr single_pt = reader.read("POINT (5 6)");
  Geometry::Ptr unflattened = op.processDisjointSubsets(
      *single_pt, [&](const Geometry& subset) -> Geometry::Ptr {
        std::vector<Geometry::Ptr> items;
        items.push_back(subset.clone());
        return factory->createGeometryCollection(std::move(items));
      });
  ASSERT_NE(unflattened, nullptr);
  EXPECT_EQ(unflattened->getGeometryTypeId(),
            geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(unflattened->getNumGeometries(), 1);
}

TEST(DisjointOperationTest,
     EmptyCollectionsSkipCallbackAndPreserveFactoryAndSrid) {
  PrecisionModel pm(100.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);
  WKTReader reader(*factory);

  constexpr auto kEmptyCollectionWkts = std::to_array<std::string_view>({
      "GEOMETRYCOLLECTION EMPTY",
      "MULTIPOINT EMPTY",
      "MULTILINESTRING EMPTY",
      "MULTIPOLYGON EMPTY",
  });

  CountingIntersectsFinder finder;
  DisjointOperation op(finder);

  for (bool split_inputs : {false, true}) {
    op.setSplitInputs(split_inputs);
    for (std::string_view wkt : kEmptyCollectionWkts) {
      SCOPED_TRACE(wkt);
      finder.reset_count();
      Geometry::Ptr input = reader.read(std::string(wkt));

      std::size_t calls = 0;
      Geometry::Ptr result =
          op.processDisjointSubsets(*input, [&](const Geometry& subset) {
            ++calls;
            return subset.clone();
          });

      EXPECT_EQ(calls, 0);
      EXPECT_EQ(finder.query_count(), 0);
      ASSERT_NE(result, nullptr);
      EXPECT_TRUE(result->isEmpty());
      EXPECT_EQ(result->getGeometryTypeId(),
                geos::geom::GEOS_GEOMETRYCOLLECTION);
      EXPECT_EQ(result->getNumGeometries(), 0);
      EXPECT_EQ(result->getFactory(), factory.get());
      EXPECT_EQ(result->getSRID(), 4326);
      EXPECT_DOUBLE_EQ(result->getPrecisionModel()->getScale(), 100.0);
    }
  }
}

TEST(DisjointOperationTest, CollectionOfEmptyComponentsProcessesEachIsolated) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);
  Geometry::Ptr input =
      reader.read("GEOMETRYCOLLECTION (POINT EMPTY, POLYGON EMPTY)");

  EnvelopeIntersectsClusterFinder finder;
  DisjointOperation op(finder);

  std::size_t calls = 0;
  Geometry::Ptr result =
      op.processDisjointSubsets(*input, [&](const Geometry& subset) {
        ++calls;
        return subset.clone();
      });

  EXPECT_EQ(calls, 2);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->isEmpty());
  EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
  EXPECT_EQ(result->getNumGeometries(), 2);
}

TEST(DisjointOperationTest, SetSplitInputsControlsDecompositionAndTogglesBack) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Two MultiPolygons whose first polygons overlap ([0,2]x[0,2] and
  // [1,3]x[1,3]), while their second polygons are distant ([10,12] and
  // [20,22]). Without splitting, both MultiPolygons merge into 1 cluster;
  // with splitting, they partition into 3 disjoint subsets.
  Geometry::Ptr input = reader.read(
      "GEOMETRYCOLLECTION ("
      "MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), "
      "((10 0, 12 0, 12 2, 10 2, 10 0))), "
      "MULTIPOLYGON (((1 1, 3 1, 3 3, 1 3, 1 1)), "
      "((20 0, 22 0, 22 2, 20 2, 20 0))))");
  Geometry::Ptr input_snapshot = input->clone();
  Geometry::Ptr expected_union = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 2 0, 2 1, 3 1, 3 3, 1 3, 1 2, 0 2, 0 0)), "
      "((10 0, 12 0, 12 2, 10 2, 10 0)), "
      "((20 0, 22 0, 22 2, 20 2, 20 0)))");
  expected_union->normalize();

  EnvelopeIntersectsClusterFinder finder;
  DisjointOperation op(finder);

  // Default is split_inputs == false.
  std::size_t unsplit_calls = 0;
  Geometry::Ptr unsplit_result =
      op.processDisjointSubsets(*input, [&](const Geometry& subset) {
        ++unsplit_calls;
        return subset.Union();
      });
  EXPECT_EQ(unsplit_calls, 1);
  ASSERT_NE(unsplit_result, nullptr);
  EXPECT_EQ(unsplit_result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_DOUBLE_EQ(unsplit_result->getArea(), expected_union->getArea());
  EXPECT_TRUE(unsplit_result->equals(expected_union.get()));
  unsplit_result->normalize();
  EXPECT_TRUE(unsplit_result->equalsExact(expected_union.get(), 1e-9));

  // Enable input splitting: 4 flattened polygons cluster into 3 subsets.
  op.setSplitInputs(true);
  std::vector<std::size_t> split_subset_counts;
  Geometry::Ptr split_result =
      op.processDisjointSubsets(*input, [&](const Geometry& subset) {
        split_subset_counts.push_back(subset.getNumGeometries());
        return subset.Union();
      });
  EXPECT_THAT(split_subset_counts, UnorderedElementsAre(2, 1, 1));
  ASSERT_NE(split_result, nullptr);
  EXPECT_EQ(split_result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_DOUBLE_EQ(split_result->getArea(), expected_union->getArea());
  EXPECT_TRUE(split_result->equals(expected_union.get()));
  split_result->normalize();
  EXPECT_TRUE(split_result->equalsExact(expected_union.get(), 1e-9));

  // Toggle split_inputs back to false on the same DisjointOperation instance.
  op.setSplitInputs(false);
  std::size_t toggled_back_calls = 0;
  Geometry::Ptr toggled_result =
      op.processDisjointSubsets(*input, [&](const Geometry& subset) {
        ++toggled_back_calls;
        return subset.Union();
      });
  EXPECT_EQ(toggled_back_calls, 1);
  ASSERT_NE(toggled_result, nullptr);
  EXPECT_EQ(toggled_result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_DOUBLE_EQ(toggled_result->getArea(), expected_union->getArea());
  EXPECT_TRUE(toggled_result->equals(expected_union.get()));
  toggled_result->normalize();
  EXPECT_TRUE(toggled_result->equalsExact(expected_union.get(), 1e-9));

  // Input geometry must remain unmodified.
  EXPECT_TRUE(input->equalsIdentical(input_snapshot.get()));
}

TEST(DisjointOperationTest, FlattensAndNarrowsCallbackResults) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Case 1: Overlapping polygons cluster into 1 subset whose union is a single
  // Polygon; GeometryFlattener narrows GeometryCollection(Polygon) to Polygon.
  // Use topological equals() because OverlayNG union preserves collinear
  // noding vertices along shared edges at x=1 and x=2.
  {
    Geometry::Ptr overlapping = reader.read(
        "MULTIPOLYGON ("
        "((0 0, 2 0, 2 2, 0 2, 0 0)), "
        "((1 0, 3 0, 3 2, 1 2, 1 0)))");
    Geometry::Ptr expected = reader.read("POLYGON ((0 0, 3 0, 3 2, 0 2, 0 0))");

    EnvelopeIntersectsClusterFinder finder;
    DisjointOperation op(finder);
    Geometry::Ptr result = op.processDisjointSubsets(
        *overlapping, [](const Geometry& subset) { return subset.Union(); });

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_POLYGON);
    EXPECT_TRUE(result->isValid());
    EXPECT_DOUBLE_EQ(result->getArea(), 6.0);
    EXPECT_TRUE(result->equals(expected.get()));
  }

  // Case 2: EnvelopeDistanceClusterFinder(2.0) groups two nearby disjoint
  // boxes ([0,1]x[0,1] and [2,3]x[0,1]) into one subset whose Union() returns
  // a MultiPolygon, while a distant box ([10,11]x[10,11]) yields a Polygon.
  // DisjointOperation flattens the combined results into a single MultiPolygon.
  {
    Geometry::Ptr boxes = reader.read(
        "MULTIPOLYGON ("
        "((0 0, 1 0, 1 1, 0 1, 0 0)), "
        "((2 0, 3 0, 3 1, 2 1, 2 0)), "
        "((10 10, 11 10, 11 11, 10 11, 10 10)))");
    Geometry::Ptr expected = boxes->clone();

    EnvelopeDistanceClusterFinder finder(2.0);
    DisjointOperation op(finder);
    std::size_t calls = 0;
    Geometry::Ptr result =
        op.processDisjointSubsets(*boxes, [&](const Geometry& subset) {
          ++calls;
          return subset.Union();
        });

    EXPECT_EQ(calls, 2);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
    EXPECT_EQ(result->getNumGeometries(), 3);
    result->normalize();
    expected->normalize();
    EXPECT_TRUE(result->equalsExact(expected.get(), 1e-9));
  }

  // Case 3: Heterogeneous outputs remain a flat GeometryCollection.
  {
    Geometry::Ptr mixed = reader.read(
        "GEOMETRYCOLLECTION ("
        "POINT (0 0), "
        "LINESTRING (10 0, 11 0), "
        "POLYGON ((20 0, 21 0, 21 1, 20 1, 20 0)))");
    EnvelopeIntersectsClusterFinder finder;
    DisjointOperation op(finder);
    Geometry::Ptr result = op.processDisjointSubsets(
        *mixed, [](const Geometry& subset) { return subset.clone(); });

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_GEOMETRYCOLLECTION);
    EXPECT_EQ(result->getNumGeometries(), 3);
    for (std::size_t i = 0; i < result->getNumGeometries(); ++i) {
      EXPECT_FALSE(result->getGeometryN(i)->isCollection());
    }
    result->normalize();
    mixed->normalize();
    EXPECT_TRUE(result->equalsExact(mixed.get(), 1e-9));
  }

  // Case 4: Nested GeometryCollection outputs from callbacks are flattened and
  // narrowed into a homogeneous MultiPoint.
  {
    Geometry::Ptr points = reader.read("MULTIPOINT ((0 0), (10 10))");
    EnvelopeIntersectsClusterFinder finder;
    DisjointOperation op(finder);
    Geometry::Ptr result = op.processDisjointSubsets(
        *points, [&](const Geometry& subset) -> Geometry::Ptr {
          std::vector<Geometry::Ptr> wrapped;
          wrapped.push_back(subset.clone());
          return factory->createGeometryCollection(std::move(wrapped));
        });

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOINT);
    EXPECT_EQ(result->getNumGeometries(), 2);
    result->normalize();
    points->normalize();
    EXPECT_TRUE(result->equalsIdentical(points.get()));
  }
}

TEST(DisjointOperationTest, DelegatesToGeometryIntersectsClusterFinder) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Triangle 1 (x+y <= 10) and Triangle 2 (x+y >= 12) have intersecting
  // bounding envelopes ([0,10]x[0,10] and [2,10]x[2,10]) but are disjoint,
  // whereas Square 3 ([0,1]x[0,1]) lies inside Triangle 1.
  Geometry::Ptr polygons = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 10 0, 0 10, 0 0)), "
      "((10 10, 10 2, 2 10, 10 10)), "
      "((0 0, 1 0, 1 1, 0 1, 0 0)))");

  // Single-use GeometryIntersectsClusterFinder distinguishes disjoint geometry
  // from intersecting bounding box.
  GeometryIntersectsClusterFinder geom_finder;
  DisjointOperation geom_op(geom_finder);
  std::vector<std::size_t> geom_subset_counts;
  Geometry::Ptr geom_result =
      geom_op.processDisjointSubsets(*polygons, [&](const Geometry& subset) {
        geom_subset_counts.push_back(subset.getNumGeometries());
        return subset.clone();
      });
  EXPECT_THAT(geom_subset_counts, UnorderedElementsAre(2, 1));
  ASSERT_NE(geom_result, nullptr);
  EXPECT_EQ(geom_result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(geom_result->getNumGeometries(), 3);

  // By contrast, EnvelopeIntersectsClusterFinder merges all three into 1 set.
  EnvelopeIntersectsClusterFinder env_finder;
  DisjointOperation env_op(env_finder);
  std::size_t env_calls = 0;
  Geometry::Ptr env_result =
      env_op.processDisjointSubsets(*polygons, [&](const Geometry& subset) {
        ++env_calls;
        return subset.clone();
      });
  EXPECT_EQ(env_calls, 1);
  ASSERT_NE(env_result, nullptr);
  EXPECT_EQ(env_result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(env_result->getNumGeometries(), 3);
}

TEST(DisjointOperationTest,
     ForwardsCallableVariantsPreservesZmAndPropagatesExceptions) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(*factory);

  // Lvalue std::function callable returning std::unique_ptr<Polygon> on a
  // multi-component input so the loop body `subset = f(*subset)` executes.
  Geometry::Ptr two_boxes = reader.read(
      "MULTIPOLYGON ("
      "((0 0, 1 0, 1 1, 0 1, 0 0)), "
      "((10 10, 11 10, 11 11, 10 11, 10 10)))");
  EnvelopeIntersectsClusterFinder finder;
  DisjointOperation op(finder);

  std::function<std::unique_ptr<Polygon>(const Geometry&)> box_hull_fn =
      [&](const Geometry& subset) -> std::unique_ptr<Polygon> {
    const Envelope* env = subset.getEnvelopeInternal();
    std::unique_ptr<Geometry> env_geom = factory->toGeometry(env);
    auto* poly = dynamic_cast<Polygon*>(env_geom.get());
    EXPECT_NE(poly, nullptr);
    if (poly == nullptr) {
      return factory->createPolygon();
    }
    env_geom.release();
    return std::unique_ptr<Polygon>(poly);
  };
  Geometry::Ptr lvalue_result =
      op.processDisjointSubsets(*two_boxes, box_hull_fn);
  ASSERT_NE(lvalue_result, nullptr);
  EXPECT_EQ(lvalue_result->getGeometryTypeId(), geos::geom::GEOS_MULTIPOLYGON);
  EXPECT_EQ(lvalue_result->getNumGeometries(), 2);

  // Preserves Z and M coordinates through clustering and output flattening.
  Geometry::Ptr mls_zm = reader.read(
      "MULTILINESTRING ZM ("
      "(0 0 1 10, 2 2 2 20), "
      "(0 1 3 30, 2 3 4 40), "
      "(10 10 5 50, 11 11 6 60))");
  Geometry::Ptr zm_result = op.processDisjointSubsets(
      *mls_zm, [](const Geometry& subset) { return subset.clone(); });
  ASSERT_NE(zm_result, nullptr);
  EXPECT_EQ(zm_result->getGeometryTypeId(), geos::geom::GEOS_MULTILINESTRING);
  EXPECT_TRUE(zm_result->hasZ());
  EXPECT_TRUE(zm_result->hasM());
  zm_result->normalize();
  mls_zm->normalize();
  EXPECT_TRUE(zm_result->equalsIdentical(mls_zm.get()));

  // Exceptions thrown by the callback propagate cleanly without leaking.
  std::size_t calls_before_throw = 0;
  EXPECT_THROW(op.processDisjointSubsets(
                   *two_boxes,
                   [&](const Geometry& subset) -> std::unique_ptr<Geometry> {
                     ++calls_before_throw;
                     if (calls_before_throw == 2) {
                       throw std::runtime_error("simulated callback failure");
                     }
                     return subset.clone();
                   }),
               std::runtime_error);
  EXPECT_EQ(calls_before_throw, 2);
}

}  // namespace
}  // namespace geos_google
