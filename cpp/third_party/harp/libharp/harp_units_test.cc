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
// Unit tests for libharp UDUNITS-2 unit handling and conversion
// (libharp/harp-units.c).
//
// Google3 build notes: //third_party/udunits embeds udunits2.xml, so unit
// conversions work without UDUNITS2_XML_PATH when harp_udunits2_xml_path is
// NULL.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-units.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-internal.h

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"
#include "third_party/udunits/lib/udunits2.h"

namespace {
constinit thread_local int g_forced_ut_status = -1;
}  // namespace

extern "C" {
ut_status __real_ut_get_status(void);
ut_status __wrap_ut_get_status(void) {
  if (g_forced_ut_status >= 0) {
    return static_cast<ut_status>(g_forced_ut_status);
  }
  return __real_ut_get_status();
}
}  // extern "C"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::FloatNear;

class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    if (const char* prev = std::getenv(name_)) {
      saved_ = prev;
    }
  }
  ~ScopedEnvVar() {
    if (saved_.has_value()) {
      setenv(name_, saved_->c_str(), /*overwrite=*/1);
    } else {
      unsetenv(name_);
    }
  }
  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  const char* name_;
  std::optional<std::string> saved_;
};

TEST(HarpUnitsTest, SetUdunits2XmlPathAndErrorHandling) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto clear_path = absl::MakeCleanup([] {
    harp_unit_done();
    harp_set_udunits2_xml_path(nullptr);
    harp_set_error(HARP_SUCCESS, nullptr);
  });

  // Ensure unit_system starts uninitialized for this test.
  harp_unit_done();

  // Overwrite path multiple times, then clear with nullptr.
  EXPECT_EQ(HARP_SUCCESS, harp_set_udunits2_xml_path("/some/first/path.xml"));
  EXPECT_EQ(HARP_SUCCESS, harp_set_udunits2_xml_path("/some/second/path.xml"));
  EXPECT_EQ(HARP_SUCCESS, harp_set_udunits2_xml_path(nullptr));

  // Missing / unknown XML name triggers UT_PARSE in google3's embedded catalog.
  const TempFile missing_xml("missing_udunits2.xml");
  ASSERT_EQ(HARP_SUCCESS, harp_set_udunits2_xml_path(missing_xml.c_str()));
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(0, harp_unit_is_valid("m"));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);

  // Corrupt XML file triggers UT_PARSE.
  const TempFile corrupt_xml("corrupt_udunits2.xml");
  ASSERT_TRUE(WriteFileText(corrupt_xml.path(), "<not_valid_xml"));
  ASSERT_EQ(HARP_SUCCESS, harp_set_udunits2_xml_path(corrupt_xml.c_str()));
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(0, harp_unit_is_valid("m"));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);

  // Embedded udunits2-prefixes.xml loads a valid unit system without "second",
  // exercising freeing harp_udunits2_xml_path in unit_system_done().
  ASSERT_EQ(HARP_SUCCESS, harp_set_udunits2_xml_path("udunits2-prefixes.xml"));
  EXPECT_EQ(1, harp_unit_is_valid(""));
  EXPECT_EQ(0, harp_unit_is_valid("1 since 2000-01-01"));
  harp_unit_done();

  // Restoring nullptr allows embedded udunits2.xml to load cleanly.
  ASSERT_EQ(HARP_SUCCESS, harp_set_udunits2_xml_path(nullptr));
  EXPECT_EQ(1, harp_unit_is_valid("m"));
}

