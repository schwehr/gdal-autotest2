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
// Tests the SZIP compression filter (third_party/hdf5/src/H5Zszip.c),
// including registration metadata, can_apply datatype checks, set_local
// bits-per-pixel / scanline / endianness calculations, direct filter error
// handling, and in-memory chunked dataset round-trips.
//
// Google3 build notes: H5_HAVE_FILTER_SZIP=1 via //third_party/libaec, with
// SZ_encoder_enabled() == 1 (both encode and decode enabled) on a little-endian
// LP64 host compiled with -DNDEBUG.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Zszip.c

#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its struct layout across internal
// headers included via hdf5_internal_prelude.h.
#ifndef NDEBUG
#define H5ZSZIP_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5Z_FRIEND
#include "third_party/hdf5/src/H5Zpkg.h"
}  // extern "C"
#ifdef H5ZSZIP_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ZSZIP_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

TEST(H5ZszipTest, FilterIsRegisteredWithEncoderAndDecoderEnabled) {
  ASSERT_GE(H5open(), 0);

  EXPECT_GT(H5Zfilter_avail(H5Z_FILTER_SZIP), 0);

  unsigned int filter_flags = 0;
  ASSERT_GE(H5Zget_filter_info(H5Z_FILTER_SZIP, &filter_flags), 0);
  EXPECT_NE(filter_flags & H5Z_FILTER_CONFIG_ENCODE_ENABLED, 0U);
  EXPECT_NE(filter_flags & H5Z_FILTER_CONFIG_DECODE_ENABLED, 0U);

  EXPECT_EQ(H5Z_SZIP[0].version, H5Z_CLASS_T_VERS);
  EXPECT_EQ(H5Z_SZIP[0].id, H5Z_FILTER_SZIP);
  EXPECT_EQ(H5Z_SZIP[0].encoder_present, 1U);
  EXPECT_EQ(H5Z_SZIP[0].decoder_present, 1U);
  EXPECT_STREQ(H5Z_SZIP[0].name, "szip");
  EXPECT_NE(H5Z_SZIP[0].can_apply, nullptr);
  EXPECT_NE(H5Z_SZIP[0].set_local, nullptr);
  EXPECT_NE(H5Z_SZIP[0].filter, nullptr);
}

TEST(H5ZszipTest, CanApplyValidatesDatatypeSizeAndEndianness) {
  ASSERT_GE(H5open(), 0);

  // 1. Valid 8-, 16-, 32-, and 64-bit LE and BE datatypes succeed.
  const hid_t valid_types[] = {
      H5T_STD_I8LE,   H5T_STD_U16BE,  H5T_STD_I32LE,  H5T_STD_U64BE,
      H5T_IEEE_F32LE, H5T_IEEE_F64BE,
  };
  for (hid_t tid : valid_types) {
    EXPECT_GT(H5Z_SZIP[0].can_apply(H5I_INVALID_HID, tid, H5I_INVALID_HID),
              0);
  }

  // 2. Non-datatype ID returns FAIL (< 0).
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Z_SZIP[0].can_apply(H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID),
        0);
    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    EXPECT_LT(
        H5Z_SZIP[0].can_apply(H5I_INVALID_HID, space.get(), H5I_INVALID_HID),
        0);
  }

  // 3. Disallowed sizes (> 32 bits and != 64 bits) return false (0).
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5Z_SZIP[0].can_apply(H5I_INVALID_HID, H5T_NATIVE_LDOUBLE, H5I_INVALID_HID),
        0);

    ScopedType five_byte_str(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(five_byte_str.valid());
    ASSERT_GE(H5Tset_size(five_byte_str.get(), 5), 0);
    EXPECT_EQ(H5Z_SZIP[0].can_apply(H5I_INVALID_HID, five_byte_str.get(),
                                    H5I_INVALID_HID),
              0);
  }

  // 4. Non-LE/BE byte orders (NONE, VAX, MIXED) return false (0).
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5Z_SZIP[0].can_apply(H5I_INVALID_HID, H5T_C_S1, H5I_INVALID_HID), 0);

    ScopedType vax_int(H5Tcopy(H5T_STD_I32LE));
    ASSERT_TRUE(vax_int.valid());
    ASSERT_GE(H5Tset_order(vax_int.get(), H5T_ORDER_VAX), 0);
    EXPECT_EQ(
        H5Z_SZIP[0].can_apply(H5I_INVALID_HID, vax_int.get(), H5I_INVALID_HID),
        0);

    ScopedType mixed_cmpd(H5Tcreate(H5T_COMPOUND, 4));
    ASSERT_TRUE(mixed_cmpd.valid());
    ASSERT_GE(H5Tinsert(mixed_cmpd.get(), "a", 0, H5T_STD_I16LE), 0);
    ASSERT_GE(H5Tinsert(mixed_cmpd.get(), "b", 2, H5T_STD_I16BE), 0);
    EXPECT_EQ(H5Z_SZIP[0].can_apply(H5I_INVALID_HID, mixed_cmpd.get(),
                                    H5I_INVALID_HID),
              0);
  }
}

