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
// Tests CODA library initialization, finalization, runtime options, definition
// path resolution, version reporting, and memory deallocation (libcoda/coda.c).
//
// google3 build caveats:
//   - config.h pins CODA_VERSION to "2.19".
//   - HAVE_HDF5 is enabled (1), so coda_init() invokes coda_hdf5_init(), which
//     unconditionally succeeds.
//   - Coverage ceiling on libcoda/coda.c is 120/137 (87.6%) = 100% of
//     reachable non-OOM, non-dead lines: the 17 missed lines (379, 381, 434,
//     441, 450-451, 456-457, 494-495, 504-506, 508, 527, 529-530) are all OOM
//     error paths (malloc/realloc/strdup == NULL) or dead error handling on
//     coda_hdf5_init(), which unconditionally returns 0.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include "third_party/stcorp_coda/libcoda/coda.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

constexpr std::string_view kTestIndexXml = R"(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="TestClass">
  <cd:ProductType name="TestType">
    <cd:ProductDefinition id="TestProd" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">CODATEST</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";

constexpr std::string_view kAltIndexXml = R"(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="AltClass">
  <cd:ProductType name="AltType">
    <cd:ProductDefinition id="AltProd" format="binary" version="2">
      <cd:DetectionRule>
        <cd:MatchData offset="0">CODAALT1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";

// Binary layout: 8-byte "CODATEST" magic followed by 16-bit little-endian raw
// integer converted via (raw * 2 + 10).
constexpr std::string_view kProductXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="TestProd" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="val">
      <cd:Integer>
        <cd:BitSize>16</cd:BitSize>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:LittleEndian/>
        <cd:Conversion numerator="2" denominator="1" offset="10">
          <cd:Unit>m</cd:Unit>
        </cd:Conversion>
      </cd:Integer>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

// "CODATEST" followed by little-endian uint16_t 7 -> converted double is 24.0.
constexpr auto kTestProductBytes = std::to_array<uint8_t>(
    {'C', 'O', 'D', 'A', 'T', 'E', 'S', 'T', 0x07, 0x00});

// "CODAALT1" followed by 2 padding bytes for testing replacement of paths.
constexpr auto kAltProductBytes = std::to_array<uint8_t>(
    {'C', 'O', 'D', 'A', 'A', 'L', 'T', '1', 0x01, 0x00});

std::vector<uint8_t> MakeTestCodadefBytes() {
  const std::array<std::pair<std::string_view, std::string_view>, 1> entries = {
      {{"products/TestProd.xml", kProductXml}},
  };
  return BuildCodadefBytes(kTestIndexXml, entries);
}

std::vector<uint8_t> MakeAltCodadefBytes() {
  return BuildCodadefBytes(kAltIndexXml);
}

TEST(CodaTest, GetLibcodaVersionMatchesConfigVersion) {
  const char* version = coda_get_libcoda_version();
  ASSERT_NE(nullptr, version);
  EXPECT_STREQ("2.19", version);
  EXPECT_STREQ("2.19", libcoda_version);
}

TEST(CodaTest, OptionSettersAndGettersValidateInputsAndPreserveState) {
  const ScopedCodaOptions restore_options;
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  struct OptionCase {
    std::string_view name;
    int (*setter)(int);
    int (*getter)(void);
  };
  constexpr OptionCase kOptionCases[] = {
      {.name = "bypass_special_types",
       .setter = coda_set_option_bypass_special_types,
       .getter = coda_get_option_bypass_special_types},
      {.name = "perform_boundary_checks",
       .setter = coda_set_option_perform_boundary_checks,
       .getter = coda_get_option_perform_boundary_checks},
      {.name = "perform_conversions",
       .setter = coda_set_option_perform_conversions,
       .getter = coda_get_option_perform_conversions},
      {.name = "use_fast_size_expressions",
       .setter = coda_set_option_use_fast_size_expressions,
       .getter = coda_get_option_use_fast_size_expressions},
      {.name = "use_mmap",
       .setter = coda_set_option_use_mmap,
       .getter = coda_get_option_use_mmap},
  };

  for (const OptionCase& tc : kOptionCases) {
    SCOPED_TRACE(tc.name);

    ASSERT_EQ(CODA_SUCCESS, tc.setter(0));
    EXPECT_EQ(0, tc.getter());

    ASSERT_EQ(CODA_SUCCESS, tc.setter(1));
    EXPECT_EQ(1, tc.getter());

    for (const int invalid : {-1, 2, 42}) {
      coda_set_error(CODA_SUCCESS, nullptr);
      EXPECT_EQ(-1, tc.setter(invalid));
      EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
      EXPECT_THAT(coda_errno_to_string(coda_errno),
                  HasSubstr("enable argument"));
      EXPECT_EQ(1, tc.getter());
    }

    ASSERT_EQ(CODA_SUCCESS, tc.setter(0));
    EXPECT_EQ(0, tc.getter());
  }
}

