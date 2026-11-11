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
// Unit tests for ISO-19111 datum classes (Datum, DatumEnsemble,
// PrimeMeridian, Ellipsoid, GeodeticReferenceFrame,
// DynamicGeodeticReferenceFrame, RealizationMethod, VerticalReferenceFrame,
// DynamicVerticalReferenceFrame, TemporalDatum, EngineeringDatum,
// ParametricDatum) in proj/src/iso19111/datum.cpp. Note: in google3 default
// non-GCE builds, proj.db is embedded in memory and networking is disabled.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/datum.cpp

#include "third_party/proj4/proj/include/proj/datum.hpp"

#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinatesystem.hpp"
#include "third_party/proj4/proj/include/proj/crs.hpp"
#include "third_party/proj4/proj/include/proj/internal/datum_internal.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

using ::osgeo::proj::common::Angle;
using ::osgeo::proj::common::DateTime;
using ::osgeo::proj::common::IdentifiedObject;
using ::osgeo::proj::common::Length;
using ::osgeo::proj::common::Measure;
using ::osgeo::proj::common::ObjectDomain;
using ::osgeo::proj::common::ObjectUsage;
using ::osgeo::proj::common::Scale;
using ::osgeo::proj::common::UnitOfMeasure;
using ::osgeo::proj::crs::GeographicCRS;
using ::osgeo::proj::cs::EllipsoidalCS;
using ::osgeo::proj::datum::DatumEnsemble;
using ::osgeo::proj::datum::DatumNNPtr;
using ::osgeo::proj::datum::DynamicGeodeticReferenceFrame;
using ::osgeo::proj::datum::DynamicVerticalReferenceFrame;
using ::osgeo::proj::datum::Ellipsoid;
using ::osgeo::proj::datum::EngineeringDatum;
using ::osgeo::proj::datum::GeodeticReferenceFrame;
using ::osgeo::proj::datum::ParametricDatum;
using ::osgeo::proj::datum::PrimeMeridian;
using ::osgeo::proj::datum::RealizationMethod;
using ::osgeo::proj::datum::TemporalDatum;
using ::osgeo::proj::datum::VerticalReferenceFrame;
using ::osgeo::proj::io::AuthorityFactory;
using ::osgeo::proj::io::DatabaseContext;
using ::osgeo::proj::io::DatabaseContextNNPtr;
using ::osgeo::proj::io::DatabaseContextPtr;
using ::osgeo::proj::io::FormattingException;
using ::osgeo::proj::io::IJSONExportable;
using ::osgeo::proj::io::IPROJStringExportable;
using ::osgeo::proj::io::IWKTExportable;
using ::osgeo::proj::io::JSONFormatter;
using ::osgeo::proj::io::PROJStringFormatter;
using ::osgeo::proj::io::WKTFormatter;
using ::osgeo::proj::metadata::Extent;
using ::osgeo::proj::metadata::Identifier;
using ::osgeo::proj::metadata::PositionalAccuracy;
using ::osgeo::proj::util::ArrayOfBaseObject;
using ::osgeo::proj::util::BaseObject;
using ::osgeo::proj::util::Exception;
using ::osgeo::proj::util::IComparable;
using ::osgeo::proj::util::nn_static_pointer_cast;
using ::osgeo::proj::util::optional;
using ::osgeo::proj::util::PropertyMap;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;

DatabaseContextNNPtr MakeDbContext(PJ_CONTEXT* ctx) {
  return DatabaseContext::create(std::string(), {}, ctx);
}

std::string ToWkt(const IWKTExportable& obj,
                  WKTFormatter::Convention convention,
                  const DatabaseContextPtr& db_context = nullptr,
                  bool output_id = true) {
  auto formatter = WKTFormatter::create(convention, db_context);
  formatter->setMultiLine(false);
  if (!output_id) {
    formatter->setOutputId(false);
  }
  return obj.exportToWKT(formatter.get());
}

std::string ToJson(const IJSONExportable& obj,
                   const DatabaseContextPtr& db_context = nullptr) {
  auto formatter = JSONFormatter::create(db_context);
  formatter->setMultiLine(false);
  return obj.exportToJSON(formatter.get());
}

std::string ToProjString(const IPROJStringExportable& obj,
                         const DatabaseContextPtr& db_context = nullptr) {
  auto formatter = PROJStringFormatter::create(
      PROJStringFormatter::Convention::PROJ_5, db_context);
  return obj.exportToPROJString(formatter.get());
}

PropertyMap NamedProps(const std::string& name) {
  return PropertyMap().set(IdentifiedObject::NAME_KEY, name);
}

PropertyMap EpsgNamedProps(const std::string& name, int code) {
  return PropertyMap()
      .set(IdentifiedObject::NAME_KEY, name)
      .set(Identifier::CODESPACE_KEY, Identifier::EPSG)
      .set(Identifier::CODE_KEY, code);
}

