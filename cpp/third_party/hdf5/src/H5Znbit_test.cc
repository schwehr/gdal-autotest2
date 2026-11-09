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
// Tests the n-bit compression filter (third_party/hdf5/src/H5Znbit.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Znbit.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(hwaddress_sanitizer)
#define H5ZNBIT_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define H5ZNBIT_TEST_HAS_OTHER_SAN 1
#endif
#endif
#if !defined(H5ZNBIT_TEST_HAS_ASAN_OR_LSAN) &&                   \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__) || \
     defined(LEAK_SANITIZER) || defined(HWADDRESS_SANITIZER) ||     \
     defined(__SANITIZE_HWADDRESS__))
#define H5ZNBIT_TEST_HAS_ASAN_OR_LSAN 1
#endif
#if !defined(H5ZNBIT_TEST_HAS_OTHER_SAN) &&                 \
    (defined(MEMORY_SANITIZER) || defined(THREAD_SANITIZER) || \
     defined(__SANITIZE_THREAD__))
#define H5ZNBIT_TEST_HAS_OTHER_SAN 1
#endif

#if !defined(H5ZNBIT_TEST_HAS_ASAN_OR_LSAN) && \
    !defined(H5ZNBIT_TEST_HAS_OTHER_SAN)
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#endif

// `:hdf5` compiles with -DNDEBUG; match its layout across internal headers.
#ifndef NDEBUG
#define H5ZNBIT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5Z_FRIEND
#include "third_party/hdf5/src/H5Zpkg.h"
}  // extern "C"
#ifdef H5ZNBIT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ZNBIT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

// Builds a packed 32-bit float datatype with custom sign/exponent/mantissa
// layout inside [offset, offset + precision).
ScopedType MakePackedFloatType(hid_t base_float_type, size_t precision,
                               size_t offset) {
  ScopedType t(H5Tcopy(base_float_type));
  if (!t.valid()) return ScopedType();
  // Keep 8-bit exponent and adjust mantissa/sign to fit inside `precision`.
  // Minimum supported precision here is 10 (1 sign + 8 exp + 1 mantissa).
  if (precision < 10 || precision + offset > 32) return ScopedType();
  // Move fields to [0, precision) first while offset==0, shrink precision and
  // shift offset (keeping size==4), then shift fields into [offset, offset+precision).
  if (H5Tset_fields(t.get(), precision - 1, precision - 9, 8, 0,
                    precision - 9) < 0) {
    return ScopedType();
  }
  if (H5Tset_ebias(t.get(), 127) < 0) return ScopedType();
  if (H5Tset_precision(t.get(), precision) < 0) return ScopedType();
  if (H5Tset_offset(t.get(), offset) < 0) return ScopedType();
  if (H5Tset_fields(t.get(), offset + precision - 1, offset + precision - 9, 8,
                    offset, precision - 9) < 0) {
    return ScopedType();
  }
  return t;
}

