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
// Tests CODA GRIB dynamic value array type constructors, scale-factor /
// bitmask-cumsum128 helpers, destructor cleanup, and full GRIB1 & GRIB2 type
// hierarchy traversal (libcoda/coda-grib-type.c).
//
// google3 build caveats:
//   - Internal headers lack extern "C" guards and require
//   coda_internal_prelude.h
//     before coda-grib-internal.h.
//   - Upstream coda-grib.c:2342-2358 allocates `raw_data` for GRIB2 Section 2
//     (Local Use) with section_size > 5, copies it via coda_mem_raw_new(), and
//     never frees `raw_data`; coda_open() on such files is wrapped in an
//     unconditional absl::LeakCheckDisabler.
//   - Upstream coda-grib.c:1540 (`sign << 31` in ibmfloat_to_iee754) triggers
//     UBSan signed shift overflow on negative IBM floats, so GRIB1 file-backed
//     messages use non-negative IBM float reference values.
//   - Verified 100.0% reachable coverage ceiling on coda-grib-type.c
//     (97/110 = 88.2%, 13 missed): all 13 uncovered lines are malloc() OOM
//     branches (105, 107, 128-129, 131, 171-174, 181-184). In
//     coda_grib_type_delete(), line 64 also dereferences type->definition
//     before the defensive NULL check on line 79.
//   - In coda-grib.c, GRIB2 product template 40 reads 25 bytes into a stack
//     buffer and reads buffer[25..26] if typeOfSecondFixedSurface != 255; tests
//     using template 40 keep type_of_second_fixed_surface == 255.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib-type.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-grib-cursor.c

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/grib_builder.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-grib-internal.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

template <typename T>
struct CodaTypeReleaser {
  void operator()(T* type) const {
    coda_type_release(reinterpret_cast<coda_type*>(type));
  }
};

template <typename T>
using UniqueCodaType = std::unique_ptr<T, CodaTypeReleaser<T>>;

template <typename T>
struct GribTypeDeleter {
  void operator()(T* type) const {
    coda_grib_type_delete(reinterpret_cast<coda_dynamic_type*>(type));
  }
};

template <typename T>
using UniqueGribType = std::unique_ptr<T, GribTypeDeleter<T>>;

constexpr coda_endianness kNativeEndianness =
    std::endian::native == std::endian::big ? coda_big_endian
                                            : coda_little_endian;

// Isolates CODA_DEFINITION, CODA_LEAP_SECOND_TABLE, and runtime options before
// coda_init() so ambient environment state cannot affect tests.
class ScopedIsolatedCodaSession {
 public:
  ScopedIsolatedCodaSession()
      : env_def_("CODA_DEFINITION"), env_leap_("CODA_LEAP_SECOND_TABLE") {
    env_def_.Unset();
    env_leap_.Unset();
    coda_set_definition_path(nullptr);
  }

  ~ScopedIsolatedCodaSession() { coda_set_definition_path(nullptr); }

  int Init() { return session_.Init(); }

 private:
  ScopedCodaOptions options_;
  ScopedEnvVar env_def_;
  ScopedEnvVar env_leap_;
  ScopedCodaSession session_;
};

// White-box helper reading the top dynamic type from `cursor` (unmangled via
// CODA_INTERNAL). Used to assert internal coda_grib_value_array fields
// directly constructed by libcoda/coda-grib-type.c.
const coda_dynamic_type* CursorTopDynamicType(const coda_cursor& cursor) {
  return cursor.stack[cursor.n - 1].type;
}

// Builds a GRIB-format array definition whose base_type is `base_type`.
UniqueCodaType<coda_type_array> MakeGribArrayDef(coda_type* base_type) {
  UniqueCodaType<coda_type_array> arr_def(
      coda_type_array_new(coda_format_grib));
  if (arr_def == nullptr) {
    return nullptr;
  }
  if (coda_type_array_set_base_type(arr_def.get(), base_type) != 0) {
    return nullptr;
  }
  if (coda_type_array_add_variable_dimension(arr_def.get(), nullptr) != 0) {
    return nullptr;
  }
  return arr_def;
}

UniqueCodaType<coda_type_number> MakeGribRealDef(
    coda_native_type read_type = coda_native_type_float,
    int64_t bit_size = 32) {
  UniqueCodaType<coda_type_number> real_def(
      coda_type_number_new(coda_format_grib, coda_real_class));
  if (real_def == nullptr) {
    return nullptr;
  }
  if (coda_type_set_read_type(reinterpret_cast<coda_type*>(real_def.get()),
                              read_type) != 0) {
    return nullptr;
  }
  if (coda_type_set_bit_size(reinterpret_cast<coda_type*>(real_def.get()),
                             bit_size) != 0) {
    return nullptr;
  }
  if (coda_type_number_set_endianness(real_def.get(), kNativeEndianness) != 0) {
    return nullptr;
  }
  return real_def;
}

UniqueCodaType<coda_type> MakeNonRealGribType(coda_type_class type_class) {
  switch (type_class) {
    case coda_integer_class:
      return UniqueCodaType<coda_type>(reinterpret_cast<coda_type*>(
          coda_type_number_new(coda_format_grib, coda_integer_class)));
    case coda_text_class:
      return UniqueCodaType<coda_type>(
          reinterpret_cast<coda_type*>(coda_type_text_new(coda_format_grib)));
    case coda_raw_class:
      return UniqueCodaType<coda_type>(
          reinterpret_cast<coda_type*>(coda_type_raw_new(coda_format_grib)));
    case coda_record_class:
      return UniqueCodaType<coda_type>(
          reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_grib)));
    default:
      return nullptr;
  }
}

