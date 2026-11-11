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
// Original C++20 GoogleTest unit tests for PROJ ISO 19111 CRS classes and
// helpers implemented in third_party/proj4/proj/src/iso19111/crs.cpp.
// Note: in google3 default non-GCE builds, proj.db is embedded via memvfs.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/crs.cpp

#include "third_party/proj4/proj/include/proj/crs.hpp"

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinateoperation.hpp"
#include "third_party/proj4/proj/include/proj/coordinatesystem.hpp"
#include "third_party/proj4/proj/include/proj/datum.hpp"
#include "third_party/proj4/proj/include/proj/internal/coordinatesystem_internal.hpp"
#include "third_party/proj4/proj/include/proj/internal/crs_internal.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

namespace p_common = NS_PROJ::common;
namespace p_crs = NS_PROJ::crs;
namespace p_cs = NS_PROJ::cs;
namespace p_datum = NS_PROJ::datum;
namespace p_io = NS_PROJ::io;
namespace p_meta = NS_PROJ::metadata;
namespace p_op = NS_PROJ::operation;
namespace p_util = NS_PROJ::util;

using ::testing::DoubleNear;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::IsNull;
using ::testing::Not;
using ::testing::NotNull;
using ::testing::SizeIs;

p_io::DatabaseContextNNPtr MakeDb(const ScopedContext& ctx) {
  return p_io::DatabaseContext::create("", {}, ctx.get());
}

p_util::PropertyMap MakeNameProps(const std::string& name) {
  p_util::PropertyMap props;
  props.set(p_common::IdentifiedObject::NAME_KEY, name);
  return props;
}

p_meta::IdentifierNNPtr MakeIdentifier(const std::string& auth,
                                       const std::string& code) {
  p_util::PropertyMap id_props;
  id_props.set(p_meta::Identifier::CODESPACE_KEY, auth);
  return p_meta::Identifier::create(code, id_props);
}

p_util::PropertyMap MakeNameCodeProps(const std::string& name,
                                      const std::string& auth,
                                      const std::string& code) {
  p_util::PropertyMap props;
  props.set(p_common::IdentifiedObject::NAME_KEY, name);
  props.set(p_common::IdentifiedObject::IDENTIFIERS_KEY,
            MakeIdentifier(auth, code));
  return props;
}

std::string ToWkt(
    const p_crs::CRSNNPtr& crs, p_io::WKTFormatter::Convention conv,
    const p_io::DatabaseContextPtr& db = nullptr, bool strict = true,
    std::optional<p_io::WKTFormatter::OutputAxisRule> axis_rule = std::nullopt,
    bool allow_ellps_height_vert = false, bool allow_linunit = false,
    bool id_top_level_only = false) {
  auto formatter = p_io::WKTFormatter::create(conv, db);
  formatter->setStrict(strict);
  formatter->setMultiLine(false);
  if (axis_rule.has_value()) {
    formatter->setOutputAxis(*axis_rule);
  }
  formatter->setAllowEllipsoidalHeightAsVerticalCRS(allow_ellps_height_vert);
  formatter->setAllowLINUNITNode(allow_linunit);
  if (id_top_level_only) {
    formatter->setOutputId(false);
  }
  return crs->exportToWKT(formatter.get());
}

std::string ToProj(const p_io::IPROJStringExportable& obj,
                   p_io::PROJStringFormatter::Convention conv =
                       p_io::PROJStringFormatter::Convention::PROJ_5,
                   const p_io::DatabaseContextPtr& db = nullptr,
                   bool crs_export = true, bool legacy_crs_to_crs = false,
                   bool omit_proj_longlat = false,
                   bool omit_z_unit_convert = false) {
  auto formatter = p_io::PROJStringFormatter::create(conv, db);
  formatter->setCRSExport(crs_export);
  formatter->setLegacyCRSToCRSContext(legacy_crs_to_crs);
  if (omit_proj_longlat) {
    formatter->setOmitProjLongLatIfPossible(true);
  }
  if (omit_z_unit_convert) {
    formatter->pushOmitZUnitConversion();
  }
  std::string out;
  if (crs_export) {
    out = obj.exportToPROJString(formatter.get());
  } else {
    obj._exportToPROJString(formatter.get());
    out = formatter->toString();
  }
  if (omit_z_unit_convert) {
    formatter->popOmitZUnitConversion();
  }
  if (omit_proj_longlat) {
    formatter->setOmitProjLongLatIfPossible(false);
  }
  return out;
}

std::string ToJson(const p_crs::CRSNNPtr& crs,
                   const p_io::DatabaseContextPtr& db = nullptr) {
  auto formatter = p_io::JSONFormatter::create(db);
  formatter->setMultiLine(false);
  return crs->exportToJSON(formatter.get());
}

p_op::ConversionNNPtr MakeMercatorConversion(const std::string& name = "Merc") {
  return p_op::Conversion::createMercatorVariantA(
      MakeNameProps(name),
      p_common::Angle(0.0, p_common::UnitOfMeasure::DEGREE),
      p_common::Angle(0.0, p_common::UnitOfMeasure::DEGREE),
      p_common::Scale(1.0), p_common::Length(0.0), p_common::Length(0.0));
}

p_datum::DynamicGeodeticReferenceFrameNNPtr MakeDynamicGeodeticDatum() {
  return p_datum::DynamicGeodeticReferenceFrame::create(
      MakeNameProps("Dynamic Geodetic Frame"), p_datum::Ellipsoid::WGS84,
      p_util::optional<std::string>("Anchor 2020"),
      p_datum::PrimeMeridian::GREENWICH,
      p_common::Measure(2015.0, p_common::UnitOfMeasure::YEAR),
      p_util::optional<std::string>("test_deform_model"));
}

p_datum::DynamicVerticalReferenceFrameNNPtr MakeDynamicVerticalDatum() {
  return p_datum::DynamicVerticalReferenceFrame::create(
      MakeNameProps("Dynamic Vertical Frame"),
      p_util::optional<std::string>("Vert Anchor"),
      p_util::optional<p_datum::RealizationMethod>(
          p_datum::RealizationMethod::GEOID),
      p_common::Measure(2016.5, p_common::UnitOfMeasure::YEAR),
      p_util::optional<std::string>("vert_deform_model"));
}

p_datum::DatumEnsembleNNPtr MakeGeodeticEnsemble() {
  std::vector<p_datum::DatumNNPtr> datums;
  datums.push_back(p_datum::GeodeticReferenceFrame::create(
      MakeNameProps("Member 1"), p_datum::Ellipsoid::WGS84,
      p_util::optional<std::string>(), p_datum::PrimeMeridian::GREENWICH));
  datums.push_back(p_datum::GeodeticReferenceFrame::create(
      MakeNameProps("Member 2"), p_datum::Ellipsoid::WGS84,
      p_util::optional<std::string>(), p_datum::PrimeMeridian::GREENWICH));
  return p_datum::DatumEnsemble::create(
      MakeNameProps("World Geodetic System 1984 ensemble"), datums,
      p_meta::PositionalAccuracy::create("2.0"));
}

// ---------------------------------------------------------------------------
// Group 1: Base CRS properties, metadata, extraction, cloning, alteration
// ---------------------------------------------------------------------------

