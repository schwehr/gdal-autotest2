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
// Tests the scale-offset data filter (third_party/hdf5/src/H5Zscaleoffset.c),
// including registration metadata, can_apply / set_local validation,
// variable-bit-width packing/unpacking across all bit boundaries, integer
// (signed/unsigned 8/16/32/64-bit) and floating-point (float/double D-scale)
// round-trips with defined and undefined fill values, big-endian byte order
// conversion, full-precision fallbacks, and corrupt-stream error handling.
//
// Google3 build notes: little-endian LP64 host (H5_SIZEOF_LONG ==
// H5_SIZEOF_LONG_LONG == 8) compiled with -DNDEBUG.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Zscaleoffset.c

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its struct layout across internal
// headers included via hdf5_internal_prelude.h.
#ifndef NDEBUG
#define H5ZSCALEOFFSET_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5Z_FRIEND
#include "third_party/hdf5/src/H5Zpkg.h"
}  // extern "C"
#ifdef H5ZSCALEOFFSET_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ZSCALEOFFSET_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

constexpr size_t kScaleOffsetTotalNparms = 20;
constexpr size_t kScaleOffsetHeaderBytes = 21;

TEST(H5ZscaleoffsetTest, FilterIsRegisteredWithExpectedMetadata) {
  ASSERT_GE(H5open(), 0);

  EXPECT_GT(H5Zfilter_avail(H5Z_FILTER_SCALEOFFSET), 0);

  unsigned int filter_flags = 0;
  ASSERT_GE(H5Zget_filter_info(H5Z_FILTER_SCALEOFFSET, &filter_flags), 0);
  EXPECT_NE(filter_flags & H5Z_FILTER_CONFIG_ENCODE_ENABLED, 0U);
  EXPECT_NE(filter_flags & H5Z_FILTER_CONFIG_DECODE_ENABLED, 0U);

  EXPECT_EQ(H5Z_SCALEOFFSET[0].version, H5Z_CLASS_T_VERS);
  EXPECT_EQ(H5Z_SCALEOFFSET[0].id, H5Z_FILTER_SCALEOFFSET);
  EXPECT_EQ(H5Z_SCALEOFFSET[0].encoder_present, 1U);
  EXPECT_EQ(H5Z_SCALEOFFSET[0].decoder_present, 1U);
  EXPECT_STREQ(H5Z_SCALEOFFSET[0].name, "scaleoffset");
  EXPECT_NE(H5Z_SCALEOFFSET[0].can_apply, nullptr);
  EXPECT_NE(H5Z_SCALEOFFSET[0].set_local, nullptr);
  EXPECT_NE(H5Z_SCALEOFFSET[0].filter, nullptr);
}

TEST(H5ZscaleoffsetTest, CanApplyValidatesDatatypeClassAndOrder) {
  ASSERT_GE(H5open(), 0);

  const hid_t valid_types[] = {
      H5T_STD_I8LE,   H5T_STD_U16BE,  H5T_STD_I32LE,  H5T_STD_U64BE,
      H5T_IEEE_F32LE, H5T_IEEE_F64BE,
  };
  for (hid_t tid : valid_types) {
    EXPECT_GT(
        H5Z_SCALEOFFSET[0].can_apply(H5I_INVALID_HID, tid, H5I_INVALID_HID),
        0);
  }

  ScopedErrorSilence silence;
  EXPECT_LT(H5Z_SCALEOFFSET[0].can_apply(H5I_INVALID_HID, H5I_INVALID_HID,
                                         H5I_INVALID_HID),
            0);

  // Unsupported datatype classes return false (0).
  EXPECT_EQ(H5Z_SCALEOFFSET[0].can_apply(H5I_INVALID_HID, H5T_C_S1,
                                         H5I_INVALID_HID),
            0);
  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opaque.valid());
  EXPECT_EQ(H5Z_SCALEOFFSET[0].can_apply(H5I_INVALID_HID, opaque.get(),
                                         H5I_INVALID_HID),
            0);

  // Integer type with unsupported VAX byte order returns false (0).
  ScopedType vax_int(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(vax_int.valid());
  ASSERT_GE(H5Tset_order(vax_int.get(), H5T_ORDER_VAX), 0);
  EXPECT_EQ(H5Z_SCALEOFFSET[0].can_apply(H5I_INVALID_HID, vax_int.get(),
                                         H5I_INVALID_HID),
            0);
}

