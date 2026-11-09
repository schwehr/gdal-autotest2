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
// Tests internal checksum and string-hashing routines: H5_checksum_fletcher32,
// H5_checksum_crc, H5_checksum_lookup3, H5_checksum_metadata, and
// H5_hash_string (third_party/hdf5/src/H5checksum.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5checksum.c

#ifndef NDEBUG
#define H5CHECKSUM_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#ifdef H5CHECKSUM_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5CHECKSUM_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

struct ChecksumVector {
  const char* input;
  size_t length;
  uint32_t expected_fletcher32;
  uint32_t expected_crc;
  uint32_t expected_djb2;
};

constexpr ChecksumVector kSmallVectors[] = {
    {.input = "a",
     .length = 1,
     .expected_fletcher32 = 0x61006100U,
     .expected_crc = 0xf9b2971aU,
     .expected_djb2 = 0x0002b606U},
    {.input = "ab",
     .length = 2,
     .expected_fletcher32 = 0x61626162U,
     .expected_crc = 0xfad5581bU,
     .expected_djb2 = 0x00597728U},
    {.input = "abcde",
     .length = 5,
     .expected_fletcher32 = 0x4ff029c7U,
     .expected_crc = 0xf8b945f1U,
     .expected_djb2 = 0x0f11b894U},
    {.input = "123456789",
     .length = 9,
     .expected_fletcher32 = 0x09df09d5U,
     .expected_crc = 0xfc4f2be9U,
     .expected_djb2 = 0x35cdbb82U},
};

TEST(H5ChecksumTest, Fletcher32SmallOddEvenAndMultiBlockReduction) {
  for (const auto& vec : kSmallVectors) {
    EXPECT_EQ(H5_checksum_fletcher32(vec.input, vec.length),
              vec.expected_fletcher32)
        << "input=" << vec.input;
  }

  // Zero-filled buffer of 800 bytes (> 720-byte tlen=360 chunk threshold).
  const std::vector<uint8_t> zeros(800, 0x00);
  EXPECT_EQ(H5_checksum_fletcher32(zeros.data(), zeros.size()), 0x00000000U);

  // 722 bytes (361 words) crosses the 360-word inner loop reduction boundary;
  // 723 bytes also exercises the trailing odd-byte path after reduction.
  std::vector<uint8_t> ones_even(722, 0xff);
  EXPECT_EQ(H5_checksum_fletcher32(ones_even.data(), ones_even.size()),
            0xffffffffU);

  std::vector<uint8_t> ones_odd(723, 0xff);
  EXPECT_EQ(H5_checksum_fletcher32(ones_odd.data(), ones_odd.size()),
            0xff00ff00U);

  // Non-uniform multi-block buffer: verify bit sensitivity before and after the
  // 720-byte reduction boundary as well as in the trailing odd byte.
  std::vector<uint8_t> pattern(1501);
  for (size_t i = 0; i < pattern.size(); ++i) {
    pattern[i] = static_cast<uint8_t>((i * 37 + 13) & 0xff);
  }
  const uint32_t base_sum =
      H5_checksum_fletcher32(pattern.data(), pattern.size());
  for (const size_t flip_idx : {size_t{0}, size_t{720}, size_t{1500}}) {
    pattern[flip_idx] ^= 0x01;
    EXPECT_NE(H5_checksum_fletcher32(pattern.data(), pattern.size()), base_sum)
        << "flip_idx=" << flip_idx;
    pattern[flip_idx] ^= 0x01;
  }
}

TEST(H5ChecksumTest, CrcTableLazyInitAndKnownVectors) {
  // HDF5's H5__checksum_crc_make_table uses H5_CRC_QUOTIENT (0x04C11DB7) in a
  // right-shift bit loop rather than reflected Ethernet 0xEDB88320; verify
  // both initial table generation and cached fast-path calls.
  for (const auto& vec : kSmallVectors) {
    EXPECT_EQ(H5_checksum_crc(vec.input, vec.length), vec.expected_crc)
        << "input=" << vec.input;
  }

  // Repeat call to cover the !H5_crc_table_computed == false fast path.
  const std::vector<uint8_t> zeros(800, 0x00);
  EXPECT_EQ(H5_checksum_crc(zeros.data(), zeros.size()), 0xf9e9578eU);
  EXPECT_EQ(H5_checksum_crc(zeros.data(), zeros.size()), 0xf9e9578eU);

  // Single-bit flip must change the CRC.
  std::vector<uint8_t> modified = zeros;
  modified[400] = 0x80;
  EXPECT_NE(H5_checksum_crc(modified.data(), modified.size()), 0xf9e9578eU);
}