TEST(DatumTest, DatumPropertiesAnchorEpochAndEquivalence) {
  auto full_datum = GeodeticReferenceFrame::create(
      NamedProps("Test Datum")
          .set("PUBLICATION_DATE", "2020-01-15")
          .set("ANCHOR_EPOCH", "2015.5"),
      Ellipsoid::WGS84, optional<std::string>("Monument A"),
      optional<Measure>(Measure(2010.0, UnitOfMeasure::YEAR)),
      PrimeMeridian::GREENWICH);

  ASSERT_TRUE(full_datum->anchorDefinition().has_value());
  EXPECT_EQ("Monument A", *full_datum->anchorDefinition());
  ASSERT_TRUE(full_datum->anchorEpoch().has_value());
  EXPECT_DOUBLE_EQ(
      2015.5, full_datum->anchorEpoch()->convertToUnit(UnitOfMeasure::YEAR));
  ASSERT_TRUE(full_datum->publicationDate().has_value());
  EXPECT_EQ("2020-01-15", full_datum->publicationDate()->toString());
  EXPECT_EQ(nullptr, full_datum->conventionalRS());

  // Non-numeric ANCHOR_EPOCH property leaves constructor-supplied epoch intact.
  auto bad_epoch_datum = GeodeticReferenceFrame::create(
      NamedProps("Test Datum").set("ANCHOR_EPOCH", "not_a_number"),
      Ellipsoid::WGS84, optional<std::string>("Monument A"),
      optional<Measure>(Measure(2010.0, UnitOfMeasure::YEAR)),
      PrimeMeridian::GREENWICH);
  ASSERT_TRUE(bad_epoch_datum->anchorEpoch().has_value());
  EXPECT_DOUBLE_EQ(2010.0, bad_epoch_datum->anchorEpoch()->convertToUnit(
                               UnitOfMeasure::YEAR));
  EXPECT_FALSE(bad_epoch_datum->publicationDate().has_value());

  const std::string wkt2019 =
      ToWkt(*full_datum, WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(wkt2019, HasSubstr("ANCHOR[\"Monument A\"]"));
  EXPECT_THAT(wkt2019, HasSubstr("ANCHOREPOCH[2015.5]"));

  const std::string wkt2015 =
      ToWkt(*full_datum, WKTFormatter::Convention::WKT2_2015);
  EXPECT_THAT(wkt2015, HasSubstr("ANCHOR[\"Monument A\"]"));
  EXPECT_THAT(wkt2015, Not(HasSubstr("ANCHOREPOCH")));

  const std::string json = ToJson(*full_datum);
  EXPECT_THAT(json, HasSubstr("\"anchor\":\"Monument A\""));
  EXPECT_THAT(json, HasSubstr("\"anchor_epoch\":2015.5"));

  // Equivalence: non-Datum, differing name, anchor, and publication date.
  EXPECT_FALSE(full_datum->isEquivalentTo(Ellipsoid::WGS84.get(),
                                          IComparable::Criterion::STRICT));
  auto other_name = GeodeticReferenceFrame::create(
      NamedProps("Other Datum"), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  EXPECT_FALSE(full_datum->isEquivalentTo(other_name.get(),
                                          IComparable::Criterion::STRICT));

  auto no_anchor = GeodeticReferenceFrame::create(
      NamedProps("Test Datum").set("PUBLICATION_DATE", "2020-01-15"),
      Ellipsoid::WGS84, optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_FALSE(full_datum->isEquivalentTo(no_anchor.get(),
                                          IComparable::Criterion::STRICT));

  auto diff_anchor = GeodeticReferenceFrame::create(
      NamedProps("Test Datum").set("PUBLICATION_DATE", "2020-01-15"),
      Ellipsoid::WGS84, optional<std::string>("Monument B"),
      PrimeMeridian::GREENWICH);
  EXPECT_FALSE(full_datum->isEquivalentTo(diff_anchor.get(),
                                          IComparable::Criterion::STRICT));

  auto no_pub_date = GeodeticReferenceFrame::create(
      NamedProps("Test Datum"), Ellipsoid::WGS84,
      optional<std::string>("Monument A"), PrimeMeridian::GREENWICH);
  EXPECT_FALSE(full_datum->isEquivalentTo(no_pub_date.get(),
                                          IComparable::Criterion::STRICT));

  auto diff_pub_date = GeodeticReferenceFrame::create(
      NamedProps("Test Datum").set("PUBLICATION_DATE", "2022-05-01"),
      Ellipsoid::WGS84, optional<std::string>("Monument A"),
      PrimeMeridian::GREENWICH);
  EXPECT_FALSE(full_datum->isEquivalentTo(diff_pub_date.get(),
                                          IComparable::Criterion::STRICT));

  auto same_strict = GeodeticReferenceFrame::create(
      NamedProps("Test Datum").set("PUBLICATION_DATE", "2020-01-15"),
      Ellipsoid::WGS84, optional<std::string>("Monument A"),
      PrimeMeridian::GREENWICH);
  EXPECT_TRUE(full_datum->isEquivalentTo(same_strict.get(),
                                         IComparable::Criterion::STRICT));
  EXPECT_TRUE(full_datum->isEquivalentTo(no_anchor.get(),
                                         IComparable::Criterion::EQUIVALENT));
}

TEST(DatumTest, PrimeMeridianPropertiesWktJsonProjAndEquivalence) {
  ScopedContext ctx = MakeContext();
  DatabaseContextNNPtr db = MakeDbContext(ctx.get());

  EXPECT_DOUBLE_EQ(0.0, PrimeMeridian::GREENWICH->longitude().value());
  EXPECT_EQ("Reference meridian", PrimeMeridian::REFERENCE_MERIDIAN->nameStr());
  EXPECT_DOUBLE_EQ(2.5969213, PrimeMeridian::PARIS->longitude().value());

  // Unnamed PM defaults to "Greenwich" in WKT2/JSON and is omitted in
  // WKT2_SIMPLIFIED.
  auto unnamed_pm = PrimeMeridian::create(PropertyMap(), Angle(0.0));
  EXPECT_THAT(ToWkt(*unnamed_pm, WKTFormatter::Convention::WKT2),
              HasSubstr("PRIMEM[\"Greenwich\",0"));
  EXPECT_EQ("", ToWkt(*unnamed_pm, WKTFormatter::Convention::WKT2_SIMPLIFIED));
  EXPECT_THAT(ToJson(*unnamed_pm), HasSubstr("\"name\":\"Greenwich\""));

  // WKT2 vs WKT2_SIMPLIFIED unit omission rules (axis unit defaults to DEGREE).
  auto madrid_deg = PrimeMeridian::create(
      NamedProps("Madrid"), Angle(-3.68793888888889, UnitOfMeasure::DEGREE));
  EXPECT_THAT(ToWkt(*madrid_deg, WKTFormatter::Convention::WKT2),
              HasSubstr("ANGLEUNIT[\"degree\""));
  EXPECT_THAT(ToWkt(*madrid_deg, WKTFormatter::Convention::WKT2_SIMPLIFIED),
              Not(HasSubstr("ANGLEUNIT")));
  EXPECT_THAT(
      ToWkt(*PrimeMeridian::PARIS, WKTFormatter::Convention::WKT2_SIMPLIFIED),
      HasSubstr("PRIMEM[\"Paris\",2.5969213,ANGLEUNIT[\"grad\""));

  // WKT1_GDAL converts grad to degrees and honors setOutputId(false).
  const std::string paris_wkt1 =
      ToWkt(*PrimeMeridian::PARIS, WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THAT(paris_wkt1, HasSubstr("PRIMEM[\"Paris\",2.33722917"));
  EXPECT_THAT(paris_wkt1, HasSubstr("AUTHORITY[\"EPSG\",\"8903\"]"));
  EXPECT_THAT(ToWkt(*PrimeMeridian::PARIS, WKTFormatter::Convention::WKT1_GDAL,
                    db, /*output_id=*/false),
              Not(HasSubstr("AUTHORITY")));

  // WKT1_ESRI alias lookup, ESRI authority lookup, and morph fallback.
  auto paris_rgs = PrimeMeridian::create(NamedProps("Paris RGS"),
                                         Angle(2.5969213, UnitOfMeasure::GRAD));
  EXPECT_THAT(ToWkt(*paris_rgs, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("PRIMEM[\"Paris_RGS\","));
  auto ref_esri = PrimeMeridian::create(NamedProps("Reference_Meridian"),
                                        Angle(0.0, UnitOfMeasure::DEGREE));
  EXPECT_THAT(ToWkt(*ref_esri, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("PRIMEM[\"Reference_Meridian\",0.0]"));
  auto custom_pm = PrimeMeridian::create(NamedProps("Custom PM"),
                                         Angle(12.345, UnitOfMeasure::DEGREE));
  EXPECT_THAT(ToWkt(*custom_pm, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("PRIMEM[\"Custom_PM\",12.345]"));

  // JSON: DEGREE vs non-DEGREE (GRAD) longitude formatting, plus id omission
  // when nested inside a GeodeticReferenceFrame that has an identifier.
  const std::string greenwich_json = ToJson(*PrimeMeridian::GREENWICH);
  EXPECT_THAT(greenwich_json, HasSubstr("\"longitude\":0"));
  EXPECT_THAT(greenwich_json, HasSubstr("\"id\""));

  const std::string paris_json = ToJson(*PrimeMeridian::PARIS);
  EXPECT_THAT(paris_json, HasSubstr("\"longitude\":{\"value\":2.5969213"));
  EXPECT_THAT(paris_json, HasSubstr("\"name\":\"grad\""));

  auto grf_with_paris = GeodeticReferenceFrame::create(
      EpsgNamedProps("NTF", 99998), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::PARIS);
  const std::string grf_paris_json = ToJson(*grf_with_paris);
  EXPECT_THAT(grf_paris_json, HasSubstr("\"prime_meridian\":{"));
  EXPECT_THAT(grf_paris_json, Not(HasSubstr("8903")));

  // PROJ string export and getPROJStringWellKnownName.
  EXPECT_EQ("+proj=noop", ToProjString(*PrimeMeridian::GREENWICH));
  EXPECT_EQ("+pm=paris", ToProjString(*PrimeMeridian::PARIS));
  EXPECT_EQ("+pm=12.345", ToProjString(*custom_pm));
  EXPECT_EQ("paris", PrimeMeridian::getPROJStringWellKnownName(
                         Angle(2.33722917, UnitOfMeasure::DEGREE)));
  EXPECT_EQ("", PrimeMeridian::getPROJStringWellKnownName(
                    Angle(12.345, UnitOfMeasure::DEGREE)));

  // Equivalence checks including MapInfo 1e-8 degree tolerance.
  EXPECT_FALSE(PrimeMeridian::PARIS->isEquivalentTo(
      Ellipsoid::WGS84.get(), IComparable::Criterion::STRICT));
  auto paris_in_grad_same_name = PrimeMeridian::create(
      NamedProps("Paris"), Angle(2.5969213, UnitOfMeasure::GRAD));
  auto paris_in_deg_same_name = PrimeMeridian::create(
      NamedProps("Paris"), Angle(2.3372291666667, UnitOfMeasure::DEGREE));
  auto paris_in_deg_equiv_name = PrimeMeridian::create(
      NamedProps("paris"), Angle(2.3372291666667, UnitOfMeasure::DEGREE));
  auto paris_diff_name = PrimeMeridian::create(
      NamedProps("Paris Alt"), Angle(2.3372291666667, UnitOfMeasure::DEGREE));
  EXPECT_FALSE(PrimeMeridian::PARIS->isEquivalentTo(
      paris_diff_name.get(), IComparable::Criterion::STRICT));
  EXPECT_TRUE(PrimeMeridian::PARIS->isEquivalentTo(
      paris_in_grad_same_name.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(PrimeMeridian::PARIS->isEquivalentTo(
      paris_in_deg_same_name.get(), IComparable::Criterion::STRICT));
  EXPECT_TRUE(PrimeMeridian::PARIS->isEquivalentTo(
      paris_in_deg_same_name.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_TRUE(PrimeMeridian::PARIS->isEquivalentTo(
      paris_in_deg_equiv_name.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(PrimeMeridian::PARIS->isEquivalentTo(
      paris_diff_name.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(PrimeMeridian::PARIS->isEquivalentTo(
      custom_pm.get(), IComparable::Criterion::EQUIVALENT));
}

TEST(DatumTest, EllipsoidAccessorsWktJsonProjIdentifyEquivalenceAndBody) {
  ScopedContext ctx = MakeContext();
  DatabaseContextNNPtr db = MakeDbContext(ctx.get());

  // Sphere constructors: plain sphere, invFlattening==0 sphere, and a==b.
  auto sphere =
      Ellipsoid::createSphere(NamedProps("Unit Sphere"), Length(6371000.0));
  EXPECT_TRUE(sphere->isSphere());
  EXPECT_FALSE(sphere->inverseFlattening().has_value());
  EXPECT_FALSE(sphere->semiMinorAxis().has_value());
  EXPECT_FALSE(sphere->semiMedianAxis().has_value());
  EXPECT_DOUBLE_EQ(0.0, sphere->computedInverseFlattening());
  EXPECT_DOUBLE_EQ(0.0, sphere->squaredEccentricity());
  EXPECT_DOUBLE_EQ(6371000.0, sphere->computeSemiMinorAxis().getSIValue());
  EXPECT_EQ(Ellipsoid::EARTH, sphere->celestialBody());

  auto zero_rf_sphere = Ellipsoid::createFlattenedSphere(
      NamedProps("Zero RF"), Length(6371000.0), Scale(0.0));
  EXPECT_TRUE(zero_rf_sphere->isSphere());
  EXPECT_FALSE(zero_rf_sphere->inverseFlattening().has_value());

  auto equal_two_axis = Ellipsoid::createTwoAxis(
      NamedProps("Equal Axes"), Length(6370997.0), Length(6370997.0));
  EXPECT_TRUE(equal_two_axis->isSphere());
  EXPECT_DOUBLE_EQ(0.0, equal_two_axis->computedInverseFlattening());
  ASSERT_TRUE(equal_two_axis->semiMinorAxis().has_value());

  // Biaxial ellipsoids (WGS84, GRS1980, CLARKE_1866).
  EXPECT_FALSE(Ellipsoid::WGS84->isSphere());
  EXPECT_GT(Ellipsoid::WGS84->squaredEccentricity(), 0.006);
  EXPECT_NEAR(6356752.314245179,
              Ellipsoid::WGS84->computeSemiMinorAxis().getSIValue(), 1e-6);
  EXPECT_FALSE(Ellipsoid::CLARKE_1866->isSphere());
  EXPECT_NEAR(294.9786982138982,
              Ellipsoid::CLARKE_1866->computedInverseFlattening(), 1e-9);

  // WKT export: unnamed, LENGTHUNIT omission vs non-METRE unit, outputId.
  auto unnamed_ellps =
      Ellipsoid::createSphere(PropertyMap(), Length(6371000.0));
  EXPECT_THAT(
      ToWkt(*unnamed_ellps, WKTFormatter::Convention::WKT2),
      HasSubstr("ELLIPSOID[\"unnamed\",6371000,0,LENGTHUNIT[\"metre\""));
  EXPECT_THAT(ToWkt(*Ellipsoid::WGS84, WKTFormatter::Convention::WKT2),
              HasSubstr("LENGTHUNIT[\"metre\",1]"));
  EXPECT_THAT(
      ToWkt(*Ellipsoid::WGS84, WKTFormatter::Convention::WKT2_SIMPLIFIED),
      Not(HasSubstr("LENGTHUNIT")));
  EXPECT_THAT(ToWkt(*Ellipsoid::WGS84, WKTFormatter::Convention::WKT2, db,
                    /*output_id=*/false),
              Not(HasSubstr(",ID[")));

  auto foot_ellps = Ellipsoid::createTwoAxis(
      NamedProps("Foot Ellps"), Length(20925646.0, UnitOfMeasure::FOOT),
      Length(20854895.0, UnitOfMeasure::FOOT));
  EXPECT_THAT(ToWkt(*foot_ellps, WKTFormatter::Convention::WKT2_SIMPLIFIED),
              HasSubstr("LENGTHUNIT[\"foot\",0.3048"));
  EXPECT_THAT(ToWkt(*foot_ellps, WKTFormatter::Convention::WKT1_GDAL),
              HasSubstr("SPHEROID[\"Foot Ellps\",6378136.9008,"));

  // WKT1_ESRI: WGS 84 hardcode, DB alias, ESRI authority lookup, fallback.
  EXPECT_THAT(ToWkt(*Ellipsoid::WGS84, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("SPHEROID[\"WGS_1984\","));
  auto airy = Ellipsoid::createFlattenedSphere(
      NamedProps("Airy 1830"), Length(6377563.396), Scale(299.3249646));
  EXPECT_THAT(ToWkt(*airy, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("SPHEROID[\"Airy_1830\","));
  auto intl_1967_esri = Ellipsoid::createTwoAxis(
      NamedProps("International_1967"), Length(6378160.0), Length(6356774.516));
  EXPECT_THAT(ToWkt(*intl_1967_esri, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("SPHEROID[\"International_1967\","));
  auto custom_ellps = Ellipsoid::createFlattenedSphere(
      NamedProps("Custom Ellps"), Length(6378100.0), Scale(299.5));
  EXPECT_THAT(ToWkt(*custom_ellps, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("SPHEROID[\"Custom_Ellps\","));

  // JSON export: unnamed, sphere, flattened, two-axis metre vs foot.
  EXPECT_THAT(ToJson(*unnamed_ellps), HasSubstr("\"name\":\"unnamed\""));
  EXPECT_THAT(ToJson(*unnamed_ellps), HasSubstr("\"radius\":6371000"));
  const std::string wgs84_json = ToJson(*Ellipsoid::WGS84);
  EXPECT_THAT(wgs84_json, HasSubstr("\"semi_major_axis\":6378137"));
  EXPECT_THAT(wgs84_json, HasSubstr("\"inverse_flattening\":298.257223563"));
  EXPECT_THAT(wgs84_json, HasSubstr("\"id\""));
  EXPECT_THAT(ToJson(*Ellipsoid::CLARKE_1866),
              HasSubstr("\"semi_minor_axis\":6356583.8"));
  const std::string foot_json = ToJson(*foot_ellps);
  EXPECT_THAT(foot_json, HasSubstr("\"semi_major_axis\":{\"value\":20925646"));
  EXPECT_THAT(foot_json, HasSubstr("\"semi_minor_axis\":{\"value\":20854895"));
  EXPECT_THAT(foot_json, HasSubstr("\"name\":\"foot\""));

  // lookForProjWellKnownEllps & PROJ string formatting & identify().
  std::string proj_name;
  std::string full_name;
  EXPECT_TRUE(
      Ellipsoid::CLARKE_1866->lookForProjWellKnownEllps(proj_name, full_name));
  EXPECT_EQ("clrk66", proj_name);
  EXPECT_EQ("+ellps=clrk66", ToProjString(*Ellipsoid::CLARKE_1866));

  EXPECT_TRUE(
      Ellipsoid::GRS1980->lookForProjWellKnownEllps(proj_name, full_name));
  EXPECT_EQ("GRS80", proj_name);
  EXPECT_EQ("GRS 1980", full_name);

  EXPECT_FALSE(custom_ellps->lookForProjWellKnownEllps(proj_name, full_name));
  auto custom_sphere =
      Ellipsoid::createSphere(NamedProps("Custom Sphere"), Length(6371234.0));
  EXPECT_EQ("+R=6371234", ToProjString(*custom_sphere));
  EXPECT_EQ("+a=6378100 +rf=299.5", ToProjString(*custom_ellps));
  auto custom_two_axis = Ellipsoid::createTwoAxis(
      NamedProps("Custom Two Axis"), Length(6378100.0), Length(6356000.0));
  EXPECT_EQ("+a=6378100 +b=6356000", ToProjString(*custom_two_axis));

  auto unnamed_normal_sphere =
      Ellipsoid::createSphere(PropertyMap(), Length(6370997.0));
  EXPECT_EQ("Normal Sphere (r=6370997)",
            unnamed_normal_sphere->identify()->nameStr());
  auto unknown_wgs84 = Ellipsoid::createFlattenedSphere(
      NamedProps("unknown"), Length(6378137.0), Scale(298.257223563));
  EXPECT_EQ("WGS 84", unknown_wgs84->identify()->nameStr());
  EXPECT_EQ("Custom Sphere", custom_sphere->identify()->nameStr());
  EXPECT_EQ("", unnamed_ellps->identify()->nameStr());

  // Equivalence checks: non-Ellipsoid, STRICT name/parameter checks,
  // lax "Clarke 1880 mod.", and computeSemiMinorAxis.
  EXPECT_FALSE(Ellipsoid::WGS84->isEquivalentTo(
      PrimeMeridian::GREENWICH.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(Ellipsoid::WGS84->isEquivalentTo(
      unknown_wgs84.get(), IComparable::Criterion::STRICT));

  auto clrk80_mod = Ellipsoid::createFlattenedSphere(
      NamedProps("Clarke 1880 mod."), Length(6378249.145), Scale(293.4663));
  auto clrk80_rgs = Ellipsoid::createFlattenedSphere(
      NamedProps("Clarke 1880 (RGS)"), Length(6378249.145), Scale(293.465));
  auto clrk80_bad_rf = Ellipsoid::createFlattenedSphere(
      NamedProps("Other"), Length(6378249.145), Scale(295.0));
  EXPECT_TRUE(clrk80_mod->isEquivalentTo(clrk80_rgs.get(),
                                         IComparable::Criterion::EQUIVALENT));
  EXPECT_TRUE(clrk80_rgs->isEquivalentTo(clrk80_mod.get(),
                                         IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(clrk80_mod->isEquivalentTo(clrk80_bad_rf.get(),
                                          IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(clrk80_mod->isEquivalentTo(Ellipsoid::WGS84.get(),
                                          IComparable::Criterion::EQUIVALENT));

  EXPECT_FALSE(Ellipsoid::WGS84->isEquivalentTo(
      custom_ellps.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(Ellipsoid::CLARKE_1866->isEquivalentTo(
      custom_two_axis.get(), IComparable::Criterion::EQUIVALENT));
  auto same_a_diff_b = Ellipsoid::createTwoAxis(
      NamedProps("Same a"), Length(6378206.4), Length(6356500.0));
  EXPECT_FALSE(Ellipsoid::CLARKE_1866->isEquivalentTo(
      same_a_diff_b.get(), IComparable::Criterion::EQUIVALENT));

  auto wgs84_diff_rf = Ellipsoid::createFlattenedSphere(
      NamedProps("WGS 84"), Length(6378137.0), Scale(298.25));
  EXPECT_FALSE(Ellipsoid::WGS84->isEquivalentTo(
      wgs84_diff_rf.get(), IComparable::Criterion::STRICT));

  auto wgs84_two_axis =
      Ellipsoid::createTwoAxis(NamedProps("WGS 84"), Length(6378137.0),
                               Ellipsoid::WGS84->computeSemiMinorAxis());
  EXPECT_FALSE(Ellipsoid::WGS84->isEquivalentTo(
      wgs84_two_axis.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(wgs84_two_axis->isEquivalentTo(Ellipsoid::WGS84.get(),
                                              IComparable::Criterion::STRICT));
  EXPECT_TRUE(Ellipsoid::WGS84->isEquivalentTo(
      wgs84_two_axis.get(), IComparable::Criterion::EQUIVALENT));

  auto wgs84_two_axis_diff = Ellipsoid::createTwoAxis(
      NamedProps("WGS 84"), Length(6378137.0), Length(6356000.0));
  EXPECT_FALSE(Ellipsoid::WGS84->isEquivalentTo(
      wgs84_two_axis_diff.get(), IComparable::Criterion::EQUIVALENT));

  auto sphere_same_a =
      Ellipsoid::createSphere(NamedProps("Same Name"), Length(6378137.0));
  auto flattened_same_name = Ellipsoid::createFlattenedSphere(
      NamedProps("Same Name"), Length(6378137.0), Scale(298.257223563));
  EXPECT_FALSE(sphere_same_a->isEquivalentTo(flattened_same_name.get(),
                                             IComparable::Criterion::STRICT));

  // guessBodyName across Earth, DB ellipsoid name, DB radius, unknown, nullptr.
  struct BodyGuessCase {
    bool with_db;
    double semi_major_metre;
    std::string_view ellipsoid_name;
    std::string_view expected_body;
  };
  constexpr BodyGuessCase kBodyCases[] = {
      {
          .with_db = true,
          .semi_major_metre = 6378137.0,
          .ellipsoid_name = "",
          .expected_body = "Earth",
      },
      {
          .with_db = true,
          .semi_major_metre = 3396190.0,
          .ellipsoid_name = "Mars (2015)",
          .expected_body = "Mars",
      },
      {
          .with_db = true,
          .semi_major_metre = 500000.0,
          .ellipsoid_name = "Mars (2015)",
          .expected_body = NON_EARTH_BODY,
      },
      {
          .with_db = true,
          .semi_major_metre = 1737400.0,
          .ellipsoid_name = "",
          .expected_body = "Moon",
      },
      {
          .with_db = true,
          .semi_major_metre = 123456.0,
          .ellipsoid_name = "Nonexistent Body Ellipsoid",
          .expected_body = NON_EARTH_BODY,
      },
      {
          .with_db = false,
          .semi_major_metre = 3396190.0,
          .ellipsoid_name = "Mars (2015)",
          .expected_body = NON_EARTH_BODY,
      },
  };
  for (const auto& c : kBodyCases) {
    EXPECT_EQ(c.expected_body,
              Ellipsoid::guessBodyName(c.with_db ? db.as_nullable() : nullptr,
                                       c.semi_major_metre,
                                       std::string(c.ellipsoid_name)));
  }
}

TEST(DatumTest, GeodeticReferenceFrameWktJsonAliasAndEquivalence) {
  ScopedContext ctx = MakeContext();
  DatabaseContextNNPtr db = MakeDbContext(ctx.get());

  EXPECT_EQ("North American Datum 1927",
            GeodeticReferenceFrame::EPSG_6267->nameStr());
  EXPECT_EQ("North American Datum 1983",
            GeodeticReferenceFrame::EPSG_6269->nameStr());
  EXPECT_EQ("World Geodetic System 1984",
            GeodeticReferenceFrame::EPSG_6326->nameStr());
  EXPECT_EQ(Ellipsoid::WGS84, GeodeticReferenceFrame::EPSG_6326->ellipsoid());
  EXPECT_EQ(PrimeMeridian::GREENWICH,
            GeodeticReferenceFrame::EPSG_6326->primeMeridian());

  // Top-level WKT export emits trailing PRIMEM when not Greenwich; wrapping in
  // GeographicCRS exercises !formatter->isAtTopLevel().
  auto unnamed_grf = GeodeticReferenceFrame::create(
      PropertyMap(), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::PARIS);
  const std::string unnamed_top_wkt =
      ToWkt(*unnamed_grf, WKTFormatter::Convention::WKT2);
  EXPECT_THAT(unnamed_top_wkt, HasSubstr("DATUM[\"unnamed\","));
  EXPECT_THAT(unnamed_top_wkt, HasSubstr(",PRIMEM[\"Paris\","));

  auto ntf_crs = GeographicCRS::create(
      NamedProps("NTF"), unnamed_grf,
      EllipsoidalCS::createLatitudeLongitude(UnitOfMeasure::DEGREE));
  EXPECT_THAT(ToWkt(*ntf_crs, WKTFormatter::Convention::WKT2),
              HasSubstr("GEODCRS[\"NTF\",DATUM[\"unnamed\""));
  EXPECT_THAT(ToJson(*unnamed_grf), HasSubstr("\"name\":\"unnamed\""));

  // WKT2 outputId(false) omits datum identifier; Greenwich omits prime_meridian
  // key in JSON.
  EXPECT_THAT(ToWkt(*GeodeticReferenceFrame::EPSG_6326,
                    WKTFormatter::Convention::WKT2, db, /*output_id=*/false),
              Not(HasSubstr(",ID[")));
  EXPECT_THAT(ToJson(*GeodeticReferenceFrame::EPSG_6326),
              Not(HasSubstr("\"prime_meridian\"")));

  // WKT1_GDAL: EPSG WGS84 -> WGS_1984, nameless-id EPSG DB name lookup, TOWGS84
  // parameters, and HDatum EXTENSION.
  {
    auto wkt1_fmt =
        WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL, db);
    wkt1_fmt->setTOWGS84Parameters(
        std::vector<double>{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0});
    wkt1_fmt->setHDatumExtension("custom_hgrid.gsb");
    const std::string wkt1 =
        GeodeticReferenceFrame::EPSG_6326->exportToWKT(wkt1_fmt.get());
    EXPECT_THAT(wkt1, HasSubstr("DATUM[\"WGS_1984\","));
    EXPECT_THAT(wkt1, HasSubstr("TOWGS84[1,2,3,4,5,6,7]"));
    EXPECT_THAT(wkt1,
                HasSubstr("EXTENSION[\"PROJ4_GRIDS\",\"custom_hgrid.gsb\"]"));
  }
  auto nad27_no_id = GeodeticReferenceFrame::create(
      NamedProps("North American Datum 1927"), Ellipsoid::CLARKE_1866,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_THAT(ToWkt(*nad27_no_id, WKTFormatter::Convention::WKT1_GDAL, db),
              HasSubstr("DATUM[\"North_American_Datum_1927\","));
  auto unlisted_grf = GeodeticReferenceFrame::create(
      NamedProps("Unlisted Datum Name"), Ellipsoid::WGS84,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_THAT(ToWkt(*unlisted_grf, WKTFormatter::Convention::WKT1_GDAL, db),
              HasSubstr("DATUM[\"Unlisted Datum Name\","));

  // WKT1_ESRI branches: WGS84, DB alias, prefix before " (", ESRI authority,
  // EPSG_OLD -> ETRS89 ensemble fallback, and D_ morphing.
  EXPECT_THAT(ToWkt(*GeodeticReferenceFrame::EPSG_6326,
                    WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("DATUM[\"D_WGS_1984\","));

  auto hungarian = GeodeticReferenceFrame::create(
      NamedProps("Hungarian Datum 1909"), Ellipsoid::GRS1980,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_THAT(ToWkt(*hungarian, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("DATUM[\"D_Hungarian_Datum_1909\","));

  auto padang_jakarta = GeodeticReferenceFrame::create(
      NamedProps("Padang 1884 (Jakarta)"), Ellipsoid::WGS84,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_THAT(ToWkt(*padang_jakarta, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("DATUM[\"D_Padang_1884\","));

  auto curacao_esri = GeodeticReferenceFrame::create(
      NamedProps("Curacao_1951"), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  EXPECT_THAT(ToWkt(*curacao_esri, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("DATUM[\"Curacao_1951\","));

  auto epsg_factory = AuthorityFactory::create(db, "EPSG");
  auto etrs_aut = epsg_factory->createGeodeticDatum("1431");
  EXPECT_THAT(ToWkt(*etrs_aut, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("DATUM[\"D_ETRS_1989\","));

  auto custom_no_d = GeodeticReferenceFrame::create(
      NamedProps("My Custom Datum"), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  EXPECT_THAT(ToWkt(*custom_no_d, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("DATUM[\"D_My_Custom_Datum\","));
  auto custom_with_d = GeodeticReferenceFrame::create(
      NamedProps("D_Already_Prefixed"), Ellipsoid::WGS84,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_THAT(ToWkt(*custom_with_d, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("DATUM[\"D_Already_Prefixed\","));

  // isEquivalentTo / hasEquivalentNameToUsingAlias branches.
  auto vrf =
      VerticalReferenceFrame::create(NamedProps("World Geodetic System 1984"));
  EXPECT_FALSE(GeodeticReferenceFrame::EPSG_6326->isEquivalentTo(
      vrf.get(), IComparable::Criterion::EQUIVALENT, db));
  EXPECT_FALSE(GeodeticReferenceFrame::EPSG_6326->isEquivalentTo(
      unnamed_grf.get(), IComparable::Criterion::EQUIVALENT, db));
  auto grf_grs80 = GeodeticReferenceFrame::create(
      NamedProps("World Geodetic System 1984"), Ellipsoid::GRS1980,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_FALSE(GeodeticReferenceFrame::EPSG_6326->isEquivalentTo(
      grf_grs80.get(), IComparable::Criterion::EQUIVALENT, db));

  auto dyn_wgs84 = DynamicGeodeticReferenceFrame::create(
      NamedProps("World Geodetic System 1984"), Ellipsoid::WGS84,
      optional<std::string>(), PrimeMeridian::GREENWICH,
      Measure(2015.0, UnitOfMeasure::YEAR), optional<std::string>());
  EXPECT_FALSE(GeodeticReferenceFrame::EPSG_6326->isEquivalentTo(
      dyn_wgs84.get(), IComparable::Criterion::STRICT, db));

  auto unknown_name = GeodeticReferenceFrame::create(
      NamedProps("unknown"), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  EXPECT_TRUE(
      unknown_name->isEquivalentTo(GeodeticReferenceFrame::EPSG_6326.get(),
                                   IComparable::Criterion::EQUIVALENT, db));
  EXPECT_TRUE(GeodeticReferenceFrame::EPSG_6326->isEquivalentTo(
      unknown_name.get(), IComparable::Criterion::EQUIVALENT, db));

  auto unknown_based_this = GeodeticReferenceFrame::create(
      NamedProps("Unknown based on WGS 84"), Ellipsoid::WGS84,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  auto unknown_based_other = GeodeticReferenceFrame::create(
      NamedProps("Unknown based on ITRF"), Ellipsoid::WGS84,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_FALSE(unknown_based_this->isEquivalentTo(
      GeodeticReferenceFrame::EPSG_6326.get(),
      IComparable::Criterion::EQUIVALENT, db));
  EXPECT_FALSE(GeodeticReferenceFrame::EPSG_6326->isEquivalentTo(
      unknown_based_other.get(), IComparable::Criterion::EQUIVALENT, db));

  // compareFromThisId (official name + alias list + non-matching name) on both
  // this and other.
  auto etrs_official_id = GeodeticReferenceFrame::create(
      EpsgNamedProps("European Terrestrial Reference System 1989 ensemble",
                     6258),
      Ellipsoid::GRS1980, optional<std::string>(), PrimeMeridian::GREENWICH);
  auto etrs_alias_id = GeodeticReferenceFrame::create(
      EpsgNamedProps("ETRS89", 6258), Ellipsoid::GRS1980,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  auto etrf_no_id = GeodeticReferenceFrame::create(
      NamedProps("ETRF89"), Ellipsoid::GRS1980, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  auto bogus_grs80 = GeodeticReferenceFrame::create(
      NamedProps("Not An ETRS Alias"), Ellipsoid::GRS1980,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_TRUE(etrs_official_id->isEquivalentTo(
      etrf_no_id.get(), IComparable::Criterion::EQUIVALENT, db));
  EXPECT_TRUE(etrs_alias_id->isEquivalentTo(
      etrf_no_id.get(), IComparable::Criterion::EQUIVALENT, db));
  EXPECT_TRUE(etrf_no_id->isEquivalentTo(
      etrs_official_id.get(), IComparable::Criterion::EQUIVALENT, db));
  EXPECT_FALSE(etrs_official_id->isEquivalentTo(
      bogus_grs80.get(), IComparable::Criterion::EQUIVALENT, db));

  // compareFromThisName (neither has IDs): both directions and miss.
  auto nad27_short = GeodeticReferenceFrame::create(
      NamedProps("NAD27"), Ellipsoid::CLARKE_1866, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  EXPECT_TRUE(nad27_no_id->isEquivalentTo(
      nad27_short.get(), IComparable::Criterion::EQUIVALENT, db));
  EXPECT_TRUE(nad27_short->isEquivalentTo(
      nad27_no_id.get(), IComparable::Criterion::EQUIVALENT, db));
  auto custom_clrk66 = GeodeticReferenceFrame::create(
      NamedProps("Completely Unrelated"), Ellipsoid::CLARKE_1866,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_FALSE(nad27_no_id->isEquivalentTo(
      custom_clrk66.get(), IComparable::Criterion::EQUIVALENT, db));

  // "D_" ESRI prefix equivalence rules.
  auto d_wgs84 = GeodeticReferenceFrame::create(
      NamedProps("D_WGS_1984"), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  auto wgs84_u = GeodeticReferenceFrame::create(
      NamedProps("WGS_1984"), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  auto d_wgs84_full = GeodeticReferenceFrame::create(
      NamedProps("D_World Geodetic System 1984"), Ellipsoid::WGS84,
      optional<std::string>(), PrimeMeridian::GREENWICH);
  EXPECT_TRUE(d_wgs84->isEquivalentTo(wgs84_u.get(),
                                      IComparable::Criterion::EQUIVALENT, db));
  EXPECT_TRUE(wgs84_u->isEquivalentTo(d_wgs84.get(),
                                      IComparable::Criterion::EQUIVALENT, db));
  EXPECT_TRUE(d_wgs84->isEquivalentTo(GeodeticReferenceFrame::EPSG_6326.get(),
                                      IComparable::Criterion::EQUIVALENT, db));
  EXPECT_TRUE(GeodeticReferenceFrame::EPSG_6326->isEquivalentTo(
      d_wgs84.get(), IComparable::Criterion::EQUIVALENT, db));
  EXPECT_TRUE(GeodeticReferenceFrame::EPSG_6326->isEquivalentTo(
      d_wgs84_full.get(), IComparable::Criterion::EQUIVALENT, db));
  auto bogus_wgs84 = GeodeticReferenceFrame::create(
      NamedProps("Not A WGS Alias"), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  EXPECT_FALSE(d_wgs84->isEquivalentTo(bogus_wgs84.get(),
                                       IComparable::Criterion::EQUIVALENT, db));
  EXPECT_FALSE(d_wgs84->isEquivalentTo(bogus_grs80.get(),
                                       IComparable::Criterion::EQUIVALENT, db));
}

TEST(DatumTest, DynamicGeodeticReferenceFrameWktJsonAndEquivalence) {
  auto dyn_with_model = DynamicGeodeticReferenceFrame::create(
      NamedProps("ITRF2014"), Ellipsoid::GRS1980,
      optional<std::string>("Station network"), PrimeMeridian::GREENWICH,
      Measure(2010.0, UnitOfMeasure::YEAR), optional<std::string>("NKG2016LU"));

  EXPECT_DOUBLE_EQ(2010.0, dyn_with_model->frameReferenceEpoch().convertToUnit(
                               UnitOfMeasure::YEAR));
  ASSERT_TRUE(dyn_with_model->deformationModelName().has_value());
  EXPECT_EQ("NKG2016LU", *dyn_with_model->deformationModelName());

  const std::string wkt2019 =
      ToWkt(*dyn_with_model, WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(wkt2019,
              HasSubstr("DYNAMIC[FRAMEEPOCH[2010],MODEL[\"NKG2016LU\"]]"));
  EXPECT_THAT(wkt2019, HasSubstr("DATUM[\"ITRF2014\","));
  const std::string dyn_wkt1 =
      ToWkt(*dyn_with_model, WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_THAT(dyn_wkt1, HasSubstr("DATUM[\"ITRF2014\","));
  EXPECT_THAT(dyn_wkt1, Not(HasSubstr("DYNAMIC")));

  auto dyn_no_model = DynamicGeodeticReferenceFrame::create(
      NamedProps("ITRF2014"), Ellipsoid::GRS1980, optional<std::string>(),
      PrimeMeridian::GREENWICH, Measure(2010.0, UnitOfMeasure::YEAR),
      optional<std::string>());
  const std::string wkt2019_no_model =
      ToWkt(*dyn_no_model, WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(wkt2019_no_model, HasSubstr("DYNAMIC[FRAMEEPOCH[2010]]"));
  EXPECT_THAT(wkt2019_no_model, Not(HasSubstr("MODEL")));

  EXPECT_THAT(ToWkt(*dyn_with_model, WKTFormatter::Convention::WKT2_2015),
              Not(HasSubstr("DYNAMIC")));

  const std::string json = ToJson(*dyn_with_model);
  EXPECT_THAT(json, HasSubstr("\"type\":\"DynamicGeodeticReferenceFrame\""));
  EXPECT_THAT(json, HasSubstr("\"frame_reference_epoch\":2010"));

  // Equivalence checks: STRICT vs static GRF, EQUIVALENT vs static GRF, epoch
  // and model mismatches.
  auto static_grf = GeodeticReferenceFrame::create(
      NamedProps("ITRF2014"), Ellipsoid::GRS1980, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  EXPECT_FALSE(dyn_with_model->isEquivalentTo(static_grf.get(),
                                              IComparable::Criterion::STRICT));
  EXPECT_TRUE(dyn_with_model->isEquivalentTo(
      static_grf.get(), IComparable::Criterion::EQUIVALENT));

  auto vrf = VerticalReferenceFrame::create(NamedProps("ITRF2014"));
  EXPECT_FALSE(dyn_with_model->isEquivalentTo(
      vrf.get(), IComparable::Criterion::EQUIVALENT));

  auto dyn_diff_epoch = DynamicGeodeticReferenceFrame::create(
      NamedProps("ITRF2014"), Ellipsoid::GRS1980, optional<std::string>(),
      PrimeMeridian::GREENWICH, Measure(2015.0, UnitOfMeasure::YEAR),
      optional<std::string>("NKG2016LU"));
  EXPECT_FALSE(dyn_with_model->isEquivalentTo(
      dyn_diff_epoch.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(dyn_with_model->isEquivalentTo(
      dyn_no_model.get(), IComparable::Criterion::EQUIVALENT));

  auto dyn_same = DynamicGeodeticReferenceFrame::create(
      NamedProps("ITRF2014"), Ellipsoid::GRS1980,
      optional<std::string>("Station network"), PrimeMeridian::GREENWICH,
      Measure(2010.0, UnitOfMeasure::YEAR), optional<std::string>("NKG2016LU"));
  EXPECT_TRUE(dyn_with_model->isEquivalentTo(dyn_same.get(),
                                             IComparable::Criterion::STRICT));
}

TEST(DatumTest, DatumEnsembleValidationAsDatumWktAndJson) {
  ScopedContext ctx = MakeContext();
  DatabaseContextNNPtr db = MakeDbContext(ctx.get());

  auto grf_a = GeodeticReferenceFrame::create(
      NamedProps("Member A"), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  auto grf_b = GeodeticReferenceFrame::create(
      NamedProps("Member B"), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  auto grf_grs80 = GeodeticReferenceFrame::create(
      NamedProps("Member GRS80"), Ellipsoid::GRS1980, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  auto grf_paris = GeodeticReferenceFrame::create(
      NamedProps("Member Paris"), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::PARIS);
  auto vrf_a = VerticalReferenceFrame::create(NamedProps("Vert A"));
  auto vrf_b = VerticalReferenceFrame::create(NamedProps("Vert B"));
  auto eng = EngineeringDatum::create(NamedProps("Eng A"));
  auto accuracy = PositionalAccuracy::create("2.0");

  // Validation error branches in DatumEnsemble::create().
  EXPECT_THROW(DatumEnsemble::create(NamedProps("Too Few"), {grf_a}, accuracy),
               Exception);
  EXPECT_THROW(
      DatumEnsemble::create(NamedProps("Mixed"), {grf_a, vrf_a}, accuracy),
      Exception);
  EXPECT_THROW(DatumEnsemble::create(NamedProps("Diff Ellps"),
                                     {grf_a, grf_grs80}, accuracy),
               Exception);
  EXPECT_THROW(DatumEnsemble::create(NamedProps("Diff PM"), {grf_a, grf_paris},
                                     accuracy),
               Exception);
  EXPECT_THROW(
      DatumEnsemble::create(NamedProps("Vert Mixed"), {vrf_a, eng}, accuracy),
      Exception);

  // ensembleNameToNonEnsembleName mappings.
  struct EnsembleNameCase {
    std::string_view input;
    std::string_view expected;
  };
  constexpr EnsembleNameCase kNameCases[] = {
      {.input = "World Geodetic System 1984 ensemble",
       .expected = "World Geodetic System 1984"},
      {.input = "European Terrestrial Reference System 1989 ensemble",
       .expected = "European Terrestrial Reference System 1989"},
      {.input = "Greenland Reference 1996 ensemble",
       .expected = "Greenland 1996"},
      {.input = "Custom Ensemble", .expected = ""},
  };
  for (const auto& c : kNameCases) {
    EXPECT_EQ(c.expected, DatumEnsemble::ensembleNameToNonEnsembleName(
                              std::string(c.input)));
  }

  // Authority-loaded geodetic ensemble (EPSG:6326).
  auto epsg_factory = AuthorityFactory::create(db, "EPSG");
  auto wgs84_ensemble = epsg_factory->createDatumEnsemble("6326");
  EXPECT_GE(wgs84_ensemble->datums().size(), 2u);
  EXPECT_FALSE(wgs84_ensemble->positionalAccuracy()->value().empty());

  const std::string wgs84_ens_wkt =
      ToWkt(*wgs84_ensemble, WKTFormatter::Convention::WKT2_2019, db);
  EXPECT_THAT(wgs84_ens_wkt,
              HasSubstr("ENSEMBLE[\"World Geodetic System 1984 ensemble\","));
  EXPECT_THAT(wgs84_ens_wkt, HasSubstr("MEMBER["));
  EXPECT_THAT(wgs84_ens_wkt, HasSubstr("ENSEMBLEACCURACY[2"));
  EXPECT_THAT(wgs84_ens_wkt, HasSubstr("ID[\"EPSG\",6326]"));
  EXPECT_THAT(ToWkt(*wgs84_ensemble, WKTFormatter::Convention::WKT2_2019, db,
                    /*output_id=*/false),
              Not(HasSubstr(",ID[")));
  // WKT2_2015 / WKT1_GDAL delegates via asDatum(db); nullptr db uses
  // ensembleNameToNonEnsembleName fallback while preserving identifiers.
  EXPECT_THAT(ToWkt(*wgs84_ensemble, WKTFormatter::Convention::WKT2_2015, db),
              HasSubstr("DATUM[\"World Geodetic System 1984"));
  EXPECT_THAT(ToWkt(*wgs84_ensemble, WKTFormatter::Convention::WKT1_GDAL, db),
              HasSubstr("DATUM[\"WGS_1984\""));
  DatumNNPtr no_db_geod = wgs84_ensemble->asDatum(nullptr);
  EXPECT_EQ("World Geodetic System 1984", no_db_geod->nameStr());

  // Custom geodetic ensemble with bogus DB code triggers catch -> manual
  // fallback copying deprecated flag, identifiers, and domain usages.
  auto usages = ArrayOfBaseObject::create();
  usages->add(
      ObjectDomain::create(optional<std::string>("Geodesy"), Extent::WORLD));
  auto custom_geod_ens = DatumEnsemble::create(
      EpsgNamedProps("World Geodetic System 1984 ensemble", 999999)
          .set(IdentifiedObject::DEPRECATED_KEY, true)
          .set(ObjectUsage::OBJECT_DOMAIN_KEY,
               nn_static_pointer_cast<BaseObject>(usages)),
      {grf_a, grf_b}, accuracy);
  DatumNNPtr fallback_geod = custom_geod_ens->asDatum(db);
  EXPECT_EQ("World Geodetic System 1984", fallback_geod->nameStr());
  EXPECT_TRUE(fallback_geod->isDeprecated());
  ASSERT_EQ(1u, fallback_geod->identifiers().size());
  EXPECT_EQ("999999", fallback_geod->identifiers().front()->code());
  EXPECT_EQ(1u, fallback_geod->domains().size());

  // Unnamed geodetic ensemble & unnamed members export "unnamed" in WKT2/JSON.
  auto grf_unnamed = GeodeticReferenceFrame::create(
      PropertyMap(), Ellipsoid::WGS84, optional<std::string>(),
      PrimeMeridian::GREENWICH);
  auto unnamed_ens =
      DatumEnsemble::create(PropertyMap(), {grf_unnamed, grf_b}, accuracy);
  EXPECT_THAT(ToWkt(*unnamed_ens, WKTFormatter::Convention::WKT2_2019),
              HasSubstr("ENSEMBLE[\"unnamed\",MEMBER[\"unnamed\"],"));
  const std::string unnamed_json = ToJson(*unnamed_ens);
  EXPECT_THAT(unnamed_json, HasSubstr("\"name\":\"unnamed\""));
  EXPECT_THAT(unnamed_json, HasSubstr("\"ellipsoid\":{"));

  // Vertical ensemble from DB (EPSG:1288) and without DB fallback.
  auto vert_db_ens = epsg_factory->createDatumEnsemble("1288");
  EXPECT_EQ("British Isles height ensemble",
            vert_db_ens->asDatum(db)->nameStr());
  const std::string vert_wkt =
      ToWkt(*vert_db_ens, WKTFormatter::Convention::WKT2_2019, db);
  EXPECT_THAT(vert_wkt,
              HasSubstr("ENSEMBLE[\"British Isles height ensemble\","));
  EXPECT_THAT(vert_wkt, Not(HasSubstr("ELLIPSOID")));

  auto custom_vert_ens =
      DatumEnsemble::create(NamedProps("Vert Ens"), {vrf_a, vrf_b}, accuracy);
  EXPECT_EQ("Vert Ens", custom_vert_ens->asDatum(nullptr)->nameStr());
  const std::string custom_vert_json = ToJson(*custom_vert_ens);
  EXPECT_THAT(custom_vert_json, HasSubstr("\"accuracy\":\"2.0\""));
  EXPECT_THAT(custom_vert_json, Not(HasSubstr("\"ellipsoid\"")));
}

TEST(DatumTest, VerticalAndDynamicVerticalReferenceFrameCoverage) {
  ScopedContext ctx = MakeContext();
  DatabaseContextNNPtr db = MakeDbContext(ctx.get());

  EXPECT_EQ("geoid", RealizationMethod::GEOID.toString());
  EXPECT_EQ("tidal", RealizationMethod::TIDAL.toString());
  EXPECT_FALSE(RealizationMethod::LEVELLING.toString().empty());
  EXPECT_NE(RealizationMethod::LEVELLING, RealizationMethod::GEOID);

  // VerticalReferenceFrame 3-arg and 4-arg creation, VERT_DATUM_TYPE,
  // anchor/anchorEpoch, and WKT/JSON exports.
  auto vrf = VerticalReferenceFrame::create(
      NamedProps("Test VRF")
          .set("VERT_DATUM_TYPE", "2005")
          .set(Identifier::CODESPACE_KEY, Identifier::EPSG)
          .set(Identifier::CODE_KEY, 99997),
      optional<std::string>("Zero Gauge"),
      optional<Measure>(Measure(2020.25, UnitOfMeasure::YEAR)),
      optional<RealizationMethod>(RealizationMethod::GEOID));
  EXPECT_EQ("2005", vrf->getWKT1DatumType());
  ASSERT_TRUE(vrf->realizationMethod().has_value());
  EXPECT_EQ(RealizationMethod::GEOID, *vrf->realizationMethod());

  const std::string vrf_wkt2019 =
      ToWkt(*vrf, WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(vrf_wkt2019,
              HasSubstr("VDATUM[\"Test VRF\",ANCHOR[\"Zero Gauge\"],"
                        "ANCHOREPOCH[2020.25],ID[\"EPSG\",99997]]"));
  EXPECT_THAT(ToWkt(*vrf, WKTFormatter::Convention::WKT2_2015, db,
                    /*output_id=*/false),
              Not(HasSubstr("ANCHOREPOCH")));

  {
    auto wkt1_fmt =
        WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL, db);
    wkt1_fmt->setVDatumExtension("egm08.gtx");
    const std::string vrf_wkt1 = vrf->exportToWKT(wkt1_fmt.get());
    EXPECT_THAT(vrf_wkt1, HasSubstr("VERT_DATUM[\"Test VRF\",2005,"));
    EXPECT_THAT(vrf_wkt1,
                HasSubstr("EXTENSION[\"PROJ4_GRIDS\",\"egm08.gtx\"]"));
  }

  // WKT1_ESRI alias, ESRI authority lookup, fallback, and unnamed.
  auto egm2008 = VerticalReferenceFrame::create(NamedProps("EGM2008 geoid"));
  EXPECT_THAT(ToWkt(*egm2008, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("VDATUM[\"EGM2008_Geoid\"]"));
  auto wgs84_geoid_esri =
      VerticalReferenceFrame::create(NamedProps("WGS_1984_Geoid"));
  EXPECT_THAT(ToWkt(*wgs84_geoid_esri, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("VDATUM[\"WGS_1984_Geoid\"]"));
  auto custom_vrf = VerticalReferenceFrame::create(NamedProps("My Vert Datum"));
  EXPECT_THAT(ToWkt(*custom_vrf, WKTFormatter::Convention::WKT1_ESRI, db),
              HasSubstr("VDATUM[\"My_Vert_Datum\"]"));
  auto unnamed_vrf = VerticalReferenceFrame::create(PropertyMap());
  EXPECT_THAT(ToWkt(*unnamed_vrf, WKTFormatter::Convention::WKT2),
              HasSubstr("VDATUM[\"unnamed\"]"));

  EXPECT_THAT(ToJson(*vrf), HasSubstr("\"type\":\"VerticalReferenceFrame\""));
  EXPECT_THAT(ToJson(*unnamed_vrf), HasSubstr("\"name\":\"unnamed\""));

  // Equivalence checks on VerticalReferenceFrame.
  EXPECT_FALSE(vrf->isEquivalentTo(GeodeticReferenceFrame::EPSG_6326.get(),
                                   IComparable::Criterion::EQUIVALENT));
  auto vrf_no_method = VerticalReferenceFrame::create(NamedProps("Test VRF"));
  EXPECT_FALSE(vrf->isEquivalentTo(vrf_no_method.get(),
                                   IComparable::Criterion::EQUIVALENT));
  auto vrf_tidal = VerticalReferenceFrame::create(
      NamedProps("Test VRF"), optional<std::string>(),
      optional<RealizationMethod>(RealizationMethod::TIDAL));
  EXPECT_FALSE(
      vrf->isEquivalentTo(vrf_tidal.get(), IComparable::Criterion::EQUIVALENT));
  auto vrf_geoid_plain = VerticalReferenceFrame::create(
      NamedProps("Test VRF"), optional<std::string>(),
      optional<RealizationMethod>(RealizationMethod::GEOID));
  EXPECT_TRUE(vrf->isEquivalentTo(vrf_geoid_plain.get(),
                                  IComparable::Criterion::EQUIVALENT));

  // DynamicVerticalReferenceFrame.
  auto dyn_vrf = DynamicVerticalReferenceFrame::create(
      NamedProps("RH2000"), optional<std::string>("NAP"),
      optional<RealizationMethod>(RealizationMethod::LEVELLING),
      Measure(2000.0, UnitOfMeasure::YEAR), optional<std::string>("NKG2016LU"));
  EXPECT_DOUBLE_EQ(2000.0, dyn_vrf->frameReferenceEpoch().convertToUnit(
                               UnitOfMeasure::YEAR));
  ASSERT_TRUE(dyn_vrf->deformationModelName().has_value());
  EXPECT_EQ("NKG2016LU", *dyn_vrf->deformationModelName());

  const std::string dyn_vrf_wkt =
      ToWkt(*dyn_vrf, WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(dyn_vrf_wkt,
              HasSubstr("DYNAMIC[FRAMEEPOCH[2000],MODEL[\"NKG2016LU\"]]"));
  EXPECT_THAT(dyn_vrf_wkt, HasSubstr("VDATUM[\"RH2000\",ANCHOR[\"NAP\"]]"));
  EXPECT_THAT(ToWkt(*dyn_vrf, WKTFormatter::Convention::WKT2_2015),
              Not(HasSubstr("DYNAMIC")));
  EXPECT_THAT(ToWkt(*dyn_vrf, WKTFormatter::Convention::WKT1_GDAL),
              HasSubstr("VERT_DATUM[\"RH2000\",2005]"));

  auto dyn_vrf_no_model = DynamicVerticalReferenceFrame::create(
      NamedProps("RH2000"), optional<std::string>(),
      optional<RealizationMethod>(RealizationMethod::LEVELLING),
      Measure(2000.0, UnitOfMeasure::YEAR), optional<std::string>());
  EXPECT_THAT(ToWkt(*dyn_vrf_no_model, WKTFormatter::Convention::WKT2_2019),
              Not(HasSubstr("MODEL")));

  const std::string dyn_vrf_json = ToJson(*dyn_vrf);
  EXPECT_THAT(dyn_vrf_json,
              HasSubstr("\"type\":\"DynamicVerticalReferenceFrame\""));
  EXPECT_THAT(dyn_vrf_json, HasSubstr("\"frame_reference_epoch\":2000"));

  auto static_vrf_levelling = VerticalReferenceFrame::create(
      NamedProps("RH2000"), optional<std::string>(),
      optional<RealizationMethod>(RealizationMethod::LEVELLING));
  EXPECT_FALSE(vrf_geoid_plain->isEquivalentTo(dyn_vrf.get(),
                                               IComparable::Criterion::STRICT));
  EXPECT_FALSE(dyn_vrf->isEquivalentTo(static_vrf_levelling.get(),
                                       IComparable::Criterion::STRICT));
  EXPECT_TRUE(dyn_vrf->isEquivalentTo(static_vrf_levelling.get(),
                                      IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(dyn_vrf->isEquivalentTo(GeodeticReferenceFrame::EPSG_6326.get(),
                                       IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(dyn_vrf->isEquivalentTo(dyn_vrf_no_model.get(),
                                       IComparable::Criterion::EQUIVALENT));
  auto dyn_vrf_same = DynamicVerticalReferenceFrame::create(
      NamedProps("RH2000"), optional<std::string>("NAP"),
      optional<RealizationMethod>(RealizationMethod::LEVELLING),
      Measure(2000.0, UnitOfMeasure::YEAR), optional<std::string>("NKG2016LU"));
  EXPECT_TRUE(dyn_vrf->isEquivalentTo(dyn_vrf_same.get(),
                                      IComparable::Criterion::STRICT));
}

TEST(DatumTest, TemporalEngineeringAndParametricDatumCoverage) {
  // TemporalDatum: ISO-8601 origin, non-ISO-8601 origin, empty origin.
  auto td_iso = TemporalDatum::create(
      NamedProps("Unix Epoch"), DateTime::create("1970-01-01T00:00:00Z"),
      TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN);
  EXPECT_EQ("1970-01-01T00:00:00Z", td_iso->temporalOrigin().toString());
  EXPECT_EQ("proleptic Gregorian", td_iso->calendar());

  EXPECT_THROW(ToWkt(*td_iso, WKTFormatter::Convention::WKT1_GDAL),
               FormattingException);
  EXPECT_EQ(
      "TDATUM[\"Unix Epoch\",CALENDAR[\"proleptic Gregorian\"],"
      "TIMEORIGIN[1970-01-01T00:00:00Z]]",
      ToWkt(*td_iso, WKTFormatter::Convention::WKT2_2019));
  EXPECT_EQ("TDATUM[\"Unix Epoch\",TIMEORIGIN[1970-01-01T00:00:00Z]]",
            ToWkt(*td_iso, WKTFormatter::Convention::WKT2_2015));

  auto td_non_iso = TemporalDatum::create(
      NamedProps("Custom Epoch"), DateTime::create("arbitrary time origin"),
      TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN);
  EXPECT_EQ("TDATUM[\"Custom Epoch\",TIMEORIGIN[\"arbitrary time origin\"]]",
            ToWkt(*td_non_iso, WKTFormatter::Convention::WKT2_2015));

  auto td_empty_origin =
      TemporalDatum::create(NamedProps("No Origin"), DateTime::create(""),
                            TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN);
  EXPECT_EQ("TDATUM[\"No Origin\"]",
            ToWkt(*td_empty_origin, WKTFormatter::Convention::WKT2_2015));

  EXPECT_THAT(ToJson(*td_iso),
              HasSubstr("\"time_origin\":\"1970-01-01T00:00:00Z\""));
  EXPECT_THAT(ToJson(*td_empty_origin), Not(HasSubstr("time_origin")));

  EXPECT_FALSE(td_iso->isEquivalentTo(GeodeticReferenceFrame::EPSG_6326.get(),
                                      IComparable::Criterion::STRICT));
  auto td_diff_cal =
      TemporalDatum::create(NamedProps("Unix Epoch"),
                            DateTime::create("1970-01-01T00:00:00Z"), "Julian");
  EXPECT_FALSE(td_iso->isEquivalentTo(td_diff_cal.get(),
                                      IComparable::Criterion::STRICT));
  auto td_same = TemporalDatum::create(
      NamedProps("Unix Epoch"), DateTime::create("1970-01-01T00:00:00Z"),
      TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN);
  EXPECT_TRUE(
      td_iso->isEquivalentTo(td_same.get(), IComparable::Criterion::STRICT));

  // EngineeringDatum: WKT2, WKT1_GDAL LOCAL_DATUM, JSON, equivalence.
  auto eng = EngineeringDatum::create(NamedProps("Site Grid"),
                                      optional<std::string>("Origin Pier"));
  EXPECT_EQ("EDATUM[\"Site Grid\",ANCHOR[\"Origin Pier\"]]",
            ToWkt(*eng, WKTFormatter::Convention::WKT2));
  EXPECT_EQ("LOCAL_DATUM[\"Site Grid\",32767]",
            ToWkt(*eng, WKTFormatter::Convention::WKT1_GDAL));
  EXPECT_THAT(ToJson(*eng), HasSubstr("\"type\":\"EngineeringDatum\""));

  EXPECT_FALSE(
      eng->isEquivalentTo(td_iso.get(), IComparable::Criterion::EQUIVALENT));
  auto eng_empty = EngineeringDatum::create(PropertyMap());
  auto eng_unknown =
      EngineeringDatum::create(NamedProps(UNKNOWN_ENGINEERING_DATUM));
  EXPECT_TRUE(eng_empty->isEquivalentTo(eng_unknown.get(),
                                        IComparable::Criterion::EQUIVALENT));
  EXPECT_TRUE(eng_unknown->isEquivalentTo(eng_empty.get(),
                                          IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(eng->isEquivalentTo(eng_unknown.get(),
                                   IComparable::Criterion::EQUIVALENT));
  auto eng_same = EngineeringDatum::create(
      NamedProps("Site Grid"), optional<std::string>("Origin Pier"));
  EXPECT_TRUE(
      eng->isEquivalentTo(eng_same.get(), IComparable::Criterion::STRICT));

  // ParametricDatum: WKT1 throws, WKT2, JSON, equivalence.
  auto param = ParametricDatum::create(NamedProps("Mean Sea Surface Pressure"),
                                       optional<std::string>("1013.25 hPa"));
  EXPECT_THROW(ToWkt(*param, WKTFormatter::Convention::WKT1_GDAL),
               FormattingException);
  EXPECT_EQ("PDATUM[\"Mean Sea Surface Pressure\",ANCHOR[\"1013.25 hPa\"]]",
            ToWkt(*param, WKTFormatter::Convention::WKT2));
  EXPECT_THAT(ToJson(*param), HasSubstr("\"type\":\"ParametricDatum\""));

  EXPECT_FALSE(
      param->isEquivalentTo(eng.get(), IComparable::Criterion::STRICT));
  auto param_other = ParametricDatum::create(NamedProps("Other Surface"));
  EXPECT_FALSE(
      param->isEquivalentTo(param_other.get(), IComparable::Criterion::STRICT));
  auto param_same =
      ParametricDatum::create(NamedProps("Mean Sea Surface Pressure"),
                              optional<std::string>("1013.25 hPa"));
  EXPECT_TRUE(
      param->isEquivalentTo(param_same.get(), IComparable::Criterion::STRICT));
}

TEST(DatumTest, PublicCApiDatumAndEnsembleRoundTrip) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor quiet(ctx.get());

  ScopedPj crs_4326(proj_create(ctx.get(), "EPSG:4326"));
  ASSERT_THAT(crs_4326.get(), NotNull());
  EXPECT_EQ(0, proj_errno(crs_4326.get()));

  ScopedPj ens(proj_crs_get_datum_ensemble(ctx.get(), crs_4326.get()));
  ASSERT_THAT(ens.get(), NotNull());
  EXPECT_GE(proj_datum_ensemble_get_member_count(ctx.get(), ens.get()), 2);
  EXPECT_GT(proj_datum_ensemble_get_accuracy(ctx.get(), ens.get()), 0.0);
  ScopedPj member(proj_datum_ensemble_get_member(ctx.get(), ens.get(), 0));
  ASSERT_THAT(member.get(), NotNull());

  ScopedPj forced_datum(proj_crs_get_datum_forced(ctx.get(), crs_4326.get()));
  ASSERT_THAT(forced_datum.get(), NotNull());

  ScopedPj ellps(proj_get_ellipsoid(ctx.get(), crs_4326.get()));
  ASSERT_THAT(ellps.get(), NotNull());
  double semi_major = 0.0;
  double semi_minor = 0.0;
  int is_computed = 0;
  double inv_flattening = 0.0;
  EXPECT_EQ(1, proj_ellipsoid_get_parameters(ctx.get(), ellps.get(),
                                             &semi_major, &semi_minor,
                                             &is_computed, &inv_flattening));
  EXPECT_DOUBLE_EQ(6378137.0, semi_major);
  EXPECT_NEAR(298.257223563, inv_flattening, 1e-9);
  EXPECT_EQ(1, is_computed);

  ScopedPj pm(proj_get_prime_meridian(ctx.get(), crs_4326.get()));
  ASSERT_THAT(pm.get(), NotNull());
  double pm_lon = -1.0;
  EXPECT_EQ(1, proj_prime_meridian_get_parameters(ctx.get(), pm.get(), &pm_lon,
                                                  nullptr, nullptr));
  EXPECT_DOUBLE_EQ(0.0, pm_lon);
}

}  // namespace
}  // namespace proj
