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
// Unit tests for LercNS::Lerc2 (third_party/lerc/LercLib/Lerc2.cpp).
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc2.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc2.cpp

#include "third_party/lerc/LercLib/Lerc2.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/lerc/LercLib/BitMask.h"
#include "third_party/lerc/LercLib/Defines.h"
#include "third_party/lerc/LercLib/RLE.h"
#include "third_party/lerc/google/util/lerc_test_util.h"

namespace lerc {
namespace {

using ::LercNS::BitMask;
using ::LercNS::Byte;
using ::LercNS::Lerc2;
using ::LercNS::RLE;
using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::FloatEq;

std::vector<Byte> PatchedTruncateBlob(absl::Span<const Byte> src,
                                      std::size_t new_size) {
  std::vector<Byte> out(src.begin(), src.begin() + new_size);
  if (out.size() >= kLerc2DataTypeOffset) {
    WritePodAt(out, kLerc2BlobSizeOffset, static_cast<int>(out.size()));
    EXPECT_TRUE(RecomputeChecksum(out));
  }
  return out;
}

template <typename T>
std::vector<Byte> EncodeBlob(Lerc2& enc, const T* data, double max_z_error,
                             bool encode_mask = true) {
  const unsigned int num_bytes =
      enc.ComputeNumBytesNeededToWrite(data, max_z_error, encode_mask);
  if (num_bytes == 0U) {
    ADD_FAILURE() << "ComputeNumBytesNeededToWrite returned 0";
    return {};
  }
  std::vector<Byte> blob(num_bytes);
  Byte* dst = blob.data();
  if (!enc.Encode(data, &dst)) {
    ADD_FAILURE() << "Encode failed";
    return {};
  }
  EXPECT_EQ(static_cast<std::size_t>(dst - blob.data()), blob.size());
  return blob;
}

template <typename T>
[[nodiscard]] bool DecodeBlob(absl::Span<const Byte> blob, T* dst,
                              Byte* mask = nullptr) {
  Lerc2 dec;
  const Byte* ptr = blob.data();
  std::size_t rem = blob.size();
  if (!dec.Decode(&ptr, rem, dst, mask)) {
    return false;
  }
  EXPECT_EQ(0U, rem);
  return true;
}

template <typename T>
void ExpectEveryPrefixFails(absl::Span<const Byte> blob, std::size_t num_elems,
                            std::size_t from = 0, bool patch_checksum = false) {
  std::vector<T> out(num_elems);
  for (std::size_t trunc = from; trunc < blob.size(); ++trunc) {
    Lerc2 dec;
    if (patch_checksum) {
      const std::vector<Byte> patched = PatchedTruncateBlob(blob, trunc);
      const Byte* p = patched.data();
      std::size_t rem = patched.size();
      EXPECT_FALSE(dec.Decode(&p, rem, out.data()));
    } else {
      const Byte* p = blob.data();
      std::size_t rem = trunc;
      EXPECT_FALSE(dec.Decode(&p, rem, out.data()));
    }
  }
}

std::vector<double> MakeHighEntropyDoubles(std::size_t count,
                                           std::uint64_t seed,
                                           double scale = 1.0) {
  std::vector<double> out(count);
  std::uint64_t state = seed;
  for (double& v : out) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    v = static_cast<double>(state & 0xFFFFFFFFULL) * scale;
  }
  return out;
}

template <typename T>
void ExpectAllScalarRoundTrip(int version, Lerc2::DataType expected_dt) {
  SCOPED_TRACE(version);
  SCOPED_TRACE(static_cast<int>(expected_dt));
  constexpr int kRows = 20;
  constexpr int kCols = 28;
  constexpr int kDepth = 1;

  BitMask mask(kCols, kRows);
  mask.SetAllValid();
  mask.SetInvalid(0, 0);
  mask.SetInvalid(kRows - 1, kCols - 1);

  std::vector<T> src(kRows * kCols);
  for (int i = 0; i < kRows; ++i) {
    for (int j = 0; j < kCols; ++j) {
      const int idx = i * kCols + j;
      const int tile_id = (i / 8) * 4 + (j / 8);
      const int delta = (i % 8) * 2 + (j % 8);  // 0..21 within each 8x8 tile
      if constexpr (std::is_same_v<T, signed char>) {
        constexpr std::array<signed char, 3> kBases = {-80, 0, 40};
        src[idx] = static_cast<signed char>(kBases[tile_id % 3] + delta);
      } else if constexpr (std::is_same_v<T, Byte>) {
        constexpr std::array<Byte, 3> kBases = {0, 80, 180};
        src[idx] = static_cast<Byte>(kBases[tile_id % 3] + delta);
      } else if constexpr (std::is_same_v<T, std::int16_t>) {
        constexpr std::array<std::int16_t, 3> kBases = {-50, 10, -300};
        src[idx] = static_cast<std::int16_t>(kBases[tile_id % 3] + delta);
      } else if constexpr (std::is_same_v<T, std::uint16_t>) {
        constexpr std::array<std::uint16_t, 3> kBases = {10, 400, 40000};
        src[idx] = static_cast<std::uint16_t>(kBases[tile_id % 3] + delta);
      } else if constexpr (std::is_same_v<T, int>) {
        constexpr std::array<int, 4> kBases = {10, -300, 40000, -100000};
        src[idx] = kBases[tile_id % 4] + delta;
      } else if constexpr (std::is_same_v<T, unsigned int>) {
        constexpr std::array<unsigned int, 3> kBases = {10U, 40000U, 100000U};
        src[idx] = kBases[tile_id % 3] + static_cast<unsigned int>(delta);
      } else if constexpr (std::is_same_v<T, float>) {
        constexpr std::array<float, 3> kBases = {10.0f, -300.0f, 100.25f};
        src[idx] = kBases[tile_id % 3] + static_cast<float>(delta) * 0.25f;
      } else {
        constexpr std::array<double, 4> kBases = {10.0, 100000.0, 100000.25,
                                                  1.12345678901234};
        src[idx] = kBases[tile_id % 4] + static_cast<double>(delta) * 0.25;
      }
    }
  }

  Lerc2 enc(kDepth, kCols, kRows, mask.Bits());
  ASSERT_TRUE(enc.SetEncoderToOldVersion(version));
  const double tol = std::is_floating_point_v<T> ? 0.125 : 0.5;
  const std::vector<Byte> blob = EncodeBlob(enc, src.data(), tol);
  ASSERT_FALSE(blob.empty());

  Lerc2::HeaderInfo hd{};
  bool has_mask = false;
  ASSERT_TRUE(Lerc2::GetHeaderInfo(blob.data(), blob.size(), hd, has_mask));
  EXPECT_EQ(version, hd.version);
  EXPECT_EQ(expected_dt, hd.dt);
  EXPECT_TRUE(has_mask);
  EXPECT_EQ(kRows * kCols - 2, hd.numValidPixel);

  std::vector<T> dst(kRows * kCols, static_cast<T>(99));
  std::vector<Byte> out_mask(mask.Size(), 0);
  ASSERT_TRUE(DecodeBlob(blob, dst.data(), out_mask.data()));
  EXPECT_THAT(out_mask, ElementsAreArray(mask.Bits(), mask.Size()));

  for (int k = 0; k < kRows * kCols; ++k) {
    if (!mask.IsValid(k)) {
      EXPECT_EQ(static_cast<T>(0), dst[k]);
    } else if constexpr (std::is_floating_point_v<T>) {
      EXPECT_NEAR(static_cast<double>(src[k]), static_cast<double>(dst[k]),
                  hd.maxZError);
    } else {
      EXPECT_EQ(src[k], dst[k]);
    }
  }

  if (version >= 4) {
    Lerc2 dec;
    double rmin = 777.0;
    double rmax = 777.0;
    ASSERT_TRUE(dec.GetRanges(blob.data(), blob.size(), &rmin, &rmax));
    EXPECT_DOUBLE_EQ(hd.zMin, rmin);
    EXPECT_DOUBLE_EQ(hd.zMax, rmax);
  }
}

TEST(Lerc2Test, ConfigurationSettersAndVersionGuards) {
  Lerc2 lerc;
  EXPECT_EQ(6, Lerc2::CurrentVersion());

  EXPECT_FALSE(lerc.SetEncoderToOldVersion(1));
  EXPECT_FALSE(lerc.SetEncoderToOldVersion(Lerc2::CurrentVersion() + 1));

  // nDepth > 1 incompatible with versions < 4.
  EXPECT_TRUE(lerc.Set(3, 4, 4));
  EXPECT_FALSE(lerc.SetEncoderToOldVersion(3));
  EXPECT_TRUE(lerc.Set(1, 4, 4));
  EXPECT_TRUE(lerc.SetEncoderToOldVersion(3));
  EXPECT_FALSE(lerc.Set(2, 4, 4));

  // v6-only setters reject version < 6 and accept version 6.
  EXPECT_FALSE(lerc.SetNoDataValues(true, -9999.0, -9999.0));
  EXPECT_FALSE(lerc.SetNumBlobsMoreToCome(2));
  EXPECT_FALSE(lerc.SetIsAllInt(true));

  EXPECT_TRUE(lerc.SetEncoderToOldVersion(6));
  EXPECT_TRUE(lerc.SetNoDataValues(true, -1.0, 1.0e30));
  EXPECT_TRUE(lerc.SetNumBlobsMoreToCome(3));
  EXPECT_TRUE(lerc.SetIsAllInt(true));
  EXPECT_TRUE(lerc.SetIsAllInt(false));
  EXPECT_TRUE(lerc.SetNoDataValues(false, 0.0, 0.0));

  EXPECT_FALSE(lerc.Set(1, 0, 4));
  EXPECT_FALSE(lerc.Set(1, 4, 0));
  // Failed Set clears m_bitMask (m_pBits == nullptr) while leaving old dims
  // in m_headerInfo; restore a valid mask/size before encoding.
  ASSERT_TRUE(lerc.Set(1, 4, 4));

  // SetMinMax / ClearMinMax validation.
  EXPECT_FALSE(lerc.SetMinMax(2, 0.0, 1.0));
  EXPECT_FALSE(lerc.SetMinMax(1, 5.0, 2.0));
  EXPECT_TRUE(lerc.SetMinMax(1, -10.0, 50.0));

  std::vector<int> vals = {1, 2,  3,  4,  5,  6,  7,  8,
                           9, 10, 11, 12, 13, 14, 15, 16};
  const std::vector<Byte> blob = EncodeBlob(lerc, vals.data(), 0.5);
  ASSERT_FALSE(blob.empty());
  Lerc2::HeaderInfo hd{};
  bool has_mask = true;
  ASSERT_TRUE(Lerc2::GetHeaderInfo(blob.data(), blob.size(), hd, has_mask));
  EXPECT_FALSE(has_mask);
  EXPECT_DOUBLE_EQ(-10.0, hd.zMin);
  EXPECT_DOUBLE_EQ(50.0, hd.zMax);
  EXPECT_EQ(3, hd.nBlobsMore);

  lerc.ClearMinMax();
  const std::vector<Byte> blob2 = EncodeBlob(lerc, vals.data(), 0.5);
  ASSERT_FALSE(blob2.empty());
  ASSERT_TRUE(Lerc2::GetHeaderInfo(blob2.data(), blob2.size(), hd, has_mask));
  EXPECT_DOUBLE_EQ(1.0, hd.zMin);
  EXPECT_DOUBLE_EQ(16.0, hd.zMax);
}

TEST(Lerc2Test, AllEightScalarTypesRoundTripAcrossVersions) {
  for (const int version : {3, 4, 5, 6}) {
    ExpectAllScalarRoundTrip<signed char>(version, Lerc2::DT_Char);
    ExpectAllScalarRoundTrip<Byte>(version, Lerc2::DT_Byte);
    ExpectAllScalarRoundTrip<std::int16_t>(version, Lerc2::DT_Short);
    ExpectAllScalarRoundTrip<std::uint16_t>(version, Lerc2::DT_UShort);
    ExpectAllScalarRoundTrip<int>(version, Lerc2::DT_Int);
    ExpectAllScalarRoundTrip<unsigned int>(version, Lerc2::DT_UInt);
    ExpectAllScalarRoundTrip<float>(version, Lerc2::DT_Float);
    ExpectAllScalarRoundTrip<double>(version, Lerc2::DT_Double);
  }
}

TEST(Lerc2Test, Version2MaskedTileAlignedAndCorruptBufferVecGuard) {
  // Lerc2 v2 uses BitStuff_Before_Lerc2v3 which casts *ppByte to unsigned int*.
  // Compute the exact byte count before BitStuffer2 payload and start the blob
  // at an offset keeping that payload 4-byte aligned.
  constexpr int kRows = 4;
  constexpr int kCols = 4;
  BitMask mask(kCols, kRows);
  mask.SetAllValid();
  mask.SetInvalid(0, 0);

  std::vector<int> src(kRows * kCols);
  for (int i = 0; i < kRows * kCols; ++i) {
    // Pixel 0 is masked invalid, so zMin = 11 fits in Byte (1B); 15 elements
    // pack at 4 bits each.
    src[i] = 10 + i;
  }

  RLE rle;
  const std::size_t rle_len = rle.computeNumBytesRLE(mask.Bits(), mask.Size());
  // v2 header (58) + mask size (4) + rle + sweep flag (1) + comprFlag (1) +
  // reduced zMin Byte (1) + BitStuffer2 numBitsByte (1) + numElem Byte (1).
  const std::size_t pre_bitstuff =
      kLerc2V2HeaderBytes + 4U + rle_len + 1U + 1U + 1U + 2U;
  const std::size_t start_off = AlignPadTo4(pre_bitstuff);

  Lerc2 enc(1, kCols, kRows, mask.Bits());
  ASSERT_TRUE(enc.SetEncoderToOldVersion(2));
  const unsigned int num_bytes =
      enc.ComputeNumBytesNeededToWrite(src.data(), 0.5, true);
  ASSERT_GT(num_bytes, 0U);

  // Reserve +2 uint32 words of tail headroom: BitStuff_Before_Lerc2v3
  // memset-zeroes full uint32 words up to 3 bytes past
  // ComputeNumBytesNeededToWrite().
  std::vector<std::uint32_t> aligned((start_off + num_bytes + 3U) / 4U + 2U,
                                     0U);
  Byte* blob_begin = reinterpret_cast<Byte*>(aligned.data()) + start_off;
  Byte* dst_ptr = blob_begin;
  ASSERT_TRUE(enc.Encode(src.data(), &dst_ptr));
  ASSERT_EQ(static_cast<std::size_t>(dst_ptr - blob_begin), num_bytes);

  std::vector<int> dst(src.size(), 99);
  ASSERT_TRUE(
      DecodeBlob(absl::Span<const Byte>(blob_begin, num_bytes), dst.data()));
  EXPECT_EQ(0, dst[0]);
  for (int i = 1; i < kRows * kCols; ++i) {
    EXPECT_EQ(src[i], dst[i]);
  }

  // Truncated v2 masked-tile blob also fails cleanly.
  ExpectEveryPrefixFails<int>(absl::Span<const Byte>(blob_begin, num_bytes),
                              dst.size());

  // Lower numElements inside v2 BitStuffer2 header to trigger Lerc2::ReadTile's
  // v <= 2 bufferVecIdx == bufferVec.size() guard cleanly.
  blob_begin[pre_bitstuff - 1] = 2;
  Lerc2 dec;
  const Byte* p = blob_begin;
  std::size_t rem = num_bytes;
  EXPECT_FALSE(dec.Decode(&p, rem, dst.data()));

  // Also test truncated v2 const-image prefixes.
  std::vector<int> const_src(16, 42);
  const std::vector<Byte> v2_const = EncodeBlob(enc, const_src.data(), 0.5);
  ASSERT_FALSE(v2_const.empty());
  ExpectEveryPrefixFails<int>(v2_const, dst.size());
}

TEST(Lerc2Test, EmptyConstMultiDepthConstAndUnencodedMask) {
  // 1. Empty image (all invalid mask).
  {
    BitMask empty_mask(8, 8);
    empty_mask.SetAllInvalid();
    std::vector<float> src(64, 12.5f);
    Lerc2 enc(2, 8, 8, empty_mask.Bits());
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.1);
    ASSERT_FALSE(blob.empty());

    Lerc2::HeaderInfo hd{};
    bool has_mask = true;
    ASSERT_TRUE(Lerc2::GetHeaderInfo(blob.data(), blob.size(), hd, has_mask));
    EXPECT_FALSE(has_mask);
    EXPECT_EQ(0, hd.numValidPixel);

    Lerc2 dec;
    std::array<double, 2> mins = {-1.0, -1.0};
    std::array<double, 2> maxs = {-1.0, -1.0};
    ASSERT_TRUE(
        dec.GetRanges(blob.data(), blob.size(), mins.data(), maxs.data()));
    EXPECT_DOUBLE_EQ(0.0, mins[0]);
    EXPECT_DOUBLE_EQ(0.0, mins[1]);
    EXPECT_DOUBLE_EQ(0.0, maxs[0]);
    EXPECT_DOUBLE_EQ(0.0, maxs[1]);

    std::vector<float> dst(128, 7.0f);
    std::vector<Byte> out_mask(empty_mask.Size(), 0xFF);
    ASSERT_TRUE(DecodeBlob(blob, dst.data(), out_mask.data()));
    EXPECT_THAT(dst, Each(FloatEq(0.0f)));
    EXPECT_THAT(out_mask,
                ElementsAreArray(empty_mask.Bits(), empty_mask.Size()));
  }

