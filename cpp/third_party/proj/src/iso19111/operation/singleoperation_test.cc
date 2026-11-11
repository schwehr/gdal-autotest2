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
// Unit tests for ISO-19111 SingleOperation, OperationParameter, ParameterValue,
// OperationMethod, PointMotionOperation, and grid-description lookups in
// proj/src/iso19111/operation/singleoperation.cpp. Note: in google3 default
// non-GCE builds, proj.db and non-skipped gridshift files are embedded.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/singleoperation.cpp

#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
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
#include "third_party/proj4/proj/src/iso19111/operation/coordinateoperation_private.hpp"
#include "third_party/proj4/proj/src/iso19111/operation/operationmethod_private.hpp"
#include "third_party/proj4/proj/src/iso19111/operation/oputils.hpp"
#include "third_party/proj4/proj/src/iso19111/operation/parammappings.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_constants.h"
#include "third_party/sqlite/sqlite3.h"

namespace proj {
namespace {

using ::osgeo::proj::common::Angle;
using ::osgeo::proj::common::DataEpoch;
using ::osgeo::proj::common::IdentifiedObject;
using ::osgeo::proj::common::Length;
using ::osgeo::proj::common::Measure;
using ::osgeo::proj::common::ObjectDomain;
using ::osgeo::proj::common::ObjectUsage;
using ::osgeo::proj::common::Scale;
using ::osgeo::proj::common::UnitOfMeasure;
using ::osgeo::proj::crs::BoundCRS;
using ::osgeo::proj::crs::CompoundCRS;
using ::osgeo::proj::crs::CRS;
using ::osgeo::proj::crs::CRSNNPtr;
using ::osgeo::proj::crs::CRSPtr;
using ::osgeo::proj::crs::EngineeringCRS;
using ::osgeo::proj::crs::GeodeticCRS;
using ::osgeo::proj::crs::GeodeticCRSNNPtr;
using ::osgeo::proj::crs::GeographicCRS;
using ::osgeo::proj::crs::GeographicCRSNNPtr;
using ::osgeo::proj::crs::ProjectedCRS;
using ::osgeo::proj::crs::ProjectedCRSNNPtr;
using ::osgeo::proj::crs::VerticalCRS;
using ::osgeo::proj::crs::VerticalCRSNNPtr;
using ::osgeo::proj::cs::AxisDirection;
using ::osgeo::proj::cs::CartesianCS;
using ::osgeo::proj::cs::CoordinateSystemAxis;
using ::osgeo::proj::cs::EllipsoidalCS;
using ::osgeo::proj::cs::Meridian;
using ::osgeo::proj::cs::SphericalCS;
using ::osgeo::proj::cs::VerticalCS;
using ::osgeo::proj::datum::DynamicGeodeticReferenceFrame;
using ::osgeo::proj::datum::DynamicVerticalReferenceFrame;
using ::osgeo::proj::datum::Ellipsoid;
using ::osgeo::proj::datum::EngineeringDatum;
using ::osgeo::proj::datum::GeodeticReferenceFrame;
using ::osgeo::proj::datum::PrimeMeridian;
using ::osgeo::proj::datum::RealizationMethod;
using ::osgeo::proj::datum::VerticalReferenceFrame;
using ::osgeo::proj::io::AuthorityFactory;
using ::osgeo::proj::io::DatabaseContext;
using ::osgeo::proj::io::DatabaseContextNNPtr;
using ::osgeo::proj::io::FormattingException;
using ::osgeo::proj::io::IWKTExportable;
using ::osgeo::proj::io::JSONFormatter;
using ::osgeo::proj::io::PROJStringFormatter;
using ::osgeo::proj::io::WKTFormatter;
using ::osgeo::proj::metadata::Citation;
using ::osgeo::proj::metadata::Extent;
using ::osgeo::proj::metadata::Identifier;
using ::osgeo::proj::metadata::PositionalAccuracy;
using ::osgeo::proj::operation::ConcatenatedOperation;
using ::osgeo::proj::operation::Conversion;
using ::osgeo::proj::operation::CoordinateOperation;
using ::osgeo::proj::operation::CoordinateOperationNNPtr;
using ::osgeo::proj::operation::GeneralOperationParameter;
using ::osgeo::proj::operation::GeneralOperationParameterNNPtr;
using ::osgeo::proj::operation::GeneralParameterValue;
using ::osgeo::proj::operation::GeneralParameterValueNNPtr;
using ::osgeo::proj::operation::GridDescription;
using ::osgeo::proj::operation::InvalidOperation;
using ::osgeo::proj::operation::InvalidOperationEmptyIntersection;
using ::osgeo::proj::operation::InverseCoordinateOperation;
using ::osgeo::proj::operation::OperationMethod;
using ::osgeo::proj::operation::OperationMethodNNPtr;
using ::osgeo::proj::operation::OperationParameter;
using ::osgeo::proj::operation::OperationParameterNNPtr;
using ::osgeo::proj::operation::OperationParameterValue;
using ::osgeo::proj::operation::OperationParameterValueNNPtr;
using ::osgeo::proj::operation::ParameterValue;
using ::osgeo::proj::operation::ParameterValueNNPtr;
using ::osgeo::proj::operation::PointMotionOperation;
using ::osgeo::proj::operation::PointMotionOperationNNPtr;
using ::osgeo::proj::operation::SingleOperation;
using ::osgeo::proj::operation::SingleOperationNNPtr;
using ::osgeo::proj::operation::Transformation;
using ::osgeo::proj::operation::TransformationNNPtr;
using ::osgeo::proj::util::IComparable;
using ::osgeo::proj::util::nn_dynamic_pointer_cast;
using ::osgeo::proj::util::optional;
using ::osgeo::proj::util::PropertyMap;
using ::osgeo::proj::util::UnsupportedOperationException;
using ::testing::HasSubstr;

PropertyMap NameProps(const std::string& name) {
  return PropertyMap().set(IdentifiedObject::NAME_KEY, name);
}

PropertyMap EpsgProps(const std::string& name, int code) {
  return PropertyMap()
      .set(IdentifiedObject::NAME_KEY, name)
      .set(Identifier::CODESPACE_KEY, Identifier::EPSG)
      .set(Identifier::CODE_KEY, code);
}

OperationParameterNNPtr MakeParam(const std::string& name, int epsg_code = 0) {
  if (epsg_code != 0) {
    return OperationParameter::create(EpsgProps(name, epsg_code));
  }
  return OperationParameter::create(NameProps(name));
}

OperationParameterValueNNPtr MakeMeasureParamVal(const std::string& name,
                                                 int epsg_code, double val,
                                                 const UnitOfMeasure& unit) {
  return OperationParameterValue::create(
      MakeParam(name, epsg_code), ParameterValue::create(Measure(val, unit)));
}

OperationParameterValueNNPtr MakeFileParamVal(const std::string& name,
                                              int epsg_code,
                                              const std::string& filename) {
  return OperationParameterValue::create(
      MakeParam(name, epsg_code), ParameterValue::createFilename(filename));
}

OperationParameterValueNNPtr MakeIntParamVal(const std::string& name,
                                             int epsg_code, int val) {
  return OperationParameterValue::create(MakeParam(name, epsg_code),
                                         ParameterValue::create(val));
}

OperationParameterValueNNPtr MakeStringParamVal(const std::string& name,
                                                int epsg_code,
                                                const std::string& val) {
  return OperationParameterValue::create(MakeParam(name, epsg_code),
                                         ParameterValue::create(val));
}

GeographicCRSNNPtr MakeGeog2D(const std::string& name = "WGS 84",
                              const UnitOfMeasure& unit = UnitOfMeasure::DEGREE,
                              double pm_lon = 0.0,
                              const std::string& pm_name = "Greenwich",
                              bool spherical = false) {
  auto pm = PrimeMeridian::create(NameProps(pm_name),
                                  Angle(pm_lon, UnitOfMeasure::DEGREE));
  auto ellps = spherical ? Ellipsoid::createSphere(
                               NameProps("Sphere"),
                               Length(6371000.0, UnitOfMeasure::METRE))
                         : Ellipsoid::WGS84;
  auto datum = GeodeticReferenceFrame::create(NameProps(name + " datum"), ellps,
                                              optional<std::string>(), pm);
  return GeographicCRS::create(NameProps(name), datum,
                               EllipsoidalCS::createLatitudeLongitude(unit));
}

GeographicCRSNNPtr MakeGeog3D(
    const std::string& name = "WGS 84 3D",
    const UnitOfMeasure& horiz_unit = UnitOfMeasure::DEGREE,
    const UnitOfMeasure& vert_unit = UnitOfMeasure::METRE) {
  return GeographicCRS::create(
      NameProps(name), GeodeticReferenceFrame::EPSG_6326,
      EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(horiz_unit,
                                                              vert_unit));
}

GeodeticCRSNNPtr MakeGeocentric(
    const std::string& name = "WGS 84 geocentric",
    const UnitOfMeasure& unit = UnitOfMeasure::METRE) {
  return GeodeticCRS::create(NameProps(name), GeodeticReferenceFrame::EPSG_6326,
                             CartesianCS::createGeocentric(unit));
}

GeographicCRSNNPtr MakeDynamicGeog3D(const std::string& name,
                                     double anchor_epoch) {
  auto dyn_datum = DynamicGeodeticReferenceFrame::create(
      NameProps(name + " datum")
          .set("ANCHOR_EPOCH", std::to_string(anchor_epoch)),
      Ellipsoid::WGS84, optional<std::string>(), PrimeMeridian::GREENWICH,
      Measure(anchor_epoch, UnitOfMeasure::YEAR), optional<std::string>());
  return GeographicCRS::create(
      NameProps(name), dyn_datum,
      EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
          UnitOfMeasure::DEGREE, UnitOfMeasure::METRE));
}

VerticalCRSNNPtr MakeVertCRS(const std::string& name = "VertCRS",
                             const UnitOfMeasure& unit = UnitOfMeasure::METRE,
                             bool depth = false) {
  auto datum = VerticalReferenceFrame::create(NameProps(name + " datum"),
                                              optional<std::string>(),
                                              optional<RealizationMethod>());
  auto cs = depth ? VerticalCS::create(
                        PropertyMap(),
                        CoordinateSystemAxis::create(NameProps("Depth"), "D",
                                                     AxisDirection::DOWN, unit))
                  : VerticalCS::createGravityRelatedHeight(unit);
  return VerticalCRS::create(NameProps(name), datum, cs);
}

VerticalCRSNNPtr MakeDynamicVertCRS(
    const std::string& name, double anchor_epoch,
    const UnitOfMeasure& unit = UnitOfMeasure::METRE) {
  auto dyn_datum = DynamicVerticalReferenceFrame::create(
      NameProps(name + " datum")
          .set("ANCHOR_EPOCH", std::to_string(anchor_epoch)),
      optional<std::string>(), optional<RealizationMethod>(),
      Measure(anchor_epoch, UnitOfMeasure::YEAR), optional<std::string>());
  return VerticalCRS::create(NameProps(name), dyn_datum,
                             VerticalCS::createGravityRelatedHeight(unit));
}

ProjectedCRSNNPtr MakeProjCRS(
    const std::string& name = "UTM 31N",
    const UnitOfMeasure& unit = UnitOfMeasure::METRE) {
  auto conv = Conversion::createUTM(PropertyMap(), 31, true);
  return ProjectedCRS::create(NameProps(name), GeographicCRS::EPSG_4326, conv,
                              CartesianCS::createEastingNorthing(unit));
}

TransformationNNPtr MakeTransf(
    const CRSNNPtr& src, const CRSNNPtr& dst, const CRSNNPtr* interp,
    const std::string& method_name, int method_code,
    const std::vector<OperationParameterValueNNPtr>& param_vals,
    const PropertyMap& op_props = NameProps("test_op")) {
  std::vector<GeneralParameterValueNNPtr> gvals;
  std::vector<OperationParameterNNPtr> params;
  params.reserve(param_vals.size());
  gvals.reserve(param_vals.size());
  for (const auto& pv : param_vals) {
    params.push_back(pv->parameter());
    gvals.push_back(pv);
  }
  auto method = OperationMethod::create(
      method_code != 0 ? EpsgProps(method_name, method_code)
                       : NameProps(method_name),
      params);
  return Transformation::create(op_props, src, dst,
                                interp ? interp->as_nullable() : nullptr,
                                method, gvals, {});
}

TransformationNNPtr MakeTransf(
    const CRSNNPtr& src, const CRSNNPtr& dst, std::nullptr_t,
    const std::string& method_name, int method_code,
    const std::vector<OperationParameterValueNNPtr>& param_vals,
    const PropertyMap& op_props = NameProps("test_op")) {
  return MakeTransf(src, dst, static_cast<const CRSNNPtr*>(nullptr),
                    method_name, method_code, param_vals, op_props);
}

TransformationNNPtr MakeTransf(
    const CRSNNPtr& src, const CRSNNPtr& dst, const GeographicCRSNNPtr* interp,
    const std::string& method_name, int method_code,
    const std::vector<OperationParameterValueNNPtr>& param_vals,
    const PropertyMap& op_props = NameProps("test_op")) {
  CRSNNPtr base_interp = interp ? static_cast<CRSNNPtr>(*interp) : src;
  return MakeTransf(
      src, dst, interp ? &base_interp : static_cast<const CRSNNPtr*>(nullptr),
      method_name, method_code, param_vals, op_props);
}

std::string ExportToPROJ(const CoordinateOperationNNPtr& op,
                         PROJStringFormatter::Convention conv =
                             PROJStringFormatter::Convention::PROJ_5) {
  auto fmt = PROJStringFormatter::create(conv, nullptr);
  return op->exportToPROJString(fmt.get());
}

// ---------------------------------------------------------------------------
// 1. Exceptions & GridDescription
// ---------------------------------------------------------------------------

TEST(SingleOperationTest, ExceptionsAndGridDescription) {
  InvalidOperation ex_cstr("cstr invalid op");
  EXPECT_STREQ("cstr invalid op", ex_cstr.what());
  InvalidOperation ex_copy(ex_cstr);
  EXPECT_STREQ("cstr invalid op", ex_copy.what());

  InvalidOperation ex_str(std::string("str invalid op"));
  EXPECT_STREQ("str invalid op", ex_str.what());

  InvalidOperationEmptyIntersection ex_empty("empty intersection");
  EXPECT_STREQ("empty intersection", ex_empty.what());
  InvalidOperationEmptyIntersection ex_empty_copy(ex_empty);
  EXPECT_STREQ("empty intersection", ex_empty_copy.what());

  GridDescription g1;
  g1.shortName = "a_grid.tif";
  g1.fullName = "/mem/a_grid.tif";
  g1.packageName = "pkg";
  g1.url = "https://example.com/a_grid.tif";
  g1.directDownload = true;
  g1.openLicense = true;
  g1.available = false;

  GridDescription g2(g1);
  g2.shortName = "b_grid.tif";
  EXPECT_LT(g1, g2);
  EXPECT_FALSE(g2 < g1);

  GridDescription g3(std::move(g2));
  EXPECT_EQ("b_grid.tif", g3.shortName);
}

// ---------------------------------------------------------------------------
// 2. CoordinateOperation & CoordinateTransformer
// ---------------------------------------------------------------------------

// Helper subclass of OperationMethod to exercise protected copy ctor.
class CopyableOperationMethod final : public OperationMethod {
 public:
  explicit CopyableOperationMethod(const OperationMethod& other)
      : OperationMethod(other) {}
};

struct CoordOpPeer : public CoordinateOperation {
  static void SetHasBallpark(CoordinateOperation& op, bool b) {
    (op.*(&CoordOpPeer::setHasBallparkTransformation))(b);
  }
  static void SetRequiresTime(CoordinateOperation& op, bool b) {
    (op.*(&CoordOpPeer::setRequiresPerCoordinateInputTime))(b);
  }
  static void SetSourceEpoch(CoordinateOperation& op, const DataEpoch& ep) {
    (op.*(&CoordOpPeer::setSourceCoordinateEpoch))(optional<DataEpoch>(ep));
  }
  static void SetTargetEpoch(CoordinateOperation& op, const DataEpoch& ep) {
    (op.*(&CoordOpPeer::setTargetCoordinateEpoch))(optional<DataEpoch>(ep));
  }
  static void SetWeakCRSs(CoordinateOperation& op, std::weak_ptr<CRS> src,
                          std::weak_ptr<CRS> dst) {
    (op.*(&CoordOpPeer::setWeakSourceTargetCRS))(std::move(src),
                                                 std::move(dst));
  }
  static void SetInterpCRS(CoordinateOperation& op, const CRSPtr& interp) {
    (op.*(&CoordOpPeer::setInterpolationCRS))(interp);
  }
  static void SetCRSsUpdateInverse(CoordinateOperation& op, const CRSNNPtr& src,
                                   const CRSNNPtr& dst,
                                   const CRSPtr& interp = nullptr) {
    (op.*(&CoordOpPeer::setCRSsUpdateInverse))(src, dst, interp);
  }
  static void SetCRSsFromOp(CoordinateOperation& op,
                            const CoordinateOperation* in,
                            bool inverse_source_target) {
    void (CoordinateOperation::*fn)(const CoordinateOperation*, bool) =
        &CoordOpPeer::setCRSs;
    (op.*fn)(in, inverse_source_target);
  }
};

TEST(SingleOperationTest, CoordinateOperationAccessorsAndSetters) {
  ScopedContext ctx = MakeContext();
  auto src = GeographicCRS::EPSG_4326;
  auto dst = GeographicCRS::EPSG_4269;
  auto interp = GeographicCRS::create(
      EpsgProps("ETRS89", 4258), GeodeticReferenceFrame::EPSG_6326,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));

  auto acc = PositionalAccuracy::create("1.5");
  auto op_props = PropertyMap()
                      .set(IdentifiedObject::NAME_KEY, "Helmert op")
                      .set(CoordinateOperation::OPERATION_VERSION_KEY, "v2.0");
  auto method_props =
      EpsgProps(EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
                EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D);
  std::vector<OperationParameterNNPtr> params{
      MakeParam(EPSG_NAME_PARAMETER_X_AXIS_TRANSLATION,
                EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION),
      MakeParam(EPSG_NAME_PARAMETER_Y_AXIS_TRANSLATION,
                EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION),
      MakeParam(EPSG_NAME_PARAMETER_Z_AXIS_TRANSLATION,
                EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION),
  };
  std::vector<ParameterValueNNPtr> vals{
      ParameterValue::create(Measure(1.0, UnitOfMeasure::METRE)),
      ParameterValue::create(Measure(2.0, UnitOfMeasure::METRE)),
      ParameterValue::create(Measure(3.0, UnitOfMeasure::METRE)),
  };
  auto tr = Transformation::create(op_props, src, dst, interp.as_nullable(),
                                   method_props, params, vals, {acc});

  ASSERT_TRUE(tr->operationVersion().has_value());
  EXPECT_EQ("v2.0", *tr->operationVersion());
  ASSERT_EQ(1u, tr->coordinateOperationAccuracies().size());
  EXPECT_EQ("1.5", tr->coordinateOperationAccuracies()[0]->value());
  EXPECT_EQ(src.get(), tr->sourceCRS().get());
  EXPECT_EQ(dst.get(), tr->targetCRS().get());
  EXPECT_EQ(interp.get(), tr->interpolationCRS().get());

  EXPECT_FALSE(tr->hasBallparkTransformation());
  CoordOpPeer::SetHasBallpark(*tr, true);
  EXPECT_TRUE(tr->hasBallparkTransformation());

  EXPECT_FALSE(tr->requiresPerCoordinateInputTime());
  CoordOpPeer::SetRequiresTime(*tr, true);
  EXPECT_TRUE(tr->requiresPerCoordinateInputTime());

  DataEpoch ep_src(Measure(2010.0, UnitOfMeasure::YEAR));
  DataEpoch ep_dst(Measure(2020.0, UnitOfMeasure::YEAR));
  CoordOpPeer::SetSourceEpoch(*tr, ep_src);
  CoordOpPeer::SetTargetEpoch(*tr, ep_dst);
  ASSERT_TRUE(tr->sourceCoordinateEpoch().has_value());
  ASSERT_TRUE(tr->targetCoordinateEpoch().has_value());
  EXPECT_DOUBLE_EQ(2010.0,
                   tr->sourceCoordinateEpoch()->coordinateEpoch().value());
  // Because sourceCoordinateEpoch is now set, requiresPerCoordinateInputTime()
  // returns false!
  EXPECT_FALSE(tr->requiresPerCoordinateInputTime());

  // Exercise setWeakSourceTargetCRS, setInterpolationCRS, setCRSsUpdateInverse,
  // setCRSs(const CoordinateOperation*, bool).
  CoordOpPeer::SetWeakCRSs(*tr, src.as_nullable(), dst.as_nullable());
  EXPECT_EQ(src.get(), tr->sourceCRS().get());
  EXPECT_EQ(dst.get(), tr->targetCRS().get());

  CoordOpPeer::SetInterpCRS(*tr, nullptr);
  EXPECT_EQ(nullptr, tr->interpolationCRS().get());
  CoordOpPeer::SetInterpCRS(*tr, interp.as_nullable());
  EXPECT_EQ(interp.get(), tr->interpolationCRS().get());

  // Exercise InverseCoordinateOperation epoch/CRS forwarding.
  auto inv_op = tr->inverse();
  CoordOpPeer::SetSourceEpoch(*inv_op, ep_src);
  CoordOpPeer::SetTargetEpoch(*inv_op, ep_dst);
  CoordOpPeer::SetCRSsUpdateInverse(*inv_op, dst, src);
  EXPECT_EQ(dst.get(), inv_op->sourceCRS().get());
  EXPECT_EQ(src.get(), inv_op->targetCRS().get());

  // Exercise setCRSs(const CoordinateOperation*, bool) in both directions.
  auto tr2 = Transformation::create(op_props, dst, src, nullptr, method_props,
                                    params, vals, {});
  CoordOpPeer::SetCRSsFromOp(*tr2, tr.get(), /*inverse_source_target=*/false);
  EXPECT_EQ(src.get(), tr2->sourceCRS().get());
  EXPECT_EQ(dst.get(), tr2->targetCRS().get());
  EXPECT_EQ(interp.get(), tr2->interpolationCRS().get());

  CoordOpPeer::SetCRSsFromOp(*tr2, tr.get(), /*inverse_source_target=*/true);
  EXPECT_EQ(dst.get(), tr2->sourceCRS().get());
  EXPECT_EQ(src.get(), tr2->targetCRS().get());

  // Exercise setCRSsUpdateInverse on ConcatenatedOperation.
  auto concat = ConcatenatedOperation::create(NameProps("concat"),
                                              {tr, tr->inverse()}, {});
  CoordOpPeer::SetCRSsUpdateInverse(*concat, src, src);
  EXPECT_EQ(src.get(), concat->sourceCRS().get());
  EXPECT_EQ(src.get(), concat->targetCRS().get());

  // Exercise shallowClone on CoordinateOperation.
  auto cloned = tr->shallowClone();
  EXPECT_EQ(tr->nameStr(), cloned->nameStr());
}

