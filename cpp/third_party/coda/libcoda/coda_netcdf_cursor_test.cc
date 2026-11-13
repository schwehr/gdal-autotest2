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
// Tests CODA NetCDF cursor navigation, metadata queries, scalar reads, string
// and char-array reads, C vs Fortran array reads, partial array reads (fixed
// and record/unlimited variables), conversions, boundary checks, attributes
// navigation, NetCDF v1/v2/v5 formats, and error paths
// (libcoda/coda-netcdf-cursor.c).
//
// google3 build caveats:
//   - HAVE_NETCDF_H is undefined in config.h; CODA uses its built-in NetCDF
//     classic parser (CDF\x01, CDF\x02, CDF\x05) in libcoda/coda-netcdf.c.
//   - Compiled on little-endian LP64 Linux (WORDS_BIGENDIAN branches are
//     compiled out).
//   - Upstream coda-netcdf-cursor.c read_partial_array() endian-swap loops
//     (lines 329, 335, 341) iterate up to type->definition->num_elements rather
//     than length; tests for 16/32/64-bit partial array reads pre-size output
//     buffers to num_elements (zero-initialized) to avoid out-of-bounds swaps.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-netcdf-cursor.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-netcdf.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-netcdf.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-netcdf-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin.h"
#include "third_party/stcorp_coda/libcoda/coda-netcdf.h"
}  // extern "C"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_coda/google/util/netcdf_builder.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

namespace stcorp_coda {
namespace {

using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::FloatEq;
using ::testing::IsNan;

class CodaNetcdfCursorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    env_def_.Unset();
    env_leap_.Unset();
    coda_set_error(CODA_SUCCESS, nullptr);
    ASSERT_EQ(CODA_SUCCESS, session_.Init());
  }

  void TearDown() override {
    session_.Reset();
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ScopedEnvVar env_def_{"CODA_DEFINITION"};
  ScopedEnvVar env_leap_{"CODA_LEAP_SECOND_TABLE"};
  ScopedCodaSession session_;
};

template <typename Range>
std::vector<uint8_t> EncodeBe(const Range& values) {
  using T = std::remove_cv_t<typename Range::value_type>;
  std::vector<uint8_t> out;
  out.reserve(values.size() * sizeof(T));
  for (const T& v : values) {
    if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, char>) {
      out.push_back(static_cast<uint8_t>(v));
    } else if constexpr (std::is_same_v<T, int16_t>) {
      NetcdfBuilder::AppendBe16(out, static_cast<uint16_t>(v));
    } else if constexpr (std::is_same_v<T, int32_t>) {
      NetcdfBuilder::AppendBe32(out, static_cast<uint32_t>(v));
    } else if constexpr (std::is_same_v<T, float>) {
      NetcdfBuilder::AppendBe32(out, std::bit_cast<uint32_t>(v));
    } else if constexpr (std::is_same_v<T, double>) {
      NetcdfBuilder::AppendBe64(out, std::bit_cast<uint64_t>(v));
    }
  }
  return out;
}

template <typename T>
std::vector<uint8_t> EncodeBeScalar(T value) {
  const std::array<T, 1> arr = {value};
  return EncodeBe(arr);
}

void OpenBuiltNetcdf(const TempFile& file, const NetcdfBuilder& builder,
                     ScopedProduct& product) {
  const std::vector<uint8_t> bytes = builder.Build();
  ASSERT_TRUE(file.WriteBytes(bytes));
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));
}

coda_cursor GotoField(const coda_cursor& parent, const char* name) {
  coda_cursor child = parent;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&child, name));
  return child;
}

TEST_F(CodaNetcdfCursorTest, OpenMissingAndCorruptFilesFail) {
  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) / "netcdf_missing_file.nc";
  std::error_code ec;
  std::filesystem::remove(missing_path, ec);

  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing_path.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);

  // Unsupported NetCDF classic version (CDF\x03): coda_open() stops in
  // coda-product.c recognition, while coda_bin_open() + coda_netcdf_reopen()
  // exercises coda-netcdf.c's version check directly.
  TempFile bad_version("netcdf_bad_version.nc");
  constexpr std::array<uint8_t, 8> kBadVersion = {'C', 'D', 'F', 3, 0, 0, 0, 0};
  ASSERT_TRUE(bad_version.WriteBytes(kBadVersion));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(bad_version.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
  {
    coda_product* raw = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(bad_version.c_str(), kBadVersion.size(), &raw));
    coda_set_error(CODA_SUCCESS, nullptr);
    const int reopen_rc = coda_netcdf_reopen(&raw);
    EXPECT_EQ(-1, reopen_rc);
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    if (reopen_rc == CODA_SUCCESS) {
      coda_close(raw);
    }
  }

  // Truncated NetCDF header after magic.
  TempFile truncated_hdr("netcdf_truncated_hdr.nc");
  constexpr std::array<uint8_t, 6> kTruncatedHdr = {'C', 'D', 'F', 1, 0, 0};
  ASSERT_TRUE(truncated_hdr.WriteBytes(kTruncatedHdr));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(truncated_hdr.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  // Invalid NC_DIMENSION tag in header.
  TempFile bad_dim_tag("netcdf_bad_dim_tag.nc");
  std::vector<uint8_t> bad_dim_bytes = {'C', 'D', 'F', 1};
  NetcdfBuilder::AppendBe32(bad_dim_bytes, 0);   // numrecs
  NetcdfBuilder::AppendBe32(bad_dim_bytes, 99);  // invalid tag (expected 0/10)
  NetcdfBuilder::AppendBe32(bad_dim_bytes, 1);   // nelems
  ASSERT_TRUE(bad_dim_tag.WriteBytes(bad_dim_bytes));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(bad_dim_tag.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);

  // Variable offset beyond end of file.
  TempFile bad_offset("netcdf_bad_offset.nc");
  NetcdfBuilder builder(1);
  builder.AddVar({
      .name = "x",
      .dim_ids = {},
      .attrs = {},
      .type = NetcdfType::kInt,
      .vsize = 4,
      .data = EncodeBeScalar<int32_t>(1),
  });
  std::vector<uint8_t> bytes = builder.Build();
  // Patch the low byte of variable x's 32-bit `begin` offset (located right
  // before the 4-byte variable payload) so var_offset > file_size.
  ASSERT_GE(bytes.size(), 8U);
  bytes[bytes.size() - 5] = static_cast<uint8_t>(bytes[bytes.size() - 5] + 16);
  ASSERT_TRUE(bad_offset.WriteBytes(bytes));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(bad_offset.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
}

TEST_F(CodaNetcdfCursorTest, RootNavigationMetadataAndUnsupportedOperations) {
  TempFile nc_file("netcdf_cursor_root_nav.nc");
  NetcdfBuilder builder(1);
  builder.AddDim({.name = "n", .size = 3});
  builder.AddGlobalAttr({
      .name = "title",
      .type = NetcdfType::kChar,
      .count = 11,
      .raw_values = EncodeBe(std::string_view("netcdf_test")),
  });
  builder.AddVar({
      .name = "s_i32",
      .dim_ids = {},
      .attrs = {},
      .type = NetcdfType::kInt,
      .vsize = 4,
      .data = EncodeBeScalar<int32_t>(42),
  });
  constexpr std::array<int32_t, 3> kVec = {10, 20, 30};
  builder.AddVar({
      .name = "v_i32",
      .dim_ids = {0},
      .attrs = {},
      .type = NetcdfType::kInt,
      .vsize = 12,
      .data = EncodeBe(kVec),
  });

  ScopedProduct product;
  OpenBuiltNetcdf(nc_file, builder, product);

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  coda_product* got_product = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_product_file(&root, &got_product));
  EXPECT_EQ(product.get(), got_product);

  coda_format format = coda_format_ascii;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&root, &format));
  EXPECT_EQ(coda_format_netcdf, format);

  coda_type_class type_class = coda_special_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&root, &type_class));
  EXPECT_EQ(coda_record_class, type_class);

  int depth = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&root, &depth));
  EXPECT_EQ(0, depth);

  long num_fields = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&root, &num_fields));
  EXPECT_EQ(2, num_fields);

  int available = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &root, 0, &available));
  EXPECT_EQ(1, available);

  // Root has no parent.
  coda_cursor c = root;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_parent(&c));
  EXPECT_EQ(CODA_ERROR_NO_PARENT, coda_errno);

  // Record navigation across fields.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&c));
  long field_idx = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &field_idx));
  EXPECT_EQ(0, field_idx);

  // Scalar NetCDF variable reports num_elements == 1, bit/byte size == -1,
  // file_bit_offset == -1, and rejects file_byte_offset, read_bits, read_bytes.
  long scalar_elems = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &scalar_elems));
  EXPECT_EQ(1, scalar_elems);

  int64_t bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bit_size));
  EXPECT_EQ(-1, bit_size);

  int64_t byte_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&c, &byte_size));
  EXPECT_EQ(-1, byte_size);

  int64_t bit_offset = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_bit_offset(&c, &bit_offset));
  EXPECT_EQ(-1, bit_offset);

  int64_t byte_offset = 0;
  EXPECT_EQ(-1, coda_cursor_get_file_byte_offset(&c, &byte_offset));
  EXPECT_EQ(-1, byte_offset);

  std::array<uint8_t, 4> raw_buf{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_bits(&c, raw_buf.data(), 0, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&c, raw_buf.data(), 0, 4));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  // Move to next field (v_i32) and verify num_elements == 3 and goto path.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &field_idx));
  EXPECT_EQ(1, field_idx);
  long vec_elems = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &vec_elems));
  EXPECT_EQ(3, vec_elems);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/v_i32[1]"));
  int32_t v1 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &v1));
  EXPECT_EQ(20, v1);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&c, &depth));
  EXPECT_EQ(0, depth);
}

