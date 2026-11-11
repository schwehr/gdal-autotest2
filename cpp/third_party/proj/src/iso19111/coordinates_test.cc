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
// Unit tests for ISO-19111:2019 CoordinateMetadata
// (proj/src/iso19111/coordinates.cpp) and its C API interoperability. Note: in
// google3 default non-GCE builds, networking is disabled and proj.db is
// embedded in memory.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/coordinates.cpp

#include "third_party/proj4/proj/include/proj/coordinates.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinatesystem.hpp"
#include "third_party/proj4/proj/include/proj/crs.hpp"
#include "third_party/proj4/proj/include/proj/datum.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/nn.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

namespace common = ::osgeo::proj::common;
namespace coordinates = ::osgeo::proj::coordinates;
namespace crs = ::osgeo::proj::crs;
namespace cs = ::osgeo::proj::cs;
namespace datum = ::osgeo::proj::datum;
namespace io = ::osgeo::proj::io;
namespace util = ::osgeo::proj::util;

using ::testing::HasSubstr;

crs::GeographicCRSNNPtr MakeDynamicGeographicCrs() {
  auto frame = datum::DynamicGeodeticReferenceFrame::create(
      util::PropertyMap().set(common::IdentifiedObject::NAME_KEY,
                              "Synthetic Dynamic Frame"),
      datum::Ellipsoid::GRS1980, util::optional<std::string>(),
      datum::PrimeMeridian::GREENWICH,
      common::Measure(2010.0, common::UnitOfMeasure::YEAR),
      util::optional<std::string>());
  return crs::GeographicCRS::create(
      util::PropertyMap().set(common::IdentifiedObject::NAME_KEY,
                              "Synthetic Dynamic Geographic 2D"),
      frame,
      cs::EllipsoidalCS::createLatitudeLongitude(
          common::UnitOfMeasure::DEGREE));
}

crs::VerticalCRSNNPtr MakeStaticVerticalCrs() {
  auto vrf = datum::VerticalReferenceFrame::create(util::PropertyMap().set(
      common::IdentifiedObject::NAME_KEY, "Synthetic Static Vertical Frame"));
  return crs::VerticalCRS::create(
      util::PropertyMap().set(common::IdentifiedObject::NAME_KEY,
                              "Synthetic Static Vertical"),
      vrf,
      cs::VerticalCS::createGravityRelatedHeight(common::UnitOfMeasure::METRE));
}

crs::VerticalCRSNNPtr MakeDynamicVerticalCrs() {
  auto dvrf = datum::DynamicVerticalReferenceFrame::create(
      util::PropertyMap().set(common::IdentifiedObject::NAME_KEY,
                              "Synthetic Dynamic Vertical Frame"),
      util::optional<std::string>(),
      util::optional<datum::RealizationMethod>(
          datum::RealizationMethod::LEVELLING),
      common::Measure(2015.0, common::UnitOfMeasure::YEAR),
      util::optional<std::string>());
  return crs::VerticalCRS::create(
      util::PropertyMap().set(common::IdentifiedObject::NAME_KEY,
                              "Synthetic Dynamic Vertical"),
      dvrf,
      cs::VerticalCS::createGravityRelatedHeight(common::UnitOfMeasure::METRE));
}

int AxisCountOf(const crs::CRSNNPtr& crs_obj) {
  const auto* single = dynamic_cast<const crs::SingleCRS*>(crs_obj.get());
  return single != nullptr
             ? static_cast<int>(single->coordinateSystem()->axisList().size())
             : -1;
}

TEST(CoordinatesTest, CreateStaticCrsWithoutEpochSucceeds) {
  // Static CRSs and WGS84 ensembles (isDynamic(false) == false) accept
  // epochless CoordinateMetadata construction.
  const crs::CRSNNPtr kStaticCrsList[] = {
      crs::GeographicCRS::EPSG_4269,
      crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4979,
      MakeStaticVerticalCrs(),
  };

  for (const auto& crs_obj : kStaticCrsList) {
    ASSERT_FALSE(crs_obj->isDynamic(/*considerWGS84AsDynamic=*/false));
    auto cm = coordinates::CoordinateMetadata::create(crs_obj);
    EXPECT_EQ(crs_obj.get(), cm->crs().get());
    EXPECT_TRUE(cm->crs()->isEquivalentTo(crs_obj.get()));
    EXPECT_FALSE(cm->coordinateEpoch().has_value());
    EXPECT_TRUE(std::isnan(cm->coordinateEpochAsDecimalYear()));
    EXPECT_EQ(cm.get(), cm->shared_from_this().get());
  }
}

