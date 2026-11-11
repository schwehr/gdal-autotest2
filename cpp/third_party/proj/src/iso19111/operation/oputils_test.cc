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
// Unit tests for ISO-19111 internal coordinate operation utilities
// (proj/src/iso19111/operation/oputils.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/oputils.cpp

#include "third_party/proj4/proj/src/iso19111/operation/oputils.hpp"

#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinateoperation.hpp"
#include "third_party/proj4/proj/include/proj/coordinatesystem.hpp"
#include "third_party/proj4/proj/include/proj/crs.hpp"
#include "third_party/proj4/proj/include/proj/datum.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/iso19111/operation/parammappings.hpp"
#include "third_party/proj4/proj/src/proj_constants.h"

namespace proj {
namespace {

using ::NS_PROJ::common::IdentifiedObject;
using ::NS_PROJ::common::ObjectUsage;
using ::NS_PROJ::common::UnitOfMeasure;
using ::NS_PROJ::crs::BoundCRS;
using ::NS_PROJ::crs::CompoundCRS;
using ::NS_PROJ::crs::CRSNNPtr;
using ::NS_PROJ::crs::GeodeticCRS;
using ::NS_PROJ::crs::GeodeticCRSNNPtr;
using ::NS_PROJ::crs::GeographicCRS;
using ::NS_PROJ::crs::GeographicCRSNNPtr;
using ::NS_PROJ::crs::VerticalCRS;
using ::NS_PROJ::crs::VerticalCRSNNPtr;
using ::NS_PROJ::cs::AxisDirection;
using ::NS_PROJ::cs::CartesianCS;
using ::NS_PROJ::cs::CoordinateSystemAxis;
using ::NS_PROJ::cs::EllipsoidalCS;
using ::NS_PROJ::cs::SphericalCS;
using ::NS_PROJ::cs::VerticalCS;
using ::NS_PROJ::datum::Ellipsoid;
using ::NS_PROJ::datum::GeodeticReferenceFrame;
using ::NS_PROJ::datum::PrimeMeridian;
using ::NS_PROJ::datum::VerticalReferenceFrame;
using ::NS_PROJ::io::WKTFormatter;
using ::NS_PROJ::metadata::Extent;
using ::NS_PROJ::metadata::ExtentPtr;
using ::NS_PROJ::metadata::GeographicBoundingBox;
using ::NS_PROJ::metadata::Identifier;
using ::NS_PROJ::metadata::PositionalAccuracy;
using ::NS_PROJ::metadata::PositionalAccuracyNNPtr;
using ::NS_PROJ::operation::addDefaultNameIfNeeded;
using ::NS_PROJ::operation::addDomains;
using ::NS_PROJ::operation::addModifiedIdentifier;
using ::NS_PROJ::operation::areEquivalentParameters;
using ::NS_PROJ::operation::AXIS_ORDER_CHANGE_2D_NAME;
using ::NS_PROJ::operation::AXIS_ORDER_CHANGE_3D_NAME;
using ::NS_PROJ::operation::BALLPARK_GEOCENTRIC_TRANSLATION;
using ::NS_PROJ::operation::BALLPARK_GEOGRAPHIC_OFFSET;
using ::NS_PROJ::operation::BALLPARK_VERTICAL_TRANSFORMATION;
using ::NS_PROJ::operation::
    BALLPARK_VERTICAL_TRANSFORMATION_NO_ELLIPSOID_VERT_HEIGHT;
using ::NS_PROJ::operation::buildOpName;
using ::NS_PROJ::operation::computeConcatenatedName;
using ::NS_PROJ::operation::ConcatenatedOperation;
using ::NS_PROJ::operation::Conversion;
using ::NS_PROJ::operation::CoordinateOperationNNPtr;
using ::NS_PROJ::operation::createMapNameEPSGCode;
using ::NS_PROJ::operation::createMethodMapNameEPSGCode;
using ::NS_PROJ::operation::createOpParamNameEPSGCode;
using ::NS_PROJ::operation::createPropertiesForInverse;
using ::NS_PROJ::operation::exportSourceCRSAndTargetCRSToWKT;
using ::NS_PROJ::operation::GeneralOperationParameterNNPtr;
using ::NS_PROJ::operation::GeneralParameterValueNNPtr;
using ::NS_PROJ::operation::getAccuracy;
using ::NS_PROJ::operation::getExtent;
using ::NS_PROJ::operation::getExtentPossiblySynthetized;
using ::NS_PROJ::operation::isTimeDependent;
using ::NS_PROJ::operation::NULL_GEOCENTRIC_TRANSLATION;
using ::NS_PROJ::operation::NULL_GEOGRAPHIC_OFFSET;
using ::NS_PROJ::operation::OperationMethod;
using ::NS_PROJ::operation::OperationParameter;
using ::NS_PROJ::operation::OperationParameterNNPtr;
using ::NS_PROJ::operation::SingleOperation;
using ::NS_PROJ::operation::Transformation;
using ::NS_PROJ::operation::TransformationNNPtr;
using ::NS_PROJ::operation::WKT1_CENTRAL_MERIDIAN;
using ::NS_PROJ::operation::WKT1_LATITUDE_OF_ORIGIN;
using ::NS_PROJ::operation::WKT1_SCALE_FACTOR;
using ::NS_PROJ::operation::WKT1_STANDARD_PARALLEL_1;
using ::NS_PROJ::util::BoxedValue;
using ::NS_PROJ::util::optional;
using ::NS_PROJ::util::PropertyMap;
using ::testing::AllOf;
using ::testing::HasSubstr;
using ::testing::Not;

PropertyMap NameProps(const std::string& name) {
  return PropertyMap().set(IdentifiedObject::NAME_KEY, name);
}

PropertyMap NameDomainProps(const std::string& name, const ExtentPtr& extent) {
  PropertyMap props;
  props.set(IdentifiedObject::NAME_KEY, name);
  if (extent) {
    props.set(
        ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
        NN_CHECK_THROW(
            std::static_pointer_cast<::osgeo::proj::util::BaseObject>(extent)));
  }
  return props;
}

GeographicCRSNNPtr MakeGeog2D(const std::string& name,
                              const ExtentPtr& extent = nullptr) {
  auto datum = GeodeticReferenceFrame::create(
      NameProps("Datum-" + name), Ellipsoid::GRS1980, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  return GeographicCRS::create(
      NameDomainProps(name, extent), datum,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
}

GeographicCRSNNPtr MakeGeog3D(const std::string& name) {
  auto datum = GeodeticReferenceFrame::create(
      NameProps("Datum-" + name), Ellipsoid::GRS1980, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  return GeographicCRS::create(
      NameProps(name), datum,
      EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
          UnitOfMeasure::DEGREE, UnitOfMeasure::METRE));
}

GeodeticCRSNNPtr MakeGeocentric(const std::string& name) {
  auto datum = GeodeticReferenceFrame::create(
      NameProps("Datum-" + name), Ellipsoid::GRS1980, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  return GeodeticCRS::create(
      NameProps(name), datum,
      CartesianCS::createGeocentric(UnitOfMeasure::METRE));
}

GeodeticCRSNNPtr MakeSpherical(const std::string& name) {
  auto datum = GeodeticReferenceFrame::create(
      NameProps("Datum-" + name), Ellipsoid::GRS1980, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  auto lat_axis =
      CoordinateSystemAxis::create(NameProps("Spherical latitude"), "U",
                                   AxisDirection::NORTH, UnitOfMeasure::DEGREE);
  auto lon_axis =
      CoordinateSystemAxis::create(NameProps("Spherical longitude"), "V",
                                   AxisDirection::EAST, UnitOfMeasure::DEGREE);
  auto r_axis =
      CoordinateSystemAxis::create(NameProps("Geocentric radius"), "r",
                                   AxisDirection::UP, UnitOfMeasure::METRE);
  auto cs =
      SphericalCS::create(NameProps("Spherical"), lat_axis, lon_axis, r_axis);
  return GeodeticCRS::create(NameProps(name), datum, cs);
}

VerticalCRSNNPtr MakeVertical(const std::string& name,
                              const ExtentPtr& extent = nullptr) {
  auto datum = VerticalReferenceFrame::create(NameProps("VDatum-" + name));
  return VerticalCRS::create(
      NameDomainProps(name, extent), datum,
      VerticalCS::createGravityRelatedHeight(UnitOfMeasure::METRE));
}

TransformationNNPtr MakeSimpleTransf(
    const PropertyMap& props, const CRSNNPtr& src, const CRSNNPtr& tgt,
    const std::string& accuracy = std::string(), bool custom_method = false) {
  std::vector<PositionalAccuracyNNPtr> accs;
  if (!accuracy.empty()) {
    accs.push_back(PositionalAccuracy::create(accuracy));
  }
  if (custom_method) {
    auto method =
        OperationMethod::create(NameProps("Custom unimplemented method"),
                                std::vector<OperationParameterNNPtr>{});
    return Transformation::create(props, src, tgt, nullptr, method, {}, accs);
  }
  return Transformation::createGeocentricTranslations(props, src, tgt, 0.0, 0.0,
                                                      0.0, accs);
}

TEST(OpUtilsTest, NameAndMethodConstantsAndMapHelpers) {
  EXPECT_EQ(std::string(BALLPARK_GEOCENTRIC_TRANSLATION),
            "Ballpark geocentric translation");
  EXPECT_EQ(std::string(NULL_GEOGRAPHIC_OFFSET), "Null geographic offset");
  EXPECT_EQ(std::string(NULL_GEOCENTRIC_TRANSLATION),
            "Null geocentric translation");
  EXPECT_EQ(std::string(BALLPARK_GEOGRAPHIC_OFFSET),
            "Ballpark geographic offset");
  EXPECT_EQ(std::string(BALLPARK_VERTICAL_TRANSFORMATION),
            "ballpark vertical transformation");
  EXPECT_EQ(
      std::string(BALLPARK_VERTICAL_TRANSFORMATION_NO_ELLIPSOID_VERT_HEIGHT),
      "ballpark vertical transformation, without ellipsoid height to "
      "vertical height correction");

  OperationParameterNNPtr param =
      createOpParamNameEPSGCode(EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN);
  EXPECT_EQ(param->nameStr(), EPSG_NAME_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN);
  EXPECT_EQ(param->getEPSGCode(),
            EPSG_CODE_PARAMETER_LATITUDE_OF_NATURAL_ORIGIN);

  PropertyMap method_map =
      createMethodMapNameEPSGCode(EPSG_CODE_METHOD_TRANSVERSE_MERCATOR);
  auto method = OperationMethod::create(
      method_map, std::vector<GeneralOperationParameterNNPtr>{});
  EXPECT_EQ(method->getEPSGCode(), EPSG_CODE_METHOD_TRANSVERSE_MERCATOR);
  EXPECT_EQ(method->nameStr(), "Transverse Mercator");

  PropertyMap geocentric_map = createMethodMapNameEPSGCode(
      EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOCENTRIC);
  auto geocentric_method = OperationMethod::create(
      geocentric_map, std::vector<GeneralOperationParameterNNPtr>{});
  EXPECT_EQ(geocentric_method->getEPSGCode(),
            EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOCENTRIC);

  // Explicit std::string overload.
  const std::string str_name = "Custom param std string";
  auto p_from_str =
      OperationParameter::create(createMapNameEPSGCode(str_name, 12345));
  EXPECT_EQ(p_from_str->nameStr(), str_name);
  EXPECT_EQ(p_from_str->getEPSGCode(), 12345);

  // Explicit const char* overload.
  const char* cstr_name = "Custom param cstr";
  auto p_from_cstr =
      OperationParameter::create(createMapNameEPSGCode(cstr_name, 54321));
  EXPECT_EQ(p_from_cstr->nameStr(), cstr_name);
  EXPECT_EQ(p_from_cstr->getEPSGCode(), 54321);
}

TEST(OpUtilsTest, AddDomainsEmptyAndNonEmpty) {
  auto crs_no_domain = MakeGeog2D("NoDomainCRS");
  PropertyMap map_empty;
  addDomains(map_empty, crs_no_domain.get());
  EXPECT_EQ(map_empty.get(ObjectUsage::OBJECT_DOMAIN_KEY), nullptr);

  auto domain_obj = Extent::createFromBBOX(-10.0, 35.0, 20.0, 60.0);
  auto crs_with_domain = MakeGeog2D("WithDomainCRS", domain_obj);
  PropertyMap map_with_domain = NameProps("CopiedDomain");
  PropertyMap& returned_ref =
      addDomains(map_with_domain, crs_with_domain.get());
  EXPECT_EQ(&returned_ref, &map_with_domain);
  EXPECT_NE(map_with_domain.get(ObjectUsage::OBJECT_DOMAIN_KEY), nullptr);

  auto rebuilt = GeographicCRS::create(
      map_with_domain,
      GeodeticReferenceFrame::create(NameProps("D"), Ellipsoid::GRS1980,
                                     optional<std::string>(),
                                     PrimeMeridian::GREENWICH),
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  ASSERT_EQ(rebuilt->domains().size(), 1);
  ASSERT_NE(rebuilt->domains()[0]->domainOfValidity(), nullptr);
  EXPECT_TRUE(rebuilt->domains()[0]->domainOfValidity()->isEquivalentTo(
      domain_obj.get()));
}

TEST(OpUtilsTest, BuildOpNameAndCRSQualifiers) {
  auto geog2d_a = MakeGeog2D("CommonCRS");
  auto geog2d_b = MakeGeog2D("CommonCRS");
  auto geog3d = MakeGeog3D("CommonCRS");
  auto geocentric = MakeGeocentric("CommonCRS");
  auto spherical = MakeSpherical("CommonCRS");
  auto vert_a = MakeVertical("CommonCRS");
  auto vert_b = MakeVertical("CommonCRS");
  auto other = MakeGeog2D("OtherCRS");

  EXPECT_EQ(buildOpName("Transformation", geog2d_a.as_nullable(),
                        other.as_nullable()),
            "Transformation from CommonCRS to OtherCRS");
  EXPECT_EQ(buildOpName("Transformation", geog2d_a.as_nullable(),
                        geog3d.as_nullable()),
            "Transformation from CommonCRS (geog2D) to CommonCRS (geog3D)");
  EXPECT_EQ(buildOpName("Transformation", geocentric.as_nullable(),
                        geog2d_a.as_nullable()),
            "Transformation from CommonCRS (geocentric) to CommonCRS (geog2D)");
  EXPECT_EQ(buildOpName("Transformation", geog2d_a.as_nullable(),
                        geog2d_b.as_nullable()),
            "Transformation from CommonCRS to CommonCRS");
  EXPECT_EQ(buildOpName("Conversion", spherical.as_nullable(),
                        geog2d_a.as_nullable()),
            "Conversion from CommonCRS to CommonCRS (geog2D)");
  EXPECT_EQ(
      buildOpName("Operation", vert_a.as_nullable(), vert_b.as_nullable()),
      "Operation from CommonCRS to CommonCRS");
}

TEST(OpUtilsTest, AddModifiedIdentifierAndInverseMethodProperties) {
  auto no_id_method = OperationMethod::create(
      NameProps("NoId"), std::vector<OperationParameterNNPtr>{});
  PropertyMap no_id_map;
  addModifiedIdentifier(no_id_map, no_id_method.get(), /*inverse=*/true,
                        /*derivedFrom=*/false);
  EXPECT_EQ(no_id_map.get(IdentifiedObject::IDENTIFIERS_KEY), nullptr);

  auto epsg_method =
      OperationMethod::create(createMapNameEPSGCode("Forward Method", 9901),
                              std::vector<OperationParameterNNPtr>{});

  {
    PropertyMap m = NameProps("Direct");
    addModifiedIdentifier(m, epsg_method.get(), /*inverse=*/false,
                          /*derivedFrom=*/false);
    auto out =
        OperationMethod::create(m, std::vector<OperationParameterNNPtr>{});
    ASSERT_EQ(out->identifiers().size(), 1);
    EXPECT_EQ(*(out->identifiers()[0]->codeSpace()), "EPSG");
    EXPECT_EQ(out->identifiers()[0]->code(), "9901");
  }
  {
    PropertyMap m = NameProps("Derived");
    addModifiedIdentifier(m, epsg_method.get(), /*inverse=*/false,
                          /*derivedFrom=*/true);
    auto out =
        OperationMethod::create(m, std::vector<OperationParameterNNPtr>{});
    ASSERT_EQ(out->identifiers().size(), 1);
    EXPECT_EQ(*(out->identifiers()[0]->codeSpace()), "DERIVED_FROM(EPSG)");
  }
  {
    PropertyMap m = NameProps("Inverse");
    addModifiedIdentifier(m, epsg_method.get(), /*inverse=*/true,
                          /*derivedFrom=*/false);
    auto out =
        OperationMethod::create(m, std::vector<OperationParameterNNPtr>{});
    ASSERT_EQ(out->identifiers().size(), 1);
    EXPECT_EQ(*(out->identifiers()[0]->codeSpace()), "INVERSE(EPSG)");
  }
  {
    PropertyMap m = NameProps("InverseDerived");
    addModifiedIdentifier(m, epsg_method.get(), /*inverse=*/true,
                          /*derivedFrom=*/true);
    auto out =
        OperationMethod::create(m, std::vector<OperationParameterNNPtr>{});
    ASSERT_EQ(out->identifiers().size(), 1);
    EXPECT_EQ(*(out->identifiers()[0]->codeSpace()),
              "INVERSE(DERIVED_FROM(EPSG))");
  }
  {
    PropertyMap inv_id_props =
        NameProps("AlreadyInverse")
            .set(Identifier::CODESPACE_KEY, "INVERSE(EPSG)")
            .set(Identifier::CODE_KEY, "9902");
    auto inv_id_method = OperationMethod::create(
        inv_id_props, std::vector<OperationParameterNNPtr>{});
    PropertyMap m = NameProps("Unwrapped");
    addModifiedIdentifier(m, inv_id_method.get(), /*inverse=*/true,
                          /*derivedFrom=*/false);
    auto out =
        OperationMethod::create(m, std::vector<OperationParameterNNPtr>{});
    ASSERT_EQ(out->identifiers().size(), 1);
    EXPECT_EQ(*(out->identifiers()[0]->codeSpace()), "EPSG");
    EXPECT_EQ(out->identifiers()[0]->code(), "9902");
  }
  {
    PropertyMap partial_inv_props =
        NameProps("PartialInversePrefix")
            .set(Identifier::CODESPACE_KEY, "INVERSE(EPSG)_MOD")
            .set(Identifier::CODE_KEY, "9903");
    auto partial_method = OperationMethod::create(
        partial_inv_props, std::vector<OperationParameterNNPtr>{});
    PropertyMap m = NameProps("DoubleWrapped");
    addModifiedIdentifier(m, partial_method.get(), /*inverse=*/true,
                          /*derivedFrom=*/false);
    auto out =
        OperationMethod::create(m, std::vector<OperationParameterNNPtr>{});
    ASSERT_EQ(out->identifiers().size(), 1);
    EXPECT_EQ(*(out->identifiers()[0]->codeSpace()),
              "INVERSE(INVERSE(EPSG)_MOD)");
  }

  // createPropertiesForInverse(OperationMethodNNPtr)
  auto empty_name_method = OperationMethod::create(
      PropertyMap(), std::vector<OperationParameterNNPtr>{});
  PropertyMap empty_inv = createPropertiesForInverse(empty_name_method);
  EXPECT_EQ(empty_inv.get(IdentifiedObject::NAME_KEY), nullptr);

  PropertyMap fwd_inv = createPropertiesForInverse(epsg_method);
  auto inv_method =
      OperationMethod::create(fwd_inv, std::vector<OperationParameterNNPtr>{});
  EXPECT_EQ(inv_method->nameStr(), "Inverse of Forward Method");
  ASSERT_EQ(inv_method->identifiers().size(), 1);
  EXPECT_EQ(*(inv_method->identifiers()[0]->codeSpace()), "INVERSE(EPSG)");

  PropertyMap double_inv = createPropertiesForInverse(inv_method);
  auto roundtrip_method = OperationMethod::create(
      double_inv, std::vector<OperationParameterNNPtr>{});
  EXPECT_EQ(roundtrip_method->nameStr(), "Forward Method");
  ASSERT_EQ(roundtrip_method->identifiers().size(), 1);
  EXPECT_EQ(*(roundtrip_method->identifiers()[0]->codeSpace()), "EPSG");
}

TEST(OpUtilsTest, CreatePropertiesForInverseCoordinateOperations) {
  auto src = MakeGeog2D("SrcCRS");
  auto tgt = MakeGeog2D("TgtCRS");
  auto extent = Extent::createFromBBOX(-5.0, 40.0, 10.0, 50.0);

  auto read_name = [](const PropertyMap& map) {
    std::string val;
    EXPECT_TRUE(map.getStringValue(IdentifiedObject::NAME_KEY, val));
    return val;
  };
  auto read_remarks = [](const PropertyMap& map) {
    std::string val;
    EXPECT_TRUE(map.getStringValue(IdentifiedObject::REMARKS_KEY, val));
    return val;
  };
  auto read_epsg_code = [](const PropertyMap& map) -> int {
    const auto* ptr = map.get("OPERATION_METHOD_EPSG_CODE");
    if (!ptr) {
      return 0;
    }
    const auto* boxed = dynamic_cast<const BoxedValue*>(ptr->get());
    return boxed ? boxed->integerValue() : 0;
  };

  // OpType prefix branches + buildOpName matching reversal +
  // remarks/domains/EPSG method.
  {
    PropertyMap props = NameDomainProps(
        std::string(BALLPARK_GEOCENTRIC_TRANSLATION) + " from SrcCRS to TgtCRS",
        extent);
    props.set(IdentifiedObject::REMARKS_KEY, "ballpark remark")
        .set(Identifier::CODESPACE_KEY, Identifier::EPSG)
        .set(Identifier::CODE_KEY, "8888");
    auto op = MakeSimpleTransf(props, src, tgt);
    PropertyMap inv = createPropertiesForInverse(op.get(), /*derivedFrom=*/true,
                                                 /*approximateInversion=*/true);
    EXPECT_EQ(read_name(inv), std::string(BALLPARK_GEOCENTRIC_TRANSLATION) +
                                  " from TgtCRS to SrcCRS (approx. inversion)");
    EXPECT_EQ(read_remarks(inv), "ballpark remark");
    EXPECT_NE(inv.get(ObjectUsage::OBJECT_DOMAIN_KEY), nullptr);
    EXPECT_NE(inv.get(IdentifiedObject::IDENTIFIERS_KEY), nullptr);
    EXPECT_EQ(read_epsg_code(inv),
              EPSG_CODE_METHOD_GEOCENTRIC_TRANSLATION_GEOGRAPHIC_2D);
  }
  {
    auto op =
        MakeSimpleTransf(NameProps(std::string(BALLPARK_GEOGRAPHIC_OFFSET) +
                                   " from SrcCRS to TgtCRS"),
                         src, tgt);
    EXPECT_EQ(
        read_name(createPropertiesForInverse(op.get(), false, false)),
        std::string(BALLPARK_GEOGRAPHIC_OFFSET) + " from TgtCRS to SrcCRS");
  }
  {
    auto op = MakeSimpleTransf(NameProps(std::string(NULL_GEOGRAPHIC_OFFSET) +
                                         " from SrcCRS to TgtCRS"),
                               src, tgt);
    EXPECT_EQ(read_name(createPropertiesForInverse(op.get(), false, false)),
              std::string(NULL_GEOGRAPHIC_OFFSET) + " from TgtCRS to SrcCRS");
  }
  {
    auto op =
        MakeSimpleTransf(NameProps(std::string(NULL_GEOCENTRIC_TRANSLATION) +
                                   " from SrcCRS to TgtCRS"),
                         src, tgt);
    EXPECT_EQ(
        read_name(createPropertiesForInverse(op.get(), false, false)),
        std::string(NULL_GEOCENTRIC_TRANSLATION) + " from TgtCRS to SrcCRS");
  }

  // Non-Transformation with name starting with "Transformation from ".
  {
    auto op = SingleOperation::createPROJBased(
        NameProps("Transformation from SrcCRS to TgtCRS"), "+proj=noop",
        src.as_nullable(), tgt.as_nullable(), {});
    EXPECT_EQ(read_name(createPropertiesForInverse(op.get(), false, false)),
              "Transformation from TgtCRS to SrcCRS");
  }

  // Empty name Transformation with source & target ->
  // buildOpName("Transformation", target, source). Plus custom method with
  // getEPSGCode() == 0.
  {
    auto op = MakeSimpleTransf(PropertyMap(), src, tgt, "",
                               /*custom_method=*/true);
    PropertyMap inv = createPropertiesForInverse(op.get(), false, false);
    EXPECT_EQ(read_name(inv), "Transformation from TgtCRS to SrcCRS");
    EXPECT_EQ(inv.get("OPERATION_METHOD_EPSG_CODE"), nullptr);
  }

  // Transformation name containing " + ", quoted inverse, unquoted inverse,
  // plain name.
  {
    auto op_plus = MakeSimpleTransf(NameProps("Part1 + Part2"), src, tgt);
    EXPECT_EQ(
        read_name(createPropertiesForInverse(op_plus.get(), false, false)),
        "Inverse of 'Part1 + Part2'");

    auto op_quoted_inv =
        MakeSimpleTransf(NameProps("Inverse of 'QuotedOpName'"), src, tgt);
    EXPECT_EQ(read_name(createPropertiesForInverse(op_quoted_inv.get(), false,
                                                   false)),
              "QuotedOpName");

    auto op_plain_inv =
        MakeSimpleTransf(NameProps("Inverse of SimpleHelmert"), src, tgt);
    EXPECT_EQ(
        read_name(createPropertiesForInverse(op_plain_inv.get(), false, false)),
        "SimpleHelmert");

    auto op_plain = MakeSimpleTransf(NameProps("SimpleHelmert"), src, tgt);
    EXPECT_EQ(
        read_name(createPropertiesForInverse(op_plain.get(), false, false)),
        "Inverse of SimpleHelmert");
  }

  // Conversion (dynamic_cast<Conversion*>) branches:
  // 1) Multi-step with quotes, INVERSE_OF, AXIS_ORDER_CHANGE_2D_NAME,
  // AXIS_ORDER_CHANGE_3D_NAME. 2) Single step starting with INVERSE_OF. 3)
  // Empty name with no source/target CRS -> no NAME_KEY emitted.
  auto conv_method = OperationMethod::create(
      NameProps("Custom Conv"), std::vector<OperationParameterNNPtr>{});
  {
    const std::string compound_step_name =
        "Inverse of Step1 + 'Quoted + Step' + " + AXIS_ORDER_CHANGE_2D_NAME +
        " + " + AXIS_ORDER_CHANGE_3D_NAME + " + Step2";
    auto conv = Conversion::create(NameProps(compound_step_name), conv_method,
                                   std::vector<GeneralParameterValueNNPtr>{});
    EXPECT_EQ(read_name(createPropertiesForInverse(conv.get(), false, false)),
              "Inverse of Step2 + " + AXIS_ORDER_CHANGE_3D_NAME + " + " +
                  AXIS_ORDER_CHANGE_2D_NAME +
                  " + Inverse of 'Quoted + Step' + Step1");
  }
  {
    auto conv =
        Conversion::create(NameProps("Inverse of Zone 31N"), conv_method,
                           std::vector<GeneralParameterValueNNPtr>{});
    EXPECT_EQ(read_name(createPropertiesForInverse(conv.get(), false, false)),
              "Zone 31N");
  }
  {
    auto unnamed_conv = Conversion::create(
        PropertyMap(), conv_method, std::vector<GeneralParameterValueNNPtr>{});
    PropertyMap inv =
        createPropertiesForInverse(unnamed_conv.get(), false, false);
    EXPECT_EQ(inv.get(IdentifiedObject::NAME_KEY), nullptr);
  }

  // ConcatenatedOperation -> opType = "Operation", not a SingleOperation.
  {
    auto mid = MakeGeog2D("MidCRS");
    auto s1 = MakeSimpleTransf(NameProps("S1"), src, mid);
    auto s2 = MakeSimpleTransf(NameProps("S2"), mid, tgt);
    auto concat =
        ConcatenatedOperation::create(NameProps("Combined Op"), {s1, s2}, {});
    PropertyMap inv = createPropertiesForInverse(concat.get(), false, false);
    EXPECT_EQ(read_name(inv), "Inverse of Combined Op");
    EXPECT_EQ(inv.get("OPERATION_METHOD_EPSG_CODE"), nullptr);
  }
}

TEST(OpUtilsTest, AddDefaultNameIfNeeded) {
  PropertyMap without_name;
  PropertyMap with_default =
      addDefaultNameIfNeeded(without_name, "Fallback Name");
  std::string val;
  ASSERT_TRUE(with_default.getStringValue(IdentifiedObject::NAME_KEY, val));
  EXPECT_EQ(val, "Fallback Name");

  PropertyMap with_existing = NameProps("Explicit Name");
  PropertyMap preserved =
      addDefaultNameIfNeeded(with_existing, "Fallback Name");
  val.clear();
  ASSERT_TRUE(preserved.getStringValue(IdentifiedObject::NAME_KEY, val));
  EXPECT_EQ(val, "Explicit Name");
}

TEST(OpUtilsTest, AreEquivalentParametersAndIsTimeDependent) {
  // Forward and reversed ordering across equivalent-parameter groups.
  EXPECT_TRUE(
      areEquivalentParameters("latitude_of_point_1", "Latitude_Of_1st_Point"));
  EXPECT_TRUE(
      areEquivalentParameters("Latitude_Of_1st_Point", "latitude_of_point_1"));
  EXPECT_TRUE(areEquivalentParameters("longitude_of_point_2",
                                      "Longitude_Of_2nd_Point"));
  EXPECT_TRUE(areEquivalentParameters("satellite_height", "height"));
  EXPECT_TRUE(
      areEquivalentParameters(EPSG_NAME_PARAMETER_FALSE_EASTING,
                              EPSG_NAME_PARAMETER_EASTING_FALSE_ORIGIN));
  EXPECT_TRUE(
      areEquivalentParameters(EPSG_NAME_PARAMETER_FALSE_NORTHING,
                              EPSG_NAME_PARAMETER_NORTHING_PROJECTION_CENTRE));
  EXPECT_TRUE(areEquivalentParameters(
      EPSG_NAME_PARAMETER_SCALE_FACTOR_AT_NATURAL_ORIGIN, WKT1_SCALE_FACTOR));
  EXPECT_TRUE(
      areEquivalentParameters(WKT1_LATITUDE_OF_ORIGIN, "Central_Parallel"));
  EXPECT_TRUE(areEquivalentParameters(WKT1_CENTRAL_MERIDIAN,
                                      EPSG_NAME_PARAMETER_LONGITUDE_OF_ORIGIN));
  EXPECT_TRUE(
      areEquivalentParameters(EPSG_NAME_PARAMETER_AZIMUTH_INITIAL_LINE,
                              EPSG_NAME_PARAMETER_AZIMUTH_PROJECTION_CENTRE));
  EXPECT_TRUE(areEquivalentParameters("pseudo_standard_parallel_1",
                                      WKT1_STANDARD_PARALLEL_1));

  EXPECT_FALSE(areEquivalentParameters(EPSG_NAME_PARAMETER_FALSE_EASTING,
                                       EPSG_NAME_PARAMETER_FALSE_NORTHING));
  EXPECT_FALSE(areEquivalentParameters("nonexistent_a", "nonexistent_b"));

  EXPECT_TRUE(isTimeDependent("Time-dependent Coordinate Frame rotation"));
  EXPECT_TRUE(isTimeDependent("custom TIME DEPENDENT Helmert"));
  EXPECT_FALSE(isTimeDependent("Geocentric translations"));
}

TEST(OpUtilsTest, ComputeConcatenatedName) {
  EXPECT_EQ(computeConcatenatedName({}), "");

  auto crs_a = MakeGeog2D("A");
  auto crs_b = MakeGeog2D("B");
  auto op1 = MakeSimpleTransf(NameProps("First step"), crs_a, crs_b);
  auto op2_unnamed = MakeSimpleTransf(PropertyMap(), crs_b, crs_a);
  auto op3 = MakeSimpleTransf(NameProps("Third step"), crs_a, crs_b);
  EXPECT_EQ(computeConcatenatedName({op1, op2_unnamed, op3}),
            "First step + unnamed + Third step");
}

TEST(OpUtilsTest, GetExtentForOperationsCRSsAndCompoundSynthesis) {
  auto crs_a = MakeGeog2D("A");
  auto crs_b = MakeGeog2D("B");
  auto crs_c = MakeGeog2D("C");

  // Conversion -> WORLD extent.
  auto conv = Conversion::createUTM(NameProps("UTM 31N"), 31, true);
  bool empty_intersection = true;
  auto conv_extent =
      getExtent(conv, /*conversionExtentIsWorld=*/true, empty_intersection);
  EXPECT_FALSE(empty_intersection);
  EXPECT_EQ(conv_extent, Extent::WORLD);

  // Transformation without domain -> nullptr, emptyIntersection=false.
  auto t_no_domain = MakeSimpleTransf(NameProps("NoDomain"), crs_a, crs_b);
  empty_intersection = true;
  EXPECT_EQ(getExtent(t_no_domain, true, empty_intersection), nullptr);
  EXPECT_FALSE(empty_intersection);

  // Transformation with domain -> direct domain extent.
  auto bbox_eu = Extent::createFromBBOX(-10.0, 35.0, 20.0, 60.0);
  auto bbox_west = Extent::createFromBBOX(0.0, 40.0, 30.0, 70.0);
  auto bbox_disjoint = Extent::createFromBBOX(120.0, 20.0, 150.0, 45.0);
  auto t_eu = MakeSimpleTransf(NameDomainProps("EU", bbox_eu), crs_a, crs_b);
  auto t_west =
      MakeSimpleTransf(NameDomainProps("West", bbox_west), crs_b, crs_c);
  auto t_disjoint =
      MakeSimpleTransf(NameDomainProps("Asia", bbox_disjoint), crs_b, crs_c);

  auto direct_ext = getExtent(t_eu, true, empty_intersection);
  ASSERT_NE(direct_ext, nullptr);
  EXPECT_FALSE(empty_intersection);

  // ConcatenatedOperation delegating getExtent to sub-operations.
  auto concat_intersect =
      ConcatenatedOperation::create(NameProps("Intersect"), {t_eu, t_west}, {});
  auto inter_ext = getExtent(concat_intersect, true, empty_intersection);
  ASSERT_NE(inter_ext, nullptr);
  EXPECT_FALSE(empty_intersection);
  const auto* inter_bbox = dynamic_cast<const GeographicBoundingBox*>(
      inter_ext->geographicElements()[0].get());
  ASSERT_NE(inter_bbox, nullptr);
  EXPECT_DOUBLE_EQ(inter_bbox->westBoundLongitude(), 0.0);
  EXPECT_DOUBLE_EQ(inter_bbox->eastBoundLongitude(), 20.0);
  EXPECT_DOUBLE_EQ(inter_bbox->southBoundLatitude(), 40.0);
  EXPECT_DOUBLE_EQ(inter_bbox->northBoundLatitude(), 60.0);

  // Vector containing null extent followed by valid extent -> skips null.
  EXPECT_NE(getExtent(std::vector<CoordinateOperationNNPtr>{t_no_domain, t_eu},
                      true, empty_intersection),
            nullptr);
  EXPECT_FALSE(empty_intersection);

  // Disjoint sub-operations -> nullptr and emptyIntersection=true, propagating
  // outward.
  auto concat_disjoint = ConcatenatedOperation::create(NameProps("Disjoint"),
                                                       {t_eu, t_disjoint}, {});
  EXPECT_EQ(getExtent(concat_disjoint, true, empty_intersection), nullptr);
  EXPECT_TRUE(empty_intersection);
  EXPECT_EQ(
      getExtent(std::vector<CoordinateOperationNNPtr>{concat_disjoint, conv},
                true, empty_intersection),
      nullptr);
  EXPECT_TRUE(empty_intersection);

  // CRS getExtent + BoundCRS baseCRS fallback + CompoundCRS synthesis.
  auto crs_eu = MakeGeog2D("CRS_EU", bbox_eu);
  auto crs_plain = MakeGeog2D("CRS_Plain");
  EXPECT_EQ(getExtent(crs_plain), nullptr);

  auto bound_crs = BoundCRS::create(
      crs_eu, crs_plain,
      MakeSimpleTransf(NameProps("BoundTransf"), crs_eu, crs_plain));
  EXPECT_EQ(bound_crs->domains().size(), 0);
  ASSERT_NE(getExtent(bound_crs), nullptr);

  bool approx_out = true;
  EXPECT_NE(getExtentPossiblySynthetized(crs_eu, approx_out), nullptr);
  EXPECT_FALSE(approx_out);

  EXPECT_EQ(getExtentPossiblySynthetized(crs_plain, approx_out), nullptr);
  EXPECT_FALSE(approx_out);

  auto vert_west = MakeVertical("Vert_West", bbox_west);
  auto vert_plain = MakeVertical("Vert_Plain");

  auto comp_both =
      CompoundCRS::create(NameProps("BothExt"), {crs_eu, vert_west});
  auto comp_both_ext = getExtentPossiblySynthetized(comp_both, approx_out);
  ASSERT_NE(comp_both_ext, nullptr);
  EXPECT_TRUE(approx_out);

  auto comp_vert_only =
      CompoundCRS::create(NameProps("VertOnly"), {crs_plain, vert_west});
  auto comp_vert_ext = getExtentPossiblySynthetized(comp_vert_only, approx_out);
  ASSERT_NE(comp_vert_ext, nullptr);
  EXPECT_TRUE(approx_out);

  auto comp_horiz_only =
      CompoundCRS::create(NameProps("HorizOnly"), {crs_eu, vert_plain});
  auto comp_horiz_ext =
      getExtentPossiblySynthetized(comp_horiz_only, approx_out);
  ASSERT_NE(comp_horiz_ext, nullptr);
  EXPECT_TRUE(approx_out);

  auto comp_none =
      CompoundCRS::create(NameProps("NoExt"), {crs_plain, vert_plain});
  EXPECT_EQ(getExtentPossiblySynthetized(comp_none, approx_out), nullptr);
  EXPECT_TRUE(approx_out);
}

TEST(OpUtilsTest, GetAccuracyAndETRS89NoOpEnsembleHandling) {
  auto crs_a = MakeGeog2D("A");
  auto crs_b = MakeGeog2D("B");
  auto crs_c = MakeGeog2D("C");

  auto conv = Conversion::createUTM(NameProps("UTM 32N"), 32, true);
  EXPECT_DOUBLE_EQ(getAccuracy(conv), 0.0);

  auto t_valid = MakeSimpleTransf(NameProps("Acc1.5"), crs_a, crs_b, "1.5");
  EXPECT_DOUBLE_EQ(getAccuracy(t_valid), 1.5);

  auto t_bad_str =
      MakeSimpleTransf(NameProps("AccInvalid"), crs_a, crs_b, "invalid");
  EXPECT_DOUBLE_EQ(getAccuracy(t_bad_str), -1.0);

  auto t_empty_acc = MakeSimpleTransf(NameProps("AccNone"), crs_b, crs_c);
  EXPECT_DOUBLE_EQ(getAccuracy(t_empty_acc), -1.0);

  auto t_valid_2 = MakeSimpleTransf(NameProps("Acc0.8"), crs_b, crs_c, "0.8");
  auto concat_no_acc =
      ConcatenatedOperation::create(NameProps("C"), {t_valid, t_valid_2}, {});
  EXPECT_DOUBLE_EQ(getAccuracy(concat_no_acc), 2.3);

  auto concat_explicit_acc =
      ConcatenatedOperation::create(NameProps("C_exp"), {t_valid, t_valid_2},
                                    {PositionalAccuracy::create("0.25")});
  EXPECT_DOUBLE_EQ(getAccuracy(concat_explicit_acc), 0.25);

  // Vector helpers: empty, unknown element, mixed 0.0 + cached next_accuracy.
  EXPECT_DOUBLE_EQ(getAccuracy(std::vector<CoordinateOperationNNPtr>{}), -1.0);
  EXPECT_DOUBLE_EQ(
      getAccuracy(std::vector<CoordinateOperationNNPtr>{t_valid, t_empty_acc}),
      -1.0);
  EXPECT_DOUBLE_EQ(getAccuracy(std::vector<CoordinateOperationNNPtr>{
                       conv, t_valid, t_valid_2}),
                   2.3);

  // Special ETRS89-XXX -> ETRS89 / ETRFxxxx -> ETRS89-YYY cases.
  auto etrs_fra = MakeGeog2D("ETRS89-FRA");
  auto etrs89 = MakeGeog2D("ETRS89");
  auto etrf2000 = MakeGeog2D("ETRF2000");
  auto etrs_deu = MakeGeog2D("ETRS89-DEU");

  auto noop_step_1 = SingleOperation::createPROJBased(
      NameProps("NoOp1"), "+proj=noop", etrs_fra.as_nullable(),
      etrs89.as_nullable(), {PositionalAccuracy::create("0.04")});
  auto helmert_step_2 = Transformation::createGeocentricTranslations(
      NameProps("Shift2"), etrs89, etrs_deu, 0.01, 0.0, 0.0,
      {PositionalAccuracy::create("0.08")});
  EXPECT_DOUBLE_EQ(getAccuracy(std::vector<CoordinateOperationNNPtr>{
                       noop_step_1, helmert_step_2}),
                   0.08);

  auto helmert_step_1 = Transformation::createGeocentricTranslations(
      NameProps("Shift1"), etrs_fra, etrf2000, 0.01, 0.0, 0.0,
      {PositionalAccuracy::create("0.03")});
  auto noop_step_2 = SingleOperation::createPROJBased(
      NameProps("NoOp2"), "+proj=noop", etrf2000.as_nullable(),
      etrs_deu.as_nullable(), {PositionalAccuracy::create("0.09")});
  EXPECT_DOUBLE_EQ(getAccuracy(std::vector<CoordinateOperationNNPtr>{
                       helmert_step_1, noop_step_2}),
                   0.09);

  // Exceeds 0.1 threshold -> summed instead of max.
  auto large_step_1 = SingleOperation::createPROJBased(
      NameProps("LargeNoOp"), "+proj=noop", etrs_fra.as_nullable(),
      etrs89.as_nullable(), {PositionalAccuracy::create("0.2")});
  EXPECT_DOUBLE_EQ(getAccuracy(std::vector<CoordinateOperationNNPtr>{
                       large_step_1, helmert_step_2}),
                   0.28);

  // Neither op is a no-op (and first throws on exportToPROJString) -> summed.
  auto unexportable_step =
      MakeSimpleTransf(NameProps("Unexportable"), etrs_fra, etrs89, "0.04",
                       /*custom_method=*/true);
  EXPECT_DOUBLE_EQ(getAccuracy(std::vector<CoordinateOperationNNPtr>{
                       unexportable_step, helmert_step_2}),
                   0.12);
}

TEST(OpUtilsTest, ExportSourceCRSAndTargetCRSToWKT) {
  auto extent = Extent::createFromBBOX(-10.0, 35.0, 20.0, 60.0);
  auto op_with_ids =
      MakeSimpleTransf(NameDomainProps("IdOp", extent),
                       GeographicCRS::EPSG_4326, GeographicCRS::EPSG_4269);

  // WKT2_2019: idOnTopLevelOnly() is false -> CRS IDs emitted, domains usage
  // disabled.
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
    fmt->startNode("COORDINATEOPERATION", /*hasId=*/true);
    absl::Cleanup end_node = [&]() { fmt->endNode(); };
    exportSourceCRSAndTargetCRSToWKT(op_with_ids.get(), fmt.get());
    std::move(end_node).Invoke();
    EXPECT_THAT(
        fmt->toString(),
        AllOf(HasSubstr("SOURCECRS["), HasSubstr("TARGETCRS["),
              HasSubstr("ID[\"EPSG\",4326]"), HasSubstr("ID[\"EPSG\",4269]")));
  }

  // WKT2_2019_SIMPLIFIED with topLevelHasId() == true -> canExportCRSId is
  // false.
  {
    auto fmt =
        WKTFormatter::create(WKTFormatter::Convention::WKT2_2019_SIMPLIFIED);
    fmt->startNode("COORDINATEOPERATION", /*hasId=*/true);
    absl::Cleanup end_node = [&]() { fmt->endNode(); };
    exportSourceCRSAndTargetCRSToWKT(op_with_ids.get(), fmt.get());
    std::move(end_node).Invoke();
    EXPECT_THAT(fmt->toString(),
                AllOf(HasSubstr("SOURCECRS["), HasSubstr("TARGETCRS["),
                      Not(HasSubstr("4326")), Not(HasSubstr("4269"))));
  }

  // WKT2 (2015 keywords): use2019Keywords() is false -> canExportCRSId is
  // false.
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    fmt->startNode("COORDINATEOPERATION", /*hasId=*/false);
    absl::Cleanup end_node = [&]() { fmt->endNode(); };
    exportSourceCRSAndTargetCRSToWKT(op_with_ids.get(), fmt.get());
    std::move(end_node).Invoke();
    EXPECT_THAT(fmt->toString(),
                AllOf(HasSubstr("SOURCECRS["), HasSubstr("TARGETCRS[")));
  }

  // WKT2_2019 with CRSs having no identifiers and operation having no domains.
  {
    auto src_no_id = MakeGeog2D("SrcNoId");
    auto tgt_no_id = MakeGeog2D("TgtNoId");
    auto op_no_ids =
        MakeSimpleTransf(NameProps("NoIdOp"), src_no_id, tgt_no_id);
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
    fmt->startNode("COORDINATEOPERATION", /*hasId=*/false);
    absl::Cleanup end_node = [&]() { fmt->endNode(); };
    exportSourceCRSAndTargetCRSToWKT(op_no_ids.get(), fmt.get());
    std::move(end_node).Invoke();
    EXPECT_THAT(fmt->toString(),
                AllOf(HasSubstr("SOURCECRS["), HasSubstr("SrcNoId"),
                      HasSubstr("TARGETCRS["), HasSubstr("TgtNoId")));
  }
}

}  // namespace
}  // namespace proj
