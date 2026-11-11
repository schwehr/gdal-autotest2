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
// Unit tests for ISO 19111 Transformation and InverseTransformation
// (proj/src/iso19111/operation/transformation.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/transformation.cpp

#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
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
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_constants.h"
#include "third_party/proj4/proj/src/proj_internal.h"
#include "third_party/proj4/proj/src/proj_json_streaming_writer.hpp"

namespace proj {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

namespace common = NS_PROJ::common;
namespace crs = NS_PROJ::crs;
namespace cs = NS_PROJ::cs;
namespace datum = NS_PROJ::datum;
namespace io = NS_PROJ::io;
namespace metadata = NS_PROJ::metadata;
namespace operation = NS_PROJ::operation;
namespace util = NS_PROJ::util;

util::PropertyMap NameProps(const char* name) {
  return util::PropertyMap().set(common::IdentifiedObject::NAME_KEY, name);
}

int CountOccurrences(std::string_view text, std::string_view needle) {
  int count = 0;
  std::size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string_view::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

crs::VerticalCRSNNPtr MakeVerticalCRS(const char* name) {
  auto vdatum = datum::VerticalReferenceFrame::create(NameProps(name));
  auto vcs =
      cs::VerticalCS::createGravityRelatedHeight(common::UnitOfMeasure::METRE);
  return crs::VerticalCRS::create(NameProps(name), vdatum, vcs);
}

crs::GeodeticCRSNNPtr MakePlanetocentricCRS(bool reversed_axes) {
  auto ellipsoid = datum::Ellipsoid::createSphere(
      NameProps("Test Sphere"), common::Length(3396190.0), "Mars");
  auto vrf = datum::GeodeticReferenceFrame::create(
      NameProps("Planet Datum"), ellipsoid, util::optional<std::string>(),
      datum::PrimeMeridian::GREENWICH);
  auto lat_axis = cs::CoordinateSystemAxis::create(
      NameProps("planetocentric latitude"), "B", cs::AxisDirection::NORTH,
      common::UnitOfMeasure::DEGREE);
  auto lon_axis = cs::CoordinateSystemAxis::create(
      NameProps("planetocentric longitude"), "L", cs::AxisDirection::EAST,
      common::UnitOfMeasure::DEGREE);
  auto spherical_cs =
      reversed_axes
          ? cs::SphericalCS::create(NameProps("Spherical"), lon_axis, lat_axis)
          : cs::SphericalCS::create(NameProps("Spherical"), lat_axis, lon_axis);
  return crs::GeodeticCRS::create(NameProps("Planetocentric CRS"), vrf,
                                  spherical_cs);
}

std::vector<operation::OperationParameterNNPtr> MakeSevenParamsList() {
  return {
      operation::createOpParamNameEPSGCode(
          EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION),
      operation::createOpParamNameEPSGCode(
          EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION),
      operation::createOpParamNameEPSGCode(
          EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION),
      operation::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_X_AXIS_ROTATION),
      operation::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_Y_AXIS_ROTATION),
      operation::createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_Z_AXIS_ROTATION),
      operation::createOpParamNameEPSGCode(
          EPSG_CODE_PARAMETER_SCALE_DIFFERENCE),
  };
}

std::vector<operation::ParameterValueNNPtr> MakeSevenValuesList(
    double tx, double ty, double tz, double rx, double ry, double rz,
    double scale) {
  return {
      operation::ParameterValue::create(common::Length(tx)),
      operation::ParameterValue::create(common::Length(ty)),
      operation::ParameterValue::create(common::Length(tz)),
      operation::ParameterValue::create(
          common::Angle(rx, common::UnitOfMeasure::ARC_SECOND)),
      operation::ParameterValue::create(
          common::Angle(ry, common::UnitOfMeasure::ARC_SECOND)),
      operation::ParameterValue::create(
          common::Angle(rz, common::UnitOfMeasure::ARC_SECOND)),
      operation::ParameterValue::create(
          common::Scale(scale, common::UnitOfMeasure::PARTS_PER_MILLION)),
  };
}

std::vector<operation::OperationParameterNNPtr> MakeFifteenParamsList() {
  std::vector<operation::OperationParameterNNPtr> params =
      MakeSevenParamsList();
  params.push_back(operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_RATE_X_AXIS_TRANSLATION));
  params.push_back(operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_RATE_Y_AXIS_TRANSLATION));
  params.push_back(operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_RATE_Z_AXIS_TRANSLATION));
  params.push_back(operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_RATE_X_AXIS_ROTATION));
  params.push_back(operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_RATE_Y_AXIS_ROTATION));
  params.push_back(operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_RATE_Z_AXIS_ROTATION));
  params.push_back(operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_RATE_SCALE_DIFFERENCE));
  params.push_back(operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_REFERENCE_EPOCH));
  return params;
}

std::vector<operation::ParameterValueNNPtr> MakeFifteenValuesList(
    double tx, double ty, double tz, double rx, double ry, double rz,
    double scale, double dtx, double dty, double dtz, double drx, double dry,
    double drz, double dscale, double epoch) {
  std::vector<operation::ParameterValueNNPtr> vals =
      MakeSevenValuesList(tx, ty, tz, rx, ry, rz, scale);
  vals.push_back(operation::ParameterValue::create(
      common::Measure(dtx, common::UnitOfMeasure::METRE_PER_YEAR)));
  vals.push_back(operation::ParameterValue::create(
      common::Measure(dty, common::UnitOfMeasure::METRE_PER_YEAR)));
  vals.push_back(operation::ParameterValue::create(
      common::Measure(dtz, common::UnitOfMeasure::METRE_PER_YEAR)));
  vals.push_back(operation::ParameterValue::create(
      common::Measure(drx, common::UnitOfMeasure::ARC_SECOND_PER_YEAR)));
  vals.push_back(operation::ParameterValue::create(
      common::Measure(dry, common::UnitOfMeasure::ARC_SECOND_PER_YEAR)));
  vals.push_back(operation::ParameterValue::create(
      common::Measure(drz, common::UnitOfMeasure::ARC_SECOND_PER_YEAR)));
  vals.push_back(operation::ParameterValue::create(
      common::Measure(dscale, common::UnitOfMeasure::PPM_PER_YEAR)));
  vals.push_back(operation::ParameterValue::create(
      common::Measure(epoch, common::UnitOfMeasure::YEAR)));
  return vals;
}