TEST(CoordinatesTest, CreateDynamicCrsWithoutEpochThrows) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  auto db_ctx = io::DatabaseContext::create(
      std::string(), std::vector<std::string>(), ctx.get());
  auto factory = io::AuthorityFactory::create(db_ctx, "EPSG");
  // EPSG:9000 is ITRF2014 (Geographic 2D, dynamic geodetic reference frame).
  auto itrf2014 = factory->createCoordinateReferenceSystem("9000");

  const crs::CRSNNPtr dynamic_crs_list[] = {
      MakeDynamicGeographicCrs(),
      MakeDynamicVerticalCrs(),
      itrf2014,
  };

  for (const auto& crs_obj : dynamic_crs_list) {
    ASSERT_TRUE(crs_obj->isDynamic(/*considerWGS84AsDynamic=*/false));
    EXPECT_THROW(coordinates::CoordinateMetadata::create(crs_obj),
                 util::Exception);
  }
}

TEST(CoordinatesTest, CreateWithEpochDynamicAndWgs84Branches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  auto db_ctx = io::DatabaseContext::create(
      std::string(), std::vector<std::string>(), ctx.get());
  auto factory = io::AuthorityFactory::create(db_ctx, "EPSG");
  auto itrf2014 = factory->createCoordinateReferenceSystem("9000");
  auto dyn_geog = MakeDynamicGeographicCrs();
  auto dyn_vert = MakeDynamicVerticalCrs();

  // Exercise both getRoundedEpochInDecimalYear() snapping (millennial values
  // within 1e-3 after year -> seconds -> year conversion) and unrounded
  // fractional passthrough.
  struct EpochCase {
    double input_year;
    double expected_year;
    double tolerance;
  };
  constexpr EpochCase kEpochCases[] = {
      {.input_year = 2010.0, .expected_year = 2010.0, .tolerance = 1e-12},
      {.input_year = 2018.1, .expected_year = 2018.1, .tolerance = 1e-12},
      {.input_year = 2025.375, .expected_year = 2025.375, .tolerance = 1e-12},
      {.input_year = 2020.123456,
       .expected_year = 2020.123456,
       .tolerance = 1e-10},
  };

  for (const auto& tc : kEpochCases) {
    auto cm = coordinates::CoordinateMetadata::create(dyn_geog, tc.input_year);
    ASSERT_TRUE(cm->coordinateEpoch().has_value());
    EXPECT_EQ(common::UnitOfMeasure::YEAR,
              cm->coordinateEpoch()->coordinateEpoch().unit());
    EXPECT_NEAR(tc.expected_year, cm->coordinateEpochAsDecimalYear(),
                tc.tolerance);
    EXPECT_EQ(cm.get(), cm->shared_from_this().get());
  }

  // Dynamic vertical CRS and database-loaded ITRF2014 succeed via 2-arg and
  // 3-arg overloads without requiring PointMotionOperation lookup.
  auto cm_vert = coordinates::CoordinateMetadata::create(dyn_vert, 2016.25);
  ASSERT_TRUE(cm_vert->coordinateEpoch().has_value());
  EXPECT_NEAR(2016.25, cm_vert->coordinateEpochAsDecimalYear(), 1e-12);

  auto cm_itrf_2arg = coordinates::CoordinateMetadata::create(itrf2014, 2024.5);
  ASSERT_TRUE(cm_itrf_2arg->coordinateEpoch().has_value());
  EXPECT_NEAR(2024.5, cm_itrf_2arg->coordinateEpochAsDecimalYear(), 1e-12);

  auto cm_itrf_3arg = coordinates::CoordinateMetadata::create(
      itrf2014, 2024.5, db_ctx.as_nullable());
  ASSERT_TRUE(cm_itrf_3arg->coordinateEpoch().has_value());
  EXPECT_NEAR(2024.5, cm_itrf_3arg->coordinateEpochAsDecimalYear(), 1e-12);

  // WGS84 ensemble CRSs return true from
  // isDynamic(considerWGS84AsDynamic=true), so providing an epoch succeeds even
  // with dbContext == nullptr.
  auto cm_wgs84_2d = coordinates::CoordinateMetadata::create(
      crs::GeographicCRS::EPSG_4326, 2021.75);
  EXPECT_TRUE(cm_wgs84_2d->coordinateEpoch().has_value());
  EXPECT_NEAR(2021.75, cm_wgs84_2d->coordinateEpochAsDecimalYear(), 1e-12);

  auto cm_wgs84_3d = coordinates::CoordinateMetadata::create(
      crs::GeographicCRS::EPSG_4979, 2021.75);
  EXPECT_TRUE(cm_wgs84_3d->coordinateEpoch().has_value());
  EXPECT_NEAR(2021.75, cm_wgs84_3d->coordinateEpochAsDecimalYear(), 1e-12);
}