TEST(H5ChecksumTest, Lookup3CoversAllRemainderCasesInitvalsAndAlignments) {
  constexpr char kPhrase[] = "Four score and seven years ago";
  constexpr size_t kPhraseLen = sizeof(kPhrase) - 1;
  EXPECT_EQ(H5_checksum_lookup3(kPhrase, kPhraseLen, 0U), 0x17770551U);
  EXPECT_EQ(H5_checksum_lookup3(kPhrase, kPhraseLen, 1U), 0xcd628161U);

  // Expected hashes for lengths 1..14 of "abcdefghijklmnop" (initval = 0),
  // hitting every fallthrough case (12 down to 1) and the >12 mix loop.
  constexpr auto kExpectedByLen = std::to_array<uint32_t>({
      0x58d68708U,  // len=1
      0xfbb3a8dfU,  // len=2
      0x0e397631U,  // len=3
      0xb5f4889cU,  // len=4
      0x026d72deU,  // len=5
      0xd6fa502eU,  // len=6
      0xb11ad4a5U,  // len=7
      0x2995c3beU,  // len=8
      0xac6572b4U,  // len=9
      0x8bf7d2efU,  // len=10
      0x5f61edf8U,  // len=11
      0x4012f87bU,  // len=12
      0x928128f9U,  // len=13 (1 mix + case 1)
      0x2bb84ef8U,  // len=14 (1 mix + case 2)
  });

  constexpr char kAlpha[] = "abcdefghijklmnop";
  for (size_t i = 0; i < kExpectedByLen.size(); ++i) {
    EXPECT_EQ(H5_checksum_lookup3(kAlpha, i + 1, 0U), kExpectedByLen[i])
        << "len=" << (i + 1);
  }

  // Unaligned buffer offsets (0, 1, 2, 3) must produce identical hashes.
  std::array<uint8_t, 32> unaligned{};
  for (size_t offset = 0; offset < 4; ++offset) {
    std::memcpy(unaligned.data() + offset, kAlpha, 14);
    EXPECT_EQ(H5_checksum_lookup3(unaligned.data() + offset, 14, 0U),
              kExpectedByLen[13])
        << "offset=" << offset;
  }

  // Multi-block buffer (37 bytes = 3 mix blocks + 1 remainder byte).
  std::vector<uint8_t> long_buf(37);
  for (size_t i = 0; i < long_buf.size(); ++i) {
    long_buf[i] = static_cast<uint8_t>(i);
  }
  const uint32_t h0 = H5_checksum_lookup3(long_buf.data(), long_buf.size(), 0U);
  const uint32_t h1 =
      H5_checksum_lookup3(long_buf.data(), long_buf.size(), 0x12345678U);
  EXPECT_NE(h0, h1);
}

TEST(H5ChecksumTest, MetadataWrapsLookup3AndRoundTripsWithFileChecksums) {
  std::array<uint8_t, 64> buf{};
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<uint8_t>(i * 3 + 5);
  }
  for (const size_t len :
       {size_t{1}, size_t{7}, size_t{12}, size_t{25}, size_t{64}}) {
    EXPECT_EQ(H5_checksum_metadata(buf.data(), len, 0xabcdef01U),
              H5_checksum_lookup3(buf.data(), len, 0xabcdef01U));
  }

  // End-to-end check: latest superblock/metadata uses lookup3 metadata
  // checksums and a fletcher32-filtered chunked dataset exercises
  // H5_checksum_fletcher32 during write and read.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);

  ScopedFile file(
      H5Fcreate("checksum_rt.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDims[1] = {8};
  constexpr hsize_t kChunk[1] = {4};
  ScopedSpace space(H5Screate_simple(1, kDims, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunk), 0);
  ASSERT_GE(H5Pset_fletcher32(dcpl.get()), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "checked", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  constexpr std::array<int32_t, 8> kWrite = {10, 20, 30, 40, 50, 60, 70, 80};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, kWrite.data()),
            0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

  std::array<int32_t, 8> read_back{};
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    read_back.data()),
            0);
  EXPECT_THAT(read_back, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));
}

TEST(H5ChecksumTest, HashStringComputesDjb2) {
  EXPECT_EQ(H5_hash_string(""), 5381U);
  for (const auto& vec : kSmallVectors) {
    EXPECT_EQ(H5_hash_string(vec.input), vec.expected_djb2)
        << "input=" << vec.input;
  }

  EXPECT_NE(H5_hash_string("dataset"), H5_hash_string("DATASET"));
  EXPECT_NE(H5_hash_string("abc"), H5_hash_string("abcd"));
}

}  // namespace
}  // namespace hdf5
