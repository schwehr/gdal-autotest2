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
// Tests CODA CDF cursor navigation, metadata queries, scalar reads, array
// reads, partial array reads, string reads, endianness, Fortran vs C ordering,
// sparse records (padded and previous), gzip-compressed CVVR/VVR/VXR trees,
// EPOCH/TIME_TT2000/EPOCH16 handling, attributes, runtime options, and error
// paths (libcoda/coda-cdf-cursor.c).
//
// google3 build caveats:
//   - Compiled on little-endian LP64 Linux (WORDS_BIGENDIAN and WIN32 branches
//     are compiled out).
//   - Upstream coda-cdf.c omits `offset += 340` for rVariables when
//     `rnum_dims > 0`, so valid rVariable tests keep `rdims` empty.
//   - Upstream coda-cdf-cursor.c computes `record_to_id = (offset + length) /
//     num_values_per_record` in `read_partial_array()`, which reads one past
//     `variable->offset[]` when `offset + length == total_elements`; partial
//     array tests keep `offset + length < total_elements`.
//   - Upstream coda_cdf_cursor_read_string() writes `dst[dst_size - 1] = '\0'`,
//     so callers must pass `dst_size = length + 1` (or zero-fill larger
//     buffers).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cdf-cursor.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cdf.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cdf.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/stcorp_coda/google/util/cdf_builder.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-cdf.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

class CodaCdfCursorTest : public ::testing::Test {
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

  ScopedCodaOptions options_;
  ScopedEnvVar env_def_{"CODA_DEFINITION"};
  ScopedEnvVar env_leap_{"CODA_LEAP_SECOND_TABLE"};
  ScopedCodaSession session_;
};

void OpenCdfBytes(const TempFile& file, std::span<const uint8_t> bytes,
                  ScopedProduct& product) {
  ASSERT_TRUE(file.WriteBytes(bytes));
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));
  ASSERT_NE(nullptr, product.get());
}

void OpenBuiltCdf(const TempFile& file, const CdfBuilder& builder,
                  ScopedProduct& product) {
  const std::vector<uint8_t> bytes = builder.Build();
  OpenCdfBytes(file, bytes, product);
}

coda_cursor GotoField(const coda_cursor& parent, const char* name) {
  coda_cursor child = parent;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&child, name));
  return child;
}

template <typename T>
CdfVarSpec MakeScalarVar(std::string name, CdfDataType type, T value,
                         bool big_endian) {
  CdfVarSpec spec;
  spec.name = std::move(name);
  spec.data_type = type;
  spec.num_elems = 1;
  spec.rec_varys = false;
  spec.data = CdfBuilder::EncodeValue<T>(value, big_endian);
  return spec;
}

CdfVarSpec MakeStringVar(std::string name, CdfDataType type, int32_t str_len,
                         std::string_view text, bool rec_varys = false) {
  CdfVarSpec spec;
  spec.name = std::move(name);
  spec.data_type = type;
  spec.num_elems = str_len;
  spec.rec_varys = rec_varys;
  spec.data.assign(text.begin(), text.end());
  return spec;
}

template <typename T>
CdfVarSpec MakeArrayVar(std::string name, CdfDataType type, bool rec_varys,
                        std::vector<int32_t> dims, std::span<const T> values,
                        bool big_endian) {
  CdfVarSpec spec;
  spec.name = std::move(name);
  spec.data_type = type;
  spec.num_elems = 1;
  spec.rec_varys = rec_varys;
  spec.dims = std::move(dims);
  spec.data = CdfBuilder::EncodeValues<T>(values, big_endian);
  return spec;
}

TEST_F(CodaCdfCursorTest, OpenErrorsAndRootCursorMetadata) {
  // 1. Missing file fails with CODA_ERROR_FILE_NOT_FOUND.
  const TempFile missing("cdf_cursor_missing.cdf");
  ScopedProduct missing_prod;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing.c_str(), missing_prod.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  // 2. Truncated/corrupt CDF header fails with CODA_ERROR_PRODUCT.
  const TempFile corrupt("cdf_cursor_corrupt.cdf");
  constexpr std::array<uint8_t, 16> kTruncatedCdf = {
      0xCD, 0xF3, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38,
  };
  ASSERT_TRUE(corrupt.WriteBytes(kTruncatedCdf));
  ScopedProduct corrupt_prod;
  EXPECT_EQ(-1, coda_open(corrupt.c_str(), corrupt_prod.OutParam()));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  // 3. Valid empty CDF sets root cursor and reports unsupported raw bit/byte
  // reads cleanly.
  const TempFile valid("cdf_cursor_root.cdf");
  CdfBuilder builder;
  ScopedProduct product;
  ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(valid, builder, product));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  coda_format format = coda_format_ascii;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&root, &format));
  EXPECT_EQ(coda_format_cdf, format);

  coda_type_class type_class = coda_special_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&root, &type_class));
  EXPECT_EQ(coda_record_class, type_class);

  int depth = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&root, &depth));
  EXPECT_EQ(0, depth);

  long num_elements = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&root, &num_elements));
  EXPECT_EQ(0, num_elements);

  int64_t bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&root, &bit_size));
  EXPECT_EQ(-1, bit_size);

  int64_t byte_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&root, &byte_size));
  EXPECT_EQ(-1, byte_size);

  int64_t bit_offset = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_bit_offset(&root, &bit_offset));
  EXPECT_EQ(-1, bit_offset);

  int64_t byte_offset = 0;
  EXPECT_EQ(-1, coda_cursor_get_file_byte_offset(&root, &byte_offset));
  EXPECT_EQ(-1, byte_offset);

  std::array<uint8_t, 4> raw_buf = {0xAA, 0xAA, 0xAA, 0xAA};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_bits(&root, raw_buf.data(), 0, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&root, raw_buf.data(), 0, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
}

