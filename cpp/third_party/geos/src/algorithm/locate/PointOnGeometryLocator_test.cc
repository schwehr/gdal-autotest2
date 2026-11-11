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
// third_party/geos/include/geos/algorithm/locate/PointOnGeometryLocator.h and
// third_party/geos/src/algorithm/locate/PointOnGeometryLocator.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/algorithm/locate/PointOnGeometryLocator.h

#include "third_party/geos/include/geos/algorithm/locate/PointOnGeometryLocator.h"

#include <array>
#include <memory>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/locate/IndexedPointInAreaLocator.h"
#include "third_party/geos/include/geos/algorithm/locate/SimplePointInAreaLocator.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Location.h"
#include "third_party/geos/include/geos/io/WKTReader.h"

namespace geos_google {
namespace {

using geos::algorithm::locate::IndexedPointInAreaLocator;
using geos::algorithm::locate::PointOnGeometryLocator;
using geos::algorithm::locate::SimplePointInAreaLocator;
using geos::geom::CoordinateXY;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Location;
using geos::io::WKTReader;

static_assert(std::is_abstract_v<PointOnGeometryLocator>);
static_assert(std::is_polymorphic_v<PointOnGeometryLocator>);
static_assert(std::has_virtual_destructor_v<PointOnGeometryLocator>);
static_assert(!std::is_constructible_v<PointOnGeometryLocator>);

class UnitSquareStubLocator final : public PointOnGeometryLocator {
 public:
  explicit UnitSquareStubLocator(bool* destroyed_flag)
      : destroyed_(destroyed_flag) {}

  ~UnitSquareStubLocator() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  Location locate(const CoordinateXY* p) override {
    if (p == nullptr) {
      return Location::NONE;
    }
    if (p->x > 0.0 && p->x < 1.0 && p->y > 0.0 && p->y < 1.0) {
      return Location::INTERIOR;
    }
    if (p->x >= 0.0 && p->x <= 1.0 && p->y >= 0.0 && p->y <= 1.0) {
      return Location::BOUNDARY;
    }
    return Location::EXTERIOR;
  }

 private:
  bool* destroyed_;
};

TEST(PointOnGeometryLocatorTest, VirtualDispatchAndStackDestructor) {
  bool destroyed = false;
  {
    UnitSquareStubLocator stub(&destroyed);
    PointOnGeometryLocator& interface = stub;

    const CoordinateXY interior(0.5, 0.5);
    const CoordinateXY boundary(1.0, 0.5);
    const CoordinateXY exterior(2.0, 0.5);

    EXPECT_EQ(interface.locate(&interior), Location::INTERIOR);
    EXPECT_EQ(interface.locate(&boundary), Location::BOUNDARY);
    EXPECT_EQ(interface.locate(&exterior), Location::EXTERIOR);
    EXPECT_EQ(interface.locate(nullptr), Location::NONE);
    EXPECT_FALSE(destroyed);
  }
  EXPECT_TRUE(destroyed);
}

TEST(PointOnGeometryLocatorTest, VirtualDestructorDeletesViaBasePointer) {
  bool destroyed = false;
  std::unique_ptr<PointOnGeometryLocator> locator =
      std::make_unique<UnitSquareStubLocator>(&destroyed);
  const CoordinateXY inside(0.25, 0.75);
  EXPECT_EQ(locator->locate(&inside), Location::INTERIOR);
  EXPECT_FALSE(destroyed);

  locator.reset();
  EXPECT_TRUE(destroyed);
}

TEST(PointOnGeometryLocatorTest, ConcreteImplementationsAgreePolymorphically) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<Geometry> polygon = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (3 3, 7 3, 7 7, 3 7, 3 3))");
  ASSERT_NE(polygon, nullptr);

  std::array<std::unique_ptr<PointOnGeometryLocator>, 2> locators = {
      std::make_unique<SimplePointInAreaLocator>(*polygon),
      std::make_unique<IndexedPointInAreaLocator>(*polygon),
  };

  struct QueryCase {
    std::string_view name;
    double x;
    double y;
    Location expected;
  };
  constexpr auto kCases = std::to_array<QueryCase>({
      {.name = "interior", .x = 1.0, .y = 1.0, .expected = Location::INTERIOR},
      {.name = "outer_boundary",
       .x = 5.0,
       .y = 0.0,
       .expected = Location::BOUNDARY},
      {.name = "hole_boundary",
       .x = 3.0,
       .y = 5.0,
       .expected = Location::BOUNDARY},
      {.name = "inside_hole",
       .x = 5.0,
       .y = 5.0,
       .expected = Location::EXTERIOR},
      {.name = "outside", .x = 15.0, .y = 5.0, .expected = Location::EXTERIOR},
  });

  for (const auto& locator : locators) {
    for (const auto& tc : kCases) {
      SCOPED_TRACE(tc.name);
      const CoordinateXY pt(tc.x, tc.y);
      EXPECT_EQ(locator->locate(&pt), tc.expected);
    }
  }
}

}  // namespace
}  // namespace geos_google
