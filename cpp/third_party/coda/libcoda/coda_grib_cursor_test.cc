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
// Tests CODA GRIB1 and GRIB2 cursor navigation, metadata queries, scalar
// reads, array reads, partial array reads, simple packing (0-bit constant
// fields, multi-width bit-packing, binary/decimal scale factors, reference
// values, and bitmasks with NaN missing values), direct float arrays
// (coordinateValues), attributes, boundary checks, C/Fortran ordering, and
// out-of-bounds / truncated-read error paths (libcoda/coda-grib-cursor.c).
//
// google3 build caveats:
//   - WORDS_BIGENDIAN is undefined; byte-swapping paths in coda-grib-cursor.c
//     are active on little-endian hosts.
//   - Upstream coda-grib-cursor.c returns referenceValue directly when
//     element_bit_size == 0 (before applying decimalScaleFactor or bitmask).
//   - Upstream coda-grib.c mislocates GRIB2 product template 40
//     coordinateValues at the wrong offset (reads 25 bytes instead of 27), so
//     direct float coordinateValues tests use template 0.
//   - Upstream coda-grib.c:1540 (`sign << 31` in ibmfloat_to_iee754) triggers
//     UBSan signed shift overflow on negative IBM floats, so GRIB1 file-backed
//     messages use non-negative IBM float reference values.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib-cursor.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-grib.h"
}  // extern "C"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_coda/google/util/grib_builder.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;

constexpr float kSentinelFloat = -999.0f;
constexpr double kSentinelDouble = -999.0;

class CodaGribCursorTest : public ::testing::Test {
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
  ScopedCodaOptions options_;
  ScopedCodaSession session_;
};

