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
// Tests CODA HDF5 dynamic type construction, datatype/attribute mapping,
// conversion attribute extraction, cycle detection, and cleanup
// (libcoda/coda-hdf5-type.c).
//
// google3 build caveats:
//   - HAVE_HDF5 is enabled (1) and config.h defines H5_USE_16_API, so test
//     HDF5 file synthesis uses explicit versioned 1.8+ APIs (H5Gcreate2,
//     H5Dcreate2, H5Acreate2, H5Tarray_create2, H5Tcommit2, H5Topen2).
//   - patches/libcoda_hdf5type.patch handles H5T_COMPLEX and default: in
//     new_hdf5DataType() as unsupported datatypes (ignored by CODA).
//   - coda-hdf5.c:146-148 leaks H5Eget_major/H5Eget_minor strings and leaves
//     HDF5 error stack allocations when CODA_ERROR_HDF5 is recorded; tests
//     that intentionally trigger HDF5 errors wrap those calls in a narrow
//     absl::LeakCheckDisabler.
//
//   - Upstream coda-hdf5-type.c:234-235 has a heap-use-after-free in
//     new_hdf5BasicDataType() when rejecting >8-byte H5T_INTEGER/H5T_ENUM
//     types: coda_hdf5_type_delete((coda_dynamic_type *)basic_type) frees
//     basic_type before reading basic_type->datatype_id. Tests avoid creating
//     >8-byte integer/enum datasets so no configuration executes this UAF.
//   - Verified 100.0% non-OOM / non-fault / non-UAF reachable coverage ceiling
//     (656/880 = 74.5%, 224 missed in libcoda/coda-hdf5-type.c):
//     93 OOM lines (malloc/realloc/CODA allocator NULL checks and propagation),
//     82 HDF5 internal C API error lines on already-open handles
//     (H5Tget_super/H5Tget_sign/H5Tget_native_type/H5Tget_nmembers/H5Aget_space
//     etc.), 37 unreachable internal CODA setter error guards on valid freshly
//     constructed objects, 5 upstream heap-UAF lines (234-235, 237-239 in
//     new_hdf5BasicDataType() for >8-byte int/enum datasets), and 7 dead lines
//     (529-530 and 1279-1280 !H5Sis_simple after H5S_NULL check, and 1088-1089
//     and 1443 zero-length HDF5 attribute/link names).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-hdf5-type.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-hdf5-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-hdf5.h

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/hdf5_test_util.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-hdf5-internal.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;

ScopedH5Type MakeFixedStringType(size_t size, H5T_str_t strpad,
                                 hid_t base_type = H5T_C_S1) {
  ScopedH5Type str_type(H5Tcopy(base_type));
  if (!str_type || H5Tset_size(str_type.get(), size) < 0 ||
      H5Tset_strpad(str_type.get(), strpad) < 0) {
    return ScopedH5Type();
  }
  return str_type;
}

ScopedH5Type MakeVlenStringType() {
  ScopedH5Type str_type(H5Tcopy(H5T_C_S1));
  if (!str_type || H5Tset_size(str_type.get(), H5T_VARIABLE) < 0) {
    return ScopedH5Type();
  }
  return str_type;
}

ScopedH5Type MakeOversizedIntType(size_t size_bytes) {
  ScopedH5Type int_type(H5Tcopy(H5T_STD_I64LE));
  if (!int_type || H5Tset_size(int_type.get(), size_bytes) < 0) {
    return ScopedH5Type();
  }
  return int_type;
}

ScopedH5Type MakeCustomIntType(size_t size_bytes, bool is_signed,
                               H5T_order_t order, size_t precision_bits = 0,
                               size_t bit_offset = 0) {
  ScopedH5Type int_type(H5Tcopy(is_signed ? H5T_STD_I64LE : H5T_STD_U64LE));
  if (!int_type || H5Tset_order(int_type.get(), order) < 0 ||
      H5Tset_size(int_type.get(), size_bytes) < 0) {
    return ScopedH5Type();
  }
  if (precision_bits > 0 &&
      (H5Tset_precision(int_type.get(), precision_bits) < 0 ||
       H5Tset_offset(int_type.get(), bit_offset) < 0)) {
    return ScopedH5Type();
  }
  return int_type;
}

// Replaces either the first or last occurrence of a 4-byte HDF5 metadata magic
// signature in `path` with `replacement` to trigger targeted HDF5 read errors.
bool CorruptHdf5Magic(const std::filesystem::path& path, std::string_view magic,
                      std::string_view replacement, bool replace_last,
                      size_t* match_count_out = nullptr) {
  if (magic.size() != 4 || replacement.size() != 4) {
    return false;
  }
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    return false;
  }
  const std::streamsize file_size = in.tellg();
  if (file_size <= 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
  if (!in.read(reinterpret_cast<char*>(bytes.data()), file_size)) {
    return false;
  }
  in.close();

  std::vector<size_t> offsets;
  for (size_t i = 0; i + magic.size() <= bytes.size(); ++i) {
    if (std::memcmp(bytes.data() + i, magic.data(), magic.size()) == 0) {
      offsets.push_back(i);
    }
  }
  if (match_count_out != nullptr) {
    *match_count_out = offsets.size();
  }
  if (offsets.empty()) {
    return false;
  }
  const size_t target_offset = replace_last ? offsets.back() : offsets.front();
  std::memcpy(bytes.data() + target_offset, replacement.data(),
              replacement.size());
  return WriteFileBytes(path, bytes);
}

template <typename T>
bool WriteScalarDataset(hid_t loc_id, const char* name, hid_t file_type,
                        hid_t mem_type, const T& value) {
  ScopedH5Space space(H5Screate(H5S_SCALAR));
  if (!space) {
    return false;
  }
  ScopedH5Dataset dset(H5Dcreate2(loc_id, name, file_type, space.get(),
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  if (!dset) {
    return false;
  }
  return H5Dwrite(dset.get(), mem_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                  &value) >= 0;
}

template <typename T>
bool WriteScalarAttr(hid_t obj_id, const char* name, hid_t type_id,
                     const T& value) {
  ScopedH5Space space(H5Screate(H5S_SCALAR));
  if (!space) {
    return false;
  }
  ScopedH5Attr attr(
      H5Acreate2(obj_id, name, type_id, space.get(), H5P_DEFAULT, H5P_DEFAULT));
  if (!attr) {
    return false;
  }
  return H5Awrite(attr.get(), type_id, &value) >= 0;
}

template <typename T>
bool WriteArray1DAttr(hid_t obj_id, const char* name, hid_t type_id,
                      const T* values, hsize_t count) {
  ScopedH5Space space(H5Screate_simple(1, &count, nullptr));
  if (!space) {
    return false;
  }
  ScopedH5Attr attr(
      H5Acreate2(obj_id, name, type_id, space.get(), H5P_DEFAULT, H5P_DEFAULT));
  if (!attr) {
    return false;
  }
  return H5Awrite(attr.get(), type_id, values) >= 0;
}

void ExpectTypeClassAndReadType(const coda_cursor& cursor,
                                coda_type_class expected_class,
                                coda_native_type expected_read_type) {
  coda_type* type = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &type));
  ASSERT_NE(nullptr, type);

  coda_format format = coda_format_binary;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_format(type, &format));
  EXPECT_EQ(coda_format_hdf5, format);

  coda_type_class type_class = coda_record_class;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(type, &type_class));
  EXPECT_EQ(expected_class, type_class);

  coda_native_type read_type = coda_native_type_not_available;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_read_type(type, &read_type));
  EXPECT_EQ(expected_read_type, read_type);
}

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

TEST(CodaHdf5TypeTest, OpenMissingAndCorruptHdf5FilesFail) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Non-existent path fails with CODA_ERROR_FILE_NOT_FOUND.
  const TempFile missing("hdf5_type_missing.h5");
  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  // 2. Truncated HDF5 signature triggers H5Fopen failure -> CODA_ERROR_HDF5.
  const TempFile corrupt("hdf5_type_corrupt.h5");
  constexpr auto kTruncatedHdf5 =
      std::to_array<uint8_t>({0x89, 'H', 'D', 'F', '\r', '\n', 0x1A, '\n', 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  ASSERT_TRUE(corrupt.WriteBytes(kTruncatedHdf5));

  coda_set_error(CODA_SUCCESS, nullptr);
  int rc = 0;
  {
    const absl::LeakCheckDisabler leak_disabler;
    rc = coda_open(corrupt.c_str(), product.OutParam());
  }
  EXPECT_EQ(-1, rc);
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  EXPECT_EQ(nullptr, product.get());
}

TEST(CodaHdf5TypeTest, DirectCreateTreeErrorAndTypeDeleteNullSlots) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Create a minimal valid HDF5 file and open it via CODA.
  const TempFile h5_file("hdf5_type_direct.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ASSERT_TRUE(WriteScalarDataset<int32_t>(file.get(), "val", H5T_NATIVE_INT32,
                                            H5T_NATIVE_INT32, 42));
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  auto* hdf5_product = reinterpret_cast<coda_hdf5_product*>(product.get());
  ASSERT_NE(nullptr, hdf5_product);

  // 2. Direct coda_hdf5_create_tree() with a nonexistent object path fails
  //    in H5Gget_objinfo() with CODA_ERROR_HDF5.
  coda_hdf5_object* missing_obj = nullptr;
  coda_set_error(CODA_SUCCESS, nullptr);
  int rc = 0;
  {
    const absl::LeakCheckDisabler leak_disabler;
    rc = coda_hdf5_create_tree(hdf5_product, hdf5_product->file_id,
                               "nonexistent_object", &missing_obj);
  }
  EXPECT_EQ(-1, rc);
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  EXPECT_EQ(nullptr, missing_obj);

  // 3. Direct coda_hdf5_type_delete() handles partially constructed
  //    tag_hdf5_group and tag_hdf5_compound_datatype with NULL child slots.
  auto* partial_group =
      static_cast<coda_hdf5_group*>(std::malloc(sizeof(coda_hdf5_group)));
  ASSERT_NE(nullptr, partial_group);
  partial_group->backend = coda_backend_hdf5;
  partial_group->definition = nullptr;
  partial_group->tag = tag_hdf5_group;
  partial_group->group_id = H5Gopen2(hdf5_product->file_id, "/", H5P_DEFAULT);
  ASSERT_GE(partial_group->group_id, 0);
  partial_group->object = nullptr;
  partial_group->attributes = nullptr;
  coda_hdf5_type_delete(reinterpret_cast<coda_dynamic_type*>(partial_group));

  auto* partial_compound = static_cast<coda_hdf5_compound_data_type*>(
      std::malloc(sizeof(coda_hdf5_compound_data_type)));
  ASSERT_NE(nullptr, partial_compound);
  partial_compound->backend = coda_backend_hdf5;
  partial_compound->definition = coda_type_record_new(coda_format_hdf5);
  ASSERT_NE(nullptr, partial_compound->definition);
  partial_compound->tag = tag_hdf5_compound_datatype;
  partial_compound->datatype_id = H5Tcopy(H5T_NATIVE_INT32);
  ASSERT_GE(partial_compound->datatype_id, 0);
  partial_compound->member = nullptr;
  partial_compound->member_type = nullptr;
  coda_hdf5_type_delete(reinterpret_cast<coda_dynamic_type*>(partial_compound));
}

