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

// Unit tests for PROJ ISO19111 ConcatenatedOperation.
// See also:
// https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/concatenatedoperation.cpp

#include <memory>
#include <set>
#include <string>
#include <utility>
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
#include "third_party/proj4/proj/include/proj/nn.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_constants.h"

namespace proj {
namespace {

namespace pcommon = ::osgeo::proj::common;
namespace pcs = ::osgeo::proj::cs;
namespace pcrs = ::osgeo::proj::crs;
namespace pdatum = ::osgeo::proj::datum;
namespace pio = ::osgeo::proj::io;
namespace pmeta = ::osgeo::proj::metadata;
namespace pop = ::osgeo::proj::operation;
namespace putil = ::osgeo::proj::util;

using ::testing::HasSubstr;

putil::PropertyMap NameProps(const std::string& name) {
  return putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, name);
}

putil::PropertyMap CodeProps(const std::string& name, const std::string& cs,
                             const std::string& code) {
  return putil::PropertyMap()
      .set(pcommon::IdentifiedObject::NAME_KEY, name)
      .set(pmeta::Identifier::CODESPACE_KEY, cs)
      .set(pmeta::Identifier::CODE_KEY, code);
}

putil::PropertyMap EpsgIntProps(const std::string& name, int code) {
  return putil::PropertyMap()
      .set(pcommon::IdentifiedObject::NAME_KEY, name)
      .set(pmeta::Identifier::CODESPACE_KEY, pmeta::Identifier::EPSG)
      .set(pmeta::Identifier::CODE_KEY, code);
}

template <typename Target, typename Source>
putil::nn<std::shared_ptr<Target>> DynamicCastNN(
    const putil::nn<std::shared_ptr<Source>>& src) {
  auto casted = putil::nn_dynamic_pointer_cast<Target>(src);
  return NN_CHECK_THROW(casted);
}

pdatum::GeodeticReferenceFrameNNPtr MakeGeodeticDatum(const std::string& name,
                                                      double radius) {
  auto ellps = pdatum::Ellipsoid::createSphere(NameProps(name + " Ellipsoid"),
                                               pcommon::Length(radius));
  return pdatum::GeodeticReferenceFrame::create(
      NameProps(name + " Datum"), ellps, putil::optional<std::string>(),
      pdatum::PrimeMeridian::GREENWICH);
}

pcrs::GeographicCRSNNPtr MakeGeog2D(const std::string& name, double radius,
                                    const std::string& csName = "",
                                    const std::string& code = "") {
  putil::PropertyMap props =
      code.empty() ? NameProps(name) : CodeProps(name, csName, code);
  return pcrs::GeographicCRS::create(
      props, MakeGeodeticDatum(name, radius),
      pcs::EllipsoidalCS::createLatitudeLongitude(
          pcommon::UnitOfMeasure::DEGREE));
}

pcrs::GeographicCRSNNPtr MakeGeog3D(const std::string& name, double radius) {
  return pcrs::GeographicCRS::create(
      NameProps(name), MakeGeodeticDatum(name, radius),
      pcs::EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
          pcommon::UnitOfMeasure::DEGREE, pcommon::UnitOfMeasure::METRE));
}

pcrs::GeodeticCRSNNPtr MakeGeoc(const std::string& name, double radius) {
  return pcrs::GeodeticCRS::create(
      NameProps(name), MakeGeodeticDatum(name, radius),
      pcs::CartesianCS::createGeocentric(pcommon::UnitOfMeasure::METRE));
}

pcrs::VerticalCRSNNPtr MakeVert(const std::string& name, bool isUp = true,
                                const std::string& csName = "",
                                const std::string& code = "") {
  putil::PropertyMap props =
      code.empty() ? NameProps(name) : CodeProps(name, csName, code);
  auto datum = pdatum::VerticalReferenceFrame::create(
      NameProps(name + " Datum"), putil::optional<std::string>(),
      putil::optional<pdatum::RealizationMethod>());
  auto axis = pcs::CoordinateSystemAxis::create(
      NameProps(isUp ? "Gravity-related height" : "Depth"), isUp ? "H" : "D",
      isUp ? pcs::AxisDirection::UP : pcs::AxisDirection::DOWN,
      pcommon::UnitOfMeasure::METRE);
  auto vcs = pcs::VerticalCS::create(putil::PropertyMap(), axis);
  return pcrs::VerticalCRS::create(props, datum, vcs);
}

pcrs::ProjectedCRSNNPtr MakeUTMProj(const std::string& name,
                                    const pcrs::GeographicCRSNNPtr& base,
                                    int zone) {
  auto geodBase = putil::nn_static_pointer_cast<pcrs::GeodeticCRS>(base);
  auto conv = pop::Conversion::createUTM(NameProps("UTM zone"), zone, true);
  return pcrs::ProjectedCRS::create(
      NameProps(name), geodBase, conv,
      pcs::CartesianCS::createEastingNorthing(pcommon::UnitOfMeasure::METRE));
}

pop::TransformationNNPtr MakeGeo2DOffset(
    const std::string& name, const pcrs::CRSNNPtr& src,
    const pcrs::CRSNNPtr& tgt, double dlatDeg = 0.0, double dlonDeg = 0.0,
    const std::vector<pmeta::PositionalAccuracyNNPtr>& acc = {}) {
  return pop::Transformation::createGeographic2DOffsets(
      NameProps(name), src, tgt,
      pcommon::Angle(dlatDeg, pcommon::UnitOfMeasure::DEGREE),
      pcommon::Angle(dlonDeg, pcommon::UnitOfMeasure::DEGREE), acc);
}

pop::TransformationNNPtr MakeVertOffset(
    const std::string& name, const pcrs::CRSNNPtr& src,
    const pcrs::CRSNNPtr& tgt, double dhMetre = 0.0,
    const std::vector<pmeta::PositionalAccuracyNNPtr>& acc = {}) {
  return pop::Transformation::createVerticalOffset(
      NameProps(name), src, tgt,
      pcommon::Length(dhMetre, pcommon::UnitOfMeasure::METRE), acc);
}

template <typename T>
std::string ExportWkt(const T& obj,
                      pio::WKTFormatter::Convention convention =
                          pio::WKTFormatter::Convention::WKT2_2019,
                      const pio::DatabaseContextPtr& dbCtx = nullptr) {
  auto fmt = pio::WKTFormatter::create(convention, dbCtx);
  fmt->setMultiLine(false);
  return obj->exportToWKT(fmt.get());
}

template <typename T>
std::string ExportJson(const T& obj) {
  auto fmt = pio::JSONFormatter::create();
  fmt->setMultiLine(false);
  return obj->exportToJSON(fmt.get());
}

std::string ExportProj(const pop::CoordinateOperationNNPtr& op) {
  auto fmt = pio::PROJStringFormatter::create();
  return op->exportToPROJString(fmt.get());
}

std::string BuildConcatWkt(
    const pcrs::CRSNNPtr& sourceCrs, const pcrs::CRSNNPtr& targetCrs,
    const std::vector<pop::CoordinateOperationNNPtr>& steps,
    const std::string& name = "test_concat") {
  std::string wkt = "CONCATENATEDOPERATION[\"" + name + "\",SOURCECRS[" +
                    ExportWkt(sourceCrs) + "],TARGETCRS[" +
                    ExportWkt(targetCrs) + "]";
  for (const auto& step : steps) {
    wkt += ",STEP[" + ExportWkt(step) + "]";
  }
  wkt += "]";
  return wkt;
}

pop::ConcatenatedOperationNNPtr ParseConcatWkt(
    const std::string& wkt, const pio::DatabaseContextPtr& dbCtx = nullptr) {
  pio::WKTParser parser;
  if (dbCtx != nullptr) {
    parser.attachDatabaseContext(dbCtx);
  }
  return DynamicCastNN<pop::ConcatenatedOperation>(parser.createFromWKT(wkt));
}