TEST(H5ZscaleoffsetTest, SetLocalValidatesInputsAndUnsupportedSizes) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  constexpr hsize_t kDim = 8;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());

  // Bad dcpl_id and missing filter.
  EXPECT_LT(H5Z_SCALEOFFSET[0].set_local(H5I_INVALID_HID, H5T_NATIVE_INT32,
                                         space.get()),
            0);
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  EXPECT_LT(
      H5Z_SCALEOFFSET[0].set_local(fapl.get(), H5T_NATIVE_INT32, space.get()),
      0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  EXPECT_LT(
      H5Z_SCALEOFFSET[0].set_local(dcpl.get(), H5T_NATIVE_INT32, space.get()),
      0);

  ASSERT_GE(H5Pset_scaleoffset(dcpl.get(), H5Z_SO_INT, 0), 0);

  // Invalid type_id, invalid space_id, unsupported class, unsupported order.
  EXPECT_LT(
      H5Z_SCALEOFFSET[0].set_local(dcpl.get(), H5I_INVALID_HID, space.get()),
      0);
  EXPECT_LT(H5Z_SCALEOFFSET[0].set_local(dcpl.get(), H5T_NATIVE_INT32,
                                         H5I_INVALID_HID),
            0);
  EXPECT_LT(H5Z_SCALEOFFSET[0].set_local(dcpl.get(), H5T_C_S1, space.get()), 0);

  ScopedType vax_int(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(vax_int.valid());
  ASSERT_GE(H5Tset_order(vax_int.get(), H5T_ORDER_VAX), 0);
  EXPECT_LT(
      H5Z_SCALEOFFSET[0].set_local(dcpl.get(), vax_int.get(), space.get()), 0);

  // Unsupported integer/float sizes with defined fill value fail in
  // H5Z__scaleoffset_get_type inside set_local.
  ScopedType u24(H5Tcopy(H5T_STD_U32LE));
  ASSERT_TRUE(u24.valid());
  ASSERT_GE(H5Tset_size(u24.get(), 3), 0);
  EXPECT_LT(H5Z_SCALEOFFSET[0].set_local(dcpl.get(), u24.get(), space.get()),
            0);

  ScopedType i24(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(i24.valid());
  ASSERT_GE(H5Tset_size(i24.get(), 3), 0);
  EXPECT_LT(H5Z_SCALEOFFSET[0].set_local(dcpl.get(), i24.get(), space.get()),
            0);

  ScopedPlist dcpl_flt(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_flt.valid());
  ASSERT_GE(H5Pset_scaleoffset(dcpl_flt.get(), H5Z_SO_FLOAT_DSCALE, 2), 0);
  EXPECT_LT(H5Z_SCALEOFFSET[0].set_local(dcpl_flt.get(), H5T_NATIVE_LDOUBLE,
                                         space.get()),
            0);
}

TEST(H5ZscaleoffsetTest, DirectFilterRejectsInvalidParamsAndCorruptStreams) {
  ASSERT_GE(H5open(), 0);

  auto make_cd = [](unsigned int scale_type, unsigned int scale_factor,
                    unsigned int nelmts, unsigned int cls, unsigned int size,
                    unsigned int sign, unsigned int order,
                    unsigned int filavail) {
    std::vector<unsigned int> cd(kScaleOffsetTotalNparms, 0);
    cd[0] = scale_type;
    cd[1] = scale_factor;
    cd[2] = nelmts;
    cd[3] = cls;
    cd[4] = size;
    cd[5] = sign;
    cd[6] = order;
    cd[7] = filavail;
    return cd;
  };

  auto alloc_buf = [](size_t bytes) {
    void* p = H5allocate_memory(bytes, true);
    return p;
  };

  {
    ScopedErrorSilence silence;
    auto cd = make_cd(H5Z_SO_INT, 0, 4, 0, 4, 1, 0, 0);
    void* buf = alloc_buf(16);
    ASSERT_NE(buf, nullptr);
    absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
    size_t buf_size = 16;

    // 1. cd_nelmts != 20.
    EXPECT_EQ(
        H5Z_SCALEOFFSET[0].filter(0, 19, cd.data(), 16, &buf_size, &buf), 0U);

    // 2. Float class with H5Z_SO_INT scale type.
    auto bad_flt_type = make_cd(H5Z_SO_INT, 0, 4, 1, 4, 0, 0, 0);
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(0, kScaleOffsetTotalNparms,
                                        bad_flt_type.data(), 16, &buf_size,
                                        &buf),
              0U);

    // 3. Integer class with H5Z_SO_FLOAT_DSCALE scale type.
    auto bad_int_type = make_cd(H5Z_SO_FLOAT_DSCALE, 2, 4, 0, 4, 1, 0, 0);
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(0, kScaleOffsetTotalNparms,
                                        bad_int_type.data(), 16, &buf_size,
                                        &buf),
              0U);

    // 4. Unsupported E-scaling method.
    auto escale = make_cd(H5Z_SO_FLOAT_ESCALE, 8, 4, 1, 4, 0, 0, 0);
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(0, kScaleOffsetTotalNparms,
                                        escale.data(), 16, &buf_size, &buf),
              0U);

    // 5. scale_factor > size * 8 for integer.
    auto too_many_bits = make_cd(H5Z_SO_INT, 33, 4, 0, 4, 1, 0, 0);
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(0, kScaleOffsetTotalNparms,
                                        too_many_bits.data(), 16, &buf_size,
                                        &buf),
              0U);

    // 6. Unsupported datatype size (3 bytes) in compress & decompress.
    void* buf12 = alloc_buf(12);
    ASSERT_NE(buf12, nullptr);
    absl::Cleanup free_buf12 = [&buf12] { H5free_memory(buf12); };
    size_t size12 = 12;
    auto bad_size = make_cd(H5Z_SO_INT, 0, 4, 0, 3, 0, 0, 0);
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(0, kScaleOffsetTotalNparms,
                                        bad_size.data(), 12, &size12, &buf12),
              0U);
    // Construct valid 21-byte header with minbits=4, minval_size=8 for
    // reverse bad-size check.
    void* hdr_buf = alloc_buf(32);
    ASSERT_NE(hdr_buf, nullptr);
    absl::Cleanup free_hdr = [&hdr_buf] { H5free_memory(hdr_buf); };
    auto* bytes = static_cast<uint8_t*>(hdr_buf);
    bytes[0] = 4;
    bytes[4] = 8;
    size_t hdr_size = 32;
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(H5Z_FLAG_REVERSE,
                                        kScaleOffsetTotalNparms,
                                        bad_size.data(), 32, &hdr_size,
                                        &hdr_buf),
              0U);
  }

  // 7. scale_factor == size * 8 is a no-op pass-through returning *buf_size.
  {
    auto cd_noop = make_cd(H5Z_SO_INT, 32, 4, 0, 4, 1, 0, 0);
    void* buf = alloc_buf(16);
    ASSERT_NE(buf, nullptr);
    absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
    size_t buf_size = 16;
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(0, kScaleOffsetTotalNparms,
                                        cd_noop.data(), 16, &buf_size, &buf),
              16U);
  }

  // 8. Negative scale_factor for integer resets to 0 and compresses cleanly;
  //    also test decompressing a stream written with smaller encoded minval
  //    width (header byte 4 < sizeof(unsigned long long)).
  {
    auto cd_neg = make_cd(H5Z_SO_INT, static_cast<unsigned int>(-5), 4, 0, 4,
                          1, 0, 0);
    void* buf = alloc_buf(16);
    ASSERT_NE(buf, nullptr);
    absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
    int32_t vals[4] = {10, 12, 14, 16};
    std::memcpy(buf, vals, sizeof(vals));
    size_t buf_size = 16;
    const size_t comp = H5Z_SCALEOFFSET[0].filter(
        0, kScaleOffsetTotalNparms, cd_neg.data(), 16, &buf_size, &buf);
    ASSERT_GT(comp, kScaleOffsetHeaderBytes);

    // Clone compressed buffer and set encoded minval byte width = 4 (< 8).
    void* buf_narrow_mv = alloc_buf(comp);
    ASSERT_NE(buf_narrow_mv, nullptr);
    absl::Cleanup free_narrow = [&buf_narrow_mv] {
      H5free_memory(buf_narrow_mv);
    };
    std::memcpy(buf_narrow_mv, buf, comp);
    static_cast<uint8_t*>(buf_narrow_mv)[4] = 4;
    size_t narrow_size = comp;

    ASSERT_EQ(H5Z_SCALEOFFSET[0].filter(H5Z_FLAG_REVERSE,
                                        kScaleOffsetTotalNparms, cd_neg.data(),
                                        comp, &buf_size, &buf),
              16U);
    int32_t out[4] = {};
    std::memcpy(out, buf, sizeof(out));
    EXPECT_THAT(out, ElementsAreArray(vals));

    ASSERT_EQ(H5Z_SCALEOFFSET[0].filter(H5Z_FLAG_REVERSE,
                                        kScaleOffsetTotalNparms, cd_neg.data(),
                                        comp, &narrow_size, &buf_narrow_mv),
              16U);
    int32_t narrow_out[4] = {};
    std::memcpy(narrow_out, buf_narrow_mv, sizeof(narrow_out));
    EXPECT_THAT(narrow_out, ElementsAreArray(vals));
  }

  // 9. Decompress short-buffer / corrupt-header / mid-element truncation errors.
  {
    ScopedErrorSilence silence;
    auto cd = make_cd(H5Z_SO_INT, 0, 4, 0, 4, 1, 0, 0);

    // < 5 bytes fails initial buffer check.
    void* short4 = alloc_buf(4);
    ASSERT_NE(short4, nullptr);
    absl::Cleanup free4 = [&short4] { H5free_memory(short4); };
    size_t sz4 = 4;
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(H5Z_FLAG_REVERSE,
                                        kScaleOffsetTotalNparms, cd.data(), 4,
                                        &sz4, &short4),
              0U);

    // minbits > p.size * 8 fails header check.
    void* bad_mb = alloc_buf(24);
    ASSERT_NE(bad_mb, nullptr);
    absl::Cleanup free_mb = [&bad_mb] { H5free_memory(bad_mb); };
    static_cast<uint8_t*>(bad_mb)[0] = 33;
    size_t sz_mb = 24;
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(H5Z_FLAG_REVERSE,
                                        kScaleOffsetTotalNparms, cd.data(), 24,
                                        &sz_mb, &bad_mb),
              0U);

    // 5 + minval_size > buf_size fails minval length check.
    void* short_mv = alloc_buf(10);
    ASSERT_NE(short_mv, nullptr);
    absl::Cleanup free_mv = [&short_mv] { H5free_memory(short_mv); };
    static_cast<uint8_t*>(short_mv)[0] = 4;
    static_cast<uint8_t*>(short_mv)[4] = 8;
    size_t sz_mv = 10;
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(H5Z_FLAG_REVERSE,
                                        kScaleOffsetTotalNparms, cd.data(), 10,
                                        &sz_mv, &short_mv),
              0U);

    // Buffer ends right at 21-byte header with minbits=12 -> initial *j >=
    // buf_size failure inside decompress_one_byte.
    void* trunc_hdr = alloc_buf(kScaleOffsetHeaderBytes);
    ASSERT_NE(trunc_hdr, nullptr);
    absl::Cleanup free_th = [&trunc_hdr] { H5free_memory(trunc_hdr); };
    static_cast<uint8_t*>(trunc_hdr)[0] = 12;
    static_cast<uint8_t*>(trunc_hdr)[4] = 8;
    size_t sz_th = kScaleOffsetHeaderBytes;
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(H5Z_FLAG_REVERSE,
                                        kScaleOffsetTotalNparms, cd.data(),
                                        kScaleOffsetHeaderBytes, &sz_th,
                                        &trunc_hdr),
              0U);

    // Buffer has only 1 payload byte when first element needs 12 bits -> fails
    // on second byte fetch inside decompress_one_byte.
    void* trunc_mid = alloc_buf(kScaleOffsetHeaderBytes + 1);
    ASSERT_NE(trunc_mid, nullptr);
    absl::Cleanup free_tm = [&trunc_mid] { H5free_memory(trunc_mid); };
    static_cast<uint8_t*>(trunc_mid)[0] = 12;
    static_cast<uint8_t*>(trunc_mid)[4] = 8;
    size_t sz_tm = kScaleOffsetHeaderBytes + 1;
    EXPECT_EQ(H5Z_SCALEOFFSET[0].filter(H5Z_FLAG_REVERSE,
                                        kScaleOffsetTotalNparms, cd.data(),
                                        kScaleOffsetHeaderBytes + 1, &sz_tm,
                                        &trunc_mid),
              0U);
  }
}

