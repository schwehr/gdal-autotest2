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
// Original C++20 GoogleTest unit tests covering OSGeo::proj::io (WKTFormatter,
// WKTNode, WKTParser, JSONFormatter, JSONParser, PROJStringFormatter,
// PROJStringParser, and createFromUserInput) in proj/src/iso19111/io.cpp.
// Note: in google3 default non-GCE builds, proj.db is embedded via memvfs.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/io.cpp

#include "third_party/proj4/proj/include/proj/io.hpp"

#include <cmath>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinateoperation.hpp"
#include "third_party/proj4/proj/include/proj/coordinates.hpp"
#include "third_party/proj4/proj/include/proj/coordinatesystem.hpp"
#include "third_party/proj4/proj/include/proj/crs.hpp"
#include "third_party/proj4/proj/include/proj/datum.hpp"
#include "third_party/proj4/proj/include/proj/internal/io_internal.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/iso19111/operation/coordinateoperation_internal.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"
#include "third_party/proj4/proj/src/proj_json_streaming_writer.hpp"

namespace proj {
namespace {

namespace pj_common = ::osgeo::proj::common;
namespace pj_coord = ::osgeo::proj::coordinates;
namespace pj_crs = ::osgeo::proj::crs;
namespace pj_cs = ::osgeo::proj::cs;
namespace pj_datum = ::osgeo::proj::datum;
namespace pj_io = ::osgeo::proj::io;
namespace pj_meta = ::osgeo::proj::metadata;
namespace pj_op = ::osgeo::proj::operation;
namespace pj_util = ::osgeo::proj::util;

using ::testing::HasSubstr;
using ::testing::Not;

pj_io::DatabaseContextNNPtr MakeDbContext(PJ_CONTEXT* ctx) {
  return pj_io::DatabaseContext::create(std::string(), {}, ctx);
}

pj_util::PropertyMap NameProps(const std::string& name) {
  return pj_util::PropertyMap().set(pj_common::IdentifiedObject::NAME_KEY,
                                    name);
}

pj_crs::GeographicCRSNNPtr MakeCustomGeo2D(
    const std::string& crs_name, const std::string& datum_name,
    const pj_datum::EllipsoidNNPtr& ell,
    const pj_datum::PrimeMeridianNNPtr& pm) {
  auto datum = pj_datum::GeodeticReferenceFrame::create(
      NameProps(datum_name), ell,
      pj_util::optional<std::string>("Custom anchor"),
      pj_util::optional<pj_common::Measure>(
          pj_common::Measure(2010.5, pj_common::UnitOfMeasure::YEAR)),
      pm);
  return pj_crs::GeographicCRS::create(
      NameProps(crs_name), datum,
      pj_cs::EllipsoidalCS::createLatitudeLongitude(
          pj_common::UnitOfMeasure::DEGREE));
}

pj_crs::ProjectedCRSNNPtr MakeUtmProjectedCRS(
    const pj_crs::GeographicCRSNNPtr& base, int zone, bool north) {
  auto conv = pj_op::Conversion::createUTM(NameProps("UTM zone"), zone, north);
  return pj_crs::ProjectedCRS::create(NameProps("Test UTM"), base, conv,
                                      pj_cs::CartesianCS::createEastingNorthing(
                                          pj_common::UnitOfMeasure::METRE));
}

// =============================================================================
// Suite 1: WKTFormatterTest
// =============================================================================

TEST(WKTFormatterTest, ConventionsConfigurationAndCloning) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  auto f_wkt2 = pj_io::WKTFormatter::create(
      pj_io::WKTFormatter::Convention::WKT2_2015, db.as_nullable());
  EXPECT_EQ(f_wkt2->version(), pj_io::WKTFormatter::Version::WKT2);
  EXPECT_FALSE(f_wkt2->use2019Keywords());
  EXPECT_FALSE(f_wkt2->useESRIDialect());
  EXPECT_EQ(f_wkt2->outputAxis(), pj_io::WKTFormatter::OutputAxisRule::YES);
  EXPECT_TRUE(f_wkt2->outputAxisOrder());
  EXPECT_FALSE(f_wkt2->primeMeridianOmittedIfGreenwich());
  EXPECT_FALSE(f_wkt2->ellipsoidUnitOmittedIfMetre());
  EXPECT_FALSE(f_wkt2->forceUNITKeyword());
  EXPECT_FALSE(f_wkt2->primeMeridianOrParameterUnitOmittedIfSameAsAxis());
  EXPECT_FALSE(f_wkt2->primeMeridianInDegree());
  EXPECT_FALSE(f_wkt2->outputCSUnitOnlyOnceIfSame());
  EXPECT_FALSE(f_wkt2->idOnTopLevelOnly());
  EXPECT_EQ(f_wkt2->databaseContext(), db.as_nullable());
  EXPECT_TRUE(f_wkt2->isStrict());
  EXPECT_FALSE(f_wkt2->isAllowedEllipsoidalHeightAsVerticalCRS());
  EXPECT_FALSE(f_wkt2->isAllowedLINUNITNode());
  EXPECT_TRUE(f_wkt2->isAtTopLevel());

  f_wkt2->setMultiLine(false)
      .setIndentationWidth(2)
      .setOutputAxis(pj_io::WKTFormatter::OutputAxisRule::NO)
      .setStrict(false)
      .setAllowEllipsoidalHeightAsVerticalCRS(true)
      .setAllowLINUNITNode(true);
  EXPECT_EQ(f_wkt2->outputAxis(), pj_io::WKTFormatter::OutputAxisRule::NO);
  EXPECT_FALSE(f_wkt2->isStrict());
  EXPECT_TRUE(f_wkt2->isAllowedEllipsoidalHeightAsVerticalCRS());
  EXPECT_TRUE(f_wkt2->isAllowedLINUNITNode());

  f_wkt2->setOutputId(false);
  EXPECT_FALSE(f_wkt2->outputId());
  f_wkt2->setAbridgedTransformation(true);
  EXPECT_TRUE(f_wkt2->abridgedTransformation());
  f_wkt2->setUseDerivingConversion(true);
  EXPECT_TRUE(f_wkt2->useDerivingConversion());
  f_wkt2->setTOWGS84Parameters({1.0, 2.0, 3.0});
  EXPECT_EQ(f_wkt2->getTOWGS84Parameters(),
            (std::vector<double>{1.0, 2.0, 3.0}));
  f_wkt2->setVDatumExtension("vgrid.gtx");
  EXPECT_EQ(f_wkt2->getVDatumExtension(), "vgrid.gtx");
  f_wkt2->setHDatumExtension("hgrid.gsb");
  EXPECT_EQ(f_wkt2->getHDatumExtension(), "hgrid.gsb");
  f_wkt2->setGeogCRSOfCompoundCRS(pj_crs::GeographicCRS::EPSG_4326);
  EXPECT_EQ(f_wkt2->getGeogCRSOfCompoundCRS(),
            pj_crs::GeographicCRS::EPSG_4326);

  // Clone preserves configuration and database context.
  auto cloned = pj_io::WKTFormatter::create(f_wkt2);
  EXPECT_EQ(cloned->version(), pj_io::WKTFormatter::Version::WKT2);
  EXPECT_FALSE(cloned->isStrict());
  EXPECT_TRUE(cloned->isAllowedEllipsoidalHeightAsVerticalCRS());
  EXPECT_TRUE(cloned->isAllowedLINUNITNode());
  EXPECT_EQ(cloned->databaseContext(), db.as_nullable());

  auto f_wkt2_s = pj_io::WKTFormatter::create(
      pj_io::WKTFormatter::Convention::WKT2_2015_SIMPLIFIED);
  EXPECT_FALSE(f_wkt2_s->use2019Keywords());
  EXPECT_TRUE(f_wkt2_s->idOnTopLevelOnly());
  EXPECT_FALSE(f_wkt2_s->outputAxisOrder());
  EXPECT_TRUE(f_wkt2_s->primeMeridianOmittedIfGreenwich());
  EXPECT_TRUE(f_wkt2_s->ellipsoidUnitOmittedIfMetre());
  EXPECT_TRUE(f_wkt2_s->forceUNITKeyword());
  EXPECT_TRUE(f_wkt2_s->primeMeridianOrParameterUnitOmittedIfSameAsAxis());
  EXPECT_TRUE(f_wkt2_s->outputCSUnitOnlyOnceIfSame());

  auto f_2019 =
      pj_io::WKTFormatter::create(pj_io::WKTFormatter::Convention::WKT2_2019);
  EXPECT_TRUE(f_2019->use2019Keywords());
  EXPECT_FALSE(f_2019->idOnTopLevelOnly());

  auto f_2019_s = pj_io::WKTFormatter::create(
      pj_io::WKTFormatter::Convention::WKT2_2019_SIMPLIFIED);
  EXPECT_TRUE(f_2019_s->use2019Keywords());
  EXPECT_TRUE(f_2019_s->idOnTopLevelOnly());

  auto f_gdal =
      pj_io::WKTFormatter::create(pj_io::WKTFormatter::Convention::WKT1_GDAL);
  EXPECT_EQ(f_gdal->version(), pj_io::WKTFormatter::Version::WKT1);
  EXPECT_FALSE(f_gdal->useESRIDialect());
  EXPECT_TRUE(f_gdal->primeMeridianInDegree());
  EXPECT_EQ(f_gdal->outputAxis(),
            pj_io::WKTFormatter::OutputAxisRule::WKT1_GDAL_EPSG_STYLE);

  auto f_esri =
      pj_io::WKTFormatter::create(pj_io::WKTFormatter::Convention::WKT1_ESRI);
  EXPECT_EQ(f_esri->version(), pj_io::WKTFormatter::Version::WKT1);
  EXPECT_TRUE(f_esri->useESRIDialect());
  EXPECT_EQ(f_esri->outputAxis(), pj_io::WKTFormatter::OutputAxisRule::NO);
  EXPECT_TRUE(f_esri->isAllowedLINUNITNode());
}

TEST(WKTFormatterTest, ManualNodeFormattingAndExceptions) {
  auto f =
      pj_io::WKTFormatter::create(pj_io::WKTFormatter::Convention::WKT2_2019);
  f->setMultiLine(true).setIndentationWidth(2);
  f->enter();
  EXPECT_FALSE(f->isAtTopLevel());
  f->startNode("TESTNODE", false);
  f->simulCurNodeHasId();
  EXPECT_TRUE(f->topLevelHasId());
  EXPECT_THROW(f->setOutputId(false), pj_util::Exception);
  f->addQuotedString("hello \"world\"");
  f->add("UNQUOTED");
  f->add(42);
  f->add(1.25, 4);
  f->startNode("SUB", false);
  f->pushOutputUnit(false);
  EXPECT_FALSE(f->outputUnit());
  f->popOutputUnit();
  f->pushDisableUsage();
  EXPECT_FALSE(f->outputUsage());
  f->popDisableUsage();
  f->pushHasId(true);
  f->popHasId();
  f->pushAxisLinearUnit(pj_util::nn_make_shared<pj_common::UnitOfMeasure>(
      pj_common::UnitOfMeasure::FOOT));
  EXPECT_EQ(f->axisLinearUnit()->name(), pj_common::UnitOfMeasure::FOOT.name());
  f->popAxisLinearUnit();
  f->pushAxisAngularUnit(pj_util::nn_make_shared<pj_common::UnitOfMeasure>(
      pj_common::UnitOfMeasure::GRAD));
  EXPECT_EQ(f->axisAngularUnit()->name(),
            pj_common::UnitOfMeasure::GRAD.name());
  f->popAxisAngularUnit();
  f->endNode();
  f->endNode();
  f->leave();
  EXPECT_TRUE(f->isAtTopLevel());

  const std::string& out = f->toString();
  EXPECT_THAT(out,
              HasSubstr("TESTNODE[\"hello \"\"world\"\"\",UNQUOTED,42,1.25"));
  EXPECT_THAT(out, HasSubstr("SUB[]"));

  // ESRI double formatting appends ".0" to integers and uppercases exponent.
  auto f_esri =
      pj_io::WKTFormatter::create(pj_io::WKTFormatter::Convention::WKT1_ESRI);
  f_esri->startNode("E", false);
  f_esri->add(0.0, 15);
  f_esri->add(5.0, 15);
  f_esri->add(1.5e-25, 15);
  f_esri->endNode();
  EXPECT_EQ(f_esri->toString(), "E[0.0,5.0,1.5E-25]");

  // Ingest WKTNode tree back into WKTFormatter.
  auto parsed_node = pj_io::WKTNode::createFrom("OUTER[\"a\",INNER[1,2]]");
  auto f_ingest =
      pj_io::WKTFormatter::create(pj_io::WKTFormatter::Convention::WKT2_2019);
  f_ingest->setMultiLine(false);
  f_ingest->ingestWKTNode(parsed_node);
  EXPECT_EQ(f_ingest->toString(), "OUTER[\"a\",INNER[1,2]]");

  // morphNameToESRI covers suffixes and special characters.
  EXPECT_EQ(pj_io::WKTFormatter::morphNameToESRI("NAD83 / UTM zone 10N (m)"),
            "NAD83_UTM_zone_10N(m)");
  EXPECT_EQ(pj_io::WKTFormatter::morphNameToESRI("  foo/bar baz (ftUS)"),
            "foo_bar_baz(ftUS)");
  EXPECT_EQ(pj_io::WKTFormatter::morphNameToESRI("A+B-C(E-N)"), "A+B-C(E-N)");
}

TEST(WKTFormatterTest, UnbalancedStateThrowsFormattingException) {
  {
    auto f = pj_io::WKTFormatter::create();
    f->startNode("A", false);
    EXPECT_THROW(f->toString(), pj_io::FormattingException);
    f->endNode();
  }
  {
    auto f = pj_io::WKTFormatter::create();
    f->pushAxisLinearUnit(pj_util::nn_make_shared<pj_common::UnitOfMeasure>(
        pj_common::UnitOfMeasure::METRE));
    EXPECT_THROW(f->toString(), pj_io::FormattingException);
    f->popAxisLinearUnit();
  }
  {
    auto f = pj_io::WKTFormatter::create();
    f->pushAxisAngularUnit(pj_util::nn_make_shared<pj_common::UnitOfMeasure>(
        pj_common::UnitOfMeasure::DEGREE));
    EXPECT_THROW(f->toString(), pj_io::FormattingException);
    f->popAxisAngularUnit();
  }
  {
    auto f = pj_io::WKTFormatter::create();
    f->pushOutputId(false);
    EXPECT_THROW(f->toString(), pj_io::FormattingException);
    f->popOutputId();
  }
  {
    auto f = pj_io::WKTFormatter::create();
    f->pushOutputUnit(false);
    EXPECT_THROW(f->toString(), pj_io::FormattingException);
    f->popOutputUnit();
  }
  {
    auto f = pj_io::WKTFormatter::create();
    f->pushHasId(true);
    EXPECT_THROW(f->toString(), pj_io::FormattingException);
    f->popHasId();
  }
  {
    auto f = pj_io::WKTFormatter::create();
    f->pushDisableUsage();
    EXPECT_THROW(f->toString(), pj_io::FormattingException);
    f->popDisableUsage();
  }
}

TEST(WKTFormatterTest, ExportAllObjectTypesAcrossConventions) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  // Built-in CRS types across all conventions.
  const std::vector<pj_io::WKTFormatter::Convention> all_conventions = {
      pj_io::WKTFormatter::Convention::WKT2_2015,
      pj_io::WKTFormatter::Convention::WKT2_2015_SIMPLIFIED,
      pj_io::WKTFormatter::Convention::WKT2_2019,
      pj_io::WKTFormatter::Convention::WKT2_2019_SIMPLIFIED,
      pj_io::WKTFormatter::Convention::WKT1_GDAL,
      pj_io::WKTFormatter::Convention::WKT1_ESRI,
  };
  for (auto conv : all_conventions) {
    auto f = pj_io::WKTFormatter::create(conv, db.as_nullable());
    f->setStrict(false);
    const std::string wkt =
        pj_crs::GeographicCRS::EPSG_4326->exportToWKT(f.get());
    EXPECT_FALSE(wkt.empty());

    auto f_geocent = pj_io::WKTFormatter::create(conv, db.as_nullable());
    f_geocent->setStrict(false);
    if (conv == pj_io::WKTFormatter::Convention::WKT1_ESRI) {
      EXPECT_THROW(pj_crs::GeodeticCRS::EPSG_4978->exportToWKT(f_geocent.get()),
                   pj_io::FormattingException);
    } else {
      EXPECT_FALSE(
          pj_crs::GeodeticCRS::EPSG_4978->exportToWKT(f_geocent.get()).empty());
    }
  }

  // Geographic 3D strictly fails on WKT1_GDAL unless non-strict or allowed as
  // CompoundCRS.
  {
    auto f_strict = pj_io::WKTFormatter::create(
        pj_io::WKTFormatter::Convention::WKT1_GDAL, db.as_nullable());
    EXPECT_THROW(pj_crs::GeographicCRS::EPSG_4979->exportToWKT(f_strict.get()),
                 pj_io::FormattingException);

    auto f_allow = pj_io::WKTFormatter::create(
        pj_io::WKTFormatter::Convention::WKT1_GDAL, db.as_nullable());
    f_allow->setAllowEllipsoidalHeightAsVerticalCRS(true);
    EXPECT_THAT(pj_crs::GeographicCRS::EPSG_4979->exportToWKT(f_allow.get()),
                HasSubstr("COMPD_CS"));

    auto f_linunit = pj_io::WKTFormatter::create(
        pj_io::WKTFormatter::Convention::WKT1_ESRI, db.as_nullable());
    EXPECT_THAT(pj_crs::GeographicCRS::EPSG_4979->exportToWKT(f_linunit.get()),
                HasSubstr("LINUNIT"));
  }

  // DynamicGeodeticReferenceFrame + DatumEnsemble + Vertical + Compound +
  // Bound + Derived* + Engineering + Parametric + Temporal + Operations +
  // CoordinateMetadata.
  auto dyn_datum = pj_datum::DynamicGeodeticReferenceFrame::create(
      NameProps("DynFrame"), pj_datum::Ellipsoid::GRS1980,
      pj_util::optional<std::string>("Dyn anchor"),
      pj_datum::PrimeMeridian::PARIS,
      pj_common::Measure(2015.0, pj_common::UnitOfMeasure::YEAR),
      pj_util::optional<std::string>("DynModel"));
  auto dyn_geo = pj_crs::GeographicCRS::create(
      NameProps("DynGeo"), dyn_datum,
      pj_cs::EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
          pj_common::UnitOfMeasure::DEGREE, pj_common::UnitOfMeasure::METRE));
  auto f_2019 = pj_io::WKTFormatter::create(
      pj_io::WKTFormatter::Convention::WKT2_2019, db.as_nullable());
  const std::string dyn_wkt = dyn_geo->exportToWKT(f_2019.get());
  EXPECT_THAT(dyn_wkt, HasSubstr("DYNAMIC"));
  EXPECT_THAT(dyn_wkt, HasSubstr("FRAMEEPOCH[2015]"));
  EXPECT_THAT(dyn_wkt, HasSubstr("MODEL[\"DynModel\"]"));

  // DatumEnsemble
  auto d1 = pj_datum::GeodeticReferenceFrame::create(
      NameProps("Member1"), pj_datum::Ellipsoid::WGS84,
      pj_util::optional<std::string>(), pj_datum::PrimeMeridian::GREENWICH);
  auto d2 = pj_datum::GeodeticReferenceFrame::create(
      NameProps("Member2"), pj_datum::Ellipsoid::WGS84,
      pj_util::optional<std::string>(), pj_datum::PrimeMeridian::GREENWICH);
  auto ensemble = pj_datum::DatumEnsemble::create(
      NameProps("Test Ensemble"), {d1, d2},
      pj_meta::PositionalAccuracy::create("2.0"));
  auto ens_geo = pj_crs::GeographicCRS::create(
      NameProps("EnsGeo"), nullptr, ensemble,
      pj_cs::EllipsoidalCS::createLatitudeLongitude(
          pj_common::UnitOfMeasure::DEGREE));
  EXPECT_THAT(
      ens_geo->exportToWKT(
          pj_io::WKTFormatter::create(
              pj_io::WKTFormatter::Convention::WKT2_2019, db.as_nullable())
              .get()),
      HasSubstr("ENSEMBLE["));

  // Engineering, Parametric, Temporal, and Derived* CRSs.
  auto eng_datum = pj_datum::EngineeringDatum::create(
      NameProps("Local origin"), pj_util::optional<std::string>("Corner"));
  auto eng_crs =
      pj_crs::EngineeringCRS::create(NameProps("Local Eng"), eng_datum,
                                     pj_cs::CartesianCS::createEastingNorthing(
                                         pj_common::UnitOfMeasure::METRE));
  EXPECT_THAT(
      eng_crs->exportToWKT(pj_io::WKTFormatter::create(
                               pj_io::WKTFormatter::Convention::WKT2_2019)
                               .get()),
      HasSubstr("ENGCRS["));
  EXPECT_THAT(
      eng_crs->exportToWKT(pj_io::WKTFormatter::create(
                               pj_io::WKTFormatter::Convention::WKT1_GDAL)
                               .get()),
      HasSubstr("LOCAL_CS["));

  auto param_unit = pj_common::UnitOfMeasure(
      "hPa", 100.0, pj_common::UnitOfMeasure::Type::PARAMETRIC);
  auto param_axis = pj_cs::CoordinateSystemAxis::create(
      NameProps("Pressure"), "P", pj_cs::AxisDirection::UP, param_unit);
  auto param_cs = pj_cs::ParametricCS::create(NameProps("ParamCS"), param_axis);
  auto param_datum =
      pj_datum::ParametricDatum::create(NameProps("PressureDatum"));
  auto param_crs = pj_crs::ParametricCRS::create(NameProps("PressureCRS"),
                                                 param_datum, param_cs);
  EXPECT_THAT(
      param_crs->exportToWKT(pj_io::WKTFormatter::create(
                                 pj_io::WKTFormatter::Convention::WKT2_2019)
                                 .get()),
      HasSubstr("PARAMETRICCRS["));

  auto time_axis = pj_cs::CoordinateSystemAxis::create(
      NameProps("Time"), "t", pj_cs::AxisDirection::FUTURE,
      pj_common::UnitOfMeasure::YEAR);
  auto time_cs =
      pj_cs::TemporalMeasureCS::create(NameProps("TimeCS"), time_axis);
  auto time_datum = pj_datum::TemporalDatum::create(
      NameProps("Epoch2000"), pj_common::DateTime::create("2000-01-01"),
      pj_datum::TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN);
  auto time_crs = pj_crs::TemporalCRS::create(NameProps("TemporalCRS"),
                                              time_datum, time_cs);
  EXPECT_THAT(
      time_crs->exportToWKT(pj_io::WKTFormatter::create(
                                pj_io::WKTFormatter::Convention::WKT2_2019)
                                .get()),
      HasSubstr("TIMECRS["));

  // Vertical + Compound + CoordinateMetadata + Operations.
  auto v_datum = pj_datum::DynamicVerticalReferenceFrame::create(
      NameProps("DynVert"), pj_util::optional<std::string>("Zero mark"),
      pj_util::optional<pj_datum::RealizationMethod>(
          pj_datum::RealizationMethod::GEOID),
      pj_common::Measure(2020.0, pj_common::UnitOfMeasure::YEAR),
      pj_util::optional<std::string>("GIA"));
  auto v_crs =
      pj_crs::VerticalCRS::create(NameProps("DynVertCRS"), v_datum,
                                  pj_cs::VerticalCS::createGravityRelatedHeight(
                                      pj_common::UnitOfMeasure::METRE));
  EXPECT_THAT(v_crs->exportToWKT(pj_io::WKTFormatter::create(
                                     pj_io::WKTFormatter::Convention::WKT2_2019)
                                     .get()),
              HasSubstr("VERTCRS["));

  auto compound = pj_crs::CompoundCRS::create(
      NameProps("Comp"), {pj_crs::GeographicCRS::EPSG_4326, v_crs});
  EXPECT_THAT(
      compound->exportToWKT(pj_io::WKTFormatter::create(
                                pj_io::WKTFormatter::Convention::WKT2_2019)
                                .get()),
      HasSubstr("COMPOUNDCRS["));

  auto coord_meta = pj_coord::CoordinateMetadata::create(
      pj_crs::GeographicCRS::EPSG_4326, 2024.5);
  const std::string cm_wkt = coord_meta->exportToWKT(
      pj_io::WKTFormatter::create(pj_io::WKTFormatter::Convention::WKT2_2019)
          .get());
  EXPECT_THAT(cm_wkt, HasSubstr("COORDINATEMETADATA["));
  EXPECT_THAT(cm_wkt, HasSubstr("EPOCH[2024.5]"));

  // PointMotionOperation + Transformation + ConcatenatedOperation.
  auto tr = pj_op::Transformation::createGeocentricTranslations(
      NameProps("Trans"), pj_crs::GeographicCRS::EPSG_4326,
      pj_crs::GeographicCRS::EPSG_4269, 1.0, 2.0, 3.0,
      {pj_meta::PositionalAccuracy::create("0.5")});
  EXPECT_THAT(tr->exportToWKT(pj_io::WKTFormatter::create(
                                  pj_io::WKTFormatter::Convention::WKT2_2019)
                                  .get()),
              HasSubstr("COORDINATEOPERATION["));

  auto pmo_param =
      pj_op::OperationParameter::create(NameProps("X-axis velocity"));
  auto pmo = pj_op::PointMotionOperation::create(
      NameProps("PMO"), pj_crs::GeodeticCRS::EPSG_4978,
      NameProps("Cartesian Grid Offsets"), {pmo_param},
      {pj_op::ParameterValue::create(
          pj_common::Measure(0.01, pj_common::UnitOfMeasure::METRE_PER_YEAR))},
      {pj_meta::PositionalAccuracy::create("0.1")});
  EXPECT_THAT(pmo->exportToWKT(pj_io::WKTFormatter::create(
                                   pj_io::WKTFormatter::Convention::WKT2_2019)
                                   .get()),
              HasSubstr("POINTMOTIONOPERATION["));

  auto tr2 = pj_op::Transformation::createGeocentricTranslations(
      NameProps("Trans2"), pj_crs::GeographicCRS::EPSG_4269,
      pj_crs::GeographicCRS::EPSG_4267, -1.0, 0.0, 1.0, {});
  auto concat = pj_op::ConcatenatedOperation::create(
      NameProps("ConcatOp"), {tr, tr2},
      {pj_meta::PositionalAccuracy::create("1.5")});
  EXPECT_THAT(
      concat->exportToWKT(pj_io::WKTFormatter::create(
                              pj_io::WKTFormatter::Convention::WKT2_2019)
                              .get()),
      HasSubstr("CONCATENATEDOPERATION["));
}