TEST(ConcatenatedOperationTest, CreateValidationErrors) {
  auto g0 = MakeGeog2D("G0", 6370000.0);
  auto g1 = MakeGeog2D("G1", 6371000.0);
  auto g2 = MakeGeog2D("G2", 6372000.0);
  auto op0 = MakeGeo2DOffset("op0", g0, g1);

  EXPECT_THROW(pop::ConcatenatedOperation::create(NameProps("empty"), {}, {}),
               pop::InvalidOperation);
  EXPECT_THROW(
      pop::ConcatenatedOperation::create(NameProps("single"), {op0}, {}),
      pop::InvalidOperation);

  auto noCrsConv = pop::Conversion::createUTM(NameProps("utm32"), 32, true);
  EXPECT_THROW(pop::ConcatenatedOperation::create(NameProps("null_crs"),
                                                  {op0, noCrsConv}, {}),
               pop::InvalidOperation);

  auto mismatched = MakeGeo2DOffset("mismatch", g2, g0);
  EXPECT_THROW(pop::ConcatenatedOperation::create(NameProps("mismatch"),
                                                  {op0, mismatched}, {}),
               pop::InvalidOperation);
}

TEST(ConcatenatedOperationTest,
     RequiresPerCoordinateInputTimeAndBallparkPropagation) {
  auto geoc0 = MakeGeoc("Geoc0", 6370000.0);
  auto geoc1 = MakeGeoc("Geoc1", 6371000.0);
  auto geoc2 = MakeGeoc("Geoc2", 6372000.0);

  auto tStatic = pop::Transformation::createGeocentricTranslations(
      NameProps("static1"), geoc0, geoc1, 1.0, 2.0, 3.0, {});
  auto tStatic2 = pop::Transformation::createGeocentricTranslations(
      NameProps("static2"), geoc1, geoc2, 4.0, 5.0, 6.0, {});

  auto concatStatic = pop::ConcatenatedOperation::create(
      NameProps("static_concat"), {tStatic, tStatic2}, {});
  EXPECT_FALSE(concatStatic->requiresPerCoordinateInputTime());
  EXPECT_FALSE(concatStatic->hasBallparkTransformation());
  EXPECT_EQ(concatStatic->operations().size(), 2U);

  auto tTimeDep = pop::Transformation::createTimeDependentPositionVector(
      NameProps("time_dep"), geoc1, geoc2, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
      0.01, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 2020.0, {});
  ASSERT_TRUE(tTimeDep->requiresPerCoordinateInputTime());

  auto concatTimeDep = pop::ConcatenatedOperation::create(
      NameProps("time_dep_concat"), {tStatic, tTimeDep}, {});
  EXPECT_TRUE(concatTimeDep->requiresPerCoordinateInputTime());

  auto tBallpark = pop::Transformation::createGeocentricTranslations(
      NameProps("Ballpark geocentric translation"), geoc1, geoc2, 0.0, 0.0, 0.0,
      {});
  ASSERT_TRUE(tBallpark->hasBallparkTransformation());

  auto concatBallpark = pop::ConcatenatedOperation::create(
      NameProps("ballpark_concat"), {tStatic, tBallpark}, {});
  EXPECT_TRUE(concatBallpark->hasBallparkTransformation());
}

TEST(ConcatenatedOperationTest, AreCRSMoreOrLessEquivalentBranches) {
  auto g0 = MakeGeog2D("G0", 6370000.0);
  auto g1IdMatchA = MakeGeog2D("G1A", 6371000.0, "TEST", "42");
  auto g1IdMatchB = MakeGeog2D("G1B", 6372000.0, "TEST", "42");
  auto g1DiffCode = MakeGeog2D("G1C", 6372000.0, "TEST", "43");
  auto g1DiffSpace = MakeGeog2D("G1D", 6372000.0, "OTHER", "42");
  auto g3 = MakeGeog2D("G3", 6373000.0);

  // Single-identifier match overrides differing underlying ellipsoids.
  auto opA = MakeGeo2DOffset("opA", g0, g1IdMatchA);
  auto opB = MakeGeo2DOffset("opB", g1IdMatchB, g3);
  auto idMatched =
      pop::ConcatenatedOperation::create(NameProps("id_match"), {opA, opB}, {});
  EXPECT_EQ(idMatched->operations().size(), 2U);

  EXPECT_THROW(pop::ConcatenatedOperation::create(
                   NameProps("diff_code"),
                   {opA, MakeGeo2DOffset("opDiffCode", g1DiffCode, g3)}, {}),
               pop::InvalidOperation);
  EXPECT_THROW(pop::ConcatenatedOperation::create(
                   NameProps("diff_space"),
                   {opA, MakeGeo2DOffset("opDiffSpace", g1DiffSpace, g3)}, {}),
               pop::InvalidOperation);

  // CompoundCRS vs component[1] VerticalCRS in both directions.
  auto vert = MakeVert("VShared");
  auto horizA = MakeGeog2D("HorizA", 6374000.0);
  auto horizB = MakeGeog2D("HorizB", 6375000.0);
  auto compA = pcrs::CompoundCRS::create(
      NameProps("CompA"), std::vector<pcrs::CRSNNPtr>{horizA, vert});
  auto compB = pcrs::CompoundCRS::create(
      NameProps("CompB"), std::vector<pcrs::CRSNNPtr>{horizB, vert});

  auto stepToCompA = MakeGeo2DOffset("toCompA", g0, compA);
  auto stepVertToG3 = MakeGeo2DOffset("vertToG3", vert, g3);
  auto concatCompLeft = pop::ConcatenatedOperation::create(
      NameProps("comp_left"), {stepToCompA, stepVertToG3}, {});
  EXPECT_EQ(concatCompLeft->operations().size(), 2U);

  auto stepG0ToVert = MakeGeo2DOffset("g0ToVert", g0, vert);
  auto stepCompBToG3 = MakeGeo2DOffset("compBToG3", compB, g3);
  auto concatCompRight = pop::ConcatenatedOperation::create(
      NameProps("comp_right"), {stepG0ToVert, stepCompBToG3}, {});
  EXPECT_EQ(concatCompRight->operations().size(), 2U);

  // Two differing CompoundCRS objects do not match via single fallback.
  EXPECT_THROW(pop::ConcatenatedOperation::create(
                   NameProps("comp_both"), {stepToCompA, stepCompBToG3}, {}),
               pop::InvalidOperation);
}

