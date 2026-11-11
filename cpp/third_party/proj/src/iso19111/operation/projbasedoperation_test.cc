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
// Unit tests for PROJBasedOperation
// (proj/src/iso19111/operation/projbasedoperation.cpp). Note: in google3
// default builds, networking is disabled and proj.db is embedded in memory.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/projbasedoperation.cpp

#include <array>
#include <map>
#include <numbers>
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
#include "third_party/proj4/proj/include/proj/crs.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/iso19111/operation/coordinateoperation_internal.hpp"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

namespace pcommon = ::osgeo::proj::common;
namespace pcrs = ::osgeo::proj::crs;
namespace pio = ::osgeo::proj::io;
namespace pmeta = ::osgeo::proj::metadata;
namespace pop = ::osgeo::proj::operation;
namespace putil = ::osgeo::proj::util;

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::NotNull;
using ::testing::SizeIs;
using ::testing::StartsWith;

constexpr double DegToRad(double deg) {
  return deg * (std::numbers::pi / 180.0);
}

// Subclass exposing protected SingleOperation::setParameterValues() so WKT and
// JSON parameter export loops in PROJBasedOperation can be exercised cleanly.
class TestPROJBasedOperation final : public pop::PROJBasedOperation {
 public:
  explicit TestPROJBasedOperation(const pop::OperationMethodNNPtr& method_in)
      : pop::CoordinateOperation(), pop::PROJBasedOperation(method_in) {}

  TestPROJBasedOperation(const TestPROJBasedOperation&) = delete;
  TestPROJBasedOperation& operator=(const TestPROJBasedOperation&) = delete;

  static pop::PROJBasedOperationNNPtr CreateWithParameters(
      const putil::PropertyMap& properties,
      std::vector<pop::GeneralParameterValueNNPtr> params,
      const pop::OperationMethodNNPtr& method_in) {
    auto op = putil::nn_make_shared<TestPROJBasedOperation>(method_in);
    op->assignSelf(op);
    op->setProperties(properties);
    op->setParameterValues(std::move(params));
    return putil::nn_static_pointer_cast<pop::PROJBasedOperation>(op);
  }
};

// Exportable emitting a syntactically invalid nested pipeline so gridsNeeded()
// catches io::ParsingException without create() rejecting the input upfront.
class NestedPipelineExportable final : public pio::IPROJStringExportable {
 public:
  void _exportToPROJString(pio::PROJStringFormatter* formatter) const override {
    formatter->addStep("pipeline");
    formatter->addStep("helmert");
  }
};

TEST(ProjBasedOperationTest,
     CreateFromProjStringSetsDefaultsCustomPropertiesAndCRSs) {
  constexpr char kProj[] = "+proj=helmert +x=10 +y=-20 +z=30";

  // 1. Empty PropertyMap triggers default operation name; null CRSs leave both
  // endpoints null; static Helmert does not require per-coordinate input time.
  auto op_default = pop::PROJBasedOperation::create(putil::PropertyMap(), kProj,
                                                    nullptr, nullptr, {});
  EXPECT_EQ("PROJ-based coordinate operation", op_default->nameStr());
  EXPECT_EQ(std::string("PROJ-based operation method: ") + kProj,
            op_default->method()->nameStr());
  EXPECT_EQ(nullptr, op_default->sourceCRS());
  EXPECT_EQ(nullptr, op_default->targetCRS());
  EXPECT_EQ(nullptr, op_default->interpolationCRS());
  EXPECT_THAT(op_default->coordinateOperationAccuracies(), IsEmpty());
  EXPECT_FALSE(op_default->requiresPerCoordinateInputTime());
  EXPECT_THAT(op_default->parameterValues(), IsEmpty());

  // 2. Public SingleOperation::createPROJBased delegates identically and
  // preserves custom name, non-null source/target CRSs, and accuracies.
  putil::PropertyMap props;
  props.set(pcommon::IdentifiedObject::NAME_KEY, "Custom Helmert");
  auto accuracy = pmeta::PositionalAccuracy::create("2.5");
  auto op_with_crs = pop::SingleOperation::createPROJBased(
      props, "+proj=helmert +x=10 +t_epoch=2020.0",
      pcrs::GeographicCRS::EPSG_4326.as_nullable(),
      pcrs::GeographicCRS::EPSG_4979.as_nullable(), {accuracy});
  EXPECT_EQ("Custom Helmert", op_with_crs->nameStr());
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4326.get(),
            op_with_crs->sourceCRS().get());
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4979.get(),
            op_with_crs->targetCRS().get());
  ASSERT_THAT(op_with_crs->coordinateOperationAccuracies(), SizeIs(1));
  EXPECT_EQ("2.5", op_with_crs->coordinateOperationAccuracies()[0]->value());
  EXPECT_TRUE(op_with_crs->requiresPerCoordinateInputTime());

  // 3. Passing only one CRS skips setCRSs() via (sourceCRS && targetCRS)
  // short-circuiting.
  auto only_source = pop::PROJBasedOperation::create(
      putil::PropertyMap(), kProj, pcrs::GeographicCRS::EPSG_4326.as_nullable(),
      nullptr, {});
  EXPECT_EQ(nullptr, only_source->sourceCRS());
  EXPECT_EQ(nullptr, only_source->targetCRS());

  auto only_target = pop::PROJBasedOperation::create(
      putil::PropertyMap(), kProj, nullptr,
      pcrs::GeographicCRS::EPSG_4979.as_nullable(), {});
  EXPECT_EQ(nullptr, only_target->sourceCRS());
  EXPECT_EQ(nullptr, only_target->targetCRS());
}