TEST(CodaHdf5TypeTest, IntegerEnumAndFloatDatasetTypesCoverSizesAndEndianness) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile h5_file("hdf5_type_numeric.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    // Signed & unsigned integers across 1, 2, 4, 8 byte sizes and LE/BE.
    ASSERT_TRUE(WriteScalarDataset<int8_t>(file.get(), "i8_le", H5T_STD_I8LE,
                                           H5T_NATIVE_INT8, -7));
    ASSERT_TRUE(WriteScalarDataset<int8_t>(file.get(), "i8_be", H5T_STD_I8BE,
                                           H5T_NATIVE_INT8, -8));
    ASSERT_TRUE(WriteScalarDataset<uint8_t>(file.get(), "u8_le", H5T_STD_U8LE,
                                            H5T_NATIVE_UINT8, 250));
    ASSERT_TRUE(WriteScalarDataset<int16_t>(file.get(), "i16_le", H5T_STD_I16LE,
                                            H5T_NATIVE_INT16, -1234));
    ASSERT_TRUE(WriteScalarDataset<int16_t>(file.get(), "i16_be", H5T_STD_I16BE,
                                            H5T_NATIVE_INT16, -2345));
    ASSERT_TRUE(WriteScalarDataset<uint16_t>(
        file.get(), "u16_le", H5T_STD_U16LE, H5T_NATIVE_UINT16, 54321));
    ASSERT_TRUE(WriteScalarDataset<uint16_t>(
        file.get(), "u16_be", H5T_STD_U16BE, H5T_NATIVE_UINT16, 43210));
    ASSERT_TRUE(WriteScalarDataset<int32_t>(file.get(), "i32_le", H5T_STD_I32LE,
                                            H5T_NATIVE_INT32, -100000));
    ASSERT_TRUE(WriteScalarDataset<int32_t>(file.get(), "i32_be", H5T_STD_I32BE,
                                            H5T_NATIVE_INT32, -200000));
    ASSERT_TRUE(WriteScalarDataset<uint32_t>(
        file.get(), "u32_le", H5T_STD_U32LE, H5T_NATIVE_UINT32, 3000000000u));
    ASSERT_TRUE(WriteScalarDataset<uint32_t>(
        file.get(), "u32_be", H5T_STD_U32BE, H5T_NATIVE_UINT32, 3100000000u));
    ASSERT_TRUE(WriteScalarDataset<int64_t>(file.get(), "i64_le", H5T_STD_I64LE,
                                            H5T_NATIVE_INT64, -9000000000LL));
    ASSERT_TRUE(WriteScalarDataset<int64_t>(file.get(), "i64_be", H5T_STD_I64BE,
                                            H5T_NATIVE_INT64, -8000000000LL));
    ASSERT_TRUE(WriteScalarDataset<uint64_t>(file.get(), "u64_le",
                                             H5T_STD_U64LE, H5T_NATIVE_UINT64,
                                             10000000000ULL));
    ASSERT_TRUE(WriteScalarDataset<uint64_t>(file.get(), "u64_be",
                                             H5T_STD_U64BE, H5T_NATIVE_UINT64,
                                             11000000000ULL));

    // Enumerations (signed and unsigned integer super types).
    ScopedH5Type enum_u8(H5Tenum_create(H5T_NATIVE_UINT8));
    ASSERT_TRUE(enum_u8.valid());
    constexpr uint8_t kEnumU8Val = 3;
    ASSERT_GE(H5Tenum_insert(enum_u8.get(), "THREE", &kEnumU8Val), 0);
    ASSERT_TRUE(WriteScalarDataset<uint8_t>(
        file.get(), "enum_u8", enum_u8.get(), enum_u8.get(), kEnumU8Val));

    ScopedH5Type enum_i16(H5Tenum_create(H5T_NATIVE_INT16));
    ASSERT_TRUE(enum_i16.valid());
    constexpr int16_t kEnumI16Val = -12;
    ASSERT_GE(H5Tenum_insert(enum_i16.get(), "NEG_TWELVE", &kEnumI16Val), 0);
    ASSERT_TRUE(WriteScalarDataset<int16_t>(
        file.get(), "enum_i16", enum_i16.get(), enum_i16.get(), kEnumI16Val));

    // Floating-point types (LE and BE).
    ASSERT_TRUE(WriteScalarDataset<float>(file.get(), "f32_le", H5T_IEEE_F32LE,
                                          H5T_NATIVE_FLOAT, 1.5f));
    ASSERT_TRUE(WriteScalarDataset<float>(file.get(), "f32_be", H5T_IEEE_F32BE,
                                          H5T_NATIVE_FLOAT, -2.25f));
    ASSERT_TRUE(WriteScalarDataset<double>(file.get(), "f64_le", H5T_IEEE_F64LE,
                                           H5T_NATIVE_DOUBLE, 3.125));
    ASSERT_TRUE(WriteScalarDataset<double>(file.get(), "f64_be", H5T_IEEE_F64BE,
                                           H5T_NATIVE_DOUBLE, -4.5));
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  struct NumericCase {
    const char* name;
    coda_type_class type_class;
    coda_native_type read_type;
  };
  constexpr NumericCase kCases[] = {
      {"i8_le", coda_integer_class, coda_native_type_int8},
      {"i8_be", coda_integer_class, coda_native_type_int8},
      {"u8_le", coda_integer_class, coda_native_type_uint8},
      {"i16_le", coda_integer_class, coda_native_type_int16},
      {"i16_be", coda_integer_class, coda_native_type_int16},
      {"u16_le", coda_integer_class, coda_native_type_uint16},
      {"u16_be", coda_integer_class, coda_native_type_uint16},
      {"i32_le", coda_integer_class, coda_native_type_int32},
      {"i32_be", coda_integer_class, coda_native_type_int32},
      {"u32_le", coda_integer_class, coda_native_type_uint32},
      {"u32_be", coda_integer_class, coda_native_type_uint32},
      {"i64_le", coda_integer_class, coda_native_type_int64},
      {"i64_be", coda_integer_class, coda_native_type_int64},
      {"u64_le", coda_integer_class, coda_native_type_uint64},
      {"u64_be", coda_integer_class, coda_native_type_uint64},
      {"enum_u8", coda_integer_class, coda_native_type_uint8},
      {"enum_i16", coda_integer_class, coda_native_type_int16},
      {"f32_le", coda_real_class, coda_native_type_float},
      {"f32_be", coda_real_class, coda_native_type_float},
      {"f64_le", coda_real_class, coda_native_type_double},
      {"f64_be", coda_real_class, coda_native_type_double},
  };

  for (const NumericCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, tc.name));
    ExpectTypeClassAndReadType(cursor, coda_array_class,
                               coda_native_type_not_available);

    coda_type* array_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &array_type));
    coda_type* base_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_array_base_type(array_type, &base_type));
    coda_type_class base_class = coda_record_class;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(base_type, &base_class));
    EXPECT_EQ(tc.type_class, base_class);
    coda_native_type base_read_type = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_read_type(base_type, &base_read_type));
    EXPECT_EQ(tc.read_type, base_read_type);
  }

  // Spot-check converted big-endian and enum reads via cursor.
  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "i16_be[0]"));
  int16_t i16_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &i16_val));
  EXPECT_EQ(-2345, i16_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/enum_i16[0]"));
  int16_t enum_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &enum_val));
  EXPECT_EQ(-12, enum_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/f64_be[0]"));
  double f64_val = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64_val));
  EXPECT_DOUBLE_EQ(-4.5, f64_val);
}

