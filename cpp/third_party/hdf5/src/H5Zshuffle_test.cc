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
// Tests the byte-shuffling data filter (third_party/hdf5/src/H5Zshuffle.c),
// including registration metadata, set_local parameter validation, Duff's
// device byte-plane shuffling/unshuffling across all modulo-8 remainders,
// fractional leftover byte handling, pass-through edge cases, and chunked
// dataset round-trips in memory.
//
// Google3 build notes: `:hdf5` is built with -DNDEBUG on a little-endian LP64
// host and uses Duff's device (NO_DUFFS_DEVICE is undefined).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Zshuffle.c

#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its struct layout across internal
// headers included via hdf5_internal_prelude.h.
#ifndef NDEBUG
#define H5ZSHUFFLE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5Z_FRIEND
#include "third_party/hdf5/src/H5Zpkg.h"
}  // extern "C"
#ifdef H5ZSHUFFLE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ZSHUFFLE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

TEST(H5ZshuffleTest, FilterIsRegisteredWithExpectedMetadata) {
  ASSERT_GE(H5open(), 0);

  EXPECT_GT(H5Zfilter_avail(H5Z_FILTER_SHUFFLE), 0);

  unsigned int filter_flags = 0;
  ASSERT_GE(H5Zget_filter_info(H5Z_FILTER_SHUFFLE, &filter_flags), 0);
  EXPECT_NE(filter_flags & H5Z_FILTER_CONFIG_ENCODE_ENABLED, 0U);
  EXPECT_NE(filter_flags & H5Z_FILTER_CONFIG_DECODE_ENABLED, 0U);

  EXPECT_EQ(H5Z_SHUFFLE[0].version, H5Z_CLASS_T_VERS);
  EXPECT_EQ(H5Z_SHUFFLE[0].id, H5Z_FILTER_SHUFFLE);
  EXPECT_EQ(H5Z_SHUFFLE[0].encoder_present, 1U);
  EXPECT_EQ(H5Z_SHUFFLE[0].decoder_present, 1U);
  EXPECT_STREQ(H5Z_SHUFFLE[0].name, "shuffle");
  EXPECT_EQ(H5Z_SHUFFLE[0].can_apply, nullptr);
  EXPECT_NE(H5Z_SHUFFLE[0].set_local, nullptr);
  EXPECT_NE(H5Z_SHUFFLE[0].filter, nullptr);
}

TEST(H5ZshuffleTest, SetLocalValidatesInputsAndStoresDatatypeSize) {
  ASSERT_GE(H5open(), 0);

  // 1. Reject invalid or wrong-class property list IDs.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Z_SHUFFLE[0].set_local(H5I_INVALID_HID, H5T_NATIVE_INT32,
                                       H5I_INVALID_HID),
              0);
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    EXPECT_LT(
        H5Z_SHUFFLE[0].set_local(fapl.get(), H5T_NATIVE_INT32, H5I_INVALID_HID),
        0);
  }

  // 2. Reject a DCPL that does not have the shuffle filter configured.
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Z_SHUFFLE[0].set_local(dcpl.get(), H5T_NATIVE_INT32, H5I_INVALID_HID),
        0);
  }

  // 3. With shuffle added, reject non-datatype IDs and accept valid types.
  ASSERT_GE(H5Pset_shuffle(dcpl.get()), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Z_SHUFFLE[0].set_local(dcpl.get(), H5I_INVALID_HID, H5I_INVALID_HID),
        0);
    EXPECT_LT(
        H5Z_SHUFFLE[0].set_local(dcpl.get(), dcpl.get(), H5I_INVALID_HID), 0);
  }

  ASSERT_GE(
      H5Z_SHUFFLE[0].set_local(dcpl.get(), H5T_NATIVE_INT32, H5I_INVALID_HID),
      0);

  unsigned int flags = 0;
  size_t cd_nelmts = 1;
  unsigned int cd_values[1] = {0};
  unsigned int filter_config = 0;
  ASSERT_GE(H5Pget_filter_by_id2(dcpl.get(), H5Z_FILTER_SHUFFLE, &flags,
                                 &cd_nelmts, cd_values, 0, nullptr,
                                 &filter_config),
            0);
  EXPECT_EQ(cd_nelmts, 1U);
  EXPECT_EQ(cd_values[0], sizeof(int32_t));
}

TEST(H5ZshuffleTest, DirectFilterRejectsInvalidParameters) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8};
  void* buf = H5allocate_memory(data.size(), false);
  ASSERT_NE(buf, nullptr);
  absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
  std::memcpy(buf, data.data(), data.size());

  size_t buf_size = data.size();
  const unsigned int valid_cd[1] = {4};
  const unsigned int zero_cd[1] = {0};
  const unsigned int two_cd[2] = {4, 1};

  // cd_nelmts != H5Z_SHUFFLE_TOTAL_NPARMS (1) fails without modifying buffer.
  EXPECT_EQ(H5Z_SHUFFLE[0].filter(0, 0, valid_cd, data.size(), &buf_size, &buf),
            0U);
  EXPECT_EQ(H5Z_SHUFFLE[0].filter(H5Z_FLAG_REVERSE, 2, two_cd, data.size(),
                                    &buf_size, &buf),
            0U);
  // Element byte size of 0 is invalid.
  EXPECT_EQ(H5Z_SHUFFLE[0].filter(0, 1, zero_cd, data.size(), &buf_size, &buf),
            0U);
  EXPECT_EQ(H5Z_SHUFFLE[0].filter(H5Z_FLAG_REVERSE, 1, zero_cd, data.size(),
                                    &buf_size, &buf),
            0U);
}

