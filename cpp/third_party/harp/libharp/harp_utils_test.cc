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
// Unit tests for libharp utility functions (libharp/harp-utils.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-utils.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-internal.h

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;

struct DataTypeCase {
  harp_data_type data_type;
  std::string_view expected_name;
  long expected_size;
};

constexpr DataTypeCase kDataTypeCases[] = {
    {.data_type = harp_type_int8, .expected_name = "int8", .expected_size = 1},
    {.data_type = harp_type_int16,
     .expected_name = "int16",
     .expected_size = 2},
    {.data_type = harp_type_int32,
     .expected_name = "int32",
     .expected_size = 4},
    {.data_type = harp_type_float,
     .expected_name = "float",
     .expected_size = 4},
    {.data_type = harp_type_double,
     .expected_name = "double",
     .expected_size = 8},
    {.data_type = harp_type_string,
     .expected_name = "string",
     .expected_size = static_cast<long>(sizeof(char*))},
};

TEST(HarpUtilsTest, IdentifierAndWildcardMatching) {
  EXPECT_EQ(0, harp_is_identifier(nullptr));
  EXPECT_EQ(0, harp_is_identifier(""));
  EXPECT_EQ(0, harp_is_identifier("1var"));
  EXPECT_EQ(0, harp_is_identifier("_var"));
  EXPECT_EQ(0, harp_is_identifier("var-name"));
  EXPECT_EQ(1, harp_is_identifier("a"));
  EXPECT_EQ(1, harp_is_identifier("O3_column_number_density_1"));

  EXPECT_EQ(1, harp_match_wildcard("", ""));
  EXPECT_EQ(1, harp_match_wildcard("*", ""));
  EXPECT_EQ(1, harp_match_wildcard("*", "anything"));
  EXPECT_EQ(1, harp_match_wildcard("O3_*_density", "O3_column_density"));
  EXPECT_EQ(1, harp_match_wildcard("a?c*e", "abcde"));
  EXPECT_EQ(0, harp_match_wildcard("?", ""));
  EXPECT_EQ(0, harp_match_wildcard("*a", ""));
  EXPECT_EQ(1, harp_match_wildcard("*", "a"));
  EXPECT_EQ(0, harp_match_wildcard("a?c", "ac"));
  EXPECT_EQ(0, harp_match_wildcard("abc", "abd"));
  EXPECT_EQ(0, harp_match_wildcard("abc*", "ab"));
}

TEST(HarpUtilsTest, BasenameAndPathHelpers) {
  EXPECT_EQ(nullptr, harp_basename(nullptr));
  EXPECT_STREQ("", harp_basename(""));
  EXPECT_STREQ("file.nc", harp_basename("file.nc"));
  EXPECT_STREQ("file.nc", harp_basename("/a/b/file.nc"));
  EXPECT_STREQ("", harp_basename("/a/b/"));

  // harp_path_from_path exercises clean_path normalization branches.
  struct PathCase {
    const char* initial;
    int is_filepath;
    const char* append;
    std::string_view expected;
  };
  constexpr PathCase kPathCases[] = {
      {.initial = "./a//b/./c",
       .is_filepath = 0,
       .append = "../d",
       .expected = "a/b/d"},
      {.initial = "/dir/sub/file.txt",
       .is_filepath = 1,
       .append = "../../other",
       .expected = "/other"},
      {.initial = "a/b",
       .is_filepath = 0,
       .append = "..\\d",
       .expected = "a\\d"},
      {.initial = "./", .is_filepath = 0, .append = nullptr, .expected = "."},
      {.initial = "", .is_filepath = 0, .append = "", .expected = ""},
      {.initial = "../..",
       .is_filepath = 0,
       .append = "..",
       .expected = "../../.."},
      {.initial = "", .is_filepath = 1, .append = "only", .expected = "only"},
  };
  for (const auto& tc : kPathCases) {
    char* raw_out = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_path_from_path(tc.initial, tc.is_filepath,
                                                tc.append, &raw_out));
    ScopedMallocChar out(raw_out);
    EXPECT_EQ(tc.expected, out.get());
  }

  // harp_path_find_file with empty/null searchpath, directory collision, and
  // realloc growth across longer path components.
  char* raw_loc = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_path_find_file(nullptr, "a", &raw_loc));
  EXPECT_EQ(nullptr, raw_loc);
  ASSERT_EQ(HARP_SUCCESS, harp_path_find_file("", "a", &raw_loc));
  EXPECT_EQ(nullptr, raw_loc);

  const TempDir short_dir("p1");
  const TempDir sub_as_file("p1/target.dat");
  const TempDir long_dir("much_longer_directory_name_for_realloc");
  const TempFile target("much_longer_directory_name_for_realloc/target.dat");
  ASSERT_TRUE(WriteFileText(target.path(), "ok"));

  const std::string searchpath = short_dir.string() + ":" + long_dir.string();
  ASSERT_EQ(HARP_SUCCESS,
            harp_path_find_file(searchpath.c_str(), "target.dat", &raw_loc));
  ScopedMallocChar found(raw_loc);
  ASSERT_NE(nullptr, found.get());
  EXPECT_EQ(target.string(), found.get());

  char* raw_missing = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_path_find_file(searchpath.c_str(), "absent.dat",
                                              &raw_missing));
  EXPECT_EQ(nullptr, raw_missing);
}

