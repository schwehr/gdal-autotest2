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
// Tests ISO-19111 CoordinateOperationFactory and CoordinateOperationContext
// (proj/src/iso19111/operation/coordinateoperationfactory.cpp).
// Note: in google3 default non-GCE builds, networking is disabled, proj.db is
// embedded via SQLite memvfs, and gridshift/* files are embedded except
// ntv2_0.gsb / ca_nrc_ntv2_0.tif; synthetic grids use InMemoryFileSystem
// (/mem).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/coordinateoperationfactory.cpp

#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>  // NOLINT(build/c++20)
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
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_constants.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

namespace pcommon = ::osgeo::proj::common;
namespace pcoords = ::osgeo::proj::coordinates;
namespace pcrs = ::osgeo::proj::crs;
namespace pcs = ::osgeo::proj::cs;
namespace pdatum = ::osgeo::proj::datum;
namespace pio = ::osgeo::proj::io;
namespace pmeta = ::osgeo::proj::metadata;
namespace pop = ::osgeo::proj::operation;
namespace putil = ::osgeo::proj::util;

using ::testing::HasSubstr;

// RAII guard for single-threaded environment-variable tests. Saves any prior
// value on construction and restores or clears it upon destruction.
class ScopedEnvVar {
 public:
  ScopedEnvVar(std::string name, const char* value) : name_(std::move(name)) {
    const char* existing = std::getenv(name_.c_str());
    if (existing != nullptr) {
      had_previous_ = true;
      previous_ = existing;
    }
    Set(value);
  }

  ~ScopedEnvVar() {
    if (had_previous_) {
      setenv(name_.c_str(), previous_.c_str(), /*overwrite=*/1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  void Set(const char* value) const {
    if (value != nullptr) {
      setenv(name_.c_str(), value, /*overwrite=*/1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  std::string name_;
  bool had_previous_ = false;
  std::string previous_;
};

pcrs::CRSPtr TryCreateCrs(const pio::DatabaseContextNNPtr& db,
                          const std::string& input) {
  try {
    auto obj = pio::createFromUserInput(input, db);
    auto crs = std::dynamic_pointer_cast<pcrs::CRS>(obj.as_nullable());
    EXPECT_NE(nullptr, crs) << "input=" << input;
    return crs;
  } catch (const putil::Exception& e) {
    ADD_FAILURE() << "TryCreateCrs(" << input << ") threw: " << e.what();
    return nullptr;
  }
}

pcrs::CRSPtr TryFindCrsByName(const pio::AuthorityFactoryNNPtr& af,
                              const std::string& name,
                              pio::AuthorityFactory::ObjectType type) {
  try {
    auto matches = af->createObjectsFromName(name, {type}, false, 2);
    if (matches.size() == 1) {
      auto crs =
          std::dynamic_pointer_cast<pcrs::CRS>(matches.front().as_nullable());
      EXPECT_NE(nullptr, crs) << "name=" << name;
      return crs;
    }
    ADD_FAILURE() << "TryFindCrsByName(" << name
                  << ") returned matches.size()=" << matches.size();
  } catch (const putil::Exception& e) {
    ADD_FAILURE() << "TryFindCrsByName(" << name << ") threw: " << e.what();
  }
  return nullptr;
}

void ExportOpsToProj5Strings(const pio::DatabaseContextPtr& db,
                             std::span<const pop::CoordinateOperationNNPtr> ops,
                             std::vector<std::string>* out_strings = nullptr) {
  for (const auto& op : ops) {
    try {
      auto fmt = pio::PROJStringFormatter::create(
          pio::PROJStringFormatter::Convention::PROJ_5, db);
      std::string s = op->exportToPROJString(fmt.get());
      if (out_strings != nullptr) {
        out_strings->push_back(std::move(s));
      }
    } catch (const putil::Exception&) {
    }
    try {
      auto wkt_fmt = pio::WKTFormatter::create(
          pio::WKTFormatter::Convention::WKT2_2019, db);
      (void)op->exportToWKT(wkt_fmt.get());
    } catch (const putil::Exception&) {
    }
  }
}

pdatum::EllipsoidNNPtr MakeEllipsoid(const std::string& name, double a_m,
                                     double inv_f,
                                     const std::string& celestial_body) {
  return pdatum::Ellipsoid::createFlattenedSphere(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, name),
      pcommon::Length(a_m), pcommon::Scale(inv_f), celestial_body);
}

pdatum::EllipsoidNNPtr MakeSphere(const std::string& name, double radius_m,
                                  const std::string& celestial_body) {
  return pdatum::Ellipsoid::createSphere(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, name),
      pcommon::Length(radius_m), celestial_body);
}

pcrs::GeographicCRSNNPtr MakeGeog2D(
    const std::string& crs_name, const std::string& datum_name,
    const pdatum::EllipsoidNNPtr& ellps,
    const pdatum::PrimeMeridianNNPtr& pm = pdatum::PrimeMeridian::GREENWICH,
    const pcommon::UnitOfMeasure& ang_unit = pcommon::UnitOfMeasure::DEGREE,
    bool lon_lat = false,
    const pmeta::ExtentPtr& extent = pmeta::Extent::WORLD) {
  auto datum = pdatum::GeodeticReferenceFrame::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, datum_name),
      ellps, osgeo::proj::util::optional<std::string>(), pm);
  auto cs = lon_lat ? pcs::EllipsoidalCS::createLongitudeLatitude(ang_unit)
                    : pcs::EllipsoidalCS::createLatitudeLongitude(ang_unit);
  putil::PropertyMap props;
  props.set(pcommon::IdentifiedObject::NAME_KEY, crs_name);
  if (extent) {
    props.set(
        pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
        NN_CHECK_THROW(std::static_pointer_cast<putil::BaseObject>(extent)));
  }
  return pcrs::GeographicCRS::create(props, datum, cs);
}

pcrs::GeographicCRSNNPtr MakeGeog3D(
    const std::string& crs_name, const std::string& datum_name,
    const pdatum::EllipsoidNNPtr& ellps,
    const pdatum::PrimeMeridianNNPtr& pm = pdatum::PrimeMeridian::GREENWICH,
    const pcommon::UnitOfMeasure& ang_unit = pcommon::UnitOfMeasure::DEGREE,
    const pcommon::UnitOfMeasure& lin_unit = pcommon::UnitOfMeasure::METRE,
    bool lon_lat = false, bool height_down = false) {
  auto datum = pdatum::GeodeticReferenceFrame::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, datum_name),
      ellps, osgeo::proj::util::optional<std::string>(), pm);
  pcs::EllipsoidalCSNNPtr cs =
      lon_lat ? pcs::EllipsoidalCS::createLongitudeLatitudeEllipsoidalHeight(
                    ang_unit, lin_unit)
              : pcs::EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
                    ang_unit, lin_unit);
  if (height_down) {
    const auto& axes = cs->axisList();
    auto z_down = pcs::CoordinateSystemAxis::create(
        putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                 "Ellipsoidal depth"),
        "d", pcs::AxisDirection::DOWN, lin_unit);
    cs = pcs::EllipsoidalCS::create(putil::PropertyMap(), axes[0], axes[1],
                                    z_down);
  }
  return pcrs::GeographicCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, crs_name)
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      datum, cs);
}

pcrs::VerticalCRSNNPtr MakeVertCrs(
    const std::string& crs_name, const std::string& datum_name,
    const pcommon::UnitOfMeasure& unit = pcommon::UnitOfMeasure::METRE,
    bool is_depth = false, const pop::TransformationPtr& geoid_model = nullptr,
    const pmeta::ExtentPtr& extent = pmeta::Extent::WORLD) {
  auto vdatum = pdatum::VerticalReferenceFrame::create(putil::PropertyMap().set(
      pcommon::IdentifiedObject::NAME_KEY, datum_name));
  auto axis = pcs::CoordinateSystemAxis::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               is_depth ? "Depth" : "Gravity-related height"),
      is_depth ? "D" : "H",
      is_depth ? pcs::AxisDirection::DOWN : pcs::AxisDirection::UP, unit);
  auto vcs = pcs::VerticalCS::create(putil::PropertyMap(), axis);
  putil::PropertyMap props;
  props.set(pcommon::IdentifiedObject::NAME_KEY, crs_name);
  if (extent) {
    props.set(
        pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
        NN_CHECK_THROW(std::static_pointer_cast<putil::BaseObject>(extent)));
  }
  if (geoid_model) {
    props.set("GEOID_MODEL",
              NN_CHECK_THROW(
                  std::static_pointer_cast<putil::BaseObject>(geoid_model)));
  }
  return pcrs::VerticalCRS::create(props, vdatum, vcs);
}

pop::TransformationNNPtr MakeGeoidModel(
    const std::string& name, const pcrs::CRSNNPtr& vert_base,
    const pcrs::CRSPtr& interp_crs, const std::string& auth = std::string(),
    const std::string& code = std::string(),
    const pmeta::ExtentPtr& extent = nullptr,
    const std::string& accuracy_m = "0.05") {
  putil::PropertyMap props;
  props.set(pcommon::IdentifiedObject::NAME_KEY, name);
  if (!auth.empty() && !code.empty()) {
    props.set(pmeta::Identifier::CODESPACE_KEY, auth);
    props.set(pmeta::Identifier::CODE_KEY, code);
  }
  if (extent) {
    props.set(
        pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
        NN_CHECK_THROW(std::static_pointer_cast<putil::BaseObject>(extent)));
  }
  auto method = pop::OperationMethod::create(
      putil::PropertyMap(), std::vector<pop::OperationParameterNNPtr>());
  return pop::Transformation::create(
      props, vert_base, pcrs::GeographicCRS::EPSG_4979, interp_crs, method, {},
      {pmeta::PositionalAccuracy::create(accuracy_m)});
}

pcrs::BoundCRSNNPtr MakeHelmertBound(const pcrs::CRSNNPtr& base,
                                     const pcrs::CRSNNPtr& hub,
                                     double dx = 10.0, double dy = -20.0,
                                     double dz = 30.0) {
  auto transf = pop::Transformation::createGeocentricTranslations(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "Helmert3")
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      base, hub, dx, dy, dz, {pmeta::PositionalAccuracy::create("2.0")});
  return pcrs::BoundCRS::create(base, hub, transf);
}

void InstallTestGtxGrid(InMemoryFileSystem* fs, const std::string& path,
                        float shift_m = 45.0f) {
  const std::vector<float> grid_values(4, shift_m);
  // Span [-10, 20] deg E and [40, 70] deg N so UTM zone 32N test coordinates
  // (lon ~9E, lat ~49.65N) fall strictly inside the grid extent.
  fs->AddFile(path, BuildGtxGrid(/*west_deg=*/-10.0, /*south_deg=*/40.0,
                                 /*res_x_deg=*/30.0, /*res_y_deg=*/30.0,
                                 /*rows=*/2, /*cols=*/2, grid_values));
}

// ---------------------------------------------------------------------------
// 1. CoordinateOperationContext & Extent helpers
// ---------------------------------------------------------------------------

TEST(CoordinateOperationFactoryTest, ContextAccessorsSettersAndClone) {
  auto ctx = MakeContext();
  ASSERT_NE(ctx, nullptr);
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto extent = pmeta::Extent::createFromBBOX(
      -10.0, 40.0, 20.0, 60.0, putil::optional<std::string>("EuropeAOI"));
  auto op_ctx = pop::CoordinateOperationContext::create(af, extent, 1.5);

  EXPECT_EQ(op_ctx->getAuthorityFactory().get(), af.get());
  ASSERT_NE(op_ctx->getAreaOfInterest(), nullptr);
  EXPECT_EQ(op_ctx->getDesiredAccuracy(), 1.5);
  EXPECT_TRUE(op_ctx->getAllowBallparkTransformations());
  EXPECT_EQ(
      op_ctx->getSourceAndTargetCRSExtentUse(),
      pop::CoordinateOperationContext::SourceTargetCRSExtentUse::SMALLEST);
  EXPECT_EQ(
      op_ctx->getSpatialCriterion(),
      pop::CoordinateOperationContext::SpatialCriterion::STRICT_CONTAINMENT);
  EXPECT_TRUE(op_ctx->getUsePROJAlternativeGridNames());
  EXPECT_TRUE(op_ctx->getDiscardSuperseded());
  EXPECT_EQ(
      op_ctx->getGridAvailabilityUse(),
      pop::CoordinateOperationContext::GridAvailabilityUse::USE_FOR_SORTING);
  EXPECT_EQ(op_ctx->getAllowUseIntermediateCRS(),
            pop::CoordinateOperationContext::IntermediateCRSUse::
                IF_NO_DIRECT_TRANSFORMATION);

  auto new_extent = pmeta::Extent::createFromBBOX(170.0, -50.0, -170.0, 50.0);
  op_ctx->setAreaOfInterest(new_extent);
  op_ctx->setDesiredAccuracy(0.25);
  op_ctx->setAllowBallparkTransformations(false);
  op_ctx->setSourceAndTargetCRSExtentUse(
      pop::CoordinateOperationContext::SourceTargetCRSExtentUse::INTERSECTION);
  op_ctx->setSpatialCriterion(
      pop::CoordinateOperationContext::SpatialCriterion::PARTIAL_INTERSECTION);
  op_ctx->setUsePROJAlternativeGridNames(false);
  op_ctx->setDiscardSuperseded(false);
  op_ctx->setGridAvailabilityUse(
      pop::CoordinateOperationContext::GridAvailabilityUse::
          DISCARD_OPERATION_IF_MISSING_GRID);
  op_ctx->setAllowUseIntermediateCRS(
      pop::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  op_ctx->setIntermediateCRS({{"EPSG", "4258"}, {"EPSG", "4326"}});
  op_ctx->setSourceCoordinateEpoch(
      putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
          pcommon::Measure(2015.0, pcommon::UnitOfMeasure::YEAR))));
  op_ctx->setTargetCoordinateEpoch(
      putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
          pcommon::Measure(2020.0, pcommon::UnitOfMeasure::YEAR))));

  auto cloned = op_ctx->clone();
  ASSERT_NE(cloned->getAreaOfInterest(), nullptr);
  EXPECT_EQ(cloned->getDesiredAccuracy(), 0.25);
  EXPECT_FALSE(cloned->getAllowBallparkTransformations());
  EXPECT_EQ(
      cloned->getSourceAndTargetCRSExtentUse(),
      pop::CoordinateOperationContext::SourceTargetCRSExtentUse::INTERSECTION);
  EXPECT_EQ(
      cloned->getSpatialCriterion(),
      pop::CoordinateOperationContext::SpatialCriterion::PARTIAL_INTERSECTION);
  EXPECT_FALSE(cloned->getUsePROJAlternativeGridNames());
  EXPECT_FALSE(cloned->getDiscardSuperseded());
  EXPECT_EQ(cloned->getGridAvailabilityUse(),
            pop::CoordinateOperationContext::GridAvailabilityUse::
                DISCARD_OPERATION_IF_MISSING_GRID);
  EXPECT_EQ(cloned->getAllowUseIntermediateCRS(),
            pop::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  ASSERT_EQ(cloned->getIntermediateCRS().size(), 2u);
  EXPECT_EQ(cloned->getIntermediateCRS()[0].second, "4258");
  ASSERT_TRUE(cloned->getSourceCoordinateEpoch().has_value());
  EXPECT_DOUBLE_EQ(
      cloned->getSourceCoordinateEpoch()->coordinateEpoch().value(), 2015.0);
  ASSERT_TRUE(cloned->getTargetCoordinateEpoch().has_value());
  EXPECT_DOUBLE_EQ(
      cloned->getTargetCoordinateEpoch()->coordinateEpoch().value(), 2020.0);
}