// Writes and reads back raw packed bytes using identical file/memory datatype
// so no datatype conversion obscures the exact bit patterns processed by nbit.
void VerifyPackedBytesRoundTrip(hid_t file_id, const char* name, hid_t dtype_id,
                                hsize_t num_elems, hsize_t chunk_elems,
                                const std::vector<uint8_t>& input) {
  ScopedSpace space(H5Screate_simple(1, &num_elems, nullptr));
  ASSERT_TRUE(space.valid()) << name;

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid()) << name;
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &chunk_elems), 0) << name;
  ASSERT_GE(H5Pset_nbit(dcpl.get()), 0) << name;

  {
    ScopedDataset dset(H5Dcreate2(file_id, name, dtype_id, space.get(),
                                  H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(dset.valid()) << name;
    ASSERT_GE(H5Dwrite(dset.get(), dtype_id, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       input.data()),
              0)
        << name;
  }
  // Reopen after closing so the dataset chunk cache (rdcc) is discarded and
  // H5Dread executes H5Z_NBIT decompression on the stored compressed chunk.
  ScopedDataset reopen(H5Dopen2(file_id, name, H5P_DEFAULT));
  ASSERT_TRUE(reopen.valid()) << name;
  std::vector<uint8_t> output(input.size(), 0xa5);
  ASSERT_GE(H5Dread(reopen.get(), dtype_id, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    output.data()),
            0)
      << name;
  EXPECT_THAT(output, ElementsAreArray(input)) << name;
}

// Builds a zero-padded bit pattern occupying `[offset, offset + precision)` in
// either little-endian or big-endian byte order for a `elem_size`-byte word.
void FillBitPatternWord(uint8_t* dst, size_t elem_size, bool big_endian,
                        size_t precision, size_t offset, uint64_t seed) {
  std::memset(dst, 0, elem_size);
  const uint64_t mask = (precision >= 64) ? ~uint64_t{0}
                                          : ((uint64_t{1} << precision) - 1U);
  const uint64_t value = ((seed * 0x9e3779b97f4a7c15ULL) ^ 0x5a5a5a5a5a5a5a5aULL) &
                         mask;
  const uint64_t shifted = value << offset;
  for (size_t b = 0; b < elem_size; ++b) {
    const size_t shift = big_endian ? (elem_size - 1 - b) * 8 : b * 8;
    dst[b] = static_cast<uint8_t>((shifted >> shift) & 0xffU);
  }
}

TEST(H5ZnbitTest, FilterAvailableAndMetadata) {
  EXPECT_GT(H5Zfilter_avail(H5Z_FILTER_NBIT), 0);

  unsigned flags = 0;
  ASSERT_GE(H5Zget_filter_info(H5Z_FILTER_NBIT, &flags), 0);
  EXPECT_EQ(flags, static_cast<unsigned>(H5Z_FILTER_CONFIG_ENCODE_ENABLED |
                                         H5Z_FILTER_CONFIG_DECODE_ENABLED));

  EXPECT_EQ(H5Z_NBIT[0].version, H5Z_CLASS_T_VERS);
  EXPECT_EQ(H5Z_NBIT[0].id, H5Z_FILTER_NBIT);
  EXPECT_EQ(H5Z_NBIT[0].encoder_present, 1U);
  EXPECT_EQ(H5Z_NBIT[0].decoder_present, 1U);
  EXPECT_EQ(std::string_view(H5Z_NBIT[0].name), "nbit");
  ASSERT_NE(H5Z_NBIT[0].can_apply, nullptr);
  ASSERT_NE(H5Z_NBIT[0].set_local, nullptr);
  ASSERT_NE(H5Z_NBIT[0].filter, nullptr);
}

TEST(H5ZnbitTest, AtomicIntegersAndFloatsLittleAndBigEndian) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  struct AtomicCase {
    const char* name;
    hid_t base_type;
    bool is_float;
    bool big_endian;
    size_t precision;
    size_t offset;
    hsize_t num_elems;
    hsize_t chunk_elems;
  };

  // Matrix designed to hit all byte-index and buffer-bit boundary paths:
  // - single-byte (begin_i == end_i),
  // - multi-byte with interior full bytes (k == begin_i, interior, k == end_i),
  // - (precision + offset) % 8 == 0 vs != 0 (LE),
  // - offset % 8 == 0 vs != 0 (BE),
  // - bit widths (5, 7, 13, 19, 25) hitting dat_len == 0, *buf_len > dat_len,
  //   and *buf_len < dat_len across multiple elements in a chunk.
  const std::array<AtomicCase, 12> kCases = {{
      // Full precision (need_not_compress = true)
      {"int32_full_le", H5T_STD_U32LE, false, false, 32, 0, 16, 16},
      // LE single byte: begin_i == end_i == 0, non-byte-aligned bit packing
      {"int16_le_5p_2o", H5T_STD_U16LE, false, false, 5, 2, 25, 25},
      // LE multi-byte spanning 3 bytes (k=2,1,0) with (p+o)%8 == 0
      {"int32_le_19p_5o", H5T_STD_U32LE, false, false, 19, 5, 24, 24},
      // LE multi-byte spanning 3 bytes with (p+o)%8 != 0
      {"int32_le_18p_5o", H5T_STD_U32LE, false, false, 18, 5, 25, 25},
      // LE spanning 4 bytes (k=3,2,1,0) with interior byte hitting dat_len==0
      {"int32_le_24p_8o", H5T_STD_U32LE, false, false, 24, 8, 8, 8},
      // BE single byte: begin_i == end_i, offset%8 != 0
      {"int16_be_5p_3o", H5T_STD_U16BE, false, true, 5, 3, 25, 25},
      // BE multi-byte with offset % 8 == 0 (hits end_i = ... - 1)
      {"int32_be_17p_8o", H5T_STD_U32BE, false, true, 17, 8, 25, 25},
      // BE multi-byte with offset % 8 != 0 spanning bytes 0..3
      {"int32_be_20p_5o", H5T_STD_U32BE, false, true, 20, 5, 25, 25},
      // 8-bit packed integer where precision == 7 hits hanging bits at EOF
      {"int8_le_7p_1o", H5T_STD_U8LE, false, false, 7, 1, 17, 17},
      // Packed IEEE float LE (H5T_FLOAT switch arms)
      {"float32_le_19p_5o", H5T_IEEE_F32LE, true, false, 19, 5, 20, 20},
      // Packed IEEE float BE (H5T_FLOAT switch arms)
      {"float32_be_20p_4o", H5T_IEEE_F32BE, true, true, 20, 4, 20, 20},
      // Native float full precision
      {"float32_full", H5T_IEEE_F32LE, true, false, 32, 0, 12, 12},
  }};

  for (const auto& c : kCases) {
    ScopedType dtype;
    if (c.is_float && c.precision < 32) {
      dtype = MakePackedFloatType(c.base_type, c.precision, c.offset);
    } else {
      dtype = ScopedType(H5Tcopy(c.base_type));
      if (dtype.valid() && c.precision < H5Tget_size(dtype.get()) * 8) {
        ASSERT_GE(H5Tset_precision(dtype.get(), c.precision), 0) << c.name;
        ASSERT_GE(H5Tset_offset(dtype.get(), c.offset), 0) << c.name;
      }
    }
    ASSERT_TRUE(dtype.valid()) << c.name;

    const size_t elem_size = H5Tget_size(dtype.get());
    std::vector<uint8_t> data(static_cast<size_t>(c.num_elems) * elem_size);
    for (hsize_t i = 0; i < c.num_elems; ++i) {
      FillBitPatternWord(data.data() + static_cast<size_t>(i) * elem_size,
                         elem_size, c.big_endian, c.precision, c.offset,
                         i + 1);
    }
    VerifyPackedBytesRoundTrip(file.get(), c.name, dtype.get(), c.num_elems,
                               c.chunk_elems, data);
  }
}

