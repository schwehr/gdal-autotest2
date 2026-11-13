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
// Tests CODA's built-in NetCDF backend (libcoda/coda-netcdf.c), covering
// classic (CDF\x01), 64-bit offset (CDF\x02), and 64-bit data (CDF\x05)
// parsing, dimensions, global and variable attributes, conversions, record
// variables, >8D dimension collapsing, duplicate names, mmap/pread I/O, and
// malformed/truncated error handling.
//
// google3 build caveats:
//   - HAVE_NETCDF_H is undefined in config.h, so CODA uses its built-in
//     NetCDF parser in coda-netcdf.c.
//   - patches/libcoda_coda-netcdf.c.patch rejects num_dims < 0 || num_dims >
//     10000 with CODA_ERROR_PRODUCT.
//   - Built on little-endian Linux (WORDS_BIGENDIAN undefined), so big-endian
//     byte-swapping paths are active.
//   - Verified 100.0% reachable coverage ceiling (457/544 = 84.0%, 87 missed):
//     53 OOM allocation-failure paths (144-146, 222, 228, 250-253, 457-459,
//     471-474, 486-489, 493-496, 569-571, 632-633, 758-761, 787-789, 799-802,
//     816-818, 843, 845-846, 865, 867-868, 875-876, 882-883); 15 dead
//     nc_type 7..11/default switch lines in read_att_array() (414-441,
//     rejected earlier at 287-307); 17 unreachable setter-error lines on
//     freshly created objects (767-770, 779-782, 808-811, 932, 934-937); and
//     2 dead mem_ptr != NULL cleanup lines in coda_netcdf_close() (976-977).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-netcdf.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-netcdf.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-netcdf-internal.h

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/netcdf_builder.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin.h"
#include "third_party/stcorp_coda/libcoda/coda-netcdf.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

std::vector<uint8_t> BeInt8(std::initializer_list<int8_t> values) {
  std::vector<uint8_t> out;
  out.reserve(values.size());
  for (const int8_t v : values) {
    out.push_back(static_cast<uint8_t>(v));
  }
  return out;
}

std::vector<uint8_t> BeInt16(std::initializer_list<int16_t> values) {
  std::vector<uint8_t> out;
  out.reserve(values.size() * 2);
  for (const int16_t v : values) {
    NetcdfBuilder::AppendBe16(out, static_cast<uint16_t>(v));
  }
  return out;
}

std::vector<uint8_t> BeInt32(std::initializer_list<int32_t> values) {
  std::vector<uint8_t> out;
  out.reserve(values.size() * 4);
  for (const int32_t v : values) {
    NetcdfBuilder::AppendBe32(out, static_cast<uint32_t>(v));
  }
  return out;
}

std::vector<uint8_t> BeFloat(std::initializer_list<float> values) {
  std::vector<uint8_t> out;
  out.reserve(values.size() * 4);
  for (const float v : values) {
    NetcdfBuilder::AppendBe32(out, std::bit_cast<uint32_t>(v));
  }
  return out;
}

std::vector<uint8_t> BeDouble(std::initializer_list<double> values) {
  std::vector<uint8_t> out;
  out.reserve(values.size() * 8);
  for (const double v : values) {
    NetcdfBuilder::AppendBe64(out, std::bit_cast<uint64_t>(v));
  }
  return out;
}

std::vector<uint8_t> RawChars(std::string_view text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

// Appends a NetCDF variable header entry with `begin = 0` and returns the
// byte offset of the `begin` field for later patching.
size_t AppendVarHeader(std::vector<uint8_t>& out, int version,
                       std::string_view name, std::span<const int32_t> dim_ids,
                       std::span<const NetcdfAttrSpec> attrs, NetcdfType type,
                       int64_t vsize) {
  NetcdfBuilder helper(version);
  helper.AppendName(out, name);
  helper.AppendLength(out, static_cast<int64_t>(dim_ids.size()));
  for (const int32_t dim_id : dim_ids) {
    NetcdfBuilder::AppendBe32(out, static_cast<uint32_t>(dim_id));
  }
  helper.AppendAttrArray(out, attrs);
  NetcdfBuilder::AppendBe32(out, static_cast<uint32_t>(type));
  helper.AppendLength(out, vsize);
  const size_t begin_pos = out.size();
  helper.AppendOffset(out, 0);
  return begin_pos;
}

void PatchOffset(std::vector<uint8_t>& out, int version, size_t patch_pos,
                 int64_t offset) {
  if (version == 1) {
    out[patch_pos + 0] = static_cast<uint8_t>((offset >> 24) & 0xFF);
    out[patch_pos + 1] = static_cast<uint8_t>((offset >> 16) & 0xFF);
    out[patch_pos + 2] = static_cast<uint8_t>((offset >> 8) & 0xFF);
    out[patch_pos + 3] = static_cast<uint8_t>(offset & 0xFF);
  } else {
    for (int b = 0; b < 8; ++b) {
      out[patch_pos + b] =
          static_cast<uint8_t>((offset >> (56 - 8 * b)) & 0xFF);
    }
  }
}

void ReadCursorString(const coda_cursor& cursor, std::string* out) {
  long length = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &length));
  ASSERT_GE(length, 0);
  std::vector<char> buf(static_cast<size_t>(length) + 1, '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, buf.data(), length + 1));
  out->assign(buf.data(), static_cast<size_t>(length));
}

