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
// Tests for common utility routines (apps/commonutils.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/commonutils.cpp

#include "third_party/gdal/apps/commonutils.h"

#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

TEST(CommonUtilsTest, GetOutputDrivers) {
  GDALRegister_GTiff();

  const auto drivers = GetOutputDriversFor("test.tif", GDAL_OF_RASTER);
  EXPECT_THAT(drivers, Contains("GTiff"));

  EXPECT_EQ("GTiff", GetOutputDriverForRaster("test.tif"));

  const auto unknown_drivers = GetOutputDriversFor("test.xyz", GDAL_OF_RASTER);
  EXPECT_THAT(unknown_drivers, IsEmpty());

  {
    WithQuietHandler quiet;
    EXPECT_EQ("", GetOutputDriverForRaster("test.xyz"));
  }
}

TEST(CommonUtilsTest, EarlySetConfigOptionsSuccess) {
  absl::Cleanup cleanup = [] {
    CPLSetConfigOption("CPL_DEBUG", nullptr);
    CPLSetConfigOption("TEST_OPT1", nullptr);
    CPLSetConfigOption("TEST_OPT2", nullptr);
    CPLSetConfigOption("TEST_OPT3", nullptr);
  };

  std::vector<std::string> args = {
      "app",
      "--debug",
      "ON",
      "--config",
      "CPL_DEBUG",
      "VERBOSE",
      "--config",
      "CPL_DEBUG=SUPER_VERBOSE",
      "--config",
      "TEST_OPT1",
      "VAL1",
      "--config",
      "TEST_OPT2=VAL2",
      "--config",
      "TEST_OPT3",
      "VAL3",
  };

  std::vector<char*> argv;
  argv.reserve(args.size());
  for (auto& s : args) {
    argv.push_back(s.data());
  }

  EarlySetConfigOptions(static_cast<int>(argv.size()), argv.data());

  EXPECT_STREQ("SUPER_VERBOSE", CPLGetConfigOption("CPL_DEBUG", nullptr));
  EXPECT_STREQ("VAL1", CPLGetConfigOption("TEST_OPT1", nullptr));
  EXPECT_STREQ("VAL2", CPLGetConfigOption("TEST_OPT2", nullptr));
  EXPECT_STREQ("VAL3", CPLGetConfigOption("TEST_OPT3", nullptr));
}

TEST(CommonUtilsTest, EarlySetConfigOptionsMissingValues) {
  // Case 1: In pass 1, --config without key/value
  {
    std::vector<std::string> args = {"app", "--config"};
    std::vector<char*> argv = {args[0].data(), args[1].data()};
    CPLErrorReset();
    {
      WithQuietHandler quiet;
      EarlySetConfigOptions(static_cast<int>(argv.size()), argv.data());
    }
    // No error because i + 1 < argc is false
    CPLErrorReset();
  }

  // Case 2: In pass 1, --config KEY without value
  {
    std::vector<std::string> args = {"app", "--config", "SOME_KEY"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data()};
    CPLErrorReset();
    {
      WithQuietHandler quiet;
      EarlySetConfigOptions(static_cast<int>(argv.size()), argv.data());
    }
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("--config option given without"));
    CPLErrorReset();
  }

  // Case 3: In pass 2, --config KEY without value (when preceded by a handled
  // pass 1 option)
  {
    absl::Cleanup reset_debug = [] {
      CPLSetConfigOption("CPL_DEBUG", nullptr);
    };
    std::vector<std::string> args = {"app", "--debug", "ON", "--config",
                                     "SOME_KEY"};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& s : args) {
      argv.push_back(s.data());
    }
    CPLErrorReset();
    {
      WithQuietHandler quiet;
      EarlySetConfigOptions(static_cast<int>(argv.size()), argv.data());
    }
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("--config option given without"));
    CPLErrorReset();
  }
}

TEST(CommonUtilsTest, RemoveBOM) {
  // 1. Buffer with BOM
  char with_bom[] = "\xEF\xBB\xBFHello World";
  GDALRemoveBOM(reinterpret_cast<GByte*>(with_bom));
  EXPECT_STREQ("Hello World", with_bom);

  // 2. Buffer without BOM
  char no_bom[] = "Hello World";
  GDALRemoveBOM(reinterpret_cast<GByte*>(no_bom));
  EXPECT_STREQ("Hello World", no_bom);

  // 3. BOM only
  char bom_only[] = "\xEF\xBB\xBF";
  GDALRemoveBOM(reinterpret_cast<GByte*>(bom_only));
  EXPECT_STREQ("", bom_only);
}

TEST(CommonUtilsTest, ArgIsNumeric) {
  struct TestCase {
    const char* str;
    int expected;
  };
  constexpr TestCase kCases[] = {
      {.str = "123", .expected = 1},     {.str = "-456", .expected = 1},
      {.str = "3.14159", .expected = 1}, {.str = "1e-4", .expected = 1},
      {.str = "abc", .expected = 0},     {.str = "123a", .expected = 0},
      {.str = "", .expected = 0},
  };
  for (const auto& [str, expected] : kCases) {
    EXPECT_EQ(expected, ArgIsNumeric(str));
  }
}

TEST(CommonUtilsTest, PatternMatch) {
  struct TestCase {
    const char* pattern;
    const char* target;
    bool expected;
  };
  constexpr TestCase kCases[] = {
      // Empty patterns
      {.pattern = "", .target = "", .expected = true},
      {.pattern = "*", .target = "", .expected = true},
      {.pattern = "a", .target = "", .expected = false},
      {.pattern = "", .target = "a", .expected = false},
      // Exact & case insensitive
      {.pattern = "abc", .target = "abc", .expected = true},
      {.pattern = "abc", .target = "ABC", .expected = true},
      {.pattern = "ABC", .target = "abc", .expected = true},
      {.pattern = "abd", .target = "abc", .expected = false},
      // Wildcard '?' ASCII
      {.pattern = "c?t", .target = "cat", .expected = true},
      {.pattern = "c?t", .target = "cart", .expected = false},
      // Wildcard '?' with multi-byte UTF-8 character (e.g. é = \xC3\xA9)
      {.pattern = "c?t", .target = "c\xC3\xA9t", .expected = true},
      {.pattern = "c?x", .target = "c\xC3\xA9t", .expected = false},
      // Wildcard '*' tests
      {.pattern = "*", .target = "foobar", .expected = true},
      {.pattern = "foo*", .target = "foobar", .expected = true},
      {.pattern = "*bar", .target = "foobar", .expected = true},
      {.pattern = "f*r", .target = "foobar", .expected = true},
      {.pattern = "f*o*b*a*r*", .target = "foobar", .expected = true},
      {.pattern = "f*z", .target = "foobar", .expected = false},
      {.pattern = "foobarbaz", .target = "foobar", .expected = false},
  };
  for (const auto& [pattern, target, expected] : kCases) {
    EXPECT_EQ(expected, GDALPatternMatch(target, pattern));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/commonutils.cpp.