TEST(H5ZnbitTest, NativeIntegerConversionRoundTripWithStorageSavings) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedType ftype(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(ftype.valid());
  ASSERT_GE(H5Tset_precision(ftype.get(), 11), 0);
  ASSERT_GE(H5Tset_offset(ftype.get(), 3), 0);

  constexpr hsize_t kNumElems = 512;
  constexpr hsize_t kChunkDims[1] = {128};
  ScopedSpace space(H5Screate_simple(1, &kNumElems, nullptr));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunkDims), 0);
  ASSERT_GE(H5Pset_nbit(dcpl.get()), 0);

  std::vector<int32_t> expected(kNumElems);
  for (hsize_t i = 0; i < kNumElems; ++i) {
    // Fits within signed 11-bit range [-1024, 1023].
    expected[i] = static_cast<int32_t>((i % 200) - 100);
  }
  {
    ScopedDataset dset(H5Dcreate2(file.get(), "conv_int", ftype.get(),
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, expected.data()),
              0);
  }

  ScopedDataset reopen(H5Dopen2(file.get(), "conv_int", H5P_DEFAULT));
  ASSERT_TRUE(reopen.valid());
  EXPECT_LT(H5Dget_storage_size(reopen.get()), kNumElems * sizeof(int32_t));

  std::vector<int32_t> actual(kNumElems, 0);
  ASSERT_GE(H5Dread(reopen.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, actual.data()),
            0);
  EXPECT_THAT(actual, ElementsAreArray(expected));
}

TEST(H5ZnbitTest, TopLevelArrayAndNestedArrayCompoundAtomic) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedType packed_i16(H5Tcopy(H5T_STD_U16LE));
  ASSERT_TRUE(packed_i16.valid());
  ASSERT_GE(H5Tset_precision(packed_i16.get(), 9), 0);
  ASSERT_GE(H5Tset_offset(packed_i16.get(), 3), 0);

  // 1. Array of atomic.
  const hsize_t arr_dims1[1] = {3};
  ScopedType arr_atomic(H5Tarray_create2(packed_i16.get(), 1, arr_dims1));
  ASSERT_TRUE(arr_atomic.valid());
  {
    constexpr hsize_t kNumElems = 11;
    std::vector<uint8_t> data(kNumElems * H5Tget_size(arr_atomic.get()));
    for (size_t i = 0; i < data.size() / 2; ++i) {
      FillBitPatternWord(data.data() + i * 2, 2, false, 9, 3, i + 1);
    }
    VerifyPackedBytesRoundTrip(file.get(), "arr_atomic", arr_atomic.get(),
                               kNumElems, kNumElems, data);
  }

  // 2. Nested array of array of atomic.
  const hsize_t arr_dims2[1] = {2};
  ScopedType nested_arr(H5Tarray_create2(arr_atomic.get(), 1, arr_dims2));
  ASSERT_TRUE(nested_arr.valid());
  {
    constexpr hsize_t kNumElems = 7;
    std::vector<uint8_t> data(kNumElems * H5Tget_size(nested_arr.get()));
    for (size_t i = 0; i < data.size() / 2; ++i) {
      FillBitPatternWord(data.data() + i * 2, 2, false, 9, 3, i + 101);
    }
    VerifyPackedBytesRoundTrip(file.get(), "nested_arr", nested_arr.get(),
                               kNumElems, kNumElems, data);
  }

  // 3. Array of compound (with packed atomic + nooptype).
  ScopedType inner_comp(H5Tcreate(H5T_COMPOUND, 4));
  ASSERT_TRUE(inner_comp.valid());
  ASSERT_GE(H5Tinsert(inner_comp.get(), "a", 0, packed_i16.get()), 0);
  ScopedType fstr(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(fstr.valid());
  ASSERT_GE(H5Tset_size(fstr.get(), 2), 0);
  ASSERT_GE(H5Tinsert(inner_comp.get(), "s", 2, fstr.get()), 0);

  ScopedType arr_comp(H5Tarray_create2(inner_comp.get(), 1, arr_dims2));
  ASSERT_TRUE(arr_comp.valid());
  {
    constexpr hsize_t kNumElems = 9;
    const size_t rec_sz = H5Tget_size(arr_comp.get());
    std::vector<uint8_t> data(kNumElems * rec_sz, 0);
    for (size_t i = 0; i < kNumElems; ++i) {
      for (size_t sub = 0; sub < 2; ++sub) {
        uint8_t* p = data.data() + i * rec_sz + sub * 4;
        FillBitPatternWord(p, 2, false, 9, 3, i * 2 + sub + 1);
        p[2] = static_cast<uint8_t>('A' + (i % 26));
        p[3] = static_cast<uint8_t>('0' + (sub % 10));
      }
    }
    VerifyPackedBytesRoundTrip(file.get(), "arr_comp", arr_comp.get(),
                               kNumElems, kNumElems, data);
  }
}

