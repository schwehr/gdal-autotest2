// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Unit tests for ISO-19111/19115 metadata classes (Citation, GeographicExtent,
// GeographicBoundingBox, VerticalExtent, TemporalExtent, Extent, Identifier,
// and PositionalAccuracy) in proj/src/iso19111/metadata.cpp. Note: in google3
// default non-GCE builds, proj.db is embedded in memory and networking is
// disabled. Tests calling PROJ_INTERNAL C++ methods require -DFROM_PROJ_CPP,
// -fexceptions, and linkstatic = True.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/metadata.cpp

#include "third_party/proj4/proj/include/proj/metadata.hpp"

#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

using ::osgeo::proj::common::UnitOfMeasure;
using ::osgeo::proj::io::DatabaseContext;
using ::osgeo::proj::io::DatabaseContextPtr;
using ::osgeo::proj::io::IJSONExportable;
using ::osgeo::proj::io::IWKTExportable;
using ::osgeo::proj::io::JSONFormatter;
using ::osgeo::proj::io::WKTFormatter;
using ::osgeo::proj::metadata::Citation;
using ::osgeo::proj::metadata::Extent;
using ::osgeo::proj::metadata::GeographicBoundingBox;
using ::osgeo::proj::metadata::GeographicExtent;
using ::osgeo::proj::metadata::GeographicExtentNNPtr;
using ::osgeo::proj::metadata::GeographicExtentPtr;
using ::osgeo::proj::metadata::Identifier;
using ::osgeo::proj::metadata::IdentifierNNPtr;
using ::osgeo::proj::metadata::PositionalAccuracy;
using ::osgeo::proj::metadata::TemporalExtent;
using ::osgeo::proj::metadata::VerticalExtent;
using ::osgeo::proj::util::IComparable;
using ::osgeo::proj::util::InvalidValueTypeException;
using ::osgeo::proj::util::nn_dynamic_pointer_cast;
using ::osgeo::proj::util::nn_make_shared;
using ::osgeo::proj::util::nn_static_pointer_cast;
using ::osgeo::proj::util::optional;
using ::osgeo::proj::util::PropertyMap;
using ::testing::IsEmpty;
using ::testing::NotNull;

class TestableCitation final : public Citation {
 public:
  using Citation::Citation;
  using Citation::operator=;
};

class TestableIdentifier final : public Identifier {
 public:
  static IdentifierNNPtr CreateFromDescription(const std::string& desc) {
    return Identifier::createFromDescription(desc);
  }
};

class RawGeographicBoundingBox final : public GeographicBoundingBox {
 public:
  RawGeographicBoundingBox(double west, double south, double east, double north)
      : GeographicBoundingBox(west, south, east, north) {}
};

class DummyGeographicExtent final : public GeographicExtent {
 public:
  DummyGeographicExtent() : GeographicExtent() {}
  ~DummyGeographicExtent() override = default;

  bool _isEquivalentTo(const IComparable* other, Criterion,
                       const DatabaseContextPtr&) const override {
    return other == this;
  }

  bool contains(const GeographicExtentNNPtr&) const override { return false; }

  bool intersects(const GeographicExtentNNPtr&) const override { return false; }

  GeographicExtentPtr intersection(
      const GeographicExtentNNPtr&) const override {
    return nullptr;
  }
};

std::string ExportWkt(const IWKTExportable& obj,
                      WKTFormatter::Convention convention,
                      const DatabaseContextPtr& db_context = nullptr) {
  auto formatter = WKTFormatter::create(convention, db_context);
  formatter->setMultiLine(false);
  return obj.exportToWKT(formatter.get());
}

std::string ExportJson(const IJSONExportable& obj,
                       const DatabaseContextPtr& db_context = nullptr) {
  auto formatter = JSONFormatter::create(db_context);
  formatter->setMultiLine(false).setSchema("");
  return obj.exportToJSON(formatter.get());
}

void ExpectBboxBounds(const GeographicBoundingBox& bbox, double west,
                      double south, double east, double north) {
  EXPECT_DOUBLE_EQ(west, bbox.westBoundLongitude());
  EXPECT_DOUBLE_EQ(south, bbox.southBoundLatitude());
  EXPECT_DOUBLE_EQ(east, bbox.eastBoundLongitude());
  EXPECT_DOUBLE_EQ(north, bbox.northBoundLatitude());
}

TEST(MetadataTest, CitationDefaultCopyAssignAndSelfAssign) {
  const Citation empty_citation;
  EXPECT_FALSE(empty_citation.title().has_value());

  const Citation epsg_citation("EPSG");
  ASSERT_TRUE(epsg_citation.title().has_value());
  EXPECT_EQ("EPSG", *epsg_citation.title());

  const Citation copied_citation(epsg_citation);
  ASSERT_TRUE(copied_citation.title().has_value());
  EXPECT_EQ("EPSG", *copied_citation.title());

  TestableCitation assigned("Initial");
  assigned = TestableCitation("Updated");
  ASSERT_TRUE(assigned.title().has_value());
  EXPECT_EQ("Updated", *assigned.title());

  // Exercise the `this != &other` false branch without triggering
  // -Wself-assign.
  const Citation& self_ref = assigned;
  assigned = self_ref;
  ASSERT_TRUE(assigned.title().has_value());
  EXPECT_EQ("Updated", *assigned.title());
}