  // 2. Single-value constant image with encodeMask = false.
  {
    BitMask mask(8, 8);
    mask.SetAllValid();
    mask.SetInvalid(3, 5);
    std::vector<double> src(64, 42.25);

    Lerc2 enc(1, 8, 8, mask.Bits());
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.0, false);
    ASSERT_FALSE(blob.empty());
    Lerc2::HeaderInfo hd{};
    bool has_mask = true;
    ASSERT_TRUE(Lerc2::GetHeaderInfo(blob.data(), blob.size(), hd, has_mask));
    EXPECT_FALSE(has_mask);

    // Pre-seed decoder mask so unencoded mask reuses previous mask.
    Lerc2 dec(1, 8, 8, mask.Bits());
    double rmin = 777.0;
    double rmax = 777.0;
    ASSERT_TRUE(dec.GetRanges(blob.data(), blob.size(), &rmin, &rmax));
    EXPECT_DOUBLE_EQ(42.25, rmin);
    EXPECT_DOUBLE_EQ(42.25, rmax);

    std::vector<double> dst(64, -1.0);
    const Byte* ptr = blob.data();
    std::size_t rem = blob.size();
    ASSERT_TRUE(dec.Decode(&ptr, rem, dst.data()));
    EXPECT_EQ(0U, rem);
    EXPECT_DOUBLE_EQ(0.0, dst[3 * 8 + 5]);
    EXPECT_DOUBLE_EQ(42.25, dst[0]);
  }

  // 3. Per-depth-constant image (nDepth=3): each slice constant, slices differ,
  // triggering minMaxEqual == true in CheckMinMaxRanges / FillConstImage.
  {
    BitMask mask(6, 6);
    mask.SetAllValid();
    mask.SetInvalid(2, 2);
    std::vector<int> src(36 * 3);
    for (int k = 0; k < 36; ++k) {
      src[k * 3 + 0] = 5;
      src[k * 3 + 1] = 100;
      src[k * 3 + 2] = -20;
    }

    Lerc2 enc(3, 6, 6, mask.Bits());
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());
    Lerc2 dec;
    std::array<double, 3> mins{};
    mins.fill(777.0);
    std::array<double, 3> maxs{};
    maxs.fill(777.0);
    ASSERT_TRUE(
        dec.GetRanges(blob.data(), blob.size(), mins.data(), maxs.data()));
    EXPECT_DOUBLE_EQ(5.0, mins[0]);
    EXPECT_DOUBLE_EQ(100.0, mins[1]);
    EXPECT_DOUBLE_EQ(-20.0, mins[2]);
    EXPECT_DOUBLE_EQ(5.0, maxs[0]);
    EXPECT_DOUBLE_EQ(100.0, maxs[1]);
    EXPECT_DOUBLE_EQ(-20.0, maxs[2]);

    std::vector<int> dst(36 * 3, -999);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    EXPECT_EQ(5, dst[0]);
    EXPECT_EQ(100, dst[1]);
    EXPECT_EQ(-20, dst[2]);
    EXPECT_EQ(0, dst[(2 * 6 + 2) * 3 + 1]);
  }
}