TEST(CoordinateOperationFactoryTest, ExtentPseudoAreaAntimeridianAndTemporal) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();

  auto src = epsg->createCoordinateReferenceSystem("4230");  // ED50
  auto dst = epsg->createCoordinateReferenceSystem("4326");  // WGS 84

  // Antimeridian extent (west > east) exercises wraparound in getPseudoArea().
  auto antimeridian_aoi =
      pmeta::Extent::createFromBBOX(170.0, -50.0, -170.0, 50.0);
  auto op_ctx1 =
      pop::CoordinateOperationContext::create(af, antimeridian_aoi, 0.0);
  auto ops1 = factory->createOperations(src, dst, op_ctx1);
  EXPECT_FALSE(ops1.empty());

  // Temporal-only extent has no geographic elements -> getPseudoArea() == 0.0.
  auto temp_extent = pmeta::Extent::create(
      putil::optional<std::string>("TemporalOnly"), {}, {},
      {pmeta::TemporalExtent::create("2000-01-01", "2020-01-01")});
  auto op_ctx2 = pop::CoordinateOperationContext::create(af, temp_extent, 0.0);
  auto ops2 = factory->createOperations(src, dst, op_ctx2);
  EXPECT_FALSE(ops2.empty());

  auto temp_crs =
      MakeGeog2D("TempCrs", "TempDatum", pdatum::Ellipsoid::WGS84,
                 pdatum::PrimeMeridian::GREENWICH,
                 pcommon::UnitOfMeasure::DEGREE, false, temp_extent);
  auto op_ctx_default =
      pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  EXPECT_FALSE(
      factory->createOperations(temp_crs, dst, op_ctx_default).empty());

  // Disjoint CRS extents with INTERSECTION returns empty early at line 8099.
  auto utm_10n =
      epsg->createCoordinateReferenceSystem("26910");  // North America
  auto utm_32n =
      epsg->createCoordinateReferenceSystem("32632");  // Europe/Africa
  auto op_ctx3 = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  op_ctx3->setSourceAndTargetCRSExtentUse(
      pop::CoordinateOperationContext::SourceTargetCRSExtentUse::INTERSECTION);
  EXPECT_TRUE(factory->createOperations(utm_10n, utm_32n, op_ctx3).empty());
}

// ---------------------------------------------------------------------------
// 2. Filtering, Sorting & Duplicate/Uninteresting Removal
// ---------------------------------------------------------------------------

TEST(CoordinateOperationFactoryTest,
     FilterAccuracySpatialCriterionCrsExtentAndBallpark) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();

  auto src = epsg->createCoordinateReferenceSystem("4230");  // ED50
  auto dst = epsg->createCoordinateReferenceSystem("4326");  // WGS 84

  // Strict desired accuracy filters out coarse/unknown ops.
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.01);
  auto ops_tight = factory->createOperations(src, dst, op_ctx);
  for (const auto& op : ops_tight) {
    if (!op->coordinateOperationAccuracies().empty()) {
      EXPECT_LE(std::stod(op->coordinateOperationAccuracies().front()->value()),
                100.0);
    }
  }

  // Test all SourceTargetCRSExtentUse modes with a valid overlapping AOI.
  auto aoi = pmeta::Extent::createFromBBOX(-2.0, 48.0, 4.0, 52.0);
  constexpr pop::CoordinateOperationContext::SourceTargetCRSExtentUse
      kExtentUses[] = {
          pop::CoordinateOperationContext::SourceTargetCRSExtentUse::NONE,
          pop::CoordinateOperationContext::SourceTargetCRSExtentUse::BOTH,
          pop::CoordinateOperationContext::SourceTargetCRSExtentUse::
              INTERSECTION,
          pop::CoordinateOperationContext::SourceTargetCRSExtentUse::SMALLEST,
      };
  for (auto extent_use : kExtentUses) {
    auto c = pop::CoordinateOperationContext::create(af, aoi, 0.0);
    c->setSourceAndTargetCRSExtentUse(extent_use);
    c->setSpatialCriterion(pop::CoordinateOperationContext::SpatialCriterion::
                               PARTIAL_INTERSECTION);
    EXPECT_FALSE(factory->createOperations(src, dst, c).empty());
  }

  // allowBallparkTransformations = false suppresses ballpark fallback between
  // unrelated datums.
  auto custom1 =
      MakeGeog2D("CustomGeog1", "CustomDatum1", pdatum::Ellipsoid::WGS84);
  auto custom2 =
      MakeGeog2D("CustomGeog2", "CustomDatum2", pdatum::Ellipsoid::CLARKE_1866);
  auto c_ballpark = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  EXPECT_EQ(factory->createOperations(custom1, custom2, c_ballpark).size(), 1u);
  c_ballpark->setAllowBallparkTransformations(false);
  EXPECT_TRUE(factory->createOperations(custom1, custom2, c_ballpark).empty());
}

TEST(CoordinateOperationFactoryTest, FilterAreaDescriptionMatchAndNoExtentOp) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();

  // EPSG:4230 -> EPSG:4326 contains region-specific ops; passing an AOI with an
  // exact extent description exercises description matching in FilterResults.
  auto src = epsg->createCoordinateReferenceSystem("4230");
  auto dst = epsg->createCoordinateReferenceSystem("4326");
  auto ops_all = factory->createOperations(
      src, dst, pop::CoordinateOperationContext::create(af, nullptr, 0.0));
  ASSERT_FALSE(ops_all.empty());

  pmeta::ExtentPtr chosen_extent;
  for (const auto& op : ops_all) {
    for (const auto& dom : op->domains()) {
      if (dom->domainOfValidity() &&
          dom->domainOfValidity()->description().has_value()) {
        chosen_extent = dom->domainOfValidity();
        break;
      }
    }
    if (chosen_extent) {
      break;
    }
  }
  if (chosen_extent) {
    auto c = pop::CoordinateOperationContext::create(af, chosen_extent, 0.0);
    EXPECT_FALSE(factory->createOperations(src, dst, c).empty());
  }

  // Synthesize an AOI where operations lack extents or have only ballpark ops.
  auto b1 = MakeHelmertBound(
      MakeGeog2D("NoExt1", "NoExtDatum1", pdatum::Ellipsoid::WGS84,
                 pdatum::PrimeMeridian::GREENWICH,
                 pcommon::UnitOfMeasure::DEGREE, false, nullptr),
      pcrs::GeographicCRS::EPSG_4326);
  auto custom_target = MakeGeog2D(
      "NoExtTarget", "WGS 84", pdatum::Ellipsoid::WGS84,
      pdatum::PrimeMeridian::GREENWICH, pcommon::UnitOfMeasure::DEGREE, false,
      pmeta::Extent::createFromBBOX(-10.0, 40.0, 10.0, 60.0));
  auto aoi = pmeta::Extent::createFromBBOX(-5.0, 45.0, 5.0, 55.0);
  auto c_fallback = pop::CoordinateOperationContext::create(af, aoi, 50.0);
  c_fallback->setAllowBallparkTransformations(true);
  EXPECT_FALSE(
      factory->createOperations(b1, custom_target, c_fallback).empty());
}

TEST(CoordinateOperationFactoryTest,
     GridAvailabilityAlternativeGridNamesAndDiscardSuperseded) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();

  // EPSG:4277 (OSGB36) -> EPSG:4326 (WGS 84) has both grid-based and Helmert
  // ops, plus PROJ alternative grid names and superseded entries.
  auto osgb36 = epsg->createCoordinateReferenceSystem("4277");
  auto wgs84 = epsg->createCoordinateReferenceSystem("4326");

  constexpr pop::CoordinateOperationContext::GridAvailabilityUse kGridUses[] = {
      pop::CoordinateOperationContext::GridAvailabilityUse::USE_FOR_SORTING,
      pop::CoordinateOperationContext::GridAvailabilityUse::
          DISCARD_OPERATION_IF_MISSING_GRID,
      pop::CoordinateOperationContext::GridAvailabilityUse::
          IGNORE_GRID_AVAILABILITY,
      pop::CoordinateOperationContext::GridAvailabilityUse::KNOWN_AVAILABLE,
  };
  for (auto grid_use : kGridUses) {
    auto c = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
    c->setGridAvailabilityUse(grid_use);
    c->setUsePROJAlternativeGridNames(true);
    c->setDiscardSuperseded(true);
    EXPECT_FALSE(factory->createOperations(osgb36, wgs84, c).empty());

    c->setUsePROJAlternativeGridNames(false);
    c->setDiscardSuperseded(false);
    EXPECT_FALSE(factory->createOperations(osgb36, wgs84, c).empty());
  }
}

TEST(CoordinateOperationFactoryTest,
     SortHeuristicsDebugCheckDuplicateRemovalAndBallparkTieBreakers) {
  // Enable sort-consistency debug checking with assert suppression enabled.
  ScopedEnvVar env_debug("PROJ_DEBUG_SORT_FUNCT", "1");
  ScopedEnvVar env_no_assert("PROJ_DEBUG_SORT_FUNCT_ASSERT", "1");

  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();

  // 1. NTF (Paris) to RGF93 v1 (4807 -> 4171) exercises NTF (Paris) to NTF
  // (1)/(2), NTF (Paris) to ETRS89-FRA [RGF93 v1] (1)/(2), and duplicate
  // removal.
  auto ntf_paris = epsg->createCoordinateReferenceSystem("4807");
  auto rgf93 = epsg->createCoordinateReferenceSystem("4171");
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  auto ops_ntf = factory->createOperations(ntf_paris, rgf93, op_ctx);
  ASSERT_FALSE(ops_ntf.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_ntf);

  // 2. ETRS89 -> ETRS89-FRA [RGF93 v1] exercises ETRS89-XXX to ETRS89-YYY sort.
  auto etrs89 = epsg->createCoordinateReferenceSystem("4258");
  auto ops_etrs = factory->createOperations(etrs89, rgf93, op_ctx);
  EXPECT_FALSE(ops_etrs.empty());

  // 3. Ballpark sorting preferring prefix-similar names ("Alpha v6" vs "Beta")
  // and shorter names when other criteria tie.
  auto alpha_v6 = MakeGeog2D("Alpha v6", "Alpha v6", pdatum::Ellipsoid::WGS84);
  auto alpha = MakeGeog2D("Alpha", "Alpha", pdatum::Ellipsoid::WGS84);
  EXPECT_FALSE(factory->createOperations(alpha_v6, alpha, op_ctx).empty());

  // 4. Egypt 1907 -> WGS 84 triggers removeUninterestingOps step-count pruning.
  auto egypt = epsg->createCoordinateReferenceSystem("4229");
  auto wgs84 = epsg->createCoordinateReferenceSystem("4326");
  EXPECT_FALSE(factory->createOperations(egypt, wgs84, op_ctx).empty());
}

// ---------------------------------------------------------------------------
// 3. DB Lookups, Authority Scopes, Pivot Search & Datum Pivots
// ---------------------------------------------------------------------------

TEST(CoordinateOperationFactoryTest,
     BuildCrsIdsMismatchedIdNameCacheAndAuthorityScopes) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af_any = pio::AuthorityFactory::create(db, std::string());
  auto af_lit_any = pio::AuthorityFactory::create(db, "any");
  auto af_epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto af_proj = pio::AuthorityFactory::create(db, "PROJ");
  auto factory = pop::CoordinateOperationFactory::create();

  auto wgs84 = af_epsg->createCoordinateReferenceSystem("4326");
  auto ed50 = af_epsg->createCoordinateReferenceSystem("4230");

  // 1. CRS with mismatched code triggers ID-vs-CRS consistency check fallback.
  auto wgs84_bad_id = wgs84->alterId("EPSG", "4230");
  EXPECT_FALSE(factory
                   ->createOperations(wgs84_bad_id, ed50,
                                      pop::CoordinateOperationContext::create(
                                          af_any, nullptr, 0.0))
                   .empty());

  auto wgs84_missing_id = wgs84->alterId("EPSG", "99999999");
  EXPECT_FALSE(factory
                   ->createOperations(wgs84_missing_id, ed50,
                                      pop::CoordinateOperationContext::create(
                                          af_any, nullptr, 0.0))
                   .empty());

  // 2. Un-coded Geog2D, Geog3D, Projected, Vertical matched by exact DB name;
  // calling twice hits cacheNameToCRS.
  auto wgs84_2d_named = MakeGeog2D("WGS 84", "World Geodetic System 1984",
                                   pdatum::Ellipsoid::WGS84);
  auto utm32_bad_id =
      af_epsg->createCoordinateReferenceSystem("32632")->alterId("EPSG",
                                                                 "4230");
  auto utm32_noid =
      TryCreateCrs(db, R"(+proj=utm +zone=32 +datum=WGS84 +type=crs)");
  ASSERT_NE(utm32_noid, nullptr);
  utm32_noid = utm32_noid->alterName("WGS 84 / UTM zone 32N").as_nullable();

  auto op_ctx_any =
      pop::CoordinateOperationContext::create(af_any, nullptr, 0.0);
  op_ctx_any->setAllowUseIntermediateCRS(
      pop::CoordinateOperationContext::IntermediateCRSUse::NEVER);
  EXPECT_FALSE(
      factory->createOperations(wgs84_2d_named, ed50, op_ctx_any).empty());
  EXPECT_FALSE(
      factory->createOperations(utm32_bad_id, ed50, op_ctx_any).empty());
  EXPECT_FALSE(
      factory->createOperations(NN_CHECK_THROW(utm32_noid), ed50, op_ctx_any)
          .empty());

  auto wgs84_3d_bad_id =
      af_epsg->createCoordinateReferenceSystem("4979")->alterId("EPSG", "4230");
  EXPECT_FALSE(
      factory->createOperations(wgs84_3d_bad_id, ed50, op_ctx_any).empty());

  auto wgs84_3d_noid = MakeGeog3D("WGS 84", "World Geodetic System 1984",
                                  pdatum::Ellipsoid::WGS84);
  EXPECT_FALSE(
      factory->createOperations(wgs84_3d_noid, ed50, op_ctx_any).empty());

  auto vert_navd88_bad_id =
      af_epsg->createCoordinateReferenceSystem("5703")->alterId("EPSG", "4230");
  EXPECT_FALSE(
      factory->createOperations(wgs84, vert_navd88_bad_id, op_ctx_any).empty());

  auto vert_navd88_noid =
      MakeVertCrs("NAVD88 height", "North American Vertical Datum 1988");
  EXPECT_FALSE(
      factory->createOperations(wgs84_2d_named, vert_navd88_noid, op_ctx_any)
          .empty());

  // 3. Unknown datum name triggers early exit in buildCRSIds / intermediate
  // search.
  auto unknown_geog =
      MakeGeog2D("unknown", "unknown", pdatum::Ellipsoid::WGS84);
  EXPECT_FALSE(
      factory->createOperations(unknown_geog, ed50, op_ctx_any).empty());
  auto unknown_based = MakeGeog2D("Custom", "Unknown based on WGS 84 ellipsoid",
                                  pdatum::Ellipsoid::WGS84);
  EXPECT_FALSE(
      factory->createOperations(unknown_based, ed50, op_ctx_any).empty());

  // 4. Test distinct AuthorityFactory configurations + null authority factory.
  for (const auto& af :
       {af_lit_any.as_nullable(), af_epsg.as_nullable(), af_proj.as_nullable(),
        pio::AuthorityFactoryPtr(nullptr)}) {
    auto c = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
    EXPECT_FALSE(factory->createOperations(wgs84, ed50, c).empty());
  }
}

TEST(CoordinateOperationFactoryTest,
     IntermediatePivotsForceSearchPivotAndDatumBasedPivot) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();

  // 1. Explicit intermediate CRS + ALWAYS mode.
  auto src = epsg->createCoordinateReferenceSystem("4807");  // NTF (Paris)
  auto dst = epsg->createCoordinateReferenceSystem("4230");  // ED50
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  op_ctx->setAllowUseIntermediateCRS(
      pop::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  op_ctx->setIntermediateCRS({{"EPSG", "4258"}});
  EXPECT_FALSE(factory->createOperations(src, dst, op_ctx).empty());

  // NEVER mode avoids DB intermediate pivot search.
  op_ctx->setAllowUseIntermediateCRS(
      pop::CoordinateOperationContext::IntermediateCRSUse::NEVER);
  EXPECT_FALSE(factory->createOperations(src, dst, op_ctx).empty());

  // 2. PROJ_FORCE_SEARCH_PIVOT forces pivot search even when direct op exists.
  {
    ScopedEnvVar force_pivot("PROJ_FORCE_SEARCH_PIVOT", "1");
    auto c = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
    c->setAllowUseIntermediateCRS(
        pop::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
    EXPECT_FALSE(
        factory
            ->createOperations(epsg->createCoordinateReferenceSystem("4258"),
                               epsg->createCoordinateReferenceSystem("4326"), c)
            .empty());
  }

  // 3. Un-coded GeogCRS using real EPSG OSGB36 and DHDN datums hits candidate
  // lookup in findsOpsInRegistryWithIntermediate (forward & reverse).
  auto osgb_datum = epsg->createGeodeticDatum("6277");  // OSGB36
  auto dhdn_datum = epsg->createGeodeticDatum("6314");  // DHDN
  auto cs_latlon = pcs::EllipsoidalCS::createLatitudeLongitude(
      pcommon::UnitOfMeasure::DEGREE);
  auto custom_osgb = pcrs::GeographicCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Custom OSGB"),
      osgb_datum, cs_latlon);
  auto custom_dhdn = pcrs::GeographicCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Custom DHDN"),
      dhdn_datum, cs_latlon);
  auto c_any = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  c_any->setAllowUseIntermediateCRS(
      pop::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  EXPECT_FALSE(
      factory->createOperations(custom_osgb, custom_dhdn, c_any).empty());
  EXPECT_FALSE(factory->createOperations(custom_osgb, dst, c_any).empty());
  EXPECT_FALSE(factory->createOperations(dst, custom_osgb, c_any).empty());
}