TEST_F(CodaCdfCursorTest, ScalarAllTypesBigAndLittleEndian) {
  struct EndianCase {
    CdfEncoding encoding;
    const char* tag;
  };
  constexpr std::array<EndianCase, 2> kEndians = {{
      {.encoding = CdfEncoding::kNetwork, .tag = "be"},
      {.encoding = CdfEncoding::kIbmPc, .tag = "le"},
  }};

  for (const EndianCase& ec : kEndians) {
    SCOPED_TRACE(ec.tag);
    CdfBuilder builder;
    builder.SetEncoding(ec.encoding);
    const bool be = builder.is_big_endian();

    builder
        .AddRVar(MakeScalarVar<int8_t>("r_int1", CdfDataType::kInt1, -42, be))
        .AddZVar(MakeScalarVar<int8_t>("z_int1", CdfDataType::kInt1, -17, be))
        .AddZVar(MakeScalarVar<int8_t>("z_byte", CdfDataType::kByte, -100, be))
        .AddZVar(
            MakeScalarVar<uint8_t>("z_uint1", CdfDataType::kUint1, 213, be))
        .AddZVar(
            MakeScalarVar<int16_t>("z_int2", CdfDataType::kInt2, -12345, be))
        .AddZVar(
            MakeScalarVar<uint16_t>("z_uint2", CdfDataType::kUint2, 54321, be))
        .AddZVar(MakeScalarVar<int32_t>("z_int4", CdfDataType::kInt4,
                                        -0x1234567, be))
        .AddZVar(MakeScalarVar<uint32_t>("z_uint4", CdfDataType::kUint4,
                                         0xF1234567U, be))
        .AddZVar(MakeScalarVar<int64_t>("z_int8", CdfDataType::kInt8,
                                        -0x0123456789ABCDEFLL, be))
        .AddZVar(
            MakeScalarVar<float>("z_real4", CdfDataType::kReal4, -3.25f, be))
        .AddZVar(
            MakeScalarVar<float>("z_float", CdfDataType::kFloat, 12.5f, be))
        .AddZVar(MakeScalarVar<double>("z_real8", CdfDataType::kReal8,
                                       -1234.625, be))
        .AddZVar(MakeScalarVar<double>("z_double", CdfDataType::kDouble,
                                       9876.125, be))
        .AddZVar(MakeStringVar("z_char1", CdfDataType::kChar, 1, "Z"))
        .AddZVar(MakeStringVar("z_uchar1", CdfDataType::kUchar, 1, "a"))
        .AddZVar(MakeStringVar("z_char6", CdfDataType::kChar, 6, "CDFstr"))
        .AddZVar(MakeStringVar("z_uchar5", CdfDataType::kUchar, 5, "Ustr!"));

    const TempFile file(std::string("cdf_scalar_") + ec.tag + ".cdf");
    ScopedProduct product;
    ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

    // Verify scalar element counts and values across all numeric types.
    {
      coda_cursor c = GotoField(root, "r_int1");
      long n = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &n));
      EXPECT_EQ(1, n);
      int8_t v = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&c, &v));
      EXPECT_EQ(-42, v);
    }
    {
      coda_cursor c = GotoField(root, "z_int1");
      int8_t v = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&c, &v));
      EXPECT_EQ(-17, v);
    }
    {
      coda_cursor c = GotoField(root, "z_byte");
      int8_t v = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&c, &v));
      EXPECT_EQ(-100, v);
    }
    {
      coda_cursor c = GotoField(root, "z_uint1");
      uint8_t v = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&c, &v));
      EXPECT_EQ(213U, v);
    }
    {
      coda_cursor c = GotoField(root, "z_int2");
      int16_t v = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &v));
      EXPECT_EQ(-12345, v);
    }
    {
      coda_cursor c = GotoField(root, "z_uint2");
      uint16_t v = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&c, &v));
      EXPECT_EQ(54321U, v);
    }
    {
      coda_cursor c = GotoField(root, "z_int4");
      int32_t v = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &v));
      EXPECT_EQ(-0x1234567, v);
    }
    {
      coda_cursor c = GotoField(root, "z_uint4");
      uint32_t v = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&c, &v));
      EXPECT_EQ(0xF1234567U, v);
    }
    {
      coda_cursor c = GotoField(root, "z_int8");
      int64_t v = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &v));
      EXPECT_EQ(-0x0123456789ABCDEFLL, v);
    }
    {
      coda_cursor c = GotoField(root, "z_real4");
      float v = 0.0f;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &v));
      EXPECT_EQ(-3.25f, v);
    }
    {
      coda_cursor c = GotoField(root, "z_float");
      float v = 0.0f;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &v));
      EXPECT_EQ(12.5f, v);
    }
    {
      coda_cursor c = GotoField(root, "z_real8");
      double v = 0.0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &v));
      EXPECT_EQ(-1234.625, v);
    }
    {
      coda_cursor c = GotoField(root, "z_double");
      double v = 0.0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &v));
      EXPECT_EQ(9876.125, v);
    }

    // CHAR / UCHAR 1-byte scalars work with read_char and read_string.
    {
      coda_cursor c = GotoField(root, "z_char1");
      char ch = '\0';
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&c, &ch));
      EXPECT_EQ('Z', ch);
      long len = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &len));
      EXPECT_EQ(1, len);
      std::array<char, 2> buf = {'?', '?'};
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, buf.data(), 2));
      EXPECT_STREQ("Z", buf.data());
    }
    {
      coda_cursor c = GotoField(root, "z_uchar1");
      char ch = '\0';
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&c, &ch));
      EXPECT_EQ('a', ch);
    }

    // Multi-character CHAR / UCHAR strings: full read and truncated dst_size
    // (exercises `size_boundary < value_size` in read_basic_type).
    {
      coda_cursor c = GotoField(root, "z_char6");
      long len = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &len));
      EXPECT_EQ(6, len);

      std::array<char, 7> full = {};
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, full.data(), 7));
      EXPECT_STREQ("CDFstr", full.data());

      std::array<char, 4> truncated = {'x', 'x', 'x', 'x'};
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, truncated.data(), 4));
      EXPECT_STREQ("CDF", truncated.data());
    }
    {
      coda_cursor c = GotoField(root, "z_uchar5");
      long len = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &len));
      EXPECT_EQ(5, len);
      std::array<char, 6> full = {};
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, full.data(), 6));
      EXPECT_STREQ("Ustr!", full.data());
    }
  }
}

