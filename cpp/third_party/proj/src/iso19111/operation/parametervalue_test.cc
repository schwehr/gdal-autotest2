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
// Unit tests for ISO-19111 ParameterValue construction, accessors, WKT
// formatting, and equivalence comparisons
// (proj/src/iso19111/operation/parametervalue.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/parametervalue.cpp

#include <cstddef>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinateoperation.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/nn.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"

namespace proj {
namespace {

using ::NS_PROJ::common::IdentifiedObject;
using ::NS_PROJ::common::Measure;
using ::NS_PROJ::common::UnitOfMeasure;
using ::NS_PROJ::common::UnitOfMeasureNNPtr;
using ::NS_PROJ::io::FormattingException;
using ::NS_PROJ::io::WKTFormatter;
using ::NS_PROJ::operation::OperationParameter;
using ::NS_PROJ::operation::OperationParameterValue;
using ::NS_PROJ::operation::ParameterValue;
using ::NS_PROJ::operation::ParameterValueNNPtr;
using ::NS_PROJ::util::IComparable;
using ::NS_PROJ::util::PropertyMap;
using ::testing::AllOf;
using ::testing::HasSubstr;

UnitOfMeasureNNPtr MakeUnitNN(const UnitOfMeasure& unit) {
  return NN_CHECK_THROW(std::make_shared<UnitOfMeasure>(unit));
}

std::string ExportParamValueToWkt(
    const ParameterValueNNPtr& pv, WKTFormatter* formatter,
    const UnitOfMeasure* axis_linear_unit = nullptr,
    const UnitOfMeasure* axis_angular_unit = nullptr) {
  if (axis_linear_unit != nullptr) {
    formatter->pushAxisLinearUnit(MakeUnitNN(*axis_linear_unit));
  }
  if (axis_angular_unit != nullptr) {
    formatter->pushAxisAngularUnit(MakeUnitNN(*axis_angular_unit));
  }
  formatter->startNode("PARAMETER", /*hasId=*/false);
  absl::Cleanup end_node = [formatter, axis_linear_unit, axis_angular_unit]() {
    formatter->endNode();
    if (axis_angular_unit != nullptr) {
      formatter->popAxisAngularUnit();
    }
    if (axis_linear_unit != nullptr) {
      formatter->popAxisLinearUnit();
    }
  };
  pv->_exportToWKT(formatter);
  std::move(end_node).Invoke();
  return formatter->toString();
}

double ParseSingleNumericParameter(const std::string& wkt) {
  constexpr std::string_view kPrefix = "PARAMETER[";
  if (wkt.size() <= kPrefix.size() + 1 ||
      wkt.compare(0, kPrefix.size(), kPrefix) != 0 || wkt.back() != ']') {
    ADD_FAILURE() << "Unexpected PARAMETER WKT format: " << wkt;
    return 0.0;
  }
  const std::string inner =
      wkt.substr(kPrefix.size(), wkt.size() - kPrefix.size() - 1);
  size_t parsed_chars = 0;
  const double value = std::stod(inner, &parsed_chars);
  if (parsed_chars != inner.size()) {
    ADD_FAILURE() << "Expected pure numeric parameter payload in: " << wkt;
  }
  return value;
}

TEST(ParameterValueTest, CreateAndAccessorsAllTypes) {
  const Measure deg_measure(45.5, UnitOfMeasure::DEGREE);
  ParameterValueNNPtr measure_pv = ParameterValue::create(deg_measure);
  EXPECT_EQ(measure_pv->type(), ParameterValue::Type::MEASURE);
  EXPECT_TRUE(measure_pv->value() == deg_measure);
  EXPECT_DOUBLE_EQ(measure_pv->value().value(), 45.5);
  EXPECT_EQ(measure_pv->value().unit(), UnitOfMeasure::DEGREE);

  const char* cstr = "grid_shift_cstr";
  ParameterValueNNPtr cstr_pv = ParameterValue::create(cstr);
  EXPECT_EQ(cstr_pv->type(), ParameterValue::Type::STRING);
  EXPECT_EQ(cstr_pv->stringValue(), "grid_shift_cstr");

  const std::string cpp_str = "NADCON5";
  ParameterValueNNPtr str_pv = ParameterValue::create(cpp_str);
  EXPECT_EQ(str_pv->type(), ParameterValue::Type::STRING);
  EXPECT_EQ(str_pv->stringValue(), "NADCON5");

  ParameterValueNNPtr file_pv =
      ParameterValue::createFilename("us_noaa_conus.tif");
  EXPECT_EQ(file_pv->type(), ParameterValue::Type::FILENAME);
  EXPECT_EQ(file_pv->valueFile(), "us_noaa_conus.tif");
  EXPECT_EQ(file_pv->stringValue(), "us_noaa_conus.tif");

  ParameterValueNNPtr pos_int_pv = ParameterValue::create(42);
  EXPECT_EQ(pos_int_pv->type(), ParameterValue::Type::INTEGER);
  EXPECT_EQ(pos_int_pv->integerValue(), 42);

  ParameterValueNNPtr neg_int_pv = ParameterValue::create(-7);
  EXPECT_EQ(neg_int_pv->type(), ParameterValue::Type::INTEGER);
  EXPECT_EQ(neg_int_pv->integerValue(), -7);

  ParameterValueNNPtr true_pv = ParameterValue::create(true);
  EXPECT_EQ(true_pv->type(), ParameterValue::Type::BOOLEAN);
  EXPECT_TRUE(true_pv->booleanValue());

  ParameterValueNNPtr false_pv = ParameterValue::create(false);
  EXPECT_EQ(false_pv->type(), ParameterValue::Type::BOOLEAN);
  EXPECT_FALSE(false_pv->booleanValue());
}

TEST(ParameterValueTest, ExportToWktWkt2FullAndNoneUnit) {
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    auto pv = ParameterValue::create(Measure(500000.0, UnitOfMeasure::METRE));
    EXPECT_THAT(ExportParamValueToWkt(pv, fmt.get()),
                AllOf(HasSubstr("500000"), HasSubstr("LENGTHUNIT[\"metre\"")));
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
    auto pv = ParameterValue::create(Measure(15.0, UnitOfMeasure::DEGREE));
    EXPECT_THAT(ExportParamValueToWkt(pv, fmt.get()),
                AllOf(HasSubstr("15"), HasSubstr("ANGLEUNIT[\"degree\"")));
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    auto pv =
        ParameterValue::create(Measure(0.9996, UnitOfMeasure::SCALE_UNITY));
    EXPECT_THAT(ExportParamValueToWkt(pv, fmt.get()),
                AllOf(HasSubstr("0.9996"), HasSubstr("SCALEUNIT[\"unity\"")));
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    auto pv = ParameterValue::create(Measure(3.5, UnitOfMeasure::NONE));
    EXPECT_EQ(ExportParamValueToWkt(pv, fmt.get()), "PARAMETER[3.5]");
  }
}

TEST(ParameterValueTest, ExportToWktWkt2SimplifiedUnitOmissionRules) {
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_SIMPLIFIED);
    auto pv =
        ParameterValue::create(Measure(0.9996, UnitOfMeasure::SCALE_UNITY));
    EXPECT_EQ(ExportParamValueToWkt(pv, fmt.get()), "PARAMETER[0.9996]");
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_SIMPLIFIED);
    auto pv = ParameterValue::create(Measure(500000.0, UnitOfMeasure::METRE));
    EXPECT_EQ(ExportParamValueToWkt(pv, fmt.get()), "PARAMETER[500000]");
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_SIMPLIFIED);
    auto pv = ParameterValue::create(Measure(100.0, UnitOfMeasure::FOOT));
    EXPECT_THAT(ExportParamValueToWkt(pv, fmt.get()),
                AllOf(HasSubstr("100"), HasSubstr("UNIT[\"foot\"")));
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_SIMPLIFIED);
    auto foot_pv = ParameterValue::create(Measure(100.0, UnitOfMeasure::FOOT));
    EXPECT_EQ(ExportParamValueToWkt(foot_pv, fmt.get(), &UnitOfMeasure::FOOT),
              "PARAMETER[100]");

    auto fmt_metre =
        WKTFormatter::create(WKTFormatter::Convention::WKT2_SIMPLIFIED);
    auto metre_pv = ParameterValue::create(Measure(25.0, UnitOfMeasure::METRE));
    EXPECT_THAT(
        ExportParamValueToWkt(metre_pv, fmt_metre.get(), &UnitOfMeasure::FOOT),
        AllOf(HasSubstr("25"), HasSubstr("UNIT[\"metre\"")));
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_SIMPLIFIED);
    auto deg_pv = ParameterValue::create(Measure(15.0, UnitOfMeasure::DEGREE));
    EXPECT_EQ(ExportParamValueToWkt(deg_pv, fmt.get()), "PARAMETER[15]");

    auto fmt_rad =
        WKTFormatter::create(WKTFormatter::Convention::WKT2_SIMPLIFIED);
    auto rad_pv = ParameterValue::create(Measure(0.5, UnitOfMeasure::RADIAN));
    EXPECT_THAT(ExportParamValueToWkt(rad_pv, fmt_rad.get()),
                AllOf(HasSubstr("0.5"), HasSubstr("UNIT[\"radian\"")));
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_SIMPLIFIED);
    auto rad_pv = ParameterValue::create(Measure(1.5, UnitOfMeasure::RADIAN));
    EXPECT_EQ(ExportParamValueToWkt(rad_pv, fmt.get(), nullptr,
                                    &UnitOfMeasure::RADIAN),
              "PARAMETER[1.5]");

    auto fmt_deg =
        WKTFormatter::create(WKTFormatter::Convention::WKT2_SIMPLIFIED);
    auto deg_pv = ParameterValue::create(Measure(30.0, UnitOfMeasure::DEGREE));
    EXPECT_THAT(ExportParamValueToWkt(deg_pv, fmt_deg.get(), nullptr,
                                      &UnitOfMeasure::RADIAN),
                AllOf(HasSubstr("30"), HasSubstr("UNIT[\"degree\"")));
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2_SIMPLIFIED);
    auto ppm_pv =
        ParameterValue::create(Measure(2.5, UnitOfMeasure::PARTS_PER_MILLION));
    EXPECT_THAT(
        ExportParamValueToWkt(ppm_pv, fmt.get()),
        AllOf(HasSubstr("2.5"), HasSubstr("UNIT["), HasSubstr("1E-06")));
  }
}

