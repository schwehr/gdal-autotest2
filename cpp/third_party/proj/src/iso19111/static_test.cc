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
// Unit tests for ISO-19111 static constants and predefined objects
// (NameSpace::GLOBAL, property keys, Extent::WORLD, WKTConstants,
// UnitOfMeasure, AxisDirection, RangeMeaning, AxisDirectionWKT1, AxisName,
// AxisAbbreviation, RealizationMethod, PrimeMeridian, Ellipsoid,
// GeodeticReferenceFrame, TemporalDatum, GeodeticCRS, GeographicCRS, and
// operation constants) in proj/src/iso19111/static.cpp. Note: in google3
// default non-GCE builds, proj.db is embedded in memory and networking is
// disabled.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/static.cpp

#include <cstddef>
#include <iterator>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinateoperation.hpp"
#include "third_party/proj4/proj/include/proj/coordinatesystem.hpp"
#include "third_party/proj4/proj/include/proj/crs.hpp"
#include "third_party/proj4/proj/include/proj/datum.hpp"
#include "third_party/proj4/proj/include/proj/internal/coordinatesystem_internal.hpp"
#include "third_party/proj4/proj/include/proj/internal/io_internal.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/iso19111/operation/oputils.hpp"

namespace proj {
namespace {

using ::osgeo::proj::common::IdentifiedObject;
using ::osgeo::proj::common::Measure;
using ::osgeo::proj::common::ObjectUsage;
using ::osgeo::proj::common::UnitOfMeasure;
using ::osgeo::proj::crs::GeodeticCRS;
using ::osgeo::proj::crs::GeographicCRS;
using ::osgeo::proj::cs::AxisAbbreviation;
using ::osgeo::proj::cs::AxisDirection;
using ::osgeo::proj::cs::AxisDirectionWKT1;
using ::osgeo::proj::cs::AxisName;
using ::osgeo::proj::cs::RangeMeaning;
using ::osgeo::proj::datum::Ellipsoid;
using ::osgeo::proj::datum::GeodeticReferenceFrame;
using ::osgeo::proj::datum::PrimeMeridian;
using ::osgeo::proj::datum::RealizationMethod;
using ::osgeo::proj::datum::TemporalDatum;
using ::osgeo::proj::io::WKTConstants;
using ::osgeo::proj::metadata::Extent;
using ::osgeo::proj::metadata::GeographicBoundingBox;
using ::osgeo::proj::metadata::Identifier;
using ::osgeo::proj::operation::AXIS_ORDER_CHANGE_2D_NAME;
using ::osgeo::proj::operation::AXIS_ORDER_CHANGE_3D_NAME;
using ::osgeo::proj::operation::CoordinateOperation;
using ::osgeo::proj::operation::INVERSE_OF;
using ::osgeo::proj::operation::nullMeasure;
using ::osgeo::proj::util::NameFactory;
using ::osgeo::proj::util::NameSpace;
using ::osgeo::proj::util::NameSpaceNNPtr;
using ::testing::IsEmpty;
using ::testing::NotNull;

constexpr double kPi = std::numbers::pi;
constexpr double kSecondsPerTropicalYear = 31556925.445;

class NameSpaceAccessor final : public NameSpace {
 public:
  static const NameSpaceNNPtr& Global() { return NameSpace::GLOBAL; }
};

struct StringConstantCase {
  const std::string* actual;
  std::string_view expected;
};

TEST(StaticTest, GlobalNamespaceAndPropertyKeyStrings) {
  // 1. Verify NameSpace::GLOBAL matches the fallback scope from NameFactory.
  const NameSpaceNNPtr& global = NameSpaceAccessor::Global();
  EXPECT_TRUE(global->isGlobal());
  ASSERT_THAT(global->name().get(), NotNull());
  EXPECT_EQ("global", global->name()->toString());
  EXPECT_EQ("a:b",
            NameFactory::createGenericName(nullptr, {"a", "b"})->toString());
  EXPECT_EQ(global.get(),
            NameFactory::createLocalName(nullptr, "probe")->scope().get());

  // 2. Verify all Identifier, IdentifiedObject, ObjectUsage, Datum, Ellipsoid,
  //    and CoordinateOperation string constants.
  constexpr StringConstantCase kKeyCases[] = {
      {.actual = &Identifier::AUTHORITY_KEY, .expected = "authority"},
      {.actual = &Identifier::CODE_KEY, .expected = "code"},
      {.actual = &Identifier::CODESPACE_KEY, .expected = "codespace"},
      {.actual = &Identifier::VERSION_KEY, .expected = "version"},
      {.actual = &Identifier::DESCRIPTION_KEY, .expected = "description"},
      {.actual = &Identifier::URI_KEY, .expected = "uri"},
      {.actual = &Identifier::EPSG, .expected = "EPSG"},
      {.actual = &Identifier::OGC, .expected = "OGC"},
      {.actual = &IdentifiedObject::NAME_KEY, .expected = "name"},
      {.actual = &IdentifiedObject::IDENTIFIERS_KEY, .expected = "identifiers"},
      {.actual = &IdentifiedObject::ALIAS_KEY, .expected = "alias"},
      {.actual = &IdentifiedObject::REMARKS_KEY, .expected = "remarks"},
      {.actual = &IdentifiedObject::DEPRECATED_KEY, .expected = "deprecated"},
      {.actual = &ObjectUsage::SCOPE_KEY, .expected = "scope"},
      {.actual = &ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
       .expected = "domainOfValidity"},
      {.actual = &ObjectUsage::OBJECT_DOMAIN_KEY, .expected = "objectDomain"},
      {.actual = &Ellipsoid::EARTH, .expected = "Earth"},
      {.actual = &TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN,
       .expected = "proleptic Gregorian"},
      {.actual = &CoordinateOperation::OPERATION_VERSION_KEY,
       .expected = "operationVersion"},
      {.actual = &INVERSE_OF, .expected = "Inverse of "},
      {.actual = &AXIS_ORDER_CHANGE_2D_NAME,
       .expected = "axis order change (2D)"},
      {.actual = &AXIS_ORDER_CHANGE_3D_NAME,
       .expected = "axis order change (geographic3D horizontal)"},
  };
  for (const auto& tc : kKeyCases) {
    EXPECT_EQ(tc.expected, *tc.actual);
  }
}

TEST(StaticTest, ExtentWorldMatchesFullGlobeBounds) {
  ASSERT_TRUE(Extent::WORLD->description().has_value());
  EXPECT_EQ("World", *Extent::WORLD->description());
  ASSERT_EQ(1u, Extent::WORLD->geographicElements().size());
  EXPECT_THAT(Extent::WORLD->verticalElements(), IsEmpty());
  EXPECT_THAT(Extent::WORLD->temporalElements(), IsEmpty());

  const auto* bbox = dynamic_cast<const GeographicBoundingBox*>(
      Extent::WORLD->geographicElements()[0].get());
  ASSERT_THAT(bbox, NotNull());
  EXPECT_DOUBLE_EQ(-180.0, bbox->westBoundLongitude());
  EXPECT_DOUBLE_EQ(-90.0, bbox->southBoundLatitude());
  EXPECT_DOUBLE_EQ(180.0, bbox->eastBoundLongitude());
  EXPECT_DOUBLE_EQ(90.0, bbox->northBoundLatitude());

  EXPECT_TRUE(
      Extent::WORLD->contains(Extent::createFromBBOX(-10, -10, 10, 10)));
}

TEST(StaticTest, WktConstantsAndListMatchAll105KeywordsInOrder) {
  constexpr StringConstantCase kWktCases[] = {
      // WKT1
      {.actual = &WKTConstants::GEOCCS, .expected = "GEOCCS"},
      {.actual = &WKTConstants::GEOGCS, .expected = "GEOGCS"},
      {.actual = &WKTConstants::DATUM, .expected = "DATUM"},
      {.actual = &WKTConstants::UNIT, .expected = "UNIT"},
      {.actual = &WKTConstants::SPHEROID, .expected = "SPHEROID"},
      {.actual = &WKTConstants::AXIS, .expected = "AXIS"},
      {.actual = &WKTConstants::PRIMEM, .expected = "PRIMEM"},
      {.actual = &WKTConstants::AUTHORITY, .expected = "AUTHORITY"},
      {.actual = &WKTConstants::PROJCS, .expected = "PROJCS"},
      {.actual = &WKTConstants::PROJECTION, .expected = "PROJECTION"},
      {.actual = &WKTConstants::PARAMETER, .expected = "PARAMETER"},
      {.actual = &WKTConstants::VERT_CS, .expected = "VERT_CS"},
      {.actual = &WKTConstants::VERTCS, .expected = "VERTCS"},
      {.actual = &WKTConstants::VERT_DATUM, .expected = "VERT_DATUM"},
      {.actual = &WKTConstants::COMPD_CS, .expected = "COMPD_CS"},
      {.actual = &WKTConstants::TOWGS84, .expected = "TOWGS84"},
      {.actual = &WKTConstants::EXTENSION, .expected = "EXTENSION"},
      {.actual = &WKTConstants::LOCAL_CS, .expected = "LOCAL_CS"},
      {.actual = &WKTConstants::LOCAL_DATUM, .expected = "LOCAL_DATUM"},
      {.actual = &WKTConstants::LINUNIT, .expected = "LINUNIT"},
      // WKT2 preferred
      {.actual = &WKTConstants::GEODCRS, .expected = "GEODCRS"},
      {.actual = &WKTConstants::LENGTHUNIT, .expected = "LENGTHUNIT"},
      {.actual = &WKTConstants::ANGLEUNIT, .expected = "ANGLEUNIT"},
      {.actual = &WKTConstants::SCALEUNIT, .expected = "SCALEUNIT"},
      {.actual = &WKTConstants::TIMEUNIT, .expected = "TIMEUNIT"},
      {.actual = &WKTConstants::ELLIPSOID, .expected = "ELLIPSOID"},
      {.actual = &WKTConstants::CS_, .expected = "CS"},
      {.actual = &WKTConstants::ID, .expected = "ID"},
      {.actual = &WKTConstants::PROJCRS, .expected = "PROJCRS"},
      {.actual = &WKTConstants::BASEGEODCRS, .expected = "BASEGEODCRS"},
      {.actual = &WKTConstants::MERIDIAN, .expected = "MERIDIAN"},
      {.actual = &WKTConstants::ORDER, .expected = "ORDER"},
      {.actual = &WKTConstants::ANCHOR, .expected = "ANCHOR"},
      {.actual = &WKTConstants::ANCHOREPOCH, .expected = "ANCHOREPOCH"},
      {.actual = &WKTConstants::CONVERSION, .expected = "CONVERSION"},
      {.actual = &WKTConstants::METHOD, .expected = "METHOD"},
      {.actual = &WKTConstants::REMARK, .expected = "REMARK"},
      {.actual = &WKTConstants::GEOGCRS, .expected = "GEOGCRS"},
      {.actual = &WKTConstants::BASEGEOGCRS, .expected = "BASEGEOGCRS"},
      {.actual = &WKTConstants::SCOPE, .expected = "SCOPE"},
      {.actual = &WKTConstants::AREA, .expected = "AREA"},
      {.actual = &WKTConstants::BBOX, .expected = "BBOX"},
      {.actual = &WKTConstants::CITATION, .expected = "CITATION"},
      {.actual = &WKTConstants::URI, .expected = "URI"},
      {.actual = &WKTConstants::VERTCRS, .expected = "VERTCRS"},
      {.actual = &WKTConstants::VDATUM, .expected = "VDATUM"},
      {.actual = &WKTConstants::COMPOUNDCRS, .expected = "COMPOUNDCRS"},
      {.actual = &WKTConstants::PARAMETERFILE, .expected = "PARAMETERFILE"},
      {.actual = &WKTConstants::COORDINATEOPERATION,
       .expected = "COORDINATEOPERATION"},
      {.actual = &WKTConstants::SOURCECRS, .expected = "SOURCECRS"},
      {.actual = &WKTConstants::TARGETCRS, .expected = "TARGETCRS"},
      {.actual = &WKTConstants::INTERPOLATIONCRS,
       .expected = "INTERPOLATIONCRS"},
      {.actual = &WKTConstants::OPERATIONACCURACY,
       .expected = "OPERATIONACCURACY"},
      {.actual = &WKTConstants::CONCATENATEDOPERATION,
       .expected = "CONCATENATEDOPERATION"},
      {.actual = &WKTConstants::STEP, .expected = "STEP"},
      {.actual = &WKTConstants::BOUNDCRS, .expected = "BOUNDCRS"},
      {.actual = &WKTConstants::ABRIDGEDTRANSFORMATION,
       .expected = "ABRIDGEDTRANSFORMATION"},
      {.actual = &WKTConstants::DERIVINGCONVERSION,
       .expected = "DERIVINGCONVERSION"},
      {.actual = &WKTConstants::TDATUM, .expected = "TDATUM"},
      {.actual = &WKTConstants::CALENDAR, .expected = "CALENDAR"},
      {.actual = &WKTConstants::TIMEORIGIN, .expected = "TIMEORIGIN"},
      {.actual = &WKTConstants::TIMECRS, .expected = "TIMECRS"},
      {.actual = &WKTConstants::VERTICALEXTENT, .expected = "VERTICALEXTENT"},
      {.actual = &WKTConstants::TIMEEXTENT, .expected = "TIMEEXTENT"},
      {.actual = &WKTConstants::USAGE, .expected = "USAGE"},
      {.actual = &WKTConstants::DYNAMIC, .expected = "DYNAMIC"},
      {.actual = &WKTConstants::FRAMEEPOCH, .expected = "FRAMEEPOCH"},
      {.actual = &WKTConstants::MODEL, .expected = "MODEL"},
      {.actual = &WKTConstants::VELOCITYGRID, .expected = "VELOCITYGRID"},
      {.actual = &WKTConstants::ENSEMBLE, .expected = "ENSEMBLE"},
      {.actual = &WKTConstants::MEMBER, .expected = "MEMBER"},
      {.actual = &WKTConstants::ENSEMBLEACCURACY,
       .expected = "ENSEMBLEACCURACY"},
      {.actual = &WKTConstants::DERIVEDPROJCRS, .expected = "DERIVEDPROJCRS"},
      {.actual = &WKTConstants::BASEPROJCRS, .expected = "BASEPROJCRS"},
      {.actual = &WKTConstants::EDATUM, .expected = "EDATUM"},
      {.actual = &WKTConstants::ENGCRS, .expected = "ENGCRS"},
      {.actual = &WKTConstants::PDATUM, .expected = "PDATUM"},
      {.actual = &WKTConstants::PARAMETRICCRS, .expected = "PARAMETRICCRS"},
      {.actual = &WKTConstants::PARAMETRICUNIT, .expected = "PARAMETRICUNIT"},
      {.actual = &WKTConstants::BASEVERTCRS, .expected = "BASEVERTCRS"},
      {.actual = &WKTConstants::BASEENGCRS, .expected = "BASEENGCRS"},
      {.actual = &WKTConstants::BASEPARAMCRS, .expected = "BASEPARAMCRS"},
      {.actual = &WKTConstants::BASETIMECRS, .expected = "BASETIMECRS"},
      {.actual = &WKTConstants::VERSION, .expected = "VERSION"},
      {.actual = &WKTConstants::GEOIDMODEL, .expected = "GEOIDMODEL"},
      {.actual = &WKTConstants::COORDINATEMETADATA,
       .expected = "COORDINATEMETADATA"},
      {.actual = &WKTConstants::EPOCH, .expected = "EPOCH"},
      {.actual = &WKTConstants::AXISMINVALUE, .expected = "AXISMINVALUE"},
      {.actual = &WKTConstants::AXISMAXVALUE, .expected = "AXISMAXVALUE"},
      {.actual = &WKTConstants::RANGEMEANING, .expected = "RANGEMEANING"},
      {.actual = &WKTConstants::POINTMOTIONOPERATION,
       .expected = "POINTMOTIONOPERATION"},
      // WKT2 alternates
      {.actual = &WKTConstants::GEODETICCRS, .expected = "GEODETICCRS"},
      {.actual = &WKTConstants::GEODETICDATUM, .expected = "GEODETICDATUM"},
      {.actual = &WKTConstants::PROJECTEDCRS, .expected = "PROJECTEDCRS"},
      {.actual = &WKTConstants::PRIMEMERIDIAN, .expected = "PRIMEMERIDIAN"},
      {.actual = &WKTConstants::GEOGRAPHICCRS, .expected = "GEOGRAPHICCRS"},
      {.actual = &WKTConstants::TRF, .expected = "TRF"},
      {.actual = &WKTConstants::VERTICALCRS, .expected = "VERTICALCRS"},
      {.actual = &WKTConstants::VERTICALDATUM, .expected = "VERTICALDATUM"},
      {.actual = &WKTConstants::VRF, .expected = "VRF"},
      {.actual = &WKTConstants::TIMEDATUM, .expected = "TIMEDATUM"},
      {.actual = &WKTConstants::TEMPORALQUANTITY,
       .expected = "TEMPORALQUANTITY"},
      {.actual = &WKTConstants::ENGINEERINGDATUM,
       .expected = "ENGINEERINGDATUM"},
      {.actual = &WKTConstants::ENGINEERINGCRS, .expected = "ENGINEERINGCRS"},
      {.actual = &WKTConstants::PARAMETRICDATUM, .expected = "PARAMETRICDATUM"},
  };

  const std::vector<std::string>& registered = WKTConstants::constants();
  ASSERT_EQ(std::size(kWktCases), registered.size());
  for (std::size_t i = 0; i < std::size(kWktCases); ++i) {
    EXPECT_EQ(kWktCases[i].expected, *kWktCases[i].actual) << "index=" << i;
    EXPECT_EQ(kWktCases[i].expected, registered[i]) << "index=" << i;
  }
}

TEST(StaticTest, UnitOfMeasurePredefinedInstancesMatchEpsgDefinitions) {
  struct UnitCase {
    const UnitOfMeasure* unit;
    std::string_view name;
    double to_si;
    UnitOfMeasure::Type type;
    std::string_view codespace;
    std::string_view code;
  };
  constexpr UnitCase kUnits[] = {
      {.unit = &UnitOfMeasure::NONE,
       .name = "",
       .to_si = 1.0,
       .type = UnitOfMeasure::Type::NONE,
       .codespace = "",
       .code = ""},
      {.unit = &UnitOfMeasure::SCALE_UNITY,
       .name = "unity",
       .to_si = 1.0,
       .type = UnitOfMeasure::Type::SCALE,
       .codespace = "EPSG",
       .code = "9201"},
      {.unit = &UnitOfMeasure::PARTS_PER_MILLION,
       .name = "parts per million",
       .to_si = 1e-6,
       .type = UnitOfMeasure::Type::SCALE,
       .codespace = "EPSG",
       .code = "9202"},
      {.unit = &UnitOfMeasure::METRE,
       .name = "metre",
       .to_si = 1.0,
       .type = UnitOfMeasure::Type::LINEAR,
       .codespace = "EPSG",
       .code = "9001"},
      {.unit = &UnitOfMeasure::FOOT,
       .name = "foot",
       .to_si = 0.3048,
       .type = UnitOfMeasure::Type::LINEAR,
       .codespace = "EPSG",
       .code = "9002"},
      {.unit = &UnitOfMeasure::US_FOOT,
       .name = "US survey foot",
       .to_si = 0.304800609601219241184,
       .type = UnitOfMeasure::Type::LINEAR,
       .codespace = "EPSG",
       .code = "9003"},
      {.unit = &UnitOfMeasure::DEGREE,
       .name = "degree",
       .to_si = kPi / 180.0,
       .type = UnitOfMeasure::Type::ANGULAR,
       .codespace = "EPSG",
       .code = "9122"},
      {.unit = &UnitOfMeasure::ARC_SECOND,
       .name = "arc-second",
       .to_si = kPi / 180.0 / 3600.0,
       .type = UnitOfMeasure::Type::ANGULAR,
       .codespace = "EPSG",
       .code = "9104"},
      {.unit = &UnitOfMeasure::GRAD,
       .name = "grad",
       .to_si = kPi / 200.0,
       .type = UnitOfMeasure::Type::ANGULAR,
       .codespace = "EPSG",
       .code = "9105"},
      {.unit = &UnitOfMeasure::RADIAN,
       .name = "radian",
       .to_si = 1.0,
       .type = UnitOfMeasure::Type::ANGULAR,
       .codespace = "EPSG",
       .code = "9101"},
      {.unit = &UnitOfMeasure::MICRORADIAN,
       .name = "microradian",
       .to_si = 1e-6,
       .type = UnitOfMeasure::Type::ANGULAR,
       .codespace = "EPSG",
       .code = "9109"},
      {.unit = &UnitOfMeasure::SECOND,
       .name = "second",
       .to_si = 1.0,
       .type = UnitOfMeasure::Type::TIME,
       .codespace = "EPSG",
       .code = "1040"},
      {.unit = &UnitOfMeasure::YEAR,
       .name = "year",
       .to_si = kSecondsPerTropicalYear,
       .type = UnitOfMeasure::Type::TIME,
       .codespace = "EPSG",
       .code = "1029"},
      {.unit = &UnitOfMeasure::METRE_PER_YEAR,
       .name = "metres per year",
       .to_si = 1.0 / kSecondsPerTropicalYear,
       .type = UnitOfMeasure::Type::LINEAR,
       .codespace = "EPSG",
       .code = "1042"},
      {.unit = &UnitOfMeasure::ARC_SECOND_PER_YEAR,
       .name = "arc-seconds per year",
       .to_si = kPi / 180.0 / 3600.0 / kSecondsPerTropicalYear,
       .type = UnitOfMeasure::Type::ANGULAR,
       .codespace = "EPSG",
       .code = "1043"},
      {.unit = &UnitOfMeasure::PPM_PER_YEAR,
       .name = "parts per million per year",
       .to_si = 1e-6 / kSecondsPerTropicalYear,
       .type = UnitOfMeasure::Type::SCALE,
       .codespace = "EPSG",
       .code = "1036"},
  };

  for (const auto& tc : kUnits) {
    EXPECT_EQ(tc.name, tc.unit->name());
    EXPECT_NEAR(tc.to_si, tc.unit->conversionToSI(), tc.to_si * 1e-14)
        << tc.name;
    EXPECT_EQ(tc.type, tc.unit->type()) << tc.name;
    EXPECT_EQ(tc.codespace, tc.unit->codeSpace()) << tc.name;
    EXPECT_EQ(tc.code, tc.unit->code()) << tc.name;
  }

  // Spot-check cross-unit conversions between static instances.
  EXPECT_NEAR(
      0.3048,
      Measure(1.0, UnitOfMeasure::FOOT).convertToUnit(UnitOfMeasure::METRE),
      1e-14);
  EXPECT_NEAR(
      200.0,
      Measure(180.0, UnitOfMeasure::DEGREE).convertToUnit(UnitOfMeasure::GRAD),
      1e-12);
}

TEST(StaticTest, AxisDirectionsRangeMeaningWkt1DirectionsAndAxisStrings) {
  // 1. All 40 AxisDirection static objects and their registry entries.
  struct AxisDirCase {
    const AxisDirection* dir;
    std::string_view name;
  };
  constexpr AxisDirCase kAxisDirs[] = {
      {.dir = &AxisDirection::NORTH, .name = "north"},
      {.dir = &AxisDirection::NORTH_NORTH_EAST, .name = "northNorthEast"},
      {.dir = &AxisDirection::NORTH_EAST, .name = "northEast"},
      {.dir = &AxisDirection::EAST_NORTH_EAST, .name = "eastNorthEast"},
      {.dir = &AxisDirection::EAST, .name = "east"},
      {.dir = &AxisDirection::EAST_SOUTH_EAST, .name = "eastSouthEast"},
      {.dir = &AxisDirection::SOUTH_EAST, .name = "southEast"},
      {.dir = &AxisDirection::SOUTH_SOUTH_EAST, .name = "southSouthEast"},
      {.dir = &AxisDirection::SOUTH, .name = "south"},
      {.dir = &AxisDirection::SOUTH_SOUTH_WEST, .name = "southSouthWest"},
      {.dir = &AxisDirection::SOUTH_WEST, .name = "southWest"},
      {.dir = &AxisDirection::WEST_SOUTH_WEST, .name = "westSouthWest"},
      {.dir = &AxisDirection::WEST, .name = "west"},
      {.dir = &AxisDirection::WEST_NORTH_WEST, .name = "westNorthWest"},
      {.dir = &AxisDirection::NORTH_WEST, .name = "northWest"},
      {.dir = &AxisDirection::NORTH_NORTH_WEST, .name = "northNorthWest"},
      {.dir = &AxisDirection::UP, .name = "up"},
      {.dir = &AxisDirection::DOWN, .name = "down"},
      {.dir = &AxisDirection::GEOCENTRIC_X, .name = "geocentricX"},
      {.dir = &AxisDirection::GEOCENTRIC_Y, .name = "geocentricY"},
      {.dir = &AxisDirection::GEOCENTRIC_Z, .name = "geocentricZ"},
      {.dir = &AxisDirection::COLUMN_POSITIVE, .name = "columnPositive"},
      {.dir = &AxisDirection::COLUMN_NEGATIVE, .name = "columnNegative"},
      {.dir = &AxisDirection::ROW_POSITIVE, .name = "rowPositive"},
      {.dir = &AxisDirection::ROW_NEGATIVE, .name = "rowNegative"},
      {.dir = &AxisDirection::DISPLAY_RIGHT, .name = "displayRight"},
      {.dir = &AxisDirection::DISPLAY_LEFT, .name = "displayLeft"},
      {.dir = &AxisDirection::DISPLAY_UP, .name = "displayUp"},
      {.dir = &AxisDirection::DISPLAY_DOWN, .name = "displayDown"},
      {.dir = &AxisDirection::FORWARD, .name = "forward"},
      {.dir = &AxisDirection::AFT, .name = "aft"},
      {.dir = &AxisDirection::PORT, .name = "port"},
      {.dir = &AxisDirection::STARBOARD, .name = "starboard"},
      {.dir = &AxisDirection::CLOCKWISE, .name = "clockwise"},
      {.dir = &AxisDirection::COUNTER_CLOCKWISE, .name = "counterClockwise"},
      {.dir = &AxisDirection::TOWARDS, .name = "towards"},
      {.dir = &AxisDirection::AWAY_FROM, .name = "awayFrom"},
      {.dir = &AxisDirection::FUTURE, .name = "future"},
      {.dir = &AxisDirection::PAST, .name = "past"},
      {.dir = &AxisDirection::UNSPECIFIED, .name = "unspecified"},
  };
  for (const auto& tc : kAxisDirs) {
    EXPECT_EQ(tc.name, tc.dir->toString());
    EXPECT_EQ(tc.dir, AxisDirection::valueOf(std::string(tc.name))) << tc.name;
  }
  EXPECT_EQ(nullptr, AxisDirection::valueOf("nonexistent_direction"));

  // 2. RangeMeaning registry and constants.
  EXPECT_EQ("exact", RangeMeaning::EXACT.toString());
  EXPECT_EQ(&RangeMeaning::EXACT, RangeMeaning::valueOf("exact"));
  EXPECT_EQ("wraparound", RangeMeaning::WRAPAROUND.toString());
  EXPECT_EQ(&RangeMeaning::WRAPAROUND, RangeMeaning::valueOf("wraparound"));
  EXPECT_EQ(nullptr, RangeMeaning::valueOf("unknown"));

  // 3. AxisDirectionWKT1 registry and constants.
  struct AxisDirWkt1Case {
    const AxisDirectionWKT1* dir;
    std::string_view name;
  };
  constexpr AxisDirWkt1Case kWkt1Dirs[] = {
      {.dir = &AxisDirectionWKT1::NORTH, .name = "NORTH"},
      {.dir = &AxisDirectionWKT1::EAST, .name = "EAST"},
      {.dir = &AxisDirectionWKT1::SOUTH, .name = "SOUTH"},
      {.dir = &AxisDirectionWKT1::WEST, .name = "WEST"},
      {.dir = &AxisDirectionWKT1::UP, .name = "UP"},
      {.dir = &AxisDirectionWKT1::DOWN, .name = "DOWN"},
      {.dir = &AxisDirectionWKT1::OTHER, .name = "OTHER"},
  };
  for (const auto& tc : kWkt1Dirs) {
    EXPECT_EQ(tc.name, tc.dir->toString());
    EXPECT_EQ(tc.dir, AxisDirectionWKT1::valueOf(std::string(tc.name)));
  }
  EXPECT_EQ(nullptr, AxisDirectionWKT1::valueOf("INVALID"));

  // 4. AxisName and AxisAbbreviation constants.
  constexpr StringConstantCase kAxisStrings[] = {
      {.actual = &AxisName::Longitude, .expected = "Longitude"},
      {.actual = &AxisName::Latitude, .expected = "Latitude"},
      {.actual = &AxisName::Easting, .expected = "Easting"},
      {.actual = &AxisName::Northing, .expected = "Northing"},
      {.actual = &AxisName::Westing, .expected = "Westing"},
      {.actual = &AxisName::Southing, .expected = "Southing"},
      {.actual = &AxisName::Ellipsoidal_height,
       .expected = "Ellipsoidal height"},
      {.actual = &AxisName::Geocentric_X, .expected = "Geocentric X"},
      {.actual = &AxisName::Geocentric_Y, .expected = "Geocentric Y"},
      {.actual = &AxisName::Geocentric_Z, .expected = "Geocentric Z"},
      {.actual = &AxisAbbreviation::lon, .expected = "lon"},
      {.actual = &AxisAbbreviation::lat, .expected = "lat"},
      {.actual = &AxisAbbreviation::E, .expected = "E"},
      {.actual = &AxisAbbreviation::N, .expected = "N"},
      {.actual = &AxisAbbreviation::h, .expected = "h"},
      {.actual = &AxisAbbreviation::X, .expected = "X"},
      {.actual = &AxisAbbreviation::Y, .expected = "Y"},
      {.actual = &AxisAbbreviation::Z, .expected = "Z"},
  };
  for (const auto& tc : kAxisStrings) {
    EXPECT_EQ(tc.expected, *tc.actual);
  }
}

TEST(StaticTest, RealizationMethodPrimeMeridianEllipsoidAndReferenceFrames) {
  // 1. RealizationMethod constants.
  EXPECT_EQ("levelling", RealizationMethod::LEVELLING.toString());
  EXPECT_EQ("geoid", RealizationMethod::GEOID.toString());
  EXPECT_EQ("tidal", RealizationMethod::TIDAL.toString());

  // 2. PrimeMeridian constants.
  EXPECT_EQ("Greenwich", PrimeMeridian::GREENWICH->nameStr());
  EXPECT_EQ(8901, PrimeMeridian::GREENWICH->getEPSGCode());
  EXPECT_DOUBLE_EQ(0.0, PrimeMeridian::GREENWICH->longitude().value());
  EXPECT_EQ(UnitOfMeasure::DEGREE,
            PrimeMeridian::GREENWICH->longitude().unit());

  EXPECT_EQ("Reference meridian", PrimeMeridian::REFERENCE_MERIDIAN->nameStr());
  EXPECT_EQ(0, PrimeMeridian::REFERENCE_MERIDIAN->getEPSGCode());
  EXPECT_TRUE(PrimeMeridian::REFERENCE_MERIDIAN->identifiers().empty());
  EXPECT_DOUBLE_EQ(0.0, PrimeMeridian::REFERENCE_MERIDIAN->longitude().value());

  EXPECT_EQ("Paris", PrimeMeridian::PARIS->nameStr());
  EXPECT_EQ(8903, PrimeMeridian::PARIS->getEPSGCode());
  EXPECT_DOUBLE_EQ(2.5969213, PrimeMeridian::PARIS->longitude().value());
  EXPECT_EQ(UnitOfMeasure::GRAD, PrimeMeridian::PARIS->longitude().unit());

  // 3. Ellipsoid constants.
  EXPECT_EQ("Clarke 1866", Ellipsoid::CLARKE_1866->nameStr());
  EXPECT_EQ(7008, Ellipsoid::CLARKE_1866->getEPSGCode());
  EXPECT_DOUBLE_EQ(6378206.4, Ellipsoid::CLARKE_1866->semiMajorAxis().value());
  ASSERT_TRUE(Ellipsoid::CLARKE_1866->semiMinorAxis().has_value());
  EXPECT_DOUBLE_EQ(6356583.8, Ellipsoid::CLARKE_1866->semiMinorAxis()->value());
  EXPECT_FALSE(Ellipsoid::CLARKE_1866->inverseFlattening().has_value());
  EXPECT_FALSE(Ellipsoid::CLARKE_1866->isSphere());
  EXPECT_EQ(Ellipsoid::EARTH, Ellipsoid::CLARKE_1866->celestialBody());

  EXPECT_EQ("WGS 84", Ellipsoid::WGS84->nameStr());
  EXPECT_EQ(7030, Ellipsoid::WGS84->getEPSGCode());
  EXPECT_DOUBLE_EQ(6378137.0, Ellipsoid::WGS84->semiMajorAxis().value());
  ASSERT_TRUE(Ellipsoid::WGS84->inverseFlattening().has_value());
  EXPECT_DOUBLE_EQ(298.257223563,
                   Ellipsoid::WGS84->inverseFlattening()->value());
  EXPECT_FALSE(Ellipsoid::WGS84->semiMinorAxis().has_value());
  EXPECT_FALSE(Ellipsoid::WGS84->isSphere());

  EXPECT_EQ("GRS 1980", Ellipsoid::GRS1980->nameStr());
  EXPECT_EQ(7019, Ellipsoid::GRS1980->getEPSGCode());
  EXPECT_DOUBLE_EQ(6378137.0, Ellipsoid::GRS1980->semiMajorAxis().value());
  ASSERT_TRUE(Ellipsoid::GRS1980->inverseFlattening().has_value());
  EXPECT_DOUBLE_EQ(298.257222101,
                   Ellipsoid::GRS1980->inverseFlattening()->value());
  EXPECT_FALSE(Ellipsoid::GRS1980->semiMinorAxis().has_value());

  // 4. GeodeticReferenceFrame constants.
  EXPECT_EQ("North American Datum 1927",
            GeodeticReferenceFrame::EPSG_6267->nameStr());
  EXPECT_EQ(6267, GeodeticReferenceFrame::EPSG_6267->getEPSGCode());
  EXPECT_EQ(Ellipsoid::CLARKE_1866.get(),
            GeodeticReferenceFrame::EPSG_6267->ellipsoid().get());
  EXPECT_EQ(PrimeMeridian::GREENWICH.get(),
            GeodeticReferenceFrame::EPSG_6267->primeMeridian().get());

  EXPECT_EQ("North American Datum 1983",
            GeodeticReferenceFrame::EPSG_6269->nameStr());
  EXPECT_EQ(6269, GeodeticReferenceFrame::EPSG_6269->getEPSGCode());
  EXPECT_EQ(Ellipsoid::GRS1980.get(),
            GeodeticReferenceFrame::EPSG_6269->ellipsoid().get());
  EXPECT_EQ(PrimeMeridian::GREENWICH.get(),
            GeodeticReferenceFrame::EPSG_6269->primeMeridian().get());

  EXPECT_EQ("World Geodetic System 1984",
            GeodeticReferenceFrame::EPSG_6326->nameStr());
  EXPECT_EQ(6326, GeodeticReferenceFrame::EPSG_6326->getEPSGCode());
  EXPECT_EQ(Ellipsoid::WGS84.get(),
            GeodeticReferenceFrame::EPSG_6326->ellipsoid().get());
  EXPECT_EQ(PrimeMeridian::GREENWICH.get(),
            GeodeticReferenceFrame::EPSG_6326->primeMeridian().get());
}

TEST(StaticTest, PredefinedCrssAndNullMeasure) {
  // 1. GeodeticCRS::EPSG_4978 (geocentric WGS 84).
  EXPECT_EQ("WGS 84", GeodeticCRS::EPSG_4978->nameStr());
  EXPECT_EQ(4978, GeodeticCRS::EPSG_4978->getEPSGCode());
  EXPECT_TRUE(GeodeticCRS::EPSG_4978->isGeocentric());
  EXPECT_EQ(GeodeticReferenceFrame::EPSG_6326.get(),
            GeodeticCRS::EPSG_4978->datum().get());
  const auto& geoc_axes =
      GeodeticCRS::EPSG_4978->coordinateSystem()->axisList();
  ASSERT_EQ(3u, geoc_axes.size());
  EXPECT_EQ(UnitOfMeasure::METRE, geoc_axes[0]->unit());

  // 2. GeographicCRS::EPSG_4267 (NAD27, lat/lon deg).
  EXPECT_EQ("NAD27", GeographicCRS::EPSG_4267->nameStr());
  EXPECT_EQ(4267, GeographicCRS::EPSG_4267->getEPSGCode());
  EXPECT_EQ(GeodeticReferenceFrame::EPSG_6267.get(),
            GeographicCRS::EPSG_4267->datum().get());
  ASSERT_EQ(2u,
            GeographicCRS::EPSG_4267->coordinateSystem()->axisList().size());

  // 3. GeographicCRS::EPSG_4269 (NAD83, lat/lon deg).
  EXPECT_EQ("NAD83", GeographicCRS::EPSG_4269->nameStr());
  EXPECT_EQ(4269, GeographicCRS::EPSG_4269->getEPSGCode());
  EXPECT_EQ(GeodeticReferenceFrame::EPSG_6269.get(),
            GeographicCRS::EPSG_4269->datum().get());

  // 4. GeographicCRS::EPSG_4326 (WGS 84 2D, lat/lon deg).
  EXPECT_EQ("WGS 84", GeographicCRS::EPSG_4326->nameStr());
  EXPECT_EQ(4326, GeographicCRS::EPSG_4326->getEPSGCode());
  EXPECT_EQ(GeodeticReferenceFrame::EPSG_6326.get(),
            GeographicCRS::EPSG_4326->datum().get());
  const auto& epsg4326_axes =
      GeographicCRS::EPSG_4326->coordinateSystem()->axisList();
  ASSERT_EQ(2u, epsg4326_axes.size());
  EXPECT_EQ(AxisDirection::NORTH, epsg4326_axes[0]->direction());
  EXPECT_EQ(AxisDirection::EAST, epsg4326_axes[1]->direction());
  EXPECT_EQ(UnitOfMeasure::DEGREE, epsg4326_axes[0]->unit());

  // 5. GeographicCRS::OGC_CRS84 (WGS 84 2D, lon/lat deg).
  EXPECT_EQ("WGS 84 (CRS84)", GeographicCRS::OGC_CRS84->nameStr());
  EXPECT_EQ(0, GeographicCRS::OGC_CRS84->getEPSGCode());
  ASSERT_EQ(1u, GeographicCRS::OGC_CRS84->identifiers().size());
  ASSERT_TRUE(
      GeographicCRS::OGC_CRS84->identifiers()[0]->codeSpace().has_value());
  EXPECT_EQ("OGC", *GeographicCRS::OGC_CRS84->identifiers()[0]->codeSpace());
  EXPECT_EQ("CRS84", GeographicCRS::OGC_CRS84->identifiers()[0]->code());
  const auto& crs84_axes =
      GeographicCRS::OGC_CRS84->coordinateSystem()->axisList();
  ASSERT_EQ(2u, crs84_axes.size());
  EXPECT_EQ(AxisDirection::EAST, crs84_axes[0]->direction());
  EXPECT_EQ(AxisDirection::NORTH, crs84_axes[1]->direction());

  // 6. GeographicCRS::EPSG_4807 (NTF Paris, lat/lon grad, Paris PM).
  EXPECT_EQ("NTF (Paris)", GeographicCRS::EPSG_4807->nameStr());
  EXPECT_EQ(4807, GeographicCRS::EPSG_4807->getEPSGCode());
  EXPECT_EQ(6807, GeographicCRS::EPSG_4807->datum()->getEPSGCode());
  EXPECT_EQ(PrimeMeridian::PARIS.get(),
            GeographicCRS::EPSG_4807->primeMeridian().get());
  EXPECT_EQ("Clarke 1880 (IGN)",
            GeographicCRS::EPSG_4807->ellipsoid()->nameStr());
  EXPECT_EQ(7011, GeographicCRS::EPSG_4807->ellipsoid()->getEPSGCode());
  const auto& ntf_axes =
      GeographicCRS::EPSG_4807->coordinateSystem()->axisList();
  ASSERT_EQ(2u, ntf_axes.size());
  EXPECT_EQ(UnitOfMeasure::GRAD, ntf_axes[0]->unit());

  // 7. GeographicCRS::EPSG_4979 (WGS 84 3D, lat/lon deg + ellipsoidal h m).
  EXPECT_EQ("WGS 84", GeographicCRS::EPSG_4979->nameStr());
  EXPECT_EQ(4979, GeographicCRS::EPSG_4979->getEPSGCode());
  EXPECT_EQ(GeodeticReferenceFrame::EPSG_6326.get(),
            GeographicCRS::EPSG_4979->datum().get());
  const auto& epsg4979_axes =
      GeographicCRS::EPSG_4979->coordinateSystem()->axisList();
  ASSERT_EQ(3u, epsg4979_axes.size());
  EXPECT_EQ(UnitOfMeasure::DEGREE, epsg4979_axes[0]->unit());
  EXPECT_EQ(AxisDirection::UP, epsg4979_axes[2]->direction());
  EXPECT_EQ(UnitOfMeasure::METRE, epsg4979_axes[2]->unit());

  // 8. operation::nullMeasure is default-constructed (0.0, empty UNKNOWN unit).
  EXPECT_DOUBLE_EQ(0.0, nullMeasure.value());
  EXPECT_EQ(UnitOfMeasure(), nullMeasure.unit());
  EXPECT_EQ(UnitOfMeasure::Type::UNKNOWN, nullMeasure.unit().type());
}

}  // namespace
}  // namespace proj
