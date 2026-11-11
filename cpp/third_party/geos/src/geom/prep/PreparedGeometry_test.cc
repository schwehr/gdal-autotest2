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

// Unit tests for third_party/geos/include/geos/geom/prep/PreparedGeometry.h
// and third_party/geos/src/geom/prep/PreparedGeometry.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/geom/prep/PreparedGeometry.h
//   https://github.com/libgeos/geos/blob/main/src/geom/prep/PreparedGeometry.cpp

#include "third_party/geos/include/geos/geom/prep/PreparedGeometry.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/IntersectionMatrix.h"
#include "third_party/geos/include/geos/geom/prep/PreparedGeometryFactory.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/operation/distance/DistanceOp.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::IntersectionMatrix;
using geos::geom::prep::PreparedGeometry;
using geos::geom::prep::PreparedGeometryFactory;
using geos::io::WKTReader;
using geos::operation::distance::DistanceOp;

class StubPreparedGeometry final : public PreparedGeometry {
 public:
  explicit StubPreparedGeometry(const Geometry& geom,
                                int* destroy_count = nullptr)
      : geom_(geom), destroy_count_(destroy_count) {}

  ~StubPreparedGeometry() override {
    if (destroy_count_ != nullptr) {
      ++(*destroy_count_);
    }
  }

  const Geometry& getGeometry() const override { return geom_; }

  bool contains(const Geometry* geom) const override {
    return geom_.contains(geom);
  }

  bool containsProperly(const Geometry* geom) const override {
    return geom_.relate(geom, "T**FF*FF*");
  }

  bool coveredBy(const Geometry* geom) const override {
    return geom_.coveredBy(geom);
  }

  bool covers(const Geometry* geom) const override {
    return geom_.covers(geom);
  }

  bool crosses(const Geometry* geom) const override {
    return geom_.crosses(geom);
  }

  bool disjoint(const Geometry* geom) const override {
    return geom_.disjoint(geom);
  }

  bool intersects(const Geometry* geom) const override {
    return geom_.intersects(geom);
  }

  bool overlaps(const Geometry* geom) const override {
    return geom_.overlaps(geom);
  }

  bool touches(const Geometry* geom) const override {
    return geom_.touches(geom);
  }

  bool within(const Geometry* geom) const override {
    return geom_.within(geom);
  }

  std::unique_ptr<CoordinateSequence> nearestPoints(
      const Geometry* geom) const override {
    return DistanceOp::nearestPoints(&geom_, geom);
  }

  double distance(const Geometry* geom) const override {
    return geom_.distance(geom);
  }

  bool isWithinDistance(const Geometry* geom, double dist) const override {
    return geom_.isWithinDistance(geom, dist);
  }

  std::unique_ptr<IntersectionMatrix> relate(
      const Geometry* geom) const override {
    return geom_.relate(geom);
  }

  bool relate(const Geometry* geom, const std::string& pat) const override {
    return geom_.relate(geom, pat);
  }

 private:
  const Geometry& geom_;
  int* destroy_count_;
};

TEST(PreparedGeometryTest, VirtualDestructorOnStackAndHeap) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  Geometry::Ptr poly = reader.read("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))");
  ASSERT_NE(poly, nullptr);

  int destroy_count = 0;
  {
    StubPreparedGeometry stack_pg(*poly, &destroy_count);
    const PreparedGeometry& iface = stack_pg;
    EXPECT_EQ(&iface.getGeometry(), poly.get());
  }
  EXPECT_EQ(destroy_count, 1);

  {
    std::unique_ptr<PreparedGeometry> heap_pg =
        std::make_unique<StubPreparedGeometry>(*poly, &destroy_count);
    EXPECT_EQ(&heap_pg->getGeometry(), poly.get());
  }
  EXPECT_EQ(destroy_count, 2);

  std::unique_ptr<PreparedGeometry> factory_pg =
      PreparedGeometryFactory::prepare(poly.get());
  ASSERT_NE(factory_pg, nullptr);
  PreparedGeometryFactory::destroy(factory_pg.release());
}