TEST(CodaNetcdfTest, RecognizesAndOpensEmptyFilesAcrossAllSupportedVersions) {
  constexpr int kVersions[] = {1, 2, 5};
  for (const int version : kVersions) {
    SCOPED_TRACE(version);
    ScopedCodaSession session;
    ASSERT_EQ(CODA_SUCCESS, session.Init());

    const std::vector<uint8_t> bytes = NetcdfBuilder(version).Build();
    TempFile file("netcdf_empty_v" + std::to_string(version) + ".nc");
    ASSERT_TRUE(file.WriteBytes(bytes));

    int64_t file_size = -1;
    coda_format format = coda_format_binary;
    const char* product_class = "sentinel";
    const char* product_type = "sentinel";
    int recognized_version = 42;
    ASSERT_EQ(
        CODA_SUCCESS,
        coda_recognize_file(file.c_str(), &file_size, &format, &product_class,
                            &product_type, &recognized_version));
    EXPECT_EQ(static_cast<int64_t>(bytes.size()), file_size);
    EXPECT_EQ(coda_format_netcdf, format);
    EXPECT_EQ(nullptr, product_class);
    EXPECT_EQ(nullptr, product_type);
    EXPECT_EQ(-1, recognized_version);

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    coda_format open_format = coda_format_binary;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_format(product.get(), &open_format));
    EXPECT_EQ(coda_format_netcdf, open_format);

    int64_t open_size = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_file_size(product.get(), &open_size));
    EXPECT_EQ(static_cast<int64_t>(bytes.size()), open_size);

    const char* filename = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_filename(product.get(), &filename));
    EXPECT_STREQ(file.c_str(), filename);

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    coda_type_class type_class = coda_raw_class;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
    EXPECT_EQ(coda_record_class, type_class);

    long num_elements = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_num_elements(&cursor, &num_elements));
    EXPECT_EQ(0, num_elements);

    int has_attrs = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&cursor, &has_attrs));
    EXPECT_EQ(0, has_attrs);
  }
}

TEST(CodaNetcdfTest, ReadsAllNumericAndTextTypesAcrossSupportedVersions) {
  constexpr int kVersions[] = {1, 2, 5};
  for (const int version : kVersions) {
    SCOPED_TRACE(version);
    ScopedCodaSession session;
    ASSERT_EQ(CODA_SUCCESS, session.Init());

    // Include both 4-byte aligned ("rows") and unaligned ("cols", "str6")
    // dimension/variable names to cover padding branches.
    NetcdfBuilder builder(version);
    builder.AddDim({.name = "rows", .size = 2});
    builder.AddDim({.name = "cols", .size = 3});
    builder.AddDim({.name = "str6", .size = 6});

    builder.AddVar({.name = "s_i8",
                    .dim_ids = {},
                    .attrs = {},
                    .type = NetcdfType::kByte,
                    .vsize = 4,
                    .data = BeInt8({-42})});
    builder.AddVar({.name = "s_ch",
                    .dim_ids = {},
                    .attrs = {},
                    .type = NetcdfType::kChar,
                    .vsize = 4,
                    .data = RawChars("Q")});
    builder.AddVar({.name = "s_i16",
                    .dim_ids = {},
                    .attrs = {},
                    .type = NetcdfType::kShort,
                    .vsize = 4,
                    .data = BeInt16({-1234})});
    builder.AddVar({.name = "s_i32",
                    .dim_ids = {},
                    .attrs = {},
                    .type = NetcdfType::kInt,
                    .vsize = 4,
                    .data = BeInt32({0x12345678})});
    builder.AddVar({.name = "s_f32",
                    .dim_ids = {},
                    .attrs = {},
                    .type = NetcdfType::kFloat,
                    .vsize = 4,
                    .data = BeFloat({3.25f})});
    builder.AddVar({.name = "s_f64",
                    .dim_ids = {},
                    .attrs = {},
                    .type = NetcdfType::kDouble,
                    .vsize = 8,
                    .data = BeDouble({-123.5})});
    // 1D fixed CHAR variable is treated as a scalar string.
    builder.AddVar({.name = "s_str",
                    .dim_ids = {2},
                    .attrs = {},
                    .type = NetcdfType::kChar,
                    .vsize = 8,
                    .data = RawChars("netcdf")});
    // 2D fixed CHAR variable [rows, str6] becomes a 1D array of strings.
    builder.AddVar({.name = "a_str",
                    .dim_ids = {0, 2},
                    .attrs = {},
                    .type = NetcdfType::kChar,
                    .vsize = 12,
                    .data = RawChars("alpha0beta11")});
    // 2D numeric arrays [rows, cols].
    builder.AddVar({.name = "a_i8",
                    .dim_ids = {0, 1},
                    .attrs = {},
                    .type = NetcdfType::kByte,
                    .vsize = 8,
                    .data = BeInt8({1, -2, 3, -4, 5, -6})});
    builder.AddVar({.name = "a_i16",
                    .dim_ids = {0, 1},
                    .attrs = {},
                    .type = NetcdfType::kShort,
                    .vsize = 12,
                    .data = BeInt16({10, -20, 30, -40, 50, -60})});
    builder.AddVar({.name = "a_i32",
                    .dim_ids = {0, 1},
                    .attrs = {},
                    .type = NetcdfType::kInt,
                    .vsize = 24,
                    .data = BeInt32({100, -200, 300, -400, 500, -600})});
    builder.AddVar({.name = "a_f32",
                    .dim_ids = {0, 1},
                    .attrs = {},
                    .type = NetcdfType::kFloat,
                    .vsize = 24,
                    .data = BeFloat({1.0f, 2.5f, -3.5f, 4.0f, 5.5f, -6.5f})});
    builder.AddVar({.name = "a_f64",
                    .dim_ids = {0, 1},
                    .attrs = {},
                    .type = NetcdfType::kDouble,
                    .vsize = 48,
                    .data = BeDouble({0.5, -1.5, 2.5, -3.5, 4.5, -5.5})});

    TempFile file("netcdf_types_v" + std::to_string(version) + ".nc");
    ASSERT_TRUE(file.WriteBytes(builder.Build()));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/s_i8"));
    int8_t v_i8 = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &v_i8));
    EXPECT_EQ(-42, v_i8);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/s_ch"));
    char v_ch = '\0';
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&cursor, &v_ch));
    EXPECT_EQ('Q', v_ch);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/s_i16"));
    int16_t v_i16 = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &v_i16));
    EXPECT_EQ(-1234, v_i16);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/s_i32"));
    int32_t v_i32 = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v_i32));
    EXPECT_EQ(0x12345678, v_i32);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/s_f32"));
    float v_f32 = 0.0f;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &v_f32));
    EXPECT_FLOAT_EQ(3.25f, v_f32);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/s_f64"));
    double v_f64 = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &v_f64));
    EXPECT_DOUBLE_EQ(-123.5, v_f64);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/s_str"));
    std::string str_val;
    ReadCursorString(cursor, &str_val);
    EXPECT_EQ("netcdf", str_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/a_str"));
    int num_dims = -1;
    std::array<long, CODA_MAX_NUM_DIMS> dims{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
    EXPECT_EQ(1, num_dims);
    EXPECT_EQ(2, dims[0]);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&cursor, 0));
    ReadCursorString(cursor, &str_val);
    EXPECT_EQ("alpha0", str_val);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&cursor));
    ReadCursorString(cursor, &str_val);
    EXPECT_EQ("beta11", str_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/a_i8"));
    std::vector<int8_t> arr_i8(6, -99);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8_array(&cursor, arr_i8.data(),
                                                        coda_array_ordering_c));
    EXPECT_THAT(arr_i8, ElementsAre(1, -2, 3, -4, 5, -6));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/a_i16"));
    std::vector<int16_t> arr_i16(6, -99);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int16_array(&cursor, arr_i16.data(),
                                           coda_array_ordering_c));
    EXPECT_THAT(arr_i16, ElementsAre(10, -20, 30, -40, 50, -60));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/a_i32"));
    std::vector<int32_t> arr_i32(6, -99);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_array(&cursor, arr_i32.data(),
                                           coda_array_ordering_c));
    EXPECT_THAT(arr_i32, ElementsAre(100, -200, 300, -400, 500, -600));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/a_f32"));
    std::vector<float> arr_f32(6, -99.0f);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_float_array(&cursor, arr_f32.data(),
                                           coda_array_ordering_c));
    EXPECT_THAT(arr_f32, ElementsAre(1.0f, 2.5f, -3.5f, 4.0f, 5.5f, -6.5f));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/a_f64"));
    std::vector<double> arr_f64(6, -99.0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_array(&cursor, arr_f64.data(),
                                            coda_array_ordering_c));
    EXPECT_THAT(arr_f64, ElementsAre(0.5, -1.5, 2.5, -3.5, 4.5, -5.5));
  }
}