TEST(Lerc2Test, HuffmanEightBitOrigDeltaMaskedAndMultiDepth) {
  // 1. Skewed uncorrelated values -> IEM_Huffman on Byte, both all-valid and
  // masked, so DecodeHuffman's all-valid and masked IEM_Huffman branches run.
  for (const bool with_mask : {false, true}) {
    constexpr int kRows = 24;
    constexpr int kCols = 24;
    BitMask mask(kCols, kRows);
    mask.SetAllValid();
    if (with_mask) {
      mask.SetInvalid(0, 1);
      mask.SetInvalid(10, 10);
    }

    std::vector<Byte> src(kRows * kCols * 3, 7);
    for (int i = 0; i < kRows * kCols * 3; i += 29) {
      src[i] = static_cast<Byte>((i * 53) % 250);
    }

    Lerc2 enc(3, kCols, kRows, with_mask ? mask.Bits() : nullptr);
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());

    std::vector<Byte> dst(src.size(), 99);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    for (int k = 0; k < kRows * kCols; ++k) {
      if (!with_mask || mask.IsValid(k)) {
        for (int m = 0; m < 3; ++m) {
          EXPECT_EQ(src[k * 3 + m], dst[k * 3 + m]);
        }
      }
    }
  }

  // 2. Smooth 2D ramp -> IEM_DeltaHuffman on signed char (offset=128) both
  // all-valid and masked, across v2/v3 (delta-only) and v6.
  for (const bool with_mask : {false, true}) {
    for (const int version : {3, 6}) {
      constexpr int kRows = 24;
      constexpr int kCols = 24;
      BitMask mask(kCols, kRows);
      mask.SetAllValid();
      if (with_mask) {
        mask.SetInvalid(0, 5);
        mask.SetInvalid(3, 7);
        mask.SetInvalid(12, 12);
      }

      std::vector<signed char> src(kRows * kCols);
      for (int i = 0; i < kRows; ++i) {
        for (int j = 0; j < kCols; ++j) {
          src[i * kCols + j] = static_cast<signed char>(-60 + i * 2 + j * 3);
        }
      }

      Lerc2 enc(1, kCols, kRows, with_mask ? mask.Bits() : nullptr);
      ASSERT_TRUE(enc.SetEncoderToOldVersion(version));
      const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
      ASSERT_FALSE(blob.empty());

      std::vector<signed char> dst(src.size(), 99);
      ASSERT_TRUE(DecodeBlob(blob, dst.data()));
      for (int k = 0; k < kRows * kCols; ++k) {
        if (!with_mask || mask.IsValid(k)) {
          EXPECT_EQ(src[k], dst[k]);
        }
      }
    }
  }
}

