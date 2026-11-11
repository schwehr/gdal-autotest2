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
// Unit tests for internal ISO-19111 VectorOfValues, VectorOfParameters, and
// createParams helpers
// (proj/src/iso19111/operation/vectorofvaluesparams.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/operation/vectorofvaluesparams.cpp

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/proj/include/proj/common.hpp"
#include "third_party/proj4/proj/include/proj/coordinateoperation.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"

namespace {
thread_local bool g_throw_in_vector_alloc = false;
}  // namespace

// Explicit specialization of std::allocator<ParameterValueNNPtr>::allocate
// (using only standard C++20 ::operator new, no libc++-private symbols; note
// C++20 [namespace.std]/3) so the 5 exception-cleanup landing-pad lines in
// createParams(3..7) in the 40-line vectorofvaluesparams.cpp translation unit
// can be exercised deterministically. Must precede vectorofvaluesparams.hpp,
// which instantiates std::vector<ParameterValueNNPtr>.
template <>
constexpr ::NS_PROJ::operation::ParameterValueNNPtr*
std::allocator<::NS_PROJ::operation::ParameterValueNNPtr>::allocate(
    std::size_t n) {
  if (std::is_constant_evaluated()) {
    return nullptr;
  }
  if (g_throw_in_vector_alloc) {
    throw std::bad_alloc();
  }
  return static_cast<::NS_PROJ::operation::ParameterValueNNPtr*>(
      ::operator new(n * sizeof(::NS_PROJ::operation::ParameterValueNNPtr)));
}

#include "third_party/proj4/proj/src/iso19111/operation/vectorofvaluesparams.hpp"