TEST_F(CodaCdfCursorTest, ArrayNavigationBoundsAndBoundaryCheckOption) {
  CdfBuilder builder;
  builder.SetEncoding(CdfEncoding::kNetwork);
  const bool be = builder.is_big_endian();

  constexpr std::array<int32_t, 4> kRVec = {10, 20, 30, 40};
  constexpr std::array<int32_t, 6> kMat2x3 = {1, 2, 3, 4, 5, 6};
  constexpr std::array<int32_t, 12> kTensor2x2x3 = {
      100, 101, 102, 103, 104, 105, 200, 201, 202, 203, 204, 205,
  };
  constexpr std::array<int32_t, 6> kNonVaryDimVals = {7, 8, 9, 10, 11, 12};

  CdfVarSpec non_vary_var =
      MakeArrayVar<int32_t>("z_nonvary_dim", CdfDataType::kInt4,
                            /*rec_varys=*/true, {3, 4}, kNonVaryDimVals, be);
  non_vary_var.dim_varys = {1, 0};

  builder
      .AddRVar(MakeArrayVar<int32_t>("r_vec", CdfDataType::kInt4,
                                     /*rec_varys=*/true, {}, kRVec, be))
      .AddZVar(MakeArrayVar<int32_t>("z_mat", CdfDataType::kInt4,
                                     /*rec_varys=*/false, {2, 3}, kMat2x3, be))
      .AddZVar(MakeArrayVar<int32_t>("z_tensor", CdfDataType::kInt4,
                                     /*rec_varys=*/true, {2, 3}, kTensor2x2x3,
                                     be))
      .AddZVar(std::move(non_vary_var));

  const TempFile file("cdf_nav_bounds.cdf");
  ScopedProduct product;
  ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // 1. 1D rVariable navigation via subscripts, index, next, parent, and root.
  {
    coda_cursor c = GotoField(root, "r_vec");
    long n = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &n));
    EXPECT_EQ(4, n);

    int num_dims = -1;
    std::array<long, CODA_MAX_NUM_DIMS> dims = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&c, &num_dims, dims.data()));
    EXPECT_EQ(1, num_dims);
    EXPECT_EQ(4, dims[0]);

    const std::array<long, 1> sub = {2};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&c, 1, sub.data()));
    int32_t val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &val));
    EXPECT_EQ(30, val);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&c));

    // Direct call to coda_cdf_cursor_goto_array_element on a non-array
    // exercises `coda_type_get_array_dim != 0` in coda-cdf-cursor.c.
    coda_cursor non_array = root;
    const std::array<long, 1> dummy_sub = {0};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cdf_cursor_goto_array_element(&non_array, 1,
                                                     dummy_sub.data()));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  // 2. 2D non-record and 3D record-varying zVariable subscript math.
  {
    coda_cursor c = GotoField(root, "z_mat");
    const std::array<long, 2> sub = {1, 2};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&c, 2, sub.data()));
    int32_t val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &val));
    EXPECT_EQ(6, val);
    long idx = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &idx));
    EXPECT_EQ(5, idx);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&c));
    int depth = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&c, &depth));
    EXPECT_EQ(0, depth);
  }

  {
    coda_cursor c = GotoField(root, "z_tensor");
    int num_dims = -1;
    std::array<long, CODA_MAX_NUM_DIMS> dims = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&c, &num_dims, dims.data()));
    EXPECT_EQ(3, num_dims);
    EXPECT_EQ(2, dims[0]);
    EXPECT_EQ(2, dims[1]);
    EXPECT_EQ(3, dims[2]);

    const std::array<long, 3> sub = {1, 0, 2};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&c, 3, sub.data()));
    int32_t val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &val));
    EXPECT_EQ(202, val);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&c));

    // Rank mismatch and subscript out-of-bounds errors.
    const std::array<long, 2> bad_rank = {0, 0};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&c, 2, bad_rank.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_NUM_DIMS_MISMATCH, coda_errno);

    const std::array<long, 3> neg_sub = {0, -1, 0};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&c, 3, neg_sub.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    const std::array<long, 3> high_sub = {0, 2, 0};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&c, 3, high_sub.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  }

  // 3. Non-varying dimension is omitted from CODA array dimensions.
  {
    coda_cursor c = GotoField(root, "z_nonvary_dim");
    int num_dims = -1;
    std::array<long, CODA_MAX_NUM_DIMS> dims = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&c, &num_dims, dims.data()));
    EXPECT_EQ(2, num_dims);
    EXPECT_EQ(2, dims[0]);
    EXPECT_EQ(3, dims[1]);
    std::vector<int32_t> vals(6, -999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &c, vals.data(), coda_array_ordering_c));
    EXPECT_THAT(vals, ElementsAre(7, 8, 9, 10, 11, 12));
  }

  // 4. Boundary check option (1 vs 0) on goto_array_element_by_index and
  // goto_next_array_element.
  {
    coda_cursor c = GotoField(root, "r_vec");
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));
    EXPECT_EQ(1, coda_get_option_perform_boundary_checks());

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&c, -1));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&c, 4));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    // Step through all 4 elements, then verify stepping past the end fails.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&c));
    for (int i = 0; i < 4; ++i) {
      long idx = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &idx));
      EXPECT_EQ(i, idx);
      int32_t val = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &val));
      EXPECT_EQ((i + 1) * 10, val);
      if (i < 3) {
        ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&c));
      }
    }
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&c));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&c));

    // Disable boundary checks: index-based navigation accepts out-of-range
    // indices, while subscript navigation still enforces bounds.
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
    EXPECT_EQ(0, coda_get_option_perform_boundary_checks());

    const std::array<long, 1> oob_sub = {4};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&c, 1, oob_sub.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 4));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&c));
    long idx = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&c, &idx));
    EXPECT_EQ(5, idx);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&c));

    // Also test negative index in goto_next_array_element with checks
    // re-enabled.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, -3));
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&c));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST_F(CodaCdfCursorTest, AllTypesFullAndPartialArrayReadsBigAndLittleEndian) {
  struct EndianCase {
    CdfEncoding encoding;
    const char* tag;
  };
  constexpr std::array<EndianCase, 2> kEndians = {{
      {.encoding = CdfEncoding::kNetwork, .tag = "be"},
      {.encoding = CdfEncoding::kIbmPc, .tag = "le"},
  }};

  // Each numeric array has 3 records x 3 elements = 9 elements so partial reads
  // at offset=1, length=4 and offset=2, length=6 span record boundaries while
  // keeping (offset + length) < 9.
  constexpr std::array<int8_t, 9> kI8 = {-9, -5, -1, 0, 1, 5, 9, 42, -120};
  constexpr std::array<uint8_t, 9> kU8 = {0,   1,   2,   100, 128,
                                          200, 220, 250, 255};
  constexpr std::array<int16_t, 9> kI16 = {-30000, -100, -1,  0,    1,
                                           100,    500,  999, 32000};
  constexpr std::array<uint16_t, 9> kU16 = {0,    1,     255,   256,  1000,
                                            5000, 40000, 60000, 65535};
  constexpr std::array<int32_t, 9> kI32 = {
      -1000000, -500, -1, 0, 1, 500, 10000, 999999, 2000000000};
  constexpr std::array<uint32_t, 9> kU32 = {
      0U,          1U,          65536U,      100000U,    200000U,
      0x80000000U, 0x90000000U, 0xF0000000U, 0xFFFFFFFFU};
  constexpr std::array<int64_t, 9> kI64 = {
      -5000000000LL, -1000000LL,    -1LL,           0LL, 1LL, 1000000LL,
      5000000000LL,  10000000000LL, 9000000000000LL};
  constexpr std::array<float, 9> kF32 = {-8.5f, -2.25f, -0.5f, 0.0f, 0.25f,
                                         1.5f,  3.75f,  16.0f, 64.5f};
  constexpr std::array<double, 9> kF64 = {-128.5, -16.25, -0.5,   0.0,   0.125,
                                          2.5,    10.75,  256.25, 1024.5};
  constexpr std::array<uint8_t, 9> kChBytes = {'a', 'b', 'c', 'd', 'e',
                                               'f', 'g', 'h', 'i'};
  constexpr std::array<char, 9> kChExpected = {'a', 'b', 'c', 'd', 'e',
                                               'f', 'g', 'h', 'i'};

  for (const EndianCase& ec : kEndians) {
    SCOPED_TRACE(ec.tag);
    CdfBuilder builder;
    builder.SetEncoding(ec.encoding);
    const bool be = builder.is_big_endian();

    CdfVarSpec ch_spec;
    ch_spec.name = "a_char";
    ch_spec.data_type = CdfDataType::kChar;
    ch_spec.num_elems = 1;
    ch_spec.rec_varys = true;
    ch_spec.dims = {3};
    ch_spec.data.assign(kChBytes.begin(), kChBytes.end());

    CdfVarSpec uch_spec = ch_spec;
    uch_spec.name = "a_uchar";
    uch_spec.data_type = CdfDataType::kUchar;

    builder
        .AddZVar(MakeArrayVar<int8_t>("a_int1", CdfDataType::kInt1, true, {3},
                                      kI8, be))
        .AddZVar(MakeArrayVar<int8_t>("a_byte", CdfDataType::kByte, true, {3},
                                      kI8, be))
        .AddZVar(MakeArrayVar<uint8_t>("a_uint1", CdfDataType::kUint1, true,
                                       {3}, kU8, be))
        .AddZVar(MakeArrayVar<int16_t>("a_int2", CdfDataType::kInt2, true, {3},
                                       kI16, be))
        .AddZVar(MakeArrayVar<uint16_t>("a_uint2", CdfDataType::kUint2, true,
                                        {3}, kU16, be))
        .AddZVar(MakeArrayVar<int32_t>("a_int4", CdfDataType::kInt4, true, {3},
                                       kI32, be))
        .AddZVar(MakeArrayVar<uint32_t>("a_uint4", CdfDataType::kUint4, true,
                                        {3}, kU32, be))
        .AddZVar(MakeArrayVar<int64_t>("a_int8", CdfDataType::kInt8, true, {3},
                                       kI64, be))
        .AddZVar(MakeArrayVar<float>("a_real4", CdfDataType::kReal4, true, {3},
                                     kF32, be))
        .AddZVar(MakeArrayVar<float>("a_float", CdfDataType::kFloat, true, {3},
                                     kF32, be))
        .AddZVar(MakeArrayVar<double>("a_real8", CdfDataType::kReal8, true, {3},
                                      kF64, be))
        .AddZVar(MakeArrayVar<double>("a_double", CdfDataType::kDouble, true,
                                      {3}, kF64, be))
        .AddZVar(std::move(ch_spec))
        .AddZVar(std::move(uch_spec))
        .AddZVar(MakeStringVar("a_str", CdfDataType::kChar, 4, "ab  cd  ef  ",
                               /*rec_varys=*/true));

    const TempFile file(std::string("cdf_arrays_") + ec.tag + ".cdf");
    ScopedProduct product;
    ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

    // int8 (INT1 & BYTE)
    for (const char* name : {"a_int1", "a_byte"}) {
      coda_cursor c = GotoField(root, name);
      std::vector<int8_t> full(9, -99);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8_array(
                                  &c, full.data(), coda_array_ordering_c));
      EXPECT_THAT(full, ElementsAreArray(kI8));

      std::vector<int8_t> part(4, -99);
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_int8_partial_array(&c, 1, 4, part.data()));
      EXPECT_THAT(part, ElementsAre(kI8[1], kI8[2], kI8[3], kI8[4]));
    }

    // uint8 (UINT1)
    {
      coda_cursor c = GotoField(root, "a_uint1");
      std::vector<uint8_t> full(9, 77);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8_array(
                                  &c, full.data(), coda_array_ordering_c));
      EXPECT_THAT(full, ElementsAreArray(kU8));

      std::vector<uint8_t> part(6, 77);
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_uint8_partial_array(&c, 2, 6, part.data()));
      EXPECT_THAT(part,
                  ElementsAre(kU8[2], kU8[3], kU8[4], kU8[5], kU8[6], kU8[7]));
    }

    // int16 (INT2)
    {
      coda_cursor c = GotoField(root, "a_int2");
      std::vector<int16_t> full(9, -999);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(
                                  &c, full.data(), coda_array_ordering_c));
      EXPECT_THAT(full, ElementsAreArray(kI16));

      std::vector<int16_t> part(4, -999);
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_int16_partial_array(&c, 1, 4, part.data()));
      EXPECT_THAT(part, ElementsAre(kI16[1], kI16[2], kI16[3], kI16[4]));
    }

    // uint16 (UINT2)
    {
      coda_cursor c = GotoField(root, "a_uint2");
      std::vector<uint16_t> full(9, 999);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16_array(
                                  &c, full.data(), coda_array_ordering_c));
      EXPECT_THAT(full, ElementsAreArray(kU16));

      std::vector<uint16_t> part(4, 999);
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_uint16_partial_array(&c, 1, 4, part.data()));
      EXPECT_THAT(part, ElementsAre(kU16[1], kU16[2], kU16[3], kU16[4]));
    }

    // int32 (INT4)
    {
      coda_cursor c = GotoField(root, "a_int4");
      std::vector<int32_t> full(9, -999);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                  &c, full.data(), coda_array_ordering_c));
      EXPECT_THAT(full, ElementsAreArray(kI32));

      std::vector<int32_t> part(6, -999);
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_int32_partial_array(&c, 2, 6, part.data()));
      EXPECT_THAT(part, ElementsAre(kI32[2], kI32[3], kI32[4], kI32[5], kI32[6],
                                    kI32[7]));

      // Partial array boundary checks.
      coda_set_error(CODA_SUCCESS, nullptr);
      EXPECT_EQ(-1,
                coda_cursor_read_int32_partial_array(&c, -1, 1, part.data()));
      EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
      coda_set_error(CODA_SUCCESS, nullptr);
      EXPECT_EQ(-1,
                coda_cursor_read_int32_partial_array(&c, 8, 2, part.data()));
      EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
      coda_set_error(CODA_SUCCESS, nullptr);
    }

    // uint32 (UINT4)
    {
      coda_cursor c = GotoField(root, "a_uint4");
      std::vector<uint32_t> full(9, 999U);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32_array(
                                  &c, full.data(), coda_array_ordering_c));
      EXPECT_THAT(full, ElementsAreArray(kU32));

      std::vector<uint32_t> part(4, 999U);
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_uint32_partial_array(&c, 1, 4, part.data()));
      EXPECT_THAT(part, ElementsAre(kU32[1], kU32[2], kU32[3], kU32[4]));
    }

    // int64 (INT8)
    {
      coda_cursor c = GotoField(root, "a_int8");
      std::vector<int64_t> full(9, -999LL);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64_array(
                                  &c, full.data(), coda_array_ordering_c));
      EXPECT_THAT(full, ElementsAreArray(kI64));

      std::vector<int64_t> part(4, -999LL);
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_int64_partial_array(&c, 1, 4, part.data()));
      EXPECT_THAT(part, ElementsAre(kI64[1], kI64[2], kI64[3], kI64[4]));
    }

    // float (REAL4 & FLOAT)
    for (const char* name : {"a_real4", "a_float"}) {
      coda_cursor c = GotoField(root, name);
      std::vector<float> full(9, -999.0f);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(
                                  &c, full.data(), coda_array_ordering_c));
      EXPECT_THAT(full, ElementsAreArray(kF32));

      std::vector<float> part(4, -999.0f);
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_float_partial_array(&c, 1, 4, part.data()));
      EXPECT_THAT(part, ElementsAre(kF32[1], kF32[2], kF32[3], kF32[4]));
    }

    // double (REAL8 & DOUBLE)
    for (const char* name : {"a_real8", "a_double"}) {
      coda_cursor c = GotoField(root, name);
      std::vector<double> full(9, -999.0);
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                  &c, full.data(), coda_array_ordering_c));
      EXPECT_THAT(full, ElementsAreArray(kF64));

      std::vector<double> part(4, -999.0);
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_double_partial_array(&c, 1, 4, part.data()));
      EXPECT_THAT(part, ElementsAre(kF64[1], kF64[2], kF64[3], kF64[4]));
    }

    // char (CHAR & UCHAR with num_elems == 1)
    for (const char* name : {"a_char", "a_uchar"}) {
      coda_cursor c = GotoField(root, name);
      std::vector<char> full(9, '?');
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char_array(
                                  &c, full.data(), coda_array_ordering_c));
      EXPECT_THAT(full, ElementsAreArray(kChExpected));

      std::vector<char> part(4, '?');
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_char_partial_array(&c, 1, 4, part.data()));
      EXPECT_THAT(part, ElementsAre('b', 'c', 'd', 'e'));
    }

    // Array of multi-character strings (num_elems == 4, 3 records).
    {
      coda_cursor c = GotoField(root, "a_str");
      char dummy = '\0';
      coda_set_error(CODA_SUCCESS, nullptr);
      EXPECT_EQ(-1,
                coda_cursor_read_char_array(&c, &dummy, coda_array_ordering_c));
      EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
      coda_set_error(CODA_SUCCESS, nullptr);

      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&c));
      std::array<char, 5> str_buf = {};
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, str_buf.data(), 5));
      EXPECT_STREQ("ab  ", str_buf.data());

      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&c));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, str_buf.data(), 5));
      EXPECT_STREQ("cd  ", str_buf.data());

      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&c));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, str_buf.data(), 5));
      EXPECT_STREQ("ef  ", str_buf.data());
    }
  }
}