TEST(ConcatenatedOperationTest,
     InterpolationCRSAndVerticalGeogVerticalHeuristic) {
  auto g0 = MakeGeog2D("G0", 6370000.0);
  auto g1 = MakeGeog2D("G1", 6371000.0);
  auto g2 = MakeGeog2D("G2", 6372000.0);
  auto g3 = MakeGeog2D("G3", 6373000.0);
  auto interp1 = MakeGeog2D("Interp1", 6374000.0);
  auto interp2 = MakeGeog2D("Interp2", 6375000.0);

  auto makeWithInterp = [&](const std::string& name, const pcrs::CRSNNPtr& s,
                            const pcrs::CRSNNPtr& t,
                            const pcrs::CRSPtr& interp) {
    return pop::Transformation::create(
        NameProps(name), s, t, interp, NameProps("Dummy method"),
        std::vector<pop::OperationParameterNNPtr>{},
        std::vector<pop::ParameterValueNNPtr>{}, {});
  };

  auto t1 = makeWithInterp("t1", g0, g1, interp1.as_nullable());
  auto t2Same = makeWithInterp("t2Same", g1, g2, interp1.as_nullable());
  auto concatSame = pop::ConcatenatedOperation::create(NameProps("same_interp"),
                                                       {t1, t2Same}, {});
  ASSERT_NE(concatSame->interpolationCRS(), nullptr);
  EXPECT_TRUE(concatSame->interpolationCRS()->isEquivalentTo(interp1.get()));

  auto t2Diff = makeWithInterp("t2Diff", g1, g2, interp2.as_nullable());
  auto concatDiff = pop::ConcatenatedOperation::create(NameProps("diff_interp"),
                                                       {t1, t2Diff}, {});
  EXPECT_EQ(concatDiff->interpolationCRS(), nullptr);

  auto t2Null = makeWithInterp("t2Null", g1, g2, nullptr);
  auto t3Same = makeWithInterp("t3Same", g2, g3, interp1.as_nullable());
  auto concatNullThenSame = pop::ConcatenatedOperation::create(
      NameProps("null_then_same"), {t1, t2Null, t3Same}, {});
  EXPECT_EQ(concatNullThenSame->interpolationCRS(), nullptr);

  // Automatic VerticalCRS -> GeographicCRS -> VerticalCRS interpolationCRS.
  auto v0 = MakeVert("V0");
  auto v1 = MakeVert("V1");
  auto vMid = MakeVert("VMid");
  auto concatAutoInterp = pop::ConcatenatedOperation::create(
      NameProps("vert_geog_vert"),
      {MakeVertOffset("v0_g0", v0, g0), MakeVertOffset("g0_v1", g0, v1)}, {});
  ASSERT_NE(concatAutoInterp->interpolationCRS(), nullptr);
  EXPECT_TRUE(concatAutoInterp->interpolationCRS()->isEquivalentTo(g0.get()));

  // 3 steps does not trigger 2-step Vertical -> Geog -> Vertical heuristic.
  auto concatThreeSteps = pop::ConcatenatedOperation::create(
      NameProps("vert_three_steps"),
      {MakeVertOffset("s0", v0, g0), MakeGeo2DOffset("s1", g0, g0),
       MakeVertOffset("s2", g0, v1)},
      {});
  EXPECT_EQ(concatThreeSteps->interpolationCRS(), nullptr);

  // Middle CRS is VerticalCRS rather than GeographicCRS.
  auto concatAllVert = pop::ConcatenatedOperation::create(
      NameProps("vert_vert_vert"),
      {MakeVertOffset("va", v0, vMid), MakeVertOffset("vb", vMid, v1)}, {});
  EXPECT_EQ(concatAllVert->interpolationCRS(), nullptr);

  // Matching ID passes step chaining, but differing ellipsoids fail exact
  // GeographicCRS equivalence for automatic interpolationCRS.
  auto gIdA = MakeGeog2D("GeogIdA", 6370000.0, "TEST", "777");
  auto gIdB = MakeGeog2D("GeogIdB", 6371000.0, "TEST", "777");
  auto concatNonEquivGeog = pop::ConcatenatedOperation::create(
      NameProps("non_equiv_geog"),
      {MakeVertOffset("va", v0, gIdA), MakeVertOffset("vb", gIdB, v1)}, {});
  EXPECT_EQ(concatNonEquivGeog->interpolationCRS(), nullptr);
}

TEST(ConcatenatedOperationTest, FixStepsAxisOrderReversalAndEndpointReversals) {
  auto axis2d = pop::Conversion::createAxisOrderReversal(false);
  auto axis3d = pop::Conversion::createAxisOrderReversal(true);
  auto g4326Norm = pcrs::GeographicCRS::EPSG_4326->normalizeForVisualization();
  auto g4979Norm = pcrs::GeographicCRS::EPSG_4979->normalizeForVisualization();

  auto midTrans = MakeGeo2DOffset("norm_offset", g4326Norm, g4979Norm);
  std::string wktAxis = BuildConcatWkt(pcrs::GeographicCRS::EPSG_4326,
                                       pcrs::GeographicCRS::EPSG_4979,
                                       {axis2d, midTrans, axis3d});
  auto parsedAxis = ParseConcatWkt(wktAxis);
  ASSERT_EQ(parsedAxis->operations().size(), 3U);
  ASSERT_NE(parsedAxis->operations()[0]->sourceCRS(), nullptr);
  ASSERT_NE(parsedAxis->operations()[2]->targetCRS(), nullptr);
  EXPECT_TRUE(parsedAxis->operations()[0]->sourceCRS()->isEquivalentTo(
      pcrs::GeographicCRS::EPSG_4326.get()));
  EXPECT_TRUE(parsedAxis->operations()[2]->targetCRS()->isEquivalentTo(
      pcrs::GeographicCRS::EPSG_4979.get()));

  // Single-step AxisOrderReversal hits front branch (size >= 1) and skips
  // back/reversal checks (size >= 2) before ConcatenatedOperation::create
  // throws.
  std::string wktSingleAxis = BuildConcatWkt(
      pcrs::GeographicCRS::EPSG_4326, g4326Norm, {axis2d}, "single_axis");
  EXPECT_THROW(ParseConcatWkt(wktSingleAxis), pio::ParsingException);

  // Endpoint transformation reversals when steps are listed backwards.
  auto g0 = MakeGeog2D("Src", 6370000.0);
  auto g1 = MakeGeog2D("Mid", 6371000.0);
  auto g2 = MakeGeog2D("Tgt", 6372000.0);
  std::string wktRevEnds =
      BuildConcatWkt(g0, g2,
                     {MakeGeo2DOffset("mid_to_src", g1, g0),
                      MakeGeo2DOffset("tgt_to_mid", g2, g1)},
                     "reversed_ends");
  auto parsedRevEnds = ParseConcatWkt(wktRevEnds);
  ASSERT_EQ(parsedRevEnds->operations().size(), 2U);
  EXPECT_TRUE(
      parsedRevEnds->operations()[0]->sourceCRS()->isEquivalentTo(g0.get()));
  EXPECT_TRUE(
      parsedRevEnds->operations()[0]->targetCRS()->isEquivalentTo(g1.get()));
  EXPECT_TRUE(
      parsedRevEnds->operations()[1]->sourceCRS()->isEquivalentTo(g1.get()));
  EXPECT_TRUE(
      parsedRevEnds->operations()[1]->targetCRS()->isEquivalentTo(g2.get()));
}

