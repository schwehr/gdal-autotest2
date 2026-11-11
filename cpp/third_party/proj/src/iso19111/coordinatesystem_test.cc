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
// Unit tests for ISO-19111 coordinate system classes, axes, meridians, code
// lists, WKT/JSON exports, and equivalence checks
// (proj/src/iso19111/coordinatesystem.cpp). Note: in google3 default non-GCE
// builds, proj.db is embedded in memory, network access is disabled, and
// ntv2_0.gsb / ca_nrc_ntv2_0.tif are skipped. Tests calling PROJ_INTERNAL C++
// methods require -DFROM_PROJ_CPP, -fexceptions, and linkstatic = True.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/coordinatesystem.cpp

#include "third_party/proj4/proj/include/proj/coordinatesystem.hpp"

#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/crs.hpp"
#include "third_party/proj4/proj/include/proj/datum.hpp"
#include "third_party/proj4/proj/include/proj/internal/coordinatesystem_internal.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

using ::osgeo::proj::common::Angle;
using ::osgeo::proj::common::IdentifiedObject;
using ::osgeo::proj::common::UnitOfMeasure;
using ::osgeo::proj::crs::EngineeringCRS;
using ::osgeo::proj::cs::AffineCS;
using ::osgeo::proj::cs::AxisAbbreviation;
using ::osgeo::proj::cs::AxisDirection;
using ::osgeo::proj::cs::AxisDirectionWKT1;
using ::osgeo::proj::cs::AxisName;
using ::osgeo::proj::cs::CartesianCS;
using ::osgeo::proj::cs::CoordinateSystemAxis;
using ::osgeo::proj::cs::DateTimeTemporalCS;
using ::osgeo::proj::cs::EllipsoidalCS;
using ::osgeo::proj::cs::InvalidCoordinateSystem;
using ::osgeo::proj::cs::Meridian;
using ::osgeo::proj::cs::OrdinalCS;
using ::osgeo::proj::cs::ParametricCS;
using ::osgeo::proj::cs::RangeMeaning;
using ::osgeo::proj::cs::SphericalCS;
using ::osgeo::proj::cs::TemporalCountCS;
using ::osgeo::proj::cs::TemporalMeasureCS;
using ::osgeo::proj::cs::VerticalCS;
using ::osgeo::proj::datum::EngineeringDatum;
using ::osgeo::proj::io::IJSONExportable;
using ::osgeo::proj::io::IWKTExportable;
using ::osgeo::proj::io::JSONFormatter;
using ::osgeo::proj::io::WKTFormatter;
using ::osgeo::proj::metadata::Identifier;
using ::osgeo::proj::util::Exception;
using ::osgeo::proj::util::IComparable;
using ::osgeo::proj::util::optional;
using ::osgeo::proj::util::PropertyMap;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

std::string ExportWkt(const IWKTExportable& obj,
                      WKTFormatter::Convention convention,
                      bool output_id = true) {
  auto fmt = WKTFormatter::create(convention);
  fmt->setMultiLine(false);
  if (!output_id) {
    fmt->setOutputId(false);
  }
  return obj.exportToWKT(fmt.get());
}

std::string ExportJson(const IJSONExportable& obj) {
  auto fmt = JSONFormatter::create();
  fmt->setMultiLine(false).setSchema("");
  return obj.exportToJSON(fmt.get());
}

PropertyMap MakeNameAndEpsgId(std::string_view name, std::string_view code) {
  return PropertyMap()
      .set(IdentifiedObject::NAME_KEY, std::string(name))
      .set(IdentifiedObject::IDENTIFIERS_KEY,
           Identifier::create(
               std::string(code),
               PropertyMap().set(Identifier::CODESPACE_KEY, Identifier::EPSG)));
}

TEST(CoordinateSystemTest, MeridianCreateAccessorsWktAndJsonExports) {
  const auto deg_meridian = Meridian::create(Angle(90.0));
  EXPECT_DOUBLE_EQ(90.0, deg_meridian->longitude().value());
  EXPECT_EQ(UnitOfMeasure::DEGREE, deg_meridian->longitude().unit());

  // WKT2 with ID enabled vs suppressed.
  const std::string wkt_with_id =
      ExportWkt(*deg_meridian, WKTFormatter::Convention::WKT2, true);
  EXPECT_THAT(wkt_with_id,
              HasSubstr("MERIDIAN[90,ANGLEUNIT[\"degree\",0.01745329251994"));
  EXPECT_THAT(wkt_with_id, HasSubstr("ID[\"EPSG\",9122]"));

  const std::string wkt_no_id =
      ExportWkt(*deg_meridian, WKTFormatter::Convention::WKT2, false);
  EXPECT_THAT(wkt_no_id,
              HasSubstr("MERIDIAN[90,ANGLEUNIT[\"degree\",0.01745329251994"));
  EXPECT_THAT(wkt_no_id, Not(HasSubstr("ID[")));

  // JSON: degree unit writes a bare scalar; non-degree writes {value, unit}.
  EXPECT_EQ("{\"type\":\"Meridian\",\"longitude\":90}",
            ExportJson(*deg_meridian));

  const auto grad_meridian =
      Meridian::create(Angle(100.0, UnitOfMeasure::GRAD));
  EXPECT_DOUBLE_EQ(100.0, grad_meridian->longitude().value());
  EXPECT_EQ(UnitOfMeasure::GRAD, grad_meridian->longitude().unit());
  const std::string grad_json = ExportJson(*grad_meridian);
  EXPECT_THAT(grad_json, HasSubstr("\"value\":100"));
  EXPECT_THAT(grad_json, HasSubstr("\"type\":\"AngularUnit\""));
  EXPECT_THAT(grad_json, HasSubstr("\"name\":\"grad\""));

  // Exporting a Meridian embedded in an identified Axis disables outputId()
  // inside Meridian::_exportToJSON.
  const auto axis_with_id = CoordinateSystemAxis::create(
      MakeNameAndEpsgId("Easting", "1"), "E", AxisDirection::SOUTH,
      UnitOfMeasure::METRE, deg_meridian.as_nullable());
  const std::string axis_json = ExportJson(*axis_with_id);
  EXPECT_THAT(axis_json, HasSubstr("\"meridian\":{\"longitude\":90}"));
}