TEST(ParameterValueTest, ExportToWktAbridgedTransformationConversions) {
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    fmt->setAbridgedTransformation(true);
    auto pv = ParameterValue::create(Measure(10.0, UnitOfMeasure::FOOT));
    EXPECT_NEAR(
        ParseSingleNumericParameter(ExportParamValueToWkt(pv, fmt.get())),
        3.048, 1e-12);
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    fmt->setAbridgedTransformation(true);
    auto pv = ParameterValue::create(Measure(1.0, UnitOfMeasure::DEGREE));
    EXPECT_NEAR(
        ParseSingleNumericParameter(ExportParamValueToWkt(pv, fmt.get())),
        3600.0, 1e-9);
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    fmt->setAbridgedTransformation(true);
    auto pv =
        ParameterValue::create(Measure(5.0, UnitOfMeasure::PARTS_PER_MILLION));
    EXPECT_NEAR(
        ParseSingleNumericParameter(ExportParamValueToWkt(pv, fmt.get())),
        1.000005, 1e-12);
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    fmt->setAbridgedTransformation(true);
    auto pv =
        ParameterValue::create(Measure(0.9996, UnitOfMeasure::SCALE_UNITY));
    EXPECT_NEAR(
        ParseSingleNumericParameter(ExportParamValueToWkt(pv, fmt.get())),
        0.9996, 1e-12);
  }
}