struct TreeSummary {
  std::vector<std::string> visited_paths;
  int record_count = 0;
  int union_count = 0;
  int array_count = 0;
  int grib_value_array_count = 0;
  int integer_count = 0;
  int real_count = 0;
  int raw_count = 0;
  int hidden_field_count = 0;
};

void VerifyEmptyAttributes(coda_cursor* cursor) {
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(cursor));
  const absl::Cleanup pop_attrs = [cursor] { coda_cursor_goto_parent(cursor); };

  coda_type* attr_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(cursor, &attr_type));
  ASSERT_NE(nullptr, attr_type);
  coda_type_class attr_class = coda_integer_class;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(attr_type, &attr_class));
  EXPECT_EQ(coda_record_class, attr_class);

  long num_attr_fields = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_num_record_fields(attr_type, &num_attr_fields));
  EXPECT_EQ(0, num_attr_fields);
}

void TraverseGribTree(coda_cursor* cursor, const std::string& path,
                      TreeSummary& summary) {
  summary.visited_paths.push_back(path);

  coda_type* type = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(cursor, &type));
  ASSERT_NE(nullptr, type);

  coda_format format = coda_format_binary;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_format(type, &format));
  EXPECT_EQ(coda_format_grib, format);

  coda_type_class type_class = coda_special_class;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(type, &type_class));

  VerifyEmptyAttributes(cursor);

  switch (type_class) {
    case coda_record_class: {
      summary.record_count++;
      int is_union = -1;
      ASSERT_EQ(CODA_SUCCESS,
                coda_type_get_record_union_status(type, &is_union));
      if (is_union != 0) {
        summary.union_count++;
        long active_idx = -1;
        ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_available_union_field_index(
                                    cursor, &active_idx));
        const char* field_name = nullptr;
        ASSERT_EQ(CODA_SUCCESS, coda_type_get_record_field_name(
                                    type, active_idx, &field_name));
        ASSERT_NE(nullptr, field_name);
        ASSERT_EQ(CODA_SUCCESS,
                  coda_cursor_goto_record_field_by_index(cursor, active_idx));
        TraverseGribTree(cursor, absl::StrCat(path, "/", field_name), summary);
        ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(cursor));
      } else {
        long num_fields = -1;
        ASSERT_EQ(CODA_SUCCESS,
                  coda_type_get_num_record_fields(type, &num_fields));
        for (long i = 0; i < num_fields; ++i) {
          const char* field_name = nullptr;
          ASSERT_EQ(CODA_SUCCESS,
                    coda_type_get_record_field_name(type, i, &field_name));
          ASSERT_NE(nullptr, field_name);
          int hidden = -1;
          ASSERT_EQ(CODA_SUCCESS,
                    coda_type_get_record_field_hidden_status(type, i, &hidden));
          if (hidden != 0) {
            summary.hidden_field_count++;
          }
          int available = -1;
          ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                                      cursor, i, &available));
          if (available == 1) {
            ASSERT_EQ(CODA_SUCCESS,
                      coda_cursor_goto_record_field_by_index(cursor, i));
            TraverseGribTree(cursor, absl::StrCat(path, "/", field_name),
                             summary);
            ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(cursor));
          }
        }
      }
      break;
    }
    case coda_array_class: {
      summary.array_count++;
      const coda_dynamic_type* dyn_type = CursorTopDynamicType(*cursor);
      ASSERT_NE(nullptr, dyn_type);
      if (dyn_type->backend == coda_backend_grib) {
        summary.grib_value_array_count++;
      }

      long num_elements = -1;
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_get_num_elements(cursor, &num_elements));
      int num_dims = -1;
      long dims[CODA_MAX_NUM_DIMS] = {};
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_get_array_dim(cursor, &num_dims, dims));
      EXPECT_EQ(1, num_dims);
      EXPECT_EQ(num_elements, dims[0]);

      if (num_elements > 0) {
        ASSERT_EQ(CODA_SUCCESS,
                  coda_cursor_goto_array_element_by_index(cursor, 0));
        for (long i = 0; i < num_elements; ++i) {
          // For large GRIB value arrays, recurse into boundary elements to keep
          // path vectors concise while stepping with goto_next_array_element.
          if (dyn_type->backend != coda_backend_grib || i < 4 ||
              i + 2 >= num_elements) {
            TraverseGribTree(cursor, absl::StrCat(path, "[", i, "]"), summary);
          } else {
            summary.real_count++;
          }
          if (i + 1 < num_elements) {
            ASSERT_EQ(CODA_SUCCESS,
                      coda_cursor_goto_next_array_element(cursor));
          }
        }
        ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(cursor));
      }
      break;
    }
    case coda_integer_class: {
      summary.integer_count++;
      coda_native_type read_type = coda_native_type_not_available;
      ASSERT_EQ(CODA_SUCCESS, coda_type_get_read_type(type, &read_type));
      switch (read_type) {
        case coda_native_type_uint8: {
          uint8_t v = 0;
          ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(cursor, &v));
          break;
        }
        case coda_native_type_int16: {
          int16_t v = 0;
          ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(cursor, &v));
          break;
        }
        case coda_native_type_uint16: {
          uint16_t v = 0;
          ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(cursor, &v));
          break;
        }
        case coda_native_type_int32: {
          int32_t v = 0;
          ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(cursor, &v));
          break;
        }
        case coda_native_type_uint32: {
          uint32_t v = 0;
          ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(cursor, &v));
          break;
        }
        default:
          ADD_FAILURE() << "Unexpected integer read_type " << read_type
                        << " at " << path;
          break;
      }
      break;
    }
    case coda_real_class: {
      summary.real_count++;
      coda_native_type read_type = coda_native_type_not_available;
      ASSERT_EQ(CODA_SUCCESS, coda_type_get_read_type(type, &read_type));
      if (read_type == coda_native_type_float) {
        float v = 0.0f;
        ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(cursor, &v));
      } else {
        ASSERT_EQ(coda_native_type_double, read_type);
        double v = 0.0;
        ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(cursor, &v));
      }
      break;
    }
    case coda_raw_class: {
      summary.raw_count++;
      int64_t byte_size = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(cursor, &byte_size));
      ASSERT_GE(byte_size, 0);
      if (byte_size > 0) {
        std::vector<uint8_t> raw(static_cast<size_t>(byte_size));
        ASSERT_EQ(CODA_SUCCESS,
                  coda_cursor_read_bytes(cursor, raw.data(), 0, byte_size));
      }
      break;
    }
    default:
      ADD_FAILURE() << "Unexpected GRIB type class " << type_class << " at "
                    << path;
      break;
  }
}

