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
// Tests CODA memory backend cursor navigation, metadata queries, scalar reads,
// array reads, and partial array reads (libcoda/coda-mem-cursor.c).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-mem-cursor.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-mem.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-mem-internal.h

#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-mem-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

constexpr coda_endianness kHostEndianness =
    (std::endian::native == std::endian::big) ? coda_big_endian
                                              : coda_little_endian;

struct TypeDeleter {
  void operator()(coda_type* type) const { coda_type_release(type); }
};
using ScopedType = std::unique_ptr<coda_type, TypeDeleter>;

struct FieldDeleter {
  void operator()(coda_type_record_field* field) const {
    coda_type_record_field_delete(field);
  }
};
using ScopedRecordField = std::unique_ptr<coda_type_record_field, FieldDeleter>;

struct DynamicTypeDeleter {
  void operator()(coda_dynamic_type* type) const {
    coda_dynamic_type_delete(type);
  }
};
using ScopedDynamicType =
    std::unique_ptr<coda_dynamic_type, DynamicTypeDeleter>;

template <typename T>
coda_type* TrackType(T* raw_type, std::vector<ScopedType>& owned_types) {
  auto* base = reinterpret_cast<coda_type*>(raw_type);
  owned_types.emplace_back(base);
  return base;
}

// Allocates a synthetic coda_product using coda_format_sp3 so coda_close()
// invokes coda_sp3_close() (which frees root_type, filename, mem_ptr, and the
// product struct without requiring an underlying OS file descriptor).
ScopedProduct MakeSyntheticMemProduct() {
  auto* product =
      static_cast<coda_product*>(std::calloc(1, sizeof(coda_product)));
  if (product != nullptr) {
    product->format = coda_format_sp3;
  }
  return ScopedProduct(product);
}

coda_type_number* MakeNumberType(coda_format format, coda_type_class type_class,
                                 coda_native_type read_type, int64_t bit_size,
                                 std::vector<ScopedType>& owned_types) {
  coda_type_number* num = coda_type_number_new(format, type_class);
  if (num == nullptr) {
    return nullptr;
  }
  TrackType(num, owned_types);
  if (coda_type_number_set_endianness(num, kHostEndianness) != CODA_SUCCESS ||
      coda_type_set_read_type(reinterpret_cast<coda_type*>(num), read_type) !=
          CODA_SUCCESS) {
    return nullptr;
  }
  if (bit_size >= 0 && coda_type_set_bit_size(reinterpret_cast<coda_type*>(num),
                                              bit_size) != CODA_SUCCESS) {
    return nullptr;
  }
  return num;
}

coda_type_text* MakeCharType(coda_format format,
                             std::vector<ScopedType>& owned_types) {
  coda_type_text* text = coda_type_text_new(format);
  if (text == nullptr) {
    return nullptr;
  }
  TrackType(text, owned_types);
  if (coda_type_set_read_type(reinterpret_cast<coda_type*>(text),
                              coda_native_type_char) != CODA_SUCCESS ||
      coda_type_set_bit_size(reinterpret_cast<coda_type*>(text), 8) !=
          CODA_SUCCESS) {
    return nullptr;
  }
  return text;
}

coda_type_text* MakeStringType(coda_format format, int64_t byte_size,
                               std::vector<ScopedType>& owned_types) {
  coda_type_text* text = coda_type_text_new(format);
  if (text == nullptr) {
    return nullptr;
  }
  TrackType(text, owned_types);
  if (byte_size >= 0 &&
      coda_type_set_byte_size(reinterpret_cast<coda_type*>(text), byte_size) !=
          CODA_SUCCESS) {
    return nullptr;
  }
  return text;
}

template <typename T>
struct MemTypeTraits;

template <>
struct MemTypeTraits<int8_t> {
  static constexpr coda_type_class kClass = coda_integer_class;
  static constexpr coda_native_type kNative = coda_native_type_int8;
  static constexpr int64_t kBitSize = 8;
  static constexpr std::array<int8_t, 4> kValues = {-10, 20, -30, 40};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product,
                               int8_t v) {
    return coda_mem_int8_new(reinterpret_cast<coda_type_number*>(def), nullptr,
                             product, v);
  }
  static std::string FormatAscii(int8_t v) {
    return absl::StrFormat("%20d", static_cast<int>(v));
  }
  static int ReadArray(const coda_cursor* c, int8_t* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_int8_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         int8_t* dst) {
    return coda_cursor_read_int8_partial_array(c, off, len, dst);
  }
};

template <>
struct MemTypeTraits<uint8_t> {
  static constexpr coda_type_class kClass = coda_integer_class;
  static constexpr coda_native_type kNative = coda_native_type_uint8;
  static constexpr int64_t kBitSize = 8;
  static constexpr std::array<uint8_t, 4> kValues = {10, 200, 30, 240};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product,
                               uint8_t v) {
    return coda_mem_uint8_new(reinterpret_cast<coda_type_number*>(def), nullptr,
                              product, v);
  }
  static std::string FormatAscii(uint8_t v) {
    return absl::StrFormat("%20u", static_cast<unsigned int>(v));
  }
  static int ReadArray(const coda_cursor* c, uint8_t* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_uint8_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         uint8_t* dst) {
    return coda_cursor_read_uint8_partial_array(c, off, len, dst);
  }
};

template <>
struct MemTypeTraits<int16_t> {
  static constexpr coda_type_class kClass = coda_integer_class;
  static constexpr coda_native_type kNative = coda_native_type_int16;
  static constexpr int64_t kBitSize = 16;
  static constexpr std::array<int16_t, 4> kValues = {-1000, 2000, -3000, 4000};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product,
                               int16_t v) {
    return coda_mem_int16_new(reinterpret_cast<coda_type_number*>(def), nullptr,
                              product, v);
  }
  static std::string FormatAscii(int16_t v) {
    return absl::StrFormat("%20d", static_cast<int>(v));
  }
  static int ReadArray(const coda_cursor* c, int16_t* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_int16_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         int16_t* dst) {
    return coda_cursor_read_int16_partial_array(c, off, len, dst);
  }
};

template <>
struct MemTypeTraits<uint16_t> {
  static constexpr coda_type_class kClass = coda_integer_class;
  static constexpr coda_native_type kNative = coda_native_type_uint16;
  static constexpr int64_t kBitSize = 16;
  static constexpr std::array<uint16_t, 4> kValues = {1000, 50000, 3000, 60000};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product,
                               uint16_t v) {
    return coda_mem_uint16_new(reinterpret_cast<coda_type_number*>(def),
                               nullptr, product, v);
  }
  static std::string FormatAscii(uint16_t v) {
    return absl::StrFormat("%20u", static_cast<unsigned int>(v));
  }
  static int ReadArray(const coda_cursor* c, uint16_t* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_uint16_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         uint16_t* dst) {
    return coda_cursor_read_uint16_partial_array(c, off, len, dst);
  }
};

template <>
struct MemTypeTraits<int32_t> {
  static constexpr coda_type_class kClass = coda_integer_class;
  static constexpr coda_native_type kNative = coda_native_type_int32;
  static constexpr int64_t kBitSize = 32;
  static constexpr std::array<int32_t, 4> kValues = {-100000, 200000, -300000,
                                                     400000};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product,
                               int32_t v) {
    return coda_mem_int32_new(reinterpret_cast<coda_type_number*>(def), nullptr,
                              product, v);
  }
  static std::string FormatAscii(int32_t v) {
    return absl::StrFormat("%20d", v);
  }
  static int ReadArray(const coda_cursor* c, int32_t* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_int32_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         int32_t* dst) {
    return coda_cursor_read_int32_partial_array(c, off, len, dst);
  }
};