TEST_F(CodaNetcdfCursorTest, ScalarReadsAcrossAllNetcdfTypesAndWidening) {
  ScopedCodaOptions restore_options;
  TempFile nc_file("netcdf_cursor_scalars.nc");
  NetcdfBuilder builder(1);
  builder.AddVar({
      .name = "v_byte",
      .dim_ids = {},
      .attrs = {},
      .type = NetcdfType::kByte,
      .vsize = 4,
      .data = EncodeBeScalar<int8_t>(-42),
  });
  builder.AddVar({
      .name = "v_char",
      .dim_ids = {},
      .attrs = {},
      .type = NetcdfType::kChar,
      .vsize = 4,
      .data = EncodeBeScalar<char>('K'),
  });
  builder.AddVar({
      .name = "v_short",
      .dim_ids = {},
      .attrs = {},
      .type = NetcdfType::kShort,
      .vsize = 4,
      .data = EncodeBeScalar<int16_t>(-12345),
  });
  builder.AddVar({
      .name = "v_int",
      .dim_ids = {},
      .attrs = {},
      .type = NetcdfType::kInt,
      .vsize = 4,
      .data = EncodeBeScalar<int32_t>(-1000000),
  });
  builder.AddVar({
      .name = "v_float",
      .dim_ids = {},
      .attrs = {},
      .type = NetcdfType::kFloat,
      .vsize = 4,
      .data = EncodeBeScalar<float>(3.5f),
  });
  builder.AddVar({
      .name = "v_double",
      .dim_ids = {},
      .attrs = {},
      .type = NetcdfType::kDouble,
      .vsize = 8,
      .data = EncodeBeScalar<double>(-123.456789),
  });

  // Exercise both mmap (default) and pread (use_mmap = 0) paths.
  for (const int use_mmap : {1, 0}) {
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));
    ScopedProduct product;
    OpenBuiltNetcdf(nc_file, builder, product);

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

    // NC_BYTE -> int8 + widening reads.
    {
      coda_cursor c = GotoField(root, "v_byte");
      coda_native_type rt = coda_native_type_not_available;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&c, &rt));
      EXPECT_EQ(coda_native_type_int8, rt);

      int has_ascii = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&c, &has_ascii));
      EXPECT_EQ(0, has_ascii);

      int8_t i8 = 0;
      int16_t i16 = 0;
      int32_t i32 = 0;
      int64_t i64 = 0;
      float f32 = 0.0f;
      double f64 = 0.0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&c, &i8));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &i16));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &i32));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &i64));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &f32));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64));
      EXPECT_EQ(-42, i8);
      EXPECT_EQ(-42, i16);
      EXPECT_EQ(-42, i32);
      EXPECT_EQ(-42, i64);
      EXPECT_THAT(f32, FloatEq(-42.0f));
      EXPECT_THAT(f64, DoubleEq(-42.0));
    }

    // NC_CHAR scalar -> char + string length 1.
    {
      coda_cursor c = GotoField(root, "v_char");
      coda_native_type rt = coda_native_type_not_available;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&c, &rt));
      EXPECT_EQ(coda_native_type_char, rt);

      int has_ascii = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&c, &has_ascii));
      EXPECT_EQ(1, has_ascii);

      long len = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &len));
      EXPECT_EQ(1, len);

      char ch = '\0';
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&c, &ch));
      EXPECT_EQ('K', ch);

      std::array<char, 2> str_buf{};
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_string(&c, str_buf.data(), str_buf.size()));
      EXPECT_STREQ("K", str_buf.data());
    }

    // NC_SHORT -> int16 + widening.
    {
      coda_cursor c = GotoField(root, "v_short");
      coda_native_type rt = coda_native_type_not_available;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&c, &rt));
      EXPECT_EQ(coda_native_type_int16, rt);

      int16_t i16 = 0;
      int32_t i32 = 0;
      int64_t i64 = 0;
      double f64 = 0.0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &i16));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &i32));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &i64));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64));
      EXPECT_EQ(-12345, i16);
      EXPECT_EQ(-12345, i32);
      EXPECT_EQ(-12345, i64);
      EXPECT_THAT(f64, DoubleEq(-12345.0));
    }

    // NC_INT -> int32 + widening.
    {
      coda_cursor c = GotoField(root, "v_int");
      coda_native_type rt = coda_native_type_not_available;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&c, &rt));
      EXPECT_EQ(coda_native_type_int32, rt);

      int32_t i32 = 0;
      int64_t i64 = 0;
      float f32 = 0.0f;
      double f64 = 0.0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &i32));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &i64));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &f32));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64));
      EXPECT_EQ(-1000000, i32);
      EXPECT_EQ(-1000000, i64);
      EXPECT_THAT(f32, FloatEq(-1000000.0f));
      EXPECT_THAT(f64, DoubleEq(-1000000.0));
    }

    // NC_FLOAT -> float + double.
    {
      coda_cursor c = GotoField(root, "v_float");
      coda_native_type rt = coda_native_type_not_available;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&c, &rt));
      EXPECT_EQ(coda_native_type_float, rt);

      float f32 = 0.0f;
      double f64 = 0.0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &f32));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64));
      EXPECT_THAT(f32, FloatEq(3.5f));
      EXPECT_THAT(f64, DoubleEq(3.5));
    }

    // NC_DOUBLE -> double + float.
    {
      coda_cursor c = GotoField(root, "v_double");
      coda_native_type rt = coda_native_type_not_available;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&c, &rt));
      EXPECT_EQ(coda_native_type_double, rt);

      double f64 = 0.0;
      float f32 = 0.0f;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &f32));
      EXPECT_THAT(f64, DoubleEq(-123.456789));
      EXPECT_THAT(f32, FloatEq(-123.456789f));
    }
  }
}