TEST(HarpUnitsTest, SetUdunits2XmlPathConditional) {
  ScopedEnvVar restore_env("UDUNITS2_XML_PATH");
  auto clear_path = absl::MakeCleanup([] {
    harp_set_udunits2_xml_path(nullptr);
    harp_set_error(HARP_SUCCESS, nullptr);
  });

  // 1. When UDUNITS2_XML_PATH is set, conditional search is skipped.
  setenv("UDUNITS2_XML_PATH", "/ignored/udunits2.xml", /*overwrite=*/1);
  EXPECT_EQ(HARP_SUCCESS, harp_set_udunits2_xml_path_conditional(
                              "nonexistent", nullptr, "udunits2.xml"));

  // 2. With UDUNITS2_XML_PATH unset and explicit searchpath containing a file.
  unsetenv("UDUNITS2_XML_PATH");
  const TempDir dir("units_cond_dir");
  const TempFile marker("units_cond_dir/bin_marker");
  ASSERT_TRUE(WriteFileText(marker.path(), "x"));
  EXPECT_EQ(HARP_SUCCESS,
            harp_set_udunits2_xml_path_conditional("bin_marker", dir.c_str(),
                                                   "../share/udunits2.xml"));

  // 3. With searchpath == nullptr (uses PATH / program path).
  EXPECT_EQ(HARP_SUCCESS, harp_set_udunits2_xml_path_conditional(
                              marker.c_str(), nullptr, "../udunits2.xml"));

  // 4. Not found in searchpath returns 0 without error.
  EXPECT_EQ(HARP_SUCCESS,
            harp_set_udunits2_xml_path_conditional(
                "definitely_missing_file", dir.c_str(), "udunits2.xml"));
}

TEST(HarpUnitsTest, UnitIsValidAndCompare) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  EXPECT_EQ(1, harp_unit_is_valid(""));
  EXPECT_EQ(1, harp_unit_is_valid("m"));
  EXPECT_EQ(1, harp_unit_is_valid("kg/m^3"));
  EXPECT_EQ(1, harp_unit_is_valid("days since 2000-01-01"));

  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(0, harp_unit_is_valid(nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(0, harp_unit_is_valid("unknown_unit_xyz"));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(0, harp_unit_is_valid("meter @ 100 @ 10"));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
  EXPECT_EQ(0, harp_unit_is_valid("meter since 2000-01-01"));

  // Equivalent units compare equal.
  EXPECT_EQ(0, harp_unit_compare("W", "J/s"));
  EXPECT_NE(0, harp_unit_compare("m", "km"));
  EXPECT_LT(harp_unit_compare("m", "km"), 0);
  EXPECT_GT(harp_unit_compare("km", "m"), 0);

  // Parse failures on either operand return -1 and set harp_errno.
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_unit_compare("bad_unit_a", "m"));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);

  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_unit_compare("m", "bad_unit_b"));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
}

TEST(HarpUnitsTest, UnitConverterScalarArrayAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_unit_converter_delete(nullptr);

  // Error paths in harp_unit_converter_new.
  harp_unit_converter* bad = nullptr;
  EXPECT_EQ(-1, harp_unit_converter_new(nullptr, "m", &bad));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(-1, harp_unit_converter_new("m", "bad_unit_xyz", &bad));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);

  EXPECT_EQ(-1, harp_unit_converter_new("m", "s", &bad));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);

  // Valid converter for scalar float/double and array conversions.
  harp_unit_converter* raw_conv = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_unit_converter_new("degC", "K", &raw_conv));
  ScopedHarpUnitConverter conv(raw_conv);

  EXPECT_THAT(harp_unit_converter_convert_float(conv.get(), 0.0f),
              FloatNear(273.15f, 1e-3f));
  EXPECT_THAT(harp_unit_converter_convert_double(conv.get(), 100.0),
              DoubleNear(373.15, 1e-6));

  // Array conversions via public harp_convert_unit_* helpers.
  float fvals[] = {0.0f, 100.0f};
  ASSERT_EQ(HARP_SUCCESS, harp_convert_unit_float("degC", "K", 2, fvals));
  EXPECT_THAT(
      fvals, ElementsAre(FloatNear(273.15f, 1e-3f), FloatNear(373.15f, 1e-3f)));

  double dvals[] = {1.0, 2.5};
  ASSERT_EQ(HARP_SUCCESS, harp_convert_unit_double("km", "m", 2, dvals));
  EXPECT_THAT(dvals,
              ElementsAre(DoubleNear(1000.0, 1e-9), DoubleNear(2500.0, 1e-9)));

  // Deprecated harp_convert_unit delegates to harp_convert_unit_double.
  ASSERT_EQ(HARP_SUCCESS, harp_convert_unit("m", "cm", 2, dvals));
  EXPECT_THAT(dvals, ElementsAre(DoubleNear(100000.0, 1e-6),
                                 DoubleNear(250000.0, 1e-6)));

  EXPECT_EQ(-1, harp_convert_unit_float("m", "kg", 2, fvals));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);

  EXPECT_EQ(-1, harp_convert_unit_double("m", "kg", 2, dvals));
  EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
}