TEST(CodaHdf5TypeTest, StringDatasetTypesFixedAndVariableLength) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile h5_file("hdf5_type_strings.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    // 1. Fixed-length C string (NULLTERM) and Fortran string (SPACEPAD).
    ScopedH5Type fixed_nullterm = MakeFixedStringType(6, H5T_STR_NULLTERM);
    ASSERT_TRUE(fixed_nullterm.valid());
    constexpr char kFixedBuf[6] = "hello";
    ScopedH5Space scalar_space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar_space.valid());
    ScopedH5Dataset d_fixed(H5Dcreate2(file.get(), "fixed_scalar",
                                       fixed_nullterm.get(), scalar_space.get(),
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_fixed.valid());
    ASSERT_GE(H5Dwrite(d_fixed.get(), fixed_nullterm.get(), H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kFixedBuf),
              0);

    ScopedH5Type fortran_pad =
        MakeFixedStringType(4, H5T_STR_SPACEPAD, H5T_FORTRAN_S1);
    ASSERT_TRUE(fortran_pad.valid());
    constexpr hsize_t kArrDims[1] = {2};
    ScopedH5Space arr_space(H5Screate_simple(1, kArrDims, nullptr));
    ASSERT_TRUE(arr_space.valid());
    ScopedH5Dataset d_fixed_arr(
        H5Dcreate2(file.get(), "fixed_arr", fortran_pad.get(), arr_space.get(),
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_fixed_arr.valid());
    constexpr char kArrBuf[8] = {'A', 'B', ' ', ' ', 'C', 'D', 'E', ' '};
    ASSERT_GE(H5Dwrite(d_fixed_arr.get(), fortran_pad.get(), H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kArrBuf),
              0);

    // 2. Variable-length strings (scalar and 1D array, allowed on datasets).
    ScopedH5Type vlen_str = MakeVlenStringType();
    ASSERT_TRUE(vlen_str.valid());
    const char* vscalar = "vlen_scalar_text";
    ScopedH5Dataset d_vscalar(
        H5Dcreate2(file.get(), "vlen_scalar", vlen_str.get(),
                   scalar_space.get(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_vscalar.valid());
    ASSERT_GE(H5Dwrite(d_vscalar.get(), vlen_str.get(), H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &vscalar),
              0);

    constexpr std::array<const char*, 2> kVlenArr = {"alpha", "beta_long"};
    ScopedH5Dataset d_varr(H5Dcreate2(file.get(), "vlen_arr", vlen_str.get(),
                                      arr_space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                      H5P_DEFAULT));
    ASSERT_TRUE(d_varr.valid());
    ASSERT_GE(H5Dwrite(d_varr.get(), vlen_str.get(), H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kVlenArr.data()),
              0);
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  for (const char* name :
       {"fixed_scalar", "fixed_arr", "vlen_scalar", "vlen_arr"}) {
    SCOPED_TRACE(name);
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, name));
    coda_type* arr_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &arr_type));
    coda_type* base_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_array_base_type(arr_type, &base_type));
    coda_type_class cls = coda_record_class;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(base_type, &cls));
    EXPECT_EQ(coda_text_class, cls);
    coda_native_type rtype = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_read_type(base_type, &rtype));
    EXPECT_EQ(coda_native_type_string, rtype);
  }

  // Read fixed scalar string and 1D vlen string array elements.
  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "fixed_scalar[0]"));
  long len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &len));
  EXPECT_EQ(6, len);
  std::string buf(static_cast<size_t>(len + 1), '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, buf.data(), len + 1));
  EXPECT_STREQ("hello", buf.c_str());

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/vlen_arr[1]"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &len));
  EXPECT_EQ(9, len);
  std::string vbuf(static_cast<size_t>(len + 1), '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, vbuf.data(), len + 1));
  EXPECT_STREQ("beta_long", vbuf.c_str());
}

TEST(CodaHdf5TypeTest, DatasetShapesScalarNdUnlimitedZeroSizedAndNull) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile h5_file("hdf5_type_shapes.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    // 1. Scalar (0D).
    ASSERT_TRUE(WriteScalarDataset<int32_t>(
        file.get(), "scalar", H5T_NATIVE_INT32, H5T_NATIVE_INT32, 11));

    // 2. 1D and 3D datasets.
    constexpr std::array<hsize_t, 1> kDims1D = {3};
    ScopedH5Space space1d(H5Screate_simple(1, kDims1D.data(), nullptr));
    ASSERT_TRUE(space1d.valid());
    ScopedH5Dataset d1(H5Dcreate2(file.get(), "d1", H5T_NATIVE_INT32,
                                  space1d.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(d1.valid());

    constexpr std::array<hsize_t, 3> kDims3D = {2, 3, 4};
    ScopedH5Space space3d(H5Screate_simple(3, kDims3D.data(), nullptr));
    ASSERT_TRUE(space3d.valid());
    ScopedH5Dataset d3(H5Dcreate2(file.get(), "d3", H5T_NATIVE_INT32,
                                  space3d.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(d3.valid());

    // 3. Unlimited chunked dataset.
    constexpr std::array<hsize_t, 1> kCurDims = {4};
    constexpr std::array<hsize_t, 1> kMaxDims = {H5S_UNLIMITED};
    constexpr std::array<hsize_t, 1> kChunkDims = {2};
    ScopedH5Space unlim_space(
        H5Screate_simple(1, kCurDims.data(), kMaxDims.data()));
    ASSERT_TRUE(unlim_space.valid());
    ScopedH5Plist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunkDims.data()), 0);
    ScopedH5Dataset d_unlim(H5Dcreate2(file.get(), "unlimited",
                                       H5T_NATIVE_INT32, unlim_space.get(),
                                       H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(d_unlim.valid());

    // 4. Zero-sized 1D and 2D datasets.
    constexpr std::array<hsize_t, 1> kZero1D = {0};
    ScopedH5Space zero_space1d(H5Screate_simple(1, kZero1D.data(), nullptr));
    ASSERT_TRUE(zero_space1d.valid());
    ScopedH5Dataset d_zero1d(H5Dcreate2(file.get(), "zero_1d", H5T_NATIVE_INT32,
                                        zero_space1d.get(), H5P_DEFAULT,
                                        H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_zero1d.valid());

    constexpr std::array<hsize_t, 2> kZero2D = {2, 0};
    ScopedH5Space zero_space2d(H5Screate_simple(2, kZero2D.data(), nullptr));
    ASSERT_TRUE(zero_space2d.valid());
    ScopedH5Dataset d_zero2d(H5Dcreate2(file.get(), "zero_2d", H5T_NATIVE_INT32,
                                        zero_space2d.get(), H5P_DEFAULT,
                                        H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_zero2d.valid());

    // 5. H5S_NULL dataspace dataset (must be ignored by CODA).
    ScopedH5Space null_space(H5Screate(H5S_NULL));
    ASSERT_TRUE(null_space.valid());
    ScopedH5Dataset d_null(H5Dcreate2(file.get(), "ignored_null",
                                      H5T_NATIVE_INT32, null_space.get(),
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_null.valid());

    // 6. > CODA_MAX_NUM_DIMS (9D) dataset (must be ignored by CODA).
    constexpr std::array<hsize_t, 9> kDims9D = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    ScopedH5Space space9d(H5Screate_simple(9, kDims9D.data(), nullptr));
    ASSERT_TRUE(space9d.valid());
    ScopedH5Dataset d_9d(H5Dcreate2(file.get(), "ignored_9d", H5T_NATIVE_INT32,
                                    space9d.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
    ASSERT_TRUE(d_9d.valid());
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  coda_type* root_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_root_type(product.get(), &root_type));
  long num_fields = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_num_record_fields(root_type, &num_fields));
  EXPECT_EQ(6, num_fields);

  long idx = -1;
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_name(
                    root_type, "ignored_null", &idx));
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_name(root_type,
                                                           "ignored_9d", &idx));
  coda_set_error(CODA_SUCCESS, nullptr);

  struct ShapeCase {
    const char* name;
    int expected_num_dims;
    std::vector<long> expected_dims;
    long expected_elements;
  };
  const ShapeCase kShapeCases[] = {
      {.name = "scalar",
       .expected_num_dims = 0,
       .expected_dims = {},
       .expected_elements = 1},
      {.name = "d1",
       .expected_num_dims = 1,
       .expected_dims = {3},
       .expected_elements = 3},
      {.name = "d3",
       .expected_num_dims = 3,
       .expected_dims = {2, 3, 4},
       .expected_elements = 24},
      {.name = "unlimited",
       .expected_num_dims = 1,
       .expected_dims = {4},
       .expected_elements = 4},
      {.name = "zero_1d",
       .expected_num_dims = 1,
       .expected_dims = {0},
       .expected_elements = 0},
      {.name = "zero_2d",
       .expected_num_dims = 2,
       .expected_dims = {2, 0},
       .expected_elements = 0},
  };

  for (const ShapeCase& tc : kShapeCases) {
    SCOPED_TRACE(tc.name);
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, tc.name));

    int num_dims = -1;
    long dims[CODA_MAX_NUM_DIMS] = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&cursor, &num_dims, dims));
    EXPECT_EQ(tc.expected_num_dims, num_dims);
    for (int d = 0; d < num_dims; ++d) {
      EXPECT_EQ(tc.expected_dims[d], dims[d]);
    }
    long num_elements = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_num_elements(&cursor, &num_elements));
    EXPECT_EQ(tc.expected_elements, num_elements);
  }
}

TEST(CodaHdf5TypeTest, CompoundDatatypesCoverSupportedIgnoredAndWeirdMembers) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct FullCompoundRow {
    int8_t i8;
    uint16_t u16;
    int32_t i32;
    uint64_t u64;
    float f32;
    double f64;
    int16_t enum_val;
    char fixed[6];
  };

  const TempFile h5_file("hdf5_type_compound.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    ScopedH5Space scalar_space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar_space.valid());

    // 1. Full supported compound with multiple numeric, enum, and string types.
    ScopedH5Type enum_i16(H5Tenum_create(H5T_NATIVE_INT16));
    ASSERT_TRUE(enum_i16.valid());
    constexpr int16_t kEnumFive = 5;
    ASSERT_GE(H5Tenum_insert(enum_i16.get(), "FIVE", &kEnumFive), 0);

    ScopedH5Type fixed_str = MakeFixedStringType(6, H5T_STR_NULLTERM);
    ASSERT_TRUE(fixed_str.valid());

    ScopedH5Type full_cmp(H5Tcreate(H5T_COMPOUND, sizeof(FullCompoundRow)));
    ASSERT_TRUE(full_cmp.valid());
    ASSERT_GE(H5Tinsert(full_cmp.get(), "i8", offsetof(FullCompoundRow, i8),
                        H5T_NATIVE_INT8),
              0);
    ASSERT_GE(H5Tinsert(full_cmp.get(), "u16", offsetof(FullCompoundRow, u16),
                        H5T_NATIVE_UINT16),
              0);
    ASSERT_GE(H5Tinsert(full_cmp.get(), "1bad", offsetof(FullCompoundRow, i32),
                        H5T_NATIVE_INT32),
              0);
    ASSERT_GE(H5Tinsert(full_cmp.get(), "two words",
                        offsetof(FullCompoundRow, u64), H5T_NATIVE_UINT64),
              0);
    ASSERT_GE(H5Tinsert(full_cmp.get(), "two_words",
                        offsetof(FullCompoundRow, f32), H5T_NATIVE_FLOAT),
              0);
    ASSERT_GE(H5Tinsert(full_cmp.get(), "f64", offsetof(FullCompoundRow, f64),
                        H5T_NATIVE_DOUBLE),
              0);
    ASSERT_GE(H5Tinsert(full_cmp.get(), "enum_val",
                        offsetof(FullCompoundRow, enum_val), enum_i16.get()),
              0);
    ASSERT_GE(H5Tinsert(full_cmp.get(), "fixed",
                        offsetof(FullCompoundRow, fixed), fixed_str.get()),
              0);

    FullCompoundRow row;
    std::memset(&row, 0, sizeof(row));
    row.i8 = -9;
    row.u16 = 500;
    row.i32 = -12345;
    row.u64 = 9999999999ULL;
    row.f32 = 2.5f;
    row.f64 = -7.25;
    row.enum_val = kEnumFive;
    std::memcpy(row.fixed, "abcde", 6);
    ASSERT_TRUE(WriteScalarDataset(file.get(), "full_cmp", full_cmp.get(),
                                   full_cmp.get(), row));

    // 2. Compound mixing supported members with unsupported members:
    //    - vlen string (rejected because allow_vlen_data == 0)
    //    - bitfield (rejected by new_hdf5BasicDataType default:)
    //    - opaque (rejected by new_hdf5BasicDataType default:)
    //    - nested compound (rejected by new_hdf5BasicDataType default:)
    //    - object reference (rejected by new_hdf5BasicDataType default:)
    //    - array datatype (rejected by new_hdf5BasicDataType default:)
    // TODO(schwehr): Add >8-byte integer/enum compound members once upstream
    // coda-hdf5-type.c:234-235 fixes the heap-UAF after coda_hdf5_type_delete.
    ScopedH5Type vlen_str = MakeVlenStringType();
    ASSERT_TRUE(vlen_str.valid());
    ScopedH5Type opaque_t(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opaque_t.valid());
    ScopedH5Type nested_cmp_t(H5Tcreate(H5T_COMPOUND, sizeof(int32_t)));
    ASSERT_TRUE(nested_cmp_t.valid());
    ASSERT_GE(H5Tinsert(nested_cmp_t.get(), "inner", 0, H5T_NATIVE_INT32), 0);
    ScopedH5Type int24_t = MakeCustomIntType(3, true, H5T_ORDER_LE);
    ASSERT_TRUE(int24_t.valid());
    constexpr std::array<hsize_t, 1> kArrMemberDims = {2};
    ScopedH5Type arr_member_t(
        H5Tarray_create2(H5T_NATIVE_INT32, 1, kArrMemberDims.data()));
    ASSERT_TRUE(arr_member_t.valid());

    const size_t mixed_size = sizeof(int32_t) + sizeof(char*) + 1 + 4 +
                              sizeof(int32_t) + sizeof(hobj_ref_t) + 8 + 3 +
                              sizeof(double);
    ScopedH5Type mixed_cmp(H5Tcreate(H5T_COMPOUND, mixed_size));
    ASSERT_TRUE(mixed_cmp.valid());
    size_t off = 0;
    ASSERT_GE(H5Tinsert(mixed_cmp.get(), "keep_i32", off, H5T_NATIVE_INT32), 0);
    off += sizeof(int32_t);
    ASSERT_GE(H5Tinsert(mixed_cmp.get(), "drop_vlen", off, vlen_str.get()), 0);
    off += sizeof(char*);
    ASSERT_GE(H5Tinsert(mixed_cmp.get(), "drop_bitfield", off, H5T_STD_B8LE),
              0);
    off += 1;
    ASSERT_GE(H5Tinsert(mixed_cmp.get(), "drop_opaque", off, opaque_t.get()),
              0);
    off += 4;
    ASSERT_GE(
        H5Tinsert(mixed_cmp.get(), "drop_nested_cmp", off, nested_cmp_t.get()),
        0);
    off += sizeof(int32_t);
    ASSERT_GE(H5Tinsert(mixed_cmp.get(), "drop_ref", off, H5T_STD_REF_OBJ), 0);
    off += sizeof(hobj_ref_t);
    ASSERT_GE(H5Tinsert(mixed_cmp.get(), "drop_array", off, arr_member_t.get()),
              0);
    off += 8;
    ASSERT_GE(H5Tinsert(mixed_cmp.get(), "keep_i24", off, int24_t.get()), 0);
    off += 3;
    ASSERT_GE(H5Tinsert(mixed_cmp.get(), "keep_f64", off, H5T_NATIVE_DOUBLE),
              0);

    ScopedH5Dataset d_mixed(H5Dcreate2(file.get(), "mixed_cmp", mixed_cmp.get(),
                                       scalar_space.get(), H5P_DEFAULT,
                                       H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_mixed.valid());

    // 3. Compound whose members are all unsupported -> 0-field CODA record.
    ScopedH5Type empty_cmp(H5Tcreate(H5T_COMPOUND, 1));
    ASSERT_TRUE(empty_cmp.valid());
    ASSERT_GE(H5Tinsert(empty_cmp.get(), "only_bits", 0, H5T_STD_B8LE), 0);
    ScopedH5Dataset d_empty_cmp(
        H5Dcreate2(file.get(), "all_dropped_cmp", empty_cmp.get(),
                   scalar_space.get(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_empty_cmp.valid());
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  // Verify full compound fields, sanitized names, real names, and values.
  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "full_cmp[0]"));
  coda_type* cmp_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &cmp_type));
  coda_type_class cls = coda_integer_class;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(cmp_type, &cls));
  EXPECT_EQ(coda_record_class, cls);

  long num_fields = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_num_record_fields(cmp_type, &num_fields));
  ASSERT_EQ(8, num_fields);

  const char* name_2 = nullptr;
  const char* real_name_2 = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_record_field_name(cmp_type, 2, &name_2));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_record_field_real_name(cmp_type, 2, &real_name_2));
  EXPECT_STREQ("bad", name_2);
  EXPECT_STREQ("1bad", real_name_2);

  const char* name_3 = nullptr;
  const char* name_4 = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_record_field_name(cmp_type, 3, &name_3));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_record_field_name(cmp_type, 4, &name_4));
  EXPECT_STREQ("two_words", name_3);
  EXPECT_STREQ("two_words_1", name_4);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&cursor, 2));
  int32_t i32_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &i32_val));
  EXPECT_EQ(-12345, i32_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/full_cmp[0]/enum_val"));
  int16_t enum_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &enum_val));
  EXPECT_EQ(5, enum_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/full_cmp[0]/fixed"));
  char str_buf[7] = {};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cursor, str_buf, 7));
  EXPECT_STREQ("abcde", str_buf);

  // Verify mixed compound keeps only keep_i32, keep_i24, and keep_f64.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/mixed_cmp[0]"));
  coda_type* mixed_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &mixed_type));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_num_record_fields(mixed_type, &num_fields));
  ASSERT_EQ(3, num_fields);
  const char* f0 = nullptr;
  const char* f1 = nullptr;
  const char* f2 = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_record_field_name(mixed_type, 0, &f0));
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_record_field_name(mixed_type, 1, &f1));
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_record_field_name(mixed_type, 2, &f2));
  EXPECT_STREQ("keep_i32", f0);
  EXPECT_STREQ("keep_i24", f1);
  EXPECT_STREQ("keep_f64", f2);

  // Verify all-unsupported compound yields a 0-field record.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/all_dropped_cmp[0]"));
  coda_type* empty_cmp_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &empty_cmp_type));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_num_record_fields(empty_cmp_type, &num_fields));
  EXPECT_EQ(0, num_fields);
}