TEST(CoordinatesTest, CreateWithEpochStaticAndPointMotionOperationBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  auto db_ctx = io::DatabaseContext::create(
      std::string(), std::vector<std::string>(), ctx.get());
  auto factory = io::AuthorityFactory::create(db_ctx, "EPSG");

  // 1. Static geodetic CRS (NAD83) has no PointMotionOperation: throws whether
  //    dbContext is null or non-null.
  EXPECT_THROW(coordinates::CoordinateMetadata::create(
                   crs::GeographicCRS::EPSG_4269, 2020.0),
               util::Exception);
  EXPECT_THROW(coordinates::CoordinateMetadata::create(
                   crs::GeographicCRS::EPSG_4269, 2020.0, db_ctx.as_nullable()),
               util::Exception);

  // 2. Static VerticalCRS has null extractGeodeticCRS(): throws even when
  //    dbContext is supplied.
  auto static_vert = MakeStaticVerticalCrs();
  EXPECT_EQ(nullptr, static_vert->extractGeodeticCRS());
  EXPECT_THROW(coordinates::CoordinateMetadata::create(static_vert, 2020.0,
                                                       db_ctx.as_nullable()),
               util::Exception);

  // 3. Static NAD83(CSRS)v7 (EPSG:8255) is static (isDynamic(true) == false),
  //    yet proj.db registers a PointMotionOperation (EPSG:9483 velocity grid)
  //    for it. Without dbContext it rejects epochs; with dbContext it succeeds.
  auto epsg_8255 = factory->createCoordinateReferenceSystem("8255");
  ASSERT_FALSE(epsg_8255->isDynamic(/*considerWGS84AsDynamic=*/true));
  EXPECT_THROW(coordinates::CoordinateMetadata::create(epsg_8255, 2022.5),
               util::Exception);

  auto cm_pmo = coordinates::CoordinateMetadata::create(epsg_8255, 2022.5,
                                                        db_ctx.as_nullable());
  ASSERT_TRUE(cm_pmo->coordinateEpoch().has_value());
  EXPECT_NEAR(2022.5, cm_pmo->coordinateEpochAsDecimalYear(), 1e-12);
  EXPECT_TRUE(cm_pmo->crs()->isEquivalentTo(epsg_8255.get()));
}