TEST(HarpUnitsTest, VariableConvertUnitForFloatIntAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const long dims[] = {2};
  const harp_dimension_type dim_types[] = {harp_dimension_time};

  // 1. Float variable keeps float type and scales data + valid_min/valid_max.
  {
    harp_variable* raw_var = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("altitude_f", harp_type_float, 1,
                                              dim_types, dims, &raw_var));
    ScopedHarpVariable var(raw_var);
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), "km"));
    var->data.float_data[0] = 1.0f;
    var->data.float_data[1] = 2.5f;
    var->valid_min.float_data = 0.0f;
    var->valid_max.float_data = 10.0f;

    ASSERT_EQ(HARP_SUCCESS, harp_variable_convert_unit(var.get(), "m"));
    EXPECT_EQ(harp_type_float, var->data_type);
    EXPECT_STREQ("m", var->unit);
    EXPECT_THAT(var->data.float_data[0], FloatNear(1000.0f, 1e-3f));
    EXPECT_THAT(var->data.float_data[1], FloatNear(2500.0f, 1e-3f));
    EXPECT_THAT(var->valid_min.float_data, FloatNear(0.0f, 1e-3f));
    EXPECT_THAT(var->valid_max.float_data, FloatNear(10000.0f, 1e-3f));
  }

  // 2. Integer variable is promoted to double and scaled.
  {
    harp_variable* raw_var = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("altitude_i", harp_type_int32, 1,
                                              dim_types, dims, &raw_var));
    ScopedHarpVariable var(raw_var);
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), "km"));
    var->data.int32_data[0] = 3;
    var->data.int32_data[1] = 5;
    var->valid_min.int32_data = 0;
    var->valid_max.int32_data = 100;

    ASSERT_EQ(HARP_SUCCESS, harp_variable_convert_unit(var.get(), "m"));
    EXPECT_EQ(harp_type_double, var->data_type);
    EXPECT_STREQ("m", var->unit);
    EXPECT_THAT(var->data.double_data[0], DoubleNear(3000.0, 1e-9));
    EXPECT_THAT(var->data.double_data[1], DoubleNear(5000.0, 1e-9));
    EXPECT_THAT(var->valid_min.double_data, DoubleNear(0.0, 1e-9));
    EXPECT_THAT(var->valid_max.double_data, DoubleNear(100000.0, 1e-9));
  }

  // 3. Incompatible target unit appends variable name to error message.
  {
    harp_variable* raw_var = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("bad_target", harp_type_double, 1,
                                              dim_types, dims, &raw_var));
    ScopedHarpVariable var(raw_var);
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), "m"));
    EXPECT_EQ(-1, harp_variable_convert_unit(var.get(), "kg"));
    EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
  }

  // 4. String variable fails in harp_variable_convert_data_type(..., double).
  {
    harp_variable* raw_var = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("str_var", harp_type_string, 1,
                                              dim_types, dims, &raw_var));
    ScopedHarpVariable var(raw_var);
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var.get(), "m"));
    EXPECT_EQ(-1, harp_variable_convert_unit(var.get(), "km"));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
}