template <typename T>
void VerifyIntegerRoundTrip(ScopedFile& file, const std::string& name,
                            hid_t file_type, hid_t mem_type,
                            const std::vector<T>& data, bool define_fill,
                            T fill_val, int minbits) {
  const hsize_t dim = data.size();
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid()) << name;

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid()) << name;
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &dim), 0) << name;
  if (define_fill) {
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), mem_type, &fill_val), 0) << name;
  } else {
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), mem_type, nullptr), 0) << name;
  }
  ASSERT_GE(H5Pset_scaleoffset(dcpl.get(), H5Z_SO_INT, minbits), 0) << name;

  ScopedDataset dset(H5Dcreate2(file.get(), name.c_str(), file_type,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid()) << name;
  ASSERT_GE(H5Dwrite(dset.get(), mem_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     data.data()),
            0)
      << name;
  dset.reset();

  dset.reset(H5Dopen2(file.get(), name.c_str(), H5P_DEFAULT));
  ASSERT_TRUE(dset.valid()) << name;
  std::vector<T> actual(data.size());
  ASSERT_GE(H5Dread(dset.get(), mem_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    actual.data()),
            0)
      << name;
  EXPECT_THAT(actual, ElementsAreArray(data)) << name;
}

TEST(H5ZscaleoffsetTest, AllIntegerTypesFillModesExplicitAndAutoMinbits) {
  ScopedFile file = CreateInMemoryFile("scaleoffset_integers.h5");
  ASSERT_TRUE(file.valid());

  // 1. All 8 integer types (u8, u16, u32, u64, i8, i16, i32, i64) with fill
  //    defined, fill undefined, explicit minbits, constant chunks (minbits=0),
  //    all-fill chunks, and full-precision range fallback.
  VerifyIntegerRoundTrip<uint8_t>(file, "u8_def", H5T_NATIVE_UINT8,
                                  H5T_NATIVE_UINT8, {255, 10, 12, 13, 255, 15},
                                  true, 255, 0);
  VerifyIntegerRoundTrip<uint8_t>(file, "u8_undef", H5T_NATIVE_UINT8,
                                  H5T_NATIVE_UINT8, {10, 11, 13, 17, 25}, false,
                                  0, 0);
  VerifyIntegerRoundTrip<uint8_t>(file, "u8_const", H5T_NATIVE_UINT8,
                                  H5T_NATIVE_UINT8, {42, 42, 42, 42}, false, 0,
                                  0);
  VerifyIntegerRoundTrip<uint8_t>(file, "u8_explicit", H5T_NATIVE_UINT8,
                                  H5T_NATIVE_UINT8, {10, 12, 15, 99}, true, 99,
                                  3);
  VerifyIntegerRoundTrip<uint8_t>(file, "u8_explicit_undef", H5T_NATIVE_UINT8,
                                  H5T_NATIVE_UINT8, {10, 12, 15, 17}, false, 0,
                                  4);
  VerifyIntegerRoundTrip<uint8_t>(file, "u8_full_def", H5T_NATIVE_UINT8,
                                  H5T_NATIVE_UINT8, {0, 254, 1}, true, 1, 0);
  VerifyIntegerRoundTrip<uint8_t>(file, "u8_full_undef", H5T_NATIVE_UINT8,
                                  H5T_NATIVE_UINT8, {0, 255, 100}, false, 0, 0);

  VerifyIntegerRoundTrip<uint16_t>(
      file, "u16_def", H5T_NATIVE_UINT16, H5T_NATIVE_UINT16,
      {65000, 100, 105, 110, 65000}, true, 65000, 0);
  VerifyIntegerRoundTrip<uint16_t>(file, "u16_undef", H5T_NATIVE_UINT16,
                                   H5T_NATIVE_UINT16, {1000, 1020, 1050, 1100},
                                   false, 0, 0);
  VerifyIntegerRoundTrip<uint16_t>(file, "u16_full", H5T_NATIVE_UINT16,
                                   H5T_NATIVE_UINT16, {0, 65535, 5}, false, 0,
                                   0);

  VerifyIntegerRoundTrip<uint32_t>(
      file, "u32_def", H5T_NATIVE_UINT32, H5T_NATIVE_UINT32,
      {999999U, 500, 501, 507, 999999U}, true, 999999U, 0);
  VerifyIntegerRoundTrip<uint32_t>(
      file, "u32_undef", H5T_NATIVE_UINT32, H5T_NATIVE_UINT32,
      {100000U, 100001U, 100002U, 100003U}, false, 0, 0);
  VerifyIntegerRoundTrip<uint32_t>(
      file, "u32_full", H5T_NATIVE_UINT32, H5T_NATIVE_UINT32,
      {0U, std::numeric_limits<uint32_t>::max(), 7U}, false, 0, 0);

  VerifyIntegerRoundTrip<uint64_t>(
      file, "u64_def", H5T_NATIVE_UINT64, H5T_NATIVE_UINT64,
      {1234567890123ULL, 10, 12, 15}, true, 1234567890123ULL, 0);
  VerifyIntegerRoundTrip<uint64_t>(
      file, "u64_undef", H5T_NATIVE_UINT64, H5T_NATIVE_UINT64,
      {5000000000ULL, 5000000005ULL, 5000000010ULL}, false, 0, 0);
  VerifyIntegerRoundTrip<uint64_t>(
      file, "u64_full", H5T_NATIVE_UINT64, H5T_NATIVE_UINT64,
      {0ULL, std::numeric_limits<uint64_t>::max(), 1ULL}, false, 0, 0);

  // Signed 8-bit has dedicated non-macro code paths in H5Zscaleoffset.c.
  VerifyIntegerRoundTrip<int8_t>(file, "i8_def", H5T_NATIVE_INT8,
                                 H5T_NATIVE_INT8, {-100, -5, 0, 5, -100}, true,
                                 -100, 0);
  VerifyIntegerRoundTrip<int8_t>(file, "i8_all_fill", H5T_NATIVE_INT8,
                                 H5T_NATIVE_INT8, {-9, -9, -9, -9}, true, -9,
                                 0);
  VerifyIntegerRoundTrip<int8_t>(file, "i8_undef", H5T_NATIVE_INT8,
                                 H5T_NATIVE_INT8, {-10, -8, -5, 0}, false, 0,
                                 0);
  VerifyIntegerRoundTrip<int8_t>(file, "i8_explicit_def", H5T_NATIVE_INT8,
                                 H5T_NATIVE_INT8, {-5, -3, 0, -99}, true, -99,
                                 4);
  VerifyIntegerRoundTrip<int8_t>(file, "i8_explicit_undef", H5T_NATIVE_INT8,
                                 H5T_NATIVE_INT8, {-5, -3, 0, 2}, false, 0, 4);
  VerifyIntegerRoundTrip<int8_t>(file, "i8_full_def", H5T_NATIVE_INT8,
                                 H5T_NATIVE_INT8, {-128, 127, 0}, true, 0, 0);
  VerifyIntegerRoundTrip<int8_t>(file, "i8_full_undef", H5T_NATIVE_INT8,
                                 H5T_NATIVE_INT8, {-128, 127, 10}, false, 0, 0);

  VerifyIntegerRoundTrip<int16_t>(file, "i16_def", H5T_NATIVE_INT16,
                                  H5T_NATIVE_INT16, {-30000, -20, 0, 20, -30000},
                                  true, -30000, 0);
  VerifyIntegerRoundTrip<int16_t>(file, "i16_undef", H5T_NATIVE_INT16,
                                  H5T_NATIVE_INT16, {-500, -490, -480, -470},
                                  false, 0, 0);
  VerifyIntegerRoundTrip<int16_t>(file, "i16_explicit", H5T_NATIVE_INT16,
                                  H5T_NATIVE_INT16, {-10, -5, 0, 5}, true,
                                  -30000, 5);
  VerifyIntegerRoundTrip<int16_t>(file, "i16_explicit_undef", H5T_NATIVE_INT16,
                                  H5T_NATIVE_INT16, {-10, -5, 0, 5}, false, 0,
                                  5);
  VerifyIntegerRoundTrip<int16_t>(file, "i16_full_def", H5T_NATIVE_INT16,
                                  H5T_NATIVE_INT16, {-32768, 32767, 1}, true, 1,
                                  0);
  VerifyIntegerRoundTrip<int16_t>(file, "i16_full_undef", H5T_NATIVE_INT16,
                                  H5T_NATIVE_INT16, {-32768, 32767, 1}, false,
                                  0, 0);

  VerifyIntegerRoundTrip<int32_t>(file, "i32_def", H5T_NATIVE_INT32,
                                  H5T_NATIVE_INT32, {-99999, -50, 0, 50}, true,
                                  -99999, 0);
  VerifyIntegerRoundTrip<int32_t>(file, "i32_undef", H5T_NATIVE_INT32,
                                  H5T_NATIVE_INT32, {-200, -190, -180, -170},
                                  false, 0, 0);

  VerifyIntegerRoundTrip<int64_t>(
      file, "i64_def", H5T_NATIVE_INT64, H5T_NATIVE_INT64,
      {-9876543210LL, -100, 0, 100}, true, -9876543210LL, 0);
  VerifyIntegerRoundTrip<int64_t>(
      file, "i64_undef", H5T_NATIVE_INT64, H5T_NATIVE_INT64,
      {-5000000000LL, -4999999995LL, -4999999990LL}, false, 0, 0);

  // 2. Explicit bit-widths crossing exact byte boundaries (8, 16, 24) and
  //    unaligned multi-byte splits (5, 7, 11, 13, 19, 29).
  const std::vector<uint32_t> pattern12 = {0,  1,  2,  3,  5,  7,
                                           11, 15, 20, 25, 30, 31};
  for (int bits : {5, 7, 8, 11, 13, 16, 19, 24, 29}) {
    VerifyIntegerRoundTrip<uint32_t>(
        file, "u32_bits_" + std::to_string(bits), H5T_NATIVE_UINT32,
        H5T_NATIVE_UINT32, pattern12, false, 0, bits);
  }
}