TEST(CoordinateOperationFactoryTest, IgnfGridPivotAndGeneralDatumPivot) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  // IGNF fast-path pivot (nIters == 1).
  auto ignf_ntfp = TryCreateCrs(db, "IGNF:NTFP");
  auto ignf_rgf93g = TryCreateCrs(db, "IGNF:RGF93G");
  if (ignf_ntfp && ignf_rgf93g) {
    EXPECT_FALSE(factory
                     ->createOperations(NN_CHECK_THROW(ignf_ntfp),
                                        NN_CHECK_THROW(ignf_rgf93g), op_ctx)
                     .empty());
  }

  // General 3D datum pivot where source & target differ in 3D datum/axes.
  auto etrs_3d = epsg->createCoordinateReferenceSystem("4937");
  auto itrf14_3d = TryCreateCrs(db, "EPSG:7912");
  if (itrf14_3d) {
    auto ops_3d =
        factory->createOperations(etrs_3d, NN_CHECK_THROW(itrf14_3d), op_ctx);
    EXPECT_FALSE(ops_3d.empty());
    ExportOpsToProj5Strings(db.as_nullable(), ops_3d);
  }

  // OSGB36 <-> ED50 crosses datum pivot via WGS84/ETRS89.
  auto osgb = epsg->createCoordinateReferenceSystem("4277");
  auto ed50 = epsg->createCoordinateReferenceSystem("4230");
  auto ops_pivot = factory->createOperations(osgb, ed50, op_ctx);
  EXPECT_FALSE(ops_pivot.empty());
}

// ---------------------------------------------------------------------------
// 4. Geog/Geod Conversions, Celestial Bodies, SphericalCS & Errors
// ---------------------------------------------------------------------------

TEST(CoordinateOperationFactoryTest, GeogToGeogSyntheticAndZeroUnitThrow) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  // 1. Zero conversion factor on target vertical axis throws InvalidOperation.
  pcommon::UnitOfMeasure zero_lin("zero_len", 0.0,
                                  pcommon::UnitOfMeasure::Type::LINEAR);
  auto g3d_m = MakeGeog3D("G3D_M", "D1", pdatum::Ellipsoid::WGS84);
  auto g3d_zero = MakeGeog3D("G3D_Z", "D1", pdatum::Ellipsoid::WGS84,
                             pdatum::PrimeMeridian::GREENWICH,
                             pcommon::UnitOfMeasure::DEGREE, zero_lin);
  EXPECT_THROW(factory->createOperations(g3d_m, g3d_zero, op_ctx),
               pop::InvalidOperation);

  // 2. Z unit conversion only (same datum/PM/axis order, METRE <-> FOOT).
  auto g3d_ft =
      MakeGeog3D("G3D_FT", "D1", pdatum::Ellipsoid::WGS84,
                 pdatum::PrimeMeridian::GREENWICH,
                 pcommon::UnitOfMeasure::DEGREE, pcommon::UnitOfMeasure::FOOT);
  auto ops_z = factory->createOperations(g3d_m, g3d_ft, op_ctx);
  ASSERT_FALSE(ops_z.empty());

  // 3. Axis order reversal + Z unit change + PM -> createGeodToGeodPROJBased.
  auto g3d_paris_lonlat_ft = MakeGeog3D(
      "G3D_Paris", "D1", pdatum::Ellipsoid::WGS84, pdatum::PrimeMeridian::PARIS,
      pcommon::UnitOfMeasure::GRAD, pcommon::UnitOfMeasure::FOOT,
      /*lon_lat=*/true, /*height_down=*/true);
  auto ops_proj_based =
      factory->createOperations(g3d_m, g3d_paris_lonlat_ft, op_ctx);
  ASSERT_FALSE(ops_proj_based.empty());
  std::vector<std::string> proj_strs;
  ExportOpsToProj5Strings(db.as_nullable(), ops_proj_based, &proj_strs);
  ASSERT_FALSE(proj_strs.empty());
  EXPECT_THAT(proj_strs.front(), HasSubstr("step"));

  // 4. Same datum 2D & 3D axis-order swap + angular unit conversion.
  auto g2d_latlon = MakeGeog2D("G2D_LatLon", "D1", pdatum::Ellipsoid::WGS84,
                               pdatum::PrimeMeridian::GREENWICH,
                               pcommon::UnitOfMeasure::DEGREE, false);
  auto g2d_lonlat_rad = MakeGeog2D("G2D_LonLat", "D1", pdatum::Ellipsoid::WGS84,
                                   pdatum::PrimeMeridian::GREENWICH,
                                   pcommon::UnitOfMeasure::RADIAN, true);
  EXPECT_FALSE(
      factory->createOperations(g2d_latlon, g2d_lonlat_rad, op_ctx).empty());

  auto g3d_lonlat = MakeGeog3D("G3D_LonLat", "D1", pdatum::Ellipsoid::WGS84,
                               pdatum::PrimeMeridian::GREENWICH,
                               pcommon::UnitOfMeasure::DEGREE,
                               pcommon::UnitOfMeasure::METRE, true);
  EXPECT_FALSE(factory->createOperations(g3d_m, g3d_lonlat, op_ctx).empty());

  // 5. Same ellipsoid, different Prime Meridian (forward & inverse).
  auto g2d_paris = MakeGeog2D("G2D_Paris", "D1_Paris", pdatum::Ellipsoid::WGS84,
                              pdatum::PrimeMeridian::PARIS);
  EXPECT_FALSE(
      factory->createOperations(g2d_latlon, g2d_paris, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(g2d_paris, g2d_latlon, op_ctx).empty());

  // 6. Different ellipsoid + non-zero PM on target or source.
  auto g2d_clarke_greenwich =
      MakeGeog2D("G2D_Clarke", "ClarkeDatum", pdatum::Ellipsoid::CLARKE_1866);
  auto g2d_wgs_rome = MakeGeog2D(
      "G2D_Rome", "WGS_Rome", pdatum::Ellipsoid::WGS84,
      pdatum::PrimeMeridian::create(
          putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Rome"),
          pcommon::Angle(12.452333, pcommon::UnitOfMeasure::DEGREE)));
  EXPECT_FALSE(
      factory->createOperations(g2d_clarke_greenwich, g2d_wgs_rome, op_ctx)
          .empty());
  EXPECT_FALSE(
      factory->createOperations(g2d_wgs_rome, g2d_clarke_greenwich, op_ctx)
          .empty());
}

TEST(CoordinateOperationFactoryTest,
     CelestialBodyMismatchThrowsUnlessOverrideSet) {
  ScopedEnvVar env_celestial("PROJ_IGNORE_CELESTIAL_BODY", nullptr);

  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  auto mars1 = MakeGeog2D("Mars 2000", "Mars 2000",
                          MakeSphere("Mars", 3396190.0, "Mars"));
  auto mars2 = MakeGeog2D("Mars Sphere B", "Mars B",
                          MakeSphere("Mars", 3390000.0, "Mars"));
  auto earth = MakeGeog2D("Earth", "Earth", pdatum::Ellipsoid::WGS84);

  // Same celestial body (non-Earth) succeeds without override.
  EXPECT_FALSE(factory->createOperations(mars1, mars2, op_ctx).empty());

  // Different celestial bodies throw when unset, "NO", "FALSE", "OFF".
  for (const char* val :
       {static_cast<const char*>(nullptr), "NO", "FALSE", "OFF"}) {
    env_celestial.Set(val);
    EXPECT_THROW(factory->createOperations(mars1, earth, op_ctx),
                 putil::UnsupportedOperationException);
  }

  // Overriding with "YES" bypasses the celestial body error.
  env_celestial.Set("YES");
  EXPECT_FALSE(factory->createOperations(mars1, earth, op_ctx).empty());
}

TEST(CoordinateOperationFactoryTest,
     GeocentricSphericalPlanetocentricAndBound) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  auto wgs_geocen = epsg->createCoordinateReferenceSystem("4978");
  auto wgs_geog2d = epsg->createCoordinateReferenceSystem("4326");
  auto wgs_geog3d = epsg->createCoordinateReferenceSystem("4979");
  auto etrs_geog2d = epsg->createCoordinateReferenceSystem("4258");

  EXPECT_FALSE(
      factory->createOperations(wgs_geocen, wgs_geog2d, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(wgs_geocen, wgs_geog3d, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(wgs_geocen, etrs_geog2d, op_ctx).empty());

  auto custom_geocen = pcrs::GeodeticCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "CustomGeocen"),
      pdatum::GeodeticReferenceFrame::create(
          putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                   "CustomDatumGeocen"),
          pdatum::Ellipsoid::CLARKE_1866,
          osgeo::proj::util::optional<std::string>(),
          pdatum::PrimeMeridian::GREENWICH),
      pcs::CartesianCS::createGeocentric(pcommon::UnitOfMeasure::FOOT));
  auto ops_gc = factory->createOperations(wgs_geocen, custom_geocen, op_ctx);
  EXPECT_FALSE(ops_gc.empty());

  // Spherical planetocentric CS (2D: "Planetocentric latitude", "Planetocentric
  // longitude") on WGS 84 datum & custom datum.
  auto p_lat_axis = pcs::CoordinateSystemAxis::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Planetocentric latitude"),
      "lat", pcs::AxisDirection::NORTH, pcommon::UnitOfMeasure::DEGREE);
  auto p_lon_axis = pcs::CoordinateSystemAxis::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Planetocentric longitude"),
      "lon", pcs::AxisDirection::EAST, pcommon::UnitOfMeasure::DEGREE);
  auto sph_cs_2d =
      pcs::SphericalCS::create(putil::PropertyMap(), p_lat_axis, p_lon_axis);

  auto sph_wgs = pcrs::GeodeticCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "SphWGS"),
      pdatum::GeodeticReferenceFrame::EPSG_6326, sph_cs_2d);
  ASSERT_TRUE(sph_wgs->isSphericalPlanetocentric());

  auto clarke_datum = pdatum::GeodeticReferenceFrame::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "SphClarkeDatum"),
      pdatum::Ellipsoid::CLARKE_1866,
      osgeo::proj::util::optional<std::string>(),
      pdatum::PrimeMeridian::GREENWICH);
  auto sph_clarke = pcrs::GeodeticCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "SphClarke"),
      clarke_datum, sph_cs_2d);
  ASSERT_TRUE(sph_clarke->isSphericalPlanetocentric());

  for (const auto& target :
       {wgs_geog2d, wgs_geog3d, wgs_geocen,
        putil::nn_static_pointer_cast<pcrs::CRS>(sph_clarke)}) {
    auto fwd = factory->createOperations(sph_wgs, target, op_ctx);
    EXPECT_FALSE(fwd.empty());
    ExportOpsToProj5Strings(db.as_nullable(), fwd);
    auto inv = factory->createOperations(target, sph_wgs, op_ctx);
    EXPECT_FALSE(inv.empty());
    ExportOpsToProj5Strings(db.as_nullable(), inv);
  }

  // BoundCRS of spherical planetocentric CRS (forward & inverse).
  auto bound_sph = MakeHelmertBound(sph_clarke, pcrs::GeographicCRS::EPSG_4326);
  for (const auto& target : {wgs_geog2d, wgs_geog3d}) {
    auto ops_bs_fwd = factory->createOperations(bound_sph, target, op_ctx);
    auto ops_bs_inv = factory->createOperations(target, bound_sph, op_ctx);
    EXPECT_FALSE(ops_bs_fwd.empty());
    EXPECT_FALSE(ops_bs_inv.empty());
    ExportOpsToProj5Strings(db.as_nullable(), ops_bs_fwd);
    ExportOpsToProj5Strings(db.as_nullable(), ops_bs_inv);
  }

  // Non-planetocentric 3-axis SphericalCS hits the unknown-geodetic fallback
  // (createGeodToGeodPROJBased) which throws FormattingException on export.
  auto r_axis = pcs::CoordinateSystemAxis::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Radius"),
      "R", pcs::AxisDirection::UP, pcommon::UnitOfMeasure::METRE);
  auto sph_cs_3d = pcs::SphericalCS::create(putil::PropertyMap(), p_lat_axis,
                                            p_lon_axis, r_axis);
  auto sph_3d_wgs = pcrs::GeodeticCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Sph3DWGS"),
      pdatum::GeodeticReferenceFrame::EPSG_6326, sph_cs_3d);
  auto sph_3d_clarke = pcrs::GeodeticCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Sph3DClarke"),
      clarke_datum, sph_cs_3d);
  EXPECT_THROW(factory->createOperations(sph_3d_wgs, sph_3d_clarke, op_ctx),
               pio::FormattingException);
}

// ---------------------------------------------------------------------------
// 5. Recursion Guard, PROJ4 Ext (+wktext), Derived, Engineering, Ensemble & PMO
// ---------------------------------------------------------------------------

TEST(CoordinateOperationFactoryTest,
     RecursionLimitProj4ExtEngineeringAndTemporal) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  // 1. Nest 11 DerivedGeographicCRS objects to hit nRecLevelCreateOperations
  // == 10.
  pcrs::GeographicCRSNNPtr current_geog =
      MakeGeog2D("unknown", "unknown", pdatum::Ellipsoid::WGS84);
  auto noop_conv = pop::Conversion::createGeographic2DOffsets(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Id"),
      pcommon::Angle(0.0), pcommon::Angle(0.0));
  auto ellps_cs = pcs::EllipsoidalCS::createLatitudeLongitude(
      pcommon::UnitOfMeasure::DEGREE);
  for (int i = 0; i < 11; ++i) {
    current_geog = pcrs::DerivedGeographicCRS::create(
        putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                 "DerivedLevel" + std::to_string(i)),
        current_geog, noop_conv, ellps_cs);
  }
  EXPECT_THROW(
      factory->createOperations(
          current_geog, epsg->createCoordinateReferenceSystem("4258"), op_ctx),
      pop::InvalidOperation);

  // 2. EXTENSION_PROJ4 (+wktext) forward/reverse + BoundCRS + non-exportable
  // throw.
  auto wktext_geog = pcrs::GeographicCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "ExtGeog")
          .set("EXTENSION_PROJ4", "+proj=longlat +datum=WGS84 +no_defs"),
      pdatum::GeodeticReferenceFrame::EPSG_6326, ellps_cs);
  ASSERT_FALSE(wktext_geog->getExtensionProj4().empty());
  auto wktext_bound_proj =
      MakeHelmertBound(wktext_geog, pcrs::GeographicCRS::EPSG_4326);
  auto plain_utm =
      TryCreateCrs(db, "+proj=utm +zone=32 +datum=WGS84 +type=crs");
  ASSERT_NE(plain_utm, nullptr);

  EXPECT_FALSE(
      factory->createOperations(wktext_geog, NN_CHECK_THROW(plain_utm), op_ctx)
          .empty());
  EXPECT_FALSE(factory
                   ->createOperations(NN_CHECK_THROW(plain_utm),
                                      wktext_bound_proj, op_ctx)
                   .empty());

  auto eng_datum = pdatum::EngineeringDatum::create(putil::PropertyMap().set(
      pcommon::IdentifiedObject::NAME_KEY, "EngDatum"));
  auto eng_cs =
      pcs::CartesianCS::createEastingNorthing(pcommon::UnitOfMeasure::METRE);
  auto eng1 = pcrs::EngineeringCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Eng1"),
      eng_datum, eng_cs);
  auto eng2 = pcrs::EngineeringCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Eng2"),
      pdatum::EngineeringDatum::create(putil::PropertyMap().set(
          pcommon::IdentifiedObject::NAME_KEY, "EngDatum2")),
      eng_cs);

  // Non-exportable EngineeringCRS with EXTENSION_PROJ4 throws InvalidOperation.
  EXPECT_THROW(factory->createOperations(wktext_geog, eng1, op_ctx),
               pop::InvalidOperation);
  EXPECT_THROW(factory->createOperations(eng1, wktext_geog, op_ctx),
               pop::InvalidOperation);

  // Equivalent EngineeringCRS returns CartesianGridOffsets identity; non-eq is
  // empty.
  EXPECT_EQ(factory->createOperations(eng1, eng1, op_ctx).size(), 1u);
  EXPECT_TRUE(factory->createOperations(eng1, eng2, op_ctx).empty());

  // TemporalCRS produces empty operations against incompatible CRS.
  auto temp_datum = pdatum::TemporalDatum::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "TDatum"),
      pcommon::DateTime::create("1970-01-01T00:00:00Z"),
      pdatum::TemporalDatum::CALENDAR_PROLEPTIC_GREGORIAN);
  auto temp_axis = pcs::CoordinateSystemAxis::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Time"),
      "t", pcs::AxisDirection::FUTURE, pcommon::UnitOfMeasure::SECOND);
  auto temp_crs = pcrs::TemporalCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "TempCRS"),
      temp_datum,
      pcs::DateTimeTemporalCS::create(putil::PropertyMap(), temp_axis));
  EXPECT_TRUE(
      factory->createOperations(temp_crs, NN_CHECK_THROW(plain_utm), op_ctx)
          .empty());
}