TEST(PreparedGeometryTest, InterfaceContractViaCustomSubclass) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr target = reader.read("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))");
  Geometry::Ptr inner_pt = reader.read("POINT (5 5)");
  Geometry::Ptr boundary_pt = reader.read("POINT (0 5)");
  Geometry::Ptr outside_pt = reader.read("POINT (13 4)");
  Geometry::Ptr outer_poly =
      reader.read("POLYGON ((-2 -2, 12 -2, 12 12, -2 12, -2 -2))");
  Geometry::Ptr touching_poly =
      reader.read("POLYGON ((10 0, 15 0, 15 5, 10 5, 10 0))");
  Geometry::Ptr overlapping_poly =
      reader.read("POLYGON ((5 5, 15 5, 15 15, 5 15, 5 5))");
  Geometry::Ptr crossing_line = reader.read("LINESTRING (-2 5, 12 5)");

  StubPreparedGeometry stub(*target);
  const PreparedGeometry& pg = stub;

  EXPECT_EQ(&pg.getGeometry(), target.get());
  EXPECT_TRUE(pg.contains(inner_pt.get()));
  EXPECT_TRUE(pg.containsProperly(inner_pt.get()));
  EXPECT_FALSE(pg.contains(boundary_pt.get()));
  EXPECT_FALSE(pg.containsProperly(boundary_pt.get()));
  EXPECT_TRUE(pg.covers(boundary_pt.get()));
  EXPECT_TRUE(pg.coveredBy(outer_poly.get()));
  EXPECT_TRUE(pg.within(outer_poly.get()));
  EXPECT_TRUE(pg.crosses(crossing_line.get()));
  EXPECT_TRUE(pg.disjoint(outside_pt.get()));
  EXPECT_FALSE(pg.intersects(outside_pt.get()));
  EXPECT_TRUE(pg.overlaps(overlapping_poly.get()));
  EXPECT_TRUE(pg.touches(touching_poly.get()));

  std::unique_ptr<CoordinateSequence> nearest =
      pg.nearestPoints(outside_pt.get());
  ASSERT_NE(nearest, nullptr);
  ASSERT_EQ(nearest->size(), 2u);
  CoordinateXY p0;
  CoordinateXY p1;
  nearest->getAt(0, p0);
  nearest->getAt(1, p1);
  EXPECT_DOUBLE_EQ(p0.x, 10.0);
  EXPECT_DOUBLE_EQ(p0.y, 4.0);
  EXPECT_DOUBLE_EQ(p1.x, 13.0);
  EXPECT_DOUBLE_EQ(p1.y, 4.0);

  EXPECT_DOUBLE_EQ(pg.distance(outside_pt.get()), 3.0);
  EXPECT_TRUE(pg.isWithinDistance(outside_pt.get(), 3.0));
  EXPECT_FALSE(pg.isWithinDistance(outside_pt.get(), 2.9));

  std::unique_ptr<IntersectionMatrix> im = pg.relate(overlapping_poly.get());
  ASSERT_NE(im, nullptr);
  EXPECT_EQ(im->toString(), "212101212");
  EXPECT_TRUE(pg.relate(overlapping_poly.get(), "212101212"));
  EXPECT_FALSE(pg.relate(overlapping_poly.get(), "FF*FF****"));
}

TEST(PreparedGeometryTest, InterfaceDispatchViaFactoryGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  Geometry::Ptr line = reader.read("LINESTRING (0 0, 10 0)");
  Geometry::Ptr cross_line = reader.read("LINESTRING (5 -5, 5 5)");
  Geometry::Ptr interior_sub_line = reader.read("LINESTRING (2 0, 8 0)");
  Geometry::Ptr endpoint_sub_line = reader.read("LINESTRING (0 0, 5 0)");
  Geometry::Ptr far_pt = reader.read("POINT (10 4)");

  std::unique_ptr<PreparedGeometry> pg =
      PreparedGeometryFactory::prepare(line.get());
  ASSERT_NE(pg, nullptr);
  const PreparedGeometry& iface = *pg;

  EXPECT_EQ(&iface.getGeometry(), line.get());
  EXPECT_TRUE(iface.crosses(cross_line.get()));
  EXPECT_TRUE(iface.contains(interior_sub_line.get()));
  EXPECT_TRUE(iface.containsProperly(interior_sub_line.get()));
  EXPECT_TRUE(iface.contains(endpoint_sub_line.get()));
  EXPECT_TRUE(iface.covers(endpoint_sub_line.get()));
  EXPECT_FALSE(iface.containsProperly(endpoint_sub_line.get()));
  EXPECT_TRUE(iface.disjoint(far_pt.get()));
  EXPECT_FALSE(iface.intersects(far_pt.get()));
  EXPECT_DOUBLE_EQ(iface.distance(far_pt.get()), 4.0);
  EXPECT_TRUE(iface.isWithinDistance(far_pt.get(), 4.0));
  EXPECT_FALSE(iface.isWithinDistance(far_pt.get(), 3.5));

  std::unique_ptr<CoordinateSequence> nearest =
      iface.nearestPoints(far_pt.get());
  ASSERT_NE(nearest, nullptr);
  ASSERT_EQ(nearest->size(), 2u);
  EXPECT_DOUBLE_EQ(nearest->getX(0), 10.0);
  EXPECT_DOUBLE_EQ(nearest->getY(0), 0.0);
  EXPECT_DOUBLE_EQ(nearest->getX(1), 10.0);
  EXPECT_DOUBLE_EQ(nearest->getY(1), 4.0);
}

}  // namespace
}  // namespace geos_google