TEST(ConcatenatedOperationTest, FixStepsFirstAndLastConversions) {
  auto base = MakeGeog2D("Base", 6370000.0);
  auto proj = MakeUTMProj("UTM32", base, 32);
  auto mid = MakeGeog2D("Mid", 6371000.0);
  auto end = MakeGeog2D("End", 6372000.0);
  auto wrongTgt = MakeGeog2D("WrongTgt", 6373000.0);
  auto utm = pop::Conversion::createUTM(NameProps("UTM32"), 32, true);
  auto utmInv = utm->inverse();
  auto chgUnit = pop::Conversion::createChangeVerticalUnit(NameProps("Unit"));
  auto hdr = pop::Conversion::createHeightDepthReversal(NameProps("Rev"));

  // i == 0 forward UTM Conversion gets inversed to Proj -> Base.
  auto concatFirstFwd = ParseConcatWkt(
      BuildConcatWkt(proj, end, {utm, MakeGeo2DOffset("b_end", base, end)}));
  ASSERT_EQ(concatFirstFwd->operations().size(), 2U);
  EXPECT_TRUE(
      concatFirstFwd->operations()[0]->sourceCRS()->isEquivalentTo(proj.get()));
  EXPECT_TRUE(
      concatFirstFwd->operations()[0]->targetCRS()->isEquivalentTo(base.get()));

  // i == 0 InverseConversion already inverted keeps direction.
  auto concatFirstInv = ParseConcatWkt(
      BuildConcatWkt(proj, end, {utmInv, MakeGeo2DOffset("b_end", base, end)}));
  ASSERT_EQ(concatFirstInv->operations().size(), 2U);
  EXPECT_TRUE(
      concatFirstInv->operations()[0]->sourceCRS()->isEquivalentTo(proj.get()));

  // i == 0 CompoundCRS containing ProjectedCRS as first component.
  auto vert = MakeVert("V");
  auto compProj = pcrs::CompoundCRS::create(
      NameProps("CompProj"), std::vector<pcrs::CRSNNPtr>{proj, vert});
  auto concatCompProj = ParseConcatWkt(BuildConcatWkt(
      compProj, end, {utmInv, MakeGeo2DOffset("b_end", base, end)}));
  ASSERT_EQ(concatCompProj->operations().size(), 2U);
  EXPECT_TRUE(concatCompProj->operations()[0]->sourceCRS()->isEquivalentTo(
      compProj.get()));

  // i == 0 next step reversed: 3 steps avoid pre-reversing step[1].
  auto concatNextRev = ParseConcatWkt(
      BuildConcatWkt(proj, end,
                     {utm, MakeGeo2DOffset("mid_base", mid, base),
                      MakeGeo2DOffset("mid_end", mid, end)}));
  ASSERT_EQ(concatNextRev->operations().size(), 3U);
  EXPECT_TRUE(
      concatNextRev->operations()[0]->targetCRS()->isEquivalentTo(base.get()));
  EXPECT_TRUE(
      concatNextRev->operations()[1]->sourceCRS()->isEquivalentTo(base.get()));

  // i == 0 fallback to derivedCRS->baseCRS() when next step has null sourceCRS.
  auto concatFallbackBase =
      ParseConcatWkt(BuildConcatWkt(proj, end, {utm, chgUnit}));
  ASSERT_EQ(concatFallbackBase->operations().size(), 2U);
  EXPECT_TRUE(concatFallbackBase->operations()[0]->targetCRS()->isEquivalentTo(
      base.get()));

  // i == 0 non-derived sourceCRS (Geog3D and CompoundCRS without DerivedCRS).
  auto g3d = MakeGeog3D("G3D", 6370000.0);
  auto geoc1 = MakeGeoc("Geoc1", 6370000.0);
  auto geoc2 = MakeGeoc("Geoc2", 6371000.0);
  auto geogGeoc = pop::Conversion::createGeographicGeocentric(NameProps("g2c"));
  auto geocTrans = pop::Transformation::createGeocentricTranslations(
      NameProps("geoc_trans"), geoc1, geoc2, 1.0, 2.0, 3.0, {});

  auto concatNonDerived =
      ParseConcatWkt(BuildConcatWkt(g3d, geoc2, {geogGeoc, geocTrans}));
  ASSERT_EQ(concatNonDerived->operations().size(), 2U);
  EXPECT_TRUE(concatNonDerived->operations()[0]->targetCRS()->isEquivalentTo(
      geoc1.get()));

  auto compGeog = pcrs::CompoundCRS::create(
      NameProps("CompGeog"), std::vector<pcrs::CRSNNPtr>{base, vert});
  auto concatCompGeog =
      ParseConcatWkt(BuildConcatWkt(compGeog, geoc2, {geogGeoc, geocTrans}));
  ASSERT_EQ(concatCompGeog->operations().size(), 2U);

  // Non-derived i == 0 where next step has null sourceCRS.
  EXPECT_THROW(ParseConcatWkt(BuildConcatWkt(g3d, end, {geogGeoc, chgUnit})),
               pio::ParsingException);

  // i + 1 == size trailing Conversion with DerivedCRS targetCRS.
  auto concatLastDerived = ParseConcatWkt(
      BuildConcatWkt(mid, proj, {MakeGeo2DOffset("m_b", mid, base), utm}));
  ASSERT_EQ(concatLastDerived->operations().size(), 2U);
  EXPECT_TRUE(concatLastDerived->operations()[1]->sourceCRS()->isEquivalentTo(
      base.get()));
  EXPECT_TRUE(concatLastDerived->operations()[1]->targetCRS()->isEquivalentTo(
      proj.get()));

  // i + 1 == size reverses previous step if sourceCRS matches baseCRS, then
  // first step sourceCRS fails against ConcatOp sourceCRS.
  EXPECT_THROW(
      ParseConcatWkt(BuildConcatWkt(
          mid, proj, {MakeGeo2DOffset("base_wrong", base, wrongTgt), utm})),
      pop::InvalidOperation);

  // i + 1 == size with null previous targetCRS falls back to baseCRS().
  auto v1 = MakeVert("V1");
  EXPECT_THROW(ParseConcatWkt(BuildConcatWkt(v1, proj, {chgUnit, utm})),
               pio::ParsingException);

  // i + 1 == size non-derived targetCRS with DerivedCRS step[0]->targetCRS()
  // matching forward deriving conversion -> inverses last conversion.
  auto concatLastNonDerivedMatch = ParseConcatWkt(
      BuildConcatWkt(mid, base, {MakeGeo2DOffset("m_proj", mid, proj), utm}));
  ASSERT_EQ(concatLastNonDerivedMatch->operations().size(), 2U);
  EXPECT_TRUE(
      concatLastNonDerivedMatch->operations()[1]->sourceCRS()->isEquivalentTo(
          proj.get()));
  EXPECT_TRUE(
      concatLastNonDerivedMatch->operations()[1]->targetCRS()->isEquivalentTo(
          base.get()));

  // i + 1 == size non-derived targetCRS, non-derived previous targetCRS.
  auto v2 = MakeVert("V2");
  auto v3 = MakeVert("V3");
  auto concatLastVert = ParseConcatWkt(
      BuildConcatWkt(v1, v3, {MakeVertOffset("v1_v2", v1, v2), chgUnit}));
  ASSERT_EQ(concatLastVert->operations().size(), 2U);
  EXPECT_TRUE(
      concatLastVert->operations()[1]->sourceCRS()->isEquivalentTo(v2.get()));

  // i + 1 == size where previous step has null targetCRS.
  EXPECT_THROW(ParseConcatWkt(BuildConcatWkt(
                   v1, v3, {MakeVertOffset("v1_v2", v1, v2), hdr, chgUnit})),
               pio::ParsingException);
}