TEST(CoordinateOperationFactoryTest,
     DerivedCrsChainsAndVerticalUnitOptimization) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  // 1. DerivedProjectedCRS -> base ProjectedCRS (direct) and -> WGS 84 2D
  // (chain).
  auto base_proj = epsg->createProjectedCRS("32632");
  auto affine = pop::Conversion::createAffineParametric(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Affine"),
      pcommon::Length(1.0), pcommon::Scale(1.0), pcommon::Scale(0.0),
      pcommon::Length(2.0), pcommon::Scale(0.0), pcommon::Scale(1.0));
  auto derived_proj = pcrs::DerivedProjectedCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "DerivedUTM"),
      base_proj, affine,
      pcs::CartesianCS::createEastingNorthing(pcommon::UnitOfMeasure::METRE));
  auto wgs84 = epsg->createCoordinateReferenceSystem("4326");
  EXPECT_EQ(factory->createOperations(derived_proj, base_proj, op_ctx).size(),
            1u);
  EXPECT_FALSE(factory->createOperations(derived_proj, wgs84, op_ctx).empty());
  EXPECT_FALSE(factory->createOperations(wgs84, derived_proj, op_ctx).empty());

  // 2. DerivedVerticalCRS with "WGS 84" vertical datum and unit scale 1.0 ->
  // WGS 84 3D triggers the no-op conversion stripping optimization in
  // createOperationsDerivedTo.
  auto vert_wgs = MakeVertCrs("WGS 84 height", "World Geodetic System 1984");
  auto conv_vert = pop::Conversion::createChangeVerticalUnit(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Scale1"),
      pcommon::Scale(1.0));
  auto derived_vert = pcrs::DerivedVerticalCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "DerivedVertWGS"),
      vert_wgs, conv_vert,
      pcs::VerticalCS::createGravityRelatedHeight(
          pcommon::UnitOfMeasure::METRE));
  auto wgs84_3d = epsg->createCoordinateReferenceSystem("4979");
  EXPECT_FALSE(
      factory->createOperations(derived_vert, wgs84_3d, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(wgs84_3d, derived_vert, op_ctx).empty());
}

TEST(CoordinateOperationFactoryTest,
     DatumEnsemblesPointMotionOpsAndCoordinateMetadataOverloads) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  // 1. ETRS89 / WGS84 DatumEnsemble to member and non-member CRSs.
  auto wgs84 = epsg->createCoordinateReferenceSystem("4326");
  auto wgs84_3d = epsg->createCoordinateReferenceSystem("4979");
  auto etrs89 = epsg->createCoordinateReferenceSystem("4258");
  auto etrs89_3d = epsg->createCoordinateReferenceSystem("4937");
  auto etrf2000 = TryCreateCrs(db, "EPSG:9067");
  auto wgs84_g1762 = TryCreateCrs(db, "EPSG:9055");
  auto rgf93 = epsg->createCoordinateReferenceSystem("4171");
  auto itrf2020 = TryCreateCrs(db, "EPSG:9989");
  auto itrf2014 = TryCreateCrs(db, "EPSG:7912");

  EXPECT_FALSE(factory->createOperations(etrs89, rgf93, op_ctx).empty());
  EXPECT_FALSE(factory->createOperations(wgs84_3d, etrs89_3d, op_ctx).empty());
  if (etrf2000) {
    EXPECT_FALSE(
        factory->createOperations(etrs89, NN_CHECK_THROW(etrf2000), op_ctx)
            .empty());
    auto amersfoort = epsg->createCoordinateReferenceSystem("4289");
    EXPECT_FALSE(
        factory->createOperations(NN_CHECK_THROW(etrf2000), amersfoort, op_ctx)
            .empty());
  }
  if (wgs84_g1762) {
    // Ensemble-to-member exclusion filtering path.
    EXPECT_FALSE(
        factory->createOperations(wgs84, NN_CHECK_THROW(wgs84_g1762), op_ctx)
            .empty());
  }
  if (itrf2020) {
    // hasTooSmallAreas heuristic on global dynamic frame -> regional ETRS89.
    EXPECT_FALSE(
        factory->createOperations(NN_CHECK_THROW(itrf2020), etrs89_3d, op_ctx)
            .empty());
  }

  // 2. PointMotionOperations via CoordinateMetadata overloads (9.2 and 9.4
  // APIs).
  auto dynamic_crs = itrf2014 ? NN_CHECK_THROW(itrf2014) : wgs84_3d;
  auto cm_2010 = pcoords::CoordinateMetadata::create(dynamic_crs, 2010.0, db);
  auto cm_2020 = pcoords::CoordinateMetadata::create(dynamic_crs, 2020.0, db);

  auto pmo_ops = factory->createOperations(cm_2010, cm_2020, op_ctx);
  EXPECT_FALSE(pmo_ops.empty());
  for (const auto& op : pmo_ops) {
    EXPECT_TRUE(op->sourceCoordinateEpoch().has_value());
    EXPECT_TRUE(op->targetCoordinateEpoch().has_value());
  }
  EXPECT_FALSE(factory->createOperations(cm_2010, etrs89_3d, op_ctx).empty());
  EXPECT_FALSE(factory->createOperations(etrs89_3d, cm_2020, op_ctx).empty());

  // NAD83(CSRS)v7 (EPSG:8254 3D and EPSG:8252 Geocentric) has registered
  // PointMotionOperations in proj.db, exercising getPointMotionOperationsFor.
  for (const char* code : {"8254", "8252"}) {
    if (auto csrs_v7 = TryCreateCrs(db, std::string("EPSG:") + code)) {
      auto cm_src = pcoords::CoordinateMetadata::create(NN_CHECK_THROW(csrs_v7),
                                                        2010.0, db);
      auto cm_dst = pcoords::CoordinateMetadata::create(NN_CHECK_THROW(csrs_v7),
                                                        2020.0, db);
      EXPECT_FALSE(factory->createOperations(cm_src, cm_dst, op_ctx).empty());
      if (std::string_view(code) == "8254") {
        EXPECT_FALSE(
            factory->createOperations(cm_src, wgs84_3d, op_ctx).empty());
        EXPECT_FALSE(
            factory->createOperations(wgs84_3d, cm_dst, op_ctx).empty());
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 6. Vertical, Geoid Models & Alternative Geographic Pivots
// ---------------------------------------------------------------------------

TEST(CoordinateOperationFactoryTest,
     GeogToVertFromGeoidInMemoryAndNamedGeoids) {
  auto ctx = MakeContext();
  InMemoryFileSystem fs;
  InstallTestGtxGrid(&fs, "/mem/test_geoid.gtx", 42.5f);
  ASSERT_TRUE(fs.Install(ctx.get()));
  absl::Cleanup cleanup = [] { proj_cleanup(); };

  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  auto wgs84_2d = epsg->createCoordinateReferenceSystem("4326");
  auto wgs84_3d = epsg->createCoordinateReferenceSystem("4979");
  auto wgs84_3d_ft =
      MakeGeog3D("WGS84 Ft", "World Geodetic System 1984",
                 pdatum::Ellipsoid::WGS84, pdatum::PrimeMeridian::GREENWICH,
                 pcommon::UnitOfMeasure::DEGREE, pcommon::UnitOfMeasure::FOOT);

  auto aoi = pmeta::Extent::createFromBBOX(-5.0, 42.0, 5.0, 48.0);

  // 1. GEOID_MODEL with "PROJ /mem/test_geoid.gtx", including " depth" and "
  // (ftUS)".
  auto base_vert = MakeVertCrs("Custom Geoid Base", "Custom Geoid Datum");
  auto model_gtx =
      MakeGeoidModel("PROJ /mem/test_geoid.gtx", base_vert,
                     wgs84_2d.as_nullable(), std::string(), std::string(), aoi);
  auto vert_m = MakeVertCrs("Custom height", "Custom Geoid Datum",
                            pcommon::UnitOfMeasure::METRE, false, model_gtx);
  auto vert_depth_ftus =
      MakeVertCrs("Custom depth (ftUS)", "Custom Geoid Datum",
                  pcommon::UnitOfMeasure::US_FOOT, true, model_gtx);

  auto ops_gtx = factory->createOperations(wgs84_3d_ft, vert_m, op_ctx);
  ASSERT_FALSE(ops_gtx.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_gtx);

  auto ops_depth = factory->createOperations(wgs84_2d, vert_depth_ftus, op_ctx);
  ASSERT_FALSE(ops_depth.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_depth);

  // 2. GEOID_MODEL referencing a known DB geoid name ("EGM96") and a missing
  // grid.
  auto model_egm96 = MakeGeoidModel("EGM96", base_vert, wgs84_2d.as_nullable());
  auto vert_egm =
      MakeVertCrs("Custom EGM", "Custom EGM Datum",
                  pcommon::UnitOfMeasure::METRE, false, model_egm96);
  EXPECT_FALSE(factory->createOperations(wgs84_3d, vert_egm, op_ctx).empty());

  auto model_missing = MakeGeoidModel("PROJ /mem/nonexistent_geoid.gtx",
                                      base_vert, wgs84_2d.as_nullable());
  auto vert_missing =
      MakeVertCrs("MissingGeoid", "MissingDatum", pcommon::UnitOfMeasure::METRE,
                  false, model_missing);
  EXPECT_FALSE(
      factory->createOperations(wgs84_3d, vert_missing, op_ctx).empty());
}

TEST(CoordinateOperationFactoryTest,
     GeogToVertIntermediateVertAlternativeGeogAndVertToVert) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  auto wgs84_2d = epsg->createCoordinateReferenceSystem("4326");
  auto navd88_ftus = epsg->createCoordinateReferenceSystem("6360");
  auto navd88_m = epsg->createCoordinateReferenceSystem("5703");
  auto egm96 = epsg->createCoordinateReferenceSystem("5773");

  // 1. Geog2D <-> NAVD88 (ftUS) via NAVD88 (metre).
  op_ctx->setAllowUseIntermediateCRS(
      pop::CoordinateOperationContext::IntermediateCRSUse::NEVER);
  EXPECT_FALSE(
      factory->createOperations(wgs84_2d, navd88_ftus, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(navd88_ftus, wgs84_2d, op_ctx).empty());
  EXPECT_FALSE(factory->createOperations(wgs84_2d, egm96, op_ctx).empty());

  // 2. Non-metre 3D Geog -> NAVD88 triggers
  // createOperationsGeogToVertWithAlternativeGeog.
  auto geog_non_m_z = MakeGeog3D(
      "Custom NAD83 (ftUS)", "North American Datum 1983",
      pdatum::Ellipsoid::GRS1980, pdatum::PrimeMeridian::GREENWICH,
      pcommon::UnitOfMeasure::DEGREE, pcommon::UnitOfMeasure::US_FOOT);
  EXPECT_FALSE(
      factory->createOperations(geog_non_m_z, navd88_m, op_ctx).empty());

  // 3. VertToVert with intermediate pivot (Baltic 1977 depth <-> Caspian
  // depth), height-depth reversal, unit change, and zero-unit throw.
  auto baltic_depth = TryCreateCrs(db, "EPSG:5705");
  auto caspian_depth = TryCreateCrs(db, "EPSG:5706");
  if (baltic_depth && caspian_depth) {
    EXPECT_FALSE(factory
                     ->createOperations(NN_CHECK_THROW(baltic_depth),
                                        NN_CHECK_THROW(caspian_depth), op_ctx)
                     .empty());
    EXPECT_FALSE(factory
                     ->createOperations(NN_CHECK_THROW(caspian_depth),
                                        NN_CHECK_THROW(baltic_depth), op_ctx)
                     .empty());
  }
  EXPECT_FALSE(factory->createOperations(navd88_m, egm96, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(navd88_m, navd88_ftus, op_ctx).empty());

  pcommon::UnitOfMeasure zero_lin("zero_vert", 0.0,
                                  pcommon::UnitOfMeasure::Type::LINEAR);
  auto vert_zero = MakeVertCrs("ZeroVert", "CustomVDatum", zero_lin);
  EXPECT_THROW(factory->createOperations(navd88_m, vert_zero, op_ctx),
               pop::InvalidOperation);

  // 4. VertToGeog: named DB match, Ellipsoid datum, ballpark & zero-unit throw.
  auto vert_named_db =
      MakeVertCrs("NAVD88 height", "North American Vertical Datum 1988");
  EXPECT_FALSE(
      factory->createOperations(vert_named_db, wgs84_2d, op_ctx).empty());

  auto g3d_zero =
      MakeGeog3D("G3D_ZeroV", "World Geodetic System 1984",
                 pdatum::Ellipsoid::WGS84, pdatum::PrimeMeridian::GREENWICH,
                 pcommon::UnitOfMeasure::DEGREE, zero_lin);
  EXPECT_THROW(factory->createOperations(navd88_m, g3d_zero, op_ctx),
               pop::InvalidOperation);

  auto vert_ellps_datum = MakeVertCrs("Ellipsoidal height", "Ellipsoid");
  auto geog_m = MakeGeog3D("WGS84 3D", "World Geodetic System 1984",
                           pdatum::Ellipsoid::WGS84);
  auto c_wgs = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "WGS84 + Ellipsoid"),
      {wgs84_2d, vert_ellps_datum});
  EXPECT_FALSE(factory->createOperations(c_wgs, geog_m, op_ctx).empty());
  EXPECT_FALSE(factory->createOperations(navd88_m, geog_m, op_ctx).empty());
}

// ---------------------------------------------------------------------------
// 7. BoundCRS Operations (BoundToGeog, BoundToVert, BoundToBound,
// BoundToCompound)
// ---------------------------------------------------------------------------

TEST(CoordinateOperationFactoryTest,
     BoundToGeogClarke66NadgridsBoundToVertAndBoundToBound) {
  auto ctx = MakeContext();
  InMemoryFileSystem fs;
  InstallTestGtxGrid(&fs, "/mem/bound_vert.gtx", 12.0f);
  ASSERT_TRUE(fs.Install(ctx.get()));
  absl::Cleanup cleanup = [] { proj_cleanup(); };

  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  auto wgs84_2d = epsg->createCoordinateReferenceSystem("4326");
  auto wgs84_3d = epsg->createCoordinateReferenceSystem("4979");
  auto etrs89_2d = epsg->createCoordinateReferenceSystem("4258");

  // 1. Clarke 1866 BoundCRS (+nadgrids) to NAD83 (triggers Clarke66 branch).
  auto clarke_bound_geog =
      TryCreateCrs(db, "+proj=longlat +ellps=clrk66 +nadgrids=@null +type=crs");
  auto clarke_bound_utm = TryCreateCrs(
      db, "+proj=utm +zone=16 +ellps=clrk66 +nadgrids=@null +type=crs");
  auto nad83 = epsg->createCoordinateReferenceSystem("4269");
  ASSERT_NE(clarke_bound_geog, nullptr);
  ASSERT_NE(clarke_bound_utm, nullptr);
  EXPECT_FALSE(
      factory
          ->createOperations(NN_CHECK_THROW(clarke_bound_geog), nad83, op_ctx)
          .empty());
  EXPECT_FALSE(
      factory->createOperations(NN_CHECK_THROW(clarke_bound_utm), nad83, op_ctx)
          .empty());

  // 2. Bound[VertCRS, WGS84_3D, grid] to Geog3D, VertCRS, and BoundToBound.
  auto vert1 = MakeVertCrs("BoundV1", "BoundVDatum1");
  auto vert2 = MakeVertCrs("BoundV2", "BoundVDatum2");
  auto grid_transf =
      pop::Transformation::createGravityRelatedHeightToGeographic3D(
          putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                   "MemGtx"),
          vert1, wgs84_3d, nullptr, "/mem/bound_vert.gtx",
          {pmeta::PositionalAccuracy::create("0.1")});
  auto bound_vert1 = pcrs::BoundCRS::create(vert1, wgs84_3d, grid_transf);
  auto bound_vert2 = pcrs::BoundCRS::create(
      vert2, wgs84_3d,
      pop::Transformation::createGravityRelatedHeightToGeographic3D(
          putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                   "MemGtx2"),
          vert2, wgs84_3d, nullptr, "/mem/bound_vert.gtx", {}));
  // Bound[vert1_ft] where transformation->sourceCRS() is vert1 (metre).
  auto vert1_ft =
      MakeVertCrs("BoundV1Ft", "BoundVDatum1", pcommon::UnitOfMeasure::FOOT);
  auto bound_vert1_mismatch =
      pcrs::BoundCRS::create(vert1_ft, wgs84_3d, grid_transf);

  EXPECT_FALSE(
      factory->createOperations(bound_vert1, wgs84_3d, op_ctx).empty());
  EXPECT_FALSE(factory->createOperations(bound_vert1_mismatch, wgs84_3d, op_ctx)
                   .empty());
  EXPECT_FALSE(
      factory->createOperations(bound_vert1, etrs89_2d, op_ctx).empty());

  // Zero-factor linear unit target throws in createOperationsBoundToGeog
  // (both directly and via CompoundToGeog with skipHorizontalTransformation).
  pcommon::UnitOfMeasure zero_lin("zero_b", 0.0,
                                  pcommon::UnitOfMeasure::Type::LINEAR);
  auto g3d_zero =
      MakeGeog3D("G3D_BZero", "World Geodetic System 1984",
                 pdatum::Ellipsoid::WGS84, pdatum::PrimeMeridian::GREENWICH,
                 pcommon::UnitOfMeasure::DEGREE, zero_lin);
  EXPECT_THROW(factory->createOperations(bound_vert1, g3d_zero, op_ctx),
               pop::InvalidOperation);
  auto comp_etrs_bv1 = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "ETRS89+BoundV1"),
      {etrs89_2d, bound_vert1});
  EXPECT_THROW(factory->createOperations(comp_etrs_bv1, g3d_zero, op_ctx),
               pop::InvalidOperation);
  auto etrs89_3d_ft =
      MakeGeog3D("ETRS89 3D Ft", "European Terrestrial Reference System 1989",
                 pdatum::Ellipsoid::GRS1980, pdatum::PrimeMeridian::GREENWICH,
                 pcommon::UnitOfMeasure::DEGREE, pcommon::UnitOfMeasure::FOOT);
  EXPECT_FALSE(
      factory->createOperations(comp_etrs_bv1, etrs89_3d_ft, op_ctx).empty());

  // BoundToVert and BoundToBound (same hub w/ vert, same vert datum, diff
  // hubs).
  auto navd88 = epsg->createCoordinateReferenceSystem("5703");
  EXPECT_FALSE(factory->createOperations(bound_vert1, navd88, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(bound_vert1, vert1_ft, op_ctx).empty());
  EXPECT_FALSE(
      factory
          ->createOperations(NN_CHECK_THROW(clarke_bound_geog), navd88, op_ctx)
          .empty());
  EXPECT_FALSE(
      factory->createOperations(bound_vert1, bound_vert2, op_ctx).empty());

  auto bound_h1 = MakeHelmertBound(
      MakeGeog2D("BH1", "BHD1", pdatum::Ellipsoid::CLARKE_1866), wgs84_2d);
  auto bound_h2 =
      MakeHelmertBound(MakeGeog2D("BH2", "BHD2", pdatum::Ellipsoid::GRS1980),
                       wgs84_2d, -50.0, -60.0, -70.0);
  auto bound_diff_hub = MakeHelmertBound(
      MakeGeog2D("BH3", "BHD3", pdatum::Ellipsoid::GRS1980), etrs89_2d);
  EXPECT_FALSE(factory->createOperations(bound_h1, bound_h2, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(bound_h1, bound_diff_hub, op_ctx).empty());
}

TEST(CoordinateOperationFactoryTest, BoundToCompoundAllBranches) {
  auto ctx = MakeContext();
  InMemoryFileSystem fs;
  InstallTestGtxGrid(&fs, "/mem/nad83_geoid.gtx", -30.0f);
  ASSERT_TRUE(fs.Install(ctx.get()));
  absl::Cleanup cleanup = [] { proj_cleanup(); };

  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  auto wgs84_2d = epsg->createCoordinateReferenceSystem("4326");
  auto wgs84_3d = epsg->createCoordinateReferenceSystem("4979");
  auto nad83_2d = epsg->createCoordinateReferenceSystem("4269");
  auto nad83_3d = nad83_2d->promoteTo3D("NAD83 3D", db.as_nullable());

  // 1. BOUND[NAD83_3D, TOWGS84] -> COMPOUND[BOUND[NAD83_2D, TOWGS84],
  // BOUND[VERT, NAD83_3D, GRID]]
  auto bound_nad83_3d = MakeHelmertBound(nad83_3d, wgs84_3d, 0.0, 0.0, 0.0);
  auto bound_nad83_2d = MakeHelmertBound(nad83_2d, wgs84_2d, 0.0, 0.0, 0.0);
  auto navd88_base = MakeVertCrs("NAVD88 Custom", "NAVD88 Datum");
  auto grid_op = pop::Transformation::createGravityRelatedHeightToGeographic3D(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "NAD83Grid"),
      navd88_base, nad83_3d, nullptr, "/mem/nad83_geoid.gtx", {});
  auto bound_vert_nad83 =
      pcrs::BoundCRS::create(navd88_base, nad83_3d, grid_op);
  auto comp_nad83 = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "CompBoundNAD83"),
      {bound_nad83_2d, bound_vert_nad83});

  EXPECT_FALSE(
      factory->createOperations(bound_nad83_3d, comp_nad83, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(comp_nad83, bound_nad83_3d, op_ctx).empty());

  // 2. BOUND[CRS1, WGS84] -> COMPOUND[BOUND[CRS2, WGS84], VERT] (same hub
  // datum).
  auto ed50 = epsg->createCoordinateReferenceSystem("4230");
  auto bound_ed50 = MakeHelmertBound(ed50, wgs84_2d);
  auto bound_wgs_comp = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "BoundNAD83+V"),
      {bound_nad83_2d, navd88_base});
  EXPECT_FALSE(
      factory->createOperations(bound_ed50, bound_wgs_comp, op_ctx).empty());

  // 3. BOUND[Other3D, WGS84_3D] -> COMPOUND[WGS84, VERT] (same 3D hub datum).
  auto other3d =
      MakeGeog3D("Other3D", "Other3DDatum", pdatum::Ellipsoid::GRS1980);
  auto bound_other3d = MakeHelmertBound(other3d, wgs84_3d, 10.0, 20.0, 30.0);
  auto comp_wgs_vert = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "WGS84+V"),
      {wgs84_2d, epsg->createCoordinateReferenceSystem("5773")});
  EXPECT_FALSE(
      factory->createOperations(bound_other3d, comp_wgs_vert, op_ctx).empty());
}

