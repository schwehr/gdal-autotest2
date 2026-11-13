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
// Tests CODA utility functions: format parsing, XML/identifier helpers,
// C-to-Fortran array indexing, path resolution/cleaning, IEEE 754 float
// predicates/constants, and 64-bit/float string formatting
// (libcoda/coda-utils.c).
//
// google3 build caveats:
//   - Built for Linux (WIN32 branches in coda_path_find_file() and
//     coda_path_for_program() are compiled out).
//   - Internal OOM allocation-failure branches and getcwd() failure paths are
//     not fault-injected.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-utils.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-path.h

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>  // NOLINT
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/libcoda/coda-path.h"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

struct FreeDeleter {
  void operator()(void* ptr) const { std::free(ptr); }
};

using ScopedMallocString = std::unique_ptr<char, FreeDeleter>;

TEST(CodaUtilsTest, FormatFromStringParsesAllKnownFormatsAndRejectsInvalid) {
  struct FormatCase {
    std::string_view name;
    coda_format expected;
  };
  constexpr FormatCase kValidCases[] = {
      {.name = "ascii", .expected = coda_format_ascii},
      {.name = "binary", .expected = coda_format_binary},
      {.name = "xml", .expected = coda_format_xml},
      {.name = "hdf4", .expected = coda_format_hdf4},
      {.name = "hdf5", .expected = coda_format_hdf5},
      {.name = "cdf", .expected = coda_format_cdf},
      {.name = "netcdf", .expected = coda_format_netcdf},
      {.name = "grib", .expected = coda_format_grib},
      {.name = "rinex", .expected = coda_format_rinex},
      {.name = "sp3", .expected = coda_format_sp3},
  };

  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  for (const FormatCase& tc : kValidCases) {
    coda_format actual = coda_format_sp3;
    const std::string name_str(tc.name);
    EXPECT_EQ(CODA_SUCCESS, coda_format_from_string(name_str.c_str(), &actual))
        << "name=" << tc.name;
    EXPECT_EQ(tc.expected, actual) << "name=" << tc.name;
  }

  constexpr std::string_view kInvalidNames[] = {"unknown", "", "ASCII", "hdf"};
  for (const std::string_view invalid : kInvalidNames) {
    coda_format actual = coda_format_ascii;
    coda_set_error(CODA_SUCCESS, nullptr);
    const std::string invalid_str(invalid);
    EXPECT_EQ(-1, coda_format_from_string(invalid_str.c_str(), &actual));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
    EXPECT_THAT(coda_errno_to_string(CODA_ERROR_DATA_DEFINITION),
                HasSubstr("invalid 'format' attribute value"));
    EXPECT_EQ(coda_format_ascii, actual);
  }
}

TEST(CodaUtilsTest, ElementNameFromXmlNameExtractsOptionalNamespace) {
  constexpr char kPlain[] = "measurement";
  EXPECT_EQ(kPlain, coda_element_name_from_xml_name(kPlain));
  EXPECT_STREQ("measurement", coda_element_name_from_xml_name(kPlain));

  constexpr char kEmpty[] = "";
  EXPECT_EQ(kEmpty, coda_element_name_from_xml_name(kEmpty));
  EXPECT_STREQ("", coda_element_name_from_xml_name(kEmpty));

  constexpr char kNamespaced[] = "http://example.org/ns measurement";
  EXPECT_EQ(kNamespaced + 22, coda_element_name_from_xml_name(kNamespaced));
  EXPECT_STREQ("measurement", coda_element_name_from_xml_name(kNamespaced));

  constexpr char kTrailingSpace[] = "ns ";
  EXPECT_STREQ("", coda_element_name_from_xml_name(kTrailingSpace));

  constexpr char kMultipleSpaces[] = "ns elem extra";
  EXPECT_STREQ("elem extra", coda_element_name_from_xml_name(kMultipleSpaces));
}

TEST(CodaUtilsTest, IsIdentifierValidatesLeadingAlphaAndBodyChars) {
  EXPECT_EQ(0, coda_is_identifier(nullptr));

  struct IdentifierCase {
    std::string_view input;
    int expected;
  };
  constexpr IdentifierCase kCases[] = {
      {.input = "", .expected = 0},
      {.input = "1abc", .expected = 0},
      {.input = "_abc", .expected = 0},
      {.input = " abc", .expected = 0},
      {.input = "a-b", .expected = 0},
      {.input = "a.b", .expected = 0},
      {.input = "a b", .expected = 0},
      {.input = "a", .expected = 1},
      {.input = "Z", .expected = 1},
      {.input = "abc_123", .expected = 1},
      {.input = "Field_09_", .expected = 1},
  };

  for (const IdentifierCase& tc : kCases) {
    const std::string input_str(tc.input);
    EXPECT_EQ(tc.expected, coda_is_identifier(input_str.c_str()))
        << "input=" << tc.input;
  }
}