TEST(ProjBasedOperationTest,
     CreateFromProjStringThrowsUnsupportedOperationOnInvalidSyntax) {
  constexpr std::string_view kBadProjStrings[] = {
      "+step +proj=pipeline",
      "+proj=pipeline +step +proj=pipeline",
  };
  for (const std::string_view bad : kBadProjStrings) {
    bool threw = false;
    try {
      (void)pop::PROJBasedOperation::create(
          putil::PropertyMap(), std::string(bad), nullptr, nullptr, {});
    } catch (const putil::UnsupportedOperationException& e) {
      threw = true;
      EXPECT_THAT(e.what(), HasSubstr("PROJBasedOperation::create() failed:"));
    }
    EXPECT_TRUE(threw) << "Expected UnsupportedOperationException for: " << bad;
  }
}

TEST(ProjBasedOperationTest,
     CreateFromExportableSetsMethodPrefixInverseFlagInterpolationAndBallpark) {
  auto base_op = pop::SingleOperation::createPROJBased(
      putil::PropertyMap(), "+proj=helmert +x=10 +t_epoch=2020.0", nullptr,
      nullptr);
  auto exportable =
      putil::nn_static_pointer_cast<pio::IPROJStringExportable>(base_op);

  // 1. Forward wrapper with interpolation CRS, accuracy, ballpark flag, and
  // time-epoch requirement.
  putil::PropertyMap props;
  props.set(pcommon::IdentifiedObject::NAME_KEY, "Approximate Forward");
  auto acc = pmeta::PositionalAccuracy::create("4.0");
  auto fwd = pop::PROJBasedOperation::create(
      props, exportable, /*inverse=*/false, pcrs::GeographicCRS::EPSG_4326,
      pcrs::GeographicCRS::EPSG_4979,
      pcrs::GeographicCRS::OGC_CRS84.as_nullable(), {acc},
      /*hasRoughTransformation=*/true);

  EXPECT_EQ("Approximate Forward", fwd->nameStr());
  EXPECT_THAT(fwd->method()->nameStr(),
              StartsWith("PROJ-based operation method (approximate): "));
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4326.get(), fwd->sourceCRS().get());
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4979.get(), fwd->targetCRS().get());
  EXPECT_EQ(pcrs::GeographicCRS::OGC_CRS84.get(),
            fwd->interpolationCRS().get());
  ASSERT_THAT(fwd->coordinateOperationAccuracies(), SizeIs(1));
  EXPECT_EQ("4.0", fwd->coordinateOperationAccuracies()[0]->value());
  EXPECT_TRUE(fwd->hasBallparkTransformation());
  EXPECT_TRUE(fwd->requiresPerCoordinateInputTime());

  auto fmt_fwd = pio::PROJStringFormatter::create();
  EXPECT_EQ("+proj=helmert +x=10 +t_epoch=2020.0",
            fwd->exportToPROJString(fmt_fwd.get()));

  // 2. Inverse wrapper triggers formatter->startInversion() / stopInversion()
  // and default name when NAME_KEY is absent.
  auto static_base = pop::SingleOperation::createPROJBased(
      putil::PropertyMap(), "+proj=helmert +x=10", nullptr, nullptr);
  auto static_exportable =
      putil::nn_static_pointer_cast<pio::IPROJStringExportable>(static_base);
  auto inv = pop::PROJBasedOperation::create(
      putil::PropertyMap(), static_exportable, /*inverse=*/true,
      pcrs::GeographicCRS::EPSG_4326, pcrs::GeographicCRS::EPSG_4979, nullptr,
      {}, /*hasRoughTransformation=*/false);

  EXPECT_EQ("PROJ-based coordinate operation", inv->nameStr());
  EXPECT_EQ(nullptr, inv->interpolationCRS());
  EXPECT_FALSE(inv->hasBallparkTransformation());
  EXPECT_FALSE(inv->requiresPerCoordinateInputTime());

  auto fmt_inv = pio::PROJStringFormatter::create();
  EXPECT_EQ("+proj=pipeline +step +inv +proj=helmert +x=10",
            inv->exportToPROJString(fmt_inv.get()));
}