TEST(ParameterValueTest, ExportToWkt1GdalConversionsAndInvalidTargets) {
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
    auto pv = ParameterValue::create(Measure(10.0, UnitOfMeasure::FOOT));
    EXPECT_NEAR(
        ParseSingleNumericParameter(ExportParamValueToWkt(pv, fmt.get())),
        3.048, 1e-12);
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
    auto pv = ParameterValue::create(Measure(3.048, UnitOfMeasure::METRE));
    EXPECT_NEAR(ParseSingleNumericParameter(
                    ExportParamValueToWkt(pv, fmt.get(), &UnitOfMeasure::FOOT)),
                10.0, 1e-12);
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
    fmt->pushAxisLinearUnit(MakeUnitNN(
        UnitOfMeasure("zero_linear", 0.0, UnitOfMeasure::Type::LINEAR)));
    absl::Cleanup pop_lin = [&]() { fmt->popAxisLinearUnit(); };
    auto pv = ParameterValue::create(Measure(1.0, UnitOfMeasure::METRE));
    fmt->startNode("PARAMETER", /*hasId=*/false);
    absl::Cleanup end_node = [&]() { fmt->endNode(); };
    EXPECT_THROW(pv->_exportToWKT(fmt.get()), FormattingException);
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
    auto pv = ParameterValue::create(Measure(200.0, UnitOfMeasure::GRAD));
    EXPECT_NEAR(
        ParseSingleNumericParameter(ExportParamValueToWkt(pv, fmt.get())),
        180.0, 1e-11);
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
    auto pv = ParameterValue::create(Measure(90.0, UnitOfMeasure::DEGREE));
    EXPECT_NEAR(ParseSingleNumericParameter(ExportParamValueToWkt(
                    pv, fmt.get(), nullptr, &UnitOfMeasure::GRAD)),
                100.0, 1e-11);
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
    fmt->pushAxisAngularUnit(MakeUnitNN(
        UnitOfMeasure("zero_angular", 0.0, UnitOfMeasure::Type::ANGULAR)));
    absl::Cleanup pop_ang = [&]() { fmt->popAxisAngularUnit(); };
    auto pv = ParameterValue::create(Measure(1.0, UnitOfMeasure::DEGREE));
    fmt->startNode("PARAMETER", /*hasId=*/false);
    absl::Cleanup end_node = [&]() { fmt->endNode(); };
    EXPECT_THROW(pv->_exportToWKT(fmt.get()), FormattingException);
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
    auto pv =
        ParameterValue::create(Measure(5.0, UnitOfMeasure::PARTS_PER_MILLION));
    EXPECT_NEAR(
        ParseSingleNumericParameter(ExportParamValueToWkt(pv, fmt.get())), 5e-6,
        1e-15);
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
    auto pv =
        ParameterValue::create(Measure(0.9996, UnitOfMeasure::SCALE_UNITY));
    EXPECT_NEAR(
        ParseSingleNumericParameter(ExportParamValueToWkt(pv, fmt.get())),
        0.9996, 1e-12);
  }
}