TEST(CodaUtilsTest, IdentifierFromNameNormalizesAndDeduplicates) {
  {
    const ScopedMallocString id(coda_identifier_from_name(nullptr, nullptr));
    ASSERT_NE(nullptr, id.get());
    EXPECT_STREQ("unnamed", id.get());
  }

  struct NormalizeCase {
    std::string_view input;
    std::string_view expected;
  };
  constexpr NormalizeCase kCases[] = {
      {.input = "", .expected = "unnamed"},
      {.input = "123_!@#", .expected = "unnamed"},
      {.input = "123abc", .expected = "abc"},
      {.input = "__foo-bar.baz 1", .expected = "foo_bar_baz_1"},
      {.input = "Valid_Name9", .expected = "Valid_Name9"},
  };
  for (const NormalizeCase& tc : kCases) {
    const std::string input_str(tc.input);
    const ScopedMallocString id(
        coda_identifier_from_name(input_str.c_str(), nullptr));
    ASSERT_NE(nullptr, id.get()) << "input=" << tc.input;
    EXPECT_EQ(tc.expected, std::string_view(id.get())) << "input=" << tc.input;
  }

  // Keep inserted keys alive at stable addresses until after ScopedHashtable
  // destroys the table.
  std::deque<std::string> inserted_keys;
  ScopedHashtable ht(coda_hashtable_new(/*case_sensitive=*/1));
  ASSERT_NE(nullptr, ht.get());

  const ScopedMallocString first(
      coda_identifier_from_name("alpha-1", ht.get()));
  ASSERT_NE(nullptr, first.get());
  EXPECT_STREQ("alpha_1", first.get());
  inserted_keys.emplace_back(first.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_hashtable_add_name(ht.get(), inserted_keys.back().c_str()));

  const ScopedMallocString second(
      coda_identifier_from_name("alpha-1", ht.get()));
  ASSERT_NE(nullptr, second.get());
  EXPECT_STREQ("alpha_1_1", second.get());
  inserted_keys.emplace_back(second.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_hashtable_add_name(ht.get(), inserted_keys.back().c_str()));

  const ScopedMallocString third(
      coda_identifier_from_name("alpha.1", ht.get()));
  ASSERT_NE(nullptr, third.get());
  EXPECT_STREQ("alpha_1_2", third.get());
}