TEST(ProjBasedOperationTest, InverseBranchesForExportableAndRawProjString) {
  // 1. Exportable path (projStringExportable_ && sourceCRS() && targetCRS()):
  // swaps CRSs and toggles inverse_ while preserving metadata and time flag.
  auto base_op = pop::SingleOperation::createPROJBased(
      putil::PropertyMap(), "+proj=helmert +x=10 +t_epoch=2020.0", nullptr,
      nullptr);
  auto exportable =
      putil::nn_static_pointer_cast<pio::IPROJStringExportable>(base_op);
  auto acc = pmeta::PositionalAccuracy::create("5.0");
  auto wrapped = pop::PROJBasedOperation::create(
      putil::PropertyMap(), exportable, /*inverse=*/false,
      pcrs::GeographicCRS::EPSG_4326, pcrs::GeographicCRS::EPSG_4979,
      pcrs::GeographicCRS::OGC_CRS84.as_nullable(), {acc},
      /*hasRoughTransformation=*/true);

  auto wrapped_inv = wrapped->inverse();
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4979.get(),
            wrapped_inv->sourceCRS().get());
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4326.get(),
            wrapped_inv->targetCRS().get());
  EXPECT_EQ(pcrs::GeographicCRS::OGC_CRS84.get(),
            wrapped_inv->interpolationCRS().get());
  ASSERT_THAT(wrapped_inv->coordinateOperationAccuracies(), SizeIs(1));
  EXPECT_EQ("5.0", wrapped_inv->coordinateOperationAccuracies()[0]->value());
  EXPECT_TRUE(wrapped_inv->hasBallparkTransformation());
  EXPECT_TRUE(wrapped_inv->requiresPerCoordinateInputTime());

  auto fmt_wrapped_inv = pio::PROJStringFormatter::create();
  EXPECT_EQ("+proj=pipeline +step +inv +proj=helmert +x=10 +t_epoch=2020.0",
            wrapped_inv->exportToPROJString(fmt_wrapped_inv.get()));

  auto wrapped_roundtrip = wrapped_inv->inverse();
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4326.get(),
            wrapped_roundtrip->sourceCRS().get());
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4979.get(),
            wrapped_roundtrip->targetCRS().get());
  auto fmt_wrapped_rt = pio::PROJStringFormatter::create();
  EXPECT_EQ("+proj=helmert +x=10 +t_epoch=2020.0",
            wrapped_roundtrip->exportToPROJString(fmt_wrapped_rt.get()));

  // 2. Raw PROJ-string path with non-null source/target CRSs: reverses pipeline
  // steps, swaps CRSs, preserves interpolationCRS()/accuracies/ballpark.
  auto pipeline_op = pop::PROJBasedOperation::create(
      putil::PropertyMap(),
      "+proj=pipeline +step +proj=axisswap +order=2,1 +step +proj=merc "
      "+ellps=WGS84",
      pcrs::GeographicCRS::EPSG_4326.as_nullable(),
      pcrs::GeographicCRS::EPSG_4979.as_nullable(), {acc});

  auto pipeline_inv = pipeline_op->inverse();
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4979.get(),
            pipeline_inv->sourceCRS().get());
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4326.get(),
            pipeline_inv->targetCRS().get());
  EXPECT_FALSE(pipeline_inv->hasBallparkTransformation());
  ASSERT_THAT(pipeline_inv->coordinateOperationAccuracies(), SizeIs(1));
  EXPECT_EQ("5.0", pipeline_inv->coordinateOperationAccuracies()[0]->value());

  auto fmt_pipe_fwd = pio::PROJStringFormatter::create();
  auto fmt_pipe_inv = pio::PROJStringFormatter::create();
  const std::string pipe_fwd_str =
      pipeline_op->exportToPROJString(fmt_pipe_fwd.get());
  const std::string pipe_inv_str =
      pipeline_inv->exportToPROJString(fmt_pipe_inv.get());
  EXPECT_THAT(pipe_inv_str, HasSubstr("+inv +proj=merc"));

  // Execute the exported forward and inverted pipelines with PROJ C API.
  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ScopedPj pj_fwd(proj_create(ctx.get(), pipe_fwd_str.c_str()));
  ScopedPj pj_inv(proj_create(ctx.get(), pipe_inv_str.c_str()));
  ASSERT_THAT(pj_fwd.get(), NotNull());
  ASSERT_THAT(pj_inv.get(), NotNull());

  // Pipeline starts with axisswap +order=2,1 before +proj=merc, so input is
  // (lat_rad, lon_rad).
  constexpr double kLatRad = DegToRad(48.8566);
  constexpr double kLonRad = DegToRad(2.3522);
  const PJ_COORD xy =
      proj_trans(pj_fwd.get(), PJ_FWD, proj_coord(kLatRad, kLonRad, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pj_fwd.get()));
  const PJ_COORD restored = proj_trans(pj_inv.get(), PJ_FWD, xy);
  EXPECT_EQ(0, proj_errno(pj_inv.get()));
  EXPECT_NEAR(kLatRad, restored.lp.lam, 1e-12);
  EXPECT_NEAR(kLonRad, restored.lp.phi, 1e-12);

  // 3. Raw PROJ-string path with null CRSs takes the false branch of
  // `if (sourceCRS() && targetCRS())` in inverse() and preserves time flag.
  auto helmert_op = pop::PROJBasedOperation::create(
      putil::PropertyMap(), "+proj=helmert +x=10 +y=-20 +z=30 +t_epoch=2020.0",
      nullptr, nullptr, {});
  auto helmert_inv = helmert_op->inverse();
  EXPECT_EQ(nullptr, helmert_inv->sourceCRS());
  EXPECT_EQ(nullptr, helmert_inv->targetCRS());
  EXPECT_TRUE(helmert_inv->requiresPerCoordinateInputTime());
  EXPECT_EQ("Inverse of PROJ-based coordinate operation",
            helmert_inv->nameStr());
  EXPECT_EQ("PROJ-based coordinate operation",
            helmert_inv->inverse()->nameStr());

  auto fmt_helm_fwd = pio::PROJStringFormatter::create();
  auto fmt_helm_inv = pio::PROJStringFormatter::create();
  ScopedPj pj_helm_fwd(proj_create(
      ctx.get(), helmert_op->exportToPROJString(fmt_helm_fwd.get()).c_str()));
  ScopedPj pj_helm_inv(proj_create(
      ctx.get(), helmert_inv->exportToPROJString(fmt_helm_inv.get()).c_str()));
  ASSERT_THAT(pj_helm_fwd.get(), NotNull());
  ASSERT_THAT(pj_helm_inv.get(), NotNull());

  const PJ_COORD shifted = proj_trans(
      pj_helm_fwd.get(), PJ_FWD, proj_coord(1000.0, 2000.0, 3000.0, 2025.0));
  EXPECT_EQ(0, proj_errno(pj_helm_fwd.get()));
  EXPECT_NEAR(1010.0, shifted.xyz.x, 1e-9);
  EXPECT_NEAR(1980.0, shifted.xyz.y, 1e-9);
  EXPECT_NEAR(3030.0, shifted.xyz.z, 1e-9);

  const PJ_COORD unshifted = proj_trans(pj_helm_inv.get(), PJ_FWD, shifted);
  EXPECT_EQ(0, proj_errno(pj_helm_inv.get()));
  EXPECT_NEAR(1000.0, unshifted.xyz.x, 1e-9);
  EXPECT_NEAR(2000.0, unshifted.xyz.y, 1e-9);
  EXPECT_NEAR(3000.0, unshifted.xyz.z, 1e-9);
}