TEST(HarpUnitsTest, HandleUdunitsErrorAllStatusCodes) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const absl::Cleanup restore = [] {
    g_forced_ut_status = -1;
    harp_set_error(HARP_SUCCESS, nullptr);
  };

  constexpr ut_status kStatuses[] = {
      UT_SUCCESS,     UT_BAD_ARG,         UT_EXISTS,       UT_NO_UNIT,
      UT_OS,          UT_NOT_SAME_SYSTEM, UT_MEANINGLESS,  UT_NO_SECOND,
      UT_VISIT_ERROR, UT_CANT_FORMAT,     UT_SYNTAX,       UT_UNKNOWN,
      UT_OPEN_ARG,    UT_OPEN_ENV,        UT_OPEN_DEFAULT, UT_PARSE,
  };
  for (ut_status st : kStatuses) {
    harp_set_error(HARP_SUCCESS, nullptr);
    g_forced_ut_status = static_cast<int>(st);
    EXPECT_EQ(0, harp_unit_is_valid("unknown_unit_xyz"));
    if (st != UT_SUCCESS) {
      EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
    }
  }
}

TEST(HarpUnitsTest, OutOfMemoryPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  ScopedEnvVar restore_env("UDUNITS2_XML_PATH");
  unsetenv("UDUNITS2_XML_PATH");
  const absl::Cleanup cleanup = [] {
    harp_unit_done();
    harp_set_udunits2_xml_path(nullptr);
    harp_set_error(HARP_SUCCESS, nullptr);
  };

  // 1. harp_set_udunits2_xml_path OOM.
  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_set_udunits2_xml_path("/some/oom/udunits2.xml");
            }),
            0);
  harp_set_udunits2_xml_path(nullptr);

  // 2. harp_set_udunits2_xml_path_conditional OOM across path helper calls.
  // Only test step 1 (strdup) with non-NULL searchpath because upstream
  // harp_path_find_file leaks `path` on subsequent realloc OOM
  // (harp-utils.c:238).
  const TempDir dir("units_oom_dir");
  const TempFile marker("units_oom_dir/bin_marker");
  ASSERT_TRUE(WriteFileText(marker.path(), "x"));
  {
    ScopedOomCountdown guard(1);
    EXPECT_EQ(-1, harp_set_udunits2_xml_path_conditional(
                      "bin_marker", dir.c_str(), "../udunits2.xml"));
    EXPECT_TRUE(guard.triggered());
  }
  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_set_udunits2_xml_path_conditional(marker.c_str(), nullptr,
                                                     "../udunits2.xml");
            }),
            0);
  harp_set_udunits2_xml_path(nullptr);

  // 3. unit_system_init OOM (strdup(setlocale(...)) when unit_system == NULL).
  harp_unit_done();
  EXPECT_GT(SweepOomUntilSuccess([&]() { harp_unit_is_valid("m"); }), 0);

  // 4. harp_unit_converter_new OOM.
  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_unit_converter* conv = nullptr;
              if (harp_unit_converter_new("km", "m", &conv) == HARP_SUCCESS) {
                ScopedSuppressOom suppress;
                harp_unit_converter_delete(conv);
              }
            }),
            0);

  // 5. harp_variable_convert_unit OOM (both int32->double and float paths).
  const long dims[] = {2};
  const harp_dimension_type dim_types[] = {harp_dimension_time};
  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_variable* raw = nullptr;
              {
                ScopedSuppressOom suppress;
                if (harp_variable_new("alt_i", harp_type_int32, 1, dim_types,
                                      dims, &raw) != HARP_SUCCESS) {
                  return;
                }
                if (harp_variable_set_unit(raw, "km") != HARP_SUCCESS) {
                  harp_variable_delete(raw);
                  return;
                }
              }
              harp_variable_convert_unit(raw, "m");
              ScopedSuppressOom suppress;
              harp_variable_delete(raw);
            }),
            0);
  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_variable* raw = nullptr;
              {
                ScopedSuppressOom suppress;
                if (harp_variable_new("alt_f", harp_type_float, 1, dim_types,
                                      dims, &raw) != HARP_SUCCESS) {
                  return;
                }
                if (harp_variable_set_unit(raw, "km") != HARP_SUCCESS) {
                  harp_variable_delete(raw);
                  return;
                }
              }
              harp_variable_convert_unit(raw, "m");
              ScopedSuppressOom suppress;
              harp_variable_delete(raw);
            }),
            0);
}

}  // namespace
}  // namespace stcorp_harp