TEST(H5ZszipTest, SetLocalValidatesInputsAndComputesLocalParameters) {
  ASSERT_GE(H5open(), 0);

  constexpr hsize_t kDim32 = 32;
  ScopedSpace space32(H5Screate_simple(1, &kDim32, nullptr));
  ASSERT_TRUE(space32.valid());

  // 1. Invalid DCPL, missing filter, bad type, non-atomic type, bad space,
  //    invalid endianness, and chunk smaller than pixels_per_block fail.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Z_SZIP[0].set_local(H5I_INVALID_HID, H5T_NATIVE_INT32, space32.get()),
        0);

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    EXPECT_LT(
        H5Z_SZIP[0].set_local(fapl.get(), H5T_NATIVE_INT32, space32.get()), 0);

    ScopedPlist dcpl_empty(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_empty.valid());
    EXPECT_LT(H5Z_SZIP[0].set_local(dcpl_empty.get(), H5T_NATIVE_INT32,
                                    space32.get()),
              0);

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_szip(dcpl.get(), H5_SZIP_EC_OPTION_MASK, 8), 0);

    EXPECT_LT(
        H5Z_SZIP[0].set_local(dcpl.get(), H5I_INVALID_HID, space32.get()), 0);
    EXPECT_LT(H5Z_SZIP[0].set_local(dcpl.get(), H5T_C_S1, space32.get()), 0);
    EXPECT_LT(
        H5Z_SZIP[0].set_local(dcpl.get(), H5T_NATIVE_INT32, H5I_INVALID_HID),
        0);

    ScopedType vax_int(H5Tcopy(H5T_STD_I32LE));
    ASSERT_TRUE(vax_int.valid());
    ASSERT_GE(H5Tset_order(vax_int.get(), H5T_ORDER_VAX), 0);
    EXPECT_LT(H5Z_SZIP[0].set_local(dcpl.get(), vax_int.get(), space32.get()),
              0);

    constexpr hsize_t kDim4 = 4;
    ScopedSpace space4(H5Screate_simple(1, &kDim4, nullptr));
    ASSERT_TRUE(space4.valid());
    EXPECT_LT(
        H5Z_SZIP[0].set_local(dcpl.get(), H5T_NATIVE_INT32, space4.get()), 0);
  }

  // 2. Verify BPP, PPS, and endianness mask calculations across edge cases.
  auto check_local = [](hid_t type_id, const std::vector<hsize_t>& chunk_dims,
                        unsigned int opts, unsigned int ppb,
                        unsigned int expected_bpp, unsigned int expected_pps,
                        unsigned int expected_endian_mask) {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_szip(dcpl.get(), opts, ppb), 0);

    ScopedSpace space(H5Screate_simple(static_cast<int>(chunk_dims.size()),
                                       chunk_dims.data(), nullptr));
    ASSERT_TRUE(space.valid());

    ASSERT_GE(H5Z_SZIP[0].set_local(dcpl.get(), type_id, space.get()), 0);

    unsigned int flags = 0;
    size_t cd_nelmts = H5Z_SZIP_TOTAL_NPARMS;
    unsigned int cd_values[H5Z_SZIP_TOTAL_NPARMS] = {};
    unsigned int filter_config = 0;
    ASSERT_GE(H5Pget_filter_by_id2(dcpl.get(), H5Z_FILTER_SZIP, &flags,
                                   &cd_nelmts, cd_values, 0, nullptr,
                                   &filter_config),
              0);
    ASSERT_EQ(cd_nelmts, static_cast<size_t>(H5Z_SZIP_TOTAL_NPARMS));
    EXPECT_EQ(cd_values[H5Z_SZIP_PARM_BPP], expected_bpp);
    EXPECT_EQ(cd_values[H5Z_SZIP_PARM_PPS], expected_pps);
    EXPECT_NE(cd_values[H5Z_SZIP_PARM_MASK] & expected_endian_mask, 0U);
  };

  // Sub-byte precision with offset=0 preserves precision; offset!=0 uses full
  // datatype bit width; >24 rounds up to 32 or 64.
  ScopedType u16_prec12(H5Tcopy(H5T_STD_U16LE));
  ASSERT_TRUE(u16_prec12.valid());
  ASSERT_GE(H5Tset_precision(u16_prec12.get(), 12), 0);
  check_local(u16_prec12.get(), {32}, H5_SZIP_EC_OPTION_MASK, 8, 12, 32,
              H5_SZIP_LSB_OPTION_MASK);

  ScopedType u16_prec12_off2(H5Tcopy(H5T_STD_U16LE));
  ASSERT_TRUE(u16_prec12_off2.valid());
  ASSERT_GE(H5Tset_precision(u16_prec12_off2.get(), 12), 0);
  ASSERT_GE(H5Tset_offset(u16_prec12_off2.get(), 2), 0);
  check_local(u16_prec12_off2.get(), {32}, H5_SZIP_EC_OPTION_MASK, 8, 16, 32,
              H5_SZIP_LSB_OPTION_MASK);

  ScopedType u32_prec28(H5Tcopy(H5T_STD_U32BE));
  ASSERT_TRUE(u32_prec28.valid());
  ASSERT_GE(H5Tset_precision(u32_prec28.get(), 28), 0);
  check_local(u32_prec28.get(), {32}, H5_SZIP_NN_OPTION_MASK, 8, 32, 32,
              H5_SZIP_MSB_OPTION_MASK);

  ScopedType u64_prec40(H5Tcopy(H5T_STD_U64LE));
  ASSERT_TRUE(u64_prec40.valid());
  ASSERT_GE(H5Tset_precision(u64_prec40.get(), 40), 0);
  check_local(u64_prec40.get(), {32}, H5_SZIP_EC_OPTION_MASK, 8, 64, 32,
              H5_SZIP_LSB_OPTION_MASK);

  // Scanline < ppb uses total npoints capped at ppb * 128.
  check_local(H5T_STD_I32LE, {4, 2}, H5_SZIP_EC_OPTION_MASK, 4, 32, 8,
              H5_SZIP_LSB_OPTION_MASK);
  // Scanline > SZ_MAX_PIXELS_PER_SCANLINE (4096) caps at ppb * 128.
  check_local(H5T_STD_I32LE, {5000}, H5_SZIP_EC_OPTION_MASK, 8, 32, 1024,
              H5_SZIP_LSB_OPTION_MASK);
}