TEST(MetadataTest, GeographicBoundingBoxCreateValidationAndDegenerateExpand) {
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(GeographicBoundingBox::create(kNaN, 0.0, 10.0, 10.0),
               InvalidValueTypeException);
  EXPECT_THROW(GeographicBoundingBox::create(0.0, kNaN, 10.0, 10.0),
               InvalidValueTypeException);
  EXPECT_THROW(GeographicBoundingBox::create(0.0, 0.0, kNaN, 10.0),
               InvalidValueTypeException);
  EXPECT_THROW(GeographicBoundingBox::create(0.0, 0.0, 10.0, kNaN),
               InvalidValueTypeException);
  EXPECT_THROW(GeographicBoundingBox::create(0.0, 20.0, 10.0, 10.0),
               InvalidValueTypeException);

  const auto valid = GeographicBoundingBox::create(-5.5, 41.0, 9.5, 51.5);
  ExpectBboxBounds(*valid, -5.5, 41.0, 9.5, 51.5);

  // Interior point (west == east and south == north) expands on both sides.
  const auto interior_pt =
      GeographicBoundingBox::create(10.0, 20.0, 10.0, 20.0);
  EXPECT_LT(interior_pt->westBoundLongitude(), 10.0);
  EXPECT_GT(interior_pt->eastBoundLongitude(), 10.0);
  EXPECT_LT(interior_pt->southBoundLatitude(), 20.0);
  EXPECT_GT(interior_pt->northBoundLatitude(), 20.0);

  // Clamped corners (-180, -90) and (180, 90) expand only inward.
  const auto sw_corner =
      GeographicBoundingBox::create(-180.0, -90.0, -180.0, -90.0);
  EXPECT_DOUBLE_EQ(-180.0, sw_corner->westBoundLongitude());
  EXPECT_GT(sw_corner->eastBoundLongitude(), -180.0);
  EXPECT_DOUBLE_EQ(-90.0, sw_corner->southBoundLatitude());
  EXPECT_GT(sw_corner->northBoundLatitude(), -90.0);

  const auto ne_corner =
      GeographicBoundingBox::create(180.0, 90.0, 180.0, 90.0);
  EXPECT_LT(ne_corner->westBoundLongitude(), 180.0);
  EXPECT_DOUBLE_EQ(180.0, ne_corner->eastBoundLongitude());
  EXPECT_LT(ne_corner->southBoundLatitude(), 90.0);
  EXPECT_DOUBLE_EQ(90.0, ne_corner->northBoundLatitude());
}