TEST(ProjBasedOperationTest,
     ExportToWktHandlesConversionTransformationWkt1ErrorAndParams) {
  // 1. With sourceCRS and targetCRS: delegates to exportTransformationToWKT()
  // and round-trips through WKTParser into PROJBasedOperation.
  auto acc = pmeta::PositionalAccuracy::create("1.5");
  auto op_transf = pop::PROJBasedOperation::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "WKT Transf"),
      "+proj=helmert +x=10", pcrs::GeographicCRS::EPSG_4326.as_nullable(),
      pcrs::GeographicCRS::EPSG_4979.as_nullable(), {acc});

  auto wkt2_fmt =
      pio::WKTFormatter::create(pio::WKTFormatter::Convention::WKT2_2019);
  const std::string wkt_transf = op_transf->exportToWKT(wkt2_fmt.get());
  EXPECT_THAT(wkt_transf, StartsWith("COORDINATEOPERATION[\"WKT Transf\""));
  EXPECT_THAT(
      wkt_transf,
      HasSubstr(
          "METHOD[\"PROJ-based operation method: +proj=helmert +x=10\"]"));
  EXPECT_THAT(wkt_transf, HasSubstr("OPERATIONACCURACY[1.5]"));

  auto parsed_transf_obj = pio::WKTParser().createFromWKT(wkt_transf);
  auto parsed_transf = putil::nn_dynamic_pointer_cast<pop::PROJBasedOperation>(
      parsed_transf_obj);
  ASSERT_THAT(parsed_transf.get(), NotNull());
  auto fmt_parsed_t = pio::PROJStringFormatter::create();
  EXPECT_EQ("+proj=helmert +x=10",
            parsed_transf->exportToPROJString(fmt_parsed_t.get()));

  // WKT1 export fails when acting as Transformation too.
  EXPECT_THROW(
      (void)op_transf->exportToWKT(
          pio::WKTFormatter::create(pio::WKTFormatter::Convention::WKT1_GDAL)
              .get()),
      pio::FormattingException);

  // 2. Without CRSs: throws FormattingException for WKT1 dialects and exports
  // CONVERSION[...] for WKT2 dialects.
  auto op_conv = pop::PROJBasedOperation::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY, "WKT Conv"),
      "+proj=helmert +x=5", nullptr, nullptr, {});

  for (const auto conv_kind : {pio::WKTFormatter::Convention::WKT1_GDAL,
                               pio::WKTFormatter::Convention::WKT1_ESRI}) {
    bool threw = false;
    try {
      (void)op_conv->exportToWKT(pio::WKTFormatter::create(conv_kind).get());
    } catch (const pio::FormattingException& e) {
      threw = true;
      EXPECT_STREQ("PROJBasedOperation can only be exported to WKT2", e.what());
    }
    EXPECT_TRUE(threw);
  }

  const std::string wkt_conv = op_conv->exportToWKT(
      pio::WKTFormatter::create(pio::WKTFormatter::Convention::WKT2).get());
  EXPECT_THAT(wkt_conv, StartsWith("CONVERSION[\"WKT Conv\""));
  EXPECT_THAT(
      wkt_conv,
      HasSubstr("METHOD[\"PROJ-based operation method: +proj=helmert +x=5\"]"));

  auto parsed_conv = putil::nn_dynamic_pointer_cast<pop::PROJBasedOperation>(
      pio::WKTParser().createFromWKT(wkt_conv));
  ASSERT_THAT(parsed_conv.get(), NotNull());
  auto fmt_parsed_c = pio::PROJStringFormatter::create();
  EXPECT_EQ("+proj=helmert +x=5",
            parsed_conv->exportToPROJString(fmt_parsed_c.get()));

  // 3. White-box parameterValues() loop during CONVERSION WKT2 export.
  auto method = pop::OperationMethod::create(
      putil::PropertyMap().set(
          pcommon::IdentifiedObject::NAME_KEY,
          "PROJ-based operation method: +proj=helmert +x=1.5"),
      std::vector<pop::GeneralOperationParameterNNPtr>{});
  auto param = pop::OperationParameter::create(putil::PropertyMap().set(
      pcommon::IdentifiedObject::NAME_KEY, "X-axis translation"));
  auto param_value = pop::OperationParameterValue::create(
      param, pop::ParameterValue::create(
                 pcommon::Measure(1.5, pcommon::UnitOfMeasure::METRE)));
  auto op_with_params = TestPROJBasedOperation::CreateWithParameters(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "Param Conv"),
      {param_value}, method);

  const std::string wkt_with_params = op_with_params->exportToWKT(
      pio::WKTFormatter::create(pio::WKTFormatter::Convention::WKT2_2019)
          .get());
  EXPECT_THAT(wkt_with_params,
              HasSubstr("PARAMETER[\"X-axis translation\",1.5"));
  EXPECT_THAT(wkt_with_params, HasSubstr("metre"));
}