TEST(CodaGribTypeTest, MissingAndCorruptGribFilesFail) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Missing file fails with CODA_ERROR_FILE_NOT_FOUND.
  const TempFile missing("grib_type_missing.grb");
  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  // 2. Truncated GRIB1 file fails with CODA_ERROR_OUT_OF_BOUNDS_READ.
  const TempFile truncated("grib_type_truncated.grb");
  constexpr auto kTruncatedGrib1 = std::to_array<uint8_t>(
      {'G', 'R', 'I', 'B', 0x00, 0x00, 0x20, 0x01, 0x00, 0x00});
  ASSERT_TRUE(truncated.WriteBytes(kTruncatedGrib1));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(truncated.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  // 3. GRIB1 message with invalid '7777' trailer fails with
  // CODA_ERROR_FILE_READ.
  const TempFile bad_trailer("grib_type_bad_trailer.grb");
  Grib1MessageSpec bad_spec;
  bad_spec.end_marker = {'B', 'A', 'D', '!'};
  ASSERT_TRUE(bad_trailer.WriteBytes(GribBuilder::BuildGrib1(bad_spec)));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(bad_trailer.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_READ, coda_errno);
  EXPECT_EQ(nullptr, product.get());
}

TEST(CodaGribTypeTest, ValueArrayNewRejectsNullAndNonRealBaseTypes) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. NULL definition returns CODA_ERROR_INVALID_ARGUMENT.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_grib_value_array_new(nullptr, 4, 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("definition argument is NULL"));

  // 2. Non-real base_type classes return CODA_ERROR_DATA_DEFINITION.
  struct BaseClassCase {
    coda_type_class type_class;
    std::string_view expected_class_name;
  };
  constexpr BaseClassCase kInvalidBaseClasses[] = {
      {.type_class = coda_integer_class, .expected_class_name = "integer"},
      {.type_class = coda_text_class, .expected_class_name = "text"},
      {.type_class = coda_raw_class, .expected_class_name = "raw"},
      {.type_class = coda_record_class, .expected_class_name = "record"},
  };

  for (const auto& tc : kInvalidBaseClasses) {
    UniqueCodaType<coda_type> base_def = MakeNonRealGribType(tc.type_class);
    ASSERT_NE(nullptr, base_def.get());
    UniqueCodaType<coda_type_array> arr_def = MakeGribArrayDef(base_def.get());
    ASSERT_NE(nullptr, arr_def.get());

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(nullptr, coda_grib_value_array_new(arr_def.get(), 4, 0));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno)
        << "class=" << tc.expected_class_name;
  }
}