TEST(SingleOperationTest, IsPROJInstantiableAndCoordinateTransformer) {
  ScopedContext ctx = MakeContext();
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());

  // 1. Valid operation without grids.
  auto utm = Conversion::createUTM(PropertyMap(), 31, true);
  EXPECT_TRUE(utm->isPROJInstantiable(nullptr, true));
  EXPECT_TRUE(utm->isPROJInstantiable(db_ctx.as_nullable(), false));

  auto transformer = utm->coordinateTransformer(ctx.get());
  PJ_COORD in_coord =
      proj_coord(2.0 * std::numbers::pi / 180.0,
                 49.0 * std::numbers::pi / 180.0, 0.0, HUGE_VAL);
  PJ_COORD out_coord = transformer->transform(in_coord);
  EXPECT_GT(out_coord.xy.x, 400000.0);
  EXPECT_GT(out_coord.xy.y, 5000000.0);

  // 2. Uninstantiable operation (unknown method).
  auto bad_method = OperationMethod::create(
      NameProps("NonExistentMethod"), std::vector<OperationParameterNNPtr>{});
  auto bad_conv = Conversion::create(NameProps("bad"), bad_method,
                                     std::vector<GeneralParameterValueNNPtr>{});
  EXPECT_FALSE(bad_conv->isPROJInstantiable(nullptr, true));
  auto bad_tr = bad_conv->coordinateTransformer(ctx.get());
  PJ_COORD err_out = bad_tr->transform(in_coord);
  EXPECT_TRUE(std::isinf(err_out.v[0]));
  auto default_ctx_tr = utm->coordinateTransformer(nullptr);
  EXPECT_GT(default_ctx_tr->transform(in_coord).xy.x, 400000.0);

  // 3. Operation requiring a missing grid vs @optional grid.
  auto missing_grid_tr = MakeTransf(
      GeographicCRS::EPSG_4267, GeographicCRS::EPSG_4269, nullptr,
      EPSG_NAME_METHOD_NTV2, EPSG_CODE_METHOD_NTV2,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "nonexistent_grid_xyz.gsb")});
  EXPECT_FALSE(missing_grid_tr->isPROJInstantiable(nullptr, false));

  auto optional_grid_tr = SingleOperation::createPROJBased(
      NameProps("opt_grid"), "+proj=hgridshift +grids=@nonexistent_opt.gsb",
      GeographicCRS::EPSG_4267.as_nullable(),
      GeographicCRS::EPSG_4269.as_nullable(), {});
  EXPECT_TRUE(optional_grid_tr->isPROJInstantiable(nullptr, false));
}

TEST(SingleOperationTest, NormalizeForVisualization) {
  ScopedContext ctx = MakeContext();
  // 1. Operation without source/target CRS throws
  // UnsupportedOperationException.
  auto utm = Conversion::createUTM(PropertyMap(), 31, true);
  EXPECT_THROW(utm->normalizeForVisualization(), UnsupportedOperationException);

  // 2. Operation where neither source nor target needs axis swapping
  // (OGC:CRS84 lon/lat to UTM Easting/Northing).
  auto crs84 = GeographicCRS::OGC_CRS84;
  auto proj_crs = MakeProjCRS();
  auto op_no_swap = SingleOperation::createPROJBased(
      NameProps("crs84_to_utm"), "+proj=utm +zone=31 +ellps=WGS84",
      crs84.as_nullable(), proj_crs.as_nullable(), {});
  auto norm_same = op_no_swap->normalizeForVisualization();
  EXPECT_EQ(op_no_swap.get(), norm_same.get());

  // 3. Operation where both source (EPSG:4326 lat/lon) and target (EPSG:4269
  // lat/lon) need axis swapping, with domains, remarks, identifiers.
  auto etrs89 = GeographicCRS::create(
      EpsgProps("ETRS89", 4258), GeodeticReferenceFrame::EPSG_6326,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  auto props = PropertyMap()
                   .set(IdentifiedObject::NAME_KEY, "WGS84 to NAD83")
                   .set(IdentifiedObject::REMARKS_KEY, "some remark")
                   .set(Identifier::CODESPACE_KEY, "TEST")
                   .set(Identifier::CODE_KEY, "123");
  auto op_swap = SingleOperation::createPROJBased(
      props,
      "+proj=pipeline +step +proj=axisswap +order=2,1 +step +proj=noop "
      "+step +proj=axisswap +order=2,1",
      GeographicCRS::EPSG_4326.as_nullable(),
      GeographicCRS::EPSG_4269.as_nullable(), {});
  auto norm_swapped = op_swap->normalizeForVisualization();
  EXPECT_NE(op_swap.get(), norm_swapped.get());
  EXPECT_THAT(norm_swapped->nameStr(),
              HasSubstr("with axis order normalized for visualization"));
  auto proj_str = ExportToPROJ(norm_swapped);
  EXPECT_FALSE(proj_str.empty());

  // 4. Normalize a ConcatenatedOperation to exercise subOp flattening loop.
  auto step1 = SingleOperation::createPROJBased(
      NameProps("step1"), "+proj=noop", GeographicCRS::EPSG_4326.as_nullable(),
      etrs89.as_nullable(), {});
  auto step2 = SingleOperation::createPROJBased(
      NameProps("step2"), "+proj=noop", etrs89.as_nullable(),
      GeographicCRS::EPSG_4269.as_nullable(), {});
  auto concat_props = PropertyMap()
                          .set(IdentifiedObject::NAME_KEY, "concat_swap")
                          .set(ObjectUsage::SCOPE_KEY, "test scope")
                          .set(ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
                               Extent::createFromBBOX(-180, -90, 180, 90));
  auto concat = ConcatenatedOperation::create(concat_props, {step1, step2}, {});
  auto norm_concat = concat->normalizeForVisualization();
  EXPECT_NE(concat.get(), norm_concat.get());
}

// ---------------------------------------------------------------------------
// 3. OperationMethod, OperationParameter, OperationParameterValue
// ---------------------------------------------------------------------------

TEST(SingleOperationTest, OperationMethodCreationWKTJSONAndEquivalence) {
  auto param1 = MakeParam(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                          EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN);
  auto param2 = MakeParam(EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
                          EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN);

  auto props =
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "Transverse Mercator")
          .set(Identifier::CODESPACE_KEY, Identifier::EPSG)
          .set(Identifier::CODE_KEY, EPSG_CODE_METHOD_TRANSVERSE_MERCATOR)
          .set("proj_method", "tmerc");
  auto method1 = OperationMethod::create(
      props, std::vector<GeneralOperationParameterNNPtr>{param1, param2});
  EXPECT_FALSE(method1->formula().has_value());
  EXPECT_FALSE(method1->formulaCitation().has_value());
  EXPECT_EQ(2u, method1->parameters().size());
  EXPECT_EQ(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR, method1->getEPSGCode());

  // Copy constructor via CopyableOperationMethod.
  CopyableOperationMethod copied_method(*method1);
  EXPECT_EQ(method1->nameStr(), copied_method.nameStr());

  // Create with (3D) suffix to exercise getEPSGCode() stripping " (3D)".
  auto props_cit =
      PropertyMap().set(IdentifiedObject::NAME_KEY, "Transverse Mercator (3D)");
  auto method_3d = OperationMethod::create(
      props_cit, std::vector<OperationParameterNNPtr>{param2, param1});
  EXPECT_FALSE(method_3d->formulaCitation().has_value());
  EXPECT_EQ(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR, method_3d->getEPSGCode());

  auto method_unknown = OperationMethod::create(
      NameProps("My Unmapped Method"), std::vector<OperationParameterNNPtr>{});
  EXPECT_EQ(0, method_unknown->getEPSGCode());

  // WKT2 and WKT1 export.
  auto wkt2_fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
  method1->_exportToWKT(wkt2_fmt.get());
  EXPECT_THAT(wkt2_fmt->toString(),
              HasSubstr("METHOD[\"Transverse Mercator\""));

  auto wkt1_fmt1 = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
  method1->_exportToWKT(wkt1_fmt1.get());
  EXPECT_THAT(wkt1_fmt1->toString(),
              HasSubstr("PROJECTION[\"Transverse_Mercator\""));
  EXPECT_THAT(wkt1_fmt1->toString(), HasSubstr("AUTHORITY[\"EPSG\",\"9807\"]"));

  auto wkt1_fmt2 = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
  method_unknown->_exportToWKT(wkt1_fmt2.get());
  EXPECT_THAT(wkt1_fmt2->toString(),
              HasSubstr("PROJECTION[\"My_Unmapped_Method\"]"));

  auto method_geos = OperationMethod::create(
      NameProps(PROJ_WKT2_NAME_METHOD_GEOSTATIONARY_SATELLITE_SWEEP_X),
      std::vector<OperationParameterNNPtr>{});
  auto wkt1_fmt3 = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
  method_geos->_exportToWKT(wkt1_fmt3.get());
  EXPECT_THAT(wkt1_fmt3->toString(),
              HasSubstr("PROJECTION[\"Geostationary_Satellite\"]"));

  auto method_no_wkt1 = OperationMethod::create(
      EpsgProps(EPSG_NAME_METHOD_EQUAL_EARTH, EPSG_CODE_METHOD_EQUAL_EARTH),
      std::vector<OperationParameterNNPtr>{});
  auto wkt1_fmt4 = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THROW(method_no_wkt1->_exportToWKT(wkt1_fmt4.get()),
               FormattingException);

  // JSON export.
  auto json_fmt = JSONFormatter::create();
  method1->_exportToJSON(json_fmt.get());
  EXPECT_THAT(json_fmt->toString(), HasSubstr("\"OperationMethod\""));

  // Equivalence: STRICT vs EQUIVALENT (permuted parameters).
  auto method_reordered = OperationMethod::create(
      NameProps("Transverse_Mercator"),
      std::vector<OperationParameterNNPtr>{param2, param1});
  EXPECT_FALSE(method1->isEquivalentTo(method_reordered.get(),
                                       IComparable::Criterion::STRICT));
  EXPECT_TRUE(method1->isEquivalentTo(method_reordered.get(),
                                      IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(method1->isEquivalentTo(method_unknown.get(),
                                       IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(method1->isEquivalentTo(param1.get(),
                                       IComparable::Criterion::EQUIVALENT));

  auto param3 = MakeParam(EPSG_NAME_PARAMETER_FALSE_EASTING,
                          EPSG_CODE_PARAMETER_FALSE_EASTING);
  auto method_mismatch_param = OperationMethod::create(
      NameProps("Transverse Mercator"),
      std::vector<OperationParameterNNPtr>{param1, param3});
  EXPECT_FALSE(method1->isEquivalentTo(method_mismatch_param.get(),
                                       IComparable::Criterion::EQUIVALENT));
}

TEST(SingleOperationTest, OperationParameterAndValueMethods) {
  // OperationParameter.
  auto p1 = MakeParam(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                      EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN);
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, p1->getEPSGCode());

  auto p_alt_name = MakeParam("Custom Lat Name",
                              EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN);
  EXPECT_FALSE(
      p1->isEquivalentTo(p_alt_name.get(), IComparable::Criterion::STRICT));
  EXPECT_TRUE(
      p1->isEquivalentTo(p_alt_name.get(), IComparable::Criterion::EQUIVALENT));

  auto p_lat_orig = MakeParam("Latitude of origin", 0);
  EXPECT_EQ(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
            p_lat_orig->getEPSGCode());
  auto p_scale = MakeParam("Scale factor", 0);
  EXPECT_EQ(EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN,
            p_scale->getEPSGCode());
  auto p_by_wkt2_name = MakeParam(EPSG_NAME_PARAMETER_FALSE_EASTING, 0);
  EXPECT_EQ(EPSG_CODE_PARAMETER_FALSE_EASTING, p_by_wkt2_name->getEPSGCode());
  auto p_unknown = MakeParam("Unknown Parameter XYZ", 0);
  EXPECT_EQ(0, p_unknown->getEPSGCode());

  EXPECT_NE(nullptr, OperationParameter::getNameForEPSGCode(
                         EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN));
  EXPECT_EQ(nullptr, OperationParameter::getNameForEPSGCode(0));
  EXPECT_EQ(nullptr, OperationParameter::getNameForEPSGCode(999999));

  // _exportToWKT on OperationParameter is a no-op.
  auto wkt_fmt = WKTFormatter::create();
  static_cast<const IWKTExportable*>(p1.get())->_exportToWKT(wkt_fmt.get());

  // OperationParameterValue creation, WKT & JSON export.
  auto pv_measure = MakeMeasureParamVal(EPSG_NAME_PARAMETER_FALSE_EASTING,
                                        EPSG_CODE_PARAMETER_FALSE_EASTING,
                                        500000.0, UnitOfMeasure::METRE);
  EXPECT_EQ(EPSG_NAME_PARAMETER_FALSE_EASTING,
            pv_measure->parameter()->nameStr());

  auto pv_deg =
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                          EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, 49.0,
                          UnitOfMeasure::DEGREE);
  auto pv_scale =
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN,
                          EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN,
                          0.9996, UnitOfMeasure::SCALE_UNITY);
  auto pv_arcsec = MakeMeasureParamVal(EPSG_NAME_PARAMETER_X_AXIS_ROTATION,
                                       EPSG_CODE_PARAMETER_X_AXIS_ROTATION, 1.5,
                                       UnitOfMeasure::ARC_SECOND);
  auto pv_file = MakeFileParamVal(
      EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
      EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE, "grid.gsb");
  auto pv_int = MakeIntParamVal(
      EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
      EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS, 4326);
  auto pv_str = MakeStringParamVal("Custom str param", 0, "hello");

  auto wkt2 = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
  pv_measure->_exportToWKT(wkt2.get());
  pv_file->_exportToWKT(wkt2.get());
  pv_int->_exportToWKT(wkt2.get());
  pv_str->_exportToWKT(wkt2.get());
  EXPECT_THAT(wkt2->toString(), HasSubstr("PARAMETER[\"False easting\""));
  EXPECT_THAT(
      wkt2->toString(),
      HasSubstr("PARAMETERFILE[\"Latitude and longitude difference file\""));

  auto json_fmt = JSONFormatter::create();
  pv_measure->_exportToJSON(json_fmt.get());
  pv_deg->_exportToJSON(json_fmt.get());
  pv_scale->_exportToJSON(json_fmt.get());
  pv_arcsec->_exportToJSON(json_fmt.get());
  pv_file->_exportToJSON(json_fmt.get());
  pv_int->_exportToJSON(json_fmt.get());
  pv_str->_exportToJSON(json_fmt.get());
  EXPECT_THAT(json_fmt->toString(), HasSubstr("\"ParameterValue\""));

  // convertFromAbridged: all 7 parameters by name and by EPSG code, plus
  // unknown.
  struct AbridgedCase {
    const char* name;
    int code;
    double in_val;
    double expected_val;
    UnitOfMeasure expected_unit;
  };
  const AbridgedCase cases[] = {
      {EPSG_NAME_PARAMETER_X_AXIS_TRANSLATION,
       EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION, 10.0, 10.0,
       UnitOfMeasure::METRE},
      {EPSG_NAME_PARAMETER_Y_AXIS_TRANSLATION,
       EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION, 20.0, 20.0,
       UnitOfMeasure::METRE},
      {EPSG_NAME_PARAMETER_Z_AXIS_TRANSLATION,
       EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION, 30.0, 30.0,
       UnitOfMeasure::METRE},
      {EPSG_NAME_PARAMETER_X_AXIS_ROTATION, EPSG_CODE_PARAMETER_X_AXIS_ROTATION,
       1.0, 1.0, UnitOfMeasure::ARC_SECOND},
      {EPSG_NAME_PARAMETER_Y_AXIS_ROTATION, EPSG_CODE_PARAMETER_Y_AXIS_ROTATION,
       2.0, 2.0, UnitOfMeasure::ARC_SECOND},
      {EPSG_NAME_PARAMETER_Z_AXIS_ROTATION, EPSG_CODE_PARAMETER_Z_AXIS_ROTATION,
       3.0, 3.0, UnitOfMeasure::ARC_SECOND},
      {EPSG_NAME_PARAMETER_SCALE_DIFFERENCE,
       EPSG_CODE_PARAMETER_SCALE_DIFFERENCE, 1.000005, 5.0,
       UnitOfMeasure::PARTS_PER_MILLION},
  };
  for (const auto& c : cases) {
    double val_by_name = c.in_val;
    const UnitOfMeasure* unit_by_name = nullptr;
    int code_by_name = 0;
    EXPECT_TRUE(OperationParameterValue::convertFromAbridged(
        c.name, val_by_name, unit_by_name, code_by_name));
    ASSERT_NE(nullptr, unit_by_name);
    EXPECT_NEAR(c.expected_val, val_by_name, 1e-6);
    EXPECT_EQ(c.expected_unit, *unit_by_name);

    double val_by_code = c.in_val;
    const UnitOfMeasure* unit_by_code = nullptr;
    int code_in = c.code;
    EXPECT_TRUE(OperationParameterValue::convertFromAbridged(
        "", val_by_code, unit_by_code, code_in));
    ASSERT_NE(nullptr, unit_by_code);
    EXPECT_NEAR(c.expected_val, val_by_code, 1e-6);
    EXPECT_EQ(c.expected_unit, *unit_by_code);
  }
  double dummy_val = 1.0;
  const UnitOfMeasure* dummy_unit = nullptr;
  int dummy_code = 99999;
  EXPECT_FALSE(OperationParameterValue::convertFromAbridged(
      "Unknown param", dummy_val, dummy_unit, dummy_code));

  // Equivalence of azimuth/angle parameters modulo 360 degrees.
  auto az1 = MakeMeasureParamVal(EPSG_NAME_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
                                 EPSG_CODE_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
                                 -30.0, UnitOfMeasure::DEGREE);
  auto az2 = MakeMeasureParamVal(EPSG_NAME_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
                                 EPSG_CODE_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
                                 330.0, UnitOfMeasure::DEGREE);
  auto az3 = MakeMeasureParamVal(EPSG_NAME_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
                                 EPSG_CODE_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
                                 45.0, UnitOfMeasure::DEGREE);
  EXPECT_FALSE(az1->isEquivalentTo(az2.get(), IComparable::Criterion::STRICT));
  EXPECT_TRUE(
      az1->isEquivalentTo(az2.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(
      az1->isEquivalentTo(az3.get(), IComparable::Criterion::EQUIVALENT));
}

// ---------------------------------------------------------------------------
// 4. SingleOperation Lookups, Equivalence, Validation & Grid Helpers
// ---------------------------------------------------------------------------

TEST(SingleOperationTest, ParameterValueAccessorsAndEquivalence) {
  auto utm = Conversion::createUTM(PropertyMap(), 31, true);
  EXPECT_EQ(5u, utm->parameterValues().size());

  EXPECT_NE(nullptr,
            utm->parameterValue(EPSG_CODE_PARAMETER_FALSE_EASTING).get());
  EXPECT_NE(nullptr,
            utm->parameterValue(EPSG_NAME_PARAMETER_FALSE_EASTING, 0).get());
  // Lookup using equivalent parameter name (e.g. Latitude of false origin vs
  // Latitude of natural origin).
  EXPECT_NE(nullptr, utm->parameterValue("Latitude of false origin", 0).get());
  EXPECT_EQ(nullptr, utm->parameterValue("Nonexistent parameter", 0).get());

  const Measure& fe_measure =
      utm->parameterValueMeasure(EPSG_CODE_PARAMETER_FALSE_EASTING);
  EXPECT_DOUBLE_EQ(500000.0, fe_measure.value());
  EXPECT_DOUBLE_EQ(0.0, utm->parameterValueMeasure(999999).value());
  EXPECT_DOUBLE_EQ(
      500000.0,
      utm->parameterValueMeasure(EPSG_NAME_PARAMETER_FALSE_EASTING, 0).value());

  EXPECT_DOUBLE_EQ(500000.0, utm->parameterValueNumericAsSI(
                                 EPSG_CODE_PARAMETER_FALSE_EASTING));
  EXPECT_DOUBLE_EQ(0.0, utm->parameterValueNumericAsSI(999999));

  EXPECT_DOUBLE_EQ(500000.0,
                   utm->parameterValueNumeric(EPSG_CODE_PARAMETER_FALSE_EASTING,
                                              UnitOfMeasure::METRE));
  EXPECT_DOUBLE_EQ(0.0,
                   utm->parameterValueNumeric(999999, UnitOfMeasure::METRE));
  EXPECT_DOUBLE_EQ(500000.0,
                   utm->parameterValueNumeric(EPSG_NAME_PARAMETER_FALSE_EASTING,
                                              UnitOfMeasure::METRE));
  EXPECT_DOUBLE_EQ(
      0.0, utm->parameterValueNumeric("Nonexistent", UnitOfMeasure::METRE));
}

TEST(SingleOperationTest, SingleOperationEquivalenceBranches) {
  // 1. Spherical vs Ellipsoidal method equivalence when CRSs are spherical.
  auto sph_crs = MakeGeog2D("Sphere CRS", UnitOfMeasure::DEGREE, 0.0,
                            "Greenwich", /*spherical=*/true);
  auto dst_crs = MakeProjCRS();

  struct SphCase {
    const char* ellps_name;
    int ellps_code;
    const char* sph_name;
    int sph_code;
  };
  const SphCase sph_cases[] = {
      {EPSG_NAME_METHOD_LAMBERT_AZIMUTHAL_EQUAL_AREA,
       EPSG_CODE_METHOD_LAMBERT_AZIMUTHAL_EQUAL_AREA,
       EPSG_NAME_METHOD_LAMBERT_AZIMUTHAL_EQUAL_AREA_SPHERICAL,
       EPSG_CODE_METHOD_LAMBERT_AZIMUTHAL_EQUAL_AREA_SPHERICAL},
      {EPSG_NAME_METHOD_LAMBERT_CYLINDRICAL_EQUAL_AREA,
       EPSG_CODE_METHOD_LAMBERT_CYLINDRICAL_EQUAL_AREA,
       EPSG_NAME_METHOD_LAMBERT_CYLINDRICAL_EQUAL_AREA_SPHERICAL,
       EPSG_CODE_METHOD_LAMBERT_CYLINDRICAL_EQUAL_AREA_SPHERICAL},
      {EPSG_NAME_METHOD_EQUIDISTANT_CYLINDRICAL,
       EPSG_CODE_METHOD_EQUIDISTANT_CYLINDRICAL,
       EPSG_NAME_METHOD_EQUIDISTANT_CYLINDRICAL_SPHERICAL,
       EPSG_CODE_METHOD_EQUIDISTANT_CYLINDRICAL_SPHERICAL},
  };
  for (const auto& sc : sph_cases) {
    auto op_ellps =
        MakeTransf(sph_crs, dst_crs, nullptr, sc.ellps_name, sc.ellps_code, {});
    auto op_sph =
        MakeTransf(sph_crs, dst_crs, nullptr, sc.sph_name, sc.sph_code, {});
    EXPECT_TRUE(op_ellps->isEquivalentTo(op_sph.get(),
                                         IComparable::Criterion::EQUIVALENT));
    EXPECT_TRUE(op_sph->isEquivalentTo(op_ellps.get(),
                                       IComparable::Criterion::EQUIVALENT));
  }

  // 2. TOWGS84 transformations equivalence (3-param vs 7-param PV vs CF).
  auto g1 = GeographicCRS::EPSG_4326;
  auto g2 = GeographicCRS::EPSG_4269;
  auto towgs_3 = Transformation::createGeocentricTranslations(
      NameProps("t1"), g1, g2, 10.0, 20.0, 30.0, {});
  auto towgs_7pv = Transformation::createPositionVector(
      NameProps("t1"), g1, g2, 10.0, 20.0, 30.0, 0.0, 0.0, 0.0, 0.0, {});
  auto towgs_7cf_diff = Transformation::createCoordinateFrameRotation(
      NameProps("t1"), g1, g2, 10.0, 20.0, 30.0, 1.0, 2.0, 3.0, 4.0, {});
  EXPECT_TRUE(towgs_3->isEquivalentTo(towgs_7pv.get(),
                                      IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(towgs_3->isEquivalentTo(towgs_7cf_diff.get(),
                                       IComparable::Criterion::EQUIVALENT));

  // 3. LCC 1SP vs 2SP and swapped standard parallels.
  auto cart_cs = CartesianCS::createEastingNorthing(UnitOfMeasure::METRE);
  auto lcc_2sp_a = ProjectedCRS::create(
                       NameProps("p"), g1,
                       Conversion::createLambertConicConformal_2SP(
                           NameProps("lcc"), Angle(45.0, UnitOfMeasure::DEGREE),
                           Angle(3.0, UnitOfMeasure::DEGREE),
                           Angle(45.0, UnitOfMeasure::DEGREE),
                           Angle(45.0, UnitOfMeasure::DEGREE),
                           Length(0.0, UnitOfMeasure::METRE),
                           Length(0.0, UnitOfMeasure::METRE)),
                       cart_cs)
                       ->derivingConversion();
  auto lcc_1sp = ProjectedCRS::create(
                     NameProps("p"), g1,
                     Conversion::createLambertConicConformal_1SP(
                         NameProps("lcc"), Angle(45.0, UnitOfMeasure::DEGREE),
                         Angle(3.0, UnitOfMeasure::DEGREE), Scale(1.0),
                         Length(0.0, UnitOfMeasure::METRE),
                         Length(0.0, UnitOfMeasure::METRE)),
                     cart_cs)
                     ->derivingConversion();
  EXPECT_TRUE(lcc_2sp_a->isEquivalentTo(lcc_1sp.get(),
                                        IComparable::Criterion::EQUIVALENT));
  EXPECT_TRUE(lcc_1sp->isEquivalentTo(lcc_2sp_a.get(),
                                      IComparable::Criterion::EQUIVALENT));

  auto lcc_2sp_p12 = Conversion::createLambertConicConformal_2SP(
      NameProps("lcc"), Angle(45.0, UnitOfMeasure::DEGREE),
      Angle(3.0, UnitOfMeasure::DEGREE), Angle(44.0, UnitOfMeasure::DEGREE),
      Angle(49.0, UnitOfMeasure::DEGREE), Length(0.0, UnitOfMeasure::METRE),
      Length(0.0, UnitOfMeasure::METRE));
  auto lcc_2sp_p21 = Conversion::createLambertConicConformal_2SP(
      NameProps("lcc"), Angle(45.0, UnitOfMeasure::DEGREE),
      Angle(3.0, UnitOfMeasure::DEGREE), Angle(49.0, UnitOfMeasure::DEGREE),
      Angle(44.0, UnitOfMeasure::DEGREE), Length(0.0, UnitOfMeasure::METRE),
      Length(0.0, UnitOfMeasure::METRE));
  EXPECT_TRUE(lcc_2sp_p12->isEquivalentTo(lcc_2sp_p21.get(),
                                          IComparable::Criterion::EQUIVALENT));

  // 4. Mercator Variant A (1SP) vs Variant B (2SP) with lat_ts = 0, k0 = 1.
  auto merc_1sp = ProjectedCRS::create(
                      NameProps("p"), g1,
                      Conversion::createMercatorVariantA(
                          NameProps("merc"), Angle(0.0, UnitOfMeasure::DEGREE),
                          Angle(0.0, UnitOfMeasure::DEGREE), Scale(1.0),
                          Length(0.0, UnitOfMeasure::METRE),
                          Length(0.0, UnitOfMeasure::METRE)),
                      cart_cs)
                      ->derivingConversion();
  auto merc_2sp = ProjectedCRS::create(
                      NameProps("p"), g1,
                      Conversion::createMercatorVariantB(
                          NameProps("merc"), Angle(0.0, UnitOfMeasure::DEGREE),
                          Angle(0.0, UnitOfMeasure::DEGREE),
                          Length(0.0, UnitOfMeasure::METRE),
                          Length(0.0, UnitOfMeasure::METRE)),
                      cart_cs)
                      ->derivingConversion();
  EXPECT_TRUE(merc_1sp->isEquivalentTo(merc_2sp.get(),
                                       IComparable::Criterion::EQUIVALENT));
  EXPECT_TRUE(merc_2sp->isEquivalentTo(merc_1sp.get(),
                                       IComparable::Criterion::EQUIVALENT));

  // 5. Extra parameter with neutral default value (k=1.0, fe=0.0) vs non-zero.
  auto tm_min = MakeTransf(
      g1, dst_crs, nullptr, EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
      EPSG_CODE_METHOD_TRANSVERSE_MERCATOR,
      {MakeMeasureParamVal(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, 0.0,
                           UnitOfMeasure::DEGREE)});
  auto tm_with_defaults = MakeTransf(
      g1, dst_crs, nullptr, EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
      EPSG_CODE_METHOD_TRANSVERSE_MERCATOR,
      {MakeMeasureParamVal(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, 0.0,
                           UnitOfMeasure::DEGREE),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN,
                           EPSG_CODE_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN,
                           1.0, UnitOfMeasure::SCALE_UNITY),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_FALSE_EASTING,
                           EPSG_CODE_PARAMETER_FALSE_EASTING, 0.0,
                           UnitOfMeasure::METRE)});
  auto tm_non_default = MakeTransf(
      g1, dst_crs, nullptr, EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
      EPSG_CODE_METHOD_TRANSVERSE_MERCATOR,
      {MakeMeasureParamVal(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                           EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN, 0.0,
                           UnitOfMeasure::DEGREE),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_FALSE_EASTING,
                           EPSG_CODE_PARAMETER_FALSE_EASTING, 100.0,
                           UnitOfMeasure::METRE)});
  EXPECT_TRUE(tm_min->isEquivalentTo(tm_with_defaults.get(),
                                     IComparable::Criterion::EQUIVALENT));
  EXPECT_TRUE(tm_with_defaults->isEquivalentTo(
      tm_min.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(tm_min->isEquivalentTo(tm_non_default.get(),
                                      IComparable::Criterion::EQUIVALENT));
}

TEST(SingleOperationTest, ValidateParametersAllBranches) {
  auto g1 = GeographicCRS::EPSG_4326;
  auto dst = MakeProjCRS();

  // 1. Unknown method.
  auto op_unknown =
      MakeTransf(g1, dst, nullptr, "Completely Unknown Method", 0, {});
  auto res1 = op_unknown->validateParameters();
  ASSERT_EQ(1u, res1.size());
  EXPECT_THAT(res1.front(), HasSubstr("Unknown method"));

  // 2. Method name equivalent to official name, but not strictly equal, and
  // mismatched EPSG code vs official code.
  auto op_equiv_name_wrong_code =
      MakeTransf(g1, dst, nullptr, "transverse_mercator", 99999, {});
  auto res2 = op_equiv_name_wrong_code->validateParameters();
  EXPECT_THAT(res2, ::testing::Contains(HasSubstr("not strictly equal")));
  EXPECT_THAT(res2,
              ::testing::Contains(HasSubstr("does not match official code")));
  EXPECT_THAT(res2,
              ::testing::Contains(HasSubstr("Cannot find expected parameter")));

  // 3. Method matched by EPSG code (9807) with non-equivalent name.
  auto op_wrong_method_name = MakeTransf(
      g1, dst, nullptr, "Bogus Method Name",
      EPSG_CODE_METHOD_TRANSVERSE_MERCATOR,
      {
          // Parameter name case-insensitively equal ("latitude of natural
          // origin") with wrong EPSG code (99998).
          MakeMeasureParamVal("latitude of natural origin", 99998, 0.0,
                              UnitOfMeasure::DEGREE),
          // Parameter matched by EPSG code (8802) with non-equivalent name.
          MakeMeasureParamVal("Bogus Lon Name",
                              EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
                              0.0, UnitOfMeasure::DEGREE),
          // Unexpected extra parameter.
          MakeMeasureParamVal("Unexpected Extra Param", 99997, 1.0,
                              UnitOfMeasure::METRE),
      });
  auto res3 = op_wrong_method_name->validateParameters();
  EXPECT_THAT(res3, ::testing::Contains(HasSubstr(
                        "through its EPSG code has not an equivalent name")));
  EXPECT_THAT(res3, ::testing::Contains(HasSubstr(
                        "Parameter name latitude of natural origin")));
  EXPECT_THAT(res3,
              ::testing::Contains(HasSubstr("Parameter of EPSG code 99998")));
  EXPECT_THAT(res3,
              ::testing::Contains(HasSubstr("Parameter name Bogus Lon Name")));
  EXPECT_THAT(res3, ::testing::Contains(HasSubstr("found but not expected")));

  // 4. Equidistant Cylindrical missing Latitude of natural origin is allowed.
  auto op_eqc = MakeTransf(
      g1, dst, nullptr, EPSG_NAME_METHOD_EQUIDISTANT_CYLINDRICAL,
      EPSG_CODE_METHOD_EQUIDISTANT_CYLINDRICAL,
      {
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_LATITUDE_1ST_STD_PARALLEL,
                              EPSG_CODE_PARAMETER_LATITUDE_1ST_STD_PARALLEL,
                              0.0, UnitOfMeasure::DEGREE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
                              EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
                              0.0, UnitOfMeasure::DEGREE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_FALSE_EASTING,
                              EPSG_CODE_PARAMETER_FALSE_EASTING, 0.0,
                              UnitOfMeasure::METRE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_FALSE_NORTHING,
                              EPSG_CODE_PARAMETER_FALSE_NORTHING, 0.0,
                              UnitOfMeasure::METRE),
      });
  EXPECT_TRUE(op_eqc->validateParameters().empty());
}

TEST(SingleOperationTest, GridsNeededAndFilenameHelpers) {
  ScopedContext ctx = MakeContext();
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  auto g1 = GeographicCRS::EPSG_4267;
  auto g2 = GeographicCRS::EPSG_4269;

  // 1. Comma-separated grids in gridsNeeded (with and without dbContext).
  auto op_grids = SingleOperation::createPROJBased(
      NameProps("grids_op"),
      "+proj=hgridshift +grids=@null,missing_grid_1.tif,missing_grid_2.tif",
      g1.as_nullable(), g2.as_nullable(), {});
  auto grids_no_db = op_grids->gridsNeeded(nullptr, false);
  EXPECT_GE(grids_no_db.size(), 2u);
  auto grids_with_db = op_grids->gridsNeeded(db_ctx.as_nullable(), true);
  EXPECT_GE(grids_with_db.size(), 2u);

  // 2. getPROJ4NadgridsCompatibleFilename: NTv2, NADCON,
  // HORIZONTAL_SHIFT_GTIFF, and non-nadgrids.
  auto tr_ntv2 = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV2, EPSG_CODE_METHOD_NTV2,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "my_ntv2.gsb")});
  EXPECT_EQ("my_ntv2.gsb", tr_ntv2->getPROJ4NadgridsCompatibleFilename());

  auto tr_nadcon = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NADCON, EPSG_CODE_METHOD_NADCON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        "conus.las"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        "conus.los")});
  EXPECT_EQ("conus.las", tr_nadcon->getPROJ4NadgridsCompatibleFilename());

  auto tr_hshift = MakeTransf(
      g1, g2, nullptr, PROJ_WKT2_NAME_METHOD_HORIZONTAL_SHIFT_GTIFF, 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "hshift.tif")});
  EXPECT_EQ("hshift.tif", tr_hshift->getPROJ4NadgridsCompatibleFilename());

  auto tr_helmert = Transformation::createGeocentricTranslations(
      NameProps("h"), g1, g2, 1.0, 2.0, 3.0, {});
  EXPECT_EQ("", tr_helmert->getPROJ4NadgridsCompatibleFilename());

  // 3. isGeographic3DToGravityRelatedHeight & getHeightToGeographic3DFilename.
  auto m_geoid_name = OperationMethod::create(
      NameProps("Geographic3D to GravityRelatedHeight (OSGM15)"),
      std::vector<OperationParameterNNPtr>{});
  EXPECT_TRUE(Transformation::isGeographic3DToGravityRelatedHeight(
      m_geoid_name, /*allowInverse=*/false));

  auto m_geoid_inv_name = OperationMethod::create(
      NameProps("Inverse of Geographic3D to GravityRelatedHeight (OSGM15)"),
      std::vector<OperationParameterNNPtr>{});
  EXPECT_FALSE(Transformation::isGeographic3DToGravityRelatedHeight(
      m_geoid_inv_name, /*allowInverse=*/false));
  EXPECT_TRUE(Transformation::isGeographic3DToGravityRelatedHeight(
      m_geoid_inv_name, /*allowInverse=*/true));

  auto m_geoid_code =
      OperationMethod::create(EpsgProps("Custom Geoid Method", 9665),
                              std::vector<OperationParameterNNPtr>{});
  EXPECT_TRUE(Transformation::isGeographic3DToGravityRelatedHeight(
      m_geoid_code, /*allowInverse=*/false));

  auto m_geoid_inv_code = OperationMethod::create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "Custom Inv Geoid Method")
          .set(Identifier::CODESPACE_KEY, "INVERSE(EPSG)")
          .set(Identifier::CODE_KEY, 9665),
      std::vector<OperationParameterNNPtr>{});
  EXPECT_FALSE(Transformation::isGeographic3DToGravityRelatedHeight(
      m_geoid_inv_code, /*allowInverse=*/false));
  EXPECT_TRUE(Transformation::isGeographic3DToGravityRelatedHeight(
      m_geoid_inv_code, /*allowInverse=*/true));

  auto tr_geoid_gtx = MakeTransf(
      MakeGeog3D(), MakeVertCRS(), nullptr,
      PROJ_WKT2_NAME_METHOD_HEIGHT_TO_GEOG3D, 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "egm96_15.gtx")});
  EXPECT_EQ("egm96_15.gtx", tr_geoid_gtx->getHeightToGeographic3DFilename());
  EXPECT_EQ("", tr_helmert->getHeightToGeographic3DFilename());
}

