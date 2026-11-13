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
// Tests CODA netCDF dynamic type constructors, destructors, conversions, and
// attribute attachment (libcoda/coda-netcdf-type.c), plus end-to-end opening
// and reading of synthesized NetCDF classic (v1, v2, v5) files.
//
// google3 build caveats:
//   - HAVE_NETCDF_H is undefined in config.h; CODA uses its built-in NetCDF
//     classic parser (coda-netcdf.c) rather than an external netCDF library.
//   - coda-netcdf-internal.h lacks an extern "C" guard and requires
//     coda_internal_prelude.h first.
//   - All 11 missed lines in coda-netcdf-type.c (98/109 = 89.9% compiled-in
//     coverage) are unreachable without OOM injection or abort: 8 OOM paths
//     (68, 70, 80-81, 123, 125, 171-172), 1 default: assert(0); exit(1); (166),
//     and 2 defensive coda_type_set_read_type() failure lines (176-177) that
//     cannot fail for valid nc_type values 1..6.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-netcdf-type.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-netcdf-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-netcdf.h

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
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
#include "third_party/stcorp_coda/libcoda/coda-netcdf-internal.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;

template <typename T>
struct NetcdfTypeDeleter {
  void operator()(T* type) const {
    coda_netcdf_type_delete(reinterpret_cast<coda_dynamic_type*>(type));
  }
};

template <typename T>
using UniqueNetcdfType = std::unique_ptr<T, NetcdfTypeDeleter<T>>;

template <typename T>
struct CodaTypeReleaser {
  void operator()(T* type) const {
    coda_type_release(reinterpret_cast<coda_type*>(type));
  }
};

template <typename T>
using UniqueCodaType = std::unique_ptr<T, CodaTypeReleaser<T>>;

template <typename T>
struct MemTypeDeleter {
  void operator()(T* type) const {
    coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(type));
  }
};

template <typename T>
using UniqueMemType = std::unique_ptr<T, MemTypeDeleter<T>>;

struct ConversionDeleter {
  void operator()(coda_conversion* conversion) const {
    coda_conversion_delete(conversion);
  }
};

using UniqueConversion = std::unique_ptr<coda_conversion, ConversionDeleter>;

std::vector<uint8_t> Be16Bytes(int16_t value) {
  std::vector<uint8_t> out;
  NetcdfBuilder::AppendBe16(out, static_cast<uint16_t>(value));
  return out;
}

std::vector<uint8_t> Be32Bytes(int32_t value) {
  std::vector<uint8_t> out;
  NetcdfBuilder::AppendBe32(out, static_cast<uint32_t>(value));
  return out;
}

std::vector<uint8_t> BeFloatBytes(float value) {
  std::vector<uint8_t> out;
  NetcdfBuilder::AppendBe32(out, std::bit_cast<uint32_t>(value));
  return out;
}

std::vector<uint8_t> BeDoubleBytes(double value) {
  std::vector<uint8_t> out;
  NetcdfBuilder::AppendBe64(out, std::bit_cast<uint64_t>(value));
  return out;
}