TEST(H5ZszipTest, DirectFilterRoundTripAndErrorHandling) {
  ASSERT_GE(H5open(), 0);

  constexpr size_t kNumElems = 64;
  constexpr size_t kBytes = kNumElems * sizeof(uint16_t);
  // Parameter order: [MASK, PPB, BPP, PPS].
  const unsigned int valid_cd[4] = {
      H5_SZIP_NN_OPTION_MASK | H5_SZIP_ALLOW_K13_OPTION_MASK |
          H5_SZIP_RAW_OPTION_MASK | H5_SZIP_LSB_OPTION_MASK,
      8,   // pixels per block
      16,  // bits per pixel
      64,  // pixels per scanline
  };

  // 1. Wrong cd_nelmts returns 0 and leaves buffer intact.
  {
    ScopedErrorSilence silence;
    void* buf = H5allocate_memory(kBytes, true);
    ASSERT_NE(buf, nullptr);
    absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
    size_t buf_size = kBytes;
    EXPECT_EQ(H5Z_SZIP[0].filter(0, 3, valid_cd, kBytes, &buf_size, &buf), 0U);
  }

  // 2. Invalid szip cd_values (odd pixels_per_block) fail compression.
  {
    ScopedErrorSilence silence;
    const unsigned int bad_cd[4] = {valid_cd[0], 3 /* odd ppb */, 16, 64};
    void* buf = H5allocate_memory(kBytes, true);
    ASSERT_NE(buf, nullptr);
    absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
    size_t buf_size = kBytes;
    EXPECT_EQ(H5Z_SZIP[0].filter(0, 4, bad_cd, kBytes, &buf_size, &buf), 0U);
  }

  // 3. Compress predictable data, round-trip decompress, and test decompress
  //    failures (bad params and truncated compressed stream).
  std::vector<uint16_t> orig(kNumElems);
  for (size_t i = 0; i < kNumElems; ++i) {
    orig[i] = static_cast<uint16_t>(100 + i);
  }
  void* buf = H5allocate_memory(kBytes, false);
  ASSERT_NE(buf, nullptr);
  absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
  std::memcpy(buf, orig.data(), kBytes);

  size_t buf_size = kBytes;
  const size_t comp_nbytes =
      H5Z_SZIP[0].filter(0, 4, valid_cd, kBytes, &buf_size, &buf);
  ASSERT_GT(comp_nbytes, 4U);
  ASSERT_LE(comp_nbytes, kBytes + 4);

  // Keep copies of the compressed payload to exercise decompression errors.
  void* copy_for_bad_cd = H5allocate_memory(comp_nbytes, false);
  ASSERT_NE(copy_for_bad_cd, nullptr);
  absl::Cleanup free_bad_cd = [&copy_for_bad_cd] {
    H5free_memory(copy_for_bad_cd);
  };
  std::memcpy(copy_for_bad_cd, buf, comp_nbytes);

  void* copy_for_corrupt = H5allocate_memory(comp_nbytes, false);
  ASSERT_NE(copy_for_corrupt, nullptr);
  absl::Cleanup free_corrupt = [&copy_for_corrupt] {
    H5free_memory(copy_for_corrupt);
  };
  std::memcpy(copy_for_corrupt, buf, comp_nbytes);

  const size_t decomp_nbytes = H5Z_SZIP[0].filter(
      H5Z_FLAG_REVERSE, 4, valid_cd, comp_nbytes, &buf_size, &buf);
  ASSERT_EQ(decomp_nbytes, kBytes);
  EXPECT_EQ(std::memcmp(buf, orig.data(), kBytes), 0);

  // Invalid parameters and truncated payload trigger decompression failures.
  {
    ScopedErrorSilence silence;
    const unsigned int bad_cd[4] = {valid_cd[0], 3 /* odd ppb */, 16, 64};
    size_t bad_cd_size = comp_nbytes;
    EXPECT_EQ(H5Z_SZIP[0].filter(H5Z_FLAG_REVERSE, 4, bad_cd, comp_nbytes,
                                 &bad_cd_size, &copy_for_bad_cd),
              0U);

    // Truncating compressed bytes to 1 byte after the 4-byte length header
    // causes SZ_BufftoBuffDecompress to fail.
    size_t corrupt_size = 5;
    EXPECT_EQ(H5Z_SZIP[0].filter(H5Z_FLAG_REVERSE, 4, valid_cd, 5,
                                 &corrupt_size, &copy_for_corrupt),
              0U);
  }
}