TEST(SingleOperationTest, ExportTransformationToWKTAndAbridged) {
  auto src = GeographicCRS::EPSG_4326;
  auto dst = GeographicCRS::EPSG_4269;
  auto interp = GeographicCRS::create(
      EpsgProps("ETRS89", 4258), GeodeticReferenceFrame::EPSG_6326,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  auto acc = PositionalAccuracy::create("2.0");
  auto props = PropertyMap()
                   .set(IdentifiedObject::NAME_KEY, "WKT Transf")
                   .set(CoordinateOperation::OPERATION_VERSION_KEY, "v1");
  auto tr = Transformation::create(
      props, src, dst, interp.as_nullable(),
      EpsgProps(EPSG_NAME_METHOD_VERTICAL_OFFSET_AND_SLOPE,
                EPSG_CODE_METHOD_VERTICAL_OFFSET_AND_SLOPE),
      std::vector<OperationParameterNNPtr>{
          MakeParam(EPSG_NAME_PARAMETER_VERTICAL_OFFSET,
                    EPSG_CODE_PARAMETER_VERTICAL_OFFSET)},
      std::vector<ParameterValueNNPtr>{
          ParameterValue::create(Measure(1.0, UnitOfMeasure::METRE))},
      {acc});

  // WKT1 throws FormattingException on Transformation.
  auto wkt1 = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THROW(tr->_exportToWKT(wkt1.get()), FormattingException);

  // Full WKT2_2019 export.
  auto wkt2 = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
  tr->_exportToWKT(wkt2.get());
  std::string wkt2_str = wkt2->toString();
  EXPECT_THAT(wkt2_str, HasSubstr("COORDINATEOPERATION[\"WKT Transf\""));
  EXPECT_THAT(wkt2_str, HasSubstr("VERSION[\"v1\"]"));
  EXPECT_THAT(wkt2_str, HasSubstr("INTERPOLATIONCRS["));
  EXPECT_THAT(wkt2_str, HasSubstr("OPERATIONACCURACY[2.0]"));

  // Abridged WKT2 export via BoundCRS: exercises
  // createOperationParameterValueFromInterpolationCRS for both 1046 and
  // non-1046.
  auto bound1 = BoundCRS::create(src, dst, tr);
  auto wkt2_b1 = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
  bound1->_exportToWKT(wkt2_b1.get());
  EXPECT_THAT(wkt2_b1->toString(),
              HasSubstr("ABRIDGEDTRANSFORMATION[\"WKT Transf\""));
  EXPECT_THAT(wkt2_b1->toString(),
              HasSubstr(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_HORIZONTAL_CRS));

  auto tr_other = Transformation::create(
      props, src, dst, interp.as_nullable(),
      EpsgProps(
          EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
          EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN),
      std::vector<OperationParameterNNPtr>{
          MakeParam(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                    EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE)},
      std::vector<ParameterValueNNPtr>{
          ParameterValue::createFilename("gr3df97a.txt")},
      {acc});
  auto bound2 = BoundCRS::create(src, dst, tr_other);
  auto wkt2_b2 = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
  bound2->_exportToWKT(wkt2_b2.get());
  EXPECT_THAT(wkt2_b2->toString(),
              HasSubstr(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS));
}

// ---------------------------------------------------------------------------
// 5. substitutePROJAlternativeGridNames (SingleOperation &
// PointMotionOperation)
// ---------------------------------------------------------------------------

struct ScopedSqliteDb {
  sqlite3* db = nullptr;
  ScopedSqliteDb() {
    sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                    nullptr);
    const char* ddl =
        "CREATE TABLE grid_alternatives ("
        "original_grid_name TEXT, proj_grid_name TEXT, "
        "old_proj_grid_name TEXT, proj_grid_format TEXT, "
        "proj_method TEXT, inverse_direction INTEGER, "
        "package_name TEXT, url TEXT, direct_download INTEGER, "
        "open_license INTEGER, directory TEXT);";
    sqlite3_exec(db, ddl, nullptr, nullptr, nullptr);
  }
  ~ScopedSqliteDb() {
    if (db) {
      sqlite3_close(db);
    }
  }
  void InsertGridAlt(const std::string& orig, const std::string& proj_name,
                     const std::string& format, int inv = 0) const {
    std::string sql =
        "INSERT INTO grid_alternatives (original_grid_name, proj_grid_name, "
        "proj_grid_format, inverse_direction) VALUES ('" +
        orig + "', '" + proj_name + "', '" + format + "', " +
        std::to_string(inv) + ");";
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
  }
};