TEST(CoordinatesTest, PromoteTo3DPreservesEpochAndHandlesStaticAndDynamic) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  auto db_ctx = io::DatabaseContext::create(
      std::string(), std::vector<std::string>(), ctx.get());
  auto factory = io::AuthorityFactory::create(db_ctx, "EPSG");
  auto itrf2014_2d = factory->createCoordinateReferenceSystem("9000");
  auto itrf2014_3d = factory->createCoordinateReferenceSystem("7912");

  // 1. Static 2D CRS (epochless): promote with custom name and nullptr DB, then
  //    promote with embedded DB.
  auto cm_static =
      coordinates::CoordinateMetadata::create(crs::GeographicCRS::EPSG_4269);
  auto cm_static_3d_custom = cm_static->promoteTo3D("NAD83 3D Custom", nullptr);
  EXPECT_EQ(3, AxisCountOf(cm_static_3d_custom->crs()));
  EXPECT_EQ("NAD83 3D Custom", cm_static_3d_custom->crs()->nameStr());
  EXPECT_FALSE(cm_static_3d_custom->coordinateEpoch().has_value());
  EXPECT_TRUE(std::isnan(cm_static_3d_custom->coordinateEpochAsDecimalYear()));
  EXPECT_EQ(cm_static_3d_custom.get(),
            cm_static_3d_custom->shared_from_this().get());

  auto cm_static_3d_db =
      cm_static->promoteTo3D(std::string(), db_ctx.as_nullable());
  EXPECT_EQ(3, AxisCountOf(cm_static_3d_db->crs()));
  EXPECT_FALSE(cm_static_3d_db->coordinateEpoch().has_value());

  // 2. Dynamic 2D ITRF2014 (EPSG:9000) @ 2023.125 promotes via proj.db to
  //    ITRF2014 3D (EPSG:7912) while preserving the coordinate epoch.
  auto cm_dyn = coordinates::CoordinateMetadata::create(itrf2014_2d, 2023.125);
  auto cm_dyn_3d = cm_dyn->promoteTo3D(std::string(), db_ctx.as_nullable());
  EXPECT_EQ(3, AxisCountOf(cm_dyn_3d->crs()));
  EXPECT_TRUE(cm_dyn_3d->crs()->isEquivalentTo(itrf2014_3d.get()));
  ASSERT_TRUE(cm_dyn_3d->coordinateEpoch().has_value());
  EXPECT_NEAR(2023.125, cm_dyn_3d->coordinateEpochAsDecimalYear(), 1e-12);
  EXPECT_EQ(cm_dyn_3d.get(), cm_dyn_3d->shared_from_this().get());

  // 3. Programmatic dynamic 2D CRS promotes with custom name when dbContext is
  //    null, preserving its epoch.
  auto cm_prog = coordinates::CoordinateMetadata::create(
      MakeDynamicGeographicCrs(), 2019.5);
  auto cm_prog_3d = cm_prog->promoteTo3D("Dynamic 3D Custom", nullptr);
  EXPECT_EQ(3, AxisCountOf(cm_prog_3d->crs()));
  EXPECT_EQ("Dynamic 3D Custom", cm_prog_3d->crs()->nameStr());
  ASSERT_TRUE(cm_prog_3d->coordinateEpoch().has_value());
  EXPECT_NEAR(2019.5, cm_prog_3d->coordinateEpochAsDecimalYear(), 1e-12);

  // 4. Already-3D CRS preserves 3 axes and epoch.
  auto cm_already_3d = cm_dyn_3d->promoteTo3D(std::string(), nullptr);
  EXPECT_EQ(3, AxisCountOf(cm_already_3d->crs()));
  ASSERT_TRUE(cm_already_3d->coordinateEpoch().has_value());
  EXPECT_NEAR(2023.125, cm_already_3d->coordinateEpochAsDecimalYear(), 1e-12);

  // 5. Non-promotable static 1D VerticalCRS remains 1D without epoch.
  auto cm_vert_3d =
      coordinates::CoordinateMetadata::create(MakeStaticVerticalCrs())
          ->promoteTo3D("Ignored", nullptr);
  EXPECT_EQ(1, AxisCountOf(cm_vert_3d->crs()));
  EXPECT_FALSE(cm_vert_3d->coordinateEpoch().has_value());
}