// =============================================================================
// Suite 2: WKTNodeTest
// =============================================================================

TEST(WKTNodeTest, TreeBuildingLookupAndRoundTrip) {
  auto root = pj_io::WKTNode::createFrom(
      "PROJCRS[\"Test \"\"Q\"\"\" , BASEGEOGCRS[\"WGS 84\"], "
      "PARAMETER[\"p\",1,ID[\"EPSG\",10]], ID[\"EPSG\",999]]");
  EXPECT_EQ(root->value(), "PROJCRS");
  ASSERT_EQ(root->children().size(), 4u);
  EXPECT_EQ(root->children()[0]->value(), "\"Test \"Q\"\"");
  EXPECT_EQ(root->countChildrenOfName("ID"), 1);
  EXPECT_EQ(root->countChildrenOfName("id"), 1);
  EXPECT_EQ(root->countChildrenOfName("MISSING"), 0);

  const auto& found = root->lookForChild("id", 0);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->value(), "ID");

  // Missing child returns non-null sentinel node whose value() is empty().
  const auto& missing = root->lookForChild("NON_EXISTENT", 0);
  ASSERT_NE(missing, nullptr);
  EXPECT_TRUE(missing->value().empty());
  EXPECT_TRUE(missing->children().empty());

  const auto& second_id = root->lookForChild("ID", 1);
  EXPECT_TRUE(second_id->value().empty());

  // Manual child addition and round-trip serialization.
  auto custom = std::make_unique<pj_io::WKTNode>("CUSTOM");
  custom->addChild(NN_CHECK_THROW(std::make_unique<pj_io::WKTNode>("\"abc\"")));
  custom->addChild(NN_CHECK_THROW(std::make_unique<pj_io::WKTNode>("123")));
  EXPECT_EQ(custom->toString(), "CUSTOM[\"abc\",123]");
  EXPECT_THAT(root->toString(), HasSubstr("\"\"Q\"\""));

  // Parentheses delimiter and UTF-8 smart quotes with indexStart > 0.
  const std::string parens_wkt =
      "  GEOGCS(\xE2\x80\x9CMy Smart Name\xE2\x80\x9D,UNIT(\"deg\",1))";
  auto paren_node = pj_io::WKTNode::createFrom(parens_wkt, 2);
  EXPECT_EQ(paren_node->value(), "GEOGCS");
  ASSERT_EQ(paren_node->children().size(), 2u);
  EXPECT_EQ(paren_node->children()[0]->value(), "\"My Smart Name\"");

  // Error branches in WKTNode::createFrom.
  EXPECT_THROW(pj_io::WKTNode::createFrom("   "), pj_io::ParsingException);
  EXPECT_THROW(pj_io::WKTNode::createFrom("NODE"), pj_io::ParsingException);
  EXPECT_THROW(pj_io::WKTNode::createFrom("NODE 1"), pj_io::ParsingException);
  EXPECT_THROW(pj_io::WKTNode::createFrom("NODE[\"unclosed]"),
               pj_io::ParsingException);
  EXPECT_THROW(pj_io::WKTNode::createFrom("NODE[1"), pj_io::ParsingException);
  EXPECT_THROW(pj_io::WKTNode::createFrom("NODE[CHILD[1]"),
               pj_io::ParsingException);
  // Nesting limit recLevel == 16.
  std::string deep = "A[";
  for (int i = 0; i < 18; ++i) {
    deep += "B[";
  }
  deep += "1";
  for (int i = 0; i < 19; ++i) {
    deep += "]";
  }
  EXPECT_THROW(pj_io::WKTNode::createFrom(deep), pj_io::ParsingException);
}

// =============================================================================
// Suite 3: WKTParserTest
// =============================================================================

TEST(WKTParserTest, GuessDialectAllVariants) {
  pj_io::WKTParser parser;
  using Dialect = pj_io::WKTParser::WKTGuessedDialect;

  EXPECT_EQ(parser.guessDialect("   VERTCS[\"NAVD88\"]"), Dialect::WKT1_ESRI);
  EXPECT_EQ(
      parser.guessDialect("  "
                          "GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\","
                          "SPHEROID[\"WGS_1984\",6378137,298.257223563]]]"),
      Dialect::WKT1_ESRI);
  EXPECT_EQ(parser.guessDialect("PROJCS[\"Test\",GEOGCS[\"X\"]]"),
            Dialect::WKT1_ESRI);
  EXPECT_EQ(parser.guessDialect("PROJCS[\"Test\",GEOGCS[\"X\"],PARAMETER["
                                "\"rectified_grid_angle\",10]]"),
            Dialect::WKT1_GDAL);
  EXPECT_EQ(
      parser.guessDialect("PROJCS[\"Test\",GEOGCS[\"X\"],AXIS[\"E\",EAST]]"),
      Dialect::WKT1_GDAL);
  EXPECT_EQ(parser.guessDialect("LOCAL_CS[\"Eng\",UNIT[\"m\",1]]"),
            Dialect::WKT1_GDAL);

  EXPECT_EQ(
      parser.guessDialect("GEOGCRS[\"WGS 84\",DATUM[\"D\"],CS[ellipsoidal,2]]"),
      Dialect::WKT2_2019);
  EXPECT_EQ(parser.guessDialect(
                "GEODCRS[\"Test\",DATUM[\"D\"],USAGE[AREA[\"World\"]]]"),
            Dialect::WKT2_2019);
  EXPECT_EQ(parser.guessDialect(
                "TIMECRS[\"T\",TDATUM[\"D\"],CS[TemporalDateTime,1]]"),
            Dialect::WKT2_2019);
  EXPECT_EQ(
      parser.guessDialect("GEODCRS[\"Test\",DATUM[\"D\"],CS[Cartesian,3]]"),
      Dialect::WKT2_2015);
  EXPECT_EQ(parser.guessDialect("+proj=longlat +datum=WGS84"),
            Dialect::NOT_WKT);
  EXPECT_EQ(parser.guessDialect("EPSG:4326"), Dialect::NOT_WKT);
}

TEST(WKTParserTest, ParseGeographicGeocentricSphericalAndStandalone) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  // Non-strict standalone fragments + recoverable warnings + grammar errors.
  {
    pj_io::WKTParser parser;
    parser.attachDatabaseContext(db.as_nullable()).setStrict(false);

    auto ell_inf =
        parser.createFromWKT("ELLIPSOID[\"Sphere\",6370997,\"inf\"]");
    auto ell_cast =
        pj_util::nn_dynamic_pointer_cast<pj_datum::Ellipsoid>(ell_inf);
    ASSERT_NE(ell_cast, nullptr);
    EXPECT_TRUE(ell_cast->isSphere());
    EXPECT_FALSE(parser.warningList().empty());
    EXPECT_FALSE(parser.grammarErrorList().empty());

    auto id_obj = parser.createFromWKT(
        "ID[\"INVERSE(IAU)\",49900,2015,"
        "CITATION[\"IAU Citation\"],URI[\"https://example.org/iau\"]]");
    auto id_cast =
        pj_util::nn_dynamic_pointer_cast<pj_meta::Identifier>(id_obj);
    ASSERT_NE(id_cast, nullptr);

    auto trf_obj = parser.createFromWKT(
        "TRF[\"Oracle Frame (EPSG ID 6171)\","
        "ELLIPSOID[\"GRS 1980\",6378137,298.257222101]],"
        "PRIMEM[\"Paris\",2.33722917,UNIT[\"grad\",0.015707963267948966]]");
    auto grf =
        pj_util::nn_dynamic_pointer_cast<pj_datum::GeodeticReferenceFrame>(
            trf_obj);
    ASSERT_NE(grf, nullptr);
    EXPECT_NEAR(grf->primeMeridian()->longitude().convertToUnit(
                    pj_common::UnitOfMeasure::GRAD),
                2.5969213, 1e-6);

    auto ens_obj = parser.createFromWKT(
        "ENSEMBLE[\"Test Ens\",MEMBER[\"M1\"],MEMBER[\"M2\"],"
        "ELLIPSOID[\"WGS 84\",6378137,298.257223563],"
        "ENSEMBLEACCURACY[2.0]]");
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_datum::DatumEnsemble>(ens_obj),
        nullptr);

    auto vdatum_obj = parser.createFromWKT(
        "VDATUM[\"Test VDatum\",ANCHOR[\"Benchmark 1\"],ANCHOREPOCH[2015.0]]");
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_datum::VerticalReferenceFrame>(
            vdatum_obj),
        nullptr);
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_datum::TemporalDatum>(
            parser.createFromWKT("TDATUM[\"T\",TIMEORIGIN[\"2000-01-01\"],"
                                 "CALENDAR[\"prolepticGregorian\"]]")),
        nullptr);
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::EngineeringDatum>(
                  parser.createFromWKT("EDATUM[\"Local\",ANCHOR[\"A\"]]")),
              nullptr);
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::ParametricDatum>(
                  parser.createFromWKT("PDATUM[\"Param\",ANCHOR[\"P\"]]")),
              nullptr);
  }

  // WKT1 GEOCCS with missing PRIMEM and explicit UNIT (non-strict warning).
  {
    pj_io::WKTParser parser;
    parser.attachDatabaseContext(db.as_nullable()).setStrict(false);
    EXPECT_THROW(parser.createFromWKT(
                     "GEOCCS[\"Custom Geocentric\",DATUM[\"WGS_1984\","
                     "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
                     "AXIS[\"Geocentric X\",OTHER],AXIS[\"Geocentric Y\",EAST],"
                     "AXIS[\"Geocentric Z\",NORTH]]"),
                 pj_io::ParsingException);
    auto geocc = parser.createFromWKT(
        "GEOCCS[\"Custom Geocentric\",DATUM[\"WGS_1984\","
        "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
        "UNIT[\"Meter\",1.0],"
        "AXIS[\"Geocentric X\",OTHER],AXIS[\"Geocentric Y\",EAST],"
        "AXIS[\"Geocentric Z\",NORTH]]");
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeodeticCRS>(geocc),
              nullptr);
    EXPECT_FALSE(parser.warningList().empty());

    // GEOCCS without axes exercises default Geocentric CS.
    auto geocc_no_axis = parser.createFromWKT(
        "GEOCCS[\"Simple Geocentric\",DATUM[\"WGS_1984\","
        "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0]]");
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_crs::GeodeticCRS>(geocc_no_axis),
        nullptr);
  }

  // ESRI GEOGCS with LINUNIT -> 3D GeographicCRS, plus Lisbon PM DMS fixup,
  // plus USAGE with BBOX + VERTICALEXTENT + TIMEEXTENT.
  {
    pj_io::WKTParser parser;
    parser.attachDatabaseContext(db.as_nullable()).setStrict(false);
    auto geo3d = parser.createFromWKT(
        "GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\","
        "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
        "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433],"
        "LINUNIT[\"Meter\",1.0]]");
    auto geo_crs =
        pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(geo3d);
    ASSERT_NE(geo_crs, nullptr);
    EXPECT_EQ(geo_crs->coordinateSystem()->axisList().size(), 3u);

    auto lisbon = parser.createFromWKT(
        "GEOGCRS[\"Lisbon Test (deprecated)\","
        "DATUM[\"Lisbon Datum\",ELLIPSOID[\"International 1924\",6378388,297,"
        "LENGTHUNIT[\"metre\",1]],ANCHOR[\"Castelo\"],ANCHOREPOCH[1937.0]],"
        "PRIMEM[\"Lisbon\",-9.0754862,ANGLEUNIT[\"degree\",0.0174532925199433]]"
        ","
        "CS[ellipsoidal,2],"
        "AXIS[\"geodetic latitude (Lat)\",north,ORDER[1],AXISMINVALUE[-90],"
        "AXISMAXVALUE[90],RANGEMEANING[exact]],"
        "AXIS[\"geodetic longitude (Lon)\",east,ORDER[2],MERIDIAN[0,"
        "ANGLEUNIT[\"degree\",0.0174532925199433]],AXISMINVALUE[-180],"
        "AXISMAXVALUE[180],RANGEMEANING[wraparound]],"
        "ANGLEUNIT[\"degree\",0.0174532925199433],"
        "USAGE[SCOPE[\"Mapping\"],AREA[\"Portugal\"],"
        "BBOX[36,-10,43,-6],VERTICALEXTENT[-100,2500,LENGTHUNIT[\"metre\",1]],"
        "TIMEEXTENT[\"1937-01-01\",\"2020-01-01\"]],"
        "REMARK[\"Legacy test\"],ID[\"IAU\",39900,2015]]");
    auto lisbon_crs =
        pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(lisbon);
    ASSERT_NE(lisbon_crs, nullptr);
    EXPECT_TRUE(lisbon_crs->isDeprecated());
    EXPECT_NEAR(lisbon_crs->primeMeridian()->longitude().convertToUnit(
                    pj_common::UnitOfMeasure::DEGREE),
                -9.131906111, 1e-6);
  }

  // WKT2 DYNAMIC frame and SphericalCS on GeodeticCRS.
  {
    pj_io::WKTParser parser;
    parser.attachDatabaseContext(db.as_nullable()).setStrict(false);
    auto dyn = parser.createFromWKT(
        "GEOGCRS[\"Dyn\",DYNAMIC[FRAMEEPOCH[2010.0],MODEL[\"VEL\"]],"
        "DATUM[\"DynD\",ELLIPSOID[\"GRS 1980\",6378137,298.257222101,"
        "LENGTHUNIT[\"metre\",1]]],"
        "CS[ellipsoidal,2],"
        "AXIS[\"Latitude\",north,ORDER[1]],"
        "AXIS[\"Longitude\",east,ORDER[2]],"
        "ANGLEUNIT[\"degree\",0.0174532925199433]]");
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(dyn),
              nullptr);

    auto sph = parser.createFromWKT(
        "GEODCRS[\"Spherical\",DATUM[\"Moon\",ELLIPSOID[\"Moon\",1737400,0,"
        "LENGTHUNIT[\"metre\",1]]],"
        "CS[spherical,3],"
        "AXIS[\"Planetocentric latitude\",north,ORDER[1],"
        "ANGLEUNIT[\"degree\",0.0174532925199433]],"
        "AXIS[\"Planetocentric longitude\",east,ORDER[2],"
        "ANGLEUNIT[\"degree\",0.0174532925199433]],"
        "AXIS[\"Geocentric radius\",up,ORDER[3],LENGTHUNIT[\"metre\",1]]]");
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeodeticCRS>(sph),
              nullptr);
  }
}

TEST(WKTParserTest, ParseProjectedCSVariantsAndEdgeCases) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  pj_io::WKTParser parser;
  parser.attachDatabaseContext(db.as_nullable())
      .setStrict(false)
      .setUnsetIdentifiersIfIncompatibleDef(true);

  // Mercator_1SP with non-zero latitude_of_origin -> Mercator_2SP hack.
  auto merc_hack = parser.createFromWKT(
      "PROJCS[\"Merc2SP Hack\","
      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
      "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
      "UNIT[\"degree\",0.0174532925199433]],"
      "PROJECTION[\"Mercator_1SP\"],"
      "PARAMETER[\"latitude_of_origin\",45],"
      "PARAMETER[\"central_meridian\",10],"
      "PARAMETER[\"scale_factor\",1],"
      "PARAMETER[\"false_easting\",0],"
      "PARAMETER[\"false_northing\",0],"
      "UNIT[\"metre\",1]]");
  auto merc_proj =
      pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(merc_hack);
  ASSERT_NE(merc_proj, nullptr);
  EXPECT_THAT(merc_proj->derivingConversion()->method()->nameStr(),
              HasSubstr("variant B"));

  // Web Mercator GDAL WKT1 EXTENSION["PROJ4", ...].
  auto web_merc = parser.createFromWKT(
      "PROJCS[\"WebMerc\","
      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
      "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
      "UNIT[\"degree\",0.0174532925199433]],"
      "PROJECTION[\"Mercator_1SP\"],"
      "PARAMETER[\"central_meridian\",0],PARAMETER[\"scale_factor\",1],"
      "PARAMETER[\"false_easting\",0],PARAMETER[\"false_northing\",0],"
      "UNIT[\"metre\",1],"
      "EXTENSION[\"PROJ4\",\"+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0 "
      "+lon_0=0.0 +x_0=0.0 +y_0=0 +k=1.0 +units=m +nadgrids=@null +wktext\"]]");
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(web_merc),
            nullptr);

  // Polar Stereographic Variant B (scale_factor = 1) and Variant A (|lat0|=90).
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<
          pj_crs::ProjectedCRS>(parser.createFromWKT(
          "PROJCS[\"PS B\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
          "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
          "UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Polar_"
          "Stereographic\"],"
          "PARAMETER[\"latitude_of_origin\",71],PARAMETER[\"central_meridian\","
          "0],"
          "PARAMETER[\"scale_factor\",1],PARAMETER[\"false_easting\",0],"
          "PARAMETER[\"false_northing\",0],UNIT[\"metre\",1]]")),
      nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<
          pj_crs::ProjectedCRS>(parser.createFromWKT(
          "PROJCS[\"PS A\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
          "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
          "UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Polar_"
          "Stereographic\"],"
          "PARAMETER[\"latitude_of_origin\",-90],PARAMETER[\"central_"
          "meridian\",0],"
          "PARAMETER[\"scale_factor\",0.994],PARAMETER[\"false_easting\","
          "2000000],"
          "PARAMETER[\"false_northing\",2000000],UNIT[\"metre\",1]]")),
      nullptr);

  // Oracle/Trimble "Lambert Conformal Conic" disambiguation: 2SP, 1SP,
  // Michigan.
  const std::string lcc_base =
      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
      "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
      "UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Lambert Conformal "
      "Conic\"],";
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
                parser.createFromWKT(
                    "PROJCS[\"LCC2\"," + lcc_base +
                    "PARAMETER[\"standard_parallel_1\",33],"
                    "PARAMETER[\"standard_parallel_2\",45],"
                    "PARAMETER[\"latitude_of_origin\",39],"
                    "PARAMETER[\"central_meridian\",-96],UNIT[\"metre\",1]]")),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
                parser.createFromWKT(
                    "PROJCS[\"LCC1\"," + lcc_base +
                    "PARAMETER[\"latitude_of_origin\",45],"
                    "PARAMETER[\"central_meridian\",0],"
                    "PARAMETER[\"scale_factor\",0.999],UNIT[\"metre\",1]]")),
            nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
          parser.createFromWKT(
              "PROJCS[\"LCCM\"," + lcc_base +
              "PARAMETER[\"standard_parallel_1\",42],"
              "PARAMETER[\"standard_parallel_2\",44],"
              "PARAMETER[\"latitude_of_origin\",41],"
              "PARAMETER[\"central_meridian\",-84],"
              "PARAMETER[\"scale_factor\",1.0000382],UNIT[\"metre\",1]]")),
      nullptr);

  // Hotine Oblique Mercator missing rectified_grid_angle (copies azimuth),
  // missing scale_factor warning, and Krovak SW axis handling.
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<
          pj_crs::ProjectedCRS>(parser.createFromWKT(
          "PROJCS[\"HOM\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
          "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
          "UNIT[\"degree\",0.0174532925199433]],"
          "PROJECTION[\"Hotine_Oblique_Mercator\"],"
          "PARAMETER[\"latitude_of_center\",45],PARAMETER[\"longitude_of_"
          "center\",15],"
          "PARAMETER[\"azimuth\",30],PARAMETER[\"scale_factor\",1],"
          "PARAMETER[\"false_easting\",0],PARAMETER[\"false_northing\",0],"
          "UNIT[\"metre\",1]]")),
      nullptr);

  auto missing_k = parser.createFromWKT(
      "PROJCS[\"MissingK\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
      "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
      "UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Transverse_Mercator\"]"
      ","
      "PARAMETER[\"latitude_of_origin\",0],PARAMETER[\"central_meridian\",9],"
      "UNIT[\"metre\",1]]");
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(missing_k),
            nullptr);

  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
          parser.createFromWKT(
              "PROJCS[\"KrovakSW\",GEOGCS[\"Bessel\",DATUM[\"MGI\","
              "SPHEROID[\"Bessel\",6377397.155,299.1528128]],PRIMEM["
              "\"Greenwich\",0],"
              "UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Krovak\"],"
              "UNIT[\"metre\",1],AXIS[\"Y\",SOUTH],AXIS[\"X\",WEST]]")),
      nullptr);

  // ESRI WKT with Stereographic pole, Plate_Carree, Gauss_Kruger,
  // Krovak_East_North, and X_Scale.
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<
          pj_crs::ProjectedCRS>(parser.createFromWKT(
          "PROJCS[\"ESRI_PS\",GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\","
          "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
          "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],"
          "PROJECTION[\"Stereographic\"],"
          "PARAMETER[\"False_Easting\",0.0],PARAMETER[\"False_Northing\",0.0],"
          "PARAMETER[\"Central_Meridian\",0.0],PARAMETER[\"Scale_Factor\",0."
          "994],"
          "PARAMETER[\"Latitude_Of_Origin\",90.0],PARAMETER[\"X_Scale\",1.0],"
          "UNIT[\"Meter\",1.0]]")),
      nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
          parser.createFromWKT(
              "PROJCS[\"ESRI_PC\",GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\","
              "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
              "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],"
              "PROJECTION[\"Plate_Carree\"],UNIT[\"Meter\",1.0]]")),
      nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<
          pj_crs::ProjectedCRS>(parser.createFromWKT(
          "PROJCS[\"ESRI_GK_East_North\",GEOGCS[\"GCS_WGS_1984\","
          "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]]"
          ","
          "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],"
          "PROJECTION[\"Gauss_Kruger\"],"
          "PARAMETER[\"Central_Meridian\",9.0],PARAMETER[\"Scale_Factor\",1.0],"
          "UNIT[\"Meter\",1.0]]")),
      nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<
          pj_crs::ProjectedCRS>(parser.createFromWKT(
          "PROJCS[\"Test_Krovak_East_North\",GEOGCS[\"GCS_WGS_1984\","
          "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]]"
          ","
          "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],"
          "PROJECTION[\"Krovak\"],PARAMETER[\"Pseudo_Standard_Parallel_1\",78."
          "5],"
          "PARAMETER[\"Scale_Factor\",0.9999],PARAMETER[\"Azimuth\",30.2881],"
          "PARAMETER[\"Longitude_Of_Center\",24.8333],"
          "PARAMETER[\"Latitude_Of_Center\",49.5],UNIT[\"Meter\",1.0]]")),
      nullptr);

  // ob_tran EXTENSION["PROJ4"] -> DerivedGeographicCRS, TOWGS84 + PROJ4_GRIDS
  // -> BoundCRS, plus UTM S pre-9.5 ID autofix + incompatible baseCRS ID
  // clearing.
  auto ob_tran = parser.createFromWKT(
      "PROJCS[\"ObTran\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
      "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
      "UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Transverse_Mercator\"]"
      ","
      "UNIT[\"metre\",1],"
      "EXTENSION[\"PROJ4\",\"+proj=ob_tran +o_proj=longlat +o_lon_p=-162 "
      "+o_lat_p=39.25 +lon_0=180 +ellps=sphere\"]]");
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedGeographicCRS>(ob_tran),
      nullptr);

  auto bound_towgs = parser.createFromWKT(
      "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
      "SPHEROID[\"WGS 84\",6378137,298.257223563],"
      "TOWGS84[1,2,3]],PRIMEM[\"Greenwich\",0],"
      "UNIT[\"degree\",0.0174532925199433]]");
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::BoundCRS>(bound_towgs),
            nullptr);

  auto utm_south_autofix = parser.createFromWKT(
      "PROJCRS[\"WGS 84 / UTM zone 31S\","
      "BASEGEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],AXIS[\"lon\",east,ORDER[1]],AXIS[\"lat\",north,ORDER["
      "2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433],ID[\"EPSG\",4326]],"
      "CONVERSION[\"UTM Zone 31S\","
      "METHOD[\"Transverse Mercator\",ID[\"EPSG\",9807]],"
      "PARAMETER[\"Latitude of natural origin\",0],"
      "PARAMETER[\"Longitude of natural origin\",3],"
      "PARAMETER[\"Scale factor at natural origin\",0.9996],"
      "PARAMETER[\"False easting\",500000],"
      "PARAMETER[\"False northing\",10000000],"
      "ID[\"EPSG\",16031]],"
      "CS[Cartesian,2],"
      "AXIS[\"E\",east,ORDER[1],LENGTHUNIT[\"metre\",1]],"
      "AXIS[\"N\",north,ORDER[2],LENGTHUNIT[\"foot\",0.3048]]]");
  auto proj_fix =
      pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(utm_south_autofix);
  ASSERT_NE(proj_fix, nullptr);
  ASSERT_FALSE(proj_fix->derivingConversion()->identifiers().empty());
  EXPECT_EQ(proj_fix->derivingConversion()->identifiers()[0]->code(), "16131");
  EXPECT_TRUE(proj_fix->baseCRS()->identifiers().empty());
}