::testing::AssertionResult WriteAndOpenGrib(const TempFile& file,
                                            std::span<const uint8_t> bytes,
                                            ScopedProduct& product) {
  if (!file.WriteBytes(bytes)) {
    return ::testing::AssertionFailure()
           << "Failed to write GRIB bytes to " << file.string();
  }
  coda_set_error(CODA_SUCCESS, nullptr);
  if (coda_open(file.c_str(), product.OutParam()) != CODA_SUCCESS) {
    return ::testing::AssertionFailure()
           << "coda_open failed for " << file.string() << ": "
           << coda_errno_to_string(coda_errno);
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult GotoPath(coda_product* product, const char* path,
                                    coda_cursor* cursor) {
  if (coda_cursor_set_product(cursor, product) != CODA_SUCCESS) {
    return ::testing::AssertionFailure() << "coda_cursor_set_product failed: "
                                         << coda_errno_to_string(coda_errno);
  }
  if (coda_cursor_goto(cursor, path) != CODA_SUCCESS) {
    return ::testing::AssertionFailure()
           << "coda_cursor_goto('" << path
           << "') failed: " << coda_errno_to_string(coda_errno);
  }
  return ::testing::AssertionSuccess();
}

TEST_F(CodaGribCursorTest, OpenMissingAndTruncatedGribFilesFail) {
  const TempFile missing("grib_cursor_missing.grb");
  ScopedProduct product;

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);

  // Truncated GRIB2 stream: valid 16-byte indicator followed by incomplete
  // Section 1 causes coda_open to fail with CODA_ERROR_OUT_OF_BOUNDS_READ.
  std::vector<uint8_t> grib2 = GribBuilder::BuildGrib2(Grib2MessageSpec{});
  ASSERT_GT(grib2.size(), 24u);
  grib2.resize(24);
  const TempFile truncated("grib_cursor_truncated.grb2");
  ASSERT_TRUE(truncated.WriteBytes(grib2));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(truncated.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
}

TEST_F(CodaGribCursorTest, Grib1SetProductMetadataAttributesAndNavigation) {
  const std::vector<uint8_t> bytes = GribBuilder::BuildGrib1(Grib1MessageSpec{
      .gds = Grib1GdsSpec{.ni = 2, .nj = 3},
      .bds =
          Grib1BdsSpec{
              .bits_per_value = 8,
              .packed_ints = {10, 20, 30, 40, 50, 60},
          },
  });

  const TempFile file("grib1_cursor_nav.grb");
  ScopedProduct product;
  ASSERT_TRUE(WriteAndOpenGrib(file, bytes, product));

  // Verify direct coda_grib_cursor_set_product initializes root cursor state.
  coda_cursor root{};
  ASSERT_EQ(CODA_SUCCESS, coda_grib_cursor_set_product(&root, product.get()));
  int depth = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&root, &depth));
  EXPECT_EQ(0, depth);
  coda_format format = coda_format_binary;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&root, &format));
  EXPECT_EQ(coda_format_grib, format);
  long num_messages = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&root, &num_messages));
  EXPECT_EQ(1, num_messages);

  // Navigate to the GRIB1 values array (backend = coda_backend_grib).
  coda_cursor values{};
  ASSERT_TRUE(GotoPath(product.get(), "/[0]/grib1/data/values", &values));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&values, &format));
  EXPECT_EQ(coda_format_grib, format);

  coda_type_class type_class = coda_record_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&values, &type_class));
  EXPECT_EQ(coda_array_class, type_class);

  long num_elements = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&values, &num_elements));
  EXPECT_EQ(6, num_elements);

  int num_dims = -1;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&values, &num_dims, dims.data()));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(6, dims[0]);

  int64_t bit_size = 0;
  int64_t byte_size = 0;
  int64_t file_bit_offset = 0;
  int64_t file_byte_offset = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&values, &bit_size));
  EXPECT_EQ(-1, bit_size);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&values, &byte_size));
  EXPECT_EQ(-1, byte_size);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_file_bit_offset(&values, &file_bit_offset));
  EXPECT_EQ(-1, file_bit_offset);
  EXPECT_EQ(-1, coda_cursor_get_file_byte_offset(&values, &file_byte_offset));
  EXPECT_EQ(-1, file_byte_offset);

  int has_ascii = -1;
  int has_attrs = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&values, &has_ascii));
  EXPECT_EQ(0, has_ascii);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&values, &has_attrs));
  EXPECT_EQ(0, has_attrs);

  // Attributes on a GRIB array yield a virtual empty record.
  coda_cursor attrs = values;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attrs));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&attrs, &type_class));
  EXPECT_EQ(coda_record_class, type_class);
  long attr_count = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&attrs, &attr_count));
  EXPECT_EQ(0, attr_count);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&attrs));

  // Iterate all elements via goto_first_array_element /
  // goto_next_array_element.
  coda_cursor elem = values;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&elem));
  for (long i = 0; i < 6; ++i) {
    long idx = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&elem, &idx));
    EXPECT_EQ(i, idx);

    long elem_count = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&elem, &elem_count));
    EXPECT_EQ(1, elem_count);

    coda_type_class elem_class = coda_array_class;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&elem, &elem_class));
    EXPECT_EQ(coda_real_class, elem_class);

    coda_native_type read_type = coda_native_type_int32;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&elem, &read_type));
    EXPECT_EQ(coda_native_type_float, read_type);

    float fval = kSentinelFloat;
    double dval = kSentinelDouble;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&elem, &fval));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&elem, &dval));
    const float expected = static_cast<float>((i + 1) * 10);
    EXPECT_FLOAT_EQ(expected, fval);
    EXPECT_DOUBLE_EQ(static_cast<double>(expected), dval);

    if (i < 5) {
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
    }
  }

  // Stepping past the last element fails and preserves the cursor position.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&elem));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  long last_idx = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&elem, &last_idx));
  EXPECT_EQ(5, last_idx);
  float last_val = kSentinelFloat;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&elem, &last_val));
  EXPECT_FLOAT_EQ(60.0f, last_val);

  // Attributes on a scalar GRIB array element also yield an empty record.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&elem));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&elem, &type_class));
  EXPECT_EQ(coda_record_class, type_class);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&elem, &attr_count));
  EXPECT_EQ(0, attr_count);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&elem));

  // Direct subscript navigation via coda_cursor_goto_array_element.
  coda_cursor sub = values;
  const long sub_idx = 3;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&sub, 1, &sub_idx));
  float sub_val = kSentinelFloat;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&sub, &sub_val));
  EXPECT_FLOAT_EQ(40.0f, sub_val);
}