TEST(CodaUtilsTest, CIndexToFortranIndexConvertsDimensionsAndRejectsOverflow) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  EXPECT_EQ(0L, coda_c_index_to_fortran_index(0, nullptr, 0));

  constexpr long kDims1D[] = {5};
  EXPECT_EQ(3L, coda_c_index_to_fortran_index(1, kDims1D, 3));

  // Matches documented example: (3, 4) array, subscript (0, 2) -> C index 2,
  // Fortran index 6.
  constexpr long kDims2D[] = {3, 4};
  EXPECT_EQ(0L, coda_c_index_to_fortran_index(2, kDims2D, 0));
  EXPECT_EQ(6L, coda_c_index_to_fortran_index(2, kDims2D, 2));
  EXPECT_EQ(1L, coda_c_index_to_fortran_index(2, kDims2D, 4));
  EXPECT_EQ(11L, coda_c_index_to_fortran_index(2, kDims2D, 11));

  // Exhaustively verify a 3D (2 x 3 x 4) array.
  constexpr long kDims3D[] = {2, 3, 4};
  for (long i = 0; i < 2; ++i) {
    for (long j = 0; j < 3; ++j) {
      for (long k = 0; k < 4; ++k) {
        const long c_index = (i * 3 + j) * 4 + k;
        const long expected_f_index = i + 2 * (j + 3 * k);
        EXPECT_EQ(expected_f_index,
                  coda_c_index_to_fortran_index(3, kDims3D, c_index));
      }
    }
  }

  // Maximum allowed dimensions (CODA_MAX_NUM_DIMS == 8).
  constexpr std::array<long, CODA_MAX_NUM_DIMS> kDimsMax = {2, 2, 2, 2,
                                                            2, 2, 2, 2};
  EXPECT_EQ(128L, coda_c_index_to_fortran_index(CODA_MAX_NUM_DIMS,
                                                kDimsMax.data(), 1));

  // Exceeding CODA_MAX_NUM_DIMS fails with CODA_ERROR_INVALID_ARGUMENT.
  constexpr std::array<long, CODA_MAX_NUM_DIMS + 1> kDimsTooMany = {
      2, 2, 2, 2, 2, 2, 2, 2, 2};
  EXPECT_EQ(-1L, coda_c_index_to_fortran_index(CODA_MAX_NUM_DIMS + 1,
                                               kDimsTooMany.data(), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

TEST(CodaUtilsTest, PathFromPathNormalizesAndResolvesSegments) {
  struct PathCase {
    const char* initial;
    int is_filepath;
    const char* append;
    std::string_view expected;
  };
  constexpr PathCase kCases[] = {
      {.initial = "/usr/local/bin/foo",
       .is_filepath = 1,
       .append = "../share/foo/doc",
       .expected = "/usr/local/share/foo/doc"},
      {.initial = "foo", .is_filepath = 1, .append = "bar", .expected = "bar"},
      {.initial = "foo", .is_filepath = 1, .append = nullptr, .expected = ""},
      {.initial = "",
       .is_filepath = 0,
       .append = "rel/file",
       .expected = "rel/file"},
      {.initial = "/a/b", .is_filepath = 0, .append = "", .expected = "/a/b"},
      {.initial = "././a/b",
       .is_filepath = 0,
       .append = nullptr,
       .expected = "a/b"},
      {.initial = "./", .is_filepath = 0, .append = nullptr, .expected = "."},
      {.initial = "/.", .is_filepath = 0, .append = nullptr, .expected = "."},
      {.initial = "/a/..",
       .is_filepath = 0,
       .append = nullptr,
       .expected = "."},
      {.initial = "/a/..//b/./c/",
       .is_filepath = 0,
       .append = nullptr,
       .expected = "/b/c/"},
      {.initial = "a/b/..",
       .is_filepath = 0,
       .append = nullptr,
       .expected = "a"},
      {.initial = "a/..",
       .is_filepath = 0,
       .append = nullptr,
       .expected = "a/.."},
      {.initial = "../..",
       .is_filepath = 0,
       .append = nullptr,
       .expected = "../.."},
      {.initial = "a/../..",
       .is_filepath = 0,
       .append = nullptr,
       .expected = "a/../.."},
      {.initial = "a\\b\\file",
       .is_filepath = 1,
       .append = "..\\c",
       .expected = "a\\c"},
      {.initial = "a\\\\b\\.\\c\\..",
       .is_filepath = 0,
       .append = nullptr,
       .expected = "a\\b"},
      {.initial = "a\\..\\..",
       .is_filepath = 0,
       .append = nullptr,
       .expected = "a\\..\\.."},
  };

  for (const PathCase& tc : kCases) {
    char* raw_result = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_path_from_path(tc.initial, tc.is_filepath,
                                                tc.append, &raw_result));
    const ScopedMallocString result(raw_result);
    ASSERT_NE(nullptr, result.get());
    EXPECT_EQ(tc.expected, std::string_view(result.get()))
        << "initial=" << tc.initial;
  }
}

TEST(CodaUtilsTest, PathFindFileSearchesColonSeparatedPathsAndIgnoresDirs) {
  char* raw_location = nullptr;
  EXPECT_EQ(CODA_SUCCESS,
            coda_path_find_file(nullptr, "file.txt", &raw_location));
  EXPECT_EQ(nullptr, raw_location);

  EXPECT_EQ(CODA_SUCCESS, coda_path_find_file("", "file.txt", &raw_location));
  EXPECT_EQ(nullptr, raw_location);

  const TempDir root("coda_utils_find_file");
  const std::filesystem::path short_dir = root.child("d1");
  const std::filesystem::path long_dir =
      root.child("second_directory_with_longer_name_for_realloc");
  std::error_code ec;
  std::filesystem::create_directories(short_dir / "target.dat", ec);
  ASSERT_FALSE(ec);
  std::filesystem::create_directories(long_dir, ec);
  ASSERT_FALSE(ec);

  const std::filesystem::path target_file = long_dir / "target.dat";
  ASSERT_TRUE(WriteFileText(target_file, "ok"));

  const std::string search_path =
      absl::StrCat(short_dir.string(), ":", long_dir.string());

  // 1. short_dir/target.dat is a directory (not S_IFREG), so search advances
  //    to long_dir (triggering buffer realloc) and finds the regular file.
  ASSERT_EQ(CODA_SUCCESS, coda_path_find_file(search_path.c_str(), "target.dat",
                                              &raw_location));
  const ScopedMallocString found(raw_location);
  ASSERT_NE(nullptr, found.get());
  EXPECT_EQ(target_file.string(), std::string_view(found.get()));

  // 2. Missing file returns CODA_SUCCESS with NULL location.
  raw_location = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_path_find_file(search_path.c_str(),
                                              "missing.dat", &raw_location));
  EXPECT_EQ(nullptr, raw_location);
}