TEST_F(CodaNetcdfCursorTest, StringVariablesAndCharArrays) {
  TempFile nc_file("netcdf_cursor_strings_chars.nc");
  NetcdfBuilder builder(1);
  builder.set_num_recs(3);
  builder.AddDim({.name = "rec", .size = 0});   // dim 0: unlimited (3)
  builder.AddDim({.name = "len6", .size = 6});  // dim 1
  builder.AddDim({.name = "len5", .size = 5});  // dim 2
  builder.AddDim({.name = "n2", .size = 2});    // dim 3
  builder.AddDim({.name = "n1", .size = 1});    // dim 4

  // 1D fixed char var -> scalar string of length 6.
  builder.AddVar({
      .name = "str1d",
      .dim_ids = {1},
      .attrs = {},
      .type = NetcdfType::kChar,
      .vsize = 8,
      .data = EncodeBe(std::string_view("Hello!")),
  });
  // 2D fixed char var [2, 5] -> 1D array of 2 strings of length 5.
  builder.AddVar({
      .name = "str2d",
      .dim_ids = {3, 2},
      .attrs = {},
      .type = NetcdfType::kChar,
      .vsize = 12,
      .data = EncodeBe(std::string_view("AlphaBravo")),
  });
  // 1D fixed char var of length 1 -> scalar char.
  builder.AddVar({
      .name = "ch1",
      .dim_ids = {4},
      .attrs = {},
      .type = NetcdfType::kChar,
      .vsize = 4,
      .data = EncodeBeScalar<char>('Z'),
  });
  // 3D fixed char var [2, 2, 1] -> 2D [2, 2] char array (tests C vs Fortran).
  builder.AddVar({
      .name = "char_mat",
      .dim_ids = {3, 3, 4},
      .attrs = {},
      .type = NetcdfType::kChar,
      .vsize = 4,
      .data = EncodeBe(std::string_view("ABCD")),
  });
  // 1D record char var [rec] -> 1D array of 3 chars (coda-netcdf.c:731).
  builder.AddVar({
      .name = "rec_chars",
      .dim_ids = {0},
      .attrs = {},
      .type = NetcdfType::kChar,
      .vsize = 4,
      .data = std::vector<uint8_t>{'P', 0, 0, 0, 'Q', 0, 0, 0, 'R', 0, 0, 0},
  });

  ScopedProduct product;
  OpenBuiltNetcdf(nc_file, builder, product);

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // 1. 1D fixed char var: full read and truncated read (size_boundary < len).
  {
    coda_cursor c = GotoField(root, "str1d");
    coda_type_class tc = coda_record_class;
    coda_native_type rt = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&c, &tc));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&c, &rt));
    EXPECT_EQ(coda_text_class, tc);
    EXPECT_EQ(coda_native_type_string, rt);

    long len = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &len));
    EXPECT_EQ(6, len);

    std::array<char, 7> full_buf{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&c, full_buf.data(), full_buf.size()));
    EXPECT_STREQ("Hello!", full_buf.data());

    std::array<char, 4> short_buf{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&c, short_buf.data(), short_buf.size()));
    EXPECT_STREQ("Hel", short_buf.data());

    // Cannot read multi-byte string via coda_cursor_read_char().
    char ch = '\0';
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_char(&c, &ch));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  }

  // 2. 2D fixed char var -> array of strings.
  {
    coda_cursor c = GotoField(root, "str2d");
    int num_dims = -1;
    std::array<long, CODA_MAX_NUM_DIMS> dims{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&c, &num_dims, dims.data()));
    EXPECT_EQ(1, num_dims);
    EXPECT_EQ(2, dims[0]);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&c));
    long len = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &len));
    EXPECT_EQ(5, len);
    std::array<char, 6> buf{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, buf.data(), 6));
    EXPECT_STREQ("Alpha", buf.data());

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, buf.data(), 6));
    EXPECT_STREQ("Bravo", buf.data());
  }

  // 3. 1D fixed char var of length 1 -> scalar char.
  {
    coda_cursor c = GotoField(root, "ch1");
    char ch = '\0';
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&c, &ch));
    EXPECT_EQ('Z', ch);
  }

  // 4. 2D char array: C vs Fortran ordering and partial read.
  {
    coda_cursor c = GotoField(root, "char_mat");
    std::array<char, 4> c_ord{};
    std::array<char, 4> f_ord{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char_array(&c, c_ord.data(),
                                                        coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char_array(
                                &c, f_ord.data(), coda_array_ordering_fortran));
    EXPECT_THAT(c_ord, ElementsAre('A', 'B', 'C', 'D'));
    EXPECT_THAT(f_ord, ElementsAre('A', 'C', 'B', 'D'));

    std::array<char, 2> part{'?', '?'};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_char_partial_array(&c, 1, 2, part.data()));
    EXPECT_THAT(part, ElementsAre('B', 'C'));
  }

  // 5. 1D record char array -> char array across records.
  {
    coda_cursor c = GotoField(root, "rec_chars");
    std::array<char, 3> out{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char_array(&c, out.data(),
                                                        coda_array_ordering_c));
    EXPECT_THAT(out, ElementsAre('P', 'Q', 'R'));

    std::array<char, 2> part{'?', '?'};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_char_partial_array(&c, 1, 2, part.data()));
    EXPECT_THAT(part, ElementsAre('Q', 'R'));

    coda_cursor elem = c;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 2));
    char ch = '\0';
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&elem, &ch));
    EXPECT_EQ('R', ch);
  }
}

TEST_F(CodaNetcdfCursorTest, RecordStrings2DCharArray) {
  TempFile nc_file("netcdf_cursor_rec_strings.nc");
  NetcdfBuilder builder(1);
  builder.set_num_recs(2);
  builder.AddDim({.name = "rec", .size = 0});
  builder.AddDim({.name = "slen", .size = 5});
  builder.AddVar({
      .name = "rec_str",
      .dim_ids = {0, 1},
      .attrs = {},
      .type = NetcdfType::kChar,
      .vsize = 8,
      .data = std::vector<uint8_t>{'N', 'o', 'r', 't', 'h', 0, 0, 0, 'S', 'o',
                                   'u', 't', 'h', 0, 0, 0},
  });

  ScopedProduct product;
  OpenBuiltNetcdf(nc_file, builder, product);

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
  coda_cursor c = GotoField(root, "rec_str");

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&c));
  std::array<char, 6> buf{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, buf.data(), 6));
  EXPECT_STREQ("North", buf.data());

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, buf.data(), 6));
  EXPECT_STREQ("South", buf.data());
}