TEST_F(CodaCdfCursorTest, FortranVsCArrayOrdering) {
  // 1. Row-major (C ordering) CDF with 2x3 matrix: [[1, 2, 3], [4, 5, 6]].
  {
    CdfBuilder builder;
    builder.SetArrayOrdering(CdfArrayOrdering::kRowMajor)
        .SetEncoding(CdfEncoding::kIbmPc);
    constexpr std::array<int32_t, 6> kRowMajorVals = {1, 2, 3, 4, 5, 6};
    builder.AddZVar(MakeArrayVar<int32_t>("mat", CdfDataType::kInt4, false,
                                          {2, 3}, kRowMajorVals, false));
    const TempFile file("cdf_order_c.cdf");
    ScopedProduct product;
    ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    coda_cursor mat = GotoField(root, "mat");

    int num_dims = -1;
    std::array<long, CODA_MAX_NUM_DIMS> dims = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&mat, &num_dims, dims.data()));
    EXPECT_EQ(2, num_dims);
    EXPECT_EQ(2, dims[0]);
    EXPECT_EQ(3, dims[1]);

    std::vector<int32_t> c_out(6, -1);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &mat, c_out.data(), coda_array_ordering_c));
    EXPECT_THAT(c_out, ElementsAre(1, 2, 3, 4, 5, 6));

    std::vector<int32_t> f_out(6, -1);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_array(&mat, f_out.data(),
                                           coda_array_ordering_fortran));
    EXPECT_THAT(f_out, ElementsAre(1, 4, 2, 5, 3, 6));
  }

  // 2. Column-major (Fortran ordering) CDF: coda-cdf-type.c reverses variable
  // dims [2, 3] -> [3, 2] so CODA C-order reads match disk sequence.
  {
    CdfBuilder builder;
    builder.SetArrayOrdering(CdfArrayOrdering::kColumnMajor)
        .SetEncoding(CdfEncoding::kNetwork);
    constexpr std::array<int32_t, 6> kColMajorVals = {1, 4, 2, 5, 3, 6};
    builder.AddZVar(MakeArrayVar<int32_t>("mat", CdfDataType::kInt4, false,
                                          {2, 3}, kColMajorVals, true));
    const TempFile file("cdf_order_fortran.cdf");
    ScopedProduct product;
    ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    coda_cursor mat = GotoField(root, "mat");

    int num_dims = -1;
    std::array<long, CODA_MAX_NUM_DIMS> dims = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&mat, &num_dims, dims.data()));
    EXPECT_EQ(2, num_dims);
    EXPECT_EQ(3, dims[0]);
    EXPECT_EQ(2, dims[1]);

    std::vector<int32_t> c_out(6, -1);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &mat, c_out.data(), coda_array_ordering_c));
    EXPECT_THAT(c_out, ElementsAre(1, 4, 2, 5, 3, 6));

    std::vector<int32_t> f_out(6, -1);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_array(&mat, f_out.data(),
                                           coda_array_ordering_fortran));
    EXPECT_THAT(f_out, ElementsAre(1, 2, 3, 4, 5, 6));
  }
}

