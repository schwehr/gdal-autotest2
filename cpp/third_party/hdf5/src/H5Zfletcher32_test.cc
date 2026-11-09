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
// Tests the Fletcher32 checksum filter (third_party/hdf5/src/H5Zfletcher32.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Zfletcher32.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with -DNDEBUG; match its layout across internal headers.
#ifndef NDEBUG
#define H5ZFLETCHER32_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5Z_FRIEND
#include "third_party/hdf5/src/H5Zpkg.h"
}  // extern "C"
#ifdef H5ZFLETCHER32_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ZFLETCHER32_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAreArray;

TEST(H5Zfletcher32Test, FilterAvailableAndClassMetadata) {
  EXPECT_GT(H5Zfilter_avail(H5Z_FILTER_FLETCHER32), 0);

  unsigned flags = 0;
  ASSERT_GE(H5Zget_filter_info(H5Z_FILTER_FLETCHER32, &flags), 0);
  EXPECT_EQ(
      flags,
      static_cast<unsigned>(H5Z_FILTER_CONFIG_ENCODE_ENABLED |
                            H5Z_FILTER_CONFIG_DECODE_ENABLED));

  EXPECT_EQ(H5Z_FLETCHER32[0].version, H5Z_CLASS_T_VERS);
  EXPECT_EQ(H5Z_FLETCHER32[0].id, H5Z_FILTER_FLETCHER32);
  EXPECT_EQ(H5Z_FLETCHER32[0].encoder_present, 1U);
  EXPECT_EQ(H5Z_FLETCHER32[0].decoder_present, 1U);
  EXPECT_EQ(std::string_view(H5Z_FLETCHER32[0].name), "fletcher32");
  EXPECT_EQ(H5Z_FLETCHER32[0].can_apply, nullptr);
  EXPECT_EQ(H5Z_FLETCHER32[0].set_local, nullptr);
  ASSERT_NE(H5Z_FLETCHER32[0].filter, nullptr);
}

TEST(H5Zfletcher32Test, ChunkedDatasetDetectsCorruptionAndHonorsEdcDisable) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDim = 16;
  constexpr hsize_t kChunkDim = 16;
  std::array<uint32_t, kDim> expected{};
  for (size_t i = 0; i < kDim; ++i) {
    expected[i] = static_cast<uint32_t>(0x10203000U + i * 17U);
  }

  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kChunkDim), 0);
  ASSERT_GE(H5Pset_fletcher32(dcpl.get()), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "checked", H5T_NATIVE_UINT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_UINT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, expected.data()),
            0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

  // 1. Valid read succeeds with EDC enabled.
  std::array<uint32_t, kDim> actual{};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_UINT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, actual.data()),
            0);
  EXPECT_THAT(actual, ElementsAreArray(expected));

  // 2. Read raw filtered chunk (64 data bytes + 4 checksum bytes) and corrupt
  // a payload byte without updating the trailing checksum.
  constexpr hsize_t kOrigin[1] = {0};
  hsize_t raw_size = 0;
  ASSERT_GE(H5Dget_chunk_storage_size(dset.get(), kOrigin, &raw_size), 0);
  ASSERT_EQ(raw_size, kDim * sizeof(uint32_t) + 4);

  std::vector<uint8_t> raw_chunk(raw_size);
  size_t raw_buf_size = raw_chunk.size();
  uint32_t filter_mask = 0;
  ASSERT_GE(H5Dread_chunk(dset.get(), H5P_DEFAULT, kOrigin, &filter_mask,
                          raw_chunk.data(), &raw_buf_size),
            0);
  raw_chunk[0] ^= 0x01;
  ASSERT_GE(H5Dwrite_chunk(dset.get(), H5P_DEFAULT, filter_mask, kOrigin,
                           raw_chunk.size(), raw_chunk.data()),
            0);

  // 3. Default DXPL detects checksum mismatch and fails.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dread(dset.get(), H5T_NATIVE_UINT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, actual.data()),
              0);
  }

  // 4. Disabling EDC via H5Pset_edc_check skips checksum validation.
  ScopedPlist dxpl_skip(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl_skip.valid());
  ASSERT_GE(H5Pset_edc_check(dxpl_skip.get(), H5Z_DISABLE_EDC), 0);
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_UINT32, H5S_ALL, H5S_ALL,
                    dxpl_skip.get(), actual.data()),
            0);
  expected[0] ^= 0x01;
  EXPECT_THAT(actual, ElementsAreArray(expected));
}

TEST(H5Zfletcher32Test, DirectFilterRoundTripAndLegacyReversedChecksum) {
  for (size_t nbytes : {size_t{1}, size_t{3}, size_t{5}, size_t{17}, size_t{64}}) {
    std::vector<uint8_t> payload(nbytes);
    for (size_t i = 0; i < nbytes; ++i) {
      payload[i] = static_cast<uint8_t>(0x13 + i * 29);
    }

    void* buf = H5allocate_memory(nbytes, false);
    ASSERT_NE(buf, nullptr);
    absl::Cleanup free_buf = [&buf] { H5free_memory(buf); };
    std::memcpy(buf, payload.data(), nbytes);

    size_t buf_size = nbytes;
    const size_t encoded_size =
        H5Z_FLETCHER32[0].filter(0, 0, nullptr, nbytes, &buf_size, &buf);
    ASSERT_EQ(encoded_size, nbytes + 4);
    EXPECT_EQ(buf_size, nbytes + 4);

    // Reverse with exact checksum succeeds.
    size_t decoded_size = H5Z_FLETCHER32[0].filter(
        H5Z_FLAG_REVERSE, 0, nullptr, encoded_size, &buf_size, &buf);
    ASSERT_EQ(decoded_size, nbytes);
    std::vector<uint8_t> decoded(nbytes);
    std::memcpy(decoded.data(), buf, nbytes);
    EXPECT_THAT(decoded, ElementsAreArray(payload));

    // Swap bytes (0<->1, 2<->3) in the trailing checksum to exercise the legacy
    // pre-1.6.3 reversed-byte-order fallback check.
    auto* raw = static_cast<uint8_t*>(buf);
    std::swap(raw[nbytes + 0], raw[nbytes + 1]);
    std::swap(raw[nbytes + 2], raw[nbytes + 3]);
    decoded_size = H5Z_FLETCHER32[0].filter(
        H5Z_FLAG_REVERSE, 0, nullptr, encoded_size, &buf_size, &buf);
    EXPECT_EQ(decoded_size, nbytes);

    // Flipping checksum bits breaks both current and legacy checks.
    raw[nbytes + 0] ^= 0xff;
    raw[nbytes + 1] ^= 0xff;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5Z_FLETCHER32[0].filter(H5Z_FLAG_REVERSE, 0, nullptr,
                                         encoded_size, &buf_size, &buf),
                0U);
    }

    // H5Z_FLAG_SKIP_EDC bypasses validation even with a broken checksum.
    EXPECT_EQ(H5Z_FLETCHER32[0].filter(H5Z_FLAG_REVERSE | H5Z_FLAG_SKIP_EDC, 0,
                                       nullptr, encoded_size, &buf_size, &buf),
              nbytes);
  }
}

}  // namespace
}  // namespace hdf5