// ---------------------------------------------------------------------------
// 8. CompoundCRS (ToGeog, ToCompound, ToGeod, ProjInterp, Resolve & Execution)
// ---------------------------------------------------------------------------

TEST(CoordinateOperationFactoryTest,
     CompoundToGeogDerivedBoundHackAndProjInterp) {
  auto ctx = MakeContext();
  InMemoryFileSystem fs;
  InstallTestGtxGrid(&fs, "/mem/comp_geoid.gtx", 40.0f);
  ASSERT_TRUE(fs.Install(ctx.get()));
  absl::Cleanup cleanup = [] { proj_cleanup(); };

  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  auto wgs84_2d = epsg->createCoordinateReferenceSystem("4326");
  auto wgs84_3d = epsg->createCoordinateReferenceSystem("4979");
  auto etrs89_3d = epsg->createCoordinateReferenceSystem("4937");
  auto egm96 = epsg->createCoordinateReferenceSystem("5773");

  // 1. Compound[DerivedCRS, Vert] -> Geog and Compound[Bound[DerivedCRS], Vert]
  // -> Geog.
  auto utm32 = epsg->createProjectedCRS("32632");
  auto comp_derived = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "UTM32+EGM96"),
      {utm32, egm96});
  EXPECT_FALSE(
      factory->createOperations(comp_derived, wgs84_3d, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(comp_derived, wgs84_2d, op_ctx).empty());

  auto bound_utm32 = pcrs::BoundCRS::createFromTOWGS84(
      utm32, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
  auto comp_bound_derived = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "BoundUTM+V"),
      {bound_utm32, egm96});
  EXPECT_FALSE(
      factory->createOperations(comp_bound_derived, wgs84_3d, op_ctx).empty());

  // 2. Compound[Bound[Horiz], Bound[Vert]] via PROJ strings (+towgs84
  // +geoidgrids).
  auto proj_comp_geoid =
      TryCreateCrs(db,
                   "+proj=utm +zone=32 +ellps=intl +towgs84=-87,-98,-121 "
                   "+geoidgrids=/mem/comp_geoid.gtx +type=crs");
  ASSERT_NE(proj_comp_geoid, nullptr);
  auto ops_bound_comp = factory->createOperations(
      NN_CHECK_THROW(proj_comp_geoid), etrs89_3d, op_ctx);
  ASSERT_FALSE(ops_bound_comp.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_bound_comp);

  // 3. Compound[Horiz, Vert with ProjectedCRS interpolationCRS] -> hits
  // interpolationCRSProj branches in
  // MyPROJStringExportableHorizVerticalHorizPROJBased.
  auto rd_new = epsg->createProjectedCRS("28992");
  auto amersfoort_v = MakeVertCrs("CustomVertProjInterp", "CustomVDatumProj");
  auto geoid_proj_interp = MakeGeoidModel("PROJ /mem/comp_geoid.gtx",
                                          amersfoort_v, rd_new.as_nullable());
  auto amersfoort_v_geoid =
      MakeVertCrs("CustomVertProjInterp", "CustomVDatumProj",
                  pcommon::UnitOfMeasure::METRE, false, geoid_proj_interp);
  auto comp_proj_interp = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "RDNew+VertProjInterp"),
      {rd_new, amersfoort_v_geoid});
  auto ops_pi = factory->createOperations(comp_proj_interp, etrs89_3d, op_ctx);
  ASSERT_FALSE(ops_pi.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_pi);

  // 4. Special DB paths: EPSG:7415 -> EPSG:4937 / EPSG:4979 and NAD83(CSRS)v7
  // + CGVD2013a(1997) @ 1997 -> NAD83(CSRS)v7 3D @ 1997.
  if (auto rd_nap = TryCreateCrs(db, "EPSG:7415")) {
    auto ops_nl =
        factory->createOperations(NN_CHECK_THROW(rd_nap), etrs89_3d, op_ctx);
    EXPECT_FALSE(ops_nl.empty());
    ExportOpsToProj5Strings(db.as_nullable(), ops_nl);
  }
  if (auto nad83_csrs7_2d = TryCreateCrs(db, "EPSG:8255")) {
    if (auto nad83_csrs7_3d = TryCreateCrs(db, "EPSG:8254")) {
      if (auto cgvd_97 = TryFindCrsByName(
              af, "CGVD2013a(1997) height",
              pio::AuthorityFactory::ObjectType::VERTICAL_CRS)) {
        auto comp_ca97 = pcrs::CompoundCRS::create(
            putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                     "NAD83v7+CGVD97"),
            {NN_CHECK_THROW(nad83_csrs7_2d), NN_CHECK_THROW(cgvd_97)});
        auto c97 = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
        c97->setSourceCoordinateEpoch(
            putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
                pcommon::Measure(1997.0, pcommon::UnitOfMeasure::YEAR))));
        c97->setTargetCoordinateEpoch(
            putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
                pcommon::Measure(1997.0, pcommon::UnitOfMeasure::YEAR))));
        EXPECT_FALSE(factory
                         ->createOperations(comp_ca97,
                                            NN_CHECK_THROW(nad83_csrs7_3d), c97)
                         .empty());
      }
    }
  }
}

TEST(CoordinateOperationFactoryTest,
     CompoundToCompoundAllBranchesPmoAndPipelineExecution) {
  auto ctx = MakeContext();
  InMemoryFileSystem fs;
  InstallTestGtxGrid(&fs, "/mem/c2c_geoid1.gtx", 10.0f);
  InstallTestGtxGrid(&fs, "/mem/c2c_geoid2.gtx", 20.0f);
  ASSERT_TRUE(fs.Install(ctx.get()));
  absl::Cleanup cleanup = [] { proj_cleanup(); };

  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  // 1. Mismatched component counts & non-geog horizontal component -> empty.
  auto eng = pcrs::EngineeringCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "EngC"),
      pdatum::EngineeringDatum::create(putil::PropertyMap().set(
          pcommon::IdentifiedObject::NAME_KEY, "EngD")),
      pcs::CartesianCS::createEastingNorthing(pcommon::UnitOfMeasure::METRE));
  auto navd88 = epsg->createCoordinateReferenceSystem("5703");
  auto egm96 = epsg->createCoordinateReferenceSystem("5773");
  auto comp_eng = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Eng+V"),
      {eng, navd88});
  auto comp_wgs = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "WGS+V"),
      {epsg->createCoordinateReferenceSystem("4326"), egm96});
  EXPECT_TRUE(factory->createOperations(comp_eng, comp_wgs, op_ctx).empty());

  // 2. Compound[Bound, Bound] <-> Compound[Bound, Bound] (same hub).
  auto c2c_1 =
      TryCreateCrs(db,
                   "+proj=utm +zone=32 +ellps=intl +towgs84=-87,-98,-121 "
                   "+geoidgrids=/mem/c2c_geoid1.gtx +type=crs");
  auto c2c_2 = TryCreateCrs(
      db,
      "+proj=tmerc +lat_0=0 +lon_0=9 +k=1 +x_0=0 +y_0=0 +ellps=clrk66 "
      "+towgs84=-10,150,180 +geoidgrids=/mem/c2c_geoid2.gtx +type=crs");
  ASSERT_NE(c2c_1, nullptr);
  ASSERT_NE(c2c_2, nullptr);
  std::vector<std::string> c2c_proj;
  auto ops_c2c = factory->createOperations(NN_CHECK_THROW(c2c_1),
                                           NN_CHECK_THROW(c2c_2), op_ctx);
  ASSERT_FALSE(ops_c2c.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_c2c, &c2c_proj);
  ASSERT_FALSE(c2c_proj.empty());

  // Verify the exported PROJ string actually executes cleanly on ctx.
  {
    ScopedPj pj(proj_create(ctx.get(), c2c_proj.front().c_str()));
    ASSERT_NE(pj.get(), nullptr);
    PJ_COORD in = proj_coord(500000.0, 5500000.0, 100.0, 0.0);
    PJ_COORD out = proj_trans(pj.get(), PJ_FWD, in);
    EXPECT_EQ(0, proj_errno(pj.get()));
    EXPECT_TRUE(std::isfinite(out.xyz.x));
    EXPECT_TRUE(std::isfinite(out.xyz.y));
    EXPECT_TRUE(std::isfinite(out.xyz.z));
  }

  // 3. Belgian BD72 + Ostend <-> Amersfoort + NAP (uses ETRS89 interpolation
  // pivot) and Slovenian EPSG:3912+5779 -> EPSG:3794+8690 (horiz + null vert).
  auto bel_comp = TryCreateCrs(db, "EPSG:31370+5710");
  auto nl_comp = TryCreateCrs(db, "EPSG:7415");
  if (bel_comp && nl_comp) {
    auto ops_bn = factory->createOperations(NN_CHECK_THROW(bel_comp),
                                            NN_CHECK_THROW(nl_comp), op_ctx);
    EXPECT_FALSE(ops_bn.empty());
    ExportOpsToProj5Strings(db.as_nullable(), ops_bn);
  }
  auto si_1 = TryCreateCrs(db, "EPSG:3912+5779");
  auto si_2 = TryCreateCrs(db, "EPSG:3794+8690");
  if (si_1 && si_2) {
    EXPECT_FALSE(factory
                     ->createOperations(NN_CHECK_THROW(si_1),
                                        NN_CHECK_THROW(si_2), op_ctx)
                     .empty());
  }

  // 4. Compound -> GeocentricCRS (createOperationsToGeod & inverse).
  auto wgs_geocen = epsg->createCoordinateReferenceSystem("4978");
  auto ops_to_geocen = factory->createOperations(comp_wgs, wgs_geocen, op_ctx);
  EXPECT_FALSE(ops_to_geocen.empty());
  EXPECT_FALSE(factory->createOperations(wgs_geocen, comp_wgs, op_ctx).empty());

  // 5. Same Compound with epochs triggers PMO check in CompoundToCompound.
  auto c_ep = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  c_ep->setSourceCoordinateEpoch(
      putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
          pcommon::Measure(2015.0, pcommon::UnitOfMeasure::YEAR))));
  c_ep->setTargetCoordinateEpoch(
      putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
          pcommon::Measure(2020.0, pcommon::UnitOfMeasure::YEAR))));
  if (auto itrf_2d = TryCreateCrs(db, "EPSG:8999")) {
    auto comp_pmo = pcrs::CompoundCRS::create(
        putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                 "ITRF+EGM"),
        {NN_CHECK_THROW(itrf_2d), egm96});
    EXPECT_FALSE(factory->createOperations(comp_pmo, comp_pmo, c_ep).empty());
  }
  if (auto csrs7_2d = TryCreateCrs(db, "EPSG:8255")) {
    auto cgvd_97 =
        TryFindCrsByName(af, "CGVD2013a(1997) height",
                         pio::AuthorityFactory::ObjectType::VERTICAL_CRS);
    auto cgvd_02 =
        TryFindCrsByName(af, "CGVD2013a(2002) height",
                         pio::AuthorityFactory::ObjectType::VERTICAL_CRS);
    if (cgvd_97 && cgvd_02) {
      auto comp_ca97 = pcrs::CompoundCRS::create(
          putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                   "NAD83v7+CGVD2013a(1997)"),
          {NN_CHECK_THROW(csrs7_2d), NN_CHECK_THROW(cgvd_97)});
      auto comp_ca02 = pcrs::CompoundCRS::create(
          putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                   "NAD83v7+CGVD2013a(2002)"),
          {NN_CHECK_THROW(csrs7_2d), NN_CHECK_THROW(cgvd_02)});
      auto c_ca_ep = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
      c_ca_ep->setSpatialCriterion(pop::CoordinateOperationContext::
                                       SpatialCriterion::PARTIAL_INTERSECTION);
      c_ca_ep->setSourceCoordinateEpoch(
          putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
              pcommon::Measure(1997.0, pcommon::UnitOfMeasure::YEAR))));
      c_ca_ep->setTargetCoordinateEpoch(
          putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
              pcommon::Measure(2002.0, pcommon::UnitOfMeasure::YEAR))));
      auto ops_ca_pmo =
          factory->createOperations(comp_ca97, comp_ca02, c_ca_ep);
      EXPECT_FALSE(ops_ca_pmo.empty());
      ExportOpsToProj5Strings(db.as_nullable(), ops_ca_pmo);
    }
  }

  // 6. Compound[WGS84, BallparkVert] <-> Compound[ETRS89, IdenticalVert].
  auto custom_v = MakeVertCrs("SameVert", "SameVertDatum");
  auto comp_a = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "WGS+SameV"),
      {epsg->createCoordinateReferenceSystem("4326"), custom_v});
  auto comp_b = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "ETRS+SameV"),
      {epsg->createCoordinateReferenceSystem("4258"), custom_v});
  EXPECT_FALSE(factory->createOperations(comp_a, comp_b, op_ctx).empty());
}