TEST(CoordinateSystemTest, AxisCreateAccessorsAlterUnitAndNormalizeName) {
  const auto meridian = Meridian::create(Angle(45.0));
  const auto axis_basic = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Easting"), "E",
      AxisDirection::EAST, UnitOfMeasure::METRE, meridian.as_nullable());
  EXPECT_EQ("Easting", axis_basic->nameStr());
  EXPECT_EQ("E", axis_basic->abbreviation());
  EXPECT_EQ(AxisDirection::EAST, axis_basic->direction());
  EXPECT_EQ(UnitOfMeasure::METRE, axis_basic->unit());
  EXPECT_FALSE(axis_basic->minimumValue().has_value());
  EXPECT_FALSE(axis_basic->maximumValue().has_value());
  EXPECT_FALSE(axis_basic->rangeMeaning().has_value());
  ASSERT_NE(nullptr, axis_basic->meridian().get());
  EXPECT_DOUBLE_EQ(45.0, axis_basic->meridian()->longitude().value());

  const auto altered = axis_basic->alterUnit(UnitOfMeasure::FOOT);
  EXPECT_EQ("Easting", altered->nameStr());
  EXPECT_EQ("E", altered->abbreviation());
  EXPECT_EQ(AxisDirection::EAST, altered->direction());
  EXPECT_EQ(UnitOfMeasure::FOOT, altered->unit());
  ASSERT_NE(nullptr, altered->meridian().get());
  EXPECT_DOUBLE_EQ(45.0, altered->meridian()->longitude().value());

  const auto axis_full = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, AxisName::Longitude),
      AxisAbbreviation::lon, AxisDirection::EAST, UnitOfMeasure::DEGREE,
      optional<double>(-180.0), optional<double>(180.0),
      optional<RangeMeaning>(RangeMeaning::WRAPAROUND), nullptr);
  ASSERT_TRUE(axis_full->minimumValue().has_value());
  ASSERT_TRUE(axis_full->maximumValue().has_value());
  ASSERT_TRUE(axis_full->rangeMeaning().has_value());
  EXPECT_DOUBLE_EQ(-180.0, *axis_full->minimumValue());
  EXPECT_DOUBLE_EQ(180.0, *axis_full->maximumValue());
  EXPECT_EQ(RangeMeaning::WRAPAROUND, *axis_full->rangeMeaning());
  EXPECT_EQ(nullptr, axis_full->meridian().get());

  const auto lat_north =
      CoordinateSystemAxis::createLAT_NORTH(UnitOfMeasure::DEGREE);
  EXPECT_EQ(AxisName::Latitude, lat_north->nameStr());
  EXPECT_EQ(AxisAbbreviation::lat, lat_north->abbreviation());
  EXPECT_EQ(AxisDirection::NORTH, lat_north->direction());
  EXPECT_EQ(UnitOfMeasure::DEGREE, lat_north->unit());

  const auto lon_east =
      CoordinateSystemAxis::createLONG_EAST(UnitOfMeasure::DEGREE);
  EXPECT_EQ(AxisName::Longitude, lon_east->nameStr());
  EXPECT_EQ(AxisAbbreviation::lon, lon_east->abbreviation());
  EXPECT_EQ(AxisDirection::EAST, lon_east->direction());
  EXPECT_EQ(UnitOfMeasure::DEGREE, lon_east->unit());

  struct NormalizeCase {
    std::string_view input;
    std::string_view expected;
  };
  constexpr NormalizeCase kNormalizeCases[] = {
      {.input = "", .expected = ""},
      {.input = "longitude", .expected = "Longitude"},
      {.input = "geodetic latitude", .expected = "Geodetic latitude"},
      {.input = "Easting", .expected = "Easting"},
      {.input = "x", .expected = "X"},
  };
  for (const auto& tc : kNormalizeCases) {
    EXPECT_EQ(tc.expected,
              CoordinateSystemAxis::normalizeAxisName(std::string(tc.input)))
        << tc.input;
  }
}