TEST(MetadataTest, GeographicBoundingBoxEquivalenceAndNonBboxFallback) {
  const auto base = GeographicBoundingBox::create(-10.0, -20.0, 10.0, 20.0);
  const auto same = GeographicBoundingBox::create(-10.0, -20.0, 10.0, 20.0);
  const auto diff_west =
      GeographicBoundingBox::create(-11.0, -20.0, 10.0, 20.0);
  const auto diff_south =
      GeographicBoundingBox::create(-10.0, -21.0, 10.0, 20.0);
  const auto diff_east =
      GeographicBoundingBox::create(-10.0, -20.0, 11.0, 20.0);
  const auto diff_north =
      GeographicBoundingBox::create(-10.0, -20.0, 10.0, 21.0);

  EXPECT_TRUE(base->isEquivalentTo(base.get()));
  EXPECT_TRUE(
      base->isEquivalentTo(same.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(base->isEquivalentTo(diff_west.get()));
  EXPECT_FALSE(base->isEquivalentTo(diff_south.get()));
  EXPECT_FALSE(base->isEquivalentTo(diff_east.get()));
  EXPECT_FALSE(base->isEquivalentTo(diff_north.get()));
  EXPECT_FALSE(base->isEquivalentTo(nullptr));
  EXPECT_FALSE(base->isEquivalentTo(Extent::WORLD.get()));

  const GeographicExtentNNPtr non_bbox =
      nn_static_pointer_cast<GeographicExtent>(
          nn_make_shared<DummyGeographicExtent>());
  EXPECT_FALSE(base->isEquivalentTo(non_bbox.get()));
  EXPECT_FALSE(base->contains(non_bbox));
  EXPECT_FALSE(base->intersects(non_bbox));
  EXPECT_EQ(nullptr, base->intersection(non_bbox));
}

TEST(MetadataTest, GeographicBoundingBoxContainsNormalWorldAndAntimeridian) {
  const auto world = GeographicBoundingBox::create(-180.0, -90.0, 180.0, 90.0);
  const auto normal = GeographicBoundingBox::create(-10.0, -20.0, 10.0, 20.0);
  const auto antimeridian =
      GeographicBoundingBox::create(170.0, -20.0, -170.0, 20.0);

  // World contains non-degenerate normal and antimeridian boxes, but fails on
  // a degenerate oW == oE box or latitude exceeding bounds.
  EXPECT_TRUE(world->contains(normal));
  EXPECT_TRUE(world->contains(antimeridian));
  const auto degenerate =
      nn_make_shared<RawGeographicBoundingBox>(10.0, -10.0, 10.0, 10.0);
  EXPECT_FALSE(world->contains(degenerate));

  const auto mid_lat_world =
      GeographicBoundingBox::create(-180.0, -60.0, 180.0, 60.0);
  EXPECT_FALSE(mid_lat_world->contains(
      GeographicBoundingBox::create(-10.0, -70.0, 10.0, 50.0)));
  EXPECT_FALSE(mid_lat_world->contains(
      GeographicBoundingBox::create(-10.0, -50.0, 10.0, 70.0)));

  // Non-world boxes (both normal and antimeridian) never contain world.
  EXPECT_FALSE(normal->contains(world));
  EXPECT_FALSE(antimeridian->contains(world));

  // Normal vs normal and normal vs antimeridian.
  EXPECT_TRUE(
      normal->contains(GeographicBoundingBox::create(-5.0, -10.0, 5.0, 10.0)));
  EXPECT_FALSE(
      normal->contains(GeographicBoundingBox::create(-15.0, -10.0, 5.0, 10.0)));
  EXPECT_FALSE(
      normal->contains(GeographicBoundingBox::create(-5.0, -10.0, 15.0, 10.0)));
  EXPECT_FALSE(normal->contains(antimeridian));

  // Antimeridian containing normal boxes on east side, west side, or neither.
  EXPECT_TRUE(antimeridian->contains(
      GeographicBoundingBox::create(172.0, -10.0, 179.0, 10.0)));
  EXPECT_TRUE(antimeridian->contains(
      GeographicBoundingBox::create(-179.0, -10.0, -172.0, 10.0)));
  EXPECT_FALSE(antimeridian->contains(
      GeographicBoundingBox::create(160.0, -10.0, 175.0, 10.0)));
  EXPECT_FALSE(antimeridian->contains(
      GeographicBoundingBox::create(-175.0, -10.0, -160.0, 10.0)));
  EXPECT_FALSE(antimeridian->contains(
      GeographicBoundingBox::create(-10.0, -10.0, 10.0, 10.0)));

  // Antimeridian vs antimeridian.
  EXPECT_TRUE(antimeridian->contains(
      GeographicBoundingBox::create(175.0, -10.0, -175.0, 10.0)));
  EXPECT_FALSE(antimeridian->contains(
      GeographicBoundingBox::create(165.0, -10.0, -175.0, 10.0)));
  EXPECT_FALSE(antimeridian->contains(
      GeographicBoundingBox::create(175.0, -10.0, -165.0, 10.0)));
}

TEST(MetadataTest, GeographicBoundingBoxIntersectsAndIntersectionAllBranches) {
  struct BboxSpec {
    double west;
    double south;
    double east;
    double north;
  };
  struct IntersectionCase {
    std::string_view name;
    BboxSpec a;
    BboxSpec b;
    bool expected_intersects;
    bool expected_has_intersection;
    BboxSpec expected_inter;
  };

  constexpr IntersectionCase kCases[] = {
      {
          .name = "LatitudeDisjointNorthOfOther",
          .a = {0.0, 20.0, 10.0, 30.0},
          .b = {0.0, 0.0, 10.0, 10.0},
          .expected_intersects = false,
          .expected_has_intersection = false,
          .expected_inter = {},
      },
      {
          .name = "LatitudeDisjointSouthOfOther",
          .a = {0.0, 0.0, 10.0, 10.0},
          .b = {0.0, 20.0, 10.0, 30.0},
          .expected_intersects = false,
          .expected_has_intersection = false,
          .expected_inter = {},
      },
      {
          .name = "WorldAndAntimeridian",
          .a = {-180.0, -90.0, 180.0, 90.0},
          .b = {170.0, -10.0, -170.0, 10.0},
          .expected_intersects = true,
          .expected_has_intersection = true,
          .expected_inter = {170.0, -10.0, -170.0, 10.0},
      },
      {
          .name = "AntimeridianAndWorld",
          .a = {170.0, -10.0, -170.0, 10.0},
          .b = {-180.0, -90.0, 180.0, 90.0},
          .expected_intersects = true,
          .expected_has_intersection = true,
          .expected_inter = {170.0, -10.0, -170.0, 10.0},
      },
      {
          .name = "NormalOverlap",
          .a = {0.0, 0.0, 20.0, 20.0},
          .b = {10.0, -5.0, 30.0, 15.0},
          .expected_intersects = true,
          .expected_has_intersection = true,
          .expected_inter = {10.0, 0.0, 20.0, 15.0},
      },
      {
          .name = "NormalLongitudeDisjoint",
          .a = {0.0, 0.0, 10.0, 10.0},
          .b = {20.0, 0.0, 30.0, 10.0},
          .expected_intersects = false,
          .expected_has_intersection = false,
          .expected_inter = {},
      },
      {
          .name = "NormalTouchingMeridian",
          .a = {0.0, 0.0, 10.0, 10.0},
          .b = {10.0, 0.0, 20.0, 10.0},
          .expected_intersects = false,
          .expected_has_intersection = false,
          .expected_inter = {},
      },
      {
          .name = "NormalAndAntimeridianWestOutOfRange",
          .a = {0.0, -10.0, 10.0, 10.0},
          .b = {190.0, -10.0, 170.0, 10.0},
          .expected_intersects = false,
          .expected_has_intersection = false,
          .expected_inter = {},
      },
      {
          .name = "NormalAndAntimeridianEastOutOfRange",
          .a = {0.0, -10.0, 10.0, 10.0},
          .b = {-170.0, -10.0, -190.0, 10.0},
          .expected_intersects = false,
          .expected_has_intersection = false,
          .expected_inter = {},
      },
      {
          .name = "NormalIntersectsEasternHalfOfAntimeridian",
          .a = {172.0, -5.0, 179.0, 5.0},
          .b = {170.0, -10.0, -175.0, 10.0},
          .expected_intersects = true,
          .expected_has_intersection = true,
          .expected_inter = {172.0, -5.0, 179.0, 5.0},
      },
      {
          .name = "NormalIntersectsWesternHalfOfAntimeridian",
          .a = {-179.0, -5.0, -172.0, 5.0},
          .b = {175.0, -10.0, -170.0, 10.0},
          .expected_intersects = true,
          .expected_has_intersection = true,
          .expected_inter = {-179.0, -5.0, -172.0, 5.0},
      },
      {
          .name = "NormalDisjointFromAntimeridian",
          .a = {-10.0, -5.0, 10.0, 5.0},
          .b = {175.0, -10.0, -175.0, 10.0},
          .expected_intersects = false,
          .expected_has_intersection = false,
          .expected_inter = {},
      },
      {
          // Extended normal box [-200, 200] avoids the W==-180 && E==180 fast
          // path so both halves of the antimeridian split are non-null and the
          // wider half is selected.
          .name = "ExtendedNormalPicksWiderEastHalf",
          .a = {-200.0, -20.0, 200.0, 20.0},
          .b = {170.0, -10.0, -175.0, 10.0},
          .expected_intersects = true,
          .expected_has_intersection = true,
          .expected_inter = {170.0, -10.0, 180.0, 10.0},
      },
      {
          .name = "ExtendedNormalPicksWiderWestHalf",
          .a = {-200.0, -20.0, 200.0, 20.0},
          .b = {175.0, -10.0, -170.0, 10.0},
          .expected_intersects = true,
          .expected_has_intersection = true,
          .expected_inter = {-180.0, -10.0, -170.0, 10.0},
      },
      {
          .name = "AntimeridianAndNormalDelegation",
          .a = {170.0, -10.0, -170.0, 10.0},
          .b = {-175.0, -5.0, -165.0, 5.0},
          .expected_intersects = true,
          .expected_has_intersection = true,
          .expected_inter = {-175.0, -5.0, -170.0, 5.0},
      },
      {
          .name = "AntimeridianAndAntimeridian",
          .a = {170.0, -20.0, -170.0, 20.0},
          .b = {175.0, -10.0, -165.0, 30.0},
          .expected_intersects = true,
          .expected_has_intersection = true,
          .expected_inter = {175.0, -10.0, -170.0, 20.0},
      },
  };

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const auto a = GeographicBoundingBox::create(tc.a.west, tc.a.south,
                                                 tc.a.east, tc.a.north);
    const auto b = GeographicBoundingBox::create(tc.b.west, tc.b.south,
                                                 tc.b.east, tc.b.north);
    EXPECT_EQ(tc.expected_intersects, a->intersects(b));
    const auto inter = a->intersection(b);
    if (!tc.expected_has_intersection) {
      EXPECT_EQ(nullptr, inter);
    } else {
      ASSERT_NE(nullptr, inter);
      const auto bbox = std::dynamic_pointer_cast<GeographicBoundingBox>(inter);
      ASSERT_NE(nullptr, bbox);
      ExpectBboxBounds(*bbox, tc.expected_inter.west, tc.expected_inter.south,
                       tc.expected_inter.east, tc.expected_inter.north);
    }
  }

  // Boxes touching along a single latitude line still produce an intersection
  // expanded slightly around the shared parallel.
  const auto south_box = GeographicBoundingBox::create(0.0, 0.0, 10.0, 10.0);
  const auto north_box = GeographicBoundingBox::create(0.0, 10.0, 10.0, 20.0);
  EXPECT_TRUE(south_box->intersects(north_box));
  const auto line_inter = south_box->intersection(north_box);
  ASSERT_NE(nullptr, line_inter);
  const auto line_bbox =
      std::dynamic_pointer_cast<GeographicBoundingBox>(line_inter);
  ASSERT_NE(nullptr, line_bbox);
  EXPECT_LT(line_bbox->southBoundLatitude(), 10.0);
  EXPECT_GT(line_bbox->northBoundLatitude(), 10.0);
}

TEST(MetadataTest, VerticalExtentAccessorsEquivalenceContainsAndIntersects) {
  const auto metre_a = nn_make_shared<UnitOfMeasure>(UnitOfMeasure::METRE);
  const auto metre_b = nn_make_shared<UnitOfMeasure>(UnitOfMeasure::METRE);
  const auto foot = nn_make_shared<UnitOfMeasure>(UnitOfMeasure::FOOT);

  const auto base = VerticalExtent::create(0.0, 100.0, metre_a);
  EXPECT_DOUBLE_EQ(0.0, base->minimumValue());
  EXPECT_DOUBLE_EQ(100.0, base->maximumValue());
  EXPECT_EQ(UnitOfMeasure::METRE, *base->unit());

  // _isEquivalentTo compares unit shared_ptr identity, min, and max.
  const auto same = VerticalExtent::create(0.0, 100.0, metre_a);
  const auto diff_min = VerticalExtent::create(1.0, 100.0, metre_a);
  const auto diff_max = VerticalExtent::create(0.0, 101.0, metre_a);
  const auto diff_unit_ptr = VerticalExtent::create(0.0, 100.0, metre_b);
  const auto temporal = TemporalExtent::create("2020", "2021");

  EXPECT_TRUE(base->isEquivalentTo(same.get()));
  EXPECT_FALSE(base->isEquivalentTo(diff_min.get()));
  EXPECT_FALSE(base->isEquivalentTo(diff_max.get()));
  EXPECT_FALSE(base->isEquivalentTo(diff_unit_ptr.get()));
  EXPECT_FALSE(base->isEquivalentTo(nullptr));
  EXPECT_FALSE(base->isEquivalentTo(temporal.get()));

  // contains() and intersects() convert via conversionToSI() (1 ft = 0.3048 m).
  const auto inside_feet =
      VerticalExtent::create(10.0, 300.0, foot);  // <= 91.44 m
  const auto higher_feet =
      VerticalExtent::create(10.0, 400.0, foot);  // 121.92 m
  const auto lower_m = VerticalExtent::create(-5.0, 50.0, metre_b);
  const auto touching_high = VerticalExtent::create(100.0, 150.0, metre_b);
  const auto disjoint_high = VerticalExtent::create(101.0, 150.0, metre_b);
  const auto disjoint_low = VerticalExtent::create(-20.0, -1.0, metre_b);

  EXPECT_TRUE(base->contains(inside_feet));
  EXPECT_FALSE(base->contains(higher_feet));
  EXPECT_FALSE(base->contains(lower_m));

  EXPECT_TRUE(base->intersects(inside_feet));
  EXPECT_TRUE(base->intersects(higher_feet));
  EXPECT_TRUE(base->intersects(touching_high));
  EXPECT_FALSE(base->intersects(disjoint_high));
  EXPECT_FALSE(base->intersects(disjoint_low));
}

TEST(MetadataTest, TemporalExtentAccessorsEquivalenceContainsAndIntersects) {
  const auto t2010_2020 = TemporalExtent::create("2010-01-01", "2020-12-31");
  EXPECT_EQ("2010-01-01", t2010_2020->start());
  EXPECT_EQ("2020-12-31", t2010_2020->stop());

  const auto same = TemporalExtent::create("2010-01-01", "2020-12-31");
  const auto diff_start = TemporalExtent::create("2011-01-01", "2020-12-31");
  const auto diff_stop = TemporalExtent::create("2010-01-01", "2021-12-31");
  const auto bbox = GeographicBoundingBox::create(0.0, 0.0, 10.0, 10.0);

  EXPECT_TRUE(t2010_2020->isEquivalentTo(same.get()));
  EXPECT_FALSE(t2010_2020->isEquivalentTo(diff_start.get()));
  EXPECT_FALSE(t2010_2020->isEquivalentTo(diff_stop.get()));
  EXPECT_FALSE(t2010_2020->isEquivalentTo(nullptr));
  EXPECT_FALSE(t2010_2020->isEquivalentTo(bbox.get()));

  const auto inside = TemporalExtent::create("2012-01-01", "2018-01-01");
  const auto before_start = TemporalExtent::create("2009-01-01", "2015-01-01");
  const auto after_stop = TemporalExtent::create("2015-01-01", "2021-01-01");
  const auto touching = TemporalExtent::create("2020-12-31", "2025-01-01");
  const auto disjoint_before =
      TemporalExtent::create("2000-01-01", "2009-12-31");
  const auto disjoint_after =
      TemporalExtent::create("2021-01-01", "2025-01-01");

  EXPECT_TRUE(t2010_2020->contains(inside));
  EXPECT_FALSE(t2010_2020->contains(before_start));
  EXPECT_FALSE(t2010_2020->contains(after_stop));

  EXPECT_TRUE(t2010_2020->intersects(inside));
  EXPECT_TRUE(t2010_2020->intersects(before_start));
  EXPECT_TRUE(t2010_2020->intersects(touching));
  EXPECT_FALSE(t2010_2020->intersects(disjoint_before));
  EXPECT_FALSE(t2010_2020->intersects(disjoint_after));
}

TEST(MetadataTest, ExtentCreateWorldCopyAndEquivalence) {
  ASSERT_TRUE(Extent::WORLD->description().has_value());
  EXPECT_EQ("World", *Extent::WORLD->description());
  ASSERT_EQ(1u, Extent::WORLD->geographicElements().size());
  EXPECT_THAT(Extent::WORLD->verticalElements(), IsEmpty());
  EXPECT_THAT(Extent::WORLD->temporalElements(), IsEmpty());

  const auto metre = nn_make_shared<UnitOfMeasure>(UnitOfMeasure::METRE);
  const auto g1 = GeographicBoundingBox::create(-10.0, -10.0, 10.0, 10.0);
  const auto g2 = GeographicBoundingBox::create(20.0, 20.0, 30.0, 30.0);
  const auto g_diff = GeographicBoundingBox::create(20.0, 20.0, 31.0, 30.0);
  const auto v1 = VerticalExtent::create(0.0, 100.0, metre);
  const auto v_diff = VerticalExtent::create(0.0, 200.0, metre);
  const auto t1 = TemporalExtent::create("2010", "2020");
  const auto t_diff = TemporalExtent::create("2010", "2021");

  const auto full =
      Extent::create(optional<std::string>("Area"), {g1, g2}, {v1}, {t1});
  const auto same =
      Extent::create(optional<std::string>("Area"), {g1, g2}, {v1}, {t1});
  const Extent copied(*full);

  EXPECT_TRUE(full->isEquivalentTo(same.get()));
  EXPECT_TRUE(full->isEquivalentTo(&copied));

  // Verify every short-circuit condition in Extent::_isEquivalentTo.
  EXPECT_FALSE(full->isEquivalentTo(nullptr));
  EXPECT_FALSE(full->isEquivalentTo(g1.get()));
  EXPECT_FALSE(full->isEquivalentTo(
      Extent::create(optional<std::string>(), {g1, g2}, {v1}, {t1}).get()));
  EXPECT_FALSE(full->isEquivalentTo(
      Extent::create(optional<std::string>("Other"), {g1, g2}, {v1}, {t1})
          .get()));
  EXPECT_FALSE(full->isEquivalentTo(
      Extent::create(optional<std::string>("Area"), {g1}, {v1}, {t1}).get()));
  EXPECT_FALSE(full->isEquivalentTo(
      Extent::create(optional<std::string>("Area"), {g1, g2}, {}, {t1}).get()));
  EXPECT_FALSE(full->isEquivalentTo(
      Extent::create(optional<std::string>("Area"), {g1, g2}, {v1}, {}).get()));
  EXPECT_FALSE(full->isEquivalentTo(
      Extent::create(optional<std::string>("Area"), {g1, g_diff}, {v1}, {t1})
          .get()));
  EXPECT_FALSE(full->isEquivalentTo(
      Extent::create(optional<std::string>("Area"), {g1, g2}, {v_diff}, {t1})
          .get()));
  EXPECT_FALSE(full->isEquivalentTo(
      Extent::create(optional<std::string>("Area"), {g1, g2}, {v1}, {t_diff})
          .get()));

  // Both extents without description still compare equivalent.
  const auto no_desc_a = Extent::createFromBBOX(0.0, 0.0, 10.0, 10.0);
  const auto no_desc_b = Extent::createFromBBOX(0.0, 0.0, 10.0, 10.0);
  EXPECT_FALSE(no_desc_a->description().has_value());
  EXPECT_TRUE(no_desc_a->isEquivalentTo(no_desc_b.get()));
}

TEST(MetadataTest, ExtentContainsIntersectsAndIntersection) {
  const auto metre = nn_make_shared<UnitOfMeasure>(UnitOfMeasure::METRE);
  const auto base =
      Extent::create(optional<std::string>("Base"),
                     {GeographicBoundingBox::create(0.0, 0.0, 20.0, 20.0)},
                     {VerticalExtent::create(0.0, 100.0, metre)},
                     {TemporalExtent::create("2010", "2020")});
  const auto contained =
      Extent::create(optional<std::string>("Inside"),
                     {GeographicBoundingBox::create(5.0, 5.0, 15.0, 15.0)},
                     {VerticalExtent::create(10.0, 90.0, metre)},
                     {TemporalExtent::create("2012", "2018")});
  const auto bad_geog =
      Extent::create(optional<std::string>(),
                     {GeographicBoundingBox::create(30.0, 30.0, 40.0, 40.0)},
                     {VerticalExtent::create(10.0, 90.0, metre)},
                     {TemporalExtent::create("2012", "2018")});
  const auto bad_vert =
      Extent::create(optional<std::string>(),
                     {GeographicBoundingBox::create(5.0, 5.0, 15.0, 15.0)},
                     {VerticalExtent::create(200.0, 300.0, metre)},
                     {TemporalExtent::create("2012", "2018")});
  const auto bad_temp =
      Extent::create(optional<std::string>(),
                     {GeographicBoundingBox::create(5.0, 5.0, 15.0, 15.0)},
                     {VerticalExtent::create(10.0, 90.0, metre)},
                     {TemporalExtent::create("2025", "2030")});

  EXPECT_TRUE(base->contains(contained));
  EXPECT_FALSE(base->contains(bad_geog));
  EXPECT_FALSE(base->contains(bad_vert));
  EXPECT_FALSE(base->contains(bad_temp));

  EXPECT_TRUE(base->intersects(contained));
  EXPECT_FALSE(base->intersects(bad_geog));
  EXPECT_FALSE(base->intersects(bad_vert));
  EXPECT_FALSE(base->intersects(bad_temp));

  // When a category has 0 elements in either extent, that category is ignored.
  const auto empty_extent = Extent::create(optional<std::string>(), {}, {}, {});
  EXPECT_TRUE(base->contains(empty_extent));
  EXPECT_TRUE(base->intersects(empty_extent));
  EXPECT_TRUE(empty_extent->contains(base));
  EXPECT_TRUE(empty_extent->intersects(base));

  // intersection(): this->contains(other) and other->contains(this).
  EXPECT_EQ(contained, base->intersection(contained));
  EXPECT_EQ(contained, contained->intersection(base));

  // intersection(): geographically contained, but vertical mismatch causes
  // contains() to be false and falls through to geographic-only intersection.
  const auto vert_mismatch_inter = base->intersection(bad_vert);
  ASSERT_NE(nullptr, vert_mismatch_inter);
  EXPECT_THAT(vert_mismatch_inter->verticalElements(), IsEmpty());
  EXPECT_THAT(vert_mismatch_inter->temporalElements(), IsEmpty());
  ASSERT_EQ(1u, vert_mismatch_inter->geographicElements().size());

  // intersection(): partial geographic overlap returns a new Extent.
  const auto partial_a = Extent::createFromBBOX(0.0, 0.0, 20.0, 20.0);
  const auto partial_b = Extent::createFromBBOX(10.0, 10.0, 30.0, 30.0);
  const auto partial_inter = partial_a->intersection(partial_b);
  ASSERT_NE(nullptr, partial_inter);
  ASSERT_EQ(1u, partial_inter->geographicElements().size());
  const auto partial_bbox = nn_dynamic_pointer_cast<GeographicBoundingBox>(
      partial_inter->geographicElements()[0]);
  ASSERT_NE(nullptr, partial_bbox);
  ExpectBboxBounds(*partial_bbox, 10.0, 10.0, 20.0, 20.0);

  // intersection(): disjoint, missing, or multi-element geog -> nullptr.
  const auto disjoint = Extent::createFromBBOX(50.0, 50.0, 60.0, 60.0);
  const auto multi_geog =
      Extent::create(optional<std::string>(),
                     {GeographicBoundingBox::create(0.0, 0.0, 10.0, 10.0),
                      GeographicBoundingBox::create(20.0, 20.0, 30.0, 30.0)},
                     {}, {});
  EXPECT_EQ(nullptr, partial_a->intersection(disjoint));
  EXPECT_EQ(nullptr, partial_a->intersection(empty_extent));
  EXPECT_EQ(nullptr, empty_extent->intersection(partial_a));
  EXPECT_EQ(nullptr, multi_geog->intersection(partial_a));
  EXPECT_EQ(nullptr, partial_a->intersection(multi_geog));
}

TEST(MetadataTest, IdentifierPropertiesValidationAndCopy) {
  EXPECT_EQ("authority", Identifier::AUTHORITY_KEY);
  EXPECT_EQ("code", Identifier::CODE_KEY);
  EXPECT_EQ("codespace", Identifier::CODESPACE_KEY);
  EXPECT_EQ("version", Identifier::VERSION_KEY);
  EXPECT_EQ("description", Identifier::DESCRIPTION_KEY);
  EXPECT_EQ("uri", Identifier::URI_KEY);
  EXPECT_EQ("EPSG", Identifier::EPSG);
  EXPECT_EQ("OGC", Identifier::OGC);

  const auto empty = Identifier::create();
  EXPECT_THAT(empty->code(), IsEmpty());
  EXPECT_FALSE(empty->authority().has_value());
  EXPECT_FALSE(empty->codeSpace().has_value());
  EXPECT_FALSE(empty->version().has_value());
  EXPECT_FALSE(empty->description().has_value());
  EXPECT_FALSE(empty->uri().has_value());

  const auto from_desc = TestableIdentifier::CreateFromDescription("WGS 84");
  EXPECT_THAT(from_desc->code(), IsEmpty());
  ASSERT_TRUE(from_desc->description().has_value());
  EXPECT_EQ("WGS 84", *from_desc->description());

  // Full property construction with integer CODE_KEY overriding codeIn and
  // Citation object for AUTHORITY_KEY.
  const auto full = Identifier::create(
      "ignored", PropertyMap()
                     .set(Identifier::AUTHORITY_KEY,
                          nn_make_shared<Citation>("IOGP Authority"))
                     .set(Identifier::CODE_KEY, 4326)
                     .set(Identifier::CODESPACE_KEY, Identifier::EPSG)
                     .set(Identifier::VERSION_KEY, "10.0")
                     .set(Identifier::DESCRIPTION_KEY, "WGS 84")
                     .set(Identifier::URI_KEY, "https://example.org/4326"));
  ASSERT_TRUE(full->authority().has_value());
  ASSERT_TRUE(full->authority()->title().has_value());
  EXPECT_EQ("IOGP Authority", *full->authority()->title());
  EXPECT_EQ("4326", full->code());
  ASSERT_TRUE(full->codeSpace().has_value());
  EXPECT_EQ("EPSG", *full->codeSpace());
  ASSERT_TRUE(full->version().has_value());
  EXPECT_EQ("10.0", *full->version());
  ASSERT_TRUE(full->description().has_value());
  EXPECT_EQ("WGS 84", *full->description());
  ASSERT_TRUE(full->uri().has_value());
  EXPECT_EQ("https://example.org/4326", *full->uri());

  const Identifier copied(*full);
  EXPECT_EQ("4326", copied.code());
  EXPECT_EQ("EPSG", *copied.codeSpace());
  EXPECT_EQ("10.0", *copied.version());
  EXPECT_EQ("WGS 84", *copied.description());
  EXPECT_EQ("https://example.org/4326", *copied.uri());

  // String values for AUTHORITY_KEY and CODE_KEY.
  const auto str_id = Identifier::create(
      "", PropertyMap()
              .set(Identifier::AUTHORITY_KEY, "OGC")
              .set(Identifier::CODE_KEY, "CRS84")
              .set(Identifier::CODESPACE_KEY, Identifier::OGC));
  EXPECT_EQ("OGC", *str_id->authority()->title());
  EXPECT_EQ("CRS84", str_id->code());

  // Invalid value types throw InvalidValueTypeException across all branches.
  const auto non_citation_obj = PositionalAccuracy::create("1 m");
  EXPECT_THROW(Identifier::create(
                   "1", PropertyMap().set(Identifier::AUTHORITY_KEY, 123)),
               InvalidValueTypeException);
  EXPECT_THROW(
      Identifier::create(
          "1", PropertyMap().set(Identifier::AUTHORITY_KEY, non_citation_obj)),
      InvalidValueTypeException);
  EXPECT_THROW(
      Identifier::create("1", PropertyMap().set(Identifier::CODE_KEY, true)),
      InvalidValueTypeException);
  EXPECT_THROW(Identifier::create("1", PropertyMap().set(Identifier::CODE_KEY,
                                                         non_citation_obj)),
               InvalidValueTypeException);
  EXPECT_THROW(
      Identifier::create("1", PropertyMap().set(Identifier::CODESPACE_KEY, 42)),
      InvalidValueTypeException);
}

TEST(MetadataTest, IdentifierExportToWktAndJsonCoversAllFormattingBranches) {
  // Missing codeSpace or missing code exports nothing in both WKT and JSON.
  const auto no_codespace = Identifier::create("4326", PropertyMap());
  const auto no_code = Identifier::create(
      "", PropertyMap().set(Identifier::CODESPACE_KEY, Identifier::EPSG));
  EXPECT_THAT(ExportWkt(*no_codespace, WKTFormatter::Convention::WKT2),
              IsEmpty());
  EXPECT_THAT(ExportWkt(*no_code, WKTFormatter::Convention::WKT2), IsEmpty());
  EXPECT_THAT(ExportJson(*no_codespace), IsEmpty());
  EXPECT_THAT(ExportJson(*no_code), IsEmpty());

  // WKT1 outputs AUTHORITY["codespace","code"].
  const auto epsg_basic = Identifier::create(
      "4326", PropertyMap()
                  .set(Identifier::CODESPACE_KEY, Identifier::EPSG)
                  .set(Identifier::AUTHORITY_KEY, Identifier::EPSG));
  EXPECT_EQ("AUTHORITY[\"EPSG\",\"4326\"]",
            ExportWkt(*epsg_basic, WKTFormatter::Convention::WKT1_GDAL));

  // WKT2/JSON with numeric code, no version, matching authority (no CITATION).
  EXPECT_EQ("ID[\"EPSG\",4326]",
            ExportWkt(*epsg_basic, WKTFormatter::Convention::WKT2));
  EXPECT_EQ("{\"authority\":\"EPSG\",\"code\":4326}", ExportJson(*epsg_basic));

  // WKT2/JSON with string code, numeric version, distinct authority, and URI.
  const auto full_numeric_ver = Identifier::create(
      "CRS84", PropertyMap()
                   .set(Identifier::CODESPACE_KEY, Identifier::OGC)
                   .set(Identifier::VERSION_KEY, "1.3")
                   .set(Identifier::AUTHORITY_KEY, "Open Geospatial Consortium")
                   .set(Identifier::URI_KEY,
                        "http://www.opengis.net/def/crs/OGC/1.3/CRS84"));
  EXPECT_EQ(
      "ID[\"OGC\",\"CRS84\",1.3,"
      "CITATION[\"Open Geospatial Consortium\"],"
      "URI[\"http://www.opengis.net/def/crs/OGC/1.3/CRS84\"]]",
      ExportWkt(*full_numeric_ver, WKTFormatter::Convention::WKT2));
  EXPECT_EQ(
      "{\"authority\":\"OGC\",\"code\":\"CRS84\",\"version\":1.3,"
      "\"authority_citation\":\"Open Geospatial Consortium\","
      "\"uri\":\"http://www.opengis.net/def/crs/OGC/1.3/CRS84\"}",
      ExportJson(*full_numeric_ver));

  // Non-numeric version string is quoted in both WKT2 and JSON.
  const auto alpha_ver =
      Identifier::create("100", PropertyMap()
                                    .set(Identifier::CODESPACE_KEY, "CUSTOM")
                                    .set(Identifier::VERSION_KEY, "v2026a"));
  EXPECT_EQ("ID[\"CUSTOM\",100,\"v2026a\"]",
            ExportWkt(*alpha_ver, WKTFormatter::Convention::WKT2));
  EXPECT_EQ("{\"authority\":\"CUSTOM\",\"code\":100,\"version\":\"v2026a\"}",
            ExportJson(*alpha_ver));

  // DatabaseContext versioned-authority lookup splits IAU_2015 -> (IAU, 2015).
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  const auto db = DatabaseContext::create(std::string(), {}, ctx.get());
  const auto iau_id = Identifier::create(
      "39900", PropertyMap().set(Identifier::CODESPACE_KEY, "IAU_2015"));
  EXPECT_EQ("ID[\"IAU\",39900,2015]",
            ExportWkt(*iau_id, WKTFormatter::Convention::WKT2, db));
  EXPECT_EQ("{\"authority\":\"IAU\",\"code\":39900,\"version\":2015}",
            ExportJson(*iau_id, db));
  EXPECT_EQ("ID[\"EPSG\",4326]",
            ExportWkt(*epsg_basic, WKTFormatter::Convention::WKT2, db));
}

TEST(MetadataTest, IdentifierCanonicalizeNameCoversAllNormalizationRules) {
  struct CanonicalCase {
    std::string_view input;
    bool bigger_diff;
    std::string_view expected;
  };
  constexpr CanonicalCase kCases[] = {
      {.input = "", .bigger_diff = true, .expected = ""},
      // Ignored characters and case folding.
      {.input = "A_b-C/d(e)f.g&h,i ",
       .bigger_diff = true,
       .expected = "abcdefghi"},
      // " + " separator removal.
      {.input = "WGS 84 + EGM96 height",
       .bigger_diff = true,
       .expected = "wgs84egm96"},
      // "19dd" -> "dd" only when preceded by non-empty non-digit.
      {.input = "NAD 1983", .bigger_diff = true, .expected = "nad83"},
      {.input = "1984", .bigger_diff = true, .expected = "1984"},
      {.input = "A21984", .bigger_diff = true, .expected = "a21984"},
      {.input = "WGS 19ab", .bigger_diff = true, .expected = "wgs19ab"},
      {.input = "WGS 1884", .bigger_diff = true, .expected = "wgs1884"},
      // biggerDifferencesAllowed toggles "zone", "height", "north", "south".
      {.input = "UTM Zone 32North", .bigger_diff = true, .expected = "utm32n"},
      {.input = "UTM Zone 32North",
       .bigger_diff = false,
       .expected = "utmzone32north"},
      {.input = "UTM Zone 32 South", .bigger_diff = true, .expected = "utm32s"},
      {.input = "Zone 32", .bigger_diff = true, .expected = "zone32"},
      {.input = "Timezone", .bigger_diff = true, .expected = "timezone"},
      {.input = "TrueNorth", .bigger_diff = true, .expected = "truenorth"},
      {.input = "EGM96 height",
       .bigger_diff = false,
       .expected = "egm96height"},
      // All 10 UTF-8 accent entries in map_utf8_to_lower.
      {.input = "\xc3\xa1\xc3\xa4\xc4\x9b\xc3\xa8\xc3\xa9"
                "\xc3\xab\xc3\xad\xc3\xb4\xc3\xb6\xc3\xa7",
       .bigger_diff = true,
       .expected = "aaeeeeiooc"},
      // Unmapped >127 UTF-8 sequences stay intact.
      {.input = "Espa\xc3\xb1"
                "a",
       .bigger_diff = true,
       .expected = "espa\xc3\xb1"
                   "a"},
      // "_IntlFeet" suffix only canonicalizes at end of string.
      {.input = "US_IntlFeet", .bigger_diff = true, .expected = "usfeet"},
      {.input = "US_IntlFeet_X",
       .bigger_diff = true,
       .expected = "usintlfeetx"},
      {.input = "US_Intl", .bigger_diff = true, .expected = "usintl"},
      {.input = "UTM 32Nor", .bigger_diff = true, .expected = "utm32nor"},
  };

  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.expected, Identifier::canonicalizeName(std::string(tc.input),
                                                        tc.bigger_diff))
        << tc.input;
  }
}