TEST(HarpUtilsTest, PathForProgramWithAndWithoutPathEnv) {
  const char* prev_path = std::getenv("PATH");
  std::optional<std::string> saved_path;
  if (prev_path != nullptr) {
    saved_path = prev_path;
  }
  auto restore_path = absl::MakeCleanup([&] {
    if (saved_path.has_value()) {
      setenv("PATH", saved_path->c_str(), /*overwrite=*/1);
    } else {
      unsetenv("PATH");
    }
  });

  char* raw_loc = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_path_for_program(nullptr, &raw_loc));
  EXPECT_EQ(nullptr, raw_loc);

  // Absolute path with '/' returns copy directly.
  ASSERT_EQ(HARP_SUCCESS, harp_path_for_program("/usr/bin/tool", &raw_loc));
  {
    ScopedMallocChar loc(raw_loc);
    EXPECT_STREQ("/usr/bin/tool", loc.get());
  }

  // Relative path with '/' is resolved against cwd.
  ASSERT_EQ(HARP_SUCCESS, harp_path_for_program("rel/tool", &raw_loc));
  {
    ScopedMallocChar loc(raw_loc);
    ASSERT_NE(nullptr, loc.get());
    EXPECT_EQ('/', loc.get()[0]);
    EXPECT_TRUE(std::string_view(loc.get()).ends_with("/rel/tool"));
  }

  // Program lookup via PATH.
  const TempDir bin_dir("utils_bin_dir");
  const TempFile prog("utils_bin_dir/my_harp_tool");
  ASSERT_TRUE(WriteFileText(prog.path(), "x"));
  setenv("PATH", bin_dir.c_str(), /*overwrite=*/1);

  ASSERT_EQ(HARP_SUCCESS, harp_path_for_program("my_harp_tool", &raw_loc));
  {
    ScopedMallocChar loc(raw_loc);
    ASSERT_NE(nullptr, loc.get());
    EXPECT_EQ(prog.string(), loc.get());
  }

  // Relative PATH component triggers relative-to-absolute normalization.
  setenv("PATH", ".", /*overwrite=*/1);
  const std::string cwd_basename =
      std::filesystem::current_path().filename().string();
  (void)cwd_basename;
  ASSERT_EQ(HARP_SUCCESS,
            harp_path_for_program("missing_relative_tool", &raw_loc));
  EXPECT_EQ(nullptr, raw_loc);

  // Unset PATH yields nullptr.
  unsetenv("PATH");
  ASSERT_EQ(HARP_SUCCESS, harp_path_for_program("my_harp_tool", &raw_loc));
  EXPECT_EQ(nullptr, raw_loc);
}