TEST(H5ZnbitTest, CompoundWithAllMemberClassesIncludingArraysAndNooptypes) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // Packed 5-bit atomic int (leaves buf_len = 3 on first element so subsequent
  // nooptype exercises non-byte-aligned buf_len != 8).
  ScopedType p5(H5Tcopy(H5T_STD_U8LE));
  ASSERT_TRUE(p5.valid());
  ASSERT_GE(H5Tset_precision(p5.get(), 5), 0);
  ASSERT_GE(H5Tset_offset(p5.get(), 1), 0);

  // Packed 3-bit atomic int (5 + 3 = 8 bits restores byte alignment buf_len = 8
  // before the next nooptype, hitting `if (dat_len == 0) continue`).
  ScopedType p3(H5Tcopy(H5T_STD_U8LE));
  ASSERT_TRUE(p3.valid());
  ASSERT_GE(H5Tset_precision(p3.get(), 3), 0);
  ASSERT_GE(H5Tset_offset(p3.get(), 2), 0);

  // Packed 19-bit BE float member.
  ScopedType pfloat = MakePackedFloatType(H5T_IEEE_F32BE, 19, 5);
  ASSERT_TRUE(pfloat.valid());

  // Array of nooptype (opaque[2]) inside compound so need_not_compress is false
  // and compress/decompress_one_array(H5Z_NBIT_NOOPTYPE) are executed.
  ScopedType opaque_t(H5Tcreate(H5T_OPAQUE, 3));
  ASSERT_TRUE(opaque_t.valid());
  const hsize_t arr_dim2[1] = {2};
  ScopedType arr_noop(H5Tarray_create2(opaque_t.get(), 1, arr_dim2));
  ASSERT_TRUE(arr_noop.valid());

  // Fixed string, enum, bitfield, time, reference, complex nooptypes.
  ScopedType str3(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str3.valid());
  ASSERT_GE(H5Tset_size(str3.get(), 3), 0);

  ScopedType enum_t(H5Tenum_create(H5T_STD_U16LE));
  ASSERT_TRUE(enum_t.valid());
  uint16_t ev0 = 7;
  uint16_t ev1 = 42;
  ASSERT_GE(H5Tenum_insert(enum_t.get(), "SEVEN", &ev0), 0);
  ASSERT_GE(H5Tenum_insert(enum_t.get(), "FORTYTWO", &ev1), 0);

  // Nested compound + nested array of compound.
  ScopedType sub_comp(H5Tcreate(H5T_COMPOUND, 3));
  ASSERT_TRUE(sub_comp.valid());
  ASSERT_GE(H5Tinsert(sub_comp.get(), "b8", 0, H5T_STD_B8LE), 0);
  ASSERT_GE(H5Tinsert(sub_comp.get(), "sub_p5", 1, p5.get()), 0);
  ASSERT_GE(H5Tinsert(sub_comp.get(), "sub_p3", 2, p3.get()), 0);
  ScopedType arr_sub_comp(H5Tarray_create2(sub_comp.get(), 1, arr_dim2));
  ASSERT_TRUE(arr_sub_comp.valid());

  // Build big compound assembling every H5Znbit class arm.
  size_t off = 0;
  const size_t o_p5 = off;
  off += H5Tget_size(p5.get());  // +1 = 1
  const size_t o_arr_noop = off;
  off += H5Tget_size(arr_noop.get());  // +6 = 7
  const size_t o_str3 = off;
  off += H5Tget_size(str3.get());  // +3 = 10
  const size_t o_p3 = off;
  off += H5Tget_size(p3.get());  // +1 = 11
  const size_t o_enum = off;
  off += H5Tget_size(enum_t.get());  // +2 = 13
  const size_t o_time = off;
  off += H5Tget_size(H5T_UNIX_D32LE);  // +4 = 17
  const size_t o_bitfield = off;
  off += H5Tget_size(H5T_STD_B16LE);  // +2 = 19
  const size_t o_complex = off;
  off += H5Tget_size(H5T_NATIVE_FLOAT_COMPLEX);  // +8 = 27
  const size_t o_ref = off;
  off += H5Tget_size(H5T_STD_REF_OBJ);  // +8 = 35
  const size_t o_pfloat = off;
  off += H5Tget_size(pfloat.get());  // +4
  const size_t o_sub_comp = off;
  off += H5Tget_size(sub_comp.get());  // +3
  const size_t o_arr_sub_comp = off;
  off += H5Tget_size(arr_sub_comp.get());  // +6

  ScopedType comp(H5Tcreate(H5T_COMPOUND, off));
  ASSERT_TRUE(comp.valid());
  ASSERT_GE(H5Tinsert(comp.get(), "p5", o_p5, p5.get()), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "arr_noop", o_arr_noop, arr_noop.get()), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "str3", o_str3, str3.get()), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "p3", o_p3, p3.get()), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "enum_v", o_enum, enum_t.get()), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "time_v", o_time, H5T_UNIX_D32LE), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "bitf_v", o_bitfield, H5T_STD_B16LE), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "cplx_v", o_complex,
                      H5T_NATIVE_FLOAT_COMPLEX),
            0);
  ASSERT_GE(H5Tinsert(comp.get(), "ref_v", o_ref, H5T_STD_REF_OBJ), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "pfloat", o_pfloat, pfloat.get()), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "sub_comp", o_sub_comp, sub_comp.get()), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "arr_sub_comp", o_arr_sub_comp,
                      arr_sub_comp.get()),
            0);

  constexpr hsize_t kNumElems = 11;
  std::vector<uint8_t> data(kNumElems * off, 0);
  for (size_t i = 0; i < kNumElems; ++i) {
    uint8_t* rec = data.data() + i * off;
    FillBitPatternWord(rec + o_p5, 1, false, 5, 1, i + 1);
    for (size_t k = 0; k < 6; ++k) {
      rec[o_arr_noop + k] = static_cast<uint8_t>((i * 11 + k) & 0xffU);
    }
    rec[o_str3 + 0] = static_cast<uint8_t>('x');
    rec[o_str3 + 1] = static_cast<uint8_t>('a' + (i % 26));
    rec[o_str3 + 2] = '\0';
    FillBitPatternWord(rec + o_p3, 1, false, 3, 2, i + 3);
    const uint16_t ev = (i % 2 == 0) ? ev0 : ev1;
    std::memcpy(rec + o_enum, &ev, 2);
    const uint32_t tv = static_cast<uint32_t>(1700000000U + i);
    std::memcpy(rec + o_time, &tv, 4);
    const uint16_t bv = static_cast<uint16_t>(0x1020U + i);
    std::memcpy(rec + o_bitfield, &bv, 2);
    const float cpx[2] = {static_cast<float>(i), -static_cast<float>(i) * 0.5f};
    std::memcpy(rec + o_complex, cpx, 8);
    // Keep reference bytes zeroed when using raw H5T_STD_REF buffer.
    FillBitPatternWord(rec + o_pfloat, 4, true, 19, 5, i + 5);
    rec[o_sub_comp + 0] = static_cast<uint8_t>(i & 0xffU);
    FillBitPatternWord(rec + o_sub_comp + 1, 1, false, 5, 1, i + 7);
    FillBitPatternWord(rec + o_sub_comp + 2, 1, false, 3, 2, i + 9);
    for (size_t s = 0; s < 2; ++s) {
      rec[o_arr_sub_comp + s * 3 + 0] = static_cast<uint8_t>(i + s);
      FillBitPatternWord(rec + o_arr_sub_comp + s * 3 + 1, 1, false, 5, 1,
                         i + s + 11);
      FillBitPatternWord(rec + o_arr_sub_comp + s * 3 + 2, 1, false, 3, 2,
                         i + s + 13);
    }
  }

  VerifyPackedBytesRoundTrip(file.get(), "rich_compound", comp.get(),
                             kNumElems, kNumElems, data);

  // Also test full-precision compound (need_not_compress = true branch).
  ScopedType full_comp(H5Tcreate(H5T_COMPOUND, 6));
  ASSERT_TRUE(full_comp.valid());
  ASSERT_GE(H5Tinsert(full_comp.get(), "i32", 0, H5T_STD_I32LE), 0);
  ASSERT_GE(H5Tinsert(full_comp.get(), "u16", 4, H5T_STD_U16LE), 0);
  std::vector<uint8_t> full_data(6 * 8, 0x3c);
  VerifyPackedBytesRoundTrip(file.get(), "full_compound", full_comp.get(), 8, 8,
                             full_data);
}