TEST(CodaGribTypeTest, ValueArrayNewInitializesFieldsAndRetainCounts) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  UniqueCodaType<coda_type_number> real_def = MakeGribRealDef();
  ASSERT_NE(nullptr, real_def.get());
  UniqueCodaType<coda_type_array> arr_def =
      MakeGribArrayDef(reinterpret_cast<coda_type*>(real_def.get()));
  ASSERT_NE(nullptr, arr_def.get());

  EXPECT_EQ(0, arr_def->retain_count);
  EXPECT_EQ(1, real_def->retain_count);

  // 1. Large 64-bit byte_offset verifies bit_offset = 8 * byte_offset without
  //    32-bit truncation.
  constexpr int64_t kLargeByteOffset = int64_t{12345678901LL};
  {
    UniqueGribType<coda_grib_value_array> arr(
        coda_grib_value_array_new(arr_def.get(), 5, kLargeByteOffset));
    ASSERT_NE(nullptr, arr.get());

    EXPECT_EQ(coda_backend_grib, arr->backend);
    EXPECT_EQ(arr_def.get(), arr->definition);
    EXPECT_EQ(1, arr_def->retain_count);
    EXPECT_EQ(5, arr->num_elements);
    EXPECT_EQ(8 * kLargeByteOffset, arr->bit_offset);
    EXPECT_EQ(0, arr->simple_packing);
    EXPECT_EQ(32, arr->element_bit_size);
    EXPECT_EQ(0, arr->decimalScaleFactor);
    EXPECT_EQ(0, arr->binaryScaleFactor);
    EXPECT_EQ(0.0f, arr->referenceValue);
    EXPECT_EQ(0.0, arr->scalefactor);
    EXPECT_EQ(0.0, arr->offset);
    EXPECT_EQ(nullptr, arr->bitmask);
    EXPECT_EQ(nullptr, arr->bitmask_cumsum128);

    ASSERT_NE(nullptr, arr->base_type);
    EXPECT_EQ(coda_backend_grib, arr->base_type->backend);
    EXPECT_EQ(reinterpret_cast<coda_type*>(real_def.get()),
              arr->base_type->definition);
    EXPECT_EQ(2, real_def->retain_count);
  }

  // 2. Deleting via coda_grib_type_delete restores definition retain counts.
  EXPECT_EQ(0, arr_def->retain_count);
  EXPECT_EQ(1, real_def->retain_count);

  // 3. Zero-element array with zero byte offset succeeds cleanly.
  {
    UniqueGribType<coda_grib_value_array> empty_arr(
        coda_grib_value_array_new(arr_def.get(), 0, 0));
    ASSERT_NE(nullptr, empty_arr.get());
    EXPECT_EQ(0, empty_arr->num_elements);
    EXPECT_EQ(0, empty_arr->bit_offset);
    EXPECT_EQ(0, empty_arr->simple_packing);
  }
  EXPECT_EQ(0, arr_def->retain_count);
  EXPECT_EQ(1, real_def->retain_count);
}

TEST(CodaGribTypeTest, SimplePackingNewErrorPropagationAndScaleFactors) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Propagate NULL definition error from coda_grib_value_array_new().
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_grib_value_array_simple_packing_new(
                         nullptr, 4, 0, 8, 0, 0, 0.0f, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 2. Propagate non-real base_type error from coda_grib_value_array_new().
  {
    UniqueCodaType<coda_type> int_def = MakeNonRealGribType(coda_integer_class);
    ASSERT_NE(nullptr, int_def.get());
    UniqueCodaType<coda_type_array> int_arr_def =
        MakeGribArrayDef(int_def.get());
    ASSERT_NE(nullptr, int_arr_def.get());

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(nullptr, coda_grib_value_array_simple_packing_new(
                           int_arr_def.get(), 4, 0, 8, 0, 0, 0.0f, nullptr));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  }

  // 3. Exercise fpow() across positive, zero, and negative exponents for both
  //    binaryScaleFactor and decimalScaleFactor, plus 0-bit packing.
  UniqueCodaType<coda_type_number> real_def = MakeGribRealDef();
  ASSERT_NE(nullptr, real_def.get());
  UniqueCodaType<coda_type_array> arr_def =
      MakeGribArrayDef(reinterpret_cast<coda_type*>(real_def.get()));
  ASSERT_NE(nullptr, arr_def.get());

  struct ScaleCase {
    int element_bit_size;
    int16_t decimal_scale;
    int16_t binary_scale;
    float reference_value;
    double expected_scalefactor;
    double expected_offset;
  };
  constexpr ScaleCase kScaleCases[] = {
      {.element_bit_size = 0,
       .decimal_scale = 0,
       .binary_scale = 0,
       .reference_value = 1.5f,
       .expected_scalefactor = 1.0,
       .expected_offset = 1.5},
      {.element_bit_size = 8,
       .decimal_scale = 2,
       .binary_scale = -3,
       .reference_value = 250.0f,
       .expected_scalefactor = 0.00125,
       .expected_offset = 2.5},
      {.element_bit_size = 12,
       .decimal_scale = -2,
       .binary_scale = 3,
       .reference_value = -4.0f,
       .expected_scalefactor = 800.0,
       .expected_offset = -400.0},
      {.element_bit_size = 16,
       .decimal_scale = 1,
       .binary_scale = 0,
       .reference_value = 0.0f,
       .expected_scalefactor = 0.1,
       .expected_offset = 0.0},
      {.element_bit_size = 24,
       .decimal_scale = 0,
       .binary_scale = 4,
       .reference_value = 3.25f,
       .expected_scalefactor = 16.0,
       .expected_offset = 3.25},
  };

  for (const auto& tc : kScaleCases) {
    UniqueGribType<coda_grib_value_array> arr(
        coda_grib_value_array_simple_packing_new(
            arr_def.get(), 10, 64, tc.element_bit_size, tc.decimal_scale,
            tc.binary_scale, tc.reference_value, nullptr));
    ASSERT_NE(nullptr, arr.get());
    EXPECT_EQ(1, arr->simple_packing);
    EXPECT_EQ(tc.element_bit_size, arr->element_bit_size);
    EXPECT_EQ(tc.decimal_scale, arr->decimalScaleFactor);
    EXPECT_EQ(tc.binary_scale, arr->binaryScaleFactor);
    EXPECT_FLOAT_EQ(tc.reference_value, arr->referenceValue);
    EXPECT_NEAR(tc.expected_scalefactor, arr->scalefactor, 1e-12);
    EXPECT_NEAR(tc.expected_offset, arr->offset, 1e-12);
    EXPECT_EQ(nullptr, arr->bitmask);
    EXPECT_EQ(nullptr, arr->bitmask_cumsum128);
  }
}