TEST(HarpUtilsTest, DataTypeMetadataFillMinMaxAndFloatPredicates) {
  for (const auto& tc : kDataTypeCases) {
    EXPECT_EQ(tc.expected_name, harp_get_data_type_name(tc.data_type));
    EXPECT_EQ(tc.expected_size, harp_get_size_for_type(tc.data_type));
  }

  for (harp_data_type dt : {harp_type_int8, harp_type_int16, harp_type_int32,
                            harp_type_float, harp_type_double}) {
    const harp_scalar fill = harp_get_fill_value_for_type(dt);
    const harp_scalar vmin = harp_get_valid_min_for_type(dt);
    const harp_scalar vmax = harp_get_valid_max_for_type(dt);
    EXPECT_EQ(1, harp_is_fill_value_for_type(dt, fill));
    EXPECT_EQ(1, harp_is_valid_min_for_type(dt, vmin));
    EXPECT_EQ(1, harp_is_valid_max_for_type(dt, vmax));
    EXPECT_EQ(0, harp_is_fill_value_for_type(dt, vmax));
    EXPECT_EQ(0, harp_is_valid_min_for_type(dt, vmax));
    EXPECT_EQ(0, harp_is_valid_max_for_type(dt, vmin));
  }

  EXPECT_EQ(1, harp_isfinite(0.0));
  EXPECT_EQ(1, harp_isfinite(-42.5));
  EXPECT_EQ(0, harp_isfinite(harp_nan()));
  EXPECT_EQ(0, harp_isfinite(harp_plusinf()));
  EXPECT_EQ(0, harp_isfinite(harp_mininf()));

  EXPECT_EQ(1, harp_isnan(harp_nan()));
  EXPECT_EQ(1, harp_isnan(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_EQ(0, harp_isnan(1.0));
  EXPECT_EQ(0, harp_isnan(harp_plusinf()));

  EXPECT_EQ(1, harp_isinf(harp_plusinf()));
  EXPECT_EQ(1, harp_isinf(harp_mininf()));
  EXPECT_EQ(0, harp_isinf(harp_nan()));
  EXPECT_EQ(0, harp_isinf(0.0));
  EXPECT_EQ(1, harp_isplusinf(harp_plusinf()));
  EXPECT_EQ(0, harp_isplusinf(harp_mininf()));
  EXPECT_EQ(1, harp_ismininf(harp_mininf()));
  EXPECT_EQ(0, harp_ismininf(harp_plusinf()));
}

TEST(HarpUtilsTest, ParseDoubleValidSpecialAndErrorCases) {
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  double val = 0.0;
  EXPECT_EQ(7L, harp_parse_double("   42.5", 7, &val, 0));
  EXPECT_THAT(val, DoubleNear(42.5, 1e-12));

  EXPECT_EQ(6L, harp_parse_double("+1.5e2", 6, &val, 0));
  EXPECT_THAT(val, DoubleNear(150.0, 1e-12));

  EXPECT_EQ(6L, harp_parse_double("-2.5D1", 6, &val, 0));
  EXPECT_THAT(val, DoubleNear(-25.0, 1e-12));

  EXPECT_EQ(5L, harp_parse_double("1E-02", 5, &val, 0));
  EXPECT_THAT(val, DoubleNear(0.01, 1e-12));

  EXPECT_EQ(2L, harp_parse_double(".5", 2, &val, 0));
  EXPECT_THAT(val, DoubleNear(0.5, 1e-12));

  EXPECT_EQ(5L, harp_parse_double("3.5  ", 5, &val, 0));
  EXPECT_THAT(val, DoubleNear(3.5, 1e-12));

  EXPECT_EQ(2L,
            harp_parse_double("12xy", 4, &val, /*ignore_trailing_bytes=*/1));
  EXPECT_THAT(val, DoubleNear(12.0, 1e-12));

  // NaN & Inf variants.
  EXPECT_EQ(3L, harp_parse_double("NaN", 3, &val, 0));
  EXPECT_TRUE(std::isnan(val));
  EXPECT_EQ(3L, harp_parse_double("nanXY", 5, &val, 1));
  EXPECT_TRUE(std::isnan(val));

  EXPECT_EQ(4L, harp_parse_double("+inf", 4, &val, 0));
  EXPECT_EQ(1, harp_isplusinf(val));
  EXPECT_EQ(4L, harp_parse_double("-INF", 4, &val, 0));
  EXPECT_EQ(1, harp_ismininf(val));
  EXPECT_EQ(3L, harp_parse_double("infXY", 5, &val, 1));
  EXPECT_EQ(1, harp_isplusinf(val));

  EXPECT_EQ(3L,
            harp_parse_double("1e2a", 4, &val, /*ignore_trailing_bytes=*/1));
  EXPECT_THAT(val, DoubleNear(100.0, 1e-12));

  // Exponent > 1024 clamps parsing loop to avoid integer overflow.
  EXPECT_EQ(6L, harp_parse_double("1e1025", 6, &val, 0));
  EXPECT_EQ(1, harp_isplusinf(val));

  // Error cases.
  constexpr std::string_view kBadDoubles[] = {
      "", "+", ".", "abc", "nan!", "+nan", "inf!", "1e", "1e+", "12.3x",
  };
  for (std::string_view s : kBadDoubles) {
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1L,
              harp_parse_double(s.data(), static_cast<long>(s.size()), &val, 0))
        << "input=" << s;
    EXPECT_EQ(HARP_ERROR_INVALID_FORMAT, harp_errno) << "input=" << s;
  }
}

TEST(HarpUtilsTest, DimensionProductAndStringArrayHelpers) {
  EXPECT_EQ(1L, harp_get_num_elements(0, nullptr));
  const long dims[] = {2, 3, 4};
  EXPECT_EQ(24L, harp_get_num_elements(3, dims));
  const long zero_dims[] = {2, 0, 4};
  EXPECT_EQ(0L, harp_get_num_elements(3, zero_dims));

  char s0[] = "hi";
  char s1[] = "hello";
  char* strs[] = {s0, nullptr, s1};
  EXPECT_EQ(5L, harp_get_max_string_length(3, strs));

  char* raw_buf1 = nullptr;
  long len1 = 0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_get_char_array_from_string_array(
                3, strs, /*min_string_length=*/8, &len1, &raw_buf1));
  ScopedMallocChar buf1(raw_buf1);
  EXPECT_EQ(8L, len1);
  EXPECT_STREQ("hi", &buf1.get()[0]);
  EXPECT_EQ('\0', buf1.get()[8]);
  EXPECT_STREQ("hello", &buf1.get()[16]);

  char* raw_buf2 = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_get_char_array_from_string_array(
                3, strs, /*min_string_length=*/2, nullptr, &raw_buf2));
  ScopedMallocChar buf2(raw_buf2);
  EXPECT_EQ("hello", std::string_view(&buf2.get()[10], 5));
}