template <>
struct MemTypeTraits<uint32_t> {
  static constexpr coda_type_class kClass = coda_integer_class;
  static constexpr coda_native_type kNative = coda_native_type_uint32;
  static constexpr int64_t kBitSize = 32;
  static constexpr std::array<uint32_t, 4> kValues = {100000U, 3000000000U,
                                                      300000U, 4000000000U};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product,
                               uint32_t v) {
    return coda_mem_uint32_new(reinterpret_cast<coda_type_number*>(def),
                               nullptr, product, v);
  }
  static std::string FormatAscii(uint32_t v) {
    return absl::StrFormat("%20u", v);
  }
  static int ReadArray(const coda_cursor* c, uint32_t* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_uint32_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         uint32_t* dst) {
    return coda_cursor_read_uint32_partial_array(c, off, len, dst);
  }
};

template <>
struct MemTypeTraits<int64_t> {
  static constexpr coda_type_class kClass = coda_integer_class;
  static constexpr coda_native_type kNative = coda_native_type_int64;
  static constexpr int64_t kBitSize = 64;
  static constexpr std::array<int64_t, 4> kValues = {
      -5000000000LL, 6000000000LL, -7000000000LL, 8000000000LL};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product,
                               int64_t v) {
    return coda_mem_int64_new(reinterpret_cast<coda_type_number*>(def), nullptr,
                              product, v);
  }
  static std::string FormatAscii(int64_t v) {
    return absl::StrFormat("%20lld", static_cast<long long>(v));
  }
  static int ReadArray(const coda_cursor* c, int64_t* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_int64_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         int64_t* dst) {
    return coda_cursor_read_int64_partial_array(c, off, len, dst);
  }
};

template <>
struct MemTypeTraits<uint64_t> {
  static constexpr coda_type_class kClass = coda_integer_class;
  static constexpr coda_native_type kNative = coda_native_type_uint64;
  static constexpr int64_t kBitSize = 64;
  static constexpr std::array<uint64_t, 4> kValues = {
      5000000000ULL, 15000000000000ULL, 7000000000ULL, 18000000000000ULL};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product,
                               uint64_t v) {
    return coda_mem_uint64_new(reinterpret_cast<coda_type_number*>(def),
                               nullptr, product, v);
  }
  static std::string FormatAscii(uint64_t v) {
    return absl::StrFormat("%20llu", static_cast<unsigned long long>(v));
  }
  static int ReadArray(const coda_cursor* c, uint64_t* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_uint64_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         uint64_t* dst) {
    return coda_cursor_read_uint64_partial_array(c, off, len, dst);
  }
};

template <>
struct MemTypeTraits<float> {
  static constexpr coda_type_class kClass = coda_real_class;
  static constexpr coda_native_type kNative = coda_native_type_float;
  static constexpr int64_t kBitSize = 32;
  static constexpr std::array<float, 4> kValues = {1.5f, -2.5f, 3.25f, -4.75f};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product, float v) {
    return coda_mem_float_new(reinterpret_cast<coda_type_number*>(def), nullptr,
                              product, v);
  }
  static std::string FormatAscii(float v) {
    return absl::StrFormat("%20g", static_cast<double>(v));
  }
  static int ReadArray(const coda_cursor* c, float* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_float_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len, float* dst) {
    return coda_cursor_read_float_partial_array(c, off, len, dst);
  }
};

template <>
struct MemTypeTraits<double> {
  static constexpr coda_type_class kClass = coda_real_class;
  static constexpr coda_native_type kNative = coda_native_type_double;
  static constexpr int64_t kBitSize = 64;
  static constexpr std::array<double, 4> kValues = {1.5, -2.5, 3.25, -4.75};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product,
                               double v) {
    return coda_mem_double_new(reinterpret_cast<coda_type_number*>(def),
                               nullptr, product, v);
  }
  static std::string FormatAscii(double v) {
    return absl::StrFormat("%20g", v);
  }
  static int ReadArray(const coda_cursor* c, double* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_double_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         double* dst) {
    return coda_cursor_read_double_partial_array(c, off, len, dst);
  }
};

template <>
struct MemTypeTraits<char> {
  static constexpr coda_type_class kClass = coda_text_class;
  static constexpr coda_native_type kNative = coda_native_type_char;
  static constexpr int64_t kBitSize = 8;
  static constexpr std::array<char, 4> kValues = {'a', 'b', 'c', 'd'};

  static coda_mem_data* NewMem(coda_type* def, coda_product* product, char v) {
    return coda_mem_char_new(reinterpret_cast<coda_type_text*>(def), nullptr,
                             product, v);
  }
  static std::string FormatAscii(char v) { return std::string(1, v); }
  static int ReadArray(const coda_cursor* c, char* dst,
                       coda_array_ordering ord) {
    return coda_cursor_read_char_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len, char* dst) {
    return coda_cursor_read_char_partial_array(c, off, len, dst);
  }
};

template <typename T>
coda_type* MakeElemTypeForArray(coda_format format, bool ascii_element,
                                std::vector<ScopedType>& owned_types) {
  if constexpr (std::is_same_v<T, char>) {
    return reinterpret_cast<coda_type*>(MakeCharType(format, owned_types));
  } else {
    const int64_t bit_size =
        ascii_element ? 20 * 8 : MemTypeTraits<T>::kBitSize;
    return reinterpret_cast<coda_type*>(
        MakeNumberType(format, MemTypeTraits<T>::kClass,
                       MemTypeTraits<T>::kNative, bit_size, owned_types));
  }
}

template <typename T>
class CodaMemCursorTypedArrayTest : public ::testing::Test {};

using ArrayElementTypes =
    ::testing::Types<int8_t, uint8_t, int16_t, uint16_t, int32_t, uint32_t,
                     int64_t, uint64_t, float, double, char>;
TYPED_TEST_SUITE(CodaMemCursorTypedArrayTest, ArrayElementTypes);