TEST(CodaTest, InitResetsBoundaryChecksAndConversionsOnlyOnFirstInit) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  int init_count = 0;
  const absl::Cleanup cleanup_coda = [&init_count] {
    while (init_count > 0) {
      coda_done();
      --init_count;
    }
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Configure all options away from their defaults before first coda_init().
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_bypass_special_types(1));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_fast_size_expressions(0));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(0));

  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;

  // 2. First coda_init() resets boundary checks and conversions to 1, while
  //    preserving bypass_special_types, use_fast_size_expressions, and
  //    use_mmap.
  EXPECT_EQ(1, coda_get_option_bypass_special_types());
  EXPECT_EQ(1, coda_get_option_perform_boundary_checks());
  EXPECT_EQ(1, coda_get_option_perform_conversions());
  EXPECT_EQ(0, coda_get_option_use_fast_size_expressions());
  EXPECT_EQ(0, coda_get_option_use_mmap());

  // 3. Nested coda_init() (coda_init_counter > 0) does not reset options.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
  EXPECT_EQ(0, coda_get_option_perform_boundary_checks());
  EXPECT_EQ(0, coda_get_option_perform_conversions());

  coda_done();
  --init_count;
  coda_done();
  --init_count;
}

TEST(CodaTest, DoneWhenUninitializedIsNoOpAndInitDoneAreRefcounted) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  int init_count = 0;
  const absl::Cleanup cleanup_coda = [&init_count] {
    while (init_count > 0) {
      coda_done();
      --init_count;
    }
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // Calling coda_done() when coda_init_counter == 0 must be a harmless no-op.
  coda_done();

  TempFile codadef_file("coda_test_refcount.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(MakeTestCodadefBytes()));
  TempFile product_file("coda_test_refcount.dat");
  ASSERT_TRUE(product_file.WriteBytes(kTestProductBytes));

  // 1. Nested init increments reference count; inner done leaves dictionary
  //    alive.
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;

  coda_done();
  --init_count;

  int64_t file_size = 0;
  coda_format format = coda_format_ascii;
  const char* product_class = nullptr;
  const char* product_type = nullptr;
  int version = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(product_file.c_str(), &file_size, &format,
                                &product_class, &product_type, &version));
  EXPECT_EQ(10, file_size);
  EXPECT_EQ(coda_format_binary, format);
  EXPECT_STREQ("TestClass", product_class);
  EXPECT_STREQ("TestType", product_type);
  EXPECT_EQ(1, version);

  // 2. Final coda_done() tears down the dictionary and clears
  //    coda_definition_path; an extra coda_done() is a no-op.
  coda_done();
  --init_count;
  coda_done();

  // 3. Re-initializing without setting a path no longer recognizes TestClass.
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(product_file.c_str(), &file_size, &format,
                                &product_class, &product_type, &version));
  EXPECT_EQ(coda_format_binary, format);
  EXPECT_EQ(nullptr, product_class);
  EXPECT_EQ(nullptr, product_type);
  EXPECT_EQ(-1, version);
}

TEST(CodaTest, SetDefinitionPathReplacesAndClearsPreviousPath) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  int init_count = 0;
  const absl::Cleanup cleanup_coda = [&init_count] {
    while (init_count > 0) {
      coda_done();
      --init_count;
    }
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };
  ScopedProduct product;

  TempFile test_codadef("coda_test_path_a.codadef");
  ASSERT_TRUE(test_codadef.WriteBytes(MakeTestCodadefBytes()));
  TempFile alt_codadef("coda_test_path_b.codadef");
  ASSERT_TRUE(alt_codadef.WriteBytes(MakeAltCodadefBytes()));
  TempFile test_prod("coda_test_path_a.dat");
  ASSERT_TRUE(test_prod.WriteBytes(kTestProductBytes));
  TempFile alt_prod("coda_test_path_b.dat");
  ASSERT_TRUE(alt_prod.WriteBytes(kAltProductBytes));

  // 1. Passing NULL when already NULL succeeds; replacing and clearing frees
  //    the previously duplicated path string.
  EXPECT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  EXPECT_EQ(CODA_SUCCESS, coda_set_definition_path(test_codadef.c_str()));
  EXPECT_EQ(CODA_SUCCESS, coda_set_definition_path(alt_codadef.c_str()));
  EXPECT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));

  // 2. After clearing with NULL, neither definition is loaded on init.
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
  const char* product_class = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(alt_prod.c_str(), nullptr, nullptr,
                                &product_class, nullptr, nullptr));
  EXPECT_EQ(nullptr, product_class);
  coda_done();
  --init_count;

  // 3. Set test_codadef, then replace it with alt_codadef before init: only
  //    AltClass is loaded.
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(test_codadef.c_str()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(alt_codadef.c_str()));
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;

  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(test_prod.c_str(), nullptr, nullptr,
                                &product_class, nullptr, nullptr));
  EXPECT_EQ(nullptr, product_class);

  const char* product_type = nullptr;
  int version = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(alt_prod.c_str(), nullptr, nullptr,
                                &product_class, &product_type, &version));
  EXPECT_STREQ("AltClass", product_class);
  EXPECT_STREQ("AltType", product_type);
  EXPECT_EQ(2, version);

  // 4. Open test product with test_codadef active and verify definition file.
  product.Reset();
  coda_done();
  --init_count;
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(test_codadef.c_str()));
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
  ASSERT_EQ(CODA_SUCCESS, coda_open(test_prod.c_str(), product.OutParam()));
  const char* def_file = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_definition_file(product.get(), &def_file));
  ASSERT_NE(nullptr, def_file);
  EXPECT_STREQ(test_codadef.c_str(), def_file);
}