TEST(ParameterValueTest, ExportToWktStringFilenameIntegerAndBooleanThrows) {
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    auto pv = ParameterValue::create("foo\"bar");
    EXPECT_EQ(ExportParamValueToWkt(pv, fmt.get()),
              "PARAMETER[\"foo\"\"bar\"]");
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    auto pv = ParameterValue::createFilename("grid.gtx");
    EXPECT_EQ(ExportParamValueToWkt(pv, fmt.get()), "PARAMETER[\"grid.gtx\"]");

    auto param = OperationParameter::create(
        PropertyMap().set(IdentifiedObject::NAME_KEY, "Geoid grid"));
    auto op_pv = OperationParameterValue::create(param, pv);
    auto op_fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    EXPECT_THAT(op_pv->exportToWKT(op_fmt.get()),
                HasSubstr("PARAMETERFILE[\"Geoid grid\",\"grid.gtx\"]"));
  }
  {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    auto pv = ParameterValue::create(17);
    EXPECT_EQ(ExportParamValueToWkt(pv, fmt.get()), "PARAMETER[17]");
  }
  for (bool bool_val : {true, false}) {
    auto fmt = WKTFormatter::create(WKTFormatter::Convention::WKT2);
    auto pv = ParameterValue::create(bool_val);
    fmt->startNode("PARAMETER", /*hasId=*/false);
    absl::Cleanup end_node = [&]() { fmt->endNode(); };
    EXPECT_THROW(pv->_exportToWKT(fmt.get()), FormattingException);
  }
}