TEST(H5ZnbitTest, TopLevelNooptypesAndCompoundWithVlenMembers) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1. Top-level nooptypes exercise top-level calc/set no-op switch arms and
  // set need_not_compress = true (cd_values[1] = 1 -> no-op filter pass).
  ScopedType str4(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str4.valid());
  ASSERT_GE(H5Tset_size(str4.get(), 4), 0);

  ScopedType opaque2(H5Tcreate(H5T_OPAQUE, 2));
  ASSERT_TRUE(opaque2.valid());

  ScopedType enum1(H5Tenum_create(H5T_STD_I8LE));
  ASSERT_TRUE(enum1.valid());
  int8_t e0 = 1;
  ASSERT_GE(H5Tenum_insert(enum1.get(), "ONE", &e0), 0);

  ScopedType vlen_i(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_i.valid());

  const std::array<std::pair<const char*, hid_t>, 8> kTopNoops = {{
      {"noop_time", H5T_UNIX_D32LE},
      {"noop_str", str4.get()},
      {"noop_bitfield", H5T_STD_B16LE},
      {"noop_opaque", opaque2.get()},
      {"noop_ref", H5T_STD_REF_OBJ},
      {"noop_enum", enum1.get()},
      {"noop_vlen", vlen_i.get()},
      {"noop_complex", H5T_NATIVE_FLOAT_COMPLEX},
  }};
  for (const auto& [name, tid] : kTopNoops) {
    constexpr hsize_t kDim = 4;
    ScopedSpace sp(H5Screate_simple(1, &kDim, nullptr));
    ASSERT_TRUE(sp.valid()) << name;
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid()) << name;
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kDim), 0) << name;
    ASSERT_GE(H5Pset_nbit(dcpl.get()), 0) << name;
    ScopedDataset dset(H5Dcreate2(file.get(), name, tid, sp.get(), H5P_DEFAULT,
                                  dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(dset.valid()) << name;

    // Verify need_not_compress flag was recorded as 1 in modified local dcpl.
    ScopedPlist got_dcpl(H5Dget_create_plist(dset.get()));
    ASSERT_TRUE(got_dcpl.valid()) << name;
    unsigned filt_flags = 0;
    unsigned filt_config = 0;
    std::array<unsigned, 16> cd{};
    size_t cd_nelmts = cd.size();
    ASSERT_GE(H5Pget_filter_by_id2(got_dcpl.get(), H5Z_FILTER_NBIT, &filt_flags,
                                    &cd_nelmts, cd.data(), 0, nullptr,
                                    &filt_config),
              0)
        << name;
    ASSERT_EQ(cd_nelmts, 3U) << name;
    EXPECT_EQ(cd[1], 1U) << name;
  }

  // 2. Compound with H5T_VLEN as both non-last and last member + VL string
  // (exercises H5Z__set_parms_compound H5T_VLEN offset difference branches).
  struct VlenCompRec {
    int32_t tag;
    hvl_t vl_mid;
    const char* vl_str;
    hvl_t vl_last;
  };
  ScopedType vlstr(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vlstr.valid());
  ASSERT_GE(H5Tset_size(vlstr.get(), H5T_VARIABLE), 0);

  ScopedType comp_vlen(H5Tcreate(H5T_COMPOUND, sizeof(VlenCompRec)));
  ASSERT_TRUE(comp_vlen.valid());
  ASSERT_GE(H5Tinsert(comp_vlen.get(), "tag", offsetof(VlenCompRec, tag),
                      H5T_NATIVE_INT32),
            0);
  ASSERT_GE(H5Tinsert(comp_vlen.get(), "vl_mid", offsetof(VlenCompRec, vl_mid),
                      vlen_i.get()),
            0);
  ASSERT_GE(H5Tinsert(comp_vlen.get(), "vl_str", offsetof(VlenCompRec, vl_str),
                      vlstr.get()),
            0);
  ASSERT_GE(
      H5Tinsert(comp_vlen.get(), "vl_last", offsetof(VlenCompRec, vl_last),
                vlen_i.get()),
      0);

  constexpr hsize_t kDim = 2;
  ScopedSpace sp(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(sp.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kDim), 0);
  ASSERT_GE(H5Pset_nbit(dcpl.get()), 0);
  std::array<int32_t, 2> v0 = {10, 20};
  std::array<int32_t, 3> v1 = {30, 40, 50};
  std::array<VlenCompRec, 2> write_recs = {{
      {1, {v0.size(), v0.data()}, "alpha", {v1.size(), v1.data()}},
      {2, {v1.size(), v1.data()}, "beta", {v0.size(), v0.data()}},
  }};
  {
    ScopedDataset dset(H5Dcreate2(file.get(), "comp_vlen", comp_vlen.get(),
                                  sp.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset.get(), comp_vlen.get(), H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, write_recs.data()),
              0);
  }

  ScopedDataset reopen(H5Dopen2(file.get(), "comp_vlen", H5P_DEFAULT));
  ASSERT_TRUE(reopen.valid());
  std::array<VlenCompRec, 2> read_recs{};
  ASSERT_GE(H5Dread(reopen.get(), comp_vlen.get(), H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, read_recs.data()),
            0);
  absl::Cleanup reclaim = [&] {
    H5Treclaim(comp_vlen.get(), sp.get(), H5P_DEFAULT, read_recs.data());
  };
  EXPECT_EQ(read_recs[0].tag, 1);
  EXPECT_STREQ(read_recs[0].vl_str, "alpha");
  ASSERT_EQ(read_recs[0].vl_mid.len, 2U);
  EXPECT_EQ(static_cast<int32_t*>(read_recs[0].vl_mid.p)[1], 20);
  ASSERT_EQ(read_recs[1].vl_last.len, 2U);
  EXPECT_EQ(static_cast<int32_t*>(read_recs[1].vl_last.p)[0], 10);
}