TEST(Lerc2Test, LosslessFloatHuffmanOneSweepRawBinaryLutAndLargeBlock) {
  // 1. v6 smooth float and double -> TryHuffmanFlt (IEM_DeltaDeltaHuffman).
  // MSan safety on blob_f depends on upstream EncodeHuffman's heuristic and
  // stream termination for this test input.
  {
    constexpr int kRows = 32;
    constexpr int kCols = 32;
    std::vector<float> src_f(kRows * kCols);
    std::vector<double> src_d(kRows * kCols * 2);
    for (int i = 0; i < kRows; ++i) {
      for (int j = 0; j < kCols; ++j) {
        const int idx = i * kCols + j;
        src_f[idx] = 100.0f + static_cast<float>(i) * 0.5f +
                     static_cast<float>(j) * 0.25f;
        src_d[idx * 2 + 0] = src_f[idx];
        src_d[idx * 2 + 1] = src_f[idx] + 5.0;
      }
    }

    Lerc2 enc_f(1, kCols, kRows);
    const std::vector<Byte> blob_f = EncodeBlob(enc_f, src_f.data(), 0.0);
    ASSERT_FALSE(blob_f.empty());
    std::vector<float> dst_f(src_f.size(), -1.0f);
    ASSERT_TRUE(DecodeBlob(blob_f, dst_f.data()));
    EXPECT_EQ(src_f, dst_f);

#if !defined(MEMORY_SANITIZER)
    // TODO(schwehr): Report upstream: fpl_EsriHuffman::EncodeHuffman
    // (HUFFMAN_NORMAL) allocates with malloc() without zero-clearing and
    // includes 4 trailing decode-LUT read-ahead padding bytes, which
    // Lerc2::ComputeChecksumFletcher32 hashes.
    Lerc2 enc_d(2, kCols, kRows);
    const std::vector<Byte> blob_d = EncodeBlob(enc_d, src_d.data(), 0.0);
    ASSERT_FALSE(blob_d.empty());
    std::vector<double> dst_d(src_d.size(), -1.0);
    ASSERT_TRUE(DecodeBlob(blob_d, dst_d.data()));
    EXPECT_EQ(src_d, dst_d);
#endif
  }

  // 2. WriteDataOneSweep / ReadDataOneSweep with high-entropy masked data so
  // tiling cannot beat raw one-sweep size. Also exceed 720 bytes for
  // Fletcher-32; the odd-length tail branch depends on (blobSize - 14) parity.
  {
    constexpr int kRows = 21;
    constexpr int kCols = 17;
    BitMask mask(kCols, kRows);
    mask.SetAllValid();
    mask.SetInvalid(0, 0);
    const std::vector<double> src = MakeHighEntropyDoubles(
        kRows * kCols, 0x9E3779B97F4A7C15ULL, 1.1234567890123);

    Lerc2 enc(1, kCols, kRows, mask.Bits());
    ASSERT_TRUE(enc.SetEncoderToOldVersion(5));
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.0);
    ASSERT_FALSE(blob.empty());
    EXPECT_GT(blob.size(), 800U);

    std::vector<double> dst(src.size(), -999.0);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    for (std::size_t k = 1; k < src.size(); ++k) {
      EXPECT_DOUBLE_EQ(src[k], dst[k]);
    }
    EXPECT_DOUBLE_EQ(0.0, dst[0]);
  }

  // 3. BEM_RawBinary safely on Byte (sizeof == 1 avoids UBSan misaligned
  // pointer casts inside Lerc2::ReadTile): left 8x8 tile is constant 0 (1B),
  // right 8x8 tile has only 2 valid distinct bytes (nBytesRaw=3 < BitStuff).
  {
    constexpr int kRows = 8;
    constexpr int kCols = 16;
    BitMask mask(kCols, kRows);
    mask.SetAllInvalid();
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 8; ++j) {
        mask.SetValid(i, j);
      }
    }
    mask.SetValid(0, 8);
    mask.SetValid(0, 9);

    std::vector<Byte> src(kRows * kCols, 0);
    src[0 * kCols + 8] = 17;
    src[0 * kCols + 9] = 240;

    Lerc2 enc(1, kCols, kRows, mask.Bits());
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());

    std::vector<Byte> dst(src.size(), 0xFF);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 8; ++j) {
        EXPECT_EQ(0, dst[i * kCols + j]);
      }
    }
    EXPECT_EQ(17, dst[0 * kCols + 8]);
    EXPECT_EQ(240, dst[0 * kCols + 9]);
    EXPECT_EQ(0, dst[0 * kCols + 10]);
  }

  // 4. Empty left tile (numValidPixel == 0) + non-constant right tile hits
  // WriteTiles's numValidPixel == 0 && !(*ppByte) early break.
  {
    constexpr int kRows = 8;
    constexpr int kCols = 16;
    BitMask mask(kCols, kRows);
    mask.SetAllInvalid();
    std::vector<int> src(kRows * kCols, 0);
    for (int i = 0; i < 8; ++i) {
      for (int j = 8; j < 16; ++j) {
        mask.SetValid(i, j);
        src[i * kCols + j] = i * 10 + j;
      }
    }
    Lerc2 enc(1, kCols, kRows, mask.Bits());
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());
    std::vector<int> dst(src.size(), -1);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    for (int i = 0; i < 8; ++i) {
      for (int j = 8; j < 16; ++j) {
        EXPECT_EQ(src[i * kCols + j], dst[i * kCols + j]);
      }
    }
  }

  // 5. Distinct per-band constants trigger CheckMinMaxRanges minMaxEqual.
  {
    constexpr int kRows = 4;
    constexpr int kCols = 4;
    constexpr int kDepth = 2;
    std::vector<int> src(kRows * kCols * kDepth);
    for (int k = 0; k < kRows * kCols; ++k) {
      src[k * 2 + 0] = 5;
      src[k * 2 + 1] = 19;
    }
    Lerc2 enc(kDepth, kCols, kRows);
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());
    std::vector<int> dst(src.size(), -999);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    EXPECT_EQ(src, dst);
  }

  // 6. BEM_BitStuffLUT (repeated background + sparse large spikes).
  {
    constexpr int kRows = 16;
    constexpr int kCols = 16;
    std::vector<unsigned int> src(kRows * kCols, 100);
    src[0 * kCols + 1] = 1000000;
    src[2 * kCols + 3] = 2000000;
    src[9 * kCols + 9] = 3000000;

    Lerc2 enc(1, kCols, kRows);
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());
    std::vector<unsigned int> dst(src.size(), 999U);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    EXPECT_EQ(src, dst);
  }

  // 7. microBlockSize doubling from 8 to 16 (32x32 low-bitrate uint16).
  {
    constexpr int kRows = 32;
    constexpr int kCols = 32;
    std::vector<std::uint16_t> src(kRows * kCols, 10);
    // Gentle 16x16 plateaus -> < 1.5 bpp -> microBlockSize doubles to 16.
    for (int i = 16; i < 32; ++i) {
      for (int j = 0; j < 16; ++j) {
        src[i * kCols + j] = 20;
      }
      for (int j = 16; j < 32; ++j) {
        src[i * kCols + j] = 30;
      }
    }

    Lerc2 enc(1, kCols, kRows);
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());
    Lerc2::HeaderInfo hd{};
    bool has_mask = false;
    ASSERT_TRUE(Lerc2::GetHeaderInfo(blob.data(), blob.size(), hd, has_mask));
    EXPECT_EQ(16, hd.microBlockSize);

    std::vector<std::uint16_t> dst(src.size(), 999U);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    EXPECT_EQ(src, dst);
  }
}

TEST(Lerc2Test, DiffSliceEncodingAndOverflowFallback) {
  // 1. nDepth=4 integer lossless v6:
  //    depth 0: ramp;
  //    depth 1: equal to depth 0 -> bDiffEnc const-0 (comprFlag 2);
  //    depth 2: depth 1 + 10 -> bDiffEnc const offset (comprFlag 3);
  //    depth 3: depth 2 + sparse LUT-friendly diff spikes.
  {
    constexpr int kRows = 8;
    constexpr int kCols = 8;
    constexpr int kDepth = 4;
    BitMask mask(kCols, kRows);
    mask.SetAllValid();
    mask.SetInvalid(0, 0);

    std::vector<int> src(kRows * kCols * kDepth, 0);
    for (int k = 0; k < kRows * kCols; ++k) {
      const int v0 = 1000 + k * 7;
      src[k * kDepth + 0] = v0;
      src[k * kDepth + 1] = v0;
      src[k * kDepth + 2] = v0 + 10;
      src[k * kDepth + 3] = v0 + 10 + ((k == 5 || k == 19) ? 50000 : 1);
    }

    Lerc2 enc(kDepth, kCols, kRows, mask.Bits());
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());

    std::vector<int> dst(src.size(), -99);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    EXPECT_EQ(0, dst[0]);
    for (int k = 1; k < kRows * kCols; ++k) {
      for (int m = 0; m < kDepth; ++m) {
        EXPECT_EQ(src[k * kDepth + m], dst[k * kDepth + m]);
      }
    }
  }

  // 2. Same diff-slice behavior with all pixels valid (exercises
  // Lerc2::ReadTile's bufferVec.size() == maxElementCount fast path).
  {
    constexpr int kRows = 8;
    constexpr int kCols = 8;
    constexpr int kDepth = 3;
    std::vector<std::int16_t> src(kRows * kCols * kDepth, 0);
    for (int k = 0; k < kRows * kCols; ++k) {
      const std::int16_t v0 = static_cast<std::int16_t>(100 + k * 3);
      src[k * kDepth + 0] = v0;
      src[k * kDepth + 1] = static_cast<std::int16_t>(v0 + 5);
      src[k * kDepth + 2] = static_cast<std::int16_t>(v0 + 5 + (k % 3));
    }

    Lerc2 enc(kDepth, kCols, kRows);
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());
    std::vector<std::int16_t> dst(src.size(), -99);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    EXPECT_EQ(src, dst);
  }

  // 3. NeedToCheckForIntOverflow: global zMax - zMin >= 0x7FFFFFFF across two
  // 8x8 tiles triggers ComputeDiffSliceInt's bCheckForIntOverflow path cleanly.
  // TODO(schwehr): Report upstream: Lerc2::ComputeDiffSliceInt casts
  // `double z` to `int` before checking [zIntMin, zIntMax], so actual
  // int32-diff overflow triggers signed-conversion UB.
  {
    constexpr int kRows = 8;
    constexpr int kCols = 16;
    constexpr int kDepth = 2;
    std::vector<int> src(kRows * kCols * kDepth, 0);
    for (int i = 0; i < kRows; ++i) {
      for (int j = 0; j < kCols; ++j) {
        const int k = i * kCols + j;
        if (j < 8) {
          src[k * 2 + 0] = -1500000000 + j;
          src[k * 2 + 1] = src[k * 2 + 0] + (i % 3);
        } else {
          src[k * 2 + 0] = 1500000000 + j;
          src[k * 2 + 1] = src[k * 2 + 0] + (i % 5);
        }
      }
    }

    Lerc2 enc(kDepth, kCols, kRows);
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());
    std::vector<int> dst(src.size(), -99);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    EXPECT_EQ(src, dst);
  }
  {
    constexpr int kRows = 8;
    constexpr int kCols = 16;
    constexpr int kDepth = 2;
    std::vector<unsigned int> src(kRows * kCols * kDepth, 0);
    for (int i = 0; i < kRows; ++i) {
      for (int j = 0; j < kCols; ++j) {
        const int k = i * kCols + j;
        if (j < 8) {
          src[k * 2 + 0] = 100U + static_cast<unsigned int>(j);
          src[k * 2 + 1] = src[k * 2 + 0] + 2U;
        } else {
          src[k * 2 + 0] = 3000000000U + static_cast<unsigned int>(j);
          src[k * 2 + 1] = src[k * 2 + 0] + 5U;
        }
      }
    }

    Lerc2 enc(kDepth, kCols, kRows);
    const std::vector<Byte> blob = EncodeBlob(enc, src.data(), 0.5);
    ASSERT_FALSE(blob.empty());
    std::vector<unsigned int> dst(src.size(), 999U);
    ASSERT_TRUE(DecodeBlob(blob, dst.data()));
    EXPECT_EQ(src, dst);
  }
}