std::vector<uint8_t> TextBytes(std::string_view text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

TEST(CodaNetcdfTypeTest, BasicTypeNewCoversAllNcTypesAndCharLengths) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct BasicTypeCase {
    int nc_type;
    int64_t offset;
    int record_var;
    int64_t length;
    coda_type_class expected_class;
    coda_native_type expected_read_type;
    int64_t expected_bit_size;
  };
  constexpr BasicTypeCase kCases[] = {
      {.nc_type = 1,
       .offset = 16,
       .record_var = 0,
       .length = 1,
       .expected_class = coda_integer_class,
       .expected_read_type = coda_native_type_int8,
       .expected_bit_size = 8},
      {.nc_type = 2,
       .offset = 32,
       .record_var = 0,
       .length = 0,
       .expected_class = coda_text_class,
       .expected_read_type = coda_native_type_char,
       .expected_bit_size = 0},
      {.nc_type = 2,
       .offset = 48,
       .record_var = 1,
       .length = 1,
       .expected_class = coda_text_class,
       .expected_read_type = coda_native_type_char,
       .expected_bit_size = 8},
      {.nc_type = 2,
       .offset = 64,
       .record_var = 0,
       .length = 12,
       .expected_class = coda_text_class,
       .expected_read_type = coda_native_type_string,
       .expected_bit_size = 96},
      {.nc_type = 3,
       .offset = 128,
       .record_var = 1,
       .length = 1,
       .expected_class = coda_integer_class,
       .expected_read_type = coda_native_type_int16,
       .expected_bit_size = 16},
      {.nc_type = 4,
       .offset = 256,
       .record_var = 0,
       .length = 1,
       .expected_class = coda_integer_class,
       .expected_read_type = coda_native_type_int32,
       .expected_bit_size = 32},
      {.nc_type = 5,
       .offset = 512,
       .record_var = 1,
       .length = 1,
       .expected_class = coda_real_class,
       .expected_read_type = coda_native_type_float,
       .expected_bit_size = 32},
      {.nc_type = 6,
       .offset = int64_t{1} << 33,
       .record_var = 0,
       .length = 1,
       .expected_class = coda_real_class,
       .expected_read_type = coda_native_type_double,
       .expected_bit_size = 64},
  };

  for (const BasicTypeCase& tc : kCases) {
    UniqueNetcdfType<coda_netcdf_basic_type> basic(coda_netcdf_basic_type_new(
        tc.nc_type, tc.offset, tc.record_var, tc.length));
    ASSERT_NE(nullptr, basic.get());

    EXPECT_EQ(coda_backend_netcdf, basic->backend);
    EXPECT_EQ(nullptr, basic->attributes);
    EXPECT_EQ(tc.offset, basic->offset);
    EXPECT_EQ(tc.record_var, basic->record_var);
    ASSERT_NE(nullptr, basic->definition);

    coda_format format = coda_format_binary;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_format(basic->definition, &format));
    EXPECT_EQ(coda_format_netcdf, format);

    coda_type_class type_class = coda_record_class;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_class(basic->definition, &type_class));
    EXPECT_EQ(tc.expected_class, type_class);

    coda_native_type read_type = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_read_type(basic->definition, &read_type));
    EXPECT_EQ(tc.expected_read_type, read_type);

    int64_t bit_size = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_bit_size(basic->definition, &bit_size));
    EXPECT_EQ(tc.expected_bit_size, bit_size);

    if (tc.nc_type == 2) {
      long string_length = -1;
      ASSERT_EQ(CODA_SUCCESS,
                coda_type_get_string_length(basic->definition, &string_length));
      EXPECT_EQ(tc.length, string_length);
    }
  }
}

TEST(CodaNetcdfTypeTest, BasicTypeNewRejectsNegativeCharLength) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // nc_type == 2 uses `length` directly as `byte_size`; negative lengths fail
  // inside coda_type_set_byte_size() and exercise cleanup via
  // coda_dynamic_type_delete().
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_netcdf_basic_type_new(2, 0, 0, -1));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
}