TEST(CodaUtilsTest, PathForProgramHandlesNullAbsoluteRelativeAndPathEnv) {
  char* raw_location = nullptr;
  EXPECT_EQ(CODA_SUCCESS, coda_path_for_program(nullptr, &raw_location));
  EXPECT_EQ(nullptr, raw_location);

  // 1. Absolute path is duplicated as-is.
  ASSERT_EQ(CODA_SUCCESS,
            coda_path_for_program("/usr/bin/coda_tool", &raw_location));
  {
    const ScopedMallocString loc(raw_location);
    ASSERT_NE(nullptr, loc.get());
    EXPECT_STREQ("/usr/bin/coda_tool", loc.get());
  }

  // 2. Windows drive-prefixed path with backslash skips getcwd() expansion.
  raw_location = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_path_for_program("C:\\tools\\coda.exe", &raw_location));
  {
    const ScopedMallocString loc(raw_location);
    ASSERT_NE(nullptr, loc.get());
    EXPECT_STREQ("C:\\tools\\coda.exe", loc.get());
  }

  // 3. Relative path containing '/' is resolved against getcwd().
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  ASSERT_FALSE(ec);
  raw_location = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_path_for_program("bin/coda_tool", &raw_location));
  {
    const ScopedMallocString loc(raw_location);
    ASSERT_NE(nullptr, loc.get());
    EXPECT_EQ((cwd / "bin/coda_tool").string(), std::string_view(loc.get()));
  }

  // 4. Save and restore PATH before testing bare program names.
  const char* orig_path_env = std::getenv("PATH");
  const std::optional<std::string> saved_path =
      orig_path_env != nullptr ? std::optional<std::string>(orig_path_env)
                               : std::nullopt;
  const absl::Cleanup restore_path = [&saved_path] {
    if (saved_path.has_value()) {
      setenv("PATH", saved_path->c_str(), /*replace=*/1);
    } else {
      unsetenv("PATH");
    }
  };

  // Unset PATH yields NULL location.
  unsetenv("PATH");
  raw_location = nullptr;
  EXPECT_EQ(CODA_SUCCESS, coda_path_for_program("coda_tool", &raw_location));
  EXPECT_EQ(nullptr, raw_location);

  // Program present in a TempDir on PATH is resolved.
  const TempDir bin_dir("coda_utils_prog_path");
  const std::filesystem::path prog = bin_dir.child("coda_synthetic_tool");
  ASSERT_TRUE(WriteFileText(prog, "#!/bin/sh\n"));
  ASSERT_EQ(0, setenv("PATH", bin_dir.c_str(), /*replace=*/1));

  raw_location = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_path_for_program("coda_synthetic_tool", &raw_location));
  {
    const ScopedMallocString loc(raw_location);
    ASSERT_NE(nullptr, loc.get());
    EXPECT_EQ(prog.string(), std::string_view(loc.get()));
  }

  raw_location = nullptr;
  EXPECT_EQ(CODA_SUCCESS,
            coda_path_for_program("coda_absent_tool", &raw_location));
  EXPECT_EQ(nullptr, raw_location);
}

TEST(CodaUtilsTest, SpecialFloatConstantsAndPredicatesMatchIeee754Bits) {
  const double nan_val = coda_NaN();
  const double pos_inf = coda_PlusInf();
  const double neg_inf = coda_MinInf();

  EXPECT_EQ(uint64_t{0x7ff8000000000000ULL}, std::bit_cast<uint64_t>(nan_val));
  EXPECT_EQ(uint64_t{0x7ff0000000000000ULL}, std::bit_cast<uint64_t>(pos_inf));
  EXPECT_EQ(uint64_t{0xfff0000000000000ULL}, std::bit_cast<uint64_t>(neg_inf));

  EXPECT_EQ(1, coda_isNaN(nan_val));
  EXPECT_EQ(1, coda_isNaN(std::numeric_limits<double>::quiet_NaN()));
  // Signaling NaN and negative-sign NaN payloads.
  EXPECT_EQ(1,
            coda_isNaN(std::bit_cast<double>(uint64_t{0x7ff0000000000001ULL})));
  EXPECT_EQ(1,
            coda_isNaN(std::bit_cast<double>(uint64_t{0xfff8000000000000ULL})));
  EXPECT_EQ(0, coda_isNaN(0.0));
  EXPECT_EQ(0, coda_isNaN(1.5));
  EXPECT_EQ(0, coda_isNaN(pos_inf));
  EXPECT_EQ(0, coda_isNaN(neg_inf));

  EXPECT_EQ(1, coda_isPlusInf(pos_inf));
  EXPECT_EQ(0, coda_isMinInf(pos_inf));
  EXPECT_EQ(1, coda_isInf(pos_inf));

  EXPECT_EQ(0, coda_isPlusInf(neg_inf));
  EXPECT_EQ(1, coda_isMinInf(neg_inf));
  EXPECT_EQ(1, coda_isInf(neg_inf));

  EXPECT_EQ(0, coda_isInf(0.0));
  EXPECT_EQ(0, coda_isInf(-123.456));
  EXPECT_EQ(0, coda_isInf(nan_val));
}