TEST(CoordinatesTest, ExportToWktRejectsPre2019AndRoundTripsWkt2_2019) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  auto db_ctx = io::DatabaseContext::create(
      std::string(), std::vector<std::string>(), ctx.get());
  auto factory = io::AuthorityFactory::create(db_ctx, "EPSG");
  auto itrf2014 = factory->createCoordinateReferenceSystem("9000");

  auto cm_static =
      coordinates::CoordinateMetadata::create(crs::GeographicCRS::EPSG_4269);
  auto cm_dynamic = coordinates::CoordinateMetadata::create(itrf2014, 2024.5);

  // Pre-2019 WKT conventions must fail both the version != WKT2 branch (WKT1)
  // and the !use2019Keywords() branch (WKT2:2015 / WKT2_SIMPLIFIED).
  struct UnsupportedWktCase {
    io::WKTFormatter::Convention convention;
    std::string_view name;
  };
  constexpr UnsupportedWktCase kUnsupportedConventions[] = {
      {.convention = io::WKTFormatter::Convention::WKT1_GDAL,
       .name = "WKT1_GDAL"},
      {.convention = io::WKTFormatter::Convention::WKT1_ESRI,
       .name = "WKT1_ESRI"},
      {.convention = io::WKTFormatter::Convention::WKT2, .name = "WKT2_2015"},
      {.convention = io::WKTFormatter::Convention::WKT2_SIMPLIFIED,
       .name = "WKT2_SIMPLIFIED"},
  };

  for (const auto& tc : kUnsupportedConventions) {
    SCOPED_TRACE(std::string(tc.name));
    auto f_static =
        io::WKTFormatter::create(tc.convention, db_ctx.as_nullable());
    EXPECT_THROW(cm_static->exportToWKT(f_static.get()),
                 io::FormattingException);

    auto f_dyn = io::WKTFormatter::create(tc.convention, db_ctx.as_nullable());
    EXPECT_THROW(cm_dynamic->exportToWKT(f_dyn.get()), io::FormattingException);
  }

  // Supported WKT2:2019 and WKT2:2019_SIMPLIFIED round-trip cleanly via strict
  // WKTParser.
  constexpr io::WKTFormatter::Convention kSupportedConventions[] = {
      io::WKTFormatter::Convention::WKT2_2019,
      io::WKTFormatter::Convention::WKT2_2019_SIMPLIFIED,
  };

  for (const auto convention : kSupportedConventions) {
    // 1. Static CoordinateMetadata omits EPOCH[...].
    auto f_static = io::WKTFormatter::create(convention, db_ctx.as_nullable());
    f_static->setMultiLine(false);
    const std::string wkt_static = cm_static->exportToWKT(f_static.get());
    EXPECT_THAT(wkt_static, HasSubstr("COORDINATEMETADATA["));
    EXPECT_EQ(std::string::npos, wkt_static.find("EPOCH["));

    io::WKTParser parser_static;
    parser_static.attachDatabaseContext(db_ctx.as_nullable()).setStrict(true);
    auto parsed_static =
        util::nn_dynamic_pointer_cast<coordinates::CoordinateMetadata>(
            parser_static.createFromWKT(wkt_static));
    ASSERT_NE(nullptr, parsed_static.get());
    EXPECT_FALSE(parsed_static->coordinateEpoch().has_value());
    EXPECT_TRUE(parsed_static->crs()->isEquivalentTo(
        cm_static->crs().get(), util::IComparable::Criterion::EQUIVALENT,
        db_ctx.as_nullable()));
    EXPECT_TRUE(parser_static.warningList().empty());
    EXPECT_TRUE(parser_static.grammarErrorList().empty());

    // 2. Dynamic CoordinateMetadata emits EPOCH[2024.5].
    auto f_dyn = io::WKTFormatter::create(convention, db_ctx.as_nullable());
    const std::string wkt_dyn = cm_dynamic->exportToWKT(f_dyn.get());
    EXPECT_THAT(wkt_dyn, HasSubstr("COORDINATEMETADATA["));
    EXPECT_THAT(wkt_dyn, HasSubstr("EPOCH[2024.5]"));

    io::WKTParser parser_dyn;
    parser_dyn.attachDatabaseContext(db_ctx.as_nullable()).setStrict(true);
    auto parsed_dyn =
        util::nn_dynamic_pointer_cast<coordinates::CoordinateMetadata>(
            parser_dyn.createFromWKT(wkt_dyn));
    ASSERT_NE(nullptr, parsed_dyn.get());
    ASSERT_TRUE(parsed_dyn->coordinateEpoch().has_value());
    EXPECT_NEAR(2024.5, parsed_dyn->coordinateEpochAsDecimalYear(), 1e-12);
    EXPECT_TRUE(parsed_dyn->crs()->isEquivalentTo(
        cm_dynamic->crs().get(), util::IComparable::Criterion::EQUIVALENT,
        db_ctx.as_nullable()));
    EXPECT_TRUE(parser_dyn.warningList().empty());
    EXPECT_TRUE(parser_dyn.grammarErrorList().empty());
  }
}