namespace proj {
namespace {

namespace common = ::NS_PROJ::common;
namespace op = ::NS_PROJ::operation;
namespace util = ::NS_PROJ::util;

// Compile-time ownership and inheritance invariants from
// vectorofvaluesparams.hpp.
static_assert(std::is_base_of_v<std::vector<op::ParameterValueNNPtr>,
                                op::VectorOfValues>);
static_assert(!std::is_copy_constructible_v<op::VectorOfValues>);
static_assert(std::is_move_constructible_v<op::VectorOfValues>);
static_assert(!std::is_copy_assignable_v<op::VectorOfValues>);
static_assert(!std::is_move_assignable_v<op::VectorOfValues>);

static_assert(std::is_base_of_v<std::vector<op::OperationParameterNNPtr>,
                                op::VectorOfParameters>);
static_assert(!std::is_copy_constructible_v<op::VectorOfParameters>);
static_assert(!std::is_move_constructible_v<op::VectorOfParameters>);

struct ExpectedMeasure {
  double value;
  const common::UnitOfMeasure* unit;
};

void ExpectMeasureVectorMatches(
    const op::VectorOfValues& actual,
    std::initializer_list<ExpectedMeasure> expected) {
  ASSERT_EQ(expected.size(), actual.size());
  size_t index = 0;
  for (const ExpectedMeasure& exp : expected) {
    EXPECT_EQ(op::ParameterValue::Type::MEASURE, actual[index]->type())
        << "index=" << index;
    EXPECT_DOUBLE_EQ(exp.value, actual[index]->value().value())
        << "index=" << index;
    EXPECT_EQ(*exp.unit, actual[index]->value().unit()) << "index=" << index;
    ++index;
  }
}

op::OperationParameterNNPtr MakeOperationParam(const char* name) {
  return op::OperationParameter::create(
      util::PropertyMap().set(common::IdentifiedObject::NAME_KEY, name));
}

TEST(VectorOfValuesParamsTest, VectorOfValuesDefaultAndParameterValueInit) {
  // 1. Default constructor creates an empty vector and exercises
  // ~VectorOfValues().
  const op::VectorOfValues empty_default;
  EXPECT_TRUE(empty_default.empty());
  EXPECT_EQ(0u, empty_default.size());

  // 2. Empty ParameterValueNNPtr initializer list produces an empty vector.
  const op::VectorOfValues empty_pv_list(
      std::initializer_list<op::ParameterValueNNPtr>{});
  EXPECT_TRUE(empty_pv_list.empty());

  // 3. Populated ParameterValueNNPtr initializer list preserves heterogeneous
  // types.
  const op::VectorOfValues pv_list{
      op::ParameterValue::create(common::Length(125.5)),
      op::ParameterValue::create("NAD83"),
      op::ParameterValue::create(16),
      op::ParameterValue::create(false),
      op::ParameterValue::createFilename("us_noaa_nadcon5.tif"),
  };
  ASSERT_EQ(5u, pv_list.size());
  EXPECT_EQ(op::ParameterValue::Type::MEASURE, pv_list[0]->type());
  EXPECT_DOUBLE_EQ(125.5, pv_list[0]->value().value());
  EXPECT_EQ(common::UnitOfMeasure::METRE, pv_list[0]->value().unit());
  EXPECT_EQ(op::ParameterValue::Type::STRING, pv_list[1]->type());
  EXPECT_EQ("NAD83", pv_list[1]->stringValue());
  EXPECT_EQ(op::ParameterValue::Type::INTEGER, pv_list[2]->type());
  EXPECT_EQ(16, pv_list[2]->integerValue());
  EXPECT_EQ(op::ParameterValue::Type::BOOLEAN, pv_list[3]->type());
  EXPECT_FALSE(pv_list[3]->booleanValue());
  EXPECT_EQ(op::ParameterValue::Type::FILENAME, pv_list[4]->type());
  EXPECT_EQ("us_noaa_nadcon5.tif", pv_list[4]->valueFile());
}

TEST(VectorOfValuesParamsTest,
     VectorOfValuesFromMeasureInitializerListAndMove) {
  // 1. Empty Measure initializer list exercises zero-iteration loop in
  // buildParameterValueFromMeasure().
  const op::VectorOfValues empty_measure_list(
      std::initializer_list<common::Measure>{});
  EXPECT_TRUE(empty_measure_list.empty());

  // 2. Single-element Measure initializer list with default UnitOfMeasure().
  const op::VectorOfValues single_measure{common::Measure(42.25)};
  ASSERT_EQ(1u, single_measure.size());
  EXPECT_EQ(op::ParameterValue::Type::MEASURE, single_measure[0]->type());
  EXPECT_DOUBLE_EQ(42.25, single_measure[0]->value().value());
  EXPECT_EQ(common::UnitOfMeasure(), single_measure[0]->value().unit());

  // 3. Multi-element Measure initializer list covers Measure and derived
  // Length, Angle, and Scale conversions.
  op::VectorOfValues multi_measures{
      common::Measure(2.5, common::UnitOfMeasure::NONE),
      common::Length(500000.0),
      common::Angle(-45.0),
      common::Scale(0.9996),
  };
  ExpectMeasureVectorMatches(multi_measures,
                             {
                                 {2.5, &common::UnitOfMeasure::NONE},
                                 {500000.0, &common::UnitOfMeasure::METRE},
                                 {-45.0, &common::UnitOfMeasure::DEGREE},
                                 {0.9996, &common::UnitOfMeasure::SCALE_UNITY},
                             });

  // 4. Move constructor transfers ownership without copying ParameterValue
  // objects.
  const op::ParameterValue* first_ptr = multi_measures[0].get();
  op::VectorOfValues moved(std::move(multi_measures));
  EXPECT_TRUE(multi_measures.empty());
  ASSERT_EQ(4u, moved.size());
  EXPECT_EQ(first_ptr, moved[0].get());

  // Inherited std::vector mutators remain usable after move construction.
  moved.push_back(op::ParameterValue::create(
      common::Length(10.0, common::UnitOfMeasure::FOOT)));
  ASSERT_EQ(5u, moved.size());
  EXPECT_EQ(common::UnitOfMeasure::FOOT, moved[4]->value().unit());
  EXPECT_NEAR(3.048, moved[4]->value().getSIValue(), 1e-12);
}

TEST(VectorOfValuesParamsTest, VectorOfValuesPreservesSignedZeroAndNonFinite) {
  const double nan_val = std::numeric_limits<double>::quiet_NaN();
  const double inf_val = std::numeric_limits<double>::infinity();

  const op::VectorOfValues edge_values{
      common::Measure(-0.0, common::UnitOfMeasure::RADIAN),
      common::Measure(nan_val, common::UnitOfMeasure::METRE),
      common::Measure(inf_val, common::UnitOfMeasure::DEGREE),
      common::Measure(-inf_val, common::UnitOfMeasure::SCALE_UNITY),
  };
  ASSERT_EQ(4u, edge_values.size());
  EXPECT_DOUBLE_EQ(0.0, edge_values[0]->value().value());
  EXPECT_TRUE(std::signbit(edge_values[0]->value().value()));
  EXPECT_EQ(common::UnitOfMeasure::RADIAN, edge_values[0]->value().unit());
  EXPECT_TRUE(std::isnan(edge_values[1]->value().value()));
  EXPECT_EQ(common::UnitOfMeasure::METRE, edge_values[1]->value().unit());
  EXPECT_TRUE(std::isinf(edge_values[2]->value().value()));
  EXPECT_GT(edge_values[2]->value().value(), 0.0);
  EXPECT_TRUE(std::isinf(edge_values[3]->value().value()));
  EXPECT_LT(edge_values[3]->value().value(), 0.0);
}

TEST(VectorOfValuesParamsTest, CreateParamsThreeThroughSevenOverloads) {
  // 1. 3-argument createParams overload (geocentric translation offsets).
  const op::VectorOfValues params3 =
      op::createParams(common::Length(10.5), common::Length(-20.25),
                       common::Length(30.75, common::UnitOfMeasure::FOOT));
  ExpectMeasureVectorMatches(params3,
                             {
                                 {10.5, &common::UnitOfMeasure::METRE},
                                 {-20.25, &common::UnitOfMeasure::METRE},
                                 {30.75, &common::UnitOfMeasure::FOOT},
                             });

  // 2. 4-argument createParams overload (e.g., 1SP projection parameters).
  const op::VectorOfValues params4 = op::createParams(
      common::Angle(0.5 * std::numbers::pi, common::UnitOfMeasure::RADIAN),
      common::Angle(-90.0), common::Length(1000.0), common::Length(2000.0));
  ExpectMeasureVectorMatches(
      params4, {
                   {0.5 * std::numbers::pi, &common::UnitOfMeasure::RADIAN},
                   {-90.0, &common::UnitOfMeasure::DEGREE},
                   {1000.0, &common::UnitOfMeasure::METRE},
                   {2000.0, &common::UnitOfMeasure::METRE},
               });

  // 3. 5-argument createParams overload (Transverse Mercator parameters).
  const op::VectorOfValues params5 = op::createParams(
      common::Angle(0.0), common::Angle(9.0), common::Scale(0.9996),
      common::Length(500000.0), common::Length(0.0));
  ExpectMeasureVectorMatches(params5,
                             {
                                 {0.0, &common::UnitOfMeasure::DEGREE},
                                 {9.0, &common::UnitOfMeasure::DEGREE},
                                 {0.9996, &common::UnitOfMeasure::SCALE_UNITY},
                                 {500000.0, &common::UnitOfMeasure::METRE},
                                 {0.0, &common::UnitOfMeasure::METRE},
                             });

  // 4. 6-argument createParams overload (Lambert Conic Conformal 2SP).
  const op::VectorOfValues params6 = op::createParams(
      common::Angle(49.0), common::Angle(-2.0), common::Angle(44.0),
      common::Angle(50.0), common::Length(700000.0), common::Length(6600000.0));
  ExpectMeasureVectorMatches(params6,
                             {
                                 {49.0, &common::UnitOfMeasure::DEGREE},
                                 {-2.0, &common::UnitOfMeasure::DEGREE},
                                 {44.0, &common::UnitOfMeasure::DEGREE},
                                 {50.0, &common::UnitOfMeasure::DEGREE},
                                 {700000.0, &common::UnitOfMeasure::METRE},
                                 {6600000.0, &common::UnitOfMeasure::METRE},
                             });

  // 5. 7-argument createParams overload (7-parameter Helmert transformation).
  const op::VectorOfValues params7 = op::createParams(
      common::Length(12.3), common::Length(-45.6), common::Length(78.9),
      common::Angle(0.12, common::UnitOfMeasure::ARC_SECOND),
      common::Angle(-0.34, common::UnitOfMeasure::ARC_SECOND),
      common::Angle(0.56, common::UnitOfMeasure::ARC_SECOND),
      common::Scale(1.75, common::UnitOfMeasure::PARTS_PER_MILLION));
  ExpectMeasureVectorMatches(
      params7, {
                   {12.3, &common::UnitOfMeasure::METRE},
                   {-45.6, &common::UnitOfMeasure::METRE},
                   {78.9, &common::UnitOfMeasure::METRE},
                   {0.12, &common::UnitOfMeasure::ARC_SECOND},
                   {-0.34, &common::UnitOfMeasure::ARC_SECOND},
                   {0.56, &common::UnitOfMeasure::ARC_SECOND},
                   {1.75, &common::UnitOfMeasure::PARTS_PER_MILLION},
               });
  EXPECT_NEAR(1.75e-6, params7[6]->value().getSIValue(), 1e-18);
}

TEST(VectorOfValuesParamsTest, VectorOfParametersConstructionAndDestruction) {
  // 1. Default constructor yields an empty vector.
  const op::VectorOfParameters empty_default;
  EXPECT_TRUE(empty_default.empty());
  EXPECT_EQ(0u, empty_default.size());

  // 2. Empty initializer list yields an empty vector.
  const op::VectorOfParameters empty_init(
      std::initializer_list<op::OperationParameterNNPtr>{});
  EXPECT_TRUE(empty_init.empty());

  // 3. Initializer list populates OperationParameter elements in order.
  op::VectorOfParameters params{
      MakeOperationParam("Latitude of natural origin"),
      MakeOperationParam("Longitude of natural origin"),
      MakeOperationParam("Scale factor at natural origin"),
  };
  ASSERT_EQ(3u, params.size());
  EXPECT_EQ("Latitude of natural origin", params[0]->nameStr());
  EXPECT_EQ("Longitude of natural origin", params[1]->nameStr());
  EXPECT_EQ("Scale factor at natural origin", params[2]->nameStr());

  // 4. Inherited std::vector insertion works before out-of-line
  // ~VectorOfParameters().
  params.push_back(MakeOperationParam("False easting"));
  ASSERT_EQ(4u, params.size());
  EXPECT_EQ("False easting", params[3]->nameStr());
}

TEST(VectorOfValuesParamsTest, IntegratesWithConversionCreate) {
  const op::VectorOfParameters params{
      MakeOperationParam("Latitude of natural origin"),
      MakeOperationParam("Longitude of natural origin"),
      MakeOperationParam("Scale factor at natural origin"),
      MakeOperationParam("False easting"),
      MakeOperationParam("False northing"),
  };
  const op::VectorOfValues values = op::createParams(
      common::Angle(0.0), common::Angle(-75.0), common::Scale(0.9996),
      common::Length(500000.0), common::Length(0.0));

  const op::ConversionNNPtr conv = op::Conversion::create(
      util::PropertyMap().set(common::IdentifiedObject::NAME_KEY,
                              "UTM zone 18N"),
      util::PropertyMap().set(common::IdentifiedObject::NAME_KEY,
                              "Transverse Mercator"),
      params, values);

  EXPECT_EQ(5u, conv->parameterValues().size());
  EXPECT_EQ(5u, conv->method()->parameters().size());

  const op::ParameterValuePtr& central_meridian =
      conv->parameterValue("Longitude of natural origin");
  ASSERT_NE(nullptr, central_meridian);
  EXPECT_DOUBLE_EQ(-75.0, central_meridian->value().value());
  EXPECT_EQ(common::UnitOfMeasure::DEGREE, central_meridian->value().unit());

  const op::ParameterValuePtr& false_easting =
      conv->parameterValue("False easting");
  ASSERT_NE(nullptr, false_easting);
  EXPECT_DOUBLE_EQ(500000.0, false_easting->value().value());
  EXPECT_EQ(common::UnitOfMeasure::METRE, false_easting->value().unit());

  // Exercise exception-unwinding cleanup paths in createParams(3..7) when
  // VectorOfValues construction throws std::bad_alloc.
  const common::Length m(1.0);
  g_throw_in_vector_alloc = true;
  absl::Cleanup reset_alloc = [] { g_throw_in_vector_alloc = false; };
  for (int arity = 3; arity <= 7; ++arity) {
    try {
      switch (arity) {
        case 3:
          (void)op::createParams(m, m, m);
          break;
        case 4:
          (void)op::createParams(m, m, m, m);
          break;
        case 5:
          (void)op::createParams(m, m, m, m, m);
          break;
        case 6:
          (void)op::createParams(m, m, m, m, m, m);
          break;
        case 7:
          (void)op::createParams(m, m, m, m, m, m, m);
          break;
      }
    } catch (const std::bad_alloc&) {
    }
  }
}

}  // namespace
}  // namespace proj