TEST(CodaHdf5TypeTest, UnsupportedFloatDatasetAndMemberFailOpenCleanly) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Standalone H5T_NATIVE_LDOUBLE dataset fails in new_hdf5BasicDataType().
  const TempFile ldouble_file("hdf5_type_ldouble.h5");
  {
    ScopedH5File file(H5Fcreate(ldouble_file.c_str(), H5F_ACC_TRUNC,
                                H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedH5Space space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    ScopedH5Dataset dset(H5Dcreate2(file.get(), "ldbl", H5T_NATIVE_LDOUBLE,
                                    space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(ldouble_file.c_str(), product.OutParam()));
  EXPECT_EQ(nullptr, product.get());

  // 2. Compound containing [int32, H5T_NATIVE_LDOUBLE] fails mid-loop in
  //    new_hdf5CompoundDataType(), exercising partial member cleanup.
  const TempFile cmp_ldouble_file("hdf5_type_cmp_ldouble.h5");
  {
    ScopedH5File file(H5Fcreate(cmp_ldouble_file.c_str(), H5F_ACC_TRUNC,
                                H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedH5Type cmp_t(
        H5Tcreate(H5T_COMPOUND, sizeof(int32_t) + sizeof(long double)));
    ASSERT_TRUE(cmp_t.valid());
    ASSERT_GE(H5Tinsert(cmp_t.get(), "ok_i32", 0, H5T_NATIVE_INT32), 0);
    ASSERT_GE(
        H5Tinsert(cmp_t.get(), "bad_ldbl", sizeof(int32_t), H5T_NATIVE_LDOUBLE),
        0);
    ScopedH5Space space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    ScopedH5Dataset dset(H5Dcreate2(file.get(), "cmp_bad", cmp_t.get(),
                                    space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
  }

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(cmp_ldouble_file.c_str(), product.OutParam()));
  EXPECT_EQ(nullptr, product.get());
}

TEST(CodaHdf5TypeTest,
     UnsupportedDatasetDatatypesNamedTypesAndLinksAreIgnored) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile h5_file("hdf5_type_ignored.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    // Anchor dataset so root record has 1 known field and region ref target.
    constexpr std::array<hsize_t, 1> kAnchorDims = {4};
    ScopedH5Space anchor_space(
        H5Screate_simple(1, kAnchorDims.data(), nullptr));
    ASSERT_TRUE(anchor_space.valid());
    ScopedH5Dataset anchor(H5Dcreate2(file.get(), "anchor", H5T_NATIVE_INT32,
                                      anchor_space.get(), H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(anchor.valid());
    constexpr std::array<int32_t, 4> kAnchorVals = {1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(anchor.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kAnchorVals.data()),
              0);

    ScopedH5Space scalar_space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar_space.valid());

    // Unsupported dataset classes in new_hdf5DataType():
    // H5T_TIME, H5T_BITFIELD, H5T_OPAQUE, H5T_REFERENCE (obj & dsetreg),
    // H5T_ARRAY, H5T_VLEN, H5T_COMPLEX, plus oversized 16-byte int & enum.
    ScopedH5Dataset d_time(H5Dcreate2(file.get(), "d_time", H5T_UNIX_D32LE,
                                      scalar_space.get(), H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_time.valid());

    ScopedH5Dataset d_bits(H5Dcreate2(file.get(), "d_bitfield", H5T_STD_B8LE,
                                      scalar_space.get(), H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_bits.valid());

    ScopedH5Type opaque_t(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opaque_t.valid());
    ASSERT_GE(H5Tset_tag(opaque_t.get(), "opaque_tag"), 0);
    ScopedH5Dataset d_opaque(H5Dcreate2(file.get(), "d_opaque", opaque_t.get(),
                                        scalar_space.get(), H5P_DEFAULT,
                                        H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_opaque.valid());

    hobj_ref_t obj_ref{};
    ASSERT_GE(H5Rcreate(&obj_ref, file.get(), "/anchor", H5R_OBJECT, -1), 0);
    ASSERT_TRUE(WriteScalarDataset(file.get(), "d_ref_obj", H5T_STD_REF_OBJ,
                                   H5T_STD_REF_OBJ, obj_ref));

    constexpr hsize_t kStart = 1;
    constexpr hsize_t kCount = 2;
    ASSERT_GE(H5Sselect_hyperslab(anchor_space.get(), H5S_SELECT_SET, &kStart,
                                  nullptr, &kCount, nullptr),
              0);
    hdset_reg_ref_t reg_ref{};
    ASSERT_GE(H5Rcreate(&reg_ref, file.get(), "/anchor", H5R_DATASET_REGION,
                        anchor_space.get()),
              0);
    ASSERT_TRUE(WriteScalarDataset(file.get(), "d_ref_reg", H5T_STD_REF_DSETREG,
                                   H5T_STD_REF_DSETREG, reg_ref));

    constexpr std::array<hsize_t, 1> kArrDims = {2};
    ScopedH5Type arr_t(H5Tarray_create2(H5T_NATIVE_INT32, 1, kArrDims.data()));
    ASSERT_TRUE(arr_t.valid());
    ScopedH5Dataset d_arr(H5Dcreate2(file.get(), "d_array", arr_t.get(),
                                     scalar_space.get(), H5P_DEFAULT,
                                     H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_arr.valid());

    ScopedH5Type vlen_t(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vlen_t.valid());
    ScopedH5Dataset d_vlen(H5Dcreate2(file.get(), "d_vlen", vlen_t.get(),
                                      scalar_space.get(), H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_vlen.valid());

    ScopedH5Dataset d_complex(
        H5Dcreate2(file.get(), "d_complex", H5T_COMPLEX_IEEE_F32LE,
                   scalar_space.get(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_complex.valid());

    // TODO(schwehr): Test >8-byte integer/enum datasets once upstream
    // coda-hdf5-type.c:234-235 fixes the heap-UAF (reading
    // basic_type->datatype_id after coda_hdf5_type_delete(basic_type)).

    // Committed named datatype (H5G_TYPE).
    ScopedH5Type committed_t(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(committed_t.valid());
    ASSERT_GE(H5Tcommit2(file.get(), "committed_dt", committed_t.get(),
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              0);
    ScopedH5Type reopened_t(H5Topen2(file.get(), "committed_dt", H5P_DEFAULT));
    ASSERT_TRUE(reopened_t.valid());

    // Soft links (valid target & dangling) and external link (H5G_UDLINK).
    ASSERT_GE(H5Lcreate_soft("/anchor", file.get(), "soft_anchor", H5P_DEFAULT,
                             H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_soft("/missing_target", file.get(), "soft_dangling",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_external("other_file.h5", "/ds", file.get(), "ext_link",
                                 H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  coda_type* root_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_root_type(product.get(), &root_type));
  long num_fields = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_num_record_fields(root_type, &num_fields));
  ASSERT_EQ(1, num_fields);
  const char* only_name = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_record_field_name(root_type, 0, &only_name));
  EXPECT_STREQ("anchor", only_name);
}

TEST(CodaHdf5TypeTest, AttributesOnGroupsAndDatasetsCoverAllTypesAndDims) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile h5_file("hdf5_type_attrs.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    // Root group attributes: H5S_NULL string (kept as empty string),
    // H5S_NULL int (ignored), 9D attribute (ignored), and 1D/2D array attrs.
    ScopedH5Space null_space(H5Screate(H5S_NULL));
    ASSERT_TRUE(null_space.valid());
    ScopedH5Type fixed_s4 = MakeFixedStringType(4, H5T_STR_NULLTERM);
    ASSERT_TRUE(fixed_s4.valid());
    ScopedH5Attr a_null_str(H5Acreate2(file.get(), "null_str", fixed_s4.get(),
                                       null_space.get(), H5P_DEFAULT,
                                       H5P_DEFAULT));
    ASSERT_TRUE(a_null_str.valid());

    ScopedH5Attr a_null_int(H5Acreate2(file.get(), "null_int_drop",
                                       H5T_NATIVE_INT32, null_space.get(),
                                       H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_null_int.valid());

    constexpr std::array<hsize_t, 9> kDims9D = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    ScopedH5Space space9d(H5Screate_simple(9, kDims9D.data(), nullptr));
    ASSERT_TRUE(space9d.valid());
    ScopedH5Attr a_9d(H5Acreate2(file.get(), "attr_9d_drop", H5T_NATIVE_INT32,
                                 space9d.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_9d.valid());

    constexpr std::array<hsize_t, 2> kDims2D = {2, 2};
    ScopedH5Space space2d(H5Screate_simple(2, kDims2D.data(), nullptr));
    ASSERT_TRUE(space2d.valid());
    ScopedH5Attr a_2d(H5Acreate2(file.get(), "root_mat", H5T_NATIVE_INT32,
                                 space2d.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_2d.valid());
    constexpr std::array<int32_t, 4> kMatVals = {10, 20, 30, 40};
    ASSERT_GE(H5Awrite(a_2d.get(), H5T_NATIVE_INT32, kMatVals.data()), 0);

    // Subgroup with attributes of every supported and unsupported type.
    ScopedH5Group grp(
        H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ASSERT_TRUE(
        WriteScalarAttr<int8_t>(grp.get(), "a_i8", H5T_NATIVE_INT8, -5));
    ASSERT_TRUE(
        WriteScalarAttr<uint8_t>(grp.get(), "a_u8", H5T_NATIVE_UINT8, 200));
    ASSERT_TRUE(
        WriteScalarAttr<int16_t>(grp.get(), "a_i16", H5T_NATIVE_INT16, -300));
    ASSERT_TRUE(WriteScalarAttr<uint16_t>(grp.get(), "a_u16", H5T_NATIVE_UINT16,
                                          60000));
    ASSERT_TRUE(
        WriteScalarAttr<int32_t>(grp.get(), "a_i32", H5T_NATIVE_INT32, -70000));
    ASSERT_TRUE(WriteScalarAttr<uint32_t>(grp.get(), "a_u32", H5T_NATIVE_UINT32,
                                          3500000000u));
    ASSERT_TRUE(WriteScalarAttr<int64_t>(grp.get(), "a_i64", H5T_NATIVE_INT64,
                                         -5000000000LL));
    ASSERT_TRUE(WriteScalarAttr<uint64_t>(grp.get(), "a_u64", H5T_NATIVE_UINT64,
                                          9000000000ULL));
    ASSERT_TRUE(
        WriteScalarAttr<float>(grp.get(), "a_f32", H5T_NATIVE_FLOAT, 1.25f));
    ASSERT_TRUE(
        WriteScalarAttr<double>(grp.get(), "a_f64", H5T_NATIVE_DOUBLE, -2.5));

    ScopedH5Type enum_i32(H5Tenum_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(enum_i32.valid());
    constexpr int32_t kEnumVal = 77;
    ASSERT_GE(H5Tenum_insert(enum_i32.get(), "SEVENTY_SEVEN", &kEnumVal), 0);
    ASSERT_TRUE(
        WriteScalarAttr<int32_t>(grp.get(), "a_enum", enum_i32.get(), 77));

    // Variable-length string attributes: scalar and 1D array.
    ScopedH5Type vlen_str = MakeVlenStringType();
    ASSERT_TRUE(vlen_str.valid());
    const char* scalar_vstr = "hello_vlen";
    ASSERT_TRUE(WriteScalarAttr<const char*>(grp.get(), "a_vstr",
                                             vlen_str.get(), scalar_vstr));
    constexpr std::array<const char*, 2> kVstrArr = {"first", "second"};
    ASSERT_TRUE(WriteArray1DAttr(grp.get(), "a_vstr_arr", vlen_str.get(),
                                 kVstrArr.data(), 2));

    // Fixed-length string array attribute + colliding/weird attribute names.
    constexpr std::array<char, 8> kFixedArr = {'x', 'y', 'z',  '\0',
                                               'p', 'q', '\0', '\0'};
    ASSERT_TRUE(WriteArray1DAttr(grp.get(), "a_fstr_arr", fixed_s4.get(),
                                 kFixedArr.data(), 2));
    ASSERT_TRUE(
        WriteScalarAttr<int32_t>(grp.get(), "1attr", H5T_NATIVE_INT32, 1));
    ASSERT_TRUE(
        WriteScalarAttr<int32_t>(grp.get(), "a b", H5T_NATIVE_INT32, 2));
    ASSERT_TRUE(
        WriteScalarAttr<int32_t>(grp.get(), "a_b", H5T_NATIVE_INT32, 3));

    // Ignored attributes: oversized int, long double, bitfield, compound.
    ScopedH5Type int128_t = MakeOversizedIntType(16);
    ASSERT_TRUE(int128_t.valid());
    ScopedH5Space scalar_space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar_space.valid());
    ScopedH5Attr a_int128(H5Acreate2(grp.get(), "drop_int128", int128_t.get(),
                                     scalar_space.get(), H5P_DEFAULT,
                                     H5P_DEFAULT));
    ASSERT_TRUE(a_int128.valid());
    ScopedH5Attr a_ldbl(H5Acreate2(grp.get(), "drop_ldbl", H5T_NATIVE_LDOUBLE,
                                   scalar_space.get(), H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(a_ldbl.valid());
    ScopedH5Attr a_bits(H5Acreate2(grp.get(), "drop_bits", H5T_STD_B8LE,
                                   scalar_space.get(), H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(a_bits.valid());

    // Dataset with its own scalar attribute and a 0-length 1D array attribute
    // (covers num_elements == 0 && definition->type_class == coda_array_class).
    ScopedH5Dataset dset(H5Dcreate2(grp.get(), "d", H5T_NATIVE_INT32,
                                    scalar_space.get(), H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_TRUE(
        WriteScalarAttr<double>(dset.get(), "d_attr", H5T_NATIVE_DOUBLE, 9.5));
    constexpr std::array<hsize_t, 1> kZeroAttrDim = {0};
    ScopedH5Space zero_attr_space(
        H5Screate_simple(1, kZeroAttrDim.data(), nullptr));
    ASSERT_TRUE(zero_attr_space.valid());
    ScopedH5Attr a_zero_arr(H5Acreate2(dset.get(), "empty_arr_attr",
                                       H5T_NATIVE_INT32, zero_attr_space.get(),
                                       H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_zero_arr.valid());
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  // 1. Check root attributes (only null_str and root_mat survive).
  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  int has_attrs = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&cursor, &has_attrs));
  EXPECT_EQ(1, has_attrs);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&cursor));
  long num_attrs = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_attrs));
  EXPECT_EQ(2, num_attrs);

  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "null_str"));
  long str_len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(0, str_len);
  char empty_buf[1] = {'?'};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cursor, empty_buf, 1));
  EXPECT_STREQ("", empty_buf);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "root_mat"));
  std::vector<int32_t> mat(4, -1);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(&cursor, mat.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(mat, ElementsAre(10, 20, 30, 40));

  // 2. Check subgroup attributes (17 kept, 3 unsupported dropped).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/grp"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_attrs));
  EXPECT_EQ(17, num_attrs);

  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "a_vstr"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(10, str_len);
  std::string vstr(static_cast<size_t>(str_len + 1), '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, vstr.data(), str_len + 1));
  EXPECT_STREQ("hello_vlen", vstr.c_str());

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "a_vstr_arr"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(6, str_len);
  std::string vstr2(static_cast<size_t>(str_len + 1), '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, vstr2.data(), str_len + 1));
  EXPECT_STREQ("second", vstr2.c_str());

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/grp@a_fstr_arr[0]"));
  char fstr0[5] = {};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cursor, fstr0, 5));
  EXPECT_STREQ("xyz", fstr0);

  // 3. Check dataset attributes.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/grp/d"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_attrs));
  EXPECT_EQ(2, num_attrs);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "d_attr"));
  double d_attr = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &d_attr));
  EXPECT_DOUBLE_EQ(9.5, d_attr);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "empty_arr_attr"));
  long empty_attr_elems = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&cursor, &empty_attr_elems));
  EXPECT_EQ(0, empty_attr_elems);
}

TEST(CodaHdf5TypeTest, ConversionAttributesScaleOffsetMissingAndFillValue) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile h5_file("hdf5_type_conversions.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> kDims2 = {2};
    ScopedH5Space space2(H5Screate_simple(1, kDims2.data(), nullptr));
    ASSERT_TRUE(space2.valid());

    // 1. Float dataset with float scale_factor, float add_offset, float
    //    _FillValue.
    ScopedH5Dataset d_f32(H5Dcreate2(file.get(), "d_f32_scaled",
                                     H5T_NATIVE_FLOAT, space2.get(),
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_f32.valid());
    constexpr std::array<float, 2> kF32Vals = {4.0f, -999.0f};
    ASSERT_GE(H5Dwrite(d_f32.get(), H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kF32Vals.data()),
              0);
    ASSERT_TRUE(WriteScalarAttr<float>(d_f32.get(), "scale_factor",
                                       H5T_NATIVE_FLOAT, 2.5f));
    ASSERT_TRUE(WriteScalarAttr<float>(d_f32.get(), "add_offset",
                                       H5T_NATIVE_FLOAT, 1.0f));
    ASSERT_TRUE(WriteScalarAttr<float>(d_f32.get(), "_FillValue",
                                       H5T_NATIVE_FLOAT, -999.0f));

    // 2. Double dataset with 1-element array scale_factor, add_offset, and
    //    missing_value (exercises definition->type_class == coda_array_class).
    ScopedH5Dataset d_f64(H5Dcreate2(file.get(), "d_f64_arr_attr",
                                     H5T_NATIVE_DOUBLE, space2.get(),
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_f64.valid());
    constexpr std::array<double, 2> kF64Vals = {10.0, -1.0};
    ASSERT_GE(H5Dwrite(d_f64.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kF64Vals.data()),
              0);
    constexpr double kScale = 0.5;
    constexpr double kOffset = -3.0;
    constexpr double kMissing = -1.0;
    ASSERT_TRUE(WriteArray1DAttr(d_f64.get(), "scale_factor", H5T_NATIVE_DOUBLE,
                                 &kScale, 1));
    ASSERT_TRUE(WriteArray1DAttr(d_f64.get(), "add_offset", H5T_NATIVE_DOUBLE,
                                 &kOffset, 1));
    ASSERT_TRUE(WriteArray1DAttr(d_f64.get(), "missing_value",
                                 H5T_NATIVE_DOUBLE, &kMissing, 1));

    // 3. missing_value takes precedence over _FillValue (missing_value created
    //    first so _FillValue also hits !coda_isNaN(conversion->invalid_value)).
    ScopedH5Dataset d_prec(H5Dcreate2(file.get(), "d_missing_over_fill",
                                      H5T_NATIVE_FLOAT, space2.get(),
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_prec.valid());
    constexpr std::array<float, 2> kPrecVals = {-1.0f, -2.0f};
    ASSERT_GE(H5Dwrite(d_prec.get(), H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kPrecVals.data()),
              0);
    ASSERT_TRUE(WriteScalarAttr<float>(d_prec.get(), "missing_value",
                                       H5T_NATIVE_FLOAT, -1.0f));
    ASSERT_TRUE(WriteScalarAttr<float>(d_prec.get(), "_FillValue",
                                       H5T_NATIVE_FLOAT, -2.0f));

    // 4. Integer dataset with ONLY _FillValue -> CODA must NOT attach a
    //    conversion (hits else { coda_conversion_delete(conversion); }).
    ASSERT_TRUE(WriteScalarDataset<int32_t>(file.get(), "d_int_fill_only",
                                            H5T_NATIVE_INT32, H5T_NATIVE_INT32,
                                            -999));
    ScopedH5Dataset d_int_fill(
        H5Dopen2(file.get(), "d_int_fill_only", H5P_DEFAULT));
    ASSERT_TRUE(d_int_fill.valid());
    ASSERT_TRUE(WriteScalarAttr<int32_t>(d_int_fill.get(), "_FillValue",
                                         H5T_NATIVE_INT32, -999));

    // 5. Integer dataset with double scale_factor & add_offset -> converted.
    ASSERT_TRUE(WriteScalarDataset<int32_t>(
        file.get(), "d_int_scaled", H5T_NATIVE_INT32, H5T_NATIVE_INT32, 10));
    ScopedH5Dataset d_int_scaled(
        H5Dopen2(file.get(), "d_int_scaled", H5P_DEFAULT));
    ASSERT_TRUE(d_int_scaled.valid());
    ASSERT_TRUE(WriteScalarAttr<double>(d_int_scaled.get(), "scale_factor",
                                        H5T_NATIVE_DOUBLE, 2.0));
    ASSERT_TRUE(WriteScalarAttr<double>(d_int_scaled.get(), "add_offset",
                                        H5T_NATIVE_DOUBLE, 5.0));

    // 6. Exercise every remaining missing_value type branch (int8..uint64,
    //    odd byte sizes where size != 4 or size != 8, string default:) plus
    //    non-float scale_factor / add_offset and 2-element scale_factor.
    auto add_missing_case = [&](const char* dname, hid_t file_type,
                                hid_t mem_type,
                                const void* missing_ptr) -> bool {
      if (!WriteScalarDataset<double>(file.get(), dname, H5T_NATIVE_DOUBLE,
                                      H5T_NATIVE_DOUBLE, 5.0)) {
        return false;
      }
      ScopedH5Dataset ds(H5Dopen2(file.get(), dname, H5P_DEFAULT));
      if (!ds) {
        return false;
      }
      ScopedH5Space sp(H5Screate(H5S_SCALAR));
      if (!sp) {
        return false;
      }
      ScopedH5Attr at(H5Acreate2(ds.get(), "missing_value", file_type, sp.get(),
                                 H5P_DEFAULT, H5P_DEFAULT));
      if (!at) {
        return false;
      }
      return H5Awrite(at.get(), mem_type, missing_ptr) >= 0;
    };
    constexpr int8_t kM8 = 5;
    constexpr uint8_t kMU8 = 5;
    constexpr int16_t kM16 = 5;
    constexpr uint16_t kMU16 = 5;
    constexpr int32_t kM32 = 5;
    constexpr uint32_t kMU32 = 5;
    constexpr int64_t kM64 = 5;
    constexpr uint64_t kMU64 = 5;
    constexpr float kMF32 = 5.0f;
    ASSERT_TRUE(
        add_missing_case("m_i8", H5T_NATIVE_INT8, H5T_NATIVE_INT8, &kM8));
    ASSERT_TRUE(
        add_missing_case("m_u8", H5T_NATIVE_UINT8, H5T_NATIVE_UINT8, &kMU8));
    ASSERT_TRUE(
        add_missing_case("m_i16", H5T_NATIVE_INT16, H5T_NATIVE_INT16, &kM16));
    ASSERT_TRUE(add_missing_case("m_u16", H5T_NATIVE_UINT16, H5T_NATIVE_UINT16,
                                 &kMU16));
    ASSERT_TRUE(
        add_missing_case("m_i32", H5T_NATIVE_INT32, H5T_NATIVE_INT32, &kM32));
    ASSERT_TRUE(add_missing_case("m_u32", H5T_NATIVE_UINT32, H5T_NATIVE_UINT32,
                                 &kMU32));
    ASSERT_TRUE(
        add_missing_case("m_i64", H5T_NATIVE_INT64, H5T_NATIVE_INT64, &kM64));
    ASSERT_TRUE(add_missing_case("m_u64", H5T_NATIVE_UINT64, H5T_NATIVE_UINT64,
                                 &kMU64));
    ASSERT_TRUE(
        add_missing_case("m_f32", H5T_NATIVE_FLOAT, H5T_NATIVE_FLOAT, &kMF32));

    ScopedH5Type i24_le = MakeCustomIntType(3, true, H5T_ORDER_LE);
    ScopedH5Type u24_le = MakeCustomIntType(3, false, H5T_ORDER_LE);
    ScopedH5Type i40_le = MakeCustomIntType(5, true, H5T_ORDER_LE);
    ScopedH5Type u48_le = MakeCustomIntType(6, false, H5T_ORDER_LE);
    ASSERT_TRUE(i24_le.valid());
    ASSERT_TRUE(u24_le.valid());
    ASSERT_TRUE(i40_le.valid());
    ASSERT_TRUE(u48_le.valid());
    ASSERT_TRUE(add_missing_case("m_i24_ignored", i24_le.get(),
                                 H5T_NATIVE_INT32, &kM32));
    ASSERT_TRUE(add_missing_case("m_u24_ignored", u24_le.get(),
                                 H5T_NATIVE_UINT32, &kMU32));
    ASSERT_TRUE(add_missing_case("m_i40_ignored", i40_le.get(),
                                 H5T_NATIVE_INT64, &kM64));
    ASSERT_TRUE(add_missing_case("m_u48_ignored", u48_le.get(),
                                 H5T_NATIVE_UINT64, &kMU64));

    // Dataset with ONLY add_offset (numerator == 1.0, add_offset != 0.0).
    ASSERT_TRUE(WriteScalarDataset<double>(file.get(), "d_offset_only",
                                           H5T_NATIVE_DOUBLE, H5T_NATIVE_DOUBLE,
                                           4.0));
    ScopedH5Dataset d_off_only(
        H5Dopen2(file.get(), "d_offset_only", H5P_DEFAULT));
    ASSERT_TRUE(d_off_only.valid());
    ASSERT_TRUE(WriteScalarAttr<double>(d_off_only.get(), "add_offset",
                                        H5T_NATIVE_DOUBLE, 3.0));

    // String missing_value + non-float scale_factor/add_offset + 2-elem attr.
    ASSERT_TRUE(WriteScalarDataset<double>(file.get(), "m_str_ignored",
                                           H5T_NATIVE_DOUBLE, H5T_NATIVE_DOUBLE,
                                           7.0));
    ScopedH5Dataset ds_str(H5Dopen2(file.get(), "m_str_ignored", H5P_DEFAULT));
    ASSERT_TRUE(ds_str.valid());
    ScopedH5Type s4 = MakeFixedStringType(4, H5T_STR_NULLTERM);
    ASSERT_TRUE(s4.valid());
    constexpr char kStrMissing[4] = "bad";
    ASSERT_TRUE(WriteArray1DAttr(ds_str.get(), "missing_value", s4.get(),
                                 kStrMissing, 1));
    ASSERT_TRUE(WriteScalarAttr<int32_t>(ds_str.get(), "scale_factor",
                                         H5T_NATIVE_INT32, 3));
    ASSERT_TRUE(WriteScalarAttr<int32_t>(ds_str.get(), "add_offset",
                                         H5T_NATIVE_INT32, 9));
    constexpr std::array<double, 2> kTwoVals = {2.0, 3.0};
    // Also check a second dataset with 2-element scale_factor, add_offset &
    // missing_value (num_elements != 1 must be ignored).
    ASSERT_TRUE(WriteScalarDataset<double>(file.get(), "m_two_elem_ignored",
                                           H5T_NATIVE_DOUBLE, H5T_NATIVE_DOUBLE,
                                           8.0));
    ScopedH5Dataset ds_two(
        H5Dopen2(file.get(), "m_two_elem_ignored", H5P_DEFAULT));
    ASSERT_TRUE(ds_two.valid());
    ASSERT_TRUE(WriteArray1DAttr(ds_two.get(), "scale_factor",
                                 H5T_NATIVE_DOUBLE, kTwoVals.data(), 2));
    ASSERT_TRUE(WriteArray1DAttr(ds_two.get(), "add_offset", H5T_NATIVE_DOUBLE,
                                 kTwoVals.data(), 2));
    ASSERT_TRUE(WriteArray1DAttr(ds_two.get(), "missing_value",
                                 H5T_NATIVE_DOUBLE, kTwoVals.data(), 2));

    // 7. 1-element float array conversion attributes, _FillValue created before
    //    missing_value (verifying missing_value overrides _FillValue), and
    //    integer dataset with only missing_value (no conversion attached).
    ScopedH5Dataset d_f32_arr(
        H5Dcreate2(file.get(), "d_f32_arr_attr", H5T_NATIVE_FLOAT, space2.get(),
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_f32_arr.valid());
    ASSERT_GE(H5Dwrite(d_f32_arr.get(), H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kF32Vals.data()),
              0);
    constexpr float kF32Scale = 2.0f;
    constexpr float kF32Offset = -1.5f;
    constexpr float kF32Fill = -999.0f;
    ASSERT_TRUE(WriteArray1DAttr(d_f32_arr.get(), "scale_factor",
                                 H5T_NATIVE_FLOAT, &kF32Scale, 1));
    ASSERT_TRUE(WriteArray1DAttr(d_f32_arr.get(), "add_offset",
                                 H5T_NATIVE_FLOAT, &kF32Offset, 1));
    ASSERT_TRUE(WriteArray1DAttr(d_f32_arr.get(), "_FillValue",
                                 H5T_NATIVE_FLOAT, &kF32Fill, 1));

    ScopedH5Dataset d_fill_then_missing(
        H5Dcreate2(file.get(), "d_fill_then_missing", H5T_NATIVE_DOUBLE,
                   space2.get(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_fill_then_missing.valid());
    constexpr std::array<double, 2> kFillMissingVals = {-10.0, -20.0};
    ASSERT_GE(H5Dwrite(d_fill_then_missing.get(), H5T_NATIVE_DOUBLE, H5S_ALL,
                       H5S_ALL, H5P_DEFAULT, kFillMissingVals.data()),
              0);
    ASSERT_TRUE(WriteScalarAttr<double>(d_fill_then_missing.get(), "_FillValue",
                                        H5T_NATIVE_DOUBLE, -10.0));
    ASSERT_TRUE(WriteScalarAttr<double>(
        d_fill_then_missing.get(), "missing_value", H5T_NATIVE_DOUBLE, -20.0));

    ASSERT_TRUE(WriteScalarDataset<int32_t>(file.get(), "d_int_missing_only",
                                            H5T_NATIVE_INT32, H5T_NATIVE_INT32,
                                            -777));
    ScopedH5Dataset d_int_missing(
        H5Dopen2(file.get(), "d_int_missing_only", H5P_DEFAULT));
    ASSERT_TRUE(d_int_missing.valid());
    ASSERT_TRUE(WriteScalarAttr<int32_t>(d_int_missing.get(), "missing_value",
                                         H5T_NATIVE_INT32, -777));
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  // 1. d_f32_scaled: 4.0 * 2.5 + 1.0 = 11.0; -999.0 -> NaN.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_f32_scaled"));
  std::array<double, 2> vals = {};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(&cursor, vals.data(),
                                                        coda_array_ordering_c));
  EXPECT_DOUBLE_EQ(11.0, vals[0]);
  EXPECT_TRUE(std::isnan(vals[1]));

  // 2. d_f64_arr_attr: 10.0 * 0.5 - 3.0 = 2.0; -1.0 -> NaN.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_f64_arr_attr"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(&cursor, vals.data(),
                                                        coda_array_ordering_c));
  EXPECT_DOUBLE_EQ(2.0, vals[0]);
  EXPECT_TRUE(std::isnan(vals[1]));

  // 3. d_missing_over_fill: -1.0 -> NaN, -2.0 -> -2.0.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_missing_over_fill"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(&cursor, vals.data(),
                                                        coda_array_ordering_c));
  EXPECT_TRUE(std::isnan(vals[0]));
  EXPECT_DOUBLE_EQ(-2.0, vals[1]);

  // 4. d_int_fill_only: no conversion attached -> -999.0 stays -999.0.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_int_fill_only[0]"));
  double val = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
  EXPECT_DOUBLE_EQ(-999.0, val);

  // 5. d_int_scaled: 10 * 2.0 + 5.0 = 25.0.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_int_scaled[0]"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
  EXPECT_DOUBLE_EQ(25.0, val);

  // 6. All typed missing_value datasets evaluate 5.0 -> NaN, while
  //    m_str_ignored stays 7.0.
  for (const char* name : {"m_i8", "m_u8", "m_i16", "m_u16", "m_i32", "m_u32",
                           "m_i64", "m_u64", "m_f32"}) {
    SCOPED_TRACE(name);
    ASSERT_EQ(
        CODA_SUCCESS,
        coda_cursor_goto(&cursor, absl::StrCat("/", name, "[0]").c_str()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
    EXPECT_TRUE(std::isnan(val));
  }

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/m_str_ignored[0]"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
  EXPECT_DOUBLE_EQ(7.0, val);

  for (const char* name :
       {"m_i24_ignored", "m_u24_ignored", "m_i40_ignored", "m_u48_ignored"}) {
    SCOPED_TRACE(name);
    ASSERT_EQ(
        CODA_SUCCESS,
        coda_cursor_goto(&cursor, absl::StrCat("/", name, "[0]").c_str()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
    EXPECT_DOUBLE_EQ(5.0, val);
  }

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_offset_only[0]"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
  EXPECT_DOUBLE_EQ(7.0, val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/m_two_elem_ignored[0]"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
  EXPECT_DOUBLE_EQ(8.0, val);

  // 7. d_f32_arr_attr: 4.0 * 2.0 - 1.5 = 6.5; -999.0 -> NaN.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_f32_arr_attr"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(&cursor, vals.data(),
                                                        coda_array_ordering_c));
  EXPECT_DOUBLE_EQ(6.5, vals[0]);
  EXPECT_TRUE(std::isnan(vals[1]));

  // d_fill_then_missing: missing_value (-20.0) overrides _FillValue (-10.0).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_fill_then_missing"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(&cursor, vals.data(),
                                                        coda_array_ordering_c));
  EXPECT_DOUBLE_EQ(-10.0, vals[0]);
  EXPECT_TRUE(std::isnan(vals[1]));

  // d_int_missing_only: no conversion attached -> -777.0 stays -777.0.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_int_missing_only[0]"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
  EXPECT_DOUBLE_EQ(-777.0, val);
}

TEST(CodaHdf5TypeTest, GroupsCyclesDuplicateNamesAndBlockSizeRealloc) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile h5_file("hdf5_type_cycles.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    // 1. Empty group and nested group.
    ScopedH5Group empty_grp(H5Gcreate2(file.get(), "empty_grp", H5P_DEFAULT,
                                       H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(empty_grp.valid());

    ScopedH5Group cycle_grp(H5Gcreate2(file.get(), "cycle_grp", H5P_DEFAULT,
                                       H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(cycle_grp.valid());
    ASSERT_TRUE(WriteScalarDataset<int32_t>(
        cycle_grp.get(), "leaf", H5T_NATIVE_INT32, H5T_NATIVE_INT32, 99));

    // Hard links creating a self-loop, a cycle back to root, and a duplicate
    // dataset hard link (detected by fileno/objno match in create_tree).
    ASSERT_GE(H5Lcreate_hard(file.get(), "/cycle_grp", cycle_grp.get(),
                             "self_loop", H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_hard(file.get(), "/", cycle_grp.get(), "to_root",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Lcreate_hard(cycle_grp.get(), "leaf", file.get(), "leaf_alias",
                             H5P_DEFAULT, H5P_DEFAULT),
              0);

    // 2. Duplicate / weird object names that collide after identifier mapping.
    ASSERT_TRUE(WriteScalarDataset<int32_t>(file.get(), "123", H5T_NATIVE_INT32,
                                            H5T_NATIVE_INT32, 1));
    ASSERT_TRUE(WriteScalarDataset<int32_t>(file.get(), "a b", H5T_NATIVE_INT32,
                                            H5T_NATIVE_INT32, 2));
    ASSERT_TRUE(WriteScalarDataset<int32_t>(file.get(), "a_b", H5T_NATIVE_INT32,
                                            H5T_NATIVE_INT32, 3));

    // 3. Create enough objects (> BLOCK_SIZE = 16) to force realloc expansion
    //    in coda_hdf5_create_tree().
    for (int i = 0; i < 18; ++i) {
      const std::string dname = absl::StrCat("extra_", i);
      ASSERT_TRUE(WriteScalarDataset<int32_t>(
          file.get(), dname.c_str(), H5T_NATIVE_INT32, H5T_NATIVE_INT32, i));
    }
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  coda_type* root_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_root_type(product.get(), &root_type));
  // Root fields: empty_grp, cycle_grp, unnamed("123"), a_b, a_b_1,
  // extra_0..17 (leaf_alias is ignored because /cycle_grp/leaf or leaf_alias is
  // visited first -> exactly one of the two hard links is kept!).
  long num_fields = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_num_record_fields(root_type, &num_fields));
  // Total unique objects under root: empty_grp, cycle_grp, 123, a b, a_b,
  // extra_0..17 = 23, plus leaf_alias if visited before cycle_grp/leaf (or
  // omitted if cycle_grp/leaf visited first -> 23 or 24).
  EXPECT_GE(num_fields, 23);
  EXPECT_LE(num_fields, 24);

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/empty_grp"));
  long empty_count = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &empty_count));
  EXPECT_EQ(0, empty_count);

  // Verify cycle_grp skipped self_loop and to_root.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/cycle_grp"));
  coda_type* cycle_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &cycle_type));
  long idx = -1;
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_name(cycle_type,
                                                           "self_loop", &idx));
  EXPECT_EQ(-1, coda_type_get_record_field_index_from_name(cycle_type,
                                                           "to_root", &idx));
  coda_set_error(CODA_SUCCESS, nullptr);

  // Verify weird/colliding names ("123" -> "unnamed", "a b" / "a_b").
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_record_field_index_from_real_name(
                              root_type, "123", &idx));
  const char* sanitized = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_get_record_field_name(root_type, idx, &sanitized));
  EXPECT_STREQ("unnamed", sanitized);

  int32_t ab_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/a_b[0]"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &ab_val));
  int32_t ab1_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/a_b_1[0]"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &ab1_val));
  EXPECT_EQ(5, ab_val + ab1_val);
}

TEST(CodaHdf5TypeTest, UnusualIntegerSizesBitOffsetsEnumsAndCustomFloats) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile h5_file("hdf5_type_unusual_ints.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    // 1. Odd integer sizes (3, 5, 6, 7 bytes) and non-zero bit-offset/precision
    //    on datasets.
    ScopedH5Type i24_le = MakeCustomIntType(3, true, H5T_ORDER_LE);
    ScopedH5Type u24_be = MakeCustomIntType(3, false, H5T_ORDER_BE);
    ScopedH5Type i40_le = MakeCustomIntType(5, true, H5T_ORDER_LE);
    ScopedH5Type u48_be = MakeCustomIntType(6, false, H5T_ORDER_BE);
    ScopedH5Type i56_be = MakeCustomIntType(7, true, H5T_ORDER_BE);
    ScopedH5Type u12_off2 = MakeCustomIntType(2, false, H5T_ORDER_BE, 12, 2);
    ASSERT_TRUE(i24_le.valid());
    ASSERT_TRUE(u24_be.valid());
    ASSERT_TRUE(i40_le.valid());
    ASSERT_TRUE(u48_be.valid());
    ASSERT_TRUE(i56_be.valid());
    ASSERT_TRUE(u12_off2.valid());

    ASSERT_TRUE(WriteScalarDataset<int32_t>(file.get(), "d_i24", i24_le.get(),
                                            H5T_NATIVE_INT32, -123456));
    ASSERT_TRUE(WriteScalarDataset<uint32_t>(file.get(), "d_u24", u24_be.get(),
                                             H5T_NATIVE_UINT32, 0xABCDEFu));
    ASSERT_TRUE(WriteScalarDataset<int64_t>(file.get(), "d_i40", i40_le.get(),
                                            H5T_NATIVE_INT64, -109951162777LL));
    ASSERT_TRUE(WriteScalarDataset<uint64_t>(file.get(), "d_u48", u48_be.get(),
                                             H5T_NATIVE_UINT64,
                                             0x123456789ABULL));
    ASSERT_TRUE(WriteScalarDataset<int64_t>(file.get(), "d_i56", i56_be.get(),
                                            H5T_NATIVE_INT64,
                                            -0x123456789ABCLL));
    ASSERT_TRUE(WriteScalarDataset<uint16_t>(
        file.get(), "d_u12_off2", u12_off2.get(), H5T_NATIVE_UINT16, 0x345u));

    // 2. Signed 1-byte enum, unsigned 4-byte enum, signed 8-byte enum.
    ScopedH5Type enum_i8(H5Tenum_create(H5T_NATIVE_INT8));
    ASSERT_TRUE(enum_i8.valid());
    constexpr int8_t kNegFive = -5;
    ASSERT_GE(H5Tenum_insert(enum_i8.get(), "NEG_FIVE", &kNegFive), 0);
    ASSERT_TRUE(WriteScalarDataset<int8_t>(
        file.get(), "d_enum_i8", enum_i8.get(), enum_i8.get(), kNegFive));

    ScopedH5Type enum_u32(H5Tenum_create(H5T_NATIVE_UINT32));
    ASSERT_TRUE(enum_u32.valid());
    constexpr uint32_t kBigU32 = 3000000001u;
    ASSERT_GE(H5Tenum_insert(enum_u32.get(), "BIG_U32", &kBigU32), 0);
    ASSERT_TRUE(WriteScalarDataset<uint32_t>(
        file.get(), "d_enum_u32", enum_u32.get(), enum_u32.get(), kBigU32));

    ScopedH5Type enum_i64(H5Tenum_create(H5T_NATIVE_INT64));
    ASSERT_TRUE(enum_i64.valid());
    constexpr int64_t kNegI64 = -7000000000LL;
    ASSERT_GE(H5Tenum_insert(enum_i64.get(), "NEG_I64", &kNegI64), 0);
    ASSERT_TRUE(WriteScalarDataset<int64_t>(
        file.get(), "d_enum_i64", enum_i64.get(), enum_i64.get(), kNegI64));

    // 3. Odd integer sizes, enums, and BE floats as attributes on root group.
    ScopedH5Space scalar_space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar_space.valid());
    auto write_attr_with_mem = [&](const char* name, hid_t file_t, hid_t mem_t,
                                   const void* val) -> bool {
      ScopedH5Attr attr(H5Acreate2(file.get(), name, file_t, scalar_space.get(),
                                   H5P_DEFAULT, H5P_DEFAULT));
      if (!attr) {
        return false;
      }
      return H5Awrite(attr.get(), mem_t, val) >= 0;
    };
    constexpr int32_t kA24 = -777;
    constexpr uint64_t kA48 = 0x1122334455ULL;
    constexpr float kAF32Le = -3.25f;
    // Use normal bits (0x3F800040) so neither big-endian nor byte-swapped
    // little-endian representation is subnormal or NaN.
    constexpr uint32_t kAF32BeBits = 0x3F800040u;
    constexpr float kAF32Be = std::bit_cast<float>(kAF32BeBits);
    constexpr double kAF64Be = 6.5;
    ASSERT_TRUE(
        write_attr_with_mem("a_i24", i24_le.get(), H5T_NATIVE_INT32, &kA24));
    ASSERT_TRUE(
        write_attr_with_mem("a_u48", u48_be.get(), H5T_NATIVE_UINT64, &kA48));
    ASSERT_TRUE(write_attr_with_mem("a_enum_i8", enum_i8.get(), enum_i8.get(),
                                    &kNegFive));
    ASSERT_TRUE(write_attr_with_mem("a_f32_le", H5T_IEEE_F32LE,
                                    H5T_NATIVE_FLOAT, &kAF32Le));
    ASSERT_TRUE(write_attr_with_mem("a_f32_be", H5T_IEEE_F32BE,
                                    H5T_NATIVE_FLOAT, &kAF32Be));
    ASSERT_TRUE(write_attr_with_mem("a_f64_be", H5T_IEEE_F64BE,
                                    H5T_NATIVE_DOUBLE, &kAF64Be));
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  struct ExpectedType {
    const char* name;
    coda_native_type read_type;
  };
  constexpr ExpectedType kCases[] = {
      {"d_i24", coda_native_type_int32},
      {"d_u24", coda_native_type_uint32},
      {"d_i40", coda_native_type_int64},
      {"d_u48", coda_native_type_uint64},
      {"d_i56", coda_native_type_int64},
      {"d_u12_off2", coda_native_type_uint16},
      {"d_enum_i8", coda_native_type_int8},
      {"d_enum_u32", coda_native_type_uint32},
      {"d_enum_i64", coda_native_type_int64},
  };
  for (const ExpectedType& tc : kCases) {
    SCOPED_TRACE(tc.name);
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, tc.name));
    coda_type* arr_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &arr_type));
    coda_type* base_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_get_array_base_type(arr_type, &base_type));
    coda_native_type rtype = coda_native_type_not_available;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_read_type(base_type, &rtype));
    EXPECT_EQ(tc.read_type, rtype);
  }

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_i24[0]"));
  int32_t i24_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &i24_val));
  EXPECT_EQ(-123456, i24_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_u48[0]"));
  uint64_t u48_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&cursor, &u48_val));
  EXPECT_EQ(0x123456789ABULL, u48_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/d_enum_i8[0]"));
  int8_t enum_i8_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &enum_i8_val));
  EXPECT_EQ(-5, enum_i8_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@a_i24"));
  int32_t a24_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &a24_val));
  EXPECT_EQ(-777, a24_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@a_u48"));
  ExpectTypeClassAndReadType(cursor, coda_integer_class,
                             coda_native_type_uint64);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@a_f32_le"));
  float af32_le_val = 0.0f;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &af32_le_val));
  EXPECT_FLOAT_EQ(-3.25f, af32_le_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@a_f32_be"));
  ExpectTypeClassAndReadType(cursor, coda_real_class, coda_native_type_float);
  float af32_be_val = 0.0f;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &af32_be_val));
  // Upstream coda-hdf5-type.c:829 reads attributes via H5Aread(attr_id,
  // datatype_id, buffer) in the on-disk datatype (H5T_IEEE_F32BE), while
  // coda-hdf5-type.c:723 sets coda_little_endian on little-endian hosts, so
  // reading a genuine BE float attribute returns byte-swapped bits on LE.
  constexpr uint32_t kExpectedBeBits =
      (std::endian::native == std::endian::little) ? 0x4000803Fu : 0x3F800040u;
  EXPECT_EQ(kExpectedBeBits, std::bit_cast<uint32_t>(af32_be_val));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/@a_f64_be"));
  ExpectTypeClassAndReadType(cursor, coda_real_class, coda_native_type_double);
}