TEST(ProjBasedOperationTest,
     ExportToJsonHandlesConversionTransformationUnnamedIdAndParams) {
  auto format_compact_json = [](const pio::IJSONExportable& obj) {
    auto fmt = pio::JSONFormatter::create();
    fmt->setMultiLine(false);
    return obj.exportToJSON(fmt.get());
  };

  // 1. Both CRSs present + identifier -> "type":"Transformation", source_crs,
  // target_crs, non-empty name, and non-empty identifiers().
  putil::PropertyMap transf_props;
  transf_props.set(pcommon::IdentifiedObject::NAME_KEY, "JSON Transf");
  transf_props.set(pcommon::IdentifiedObject::IDENTIFIERS_KEY,
                   pmeta::Identifier::create(
                       "OP1", putil::PropertyMap().set(
                                  pmeta::Identifier::CODESPACE_KEY, "TEST")));
  auto op_transf = pop::PROJBasedOperation::create(
      transf_props, "+proj=helmert +x=10",
      pcrs::GeographicCRS::EPSG_4326.as_nullable(),
      pcrs::GeographicCRS::EPSG_4979.as_nullable(), {});
  ASSERT_FALSE(op_transf->identifiers().empty());

  const std::string json_transf = format_compact_json(*op_transf);
  EXPECT_THAT(json_transf, HasSubstr("\"type\":\"Transformation\""));
  EXPECT_THAT(json_transf, HasSubstr("\"name\":\"JSON Transf\""));
  EXPECT_THAT(json_transf, HasSubstr("\"source_crs\":{"));
  EXPECT_THAT(json_transf, HasSubstr("\"target_crs\":{"));
  EXPECT_THAT(json_transf, HasSubstr("\"method\":{"));
  EXPECT_THAT(json_transf, HasSubstr("\"parameters\":[]"));

  // 2. Null CRSs + explicitly empty name -> "type":"Conversion" and
  // "name":"unnamed", with no source_crs or target_crs.
  putil::PropertyMap empty_name_props;
  empty_name_props.set(pcommon::IdentifiedObject::NAME_KEY, "");
  auto op_unnamed = pop::PROJBasedOperation::create(
      empty_name_props, "+proj=helmert +x=10", nullptr, nullptr, {});
  EXPECT_TRUE(op_unnamed->nameStr().empty());

  const std::string json_unnamed = format_compact_json(*op_unnamed);
  EXPECT_THAT(json_unnamed, HasSubstr("\"type\":\"Conversion\""));
  EXPECT_THAT(json_unnamed, HasSubstr("\"name\":\"unnamed\""));
  EXPECT_THAT(json_unnamed, Not(HasSubstr("\"source_crs\"")));
  EXPECT_THAT(json_unnamed, Not(HasSubstr("\"target_crs\"")));

  // 3. White-box parameterValues() loop during JSON export.
  auto method = pop::OperationMethod::create(
      putil::PropertyMap().set(
          pcommon::IdentifiedObject::NAME_KEY,
          "PROJ-based operation method: +proj=helmert +x=1.5"),
      std::vector<pop::GeneralOperationParameterNNPtr>{});
  auto param = pop::OperationParameter::create(putil::PropertyMap().set(
      pcommon::IdentifiedObject::NAME_KEY, "X-axis translation"));
  auto param_value = pop::OperationParameterValue::create(
      param, pop::ParameterValue::create(
                 pcommon::Measure(1.5, pcommon::UnitOfMeasure::METRE)));
  auto op_with_params = TestPROJBasedOperation::CreateWithParameters(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "JSON Param Conv"),
      {param_value}, method);

  const std::string json_params = format_compact_json(*op_with_params);
  EXPECT_THAT(json_params, HasSubstr("\"type\":\"Conversion\""));
  EXPECT_THAT(json_params, HasSubstr("\"name\":\"JSON Param Conv\""));
  EXPECT_THAT(json_params, HasSubstr("\"name\":\"X-axis translation\""));
  EXPECT_THAT(json_params, HasSubstr("\"value\":1.5"));
  EXPECT_THAT(json_params, HasSubstr("\"unit\":\"metre\""));
}

