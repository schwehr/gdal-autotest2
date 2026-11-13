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
// Tests CODA static type constructors, metadata/expression setters, record and
// union layout rules, validation helpers, singletons, and public type getters
// (libcoda/coda-type.c).
//
// google3 build caveats:
//   - Internal headers lack extern "C" guards and require
//     coda_internal_prelude.h before coda-type.h.
//   - Upstream coda_type_record_insert_field() appends `field` to a union's
//     field array before rejecting `bit_offset_expr != NULL`, so the union
//     retains ownership of `field` even though the call returns -1.
//   - Upstream coda_type_raw_file_singleton() is a TLS singleton not released
//     by coda_type_done().
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-type.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-type.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

#include "third_party/stcorp_coda/google/util/scoped_coda.h"

namespace stcorp_coda {
namespace {

template <typename T>
struct CodaTypeReleaser {
  void operator()(T* type) const {
    coda_type_release(reinterpret_cast<coda_type*>(type));
  }
};

template <typename T>
using UniqueCodaType = std::unique_ptr<T, CodaTypeReleaser<T>>;

struct RecordFieldDeleter {
  void operator()(coda_type_record_field* field) const {
    coda_type_record_field_delete(field);
  }
};
using UniqueRecordField =
    std::unique_ptr<coda_type_record_field, RecordFieldDeleter>;

struct ConversionDeleter {
  void operator()(coda_conversion* conversion) const {
    coda_conversion_delete(conversion);
  }
};
using UniqueConversion = std::unique_ptr<coda_conversion, ConversionDeleter>;

struct IntMappingDeleter {
  void operator()(coda_ascii_integer_mapping* mapping) const {
    coda_ascii_integer_mapping_delete(mapping);
  }
};
using UniqueIntMapping =
    std::unique_ptr<coda_ascii_integer_mapping, IntMappingDeleter>;

struct FloatMappingDeleter {
  void operator()(coda_ascii_float_mapping* mapping) const {
    coda_ascii_float_mapping_delete(mapping);
  }
};
using UniqueFloatMapping =
    std::unique_ptr<coda_ascii_float_mapping, FloatMappingDeleter>;

struct FreeDeleter {
  void operator()(void* ptr) const { std::free(ptr); }
};
using UniqueMallocString = std::unique_ptr<char, FreeDeleter>;

coda_type* AsBaseType(coda_type_record* type) {
  return reinterpret_cast<coda_type*>(type);
}
coda_type* AsBaseType(coda_type_array* type) {
  return reinterpret_cast<coda_type*>(type);
}
coda_type* AsBaseType(coda_type_number* type) {
  return reinterpret_cast<coda_type*>(type);
}
coda_type* AsBaseType(coda_type_text* type) {
  return reinterpret_cast<coda_type*>(type);
}
coda_type* AsBaseType(coda_type_raw* type) {
  return reinterpret_cast<coda_type*>(type);
}
coda_type* AsBaseType(coda_type_special* type) {
  return reinterpret_cast<coda_type*>(type);
}

UniqueCodaType<coda_type_number> MakeIntType(coda_format format,
                                             coda_native_type read_type,
                                             int64_t bit_size) {
  UniqueCodaType<coda_type_number> type(
      coda_type_number_new(format, coda_integer_class));
  if (type == nullptr) {
    return nullptr;
  }
  if (coda_type_set_read_type(AsBaseType(type.get()), read_type) !=
      CODA_SUCCESS) {
    return nullptr;
  }
  if (bit_size >= 0 && coda_type_set_bit_size(AsBaseType(type.get()),
                                              bit_size) != CODA_SUCCESS) {
    return nullptr;
  }
  return type;
}

UniqueCodaType<coda_type_number> MakeRealType(coda_format format,
                                              coda_native_type read_type,
                                              int64_t bit_size) {
  UniqueCodaType<coda_type_number> type(
      coda_type_number_new(format, coda_real_class));
  if (type == nullptr) {
    return nullptr;
  }
  if (coda_type_set_read_type(AsBaseType(type.get()), read_type) !=
      CODA_SUCCESS) {
    return nullptr;
  }
  if (bit_size >= 0 && coda_type_set_bit_size(AsBaseType(type.get()),
                                              bit_size) != CODA_SUCCESS) {
    return nullptr;
  }
  return type;
}

UniqueCodaType<coda_type_text> MakeTextType(coda_format format,
                                            int64_t bit_size) {
  UniqueCodaType<coda_type_text> type(coda_type_text_new(format));
  if (type == nullptr) {
    return nullptr;
  }
  if (bit_size >= 0 && coda_type_set_bit_size(AsBaseType(type.get()),
                                              bit_size) != CODA_SUCCESS) {
    return nullptr;
  }
  return type;
}

class CodaTypeTest : public ::testing::Test {
 protected:
  void SetUp() override { coda_set_error(CODA_SUCCESS, nullptr); }

  void TearDown() override {
    coda_type_done();
    coda_set_error(CODA_SUCCESS, nullptr);
  }
};

TEST_F(CodaTypeTest, FormatClassNativeAndSpecialNames) {
  struct FormatCase {
    coda_format format;
    std::string_view expected;
  };
  constexpr FormatCase kFormatCases[] = {
      {.format = coda_format_ascii, .expected = "ascii"},
      {.format = coda_format_binary, .expected = "binary"},
      {.format = coda_format_xml, .expected = "xml"},
      {.format = coda_format_hdf4, .expected = "hdf4"},
      {.format = coda_format_hdf5, .expected = "hdf5"},
      {.format = coda_format_cdf, .expected = "cdf"},
      {.format = coda_format_netcdf, .expected = "netcdf"},
      {.format = coda_format_grib, .expected = "grib"},
      {.format = coda_format_rinex, .expected = "rinex"},
      {.format = coda_format_sp3, .expected = "sp3"},
      {.format = static_cast<coda_format>(10), .expected = "unknown"},
  };
  for (const auto& tc : kFormatCases) {
    EXPECT_EQ(tc.expected, coda_type_get_format_name(tc.format));
  }

  struct ClassCase {
    coda_type_class type_class;
    std::string_view expected;
  };
  constexpr ClassCase kClassCases[] = {
      {.type_class = coda_record_class, .expected = "record"},
      {.type_class = coda_array_class, .expected = "array"},
      {.type_class = coda_integer_class, .expected = "integer"},
      {.type_class = coda_real_class, .expected = "real"},
      {.type_class = coda_text_class, .expected = "text"},
      {.type_class = coda_raw_class, .expected = "raw"},
      {.type_class = coda_special_class, .expected = "special"},
      {.type_class = static_cast<coda_type_class>(7), .expected = "unknown"},
  };
  for (const auto& tc : kClassCases) {
    EXPECT_EQ(tc.expected, coda_type_get_class_name(tc.type_class));
  }

  struct NativeTypeCase {
    coda_native_type native_type;
    std::string_view expected;
  };
  constexpr NativeTypeCase kNativeCases[] = {
      {.native_type = coda_native_type_not_available, .expected = "N/A"},
      {.native_type = coda_native_type_int8, .expected = "int8"},
      {.native_type = coda_native_type_uint8, .expected = "uint8"},
      {.native_type = coda_native_type_int16, .expected = "int16"},
      {.native_type = coda_native_type_uint16, .expected = "uint16"},
      {.native_type = coda_native_type_int32, .expected = "int32"},
      {.native_type = coda_native_type_uint32, .expected = "uint32"},
      {.native_type = coda_native_type_int64, .expected = "int64"},
      {.native_type = coda_native_type_uint64, .expected = "uint64"},
      {.native_type = coda_native_type_float, .expected = "float"},
      {.native_type = coda_native_type_double, .expected = "double"},
      {.native_type = coda_native_type_char, .expected = "char"},
      {.native_type = coda_native_type_string, .expected = "string"},
      {.native_type = coda_native_type_bytes, .expected = "bytes"},
      {.native_type = static_cast<coda_native_type>(13), .expected = "unknown"},
  };
  for (const auto& tc : kNativeCases) {
    EXPECT_EQ(tc.expected, coda_type_get_native_type_name(tc.native_type));
  }

  struct SpecialTypeCase {
    coda_special_type special_type;
    std::string_view expected;
  };
  constexpr SpecialTypeCase kSpecialCases[] = {
      {.special_type = coda_special_no_data, .expected = "no_data"},
      {.special_type = coda_special_vsf_integer, .expected = "vsf_integer"},
      {.special_type = coda_special_time, .expected = "time"},
      {.special_type = coda_special_complex, .expected = "complex"},
  };
  for (const auto& tc : kSpecialCases) {
    EXPECT_EQ(tc.expected, coda_type_get_special_type_name(tc.special_type));
  }
}

TEST_F(CodaTypeTest, ConversionLifecycleAndUnit) {
  coda_conversion_delete(nullptr);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_conversion_new(1.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueConversion conv(coda_conversion_new(2.5, 4.0, -1.5, -999.0));
  ASSERT_NE(nullptr, conv);
  EXPECT_DOUBLE_EQ(2.5, conv->numerator);
  EXPECT_DOUBLE_EQ(4.0, conv->denominator);
  EXPECT_DOUBLE_EQ(-1.5, conv->add_offset);
  EXPECT_DOUBLE_EQ(-999.0, conv->invalid_value);
  EXPECT_EQ(nullptr, conv->unit);

  EXPECT_EQ(CODA_SUCCESS, coda_conversion_set_unit(conv.get(), nullptr));
  EXPECT_EQ(nullptr, conv->unit);

  EXPECT_EQ(CODA_SUCCESS, coda_conversion_set_unit(conv.get(), "m/s"));
  EXPECT_STREQ("m/s", conv->unit);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_conversion_set_unit(conv.get(), "km/h"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
}

TEST_F(CodaTypeTest, AsciiMappingsCreateDeleteAndLengthBounds) {
  coda_ascii_integer_mapping_delete(nullptr);
  coda_ascii_float_mapping_delete(nullptr);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_ascii_integer_mapping_new(nullptr, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  const std::string too_long(MAX_ASCII_NUMBER_LENGTH + 1, '9');
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_ascii_integer_mapping_new(too_long.c_str(), 1));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  const std::string max_len(MAX_ASCII_NUMBER_LENGTH, '1');
  UniqueIntMapping int_map(coda_ascii_integer_mapping_new(max_len.c_str(), 42));
  ASSERT_NE(nullptr, int_map);
  EXPECT_EQ(MAX_ASCII_NUMBER_LENGTH, int_map->length);
  EXPECT_STREQ(max_len.c_str(), int_map->str);
  EXPECT_EQ(42, int_map->value);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_ascii_float_mapping_new(nullptr, 1.0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_ascii_float_mapping_new(too_long.c_str(), 1.0));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueFloatMapping flt_map(coda_ascii_float_mapping_new("", -1.25));
  ASSERT_NE(nullptr, flt_map);
  EXPECT_EQ(0, flt_map->length);
  EXPECT_STREQ("", flt_map->str);
  EXPECT_DOUBLE_EQ(-1.25, flt_map->value);
}

TEST_F(CodaTypeTest, CommonMetadataNameDescriptionAndAttributes) {
  coda_type_release(nullptr);

  UniqueCodaType<coda_type_number> num =
      MakeIntType(coda_format_binary, coda_native_type_int32, 32);
  ASSERT_NE(nullptr, num);
  coda_type* base = AsBaseType(num.get());

  // Basic getters and NULL checks.
  coda_format format = coda_format_ascii;
  coda_type_class type_class = coda_record_class;
  const char* name = nullptr;
  const char* description = nullptr;
  int has_attrs = -1;
  coda_type* attrs_type = nullptr;

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_format(nullptr, &format));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_format(base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_format(base, &format));
  EXPECT_EQ(coda_format_binary, format);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_class(nullptr, &type_class));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_class(base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_class(base, &type_class));
  EXPECT_EQ(coda_integer_class, type_class);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_name(nullptr, &name));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_name(base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_name(base, &name));
  EXPECT_EQ(nullptr, name);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_description(nullptr, &description));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_description(base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_description(base, &description));
  EXPECT_EQ(nullptr, description);