TEST_F(CodaNetcdfCursorTest, ArrayReadsCAndFortranOrderingAllNumericTypes) {
  TempFile nc_file("netcdf_cursor_numeric_arrays.nc");
  NetcdfBuilder builder(1);
  builder.AddDim({.name = "d2", .size = 2});  // dim 0
  builder.AddDim({.name = "d3", .size = 3});  // dim 1
  builder.AddDim({.name = "d4", .size = 4});  // dim 2

  constexpr std::array<int8_t, 4> kArrI8 = {-10, 0, 25, 127};
  builder.AddVar({
      .name = "arr_i8",
      .dim_ids = {2},
      .attrs = {},
      .type = NetcdfType::kByte,
      .vsize = 4,
      .data = EncodeBe(kArrI8),
  });

  constexpr std::array<int16_t, 6> kMatI16 = {100, -200, 300, -400, 500, -600};
  builder.AddVar({
      .name = "mat_i16",
      .dim_ids = {0, 1},
      .attrs = {},
      .type = NetcdfType::kShort,
      .vsize = 12,
      .data = EncodeBe(kMatI16),
  });

  constexpr std::array<int32_t, 6> kMatI32 = {1, 2, 3, 4, 5, 6};
  builder.AddVar({
      .name = "mat_i32",
      .dim_ids = {0, 1},
      .attrs = {},
      .type = NetcdfType::kInt,
      .vsize = 24,
      .data = EncodeBe(kMatI32),
  });

  constexpr std::array<float, 6> kMatF32 = {1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f};
  builder.AddVar({
      .name = "mat_f32",
      .dim_ids = {0, 1},
      .attrs = {},
      .type = NetcdfType::kFloat,
      .vsize = 24,
      .data = EncodeBe(kMatF32),
  });

  constexpr std::array<double, 12> kCubeF64 = {1.0, 2.0, 3.0, 4.0,  5.0,  6.0,
                                               7.0, 8.0, 9.0, 10.0, 11.0, 12.0};
  builder.AddVar({
      .name = "cube_f64",
      .dim_ids = {0, 0, 1},
      .attrs = {},
      .type = NetcdfType::kDouble,
      .vsize = 96,
      .data = EncodeBe(kCubeF64),
  });

  ScopedProduct product;
  OpenBuiltNetcdf(nc_file, builder, product);

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // 1D int8 array + widening.
  {
    coda_cursor c = GotoField(root, "arr_i8");
    std::vector<int8_t> out_i8(4, -99);
    std::vector<int16_t> out_i16(4, -999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8_array(&c, out_i8.data(),
                                                        coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(
                                &c, out_i16.data(), coda_array_ordering_c));
    EXPECT_THAT(out_i8, ElementsAreArray(kArrI8));
    EXPECT_THAT(out_i16, ElementsAre(-10, 0, 25, 127));
  }

  // 2D int16 matrix: C vs Fortran ordering + subscript navigation + widening.
  {
    coda_cursor c = GotoField(root, "mat_i16");
    int num_dims = -1;
    std::array<long, CODA_MAX_NUM_DIMS> dims{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&c, &num_dims, dims.data()));
    EXPECT_EQ(2, num_dims);
    EXPECT_EQ(2, dims[0]);
    EXPECT_EQ(3, dims[1]);

    std::vector<int16_t> c_ord(6, -999);
    std::vector<int16_t> f_ord(6, -999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(
                                &c, c_ord.data(), coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(
                                &c, f_ord.data(), coda_array_ordering_fortran));
    EXPECT_THAT(c_ord, ElementsAre(100, -200, 300, -400, 500, -600));
    EXPECT_THAT(f_ord, ElementsAre(100, -400, -200, 500, 300, -600));

    std::vector<int32_t> widened(6, -999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &c, widened.data(), coda_array_ordering_c));
    EXPECT_THAT(widened, ElementsAre(100, -200, 300, -400, 500, -600));

    coda_cursor elem = c;
    constexpr std::array<long, 2> kSub = {1, 2};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element(&elem, 2, kSub.data()));
    int16_t val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&elem, &val));
    EXPECT_EQ(-600, val);
  }

  // 2D int32 matrix: C vs Fortran ordering + int64 widening.
  {
    coda_cursor c = GotoField(root, "mat_i32");
    std::vector<int32_t> c_ord(6, -1);
    std::vector<int32_t> f_ord(6, -1);
    std::vector<int64_t> wide64(6, -1);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &c, c_ord.data(), coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &c, f_ord.data(), coda_array_ordering_fortran));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64_array(
                                &c, wide64.data(), coda_array_ordering_c));
    EXPECT_THAT(c_ord, ElementsAre(1, 2, 3, 4, 5, 6));
    EXPECT_THAT(f_ord, ElementsAre(1, 4, 2, 5, 3, 6));
    EXPECT_THAT(wide64, ElementsAre(1, 2, 3, 4, 5, 6));
  }

  // 2D float matrix: C vs Fortran ordering + double widening.
  {
    coda_cursor c = GotoField(root, "mat_f32");
    std::vector<float> c_ord(6, -1.0f);
    std::vector<float> f_ord(6, -1.0f);
    std::vector<double> wide64(6, -1.0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(
                                &c, c_ord.data(), coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(
                                &c, f_ord.data(), coda_array_ordering_fortran));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, wide64.data(), coda_array_ordering_c));
    EXPECT_THAT(c_ord, ElementsAre(1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f));
    EXPECT_THAT(f_ord, ElementsAre(1.5f, 4.5f, 2.5f, 5.5f, 3.5f, 6.5f));
    EXPECT_THAT(wide64, ElementsAre(1.5, 2.5, 3.5, 4.5, 5.5, 6.5));
  }

  // 3D double cube [2, 2, 3]: C vs Fortran ordering + 3D subscript indexing.
  {
    coda_cursor c = GotoField(root, "cube_f64");
    int num_dims = -1;
    std::array<long, CODA_MAX_NUM_DIMS> dims{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&c, &num_dims, dims.data()));
    EXPECT_EQ(3, num_dims);
    EXPECT_EQ(2, dims[0]);
    EXPECT_EQ(2, dims[1]);
    EXPECT_EQ(3, dims[2]);

    std::vector<double> c_ord(12, -1.0);
    std::vector<double> f_ord(12, -1.0);
    std::vector<float> f32_ord(12, -1.0f);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, c_ord.data(), coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, f_ord.data(), coda_array_ordering_fortran));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(
                                &c, f32_ord.data(), coda_array_ordering_c));
    EXPECT_THAT(c_ord, ElementsAreArray(kCubeF64));
    // Fortran order for [2, 2, 3]: (0,0,0)=1, (1,0,0)=7, (0,1,0)=4, (1,1,0)=10,
    // (0,0,1)=2, (1,0,1)=8, (0,1,1)=5, (1,1,1)=11, (0,0,2)=3, ...
    EXPECT_THAT(f_ord, ElementsAre(1.0, 7.0, 4.0, 10.0, 2.0, 8.0, 5.0, 11.0,
                                   3.0, 9.0, 6.0, 12.0));
    EXPECT_THAT(f32_ord, ElementsAre(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
                                     8.0f, 9.0f, 10.0f, 11.0f, 12.0f));

    coda_cursor elem = c;
    constexpr std::array<long, 3> kSub = {1, 0, 2};  // flat index 1*6 + 0*3 + 2
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element(&elem, 3, kSub.data()));
    long flat_idx = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&elem, &flat_idx));
    EXPECT_EQ(8, flat_idx);
    double val = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&elem, &val));
    EXPECT_THAT(val, DoubleEq(9.0));
  }
}