TEST(HarpUtilsTest, ArrayNullAndReplaceFillValue) {
  int8_t i8[] = {1, -99, 2};
  harp_array a_i8{};
  a_i8.int8_data = i8;
  harp_scalar fill_i8{};
  fill_i8.int8_data = -99;
  harp_array_replace_fill_value(harp_type_int8, 3, a_i8, fill_i8);
  EXPECT_THAT(i8, ElementsAre(1, 0, 2));
  // Replacing with default fill value is a no-op.
  fill_i8.int8_data = 0;
  harp_array_replace_fill_value(harp_type_int8, 3, a_i8, fill_i8);
  harp_array_null(harp_type_int8, 3, a_i8);
  EXPECT_THAT(i8, ElementsAre(0, 0, 0));

  int16_t i16[] = {10, -999, 20};
  harp_array a_i16{};
  a_i16.int16_data = i16;
  harp_scalar fill_i16{};
  fill_i16.int16_data = -999;
  harp_array_replace_fill_value(harp_type_int16, 3, a_i16, fill_i16);
  EXPECT_THAT(i16, ElementsAre(10, 0, 20));
  harp_array_null(harp_type_int16, 3, a_i16);
  EXPECT_THAT(i16, ElementsAre(0, 0, 0));

  int32_t i32[] = {100, -9999, 200};
  harp_array a_i32{};
  a_i32.int32_data = i32;
  harp_scalar fill_i32{};
  fill_i32.int32_data = -9999;
  harp_array_replace_fill_value(harp_type_int32, 3, a_i32, fill_i32);
  EXPECT_THAT(i32, ElementsAre(100, 0, 200));
  harp_array_null(harp_type_int32, 3, a_i32);
  EXPECT_THAT(i32, ElementsAre(0, 0, 0));

  float f[] = {1.0f, -999.0f, 2.0f};
  harp_array a_f{};
  a_f.float_data = f;
  harp_scalar fill_f{};
  fill_f.float_data = -999.0f;
  harp_array_replace_fill_value(harp_type_float, 3, a_f, fill_f);
  EXPECT_EQ(1.0f, f[0]);
  EXPECT_TRUE(std::isnan(f[1]));
  EXPECT_EQ(2.0f, f[2]);
  harp_array_null(harp_type_float, 3, a_f);
  EXPECT_TRUE(std::isnan(f[0]));

  double d[] = {1.0, -999.0, 2.0};
  harp_array a_d{};
  a_d.double_data = d;
  harp_scalar fill_d{};
  fill_d.double_data = -999.0;
  harp_array_replace_fill_value(harp_type_double, 3, a_d, fill_d);
  EXPECT_EQ(1.0, d[0]);
  EXPECT_TRUE(std::isnan(d[1]));
  EXPECT_EQ(2.0, d[2]);
  harp_array_null(harp_type_double, 3, a_d);
  EXPECT_TRUE(std::isnan(d[0]));

  char* strs[] = {strdup("alpha"), nullptr, strdup("beta")};
  harp_array a_s{};
  a_s.string_data = strs;
  harp_array_null(harp_type_string, 3, a_s);
  EXPECT_EQ(nullptr, strs[0]);
  EXPECT_EQ(nullptr, strs[1]);
  EXPECT_EQ(nullptr, strs[2]);
}