TEST_F(CodaGribCursorTest, SimplePackingBitWidthsScaleFactorsAndOrderings) {
  GribBuilder builder;

  // Message 0 (GRIB1): bits_per_value = 0 (constant field).
  // Note: coda-grib-cursor.c returns referenceValue directly when
  // element_bit_size == 0 (decimalScaleFactor is not applied).
  builder.AddGrib1(Grib1MessageSpec{
      .decimal_scale_factor = 1,
      .gds = Grib1GdsSpec{.ni = 2, .nj = 2},
      .bds = Grib1BdsSpec{.reference_value = 42.5f, .bits_per_value = 0},
  });

  // Message 1 (GRIB1): 1-bit packing (5 elements spanning 1 byte), E = 1, R =
  // 2 (non-negative to avoid signed shift UB at coda-grib.c:1540).
  builder.AddGrib1(Grib1MessageSpec{
      .gds = Grib1GdsSpec{.ni = 5, .nj = 1},
      .bds =
          Grib1BdsSpec{
              .binary_scale_factor = 1,
              .reference_value = 2.0f,
              .bits_per_value = 1,
              .packed_ints = {0, 1, 1, 0, 1},
          },
  });

  // Message 2 (GRIB1): 4-bit packing with positive D=1 and negative E=-1.
  // scalefactor = 2^-1 * 10^-1 = 0.05, offset = 100.0 * 10^-1 = 10.0.
  builder.AddGrib1(Grib1MessageSpec{
      .decimal_scale_factor = 1,
      .gds = Grib1GdsSpec{.ni = 2, .nj = 2},
      .bds =
          Grib1BdsSpec{
              .binary_scale_factor = -1,
              .reference_value = 100.0f,
              .bits_per_value = 4,
              .packed_ints = {0, 2, 4, 15},
          },
  });

  // Message 3 (GRIB2): 12-bit unaligned packing with D=-1, E=2, R=1.5.
  // scalefactor = 2^2 * 10^1 = 40.0, offset = 1.5 * 10 = 15.0.
  builder.AddGrib2(Grib2MessageSpec{
      .grid = Grib2GridSpec{.number_of_data_points = 4, .ni = 2, .nj = 2},
      .data_rep =
          Grib2DataRepSpec{
              .number_of_data_points = 4,
              .reference_value = 1.5f,
              .binary_scale_factor = 2,
              .decimal_scale_factor = -1,
              .bits_per_value = 12,
          },
      .data = Grib2DataSpec{.packed_ints = {0, 1, 100, 4095}},
  });

  // Message 4 (GRIB2): 24-bit packing (byte-aligned multi-byte reads).
  builder.AddGrib2(Grib2MessageSpec{
      .grid = Grib2GridSpec{.number_of_data_points = 3, .ni = 3, .nj = 1},
      .data_rep =
          Grib2DataRepSpec{
              .number_of_data_points = 3,
              .reference_value = 0.0f,
              .binary_scale_factor = 0,
              .decimal_scale_factor = 0,
              .bits_per_value = 24,
          },
      .data = Grib2DataSpec{.packed_ints = {0, 0x012345, 0x100000}},
  });

  const std::vector<uint8_t> bytes = builder.Build();
  const TempFile file("grib_simple_packing.grb");
  ScopedProduct product;
  ASSERT_TRUE(WriteAndOpenGrib(file, bytes, product));

  struct ExpectedCase {
    const char* path;
    std::vector<float> expected;
  };
  const std::vector<ExpectedCase> cases = {
      {.path = "/[0]/grib1/data/values",
       .expected = {42.5f, 42.5f, 42.5f, 42.5f}},
      {.path = "/[1]/grib1/data/values",
       .expected = {2.0f, 4.0f, 4.0f, 2.0f, 4.0f}},
      {.path = "/[2]/grib1/data/values",
       .expected = {10.0f, 10.1f, 10.2f, 10.75f}},
      {.path = "/[3]/grib2/data[0]/values",
       .expected = {15.0f, 55.0f, 4015.0f, 163815.0f}},
      {.path = "/[4]/grib2/data[0]/values",
       .expected = {0.0f, 74565.0f, 1048576.0f}},
  };

  for (const ExpectedCase& tc : cases) {
    coda_cursor cursor{};
    ASSERT_TRUE(GotoPath(product.get(), tc.path, &cursor));

    const size_t n = tc.expected.size();
    std::vector<float> floats_c(n, kSentinelFloat);
    std::vector<float> floats_f(n, kSentinelFloat);
    std::vector<double> doubles_c(n, kSentinelDouble);
    std::vector<double> doubles_f(n, kSentinelDouble);

    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_float_array(&cursor, floats_c.data(),
                                           coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_float_array(&cursor, floats_f.data(),
                                           coda_array_ordering_fortran));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_array(&cursor, doubles_c.data(),
                                            coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_array(&cursor, doubles_f.data(),
                                            coda_array_ordering_fortran));

    for (size_t i = 0; i < n; ++i) {
      EXPECT_FLOAT_EQ(tc.expected[i], floats_c[i])
          << tc.path << "[" << i << "]";
      EXPECT_FLOAT_EQ(tc.expected[i], floats_f[i])
          << tc.path << "[" << i << "]";
      EXPECT_DOUBLE_EQ(static_cast<double>(tc.expected[i]), doubles_c[i])
          << tc.path << "[" << i << "]";
      EXPECT_DOUBLE_EQ(static_cast<double>(tc.expected[i]), doubles_f[i])
          << tc.path << "[" << i << "]";
    }

    // Partial array reads: middle slice and zero-length slice.
    std::vector<float> partial_f(2, kSentinelFloat);
    std::vector<double> partial_d(2, kSentinelDouble);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_partial_array(
                                &cursor, 1, 2, partial_f.data()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_partial_array(
                                &cursor, 1, 2, partial_d.data()));
    EXPECT_FLOAT_EQ(tc.expected[1], partial_f[0]);
    EXPECT_FLOAT_EQ(tc.expected[2], partial_f[1]);
    EXPECT_DOUBLE_EQ(static_cast<double>(tc.expected[1]), partial_d[0]);
    EXPECT_DOUBLE_EQ(static_cast<double>(tc.expected[2]), partial_d[1]);

    float zero_len_sentinel = kSentinelFloat;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_partial_array(
                                &cursor, 0, 0, &zero_len_sentinel));
    EXPECT_FLOAT_EQ(kSentinelFloat, zero_len_sentinel);
  }
}