TEST(ConcatenatedOperationTest, FixStepsMiddleConversions) {
  auto src = MakeGeog2D("Src", 6370000.0);
  auto base = MakeGeog2D("Base", 6371000.0);
  auto proj = MakeUTMProj("UTM32", base, 32);
  auto end = MakeGeog2D("End", 6372000.0);
  auto utm = pop::Conversion::createUTM(NameProps("UTM32"), 32, true);

  // Issue #2817: middle conversion between ProjectedCRS and GeographicCRS.
  auto concat2817 =
      ParseConcatWkt(BuildConcatWkt(src, end,
                                    {MakeGeo2DOffset("s_p", src, proj), utm,
                                     MakeGeo2DOffset("b_e", base, end)}));
  ASSERT_EQ(concat2817->operations().size(), 3U);
  EXPECT_TRUE(
      concat2817->operations()[1]->sourceCRS()->isEquivalentTo(proj.get()));
  EXPECT_TRUE(
      concat2817->operations()[1]->targetCRS()->isEquivalentTo(base.get()));

  // Issue #4116: 4 steps avoid early tail-reversal so middle conversion
  // reverses step[2] and updates step[1]'s targetCRS.
  auto vSrc = MakeVert("VSrc", true);
  auto vFooH = MakeVert("Foo height", true);
  auto vFooD = MakeVert("Foo depth", false);
  auto vBarH = MakeVert("Bar height", true);
  auto vEnd = MakeVert("VEnd", true);
  auto hdr4116 = pop::Conversion::createHeightDepthReversal(
      NameProps("Foo height to Foo depth"));
  auto concat4116 = ParseConcatWkt(BuildConcatWkt(
      vSrc, vEnd,
      {MakeVertOffset("VSrc to Foo height", vSrc, vFooH), hdr4116,
       MakeVertOffset("Bar height to Foo depth", vBarH, vFooD),
       MakeVertOffset("Bar height to VEnd", vBarH, vEnd)}));
  ASSERT_EQ(concat4116->operations().size(), 4U);
  EXPECT_TRUE(
      concat4116->operations()[1]->targetCRS()->isEquivalentTo(vFooD.get()));
  EXPECT_TRUE(
      concat4116->operations()[2]->sourceCRS()->isEquivalentTo(vFooD.get()));
  EXPECT_TRUE(
      concat4116->operations()[2]->targetCRS()->isEquivalentTo(vBarH.get()));

  // Negative checks for #4116 string heuristic: no " to " and non-matching
  // nextOpFrom/nextOpTo.
  auto hdrNoArrow =
      pop::Conversion::createHeightDepthReversal(NameProps("Rev"));
  auto concatNoArrow = ParseConcatWkt(BuildConcatWkt(
      vSrc, vBarH,
      {MakeVertOffset("s0", vSrc, vFooH), hdrNoArrow,
       MakeVertOffset("Foo depth to Bar height", vFooD, vBarH)}));
  ASSERT_EQ(concatNoArrow->operations().size(), 3U);

  auto concatForwardNext = ParseConcatWkt(BuildConcatWkt(
      vSrc, vBarH,
      {MakeVertOffset("s0", vSrc, vFooH), hdr4116,
       MakeVertOffset("Foo depth to Bar height", vFooD, vBarH)}));
  ASSERT_EQ(concatForwardNext->operations().size(), 3U);

  // EPSG:7987 heuristic: HeightDepthReversal followed by ChangeVerticalUnit
  // synthesizes "... depth" VerticalCRS when source ends with " height" & UP.
  auto chgUnit = pop::Conversion::createChangeVerticalUnit(NameProps("Unit"));
  auto concat7987 = ParseConcatWkt(BuildConcatWkt(
      vSrc, vEnd, {MakeVertOffset("s0", vSrc, vFooH), hdrNoArrow, chgUnit}));
  ASSERT_EQ(concat7987->operations().size(), 3U);
  ASSERT_NE(concat7987->operations()[1]->targetCRS(), nullptr);
  EXPECT_EQ(concat7987->operations()[1]->targetCRS()->nameStr(), "Foo depth");

  // Negative EPSG:7987: name does not end with " height".
  auto vNoHeight = MakeVert("Foo elev", true);
  EXPECT_THROW(
      ParseConcatWkt(BuildConcatWkt(
          vSrc, vEnd,
          {MakeVertOffset("s0", vSrc, vNoHeight), hdrNoArrow, chgUnit})),
      pio::ParsingException);

  // Negative EPSG:7987: name ends with " height" but direction is DOWN.
  auto vHeightDown = MakeVert("Bad height", false);
  EXPECT_THROW(
      ParseConcatWkt(BuildConcatWkt(
          vSrc, vEnd,
          {MakeVertOffset("s0", vSrc, vHeightDown), hdrNoArrow, chgUnit})),
      pio::ParsingException);
}

TEST(ConcatenatedOperationTest,
     FixStepsTransformationsGeogGeocBridgingAndNadcon3D) {
  auto v0 = MakeVert("V0");
  auto v1 = MakeVert("V1");
  auto v2 = MakeVert("V2");
  auto chgUnit = pop::Conversion::createChangeVerticalUnit(NameProps("Unit"));

  // 4 steps leave step[2]->targetCRS() == nullptr before Transformation
  // step[3].
  try {
    ParseConcatWkt(BuildConcatWkt(v0, v2,
                                  {MakeVertOffset("s0", v0, v1), chgUnit,
                                   chgUnit, MakeVertOffset("s3", v1, v2)}));
    FAIL() << "Expected InvalidOperation for missing targetCRS at step 3";
  } catch (const pop::InvalidOperation& e) {
    EXPECT_THAT(e.what(), HasSubstr("step 3"));
  }

  // Middle Transformation reversal inside a 3-step chain.
  auto g0 = MakeGeog2D("G0", 6370000.0);
  auto g1 = MakeGeog2D("G1", 6371000.0);
  auto g2 = MakeGeog2D("G2", 6372000.0);
  auto g3 = MakeGeog2D("G3", 6373000.0);
  auto concatMidRev = ParseConcatWkt(BuildConcatWkt(
      g0, g3,
      {MakeGeo2DOffset("s0", g0, g1), MakeGeo2DOffset("s1_rev", g2, g1),
       MakeGeo2DOffset("s2", g2, g3)}));
  ASSERT_EQ(concatMidRev->operations().size(), 3U);
  EXPECT_TRUE(
      concatMidRev->operations()[1]->sourceCRS()->isEquivalentTo(g1.get()));
  EXPECT_TRUE(
      concatMidRev->operations()[1]->targetCRS()->isEquivalentTo(g2.get()));

  // Automatic Geographic <-> Geocentric bridging on matching CRS names
  // across all 4 combinations (source/target x Geog/Geoc).
  auto geogM = MakeGeog3D("BridgeName", 6370000.0);
  auto geocM = MakeGeoc("BridgeName", 6370000.0);
  auto mid = MakeGeog2D("Mid", 6371000.0);
  auto end = MakeGeog2D("End", 6372000.0);

  auto b1 = ParseConcatWkt(BuildConcatWkt(
      geocM, end,
      {MakeGeo2DOffset("s0", geogM, mid), MakeGeo2DOffset("s1", mid, end)}));
  ASSERT_EQ(b1->operations().size(), 3U);
  EXPECT_TRUE(b1->operations()[0]->sourceCRS()->isEquivalentTo(geocM.get()));
  EXPECT_TRUE(b1->operations()[0]->targetCRS()->isEquivalentTo(geogM.get()));

  auto b2 = ParseConcatWkt(BuildConcatWkt(
      geogM, end,
      {MakeGeo2DOffset("s0", geocM, mid), MakeGeo2DOffset("s1", mid, end)}));
  ASSERT_EQ(b2->operations().size(), 3U);
  EXPECT_TRUE(b2->operations()[0]->sourceCRS()->isEquivalentTo(geogM.get()));
  EXPECT_TRUE(b2->operations()[0]->targetCRS()->isEquivalentTo(geocM.get()));

  auto b3 = ParseConcatWkt(BuildConcatWkt(
      geocM, end,
      {MakeGeo2DOffset("s0", mid, geogM), MakeGeo2DOffset("s1", mid, end)}));
  ASSERT_EQ(b3->operations().size(), 3U);
  EXPECT_TRUE(b3->operations()[1]->sourceCRS()->isEquivalentTo(geogM.get()));

  auto b4 = ParseConcatWkt(BuildConcatWkt(
      geogM, end,
      {MakeGeo2DOffset("s0", mid, geocM), MakeGeo2DOffset("s1", mid, end)}));
  ASSERT_EQ(b4->operations().size(), 3U);
  EXPECT_TRUE(b4->operations()[1]->sourceCRS()->isEquivalentTo(geocM.get()));

  // Issue #3819: final NADCON5 (3D) or 3D-grid-file transformation registered
  // between 2D GeographicCRS gets promoted to 3D GeographicCRS endpoints.
  auto g3dSrc = MakeGeog3D("Src3D", 6370000.0);
  auto g3dA = MakeGeog3D("Alpha", 6371000.0);
  auto g2dA = MakeGeog2D("Alpha", 6371000.0);
  auto g2dB = MakeGeog2D("Beta", 6372000.0);
  auto g3dB = MakeGeog3D("Beta", 6372000.0);

  auto step0 = pop::Transformation::createGeographic3DOffsets(
      NameProps("s0"), g3dSrc, g3dA,
      pcommon::Angle(0.0, pcommon::UnitOfMeasure::DEGREE),
      pcommon::Angle(0.0, pcommon::UnitOfMeasure::DEGREE),
      pcommon::Length(0.0, pcommon::UnitOfMeasure::METRE), {});

  auto nadcon1075 = pop::Transformation::create(
      NameProps("nadcon3d_epsg"), g2dA, g2dB, nullptr,
      EpsgIntProps(EPSG_NAME_METHOD_NADCON5_3D, EPSG_CODE_METHOD_NADCON5_3D),
      std::vector<pop::OperationParameterNNPtr>{},
      std::vector<pop::ParameterValueNNPtr>{}, {});
  auto parsedNadcon1075 =
      ParseConcatWkt(BuildConcatWkt(g3dSrc, g3dB, {step0, nadcon1075}));
  ASSERT_EQ(parsedNadcon1075->operations().size(), 2U);
  EXPECT_TRUE(parsedNadcon1075->operations()[1]->sourceCRS()->isEquivalentTo(
      g3dA.get()));
  EXPECT_TRUE(parsedNadcon1075->operations()[1]->targetCRS()->isEquivalentTo(
      g3dB.get()));

  auto custom3dParam = pop::Transformation::create(
      NameProps("nadcon3d_param"), g2dA, g2dB, nullptr,
      NameProps("Custom 3D Grid"),
      {pop::OperationParameter::create(NameProps(
          // NOLINTNEXTLINE(whitespace/line_length)
          PROJ_WKT2_PARAMETER_LATITUDE_LONGITUDE_ELLIPOISDAL_HEIGHT_DIFFERENCE_FILE))},
      {pop::ParameterValue::createFilename("grid3d.bin")}, {});
  auto parsedNadconParam =
      ParseConcatWkt(BuildConcatWkt(g3dSrc, g3dB, {step0, custom3dParam}));
  ASSERT_EQ(parsedNadconParam->operations().size(), 2U);
  EXPECT_TRUE(parsedNadconParam->operations()[1]->targetCRS()->isEquivalentTo(
      g3dB.get()));

  // Non-3D method fails target CRS check after the loop.
  auto plain2d = MakeGeo2DOffset("plain2d", g2dA, g2dB);
  EXPECT_THROW(ParseConcatWkt(BuildConcatWkt(g3dSrc, g3dB, {step0, plain2d})),
               pop::InvalidOperation);

  // Post-loop trailing Geog <-> Geoc bridging in both directions.
  auto geogEnd = MakeGeog3D("EndMatch", 6373000.0);
  auto geocEnd = MakeGeoc("EndMatch", 6373000.0);

  auto post1 = ParseConcatWkt(BuildConcatWkt(
      g0, geocEnd,
      {MakeGeo2DOffset("s0", g0, mid), MakeGeo2DOffset("s1", mid, geogEnd)}));
  ASSERT_EQ(post1->operations().size(), 3U);
  EXPECT_TRUE(
      post1->operations()[2]->sourceCRS()->isEquivalentTo(geogEnd.get()));
  EXPECT_TRUE(
      post1->operations()[2]->targetCRS()->isEquivalentTo(geocEnd.get()));

  auto post2 = ParseConcatWkt(BuildConcatWkt(
      g0, geogEnd,
      {MakeGeo2DOffset("s0", g0, mid), MakeGeo2DOffset("s1", mid, geocEnd)}));
  ASSERT_EQ(post2->operations().size(), 3U);
  EXPECT_TRUE(
      post2->operations()[2]->sourceCRS()->isEquivalentTo(geocEnd.get()));
  EXPECT_TRUE(
      post2->operations()[2]->targetCRS()->isEquivalentTo(geogEnd.get()));

  // Post-loop source/target CRS mismatch errors.
  auto wrongSrc = MakeGeog2D("WrongSrc", 6374000.0);
  auto wrongEnd = MakeGeog2D("WrongEnd", 6375000.0);
  EXPECT_THROW(
      ParseConcatWkt(BuildConcatWkt(
          wrongSrc, end,
          {MakeGeo2DOffset("s0", g0, mid), MakeGeo2DOffset("s1", mid, end)})),
      pop::InvalidOperation);
  EXPECT_THROW(
      ParseConcatWkt(BuildConcatWkt(
          g0, wrongEnd,
          {MakeGeo2DOffset("s0", g0, mid), MakeGeo2DOffset("s1", mid, end)})),
      pop::InvalidOperation);
}