TEST(CodaNetcdfTest, ReadsGlobalAndVariableAttributesWithAllTypesAndPadding) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  NetcdfBuilder builder(/*version=*/1);
  builder.AddDim({.name = "n", .size = 2});

  // Global attributes exercise all 6 NC types, scalar vs array attributes,
  // 4-byte aligned vs padded names, and 4-byte aligned vs padded values.
  builder.AddGlobalAttr({.name = "g_b1",
                         .type = NetcdfType::kByte,
                         .count = 1,
                         .raw_values = BeInt8({-7})});
  builder.AddGlobalAttr({.name = "g_b3",
                         .type = NetcdfType::kByte,
                         .count = 3,
                         .raw_values = BeInt8({11, 22, 33})});
  builder.AddGlobalAttr({.name = "g_c1",
                         .type = NetcdfType::kChar,
                         .count = 1,
                         .raw_values = RawChars("Z")});
  builder.AddGlobalAttr({.name = "g_c4",
                         .type = NetcdfType::kChar,
                         .count = 4,
                         .raw_values = RawChars("abcd")});
  builder.AddGlobalAttr({.name = "units",
                         .type = NetcdfType::kChar,
                         .count = 5,
                         .raw_values = RawChars("kelvn")});
  builder.AddGlobalAttr({.name = "g_s1",
                         .type = NetcdfType::kShort,
                         .count = 1,
                         .raw_values = BeInt16({-321})});
  builder.AddGlobalAttr({.name = "g_s2",
                         .type = NetcdfType::kShort,
                         .count = 2,
                         .raw_values = BeInt16({-100, 200})});
  builder.AddGlobalAttr({.name = "g_i1",
                         .type = NetcdfType::kInt,
                         .count = 1,
                         .raw_values = BeInt32({987654})});
  builder.AddGlobalAttr({.name = "g_i3",
                         .type = NetcdfType::kInt,
                         .count = 3,
                         .raw_values = BeInt32({1, -2, 3})});
  builder.AddGlobalAttr({.name = "g_f1",
                         .type = NetcdfType::kFloat,
                         .count = 1,
                         .raw_values = BeFloat({1.25f})});
  builder.AddGlobalAttr({.name = "g_d1",
                         .type = NetcdfType::kDouble,
                         .count = 1,
                         .raw_values = BeDouble({-9.5})});

  // Scalar variable with attributes, array variable with attributes, and a
  // scalar variable without attributes.
  builder.AddVar({.name = "scalar_with_attr",
                  .dim_ids = {},
                  .attrs = {{.name = "tag",
                             .type = NetcdfType::kChar,
                             .count = 3,
                             .raw_values = RawChars("ok!")}},
                  .type = NetcdfType::kInt,
                  .vsize = 4,
                  .data = BeInt32({77})});
  builder.AddVar({.name = "array_with_attr",
                  .dim_ids = {0},
                  .attrs = {{.name = "range",
                             .type = NetcdfType::kInt,
                             .count = 2,
                             .raw_values = BeInt32({0, 10})}},
                  .type = NetcdfType::kInt,
                  .vsize = 8,
                  .data = BeInt32({4, 8})});
  builder.AddVar({.name = "plain_var",
                  .dim_ids = {},
                  .attrs = {},
                  .type = NetcdfType::kInt,
                  .vsize = 4,
                  .data = BeInt32({12})});

  TempFile file("netcdf_attrs.nc");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  int has_attrs = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&cursor, &has_attrs));
  EXPECT_EQ(1, has_attrs);

  int8_t b1 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@g_b1"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &b1));
  EXPECT_EQ(-7, b1);

  std::vector<int8_t> b3(3, 0);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@g_b3"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8_array(&cursor, b3.data(),
                                                      coda_array_ordering_c));
  EXPECT_THAT(b3, ElementsAre(11, 22, 33));

  char c1 = '\0';
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@g_c1"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&cursor, &c1));
  EXPECT_EQ('Z', c1);

  std::string str;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@g_c4"));
  ReadCursorString(cursor, &str);
  EXPECT_EQ("abcd", str);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@units"));
  ReadCursorString(cursor, &str);
  EXPECT_EQ("kelvn", str);

  int16_t s1 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@g_s1"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &s1));
  EXPECT_EQ(-321, s1);

  std::vector<int16_t> s2(2, 0);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@g_s2"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(&cursor, s2.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(s2, ElementsAre(-100, 200));

  int32_t i1 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@g_i1"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &i1));
  EXPECT_EQ(987654, i1);

  std::vector<int32_t> i3(3, 0);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@g_i3"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(&cursor, i3.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(i3, ElementsAre(1, -2, 3));

  float f1 = 0.0f;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@g_f1"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &f1));
  EXPECT_FLOAT_EQ(1.25f, f1);

  double d1 = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@g_d1"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &d1));
  EXPECT_DOUBLE_EQ(-9.5, d1);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/scalar_with_attr@tag"));
  ReadCursorString(cursor, &str);
  EXPECT_EQ("ok!", str);

  std::vector<int32_t> range(2, -1);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/array_with_attr@range"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(&cursor, range.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(range, ElementsAre(0, 10));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/plain_var"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&cursor, &has_attrs));
  EXPECT_EQ(0, has_attrs);
}