TEST(TransformationTest, CreateOverloadsValidationCloningAndPromoteDemote) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  auto db_context = io::DatabaseContext::create(std::string(), {}, ctx.get());

  auto param_x = operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION);
  auto param_y = operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION);
  auto val_x = operation::ParameterValue::create(common::Length(1.0));
  auto val_y = operation::ParameterValue::create(common::Length(2.0));

  auto method_two_params = operation::OperationMethod::create(
      NameProps("Two Params Method"),
      std::vector<operation::OperationParameterNNPtr>{param_x, param_y});

  // 1. Parameter count mismatch on first create() overload throws.
  EXPECT_THROW(
      operation::Transformation::create(
          NameProps("Mismatch 1"), crs::GeographicCRS::EPSG_4326,
          crs::GeographicCRS::EPSG_4269, nullptr, method_two_params,
          std::vector<operation::GeneralParameterValueNNPtr>{
              operation::OperationParameterValue::create(param_x, val_x)},
          {}),
      operation::InvalidOperation);

  // 2. Parameter count mismatch on second create() overload throws.
  EXPECT_THROW(
      operation::Transformation::create(
          NameProps("Mismatch 2"), crs::GeographicCRS::EPSG_4326,
          crs::GeographicCRS::EPSG_4269, nullptr,
          NameProps("Two Params Method"),
          std::vector<operation::OperationParameterNNPtr>{param_x, param_y},
          std::vector<operation::ParameterValueNNPtr>{val_x}, {}),
      operation::InvalidOperation);

  // 3. First overload with "ballpark" name, interpolationCRS, and accuracies.
  auto ballpark_op = operation::Transformation::create(
      NameProps("My BallPark Shift"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269,
      crs::GeographicCRS::EPSG_4326.as_nullable(), method_two_params,
      std::vector<operation::GeneralParameterValueNNPtr>{
          operation::OperationParameterValue::create(param_x, val_x),
          operation::OperationParameterValue::create(param_y, val_y)},
      {metadata::PositionalAccuracy::create("2.5")});
  EXPECT_TRUE(ballpark_op->hasBallparkTransformation());
  EXPECT_NE(nullptr, ballpark_op->interpolationCRS());
  ASSERT_THAT(ballpark_op->coordinateOperationAccuracies(), SizeIs(1));
  EXPECT_EQ("2.5", ballpark_op->coordinateOperationAccuracies()[0]->value());
  EXPECT_TRUE(ballpark_op->sourceCRS()->isEquivalentTo(
      crs::GeographicCRS::EPSG_4326.get()));
  EXPECT_TRUE(ballpark_op->targetCRS()->isEquivalentTo(
      crs::GeographicCRS::EPSG_4269.get()));

  // 4. First overload without NAME_KEY does not set ballpark flag.
  auto unnamed_op = operation::Transformation::create(
      util::PropertyMap(), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr, method_two_params,
      std::vector<operation::GeneralParameterValueNNPtr>{
          operation::OperationParameterValue::create(param_x, val_x),
          operation::OperationParameterValue::create(param_y, val_y)},
      {});
  EXPECT_FALSE(unnamed_op->hasBallparkTransformation());
  EXPECT_THAT(unnamed_op->nameStr(), IsEmpty());

  // 5. CoordinateOperation::shallowClone() calls
  // Transformation::_shallowClone()
  //    both on a forward transformation and on an analytic inverse (with
  //    non-null forwardOperation_).
  auto forward_clone = ballpark_op->CoordinateOperation::shallowClone();
  EXPECT_TRUE(forward_clone->isEquivalentTo(ballpark_op.get()));

  auto analytic_fwd = operation::Transformation::createGeocentricTranslations(
      NameProps("Analytic Ballpark Shift"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, 1.0, 2.0, 3.0, {});
  auto analytic_inv = analytic_fwd->inverse();
  EXPECT_TRUE(analytic_inv->hasBallparkTransformation());
  auto analytic_inv_clone = analytic_inv->CoordinateOperation::shallowClone();
  EXPECT_TRUE(analytic_inv_clone->isEquivalentTo(analytic_inv.get()));

  // 6. promoteTo3D and demoteTo2D with nullptr and non-null DatabaseContext.
  auto promoted_no_db = analytic_fwd->promoteTo3D("", nullptr);
  EXPECT_EQ(3u, util::nn_dynamic_pointer_cast<crs::SingleCRS>(
                    promoted_no_db->sourceCRS())
                    ->coordinateSystem()
                    ->axisList()
                    .size());
  auto promoted_with_db = analytic_fwd->promoteTo3D("", db_context);
  EXPECT_EQ(3u, util::nn_dynamic_pointer_cast<crs::SingleCRS>(
                    promoted_with_db->sourceCRS())
                    ->coordinateSystem()
                    ->axisList()
                    .size());

  auto fwd_3d = operation::Transformation::createGeocentricTranslations(
      NameProps("3D Shift"), crs::GeographicCRS::EPSG_4979,
      crs::GeographicCRS::EPSG_4979, 4.0, 5.0, 6.0, {});
  auto demoted_no_db = fwd_3d->demoteTo2D("", nullptr);
  EXPECT_EQ(2u, util::nn_dynamic_pointer_cast<crs::SingleCRS>(
                    demoted_no_db->sourceCRS())
                    ->coordinateSystem()
                    ->axisList()
                    .size());
  auto demoted_with_db = fwd_3d->demoteTo2D("", db_context);
  EXPECT_EQ(2u, util::nn_dynamic_pointer_cast<crs::SingleCRS>(
                    demoted_with_db->sourceCRS())
                    ->coordinateSystem()
                    ->axisList()
                    .size());
}

TEST(TransformationTest, GetTOWGS84ParametersValidAndErrorCases) {
  // 1. All 7 Coordinate Frame EPSG codes invert rotation signs.
  constexpr int kCoordinateFrameCodes[] = {
      EPSG_CODE_METHOD_COORDINATE_FRAME_GEOCENTRIC,
      EPSG_CODE_METHOD_COORDINATE_FRAME_FULL_MATRIX_GEOCENTRIC,
      EPSG_CODE_METHOD_COORDINATE_FRAME_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_COORDINATE_FRAME_FULL_MATRIX_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_COORDINATE_FRAME_FULL_MATRIX_GEOGRAPHIC_3D,
      EPSG_CODE_METHOD_COORDINATE_FRAME_GEOGRAPHIC_3D,
      EPSG_CODE_METHOD_COORDINATE_FRAME_GEOG3D_TO_COMPOUND,
  };
  for (int code : kCoordinateFrameCodes) {
    auto props = NameProps("CF Test").set("OPERATION_METHOD_EPSG_CODE", code);
    auto tf = operation::Transformation::createCoordinateFrameRotation(
        props, crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269,
        10.0, 20.0, 30.0, 0.5, -1.5, 2.5, 3.5, {});
    const std::vector<double> params = tf->getTOWGS84Parameters(true);
    EXPECT_THAT(params, ElementsAre(10.0, 20.0, 30.0, -0.5, 1.5, -2.5, 3.5));
  }

  // Custom 7-param Coordinate Frame without EPSG code, mixed angular/scale
  // units, and 0.0 Z rotation exercising fixNegativeZero(-0.0) -> +0.0.
  auto custom_cf = operation::Transformation::create(
      NameProps("Custom CF Op"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr,
      NameProps("Custom Coordinate Frame"), MakeSevenParamsList(),
      std::vector<operation::ParameterValueNNPtr>{
          operation::ParameterValue::create(common::Length(1.0)),
          operation::ParameterValue::create(common::Length(2.0)),
          operation::ParameterValue::create(common::Length(3.0)),
          operation::ParameterValue::create(
              common::Angle(std::numbers::pi / 180.0 / 3600.0,
                            common::UnitOfMeasure::RADIAN)),
          operation::ParameterValue::create(common::Angle(
              1.0 / 3600.0 * (10.0 / 9.0), common::UnitOfMeasure::GRAD)),
          operation::ParameterValue::create(
              common::Angle(0.0, common::UnitOfMeasure::ARC_SECOND)),
          operation::ParameterValue::create(
              common::Scale(1e-6, common::UnitOfMeasure::SCALE_UNITY)),
      },
      {});
  const std::vector<double> custom_cf_params =
      custom_cf->getTOWGS84Parameters(true);
  ASSERT_THAT(custom_cf_params, SizeIs(7));
  EXPECT_DOUBLE_EQ(1.0, custom_cf_params[0]);
  EXPECT_DOUBLE_EQ(2.0, custom_cf_params[1]);
  EXPECT_DOUBLE_EQ(3.0, custom_cf_params[2]);
  EXPECT_THAT(custom_cf_params[3], DoubleNear(-1.0, 1e-12));
  EXPECT_THAT(custom_cf_params[4], DoubleNear(-1.0, 1e-12));
  EXPECT_EQ(0.0, custom_cf_params[5]);
  EXPECT_FALSE(std::signbit(custom_cf_params[5]));
  EXPECT_THAT(custom_cf_params[6], DoubleNear(1.0, 1e-12));

  // 2. All 3 Position Vector EPSG codes preserve rotation signs.
  constexpr int kPositionVectorCodes[] = {
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC,
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOGRAPHIC_3D,
  };
  for (int code : kPositionVectorCodes) {
    auto props = NameProps("PV Test").set("OPERATION_METHOD_EPSG_CODE", code);
    auto tf = operation::Transformation::createPositionVector(
        props, crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269,
        10.0, -20.0, 30.0, 0.5, -1.5, 2.5, -3.5, {});
    const std::vector<double> params = tf->getTOWGS84Parameters(true);
    EXPECT_THAT(params, ElementsAre(10.0, -20.0, 30.0, 0.5, -1.5, 2.5, -3.5));
  }

  // Custom 7-param Position Vector without EPSG code with -0.0 rotation.
  auto custom_pv = operation::Transformation::create(
      NameProps("Custom PV Op"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr,
      NameProps("Custom Position Vector"), MakeSevenParamsList(),
      MakeSevenValuesList(4.0, 5.0, 6.0, -0.0, 1.0, -2.0, 0.25), {});
  const std::vector<double> custom_pv_params =
      custom_pv->getTOWGS84Parameters(true);
  ASSERT_THAT(custom_pv_params, SizeIs(7));
  EXPECT_FALSE(std::signbit(custom_pv_params[3]));
  EXPECT_THAT(custom_pv_params,
              ElementsAre(4.0, 5.0, 6.0, 0.0, 1.0, -2.0, 0.25));

  // 3. All 3 Geocentric translations EPSG codes + custom without EPSG code.
  constexpr int kGeocentricCodes[] = {
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOCENTRIC,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_3D,
  };
  for (int code : kGeocentricCodes) {
    auto props = NameProps("GT Test").set("OPERATION_METHOD_EPSG_CODE", code);
    auto tf = operation::Transformation::createGeocentricTranslations(
        props, crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269,
        100.0, 200.0, 300.0, {});
    EXPECT_THAT(tf->getTOWGS84Parameters(true),
                ElementsAre(100.0, 200.0, 300.0, 0.0, 0.0, 0.0, 0.0));
  }

  // Note: do not call inverse() on a 0-code custom Geocentric translations op
  // because createMethodMapNameEPSGCode(0) asserts.
  auto custom_gt = operation::Transformation::create(
      NameProps("Custom GT Op"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr,
      NameProps("Custom Geocentric translations"),
      std::vector<operation::OperationParameterNNPtr>{
          operation::createOpParamNameEPSGCode(
              EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION),
          operation::createOpParamNameEPSGCode(
              EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION),
          operation::createOpParamNameEPSGCode(
              EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION),
      },
      std::vector<operation::ParameterValueNNPtr>{
          operation::ParameterValue::create(common::Length(7.0)),
          operation::ParameterValue::create(common::Length(8.0)),
          operation::ParameterValue::create(common::Length(9.0)),
      },
      {});
  EXPECT_THAT(custom_gt->getTOWGS84Parameters(true),
              ElementsAre(7.0, 8.0, 9.0, 0.0, 0.0, 0.0, 0.0));

  // 4. Non-MEASURE parameter value and incomplete parameter sets.
  auto param_x = operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION);
  auto param_y = operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION);
  auto param_z = operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION);
  auto non_measure_gt = operation::Transformation::create(
      NameProps("NonMeasure"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr,
      operation::createMethodMapNameEPSGCode(
          EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D),
      std::vector<operation::OperationParameterNNPtr>{param_x},
      std::vector<operation::ParameterValueNNPtr>{
          operation::ParameterValue::create("not a measure")},
      {});
  EXPECT_THAT(non_measure_gt->getTOWGS84Parameters(false), IsEmpty());
  EXPECT_THROW(non_measure_gt->getTOWGS84Parameters(true),
               io::FormattingException);

  // Method EPSG:9606 with only X, Y, Z (missing rotations and scale).
  auto incomplete_pv = operation::Transformation::create(
      NameProps("Incomplete PV"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr,
      operation::createMethodMapNameEPSGCode(
          EPSG_CODE_METHOD_POSITION_VECTOR_GEOGRAPHIC_2D),
      std::vector<operation::OperationParameterNNPtr>{param_x, param_y,
                                                      param_z},
      std::vector<operation::ParameterValueNNPtr>{
          operation::ParameterValue::create(common::Length(1.0)),
          operation::ParameterValue::create(common::Length(2.0)),
          operation::ParameterValue::create(common::Length(3.0)),
      },
      {});
  EXPECT_THAT(incomplete_pv->getTOWGS84Parameters(false), IsEmpty());
  EXPECT_THROW(incomplete_pv->getTOWGS84Parameters(true),
               io::FormattingException);

  // 5. Unsupported method or wrong paramCount without EPSG code.
  auto ntv2 = operation::Transformation::createNTv2(
      NameProps("NTv2"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, "dummy.gsb", {});
  EXPECT_THAT(ntv2->getTOWGS84Parameters(false), IsEmpty());
  EXPECT_THROW(ntv2->getTOWGS84Parameters(true), io::FormattingException);

  auto cf_three_params_no_code = operation::Transformation::create(
      NameProps("CF Wrong Count"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr,
      NameProps("Custom Coordinate Frame"),
      std::vector<operation::OperationParameterNNPtr>{param_x, param_y,
                                                      param_z},
      std::vector<operation::ParameterValueNNPtr>{
          operation::ParameterValue::create(common::Length(1.0)),
          operation::ParameterValue::create(common::Length(2.0)),
          operation::ParameterValue::create(common::Length(3.0)),
      },
      {});
  EXPECT_THAT(cf_three_params_no_code->getTOWGS84Parameters(false), IsEmpty());
  EXPECT_THROW(cf_three_params_no_code->getTOWGS84Parameters(true),
               io::FormattingException);
}

TEST(TransformationTest, FactoryMethodsAndGetTransformationTypeBranches) {
  auto vert_crs = MakeVerticalCRS("VertCRS");
  auto sphere_src = MakePlanetocentricCRS(/*reversed_axes=*/false);
  auto sphere_rev = MakePlanetocentricCRS(/*reversed_axes=*/true);

  // 1. useOperationMethodEPSGCodeIfPresent: integer override vs string vs
  //    non-BoxedValue BaseObject.
  auto override_int = operation::Transformation::createGeocentricTranslations(
      NameProps("Override Int")
          .set("OPERATION_METHOD_EPSG_CODE",
               EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_3D),
      crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269, 1.0, 2.0,
      3.0, {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_3D,
            override_int->method()->getEPSGCode());

  auto override_str = operation::Transformation::createGeocentricTranslations(
      NameProps("Override Str").set("OPERATION_METHOD_EPSG_CODE", "1035"),
      crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269, 1.0, 2.0,
      3.0, {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
            override_str->method()->getEPSGCode());

  auto override_obj = operation::Transformation::createGeocentricTranslations(
      NameProps("Override Obj")
          .set("OPERATION_METHOD_EPSG_CODE",
               metadata::PositionalAccuracy::create("1.0")),
      crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269, 1.0, 2.0,
      3.0, {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
            override_obj->method()->getEPSGCode());

  // 2. Geocentric, Geog2D, Geog3D, mixed 2D/3D, and SphericalPlanetocentric.
  auto gt_geocen = operation::Transformation::createGeocentricTranslations(
      NameProps("GT Geocen"), crs::GeodeticCRS::EPSG_4978,
      crs::GeodeticCRS::EPSG_4978, 1.0, 2.0, 3.0, {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOCENTRIC,
            gt_geocen->method()->getEPSGCode());

  auto gt_2d3d = operation::Transformation::createGeocentricTranslations(
      NameProps("GT 2D->3D"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4979, 1.0, 2.0, 3.0, {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_3D,
            gt_2d3d->method()->getEPSGCode());

  auto gt_3d2d = operation::Transformation::createGeocentricTranslations(
      NameProps("GT 3D->2D"), crs::GeographicCRS::EPSG_4979,
      crs::GeographicCRS::EPSG_4326, 1.0, 2.0, 3.0, {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_3D,
            gt_3d2d->method()->getEPSGCode());

  auto gt_sphere_src = operation::Transformation::createGeocentricTranslations(
      NameProps("GT Sphere Src"), sphere_src, crs::GeographicCRS::EPSG_4326,
      1.0, 2.0, 3.0, {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
            gt_sphere_src->method()->getEPSGCode());

  auto gt_sphere_dst = operation::Transformation::createGeocentricTranslations(
      NameProps("GT Sphere Dst"), crs::GeographicCRS::EPSG_4326, sphere_rev,
      1.0, 2.0, 3.0, {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
            gt_sphere_dst->method()->getEPSGCode());

  // Position Vector across Geocentric, Geog2D, Geog3D.
  EXPECT_EQ(EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC,
            operation::Transformation::createPositionVector(
                NameProps("PV Geocen"), crs::GeodeticCRS::EPSG_4978,
                crs::GeodeticCRS::EPSG_4978, 1, 2, 3, 4, 5, 6, 7, {})
                ->method()
                ->getEPSGCode());
  EXPECT_EQ(EPSG_CODE_METHOD_POSITION_VECTOR_GEOGRAPHIC_3D,
            operation::Transformation::createPositionVector(
                NameProps("PV Geog3D"), crs::GeographicCRS::EPSG_4979,
                crs::GeographicCRS::EPSG_4979, 1, 2, 3, 4, 5, 6, 7, {})
                ->method()
                ->getEPSGCode());

  // Coordinate Frame across Geocentric, Geog2D, Geog3D.
  EXPECT_EQ(EPSG_CODE_METHOD_COORDINATE_FRAME_GEOCENTRIC,
            operation::Transformation::createCoordinateFrameRotation(
                NameProps("CF Geocen"), crs::GeodeticCRS::EPSG_4978,
                crs::GeodeticCRS::EPSG_4978, 1, 2, 3, 4, 5, 6, 7, {})
                ->method()
                ->getEPSGCode());
  EXPECT_EQ(EPSG_CODE_METHOD_COORDINATE_FRAME_GEOGRAPHIC_3D,
            operation::Transformation::createCoordinateFrameRotation(
                NameProps("CF Geog3D"), crs::GeographicCRS::EPSG_4979,
                crs::GeographicCRS::EPSG_4979, 1, 2, 3, 4, 5, 6, 7, {})
                ->method()
                ->getEPSGCode());

  // Time-dependent Position Vector across Geocentric, Geog2D, Geog3D.
  EXPECT_EQ(EPSG_CODE_METHOD_TIME_DEPENDENT_POSITION_VECTOR_GEOCENTRIC,
            operation::Transformation::createTimeDependentPositionVector(
                NameProps("TD-PV Geocen"), crs::GeodeticCRS::EPSG_4978,
                crs::GeodeticCRS::EPSG_4978, 1, 2, 3, 4, 5, 6, 7, 0.1, 0.2, 0.3,
                0.01, 0.02, 0.03, 0.04, 2010.0, {})
                ->method()
                ->getEPSGCode());
  EXPECT_EQ(EPSG_CODE_METHOD_TIME_DEPENDENT_POSITION_VECTOR_GEOGRAPHIC_2D,
            operation::Transformation::createTimeDependentPositionVector(
                NameProps("TD-PV 2D"), crs::GeographicCRS::EPSG_4326,
                crs::GeographicCRS::EPSG_4269, 1, 2, 3, 4, 5, 6, 7, 0.1, 0.2,
                0.3, 0.01, 0.02, 0.03, 0.04, 2010.0, {})
                ->method()
                ->getEPSGCode());
  EXPECT_EQ(EPSG_CODE_METHOD_TIME_DEPENDENT_POSITION_VECTOR_GEOGRAPHIC_3D,
            operation::Transformation::createTimeDependentPositionVector(
                NameProps("TD-PV 3D"), crs::GeographicCRS::EPSG_4979,
                crs::GeographicCRS::EPSG_4979, 1, 2, 3, 4, 5, 6, 7, 0.1, 0.2,
                0.3, 0.01, 0.02, 0.03, 0.04, 2010.0, {})
                ->method()
                ->getEPSGCode());

  // Time-dependent Coordinate Frame across Geocentric, Geog2D, Geog3D.
  EXPECT_EQ(
      EPSG_CODE_METHOD_TIME_DEPENDENT_COORDINATE_FRAME_GEOCENTRIC,
      operation::Transformation::createTimeDependentCoordinateFrameRotation(
          NameProps("TD-CF Geocen"), crs::GeodeticCRS::EPSG_4978,
          crs::GeodeticCRS::EPSG_4978, 1, 2, 3, 4, 5, 6, 7, 0.1, 0.2, 0.3, 0.01,
          0.02, 0.03, 0.04, 2010.0, {})
          ->method()
          ->getEPSGCode());
  EXPECT_EQ(
      EPSG_CODE_METHOD_TIME_DEPENDENT_COORDINATE_FRAME_GEOGRAPHIC_2D,
      operation::Transformation::createTimeDependentCoordinateFrameRotation(
          NameProps("TD-CF 2D"), crs::GeographicCRS::EPSG_4326,
          crs::GeographicCRS::EPSG_4269, 1, 2, 3, 4, 5, 6, 7, 0.1, 0.2, 0.3,
          0.01, 0.02, 0.03, 0.04, 2010.0, {})
          ->method()
          ->getEPSGCode());
  EXPECT_EQ(
      EPSG_CODE_METHOD_TIME_DEPENDENT_COORDINATE_FRAME_GEOGRAPHIC_3D,
      operation::Transformation::createTimeDependentCoordinateFrameRotation(
          NameProps("TD-CF 3D"), crs::GeographicCRS::EPSG_4979,
          crs::GeographicCRS::EPSG_4979, 1, 2, 3, 4, 5, 6, 7, 0.1, 0.2, 0.3,
          0.01, 0.02, 0.03, 0.04, 2010.0, {})
          ->method()
          ->getEPSGCode());

  // 3. Invalid CRS pairs throw InvalidOperation in getTransformationType.
  EXPECT_THROW(operation::Transformation::createGeocentricTranslations(
                   NameProps("Bad Geocen->Geog"), crs::GeodeticCRS::EPSG_4978,
                   crs::GeographicCRS::EPSG_4326, 1, 2, 3, {}),
               operation::InvalidOperation);
  EXPECT_THROW(operation::Transformation::createGeocentricTranslations(
                   NameProps("Bad Geog->Geocen"), crs::GeographicCRS::EPSG_4326,
                   crs::GeodeticCRS::EPSG_4978, 1, 2, 3, {}),
               operation::InvalidOperation);
  EXPECT_THROW(operation::Transformation::createGeocentricTranslations(
                   NameProps("Bad Vert Src"), vert_crs,
                   crs::GeographicCRS::EPSG_4326, 1, 2, 3, {}),
               operation::InvalidOperation);
  EXPECT_THROW(operation::Transformation::createGeocentricTranslations(
                   NameProps("Bad Vert Dst"), crs::GeographicCRS::EPSG_4326,
                   vert_crs, 1, 2, 3, {}),
               operation::InvalidOperation);

  // 4. Molodensky & Abridged Molodensky.
  auto mol = operation::Transformation::createMolodensky(
      NameProps("Molodensky"), crs::GeographicCRS::EPSG_4267,
      crs::GeographicCRS::EPSG_4326, 1.0, 2.0, 3.0, -69.4, -3.7e-5, {});
  EXPECT_EQ(EPSG_CODE_METHOD_MOLODENSKY, mol->method()->getEPSGCode());

  auto abridged_mol = operation::Transformation::createAbridgedMolodensky(
      NameProps("Abridged Molodensky"), crs::GeographicCRS::EPSG_4267,
      crs::GeographicCRS::EPSG_4326, 1.0, 2.0, 3.0, -69.4, -3.7e-5, {});
  EXPECT_EQ(EPSG_CODE_METHOD_ABRIDGED_MOLODENSKY,
            abridged_mol->method()->getEPSGCode());

  // 5. createTOWGS84 validation and valid CRS variants.
  EXPECT_THROW(operation::Transformation::createTOWGS84(
                   crs::GeographicCRS::EPSG_4326, {1.0, 2.0}),
               operation::InvalidOperation);
  EXPECT_THROW(operation::Transformation::createTOWGS84(
                   crs::GeographicCRS::EPSG_4326, {1.0, 2.0, 3.0, 4.0, 5.0}),
               operation::InvalidOperation);
  EXPECT_THROW(
      operation::Transformation::createTOWGS84(vert_crs, {1.0, 2.0, 3.0}),
      operation::InvalidOperation);

  auto towgs3_geog = operation::Transformation::createTOWGS84(
      crs::GeographicCRS::EPSG_4269, {1.0, 2.0, 3.0});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
            towgs3_geog->method()->getEPSGCode());
  EXPECT_TRUE(towgs3_geog->targetCRS()->isEquivalentTo(
      crs::GeographicCRS::EPSG_4326.get()));

  auto towgs7_geocen = operation::Transformation::createTOWGS84(
      crs::GeodeticCRS::EPSG_4978, {1.0, 2.0, 3.0, 0.1, 0.2, 0.3, 0.4});
  EXPECT_EQ(EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC,
            towgs7_geocen->method()->getEPSGCode());
  EXPECT_TRUE(towgs7_geocen->targetCRS()->isEquivalentTo(
      crs::GeodeticCRS::EPSG_4978.get()));

  auto towgs3_sphere =
      operation::Transformation::createTOWGS84(sphere_src, {1.0, 2.0, 3.0});
  EXPECT_TRUE(towgs3_sphere->targetCRS()->isEquivalentTo(
      crs::GeographicCRS::EPSG_4326.get()));

  // 6. Grid and offset factories.
  auto ntv2 = operation::Transformation::createNTv2(
      NameProps("NTv2 Grid"), crs::GeographicCRS::EPSG_4269,
      crs::GeographicCRS::EPSG_4326, "ca_nrc_ntv2_0.gsb", {});
  EXPECT_EQ(EPSG_CODE_METHOD_NTV2, ntv2->method()->getEPSGCode());

  auto h_to_geog3d =
      operation::Transformation::createGravityRelatedHeightToGeographic3D(
          NameProps("Geoid Grid"), vert_crs, crs::GeographicCRS::EPSG_4979,
          crs::GeographicCRS::EPSG_4326.as_nullable(), "us_noaa_g2018u0.tif",
          {});
  EXPECT_EQ(PROJ_WKT2_NAME_METHOD_HEIGHT_TO_GEOG3D,
            h_to_geog3d->method()->nameStr());
  EXPECT_NE(nullptr, h_to_geog3d->interpolationCRS());

  auto vertcon = operation::Transformation::createVERTCON(
      NameProps("VERTCON Grid"), vert_crs, vert_crs, "us_noaa_vertconc.tif",
      {});
  EXPECT_EQ(EPSG_CODE_METHOD_VERTCON, vertcon->method()->getEPSGCode());

  auto lon_rot = operation::Transformation::createLongitudeRotation(
      NameProps("Lon Rot"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4326,
      common::Angle(2.5, common::UnitOfMeasure::DEGREE));
  EXPECT_EQ(EPSG_CODE_METHOD_LONGITUDE_ROTATION,
            lon_rot->method()->getEPSGCode());
  ASSERT_THAT(lon_rot->coordinateOperationAccuracies(), SizeIs(1));
  EXPECT_EQ("0", lon_rot->coordinateOperationAccuracies()[0]->value());

  auto g2d_off = operation::Transformation::createGeographic2DOffsets(
      NameProps("Geog2D Offsets"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269,
      common::Angle(1.0, common::UnitOfMeasure::ARC_SECOND),
      common::Angle(-2.0, common::UnitOfMeasure::ARC_SECOND), {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOGRAPHIC2D_OFFSETS,
            g2d_off->method()->getEPSGCode());

  auto g3d_off = operation::Transformation::createGeographic3DOffsets(
      NameProps("Geog3D Offsets"), crs::GeographicCRS::EPSG_4979,
      crs::GeographicCRS::EPSG_4979,
      common::Angle(1.0, common::UnitOfMeasure::ARC_SECOND),
      common::Angle(-2.0, common::UnitOfMeasure::ARC_SECOND),
      common::Length(3.5), {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOGRAPHIC3D_OFFSETS,
            g3d_off->method()->getEPSGCode());

  auto g2dh_off =
      operation::Transformation::createGeographic2DWithHeightOffsets(
          NameProps("Geog2D+H Offsets"), crs::GeographicCRS::EPSG_4326,
          crs::GeographicCRS::EPSG_4269,
          common::Angle(1.0, common::UnitOfMeasure::ARC_SECOND),
          common::Angle(-2.0, common::UnitOfMeasure::ARC_SECOND),
          common::Length(42.0), {});
  EXPECT_EQ(EPSG_CODE_METHOD_GEOGRAPHIC2D_WITH_HEIGHT_OFFSETS,
            g2dh_off->method()->getEPSGCode());

  auto cart_off = operation::Transformation::createCartesianGridOffsets(
      NameProps("Cart Offsets"), crs::GeodeticCRS::EPSG_4978,
      crs::GeodeticCRS::EPSG_4978, common::Length(10.0), common::Length(-20.0),
      {});
  EXPECT_EQ(EPSG_CODE_METHOD_CARTESIAN_GRID_OFFSETS,
            cart_off->method()->getEPSGCode());

  auto vert_off = operation::Transformation::createVerticalOffset(
      NameProps("Vert Offset"), vert_crs, vert_crs, common::Length(1.25), {});
  EXPECT_EQ(EPSG_CODE_METHOD_VERTICAL_OFFSET,
            vert_off->method()->getEPSGCode());

  auto chg_vunit = operation::Transformation::createChangeVerticalUnit(
      NameProps("Change Vert Unit"), vert_crs, vert_crs,
      common::Scale(0.3048, common::UnitOfMeasure::SCALE_UNITY), {});
  EXPECT_EQ(EPSG_CODE_METHOD_CHANGE_VERTICAL_UNIT,
            chg_vunit->method()->getEPSGCode());
}

TEST(TransformationTest, AnalyticInversesNegateAndDoubleInverse) {
  auto vert_crs = MakeVerticalCRS("VertCRS");

  // 1. Geocentric translations: EPSG code path even when method name does not
  //    contain "Geocentric translations", plus negate(0.0) / negate(-0.0).
  constexpr int kGeocentricCodes[] = {
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOCENTRIC,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_3D,
  };
  for (int code : kGeocentricCodes) {
    auto custom_named_gt = operation::Transformation::create(
        NameProps("Shift"), crs::GeographicCRS::EPSG_4326,
        crs::GeographicCRS::EPSG_4269, nullptr,
        util::PropertyMap()
            .set(common::IdentifiedObject::NAME_KEY, "Alias Without Match")
            .set(metadata::Identifier::CODESPACE_KEY,
                 metadata::Identifier::EPSG)
            .set(metadata::Identifier::CODE_KEY, code),
        std::vector<operation::OperationParameterNNPtr>{
            operation::createOpParamNameEPSGCode(
                EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION),
            operation::createOpParamNameEPSGCode(
                EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION),
            operation::createOpParamNameEPSGCode(
                EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION),
        },
        std::vector<operation::ParameterValueNNPtr>{
            operation::ParameterValue::create(common::Length(10.0)),
            operation::ParameterValue::create(common::Length(0.0)),
            operation::ParameterValue::create(common::Length(-0.0)),
        },
        {});
    auto inv = util::nn_dynamic_pointer_cast<operation::Transformation>(
        custom_named_gt->inverse());
    ASSERT_NE(nullptr, inv);
    EXPECT_DOUBLE_EQ(-10.0, inv->parameterValueNumericAsSI(
                                EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION));
    EXPECT_DOUBLE_EQ(0.0, inv->parameterValueNumericAsSI(
                              EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION));
    EXPECT_FALSE(std::signbit(inv->parameterValueNumericAsSI(
        EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION)));
    EXPECT_FALSE(std::signbit(inv->parameterValueNumericAsSI(
        EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION)));
    // Calling inverse() on the analytic inverse returns the registered forward.
    auto dbl_inv = inv->inverse();
    EXPECT_TRUE(dbl_inv->isEquivalentTo(custom_named_gt.get()));
  }

  // 2. Molodensky & Abridged Molodensky.
  auto mol = operation::Transformation::createMolodensky(
      NameProps("Molodensky"), crs::GeographicCRS::EPSG_4267,
      crs::GeographicCRS::EPSG_4326, 10.0, -20.0, 30.0, -69.4, -3.7e-5, {});
  auto mol_inv =
      util::nn_dynamic_pointer_cast<operation::Transformation>(mol->inverse());
  ASSERT_NE(nullptr, mol_inv);
  EXPECT_DOUBLE_EQ(-10.0, mol_inv->parameterValueNumericAsSI(
                              EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION));
  EXPECT_DOUBLE_EQ(69.4, mol_inv->parameterValueNumericAsSI(
                             EPSG_CODE_PARAMETER_SEMI_MAJOR_AXIS_DIFFERENCE));
  EXPECT_TRUE(mol_inv->inverse()->isEquivalentTo(mol.get()));

  auto ab_mol = operation::Transformation::createAbridgedMolodensky(
      NameProps("Abridged Molodensky"), crs::GeographicCRS::EPSG_4267,
      crs::GeographicCRS::EPSG_4326, 10.0, -20.0, 30.0, -69.4, -3.7e-5, {});
  auto ab_mol_inv = util::nn_dynamic_pointer_cast<operation::Transformation>(
      ab_mol->inverse());
  ASSERT_NE(nullptr, ab_mol_inv);
  EXPECT_EQ(EPSG_CODE_METHOD_ABRIDGED_MOLODENSKY,
            ab_mol_inv->method()->getEPSGCode());
  EXPECT_DOUBLE_EQ(20.0, ab_mol_inv->parameterValueNumericAsSI(
                             EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION));

  // 3. Longitude rotation (non-degree unit preserved).
  auto lon_rot = operation::Transformation::createLongitudeRotation(
      NameProps("Paris Meridian"), crs::GeographicCRS::EPSG_4807,
      crs::GeographicCRS::EPSG_4326,
      common::Angle(2.5969213, common::UnitOfMeasure::GRAD));
  auto lon_rot_inv = util::nn_dynamic_pointer_cast<operation::Transformation>(
      lon_rot->inverse());
  ASSERT_NE(nullptr, lon_rot_inv);
  const auto& inv_offset =
      lon_rot_inv->parameterValueMeasure(EPSG_CODE_PARAMETER_LONGITUDE_OFFSET);
  EXPECT_DOUBLE_EQ(-2.5969213, inv_offset.value());
  EXPECT_EQ(common::UnitOfMeasure::GRAD, inv_offset.unit());

  // 4. Geographic2D, Geographic3D, Geographic2DWithHeight, CartesianGrid,
  //    Vertical offsets.
  auto g2d = operation::Transformation::createGeographic2DOffsets(
      NameProps("G2D"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269,
      common::Angle(1.5, common::UnitOfMeasure::ARC_SECOND),
      common::Angle(-2.5, common::UnitOfMeasure::ARC_SECOND), {});
  auto g2d_inv =
      util::nn_dynamic_pointer_cast<operation::Transformation>(g2d->inverse());
  ASSERT_NE(nullptr, g2d_inv);
  EXPECT_DOUBLE_EQ(
      -1.5, g2d_inv->parameterValueMeasure(EPSG_CODE_PARAMETER_LATITUDE_OFFSET)
                .value());
  EXPECT_DOUBLE_EQ(
      2.5, g2d_inv->parameterValueMeasure(EPSG_CODE_PARAMETER_LONGITUDE_OFFSET)
               .value());

  auto g3d = operation::Transformation::createGeographic3DOffsets(
      NameProps("G3D"), crs::GeographicCRS::EPSG_4979,
      crs::GeographicCRS::EPSG_4979,
      common::Angle(1.0, common::UnitOfMeasure::ARC_SECOND),
      common::Angle(2.0, common::UnitOfMeasure::ARC_SECOND),
      common::Length(5.0), {});
  auto g3d_inv =
      util::nn_dynamic_pointer_cast<operation::Transformation>(g3d->inverse());
  ASSERT_NE(nullptr, g3d_inv);
  EXPECT_DOUBLE_EQ(
      -5.0, g3d_inv->parameterValueMeasure(EPSG_CODE_PARAMETER_VERTICAL_OFFSET)
                .value());

  auto g2dh = operation::Transformation::createGeographic2DWithHeightOffsets(
      NameProps("G2DH"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269,
      common::Angle(1.0, common::UnitOfMeasure::ARC_SECOND),
      common::Angle(2.0, common::UnitOfMeasure::ARC_SECOND),
      common::Length(33.5), {});
  auto g2dh_inv =
      util::nn_dynamic_pointer_cast<operation::Transformation>(g2dh->inverse());
  ASSERT_NE(nullptr, g2dh_inv);
  EXPECT_DOUBLE_EQ(
      -33.5, g2dh_inv->parameterValueMeasure(EPSG_CODE_PARAMETER_GEOID_HEIGHT)
                 .value());

  auto cart = operation::Transformation::createCartesianGridOffsets(
      NameProps("Cart"), crs::GeodeticCRS::EPSG_4978,
      crs::GeodeticCRS::EPSG_4978, common::Length(12.5), common::Length(-7.5),
      {});
  auto cart_inv =
      util::nn_dynamic_pointer_cast<operation::Transformation>(cart->inverse());
  ASSERT_NE(nullptr, cart_inv);
  EXPECT_DOUBLE_EQ(
      -12.5, cart_inv->parameterValueMeasure(EPSG_CODE_PARAMETER_EASTING_OFFSET)
                 .value());
  EXPECT_DOUBLE_EQ(
      7.5, cart_inv->parameterValueMeasure(EPSG_CODE_PARAMETER_NORTHING_OFFSET)
               .value());

  auto vert_off = operation::Transformation::createVerticalOffset(
      NameProps("VertOff"), vert_crs, vert_crs, common::Length(4.5), {});
  auto vert_off_inv = util::nn_dynamic_pointer_cast<operation::Transformation>(
      vert_off->inverse());
  ASSERT_NE(nullptr, vert_off_inv);
  EXPECT_DOUBLE_EQ(
      -4.5,
      vert_off_inv->parameterValueMeasure(EPSG_CODE_PARAMETER_VERTICAL_OFFSET)
          .value());

  // 5. Change of Vertical Unit: non-zero factor (2.5 -> 0.4) and zero factor
  //    (0.0 -> 0.0).
  auto cvu = operation::Transformation::createChangeVerticalUnit(
      NameProps("CVU"), vert_crs, vert_crs,
      common::Scale(2.5, common::UnitOfMeasure::SCALE_UNITY), {});
  auto cvu_inv =
      util::nn_dynamic_pointer_cast<operation::Transformation>(cvu->inverse());
  ASSERT_NE(nullptr, cvu_inv);
  EXPECT_NEAR(0.4,
              cvu_inv->parameterValueNumericAsSI(
                  EPSG_CODE_PARAMETER_UNIT_CONVERSION_SCALAR),
              1e-15);

  auto cvu_zero = operation::Transformation::createChangeVerticalUnit(
      NameProps("CVU Zero"), vert_crs, vert_crs,
      common::Scale(0.0, common::UnitOfMeasure::SCALE_UNITY), {});
  auto cvu_zero_inv = util::nn_dynamic_pointer_cast<operation::Transformation>(
      cvu_zero->inverse());
  ASSERT_NE(nullptr, cvu_zero_inv);
  EXPECT_DOUBLE_EQ(0.0, cvu_zero_inv->parameterValueNumericAsSI(
                            EPSG_CODE_PARAMETER_UNIT_CONVERSION_SCALAR));
}

TEST(TransformationTest, InverseTransformationCloningAndApproximateWKTExport) {
  auto vert_crs = MakeVerticalCRS("VertCRS");
  auto formatter_wkt2 = [] {
    return io::WKTFormatter::create(io::WKTFormatter::Convention::WKT2_2019);
  };

  // 1. Coordinate Frame 7-param: all 7 EPSG codes + custom without EPSG code,
  //    both exact (zero rot/scale) and approximate (non-zero rot/scale).
  constexpr int kCfCodes[] = {
      EPSG_CODE_METHOD_COORDINATE_FRAME_GEOCENTRIC,
      EPSG_CODE_METHOD_COORDINATE_FRAME_FULL_MATRIX_GEOCENTRIC,
      EPSG_CODE_METHOD_COORDINATE_FRAME_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_COORDINATE_FRAME_FULL_MATRIX_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_COORDINATE_FRAME_FULL_MATRIX_GEOGRAPHIC_3D,
      EPSG_CODE_METHOD_COORDINATE_FRAME_GEOGRAPHIC_3D,
      EPSG_CODE_METHOD_COORDINATE_FRAME_GEOG3D_TO_COMPOUND,
  };
  for (int code : kCfCodes) {
    auto cf_approx = operation::Transformation::createCoordinateFrameRotation(
        NameProps("CF Shift").set("OPERATION_METHOD_EPSG_CODE", code),
        crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269, 1.0, 2.0,
        3.0, 0.1, 0.2, 0.3, 0.4, {});
    auto inv = cf_approx->inverse();
    auto* inv_ptr =
        dynamic_cast<const operation::InverseTransformation*>(inv.get());
    ASSERT_NE(nullptr, inv_ptr);
    EXPECT_TRUE(
        inv_ptr->inverseAsTransformation()->isEquivalentTo(cf_approx.get()));
    EXPECT_TRUE(inv->inverse()->isEquivalentTo(cf_approx.get()));
    EXPECT_TRUE(
        inv->CoordinateOperation::shallowClone()->isEquivalentTo(inv.get()));
    const std::string wkt = inv->exportToWKT(formatter_wkt2().get());
    EXPECT_THAT(wkt, HasSubstr("COORDINATEOPERATION"));
    EXPECT_THAT(wkt, HasSubstr("(approx. inversion)"));
  }

  auto cf_exact = operation::Transformation::create(
      NameProps("CF Exact"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr,
      NameProps("Custom Coordinate Frame"), MakeSevenParamsList(),
      MakeSevenValuesList(5.0, -6.0, 7.0, 0.0, 0.0, 0.0, 0.0), {});
  const std::string cf_exact_wkt =
      cf_exact->inverse()->exportToWKT(formatter_wkt2().get());
  EXPECT_THAT(cf_exact_wkt, HasSubstr("Inverse of CF Exact"));
  EXPECT_THAT(cf_exact_wkt, Not(HasSubstr("(approx. inversion)")));

  // 2. Position Vector 7-param: all 3 EPSG codes + custom without EPSG code.
  constexpr int kPvCodes[] = {
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC,
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOGRAPHIC_3D,
  };
  for (int code : kPvCodes) {
    auto pv = operation::Transformation::createPositionVector(
        NameProps("PV Shift").set("OPERATION_METHOD_EPSG_CODE", code),
        crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269, 1.0, 2.0,
        3.0, 0.1, 0.0, 0.0, 0.0, {});
    const std::string wkt = pv->inverse()->exportToWKT(formatter_wkt2().get());
    EXPECT_THAT(wkt, HasSubstr("(approx. inversion)"));
  }

  auto pv_custom = operation::Transformation::create(
      NameProps("PV Custom"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr,
      NameProps("Custom Position Vector"), MakeSevenParamsList(),
      MakeSevenValuesList(1.0, 2.0, 3.0, 0.5, 0.6, 0.7, 0.8), {});
  const std::string pv_custom_wkt =
      pv_custom->inverse()->exportToWKT(formatter_wkt2().get());
  EXPECT_THAT(pv_custom_wkt, HasSubstr("Custom Position Vector"));
  EXPECT_THAT(pv_custom_wkt, HasSubstr("(approx. inversion)"));

  // 3. Time-dependent Coordinate Frame 15-param: all 3 EPSG codes + non-zero
  //    rate_scaleDiff branch + custom without EPSG code.
  constexpr int kTdCfCodes[] = {
      EPSG_CODE_METHOD_TIME_DEPENDENT_COORDINATE_FRAME_GEOCENTRIC,
      EPSG_CODE_METHOD_TIME_DEPENDENT_COORDINATE_FRAME_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_TIME_DEPENDENT_COORDINATE_FRAME_GEOGRAPHIC_3D,
  };
  for (int code : kTdCfCodes) {
    auto tdcf =
        operation::Transformation::createTimeDependentCoordinateFrameRotation(
            NameProps("TD-CF").set("OPERATION_METHOD_EPSG_CODE", code),
            crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269, 1.0,
            2.0, 3.0, 0.0, 0.0, 0.0, 0.0, 0.1, 0.2, 0.3, 0.05, 0.0, 0.0, 0.0,
            2015.0, {});
    const std::string wkt =
        tdcf->inverse()->exportToWKT(formatter_wkt2().get());
    EXPECT_THAT(wkt, HasSubstr("(approx. inversion)"));
  }

  auto tdcf_rate_scale =
      operation::Transformation::createTimeDependentCoordinateFrameRotation(
          NameProps("TD-CF ScaleRate"), crs::GeographicCRS::EPSG_4326,
          crs::GeographicCRS::EPSG_4269, 1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 0.0, 0.0,
          0.0, 0.0, 0.0, 0.0, 0.0, 0.02, 2015.0, {});
  EXPECT_THAT(tdcf_rate_scale->inverse()->exportToWKT(formatter_wkt2().get()),
              HasSubstr("(approx. inversion)"));

  auto tdcf_custom = operation::Transformation::create(
      NameProps("TD-CF Custom Exact"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr,
      NameProps("Custom Time-dependent Coordinate Frame"),
      MakeFifteenParamsList(),
      MakeFifteenValuesList(1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 0.0, 0.1, 0.2, 0.3,
                            0.0, 0.0, 0.0, 0.0, 2020.0),
      {});
  const std::string tdcf_custom_wkt =
      tdcf_custom->inverse()->exportToWKT(formatter_wkt2().get());
  EXPECT_THAT(tdcf_custom_wkt, HasSubstr("Inverse of TD-CF Custom Exact"));
  EXPECT_THAT(tdcf_custom_wkt, Not(HasSubstr("(approx. inversion)")));

  // 4. Time-dependent Position Vector 15-param: all 3 EPSG codes + custom.
  constexpr int kTdPvCodes[] = {
      EPSG_CODE_METHOD_TIME_DEPENDENT_POSITION_VECTOR_GEOCENTRIC,
      EPSG_CODE_METHOD_TIME_DEPENDENT_POSITION_VECTOR_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_TIME_DEPENDENT_POSITION_VECTOR_GEOGRAPHIC_3D,
  };
  for (int code : kTdPvCodes) {
    auto tdpv = operation::Transformation::createTimeDependentPositionVector(
        NameProps("TD-PV").set("OPERATION_METHOD_EPSG_CODE", code),
        crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269, 1.0, 2.0,
        3.0, 0.0, 0.0, 0.0, 0.0, 0.1, 0.2, 0.3, 0.0, 0.0, 0.0, 0.0, 2010.0, {});
    const std::string wkt =
        tdpv->inverse()->exportToWKT(formatter_wkt2().get());
    EXPECT_THAT(wkt, HasSubstr("COORDINATEOPERATION"));
    EXPECT_THAT(wkt, Not(HasSubstr("(approx. inversion)")));
  }

  auto tdpv_custom = operation::Transformation::create(
      NameProps("TD-PV Custom"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269, nullptr,
      NameProps("Custom Time-dependent Position Vector"),
      MakeFifteenParamsList(),
      MakeFifteenValuesList(1.0, 2.0, 3.0, 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                            0.0, 0.0, 0.0, 0.0, 2012.0),
      {});
  EXPECT_THAT(tdpv_custom->inverse()->exportToWKT(formatter_wkt2().get()),
              HasSubstr("(approx. inversion)"));

  // 5. Fallback non-approximate InverseTransformation WKT export: NTv2,
  //    VERTCON, GravityRelatedHeightToGeographic3D, and
  //    "Geocentric translations (geog2D domain) by grid (IGN)" (contains both
  //    "Geocentric translations" and "grid", so skipped at line 1574-1575).
  auto ntv2 = operation::Transformation::createNTv2(
      NameProps("NTv2 Shift"), crs::GeographicCRS::EPSG_4269,
      crs::GeographicCRS::EPSG_4326, "grid.gsb", {});
  auto ntv2_inv = ntv2->inverse();
  EXPECT_NE(nullptr, dynamic_cast<const operation::InverseTransformation*>(
                         ntv2_inv.get()));
  EXPECT_THAT(ntv2_inv->exportToWKT(formatter_wkt2().get()),
              HasSubstr("Inverse of NTv2 Shift"));

  auto vertcon = operation::Transformation::createVERTCON(
      NameProps("VERTCON Shift"), vert_crs, vert_crs, "vert.tif", {});
  auto vertcon_inv = vertcon->inverse();
  EXPECT_NE(nullptr, dynamic_cast<const operation::InverseTransformation*>(
                         vertcon_inv.get()));
  EXPECT_THAT(vertcon_inv->exportToWKT(formatter_wkt2().get()),
              HasSubstr("Inverse of VERTCON Shift"));

  auto param_grid = operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE);
  auto ign_grid = operation::Transformation::create(
      NameProps("IGN Grid"), crs::GeographicCRS::EPSG_4326,
      crs::GeographicCRS::EPSG_4269,
      crs::GeographicCRS::EPSG_4326.as_nullable(),
      operation::createMethodMapNameEPSGCode(
          EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN),
      std::vector<operation::OperationParameterNNPtr>{param_grid},
      std::vector<operation::ParameterValueNNPtr>{
          operation::ParameterValue::createFilename("gr3df97a.tif")},
      {});
  auto ign_inv = ign_grid->inverse();
  EXPECT_NE(nullptr, dynamic_cast<const operation::InverseTransformation*>(
                         ign_inv.get()));
  EXPECT_THAT(ign_inv->exportToWKT(formatter_wkt2().get()),
              HasSubstr("Inverse of IGN Grid"));
}

TEST(TransformationTest, ExportToWKTJSONAndPROJString) {
  auto vert_crs = MakeVerticalCRS("VertCRS");

  // 1. WKT2 succeeds; WKT1_GDAL throws FormattingException.
  auto gt = operation::Transformation::createGeocentricTranslations(
      NameProps("WKT Shift"), crs::GeodeticCRS::EPSG_4978,
      crs::GeodeticCRS::EPSG_4978, 1.0, 2.0, 3.0, {});
  auto wkt2 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(gt->exportToWKT(wkt2.get()), HasSubstr("COORDINATEOPERATION"));
  auto wkt1 = io::WKTFormatter::create(io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THROW(gt->exportToWKT(wkt1.get()), io::FormattingException);

  // 2. JSON export: unnamed -> "unnamed", interpolation_crs, accuracy.
  //    CPLJSonStreamingWriter pretty-prints by default (": ").
  auto h_to_g3d_full =
      operation::Transformation::createGravityRelatedHeightToGeographic3D(
          util::PropertyMap(), vert_crs, crs::GeographicCRS::EPSG_4979,
          crs::GeographicCRS::EPSG_4326.as_nullable(), "geoid.tif",
          {metadata::PositionalAccuracy::create("0.5")});
  auto json_full = io::JSONFormatter::create();
  const std::string json_full_str =
      h_to_g3d_full->exportToJSON(json_full.get());
  EXPECT_THAT(json_full_str, HasSubstr("\"unnamed\""));
  EXPECT_THAT(json_full_str, HasSubstr("\"interpolation_crs\""));
  EXPECT_THAT(json_full_str, HasSubstr("\"accuracy\": \"0.5\""));

  auto json_compact = io::JSONFormatter::create();
  json_compact->setMultiLine(false);
  EXPECT_THAT(h_to_g3d_full->exportToJSON(json_compact.get()),
              HasSubstr("\"accuracy\":\"0.5\""));

  // Non-empty name, null interpolationCRS, empty accuracies.
  auto json_named = io::JSONFormatter::create();
  const std::string json_named_str = gt->exportToJSON(json_named.get());
  EXPECT_THAT(json_named_str, HasSubstr("\"WKT Shift\""));
  EXPECT_THAT(json_named_str, Not(HasSubstr("\"interpolation_crs\"")));
  EXPECT_THAT(json_named_str, Not(HasSubstr("\"accuracy\"")));

  // BoundCRS with matching baseCRS vs mismatched baseCRS exercises
  // abridgedTransformationWriteSourceCRS() false and true branches.
  // BoundCRS::_exportToJSON calls setOmitTypeInImmediateChild(), so
  // "AbridgedTransformation" type key is suppressed inside BoundCRS.
  auto tf_from_4267 = operation::Transformation::createGeocentricTranslations(
      NameProps("NAD27 to WGS84")
          .set(metadata::Identifier::CODESPACE_KEY, metadata::Identifier::EPSG)
          .set(metadata::Identifier::CODE_KEY, "9991"),
      crs::GeographicCRS::EPSG_4267, crs::GeographicCRS::EPSG_4326, 1.0, 2.0,
      3.0, {});
  auto bound_matching =
      crs::BoundCRS::create(crs::GeographicCRS::EPSG_4267,
                            crs::GeographicCRS::EPSG_4326, tf_from_4267);
  auto json_bound_match = io::JSONFormatter::create();
  const std::string json_bound_match_str =
      bound_matching->exportToJSON(json_bound_match.get());
  EXPECT_THAT(json_bound_match_str, HasSubstr("\"transformation\""));
  EXPECT_THAT(json_bound_match_str,
              Not(HasSubstr("\"AbridgedTransformation\"")));
  EXPECT_EQ(1, CountOccurrences(json_bound_match_str, "\"source_crs\""));

  auto bound_mismatch =
      crs::BoundCRS::create(crs::GeographicCRS::EPSG_4269,
                            crs::GeographicCRS::EPSG_4326, tf_from_4267);
  auto json_bound_mismatch = io::JSONFormatter::create();
  const std::string json_bound_mismatch_str =
      bound_mismatch->exportToJSON(json_bound_mismatch.get());
  EXPECT_THAT(json_bound_mismatch_str, HasSubstr("\"transformation\""));
  EXPECT_EQ(2, CountOccurrences(json_bound_mismatch_str, "\"source_crs\""));

  // AbridgedTransformation with outputId() == true vs outputId() == false.
  auto json_abridged_with_id = io::JSONFormatter::create();
  json_abridged_with_id->setAbridgedTransformation(true);
  const std::string json_id_true =
      tf_from_4267->exportToJSON(json_abridged_with_id.get());
  EXPECT_THAT(json_id_true, HasSubstr("\"AbridgedTransformation\""));
  EXPECT_THAT(json_id_true, HasSubstr("9991"));
  EXPECT_THAT(json_id_true, Not(HasSubstr("\"accuracy\"")));

  auto json_abridged_nested = io::JSONFormatter::create();
  {
    auto outer_obj = json_abridged_nested->MakeObjectContext("Parent", true);
    json_abridged_nested->writer()->AddObjKey("transformation");
    json_abridged_nested->setAbridgedTransformation(true);
    tf_from_4267->_exportToJSON(json_abridged_nested.get());
    json_abridged_nested->setAbridgedTransformation(false);
  }
  EXPECT_THAT(json_abridged_nested->toString(),
              HasSubstr("\"AbridgedTransformation\""));
  EXPECT_THAT(json_abridged_nested->toString(), Not(HasSubstr("9991")));

  // 3. _exportToPROJString: PROJ_4 throws, supported PROJ_5 succeeds,
  //    unsupported PROJ_5 throws "Unimplemented ...".
  auto proj4_fmt = io::PROJStringFormatter::create(
      io::PROJStringFormatter::Convention::PROJ_4);
  EXPECT_THROW(gt->exportToPROJString(proj4_fmt.get()),
               io::FormattingException);

  auto proj5_geocen_fmt = io::PROJStringFormatter::create(
      io::PROJStringFormatter::Convention::PROJ_5);
  EXPECT_THAT(gt->exportToPROJString(proj5_geocen_fmt.get()),
              HasSubstr("+proj=helmert"));

  auto proj5_geog2d_fmt = io::PROJStringFormatter::create(
      io::PROJStringFormatter::Convention::PROJ_5);
  const std::string proj5_geog2d =
      tf_from_4267->exportToPROJString(proj5_geog2d_fmt.get());
  EXPECT_THAT(proj5_geog2d, HasSubstr("+proj=push"));
  EXPECT_THAT(proj5_geog2d, HasSubstr("+proj=helmert"));

  auto param_x = operation::createOpParamNameEPSGCode(
      EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION);
  auto unsupported = operation::Transformation::create(
      NameProps("Custom Unimplemented Transform"),
      crs::GeographicCRS::EPSG_4326, crs::GeographicCRS::EPSG_4269, nullptr,
      NameProps("Custom Unimplemented Method"),
      std::vector<operation::OperationParameterNNPtr>{param_x},
      std::vector<operation::ParameterValueNNPtr>{
          operation::ParameterValue::create(common::Length(1.0))},
      {});
  auto proj5_err_fmt = io::PROJStringFormatter::create(
      io::PROJStringFormatter::Convention::PROJ_5);
  EXPECT_THROW(unsupported->exportToPROJString(proj5_err_fmt.get()),
               io::FormattingException);
}

}  // namespace
}  // namespace proj