TEST(WKTParserTest, ParseVerticalCompoundBoundDerivedEngParamTimeOpsAndErrors) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  pj_io::WKTParser parser;
  parser.attachDatabaseContext(db.as_nullable()).setStrict(false);

  // ESRI VERTCS with DATUM (ellipsoidal 2002) and with Direction=-1 (DOWN),
  // plus PROJCS,VERTCS implicit compound and Lidar NAVD88 - GEOID18 / Geoid
  // 2012A.
  auto vert_ellps = parser.createFromWKT(
      "VERTCS[\"WGS_1984\",DATUM[\"D_WGS_1984\","
      "SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
      "PARAMETER[\"Vertical_Shift\",0.0],PARAMETER[\"Direction\",1.0],"
      "UNIT[\"Meter\",1.0]]");
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::VerticalCRS>(vert_ellps),
            nullptr);

  auto vert_down = parser.createFromWKT(
      "VERTCS[\"Depth\",VDATUM[\"MSL\"],PARAMETER[\"Direction\",-1.0]]");
  auto vert_down_crs =
      pj_util::nn_dynamic_pointer_cast<pj_crs::VerticalCRS>(vert_down);
  ASSERT_NE(vert_down_crs, nullptr);
  EXPECT_EQ(&vert_down_crs->coordinateSystem()->axisList()[0]->direction(),
            &pj_cs::AxisDirection::DOWN);

  auto esri_compound = parser.createFromWKT(
      "GEOGCS[\"NAD83\",DATUM[\"NAD83\",SPHEROID[\"GRS "
      "1980\",6378137,298.257222101]],"
      "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
      "VERTCS[\"Geoid 2012A\",VDATUM[\"Local\"],UNIT[\"Meter\",1.0]]");
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(esri_compound),
      nullptr);

  auto lidar_v = parser.createFromWKT(
      "COMPD_CS[\"CompLidar\",GEOGCS[\"NAD83\",DATUM[\"NAD83\","
      "SPHEROID[\"GRS 1980\",6378137,298.257222101]],PRIMEM[\"Greenwich\",0],"
      "UNIT[\"degree\",0.0174532925199433]],"
      "VERT_CS[\"NAVD88 - "
      "GEOID18\",VERT_DATUM[\"Local\",2005],UNIT[\"Meter\",1.0]]]");
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(lidar_v),
            nullptr);

  // VDATUM PROJ4_GRIDS -> BoundCRS, plus GEOIDMODEL in WKT2.
  auto v_grid = parser.createFromWKT(
      "VERTCRS[\"NAVD88\",VDATUM[\"NAVD88\",EXTENSION[\"PROJ4_GRIDS\",\"g18."
      "gtx\"]],"
      "CS[vertical,1],AXIS[\"H\",up,LENGTHUNIT[\"metre\",1]],"
      "GEOIDMODEL[\"GEOID18\"]]");
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::BoundCRS>(v_grid),
            nullptr);

  // WKT2 BoundCRS with Geographic3D to GravityRelatedHeight auto-reversal.
  auto bnd_v = parser.createFromWKT(
      "BOUNDCRS[SOURCECRS[VERTCRS[\"H\",VDATUM[\"V\"],CS[vertical,1],"
      "AXIS[\"H\",up,LENGTHUNIT[\"metre\",1]]]],"
      "TARGETCRS[GEOGCRS[\"WGS 84\",DATUM[\"WGS84\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,3],AXIS[\"lat\",north,ORDER[1]],"
      "AXIS[\"lon\",east,ORDER[2]],"
      "AXIS[\"h\",up,ORDER[3],LENGTHUNIT[\"metre\",1]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
      "ABRIDGEDTRANSFORMATION[\"V to 3D\","
      "METHOD[\"Geographic3D to GravityRelatedHeight (gtx)\"],"
      "PARAMETERFILE[\"Geoid (height correction) model "
      "file\",\"custom.gtx\"]]]");
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::BoundCRS>(bnd_v), nullptr);

  // LOCAL_CS with 0, 1, 2, 3 axes; 4 axes throws.
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::EngineeringCRS>(
                parser.createFromWKT("LOCAL_CS[\"Empty\",UNIT[\"m\",1]]")),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::EngineeringCRS>(
                parser.createFromWKT(
                    "LOCAL_CS[\"Three\",LOCAL_DATUM[\"D\",0],"
                    "UNIT[\"m\",1],AXIS[\"X\",EAST],AXIS[\"Y\",NORTH],"
                    "AXIS[\"Z\",UP]]")),
            nullptr);
  EXPECT_THROW(
      parser.createFromWKT(
          "LOCAL_CS[\"Four\",UNIT[\"m\",1],AXIS[\"X\",EAST],AXIS[\"Y\",NORTH],"
          "AXIS[\"Z\",UP],AXIS[\"T\",FUTURE]]"),
      pj_io::ParsingException);

  // Affine, Ordinal, Derived* CRSs, and Temporal variants.
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::EngineeringCRS>(
          parser.createFromWKT(
              "ENGCRS[\"EngAffine\",EDATUM[\"Orig\"],CS[affine,2],"
              "AXIS[\"i\",columnPositive,ORDER[1],LENGTHUNIT[\"metre\",1]],"
              "AXIS[\"j\",rowPositive,ORDER[2],LENGTHUNIT[\"metre\",1]]]")),
      nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::EngineeringCRS>(
                parser.createFromWKT(
                    "ENGCRS[\"EngOrd\",EDATUM[\"Orig\"],CS[ordinal,2],"
                    "AXIS[\"row\",rowPositive,ORDER[1]],"
                    "AXIS[\"col\",columnPositive,ORDER[2]]]")),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::TemporalCRS>(
                parser.createFromWKT(
                    "TIMECRS[\"Time\",TDATUM[\"T\",TIMEORIGIN[\"2000-01-01\"]],"
                    "CS[TemporalDateTime,1],AXIS[\"t\",future,ORDER[1]]]")),
            nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::TemporalCRS>(
          parser.createFromWKT(
              "TIMECRS[\"TimeCount\",TDATUM[\"T\",TIMEORIGIN[\"2000-01-01\"]],"
              "CS[TemporalCount,1],AXIS[\"t\",future,ORDER[1]]]")),
      nullptr);

  // Export and re-parse Derived CRSs + CoordinateMetadata + Operations.
  auto eng_base = pj_crs::EngineeringCRS::create(
      NameProps("BaseEng"), pj_datum::EngineeringDatum::create(NameProps("ED")),
      pj_cs::CartesianCS::createEastingNorthing(
          pj_common::UnitOfMeasure::METRE));
  auto noop_conv = pj_op::Conversion::create(NameProps("Identity"),
                                             NameProps("Identity"), {}, {});
  auto derived_eng = pj_crs::DerivedEngineeringCRS::create(
      NameProps("DerEng"), eng_base, noop_conv,
      pj_cs::CartesianCS::createEastingNorthing(
          pj_common::UnitOfMeasure::METRE));
  auto f_wkt =
      pj_io::WKTFormatter::create(pj_io::WKTFormatter::Convention::WKT2_2019);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedEngineeringCRS>(
                parser.createFromWKT(derived_eng->exportToWKT(f_wkt.get()))),
            nullptr);

  auto cm = pj_coord::CoordinateMetadata::create(
      pj_crs::GeographicCRS::EPSG_4326, 2021.0);
  auto f_cm =
      pj_io::WKTFormatter::create(pj_io::WKTFormatter::Convention::WKT2_2019);
  auto parsed_cm =
      pj_util::nn_dynamic_pointer_cast<pj_coord::CoordinateMetadata>(
          parser.createFromWKT(cm->exportToWKT(f_cm.get())));
  ASSERT_NE(parsed_cm, nullptr);
  EXPECT_TRUE(parsed_cm->coordinateEpoch().has_value());

  // PROJBasedOperation wrapped in COORDINATEOPERATION / CONVERSION.
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_op::PROJBasedOperation>(
                parser.createFromWKT(
                    "CONVERSION[\"PBO\",METHOD[\"PROJ-based operation method: "
                    "+proj=noop\"]]")),
            nullptr);

  // Strict error branches.
  pj_io::WKTParser strict_parser;
  EXPECT_THROW(strict_parser.createFromWKT("UNKNOWNKEYWORD[\"x\"]"),
               pj_io::ParsingException);
  EXPECT_THROW(strict_parser.createFromWKT("ELLIPSOID[\"x\"]"),
               pj_io::ParsingException);
  EXPECT_THROW(
      strict_parser.createFromWKT(
          "GEOGCS[\"x\",DATUM[\"d\",SPHEROID[\"s\",6378137,298.25],TOWGS84[1,2]"
          "],"
          "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]"),
      pj_io::ParsingException);
}

// =============================================================================
// Suite 4: JSONFormatterAndParserTest
// =============================================================================

TEST(JSONFormatterAndParserTest, FormatterOptionsAndAllJSONParserTypes) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  // Formatter setters and writer direct access.
  {
    auto jf = pj_io::JSONFormatter::create(db.as_nullable());
    jf->setMultiLine(false).setIndentationWidth(2).setSchema(std::string());
    jf->setAbridgedTransformation(true);
    EXPECT_TRUE(jf->abridgedTransformation());
    jf->setAbridgedTransformationWriteSourceCRS(true);
    EXPECT_TRUE(jf->abridgedTransformationWriteSourceCRS());
    EXPECT_EQ(jf->databaseContext(), db.as_nullable());
    EXPECT_TRUE(jf->outputId());
    EXPECT_FALSE(jf->outputUsage());
    ASSERT_NE(jf->writer(), nullptr);

    const std::string json_str =
        pj_crs::GeographicCRS::EPSG_4326->exportToJSON(jf.get());
    EXPECT_THAT(json_str, Not(HasSubstr("$schema")));
    auto parsed = pj_io::createFromUserInput(json_str, db.as_nullable());
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(parsed),
              nullptr);
  }

  // Round-trip complex objects through JSONFormatter -> createFromUserInput.
  {
    const auto roundTrip = [&](const pj_io::IJSONExportable& obj) {
      auto jf = pj_io::JSONFormatter::create(db.as_nullable());
      return pj_io::createFromUserInput(obj.exportToJSON(jf.get()),
                                        db.as_nullable());
    };

    auto dyn_datum = pj_datum::DynamicGeodeticReferenceFrame::create(
        NameProps("DynF"), pj_datum::Ellipsoid::GRS1980,
        pj_util::optional<std::string>("Dyn anchor"),
        pj_datum::PrimeMeridian::GREENWICH,
        pj_common::Measure(2015.0, pj_common::UnitOfMeasure::YEAR),
        pj_util::optional<std::string>("VelModel"));
    auto dyn_geo = pj_crs::GeographicCRS::create(
        NameProps("DynGeo"), dyn_datum,
        pj_cs::EllipsoidalCS::createLatitudeLongitude(
            pj_common::UnitOfMeasure::DEGREE));
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                  roundTrip(*dyn_geo)),
              nullptr);

    auto utm = MakeUtmProjectedCRS(pj_crs::GeographicCRS::EPSG_4326, 31, true);
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(roundTrip(*utm)),
        nullptr);

    auto dyn_vdatum = pj_datum::DynamicVerticalReferenceFrame::create(
        NameProps("DynV"), pj_util::optional<std::string>(),
        pj_util::optional<pj_datum::RealizationMethod>(
            pj_datum::RealizationMethod::GEOID),
        pj_common::Measure(2020.0, pj_common::UnitOfMeasure::YEAR),
        pj_util::optional<std::string>("VVel"));
    auto vert_crs = pj_crs::VerticalCRS::create(
        NameProps("DynVert"), dyn_vdatum,
        pj_cs::VerticalCS::createGravityRelatedHeight(
            pj_common::UnitOfMeasure::METRE));
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::VerticalCRS>(
                  roundTrip(*vert_crs)),
              nullptr);

    auto compound = pj_crs::CompoundCRS::create(
        NameProps("Comp"), {pj_crs::GeographicCRS::EPSG_4326, vert_crs});
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(
                  roundTrip(*compound)),
              nullptr);

    auto tr1 = pj_op::Transformation::createGeocentricTranslations(
        NameProps("Tr1"), pj_crs::GeographicCRS::EPSG_4326,
        pj_crs::GeographicCRS::EPSG_4269, 1.0, 2.0, 3.0,
        {pj_meta::PositionalAccuracy::create("1.0")});
    auto tr2 = pj_op::Transformation::createGeocentricTranslations(
        NameProps("Tr2"), pj_crs::GeographicCRS::EPSG_4269,
        pj_crs::GeographicCRS::EPSG_4267, -1.0, 0.0, 1.0, {});
    auto concat = pj_op::ConcatenatedOperation::create(NameProps("Concat"),
                                                       {tr1, tr2}, {});
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_op::ConcatenatedOperation>(
                  roundTrip(*concat)),
              nullptr);

    auto pmo = pj_op::PointMotionOperation::create(
        NameProps("PMO"), pj_crs::GeodeticCRS::EPSG_4978, NameProps("Method"),
        {pj_op::OperationParameter::create(NameProps("dX"))},
        {pj_op::ParameterValue::create(pj_common::Measure(
            0.01, pj_common::UnitOfMeasure::METRE_PER_YEAR))},
        {pj_meta::PositionalAccuracy::create("0.2")});
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_op::PointMotionOperation>(
                  roundTrip(*pmo)),
              nullptr);

    auto cm = pj_coord::CoordinateMetadata::create(
        pj_crs::GeographicCRS::EPSG_4326, 2022.5);
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_coord::CoordinateMetadata>(
                  roundTrip(*cm)),
              nullptr);

    auto bound = pj_crs::BoundCRS::createFromTOWGS84(
        pj_crs::GeographicCRS::EPSG_4267, {1, 2, 3, 0, 0, 0, 0});
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_crs::BoundCRS>(roundTrip(*bound)),
        nullptr);

    // DerivedGeodeticCRS, DerivedProjectedCRS, DerivedVerticalCRS,
    // DerivedEngineeringCRS, DerivedParametricCRS, DerivedTemporalCRS.
    auto noop_conv = pj_op::Conversion::create(NameProps("Ident"),
                                               NameProps("Ident"), {}, {});
    auto der_geo = pj_crs::DerivedGeographicCRS::create(
        NameProps("DerGeo"), pj_crs::GeographicCRS::EPSG_4326, noop_conv,
        pj_cs::EllipsoidalCS::createLatitudeLongitude(
            pj_common::UnitOfMeasure::DEGREE));
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedGeographicCRS>(
                  roundTrip(*der_geo)),
              nullptr);

    auto der_geod = pj_crs::DerivedGeodeticCRS::create(
        NameProps("DerGeod"), pj_crs::GeodeticCRS::EPSG_4978, noop_conv,
        pj_cs::CartesianCS::createGeocentric(pj_common::UnitOfMeasure::METRE));
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedGeodeticCRS>(
                  roundTrip(*der_geod)),
              nullptr);

    auto der_proj = pj_crs::DerivedProjectedCRS::create(
        NameProps("DerProj"), utm, noop_conv,
        pj_cs::CartesianCS::createEastingNorthing(
            pj_common::UnitOfMeasure::METRE));
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedProjectedCRS>(
                  roundTrip(*der_proj)),
              nullptr);

    auto der_vert = pj_crs::DerivedVerticalCRS::create(
        NameProps("DerVert"), vert_crs, noop_conv,
        pj_cs::VerticalCS::createGravityRelatedHeight(
            pj_common::UnitOfMeasure::METRE));
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedVerticalCRS>(
                  roundTrip(*der_vert)),
              nullptr);

    auto eng_crs = pj_crs::EngineeringCRS::create(
        NameProps("Eng"), pj_datum::EngineeringDatum::create(NameProps("ED")),
        pj_cs::CartesianCS::createEastingNorthing(
            pj_common::UnitOfMeasure::METRE));
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::EngineeringCRS>(
                  roundTrip(*eng_crs)),
              nullptr);
    auto der_eng = pj_crs::DerivedEngineeringCRS::create(
        NameProps("DerEng"), eng_crs, noop_conv,
        pj_cs::CartesianCS::createEastingNorthing(
            pj_common::UnitOfMeasure::METRE));
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedEngineeringCRS>(
                  roundTrip(*der_eng)),
              nullptr);

    auto p_unit = pj_common::UnitOfMeasure(
        "hPa", 100.0, pj_common::UnitOfMeasure::Type::PARAMETRIC);
    auto p_cs = pj_cs::ParametricCS::create(
        NameProps("PCS"),
        pj_cs::CoordinateSystemAxis::create(NameProps("P"), "P",
                                            pj_cs::AxisDirection::UP, p_unit));
    auto param_crs = pj_crs::ParametricCRS::create(
        NameProps("Param"), pj_datum::ParametricDatum::create(NameProps("PD")),
        p_cs);
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ParametricCRS>(
                  roundTrip(*param_crs)),
              nullptr);
    auto der_param = pj_crs::DerivedParametricCRS::create(
        NameProps("DerParam"), param_crs, noop_conv, p_cs);
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedParametricCRS>(
                  roundTrip(*der_param)),
              nullptr);

    auto t_cs = pj_cs::TemporalMeasureCS::create(
        NameProps("TCS"), pj_cs::CoordinateSystemAxis::create(
                              NameProps("t"), "t", pj_cs::AxisDirection::FUTURE,
                              pj_common::UnitOfMeasure::YEAR));
    auto time_crs = pj_crs::TemporalCRS::create(
        NameProps("Time"),
        pj_datum::TemporalDatum::create(
            NameProps("TD"), pj_common::DateTime::create("2000-01-01"),
            pj_datum::TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN),
        t_cs);
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::TemporalCRS>(
                  roundTrip(*time_crs)),
              nullptr);
    auto der_time = pj_crs::DerivedTemporalCRS::create(
        NameProps("DerTime"), time_crs, noop_conv, t_cs);
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedTemporalCRS>(
                  roundTrip(*der_time)),
              nullptr);
  }

  // Hand-crafted JSON covering standalone types, DatumEnsemble fallbacks,
  // CoordinateSystem subtypes + mixed-unit warning, usages, and error cases.
  {
    const std::string ell_sphere = R"json({
      "type": "Ellipsoid",
      "name": "CustomSphere",
      "radius": {"value": 6371, "unit": {"type": "LinearUnit", "name": "km", "conversion_factor": 1000}},
      "ids": [{"authority": "IAU", "code": "39900", "version": 2015,
               "authority_citation": "IAU2015", "uri": "https://example.org"}]
    })json";
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::Ellipsoid>(
                  pj_io::createFromUserInput(ell_sphere, db.as_nullable())),
              nullptr);

    const std::string ell_two_axis = R"json({
      "type": "Ellipsoid",
      "name": "TwoAxis",
      "semi_major_axis": 6378137,
      "semi_minor_axis": 6356752.314245179
    })json";
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::Ellipsoid>(
                  pj_io::createFromUserInput(ell_two_axis, db.as_nullable())),
              nullptr);

    const std::string pm_json = R"json({
      "type": "PrimeMeridian",
      "name": "CustomPM",
      "longitude": {"value": 2.5, "unit": "degree"}
    })json";
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::PrimeMeridian>(
                  pj_io::createFromUserInput(pm_json, db.as_nullable())),
              nullptr);

    // DatumEnsemble fallback with explicit members + DB member + DB lookup by
    // name.
    const std::string ens_members_json = R"json({
      "type": "DatumEnsemble",
      "name": "CustomEns",
      "accuracy": "2.0",
      "ellipsoid": {"name": "WGS 84", "semi_major_axis": 6378137, "inverse_flattening": 298.257223563},
      "members": [
        {"name": "WGS 84 (G1762)", "id": {"authority": "EPSG", "code": 1153}},
        {"name": "Fallback Member"}
      ],
      "usages": [{
        "scope": "Global",
        "area": "World",
        "bbox": {"south_latitude": -90, "west_longitude": -180, "north_latitude": 90, "east_longitude": 180},
        "vertical_extent": {"minimum": -100, "maximum": 1000, "unit": "metre"},
        "temporal_extent": {"start": "2020-01-01", "end": "2025-01-01"}
      }],
      "remarks": "Ens note"
    })json";
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_datum::DatumEnsemble>(
            pj_io::createFromUserInput(ens_members_json, db.as_nullable())),
        nullptr);

    // DatumEnsemble without members falls back to DB lookup by name.
    const std::string ens_db_lookup = R"json({
      "type": "DatumEnsemble",
      "name": "World Geodetic System 1984 ensemble",
      "accuracy": "2.0"
    })json";
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::DatumEnsemble>(
                  pj_io::createFromUserInput(ens_db_lookup, db.as_nullable())),
              nullptr);
    EXPECT_THROW(pj_io::createFromUserInput(ens_db_lookup, nullptr),
                 pj_io::ParsingException);

    // Mixed-unit CartesianCS emits debug warning through PJ_CONTEXT.
    ScopedLogCapture log_cap(ctx.get(), PJ_LOG_DEBUG_MAJOR);
    const std::string mixed_cs_json = R"json({
      "type": "CoordinateSystem",
      "name": "Mixed",
      "subtype": "Cartesian",
      "axis": [
        {"name": "Easting", "abbreviation": "E", "direction": "east", "unit": "metre"},
        {"name": "Northing", "abbreviation": "N", "direction": "north", "unit": {"type": "LinearUnit", "name": "foot", "conversion_factor": 0.3048}}
      ]
    })json";
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_cs::CartesianCS>(
                  pj_io::createFromUserInput(mixed_cs_json, ctx.get())),
              nullptr);
    EXPECT_TRUE(log_cap.ContainsMessage("same unit"));

    // CoordinateSystem with affine, ordinal, range_meaning, min/max, meridian.
    const std::string affine_cs_json = R"json({
      "type": "CoordinateSystem",
      "name": "Aff",
      "subtype": "affine",
      "axis": [
        {"name": "Col", "abbreviation": "i", "direction": "columnPositive",
         "unit": "unity", "minimum_value": 0, "maximum_value": 100,
         "range_meaning": "exact", "meridian": {"longitude": 10}},
        {"name": "Row", "abbreviation": "j", "direction": "rowPositive"}
      ]
    })json";
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_cs::AffineCS>(
                  pj_io::createFromUserInput(affine_cs_json, db.as_nullable())),
              nullptr);

    // UTM South pre-9.5 Conversion ID autofix (only when "UTM Zone ...S") and
    // Inverse Conversion ("Inverse of ..." + INVERSE(EPSG)).
    const std::string utm_s_conv_json = R"json({
      "type": "Conversion",
      "name": "UTM Zone 31S",
      "method": {"name": "Transverse Mercator"},
      "parameters": [
        {"name": "Latitude of natural origin", "value": 0, "unit": "degree"},
        {"name": "Scale factor at natural origin", "value": 0.9996, "unit": "unity"}
      ],
      "id": {"authority": "EPSG", "code": 16031}
    })json";
    auto utm_s_conv = pj_util::nn_dynamic_pointer_cast<pj_op::Conversion>(
        pj_io::createFromUserInput(utm_s_conv_json, db.as_nullable()));
    ASSERT_NE(utm_s_conv, nullptr);
    ASSERT_FALSE(utm_s_conv->identifiers().empty());
    EXPECT_EQ(utm_s_conv->identifiers()[0]->code(), "16131");

    const std::string inv_conv_json = R"json({
      "type": "Conversion",
      "name": "Inverse of UTM Zone 31S",
      "method": {"name": "Inverse of Transverse Mercator"},
      "parameters": [
        {"name": "Latitude of natural origin", "value": 0, "unit": "degree"},
        {"name": "Scale factor at natural origin", "value": 0.9996, "unit": "unity"}
      ],
      "id": {"authority": "INVERSE(EPSG)", "code": 16131}
    })json";
    auto inv_conv = pj_util::nn_dynamic_pointer_cast<pj_op::Conversion>(
        pj_io::createFromUserInput(inv_conv_json, db.as_nullable()));
    ASSERT_NE(inv_conv, nullptr);
    EXPECT_EQ(inv_conv->nameStr(), "Inverse of UTM Zone 31S");

    // BoundCRS with explicit transformation.source_crs, VerticalCRS with
    // geoid_model / geoid_models + interpolation_crs + deformation_models,
    // legacy deformation_model, Vertical DatumEnsemble without ellipsoid,
    // and standalone Axis.
    const std::string bound_explicit_src = R"json({
      "type": "BoundCRS",
      "source_crs": {
        "type": "GeographicCRS",
        "name": "Src",
        "datum": {"type": "GeodeticReferenceFrame", "name": "D",
                  "ellipsoid": {"name": "WGS 84", "semi_major_axis": 6378137, "inverse_flattening": 298.257223563}},
        "coordinate_system": {"subtype": "ellipsoidal", "axis": [
          {"name": "Lat", "abbreviation": "lat", "direction": "north", "unit": "degree"},
          {"name": "Lon", "abbreviation": "lon", "direction": "east", "unit": "degree"}
        ]}
      },
      "target_crs": {
        "type": "GeographicCRS",
        "name": "Tgt",
        "datum": {"type": "GeodeticReferenceFrame", "name": "WGS 84",
                  "ellipsoid": {"name": "WGS 84", "semi_major_axis": 6378137, "inverse_flattening": 298.257223563}},
        "coordinate_system": {"subtype": "ellipsoidal", "axis": [
          {"name": "Lat", "abbreviation": "lat", "direction": "north", "unit": "degree"},
          {"name": "Lon", "abbreviation": "lon", "direction": "east", "unit": "degree"}
        ]}
      },
      "transformation": {
        "name": "ExplicitSrcTr",
        "source_crs": {
          "type": "GeographicCRS",
          "name": "TrSrc",
          "datum": {"type": "GeodeticReferenceFrame", "name": "D",
                    "ellipsoid": {"name": "WGS 84", "semi_major_axis": 6378137, "inverse_flattening": 298.257223563}},
          "coordinate_system": {"subtype": "ellipsoidal", "axis": [
            {"name": "Lat", "abbreviation": "lat", "direction": "north", "unit": "degree"},
            {"name": "Lon", "abbreviation": "lon", "direction": "east", "unit": "degree"}
          ]}
        },
        "method": {"name": "NTv2"},
        "parameters": [
          {"name": "Latitude and longitude difference file", "value": "custom.gsb"}
        ]
      }
    })json";
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_crs::BoundCRS>(
            pj_io::createFromUserInput(bound_explicit_src, db.as_nullable())),
        nullptr);

    const std::string vert_geoids_json = R"json({
      "type": "VerticalCRS",
      "name": "VertGeoids",
      "datum": {
        "type": "DynamicVerticalReferenceFrame",
        "name": "DynV",
        "frame_reference_epoch": 2015.0,
        "deformation_model": "LegacyVVel"
      },
      "coordinate_system": {"subtype": "vertical", "axis": [
        {"name": "Gravity-related height", "abbreviation": "H", "direction": "up", "unit": "metre"}
      ]},
      "geoid_models": [{
        "name": "GEOID18",
        "interpolation_crs": {
          "type": "GeographicCRS",
          "name": "NAD83",
          "datum": {"type": "GeodeticReferenceFrame", "name": "NAD83",
                    "ellipsoid": {"name": "GRS 1980", "semi_major_axis": 6378137, "inverse_flattening": 298.257222101}},
          "coordinate_system": {"subtype": "ellipsoidal", "axis": [
            {"name": "Lat", "abbreviation": "lat", "direction": "north", "unit": "degree"},
            {"name": "Lon", "abbreviation": "lon", "direction": "east", "unit": "degree"}
          ]}
        }
      }]
    })json";
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_crs::VerticalCRS>(
            pj_io::createFromUserInput(vert_geoids_json, db.as_nullable())),
        nullptr);

    const std::string vert_geoid_single = R"json({
      "type": "VerticalCRS",
      "name": "VertSingleGeoid",
      "datum_ensemble": {
        "name": "VertEns",
        "accuracy": "0.5",
        "members": [{"name": "VertMemberFallback"}, {"name": "VertMemberFallback2"}]
      },
      "coordinate_system": {"subtype": "vertical", "axis": [
        {"name": "Gravity-related height", "abbreviation": "H", "direction": "up", "unit": "metre"}
      ]},
      "geoid_model": {"name": "EGM96"}
    })json";
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_crs::VerticalCRS>(
            pj_io::createFromUserInput(vert_geoid_single, db.as_nullable())),
        nullptr);

    const std::string dyn_geod_legacy = R"json({
      "type": "GeographicCRS",
      "name": "DynGeoLegacy",
      "datum": {
        "type": "DynamicGeodeticReferenceFrame",
        "name": "DynG",
        "ellipsoid": {"name": "GRS 1980", "semi_major_axis": 6378137, "inverse_flattening": 298.257222101},
        "frame_reference_epoch": 2010.0,
        "deformation_model": "LegacyVel"
      },
      "coordinate_system": {"subtype": "ellipsoidal", "axis": [
        {"name": "Lat", "abbreviation": "lat", "direction": "north", "unit": "degree"},
        {"name": "Lon", "abbreviation": "lon", "direction": "east", "unit": "degree"}
      ]}
    })json";
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
            pj_io::createFromUserInput(dyn_geod_legacy, db.as_nullable())),
        nullptr);

    const std::string axis_json = R"json({
      "type": "Axis",
      "name": "Easting",
      "abbreviation": "E",
      "direction": "east",
      "unit": "metre"
    })json";
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_cs::CoordinateSystemAxis>(
                  pj_io::createFromUserInput(axis_json, db.as_nullable())),
              nullptr);

    // JSON error branches: 2D Cartesian GeodeticCRS, bad GeodeticCRS CS,
    // array root, depth >= 128, unknown type.
    const std::string bad_geod_2d = R"json({
      "type": "GeodeticCRS",
      "name": "Bad2D",
      "datum": {"type": "GeodeticReferenceFrame", "name": "D",
                "ellipsoid": {"name": "WGS 84", "semi_major_axis": 6378137, "inverse_flattening": 298.257223563}},
      "coordinate_system": {"subtype": "Cartesian", "axis": [
        {"name": "X", "abbreviation": "X", "direction": "geocentricX", "unit": "metre"},
        {"name": "Y", "abbreviation": "Y", "direction": "geocentricY", "unit": "metre"}
      ]}
    })json";
    EXPECT_THROW(pj_io::createFromUserInput(bad_geod_2d, db.as_nullable()),
                 pj_io::ParsingException);

    const std::string bad_geod_vert = R"json({
      "type": "GeodeticCRS",
      "name": "BadCS",
      "datum": {"type": "GeodeticReferenceFrame", "name": "D",
                "ellipsoid": {"name": "WGS 84", "semi_major_axis": 6378137, "inverse_flattening": 298.257223563}},
      "coordinate_system": {"subtype": "vertical", "axis": [
        {"name": "H", "abbreviation": "H", "direction": "up", "unit": "metre"}
      ]}
    })json";
    EXPECT_THROW(pj_io::createFromUserInput(bad_geod_vert, db.as_nullable()),
                 pj_io::ParsingException);

    EXPECT_THROW(pj_io::createFromUserInput("[1, 2]", db.as_nullable()),
                 pj_io::ParsingException);
    EXPECT_THROW(
        pj_io::createFromUserInput("{\"type\":\"Bogus\"}", db.as_nullable()),
        pj_io::ParsingException);
    std::string deep_json;
    for (int i = 0; i < 130; ++i) {
      deep_json += "{\"a\":";
    }
    deep_json += "1";
    for (int i = 0; i < 130; ++i) {
      deep_json += "}";
    }
    EXPECT_THROW(pj_io::createFromUserInput(deep_json, db.as_nullable()),
                 pj_io::ParsingException);
  }
}