TEST(CoordinateOperationFactoryTest,
     CanonicalBoundCrsAndGetResolvedCrsEdgeCases) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  // 1. CRS with +over returns early in getResolvedCRS().
  auto over_utm =
      TryCreateCrs(db, "+proj=utm +zone=32 +datum=WGS84 +over +type=crs");
  ASSERT_NE(over_utm, nullptr);
  auto wgs84 = epsg->createCoordinateReferenceSystem("4326");
  EXPECT_FALSE(
      factory->createOperations(NN_CHECK_THROW(over_utm), wgs84, op_ctx)
          .empty());

  // 2. Canonical BoundCRS unwrapping in
  // CoordinateOperationFactory::createOperations().
  auto nad27 = epsg->createCoordinateReferenceSystem("4267");
  auto bound_nad27 = MakeHelmertBound(nad27, wgs84, -8.0, 160.0, 176.0);
  auto canonical_base = bound_nad27->baseCRSWithCanonicalBoundCRS();
  EXPECT_NE(canonical_base->canonicalBoundCRS(), nullptr);
  EXPECT_FALSE(
      factory->createOperations(canonical_base, wgs84, op_ctx).empty());

  // 3. CompoundCRS named lookup in getResolvedCRS:
  //    a) Without geoid model (resolves to DB compound CRS).
  auto rd_new_noid =
      TryCreateCrs(db, epsg->createProjectedCRS("28992")->exportToPROJString(
                           pio::PROJStringFormatter::create().get()) +
                           " +type=crs");
  auto nap_m = MakeVertCrs("NAP height", "Normaal Amsterdams Peil");
  auto amersfoort_rd = epsg->createProjectedCRS("28992");
  auto rd_nap_named = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Amersfoort / RD New + NAP height"),
      {amersfoort_rd, nap_m});
  EXPECT_FALSE(factory->createOperations(rd_nap_named, wgs84, op_ctx).empty());

  //    b) With explicit geoidModel attached -> keeps custom CompoundCRS and
  //    computes component extent intersection instead.
  auto model_egm = MakeGeoidModel("EGM96", nap_m, wgs84.as_nullable());
  auto nap_with_geoid =
      MakeVertCrs("NAP height", "Normaal Amsterdams Peil",
                  pcommon::UnitOfMeasure::METRE, false, model_egm);
  auto rd_nap_geoid = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Amersfoort / RD New + NAP height"),
      {amersfoort_rd, nap_with_geoid});
  EXPECT_FALSE(factory->createOperations(rd_nap_geoid, wgs84, op_ctx).empty());
  (void)rd_new_noid;

  // 4. GeographicCRS, ProjectedCRS, and CompoundCRS with explicit EPSG IDs but
  //    no domain of validity (extentOut == nullptr / approxExtent), plus an
  //    invalid CompoundCRS EPSG code hitting the exception catch block.
  auto wgs84_datum = pdatum::GeodeticReferenceFrame::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "World Geodetic System 1984"),
      pdatum::Ellipsoid::WGS84, osgeo::proj::util::optional<std::string>(),
      pdatum::PrimeMeridian::GREENWICH);
  auto geog_id_no_ext = pcrs::GeographicCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "WGS 84")
          .set(pmeta::Identifier::CODESPACE_KEY, "EPSG")
          .set(pmeta::Identifier::CODE_KEY, "4326"),
      wgs84_datum,
      pcs::EllipsoidalCS::createLatitudeLongitude(
          pcommon::UnitOfMeasure::DEGREE));
  EXPECT_FALSE(
      factory->createOperations(geog_id_no_ext, wgs84, op_ctx).empty());

  auto utm32_conv = pop::Conversion::createUTM(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "UTM zone 32N"),
      32, true);
  auto proj_id_no_ext = pcrs::ProjectedCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "WGS 84 / UTM zone 32N")
          .set(pmeta::Identifier::CODESPACE_KEY, "EPSG")
          .set(pmeta::Identifier::CODE_KEY, "32632"),
      geog_id_no_ext, utm32_conv,
      pcs::CartesianCS::createEastingNorthing(pcommon::UnitOfMeasure::METRE));
  EXPECT_FALSE(
      factory->createOperations(proj_id_no_ext, wgs84, op_ctx).empty());

  auto nap_no_ext = MakeVertCrs("NAP height", "Normaal Amsterdams Peil",
                                pcommon::UnitOfMeasure::METRE, false, nullptr,
                                /*extent=*/nullptr);
  auto comp_id_valid = pcrs::CompoundCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY,
               "Amersfoort / RD New + NAP height")
          .set(pmeta::Identifier::CODESPACE_KEY, "EPSG")
          .set(pmeta::Identifier::CODE_KEY, "7415"),
      {amersfoort_rd, nap_no_ext});
  EXPECT_FALSE(factory->createOperations(comp_id_valid, wgs84, op_ctx).empty());

  auto comp_id_invalid = pcrs::CompoundCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY,
               "Amersfoort / RD New + NAP height")
          .set(pmeta::Identifier::CODESPACE_KEY, "EPSG")
          .set(pmeta::Identifier::CODE_KEY, "9999999"),
      {amersfoort_rd, nap_no_ext});
  EXPECT_FALSE(
      factory->createOperations(comp_id_invalid, wgs84, op_ctx).empty());
}

TEST(CoordinateOperationFactoryTest,
     CreateOperationTwoArgAndFilterSourceTargetExtentBothAndIntersection) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();

  // 1. Two-argument createOperation(sourceCRS, targetCRS) returning non-null
  //    for equivalent EngineeringCRSs and nullptr for distinct EngineeringCRSs.
  auto eng1 = pcrs::EngineeringCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Eng1"),
      pdatum::EngineeringDatum::create(
          putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "ED1")),
      pcs::CartesianCS::createEastingNorthing(pcommon::UnitOfMeasure::METRE));
  auto eng2 = pcrs::EngineeringCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "Eng2"),
      pdatum::EngineeringDatum::create(
          putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "ED2")),
      pcs::CartesianCS::createEastingNorthing(pcommon::UnitOfMeasure::METRE));
  EXPECT_NE(factory->createOperation(eng1, eng1), nullptr);
  EXPECT_EQ(factory->createOperation(eng1, eng2), nullptr);

  auto ed50 = epsg->createCoordinateReferenceSystem("4230");
  auto wgs84 = epsg->createCoordinateReferenceSystem("4326");
  auto custom_no_ext = MakeGeog2D(
      "CustomNoExtent", "CustomDatumNoExt", pdatum::Ellipsoid::WGS84,
      pdatum::PrimeMeridian::GREENWICH, pcommon::UnitOfMeasure::DEGREE, false,
      /*extent=*/nullptr);

  // 2. SourceTargetCRSExtentUse::INTERSECTION without user-specified AOI:
  //    a) both extents non-null, b) only extent1 non-null, c) only extent2.
  {
    auto c_inter = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
    c_inter->setSourceAndTargetCRSExtentUse(
        pop::CoordinateOperationContext::SourceTargetCRSExtentUse::
            INTERSECTION);
    EXPECT_FALSE(factory->createOperations(ed50, wgs84, c_inter).empty());
    EXPECT_FALSE(
        factory->createOperations(ed50, custom_no_ext, c_inter).empty());
    EXPECT_FALSE(
        factory->createOperations(custom_no_ext, ed50, c_inter).empty());
  }

  // 3. SourceTargetCRSExtentUse::BOTH with STRICT_CONTAINMENT and
  //    PARTIAL_INTERSECTION (both extents non-null, one extent null, and
  //    disjoint extents).
  for (auto crit :
       {pop::CoordinateOperationContext::SpatialCriterion::STRICT_CONTAINMENT,
        pop::CoordinateOperationContext::SpatialCriterion::
            PARTIAL_INTERSECTION}) {
    auto c_both = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
    c_both->setSourceAndTargetCRSExtentUse(
        pop::CoordinateOperationContext::SourceTargetCRSExtentUse::BOTH);
    c_both->setSpatialCriterion(crit);
    EXPECT_FALSE(factory->createOperations(ed50, wgs84, c_both).empty());
    EXPECT_FALSE(
        factory->createOperations(ed50, custom_no_ext, c_both).empty());
    EXPECT_FALSE(
        factory->createOperations(custom_no_ext, ed50, c_both).empty());

    // BoundCRS whose transformation has no domainOfValidity (extent == nullptr)
    // and base/target CRSs have non-null extents.
    auto transf_no_ext = pop::Transformation::createGeocentricTranslations(
        putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                 "HelmertNoExtent"),
        ed50, wgs84, -87.0, -98.0, -121.0,
        {pmeta::PositionalAccuracy::create("5.0")});
    auto bound_no_ext = pcrs::BoundCRS::create(ed50, wgs84, transf_no_ext);
    (void)factory->createOperations(bound_no_ext, wgs84, c_both);
  }

  // 4. User-specified AreaOfInterest matching the exact description of one of
  //    the ED50 -> WGS 84 operations so other operations hit the description
  //    mismatch continue branch.
  auto c_all = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  c_all->setSpatialCriterion(
      pop::CoordinateOperationContext::SpatialCriterion::PARTIAL_INTERSECTION);
  auto ops_ed50 = factory->createOperations(ed50, wgs84, c_all);
  for (const auto& op : ops_ed50) {
    if (!op->domains().empty() &&
        op->domains().front()->domainOfValidity() != nullptr &&
        op->domains().front()->domainOfValidity()->description().has_value()) {
      auto exact_aoi = op->domains().front()->domainOfValidity();
      auto c_desc = pop::CoordinateOperationContext::create(af, exact_aoi, 0.0);
      c_desc->setSpatialCriterion(pop::CoordinateOperationContext::
                                      SpatialCriterion::STRICT_CONTAINMENT);
      auto ops_desc = factory->createOperations(ed50, wgs84, c_desc);
      EXPECT_FALSE(ops_desc.empty());
      break;
    }
  }
}

TEST(CoordinateOperationFactoryTest,
     SortFunctionComparisonsAndUnexportableOperations) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  op_ctx->setSpatialCriterion(
      pop::CoordinateOperationContext::SpatialCriterion::PARTIAL_INTERSECTION);
  op_ctx->setGridAvailabilityUse(
      pop::CoordinateOperationContext::GridAvailabilityUse::USE_FOR_SORTING);

  // 1. NTF (Paris) (EPSG:4807) -> NTF (EPSG:4275) and ETRS89-FRA [RGF93 v1]
  //    (EPSG:9777): exercises same-length "NTF (Paris) to ... (1)" vs "(2)"
  //    and "ETRS89-" name comparisons in SortFunction.
  auto ntf_paris = epsg->createCoordinateReferenceSystem("4807");
  auto ntf = epsg->createCoordinateReferenceSystem("4275");
  EXPECT_FALSE(factory->createOperations(ntf_paris, ntf, op_ctx).empty());
  if (auto etrs_fra = TryCreateCrs(db, "EPSG:9777")) {
    EXPECT_FALSE(
        factory->createOperations(ntf_paris, NN_CHECK_THROW(etrs_fra), op_ctx)
            .empty());
    if (auto etrs_bel = TryCreateCrs(db, "EPSG:9758")) {
      auto c_interm = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
      c_interm->setAllowUseIntermediateCRS(
          pop::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
      c_interm->setSpatialCriterion(pop::CoordinateOperationContext::
                                        SpatialCriterion::PARTIAL_INTERSECTION);
      EXPECT_FALSE(factory
                       ->createOperations(NN_CHECK_THROW(etrs_fra),
                                          NN_CHECK_THROW(etrs_bel), c_interm)
                       .empty());
    }
  }
}