TEST(H5ZnbitTest, CanApplyAndSetLocalErrorPathsAndParmLimit) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDim = 4;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kDim), 0);
  ASSERT_GE(H5Pset_nbit(dcpl.get()), 0);

  ScopedErrorSilence silence;

  // 1. Direct H5Z_NBIT[0].can_apply invalid datatype IDs.
  EXPECT_LT(H5Z_NBIT[0].can_apply(dcpl.get(), H5I_INVALID_HID, space.get()), 0);
  EXPECT_LT(H5Z_NBIT[0].can_apply(dcpl.get(), space.get(), space.get()), 0);
  EXPECT_GT(H5Z_NBIT[0].can_apply(dcpl.get(), H5T_NATIVE_INT32, space.get()),
            0);

  // 2. Direct H5Z_NBIT[0].set_local invalid IDs and missing filter.
  EXPECT_LT(H5Z_NBIT[0].set_local(dcpl.get(), H5I_INVALID_HID, space.get()), 0);
  EXPECT_LT(H5Z_NBIT[0].set_local(H5I_INVALID_HID, H5T_NATIVE_INT32,
                                  space.get()),
            0);
  EXPECT_LT(H5Z_NBIT[0].set_local(dcpl.get(), H5T_NATIVE_INT32,
                                  H5I_INVALID_HID),
            0);
  ScopedPlist no_nbit_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(no_nbit_dcpl.valid());
  EXPECT_LT(H5Z_NBIT[0].set_local(no_nbit_dcpl.get(), H5T_NATIVE_INT32,
                                  space.get()),
            0);

  // 3. Unsupported byte order (H5T_ORDER_VAX) directly, in array, and in
  // compound exercises set_parms_atomic / set_parms_array / set_parms_compound
  // failure paths.
  EXPECT_LT(H5Z_NBIT[0].set_local(dcpl.get(), H5T_VAX_F32, space.get()), 0);

  const hsize_t dim2[1] = {2};
  ScopedType arr_vax(H5Tarray_create2(H5T_VAX_F32, 1, dim2));
  ASSERT_TRUE(arr_vax.valid());
  EXPECT_LT(H5Z_NBIT[0].set_local(dcpl.get(), arr_vax.get(), space.get()), 0);

  ScopedType nested_arr_vax(H5Tarray_create2(arr_vax.get(), 1, dim2));
  ASSERT_TRUE(nested_arr_vax.valid());
  EXPECT_LT(
      H5Z_NBIT[0].set_local(dcpl.get(), nested_arr_vax.get(), space.get()), 0);

  ScopedType comp_vax(H5Tcreate(H5T_COMPOUND, 4));
  ASSERT_TRUE(comp_vax.valid());
  ASSERT_GE(H5Tinsert(comp_vax.get(), "v", 0, H5T_VAX_F32), 0);
  EXPECT_LT(H5Z_NBIT[0].set_local(dcpl.get(), comp_vax.get(), space.get()), 0);

  ScopedType arr_comp_vax(H5Tarray_create2(comp_vax.get(), 1, dim2));
  ASSERT_TRUE(arr_comp_vax.valid());
  EXPECT_LT(H5Z_NBIT[0].set_local(dcpl.get(), arr_comp_vax.get(), space.get()),
            0);

  ScopedType nested_comp_vax(H5Tcreate(H5T_COMPOUND, 4));
  ASSERT_TRUE(nested_comp_vax.valid());
  ASSERT_GE(H5Tinsert(nested_comp_vax.get(), "c", 0, comp_vax.get()), 0);
  EXPECT_LT(
      H5Z_NBIT[0].set_local(dcpl.get(), nested_comp_vax.get(), space.get()), 0);

  // 4. Array of VL datatype and array of VL string hit H5Z__set_parms_array
  // unsupported base class rejection (line 562), and compound containing it
  // hits H5Z__set_parms_compound array-error propagation (line 670).
  ScopedType vlen_t(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_t.valid());
  ScopedType arr_vlen(H5Tarray_create2(vlen_t.get(), 1, dim2));
  ASSERT_TRUE(arr_vlen.valid());
  EXPECT_LT(H5Z_NBIT[0].set_local(dcpl.get(), arr_vlen.get(), space.get()), 0);

  ScopedType comp_arr_vlen(H5Tcreate(H5T_COMPOUND, H5Tget_size(arr_vlen.get())));
  ASSERT_TRUE(comp_arr_vlen.valid());
  ASSERT_GE(H5Tinsert(comp_arr_vlen.get(), "av", 0, arr_vlen.get()), 0);
  EXPECT_LT(H5Z_NBIT[0].set_local(dcpl.get(), comp_arr_vlen.get(), space.get()),
            0);

  // 5. Exceed H5Z_NBIT_MAX_NPARMS (4096) with a compound of 700 uint8 members
  // (3 + 3 + 700 * 6 = 4206 > 4096).
  constexpr size_t kManyMembers = 700;
  ScopedType huge_comp(H5Tcreate(H5T_COMPOUND, kManyMembers));
  ASSERT_TRUE(huge_comp.valid());
  for (size_t i = 0; i < kManyMembers; ++i) {
    const std::string mname = "m" + std::to_string(i);
    ASSERT_GE(H5Tinsert(huge_comp.get(), mname.c_str(), i, H5T_STD_U8LE), 0);
  }
  EXPECT_LT(H5Z_NBIT[0].set_local(dcpl.get(), huge_comp.get(), space.get()), 0);
}