TEST(CodaNetcdfTypeTest, BasicTypeSetConversionSucceedsAndRejectsDuplicate) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Integer basic type accepts a conversion and transfers ownership.
  UniqueNetcdfType<coda_netcdf_basic_type> int_type(
      coda_netcdf_basic_type_new(3, 16, 0, 1));
  ASSERT_NE(nullptr, int_type.get());

  UniqueConversion conv1(coda_conversion_new(0.5, 1.0, 10.0, -999.0));
  ASSERT_NE(nullptr, conv1.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_netcdf_basic_type_set_conversion(int_type.get(), conv1.get()));
  conv1.release();

  const auto* num_def =
      reinterpret_cast<const coda_type_number*>(int_type->definition);
  ASSERT_NE(nullptr, num_def->conversion);
  EXPECT_EQ(0.5, num_def->conversion->numerator);
  EXPECT_EQ(1.0, num_def->conversion->denominator);
  EXPECT_EQ(10.0, num_def->conversion->add_offset);
  EXPECT_EQ(-999.0, num_def->conversion->invalid_value);

  // 2. Setting a second conversion on the same basic type fails.
  UniqueConversion dup_conv(coda_conversion_new(2.0, 1.0, 0.0, 0.0));
  ASSERT_NE(nullptr, dup_conv.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_netcdf_basic_type_set_conversion(int_type.get(),
                                                      dup_conv.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 3. Real basic type also accepts a conversion.
  UniqueNetcdfType<coda_netcdf_basic_type> real_type(
      coda_netcdf_basic_type_new(5, 32, 1, 1));
  ASSERT_NE(nullptr, real_type.get());
  UniqueConversion real_conv(coda_conversion_new(1.0, 1.0, 273.15, -1.0));
  ASSERT_NE(nullptr, real_conv.get());
  ASSERT_EQ(CODA_SUCCESS, coda_netcdf_basic_type_set_conversion(
                              real_type.get(), real_conv.get()));
  real_conv.release();
}

TEST(CodaNetcdfTypeTest, BasicTypeSetAttributesSucceedsAndDetectsConflicts) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Attach an attribute record with a nested NetCDF basic-type attribute.
  UniqueNetcdfType<coda_netcdf_basic_type> basic(
      coda_netcdf_basic_type_new(4, 0, 0, 1));
  ASSERT_NE(nullptr, basic.get());

  UniqueCodaType<coda_type_record> attr_def(
      coda_type_record_new(coda_format_netcdf));
  ASSERT_NE(nullptr, attr_def.get());
  UniqueMemType<coda_mem_record> attrs(
      coda_mem_record_new(attr_def.get(), nullptr));
  ASSERT_NE(nullptr, attrs.get());

  UniqueNetcdfType<coda_netcdf_basic_type> attr_val(
      coda_netcdf_basic_type_new(2, 12, 0, 1));
  ASSERT_NE(nullptr, attr_val.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(
                attrs.get(), "units",
                reinterpret_cast<coda_dynamic_type*>(attr_val.get()), 1));
  attr_val.release();

  ASSERT_EQ(CODA_SUCCESS,
            coda_netcdf_basic_type_set_attributes(basic.get(), attrs.get()));
  attrs.release();

  EXPECT_NE(nullptr, basic->attributes);
  int has_attributes = 0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_has_attributes(basic->definition, &has_attributes));
  EXPECT_EQ(1, has_attributes);

  coda_type* retrieved_attrs = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_attributes(basic->definition, &retrieved_attrs));
  EXPECT_EQ(reinterpret_cast<coda_type*>(attr_def.get()), retrieved_attrs);

  // 2. If definition->attributes is already set while basic->attributes is
  //    NULL, coda_netcdf_basic_type_set_attributes() returns -1.
  UniqueNetcdfType<coda_netcdf_basic_type> conflict_basic(
      coda_netcdf_basic_type_new(4, 0, 0, 1));
  ASSERT_NE(nullptr, conflict_basic.get());
  UniqueCodaType<coda_type_record> pre_attr_def(
      coda_type_record_new(coda_format_netcdf));
  ASSERT_NE(nullptr, pre_attr_def.get());
  ASSERT_EQ(CODA_SUCCESS, coda_type_set_attributes(conflict_basic->definition,
                                                   pre_attr_def.get()));

  UniqueCodaType<coda_type_record> new_attr_def(
      coda_type_record_new(coda_format_netcdf));
  ASSERT_NE(nullptr, new_attr_def.get());
  UniqueMemType<coda_mem_record> new_attrs(
      coda_mem_record_new(new_attr_def.get(), nullptr));
  ASSERT_NE(nullptr, new_attrs.get());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_netcdf_basic_type_set_attributes(conflict_basic.get(),
                                                      new_attrs.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(nullptr, conflict_basic->attributes);
}