TEST_F(CodaNetcdfCursorTest, PartialArrayReadsFixedArraysAllTypes) {
  TempFile nc_file("netcdf_cursor_partial_fixed.nc");
  NetcdfBuilder builder(1);
  builder.AddDim({.name = "n5", .size = 5});

  constexpr std::array<int8_t, 5> kI8 = {-50, -10, 0, 25, 100};
  constexpr std::array<int16_t, 5> kI16 = {-300, -100, 0, 100, 300};
  constexpr std::array<int32_t, 5> kI32 = {10, 20, 30, 40, 50};
  constexpr std::array<float, 5> kF32 = {1.25f, 2.25f, 3.25f, 4.25f, 5.25f};
  constexpr std::array<double, 5> kF64 = {10.5, 20.5, 30.5, 40.5, 50.5};

  builder.AddVar({.name = "p_i8",
                  .dim_ids = {0},
                  .attrs = {},
                  .type = NetcdfType::kByte,
                  .vsize = 8,
                  .data = EncodeBe(kI8)});
  builder.AddVar({.name = "p_i16",
                  .dim_ids = {0},
                  .attrs = {},
                  .type = NetcdfType::kShort,
                  .vsize = 12,
                  .data = EncodeBe(kI16)});
  builder.AddVar({.name = "p_i32",
                  .dim_ids = {0},
                  .attrs = {},
                  .type = NetcdfType::kInt,
                  .vsize = 20,
                  .data = EncodeBe(kI32)});
  builder.AddVar({.name = "p_f32",
                  .dim_ids = {0},
                  .attrs = {},
                  .type = NetcdfType::kFloat,
                  .vsize = 20,
                  .data = EncodeBe(kF32)});
  builder.AddVar({.name = "p_f64",
                  .dim_ids = {0},
                  .attrs = {},
                  .type = NetcdfType::kDouble,
                  .vsize = 40,
                  .data = EncodeBe(kF64)});

  ScopedProduct product;
  OpenBuiltNetcdf(nc_file, builder, product);

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // NC_BYTE partial read (8-bit has no swap loop) + widening partial reads.
  {
    coda_cursor c = GotoField(root, "p_i8");
    std::vector<int8_t> part_i8(3, -1);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int8_partial_array(&c, 1, 3, part_i8.data()));
    EXPECT_THAT(part_i8, ElementsAre(-10, 0, 25));

    std::vector<int16_t> part_i16(3, -1);
    std::vector<int32_t> part_i32(3, -1);
    std::vector<int64_t> part_i64(3, -1);
    std::vector<float> part_f32(3, -1.0f);
    std::vector<double> part_f64(3, -1.0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int16_partial_array(&c, 2, 3, part_i16.data()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_partial_array(&c, 2, 3, part_i32.data()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int64_partial_array(&c, 2, 3, part_i64.data()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_float_partial_array(&c, 2, 3, part_f32.data()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_partial_array(&c, 2, 3, part_f64.data()));
    EXPECT_THAT(part_i16, ElementsAre(0, 25, 100));
    EXPECT_THAT(part_i32, ElementsAre(0, 25, 100));
    EXPECT_THAT(part_i64, ElementsAre(0, 25, 100));
    EXPECT_THAT(part_f32, ElementsAre(0.0f, 25.0f, 100.0f));
    EXPECT_THAT(part_f64, ElementsAre(0.0, 25.0, 100.0));
  }

  // Note: For 16/32/64-bit types, upstream read_partial_array() swaps
  // num_elements entries instead of length; allocate num_elements zeroed.
  {
    coda_cursor c = GotoField(root, "p_i16");
    std::vector<int16_t> buf(5, 0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int16_partial_array(&c, 1, 3, buf.data()));
    EXPECT_THAT(std::span<const int16_t>(buf.data(), 3),
                ElementsAre(-100, 0, 100));
  }
  {
    coda_cursor c = GotoField(root, "p_i32");
    std::vector<int32_t> buf(5, 0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_partial_array(&c, 2, 2, buf.data()));
    EXPECT_THAT(std::span<const int32_t>(buf.data(), 2), ElementsAre(30, 40));
  }
  {
    coda_cursor c = GotoField(root, "p_f32");
    std::vector<float> buf(5, 0.0f);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_float_partial_array(&c, 1, 3, buf.data()));
    EXPECT_THAT(std::span<const float>(buf.data(), 3),
                ElementsAre(2.25f, 3.25f, 4.25f));
  }
  {
    coda_cursor c = GotoField(root, "p_f64");
    std::vector<double> buf(5, 0.0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_partial_array(&c, 2, 3, buf.data()));
    EXPECT_THAT(std::span<const double>(buf.data(), 3),
                ElementsAre(30.5, 40.5, 50.5));
  }
}

TEST_F(CodaNetcdfCursorTest, RecordUnlimitedVariablesFullPartialAndElements) {
  // 2D record NC_SHORT [rec=2, 3] with vsize=8 (6 bytes data + 2 bytes pad),
  // exercising record_size != block_size and partial reads spanning records.
  {
    TempFile nc_file("netcdf_cursor_rec_i16.nc");
    NetcdfBuilder builder(1);
    builder.set_num_recs(2);
    builder.AddDim({.name = "rec", .size = 0});
    builder.AddDim({.name = "col", .size = 3});

    std::vector<uint8_t> rec_data =
        EncodeBe(std::array<int16_t, 3>{11, 12, 13});
    rec_data.insert(rec_data.end(), {0, 0});  // 4-byte record padding
    const std::vector<uint8_t> rec1 =
        EncodeBe(std::array<int16_t, 3>{21, 22, 23});
    rec_data.insert(rec_data.end(), rec1.begin(), rec1.end());
    rec_data.insert(rec_data.end(), {0, 0});

    builder.AddVar({
        .name = "rec_mat_i16",
        .dim_ids = {0, 1},
        .attrs = {},
        .type = NetcdfType::kShort,
        .vsize = 8,
        .data = rec_data,
    });

    ScopedProduct product;
    OpenBuiltNetcdf(nc_file, builder, product);
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    coda_cursor c = GotoField(root, "rec_mat_i16");

    std::vector<int16_t> full(6, -1);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(
                                &c, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full, ElementsAre(11, 12, 13, 21, 22, 23));

    // Partial slice [1..5) spans both records: hits offset > i*3 (i=0) and
    // offset + length < (i+1)*3 (i=1) in read_partial_array().
    std::vector<int16_t> partial(6, 0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int16_partial_array(&c, 1, 4, partial.data()));
    EXPECT_THAT(std::span<const int16_t>(partial.data(), 4),
                ElementsAre(12, 13, 21, 22));

    // Element-by-element reads across records via subscript and index.
    coda_cursor elem = c;
    constexpr std::array<long, 2> kSub10 = {1, 0};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element(&elem, 2, kSub10.data()));
    int16_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&elem, &v));
    EXPECT_EQ(21, v);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&elem, &v));
    EXPECT_EQ(22, v);
  }

  // 2D record NC_DOUBLE [rec=2, 3] covering 64-bit record read & partial read.
  {
    TempFile nc_file("netcdf_cursor_rec_f64.nc");
    NetcdfBuilder builder(1);
    builder.set_num_recs(2);
    builder.AddDim({.name = "rec", .size = 0});
    builder.AddDim({.name = "col", .size = 3});

    constexpr std::array<double, 6> kRecDoubles = {1.1, 2.2, 3.3,
                                                   4.4, 5.5, 6.6};
    builder.AddVar({
        .name = "rec_mat_f64",
        .dim_ids = {0, 1},
        .attrs = {},
        .type = NetcdfType::kDouble,
        .vsize = 24,
        .data = EncodeBe(kRecDoubles),
    });

    ScopedProduct product;
    OpenBuiltNetcdf(nc_file, builder, product);
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    coda_cursor c = GotoField(root, "rec_mat_f64");

    std::vector<double> full(6, -1.0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full, ElementsAreArray(kRecDoubles));

    std::vector<double> partial(6, 0.0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_partial_array(&c, 1, 4, partial.data()));
    EXPECT_THAT(std::span<const double>(partial.data(), 4),
                ElementsAre(2.2, 3.3, 4.4, 5.5));
  }
}