TEST(CrsTest, BaseMetadataPropertiesAndCloning) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");

  // EXTENSION_PROJ4, OVER, REMARKS handling, and setProperties edge cases.
  {
    p_util::PropertyMap props;
    props.set(p_common::IdentifiedObject::NAME_KEY, "Custom Geog");
    props.set("EXTENSION_PROJ4", "+proj=longlat +ellps=WGS84 +type=crs");
    props.set("OVER", true);
    props.set(p_common::IdentifiedObject::REMARKS_KEY, "Custom remark");
    auto crs = p_crs::GeographicCRS::create(
        props, p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(crs->getExtensionProj4(),
                Eq("+proj=longlat +ellps=WGS84 +type=crs"));
    EXPECT_TRUE(crs->hasOver());
    EXPECT_FALSE(crs->hasImplicitCS());
    EXPECT_THAT(
        crs->remarks(),
        Eq("PROJ CRS string: +proj=longlat +ellps=WGS84 +type=crs. Custom "
           "remark"));

    p_util::PropertyMap props_rem_only;
    props_rem_only.set(p_common::IdentifiedObject::NAME_KEY, "Remarks Only");
    props_rem_only.set(p_common::IdentifiedObject::REMARKS_KEY,
                       "PROJ CRS string: +proj=longlat +over");
    auto crs_rem = p_crs::GeographicCRS::create(
        props_rem_only, p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(crs_rem->getExtensionProj4(), Eq("+proj=longlat +over"));
    EXPECT_FALSE(crs_rem->hasOver());
    EXPECT_THAT(crs_rem->remarks(), Eq("PROJ CRS string: +proj=longlat +over"));

    p_util::PropertyMap props_dot_remarks;
    props_dot_remarks.set(p_common::IdentifiedObject::NAME_KEY, "Dot Remarks");
    props_dot_remarks.set(
        p_common::IdentifiedObject::REMARKS_KEY,
        "PROJ CRS string: +proj=longlat. Extra remark preserved");
    auto crs_dot = p_crs::GeographicCRS::create(
        props_dot_remarks, p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(crs_dot->getExtensionProj4(), Eq("+proj=longlat"));
    EXPECT_THAT(crs_dot->remarks(),
                Eq("PROJ CRS string: +proj=longlat. Extra remark preserved"));

    p_util::PropertyMap bad_ext_props;
    bad_ext_props.set(p_common::IdentifiedObject::NAME_KEY, "Bad Ext");
    bad_ext_props.set("EXTENSION_PROJ4", 42);
    EXPECT_THROW(p_crs::GeographicCRS::create(
                     bad_ext_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
                     p_cs::EllipsoidalCS::createLatitudeLongitude(
                         p_common::UnitOfMeasure::DEGREE)),
                 p_util::InvalidValueTypeException);
  }

  // isDynamic variants across static, dynamic frames, WGS84 datum/ensemble,
  // and vertical frames.
  {
    auto static_crs = p_crs::GeographicCRS::EPSG_4269;
    EXPECT_FALSE(static_crs->isDynamic(false));
    EXPECT_FALSE(static_crs->isDynamic(true));

    auto dyn_geod = p_crs::GeographicCRS::create(
        MakeNameProps("DynGeog"), MakeDynamicGeodeticDatum(),
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_TRUE(dyn_geod->isDynamic(false));

    auto wgs84_datum_geog = p_crs::GeographicCRS::create(
        MakeNameProps("WGS84 Datum"),
        p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_FALSE(wgs84_datum_geog->isDynamic(false));
    EXPECT_TRUE(wgs84_datum_geog->isDynamic(true));

    auto wgs84_ens_geog = p_crs::GeographicCRS::create(
        MakeNameProps("WGS84 Ens"), nullptr, MakeGeodeticEnsemble(),
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_FALSE(wgs84_ens_geog->isDynamic(false));
    EXPECT_TRUE(wgs84_ens_geog->isDynamic(true));

    auto dyn_vert = p_crs::VerticalCRS::create(
        MakeNameProps("DynVert"), MakeDynamicVerticalDatum(),
        p_cs::VerticalCS::createGravityRelatedHeight(
            p_common::UnitOfMeasure::METRE));
    EXPECT_TRUE(dyn_vert->isDynamic(false));

    auto static_vert = p_crs::VerticalCRS::create(
        MakeNameProps("StaticVert"),
        p_datum::VerticalReferenceFrame::create(MakeNameProps("Static VDatum")),
        p_cs::VerticalCS::createGravityRelatedHeight(
            p_common::UnitOfMeasure::METRE));
    EXPECT_FALSE(static_vert->isDynamic(false));
  }

  // extractGeodeticCRS, extractGeographicCRS, extractVerticalCRS across CRS
  // hierarchy types.
  {
    auto geocentric = p_crs::GeodeticCRS::EPSG_4978;
    EXPECT_THAT(geocentric->extractGeodeticCRS(), NotNull());
    EXPECT_THAT(geocentric->extractGeographicCRS(), IsNull());
    EXPECT_THAT(geocentric->extractVerticalCRS(), IsNull());

    auto proj = auth_epsg->createProjectedCRS("32631");
    EXPECT_THAT(proj->extractGeodeticCRS(), NotNull());
    EXPECT_THAT(proj->extractGeographicCRS(), NotNull());
    EXPECT_THAT(proj->extractVerticalCRS(), IsNull());

    auto vert = auth_epsg->createVerticalCRS("5703");
    EXPECT_THAT(vert->extractGeodeticCRS(), IsNull());
    EXPECT_THAT(vert->extractVerticalCRS(), NotNull());

    auto compound =
        p_crs::CompoundCRS::create(MakeNameProps("3D Compound"), {proj, vert});
    EXPECT_THAT(compound->extractGeodeticCRS(), NotNull());
    EXPECT_THAT(compound->extractGeographicCRS(), NotNull());
    EXPECT_THAT(compound->extractVerticalCRS(), NotNull());

    auto bound = p_crs::BoundCRS::createFromTOWGS84(compound, {1, 2, 3});
    EXPECT_THAT(bound->extractGeodeticCRS(), NotNull());
    EXPECT_THAT(bound->extractVerticalCRS(), NotNull());

    auto affine_cs = p_cs::AffineCS::create(
        p_util::PropertyMap(),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("i"), "i",
                                           p_cs::AxisDirection::COLUMN_POSITIVE,
                                           p_common::UnitOfMeasure::METRE),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("j"), "j",
                                           p_cs::AxisDirection::ROW_POSITIVE,
                                           p_common::UnitOfMeasure::METRE));
    auto derived_proj = p_crs::DerivedProjectedCRS::create(
        MakeNameProps("DerivedProj"), proj,
        p_op::Conversion::createAffineParametric(
            MakeNameProps("Aff"), p_common::Length(0.0), p_common::Scale(1.0),
            p_common::Scale(0.0), p_common::Length(0.0), p_common::Scale(0.0),
            p_common::Scale(1.0)),
        affine_cs);
    EXPECT_THAT(derived_proj->extractGeodeticCRS(), NotNull());
    EXPECT_THAT(derived_proj->extractGeographicCRS(), NotNull());
  }

  // getNonDeprecated across valid, deprecated, no-ID, and non-DB CRS types.
  {
    auto depr_geog = auth_epsg->createCoordinateReferenceSystem("4035");
    auto non_depr = depr_geog->getNonDeprecated(db);
    EXPECT_THAT(non_depr, SizeIs(1));

    auto valid_geog = auth_epsg->createCoordinateReferenceSystem("4326");
    auto valid_list = valid_geog->getNonDeprecated(db);
    EXPECT_THAT(valid_list, IsEmpty());

    auto depr_proj = auth_epsg->createCoordinateReferenceSystem("2036");
    EXPECT_THAT(depr_proj->getNonDeprecated(db), SizeIs(1));

    auto depr_vert = auth_epsg->createCoordinateReferenceSystem("5704");
    EXPECT_THAT(depr_vert->getNonDeprecated(db), SizeIs(1));

    auto depr_compound = auth_epsg->createCoordinateReferenceSystem("6871");
    EXPECT_THAT(depr_compound->getNonDeprecated(db), SizeIs(1));

    auto unknown_geog = p_crs::GeographicCRS::create(
        MakeNameProps("Unregistered"),
        p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(unknown_geog->getNonDeprecated(db), IsEmpty());

    auto eng_crs = p_crs::EngineeringCRS::create(
        MakeNameProps("LocalEng"),
        p_datum::EngineeringDatum::create(MakeNameProps("EngDatum")),
        p_cs::CartesianCS::createEastingNorthing(
            p_common::UnitOfMeasure::METRE));
    EXPECT_THAT(eng_crs->getNonDeprecated(db), IsEmpty());
  }

  // getOriginatingAuthName, shallowClone, alterName, alterId, alterGeodeticCRS.
  {
    EXPECT_THAT(p_crs::GeographicCRS::EPSG_4326->getOriginatingAuthName(),
                Eq("EPSG"));

    auto no_id = p_crs::GeographicCRS::create(
        MakeNameProps("NoID"), p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(no_id->getOriginatingAuthName(), IsEmpty());

    p_util::PropertyMap multi_id_props;
    multi_id_props.set(p_common::IdentifiedObject::NAME_KEY, "MultiID");
    auto id_arr = p_util::ArrayOfBaseObject::create();
    id_arr->add(MakeIdentifier("EPSG", "4326"));
    id_arr->add(MakeIdentifier("OGC", "CRS84"));
    multi_id_props.set(p_common::IdentifiedObject::IDENTIFIERS_KEY, id_arr);
    auto multi_id_crs = p_crs::GeographicCRS::create(
        multi_id_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(multi_id_crs->getOriginatingAuthName(), IsEmpty());

    auto promoted_with_auth =
        p_crs::GeographicCRS::EPSG_4269->promoteTo3D("", db);
    EXPECT_THAT(promoted_with_auth->getOriginatingAuthName(), Eq("EPSG"));

    p_util::PropertyMap bad_remark_props;
    bad_remark_props.set(p_common::IdentifiedObject::NAME_KEY, "BadRemark");
    bad_remark_props.set(p_common::IdentifiedObject::REMARKS_KEY,
                         "Promoted to 3D from nocolon");
    auto bad_remark_crs = p_crs::GeographicCRS::create(
        bad_remark_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(bad_remark_crs->getOriginatingAuthName(), IsEmpty());

    auto cloned = p_crs::GeographicCRS::EPSG_4326->shallowClone();
    EXPECT_TRUE(cloned->isEquivalentTo(p_crs::GeographicCRS::EPSG_4326.get()));

    p_util::PropertyMap depr_props;
    depr_props.set(p_common::IdentifiedObject::NAME_KEY, "OldName");
    depr_props.set(p_common::IdentifiedObject::DEPRECATED_KEY, true);
    auto depr_crs = p_crs::GeographicCRS::create(
        depr_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(depr_crs->alterName("NewName")->nameStr(), Eq("NewName"));
    EXPECT_THAT(depr_crs->alterName("NewName (deprecated)")->nameStr(),
                Eq("NewName"));

    auto altered_id = p_crs::GeographicCRS::EPSG_4326->alterId("CUSTOM", "999");
    ASSERT_THAT(altered_id->identifiers(), SizeIs(1));
    EXPECT_THAT(*altered_id->identifiers()[0]->codeSpace(), Eq("CUSTOM"));
    EXPECT_THAT(altered_id->identifiers()[0]->code(), Eq("999"));

    auto proj = auth_epsg->createProjectedCRS("32631");
    auto altered_proj = proj->alterGeodeticCRS(p_crs::GeographicCRS::EPSG_4269);
    auto altered_proj_cast =
        dynamic_cast<const p_crs::ProjectedCRS*>(altered_proj.get());
    ASSERT_THAT(altered_proj_cast, NotNull());
    EXPECT_THAT(altered_proj_cast->baseCRS()->nameStr(), Eq("NAD83"));

    auto vert = auth_epsg->createVerticalCRS("5703");
    auto compound =
        p_crs::CompoundCRS::create(MakeNameProps("Comp"), {proj, vert});
    auto altered_comp =
        compound->alterGeodeticCRS(p_crs::GeographicCRS::EPSG_4269);
    EXPECT_NE(altered_comp.get(), compound.get());
    EXPECT_EQ(vert->alterGeodeticCRS(p_crs::GeographicCRS::EPSG_4269).get(),
              vert.get());
  }
}

// ---------------------------------------------------------------------------
// Group 2: promoteTo3D, demoteTo2D, axis switching/normalization, linear unit
// alteration, and createBoundCRSToWGS84IfPossible
// ---------------------------------------------------------------------------

TEST(CrsTest, PromoteDemoteAxisSwitchUnitAndBoundToWgs84) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");

  // promoteTo3D across Geographic, Projected, Compound, Bound, and custom CS.
  {
    auto g4269 = auth_epsg->createGeographicCRS("4269");
    auto g4269_3d_auto = g4269->promoteTo3D("", db);
    EXPECT_THAT(g4269_3d_auto->nameStr(), Eq("NAD83"));
    EXPECT_THAT(g4269_3d_auto->remarks(), HasSubstr("Promoted to 3D from"));

    auto g4269_3d_named = g4269->promoteTo3D("NAD83-3D", nullptr);
    EXPECT_THAT(g4269_3d_named->nameStr(), Eq("NAD83-3D"));

    auto g4326_3d = p_crs::GeographicCRS::EPSG_4326->promoteTo3D("", db);
    ASSERT_THAT(g4326_3d->identifiers(), SizeIs(1));
    EXPECT_THAT(g4326_3d->identifiers()[0]->code(), Eq("4979"));

    auto foot_v_axis = p_cs::CoordinateSystemAxis::create(
        MakeNameProps(p_cs::AxisName::Ellipsoidal_height),
        p_cs::AxisAbbreviation::h, p_cs::AxisDirection::UP,
        p_common::UnitOfMeasure::FOOT);
    auto g4269_3d_ft = g4269->promoteTo3D("NAD83-ft", db, foot_v_axis);
    EXPECT_THAT(g4269_3d_ft->extractGeographicCRS()
                    ->coordinateSystem()
                    ->axisList()[2]
                    ->unit(),
                Eq(p_common::UnitOfMeasure::FOOT));
    auto proj_2d = auth_epsg->createProjectedCRS("32631");
    auto proj_2d_ft = proj_2d->alterCSLinearUnit(p_common::UnitOfMeasure::FOOT);
    auto proj_3d_ft = proj_2d_ft->promoteTo3D("UTM31N-ft", db, foot_v_axis);
    auto proj_3d_cast =
        dynamic_cast<const p_crs::ProjectedCRS*>(proj_3d_ft.get());
    ASSERT_THAT(proj_3d_cast, NotNull());
    EXPECT_THAT(proj_3d_cast->coordinateSystem()->axisList(), SizeIs(3));
    EXPECT_THAT(proj_3d_cast->coordinateSystem()->axisList()[2]->unit(),
                Eq(p_common::UnitOfMeasure::FOOT));

    auto vert_5703 = auth_epsg->createVerticalCRS("5703");
    auto compound =
        p_crs::CompoundCRS::create(MakeNameProps("P+V"), {proj_2d, vert_5703});
    EXPECT_EQ(compound->promoteTo3D("", db).get(), compound.get());

    auto bound_towgs84 =
        p_crs::BoundCRS::createFromTOWGS84(g4269, {1, 2, 3, 0, 0, 0, 0});
    auto promoted_bound_wgs = bound_towgs84->promoteTo3D("Bound3D", nullptr);
    EXPECT_NE(promoted_bound_wgs.get(), bound_towgs84.get());

    auto non_wgs_transf = p_op::Transformation::createGeocentricTranslations(
        MakeNameProps("ToNAD83"), p_crs::GeographicCRS::EPSG_4267, g4269, 1.0,
        2.0, 3.0, {});
    auto bound_non_wgs = p_crs::BoundCRS::create(
        p_crs::GeographicCRS::EPSG_4267, g4269, non_wgs_transf);
    auto promoted_bound_non = bound_non_wgs->promoteTo3D("BoundNonWgs", db);
    EXPECT_THAT(promoted_bound_non->nameStr(), Eq("BoundNonWgs"));
  }

  // demoteTo2D on base CRS*, DerivedGeographicCRS, DerivedProjectedCRS,
  // CompoundCRS, BoundCRS, and fallback VerticalCRS.
  {
    auto g4979 = auth_epsg->createGeographicCRS("4979");
    auto demoted_g = g4979->demoteTo2D("", db);
    ASSERT_THAT(demoted_g->identifiers(), SizeIs(1));
    EXPECT_THAT(demoted_g->identifiers()[0]->code(), Eq("4326"));

    auto g4326_already2d =
        p_crs::GeographicCRS::EPSG_4326->demoteTo2D("Keep", db);
    EXPECT_EQ(g4326_already2d.get(), p_crs::GeographicCRS::EPSG_4326.get());

    auto proj_3d = auth_epsg->createProjectedCRS("32631")->promoteTo3D("", db);
    auto demoted_p = proj_3d->demoteTo2D("", db);
    EXPECT_THAT(demoted_p->nameStr(), Eq("WGS 84 / UTM zone 31N"));
    EXPECT_THAT(demoted_p->extractGeographicCRS(), NotNull());

    auto proj_2d_ref = auth_epsg->createProjectedCRS("32631");
    auto proj_3d_with_epsg_id = p_crs::ProjectedCRS::create(
        MakeNameCodeProps("WGS 84 / UTM zone 31N", "EPSG", "999999"), g4979,
        proj_2d_ref->derivingConversion(),
        p_util::nn_dynamic_pointer_cast<p_crs::ProjectedCRS>(proj_3d)
            ->coordinateSystem());
    auto demoted_p_db = proj_3d_with_epsg_id->CRS::demoteTo2D("", db);
    EXPECT_THAT(demoted_p_db->identifiers(), IsEmpty());
    ASSERT_THAT(demoted_p_db->extractGeographicCRS(), NotNull());
    ASSERT_THAT(demoted_p_db->extractGeographicCRS()->identifiers(), SizeIs(1));
    EXPECT_THAT(demoted_p_db->extractGeographicCRS()->identifiers()[0]->code(),
                Eq("4326"));

    auto vert_5703 = auth_epsg->createVerticalCRS("5703");
    EXPECT_THROW(p_crs::CompoundCRS::create(MakeNameProps("BadComp"),
                                            {proj_3d, vert_5703}),
                 p_crs::InvalidCompoundCRSException);
    auto comp_proj_vert = p_crs::CompoundCRS::create(MakeNameProps("Comp"),
                                                     {proj_2d_ref, vert_5703});
    EXPECT_THAT(comp_proj_vert->stripVerticalComponent()->nameStr(),
                Eq("WGS 84 / UTM zone 31N"));

    auto bound_3d = p_crs::BoundCRS::createFromTOWGS84(g4979, {1, 2, 3});
    EXPECT_NE(bound_3d->demoteTo2D("DemBound", db).get(), bound_3d.get());

    EXPECT_EQ(vert_5703->demoteTo2D("", db).get(), vert_5703.get());

    auto dg_3d = p_crs::DerivedGeographicCRS::create(
        MakeNameProps("DG3D"), g4979,
        p_op::Conversion::createPoleRotationGRIBConvention(
            MakeNameProps("Rot"),
            p_common::Angle(-30.0, p_common::UnitOfMeasure::DEGREE),
            p_common::Angle(10.0, p_common::UnitOfMeasure::DEGREE),
            p_common::Angle(0.0, p_common::UnitOfMeasure::DEGREE)),
        p_cs::EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
            p_common::UnitOfMeasure::DEGREE, p_common::UnitOfMeasure::METRE));
    auto dg_2d = dg_3d->demoteTo2D("DG2D", db);
    EXPECT_THAT(dg_2d->coordinateSystem()->axisList(), SizeIs(2));
    EXPECT_EQ(dg_2d->demoteTo2D("", db).get(), dg_2d.get());
    const p_crs::CRS* dg_as_base = dg_3d.get();
    auto dg_demoted_base = dg_as_base->demoteTo2D("", db);
    EXPECT_THAT(
        dynamic_cast<const p_crs::DerivedGeographicCRS*>(dg_demoted_base.get()),
        NotNull());

    auto proj_2d = auth_epsg->createProjectedCRS("32631");
    auto dp_3d = p_crs::DerivedProjectedCRS::create(
        MakeNameProps("DP3D"), proj_2d,
        p_op::Conversion::createAffineParametric(
            MakeNameProps("Aff3D"), p_common::Length(0.0), p_common::Scale(1.0),
            p_common::Scale(0.0), p_common::Length(0.0), p_common::Scale(0.0),
            p_common::Scale(1.0)),
        p_cs::CartesianCS::createGeocentric(p_common::UnitOfMeasure::METRE));
    auto dp_2d = dp_3d->demoteTo2D("DP2D", db);
    EXPECT_THAT(dp_2d->coordinateSystem()->axisList(), SizeIs(2));
    EXPECT_EQ(dp_2d->demoteTo2D("", db).get(), dp_2d.get());
    const p_crs::CRS* dp_as_base = dp_3d.get();
    auto dp_demoted_base = dp_as_base->demoteTo2D("", db);
    EXPECT_THAT(
        dynamic_cast<const p_crs::DerivedProjectedCRS*>(dp_demoted_base.get()),
        NotNull());
  }

  // mustAxisOrderBeSwitchedForVisualization, applyAxisOrderReversal,
  // normalizeForVisualization (2D, 3D, UPS North/South, Compound, Bound,
  // DerivedProjected, Vertical error, suffix stripping).
  {
    auto g4326 = p_crs::GeographicCRS::EPSG_4326;
    EXPECT_TRUE(g4326->mustAxisOrderBeSwitchedForVisualization());
    auto g4326_norm = g4326->normalizeForVisualization();
    EXPECT_THAT(g4326_norm->nameStr(),
                HasSubstr(NORMALIZED_AXIS_ORDER_SUFFIX_STR));
    EXPECT_FALSE(g4326_norm->mustAxisOrderBeSwitchedForVisualization());
    auto g4326_re_rev =
        g4326_norm->applyAxisOrderReversal(AXIS_ORDER_REVERSED_SUFFIX_STR);
    EXPECT_THAT(g4326_re_rev->nameStr(), Eq("WGS 84"));
    EXPECT_THAT(g4326->applyAxisOrderReversal(AXIS_ORDER_REVERSED_SUFFIX_STR)
                    ->nameStr(),
                Eq("WGS 84" AXIS_ORDER_REVERSED_SUFFIX_STR));

    auto g4979 = auth_epsg->createGeographicCRS("4979");
    EXPECT_TRUE(g4979->mustAxisOrderBeSwitchedForVisualization());
    EXPECT_THAT(g4979->normalizeForVisualization()->nameStr(),
                HasSubstr(NORMALIZED_AXIS_ORDER_SUFFIX_STR));

    // Polar UPS North (180 deg / 90 deg with SOUTH/SOUTH axes).
    auto ups_n = auth_epsg->createProjectedCRS("32661");
    EXPECT_TRUE(ups_n->mustAxisOrderBeSwitchedForVisualization());
    auto ups_n_norm = ups_n->normalizeForVisualization();
    auto ups_n_cast =
        dynamic_cast<const p_crs::ProjectedCRS*>(ups_n_norm.get());
    ASSERT_THAT(ups_n_cast, NotNull());
    EXPECT_THAT(ups_n_cast->coordinateSystem()->axisList()[0]->nameStr(),
                HasSubstr("Easting"));

    // Polar UPS South (0 deg / 90 deg with NORTH/NORTH axes).
    auto ups_s = auth_epsg->createProjectedCRS("32761");
    EXPECT_TRUE(ups_s->mustAxisOrderBeSwitchedForVisualization());
    EXPECT_THAT(ups_s->normalizeForVisualization()->nameStr(),
                HasSubstr(NORMALIZED_AXIS_ORDER_SUFFIX_STR));

    // DerivedProjectedCRS 2D (Northing/Easting) & CompoundCRS & BoundCRS.
    auto proj_ne = p_crs::ProjectedCRS::create(
        MakeNameProps("ProjNE"), g4326, MakeMercatorConversion(),
        p_cs::CartesianCS::createNorthingEasting(
            p_common::UnitOfMeasure::METRE));
    auto dp_ne = p_crs::DerivedProjectedCRS::create(
        MakeNameProps("DerNE"), proj_ne,
        p_op::Conversion::createAffineParametric(
            MakeNameProps("Aff"), p_common::Length(0.0), p_common::Scale(1.0),
            p_common::Scale(0.0), p_common::Length(0.0), p_common::Scale(0.0),
            p_common::Scale(1.0)),
        p_cs::CartesianCS::createNorthingEasting(
            p_common::UnitOfMeasure::METRE));
    EXPECT_TRUE(dp_ne->mustAxisOrderBeSwitchedForVisualization());
    EXPECT_FALSE(dp_ne->normalizeForVisualization()
                     ->mustAxisOrderBeSwitchedForVisualization());

    auto vert_5703 = auth_epsg->createVerticalCRS("5703");
    auto comp_rev = p_crs::CompoundCRS::create(MakeNameProps("CompRev"),
                                               {g4326, vert_5703});
    EXPECT_TRUE(comp_rev->mustAxisOrderBeSwitchedForVisualization());
    EXPECT_FALSE(comp_rev->normalizeForVisualization()
                     ->mustAxisOrderBeSwitchedForVisualization());

    auto bound_rev = p_crs::BoundCRS::createFromTOWGS84(g4326, {1, 2, 3});
    EXPECT_FALSE(bound_rev->mustAxisOrderBeSwitchedForVisualization());
    auto bound_norm = bound_rev->normalizeForVisualization();
    EXPECT_FALSE(bound_norm->mustAxisOrderBeSwitchedForVisualization());

    EXPECT_THROW(
        vert_5703->applyAxisOrderReversal(AXIS_ORDER_REVERSED_SUFFIX_STR),
        p_util::UnsupportedOperationException);
  }

  // alterCSLinearUnit across Projected, Geocentric, Geog3D, Vertical,
  // Engineering (Cartesian/Vertical/Ordinal), DerivedProjected, Compound,
  // Bound, and Geog2D fallback.
  {
    auto ft = p_common::UnitOfMeasure::FOOT;
    auto proj = auth_epsg->createProjectedCRS("32631");
    auto proj_ft_crs = proj->alterCSLinearUnit(ft);
    auto proj_ft = dynamic_cast<const p_crs::ProjectedCRS*>(proj_ft_crs.get());
    ASSERT_THAT(proj_ft, NotNull());
    EXPECT_THAT(proj_ft->coordinateSystem()->axisList()[0]->unit(), Eq(ft));

    auto geoc_ft_crs = p_crs::GeodeticCRS::EPSG_4978->alterCSLinearUnit(ft);
    auto geoc_ft = dynamic_cast<const p_crs::GeodeticCRS*>(geoc_ft_crs.get());
    ASSERT_THAT(geoc_ft, NotNull());
    EXPECT_THAT(geoc_ft->coordinateSystem()->axisList()[0]->unit(), Eq(ft));

    auto geog3d_ft_crs =
        auth_epsg->createGeographicCRS("4979")->alterCSLinearUnit(ft);
    auto geog3d_ft =
        dynamic_cast<const p_crs::GeographicCRS*>(geog3d_ft_crs.get());
    ASSERT_THAT(geog3d_ft, NotNull());
    EXPECT_THAT(geog3d_ft->coordinateSystem()->axisList()[2]->unit(), Eq(ft));

    auto vert = auth_epsg->createVerticalCRS("5703");
    auto vert_ft_crs = vert->alterCSLinearUnit(ft);
    auto vert_ft = dynamic_cast<const p_crs::VerticalCRS*>(vert_ft_crs.get());
    ASSERT_THAT(vert_ft, NotNull());
    EXPECT_THAT(vert_ft->coordinateSystem()->axisList()[0]->unit(), Eq(ft));

    auto eng_datum = p_datum::EngineeringDatum::create(MakeNameProps("ED"));
    auto eng_cart =
        p_crs::EngineeringCRS::create(MakeNameProps("EngCart"), eng_datum,
                                      p_cs::CartesianCS::createEastingNorthing(
                                          p_common::UnitOfMeasure::METRE));
    auto eng_cart_ft_crs = eng_cart->alterCSLinearUnit(ft);
    auto eng_cart_ft =
        dynamic_cast<const p_crs::EngineeringCRS*>(eng_cart_ft_crs.get());
    ASSERT_THAT(eng_cart_ft, NotNull());
    EXPECT_THAT(eng_cart_ft->coordinateSystem()->axisList()[0]->unit(), Eq(ft));

    auto eng_vert = p_crs::EngineeringCRS::create(
        MakeNameProps("EngVert"), eng_datum,
        p_cs::VerticalCS::createGravityRelatedHeight(
            p_common::UnitOfMeasure::METRE));
    EXPECT_NE(eng_vert->alterCSLinearUnit(ft).get(), eng_vert.get());

    auto ord_axis = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("Idx"), "Idx", p_cs::AxisDirection::COLUMN_POSITIVE,
        p_common::UnitOfMeasure::NONE);
    auto eng_ord = p_crs::EngineeringCRS::create(
        MakeNameProps("EngOrd"), eng_datum,
        p_cs::OrdinalCS::create(p_util::PropertyMap(), {ord_axis}));
    EXPECT_EQ(eng_ord->alterCSLinearUnit(ft).get(), eng_ord.get());

    auto dp_cart = p_crs::DerivedProjectedCRS::create(
        MakeNameProps("DPCart"), proj,
        p_op::Conversion::createAffineParametric(
            MakeNameProps("Aff"), p_common::Length(0.0), p_common::Scale(1.0),
            p_common::Scale(0.0), p_common::Length(0.0), p_common::Scale(0.0),
            p_common::Scale(1.0)),
        p_cs::CartesianCS::createEastingNorthing(
            p_common::UnitOfMeasure::METRE));
    EXPECT_NE(dp_cart->alterCSLinearUnit(ft).get(), dp_cart.get());

    auto affine_cs = p_cs::AffineCS::create(
        p_util::PropertyMap(),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("i"), "i",
                                           p_cs::AxisDirection::COLUMN_POSITIVE,
                                           p_common::UnitOfMeasure::METRE),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("j"), "j",
                                           p_cs::AxisDirection::ROW_POSITIVE,
                                           p_common::UnitOfMeasure::METRE));
    auto dp_affine = p_crs::DerivedProjectedCRS::create(
        MakeNameProps("DPAff"), proj,
        p_op::Conversion::createAffineParametric(
            MakeNameProps("Aff"), p_common::Length(0.0), p_common::Scale(1.0),
            p_common::Scale(0.0), p_common::Length(0.0), p_common::Scale(0.0),
            p_common::Scale(1.0)),
        affine_cs);
    EXPECT_NE(dp_affine->alterCSLinearUnit(ft).get(), dp_affine.get());

    auto comp_ft =
        p_crs::CompoundCRS::create(MakeNameProps("Comp"), {proj, vert})
            ->alterCSLinearUnit(ft);
    EXPECT_THAT(comp_ft->extractVerticalCRS()
                    ->coordinateSystem()
                    ->axisList()[0]
                    ->unit(),
                Eq(ft));

    auto bound_ft = p_crs::BoundCRS::createFromTOWGS84(proj, {1, 2, 3})
                        ->alterCSLinearUnit(ft);
    EXPECT_NE(bound_ft.get(), nullptr);

    EXPECT_EQ(p_crs::GeographicCRS::EPSG_4326->alterCSLinearUnit(ft).get(),
              p_crs::GeographicCRS::EPSG_4326.get());
  }

  // createBoundCRSToWGS84IfPossible edge branches.
  {
    using Interm = p_op::CoordinateOperationContext::IntermediateCRSUse;
    auto g4807 = auth_epsg->createGeographicCRS("4807");  // NTF (Paris)
    EXPECT_EQ(
        g4807->createBoundCRSToWGS84IfPossible(nullptr, Interm::ALWAYS).get(),
        g4807.get());

    auto bound_4326 = p_crs::BoundCRS::createFromTOWGS84(g4807, {1, 2, 3});
    EXPECT_EQ(
        bound_4326->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS).get(),
        bound_4326.get());
    auto base_canon = bound_4326->baseCRSWithCanonicalBoundCRS();
    EXPECT_EQ(
        base_canon->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS).get(),
        base_canon->canonicalBoundCRS().get());

    // Geocentric EPSG:4978 returns itself; EPSG:4936 returns BoundCRS if
    // unique Helmert exists or itself.
    EXPECT_EQ(p_crs::GeodeticCRS::EPSG_4978
                  ->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS)
                  .get(),
              p_crs::GeodeticCRS::EPSG_4978.get());
    auto g4936 = auth_epsg->createGeodeticCRS("4936");
    EXPECT_NE(g4936->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS).get(),
              nullptr);
    EXPECT_EQ(p_crs::GeographicCRS::EPSG_4326
                  ->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS)
                  .get(),
              p_crs::GeographicCRS::EPSG_4326.get());

    // NTF (Paris) EPSG:4807 uses longitude rotation + Helmert ConcatenatedOp.
    auto bound_4807 =
        g4807->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS);
    EXPECT_THAT(dynamic_cast<const p_crs::BoundCRS*>(bound_4807.get()),
                NotNull());

    // NTF (Paris) / Lambert zone II (EPSG:27572) and ED50 / UTM zone 31N
    // (EPSG:23031).
    auto p27572 = auth_epsg->createProjectedCRS("27572");
    auto bound_27572 =
        p27572->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS);
    EXPECT_THAT(dynamic_cast<const p_crs::BoundCRS*>(bound_27572.get()),
                NotNull());
    auto p23031 = auth_epsg->createProjectedCRS("23031");
    auto bound_23031 =
        p23031->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS);
    EXPECT_NE(bound_23031.get(), nullptr);

    // VerticalCRS EPSG:5703 (NAVD88) exercises gridsNeeded branch.
    auto v5703 = auth_epsg->createVerticalCRS("5703");
    EXPECT_NE(v5703
                  ->createBoundCRSToWGS84IfPossible(
                      db, Interm::IF_NO_DIRECT_TRANSFORMATION)
                  .get(),
              nullptr);
    EXPECT_NE(v5703->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS).get(),
              nullptr);

    // 2 domains: nested extents vs disjoint extents.
    p_util::PropertyMap props_nested;
    props_nested.set(p_common::IdentifiedObject::NAME_KEY, "NestedDomains");
    auto dom_arr_nested = p_util::ArrayOfBaseObject::create();
    dom_arr_nested->add(p_common::ObjectDomain::create(
        p_util::optional<std::string>("outer"),
        p_meta::Extent::createFromBBOX(-10.0, 40.0, 20.0, 60.0).as_nullable()));
    dom_arr_nested->add(p_common::ObjectDomain::create(
        p_util::optional<std::string>("inner"),
        p_meta::Extent::createFromBBOX(0.0, 42.0, 10.0, 50.0).as_nullable()));
    props_nested.set(p_common::ObjectUsage::OBJECT_DOMAIN_KEY, dom_arr_nested);
    auto nested_geog = p_crs::GeographicCRS::create(
        props_nested, p_datum::GeodeticReferenceFrame::EPSG_6267,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_NE(
        nested_geog->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS).get(),
        nullptr);

    p_util::PropertyMap props_disjoint;
    props_disjoint.set(p_common::IdentifiedObject::NAME_KEY, "DisjointDomains");
    auto dom_arr_disjoint = p_util::ArrayOfBaseObject::create();
    dom_arr_disjoint->add(p_common::ObjectDomain::create(
        p_util::optional<std::string>("d1"),
        p_meta::Extent::createFromBBOX(-10.0, 40.0, 0.0, 50.0).as_nullable()));
    dom_arr_disjoint->add(p_common::ObjectDomain::create(
        p_util::optional<std::string>("d2"),
        p_meta::Extent::createFromBBOX(10.0, 40.0, 20.0, 50.0).as_nullable()));
    props_disjoint.set(p_common::ObjectUsage::OBJECT_DOMAIN_KEY,
                       dom_arr_disjoint);
    auto disjoint_geog = p_crs::GeographicCRS::create(
        props_disjoint, p_datum::GeodeticReferenceFrame::EPSG_6267,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_EQ(disjoint_geog->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS)
                  .get(),
              disjoint_geog.get());

    // CompoundCRS with 2 components vs 3 components (Temporal).
    auto comp2 =
        p_crs::CompoundCRS::create(MakeNameProps("Comp2"), {g4807, v5703});
    auto comp2_bound =
        comp2->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS);
    EXPECT_THAT(dynamic_cast<const p_crs::CompoundCRS*>(comp2_bound.get()),
                NotNull());
    auto time_crs = p_crs::TemporalCRS::create(
        MakeNameProps("Time"),
        p_datum::TemporalDatum::create(
            MakeNameProps("TD"),
            p_common::DateTime::create("2000-01-01T00:00:00Z"),
            p_datum::TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN),
        p_cs::DateTimeTemporalCS::create(
            p_util::PropertyMap(),
            p_cs::CoordinateSystemAxis::create(MakeNameProps("t"), "t",
                                               p_cs::AxisDirection::FUTURE,
                                               p_common::UnitOfMeasure::NONE)));
    auto comp3 = p_crs::CompoundCRS::create(MakeNameProps("Comp3"),
                                            {g4807, v5703, time_crs});
    EXPECT_EQ(comp3->createBoundCRSToWGS84IfPossible(db, Interm::ALWAYS).get(),
              comp3.get());
  }
}