TEST_F(CodaCdfCursorTest, GzipCompressedCvvrVvrAndVxrTrees) {
  CdfBuilder builder;
  builder.SetEncoding(CdfEncoding::kNetwork);
  const bool be = builder.is_big_endian();

  // 1. Single CVVR gzip-compressed 2D int32 array (3 records x 2 elements).
  constexpr std::array<int32_t, 6> kSingleCvvrVals = {11, 22, 33, 44, 55, 66};
  CdfVarSpec single_cvvr = MakeArrayVar<int32_t>(
      "cvvr_single", CdfDataType::kInt4, true, {2}, kSingleCvvrVals, be);
  single_cvvr.has_compression = true;
  single_cvvr.compress_data = true;

  // 2. Linked VXR chain (3 CVVR chunks, max_vxr_entries = 1).
  CdfVarSpec chain_cvvr;
  chain_cvvr.name = "cvvr_vxr_chain";
  chain_cvvr.data_type = CdfDataType::kInt2;
  chain_cvvr.rec_varys = true;
  chain_cvvr.dims = {2};
  chain_cvvr.has_compression = true;
  chain_cvvr.max_vxr_entries = 1;
  chain_cvvr.nested_vxr_tree = false;
  chain_cvvr.chunks = {
      {.first_rec = 0,
       .last_rec = 0,
       .data = CdfBuilder::EncodeValues<int16_t>({100, 101}, be),
       .compressed = true},
      {.first_rec = 1,
       .last_rec = 1,
       .data = CdfBuilder::EncodeValues<int16_t>({200, 201}, be),
       .compressed = true},
      {.first_rec = 2,
       .last_rec = 2,
       .data = CdfBuilder::EncodeValues<int16_t>({300, 301}, be),
       .compressed = true},
  };

  // 3. Multi-level nested VXR tree (5 chunks, max_vxr_entries = 2) mixing
  // compressed CVVR and uncompressed VVR chunks inside a compressed variable.
  CdfVarSpec tree_var;
  tree_var.name = "cvvr_vxr_tree";
  tree_var.data_type = CdfDataType::kDouble;
  tree_var.rec_varys = true;
  tree_var.dims = {2};
  tree_var.has_compression = true;
  tree_var.max_vxr_entries = 2;
  tree_var.nested_vxr_tree = true;
  tree_var.chunks = {
      {.first_rec = 0,
       .last_rec = 0,
       .data = CdfBuilder::EncodeValues<double>({1.5, 2.5}, be),
       .compressed = true},
      {.first_rec = 1,
       .last_rec = 1,
       .data = CdfBuilder::EncodeValues<double>({3.5, 4.5}, be),
       .compressed = false},
      {.first_rec = 2,
       .last_rec = 2,
       .data = CdfBuilder::EncodeValues<double>({5.5, 6.5}, be),
       .compressed = true},
      {.first_rec = 3,
       .last_rec = 3,
       .data = CdfBuilder::EncodeValues<double>({7.5, 8.5}, be),
       .compressed = false},
      {.first_rec = 4,
       .last_rec = 4,
       .data = CdfBuilder::EncodeValues<double>({9.5, 10.5}, be),
       .compressed = true},
  };

  // 4. Compressed string variable (exercises `variable->data != NULL` in
  // read_basic_type + read_string).
  CdfVarSpec comp_str =
      MakeStringVar("cvvr_str", CdfDataType::kChar, 4, "gzip", false);
  comp_str.has_compression = true;
  comp_str.compress_data = true;

  builder.AddZVar(std::move(single_cvvr))
      .AddZVar(std::move(chain_cvvr))
      .AddZVar(std::move(tree_var))
      .AddZVar(std::move(comp_str));

  const TempFile file("cdf_compressed_vxr.cdf");
  ScopedProduct product;
  ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  {
    coda_cursor c = GotoField(root, "cvvr_single");
    std::vector<int32_t> full(6, -1);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &c, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full, ElementsAreArray(kSingleCvvrVals));

    std::vector<int32_t> part(3, -1);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_partial_array(&c, 1, 3, part.data()));
    EXPECT_THAT(part, ElementsAre(22, 33, 44));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 4));
    int32_t scalar = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &scalar));
    EXPECT_EQ(55, scalar);
  }

  {
    coda_cursor c = GotoField(root, "cvvr_vxr_chain");
    std::vector<int16_t> full(6, -1);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(
                                &c, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full, ElementsAre(100, 101, 200, 201, 300, 301));
  }

  {
    coda_cursor c = GotoField(root, "cvvr_vxr_tree");
    std::vector<double> full(10, -1.0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full,
                ElementsAre(1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5));

    std::vector<double> part(5, -1.0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_partial_array(&c, 3, 5, part.data()));
    EXPECT_THAT(part, ElementsAre(4.5, 5.5, 6.5, 7.5, 8.5));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 7));
    double elem = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &elem));
    EXPECT_EQ(8.5, elem);
  }

  {
    coda_cursor c = GotoField(root, "cvvr_str");
    std::array<char, 5> buf = {};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c, buf.data(), 5));
    EXPECT_STREQ("gzip", buf.data());
  }
}