TEST(Lerc2Test, BitPlaneCompressionAndTryRaiseMaxZError) {
  // 1. TryBitPlaneCompression (>= 5000 pixels, maxZError = 777 -> eps = 0.01):
  //    cover nDepth=1 unsigned, nDepth=1 signed, nDepth=2 / partial mask
  //    unsigned & signed, plus low-pixel fallback (< 5000).
  {
    constexpr int kRows = 80;
    constexpr int kCols = 80;  // 6400 > 5000
    std::vector<unsigned int> udata(kRows * kCols);
    std::vector<int> sdata(kRows * kCols);
    std::uint32_t rng = 0x12345678U;
    for (int i = 0; i < kRows; ++i) {
      for (int j = 0; j < kCols; ++j) {
        rng = rng * 1664525U + 1013904223U;
        const unsigned int noise = (rng >> 28) & 0xFU;  // bottom 4 bits noisy
        udata[i * kCols + j] = static_cast<unsigned int>((i + j) << 5) | noise;
        sdata[i * kCols + j] = static_cast<int>(((i + j - 40) * 32) | noise);
      }
    }

    Lerc2 enc_u(1, kCols, kRows);
    const std::vector<Byte> bu = EncodeBlob(enc_u, udata.data(), 777.0);
    ASSERT_FALSE(bu.empty());
    Lerc2::HeaderInfo hdu{};
    bool has_mask = false;
    ASSERT_TRUE(Lerc2::GetHeaderInfo(bu.data(), bu.size(), hdu, has_mask));
    EXPECT_GT(hdu.maxZError, 0.5);

    Lerc2 enc_s(1, kCols, kRows);
    const std::vector<Byte> bs = EncodeBlob(enc_s, sdata.data(), 777.0);
    ASSERT_FALSE(bs.empty());
    Lerc2::HeaderInfo hds{};
    ASSERT_TRUE(Lerc2::GetHeaderInfo(bs.data(), bs.size(), hds, has_mask));
    EXPECT_GT(hds.maxZError, 0.5);

    // nDepth=2 with invalid pixel hits general masked loop (both unsigned &
    // signed).
    BitMask mask(kCols, kRows);
    mask.SetAllValid();
    mask.SetInvalid(0, 0);
    std::vector<std::uint16_t> u2(kRows * kCols * 2);
    std::vector<std::int16_t> s2(kRows * kCols * 2);
    for (int k = 0; k < kRows * kCols; ++k) {
      rng = rng * 1664525U + 1013904223U;
      const std::uint16_t noise =
          static_cast<std::uint16_t>((rng >> 29) & 0x7U);
      u2[k * 2 + 0] = static_cast<std::uint16_t>((k % 120) << 4 | noise);
      u2[k * 2 + 1] = static_cast<std::uint16_t>((k % 100) << 4 | noise);
      s2[k * 2 + 0] = static_cast<std::int16_t>(u2[k * 2 + 0] - 500);
      s2[k * 2 + 1] = static_cast<std::int16_t>(u2[k * 2 + 1] - 500);
    }

    Lerc2 enc_u2(2, kCols, kRows, mask.Bits());
    const std::vector<Byte> bu2 = EncodeBlob(enc_u2, u2.data(), 777.0);
    ASSERT_FALSE(bu2.empty());
    ASSERT_TRUE(Lerc2::GetHeaderInfo(bu2.data(), bu2.size(), hdu, has_mask));
    EXPECT_GT(hdu.maxZError, 0.5);

    Lerc2 enc_s2(2, kCols, kRows, mask.Bits());
    const std::vector<Byte> bs2 = EncodeBlob(enc_s2, s2.data(), 777.0);
    ASSERT_FALSE(bs2.empty());
    ASSERT_TRUE(Lerc2::GetHeaderInfo(bs2.data(), bs2.size(), hds, has_mask));
    EXPECT_GT(hds.maxZError, 0.5);

    // Sparse mask on >= 5000 valid pixels whose valid neighbors < 5000 hits
    // cnt < minCnt fallback (checkerboard of 100x100 = 5000 valid, 0 adj
    // pairs).
    BitMask checker(100, 100);
    checker.SetAllInvalid();
    for (int i = 0; i < 100; ++i) {
      for (int j = (i & 1); j < 100; j += 2) {
        checker.SetValid(i, j);
      }
    }
    std::vector<unsigned int> ch(10000, 1);
    ch[2] = 99;  // row 0, col 2 is valid in (i & 1) checkerboard
    Lerc2 enc_ch(1, 100, 100, checker.Bits());
    const std::vector<Byte> bch = EncodeBlob(enc_ch, ch.data(), 777.0);
    ASSERT_FALSE(bch.empty());
    ASSERT_TRUE(Lerc2::GetHeaderInfo(bch.data(), bch.size(), hdu, has_mask));
    EXPECT_DOUBLE_EQ(0.5, hdu.maxZError);
    // Low pixel count (< 5000) falls back to lossless in
    // TryBitPlaneCompression.
    std::vector<int> small_bp(100);
    for (int k = 0; k < 100; ++k) {
      small_bp[k] = k * 3;
    }
    Lerc2 enc_small(1, 10, 10);
    const std::vector<Byte> bsmall =
        EncodeBlob(enc_small, small_bp.data(), 777.0);
    ASSERT_FALSE(bsmall.empty());
    ASSERT_TRUE(
        Lerc2::GetHeaderInfo(bsmall.data(), bsmall.size(), hds, has_mask));
    EXPECT_DOUBLE_EQ(0.5, hds.maxZError);
  }

  // 2. Float rejects negative maxZError (even 777 -> -0.01) and maxZError ==
  // 0.5 hits TryRaiseMaxZError's zErr.empty() early return.
  {
    std::vector<float> f(16, 1.0f);
    f[0] = 2.0f;
    Lerc2 enc(1, 4, 4);
    EXPECT_EQ(0U, enc.ComputeNumBytesNeededToWrite(f.data(), -0.01, true));
    EXPECT_EQ(0U, enc.ComputeNumBytesNeededToWrite(f.data(), 777.0, true));
    const std::vector<Byte> bf_half = EncodeBlob(enc, f.data(), 0.5);
    ASSERT_FALSE(bf_half.empty());
    Lerc2::HeaderInfo hd_half{};
    bool has_mask_half = false;
    ASSERT_TRUE(Lerc2::GetHeaderInfo(bf_half.data(), bf_half.size(), hd_half,
                                     has_mask_half));
    EXPECT_DOUBLE_EQ(0.5, hd_half.maxZError);
  }

  // 3. TryRaiseMaxZError on float/double rounded to %.2f -> raises 0.001 to
  // 0.005.
  {
    constexpr int kRows = 16;
    constexpr int kCols = 16;
    std::vector<float> rounded_f(kRows * kCols);
    std::vector<float> noisy_f(kRows * kCols);
    for (int i = 0; i < kRows * kCols; ++i) {
      rounded_f[i] =
          static_cast<float>(std::round((10.0 + i * 0.037) * 100.0) / 100.0);
      noisy_f[i] = 10.0f + static_cast<float>(i) * 0.000123456f;
    }

    Lerc2 enc1(1, kCols, kRows);
    const std::vector<Byte> b1 = EncodeBlob(enc1, rounded_f.data(), 0.001);
    ASSERT_FALSE(b1.empty());
    Lerc2::HeaderInfo hd1{};
    bool has_mask = false;
    ASSERT_TRUE(Lerc2::GetHeaderInfo(b1.data(), b1.size(), hd1, has_mask));
    EXPECT_DOUBLE_EQ(0.005, hd1.maxZError);

    // nDepth=2 + masked variant of TryRaiseMaxZError.
    BitMask mask(kCols, kRows);
    mask.SetAllValid();
    mask.SetInvalid(1, 1);
    std::vector<double> rounded_d2(kRows * kCols * 2);
    for (int i = 0; i < kRows * kCols; ++i) {
      const double v = std::round((5.0 + i * 0.073) * 100.0) / 100.0;
      rounded_d2[i * 2 + 0] = v;
      rounded_d2[i * 2 + 1] = v + 1.0;
    }
    Lerc2 enc2(2, kCols, kRows, mask.Bits());
    const std::vector<Byte> b2 = EncodeBlob(enc2, rounded_d2.data(), 0.001);
    ASSERT_FALSE(b2.empty());
    Lerc2::HeaderInfo hd2{};
    ASSERT_TRUE(Lerc2::GetHeaderInfo(b2.data(), b2.size(), hd2, has_mask));
    EXPECT_DOUBLE_EQ(0.005, hd2.maxZError);

    // High-precision data prunes all candidates and keeps original tolerance
    // (both unmasked special case and masked general case).
    const std::vector<Byte> b3 = EncodeBlob(enc1, noisy_f.data(), 0.0001);
    ASSERT_FALSE(b3.empty());
    Lerc2::HeaderInfo hd3{};
    ASSERT_TRUE(Lerc2::GetHeaderInfo(b3.data(), b3.size(), hd3, has_mask));
    EXPECT_DOUBLE_EQ(0.0001, hd3.maxZError);

    Lerc2 enc1_m(1, kCols, kRows, mask.Bits());
    const std::vector<Byte> b4 = EncodeBlob(enc1_m, noisy_f.data(), 0.0001);
    ASSERT_FALSE(b4.empty());
    ASSERT_TRUE(Lerc2::GetHeaderInfo(b4.data(), b4.size(), hd3, has_mask));
    EXPECT_DOUBLE_EQ(0.0001, hd3.maxZError);
  }
}