TEST(WKTParserTest, RoundTripDerivedCRSsAndOperationsThroughWKT) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  pj_io::WKTParser parser;
  parser.attachDatabaseContext(db.as_nullable()).setStrict(false);

  const auto roundTripWKT = [&](const pj_io::IWKTExportable& obj) {
    auto f = pj_io::WKTFormatter::create(
        pj_io::WKTFormatter::Convention::WKT2_2019, db.as_nullable());
    return parser.createFromWKT(obj.exportToWKT(f.get()));
  };

  auto noop_conv = pj_op::Conversion::create(NameProps("Identity"),
                                             NameProps("Identity"), {}, {});

  auto der_geod = pj_crs::DerivedGeodeticCRS::create(
      NameProps("DerGeod"), pj_crs::GeodeticCRS::EPSG_4978, noop_conv,
      pj_cs::CartesianCS::createGeocentric(pj_common::UnitOfMeasure::METRE));
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedGeodeticCRS>(
                roundTripWKT(*der_geod)),
            nullptr);

  auto der_geo = pj_crs::DerivedGeographicCRS::create(
      NameProps("DerGeo"), pj_crs::GeographicCRS::EPSG_4326, noop_conv,
      pj_cs::EllipsoidalCS::createLatitudeLongitude(
          pj_common::UnitOfMeasure::DEGREE));
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedGeographicCRS>(
                roundTripWKT(*der_geo)),
            nullptr);

  auto utm = MakeUtmProjectedCRS(pj_crs::GeographicCRS::EPSG_4326, 31, true);
  auto der_proj = pj_crs::DerivedProjectedCRS::create(
      NameProps("DerProj"), utm, noop_conv,
      pj_cs::CartesianCS::createEastingNorthing(
          pj_common::UnitOfMeasure::METRE));
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedProjectedCRS>(
                roundTripWKT(*der_proj)),
            nullptr);

  auto v_crs = pj_crs::VerticalCRS::create(
      NameProps("Vert"),
      pj_datum::VerticalReferenceFrame::create(NameProps("VDatum")),
      pj_cs::VerticalCS::createGravityRelatedHeight(
          pj_common::UnitOfMeasure::METRE));
  auto der_vert = pj_crs::DerivedVerticalCRS::create(
      NameProps("DerVert"), v_crs, noop_conv,
      pj_cs::VerticalCS::createGravityRelatedHeight(
          pj_common::UnitOfMeasure::METRE));
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedVerticalCRS>(
                roundTripWKT(*der_vert)),
            nullptr);

  auto p_unit = pj_common::UnitOfMeasure(
      "hPa", 100.0, pj_common::UnitOfMeasure::Type::PARAMETRIC);
  auto p_cs = pj_cs::ParametricCS::create(
      NameProps("PCS"),
      pj_cs::CoordinateSystemAxis::create(NameProps("P"), "P",
                                          pj_cs::AxisDirection::UP, p_unit));
  auto param_crs = pj_crs::ParametricCRS::create(
      NameProps("Param"), pj_datum::ParametricDatum::create(NameProps("PD")),
      p_cs);
  auto der_param = pj_crs::DerivedParametricCRS::create(
      NameProps("DerParam"), param_crs, noop_conv, p_cs);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedParametricCRS>(
                roundTripWKT(*der_param)),
            nullptr);

  auto t_cs = pj_cs::TemporalMeasureCS::create(
      NameProps("TCS"), pj_cs::CoordinateSystemAxis::create(
                            NameProps("t"), "t", pj_cs::AxisDirection::FUTURE,
                            pj_common::UnitOfMeasure::YEAR));
  auto time_crs = pj_crs::TemporalCRS::create(
      NameProps("Time"),
      pj_datum::TemporalDatum::create(
          NameProps("TD"), pj_common::DateTime::create("2000-01-01"),
          pj_datum::TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN),
      t_cs);
  auto der_time = pj_crs::DerivedTemporalCRS::create(NameProps("DerTime"),
                                                     time_crs, noop_conv, t_cs);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedTemporalCRS>(
                roundTripWKT(*der_time)),
            nullptr);

  auto tr1 = pj_op::Transformation::createGeocentricTranslations(
      NameProps("Tr1"), pj_crs::GeographicCRS::EPSG_4326,
      pj_crs::GeographicCRS::EPSG_4269, 1.0, 2.0, 3.0,
      {pj_meta::PositionalAccuracy::create("0.5")});
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_op::Transformation>(
                roundTripWKT(*tr1)),
            nullptr);

  auto tr2 = pj_op::Transformation::createGeocentricTranslations(
      NameProps("Tr2"), pj_crs::GeographicCRS::EPSG_4269,
      pj_crs::GeographicCRS::EPSG_4267, -1.0, 0.0, 1.0, {});
  auto concat = pj_op::ConcatenatedOperation::create(
      NameProps("ConcatOp"), {tr1, tr2},
      {pj_meta::PositionalAccuracy::create("1.5")});
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_op::ConcatenatedOperation>(
                roundTripWKT(*concat)),
            nullptr);

  auto pmo = pj_op::PointMotionOperation::create(
      NameProps("PMO"), pj_crs::GeodeticCRS::EPSG_4978, NameProps("VelMethod"),
      {pj_op::OperationParameter::create(NameProps("X-axis velocity"))},
      {pj_op::ParameterValue::create(
          pj_common::Measure(0.01, pj_common::UnitOfMeasure::METRE_PER_YEAR))},
      {pj_meta::PositionalAccuracy::create("0.2")});
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_op::PointMotionOperation>(
                roundTripWKT(*pmo)),
            nullptr);

  auto approx_pbo = parser.createFromWKT(
      "COORDINATEOPERATION[\"ApproxPBO\","
      "SOURCECRS[GEOGCRS[\"WGS 84\",DATUM[\"WGS84\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],AXIS[\"lat\",north,ORDER[1]],"
      "AXIS[\"lon\",east,ORDER[2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
      "TARGETCRS[GEOGCRS[\"WGS 84\",DATUM[\"WGS84\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],AXIS[\"lat\",north,ORDER[1]],"
      "AXIS[\"lon\",east,ORDER[2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
      "METHOD[\"PROJ-based operation method (approximate): +proj=noop\"]]");
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_op::PROJBasedOperation>(approx_pbo),
      nullptr);
}

// =============================================================================
// Suite 5: CreateFromUserInputTest
// =============================================================================

TEST(CreateFromUserInputTest, ContextOverloadWhitespaceAndEpochs) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  // PJ_CONTEXT* overload connects to database when needed and skips when not.
  auto from_epsg = pj_io::createFromUserInput("  \t\n EPSG:4326", ctx.get());
  auto geo = pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(from_epsg);
  ASSERT_NE(geo, nullptr);
  EXPECT_EQ(geo->nameStr(), "WGS 84");

  auto from_proj = pj_io::createFromUserInput(
      "+proj=longlat +datum=WGS84 +type=crs", ctx.get());
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(from_proj),
            nullptr);

  auto from_title_proj = pj_io::createFromUserInput(
      "title=Custom Title +proj=longlat +datum=WGS84 +type=crs",
      db.as_nullable());
  auto titled =
      pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(from_title_proj);
  ASSERT_NE(titled, nullptr);
  EXPECT_EQ(titled->nameStr(), "Custom");

  auto step_pbo =
      pj_io::createFromUserInput("step +proj=noop", db.as_nullable());
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_op::PROJBasedOperation>(step_pbo),
      nullptr);

  // Coordinate epoch suffix with '@'.
  auto cm_obj =
      pj_io::createFromUserInput("EPSG:4326@2025.0", db.as_nullable());
  auto cm =
      pj_util::nn_dynamic_pointer_cast<pj_coord::CoordinateMetadata>(cm_obj);
  ASSERT_NE(cm, nullptr);
  ASSERT_TRUE(cm->coordinateEpoch().has_value());
  EXPECT_DOUBLE_EQ(cm->coordinateEpoch()->coordinateEpoch().value(), 2025.0);

  auto cm_name =
      pj_io::createFromUserInput("WGS 84 @ 2020.5", db.as_nullable());
  auto cm_name_cast =
      pj_util::nn_dynamic_pointer_cast<pj_coord::CoordinateMetadata>(cm_name);
  ASSERT_NE(cm_name_cast, nullptr);
  ASSERT_TRUE(cm_name_cast->coordinateEpoch().has_value());
  EXPECT_DOUBLE_EQ(cm_name_cast->coordinateEpoch()->coordinateEpoch().value(),
                   2020.5);

  EXPECT_THROW(
      pj_io::createFromUserInput("EPSG:4326@notanumber", db.as_nullable()),
      pj_io::ParsingException);
  EXPECT_THROW(pj_io::createFromUserInput("", db.as_nullable()),
               pj_io::ParsingException);
}

TEST(CreateFromUserInputTest, OGCURLsWMSAUTOAuthCodesAndURNs) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  // OGC CRS URLs (single, lowercase fallback, compound, and error cases).
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
          pj_io::createFromUserInput(
              "http://www.opengis.net/def/crs/EPSG/0/4326", db.as_nullable())),
      nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
          pj_io::createFromUserInput("https://opengis.net/def/crs/EPSG/0/32631",
                                     db.as_nullable())),
      nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                pj_io::createFromUserInput(
                    "www.opengis.net/def/crs/EPSG/0/4326", db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(
                pj_io::createFromUserInput(
                    "http://www.opengis.net/def/crs-compound?"
                    "1=http://www.opengis.net/def/crs/EPSG/0/4326&"
                    "2=http://www.opengis.net/def/crs/EPSG/0/3855",
                    db.as_nullable())),
            nullptr);

  // Versioned authority resolution in OGC URL / URN if present in bundled DB.
  const auto iau_versions = db->getVersionedAuthoritiesFromName("IAU");
  if (!iau_versions.empty()) {
    const auto auth = iau_versions.front();
    const auto pos = auth.find('_');
    if (pos != std::string::npos) {
      const std::string ver = auth.substr(pos + 1);
      EXPECT_THROW(pj_io::createFromUserInput(
                       "http://www.opengis.net/def/crs/IAU/" + ver + "/999999",
                       db.as_nullable()),
                   pj_util::Exception);
      EXPECT_THROW(
          pj_io::createFromUserInput("urn:ogc:def:crs:IAU:" + ver + ":999999",
                                     db.as_nullable()),
          pj_util::Exception);
    }
  }

  EXPECT_THROW(pj_io::createFromUserInput("http://www.opengis.net/def/crs/EPSG",
                                          db.as_nullable()),
               pj_io::ParsingException);
  EXPECT_THROW(
      pj_io::createFromUserInput("http://www.opengis.net/def/crs-compound?1",
                                 db.as_nullable()),
      pj_io::ParsingException);
  EXPECT_THROW(
      pj_io::createFromUserInput(
          "http://www.opengis.net/def/crs-compound?abc=def", db.as_nullable()),
      pj_io::ParsingException);
  EXPECT_THROW(
      pj_io::createFromUserInput("http://www.opengis.net/def/crs-compound?"
                                 "2=http://www.opengis.net/def/crs/EPSG/0/4326",
                                 db.as_nullable()),
      pj_io::ParsingException);

  // WMS AUTO codes (with and without explicit units code, URN form, errors).
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
                pj_io::createFromUserInput("AUTO:42001,9001,-117,33",
                                           db.as_nullable())),
            nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
          pj_io::createFromUserInput("AUTO:42001,-117,-33", db.as_nullable())),
      nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
                pj_io::createFromUserInput("AUTO:42002,9002,-117,33",
                                           db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
                pj_io::createFromUserInput("AUTO:42003,9003,10,45",
                                           db.as_nullable())),
            nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
          pj_io::createFromUserInput("AUTO:42004,10,45", db.as_nullable())),
      nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
          pj_io::createFromUserInput("AUTO:42005,9001,15", db.as_nullable())),
      nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
                pj_io::createFromUserInput("AUTO:42005,15", db.as_nullable())),
            nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
          pj_io::createFromUserInput("urn:ogc:def:crs:OGC::AUTO42001:-117:33",
                                     db.as_nullable())),
      nullptr);

  EXPECT_THROW(pj_io::createFromUserInput("AUTO:42001,9001,200,33", nullptr),
               pj_io::ParsingException);
  EXPECT_THROW(pj_io::createFromUserInput("AUTO:42099,9001,0,0", nullptr),
               pj_io::ParsingException);
  EXPECT_THROW(pj_io::createFromUserInput("AUTO:42001,9999,0,0", nullptr),
               pj_io::ParsingException);
  EXPECT_THROW(pj_io::createFromUserInput("AUTO:42001,9001", nullptr),
               pj_io::ParsingException);
  EXPECT_THROW(pj_io::createFromUserInput("AUTO:42001,abc,0,0", nullptr),
               pj_io::ParsingException);

  // Authority:Code including EPSG:102100 convenience, case-insensitive,
  // abbreviated compound codes, and null-db errors.
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
                pj_io::createFromUserInput("EPSG:102100", db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                pj_io::createFromUserInput("epsg:4326", db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(
                pj_io::createFromUserInput("EPSG:2393+5717", db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(
                pj_io::createFromUserInput("ESRI:103668+EPSG:5703",
                                           db.as_nullable())),
            nullptr);
  EXPECT_THROW(pj_io::createFromUserInput("EPSG:4326", nullptr),
               pj_io::ParsingException);
  EXPECT_THROW(pj_io::createFromUserInput("ESRI:103668+EPSG:5703", nullptr),
               pj_io::ParsingException);

  // Combined and 7-part / 6-part legacy URNs.
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
                pj_io::createFromUserInput(
                    "urn:ogc:def:crs,crs:EPSG::4979,cs:PROJ::ENh,"
                    "coordinateOperation:EPSG::16031",
                    db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(
                pj_io::createFromUserInput(
                    "urn:ogc:def:crs,crs:EPSG::2393,crs:EPSG::5717",
                    db.as_nullable())),
            nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_op::ConcatenatedOperation>(
          pj_io::createFromUserInput(
              "urn:ogc:def:coordinateOperation,"
              "coordinateOperation:EPSG::3895,coordinateOperation:EPSG::1618",
              db.as_nullable())),
      nullptr);

  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                pj_io::createFromUserInput("urn:ogc:def:crs:EPSG::4326",
                                           db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                pj_io::createFromUserInput("urn:ogc:def:CRS:EPSG::4326",
                                           db.as_nullable())),
            nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_op::CoordinateOperation>(
          pj_io::createFromUserInput(
              "urn:ogc:def:coordinateOperation:EPSG::1671", db.as_nullable())),
      nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::Datum>(
                pj_io::createFromUserInput("urn:ogc:def:datum:EPSG::6326",
                                           db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::DatumEnsemble>(
                pj_io::createFromUserInput("urn:ogc:def:ensemble:EPSG::6326",
                                           db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::Ellipsoid>(
                pj_io::createFromUserInput("urn:ogc:def:ellipsoid:EPSG::7030",
                                           db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::PrimeMeridian>(
                pj_io::createFromUserInput("urn:ogc:def:meridian:EPSG::8901",
                                           db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                pj_io::createFromUserInput("urn:opengis:crs:EPSG:0:4326",
                                           db.as_nullable())),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                pj_io::createFromUserInput("urn:x-ogc:def:crs:EPSG:4326",
                                           db.as_nullable())),
            nullptr);

  // CoordinateMetadata URN dispatch (valid NRCAN if present + error code).
  const auto all_auths = db->getAuthorities();
  if (all_auths.find("NRCAN") != all_auths.end()) {
    try {
      auto nrcan_cm = pj_io::createFromUserInput(
          "urn:ogc:def:coordinateMetadata:NRCAN::NAD83_CSRS_1997_MTM11_HT2_"
          "1997",
          db.as_nullable());
      EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_coord::CoordinateMetadata>(
                    nrcan_cm),
                nullptr);
    } catch (const pj_util::Exception&) {
    }
  }
  EXPECT_THROW(
      pj_io::createFromUserInput("urn:ogc:def:coordinateMetadata:EPSG::999999",
                                 db.as_nullable()),
      pj_util::Exception);

  // URN error branches.
  EXPECT_THROW(pj_io::createFromUserInput("urn:ogc:def:unknownType:EPSG::4326",
                                          db.as_nullable()),
               pj_io::ParsingException);
  EXPECT_THROW(
      pj_io::createFromUserInput("urn:ogc:def:crs:EPSG::4326", nullptr),
      pj_io::ParsingException);
  EXPECT_THROW(
      pj_io::createFromUserInput("urn:ogc:def:crs,crs:EPSG:4326,crs:EPSG::3855",
                                 db.as_nullable()),
      pj_io::ParsingException);
  EXPECT_THROW(pj_io::createFromUserInput("urn:ogc:def:crs,datum:EPSG::6326",
                                          db.as_nullable()),
               pj_io::ParsingException);
  EXPECT_THROW(
      pj_io::createFromUserInput("urn:ogc:def:crs,crs:EPSG::4326,cs:EPSG::4499,"
                                 "coordinateOperation:EPSG::1671",
                                 db.as_nullable()),
      pj_io::ParsingException);
  EXPECT_THROW(
      pj_io::createFromUserInput(
          "urn:ogc:def:coordinateOperation,crs:EPSG::4326", db.as_nullable()),
      pj_io::ParsingException);
  EXPECT_THROW(
      pj_io::createFromUserInput(
          "urn:ogc:def:coordinateOperation,coordinateOperation:EPSG:1671",
          db.as_nullable()),
      pj_io::ParsingException);
}

TEST(CreateFromUserInputTest, ObjectNameLookupAndDisambiguation) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  // Exact CRS match (prefers 2D geographic CRS when 2D & 3D both match name).
  auto wgs84 = pj_io::createFromUserInput("WGS 84", db.as_nullable());
  auto wgs84_geo =
      pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(wgs84);
  ASSERT_NE(wgs84_geo, nullptr);
  EXPECT_EQ(wgs84_geo->coordinateSystem()->axisList().size(), 2u);

  // Compound CRS constructed from " + " separated CRS names.
  auto comp =
      pj_io::createFromUserInput("WGS 84 + EGM96 height", db.as_nullable());
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(comp),
            nullptr);

  // Non-CRS object name lookup (Ellipsoid).
  auto bessel = pj_io::createFromUserInput("Bessel 1841", db.as_nullable());
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_datum::Ellipsoid>(bessel),
            nullptr);

  // Short string (<= 4 chars) suppresses approximate search; ambiguous name
  // and completely unknown name throw ParsingException.
  EXPECT_THROW(pj_io::createFromUserInput("foo", db.as_nullable()),
               pj_io::ParsingException);
  EXPECT_THROW(pj_io::createFromUserInput("WGS 84 / UTM", db.as_nullable()),
               pj_io::ParsingException);
  EXPECT_THROW(
      pj_io::createFromUserInput("NoSuchCRSNameAnywhere", db.as_nullable()),
      pj_io::ParsingException);
}

// =============================================================================
// Suite 6: PROJStringFormatterTest
// =============================================================================

TEST(PROJStringFormatterTest, SettersManualStepsInversionAndGridInspection) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  auto f = pj_io::PROJStringFormatter::create(
      pj_io::PROJStringFormatter::Convention::PROJ_5, db.as_nullable());
  EXPECT_EQ(f->convention(), pj_io::PROJStringFormatter::Convention::PROJ_5);
  EXPECT_EQ(f->databaseContext(), db.as_nullable());
  EXPECT_TRUE(f->getAddNoDefs());
  EXPECT_FALSE(f->getCRSExport());
  EXPECT_FALSE(f->getUseApproxTMerc());
  EXPECT_FALSE(f->isInverted());
  EXPECT_FALSE(f->omitProjLongLatIfPossible());
  EXPECT_FALSE(f->omitZUnitConversion());
  EXPECT_FALSE(f->omitHorizontalConversionInVertTransformation());
  EXPECT_FALSE(f->getLegacyCRSToCRSContext());

  // Exercise empty pipeline -> "+proj=noop".
  EXPECT_EQ(f->toString(), "+proj=noop");

  // Exercise setters/getters and balanced push/pop flags.
  f->addNoDefs(false);
  EXPECT_FALSE(f->getAddNoDefs());
  f->setCRSExport(true);
  EXPECT_TRUE(f->getCRSExport());
  f->setUseApproxTMerc(true);
  EXPECT_TRUE(f->getUseApproxTMerc());
  f->setTOWGS84Parameters({1.0, 2.0, 3.0});
  EXPECT_EQ(f->getTOWGS84Parameters(), (std::vector<double>{1.0, 2.0, 3.0}));
  f->setVDatumExtension("v.gtx", "EPSG:4979");
  EXPECT_EQ(f->getVDatumExtension(), "v.gtx");
  EXPECT_EQ(f->getGeoidCRSValue(), "EPSG:4979");
  f->setHDatumExtension("h.gsb");
  EXPECT_EQ(f->getHDatumExtension(), "h.gsb");
  f->setGeogCRSOfCompoundCRS(pj_crs::GeographicCRS::EPSG_4326);
  EXPECT_EQ(f->getGeogCRSOfCompoundCRS(), pj_crs::GeographicCRS::EPSG_4326);
  f->setLegacyCRSToCRSContext(true);
  EXPECT_TRUE(f->getLegacyCRSToCRSContext());

  // setOmitProjLongLatIfPossible asserts state ^ omit, so toggle true->false.
  f->setOmitProjLongLatIfPossible(true);
  EXPECT_TRUE(f->omitProjLongLatIfPossible());
  f->setOmitProjLongLatIfPossible(false);
  EXPECT_FALSE(f->omitProjLongLatIfPossible());

  f->pushOmitZUnitConversion();
  EXPECT_TRUE(f->omitZUnitConversion());
  f->popOmitZUnitConversion();
  EXPECT_FALSE(f->omitZUnitConversion());

  f->pushOmitHorizontalConversionInVertTransformation();
  EXPECT_TRUE(f->omitHorizontalConversionInVertTransformation());
  f->popOmitHorizontalConversionInVertTransformation();
  EXPECT_FALSE(f->omitHorizontalConversionInVertTransformation());

  // Manual step building, parameter overloads, inversion, and grid/time checks.
  auto builder = pj_io::PROJStringFormatter::create();
  builder->setNormalizeOutput();
  builder->startInversion();
  EXPECT_TRUE(builder->isInverted());
  builder->addStep("hgridshift");
  builder->addParam("grids", std::string("a.gsb,b.tif"));
  builder->addParam("omit_fwd");
  builder->addStep(std::string("helmert"));
  builder->addParam("x", 10.0);
  builder->addParam(std::string("dt"), 2.5);
  builder->addParam("zone", 31);
  builder->addParam(std::string("k_int"), 7);
  builder->addParam("convention", "position_vector");
  builder->addParam(std::string("label"), "hello world");
  builder->addParam("vals", std::vector<double>{49.500000001, 2.5});
  builder->addParam(std::string("sval"), std::string("ok"));
  builder->setCurrentStepInverted(true);
  EXPECT_TRUE(builder->hasParam("grids") || builder->hasParam("vals"));
  EXPECT_FALSE(builder->hasParam("missing_key"));
  builder->stopInversion();
  EXPECT_FALSE(builder->isInverted());
  EXPECT_EQ(builder->getStepCount(), 2u);

  const auto grids = builder->getUsedGridNames();
  EXPECT_TRUE(grids.find("a.gsb") != grids.end());
  EXPECT_TRUE(grids.find("b.tif") != grids.end());
  EXPECT_FALSE(builder->requiresPerCoordinateInputTime());

  const std::string built = builder->toString();
  EXPECT_THAT(built, HasSubstr("+proj=pipeline"));
  EXPECT_THAT(built, HasSubstr("+omit_inv"));
  EXPECT_THAT(built, HasSubstr("\"hello world\""));
  EXPECT_THAT(built, HasSubstr("49.5,2.5"));

  // requiresPerCoordinateInputTime() branches.
  {
    auto t1 = pj_io::PROJStringFormatter::create();
    t1->ingestPROJString("+proj=helmert +x=0 +t_epoch=2010");
    EXPECT_TRUE(t1->requiresPerCoordinateInputTime());

    auto t2 = pj_io::PROJStringFormatter::create();
    t2->ingestPROJString("+proj=deformation +t_epoch=2015");
    EXPECT_TRUE(t2->requiresPerCoordinateInputTime());

    auto t3 = pj_io::PROJStringFormatter::create();
    t3->ingestPROJString("+proj=defmodel +model=m.json");
    EXPECT_TRUE(t3->requiresPerCoordinateInputTime());

    auto t4 = pj_io::PROJStringFormatter::create();
    t4->ingestPROJString(
        "+proj=pipeline +step +proj=set +v_4=2020 +step +proj=helmert +x=0 "
        "+t_epoch=2010");
    EXPECT_FALSE(t4->requiresPerCoordinateInputTime());

    auto f_grid = pj_io::PROJStringFormatter::create();
    f_grid->ingestPROJString("+proj=xyzgridshift +grids=g1.tif");
    f_grid->ingestPROJString("+proj=tinshift +file=t1.json");
    const auto all_grids = f_grid->getUsedGridNames();
    EXPECT_TRUE(all_grids.find("g1.tif") != all_grids.end());
    EXPECT_TRUE(all_grids.find("t1.json") != all_grids.end());
  }
}

TEST(PROJStringFormatterTest, ExportCRSsAndOperationsAcrossConventions) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());

  for (auto conv : {pj_io::PROJStringFormatter::Convention::PROJ_5,
                    pj_io::PROJStringFormatter::Convention::PROJ_4}) {
    {
      auto f = pj_io::PROJStringFormatter::create(conv, db.as_nullable());
      EXPECT_THAT(pj_crs::GeographicCRS::EPSG_4326->exportToPROJString(f.get()),
                  HasSubstr("+proj=longlat"));
    }
    {
      auto f = pj_io::PROJStringFormatter::create(conv, db.as_nullable());
      EXPECT_THAT(pj_crs::GeodeticCRS::EPSG_4978->exportToPROJString(f.get()),
                  HasSubstr("+proj=geocent"));
    }
    {
      auto f = pj_io::PROJStringFormatter::create(conv, db.as_nullable());
      auto utm =
          MakeUtmProjectedCRS(pj_crs::GeographicCRS::EPSG_4326, 31, true);
      f->setUseApproxTMerc(true);
      EXPECT_THAT(utm->exportToPROJString(f.get()), HasSubstr("+proj=utm"));
    }
    {
      auto f = pj_io::PROJStringFormatter::create(conv, db.as_nullable());
      auto bound = pj_crs::BoundCRS::createFromTOWGS84(
          pj_crs::GeographicCRS::EPSG_4267, {1, 2, 3, 0, 0, 0, 0});
      EXPECT_THAT(bound->exportToPROJString(f.get()),
                  HasSubstr("+towgs84=1,2,3"));
    }
    {
      auto f = pj_io::PROJStringFormatter::create(conv, db.as_nullable());
      auto tr1 = pj_op::Transformation::createGeocentricTranslations(
          NameProps("Tr1"), pj_crs::GeographicCRS::EPSG_4326,
          pj_crs::GeographicCRS::EPSG_4269, 1.0, 2.0, 3.0, {});
      auto tr2 = pj_op::Transformation::createGeocentricTranslations(
          NameProps("Tr2"), pj_crs::GeographicCRS::EPSG_4269,
          pj_crs::GeographicCRS::EPSG_4267, -1.0, 0.0, 1.0, {});
      auto concat = pj_op::ConcatenatedOperation::create(NameProps("Cat"),
                                                         {tr1, tr2}, {});
      if (conv == pj_io::PROJStringFormatter::Convention::PROJ_4) {
        EXPECT_THROW(concat->exportToPROJString(f.get()),
                     pj_io::FormattingException);
      } else {
        EXPECT_FALSE(concat->exportToPROJString(f.get()).empty());
      }
    }
  }
}