TEST_F(CodaGribCursorTest, BitmaskMissingPixelsAndCumsum128Blocks) {
  // Use 144 elements (18 bitmap bytes) so bitmask indexing exercises:
  //   - bm_index == 0 (first 128-bit block, first byte)
  //   - 1 <= bm_index <= 15 (bm_index % 16 != 0 inside first 128-bit block)
  //   - bm_index == 16 (second 128-bit block, bm_index % 16 == 0)
  //   - bm_index == 17 (second 128-bit block, bm_index % 16 != 0)
  //   - both bitmask == 0 (NaN) and bitmask == 1 (packed value)
  constexpr size_t kNumGridPoints = 144;
  std::vector<bool> mask(kNumGridPoints, false);
  std::vector<uint64_t> packed;
  std::vector<float> expected(kNumGridPoints, 0.0f);
  for (size_t i = 0; i < kNumGridPoints; ++i) {
    const bool present = (i % 3 != 1);
    mask[i] = present;
    if (present) {
      const uint64_t code = packed.size();
      packed.push_back(code);
      expected[i] = 1.0f + static_cast<float>(code);
    }
  }

  GribBuilder builder;

  // Message 0: GRIB1 with 144-element bitmap and 8-bit simple packing.
  builder.AddGrib1(Grib1MessageSpec{
      .gds = Grib1GdsSpec{.ni = 12, .nj = 12},
      .bms = Grib1BmsSpec{.bitmap = mask},
      .bds =
          Grib1BdsSpec{
              .reference_value = 1.0f,
              .bits_per_value = 8,
              .packed_ints = packed,
          },
  });

  // Message 1: GRIB2 multi-field message:
  //   - field 0 defines inline bitmap (indicator = 0)
  //   - field 1 reuses previous bitmap (indicator = 254)
  //   - field 2 has bits_per_value = 0 with inline bitmap (element_bit_size ==
  //   0
  //     branch returns reference_value before consulting bitmask).
  builder.AddGrib2(Grib2MessageSpec{
      .fields =
          {
              Grib2FieldSpec{
                  .grid =
                      Grib2GridSpec{
                          .number_of_data_points = 144, .ni = 12, .nj = 12},
                  .data_rep =
                      Grib2DataRepSpec{
                          .number_of_data_points = 144,
                          .reference_value = 1.0f,
                          .bits_per_value = 8,
                      },
                  .bitmap = Grib2BitmapSpec{.bitmap = mask},
                  .data = Grib2DataSpec{.packed_ints = packed},
              },
              Grib2FieldSpec{
                  .grid = std::nullopt,
                  .data_rep =
                      Grib2DataRepSpec{
                          .number_of_data_points = 144,
                          .reference_value = 1.0f,
                          .bits_per_value = 8,
                      },
                  .bitmap = Grib2BitmapSpec{.indicator_override = 254},
                  .data = Grib2DataSpec{.packed_ints = packed},
              },
              Grib2FieldSpec{
                  .grid = std::nullopt,
                  .data_rep =
                      Grib2DataRepSpec{
                          .number_of_data_points = 144,
                          .reference_value = 7.5f,
                          .bits_per_value = 0,
                      },
                  .bitmap = Grib2BitmapSpec{.bitmap = mask},
                  .data = Grib2DataSpec{},
              },
          },
  });

  const std::vector<uint8_t> bytes = builder.Build();
  const TempFile file("grib_bitmask.grb");
  ScopedProduct product;
  ASSERT_TRUE(WriteAndOpenGrib(file, bytes, product));

  for (const char* path :
       {"/[0]/grib1/data/values", "/[1]/grib2/data[0]/values",
        "/[1]/grib2/data[1]/values"}) {
    coda_cursor cursor{};
    ASSERT_TRUE(GotoPath(product.get(), path, &cursor));

    std::vector<float> floats(kNumGridPoints, kSentinelFloat);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(
                                &cursor, floats.data(), coda_array_ordering_c));

    std::vector<double> doubles(kNumGridPoints, kSentinelDouble);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_array(&cursor, doubles.data(),
                                            coda_array_ordering_c));

    for (size_t i = 0; i < kNumGridPoints; ++i) {
      if (mask[i]) {
        EXPECT_FLOAT_EQ(expected[i], floats[i]) << path << "[" << i << "]";
        EXPECT_DOUBLE_EQ(static_cast<double>(expected[i]), doubles[i])
            << path << "[" << i << "]";
      } else {
        EXPECT_TRUE(std::isnan(floats[i])) << path << "[" << i << "]";
        EXPECT_TRUE(std::isnan(doubles[i])) << path << "[" << i << "]";
      }
    }

    // Partial read across the 128-bit cumsum boundary (indices 126..139).
    std::array<float, 14> slice{};
    slice.fill(kSentinelFloat);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_partial_array(
                                &cursor, 126, 14, slice.data()));
    for (size_t j = 0; j < slice.size(); ++j) {
      const size_t idx = 126 + j;
      if (mask[idx]) {
        EXPECT_FLOAT_EQ(expected[idx], slice[j]) << path << "[" << idx << "]";
      } else {
        EXPECT_TRUE(std::isnan(slice[j])) << path << "[" << idx << "]";
      }
    }

    // Scalar reads at key bitmask byte boundaries.
    for (long idx : {0L, 1L, 7L, 8L, 127L, 128L, 135L, 136L, 143L}) {
      coda_cursor elem = cursor;
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_goto_array_element_by_index(&elem, idx));
      float v = kSentinelFloat;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&elem, &v));
      if (mask[static_cast<size_t>(idx)]) {
        EXPECT_FLOAT_EQ(expected[static_cast<size_t>(idx)], v);
      } else {
        EXPECT_TRUE(std::isnan(v));
      }
    }
  }

  // Verify 0-bit field with bitmap returns reference_value for all elements.
  coda_cursor const_cursor{};
  ASSERT_TRUE(
      GotoPath(product.get(), "/[1]/grib2/data[2]/values", &const_cursor));
  std::vector<float> const_vals(kNumGridPoints, kSentinelFloat);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_float_array(&const_cursor, const_vals.data(),
                                         coda_array_ordering_c));
  for (size_t i = 0; i < kNumGridPoints; ++i) {
    EXPECT_FLOAT_EQ(7.5f, const_vals[i]) << "index " << i;
  }
}