TEST_F(CodaNetcdfCursorTest, ConversionsOptionEnableAndDisable) {
  ScopedCodaOptions restore_options;
  TempFile nc_file("netcdf_cursor_conversions.nc");
  NetcdfBuilder builder(1);
  builder.AddDim({.name = "n4", .size = 4});

  // 1. Scalar NC_SHORT with double scale_factor, add_offset, and _FillValue.
  builder.AddVar({
      .name = "packed_i16",
      .dim_ids = {},
      .attrs =
          {
              {.name = "scale_factor",
               .type = NetcdfType::kDouble,
               .count = 1,
               .raw_values = EncodeBeScalar<double>(0.5)},
              {.name = "add_offset",
               .type = NetcdfType::kDouble,
               .count = 1,
               .raw_values = EncodeBeScalar<double>(10.0)},
              {.name = "_FillValue",
               .type = NetcdfType::kShort,
               .count = 1,
               .raw_values = EncodeBeScalar<int16_t>(-999)},
          },
      .type = NetcdfType::kShort,
      .vsize = 4,
      .data = EncodeBeScalar<int16_t>(100),
  });
  builder.AddVar({
      .name = "packed_fill_i16",
      .dim_ids = {},
      .attrs =
          {
              {.name = "scale_factor",
               .type = NetcdfType::kDouble,
               .count = 1,
               .raw_values = EncodeBeScalar<double>(0.5)},
              {.name = "add_offset",
               .type = NetcdfType::kDouble,
               .count = 1,
               .raw_values = EncodeBeScalar<double>(10.0)},
              {.name = "_FillValue",
               .type = NetcdfType::kShort,
               .count = 1,
               .raw_values = EncodeBeScalar<int16_t>(-999)},
          },
      .type = NetcdfType::kShort,
      .vsize = 4,
      .data = EncodeBeScalar<int16_t>(-999),
  });

  // 2. 1D NC_BYTE array with float scale_factor/add_offset and missing_value.
  constexpr std::array<int8_t, 4> kPackedBytes = {10, 20, -128, 30};
  builder.AddVar({
      .name = "packed_arr_i8",
      .dim_ids = {0},
      .attrs =
          {
              {.name = "scale_factor",
               .type = NetcdfType::kFloat,
               .count = 1,
               .raw_values = EncodeBeScalar<float>(2.0f)},
              {.name = "add_offset",
               .type = NetcdfType::kFloat,
               .count = 1,
               .raw_values = EncodeBeScalar<float>(-5.0f)},
              {.name = "missing_value",
               .type = NetcdfType::kByte,
               .count = 1,
               .raw_values = EncodeBeScalar<int8_t>(-128)},
          },
      .type = NetcdfType::kByte,
      .vsize = 4,
      .data = EncodeBe(kPackedBytes),
  });

  // 3. NC_FLOAT scalar with _FillValue only (no scale/offset).
  builder.AddVar({
      .name = "float_fill_only",
      .dim_ids = {},
      .attrs =
          {
              {.name = "_FillValue",
               .type = NetcdfType::kFloat,
               .count = 1,
               .raw_values = EncodeBeScalar<float>(-999.0f)},
          },
      .type = NetcdfType::kFloat,
      .vsize = 4,
      .data = EncodeBeScalar<float>(-999.0f),
  });

  ScopedProduct product;
  OpenBuiltNetcdf(nc_file, builder, product);

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // With conversions enabled (default = 1).
  ASSERT_EQ(1, coda_get_option_perform_conversions());
  {
    coda_cursor c = GotoField(root, "packed_i16");
    coda_native_type rt = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&c, &rt));
    EXPECT_EQ(coda_native_type_double, rt);

    int16_t raw = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int16(&c, &raw));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    double dval = 0.0;
    float fval = 0.0f;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &dval));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &fval));
    EXPECT_THAT(dval, DoubleEq(60.0));
    EXPECT_THAT(fval, FloatEq(60.0f));
  }
  {
    coda_cursor c = GotoField(root, "packed_fill_i16");
    double dval = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &dval));
    EXPECT_THAT(dval, IsNan());
  }
  {
    coda_cursor c = GotoField(root, "float_fill_only");
    double dval = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &dval));
    EXPECT_THAT(dval, IsNan());
  }
  {
    coda_cursor c = GotoField(root, "packed_arr_i8");
    std::vector<double> darr(4, 0.0);
    std::vector<float> farr(4, 0.0f);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, darr.data(), coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(
                                &c, farr.data(), coda_array_ordering_c));
    EXPECT_THAT(darr, ElementsAre(DoubleEq(15.0), DoubleEq(35.0), IsNan(),
                                  DoubleEq(55.0)));
    EXPECT_THAT(farr, ElementsAre(FloatEq(15.0f), FloatEq(35.0f), IsNan(),
                                  FloatEq(55.0f)));

    // 8-bit NC_BYTE partial array has no swap loop, safe at length < 4.
    std::vector<double> dpart(3, 0.0);
    std::vector<float> fpart(3, 0.0f);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_partial_array(&c, 1, 3, dpart.data()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_float_partial_array(&c, 1, 3, fpart.data()));
    EXPECT_THAT(dpart, ElementsAre(DoubleEq(35.0), IsNan(), DoubleEq(55.0)));
    EXPECT_THAT(fpart, ElementsAre(FloatEq(35.0f), IsNan(), FloatEq(55.0f)));
  }

  // With conversions disabled (0).
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
  {
    coda_cursor c = GotoField(root, "packed_i16");
    coda_native_type rt = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&c, &rt));
    EXPECT_EQ(coda_native_type_int16, rt);

    int16_t raw = 0;
    double dval = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &raw));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &dval));
    EXPECT_EQ(100, raw);
    EXPECT_THAT(dval, DoubleEq(100.0));
  }
  {
    coda_cursor c = GotoField(root, "packed_fill_i16");
    double dval = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &dval));
    EXPECT_THAT(dval, DoubleEq(-999.0));
  }
  {
    coda_cursor c = GotoField(root, "float_fill_only");
    float fval = 0.0f;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &fval));
    EXPECT_THAT(fval, FloatEq(-999.0f));
  }
  {
    coda_cursor c = GotoField(root, "packed_arr_i8");
    std::vector<int8_t> raw_arr(4, 0);
    std::vector<int8_t> raw_part(2, 0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8_array(&c, raw_arr.data(),
                                                        coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int8_partial_array(&c, 1, 2, raw_part.data()));
    EXPECT_THAT(raw_arr, ElementsAre(10, 20, -128, 30));
    EXPECT_THAT(raw_part, ElementsAre(20, -128));
  }
}

TEST_F(CodaNetcdfCursorTest, BoundaryChecksOptionEnableAndDisable) {
  ScopedCodaOptions restore_options;
  TempFile nc_file("netcdf_cursor_bounds.nc");
  NetcdfBuilder builder(1);
  builder.AddDim({.name = "r", .size = 2});
  builder.AddDim({.name = "c", .size = 3});
  constexpr std::array<int32_t, 6> kMat = {1, 2, 3, 4, 5, 6};
  builder.AddVar({
      .name = "mat",
      .dim_ids = {0, 1},
      .attrs = {},
      .type = NetcdfType::kInt,
      .vsize = 24,
      .data = EncodeBe(kMat),
  });

  ScopedProduct product;
  OpenBuiltNetcdf(nc_file, builder, product);
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
  coda_cursor mat = GotoField(root, "mat");

  // 1. Boundary checks enabled (default = 1).
  ASSERT_EQ(1, coda_get_option_perform_boundary_checks());
  {
    coda_cursor c = mat;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&c, -1));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&c, 6));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 5));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&c));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  }
  {
    std::array<int32_t, 6> dst{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_cursor_read_int32_partial_array(&mat, -1, 1, dst.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_partial_array(&mat, 6, 1, dst.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_partial_array(&mat, 4, 3, dst.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  }
  {
    coda_cursor c = mat;
    constexpr std::array<long, 1> kSub1 = {0};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&c, 1, kSub1.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_NUM_DIMS_MISMATCH, coda_errno);

    constexpr std::array<long, 2> kNegRow = {-1, 0};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&c, 2, kNegRow.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    constexpr std::array<long, 2> kOobCol = {0, 3};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&c, 2, kOobCol.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  }

  // 2. Boundary checks disabled (0): index checks skipped (do not read OOB!).
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
  {
    coda_cursor c = mat;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 6));
    long idx = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &idx));
    EXPECT_EQ(6, idx);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 5));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &idx));
    EXPECT_EQ(6, idx);

    // Negative index allowed when checks disabled; re-enabling catches < 0.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, -3));
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&c));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  }
  {
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
    // Subscript navigation always checks bounds even when option is 0.
    coda_cursor c = mat;
    constexpr std::array<long, 2> kOobRow = {2, 0};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&c, 2, kOobRow.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    // Partial read past end skips cursor check and hits read_bytes EOF error.
    std::array<int32_t, 6> dst{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_partial_array(&mat, 6, 1, dst.data()));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }
}