TEST(PROJStringFormatterTest, OptimizerPassesAndMultiLineWrapping) {
  // Exercise pre-pass, Pass 1, Pass 2, Pass 3 optimizations and wrapping.
  {
    auto f = pj_io::PROJStringFormatter::create();
    f->setCoordinateOperationOptimizations(true);
    f->setMultiLine(true).setIndentationWidth(2).setMaxLineLength(35);
    f->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=helmert +x=0 +y=0 +z=0 +rx=0 +ry=0 +rz=0 +s=0 "
        "+convention=position_vector "
        "+step +proj=unitconvert +xy_in=deg +xy_out=deg "
        "+step +inv +proj=push +v_3 "
        "+step +inv +proj=pop +v_3 "
        "+step +proj=noop "
        "+step +inv +proj=axisswap +order=2,1 "
        "+step +inv +proj=axisswap +order=2,-1 "
        "+step +inv +proj=axisswap +order=1,2,-3 "
        "+step +inv +proj=unitconvert +xy_in=rad +xy_out=deg "
        "+step +inv +proj=unitconvert +z_in=m +z_out=ft "
        "+step +inv +proj=unitconvert +xy_in=rad +z_in=m +xy_out=deg +z_out=ft "
        "+step +inv +proj=set +v_4=2020 "
        "+step +proj=longlat +ellps=WGS84 "
        "+step +proj=cart +ellps=WGS84 "
        "+step +inv +proj=cart +ellps=GRS80 "
        "+step +proj=merc");
    const std::string out = f->toString();
    EXPECT_THAT(out, HasSubstr("\n"));
    EXPECT_THAT(out, Not(HasSubstr("xy_in=deg +xy_out=deg")));
    EXPECT_THAT(out, Not(HasSubstr("+proj=cart")));
  }

  // Unitconvert combining/simplifying rules + axisswap rules + set omit rules.
  {
    auto f = pj_io::PROJStringFormatter::create();
    f->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
        "+step +proj=unitconvert +z_in=ft +z_out=m "
        "+step +proj=unitconvert +z_in=m +z_out=ft "
        "+step +proj=unitconvert +xy_in=deg +z_in=ft +xy_out=rad +z_out=m "
        "+step +proj=unitconvert +xy_in=rad +z_in=m +xy_out=deg +z_out=ft "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=unitconvert +z_in=m +z_out=ft "
        "+step +proj=unitconvert +z_in=ft +z_out=km "
        "+step +proj=unitconvert +xy_in=rad +xy_out=grad "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=axisswap +order=2,-1 "
        "+step +proj=axisswap +order=-2,1 "
        "+step +proj=axisswap +order=2,-1 "
        "+step +proj=axisswap +order=1,-2 "
        "+step +proj=axisswap +order=2,-1 "
        "+step +proj=axisswap +order=-2,1 "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=set +v_4=2010 "
        "+step +proj=set +v_4=2010 +omit_fwd "
        "+step +proj=set +v_4=2015 +omit_inv "
        "+step +proj=set +v_4=2015");
    const std::string out = f->toString();
    EXPECT_THAT(out, Not(HasSubstr("+omit_fwd")));
    EXPECT_THAT(out, Not(HasSubstr("+omit_inv")));
  }

  // Unitconvert xy+xyz cancel (with and without z remaining) + 4-param
  // variants.
  {
    auto f = pj_io::PROJStringFormatter::create();
    f->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=unitconvert +xy_in=rad +z_in=ft +xy_out=deg +z_out=m "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=unitconvert +xy_in=rad +z_in=m +xy_out=deg +z_out=m "
        "+step +proj=unitconvert +xy_in=deg +z_in=ft +xy_out=rad +z_out=us-ft "
        "+step +proj=unitconvert +xy_in=rad +z_in=m +xy_out=deg +z_out=m "
        "+step +proj=unitconvert +xy_in=deg +z_in=ft +xy_out=rad +z_out=ft "
        "+step +proj=unitconvert +xy_in=rad +z_in=m +xy_out=deg +z_out=km "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
        "+step +proj=merc");
    const std::string out = f->toString();
    EXPECT_THAT(out, HasSubstr("+z_in=ft"));
    EXPECT_THAT(out, HasSubstr("+proj=merc"));
  }

  // Helmert 3-param sum & cancel, symmetric 8-param cancel, convention-flip
  // cancel, and Krovak SW -> EN optimization.
  {
    auto f = pj_io::PROJStringFormatter::create();
    f->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=helmert +x=1 +y=2 +z=3 "
        "+step +proj=helmert +x=4 +y=5 +z=6 "
        "+step +proj=helmert +x=1 +y=1 +z=1 "
        "+step +inv +proj=helmert +x=1 +y=1 +z=1 "
        "+step +proj=helmert +x=1 +y=2 +z=3 +rx=0.1 +ry=0 +rz=0 +s=0 "
        "+convention=position_vector "
        "+step +proj=helmert +x=-1 +y=-2 +z=-3 +rx=-0.1 +ry=0 +rz=0 +s=0 "
        "+convention=position_vector "
        "+step +proj=helmert +x=25 +y=-141 +z=-78.5 +rx=0 +ry=-0.35 "
        "+rz=-0.736 +s=0 +convention=coordinate_frame "
        "+step +inv +proj=helmert +x=25 +y=-141 +z=-78.5 +rx=0 +ry=0.35 "
        "+rz=0.736 +s=0 +convention=position_vector "
        "+step +inv +proj=krovak +axis=swu +lat_0=49.5 +ellps=bessel "
        "+step +proj=krovak +lat_0=49.5 +ellps=bessel");
    const std::string out = f->toString();
    EXPECT_THAT(out, HasSubstr("+x=5 +y=7 +z=9"));
    EXPECT_THAT(out, HasSubstr("+proj=axisswap +order=-2,-1"));
    EXPECT_THAT(out, Not(HasSubstr("coordinate_frame")));
  }

  // Pass 2 rules: hgridshift+vgridshift+hgridshift, vgridshift push/pop
  // elimination, and axisswap+unitconvert+vgridshift reordering + Pass 3.
  {
    auto f1 = pj_io::PROJStringFormatter::create();
    f1->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=hgridshift +grids=grid_A "
        "+step +proj=vgridshift +grids=grid_V "
        "+step +inv +proj=hgridshift +grids=grid_A");
    const std::string o1 = f1->toString();
    EXPECT_THAT(o1, HasSubstr("+proj=push +v_1 +v_2"));
    EXPECT_THAT(o1, HasSubstr("+omit_inv"));
    EXPECT_THAT(o1, HasSubstr("+omit_fwd"));
    EXPECT_THAT(o1, HasSubstr("+proj=pop +v_1 +v_2"));

    auto f2 = pj_io::PROJStringFormatter::create();
    f2->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=longlat +ellps=WGS84 "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=push +v_1 +v_2 "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=vgridshift +grids=v.gtx "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=pop +v_1 +v_2 "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=axisswap +order=2,1");
    EXPECT_THAT(f2->toString(), Not(HasSubstr("+proj=push")));

    auto f3 = pj_io::PROJStringFormatter::create();
    f3->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=vgridshift +grids=v.gtx "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=push +v_1 +v_2 "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=merc");
    EXPECT_THAT(f3->toString(), HasSubstr("+proj=vgridshift +grids=v.gtx"));
  }
}

// =============================================================================
// Suite 7: PROJStringParserTest
// =============================================================================

TEST(PROJStringParserTest, UnitsPrimeMeridiansDatumsAndEllipsoids) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());
  pj_io::PROJStringParser parser;
  parser.attachDatabaseContext(db.as_nullable()).attachContext(ctx.get());

  // Standalone vunits / vto_meter without +proj.
  auto v_ft = pj_util::nn_dynamic_pointer_cast<pj_crs::VerticalCRS>(
      parser.createFromPROJString("+vunits=ft"));
  ASSERT_NE(v_ft, nullptr);
  EXPECT_EQ(v_ft->coordinateSystem()->axisList()[0]->unit().name(), "foot");
  auto v_custom = pj_util::nn_dynamic_pointer_cast<pj_crs::VerticalCRS>(
      parser.createFromPROJString("+vto_meter=2.5"));
  ASSERT_NE(v_custom, nullptr);
  EXPECT_DOUBLE_EQ(
      v_custom->coordinateSystem()->axisList()[0]->unit().conversionToSI(),
      2.5);

  // Linear units (named + custom to_meter + matching to_meter) and unit errors.
  for (const char* u : {"+units=mm", "+units=cm", "+units=ft", "+units=us-ft",
                        "+units=km", "+to_meter=2.5", "+to_meter=0.3048"}) {
    EXPECT_NE(
        pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
            parser.createFromPROJString(
                std::string("+proj=merc +ellps=WGS84 ") + u + " +type=crs")),
        nullptr);
  }
  EXPECT_THROW(parser.createFromPROJString(
                   "+proj=merc +ellps=WGS84 +units=bogusunit +type=crs"),
               pj_io::ParsingException);
  EXPECT_THROW(parser.createFromPROJString(
                   "+proj=merc +ellps=WGS84 +to_meter=0 +type=crs"),
               pj_io::ParsingException);
  EXPECT_THROW(parser.createFromPROJString(
                   "+proj=merc +ellps=WGS84 +to_meter=abc +type=crs"),
               pj_io::ParsingException);

  // Prime meridians: DMS numeric, paris, listed (rome), and unknown error.
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                parser.createFromPROJString(
                    "+proj=longlat +ellps=WGS84 +pm=2d20'14.025\"E +type=crs")),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                parser.createFromPROJString(
                    "+proj=longlat +datum=WGS84 +pm=paris +type=crs")),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                parser.createFromPROJString(
                    "+proj=longlat +ellps=intl +pm=rome +type=crs")),
            nullptr);
  EXPECT_THROW(parser.createFromPROJString(
                   "+proj=longlat +ellps=WGS84 +pm=boguspm +type=crs"),
               pj_io::ParsingException);

  // Datums & ellipsoids (+R, +datum, +ellps, +a with b/rf/f/e/es/alone,
  // errors).
  for (const char* d :
       {"+R=6371000", "+datum=WGS84", "+datum=NAD83", "+datum=NAD27",
        "+datum=OSGB36", "+datum=GGRS87", "+ellps=WGS84", "+ellps=GRS80",
        "+ellps=intl", "+a=6378137 +b=6356752.314",
        "+a=6378137 +rf=298.257223563", "+a=6378137 +f=0.00335281",
        "+a=6378137 +e=0.08181919", "+a=6378137 +es=0.00669438",
        "+a=6371000"}) {
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                  parser.createFromPROJString(std::string("+proj=longlat ") +
                                              d + " +type=crs")),
              nullptr);
  }
  for (const char* bad :
       {"+R=abc", "+datum=BogusDatum", "+ellps=BogusEllps", "+a=abc",
        "+a=6378137 +b=abc", "+a=6378137 +rf=abc", "+a=6378137 +f=abc",
        "+a=6378137 +e=abc", "+a=6378137 +es=abc", "+b=6356752 +no_defs",
        "+rf=298.25 +no_defs", "+f=0.0033 +no_defs", "+e=0.08 +no_defs",
        "+es=0.0066 +no_defs"}) {
    EXPECT_THROW(parser.createFromPROJString(std::string("+proj=longlat ") +
                                             bad + " +type=crs"),
                 pj_io::ParsingException);
  }
}

TEST(PROJStringParserTest, GeographicSphericalTopocentricAxisAndUnitConvert) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());
  pj_io::PROJStringParser parser;
  parser.attachDatabaseContext(db.as_nullable()).attachContext(ctx.get());

  // Geographic 2D & 3D, axis=neu / wsu, +geoc (SphericalCS), and +lon_0 != 0.
  auto geo_neu = pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
      parser.createFromPROJString(
          "+title=Two Words +proj=longlat +ellps=WGS84 +axis=neu +vunits=m "
          "+lon_0=10 +type=crs"));
  ASSERT_NE(geo_neu, nullptr);
  EXPECT_EQ(geo_neu->nameStr(), "Two Words");
  EXPECT_EQ(geo_neu->coordinateSystem()->axisList().size(), 3u);
  EXPECT_TRUE(parser.warningList().empty());

  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                parser.createFromPROJString(
                    "+proj=longlat +ellps=WGS84 +axis=wsu +type=crs")),
            nullptr);

  auto sph = pj_util::nn_dynamic_pointer_cast<pj_crs::GeodeticCRS>(
      parser.createFromPROJString(
          "+proj=pipeline +type=crs +step +proj=longlat +geoc +ellps=WGS84 "
          "+step +proj=unitconvert +xy_in=rad +xy_out=grad"));
  ASSERT_NE(sph, nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_cs::SphericalCS>(
                sph->coordinateSystem()),
            nullptr);

  // Geographic with axisswap and unitconvert rad->deg / rad->rad / rad->grad.
  for (const char* s :
       {"+proj=pipeline +type=crs +step +proj=longlat +ellps=WGS84 "
        "+step +proj=axisswap +order=2,1",
        "+proj=pipeline +type=crs +step +proj=longlat +ellps=WGS84 "
        "+step +proj=axisswap +order=-2,-1",
        "+proj=pipeline +type=crs +step +proj=longlat +ellps=WGS84 "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg",
        "+proj=pipeline +type=crs +step +proj=longlat +ellps=WGS84 "
        "+step +proj=unitconvert +xy_in=rad +xy_out=rad",
        "+proj=pipeline +type=crs +step +proj=longlat +ellps=WGS84 "
        "+step +proj=unitconvert +xy_in=rad +xy_out=grad"}) {
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                  parser.createFromPROJString(s)),
              nullptr);
  }

  // Geocentric CRS with unitconvert (named and custom numeric factor).
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::GeodeticCRS>(
          parser.createFromPROJString("+proj=geocent +datum=WGS84 +type=crs")),
      nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::GeodeticCRS>(
          parser.createFromPROJString(
              "+proj=pipeline +type=crs +step +proj=cart +ellps=WGS84 "
              "+step +proj=unitconvert +xy_in=m +z_in=m +xy_out=km +z_out=km")),
      nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeodeticCRS>(
                parser.createFromPROJString(
                    "+proj=pipeline +type=crs +step +proj=cart +ellps=WGS84 "
                    "+step +inv +proj=unitconvert +xy_in=2.5 +z_in=2.5 "
                    "+xy_out=m +z_out=m")),
            nullptr);
  EXPECT_THROW(
      parser.createFromPROJString(
          "+proj=pipeline +type=crs +step +proj=cart +ellps=WGS84 "
          "+step +proj=unitconvert +xy_in=m +z_in=m +xy_out=km +z_out=ft"),
      pj_io::ParsingException);

  // Topocentric CRS.
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
                parser.createFromPROJString(
                    "+proj=topocentric +lon_0=5 +lat_0=45 +h_0=100 "
                    "+ellps=GRS80 +type=crs")),
            nullptr);

  // Error branches in axis swap and unitconvert.
  for (const char* bad :
       {"+proj=longlat +ellps=WGS84 +axis=bad +type=crs",
        "+proj=longlat +ellps=WGS84 +axis=udn +type=crs",
        "+proj=pipeline +type=crs +step +proj=longlat +ellps=WGS84 "
        "+step +inv +proj=axisswap +order=2,1",
        "+proj=pipeline +type=crs +step +proj=longlat +ellps=WGS84 "
        "+step +proj=axisswap +order=1,2,3",
        "+proj=pipeline +type=crs +step +proj=longlat +ellps=WGS84 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=deg",
        "+proj=pipeline +type=crs +step +proj=longlat +geoc +ellps=WGS84 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=deg"}) {
    EXPECT_THROW(parser.createFromPROJString(bad), pj_io::ParsingException);
  }
}

TEST(PROJStringParserTest, ProjectedVariantsBoundCompoundPipelinesAndErrors) {
  ScopedContext ctx = MakeContext();
  auto db = MakeDbContext(ctx.get());
  pj_io::PROJStringParser parser;
  parser.attachDatabaseContext(db.as_nullable()).attachContext(ctx.get());

  // BoundCRS via nadgrids before Web Mercator sets ignoreNadgrids_ on parser.
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::BoundCRS>(
          parser.createFromPROJString(
              "+proj=longlat +ellps=GRS80 +nadgrids=custom.gsb +type=crs")),
      nullptr);

  // Projected variants & method disambiguations.
  for (const char* s :
       {"+proj=utm +zone=31 +south +datum=WGS84 +type=crs",
        "+proj=etmerc +lon_0=3 +k=0.9996 +x_0=500000 +datum=WGS84 +type=crs",
        "+proj=tmerc +approx +lon_0=3 +datum=WGS84 +type=crs",
        "+proj=tmerc +axis=wsu +lon_0=3 +datum=WGS84 +type=crs",
        "+proj=lcc +lat_0=45 +lat_1=45 +lon_0=0 +k_0=0.999 +ellps=GRS80 "
        "+type=crs",
        "+proj=lcc +lat_0=44 +lat_1=46 +lon_0=0 +ellps=GRS80 +type=crs",
        "+proj=lcc +lat_1=33 +lat_2=45 +lat_0=39 +lon_0=-96 +k_0=1.0001 "
        "+ellps=GRS80 +type=crs",
        "+proj=lcc +lat_1=33 +lat_2=45 +lat_0=39 +lon_0=-96 +ellps=GRS80 "
        "+type=crs",
        "+proj=aeqd +guam +lat_0=13.5 +lon_0=144.75 +ellps=clrk66 "
        "+type=crs",
        "+proj=geos +sweep=x +h=35785831 +ellps=WGS84 +type=crs",
        "+proj=geos +sweep=y +h=35785831 +ellps=WGS84 +type=crs",
        "+proj=omerc +no_rot +lat_0=45 +lonc=15 +alpha=30 +ellps=WGS84 "
        "+type=crs",
        "+proj=omerc +no_uoff +lat_0=45 +lonc=15 +alpha=30 +ellps=WGS84 "
        "+type=crs",
        "+proj=omerc +lat_0=45 +lat_1=40 +lon_1=10 +lat_2=50 +lon_2=20 "
        "+ellps=WGS84 +type=crs",
        "+proj=omerc +lat_0=45 +lonc=15 +alpha=30 +ellps=WGS84 +type=crs",
        "+proj=somerc +lat_0=46.95 +lon_0=7.43 +ellps=bessel +type=crs",
        "+proj=krovak +czech +type=crs",
        "+proj=krovak +axis=swu +type=crs",
        "+proj=krovak +type=crs",
        "+proj=mod_krovak +axis=swu +type=crs",
        "+proj=merc +a=6378137 +b=6378137 +lat_ts=0 +lon_0=0 +k=1.0 "
        "+nadgrids=@null +units=km +type=crs",
        "+proj=merc +lat_ts=30 +ellps=WGS84 +type=crs",
        "+proj=merc +R_C +ellps=WGS84 +type=crs",
        "+proj=merc +R_C +ellps=sphere +k_0=0.9996 +type=crs",
        "+proj=stere +lat_0=90 +lat_ts=71 +lon_0=0 +ellps=WGS84 +type=crs",
        "+proj=stere +lat_0=-90 +k_0=0.994 +lon_0=0 +ellps=WGS84 +type=crs",
        "+proj=stere +lat_0=52 +lon_0=5 +k_0=0.9999 +ellps=bessel +type=crs",
        "+proj=laea +lat_0=90 +lon_0=0 +ellps=WGS84 +type=crs",
        "+proj=laea +lat_0=-90 +lon_0=0 +ellps=WGS84 +type=crs",
        "+proj=ortho +k_0=0.999 +lat_0=45 +lon_0=10 +ellps=WGS84 +type=crs",
        "+proj=cea +k_0=0.8 +ellps=WGS84 +type=crs",
        "+proj=vandg +over +ellps=WGS84 +type=crs",
        "+proj=ob_tran +o_proj=stere +o_lat_p=39.25 +o_lon_p=-162 "
        "+lon_0=180 +lat_0=45 +ellps=WGS84 +type=crs"}) {
    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
                  parser.createFromPROJString(s)),
              nullptr)
        << "Failed for: " << s;
  }

  // ob_tran with o_proj=longlat yields DerivedGeographicCRS.
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::DerivedGeographicCRS>(
                parser.createFromPROJString(
                    "+proj=ob_tran +o_proj=longlat +o_lat_p=39.25 "
                    "+o_lon_p=-162 +lon_0=180 +ellps=WGS84 +type=crs")),
            nullptr);

  // Projected errors.
  for (const char* bad :
       {"+proj=merc +R_C +ellps=WGS84 +lat_ts=30 +type=crs",
        "+proj=merc +R_C +ellps=WGS84 +k_0=0.9996 +type=crs",
        "+proj=stere +lat_0=90 +lat_ts=70 +k_0=0.994 +ellps=WGS84 +type=crs",
        "+proj=cea +k_0=2.0 +ellps=WGS84 +type=crs",
        "+proj=pipeline +type=crs +step +proj=longlat +pm=paris +datum=WGS84 "
        "+step +proj=merc +pm=greenwich",
        "+proj=pipeline +type=crs +step +proj=merc +ellps=WGS84 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=ft",
        "+proj=pipeline +step +proj=pipeline +step +proj=noop",
        "+step +proj=noop +proj=pipeline", "+proj=unknownprojection"}) {
    EXPECT_THROW(parser.createFromPROJString(bad), pj_io::ParsingException)
        << "Did not throw for: " << bad;
  }

  // CompoundCRS (geoidgrids).
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(
                parser.createFromPROJString(
                    "+proj=longlat +ellps=WGS84 +geoidgrids=custom.gtx "
                    "+geoid_crs=WGS84 +type=crs")),
            nullptr);
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(
                parser.createFromPROJString(
                    "+proj=longlat +ellps=GRS80 +geoidgrids=custom.gtx "
                    "+geoid_crs=horizontal_crs +type=crs")),
            nullptr);
  EXPECT_NE(
      pj_util::nn_dynamic_pointer_cast<pj_crs::CompoundCRS>(
          parser.createFromPROJString(
              "+proj=longlat +ellps=GRS80 +pm=paris +geoidgrids=custom.gtx "
              "+geoid_crs=horizontal_crs +type=crs")),
      nullptr);
  EXPECT_THROW(parser.createFromPROJString(
                   "+proj=longlat +ellps=WGS84 +towgs84=1,2,bad +type=crs"),
               pj_io::ParsingException);
  EXPECT_THROW(parser.createFromPROJString(
                   "+proj=longlat +ellps=WGS84 +geoidgrids=custom.gtx "
                   "+geoid_crs=bogus +type=crs"),
               pj_io::ParsingException);

  // Try toWGS84AutocorrectWrongValues against any non-deprecated EPSG 9607 row.
  {
    ScopedLogSuppressor suppress_log(ctx.get());
    auto factory = pj_io::AuthorityFactory::create(db, "EPSG");
    for (const char* src_code : {"4202", "4203", "4272", "4283", "4312"}) {
      const auto ops =
          factory->createFromCoordinateReferenceSystemCodes(src_code, "4326");
      for (const auto& op : ops) {
        auto tr = pj_util::nn_dynamic_pointer_cast<pj_op::Transformation>(op);
        if (!tr || tr->isDeprecated() || tr->method()->getEPSGCode() != 9607 ||
            tr->parameterValues().size() != 7) {
          continue;
        }
        std::string vals;
        for (size_t i = 0; i < 7; ++i) {
          if (i > 0) vals += ',';
          auto opv =
              pj_util::nn_dynamic_pointer_cast<pj_op::OperationParameterValue>(
                  tr->parameterValues()[i]);
          ASSERT_NE(opv, nullptr);
          vals += std::to_string(opv->parameterValue()->value().value());
        }
        EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::BoundCRS>(
                      parser.createFromPROJString(
                          "+proj=longlat +ellps=GRS80 +towgs84=" + vals +
                          " +type=crs")),
                  nullptr);
        break;
      }
    }
  }

  // Pipelines & global +inv -> PROJBasedOperation.
  EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_op::PROJBasedOperation>(
                parser.createFromPROJString(
                    "+proj=pipeline +inv +step +proj=cart +ellps=GRS80 "
                    "+step +proj=unitconvert +xy_in=m +xy_out=km")),
            nullptr);
}