TEST(CodaNetcdfTest, AppliesConversionsAndMissingValuePrecedence) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const int saved_conversions = coda_get_option_perform_conversions();
  const absl::Cleanup restore_conversions = [saved_conversions] {
    coda_set_option_perform_conversions(saved_conversions);
  };

  NetcdfBuilder builder(/*version=*/1);
  builder.AddDim({.name = "n", .size = 3});

  // 1. int16 array with float scale_factor and float add_offset.
  builder.AddVar({.name = "scaled_f32",
                  .dim_ids = {0},
                  .attrs =
                      {
                          {.name = "scale_factor",
                           .type = NetcdfType::kFloat,
                           .count = 1,
                           .raw_values = BeFloat({0.5f})},
                          {.name = "add_offset",
                           .type = NetcdfType::kFloat,
                           .count = 1,
                           .raw_values = BeFloat({10.0f})},
                      },
                  .type = NetcdfType::kShort,
                  .vsize = 8,
                  .data = BeInt16({2, -4, 6})});

  // 2. int32 array with double scale_factor/add_offset, missing_value before
  // _FillValue (missing_value takes precedence over _FillValue).
  builder.AddVar({.name = "prec_i32",
                  .dim_ids = {0},
                  .attrs =
                      {
                          {.name = "scale_factor",
                           .type = NetcdfType::kDouble,
                           .count = 1,
                           .raw_values = BeDouble({2.0})},
                          {.name = "add_offset",
                           .type = NetcdfType::kDouble,
                           .count = 1,
                           .raw_values = BeDouble({-3.0})},
                          {.name = "missing_value",
                           .type = NetcdfType::kInt,
                           .count = 1,
                           .raw_values = BeInt32({-999})},
                          {.name = "_FillValue",
                           .type = NetcdfType::kInt,
                           .count = 1,
                           .raw_values = BeInt32({-888})},
                      },
                  .type = NetcdfType::kInt,
                  .vsize = 12,
                  .data = BeInt32({5, -999, -888})});

  // 3. float array with only _FillValue (conversion preserved for
  // float/double).
  builder.AddVar({.name = "fill_f32",
                  .dim_ids = {0},
                  .attrs = {{.name = "_FillValue",
                             .type = NetcdfType::kFloat,
                             .count = 1,
                             .raw_values = BeFloat({-99.0f})}},
                  .type = NetcdfType::kFloat,
                  .vsize = 12,
                  .data = BeFloat({1.5f, -99.0f, 2.5f})});

  // 4. double array with only missing_value (conversion preserved).
  builder.AddVar({.name = "miss_f64",
                  .dim_ids = {0},
                  .attrs = {{.name = "missing_value",
                             .type = NetcdfType::kDouble,
                             .count = 1,
                             .raw_values = BeDouble({1e20})}},
                  .type = NetcdfType::kDouble,
                  .vsize = 24,
                  .data = BeDouble({1e20, 4.25, -2.5})});

  // 5. int8 scalar with scale_factor + int8 missing_value.
  builder.AddVar({.name = "miss_i8",
                  .dim_ids = {},
                  .attrs =
                      {
                          {.name = "scale_factor",
                           .type = NetcdfType::kDouble,
                           .count = 1,
                           .raw_values = BeDouble({2.0})},
                          {.name = "missing_value",
                           .type = NetcdfType::kByte,
                           .count = 1,
                           .raw_values = BeInt8({-128})},
                      },
                  .type = NetcdfType::kByte,
                  .vsize = 4,
                  .data = BeInt8({-128})});

  // 6. int16 scalar with add_offset + int16 missing_value.
  builder.AddVar({.name = "miss_i16",
                  .dim_ids = {},
                  .attrs =
                      {
                          {.name = "add_offset",
                           .type = NetcdfType::kFloat,
                           .count = 1,
                           .raw_values = BeFloat({100.0f})},
                          {.name = "missing_value",
                           .type = NetcdfType::kShort,
                           .count = 1,
                           .raw_values = BeInt16({-32768})},
                      },
                  .type = NetcdfType::kShort,
                  .vsize = 4,
                  .data = BeInt16({-32768})});

  // 7. int32 scalar with ONLY missing_value (non-float conversion deleted).
  builder.AddVar({.name = "raw_i32_miss_only",
                  .dim_ids = {},
                  .attrs =
                      {
                          {.name = "missing_value",
                           .type = NetcdfType::kInt,
                           .count = 1,
                           .raw_values = BeInt32({-1})},
                          {.name = "scale_factor",
                           .type = NetcdfType::kInt,
                           .count = 2,
                           .raw_values = BeInt32({1, 2})},
                      },
                  .type = NetcdfType::kInt,
                  .vsize = 4,
                  .data = BeInt32({-1})});

  TempFile file("netcdf_conversions.nc");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  // Check converted values with conversions enabled.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(1));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/scaled_f32"));
  std::vector<double> scaled_vals(3, 0.0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_double_array(&cursor, scaled_vals.data(),
                                          coda_array_ordering_c));
  EXPECT_THAT(scaled_vals, ElementsAre(11.0, 8.0, 13.0));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/prec_i32"));
  std::vector<double> prec_vals(3, 0.0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_double_array(&cursor, prec_vals.data(),
                                          coda_array_ordering_c));
  EXPECT_DOUBLE_EQ(7.0, prec_vals[0]);
  EXPECT_EQ(1, coda_isNaN(prec_vals[1]));
  EXPECT_DOUBLE_EQ(-1779.0, prec_vals[2]);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/fill_f32"));
  std::vector<double> fill_vals(3, 0.0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_double_array(&cursor, fill_vals.data(),
                                          coda_array_ordering_c));
  EXPECT_DOUBLE_EQ(1.5, fill_vals[0]);
  EXPECT_EQ(1, coda_isNaN(fill_vals[1]));
  EXPECT_DOUBLE_EQ(2.5, fill_vals[2]);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/miss_f64"));
  std::vector<double> miss_vals(3, 0.0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_double_array(&cursor, miss_vals.data(),
                                          coda_array_ordering_c));
  EXPECT_EQ(1, coda_isNaN(miss_vals[0]));
  EXPECT_DOUBLE_EQ(4.25, miss_vals[1]);
  EXPECT_DOUBLE_EQ(-2.5, miss_vals[2]);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/miss_i8"));
  double val = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
  EXPECT_EQ(1, coda_isNaN(val));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/miss_i16"));
  val = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
  EXPECT_EQ(1, coda_isNaN(val));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/raw_i32_miss_only"));
  coda_native_type read_type = coda_native_type_not_available;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&cursor, &read_type));
  EXPECT_EQ(coda_native_type_int32, read_type);
  int32_t raw_i32 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &raw_i32));
  EXPECT_EQ(-1, raw_i32);

  // Disabling conversions exposes raw unscaled int16 values.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/scaled_f32"));
  std::vector<int16_t> raw_i16(3, 0);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(&cursor, raw_i16.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(raw_i16, ElementsAre(2, -4, 6));
}