TEST(H5ZscaleoffsetTest, FloatingPointDScaleAndFullPrecisionFallback) {
  ScopedFile file = CreateInMemoryFile("scaleoffset_floats.h5");
  ASSERT_TRUE(file.valid());

  auto test_f32 = [&](const std::string& name, const std::vector<float>& data,
                      bool define_fill, float fill_val, int d_scale,
                      float tol) {
    const hsize_t dim = data.size();
    ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
    ASSERT_TRUE(space.valid()) << name;
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid()) << name;
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &dim), 0) << name;
    if (define_fill) {
      ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_FLOAT, &fill_val), 0);
    } else {
      ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_FLOAT, nullptr), 0);
    }
    ASSERT_GE(H5Pset_scaleoffset(dcpl.get(), H5Z_SO_FLOAT_DSCALE, d_scale), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), name.c_str(), H5T_NATIVE_FLOAT,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid()) << name;
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data.data()),
              0);
    dset.reset();

    dset.reset(H5Dopen2(file.get(), name.c_str(), H5P_DEFAULT));
    ASSERT_TRUE(dset.valid()) << name;
    std::vector<float> out(data.size(), 0.0f);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    for (size_t i = 0; i < data.size(); ++i) {
      EXPECT_NEAR(out[i], data[i], tol) << name << " idx=" << i;
    }
  };

  auto test_f64 = [&](const std::string& name, const std::vector<double>& data,
                      bool define_fill, double fill_val, int d_scale,
                      double tol) {
    const hsize_t dim = data.size();
    ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
    ASSERT_TRUE(space.valid()) << name;
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid()) << name;
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &dim), 0) << name;
    if (define_fill) {
      ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_DOUBLE, &fill_val), 0);
    } else {
      ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_DOUBLE, nullptr), 0);
    }
    ASSERT_GE(H5Pset_scaleoffset(dcpl.get(), H5Z_SO_FLOAT_DSCALE, d_scale), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), name.c_str(), H5T_NATIVE_DOUBLE,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid()) << name;
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data.data()),
              0);
    dset.reset();

    dset.reset(H5Dopen2(file.get(), name.c_str(), H5P_DEFAULT));
    ASSERT_TRUE(dset.valid()) << name;
    std::vector<double> out(data.size(), 0.0);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    for (size_t i = 0; i < data.size(); ++i) {
      EXPECT_NEAR(out[i], data[i], tol) << name << " idx=" << i;
    }
  };

  test_f32("f32_def", {-999.0f, -1.234f, 0.0f, 2.345f, -999.0f}, true,
           -999.0f, 2, 0.01f);
  test_f32("f32_all_fill", {-50.0f, -50.0f, -50.0f, -50.0f}, true, -50.0f, 1,
           0.01f);
  test_f32("f32_undef", {10.123f, 10.456f, 10.789f, 11.001f}, false, 0.0f, 2,
           0.01f);
  test_f32("f32_const", {3.5f, 3.5f, 3.5f, 3.5f}, false, 0.0f, 2, 1e-6f);
  // Scaled difference exceeds 2^31 -> triggers full-precision fallback.
  test_f32("f32_full_def", {0.0f, 1.0e6f, -999.0f}, true, -999.0f, 4, 1e-3f);
  test_f32("f32_full_undef", {0.0f, 1.0e6f, 500.0f}, false, 0.0f, 4, 1e-3f);

  test_f64("f64_def", {-9999.0, -3.1415, 0.0, 2.7182, -9999.0}, true, -9999.0,
           3, 0.001);
  test_f64("f64_undef", {100.1234, 100.5678, 101.0001}, false, 0.0, 3, 0.001);
  test_f64("f64_const", {-7.25, -7.25, -7.25, -7.25}, false, 0.0, 2, 1e-12);
  // Scaled difference exceeds 2^63 -> triggers full-precision fallback.
  test_f64("f64_full_def", {0.0, 1.0e14, -9999.0}, true, -9999.0, 6, 1e-6);
  test_f64("f64_full_undef", {0.0, 1.0e14, 42.0}, false, 0.0, 6, 1e-6);
}