// ---------------------------------------------------------------------------
// Group 3: GeodeticCRS and GeographicCRS
// ---------------------------------------------------------------------------

TEST(CrsTest, GeodeticAndGeographicCrs) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");

  // Validation throws for datum vs datumEnsemble.
  {
    auto ens = MakeGeodeticEnsemble();
    auto ellps_cs = p_cs::EllipsoidalCS::createLatitudeLongitude(
        p_common::UnitOfMeasure::DEGREE);
    EXPECT_THROW(p_crs::GeographicCRS::create(MakeNameProps("Neither"), nullptr,
                                              nullptr, ellps_cs),
                 p_util::Exception);
    EXPECT_THROW(p_crs::GeographicCRS::create(
                     MakeNameProps("Both"),
                     p_datum::GeodeticReferenceFrame::EPSG_6326, ens, ellps_cs),
                 p_util::Exception);

    std::vector<p_datum::DatumNNPtr> vdatums;
    vdatums.push_back(
        p_datum::VerticalReferenceFrame::create(MakeNameProps("VD1")));
    vdatums.push_back(
        p_datum::VerticalReferenceFrame::create(MakeNameProps("VD2")));
    auto vert_ens = p_datum::DatumEnsemble::create(
        MakeNameProps("VertEns"), vdatums,
        p_meta::PositionalAccuracy::create("1.0"));
    EXPECT_THROW(p_crs::GeographicCRS::create(MakeNameProps("WrongEns"),
                                              nullptr, vert_ens, ellps_cs),
                 p_util::Exception);
  }

  // Ensemble accessors (datumNonNull, primeMeridian, ellipsoid), dynamic datum
  // velocityModel, and baseIsEquivalentTo (EXTENSION_PROJ4 XOR & normalized).
  {
    auto ens = MakeGeodeticEnsemble();
    auto g_ens = p_crs::GeographicCRS::create(
        MakeNameProps("EnsGeog"), nullptr, ens,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(g_ens->datum(), IsNull());
    EXPECT_THAT(g_ens->datumEnsemble(), NotNull());
    EXPECT_THAT(g_ens->datumNonNull(db), NotNull());
    EXPECT_EQ(g_ens->primeMeridian().get(),
              p_datum::PrimeMeridian::GREENWICH.get());
    EXPECT_THAT(g_ens->ellipsoid()->semiMajorAxis().getSIValue(),
                DoubleNear(6378137.0, 1e-6));
    EXPECT_THAT(g_ens->velocityModel(), IsEmpty());

    // Ensemble equivalent to DB WGS84 with EQUIVALENT & db.
    EXPECT_TRUE(
        g_ens->isEquivalentTo(p_crs::GeographicCRS::EPSG_4326.get(),
                              p_util::IComparable::Criterion::EQUIVALENT, db));

    // EXTENSION_PROJ4 equivalence branches.
    p_util::PropertyMap p_ext1;
    p_ext1.set(p_common::IdentifiedObject::NAME_KEY, "G");
    p_ext1.set("EXTENSION_PROJ4",
               "+proj=longlat +datum=WGS84 +no_defs +type=crs");
    p_util::PropertyMap p_ext2;
    p_ext2.set(p_common::IdentifiedObject::NAME_KEY, "G");
    p_ext2.set("EXTENSION_PROJ4", "+proj=longlat +datum=WGS84");
    p_util::PropertyMap p_ext3;
    p_ext3.set(p_common::IdentifiedObject::NAME_KEY, "G");
    p_ext3.set("EXTENSION_PROJ4", "+proj=longlat +ellps=GRS80");
    p_util::PropertyMap p_ext_type_only;
    p_ext_type_only.set(p_common::IdentifiedObject::NAME_KEY, "G");
    p_ext_type_only.set("EXTENSION_PROJ4", "+no_defs");
    auto cs_ll = p_cs::EllipsoidalCS::createLatitudeLongitude(
        p_common::UnitOfMeasure::DEGREE);
    auto g_ext1 = p_crs::GeographicCRS::create(
        p_ext1, p_datum::GeodeticReferenceFrame::EPSG_6326, cs_ll);
    auto g_ext2 = p_crs::GeographicCRS::create(
        p_ext2, p_datum::GeodeticReferenceFrame::EPSG_6326, cs_ll);
    auto g_ext3 = p_crs::GeographicCRS::create(
        p_ext3, p_datum::GeodeticReferenceFrame::EPSG_6326, cs_ll);
    auto g_ext_type_only = p_crs::GeographicCRS::create(
        p_ext_type_only, p_datum::GeodeticReferenceFrame::EPSG_6326, cs_ll);
    auto g_noext = p_crs::GeographicCRS::create(
        MakeNameProps("G"), p_datum::GeodeticReferenceFrame::EPSG_6326, cs_ll);
    EXPECT_FALSE(g_ext1->isEquivalentTo(
        g_noext.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
    EXPECT_TRUE(g_ext_type_only->isEquivalentTo(
        g_noext.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
    EXPECT_TRUE(g_ext1->isEquivalentTo(
        g_ext2.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
    EXPECT_TRUE(g_ext1->isEquivalentTo(
        g_ext3.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
  }

  // SphericalCS GeodeticCRS: 3D, 2D planetocentric (PROJ omit/emit), and
  // non-planetocentric throws.
  {
    auto sph_lat = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("Spherical latitude"), "phi", p_cs::AxisDirection::NORTH,
        p_common::UnitOfMeasure::DEGREE);
    auto sph_lon = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("Spherical longitude"), "theta",
        p_cs::AxisDirection::EAST, p_common::UnitOfMeasure::DEGREE);
    auto sph_r = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("Geocentric radius"), "r", p_cs::AxisDirection::UP,
        p_common::UnitOfMeasure::METRE);
    auto sph_3d_cs = p_cs::SphericalCS::create(p_util::PropertyMap(), sph_lat,
                                               sph_lon, sph_r);
    auto geod_sph_3d = p_crs::GeodeticCRS::create(
        MakeNameProps("Spherical3D"),
        p_datum::GeodeticReferenceFrame::EPSG_6326, sph_3d_cs);
    EXPECT_THAT(
        ToWkt(geod_sph_3d, p_io::WKTFormatter::Convention::WKT2_2019, db),
        HasSubstr("CS[spherical,3]"));
    EXPECT_THAT(
        ToWkt(geod_sph_3d, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        HasSubstr("GEOGCS[\"Spherical3D\""));
    EXPECT_THROW(
        ToWkt(geod_sph_3d, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
        p_io::FormattingException);
    EXPECT_THAT(ToJson(geod_sph_3d, db), HasSubstr("\"GeodeticCRS\""));
    EXPECT_THROW(ToProj(*geod_sph_3d), p_io::FormattingException);

    // 2D planetocentric SphericalCS on ellipsoid & on sphere with Paris PM.
    auto pc_lat = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("planetocentric latitude"), "U",
        p_cs::AxisDirection::NORTH, p_common::UnitOfMeasure::DEGREE);
    auto pc_lon = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("planetocentric longitude"), "V",
        p_cs::AxisDirection::EAST, p_common::UnitOfMeasure::DEGREE);
    auto pc_cs =
        p_cs::SphericalCS::create(p_util::PropertyMap(), pc_lat, pc_lon);
    auto geod_pc_ellps = p_crs::GeodeticCRS::create(
        MakeNameProps("PlanetocentricEllps"),
        p_datum::GeodeticReferenceFrame::EPSG_6326, pc_cs);
    EXPECT_THAT(ToProj(*geod_pc_ellps,
                       p_io::PROJStringFormatter::Convention::PROJ_5, db,
                       /*crs_export=*/false, false, /*omit_proj_longlat=*/true),
                HasSubstr("+proj=geoc"));
    EXPECT_THROW(
        ToProj(*geod_pc_ellps, p_io::PROJStringFormatter::Convention::PROJ_5,
               db, /*crs_export=*/true),
        p_io::FormattingException);

    auto sph_datum = p_datum::GeodeticReferenceFrame::create(
        MakeNameProps("SphereDatum"),
        p_datum::Ellipsoid::createSphere(MakeNameProps("Sphere"),
                                         p_common::Length(6371000.0)),
        p_util::optional<std::string>(), p_datum::PrimeMeridian::GREENWICH);
    auto geod_pc_sph = p_crs::GeodeticCRS::create(
        MakeNameProps("PlanetocentricSphere"), sph_datum, pc_cs);
    EXPECT_THAT(
        ToProj(*geod_pc_sph, p_io::PROJStringFormatter::Convention::PROJ_5, db,
               /*crs_export=*/false, false, /*omit_proj_longlat=*/true),
        AllOf(HasSubstr("+proj=unitconvert"), Not(HasSubstr("+proj=geoc"))));
  }

  // Geocentric Cartesian GeodeticCRS: WKT2, WKT1, WKT1_ESRI(throws), PROJ
  // (cart vs geocent, metre vs foot), JSON.
  {
    auto g4978 = p_crs::GeodeticCRS::EPSG_4978;
    EXPECT_TRUE(g4978->isGeocentric());
    EXPECT_THAT(ToWkt(g4978, p_io::WKTFormatter::Convention::WKT2_2019, db),
                HasSubstr("GEODCRS[\"WGS 84\""));
    EXPECT_THAT(
        ToWkt(g4978, p_io::WKTFormatter::Convention::WKT1_GDAL, db, true,
              p_io::WKTFormatter::OutputAxisRule::WKT1_GDAL_EPSG_STYLE),
        AllOf(HasSubstr("GEOCCS["), HasSubstr("AXIS[")));
    EXPECT_THROW(ToWkt(g4978, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
                 p_io::FormattingException);
    EXPECT_THAT(
        ToProj(*g4978, p_io::PROJStringFormatter::Convention::PROJ_5, db,
               /*crs_export=*/false),
        HasSubstr("+proj=cart"));
    EXPECT_THAT(
        ToProj(*g4978, p_io::PROJStringFormatter::Convention::PROJ_5, db,
               /*crs_export=*/true),
        AllOf(HasSubstr("+proj=geocent"), HasSubstr("+units=m")));
    EXPECT_THAT(ToJson(g4978, db), HasSubstr("\"GeodeticCRS\""));

    auto g4978_ft_crs = g4978->alterCSLinearUnit(p_common::UnitOfMeasure::FOOT);
    auto g4978_ft = dynamic_cast<const p_crs::GeodeticCRS*>(g4978_ft_crs.get());
    ASSERT_THAT(g4978_ft, NotNull());
    EXPECT_THAT(
        ToProj(*g4978_ft, p_io::PROJStringFormatter::Convention::PROJ_5, db,
               /*crs_export=*/false),
        AllOf(HasSubstr("+proj=unitconvert"), HasSubstr("+xy_out=ft")));
    EXPECT_THROW(
        ToProj(*g4978_ft, p_io::PROJStringFormatter::Convention::PROJ_5, db,
               /*crs_export=*/true),
        p_io::FormattingException);
  }

  // GeographicCRS: is2DPartOf3D, WKT1_GDAL (3D throws, allowNonConformant,
  // allowEllipsoidalHeightAsVerticalCRS, deprecated + EXTENSION_PROJ4),
  // WKT1_ESRI (2D, 3D, allowLINUNIT, non-DB 3D throws), PROJ, JSON, eq.
  {
    auto g4326 = p_crs::GeographicCRS::EPSG_4326;
    auto g4979 = auth_epsg->createGeographicCRS("4979");
    EXPECT_TRUE(g4326->is2DPartOf3D(NN_CHECK_THROW(g4979.get()), db));
    EXPECT_FALSE(p_crs::GeographicCRS::EPSG_4269->is2DPartOf3D(
        NN_CHECK_THROW(g4979.get()), db));

    EXPECT_THROW(ToWkt(g4979, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                 p_io::FormattingException);
    EXPECT_THAT(ToWkt(g4979, p_io::WKTFormatter::Convention::WKT1_GDAL, db,
                      true, std::nullopt,
                      /*allow_ellps_height_vert=*/true),
                AllOf(HasSubstr("COMPD_CS["),
                      HasSubstr("VERT_DATUM[\"Ellipsoid\",2002]")));
    EXPECT_THAT(ToWkt(g4979->allowNonConformantWKT1Export(),
                      p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                HasSubstr("COMPD_CS[\"WGS 84 + WGS 84\""));

    // WKT1_ESRI on 2D, 3D (EPSG:4979, LINUNIT, non-DB throws), EPSG_OLD
    // fallback (EPSG:4258 vs deprecated/Norwegian ETRS89-NOR EPSG:9333 or
    // 4230).
    EXPECT_THAT(ToWkt(g4326, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
                HasSubstr("GEOGCS[\"GCS_WGS_1984\""));
    EXPECT_THAT(
        ToWkt(g4979, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
        AllOf(HasSubstr("GEOGCS[\"GCS_WGS_1984\""), HasSubstr("VERTCS[")));
    EXPECT_THAT(
        ToWkt(g4979, p_io::WKTFormatter::Convention::WKT1_ESRI, db, true,
              std::nullopt, false,
              /*allow_linunit=*/true),
        AllOf(HasSubstr("GEOGCS[\"WGS_1984_3D\""), HasSubstr("LINUNIT[")));

    auto custom_datum = p_datum::GeodeticReferenceFrame::create(
        MakeNameProps("Completely Custom Datum"), p_datum::Ellipsoid::WGS84,
        p_util::optional<std::string>(), p_datum::PrimeMeridian::GREENWICH);
    auto custom_geog_2d = p_crs::GeographicCRS::create(
        MakeNameProps("My Custom CRS"), custom_datum,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    auto custom_geog_3d = p_crs::GeographicCRS::create(
        MakeNameProps("My Custom CRS 3D"), custom_datum,
        p_cs::EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
            p_common::UnitOfMeasure::DEGREE, p_common::UnitOfMeasure::METRE));
    EXPECT_THAT(
        ToWkt(custom_geog_2d, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
        HasSubstr("GEOGCS[\"GCS_My_Custom_CRS\""));
    EXPECT_THROW(
        ToWkt(custom_geog_3d, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
        p_io::FormattingException);

    // Deprecated GeographicCRS + EXTENSION_PROJ4 in WKT1_GDAL.
    p_util::PropertyMap dep_ext_props;
    dep_ext_props.set(p_common::IdentifiedObject::NAME_KEY, "DeprGeog");
    dep_ext_props.set(p_common::IdentifiedObject::DEPRECATED_KEY, true);
    dep_ext_props.set("EXTENSION_PROJ4", "+proj=longlat +datum=WGS84");
    auto dep_ext_geog = p_crs::GeographicCRS::create(
        dep_ext_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(
        ToWkt(dep_ext_geog, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        AllOf(HasSubstr("DeprGeog (deprecated)"),
              HasSubstr("EXTENSION[\"PROJ4\"")));

    // PROJ export: legacyCRSToCRSContext, omitZUnitConversion, WEST/SOUTH
    // axis swap, and custom angular unit.
    EXPECT_THAT(ToProj(*p_crs::GeographicCRS::EPSG_4269,
                       p_io::PROJStringFormatter::Convention::PROJ_5, db,
                       /*crs_export=*/true, /*legacy_crs_to_crs=*/true),
                HasSubstr("+ellps=GRS80"));
    EXPECT_THAT(ToProj(*p_crs::GeographicCRS::EPSG_4269,
                       p_io::PROJStringFormatter::Convention::PROJ_5, db,
                       /*crs_export=*/true, /*legacy_crs_to_crs=*/false),
                HasSubstr("+datum=NAD83"));
    EXPECT_THAT(
        ToProj(*g4979, p_io::PROJStringFormatter::Convention::PROJ_5, db,
               /*crs_export=*/false, false, false,
               /*omit_z_unit_convert=*/true),
        AllOf(HasSubstr("+proj=unitconvert"), Not(HasSubstr("+z_in=m"))));

    auto west_south_cs = p_cs::EllipsoidalCS::create(
        p_util::PropertyMap(),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("Westing"), "W",
                                           p_cs::AxisDirection::WEST,
                                           p_common::UnitOfMeasure::GRAD),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("Southing"), "S",
                                           p_cs::AxisDirection::SOUTH,
                                           p_common::UnitOfMeasure::GRAD));
    auto geog_ws = p_crs::GeographicCRS::create(
        MakeNameProps("WestSouth"), p_datum::GeodeticReferenceFrame::EPSG_6326,
        west_south_cs);
    EXPECT_THAT(ToProj(*geog_ws, p_io::PROJStringFormatter::Convention::PROJ_5,
                       db, /*crs_export=*/false),
                AllOf(HasSubstr("+xy_out=grad"),
                      HasSubstr("+proj=axisswap +order=-1,-2")));

    // JSON export: dynamic datum deformation_models, ensemble, unnamed.
    auto unnamed_dyn = p_crs::GeographicCRS::create(
        MakeNameProps(""), MakeDynamicGeodeticDatum(),
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(ToJson(unnamed_dyn, db),
                AllOf(HasSubstr("\"name\":\"unnamed\""),
                      HasSubstr("\"deformation_models\""),
                      HasSubstr("test_deform_model")));

    // Equivalence: Undefined geographic SRS & axis order relaxation.
    auto crs84 = p_crs::GeographicCRS::OGC_CRS84;
    EXPECT_FALSE(g4326->isEquivalentTo(
        crs84.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
    EXPECT_TRUE(g4326->isEquivalentTo(
        crs84.get(),
        p_util::IComparable::Criterion::EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS,
        db));
    auto undef1 = p_crs::GeographicCRS::create(
        MakeNameProps("Undefined geographic SRS"),
        p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLongitudeLatitude(
            p_common::UnitOfMeasure::DEGREE));
    auto undef2 = p_crs::GeographicCRS::create(
        MakeNameProps("Undefined geographic SRS"),
        p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_FALSE(undef2->isEquivalentTo(
        g4326.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
    EXPECT_TRUE(undef1->isEquivalentTo(
        undef2.get(),
        p_util::IComparable::Criterion::EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS,
        db));

    // identify(): null factory, exact ID, wrong ID, insignificant name,
    // ellipsoid search, spherical radius search, and 90% filter.
    EXPECT_THAT(custom_geog_2d->identify(nullptr), IsEmpty());
    EXPECT_FALSE(g4326->identify(nullptr).empty());

    auto wrong_id_4326 = p_crs::GeographicCRS::create(
        MakeNameCodeProps("Not WGS84", "EPSG", "4269"),
        p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    auto wrong_id_res = wrong_id_4326->identify(auth_epsg);
    ASSERT_FALSE(wrong_id_res.empty());
    EXPECT_EQ(wrong_id_res.front().second, 25);

    auto unknown_wgs84 = p_crs::GeographicCRS::create(
        MakeNameProps("unknown"), p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_FALSE(unknown_wgs84->identify(auth_epsg).empty());

    auto custom_named_wgs84_datum = p_crs::GeographicCRS::create(
        MakeNameProps("My Weird Alias"),
        p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_FALSE(custom_named_wgs84_datum->identify(auth_epsg).empty());

    // Search by ellipsoid when datum name is unrecognized.
    auto unknown_datum_wgs84_ellps = p_datum::GeodeticReferenceFrame::create(
        MakeNameProps("Completely Unknown Datum"), p_datum::Ellipsoid::WGS84,
        p_util::optional<std::string>(), p_datum::PrimeMeridian::GREENWICH);
    auto ellps_search_crs = p_crs::GeographicCRS::create(
        MakeNameProps("NoSuchGeog"), unknown_datum_wgs84_ellps,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_FALSE(ellps_search_crs->identify(auth_epsg).empty());

    // Approximate name match on WGS_84.
    auto equiv_name_4326 = p_crs::GeographicCRS::create(
        MakeNameProps("WGS_84"), p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    auto eq_res = equiv_name_4326->identify(auth_epsg);
    ASSERT_FALSE(eq_res.empty());
    EXPECT_GE(eq_res.front().second, 90);
  }
}

// ---------------------------------------------------------------------------
// Group 4: VerticalCRS and DerivedVerticalCRS
// ---------------------------------------------------------------------------

TEST(CrsTest, VerticalAndDerivedVerticalCrs) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");

  // Validation throws for datum vs ensemble.
  {
    auto vcs = p_cs::VerticalCS::createGravityRelatedHeight(
        p_common::UnitOfMeasure::METRE);
    EXPECT_THROW(p_crs::VerticalCRS::create(MakeNameProps("Neither"), nullptr,
                                            nullptr, vcs),
                 p_util::Exception);
    auto ens = MakeGeodeticEnsemble();
    EXPECT_THROW(p_crs::VerticalCRS::create(MakeNameProps("WrongEns"), nullptr,
                                            ens, vcs),
                 p_util::Exception);
  }

  // GEOID_MODEL (single & ArrayOfBaseObject), WKT2_2019, WKT1_GDAL, WKT1_ESRI
  // (EPSG:5703, VERT_DATUM_TYPE="2002", DOWN direction), PROJ, JSON, identify.
  {
    auto vcs = p_cs::VerticalCS::createGravityRelatedHeight(
        p_common::UnitOfMeasure::METRE);
    auto vdatum = p_datum::VerticalReferenceFrame::create(
        MakeNameProps("Custom VDatum"), p_util::optional<std::string>(),
        p_util::optional<p_datum::RealizationMethod>(
            p_datum::RealizationMethod::GEOID));
    auto src_vert =
        p_crs::VerticalCRS::create(MakeNameProps("SrcVert"), vdatum, vcs);
    auto interp_crs = p_crs::GeographicCRS::EPSG_4326;
    auto gmodel_1 =
        p_op::Transformation::createGravityRelatedHeightToGeographic3D(
            MakeNameProps("Geoid1"), src_vert, p_crs::GeographicCRS::EPSG_4979,
            interp_crs.as_nullable(), "geoid1.gtx", {});
    auto gmodel_2 =
        p_op::Transformation::createGravityRelatedHeightToGeographic3D(
            MakeNameProps("Geoid2"), src_vert, p_crs::GeographicCRS::EPSG_4979,
            interp_crs.as_nullable(), "geoid2.gtx", {});

    p_util::PropertyMap props_single;
    props_single.set(p_common::IdentifiedObject::NAME_KEY, "Vert Single");
    props_single.set("GEOID_MODEL", gmodel_1);
    auto v_single = p_crs::VerticalCRS::create(props_single, vdatum, vcs);
    EXPECT_THAT(ToWkt(v_single, p_io::WKTFormatter::Convention::WKT2_2019, db),
                HasSubstr("GEOIDMODEL[\"Geoid1\"]"));
    EXPECT_THAT(ToJson(v_single, db),
                AllOf(HasSubstr("\"geoid_model\""),
                      HasSubstr("\"interpolation_crs\"")));

    p_util::PropertyMap props_multi;
    props_multi.set(p_common::IdentifiedObject::NAME_KEY, "Vert Multi");
    auto arr = p_util::ArrayOfBaseObject::create();
    arr->add(gmodel_1);
    arr->add(gmodel_2);
    props_multi.set("GEOID_MODEL", arr);
    auto v_multi = p_crs::VerticalCRS::create(props_multi, vdatum, vcs);
    EXPECT_THAT(ToWkt(v_multi, p_io::WKTFormatter::Convention::WKT2_2019, db),
                AllOf(HasSubstr("GEOIDMODEL[\"Geoid1\"]"),
                      HasSubstr("GEOIDMODEL[\"Geoid2\"]")));
    EXPECT_THAT(ToJson(v_multi, db), HasSubstr("\"geoid_models\""));

    // Dynamic vertical datum in JSON.
    auto v_dyn = p_crs::VerticalCRS::create(MakeNameProps(""),
                                            MakeDynamicVerticalDatum(), vcs);
    EXPECT_THAT(ToJson(v_dyn, db), AllOf(HasSubstr("\"name\":\"unnamed\""),
                                         HasSubstr("\"deformation_models\""),
                                         HasSubstr("vert_deform_model")));

    // WKT1_GDAL & WKT1_ESRI (EPSG:5703 + custom DOWN + custom 2002).
    auto v5703 = auth_epsg->createVerticalCRS("5703");
    EXPECT_THAT(
        ToWkt(v5703, p_io::WKTFormatter::Convention::WKT1_GDAL, db, true,
              p_io::WKTFormatter::OutputAxisRule::WKT1_GDAL_EPSG_STYLE),
        AllOf(HasSubstr("VERT_CS["), HasSubstr("AXIS[")));
    EXPECT_THAT(ToWkt(v5703, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
                HasSubstr("VERTCS["));

    p_util::PropertyMap ellps_vd_props;
    ellps_vd_props.set(p_common::IdentifiedObject::NAME_KEY, "EllpsVD");
    ellps_vd_props.set("VERT_DATUM_TYPE", "2002");
    auto ellps_vd = p_datum::VerticalReferenceFrame::create(ellps_vd_props);
    auto depth_axis = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("Depth"), "D", p_cs::AxisDirection::DOWN,
        p_common::UnitOfMeasure::FOOT);
    auto v_down = p_crs::VerticalCRS::create(
        MakeNameProps("Custom Depth"), ellps_vd,
        p_cs::VerticalCS::create(p_util::PropertyMap(), depth_axis));
    EXPECT_THAT(ToWkt(v_down, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
                AllOf(HasSubstr("VERTCS[\"Custom_Depth\""),
                      HasSubstr("PARAMETER[\"Direction\",-1.0]")));
    EXPECT_THAT(ToProj(*v_down), HasSubstr("+vunits=ft"));

    auto custom_lin_unit = p_common::UnitOfMeasure(
        "MyUnit", 0.5, p_common::UnitOfMeasure::Type::LINEAR);
    auto v_custom_unit = p_crs::VerticalCRS::create(
        MakeNameProps("VertHalfM"), vdatum,
        p_cs::VerticalCS::createGravityRelatedHeight(custom_lin_unit));
    EXPECT_THAT(ToProj(*v_custom_unit), HasSubstr("+vto_meter=0.5"));

    // VerticalCRS identify().
    EXPECT_THAT(v_single->identify(nullptr), IsEmpty());
    EXPECT_THAT(v5703->identify(auth_epsg), SizeIs(1));
    auto wrong_id_v = p_crs::VerticalCRS::create(
        MakeNameCodeProps("NAVD88 height", "EPSG", "5701"),
        NN_CHECK_THROW(v5703->datum()), v5703->coordinateSystem());
    auto wrong_v_res = wrong_id_v->identify(auth_epsg);
    ASSERT_FALSE(wrong_v_res.empty());
    EXPECT_EQ(wrong_v_res.front().second, 25);
    auto unnamed_navd = p_crs::VerticalCRS::create(
        MakeNameProps("NAVD88"), NN_CHECK_THROW(v5703->datum()),
        v5703->coordinateSystem());
    EXPECT_FALSE(unnamed_navd->identify(auth_epsg).empty());
  }

  // DerivedVerticalCRS (ONLY 1-level for WKT1 export!).
  {
    auto v5703 = auth_epsg->createVerticalCRS("5703");
    auto vcs_ft = p_cs::VerticalCS::createGravityRelatedHeight(
        p_common::UnitOfMeasure::FOOT);

    auto dv_unit_scale = p_crs::DerivedVerticalCRS::create(
        MakeNameProps("NAVD88 ft scaled"), v5703,
        p_op::Conversion::createChangeVerticalUnit(MakeNameProps("UnitScale"),
                                                   p_common::Scale(0.3048)),
        vcs_ft);
    EXPECT_THAT(
        ToWkt(dv_unit_scale, p_io::WKTFormatter::Convention::WKT2_2019, db),
        AllOf(HasSubstr("VERTCRS[\"NAVD88 ft scaled\""),
              HasSubstr("BASEVERTCRS[\"NAVD88 height\"")));
    EXPECT_THAT(
        ToWkt(dv_unit_scale, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        HasSubstr("VERT_CS[\"NAVD88 ft scaled\""));
    EXPECT_THAT(ToJson(dv_unit_scale, db), HasSubstr("\"DerivedVerticalCRS\""));
    EXPECT_THROW(ToProj(*dv_unit_scale), p_io::FormattingException);

    auto dv_unit_no_factor = p_crs::DerivedVerticalCRS::create(
        MakeNameProps("NAVD88 ft"), v5703,
        p_op::Conversion::createChangeVerticalUnit(MakeNameProps("Unit")),
        vcs_ft);
    EXPECT_THAT(
        ToWkt(dv_unit_no_factor, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        HasSubstr("VERT_CS[\"NAVD88 ft\""));

    auto depth_cs = p_cs::VerticalCS::create(
        p_util::PropertyMap(),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("Depth"), "D",
                                           p_cs::AxisDirection::DOWN,
                                           p_common::UnitOfMeasure::METRE));
    auto dv_reversal = p_crs::DerivedVerticalCRS::create(
        MakeNameProps("NAVD88 depth"), v5703,
        p_op::Conversion::createHeightDepthReversal(MakeNameProps("Rev")),
        depth_cs);
    EXPECT_THAT(
        ToWkt(dv_reversal, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        HasSubstr("AXIS[\"Depth\",DOWN]"));

    auto dv_affine = p_crs::DerivedVerticalCRS::create(
        MakeNameProps("NAVD88 affine"), v5703,
        p_op::Conversion::createAffineParametric(
            MakeNameProps("AffV"), p_common::Length(1.0), p_common::Scale(1.0),
            p_common::Scale(0.0), p_common::Length(0.0), p_common::Scale(0.0),
            p_common::Scale(1.0)),
        vcs_ft);
    EXPECT_THROW(
        ToWkt(dv_affine, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        p_io::FormattingException);

    auto dv_clone = dv_unit_scale->shallowClone();
    EXPECT_TRUE(dv_clone->isEquivalentTo(dv_unit_scale.get()));
    EXPECT_FALSE(dv_unit_scale->isEquivalentTo(v5703.get()));
    EXPECT_THAT(dv_unit_scale->identify(auth_epsg), IsEmpty());
  }
}

// ---------------------------------------------------------------------------
// Group 5: ProjectedCRS
// ---------------------------------------------------------------------------

TEST(CrsTest, ProjectedCrs) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");

  // Basic accessors, demoteTo2D, and alterParametersLinearUnit.
  {
    auto proj = auth_epsg->createProjectedCRS("32631");
    EXPECT_THAT(proj->baseCRS()->nameStr(), Eq("WGS 84"));
    EXPECT_THAT(proj->coordinateSystem()->axisList(), SizeIs(2));
    EXPECT_THAT(proj->derivingConversion()->nameStr(),
                HasSubstr("UTM zone 31N"));

    EXPECT_EQ(proj->demoteTo2D("", db).get(), proj.get());
    auto proj_3d_base = proj->promoteTo3D("UTM31N 3D", nullptr);
    auto proj_3d = dynamic_cast<const p_crs::ProjectedCRS*>(proj_3d_base.get());
    ASSERT_THAT(proj_3d, NotNull());
    EXPECT_THAT(proj_3d->coordinateSystem()->axisList(), SizeIs(3));
    auto demoted_no_db = proj_3d->demoteTo2D("UTM31N Demoted", nullptr);
    EXPECT_THAT(demoted_no_db->nameStr(), Eq("UTM31N Demoted"));
    EXPECT_THAT(demoted_no_db->coordinateSystem()->axisList(), SizeIs(2));

    auto altered_conv_true =
        proj->alterParametersLinearUnit(p_common::UnitOfMeasure::FOOT, true);
    auto altered_conv_false =
        proj->alterParametersLinearUnit(p_common::UnitOfMeasure::FOOT, false);
    EXPECT_NE(altered_conv_true.get(), proj.get());
    EXPECT_NE(altered_conv_false.get(), proj.get());
  }

  // WKT export branches: 3D (WKT2_2019, WKT2_2015/WKT1_GDAL throws,
  // allowNonConformant, allowEllipsoidalHeight, WKT1_ESRI), and 2D
  // (WKT2_2019_SIMPLIFIED idOnTopLevelOnly, WKT1_ESRI DB/morph, Popular
  // Visualisation Mercator, deprecated+EXTENSION_PROJ4, geocentric base).
  {
    auto proj = auth_epsg->createProjectedCRS("32631");
    auto proj_3d = proj->promoteTo3D("WGS 84 / UTM zone 31N", db);
    EXPECT_THAT(ToWkt(proj_3d, p_io::WKTFormatter::Convention::WKT2_2019, db),
                AllOf(HasSubstr("PROJCRS["), HasSubstr("CS[Cartesian,3]")));
    EXPECT_THROW(ToWkt(proj_3d, p_io::WKTFormatter::Convention::WKT2_2015, db),
                 p_io::FormattingException);
    EXPECT_THROW(ToWkt(proj_3d, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                 p_io::FormattingException);
    EXPECT_THAT(ToWkt(proj_3d->allowNonConformantWKT1Export(),
                      p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                HasSubstr("COMPD_CS[\"WGS 84 / UTM zone 31N + WGS 84\""));
    EXPECT_THAT(ToWkt(proj_3d, p_io::WKTFormatter::Convention::WKT1_GDAL, db,
                      true, std::nullopt,
                      /*allow_ellps_height_vert=*/true),
                AllOf(HasSubstr("COMPD_CS["),
                      HasSubstr("VERT_DATUM[\"Ellipsoid\",2002]")));
    EXPECT_THAT(ToWkt(proj_3d, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
                AllOf(HasSubstr("PROJCS["), HasSubstr("VERTCS[")));

    EXPECT_THAT(
        ToWkt(proj, p_io::WKTFormatter::Convention::WKT2_2019_SIMPLIFIED, db),
        AllOf(HasSubstr("PROJCRS[\"WGS 84 / UTM zone 31N\""),
              Not(HasSubstr("BASEGEOGCRS[\"WGS 84\",DATUM[\"World Geodetic "
                            "System 1984\",ELLIPSOID[\"WGS "
                            "84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]"
                            "],ID[\"EPSG\",6326]]"))));
    EXPECT_THAT(ToWkt(proj, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
                HasSubstr("PROJCS[\"WGS_1984_UTM_Zone_31N\""));
    EXPECT_THAT(
        ToWkt(proj, p_io::WKTFormatter::Convention::WKT1_GDAL, db, true,
              p_io::WKTFormatter::OutputAxisRule::WKT1_GDAL_EPSG_STYLE),
        AllOf(HasSubstr("PROJCS["), HasSubstr("AXIS[\"Easting\",EAST]")));

    auto pop_vis = auth_epsg->createProjectedCRS("3785");
    EXPECT_THAT(ToWkt(pop_vis, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                AllOf(HasSubstr("PROJECTION[\"Mercator_1SP\"]"),
                      HasSubstr("TOWGS84[0,0,0,0,0,0,0]")));

    p_util::PropertyMap dep_proj_props;
    dep_proj_props.set(p_common::IdentifiedObject::NAME_KEY, "Custom Merc");
    dep_proj_props.set(p_common::IdentifiedObject::DEPRECATED_KEY, true);
    dep_proj_props.set("EXTENSION_PROJ4", "+proj=merc +datum=WGS84 +type=crs");
    auto custom_proj = p_crs::ProjectedCRS::create(
        dep_proj_props, p_crs::GeographicCRS::EPSG_4326,
        MakeMercatorConversion(),
        p_cs::CartesianCS::createEastingNorthing(
            p_common::UnitOfMeasure::METRE));
    EXPECT_THAT(
        ToWkt(custom_proj, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        AllOf(HasSubstr("Custom Merc (deprecated)"),
              HasSubstr("EXTENSION[\"PROJ4\"")));
    EXPECT_THAT(
        ToWkt(custom_proj, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
        HasSubstr("PROJCS[\"Custom_Merc\""));

    // ProjectedCRS with geocentric base CRS emits BASEGEODCRS in WKT2.
    auto proj_on_geoc = p_crs::ProjectedCRS::create(
        MakeNameProps("OnGeocentric"), p_crs::GeodeticCRS::EPSG_4978,
        MakeMercatorConversion(),
        p_cs::CartesianCS::createEastingNorthing(
            p_common::UnitOfMeasure::METRE));
    EXPECT_THAT(
        ToWkt(proj_on_geoc, p_io::WKTFormatter::Convention::WKT2_2019, db),
        HasSubstr("BASEGEODCRS[\"WGS 84\""));
  }

  // PROJ string export branches and JSON export.
  {
    auto proj = auth_epsg->createProjectedCRS("32631");
    EXPECT_THAT(ToJson(proj, db), HasSubstr("\"ProjectedCRS\""));
    EXPECT_THAT(ToProj(*proj, p_io::PROJStringFormatter::Convention::PROJ_5, db,
                       /*crs_export=*/true, /*legacy_crs_to_crs=*/false),
                HasSubstr("+units=m"));
    EXPECT_THAT(ToProj(*proj, p_io::PROJStringFormatter::Convention::PROJ_5, db,
                       /*crs_export=*/true, /*legacy_crs_to_crs=*/true),
                Not(HasSubstr("+units=m")));

    auto custom_u = p_common::UnitOfMeasure(
        "HalfMetre", 0.5, p_common::UnitOfMeasure::Type::LINEAR);
    auto proj_custom_u = proj->alterCSLinearUnit(custom_u);
    auto proj_custom_cast =
        dynamic_cast<const p_crs::ProjectedCRS*>(proj_custom_u.get());
    ASSERT_THAT(proj_custom_cast, NotNull());
    EXPECT_THAT(ToProj(*proj_custom_cast,
                       p_io::PROJStringFormatter::Convention::PROJ_5, db,
                       /*crs_export=*/true),
                HasSubstr("+to_meter=0.5"));
    EXPECT_THAT(ToProj(*proj_custom_cast,
                       p_io::PROJStringFormatter::Convention::PROJ_5, db,
                       /*crs_export=*/false),
                HasSubstr("+xy_out=0.5"));

    auto proj_3d_ft = proj->promoteTo3D("3D Ft", db)
                          ->alterCSLinearUnit(p_common::UnitOfMeasure::FOOT);
    auto proj_3d_ft_cast =
        dynamic_cast<const p_crs::ProjectedCRS*>(proj_3d_ft.get());
    ASSERT_THAT(proj_3d_ft_cast, NotNull());
    EXPECT_THAT(ToProj(*proj_3d_ft_cast,
                       p_io::PROJStringFormatter::Convention::PROJ_5, db,
                       /*crs_export=*/false),
                AllOf(HasSubstr("+xy_out=ft"), HasSubstr("+z_out=ft")));

    auto proj_ws = p_crs::ProjectedCRS::create(
        MakeNameProps("WestSouth"), p_crs::GeographicCRS::EPSG_4326,
        MakeMercatorConversion(),
        p_cs::CartesianCS::createWestingSouthing(
            p_common::UnitOfMeasure::METRE));
    EXPECT_THAT(ToProj(*proj_ws, p_io::PROJStringFormatter::Convention::PROJ_5,
                       db, /*crs_export=*/false),
                HasSubstr("+proj=axisswap +order=-1,-2"));

    // Polar UPS North (N,E) & South (N,E) -> order=2,1.
    auto ups_n_ne = auth_epsg->createProjectedCRS("32661");
    EXPECT_THAT(ToProj(*ups_n_ne, p_io::PROJStringFormatter::Convention::PROJ_5,
                       db, /*crs_export=*/false),
                HasSubstr("+proj=axisswap +order=2,1"));
    auto ups_s_ne = auth_epsg->createProjectedCRS("32761");
    EXPECT_THAT(ToProj(*ups_s_ne, p_io::PROJStringFormatter::Convention::PROJ_5,
                       db, /*crs_export=*/false),
                HasSubstr("+proj=axisswap +order=2,1"));

    p_util::PropertyMap ext_props;
    ext_props.set(p_common::IdentifiedObject::NAME_KEY, "ExtProj4");
    ext_props.set("EXTENSION_PROJ4", "+proj=merc +datum=WGS84 +type=crs");
    auto proj_ext = p_crs::ProjectedCRS::create(
        ext_props, p_crs::GeographicCRS::EPSG_4326, MakeMercatorConversion(),
        p_cs::CartesianCS::createEastingNorthing(
            p_common::UnitOfMeasure::METRE));
    EXPECT_THAT(ToProj(*proj_ext), HasSubstr("+proj=merc"));
  }

  // _isEquivalentTo with IMPLICIT_CS and identify() across WGS84/NAD27/NAD83
  // UTM shortcuts, LONG_EAST_LAT_NORTH base, ID mismatch, datum/CS mismatch.
  {
    auto cs_en = p_cs::CartesianCS::createEastingNorthing(
        p_common::UnitOfMeasure::METRE);
    p_util::PropertyMap implicit_geog_props;
    implicit_geog_props.set(p_common::IdentifiedObject::NAME_KEY, "WGS 84");
    implicit_geog_props.set("IMPLICIT_CS", true);
    auto base_crs84_implicit = p_crs::GeographicCRS::create(
        implicit_geog_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
        p_cs::EllipsoidalCS::createLongitudeLatitude(
            p_common::UnitOfMeasure::DEGREE));
    auto proj_implicit = p_crs::ProjectedCRS::create(
        MakeNameProps("WGS 84 / UTM zone 31N"), base_crs84_implicit,
        p_op::Conversion::createUTM(MakeNameProps("UTM31N"), 31, true), cs_en);
    auto proj_32631 = auth_epsg->createProjectedCRS("32631");
    EXPECT_TRUE(proj_implicit->isEquivalentTo(
        proj_32631.get(), p_util::IComparable::Criterion::EQUIVALENT, db));

    // Identify without DB for UTM WGS84, NAD27 (zone 11 & zone 59), NAD83
    // (zone 11 & zone 59).
    EXPECT_FALSE(proj_implicit->identify(nullptr).empty());

    auto utm_nad27_z11 = p_crs::ProjectedCRS::create(
        MakeNameProps("NAD27 / UTM zone 11N"), p_crs::GeographicCRS::EPSG_4267,
        p_op::Conversion::createUTM(MakeNameProps("U11"), 11, true), cs_en);
    auto res_n27 = utm_nad27_z11->identify(nullptr);
    ASSERT_THAT(res_n27, SizeIs(1));
    EXPECT_EQ(res_n27.front().first->identifiers()[0]->code(), "26711");
    EXPECT_EQ(res_n27.front().second, 100);

    auto utm_nad27_z59 = p_crs::ProjectedCRS::create(
        MakeNameProps("OtherName"), p_crs::GeographicCRS::EPSG_4267,
        p_op::Conversion::createUTM(MakeNameProps("U59"), 59, true), cs_en);
    auto res_n27_59 = utm_nad27_z59->identify(nullptr);
    ASSERT_THAT(res_n27_59, SizeIs(1));
    EXPECT_EQ(res_n27_59.front().first->identifiers()[0]->code(), "3370");
    EXPECT_EQ(res_n27_59.front().second, 70);

    auto utm_nad83_z11 = p_crs::ProjectedCRS::create(
        MakeNameProps("NAD83_UTM_zone_11N"), p_crs::GeographicCRS::EPSG_4269,
        p_op::Conversion::createUTM(MakeNameProps("U11"), 11, true), cs_en);
    auto res_n83 = utm_nad83_z11->identify(nullptr);
    ASSERT_THAT(res_n83, SizeIs(1));
    EXPECT_EQ(res_n83.front().first->identifiers()[0]->code(), "26911");
    EXPECT_EQ(res_n83.front().second, 90);

    auto utm_nad83_z59 = p_crs::ProjectedCRS::create(
        MakeNameProps("Other"), p_crs::GeographicCRS::EPSG_4269,
        p_op::Conversion::createUTM(MakeNameProps("U59"), 59, true), cs_en);
    auto res_n83_59 = utm_nad83_z59->identify(nullptr);
    ASSERT_THAT(res_n83_59, SizeIs(1));
    EXPECT_EQ(res_n83_59.front().first->identifiers()[0]->code(), "3372");

    // Wrong EPSG code + DB identify branches.
    auto wrong_id_proj = p_crs::ProjectedCRS::create(
        MakeNameCodeProps("WGS 84 / UTM zone 31N", "EPSG", "32632"),
        p_crs::GeographicCRS::EPSG_4326,
        p_op::Conversion::createUTM(MakeNameProps("U31"), 31, true), cs_en);
    auto res_wrong = wrong_id_proj->identify(auth_epsg);
    EXPECT_FALSE(res_wrong.empty());

    // Identify with DB where name matches ESRI / alternate alias and CS swaps
    // (Northing/Easting -> 50% vs datum mismatch -> 60%).
    auto merc_conv = MakeMercatorConversion("World Mercator");
    auto unknown_datum = p_datum::GeodeticReferenceFrame::create(
        MakeNameProps("Not WGS84 Datum"), p_datum::Ellipsoid::WGS84,
        p_util::optional<std::string>(), p_datum::PrimeMeridian::GREENWICH);
    auto custom_geog = p_crs::GeographicCRS::create(
        MakeNameProps("CustomGeog"), unknown_datum,
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    auto proj_datum_diff = p_crs::ProjectedCRS::create(
        MakeNameProps("WGS 84 / World Mercator"), custom_geog,
        p_op::Conversion::createMercatorVariantA(
            MakeNameProps("Merc"),
            p_common::Angle(0.0, p_common::UnitOfMeasure::DEGREE),
            p_common::Angle(0.0, p_common::UnitOfMeasure::DEGREE),
            p_common::Scale(1.0), p_common::Length(0.0), p_common::Length(0.0)),
        cs_en);
    auto res_datum_diff = proj_datum_diff->identify(auth_epsg);
    EXPECT_FALSE(res_datum_diff.empty());

    auto proj_ne_swap = p_crs::ProjectedCRS::create(
        MakeNameProps("WGS 84 / World Mercator"),
        p_crs::GeographicCRS::EPSG_4326,
        p_op::Conversion::createMercatorVariantA(
            MakeNameProps("Merc"),
            p_common::Angle(0.0, p_common::UnitOfMeasure::DEGREE),
            p_common::Angle(0.0, p_common::UnitOfMeasure::DEGREE),
            p_common::Scale(1.0), p_common::Length(0.0), p_common::Length(0.0)),
        p_cs::CartesianCS::createNorthingEasting(
            p_common::UnitOfMeasure::METRE));
    auto res_ne = proj_ne_swap->identify(auth_epsg);
    EXPECT_FALSE(res_ne.empty());
  }
}

// ---------------------------------------------------------------------------
// Group 6: CompoundCRS and BoundCRS
// ---------------------------------------------------------------------------

TEST(CrsTest, CompoundAndBoundCrs) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");

  // CompoundCRS::create validation, auto-naming, createLax, WKT/ESRI/JSON/PROJ,
  // equivalence, and identify.
  {
    auto proj = auth_epsg->createProjectedCRS("32631");
    auto vert = auth_epsg->createVerticalCRS("5703");
    EXPECT_THROW(p_crs::CompoundCRS::create(MakeNameProps("TooFew"), {proj}),
                 p_crs::InvalidCompoundCRSException);
    EXPECT_THROW(p_crs::CompoundCRS::create(
                     MakeNameProps("Invalid"),
                     {auth_epsg->createGeographicCRS("4979"), vert}),
                 p_crs::InvalidCompoundCRSException);

    // Unnamed CompoundCRS synthesizes "comp0 + comp1".
    auto comp_auto_name =
        p_crs::CompoundCRS::create(p_util::PropertyMap(), {proj, vert});
    EXPECT_THAT(comp_auto_name->nameStr(),
                Eq("WGS 84 / UTM zone 31N + NAVD88 height"));
    auto comp_clone = comp_auto_name->shallowClone();
    EXPECT_TRUE(comp_clone->isEquivalentTo(comp_auto_name.get()));

    // WKT2, WKT1_GDAL, WKT1_ESRI (2 vs 3 components), JSON, PROJ.
    EXPECT_THAT(
        ToWkt(comp_auto_name, p_io::WKTFormatter::Convention::WKT2_2019, db),
        HasSubstr("COMPOUNDCRS["));
    EXPECT_THAT(
        ToWkt(comp_auto_name, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        HasSubstr("COMPD_CS["));
    EXPECT_THAT(
        ToWkt(comp_auto_name, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
        AllOf(HasSubstr("PROJCS["), HasSubstr("VERTCS["),
              Not(HasSubstr("COMPD_CS["))));
    EXPECT_THAT(ToJson(comp_auto_name, db), HasSubstr("\"CompoundCRS\""));
    EXPECT_THAT(ToProj(*comp_auto_name), HasSubstr("+proj=utm"));

    auto time_crs = p_crs::TemporalCRS::create(
        MakeNameProps("Time"),
        p_datum::TemporalDatum::create(
            MakeNameProps("TD"),
            p_common::DateTime::create("2000-01-01T00:00:00Z"),
            p_datum::TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN),
        p_cs::DateTimeTemporalCS::create(
            p_util::PropertyMap(),
            p_cs::CoordinateSystemAxis::create(MakeNameProps("t"), "t",
                                               p_cs::AxisDirection::FUTURE,
                                               p_common::UnitOfMeasure::NONE)));
    auto comp3 = p_crs::CompoundCRS::create(MakeNameProps("SpatioTemporal"),
                                            {proj, vert, time_crs});
    EXPECT_THROW(ToWkt(comp3, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
                 p_io::FormattingException);
    EXPECT_THAT(ToWkt(comp3, p_io::WKTFormatter::Convention::WKT2_2019, db),
                HasSubstr("COMPOUNDCRS["));

    // createLax: Geo2D + Geo2D equivalent vs mismatched, Bound + Vert 2002
    // (ft/DOWN).
    auto lax_geog = p_crs::CompoundCRS::createLax(
        MakeNameProps("LaxGeog"),
        {p_crs::GeographicCRS::EPSG_4326, p_crs::GeographicCRS::EPSG_4326}, db);
    EXPECT_THAT(ToWkt(lax_geog, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                HasSubstr("COMPD_CS[\"WGS 84 + WGS 84\""));
    EXPECT_THROW(
        p_crs::CompoundCRS::createLax(
            MakeNameProps("LaxBad"),
            {p_crs::GeographicCRS::EPSG_4326, p_crs::GeographicCRS::EPSG_4269},
            db),
        p_crs::InvalidCompoundCRSException);

    p_util::PropertyMap p2002;
    p2002.set(p_common::IdentifiedObject::NAME_KEY, "EllipsFt");
    p2002.set("VERT_DATUM_TYPE", "2002");
    auto vd2002 = p_datum::VerticalReferenceFrame::create(p2002);
    auto vert_2002_ft =
        p_crs::VerticalCRS::create(MakeNameProps("Ellips (ft)"), vd2002,
                                   p_cs::VerticalCS::createGravityRelatedHeight(
                                       p_common::UnitOfMeasure::FOOT));
    auto proj_ft = proj->alterCSLinearUnit(p_common::UnitOfMeasure::FOOT);
    auto bound_proj_ft = p_crs::BoundCRS::createFromTOWGS84(proj_ft, {0, 0, 0});
    auto lax_bound_2002 = p_crs::CompoundCRS::createLax(
        MakeNameProps("OrigComp"), {bound_proj_ft, vert_2002_ft}, db);
    EXPECT_THAT(lax_bound_2002->nameStr(), HasSubstr("(Ellips (ft))"));
    EXPECT_THAT(
        ToWkt(lax_bound_2002, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        HasSubstr("COMPD_CS[\"OrigComp\""));

    // CompoundCRS::identify: null, DB EPSG:7405, wrong ID, synthesized
    // Horiz + Vert (no DB CompoundCRS exists for UTM 31N + NAVD88).
    EXPECT_THAT(comp_auto_name->identify(nullptr), IsEmpty());
    auto c7405 = auth_epsg->createCompoundCRS("7405");
    EXPECT_THAT(c7405->identify(auth_epsg), SizeIs(1));
    auto c_synth = comp_auto_name->identify(auth_epsg);
    ASSERT_THAT(c_synth, SizeIs(1));
    EXPECT_EQ(c_synth.front().second, 100);
    auto comp_diff_name =
        p_crs::CompoundCRS::create(MakeNameProps("SomeName"), {proj, vert});
    auto c_synth_diff = comp_diff_name->identify(auth_epsg);
    ASSERT_THAT(c_synth_diff, SizeIs(1));
    EXPECT_EQ(c_synth_diff.front().second, 70);
  }

  // BoundCRS: create variants, createFromTOWGS84, createFromNadgrids (with &
  // without Paris PM), canonicalBoundCRS, WKT2/WKT1/ESRI, JSON (source_crs
  // omission vs inclusion), PROJ, eq, _identify.
  {
    auto g4267 = p_crs::GeographicCRS::EPSG_4267;
    auto g4807 = auth_epsg->createGeographicCRS("4807");  // Paris PM
    auto p26711 = auth_epsg->createProjectedCRS("26711");
    auto p32631 = auth_epsg->createProjectedCRS("32631");
    auto v5703 = auth_epsg->createVerticalCRS("5703");

    auto bound_towgs = p_crs::BoundCRS::createFromTOWGS84(g4267, {1, 2, 3});
    EXPECT_THAT(
        ToWkt(bound_towgs, p_io::WKTFormatter::Convention::WKT2_2019, db),
        AllOf(HasSubstr("BOUNDCRS["), HasSubstr("ABRIDGEDTRANSFORMATION[")));
    EXPECT_THAT(
        ToWkt(bound_towgs, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        HasSubstr("TOWGS84[1,2,3,0,0,0,0]"));
    EXPECT_THAT(
        ToWkt(bound_towgs, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
        AllOf(HasSubstr("GEOGCS["), Not(HasSubstr("TOWGS84["))));
    EXPECT_THAT(ToProj(*bound_towgs), HasSubstr("+towgs84=1,2,3,0,0,0,0"));
    EXPECT_THAT(
        bound_towgs->baseCRSWithCanonicalBoundCRS()->canonicalBoundCRS().get(),
        NotNull());

    auto bound_grids = p_crs::BoundCRS::createFromNadgrids(g4267, "conus");
    EXPECT_THAT(
        ToWkt(bound_grids, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        HasSubstr("EXTENSION[\"PROJ4_GRIDS\",\"conus\"]"));
    EXPECT_THAT(ToProj(*bound_grids), HasSubstr("+nadgrids=conus"));

    auto bound_paris =
        p_crs::BoundCRS::createFromNadgrids(g4807, "ntf_r93.gsb");
    EXPECT_THAT(bound_paris->transformation()->sourceCRS()->nameStr(),
                HasSubstr("(with Greenwich prime meridian)"));

    // Vertical BoundCRS with height-to-geog3D grid (hub WGS 84 vs horizontal
    // compound CRS hub).
    auto vert_transf_wgs =
        p_op::Transformation::createGravityRelatedHeightToGeographic3D(
            MakeNameProps("VGrid"), v5703, p_crs::GeographicCRS::EPSG_4979,
            p_crs::GeographicCRS::EPSG_4326.as_nullable(), "g2012ba0.gtx", {});
    auto bound_v_wgs = p_crs::BoundCRS::create(
        v5703, p_crs::GeographicCRS::EPSG_4326, vert_transf_wgs);
    EXPECT_THAT(
        ToWkt(bound_v_wgs, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        HasSubstr("EXTENSION[\"PROJ4_GRIDS\",\"g2012ba0.gtx\"]"));
    EXPECT_THAT(ToProj(*bound_v_wgs),
                AllOf(HasSubstr("+geoidgrids=g2012ba0.gtx"),
                      HasSubstr("+geoid_crs=WGS84")));

    auto comp_v_hub = p_crs::CompoundCRS::create(
        MakeNameProps("CompVertGrid"),
        {p_crs::GeographicCRS::EPSG_4269, bound_v_wgs});
    auto vert_transf_nad =
        p_op::Transformation::createGravityRelatedHeightToGeographic3D(
            MakeNameProps("VGridNAD"), v5703, p_crs::GeographicCRS::EPSG_4979,
            p_crs::GeographicCRS::EPSG_4269.as_nullable(), "g2012ba0.gtx", {});
    auto bound_v_nad = p_crs::BoundCRS::create(
        v5703, p_crs::GeographicCRS::EPSG_4269, vert_transf_nad);
    auto comp_v_horiz = p_crs::CompoundCRS::create(
        MakeNameProps("CompHoriz"),
        {p_crs::GeographicCRS::EPSG_4269, bound_v_nad});
    EXPECT_THAT(ToProj(*comp_v_horiz), HasSubstr("+geoid_crs=horizontal_crs"));
    (void)comp_v_hub;

    // Non-WGS84 hub BoundCRS: WKT1 throws, JSON source_crs branch tested.
    auto non_wgs_transf = p_op::Transformation::createGeocentricTranslations(
        MakeNameProps("ToNAD83"), g4267, p_crs::GeographicCRS::EPSG_4269, 1.0,
        2.0, 3.0, {});
    auto bound_non_wgs = p_crs::BoundCRS::create(
        MakeNameProps("CustomBoundName"), g4267,
        p_crs::GeographicCRS::EPSG_4269, non_wgs_transf);
    EXPECT_THROW(
        ToWkt(bound_non_wgs, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        p_io::FormattingException);
    EXPECT_THAT(ToJson(bound_non_wgs, db),
                AllOf(HasSubstr("\"name\":\"CustomBoundName\""),
                      HasSubstr("\"BoundCRS\"")));

    // JSON abridgedTransformationWriteSourceCRS true vs false on ProjectedCRS.
    auto bound_proj_write_src = p_crs::BoundCRS::create(
        p32631, p_crs::GeographicCRS::EPSG_4269, non_wgs_transf);
    EXPECT_THAT(ToJson(bound_proj_write_src, db), HasSubstr("\"source_crs\""));

    auto transf_nad27_wgs = p_op::Transformation::createGeocentricTranslations(
        MakeNameProps("ToWGS84"), g4267, p_crs::GeographicCRS::EPSG_4326, -8.0,
        160.0, 176.0, {});
    auto bound_proj_omit_src = p_crs::BoundCRS::create(
        p26711, p_crs::GeographicCRS::EPSG_4326, transf_nad27_wgs);
    EXPECT_THAT(ToJson(bound_proj_omit_src, db), HasSubstr("\"BoundCRS\""));

    // Unexportable baseCRS in BoundCRS throws for PROJ export.
    auto eng_crs = p_crs::EngineeringCRS::create(
        MakeNameProps("Eng"),
        p_datum::EngineeringDatum::create(MakeNameProps("ED")),
        p_cs::CartesianCS::createEastingNorthing(
            p_common::UnitOfMeasure::METRE));
    auto bound_eng = p_crs::BoundCRS::create(eng_crs, g4267, transf_nad27_wgs);
    EXPECT_THROW(ToProj(*bound_eng), p_io::FormattingException);

    // _identify on BoundCRS (null factory vs EPSG:4326 hub with exact/null
    // transf and Paris ConcatenatedOperation).
    EXPECT_THAT(bound_towgs->identify(nullptr), IsEmpty());
    EXPECT_FALSE(bound_proj_omit_src->identify(auth_epsg).empty());
    auto bound_null_4267 = p_crs::BoundCRS::createFromTOWGS84(g4267, {0, 0, 0});
    EXPECT_FALSE(bound_null_4267->identify(auth_epsg).empty());
    auto bound_paris_0 = p_crs::BoundCRS::createFromTOWGS84(g4807, {0, 0, 0});
    EXPECT_FALSE(bound_paris_0->identify(auth_epsg).empty());
  }
}

// ---------------------------------------------------------------------------
// Group 7: Other Derived CRS and Non-Geodetic CRS Types
// ---------------------------------------------------------------------------

TEST(CrsTest, OtherDerivedAndNonGeodeticCrs) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");

  // DerivedGeodeticCRS (CartesianCS and SphericalCS).
  {
    auto aff = p_op::Conversion::createAffineParametric(
        MakeNameProps("AffGeod"), p_common::Length(1.0), p_common::Scale(1.0),
        p_common::Scale(0.0), p_common::Length(2.0), p_common::Scale(0.0),
        p_common::Scale(1.0));
    auto d_geod_cart = p_crs::DerivedGeodeticCRS::create(
        MakeNameProps("DerivedGeodCart"), p_crs::GeodeticCRS::EPSG_4978, aff,
        p_cs::CartesianCS::createGeocentric(p_common::UnitOfMeasure::METRE));
    EXPECT_THAT(d_geod_cart->baseCRS()->nameStr(), Eq("WGS 84"));
    EXPECT_TRUE(d_geod_cart->shallowClone()->isEquivalentTo(d_geod_cart.get()));
    EXPECT_THAT(
        ToWkt(d_geod_cart, p_io::WKTFormatter::Convention::WKT2_2019, db),
        AllOf(HasSubstr("GEODCRS[\"DerivedGeodCart\""),
              HasSubstr("BASEGEODCRS[\"WGS 84\"")));
    EXPECT_THROW(
        ToWkt(d_geod_cart, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        p_io::FormattingException);
    EXPECT_THROW(ToProj(*d_geod_cart), p_io::FormattingException);
    EXPECT_THAT(ToJson(d_geod_cart, db), HasSubstr("\"DerivedGeodeticCRS\""));
    EXPECT_THAT(d_geod_cart->identify(auth_epsg), IsEmpty());

    auto sph_lat = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("Spherical latitude"), "phi", p_cs::AxisDirection::NORTH,
        p_common::UnitOfMeasure::DEGREE);
    auto sph_lon = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("Spherical longitude"), "theta",
        p_cs::AxisDirection::EAST, p_common::UnitOfMeasure::DEGREE);
    auto sph_r = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("Geocentric radius"), "r", p_cs::AxisDirection::UP,
        p_common::UnitOfMeasure::METRE);
    auto d_geod_sph = p_crs::DerivedGeodeticCRS::create(
        MakeNameProps("DerivedGeodSph"), p_crs::GeodeticCRS::EPSG_4978, aff,
        p_cs::SphericalCS::create(p_util::PropertyMap(), sph_lat, sph_lon,
                                  sph_r));
    EXPECT_THAT(
        ToWkt(d_geod_sph, p_io::WKTFormatter::Convention::WKT2_2015, db),
        HasSubstr("CS[spherical,3]"));
    EXPECT_FALSE(d_geod_cart->isEquivalentTo(d_geod_sph.get()));
  }

  // DerivedGeographicCRS (WKT2_2019 vs WKT2_2015 vs WKT1 throw, PROJ ob_tran,
  // GRIB pole rotation, NetCDF CF pole rotation, generic throw, JSON, eq,
  // demoteTo2D).
  {
    auto dg_grib = p_crs::DerivedGeographicCRS::create(
        MakeNameProps("RotatedGRIB"), p_crs::GeographicCRS::EPSG_4326,
        p_op::Conversion::createPoleRotationGRIBConvention(
            MakeNameProps("PoleRotGRIB"),
            p_common::Angle(-30.0, p_common::UnitOfMeasure::DEGREE),
            p_common::Angle(10.0, p_common::UnitOfMeasure::DEGREE),
            p_common::Angle(0.0, p_common::UnitOfMeasure::DEGREE)),
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_TRUE(dg_grib->shallowClone()->isEquivalentTo(dg_grib.get()));
    EXPECT_THAT(ToWkt(dg_grib, p_io::WKTFormatter::Convention::WKT2_2019, db),
                AllOf(HasSubstr("GEOGCRS[\"RotatedGRIB\""),
                      HasSubstr("BASEGEOGCRS[\"WGS 84\"")));
    EXPECT_THAT(ToWkt(dg_grib, p_io::WKTFormatter::Convention::WKT2_2015, db),
                AllOf(HasSubstr("GEODCRS[\"RotatedGRIB\""),
                      HasSubstr("BASEGEODCRS[\"WGS 84\"")));
    EXPECT_THROW(ToWkt(dg_grib, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                 p_io::FormattingException);
    EXPECT_THAT(ToProj(*dg_grib), HasSubstr("+proj=ob_tran"));
    EXPECT_THAT(ToJson(dg_grib, db), HasSubstr("\"DerivedGeographicCRS\""));
    EXPECT_THAT(dg_grib->identify(auth_epsg), IsEmpty());

    auto dg_cf = p_crs::DerivedGeographicCRS::create(
        MakeNameProps("RotatedCF"), p_crs::GeographicCRS::EPSG_4326,
        p_op::Conversion::createPoleRotationNetCDFCFConvention(
            MakeNameProps("PoleRotCF"),
            p_common::Angle(39.25, p_common::UnitOfMeasure::DEGREE),
            p_common::Angle(-162.0, p_common::UnitOfMeasure::DEGREE),
            p_common::Angle(18.0, p_common::UnitOfMeasure::DEGREE)),
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THAT(ToProj(*dg_cf), HasSubstr("+proj=ob_tran"));
    EXPECT_FALSE(dg_grib->isEquivalentTo(dg_cf.get()));

    // PROJ ob_tran o_proj=longlat via PROJStringParser.
    auto parsed_ob_tran =
        p_io::PROJStringParser().attachDatabaseContext(db).createFromPROJString(
            "+proj=ob_tran +o_proj=longlat +o_lat_p=39.25 +o_lon_p=-162 "
            "+lon_0=18 "
            "+datum=WGS84 +type=crs");
    auto dg_ob_tran =
        dynamic_cast<const p_crs::DerivedGeographicCRS*>(parsed_ob_tran.get());
    ASSERT_THAT(dg_ob_tran, NotNull());
    EXPECT_THAT(ToProj(*dg_ob_tran), HasSubstr("+proj=ob_tran"));

    // Non-ob_tran deriving conversion throws for PROJ export.
    auto dg_bad_proj = p_crs::DerivedGeographicCRS::create(
        MakeNameProps("NonProjDerived"), p_crs::GeographicCRS::EPSG_4326,
        p_op::Conversion::createGeographic2DOffsets(
            MakeNameProps("Off"),
            p_common::Angle(1.0, p_common::UnitOfMeasure::DEGREE),
            p_common::Angle(2.0, p_common::UnitOfMeasure::DEGREE)),
        p_cs::EllipsoidalCS::createLatitudeLongitude(
            p_common::UnitOfMeasure::DEGREE));
    EXPECT_THROW(ToProj(*dg_bad_proj), p_io::FormattingException);
  }

  // DerivedProjectedCRS: WKT2_2019 vs WKT2_2015 throw, WKT2_2019_SIMPLIFIED,
  // non-metre baseProjCRS CS vs DB unique match, PROJ
  // addUnitConvertAndAxisSwap, JSON, eq.
  {
    auto base_proj_m = auth_epsg->createProjectedCRS("32631");
    auto base_proj_ft = p_crs::ProjectedCRS::create(
        MakeNameProps("Custom Foot Proj"), p_crs::GeographicCRS::EPSG_4326,
        MakeMercatorConversion(),
        p_cs::CartesianCS::createEastingNorthing(
            p_common::UnitOfMeasure::FOOT));
    auto aff = p_op::Conversion::createAffineParametric(
        MakeNameProps("Aff"), p_common::Length(0.0), p_common::Scale(1.0),
        p_common::Scale(0.0), p_common::Length(0.0), p_common::Scale(0.0),
        p_common::Scale(1.0));
    auto affine_cs = p_cs::AffineCS::create(
        p_util::PropertyMap(),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("i"), "i",
                                           p_cs::AxisDirection::COLUMN_POSITIVE,
                                           p_common::UnitOfMeasure::FOOT),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("j"), "j",
                                           p_cs::AxisDirection::ROW_POSITIVE,
                                           p_common::UnitOfMeasure::FOOT));
    auto dp_custom_ft = p_crs::DerivedProjectedCRS::create(
        MakeNameProps("DP Custom Foot"), base_proj_ft, aff, affine_cs);
    EXPECT_TRUE(
        dp_custom_ft->shallowClone()->isEquivalentTo(dp_custom_ft.get()));
    EXPECT_THAT(
        ToWkt(dp_custom_ft, p_io::WKTFormatter::Convention::WKT2_2019, db),
        AllOf(HasSubstr("DERIVEDPROJCRS[\"DP Custom Foot\""),
              HasSubstr("CS[Cartesian,2]"), HasSubstr("CS[affine,2]")));
    EXPECT_THROW(
        ToWkt(dp_custom_ft, p_io::WKTFormatter::Convention::WKT2_2015, db),
        p_io::FormattingException);

    // DerivedProjectedCRS without ID over DB baseProjCRS emits baseProjCRS ID,
    // and WKT2_2019_SIMPLIFIED hits PM/parameter unit omission branch.
    auto dp_with_db_base = p_crs::DerivedProjectedCRS::create(
        MakeNameProps("DP DB Base"), base_proj_m, aff, affine_cs);
    EXPECT_THAT(ToWkt(dp_with_db_base,
                      p_io::WKTFormatter::Convention::WKT2_2019_SIMPLIFIED, db),
                AllOf(HasSubstr("BASEPROJCRS[\"WGS 84 / UTM zone 31N\""),
                      HasSubstr("ID[\"EPSG\",32631]")));
    auto dp_proj_fmt = p_io::PROJStringFormatter::create();
    dp_proj_fmt->setCRSExport(true);
    dp_with_db_base->addUnitConvertAndAxisSwap(dp_proj_fmt.get());
    EXPECT_THAT(dp_proj_fmt->toString(), HasSubstr("+units=ft"));
    EXPECT_THAT(ToJson(dp_with_db_base, db),
                HasSubstr("\"DerivedProjectedCRS\""));
    EXPECT_FALSE(dp_with_db_base->isEquivalentTo(dp_custom_ft.get()));
  }

  // TemporalCRS, EngineeringCRS, ParametricCRS, and Derived{Engineering,
  // Parametric,Temporal}CRS.
  {
    auto t_datum = p_datum::TemporalDatum::create(
        MakeNameProps("Gregorian"),
        p_common::DateTime::create("2000-01-01T00:00:00Z"),
        p_datum::TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN);
    auto t_axis = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("t"), "t", p_cs::AxisDirection::FUTURE,
        p_common::UnitOfMeasure::SECOND);
    auto t_measure = p_crs::TemporalCRS::create(
        MakeNameProps("TimeMeasure"), t_datum,
        p_cs::TemporalMeasureCS::create(p_util::PropertyMap(), t_axis));
    EXPECT_TRUE(t_measure->shallowClone()->isEquivalentTo(t_measure.get()));
    EXPECT_THAT(ToWkt(t_measure, p_io::WKTFormatter::Convention::WKT2_2019, db),
                HasSubstr("TIMECRS[\"TimeMeasure\""));
    EXPECT_THROW(
        ToWkt(t_measure, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        p_io::FormattingException);
    EXPECT_THAT(ToJson(t_measure, db), HasSubstr("\"TemporalCRS\""));

    auto t_unnamed = p_crs::TemporalCRS::create(
        MakeNameProps(""), t_datum,
        p_cs::TemporalCountCS::create(
            p_util::PropertyMap(),
            p_cs::CoordinateSystemAxis::create(MakeNameProps("c"), "c",
                                               p_cs::AxisDirection::FUTURE,
                                               p_common::UnitOfMeasure::NONE)));
    EXPECT_THAT(ToJson(t_unnamed, db), HasSubstr("\"name\":\"unnamed\""));

    // DerivedTemporalCRS (supports WKT2_2015 and WKT2_2019; throws WKT1).
    auto aff_t = p_op::Conversion::createAffineParametric(
        MakeNameProps("AffT"), p_common::Length(10.0), p_common::Scale(1.0),
        p_common::Scale(0.0), p_common::Length(0.0), p_common::Scale(0.0),
        p_common::Scale(1.0));
    auto dt = p_crs::DerivedTemporalCRS::create(
        MakeNameProps("DerivedTime"), t_measure, aff_t,
        p_cs::TemporalMeasureCS::create(p_util::PropertyMap(), t_axis));
    EXPECT_TRUE(dt->shallowClone()->isEquivalentTo(dt.get()));
    EXPECT_THAT(dt->baseCRS()->nameStr(), Eq("TimeMeasure"));
    EXPECT_THAT(ToWkt(dt, p_io::WKTFormatter::Convention::WKT2_2015, db),
                HasSubstr("BASETIMECRS[\"TimeMeasure\""));
    EXPECT_THROW(ToWkt(dt, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                 p_io::FormattingException);
    EXPECT_THAT(ToJson(dt, db), HasSubstr("\"DerivedTemporalCRS\""));

    // EngineeringCRS: LOCAL_CS omits "Unknown engineering datum", keeps custom;
    // EQUIVALENT X/Y <-> Easting/Northing with UNSPECIFIED axes.
    auto ed_unknown = p_datum::EngineeringDatum::create(
        MakeNameProps("Unknown engineering datum"));
    auto ed_custom =
        p_datum::EngineeringDatum::create(MakeNameProps("Site Datum"));
    auto cs_xy_unspec = p_cs::CartesianCS::create(
        p_util::PropertyMap(),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("X"), "X",
                                           p_cs::AxisDirection::UNSPECIFIED,
                                           p_common::UnitOfMeasure::METRE),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("Y"), "Y",
                                           p_cs::AxisDirection::UNSPECIFIED,
                                           p_common::UnitOfMeasure::METRE));
    auto cs_en_unspec = p_cs::CartesianCS::create(
        p_util::PropertyMap(),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("Easting"), "E",
                                           p_cs::AxisDirection::UNSPECIFIED,
                                           p_common::UnitOfMeasure::METRE),
        p_cs::CoordinateSystemAxis::create(MakeNameProps("Northing"), "N",
                                           p_cs::AxisDirection::UNSPECIFIED,
                                           p_common::UnitOfMeasure::METRE));
    auto eng_1 = p_crs::EngineeringCRS::create(MakeNameProps("Eng1"),
                                               ed_unknown, cs_xy_unspec);
    auto eng_2 = p_crs::EngineeringCRS::create(MakeNameProps("Eng2"),
                                               ed_unknown, cs_en_unspec);
    auto eng_3 = p_crs::EngineeringCRS::create(MakeNameProps("Eng3"), ed_custom,
                                               cs_en_unspec);
    EXPECT_TRUE(eng_1->shallowClone()->isEquivalentTo(eng_1.get()));
    EXPECT_THAT(ToWkt(eng_1, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                AllOf(HasSubstr("LOCAL_CS[\"Eng1\""),
                      Not(HasSubstr("LOCAL_DATUM[\"Unknown"))));
    EXPECT_THAT(ToWkt(eng_3, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                AllOf(HasSubstr("LOCAL_CS[\"Eng3\""),
                      HasSubstr("LOCAL_DATUM[\"Site Datum\"")));
    EXPECT_THAT(ToJson(eng_3, db), HasSubstr("\"EngineeringCRS\""));
    EXPECT_TRUE(eng_1->isEquivalentTo(
        eng_2.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
    EXPECT_TRUE(eng_2->isEquivalentTo(
        eng_1.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
    EXPECT_FALSE(eng_1->isEquivalentTo(
        eng_2.get(), p_util::IComparable::Criterion::STRICT, db));

    // DerivedEngineeringCRS (WKT2_2019 only).
    auto d_eng = p_crs::DerivedEngineeringCRS::create(
        MakeNameProps("DerivedEng"), eng_3, aff_t, cs_xy_unspec);
    EXPECT_TRUE(d_eng->shallowClone()->isEquivalentTo(d_eng.get()));
    EXPECT_THAT(d_eng->baseCRS()->nameStr(), Eq("Eng3"));
    EXPECT_THAT(ToWkt(d_eng, p_io::WKTFormatter::Convention::WKT2_2019, db),
                HasSubstr("BASEENGCRS[\"Eng3\""));
    EXPECT_THROW(ToWkt(d_eng, p_io::WKTFormatter::Convention::WKT2_2015, db),
                 p_io::FormattingException);
    EXPECT_THAT(ToJson(d_eng, db), HasSubstr("\"DerivedEngineeringCRS\""));

    // ParametricCRS and DerivedParametricCRS.
    auto p_datum =
        p_datum::ParametricDatum::create(MakeNameProps("Pressure Datum"));
    auto p_axis = p_cs::CoordinateSystemAxis::create(
        MakeNameProps("Pressure"), "P", p_cs::AxisDirection::DOWN,
        p_common::UnitOfMeasure("hPa", 100.0,
                                p_common::UnitOfMeasure::Type::PARAMETRIC));
    auto p_cs = p_cs::ParametricCS::create(p_util::PropertyMap(), p_axis);
    auto param_crs =
        p_crs::ParametricCRS::create(MakeNameProps("ParamCRS"), p_datum, p_cs);
    EXPECT_TRUE(param_crs->shallowClone()->isEquivalentTo(param_crs.get()));
    EXPECT_THAT(ToWkt(param_crs, p_io::WKTFormatter::Convention::WKT2_2019, db),
                HasSubstr("PARAMETRICCRS[\"ParamCRS\""));
    EXPECT_THROW(
        ToWkt(param_crs, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
        p_io::FormattingException);
    EXPECT_THAT(ToJson(param_crs, db), HasSubstr("\"ParametricCRS\""));

    auto d_param = p_crs::DerivedParametricCRS::create(
        MakeNameProps("DerivedParam"), param_crs, aff_t, p_cs);
    EXPECT_TRUE(d_param->shallowClone()->isEquivalentTo(d_param.get()));
    EXPECT_THAT(d_param->baseCRS()->nameStr(), Eq("ParamCRS"));
    EXPECT_THAT(ToWkt(d_param, p_io::WKTFormatter::Convention::WKT2_2015, db),
                HasSubstr("BASEPARAMCRS[\"ParamCRS\""));
    EXPECT_THROW(ToWkt(d_param, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
                 p_io::FormattingException);
    EXPECT_THAT(ToJson(d_param, db), HasSubstr("\"DerivedParametricCRS\""));
  }
}

// ---------------------------------------------------------------------------
// Group 8: Additional Branch Coverage for CRS Base, Geodetic, Vertical,
//          Projected, Compound, Bound, and Derived CRS
// ---------------------------------------------------------------------------

TEST(CrsTest, AdditionalBaseCrsAndBoundToWgs84Branches) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");

  auto g4326 = auth_epsg->createGeographicCRS("4326");
  auto g4269 = auth_epsg->createGeographicCRS("4269");
  auto g4151 = auth_epsg->createGeographicCRS("4151");
  auto v5703 = auth_epsg->createVerticalCRS("5703");
  auto p32631 = auth_epsg->createProjectedCRS("32631");

  // extractGeographicCRS on CompoundCRS without any geodetic component (269,
  // 306) and extractVerticalCRS on CompoundCRS without vertical component
  // (492).
  auto ed = p_datum::EngineeringDatum::create(MakeNameProps("EngDatum"));
  auto eng_2d = p_crs::EngineeringCRS::create(
      MakeNameProps("Eng2D"), ed,
      p_cs::CartesianCS::createEastingNorthing(p_common::UnitOfMeasure::METRE));
  auto comp_eng_vert =
      p_crs::CompoundCRS::create(MakeNameProps("Eng+Vert"), {eng_2d, v5703});
  EXPECT_THAT(comp_eng_vert->extractGeographicCRS(), IsNull());
  EXPECT_THAT(comp_eng_vert->extractGeodeticCRS(), IsNull());

  auto td = p_datum::TemporalDatum::create(
      MakeNameProps("TempDatum"), p_common::DateTime::create("2000-01-01"),
      p_datum::TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN);
  auto t_axis = p_cs::CoordinateSystemAxis::create(
      MakeNameProps("Time"), "t", p_cs::AxisDirection::FUTURE,
      p_common::UnitOfMeasure::SECOND);
  auto temp_crs = p_crs::TemporalCRS::create(
      MakeNameProps("TempCRS"), td,
      p_cs::TemporalCountCS::create(p_util::PropertyMap(), t_axis));
  auto comp_geog_temp =
      p_crs::CompoundCRS::create(MakeNameProps("Geog+Temp"), {g4326, temp_crs});
  EXPECT_THAT(comp_geog_temp->extractVerticalCRS(), IsNull());

  // createPropertyMap preserving DEPRECATED_KEY (317-318), and
  // CRS::alterGeodeticCRS directly on GeographicCRS (329) and
  // DerivedProjectedCRS (339-346).
  p_util::PropertyMap dep_props = MakeNameProps("DeprecatedGeog");
  dep_props.set(p_common::IdentifiedObject::DEPRECATED_KEY, true);
  auto dep_geog = p_crs::GeographicCRS::create(
      dep_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
      p_cs::EllipsoidalCS::createLatitudeLongitude(
          p_common::UnitOfMeasure::DEGREE));
  EXPECT_TRUE(dep_geog->isDeprecated());
  auto dep_altered = dep_geog->alterGeodeticCRS(g4269);
  EXPECT_FALSE(dep_altered->isDeprecated());
  EXPECT_THAT(dep_altered->extractGeodeticCRS()->nameStr(), Eq("NAD83"));

  auto aff = p_op::Conversion::createAffineParametric(
      MakeNameProps("Affine"), p_common::Length(10.0), p_common::Scale(1.0),
      p_common::Scale(0.0), p_common::Length(20.0), p_common::Scale(0.0),
      p_common::Scale(1.0));
  auto dp_2d = p_crs::DerivedProjectedCRS::create(
      MakeNameProps("DerivedProj2D"), p32631, aff,
      p_cs::CartesianCS::createEastingNorthing(p_common::UnitOfMeasure::METRE));
  auto dp_altered_geod = dp_2d->alterGeodeticCRS(g4269);
  EXPECT_THAT(dp_altered_geod->extractGeodeticCRS()->nameStr(), Eq("NAD83"));

  // createBoundCRSToWGS84IfPossible:
  // - BoundCRS whose hubCRS is not EPSG:4326 (532).
  auto non_wgs_transf = p_op::Transformation::createGeocentricTranslations(
      MakeNameProps("ToNAD83"), g4326, g4269, 1.0, 2.0, 3.0, {});
  auto bound_hub_nad83 = p_crs::BoundCRS::create(g4326, g4269, non_wgs_transf);
  auto bound_stays = bound_hub_nad83->createBoundCRSToWGS84IfPossible(
      db, p_op::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  EXPECT_EQ(bound_stays.get(), bound_hub_nad83.get());
  EXPECT_NE(bound_hub_nad83->allowNonConformantWKT1Export().get(), nullptr);

  // - CRS with 2 domains where extent0 is contained in extent1 (570-571).
  auto small_ext = p_meta::Extent::createFromBBOX(5.0, 45.0, 10.0, 48.0);
  auto large_ext = p_meta::Extent::createFromBBOX(-10.0, 35.0, 30.0, 60.0);
  auto dom_arr = p_util::ArrayOfBaseObject::create();
  dom_arr->add(p_common::ObjectDomain::create(
      p_util::optional<std::string>("s1"), small_ext));
  dom_arr->add(p_common::ObjectDomain::create(
      p_util::optional<std::string>("s2"), large_ext));
  p_util::PropertyMap multi_dom_props = MakeNameProps("CHTRF95_TwoDomains");
  multi_dom_props.set(p_common::ObjectUsage::OBJECT_DOMAIN_KEY, dom_arr);
  auto g_two_domains = p_crs::GeographicCRS::create(
      multi_dom_props, g4151->datum(), nullptr, g4151->coordinateSystem());
  auto bound_two_dom = g_two_domains->createBoundCRSToWGS84IfPossible(
      db, p_op::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  EXPECT_NE(bound_two_dom.get(), nullptr);

  // - VerticalCRS EGM96 height (EPSG:5773) has a known open-license grid from
  //   EPSG:4979 -> creates a BoundCRS (626-632, 636, 638, 642-647, 650).
  auto v5773 = auth_epsg->createVerticalCRS("5773");
  auto v5773_bound = v5773->createBoundCRSToWGS84IfPossible(
      db, p_op::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  EXPECT_NE(dynamic_cast<const p_crs::BoundCRS*>(v5773_bound.get()), nullptr);

  // - VerticalCRS EGM84 height (EPSG:5798) has an unknown grid (DIRACC.DAT)
  //   from EPSG:4979 -> gridsKnown = false (633-634).
  auto v5798 = auth_epsg->createVerticalCRS("5798");
  auto v5798_bound = v5798->createBoundCRSToWGS84IfPossible(
      db, p_op::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  EXPECT_EQ(v5798_bound.get(), v5798.get());

  // - Static EPSG_4807 has no domains -> getResolvedCRS called at 688-689.
  auto bound_static_4807 =
      p_crs::GeographicCRS::EPSG_4807->createBoundCRSToWGS84IfPossible(
          db, p_op::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  EXPECT_NE(dynamic_cast<const p_crs::BoundCRS*>(bound_static_4807.get()),
            nullptr);

  // - DerivedGeographicCRS based on EPSG:4151 (CHTRF95, single Helmert to
  //   WGS 84) -> ConcatenatedOperation(Conversion + Transformation) (741-742).
  auto rot_conv = p_op::Conversion::createAxisOrderReversal(false);
  auto dg_4151 = p_crs::DerivedGeographicCRS::create(
      MakeNameProps("DerivedCHTRF95"), g4151, rot_conv,
      p_cs::EllipsoidalCS::createLongitudeLatitude(
          p_common::UnitOfMeasure::DEGREE));
  auto dg_4151_bound = dg_4151->createBoundCRSToWGS84IfPossible(
      db, p_op::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  EXPECT_NE(dg_4151_bound.get(), nullptr);

  // mustAxisOrderBeSwitchedForVisualization on Polar Stereographic South with
  // NORTH along 180 deg and NORTH along 90 deg W (EPSG:5482) (906-911).
  auto p5482 = auth_epsg->createProjectedCRS("5482");
  EXPECT_TRUE(p5482->mustAxisOrderBeSwitchedForVisualization());

  // applyAxisOrderReversal when name already ends with suffix (1026, 1028),
  // and on 3D ProjectedCRS (1102-1103) and 3D DerivedProjectedCRS (1116-1117).
  auto p32631_3d = p32631->promoteTo3D("", db);
  auto p32631_3d_norm = p32631_3d->normalizeForVisualization();
  EXPECT_NE(dynamic_cast<const p_crs::ProjectedCRS*>(p32631_3d_norm.get()),
            nullptr);
  auto dp_3d = dp_2d->promoteTo3D("", db);
  auto dp_3d_norm = dp_3d->normalizeForVisualization();
  EXPECT_NE(dynamic_cast<const p_crs::DerivedProjectedCRS*>(dp_3d_norm.get()),
            nullptr);

  auto g4326_norm1 = g4326->normalizeForVisualization();
  p_util::PropertyMap rev_props =
      MakeNameProps("CustomCRS (with axis order reversed)");
  auto g_already_rev = p_crs::GeographicCRS::create(
      rev_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
      p_cs::EllipsoidalCS::createLatitudeLongitude(
          p_common::UnitOfMeasure::DEGREE));
  auto g_rev_again = g_already_rev->normalizeForVisualization();
  EXPECT_THAT(g_rev_again->nameStr(), Eq("CustomCRS"));
  EXPECT_NE(g4326_norm1.get(), nullptr);

  // promoteTo3D on 2D DerivedProjectedCRS with remarks and FOOT unit
  // (1322-1328, 1381-1382, 1406-1420) and 2D DerivedGeographicCRS (1388-1402).
  p_util::PropertyMap dp_rem_props = MakeNameProps("DerivedProjFoot");
  dp_rem_props.set(p_common::IdentifiedObject::REMARKS_KEY, "Test remark");
  dp_rem_props.set(p_common::ObjectUsage::SCOPE_KEY, "Test scope");
  auto dp_foot = p_crs::DerivedProjectedCRS::create(
      dp_rem_props, p32631, aff,
      p_cs::CartesianCS::createEastingNorthing(p_common::UnitOfMeasure::FOOT));
  auto dp_foot_3d = dp_foot->promoteTo3D("DerivedProjFoot3D", db);
  EXPECT_NE(dynamic_cast<const p_crs::DerivedProjectedCRS*>(dp_foot_3d.get()),
            nullptr);
  EXPECT_THAT(dp_foot_3d->remarks(), Eq("Test remark"));

  auto dg_3d = dg_4151->promoteTo3D("DerivedCHTRF95_3D", db);
  EXPECT_NE(dynamic_cast<const p_crs::DerivedGeographicCRS*>(dg_3d.get()),
            nullptr);
}

TEST(CrsTest, AdditionalGeodeticAndGeographicBranches) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");

  // SingleCRS::_isEquivalentTo STRICT with one datum and one datumEnsemble
  // (1681, 1684), and GeodeticCRS::create with both datum and datumEnsemble
  // throwing (1764).
  auto ens = MakeGeodeticEnsemble();
  auto g_with_ens =
      p_crs::GeographicCRS::create(MakeNameProps("WGS 84"), nullptr, ens,
                                   p_cs::EllipsoidalCS::createLatitudeLongitude(
                                       p_common::UnitOfMeasure::DEGREE));
  auto g_with_datum = p_crs::GeographicCRS::create(
      MakeNameProps("WGS 84"), p_datum::GeodeticReferenceFrame::EPSG_6326,
      p_cs::EllipsoidalCS::createLatitudeLongitude(
          p_common::UnitOfMeasure::DEGREE));
  EXPECT_FALSE(g_with_datum->isEquivalentTo(
      g_with_ens.get(), p_util::IComparable::Criterion::STRICT, db));
  EXPECT_FALSE(g_with_ens->isEquivalentTo(
      g_with_datum.get(), p_util::IComparable::Criterion::STRICT, db));
  EXPECT_THROW(
      p_crs::GeodeticCRS::create(
          MakeNameProps("BothThrows"),
          p_datum::GeodeticReferenceFrame::EPSG_6326, ens,
          p_cs::CartesianCS::createGeocentric(p_common::UnitOfMeasure::METRE)),
      p_util::Exception);

  // GeodeticCRS::shallowClone on non-Geographic GeodeticCRS (1820-1824).
  auto geoc_clone = p_crs::GeodeticCRS::EPSG_4978->shallowClone();
  EXPECT_TRUE(geoc_clone->isEquivalentTo(p_crs::GeodeticCRS::EPSG_4978.get()));

  // isSphericalPlanetocentric with 2D SphericalCS having planetocentric
  // longitude first, planetocentric latitude second (1944-1945).
  auto sp_lon = p_cs::CoordinateSystemAxis::create(
      MakeNameProps("Planetocentric longitude"), "lon",
      p_cs::AxisDirection::EAST, p_common::UnitOfMeasure::DEGREE);
  auto sp_lat = p_cs::CoordinateSystemAxis::create(
      MakeNameProps("Planetocentric latitude"), "lat",
      p_cs::AxisDirection::NORTH, p_common::UnitOfMeasure::DEGREE);
  auto sp_cs_2d =
      p_cs::SphericalCS::create(p_util::PropertyMap(), sp_lon, sp_lat);
  auto sp_geod = p_crs::GeodeticCRS::create(
      MakeNameProps("PlanetocentricLonLat"),
      p_datum::GeodeticReferenceFrame::EPSG_6326, sp_cs_2d);
  EXPECT_TRUE(sp_geod->isSphericalPlanetocentric());

  // exportAsESRIWktCompoundCRSWithEllipsoidalHeight fallback when datum has no
  // ESRI:from_geogdatum_EPSG_<code> entry in proj.db (e.g. EPSG:3823 TWD97 3D,
  // datum EPSG:1026) (2080-2105).
  auto g3823 = auth_epsg->createGeographicCRS("3823");
  EXPECT_THAT(ToWkt(g3823, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
              AllOf(HasSubstr("GEOGCS[\"GCS_TWD_1997\""),
                    HasSubstr("VERTCS[\"D_TWD_1997\"")));

  // attachOriginalCompoundCRS on a 3D GeographicCRS exported to WKT1_GDAL
  // (2205-2206).
  auto g4326 = auth_epsg->createGeographicCRS("4326");
  auto g4979 = auth_epsg->createGeographicCRS("4979");
  auto v5703 = auth_epsg->createVerticalCRS("5703");
  auto comp_orig =
      p_crs::CompoundCRS::create(MakeNameProps("WGS84+NAVD88"), {g4326, v5703});
  auto g4979_with_orig = g4979->attachOriginalCompoundCRS(comp_orig);
  EXPECT_THAT(
      ToWkt(g4979_with_orig, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
      HasSubstr("COMPD_CS["));

  // GeodeticCRS::_exportToWKT WKT1_ESRI DB name/alias lookup:
  // - CRS without ID whose name is an ESRI GeodeticCRS name (2265).
  auto g_esri_named = p_crs::GeographicCRS::create(
      MakeNameProps("GCS_WGS_1984"), p_datum::GeodeticReferenceFrame::EPSG_6326,
      p_cs::EllipsoidalCS::createLatitudeLongitude(
          p_common::UnitOfMeasure::DEGREE));
  EXPECT_THAT(
      ToWkt(g_esri_named, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
      HasSubstr("GEOGCS[\"GCS_WGS_1984\""));

  // - EPSG:10875 (2D) and EPSG:10874 (3D) have no direct ESRI alias and 1
  //   EPSG_OLD alias ("ETRS89"), resolving via candidate loop (2270-2325).
  auto g10875 = auth_epsg->createGeographicCRS("10875");
  EXPECT_THAT(ToWkt(g10875, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("GCS_ETRS_1989"));
  auto g10874 = auth_epsg->createGeographicCRS("10874");
  EXPECT_THAT(
      ToWkt(g10874, p_io::WKTFormatter::Convention::WKT1_ESRI, db,
            /*strict=*/false, std::nullopt, /*allow_ellps_height_vert=*/false,
            /*allow_linunit=*/true),
      HasSubstr("GEOGCS[\"ETRS_1989_3D\""));

  // GeodeticCRS PROJ string export branches:
  // - Geocentric CRS with custom linear unit having no PROJ short name
  //   (2404-2406).
  p_common::UnitOfMeasure custom_lin("HalfM", 0.5,
                                     p_common::UnitOfMeasure::Type::LINEAR);
  auto geoc_custom_u = p_crs::GeodeticCRS::create(
      MakeNameProps("GeocCustomU"), p_datum::GeodeticReferenceFrame::EPSG_6326,
      p_cs::CartesianCS::createGeocentric(custom_lin));
  EXPECT_THAT(
      ToProj(*geoc_custom_u, p_io::PROJStringFormatter::Convention::PROJ_5, db,
             /*crs_export=*/false),
      HasSubstr("+xy_out=0.5 +z_out=0.5"));

  // - 3D GeographicCRS with custom angular unit and custom linear Z unit
  //   (2459-2460, 2468-2469).
  p_common::UnitOfMeasure custom_ang("Centirad", 0.01,
                                     p_common::UnitOfMeasure::Type::ANGULAR);
  auto ellps_custom_3d =
      p_cs::EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(custom_ang,
                                                                    custom_lin);
  auto geog_custom_3d = p_crs::GeographicCRS::create(
      MakeNameProps("GeogCustom3D"), p_datum::GeodeticReferenceFrame::EPSG_6326,
      ellps_custom_3d);
  EXPECT_THAT(
      ToProj(*geog_custom_3d, p_io::PROJStringFormatter::Convention::PROJ_5, db,
             /*crs_export=*/false),
      AllOf(HasSubstr("+xy_out=0.01"), HasSubstr("+z_out=0.5")));

  // - Geocentric CRS with EXTENSION_PROJ4 property (2486-2489), and with NAD27
  //   / NAD83 datums in crs_export mode (2555-2556, 2563-2564).
  p_util::PropertyMap ext_geoc_props = MakeNameProps("GeocExt");
  ext_geoc_props.set("EXTENSION_PROJ4", "+proj=cart +ellps=WGS84 +over");
  auto geoc_ext = p_crs::GeodeticCRS::create(
      ext_geoc_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
      p_cs::CartesianCS::createGeocentric(p_common::UnitOfMeasure::METRE));
  EXPECT_THAT(ToProj(*geoc_ext), HasSubstr("+over"));

  auto geoc_nad27 = p_crs::GeodeticCRS::create(
      MakeNameProps("GeocNAD27"), p_datum::GeodeticReferenceFrame::EPSG_6267,
      p_cs::CartesianCS::createGeocentric(p_common::UnitOfMeasure::METRE));
  EXPECT_THAT(ToProj(*geoc_nad27), HasSubstr("+datum=NAD27"));
  auto geoc_nad83 = p_crs::GeodeticCRS::create(
      MakeNameProps("GeocNAD83"), p_datum::GeodeticReferenceFrame::EPSG_6269,
      p_cs::CartesianCS::createGeocentric(p_common::UnitOfMeasure::METRE));
  EXPECT_THAT(ToProj(*geoc_nad83), HasSubstr("+datum=NAD83"));

  // GeodeticCRS::identify branches:
  // - Name "WGS 84" with NAD27 datum and nullptr factory -> 25% (2800-2802).
  auto g_wgs_name_nad27 = p_crs::GeographicCRS::create(
      MakeNameProps("WGS 84"), p_datum::GeodeticReferenceFrame::EPSG_6267,
      p_cs::EllipsoidalCS::createLatitudeLongitude(
          p_common::UnitOfMeasure::DEGREE));
  auto id_wgs_nad27 = g_wgs_name_nad27->identify(nullptr);
  ASSERT_THAT(id_wgs_nad27, SizeIs(1));
  EXPECT_THAT(id_wgs_nad27.front().second, Eq(25));

  // - Name "OtherName" with EPSG_6326 datum and nullptr factory -> 70%
  //   (2804-2806).
  auto g_other_wgs_datum = p_crs::GeographicCRS::create(
      MakeNameProps("OtherName"), p_datum::GeodeticReferenceFrame::EPSG_6326,
      p_cs::EllipsoidalCS::createLatitudeLongitude(
          p_common::UnitOfMeasure::DEGREE));
  auto id_other_wgs = g_other_wgs_datum->identify(nullptr);
  ASSERT_THAT(id_other_wgs, SizeIs(1));
  EXPECT_THAT(id_other_wgs.front().second, Eq(70));

  // - 3D GeographicCRS without ID identified via auth_epsg (2820, 2952-2954).
  auto g4979_no_id = g4979->alterName("WGS 84");
  auto id_4979 = g4979_no_id->identify(auth_epsg);
  ASSERT_FALSE(id_4979.empty());
  EXPECT_THAT(id_4979.front().second, Eq(100));

  // - GeographicCRS with unregistered datum name and custom Ellipsoid without
  //   IDs -> createEllipsoidFromExisting (2858-2859).
  auto custom_ellps = p_datum::Ellipsoid::createFlattenedSphere(
      MakeNameProps("CustomGRS80"), p_common::Length(6378137.0),
      p_common::Scale(298.257222101));
  auto custom_datum_no_id = p_datum::GeodeticReferenceFrame::create(
      MakeNameProps("UnregisteredDatumName"), custom_ellps,
      p_util::optional<std::string>(), p_datum::PrimeMeridian::GREENWICH);
  auto g_custom_ellps = p_crs::GeographicCRS::create(
      MakeNameProps("UnregisteredCRSName"), custom_datum_no_id,
      p_cs::EllipsoidalCS::createLatitudeLongitude(
          p_common::UnitOfMeasure::DEGREE));
  EXPECT_FALSE(g_custom_ellps->identify(auth_epsg).empty());

  // - GeographicCRS with unregistered CRS name and datum without ID whose name
  //   is "European Terrestrial Reference System 1989" -> datum lookup by name
  //   (2905-2907).
  auto etrs_datum_no_id = p_datum::GeodeticReferenceFrame::create(
      MakeNameProps("European Terrestrial Reference System 1989"), custom_ellps,
      p_util::optional<std::string>(), p_datum::PrimeMeridian::GREENWICH);
  auto g_etrs_datum_name = p_crs::GeographicCRS::create(
      MakeNameProps("UnregisteredCRSName"), etrs_datum_no_id,
      p_cs::EllipsoidalCS::createLatitudeLongitude(
          p_common::UnitOfMeasure::DEGREE));
  EXPECT_FALSE(g_etrs_datum_name->identify(auth_epsg).empty());

  // GeographicCRS::is2DPartOf3D & _isEquivalentTo with
  // EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS on 3D and West/South CS (3208, 3219,
  // 3279-3300).
  EXPECT_FALSE(g4979->is2DPartOf3D(NN_CHECK_THROW(g4979.get()), db));
  auto g4979_lon_lat = p_crs::GeographicCRS::create(
      MakeNameProps("WGS 84"), p_datum::GeodeticReferenceFrame::EPSG_6326,
      p_cs::EllipsoidalCS::createLongitudeLatitudeEllipsoidalHeight(
          p_common::UnitOfMeasure::DEGREE, p_common::UnitOfMeasure::METRE));
  EXPECT_FALSE(g4326->is2DPartOf3D(NN_CHECK_THROW(g4979_lon_lat.get()), db));
  EXPECT_TRUE(g4979->isEquivalentTo(
      g4979_lon_lat.get(),
      p_util::IComparable::Criterion::EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS,
      db));
  EXPECT_TRUE(g4979_lon_lat->isEquivalentTo(
      g4979.get(),
      p_util::IComparable::Criterion::EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS,
      db));

  auto ws_cs = p_cs::EllipsoidalCS::create(
      p_util::PropertyMap(),
      p_cs::CoordinateSystemAxis::create(MakeNameProps("Lat"), "lat",
                                         p_cs::AxisDirection::SOUTH,
                                         p_common::UnitOfMeasure::DEGREE),
      p_cs::CoordinateSystemAxis::create(MakeNameProps("Lon"), "lon",
                                         p_cs::AxisDirection::WEST,
                                         p_common::UnitOfMeasure::DEGREE));
  auto g_ws = p_crs::GeographicCRS::create(
      MakeNameProps("WGS 84"), p_datum::GeodeticReferenceFrame::EPSG_6326,
      ws_cs);
  EXPECT_FALSE(g_ws->isEquivalentTo(
      g4326.get(),
      p_util::IComparable::Criterion::EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS,
      db));

  // GeographicCRS::_exportToPROJString with EXTENSION_PROJ4 (3431-3434),
  // legacy_crs_to_crs on EPSG_4326 (3451-3452), and OVER without
  // EXTENSION_PROJ4 (3469-3470).
  p_util::PropertyMap g_ext_props = MakeNameProps("GeogExt");
  g_ext_props.set("EXTENSION_PROJ4", "+proj=longlat +ellps=WGS84 +over");
  auto g_ext = p_crs::GeographicCRS::create(
      g_ext_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
      p_cs::EllipsoidalCS::createLatitudeLongitude(
          p_common::UnitOfMeasure::DEGREE));
  EXPECT_THAT(ToProj(*g_ext), HasSubstr("+over"));
  EXPECT_THAT(ToProj(*p_crs::GeographicCRS::EPSG_4326,
                     p_io::PROJStringFormatter::Convention::PROJ_5, db,
                     /*crs_export=*/true, /*legacy_crs_to_crs=*/true),
              HasSubstr("+ellps=WGS84"));
  p_util::PropertyMap g_over_props = MakeNameProps("GeogOver");
  g_over_props.set("OVER", true);
  auto g_over = p_crs::GeographicCRS::create(
      g_over_props, p_datum::GeodeticReferenceFrame::EPSG_6326,
      p_cs::EllipsoidalCS::createLatitudeLongitude(
          p_common::UnitOfMeasure::DEGREE));
  EXPECT_THAT(ToProj(*g_over), HasSubstr("+over"));
}

TEST(CrsTest, AdditionalVerticalProjectedCompoundAndBoundBranches) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");
  auto auth_esri = p_io::AuthorityFactory::create(db, "ESRI");
  auto auth_all = p_io::AuthorityFactory::create(db, std::string());

  auto g4326 = auth_epsg->createGeographicCRS("4326");
  auto g4807 = auth_epsg->createGeographicCRS("4807");
  auto v5703 = auth_epsg->createVerticalCRS("5703");
  auto p32631 = auth_epsg->createProjectedCRS("32631");
  auto c7405 = auth_epsg->createCompoundCRS("7405");

  // VerticalCRS with DatumEnsemble (3504, 3511, 3604, 3770-3772),
  // shallowClone (3541-3545), geoidModel/velocityModel (3579-3580),
  // addLinearUnitConvert (3827-3844), and identify (4000-4002, 4029, 4032).
  auto vd1 = p_datum::VerticalReferenceFrame::create(MakeNameProps("VD1"));
  auto vd2 = p_datum::VerticalReferenceFrame::create(MakeNameProps("VD2"));
  std::vector<p_datum::DatumNNPtr> v_datums = {vd1, vd2};
  auto v_ens =
      p_datum::DatumEnsemble::create(MakeNameProps("VertEnsemble"), v_datums,
                                     p_meta::PositionalAccuracy::create("0.5"));
  EXPECT_THROW(
      p_crs::VerticalCRS::create(MakeNameProps("BothVertThrows"), vd1, v_ens,
                                 p_cs::VerticalCS::createGravityRelatedHeight(
                                     p_common::UnitOfMeasure::METRE)),
      p_util::Exception);
  auto v_with_ens =
      p_crs::VerticalCRS::create(MakeNameProps("VertWithEns"), nullptr, v_ens,
                                 p_cs::VerticalCS::createGravityRelatedHeight(
                                     p_common::UnitOfMeasure::METRE));
  EXPECT_THAT(v_with_ens->datumNonNull(db)->nameStr(), Eq("VertEnsemble"));
  EXPECT_THAT(ToJson(v_with_ens, db), HasSubstr("\"datum_ensemble\""));
  EXPECT_TRUE(v5703->shallowClone()->isEquivalentTo(v5703.get()));
  EXPECT_THAT(v5703->geoidModel(), IsEmpty());
  EXPECT_THAT(v5703->velocityModel(), IsEmpty());

  auto v_ft =
      p_crs::VerticalCRS::create(MakeNameProps("VertFoot"), vd1,
                                 p_cs::VerticalCS::createGravityRelatedHeight(
                                     p_common::UnitOfMeasure::FOOT));
  auto proj_fmt_ft = p_io::PROJStringFormatter::create(
      p_io::PROJStringFormatter::Convention::PROJ_5, db);
  v_ft->addLinearUnitConvert(proj_fmt_ft.get());
  EXPECT_THAT(proj_fmt_ft->toString(), HasSubstr("ft"));
  p_common::UnitOfMeasure half_m("HalfM", 0.5,
                                 p_common::UnitOfMeasure::Type::LINEAR);
  auto v_half = p_crs::VerticalCRS::create(
      MakeNameProps("VertHalf"), vd1,
      p_cs::VerticalCS::createGravityRelatedHeight(half_m));
  auto proj_fmt_half = p_io::PROJStringFormatter::create(
      p_io::PROJStringFormatter::Convention::PROJ_5, db);
  v_half->addLinearUnitConvert(proj_fmt_half.get());
  EXPECT_THAT(proj_fmt_half->toString(), HasSubstr("0.5"));

  auto v5703_no_id = v5703->alterName("NAVD88 height");
  auto id_v5703 = v5703_no_id->identify(auth_epsg);
  ASSERT_THAT(id_v5703, SizeIs(1));
  EXPECT_THAT(id_v5703.front().second, Eq(100));

  auto v_msl_ft = v_ft->alterName("MSL height");
  EXPECT_FALSE(v_msl_ft->identify(auth_all).empty());
  EXPECT_FALSE(
      static_cast<const p_crs::CRS*>(v5703.get())->identify(auth_epsg).empty());

  // ProjectedCRS::_exportToWKT WKT1_ESRI DB lookup:
  // - Without ID whose name is an ESRI ProjectedCRS name (4412, 4445-4459).
  auto p_esri_53008 = auth_esri->createProjectedCRS("53008");
  EXPECT_THAT(
      ToWkt(p_esri_53008, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
      HasSubstr("PROJCS[\"Sphere_Sinusoidal\""));
  auto p_esri_no_id = p_esri_53008->alterName("Sphere_Sinusoidal");
  EXPECT_THAT(
      ToWkt(p_esri_no_id, p_io::WKTFormatter::Convention::WKT1_ESRI, db),
      HasSubstr("PROJCS[\"Sphere_Sinusoidal\""));

  // ProjectedCRS unnamed JSON export (4614-4615), 3D with custom Z unit PROJ
  // export (4732, 4751-4752), and Northing/Easting PROJ export (4784,
  // 4787-4788).
  auto p_unnamed = p32631->alterName("");
  EXPECT_THAT(ToJson(p_unnamed, db), HasSubstr("\"unnamed\""));

  auto cart_3d_half_z = p_cs::CartesianCS::create(
      p_util::PropertyMap(),
      p_cs::CoordinateSystemAxis::create(MakeNameProps("Easting"), "E",
                                         p_cs::AxisDirection::EAST,
                                         p_common::UnitOfMeasure::METRE),
      p_cs::CoordinateSystemAxis::create(MakeNameProps("Northing"), "N",
                                         p_cs::AxisDirection::NORTH,
                                         p_common::UnitOfMeasure::METRE),
      p_cs::CoordinateSystemAxis::create(MakeNameProps("Height"), "h",
                                         p_cs::AxisDirection::UP, half_m),
      /*enforceSameUnit=*/false);
  auto p_3d_half_z =
      p_crs::ProjectedCRS::create(MakeNameProps("Proj3DHalfZ"), g4326,
                                  p32631->derivingConversion(), cart_3d_half_z);
  EXPECT_THAT(
      ToProj(*p_3d_half_z, p_io::PROJStringFormatter::Convention::PROJ_5, db,
             /*crs_export=*/false),
      HasSubstr("0.5"));

  auto p_ne = p_crs::ProjectedCRS::create(
      MakeNameProps("ProjNE"), g4326, p32631->derivingConversion(),
      p_cs::CartesianCS::createNorthingEasting(p_common::UnitOfMeasure::METRE));
  EXPECT_THAT(ToProj(*p_ne, p_io::PROJStringFormatter::Convention::PROJ_5, db,
                     /*crs_export=*/false),
              HasSubstr("+order=2,1"));

  // ProjectedCRS::identify branches:
  // - IMPLICIT_CS with Northing/Easting (4972-4982).
  p_util::PropertyMap imp_props = MakeNameProps("WGS 84 / UTM zone 31N");
  imp_props.set("IMPLICIT_CS", true);
  auto p_implicit_ne = p_crs::ProjectedCRS::create(
      imp_props, g4326, p32631->derivingConversion(),
      p_cs::CartesianCS::createNorthingEasting(p_common::UnitOfMeasure::METRE));
  EXPECT_FALSE(p_implicit_ne->identify(auth_epsg).empty());

  // - Invalid EPSG ID (5052-5056) and non-matching EPSG ID (5036-5050,
  //   4989-4993).
  auto p_bad_id = p_crs::ProjectedCRS::create(
      MakeNameCodeProps("WGS 84 / UTM zone 31N", "EPSG", "999999"), g4326,
      p32631->derivingConversion(), p32631->coordinateSystem());
  auto id_bad = p_bad_id->identify(auth_epsg);
  ASSERT_FALSE(id_bad.empty());
  EXPECT_THAT(id_bad.front().second, Eq(100));

  auto p_wrong_id = p_crs::ProjectedCRS::create(
      MakeNameCodeProps("WGS 84 / UTM zone 31N", "EPSG", "32632"), g4326,
      p32631->derivingConversion(), p32631->coordinateSystem());
  EXPECT_FALSE(p_wrong_id->identify(auth_epsg).empty());

  // - Insignificant CRS name ("unnamed") and insignificant datum name
  //   ("unknown") -> createProjectedCRSFromExisting (5001-5013).
  auto unknown_datum = p_datum::GeodeticReferenceFrame::create(
      MakeNameProps("unknown"), p_datum::Ellipsoid::WGS84,
      p_util::optional<std::string>(), p_datum::PrimeMeridian::GREENWICH);
  auto g_unknown =
      p_crs::GeographicCRS::create(MakeNameProps("unnamed"), unknown_datum,
                                   p_cs::EllipsoidalCS::createLatitudeLongitude(
                                       p_common::UnitOfMeasure::DEGREE));
  auto p_unknown = p_crs::ProjectedCRS::create(
      MakeNameProps("unnamed"), g_unknown, p32631->derivingConversion(),
      p32631->coordinateSystem());
  EXPECT_FALSE(p_unknown->identify(auth_epsg).empty());

  // - Exact non-UTM name match in DB ("WGS 84 / World Mercator") (5080) and
  //   25% name match falling through to createProjectedCRSFromExisting
  //   (5100-5123).
  auto p3395 = auth_epsg->createProjectedCRS("3395");
  auto p3395_no_id = p3395->alterName("WGS 84 / World Mercator");
  auto id_3395 = p3395_no_id->identify(auth_epsg);
  ASSERT_THAT(id_3395, SizeIs(1));
  EXPECT_THAT(id_3395.front().second, Eq(100));

  auto p3857 = auth_epsg->createProjectedCRS("3857");
  auto p3857_wrong_conv = p_crs::ProjectedCRS::create(
      MakeNameProps("WGS 84 / Pseudo-Mercator"), g4326,
      p32631->derivingConversion(), p32631->coordinateSystem());
  EXPECT_FALSE(p3857_wrong_conv->identify(auth_all).empty());

  // CompoundCRS::InvalidCompoundCRSException (5225, 5227, 5235-5236),
  // CompoundCRS::create combinations (5324, 5330, 5332, 5337-5347, 5369),
  // unnamed JSON export (5503-5504), isEquivalentTo mismatches (5549, 5554,
  // 5559), and identify / _identify (5638-5735, 5768-5778, 5789-5797).
  p_crs::InvalidCompoundCRSException exc1(std::string("invalid compound"));
  p_crs::InvalidCompoundCRSException exc2(exc1);
  EXPECT_THAT(std::string(exc2.what()), Eq("invalid compound"));

  auto eng_1d_cs = p_cs::CartesianCS::create(
      p_util::PropertyMap(),
      p_cs::CoordinateSystemAxis::create(MakeNameProps("X"), "X",
                                         p_cs::AxisDirection::EAST,
                                         p_common::UnitOfMeasure::METRE),
      p_cs::CoordinateSystemAxis::create(MakeNameProps("Y"), "Y",
                                         p_cs::AxisDirection::NORTH,
                                         p_common::UnitOfMeasure::METRE));
  auto ed = p_datum::EngineeringDatum::create(MakeNameProps("ED"));
  auto eng_2d =
      p_crs::EngineeringCRS::create(MakeNameProps("Eng2D"), ed, eng_1d_cs);
  auto comp_eng_v =
      p_crs::CompoundCRS::create(MakeNameProps("Eng2D+V"), {eng_2d, v5703});
  EXPECT_THAT(comp_eng_v->componentReferenceSystems(), SizeIs(2));

  auto aff = p_op::Conversion::createAffineParametric(
      MakeNameProps("Affine"), p_common::Length(1.0), p_common::Scale(1.0),
      p_common::Scale(0.0), p_common::Length(2.0), p_common::Scale(0.0),
      p_common::Scale(1.0));
  auto dp_2d = p_crs::DerivedProjectedCRS::create(
      MakeNameProps("DP2D"), p32631, aff, p32631->coordinateSystem());
  auto comp_dp_v =
      p_crs::CompoundCRS::create(MakeNameProps("DP+V"), {dp_2d, v5703});
  EXPECT_THAT(comp_dp_v->componentReferenceSystems(), SizeIs(2));

  auto td = p_datum::TemporalDatum::create(
      MakeNameProps("TD"), p_common::DateTime::create("2000-01-01"),
      p_datum::TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN);
  auto temp_crs = p_crs::TemporalCRS::create(
      MakeNameProps("Temp"), td,
      p_cs::TemporalCountCS::create(
          p_util::PropertyMap(),
          p_cs::CoordinateSystemAxis::create(MakeNameProps("Time"), "t",
                                             p_cs::AxisDirection::FUTURE,
                                             p_common::UnitOfMeasure::SECOND)));
  auto comp_geoc_t = p_crs::CompoundCRS::create(
      MakeNameProps("Geoc+T"), {p_crs::GeodeticCRS::EPSG_4978, temp_crs});
  EXPECT_THAT(comp_geoc_t->componentReferenceSystems(), SizeIs(2));

  auto pd = p_datum::ParametricDatum::create(MakeNameProps("PD"));
  auto param_crs = p_crs::ParametricCRS::create(
      MakeNameProps("Param"), pd,
      p_cs::ParametricCS::create(
          p_util::PropertyMap(),
          p_cs::CoordinateSystemAxis::create(
              MakeNameProps("P"), "P", p_cs::AxisDirection::UP,
              p_common::UnitOfMeasure(
                  "hPa", 100.0, p_common::UnitOfMeasure::Type::PARAMETRIC))));
  auto comp_geog_param = p_crs::CompoundCRS::create(MakeNameProps("Geog+Param"),
                                                    {g4326, param_crs});
  EXPECT_THAT(comp_geog_param->componentReferenceSystems(), SizeIs(2));

  auto comp_auto_unnamed =
      p_crs::CompoundCRS::create(p_util::PropertyMap(), {p_unnamed, v5703});
  EXPECT_THAT(comp_auto_unnamed->nameStr(), HasSubstr("unnamed"));
  auto comp_empty_name = comp_auto_unnamed->alterName("");
  EXPECT_THAT(ToJson(comp_empty_name, db), HasSubstr("\"unnamed\""));

  auto comp_3 = p_crs::CompoundCRS::create(MakeNameProps("3Comp"),
                                           {g4326, v5703, temp_crs});
  EXPECT_FALSE(comp_eng_v->isEquivalentTo(
      comp_dp_v.get(), p_util::IComparable::Criterion::STRICT, db));
  EXPECT_FALSE(comp_eng_v->isEquivalentTo(
      comp_3.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
  EXPECT_FALSE(comp_eng_v->isEquivalentTo(
      comp_dp_v.get(), p_util::IComparable::Criterion::EQUIVALENT, db));

  auto c_bad_id =
      p_crs::CompoundCRS::create(MakeNameCodeProps("BadComp", "EPSG", "999999"),
                                 c7405->componentReferenceSystems());
  EXPECT_TRUE(c_bad_id->identify(auth_epsg).empty());

  auto c7405_exact_name =
      c7405->alterName("OSGB36 / British National Grid + ODN height");
  auto id_c7405_exact = c7405_exact_name->identify(auth_epsg);
  ASSERT_THAT(id_c7405_exact, SizeIs(1));
  EXPECT_THAT(id_c7405_exact.front().second, Eq(100));

  auto c7405_eq_name =
      c7405->alterName("OSGB36_British_National_Grid_ODN_height");
  auto id_c7405_eq = c7405_eq_name->identify(auth_epsg);
  ASSERT_THAT(id_c7405_eq, SizeIs(1));
  EXPECT_THAT(id_c7405_eq.front().second, Eq(100));

  auto c7405_unnamed = c7405->alterName("unnamed");
  EXPECT_FALSE(c7405_unnamed->identify(auth_epsg).empty());

  auto c_approx_mismatch = p_crs::CompoundCRS::create(
      MakeNameProps("ETRS89 + EVRF"),
      {c7405->componentReferenceSystems()[0], v5703});
  EXPECT_FALSE(c_approx_mismatch->identify(auth_epsg).empty());
  EXPECT_FALSE(
      static_cast<const p_crs::CRS*>(c7405.get())->identify(auth_epsg).empty());

  // BoundCRS::_isEquivalentTo (6212-6229) and _identify (6298-6306, 6335-6375).
  auto b1 = p_crs::BoundCRS::createFromTOWGS84(p32631, {0, 0, 0});
  auto b2 = p_crs::BoundCRS::createFromTOWGS84(p32631, {1, 2, 3});
  EXPECT_TRUE(b1->isEquivalentTo(
      b1.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
  EXPECT_FALSE(b1->isEquivalentTo(
      b2.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
  EXPECT_FALSE(b1->isEquivalentTo(
      p32631.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
  EXPECT_FALSE(b1->identify(auth_epsg).empty());
  EXPECT_FALSE(b2->identify(auth_epsg).empty());

  auto b_paris_1193 =
      p_crs::BoundCRS::createFromTOWGS84(g4807, {-168.0, -60.0, 320.0});
  EXPECT_FALSE(b_paris_1193->identify(auth_epsg).empty());
}

TEST(CrsTest, AdditionalDerivedEngineeringParametricAndVerticalBranches) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDb(ctx);
  auto auth_epsg = p_io::AuthorityFactory::create(db, "EPSG");

  // DerivedGeodeticCRS whose base CRS has a DatumEnsemble exported to WKT2_2019
  // (6520-6523).
  auto ens = MakeGeodeticEnsemble();
  auto g_ens =
      p_crs::GeographicCRS::create(MakeNameProps("GeogWithEns"), nullptr, ens,
                                   p_cs::EllipsoidalCS::createLatitudeLongitude(
                                       p_common::UnitOfMeasure::DEGREE));
  auto aff = p_op::Conversion::createAffineParametric(
      MakeNameProps("Aff"), p_common::Length(1.0), p_common::Scale(1.0),
      p_common::Scale(0.0), p_common::Length(2.0), p_common::Scale(0.0),
      p_common::Scale(1.0));
  auto d_geod_ens = p_crs::DerivedGeodeticCRS::create(
      MakeNameProps("DerivedGeodEns"), g_ens, aff,
      p_cs::CartesianCS::createGeocentric(p_common::UnitOfMeasure::METRE));
  EXPECT_THAT(ToWkt(d_geod_ens, p_io::WKTFormatter::Convention::WKT2_2019, db),
              HasSubstr("ENSEMBLE["));

  // DerivedProjectedCRS whose baseCRS has no ID, FOOT unit, and a known DB name
  // exported to WKT2_2019 with db -> knownBaseCRS = true (6932-6933).
  auto p32631 = auth_epsg->createProjectedCRS("32631");
  auto p32631_ft_no_id_crs =
      p32631->alterCSLinearUnit(p_common::UnitOfMeasure::FOOT)
          ->alterName("WGS 84 / UTM zone 31N");
  auto p32631_ft_no_id =
      NN_CHECK_THROW(p_util::nn_dynamic_pointer_cast<p_crs::ProjectedCRS>(
          p32631_ft_no_id_crs));
  auto dp_ft = p_crs::DerivedProjectedCRS::create(
      MakeNameProps("DerivedFromKnownNameFoot"), p32631_ft_no_id, aff,
      p_cs::CartesianCS::createEastingNorthing(p_common::UnitOfMeasure::FOOT));
  EXPECT_THAT(ToWkt(dp_ft, p_io::WKTFormatter::Convention::WKT2_2019, db),
              HasSubstr("BASEPROJCRS[\"WGS 84 / UTM zone 31N\""));

  // EngineeringCRS unnamed JSON export (7212-7213) and _isEquivalentTo with
  // X/Y UNSPECIFIED vs Easting/Northing EAST/NORTH (7245, 7252-7280).
  auto ed_unknown = p_datum::EngineeringDatum::create(
      MakeNameProps("Unknown engineering datum"));
  auto ed_other = p_datum::EngineeringDatum::create(
      MakeNameProps("Other engineering datum"));
  auto cs_xy_unspec = p_cs::CartesianCS::create(
      p_util::PropertyMap(),
      p_cs::CoordinateSystemAxis::create(MakeNameProps("X"), "X",
                                         p_cs::AxisDirection::UNSPECIFIED,
                                         p_common::UnitOfMeasure::METRE),
      p_cs::CoordinateSystemAxis::create(MakeNameProps("Y"), "Y",
                                         p_cs::AxisDirection::UNSPECIFIED,
                                         p_common::UnitOfMeasure::METRE));
  auto cs_en_east_north =
      p_cs::CartesianCS::createEastingNorthing(p_common::UnitOfMeasure::METRE);
  auto cs_ne_north_east =
      p_cs::CartesianCS::createNorthingEasting(p_common::UnitOfMeasure::METRE);
  auto eng_xy_unspec = p_crs::EngineeringCRS::create(MakeNameProps(""),
                                                     ed_unknown, cs_xy_unspec);
  auto eng_en_dir = p_crs::EngineeringCRS::create(MakeNameProps("EngEN"),
                                                  ed_unknown, cs_en_east_north);
  auto eng_ne_dir = p_crs::EngineeringCRS::create(MakeNameProps("EngNE"),
                                                  ed_unknown, cs_ne_north_east);
  auto eng_other_datum = p_crs::EngineeringCRS::create(
      MakeNameProps("EngOtherD"), ed_other, cs_xy_unspec);

  EXPECT_THAT(ToJson(eng_xy_unspec, db), HasSubstr("\"unnamed\""));
  EXPECT_FALSE(eng_xy_unspec->isEquivalentTo(
      eng_other_datum.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
  EXPECT_TRUE(eng_xy_unspec->isEquivalentTo(
      eng_en_dir.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
  EXPECT_TRUE(eng_en_dir->isEquivalentTo(
      eng_xy_unspec.get(), p_util::IComparable::Criterion::EQUIVALENT, db));
  EXPECT_FALSE(eng_xy_unspec->isEquivalentTo(
      eng_ne_dir.get(), p_util::IComparable::Criterion::EQUIVALENT, db));

  // ParametricCRS unnamed JSON export (7390-7391).
  auto pd = p_datum::ParametricDatum::create(MakeNameProps("PD"));
  auto param_unnamed = p_crs::ParametricCRS::create(
      MakeNameProps(""), pd,
      p_cs::ParametricCS::create(
          p_util::PropertyMap(),
          p_cs::CoordinateSystemAxis::create(
              MakeNameProps("P"), "P", p_cs::AxisDirection::UP,
              p_common::UnitOfMeasure(
                  "hPa", 100.0, p_common::UnitOfMeasure::Type::PARAMETRIC))));
  EXPECT_THAT(ToJson(param_unnamed, db), HasSubstr("\"unnamed\""));

  // Chained DerivedVerticalCRS (DerivedVerticalCRS whose baseCRS is another
  // DerivedVerticalCRS) exported to WKT1_GDAL (7512, 7522).
  auto v5703 = auth_epsg->createVerticalCRS("5703");
  auto v_conv_other = p_op::Conversion::createAxisOrderReversal(/*is3D=*/false);
  auto dv1 = p_crs::DerivedVerticalCRS::create(
      MakeNameProps("DV1"), v5703, v_conv_other,
      p_cs::VerticalCS::createGravityRelatedHeight(
          p_common::UnitOfMeasure::METRE));
  auto v_conv_unit = p_op::Conversion::createChangeVerticalUnit(
      MakeNameProps("VU1"), p_common::Scale(1.0));
  auto dv2 = p_crs::DerivedVerticalCRS::create(
      MakeNameProps("DV2"), dv1, v_conv_unit,
      p_cs::VerticalCS::createGravityRelatedHeight(
          p_common::UnitOfMeasure::METRE));
  EXPECT_THROW(ToWkt(dv2, p_io::WKTFormatter::Convention::WKT1_GDAL, db),
               p_io::FormattingException);
}

}  // namespace
}  // namespace proj