TEST(MetadataTest, IdentifierIsEquivalentNameCoversTwoAndThreeArgOverloads) {
  struct EquivCase {
    const char* a;
    const char* b;
    bool bigger_diff;
    bool expected;
  };
  constexpr EquivCase kCases[] = {
      {.a = "", .b = "", .bigger_diff = true, .expected = true},
      {.a = "", .b = "a", .bigger_diff = true, .expected = false},
      {.a = "a", .b = "", .bigger_diff = true, .expected = false},
      {.a = "WGS_84-(1)",
       .b = "wgs/84.1&,",
       .bigger_diff = true,
       .expected = true},
      {.a = "WGS84 ", .b = "WGS84", .bigger_diff = true, .expected = true},
      {.a = "WGS84", .b = "WGS84 ", .bigger_diff = true, .expected = true},
      {.a = "abc", .b = "abcd", .bigger_diff = true, .expected = false},
      {.a = "abcd", .b = "abc", .bigger_diff = true, .expected = false},
      // " + " skipping on both sides, except when trailing at end of string.
      {.a = "WGS 84 + EGM96",
       .b = "WGS84EGM96",
       .bigger_diff = true,
       .expected = true},
      {.a = "WGS84EGM96",
       .b = "WGS 84 + EGM96",
       .bigger_diff = true,
       .expected = true},
      {.a = "WGS84 + ", .b = "WGS84", .bigger_diff = true, .expected = false},
      {.a = "WGS84", .b = "WGS84 + ", .bigger_diff = true, .expected = false},
      // _IntlFeet vs _Feet in both argument orders, only at end of string.
      {.a = "NAD83_IntlFeet",
       .b = "NAD83_Feet",
       .bigger_diff = true,
       .expected = true},
      {.a = "NAD83_Feet",
       .b = "NAD83_IntlFeet",
       .bigger_diff = true,
       .expected = true},
      {.a = "NAD83_IntlFeet_2",
       .b = "NAD83_Feet_2",
       .bigger_diff = true,
       .expected = false},
      {.a = "NAD83_IntlFeet",
       .b = "NAD83_Feet_2",
       .bigger_diff = true,
       .expected = false},
      {.a = "NAD83_Feet",
       .b = "NAD83_IntlFeet_2",
       .bigger_diff = true,
       .expected = false},
      {.a = "NAD83_Feet_2",
       .b = "NAD83_IntlFeet",
       .bigger_diff = true,
       .expected = false},
      // "19dd" -> "dd" on both sides vs digit-preceded.
      {.a = "WGS 1984", .b = "WGS84", .bigger_diff = true, .expected = true},
      {.a = "WGS84", .b = "WGS 1984", .bigger_diff = true, .expected = true},
      {.a = "21984", .b = "284", .bigger_diff = true, .expected = false},
      // "zone", "height", "north", "south" with biggerDifferencesAllowed
      // true/false.
      {.a = "UTM Zone 32",
       .b = "UTM 32",
       .bigger_diff = true,
       .expected = true},
      {.a = "UTM 32",
       .b = "UTM Zone 32",
       .bigger_diff = true,
       .expected = true},
      {.a = "UTM Zone 32",
       .b = "UTM 32",
       .bigger_diff = false,
       .expected = false},
      {.a = "EGM96 height",
       .b = "EGM96",
       .bigger_diff = true,
       .expected = true},
      {.a = "EGM96",
       .b = "EGM96 height",
       .bigger_diff = true,
       .expected = true},
      {.a = "EGM96 height",
       .b = "EGM96",
       .bigger_diff = false,
       .expected = false},
      {.a = "UTM 32North",
       .b = "UTM 32N",
       .bigger_diff = true,
       .expected = true},
      {.a = "UTM 32N",
       .b = "UTM 32North",
       .bigger_diff = true,
       .expected = true},
      {.a = "UTM 32South",
       .b = "UTM 32S",
       .bigger_diff = true,
       .expected = true},
      {.a = "UTM 32S",
       .b = "UTM 32 South",
       .bigger_diff = true,
       .expected = true},
      {.a = "UTM 32North",
       .b = "UTM 32N",
       .bigger_diff = false,
       .expected = false},
      // UTF-8 accents on either side, plus unmapped UTF-8 mismatch.
      {.a = "\xc3\xa1\xc3\xa4\xc4\x9b\xc3\xa8\xc3\xa9"
            "\xc3\xab\xc3\xad\xc3\xb4\xc3\xb6\xc3\xa7",
       .b = "aaeeeeiooc",
       .bigger_diff = true,
       .expected = true},
      {.a = "aaeeeeiooc",
       .b = "\xc3\xa1\xc3\xa4\xc4\x9b\xc3\xa8\xc3\xa9"
            "\xc3\xab\xc3\xad\xc3\xb4\xc3\xb6\xc3\xa7",
       .bigger_diff = true,
       .expected = true},
      {.a = "Espa\xc3\xb1"
            "a",
       .b = "Espana",
       .bigger_diff = true,
       .expected = false},
  };

  for (const auto& tc : kCases) {
    EXPECT_EQ(tc.expected,
              Identifier::isEquivalentName(tc.a, tc.b, tc.bigger_diff))
        << tc.a << " vs " << tc.b << " (bigger_diff=" << tc.bigger_diff << ")";
  }

  // Two-argument overload defaults biggerDifferencesAllowed to true.
  EXPECT_TRUE(Identifier::isEquivalentName("UTM Zone 32North", "UTM 32N"));
  EXPECT_FALSE(Identifier::isEquivalentName("UTM Zone 32North", "UTM 32S"));
}

TEST(MetadataTest, PositionalAccuracyCreateAndValue) {
  const auto acc = PositionalAccuracy::create("1.5 m");
  EXPECT_EQ("1.5 m", acc->value());

  const auto empty_acc = PositionalAccuracy::create("");
  EXPECT_THAT(empty_acc->value(), IsEmpty());
}

}  // namespace
}  // namespace proj