TEST_F(CodaCdfCursorTest, SparseRecordsPaddedAndPrevious) {
  CdfBuilder builder;
  builder.SetEncoding(CdfEncoding::kIbmPc);
  const bool be = builder.is_big_endian();

  // 1. Sparse padded variable with all records present succeeds.
  CdfVarSpec padded_full =
      MakeArrayVar<int32_t>("sparse_padded_full", CdfDataType::kInt4, true, {},
                            std::array<int32_t, 2>{10, 20}, be);
  padded_full.sparse_records = CdfSparseMethod::kPadded;

  // 2. Sparse padded variable missing middle record 1 (3 records x 2 elems).
  CdfVarSpec padded_missing;
  padded_missing.name = "sparse_padded_gap";
  padded_missing.data_type = CdfDataType::kInt4;
  padded_missing.rec_varys = true;
  padded_missing.dims = {2};
  padded_missing.sparse_records = CdfSparseMethod::kPadded;
  padded_missing.chunks = {
      {.first_rec = 0,
       .last_rec = 0,
       .data = CdfBuilder::EncodeValues<int32_t>({100, 101}, be)},
      {.first_rec = 2,
       .last_rec = 2,
       .data = CdfBuilder::EncodeValues<int32_t>({300, 301}, be)},
  };

  // 3. Sparse previous variable missing record 0 (only record 1 present).
  CdfVarSpec prev_missing;
  prev_missing.name = "sparse_prev_gap";
  prev_missing.data_type = CdfDataType::kInt4;
  prev_missing.rec_varys = true;
  prev_missing.sparse_records = CdfSparseMethod::kPrevious;
  prev_missing.chunks = {
      {.first_rec = 1,
       .last_rec = 1,
       .data = CdfBuilder::EncodeValues<int32_t>({777}, be)},
  };

  // 4. Sparse string variable missing record 0 so read_string fails via
  // read_basic_type.
  CdfVarSpec str_missing;
  str_missing.name = "sparse_str_gap";
  str_missing.data_type = CdfDataType::kChar;
  str_missing.num_elems = 4;
  str_missing.rec_varys = true;
  str_missing.sparse_records = CdfSparseMethod::kPadded;
  str_missing.chunks = {
      {.first_rec = 1, .last_rec = 1, .data = {'r', 'e', 'c', '1'}},
  };

  builder.AddZVar(std::move(padded_full))
      .AddZVar(std::move(padded_missing))
      .AddZVar(std::move(prev_missing))
      .AddZVar(std::move(str_missing));

  const TempFile file("cdf_sparse.cdf");
  ScopedProduct product;
  ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  {
    coda_cursor c = GotoField(root, "sparse_padded_full");
    std::vector<int32_t> vals(2, -1);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &c, vals.data(), coda_array_ordering_c));
    EXPECT_THAT(vals, ElementsAre(10, 20));
  }

  {
    coda_cursor c = GotoField(root, "sparse_padded_gap");
    // Present records (0 and 2) and partial read inside record 0 succeed.
    coda_cursor elem = c;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 0));
    int32_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&elem, &v));
    EXPECT_EQ(100, v);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&elem));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 5));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&elem, &v));
    EXPECT_EQ(301, v);

    int32_t part = 0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_partial_array(&c, 0, 1, &part));
    EXPECT_EQ(100, part);

    // Missing record 1 fails across read_basic_type, read_array, and
    // read_partial_array with CODA_ERROR_UNSUPPORTED_PRODUCT.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&elem));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 2));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32(&elem, &v));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);

    std::vector<int32_t> full(6, -1);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_array(&c, full.data(),
                                               coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);

    std::vector<int32_t> gap_part(2, -1);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_cursor_read_int32_partial_array(&c, 1, 2, gap_part.data()));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  {
    coda_cursor c = GotoField(root, "sparse_prev_gap");
    coda_cursor elem = c;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 1));
    int32_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&elem, &v));
    EXPECT_EQ(777, v);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&elem));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 0));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32(&elem, &v));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  {
    coda_cursor c = GotoField(root, "sparse_str_gap");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 0));
    std::array<char, 5> buf = {};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_string(&c, buf.data(), 5));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST_F(CodaCdfCursorTest, EpochTimeTt2000Epoch16AndBypassSpecialTypes) {
  // 1. EPOCH16 variable and attribute are rejected by coda_open().
  {
    CdfBuilder bad_var;
    CdfVarSpec e16;
    e16.name = "epoch16_var";
    e16.data_type = CdfDataType::kEpoch16;
    e16.rec_varys = false;
    e16.data.assign(16, 0U);
    bad_var.AddZVar(std::move(e16));
    const TempFile bad_var_file("cdf_epoch16_var.cdf");
    ASSERT_TRUE(bad_var_file.WriteBytes(bad_var.Build()));
    ScopedProduct prod;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open(bad_var_file.c_str(), prod.OutParam()));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }
  {
    CdfBuilder bad_attr;
    CdfAttrEntrySpec e16_entry;
    e16_entry.entry_num = 0;
    e16_entry.data_type = CdfDataType::kEpoch16;
    e16_entry.num_elems = 1;
    e16_entry.raw_value.assign(16, 0U);
    bad_attr.AddAttr({.name = "bad_epoch16",
                      .scope = CdfAttrScope::kGlobal,
                      .entries = {std::move(e16_entry)}});
    const TempFile bad_attr_file("cdf_epoch16_attr.cdf");
    ASSERT_TRUE(bad_attr_file.WriteBytes(bad_attr.Build()));
    ScopedProduct prod;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open(bad_attr_file.c_str(), prod.OutParam()));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  // 2. EPOCH (31) and TIME_TT2000 (33) scalars and arrays as special doubles
  // and via coda_cursor_use_base_type_of_special_type().
  constexpr double kEpoch0Ms = 63113904000000.0;       // 2000-01-01T00:00:00Z
  constexpr double kEpoch1Ms = 63113904001500.0;       // +1.5s
  constexpr int64_t kTt2000ZeroNs = 43200000000000LL;  // 2000-01-01T00:00:00TT
  constexpr int64_t kTt2000OneHalfNs = 43201500000000LL;  // +1.5s

  CdfBuilder builder;
  builder.SetEncoding(CdfEncoding::kNetwork);
  const bool be = builder.is_big_endian();
  builder
      .AddZVar(MakeScalarVar<double>("epoch_scalar", CdfDataType::kEpoch,
                                     kEpoch1Ms, be))
      .AddZVar(MakeArrayVar<double>("epoch_arr", CdfDataType::kEpoch, true, {},
                                    std::array<double, 2>{kEpoch0Ms, kEpoch1Ms},
                                    be))
      .AddZVar(MakeScalarVar<int64_t>("tt_scalar", CdfDataType::kTimeTt2000,
                                      kTt2000OneHalfNs, be))
      .AddZVar(MakeArrayVar<int64_t>(
          "tt_arr", CdfDataType::kTimeTt2000, true, {},
          std::array<int64_t, 2>{kTt2000ZeroNs, kTt2000OneHalfNs}, be))
      .AddZVar(MakeScalarVar<int32_t>("plain_i32", CdfDataType::kInt4, 7, be));

  const TempFile file("cdf_time_types.cdf");
  ScopedProduct product;
  ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // EPOCH scalar & array.
  {
    coda_cursor c = GotoField(root, "epoch_scalar");
    coda_type_class tc = coda_integer_class;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&c, &tc));
    EXPECT_EQ(coda_special_class, tc);
    coda_special_type st = coda_special_no_data;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&c, &st));
    EXPECT_EQ(coda_special_time, st);

    double seconds = -1.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &seconds));
    EXPECT_DOUBLE_EQ(1.5, seconds);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&c));
    double raw_ms = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &raw_ms));
    EXPECT_DOUBLE_EQ(kEpoch1Ms, raw_ms);
  }
  {
    coda_cursor c = GotoField(root, "epoch_arr");
    std::vector<double> secs(2, -1.0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, secs.data(), coda_array_ordering_c));
    EXPECT_DOUBLE_EQ(0.0, secs[0]);
    EXPECT_DOUBLE_EQ(1.5, secs[1]);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 1));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&c));
    double raw_ms = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &raw_ms));
    EXPECT_DOUBLE_EQ(kEpoch1Ms, raw_ms);
  }

  // TIME_TT2000 scalar & array.
  {
    coda_cursor c = GotoField(root, "tt_scalar");
    coda_special_type st = coda_special_no_data;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&c, &st));
    EXPECT_EQ(coda_special_time, st);

    double seconds = -1.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &seconds));
    EXPECT_DOUBLE_EQ(1.5, seconds);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&c));
    int64_t raw_ns = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &raw_ns));
    EXPECT_EQ(kTt2000OneHalfNs, raw_ns);
  }
  {
    coda_cursor c = GotoField(root, "tt_arr");
    std::vector<double> secs(2, -1.0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, secs.data(), coda_array_ordering_c));
    EXPECT_DOUBLE_EQ(0.0, secs[0]);
    EXPECT_DOUBLE_EQ(1.5, secs[1]);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 0));
    double elem_sec = -1.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &elem_sec));
    EXPECT_DOUBLE_EQ(0.0, elem_sec);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&c));
    int64_t raw_ns = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &raw_ns));
    EXPECT_EQ(kTt2000ZeroNs, raw_ns);
  }

  // Error on calling use_base_type_of_special_type on non-special type, plus
  // direct read_array/read_partial_array on scalar (variable->base_type ==
  // NULL) and attributes check on tag_cdf_time.
  {
    coda_cursor c = GotoField(root, "plain_i32");
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_use_base_type_of_special_type(&c));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);

    int32_t scalar_arr_val = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cdf_cursor_read_int32_array(&c, &scalar_arr_val));
    EXPECT_EQ(7, scalar_arr_val);

    int32_t scalar_part_val = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cdf_cursor_read_int32_partial_array(
                                &c, 0, 0, &scalar_part_val));
    EXPECT_EQ(-1, scalar_part_val);

    coda_cursor tc = GotoField(root, "epoch_scalar");
    int has_attr = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&tc, &has_attr));
    EXPECT_EQ(0, has_attr);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&tc));
    long n_attrs = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&tc, &n_attrs));
    EXPECT_EQ(0, n_attrs);
  }

  // coda_option_bypass_special_types = 1 automatically switches to base type
  // on navigation.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_bypass_special_types(1));
  {
    coda_cursor c = GotoField(root, "tt_scalar");
    coda_type_class tc = coda_special_class;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&c, &tc));
    EXPECT_EQ(coda_integer_class, tc);
    int64_t raw_ns = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &raw_ns));
    EXPECT_EQ(kTt2000OneHalfNs, raw_ns);

    coda_cursor arr = GotoField(root, "tt_arr");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&arr, 1));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&arr, &tc));
    EXPECT_EQ(coda_integer_class, tc);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&arr, &raw_ns));
    EXPECT_EQ(kTt2000OneHalfNs, raw_ns);
  }
}