TEST(CodaNetcdfTypeTest, ArrayNewSetAttributesAndDeletePaths) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Multi-dimensional array with base type and attributes.
  {
    UniqueNetcdfType<coda_netcdf_basic_type> base(
        coda_netcdf_basic_type_new(4, 64, 0, 1));
    ASSERT_NE(nullptr, base.get());

    std::array<long, CODA_MAX_NUM_DIMS> dims = {3, 4};
    UniqueNetcdfType<coda_netcdf_array> array(
        coda_netcdf_array_new(2, dims.data(), base.get()));
    ASSERT_NE(nullptr, array.get());
    base.release();

    EXPECT_EQ(coda_backend_netcdf, array->backend);
    EXPECT_NE(nullptr, array->base_type);
    EXPECT_EQ(nullptr, array->attributes);

    int num_dims = -1;
    std::array<long, CODA_MAX_NUM_DIMS> actual_dims{};
    actual_dims.fill(-999);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_array_dim(
                  reinterpret_cast<const coda_type*>(array->definition),
                  &num_dims, actual_dims.data()));
    EXPECT_EQ(2, num_dims);
    EXPECT_EQ(3, actual_dims[0]);
    EXPECT_EQ(4, actual_dims[1]);

    UniqueCodaType<coda_type_record> attr_def(
        coda_type_record_new(coda_format_netcdf));
    ASSERT_NE(nullptr, attr_def.get());
    UniqueMemType<coda_mem_record> attrs(
        coda_mem_record_new(attr_def.get(), nullptr));
    ASSERT_NE(nullptr, attrs.get());
    ASSERT_EQ(CODA_SUCCESS,
              coda_netcdf_array_set_attributes(array.get(), attrs.get()));
    attrs.release();

    EXPECT_NE(nullptr, array->attributes);
    int has_attributes = 0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_has_attributes(
                  reinterpret_cast<const coda_type*>(array->definition),
                  &has_attributes));
    EXPECT_EQ(1, has_attributes);
  }

  // 2. Zero-dimension array (0 iterations of the dimension loop) and cleanup
  // via
  //    coda_dynamic_type_delete() dispatch.
  {
    UniqueNetcdfType<coda_netcdf_basic_type> base(
        coda_netcdf_basic_type_new(1, 0, 0, 1));
    ASSERT_NE(nullptr, base.get());
    std::array<long, CODA_MAX_NUM_DIMS> dims{};
    UniqueNetcdfType<coda_netcdf_array> raw_array(
        coda_netcdf_array_new(0, dims.data(), base.get()));
    ASSERT_NE(nullptr, raw_array.get());
    base.release();

    int num_dims = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_array_num_dims(
                  reinterpret_cast<const coda_type*>(raw_array->definition),
                  &num_dims));
    EXPECT_EQ(0, num_dims);
  }

  // 3. Base-type format mismatch causes coda_type_array_set_base_type() to
  //    fail and deletes the partially built array (base_type == NULL branch in
  //    coda_netcdf_type_delete).
  {
    UniqueNetcdfType<coda_netcdf_basic_type> foreign_base(
        coda_netcdf_basic_type_new(1, 0, 0, 1));
    ASSERT_NE(nullptr, foreign_base.get());
    coda_type_number* ascii_def =
        coda_type_number_new(coda_format_ascii, coda_integer_class);
    ASSERT_NE(nullptr, ascii_def);
    coda_type_release(foreign_base->definition);
    foreign_base->definition = reinterpret_cast<coda_type*>(ascii_def);

    std::array<long, CODA_MAX_NUM_DIMS> dims = {2};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(nullptr,
              coda_netcdf_array_new(1, dims.data(), foreign_base.get()));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  }

  // 4. Negative dimension fails in coda_type_array_add_fixed_dimension() and
  //    cleans up the partially built array.
  {
    UniqueNetcdfType<coda_netcdf_basic_type> base(
        coda_netcdf_basic_type_new(6, 0, 0, 1));
    ASSERT_NE(nullptr, base.get());
    std::array<long, CODA_MAX_NUM_DIMS> bad_dims = {3, -2};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(nullptr, coda_netcdf_array_new(2, bad_dims.data(), base.get()));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  }

  // 5. coda_netcdf_array_set_attributes() returns -1 when
  // definition->attributes
  //    is already set while array->attributes is NULL.
  {
    UniqueNetcdfType<coda_netcdf_basic_type> base(
        coda_netcdf_basic_type_new(5, 0, 0, 1));
    ASSERT_NE(nullptr, base.get());
    std::array<long, CODA_MAX_NUM_DIMS> dims = {2};
    UniqueNetcdfType<coda_netcdf_array> array(
        coda_netcdf_array_new(1, dims.data(), base.get()));
    ASSERT_NE(nullptr, array.get());
    base.release();

    UniqueCodaType<coda_type_record> pre_attr_def(
        coda_type_record_new(coda_format_netcdf));
    ASSERT_NE(nullptr, pre_attr_def.get());
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_attributes(
                                reinterpret_cast<coda_type*>(array->definition),
                                pre_attr_def.get()));

    UniqueCodaType<coda_type_record> new_attr_def(
        coda_type_record_new(coda_format_netcdf));
    ASSERT_NE(nullptr, new_attr_def.get());
    UniqueMemType<coda_mem_record> new_attrs(
        coda_mem_record_new(new_attr_def.get(), nullptr));
    ASSERT_NE(nullptr, new_attrs.get());

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_netcdf_array_set_attributes(array.get(), new_attrs.get()));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
    EXPECT_EQ(nullptr, array->attributes);
  }
}