TEST(ConcatenatedOperationTest, CreateComputeMetadataBranches) {
  auto g0 = MakeGeog2D("G0", 6370000.0);
  auto g1 = MakeGeog2D("G1", 6371000.0);
  auto g2 = MakeGeog2D("G2", 6372000.0);
  auto g3 = MakeGeog2D("G3", 6373000.0);

  auto makeWithMeta = [&](const std::string& name, const pcrs::CRSNNPtr& s,
                          const pcrs::CRSNNPtr& t, const pmeta::ExtentPtr& ext,
                          const std::string& accVal) {
    auto props = NameProps(name);
    if (ext != nullptr) {
      props.set(
          pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
          NN_CHECK_THROW(std::static_pointer_cast<putil::BaseObject>(ext)));
    }
    std::vector<pmeta::PositionalAccuracyNNPtr> acc;
    if (!accVal.empty()) {
      acc.emplace_back(pmeta::PositionalAccuracy::create(accVal));
    }
    return pop::Transformation::createGeographic2DOffsets(
        props, s, t, pcommon::Angle(0.0, pcommon::UnitOfMeasure::DEGREE),
        pcommon::Angle(0.0, pcommon::UnitOfMeasure::DEGREE), acc);
  };

  auto extWest = pmeta::Extent::createFromBBOX(-10.0, -10.0, 30.0, 30.0);
  auto extEast = pmeta::Extent::createFromBBOX(0.0, 0.0, 40.0, 40.0);
  auto extDisjoint = pmeta::Extent::createFromBBOX(50.0, 50.0, 60.0, 60.0);

  auto op0 = makeWithMeta("Op0", g0, g1, extWest, "1.5");
  auto op1 = makeWithMeta("Op1", g1, g2, extEast, "2.5");
  auto op2 = makeWithMeta("Op2", g2, g3, nullptr, "");

  // Single operation returns input directly.
  auto single = pop::ConcatenatedOperation::createComputeMetadata({op0}, true);
  EXPECT_EQ(single.get(), op0.get());

  // Nested ConcatenatedOperation flattening, overlapping extent, summed
  // accuracy.
  auto nested =
      pop::ConcatenatedOperation::createComputeMetadata({op0, op1}, true);
  auto nestedConcat = DynamicCastNN<pop::ConcatenatedOperation>(nested);
  ASSERT_FALSE(nestedConcat->coordinateOperationAccuracies().empty());
  EXPECT_EQ(nestedConcat->coordinateOperationAccuracies()[0]->value(), "4");
  ASSERT_EQ(nestedConcat->domains().size(), 1U);

  auto flattened = pop::ConcatenatedOperation::createComputeMetadata(
      {nestedConcat, op2}, true);
  auto flattenedConcat = DynamicCastNN<pop::ConcatenatedOperation>(flattened);
  EXPECT_EQ(flattenedConcat->operations().size(), 3U);
  // Missing accuracy on op2 makes total accuracy unknown (-1.0) -> empty list.
  EXPECT_TRUE(flattenedConcat->coordinateOperationAccuracies().empty());

  // Consecutive inverse cancellation: 3 -> 1 returns surviving op directly.
  auto cancelledTo1 = pop::ConcatenatedOperation::createComputeMetadata(
      {op0, op0->inverse(), op0}, true);
  EXPECT_TRUE(cancelledTo1->isEquivalentTo(op0.get()));

  // Consecutive inverse cancellation: 4 -> 2 hits indices.size() <= 2 break.
  auto op0Ballpark = makeWithMeta("Ballpark op0", g0, g1, nullptr, "");
  auto cancelledTo2 = pop::ConcatenatedOperation::createComputeMetadata(
      {op0Ballpark, op1, op1->inverse(), op1}, true);
  auto cancelled2Concat =
      DynamicCastNN<pop::ConcatenatedOperation>(cancelledTo2);
  EXPECT_EQ(cancelled2Concat->operations().size(), 2U);
  EXPECT_TRUE(cancelled2Concat->hasBallparkTransformation());

  // Bailout when 4 ops cancel first two and remaining two are an inverse pair.
  auto bailout = pop::ConcatenatedOperation::createComputeMetadata(
      {op0, op0->inverse(), op0, op0->inverse()}, true);
  EXPECT_EQ(
      DynamicCastNN<pop::ConcatenatedOperation>(bailout)->operations().size(),
      2U);

  // Disjoint extents: checkExtent=true throws
  // InvalidOperationEmptyIntersection, checkExtent=false creates
  // ConcatenatedOperation without extent.
  auto opDisjoint = makeWithMeta("OpDisjoint", g1, g2, extDisjoint, "1.0");
  EXPECT_THROW(pop::ConcatenatedOperation::createComputeMetadata(
                   {op0, opDisjoint}, true),
               pop::InvalidOperation);
  auto allowedDisjoint = pop::ConcatenatedOperation::createComputeMetadata(
      {op0, opDisjoint}, false);
  EXPECT_TRUE(allowedDisjoint->domains().empty());
}