TEST(CodaGribTypeTest, SimplePackingNewComputesBitmaskAndCumsum128) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  UniqueCodaType<coda_type_number> real_def = MakeGribRealDef();
  ASSERT_NE(nullptr, real_def.get());
  UniqueCodaType<coda_type_array> arr_def =
      MakeGribArrayDef(reinterpret_cast<coda_type*>(real_def.get()));
  ASSERT_NE(nullptr, arr_def.get());

  // 1. Single-byte bitmask (num_elements = 5 -> bitmask_size = 1).
  //    Exercises i == 0 where i % 16 == 0.
  {
    constexpr std::array<uint8_t, 1> kSingleByteMask = {0xA8};  // 10101000
    UniqueGribType<coda_grib_value_array> arr(
        coda_grib_value_array_simple_packing_new(arr_def.get(), 5, 16, 8, 0, 0,
                                                 0.0f, kSingleByteMask.data()));
    ASSERT_NE(nullptr, arr.get());
    ASSERT_NE(nullptr, arr->bitmask);
    ASSERT_NE(nullptr, arr->bitmask_cumsum128);
    EXPECT_EQ(0xA8, arr->bitmask[0]);
    EXPECT_EQ(3, arr->bitmask_cumsum128[0]);
  }

  // 2. Multi-block bitmask crossing two 16-byte (128-bit) boundaries:
  //    num_elements = 260 -> bit_size_to_byte_size(260) = 33 bytes.
  //    - Block 0 (bytes 0..15): varied bit patterns testing every bit position.
  //    - Block 1 (bytes 16..31): all 0xFF bytes -> cumsum128[31] == 128.
  //    - Block 2 (byte 32): resets at i % 16 == 0 to popcount(mask[32]).
  std::array<uint8_t, 33> mask = {};
  constexpr std::array<uint8_t, 16> kBlock0 = {
      0x00, 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02,
      0x01, 0xAA, 0x55, 0xF0, 0x0F, 0xC3, 0x3C, 0xFF,
  };
  for (size_t i = 0; i < 16; ++i) {
    mask[i] = kBlock0[i];
  }
  for (size_t i = 16; i < 32; ++i) {
    mask[i] = 0xFF;
  }
  mask[32] = 0xA0;  // 2 bits set in final byte.

  UniqueGribType<coda_grib_value_array> arr(
      coda_grib_value_array_simple_packing_new(arr_def.get(), 260, 128, 12, 0,
                                               0, 0.0f, mask.data()));
  ASSERT_NE(nullptr, arr.get());
  ASSERT_NE(nullptr, arr->bitmask);
  ASSERT_NE(nullptr, arr->bitmask_cumsum128);

  uint8_t expected_cumsum = 0;
  for (size_t i = 0; i < mask.size(); ++i) {
    EXPECT_EQ(mask[i], arr->bitmask[i]) << "byte " << i;
    const uint8_t byte_pop = static_cast<uint8_t>(std::popcount(mask[i]));
    if (i % 16 == 0) {
      expected_cumsum = byte_pop;
    } else {
      expected_cumsum = static_cast<uint8_t>(expected_cumsum + byte_pop);
    }
    EXPECT_EQ(expected_cumsum, arr->bitmask_cumsum128[i]) << "byte " << i;
  }
  // Verify exact block boundary values:
  // Block 0 popcounts: 0+1+1+1+1+1+1+1+1+4+4+4+4+4+4+8 = 40.
  EXPECT_EQ(40, arr->bitmask_cumsum128[15]);
  // Block 1 resets at index 16 to 8 and reaches 128 at index 31.
  EXPECT_EQ(8, arr->bitmask_cumsum128[16]);
  EXPECT_EQ(128, arr->bitmask_cumsum128[31]);
  // Block 2 resets at index 32 to 2.
  EXPECT_EQ(2, arr->bitmask_cumsum128[32]);
}