TEST(CodaNetcdfTypeTest, OpenMissingAndCorruptNetcdfFilesFail) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Non-existent path fails with CODA_ERROR_FILE_NOT_FOUND.
  const TempFile missing("netcdf_type_missing.nc");
  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  // 2. Truncated header (4-byte CDF\x01 magic only) fails with
  //    CODA_ERROR_OUT_OF_BOUNDS_READ while reading numrecs.
  const TempFile truncated("netcdf_type_truncated.nc");
  constexpr auto kTruncatedCdf1 = std::to_array<uint8_t>({'C', 'D', 'F', 0x01});
  ASSERT_TRUE(truncated.WriteBytes(kTruncatedCdf1));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(truncated.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  // 3. Invalid dim_array tag fails with CODA_ERROR_PRODUCT.
  const TempFile bad_tag("netcdf_type_bad_tag.nc");
  constexpr auto kBadDimTag = std::to_array<uint8_t>({
      'C',
      'D',
      'F',
      0x01,  // magic
      0,
      0,
      0,
      0,  // numrecs = 0
      0,
      0,
      0,
      99,  // invalid dim_array tag (expected 0 or 10)
      0,
      0,
      0,
      0,  // num_dims = 0
  });
  ASSERT_TRUE(bad_tag.WriteBytes(kBadDimTag));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(bad_tag.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  EXPECT_EQ(nullptr, product.get());
}

TEST(CodaNetcdfTypeTest, NetcdfBuilderAllTypesAttributesAndConversions) {
  const ScopedCodaOptions restore_options;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Synthesize a NetCDF classic (v1) file covering all 6 NetCDF types,
  //    global/variable/array attributes, scale_factor/add_offset/_FillValue,
  //    fixed arrays (short and char-string), and one record variable.
  NetcdfBuilder builder(/*version=*/1);
  builder.set_num_recs(2);
  builder.AddDim({.name = "rec", .size = 0});
  builder.AddDim({.name = "n", .size = 3});
  builder.AddDim({.name = "strlen", .size = 4});

  builder.AddGlobalAttr({.name = "title",
                         .type = NetcdfType::kChar,
                         .count = 4,
                         .raw_values = TextBytes("test")});
  std::vector<uint8_t> coeffs;
  NetcdfBuilder::AppendBe64(coeffs, std::bit_cast<uint64_t>(1.5));
  NetcdfBuilder::AppendBe64(coeffs, std::bit_cast<uint64_t>(-2.5));
  builder.AddGlobalAttr({.name = "coeffs",
                         .type = NetcdfType::kDouble,
                         .count = 2,
                         .raw_values = coeffs});

  builder.AddVar({.name = "s_byte",
                  .dim_ids = {},
                  .attrs = {},
                  .type = NetcdfType::kByte,
                  .vsize = 4,
                  .data = {static_cast<uint8_t>(int8_t{-5})}});
  builder.AddVar({.name = "s_char",
                  .dim_ids = {},
                  .attrs = {},
                  .type = NetcdfType::kChar,
                  .vsize = 4,
                  .data = {'Z'}});
  builder.AddVar({.name = "s_short",
                  .dim_ids = {},
                  .attrs = {{.name = "units",
                             .type = NetcdfType::kChar,
                             .count = 1,
                             .raw_values = TextBytes("K")}},
                  .type = NetcdfType::kShort,
                  .vsize = 4,
                  .data = Be16Bytes(-1234)});
  builder.AddVar({.name = "s_int",
                  .dim_ids = {},
                  .attrs = {},
                  .type = NetcdfType::kInt,
                  .vsize = 4,
                  .data = Be32Bytes(100000)});
  builder.AddVar({.name = "s_float",
                  .dim_ids = {},
                  .attrs = {{.name = "scale_factor",
                             .type = NetcdfType::kFloat,
                             .count = 1,
                             .raw_values = BeFloatBytes(2.0f)},
                            {.name = "add_offset",
                             .type = NetcdfType::kFloat,
                             .count = 1,
                             .raw_values = BeFloatBytes(1.0f)},
                            {.name = "_FillValue",
                             .type = NetcdfType::kFloat,
                             .count = 1,
                             .raw_values = BeFloatBytes(-999.0f)}},
                  .type = NetcdfType::kFloat,
                  .vsize = 4,
                  .data = BeFloatBytes(3.0f)});
  builder.AddVar({.name = "s_double",
                  .dim_ids = {},
                  .attrs = {{.name = "missing_value",
                             .type = NetcdfType::kDouble,
                             .count = 1,
                             .raw_values = BeDoubleBytes(-1.0)}},
                  .type = NetcdfType::kDouble,
                  .vsize = 8,
                  .data = BeDoubleBytes(-1.0)});

  std::vector<uint8_t> arr_i16_data;
  NetcdfBuilder::AppendBe16(arr_i16_data, static_cast<uint16_t>(10));
  NetcdfBuilder::AppendBe16(arr_i16_data, static_cast<uint16_t>(20));
  NetcdfBuilder::AppendBe16(arr_i16_data, static_cast<uint16_t>(30));
  std::vector<uint8_t> bounds_attr;
  NetcdfBuilder::AppendBe16(bounds_attr, static_cast<uint16_t>(0));
  NetcdfBuilder::AppendBe16(bounds_attr, static_cast<uint16_t>(100));
  builder.AddVar({.name = "arr_i16",
                  .dim_ids = {1},
                  .attrs = {{.name = "bounds",
                             .type = NetcdfType::kShort,
                             .count = 2,
                             .raw_values = bounds_attr}},
                  .type = NetcdfType::kShort,
                  .vsize = 8,
                  .data = arr_i16_data});

  builder.AddVar({.name = "arr_str",
                  .dim_ids = {1, 2},
                  .attrs = {},
                  .type = NetcdfType::kChar,
                  .vsize = 12,
                  .data = {'o', 'n', 'e', '\0', 't', 'w', 'o', '\0', 's', 'i',
                           'x', '\0'}});

  std::vector<uint8_t> rec_i32_data;
  NetcdfBuilder::AppendBe32(rec_i32_data, 111);
  NetcdfBuilder::AppendBe32(rec_i32_data, 222);
  builder.AddVar({.name = "rec_i32",
                  .dim_ids = {0},
                  .attrs = {},
                  .type = NetcdfType::kInt,
                  .vsize = 4,
                  .data = rec_i32_data});

  const TempFile nc_file("netcdf_type_all.nc");
  ASSERT_TRUE(nc_file.WriteBytes(builder.Build()));

  // 2. Open via coda_open() and inspect types, attributes, and values.
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(nc_file.c_str(), product.OutParam()));
  coda_format format = coda_format_binary;
  ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &format));
  EXPECT_EQ(coda_format_netcdf, format);

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  // Global attributes.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "@title"));
  long title_len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &title_len));
  EXPECT_EQ(4, title_len);
  std::vector<char> title_buf(static_cast<size_t>(title_len) + 1, '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, title_buf.data(),
                                    static_cast<long>(title_buf.size())));
  EXPECT_STREQ("test", title_buf.data());
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "@coeffs"));
  std::vector<double> actual_coeffs(2, -999.0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_double_array(&cursor, actual_coeffs.data(),
                                          coda_array_ordering_c));
  EXPECT_THAT(actual_coeffs, ElementsAre(1.5, -2.5));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  // Scalar variables.
  int8_t v_byte = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "s_byte"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &v_byte));
  EXPECT_EQ(-5, v_byte);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  char v_char = '\0';
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "s_char"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&cursor, &v_char));
  EXPECT_EQ('Z', v_char);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  int16_t v_short = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "s_short"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &v_short));
  EXPECT_EQ(-1234, v_short);
  char units_char = '\0';
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "@units"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&cursor, &units_char));
  EXPECT_EQ('K', units_char);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  int32_t v_int = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "s_int"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v_int));
  EXPECT_EQ(100000, v_int);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  // Scale/offset conversion and _FillValue/missing_value handling.
  double v_float_conv = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "s_float"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &v_float_conv));
  EXPECT_DOUBLE_EQ(7.0, v_float_conv);
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
  float v_float_raw = 0.0f;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &v_float_raw));
  EXPECT_FLOAT_EQ(3.0f, v_float_raw);
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(1));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  double v_double_missing = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "s_double"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &v_double_missing));
  EXPECT_EQ(1, coda_isNaN(v_double_missing));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  // Fixed array with array attributes.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "arr_i16"));
  std::vector<int16_t> actual_i16(3, -999);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_int16_array(&cursor, actual_i16.data(),
                                         coda_array_ordering_c));
  EXPECT_THAT(actual_i16, ElementsAre(10, 20, 30));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "@bounds"));
  std::vector<int16_t> actual_bounds(2, -999);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_int16_array(&cursor, actual_bounds.data(),
                                         coda_array_ordering_c));
  EXPECT_THAT(actual_bounds, ElementsAre(0, 100));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  // 2D char array mapped to 1D array of strings.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "arr_str[1]"));
  long elem_len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &elem_len));
  EXPECT_EQ(4, elem_len);
  std::vector<char> elem_buf(static_cast<size_t>(elem_len) + 1, '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, elem_buf.data(),
                                    static_cast<long>(elem_buf.size())));
  EXPECT_STREQ("two", elem_buf.data());
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  // Record variable along unlimited dimension.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "rec_i32"));
  std::vector<int32_t> actual_rec(2, -999);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_int32_array(&cursor, actual_rec.data(),
                                         coda_array_ordering_c));
  EXPECT_THAT(actual_rec, ElementsAre(111, 222));
}