TEST(H5ZszipTest, ChunkedDatasetRoundTripsAcrossTypesModesAndPadding) {
  ScopedFile file = CreateInMemoryFile("szip_roundtrip.h5");
  ASSERT_TRUE(file.valid());

  auto round_trip_i32 = [&](const char* name, const std::vector<hsize_t>& dims,
                            const std::vector<hsize_t>& chunk,
                            unsigned int opts, unsigned int ppb,
                            hid_t file_type) {
    ScopedSpace space(H5Screate_simple(static_cast<int>(dims.size()),
                                       dims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(
        H5Pset_chunk(dcpl.get(), static_cast<int>(chunk.size()), chunk.data()),
        0);
    ASSERT_GE(H5Pset_szip(dcpl.get(), opts, ppb), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), name, file_type, space.get(),
                                  H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    size_t n = 1;
    for (hsize_t d : dims) n *= static_cast<size_t>(d);
    std::vector<int32_t> write_data(n);
    for (size_t i = 0; i < n; ++i) {
      write_data[i] = static_cast<int32_t>(i % 25);
    }
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, write_data.data()),
              0);
    dset.reset();

    dset.reset(H5Dopen2(file.get(), name, H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<int32_t> read_data(n, -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_data.data()),
              0);
    EXPECT_THAT(read_data, ElementsAreArray(write_data)) << name;
  };

  // NN and EC modes, LE and BE storage types, non-multiple scanline padding,
  // and >4096 fastest dimension.
  round_trip_i32("ec_le", {10, 6}, {5, 6}, H5_SZIP_EC_OPTION_MASK, 4,
                 H5T_STD_I32LE);
  round_trip_i32("nn_be", {12, 8}, {4, 2}, H5_SZIP_NN_OPTION_MASK, 4,
                 H5T_STD_I32BE);
  round_trip_i32("large_scanline", {5000}, {5000}, H5_SZIP_NN_OPTION_MASK, 8,
                 H5T_STD_I32LE);

  // 64-bit floating-point round-trip and incompressible optional-filter bypass.
  {
    constexpr hsize_t kDim = 64;
    ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kDim), 0);
    ASSERT_GE(H5Pset_szip(dcpl.get(), H5_SZIP_EC_OPTION_MASK, 8), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "f64", H5T_IEEE_F64LE,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<double> f64_in(kDim);
    for (size_t i = 0; i < kDim; ++i) {
      f64_in[i] = static_cast<double>(i % 4) * 0.5;
    }
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, f64_in.data()),
              0);
    dset.reset();

    dset.reset(H5Dopen2(file.get(), "f64", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<double> f64_out(kDim, -1.0);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, f64_out.data()),
              0);
    EXPECT_THAT(f64_out, ElementsAreArray(f64_in));
  }

  // High-entropy uint8 chunk exercises SZ_OUTBUFF_FULL -> filter returns 0,
  // which H5Z_pipeline handles gracefully since H5Pset_szip marks it optional.
  {
    constexpr hsize_t kDim = 32;
    ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kDim), 0);
    ASSERT_GE(H5Pset_szip(dcpl.get(), H5_SZIP_EC_OPTION_MASK, 8), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "incompressible_u8",
                                  H5T_STD_U8LE, space.get(), H5P_DEFAULT,
                                  dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::vector<uint8_t> u8_in(kDim);
    uint32_t rng = 0x12345678U;
    for (size_t i = 0; i < kDim; ++i) {
      rng = rng * 1664525U + 1013904223U;
      u8_in[i] = static_cast<uint8_t>((rng >> 24) ^ (rng >> 8));
    }
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, u8_in.data()),
              0);
    dset.reset();

    dset.reset(H5Dopen2(file.get(), "incompressible_u8", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::vector<uint8_t> u8_out(kDim, 0);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, u8_out.data()),
              0);
    EXPECT_THAT(u8_out, ElementsAreArray(u8_in));
  }
}

}  // namespace
}  // namespace hdf5