TEST(H5ZnbitTest, DirectFilterRejectsInvalidCdValuesOnDecompress) {
  constexpr size_t kBufBytes = 16;
  void* buf = H5allocate_memory(kBufBytes, true);
  ASSERT_NE(buf, nullptr);
  absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };

  ScopedErrorSilence silence;
  size_t buf_size = kBufBytes;

  // 1. Mismatched cd_nelmts != cd_values[0].
  const std::array<unsigned, 3> mismatch_cd = {99, 0, 1};
  EXPECT_EQ(H5Z_NBIT[0].filter(0, mismatch_cd.size(), mismatch_cd.data(),
                               kBufBytes, &buf_size, &buf),
            0U);

  // 2. Top-level atomic invalid precision + offset > size * 8.
  // parms: [nparms=8, need_not_compress=0, d_nelmts=1, ATOMIC=1, size=2,
  //         order=LE(0), precision=15, offset=4]
  const std::array<unsigned, 8> bad_atomic = {8, 0, 1, 1, 2, 0, 15, 4};
  EXPECT_EQ(H5Z_NBIT[0].filter(H5Z_FLAG_REVERSE, bad_atomic.size(),
                               bad_atomic.data(), 4, &buf_size, &buf),
            0U);

  // 3. Top-level array -> atomic with invalid precision + offset.
  // [nparms=10, 0, 1, ARRAY=2, total_size=4, ATOMIC=1, size=2, LE=0, 15, 4]
  const std::array<unsigned, 10> bad_arr_atomic = {
      10, 0, 1, 2, 4, 1, 2, 0, 15, 4};
  EXPECT_EQ(H5Z_NBIT[0].filter(H5Z_FLAG_REVERSE, bad_arr_atomic.size(),
                               bad_arr_atomic.data(), 4, &buf_size, &buf),
            0U);

  // 4. Top-level array -> nested array -> atomic invalid precision + offset.
  // [nparms=12, 0, 1, ARRAY=2, total=8, ARRAY=2, base_total=4, ATOMIC=1, 2, 0,
  //  15, 4]
  const std::array<unsigned, 12> bad_nested_arr = {
      12, 0, 1, 2, 8, 2, 4, 1, 2, 0, 15, 4};
  EXPECT_EQ(H5Z_NBIT[0].filter(H5Z_FLAG_REVERSE, bad_nested_arr.size(),
                               bad_nested_arr.data(), 4, &buf_size, &buf),
            0U);

  // 5. Top-level array -> compound -> atomic invalid precision + offset.
  // [nparms=14, 0, 1, ARRAY=2, total=4, COMPOUND=3, comp_sz=2, nmem=1, off=0,
  //  ATOMIC=1, sz=2, LE=0, prec=15, off=4]
  const std::array<unsigned, 14> bad_arr_comp = {
      14, 0, 1, 2, 4, 3, 2, 1, 0, 1, 2, 0, 15, 4};
  EXPECT_EQ(H5Z_NBIT[0].filter(H5Z_FLAG_REVERSE, bad_arr_comp.size(),
                               bad_arr_comp.data(), 4, &buf_size, &buf),
            0U);

  // 6. Compound used_size > size.
  const std::array<unsigned, 12> bad_comp_used_gt = {
      12, 0, 1, 3, 2, 1, 0, 1, 4, 0, 8, 0};
  EXPECT_EQ(H5Z_NBIT[0].filter(H5Z_FLAG_REVERSE, bad_comp_used_gt.size(),
                               bad_comp_used_gt.data(), 4, &buf_size, &buf),
            0U);

  // 7. Compound member_size == 0 (used_size <= prev_used_size).
  const std::array<unsigned, 12> bad_comp_zero_sz = {
      12, 0, 1, 3, 4, 1, 0, 1, 0, 0, 8, 0};
  EXPECT_EQ(H5Z_NBIT[0].filter(H5Z_FLAG_REVERSE, bad_comp_zero_sz.size(),
                               bad_comp_zero_sz.data(), 4, &buf_size, &buf),
            0U);

  // 8. Compound member_offset + member_size > size.
  const std::array<unsigned, 12> bad_comp_off_overflow = {
      12, 0, 1, 3, 4, 1, 3, 1, 2, 0, 8, 0};
  EXPECT_EQ(H5Z_NBIT[0].filter(H5Z_FLAG_REVERSE, bad_comp_off_overflow.size(),
                               bad_comp_off_overflow.data(), 4, &buf_size,
                               &buf),
            0U);

  // 9. Compound atomic invalid precision + offset.
  const std::array<unsigned, 12> bad_comp_atomic = {
      12, 0, 1, 3, 4, 1, 0, 1, 2, 0, 15, 4};
  EXPECT_EQ(H5Z_NBIT[0].filter(H5Z_FLAG_REVERSE, bad_comp_atomic.size(),
                               bad_comp_atomic.data(), 4, &buf_size, &buf),
            0U);

  // 10. Compound -> array -> bad atomic.
  const std::array<unsigned, 14> bad_comp_arr = {
      14, 0, 1, 3, 4, 1, 0, 2, 4, 1, 2, 0, 15, 4};
  EXPECT_EQ(H5Z_NBIT[0].filter(H5Z_FLAG_REVERSE, bad_comp_arr.size(),
                               bad_comp_arr.data(), 4, &buf_size, &buf),
            0U);

  // 11. Compound -> nested compound -> bad atomic.
  const std::array<unsigned, 16> bad_comp_nested = {
      16, 0, 1, 3, 4, 1, 0, 3, 4, 1, 0, 1, 2, 0, 15, 4};
  EXPECT_EQ(H5Z_NBIT[0].filter(H5Z_FLAG_REVERSE, bad_comp_nested.size(),
                               bad_comp_nested.data(), 4, &buf_size, &buf),
            0U);
}