TEST(CoordinateSystemTest, AxisExportToWktCoversWkt1Wkt2AndWkt22019Branches) {
  // WKT2 (2015) branches: name + abbrev, Latitude without abbrev, empty name,
  // empty abbrev, meridian, UnitOfMeasure::NONE, and outputId toggling.
  const auto depth_axis =
      CoordinateSystemAxis::create(MakeNameAndEpsgId("Depth", "10"), "D",
                                   AxisDirection::DOWN, UnitOfMeasure::METRE);
  const std::string depth_wkt2 =
      ExportWkt(*depth_axis, WKTFormatter::Convention::WKT2, true);
  EXPECT_THAT(depth_wkt2, HasSubstr("AXIS[\"depth (D)\",down,"));
  EXPECT_THAT(depth_wkt2, HasSubstr("LENGTHUNIT[\"metre\",1]"));
  EXPECT_THAT(depth_wkt2, HasSubstr("ID[\"EPSG\",10]"));

  const std::string depth_wkt2_no_id =
      ExportWkt(*depth_axis, WKTFormatter::Convention::WKT2, false);
  EXPECT_THAT(depth_wkt2_no_id, Not(HasSubstr("ID[\"EPSG\",10]")));

  const auto lat_axis =
      CoordinateSystemAxis::createLAT_NORTH(UnitOfMeasure::DEGREE);
  EXPECT_THAT(ExportWkt(*lat_axis, WKTFormatter::Convention::WKT2),
              HasSubstr("AXIS[\"latitude\",north,"));

  const auto empty_name_axis = CoordinateSystemAxis::create(
      PropertyMap(), "u", AxisDirection::EAST, UnitOfMeasure::METRE);
  EXPECT_THAT(ExportWkt(*empty_name_axis, WKTFormatter::Convention::WKT2),
              HasSubstr("AXIS[\"(u)\",east,"));

  const auto empty_abbrev_axis = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Depth"), "",
      AxisDirection::DOWN, UnitOfMeasure::NONE,
      Meridian::create(Angle(30.0)).as_nullable());
  const std::string empty_abbrev_wkt2 =
      ExportWkt(*empty_abbrev_axis, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(empty_abbrev_wkt2, HasSubstr("AXIS[\"depth\",down,MERIDIAN[30,"));
  EXPECT_THAT(empty_abbrev_wkt2, Not(HasSubstr(",UNIT[")));

  // WKT2:2019 AXISMINVALUE, AXISMAXVALUE, and RANGEMEANING combinations.
  const auto axis_min_max_range = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, AxisName::Longitude),
      AxisAbbreviation::lon, AxisDirection::EAST, UnitOfMeasure::DEGREE,
      optional<double>(-180.0), optional<double>(180.0),
      optional<RangeMeaning>(RangeMeaning::WRAPAROUND));
  const std::string wkt2_2019 =
      ExportWkt(*axis_min_max_range, WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(wkt2_2019, HasSubstr("AXISMINVALUE[-180]"));
  EXPECT_THAT(wkt2_2019, HasSubstr("AXISMAXVALUE[180]"));
  EXPECT_THAT(wkt2_2019, HasSubstr("RANGEMEANING[wraparound]"));

  // WKT2:2015 must suppress 2019-only range metadata even when present.
  const std::string wkt2_2015 =
      ExportWkt(*axis_min_max_range, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(wkt2_2015, Not(HasSubstr("AXISMINVALUE")));
  EXPECT_THAT(wkt2_2015, Not(HasSubstr("AXISMAXVALUE")));
  EXPECT_THAT(wkt2_2015, Not(HasSubstr("RANGEMEANING")));

  const auto axis_min_only = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Height"), "H",
      AxisDirection::UP, UnitOfMeasure::METRE, optional<double>(0.0),
      optional<double>(), optional<RangeMeaning>());
  const std::string wkt_min_only =
      ExportWkt(*axis_min_only, WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(wkt_min_only, HasSubstr("AXISMINVALUE[0]"));
  EXPECT_THAT(wkt_min_only, Not(HasSubstr("AXISMAXVALUE")));
  EXPECT_THAT(wkt_min_only, Not(HasSubstr("RANGEMEANING")));

  const auto axis_max_with_rm_no_min = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Height"), "H",
      AxisDirection::UP, UnitOfMeasure::METRE, optional<double>(),
      optional<double>(1000.0), optional<RangeMeaning>(RangeMeaning::EXACT));
  const std::string wkt_max_only =
      ExportWkt(*axis_max_with_rm_no_min, WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(wkt_max_only, Not(HasSubstr("AXISMINVALUE")));
  EXPECT_THAT(wkt_max_only, HasSubstr("AXISMAXVALUE[1000]"));
  EXPECT_THAT(wkt_max_only, Not(HasSubstr("RANGEMEANING")));

  const auto axis_min_max_no_rm = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Height"), "H",
      AxisDirection::UP, UnitOfMeasure::METRE, optional<double>(0.0),
      optional<double>(1000.0), optional<RangeMeaning>());
  const std::string wkt_no_rm =
      ExportWkt(*axis_min_max_no_rm, WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(wkt_no_rm, HasSubstr("AXISMINVALUE[0]"));
  EXPECT_THAT(wkt_no_rm, HasSubstr("AXISMAXVALUE[1000]"));
  EXPECT_THAT(wkt_no_rm, Not(HasSubstr("RANGEMEANING")));

  // WKT1_GDAL branches: "Geodetic latitude" -> "Latitude",
  // "Geodetic longitude" -> "Longitude", other name kept verbatim, GEOCENTRIC_Z
  // mapped to NORTH, unmapped directions mapped to OTHER, meridian skipped.
  const auto geod_lat = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Geodetic latitude"), "lat",
      AxisDirection::NORTH, UnitOfMeasure::DEGREE);
  EXPECT_THAT(ExportWkt(*geod_lat, WKTFormatter::Convention::WKT1_GDAL),
              HasSubstr("AXIS[\"Latitude\",NORTH,UNIT[\"degree\","));

  const auto geod_lon = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Geodetic longitude"),
      "lon", AxisDirection::EAST, UnitOfMeasure::DEGREE);
  EXPECT_THAT(ExportWkt(*geod_lon, WKTFormatter::Convention::WKT1_GDAL),
              HasSubstr("AXIS[\"Longitude\",EAST,UNIT[\"degree\","));

  const auto geoc_z = CoordinateSystemAxis::create(
      MakeNameAndEpsgId("Geocentric Z", "117"), "Z",
      AxisDirection::GEOCENTRIC_Z, UnitOfMeasure::METRE,
      Meridian::create(Angle(0.0)).as_nullable());
  const std::string geoc_z_wkt1 =
      ExportWkt(*geoc_z, WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THAT(geoc_z_wkt1, HasSubstr("AXIS[\"Geocentric Z\",NORTH,"));
  EXPECT_THAT(geoc_z_wkt1, HasSubstr("AUTHORITY[\"EPSG\",\"117\"]"));
  EXPECT_THAT(geoc_z_wkt1, Not(HasSubstr("MERIDIAN")));

  const auto geoc_x = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Geocentric X"), "X",
      AxisDirection::GEOCENTRIC_X, UnitOfMeasure::METRE);
  EXPECT_THAT(ExportWkt(*geoc_x, WKTFormatter::Convention::WKT1_GDAL),
              HasSubstr("AXIS[\"Geocentric X\",OTHER,UNIT[\"metre\","));
}

TEST(CoordinateSystemTest, AxisExportToJsonCoversUnitsMetadataAndIdBranches) {
  const auto metre_axis =
      CoordinateSystemAxis::create(MakeNameAndEpsgId("Northing", "2"), "N",
                                   AxisDirection::NORTH, UnitOfMeasure::METRE);
  const std::string metre_json = ExportJson(*metre_axis);
  EXPECT_THAT(metre_json, HasSubstr("\"unit\":\"metre\""));
  EXPECT_THAT(metre_json,
              HasSubstr("\"id\":{\"authority\":\"EPSG\",\"code\":2}"));

  const auto degree_axis = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Latitude"), "lat",
      AxisDirection::NORTH, UnitOfMeasure::DEGREE, optional<double>(-90.0),
      optional<double>(90.0), optional<RangeMeaning>(RangeMeaning::EXACT),
      Meridian::create(Angle(10.0)).as_nullable());
  const std::string degree_json = ExportJson(*degree_axis);
  EXPECT_THAT(degree_json, HasSubstr("\"unit\":\"degree\""));
  EXPECT_THAT(degree_json, HasSubstr("\"meridian\":{\"longitude\":10}"));
  EXPECT_THAT(degree_json, HasSubstr("\"minimum_value\":-90"));
  EXPECT_THAT(degree_json, HasSubstr("\"maximum_value\":90"));
  EXPECT_THAT(degree_json, HasSubstr("\"range_meaning\":\"exact\""));

  const auto foot_axis = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Easting"), "E",
      AxisDirection::EAST, UnitOfMeasure::FOOT, optional<double>(),
      optional<double>(1000.0), optional<RangeMeaning>(RangeMeaning::EXACT));
  const std::string foot_json = ExportJson(*foot_axis);
  EXPECT_THAT(foot_json,
              HasSubstr("\"unit\":{\"type\":\"LinearUnit\",\"name\":\"foot\""));
  EXPECT_THAT(foot_json, HasSubstr("\"maximum_value\":1000"));
  EXPECT_THAT(foot_json, Not(HasSubstr("\"minimum_value\"")));
  EXPECT_THAT(foot_json, Not(HasSubstr("\"range_meaning\"")));

  const auto none_unit_axis = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Index"), "i",
      AxisDirection::COLUMN_POSITIVE, UnitOfMeasure::NONE,
      optional<double>(1.0), optional<double>(10.0), optional<RangeMeaning>());
  const std::string none_json = ExportJson(*none_unit_axis);
  EXPECT_THAT(none_json, Not(HasSubstr("\"unit\"")));
  EXPECT_THAT(none_json, HasSubstr("\"minimum_value\":1"));
  EXPECT_THAT(none_json, HasSubstr("\"maximum_value\":10"));
  EXPECT_THAT(none_json, Not(HasSubstr("\"range_meaning\"")));
}