TEST(CodaNetcdfTest, ReadsInterleavedRecordVariablesAndOneDimRecordChar) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // Build a CDF\x01 file with 1 unlimited dim ("rec", size 0, num_recs = 3)
  // and 2 fixed dims ("x" = 2, "s4" = 4). Interleave record variables in the
  // record section so product->record_size spans multiple variables.
  constexpr int kVersion = 1;
  constexpr int64_t kNumRecs = 3;
  NetcdfBuilder hdr(kVersion);
  std::vector<uint8_t> out = {'C', 'D', 'F', 1};
  hdr.AppendLength(out, kNumRecs);

  // dim_array: rec(0), x(2), s4(4)
  NetcdfBuilder::AppendBe32(out, 10);  // NC_DIMENSION
  hdr.AppendLength(out, 3);
  hdr.AppendName(out, "rec");
  hdr.AppendLength(out, 0);
  hdr.AppendName(out, "x");
  hdr.AppendLength(out, 2);
  hdr.AppendName(out, "s4");
  hdr.AppendLength(out, 4);

  // gatt_array: empty
  hdr.AppendAttrArray(out, {});

  // var_array: 5 variables (1 fixed, 4 record)
  NetcdfBuilder::AppendBe32(out, 11);  // NC_VARIABLE
  hdr.AppendLength(out, 5);
  const std::array<int32_t, 1> dim_x = {1};
  const std::array<int32_t, 1> dim_rec = {0};
  const std::array<int32_t, 2> dim_rec_x = {0, 1};
  const std::array<int32_t, 2> dim_rec_s4 = {0, 2};

  const size_t fixed_pos = AppendVarHeader(out, kVersion, "fixed_i", dim_x, {},
                                           NetcdfType::kInt, /*vsize=*/8);
  const size_t rec_i16_pos = AppendVarHeader(
      out, kVersion, "rec_i16", dim_rec, {}, NetcdfType::kShort, /*vsize=*/4);
  const size_t rec_f64_pos =
      AppendVarHeader(out, kVersion, "rec_f64", dim_rec_x, {},
                      NetcdfType::kDouble, /*vsize=*/16);
  const size_t rec_ch1d_pos = AppendVarHeader(
      out, kVersion, "rec_ch1d", dim_rec, {}, NetcdfType::kChar, /*vsize=*/4);
  const size_t rec_str2d_pos =
      AppendVarHeader(out, kVersion, "rec_str2d", dim_rec_s4, {},
                      NetcdfType::kChar, /*vsize=*/4);

  // Fixed variable data.
  const int64_t fixed_off = static_cast<int64_t>(out.size());
  PatchOffset(out, kVersion, fixed_pos, fixed_off);
  const std::vector<uint8_t> fixed_bytes = BeInt32({100, 200});
  out.insert(out.end(), fixed_bytes.begin(), fixed_bytes.end());

  // Record section: 3 records, each containing rec_i16(4) + rec_f64(16) +
  // rec_ch1d(4) + rec_str2d(4) = 28 bytes.
  const int64_t rec_start = static_cast<int64_t>(out.size());
  PatchOffset(out, kVersion, rec_i16_pos, rec_start);
  PatchOffset(out, kVersion, rec_f64_pos, rec_start + 4);
  PatchOffset(out, kVersion, rec_ch1d_pos, rec_start + 20);
  PatchOffset(out, kVersion, rec_str2d_pos, rec_start + 24);

  constexpr std::array<int16_t, 3> kRecI16 = {10, 20, 30};
  constexpr std::array<double, 6> kRecF64 = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5};
  constexpr std::array<char, 3> kRecCh = {'A', 'B', 'C'};
  constexpr std::array<std::string_view, 3> kRecStr = {"r000", "r111", "r222"};
  for (int r = 0; r < kNumRecs; ++r) {
    const std::vector<uint8_t> i16_bytes = BeInt16({kRecI16[r]});
    out.insert(out.end(), i16_bytes.begin(), i16_bytes.end());
    NetcdfBuilder::AppendPad4(out, i16_bytes.size());

    const std::vector<uint8_t> f64_bytes =
        BeDouble({kRecF64[2 * r], kRecF64[2 * r + 1]});
    out.insert(out.end(), f64_bytes.begin(), f64_bytes.end());

    out.push_back(static_cast<uint8_t>(kRecCh[r]));
    NetcdfBuilder::AppendPad4(out, 1);

    out.insert(out.end(), kRecStr[r].begin(), kRecStr[r].end());
  }

  TempFile file("netcdf_records.nc");
  ASSERT_TRUE(file.WriteBytes(out));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/fixed_i"));
  std::vector<int32_t> fixed_vals(2, 0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_int32_array(&cursor, fixed_vals.data(),
                                         coda_array_ordering_c));
  EXPECT_THAT(fixed_vals, ElementsAre(100, 200));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/rec_i16"));
  std::vector<int16_t> i16_vals(3, 0);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(&cursor, i16_vals.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(i16_vals, ElementsAre(10, 20, 30));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/rec_f64"));
  std::vector<double> f64_vals(6, 0.0);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                              &cursor, f64_vals.data(), coda_array_ordering_c));
  EXPECT_THAT(f64_vals, ElementsAre(1.5, 2.5, 3.5, 4.5, 5.5, 6.5));

  // Element-by-element cursor read on record variable.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/rec_f64"));
  constexpr long kSubs[2] = {2, 1};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&cursor, 2, kSubs));
  double single_f64 = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &single_f64));
  EXPECT_DOUBLE_EQ(6.5, single_f64);

  // 1D record CHAR variable must remain a 1D array of char (not a string).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/rec_ch1d"));
  coda_type_class type_class = coda_raw_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_array_class, type_class);
  std::vector<char> ch_vals(3, '\0');
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char_array(&cursor, ch_vals.data(),
                                                      coda_array_ordering_c));
  EXPECT_THAT(ch_vals, ElementsAre('A', 'B', 'C'));

  // 2D record CHAR variable [rec, s4] becomes a 1D array of 4-byte strings.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/rec_str2d"));
  for (long idx = 0; idx < 3; ++idx) {
    coda_cursor elem = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&elem, idx));
    std::string s;
    ReadCursorString(elem, &s);
    EXPECT_EQ(kRecStr[idx], s);
  }
}