TEST(ConcatenatedOperationTest, InverseCloneAndEquivalenceBranches) {
  auto g0 = MakeGeog2D("G0", 6370000.0);
  auto g1 = MakeGeog2D("G1", 6371000.0);
  auto g2 = MakeGeog2D("G2", 6372000.0);
  auto g3 = MakeGeog2D("G3", 6373000.0);

  auto op0 = MakeGeo2DOffset("Step0", g0, g1, 0.1, 0.2,
                             {pmeta::PositionalAccuracy::create("0.5")});
  auto op1 = MakeGeo2DOffset("Step1", g1, g2, 0.3, 0.4,
                             {pmeta::PositionalAccuracy::create("1.5")});
  auto op2 = MakeGeo2DOffset("Step2", g2, g3, 0.5, 0.6);

  // inverse() with computedName_ == true vs computedName_ == false.
  auto computed = DynamicCastNN<pop::ConcatenatedOperation>(
      pop::ConcatenatedOperation::createComputeMetadata({op0, op1}, false));
  auto computedInv = computed->inverse();
  EXPECT_THAT(computedInv->nameStr(), HasSubstr("Step1"));
  ASSERT_FALSE(computedInv->coordinateOperationAccuracies().empty());

  auto explicitConcat = pop::ConcatenatedOperation::create(
      CodeProps("ExplicitName", "TEST", "10"), {op0, op1},
      {pmeta::PositionalAccuracy::create("2.0")});
  auto explicitInv = explicitConcat->inverse();
  EXPECT_EQ(explicitInv->sourceCRS().get(), g2.get());
  EXPECT_EQ(explicitInv->targetCRS().get(), g0.get());

  // Direct PROJ_FOR_TEST shallowClone() preserves structure and equivalence.
  auto cloned = explicitConcat->shallowClone();
  EXPECT_NE(cloned.get(), explicitConcat.get());
  EXPECT_TRUE(cloned->isEquivalentTo(explicitConcat.get(),
                                     putil::IComparable::Criterion::STRICT));

  // _isEquivalentTo branches.
  auto sameStepsDiffMeta = pop::ConcatenatedOperation::create(
      NameProps("OtherName"), {op0, op1}, {});
  EXPECT_FALSE(explicitConcat->isEquivalentTo(
      sameStepsDiffMeta.get(), putil::IComparable::Criterion::STRICT));
  EXPECT_TRUE(explicitConcat->isEquivalentTo(
      sameStepsDiffMeta.get(), putil::IComparable::Criterion::EQUIVALENT));

  EXPECT_FALSE(explicitConcat->isEquivalentTo(
      op0.get(), putil::IComparable::Criterion::EQUIVALENT));

  auto threeSteps = pop::ConcatenatedOperation::create(NameProps("Three"),
                                                       {op0, op1, op2}, {});
  EXPECT_FALSE(explicitConcat->isEquivalentTo(
      threeSteps.get(), putil::IComparable::Criterion::EQUIVALENT));

  auto diffSecondStep = pop::ConcatenatedOperation::create(
      NameProps("DiffSecond"),
      {op0, MakeGeo2DOffset("Step1Alt", g1, g2, 9.0, 9.0)}, {});
  EXPECT_FALSE(explicitConcat->isEquivalentTo(
      diffSecondStep.get(), putil::IComparable::Criterion::EQUIVALENT));

  // gridsNeeded aggregates grids across child steps.
  auto ntv2 = pop::Transformation::createNTv2(
      NameProps("ntv2_step"), g1, g2, "test_missing_concat_grid.gsb", {});
  auto withGrid = pop::ConcatenatedOperation::create(NameProps("WithGrid"),
                                                     {op0, ntv2}, {});
  std::set<pop::GridDescription> grids = withGrid->gridsNeeded(nullptr, false);
  ASSERT_EQ(grids.size(), 1U);
  EXPECT_EQ(grids.begin()->shortName, "test_missing_concat_grid.gsb");
}