TEST(CoordinatesTest, ExportToJsonOutputsAndRoundTripsWithAndWithoutEpoch) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  auto db_ctx = io::DatabaseContext::create(
      std::string(), std::vector<std::string>(), ctx.get());
  auto factory = io::AuthorityFactory::create(db_ctx, "EPSG");
  auto itrf2014 = factory->createCoordinateReferenceSystem("9000");

  // 1. Static CoordinateMetadata: includes "CoordinateMetadata" and "crs",
  // omits
  //    "coordinateEpoch".
  auto cm_static =
      coordinates::CoordinateMetadata::create(crs::GeographicCRS::EPSG_4269);
  auto json_fmt_static = io::JSONFormatter::create(db_ctx.as_nullable());
  const std::string json_static =
      cm_static->exportToJSON(json_fmt_static.get());
  EXPECT_THAT(json_static, HasSubstr("\"CoordinateMetadata\""));
  EXPECT_THAT(json_static, HasSubstr("\"crs\""));
  EXPECT_EQ(std::string::npos, json_static.find("\"coordinateEpoch\""));

  auto parsed_static =
      util::nn_dynamic_pointer_cast<coordinates::CoordinateMetadata>(
          io::createFromUserInput(json_static, db_ctx.as_nullable()));
  ASSERT_NE(nullptr, parsed_static.get());
  EXPECT_FALSE(parsed_static->coordinateEpoch().has_value());
  EXPECT_TRUE(parsed_static->crs()->isEquivalentTo(
      cm_static->crs().get(), util::IComparable::Criterion::EQUIVALENT,
      db_ctx.as_nullable()));

  // 2. Dynamic CoordinateMetadata: compact JSON includes "coordinateEpoch".
  auto cm_dyn = coordinates::CoordinateMetadata::create(itrf2014, 2024.125);
  auto json_fmt_dyn = io::JSONFormatter::create(db_ctx.as_nullable());
  json_fmt_dyn->setMultiLine(false);
  const std::string json_dyn = cm_dyn->exportToJSON(json_fmt_dyn.get());
  EXPECT_THAT(json_dyn, HasSubstr("\"CoordinateMetadata\""));
  EXPECT_THAT(json_dyn, HasSubstr("\"crs\""));
  EXPECT_THAT(json_dyn, HasSubstr("\"coordinateEpoch\":2024.125"));

  auto parsed_dyn =
      util::nn_dynamic_pointer_cast<coordinates::CoordinateMetadata>(
          io::createFromUserInput(json_dyn, db_ctx.as_nullable()));
  ASSERT_NE(nullptr, parsed_dyn.get());
  ASSERT_TRUE(parsed_dyn->coordinateEpoch().has_value());
  EXPECT_NEAR(2024.125, parsed_dyn->coordinateEpochAsDecimalYear(), 1e-12);
  EXPECT_TRUE(parsed_dyn->crs()->isEquivalentTo(
      cm_dyn->crs().get(), util::IComparable::Criterion::EQUIVALENT,
      db_ctx.as_nullable()));
}