TEST(SingleOperationTest, SubstitutePROJAlternativeGridNamesAllBranches) {
  ScopedSqliteDb mem_sql;
  mem_sql.InsertGridAlt("nadcon_ct2.las", "conus_ct2", "CTable2", 0);
  mem_sql.InsertGridAlt("nadcon_gtiff.las", "us_noaa_conus.tif", "GTiff", 0);
  mem_sql.InsertGridAlt("nadcon_gtiff.los", "us_noaa_conus.tif", "GTiff", 0);
  mem_sql.InsertGridAlt("nadcon5_2d.b", "us_noaa_nadcon5_2d.tif", "GTiff", 0);
  mem_sql.InsertGridAlt("nadcon5_inv.b", "us_noaa_nadcon5_inv.tif", "GTiff", 1);
  mem_sql.InsertGridAlt("nadcon5_3d_same.b", "us_noaa_nadcon5_3d.tif", "GTiff",
                        0);
  mem_sql.InsertGridAlt("nadcon5_3d_h.b", "us_noaa_nadcon5_h.tif", "GTiff", 0);
  mem_sql.InsertGridAlt("ntv1_orig.dac", "ca_nrc_ntv1.gsb", "NTv2", 0);
  mem_sql.InsertGridAlt("ntv1_gtiff.dac", "ca_nrc_ntv1.tif", "GTiff", 0);
  mem_sql.InsertGridAlt("ntv2_gtiff.gsb", "ca_nrc_ntv2.tif", "GTiff", 0);
  mem_sql.InsertGridAlt("ntv2_inv_gtiff.gsb", "ca_nrc_ntv2_inv.tif", "GTiff",
                        1);
  mem_sql.InsertGridAlt("ntv1_inv_ntv2.dac", "ca_nrc_ntv1_inv.gsb", "NTv2", 1);
  mem_sql.InsertGridAlt("ntv1_inv_ntv1.dac", "ca_nrc_ntv1_inv.dac", "NTv1", 1);
  mem_sql.InsertGridAlt("nadcon_inv_ct2.las", "conus_inv_ct2", "CTable2", 1);
  mem_sql.InsertGridAlt("vertcon_fwd.94", "vertconw.gtx", "GTX", 0);
  mem_sql.InsertGridAlt("vertcon_inv.94", "vertconw.tif", "GTiff", 1);
  mem_sql.InsertGridAlt("geoid_fwd.bin", "egm96_15.gtx", "GTX", 0);
  mem_sql.InsertGridAlt("geoid_inv.bin", "egm96_15.tif", "GTiff", 1);
  mem_sql.InsertGridAlt("reg_vert_fwd.txt", "reg_vert.gtx", "GTX", 0);
  mem_sql.InsertGridAlt("reg_vert_inv.txt", "reg_vert.tif", "GTiff", 1);
  mem_sql.InsertGridAlt("ign_fwd.txt", "fr_ign_gr3df97a.tif", "GTiff", 0);
  mem_sql.InsertGridAlt("ign_inv.txt", "fr_ign_gr3df97a.gsb", "NTv2", 1);
  mem_sql.InsertGridAlt("vel_3d.vel", "ca_nrc_vel3d.tif", "GTiff", 0);
  mem_sql.InsertGridAlt("vel_neu.vel", "ca_nrc_velneu.tif", "GTiff", 0);
  mem_sql.InsertGridAlt("nz_def.csv", "nz_linz_def.json", "JSON", 0);
  mem_sql.InsertGridAlt("nz_def_inv.csv", "nz_linz_def_inv.json", "JSON", 1);
  mem_sql.InsertGridAlt("tin_model.json", "tin_model.json", "JSON", 0);
  mem_sql.InsertGridAlt("tin_inv.json", "tin_inv.json", "JSON", 1);
  mem_sql.InsertGridAlt("gtg_vel.gtg", "fr_ign_vel.tif", "GTiff", 0);
  mem_sql.InsertGridAlt("gtg_grid.gtg", "fr_ign_grid.tif", "GTiff", 0);

  auto db_ctx = DatabaseContext::create(mem_sql.db);
  auto g1 = GeographicCRS::EPSG_4267;
  auto g2 = GeographicCRS::EPSG_4269;
  auto g3d = MakeGeog3D();
  auto v1 = MakeVertCRS("V1");
  auto v2 = MakeVertCRS("V2");
  auto gc1 = MakeGeocentric("GC1");
  auto gc2 = MakeGeocentric("GC2");

  // 1a. NADCON -> CTable2.
  auto nadcon_ct2 = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NADCON, EPSG_CODE_METHOD_NADCON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        "nadcon_ct2.las"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        "nadcon_ct2.los")});
  auto sub_nadcon_ct2 = nadcon_ct2->substitutePROJAlternativeGridNames(db_ctx);
  EXPECT_EQ(PROJ_WKT2_NAME_METHOD_CTABLE2, sub_nadcon_ct2->method()->nameStr());

  // 1b. NADCON -> HORIZONTAL_SHIFT_GTIFF.
  auto nadcon_gtiff = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NADCON, EPSG_CODE_METHOD_NADCON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        "nadcon_gtiff.las"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        "nadcon_gtiff.los")});
  auto sub_nadcon_gtiff =
      nadcon_gtiff->substitutePROJAlternativeGridNames(db_ctx);
  EXPECT_EQ(PROJ_WKT2_NAME_METHOD_HORIZONTAL_SHIFT_GTIFF,
            sub_nadcon_gtiff->method()->nameStr());

  // 2. NADCON5 (2D) -> GENERAL_SHIFT_GTIFF (forward and inverse).
  auto nadcon5_2d = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NADCON5_2D, EPSG_CODE_METHOD_NADCON5_2D,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        "nadcon5_2d.b"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        "nadcon5_2d.b")});
  auto sub_nadcon5_2d = nadcon5_2d->substitutePROJAlternativeGridNames(db_ctx);
  EXPECT_EQ(PROJ_WKT2_NAME_METHOD_GENERAL_SHIFT_GTIFF,
            sub_nadcon5_2d->method()->nameStr());

  auto nadcon5_2d_inv = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NADCON5_2D, EPSG_CODE_METHOD_NADCON5_2D,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        "nadcon5_inv.b"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        "nadcon5_inv.b")});
  EXPECT_THAT(nadcon5_2d_inv->substitutePROJAlternativeGridNames(db_ctx)
                  ->method()
                  ->nameStr(),
              HasSubstr("Inverse of"));

  // 3a. NADCON5 (3D) -> GENERAL_SHIFT_GTIFF (single combined file).
  auto nadcon5_3d_same = MakeTransf(
      g3d, g3d, nullptr, EPSG_NAME_METHOD_NADCON5_3D,
      EPSG_CODE_METHOD_NADCON5_3D,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        "nadcon5_3d_same.b"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        "nadcon5_3d_same.b"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_ELLIPSOIDAL_HEIGHT_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_ELLIPSOIDAL_HEIGHT_DIFFERENCE_FILE,
                        "nadcon5_3d_same.b")});
  auto sub_nadcon5_3d_same =
      nadcon5_3d_same->substitutePROJAlternativeGridNames(db_ctx);
  EXPECT_EQ(PROJ_WKT2_NAME_METHOD_GENERAL_SHIFT_GTIFF,
            sub_nadcon5_3d_same->method()->nameStr());

  // 3b. NADCON5 (3D) -> GENERAL_SHIFT_GTIFF (separate height file).
  auto nadcon5_3d_sep = MakeTransf(
      g3d, g3d, nullptr, EPSG_NAME_METHOD_NADCON5_3D,
      EPSG_CODE_METHOD_NADCON5_3D,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        "nadcon5_2d.b"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        "nadcon5_2d.b"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_ELLIPSOIDAL_HEIGHT_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_ELLIPSOIDAL_HEIGHT_DIFFERENCE_FILE,
                        "nadcon5_3d_h.b")});
  auto sub_nadcon5_3d_sep =
      nadcon5_3d_sep->substitutePROJAlternativeGridNames(db_ctx);
  EXPECT_EQ("us_noaa_nadcon5_2d.tif",
            nn_dynamic_pointer_cast<OperationParameterValue>(
                sub_nadcon5_3d_sep->parameterValues()[0])
                ->parameterValue()
                ->valueFile());

  // 4. Change of Vertical Unit (1069) with empty params returns self.
  auto cvu_empty =
      MakeTransf(v1, v2, nullptr, EPSG_NAME_METHOD_CHANGE_VERTICAL_UNIT,
                 EPSG_CODE_METHOD_CHANGE_VERTICAL_UNIT, {});
  auto sub_cvu = cvu_empty->substitutePROJAlternativeGridNames(db_ctx);
  EXPECT_EQ(EPSG_CODE_METHOD_CHANGE_VERTICAL_UNIT,
            sub_cvu->method()->getEPSGCode());

  // 5a & 5b. NTv1 -> NTv2 and NTv1/NTv2 -> HORIZONTAL_SHIFT_GTIFF (forward &
  // inverse).
  auto ntv1_to_ntv2 = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV1, EPSG_CODE_METHOD_NTV1,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "ntv1_orig.dac")});
  EXPECT_EQ(EPSG_CODE_METHOD_NTV2,
            ntv1_to_ntv2->substitutePROJAlternativeGridNames(db_ctx)
                ->method()
                ->getEPSGCode());

  auto ntv1_to_gtiff = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV1, EPSG_CODE_METHOD_NTV1,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "ntv1_gtiff.dac")});
  EXPECT_EQ(PROJ_WKT2_NAME_METHOD_HORIZONTAL_SHIFT_GTIFF,
            ntv1_to_gtiff->substitutePROJAlternativeGridNames(db_ctx)
                ->method()
                ->nameStr());

  auto ntv2_to_gtiff = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV2, EPSG_CODE_METHOD_NTV2,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "ntv2_gtiff.gsb")});
  EXPECT_EQ(PROJ_WKT2_NAME_METHOD_HORIZONTAL_SHIFT_GTIFF,
            ntv2_to_gtiff->substitutePROJAlternativeGridNames(db_ctx)
                ->method()
                ->nameStr());

  auto ntv2_inv_gtiff = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV2, EPSG_CODE_METHOD_NTV2,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "ntv2_inv_gtiff.gsb")});
  EXPECT_THAT(ntv2_inv_gtiff->substitutePROJAlternativeGridNames(db_ctx)
                  ->method()
                  ->nameStr(),
              HasSubstr("Inverse of"));

  auto ntv1_inv_ntv2 = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV1, EPSG_CODE_METHOD_NTV1,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "ntv1_inv_ntv2.dac")});
  EXPECT_THAT(ntv1_inv_ntv2->substitutePROJAlternativeGridNames(db_ctx)
                  ->method()
                  ->nameStr(),
              HasSubstr("Inverse of"));

  auto ntv1_inv_ntv1 = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV1, EPSG_CODE_METHOD_NTV1,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "ntv1_inv_ntv1.dac")});
  EXPECT_THAT(ntv1_inv_ntv1->substitutePROJAlternativeGridNames(db_ctx)
                  ->method()
                  ->nameStr(),
              HasSubstr("Inverse of"));

  auto nadcon_inv_ct2 = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NADCON, EPSG_CODE_METHOD_NADCON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_DIFFERENCE_FILE,
                        "nadcon_inv_ct2.las"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LONGITUDE_DIFFERENCE_FILE,
                        "nadcon_inv_ct2.las")});
  EXPECT_THAT(nadcon_inv_ct2->substitutePROJAlternativeGridNames(db_ctx)
                  ->method()
                  ->nameStr(),
              HasSubstr("Inverse of"));

  // 5c. VERTCON forward and inverse.
  auto vertcon_fwd = MakeTransf(
      v1, v2, nullptr, EPSG_NAME_METHOD_VERTCON, EPSG_CODE_METHOD_VERTCON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_VERTICAL_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_VERTICAL_OFFSET_FILE,
                        "vertcon_fwd.94")});
  EXPECT_EQ("vertconw.gtx",
            nn_dynamic_pointer_cast<OperationParameterValue>(
                vertcon_fwd->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()[0])
                ->parameterValue()
                ->valueFile());

  auto vertcon_inv = MakeTransf(
      v1, v2, nullptr, EPSG_NAME_METHOD_VERTCON, EPSG_CODE_METHOD_VERTCON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_VERTICAL_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_VERTICAL_OFFSET_FILE,
                        "vertcon_inv.94")});
  EXPECT_THAT(vertcon_inv->substitutePROJAlternativeGridNames(db_ctx)
                  ->method()
                  ->nameStr(),
              HasSubstr("Inverse of"));

  // 5d & 5e. Geographic3D to GravityRelatedHeight & GravityRelatedHeight to
  // Geographic3D (1-param and 2-param).
  auto geoid_1p_fwd = MakeTransf(
      g3d, v1, nullptr, "Geographic3D to GravityRelatedHeight (EGM)", 9665,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "geoid_fwd.bin")});
  auto sub_geoid_1p_fwd =
      geoid_1p_fwd->substitutePROJAlternativeGridNames(db_ctx);
  EXPECT_EQ("Geographic3D to GravityRelatedHeight (EGM)",
            sub_geoid_1p_fwd->method()->nameStr());
  EXPECT_EQ("egm96_15.gtx", nn_dynamic_pointer_cast<OperationParameterValue>(
                                sub_geoid_1p_fwd->parameterValues()[0])
                                ->parameterValue()
                                ->valueFile());

  auto geoid_1p_inv = MakeTransf(
      g3d, v1, nullptr, "Geographic3D to GravityRelatedHeight (EGM)", 9665,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "geoid_inv.bin")});
  EXPECT_THROW(geoid_1p_inv->substitutePROJAlternativeGridNames(db_ctx),
               UnsupportedOperationException);

  auto h_to_geog3d = MakeTransf(
      v1, g3d, nullptr, "GravityRelatedHeight to Geographic3D", 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "geoid_fwd.bin")});
  EXPECT_EQ("GravityRelatedHeight to Geographic3D",
            h_to_geog3d->substitutePROJAlternativeGridNames(db_ctx)
                ->method()
                ->nameStr());

  // 5f. Regular vertical grid method (1-param and 2-param).
  auto reg_vert_1p =
      MakeTransf(v1, v2, nullptr, EPSG_NAME_METHOD_VERTICALGRID_GTX,
                 EPSG_CODE_METHOD_VERTICALGRID_GTX,
                 {MakeFileParamVal(EPSG_NAME_PARAMETER_VERTICAL_OFFSET_FILE,
                                   EPSG_CODE_PARAMETER_VERTICAL_OFFSET_FILE,
                                   "reg_vert_inv.txt")});
  EXPECT_THAT(reg_vert_1p->substitutePROJAlternativeGridNames(db_ctx)
                  ->method()
                  ->nameStr(),
              HasSubstr("Inverse of"));

  auto reg_vert_2p = MakeTransf(
      v1, v2, nullptr, EPSG_NAME_METHOD_VERTICALGRID_GTX,
      EPSG_CODE_METHOD_VERTICALGRID_GTX,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_VERTICAL_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_VERTICAL_OFFSET_FILE,
                        "reg_vert_fwd.txt"),
       MakeIntParamVal(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                       EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                       4326)});
  EXPECT_EQ(1u, reg_vert_2p->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()
                    .size());

  // 5g & 6b. Geocentric translation by grid (IGN) (1-param and 2-param).
  auto ign_1p = MakeTransf(
      g1, g2, &g1,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "ign_inv.txt")});
  EXPECT_THROW(ign_1p->substitutePROJAlternativeGridNames(db_ctx),
               UnsupportedOperationException);

  auto ign_2p = MakeTransf(
      g1, g2, nullptr,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      {MakeIntParamVal(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                       EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                       4326),
       MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "ign_fwd.txt")});
  EXPECT_EQ(1u, ign_2p->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()
                    .size());

  // 5h, 5i, 5j, 5k, 5l. Velocity grids, NZ deformation, TIN, GTG.
  auto vel_3d = MakeTransf(
      g3d, g3d, &g3d,
      EPSG_NAME_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel_3d.vel")});
  EXPECT_EQ("ca_nrc_vel3d.tif",
            nn_dynamic_pointer_cast<OperationParameterValue>(
                vel_3d->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()[0])
                ->parameterValue()
                ->valueFile());

  auto vel_neu = MakeTransf(
      v1, v2, &g1,
      EPSG_NAME_METHOD_VERTICAL_OFFSET_USING_NEU_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_VERTICAL_OFFSET_USING_NEU_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel_neu.vel")});
  EXPECT_EQ("ca_nrc_velneu.tif",
            nn_dynamic_pointer_cast<OperationParameterValue>(
                vel_neu->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()[0])
                ->parameterValue()
                ->valueFile());

  auto nz_def = MakeTransf(
      g3d, g3d, nullptr, EPSG_NAME_METHOD_NEW_ZEALAND_DEFORMATION_MODEL,
      EPSG_CODE_METHOD_NEW_ZEALAND_DEFORMATION_MODEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "nz_def.csv")});
  EXPECT_EQ("nz_linz_def.json",
            nn_dynamic_pointer_cast<OperationParameterValue>(
                nz_def->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()[0])
                ->parameterValue()
                ->valueFile());

  auto nz_def_inv = MakeTransf(
      g3d, g3d, nullptr, EPSG_NAME_METHOD_NEW_ZEALAND_DEFORMATION_MODEL,
      EPSG_CODE_METHOD_NEW_ZEALAND_DEFORMATION_MODEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "nz_def_inv.csv")});
  EXPECT_THAT(nz_def_inv->substitutePROJAlternativeGridNames(db_ctx)
                  ->method()
                  ->nameStr(),
              HasSubstr("Inverse of"));

  // Note: EPSG_CODE_PARAMETER_TIN_OFFSET_FILE is omitted from gParamNameCodes
  // in parammappings.cpp, so createOpParamNameEPSGCode(1064) asserts if
  // filename != projFilename. Test the filename == projFilename branches
  // (forward identity and inverse UnsupportedOperationException) on TIN
  // methods while NZ deformation covers the filename != projFilename branches.
  auto tin_op =
      MakeTransf(v1, v2, nullptr,
                 EPSG_NAME_METHOD_VERTICAL_OFFSET_BY_TIN_INTERPOLATION_JSON,
                 EPSG_CODE_METHOD_VERTICAL_OFFSET_BY_TIN_INTERPOLATION_JSON,
                 {MakeFileParamVal(EPSG_NAME_PARAMETER_TIN_OFFSET_FILE,
                                   EPSG_CODE_PARAMETER_TIN_OFFSET_FILE,
                                   "tin_model.json")});
  EXPECT_EQ("tin_model.json",
            nn_dynamic_pointer_cast<OperationParameterValue>(
                tin_op->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()[0])
                ->parameterValue()
                ->valueFile());

  auto tin_op_inv = MakeTransf(
      v1, v2, nullptr,
      EPSG_NAME_METHOD_VERTICAL_OFFSET_BY_TIN_INTERPOLATION_JSON,
      EPSG_CODE_METHOD_VERTICAL_OFFSET_BY_TIN_INTERPOLATION_JSON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_TIN_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_TIN_OFFSET_FILE, "tin_inv.json")});
  EXPECT_THROW(tin_op_inv->substitutePROJAlternativeGridNames(db_ctx),
               UnsupportedOperationException);

  auto gtg_1p = MakeTransf(
      gc1, gc2, &g1,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "gtg_vel.gtg")});
  EXPECT_EQ("fr_ign_vel.tif",
            nn_dynamic_pointer_cast<OperationParameterValue>(
                gtg_1p->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()[0])
                ->parameterValue()
                ->valueFile());

  // 6a. 2-param Geographic3D to GravityRelatedHeight with real proj.db lookup
  // of interpolationCRS.
  auto geoid_2p = MakeTransf(
      g3d, v1, nullptr, "Geographic3D to GravityRelatedHeight (EGM)", 9665,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "geoid_fwd.bin"),
       MakeIntParamVal(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                       EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                       4326)});
  EXPECT_EQ(1u, geoid_2p->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()
                    .size());

  // 6d. 9-param GTG Position Vector + NEU velocity grid + interp CRS code.
  auto gtg_9p = MakeTransf(
      gc1, gc2, &g1,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_NAME_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_X_AXIS_TRANSLATION,
                              EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION, 1.0,
                              UnitOfMeasure::METRE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_Y_AXIS_TRANSLATION,
                              EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION, 2.0,
                              UnitOfMeasure::METRE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_Z_AXIS_TRANSLATION,
                              EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION, 3.0,
                              UnitOfMeasure::METRE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_X_AXIS_ROTATION,
                              EPSG_CODE_PARAMETER_X_AXIS_ROTATION, 0.1,
                              UnitOfMeasure::ARC_SECOND),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_Y_AXIS_ROTATION,
                              EPSG_CODE_PARAMETER_Y_AXIS_ROTATION, 0.2,
                              UnitOfMeasure::ARC_SECOND),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_Z_AXIS_ROTATION,
                              EPSG_CODE_PARAMETER_Z_AXIS_ROTATION, 0.3,
                              UnitOfMeasure::ARC_SECOND),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_SCALE_DIFFERENCE,
                              EPSG_CODE_PARAMETER_SCALE_DIFFERENCE, 0.4,
                              UnitOfMeasure::PARTS_PER_MILLION),
          MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                           EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                           "gtg_vel.gtg"),
          MakeIntParamVal(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                          EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                          4326),
      });
  EXPECT_EQ(9u, gtg_9p->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()
                    .size());

  // 6e. 3-param GTG Geocentric translations by grid + NEU velocity grid +
  // interp CRS code.
  auto gtg_3p = MakeTransf(
      gc1, gc2, &g1,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {
          MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                           EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                           "gtg_grid.gtg"),
          MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                           EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                           "gtg_vel.gtg"),
          MakeIntParamVal(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                          EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                          4326),
      });
  EXPECT_EQ(3u, gtg_3p->substitutePROJAlternativeGridNames(db_ctx)
                    ->parameterValues()
                    .size());
}

// ---------------------------------------------------------------------------
// 6. exportToPROJStringGeneric: Vertical Units, Affine, Similarity, Seismic,
//    Axis Reversal, Geographic/Geocentric, Helmert, Molodensky, Offsets
// ---------------------------------------------------------------------------