TEST(ProjBasedOperationTest, ShallowCloneProducesDistinctEquivalentCopy) {
  auto op_no_crs = pop::PROJBasedOperation::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "CloneConv"),
      "+proj=helmert +x=7", nullptr, nullptr, {});
  auto clone_no_crs = op_no_crs->shallowClone();
  EXPECT_NE(op_no_crs.get(), clone_no_crs.get());
  EXPECT_TRUE(op_no_crs->isEquivalentTo(clone_no_crs.get(),
                                        putil::IComparable::Criterion::STRICT));
  EXPECT_EQ(nullptr, clone_no_crs->sourceCRS());
  EXPECT_EQ(nullptr, clone_no_crs->targetCRS());

  auto base_op = pop::SingleOperation::createPROJBased(
      putil::PropertyMap(), "+proj=helmert +x=10 +t_epoch=2020.0", nullptr,
      nullptr);
  auto exportable =
      putil::nn_static_pointer_cast<pio::IPROJStringExportable>(base_op);
  auto acc = pmeta::PositionalAccuracy::create("6.0");
  auto op_wrapped = pop::PROJBasedOperation::create(
      putil::PropertyMap().set(pcommon::IdentifiedObject::NAME_KEY,
                               "CloneWrapped"),
      exportable, /*inverse=*/true, pcrs::GeographicCRS::EPSG_4326,
      pcrs::GeographicCRS::EPSG_4979,
      pcrs::GeographicCRS::OGC_CRS84.as_nullable(), {acc},
      /*hasRoughTransformation=*/true);

  auto clone_wrapped = op_wrapped->shallowClone();
  EXPECT_NE(op_wrapped.get(), clone_wrapped.get());
  EXPECT_TRUE(op_wrapped->isEquivalentTo(
      clone_wrapped.get(), putil::IComparable::Criterion::STRICT));
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4326.get(),
            clone_wrapped->sourceCRS().get());
  EXPECT_EQ(pcrs::GeographicCRS::EPSG_4979.get(),
            clone_wrapped->targetCRS().get());
  EXPECT_EQ(pcrs::GeographicCRS::OGC_CRS84.get(),
            clone_wrapped->interpolationCRS().get());
  EXPECT_TRUE(clone_wrapped->hasBallparkTransformation());
  EXPECT_TRUE(clone_wrapped->requiresPerCoordinateInputTime());

  auto fmt_orig = pio::PROJStringFormatter::create();
  auto fmt_clone = pio::PROJStringFormatter::create();
  EXPECT_EQ(op_wrapped->exportToPROJString(fmt_orig.get()),
            clone_wrapped->exportToPROJString(fmt_clone.get()));
}