TEST(CoordinatesTest, CApiAndDatabaseFactoryInterop) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  auto db_ctx = io::DatabaseContext::create(
      std::string(), std::vector<std::string>(), ctx.get());

  // 1. Embedded proj.db CoordinateMetadata lookup (NRCAN static frame with
  //    PointMotionOperation / registered epoch).
  auto nrcan_factory = io::AuthorityFactory::create(db_ctx, "NRCAN");
  auto nrcan_cm = nrcan_factory->createCoordinateMetadata(
      "NAD83_CSRS_1997_MTM10_CGVD2013_1997");
  ASSERT_TRUE(nrcan_cm->coordinateEpoch().has_value());
  EXPECT_NEAR(1997.0, nrcan_cm->coordinateEpochAsDecimalYear(), 1e-12);

  // 2. C API checks on dynamic CRS (EPSG:9000), static CRS without PMO
  //    (EPSG:4269), and static CRS with PMO (EPSG:8255).
  ScopedPj itrf(proj_create(ctx.get(), "EPSG:9000"));
  ScopedPj nad83(proj_create(ctx.get(), "EPSG:4269"));
  ScopedPj csrs_v7(proj_create(ctx.get(), "EPSG:8255"));
  ScopedPj non_crs(proj_create(ctx.get(), "+proj=merc"));
  ASSERT_NE(nullptr, itrf.get());
  ASSERT_NE(nullptr, nad83.get());
  ASSERT_NE(nullptr, csrs_v7.get());
  ASSERT_NE(nullptr, non_crs.get());

  EXPECT_EQ(1, proj_crs_is_dynamic(ctx.get(), itrf.get()));
  EXPECT_EQ(0, proj_crs_is_dynamic(ctx.get(), nad83.get()));
  EXPECT_EQ(0, proj_crs_has_point_motion_operation(ctx.get(), nad83.get()));
  EXPECT_EQ(1, proj_crs_has_point_motion_operation(ctx.get(), csrs_v7.get()));

  // 3. Create CoordinateMetadata for dynamic CRS & static PMO CRS, promote to
  //    3D, and serialize via proj_as_wkt / proj_as_projjson.
  ScopedPj cm_itrf(
      proj_coordinate_metadata_create(ctx.get(), itrf.get(), 2024.5));
  ASSERT_NE(nullptr, cm_itrf.get());
  EXPECT_EQ(PJ_TYPE_COORDINATE_METADATA, proj_get_type(cm_itrf.get()));
  EXPECT_NEAR(2024.5,
              proj_coordinate_metadata_get_epoch(ctx.get(), cm_itrf.get()),
              1e-12);

  const char* raw_wkt =
      proj_as_wkt(ctx.get(), cm_itrf.get(), PJ_WKT2_2019, nullptr);
  ASSERT_NE(nullptr, raw_wkt);
  const std::string wkt_2019(raw_wkt);
  EXPECT_THAT(wkt_2019, HasSubstr("COORDINATEMETADATA["));
  EXPECT_THAT(wkt_2019, HasSubstr("EPOCH[2024.5]"));

  const char* raw_json = proj_as_projjson(ctx.get(), cm_itrf.get(), nullptr);
  ASSERT_NE(nullptr, raw_json);
  const std::string json_str(raw_json);
  EXPECT_THAT(json_str, HasSubstr("\"CoordinateMetadata\""));
  EXPECT_THAT(json_str, HasSubstr("2024.5"));

  ScopedPj cm_itrf_3d(
      proj_crs_promote_to_3D(ctx.get(), nullptr, cm_itrf.get()));
  ASSERT_NE(nullptr, cm_itrf_3d.get());
  EXPECT_EQ(PJ_TYPE_COORDINATE_METADATA, proj_get_type(cm_itrf_3d.get()));
  EXPECT_NEAR(2024.5,
              proj_coordinate_metadata_get_epoch(ctx.get(), cm_itrf_3d.get()),
              1e-12);

  ScopedPj cm_pmo(
      proj_coordinate_metadata_create(ctx.get(), csrs_v7.get(), 2022.25));
  ASSERT_NE(nullptr, cm_pmo.get());
  EXPECT_EQ(PJ_TYPE_COORDINATE_METADATA, proj_get_type(cm_pmo.get()));
  EXPECT_NEAR(2022.25,
              proj_coordinate_metadata_get_epoch(ctx.get(), cm_pmo.get()),
              1e-12);

  // 4. Shorthand "CRS@epoch" string instantiation via proj_create.
  ScopedPj cm_shorthand(proj_create(ctx.get(), "ITRF2014@2025.25"));
  ASSERT_NE(nullptr, cm_shorthand.get());
  EXPECT_EQ(PJ_TYPE_COORDINATE_METADATA, proj_get_type(cm_shorthand.get()));
  EXPECT_NEAR(2025.25,
              proj_coordinate_metadata_get_epoch(ctx.get(), cm_shorthand.get()),
              1e-12);

  // 5. Negative C API & formatting paths (expected errors suppressed locally).
  {
    ScopedLogSuppressor silence(ctx.get());
    EXPECT_EQ(nullptr,
              proj_as_wkt(ctx.get(), cm_itrf.get(), PJ_WKT2_2015, nullptr));
    EXPECT_EQ(nullptr, proj_create(ctx.get(), "EPSG:4269@2020.0"));
    EXPECT_EQ(nullptr,
              proj_coordinate_metadata_create(ctx.get(), nad83.get(), 2020.0));
    EXPECT_EQ(nullptr, proj_coordinate_metadata_create(ctx.get(), non_crs.get(),
                                                       2020.0));
    EXPECT_TRUE(
        std::isnan(proj_coordinate_metadata_get_epoch(ctx.get(), itrf.get())));

    proj_errno_reset(itrf.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));
    EXPECT_EQ(nullptr,
              proj_coordinate_metadata_create(ctx.get(), nullptr, 2020.0));
    EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_context_errno(ctx.get()));

    proj_errno_reset(itrf.get());
    EXPECT_EQ(0, proj_context_errno(ctx.get()));
    EXPECT_TRUE(
        std::isnan(proj_coordinate_metadata_get_epoch(ctx.get(), nullptr)));
    EXPECT_EQ(PROJ_ERR_OTHER_API_MISUSE, proj_context_errno(ctx.get()));
  }
}

}  // namespace
}  // namespace proj