TEST(SingleOperationTest, ExportToPROJStringVerticalUnitAndBasicOps) {
  auto v_m1 = MakeVertCRS("V_m1", UnitOfMeasure::METRE);
  auto v_m2 = MakeVertCRS("V_m2", UnitOfMeasure::METRE);
  auto v_ft = MakeVertCRS("V_ft", UnitOfMeasure::FOOT);
  auto v_usft = MakeVertCRS("V_usft", UnitOfMeasure::US_FOOT);

  // 1. Change of Vertical Unit (1069 - explicit scalar: 1.0, ft->m, m->ft,
  // ft->usft, usft->ft, custom scalar, and inverse).
  auto make_cvu_1069 = [&](const VerticalCRSNNPtr& src_v,
                           const VerticalCRSNNPtr& dst_v, double factor) {
    return MakeTransf(
        src_v, dst_v, nullptr, EPSG_NAME_METHOD_CHANGE_VERTICAL_UNIT,
        EPSG_CODE_METHOD_CHANGE_VERTICAL_UNIT,
        {MakeMeasureParamVal(EPSG_NAME_PARAMETER_UNIT_CONVERSION_SCALAR,
                             EPSG_CODE_PARAMETER_UNIT_CONVERSION_SCALAR, factor,
                             UnitOfMeasure::SCALE_UNITY)});
  };
  auto cvu_m_m = make_cvu_1069(v_m1, v_m2, 1.0);
  EXPECT_THAT(ExportToPROJ(cvu_m_m), HasSubstr("+proj=noop"));

  auto cvu_ft_m = make_cvu_1069(v_ft, v_m1, 0.3048);
  EXPECT_THAT(ExportToPROJ(cvu_ft_m), HasSubstr("+z_in=ft +z_out=m"));
  EXPECT_THAT(ExportToPROJ(cvu_ft_m->inverse()),
              HasSubstr("+z_in=m +z_out=ft"));

  auto cvu_m_ft = make_cvu_1069(v_m1, v_ft, 1.0 / 0.3048);
  EXPECT_THAT(ExportToPROJ(cvu_m_ft), HasSubstr("+z_in=m +z_out=ft"));

  const double us_ft_to_m = 1200.0 / 3937.0;
  auto cvu_ft_usft = make_cvu_1069(v_ft, v_usft, 0.3048 / us_ft_to_m);
  EXPECT_THAT(ExportToPROJ(cvu_ft_usft), HasSubstr("+z_in=ft +z_out=us-ft"));

  auto cvu_usft_ft = make_cvu_1069(v_usft, v_ft, us_ft_to_m / 0.3048);
  EXPECT_THAT(ExportToPROJ(cvu_usft_ft), HasSubstr("+z_in=us-ft +z_out=ft"));

  auto v_custom = MakeVertCRS(
      "V_custom",
      UnitOfMeasure("CustomVert", 2.5, UnitOfMeasure::Type::LINEAR));
  auto cvu_custom = make_cvu_1069(v_custom, v_m1, 2.5);
  EXPECT_THAT(ExportToPROJ(cvu_custom), HasSubstr("+proj=affine +s33=2.5"));

  // Non-VerticalCRS throws FormattingException for 1104 Conversion.
  auto cvu_bad = Conversion::createChangeVerticalUnit(NameProps("cvu_bad"));
  EXPECT_THROW(ExportToPROJ(cvu_bad), FormattingException);

  // 3. Height Depth Reversal (1068).
  auto hdr =
      MakeTransf(v_m1, v_m2, nullptr, EPSG_NAME_METHOD_HEIGHT_DEPTH_REVERSAL,
                 EPSG_CODE_METHOD_HEIGHT_DEPTH_REVERSAL, {});
  EXPECT_THAT(ExportToPROJ(hdr), HasSubstr("+proj=axisswap +order=1,2,-3"));

  // 4. Affine parametric transformation (2D and 3D).
  auto p_crs = MakeProjCRS();
  auto aff_2d = Conversion::createAffineParametric(
      NameProps("aff2d"), Measure(10.0, UnitOfMeasure::METRE), Scale(1.0),
      Scale(0.0), Measure(20.0, UnitOfMeasure::METRE), Scale(0.0), Scale(1.0));
  EXPECT_THAT(ExportToPROJ(aff_2d), HasSubstr("+proj=affine +xoff=10"));
  EXPECT_THAT(ExportToPROJ(aff_2d), HasSubstr("+yoff=20"));

  auto aff_3d = MakeTransf(
      p_crs, p_crs, nullptr,
      PROJ_WKT2_NAME_METHOD_3D_AFFINE_PARAMETRIC_TRANSFORMATION, 0,
      {
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_A0, EPSG_CODE_PARAMETER_A0,
                              1.0, UnitOfMeasure::METRE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_A1, EPSG_CODE_PARAMETER_A1,
                              1.0, UnitOfMeasure::SCALE_UNITY),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_A2, EPSG_CODE_PARAMETER_A2,
                              0.0, UnitOfMeasure::SCALE_UNITY),
          MakeMeasureParamVal(PROJ_WKT2_NAME_PARAMETER_A3, 0, 0.0,
                              UnitOfMeasure::SCALE_UNITY),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_B0, EPSG_CODE_PARAMETER_B0,
                              2.0, UnitOfMeasure::METRE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_B1, EPSG_CODE_PARAMETER_B1,
                              0.0, UnitOfMeasure::SCALE_UNITY),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_B2, EPSG_CODE_PARAMETER_B2,
                              1.0, UnitOfMeasure::SCALE_UNITY),
          MakeMeasureParamVal(PROJ_WKT2_NAME_PARAMETER_B3, 0, 0.0,
                              UnitOfMeasure::SCALE_UNITY),
          MakeMeasureParamVal(PROJ_WKT2_NAME_PARAMETER_C0, 0, 3.0,
                              UnitOfMeasure::METRE),
          MakeMeasureParamVal(PROJ_WKT2_NAME_PARAMETER_C1, 0, 0.0,
                              UnitOfMeasure::SCALE_UNITY),
          MakeMeasureParamVal(PROJ_WKT2_NAME_PARAMETER_C2, 0, 0.0,
                              UnitOfMeasure::SCALE_UNITY),
          MakeMeasureParamVal(PROJ_WKT2_NAME_PARAMETER_C3, 0, 2.0,
                              UnitOfMeasure::SCALE_UNITY),
      });
  EXPECT_THAT(ExportToPROJ(aff_3d), HasSubstr("+zoff=3"));
  EXPECT_THAT(ExportToPROJ(aff_3d), HasSubstr("+s33=2"));

  // 5. Similarity transformation (9621).
  auto sim = MakeTransf(
      p_crs, p_crs, nullptr, EPSG_NAME_METHOD_SIMILARITY_TRANSFORMATION,
      EPSG_CODE_METHOD_SIMILARITY_TRANSFORMATION,
      {
          MakeMeasureParamVal(
              EPSG_NAME_PARAMETER_ORDINATE_1_EVAL_POINT_TARGET_CRS,
              EPSG_CODE_PARAMETER_ORDINATE_1_EVAL_POINT_TARGET_CRS, 100.0,
              UnitOfMeasure::METRE),
          MakeMeasureParamVal(
              EPSG_NAME_PARAMETER_ORDINATE_2_EVAL_POINT_TARGET_CRS,
              EPSG_CODE_PARAMETER_ORDINATE_2_EVAL_POINT_TARGET_CRS, 200.0,
              UnitOfMeasure::METRE),
          MakeMeasureParamVal(
              EPSG_NAME_PARAMETER_SCALE_FACTOR_FOR_SOURCE_CRS_AXES,
              EPSG_CODE_PARAMETER_SCALE_FACTOR_FOR_SOURCE_CRS_AXES, 1.0,
              UnitOfMeasure::SCALE_UNITY),
          MakeMeasureParamVal(
              EPSG_NAME_PARAMETER_ROTATION_ANGLE_OF_SOURCE_CRS_AXES,
              EPSG_CODE_PARAMETER_ROTATION_ANGLE_OF_SOURCE_CRS_AXES, 0.0,
              UnitOfMeasure::DEGREE),
      });
  EXPECT_THAT(ExportToPROJ(sim), HasSubstr("+proj=affine"));

  // 6. Seismic bin grid (9666 and 1049, plus zero SF error).
  auto make_seismic = [&](int method_code, const char* method_name, double sf) {
    return MakeTransf(
        p_crs, p_crs, nullptr, method_name, method_code,
        {
            MakeMeasureParamVal(EPSG_NAME_PARAMETER_BIN_GRID_ORIGIN_I,
                                EPSG_CODE_PARAMETER_BIN_GRID_ORIGIN_I, 1.0,
                                UnitOfMeasure::SCALE_UNITY),
            MakeMeasureParamVal(EPSG_NAME_PARAMETER_BIN_GRID_ORIGIN_J,
                                EPSG_CODE_PARAMETER_BIN_GRID_ORIGIN_J, 1.0,
                                UnitOfMeasure::SCALE_UNITY),
            MakeMeasureParamVal(EPSG_NAME_PARAMETER_BIN_GRID_ORIGIN_EASTING,
                                EPSG_CODE_PARAMETER_BIN_GRID_ORIGIN_EASTING,
                                450000.0, UnitOfMeasure::METRE),
            MakeMeasureParamVal(EPSG_NAME_PARAMETER_BIN_GRID_ORIGIN_NORTHING,
                                EPSG_CODE_PARAMETER_BIN_GRID_ORIGIN_NORTHING,
                                5500000.0, UnitOfMeasure::METRE),
            MakeMeasureParamVal(EPSG_NAME_PARAMETER_SCALE_FACTOR_OF_BIN_GRID,
                                EPSG_CODE_PARAMETER_SCALE_FACTOR_OF_BIN_GRID,
                                sf, UnitOfMeasure::SCALE_UNITY),
            MakeMeasureParamVal(EPSG_NAME_PARAMETER_BIN_WIDTH_ON_I_AXIS,
                                EPSG_CODE_PARAMETER_BIN_WIDTH_ON_I_AXIS, 25.0,
                                UnitOfMeasure::METRE),
            MakeMeasureParamVal(EPSG_NAME_PARAMETER_BIN_WIDTH_ON_J_AXIS,
                                EPSG_CODE_PARAMETER_BIN_WIDTH_ON_J_AXIS, 12.5,
                                UnitOfMeasure::METRE),
            MakeMeasureParamVal(
                EPSG_NAME_PARAMETER_MAP_GRID_BEARING_OF_BIN_GRID_J_AXIS,
                EPSG_CODE_PARAMETER_MAP_GRID_BEARING_OF_BIN_GRID_J_AXIS, 20.0,
                UnitOfMeasure::DEGREE),
            MakeMeasureParamVal(
                EPSG_NAME_PARAMETER_BIN_NODE_INCREMENT_ON_I_AXIS,
                EPSG_CODE_PARAMETER_BIN_NODE_INCREMENT_ON_I_AXIS, 1.0,
                UnitOfMeasure::SCALE_UNITY),
            MakeMeasureParamVal(
                EPSG_NAME_PARAMETER_BIN_NODE_INCREMENT_ON_J_AXIS,
                EPSG_CODE_PARAMETER_BIN_NODE_INCREMENT_ON_J_AXIS, 1.0,
                UnitOfMeasure::SCALE_UNITY),
        });
  };
  auto seis_plus =
      make_seismic(EPSG_CODE_METHOD_SEISMIC_BIN_GRID_I_EQ_J_PLUS_90,
                   EPSG_NAME_METHOD_SEISMIC_BIN_GRID_I_EQ_J_PLUS_90, 1.0);
  EXPECT_THAT(ExportToPROJ(seis_plus), HasSubstr("+proj=affine"));
  auto seis_minus =
      make_seismic(EPSG_CODE_METHOD_SEISMIC_BIN_GRID_I_EQ_J_MINUS_90,
                   EPSG_NAME_METHOD_SEISMIC_BIN_GRID_I_EQ_J_MINUS_90, 1.0);
  EXPECT_THAT(ExportToPROJ(seis_minus), HasSubstr("+proj=affine"));
  auto seis_zero =
      make_seismic(EPSG_CODE_METHOD_SEISMIC_BIN_GRID_I_EQ_J_PLUS_90,
                   EPSG_NAME_METHOD_SEISMIC_BIN_GRID_I_EQ_J_PLUS_90, 0.0);
  EXPECT_THROW(ExportToPROJ(seis_zero), FormattingException);

  // 7. Axis Order Reversal (same unit and different units: degree vs grad).
  auto g_deg = MakeGeog2D("G_deg", UnitOfMeasure::DEGREE);
  auto g_grad = MakeGeog2D("G_grad", UnitOfMeasure::GRAD);
  auto aor_same = Conversion::createAxisOrderReversal(/*is3D=*/false);
  auto tr_aor_diff = MakeTransf(g_deg, g_grad, nullptr,
                                EPSG_NAME_METHOD_AXIS_ORDER_REVERSAL_2D,
                                EPSG_CODE_METHOD_AXIS_ORDER_REVERSAL_2D, {});
  EXPECT_THAT(ExportToPROJ(aor_same), HasSubstr("+proj=axisswap +order=2,1"));
  EXPECT_THAT(ExportToPROJ(tr_aor_diff), HasSubstr("+proj=unitconvert"));

  // 8. Geographic/geocentric conversions (9602).
  auto gc = MakeGeocentric();
  auto geog_to_geoc =
      MakeTransf(g_deg, gc, nullptr, EPSG_NAME_METHOD_GEOGRAPHIC_GEOCENTRIC,
                 EPSG_CODE_METHOD_GEOGRAPHIC_GEOCENTRIC, {});
  EXPECT_THAT(ExportToPROJ(geog_to_geoc), HasSubstr("+proj=cart"));
  auto geoc_to_geog =
      MakeTransf(gc, g_deg, nullptr, EPSG_NAME_METHOD_GEOGRAPHIC_GEOCENTRIC,
                 EPSG_CODE_METHOD_GEOGRAPHIC_GEOCENTRIC, {});
  EXPECT_THAT(ExportToPROJ(geoc_to_geog), HasSubstr("+inv +proj=cart"));
  auto bad_geoc =
      MakeTransf(v_m1, v_m2, nullptr, EPSG_NAME_METHOD_GEOGRAPHIC_GEOCENTRIC,
                 EPSG_CODE_METHOD_GEOGRAPHIC_GEOCENTRIC, {});
  EXPECT_THROW(ExportToPROJ(bad_geoc), FormattingException);
}

TEST(SingleOperationTest, ExportToPROJStringHelmertMolodenskyAndOffsets) {
  auto g2d_a = GeographicCRS::EPSG_4326;
  auto g2d_b = GeographicCRS::EPSG_4269;
  auto g3d = MakeGeog3D();
  auto gc = MakeGeocentric();
  auto v_m = MakeVertCRS("V_m", UnitOfMeasure::METRE);
  auto comp_crs = CompoundCRS::create(NameProps("Comp"), {g2d_a, v_m});

  // 1. Helmert: 3-param, 7-param PV/CF, Full Matrix (+exact), 15-param
  // Time-dependent with source/target coordinate epochs.
  auto h_full =
      MakeTransf(g2d_a, g2d_b, nullptr,
                 EPSG_NAME_METHOD_COORDINATE_FRAME_FULL_MATRIX_GEOGRAPHIC_2D,
                 EPSG_CODE_METHOD_COORDINATE_FRAME_FULL_MATRIX_GEOGRAPHIC_2D,
                 {
                     MakeMeasureParamVal(EPSG_NAME_PARAMETER_X_AXIS_TRANSLATION,
                                         EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION,
                                         1.0, UnitOfMeasure::METRE),
                     MakeMeasureParamVal(EPSG_NAME_PARAMETER_Y_AXIS_TRANSLATION,
                                         EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION,
                                         2.0, UnitOfMeasure::METRE),
                     MakeMeasureParamVal(EPSG_NAME_PARAMETER_Z_AXIS_TRANSLATION,
                                         EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION,
                                         3.0, UnitOfMeasure::METRE),
                     MakeMeasureParamVal(EPSG_NAME_PARAMETER_X_AXIS_ROTATION,
                                         EPSG_CODE_PARAMETER_X_AXIS_ROTATION,
                                         0.1, UnitOfMeasure::ARC_SECOND),
                     MakeMeasureParamVal(EPSG_NAME_PARAMETER_Y_AXIS_ROTATION,
                                         EPSG_CODE_PARAMETER_Y_AXIS_ROTATION,
                                         0.2, UnitOfMeasure::ARC_SECOND),
                     MakeMeasureParamVal(EPSG_NAME_PARAMETER_Z_AXIS_ROTATION,
                                         EPSG_CODE_PARAMETER_Z_AXIS_ROTATION,
                                         0.3, UnitOfMeasure::ARC_SECOND),
                     MakeMeasureParamVal(EPSG_NAME_PARAMETER_SCALE_DIFFERENCE,
                                         EPSG_CODE_PARAMETER_SCALE_DIFFERENCE,
                                         0.4, UnitOfMeasure::PARTS_PER_MILLION),
                 });
  EXPECT_THAT(ExportToPROJ(h_full), HasSubstr("+exact"));

  auto h_15p = Transformation::createTimeDependentPositionVector(
      NameProps("h15p"), gc, gc, 1.0, 2.0, 3.0, 0.1, 0.2, 0.3, 0.4, 0.01, 0.02,
      0.03, 0.001, 0.002, 0.003, 0.004, 2000.0, {});
  CoordOpPeer::SetSourceEpoch(*h_15p,
                              DataEpoch(Measure(2015.0, UnitOfMeasure::YEAR)));
  CoordOpPeer::SetTargetEpoch(*h_15p,
                              DataEpoch(Measure(2025.0, UnitOfMeasure::YEAR)));
  std::string h15_str = ExportToPROJ(h_15p);
  EXPECT_THAT(h15_str, HasSubstr("+proj=set +v_4=2015"));
  EXPECT_THAT(h15_str, HasSubstr("+proj=helmert"));

  auto h_15p_cf = Transformation::createTimeDependentCoordinateFrameRotation(
      NameProps("h15cf"), gc, gc, 1.0, 2.0, 3.0, 0.1, 0.2, 0.3, 0.4, 0.01, 0.02,
      0.03, 0.001, 0.002, 0.003, 0.004, 2000.0, {});
  CoordOpPeer::SetCRSsUpdateInverse(*h_15p_cf, comp_crs, comp_crs);
  EXPECT_THAT(ExportToPROJ(h_15p_cf),
              HasSubstr("+convention=coordinate_frame"));

  // Non-Geodetic CRS on Helmert throws FormattingException.
  auto h_bad = MakeTransf(
      v_m, v_m, nullptr, EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D, {});
  EXPECT_THROW(ExportToPROJ(h_bad), FormattingException);

  // 2. Molodensky-Badekas (PV and CF) & Molodensky / Abridged Molodensky.
  auto mb_params = std::vector<OperationParameterValueNNPtr>{
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_X_AXIS_TRANSLATION,
                          EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION, 1.0,
                          UnitOfMeasure::METRE),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_Y_AXIS_TRANSLATION,
                          EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION, 2.0,
                          UnitOfMeasure::METRE),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_Z_AXIS_TRANSLATION,
                          EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION, 3.0,
                          UnitOfMeasure::METRE),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_X_AXIS_ROTATION,
                          EPSG_CODE_PARAMETER_X_AXIS_ROTATION, 0.1,
                          UnitOfMeasure::ARC_SECOND),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_Y_AXIS_ROTATION,
                          EPSG_CODE_PARAMETER_Y_AXIS_ROTATION, 0.2,
                          UnitOfMeasure::ARC_SECOND),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_Z_AXIS_ROTATION,
                          EPSG_CODE_PARAMETER_Z_AXIS_ROTATION, 0.3,
                          UnitOfMeasure::ARC_SECOND),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_SCALE_DIFFERENCE,
                          EPSG_CODE_PARAMETER_SCALE_DIFFERENCE, 0.4,
                          UnitOfMeasure::PARTS_PER_MILLION),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_ORDINATE_1_EVAL_POINT,
                          EPSG_CODE_PARAMETER_ORDINATE_1_EVAL_POINT, 100.0,
                          UnitOfMeasure::METRE),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_ORDINATE_2_EVAL_POINT,
                          EPSG_CODE_PARAMETER_ORDINATE_2_EVAL_POINT, 200.0,
                          UnitOfMeasure::METRE),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_ORDINATE_3_EVAL_POINT,
                          EPSG_CODE_PARAMETER_ORDINATE_3_EVAL_POINT, 300.0,
                          UnitOfMeasure::METRE),
  };
  auto mb_pv = MakeTransf(
      g3d, g3d, nullptr, EPSG_NAME_METHOD_MOLODENSKY_BADEKAS_PV_GEOGRAPHIC_3D,
      EPSG_CODE_METHOD_MOLODENSKY_BADEKAS_PV_GEOGRAPHIC_3D, mb_params);
  EXPECT_THAT(ExportToPROJ(mb_pv), HasSubstr("+proj=molobadekas"));

  auto mb_cf = MakeTransf(g2d_a, g2d_b, nullptr,
                          EPSG_NAME_METHOD_MOLODENSKY_BADEKAS_CF_GEOGRAPHIC_2D,
                          EPSG_CODE_METHOD_MOLODENSKY_BADEKAS_CF_GEOGRAPHIC_2D,
                          mb_params);
  EXPECT_THAT(ExportToPROJ(mb_cf), HasSubstr("+convention=coordinate_frame"));

  auto mol = Transformation::createMolodensky(
      NameProps("mol"), g2d_a, g2d_b, 10.0, 20.0, 30.0, 100.0, 0.0001, {});
  EXPECT_THAT(ExportToPROJ(mol), HasSubstr("+proj=molodensky"));

  auto ab_mol = Transformation::createAbridgedMolodensky(
      NameProps("ab_mol"), g2d_a, g2d_b, 10.0, 20.0, 30.0, 100.0, 0.0001, {});
  EXPECT_THAT(ExportToPROJ(ab_mol), HasSubstr("+abridged"));

  auto mol_bad = MakeTransf(gc, gc, nullptr, EPSG_NAME_METHOD_MOLODENSKY,
                            EPSG_CODE_METHOD_MOLODENSKY, {});
  EXPECT_THROW(ExportToPROJ(mol_bad), FormattingException);

  // 3. Geographic2D/3D Offsets & Geographic2D with Height Offsets.
  auto g2d_off = Transformation::createGeographic2DOffsets(
      NameProps("g2d_off"), g2d_a, g2d_b, Angle(0.01, UnitOfMeasure::DEGREE),
      Angle(0.02, UnitOfMeasure::DEGREE), {});
  EXPECT_THAT(ExportToPROJ(g2d_off), HasSubstr("+proj=geogoffset"));

  auto g3d_off = Transformation::createGeographic3DOffsets(
      NameProps("g3d_off"), g3d, g3d, Angle(0.01, UnitOfMeasure::DEGREE),
      Angle(0.02, UnitOfMeasure::DEGREE), Length(1.5, UnitOfMeasure::METRE),
      {});
  EXPECT_THAT(ExportToPROJ(g3d_off), HasSubstr("+dh=1.5"));

  auto g2d_h_off = Transformation::createGeographic2DWithHeightOffsets(
      NameProps("g2d_h_off"), comp_crs, comp_crs,
      Angle(0.01, UnitOfMeasure::DEGREE), Angle(0.02, UnitOfMeasure::DEGREE),
      Length(2.5, UnitOfMeasure::METRE), {});
  EXPECT_THAT(ExportToPROJ(g2d_h_off), HasSubstr("+dh=2.5"));

  // 4. Cartesian Grid Offsets (ProjectedCRS, EngineeringCRS in metres, and
  // error branches).
  auto p_crs = MakeProjCRS();
  auto cart_off_proj = Transformation::createCartesianGridOffsets(
      NameProps("c_off"), p_crs, p_crs, Length(5.0, UnitOfMeasure::METRE),
      Length(6.0, UnitOfMeasure::METRE), {});
  EXPECT_THAT(ExportToPROJ(cart_off_proj), HasSubstr("+xoff=5 +yoff=6"));

  auto eng_datum =
      EngineeringDatum::create(NameProps("EngDatum"), optional<std::string>());
  auto eng_crs_m = EngineeringCRS::create(
      NameProps("Eng_m"), eng_datum,
      CartesianCS::createEastingNorthing(UnitOfMeasure::METRE));
  auto cart_off_eng = Transformation::createCartesianGridOffsets(
      NameProps("c_off_eng"), eng_crs_m, eng_crs_m,
      Length(5.0, UnitOfMeasure::METRE), Length(6.0, UnitOfMeasure::METRE), {});
  EXPECT_THAT(ExportToPROJ(cart_off_eng), HasSubstr("+xoff=5 +yoff=6"));

  auto eng_crs_ft = EngineeringCRS::create(
      NameProps("Eng_ft"), eng_datum,
      CartesianCS::createEastingNorthing(UnitOfMeasure::FOOT));
  auto cart_off_ft_err = Transformation::createCartesianGridOffsets(
      NameProps("c_off_ft"), eng_crs_ft, eng_crs_ft,
      Length(5.0, UnitOfMeasure::METRE), Length(6.0, UnitOfMeasure::METRE), {});
  EXPECT_THROW(ExportToPROJ(cart_off_ft_err), FormattingException);

  auto cart_off_geog_err = Transformation::createCartesianGridOffsets(
      NameProps("c_off_geog"), g2d_a, g2d_b, Length(5.0, UnitOfMeasure::METRE),
      Length(6.0, UnitOfMeasure::METRE), {});
  EXPECT_THROW(ExportToPROJ(cart_off_geog_err), FormattingException);

  // 5. Vertical Offset (9616), Geographic3D to GravityRelatedHeight (1136),
  // Geog3D to Geog2D+GravityRelatedHeight (1131), Vertical Offset and Slope
  // (1046).
  auto v_off = Transformation::createVerticalOffset(
      NameProps("v_off"), v_m, v_m, Length(3.0, UnitOfMeasure::METRE), {});
  EXPECT_THAT(ExportToPROJ(v_off), HasSubstr("+dh=3"));
  auto v_off_comp = Transformation::createVerticalOffset(
      NameProps("v_off_c"), comp_crs, comp_crs,
      Length(3.0, UnitOfMeasure::METRE), {});
  EXPECT_THAT(ExportToPROJ(v_off_comp), HasSubstr("+dh=3"));

  auto g3d_to_grav = MakeTransf(
      v_m, v_m, nullptr, EPSG_NAME_METHOD_GEOGRAPHIC3D_TO_GRAVITYRELATEDHEIGHT,
      EPSG_CODE_METHOD_GEOGRAPHIC3D_TO_GRAVITYRELATEDHEIGHT,
      {MakeMeasureParamVal(EPSG_NAME_PARAMETER_GEOID_HEIGHT,
                           EPSG_CODE_PARAMETER_GEOID_HEIGHT, 4.0,
                           UnitOfMeasure::METRE)});
  EXPECT_THAT(ExportToPROJ(g3d_to_grav), HasSubstr("+zoff=-4"));
  EXPECT_THAT(ExportToPROJ(g3d_to_grav->inverse()),
              HasSubstr("+inv +proj=affine +zoff=-4"));

  auto g3d_to_2d_grav =
      MakeTransf(g3d, comp_crs, nullptr,
                 EPSG_NAME_METHOD_GEOGRAPHIC3D_TO_GEOG2D_GRAVITYRELATEDHEIGHT,
                 EPSG_CODE_METHOD_GEOGRAPHIC3D_TO_GEOG2D_GRAVITYRELATEDHEIGHT,
                 {MakeMeasureParamVal(EPSG_NAME_PARAMETER_GEOID_UNDULATION,
                                      EPSG_CODE_PARAMETER_GEOID_UNDULATION, 5.0,
                                      UnitOfMeasure::METRE)});
  EXPECT_THAT(ExportToPROJ(g3d_to_2d_grav), HasSubstr("+zoff=-5"));
  EXPECT_THAT(ExportToPROJ(g3d_to_2d_grav->inverse()),
              HasSubstr("+inv +proj=affine +zoff=-5"));

  auto vos = MakeTransf(
      v_m, v_m, &g2d_a, EPSG_NAME_METHOD_VERTICAL_OFFSET_AND_SLOPE,
      EPSG_CODE_METHOD_VERTICAL_OFFSET_AND_SLOPE,
      {
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_ORDINATE_1_EVAL_POINT,
                              EPSG_CODE_PARAMETER_ORDINATE_1_EVAL_POINT, 48.0,
                              UnitOfMeasure::DEGREE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_ORDINATE_2_EVAL_POINT,
                              EPSG_CODE_PARAMETER_ORDINATE_2_EVAL_POINT, 2.0,
                              UnitOfMeasure::DEGREE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_VERTICAL_OFFSET,
                              EPSG_CODE_PARAMETER_VERTICAL_OFFSET, 1.2,
                              UnitOfMeasure::METRE),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_INCLINATION_IN_LATITUDE,
                              EPSG_CODE_PARAMETER_INCLINATION_IN_LATITUDE, 0.5,
                              UnitOfMeasure::ARC_SECOND),
          MakeMeasureParamVal(EPSG_NAME_PARAMETER_INCLINATION_IN_LONGITUDE,
                              EPSG_CODE_PARAMETER_INCLINATION_IN_LONGITUDE, 0.6,
                              UnitOfMeasure::ARC_SECOND),
      });
  EXPECT_THAT(ExportToPROJ(vos), HasSubstr("+proj=vertoffset"));
}

// ---------------------------------------------------------------------------
// 7. exportToPROJStringGeneric: Grids, IGN, Velocity, Geoid, VERTCON,
//    Longitude Rotation, NZ Deformation, TIN, GTG, PROJ-based
// ---------------------------------------------------------------------------