TEST_F(CodaGribCursorTest, Grib2CoordinateValuesDirectFloatArray) {
  constexpr std::array<float, 5> kCoords = {-12.5f, 0.0f, 3.25f, 1024.75f,
                                            0.001f};
  const std::vector<uint8_t> bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
      .product =
          Grib2ProductSpec{
              .template_number = 0,
              .coordinate_values = {kCoords.begin(), kCoords.end()},
          },
      .data_rep =
          Grib2DataRepSpec{
              .number_of_data_points = 4,
              .bits_per_value = 8,
          },
  });

  const TempFile file("grib2_coord_values.grb2");
  ScopedProduct product;
  ASSERT_TRUE(WriteAndOpenGrib(file, bytes, product));

  coda_cursor coords{};
  ASSERT_TRUE(
      GotoPath(product.get(), "/[0]/grib2/data[0]/coordinateValues", &coords));

  long num_elements = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&coords, &num_elements));
  EXPECT_EQ(5, num_elements);

  int num_dims = -1;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&coords, &num_dims, dims.data()));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(5, dims[0]);

  // Read full array with both C and Fortran ordering as float and double.
  std::array<float, 5> actual_fc{};
  std::array<float, 5> actual_ff{};
  std::array<double, 5> actual_dc{};
  actual_fc.fill(kSentinelFloat);
  actual_ff.fill(kSentinelFloat);
  actual_dc.fill(kSentinelDouble);

  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_float_array(&coords, actual_fc.data(),
                                         coda_array_ordering_c));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_float_array(&coords, actual_ff.data(),
                                         coda_array_ordering_fortran));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_double_array(&coords, actual_dc.data(),
                                          coda_array_ordering_c));

  for (size_t i = 0; i < kCoords.size(); ++i) {
    EXPECT_FLOAT_EQ(kCoords[i], actual_fc[i]);
    EXPECT_FLOAT_EQ(kCoords[i], actual_ff[i]);
    EXPECT_DOUBLE_EQ(static_cast<double>(kCoords[i]), actual_dc[i]);
  }

  // Partial array reads on direct IEEE754 float32 coordinateValues.
  std::array<float, 3> partial_f{};
  std::array<double, 3> partial_d{};
  partial_f.fill(kSentinelFloat);
  partial_d.fill(kSentinelDouble);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_partial_array(
                              &coords, 1, 3, partial_f.data()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_partial_array(
                              &coords, 1, 3, partial_d.data()));
  EXPECT_THAT(partial_f, ElementsAre(kCoords[1], kCoords[2], kCoords[3]));
  EXPECT_THAT(partial_d, ElementsAre(static_cast<double>(kCoords[1]),
                                     static_cast<double>(kCoords[2]),
                                     static_cast<double>(kCoords[3])));

  // Scalar iteration across coordinateValues.
  coda_cursor elem = coords;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&elem));
  for (size_t i = 0; i < kCoords.size(); ++i) {
    float v = kSentinelFloat;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&elem, &v));
    EXPECT_FLOAT_EQ(kCoords[i], v);
    if (i + 1 < kCoords.size()) {
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
    }
  }
}