TYPED_TEST(CodaMemCursorTypedArrayTest, ReadArrayAndPartialAcrossAllBackends) {
  using Traits = MemTypeTraits<TypeParam>;
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<ScopedType> owned_types;
  ScopedProduct product = MakeSyntheticMemProduct();
  ASSERT_NE(nullptr, product.get());

  // 1. Build a tag_mem_array (2x2), a tag_mem_data wrapping a binary 2x2 array,
  //    a tag_mem_data wrapping an ASCII 2x2 array, and a malformed ASCII array
  //    under a single root coda_mem_record.
  coda_type* mem_elem_def =
      MakeElemTypeForArray<TypeParam>(coda_format_binary, false, owned_types);
  ASSERT_NE(nullptr, mem_elem_def);
  auto* mem_arr_def = coda_type_array_new(coda_format_binary);
  ASSERT_NE(nullptr, mem_arr_def);
  TrackType(mem_arr_def, owned_types);
  ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(mem_arr_def, 2));
  ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(mem_arr_def, 2));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_set_base_type(mem_arr_def, mem_elem_def));

  ScopedDynamicType mem_arr_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_array_new(mem_arr_def, nullptr)));
  ASSERT_NE(nullptr, mem_arr_dyn.get());
  auto* mem_arr = reinterpret_cast<coda_mem_array*>(mem_arr_dyn.get());
  for (int i = 0; i < 4; ++i) {
    ScopedDynamicType elem(reinterpret_cast<coda_dynamic_type*>(
        Traits::NewMem(mem_elem_def, product.get(), Traits::kValues[i])));
    ASSERT_NE(nullptr, elem.get());
    ASSERT_EQ(CODA_SUCCESS, coda_mem_array_set_element(mem_arr, i, elem.get()));
    elem.release();
  }

  // Binary tag_mem_data 2x2 array.
  coda_type* bin_elem_def =
      MakeElemTypeForArray<TypeParam>(coda_format_binary, false, owned_types);
  ASSERT_NE(nullptr, bin_elem_def);
  auto* bin_arr_def = coda_type_array_new(coda_format_binary);
  ASSERT_NE(nullptr, bin_arr_def);
  TrackType(bin_arr_def, owned_types);
  ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(bin_arr_def, 2));
  ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(bin_arr_def, 2));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_set_base_type(bin_arr_def, bin_elem_def));

  std::array<uint8_t, 4 * sizeof(TypeParam)> bin_bytes{};
  std::memcpy(bin_bytes.data(), Traits::kValues.data(), bin_bytes.size());
  ScopedDynamicType bin_arr_dyn(
      reinterpret_cast<coda_dynamic_type*>(coda_mem_data_new(
          reinterpret_cast<coda_type*>(bin_arr_def), nullptr, product.get(),
          static_cast<long>(bin_bytes.size()), bin_bytes.data())));
  ASSERT_NE(nullptr, bin_arr_dyn.get());

  // ASCII tag_mem_data 2x2 array (using coda_format_ascii base_type).
  coda_type* ascii_elem_def =
      MakeElemTypeForArray<TypeParam>(coda_format_ascii, true, owned_types);
  ASSERT_NE(nullptr, ascii_elem_def);
  auto* ascii_arr_def = coda_type_array_new(coda_format_ascii);
  ASSERT_NE(nullptr, ascii_arr_def);
  TrackType(ascii_arr_def, owned_types);
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_add_fixed_dimension(ascii_arr_def, 2));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_add_fixed_dimension(ascii_arr_def, 2));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_set_base_type(ascii_arr_def, ascii_elem_def));

  std::string ascii_payload;
  for (const TypeParam v : Traits::kValues) {
    ascii_payload += Traits::FormatAscii(v);
  }
  ScopedDynamicType ascii_arr_dyn(
      reinterpret_cast<coda_dynamic_type*>(coda_mem_data_new(
          reinterpret_cast<coda_type*>(ascii_arr_def), nullptr, product.get(),
          static_cast<long>(ascii_payload.size()),
          reinterpret_cast<const uint8_t*>(ascii_payload.data()))));
  ASSERT_NE(nullptr, ascii_arr_dyn.get());

  // Malformed/truncated ASCII tag_mem_data array to cover read_array failure.
  ScopedDynamicType bad_ascii_arr_dyn;
  if constexpr (std::is_same_v<TypeParam, char>) {
    auto* bad_arr_def = coda_type_array_new(coda_format_ascii);
    ASSERT_NE(nullptr, bad_arr_def);
    TrackType(bad_arr_def, owned_types);
    ScopedExpr dim_expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("4", dim_expr.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_variable_dimension(
                                bad_arr_def, dim_expr.get()));
    dim_expr.release();
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_set_base_type(bad_arr_def, ascii_elem_def));
    constexpr char kTruncated[] = "abc";
    bad_ascii_arr_dyn.reset(
        reinterpret_cast<coda_dynamic_type*>(coda_mem_data_new(
            reinterpret_cast<coda_type*>(bad_arr_def), nullptr, product.get(),
            3, reinterpret_cast<const uint8_t*>(kTruncated))));
  } else {
    std::string bad_payload = "                bad!";
    for (int i = 1; i < 4; ++i) {
      bad_payload += Traits::FormatAscii(Traits::kValues[i]);
    }
    bad_ascii_arr_dyn.reset(
        reinterpret_cast<coda_dynamic_type*>(coda_mem_data_new(
            reinterpret_cast<coda_type*>(ascii_arr_def), nullptr, product.get(),
            static_cast<long>(bad_payload.size()),
            reinterpret_cast<const uint8_t*>(bad_payload.data()))));
  }
  ASSERT_NE(nullptr, bad_ascii_arr_dyn.get());

  auto* root_def = coda_type_record_new(coda_format_binary);
  ASSERT_NE(nullptr, root_def);
  TrackType(root_def, owned_types);
  ScopedDynamicType root_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_record_new(root_def, nullptr)));
  ASSERT_NE(nullptr, root_dyn.get());
  auto* root = reinterpret_cast<coda_mem_record*>(root_dyn.get());

  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(root, "mem_arr", mem_arr_dyn.get(), 1));
  mem_arr_dyn.release();
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(root, "bin_arr", bin_arr_dyn.get(), 1));
  bin_arr_dyn.release();
  ASSERT_EQ(CODA_SUCCESS, coda_mem_record_add_field(root, "ascii_arr",
                                                    ascii_arr_dyn.get(), 1));
  ascii_arr_dyn.release();
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(root, "bad_ascii_arr",
                                      bad_ascii_arr_dyn.get(), 1));
  bad_ascii_arr_dyn.release();
  product.get()->root_type = root_dyn.release();

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  const std::array<TypeParam, 4> expected_c = Traits::kValues;
  const std::array<TypeParam, 4> expected_fortran = {
      Traits::kValues[0], Traits::kValues[2], Traits::kValues[1],
      Traits::kValues[3]};

  // 2. Verify all three valid array representations: tag_mem_array,
  //    tag_mem_data(binary), and tag_mem_data(ascii).
  for (const char* field_name : {"mem_arr", "bin_arr", "ascii_arr"}) {
    coda_cursor arr_cursor = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&arr_cursor, field_name));

    std::array<TypeParam, 4> out_c{};
    ASSERT_EQ(CODA_SUCCESS, Traits::ReadArray(&arr_cursor, out_c.data(),
                                              coda_array_ordering_c));
    EXPECT_THAT(out_c, ElementsAreArray(expected_c));

    std::array<TypeParam, 4> out_f{};
    ASSERT_EQ(CODA_SUCCESS, Traits::ReadArray(&arr_cursor, out_f.data(),
                                              coda_array_ordering_fortran));
    EXPECT_THAT(out_f, ElementsAreArray(expected_fortran));

    std::array<TypeParam, 2> out_partial{};
    ASSERT_EQ(CODA_SUCCESS,
              Traits::ReadPartial(&arr_cursor, 1, 2, out_partial.data()));
    EXPECT_THAT(out_partial,
                ElementsAre(Traits::kValues[1], Traits::kValues[2]));
  }

  // 3. Verify read failure on malformed/truncated ASCII tag_mem_data array.
  coda_cursor bad_cursor = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(
                              &bad_cursor, "bad_ascii_arr"));
  std::array<TypeParam, 4> bad_out{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, Traits::ReadArray(&bad_cursor, bad_out.data(),
                                  coda_array_ordering_c));
  EXPECT_NE(CODA_SUCCESS, coda_errno);
}