TEST(ProjBasedOperationTest,
     GridsNeededResolvesAvailableMissingNullAndInvalidProjStrings) {
  absl::Cleanup cleanup_caches = [] { proj_cleanup(); };

  // Keep InMemoryFileSystem alive strictly longer than ctx and db_ctx.
  InMemoryFileSystem fs;
  constexpr auto kShifts = std::to_array<float>({1.0f, 1.0f, 1.0f, 1.0f});
  fs.AddFile("/mem/test_vgrid.gtx",
             BuildGtxGrid(-10.0, 40.0, 5.0, 5.0, 2, 2, kShifts));

  ScopedContext ctx = MakeContext();
  ASSERT_THAT(ctx.get(), NotNull());
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedLogSuppressor suppress_logs(ctx.get());

  auto db_ctx = pio::DatabaseContext::create("", {}, ctx.get());

  // 1. Operation without grids yields an empty set.
  auto no_grids_op = pop::PROJBasedOperation::create(
      putil::PropertyMap(), "+proj=helmert +x=1", nullptr, nullptr, {});
  EXPECT_THAT(no_grids_op->gridsNeeded(db_ctx.as_nullable(), false), IsEmpty());

  // 2. Pipeline referencing an available in-memory GTX grid, the implicit
  // "null" grid, and a missing grid.
  auto multi_grid_op = pop::PROJBasedOperation::create(
      putil::PropertyMap(),
      "+proj=pipeline "
      "+step +proj=vgridshift +grids=/mem/test_vgrid.gtx "
      "+step +proj=hgridshift +grids=null,/mem/missing_grid.gsb",
      nullptr, nullptr, {});

  // Null databaseContext leaves all GridDescription::available flags false.
  const std::set<pop::GridDescription> without_db = multi_grid_op->gridsNeeded(
      nullptr, /*considerKnownGridsAsAvailable=*/false);
  ASSERT_THAT(without_db, SizeIs(3));
  std::vector<std::string> names_without_db;
  for (const auto& desc : without_db) {
    names_without_db.push_back(desc.shortName);
    EXPECT_FALSE(desc.available) << desc.shortName;
  }
  EXPECT_THAT(names_without_db, ElementsAre("/mem/missing_grid.gsb",
                                            "/mem/test_vgrid.gtx", "null"));

  // Non-null databaseContext checks the context's PROJ_FILE_API and implicit
  // "null" special case.
  for (const bool consider_known : {false, true}) {
    const std::set<pop::GridDescription> with_db =
        multi_grid_op->gridsNeeded(db_ctx.as_nullable(), consider_known);
    ASSERT_THAT(with_db, SizeIs(3));
    std::map<std::string, pop::GridDescription> by_name;
    for (const auto& desc : with_db) {
      by_name.emplace(desc.shortName, desc);
    }
    ASSERT_TRUE(by_name.contains("/mem/test_vgrid.gtx"));
    EXPECT_TRUE(by_name.at("/mem/test_vgrid.gtx").available);
    EXPECT_EQ("/mem/test_vgrid.gtx",
              by_name.at("/mem/test_vgrid.gtx").fullName);

    ASSERT_TRUE(by_name.contains("null"));
    EXPECT_TRUE(by_name.at("null").available);
    EXPECT_TRUE(by_name.at("null").openLicense);

    ASSERT_TRUE(by_name.contains("/mem/missing_grid.gsb"));
    EXPECT_FALSE(by_name.at("/mem/missing_grid.gsb").available);
  }

  // 3. Nested pipeline emitted by a custom IPROJStringExportable causes
  // ingestPROJString() inside gridsNeeded() to throw io::ParsingException,
  // which gridsNeeded() catches and returns an empty set.
  auto bad_exportable =
      putil::nn_static_pointer_cast<pio::IPROJStringExportable>(
          putil::nn_make_shared<NestedPipelineExportable>());
  auto op_bad_export = pop::PROJBasedOperation::create(
      putil::PropertyMap(), bad_exportable, /*inverse=*/false,
      pcrs::GeographicCRS::EPSG_4326, pcrs::GeographicCRS::EPSG_4979, nullptr,
      {}, /*hasRoughTransformation=*/false);
  EXPECT_THAT(op_bad_export->gridsNeeded(db_ctx.as_nullable(), false),
              IsEmpty());
}

// TODO(schwehr): Defensive catch(const io::ParsingException&) blocks in
// PROJBasedOperation::inverse() and _exportToPROJString() are unreachable via
// PROJBasedOperation::create(), which pre-validates PROJString upfront and
// routes non-null projStringExportable_ directly to _exportToPROJString().

}  // namespace
}  // namespace proj