TEST(CodaNetcdfTest, CollapsesTrailingDimensionsBeyondMaxNumDims) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  NetcdfBuilder builder(/*version=*/1);
  builder.AddDim({.name = "d1", .size = 1});
  builder.AddDim({.name = "d2", .size = 2});

  // 9D variable: 8 dims stored directly, 9th collapsed into dim[7] -> dim[7]=2.
  builder.AddVar({.name = "var9d",
                  .dim_ids = {1, 0, 0, 0, 0, 0, 0, 0, 1},
                  .attrs = {},
                  .type = NetcdfType::kInt,
                  .vsize = 16,
                  .data = BeInt32({10, 20, 30, 40})});
  // 10D variable: 9th and 10th dims collapsed into dim[7] -> dim[7]=4.
  builder.AddVar({.name = "var10d",
                  .dim_ids = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1},
                  .attrs = {},
                  .type = NetcdfType::kInt,
                  .vsize = 32,
                  .data = BeInt32({1, 2, 3, 4, 5, 6, 7, 8})});

  TempFile file("netcdf_high_dims.nc");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/var9d"));
  int num_dims = -1;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(CODA_MAX_NUM_DIMS, num_dims);
  EXPECT_THAT(dims, ElementsAre(2, 1, 1, 1, 1, 1, 1, 2));
  std::vector<int32_t> vals9(4, 0);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(&cursor, vals9.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(vals9, ElementsAre(10, 20, 30, 40));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/var10d"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(CODA_MAX_NUM_DIMS, num_dims);
  EXPECT_THAT(dims, ElementsAre(1, 1, 1, 1, 1, 1, 1, 8));
  std::vector<int32_t> vals10(8, 0);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(&cursor, vals10.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(vals10, ElementsAre(1, 2, 3, 4, 5, 6, 7, 8));
}

TEST(CodaNetcdfTest, DisambiguatesDuplicateVariableAndAttributeNames) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  NetcdfBuilder builder(/*version=*/1);
  builder.AddGlobalAttr({.name = "dup",
                         .type = NetcdfType::kInt,
                         .count = 1,
                         .raw_values = BeInt32({111})});
  builder.AddGlobalAttr({.name = "dup",
                         .type = NetcdfType::kInt,
                         .count = 1,
                         .raw_values = BeInt32({222})});
  builder.AddVar({.name = "sig",
                  .dim_ids = {},
                  .attrs = {},
                  .type = NetcdfType::kInt,
                  .vsize = 4,
                  .data = BeInt32({10})});
  builder.AddVar({.name = "sig",
                  .dim_ids = {},
                  .attrs = {},
                  .type = NetcdfType::kInt,
                  .vsize = 4,
                  .data = BeInt32({20})});

  TempFile file("netcdf_duplicates.nc");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  coda_type* root_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_root_type(product.get(), &root_type));
  const char* name = nullptr;
  const char* real_name = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_record_field_name(root_type, 1, &name));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_record_field_real_name(root_type, 1, &real_name));
  EXPECT_STREQ("sig_1", name);
  EXPECT_STREQ("sig", real_name);

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  int32_t v = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/sig"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v));
  EXPECT_EQ(10, v);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/sig_1"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v));
  EXPECT_EQ(20, v);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@dup"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v));
  EXPECT_EQ(111, v);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@dup_1"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v));
  EXPECT_EQ(222, v);
}