TEST(ConcatenatedOperationTest, ExportToWKTJSONAndPROJString) {
  auto g0 = MakeGeog2D("G0", 6370000.0);
  auto g1 = MakeGeog2D("G1", 6371000.0);
  auto g2 = MakeGeog2D("G2", 6372000.0);

  auto op0 = pop::Transformation::createGeographic2DOffsets(
      CodeProps("Step0", "TEST", "1"), g0, g1,
      pcommon::Angle(0.1, pcommon::UnitOfMeasure::DEGREE),
      pcommon::Angle(0.2, pcommon::UnitOfMeasure::DEGREE), {});
  auto op1 = pop::Transformation::createGeographic2DOffsets(
      CodeProps("Step1", "TEST", "2"), g1, g2,
      pcommon::Angle(0.3, pcommon::UnitOfMeasure::DEGREE),
      pcommon::Angle(0.4, pcommon::UnitOfMeasure::DEGREE), {});

  auto props = CodeProps("ExportConcat", "TEST", "100")
                   .set(pop::CoordinateOperation::OPERATION_VERSION_KEY, "v1.0")
                   .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
                        pmeta::Extent::WORLD);
  auto concat = pop::ConcatenatedOperation::create(
      props, {op0, op1}, {pmeta::PositionalAccuracy::create("0.5")});

  EXPECT_THROW(ExportWkt(concat, pio::WKTFormatter::Convention::WKT1_GDAL),
               pio::FormattingException);
  EXPECT_THROW(ExportWkt(concat, pio::WKTFormatter::Convention::WKT2_2015),
               pio::FormattingException);

  std::string wktFull =
      ExportWkt(concat, pio::WKTFormatter::Convention::WKT2_2019);
  EXPECT_THAT(wktFull, HasSubstr("CONCATENATEDOPERATION[\"ExportConcat\""));
  EXPECT_THAT(wktFull, HasSubstr("VERSION[\"v1.0\"]"));
  EXPECT_THAT(wktFull, HasSubstr("OPERATIONACCURACY[0.5]"));
  EXPECT_THAT(wktFull, HasSubstr("ID[\"TEST\",1]"));
  EXPECT_THAT(wktFull, HasSubstr("AREA[\"World\"]"));

  // Simplified WKT omits step IDs when top-level has an ID.
  std::string wktSimple =
      ExportWkt(concat, pio::WKTFormatter::Convention::WKT2_2019_SIMPLIFIED);
  EXPECT_THAT(wktSimple, HasSubstr("ID[\"TEST\",100]"));
  EXPECT_EQ(wktSimple.find("ID[\"TEST\",1]"), std::string::npos);

  // JSON export: unnamed and named + round-trip through JSONParser.
  auto unnamed =
      pop::ConcatenatedOperation::create(NameProps(""), {op0, op1}, {});
  std::string unnamedJson = ExportJson(unnamed);
  EXPECT_THAT(unnamedJson, HasSubstr("\"name\":\"unnamed\""));

  std::string namedJson = ExportJson(concat);
  EXPECT_THAT(namedJson, HasSubstr("\"name\":\"ExportConcat\""));
  EXPECT_THAT(namedJson, HasSubstr("\"accuracy\":\"0.5\""));
  auto reparsedJson = pio::createFromUserInput(namedJson, nullptr);
  EXPECT_NE(DynamicCastNN<pop::ConcatenatedOperation>(reparsedJson)
                ->operations()
                .size(),
            0U);

  // PROJ pipeline export with epoch variations via UTM 32N -> UTM 33N.
  auto utm32Crs = MakeUTMProj("UTM 32N", pcrs::GeographicCRS::EPSG_4326, 32);
  auto utm33Crs = MakeUTMProj("UTM 33N", pcrs::GeographicCRS::EPSG_4326, 33);
  auto opFactory = pop::CoordinateOperationFactory::create();

  auto ctxNoEpoch =
      pop::CoordinateOperationContext::create(nullptr, nullptr, 0.0);
  auto opsNoEpoch = opFactory->createOperations(utm32Crs, utm33Crs, ctxNoEpoch);
  ASSERT_EQ(opsNoEpoch.size(), 1U);
  auto concatNoEpoch = DynamicCastNN<pop::ConcatenatedOperation>(opsNoEpoch[0]);
  std::string projNoEpoch = ExportProj(concatNoEpoch);
  EXPECT_THAT(projNoEpoch, HasSubstr("+proj=pipeline"));
  EXPECT_EQ(projNoEpoch.find("+v_4="), std::string::npos);

  // Source epoch only -> duplicates sourceYear to both ends.
  auto ctxSrcEpoch =
      pop::CoordinateOperationContext::create(nullptr, nullptr, 0.0);
  ctxSrcEpoch->setSourceCoordinateEpoch(
      putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
          pcommon::Measure(2020.0, pcommon::UnitOfMeasure::YEAR))));
  auto opsSrcEpoch =
      opFactory->createOperations(utm32Crs, utm33Crs, ctxSrcEpoch);
  ASSERT_EQ(opsSrcEpoch.size(), 1U);
  auto concatSrcEpoch =
      DynamicCastNN<pop::ConcatenatedOperation>(opsSrcEpoch[0]);
  std::string projSrcEpoch = ExportProj(concatSrcEpoch);
  const auto firstV4 = projSrcEpoch.find("+v_4=2020");
  ASSERT_NE(firstV4, std::string::npos);
  EXPECT_NE(projSrcEpoch.find("+v_4=2020", firstV4 + 1), std::string::npos);

  // Inverse of source-only epoch has target epoch only -> duplicates
  // targetYear.
  auto concatTgtEpoch =
      DynamicCastNN<pop::ConcatenatedOperation>(concatSrcEpoch->inverse());
  EXPECT_FALSE(concatTgtEpoch->sourceCoordinateEpoch().has_value());
  ASSERT_TRUE(concatTgtEpoch->targetCoordinateEpoch().has_value());
  std::string projTgtEpoch = ExportProj(concatTgtEpoch);
  const auto tgtFirstV4 = projTgtEpoch.find("+v_4=2020");
  ASSERT_NE(tgtFirstV4, std::string::npos);
  EXPECT_NE(projTgtEpoch.find("+v_4=2020", tgtFirstV4 + 1), std::string::npos);

  // Distinct source and target coordinate epochs.
  auto ctxBothEpochs =
      pop::CoordinateOperationContext::create(nullptr, nullptr, 0.0);
  ctxBothEpochs->setSourceCoordinateEpoch(
      putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
          pcommon::Measure(2010.0, pcommon::UnitOfMeasure::YEAR))));
  ctxBothEpochs->setTargetCoordinateEpoch(
      putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
          pcommon::Measure(2025.0, pcommon::UnitOfMeasure::YEAR))));
  auto opsBoth = opFactory->createOperations(utm32Crs, utm33Crs, ctxBothEpochs);
  ASSERT_EQ(opsBoth.size(), 1U);
  std::string projBoth = ExportProj(opsBoth[0]);
  const auto pos2010 = projBoth.find("+v_4=2010");
  const auto pos2025 = projBoth.find("+v_4=2025");
  ASSERT_NE(pos2010, std::string::npos);
  ASSERT_NE(pos2025, std::string::npos);
  EXPECT_LT(pos2010, pos2025);
}

TEST(ConcatenatedOperationTest, TransformExecutionAndCAPIStepInspection) {
  auto crs84 = pcrs::GeographicCRS::OGC_CRS84;
  auto step0 = MakeGeo2DOffset("offset0", crs84, crs84, 0.5, 1.0);
  auto step1 = MakeGeo2DOffset("offset1", crs84, crs84, 0.25, -0.5);
  auto concat = pop::ConcatenatedOperation::create(NameProps("crs84_shift"),
                                                   {step0, step1}, {});

  ScopedContext ctx = MakeContext();
  ASSERT_NE(ctx.get(), nullptr);

  // C++ CoordinateTransformer execution in OGC:CRS84 (lon, lat in degrees).
  auto transformer = concat->coordinateTransformer(ctx.get());
  PJ_COORD res = transformer->transform(proj_coord(10.0, 20.0, 0.0, 0.0));
  EXPECT_NEAR(res.xy.x, 10.5, 1e-9);
  EXPECT_NEAR(res.xy.y, 20.75, 1e-9);

  // C API construction from WKT2:2019, forward/inverse transform, step API.
  std::string wkt = ExportWkt(concat);
  ScopedPj pj(proj_create(ctx.get(), wkt.c_str()));
  ASSERT_NE(pj.get(), nullptr);
  EXPECT_EQ(proj_get_type(pj.get()), PJ_TYPE_CONCATENATED_OPERATION);
  EXPECT_EQ(proj_concatoperation_get_step_count(ctx.get(), pj.get()), 2);

  ScopedPj cStep0(proj_concatoperation_get_step(ctx.get(), pj.get(), 0));
  ScopedPj cStep1(proj_concatoperation_get_step(ctx.get(), pj.get(), 1));
  EXPECT_NE(cStep0.get(), nullptr);
  EXPECT_NE(cStep1.get(), nullptr);

  {
    ScopedLogSuppressor suppress(ctx.get());
    EXPECT_EQ(proj_concatoperation_get_step(ctx.get(), pj.get(), -1), nullptr);
    EXPECT_EQ(proj_concatoperation_get_step(ctx.get(), pj.get(), 2), nullptr);
    EXPECT_EQ(proj_concatoperation_get_step(ctx.get(), cStep0.get(), 0),
              nullptr);
    EXPECT_EQ(proj_concatoperation_get_step_count(ctx.get(), cStep0.get()), 0);
  }
  proj_errno_reset(pj.get());

  PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, proj_coord(10.0, 20.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(fwd.xy.x, 10.5, 1e-9);
  EXPECT_NEAR(fwd.xy.y, 20.75, 1e-9);

  PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(inv.xy.x, 10.0, 1e-9);
  EXPECT_NEAR(inv.xy.y, 20.0, 1e-9);

  // Verify embedded proj.db AuthorityFactory instantiation of ConcatenatedOps.
  auto dbCtx = pio::DatabaseContext::create("", {}, ctx.get());
  auto epsgFactory =
      pio::AuthorityFactory::create(dbCtx, pmeta::Identifier::EPSG);
  auto codes = epsgFactory->getAuthorityCodes(
      pio::AuthorityFactory::ObjectType::CONCATENATED_OPERATION, false);
  ASSERT_FALSE(codes.empty());
  int inspected = 0;
  for (const auto& code : codes) {
    auto op = epsgFactory->createCoordinateOperation(code, false);
    auto dbConcat = DynamicCastNN<pop::ConcatenatedOperation>(op);
    EXPECT_GE(dbConcat->operations().size(), 2U);
    if (++inspected >= 5) {
      break;
    }
  }
}

}  // namespace
}  // namespace proj