TEST(CodaMemCursorTest, RecordNavigationAndUnionFieldsCoverMemRecordAndData) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<ScopedType> owned_types;
  ScopedProduct product = MakeSyntheticMemProduct();
  ASSERT_NE(nullptr, product.get());

  // 1. Build a root coda_mem_record with:
  //    [0] int32 (42), [1] optional NULL field (no-data), [2] int32 (99),
  //    [3] nested empty union, [4] nested populated union (field 1 = 777),
  //    [5] tag_mem_data wrapping a binary union (used for both
  //        goto_record_field_by_index and goto_next_record_field in
  //        coda-mem-cursor.c:80,112 without triggering upstream bug in
  //        coda-ascbin-cursor.c:51 on non-union tag_mem_data records).
  coda_type_number* i32_sp3 =
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_int32, 32, owned_types);
  ASSERT_NE(nullptr, i32_sp3);

  auto* root_def = coda_type_record_new(coda_format_binary);
  ASSERT_NE(nullptr, root_def);
  TrackType(root_def, owned_types);

  auto add_def_field = [&](coda_type_record* rec, const char* name,
                           coda_type* ftype, bool optional) -> bool {
    ScopedRecordField f(coda_type_record_field_new(name));
    if (!f) {
      return false;
    }
    if (optional &&
        coda_type_record_field_set_optional(f.get()) != CODA_SUCCESS) {
      return false;
    }
    if (coda_type_record_field_set_type(f.get(), ftype) != CODA_SUCCESS) {
      return false;
    }
    if (coda_type_record_add_field(rec, f.get()) != CODA_SUCCESS) {
      return false;
    }
    f.release();
    return true;
  };

  ASSERT_TRUE(add_def_field(root_def, "f0",
                            reinterpret_cast<coda_type*>(i32_sp3), false));
  ASSERT_TRUE(add_def_field(root_def, "f1_opt",
                            reinterpret_cast<coda_type*>(i32_sp3), true));
  ASSERT_TRUE(add_def_field(root_def, "f2",
                            reinterpret_cast<coda_type*>(i32_sp3), false));

  // Empty union (tag_mem_record, all fields NULL).
  auto* empty_union_def = coda_type_union_new(coda_format_binary);
  ASSERT_NE(nullptr, empty_union_def);
  TrackType(empty_union_def, owned_types);
  ASSERT_TRUE(add_def_field(empty_union_def, "u0",
                            reinterpret_cast<coda_type*>(i32_sp3), true));
  ASSERT_TRUE(add_def_field(empty_union_def, "u1",
                            reinterpret_cast<coda_type*>(i32_sp3), true));
  ASSERT_TRUE(add_def_field(root_def, "empty_union",
                            reinterpret_cast<coda_type*>(empty_union_def),
                            false));

  // Populated union (tag_mem_record, only field 1 populated).
  auto* pop_union_def = coda_type_union_new(coda_format_binary);
  ASSERT_NE(nullptr, pop_union_def);
  TrackType(pop_union_def, owned_types);
  ASSERT_TRUE(add_def_field(pop_union_def, "u0",
                            reinterpret_cast<coda_type*>(i32_sp3), true));
  ASSERT_TRUE(add_def_field(pop_union_def, "u1",
                            reinterpret_cast<coda_type*>(i32_sp3), true));
  ASSERT_TRUE(add_def_field(root_def, "pop_union",
                            reinterpret_cast<coda_type*>(pop_union_def),
                            false));

  // Binary union wrapped in tag_mem_data with union_field_expr = "1".
  coda_type_number* i32_bin =
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_int32, 32, owned_types);
  coda_type_number* i16_bin =
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_int16, 16, owned_types);
  ASSERT_NE(nullptr, i32_bin);
  ASSERT_NE(nullptr, i16_bin);
  auto* bin_union_def = coda_type_union_new(coda_format_binary);
  ASSERT_NE(nullptr, bin_union_def);
  TrackType(bin_union_def, owned_types);
  ASSERT_TRUE(add_def_field(bin_union_def, "u_i16",
                            reinterpret_cast<coda_type*>(i16_bin), true));
  ASSERT_TRUE(add_def_field(bin_union_def, "u_i32",
                            reinterpret_cast<coda_type*>(i32_bin), true));
  ScopedExpr union_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("1", union_expr.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_type_union_set_field_expression(
                              bin_union_def, union_expr.get()));
  union_expr.release();
  ASSERT_TRUE(add_def_field(root_def, "bin_union",
                            reinterpret_cast<coda_type*>(bin_union_def),
                            false));

  // Build runtime memory nodes.
  ScopedDynamicType root_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_record_new(root_def, nullptr)));
  ASSERT_NE(nullptr, root_dyn.get());
  auto* root = reinterpret_cast<coda_mem_record*>(root_dyn.get());

  ScopedDynamicType f0(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_int32_new(i32_sp3, nullptr, product.get(), 42)));
  ScopedDynamicType f2(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_int32_new(i32_sp3, nullptr, product.get(), 99)));
  ScopedDynamicType empty_union(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_record_new(empty_union_def, nullptr)));
  ScopedDynamicType pop_union(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_record_new(pop_union_def, nullptr)));
  ScopedDynamicType pop_u1(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_int32_new(i32_sp3, nullptr, product.get(), 777)));
  ASSERT_NE(nullptr, f0.get());
  ASSERT_NE(nullptr, f2.get());
  ASSERT_NE(nullptr, empty_union.get());
  ASSERT_NE(nullptr, pop_union.get());
  ASSERT_NE(nullptr, pop_u1.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(
                reinterpret_cast<coda_mem_record*>(pop_union.get()), "u1",
                pop_u1.get(), 0));
  pop_u1.release();

  std::array<uint8_t, 4> bin_union_bytes{};
  const int32_t bin_u_val = 98765;
  std::memcpy(bin_union_bytes.data(), &bin_u_val, sizeof(bin_u_val));
  ScopedDynamicType bin_union(
      reinterpret_cast<coda_dynamic_type*>(coda_mem_data_new(
          reinterpret_cast<coda_type*>(bin_union_def), nullptr, product.get(),
          static_cast<long>(bin_union_bytes.size()), bin_union_bytes.data())));
  ASSERT_NE(nullptr, bin_union.get());

  ASSERT_EQ(CODA_SUCCESS, coda_mem_record_add_field(root, "f0", f0.get(), 0));
  f0.release();
  ASSERT_EQ(CODA_SUCCESS, coda_mem_record_add_field(root, "f2", f2.get(), 0));
  f2.release();
  ASSERT_EQ(CODA_SUCCESS, coda_mem_record_add_field(root, "empty_union",
                                                    empty_union.get(), 0));
  empty_union.release();
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(root, "pop_union", pop_union.get(), 0));
  pop_union.release();
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(root, "bin_union", bin_union.get(), 0));
  bin_union.release();

  product.get()->root_type = root_dyn.release();

  coda_cursor root_cursor_for_num;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_set_product(&root_cursor_for_num, product.get()));
  (void)root_cursor_for_num;
  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  // 2. Check record field count, bit size, and availability queries.
  long num_elements = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements));
  EXPECT_EQ(6, num_elements);

  int64_t bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(-1, bit_size);

  int available = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &cursor, 0, &available));
  EXPECT_EQ(1, available);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &cursor, 1, &available));
  EXPECT_EQ(0, available);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_record_field_available_status(&cursor, -1,
                                                              &available));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_record_field_available_status(&cursor, 6,
                                                              &available));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  // 3. Navigate fields via index and goto_next_record_field (including NULL
  //    optional field -> coda_special_no_data singleton and out-of-range
  //    errors).
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&cursor, -1));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&cursor, 6));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&cursor, 0));
  int32_t v32 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v32));
  EXPECT_EQ(42, v32);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  coda_type_class tc = coda_record_class;
  coda_special_type st = coda_special_time;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &tc));
  EXPECT_EQ(coda_special_class, tc);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&cursor, &st));
  EXPECT_EQ(coda_special_no_data, st);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v32));
  EXPECT_EQ(99, v32);

  // 4. Test empty union (error) and populated union (index 1).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  long union_idx = -1;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_get_available_union_field_index(&cursor, &union_idx));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_available_union_field(&cursor));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_available_union_field_index(&cursor, &union_idx));
  EXPECT_EQ(1, union_idx);
  {
    coda_cursor union_cursor = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_available_union_field(&union_cursor));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&union_cursor, &v32));
    EXPECT_EQ(777, v32);
  }

  // 5. Test tag_mem_data wrapping a binary union (including
  //    goto_record_field_by_index + goto_next_record_field through
  //    tag_mem_data) + goto_next_record_field bounds errors.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &cursor, 1, &available));
  EXPECT_EQ(1, available);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_available_union_field_index(&cursor, &union_idx));
  EXPECT_EQ(1, union_idx);
  {
    coda_cursor bin_union_cursor = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_index(&bin_union_cursor, 0));
    coda_special_type u0_special = coda_special_time;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_special_type(&bin_union_cursor, &u0_special));
    EXPECT_EQ(coda_special_no_data, u0_special);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_next_record_field(&bin_union_cursor));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&bin_union_cursor, &v32));
    EXPECT_EQ(98765, v32);

    bin_union_cursor = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_available_union_field(&bin_union_cursor));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&bin_union_cursor, &v32));
    EXPECT_EQ(98765, v32);
  }

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  // White-box helper mutating top cursor index (unmangled via CODA_INTERNAL)
  // to test negative-index rejection guards unreachable via the public API.
  auto set_top_index = [](coda_cursor* c, long index) {
    c->stack[c->n - 1].index = index;
  };
  set_top_index(&cursor, -2);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_cursor_goto_next_record_field(&cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);
}