TEST(ParameterValueTest, EquivalenceAcrossTypesAndCriteria) {
  auto m_deg = ParameterValue::create(Measure(180.0, UnitOfMeasure::DEGREE));
  auto m_deg_same =
      ParameterValue::create(Measure(180.0, UnitOfMeasure::DEGREE));
  auto m_rad =
      ParameterValue::create(Measure(std::numbers::pi, UnitOfMeasure::RADIAN));
  auto m_diff = ParameterValue::create(Measure(90.0, UnitOfMeasure::DEGREE));
  auto m_one = ParameterValue::create(Measure(1.0, UnitOfMeasure::METRE));
  auto m_within_tol =
      ParameterValue::create(Measure(1.0 + 1e-11, UnitOfMeasure::METRE));
  auto m_outside_tol =
      ParameterValue::create(Measure(1.0 + 1e-7, UnitOfMeasure::METRE));

  auto s_abc = ParameterValue::create("abc");
  auto s_abc_2 = ParameterValue::create(std::string("abc"));
  auto s_xyz = ParameterValue::create("xyz");
  auto f_abc = ParameterValue::createFilename("abc");
  auto f_abc_2 = ParameterValue::createFilename("abc");
  auto f_xyz = ParameterValue::createFilename("xyz");
  auto i_five = ParameterValue::create(5);
  auto i_five_2 = ParameterValue::create(5);
  auto i_six = ParameterValue::create(6);
  auto b_true = ParameterValue::create(true);
  auto b_true_2 = ParameterValue::create(true);
  auto b_false = ParameterValue::create(false);
  auto b_false_2 = ParameterValue::create(false);

  auto non_pv = OperationParameter::create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "other"));

  // Null and unrelated IComparable object.
  EXPECT_FALSE(m_deg->_isEquivalentTo(nullptr, IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      m_deg->_isEquivalentTo(non_pv.get(), IComparable::Criterion::EQUIVALENT));

  // Cross-type mismatches.
  EXPECT_FALSE(
      s_abc->isEquivalentTo(f_abc.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      i_five->isEquivalentTo(m_one.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(
      b_true->isEquivalentTo(i_five.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(
      m_deg->isEquivalentTo(s_abc.get(), IComparable::Criterion::EQUIVALENT));

  // Type::MEASURE comparisons.
  EXPECT_TRUE(
      m_deg->isEquivalentTo(m_deg_same.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      m_deg->isEquivalentTo(m_rad.get(), IComparable::Criterion::STRICT));
  EXPECT_TRUE(
      m_deg->isEquivalentTo(m_rad.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(
      m_deg->isEquivalentTo(m_diff.get(), IComparable::Criterion::EQUIVALENT));
  EXPECT_TRUE(m_one->isEquivalentTo(m_within_tol.get(),
                                    IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(m_one->isEquivalentTo(m_outside_tol.get(),
                                     IComparable::Criterion::EQUIVALENT));

  // Type::STRING comparisons.
  EXPECT_TRUE(
      s_abc->isEquivalentTo(s_abc_2.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      s_abc->isEquivalentTo(s_xyz.get(), IComparable::Criterion::EQUIVALENT));

  // Type::FILENAME comparisons.
  EXPECT_TRUE(
      f_abc->isEquivalentTo(f_abc_2.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      f_abc->isEquivalentTo(f_xyz.get(), IComparable::Criterion::EQUIVALENT));

  // Type::INTEGER comparisons.
  EXPECT_TRUE(
      i_five->isEquivalentTo(i_five_2.get(), IComparable::Criterion::STRICT));
  EXPECT_FALSE(
      i_five->isEquivalentTo(i_six.get(), IComparable::Criterion::EQUIVALENT));

  // Type::BOOLEAN comparisons.
  EXPECT_TRUE(
      b_true->isEquivalentTo(b_true_2.get(), IComparable::Criterion::STRICT));
  EXPECT_TRUE(b_false->isEquivalentTo(b_false_2.get(),
                                      IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(b_true->isEquivalentTo(b_false.get(),
                                      IComparable::Criterion::EQUIVALENT));
}

}  // namespace
}  // namespace proj