TEST(CoordinateSystemTest, AxisIsEquivalentToStrictAndEquivalentBranches) {
  const auto east_m_1 = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Easting"), "E",
      AxisDirection::EAST, UnitOfMeasure::METRE);
  const auto east_m_same = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Easting"), "E",
      AxisDirection::EAST, UnitOfMeasure::METRE);
  const auto east_m_diff_abbrev = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Easting"), "X",
      AxisDirection::EAST, UnitOfMeasure::METRE);
  const auto east_m_diff_name = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Local East"), "E",
      AxisDirection::EAST, UnitOfMeasure::METRE);
  const auto north_m = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Easting"), "E",
      AxisDirection::NORTH, UnitOfMeasure::METRE);
  const auto east_ft = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Easting"), "E",
      AxisDirection::EAST, UnitOfMeasure::FOOT);
  const auto meridian = Meridian::create(Angle(0.0));

  EXPECT_TRUE(
      east_m_1->isEquivalentTo(east_m_1.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      east_m_1->isEquivalentTo(nullptr, IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      east_m_1->isEquivalentTo(meridian.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(east_m_1->isEquivalentTo(north_m.get(),
                                        IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(east_m_1->isEquivalentTo(east_ft.get(),
                                        IComparable::Criterion::EQUIVALENT));

  EXPECT_FALSE(east_m_1->isEquivalentTo(east_m_diff_name.get(),
                                        IComparable::Criterion::STRICT));
  EXPECT_TRUE(east_m_1->isEquivalentTo(east_m_diff_name.get(),
                                       IComparable::Criterion::EQUIVALENT));

  EXPECT_FALSE(east_m_1->isEquivalentTo(east_m_diff_abbrev.get(),
                                        IComparable::Criterion::STRICT));
  EXPECT_TRUE(east_m_1->isEquivalentTo(east_m_diff_abbrev.get(),
                                       IComparable::Criterion::EQUIVALENT));

  EXPECT_TRUE(east_m_1->isEquivalentTo(east_m_same.get(),
                                       IComparable::Criterion::STRICT));
}

TEST(CoordinateSystemTest, EllipsoidalCSFactoriesAxisOrderAndUnitAlteration) {
  const auto lat_lon =
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE);
  EXPECT_EQ(EllipsoidalCS::AxisOrder::LAT_NORTH_LONG_EAST,
            lat_lon->axisOrder());
  EXPECT_EQ(2U, lat_lon->axisList().size());

  const auto lat_lon_h =
      EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
          UnitOfMeasure::DEGREE, UnitOfMeasure::METRE);
  EXPECT_EQ(EllipsoidalCS::AxisOrder::LAT_NORTH_LONG_EAST_HEIGHT_UP,
            lat_lon_h->axisOrder());
  EXPECT_EQ(3U, lat_lon_h->axisList().size());

  const auto lon_lat =
      EllipsoidalCS::createLongitudeLatitude(UnitOfMeasure::DEGREE);
  EXPECT_EQ(EllipsoidalCS::AxisOrder::LONG_EAST_LAT_NORTH,
            lon_lat->axisOrder());

  const auto lon_lat_h =
      EllipsoidalCS::createLongitudeLatitudeEllipsoidalHeight(
          UnitOfMeasure::DEGREE, UnitOfMeasure::METRE);
  EXPECT_EQ(EllipsoidalCS::AxisOrder::LONG_EAST_LAT_NORTH_HEIGHT_UP,
            lon_lat_h->axisOrder());

  // AxisOrder::OTHER branches: 3D North/East/Down, 3D East/North/Down, and
  // 2D South/West.
  const auto down_axis = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Ellipsoidal depth"), "d",
      AxisDirection::DOWN, UnitOfMeasure::METRE);
  const auto lat_lon_down = EllipsoidalCS::create(
      PropertyMap(),
      CoordinateSystemAxis::createLAT_NORTH(UnitOfMeasure::DEGREE),
      CoordinateSystemAxis::createLONG_EAST(UnitOfMeasure::DEGREE), down_axis);
  EXPECT_EQ(EllipsoidalCS::AxisOrder::OTHER, lat_lon_down->axisOrder());

  const auto lon_lat_down = EllipsoidalCS::create(
      PropertyMap(),
      CoordinateSystemAxis::createLONG_EAST(UnitOfMeasure::DEGREE),
      CoordinateSystemAxis::createLAT_NORTH(UnitOfMeasure::DEGREE), down_axis);
  EXPECT_EQ(EllipsoidalCS::AxisOrder::OTHER, lon_lat_down->axisOrder());

  const auto south_west = EllipsoidalCS::create(
      PropertyMap(),
      CoordinateSystemAxis::create(
          PropertyMap().set(IdentifiedObject::NAME_KEY, "Southing"), "S",
          AxisDirection::SOUTH, UnitOfMeasure::DEGREE),
      CoordinateSystemAxis::create(
          PropertyMap().set(IdentifiedObject::NAME_KEY, "Westing"), "W",
          AxisDirection::WEST, UnitOfMeasure::DEGREE));
  EXPECT_EQ(EllipsoidalCS::AxisOrder::OTHER, south_west->axisOrder());

  const auto north_west = EllipsoidalCS::create(
      PropertyMap(),
      CoordinateSystemAxis::createLAT_NORTH(UnitOfMeasure::DEGREE),
      CoordinateSystemAxis::create(
          PropertyMap().set(IdentifiedObject::NAME_KEY, "Westing"), "W",
          AxisDirection::WEST, UnitOfMeasure::DEGREE));
  EXPECT_EQ(EllipsoidalCS::AxisOrder::OTHER, north_west->axisOrder());

  // alterAngularUnit and alterLinearUnit for 2D and 3D EllipsoidalCS.
  const auto lat_lon_grad = lat_lon->alterAngularUnit(UnitOfMeasure::GRAD);
  EXPECT_EQ(UnitOfMeasure::GRAD, lat_lon_grad->axisList()[0]->unit());
  EXPECT_EQ(UnitOfMeasure::GRAD, lat_lon_grad->axisList()[1]->unit());

  const auto lat_lon_h_grad = lat_lon_h->alterAngularUnit(UnitOfMeasure::GRAD);
  EXPECT_EQ(UnitOfMeasure::GRAD, lat_lon_h_grad->axisList()[0]->unit());
  EXPECT_EQ(UnitOfMeasure::GRAD, lat_lon_h_grad->axisList()[1]->unit());
  EXPECT_EQ(UnitOfMeasure::METRE, lat_lon_h_grad->axisList()[2]->unit());

  const auto lat_lon_lin_unchanged =
      lat_lon->alterLinearUnit(UnitOfMeasure::FOOT);
  EXPECT_EQ(2U, lat_lon_lin_unchanged->axisList().size());
  EXPECT_EQ(UnitOfMeasure::DEGREE,
            lat_lon_lin_unchanged->axisList()[0]->unit());

  const auto lat_lon_h_ft = lat_lon_h->alterLinearUnit(UnitOfMeasure::FOOT);
  EXPECT_EQ(UnitOfMeasure::DEGREE, lat_lon_h_ft->axisList()[0]->unit());
  EXPECT_EQ(UnitOfMeasure::FOOT, lat_lon_h_ft->axisList()[2]->unit());

  // disableAbbrev == true only for (Latitude, Longitude, Ellipsoidal height).
  const std::string wkt_lat_lon_h =
      ExportWkt(*lat_lon_h, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(wkt_lat_lon_h, HasSubstr("CS[ellipsoidal,3]"));
  EXPECT_THAT(wkt_lat_lon_h,
              HasSubstr("AXIS[\"ellipsoidal height\",up,ORDER[3]"));
  EXPECT_THAT(wkt_lat_lon_h, Not(HasSubstr("ellipsoidal height (h)")));

  const std::string wkt_lon_lat_h =
      ExportWkt(*lon_lat_h, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(wkt_lon_lat_h,
              HasSubstr("AXIS[\"ellipsoidal height (h)\",up,ORDER[3]"));

  // Non-matching 2nd/3rd axis names keep disableAbbrev == false.
  const auto custom_3d_mid = EllipsoidalCS::create(
      PropertyMap(),
      CoordinateSystemAxis::createLAT_NORTH(UnitOfMeasure::DEGREE),
      CoordinateSystemAxis::create(
          PropertyMap().set(IdentifiedObject::NAME_KEY, "CustomLon"), "clon",
          AxisDirection::EAST, UnitOfMeasure::DEGREE),
      CoordinateSystemAxis::create(
          PropertyMap().set(IdentifiedObject::NAME_KEY,
                            AxisName::Ellipsoidal_height),
          AxisAbbreviation::h, AxisDirection::UP, UnitOfMeasure::METRE));
  EXPECT_THAT(ExportWkt(*custom_3d_mid, WKTFormatter::Convention::WKT2),
              HasSubstr("AXIS[\"ellipsoidal height (h)\",up,ORDER[3]"));
  EXPECT_THAT(ExportWkt(*lat_lon_down, WKTFormatter::Convention::WKT2),
              HasSubstr("AXIS[\"ellipsoidal depth (d)\",down,ORDER[3]"));

  // WKT2_SIMPLIFIED: same units emit single CS-level UNIT; mixed units emit
  // per-axis UNIT.
  const std::string wkt2_simp_2d =
      ExportWkt(*lat_lon, WKTFormatter::Convention::WKT2_SIMPLIFIED);
  EXPECT_THAT(wkt2_simp_2d, HasSubstr(",UNIT[\"degree\","));

  const std::string wkt2_simp_3d =
      ExportWkt(*lat_lon_h, WKTFormatter::Convention::WKT2_SIMPLIFIED);
  EXPECT_THAT(wkt2_simp_3d, HasSubstr("UNIT[\"degree\","));
  EXPECT_THAT(wkt2_simp_3d, HasSubstr("UNIT[\"metre\",1"));
}

TEST(CoordinateSystemTest, CartesianCSFactoriesValidationAndUnitAlteration) {
  const auto en = CartesianCS::createEastingNorthing(UnitOfMeasure::METRE);
  const std::string en_wkt2 = ExportWkt(*en, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(en_wkt2, HasSubstr("CS[Cartesian,2]"));
  EXPECT_THAT(en_wkt2, HasSubstr("AXIS[\"(E)\",east,ORDER[1]"));
  EXPECT_THAT(en_wkt2, HasSubstr("AXIS[\"(N)\",north,ORDER[2]"));

  const auto ne = CartesianCS::createNorthingEasting(UnitOfMeasure::METRE);
  const std::string ne_wkt2 = ExportWkt(*ne, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(ne_wkt2, HasSubstr("AXIS[\"northing (N)\",north,ORDER[1]"));
  EXPECT_THAT(ne_wkt2, HasSubstr("AXIS[\"easting (E)\",east,ORDER[2]"));

  const auto ws = CartesianCS::createWestingSouthing(UnitOfMeasure::METRE);
  EXPECT_EQ("Y", ws->axisList()[0]->abbreviation());
  EXPECT_EQ(AxisDirection::WEST, ws->axisList()[0]->direction());
  EXPECT_EQ("X", ws->axisList()[1]->abbreviation());
  EXPECT_EQ(AxisDirection::SOUTH, ws->axisList()[1]->direction());
  const std::string ws_wkt2 = ExportWkt(*ws, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(ws_wkt2, HasSubstr("AXIS[\"easting (Y)\",west,ORDER[1]"));
  EXPECT_THAT(ws_wkt2, HasSubstr("AXIS[\"northing (X)\",south,ORDER[2]"));

  const auto north_pole = CartesianCS::createNorthPoleEastingSouthNorthingSouth(
      UnitOfMeasure::METRE);
  const std::string np_wkt2 =
      ExportWkt(*north_pole, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(np_wkt2, HasSubstr("MERIDIAN[90,"));
  EXPECT_THAT(np_wkt2, HasSubstr("MERIDIAN[180,"));

  const auto south_pole = CartesianCS::createSouthPoleEastingNorthNorthingNorth(
      UnitOfMeasure::METRE);
  const std::string sp_wkt2 =
      ExportWkt(*south_pole, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(sp_wkt2, HasSubstr("MERIDIAN[90,"));
  EXPECT_THAT(sp_wkt2, HasSubstr("MERIDIAN[0,"));

  const auto geocentric = CartesianCS::createGeocentric(UnitOfMeasure::METRE);
  EXPECT_EQ(3U, geocentric->axisList().size());
  const std::string geoc_wkt2 =
      ExportWkt(*geocentric, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(geoc_wkt2, HasSubstr("CS[Cartesian,3]"));
  EXPECT_THAT(geoc_wkt2, HasSubstr("AXIS[\"(X)\",geocentricX,ORDER[1]"));
  EXPECT_THAT(geoc_wkt2, HasSubstr("AXIS[\"(Y)\",geocentricY,ORDER[2]"));
  EXPECT_THAT(geoc_wkt2, HasSubstr("AXIS[\"(Z)\",geocentricZ,ORDER[3]"));

  // alterUnit for 2-axis and 3-axis CartesianCS.
  const auto en_ft = en->alterUnit(UnitOfMeasure::FOOT);
  EXPECT_EQ(UnitOfMeasure::FOOT, en_ft->axisList()[0]->unit());
  EXPECT_EQ(UnitOfMeasure::FOOT, en_ft->axisList()[1]->unit());

  const auto geoc_ft = geocentric->alterUnit(UnitOfMeasure::FOOT);
  EXPECT_EQ(UnitOfMeasure::FOOT, geoc_ft->axisList()[0]->unit());
  EXPECT_EQ(UnitOfMeasure::FOOT, geoc_ft->axisList()[1]->unit());
  EXPECT_EQ(UnitOfMeasure::FOOT, geoc_ft->axisList()[2]->unit());

  // Validation: enforceSameUnit == true throws InvalidCoordinateSystem when
  // axis units differ; enforceSameUnit == false succeeds.
  const auto ax_m1 = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, AxisName::Easting), "x",
      AxisDirection::EAST, UnitOfMeasure::METRE);
  const auto ax_m2 = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, AxisName::Northing), "y",
      AxisDirection::NORTH, UnitOfMeasure::METRE);
  const auto ax_ft = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Z"), "z",
      AxisDirection::UP, UnitOfMeasure::FOOT);

  EXPECT_THROW((void)CartesianCS::create(PropertyMap(), ax_m1, ax_ft, true),
               InvalidCoordinateSystem);
  const auto mixed_2d = CartesianCS::create(PropertyMap(), ax_m1, ax_ft, false);
  EXPECT_EQ(2U, mixed_2d->axisList().size());
  EXPECT_THAT(ExportWkt(*mixed_2d, WKTFormatter::Convention::WKT2),
              HasSubstr("AXIS[\"easting (x)\",east,ORDER[1]"));

  EXPECT_THROW(
      (void)CartesianCS::create(PropertyMap(), ax_m1, ax_ft, ax_m2, true),
      InvalidCoordinateSystem);
  EXPECT_THROW(
      (void)CartesianCS::create(PropertyMap(), ax_m1, ax_m2, ax_ft, true),
      InvalidCoordinateSystem);
  const auto mixed_3d =
      CartesianCS::create(PropertyMap(), ax_m1, ax_m2, ax_ft, false);
  EXPECT_EQ(3U, mixed_3d->axisList().size());

  // Direct coverage of InvalidCoordinateSystem(const std::string&) and copy
  // constructor.
  const InvalidCoordinateSystem exc_str(std::string("custom cs error"));
  const InvalidCoordinateSystem exc_copy(exc_str);
  EXPECT_STREQ("custom cs error", exc_copy.what());
  const Exception& base_exc = exc_copy;
  EXPECT_STREQ("custom cs error", base_exc.what());
}

TEST(CoordinateSystemTest,
     SphericalVerticalAffineOrdinalParametricAndTemporalCS) {
  const auto rad_ax = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Spherical radius"), "r",
      AxisDirection::AWAY_FROM, UnitOfMeasure::METRE);
  const auto lat_ax =
      CoordinateSystemAxis::createLAT_NORTH(UnitOfMeasure::DEGREE);
  const auto lon_ax =
      CoordinateSystemAxis::createLONG_EAST(UnitOfMeasure::DEGREE);

  const auto sph3 = SphericalCS::create(PropertyMap(), rad_ax, lat_ax, lon_ax);
  EXPECT_EQ(3U, sph3->axisList().size());
  EXPECT_THAT(ExportWkt(*sph3, WKTFormatter::Convention::WKT2),
              HasSubstr("CS[spherical,3]"));
  EXPECT_THAT(ExportJson(*sph3), HasSubstr("\"subtype\":\"spherical\""));

  const auto sph2 = SphericalCS::create(PropertyMap(), lat_ax, lon_ax);
  EXPECT_EQ(2U, sph2->axisList().size());
  EXPECT_THAT(ExportWkt(*sph2, WKTFormatter::Convention::WKT2),
              HasSubstr("CS[spherical,2]"));

  // VerticalCS: 1 axis => order == 0 (no ORDER[1] in WKT2).
  const auto vert_custom = VerticalCS::create(
      PropertyMap(), CoordinateSystemAxis::create(
                         PropertyMap().set(IdentifiedObject::NAME_KEY, "Depth"),
                         "D", AxisDirection::DOWN, UnitOfMeasure::METRE));
  const std::string vert_wkt2 =
      ExportWkt(*vert_custom, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(vert_wkt2, HasSubstr("CS[vertical,1]"));
  EXPECT_THAT(vert_wkt2, Not(HasSubstr("ORDER[")));

  const auto vert_h =
      VerticalCS::createGravityRelatedHeight(UnitOfMeasure::METRE);
  EXPECT_EQ("Gravity-related height", vert_h->axisList()[0]->nameStr());
  EXPECT_EQ("H", vert_h->axisList()[0]->abbreviation());
  EXPECT_EQ(AxisDirection::UP, vert_h->axisList()[0]->direction());
  EXPECT_THAT(ExportJson(*vert_h), HasSubstr("\"subtype\":\"vertical\""));
  const auto vert_ft = vert_h->alterUnit(UnitOfMeasure::FOOT);
  EXPECT_EQ(UnitOfMeasure::FOOT, vert_ft->axisList()[0]->unit());

  // AffineCS: 2-axis and 3-axis creation and alterUnit.
  const auto aff_x = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "X"), "X",
      AxisDirection::EAST_NORTH_EAST, UnitOfMeasure::METRE);
  const auto aff_y = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Y"), "Y",
      AxisDirection::NORTH_NORTH_WEST, UnitOfMeasure::METRE);
  const auto aff_z = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Z"), "Z",
      AxisDirection::UP, UnitOfMeasure::METRE);

  const auto aff2 = AffineCS::create(PropertyMap(), aff_x, aff_y);
  EXPECT_THAT(ExportWkt(*aff2, WKTFormatter::Convention::WKT2),
              HasSubstr("CS[affine,2]"));
  EXPECT_THAT(ExportJson(*aff2), HasSubstr("\"subtype\":\"affine\""));
  const auto aff2_ft = aff2->alterUnit(UnitOfMeasure::FOOT);
  EXPECT_EQ(UnitOfMeasure::FOOT, aff2_ft->axisList()[0]->unit());
  EXPECT_EQ(UnitOfMeasure::FOOT, aff2_ft->axisList()[1]->unit());

  const auto aff3 = AffineCS::create(PropertyMap(), aff_x, aff_y, aff_z);
  EXPECT_THAT(ExportWkt(*aff3, WKTFormatter::Convention::WKT2),
              HasSubstr("CS[affine,3]"));
  const auto aff3_ft = aff3->alterUnit(UnitOfMeasure::FOOT);
  EXPECT_EQ(UnitOfMeasure::FOOT, aff3_ft->axisList()[0]->unit());
  EXPECT_EQ(UnitOfMeasure::FOOT, aff3_ft->axisList()[1]->unit());
  EXPECT_EQ(UnitOfMeasure::FOOT, aff3_ft->axisList()[2]->unit());

  // OrdinalCS: 2-axis and empty axis list under WKT2_SIMPLIFIED.
  const auto ord_col = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Column"), "i",
      AxisDirection::COLUMN_POSITIVE, UnitOfMeasure::NONE);
  const auto ord_row = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Row"), "j",
      AxisDirection::ROW_POSITIVE, UnitOfMeasure::NONE);
  const auto ord2 = OrdinalCS::create(PropertyMap(), {ord_col, ord_row});
  EXPECT_THAT(ExportWkt(*ord2, WKTFormatter::Convention::WKT2),
              HasSubstr("CS[ordinal,2]"));
  EXPECT_THAT(ExportJson(*ord2), HasSubstr("\"subtype\":\"ordinal\""));

  const auto ord_empty = OrdinalCS::create(PropertyMap(), {});
  EXPECT_TRUE(ord_empty->axisList().empty());
  const std::string ord_empty_wkt =
      ExportWkt(*ord_empty, WKTFormatter::Convention::WKT2_SIMPLIFIED);
  EXPECT_THAT(ord_empty_wkt, HasSubstr("CS[ordinal,0]"));
  EXPECT_THAT(ord_empty_wkt, Not(HasSubstr("AXIS[")));
  EXPECT_THAT(ord_empty_wkt, Not(HasSubstr("UNIT[")));

  // ParametricCS.
  const auto param_axis = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Pressure"), "p",
      AxisDirection::UP,
      UnitOfMeasure("hectopascal", 100.0, UnitOfMeasure::Type::PARAMETRIC));
  const auto param_cs = ParametricCS::create(PropertyMap(), param_axis);
  EXPECT_THAT(ExportWkt(*param_cs, WKTFormatter::Convention::WKT2),
              HasSubstr("CS[parametric,1]"));
  EXPECT_THAT(ExportJson(*param_cs), HasSubstr("\"subtype\":\"parametric\""));

  // TemporalCS subclasses: WKT2_2015 emits "temporal" while WKT2_2019 emits
  // the concrete subtype keyword.
  const auto dt_axis = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Time"), "t",
      AxisDirection::FUTURE, UnitOfMeasure::NONE);
  const auto dt_cs = DateTimeTemporalCS::create(PropertyMap(), dt_axis);
  EXPECT_THAT(ExportWkt(*dt_cs, WKTFormatter::Convention::WKT2_2019),
              HasSubstr("CS[TemporalDateTime,1]"));
  EXPECT_THAT(ExportWkt(*dt_cs, WKTFormatter::Convention::WKT2_2015),
              HasSubstr("CS[temporal,1]"));
  EXPECT_THAT(ExportJson(*dt_cs),
              HasSubstr("\"subtype\":\"TemporalDateTime\""));

  const auto tc_axis = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Step"), "k",
      AxisDirection::FUTURE, UnitOfMeasure::NONE);
  const auto tc_cs = TemporalCountCS::create(PropertyMap(), tc_axis);
  EXPECT_THAT(ExportWkt(*tc_cs, WKTFormatter::Convention::WKT2_2019),
              HasSubstr("CS[TemporalCount,1]"));
  EXPECT_THAT(ExportWkt(*tc_cs, WKTFormatter::Convention::WKT2_2015),
              HasSubstr("CS[temporal,1]"));
  EXPECT_THAT(ExportJson(*tc_cs), HasSubstr("\"subtype\":\"TemporalCount\""));

  const auto tm_axis = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Time"), "t",
      AxisDirection::FUTURE, UnitOfMeasure::YEAR);
  const auto tm_cs = TemporalMeasureCS::create(PropertyMap(), tm_axis);
  EXPECT_THAT(ExportWkt(*tm_cs, WKTFormatter::Convention::WKT2_2019),
              HasSubstr("CS[TemporalMeasure,1]"));
  EXPECT_THAT(ExportWkt(*tm_cs, WKTFormatter::Convention::WKT2_2015),
              HasSubstr("CS[temporal,1]"));
  EXPECT_THAT(ExportJson(*tm_cs), HasSubstr("\"subtype\":\"TemporalMeasure\""));
}