TEST_F(CodaNetcdfCursorTest, AttributesNavigationPresentAndAbsent) {
  // File with global attributes, scalar var with attrs, array var with attrs,
  // and var without attrs.
  {
    TempFile nc_file("netcdf_cursor_attrs.nc");
    NetcdfBuilder builder(1);
    builder.AddDim({.name = "n2", .size = 2});
    builder.AddGlobalAttr({
        .name = "g_str",
        .type = NetcdfType::kChar,
        .count = 5,
        .raw_values = EncodeBe(std::string_view("hello")),
    });
    builder.AddGlobalAttr({
        .name = "g_ch",
        .type = NetcdfType::kChar,
        .count = 1,
        .raw_values = EncodeBeScalar<char>('Q'),
    });
    builder.AddGlobalAttr({
        .name = "g_arr",
        .type = NetcdfType::kFloat,
        .count = 3,
        .raw_values = EncodeBe(std::array<float, 3>{1.5f, 2.5f, 3.5f}),
    });
    builder.AddVar({
        .name = "var_with_attr",
        .dim_ids = {},
        .attrs =
            {
                {.name = "units",
                 .type = NetcdfType::kChar,
                 .count = 1,
                 .raw_values = EncodeBeScalar<char>('m')},
                {.name = "code",
                 .type = NetcdfType::kInt,
                 .count = 1,
                 .raw_values = EncodeBeScalar<int32_t>(77)},
            },
        .type = NetcdfType::kDouble,
        .vsize = 8,
        .data = EncodeBeScalar<double>(9.25),
    });
    builder.AddVar({
        .name = "arr_with_attr",
        .dim_ids = {0},
        .attrs =
            {
                {.name = "axis",
                 .type = NetcdfType::kChar,
                 .count = 1,
                 .raw_values = EncodeBeScalar<char>('X')},
            },
        .type = NetcdfType::kInt,
        .vsize = 8,
        .data = EncodeBe(std::array<int32_t, 2>{10, 20}),
    });
    builder.AddVar({
        .name = "var_no_attr",
        .dim_ids = {},
        .attrs = {},
        .type = NetcdfType::kInt,
        .vsize = 4,
        .data = EncodeBeScalar<int32_t>(5),
    });

    ScopedProduct product;
    OpenBuiltNetcdf(nc_file, builder, product);
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

    // Global attributes on root.
    int has_attrs = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&root, &has_attrs));
    EXPECT_EQ(1, has_attrs);

    coda_cursor gattrs = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&gattrs));
    long idx = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&gattrs, &idx));
    EXPECT_EQ(-1, idx);

    long num_gattrs = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&gattrs, &num_gattrs));
    EXPECT_EQ(3, num_gattrs);

    coda_cursor a_str = GotoField(gattrs, "g_str");
    std::array<char, 6> str_buf{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&a_str, str_buf.data(), 6));
    EXPECT_STREQ("hello", str_buf.data());

    // Attribute field itself has no attributes -> empty record.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&a_str, &has_attrs));
    EXPECT_EQ(0, has_attrs);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&a_str));
    long empty_count = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&a_str, &empty_count));
    EXPECT_EQ(0, empty_count);

    coda_cursor a_ch = GotoField(gattrs, "g_ch");
    char ch = '\0';
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&a_ch, &ch));
    EXPECT_EQ('Q', ch);

    coda_cursor a_arr = GotoField(gattrs, "g_arr");
    std::vector<float> fvec(3, 0.0f);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(
                                &a_arr, fvec.data(), coda_array_ordering_c));
    EXPECT_THAT(fvec, ElementsAre(1.5f, 2.5f, 3.5f));

    // Scalar variable with attributes.
    coda_cursor v_attr = GotoField(root, "var_with_attr");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&v_attr, &has_attrs));
    EXPECT_EQ(1, has_attrs);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&v_attr));
    coda_cursor code = GotoField(v_attr, "code");
    int32_t code_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&code, &code_val));
    EXPECT_EQ(77, code_val);

    // Array variable with attributes; its elements have no attributes.
    coda_cursor arr_c = GotoField(root, "arr_with_attr");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&arr_c, &has_attrs));
    EXPECT_EQ(1, has_attrs);
    {
      coda_cursor attr_rec = arr_c;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr_rec));
      coda_cursor axis = GotoField(attr_rec, "axis");
      char ax = '\0';
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&axis, &ax));
      EXPECT_EQ('X', ax);
    }
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&arr_c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&arr_c, &has_attrs));
    EXPECT_EQ(0, has_attrs);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&arr_c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&arr_c, &empty_count));
    EXPECT_EQ(0, empty_count);

    // Scalar variable without attributes -> empty record.
    coda_cursor v_no = GotoField(root, "var_no_attr");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&v_no, &has_attrs));
    EXPECT_EQ(0, has_attrs);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&v_no));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&v_no, &empty_count));
    EXPECT_EQ(0, empty_count);
  }

  // File with zero global attributes -> root goto_attributes yields empty.
  {
    TempFile nc_file("netcdf_cursor_no_gattrs.nc");
    NetcdfBuilder builder(1);
    builder.AddVar({
        .name = "x",
        .dim_ids = {},
        .attrs = {},
        .type = NetcdfType::kInt,
        .vsize = 4,
        .data = EncodeBeScalar<int32_t>(1),
    });

    ScopedProduct product;
    OpenBuiltNetcdf(nc_file, builder, product);
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    int has_attrs = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&root, &has_attrs));
    EXPECT_EQ(0, has_attrs);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&root));
    long empty_count = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&root, &empty_count));
    EXPECT_EQ(0, empty_count);
  }
}

TEST_F(CodaNetcdfCursorTest, NetcdfV2AndV5Formats) {
  for (const int version : {2, 5}) {
    TempFile nc_file(version == 2 ? "netcdf_cursor_v2.nc"
                                  : "netcdf_cursor_v5.nc");
    NetcdfBuilder builder(version);
    builder.AddDim({.name = "d", .size = 3});
    builder.AddGlobalAttr({
        .name = "ver",
        .type = NetcdfType::kInt,
        .count = 1,
        .raw_values = EncodeBeScalar<int32_t>(version),
    });
    constexpr std::array<double, 3> kVals = {1.25, 2.5, 3.75};
    builder.AddVar({
        .name = "vals",
        .dim_ids = {0},
        .attrs = {},
        .type = NetcdfType::kDouble,
        .vsize = 24,
        .data = EncodeBe(kVals),
    });

    ScopedProduct product;
    OpenBuiltNetcdf(nc_file, builder, product);
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

    coda_cursor vals = GotoField(root, "vals");
    std::vector<double> out(3, 0.0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &vals, out.data(), coda_array_ordering_c));
    EXPECT_THAT(out, ElementsAreArray(kVals));

    coda_cursor attrs = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attrs));
    coda_cursor ver = GotoField(attrs, "ver");
    int32_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&ver, &v));
    EXPECT_EQ(version, v);
  }
}