TEST(CoordinateOperationFactoryTest,
     UncodedGeodeticCrsWithIntermediateAndDatumEnsemblePivots) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();

  // 1. Un-coded GeographicCRS sharing OSGB36 datum (EPSG:6277) to ED50
  //    (EPSG:4230) with an explicit intermediate CRS (EPSG:4326): triggers
  //    findsOpsInRegistryWithIntermediate with sourceIds.empty() and
  //    non-empty opsWithIntermediate (plus reverse with targetIds.empty()).
  auto osgb_datum = epsg->createGeodeticDatum("6277");
  auto cs_latlon = pcs::EllipsoidalCS::createLatitudeLongitude(
      pcommon::UnitOfMeasure::DEGREE);
  auto custom_osgb = pcrs::GeographicCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "Uncoded OSGB36")
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      osgb_datum, cs_latlon);
  auto ed50 = epsg->createCoordinateReferenceSystem("4230");

  auto c_interm = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  c_interm->setAllowUseIntermediateCRS(
      pop::CoordinateOperationContext::IntermediateCRSUse::ALWAYS);
  c_interm->setIntermediateCRS({{"EPSG", "4326"}});
  c_interm->setSpatialCriterion(
      pop::CoordinateOperationContext::SpatialCriterion::PARTIAL_INTERSECTION);

  auto ops_src_uncoded = factory->createOperations(custom_osgb, ed50, c_interm);
  EXPECT_FALSE(ops_src_uncoded.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_src_uncoded);

  auto ops_dst_uncoded = factory->createOperations(ed50, custom_osgb, c_interm);
  EXPECT_FALSE(ops_dst_uncoded.empty());

  // 2. 3D un-coded GeographicCRSs on OSGB36 (EPSG:6277) and DHDN (EPSG:6314):
  //    neither has a 3D GeographicCRS in EPSG, so
  //    createOperationsWithDatumPivot promotes the 2D ConcatenatedOperation
  //    (4277 -> 4326 -> 4314) to 3D.
  auto dhdn_datum = epsg->createGeodeticDatum("6314");
  auto cs_3d = pcs::EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
      pcommon::UnitOfMeasure::DEGREE, pcommon::UnitOfMeasure::METRE);
  auto custom_osgb_3d = pcrs::GeographicCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "Uncoded OSGB36 3D")
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      osgb_datum, cs_3d);
  auto custom_dhdn_3d = pcrs::GeographicCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "Uncoded DHDN 3D")
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      dhdn_datum, cs_3d);
  auto c_3d = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  c_3d->setSpatialCriterion(
      pop::CoordinateOperationContext::SpatialCriterion::PARTIAL_INTERSECTION);
  auto ops_3d_pivot =
      factory->createOperations(custom_osgb_3d, custom_dhdn_3d, c_3d);
  EXPECT_FALSE(ops_3d_pivot.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_3d_pivot);

  // 3. Unknown target datum in createOperationsWithDatumPivot (line 3208).
  auto unk_target =
      MakeGeog2D("unk_target", "unknown", pdatum::Ellipsoid::WGS84);
  EXPECT_FALSE(
      factory->createOperations(custom_osgb, unk_target, c_3d).empty());

  // 4. Datum pivot with coordinate epochs on NAD83(CSRS)v7 datum (EPSG:6140,
  //    which has PointMotionOperations in proj.db) as source and as target.
  if (auto csrs7_datum = epsg->createGeodeticDatum("6140");
      csrs7_datum.as_nullable() != nullptr) {
    auto custom_csrs7 = pcrs::GeographicCRS::create(
        putil::PropertyMap()
            .set(pcommon::IdentifiedObject::NAME_KEY, "Uncoded CSRSv7")
            .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
                 pmeta::Extent::WORLD),
        csrs7_datum, cs_latlon);
    auto itrf2014 = epsg->createCoordinateReferenceSystem("9000");
    auto c_ep = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
    c_ep->setSpatialCriterion(pop::CoordinateOperationContext::
                                  SpatialCriterion::PARTIAL_INTERSECTION);
    c_ep->setSourceCoordinateEpoch(
        putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
            pcommon::Measure(2010.0, pcommon::UnitOfMeasure::YEAR))));
    c_ep->setTargetCoordinateEpoch(
        putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
            pcommon::Measure(2020.0, pcommon::UnitOfMeasure::YEAR))));
    EXPECT_FALSE(
        factory->createOperations(custom_csrs7, itrf2014, c_ep).empty());
    EXPECT_FALSE(
        factory->createOperations(itrf2014, custom_csrs7, c_ep).empty());
  }

  // 5. ETRS89 datum ensemble to Pulkovo 1942(58) (2D and promoted 3D) and
  //    Amersfoort -> ETRF2000 (reverse ensemble null op check) and
  //    ETRS89 -> ETRS89-NOR [EUREF89].
  auto etrs89_2d = epsg->createCoordinateReferenceSystem("4258");
  auto etrs89_3d = epsg->createCoordinateReferenceSystem("4937");
  auto pulkovo58_2d = epsg->createCoordinateReferenceSystem("4179");
  auto pulkovo58_3d =
      pulkovo58_2d->promoteTo3D("Pulkovo 1942(58) 3D", db.as_nullable());
  EXPECT_FALSE(
      factory->createOperations(etrs89_2d, pulkovo58_2d, c_3d).empty());
  EXPECT_FALSE(
      factory->createOperations(pulkovo58_2d, etrs89_2d, c_3d).empty());
  EXPECT_FALSE(
      factory->createOperations(etrs89_3d, pulkovo58_3d, c_3d).empty());

  auto c_aoi =
      pop::CoordinateOperationContext::create(af, pmeta::Extent::WORLD, 0.0);
  c_aoi->setSpatialCriterion(
      pop::CoordinateOperationContext::SpatialCriterion::PARTIAL_INTERSECTION);
  EXPECT_FALSE(
      factory->createOperations(etrs89_2d, pulkovo58_2d, c_aoi).empty());

  auto amersfoort = epsg->createCoordinateReferenceSystem("4289");
  auto etrf2000 = epsg->createCoordinateReferenceSystem("9067");
  EXPECT_FALSE(factory->createOperations(amersfoort, etrf2000, c_3d).empty());

  if (auto etrs_bel = TryCreateCrs(db, "EPSG:9758")) {
    EXPECT_FALSE(
        factory->createOperations(etrs89_2d, NN_CHECK_THROW(etrs_bel), c_3d)
            .empty());
  }

  // 6. hasTooSmallAreas with explicit AreaOfInterest and with one-sided null
  //    CRS extent.
  auto itrf2020 = TryCreateCrs(db, "EPSG:9989");
  if (itrf2020) {
    EXPECT_FALSE(
        factory->createOperations(NN_CHECK_THROW(itrf2020), etrs89_3d, c_aoi)
            .empty());
  }
}

TEST(CoordinateOperationFactoryTest,
     HorizNullVerticalAndInverseHorizVerticalHorizProjBased) {
  auto ctx = MakeContext();
  InMemoryFileSystem fs;
  InstallTestGtxGrid(&fs, "/mem/ed50_geoid.gtx", 12.0f);
  ASSERT_TRUE(fs.Install(ctx.get()));
  absl::Cleanup cleanup = [] { proj_cleanup(); };

  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  op_ctx->setSpatialCriterion(
      pop::CoordinateOperationContext::SpatialCriterion::PARTIAL_INTERSECTION);

  // 1. Compound[EPSG:3912, CustomVert1] -> Compound[EPSG:3794, CustomVert2]:
  //    CustomVert1 and CustomVert2 have only a ballpark vertical transformation
  //    and no geoid models, while EPSG:3912 -> EPSG:3794 has a direct
  //    ProjectedCRS-to-ProjectedCRS operation in proj.db. This triggers
  //    createHorizNullVerticalPROJBased and
  //    MyPROJStringExportableHorizNullVertical.
  auto si_proj1 = epsg->createCoordinateReferenceSystem("3912");
  auto si_proj2 = epsg->createCoordinateReferenceSystem("3794");
  auto custom_v1 = MakeVertCrs("CustomSIVert1", "CustomSIVDatum1");
  auto custom_v2 = MakeVertCrs("CustomSIVert2", "CustomSIVDatum2");
  auto comp_si1 = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "SI1 + CustomV1"),
      {si_proj1, custom_v1});
  auto comp_si2 = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "SI2 + CustomV2"),
      {si_proj2, custom_v2});
  std::vector<std::string> si_proj_strings;
  auto ops_si = factory->createOperations(comp_si1, comp_si2, op_ctx);
  EXPECT_FALSE(ops_si.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_si, &si_proj_strings);
  EXPECT_FALSE(si_proj_strings.empty());

  // 2. Compound[WGS84, Vert1] -> Compound[WGS84, Vert2] where Vert1 -> Vert2
  //    is a vertical grid transformation with interpolationCRS = ED50
  //    (EPSG:4230). Because WGS 84 -> ED50 in EPSG is "Inverse of ED50 to
  //    WGS 84 ...", opSrcCRSToGeogCRS starts with "Inverse of" and is the
  //    inverse of opGeogCRStoDstCRS (ED50 -> WGS 84), hitting
  //    createHorizVerticalHorizPROJBased line 2839 and
  //    MyPROJStringExportableHorizVerticalHorizPROJBased lines 2460-2465.
  auto wgs84_2d = epsg->createGeographicCRS("4326");
  auto ed50_2d = epsg->createGeographicCRS("4230");
  auto v_src = MakeVertCrs("ED50InterpV1", "ED50InterpVD1");
  auto v_dst = MakeVertCrs("ED50InterpV2", "ED50InterpVD2");
  auto v_grid_op = pop::Transformation::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "V1 to V2 via ED50")
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      v_src, v_dst, ed50_2d.as_nullable(),
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, EPSG_NAME_METHOD_VERTCON)
          .set(pmeta::Identifier::CODESPACE_KEY, "EPSG")
          .set(pmeta::Identifier::CODE_KEY, EPSG_CODE_METHOD_VERTCON),
      {pop::OperationParameter::create(
          putil::PropertyMap()
              .set(pcommon::IdentifiedObject::NAME_KEY,
                   EPSG_NAME_PARAMETER_VERTICAL_OFFSET_FILE)
              .set(pmeta::Identifier::CODESPACE_KEY, "EPSG")
              .set(pmeta::Identifier::CODE_KEY,
                   EPSG_CODE_PARAMETER_VERTICAL_OFFSET_FILE))},
      {pop::ParameterValue::createFilename("/mem/ed50_geoid.gtx")},
      {pmeta::PositionalAccuracy::create("0.05")});
  auto bound_v_src = pcrs::BoundCRS::create(v_src, v_dst, v_grid_op);
  auto comp_wgs_v1 = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "WGS84 + BoundV1"),
      {wgs84_2d, bound_v_src});
  auto comp_wgs_v2 = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "WGS84 + V2"),
      {wgs84_2d, v_dst});
  auto ops_hvh = factory->createOperations(comp_wgs_v1, comp_wgs_v2, op_ctx);
  EXPECT_FALSE(ops_hvh.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_hvh);

  // Also test Compound[WGS84, BoundV1] -> WGS 84 3D where v_dst has a geoid
  // model so getInterpolationGeogCRS sees an explicit interpolationCRS.
  auto wgs84_3d = epsg->createGeographicCRS("4979");
  auto v_geoid_ed50 =
      pop::Transformation::createGravityRelatedHeightToGeographic3D(
          putil::PropertyMap()
              .set(pcommon::IdentifiedObject::NAME_KEY, "V1 to WGS84 via ED50")
              .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
                   pmeta::Extent::WORLD),
          v_src, wgs84_3d, ed50_2d.as_nullable(), "/mem/ed50_geoid.gtx",
          {pmeta::PositionalAccuracy::create("0.05")});
  auto bound_v_to_wgs3d = pcrs::BoundCRS::create(v_src, wgs84_3d, v_geoid_ed50);
  auto comp_wgs_v3d = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "WGS84 + BoundVTo3D"),
      {wgs84_2d, bound_v_to_wgs3d});
  auto ops_to_3d = factory->createOperations(comp_wgs_v3d, wgs84_3d, op_ctx);
  EXPECT_FALSE(ops_to_3d.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_to_3d);
}

TEST(CoordinateOperationFactoryTest,
     GeogToVertKnownGridMetadataAndVertToVertStrategy2AndFootGeog3D) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  op_ctx->setSpatialCriterion(
      pop::CoordinateOperationContext::SpatialCriterion::PARTIAL_INTERSECTION);

  // 1. GEOID_MODEL with PROJknown grid name (e.g. "PROJ egm96_15.gtx"), empty
  //    accuracies, and nullptr extent on a VerticalCRS sharing EGM96 datum
  //    (EPSG:5171): exercises getTransformationsForGridName, matching datum
  //    filtering, accuracy/extent lookup from DB, and modelIds lookup
  //    (EPSG:10084 "WGS 84 to EGM96 height (1)").
  auto egm96_crs = epsg->createVerticalCRS("5773");
  auto egm96_datum = egm96_crs->datum();
  ASSERT_NE(egm96_datum, nullptr);
  auto axis_up_m = pcs::CoordinateSystemAxis::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Gravity-related height"),
      "H", pcs::AxisDirection::UP, pcommon::UnitOfMeasure::METRE);
  auto vcs_m = pcs::VerticalCS::create(putil::PropertyMap(), axis_up_m);
  auto base_egm_vert = pcrs::VerticalCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Custom EGM96 height"),
      NN_CHECK_THROW(egm96_datum), vcs_m);

  auto method_empty = pop::OperationMethod::create(
      putil::PropertyMap(), std::vector<pop::OperationParameterNNPtr>());
  // Model A: "PROJ egm96_15.gtx" with empty accuracies and no extent.
  auto model_known_grid = pop::Transformation::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "PROJ egm96_15.gtx"),
      base_egm_vert, pcrs::GeographicCRS::EPSG_4979, nullptr, method_empty, {},
      /*accuracies=*/{});
  // Model B: explicit EPSG:10084 identifier ("WGS 84 to EGM96 height (1)").
  auto model_epsg_10084 = pop::Transformation::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "EGM96")
          .set(pmeta::Identifier::CODESPACE_KEY, "EPSG")
          .set(pmeta::Identifier::CODE_KEY, "10084"),
      base_egm_vert, pcrs::GeographicCRS::EPSG_4979, nullptr, method_empty, {},
      /*accuracies=*/{});

  for (const auto& model : {model_known_grid, model_epsg_10084}) {
    auto vert_with_model = pcrs::VerticalCRS::create(
        putil::PropertyMap()
            .set(pcommon::IdentifiedObject::NAME_KEY, "Custom EGM96 height")
            .set("GEOID_MODEL", model),
        NN_CHECK_THROW(egm96_datum), vcs_m);
    auto ops_gv = factory->createOperations(pcrs::GeographicCRS::EPSG_4979,
                                            vert_with_model, op_ctx);
    EXPECT_FALSE(ops_gv.empty());
    ExportOpsToProj5Strings(db.as_nullable(), ops_gv);
  }

  // 2. Strategy 2 in createOperationsVertToVertWithIntermediateVert:
  //    Baltic 1977 depth (EPSG:5612) -> Caspian height (EPSG:5611).
  //    Strategy 1 (pivoting on Caspian datum candidates) finds no direct op
  //    from 5612, so Strategy 2 pivots through Baltic 1977 height (EPSG:5705).
  auto baltic_depth = epsg->createCoordinateReferenceSystem("5612");
  auto caspian_height = epsg->createCoordinateReferenceSystem("5611");
  auto ops_baltic_caspian =
      factory->createOperations(baltic_depth, caspian_height, op_ctx);
  EXPECT_FALSE(ops_baltic_caspian.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_baltic_caspian);

  // 3. 3D GeographicCRS with non-metre vertical unit (US survey foot) on a
  //    datum (WGS 84, EPSG:6326) that has NO direct 2D transformation to
  //    NAVD88 height (EPSG:5703), so geog3DToVertTryThroughGeog2D returns empty
  //    and createOperationsGeogToVertWithAlternativeGeog executes lines
  //    5110-5130.
  auto wgs84_datum = epsg->createGeodeticDatum("6326");
  auto cs_3d_ft = pcs::EllipsoidalCS::createLatitudeLongitudeEllipsoidalHeight(
      pcommon::UnitOfMeasure::DEGREE, pcommon::UnitOfMeasure::US_FOOT);
  auto wgs84_3d_ft = pcrs::GeographicCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "WGS 84 3D (ftUS)")
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      wgs84_datum, cs_3d_ft);
  auto navd88_m = epsg->createCoordinateReferenceSystem("5703");
  op_ctx->setAllowUseIntermediateCRS(
      pop::CoordinateOperationContext::IntermediateCRSUse::NEVER);
  auto ops_ft_vert = factory->createOperations(wgs84_3d_ft, navd88_m, op_ctx);
  EXPECT_FALSE(ops_ft_vert.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_ft_vert);
}