TEST(SingleOperationTest, ExportToPROJStringGridAndVelocityMethods) {
  auto g1 = GeographicCRS::EPSG_4267;
  auto g2 = GeographicCRS::EPSG_4269;
  auto g3d = MakeGeog3D();
  auto v1 = MakeVertCRS("V1", UnitOfMeasure::METRE, /*depth=*/false);
  auto v_depth = MakeVertCRS("V_depth", UnitOfMeasure::METRE, /*depth=*/true);
  auto comp_crs = CompoundCRS::create(NameProps("Comp"), {g1, v1});

  // 1. NTv2, NTv1, CTABLE2, HORIZONTAL_SHIFT_GTIFF, GENERAL_SHIFT_GTIFF.
  auto ntv2 =
      Transformation::createNTv2(NameProps("ntv2"), g1, g2, "grid.gsb", {});
  EXPECT_THAT(ExportToPROJ(ntv2),
              HasSubstr("+proj=hgridshift +grids=grid.gsb"));
  EXPECT_THAT(ExportToPROJ(ntv2->inverse()),
              HasSubstr("+inv +proj=hgridshift"));

  auto gen_shift_2d = MakeTransf(
      g1, g2, nullptr, PROJ_WKT2_NAME_METHOD_GENERAL_SHIFT_GTIFF, 0,
      {MakeFileParamVal(
          // NOLINTNEXTLINE(whitespace/line_length)
          PROJ_WKT2_PARAMETER_LATITUDE_LONGITUDE_ELLIPOISDAL_HEIGHT_DIFFERENCE_FILE,
          0, "shift3d.tif")});
  EXPECT_THAT(ExportToPROJ(gen_shift_2d), HasSubstr("+no_z_transform"));

  auto gen_shift_3d = MakeTransf(
      g3d, g3d, nullptr, PROJ_WKT2_NAME_METHOD_GENERAL_SHIFT_GTIFF, 0,
      {MakeFileParamVal(
          // NOLINTNEXTLINE(whitespace/line_length)
          PROJ_WKT2_PARAMETER_LATITUDE_LONGITUDE_ELLIPOISDAL_HEIGHT_DIFFERENCE_FILE,
          0, "shift3d.tif")});
  EXPECT_THAT(ExportToPROJ(gen_shift_3d), HasSubstr("+proj=gridshift"));

  // 2. Geocentric translation by grid (IGN) (1087): interp==src, interp==dst,
  // inverse, and error branches.
  auto ign_src = MakeTransf(
      g1, g2, &g1,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "gr3df97a.txt")});
  EXPECT_THAT(ExportToPROJ(ign_src), HasSubstr("+proj=xyzgridshift"));
  EXPECT_THAT(ExportToPROJ(ign_src->inverse()),
              HasSubstr("+proj=xyzgridshift"));

  auto ign_dst = MakeTransf(
      g1, g2, &g2,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "gr3df97a.txt")});
  EXPECT_THAT(ExportToPROJ(ign_dst), HasSubstr("+inv"));

  auto ign_no_interp = MakeTransf(
      g1, g2, nullptr,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "gr3df97a.txt")});
  EXPECT_THROW(ExportToPROJ(ign_no_interp), FormattingException);

  // 3. Geographic3D offset by velocity grid (NTv2_Vel) (1114).
  auto dyn_g1 = MakeDynamicGeog3D("DynG1", 1997.0);
  auto dyn_g2 = MakeDynamicGeog3D("DynG2", 2010.0);
  auto dyn_g1_2d = dyn_g1->demoteTo2D(std::string(), nullptr);
  auto dyn_g2_2d = dyn_g2->demoteTo2D(std::string(), nullptr);
  auto vel_1114_src = MakeTransf(
      dyn_g1, dyn_g2, &dyn_g1_2d,
      EPSG_NAME_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel3d.tif")});
  EXPECT_THAT(ExportToPROJ(vel_1114_src), HasSubstr("+proj=deformation"));
  EXPECT_THAT(ExportToPROJ(vel_1114_src->inverse()),
              HasSubstr("+proj=deformation"));

  auto vel_1114_dst = MakeTransf(
      dyn_g1, dyn_g2, &dyn_g2_2d,
      EPSG_NAME_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel3d.tif")});
  EXPECT_THAT(ExportToPROJ(vel_1114_dst), HasSubstr("+proj=deformation"));

  // Missing anchorEpoch throws FormattingException.
  auto vel_1114_no_ep = MakeTransf(
      g3d, g3d, &g3d,
      EPSG_NAME_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel3d.tif")});
  EXPECT_THROW(ExportToPROJ(vel_1114_no_ep), FormattingException);

  // 4. Vertical Offset using NEU velocity grid (NTv2_Vel) (1113).
  auto dyn_v1 = MakeDynamicVertCRS("DynV1", 1997.0);
  auto dyn_v2 = MakeDynamicVertCRS("DynV2", 2010.0);
  auto vel_1113 = MakeTransf(
      dyn_v1, dyn_v2, &g1,
      EPSG_NAME_METHOD_VERTICAL_OFFSET_USING_NEU_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_VERTICAL_OFFSET_USING_NEU_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "velneu.tif")});
  EXPECT_THAT(ExportToPROJ(vel_1113), HasSubstr("+proj=deformation"));
  EXPECT_THAT(ExportToPROJ(vel_1113->inverse()),
              HasSubstr("+proj=deformation"));

  // 5. Height to Geographic3D & Geographic3D to GravityRelatedHeight (with UP
  // and DOWN axis directions), VERTCON (.gtx and .tif), Regular vertical grids.
  auto geoid_up = MakeTransf(
      comp_crs, g3d, nullptr, PROJ_WKT2_NAME_METHOD_HEIGHT_TO_GEOG3D, 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "egm96.gtx")});
  EXPECT_THAT(ExportToPROJ(geoid_up), HasSubstr("+proj=vgridshift"));
  EXPECT_THAT(ExportToPROJ(geoid_up->inverse()), HasSubstr("+proj=vgridshift"));

  auto comp_depth = CompoundCRS::create(NameProps("CompD"), {g1, v_depth});
  auto geoid_down = MakeTransf(
      comp_depth, comp_depth, nullptr,
      "Geographic3D to GravityRelatedHeight (gtx)", 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "egm96.gtx")});
  EXPECT_THAT(ExportToPROJ(geoid_down),
              HasSubstr("+proj=axisswap +order=1,2,-3"));
  EXPECT_THAT(ExportToPROJ(geoid_down->inverse()),
              HasSubstr("+proj=axisswap +order=1,2,-3"));

  auto vertcon_gtx = MakeTransf(
      v1, v1, nullptr, EPSG_NAME_METHOD_VERTCON, EPSG_CODE_METHOD_VERTCON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_VERTICAL_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_VERTICAL_OFFSET_FILE,
                        "vertconw.gtx")});
  EXPECT_THAT(ExportToPROJ(vertcon_gtx), HasSubstr("+multiplier=0.001"));

  auto vertcon_tif = MakeTransf(
      v1, v1, nullptr, EPSG_NAME_METHOD_VERTCON, EPSG_CODE_METHOD_VERTCON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_VERTICAL_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_VERTICAL_OFFSET_FILE,
                        "vertconw.tif")});
  EXPECT_THAT(ExportToPROJ(vertcon_tif), HasSubstr("+multiplier=1"));

  auto reg_nl =
      MakeTransf(v1, v1, nullptr, EPSG_NAME_METHOD_VERTICALGRID_NRCAN_BYN,
                 EPSG_CODE_METHOD_VERTICALGRID_NRCAN_BYN,
                 {MakeFileParamVal(EPSG_NAME_PARAMETER_VERTICAL_OFFSET_FILE,
                                   EPSG_CODE_PARAMETER_VERTICAL_OFFSET_FILE,
                                   "nlgeo.gtx")});
  EXPECT_THAT(ExportToPROJ(reg_nl), HasSubstr("+multiplier=-1"));

  auto reg_diff = MakeTransf(
      comp_crs, comp_crs, nullptr,
      EPSG_NAME_METHOD_VERTICALCHANGE_BY_GEOID_GRID_DIFFERENCE_NRCAN,
      EPSG_CODE_METHOD_VERTICALCHANGE_BY_GEOID_GRID_DIFFERENCE_NRCAN,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_MODEL_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_GEOID_MODEL_DIFFERENCE_FILE,
                        "diff.gtx")});
  EXPECT_THAT(ExportToPROJ(reg_diff), HasSubstr("+multiplier=-1"));
}

TEST(SingleOperationTest, ExportToPROJStringLonRotNZDefTINGTGAndPROJBased) {
  auto g1 = GeographicCRS::EPSG_4326;
  auto g_paris =
      MakeGeog2D("Paris CRS", UnitOfMeasure::DEGREE, 2.33722917, "Paris");
  auto g3d = MakeGeog3D();
  auto p_crs = MakeProjCRS();
  auto v_m = MakeVertCRS("V_m", UnitOfMeasure::METRE);
  auto gc = MakeGeocentric();

  // 1. Longitude rotation (9601): Paris PM and custom offset.
  auto lon_rot_paris = Transformation::createLongitudeRotation(
      NameProps("lon_rot_paris"), g_paris, g1,
      Angle(-2.337229166666667, UnitOfMeasure::DEGREE));
  EXPECT_THAT(ExportToPROJ(lon_rot_paris), HasSubstr("+pm=paris"));
  EXPECT_THROW(
      ExportToPROJ(lon_rot_paris, PROJStringFormatter::Convention::PROJ_4),
      FormattingException);

  auto lon_rot_custom = Transformation::createLongitudeRotation(
      NameProps("lon_rot_custom"), g1, g1, Angle(5.0, UnitOfMeasure::DEGREE));
  EXPECT_THAT(ExportToPROJ(lon_rot_custom), HasSubstr("+pm=5"));

  // 2. New Zealand Deformation Model (1079).
  auto nz_def = MakeTransf(
      g3d, g3d, nullptr, EPSG_NAME_METHOD_NEW_ZEALAND_DEFORMATION_MODEL,
      EPSG_CODE_METHOD_NEW_ZEALAND_DEFORMATION_MODEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "nz_linz_def.json")});
  CoordOpPeer::SetSourceEpoch(*nz_def,
                              DataEpoch(Measure(2000.0, UnitOfMeasure::YEAR)));
  EXPECT_THAT(ExportToPROJ(nz_def), HasSubstr("+inv +proj=defmodel"));
  auto nz_def_inv = MakeTransf(
      g3d, g3d, nullptr,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_NEW_ZEALAND_DEFORMATION_MODEL,
      EPSG_CODE_METHOD_NEW_ZEALAND_DEFORMATION_MODEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "nz_linz_def.json")});
  CoordOpPeer::SetTargetEpoch(*nz_def_inv,
                              DataEpoch(Measure(2000.0, UnitOfMeasure::YEAR)));
  EXPECT_THAT(ExportToPROJ(nz_def_inv), HasSubstr("+proj=defmodel"));

  // 3. TIN interpolation methods (Cartesian 1138, Geographic2D 1145, Vertical
  // 1137).
  auto tin_cart = MakeTransf(
      p_crs, p_crs, nullptr,
      EPSG_NAME_METHOD_CARTESIAN_GRID_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      EPSG_CODE_METHOD_CARTESIAN_GRID_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_TIN_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_TIN_OFFSET_FILE, "tin_c.json")});
  EXPECT_THAT(ExportToPROJ(tin_cart), HasSubstr("+proj=tinshift"));
  auto tin_cart_inv = MakeTransf(
      p_crs, p_crs, nullptr,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_CARTESIAN_GRID_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      EPSG_CODE_METHOD_CARTESIAN_GRID_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_TIN_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_TIN_OFFSET_FILE, "tin_c.json")});
  EXPECT_THAT(ExportToPROJ(tin_cart_inv), HasSubstr("+inv +proj=tinshift"));

  auto tin_geog = MakeTransf(
      g1, g1, nullptr,
      EPSG_NAME_METHOD_GEOGRAPHIC2D_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      EPSG_CODE_METHOD_GEOGRAPHIC2D_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_TIN_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_TIN_OFFSET_FILE, "tin_g.json")});
  EXPECT_THAT(ExportToPROJ(tin_geog), HasSubstr("+proj=tinshift"));

  auto g_grad = MakeGeog2D("G_grad", UnitOfMeasure::GRAD);
  auto tin_geog_grad_err = MakeTransf(
      g_grad, g_grad, nullptr,
      EPSG_NAME_METHOD_GEOGRAPHIC2D_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      EPSG_CODE_METHOD_GEOGRAPHIC2D_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_TIN_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_TIN_OFFSET_FILE, "tin_g.json")});
  EXPECT_THROW(ExportToPROJ(tin_geog_grad_err), FormattingException);

  auto tin_vert = MakeTransf(
      v_m, v_m, nullptr,
      EPSG_NAME_METHOD_VERTICAL_OFFSET_BY_TIN_INTERPOLATION_JSON,
      EPSG_CODE_METHOD_VERTICAL_OFFSET_BY_TIN_INTERPOLATION_JSON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_TIN_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_TIN_OFFSET_FILE, "tin_v.json")});
  EXPECT_THAT(ExportToPROJ(tin_vert), HasSubstr("+proj=tinshift"));

  // 4. GTG velocity methods (1143, 1144, 1142).
  auto gtg_1143_no_ep = MakeTransf(
      gc, gc, &g1,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_NAME_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel.tif")});
  EXPECT_THROW(ExportToPROJ(gtg_1143_no_ep), FormattingException);

  auto gtg_1143_t_only = MakeTransf(
      gc, gc, &g1,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_NAME_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel.tif"),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR)});
  EXPECT_THAT(ExportToPROJ(gtg_1143_t_only), HasSubstr("+proj=deformation"));
  auto gtg_1143_t_only_inv = MakeTransf(
      gc, gc, &g1,
      std::string("Inverse of ") +
          // NOLINTNEXTLINE(whitespace/line_length)
          EPSG_NAME_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel.tif"),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR)});
  EXPECT_THAT(ExportToPROJ(gtg_1143_t_only_inv),
              HasSubstr("+proj=deformation"));

  auto gtg_1143_both = MakeTransf(
      gc, gc, &g1,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_NAME_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel.tif"),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_SOURCE_EPOCH,
                           EPSG_CODE_PARAMETER_SOURCE_EPOCH, 2010.0,
                           UnitOfMeasure::YEAR),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR)});
  EXPECT_THAT(ExportToPROJ(gtg_1143_both), HasSubstr("+dt=10"));
  auto gtg_1143_both_inv = MakeTransf(
      gc, gc, &g1,
      std::string("Inverse of ") +
          // NOLINTNEXTLINE(whitespace/line_length)
          EPSG_NAME_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel.tif"),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_SOURCE_EPOCH,
                           EPSG_CODE_PARAMETER_SOURCE_EPOCH, 2010.0,
                           UnitOfMeasure::YEAR),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR)});
  EXPECT_THAT(ExportToPROJ(gtg_1143_both_inv), HasSubstr("+proj=deformation"));

  auto gtg_1144 = MakeTransf(
      gc, gc, &g1,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel.tif"),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR)});
  EXPECT_THAT(ExportToPROJ(gtg_1144), HasSubstr("+proj=deformation"));
  auto gtg_1144_inv = MakeTransf(
      gc, gc, &g1,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel.tif"),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR)});
  EXPECT_THAT(ExportToPROJ(gtg_1144_inv), HasSubstr("+proj=deformation"));

  auto gtg_1144_src_err = MakeTransf(
      gc, gc, &g1,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel.tif"),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_SOURCE_EPOCH,
                           EPSG_CODE_PARAMETER_SOURCE_EPOCH, 2010.0,
                           UnitOfMeasure::YEAR)});
  EXPECT_THROW(ExportToPROJ(gtg_1144_src_err), FormattingException);

  auto gtg_1142 = MakeTransf(
      gc, gc, &g1,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "grid.tif"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel.tif"),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_SOURCE_EPOCH,
                           EPSG_CODE_PARAMETER_SOURCE_EPOCH, 2010.0,
                           UnitOfMeasure::YEAR),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR)});
  EXPECT_THAT(ExportToPROJ(gtg_1142), HasSubstr("+proj=xyzgridshift"));
  auto gtg_1142_inv = MakeTransf(
      gc, gc, &g1,
      std::string("Inverse of ") +
          // NOLINTNEXTLINE(whitespace/line_length)
          EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "grid.tif"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel.tif"),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_SOURCE_EPOCH,
                           EPSG_CODE_PARAMETER_SOURCE_EPOCH, 2010.0,
                           UnitOfMeasure::YEAR),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR)});
  EXPECT_THAT(ExportToPROJ(gtg_1142_inv), HasSubstr("+proj=xyzgridshift"));

  // 5. "PROJ-based operation method: ..." (valid and invalid).
  auto proj_based_method_ok = MakeTransf(
      g1, g1, nullptr, "PROJ-based operation method: +proj=noop", 0, {});
  EXPECT_THAT(ExportToPROJ(proj_based_method_ok), HasSubstr("+proj=noop"));

  auto proj_based_method_bad = MakeTransf(
      g1, g1, nullptr,
      "PROJ-based operation method: +step +proj=noop +proj=pipeline", 0, {});
  EXPECT_THROW(ExportToPROJ(proj_based_method_bad), FormattingException);
}

// ---------------------------------------------------------------------------
// 8. PointMotionOperation & InverseCoordinateOperation
// ---------------------------------------------------------------------------

TEST(SingleOperationTest, PointMotionOperationAllMethodsAndExports) {
  ScopedSqliteDb mem_sql;
  mem_sql.InsertGridAlt("pmo_vel.byn", "ca_nrc_pmo.tif", "GTiff", 0);
  auto db_ctx = DatabaseContext::create(mem_sql.db);

  auto g3d = MakeGeog3D();
  auto gc = MakeGeocentric();
  auto v_m = MakeVertCRS();
  auto acc = PositionalAccuracy::create("0.05");

  auto p_vel = MakeParam(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                         EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE);
  auto v_vel = ParameterValue::createFilename("pmo_vel.byn");
  auto opv_vel = OperationParameterValue::create(p_vel, v_vel);
  auto method_1070 = OperationMethod::create(
      EpsgProps(EPSG_NAME_METHOD_POINT_MOTION_BY_GRID_CANADA_NTV2_VEL,
                EPSG_CODE_METHOD_POINT_MOTION_BY_GRID_CANADA_NTV2_VEL),
      std::vector<OperationParameterNNPtr>{p_vel});

  // 1. Inconsistent parameter/value count throws InvalidOperation.
  EXPECT_THROW(
      PointMotionOperation::create(
          NameProps("bad_pmo"), g3d,
          EpsgProps(EPSG_NAME_METHOD_POINT_MOTION_BY_GRID_CANADA_NTV2_VEL,
                    EPSG_CODE_METHOD_POINT_MOTION_BY_GRID_CANADA_NTV2_VEL),
          std::vector<OperationParameterNNPtr>{p_vel},
          std::vector<ParameterValueNNPtr>{}, {}),
      InvalidOperation);

  // 2. Create with name containing " from epoch ... to epoch ..." automatically
  // parses source and target epochs.
  auto pmo_props =
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY,
               "NAD83(CSRS) motion from epoch 2002.0 to epoch 2010.0")
          .set(CoordinateOperation::OPERATION_VERSION_KEY, "v1")
          .set(Identifier::CODESPACE_KEY, "EPSG")
          .set(Identifier::CODE_KEY, 9999)
          .set(ObjectUsage::SCOPE_KEY, "geodesy")
          .set(ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               Extent::createFromBBOX(-180, -90, 180, 90));
  auto pmo = PointMotionOperation::create(
      pmo_props, g3d, method_1070,
      std::vector<GeneralParameterValueNNPtr>{opv_vel}, {acc});
  ASSERT_TRUE(pmo->sourceCoordinateEpoch().has_value());
  ASSERT_TRUE(pmo->targetCoordinateEpoch().has_value());
  EXPECT_DOUBLE_EQ(2002.0,
                   pmo->sourceCoordinateEpoch()->coordinateEpoch().value());
  EXPECT_DOUBLE_EQ(2010.0,
                   pmo->targetCoordinateEpoch()->coordinateEpoch().value());
  EXPECT_EQ(g3d.get(), pmo->sourceCRS().get());

  // 3. Inverse updates name and swaps epochs.
  auto pmo_inv = pmo->inverse();
  EXPECT_THAT(pmo_inv->nameStr(), HasSubstr("from epoch 2010 to epoch 2002"));
  EXPECT_TRUE(pmo_inv->isEquivalentTo(pmo_inv.get()));

  // 4. substitutePROJAlternativeGridNames (1-param and 2-param with interp
  // CRS).
  auto pmo_sub = pmo->substitutePROJAlternativeGridNames(db_ctx);
  EXPECT_EQ("ca_nrc_pmo.tif", nn_dynamic_pointer_cast<OperationParameterValue>(
                                  pmo_sub->parameterValues()[0])
                                  ->parameterValue()
                                  ->valueFile());

  auto p_north =
      MakeParam(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_NORTH_GRID_FILE,
                EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_NORTH_GRID_FILE);
  auto p_interp =
      MakeParam(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS);
  auto pmo_2p = PointMotionOperation::create(
      NameProps("pmo_2p"), gc,
      OperationMethod::create(
          EpsgProps(
              // NOLINTNEXTLINE(whitespace/line_length)
              EPSG_NAME_METHOD_POINT_MOTION_GEOCEN_DOMAIN_USING_NEU_VELOCITY_GRID_GRAVSOFT,
              // NOLINTNEXTLINE(whitespace/line_length)
              EPSG_CODE_METHOD_POINT_MOTION_GEOCEN_DOMAIN_USING_NEU_VELOCITY_GRID_GRAVSOFT),
          std::vector<OperationParameterNNPtr>{p_north, p_interp}),
      std::vector<GeneralParameterValueNNPtr>{
          MakeFileParamVal(
              EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_NORTH_GRID_FILE,
              EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_NORTH_GRID_FILE,
              "pmo_vel.byn"),
          MakeIntParamVal(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                          EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                          4326)},
      {});
  auto pmo_2p_sub = pmo_2p->substitutePROJAlternativeGridNames(db_ctx);
  EXPECT_EQ(1u, pmo_2p_sub->parameterValues().size());

  // 5. shallowClone and cloneWithEpochs.
  auto pmo_clone = pmo->shallowClone();
  EXPECT_EQ(pmo->nameStr(), pmo_clone->nameStr());

  DataEpoch e1(Measure(1997.0, UnitOfMeasure::YEAR));
  DataEpoch e2(Measure(2020.0, UnitOfMeasure::YEAR));
  auto pmo_epochs = pmo_2p_sub->cloneWithEpochs(e1, e2);
  ASSERT_TRUE(pmo_epochs->sourceCoordinateEpoch().has_value());
  ASSERT_TRUE(pmo_epochs->targetCoordinateEpoch().has_value());

  // 6. WKT export (WKT1 and WKT2_2015 throw; WKT2_2019 succeeds).
  auto wkt1 = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THROW(pmo->_exportToWKT(wkt1.get()), FormattingException);
  auto wkt2_2015 = WKTFormatter::create(WKTFormatter::Convention::WKT2_2015);
  EXPECT_THROW(pmo->_exportToWKT(wkt2_2015.get()), FormattingException);

  auto wkt2_2019 = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
  pmo->_exportToWKT(wkt2_2019.get());
  EXPECT_THAT(wkt2_2019->toString(), HasSubstr("POINTMOTIONOPERATION["));
  EXPECT_THAT(wkt2_2019->toString(), HasSubstr("SOURCECRS["));
  EXPECT_THAT(wkt2_2019->toString(), HasSubstr("OPERATIONACCURACY[0.05]"));

  // 7. JSON export (named and unnamed).
  auto json1 = JSONFormatter::create();
  pmo->_exportToJSON(json1.get());
  EXPECT_THAT(json1->toString(), HasSubstr("\"PointMotionOperation\""));

  auto pmo_unnamed = PointMotionOperation::create(
      PropertyMap(), g3d, method_1070,
      std::vector<GeneralParameterValueNNPtr>{opv_vel}, {});
  auto json2 = JSONFormatter::create();
  pmo_unnamed->_exportToJSON(json2.get());
  EXPECT_THAT(json2->toString(), HasSubstr("\"unnamed\""));

  // 8. PROJ string export (GeographicCRS, GeocentricCRS, and error branches).
  EXPECT_THROW(ExportToPROJ(pmo, PROJStringFormatter::Convention::PROJ_4),
               FormattingException);
  EXPECT_THAT(ExportToPROJ(pmo), HasSubstr("+proj=deformation"));
  EXPECT_THAT(ExportToPROJ(pmo_epochs), HasSubstr("+proj=deformation"));

  // Missing epochs throws FormattingException.
  EXPECT_THROW(ExportToPROJ(pmo_unnamed), FormattingException);

  // Non-GeodeticCRS throws FormattingException.
  auto pmo_vert = PointMotionOperation::create(
      pmo_props, v_m, method_1070,
      std::vector<GeneralParameterValueNNPtr>{opv_vel}, {});
  EXPECT_THROW(ExportToPROJ(pmo_vert), FormattingException);

  // Unknown method throws FormattingException.
  auto pmo_bad_method = PointMotionOperation::create(
      pmo_props, g3d,
      OperationMethod::create(NameProps("Unknown PMO Method"),
                              std::vector<OperationParameterNNPtr>{}),
      std::vector<GeneralParameterValueNNPtr>{}, {});
  EXPECT_THROW(ExportToPROJ(pmo_bad_method), FormattingException);
}