TEST(HarpUtilsTest, ArrayInvertAllBlockSizes) {
  // length <= 1 is a no-op.
  int8_t single = 7;
  const long dims1[] = {1};
  harp_array a_single{};
  a_single.int8_data = &single;
  EXPECT_EQ(HARP_SUCCESS,
            harp_array_invert(harp_type_int8, 0, 1, dims1, a_single));
  EXPECT_EQ(7, single);

  // block_size == 1 (int8, 2x3 inverted along dim 1).
  int8_t b1[] = {1, 2, 3, 4, 5, 6};
  const long dims_2x3[] = {2, 3};
  harp_array a1{};
  a1.int8_data = b1;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_invert(harp_type_int8, 1, 2, dims_2x3, a1));
  EXPECT_THAT(b1, ElementsAre(3, 2, 1, 6, 5, 4));

  // block_size == 2 (int16, 2x3 inverted along dim 1).
  int16_t b2[] = {1, 2, 3, 4, 5, 6};
  harp_array a2{};
  a2.int16_data = b2;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_invert(harp_type_int16, 1, 2, dims_2x3, a2));
  EXPECT_THAT(b2, ElementsAre(3, 2, 1, 6, 5, 4));

  // block_size == 4 (int32, 2x3 inverted along dim 1).
  int32_t b4[] = {1, 2, 3, 4, 5, 6};
  harp_array a4{};
  a4.int32_data = b4;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_invert(harp_type_int32, 1, 2, dims_2x3, a4));
  EXPECT_THAT(b4, ElementsAre(3, 2, 1, 6, 5, 4));

  // block_size == 8 (double, 2x3 inverted along dim 1).
  double b8[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  harp_array a8{};
  a8.double_data = b8;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_invert(harp_type_double, 1, 2, dims_2x3, a8));
  EXPECT_THAT(b8, ElementsAre(3.0, 2.0, 1.0, 6.0, 5.0, 4.0));

  // block_size > 8 (double, 2x3 inverted along dim 0 => block_size = 24 bytes).
  double b24[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  harp_array a24{};
  a24.double_data = b24;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_invert(harp_type_double, 0, 2, dims_2x3, a24));
  EXPECT_THAT(b24, ElementsAre(4.0, 5.0, 6.0, 1.0, 2.0, 3.0));
}