TEST(CodaUtilsTest, Str64AndStr64uFormatBoundaryValues) {
  struct SignedCase {
    int64_t value;
    std::string_view expected;
  };
  constexpr SignedCase kSignedCases[] = {
      {.value = 0, .expected = "0"},
      {.value = -1, .expected = "-1"},
      {.value = 42, .expected = "42"},
      {.value = -42, .expected = "-42"},
      {.value = 4294967295LL, .expected = "4294967295"},
      {.value = 4294967296LL, .expected = "4294967296"},
      {.value = -4294967296LL, .expected = "-4294967296"},
      {.value = std::numeric_limits<int64_t>::max(),
       .expected = "9223372036854775807"},
      {.value = std::numeric_limits<int64_t>::min(),
       .expected = "-9223372036854775808"},
  };
  for (const SignedCase& tc : kSignedCases) {
    std::array<char, 21> buf{};
    buf.fill('?');
    coda_str64(tc.value, buf.data());
    EXPECT_EQ(tc.expected, std::string_view(buf.data()))
        << "value=" << tc.value;
  }

  struct UnsignedCase {
    uint64_t value;
    std::string_view expected;
  };
  constexpr UnsignedCase kUnsignedCases[] = {
      {.value = 0ULL, .expected = "0"},
      {.value = 4294967295ULL, .expected = "4294967295"},
      {.value = 4294967296ULL, .expected = "4294967296"},
      {.value = 100000005ULL, .expected = "100000005"},
      {.value = 10000000000000000ULL, .expected = "10000000000000000"},
      {.value = 10000000100000005ULL, .expected = "10000000100000005"},
      {.value = std::numeric_limits<uint64_t>::max(),
       .expected = "18446744073709551615"},
  };
  // UINT64_MAX has 20 digits plus NUL (21 bytes; upstream docstring says 20).
  for (const UnsignedCase& tc : kUnsignedCases) {
    std::array<char, 21> buf{};
    buf.fill('?');
    coda_str64u(tc.value, buf.data());
    EXPECT_EQ(tc.expected, std::string_view(buf.data()))
        << "value=" << tc.value;
  }
}

TEST(CodaUtilsTest, StrflFormatsSpecialAndFiniteDoubles) {
  std::array<char, 24> buf{};

  buf.fill('?');
  coda_strfl(coda_NaN(), buf.data());
  EXPECT_STREQ("nan", buf.data());

  buf.fill('?');
  coda_strfl(coda_PlusInf(), buf.data());
  EXPECT_STREQ("+inf", buf.data());

  buf.fill('?');
  coda_strfl(coda_MinInf(), buf.data());
  EXPECT_STREQ("-inf", buf.data());

  struct FiniteCase {
    double value;
    std::string_view expected;
  };
  constexpr FiniteCase kFiniteCases[] = {
      {.value = 0.0, .expected = "0"},
      {.value = 1.5, .expected = "1.5"},
      {.value = -123.456, .expected = "-123.456"},
      {.value = 1e-10, .expected = "1e-10"},
      {.value = 1234567890123456.0, .expected = "1234567890123456"},
  };
  for (const FiniteCase& tc : kFiniteCases) {
    buf.fill('?');
    coda_strfl(tc.value, buf.data());
    EXPECT_EQ(tc.expected, std::string_view(buf.data()))
        << "value=" << tc.value;
  }
}

// TODO(schwehr): 95.1% line coverage in libcoda/coda-utils.c (350/368);
// remaining 18 missed lines are malloc/realloc/fopen OOM and path-resolution
// error branches.

}  // namespace
}  // namespace stcorp_coda