TEST_F(CodaGribCursorTest, ZeroElementArraysAndBoundaryChecksOption) {
  GribBuilder builder;
  // Message 0: GRIB1 with 0 elements (Ni = 0, Nj = 2).
  builder.AddGrib1(Grib1MessageSpec{
      .gds = Grib1GdsSpec{.ni = 0, .nj = 2},
      .bds = Grib1BdsSpec{.bits_per_value = 8},
  });
  // Message 1: GRIB2 with 0 elements and 4-element message 2 for OOB index
  // test.
  builder.AddGrib2(Grib2MessageSpec{
      .grid = Grib2GridSpec{.number_of_data_points = 0, .ni = 0, .nj = 2},
      .data_rep =
          Grib2DataRepSpec{.number_of_data_points = 0, .bits_per_value = 8},
  });
  builder.AddGrib2(Grib2MessageSpec{
      .grid = Grib2GridSpec{.number_of_data_points = 4, .ni = 2, .nj = 2},
      .data_rep =
          Grib2DataRepSpec{.number_of_data_points = 4, .bits_per_value = 8},
      .data = Grib2DataSpec{.packed_ints = {1, 2, 3, 4}},
  });

  const std::vector<uint8_t> bytes = builder.Build();
  const TempFile file("grib_zero_elements.grb");
  ScopedProduct product;
  ASSERT_TRUE(WriteAndOpenGrib(file, bytes, product));

  for (const char* empty_path :
       {"/[0]/grib1/data/values", "/[1]/grib2/data[0]/values"}) {
    coda_cursor empty_arr{};
    ASSERT_TRUE(GotoPath(product.get(), empty_path, &empty_arr));

    long num_elements = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_num_elements(&empty_arr, &num_elements));
    EXPECT_EQ(0, num_elements);

    // Full array reads succeed on 0-element arrays without writing dst.
    float f_sentinel = kSentinelFloat;
    double d_sentinel = kSentinelDouble;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_float_array(&empty_arr, &f_sentinel,
                                           coda_array_ordering_c));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_array(&empty_arr, &d_sentinel,
                                            coda_array_ordering_c));
    EXPECT_FLOAT_EQ(kSentinelFloat, f_sentinel);
    EXPECT_DOUBLE_EQ(kSentinelDouble, d_sentinel);

    // With boundary checks enabled, element 0 and partial(0, 0) fail OOB.
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));
    coda_set_error(CODA_SUCCESS, nullptr);
    coda_cursor elem = empty_arr;
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&elem, 0));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_float_partial_array(&empty_arr, 0, 0,
                                                       &f_sentinel));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    // With boundary checks disabled, partial(0, 0) dispatches to
    // coda_grib_cursor_read_float_partial_array with num_elements == 0.
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_partial_array(
                                &empty_arr, 0, 0, &f_sentinel));
    EXPECT_FLOAT_EQ(kSentinelFloat, f_sentinel);
  }

  // Verify goto_array_element_by_index skips bounds checks when
  // coda_option_perform_boundary_checks == 0.
  coda_cursor arr4{};
  ASSERT_TRUE(GotoPath(product.get(), "/[2]/grib2/data[0]/values", &arr4));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));

  for (long oob_idx : {-1L, 4L}) {
    coda_cursor elem = arr4;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&elem, oob_idx));
    long actual_idx = -99;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&elem, &actual_idx));
    EXPECT_EQ(oob_idx, actual_idx);
  }
}