TEST(CodaNetcdfTest, ReadsFileIdenticallyWithMmapEnabledAndDisabled) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const int saved_mmap = coda_get_option_use_mmap();
  const absl::Cleanup restore_mmap = [saved_mmap] {
    coda_set_option_use_mmap(saved_mmap);
  };

  NetcdfBuilder builder(/*version=*/2);
  builder.AddDim({.name = "n", .size = 2});
  builder.AddVar({.name = "vals",
                  .dim_ids = {0},
                  .attrs = {},
                  .type = NetcdfType::kInt,
                  .vsize = 8,
                  .data = BeInt32({7, -9})});

  TempFile file("netcdf_mmap.nc");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/vals"));
    std::vector<int32_t> vals(2, 0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &cursor, vals.data(), coda_array_ordering_c));
    EXPECT_THAT(vals, ElementsAre(7, -9));
  }
}

TEST(CodaNetcdfTest, OpenMissingFileFailsWithFileNotFound) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  const std::filesystem::path missing =
      std::filesystem::path(::testing::TempDir()) / "does_not_exist_netcdf.nc";
  ScopedProduct product;
  EXPECT_EQ(-1, coda_open(missing.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
}

TEST(CodaNetcdfTest, DirectReopenRejectsShortMagicAndUnsupportedVersion) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Fewer than 4 bytes fails inside coda_netcdf_reopen() magic read.
  TempFile short_file("netcdf_short_magic.nc");
  ASSERT_TRUE(short_file.WriteBytes(RawChars("CD")));
  coda_product* raw_short = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_bin_open(short_file.c_str(), 2, &raw_short));
  EXPECT_EQ(-1, coda_netcdf_reopen(&raw_short));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  // 2. Unsupported version byte (CDF\x03) fails with
  // CODA_ERROR_UNSUPPORTED_PRODUCT.
  coda_set_error(CODA_SUCCESS, nullptr);
  TempFile v3_file("netcdf_v3_unsupported.nc");
  const std::vector<uint8_t> v3_bytes = {'C', 'D', 'F', 3, 0, 0, 0, 0};
  ASSERT_TRUE(v3_file.WriteBytes(v3_bytes));
  coda_product* raw_v3 = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_bin_open(v3_file.c_str(),
                          static_cast<int64_t>(v3_bytes.size()), &raw_v3));
  EXPECT_EQ(-1, coda_netcdf_reopen(&raw_v3));
  EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("not a supported format version (3)"));
}