TEST(CodaGribTypeTest, TypeDeleteHandlesPartialAndNonArrayDynamicTypes) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  UniqueCodaType<coda_type_number> real_def = MakeGribRealDef();
  ASSERT_NE(nullptr, real_def.get());
  UniqueCodaType<coda_type_array> arr_def =
      MakeGribArrayDef(reinterpret_cast<coda_type*>(real_def.get()));
  ASSERT_NE(nullptr, arr_def.get());

  auto make_partial_array = [&]() -> coda_grib_value_array* {
    auto* p = static_cast<coda_grib_value_array*>(
        std::malloc(sizeof(coda_grib_value_array)));
    if (p == nullptr) {
      return nullptr;
    }
    p->backend = coda_backend_grib;
    p->definition = arr_def.get();
    arr_def->retain_count++;
    p->num_elements = 4;
    p->base_type = nullptr;
    p->bit_offset = 0;
    p->simple_packing = 0;
    p->element_bit_size = 32;
    p->decimalScaleFactor = 0;
    p->binaryScaleFactor = 0;
    p->referenceValue = 0.0f;
    p->scalefactor = 0.0;
    p->offset = 0.0;
    p->bitmask = nullptr;
    p->bitmask_cumsum128 = nullptr;
    return p;
  };

  // 1. Array dynamic type with NULL base_type, bitmask, and bitmask_cumsum128.
  {
    coda_grib_value_array* partial = make_partial_array();
    ASSERT_NE(nullptr, partial);
    coda_grib_type_delete(reinterpret_cast<coda_dynamic_type*>(partial));
    EXPECT_EQ(0, arr_def->retain_count);
  }

  // 2. Array dynamic type with non-NULL bitmask and NULL bitmask_cumsum128.
  {
    coda_grib_value_array* partial = make_partial_array();
    ASSERT_NE(nullptr, partial);
    partial->bitmask = static_cast<uint8_t*>(std::malloc(4));
    ASSERT_NE(nullptr, partial->bitmask);
    coda_grib_type_delete(reinterpret_cast<coda_dynamic_type*>(partial));
    EXPECT_EQ(0, arr_def->retain_count);
  }

  // 3. Array dynamic type with NULL bitmask and non-NULL bitmask_cumsum128.
  {
    coda_grib_value_array* partial = make_partial_array();
    ASSERT_NE(nullptr, partial);
    partial->bitmask_cumsum128 = static_cast<uint8_t*>(std::malloc(4));
    ASSERT_NE(nullptr, partial->bitmask_cumsum128);
    coda_grib_type_delete(reinterpret_cast<coda_dynamic_type*>(partial));
    EXPECT_EQ(0, arr_def->retain_count);
  }

  // 4. Non-array dynamic type (coda_real_class) dispatched via
  //    coda_dynamic_type_delete -> coda_grib_type_delete.
  {
    auto* scalar_dyn =
        static_cast<coda_dynamic_type*>(std::malloc(sizeof(coda_dynamic_type)));
    ASSERT_NE(nullptr, scalar_dyn);
    scalar_dyn->backend = coda_backend_grib;
    scalar_dyn->definition = reinterpret_cast<coda_type*>(real_def.get());
    real_def->retain_count++;
    coda_dynamic_type_delete(scalar_dyn);
    EXPECT_EQ(1, real_def->retain_count);
  }
}

TEST(CodaGribTypeTest, Grib1TypeHierarchyTraversalAndValueArrayTypes) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  GribBuilder builder;

  // Message 0: Regular Lat/Lon GDS with PDS local_use, coordinate_values,
  //            4-element bitmap (2 present, 2 masked), and scaled simple
  //            packing.
  Grib1MessageSpec msg0;
  msg0.local_use = {0xAA, 0xBB, 0xCC};
  msg0.decimal_scale_factor = 1;
  msg0.gds = Grib1GdsSpec{
      .data_representation_type = 0,
      .ni = 2,
      .nj = 2,
      .coordinate_values = {1.5f, 2.5f},
  };
  msg0.bms = Grib1BmsSpec{
      .bitmap = {true, false, true, false},
  };
  msg0.bds = Grib1BdsSpec{
      .binary_scale_factor = -1,
      .reference_value = 10.0f,
      .bits_per_value = 8,
      .values = {1.0f, 1.5f},
  };
  builder.AddGrib1(msg0);

  // Message 1: Gaussian reduced grid (Ni = 65535, list_of_numbers) with 0-bit
  //            constant field packing.
  Grib1MessageSpec msg1;
  msg1.gds = Grib1GdsSpec{
      .data_representation_type = 4,
      .ni = 65535,
      .nj = 3,
      .j_direction_increment_or_n = 2,
      .list_of_numbers = {2, 4, 2},
  };
  msg1.bds = Grib1BdsSpec{
      .reference_value = 3.5f,
      .bits_per_value = 0,
  };
  builder.AddGrib1(msg1);

  // Message 2: Predefined grid (grid_definition = 50 -> 964 elements, no GDS),
  //            0-bit constant packing.
  Grib1MessageSpec msg2;
  msg2.grid_definition = 50;
  msg2.gds = std::nullopt;
  msg2.bds = Grib1BdsSpec{
      .reference_value = 4.25f,
      .bits_per_value = 0,
  };
  builder.AddGrib1(msg2);

  // Message 3: 140-point grid (14x10) with an 18-byte bitmap crossing the
  //            128-bit cumsum128 block boundary.
  Grib1MessageSpec msg3;
  msg3.gds = Grib1GdsSpec{
      .data_representation_type = 0,
      .ni = 14,
      .nj = 10,
  };
  std::vector<bool> mask140(140, false);
  std::vector<float> present140;
  for (size_t i = 0; i < 140; ++i) {
    if (i % 3 != 0) {
      mask140[i] = true;
      present140.push_back(static_cast<float>(present140.size()));
    }
  }
  msg3.bms = Grib1BmsSpec{.bitmap = mask140};
  msg3.bds = Grib1BdsSpec{
      .binary_scale_factor = 0,
      .reference_value = 0.0f,
      .bits_per_value = 8,
      .values = present140,
  };
  builder.AddGrib1(msg3);

  const TempFile grib1_file("grib1_hierarchy.grb");
  ASSERT_TRUE(grib1_file.WriteBytes(builder.Build()));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(grib1_file.c_str(), product.OutParam()));

  // 1. Traverse entire GRIB1 type hierarchy from root.
  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  TreeSummary summary;
  TraverseGribTree(&cursor, "", summary);

  EXPECT_EQ(15, summary.record_count);
  EXPECT_EQ(4, summary.union_count);
  EXPECT_GE(summary.grib_value_array_count, 4);
  EXPECT_EQ(1, summary.raw_count);
  EXPECT_GT(summary.integer_count, 100);
  EXPECT_GT(summary.real_count, 1000);
  EXPECT_EQ(16, summary.hidden_field_count);  // 4 hidden fields per message.

  // 2. Verify Message 0 values (bitmap + decimal/binary scale factors).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/[0]/grib1/data/values"));
  const auto* val0_type = reinterpret_cast<const coda_grib_value_array*>(
      CursorTopDynamicType(cursor));
  ASSERT_EQ(coda_backend_grib, val0_type->backend);
  EXPECT_EQ(4, val0_type->num_elements);
  EXPECT_EQ(1, val0_type->simple_packing);
  EXPECT_EQ(8, val0_type->element_bit_size);
  EXPECT_EQ(1, val0_type->decimalScaleFactor);
  EXPECT_EQ(-1, val0_type->binaryScaleFactor);
  EXPECT_FLOAT_EQ(10.0f, val0_type->referenceValue);
  ASSERT_NE(nullptr, val0_type->bitmask);
  ASSERT_NE(nullptr, val0_type->bitmask_cumsum128);
  EXPECT_EQ(0xA0, val0_type->bitmask[0]);
  EXPECT_EQ(2, val0_type->bitmask_cumsum128[0]);

  std::vector<float> vals0(4, -999.0f);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(&cursor, vals0.data(),
                                                       coda_array_ordering_c));
  EXPECT_NEAR(1.0f, vals0[0], 1e-5f);
  EXPECT_TRUE(std::isnan(vals0[1]));
  EXPECT_NEAR(1.5f, vals0[2], 1e-5f);
  EXPECT_TRUE(std::isnan(vals0[3]));

  // 3. Verify Message 1 Gaussian reduced grid and 0-bit constant packing.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/[1]/grib1/grid/N"));
  uint16_t n_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&cursor, &n_val));
  EXPECT_EQ(2, n_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto(&cursor, "/[1]/grib1/grid/listOfNumbers"));
  std::array<uint16_t, 3> row_lens = {};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16_array(
                              &cursor, row_lens.data(), coda_array_ordering_c));
  EXPECT_THAT(row_lens, ElementsAre(2, 4, 2));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/[1]/grib1/data/values"));
  std::vector<float> vals1(8, -999.0f);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(&cursor, vals1.data(),
                                                       coda_array_ordering_c));
  for (float v : vals1) {
    EXPECT_FLOAT_EQ(3.5f, v);
  }

  // 4. Verify Message 2 predefined grid element count (964) and constant value.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/[2]/grib1/data/values"));
  long num_vals2 = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_vals2));
  EXPECT_EQ(964, num_vals2);
  std::array<float, 3> partial2 = {-999.0f, -999.0f, -999.0f};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_partial_array(
                              &cursor, 961, 3, partial2.data()));
  for (float v : partial2) {
    EXPECT_FLOAT_EQ(4.25f, v);
  }

  // 5. Verify Message 3 140-point bitmap across the 128-bit cumsum128 boundary.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/[3]/grib1/data/values"));
  std::vector<float> vals3(140, -999.0f);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(&cursor, vals3.data(),
                                                       coda_array_ordering_c));
  size_t expected_idx = 0;
  for (size_t i = 0; i < 140; ++i) {
    if (i % 3 == 0) {
      EXPECT_TRUE(std::isnan(vals3[i])) << "index " << i;
    } else {
      EXPECT_FLOAT_EQ(static_cast<float>(expected_idx), vals3[i])
          << "index " << i;
      expected_idx++;
    }
  }
}