TEST(Lerc2Test, RejectsNullInputs) {
  Lerc2 lerc(1, 4, 4);
  std::vector<int> data(16, 1);
  data[0] = 5;
  EXPECT_EQ(0U, lerc.ComputeNumBytesNeededToWrite(
                    static_cast<const int*>(nullptr), 0.5, true));
  EXPECT_FALSE(lerc.Encode(static_cast<const int*>(nullptr), nullptr));
  EXPECT_FALSE(lerc.Encode(data.data(), nullptr));

  Lerc2::HeaderInfo hd{};
  bool has_mask = false;
  EXPECT_FALSE(Lerc2::GetHeaderInfo(nullptr, 100, hd, has_mask));
  EXPECT_FALSE(lerc.GetRanges(nullptr, 100, nullptr, nullptr));
  const Byte* null_ptr = nullptr;
  std::size_t zero_rem = 0;
  EXPECT_FALSE(lerc.Decode(&null_ptr, zero_rem, data.data()));

  // v3 Lerc2 instance rejects GetRanges due to its own object-version guard.
  // TODO(schwehr): Report upstream: Lerc2::GetRanges checks
  // m_headerInfo.version < 4 before ReadHeader() rather than on the parsed
  // blob header.
  Lerc2 enc_v3(1, 4, 4);
  ASSERT_TRUE(enc_v3.SetEncoderToOldVersion(3));
  const std::vector<Byte> v3_tiled = EncodeBlob(enc_v3, data.data(), 0.5);
  ASSERT_FALSE(v3_tiled.empty());
  std::array<double, 1> mins{777.0};
  std::array<double, 1> maxs{777.0};
  EXPECT_FALSE(enc_v3.GetRanges(v3_tiled.data(), v3_tiled.size(), mins.data(),
                                maxs.data()));
  EXPECT_DOUBLE_EQ(777.0, mins[0]);
}

TEST(Lerc2Test, DecodeRejectsEveryTruncatedPrefix) {
  Lerc2 lerc(1, 4, 4);
  std::vector<int> data(16, 1);
  data[0] = 5;
  const std::vector<Byte> v6_tiled = EncodeBlob(lerc, data.data(), 0.5);
  ASSERT_FALSE(v6_tiled.empty());

  std::vector<Byte> bytes(256, 3);
  bytes[0] = 1;
  bytes[7] = 9;
  Lerc2 enc_b(1, 16, 16);
  // Sparse outliers in 16x16 Byte image encode as IEM_Tiling.
  const std::vector<Byte> v6_byte_tiled = EncodeBlob(enc_b, bytes.data(), 0.5);
  ASSERT_FALSE(v6_byte_tiled.empty());

  std::vector<float> flts(256);
  for (int i = 0; i < 256; ++i) {
    flts[i] = static_cast<float>(i) * 0.5f;
  }
  Lerc2 enc_f(1, 16, 16);
  // MSan safety on v6_lfpc depends on upstream EncodeHuffman's heuristic and
  // stream termination for this test input.
  const std::vector<Byte> v6_lfpc = EncodeBlob(enc_f, flts.data(), 0.0);
  ASSERT_FALSE(v6_lfpc.empty());

  Lerc2 enc_v3(1, 4, 4);
  ASSERT_TRUE(enc_v3.SetEncoderToOldVersion(3));
  const std::vector<Byte> v3_tiled = EncodeBlob(enc_v3, data.data(), 0.5);
  ASSERT_FALSE(v3_tiled.empty());

  ExpectEveryPrefixFails<int>(v6_tiled, data.size());
  ExpectEveryPrefixFails<Byte>(v6_byte_tiled, bytes.size());
  ExpectEveryPrefixFails<float>(v6_lfpc, flts.size());
  ExpectEveryPrefixFails<int>(v3_tiled, data.size());

  // Corrupted byte fails Fletcher-32 checksum check in Decode.
  auto bad = v6_tiled;
  bad.back() ^= 0xFF;
  std::vector<int> out(data.size(), -99);
  EXPECT_FALSE(DecodeBlob(bad, out.data()));
}