TEST(CodaNetcdfTest, RejectsMalformedTruncatedAndOverlimitFiles) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  auto make_oversized_num_dims = [] {
    // Pad past 10001 bytes so read_length_value() passes the <= file_size
    // check and reaches the Google patch (num_dims > 10000).
    std::vector<uint8_t> buf = {'C', 'D', 'F', 1, 0, 0, 0, 0};
    NetcdfBuilder::AppendBe32(buf, 10);     // NC_DIMENSION
    NetcdfBuilder::AppendBe32(buf, 10001);  // > 10000
    buf.resize(10030, 0);
    return buf;
  };

  struct MalformedCase {
    std::string_view label;
    std::vector<uint8_t> bytes;
    int expected_errno;
    std::string_view expected_substr;
  };

  const std::vector<MalformedCase> cases = {
      {
          .label = "truncated_numrecs_v1",
          .bytes = {'C', 'D', 'F', 1, 0, 0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_numrecs_v5",
          .bytes = {'C', 'D', 'F', 5, 0, 0, 0, 0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "negative_numrecs_v1",
          .bytes = {'C', 'D', 'F', 1, 0xFF, 0xFF, 0xFF, 0xFF},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "negative length value",
      },
      {
          .label = "negative_numrecs_v5",
          .bytes = {'C', 'D', 'F', 5, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                    0xFF},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "negative length value",
      },
      {
          .label = "numrecs_too_large",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 100},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "length value too large",
      },
      {
          .label = "truncated_dim_tag",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_dim_nelems",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0, 0, 10},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "empty_dim_nonzero_nelems",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "empty dim_array",
      },
      {
          .label = "bad_dim_tag",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "NC_DIMENSION tag",
      },
      {
          .label = "google_patch_num_dims_over_10000",
          .bytes = make_oversized_num_dims(),
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "Unreasonable num_dims (10001)",
      },
      {
          .label = "truncated_dim_name_len",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 1},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_dim_size",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0,   0,   0, 10,
                    0,   0,   0,   1, 0, 0, 0, 2, 'x', 'y', 0, 0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_gatt_with_dims",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0,   0, 0, 0, 0, 10, 0, 0,
                    0,   1,   0,   0, 0, 1, 'd', 0, 0, 0, 0, 0,  0, 2},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_gatt_nelems",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0, 12},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "empty_att_nonzero_nelems",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 1},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "empty att_array",
      },
      {
          .label = "bad_att_tag",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0,  0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 99, 0, 0, 0, 0},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "NC_ATTRIBUTE tag",
      },
      {
          .label = "truncated_att_name_len",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0,  0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 12, 0, 0, 0, 1},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_att_name_bytes",
          .bytes = {'C', 'D', 'F', 1,  0, 0, 0, 0, 0, 0, 0, 0,  0,   0,   0,  0,
                    0,   0,   0,   12, 0, 0, 0, 1, 0, 0, 0, 12, 'a', 'b', 'c'},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_att_type",
          .bytes = {'C', 'D', 'F', 1,  0, 0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0,
                    0,   0,   0,   12, 0, 0, 0, 1, 0, 0, 0, 1, 'a', 0, 0, 0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_att_nelems",
          .bytes = {'C', 'D', 'F', 1, 0,   0, 0, 0,  0, 0, 0, 0,
                    0,   0,   0,   0, 0,   0, 0, 12, 0, 0, 0, 1,
                    0,   0,   0,   1, 'a', 0, 0, 0,  0, 0, 0, 4},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "invalid_att_nc_type",
          .bytes = {'C', 'D', 'F', 1, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0,   0,
                    0,   0,   0,   0, 12, 0, 0, 0, 1, 0, 0, 0, 1, 'a', 0,
                    0,   0,   0,   0, 0,  7, 0, 0, 0, 1, 0, 0, 0, 0},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "invalid netcdf type (7)",
      },
      {
          .label = "truncated_var_array_with_dims",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0,   0, 0, 10,
                    0,   0,   0,   1, 0, 0, 0, 1, 'd', 0, 0, 0,
                    0,   0,   0,   2, 0, 0, 0, 0, 0,   0, 0, 0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_var_nelems",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "empty_var_nonzero_nelems",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "empty var_array",
      },
      {
          .label = "bad_var_tag",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "NC_VARIABLE tag",
      },
      {
          .label = "truncated_var_name_len",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 11, 0, 0, 0, 1},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_var_name_bytes",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0,  0,   0,  0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0, 0,  0,   0,  0,
                    0,   11,  0,   0, 0, 1, 0, 0, 0, 12, 'v', '1'},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_var_rank",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 11,
                    0,   0,   0,   1, 0, 0, 0, 1, 'v', 0, 0, 0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_var_dimid",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0,   0, 0, 0, 0, 0, 0,  0, 0,
                    0,   0,   0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 11, 0, 0,
                    0,   1,   0,   0, 0, 1, 'v', 0, 0, 0, 0, 0, 0,  1},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "invalid_var_dimid",
          .bytes = {'C', 'D', 'F', 1, 0,   0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0,
                    0,   0,   0,   0, 0,   0, 0, 0, 0, 0, 0, 11, 0, 0, 0, 1,
                    0,   0,   0,   1, 'v', 0, 0, 0, 0, 0, 0, 1,  0, 0, 0, 0},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "invalid dimid for variable v",
      },
      {
          .label = "truncated_var_scale_factor_float",
          .bytes = {'C', 'D', 'F', 1,   0,   0,   0,   0,   0,   0,   0,   0,
                    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
                    0,   0,   0,   11,  0,   0,   0,   1,   0,   0,   0,   1,
                    'v', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   12,
                    0,   0,   0,   1,   0,   0,   0,   12,  's', 'c', 'a', 'l',
                    'e', '_', 'f', 'a', 'c', 't', 'o', 'r', 0,   0,   0,   5,
                    0,   0,   0,   1,   0,   0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_var_scale_factor_double",
          .bytes = {'C', 'D', 'F', 1,   0,   0,   0,   0,   0,   0,   0,   0,
                    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
                    0,   0,   0,   11,  0,   0,   0,   1,   0,   0,   0,   1,
                    'v', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   12,
                    0,   0,   0,   1,   0,   0,   0,   12,  's', 'c', 'a', 'l',
                    'e', '_', 'f', 'a', 'c', 't', 'o', 'r', 0,   0,   0,   6,
                    0,   0,   0,   1,   0,   0,   0,   0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_var_missing_value_double",
          .bytes = {'C', 'D', 'F', 1,   0,   0,   0,   0,   0,   0,   0,   0,
                    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
                    0,   0,   0,   11,  0,   0,   0,   1,   0,   0,   0,   1,
                    'v', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   12,
                    0,   0,   0,   1,   0,   0,   0,   13,  'm', 'i', 's', 's',
                    'i', 'n', 'g', '_', 'v', 'a', 'l', 'u', 'e', 0,   0,   0,
                    0,   0,   0,   6,   0,   0,   0,   1,   0,   0,   0,   0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_var_vatt_bad_tag",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0,  0, 0,   0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0,  0, 0,   0, 0,
                    0,   11,  0,   0, 0, 1, 0, 0, 0,  1, 'v', 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 99, 0, 0,   0, 0},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "NC_ATTRIBUTE tag",
      },
      {
          .label = "truncated_var_nc_type",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0, 11, 0, 0, 0, 1, 0, 0, 0, 1,
                    'v', 0,   0,   0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "invalid_var_nc_type",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 11,
                    0,   0,   0,   1, 0, 0, 0, 1, 'v', 0, 0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 7},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "invalid netcdf type (7)",
      },
      {
          .label = "truncated_var_vsize",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 11,
                    0,   0,   0,   1, 0, 0, 0, 1, 'v', 0, 0, 0, 0, 0,
                    0,   0,   0,   0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 4},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_var_offset_v1",
          .bytes = {'C', 'D', 'F', 1, 0, 0, 0,   0, 0, 0, 0, 0, 0,  0, 0,
                    0,   0,   0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 11, 0, 0,
                    0,   1,   0,   0, 0, 1, 'v', 0, 0, 0, 0, 0, 0,  0, 0,
                    0,   0,   0,   0, 0, 0, 0,   0, 0, 0, 4, 0, 0,  0, 4},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "truncated_var_offset_v2",
          .bytes = {'C', 'D', 'F', 2, 0,   0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0,
                    0,   0,   0,   0, 0,   0, 0, 0, 0, 0, 0, 11, 0, 0, 0, 1,
                    0,   0,   0,   1, 'v', 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0,
                    0,   0,   0,   0, 0,   0, 0, 4, 0, 0, 0, 4,  0, 0, 0, 0},
          .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ,
          .expected_substr = "beyond the end of the file",
      },
      {
          .label = "var_offset_past_eof",
          .bytes = {'C', 'D', 'F', 1, 0,   0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0,
                    0,   0,   0,   0, 0,   0, 0, 0, 0, 0, 0, 11, 0, 0, 0, 1,
                    0,   0,   0,   1, 'v', 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0,
                    0,   0,   0,   0, 0,   0, 0, 4, 0, 0, 0, 4,  0, 0, 4, 0},
          .expected_errno = CODA_ERROR_PRODUCT,
          .expected_substr = "invalid offset for variable v",
      },
  };

  for (const MalformedCase& tc : cases) {
    SCOPED_TRACE(tc.label);
    coda_set_error(CODA_SUCCESS, nullptr);
    TempFile file("netcdf_bad_" + std::string(tc.label) + ".nc");
    ASSERT_TRUE(file.WriteBytes(tc.bytes));

    ScopedProduct product;
    EXPECT_EQ(-1, coda_open(file.c_str(), product.OutParam()));
    EXPECT_EQ(tc.expected_errno, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr(tc.expected_substr));
  }
}

// TODO(schwehr): Reach OOM error paths via allocator fault injection if needed;
// dead nc_type 7..11 cases in read_att_array() are unreachable upstream.

}  // namespace
}  // namespace stcorp_coda
