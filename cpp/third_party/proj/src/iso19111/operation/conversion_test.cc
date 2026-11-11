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
// Unit tests for ISO 19111 Conversion (factory methods, cloning, linear-unit
// alteration, inversion, method equivalence conversion, WKT/JSON/PROJ export,
// UTM identification) in proj/src/iso19111/operation/conversion.cpp.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/conversion.cpp

#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinateoperation.hpp"
#include "third_party/proj4/proj/include/proj/coordinatesystem.hpp"
#include "third_party/proj4/proj/include/proj/crs.hpp"
#include "third_party/proj4/proj/include/proj/datum.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/iso19111/operation/coordinateoperation_internal.hpp"
#include "third_party/proj4/proj/src/iso19111/operation/oputils.hpp"
#include "third_party/proj4/proj/src/iso19111/operation/parammappings.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_constants.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

namespace common = NS_PROJ::common;
namespace crs = NS_PROJ::crs;
namespace cs = NS_PROJ::cs;
namespace datum = NS_PROJ::datum;
namespace io = NS_PROJ::io;
namespace metadata = NS_PROJ::metadata;
namespace op = NS_PROJ::operation;
namespace util = NS_PROJ::util;

using ::testing::NotNull;

common::Angle Deg(double val) {
  return common::Angle(val, common::UnitOfMeasure::DEGREE);
}

common::Length Metre(double val) {
  return common::Length(val, common::UnitOfMeasure::METRE);
}

common::Scale Scale(double val) {
  return common::Scale(val, common::UnitOfMeasure::SCALE_UNITY);
}

util::PropertyMap NameProps(const std::string& name) {
  return util::PropertyMap().set(common::IdentifiedObject::NAME_KEY, name);
}

bool ContainsSubstr(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

struct CoordOpPeer : public op::CoordinateOperation {
  static void SetCRSs(op::CoordinateOperation* op_ptr, const crs::CRSNNPtr& src,
                      const crs::CRSNNPtr& dst, const crs::CRSPtr& interp) {
    using Fn = void (op::CoordinateOperation::*)(
        const crs::CRSNNPtr&, const crs::CRSNNPtr&, const crs::CRSPtr&);
    Fn fn = &CoordOpPeer::setCRSs;
    (op_ptr->*fn)(src, dst, interp);
  }
};

crs::ProjectedCRSNNPtr MakeProjectedCRS(const std::string& name,
                                        const crs::GeodeticCRSNNPtr& baseCRS,
                                        const op::ConversionNNPtr& conv,
                                        const cs::CartesianCSNNPtr& cartCS,
                                        bool hasOver = false) {
  auto props = NameProps(name);
  if (hasOver) {
    props.set("OVER", true);
  }
  return crs::ProjectedCRS::create(props, baseCRS, conv, cartCS);
}

crs::ProjectedCRSNNPtr MakeWGS84ProjectedCRS(
    const std::string& name, const op::ConversionNNPtr& conv,
    const cs::CartesianCSNNPtr& cartCS =
        cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE),
    bool hasOver = false) {
  return MakeProjectedCRS(name, crs::GeographicCRS::EPSG_4326, conv, cartCS,
                          hasOver);
}

crs::GeographicCRSNNPtr MakeSphericalGeogCRS(const std::string& name,
                                             double radius = 6371000.0) {
  auto ellps =
      datum::Ellipsoid::createSphere(NameProps("Sphere"), Metre(radius));
  auto datumObj = datum::GeodeticReferenceFrame::create(
      NameProps("Sphere datum"), ellps, util::optional<std::string>(),
      datum::PrimeMeridian::GREENWICH);
  return crs::GeographicCRS::create(NameProps(name), datumObj,
                                    cs::EllipsoidalCS::createLatitudeLongitude(
                                        common::UnitOfMeasure::DEGREE));
}

crs::GeodeticCRSNNPtr MakePlanetocentricCRS(const std::string& name) {
  auto ellps = datum::Ellipsoid::createFlattenedSphere(
      NameProps("Mars Ellipsoid"), Metre(3396190.0), Scale(169.8944472236118),
      "Mars");
  auto datumObj = datum::GeodeticReferenceFrame::create(
      NameProps("Mars datum"), ellps, util::optional<std::string>(),
      datum::PrimeMeridian::GREENWICH);
  auto latAxis = cs::CoordinateSystemAxis::create(
      NameProps("planetocentric latitude"), "lat", cs::AxisDirection::NORTH,
      common::UnitOfMeasure::DEGREE);
  auto lonAxis = cs::CoordinateSystemAxis::create(
      NameProps("planetocentric longitude"), "lon", cs::AxisDirection::EAST,
      common::UnitOfMeasure::DEGREE);
  auto sphCS =
      cs::SphericalCS::create(NameProps("Planetocentric"), latAxis, lonAxis);
  return crs::GeodeticCRS::create(NameProps(name), datumObj, sphCS);
}

crs::VerticalCRSNNPtr MakeVerticalCRS(const std::string& name,
                                      const common::UnitOfMeasure& unit) {
  auto vDatum = datum::VerticalReferenceFrame::create(NameProps("VDatum"));
  auto vCS = cs::VerticalCS::createGravityRelatedHeight(unit);
  return crs::VerticalCRS::create(NameProps(name), vDatum, vCS);
}

crs::TemporalCRSNNPtr MakeTemporalCRS(const std::string& name) {
  auto tDatum = datum::TemporalDatum::create(
      NameProps("TDatum"), common::DateTime::create("2000-01-01T00:00:00Z"),
      datum::TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN);
  auto tAxis = cs::CoordinateSystemAxis::create(
      NameProps("Time"), "T", cs::AxisDirection::FUTURE,
      common::UnitOfMeasure("day", 86400.0, common::UnitOfMeasure::Type::TIME));
  auto tCS = cs::TemporalCountCS::create(NameProps("TCS"), tAxis);
  return crs::TemporalCRS::create(NameProps(name), tDatum, tCS);
}

op::ConversionNNPtr MakeManualConversion(
    const util::PropertyMap& convProps, const util::PropertyMap& methodProps,
    const std::vector<op::OperationParameterNNPtr>& params,
    const std::vector<op::ParameterValueNNPtr>& values) {
  return op::Conversion::create(convProps, methodProps, params, values);
}

op::OperationParameterNNPtr MakeParam(const std::string& name, int epsg = 0) {
  auto props = NameProps(name);
  if (epsg != 0) {
    props.set(metadata::Identifier::CODESPACE_KEY, metadata::Identifier::EPSG)
        .set(metadata::Identifier::CODE_KEY, epsg);
  }
  return op::OperationParameter::create(props);
}

TEST(ConversionTest, CreateValidationShallowCloneAndUTMBranches) {
  // 1. Both Conversion::create overloads throw on parameter/value mismatch.
  auto param = op::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN);
  std::vector<op::GeneralOperationParameterNNPtr> methodParams{
      util::nn_static_pointer_cast<op::GeneralOperationParameter>(param)};
  auto method = op::OperationMethod::create(
      op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_MERCATOR_VARIANT_A),
      methodParams);
  EXPECT_THROW(
      op::Conversion::create(NameProps("bad"), method,
                             std::vector<op::GeneralParameterValueNNPtr>{}),
      op::InvalidOperation);

  EXPECT_THROW(
      op::Conversion::create(NameProps("bad2"), NameProps("Custom method"),
                             std::vector<op::OperationParameterNNPtr>{param},
                             std::vector<op::ParameterValueNNPtr>{}),
      op::InvalidOperation);

  // 2. createUTM invalid zone throws; valid north and south create expected
  // IDs.
  EXPECT_THROW(op::Conversion::createUTM(util::PropertyMap(), 0, true),
               op::InvalidOperation);
  EXPECT_THROW(op::Conversion::createUTM(util::PropertyMap(), 61, false),
               op::InvalidOperation);
  EXPECT_THROW(op::Conversion::createUTM(util::PropertyMap(), -3, true),
               op::InvalidOperation);

  auto utm31N = op::Conversion::createUTM(util::PropertyMap(), 31, true);
  EXPECT_EQ("UTM zone 31N", utm31N->nameStr());
  EXPECT_EQ(16031, utm31N->getEPSGCode());

  auto utm19S = op::Conversion::createUTM(util::PropertyMap(), 19, false);
  EXPECT_EQ("UTM zone 19S", utm19S->nameStr());
  EXPECT_EQ(16119, utm19S->getEPSGCode());

  auto utmCustom = op::Conversion::createUTM(NameProps("Custom UTM"), 10, true);
  EXPECT_EQ("Custom UTM", utmCustom->nameStr());
  EXPECT_EQ(0, utmCustom->getEPSGCode());

  // 3. shallowClone() and CoordinateOperation::_shallowClone() preserve CRSs.
  auto projCRS = MakeWGS84ProjectedCRS("UTM 31N CRS", utm31N);
  auto deriving = projCRS->derivingConversion();
  auto cloned = deriving->shallowClone();
  EXPECT_EQ(deriving->nameStr(), cloned->nameStr());
  EXPECT_EQ(crs::GeographicCRS::EPSG_4326.get(), cloned->sourceCRS().get());
  EXPECT_EQ(projCRS.get(), cloned->targetCRS().get());

  const op::CoordinateOperation* baseOp = deriving.get();
  auto baseCloned = baseOp->shallowClone();
  EXPECT_EQ(deriving->nameStr(), baseCloned->nameStr());
}