TEST(H5ZshuffleTest, DirectFilterPassesThroughSingleByteOrSubTwoElements) {
  ASSERT_GE(H5open(), 0);

  struct Case {
    unsigned int bytes_per_elem;
    size_t nbytes;
  };
  constexpr Case kCases[] = {
      {.bytes_per_elem = 1, .nbytes = 12},
      {.bytes_per_elem = 4, .nbytes = 3},  // numofelements == 0
      {.bytes_per_elem = 4, .nbytes = 4},  // numofelements == 1
      {.bytes_per_elem = 4, .nbytes = 7},  // numofelements == 1 with fractional
  };

  for (const Case& tc : kCases) {
    std::vector<uint8_t> input(tc.nbytes);
    std::iota(input.begin(), input.end(), static_cast<uint8_t>(0x10));

    void* buf = H5allocate_memory(tc.nbytes, false);
    ASSERT_NE(buf, nullptr);
    absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
    std::memcpy(buf, input.data(), tc.nbytes);

    void* orig_ptr = buf;
    size_t buf_size = tc.nbytes;
    const unsigned int cd[1] = {tc.bytes_per_elem};

    // Forward pass leaves buffer in place and unmodified.
    EXPECT_EQ(H5Z_SHUFFLE[0].filter(0, 1, cd, tc.nbytes, &buf_size, &buf),
              tc.nbytes);
    EXPECT_EQ(buf, orig_ptr);
    EXPECT_EQ(std::memcmp(buf, input.data(), tc.nbytes), 0);

    // Reverse pass leaves buffer in place and unmodified.
    EXPECT_EQ(H5Z_SHUFFLE[0].filter(H5Z_FLAG_REVERSE, 1, cd, tc.nbytes,
                                    &buf_size, &buf),
              tc.nbytes);
    EXPECT_EQ(buf, orig_ptr);
    EXPECT_EQ(std::memcmp(buf, input.data(), tc.nbytes), 0);
  }
}

TEST(H5ZshuffleTest, DirectFilterExactByteLayoutAndDuffsDeviceRemainders) {
  ASSERT_GE(H5open(), 0);

  // 1. Verify exact byte-plane transposition for three 4-byte elements.
  {
    const std::vector<uint8_t> input = {
        0x00, 0x01, 0x02, 0x03,  // elem 0
        0x10, 0x11, 0x12, 0x13,  // elem 1
        0x20, 0x21, 0x22, 0x23,  // elem 2
    };
    const std::vector<uint8_t> expected_shuffled = {
        0x00, 0x10, 0x20,  // byte 0 of each element
        0x01, 0x11, 0x21,  // byte 1
        0x02, 0x12, 0x22,  // byte 2
        0x03, 0x13, 0x23,  // byte 3
    };

    void* buf = H5allocate_memory(input.size(), false);
    ASSERT_NE(buf, nullptr);
    absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
    std::memcpy(buf, input.data(), input.size());

    size_t buf_size = input.size();
    const unsigned int cd[1] = {4};
    ASSERT_EQ(H5Z_SHUFFLE[0].filter(0, 1, cd, input.size(), &buf_size, &buf),
              input.size());
    EXPECT_EQ(buf_size, input.size());

    std::vector<uint8_t> actual(static_cast<const uint8_t*>(buf),
                                static_cast<const uint8_t*>(buf) + buf_size);
    EXPECT_THAT(actual, ElementsAreArray(expected_shuffled));

    ASSERT_EQ(H5Z_SHUFFLE[0].filter(H5Z_FLAG_REVERSE, 1, cd, buf_size,
                                    &buf_size, &buf),
              input.size());
    actual.assign(static_cast<const uint8_t*>(buf),
                  static_cast<const uint8_t*>(buf) + buf_size);
    EXPECT_THAT(actual, ElementsAreArray(input));
  }

  // 2. Sweep element counts 2..17 (covering every numofelements % 8 case 0..7
  //    and multiple Duff's loop iterations) and element sizes 2, 3, 4, 8, with
  //    both zero and non-zero trailing leftover bytes.
  for (unsigned int elem_size : {2U, 3U, 4U, 8U}) {
    for (size_t nelem = 2; nelem <= 17; ++nelem) {
      for (size_t leftover : {size_t{0}, size_t{1}, size_t{elem_size - 1}}) {
        const size_t nbytes = nelem * elem_size + leftover;
        std::vector<uint8_t> input(nbytes);
        for (size_t i = 0; i < nbytes; ++i) {
          input[i] = static_cast<uint8_t>((i * 37 + elem_size) & 0xff);
        }

        void* buf = H5allocate_memory(nbytes, false);
        ASSERT_NE(buf, nullptr);
        absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
        std::memcpy(buf, input.data(), nbytes);

        size_t buf_size = nbytes;
        const unsigned int cd[1] = {elem_size};

        ASSERT_EQ(H5Z_SHUFFLE[0].filter(0, 1, cd, nbytes, &buf_size, &buf),
                  nbytes)
            << "elem_size=" << elem_size << " nelem=" << nelem
            << " leftover=" << leftover;
        EXPECT_EQ(buf_size, nbytes);
        if (leftover > 0) {
          EXPECT_EQ(std::memcmp(static_cast<const uint8_t*>(buf) +
                                    nelem * elem_size,
                                input.data() + nelem * elem_size, leftover),
                    0);
        }

        ASSERT_EQ(H5Z_SHUFFLE[0].filter(H5Z_FLAG_REVERSE, 1, cd, nbytes,
                                        &buf_size, &buf),
                  nbytes)
            << "reverse elem_size=" << elem_size << " nelem=" << nelem
            << " leftover=" << leftover;
        EXPECT_EQ(buf_size, nbytes);
        EXPECT_EQ(std::memcmp(buf, input.data(), nbytes), 0);
      }
    }
  }
}