TEST_F(CodaCdfCursorTest, PerformConversionsOptionZeroAndOne) {
  CdfBuilder builder;
  builder.SetEncoding(CdfEncoding::kIbmPc)
      .AddZVar(MakeScalarVar<int16_t>("s_i16", CdfDataType::kInt2, -321, false))
      .AddZVar(MakeArrayVar<int32_t>("a_i32", CdfDataType::kInt4, true, {},
                                     std::array<int32_t, 3>{10, 20, 30}, false))
      .AddZVar(MakeScalarVar<double>("ep", CdfDataType::kEpoch,
                                     63113904002000.0, false));

  const TempFile file("cdf_conversions.cdf");
  ScopedProduct product;
  ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // Invalid option value returns CODA_ERROR_INVALID_ARGUMENT.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_set_option_perform_conversions(2));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  for (int conv_opt : {0, 1}) {
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(conv_opt));
    EXPECT_EQ(conv_opt, coda_get_option_perform_conversions());

    coda_cursor s = GotoField(root, "s_i16");
    int16_t sv = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&s, &sv));
    EXPECT_EQ(-321, sv);

    coda_cursor a = GotoField(root, "a_i32");
    std::vector<int32_t> av(3, -1);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &a, av.data(), coda_array_ordering_c));
    EXPECT_THAT(av, ElementsAre(10, 20, 30));

    int32_t part = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_partial_array(&a, 1, 1, &part));
    EXPECT_EQ(20, part);

    coda_cursor ep = GotoField(root, "ep");
    double sec = -1.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&ep, &sec));
    EXPECT_DOUBLE_EQ(2.0, sec);
  }
}