TEST(CodaNetcdfTypeTest, NetcdfBuilderVersions2And5AndRecordCharArray) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. NetCDF v2 (64-bit offsets) with a 1D record char variable (treated as a
  //    char array rather than a string when dim[0] is appendable).
  {
    NetcdfBuilder v2(/*version=*/2);
    v2.set_num_recs(2);
    v2.AddDim({.name = "rec", .size = 0});
    v2.AddVar({.name = "flags",
               .dim_ids = {0},
               .attrs = {},
               .type = NetcdfType::kChar,
               .vsize = 4,
               .data = {'A', '\0', '\0', '\0', 'B', '\0', '\0', '\0'}});

    const TempFile v2_file("netcdf_type_v2.nc");
    ASSERT_TRUE(v2_file.WriteBytes(v2.Build()));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(v2_file.c_str(), product.OutParam()));
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "flags"));

    coda_type* arr_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &arr_type));
    coda_type* base_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_array_base_type(arr_type, &base_type));
    coda_native_type read_type = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_read_type(base_type, &read_type));
    EXPECT_EQ(coda_native_type_char, read_type);

    std::vector<char> flags(2, '\0');
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char_array(&cursor, flags.data(),
                                                        coda_array_ordering_c));
    EXPECT_THAT(flags, ElementsAre('A', 'B'));
  }

  // 2. NetCDF v5 (CDF-5 64-bit lengths/offsets) with a scalar double variable.
  {
    NetcdfBuilder v5(/*version=*/5);
    v5.AddVar({.name = "val",
               .dim_ids = {},
               .attrs = {},
               .type = NetcdfType::kDouble,
               .vsize = 8,
               .data = BeDoubleBytes(42.25)});

    const TempFile v5_file("netcdf_type_v5.nc");
    ASSERT_TRUE(v5_file.WriteBytes(v5.Build()));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(v5_file.c_str(), product.OutParam()));
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "val"));
    double val = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
    EXPECT_DOUBLE_EQ(42.25, val);
  }
}

// TODO(schwehr): 89.9% reachable ceiling in libcoda/coda-netcdf-type.c
// (98/109); remaining 11 missed lines are 8 OOM branches, 1 assert(0)/exit(1),
// and 2 unreachable set_read_type error paths.

}  // namespace
}  // namespace stcorp_coda