TEST(CodaMemCursorTest, ArrayNavigationAndDimCoverFixedVariableAndMemData) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<ScopedType> owned_types;
  ScopedProduct product = MakeSyntheticMemProduct();
  ASSERT_NE(nullptr, product.get());

  coda_type_number* i32_sp3 =
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_int32, 32, owned_types);
  coda_type_number* i16_bin =
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_int16, 16, owned_types);
  ASSERT_NE(nullptr, i32_sp3);
  ASSERT_NE(nullptr, i16_bin);

  // 1. Fixed 1D tag_mem_array of length 3 where element[1] is left NULL.
  auto* fixed_arr_def = coda_type_array_new(coda_format_binary);
  ASSERT_NE(nullptr, fixed_arr_def);
  TrackType(fixed_arr_def, owned_types);
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_add_fixed_dimension(fixed_arr_def, 3));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_set_base_type(
                fixed_arr_def, reinterpret_cast<coda_type*>(i32_sp3)));

  ScopedDynamicType fixed_arr_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_array_new(fixed_arr_def, nullptr)));
  ASSERT_NE(nullptr, fixed_arr_dyn.get());
  auto* fixed_arr = reinterpret_cast<coda_mem_array*>(fixed_arr_dyn.get());
  ScopedDynamicType e0(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_int32_new(i32_sp3, nullptr, product.get(), 11)));
  ScopedDynamicType e2(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_int32_new(i32_sp3, nullptr, product.get(), 33)));
  ASSERT_NE(nullptr, e0.get());
  ASSERT_NE(nullptr, e2.get());
  ASSERT_EQ(CODA_SUCCESS, coda_mem_array_set_element(fixed_arr, 0, e0.get()));
  e0.release();
  ASSERT_EQ(CODA_SUCCESS, coda_mem_array_set_element(fixed_arr, 2, e2.get()));
  e2.release();

  // 2. Variable-dimension tag_mem_array with 2 elements added dynamically
  //    on a coda_format_sp3 product (binary arrays require a non-NULL
  //    dim_expr in coda_type_array_add_variable_dimension).
  ScopedProduct var_product = MakeSyntheticMemProduct();
  ASSERT_NE(nullptr, var_product.get());
  coda_type_number* i32_sp3_var =
      MakeNumberType(coda_format_sp3, coda_integer_class,
                     coda_native_type_int32, 32, owned_types);
  ASSERT_NE(nullptr, i32_sp3_var);
  auto* var_arr_def = coda_type_array_new(coda_format_sp3);
  ASSERT_NE(nullptr, var_arr_def);
  TrackType(var_arr_def, owned_types);
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_add_variable_dimension(var_arr_def, nullptr));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_set_base_type(
                var_arr_def, reinterpret_cast<coda_type*>(i32_sp3_var)));

  ScopedDynamicType var_arr_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_array_new(var_arr_def, nullptr)));
  ASSERT_NE(nullptr, var_arr_dyn.get());
  auto* var_arr = reinterpret_cast<coda_mem_array*>(var_arr_dyn.get());
  for (int32_t v : {101, 202}) {
    ScopedDynamicType el(reinterpret_cast<coda_dynamic_type*>(
        coda_mem_int32_new(i32_sp3_var, nullptr, var_product.get(), v)));
    ASSERT_NE(nullptr, el.get());
    ASSERT_EQ(CODA_SUCCESS, coda_mem_array_add_element(var_arr, el.get()));
    el.release();
  }
  var_product.get()->root_type = var_arr_dyn.release();

  // 3. Binary 2x2 array wrapped in tag_mem_data.
  auto* bin_arr_def = coda_type_array_new(coda_format_binary);
  ASSERT_NE(nullptr, bin_arr_def);
  TrackType(bin_arr_def, owned_types);
  ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(bin_arr_def, 2));
  ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(bin_arr_def, 2));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_set_base_type(
                bin_arr_def, reinterpret_cast<coda_type*>(i16_bin)));
  const std::array<int16_t, 4> bin_vals = {10, 20, 30, 40};
  std::array<uint8_t, 8> bin_bytes{};
  std::memcpy(bin_bytes.data(), bin_vals.data(), bin_bytes.size());
  ScopedDynamicType bin_arr_dyn(
      reinterpret_cast<coda_dynamic_type*>(coda_mem_data_new(
          reinterpret_cast<coda_type*>(bin_arr_def), nullptr, product.get(),
          static_cast<long>(bin_bytes.size()), bin_bytes.data())));
  ASSERT_NE(nullptr, bin_arr_dyn.get());

  auto* root_def = coda_type_record_new(coda_format_binary);
  ASSERT_NE(nullptr, root_def);
  TrackType(root_def, owned_types);
  ScopedDynamicType root_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_record_new(root_def, nullptr)));
  ASSERT_NE(nullptr, root_dyn.get());
  auto* root = reinterpret_cast<coda_mem_record*>(root_dyn.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(root, "fixed", fixed_arr_dyn.get(), 1));
  fixed_arr_dyn.release();
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(root, "bin", bin_arr_dyn.get(), 1));
  bin_arr_dyn.release();
  product.get()->root_type = root_dyn.release();

  coda_cursor root_cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root_cursor, product.get()));

  // Check fixed tag_mem_array.
  coda_cursor arr_cursor = root_cursor;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&arr_cursor, "fixed"));

  long num_elements = 0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&arr_cursor, &num_elements));
  EXPECT_EQ(3, num_elements);
  int64_t bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&arr_cursor, &bit_size));
  EXPECT_EQ(-1, bit_size);

  int num_dims = 0;
  long dim[CODA_MAX_NUM_DIMS]{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&arr_cursor, &num_dims, dim));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(3, dim[0]);

  const long bad_subs[2] = {0, 0};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&arr_cursor, 2, bad_subs));
  EXPECT_EQ(CODA_ERROR_ARRAY_NUM_DIMS_MISMATCH, coda_errno);

  const long neg_sub[1] = {-1};
  const long high_sub[1] = {3};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&arr_cursor, 1, neg_sub));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&arr_cursor, 1, high_sub));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&arr_cursor, -1));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&arr_cursor, 3));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  // Navigate elements 0 -> 1 (NULL -> no_data) -> 2 -> out-of-bounds.
  {
    coda_cursor elem = arr_cursor;
    const long sub0[1] = {0};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&elem, 1, sub0));
    int32_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&elem, &v));
    EXPECT_EQ(11, v);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
    coda_special_type st = coda_special_time;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&elem, &st));
    EXPECT_EQ(coda_special_no_data, st);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&elem, &v));
    EXPECT_EQ(33, v);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&elem));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    // White-box negative index reaches bounds error unreachable via public API.
    elem.stack[elem.n - 1].index = -2;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_mem_cursor_goto_next_array_element(&elem));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  }

  // Test with boundary checks disabled (saved and restored with absl::Cleanup).
  {
    const int saved_checks = coda_option_perform_boundary_checks;
    absl::Cleanup restore_checks = [saved_checks] {
      coda_option_perform_boundary_checks = saved_checks;
    };
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));

    coda_cursor c1 = arr_cursor;
    const long sub2[1] = {2};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&c1, 1, sub2));
    int32_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c1, &v));
    EXPECT_EQ(33, v);

    coda_cursor c2 = arr_cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c2, 0));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c2, &v));
    EXPECT_EQ(11, v);
  }

  // Check variable-dimension tag_mem_array.
  coda_cursor var_cursor;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_set_product(&var_cursor, var_product.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&var_cursor, &num_dims, dim));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(2, dim[0]);

  // Check tag_mem_data wrapping a binary 2x2 array.
  coda_cursor bin_cursor = root_cursor;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&bin_cursor, "bin"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&bin_cursor, &num_dims, dim));
  EXPECT_EQ(2, num_dims);
  EXPECT_EQ(2, dim[0]);
  EXPECT_EQ(2, dim[1]);

  {
    coda_cursor c = bin_cursor;
    const long subs[2] = {1, 0};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&c, 2, subs));
    int16_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &v));
    EXPECT_EQ(30, v);
  }
  {
    coda_cursor c = bin_cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 1));
    int16_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &v));
    EXPECT_EQ(20, v);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &v));
    EXPECT_EQ(30, v);
  }
}