  // Set name error and success paths.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_name(nullptr, "a"));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_name(base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  for (const char* bad_name : {"", "1abc", "a-b"}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_type_set_name(base, bad_name)) << bad_name;
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  }
  EXPECT_EQ(CODA_SUCCESS, coda_type_set_name(base, "my_int_1"));
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_name(base, &name));
  EXPECT_STREQ("my_int_1", name);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_name(base, "other_name"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Set description error and success paths.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_description(nullptr, "desc"));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_description(base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_set_description(base, "sample integer"));
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_description(base, &description));
  EXPECT_STREQ("sample integer", description);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_description(base, "second desc"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Attribute getters before attributes are set return the empty record
  // singleton.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_has_attributes(nullptr, &has_attrs));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_has_attributes(base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_has_attributes(base, &has_attrs));
  EXPECT_EQ(0, has_attrs);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_attributes(nullptr, &attrs_type));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_attributes(base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_attributes(base, &attrs_type));
  EXPECT_EQ(AsBaseType(coda_type_empty_record(coda_format_binary)), attrs_type);

  // Add attributes incrementally via coda_type_add_attribute.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_add_attribute(nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_add_attribute(base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  UniqueCodaType<coda_type_text> attr_val =
      MakeTextType(coda_format_binary, 32);
  ASSERT_NE(nullptr, attr_val);

  UniqueRecordField attr1(coda_type_record_field_new("scale"));
  ASSERT_NE(nullptr, attr1);
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              attr1.get(), AsBaseType(attr_val.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_add_attribute(base, attr1.get()));
  (void)attr1.release();

  UniqueRecordField attr2(coda_type_record_field_new("offset"));
  ASSERT_NE(nullptr, attr2);
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              attr2.get(), AsBaseType(attr_val.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_add_attribute(base, attr2.get()));
  (void)attr2.release();

  EXPECT_EQ(CODA_SUCCESS, coda_type_has_attributes(base, &has_attrs));
  EXPECT_EQ(1, has_attrs);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_attributes(base, &attrs_type));
  long num_attrs = -1;
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_num_record_fields(attrs_type, &num_attrs));
  EXPECT_EQ(2, num_attrs);

  // Set attributes directly via coda_type_set_attributes.
  UniqueCodaType<coda_type_raw> raw(coda_type_raw_new(coda_format_binary));
  ASSERT_NE(nullptr, raw);
  UniqueCodaType<coda_type_record> attr_rec(
      coda_type_record_new(coda_format_binary));
  ASSERT_NE(nullptr, attr_rec);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_attributes(nullptr, attr_rec.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_attributes(AsBaseType(raw.get()), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  EXPECT_EQ(CODA_SUCCESS,
            coda_type_set_attributes(AsBaseType(raw.get()), attr_rec.get()));
  EXPECT_EQ(1, attr_rec->retain_count);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_set_attributes(AsBaseType(raw.get()), attr_rec.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

TEST_F(CodaTypeTest, SetReadTypeValidationAcrossClasses) {
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_read_type(nullptr, coda_native_type_int32));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Disallowed classes: record, array, raw, special.
  UniqueCodaType<coda_type_record> rec(
      coda_type_record_new(coda_format_binary));
  UniqueCodaType<coda_type_array> arr(coda_type_array_new(coda_format_binary));
  UniqueCodaType<coda_type_raw> raw(coda_type_raw_new(coda_format_binary));
  UniqueCodaType<coda_type_special> complex(
      coda_type_complex_new(coda_format_binary));
  for (coda_type* t : {AsBaseType(rec.get()), AsBaseType(arr.get()),
                       AsBaseType(raw.get()), AsBaseType(complex.get())}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_type_set_read_type(t, coda_native_type_bytes));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  }

  // Integer class allows all 8 signed/unsigned integer native types only.
  UniqueCodaType<coda_type_number> int_num(
      coda_type_number_new(coda_format_binary, coda_integer_class));
  ASSERT_NE(nullptr, int_num);
  constexpr coda_native_type kValidIntTypes[] = {
      coda_native_type_int8,   coda_native_type_uint8,  coda_native_type_int16,
      coda_native_type_uint16, coda_native_type_int32,  coda_native_type_uint32,
      coda_native_type_int64,  coda_native_type_uint64,
  };
  for (coda_native_type nt : kValidIntTypes) {
    EXPECT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(AsBaseType(int_num.get()), nt));
  }
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_read_type(AsBaseType(int_num.get()),
                                        coda_native_type_double));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Real class allows float and double only.
  UniqueCodaType<coda_type_number> real_num(
      coda_type_number_new(coda_format_binary, coda_real_class));
  ASSERT_NE(nullptr, real_num);
  EXPECT_EQ(CODA_SUCCESS, coda_type_set_read_type(AsBaseType(real_num.get()),
                                                  coda_native_type_float));
  EXPECT_EQ(CODA_SUCCESS, coda_type_set_read_type(AsBaseType(real_num.get()),
                                                  coda_native_type_double));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_read_type(AsBaseType(real_num.get()),
                                        coda_native_type_int32));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Text class allows char and string only.
  UniqueCodaType<coda_type_text> text(coda_type_text_new(coda_format_ascii));
  ASSERT_NE(nullptr, text);
  EXPECT_EQ(CODA_SUCCESS, coda_type_set_read_type(AsBaseType(text.get()),
                                                  coda_native_type_char));
  EXPECT_EQ(CODA_SUCCESS, coda_type_set_read_type(AsBaseType(text.get()),
                                                  coda_native_type_string));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_read_type(AsBaseType(text.get()),
                                        coda_native_type_bytes));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
}