TEST(CoordinateSystemTest,
     CoordinateSystemWktJsonSuppressionAndEquivalenceBranches) {
  const auto ax1 =
      CoordinateSystemAxis::create(MakeNameAndEpsgId("Easting", "1"), "E",
                                   AxisDirection::EAST, UnitOfMeasure::METRE);
  const auto ax2 =
      CoordinateSystemAxis::create(MakeNameAndEpsgId("Northing", "2"), "N",
                                   AxisDirection::NORTH, UnitOfMeasure::METRE);
  const auto cs_with_id = CartesianCS::create(
      MakeNameAndEpsgId("Cartesian 2D CS", "4400"), ax1, ax2);

  // OutputAxisRule != YES returns empty string from
  // CoordinateSystem::exportToWKT.
  EXPECT_THAT(ExportWkt(*cs_with_id, WKTFormatter::Convention::WKT1_ESRI),
              IsEmpty());
  EXPECT_THAT(ExportWkt(*cs_with_id, WKTFormatter::Convention::WKT1_GDAL),
              IsEmpty());

  // WKT1_GDAL with OutputAxisRule::YES emits AXIS nodes without CS[...].
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
    fmt->setMultiLine(false).setOutputAxis(WKTFormatter::OutputAxisRule::YES);
    const std::string wkt1 = cs_with_id->exportToWKT(fmt.get());
    EXPECT_THAT(wkt1, Not(HasSubstr("CS[")));
    EXPECT_THAT(wkt1,
                HasSubstr("AXIS[\"Easting\",EAST,AUTHORITY[\"EPSG\",\"1\"]]"));
    EXPECT_THAT(
        wkt1, HasSubstr("AXIS[\"Northing\",NORTH,AUTHORITY[\"EPSG\",\"2\"]]"));
    EXPECT_THAT(wkt1, Not(HasSubstr("UNIT[")));
  }

  // Standalone CS with ID emits "id" in JSON and suppresses axis IDs.
  const std::string cs_json = ExportJson(*cs_with_id);
  EXPECT_THAT(cs_json,
              HasSubstr("\"id\":{\"authority\":\"EPSG\",\"code\":4400}"));
  EXPECT_THAT(cs_json, Not(HasSubstr("\"code\":1")));

  // CS inside an identified EngineeringCRS suppresses CS-level "id" in JSON
  // (outputId() == false branch in CoordinateSystem::_exportToJSON).
  const auto eng_datum = EngineeringDatum::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Local Datum"));
  const auto eng_crs = EngineeringCRS::create(
      MakeNameAndEpsgId("Local Engineering", "9999"), eng_datum, cs_with_id);
  const std::string crs_json = ExportJson(*eng_crs);
  EXPECT_THAT(crs_json, HasSubstr("\"code\":9999"));
  EXPECT_THAT(crs_json, Not(HasSubstr("\"code\":4400")));

  // Equivalence checks across null, type mismatch, name, size, subtype, and
  // axes.
  const auto cs_diff_name = CartesianCS::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "cartesian_2d_cs"), ax1,
      ax2);
  const auto affine_same_axes =
      AffineCS::create(MakeNameAndEpsgId("Cartesian 2D CS", "4400"), ax1, ax2);
  const auto lat_lon =
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE);
  const auto lat_lon_h =
      EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
          UnitOfMeasure::DEGREE, UnitOfMeasure::METRE);
  const auto ax2_ft = CoordinateSystemAxis::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Northing"), "N",
      AxisDirection::NORTH, UnitOfMeasure::FOOT);
  const auto cs_diff_axis = CartesianCS::create(
      MakeNameAndEpsgId("Cartesian 2D CS", "4400"), ax1, ax2_ft, false);
  const auto meridian = Meridian::create(Angle(0.0));

  EXPECT_TRUE(cs_with_id->isEquivalentTo(cs_with_id.get(),
                                         IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      cs_with_id->isEquivalentTo(nullptr, IComparable::Criterion::STRICT));
  EXPECT_FALSE(cs_with_id->isEquivalentTo(meridian.get(),
                                          IComparable::Criterion::STRICT));
  EXPECT_FALSE(cs_with_id->isEquivalentTo(cs_diff_name.get(),
                                          IComparable::Criterion::STRICT));
  EXPECT_TRUE(cs_with_id->isEquivalentTo(cs_diff_name.get(),
                                         IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(lat_lon->isEquivalentTo(lat_lon_h.get(),
                                       IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(cs_with_id->isEquivalentTo(affine_same_axes.get(),
                                          IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(cs_with_id->isEquivalentTo(cs_diff_axis.get(),
                                          IComparable::Criterion::EQUIVALENT));
}

TEST(CoordinateSystemTest, CodeListLookupsAndCApiInterop) {
  struct DirectionLookupCase {
    const char* input;
    const AxisDirection* expected;
  };
  constexpr DirectionLookupCase kDirCases[] = {
      {.input = "north", .expected = &AxisDirection::NORTH},
      {.input = "NORTH", .expected = &AxisDirection::NORTH},
      {.input = "westSouthWest", .expected = &AxisDirection::WEST_SOUTH_WEST},
      {.input = "geocentricX", .expected = &AxisDirection::GEOCENTRIC_X},
      {.input = "future", .expected = &AxisDirection::FUTURE},
      {.input = "unspecified", .expected = &AxisDirection::UNSPECIFIED},
      {.input = "", .expected = nullptr},
      {.input = "noSuchDirection", .expected = nullptr},
  };
  for (const auto& tc : kDirCases) {
    EXPECT_EQ(tc.expected, AxisDirection::valueOf(tc.input)) << tc.input;
  }

  struct RangeMeaningCase {
    const char* input;
    const RangeMeaning* expected;
  };
  constexpr RangeMeaningCase kRangeCases[] = {
      {.input = "exact", .expected = &RangeMeaning::EXACT},
      {.input = "WRAPAROUND", .expected = &RangeMeaning::WRAPAROUND},
      {.input = "", .expected = nullptr},
      {.input = "unknown", .expected = nullptr},
  };
  for (const auto& tc : kRangeCases) {
    EXPECT_EQ(tc.expected, RangeMeaning::valueOf(tc.input)) << tc.input;
  }
  const optional<RangeMeaning> empty_range_meaning;
  EXPECT_FALSE(empty_range_meaning.has_value());

  struct Wkt1DirCase {
    const char* input;
    const AxisDirectionWKT1* expected;
  };
  constexpr Wkt1DirCase kWkt1Cases[] = {
      {.input = "NORTH", .expected = &AxisDirectionWKT1::NORTH},
      {.input = "south", .expected = &AxisDirectionWKT1::SOUTH},
      {.input = "East", .expected = &AxisDirectionWKT1::EAST},
      {.input = "west", .expected = &AxisDirectionWKT1::WEST},
      {.input = "up", .expected = &AxisDirectionWKT1::UP},
      {.input = "DOWN", .expected = &AxisDirectionWKT1::DOWN},
      {.input = "other", .expected = &AxisDirectionWKT1::OTHER},
      {.input = "geocentricX", .expected = nullptr},
      {.input = "", .expected = nullptr},
  };
  for (const auto& tc : kWkt1Cases) {
    EXPECT_EQ(tc.expected, AxisDirectionWKT1::valueOf(tc.input)) << tc.input;
  }

  // Verify C API coordinate system constructors on an isolated context.
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  ScopedPj cart_cs(proj_create_cartesian_2D_cs(
      ctx.get(), PJ_CART2D_EASTING_NORTHING, "metre", 1.0));
  ASSERT_NE(nullptr, cart_cs.get());
  EXPECT_EQ(PJ_CS_TYPE_CARTESIAN, proj_cs_get_type(ctx.get(), cart_cs.get()));
  EXPECT_EQ(2, proj_cs_get_axis_count(ctx.get(), cart_cs.get()));

  const char* axis_name = nullptr;
  const char* axis_abbrev = nullptr;
  const char* axis_dir = nullptr;
  double conv_factor = 0.0;
  ASSERT_EQ(1, proj_cs_get_axis_info(ctx.get(), cart_cs.get(), 0, &axis_name,
                                     &axis_abbrev, &axis_dir, &conv_factor,
                                     nullptr, nullptr, nullptr));
  EXPECT_STREQ("Easting", axis_name);
  EXPECT_STREQ("E", axis_abbrev);
  EXPECT_STREQ("east", axis_dir);
  EXPECT_DOUBLE_EQ(1.0, conv_factor);

  ScopedPj ellps_3d(proj_create_ellipsoidal_3D_cs(
      ctx.get(), PJ_ELLPS3D_LATITUDE_LONGITUDE_HEIGHT, "degree",
      std::numbers::pi / 180.0, "metre", 1.0));
  ASSERT_NE(nullptr, ellps_3d.get());
  EXPECT_EQ(PJ_CS_TYPE_ELLIPSOIDAL,
            proj_cs_get_type(ctx.get(), ellps_3d.get()));
  EXPECT_EQ(3, proj_cs_get_axis_count(ctx.get(), ellps_3d.get()));

  char name_buf[] = "Time";
  char abbrev_buf[] = "t";
  char dir_buf[] = "future";
  char unit_buf[] = "year";
  const PJ_AXIS_DESCRIPTION temp_axis = {
      .name = name_buf,
      .abbreviation = abbrev_buf,
      .direction = dir_buf,
      .unit_name = unit_buf,
      .unit_conv_factor = 31556925.445,
      .unit_type = PJ_UT_TIME,
  };
  ScopedPj temporal_cs(
      proj_create_cs(ctx.get(), PJ_CS_TYPE_TEMPORALMEASURE, 1, &temp_axis));
  ASSERT_NE(nullptr, temporal_cs.get());
  EXPECT_EQ(PJ_CS_TYPE_TEMPORALMEASURE,
            proj_cs_get_type(ctx.get(), temporal_cs.get()));
  EXPECT_EQ(0, proj_context_errno(ctx.get()));
}

}  // namespace
}  // namespace proj