TEST(Lerc2Test, GetHeaderInfoRejectsCorruptFields) {
  // Layout of v6 header:
  //    0..5 : "Lerc2 " (6B)
  //    6..9 : version (4B)
  //   10..13: checksum (4B)
  //   14..45: nRows, nCols, nDepth, numValidPixel, microBlockSize,
  //           blobSize, dt, nBlobsMore (8 ints)
  //   46..49: 4 flag bytes (bPassNoDataValues, bIsInt, bReserved3, bReserved4)
  //   50..89: 5 doubles (maxZError, zMin, zMax, noDataVal, noDataValOrig)
  //   90..93: mask numBytesMask (4B)
  Lerc2 lerc(1, 4, 4);
  std::vector<int> data(16, 1);
  data[0] = 5;
  const std::vector<Byte> v6_tiled = EncodeBlob(lerc, data.data(), 0.5);
  ASSERT_FALSE(v6_tiled.empty());

  Lerc2::HeaderInfo hd{};
  bool has_mask = false;
  std::vector<int> out(data.size(), -99);
  {
    auto bad = v6_tiled;
    bad[0] = 'X';
    EXPECT_FALSE(Lerc2::GetHeaderInfo(bad.data(), bad.size(), hd, has_mask));
  }
  {
    auto bad = v6_tiled;
    WritePodAt(bad, kLerc2VersionOffset, Lerc2::CurrentVersion() + 1);
    EXPECT_FALSE(Lerc2::GetHeaderInfo(bad.data(), bad.size(), hd, has_mask));
  }
  {
    auto bad = v6_tiled;
    WritePodAt(bad, kLerc2NRowsOffset, 0);
    EXPECT_FALSE(Lerc2::GetHeaderInfo(bad.data(), bad.size(), hd, has_mask));
  }
  {
    auto bad = v6_tiled;
    WritePodAt(bad, kLerc2MicroBlockSizeOffset, 33);  // > 32
    EXPECT_FALSE(Lerc2::GetHeaderInfo(bad.data(), bad.size(), hd, has_mask));
  }
  {
    auto bad = v6_tiled;
    WritePodAt(bad, kLerc2DataTypeOffset,
               static_cast<int>(Lerc2::DT_Undefined));
    EXPECT_FALSE(Lerc2::GetHeaderInfo(bad.data(), bad.size(), hd, has_mask));
  }
  {
    auto bad = v6_tiled;
    WritePodAt(bad, kLerc2NRowsOffset, 50000);
    WritePodAt(bad, kLerc2NColsOffset, 50000);  // numPixel > INT_MAX
    EXPECT_FALSE(Lerc2::GetHeaderInfo(bad.data(), bad.size(), hd, has_mask));
  }
  {
    auto bad = v6_tiled;
    WritePodAt(bad, kLerc2BlobSizeOffset, 10);  // blobSize < header size
    ASSERT_TRUE(RecomputeChecksum(bad));
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }
  {
    auto bad = v6_tiled;
    WritePodAt(bad, kLerc2V6NumBytesMaskOffset, -1);  // numBytesMask < 0
    EXPECT_FALSE(Lerc2::GetHeaderInfo(bad.data(), bad.size(), hd, has_mask));
    ASSERT_TRUE(RecomputeChecksum(bad));
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }
  {
    auto bad = v6_tiled;
    // numValid == w*h yet numBytesMask != 0.
    WritePodAt(bad, kLerc2V6NumBytesMaskOffset, 2);
    ASSERT_TRUE(RecomputeChecksum(bad));
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }
}

TEST(Lerc2Test, DecodeRejectsCorruptModeAndTileFlags) {
  Lerc2 lerc(1, 4, 4);
  std::vector<int> data(16, 1);
  data[0] = 5;
  const std::vector<Byte> v6_tiled = EncodeBlob(lerc, data.data(), 0.5);
  ASSERT_FALSE(v6_tiled.empty());

  std::vector<Byte> bytes(256, 3);
  bytes[0] = 1;
  bytes[7] = 9;
  Lerc2 enc_b(1, 16, 16);
  const std::vector<Byte> v6_byte = EncodeBlob(enc_b, bytes.data(), 0.5);
  ASSERT_FALSE(v6_byte.empty());

  std::vector<Byte> ramp(256);
  for (int i = 0; i < 256; ++i) {
    ramp[i] = static_cast<Byte>(i % 16);
  }
  Lerc2 enc_v3_b(1, 16, 16);
  ASSERT_TRUE(enc_v3_b.SetEncoderToOldVersion(3));
  const std::vector<Byte> v3_ramp = EncodeBlob(enc_v3_b, ramp.data(), 0.5);
  ASSERT_FALSE(v3_ramp.empty());

  std::vector<float> flts(256);
  for (int i = 0; i < 256; ++i) {
    flts[i] = static_cast<float>(i) * 0.5f;
  }
  Lerc2 enc_f(1, 16, 16);
  const std::vector<Byte> v6_lfpc = EncodeBlob(enc_f, flts.data(), 0.0);
  ASSERT_FALSE(v6_lfpc.empty());

  constexpr std::size_t kV6ByteSweepOffset =
      kLerc2V6HeaderBytes + sizeof(int) + 2U * sizeof(Byte);
  constexpr std::size_t kV6ByteModeOffset = kV6ByteSweepOffset + 1U;
  constexpr std::size_t kV3ByteSweepOffset = kLerc2V3HeaderBytes + sizeof(int);
  constexpr std::size_t kV3ByteModeOffset = kV3ByteSweepOffset + 1U;
  constexpr std::size_t kV6FltSweepOffset =
      kLerc2V6HeaderBytes + sizeof(int) + 2U * sizeof(float);
  constexpr std::size_t kV6FltModeOffset = kV6FltSweepOffset + 1U;
  constexpr std::size_t kV6IntSweepOffset =
      kLerc2V6HeaderBytes + sizeof(int) + 2U * sizeof(int);
  constexpr std::size_t kV6IntFirstTileFlagOffset = kV6IntSweepOffset + 1U;

  // Pin encoder heuristic layout preconditions before mutating offsets.
  ASSERT_EQ(0, v6_byte[kV6ByteSweepOffset]);
  ASSERT_EQ(0, v3_ramp[kV3ByteSweepOffset]);
  ASSERT_EQ(0, v6_lfpc[kV6FltSweepOffset]);
  ASSERT_EQ(3, v6_lfpc[kV6FltModeOffset]);
  ASSERT_EQ(0, v6_tiled[kV6IntSweepOffset]);

  // Corrupt image encode mode bytes on Byte (v6 and v3) and lossless float.
  for (const Byte bad_mode : {Byte{3}, Byte{4}}) {
    auto bad = v6_byte;
    bad[kV6ByteModeOffset] = bad_mode;
    ASSERT_TRUE(RecomputeChecksum(bad));
    std::vector<Byte> out(bytes.size(), 99);
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }
  {
    auto bad = v3_ramp;
    bad[kV3ByteModeOffset] = 2;  // IEM_Huffman invalid for version < 4
    ASSERT_TRUE(RecomputeChecksum(bad));
    std::vector<Byte> out(ramp.size(), 99);
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }
  {
    auto bad = v6_lfpc;
    bad[kV6FltModeOffset] = 2;  // IEM_Huffman invalid for TryHuffmanFlt
    ASSERT_TRUE(RecomputeChecksum(bad));
    std::vector<float> out(flts.size(), -1.0f);
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }

  // Corrupt tile comprFlag integrity bits (bits 2..5) & impossible bDiffEnc
  // on iDepth == 0.
  {
    auto bad = v6_tiled;
    // Trigger ((comprFlag >> 2) & pattern) != ((j0 >> 3) & pattern).
    bad[kV6IntFirstTileFlagOffset] |= static_cast<Byte>(1U << 3);
    ASSERT_TRUE(RecomputeChecksum(bad));
    std::vector<int> out(data.size(), -99);
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }
  {
    auto bad = v6_tiled;
    // Force bDiffEnc = true when iDepth == 0.
    bad[kV6IntFirstTileFlagOffset] |= static_cast<Byte>(1U << 2);
    ASSERT_TRUE(RecomputeChecksum(bad));
    std::vector<int> out(data.size(), -99);
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }

  // Corrupt RLE mask in a partial-mask blob (covers ReadMask RLE failure).
  {
    BitMask mask(8, 8);
    mask.SetAllValid();
    mask.SetInvalid(0, 0);
    std::vector<int> mvals(64, 1);
    mvals[1] = 2;
    Lerc2 menc(1, 8, 8, mask.Bits());
    auto bad = EncodeBlob(menc, mvals.data(), 0.5);
    ASSERT_FALSE(bad.empty());
    const int mask_len = ReadPod<int>(bad, kLerc2V6NumBytesMaskOffset);
    ASSERT_GT(mask_len, 2);
    bad[kLerc2V6NumBytesMaskOffset + 4U] = 0x7F;
    bad[kLerc2V6NumBytesMaskOffset + 5U] = 0x7F;
    ASSERT_TRUE(RecomputeChecksum(bad));
    Lerc2 dec;
    std::array<double, 1> mins{777.0};
    std::array<double, 1> maxs{777.0};
    EXPECT_FALSE(
        dec.GetRanges(bad.data(), bad.size(), mins.data(), maxs.data()));
    std::vector<int> out(mvals.size(), -99);
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }

  // DT_UShort tile with bits67 == 3 triggers GetDataTypeUsed returning
  // DT_Undefined inside Lerc2::ReadTile; nDepth=2 tile with bDiffEnc &&
  // comprFlag == 0 (BEM_RawBinary) is also rejected by Lerc2::ReadTile.
  {
    std::vector<std::uint16_t> us(64, 500);
    us[1] = 505;
    Lerc2 enc_us(1, 8, 8);
    auto bad = EncodeBlob(enc_us, us.data(), 0.5);
    ASSERT_FALSE(bad.empty());
    constexpr std::size_t kUShortTileOffset =
        kLerc2V6HeaderBytes + sizeof(int) + 2U * sizeof(std::uint16_t) + 1U;
    bad[kUShortTileOffset] = static_cast<Byte>(
        (bad[kUShortTileOffset] & 0x3FU) | kBitStufferInvalidCount);
    ASSERT_TRUE(RecomputeChecksum(bad));
    std::vector<std::uint16_t> out(us.size(), 999U);
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }
  {
    std::vector<int> d2(64 * 2, 0);
    for (int k = 0; k < 64; ++k) {
      d2[k * 2 + 1] = 100 + k;
    }
    Lerc2 enc_d2(2, 8, 8);
    auto bad = EncodeBlob(enc_d2, d2.data(), 0.5);
    ASSERT_FALSE(bad.empty());
    // Header (90) + mask (4) + min/max (4 * int = 16) + sweep (1) +
    // const-0 tile iDepth=0 (1) = 112.
    constexpr std::size_t kDepth1TileOffset =
        kLerc2V6HeaderBytes + sizeof(int) + 4U * sizeof(int) + 2U;
    // bDiffEnc=1, integrity bits 0, comprFlag=0 (BEM_RawBinary).
    bad[kDepth1TileOffset] = 0x04;
    ASSERT_TRUE(RecomputeChecksum(bad));
    std::vector<int> out(d2.size(), -99);
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }
}