TEST_F(CodaGribCursorTest, OutOfBoundsAndInvalidNavigationErrors) {
  const std::vector<uint8_t> bytes = GribBuilder::BuildGrib2(Grib2MessageSpec{
      .grid = Grib2GridSpec{.number_of_data_points = 4, .ni = 2, .nj = 2},
      .product = Grib2ProductSpec{.coordinate_values = {1.0f, 2.0f, 3.0f}},
      .data_rep =
          Grib2DataRepSpec{.number_of_data_points = 4, .bits_per_value = 8},
      .data = Grib2DataSpec{.packed_ints = {5, 6, 7, 8}},
  });

  const TempFile file("grib_oob_errors.grb2");
  ScopedProduct product;
  ASSERT_TRUE(WriteAndOpenGrib(file, bytes, product));

  coda_cursor values{};
  ASSERT_TRUE(GotoPath(product.get(), "/[0]/grib2/data[0]/values", &values));

  // Rank mismatch in coda_grib_cursor_goto_array_element (only rank 1 is
  // valid).
  coda_cursor probe = values;
  const std::array<long, 2> subs2 = {0, 0};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&probe, 0, subs2.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_NUM_DIMS_MISMATCH, coda_errno);

  probe = values;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&probe, 2, subs2.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_NUM_DIMS_MISMATCH, coda_errno);

  // Out-of-bounds indices via goto_array_element_by_index and
  // goto_array_element.
  for (long bad_idx : {-1L, 4L}) {
    probe = values;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&probe, bad_idx));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    probe = values;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&probe, 1, &bad_idx));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  }

  // Out-of-bounds partial array reads.
  std::array<float, 4> fbuf{};
  std::array<double, 4> dbuf{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_read_float_partial_array(&values, -1, 1, fbuf.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_read_float_partial_array(&values, 4, 1, fbuf.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_read_double_partial_array(&values, 2, 3, dbuf.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  // Invalid operations on a scalar element and attributes record.
  coda_cursor elem = values;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&elem));

  int32_t i32 = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_int32(&elem, &i32));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  std::array<char, 16> str{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_string(&elem, str.data(),
                                        static_cast<long>(str.size())));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  const long zero = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&elem, 1, &zero));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_cursor attrs = values;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attrs));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&attrs));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
}