// ---------------------------------------------------------------------------
// 9. Additional Coverage for InverseTransformation, Equivalence, and PROJ
// Export
// ---------------------------------------------------------------------------

TEST(SingleOperationTest, InverseTransformationAndCopyConstructorsCoverage) {
  auto src = GeographicCRS::EPSG_4326;
  auto dst = GeographicCRS::EPSG_4269;

  auto tr = Transformation::createGeocentricTranslations(
      NameProps("Helmert fwd"), src, dst, 1.0, 2.0, 3.0, {});
  DataEpoch ep_src(Measure(2010.0, UnitOfMeasure::YEAR));
  DataEpoch ep_dst(Measure(2020.0, UnitOfMeasure::YEAR));
  CoordOpPeer::SetSourceEpoch(*tr, ep_src);
  CoordOpPeer::SetTargetEpoch(*tr, ep_dst);

  // Base CoordinateOperation::shallowClone() delegates to _shallowClone().
  auto base_clone = static_cast<const CoordinateOperation&>(*tr).shallowClone();
  EXPECT_EQ(tr->nameStr(), base_clone->nameStr());

  // InverseTransformation::create exercises InverseTransformation constructor,
  // setPropertiesFromForward (with source/target epochs), _inverse,
  // _isEquivalentTo, and InverseCoordinateOperation::setCRSsUpdateInverse.
  auto inv_tr = ::osgeo::proj::operation::InverseTransformation::create(tr);
  ASSERT_TRUE(inv_tr->sourceCoordinateEpoch().has_value());
  ASSERT_TRUE(inv_tr->targetCoordinateEpoch().has_value());
  EXPECT_DOUBLE_EQ(2020.0,
                   inv_tr->sourceCoordinateEpoch()->coordinateEpoch().value());
  EXPECT_DOUBLE_EQ(2010.0,
                   inv_tr->targetCoordinateEpoch()->coordinateEpoch().value());

  auto fwd_again = inv_tr->inverse();
  EXPECT_EQ(tr.get(), fwd_again.get());

  EXPECT_TRUE(
      inv_tr->isEquivalentTo(inv_tr.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      inv_tr->isEquivalentTo(tr.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      inv_tr->isEquivalentTo(tr.get(), IComparable::Criterion::EQUIVALENT));

  auto tr2 = Transformation::createGeocentricTranslations(
      NameProps("Other Helmert"), src, dst, 9.0, 8.0, 7.0, {});
  auto inv_tr2 = ::osgeo::proj::operation::InverseTransformation::create(tr2);
  EXPECT_FALSE(
      inv_tr->isEquivalentTo(inv_tr2.get(), IComparable::Criterion::STRICT));

  // OperationParameterValue::_exportToWKT with MethodMapping in WKT1 mode:
  // EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP_MICHIGAN maps
  // EPSG_CODE_PARAMETER_ELLIPSOID_SCALE_FACTOR to wkt1_name == nullptr
  // (skipped) and EPSG_CODE_PARAMETER_LATITUDE_FALSE_ORIGIN to
  // "latitude_of_origin".
  auto conv_mich = Conversion::createLambertConicConformal_2SP_Michigan(
      NameProps("LCC 2SP Michigan"), Angle(41.5), Angle(-84.3666667),
      Angle(42.1), Angle(43.6666667), Length(600000.0), Length(0.0),
      Scale(1.0000382));
  auto proj_mich = ProjectedCRS::create(
      NameProps("NAD27 / Michigan South"), GeographicCRS::EPSG_4267, conv_mich,
      CartesianCS::createEastingNorthing(UnitOfMeasure::METRE));
  auto wkt1_mich = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THROW(proj_mich->exportToWKT(wkt1_mich.get()), FormattingException);

  auto p_lat_fo = MakeParam(EPSG_NAME_PARAMETER_LATITUDE_FALSE_ORIGIN,
                            EPSG_CODE_PARAMETER_LATITUDE_FALSE_ORIGIN);
  auto p_ellps_sf = MakeParam(EPSG_NAME_PARAMETER_ELLIPSOID_SCALE_FACTOR,
                              EPSG_CODE_PARAMETER_ELLIPSOID_SCALE_FACTOR);
  auto conv_geos_mich = Conversion::create(
      NameProps("Geos Mich"),
      EpsgProps(PROJ_WKT2_NAME_METHOD_GEOSTATIONARY_SATELLITE_SWEEP_X,
                EPSG_CODE_METHOD_LAMBERT_CONIC_CONFORMAL_2SP_MICHIGAN),
      std::vector<OperationParameterNNPtr>{p_lat_fo, p_ellps_sf},
      std::vector<ParameterValueNNPtr>{
          ParameterValue::create(Measure(41.5, UnitOfMeasure::DEGREE)),
          ParameterValue::create(
              Measure(1.0000382, UnitOfMeasure::SCALE_UNITY))});
  auto proj_geos_mich = ProjectedCRS::create(
      NameProps("Proj Geos Mich"), GeographicCRS::EPSG_4267, conv_geos_mich,
      CartesianCS::createEastingNorthing(UnitOfMeasure::METRE));
  auto wkt1_geos_mich =
      WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THAT(proj_geos_mich->exportToWKT(wkt1_geos_mich.get()),
              HasSubstr("latitude_of_origin"));
}

TEST(SingleOperationTest,
     MethodParameterAndSingleOperationEquivalenceBranches) {
  auto p1 = MakeParam(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                      EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN);
  auto p2 = MakeParam(EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
                      EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN);

  // OperationMethod::_isEquivalentTo: different param counts and STRICT param
  // ordering.
  auto m_1p =
      OperationMethod::create(EpsgProps(EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
                                        EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
                              std::vector<OperationParameterNNPtr>{p1});
  auto m_2p =
      OperationMethod::create(EpsgProps(EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
                                        EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
                              std::vector<OperationParameterNNPtr>{p1, p2});
  auto m_2p_same =
      OperationMethod::create(EpsgProps(EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
                                        EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
                              std::vector<OperationParameterNNPtr>{p1, p2});
  auto m_2p_swapped =
      OperationMethod::create(EpsgProps(EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
                                        EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
                              std::vector<OperationParameterNNPtr>{p2, p1});

  EXPECT_FALSE(
      m_1p->isEquivalentTo(m_2p.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_TRUE(
      m_2p->isEquivalentTo(m_2p_same.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      m_2p->isEquivalentTo(m_2p_swapped.get(), IComparable::Criterion::STRICT));

  // OperationParameterValue::_isEquivalentTo and
  // OperationParameter::_isEquivalentTo
  auto pv1 = MakeMeasureParamVal(EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                                 EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN,
                                 0.0, UnitOfMeasure::DEGREE);
  auto pv2 =
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN,
                          EPSG_CODE_PARAMETER_LONGITUDE_OF_NATURAL_ORIGIN, 0.0,
                          UnitOfMeasure::DEGREE);
  EXPECT_FALSE(pv1->isEquivalentTo(p1.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(pv1->isEquivalentTo(pv2.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(p1->isEquivalentTo(pv1.get(), IComparable::Criterion::STRICT));

  // Azimuth parameter with non-angle (string) value in EQUIVALENT mode.
  auto pv_az1 = MakeStringParamVal(
      EPSG_NAME_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
      EPSG_CODE_PARAMETER_AZIMUTH_PROJECTION_CENTRE, "str_a");
  auto pv_az2 = MakeStringParamVal(
      EPSG_NAME_PARAMETER_AZIMUTH_PROJECTION_CENTRE,
      EPSG_CODE_PARAMETER_AZIMUTH_PROJECTION_CENTRE, "str_b");
  EXPECT_FALSE(
      pv_az1->isEquivalentTo(pv_az2.get(), IComparable::Criterion::EQUIVALENT));

  // SingleOperation::parameterValueMeasure when parameter is not found.
  auto utm = Conversion::createUTM(NameProps("UTM 31N"), 31, true);
  EXPECT_EQ(UnitOfMeasure::NONE,
            utm->parameterValueMeasure("NoSuchParam", 0).unit());

  // SingleOperation::_isEquivalentTo: non-SingleOperation and different names
  // in STRICT mode.
  EXPECT_FALSE(utm->isEquivalentTo(p1.get(), IComparable::Criterion::STRICT));
  auto utm_other_name =
      Conversion::createUTM(NameProps("UTM 31N alt"), 31, true);
  EXPECT_FALSE(utm->isEquivalentTo(utm_other_name.get(),
                                   IComparable::Criterion::STRICT));

  // SingleOperation::_isEquivalentTo in STRICT and EQUIVALENT modes for
  // different parameter counts, matching parameters, differing values, and
  // extra non-measure parameter.
  auto tm_1p_a = Conversion::create(
      NameProps("TM"),
      EpsgProps(EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
                EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
      std::vector<OperationParameterNNPtr>{p1},
      std::vector<ParameterValueNNPtr>{
          ParameterValue::create(Measure(10.0, UnitOfMeasure::DEGREE))});
  auto tm_1p_b = Conversion::create(
      NameProps("TM"),
      EpsgProps(EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
                EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
      std::vector<OperationParameterNNPtr>{p1},
      std::vector<ParameterValueNNPtr>{
          ParameterValue::create(Measure(20.0, UnitOfMeasure::DEGREE))});
  auto tm_2p = Conversion::create(
      NameProps("TM"),
      EpsgProps(EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
                EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
      std::vector<OperationParameterNNPtr>{p1, p2},
      std::vector<ParameterValueNNPtr>{
          ParameterValue::create(Measure(10.0, UnitOfMeasure::DEGREE)),
          ParameterValue::create(Measure(0.0, UnitOfMeasure::DEGREE))});

  EXPECT_FALSE(
      tm_1p_a->isEquivalentTo(tm_2p.get(), IComparable::Criterion::STRICT));
  EXPECT_TRUE(
      tm_1p_a->isEquivalentTo(tm_1p_a.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      tm_1p_a->isEquivalentTo(tm_1p_b.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(tm_1p_a->isEquivalentTo(tm_1p_b.get(),
                                       IComparable::Criterion::EQUIVALENT));

  auto p_extra = MakeParam("extra_str_param");
  auto tm_with_str = Conversion::create(
      NameProps("TM"),
      EpsgProps(EPSG_NAME_METHOD_TRANSVERSE_MERCATOR,
                EPSG_CODE_METHOD_TRANSVERSE_MERCATOR),
      std::vector<OperationParameterNNPtr>{p1, p_extra},
      std::vector<ParameterValueNNPtr>{
          ParameterValue::create(Measure(10.0, UnitOfMeasure::DEGREE)),
          ParameterValue::create("custom_str")});
  EXPECT_FALSE(tm_with_str->isEquivalentTo(tm_1p_a.get(),
                                           IComparable::Criterion::EQUIVALENT));

  // Two LCC 2SP conversions with non-swapped different standard parallels where
  // one is attached to a ProjectedCRS (exercises the fallback comparison to
  // create_projected_crs_from_this_and_compare when both are already LCC 2SP).
  auto lcc_2sp_a = Conversion::createLambertConicConformal_2SP(
      NameProps("LCC 2SP A"), Angle(40.0), Angle(0.0), Angle(45.0), Angle(45.0),
      Length(0.0), Length(0.0));
  auto lcc_2sp_b = Conversion::createLambertConicConformal_2SP(
      NameProps("LCC 2SP B"), Angle(40.0), Angle(0.0), Angle(44.0), Angle(49.0),
      Length(0.0), Length(0.0));
  auto proj_lcc_b = ProjectedCRS::create(
      NameProps("Proj LCC 2SP B"), GeographicCRS::EPSG_4326, lcc_2sp_b,
      CartesianCS::createEastingNorthing(UnitOfMeasure::METRE));
  EXPECT_FALSE(lcc_2sp_a->isEquivalentTo(proj_lcc_b->derivingConversion().get(),
                                         IComparable::Criterion::EQUIVALENT));
}

TEST(SingleOperationTest, GridsNeededAndGridSubstitutionExtraBranches) {
  ScopedContext ctx = MakeContext();
  auto db_ctx_default = DatabaseContext::create(std::string(), {}, ctx.get());
  auto g1 = GeographicCRS::EPSG_4267;
  auto g2 = GeographicCRS::EPSG_4269;
  auto g3d = MakeGeog3D();
  auto v1 = MakeVertCRS("V1");
  auto v2 = MakeVertCRS("V2");
  auto gc1 = MakeGeocentric("GC1");
  auto gc2 = MakeGeocentric("GC2");

  // 1. gridsNeeded on a non-PROJBasedOperation Transformation.
  auto tr_ntv2 = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV2, EPSG_CODE_METHOD_NTV2,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "nonexistent_ntv2_grid.gsb")});
  auto grids = tr_ntv2->gridsNeeded(db_ctx_default.as_nullable(), false);
  ASSERT_EQ(1u, grids.size());
  EXPECT_THAT(grids.begin()->shortName, HasSubstr("nonexistent_ntv2_grid.gsb"));

  // 2. getHeightToGeographic3DFilename with EPSG method 9665 (both with and
  // without parameter 8666).
  auto tr_geoid_9665 = MakeTransf(
      g3d, v1, nullptr, "Geographic3D to GravityRelatedHeight (gtx)", 9665,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "my_geoid.gtx")});
  EXPECT_EQ("my_geoid.gtx", tr_geoid_9665->getHeightToGeographic3DFilename());

  auto tr_geoid_9665_no_param = MakeTransf(
      g3d, v1, nullptr, "Geographic3D to GravityRelatedHeight (gtx)", 9665, {});
  EXPECT_EQ("", tr_geoid_9665_no_param->getHeightToGeographic3DFilename());

  // 3. ExportToPROJ on CTABLE2 and HORIZONTAL_SHIFT_GTIFF (with and without
  // file parameter).
  auto tr_ct2 = MakeTransf(
      g1, g2, nullptr, PROJ_WKT2_NAME_METHOD_CTABLE2, 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "conus_ct2.bin")});
  EXPECT_THAT(ExportToPROJ(tr_ct2), HasSubstr("+grids=conus_ct2.bin"));

  auto tr_ct2_empty =
      MakeTransf(g1, g2, nullptr, PROJ_WKT2_NAME_METHOD_CTABLE2, 0, {});
  EXPECT_THROW(ExportToPROJ(tr_ct2_empty), FormattingException);

  auto tr_hshift = MakeTransf(
      g1, g2, nullptr, PROJ_WKT2_NAME_METHOD_HORIZONTAL_SHIFT_GTIFF, 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "hshift.tif")});
  EXPECT_THAT(ExportToPROJ(tr_hshift), HasSubstr("+grids=hshift.tif"));

  auto tr_hshift_empty = MakeTransf(
      g1, g2, nullptr, PROJ_WKT2_NAME_METHOD_HORIZONTAL_SHIFT_GTIFF, 0, {});
  EXPECT_THROW(ExportToPROJ(tr_hshift_empty), FormattingException);

  // 4. substitutePROJAlternativeGridNames: remarks preservation, identity
  // grid names (orig == proj_name with inv=0 and inv=1), NTv1 forward to NTv1,
  // and EPSG_CODE_PARAMETER_GEOID_MODEL_DIFFERENCE_FILE.
  ScopedSqliteDb mem_sql;
  mem_sql.InsertGridAlt("same_ntv2_fwd.gsb", "same_ntv2_fwd.gsb", "NTv2", 0);
  mem_sql.InsertGridAlt("same_ntv2_inv.gsb", "same_ntv2_inv.gsb", "NTv2", 1);
  mem_sql.InsertGridAlt("ntv1_fwd_ntv1.dac", "ca_ntv1_fwd.dac", "NTv1", 0);
  mem_sql.InsertGridAlt("same_geoid.gtx", "same_geoid.gtx", "GTX", 0);
  mem_sql.InsertGridAlt("same_ign.txt", "same_ign.txt", "IGN", 0);
  mem_sql.InsertGridAlt("same_vel3d.vel", "same_vel3d.vel", "GTiff", 0);
  mem_sql.InsertGridAlt("vel3d_inv.vel", "ca_vel3d_inv.tif", "GTiff", 1);
  mem_sql.InsertGridAlt("same_velneu.vel", "same_velneu.vel", "GTiff", 0);
  mem_sql.InsertGridAlt("velneu_inv.vel", "ca_velneu_inv.tif", "GTiff", 1);
  mem_sql.InsertGridAlt("geoid_diff.txt", "geoid_diff.gtx", "GTX", 0);
  mem_sql.InsertGridAlt("same_reg_vert.txt", "same_reg_vert.txt", "GTX", 0);
  mem_sql.InsertGridAlt("same_gtg_vel.gtg", "same_gtg_vel.gtg", "GTiff", 0);
  mem_sql.InsertGridAlt("gtg_vel_inv.gtg", "fr_gtg_vel_inv.tif", "GTiff", 1);
  mem_sql.InsertGridAlt("same_gtg_grid.gtg", "same_gtg_grid.gtg", "GTiff", 0);
  mem_sql.InsertGridAlt("gtg_grid_inv.gtg", "fr_gtg_grid_inv.tif", "GTiff", 1);
  auto db_ctx = DatabaseContext::create(mem_sql.db);

  auto props_with_remarks =
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "op_remarks")
          .set(IdentifiedObject::REMARKS_KEY, "my remark");

  auto ntv2_same_fwd = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV2, EPSG_CODE_METHOD_NTV2,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "same_ntv2_fwd.gsb")},
      props_with_remarks);
  EXPECT_EQ(ntv2_same_fwd.get(),
            ntv2_same_fwd->substitutePROJAlternativeGridNames(db_ctx).get());

  auto ntv2_same_inv = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV2, EPSG_CODE_METHOD_NTV2,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "same_ntv2_inv.gsb")},
      props_with_remarks);
  EXPECT_THROW(ntv2_same_inv->substitutePROJAlternativeGridNames(db_ctx),
               UnsupportedOperationException);

  auto ntv1_fwd_ntv1 = MakeTransf(
      g1, g2, nullptr, EPSG_NAME_METHOD_NTV1, EPSG_CODE_METHOD_NTV1,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "ntv1_fwd_ntv1.dac")},
      props_with_remarks);
  auto sub_ntv1_fwd = ntv1_fwd_ntv1->substitutePROJAlternativeGridNames(db_ctx);
  EXPECT_EQ(EPSG_CODE_METHOD_NTV1, sub_ntv1_fwd->method()->getEPSGCode());
  EXPECT_EQ("my remark", sub_ntv1_fwd->remarks());

  auto geoid_same = MakeTransf(
      g3d, v1, nullptr, "Geographic3D to GravityRelatedHeight (OSGM15)", 9665,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "same_geoid.gtx")});
  EXPECT_EQ(geoid_same.get(),
            geoid_same->substitutePROJAlternativeGridNames(db_ctx).get());

  auto ign_same = MakeTransf(
      g1, g2, nullptr,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "same_ign.txt")});
  EXPECT_EQ(ign_same.get(),
            ign_same->substitutePROJAlternativeGridNames(db_ctx).get());

  auto vel3d_same = MakeTransf(
      g3d, g3d, nullptr,
      EPSG_NAME_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "same_vel3d.vel")});
  EXPECT_EQ(vel3d_same.get(),
            vel3d_same->substitutePROJAlternativeGridNames(db_ctx).get());

  auto vel3d_inv = MakeTransf(
      g3d, g3d, nullptr,
      EPSG_NAME_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel3d_inv.vel")});
  EXPECT_THROW(vel3d_inv->substitutePROJAlternativeGridNames(db_ctx),
               UnsupportedOperationException);

  auto velneu_same = MakeTransf(
      v1, v2, nullptr,
      EPSG_NAME_METHOD_VERTICAL_OFFSET_USING_NEU_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_VERTICAL_OFFSET_USING_NEU_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "same_velneu.vel")});
  EXPECT_EQ(velneu_same.get(),
            velneu_same->substitutePROJAlternativeGridNames(db_ctx).get());

  auto velneu_inv = MakeTransf(
      v1, v2, nullptr,
      EPSG_NAME_METHOD_VERTICAL_OFFSET_USING_NEU_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_VERTICAL_OFFSET_USING_NEU_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "velneu_inv.vel")});
  EXPECT_THROW(velneu_inv->substitutePROJAlternativeGridNames(db_ctx),
               UnsupportedOperationException);

  auto reg_vert_geoid_diff = MakeTransf(
      v1, v2, nullptr, "Vertical Offset by Grid Interpolation (NRCan)", 1088,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_MODEL_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_GEOID_MODEL_DIFFERENCE_FILE,
                        "geoid_diff.txt")});
  EXPECT_EQ("Vertical Offset by Grid Interpolation (NRCan)",
            reg_vert_geoid_diff->substitutePROJAlternativeGridNames(db_ctx)
                ->method()
                ->nameStr());

  auto reg_vert_same = MakeTransf(
      v1, v2, nullptr, "Vertical Offset by Grid Interpolation (NRCan)", 1088,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_VERTICAL_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_VERTICAL_OFFSET_FILE,
                        "same_reg_vert.txt")});
  EXPECT_EQ(reg_vert_same.get(),
            reg_vert_same->substitutePROJAlternativeGridNames(db_ctx).get());

  auto gtg_1143_same = MakeTransf(
      gc1, gc2, nullptr,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "same_gtg_vel.gtg")});
  EXPECT_EQ(gtg_1143_same.get(),
            gtg_1143_same->substitutePROJAlternativeGridNames(db_ctx).get());

  auto gtg_1143_inv = MakeTransf(
      gc1, gc2, nullptr,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "gtg_vel_inv.gtg")});
  EXPECT_THROW(gtg_1143_inv->substitutePROJAlternativeGridNames(db_ctx),
               UnsupportedOperationException);

  auto gtg_1142_same = MakeTransf(
      gc1, gc2, nullptr,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "same_gtg_grid.gtg"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "same_gtg_vel.gtg")});
  EXPECT_EQ(gtg_1142_same.get(),
            gtg_1142_same->substitutePROJAlternativeGridNames(db_ctx).get());

  auto gtg_1142_inv = MakeTransf(
      gc1, gc2, nullptr,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "gtg_grid_inv.gtg"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "gtg_vel_inv.gtg")});
  EXPECT_THROW(gtg_1142_inv->substitutePROJAlternativeGridNames(db_ctx),
               UnsupportedOperationException);
}