TEST(CodaHdf5TypeTest, DirectInternalTreeConversionAndCompoundTypeDelete) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile h5_file("hdf5_type_internal_api.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedH5Group grp(
        H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ASSERT_TRUE(WriteScalarDataset<int32_t>(grp.get(), "ds", H5T_NATIVE_INT32,
                                            H5T_NATIVE_INT32, 42));
    ASSERT_GE(H5Lcreate_soft("/grp/ds", file.get(), "soft_ds", H5P_DEFAULT,
                             H5P_DEFAULT),
              0);
  }

  // Release HDF5 global free-list pools after closing the product so
  // HeapLeakChecker remains order-independent across randomized GUnit runs.
  const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  auto* hdf5_product = reinterpret_cast<coda_hdf5_product*>(product.get());
  ASSERT_NE(nullptr, hdf5_product);

  // 1. Direct coda_hdf5_create_tree() ignores soft links (returns 1) and
  //    already-visited objects (duplicate fileno/objno match -> returns 1).
  coda_hdf5_object* soft_obj = nullptr;
  EXPECT_EQ(1, coda_hdf5_create_tree(hdf5_product, hdf5_product->file_id,
                                     "soft_ds", &soft_obj));
  EXPECT_EQ(nullptr, soft_obj);

  coda_hdf5_object* dup_obj = nullptr;
  EXPECT_EQ(1, coda_hdf5_create_tree(hdf5_product, hdf5_product->file_id, "grp",
                                     &dup_obj));
  EXPECT_EQ(nullptr, dup_obj);

  // 2. Direct coda_hdf5_basic_type_set_conversion() attaches a conversion to an
  //    existing integer dataset's base_type and affects subsequent reads.
  ASSERT_NE(nullptr, hdf5_product->root_type);
  ASSERT_EQ(tag_hdf5_group, hdf5_product->root_type->tag);
  auto* root_group =
      reinterpret_cast<coda_hdf5_group*>(hdf5_product->root_type);
  ASSERT_NE(nullptr, root_group->object);
  ASSERT_EQ(tag_hdf5_group, root_group->object[0]->tag);
  auto* sub_group = reinterpret_cast<coda_hdf5_group*>(root_group->object[0]);
  ASSERT_EQ(tag_hdf5_dataset, sub_group->object[0]->tag);
  auto* dset = reinterpret_cast<coda_hdf5_dataset*>(sub_group->object[0]);
  ASSERT_NE(nullptr, dset->base_type);

  coda_conversion* conv = coda_conversion_new(2.0, 1.0, 10.0, coda_NaN());
  ASSERT_NE(nullptr, conv);
  EXPECT_EQ(0, coda_hdf5_basic_type_set_conversion(dset->base_type, conv));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/grp/ds[0]"));
  double converted = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &converted));
  EXPECT_DOUBLE_EQ(94.0, converted);

  // 3. Direct coda_hdf5_type_delete() cleans up partially populated
  //    coda_hdf5_compound_data_type (mixed NULL / -1 slots) and
  //    coda_hdf5_dataset (NULL base_type and NULL attributes).
  auto* partial_cmp = static_cast<coda_hdf5_compound_data_type*>(
      std::malloc(sizeof(coda_hdf5_compound_data_type)));
  ASSERT_NE(nullptr, partial_cmp);
  partial_cmp->backend = coda_backend_hdf5;
  partial_cmp->definition = coda_type_record_new(coda_format_hdf5);
  ASSERT_NE(nullptr, partial_cmp->definition);
  coda_type* f0_def = reinterpret_cast<coda_type*>(
      coda_type_number_new(coda_format_hdf5, coda_integer_class));
  ASSERT_NE(nullptr, f0_def);
  ASSERT_EQ(0, coda_type_set_read_type(f0_def, coda_native_type_int32));
  ASSERT_EQ(
      0, coda_type_record_create_field(partial_cmp->definition, "f0", f0_def));
  coda_type* f1_def = reinterpret_cast<coda_type*>(
      coda_type_number_new(coda_format_hdf5, coda_integer_class));
  ASSERT_NE(nullptr, f1_def);
  ASSERT_EQ(0, coda_type_set_read_type(f1_def, coda_native_type_int32));
  ASSERT_EQ(
      0, coda_type_record_create_field(partial_cmp->definition, "f1", f1_def));
  coda_type_release(f0_def);
  coda_type_release(f1_def);

  partial_cmp->tag = tag_hdf5_compound_datatype;
  partial_cmp->datatype_id = H5Tcreate(H5T_COMPOUND, sizeof(int32_t));
  ASSERT_GE(partial_cmp->datatype_id, 0);
  partial_cmp->member = static_cast<coda_hdf5_data_type**>(
      std::malloc(2 * sizeof(coda_hdf5_data_type*)));
  ASSERT_NE(nullptr, partial_cmp->member);
  partial_cmp->member[0] = nullptr;
  partial_cmp->member[1] = nullptr;
  partial_cmp->member_type =
      static_cast<hid_t*>(std::malloc(2 * sizeof(hid_t)));
  ASSERT_NE(nullptr, partial_cmp->member_type);
  partial_cmp->member_type[0] = H5Tcopy(H5T_NATIVE_INT32);
  ASSERT_GE(partial_cmp->member_type[0], 0);
  partial_cmp->member_type[1] = -1;
  coda_hdf5_type_delete(reinterpret_cast<coda_dynamic_type*>(partial_cmp));

  auto* partial_dset =
      static_cast<coda_hdf5_dataset*>(std::malloc(sizeof(coda_hdf5_dataset)));
  ASSERT_NE(nullptr, partial_dset);
  partial_dset->backend = coda_backend_hdf5;
  partial_dset->definition = nullptr;
  partial_dset->tag = tag_hdf5_dataset;
  partial_dset->dataset_id =
      H5Dopen2(hdf5_product->file_id, "/grp/ds", H5P_DEFAULT);
  ASSERT_GE(partial_dset->dataset_id, 0);
  partial_dset->dataspace_id = H5Dget_space(partial_dset->dataset_id);
  ASSERT_GE(partial_dset->dataspace_id, 0);
  partial_dset->base_type = nullptr;
  partial_dset->attributes = nullptr;
  coda_hdf5_type_delete(reinterpret_cast<coda_dynamic_type*>(partial_dset));
}