TEST(Lerc2Test, PatchedTruncationsHitDownstreamGuards) {
  // Patched-blobSize + Fletcher-32 truncation sweeps pass ReadHeader/checksum
  // so Decode and GetRanges exercise downstream bounds guards:
  // ReadMask, ReadMinMaxRanges, flag bytes, ReadTile, DecodeHuffman, OneSweep.
  BitMask mask(8, 8);
  mask.SetAllValid();
  mask.SetInvalid(0, 0);
  std::vector<int> mvals(64, 1);
  mvals[1] = 9;
  Lerc2 menc(1, 8, 8, mask.Bits());
  const std::vector<Byte> masked_tiled = EncodeBlob(menc, mvals.data(), 0.5);
  ASSERT_FALSE(masked_tiled.empty());

  std::vector<Byte> huff_bytes(24 * 24 * 3, 7);
  for (int i = 0; i < 24 * 24 * 3; i += 29) {
    huff_bytes[i] = static_cast<Byte>((i * 53) % 250);
  }
  Lerc2 enc_h_orig(3, 24, 24);
  const std::vector<Byte> v6_huff_orig =
      EncodeBlob(enc_h_orig, huff_bytes.data(), 0.5);
  ASSERT_FALSE(v6_huff_orig.empty());
  // Header (90) + numBytesMask (4) + minMax (2*3 Byte = 6) + sweep (1) = 101.
  ASSERT_EQ(2, v6_huff_orig[101]);  // IEM_Huffman

  BitMask mask24(24, 24);
  mask24.SetAllValid();
  mask24.SetInvalid(0, 1);
  Lerc2 enc_h_masked(3, 24, 24, mask24.Bits());
  const std::vector<Byte> v6_huff_orig_m =
      EncodeBlob(enc_h_masked, huff_bytes.data(), 0.5);
  ASSERT_FALSE(v6_huff_orig_m.empty());
  const int mask24_len =
      ReadPod<int>(v6_huff_orig_m, kLerc2V6NumBytesMaskOffset);
  ASSERT_EQ(2, v6_huff_orig_m[kLerc2V6HeaderBytes + 4 + mask24_len + 6 + 1]);

  BitMask mask16(16, 16);
  mask16.SetAllValid();
  mask16.SetInvalid(0, 1);
  std::vector<signed char> ramp16(256);
  for (int i = 0; i < 256; ++i) {
    ramp16[i] = static_cast<signed char>((i % 16) * 2);
  }
  Lerc2 enc_h_delta(1, 16, 16);
  const std::vector<Byte> v6_huff_delta =
      EncodeBlob(enc_h_delta, ramp16.data(), 0.5);
  ASSERT_FALSE(v6_huff_delta.empty());
  ASSERT_EQ(1, v6_huff_delta[97]);  // IEM_DeltaHuffman
  Lerc2 enc_h_delta_m(1, 16, 16, mask16.Bits());
  const std::vector<Byte> v6_huff_delta_m =
      EncodeBlob(enc_h_delta_m, ramp16.data(), 0.5);
  ASSERT_FALSE(v6_huff_delta_m.empty());

  // Byte BEM_RawBinary blob (left const 0 tile, right 2-value raw tile).
  BitMask mask_raw(16, 8);
  mask_raw.SetAllInvalid();
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 8; ++j) {
      mask_raw.SetValid(i, j);
    }
  }
  mask_raw.SetValid(0, 8);
  mask_raw.SetValid(0, 9);
  std::vector<Byte> raw_b(128, 0);
  raw_b[8] = 17;
  raw_b[9] = 240;
  Lerc2 enc_raw(1, 16, 8, mask_raw.Bits());
  const std::vector<Byte> v6_raw_byte = EncodeBlob(enc_raw, raw_b.data(), 0.5);
  ASSERT_FALSE(v6_raw_byte.empty());

  // v5 high-entropy OneSweep blob.
  const std::vector<double> entropy =
      MakeHighEntropyDoubles(64, 0x123456789ABCDEF0ULL);
  Lerc2 enc_sw(1, 8, 8);
  ASSERT_TRUE(enc_sw.SetEncoderToOldVersion(5));
  const std::vector<Byte> v5_sweep = EncodeBlob(enc_sw, entropy.data(), 0.0);
  ASSERT_FALSE(v5_sweep.empty());

  const int masked_len = ReadPod<int>(masked_tiled, kLerc2V6NumBytesMaskOffset);
  const std::size_t ranges_end = kLerc2V6HeaderBytes + sizeof(int) +
                                 static_cast<std::size_t>(masked_len) +
                                 2U * sizeof(int);
  for (std::size_t k = kLerc2V6HeaderBytes; k < masked_tiled.size(); ++k) {
    const auto trunc = PatchedTruncateBlob(masked_tiled, k);
    Lerc2 dec;
    std::array<double, 1> mins{777.0};
    std::array<double, 1> maxs{777.0};
    const bool expect_ranges_ok = (k >= ranges_end);
    EXPECT_EQ(expect_ranges_ok, dec.GetRanges(trunc.data(), trunc.size(),
                                              mins.data(), maxs.data()));
    if (expect_ranges_ok) {
      EXPECT_DOUBLE_EQ(1.0, mins[0]);
      EXPECT_DOUBLE_EQ(9.0, maxs[0]);
    }
    std::vector<int> out(64, -99);
    EXPECT_FALSE(DecodeBlob(trunc, out.data()));
  }

  ExpectEveryPrefixFails<Byte>(v6_huff_orig, huff_bytes.size(),
                               kLerc2V6HeaderBytes, /*patch_checksum=*/true);
  ExpectEveryPrefixFails<Byte>(v6_huff_orig_m, huff_bytes.size(),
                               kLerc2V6HeaderBytes, /*patch_checksum=*/true);
  ExpectEveryPrefixFails<signed char>(v6_huff_delta, ramp16.size(),
                                      kLerc2V6HeaderBytes,
                                      /*patch_checksum=*/true);
  ExpectEveryPrefixFails<signed char>(v6_huff_delta_m, ramp16.size(),
                                      kLerc2V6HeaderBytes,
                                      /*patch_checksum=*/true);
  ExpectEveryPrefixFails<Byte>(v6_raw_byte, raw_b.size(), kLerc2V6HeaderBytes,
                               /*patch_checksum=*/true);

  {
    // Truncate 1 byte inside v5_sweep ReadDataOneSweep payload.
    const auto trunc = PatchedTruncateBlob(v5_sweep, v5_sweep.size() - 1);
    std::vector<double> out(64, -999.0);
    EXPECT_FALSE(DecodeBlob(trunc, out.data()));
  }

  // Corrupt Huffman code table so BuildTreeFromCodes fails inside
  // Lerc2::DecodeHuffman: set i0=0, i1=1, and BitStuffer2 code-lengths entry
  // with numBits=0, numElem=1.
  {
    auto bad = v6_huff_orig;
    WritePodAt(bad, 110, 0);  // i0 = 0
    WritePodAt(bad, 114, 1);  // i1 = 1
    bad[118] = kBitStufferOneByteCount;
    bad[119] = 1;  // numElem = 1 (zero code length -> GetRange fails)
    ASSERT_TRUE(RecomputeChecksum(bad));
    std::vector<Byte> out(huff_bytes.size(), 99);
    EXPECT_FALSE(DecodeBlob(bad, out.data()));
  }
}

}  // namespace
}  // namespace lerc