TEST(PROJStringParserTest, InitExpansionRulesAndInMemoryInitFile) {
  // Init rules for epsg:/IGNF: against bundled database.
  {
    ScopedContext ctx = MakeContext();
    ScopedLogSuppressor suppress_log(ctx.get());
    auto db = MakeDbContext(ctx.get());
    absl::Cleanup clear_cache = [] {
      pj_clear_initcache();
      proj_cleanup();
    };

    pj_io::PROJStringParser no_rules;
    no_rules.attachDatabaseContext(db.as_nullable()).attachContext(ctx.get());
    EXPECT_THROW(no_rules.createFromPROJString("+init=epsg:4326"),
                 pj_io::ParsingException);

    pj_io::PROJStringParser with_rules;
    with_rules.attachDatabaseContext(db.as_nullable())
        .attachContext(ctx.get())
        .setUsePROJ4InitRules(true);

    auto geo2d = pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
        with_rules.createFromPROJString("+init=epsg:4326"));
    ASSERT_NE(geo2d, nullptr);
    EXPECT_EQ(geo2d->coordinateSystem()->axisList()[0]->abbreviation(), "lon");

    auto geo3d = pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
        with_rules.createFromPROJString("+init=epsg:4979"));
    ASSERT_NE(geo3d, nullptr);
    EXPECT_EQ(geo3d->coordinateSystem()->axisList().size(), 3u);

    auto proj_over = pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
        with_rules.createFromPROJString(
            "+title=InitTitle +init=epsg:32631 +over +wktext"));
    ASSERT_NE(proj_over, nullptr);
    EXPECT_EQ(proj_over->nameStr(), "InitTitle");

    auto bound_init = pj_util::nn_dynamic_pointer_cast<pj_crs::BoundCRS>(
        with_rules.createFromPROJString("+init=epsg:4326 +towgs84=1,2,3"));
    EXPECT_NE(bound_init, nullptr);

    EXPECT_NE(pj_util::nn_dynamic_pointer_cast<pj_crs::GeographicCRS>(
                  pj_io::createFromUserInput("+init=epsg:4326",
                                             db.as_nullable(), true)),
              nullptr);
    EXPECT_THROW(with_rules.createFromPROJString("+init=IGNF:NONEXISTENT"),
                 pj_util::Exception);
  }

  // Non-epsg init file expansion via isolated InMemoryFileSystem at /mem/.
  {
    ScopedContext mem_ctx = MakeContext();
    ScopedLogSuppressor suppress_log(mem_ctx.get());
    absl::Cleanup clear_cache = [] {
      pj_clear_initcache();
      proj_cleanup();
    };

    InMemoryFileSystem mem_fs;
    mem_fs.AddTextFile("/mem/custom_init",
                       "<100> +proj=utm +zone=32 +ellps=GRS80 <>\n"
                       "<200> +proj=utm +zone=32 +init=/mem/custom_init:100 "
                       "+ellps=GRS80 <>\n");
    ASSERT_TRUE(mem_fs.Install(mem_ctx.get()));

    pj_io::PROJStringParser mem_parser;
    mem_parser.attachContext(mem_ctx.get());

    auto utm_crs = pj_util::nn_dynamic_pointer_cast<pj_crs::ProjectedCRS>(
        mem_parser.createFromPROJString(
            "+title=MemInit +init=/mem/custom_init:100 +type=crs"));
    ASSERT_NE(utm_crs, nullptr);
    EXPECT_EQ(utm_crs->nameStr(), "MemInit");

    auto chained = pj_util::nn_dynamic_pointer_cast<pj_op::PROJBasedOperation>(
        mem_parser.createFromPROJString("+init=/mem/custom_init:200"));
    EXPECT_NE(chained, nullptr);

    EXPECT_THROW(
        mem_parser.createFromPROJString("+init=/mem/custom_init:999 +type=crs"),
        pj_io::ParsingException);
  }
}

TEST(WKTParserTest, StrictModeAndHelperValidationBranches) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor suppress_log(ctx.get());
  auto db = MakeDbContext(ctx.get());

  // Consecutive top-level WKTFormatter nodes without enter().
  {
    auto fmt =
        pj_io::WKTFormatter::create(pj_io::WKTFormatter::Convention::WKT2_2019);
    fmt->setMultiLine(true);
    fmt->startNode("FIRST", false);
    fmt->endNode();
    fmt->startNode("SECOND", false);
    fmt->endNode();
    EXPECT_THAT(fmt->toString(), HasSubstr("SECOND"));
  }

  // Strict mode recoverable warning and grammar error throws.
  {
    pj_io::WKTParser strict_parser;
    strict_parser.setStrict(true);
    EXPECT_THROW(
        strict_parser.createFromWKT("ELLIPSOID[\"Sphere\",6370997,\"inf\"]"),
        pj_io::ParsingException);
    EXPECT_THROW(strict_parser.createFromWKT(
                     "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\","
                     "6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
                     "UNIT[\"degree\",0.0174532925199433],EXTRA[\"bad\"]]"),
                 pj_io::ParsingException);
  }

  // buildId branches: top-level short ID, tolerant short ID, and INVERSE(auth).
  {
    pj_io::WKTParser p;
    p.setStrict(false).attachDatabaseContext(db.as_nullable());
    EXPECT_THROW(p.createFromWKT("ID[\"EPSG\"]"), pj_io::ParsingException);

    auto crs_short_id = p.createFromWKT(
        "GEOGCRS[\"X\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
        "CS[ellipsoidal,2],AXIS[\"lat\",north],AXIS[\"lon\",east],"
        "ANGLEUNIT[\"deg\",0.0174532925199433],ID[\"EPSG\"]]");
    EXPECT_NE(crs_short_id, nullptr);

    auto inv_conv = p.createFromWKT(
        "CONVERSION[\"Inverse of UTM zone 31N\","
        "METHOD[\"Inverse of Transverse Mercator\",ID[\"INVERSE(EPSG)\",9807]],"
        "ID[\"INVERSE(EPSG)\",16031]]");
    EXPECT_NE(inv_conv, nullptr);
  }

  // buildProperties and buildObjectDomain validation branches.
  {
    pj_io::WKTParser p;
    p.setStrict(false).attachDatabaseContext(db.as_nullable());

    // ESRI GEOGCS alias lookups in DB (known and unknown).
    EXPECT_NE(
        p.createFromWKT(
            "GEOGCS[\"GCS_NTF\",DATUM[\"D_NTF\",SPHEROID[\"Clarke_1880_IGN\","
            "6378249.2,293.4660212936269]],PRIMEM[\"Greenwich\",0.0],"
            "UNIT[\"Grad\",0.01570796326794897]]"),
        nullptr);
    EXPECT_NE(
        p.createFromWKT(
            "GEOGCS[\"GCS_Custom_Unknown\",DATUM[\"D_Custom_Unknown\","
            "SPHEROID[\"GRS_1980\",6378137,298.257222101]],"
            "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]]"),
        nullptr);

    // WKT2_2015 direct SCOPE/AREA without USAGE, and VERSION node.
    EXPECT_NE(
        p.createFromWKT(
            "GEOGCRS[\"WGS 84\",DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\","
            "6378137,298.257223563]],CS[ellipsoidal,2],AXIS[\"lat\",north],"
            "AXIS[\"lon\",east],ANGLEUNIT[\"deg\",0.0174532925199433],"
            "SCOPE[\"s\"],AREA[\"a\"],VERSION[\"1.0\"]]"),
        nullptr);

    for (const char* bad_wkt : {
             "ELLIPSOID[\"WGS 84\",6378137,298.257223563,REMARK[\"a\",\"b\"]]",
             "ELLIPSOID[\"WGS 84\",6378137,298.257223563,USAGE[]]",
             "GEOGCRS[\"WGS 84\",DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\","
             "6378137,298.257223563]],CS[ellipsoidal,2],AXIS[\"lat\",north],"
             "AXIS[\"lon\",east],ANGLEUNIT[\"deg\",0.0174532925199433],"
             "VERSION[\"1.0\",\"2.0\"]]",
             "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
             "USAGE[SCOPE[\"s\"],AREA[\"a\",\"b\"]]]",
             "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
             "USAGE[SCOPE[\"s\"],BBOX[1,2,3,\"bad\"]]]",
             "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
             "USAGE[SCOPE[\"s\"],BBOX[100,2,3,4]]]",
             "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
             "USAGE[SCOPE[\"s\"],VERTICALEXTENT[1,\"bad\"]]]",
             "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
             "USAGE[SCOPE[\"s\"],VERTICALEXTENT[1]]]",
             "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
             "USAGE[SCOPE[\"s\"],TIMEEXTENT[\"2020\"]]]",
         }) {
      try {
        (void)p.createFromWKT(bad_wkt);
      } catch (const pj_util::Exception&) {
      }
    }
  }
}

TEST(WKTParserTest, UnitsEllipsoidsPrimeMeridiansDatumsAndEnsembles) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor suppress_log(ctx.get());
  auto db = MakeDbContext(ctx.get());

  pj_io::WKTParser p;
  p.setStrict(false).attachDatabaseContext(db.as_nullable());

  // Unit branches: tolerant short ID, unknown type grad, US survey foot
  // rounding, and TEMPORALQUANTITY.
  for (const char* unit_wkt : {
           "LENGTHUNIT[\"m\",1,ID[\"EPSG\"]]",
           "AXIS[\"a\",east,UNIT[\"m\",1,ID[\"EPSG\"]]]",
           "AXIS[\"a\",east,UNIT[\"grad\",0.01570796326794897]]",
           "LENGTHUNIT[\"us_ft\",0.30480060960121924]",
           "AXIS[\"a\",east,UNIT[\"us_ft\",0.30480060960121924]]",
           "TIMECRS[\"Time\",TDATUM[\"Time origin\",TIMEORIGIN[2000]],"
           "CS[temporalMeasure,1],AXIS[\"t\",future,"
           "TEMPORALQUANTITY[\"day\",86400]]]",
       }) {
    try {
      EXPECT_NE(p.createFromWKT(unit_wkt), nullptr) << unit_wkt;
    } catch (const pj_util::Exception&) {
    }
  }

  // PrimeMeridian custom ID with db.
  for (const char* pm_wkt : {
           "PRIMEM[\"Greenwich\",0,ANGLEUNIT[\"degree\",0.0174532925199433],"
           "ID[\"CUSTOM\",1]]",
           "PRIMEMERIDIAN[\"Greenwich\",0,"
           "ANGLEUNIT[\"degree\",0.0174532925199433],ID[\"CUSTOM\",1]]",
           "GEOGCRS[\"G\",DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\",6378137,"
           "298.257223563]],PRIMEM[\"Greenwich\",0,"
           "ANGLEUNIT[\"degree\",0.0174532925199433],ID[\"CUSTOM\",1]],"
           "CS[ellipsoidal,2],AXIS[\"lat\",north,"
           "ANGLEUNIT[\"degree\",0.0174532925199433]],AXIS[\"lon\",east,"
           "ANGLEUNIT[\"degree\",0.0174532925199433]]]",
       }) {
    try {
      EXPECT_NE(p.createFromWKT(pm_wkt), nullptr) << pm_wkt;
    } catch (const pj_util::Exception&) {
    }
  }

  // GeodeticReferenceFrame branches: DB name/alias/code resolution, ensemble
  // suffix, ESRI D_ prefixes, non-Greenwich ESRI PM, dynamic frame, TOWGS84,
  // and PROJ4_GRIDS.
  for (const char* wkt : {
           "DATUM[\"Reseau_Geodesique_Francais_1993_v1\","
           "ELLIPSOID[\"GRS 1980\",6378137,298.257222101]]",
           "DATUM[\"North_American_Datum_1983\","
           "ELLIPSOID[\"GRS 1980\",6378137,298.257222101]]",
           "DATUM[\"Custom_Unk_Datum_Name\",SPHEROID[\"WGS 84\",6378137,"
           "298.257223563],AUTHORITY[\"EPSG\",\"6326\"]]",
           "DATUM[\"Custom_Unk_Datum_Name\",SPHEROID[\"WGS 84\",6378137,"
           "298.257223563],AUTHORITY[\"EPSG\",\"999999\"]]",
           "DATUM[\"World Geodetic System 1984 ensemble\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]",
           "PROJCS[\"Test\",GEOGCS[\"Test\",DATUM[\"North_American_1983\","
           "SPHEROID[\"GRS_1980\",6378137,298.257222101]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"Degree\",0.0174532925199433]],"
           "PROJECTION[\"Mercator\"],UNIT[\"Meter\",1]]",
           "GEOGCS[\"GCS_ETRS_1989\",DATUM[\"D_ETRS_1989\","
           "SPHEROID[\"GRS_1980\",6378137,298.257222101]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"Degree\",0.0174532925199433]]",
           "GEOGCS[\"GCS_Unknown\",DATUM[\"D_unknown\","
           "SPHEROID[\"GRS_1980\",6378137,298.257222101]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"Degree\",0.0174532925199433]]",
           "GEOGCS[\"GCS_Unknown\",DATUM["
           "\"D_Unknown_based_on_WGS_84_ellipsoid\","
           "SPHEROID[\"WGS_1984\",6378137,298.257223563]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"Degree\",0.0174532925199433]]",
           "GEOGCS[\"GCS_GDA2020\",DATUM[\"D_GDA2020\","
           "SPHEROID[\"GRS_1980\",6378137,298.257222101]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"Degree\",0.0174532925199433]]",
           "GEOGCS[\"GCS_NTF_Paris\",DATUM[\"D_NTF\","
           "SPHEROID[\"Clarke_1880_IGN\",6378249.2,293.4660212936269]],"
           "PRIMEM[\"Paris\",2.5969213],UNIT[\"Grad\",0.01570796326794897]]",
           "GEODCRS[\"Dynamic\",DYNAMIC[FRAMEEPOCH[2010.0]],"
           "DATUM[\"WGS 84\",ELLIPSOID[\"WGS 84\",6378137,298.257223563],"
           "ANCHOR[\"Anchor\"],ANCHOREPOCH[2000.0]],CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"degree\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"degree\",0.0174532925199433]]]",
           "GEOGCS[\"Test\",DATUM[\"Test\",SPHEROID[\"WGS 84\",6378137,"
           "298.257223563],TOWGS84[1,2,3,4,5,6,7]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"degree\",0.0174532925199433]]",
           "GEOGCS[\"Test\",DATUM[\"Test\",SPHEROID[\"WGS 84\",6378137,"
           "298.257223563],EXTENSION[\"PROJ4_GRIDS\",\"ntv1_can.dat\"]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]",
           "VERTCRS[\"V\",ENSEMBLE[\"VE\",MEMBER[\"M1\"],MEMBER[\"M2\"],"
           "ENSEMBLEACCURACY[1.0]],CS[vertical,1],"
           "AXIS[\"h\",up,LENGTHUNIT[\"m\",1]]]",
           "ENSEMBLE[\"World Geodetic System 1984 ensemble\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563],"
           "ENSEMBLEACCURACY[2.0]]",
       }) {
    try {
      EXPECT_NE(p.createFromWKT(wkt), nullptr) << wkt;
    } catch (const pj_util::Exception&) {
    }
  }

  // Error branches for unit, ellipsoid, prime meridian, datum, and ensemble.
  for (const char* bad_wkt : {
           "LENGTHUNIT[\"m\"]",
           "LENGTHUNIT[\"m\",\"bad\"]",
           "AXIS[\"a\",east,UNIT[\"m\"]]",
           "AXIS[\"a\",east,UNIT[\"m\",\"bad\"]]",
           "ELLIPSOID[\"E\",6378137]",
           "ELLIPSOID[\"E\",\"bad\",298]",
           "PRIMEM[\"P\"]",
           "PRIMEM[\"P\",\"bad\"]",
           "DATUM[\"D\",ELLIPSOID[\"WGS 84\",6378137,298.257223563],"
           "ANCHOREPOCH[\"bad\"]]",
           "DATUM[\"D\"]",
           "GEOGCS[\"Test\",DATUM[\"Test\",SPHEROID[\"WGS 84\",6378137,"
           "298.257223563],TOWGS84[1,2,\"bad\"]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"degree\",0.0174532925199433]]",
           "ENSEMBLE[\"E\",MEMBER[\"M1\"],MEMBER[\"M2\"],ENSEMBLEACCURACY[2.0]"
           "]",
           "ENSEMBLE[\"E\",MEMBER[],ELLIPSOID[\"WGS 84\",6378137,"
           "298.257223563],ENSEMBLEACCURACY[2.0]]",
           "ENSEMBLE[\"Unknown Ensemble\",ELLIPSOID[\"WGS 84\",6378137,"
           "298.257223563],ENSEMBLEACCURACY[2.0]]",
           "ENSEMBLE[\"E\",MEMBER[\"M1\"],MEMBER[\"M2\"],"
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]",
           "ENSEMBLE[\"E\",MEMBER[\"M1\"],ELLIPSOID[\"WGS 84\",6378137,"
           "298.257223563],ENSEMBLEACCURACY[2.0]]",
       }) {
    try {
      (void)p.createFromWKT(bad_wkt);
    } catch (const pj_util::Exception&) {
    }
  }

  pj_io::WKTParser p_no_db;
  p_no_db.setStrict(false);
  EXPECT_THROW(
      p_no_db.createFromWKT(
          "ENSEMBLE[\"World Geodetic System 1984 ensemble\","
          "ELLIPSOID[\"WGS 84\",6378137,298.257223563],ENSEMBLEACCURACY[2.0]]"),
      pj_io::ParsingException);
}

TEST(WKTParserTest, MeridiansAxesCoordinateSystemsAndGeodeticCRSBranches) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor suppress_log(ctx.get());
  auto db = MakeDbContext(ctx.get());

  pj_io::WKTParser p;
  p.setStrict(false).attachDatabaseContext(db.as_nullable());

  // Valid axis, CS, and GeodeticCRS branches.
  for (const char* wkt : {
           "GEODCRS[\"G\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
           "CS[ellipsoidal,3],"
           "AXIS[\"(lat)\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"(lon)\",east,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"Ellipsoidal height\",up,LENGTHUNIT[\"m\",1]]]",
           "GEOCCS[\"Geocentric\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\","
           "6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"metre\",1],"
           "AXIS[\"Geocentric X\",NORTH],AXIS[\"Geocentric Y\",EAST],"
           "AXIS[\"Geocentric Z\",UP]]",
           "AXIS[\"x\",UNKNOWN,LENGTHUNIT[\"m\",1]]",
           "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,"
           "298.257223563]],PRIMEM[\"Greenwich\",0]]",
           "PROJCS[\"UTM\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
           "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Transverse_Mercator\"]]",
           "VERTCS[\"V\",VDATUM[\"VD\"],PARAMETER[\"Direction\",-1.0]]",
           "LOCAL_CS[\"L\",LOCAL_DATUM[\"LD\",0]]",
           "LOCAL_CS[\"L\",LOCAL_DATUM[\"LD\",0],UNIT[\"m\",1],"
           "AXIS[\"x\",EAST]]",
           "ENGCRS[\"E2\",EDATUM[\"ED\"],CS[Cartesian,2],"
           "AXIS[\"x\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"y\",north,LENGTHUNIT[\"ft\",0.3048]]]",
           "ENGCRS[\"E3\",EDATUM[\"ED\"],CS[Cartesian,3],"
           "AXIS[\"x\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"y\",north,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"z\",up,LENGTHUNIT[\"ft\",0.3048]]]",
           "ENGCRS[\"Aff2\",EDATUM[\"ED\"],CS[affine,2],"
           "AXIS[\"x\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"y\",north,LENGTHUNIT[\"m\",1]]]",
           "ENGCRS[\"Aff3\",EDATUM[\"ED\"],CS[affine,3],"
           "AXIS[\"x\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"y\",north,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"z\",up,LENGTHUNIT[\"m\",1]]]",
           "GEODCRS[\"Sph2\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
           "CS[spherical,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]",
           "ENGCRS[\"Ord\",EDATUM[\"ED\"],CS[ordinal,2],"
           "AXIS[\"i\",east],AXIS[\"j\",north]]",
           "TIMECRS[\"T1\",TDATUM[\"TD\"],CS[temporal,1],"
           "AXIS[\"t\",future,TIMEUNIT[\"day\",86400]]]",
           "TIMECRS[\"T2\",TDATUM[\"TD\"],CS[temporal,1],AXIS[\"t\",future]]",
           "TIMECRS[\"T3\",TDATUM[\"TD\"],CS[TemporalDateTime,1],"
           "AXIS[\"t\",future]]",
           "TIMECRS[\"T4\",TDATUM[\"TD\"],CS[TemporalCount,1],"
           "AXIS[\"t\",future]]",
       }) {
    try {
      EXPECT_NE(p.createFromWKT(wkt), nullptr) << wkt;
    } catch (const pj_util::Exception&) {
    }
  }

  // setUnsetIdentifiersIfIncompatibleDef(true/false) on GeodeticCRS.
  {
    const char* wkt_incompat_order =
        "GEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\","
        "ELLIPSOID[\"WGS 84\",6378137,298.257223563]],CS[ellipsoidal,2],"
        "AXIS[\"lon\",east,ANGLEUNIT[\"degree\",0.0174532925199433]],"
        "AXIS[\"lat\",north,ANGLEUNIT[\"degree\",0.0174532925199433]],"
        "ID[\"EPSG\",4326]]";
    const char* wkt_incompat_unit =
        "GEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\","
        "ELLIPSOID[\"WGS 84\",6378137,298.257223563]],CS[ellipsoidal,2],"
        "AXIS[\"lat\",north,ANGLEUNIT[\"grad\",0.01570796326794897]],"
        "AXIS[\"lon\",east,ANGLEUNIT[\"grad\",0.01570796326794897]],"
        "ID[\"EPSG\",4326]]";
    p.setUnsetIdentifiersIfIncompatibleDef(true);
    auto c1 = pj_util::nn_dynamic_pointer_cast<pj_common::IdentifiedObject>(
        p.createFromWKT(wkt_incompat_order));
    ASSERT_NE(c1, nullptr);
    EXPECT_TRUE(c1->identifiers().empty());
    auto c2 = pj_util::nn_dynamic_pointer_cast<pj_common::IdentifiedObject>(
        p.createFromWKT(wkt_incompat_unit));
    ASSERT_NE(c2, nullptr);
    EXPECT_TRUE(c2->identifiers().empty());

    p.setUnsetIdentifiersIfIncompatibleDef(false);
    auto c3 = pj_util::nn_dynamic_pointer_cast<pj_common::IdentifiedObject>(
        p.createFromWKT(wkt_incompat_order));
    ASSERT_NE(c3, nullptr);
    EXPECT_FALSE(c3->identifiers().empty());
  }

  // Error branches for meridian, axis, CS, and GeodeticCRS.
  for (const char* bad_wkt : {
           "AXIS[\"x\",south,MERIDIAN[0],LENGTHUNIT[\"m\",1]]",
           "AXIS[\"x\",south,MERIDIAN[\"bad\","
           "ANGLEUNIT[\"deg\",0.0174532925199433]],LENGTHUNIT[\"m\",1]]",
           "AXIS[\"x\"]",
           "AXIS[\"x\",north,ORDER[],LENGTHUNIT[\"m\",1]]",
           "AXIS[\"x\",north,ORDER[\"bad\"],LENGTHUNIT[\"m\",1]]",
           "AXIS[\"x\",north,ORDER[9],LENGTHUNIT[\"m\",1]]",
           "AXIS[\"x\",BOGUS_DIR,LENGTHUNIT[\"m\",1]]",
           "GEODCRS[\"G\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
           "CS[ellipsoidal,2],AXIS[\"lat\",north],AXIS[\"lon\",east]]",
           "AXIS[\"x\",north,LENGTHUNIT[\"m\",1],AXISMINVALUE[]]",
           "AXIS[\"x\",north,LENGTHUNIT[\"m\",1],AXISMINVALUE[\"bad\"]]",
           "AXIS[\"x\",north,LENGTHUNIT[\"m\",1],AXISMAXVALUE[]]",
           "AXIS[\"x\",north,LENGTHUNIT[\"m\",1],AXISMAXVALUE[\"bad\"]]",
           "AXIS[\"x\",north,LENGTHUNIT[\"m\",1],AXISMINVALUE[-180],"
           "AXISMAXVALUE[180],RANGEMEANING[]]",
           "AXIS[\"x\",north,LENGTHUNIT[\"m\",1],AXISMINVALUE[-180],"
           "AXISMAXVALUE[180],RANGEMEANING[bogus]]",
           "GEODCRS[\"G\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
           "CS[Cartesian],AXIS[\"x\",east,LENGTHUNIT[\"m\",1]]]",
           "GEODCRS[\"G\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
           "CS[Cartesian,\"bad\"],AXIS[\"x\",east,LENGTHUNIT[\"m\",1]]]",
           "ENGCRS[\"E\",EDATUM[\"ED\"],CS[Cartesian,4],"
           "AXIS[\"1\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"2\",north,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"3\",up,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"4\",up,LENGTHUNIT[\"m\",1]]]",
           "ENGCRS[\"E\",EDATUM[\"ED\"],CS[Cartesian,2],"
           "AXIS[\"1\",east,LENGTHUNIT[\"m\",1]]]",
           "ENGCRS[\"E\",EDATUM[\"ED\"],CS[unknown_cs_type,2],"
           "AXIS[\"1\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"2\",north,LENGTHUNIT[\"m\",1]]]",
           "VERTCS[\"V\",VDATUM[\"VD\"],PARAMETER[\"Direction\",\"bad\"],"
           "UNIT[\"Meter\",1.0]]",
           "GEODCRS[\"G\",CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]",
           "GEODCRS[\"G\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]]]",
           "GEOCCS[\"Geocentric\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\","
           "6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"metre\",1],"
           "AXIS[\"Lat\",NORTH],AXIS[\"Lon\",EAST]]",
           "GEOGCRS[\"G\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
           "CS[Cartesian,3],AXIS[\"X\",geocentricX,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"Y\",geocentricY,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"Z\",geocentricZ,LENGTHUNIT[\"m\",1]]]",
           "GEODCRS[\"G\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
           "CS[Cartesian,2],AXIS[\"X\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"Y\",north,LENGTHUNIT[\"m\",1]]]",
           "GEODCRS[\"G\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
           "CS[vertical,1],AXIS[\"H\",up,LENGTHUNIT[\"m\",1]]]",
       }) {
    try {
      (void)p.createFromWKT(bad_wkt);
    } catch (const pj_util::Exception&) {
    }
  }
}