TEST(SingleOperationTest, ExportToPROJAndWKTRemainingTransformationsCoverage) {
  ScopedContext ctx = MakeContext();
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  auto g1 = GeographicCRS::EPSG_4326;
  auto g2 = GeographicCRS::EPSG_4269;
  auto g3d = MakeGeog3D();
  auto v_m = MakeVertCRS("VertM", UnitOfMeasure::METRE);
  auto comp1 = CompoundCRS::create(NameProps("Comp1"), {g1, v_m});

  // 1. BoundCRS export to WKT when transformation already contains
  // EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS (skips adding
  // duplicate).
  auto interp = GeographicCRS::create(
      EpsgProps("ETRS89", 4258), GeodeticReferenceFrame::EPSG_6326,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  auto tr_with_interp_param = MakeTransf(
      g1, g2, &interp,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "gr3df97a.txt"),
       MakeIntParamVal(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                       EPSG_CODE_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS,
                       4258)});
  auto bound_dup = BoundCRS::create(g1, g2, tr_with_interp_param);
  auto wkt2 = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
  bound_dup->_exportToWKT(wkt2.get());
  EXPECT_THAT(wkt2->toString(),
              HasSubstr(EPSG_NAME_PARAMETER_EPSG_CODE_FOR_INTERPOLATION_CRS));

  // 2. extractGeographicCRSFromSourceAndTarget & setupPROJGeodetic:
  // BoundCRS of GeographicCRS, CompoundCRS of BoundCRS + VerticalCRS, and
  // DerivedGeographicCRS.
  auto hub_tr = Transformation::createGeocentricTranslations(
      NameProps("hub"), g1, g2, 0.0, 0.0, 0.0, {});
  auto bound_g1 = BoundCRS::create(g1, g2, hub_tr);
  auto comp_bound =
      CompoundCRS::create(NameProps("CompBound"), {bound_g1, v_m});

  std::vector<OperationParameterValueNNPtr> geoc_tr_params{
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_X_AXIS_TRANSLATION,
                          EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION, 1.0,
                          UnitOfMeasure::METRE),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_Y_AXIS_TRANSLATION,
                          EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION, 2.0,
                          UnitOfMeasure::METRE),
      MakeMeasureParamVal(EPSG_NAME_PARAMETER_Z_AXIS_TRANSLATION,
                          EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION, 3.0,
                          UnitOfMeasure::METRE)};

  auto tr_bound = MakeTransf(
      bound_g1, comp_bound, nullptr,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D, geoc_tr_params);
  EXPECT_THAT(ExportToPROJ(tr_bound), HasSubstr("+proj=helmert"));

  auto tr_comp_to_3d = MakeTransf(
      comp1, g3d, nullptr,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D, geoc_tr_params);
  EXPECT_THAT(ExportToPROJ(tr_comp_to_3d), HasSubstr("+proj=helmert"));

  auto tr_3d_to_comp = MakeTransf(
      g3d, comp1, nullptr,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D, geoc_tr_params);
  DataEpoch ep_only_src(Measure(2015.0, UnitOfMeasure::YEAR));
  CoordOpPeer::SetSourceEpoch(*tr_3d_to_comp, ep_only_src);
  EXPECT_THAT(ExportToPROJ(tr_3d_to_comp), HasSubstr("+proj=helmert"));

  // DerivedGeographicCRS exercises lines 3180-3191 and 3225-3236 in
  // setupPROJGeodeticSourceCRS / setupPROJGeodeticTargetCRS.
  auto rot_conv = Conversion::createPoleRotationGRIBConvention(
      NameProps("pole_rot"), Angle(30.0), Angle(10.0), Angle(0.0));
  auto derived_geog = ::osgeo::proj::crs::DerivedGeographicCRS::create(
      NameProps("DerivedGeog"), g1, rot_conv,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  auto tr_derived = MakeTransf(
      derived_geog, derived_geog, nullptr,
      EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D, geoc_tr_params);
  EXPECT_THAT(ExportToPROJ(tr_derived), HasSubstr("+proj=helmert"));

  // 3. Axis Order Reversal (9843 & 9844) with custom angular unit.
  UnitOfMeasure custom_ang("custom_ang", 0.5, UnitOfMeasure::Type::ANGULAR);
  auto g_custom_ang = MakeGeog2D("CustomAng2D", custom_ang);
  auto g_deg_2d = MakeGeog2D("Deg2D", UnitOfMeasure::DEGREE);
  auto g3d_custom_ang =
      MakeGeog3D("CustomAng3D", custom_ang, UnitOfMeasure::METRE);
  auto op_9843 = MakeTransf(g_custom_ang, g_deg_2d, nullptr,
                            EPSG_NAME_METHOD_AXIS_ORDER_REVERSAL_2D,
                            EPSG_CODE_METHOD_AXIS_ORDER_REVERSAL_2D, {});
  EXPECT_THAT(ExportToPROJ(op_9843), HasSubstr("+proj=axisswap"));

  auto op_9844 = MakeTransf(g3d_custom_ang, g3d, nullptr,
                            EPSG_NAME_METHOD_AXIS_ORDER_REVERSAL_3D,
                            EPSG_CODE_METHOD_AXIS_ORDER_REVERSAL_3D, {});
  EXPECT_THAT(ExportToPROJ(op_9844), HasSubstr("+proj=axisswap"));

  // 4. Geographic/Geocentric conversions with CompoundCRS.
  auto gc = MakeGeocentric();
  auto tr_geog_geoc_comp1 =
      MakeTransf(comp1, gc, nullptr, EPSG_NAME_METHOD_GEOGRAPHIC_GEOCENTRIC,
                 EPSG_CODE_METHOD_GEOGRAPHIC_GEOCENTRIC, {});
  EXPECT_THAT(ExportToPROJ(tr_geog_geoc_comp1), HasSubstr("+proj=cart"));

  auto tr_geog_geoc_comp2 =
      MakeTransf(gc, comp1, nullptr, EPSG_NAME_METHOD_GEOGRAPHIC_GEOCENTRIC,
                 EPSG_CODE_METHOD_GEOGRAPHIC_GEOCENTRIC, {});
  EXPECT_THAT(ExportToPROJ(tr_geog_geoc_comp2), HasSubstr("+proj=cart"));

  // 5. Geoid height (method_code = 0, name starting with "Inverse of
  // Geographic3D to GravityRelatedHeight") with and without DatabaseContext.
  auto tr_inv_geoid = MakeTransf(
      g3d, v_m, nullptr,
      "Inverse of Geographic3D to GravityRelatedHeight (OSGM15)", 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "egm96_15.gtx")});
  EXPECT_THAT(ExportToPROJ(tr_inv_geoid), HasSubstr("+proj=vgridshift"));
  auto fmt_with_db = PROJStringFormatter::create(
      PROJStringFormatter::Convention::PROJ_5, db_ctx.as_nullable());
  EXPECT_THAT(tr_inv_geoid->exportToPROJString(fmt_with_db.get()),
              HasSubstr("+proj=vgridshift"));

  // Geoid height with reversed CRSs (v_m -> g3d) throws FormattingException.
  auto tr_geoid_rev = MakeTransf(
      v_m, g3d, nullptr, "Geographic3D to GravityRelatedHeight (OSGM15)", 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "egm96_15.gtx")});
  EXPECT_THROW(ExportToPROJ(tr_geoid_rev), FormattingException);

  // 6. Cartesian Grid Offsets with EngineeringCRS + VerticalCS
  // (non-CartesianCS).
  auto eng_vert = EngineeringCRS::create(
      NameProps("EngVert"),
      EngineeringDatum::create(NameProps("EngDatum"), optional<std::string>()),
      VerticalCS::createGravityRelatedHeight(UnitOfMeasure::METRE));
  auto tr_cart_bad_cs = MakeTransf(
      eng_vert, eng_vert, nullptr, EPSG_NAME_METHOD_CARTESIAN_GRID_OFFSETS,
      EPSG_CODE_METHOD_CARTESIAN_GRID_OFFSETS,
      {MakeMeasureParamVal(EPSG_NAME_PARAMETER_EASTING_OFFSET,
                           EPSG_CODE_PARAMETER_EASTING_OFFSET, 1.0,
                           UnitOfMeasure::METRE),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_NORTHING_OFFSET,
                           EPSG_CODE_PARAMETER_NORTHING_OFFSET, 2.0,
                           UnitOfMeasure::METRE)});
  EXPECT_THROW(ExportToPROJ(tr_cart_bad_cs), FormattingException);

  // 7. Geographic2D Offsets with BoundCRS, and Vertical Offset & Slope with
  // CompoundCRS.
  auto bound_g2 = BoundCRS::create(g2, g1, hub_tr);
  auto tr_g2d_off_bound = MakeTransf(
      bound_g1, bound_g2, nullptr, EPSG_NAME_METHOD_GEOGRAPHIC2D_OFFSETS,
      EPSG_CODE_METHOD_GEOGRAPHIC2D_OFFSETS,
      {MakeMeasureParamVal(EPSG_NAME_PARAMETER_LATITUDE_OFFSET,
                           EPSG_CODE_PARAMETER_LATITUDE_OFFSET, 0.1,
                           UnitOfMeasure::DEGREE),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_LONGITUDE_OFFSET,
                           EPSG_CODE_PARAMETER_LONGITUDE_OFFSET, 0.2,
                           UnitOfMeasure::DEGREE)});
  EXPECT_THAT(ExportToPROJ(tr_g2d_off_bound), HasSubstr("+proj=pipeline"));

  auto tr_vos_comp = MakeTransf(
      comp1, comp1, &g1, EPSG_NAME_METHOD_VERTICAL_OFFSET_AND_SLOPE,
      EPSG_CODE_METHOD_VERTICAL_OFFSET_AND_SLOPE,
      {MakeMeasureParamVal(EPSG_NAME_PARAMETER_ORDINATE_1_EVAL_POINT,
                           EPSG_CODE_PARAMETER_ORDINATE_1_EVAL_POINT, 48.0,
                           UnitOfMeasure::DEGREE),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_ORDINATE_2_EVAL_POINT,
                           EPSG_CODE_PARAMETER_ORDINATE_2_EVAL_POINT, 2.0,
                           UnitOfMeasure::DEGREE),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_VERTICAL_OFFSET,
                           EPSG_CODE_PARAMETER_VERTICAL_OFFSET, 1.0,
                           UnitOfMeasure::METRE),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_INCLINATION_IN_LATITUDE,
                           EPSG_CODE_PARAMETER_INCLINATION_IN_LATITUDE, 0.0,
                           UnitOfMeasure::ARC_SECOND),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_INCLINATION_IN_LONGITUDE,
                           EPSG_CODE_PARAMETER_INCLINATION_IN_LONGITUDE, 0.0,
                           UnitOfMeasure::ARC_SECOND)});
  EXPECT_THAT(ExportToPROJ(tr_vos_comp), HasSubstr("+proj=vertoffset"));

  // 8. Longitude Rotation with different ellipsoids throws FormattingException.
  auto g_sphere = MakeGeog2D("SphereGeog", UnitOfMeasure::DEGREE, 0.0,
                             "Greenwich", /*spherical=*/true);
  auto tr_lon_rot_diff_ellps =
      MakeTransf(g1, g_sphere, nullptr, EPSG_NAME_METHOD_LONGITUDE_ROTATION,
                 EPSG_CODE_METHOD_LONGITUDE_ROTATION,
                 {MakeMeasureParamVal(EPSG_NAME_PARAMETER_LONGITUDE_OFFSET,
                                      EPSG_CODE_PARAMETER_LONGITUDE_OFFSET, 2.0,
                                      UnitOfMeasure::DEGREE)});
  EXPECT_THROW(ExportToPROJ(tr_lon_rot_diff_ellps), FormattingException);

  // 9. "Inverse of <method_name>" with method_code = 0 in
  // exportToPROJStringGeneric for grid-based methods.
  auto tr_inv_ntv2 = MakeTransf(
      g1, g2, nullptr, std::string("Inverse of ") + EPSG_NAME_METHOD_NTV2, 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        EPSG_CODE_PARAMETER_LATITUDE_LONGITUDE_DIFFERENCE_FILE,
                        "ntv2.gsb")});
  EXPECT_THAT(ExportToPROJ(tr_inv_ntv2), HasSubstr("+proj=hgridshift"));

  auto tr_inv_ign = MakeTransf(
      g1, g2, &g1,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_GEOG2D_DOMAIN_BY_GRID_IGN,
      0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "gr3df97a.txt")});
  EXPECT_THAT(ExportToPROJ(tr_inv_ign), HasSubstr("+grids=gr3df97a.txt"));

  auto dyn_g3d_src = MakeDynamicGeog3D("DynSrc", 2010.0);
  auto dyn_g3d_dst = MakeDynamicGeog3D("DynDst", 2020.0);
  auto dyn_g2d_src = dyn_g3d_src->demoteTo2D("DynSrc2D", nullptr);
  auto tr_inv_vel3d = MakeTransf(
      dyn_g3d_src, dyn_g3d_dst, &dyn_g2d_src,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_GEOGRAPHIC3D_OFFSET_BY_VELOCITY_GRID_NTV2_VEL,
      0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "vel3d.tif")});
  EXPECT_THAT(ExportToPROJ(tr_inv_vel3d), HasSubstr("+proj=deformation"));

  auto v_ep1 = VerticalCRS::create(
      NameProps("VertEp1"),
      VerticalReferenceFrame::create(
          NameProps("VD1"), optional<std::string>(),
          optional<Measure>(Measure(2010.0, UnitOfMeasure::YEAR))),
      VerticalCS::createGravityRelatedHeight(UnitOfMeasure::METRE));
  auto v_ep2 = VerticalCRS::create(
      NameProps("VertEp2"),
      VerticalReferenceFrame::create(
          NameProps("VD2"), optional<std::string>(),
          optional<Measure>(Measure(2020.0, UnitOfMeasure::YEAR))),
      VerticalCS::createGravityRelatedHeight(UnitOfMeasure::METRE));
  auto tr_inv_velneu = MakeTransf(
      v_ep1, v_ep2, &g1,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_VERTICAL_OFFSET_USING_NEU_VELOCITY_GRID_NTV2_VEL,
      EPSG_CODE_METHOD_VERTICAL_OFFSET_USING_NEU_VELOCITY_GRID_NTV2_VEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "velneu.tif")});
  EXPECT_THAT(ExportToPROJ(tr_inv_velneu), HasSubstr("+proj=deformation"));

  auto tr_inv_h2g3d = MakeTransf(
      v_m, g3d, nullptr,
      std::string("Inverse of ") + PROJ_WKT2_NAME_METHOD_HEIGHT_TO_GEOG3D, 0,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_GEOID_CORRECTION_FILENAME,
                        EPSG_CODE_PARAMETER_GEOID_CORRECTION_FILENAME,
                        "egm96_15.gtx")});
  EXPECT_THAT(ExportToPROJ(tr_inv_h2g3d), HasSubstr("+proj=vgridshift"));

  auto tr_inv_nz = MakeTransf(
      g3d, g3d, nullptr,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_NEW_ZEALAND_DEFORMATION_MODEL,
      EPSG_CODE_METHOD_NEW_ZEALAND_DEFORMATION_MODEL,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "nz_def.json")});
  EXPECT_THAT(ExportToPROJ(tr_inv_nz), HasSubstr("+proj=defmodel"));

  auto proj_crs_a = ProjectedCRS::create(
      NameProps("ProjA"), g1,
      Conversion::createUTM(NameProps("UTM 31N"), 31, true),
      CartesianCS::createEastingNorthing(UnitOfMeasure::METRE));
  auto tr_inv_tin_cart = MakeTransf(
      proj_crs_a, proj_crs_a, nullptr,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_CARTESIAN_GRID_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      EPSG_CODE_METHOD_CARTESIAN_GRID_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_TIN_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_TIN_OFFSET_FILE, "tin_c.json")});
  EXPECT_THAT(ExportToPROJ(tr_inv_tin_cart), HasSubstr("+proj=tinshift"));

  auto tr_inv_tin_h = MakeTransf(
      g1, g2, nullptr,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_GEOGRAPHIC2D_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      EPSG_CODE_METHOD_GEOGRAPHIC2D_OFFSETS_BY_TIN_INTERPOLATION_JSON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_TIN_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_TIN_OFFSET_FILE, "tin_h.json")});
  EXPECT_THAT(ExportToPROJ(tr_inv_tin_h), HasSubstr("+proj=tinshift"));

  auto tr_inv_tin_v = MakeTransf(
      v_m, v_m, &g1,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_VERTICAL_OFFSET_BY_TIN_INTERPOLATION_JSON,
      EPSG_CODE_METHOD_VERTICAL_OFFSET_BY_TIN_INTERPOLATION_JSON,
      {MakeFileParamVal(EPSG_NAME_PARAMETER_TIN_OFFSET_FILE,
                        EPSG_CODE_PARAMETER_TIN_OFFSET_FILE, "tin_v.json")});
  EXPECT_THAT(ExportToPROJ(tr_inv_tin_v), HasSubstr("+proj=tinshift"));

  auto tr_inv_gtg_pv = MakeTransf(
      gc, gc, nullptr,
      std::string("Inverse of ") +
          // NOLINTNEXTLINE(whitespace/line_length)
          EPSG_NAME_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_POSITION_VECTOR_GEOCENTRIC_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeMeasureParamVal(EPSG_NAME_PARAMETER_X_AXIS_TRANSLATION,
                           EPSG_CODE_PARAMETER_X_AXIS_TRANSLATION, 1.0,
                           UnitOfMeasure::METRE),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_Y_AXIS_TRANSLATION,
                           EPSG_CODE_PARAMETER_Y_AXIS_TRANSLATION, 2.0,
                           UnitOfMeasure::METRE),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_Z_AXIS_TRANSLATION,
                           EPSG_CODE_PARAMETER_Z_AXIS_TRANSLATION, 3.0,
                           UnitOfMeasure::METRE),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_X_AXIS_ROTATION,
                           EPSG_CODE_PARAMETER_X_AXIS_ROTATION, 0.1,
                           UnitOfMeasure::ARC_SECOND),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_Y_AXIS_ROTATION,
                           EPSG_CODE_PARAMETER_Y_AXIS_ROTATION, 0.2,
                           UnitOfMeasure::ARC_SECOND),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_Z_AXIS_ROTATION,
                           EPSG_CODE_PARAMETER_Z_AXIS_ROTATION, 0.3,
                           UnitOfMeasure::ARC_SECOND),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_SCALE_DIFFERENCE,
                           EPSG_CODE_PARAMETER_SCALE_DIFFERENCE, 0.4,
                           UnitOfMeasure::PARTS_PER_MILLION),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR),
       MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "gtg_vel.tif")});
  EXPECT_THAT(ExportToPROJ(tr_inv_gtg_pv), HasSubstr("+proj=deformation"));

  auto tr_inv_gtg_1143 = MakeTransf(
      gc, gc, nullptr,
      std::string("Inverse of ") +
          EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_USING_NEU_VELOCITY_GRID_GTG,
      {MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR),
       MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "gtg_vel.tif")});
  EXPECT_THAT(ExportToPROJ(tr_inv_gtg_1143), HasSubstr("+proj=deformation"));

  auto tr_inv_gtg_1142 = MakeTransf(
      gc, gc, nullptr,
      std::string("Inverse of ") +
          // NOLINTNEXTLINE(whitespace/line_length)
          EPSG_NAME_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      // NOLINTNEXTLINE(whitespace/line_length)
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATIONS_BY_GRID_GTG_AND_GEOCENTRIC_TRANSLATIONS_NEU_VELOCITIES_GTG,
      {MakeMeasureParamVal(EPSG_NAME_PARAMETER_SOURCE_EPOCH,
                           EPSG_CODE_PARAMETER_SOURCE_EPOCH, 2010.0,
                           UnitOfMeasure::YEAR),
       MakeMeasureParamVal(EPSG_NAME_PARAMETER_TARGET_EPOCH,
                           EPSG_CODE_PARAMETER_TARGET_EPOCH, 2020.0,
                           UnitOfMeasure::YEAR),
       MakeFileParamVal(EPSG_NAME_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        EPSG_CODE_PARAMETER_GEOCENTRIC_TRANSLATION_FILE,
                        "gtg_tx.tif"),
       MakeFileParamVal(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                        "gtg_vel.tif")});
  EXPECT_THAT(ExportToPROJ(tr_inv_gtg_1142), HasSubstr("+proj=xyzgridshift"));

  // 10. PointMotionOperation equivalence vs different CRS and non-PMO.
  auto dyn_g3d_a = MakeDynamicGeog3D("DynA", 2010.0);
  auto dyn_g3d_b = MakeDynamicGeog3D("DynB", 2020.0);
  auto p_vel = MakeParam(EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
                         EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE);
  auto m_1070 = OperationMethod::create(
      EpsgProps(EPSG_NAME_METHOD_POINT_MOTION_BY_GRID_CANADA_NTV2_VEL,
                EPSG_CODE_METHOD_POINT_MOTION_BY_GRID_CANADA_NTV2_VEL),
      std::vector<OperationParameterNNPtr>{p_vel});
  auto opv_vel = MakeFileParamVal(
      EPSG_NAME_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE,
      EPSG_CODE_PARAMETER_POINT_MOTION_VELOCITY_GRID_FILE, "vel.gsb");
  auto pmo_a = PointMotionOperation::create(
      NameProps("PMO"), dyn_g3d_a, m_1070,
      std::vector<GeneralParameterValueNNPtr>{opv_vel}, {});
  auto pmo_b = PointMotionOperation::create(
      NameProps("PMO_B"), dyn_g3d_b, m_1070,
      std::vector<GeneralParameterValueNNPtr>{opv_vel}, {});
  EXPECT_FALSE(
      pmo_a->isEquivalentTo(pmo_b.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      pmo_a->isEquivalentTo(hub_tr.get(), IComparable::Criterion::STRICT));

  // PMO PROJ export with custom angular/vertical units on Geographic3D CRS.
  UnitOfMeasure custom_vert("custom_m", 2.0, UnitOfMeasure::Type::LINEAR);
  auto g3d_custom_units = MakeGeog3D("Custom3D", custom_ang, custom_vert);
  DataEpoch e1(Measure(2010.0, UnitOfMeasure::YEAR));
  DataEpoch e2(Measure(2020.0, UnitOfMeasure::YEAR));
  auto pmo_custom = PointMotionOperation::create(
                        NameProps("PMO custom"), g3d_custom_units, m_1070,
                        std::vector<GeneralParameterValueNNPtr>{opv_vel}, {})
                        ->cloneWithEpochs(e1, e2);
  EXPECT_THAT(ExportToPROJ(pmo_custom), HasSubstr("+proj=deformation"));
}

}  // namespace
}  // namespace proj