TEST_F(CodaNetcdfCursorTest, TruncatedPayloadReadErrorsAndTypeMismatches) {
  // 1. Truncated scalar int32 (only 2 bytes present after offset).
  {
    TempFile nc_file("netcdf_cursor_trunc_scalar.nc");
    NetcdfBuilder builder(1);
    builder.AddVar({
        .name = "s",
        .dim_ids = {},
        .attrs = {},
        .type = NetcdfType::kInt,
        .vsize = 4,
        .data = EncodeBeScalar<int32_t>(1234),
    });
    std::vector<uint8_t> bytes = builder.Build();
    bytes.resize(bytes.size() - 2);
    ASSERT_TRUE(nc_file.WriteBytes(bytes));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(nc_file.c_str(), product.OutParam()));
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    coda_cursor c = GotoField(root, "s");

    int32_t out = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32(&c, &out));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

    // Direct internal call on non-array cursor triggers coda_type_get_array_dim
    // failure in coda_netcdf_cursor_goto_array_element().
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_netcdf_cursor_goto_array_element(&c, 0, nullptr));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  }

  // 2. Truncated string (only 2 bytes present for 10-byte string).
  {
    TempFile nc_file("netcdf_cursor_trunc_string.nc");
    NetcdfBuilder builder(1);
    builder.AddDim({.name = "slen", .size = 10});
    builder.AddVar({
        .name = "str",
        .dim_ids = {0},
        .attrs = {},
        .type = NetcdfType::kChar,
        .vsize = 12,
        .data = EncodeBe(std::string_view("0123456789")),
    });
    std::vector<uint8_t> bytes = builder.Build();
    // Keep only 2 bytes after string var_offset (Builder padded 10->12 bytes).
    bytes.resize(bytes.size() - 10);
    ASSERT_TRUE(nc_file.WriteBytes(bytes));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(nc_file.c_str(), product.OutParam()));
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    coda_cursor c = GotoField(root, "str");

    std::array<char, 4> buf{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_string(&c, buf.data(), buf.size()));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  // 3. Truncated fixed array (4 bytes present for 16-byte int32[4]).
  {
    TempFile nc_file("netcdf_cursor_trunc_fixed_arr.nc");
    NetcdfBuilder builder(1);
    builder.AddDim({.name = "n4", .size = 4});
    builder.AddVar({
        .name = "arr",
        .dim_ids = {0},
        .attrs = {},
        .type = NetcdfType::kInt,
        .vsize = 16,
        .data = EncodeBe(std::array<int32_t, 4>{1, 2, 3, 4}),
    });
    std::vector<uint8_t> bytes = builder.Build();
    bytes.resize(bytes.size() - 12);
    ASSERT_TRUE(nc_file.WriteBytes(bytes));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(nc_file.c_str(), product.OutParam()));
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    coda_cursor c = GotoField(root, "arr");

    std::vector<int32_t> dst(4, 0);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_array(&c, dst.data(),
                                               coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_partial_array(&c, 0, 4, dst.data()));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  // 4. Truncated record array (only 1 of 2 records present).
  {
    TempFile nc_file("netcdf_cursor_trunc_rec_arr.nc");
    NetcdfBuilder builder(1);
    builder.set_num_recs(2);
    builder.AddDim({.name = "rec", .size = 0});
    builder.AddVar({
        .name = "rarr",
        .dim_ids = {0},
        .attrs = {},
        .type = NetcdfType::kInt,
        .vsize = 4,
        .data = EncodeBe(std::array<int32_t, 2>{1, 2}),
    });
    std::vector<uint8_t> bytes = builder.Build();
    bytes.resize(bytes.size() - 4);
    ASSERT_TRUE(nc_file.WriteBytes(bytes));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(nc_file.c_str(), product.OutParam()));
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    coda_cursor c = GotoField(root, "rarr");

    std::vector<int32_t> dst(2, 0);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_array(&c, dst.data(),
                                               coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_partial_array(&c, 0, 2, dst.data()));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  // 5. Invalid arguments & type-mismatch errors on valid NetCDF cursors.
  {
    TempFile nc_file("netcdf_cursor_type_errors.nc");
    NetcdfBuilder builder(1);
    builder.AddDim({.name = "n2", .size = 2});
    builder.AddVar({
        .name = "s_byte",
        .dim_ids = {},
        .attrs = {},
        .type = NetcdfType::kByte,
        .vsize = 4,
        .data = EncodeBeScalar<int8_t>(1),
    });
    builder.AddVar({
        .name = "s_int",
        .dim_ids = {},
        .attrs = {},
        .type = NetcdfType::kInt,
        .vsize = 4,
        .data = EncodeBeScalar<int32_t>(2),
    });
    builder.AddVar({
        .name = "s_dbl",
        .dim_ids = {},
        .attrs = {},
        .type = NetcdfType::kDouble,
        .vsize = 8,
        .data = EncodeBeScalar<double>(3.0),
    });
    builder.AddVar({
        .name = "a_int",
        .dim_ids = {0},
        .attrs = {},
        .type = NetcdfType::kInt,
        .vsize = 8,
        .data = EncodeBe(std::array<int32_t, 2>{10, 20}),
    });

    ScopedProduct product;
    OpenBuiltNetcdf(nc_file, builder, product);
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

    coda_cursor c_byte = GotoField(root, "s_byte");
    coda_cursor c_int = GotoField(root, "s_int");
    coda_cursor c_dbl = GotoField(root, "s_dbl");
    coda_cursor c_arr = GotoField(root, "a_int");

    // NULL arguments.
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_set_product(nullptr, product.get()));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_cursor dummy{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_set_product(&dummy, nullptr));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32(&c_int, nullptr));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    // Type mismatches on scalar cursors.
    uint8_t u8 = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_uint8(&c_byte, &u8));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    int16_t i16 = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int16(&c_int, &i16));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    int64_t i64 = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int64(&c_dbl, &i64));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    char ch = '\0';
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_char(&c_byte, &ch));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    std::array<char, 8> str_buf{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_string(&c_int, str_buf.data(), 8));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    long str_len = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_string_length(&c_int, &str_len));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    std::array<double, 2> cplx{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_complex_double_pair(&c_dbl, cplx.data()));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    // Array calls on scalar cursor.
    int32_t scalar_dst = 0;
    int num_dims = 0;
    std::array<long, CODA_MAX_NUM_DIMS> dims{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_array_dim(&c_int, &num_dims, dims.data()));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_array(&c_int, &scalar_dst,
                                               coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_cursor_read_int32_partial_array(&c_int, 0, 1, &scalar_dst));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&c_int, 0));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    // Element-type mismatch on array & partial array reads.
    std::array<int16_t, 2> arr16{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int16_array(&c_arr, arr16.data(),
                                               coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_cursor_read_int16_partial_array(&c_arr, 0, 1, arr16.data()));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  }
}

// TODO(schwehr): 98.8% line coverage in libcoda/coda-netcdf-cursor.c (253/256);
// remaining 3 missed lines (262, 347, 422) are defensive size/type checks
// guarded by earlier product validation.

}  // namespace
}  // namespace stcorp_coda