TEST_F(CodaCdfCursorTest, GlobalVariableAndEmptyAttributes) {
  // 1. Product with zVariables: variable with attributes, variable without
  // attributes, and global attributes.
  {
    CdfBuilder builder;
    builder.SetEncoding(CdfEncoding::kNetwork);
    const bool be = builder.is_big_endian();

    builder
        .AddZVar(MakeArrayVar<int32_t>("var_with_attrs", CdfDataType::kInt4,
                                       false, {2}, std::array<int32_t, 2>{1, 2},
                                       be))
        .AddZVar(
            MakeScalarVar<int32_t>("var_no_attrs", CdfDataType::kInt4, 99, be))
        .AddAttr({
            .name = "TITLE",
            .scope = CdfAttrScope::kGlobal,
            .entries = {CdfAttrEntrySpec::String(0, "Global Title")},
        })
        .AddAttr({
            .name = "GLOBAL_SCALAR",
            .scope = CdfAttrScope::kGlobal,
            .entries = {CdfAttrEntrySpec::Scalar<int32_t>(0, CdfDataType::kInt4,
                                                          42, be)},
        })
        .AddAttr({
            .name = "GLOBAL_NUMS",
            .scope = CdfAttrScope::kGlobalAssumed,
            .entries = {CdfAttrEntrySpec::Array(
                0, CdfDataType::kInt4, std::array<int32_t, 2>{42, 84}, be)},
        })
        .AddAttr({
            .name = "UNITS",
            .scope = CdfAttrScope::kVariable,
            .entries = {CdfAttrEntrySpec::String(0, "m/s",
                                                 /*is_z_entry=*/true)},
        })
        .AddAttr({
            .name = "SCALE",
            .scope = CdfAttrScope::kVariableAssumed,
            .entries = {CdfAttrEntrySpec::Scalar<double>(
                0, CdfDataType::kDouble, 0.5, be, /*is_z_entry=*/true)},
        });

    const TempFile file("cdf_zvar_attrs.cdf");
    ScopedProduct product;
    {
      // TODO(schwehr): coda-cdf.c:340 read_attribute leaks multi-element
      // non-string array.
      const absl::LeakCheckDisabler leak_disabler;
      ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));
    }

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

    int has_attr = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&root, &has_attr));
    EXPECT_EQ(1, has_attr);

    coda_cursor g_attrs = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&g_attrs));
    coda_cursor title = GotoField(g_attrs, "TITLE");
    std::array<char, 13> title_buf = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&title, title_buf.data(), 13));
    EXPECT_STREQ("Global Title", title_buf.data());

    coda_cursor gscalar = GotoField(g_attrs, "GLOBAL_SCALAR");
    int32_t gscalar_val = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&gscalar, &gscalar_val));
    EXPECT_EQ(42, gscalar_val);

    // Upstream coda-cdf.c:340 omits `*attribute = (coda_dynamic_type *)array;`
    // for multi-element non-string attributes, leaving `*attribute` pointing to
    // the last scalar element (84) instead of an array.
    coda_cursor gnums = GotoField(g_attrs, "GLOBAL_NUMS");
    coda_type_class gnums_tc = coda_array_class;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&gnums, &gnums_tc));
    EXPECT_EQ(coda_integer_class, gnums_tc);
    int32_t gnums_last = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&gnums, &gnums_last));
    EXPECT_EQ(84, gnums_last);
    std::vector<int32_t> gnums_val(2, -1);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_array(&gnums, gnums_val.data(),
                                               coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);

    // Variable with attributes.
    coda_cursor v_with = GotoField(root, "var_with_attrs");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&v_with, &has_attr));
    EXPECT_EQ(1, has_attr);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&v_with));
    long n_attrs = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&v_with, &n_attrs));
    EXPECT_EQ(2, n_attrs);

    coda_cursor units = GotoField(v_with, "UNITS");
    std::array<char, 4> units_buf = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&units, units_buf.data(), 4));
    EXPECT_STREQ("m/s", units_buf.data());

    coda_cursor scale = GotoField(v_with, "SCALE");
    double scale_val = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&scale, &scale_val));
    EXPECT_DOUBLE_EQ(0.5, scale_val);

    // Array element has no attributes (covers tag != tag_cdf_variable in
    // coda_cdf_cursor_goto_attributes).
    coda_cursor elem = GotoField(root, "var_with_attrs");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&elem));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&elem, &has_attr));
    EXPECT_EQ(0, has_attr);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&elem));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&elem, &n_attrs));
    EXPECT_EQ(0, n_attrs);

    // Variable without attributes (covers attributes == NULL branch in
    // coda_cdf_cursor_goto_attributes).
    coda_cursor v_none = GotoField(root, "var_no_attrs");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&v_none, &has_attr));
    EXPECT_EQ(0, has_attr);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&v_none));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&v_none, &n_attrs));
    EXPECT_EQ(0, n_attrs);
  }

  // 2. rVariable with AgrEDR variable attribute.
  {
    CdfBuilder builder;
    builder.SetEncoding(CdfEncoding::kIbmPc)
        .AddRVar(MakeScalarVar<int32_t>("rvar", CdfDataType::kInt4, 5, false))
        .AddAttr({
            .name = "R_NOTE",
            .scope = CdfAttrScope::kVariable,
            .entries = {CdfAttrEntrySpec::String(0, "r-attr",
                                                 /*is_z_entry=*/false)},
        });
    const TempFile file("cdf_rvar_attrs.cdf");
    ScopedProduct product;
    ASSERT_NO_FATAL_FAILURE(OpenBuiltCdf(file, builder, product));

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
    coda_cursor rv = GotoField(root, "rvar");
    int has_attr = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&rv, &has_attr));
    EXPECT_EQ(1, has_attr);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&rv));
    coda_cursor note = GotoField(rv, "R_NOTE");
    std::array<char, 7> buf = {};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&note, buf.data(), 7));
    EXPECT_STREQ("r-attr", buf.data());
  }
}

TEST_F(CodaCdfCursorTest, TruncatedVvrReadBytesErrorPaths) {
  // Build a CDF whose last record is an uncompressed VVR, then truncate its
  // payload by 8 bytes and update GDR eof to match file_size. coda_open()
  // succeeds, while read_array, read_partial_array, and read_basic_type fail
  // inside read_bytes() with CODA_ERROR_OUT_OF_BOUNDS_READ.
  auto make_truncated_bytes = [](CdfVarSpec var) {
    CdfBuilder builder;
    builder.SetEncoding(CdfEncoding::kNetwork).AddZVar(std::move(var));
    CdfBuildResult res = builder.BuildWithOffsets();
    res.bytes.resize(res.bytes.size() - 8);
    CdfBuilder::PatchBe64(res.bytes, static_cast<size_t>(res.gdr_offset) + 36,
                          static_cast<int64_t>(res.bytes.size()));
    return res.bytes;
  };

  // Test both mmap enabled (1) and disabled (0, pread path).
  for (int use_mmap : {1, 0}) {
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    // 1. Numeric VVR truncation.
    const std::vector<uint8_t> num_bytes = make_truncated_bytes(
        MakeArrayVar<int32_t>("trunc_i32", CdfDataType::kInt4, false, {4},
                              std::array<int32_t, 4>{1, 2, 3, 4}, true));
    const TempFile num_file(use_mmap ? "cdf_trunc_num_mmap.cdf"
                                     : "cdf_trunc_num_fd.cdf");
    ScopedProduct num_prod;
    ASSERT_NO_FATAL_FAILURE(OpenCdfBytes(num_file, num_bytes, num_prod));

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, num_prod.get()));
    coda_cursor arr = GotoField(root, "trunc_i32");

    std::vector<int32_t> dst(4, -1);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_array(&arr, dst.data(),
                                               coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

    int32_t part = -1;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_partial_array(&arr, 2, 1, &part));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&arr, 3));
    int32_t elem = -1;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32(&arr, &elem));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);

    // 2. String VVR truncation (triggers read_basic_type failure inside
    // coda_cdf_cursor_read_string).
    const std::vector<uint8_t> str_bytes = make_truncated_bytes(
        MakeStringVar("trunc_str", CdfDataType::kChar, 12, "0123456789ab"));
    const TempFile str_file(use_mmap ? "cdf_trunc_str_mmap.cdf"
                                     : "cdf_trunc_str_fd.cdf");
    ScopedProduct str_prod;
    ASSERT_NO_FATAL_FAILURE(OpenCdfBytes(str_file, str_bytes, str_prod));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, str_prod.get()));
    coda_cursor str_c = GotoField(root, "trunc_str");
    std::array<char, 13> str_buf = {};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_string(&str_c, str_buf.data(), 13));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

// TODO(schwehr): Remaining uncovered lines in libcoda/coda-cdf-cursor.c are
// WORDS_BIGENDIAN branches, unreachable `assert(0)` defaults, and the
// defensive `offset > num_records * num_values_per_record * value_size` check
// in `read_basic_type()`.

}  // namespace
}  // namespace stcorp_coda