TEST_F(CodaGribCursorTest, TruncatedPayloadsFailInReadFloatArrayAndPartial) {
  // 1. Simple packing read_bits() past EOF:
  //    GRIB1 header declares 2x10 = 20 16-bit values (40 bytes needed), while
  //    BDS payload contains only 2 bytes followed by '7777' (6 bytes to EOF).
  const std::vector<uint8_t> grib1_short_bds =
      GribBuilder::BuildGrib1(Grib1MessageSpec{
          .gds = Grib1GdsSpec{.ni = 2, .nj = 10},
          .bds =
              Grib1BdsSpec{
                  .bits_per_value = 16,
                  .raw_packed_bytes = {0x00, 0x2A},
              },
      });
  const TempFile file1("grib1_short_bds.grb");
  ScopedProduct prod1;
  ASSERT_TRUE(WriteAndOpenGrib(file1, grib1_short_bds, prod1));

  coda_cursor g1_values{};
  ASSERT_TRUE(GotoPath(prod1.get(), "/[0]/grib1/data/values", &g1_values));

  // Element 0 is within the 2-byte payload and succeeds.
  coda_cursor g1_elem = g1_values;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&g1_elem, 0));
  float first_val = kSentinelFloat;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&g1_elem, &first_val));
  EXPECT_FLOAT_EQ(42.0f, first_val);

  // Element 19 lies past EOF and triggers read_bits() failure in read_float.
  g1_elem = g1_values;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&g1_elem, 19));
  float oob_val = kSentinelFloat;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_float(&g1_elem, &oob_val));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  std::array<float, 20> g1_arr{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_float_array(&g1_values, g1_arr.data(),
                                             coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  std::array<float, 2> g1_part{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_float_partial_array(&g1_values, 18, 2,
                                                     g1_part.data()));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  // 2. Direct float read_bytes() past EOF on GRIB2 coordinateValues:
  //    In a default single-field GRIB2 message (Sec0=16, Sec1=21, Sec3=72),
  //    Section 4 starts at byte 109 (109..112=len, 113=sec_num 4, 114..115=NV).
  //    Patching NV=100 (400 bytes) leaves section_size=34 and <40 bytes to EOF.
  std::vector<uint8_t> grib2_short_coords =
      GribBuilder::BuildGrib2(Grib2MessageSpec{});
  ASSERT_GT(grib2_short_coords.size(), 115u);
  ASSERT_EQ(4u, grib2_short_coords[113]);
  ASSERT_EQ(0u, grib2_short_coords[114]);
  ASSERT_EQ(0u, grib2_short_coords[115]);
  grib2_short_coords[115] = 100;

  const TempFile file2("grib2_short_coords.grb2");
  ScopedProduct prod2;
  ASSERT_TRUE(WriteAndOpenGrib(file2, grib2_short_coords, prod2));

  coda_cursor g2_coords{};
  ASSERT_TRUE(
      GotoPath(prod2.get(), "/[0]/grib2/data[0]/coordinateValues", &g2_coords));
  long coord_count = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&g2_coords, &coord_count));
  EXPECT_EQ(100, coord_count);

  coda_cursor g2_elem = g2_coords;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&g2_elem, 99));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_float(&g2_elem, &oob_val));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  std::array<float, 100> g2_arr{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_float_array(&g2_coords, g2_arr.data(),
                                             coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  std::array<float, 2> g2_part{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_float_partial_array(&g2_coords, 98, 2,
                                                     g2_part.data()));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
}

// TODO(schwehr): 100.0% line coverage in libcoda/coda-grib-cursor.c
// (132/132); only WORDS_BIGENDIAN branches compiled out on little-endian
// hosts are excluded.

}  // namespace
}  // namespace stcorp_coda