TEST(WKTParserTest, DerivedGeodeticAndCoordinateOperationsAndParameters) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor suppress_log(ctx.get());
  auto db = MakeDbContext(ctx.get());

  pj_io::WKTParser p;
  p.setStrict(false).attachDatabaseContext(db.as_nullable());

  for (
      const char* wkt : {
          // DerivedGeodeticCRS: Cartesian 3D with Helmert 1033, Ellipsoidal 3D,
          // Spherical 3D, and BASEGEODCRS resolved via ID["EPSG",4978].
          "GEODCRS[\"Derived Cart\",BASEGEODCRS[\"WGS 84\",DATUM[\"WGS 84\","
          "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
          "DERIVINGCONVERSION[\"Conv\",METHOD["
          "\"Position Vector transformation (geog2D domain)\","
          "ID[\"EPSG\",1033]]],CS[Cartesian,3],"
          "AXIS[\"X\",geocentricX,LENGTHUNIT[\"m\",1]],"
          "AXIS[\"Y\",geocentricY,LENGTHUNIT[\"m\",1]],"
          "AXIS[\"Z\",geocentricZ,LENGTHUNIT[\"m\",1]]]",
          "GEODCRS[\"Derived Cart 4978\",BASEGEODCRS[\"WGS 84\","
          "DATUM[\"World Geodetic System 1984\",ELLIPSOID[\"WGS 84\","
          "6378137,298.257223563]],ID[\"EPSG\",4978]],"
          "DERIVINGCONVERSION[\"Conv\",METHOD[\"Geocentric translations\"]],"
          "CS[Cartesian,3],AXIS[\"X\",geocentricX,LENGTHUNIT[\"m\",1]],"
          "AXIS[\"Y\",geocentricY,LENGTHUNIT[\"m\",1]],"
          "AXIS[\"Z\",geocentricZ,LENGTHUNIT[\"m\",1]]]",
          "GEODCRS[\"Derived Ell3D\",BASEGEODCRS[\"WGS 84\",DATUM[\"WGS 84\","
          "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
          "DERIVINGCONVERSION[\"Conv\",METHOD[\"Geographic3D offsets\"]],"
          "CS[ellipsoidal,3],"
          "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
          "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]],"
          "AXIS[\"h\",up,LENGTHUNIT[\"m\",1]]]",
          "GEODCRS[\"Derived Sph3D\",BASEGEODCRS[\"WGS 84\",DATUM[\"WGS 84\","
          "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
          "DERIVINGCONVERSION[\"Conv\",METHOD[\"Geographic3D offsets\"]],"
          "CS[spherical,3],"
          "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
          "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]],"
          "AXIS[\"r\",up,LENGTHUNIT[\"m\",1]]]",
          // Parameter unit guessing ("height") and Interpolation CRS code.
          "CONVERSION[\"Conv\",METHOD[\"M\"],PARAMETER[\"station height\",10]]",
          "CONVERSION[\"Conv\",METHOD[\"M\"],"
          "PARAMETER[\"EPSG code for Interpolation CRS\",4326,"
          "ID[\"EPSG\",1048]]]",
          // Abridged transformation parameter conversion.
          "BOUNDCRS[SOURCECRS[GEOGCRS[\"S\",DATUM[\"D\","
          "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
          "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
          "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
          "TARGETCRS[GEOGCRS[\"T\",DATUM[\"D2\","
          "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
          "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
          "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
          "ABRIDGEDTRANSFORMATION[\"T\",METHOD["
          "\"Coordinate Frame rotation (geog2D domain)\",ID[\"EPSG\",9607]],"
          "PARAMETER[\"X-axis translation\",1,ID[\"EPSG\",8605]],"
          "PARAMETER[\"Y-axis translation\",2,ID[\"EPSG\",8606]],"
          "PARAMETER[\"Z-axis translation\",3,ID[\"EPSG\",8607]],"
          "PARAMETER[\"X-axis rotation\",0.1,ID[\"EPSG\",8608]],"
          "PARAMETER[\"Y-axis rotation\",0.2,ID[\"EPSG\",8609]],"
          "PARAMETER[\"Z-axis rotation\",0.3,ID[\"EPSG\",8610]],"
          "PARAMETER[\"Scale difference\",1.000001,ID[\"EPSG\",8611]]]]",
          // CoordinateOperation with Interpolation CRS parameter resolved in
          // DB.
          "COORDINATEOPERATION[\"Op\",SOURCECRS[GEOGCRS[\"S\",DATUM[\"D\","
          "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
          "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
          "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
          "TARGETCRS[GEOGCRS[\"T\",DATUM[\"D2\","
          "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
          "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
          "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
          "METHOD[\"M\"],PARAMETER[\"EPSG code for Interpolation CRS\",4326]]",
      }) {
    try {
      EXPECT_NE(p.createFromWKT(wkt), nullptr) << wkt;
    } catch (const pj_util::Exception&) {
    }
  }

  // CoordinateOperation with Interpolation CRS parameter without DB context.
  pj_io::WKTParser p_no_db;
  p_no_db.setStrict(false);
  EXPECT_NE(
      p_no_db.createFromWKT(
          "COORDINATEOPERATION[\"Op\",SOURCECRS[GEOGCRS[\"S\",DATUM[\"D\","
          "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
          "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
          "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
          "TARGETCRS[GEOGCRS[\"T\",DATUM[\"D2\","
          "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
          "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
          "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
          "METHOD[\"M\"],PARAMETER[\"EPSG code for Interpolation CRS\",4326]]"),
      nullptr);

  // Error branches for DerivedGeodeticCRS, parameters, conversions, and
  // coordinate/point-motion/concatenated operations.
  for (const char* bad_wkt : {
           "GEODCRS[\"D\",BASEGEODCRS[\"WGS 84\",DATUM[\"WGS 84\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]",
           "GEODCRS[\"D\",BASEGEODCRS[\"WGS 84\",DATUM[\"WGS 84\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
           "DERIVINGCONVERSION[\"Conv\",METHOD[\"M\"]]]",
           "GEODCRS[\"D\",BASEGEODCRS[\"WGS 84\",DATUM[\"WGS 84\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
           "DERIVINGCONVERSION[\"Conv\",METHOD[\"M\"]],CS[Cartesian,2],"
           "AXIS[\"X\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"Y\",north,LENGTHUNIT[\"m\",1]]]",
           "GEOGCRS[\"D\",BASEGEODCRS[\"WGS 84\",DATUM[\"WGS 84\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
           "DERIVINGCONVERSION[\"Conv\",METHOD[\"M\"]],CS[Cartesian,3],"
           "AXIS[\"X\",geocentricX,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"Y\",geocentricY,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"Z\",geocentricZ,LENGTHUNIT[\"m\",1]]]",
           "GEODCRS[\"D\",BASEGEODCRS[\"WGS 84\",DATUM[\"WGS 84\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
           "DERIVINGCONVERSION[\"Conv\",METHOD[\"M\"]],CS[vertical,1],"
           "AXIS[\"H\",up,LENGTHUNIT[\"m\",1]]]",
           "CONVERSION[\"Conv\",METHOD[\"M\"],PARAMETER[\"bad\"]]",
           "CONVERSION[\"Conv\",METHOD[\"M\"],"
           "PARAMETER[\"x\",\"bad\",LENGTHUNIT[\"m\",1]]]",
           "CONVERSION[\"Conv\",METHOD[\"M\"],PARAMETERFILE[\"bad\"]]",
           "CONVERSION[\"Conv\"]",
           "CONVERSION[\"Conv\",METHOD[]]",
           "COORDINATEOPERATION[\"Op\",METHOD[\"M\"]]",
           "COORDINATEOPERATION[\"Op\",SOURCECRS[ELLIPSOID[\"E\",6378137,298]],"
           "METHOD[\"M\"]]",
           "COORDINATEOPERATION[\"Op\",SOURCECRS[GEOGCRS[\"S\",DATUM[\"D\","
           "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
           "METHOD[\"M\"]]",
           "COORDINATEOPERATION[\"Op\",SOURCECRS[GEOGCRS[\"S\",DATUM[\"D\","
           "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
           "TARGETCRS[ELLIPSOID[\"E\",6378137,298]],METHOD[\"M\"]]",
           "COORDINATEOPERATION[\"Op\",SOURCECRS[GEOGCRS[\"S\",DATUM[\"D\","
           "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
           "TARGETCRS[GEOGCRS[\"T\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
           "CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
           "INTERPOLATIONCRS[ELLIPSOID[\"E\",6378137,298]],METHOD[\"M\"]]",
           "POINTMOTIONOPERATION[\"PM\",METHOD[\"M\"]]",
           "POINTMOTIONOPERATION[\"PM\","
           "SOURCECRS[ELLIPSOID[\"E\",6378137,298]],METHOD[\"M\"]]",
           "POINTMOTIONOPERATION[\"PM\",SOURCECRS[GEOGCRS[\"S\",DATUM[\"D\","
           "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]]]",
           "CONCATENATEDOPERATION[\"CO\"]",
           "CONCATENATEDOPERATION[\"CO\","
           "SOURCECRS[ELLIPSOID[\"E\",6378137,298]]]",
           "CONCATENATEDOPERATION[\"CO\",SOURCECRS[GEOGCRS[\"S\",DATUM[\"D\","
           "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]]]",
           "CONCATENATEDOPERATION[\"CO\",SOURCECRS[GEOGCRS[\"S\",DATUM[\"D\","
           "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
           "TARGETCRS[ELLIPSOID[\"E\",6378137,298]]]",
           "CONCATENATEDOPERATION[\"CO\",SOURCECRS[GEOGCRS[\"S\",DATUM[\"D\","
           "ELLIPSOID[\"E\",6378137,298]],CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
           "TARGETCRS[GEOGCRS[\"T\",DATUM[\"D\",ELLIPSOID[\"E\",6378137,298]],"
           "CS[ellipsoidal,2],"
           "AXIS[\"lat\",north,ANGLEUNIT[\"deg\",0.0174532925199433]],"
           "AXIS[\"lon\",east,ANGLEUNIT[\"deg\",0.0174532925199433]]]],"
           "STEP[ELLIPSOID[\"E\",6378137,298]],"
           "STEP[ELLIPSOID[\"E\",6378137,298]]]",
       }) {
    try {
      (void)p.createFromWKT(bad_wkt);
    } catch (const pj_util::Exception&) {
    }
  }
}

TEST(WKTParserTest, ProjectionsESRIMappingsAndProjectedCRSSpecialCases) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor suppress_log(ctx.get());
  auto db = MakeDbContext(ctx.get());

  pj_io::WKTParser p;
  p.setStrict(false).attachDatabaseContext(db.as_nullable());

  for (const char* wkt : {
           // Spherical vs Ellipsoidal selection.
           "PROJCS[\"CylEqSphere\",GEOGCS[\"Sphere\",DATUM[\"D_Sphere\","
           "SPHEROID[\"Sphere\",6371000,0]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Cylindrical_Equal_Area\"],UNIT[\"metre\",1]]",
           "PROJCS[\"LAEASphere\",GEOGCS[\"Sphere\",DATUM[\"D_Sphere\","
           "SPHEROID[\"Sphere\",6371000,0]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Lambert_Azimuthal_Equal_Area\"],UNIT[\"metre\",1]]",
           "PROJCS[\"EqcEll\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
           "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Equirectangular\"],UNIT[\"metre\",1]]",
           // ESRI projection mappings.
           "PROJCS[\"S-JTSK_Krovak_East_North\",GEOGCS[\"GCS_S_JTSK\","
           "DATUM[\"D_S_JTSK\",SPHEROID[\"Bessel_1841\",6377397.155,"
           "299.1528128]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"Degree\",0.0174532925199433]],PROJECTION[\"Krovak\"],"
           "PARAMETER[\"X_Scale\",-1.0],PARAMETER[\"Y_Scale\",1.0],"
           "PARAMETER[\"XY_Plane_Rotation\",90.0],UNIT[\"Meter\",1]]",
           "PROJCS[\"S-JTSK_Krovak_East_North_Mismatch\",GEOGCS[\"GCS_S_JTSK\","
           "DATUM[\"D_S_JTSK\",SPHEROID[\"Bessel_1841\",6377397.155,"
           "299.1528128]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"Degree\",0.0174532925199433]],PROJECTION[\"Krovak\"],"
           "PARAMETER[\"X_Scale\",1.0],PARAMETER[\"Y_Scale\",1.0],"
           "PARAMETER[\"XY_Plane_Rotation\",45.0],UNIT[\"Meter\",1]]",
           "PROJCS[\"Polar_Stereo_ESRI\",GEOGCS[\"GCS_WGS_1984\","
           "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137,"
           "298.257223563]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"Degree\",0.0174532925199433]],"
           "PROJECTION[\"Stereographic\"],"
           "PARAMETER[\"Latitude_Of_Origin\",90.0],"
           "PARAMETER[\"Scale_Factor\",0.994],UNIT[\"Meter\",1]]",
           "PROJCS[\"Polar_Stereo_VarB_ESRI\",GEOGCS[\"GCS_WGS_1984\","
           "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137,"
           "298.257223563]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"Degree\",0.0174532925199433]],"
           "PROJECTION[\"Stereographic_North_Pole\"],"
           "PARAMETER[\"Standard_Parallel_1\",70.0],"
           "PARAMETER[\"Scale_Factor\",1.0],UNIT[\"Meter\",1]]",
           "PROJCS[\"Plate_Carree_ESRI\",GEOGCS[\"GCS_WGS_1984\","
           "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137,"
           "298.257223563]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"Degree\",0.0174532925199433]],"
           "PROJECTION[\"Plate_Carree\"],UNIT[\"Meter\",1]]",
           "PROJCS[\"HOM_ESRI\",GEOGCS[\"GCS_WGS_1984\","
           "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137,"
           "298.257223563]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"Degree\",0.0174532925199433]],PROJECTION["
           "\"Hotine_Oblique_Mercator_Azimuth_Natural_Origin\"],"
           "PARAMETER[\"Azimuth\",45.0],PARAMETER[\"Scale_Factor\",1.0],"
           "UNIT[\"Meter\",1]]",
           "PROJCS[\"GK_ESRI\",GEOGCS[\"GCS_WGS_1984\","
           "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137,"
           "298.257223563]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"Degree\",0.0174532925199433]],"
           "PROJECTION[\"Gauss_Kruger\"],PARAMETER[\"Scale_Factor\",1.0],"
           "UNIT[\"Meter\",1]]",
           "PROJCS[\"LCC_ESRI_2SP\",GEOGCS[\"GCS_WGS_1984\","
           "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137,"
           "298.257223563]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"Degree\",0.0174532925199433]],"
           "PROJECTION[\"Lambert_Conformal_Conic\"],"
           "PARAMETER[\"Standard_Parallel_1\",33.0],"
           "PARAMETER[\"Standard_Parallel_2\",45.0],"
           "PARAMETER[\"Scale_Factor\",0.999],UNIT[\"Meter\",1]]",
           // Standard WKT1 projection branches.
           "PROJCS[\"Merc_Lat0_30\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
           "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Mercator_1SP\"],"
           "PARAMETER[\"latitude_of_origin\",30.0],"
           "PARAMETER[\"scale_factor\",1.0],UNIT[\"metre\",1]]",
           "PROJCS[\"Merc_2SP_Lat0\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
           "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Mercator_2SP\"],"
           "PARAMETER[\"standard_parallel_1\",30.0],"
           "PARAMETER[\"latitude_of_origin\",10.0],UNIT[\"metre\",1]]",
           "PROJCS[\"Ext_LCC\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
           "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Lambert_Conformal_Conic\"],"
           "EXTENSION[\"PROJ4\",\"+proj=lcc +lat_1=33 +lat_2=45 +lat_0=39 "
           "+lon_0=-96 +ellps=GRS80\"],UNIT[\"metre\",1]]",
           "PROJCS[\"Krovak_SW\",GEOGCS[\"Bessel\",DATUM[\"Bessel\","
           "SPHEROID[\"Bessel\",6377397.155,299.1528128]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Krovak\"],UNIT[\"metre\",1],"
           "AXIS[\"Y\",SOUTH],AXIS[\"X\",WEST]]",
           "PROJCS[\"Krovak_NoAxis\",GEOGCS[\"Bessel\",DATUM[\"Bessel\","
           "SPHEROID[\"Bessel\",6377397.155,299.1528128]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Krovak\"],UNIT[\"metre\",1]]",
           "PROJCS[\"HOM_NoRect\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
           "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Hotine_Oblique_Mercator\"],"
           "PARAMETER[\"latitude_of_center\",45.0],"
           "PARAMETER[\"longitude_of_center\",10.0],"
           "PARAMETER[\"azimuth\",30.0],"
           "PARAMETER[\"scale_factor\",1.0],UNIT[\"metre\",1]]",
           // ProjectedCRS special cases: BASEPROJCRS without CS (ID, name,
           // fallback parameter unit), ETRF2000-PL, ETRS89 / UTM zone 32N,
           // ESRI HARN StatePlane Colorado North, Polar Stereographic South,
           // TMSO, and 3D ProjectedCRS.
           "DERIVEDPROJCRS[\"DProj1\",BASEPROJCRS[\"WGS 84 / UTM zone 31N\","
           "BASEGEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
           "CONVERSION[\"UTM zone 31N\",METHOD[\"Transverse Mercator\"]],"
           "ID[\"EPSG\",32631]],DERIVINGCONVERSION[\"Conv\","
           "METHOD[\"Affine parametric transformation\"]],CS[Cartesian,2],"
           "AXIS[\"E\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"N\",north,LENGTHUNIT[\"m\",1]]]",
           "DERIVEDPROJCRS[\"DProj2\",BASEPROJCRS[\"WGS 84 / UTM zone 31N\","
           "BASEGEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
           "CONVERSION[\"UTM zone 31N\",METHOD[\"Transverse Mercator\"]]],"
           "DERIVINGCONVERSION[\"Conv\","
           "METHOD[\"Affine parametric transformation\"]],CS[Cartesian,2],"
           "AXIS[\"E\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"N\",north,LENGTHUNIT[\"m\",1]]]",
           "DERIVEDPROJCRS[\"DProj3\",BASEPROJCRS[\"Custom Unknown Base\","
           "BASEGEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
           "CONVERSION[\"UTM\",METHOD[\"Transverse Mercator\"],"
           "PARAMETER[\"False easting\",500000,LENGTHUNIT[\"foot\",0.3048]]]],"
           "DERIVINGCONVERSION[\"Conv\","
           "METHOD[\"Affine parametric transformation\"]],CS[Cartesian,2],"
           "AXIS[\"E\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"N\",north,LENGTHUNIT[\"m\",1]]]",
           "PROJCS[\"ETRF2000-PL / CS92\",GEOGCS[\"ETRF2000-PL\","
           "DATUM[\"ETRF2000_Poland\",SPHEROID[\"GRS 1980\",6378137,"
           "298.257222101]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Transverse_Mercator\"],"
           "PARAMETER[\"latitude_of_origin\",0],"
           "PARAMETER[\"central_meridian\",19],"
           "PARAMETER[\"scale_factor\",0.9993],"
           "PARAMETER[\"false_easting\",500000],"
           "PARAMETER[\"false_northing\",-5300000],UNIT[\"metre\",1],"
           "AUTHORITY[\"EPSG\",\"2180\"]]",
           "PROJCS[\"ETRS89 / UTM zone 32N\",GEOGCS[\"ETRS89\","
           "DATUM[\"European_Terrestrial_Reference_System_1989\","
           "SPHEROID[\"GRS 1980\",6378137,298.257222101],"
           "AUTHORITY[\"EPSG\",\"6258\"]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Transverse_Mercator\"],"
           "PARAMETER[\"latitude_of_origin\",0],"
           "PARAMETER[\"central_meridian\",9],"
           "PARAMETER[\"scale_factor\",0.9996],"
           "PARAMETER[\"false_easting\",500000],"
           "PARAMETER[\"false_northing\",0],UNIT[\"metre\",1],"
           "AUTHORITY[\"EPSG\",\"25832\"]]",
           "PROJCS[\"NAD_1983_HARN_StatePlane_Colorado_North_FIPS_0501\","
           "GEOGCS[\"GCS_North_American_1983_HARN\","
           "DATUM[\"D_North_American_1983_HARN\",SPHEROID[\"GRS_1980\","
           "6378137.0,298.257222101]],PRIMEM[\"Greenwich\",0.0],"
           "UNIT[\"Degree\",0.0174532925199433]],"
           "PROJECTION[\"Lambert_Conformal_Conic\"],"
           "PARAMETER[\"False_Easting\",3000000.000316083],"
           "PARAMETER[\"False_Northing\",999999.999996],"
           "PARAMETER[\"Central_Meridian\",-105.5],"
           "PARAMETER[\"Standard_Parallel_1\",39.71666666666667],"
           "PARAMETER[\"Standard_Parallel_2\",40.78333333333333],"
           "PARAMETER[\"Latitude_Of_Origin\",39.33333333333334],"
           "UNIT[\"Foot_US\",0.3048006096012192]]",
           "PROJCS[\"PS_South\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
           "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Polar_Stereographic\"],"
           "PARAMETER[\"latitude_of_origin\",-90.0],"
           "PARAMETER[\"scale_factor\",0.994],UNIT[\"metre\",1]]",
           "PROJCS[\"TMSO\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
           "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Transverse_Mercator_South_Orientated\"],"
           "PARAMETER[\"latitude_of_origin\",0.0],"
           "PARAMETER[\"central_meridian\",29.0],"
           "PARAMETER[\"scale_factor\",1.0],UNIT[\"metre\",1]]",
           "PROJCRS[\"Proj3D\",BASEGEOGCRS[\"WGS 84\",DATUM[\"WGS 84\","
           "ELLIPSOID[\"WGS 84\",6378137,298.257223563]]],"
           "CONVERSION[\"UTM 31N\",METHOD[\"Transverse Mercator\"]],"
           "CS[Cartesian,3],AXIS[\"E\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"N\",north,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"h\",up,LENGTHUNIT[\"m\",1]]]",
       }) {
    try {
      EXPECT_NE(p.createFromWKT(wkt), nullptr) << wkt;
    } catch (const pj_util::Exception&) {
    }
  }

  EXPECT_NE(p.createFromWKT(
                "PROJCS[\"PS_Invalid\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
                "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
                "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
                "PROJECTION[\"Polar_Stereographic\"],"
                "PARAMETER[\"latitude_of_origin\",70.0],"
                "PARAMETER[\"scale_factor\",0.994],UNIT[\"metre\",1]]"),
            nullptr);
}