TEST(CodaTest, InitLoadsDefinitionsFromEnvVarWhenDefinitionPathIsUnset) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_leap.Unset();

  int init_count = 0;
  const absl::Cleanup cleanup_coda = [&init_count] {
    while (init_count > 0) {
      coda_done();
      --init_count;
    }
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };
  ScopedProduct product;

  TempDir def_dir("coda_test_env_defs");
  const std::filesystem::path codadef_path = def_dir.child("test.codadef");
  ASSERT_TRUE(WriteFileBytes(codadef_path, MakeTestCodadefBytes()));

  TempFile product_file("coda_test_env_prod.dat");
  ASSERT_TRUE(product_file.WriteBytes(kTestProductBytes));

  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  restore_def.Set(def_dir.c_str());

  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;

  ASSERT_EQ(CODA_SUCCESS, coda_open(product_file.c_str(), product.OutParam()));
  const char* product_class = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_class(product.get(), &product_class));
  EXPECT_STREQ("TestClass", product_class);
}

TEST(CodaTest, InitFailsOnMissingLeapSecondTableAndPreservesCounter) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();

  int init_count = 0;
  const absl::Cleanup cleanup_coda = [&init_count] {
    while (init_count > 0) {
      coda_done();
      --init_count;
    }
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempFile missing_leap("coda_test_missing_tai_utc.dat");
  restore_leap.Set(missing_leap.c_str());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_init());
  EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr(missing_leap.string()));

  // Counter was not incremented on failure, so restoring the env var allows
  // a clean first initialization.
  restore_leap.Unset();
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
}

TEST(CodaTest, InitFailsOnCorruptDefinitionFileAndPreservesPathUntilCleared) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  int init_count = 0;
  const absl::Cleanup cleanup_coda = [&init_count] {
    while (init_count > 0) {
      coda_done();
      --init_count;
    }
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempFile corrupt_def("coda_test_corrupt.codadef");
  ASSERT_TRUE(corrupt_def.WriteText("not a valid zip or codadef archive"));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(corrupt_def.c_str()));

  // 1. coda_init() fails in coda_read_definitions() and intentionally leaves
  //    coda_definition_path set.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_init());
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 2. Retrying without clearing coda_definition_path still fails.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_init());
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 3. Explicitly clearing coda_definition_path lets coda_init() succeed.
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
}