TEST(CodaMemCursorTest, AttributesSpecialTypesStringLengthAndBitSize) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<ScopedType> owned_types;
  ScopedProduct product = MakeSyntheticMemProduct();
  ASSERT_NE(nullptr, product.get());

  // 1. Non-null attributes record with 1 string attribute ("unit" = "m").
  coda_type_text* unit_def = MakeStringType(coda_format_ascii, -1, owned_types);
  auto* attr_rec_def = coda_type_record_new(coda_format_binary);
  ASSERT_NE(nullptr, unit_def);
  ASSERT_NE(nullptr, attr_rec_def);
  TrackType(attr_rec_def, owned_types);
  ScopedDynamicType attr_rec_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_record_new(attr_rec_def, nullptr)));
  ASSERT_NE(nullptr, attr_rec_dyn.get());
  ScopedDynamicType unit_val(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_string_new(unit_def, nullptr, product.get(), "m")));
  ASSERT_NE(nullptr, unit_val.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(
                reinterpret_cast<coda_mem_record*>(attr_rec_dyn.get()), "unit",
                unit_val.get(), 1));
  unit_val.release();

  // 2. Special type (tag_mem_special: time wrapping ascii string) with attrs.
  coda_type_text* time_str_def =
      MakeStringType(coda_format_ascii, -1, owned_types);
  ASSERT_NE(nullptr, time_str_def);
  ScopedExpr time_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("time(str(.),\"yyyy-MM-dd\")",
                                        time_expr.OutParam()));
  auto* time_def = coda_type_time_new(coda_format_binary, time_expr.get());
  ASSERT_NE(nullptr, time_def);
  time_expr.release();
  TrackType(time_def, owned_types);
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_time_set_base_type(
                time_def, reinterpret_cast<coda_type*>(time_str_def)));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_attributes(reinterpret_cast<coda_type*>(time_def),
                                     attr_rec_def));

  ScopedDynamicType time_str_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_string_new(time_str_def, nullptr, product.get(), "2026-03-15")));
  ASSERT_NE(nullptr, time_str_dyn.get());
  ScopedDynamicType time_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_time_new(time_def, attr_rec_dyn.get(), time_str_dyn.get())));
  ASSERT_NE(nullptr, time_dyn.get());
  attr_rec_dyn.release();
  time_str_dyn.release();

  // 3. Special type (tag_mem_special) wrapping binary raw bytes for
  //    read_bits/read_bytes through tag_mem_special.
  auto* raw_bin_def = coda_type_raw_new(coda_format_binary);
  ASSERT_NE(nullptr, raw_bin_def);
  TrackType(raw_bin_def, owned_types);
  ScopedExpr dummy_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("0", dummy_expr.OutParam()));
  auto* raw_time_def = coda_type_time_new(coda_format_binary, dummy_expr.get());
  ASSERT_NE(nullptr, raw_time_def);
  dummy_expr.release();
  TrackType(raw_time_def, owned_types);
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_time_set_base_type(
                raw_time_def, reinterpret_cast<coda_type*>(raw_bin_def)));

  constexpr std::array<uint8_t, 4> kRawBytes = {0xAB, 0xCD, 0xEF, 0x12};
  ScopedDynamicType raw_mem_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_raw_new(raw_bin_def, nullptr, product.get(),
                       static_cast<long>(kRawBytes.size()), kRawBytes.data())));
  ASSERT_NE(nullptr, raw_mem_dyn.get());
  ScopedDynamicType raw_special_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_time_new(raw_time_def, nullptr, raw_mem_dyn.get())));
  ASSERT_NE(nullptr, raw_special_dyn.get());
  raw_mem_dyn.release();

  // 4. tag_mem_data wrapping a binary coda_type_complex (tag_mem_data special).
  coda_type_number* f64_bin =
      MakeNumberType(coda_format_binary, coda_real_class,
                     coda_native_type_double, 64, owned_types);
  ASSERT_NE(nullptr, f64_bin);
  auto* complex_def = coda_type_complex_new(coda_format_binary);
  ASSERT_NE(nullptr, complex_def);
  TrackType(complex_def, owned_types);
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_complex_set_type(complex_def,
                                       reinterpret_cast<coda_type*>(f64_bin)));
  const std::array<double, 2> complex_vals = {1.5, -2.5};
  std::array<uint8_t, 16> complex_bytes{};
  std::memcpy(complex_bytes.data(), complex_vals.data(), complex_bytes.size());
  ScopedDynamicType complex_dyn(
      reinterpret_cast<coda_dynamic_type*>(coda_mem_data_new(
          reinterpret_cast<coda_type*>(complex_def), nullptr, product.get(),
          static_cast<long>(complex_bytes.size()), complex_bytes.data())));
  ASSERT_NE(nullptr, complex_dyn.get());

  // 5. Plain ascii string, ascii empty record, negative-size ascii text, and
  //    negative-size binary raw to cover get_bit_size / get_string_length.
  coda_type_text* plain_str_def =
      MakeStringType(coda_format_ascii, -1, owned_types);
  ASSERT_NE(nullptr, plain_str_def);
  ScopedDynamicType plain_str_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_string_new(plain_str_def, nullptr, product.get(), "hello")));

  auto* ascii_rec_def = coda_type_record_new(coda_format_ascii);
  ASSERT_NE(nullptr, ascii_rec_def);
  TrackType(ascii_rec_def, owned_types);
  ScopedDynamicType ascii_rec_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_record_new(ascii_rec_def, nullptr)));

  coda_type_text* neg_ascii_def =
      MakeStringType(coda_format_ascii, -1, owned_types);
  ASSERT_NE(nullptr, neg_ascii_def);
  ScopedExpr neg_expr1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("-1", neg_expr1.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_byte_size_expression(
                reinterpret_cast<coda_type*>(neg_ascii_def), neg_expr1.get()));
  neg_expr1.release();
  ScopedDynamicType neg_ascii_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_string_new(neg_ascii_def, nullptr, product.get(), "abc")));

  auto* neg_raw_def = coda_type_raw_new(coda_format_binary);
  ASSERT_NE(nullptr, neg_raw_def);
  TrackType(neg_raw_def, owned_types);
  ScopedExpr neg_expr2;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("-1", neg_expr2.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_byte_size_expression(
                reinterpret_cast<coda_type*>(neg_raw_def), neg_expr2.get()));
  neg_expr2.release();
  ScopedDynamicType neg_raw_dyn(
      reinterpret_cast<coda_dynamic_type*>(coda_mem_raw_new(
          neg_raw_def, nullptr, product.get(), 3, kRawBytes.data())));

  auto* dyn_raw_def = coda_type_raw_new(coda_format_binary);
  ASSERT_NE(nullptr, dyn_raw_def);
  TrackType(dyn_raw_def, owned_types);
  ScopedDynamicType dyn_raw_dyn(
      reinterpret_cast<coda_dynamic_type*>(coda_mem_raw_new(
          dyn_raw_def, nullptr, product.get(), 4, kRawBytes.data())));

  auto* root_def = coda_type_record_new(coda_format_binary);
  ASSERT_NE(nullptr, root_def);
  TrackType(root_def, owned_types);
  ScopedDynamicType root_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_record_new(root_def, nullptr)));
  ASSERT_NE(nullptr, root_dyn.get());
  auto* root = reinterpret_cast<coda_mem_record*>(root_dyn.get());

  auto add_field = [&](const char* name, ScopedDynamicType& val) {
    ASSERT_NE(nullptr, val.get());
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(root, name, val.get(), 1));
    val.release();
  };
  ASSERT_NO_FATAL_FAILURE(add_field("time_str", time_dyn));
  ASSERT_NO_FATAL_FAILURE(add_field("raw_special", raw_special_dyn));
  ASSERT_NO_FATAL_FAILURE(add_field("complex_data", complex_dyn));
  ASSERT_NO_FATAL_FAILURE(add_field("plain_str", plain_str_dyn));
  ASSERT_NO_FATAL_FAILURE(add_field("ascii_rec", ascii_rec_dyn));
  ASSERT_NO_FATAL_FAILURE(add_field("neg_ascii", neg_ascii_dyn));
  ASSERT_NO_FATAL_FAILURE(add_field("neg_raw", neg_raw_dyn));
  ASSERT_NO_FATAL_FAILURE(add_field("dyn_raw", dyn_raw_dyn));
  product.get()->root_type = root_dyn.release();

  coda_cursor root_cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root_cursor, product.get()));

  // Attributes == nullptr -> empty record.
  {
    coda_cursor c = root_cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&c));
    long n = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &n));
    EXPECT_EQ(0, n);
  }

  // tag_mem_special with non-null attributes, num_elements, bit_size,
  // read_string, and use_base_type_of_special_type.
  {
    coda_cursor c = root_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c, "time_str"));
    {
      coda_cursor attr_c = c;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr_c));
      long n = -1;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&attr_c, &n));
      EXPECT_EQ(1, n);
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_goto_record_field_by_index(&attr_c, 0));
      char unit_buf[8]{};
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_read_string(&attr_c, unit_buf, sizeof(unit_buf)));
      EXPECT_STREQ("m", unit_buf);
    }

    long n = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &n));
    EXPECT_EQ(1, n);
    int64_t bsize = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bsize));
    EXPECT_EQ(10 * 8, bsize);

    long str_len = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &str_len));
    EXPECT_EQ(10, str_len);
    std::string str_buf(static_cast<size_t>(str_len + 1), '\0');
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&c, str_buf.data(), str_len + 1));
    EXPECT_STREQ("2026-03-15", str_buf.c_str());

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&c));
    coda_type_class tc = coda_special_class;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&c, &tc));
    EXPECT_EQ(coda_text_class, tc);
    std::string base_buf(static_cast<size_t>(str_len + 1), '\0');
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&c, base_buf.data(), str_len + 1));
    EXPECT_STREQ("2026-03-15", base_buf.c_str());
  }

  // tag_mem_special wrapping raw binary bytes -> read_bits and read_bytes.
  {
    coda_cursor c = root_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c, "raw_special"));
    uint8_t bit_byte = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bits(&c, &bit_byte, 4, 8));
    EXPECT_EQ(0xBC, bit_byte);
    std::array<uint8_t, 2> bytes_out{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bytes(&c, bytes_out.data(), 1, 2));
    EXPECT_THAT(bytes_out, ElementsAre(0xCD, 0xEF));
  }

  // tag_mem_data wrapping binary coda_type_special -> use_base_type_of_special.
  {
    coda_cursor c = root_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c, "complex_data"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&c, 0));
    double re = 0.0;
    double im = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &re));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&c));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &im));
    EXPECT_DOUBLE_EQ(1.5, re);
    EXPECT_DOUBLE_EQ(-2.5, im);
  }

  // Plain ASCII string, ASCII record string length (-1), dynamic binary raw
  // bit size (8 * length), and negative size_expr failures.
  {
    coda_cursor c = root_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c, "plain_str"));
    long str_len = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &str_len));
    EXPECT_EQ(5, str_len);
    std::string buf(static_cast<size_t>(str_len + 1), '\0');
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&c, buf.data(), str_len + 1));
    EXPECT_STREQ("hello", buf.c_str());
    long n = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &n));
    EXPECT_EQ(1, n);
  }
  {
    coda_cursor c = root_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c, "ascii_rec"));
    long str_len = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &str_len));
    EXPECT_EQ(-1, str_len);
  }
  {
    coda_cursor c = root_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c, "neg_ascii"));
    int64_t bsize = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_bit_size(&c, &bsize));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
    long str_len = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_string_length(&c, &str_len));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  }
  {
    coda_cursor c = root_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c, "neg_raw"));
    int64_t bsize = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_bit_size(&c, &bsize));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  }
  {
    coda_cursor c = root_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c, "dyn_raw"));
    int64_t bsize = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bsize));
    EXPECT_EQ(32, bsize);
    long n = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &n));
    EXPECT_EQ(1, n);
  }

  // Verify coda_cursor_goto_root() updates offset when root_type itself is
  // tag_mem_data.
  {
    ScopedProduct raw_prod = MakeSyntheticMemProduct();
    ASSERT_NE(nullptr, raw_prod.get());
    ScopedDynamicType raw_root(
        reinterpret_cast<coda_dynamic_type*>(coda_mem_raw_new(
            dyn_raw_def, nullptr, raw_prod.get(),
            static_cast<long>(kRawBytes.size()), kRawBytes.data())));
    ASSERT_NE(nullptr, raw_root.get());
    raw_prod.get()->root_type = raw_root.release();

    coda_cursor rc;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&rc, raw_prod.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&rc));
    std::array<uint8_t, 4> out{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bytes(&rc, out.data(), 0, 4));
    EXPECT_THAT(out, ElementsAre(0xAB, 0xCD, 0xEF, 0x12));
  }
}