TEST(ConversionTest, AlterParametersLinearUnitBranches) {
  auto tm = op::Conversion::createTransverseMercator(
      NameProps("TM"), Deg(0.0), Deg(3.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  auto projCRS = MakeWGS84ProjectedCRS("TM CRS", tm);
  auto deriving = projCRS->derivingConversion();

  // 1. No-op when linear parameters already use METRE.
  auto unchanged =
      deriving->alterParametersLinearUnit(common::UnitOfMeasure::METRE, true);
  EXPECT_EQ(deriving.get(), unchanged.get());

  // 2. convertToNewUnit == true converts metres to international feet.
  auto inFeetConverted =
      deriving->alterParametersLinearUnit(common::UnitOfMeasure::FOOT, true);
  EXPECT_NE(deriving.get(), inFeetConverted.get());
  EXPECT_EQ("unknown", inFeetConverted->nameStr());
  EXPECT_NEAR(
      500000.0 / 0.3048,
      inFeetConverted->parameterValueNumeric(EPSG_CODE_PARAMETER_FALSE_EASTING,
                                             common::UnitOfMeasure::FOOT),
      1e-6);
  EXPECT_EQ(projCRS.get(), inFeetConverted->targetCRS().get());

  // 3. convertToNewUnit == false replaces unit while keeping raw numeric value.
  auto inFeetRaw =
      deriving->alterParametersLinearUnit(common::UnitOfMeasure::FOOT, false);
  EXPECT_DOUBLE_EQ(
      500000.0,
      inFeetRaw->parameterValueMeasure(EPSG_CODE_PARAMETER_FALSE_EASTING)
          .value());
  EXPECT_EQ(common::UnitOfMeasure::FOOT,
            inFeetRaw->parameterValueMeasure(EPSG_CODE_PARAMETER_FALSE_EASTING)
                .unit());

  // 4. Non-measure integer parameter and already-matching linear unit remain
  // untouched while a different linear unit triggers updates.
  auto mixed = MakeManualConversion(
      NameProps("Mixed"), NameProps("Custom Mixed"),
      {MakeParam("LinearMetre", EPSG_CODE_PARAMETER_FALSE_EASTING),
       MakeParam("LinearFoot", EPSG_CODE_PARAMETER_FALSE_NORTHING),
       MakeParam("IntParam", 0)},
      {op::ParameterValue::create(Metre(100.0)),
       op::ParameterValue::create(
           common::Length(200.0, common::UnitOfMeasure::FOOT)),
       op::ParameterValue::create(4326)});
  auto mixedUpdated =
      mixed->alterParametersLinearUnit(common::UnitOfMeasure::FOOT, true);
  EXPECT_NEAR(
      100.0 / 0.3048,
      mixedUpdated->parameterValueNumeric(EPSG_CODE_PARAMETER_FALSE_EASTING,
                                          common::UnitOfMeasure::FOOT),
      1e-6);
  EXPECT_DOUBLE_EQ(200.0, mixedUpdated->parameterValueNumeric(
                              EPSG_CODE_PARAMETER_FALSE_NORTHING,
                              common::UnitOfMeasure::FOOT));
  EXPECT_EQ(4326, mixedUpdated->parameterValue("IntParam")->integerValue());
}

TEST(ConversionTest, AllFactoryMethodsInstantiateExpectedOperations) {
  const auto p = NameProps("conv");
  std::vector<op::ConversionNNPtr> created;

  created.push_back(op::Conversion::createUTM(p, 32, true));
  created.push_back(op::Conversion::createTransverseMercator(
      p, Deg(0.0), Deg(9.0), Scale(0.9996), Metre(500000.0), Metre(0.0)));
  created.push_back(op::Conversion::createGaussSchreiberTransverseMercator(
      p, Deg(-21.1167), Deg(55.5333), Scale(1.0), Metre(160000.0),
      Metre(50000.0)));
  created.push_back(op::Conversion::createTransverseMercatorSouthOriented(
      p, Deg(0.0), Deg(19.0), Scale(1.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createTwoPointEquidistant(
      p, Deg(18.0), Deg(-70.0), Deg(41.0), Deg(-74.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createTunisiaMappingGrid(
      p, Deg(36.5), Deg(9.5), Metre(500000.0), Metre(300000.0)));
  created.push_back(op::Conversion::createTunisiaMiningGrid(
      p, Deg(36.5), Deg(9.5), Metre(500.0), Metre(300.0)));
  created.push_back(op::Conversion::createAlbersEqualArea(
      p, Deg(23.0), Deg(-96.0), Deg(29.5), Deg(45.5), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createLambertConicConformal_1SP(
      p, Deg(46.5), Deg(3.0), Scale(0.999), Metre(700000.0), Metre(6600000.0)));
  created.push_back(op::Conversion::createLambertConicConformal_1SP_VariantB(
      p, Deg(45.0), Scale(0.999), Deg(44.0), Deg(2.0), Metre(100.0),
      Metre(200.0)));
  created.push_back(op::Conversion::createLambertConicConformal_2SP(
      p, Deg(46.5), Deg(3.0), Deg(44.0), Deg(49.0), Metre(700000.0),
      Metre(6600000.0)));
  created.push_back(op::Conversion::createLambertConicConformal_2SP_Michigan(
      p, Deg(43.3167), Deg(-84.3667), Deg(44.1833), Deg(45.7), Metre(1999996.0),
      Metre(0.0), Scale(1.0000382)));
  created.push_back(op::Conversion::createLambertConicConformal_2SP_Belgium(
      p, Deg(90.0), Deg(4.3569), Deg(49.8333), Deg(51.1667), Metre(150000.013),
      Metre(5400088.438)));
  created.push_back(op::Conversion::createAzimuthalEquidistant(
      p, Deg(53.0), Deg(10.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createGuamProjection(
      p, Deg(13.4725), Deg(144.7487), Metre(50000.0), Metre(50000.0)));
  created.push_back(op::Conversion::createBonne(p, Deg(40.0), Deg(0.0),
                                                Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createLambertCylindricalEqualAreaSpherical(
      p, Deg(30.0), Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createLambertCylindricalEqualArea(
      p, Deg(30.0), Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createCassiniSoldner(
      p, Deg(52.4), Deg(13.6), Metre(40000.0), Metre(10000.0)));
  created.push_back(op::Conversion::createEquidistantConic(
      p, Deg(40.0), Deg(-96.0), Deg(33.0), Deg(45.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createEckertI(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createEckertII(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createEckertIII(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createEckertIV(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createEckertV(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createEckertVI(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createEquidistantCylindrical(
      p, Deg(0.0), Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createEquidistantCylindricalSpherical(
      p, Deg(0.0), Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createGall(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createGoodeHomolosine(
      p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createInterruptedGoodeHomolosine(
      p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createGeostationarySatelliteSweepX(
      p, Deg(0.0), Metre(35785831.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createGeostationarySatelliteSweepY(
      p, Deg(-75.0), Metre(35786023.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createGnomonic(p, Deg(40.0), Deg(-100.0),
                                                   Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createHotineObliqueMercatorVariantA(
      p, Deg(4.0), Deg(115.0), Deg(53.3158), Deg(53.1301), Scale(0.99984),
      Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createHotineObliqueMercatorVariantB(
      p, Deg(4.0), Deg(115.0), Deg(53.3158), Deg(53.1301), Scale(0.99984),
      Metre(590476.87), Metre(442857.65)));
  created.push_back(
      op::Conversion::createHotineObliqueMercatorTwoPointNaturalOrigin(
          p, Deg(40.0), Deg(35.0), Deg(-120.0), Deg(45.0), Deg(-115.0),
          Scale(0.9999), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createLabordeObliqueMercator(
      p, Deg(-18.9), Deg(46.4372), Deg(18.9), Scale(0.9995), Metre(400000.0),
      Metre(800000.0)));
  created.push_back(op::Conversion::createInternationalMapWorldPolyconic(
      p, Deg(-60.0), Deg(0.0), Deg(4.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createKrovakNorthOriented(
      p, Deg(49.5), Deg(24.83333333333333), Deg(30.288139722222223), Deg(78.5),
      Scale(0.9999), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createKrovak(
      p, Deg(49.5), Deg(24.83333333333333), Deg(30.288139722222223), Deg(78.5),
      Scale(0.9999), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createLambertAzimuthalEqualArea(
      p, Deg(52.0), Deg(10.0), Metre(4321000.0), Metre(3210000.0)));
  created.push_back(op::Conversion::createMillerCylindrical(
      p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createMercatorVariantA(
      p, Deg(0.0), Deg(0.0), Scale(1.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createMercatorVariantB(
      p, Deg(30.0), Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createPopularVisualisationPseudoMercator(
      p, Deg(0.0), Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createMercatorSpherical(
      p, Deg(0.0), Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createMollweide(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createNewZealandMappingGrid(
      p, Deg(-41.0), Deg(173.0), Metre(2510000.0), Metre(6023150.0)));
  created.push_back(op::Conversion::createObliqueStereographic(
      p, Deg(52.15616), Deg(5.38764), Scale(0.9999079), Metre(155000.0),
      Metre(463000.0)));
  created.push_back(op::Conversion::createOrthographic(p, Deg(45.0), Deg(-90.0),
                                                       Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createLocalOrthographic(
      p, Deg(45.0), Deg(-90.0), Deg(15.0), Scale(1.0), Metre(1000.0),
      Metre(2000.0)));
  created.push_back(op::Conversion::createAmericanPolyconic(
      p, Deg(0.0), Deg(-54.0), Metre(5000000.0), Metre(10000000.0)));
  created.push_back(op::Conversion::createPolarStereographicVariantA(
      p, Deg(90.0), Deg(0.0), Scale(0.994), Metre(2000000.0),
      Metre(2000000.0)));
  created.push_back(op::Conversion::createPolarStereographicVariantB(
      p, Deg(71.0), Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createRobinson(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createSinusoidal(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createStereographic(
      p, Deg(40.0), Deg(-100.0), Scale(1.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createVanDerGrinten(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createWagnerI(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createWagnerII(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createWagnerIII(p, Deg(30.0), Deg(0.0),
                                                    Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createWagnerIV(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createWagnerV(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createWagnerVI(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(
      op::Conversion::createWagnerVII(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createQuadrilateralizedSphericalCube(
      p, Deg(90.0), Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createSphericalCrossTrackHeight(
      p, Deg(34.0), Deg(-118.0), Deg(45.0), Metre(1000.0)));
  created.push_back(
      op::Conversion::createEqualEarth(p, Deg(0.0), Metre(0.0), Metre(0.0)));
  created.push_back(op::Conversion::createVerticalPerspective(
      p, Deg(55.0), Deg(5.0), Metre(0.0), Metre(35785831.0), Metre(0.0),
      Metre(0.0)));
  created.push_back(op::Conversion::createPoleRotationGRIBConvention(
      p, Deg(-35.0), Deg(15.0), Deg(10.0)));
  created.push_back(op::Conversion::createPoleRotationNetCDFCFConvention(
      p, Deg(39.25), Deg(-162.0), Deg(0.0)));
  created.push_back(op::Conversion::createChangeVerticalUnit(p, Scale(0.3048)));
  created.push_back(op::Conversion::createChangeVerticalUnit(p));
  created.push_back(op::Conversion::createHeightDepthReversal(p));
  created.push_back(op::Conversion::createAxisOrderReversal(false));
  created.push_back(op::Conversion::createAxisOrderReversal(true));
  created.push_back(op::Conversion::createGeographicGeocentric(p));
  created.push_back(op::Conversion::createGeographicGeocentric(
      crs::GeographicCRS::EPSG_4979, crs::GeodeticCRS::EPSG_4978));
  created.push_back(op::Conversion::createGeographicGeocentricLatitude(
      crs::GeographicCRS::EPSG_4326, MakePlanetocentricCRS("Mars")));
  created.push_back(
      op::Conversion::createGeographic2DOffsets(p, Deg(0.001), Deg(0.002)));
  created.push_back(op::Conversion::createGeographic3DOffsets(
      p, Deg(0.001), Deg(0.002), Metre(5.0)));
  created.push_back(op::Conversion::createGeographic2DWithHeightOffsets(
      p, Deg(0.001), Deg(0.002), Metre(12.5)));
  created.push_back(op::Conversion::createVerticalOffset(p, Metre(2.5)));
  created.push_back(op::Conversion::createAffineParametric(
      p, Metre(1.0), Scale(2.0), Scale(3.0), Metre(4.0), Scale(5.0),
      Scale(6.0)));
  created.push_back(op::Conversion::createAffineParametric(
      p, Metre(1.0), Scale(2.0), Scale(3.0), Scale(4.0), Metre(5.0), Scale(6.0),
      Scale(7.0), Scale(8.0), Metre(9.0), Scale(10.0), Scale(11.0),
      Scale(12.0)));

  EXPECT_EQ(86u, created.size());
  for (const auto& conv : created) {
    EXPECT_FALSE(conv->method()->nameStr().empty());
  }
}

TEST(ConversionTest, InverseConversionBranches) {
  // 1. Change Vertical Unit (with scalar): factor == 0 throws; non-zero
  // inverts.
  auto zeroZ =
      op::Conversion::createChangeVerticalUnit(NameProps("zeroZ"), Scale(0.0));
  EXPECT_THROW(zeroZ->inverse(), op::InvalidOperation);

  auto vSrc = MakeVerticalCRS("VertFeet", common::UnitOfMeasure::FOOT);
  auto vertConvScalar = op::Conversion::createChangeVerticalUnit(
      NameProps("ft2m"), Scale(0.3048));
  auto derivVertScalar = crs::DerivedVerticalCRS::create(
      NameProps("VertMetres"), vSrc, vertConvScalar,
      cs::VerticalCS::createGravityRelatedHeight(common::UnitOfMeasure::METRE));
  auto invScalar = std::dynamic_pointer_cast<op::Conversion>(
      derivVertScalar->derivingConversion()->inverse().as_nullable());
  ASSERT_THAT(invScalar, NotNull());
  EXPECT_NEAR(1.0 / 0.3048,
              invScalar->parameterValueNumericAsSI(
                  EPSG_CODE_PARAMETER_UNIT_CONVERSION_SCALAR),
              1e-12);
  EXPECT_EQ(derivVertScalar.get(), invScalar->sourceCRS().get());
  EXPECT_EQ(vSrc.get(), invScalar->targetCRS().get());

  // 2. Change Vertical Unit (no explicit factor).
  auto vertNoFactor =
      op::Conversion::createChangeVerticalUnit(NameProps("NoFactor"));
  auto derivVertNoFactor = crs::DerivedVerticalCRS::create(
      NameProps("VertMetresNoFactor"), vSrc, vertNoFactor,
      cs::VerticalCS::createGravityRelatedHeight(common::UnitOfMeasure::METRE));
  auto invNoFactor = std::dynamic_pointer_cast<op::Conversion>(
      derivVertNoFactor->derivingConversion()->inverse().as_nullable());
  ASSERT_THAT(invNoFactor, NotNull());
  EXPECT_EQ(EPSG_CODE_METHOD_CHANGE_VERTICAL_UNIT_NO_CONV_FACTOR,
            invNoFactor->method()->getEPSGCode());
  EXPECT_EQ(derivVertNoFactor.get(), invNoFactor->sourceCRS().get());

  // 3. Axis order reversal 2D & 3D self-inversion.
  auto rev2D = op::Conversion::createAxisOrderReversal(false);
  auto invRev2D =
      std::dynamic_pointer_cast<op::Conversion>(rev2D->inverse().as_nullable());
  ASSERT_THAT(invRev2D, NotNull());
  EXPECT_EQ(EPSG_CODE_METHOD_AXIS_ORDER_REVERSAL_2D,
            invRev2D->method()->getEPSGCode());

  auto rev3D = op::Conversion::createAxisOrderReversal(true);
  auto invRev3D =
      std::dynamic_pointer_cast<op::Conversion>(rev3D->inverse().as_nullable());
  ASSERT_THAT(invRev3D, NotNull());
  EXPECT_EQ(EPSG_CODE_METHOD_AXIS_ORDER_REVERSAL_3D,
            invRev3D->method()->getEPSGCode());

  // 4. Geographic/Geocentric & Height/Depth reversal & Geocentric Latitude.
  auto geogGeoc = op::Conversion::createGeographicGeocentric(
      crs::GeographicCRS::EPSG_4979, crs::GeodeticCRS::EPSG_4978);
  auto invGeogGeoc = std::dynamic_pointer_cast<op::Conversion>(
      geogGeoc->inverse().as_nullable());
  ASSERT_THAT(invGeogGeoc, NotNull());
  EXPECT_EQ(crs::GeodeticCRS::EPSG_4978.get(), invGeogGeoc->sourceCRS().get());
  EXPECT_EQ(crs::GeographicCRS::EPSG_4979.get(),
            invGeogGeoc->targetCRS().get());

  auto hdr = op::Conversion::createHeightDepthReversal(NameProps("HDR"));
  auto invHdr =
      std::dynamic_pointer_cast<op::Conversion>(hdr->inverse().as_nullable());
  ASSERT_THAT(invHdr, NotNull());
  EXPECT_EQ(EPSG_CODE_METHOD_HEIGHT_DEPTH_REVERSAL,
            invHdr->method()->getEPSGCode());

  auto mars = MakePlanetocentricCRS("Mars");
  auto geogLat = op::Conversion::createGeographicGeocentricLatitude(
      crs::GeographicCRS::EPSG_4326, mars);
  auto invGeogLat = std::dynamic_pointer_cast<op::Conversion>(
      geogLat->inverse().as_nullable());
  ASSERT_THAT(invGeogLat, NotNull());
  EXPECT_EQ(mars.get(), invGeogLat->sourceCRS().get());
  EXPECT_EQ(crs::GeographicCRS::EPSG_4326.get(), invGeogLat->targetCRS().get());

  // 5. Generic InverseConversion (clone, inverseAsConversion, double inverse,
  // exports, equivalence).
  auto tm = op::Conversion::createTransverseMercator(
      NameProps("Forward TM"), Deg(0.0), Deg(3.0), Scale(0.9996),
      Metre(500000.0), Metre(0.0));
  auto projCRS = MakeWGS84ProjectedCRS("TM CRS", tm);
  auto forward = projCRS->derivingConversion();

  auto invOp = forward->inverse();
  auto* invConv = dynamic_cast<op::InverseConversion*>(invOp.get());
  ASSERT_THAT(invConv, NotNull());
  EXPECT_EQ(forward.get(), invConv->inverseAsConversion().get());

  auto invCloned = invOp->shallowClone();
  EXPECT_TRUE(invConv->isEquivalentTo(invCloned.get()));
  EXPECT_TRUE(
      invConv->isEquivalentTo(invConv->inverse()->inverse().get(),
                              util::IComparable::Criterion::EQUIVALENT));
  EXPECT_EQ(forward.get(), invConv->inverse().get());

  auto wktFmt =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT2_2019);
  EXPECT_TRUE(ContainsSubstr(invConv->exportToWKT(wktFmt.get()), "Forward TM"));
  auto jsonFmt = io::JSONFormatter::create();
  EXPECT_TRUE(
      ContainsSubstr(invConv->exportToJSON(jsonFmt.get()), "Conversion"));
  auto projFmt = io::PROJStringFormatter::create();
  EXPECT_TRUE(
      ContainsSubstr(invConv->exportToPROJString(projFmt.get()), "+inv"));
}

TEST(ConversionTest, ConvertToOtherMethodBranches) {
  auto tm = op::Conversion::createTransverseMercator(
      NameProps("TM"), Deg(0.0), Deg(3.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  // 1. Self conversion returns same operation.
  EXPECT_EQ(
      tm.get(),
      tm->convertToOtherMethod(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR).get());

  // 2. Missing sourceCRS -> nullptr.
  EXPECT_EQ(
      nullptr,
      tm->convertToOtherMethod(EPSG_CODE_METHOD_MERCATOR_VARIANT_A).get());

  // 3. Unsupported method pair & prolate ellipsoid -> nullptr.
  auto tmProj = MakeWGS84ProjectedCRS("TM CRS", tm);
  EXPECT_EQ(nullptr,
            tmProj->derivingConversion()
                ->convertToOtherMethod(EPSG_CODE_METHOD_MERCATOR_VARIANT_A)
                .get());

  auto prolateEllps = datum::Ellipsoid::createTwoAxis(
      NameProps("Prolate"), Metre(6370000.0), Metre(6378000.0));
  auto prolateDatum = datum::GeodeticReferenceFrame::create(
      NameProps("ProlateDatum"), prolateEllps, util::optional<std::string>(),
      datum::PrimeMeridian::GREENWICH);
  auto prolateGeog =
      crs::GeographicCRS::create(NameProps("ProlateGeog"), prolateDatum,
                                 cs::EllipsoidalCS::createLatitudeLongitude(
                                     common::UnitOfMeasure::DEGREE));
  auto mercProlate = op::Conversion::createMercatorVariantA(
      NameProps("Merc"), Deg(0.0), Deg(0.0), Scale(1.0), Metre(0.0),
      Metre(0.0));
  auto projProlate = MakeProjectedCRS(
      "Merc Prolate", prolateGeog, mercProlate,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE));
  EXPECT_EQ(nullptr,
            projProlate->derivingConversion()
                ->convertToOtherMethod(EPSG_CODE_METHOD_MERCATOR_VARIANT_B)
                .get());

  // 4. Mercator Variant A -> Variant B.
  auto mercANonZeroLat = op::Conversion::createMercatorVariantA(
      NameProps("Merc"), Deg(10.0), Deg(0.0), Scale(1.0), Metre(0.0),
      Metre(0.0));
  auto crsMercLat = MakeWGS84ProjectedCRS("M1", mercANonZeroLat);
  EXPECT_EQ(nullptr,
            crsMercLat->derivingConversion()
                ->convertToOtherMethod(EPSG_CODE_METHOD_MERCATOR_VARIANT_B)
                .get());

  auto mercAZeroK = op::Conversion::createMercatorVariantA(
      NameProps("Merc"), Deg(0.0), Deg(0.0), Scale(0.0), Metre(0.0),
      Metre(0.0));
  auto crsMercK0 = MakeWGS84ProjectedCRS("M2", mercAZeroK);
  EXPECT_EQ(nullptr,
            crsMercK0->derivingConversion()
                ->convertToOtherMethod(EPSG_CODE_METHOD_MERCATOR_VARIANT_B)
                .get());

  auto mercABigK = op::Conversion::createMercatorVariantA(
      NameProps("Merc"), Deg(0.0), Deg(0.0), Scale(1.5), Metre(0.0),
      Metre(0.0));
  auto crsMercBigK = MakeWGS84ProjectedCRS("M3", mercABigK);
  EXPECT_EQ(nullptr,
            crsMercBigK->derivingConversion()
                ->convertToOtherMethod(EPSG_CODE_METHOD_MERCATOR_VARIANT_B)
                .get());

  auto mercAUnitK = op::Conversion::createMercatorVariantA(
      NameProps("Merc"), Deg(0.0), Deg(5.0), Scale(1.0), Metre(100.0),
      Metre(200.0));
  auto crsMercUnitK = MakeWGS84ProjectedCRS("M4", mercAUnitK);
  auto mercBFromUnit = crsMercUnitK->derivingConversion()->convertToOtherMethod(
      EPSG_CODE_METHOD_MERCATOR_VARIANT_B);
  ASSERT_THAT(mercBFromUnit, NotNull());
  EXPECT_DOUBLE_EQ(0.0, mercBFromUnit->parameterValueNumeric(
                            EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL,
                            common::UnitOfMeasure::DEGREE));

  auto mercASecant = op::Conversion::createMercatorVariantA(
      NameProps("Merc"), Deg(0.0), Deg(5.0), Scale(0.8), Metre(100.0),
      Metre(200.0));
  auto crsMercSecant = MakeWGS84ProjectedCRS("M5", mercASecant);
  auto mercBFromSecant =
      crsMercSecant->derivingConversion()->convertToOtherMethod(
          EPSG_CODE_METHOD_MERCATOR_VARIANT_B);
  ASSERT_THAT(mercBFromSecant, NotNull());
  EXPECT_GT(mercBFromSecant->parameterValueNumeric(
                EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL,
                common::UnitOfMeasure::DEGREE),
            30.0);

  // 5. Mercator Variant B -> Variant A.
  auto mercBPole = op::Conversion::createMercatorVariantB(
      NameProps("MercB"), Deg(90.0), Deg(0.0), Metre(0.0), Metre(0.0));
  auto crsMercBPole = MakeWGS84ProjectedCRS("MB1", mercBPole);
  EXPECT_EQ(nullptr,
            crsMercBPole->derivingConversion()
                ->convertToOtherMethod(EPSG_CODE_METHOD_MERCATOR_VARIANT_A)
                .get());

  auto mercB30 = op::Conversion::createMercatorVariantB(
      NameProps("MercB"), Deg(30.0), Deg(10.0), Metre(500.0), Metre(600.0));
  auto crsMercB30 = MakeWGS84ProjectedCRS("MB2", mercB30);
  auto mercAFromB = crsMercB30->derivingConversion()->convertToOtherMethod(
      EPSG_CODE_METHOD_MERCATOR_VARIANT_A);
  ASSERT_THAT(mercAFromB, NotNull());
  EXPECT_NEAR(std::cos(30.0 * std::numbers::pi / 180.0),
              mercAFromB->parameterValueNumericAsSI(
                  EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN),
              5e-3);

  // 6. LCC 1SP -> 2SP edge and valid cases.
  auto lcc1Pole = op::Conversion::createLambertConicConformal_1SP(
      NameProps("L1"), Deg(90.0), Deg(0.0), Scale(1.0), Metre(0.0), Metre(0.0));
  auto crsL1Pole = MakeWGS84ProjectedCRS("L1Pole", lcc1Pole);
  EXPECT_EQ(nullptr, crsL1Pole->derivingConversion()
                         ->convertToOtherMethod(
                             EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP)
                         .get());

  auto lcc1BadK = op::Conversion::createLambertConicConformal_1SP(
      NameProps("L1"), Deg(45.0), Deg(0.0), Scale(0.0), Metre(0.0), Metre(0.0));
  auto crsL1BadK = MakeWGS84ProjectedCRS("L1BadK", lcc1BadK);
  EXPECT_EQ(nullptr, crsL1BadK->derivingConversion()
                         ->convertToOtherMethod(
                             EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP)
                         .get());

  auto lcc1BigK = op::Conversion::createLambertConicConformal_1SP(
      NameProps("L1BigK"), Deg(45.0), Deg(0.0), Scale(1.5), Metre(0.0),
      Metre(0.0));
  auto crsL1BigK = MakeWGS84ProjectedCRS("L1BigK", lcc1BigK);
  EXPECT_EQ(nullptr, crsL1BigK->derivingConversion()
                         ->convertToOtherMethod(
                             EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP)
                         .get());

  auto lcc1Eq = op::Conversion::createLambertConicConformal_1SP(
      NameProps("L1"), Deg(0.0), Deg(0.0), Scale(1.0), Metre(0.0), Metre(0.0));
  auto crsL1Eq = MakeWGS84ProjectedCRS("L1Eq", lcc1Eq);
  EXPECT_EQ(nullptr, crsL1Eq->derivingConversion()
                         ->convertToOtherMethod(
                             EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP)
                         .get());

  auto lcc1Tangent = op::Conversion::createLambertConicConformal_1SP(
      NameProps("L1Tan"), Deg(45.0), Deg(3.0), Scale(1.0), Metre(1000.0),
      Metre(2000.0));
  auto crsL1Tan = MakeWGS84ProjectedCRS("L1TanCRS", lcc1Tangent);
  auto lcc2FromTan = crsL1Tan->derivingConversion()->convertToOtherMethod(
      EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP);
  ASSERT_THAT(lcc2FromTan, NotNull());
  EXPECT_DOUBLE_EQ(45.0, lcc2FromTan->parameterValueNumeric(
                             EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL,
                             common::UnitOfMeasure::DEGREE));

  // Secant LCC 1SP outside .0/.5 rounding threshold.
  auto lcc1SecNoRound = op::Conversion::createLambertConicConformal_1SP(
      NameProps("L1SecNoRound"), Deg(45.3), Deg(3.0), Scale(0.999),
      Metre(500.0), Metre(600.0));
  auto crsL1SecNoRound = MakeWGS84ProjectedCRS("L1Sec1", lcc1SecNoRound);
  EXPECT_THAT(crsL1SecNoRound->derivingConversion()->convertToOtherMethod(
                  EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP),
              NotNull());

  // Secant LCC 1SP near .5 with non-integer FN fallback.
  auto lcc1SecNonIntFN = op::Conversion::createLambertConicConformal_1SP(
      NameProps("L1SecNonIntFN"), Deg(46.5), Deg(3.0), Scale(0.999),
      Metre(500.0), Metre(100.25));
  auto crsL1SecNonIntFN = MakeWGS84ProjectedCRS("L1Sec2", lcc1SecNonIntFN);
  auto lcc2Fallback =
      crsL1SecNonIntFN->derivingConversion()->convertToOtherMethod(
          EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP);
  ASSERT_THAT(lcc2Fallback, NotNull());
  EXPECT_DOUBLE_EQ(100.25, lcc2Fallback->parameterValueNumericAsSI(
                               EPSG_CODE_PARAMETER_NORTHING_FALSE_ORIGIN));

  // Southern hemisphere LCC 1SP -> 2SP exercises !bNorth dichotomy search.
  auto lcc1South = op::Conversion::createLambertConicConformal_1SP(
      NameProps("L1South"), Deg(-35.0), Deg(145.0), Scale(0.999), Metre(0.0),
      Metre(0.0));
  auto crsL1South = MakeWGS84ProjectedCRS("L1SouthCRS", lcc1South);
  auto lcc2South = crsL1South->derivingConversion()->convertToOtherMethod(
      EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP);
  ASSERT_THAT(lcc2South, NotNull());
  EXPECT_LT(lcc2South->parameterValueNumeric(
                EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL,
                common::UnitOfMeasure::DEGREE),
            -20.0);

  // 7. LCC 2SP -> 1SP error cases & round-trip (Lambert-93 exact FN recovery).
  auto lcc2BadPhiF = op::Conversion::createLambertConicConformal_2SP(
      NameProps("L2"), Deg(90.0), Deg(0.0), Deg(44.0), Deg(49.0), Metre(0.0),
      Metre(0.0));
  EXPECT_EQ(nullptr, MakeWGS84ProjectedCRS("C1", lcc2BadPhiF)
                         ->derivingConversion()
                         ->convertToOtherMethod(
                             EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_1SP)
                         .get());

  auto lcc2BadPhi1 = op::Conversion::createLambertConicConformal_2SP(
      NameProps("L2"), Deg(45.0), Deg(0.0), Deg(90.0), Deg(49.0), Metre(0.0),
      Metre(0.0));
  EXPECT_EQ(nullptr, MakeWGS84ProjectedCRS("C2", lcc2BadPhi1)
                         ->derivingConversion()
                         ->convertToOtherMethod(
                             EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_1SP)
                         .get());

  auto lcc2BadPhi2 = op::Conversion::createLambertConicConformal_2SP(
      NameProps("L2"), Deg(45.0), Deg(0.0), Deg(44.0), Deg(90.0), Metre(0.0),
      Metre(0.0));
  EXPECT_EQ(nullptr, MakeWGS84ProjectedCRS("C3", lcc2BadPhi2)
                         ->derivingConversion()
                         ->convertToOtherMethod(
                             EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_1SP)
                         .get());

  auto lcc2Sym = op::Conversion::createLambertConicConformal_2SP(
      NameProps("L2"), Deg(0.0), Deg(0.0), Deg(-30.0), Deg(30.0), Metre(0.0),
      Metre(0.0));
  EXPECT_EQ(nullptr, MakeWGS84ProjectedCRS("C4", lcc2Sym)
                         ->derivingConversion()
                         ->convertToOtherMethod(
                             EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_1SP)
                         .get());

  auto lcc2SameP = op::Conversion::createLambertConicConformal_2SP(
      NameProps("L2Same"), Deg(45.0), Deg(3.0), Deg(45.0), Deg(45.0),
      Metre(0.0), Metre(0.0));
  auto crsL2Same = MakeWGS84ProjectedCRS("L2SameCRS", lcc2SameP);
  auto lcc1FromSame = crsL2Same->derivingConversion()->convertToOtherMethod(
      EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_1SP);
  ASSERT_THAT(lcc1FromSame, NotNull());
  EXPECT_NEAR(1.0,
              lcc1FromSame->parameterValueNumericAsSI(
                  EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN),
              1e-10);

  auto l93 = op::Conversion::createLambertConicConformal_2SP(
      NameProps("L93"), Deg(46.5), Deg(3.0), Deg(44.0), Deg(49.0),
      Metre(700000.0), Metre(6600000.0));
  auto crsL93 = MakeWGS84ProjectedCRS("L93", l93);
  auto l93_1sp = crsL93->derivingConversion()->convertToOtherMethod(
      EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_1SP);
  ASSERT_THAT(l93_1sp, NotNull());
  auto l93_roundtrip = l93_1sp->convertToOtherMethod(
      EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP);
  ASSERT_THAT(l93_roundtrip, NotNull());
  EXPECT_DOUBLE_EQ(46.5, l93_roundtrip->parameterValueNumeric(
                             EPSG_CODE_PARAMETER_LATITUDE_FALSE_ORIGIN,
                             common::UnitOfMeasure::DEGREE));
  EXPECT_NEAR(6600000.0,
              l93_roundtrip->parameterValueNumericAsSI(
                  EPSG_CODE_PARAMETER_NORTHING_FALSE_ORIGIN),
              1e-6);
}

TEST(ConversionTest, ESRIMethodNamesAndWKT1ESRIExport) {
  // 1. EquidistantCylindrical -> Plate_Carree vs Equidistant_Cylindrical.
  auto eqcZero = op::Conversion::createEquidistantCylindrical(
      NameProps("EQC"), Deg(0.0), Deg(0.0), Metre(0.0), Metre(0.0));
  auto crsPlate = MakeWGS84ProjectedCRS("Plate Carree", eqcZero);
  EXPECT_STREQ("Plate_Carree",
               crsPlate->derivingConversion()->getESRIMethodName());

  auto crsOther = MakeWGS84ProjectedCRS("World Equidistant", eqcZero);
  EXPECT_STREQ("Equidistant_Cylindrical",
               crsOther->derivingConversion()->getESRIMethodName());

  // Non-zero lat_0 forces Equidistant_Cylindrical even on Plate Carree CRS.
  auto eqcNonZero = MakeManualConversion(
      NameProps("EQC NonZero"),
      op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_EQUIDISTANT_CYLINDRICAL),
      {op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)},
      {op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Deg(15.0)),
       op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Metre(0.0))});
  auto crsPlateNonZero = MakeWGS84ProjectedCRS("Plate Carree", eqcNonZero);
  EXPECT_STREQ("Equidistant_Cylindrical",
               crsPlateNonZero->derivingConversion()->getESRIMethodName());

  // 2. TransverseMercator -> Gauss_Kruger vs Transverse_Mercator.
  auto tmGK = op::Conversion::createTransverseMercator(
      NameProps("Gauss Kruger zone 3"), Deg(0.0), Deg(9.0), Scale(1.0),
      Metre(3500000.0), Metre(0.0));
  EXPECT_STREQ("Gauss_Kruger", tmGK->getESRIMethodName());

  auto tmNormal = op::Conversion::createTransverseMercator(
      NameProps("TM"), Deg(0.0), Deg(9.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  auto crsGaussTgt = MakeWGS84ProjectedCRS("Pulkovo Gauss zone 3", tmNormal);
  EXPECT_STREQ("Gauss_Kruger",
               crsGaussTgt->derivingConversion()->getESRIMethodName());
  auto crsGKTgt = MakeWGS84ProjectedCRS("GK_Zone_3", tmNormal);
  EXPECT_STREQ("Gauss_Kruger",
               crsGKTgt->derivingConversion()->getESRIMethodName());
  auto crsTMTgt = MakeWGS84ProjectedCRS("UTM zone 32N", tmNormal);
  EXPECT_STREQ("Transverse_Mercator",
               crsTMTgt->derivingConversion()->getESRIMethodName());

  // 3. Hotine Oblique Mercator Variant A & Variant B.
  auto homASame = op::Conversion::createHotineObliqueMercatorVariantA(
      NameProps("HOMA"), Deg(4.0), Deg(115.0), Deg(53.0), Deg(53.0),
      Scale(0.9998), Metre(0.0), Metre(0.0));
  EXPECT_STREQ("Hotine_Oblique_Mercator_Azimuth_Natural_Origin",
               homASame->getESRIMethodName());
  auto homADiff = op::Conversion::createHotineObliqueMercatorVariantA(
      NameProps("HOMA"), Deg(4.0), Deg(115.0), Deg(53.5), Deg(53.0),
      Scale(0.9998), Metre(0.0), Metre(0.0));
  EXPECT_STREQ("Rectified_Skew_Orthomorphic_Natural_Origin",
               homADiff->getESRIMethodName());

  auto homBSame = op::Conversion::createHotineObliqueMercatorVariantB(
      NameProps("HOMB"), Deg(4.0), Deg(115.0), Deg(53.0), Deg(53.0),
      Scale(0.9998), Metre(0.0), Metre(0.0));
  EXPECT_STREQ("Hotine_Oblique_Mercator_Azimuth_Center",
               homBSame->getESRIMethodName());
  auto homBDiff = op::Conversion::createHotineObliqueMercatorVariantB(
      NameProps("HOMB"), Deg(4.0), Deg(115.0), Deg(53.5), Deg(53.0),
      Scale(0.9998), Metre(0.0), Metre(0.0));
  EXPECT_STREQ("Rectified_Skew_Orthomorphic_Center",
               homBDiff->getESRIMethodName());

  // 4. Polar Stereographic Variant A & Variant B.
  auto psA = op::Conversion::createPolarStereographicVariantA(
      NameProps("PSA"), Deg(90.0), Deg(0.0), Scale(0.994), Metre(2000000.0),
      Metre(2000000.0));
  EXPECT_STREQ("Stereographic", psA->getESRIMethodName());
  auto crsUpsN = MakeWGS84ProjectedCRS("WGS 84 / UPS North (E,N)", psA);
  EXPECT_STREQ("Polar_Stereographic_Variant_A",
               crsUpsN->derivingConversion()->getESRIMethodName());
  auto crsUpsS = MakeWGS84ProjectedCRS("WGS 84 / UPS South (E,N)", psA);
  EXPECT_STREQ("Polar_Stereographic_Variant_A",
               crsUpsS->derivingConversion()->getESRIMethodName());

  auto psBN = op::Conversion::createPolarStereographicVariantB(
      NameProps("PSBN"), Deg(71.0), Deg(0.0), Metre(0.0), Metre(0.0));
  EXPECT_STREQ("Stereographic_North_Pole", psBN->getESRIMethodName());
  auto psBS = op::Conversion::createPolarStereographicVariantB(
      NameProps("PSBS"), Deg(-71.0), Deg(0.0), Metre(0.0), Metre(0.0));
  EXPECT_STREQ("Stereographic_South_Pole", psBS->getESRIMethodName());

  // 5. Lambert Cylindrical Equal Area -> Behrmann vs Cylindrical_Equal_Area.
  auto cea30 = op::Conversion::createLambertCylindricalEqualArea(
      NameProps("CEA"), Deg(30.0), Deg(0.0), Metre(0.0), Metre(0.0));
  EXPECT_STREQ("Behrmann", cea30->getESRIMethodName());
  auto cea0 = op::Conversion::createLambertCylindricalEqualArea(
      NameProps("CEA"), Deg(0.0), Deg(0.0), Metre(0.0), Metre(0.0));
  EXPECT_STREQ("Cylindrical_Equal_Area", cea0->getESRIMethodName());

  // 6. Unmapped ESRI method -> nullptr.
  auto gs = op::Conversion::createGaussSchreiberTransverseMercator(
      NameProps("GS"), Deg(-21.0), Deg(55.0), Scale(1.0), Metre(0.0),
      Metre(0.0));
  EXPECT_EQ(nullptr, gs->getESRIMethodName());

  // 7. WKT1_ESRI formatting paths:
  // MercatorVariantA inside ProjectedCRS converts to MercatorVariantB.
  auto mercA = op::Conversion::createMercatorVariantA(
      NameProps("Merc1SP"), Deg(0.0), Deg(5.0), Scale(0.8), Metre(100.0),
      Metre(200.0));
  auto crsMercA = MakeWGS84ProjectedCRS("Merc ESRI", mercA);
  auto esriFmt1 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_ESRI);
  const std::string wktMercEsri = crsMercA->exportToWKT(esriFmt1.get());
  EXPECT_TRUE(ContainsSubstr(wktMercEsri, "Mercator"));
  EXPECT_TRUE(ContainsSubstr(wktMercEsri, "Standard_Parallel_1"));

  // Linear foot conversion, longitude wrapping (> 180 and < -180), scale value.
  auto footTMHi = op::Conversion::createTransverseMercator(
      NameProps("TM Hi"), Deg(0.0), Deg(200.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  auto crsFootTMHi = MakeWGS84ProjectedCRS(
      "TM Foot Hi", footTMHi,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::FOOT));
  auto esriFmt2 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_ESRI);
  const std::string wktFootHi = crsFootTMHi->exportToWKT(esriFmt2.get());
  EXPECT_TRUE(ContainsSubstr(wktFootHi, "-160"));

  auto footTMLo = op::Conversion::createTransverseMercator(
      NameProps("TM Lo"), Deg(0.0), Deg(-200.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  auto esriFmt3 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_ESRI);
  EXPECT_TRUE(ContainsSubstr(footTMLo->exportToWKT(esriFmt3.get()), "160"));

  // Missing parameters in TM trigger fallback 1.0 for scale and 0.0 for others.
  auto emptyTM = op::Conversion::create(
      NameProps("Empty TM"),
      op::OperationMethod::create(
          op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
          std::vector<op::GeneralOperationParameterNNPtr>{}),
      std::vector<op::GeneralParameterValueNNPtr>{});
  auto esriFmt4 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_ESRI);
  const std::string emptyTmWkt = emptyTM->exportToWKT(esriFmt4.get());
  EXPECT_TRUE(ContainsSubstr(emptyTmWkt, "PARAMETER[\"Scale_Factor\",1.0]"));
  EXPECT_TRUE(ContainsSubstr(emptyTmWkt, "PARAMETER[\"False_Easting\",0.0]"));

  // fixed_value ESRI parameters (GeostationarySatelliteSweepY & WebMercator).
  auto geoY = op::Conversion::createGeostationarySatelliteSweepY(
      NameProps("GeoY"), Deg(-75.0), Metre(35786023.0), Metre(0.0), Metre(0.0));
  auto esriFmt5 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_ESRI);
  EXPECT_TRUE(ContainsSubstr(geoY->exportToWKT(esriFmt5.get()), "Option"));

  auto webMerc = op::Conversion::createPopularVisualisationPseudoMercator(
      NameProps("WebMerc"), Deg(0.0), Deg(0.0), Metre(0.0), Metre(0.0));
  auto esriFmt6 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_ESRI);
  EXPECT_TRUE(ContainsSubstr(webMerc->exportToWKT(esriFmt6.get()),
                             "Auxiliary_Sphere_Type"));

  // MercatorSpherical on sphere with WKT1_ESRI converts method to Mercator.
  auto mercSph = op::Conversion::createMercatorSpherical(
      NameProps("MercSph"), Deg(0.0), Deg(5.0), Metre(0.0), Metre(0.0));
  auto sphCRS = MakeSphericalGeogCRS("SphereESRI");
  auto crsMercSph = MakeProjectedCRS(
      "MercSph ESRI", sphCRS, mercSph,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE));
  auto esriFmt7 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_ESRI);
  EXPECT_TRUE(
      ContainsSubstr(crsMercSph->exportToWKT(esriFmt7.get()), "Mercator"));

  // GeographicCRS in GRAD exercises non-DEGREE angular unit output in ESRI
  // WKT1.
  auto gradDatum = datum::GeodeticReferenceFrame::create(
      NameProps("GradDatum"), datum::Ellipsoid::WGS84,
      util::optional<std::string>(), datum::PrimeMeridian::GREENWICH);
  auto gradGeogCRS = crs::GeographicCRS::create(
      NameProps("GradGeog"), gradDatum,
      cs::EllipsoidalCS::createLatitudeLongitude(common::UnitOfMeasure::GRAD));
  auto tmGrad = op::Conversion::createTransverseMercator(
      NameProps("TM Grad"), Deg(0.0), Deg(9.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  auto crsTmGrad = MakeProjectedCRS(
      "TM Grad ESRI", gradGeogCRS, tmGrad,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE));
  auto esriFmt8 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_ESRI);
  EXPECT_TRUE(ContainsSubstr(crsTmGrad->exportToWKT(esriFmt8.get()),
                             "PARAMETER[\"Central_Meridian\",10.0]"));
}

TEST(ConversionTest, GDALMethodNamesWKT1WKT2AndJSONExport) {
  absl::Cleanup cleanup([] { proj_cleanup(); });
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  auto dbCtx = io::DatabaseContext::create(std::string(), {}, ctx.get());

  // 1. getWKT1GDALMethodName().
  auto webMerc = op::Conversion::createPopularVisualisationPseudoMercator(
      NameProps("WM"), Deg(0.0), Deg(0.0), Metre(0.0), Metre(0.0));
  EXPECT_STREQ("Mercator_1SP", webMerc->getWKT1GDALMethodName());

  auto tm = op::Conversion::createTransverseMercator(
      NameProps("TM"), Deg(0.0), Deg(3.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  EXPECT_STREQ("Transverse_Mercator", tm->getWKT1GDALMethodName());

  auto vertOff = op::Conversion::createVerticalOffset(NameProps("V"), Metre(1));
  EXPECT_EQ(nullptr, vertOff->getWKT1GDALMethodName());

  // 2. WKT1_GDAL MercatorSpherical on sphere vs ellipsoid.
  auto mercSph = op::Conversion::createMercatorSpherical(
      NameProps("MercSph"), Deg(0.0), Deg(5.0), Metre(100.0), Metre(200.0));
  auto sphCRS = MakeSphericalGeogCRS("SphereGeog");
  auto projSphMerc = MakeProjectedCRS(
      "MercSphCRS", sphCRS, mercSph,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE));
  auto gdalFmt1 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_TRUE(ContainsSubstr(projSphMerc->exportToWKT(gdalFmt1.get()),
                             "PROJECTION[\"Mercator_1SP\"]"));

  // Non-zero lat_0 on MercatorSpherical with sphere throws FormattingException.
  auto mercSphLat = op::Conversion::createMercatorSpherical(
      NameProps("MercSphLat"), Deg(10.0), Deg(5.0), Metre(0.0), Metre(0.0));
  auto projSphMercLat = MakeProjectedCRS(
      "MercSphLatCRS", sphCRS, mercSphLat,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE));
  auto gdalFmtErr1 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THROW(projSphMercLat->exportToWKT(gdalFmtErr1.get()),
               io::FormattingException);

  // On ellipsoid base CRS, WKT1_GDAL rejects MercatorSpherical.
  auto projEllpsMerc = MakeWGS84ProjectedCRS("MercEllpsCRS", mercSph);
  auto gdalFmt2 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THROW(projEllpsMerc->exportToWKT(gdalFmt2.get()),
               io::FormattingException);

  // 3. WKT1_GDAL PseudoMercator valid & non-zero lat_0 throw.
  auto gdalFmt3 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_TRUE(
      ContainsSubstr(webMerc->exportToWKT(gdalFmt3.get()), "Mercator_1SP"));

  auto webMercBad = op::Conversion::createPopularVisualisationPseudoMercator(
      NameProps("WM Bad"), Deg(5.0), Deg(0.0), Metre(0.0), Metre(0.0));
  auto gdalFmtErr2 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THROW(webMercBad->exportToWKT(gdalFmtErr2.get()),
               io::FormattingException);

  // 4. WKT1_GDAL MercatorVariantA non-zero lat_0 throws.
  auto mercABad = op::Conversion::createMercatorVariantA(
      NameProps("M1"), Deg(10.0), Deg(0.0), Scale(1.0), Metre(0.0), Metre(0.0));
  auto gdalFmtErr3 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THROW(mercABad->exportToWKT(gdalFmtErr3.get()),
               io::FormattingException);

  // 5. WKT1_GDAL "PROJ ..." custom method -> "custom_proj4".
  auto customProj =
      MakeManualConversion(NameProps("Custom"), NameProps("PROJ eqc"), {}, {});
  auto gdalFmt4 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_TRUE(ContainsSubstr(customProj->exportToWKT(gdalFmt4.get()),
                             "PROJECTION[\"custom_proj4\"]"));

  // 6. EquidistantCylindrical lat_0==0 omitted vs lat_0!=0 emitted;
  // VerticalPerspective FE/FN==0 omitted vs non-zero emitted.
  auto eqcNonZero = MakeManualConversion(
      NameProps("EQC NonZero"),
      op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_EQUIDISTANT_CYLINDRICAL),
      {op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)},
      {op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Deg(15.0)),
       op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Metre(0.0))});
  auto gdalFmt5 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_TRUE(ContainsSubstr(eqcNonZero->exportToWKT(gdalFmt5.get()), "15"));

  auto eqcSph = op::Conversion::createEquidistantCylindricalSpherical(
      NameProps("EQC Sph"), Deg(30.0), Deg(5.0), Metre(0.0), Metre(0.0));
  auto gdalFmt6 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_TRUE(ContainsSubstr(eqcSph->exportToWKT(gdalFmt6.get()),
                             "standard_parallel_1"));

  auto eqcEllpsZero = op::Conversion::createEquidistantCylindrical(
      NameProps("EQC0"), Deg(0.0), Deg(5.0), Metre(0.0), Metre(0.0));
  auto gdalFmt7 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  const std::string eqcEllpsWkt = eqcEllpsZero->exportToWKT(gdalFmt7.get());
  EXPECT_TRUE(ContainsSubstr(eqcEllpsWkt, "central_meridian"));
  EXPECT_FALSE(ContainsSubstr(eqcEllpsWkt, "latitude_of_origin"));

  auto vpZero = op::Conversion::createVerticalPerspective(
      NameProps("VP0"), Deg(55.0), Deg(5.0), Metre(0.0), Metre(35785831.0),
      Metre(0.0), Metre(0.0));
  auto wktFmtVP0 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT2_2019);
  EXPECT_FALSE(
      ContainsSubstr(vpZero->exportToWKT(wktFmtVP0.get()), "False easting"));

  auto vpNonZero = op::Conversion::createVerticalPerspective(
      NameProps("VP1"), Deg(55.0), Deg(5.0), Metre(0.0), Metre(35785831.0),
      Metre(1000.0), Metre(2000.0));
  auto wktFmtVP1 =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT2_2019);
  const std::string vp1Wkt = vpNonZero->exportToWKT(wktFmtVP1.get());
  EXPECT_TRUE(ContainsSubstr(vp1Wkt, "1000"));
  EXPECT_TRUE(ContainsSubstr(vp1Wkt, "2000"));

  // 7. WKT2 & JSON ID toggle, empty name -> "unnamed", interpolation CRS.
  auto utmId = op::Conversion::createUTM(util::PropertyMap(), 31, true);
  auto wktIdOn =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT2_2019);
  EXPECT_TRUE(
      ContainsSubstr(utmId->exportToWKT(wktIdOn.get()), "ID[\"EPSG\",16031]"));
  auto wktIdOff = io::WKTFormatter::create(
      io::WKTFormatter::Convention::WKT2_2019_SIMPLIFIED);
  wktIdOff->setOutputId(false);
  EXPECT_FALSE(ContainsSubstr(utmId->exportToWKT(wktIdOff.get()), "16031"));

  auto unnamed = op::Conversion::createGeographicGeocentric(NameProps(""));
  auto jsonFmtUnnamed = io::JSONFormatter::create();
  EXPECT_TRUE(ContainsSubstr(unnamed->exportToJSON(jsonFmtUnnamed.get()),
                             "\"unnamed\""));

  auto jsonFmt = io::JSONFormatter::create();
  const std::string utmJson = utmId->exportToJSON(jsonFmt.get());
  EXPECT_TRUE(ContainsSubstr(utmJson, "16031"));
  auto hdrJson = op::Conversion::createHeightDepthReversal(NameProps("HDR"));
  auto jsonFmtHdr = io::JSONFormatter::create();
  const std::string hdrJsonStr = hdrJson->exportToJSON(jsonFmtHdr.get());
  EXPECT_TRUE(ContainsSubstr(hdrJsonStr, "\"HDR\""));
  EXPECT_FALSE(ContainsSubstr(hdrJsonStr, "\"parameters\""));

  auto manualInterp = MakeManualConversion(
      op::createMapNameEPSGCode("TopoWithInterp", 15000),
      op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_GEOGRAPHIC_TOPOCENTRIC),
      {MakeParam("EPSG code for Interpolation CRS",
                 EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS),
       MakeParam("EPSG code for Horizontal CRS",
                 EPSG_CODE_PARAMETER_EPSG_CODE_FOR_HORIZONTAL_CRS)},
      {op::ParameterValue::create(4326), op::ParameterValue::create(4326)});
  auto wktInterp =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT2_2019);
  EXPECT_TRUE(
      ContainsSubstr(manualInterp->exportToWKT(wktInterp.get()), "1048"));
  auto jsonInterp = io::JSONFormatter::create();
  EXPECT_TRUE(ContainsSubstr(manualInterp->exportToJSON(jsonInterp.get()),
                             "\"Conversion\""));

  // Parse a WKT2 Conversion containing EPSG:1048 so WKTParser extracts
  // interpolationCRS() while stripping the raw 1048 parameter, then export it
  // to WKT2 & JSON to cover implicit interpolationCRS parameter emission.
  const std::string topoWkt =
      "CONVERSION[\"TopoParsed\","
      "METHOD[\"Geographic/topocentric conversions\",ID[\"EPSG\",9837]],"
      "PARAMETER[\"Latitude of topocentric origin\",55,"
      "ANGLEUNIT[\"degree\",0.0174532925199433],ID[\"EPSG\",8834]],"
      "PARAMETER[\"Longitude of topocentric origin\",5,"
      "ANGLEUNIT[\"degree\",0.0174532925199433],ID[\"EPSG\",8835]],"
      "PARAMETER[\"Ellipsoidal height of topocentric origin\",0,"
      "LENGTHUNIT[\"metre\",1],ID[\"EPSG\",8836]],"
      "PARAMETER[\"EPSG code for Interpolation CRS\",4326,ID[\"EPSG\",1048]]]";
  auto parsedObj =
      io::WKTParser().attachDatabaseContext(dbCtx).createFromWKT(topoWkt);
  auto parsedConv =
      std::dynamic_pointer_cast<op::Conversion>(parsedObj.as_nullable());
  ASSERT_THAT(parsedConv, NotNull());
  ASSERT_THAT(parsedConv->interpolationCRS(), NotNull());
  auto wktReexport =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT2_2019);
  EXPECT_TRUE(
      ContainsSubstr(parsedConv->exportToWKT(wktReexport.get()), "1048"));
  auto jsonReexport = io::JSONFormatter::create();
  EXPECT_TRUE(ContainsSubstr(parsedConv->exportToJSON(jsonReexport.get()),
                             "Interpolation CRS"));
}

TEST(ConversionTest, AddWKTExtensionNodeBranches) {
  // 1. "tmerc approx" and "utm approx" projMethodOverride_ branches.
  auto buildTMWithOverride = [](const char* overrideStr, double lonDeg,
                                bool isUtm) {
    std::vector<op::OperationParameterNNPtr> params{
        op::createOpParamNameEPSGCode(
            EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN),
        op::createOpParamNameEPSGCode(
            EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN),
        op::createOpParamNameEPSGCode(
            EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN),
        op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
        op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)};
    std::vector<op::GeneralOperationParameterNNPtr> genParams;
    for (const auto& p : params) {
      genParams.push_back(
          util::nn_static_pointer_cast<op::GeneralOperationParameter>(p));
    }
    auto methodProps =
        op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR)
            .set("proj_method", overrideStr);
    auto method = op::OperationMethod::create(methodProps, genParams);
    std::vector<op::GeneralParameterValueNNPtr> vals{
        op::OperationParameterValue::create(
            params[0], op::ParameterValue::create(Deg(0.0))),
        op::OperationParameterValue::create(
            params[1], op::ParameterValue::create(Deg(lonDeg))),
        op::OperationParameterValue::create(
            params[2], op::ParameterValue::create(Scale(0.9996))),
        op::OperationParameterValue::create(
            params[3], op::ParameterValue::create(Metre(500000.0))),
        op::OperationParameterValue::create(
            params[4], op::ParameterValue::create(Metre(0.0)))};
    return op::Conversion::create(NameProps(isUtm ? "UTM Approx" : "TM Approx"),
                                  method, vals);
  };

  auto tmApprox = buildTMWithOverride("tmerc approx", 2.0, false);
  auto crsTmApprox = MakeWGS84ProjectedCRS("TM Approx CRS", tmApprox);
  auto fmt1 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_TRUE(ContainsSubstr(crsTmApprox->exportToWKT(fmt1.get()), "+approx"));

  auto utmApprox = buildTMWithOverride("utm approx", 3.0, true);
  auto crsUtmApprox = MakeWGS84ProjectedCRS("UTM Approx CRS", utmApprox);
  auto fmt2 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  const std::string utmApproxWkt = crsUtmApprox->exportToWKT(fmt2.get());
  EXPECT_TRUE(ContainsSubstr(utmApproxWkt, "+proj=utm"));
  EXPECT_TRUE(ContainsSubstr(utmApproxWkt, "+approx"));

  // 2. Popular Visualisation Pseudo Mercator + Popular Visualisation Mercator.
  auto webMerc = op::Conversion::createPopularVisualisationPseudoMercator(
      NameProps("WM"), Deg(0.0), Deg(0.0), Metre(0.0), Metre(0.0));
  auto crsWmFootOver = MakeWGS84ProjectedCRS(
      "WM Foot Over", webMerc,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::FOOT),
      /*hasOver=*/true);
  auto fmt3 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  const std::string wktWm = crsWmFootOver->exportToWKT(fmt3.get());
  EXPECT_TRUE(ContainsSubstr(wktWm, "+units=ft"));
  EXPECT_TRUE(ContainsSubstr(wktWm, "+over"));
  EXPECT_TRUE(ContainsSubstr(wktWm, "@null"));

  // Custom unmapped linear unit fails createPROJ4WebMercator cleanly.
  common::UnitOfMeasure unmappedUnit("unmapped_linear", 0.42,
                                     common::UnitOfMeasure::Type::LINEAR);
  auto crsWmUnmappedUnit = MakeWGS84ProjectedCRS(
      "WM Unmapped Unit", webMerc,
      cs::CartesianCS::createEastingNorthing(unmappedUnit));
  auto fmt4 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_FALSE(
      ContainsSubstr(crsWmUnmappedUnit->exportToWKT(fmt4.get()), "EXTENSION"));

  // Non-geographic GeodeticCRS base returns false in createPROJ4WebMercator.
  auto namedWebMerc =
      MakeManualConversion(NameProps("Popular Visualisation Mercator"),
                           NameProps("CustomWM"), {}, {});
  auto crsNonGeogBase = MakeProjectedCRS(
      "NonGeog WM", crs::GeodeticCRS::EPSG_4978, namedWebMerc,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE));
  auto fmt5 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_FALSE(
      ContainsSubstr(crsNonGeogBase->exportToWKT(fmt5.get()), "EXTENSION"));

  // 3. "PROJ ..." custom method extension with LINEAR/ANGULAR/SCALE/non-measure
  // parameters, both on GeographicCRS and non-GeographicCRS base.
  auto customProj = MakeManualConversion(
      NameProps("CustomPROJ"), NameProps("PROJ eqc lat_ts=0 over"),
      {MakeParam("x_0"), MakeParam("lon_0"), MakeParam("k_0"),
       MakeParam("ignored_int")},
      {op::ParameterValue::create(Metre(100.0)),
       op::ParameterValue::create(Deg(5.0)),
       op::ParameterValue::create(Scale(0.9996)),
       op::ParameterValue::create(42)});
  auto crsCustomProj = MakeWGS84ProjectedCRS("Custom PROJ CRS", customProj);
  auto fmt6 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  const std::string wktCustomProj = crsCustomProj->exportToWKT(fmt6.get());
  EXPECT_TRUE(ContainsSubstr(wktCustomProj, "EXTENSION[\"PROJ4\""));
  EXPECT_TRUE(ContainsSubstr(wktCustomProj, "+proj=eqc"));
  EXPECT_TRUE(ContainsSubstr(wktCustomProj, "+lat_ts=0"));
  EXPECT_TRUE(ContainsSubstr(wktCustomProj, "+over"));
  EXPECT_TRUE(ContainsSubstr(wktCustomProj, "+x_0=100"));
  EXPECT_TRUE(ContainsSubstr(wktCustomProj, "+lon_0=5"));
  EXPECT_TRUE(ContainsSubstr(wktCustomProj, "+k_0=0.9996"));

  auto crsCustomNonGeog = MakeProjectedCRS(
      "Custom PROJ NonGeog", crs::GeodeticCRS::EPSG_4978, customProj,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE));
  auto fmt7 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_FALSE(
      ContainsSubstr(crsCustomNonGeog->exportToWKT(fmt7.get()), "EXTENSION"));

  // 4. GeostationarySatelliteSweepX emits EXTENSION["PROJ4", ...].
  auto geoX = op::Conversion::createGeostationarySatelliteSweepX(
      NameProps("GeoX"), Deg(0.0), Metre(35785831.0), Metre(0.0), Metre(0.0));
  auto crsGeoX = MakeWGS84ProjectedCRS("GeoX CRS", geoX);
  auto fmt8 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_TRUE(
      ContainsSubstr(crsGeoX->exportToWKT(fmt8.get()), "EXTENSION[\"PROJ4\""));

  auto crsGeoXPlaneto = MakeProjectedCRS(
      "GeoX Mars", MakePlanetocentricCRS("MarsPlaneto"), geoX,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE));
  auto fmt8b =
      io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_TRUE(ContainsSubstr(crsGeoXPlaneto->exportToWKT(fmt8b.get()),
                             "EXTENSION[\"PROJ4\""));
  EXPECT_TRUE(
      ContainsSubstr(crsGeoXPlaneto->derivingConversion()->exportToPROJString(
                         io::PROJStringFormatter::create().get()),
                     "+proj=geoc"));

  // 5. Normal TM emits no EXTENSION node.
  auto tmNormal = op::Conversion::createTransverseMercator(
      NameProps("TM"), Deg(0.0), Deg(3.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  auto crsTM = MakeWGS84ProjectedCRS("TM CRS", tmNormal);
  auto fmt9 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_FALSE(ContainsSubstr(crsTM->exportToWKT(fmt9.get()), "EXTENSION"));
}

TEST(ConversionTest, ExportToPROJStringBranches) {
  absl::Cleanup cleanup([] { proj_cleanup(); });

  auto makeProj5 = [] {
    return io::PROJStringFormatter::create(
        io::PROJStringFormatter::Convention::PROJ_5);
  };
  auto makeProj4CRS = [] {
    auto f = io::PROJStringFormatter::create(
        io::PROJStringFormatter::Convention::PROJ_4);
    f->setCRSExport(true);
    return f;
  };

  // 1. Geocentric/topocentric (9836) & Geographic/topocentric (9837).
  auto geocTopo = MakeManualConversion(
      NameProps("GeocTopo"),
      op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_GEOCENTRIC_TOPOCENTRIC),
      {}, {});
  EXPECT_THROW(geocTopo->exportToPROJString(makeProj4CRS().get()),
               io::FormattingException);

  auto geogTopo = MakeManualConversion(
      NameProps("GeogTopo"),
      op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_GEOGRAPHIC_TOPOCENTRIC),
      {MakeParam(EPSG_NAME_PARAMETER_LATITUDE_TOPOGRAPHIC_ORIGIN,
                 EPSG_CODE_PARAMETER_LATITUDE_TOPOGRAPHIC_ORIGIN),
       MakeParam(EPSG_NAME_PARAMETER_LONGITUDE_TOPOGRAPHIC_ORIGIN,
                 EPSG_CODE_PARAMETER_LONGITUDE_TOPOGRAPHIC_ORIGIN),
       MakeParam(EPSG_NAME_PARAMETER_ELLIPSOIDAL_HEIGHT_TOPOCENTRIC_ORIGIN,
                 EPSG_CODE_PARAMETER_ELLIPSOIDAL_HEIGHT_TOPOCENTRIC_ORIGIN)},
      {op::ParameterValue::create(Deg(55.0)),
       op::ParameterValue::create(Deg(5.0)),
       op::ParameterValue::create(Metre(10.0))});
  EXPECT_THROW(geogTopo->exportToPROJString(makeProj4CRS().get()),
               io::FormattingException);
  EXPECT_THROW(geogTopo->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  auto derivedTopo = crs::DerivedGeodeticCRS::create(
      NameProps("TopocentricCRS"), crs::GeographicCRS::EPSG_4979, geogTopo,
      cs::CartesianCS::createGeocentric(common::UnitOfMeasure::METRE));
  const std::string topoStr =
      derivedTopo->derivingConversion()->exportToPROJString(makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(topoStr, "+proj=cart"));
  EXPECT_TRUE(ContainsSubstr(topoStr, "+proj=topocentric"));

  // 2. Geographic latitude / Geocentric latitude conversion branches.
  auto mars = MakePlanetocentricCRS("MarsPlanetocentric");
  auto geogToMars = op::Conversion::createGeographicGeocentricLatitude(
      crs::GeographicCRS::EPSG_4326, mars);
  EXPECT_TRUE(ContainsSubstr(geogToMars->exportToPROJString(makeProj5().get()),
                             "+proj=geoc"));
  EXPECT_TRUE(ContainsSubstr(
      geogToMars->inverse()->exportToPROJString(makeProj5().get()),
      "+proj=geoc"));

  auto badLatConv = op::Conversion::createGeographicGeocentricLatitude(
      crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4979);
  EXPECT_THROW(badLatConv->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  // Wrap planetocentric CRS inside BoundCRS and CompoundCRS(Direct & Bound).
  auto marsBound = crs::BoundCRS::createFromTOWGS84(
      mars, {1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 0.0});
  auto geogToMarsBound = op::Conversion::createGeographicGeocentricLatitude(
      crs::GeographicCRS::EPSG_4326, marsBound);
  EXPECT_TRUE(ContainsSubstr(
      geogToMarsBound->exportToPROJString(makeProj5().get()), "+proj=geoc"));
  EXPECT_TRUE(ContainsSubstr(
      geogToMarsBound->inverse()->exportToPROJString(makeProj5().get()),
      "+proj=geoc"));

  auto tempCRS = MakeTemporalCRS("EpochTime");
  auto marsCompDirect = crs::CompoundCRS::create(
      NameProps("CompDirect"),
      std::vector<crs::CRSNNPtr>{
          util::nn_static_pointer_cast<crs::CRS>(mars),
          util::nn_static_pointer_cast<crs::CRS>(tempCRS)});
  auto geogToCompDirect = op::Conversion::createGeographicGeocentricLatitude(
      crs::GeographicCRS::EPSG_4326, marsCompDirect);
  EXPECT_TRUE(ContainsSubstr(
      geogToCompDirect->exportToPROJString(makeProj5().get()), "+proj=geoc"));
  EXPECT_TRUE(ContainsSubstr(
      geogToCompDirect->inverse()->exportToPROJString(makeProj5().get()),
      "+proj=geoc"));

  auto marsCompBound = crs::CompoundCRS::create(
      NameProps("CompBound"),
      std::vector<crs::CRSNNPtr>{
          util::nn_static_pointer_cast<crs::CRS>(marsBound),
          util::nn_static_pointer_cast<crs::CRS>(tempCRS)});
  auto geogToCompBound = op::Conversion::createGeographicGeocentricLatitude(
      crs::GeographicCRS::EPSG_4326, marsCompBound);
  EXPECT_TRUE(ContainsSubstr(
      geogToCompBound->exportToPROJString(makeProj5().get()), "+proj=geoc"));
  EXPECT_TRUE(ContainsSubstr(
      geogToCompBound->inverse()->exportToPROJString(makeProj5().get()),
      "+proj=geoc"));

  // 3. Source/target CompoundCRS, DerivedProjectedCRS, DerivedGeographicCRS,
  // and OVER=true.
  auto vertM = MakeVerticalCRS("VertM", common::UnitOfMeasure::METRE);
  auto tm31 = op::Conversion::createTransverseMercator(
      NameProps("TM"), Deg(0.0), Deg(4.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  auto crsTmFootOver = MakeWGS84ProjectedCRS(
      "TM Foot Over", tm31,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::FOOT),
      /*hasOver=*/true);
  EXPECT_TRUE(
      ContainsSubstr(crsTmFootOver->derivingConversion()->exportToPROJString(
                         makeProj5().get()),
                     "+over"));
  const std::string tmCrsProj4 =
      crsTmFootOver->derivingConversion()->exportToPROJString(
          makeProj4CRS().get());
  EXPECT_TRUE(ContainsSubstr(tmCrsProj4, "+proj=tmerc"));
  EXPECT_TRUE(ContainsSubstr(tmCrsProj4, "+over"));

  auto derivedProj = crs::DerivedProjectedCRS::create(
      NameProps("DerivedProj"), crsTmFootOver,
      op::Conversion::createAxisOrderReversal(false),
      util::nn_static_pointer_cast<cs::CoordinateSystem>(
          cs::CartesianCS::createNorthingEasting(
              common::UnitOfMeasure::METRE)));
  EXPECT_TRUE(ContainsSubstr(
      derivedProj->derivingConversion()->exportToPROJString(makeProj5().get()),
      "+proj=axisswap"));

  auto derivedProjCustom = crs::DerivedProjectedCRS::create(
      NameProps("DerivedProjCustom"), crsTmFootOver,
      MakeManualConversion(NameProps("CustomAffine"),
                           NameProps("PROJ affine xoff=10"), {}, {}),
      util::nn_static_pointer_cast<cs::CoordinateSystem>(
          cs::CartesianCS::createNorthingEasting(
              common::UnitOfMeasure::METRE)));
  const std::string derivedCustomStr =
      derivedProjCustom->derivingConversion()->exportToPROJString(
          makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(derivedCustomStr, "+proj=affine"));
  EXPECT_TRUE(ContainsSubstr(derivedCustomStr, "+proj=axisswap"));

  auto derivedGeog = crs::DerivedGeographicCRS::create(
      NameProps("DerivedGeog"), crs::GeographicCRS::EPSG_4326,
      op::Conversion::createPoleRotationGRIBConvention(
          NameProps("PoleRot"), Deg(-35.0), Deg(15.0), Deg(10.0)),
      cs::EllipsoidalCS::createLongitudeLatitude(
          common::UnitOfMeasure::DEGREE));
  EXPECT_TRUE(ContainsSubstr(
      derivedGeog->derivingConversion()->exportToPROJString(makeProj5().get()),
      "+proj=ob_tran"));

  // CompoundCRS -> CompoundCRS extraction exercises CompoundCRS branches
  // on both l_sourceCRS and l_targetCRS of a Conversion.
  auto srcCompWGS84 = crs::CompoundCRS::create(
      NameProps("WGS84+Vert"),
      std::vector<crs::CRSNNPtr>{
          util::nn_static_pointer_cast<crs::CRS>(crs::GeographicCRS::EPSG_4326),
          util::nn_static_pointer_cast<crs::CRS>(vertM)});
  auto dstCompTM = crs::CompoundCRS::create(
      NameProps("TM+Vert"),
      std::vector<crs::CRSNNPtr>{
          util::nn_static_pointer_cast<crs::CRS>(crsTmFootOver),
          util::nn_static_pointer_cast<crs::CRS>(vertM)});
  auto compConvTM = crsTmFootOver->derivingConversion();
  CoordOpPeer::SetCRSs(compConvTM.get(), srcCompWGS84, dstCompTM, nullptr);
  const std::string compTmStr =
      compConvTM->exportToPROJString(makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(compTmStr, "+proj=tmerc"));
  EXPECT_TRUE(ContainsSubstr(compTmStr, "+over"));

  // 4. UTM north/south + approx, and ESRI:102470 negative scale factor.
  auto utmS = op::Conversion::createUTM(util::PropertyMap(), 19, false);
  auto fmtApprox = makeProj5();
  fmtApprox->setUseApproxTMerc(true);
  const std::string utmSStr = utmS->exportToPROJString(fmtApprox.get());
  EXPECT_TRUE(ContainsSubstr(utmSStr, "+proj=utm"));
  EXPECT_TRUE(ContainsSubstr(utmSStr, "+approx"));
  EXPECT_TRUE(ContainsSubstr(utmSStr, "+south"));

  auto esriNegK = op::Conversion::createTransverseMercator(
      NameProps("ESRI:102470"), Deg(0.0), Deg(9.0), Scale(-1.0), Metre(0.0),
      Metre(0.0));
  auto crsNegK = MakeWGS84ProjectedCRS("NegK CRS", esriNegK);
  const std::string negKStr =
      crsNegK->derivingConversion()->exportToPROJString(makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(negKStr, "+axis=wsu"));
  EXPECT_TRUE(ContainsSubstr(negKStr, "+k=1"));

  // Missing TM scale defaults to 1.0.
  auto emptyTM = op::Conversion::create(
      NameProps("Empty TM"),
      op::OperationMethod::create(
          op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
          std::vector<op::GeneralOperationParameterNNPtr>{}),
      std::vector<op::GeneralParameterValueNNPtr>{});
  EXPECT_TRUE(
      ContainsSubstr(emptyTM->exportToPROJString(makeProj5().get()), "+k=1"));

  // 5. HOM A & HOM B: somerc (azi=90, rect=90) vs omerc vs missing gamma.
  auto homASomerc = op::Conversion::createHotineObliqueMercatorVariantA(
      NameProps("SomercA"), Deg(46.95), Deg(7.44), Deg(90.0), Deg(90.0),
      Scale(1.0), Metre(600000.0), Metre(200000.0));
  EXPECT_TRUE(ContainsSubstr(homASomerc->exportToPROJString(makeProj5().get()),
                             "+proj=somerc"));

  auto homBSomerc = op::Conversion::createHotineObliqueMercatorVariantB(
      NameProps("SomercB"), Deg(46.95), Deg(7.44), Deg(90.0), Deg(90.0),
      Scale(1.0), Metre(600000.0), Metre(200000.0));
  EXPECT_TRUE(ContainsSubstr(homBSomerc->exportToPROJString(makeProj5().get()),
                             "+proj=somerc"));

  auto emptyHOM = op::Conversion::create(
      NameProps("Empty HOMA"),
      op::OperationMethod::create(
          op::createMethodMapNameEPSGCode(
              EPSG_CODE_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_A),
          std::vector<op::GeneralOperationParameterNNPtr>{}),
      std::vector<op::GeneralParameterValueNNPtr>{});
  const std::string emptyHomStr =
      emptyHOM->exportToPROJString(makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(emptyHomStr, "+proj=omerc"));
  EXPECT_FALSE(ContainsSubstr(emptyHomStr, "+gamma="));

  auto emptyHOMB = op::Conversion::create(
      NameProps("Empty HOMB"),
      op::OperationMethod::create(
          op::createMethodMapNameEPSGCode(
              EPSG_CODE_METHOD_HOTINE_OBLIQUE_MERCATOR_VARIANT_B),
          std::vector<op::GeneralOperationParameterNNPtr>{}),
      std::vector<op::GeneralParameterValueNNPtr>{});
  const std::string emptyHomBStr =
      emptyHOMB->exportToPROJString(makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(emptyHomBStr, "+proj=omerc"));
  EXPECT_FALSE(ContainsSubstr(emptyHomBStr, "+gamma="));

  // 6. Krovak / KrovakNorthOriented / KrovakModified validation & +czech.
  auto krovakNO = op::Conversion::createKrovakNorthOriented(
      NameProps("KNO"), Deg(49.5), Deg(24.83333333333333),
      Deg(30.288139722222223), Deg(78.5), Scale(0.9999), Metre(0.0),
      Metre(0.0));
  EXPECT_TRUE(ContainsSubstr(krovakNO->exportToPROJString(makeProj5().get()),
                             "+proj=krovak"));

  auto krovakBadColat = op::Conversion::createKrovakNorthOriented(
      NameProps("KNOBadColat"), Deg(49.5), Deg(24.83333333333333), Deg(31.0),
      Deg(78.5), Scale(0.9999), Metre(0.0), Metre(0.0));
  EXPECT_THROW(krovakBadColat->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  auto krovakBadPsp = op::Conversion::createKrovakNorthOriented(
      NameProps("KNOBadPsp"), Deg(49.5), Deg(24.83333333333333),
      Deg(30.288139722222223), Deg(79.0), Scale(0.9999), Metre(0.0),
      Metre(0.0));
  EXPECT_THROW(krovakBadPsp->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  auto krovakStd = op::Conversion::createKrovak(
      NameProps("K"), Deg(49.5), Deg(24.83333333333333),
      Deg(30.288139722222223), Deg(78.5), Scale(0.9999), Metre(0.0),
      Metre(0.0));
  auto crsKrovakWS = MakeWGS84ProjectedCRS(
      "Krovak Czech", krovakStd,
      cs::CartesianCS::createWestingSouthing(common::UnitOfMeasure::METRE));
  EXPECT_TRUE(ContainsSubstr(
      crsKrovakWS->derivingConversion()->exportToPROJString(makeProj5().get()),
      "+czech"));
  auto crsKrovakEN = MakeWGS84ProjectedCRS("Krovak EN", krovakStd);
  EXPECT_TRUE(ContainsSubstr(
      crsKrovakEN->derivingConversion()->exportToPROJString(makeProj5().get()),
      "+axis=swu"));

  auto makeKrovakModified = [&](const std::string& convName,
                                const char* methodName, int methodCode,
                                double colatDeg) {
    return MakeManualConversion(
        NameProps(convName), op::createMapNameEPSGCode(methodName, methodCode),
        {op::createOpParamNameEPSGCode(
             EPSG_CODE_PARAMETER_LATITUDE_PROJECTION_CENTRE),
         op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_LONGITUDE_OF_ORIGIN),
         op::createOpParamNameEPSGCode(
             EPSG_CODE_PARAMETER_COLATITUDE_CONE_AXIS),
         op::createOpParamNameEPSGCode(
             EPSG_CODE_PARAMETER_LATITUDE_PSEUDO_STANDARD_PARALLEL),
         op::createOpParamNameEPSGCode(
             EPSG_CODE_PARAMETER_SCALE_FACTOR_PSEUDO_STANDARD_PARALLEL),
         op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
         op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)},
        {op::ParameterValue::create(Deg(49.5)),
         op::ParameterValue::create(Deg(24.83333333333333)),
         op::ParameterValue::create(Deg(colatDeg)),
         op::ParameterValue::create(Deg(78.5)),
         op::ParameterValue::create(Scale(0.9999)),
         op::ParameterValue::create(Metre(0.0)),
         op::ParameterValue::create(Metre(0.0))});
  };
  auto krovakMod =
      makeKrovakModified("KMod", EPSG_NAME_METHOD_KROVAK_MODIFIED,
                         EPSG_CODE_METHOD_KROVAK_MODIFIED, 30.288139722222223);
  auto crsKrovakModWS = MakeWGS84ProjectedCRS(
      "KrovakMod Czech", krovakMod,
      cs::CartesianCS::createWestingSouthing(common::UnitOfMeasure::METRE));
  const std::string kModCzech =
      crsKrovakModWS->derivingConversion()->exportToPROJString(
          makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(kModCzech, "+proj=mod_krovak"));
  EXPECT_TRUE(ContainsSubstr(kModCzech, "+czech"));

  auto crsKrovakModEN = MakeWGS84ProjectedCRS("KrovakMod EN", krovakMod);
  EXPECT_TRUE(
      ContainsSubstr(crsKrovakModEN->derivingConversion()->exportToPROJString(
                         makeProj5().get()),
                     "+axis=swu"));

  auto krovakModNO = makeKrovakModified(
      "KModNO", EPSG_NAME_METHOD_KROVAK_MODIFIED_NORTH_ORIENTED,
      EPSG_CODE_METHOD_KROVAK_MODIFIED_NORTH_ORIENTED, 30.288139722222223);
  EXPECT_TRUE(ContainsSubstr(krovakModNO->exportToPROJString(makeProj5().get()),
                             "+proj=mod_krovak"));

  auto krovakModNOBadColat = makeKrovakModified(
      "KModNOBadColat", EPSG_NAME_METHOD_KROVAK_MODIFIED_NORTH_ORIENTED,
      EPSG_CODE_METHOD_KROVAK_MODIFIED_NORTH_ORIENTED, 31.0);
  EXPECT_THROW(krovakModNOBadColat->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  // 7. Mercator A/B & TMSO validation throws and success.
  auto mercABadLat = op::Conversion::createMercatorVariantA(
      NameProps("MA"), Deg(10.0), Deg(0.0), Scale(1.0), Metre(0.0), Metre(0.0));
  EXPECT_THROW(mercABadLat->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  auto mercBBadScale = MakeManualConversion(
      NameProps("MBScale"),
      op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_MERCATOR_VARIANT_B),
      {op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN),
       MakeParam(op::WKT1_SCALE_FACTOR),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)},
      {op::ParameterValue::create(Deg(30.0)),
       op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Scale(0.99)),
       op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Metre(0.0))});
  EXPECT_THROW(mercBBadScale->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  auto mercBBadLat0 = MakeManualConversion(
      NameProps("MBLat0"),
      op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_MERCATOR_VARIANT_B),
      {op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)},
      {op::ParameterValue::create(Deg(30.0)),
       op::ParameterValue::create(Deg(10.0)),
       op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Metre(0.0))});
  EXPECT_THROW(mercBBadLat0->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  auto tmsoBadE = op::Conversion::createTransverseMercatorSouthOriented(
      NameProps("TMSOBadE"), Deg(0.0), Deg(19.0), Scale(1.0), Metre(100.0),
      Metre(0.0));
  EXPECT_THROW(tmsoBadE->exportToPROJString(makeProj5().get()),
               io::FormattingException);
  auto tmsoBadN = op::Conversion::createTransverseMercatorSouthOriented(
      NameProps("TMSOBadN"), Deg(0.0), Deg(19.0), Scale(1.0), Metre(0.0),
      Metre(100.0));
  EXPECT_THROW(tmsoBadN->exportToPROJString(makeProj5().get()),
               io::FormattingException);
  auto tmsoOk = op::Conversion::createTransverseMercatorSouthOriented(
      NameProps("TMSO"), Deg(0.0), Deg(19.0), Scale(1.0), Metre(0.0),
      Metre(0.0));
  EXPECT_TRUE(ContainsSubstr(tmsoOk->exportToPROJString(makeProj5().get()),
                             "+axis=wsu"));

  // 8. WebMercator & "Popular Visualisation Mercator" & "PROJ ..." custom.
  auto webMerc = op::Conversion::createPopularVisualisationPseudoMercator(
      NameProps("WM"), Deg(0.0), Deg(0.0), Metre(0.0), Metre(0.0));
  EXPECT_THROW(webMerc->exportToPROJString(makeProj4CRS().get()),
               io::FormattingException);
  auto crsWm = MakeWGS84ProjectedCRS("WM CRS", webMerc);
  EXPECT_TRUE(ContainsSubstr(
      crsWm->derivingConversion()->exportToPROJString(makeProj4CRS().get()),
      "+nadgrids=@null"));

  auto namedWM =
      MakeManualConversion(NameProps("Popular Visualisation Mercator"),
                           NameProps("CustomWM"), {}, {});
  EXPECT_THROW(namedWM->exportToPROJString(makeProj4CRS().get()),
               io::FormattingException);
  const std::string namedWmStandalone =
      namedWM->exportToPROJString(makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(namedWmStandalone, "+proj=webmerc"));
  EXPECT_FALSE(ContainsSubstr(namedWmStandalone, "+ellps="));
  auto crsNamedWM = MakeWGS84ProjectedCRS("Named WM", namedWM);
  EXPECT_TRUE(
      ContainsSubstr(crsNamedWM->derivingConversion()->exportToPROJString(
                         makeProj4CRS().get()),
                     "+nadgrids=@null"));
  EXPECT_TRUE(ContainsSubstr(
      crsNamedWM->derivingConversion()->exportToPROJString(makeProj5().get()),
      "+proj=webmerc"));

  auto customProj = MakeManualConversion(
      NameProps("CustomPROJ"), NameProps("PROJ eqc lat_ts=0"),
      {MakeParam("lon_0")}, {op::ParameterValue::create(Deg(5.0))});
  EXPECT_TRUE(ContainsSubstr(customProj->exportToPROJString(makeProj5().get()),
                             "+proj=eqc"));

  // 9. Pole rotation (GRIB + NetCDF-CF), Adams_Square_II, Peirce_Quincuncial.
  auto gribRot = op::Conversion::createPoleRotationGRIBConvention(
      NameProps("GRIB"), Deg(-35.0), Deg(15.0), Deg(10.0));
  EXPECT_TRUE(ContainsSubstr(gribRot->exportToPROJString(makeProj5().get()),
                             "+proj=ob_tran"));

  auto netcdfRot = op::Conversion::createPoleRotationNetCDFCFConvention(
      NameProps("NetCDF"), Deg(39.25), Deg(-162.0), Deg(0.0));
  EXPECT_TRUE(ContainsSubstr(netcdfRot->exportToPROJString(makeProj5().get()),
                             "+proj=ob_tran"));

  auto adamsWithScale = MakeManualConversion(
      NameProps("AdamsScale"), NameProps("Adams_Square_II"),
      {MakeParam("False_Easting"), MakeParam("False_Northing"),
       MakeParam("Scale_Factor"), MakeParam("Azimuth"),
       MakeParam("Longitude_Of_Center"), MakeParam("Latitude_Of_Center"),
       MakeParam("XY_Plane_Rotation")},
      {op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Scale(1.0)),
       op::ParameterValue::create(Deg(45.0)),
       op::ParameterValue::create(Deg(-66.9497)),
       op::ParameterValue::create(Deg(-49.5637)),
       op::ParameterValue::create(Deg(0.0))});
  EXPECT_TRUE(ContainsSubstr(
      adamsWithScale->exportToPROJString(makeProj5().get()), "+proj=spilhaus"));

  auto adamsNoScale = MakeManualConversion(
      NameProps("AdamsNoScale"), NameProps("Adams_Square_II"),
      {MakeParam("False_Easting"), MakeParam("False_Northing"),
       MakeParam("Azimuth"), MakeParam("Longitude_Of_Center"),
       MakeParam("Latitude_Of_Center"), MakeParam("XY_Plane_Rotation")},
      {op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Deg(45.0)),
       op::ParameterValue::create(Deg(-66.9497)),
       op::ParameterValue::create(Deg(-49.5637)),
       op::ParameterValue::create(Deg(0.0))});
  EXPECT_TRUE(ContainsSubstr(
      adamsNoScale->exportToPROJString(makeProj5().get()), "+proj=spilhaus"));

  auto peirceValid = MakeManualConversion(
      NameProps("PeirceSq"),
      NameProps(PROJ_WKT2_NAME_METHOD_PEIRCE_QUINCUNCIAL_SQUARE),
      {op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)},
      {op::ParameterValue::create(Deg(90.0)),
       op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Scale(1.0)),
       op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Metre(0.0))});
  EXPECT_TRUE(ContainsSubstr(peirceValid->exportToPROJString(makeProj5().get()),
                             "+proj=peirce_q"));

  auto peirceBadK = MakeManualConversion(
      NameProps("PeirceBadK"),
      NameProps(PROJ_WKT2_NAME_METHOD_PEIRCE_QUINCUNCIAL_DIAMOND),
      {op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)},
      {op::ParameterValue::create(Deg(90.0)),
       op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Scale(0.99)),
       op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Metre(0.0))});
  EXPECT_THROW(peirceBadK->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  auto peirceBadLat = MakeManualConversion(
      NameProps("PeirceBadLat"),
      NameProps(PROJ_WKT2_NAME_METHOD_PEIRCE_QUINCUNCIAL_SQUARE),
      {op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)},
      {op::ParameterValue::create(Deg(45.0)),
       op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Scale(1.0)),
       op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Metre(0.0))});
  EXPECT_THROW(peirceBadLat->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  // 10. ChangeVerticalUnit (with scalar, DerivedVerticalCRS, standalone throw).
  auto vScalar =
      op::Conversion::createChangeVerticalUnit(NameProps("vS"), Scale(0.3048));
  EXPECT_TRUE(ContainsSubstr(vScalar->exportToPROJString(makeProj5().get()),
                             "+proj=unitconvert"));

  auto vNoFactor = op::Conversion::createChangeVerticalUnit(NameProps("vNo"));
  EXPECT_THROW(vNoFactor->exportToPROJString(makeProj5().get()),
               io::FormattingException);

  auto derivVert = crs::DerivedVerticalCRS::create(
      NameProps("VertM"),
      MakeVerticalCRS("VertFt", common::UnitOfMeasure::FOOT), vNoFactor,
      cs::VerticalCS::createGravityRelatedHeight(common::UnitOfMeasure::METRE));
  EXPECT_TRUE(ContainsSubstr(
      derivVert->derivingConversion()->exportToPROJString(makeProj5().get()),
      "+proj=unitconvert"));

  // 11. R_A / f=0 sphere omission vs ellipsoid emission, PolarStereoB, LCC1SP,
  // and unsupported custom method throw.
  auto vdg = op::Conversion::createVanDerGrinten(NameProps("VDG"), Deg(0.0),
                                                 Metre(0.0), Metre(0.0));
  auto crsVdgWGS84 = MakeWGS84ProjectedCRS("VDG WGS84", vdg);
  EXPECT_TRUE(ContainsSubstr(
      crsVdgWGS84->derivingConversion()->exportToPROJString(makeProj5().get()),
      "+R_A"));
  auto sphCRS = MakeSphericalGeogCRS("SphereGeog");
  auto crsVdgSph = MakeProjectedCRS(
      "VDG Sph", sphCRS, vdg,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE));
  EXPECT_FALSE(ContainsSubstr(
      crsVdgSph->derivingConversion()->exportToPROJString(makeProj5().get()),
      "+R_A"));

  auto srcCompSph = crs::CompoundCRS::create(
      NameProps("Sph+Vert"),
      std::vector<crs::CRSNNPtr>{
          util::nn_static_pointer_cast<crs::CRS>(sphCRS),
          util::nn_static_pointer_cast<crs::CRS>(vertM)});
  auto dstCompVdgSph = crs::CompoundCRS::create(
      NameProps("VdgSph+Vert"),
      std::vector<crs::CRSNNPtr>{
          util::nn_static_pointer_cast<crs::CRS>(crsVdgSph),
          util::nn_static_pointer_cast<crs::CRS>(vertM)});
  auto compConvSph = crsVdgSph->derivingConversion();
  CoordOpPeer::SetCRSs(compConvSph.get(), srcCompSph, dstCompVdgSph, nullptr);
  const std::string compSphStr =
      compConvSph->exportToPROJString(makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(compSphStr, "+proj=vandg"));
  EXPECT_FALSE(ContainsSubstr(compSphStr, "+R_A"));

  auto orthoSph = MakeManualConversion(
      NameProps("OrthoSph"), NameProps(PROJ_WKT2_NAME_ORTHOGRAPHIC_SPHERICAL),
      {op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)},
      {op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Metre(0.0)),
       op::ParameterValue::create(Metre(0.0))});
  EXPECT_TRUE(
      ContainsSubstr(orthoSph->exportToPROJString(makeProj5().get()), "+f=0"));
  auto crsOrthoSph = MakeProjectedCRS(
      "OrthoSph", sphCRS, orthoSph,
      cs::CartesianCS::createEastingNorthing(common::UnitOfMeasure::METRE));
  EXPECT_FALSE(ContainsSubstr(
      crsOrthoSph->derivingConversion()->exportToPROJString(makeProj5().get()),
      "+f=0"));

  auto psBN = op::Conversion::createPolarStereographicVariantB(
      NameProps("PSBN"), Deg(71.0), Deg(0.0), Metre(0.0), Metre(0.0));
  EXPECT_TRUE(
      ContainsSubstr(psBN->exportToPROJString(makeProj5().get()), "+lat_0=90"));
  auto psBS = op::Conversion::createPolarStereographicVariantB(
      NameProps("PSBS"), Deg(-71.0), Deg(0.0), Metre(0.0), Metre(0.0));
  EXPECT_TRUE(ContainsSubstr(psBS->exportToPROJString(makeProj5().get()),
                             "+lat_0=-90"));

  auto lcc1 = op::Conversion::createLambertConicConformal_1SP(
      NameProps("LCC1"), Deg(46.5), Deg(3.0), Scale(0.999), Metre(0.0),
      Metre(0.0));
  const std::string lcc1Str = lcc1->exportToPROJString(makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(lcc1Str, "+lat_1=46.5"));
  EXPECT_TRUE(ContainsSubstr(lcc1Str, "+lat_0=46.5"));

  auto affine2D = op::Conversion::createAffineParametric(
      NameProps("Affine2D"), Metre(1.0), Scale(2.0), Scale(3.0), Metre(4.0),
      Scale(5.0), Scale(6.0));
  const std::string affine2DStr =
      affine2D->exportToPROJString(makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(affine2DStr, "+proj=affine"));
  EXPECT_TRUE(ContainsSubstr(affine2DStr, "+xoff=1"));

  auto affine3D = op::Conversion::createAffineParametric(
      NameProps("Affine3D"), Metre(1.0), Scale(2.0), Scale(3.0), Scale(4.0),
      Metre(5.0), Scale(6.0), Scale(7.0), Scale(8.0), Metre(9.0), Scale(10.0),
      Scale(11.0), Scale(12.0));
  const std::string affine3DStr =
      affine3D->exportToPROJString(makeProj5().get());
  EXPECT_TRUE(ContainsSubstr(affine3DStr, "+proj=affine"));
  EXPECT_TRUE(ContainsSubstr(affine3DStr, "+zoff=9"));

  auto unsupported = op::Conversion::createTunisiaMiningGrid(
      NameProps("TMG"), Deg(36.5), Deg(9.5), Metre(0.0), Metre(0.0));
  EXPECT_THROW(unsupported->exportToPROJString(makeProj5().get()),
               io::FormattingException);
}

TEST(ConversionTest, IsUTMAndIdentifyBranches) {
  int zone = -1;
  bool north = false;

  // 1. Non-TM conversion -> false.
  auto merc = op::Conversion::createMercatorVariantA(
      NameProps("Merc"), Deg(0.0), Deg(0.0), Scale(1.0), Metre(0.0),
      Metre(0.0));
  EXPECT_FALSE(merc->isUTM(zone, north));
  EXPECT_EQ("Merc", merc->identify()->nameStr());

  // 2. TM variants failing individual UTM predicates.
  auto badLat = op::Conversion::createTransverseMercator(
      NameProps("BadLat"), Deg(1.0), Deg(3.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  EXPECT_FALSE(badLat->isUTM(zone, north));

  auto badK = op::Conversion::createTransverseMercator(
      NameProps("BadK"), Deg(0.0), Deg(3.0), Scale(0.9999), Metre(500000.0),
      Metre(0.0));
  EXPECT_FALSE(badK->isUTM(zone, north));

  auto badFE = op::Conversion::createTransverseMercator(
      NameProps("BadFE"), Deg(0.0), Deg(3.0), Scale(0.9996), Metre(400000.0),
      Metre(0.0));
  EXPECT_FALSE(badFE->isUTM(zone, north));

  auto badFN = op::Conversion::createTransverseMercator(
      NameProps("BadFN"), Deg(0.0), Deg(3.0), Scale(0.9996), Metre(500000.0),
      Metre(1000.0));
  EXPECT_FALSE(badFN->isUTM(zone, north));

  auto badCM = op::Conversion::createTransverseMercator(
      NameProps("BadCM"), Deg(0.0), Deg(4.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  EXPECT_FALSE(badCM->isUTM(zone, north));

  auto badZoneRange = op::Conversion::createTransverseMercator(
      NameProps("BadRange"), Deg(0.0), Deg(-183.0), Scale(0.9996),
      Metre(500000.0), Metre(0.0));
  EXPECT_FALSE(badZoneRange->isUTM(zone, north));

  auto radLon = op::Conversion::createTransverseMercator(
      NameProps("RadLon"), Deg(0.0),
      common::Angle(3.0 * std::numbers::pi / 180.0,
                    common::UnitOfMeasure::RADIAN),
      Scale(0.9996), Metre(500000.0), Metre(0.0));
  EXPECT_FALSE(radLon->isUTM(zone, north));
  EXPECT_EQ("RadLon", radLon->identify()->nameStr());

  // 3. Valid UTM 31N (incl. EPSG_CODE_PARAMETER_LONGITUDE_OF_ORIGIN alias) &
  // UTM 19S -> isUTM true and identify() renames & assigns EPSG code.
  auto tm31N = op::Conversion::createTransverseMercator(
      NameProps("RawTM31N"), Deg(0.0), Deg(3.0), Scale(0.9996), Metre(500000.0),
      Metre(0.0));
  EXPECT_TRUE(tm31N->isUTM(zone, north));
  EXPECT_EQ(31, zone);
  EXPECT_TRUE(north);
  auto id31N = tm31N->identify();
  EXPECT_EQ("UTM zone 31N", id31N->nameStr());
  EXPECT_EQ(16031, id31N->getEPSGCode());

  auto tmLonOriginAlias = MakeManualConversion(
      NameProps("RawTM19S"),
      op::createMethodMapNameEPSGCode(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
      {op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_LONGITUDE_OF_ORIGIN),
       op::createOpParamNameEPSGCode(
           EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_EASTING),
       op::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_FALSE_NORTHING)},
      {op::ParameterValue::create(Deg(0.0)),
       op::ParameterValue::create(Deg(-69.0)),
       op::ParameterValue::create(Scale(0.9996)),
       op::ParameterValue::create(Metre(500000.0)),
       op::ParameterValue::create(Metre(10000000.0))});
  EXPECT_TRUE(tmLonOriginAlias->isUTM(zone, north));
  EXPECT_EQ(19, zone);
  EXPECT_FALSE(north);
  auto id19S = tmLonOriginAlias->identify();
  EXPECT_EQ("UTM zone 19S", id19S->nameStr());
  EXPECT_EQ(16119, id19S->getEPSGCode());
}

}  // namespace
}  // namespace proj