TEST(H5ZshuffleTest, ChunkedDatasetRoundTripSingleAndCombinedWithDeflate) {
  ScopedFile file = CreateInMemoryFile("shuffle_roundtrip.h5");
  ASSERT_TRUE(file.valid());

  // 1. 2D int32 dataset with shuffle + deflate pipeline, flushed by closing and
  //    reopening the dataset so both forward and reverse filter passes execute.
  {
    constexpr hsize_t kDims[2] = {16, 12};
    constexpr hsize_t kChunk[2] = {8, 6};
    ScopedSpace space(H5Screate_simple(2, kDims, nullptr));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, kChunk), 0);
    ASSERT_GE(H5Pset_shuffle(dcpl.get()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl.get(), 6), 0);

    ScopedDataset dset(H5Dcreate2(file.get(), "shuffled_i32", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    std::vector<int32_t> write_data(kDims[0] * kDims[1]);
    for (size_t i = 0; i < write_data.size(); ++i) {
      write_data[i] = static_cast<int32_t>(i * 17 - 100);
    }
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, write_data.data()),
              0);
    dset.reset();

    dset.reset(H5Dopen2(file.get(), "shuffled_i32", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    EXPECT_LT(H5Dget_storage_size(dset.get()),
              write_data.size() * sizeof(int32_t));

    std::vector<int32_t> read_data(write_data.size(), -1);
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, read_data.data()),
              0);
    EXPECT_THAT(read_data, ElementsAreArray(write_data));
  }

  // 2. 1-byte and 1-element-chunk datasets exercise the pass-through branch.
  {
    constexpr hsize_t kDims[1] = {5};
    constexpr hsize_t kChunkOne[1] = {1};
    ScopedSpace space(H5Screate_simple(1, kDims, nullptr));
    ASSERT_TRUE(space.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunkOne), 0);
    ASSERT_GE(H5Pset_shuffle(dcpl.get()), 0);

    ScopedDataset dset_u8(H5Dcreate2(file.get(), "shuffled_u8", H5T_NATIVE_UINT8,
                                     space.get(), H5P_DEFAULT, dcpl.get(),
                                     H5P_DEFAULT));
    ASSERT_TRUE(dset_u8.valid());
    const std::vector<uint8_t> u8_in = {10, 20, 30, 40, 50};
    ASSERT_GE(H5Dwrite(dset_u8.get(), H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, u8_in.data()),
              0);
    dset_u8.reset();

    dset_u8.reset(H5Dopen2(file.get(), "shuffled_u8", H5P_DEFAULT));
    ASSERT_TRUE(dset_u8.valid());
    std::vector<uint8_t> u8_out(5, 0);
    ASSERT_GE(H5Dread(dset_u8.get(), H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, u8_out.data()),
              0);
    EXPECT_THAT(u8_out, ElementsAreArray(u8_in));

    ScopedDataset dset_f64(H5Dcreate2(file.get(), "shuffled_f64_1elem",
                                      H5T_NATIVE_DOUBLE, space.get(),
                                      H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(dset_f64.valid());
    const std::vector<double> f64_in = {1.25, -2.5, 3.75, 4.125, -5.5};
    ASSERT_GE(H5Dwrite(dset_f64.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, f64_in.data()),
              0);
    dset_f64.reset();

    dset_f64.reset(H5Dopen2(file.get(), "shuffled_f64_1elem", H5P_DEFAULT));
    ASSERT_TRUE(dset_f64.valid());
    std::vector<double> f64_out(5, 0.0);
    ASSERT_GE(H5Dread(dset_f64.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, f64_out.data()),
              0);
    EXPECT_THAT(f64_out, ElementsAreArray(f64_in));
  }
}

}  // namespace
}  // namespace hdf5