TEST(CoordinateOperationFactoryTest,
     BoundToGeogDerivedAndNonEquivalentCsAndBoundToBoundSameVertDatum) {
  auto ctx = MakeContext();
  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);

  auto ed50_latlon = epsg->createGeographicCRS("4230");
  auto ed50_lonlat =
      MakeGeog2D("ED50 (lon-lat)", "European Datum 1950",
                 ed50_latlon->ellipsoid(), pdatum::PrimeMeridian::GREENWICH,
                 pcommon::UnitOfMeasure::DEGREE, /*lon_lat=*/true);
  auto wgs84_latlon = epsg->createGeographicCRS("4326");
  auto wgs84_lonlat =
      MakeGeog2D("WGS 84 (lon-lat)", "World Geodetic System 1984",
                 pdatum::Ellipsoid::WGS84, pdatum::PrimeMeridian::GREENWICH,
                 pcommon::UnitOfMeasure::DEGREE, /*lon_lat=*/true);

  // 1. BoundCRS where baseCRS (or ProjectedCRS's base GeographicCRS) is
  //    ed50_lonlat, while transformation()->sourceCRS() is ed50_latlon (same
  //    datum, different axis order so !_isEquivalentTo is true!):
  //    a) targetCRS == hubCRS (wgs84_latlon) -> hits lines 5637-5656 &
  //    5673-5674 b) targetCRS == wgs84_lonlat (same datum as hubCRS, different
  //    axis order)
  //       -> hits lines 5691-5731!
  auto helmert_latlon = pop::Transformation::createGeocentricTranslations(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "ED50 to WGS84")
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      ed50_latlon, wgs84_latlon, -87.0, -98.0, -121.0,
      {pmeta::PositionalAccuracy::create("2.0")});
  auto bound_geog_mismatch =
      pcrs::BoundCRS::create(ed50_lonlat, wgs84_latlon, helmert_latlon);
  EXPECT_FALSE(
      factory->createOperations(bound_geog_mismatch, wgs84_latlon, op_ctx)
          .empty());
  EXPECT_FALSE(
      factory->createOperations(bound_geog_mismatch, wgs84_lonlat, op_ctx)
          .empty());

  auto utm_conv = pop::Conversion::createUTM(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "UTM 31N"),
      31, true);
  auto proj_on_lonlat = pcrs::ProjectedCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "ED50 lonlat / UTM 31N")
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      ed50_lonlat, utm_conv,
      pcs::CartesianCS::createEastingNorthing(pcommon::UnitOfMeasure::METRE));
  auto bound_proj_mismatch =
      pcrs::BoundCRS::create(proj_on_lonlat, wgs84_latlon, helmert_latlon);
  EXPECT_FALSE(
      factory->createOperations(bound_proj_mismatch, wgs84_latlon, op_ctx)
          .empty());
  EXPECT_FALSE(
      factory->createOperations(bound_proj_mismatch, wgs84_lonlat, op_ctx)
          .empty());

  // 2. BoundCRS whose baseCRS is a DerivedGeographicCRS (lines 5596-5601).
  auto rot_conv = pop::Conversion::createPoleRotationGRIBConvention(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "PoleRot"),
      pcommon::Angle(10.0), pcommon::Angle(20.0), pcommon::Angle(0.0));
  auto derived_geog = pcrs::DerivedGeographicCRS::create(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "Derived ED50")
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      ed50_latlon, rot_conv,
      pcs::EllipsoidalCS::createLatitudeLongitude(
          pcommon::UnitOfMeasure::DEGREE));
  auto bound_derived_geog =
      pcrs::BoundCRS::create(derived_geog, wgs84_latlon, helmert_latlon);
  EXPECT_FALSE(
      factory->createOperations(bound_derived_geog, wgs84_latlon, op_ctx)
          .empty());

  // 3. BoundCRS[VerticalCRS, VerticalCRS] to hub VerticalCRS (hits lines
  //    5970-5973) and to GeographicCRS (hits fallback lines 5950-5952).
  auto v1_m =
      MakeVertCrs("BV1_m", "SharedBoundVDatum", pcommon::UnitOfMeasure::METRE);
  auto v1_ft =
      MakeVertCrs("BV1_ft", "SharedBoundVDatum", pcommon::UnitOfMeasure::FOOT);
  auto v2_m =
      MakeVertCrs("BV2_m", "OtherBoundVDatum", pcommon::UnitOfMeasure::METRE);
  auto v_offset = pop::Transformation::createVerticalOffset(
      putil::PropertyMap()
          .set(pcommon::IdentifiedObject::NAME_KEY, "V1 to V2")
          .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
               pmeta::Extent::WORLD),
      v1_m, v2_m, pcommon::Length(1.5),
      {pmeta::PositionalAccuracy::create("0.1")});
  auto bound_v1_to_v2 = pcrs::BoundCRS::create(v1_m, v2_m, v_offset);
  EXPECT_FALSE(factory->createOperations(bound_v1_to_v2, v2_m, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(bound_v1_to_v2, wgs84_latlon, op_ctx).empty());

  // 4. BoundToBound where both baseCRSs are VerticalCRSs sharing the same
  //    vertical datum, but the two BoundCRSs have DIFFERENT hubCRSs so line
  //    6220 is false and lines 6254-6270 execute.
  auto bound_v1_ft_to_wgs =
      pcrs::BoundCRS::create(v1_ft, wgs84_latlon, v_offset);
  auto ops_b2b_same_vd =
      factory->createOperations(bound_v1_to_v2, bound_v1_ft_to_wgs, op_ctx);
  EXPECT_FALSE(ops_b2b_same_vd.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_b2b_same_vd);
}

TEST(CoordinateOperationFactoryTest,
     CompoundToGeogDiffDatumBoundsAndPng94Kumul34AndCompoundToCompoundPaths) {
  auto ctx = MakeContext();
  InMemoryFileSystem fs;
  InstallTestGtxGrid(&fs, "/mem/nad83_geoid.gtx", -25.0f);
  InstallTestGtxGrid(&fs, "/mem/amers_geoid.gtx", 40.0f);
  InstallTestGtxGrid(&fs, "/mem/wgs_geoid.gtx", 30.0f);
  ASSERT_TRUE(fs.Install(ctx.get()));
  absl::Cleanup cleanup = [] { proj_cleanup(); };

  auto db = pio::DatabaseContext::create(std::string(), {}, ctx.get());
  auto af = pio::AuthorityFactory::create(db, std::string());
  auto epsg = pio::AuthorityFactory::create(db, "EPSG");
  auto factory = pop::CoordinateOperationFactory::create();
  auto op_ctx = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
  op_ctx->setSpatialCriterion(
      pop::CoordinateOperationContext::SpatialCriterion::PARTIAL_INTERSECTION);

  // 1. Compound[Bound[NAD83 2D -> WGS84 2D], Bound[NAVD88 -> NAD83 3D]]
  //    transformed to WGS 84 3D (EPSG:4979, different datum from NAD83!):
  //    hits lines 6637-6684 in createOperationsCompoundToGeog!
  auto nad83_2d = epsg->createGeographicCRS("4269");
  auto nad83_3d = nad83_2d->promoteTo3D("NAD83 3D", db.as_nullable());
  auto wgs84_2d = epsg->createGeographicCRS("4326");
  auto wgs84_3d = epsg->createGeographicCRS("4979");
  auto navd88_base = epsg->createVerticalCRS("5703");
  auto bound_nad83_2d = MakeHelmertBound(nad83_2d, wgs84_2d, 1.0, -1.0, 0.5);
  auto v_transf_nad83 =
      pop::Transformation::createGravityRelatedHeightToGeographic3D(
          putil::PropertyMap()
              .set(pcommon::IdentifiedObject::NAME_KEY, "NAVD88 to NAD83 3D")
              .set(pcommon::ObjectUsage::DOMAIN_OF_VALIDITY_KEY,
                   pmeta::Extent::WORLD),
          navd88_base, nad83_3d, nullptr, "/mem/nad83_geoid.gtx",
          {pmeta::PositionalAccuracy::create("0.05")});
  auto bound_vert_nad83 =
      pcrs::BoundCRS::create(navd88_base, nad83_3d, v_transf_nad83);
  auto comp_nad83_bounds = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "BoundNAD83 + BoundNAVD88"),
      {bound_nad83_2d, bound_vert_nad83});

  auto ops_c2g_diff_datum =
      factory->createOperations(comp_nad83_bounds, wgs84_3d, op_ctx);
  EXPECT_FALSE(ops_c2g_diff_datum.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_c2g_diff_datum);

  // 2. EPSG:5550+7651 ("PNG94 / PNGMG94 zone 54 + Kumul 34 height") to
  //    EPSG:9754 ("WGS 84 (G2139)" 3D): exercises lines 6855-6917 where
  //    Kumul 34 height transforms to WGS 84 via EGM96 and PNG94 transforms to
  //    WGS 84 (G2139) via WGS 84.
  auto png_comp = TryCreateCrs(db, "EPSG:5550+7651");
  auto wgs_g2139 = TryCreateCrs(db, "EPSG:9754");
  if (png_comp && wgs_g2139) {
    auto ops_png = factory->createOperations(NN_CHECK_THROW(png_comp),
                                             NN_CHECK_THROW(wgs_g2139), op_ctx);
    EXPECT_FALSE(ops_png.empty());
    ExportOpsToProj5Strings(db.as_nullable(), ops_png);
  }

  // 3. CompoundToCompound through intermediate 3D GeogCRS:
  //    a) boundSrcHorizontal != nullptr && boundDstHorizontal == nullptr &&
  //       boundSrcHorizontal->hubCRS() == componentsDst[0] (lines 7425-7446)
  //       and vice versa.
  auto v_wgs_base = MakeVertCrs("WGSGeoidV", "WGSGeoidVD");
  auto model_wgs =
      MakeGeoidModel("/mem/wgs_geoid.gtx", v_wgs_base, wgs84_2d.as_nullable());
  auto v_wgs_geoid =
      MakeVertCrs("WGSGeoidV", "WGSGeoidVD", pcommon::UnitOfMeasure::METRE,
                  false, model_wgs);
  auto comp_bound_src = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "BoundNAD83 + WGSGeoidV"),
      {bound_nad83_2d, v_wgs_geoid});
  auto comp_unb_dst = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "WGS84 + WGSGeoidV"),
      {wgs84_2d, v_wgs_geoid});
  EXPECT_FALSE(
      factory->createOperations(comp_bound_src, comp_unb_dst, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(comp_unb_dst, comp_bound_src, op_ctx).empty());

  //    b) Amersfoort (EPSG:4289, which has an EPSG code in 2D but no EPSG code
  //       when promoted to 3D) + GeoidVert <-> WGS 84 (EPSG:4326) + GeoidVert:
  //       triggers lines 7493-7545 and createOpsInTwoSteps (lines 7547-7646)!
  auto amers_2d = epsg->createGeographicCRS("4289");
  auto v_amers_base = MakeVertCrs("AmersGeoidV", "AmersGeoidVD");
  auto model_amers = MakeGeoidModel("/mem/amers_geoid.gtx", v_amers_base,
                                    amers_2d.as_nullable());
  auto v_amers_geoid =
      MakeVertCrs("AmersGeoidV", "AmersGeoidVD", pcommon::UnitOfMeasure::METRE,
                  false, model_amers);
  auto comp_amers = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Amersfoort + AmersGeoidV"),
      {amers_2d, v_amers_geoid});
  auto ops_amers_to_wgs =
      factory->createOperations(comp_amers, comp_unb_dst, op_ctx);
  EXPECT_FALSE(ops_amers_to_wgs.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_amers_to_wgs);

  auto ops_wgs_to_amers =
      factory->createOperations(comp_unb_dst, comp_amers, op_ctx);
  EXPECT_FALSE(ops_wgs_to_amers.empty());
  ExportOpsToProj5Strings(db.as_nullable(), ops_wgs_to_amers);

  //    c) VerticalCRS with missing geoid grid in CompoundToCompound so
  //       hasKnownGrid returns false and lines 7653-7657 execute.
  op_ctx->setAllowUseIntermediateCRS(
      pop::CoordinateOperationContext::IntermediateCRSUse::NEVER);
  auto v_miss_base1 = MakeVertCrs("MissGeoidV1", "MissGeoidVD1");
  auto model_miss1 = MakeGeoidModel("/mem/nonexistent_geoid1.gtx", v_miss_base1,
                                    wgs84_2d.as_nullable());
  auto v_miss1 = MakeVertCrs("MissGeoidV1", "MissGeoidVD1",
                             pcommon::UnitOfMeasure::METRE, false, model_miss1);
  auto v_miss_base2 = MakeVertCrs("MissGeoidV2", "MissGeoidVD2");
  auto model_miss2 = MakeGeoidModel("/mem/nonexistent_geoid2.gtx", v_miss_base2,
                                    wgs84_2d.as_nullable());
  auto v_miss2 = MakeVertCrs("MissGeoidV2", "MissGeoidVD2",
                             pcommon::UnitOfMeasure::METRE, false, model_miss2);
  auto comp_miss1 = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "W+Miss1"),
      {wgs84_2d, v_miss1});
  auto comp_miss2 = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "W+Miss2"),
      {wgs84_2d, v_miss2});
  EXPECT_FALSE(
      factory->createOperations(comp_miss1, comp_miss2, op_ctx).empty());

  //    d) Compound[Bound[ED50, WGS84], PlainVert1] ->
  //       Compound[Bound[NAD83, WGS84], PlainVert2] and
  //       Compound[WGS84, PlainVert2] (lines 7693-7707).
  auto ed50_2d = epsg->createGeographicCRS("4230");
  auto bound_ed50_2d =
      MakeHelmertBound(ed50_2d, wgs84_2d, -87.0, -98.0, -121.0);
  auto plain_v1 = MakeVertCrs("PlainV1", "PlainVD1");
  auto plain_v2 = MakeVertCrs("PlainV2", "PlainVD2");
  auto comp_b_ed50 = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "BoundED50 + PlainV1"),
      {bound_ed50_2d, plain_v1});
  auto comp_b_nad83 = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "BoundNAD83 + PlainV2"),
      {bound_nad83_2d, plain_v2});
  auto comp_wgs_plain = pcrs::CompoundCRS::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "WGS84 + PlainV2"),
      {wgs84_2d, plain_v2});
  EXPECT_FALSE(
      factory->createOperations(comp_b_ed50, comp_b_nad83, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(comp_b_ed50, comp_wgs_plain, op_ctx).empty());

  // 4. BoundToCompound:
  //    a) boundSrc (Bound[ED50 2D, WGS84 2D]) to comp_bound_src
  //       (Compound[Bound[NAD83 2D, WGS84 2D], v_wgs_geoid]):
  //       opDst->sourceCRS() is 3D WGS84 (equivalent to opSrc->targetCRS()),
  //       hitting lines 7978-7983.
  //    b) boundSrc (2D) to comp_wgs_plain (no BoundCRS component): hits
  //    fallback
  //       lines 8037-8039.
  EXPECT_FALSE(
      factory->createOperations(bound_ed50_2d, comp_bound_src, op_ctx).empty());
  EXPECT_FALSE(
      factory->createOperations(bound_ed50_2d, comp_wgs_plain, op_ctx).empty());

  // 5. NAD83(CSRS)v7 + CGVD28 height (with HT2_1997 / HT2_2002 / HT2_2010
  //    geoid models) <-> NAD83(CSRS)v7 + CGVD2013a(1997|2002|2010) height
  //    at matching epochs: hits lines 7340-7369 and 7714-7747!
  if (auto csrs7_2d = TryCreateCrs(db, "EPSG:8255")) {
    struct EpochCase {
      double epoch;
      const char* ht2_name;
      const char* cgvd2013_name;
    };
    const EpochCase cases[] = {
        {1997.0, "HT2_1997", "CGVD2013a(1997) height"},
        {2002.0, "HT2_2002", "CGVD2013a(2002) height"},
        {2010.0, "HT2_2010", "CGVD2013a(2010) height"},
    };
    for (const auto& tc : cases) {
      auto cgvd28_base = MakeVertCrs("CGVD28 height", "CGVD28");
      auto ht2_model = MakeGeoidModel(tc.ht2_name, cgvd28_base, csrs7_2d);
      auto cgvd28_ht2 =
          MakeVertCrs("CGVD28 height", "CGVD28", pcommon::UnitOfMeasure::METRE,
                      false, ht2_model);
      auto cgvd2013 = MakeVertCrs(tc.cgvd2013_name, tc.cgvd2013_name);
      auto comp_cgvd28 = pcrs::CompoundCRS::create(
          putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                   "CSRSv7 + CGVD28"),
          {NN_CHECK_THROW(csrs7_2d), cgvd28_ht2});
      auto comp_cgvd2013 = pcrs::CompoundCRS::create(
          putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                                   "CSRSv7 + CGVD2013a"),
          {NN_CHECK_THROW(csrs7_2d), cgvd2013});
      auto c_ep = pop::CoordinateOperationContext::create(af, nullptr, 0.0);
      c_ep->setAllowUseIntermediateCRS(
          pop::CoordinateOperationContext::IntermediateCRSUse::NEVER);
      c_ep->setSpatialCriterion(pop::CoordinateOperationContext::
                                    SpatialCriterion::PARTIAL_INTERSECTION);
      c_ep->setSourceCoordinateEpoch(
          putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
              pcommon::Measure(tc.epoch, pcommon::UnitOfMeasure::YEAR))));
      c_ep->setTargetCoordinateEpoch(
          putil::optional<pcommon::DataEpoch>(pcommon::DataEpoch(
              pcommon::Measure(tc.epoch, pcommon::UnitOfMeasure::YEAR))));
      EXPECT_FALSE(
          factory->createOperations(comp_cgvd28, comp_cgvd2013, c_ep).empty());
      EXPECT_FALSE(
          factory->createOperations(comp_cgvd2013, comp_cgvd28, c_ep).empty());
    }
  }
}

}  // namespace
}  // namespace proj