#if !defined(H5ZNBIT_TEST_HAS_ASAN_OR_LSAN) && \
    !defined(H5ZNBIT_TEST_HAS_OTHER_SAN)
TEST(H5ZnbitTest, DirectFilterHandlesAllocationFailuresCleanly) {
  ASSERT_GE(H5open(), 0);

  const std::string prev_style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  absl::Cleanup restore_style = [&] {
    GTEST_FLAG_SET(death_test_style, prev_style);
  };

  const auto run_oom_checks = []() {
    bool all_ok = false;
    {
      ScopedErrorSilence silence;
      constexpr unsigned kLargeElems = 4U << 20;
      constexpr size_t kLargeBytes = static_cast<size_t>(kLargeElems) * 4U;

      void* small_in = H5allocate_memory(64, true);
      if (small_in == nullptr) {
        std::exit(1);
      }

      std::ifstream statm("/proc/self/statm");
      unsigned long vm_pages = 0;
      if (!(statm >> vm_pages)) {
        std::exit(2);
      }
      const long page_sz = ::sysconf(_SC_PAGESIZE);
      if (page_sz <= 0) {
        std::exit(3);
      }
      const rlim_t vm_bytes =
          static_cast<rlim_t>(vm_pages) * static_cast<rlim_t>(page_sz);

      struct rlimit old_lim{};
      if (::getrlimit(RLIMIT_AS, &old_lim) != 0) {
        std::exit(4);
      }
      struct rlimit tight_lim = old_lim;
      tight_lim.rlim_cur = std::min<rlim_t>(
          old_lim.rlim_max, vm_bytes + static_cast<rlim_t>(4ULL << 20));
      if (::setrlimit(RLIMIT_AS, &tight_lim) != 0) {
        std::exit(5);
      }

      std::vector<void*> drain;
      drain.reserve(256);
      for (int i = 0; i < 256; ++i) {
        void* p = std::malloc(kLargeBytes);
        if (p == nullptr) {
          break;
        }
        drain.push_back(p);
      }

      // [nparms=8, need_not_compress=0, d_nelmts=kLargeElems, ATOMIC=1,
      //  size=4, order=LE(0), precision=16, offset=0]
      const std::array<unsigned, 8> oom_cd = {
          8, 0, kLargeElems, 1, 4, 0, 16, 0};

      size_t fwd_buf_size = kLargeBytes;
      const size_t fwd_rc = H5Z_NBIT[0].filter(
          0, oom_cd.size(), oom_cd.data(), kLargeBytes, &fwd_buf_size,
          &small_in);

      size_t rev_buf_size = 64;
      const size_t rev_rc = H5Z_NBIT[0].filter(
          H5Z_FLAG_REVERSE, oom_cd.size(), oom_cd.data(), 64, &rev_buf_size,
          &small_in);

      ::setrlimit(RLIMIT_AS, &old_lim);
      for (void* p : drain) {
        std::free(p);
      }
      H5free_memory(small_in);

      all_ok = (fwd_rc == 0 && rev_rc == 0);
    }
    std::exit(all_ok ? 0 : 6);
  };

  EXPECT_EXIT(run_oom_checks(), ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace hdf5