TEST(HarpUtilsTest, ArrayTransposeAndInvalidOrder) {
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // No-op paths: num_dimensions <= 1 or num_elements <= 1.
  double scalar = 9.0;
  const long dim1[] = {3};
  harp_array a_scalar{};
  a_scalar.double_data = &scalar;
  EXPECT_EQ(HARP_SUCCESS,
            harp_array_transpose(harp_type_double, 1, dim1, nullptr, a_scalar));

  const long dims_1x1[] = {1, 1};
  EXPECT_EQ(HARP_SUCCESS, harp_array_transpose(harp_type_double, 2, dims_1x1,
                                               nullptr, a_scalar));

  // Invalid permutation indices.
  double dummy[6] = {};
  const long dims_2x3[] = {2, 3};
  harp_array a_dummy{};
  a_dummy.double_data = dummy;
  const int bad_oob[] = {0, 2};
  EXPECT_EQ(-1, harp_array_transpose(harp_type_double, 2, dims_2x3, bad_oob,
                                     a_dummy));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  const long dims_2x2x2[] = {2, 2, 2};
  const int bad_dup[] = {0, 1, 1};
  EXPECT_EQ(-1, harp_array_transpose(harp_type_double, 3, dims_2x2x2, bad_dup,
                                     a_dummy));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Reverse transpose (order == nullptr) for 1, 2, 4, 8-byte element sizes.
  int8_t t1[] = {1, 2, 3, 4, 5, 6};
  harp_array at1{};
  at1.int8_data = t1;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_transpose(harp_type_int8, 2, dims_2x3, nullptr, at1));
  EXPECT_THAT(t1, ElementsAre(1, 4, 2, 5, 3, 6));

  int16_t t2[] = {1, 2, 3, 4, 5, 6};
  harp_array at2{};
  at2.int16_data = t2;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_transpose(harp_type_int16, 2, dims_2x3, nullptr, at2));
  EXPECT_THAT(t2, ElementsAre(1, 4, 2, 5, 3, 6));

  int32_t t4[] = {1, 2, 3, 4, 5, 6};
  harp_array at4{};
  at4.int32_data = t4;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_transpose(harp_type_int32, 2, dims_2x3, nullptr, at4));
  EXPECT_THAT(t4, ElementsAre(1, 4, 2, 5, 3, 6));

  double t8[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  harp_array at8{};
  at8.double_data = t8;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_transpose(harp_type_double, 2, dims_2x3, nullptr, at8));
  EXPECT_THAT(t8, ElementsAre(1.0, 4.0, 2.0, 5.0, 3.0, 6.0));

  // Explicit 3D permutation [1, 2, 0] on [2, 2, 2] for 2, 4, and 8 bytes.
  const int perm[] = {1, 2, 0};
  int16_t p2[] = {0, 1, 2, 3, 4, 5, 6, 7};
  harp_array ap2{};
  ap2.int16_data = p2;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_transpose(harp_type_int16, 3, dims_2x2x2, perm, ap2));
  EXPECT_THAT(p2, ElementsAre(0, 4, 1, 5, 2, 6, 3, 7));

  int32_t p4[] = {0, 1, 2, 3, 4, 5, 6, 7};
  harp_array ap4{};
  ap4.int32_data = p4;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_transpose(harp_type_int32, 3, dims_2x2x2, perm, ap4));
  EXPECT_THAT(p4, ElementsAre(0, 4, 1, 5, 2, 6, 3, 7));

  double p8[] = {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0};
  harp_array ap8{};
  ap8.double_data = p8;
  ASSERT_EQ(HARP_SUCCESS,
            harp_array_transpose(harp_type_double, 3, dims_2x2x2, perm, ap8));
  EXPECT_THAT(p8, ElementsAre(0.0, 4.0, 1.0, 5.0, 2.0, 6.0, 3.0, 7.0));
}

}  // namespace
}  // namespace stcorp_harp