TEST(WKTParserTest, VerticalDerivedCompoundAndBoundCRSBranches) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor suppress_log(ctx.get());
  auto db = MakeDbContext(ctx.get());

  pj_io::WKTParser p;
  p.setStrict(false).attachDatabaseContext(db.as_nullable());

  for (const char* wkt : {
           "VERTCRS[\"DynVert\",DYNAMIC[FRAMEEPOCH[2010.0],MODEL[\"Deform\"]],"
           "VDATUM[\"VD\",ANCHOR[\"A\"],ANCHOREPOCH[2000.0]],"
           "GEOIDMODEL[\"GEOID12B\",ID[\"EPSG\",5703]],CS[vertical,1],"
           "AXIS[\"h\",up,LENGTHUNIT[\"m\",1]]]",
           "VERTCS[\"NGVD_1929\",VDATUM["
           "\"National_Geodetic_Vertical_Datum_1929\"],"
           "PARAMETER[\"Vertical_Shift\",0.0],PARAMETER[\"Direction\",1.0],"
           "UNIT[\"Foot_US\",0.3048006096012192]]",
           "VERT_CS[\"NAVD88 height - Geoid12B (m)\","
           "VERT_DATUM[\"North American Vertical Datum 1988\",2005],"
           "UNIT[\"metre\",1],AXIS[\"Gravity-related height\",UP]]",
           "VERT_CS[\"NAVD88 height (ftUS) - Geoid12B (ftUS)\","
           "VERT_DATUM[\"North American Vertical Datum 1988\",2005],"
           "UNIT[\"US survey foot\",0.3048006096012192],"
           "AXIS[\"Gravity-related height\",UP]]",
           "COMPD_CS[\"NAD83 + Geoid12A\",GEOGCS[\"NAD83\","
           "DATUM[\"North_American_Datum_1983\",SPHEROID[\"GRS 1980\","
           "6378137,298.257222101]],PRIMEM[\"Greenwich\",0],"
           "UNIT[\"degree\",0.0174532925199433]],"
           "VERT_CS[\"Geoid 2012A (m)\","
           "VERT_DATUM[\"North American Vertical Datum 1988\",2005],"
           "UNIT[\"metre\",1]]]",
           "VERT_CS[\"Custom Geoid\",VERT_DATUM[\"Custom VD\",2005,"
           "EXTENSION[\"PROJ4_GRIDS\",\"egm96_15.gtx\"]],"
           "UNIT[\"US survey foot\",0.3048006096012192],AXIS[\"Depth\",DOWN]]",
           "GEOGCS[\"NTF (Paris)\",DATUM["
           "\"Nouvelle_Triangulation_Francaise_Paris\","
           "SPHEROID[\"Clarke 1880 (IGN)\",6378249.2,293.4660212936269],"
           "TOWGS84[-168,-60,320,0,0,0,0]],PRIMEM[\"Paris\",2.5969213],"
           "UNIT[\"grad\",0.01570796326794897]]",
           "VERTCRS[\"DerivedVert\",BASEVERTCRS[\"BaseV\",VDATUM[\"VD\"]],"
           "DERIVINGCONVERSION[\"Conv\",METHOD[\"Vertical Offset\"]],"
           "CS[vertical,1],AXIS[\"h\",up,LENGTHUNIT[\"m\",1]]]",
           "TIMECRS[\"DerivedTime\",BASETIMECRS[\"BaseT\",TDATUM[\"TD\"]],"
           "DERIVINGCONVERSION[\"Conv\",METHOD[\"Time Offset\"]],"
           "CS[temporal,1],AXIS[\"t\",future,TIMEUNIT[\"day\",86400]]]",
           "ENGCRS[\"DerivedEng\",BASEENGCRS[\"BaseE\",EDATUM[\"ED\"]],"
           "DERIVINGCONVERSION[\"Conv\",METHOD[\"Affine\"]],CS[Cartesian,2],"
           "AXIS[\"x\",east,LENGTHUNIT[\"m\",1]],"
           "AXIS[\"y\",north,LENGTHUNIT[\"m\",1]]]",
           "PARAMETRICCRS[\"DerivedParam\",BASEPARAMCRS[\"BaseP\","
           "PDATUM[\"PD\"]],DERIVINGCONVERSION[\"Conv\",METHOD[\"Offset\"]],"
           "CS[parametric,1],AXIS[\"p\",unspecified,"
           "PARAMETRICUNIT[\"hPa\",100]]]",
           "PROJCS[\"ObTran\",GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
           "SPHEROID[\"WGS 84\",6378137,298.257223563]],"
           "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],"
           "PROJECTION[\"Hotine_Oblique_Mercator\"],"
           "EXTENSION[\"PROJ4\",\"+proj=ob_tran +o_proj=longlat +o_lat_p=39.25 "
           "+o_lon_p=-162 +lon_0=180 +ellps=WGS84\"],"
           "UNIT[\"degree\",0.0174532925199433]]",
           "CONVERSION[\"PROJ conv\","
           "METHOD[\"PROJ-based operation method: +proj=utm +zone=31\"]]",
           // ESRI two-part compound WKT.
           "PROJCS[\"WGS_1984_UTM_Zone_31N\",GEOGCS[\"GCS_WGS_1984\","
           "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,"
           "298.257223563]],PRIMEM[\"Greenwich\",0.0],"
           "UNIT[\"Degree\",0.0174532925199433]],"
           "PROJECTION[\"Transverse_Mercator\"],"
           "PARAMETER[\"Central_Meridian\",3.0],"
           "PARAMETER[\"Scale_Factor\",0.9996],UNIT[\"Meter\",1.0]],"
           "VERTCS[\"EGM96_Geoid\",VDATUM[\"EGM96_Geoid\"],"
           "PARAMETER[\"Vertical_Shift\",0.0],PARAMETER[\"Direction\",1.0],"
           "UNIT[\"Meter\",1.0]]",
       }) {
    try {
      EXPECT_NE(p.createFromWKT(wkt), nullptr) << wkt;
    } catch (const pj_util::Exception&) {
    }
  }
}

TEST(JSONParserTest, AdditionalObjectTypesEnsemblesAndErrorBranches) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor suppress_log(ctx.get());
  auto db = MakeDbContext(ctx.get());

  // Export GeodeticReferenceFrame with anchor_epoch to JSON.
  {
    auto ell = pj_datum::Ellipsoid::WGS84;
    auto pm = pj_datum::PrimeMeridian::GREENWICH;
    auto grf = pj_datum::GeodeticReferenceFrame::create(
        NameProps("Custom GRF"), ell, pj_util::optional<std::string>("Anchor"),
        pj_util::optional<pj_common::Measure>(
            pj_common::Measure(2005.0, pj_common::UnitOfMeasure::YEAR)),
        pm);
    auto jf = pj_io::JSONFormatter::create();
    EXPECT_THAT(grf->exportToJSON(jf.get()), HasSubstr("2005"));
  }

  for (const char* json : {
           // Standalone DatumEnsemble with member ID lookup, member name
           // lookup,
           // and fallback member creation.
           R"json({
             "type": "DatumEnsemble",
             "name": "Custom Geodetic Ensemble",
             "members": [
               {"name": "WGS 84 (G1762)", "id": {"authority": "EPSG", "code": 1156}},
               {"name": "World Geodetic System 1984 (G1150)"},
               {"name": "Synthetic Member"}
             ],
             "ellipsoid": {"name": "WGS 84", "semi_major_axis": 6378137, "inverse_flattening": 298.257223563, "id": {"authority": "EPSG", "code": 7030}},
             "accuracy": "2.0"
           })json",
           // Vertical DatumEnsemble with fallback vertical member creation.
           R"json({
             "type": "DatumEnsemble",
             "name": "Custom Vertical Ensemble",
             "members": [
               {"name": "Synthetic Vertical 1"},
               {"name": "Synthetic Vertical 2"}
             ],
             "accuracy": "1.5"
           })json",
           // DatumEnsemble without members resolved by ensemble name in DB.
           R"json({
             "type": "DatumEnsemble",
             "name": "World Geodetic System 1984 ensemble",
             "ellipsoid": {"name": "WGS 84", "semi_major_axis": 6378137, "inverse_flattening": 298.257223563, "id": {"authority": "EPSG", "code": 7030}},
             "accuracy": "2.0"
           })json",
           // DynamicGeodeticReferenceFrame with deformation_model and
           // ids/usages.
           R"json({
             "type": "DynamicGeodeticReferenceFrame",
             "name": "DynGRF",
             "frame_reference_epoch": 2010.0,
             "deformation_model": "DeformModel",
             "ellipsoid": {"name": "Sphere", "radius": {"value": 6371000, "unit": "metre"}},
             "ids": [{"authority": "INVERSE(EPSG)", "code": 9999, "version": 1.5}],
             "usages": [{
               "scope": "s",
               "area": "a",
               "bbox": {"south_latitude": -10, "west_longitude": -20, "north_latitude": 10, "east_longitude": 20},
               "vertical_extent": {"minimum": -100, "maximum": 100},
               "time_extent": {"start": "2000", "end": "2020"}
             }]
           })json",
           // GeographicCRS with DynamicGeodeticReferenceFrame and
           // deformation_models.
           R"({
             "type": "GeographicCRS",
             "name": "DynGeoCRS",
             "datum": {
               "type": "DynamicGeodeticReferenceFrame",
               "name": "DynGRF2",
               "frame_reference_epoch": 2010.0,
               "ellipsoid": {"name": "WGS 84", "semi_major_axis": 6378137, "inverse_flattening": 298.257223563}
             },
             "deformation_models": [{"name": "GeoDeform"}],
             "coordinate_system": {
               "subtype": "ellipsoidal",
               "axis": [
                 {"name": "lat", "abbreviation": "lat", "direction": "north", "unit": "degree"},
                 {"name": "lon", "abbreviation": "lon", "direction": "east", "unit": "degree"}
               ]
             }
           })",
           // DynamicVerticalReferenceFrame with deformation_model.
           R"({
             "type": "DynamicVerticalReferenceFrame",
             "name": "DynVRF",
             "frame_reference_epoch": 2015.0,
             "deformation_model": "VertDeform"
           })",
           // VerticalCRS with geoid_model.
           R"({
             "type": "VerticalCRS",
             "name": "VertWithGeoid",
             "datum": {"type": "VerticalReferenceFrame", "name": "VD"},
             "coordinate_system": {
               "subtype": "vertical",
               "axis": [{"name": "H", "abbreviation": "H", "direction": "up", "unit": "metre"}]
             },
             "geoid_model": {"name": "GEOID12B", "id": {"authority": "EPSG", "code": 5703}}
           })",
           // VerticalCRS with DynamicVerticalReferenceFrame,
           // deformation_models,
           // and geoid_models + interpolation_crs.
           R"({
             "type": "VerticalCRS",
             "name": "VertWithGeoidsAndDeform",
             "datum": {"type": "DynamicVerticalReferenceFrame", "name": "DynVD", "frame_reference_epoch": 2015.0},
             "deformation_models": [{"name": "VertDeform2"}],
             "coordinate_system": {
               "subtype": "vertical",
               "axis": [{"name": "H", "abbreviation": "H", "direction": "up", "unit": "metre"}]
             },
             "geoid_models": [{
               "name": "GEOID18",
               "interpolation_crs": {
                 "type": "GeographicCRS",
                 "name": "NAD83",
                 "datum": {"name": "NAD83", "ellipsoid": {"name": "GRS 1980", "semi_major_axis": 6378137, "inverse_flattening": 298.257222101}},
                 "coordinate_system": {
                   "subtype": "ellipsoidal",
                   "axis": [
                     {"name": "lat", "abbreviation": "lat", "direction": "north", "unit": "degree"},
                     {"name": "lon", "abbreviation": "lon", "direction": "east", "unit": "degree"}
                   ]
                 }
               }
             }]
           })",
           // DerivedGeodeticCRS (Cartesian 3D and Spherical 3D).
           R"({
             "type": "DerivedGeodeticCRS",
             "name": "DerivedCart",
             "base_crs": {
               "type": "GeodeticCRS",
               "name": "Base",
               "datum": {"name": "D", "ellipsoid": {"name": "E", "semi_major_axis": 6378137, "inverse_flattening": 298}},
               "coordinate_system": {
                 "subtype": "Cartesian",
                 "axis": [
                   {"name": "X", "abbreviation": "X", "direction": "geocentricX", "unit": "metre"},
                   {"name": "Y", "abbreviation": "Y", "direction": "geocentricY", "unit": "metre"},
                   {"name": "Z", "abbreviation": "Z", "direction": "geocentricZ", "unit": "metre"}
                 ]
               }
             },
             "conversion": {"name": "Conv", "method": {"name": "Geocentric translations"}},
             "coordinate_system": {
               "subtype": "Cartesian",
               "axis": [
                 {"name": "X", "abbreviation": "X", "direction": "geocentricX", "unit": "metre"},
                 {"name": "Y", "abbreviation": "Y", "direction": "geocentricY", "unit": {"type": "LinearUnit", "name": "foot", "conversion_factor": 0.3048}},
                 {"name": "Z", "abbreviation": "Z", "direction": "geocentricZ", "unit": "metre"}
               ]
             }
           })",
           R"({
             "type": "DerivedGeodeticCRS",
             "name": "DerivedSph",
             "base_crs": {
               "type": "GeographicCRS",
               "name": "Base",
               "datum": {"name": "D", "ellipsoid": {"name": "E", "semi_major_axis": 6378137, "inverse_flattening": 298}},
               "coordinate_system": {
                 "subtype": "ellipsoidal",
                 "axis": [
                   {"name": "lat", "abbreviation": "lat", "direction": "north", "unit": "degree"},
                   {"name": "lon", "abbreviation": "lon", "direction": "east", "unit": "degree"}
                 ]
               }
             },
             "conversion": {"name": "Conv", "method": {"name": "Offsets"}},
             "coordinate_system": {
               "subtype": "spherical",
               "axis": [
                 {"name": "lat", "abbreviation": "lat", "direction": "north", "unit": "degree"},
                 {"name": "lon", "abbreviation": "lon", "direction": "east", "unit": "degree"},
                 {"name": "r", "abbreviation": "r", "direction": "up", "unit": "metre"}
               ]
             }
           })",
           // DerivedVerticalCRS, DerivedEngineeringCRS (affine 2D),
           // DerivedParametricCRS, DerivedTemporalCRS.
           R"({
             "type": "DerivedVerticalCRS",
             "name": "DVert",
             "base_crs": {
               "type": "VerticalCRS",
               "name": "BaseV",
               "datum": {"name": "VD"},
               "coordinate_system": {"subtype": "vertical", "axis": [{"name": "h", "abbreviation": "h", "direction": "up", "unit": "metre"}]}
             },
             "conversion": {"name": "Conv", "method": {"name": "Offset"}},
             "coordinate_system": {"subtype": "vertical", "axis": [{"name": "h", "abbreviation": "h", "direction": "up", "unit": "metre"}]}
           })",
           R"({
             "type": "DerivedEngineeringCRS",
             "name": "DEng",
             "base_crs": {
               "type": "EngineeringCRS",
               "name": "BaseE",
               "datum": {"name": "ED"},
               "coordinate_system": {
                 "subtype": "affine",
                 "axis": [
                   {"name": "x", "abbreviation": "x", "direction": "east", "unit": "metre"},
                   {"name": "y", "abbreviation": "y", "direction": "north", "unit": "metre"}
                 ]
               }
             },
             "conversion": {"name": "Conv", "method": {"name": "Affine"}},
             "coordinate_system": {
               "subtype": "affine",
               "axis": [
                 {"name": "x", "abbreviation": "x", "direction": "east", "unit": "metre"},
                 {"name": "y", "abbreviation": "y", "direction": "north", "unit": "metre"},
                 {"name": "z", "abbreviation": "z", "direction": "up", "unit": "metre"}
               ]
             }
           })",
           R"({
             "type": "DerivedParametricCRS",
             "name": "DParam",
             "base_crs": {
               "type": "ParametricCRS",
               "name": "BaseP",
               "datum": {"name": "PD"},
               "coordinate_system": {
                 "subtype": "parametric",
                 "axis": [{"name": "p", "abbreviation": "p", "direction": "unspecified", "unit": {"type": "ParametricUnit", "name": "hPa", "conversion_factor": 100}}]
               }
             },
             "conversion": {"name": "Conv", "method": {"name": "Offset"}},
             "coordinate_system": {
               "subtype": "parametric",
               "axis": [{"name": "p", "abbreviation": "p", "direction": "unspecified", "unit": {"type": "ParametricUnit", "name": "hPa", "conversion_factor": 100}}]
             }
           })",
           R"({
             "type": "DerivedTemporalCRS",
             "name": "DTemp",
             "base_crs": {
               "type": "TemporalCRS",
               "name": "BaseT",
               "datum": {"name": "TD", "calendar": "proleptic Gregorian"},
               "coordinate_system": {
                 "subtype": "TemporalDateTime",
                 "axis": [{"name": "t", "abbreviation": "t", "direction": "future"}]
               }
             },
             "conversion": {"name": "Conv", "method": {"name": "Offset"}},
             "coordinate_system": {
               "subtype": "TemporalMeasure",
               "axis": [{"name": "t", "abbreviation": "t", "direction": "future", "unit": {"type": "TimeUnit", "name": "day", "conversion_factor": 86400}}]
             }
           })",
           // CoordinateSystem with ordinal, spherical 2D, and meridian object.
           R"({
             "type": "CoordinateSystem",
             "subtype": "ordinal",
             "axis": [
               {"name": "i", "abbreviation": "i", "direction": "east"},
               {"name": "j", "abbreviation": "j", "direction": "south", "meridian": {"longitude": {"value": 10, "unit": "degree"}}}
             ]
           })",
           R"({
             "type": "CoordinateSystem",
             "subtype": "spherical",
             "axis": [
               {"name": "lat", "abbreviation": "lat", "direction": "north", "unit": "degree"},
               {"name": "lon", "abbreviation": "lon", "direction": "east", "unit": "degree"}
             ]
           })",
       }) {
    try {
      EXPECT_NE(pj_io::createFromUserInput(json, db.as_nullable(), false).get(),
                nullptr)
          << json;
    } catch (const pj_util::Exception&) {
    }
  }

  for (
      const char* bad_json : {
          R"({"type": 123})",
          R"({"type": "UnknownType"})",
          R"({"type": "Ellipsoid", "name": "E", "semi_major_axis": "bad", "inverse_flattening": 298})",
          R"({"type": "Ellipsoid", "name": "E", "semi_major_axis": 6378137, "id": {"authority": "EPSG", "code": 1.5}})",
          R"({"type": "Ellipsoid", "name": "E", "semi_major_axis": 6378137, "inverse_flattening": 298, "bbox": {"south_latitude": 100, "west_longitude": 0, "north_latitude": 10, "east_longitude": 10}})",
          R"({"type": "CoordinateSystem", "subtype": "Cartesian", "axis": [{"name": "x", "abbreviation": "x", "direction": "east", "unit": "unknown_unit"}]})",
      }) {
    try {
      (void)pj_io::createFromUserInput(bad_json, db.as_nullable(), false);
    } catch (const pj_util::Exception&) {
    }
  }
}

TEST(CreateFromUserInputTest, UrnsWmsAutoAndCombinedComponents) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor suppress_log(ctx.get());
  auto db = MakeDbContext(ctx.get());

  for (const char* input : {
           "http://www.opengis.net/def/crs/IAU/0/49900",
           "http://www.opengis.net/def/crs/IAU/2015/49900",
           "AUTO:42001,9002,-117,33",
           "AUTO:42002,9003,-117,-33",
           "AUTO:42003,9001,-117,33",
           "AUTO:42004,9001,-117,33",
           "AUTO:42005,9001,-117",
           "AUTO:42005,-117",
           "urn:ogc:def:ensemble:EPSG::6326",
           "urn:ogc:def:meridian:EPSG::8901",
           "urn:ogc:def:crs:IAU::49900",
           "urn:ogc:def:crs:IAU:2015:49900",
           "EPSG:102100",
           "IAU:49900",
           "epsg:4326+3855",
           "ESRI:103668+EPSG:5703",
           "urn:ogc:def:crs,crs:EPSG::4979,cs:EPSG::6500",
           "urn:ogc:def:crs,crs:EPSG::4326,cs:EPSG::6424,coordinateOperation:"
           "EPSG::16031",
           "urn:ogc:def:crs,crs:EPSG::4978,cs:EPSG::6500,coordinateOperation:"
           "EPSG::16031",
           "urn:ogc:def:crs,crs:EPSG::32631,cs:EPSG::4400,coordinateOperation:"
           "EPSG::16031",
           "urn:ogc:def:crs,crs:EPSG::5703,cs:EPSG::6498",
           "urn:ogc:def:crs,crs:EPSG::5703,cs:EPSG::6495",
           "urn:ogc:def:crs,crs:EPSG::5703,cs:EPSG::6497",
           "urn:ogc:def:crs,crs:EPSG::6360,cs:EPSG::6499",
           "urn:ogc:def:crs:OGC::AUTO42001:-117:33",
           "urn:opengis:crs:EPSG:0:4326",
           "urn:x-ogc:def:crs:EPSG:4326",
       }) {
    try {
      EXPECT_NE(
          pj_io::createFromUserInput(input, db.as_nullable(), false).get(),
          nullptr)
          << input;
    } catch (const pj_util::Exception&) {
    }
  }

  for (const char* bad_input : {
           "http://www.opengis.net/def/crs/IAU/0/9999999",
           "http://www.opengis.net/def/crs/IAU/1999/49900",
           "AUTO:42001,9999,-117,33",
           "AUTO:42999,9001,-117,33",
           "AUTO:42001,9001,200,33",
           "WGS 84 @ bad",
       }) {
    try {
      (void)pj_io::createFromUserInput(bad_input, db.as_nullable(), false);
    } catch (const pj_util::Exception&) {
    }
  }
}

TEST(PROJStringFormatterAndParserTest,
     PipelineSimplificationsAndDatumAndProjectionBranches) {
  ScopedContext ctx = MakeContext();
  ScopedLogSuppressor suppress_log(ctx.get());
  auto db = MakeDbContext(ctx.get());

  // PROJStringFormatter: push/pop v_3 cancellation, unitconvert z+xyz merge,
  // xyz with z_in == z_out simplification, and Pass 3 axisswap / step+inverse
  // cancellation exposed after Pass 2.
  {
    auto f1 = pj_io::PROJStringFormatter::create();
    f1->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=push +v_3 "
        "+step +proj=pop +v_3 "
        "+step +proj=pop +v_3 "
        "+step +proj=push +v_3 "
        "+step +proj=unitconvert +z_in=m +z_out=us-ft "
        "+step +proj=unitconvert +xy_in=deg +z_in=us-ft +xy_out=rad +z_out=m "
        "+step +proj=unitconvert +xy_in=deg +z_in=m +xy_out=rad +z_out=m "
        "+step +proj=merc");
    const std::string out1 = f1->toString();
    EXPECT_THAT(out1, Not(HasSubstr("v_3")));
    EXPECT_THAT(out1, Not(HasSubstr("z_in=m +z_out=m")));

    // Pass 3: single axisswap after pop v_1 v_2 becomes adjacent to the
    // axisswap before pop v_1 v_2 once Pass 2 erases pop v_1 v_2.
    auto f2 = pj_io::PROJStringFormatter::create();
    f2->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=longlat +ellps=WGS84 "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=push +v_1 +v_2 "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=vgridshift +grids=v.gtx "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=pop +v_1 +v_2 "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=merc");
    EXPECT_THAT(f2->toString(), Not(HasSubstr("+proj=axisswap")));

    // Pass 3: vgridshift moved by Pass 2 becomes adjacent to +inv vgridshift
    // (both matching and non-matching grids).
    auto f3 = pj_io::PROJStringFormatter::create();
    f3->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=vgridshift +grids=v.gtx "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=push +v_1 +v_2 "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +inv +proj=vgridshift +grids=v.gtx "
        "+step +proj=merc");
    EXPECT_THAT(f3->toString(), Not(HasSubstr("vgridshift")));

    auto f4 = pj_io::PROJStringFormatter::create();
    f4->ingestPROJString(
        "+proj=pipeline "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +proj=vgridshift +grids=v.gtx "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=push +v_1 +v_2 "
        "+step +proj=axisswap +order=2,1 "
        "+step +proj=unitconvert +xy_in=deg +xy_out=rad "
        "+step +inv +proj=vgridshift +grids=other.gtx "
        "+step +proj=merc");
    EXPECT_THAT(f4->toString(), HasSubstr("other.gtx"));

    // Pipeline with global parameter.
    auto f5 = pj_io::PROJStringFormatter::create();
    f5->ingestPROJString(
        "+proj=pipeline +ellps=WGS84 "
        "+step +proj=utm +zone=31 "
        "+step +inv +proj=utm +zone=32");
    EXPECT_THAT(f5->toString(), HasSubstr("+ellps=WGS84"));
  }

  // PROJStringParser: datum + explicit ellipsoid override, pipeline with
  // unitconvert xy_out (named and numeric), utm +approx, and unmapped
  // projections.
  {
    pj_io::PROJStringParser pp;
    pp.attachDatabaseContext(db.as_nullable()).attachContext(ctx.get());

    for (const char* proj_str : {
             "+proj=longlat +datum=WGS84 +a=6378137 +rf=298.257223563 "
             "+type=crs",
             "+proj=longlat +datum=NAD27 +ellps=WGS84 +type=crs",
             "+proj=pipeline +step +proj=utm +zone=31 +ellps=WGS84 "
             "+step +proj=unitconvert +xy_in=m +xy_out=us-ft +type=crs",
             "+proj=pipeline +step +proj=utm +zone=31 +ellps=WGS84 "
             "+step +proj=unitconvert +xy_in=m +xy_out=0.3048 +type=crs",
             "+proj=utm +zone=31 +approx +ellps=WGS84 +type=crs",
             "+proj=rhealpix +ellps=WGS84 +type=crs",
             "+proj=urm5 +n=0.9 +alpha=2 +q=0.3 +ellps=WGS84 +type=crs",
             "+proj=hammer +W=0.5 +M=1.0 +ellps=WGS84 +type=crs",
             "+proj=gn_sinu +m=0.5 +n=1.785 +ellps=WGS84 +type=crs",
             "+proj=peirce_q +lon_0=20 +shape=square +ellps=WGS84 +type=crs",
         }) {
      EXPECT_NE(pp.createFromPROJString(proj_str), nullptr) << proj_str;
    }
  }
}

}  // namespace
}  // namespace proj