TEST_F(CodaTypeTest, BitByteSizeAndSizeExpressionSetters) {
  int64_t bit_size = 0;

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size(nullptr, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_bit_size(nullptr, &bit_size));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  UniqueCodaType<coda_type_raw> bin_raw(coda_type_raw_new(coda_format_binary));
  ASSERT_NE(nullptr, bin_raw);
  coda_type* bin_base = AsBaseType(bin_raw.get());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_bit_size(bin_base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size(bin_base, -2));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // ASCII types require byte-rounded bit sizes.
  UniqueCodaType<coda_type_text> ascii_text(
      coda_type_text_new(coda_format_ascii));
  ASSERT_NE(nullptr, ascii_text);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size(AsBaseType(ascii_text.get()), 7));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  EXPECT_EQ(CODA_SUCCESS,
            coda_type_set_byte_size(AsBaseType(ascii_text.get()), 3));
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_bit_size(AsBaseType(ascii_text.get()), &bit_size));
  EXPECT_EQ(24, bit_size);

  // Fixed bit size cannot be overwritten or replaced with a size expression.
  EXPECT_EQ(CODA_SUCCESS, coda_type_set_bit_size(bin_base, 5));
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_bit_size(bin_base, &bit_size));
  EXPECT_EQ(5, bit_size);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size(bin_base, 8));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("16", expr.OutParam()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size_expression(bin_base, expr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_byte_size_expression(bin_base, expr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Scalar bit_size_expression and byte_size_expression on fresh raw types.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size_expression(nullptr, expr.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size_expression(bin_base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_byte_size_expression(nullptr, expr.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_byte_size_expression(bin_base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  UniqueCodaType<coda_type_raw> dyn_bits(coda_type_raw_new(coda_format_binary));
  ASSERT_EQ(CODA_SUCCESS, coda_type_set_bit_size_expression(
                              AsBaseType(dyn_bits.get()), expr.get()));
  (void)expr.release();
  EXPECT_EQ(-1, dyn_bits->bit_size);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_bit_size(AsBaseType(dyn_bits.get()), &bit_size));
  EXPECT_EQ(-1, bit_size);

  // Already has a size expression: bit_size, bit_size_expr, byte_size_expr
  // all fail.
  ScopedExpr expr2;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("4", expr2.OutParam()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size(AsBaseType(dyn_bits.get()), 8));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size_expression(AsBaseType(dyn_bits.get()),
                                                  expr2.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_byte_size_expression(AsBaseType(dyn_bits.get()),
                                                   expr2.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_raw> dyn_bytes(
      coda_type_raw_new(coda_format_binary));
  ASSERT_EQ(CODA_SUCCESS, coda_type_set_byte_size_expression(
                              AsBaseType(dyn_bytes.get()), expr2.get()));
  (void)expr2.release();
  EXPECT_EQ(-8, dyn_bytes->bit_size);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_bit_size(AsBaseType(dyn_bytes.get()), &bit_size));
  EXPECT_EQ(-1, bit_size);

  // Compound types allow size expressions when bit_size == 0, but reject when
  // bit_size > 0.
  UniqueCodaType<coda_type_record> empty_bin_rec(
      coda_type_record_new(coda_format_binary));
  ScopedExpr rec_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("16", rec_expr.OutParam()));
  EXPECT_EQ(CODA_SUCCESS, coda_type_set_bit_size_expression(
                              AsBaseType(empty_bin_rec.get()), rec_expr.get()));
  (void)rec_expr.release();

  UniqueCodaType<coda_type_record> empty_bin_rec2(
      coda_type_record_new(coda_format_binary));
  ScopedExpr rec_expr2;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("2", rec_expr2.OutParam()));
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_set_byte_size_expression(AsBaseType(empty_bin_rec2.get()),
                                               rec_expr2.get()));
  (void)rec_expr2.release();

  UniqueCodaType<coda_type_record> sized_rec(
      coda_type_record_new(coda_format_binary));
  UniqueCodaType<coda_type_number> i8 =
      MakeIntType(coda_format_binary, coda_native_type_int8, 8);
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_create_field(sized_rec.get(), "b",
                                                        AsBaseType(i8.get())));
  ScopedExpr fail_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("8", fail_expr.OutParam()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size_expression(AsBaseType(sized_rec.get()),
                                                  fail_expr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_byte_size_expression(AsBaseType(sized_rec.get()),
                                                   fail_expr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Array compound types follow the same bit_size == 0 vs > 0 rule.
  UniqueCodaType<coda_type_array> sized_arr(
      coda_type_array_new(coda_format_binary));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_add_fixed_dimension(sized_arr.get(), 2));
  ASSERT_EQ(CODA_SUCCESS, coda_type_array_set_base_type(sized_arr.get(),
                                                        AsBaseType(i8.get())));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size_expression(AsBaseType(sized_arr.get()),
                                                  fail_expr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_byte_size_expression(AsBaseType(sized_arr.get()),
                                                   fail_expr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
}

TEST_F(CodaTypeTest, RecordFieldHelpersAndExpressions) {
  coda_type_record_field_delete(nullptr);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_type_record_field_new(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_type_record_field_new("9invalid"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueRecordField field(coda_type_record_field_new("field_1"));
  ASSERT_NE(nullptr, field);

  // Validate before setting type fails.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_validate(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_validate(field.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // set_real_name checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_real_name(nullptr, "x"));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_real_name(field.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_record_field_set_real_name(field.get(), "Real Name"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_real_name(field.get(), "Other"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // set_type / get_type checks.
  UniqueCodaType<coda_type_number> bin_u8 =
      MakeIntType(coda_format_binary, coda_native_type_uint8, 8);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_record_field_set_type(nullptr, AsBaseType(bin_u8.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_type(field.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              field.get(), AsBaseType(bin_u8.get())));
  EXPECT_EQ(1, bin_u8->retain_count);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_type(field.get(),
                                                AsBaseType(bin_u8.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_type* got_type = nullptr;
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_record_field_get_type(field.get(), &got_type));
  EXPECT_EQ(AsBaseType(bin_u8.get()), got_type);
  EXPECT_EQ(CODA_SUCCESS, coda_type_record_field_validate(field.get()));

  // hidden / optional / available_expr checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_hidden(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_record_field_set_hidden(field.get()));
  EXPECT_EQ(1, field->hidden);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_optional(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(field.get()));
  EXPECT_EQ(1, field->optional);

  ScopedExpr avail_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("true", avail_expr.OutParam()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_available_expression(
                    nullptr, avail_expr.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_available_expression(field.get(),
                                                                nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_record_field_set_available_expression(
                              field.get(), avail_expr.get()));
  (void)avail_expr.release();
  coda_set_error(CODA_SUCCESS, nullptr);
  ScopedExpr avail_expr2;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("false", avail_expr2.OutParam()));
  EXPECT_EQ(-1, coda_type_record_field_set_available_expression(
                    field.get(), avail_expr2.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // bit_offset_expression checks.
  ScopedExpr offset_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("16", offset_expr.OutParam()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_bit_offset_expression(
                    nullptr, offset_expr.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_bit_offset_expression(field.get(),
                                                                 nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_type_record_field_set_bit_offset_expression(
                              field.get(), offset_expr.get()));
  (void)offset_expr.release();

  ScopedExpr offset_expr2;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("24", offset_expr2.OutParam()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_bit_offset_expression(
                    field.get(), offset_expr2.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // XML fields disallow bit_offset_expression.
  UniqueRecordField xml_field(coda_type_record_field_new("xml_f"));
  UniqueCodaType<coda_type_text> xml_text = MakeTextType(coda_format_xml, -1);
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              xml_field.get(), AsBaseType(xml_text.get())));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_field_set_bit_offset_expression(
                    xml_field.get(), offset_expr2.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
}

TEST_F(CodaTypeTest, RecordFormatCompatibilityInsertOffsetsAndGetters) {
  UniqueCodaType<coda_type_record> bin_rec(
      coda_type_record_new(coda_format_binary));
  UniqueCodaType<coda_type_record> ascii_rec(
      coda_type_record_new(coda_format_ascii));
  UniqueCodaType<coda_type_record> xml_rec(
      coda_type_record_new(coda_format_xml));
  UniqueCodaType<coda_type_record> netcdf_rec(
      coda_type_record_new(coda_format_netcdf));
  ASSERT_NE(nullptr, bin_rec);
  ASSERT_NE(nullptr, ascii_rec);
  ASSERT_NE(nullptr, xml_rec);
  ASSERT_NE(nullptr, netcdf_rec);

  coda_native_type rt = coda_native_type_not_available;
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_read_type(AsBaseType(bin_rec.get()), &rt));
  EXPECT_EQ(coda_native_type_bytes, rt);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_read_type(AsBaseType(xml_rec.get()), &rt));
  EXPECT_EQ(coda_native_type_not_available, rt);

  // Null / incomplete arguments to add_field and insert_field.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_add_field(nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_insert_field(nullptr, 0, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_add_field(bin_rec.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  UniqueRecordField typeless(coda_type_record_field_new("no_type"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_add_field(bin_rec.get(), typeless.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Format compatibility rule (coda-type.c:1291): binary/xml allow same format
  // or ascii; ascii/netcdf allow only their own format.
  UniqueCodaType<coda_type_number> ascii_i8 =
      MakeIntType(coda_format_ascii, coda_native_type_int8, 8);
  UniqueCodaType<coda_type_number> bin_u8 =
      MakeIntType(coda_format_binary, coda_native_type_uint8, 8);
  UniqueCodaType<coda_type_number> bin_u16 =
      MakeIntType(coda_format_binary, coda_native_type_uint16, 16);
  UniqueCodaType<coda_type_number> bin_u32 =
      MakeIntType(coda_format_binary, coda_native_type_uint32, 32);
  UniqueCodaType<coda_type_text> xml_txt = MakeTextType(coda_format_xml, -1);

  EXPECT_EQ(CODA_SUCCESS,
            coda_type_record_create_field(bin_rec.get(), "ascii_ok",
                                          AsBaseType(ascii_i8.get())));
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_record_create_field(xml_rec.get(), "ascii_ok",
                                          AsBaseType(ascii_i8.get())));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_create_field(bin_rec.get(), "bad_xml",
                                              AsBaseType(xml_txt.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_create_field(ascii_rec.get(), "bad_bin",
                                              AsBaseType(bin_u8.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_create_field(netcdf_rec.get(), "bad_ascii",
                                              AsBaseType(ascii_i8.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Insert in the middle and verify bit offsets and bit_size recalculation.
  UniqueCodaType<coda_type_record> layout_rec(
      coda_type_record_new(coda_format_binary));
  UniqueRecordField f_first(coda_type_record_field_new("first"));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              f_first.get(), AsBaseType(bin_u8.get())));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(layout_rec.get(), f_first.get()));
  (void)f_first.release();

  UniqueRecordField f_third(coda_type_record_field_new("third"));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              f_third.get(), AsBaseType(bin_u16.get())));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(layout_rec.get(), f_third.get()));
  (void)f_third.release();

  UniqueRecordField f_second(coda_type_record_field_new("second"));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              f_second.get(), AsBaseType(bin_u32.get())));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_insert_field(layout_rec.get(), 1, f_second.get()));
  (void)f_second.release();

  EXPECT_EQ(0, layout_rec->field[0]->bit_offset);
  EXPECT_EQ(8, layout_rec->field[1]->bit_offset);
  EXPECT_EQ(40, layout_rec->field[2]->bit_offset);
  EXPECT_EQ(56, layout_rec->bit_size);

  // Duplicate field name rejected.
  UniqueRecordField dup_field(coda_type_record_field_new("second"));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              dup_field.get(), AsBaseType(bin_u8.get())));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_add_field(layout_rec.get(), dup_field.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Add > BLOCK_SIZE (16) fields to exercise realloc growth.
  for (int i = 0; i < 16; ++i) {
    const std::string fname = "extra_" + std::to_string(i);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_create_field(layout_rec.get(), fname.c_str(),
                                            AsBaseType(bin_u8.get())));
  }
  long num_fields = 0;
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_num_record_fields(
                              AsBaseType(layout_rec.get()), &num_fields));
  EXPECT_EQ(19, num_fields);

  // Adding a field with bit_offset_expr and an optional field resets bit_size
  // to -1.
  UniqueRecordField offset_f(coda_type_record_field_new("dyn_off"));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              offset_f.get(), AsBaseType(bin_u8.get())));
  ScopedExpr off_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("0", off_expr.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_bit_offset_expression(
                              offset_f.get(), off_expr.get()));
  (void)off_expr.release();
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(layout_rec.get(), offset_f.get()));
  (void)offset_f.release();
  EXPECT_EQ(-1, layout_rec->field[19]->bit_offset);

  UniqueRecordField opt_f(coda_type_record_field_new("opt_f"));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              opt_f.get(), AsBaseType(bin_u8.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(opt_f.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_hidden(opt_f.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(layout_rec.get(), opt_f.get()));
  (void)opt_f.release();
  EXPECT_EQ(-1, layout_rec->bit_size);

  // Adding a fixed field after an optional field leaves bit_offset as -1.
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_create_field(layout_rec.get(), "after_opt",
                                          AsBaseType(bin_u8.get())));
  EXPECT_EQ(-1, layout_rec->field[21]->bit_offset);

  // Inserting at index 0 before a field with bit_offset_expr preserves that
  // field's -1 bit_offset while updating earlier fixed offsets.
  {
    UniqueCodaType<coda_type_record> ins_rec(
        coda_type_record_new(coda_format_binary));
    UniqueRecordField f_off(coda_type_record_field_new("with_off"));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                                f_off.get(), AsBaseType(bin_u8.get())));
    ScopedExpr e0;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("8", e0.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_bit_offset_expression(
                                f_off.get(), e0.get()));
    (void)e0.release();
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_add_field(ins_rec.get(), f_off.get()));
    (void)f_off.release();

    UniqueRecordField f_head(coda_type_record_field_new("head"));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                                f_head.get(), AsBaseType(bin_u8.get())));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_insert_field(ins_rec.get(), 0, f_head.get()));
    (void)f_head.release();
    EXPECT_EQ(0, ins_rec->field[0]->bit_offset);
    EXPECT_EQ(-1, ins_rec->field[1]->bit_offset);
  }

  // Field lookup & getters (including non-NUL-terminated name_n and XML real
  // names).
  long idx = -1;
  constexpr std::array<char, 9> kNotNullTerm = {'s', 'e', 'c', 'o', 'n',
                                                'd', 'X', 'Y', 'Z'};
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_record_field_index_from_name_n(
                AsBaseType(layout_rec.get()), kNotNullTerm.data(), 6, &idx));
  EXPECT_EQ(1, idx);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_get_record_field_index_from_name_n(
                AsBaseType(layout_rec.get()), kNotNullTerm.data(), 9, &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_field_index_from_name(
                              AsBaseType(layout_rec.get()), "third", &idx));
  EXPECT_EQ(2, idx);

  // create_field with XML namespace and duplicate real_name.
  UniqueMallocString unique_name(coda_type_record_get_unique_field_name(
      xml_rec.get(), "http://example.com my-tag"));
  ASSERT_NE(nullptr, unique_name);
  EXPECT_STREQ("my_tag", unique_name.get());

  EXPECT_EQ(CODA_SUCCESS, coda_type_record_create_field(
                              xml_rec.get(), "http://example.com my-tag",
                              AsBaseType(xml_txt.get())));
  EXPECT_EQ(CODA_SUCCESS, coda_type_record_create_field(
                              xml_rec.get(), "http://example.com my-tag",
                              AsBaseType(xml_txt.get())));

  const char* got_name = nullptr;
  const char* got_real_name = nullptr;
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_field_name(
                              AsBaseType(xml_rec.get()), 2, &got_name));
  EXPECT_STREQ("my_tag_1", got_name);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_field_real_name(
                              AsBaseType(xml_rec.get()), 1, &got_real_name));
  EXPECT_STREQ("my-tag", got_real_name);

  // Explicit real_name without XML stripping, plus fallback to field->name.
  UniqueRecordField real_f(coda_type_record_field_new("alias_f"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_field_set_real_name(real_f.get(), "ns raw-field"));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              real_f.get(), AsBaseType(bin_u8.get())));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(layout_rec.get(), real_f.get()));
  (void)real_f.release();

  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_field_index_from_real_name(
                              AsBaseType(layout_rec.get()), "first", &idx));
  EXPECT_EQ(0, idx);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_record_field_index_from_real_name(
                AsBaseType(layout_rec.get()), "ns raw-field", &idx));
  EXPECT_EQ(22, idx);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_record_field_real_name(AsBaseType(layout_rec.get()),
                                                 22, &got_real_name));
  EXPECT_STREQ("ns raw-field", got_real_name);

  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_field_real_name(
                              AsBaseType(layout_rec.get()), 0, &got_real_name));
  EXPECT_STREQ("first", got_real_name);

  int hidden = -1;
  int available = 0;
  int is_union = -1;
  coda_type* ftype = nullptr;
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_field_type(
                              AsBaseType(layout_rec.get()), 2, &ftype));
  EXPECT_EQ(AsBaseType(bin_u16.get()), ftype);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_field_hidden_status(
                              AsBaseType(layout_rec.get()), 0, &hidden));
  EXPECT_EQ(0, hidden);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_field_hidden_status(
                              AsBaseType(layout_rec.get()), 20, &hidden));
  EXPECT_EQ(1, hidden);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_field_available_status(
                              AsBaseType(layout_rec.get()), 0, &available));
  EXPECT_EQ(1, available);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_field_available_status(
                              AsBaseType(layout_rec.get()), 20, &available));
  EXPECT_EQ(-1, available);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_union_status(
                              AsBaseType(layout_rec.get()), &is_union));
  EXPECT_EQ(0, is_union);

  // Record validation and error paths for all record getter functions.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_validate(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_record_validate(layout_rec.get()));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_create_field(nullptr, "x",
                                              AsBaseType(bin_u8.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_create_field(layout_rec.get(), nullptr,
                                              AsBaseType(bin_u8.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_create_field(layout_rec.get(), "x", nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_type* non_rec = AsBaseType(bin_u8.get());
  coda_type* rec_base = AsBaseType(layout_rec.get());

  // NULL type / non-record / NULL out / bad index / bad name checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_num_record_fields(nullptr, &num_fields));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_num_record_fields(non_rec, &num_fields));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_num_record_fields(rec_base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_name(nullptr, "a", &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_name(non_rec, "a", &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_get_record_field_index_from_name(rec_base, nullptr, &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_get_record_field_index_from_name(rec_base, "a", nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_name(rec_base, "missing",
                                                           &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_get_record_field_index_from_name_n(nullptr, "a", 1, &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_get_record_field_index_from_name_n(non_rec, "a", 1, &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_name_n(rec_base, nullptr,
                                                             1, &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_name_n(rec_base, "a", 1,
                                                             nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_get_record_field_index_from_real_name(nullptr, "a", &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_get_record_field_index_from_real_name(non_rec, "a", &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_real_name(rec_base,
                                                                nullptr, &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_real_name(rec_base, "a",
                                                                nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_real_name(
                    rec_base, "missing", &idx));
  EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_type(nullptr, 0, &ftype));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_type(non_rec, 0, &ftype));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_type(rec_base, 0, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_type(rec_base, 99, &ftype));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_name(nullptr, 0, &got_name));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_name(non_rec, 0, &got_name));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_name(rec_base, 0, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_name(rec_base, -1, &got_name));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_get_record_field_real_name(nullptr, 0, &got_real_name));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_get_record_field_real_name(non_rec, 0, &got_real_name));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_real_name(rec_base, 0, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_get_record_field_real_name(rec_base, 99, &got_real_name));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_hidden_status(nullptr, 0, &hidden));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_hidden_status(non_rec, 0, &hidden));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_hidden_status(rec_base, 0, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_get_record_field_hidden_status(rec_base, -1, &hidden));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_get_record_field_available_status(nullptr, 0, &available));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_get_record_field_available_status(non_rec, 0, &available));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_get_record_field_available_status(rec_base, 0, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_field_available_status(rec_base, 99,
                                                            &available));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_union_status(nullptr, &is_union));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_union_status(non_rec, &is_union));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_record_union_status(rec_base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

TEST_F(CodaTypeTest, UnionConstructionFieldExpressionAndValidation) {
  UniqueCodaType<coda_type_record> bin_union(
      coda_type_union_new(coda_format_binary));
  ASSERT_NE(nullptr, bin_union);

  int is_union = 0;
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_record_union_status(
                              AsBaseType(bin_union.get()), &is_union));
  EXPECT_EQ(1, is_union);

  // Empty union fails validation.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_validate(bin_union.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Union rejects non-optional fields.
  UniqueCodaType<coda_type_number> u16 =
      MakeIntType(coda_format_binary, coda_native_type_uint16, 16);
  UniqueCodaType<coda_type_number> u32 =
      MakeIntType(coda_format_binary, coda_native_type_uint32, 32);

  UniqueRecordField req_f(coda_type_record_field_new("req"));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              req_f.get(), AsBaseType(u16.get())));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_add_field(bin_union.get(), req_f.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Union rejects fields with bit_offset_expr. Note that upstream
  // coda_type_record_insert_field() stores the field into bin_union->field[]
  // before failing at the bit_offset_expr check, so bin_union owns bad_off_f.
  {
    UniqueCodaType<coda_type_record> bad_union(
        coda_type_union_new(coda_format_binary));
    UniqueRecordField bad_off_f(coda_type_record_field_new("bad_off"));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                                bad_off_f.get(), AsBaseType(u16.get())));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_field_set_optional(bad_off_f.get()));
    ScopedExpr off_expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("0", off_expr.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_bit_offset_expression(
                                bad_off_f.get(), off_expr.get()));
    (void)off_expr.release();
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_type_record_add_field(bad_union.get(), bad_off_f.get()));
    (void)bad_off_f.release();
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  }

  // Add two 16-bit optional fields -> bit_size stays 16.
  UniqueRecordField opt1(coda_type_record_field_new("a"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_field_set_type(opt1.get(), AsBaseType(u16.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(opt1.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(bin_union.get(), opt1.get()));
  (void)opt1.release();
  EXPECT_EQ(16, bin_union->bit_size);

  UniqueRecordField opt2(coda_type_record_field_new("b"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_field_set_type(opt2.get(), AsBaseType(u16.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(opt2.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(bin_union.get(), opt2.get()));
  (void)opt2.release();
  EXPECT_EQ(16, bin_union->bit_size);

  // Binary union without union_field_expr fails validation.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_record_validate(bin_union.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Error paths on coda_type_union_set_field_expression.
  ScopedExpr field_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("0", field_expr.OutParam()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_union_set_field_expression(nullptr, field_expr.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_union_set_field_expression(bin_union.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  UniqueCodaType<coda_type_record> non_union(
      coda_type_record_new(coda_format_binary));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_union_set_field_expression(non_union.get(),
                                                     field_expr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Set field_expr with equal-sized fields -> bit_size remains 16 and
  // validation succeeds.
  EXPECT_EQ(CODA_SUCCESS, coda_type_union_set_field_expression(
                              bin_union.get(), field_expr.get()));
  (void)field_expr.release();
  EXPECT_EQ(16, bin_union->bit_size);
  EXPECT_EQ(CODA_SUCCESS, coda_type_record_validate(bin_union.get()));

  ScopedExpr dup_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("1", dup_expr.OutParam()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_union_set_field_expression(bin_union.get(),
                                                     dup_expr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Adding a 32-bit field resets bit_size to -1; also check
  // union_set_field_expression over differing field sizes.
  UniqueRecordField opt3(coda_type_record_field_new("c"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_field_set_type(opt3.get(), AsBaseType(u32.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(opt3.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(bin_union.get(), opt3.get()));
  (void)opt3.release();
  EXPECT_EQ(-1, bin_union->bit_size);

  UniqueCodaType<coda_type_record> mixed_union(
      coda_type_union_new(coda_format_binary));
  UniqueRecordField m1(coda_type_record_field_new("m1"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_field_set_type(m1.get(), AsBaseType(u16.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(m1.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(mixed_union.get(), m1.get()));
  (void)m1.release();
  UniqueRecordField m2(coda_type_record_field_new("m2"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_field_set_type(m2.get(), AsBaseType(u32.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(m2.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(mixed_union.get(), m2.get()));
  (void)m2.release();
  EXPECT_EQ(CODA_SUCCESS, coda_type_union_set_field_expression(
                              mixed_union.get(), dup_expr.get()));
  (void)dup_expr.release();
  EXPECT_EQ(-1, mixed_union->bit_size);

  // XML union with fields does not require union_field_expr.
  UniqueCodaType<coda_type_record> xml_union(
      coda_type_union_new(coda_format_xml));
  UniqueCodaType<coda_type_text> xml_txt = MakeTextType(coda_format_xml, -1);
  UniqueRecordField xml_opt(coda_type_record_field_new("x"));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                              xml_opt.get(), AsBaseType(xml_txt.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(xml_opt.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_record_add_field(xml_union.get(), xml_opt.get()));
  (void)xml_opt.release();
  EXPECT_EQ(CODA_SUCCESS, coda_type_record_validate(xml_union.get()));
}

TEST_F(CodaTypeTest, ArrayConstructionDimensionsFormatRulesAndGetters) {
  UniqueCodaType<coda_type_array> bin_arr(
      coda_type_array_new(coda_format_binary));
  UniqueCodaType<coda_type_array> xml_arr(coda_type_array_new(coda_format_xml));
  ASSERT_NE(nullptr, bin_arr);
  ASSERT_NE(nullptr, xml_arr);

  coda_native_type rt = coda_native_type_not_available;
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_read_type(AsBaseType(bin_arr.get()), &rt));
  EXPECT_EQ(coda_native_type_bytes, rt);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_read_type(AsBaseType(xml_arr.get()), &rt));
  EXPECT_EQ(coda_native_type_not_available, rt);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_validate(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_validate(bin_arr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_number> ascii_i8 =
      MakeIntType(coda_format_ascii, coda_native_type_int8, 8);
  UniqueCodaType<coda_type_number> bin_u16 =
      MakeIntType(coda_format_binary, coda_native_type_uint16, 16);

  // NULL / format compatibility / XML array-of-array checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_array_set_base_type(nullptr, AsBaseType(bin_u16.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  UniqueCodaType<coda_type_array> ascii_arr(
      coda_type_array_new(coda_format_ascii));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_set_base_type(ascii_arr.get(),
                                              AsBaseType(bin_u16.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_array> netcdf_arr(
      coda_type_array_new(coda_format_netcdf));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_set_base_type(netcdf_arr.get(),
                                              AsBaseType(ascii_i8.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_array> inner_xml_arr(
      coda_type_array_new(coda_format_xml));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_set_base_type(xml_arr.get(),
                                              AsBaseType(inner_xml_arr.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // XML array accepts ASCII base type and NULL variable dim_expr.
  EXPECT_EQ(CODA_SUCCESS, coda_type_array_set_base_type(
                              xml_arr.get(), AsBaseType(ascii_i8.get())));
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_array_add_variable_dimension(xml_arr.get(), nullptr));
  EXPECT_EQ(CODA_SUCCESS, coda_type_array_validate(xml_arr.get()));

  // Fixed dimensions before and after set_base_type update num_elements and
  // bit_size.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_add_fixed_dimension(nullptr, 2));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_add_fixed_dimension(bin_arr.get(), -1));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  EXPECT_EQ(CODA_SUCCESS,
            coda_type_array_add_fixed_dimension(bin_arr.get(), 2));
  EXPECT_EQ(CODA_SUCCESS, coda_type_array_set_base_type(
                              bin_arr.get(), AsBaseType(bin_u16.get())));
  EXPECT_EQ(32, bin_arr->bit_size);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_set_base_type(bin_arr.get(),
                                              AsBaseType(bin_u16.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  EXPECT_EQ(CODA_SUCCESS,
            coda_type_array_add_fixed_dimension(bin_arr.get(), 3));
  EXPECT_EQ(6, bin_arr->num_elements);
  EXPECT_EQ(96, bin_arr->bit_size);

  // Variable dimension with NULL dim_expr rejected on binary.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_add_variable_dimension(nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_add_variable_dimension(bin_arr.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  ScopedExpr dim_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("4", dim_expr.OutParam()));
  EXPECT_EQ(CODA_SUCCESS, coda_type_array_add_variable_dimension(
                              bin_arr.get(), dim_expr.get()));
  (void)dim_expr.release();
  EXPECT_EQ(-1, bin_arr->num_elements);
  EXPECT_EQ(-1, bin_arr->bit_size);

  // Adding remaining dims up to CODA_MAX_NUM_DIMS, then exceeding limit.
  for (int i = 3; i < CODA_MAX_NUM_DIMS; ++i) {
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_add_fixed_dimension(bin_arr.get(), 2));
  }
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_add_fixed_dimension(bin_arr.get(), 2));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  ScopedExpr extra_dim_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("5", extra_dim_expr.OutParam()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_array_add_variable_dimension(bin_arr.get(),
                                                       extra_dim_expr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Array getters & error paths.
  int num_dims = 0;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  coda_type* base_type = nullptr;
  coda_type* arr_base = AsBaseType(bin_arr.get());
  coda_type* non_arr = AsBaseType(bin_u16.get());

  EXPECT_EQ(CODA_SUCCESS, coda_type_get_array_num_dims(arr_base, &num_dims));
  EXPECT_EQ(CODA_MAX_NUM_DIMS, num_dims);
  num_dims = 0;
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_array_dim(arr_base, &num_dims, dims.data()));
  EXPECT_EQ(CODA_MAX_NUM_DIMS, num_dims);
  EXPECT_EQ(2, dims[0]);
  EXPECT_EQ(3, dims[1]);
  EXPECT_EQ(-1, dims[2]);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_array_base_type(arr_base, &base_type));
  EXPECT_EQ(AsBaseType(bin_u16.get()), base_type);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_array_num_dims(nullptr, &num_dims));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_array_num_dims(non_arr, &num_dims));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_array_num_dims(arr_base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_array_dim(nullptr, &num_dims, dims.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_array_dim(non_arr, &num_dims, dims.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_array_dim(arr_base, nullptr, dims.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_array_dim(arr_base, &num_dims, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_array_base_type(nullptr, &base_type));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_array_base_type(non_arr, &base_type));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_array_base_type(arr_base, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

TEST_F(CodaTypeTest, NumberConversionsUnitsAndReadType) {
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_type_number_new(coda_format_binary, coda_text_class));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_number> num(
      coda_type_number_new(coda_format_binary, coda_integer_class));
  ASSERT_NE(nullptr, num);
  ASSERT_EQ(CODA_SUCCESS, coda_type_set_read_type(AsBaseType(num.get()),
                                                  coda_native_type_int16));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_number_set_unit(num.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_number_set_unit(num.get(), "counts"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_number_set_unit(num.get(), "dup"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_number_set_endianness(nullptr, coda_little_endian));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_number_set_endianness(num.get(), coda_little_endian));
  EXPECT_EQ(coda_little_endian, num->endianness);

  UniqueConversion conv(coda_conversion_new(0.1, 1.0, 273.15, -1.0));
  ASSERT_EQ(CODA_SUCCESS, coda_conversion_set_unit(conv.get(), "K"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_number_set_conversion(nullptr, conv.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_number_set_conversion(num.get(), conv.get()));
  (void)conv.release();
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_number_set_conversion(num.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  const int saved_perform_conversions = coda_get_option_perform_conversions();
  const absl::Cleanup restore_conversions = [saved_perform_conversions] {
    coda_set_option_perform_conversions(saved_perform_conversions);
  };

  coda_native_type rt = coda_native_type_not_available;
  const char* unit = nullptr;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_read_type(nullptr, &rt));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_read_type(AsBaseType(num.get()), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_unit(nullptr, &unit));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_unit(AsBaseType(num.get()), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // With conversions enabled, converted integer reports double and conversion
  // unit.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(1));
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_read_type(AsBaseType(num.get()), &rt));
  EXPECT_EQ(coda_native_type_double, rt);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_unit(AsBaseType(num.get()), &unit));
  EXPECT_STREQ("K", unit);

  // Array delegates coda_type_get_unit to its base_type.
  UniqueCodaType<coda_type_array> arr(coda_type_array_new(coda_format_binary));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_unit(AsBaseType(arr.get()), &unit));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_set_base_type(arr.get(), AsBaseType(num.get())));
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_unit(AsBaseType(arr.get()), &unit));
  EXPECT_STREQ("K", unit);

  // With conversions disabled, returns underlying int16 and raw unit.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_read_type(AsBaseType(num.get()), &rt));
  EXPECT_EQ(coda_native_type_int16, rt);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_unit(AsBaseType(num.get()), &unit));
  EXPECT_STREQ("counts", unit);

  // Non-number/non-special/non-array type returns nullptr unit.
  UniqueCodaType<coda_type_raw> raw(coda_type_raw_new(coda_format_binary));
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_unit(AsBaseType(raw.get()), &unit));
  EXPECT_EQ(nullptr, unit);
}

TEST_F(CodaTypeTest, NumberAsciiMappingsAndBitSizeInteraction) {
  UniqueCodaType<coda_type_number> ascii_int(
      coda_type_number_new(coda_format_ascii, coda_integer_class));
  UniqueCodaType<coda_type_number> ascii_real(
      coda_type_number_new(coda_format_ascii, coda_real_class));
  ASSERT_NE(nullptr, ascii_int);
  ASSERT_NE(nullptr, ascii_real);

  UniqueIntMapping imap(coda_ascii_integer_mapping_new("1234", 1234));
  UniqueFloatMapping fmap(coda_ascii_float_mapping_new("NaN", 0.0));

  // NULL and class mismatch checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_number_add_ascii_integer_mapping(nullptr, imap.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_number_add_ascii_integer_mapping(ascii_int.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_number_add_ascii_integer_mapping(ascii_real.get(),
                                                           imap.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_number_add_ascii_float_mapping(nullptr, fmap.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_number_add_ascii_float_mapping(ascii_real.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_number_add_ascii_float_mapping(ascii_int.get(),
                                                         fmap.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Case 1: Set byte size before adding mappings. A mismatched mapping length
  // clears type->bit_size to -1 while default_bit_size remains >= 0, so a
  // subsequent coda_type_set_bit_size fails inside mapping_type_set_bit_size.
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_byte_size(AsBaseType(ascii_int.get()), 4));
  ASSERT_EQ(CODA_SUCCESS, coda_type_number_add_ascii_integer_mapping(
                              ascii_int.get(), imap.get()));
  (void)imap.release();
  EXPECT_EQ(32, ascii_int->bit_size);

  UniqueIntMapping short_map(coda_ascii_integer_mapping_new("99", 99));
  ASSERT_EQ(CODA_SUCCESS, coda_type_number_add_ascii_integer_mapping(
                              ascii_int.get(), short_map.get()));
  (void)short_map.release();
  EXPECT_EQ(-1, ascii_int->bit_size);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_set_bit_size(AsBaseType(ascii_int.get()), 32));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Case 2: Add > BLOCK_SIZE (16) float mappings before setting bit_size, then
  // exercise matching and mismatching bit_size validation.
  ASSERT_EQ(CODA_SUCCESS, coda_type_number_add_ascii_float_mapping(
                              ascii_real.get(), fmap.get()));
  (void)fmap.release();
  for (int i = 0; i < 16; ++i) {
    const std::string key = "F" + std::to_string(10 + i);
    UniqueFloatMapping extra(
        coda_ascii_float_mapping_new(key.c_str(), static_cast<double>(i)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_number_add_ascii_float_mapping(
                                ascii_real.get(), extra.get()));
    (void)extra.release();
  }
  EXPECT_EQ(17, ascii_real->mappings->num_mappings);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_set_byte_size(AsBaseType(ascii_real.get()), 3));
  EXPECT_EQ(24, ascii_real->bit_size);

  UniqueCodaType<coda_type_number> mismatch_real(
      coda_type_number_new(coda_format_ascii, coda_real_class));
  UniqueFloatMapping m2(coda_ascii_float_mapping_new("12345", 1.0));
  ASSERT_EQ(CODA_SUCCESS, coda_type_number_add_ascii_float_mapping(
                              mismatch_real.get(), m2.get()));
  (void)m2.release();
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_set_byte_size(AsBaseType(mismatch_real.get()), 3));
  EXPECT_EQ(-1, mismatch_real->bit_size);
}

TEST_F(CodaTypeTest, NumberValidateAllNativeTypesAndEndianness) {
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_number_validate(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Non-binary numbers validate without bit_size.
  UniqueCodaType<coda_type_number> ascii_num(
      coda_type_number_new(coda_format_ascii, coda_integer_class));
  EXPECT_EQ(CODA_SUCCESS, coda_type_number_validate(ascii_num.get()));

  // Binary number missing both bit_size and size_expr fails; size_expr passes.
  UniqueCodaType<coda_type_number> bin_missing(
      coda_type_number_new(coda_format_binary, coda_integer_class));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_number_validate(bin_missing.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  ScopedExpr sz_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("16", sz_expr.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_type_set_bit_size_expression(
                              AsBaseType(bin_missing.get()), sz_expr.get()));
  (void)sz_expr.release();
  EXPECT_EQ(CODA_SUCCESS, coda_type_number_validate(bin_missing.get()));

  struct NumberValidationCase {
    coda_type_class type_class;
    coda_native_type read_type;
    int64_t bit_size;
    coda_endianness endianness;
    int expected_rc;
  };
  constexpr NumberValidationCase kCases[] = {
      {.type_class = coda_integer_class,
       .read_type = coda_native_type_int8,
       .bit_size = 8,
       .endianness = coda_big_endian,
       .expected_rc = CODA_SUCCESS},
      {.type_class = coda_integer_class,
       .read_type = coda_native_type_uint8,
       .bit_size = 9,
       .endianness = coda_big_endian,
       .expected_rc = -1},
      {.type_class = coda_integer_class,
       .read_type = coda_native_type_int16,
       .bit_size = 16,
       .endianness = coda_little_endian,
       .expected_rc = CODA_SUCCESS},
      {.type_class = coda_integer_class,
       .read_type = coda_native_type_uint16,
       .bit_size = 17,
       .endianness = coda_big_endian,
       .expected_rc = -1},
      {.type_class = coda_integer_class,
       .read_type = coda_native_type_int32,
       .bit_size = 32,
       .endianness = coda_big_endian,
       .expected_rc = CODA_SUCCESS},
      {.type_class = coda_integer_class,
       .read_type = coda_native_type_uint32,
       .bit_size = 33,
       .endianness = coda_big_endian,
       .expected_rc = -1},
      {.type_class = coda_integer_class,
       .read_type = coda_native_type_int64,
       .bit_size = 64,
       .endianness = coda_big_endian,
       .expected_rc = CODA_SUCCESS},
      {.type_class = coda_integer_class,
       .read_type = coda_native_type_uint64,
       .bit_size = 65,
       .endianness = coda_big_endian,
       .expected_rc = -1},
      {.type_class = coda_integer_class,
       .read_type = coda_native_type_int16,
       .bit_size = 12,
       .endianness = coda_little_endian,
       .expected_rc = -1},
      {.type_class = coda_real_class,
       .read_type = coda_native_type_float,
       .bit_size = 32,
       .endianness = coda_big_endian,
       .expected_rc = CODA_SUCCESS},
      {.type_class = coda_real_class,
       .read_type = coda_native_type_float,
       .bit_size = 64,
       .endianness = coda_big_endian,
       .expected_rc = -1},
      {.type_class = coda_real_class,
       .read_type = coda_native_type_double,
       .bit_size = 64,
       .endianness = coda_big_endian,
       .expected_rc = CODA_SUCCESS},
      {.type_class = coda_real_class,
       .read_type = coda_native_type_double,
       .bit_size = 32,
       .endianness = coda_big_endian,
       .expected_rc = -1},
  };

  for (const auto& tc : kCases) {
    UniqueCodaType<coda_type_number> t(
        coda_type_number_new(coda_format_binary, tc.type_class));
    ASSERT_NE(nullptr, t);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(AsBaseType(t.get()), tc.read_type));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_bit_size(AsBaseType(t.get()), tc.bit_size));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_number_set_endianness(t.get(), tc.endianness));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(tc.expected_rc, coda_type_number_validate(t.get()));
    if (tc.expected_rc == -1) {
      EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
    }
  }
}

TEST_F(CodaTypeTest, TextAndRawTypesFixedValuesAndValidation) {
  // Text setters & validation.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_validate(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  UniqueCodaType<coda_type_text> bin_text(
      coda_type_text_new(coda_format_binary));
  UniqueCodaType<coda_type_text> ascii_text(
      coda_type_text_new(coda_format_ascii));
  UniqueCodaType<coda_type_text> xml_text(coda_type_text_new(coda_format_xml));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_set_fixed_value(nullptr, "abc"));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_set_fixed_value(bin_text.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_set_special_text_type(
                    nullptr, ascii_text_line_separator));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_set_special_text_type(
                    bin_text.get(), ascii_text_line_separator));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_type_text_set_special_text_type(
                              ascii_text.get(), ascii_text_line_with_eol));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_set_special_text_type(ascii_text.get(),
                                                     ascii_text_whitespace));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Binary text without size fails validation; non-multiple-of-8 bit_size
  // fails.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_validate(bin_text.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_bit_size(AsBaseType(bin_text.get()), 12));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_validate(bin_text.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // XML text without bit_size validates, unless a fixed_value is set.
  EXPECT_EQ(CODA_SUCCESS, coda_type_text_validate(xml_text.get()));
  EXPECT_EQ(CODA_SUCCESS, coda_type_text_set_fixed_value(xml_text.get(), "AB"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_set_fixed_value(xml_text.get(), "CD"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_validate(xml_text.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Char read_type requires bit_size == 8.
  UniqueCodaType<coda_type_text> char_text(
      coda_type_text_new(coda_format_ascii));
  ASSERT_EQ(CODA_SUCCESS, coda_type_set_read_type(AsBaseType(char_text.get()),
                                                  coda_native_type_char));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_bit_size(AsBaseType(char_text.get()), 16));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_validate(char_text.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_text> valid_char(
      coda_type_text_new(coda_format_ascii));
  ASSERT_EQ(CODA_SUCCESS, coda_type_set_read_type(AsBaseType(valid_char.get()),
                                                  coda_native_type_char));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_bit_size(AsBaseType(valid_char.get()), 8));
  EXPECT_EQ(CODA_SUCCESS, coda_type_text_validate(valid_char.get()));

  // Text fixed_value length mismatch vs match, and getters.
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_byte_size(AsBaseType(ascii_text.get()), 4));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_text_set_fixed_value(ascii_text.get(), "ABC"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_text_validate(ascii_text.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_text> valid_text =
      MakeTextType(coda_format_ascii, 24);
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_text_set_fixed_value(valid_text.get(), "XYZ"));
  EXPECT_EQ(CODA_SUCCESS, coda_type_text_validate(valid_text.get()));

  long str_len = -1;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_string_length(nullptr, &str_len));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_get_string_length(AsBaseType(valid_text.get()), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  UniqueCodaType<coda_type_raw> bin_raw(coda_type_raw_new(coda_format_binary));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_get_string_length(AsBaseType(bin_raw.get()), &str_len));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_type_get_string_length(
                              AsBaseType(valid_text.get()), &str_len));
  EXPECT_EQ(3, str_len);
  UniqueCodaType<coda_type_text> dyn_text(coda_type_text_new(coda_format_xml));
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_string_length(AsBaseType(dyn_text.get()), &str_len));
  EXPECT_EQ(-1, str_len);

  const char* fv = nullptr;
  long fv_len = -1;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_fixed_value(nullptr, &fv, &fv_len));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_fixed_value(AsBaseType(valid_text.get()), nullptr,
                                          &fv_len));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_type_get_fixed_value(AsBaseType(dyn_text.get()),
                                                    &fv, &fv_len));
  EXPECT_EQ(nullptr, fv);
  EXPECT_EQ(0, fv_len);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_fixed_value(
                              AsBaseType(valid_text.get()), &fv, &fv_len));
  EXPECT_STREQ("XYZ", fv);
  EXPECT_EQ(3, fv_len);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_fixed_value(
                              AsBaseType(valid_text.get()), &fv, nullptr));
  EXPECT_STREQ("XYZ", fv);

  // Raw setters, fixed values (including embedded NUL), and validation.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_raw_validate(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_raw_set_fixed_value(nullptr, 1, "a"));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_raw_set_fixed_value(bin_raw.get(), 2, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_raw_validate(bin_raw.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // length == 0 sets fixed_value_length = 0 and rejects a subsequent call.
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_raw_set_fixed_value(bin_raw.get(), 0, nullptr));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_raw_set_fixed_value(bin_raw.get(), 1, "a"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Raw fixed_value with embedded NUL and non-byte-aligned bit_size (10 bits ->
  // rounded byte size 2).
  UniqueCodaType<coda_type_raw> fixed_raw(
      coda_type_raw_new(coda_format_binary));
  constexpr std::array<char, 2> kRawBytes = {'\x01', '\x00'};
  ASSERT_EQ(CODA_SUCCESS, coda_type_raw_set_fixed_value(fixed_raw.get(), 2,
                                                        kRawBytes.data()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_raw_validate(fixed_raw.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_bit_size(AsBaseType(fixed_raw.get()), 8));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_raw_validate(fixed_raw.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_raw> aligned_raw(
      coda_type_raw_new(coda_format_binary));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_bit_size(AsBaseType(aligned_raw.get()), 10));
  ASSERT_EQ(CODA_SUCCESS, coda_type_raw_set_fixed_value(aligned_raw.get(), 2,
                                                        kRawBytes.data()));
  EXPECT_EQ(CODA_SUCCESS, coda_type_raw_validate(aligned_raw.get()));
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_fixed_value(
                              AsBaseType(aligned_raw.get()), &fv, &fv_len));
  ASSERT_NE(nullptr, fv);
  EXPECT_EQ(2, fv_len);
  EXPECT_EQ(0, std::memcmp(fv, kRawBytes.data(), 2));

  // Non-text/non-raw returns nullptr for fixed_value; XML raw without bit_size
  // passes validation.
  UniqueCodaType<coda_type_raw> xml_raw(coda_type_raw_new(coda_format_xml));
  EXPECT_EQ(CODA_SUCCESS, coda_type_raw_validate(xml_raw.get()));
  UniqueCodaType<coda_type_record> rec(
      coda_type_record_new(coda_format_binary));
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_fixed_value(AsBaseType(rec.get()), &fv, &fv_len));
  EXPECT_EQ(nullptr, fv);
}

TEST_F(CodaTypeTest, SingletonsAndTypeDone) {
  coda_type_raw* raw1 = coda_type_raw_file_singleton();
  coda_type_raw* raw2 = coda_type_raw_file_singleton();
  ASSERT_NE(nullptr, raw1);
  EXPECT_EQ(raw1, raw2);

  coda_format format = coda_format_ascii;
  coda_type_class type_class = coda_record_class;
  coda_native_type read_type = coda_native_type_not_available;
  int64_t bit_size = 0;
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_format(AsBaseType(raw1), &format));
  EXPECT_EQ(coda_format_binary, format);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_class(AsBaseType(raw1), &type_class));
  EXPECT_EQ(coda_raw_class, type_class);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_read_type(AsBaseType(raw1), &read_type));
  EXPECT_EQ(coda_native_type_bytes, read_type);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_bit_size(AsBaseType(raw1), &bit_size));
  EXPECT_EQ(-1, bit_size);

  coda_type_record* er1 = coda_type_empty_record(coda_format_xml);
  coda_type_record* er2 = coda_type_empty_record(coda_format_xml);
  ASSERT_NE(nullptr, er1);
  EXPECT_EQ(er1, er2);
  long num_fields = -1;
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_num_record_fields(AsBaseType(er1), &num_fields));
  EXPECT_EQ(0, num_fields);

  coda_type_special* nd1 = coda_type_no_data_singleton(coda_format_binary);
  coda_type_special* nd2 = coda_type_no_data_singleton(coda_format_binary);
  ASSERT_NE(nullptr, nd1);
  EXPECT_EQ(nd1, nd2);

  coda_special_type st = coda_special_time;
  coda_type* base = nullptr;
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_special_type(AsBaseType(nd1), &st));
  EXPECT_EQ(coda_special_no_data, st);
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_special_base_type(AsBaseType(nd1), &base));
  ASSERT_NE(nullptr, base);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_class(base, &type_class));
  EXPECT_EQ(coda_raw_class, type_class);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_bit_size(base, &bit_size));
  EXPECT_EQ(0, bit_size);

  coda_type_done();
  coda_type_done();
}

TEST_F(CodaTypeTest, SpecialTypesVsfIntegerTimeAndComplex) {
  // 1. VSF Integer.
  UniqueCodaType<coda_type_special> vsf(
      coda_type_vsf_integer_new(coda_format_binary));
  ASSERT_NE(nullptr, vsf);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_vsf_integer_validate(vsf.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_number> ascii_i8 =
      MakeIntType(coda_format_ascii, coda_native_type_int8, 8);
  UniqueCodaType<coda_type_number> bin_i32 =
      MakeIntType(coda_format_binary, coda_native_type_int32, 32);
  UniqueCodaType<coda_type_number> bin_u32 =
      MakeIntType(coda_format_binary, coda_native_type_uint32, 32);
  UniqueCodaType<coda_type_number> bin_i8 =
      MakeIntType(coda_format_binary, coda_native_type_int8, 8);

  // Value cannot be set before scale factor; format mismatch and uint32 scale
  // factor are rejected.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_vsf_integer_set_type(vsf.get(), AsBaseType(bin_i32.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_vsf_integer_set_scale_factor(
                    vsf.get(), AsBaseType(ascii_i8.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_vsf_integer_set_scale_factor(
                    vsf.get(), AsBaseType(bin_u32.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_type_vsf_integer_set_scale_factor(
                              vsf.get(), AsBaseType(bin_i8.get())));
  for (coda_native_type sf_rt :
       {coda_native_type_uint8, coda_native_type_int16, coda_native_type_uint16,
        coda_native_type_int32}) {
    UniqueCodaType<coda_type_special> vsf_tmp(
        coda_type_vsf_integer_new(coda_format_binary));
    UniqueCodaType<coda_type_number> sf_num =
        MakeIntType(coda_format_binary, sf_rt, 16);
    EXPECT_EQ(CODA_SUCCESS, coda_type_vsf_integer_set_scale_factor(
                                vsf_tmp.get(), AsBaseType(sf_num.get())));
  }
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_vsf_integer_set_scale_factor(
                    vsf.get(), AsBaseType(bin_i8.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_vsf_integer_set_type(vsf.get(),
                                               AsBaseType(ascii_i8.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_type_vsf_integer_set_type(
                              vsf.get(), AsBaseType(bin_i32.get())));
  EXPECT_EQ(40, vsf->bit_size);
  EXPECT_EQ(CODA_SUCCESS, coda_type_vsf_integer_validate(vsf.get()));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_vsf_integer_set_unit(vsf.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_type_vsf_integer_set_unit(vsf.get(), "Pa"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_vsf_integer_set_unit(vsf.get(), "bar"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  const char* unit = nullptr;
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_unit(AsBaseType(vsf.get()), &unit));
  EXPECT_STREQ("Pa", unit);

  // 2. Time special type.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_type_time_new(coda_format_ascii, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  ScopedExpr time_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("float(.)", time_expr.OutParam()));
  UniqueCodaType<coda_type_special> time_type(
      coda_type_time_new(coda_format_ascii, time_expr.get()));
  ASSERT_NE(nullptr, time_type);
  (void)time_expr.release();

  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_unit(AsBaseType(time_type.get()), &unit));
  EXPECT_STREQ("s since 2000-01-01", unit);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_time_validate(time_type.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueFloatMapping tm(coda_ascii_float_mapping_new("9999-99-99", 0.0));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_time_add_ascii_float_mapping(nullptr, tm.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_time_add_ascii_float_mapping(time_type.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_time_add_ascii_float_mapping(vsf.get(), tm.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_time_add_ascii_float_mapping(time_type.get(), tm.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Non-text base type on time rejects float mapping.
  ScopedExpr time_expr2;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("0.0", time_expr2.OutParam()));
  UniqueCodaType<coda_type_special> num_time(
      coda_type_time_new(coda_format_binary, time_expr2.get()));
  (void)time_expr2.release();
  UniqueCodaType<coda_type_number> bin_f64 =
      MakeRealType(coda_format_binary, coda_native_type_double, 64);
  ASSERT_EQ(CODA_SUCCESS, coda_type_time_set_base_type(
                              num_time.get(), AsBaseType(bin_f64.get())));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_time_add_ascii_float_mapping(num_time.get(), tm.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // set_base_type error paths and valid text base_type with empty + non-empty
  // mappings.
  UniqueCodaType<coda_type_text> ascii_txt =
      MakeTextType(coda_format_ascii, 80);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_type_time_set_base_type(nullptr, AsBaseType(ascii_txt.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_time_set_base_type(time_type.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_time_set_base_type(vsf.get(), AsBaseType(ascii_txt.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_type_time_set_base_type(
                              time_type.get(), AsBaseType(ascii_txt.get())));
  EXPECT_EQ(80, time_type->bit_size);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_time_set_base_type(time_type.get(),
                                             AsBaseType(ascii_txt.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Success path: coda_type_time_add_ascii_float_mapping deletes `tm` /
  // `empty_tm` on return 0.
  int map_rc =
      coda_type_time_add_ascii_float_mapping(time_type.get(), tm.get());
  if (map_rc == CODA_SUCCESS) {
    (void)tm.release();
  }
  ASSERT_EQ(CODA_SUCCESS, map_rc);

  UniqueFloatMapping empty_tm(coda_ascii_float_mapping_new("", -1.0));
  map_rc =
      coda_type_time_add_ascii_float_mapping(time_type.get(), empty_tm.get());
  if (map_rc == CODA_SUCCESS) {
    (void)empty_tm.release();
  }
  ASSERT_EQ(CODA_SUCCESS, map_rc);
  EXPECT_EQ(CODA_SUCCESS, coda_type_time_validate(time_type.get()));

  // 3. Complex special type.
  UniqueCodaType<coda_type_special> complex(
      coda_type_complex_new(coda_format_binary));
  ASSERT_NE(nullptr, complex);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_complex_validate(complex.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_complex_set_type(complex.get(),
                                           AsBaseType(ascii_txt.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_number> ascii_f64 =
      MakeRealType(coda_format_ascii, coda_native_type_double, 64);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_complex_set_type(complex.get(),
                                           AsBaseType(ascii_f64.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_type_complex_set_type(
                              complex.get(), AsBaseType(bin_f64.get())));
  EXPECT_EQ(128, complex->bit_size);
  EXPECT_EQ(CODA_SUCCESS, coda_type_complex_validate(complex.get()));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_complex_set_type(complex.get(), AsBaseType(bin_f64.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  UniqueCodaType<coda_type_special> int_complex(
      coda_type_complex_new(coda_format_binary));
  EXPECT_EQ(CODA_SUCCESS, coda_type_complex_set_type(
                              int_complex.get(), AsBaseType(bin_i32.get())));
  EXPECT_EQ(64, int_complex->bit_size);
  EXPECT_EQ(CODA_SUCCESS, coda_type_complex_validate(int_complex.get()));

  // Special type getters & error paths.
  coda_special_type st = coda_special_no_data;
  coda_type* base_type = nullptr;
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_special_type(AsBaseType(complex.get()), &st));
  EXPECT_EQ(coda_special_complex, st);
  EXPECT_EQ(CODA_SUCCESS, coda_type_get_special_base_type(
                              AsBaseType(complex.get()), &base_type));
  ASSERT_NE(nullptr, base_type);
  long num_complex_fields = 0;
  EXPECT_EQ(CODA_SUCCESS,
            coda_type_get_num_record_fields(base_type, &num_complex_fields));
  EXPECT_EQ(2, num_complex_fields);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_special_type(nullptr, &st));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_special_type(AsBaseType(bin_f64.get()), &st));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_special_type(AsBaseType(complex.get()), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_special_base_type(nullptr, &base_type));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_type_get_special_base_type(AsBaseType(bin_f64.get()),
                                                &base_type));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_type_get_special_base_type(AsBaseType(complex.get()), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

// TODO(schwehr): Remaining uncovered lines in libcoda/coda-type.c are OOM
// checks, internal static *_delete(NULL) guards, assert(0) in
// coda_type_number_validate, and the defensive bit_offset_expr check in
// coda_type_union_set_field_expression.

}  // namespace
}  // namespace stcorp_coda