TEST(CodaTest, SetDefinitionPathConditionalHandlesEnvSearchpathAndPath) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_leap.Unset();

  int init_count = 0;
  const absl::Cleanup cleanup_coda = [&init_count] {
    while (init_count > 0) {
      coda_done();
      --init_count;
    }
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempDir root_dir("coda_test_conditional_root");
  const std::filesystem::path tool_path = root_dir.child("bin/mytool");
  const std::filesystem::path defs_dir = root_dir.child("defs");
  const std::filesystem::path codadef_path = defs_dir / "test.codadef";
  ASSERT_TRUE(WriteFileText(tool_path, "stub"));
  ASSERT_TRUE(WriteFileBytes(codadef_path, MakeTestCodadefBytes()));

  TempFile product_file("coda_test_conditional_prod.dat");
  ASSERT_TRUE(product_file.WriteBytes(kTestProductBytes));

  const char* product_class = nullptr;

  // 1. When CODA_DEFINITION is set, coda_set_definition_path_conditional() is a
  //    no-op and does not populate coda_definition_path.
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  restore_def.Set(defs_dir.c_str());
  ASSERT_EQ(CODA_SUCCESS,
            coda_set_definition_path_conditional(
                "mytool", tool_path.parent_path().c_str(), "../defs"));
  restore_def.Unset();
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(product_file.c_str(), nullptr, nullptr,
                                &product_class, nullptr, nullptr));
  EXPECT_EQ(nullptr, product_class);
  coda_done();
  --init_count;

  // 2. With CODA_DEFINITION unset and searchpath provided, missing file leaves
  //    coda_definition_path unset.
  const std::string searchpath = (root_dir.child("missing_bin")).native() +
                                 ":" + tool_path.parent_path().native();
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path_conditional(
                              "no_such_tool", searchpath.c_str(), "../defs"));
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(product_file.c_str(), nullptr, nullptr,
                                &product_class, nullptr, nullptr));
  EXPECT_EQ(nullptr, product_class);
  coda_done();
  --init_count;

  // 3. Finding the file in searchpath resolves relative_location and loads the
  //    .codadef directory on coda_init().
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path_conditional(
                              "mytool", searchpath.c_str(), "../defs"));
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(product_file.c_str(), nullptr, nullptr,
                                &product_class, nullptr, nullptr));
  EXPECT_STREQ("TestClass", product_class);
  coda_done();
  --init_count;

  // 4. With searchpath == NULL, a missing program name or NULL leaves
  //    coda_definition_path unset.
  ASSERT_EQ(CODA_SUCCESS,
            coda_set_definition_path_conditional(nullptr, nullptr, "../defs"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_set_definition_path_conditional("coda_nonexistent_program_xyz",
                                                 nullptr, "../defs"));
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(product_file.c_str(), nullptr, nullptr,
                                &product_class, nullptr, nullptr));
  EXPECT_EQ(nullptr, product_class);
  coda_done();
  --init_count;

  // 5. With searchpath == NULL and a program path containing '/',
  //    coda_path_for_program() resolves relative_location to defs/.
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path_conditional(
                              tool_path.c_str(), nullptr, "../defs"));
  ASSERT_EQ(CODA_SUCCESS, coda_init());
  ++init_count;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(product_file.c_str(), nullptr, nullptr,
                                &product_class, nullptr, nullptr));
  EXPECT_STREQ("TestClass", product_class);
}

TEST(CodaTest, OptionUseMmapAndPerformConversionsAffectFileReading) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  TempFile codadef_file("coda_test_options_behavior.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(MakeTestCodadefBytes()));
  TempFile product_file("coda_test_options_behavior.dat");
  ASSERT_TRUE(product_file.WriteBytes(kTestProductBytes));

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Open and read with mmap disabled and conversions enabled (default).
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(0));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(1));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open(product_file.c_str(), product.OutParam()));

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&cursor, "magic"));
    std::array<char, 9> magic{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&cursor, magic.data(),
                                      static_cast<long>(magic.size())));
    EXPECT_STREQ("CODATEST", magic.data());

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&cursor, "val"));
    coda_native_type read_type = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&cursor, &read_type));
    EXPECT_EQ(coda_native_type_double, read_type);
    double converted = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &converted));
    EXPECT_DOUBLE_EQ(24.0, converted);
  }

  // 2. Re-open with mmap enabled and conversions disabled to read raw uint16.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(1));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open(product_file.c_str(), product.OutParam()));

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&cursor, "val"));
    coda_native_type read_type = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&cursor, &read_type));
    EXPECT_EQ(coda_native_type_uint16, read_type);
    uint16_t raw = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&cursor, &raw));
    EXPECT_EQ(7U, raw);
  }
}

TEST(CodaTest, CodaFreeDeallocatesAllocatedStringAndAcceptsNull) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  coda_free(nullptr);

  ScopedExpr expr;
  constexpr char kStringExpr[] = "\"coda_free_ok\"";
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string(kStringExpr, expr.OutParam()));

  char* value = nullptr;
  long length = 0;
  const absl::Cleanup free_value = [&value] {
    if (value != nullptr) {
      coda_free(value);
      value = nullptr;
    }
  };

  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_eval_string(expr.get(), nullptr, &value, &length));
  ASSERT_NE(nullptr, value);
  EXPECT_EQ(12L, length);
  EXPECT_STREQ("coda_free_ok", value);

  coda_free(value);
  value = nullptr;
}

// TODO(schwehr): 87.6% reachable ceiling in libcoda/coda.c (120/137); remaining
// 17 missed lines are OOM, coda_path_* failure, and coda_hdf5_init() error
// branches.

}  // namespace
}  // namespace stcorp_coda