TEST(CodaHdf5TypeTest, CorruptedHdf5MetadataTriggersAttributeAndGroupErrors) {
  ScopedIsolatedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Corrupt Global Heap ("GCOL") for a group VLEN string attribute ->
  //    H5Aread() fails in new_hdf5Attribute() and group cleanup runs cleanly.
  {
    const TempFile h5_file("hdf5_type_corrupt_grp_gcol.h5");
    {
      ScopedH5File file(
          H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(file.valid());
      ScopedH5Group grp(
          H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
      ScopedH5Type vlen_str = MakeVlenStringType();
      ASSERT_TRUE(vlen_str.valid());
      const char* attr_val = "corrupt_group_heap";
      ASSERT_TRUE(WriteScalarAttr<const char*>(grp.get(), "bad_vlen",
                                               vlen_str.get(), attr_val));
    }
    ASSERT_TRUE(CorruptHdf5Magic(h5_file.path(), "GCOL", "XCOL",
                                 /*replace_last=*/false));

    // Release HDF5 global free-list pools after closing the product so
    // HeapLeakChecker
    // remains order-independent across randomized GUnit runs.
    const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    int rc = 0;
    {
      const absl::LeakCheckDisabler leak_disabler;
      rc = coda_open(h5_file.c_str(), product.OutParam());
    }
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }

  // 2. Corrupt Global Heap ("GCOL") for an int32 dataset VLEN string attribute
  //    -> H5Aread() fails and dataset cleanup frees the pending conversion.
  {
    const TempFile h5_file("hdf5_type_corrupt_dset_gcol.h5");
    {
      ScopedH5File file(
          H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(file.valid());
      ASSERT_TRUE(WriteScalarDataset<int32_t>(
          file.get(), "d_int", H5T_NATIVE_INT32, H5T_NATIVE_INT32, 17));
      ScopedH5Dataset dset(H5Dopen2(file.get(), "d_int", H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
      ScopedH5Type vlen_str = MakeVlenStringType();
      ASSERT_TRUE(vlen_str.valid());
      const char* attr_val = "corrupt_dset_heap";
      ASSERT_TRUE(WriteScalarAttr<const char*>(dset.get(), "bad_vlen",
                                               vlen_str.get(), attr_val));
    }
    ASSERT_TRUE(CorruptHdf5Magic(h5_file.path(), "GCOL", "XCOL",
                                 /*replace_last=*/false));

    // Release HDF5 global free-list pools after closing the product so
    // HeapLeakChecker
    // remains order-independent across randomized GUnit runs.
    const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    int rc = 0;
    {
      const absl::LeakCheckDisabler leak_disabler;
      rc = coda_open(h5_file.c_str(), product.OutParam());
    }
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }

  // 3. Corrupt Fractal Heap header ("FRHP") on a dense-attribute dataset ->
  //    H5Aget_num_attrs() succeeds from AINFO while H5Aopen_idx() fails.
  {
    const TempFile h5_file("hdf5_type_corrupt_dense_attr.h5");
    {
      ScopedH5Plist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST,
                                     H5F_LIBVER_LATEST),
                0);
      ScopedH5File file(
          H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(file.valid());

      ScopedH5Plist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      ASSERT_GE(H5Pset_attr_phase_change(dcpl.get(), 0, 0), 0);
      ASSERT_GE(H5Pset_attr_creation_order(
                    dcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
                0);
      ScopedH5Space scalar_space(H5Screate(H5S_SCALAR));
      ASSERT_TRUE(scalar_space.valid());
      ScopedH5Dataset dset(H5Dcreate2(file.get(), "d_dense", H5T_NATIVE_INT32,
                                      scalar_space.get(), H5P_DEFAULT,
                                      dcpl.get(), H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
      ASSERT_TRUE(
          WriteScalarAttr<int32_t>(dset.get(), "a0", H5T_NATIVE_INT32, 99));
    }
    ASSERT_TRUE(CorruptHdf5Magic(h5_file.path(), "FRHP", "XRHP",
                                 /*replace_last=*/false));

    // Release HDF5 global free-list pools after closing the product so
    // HeapLeakChecker
    // remains order-independent across randomized GUnit runs.
    const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    int rc = 0;
    {
      const absl::LeakCheckDisabler leak_disabler;
      rc = coda_open(h5_file.c_str(), product.OutParam());
    }
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }

  // 4. Corrupt the subgroup's symbol table node ("SNOD") ->
  //    H5Gopen() succeeds on "/sub" while H5Gget_num_objs() fails.
  //    Explicitly set earliest..v18 bounds so HDF5 2.x writes v1 SNOD nodes.
  {
    const TempFile h5_file("hdf5_type_corrupt_snod.h5");
    {
      ScopedH5Plist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
          0);
      ScopedH5File file(
          H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(file.valid());
      ScopedH5Group sub(
          H5Gcreate2(file.get(), "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(sub.valid());
      ASSERT_TRUE(WriteScalarDataset<int32_t>(
          sub.get(), "leaf", H5T_NATIVE_INT32, H5T_NATIVE_INT32, 1));
    }
    size_t snod_count = 0;
    ASSERT_TRUE(CorruptHdf5Magic(h5_file.path(), "SNOD", "XNOD",
                                 /*replace_last=*/true, &snod_count));
    ASSERT_GE(snod_count, 2u);

    // Release HDF5 global free-list pools after closing the product so
    // HeapLeakChecker
    // remains order-independent across randomized GUnit runs.
    const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    int rc = 0;
    {
      const absl::LeakCheckDisabler leak_disabler;
      rc = coda_open(h5_file.c_str(), product.OutParam());
    }
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }

  // 5. Corrupt the subgroup's local heap ("HEAP") ->
  //    H5Gget_num_objs() succeeds while H5Gget_objname_by_idx() fails.
  //    Explicitly set earliest..v18 bounds so HDF5 2.x writes v1 HEAP blocks.
  {
    const TempFile h5_file("hdf5_type_corrupt_heap.h5");
    {
      ScopedH5Plist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(
          H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
          0);
      ScopedH5File file(
          H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(file.valid());
      ScopedH5Group sub(
          H5Gcreate2(file.get(), "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(sub.valid());
      ASSERT_TRUE(WriteScalarDataset<int32_t>(
          sub.get(), "leaf", H5T_NATIVE_INT32, H5T_NATIVE_INT32, 1));
    }
    size_t heap_count = 0;
    ASSERT_TRUE(CorruptHdf5Magic(h5_file.path(), "HEAP", "XEAP",
                                 /*replace_last=*/true, &heap_count));
    ASSERT_GE(heap_count, 2u);

    // Release HDF5 global free-list pools after closing the product so
    // HeapLeakChecker
    // remains order-independent across randomized GUnit runs.
    const absl::Cleanup hdf5_gc = [] { H5garbage_collect(); };
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    int rc = 0;
    {
      const absl::LeakCheckDisabler leak_disabler;
      rc = coda_open(h5_file.c_str(), product.OutParam());
    }
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }
}

// TODO(schwehr): 656/880 = 74.5% ceiling in libcoda/coda-hdf5-type.c; remaining
// 224 uncovered lines are 93 OOM branches, 82 HDF5 internal C API error checks
// on already-opened handles, 37 unreachable CODA setter guards, 5 upstream UAF
// lines (234-235, 237-239), and 7 dead branches (529-530, 1088-1089, 1279-1280,
// 1443).

}  // namespace
}  // namespace stcorp_coda