TEST(H5ZscaleoffsetTest, BigEndianFileTypesExerciseByteOrderConversion) {
  ScopedFile file = CreateInMemoryFile("scaleoffset_be.h5");
  ASSERT_TRUE(file.valid());

  VerifyIntegerRoundTrip<uint16_t>(file, "u16_be_def", H5T_STD_U16BE,
                                   H5T_NATIVE_UINT16, {65000, 100, 105, 110},
                                   true, 65000, 0);
  VerifyIntegerRoundTrip<uint16_t>(file, "u16_be_full", H5T_STD_U16BE,
                                   H5T_NATIVE_UINT16, {0, 65535, 42}, false, 0,
                                   0);
  VerifyIntegerRoundTrip<int32_t>(file, "i32_be_def", H5T_STD_I32BE,
                                  H5T_NATIVE_INT32, {-50000, -10, 0, 10}, true,
                                  -50000, 0);
  VerifyIntegerRoundTrip<uint64_t>(file, "u64_be_def", H5T_STD_U64BE,
                                   H5T_NATIVE_UINT64, {9999999999ULL, 5, 8, 11},
                                   true, 9999999999ULL, 0);

  // Big-endian floating-point round-trip with defined fill value.
  {
    const std::vector<double> data = {-8888.0, 1.25, 1.5, 1.75, -8888.0};
    const double fill_val = -8888.0;
    const hsize_t dim = data.size();
    ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &dim), 0);
    ASSERT_GE(H5Pset_fill_value(dcpl.get(), H5T_NATIVE_DOUBLE, &fill_val), 0);
    ASSERT_GE(H5Pset_scaleoffset(dcpl.get(), H5Z_SO_FLOAT_DSCALE, 2), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "f64_be", H5T_IEEE_F64BE,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data.data()),
              0);
    dset.reset();

    dset.reset(H5Dopen2(file.get(), "f64_be", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<double> out(data.size(), 0.0);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, out.data()),
              0);
    for (size_t i = 0; i < data.size(); ++i) {
      EXPECT_NEAR(out[i], data[i], 0.01);
    }
  }
}

}  // namespace
}  // namespace hdf5