TEST(CodaMemCursorTest, ScalarReadsAndRawBitsBytesCoverBinaryAsciiAndXml) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<ScopedType> owned_types;
  ScopedProduct product = MakeSyntheticMemProduct();
  ASSERT_NE(nullptr, product.get());

  auto* root_def = coda_type_record_new(coda_format_binary);
  ASSERT_NE(nullptr, root_def);
  TrackType(root_def, owned_types);
  ScopedDynamicType root_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_record_new(root_def, nullptr)));
  ASSERT_NE(nullptr, root_dyn.get());
  auto* root = reinterpret_cast<coda_mem_record*>(root_dyn.get());

  auto add_mem = [&](const char* name, coda_mem_data* data) {
    ASSERT_NE(nullptr, data);
    ScopedDynamicType dyn(reinterpret_cast<coda_dynamic_type*>(data));
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(root, name, dyn.get(), 1));
    dyn.release();
  };
  auto make_ascii_num = [&](coda_type_class tc, coda_native_type nt,
                            std::string_view text) -> coda_mem_data* {
    coda_type_number* def =
        MakeNumberType(coda_format_ascii, tc, nt, -1, owned_types);
    if (def == nullptr) {
      return nullptr;
    }
    return coda_mem_data_new(reinterpret_cast<coda_type*>(def), nullptr,
                             product.get(), static_cast<long>(text.size()),
                             reinterpret_cast<const uint8_t*>(text.data()));
  };

  // Binary scalars.
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_i8",
      coda_mem_int8_new(MakeNumberType(coda_format_binary, coda_integer_class,
                                       coda_native_type_int8, 8, owned_types),
                        nullptr, product.get(), -42)));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_u8",
      coda_mem_uint8_new(MakeNumberType(coda_format_binary, coda_integer_class,
                                        coda_native_type_uint8, 8, owned_types),
                         nullptr, product.get(), 200)));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_i16", coda_mem_int16_new(
                   MakeNumberType(coda_format_binary, coda_integer_class,
                                  coda_native_type_int16, 16, owned_types),
                   nullptr, product.get(), -12345)));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_u16", coda_mem_uint16_new(
                   MakeNumberType(coda_format_binary, coda_integer_class,
                                  coda_native_type_uint16, 16, owned_types),
                   nullptr, product.get(), 54321)));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_i32", coda_mem_int32_new(
                   MakeNumberType(coda_format_binary, coda_integer_class,
                                  coda_native_type_int32, 32, owned_types),
                   nullptr, product.get(), -1000000)));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_u32", coda_mem_uint32_new(
                   MakeNumberType(coda_format_binary, coda_integer_class,
                                  coda_native_type_uint32, 32, owned_types),
                   nullptr, product.get(), 3000000000U)));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_i64", coda_mem_int64_new(
                   MakeNumberType(coda_format_binary, coda_integer_class,
                                  coda_native_type_int64, 64, owned_types),
                   nullptr, product.get(), -5000000000000LL)));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_u64", coda_mem_uint64_new(
                   MakeNumberType(coda_format_binary, coda_integer_class,
                                  coda_native_type_uint64, 64, owned_types),
                   nullptr, product.get(), 15000000000000ULL)));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_flt", coda_mem_float_new(
                   MakeNumberType(coda_format_binary, coda_real_class,
                                  coda_native_type_float, 32, owned_types),
                   nullptr, product.get(), 3.25f)));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_dbl", coda_mem_double_new(
                   MakeNumberType(coda_format_binary, coda_real_class,
                                  coda_native_type_double, 64, owned_types),
                   nullptr, product.get(), -6.5)));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "b_chr", coda_mem_char_new(MakeCharType(coda_format_binary, owned_types),
                                 nullptr, product.get(), 'B')));

  // ASCII & XML scalars.
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "a_i8",
      make_ascii_num(coda_integer_class, coda_native_type_int8, "-42")));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "a_u8",
      make_ascii_num(coda_integer_class, coda_native_type_uint8, "200")));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "a_i16",
      make_ascii_num(coda_integer_class, coda_native_type_int16, "-12345")));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "a_u16",
      make_ascii_num(coda_integer_class, coda_native_type_uint16, "54321")));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "a_i32",
      make_ascii_num(coda_integer_class, coda_native_type_int32, "-1000000")));
  ASSERT_NO_FATAL_FAILURE(
      add_mem("a_u32", make_ascii_num(coda_integer_class,
                                      coda_native_type_uint32, "3000000000")));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "a_i64", make_ascii_num(coda_integer_class, coda_native_type_int64,
                              "-5000000000000")));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "a_u64", make_ascii_num(coda_integer_class, coda_native_type_uint64,
                              "15000000000000")));
  ASSERT_NO_FATAL_FAILURE(
      add_mem("a_flt",
              make_ascii_num(coda_real_class, coda_native_type_float, "3.25")));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "a_dbl",
      make_ascii_num(coda_real_class, coda_native_type_double, "-6.5")));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "a_chr", coda_mem_char_new(MakeCharType(coda_format_ascii, owned_types),
                                 nullptr, product.get(), 'A')));

  // Raw bytes in binary and ASCII formats (coda-type.c disallows xml fields
  // inside a coda_format_binary record, so XML scalar/raw are checked below
  // on a separate coda_format_xml root record).
  constexpr std::array<uint8_t, 4> kBinBytes = {0x11, 0x22, 0x33, 0x44};
  auto* bin_raw_def = coda_type_raw_new(coda_format_binary);
  ASSERT_NE(nullptr, bin_raw_def);
  TrackType(bin_raw_def, owned_types);
  ASSERT_NO_FATAL_FAILURE(
      add_mem("raw_bin", coda_mem_raw_new(bin_raw_def, nullptr, product.get(),
                                          4, kBinBytes.data())));
  ASSERT_NO_FATAL_FAILURE(add_mem(
      "raw_ascii",
      coda_mem_string_new(MakeStringType(coda_format_ascii, -1, owned_types),
                          nullptr, product.get(), "WXYZ")));

  product.get()->root_type = root_dyn.release();

  coda_cursor root_cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root_cursor, product.get()));

  auto goto_field = [&](const char* name) -> coda_cursor {
    coda_cursor c = root_cursor;
    EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, name));
    return c;
  };

  for (const char* prefix : {"b_", "a_"}) {
    int8_t i8 = 0;
    uint8_t u8 = 0;
    int16_t i16 = 0;
    uint16_t u16 = 0;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    float flt = 0.0f;
    double dbl = 0.0;

    coda_cursor c = goto_field((std::string(prefix) + "i8").c_str());
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&c, &i8));
    EXPECT_EQ(-42, i8);

    c = goto_field((std::string(prefix) + "u8").c_str());
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&c, &u8));
    EXPECT_EQ(200, u8);

    c = goto_field((std::string(prefix) + "i16").c_str());
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &i16));
    EXPECT_EQ(-12345, i16);

    c = goto_field((std::string(prefix) + "u16").c_str());
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&c, &u16));
    EXPECT_EQ(54321, u16);

    c = goto_field((std::string(prefix) + "i32").c_str());
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &i32));
    EXPECT_EQ(-1000000, i32);

    c = goto_field((std::string(prefix) + "u32").c_str());
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&c, &u32));
    EXPECT_EQ(3000000000U, u32);

    c = goto_field((std::string(prefix) + "i64").c_str());
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &i64));
    EXPECT_EQ(-5000000000000LL, i64);

    c = goto_field((std::string(prefix) + "u64").c_str());
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&c, &u64));
    EXPECT_EQ(15000000000000ULL, u64);

    c = goto_field((std::string(prefix) + "flt").c_str());
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &flt));
    EXPECT_FLOAT_EQ(3.25f, flt);

    c = goto_field((std::string(prefix) + "dbl").c_str());
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &dbl));
    EXPECT_DOUBLE_EQ(-6.5, dbl);
  }

  char ch = '\0';
  {
    coda_cursor c = goto_field("b_chr");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&c, &ch));
    EXPECT_EQ('B', ch);
    c = goto_field("a_chr");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&c, &ch));
    EXPECT_EQ('A', ch);
  }

  // Raw bits and bytes across binary and ASCII, plus record error path.
  {
    uint8_t bit_val = 0;
    std::array<uint8_t, 2> two_bytes{};

    coda_cursor c = goto_field("raw_bin");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bits(&c, &bit_val, 8, 8));
    EXPECT_EQ(0x22, bit_val);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bytes(&c, two_bytes.data(), 1, 2));
    EXPECT_THAT(two_bytes, ElementsAre(0x22, 0x33));

    c = goto_field("raw_ascii");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bits(&c, &bit_val, 8, 8));
    EXPECT_EQ('X', bit_val);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bytes(&c, two_bytes.data(), 1, 2));
    EXPECT_THAT(two_bytes, ElementsAre('X', 'Y'));

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_bits(&root_cursor, &bit_val, 0, 8));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_bytes(&root_cursor, two_bytes.data(), 0, 2));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  }

  // XML char and raw bytes on a coda_format_xml product.
  {
    ScopedProduct xml_product = MakeSyntheticMemProduct();
    ASSERT_NE(nullptr, xml_product.get());
    auto* xml_root_def = coda_type_record_new(coda_format_xml);
    ASSERT_NE(nullptr, xml_root_def);
    TrackType(xml_root_def, owned_types);
    ScopedDynamicType xml_root_dyn(reinterpret_cast<coda_dynamic_type*>(
        coda_mem_record_new(xml_root_def, nullptr)));
    ASSERT_NE(nullptr, xml_root_dyn.get());
    auto* xml_root = reinterpret_cast<coda_mem_record*>(xml_root_dyn.get());

    ScopedDynamicType x_chr(reinterpret_cast<coda_dynamic_type*>(
        coda_mem_char_new(MakeCharType(coda_format_xml, owned_types), nullptr,
                          xml_product.get(), 'X')));
    ScopedDynamicType raw_xml(reinterpret_cast<coda_dynamic_type*>(
        coda_mem_string_new(MakeStringType(coda_format_xml, -1, owned_types),
                            nullptr, xml_product.get(), "XML!")));
    ASSERT_NE(nullptr, x_chr.get());
    ASSERT_NE(nullptr, raw_xml.get());
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(xml_root, "x_chr", x_chr.get(), 1));
    x_chr.release();
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(xml_root, "raw_xml", raw_xml.get(), 1));
    raw_xml.release();
    xml_product.get()->root_type = xml_root_dyn.release();

    coda_cursor xml_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_set_product(&xml_cursor, xml_product.get()));
    coda_cursor c = xml_cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, "x_chr"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&c, &ch));
    EXPECT_EQ('X', ch);

    c = xml_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c, "raw_xml"));
    std::array<uint8_t, 2> two_bytes{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bytes(&c, two_bytes.data(), 0, 2));
    EXPECT_THAT(two_bytes, ElementsAre('X', 'M'));
  }
}

// TODO(schwehr): 97.6% line coverage in libcoda/coda-mem-cursor.c (644/660);
// remaining 16 missed lines are defensive default/type-class guards on internal
// memory nodes.

}  // namespace
}  // namespace stcorp_coda