TEST(CodaGribTypeTest, Grib2TypeHierarchyTraversalAndMixedEditionStream) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  GribBuilder builder;

  // Message 0: GRIB1 message followed by filler bytes before GRIB2.
  Grib1MessageSpec grib1_msg;
  grib1_msg.bds = Grib1BdsSpec{.reference_value = 5.0f, .bits_per_value = 0};
  builder.AddGrib1(grib1_msg);
  builder.AddFiller("FILLER_BYTES");

  // Message 1: Multi-field GRIB2 message.
  Grib2MessageSpec grib2_msg;
  grib2_msg.discipline = 2;

  // Field 0: local_use, Lat/Lon template 0, Product template 0 with
  //          coordinate_values (exercises coda_grib_value_array_new with
  //          simple_packing == 0) and 140-point inline bitmap (18 bytes).
  Grib2FieldSpec field0;
  field0.local_use = {0x11, 0x22, 0x33};
  field0.grid = Grib2GridSpec{
      .number_of_data_points = 140,
      .template_number = 0,
      .ni = 14,
      .nj = 10,
  };
  field0.product = Grib2ProductSpec{
      .template_number = 0,
      .type_of_first_fixed_surface = 100,
      .scale_factor_first_fixed_surface = 1,
      .scaled_value_first_fixed_surface = 5000,
      .type_of_second_fixed_surface = 255,
      .coordinate_values = {100.0f, 200.5f},
  };
  field0.data_rep = Grib2DataRepSpec{
      .number_of_data_points = 140,
      .template_number = 0,
      .reference_value = 0.0f,
      .binary_scale_factor = 0,
      .decimal_scale_factor = 0,
      .bits_per_value = 8,
  };
  std::vector<bool> mask140(140, false);
  std::vector<float> present140;
  for (size_t i = 0; i < 140; ++i) {
    if ((i & 1u) != 0) {
      mask140[i] = true;
      present140.push_back(static_cast<float>(present140.size()));
    }
  }
  field0.bitmap = Grib2BitmapSpec{.bitmap = mask140};
  field0.data = Grib2DataSpec{.values = present140};

  // Field 1: Gaussian reduced grid (template 40, Ni = 0xFFFFFFFF), Product
  //          template 40 (with constituentType; second fixed surface = 255),
  //          DataRep template 1 with 0-bit constant packing, reusing Field 0's
  //          bitmap (indicator = 254).
  Grib2FieldSpec field1;
  field1.local_use = std::nullopt;
  field1.grid = Grib2GridSpec{
      .number_of_data_points = 7,
      .template_number = 40,
      .ni = 0xFFFFFFFFu,
      .nj = 3,
      .j_direction_increment_or_n = 2,
      .list_of_numbers = {2, 3, 2},
  };
  field1.product = Grib2ProductSpec{
      .template_number = 40,
      .constituent_type = 42,
      .type_of_first_fixed_surface = 1,
      .scale_factor_first_fixed_surface = -1,
      .scaled_value_first_fixed_surface = 12,
      .type_of_second_fixed_surface = 255,
  };
  field1.data_rep = Grib2DataRepSpec{
      .number_of_data_points = 7,
      .template_number = 1,
      .reference_value = 7.25f,
      .binary_scale_factor = 0,
      .decimal_scale_factor = 0,
      .bits_per_value = 0,
  };
  field1.bitmap = Grib2BitmapSpec{.indicator_override = 254};

  grib2_msg.fields = {field0, field1};
  builder.AddGrib2(grib2_msg);

  const TempFile mixed_file("grib2_mixed_hierarchy.grb");
  ASSERT_TRUE(mixed_file.WriteBytes(builder.Build()));

  ScopedProduct product;
  {
    // TODO(schwehr): coda-grib.c:2342 leaks raw_data in GRIB2 Section 2.
    const absl::LeakCheckDisabler leak_disabler;
    ASSERT_EQ(CODA_SUCCESS, coda_open(mixed_file.c_str(), product.OutParam()));
  }

  // 1. Traverse mixed GRIB1 + GRIB2 hierarchy completely.
  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  TreeSummary summary;
  TraverseGribTree(&cursor, "", summary);

  EXPECT_EQ(10, summary.record_count);
  EXPECT_EQ(2, summary.union_count);
  // 1 GRIB1 values + 1 coordinateValues + 2 GRIB2 values.
  EXPECT_EQ(4, summary.grib_value_array_count);
  EXPECT_EQ(1, summary.raw_count);  // Field 0 local_use.

  // 2. Verify GRIB2 Field 0 coordinateValues uses un-packed
  // coda_grib_value_array.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto(&cursor, "/[1]/grib2/data[0]/coordinateValues"));
  const auto* coord_type = reinterpret_cast<const coda_grib_value_array*>(
      CursorTopDynamicType(cursor));
  ASSERT_EQ(coda_backend_grib, coord_type->backend);
  EXPECT_EQ(0, coord_type->simple_packing);
  EXPECT_EQ(2, coord_type->num_elements);
  EXPECT_EQ(nullptr, coord_type->bitmask);
  EXPECT_EQ(nullptr, coord_type->bitmask_cumsum128);

  std::array<float, 2> coords = {-999.0f, -999.0f};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(&cursor, coords.data(),
                                                       coda_array_ordering_c));
  EXPECT_FLOAT_EQ(100.0f, coords[0]);
  EXPECT_FLOAT_EQ(200.5f, coords[1]);

  // 3. Verify GRIB2 Field 0 140-point masked values across 128-bit boundary
  //    using coda_cursor_goto_array_element().
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto(&cursor, "/[1]/grib2/data[0]/values"));
  std::vector<float> vals0(140, -999.0f);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(&cursor, vals0.data(),
                                                       coda_array_ordering_c));
  size_t expected_idx = 0;
  for (size_t i = 0; i < 140; ++i) {
    if ((i & 1u) == 0) {
      EXPECT_TRUE(std::isnan(vals0[i])) << "index " << i;
    } else {
      EXPECT_FLOAT_EQ(static_cast<float>(expected_idx), vals0[i])
          << "index " << i;
      expected_idx++;
    }
  }
  const long sub = 135;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&cursor, 1, &sub));
  float elem135 = -999.0f;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &elem135));
  EXPECT_FLOAT_EQ(67.0f, elem135);

  // 4. Verify GRIB2 Field 1 template 40 fields, fixed surfaces, and reused
  //    bitmap with 0-bit constant field.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto(&cursor, "/[1]/grib2/data[1]/constituentType"));
  uint16_t constituent = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&cursor, &constituent));
  EXPECT_EQ(42, constituent);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto(&cursor, "/[1]/grib2/data[1]/firstFixedSurface"));
  double first_surface = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &first_surface));
  EXPECT_DOUBLE_EQ(120.0, first_surface);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto(&cursor, "/[1]/grib2/data[1]/secondFixedSurface"));
  double second_surface = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &second_surface));
  EXPECT_TRUE(std::isnan(second_surface));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto(&cursor, "/[1]/grib2/data[1]/values"));
  std::array<float, 7> vals1 = {};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(&cursor, vals1.data(),
                                                       coda_array_ordering_c));
  for (float v : vals1) {
    EXPECT_FLOAT_EQ(7.25f, v);
  }
}

// TODO(schwehr): 88.2% reachable ceiling in libcoda/coda-grib-type.c (97/110);
// all 13 remaining uncovered lines (105, 107, 128-129, 131, 171-174, 181-184)
// are malloc() OOM branches.

}  // namespace
}  // namespace stcorp_coda
