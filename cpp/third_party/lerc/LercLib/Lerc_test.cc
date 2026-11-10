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
// Unit tests for LercNS::Lerc (third_party/lerc/LercLib/Lerc.cpp).
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc.cpp
//   https://github.com/Esri/lerc/blob/master/src/LercLib/include/Lerc_types.h

#include "third_party/lerc/LercLib/Lerc.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/lerc/LercLib/BitMask.h"
#include "third_party/lerc/LercLib/Defines.h"
#include "third_party/lerc/LercLib/Lerc1Decode/Image.h"
#include "third_party/lerc/LercLib/Lerc2.h"
#include "third_party/lerc/LercLib/RLE.h"
#include "third_party/lerc/LercLib/include/Lerc_types.h"
#include "third_party/lerc/google/util/lerc_test_util.h"

#ifndef HAVE_LERC1_DECODE
#error "Lerc_test.cc expects HAVE_LERC1_DECODE to be defined in Defines.h."
#endif

namespace lerc {
namespace {

using ::LercNS::BitMask;
using ::LercNS::Byte;
using ::LercNS::ErrCode;
using ::LercNS::Image;
using ::LercNS::Lerc;
using ::LercNS::Lerc2;
using ::LercNS::RLE;

constexpr Byte kValidFlag = 1;
constexpr std::size_t kLerc1Band1Bytes =
    kLerc1HeaderBytes + kLerc1PartHeaderBytes + 1U + 3U * sizeof(float);
static_assert(kLerc1Band1Bytes == 63U);

template <typename T>
Lerc::DataType LercDataTypeFor() {
  if constexpr (std::is_same_v<T, signed char>) {
    return Lerc::DT_Char;
  } else if constexpr (std::is_same_v<T, Byte>) {
    return Lerc::DT_Byte;
  } else if constexpr (std::is_same_v<T, std::int16_t>) {
    return Lerc::DT_Short;
  } else if constexpr (std::is_same_v<T, std::uint16_t>) {
    return Lerc::DT_UShort;
  } else if constexpr (std::is_same_v<T, int>) {
    return Lerc::DT_Int;
  } else if constexpr (std::is_same_v<T, unsigned int>) {
    return Lerc::DT_UInt;
  } else if constexpr (std::is_same_v<T, float>) {
    return Lerc::DT_Float;
  } else {
    static_assert(std::is_same_v<T, double>);
    return Lerc::DT_Double;
  }
}

template <typename T>
std::vector<Byte> EncodeAll(const std::vector<T>& src, int version, int n_depth,
                            int n_cols, int n_rows, int n_bands, int n_masks,
                            const Byte* valid_bytes, double max_z_err,
                            const unsigned char* uses_no_data = nullptr,
                            const double* no_data_vals = nullptr) {
  SCOPED_TRACE(version);
  const Lerc::DataType dt = LercDataTypeFor<T>();
  unsigned int needed = 0;
  const ErrCode size_err = Lerc::ComputeCompressedSize(
      src.data(), version, dt, n_depth, n_cols, n_rows, n_bands, n_masks,
      valid_bytes, max_z_err, needed, uses_no_data, no_data_vals);
  if (size_err != ErrCode::Ok || needed == 0U) {
    ADD_FAILURE() << "ComputeCompressedSize failed: err="
                  << static_cast<int>(size_err) << ", needed=" << needed;
    return {};
  }

  // LERC2 v2.2 BitStuff_Before_Lerc2v3 memset-zeroes a full numUInts * 4 region
  // that can extend up to 3 padding bytes beyond ComputeCompressedSize().
  const unsigned int alloc_size = needed + (version == 2 ? 4U : 0U);
  std::vector<Byte> blob(alloc_size, 0);
  unsigned int written = 0;
  const ErrCode enc_err =
      Lerc::Encode(src.data(), version, dt, n_depth, n_cols, n_rows, n_bands,
                   n_masks, valid_bytes, max_z_err, blob.data(), alloc_size,
                   written, uses_no_data, no_data_vals);
  if (enc_err != ErrCode::Ok) {
    ADD_FAILURE() << "Encode failed: err=" << static_cast<int>(enc_err);
    return {};
  }
  EXPECT_EQ(needed, written);
  blob.resize(written);
  return blob;
}

template <typename T>
void ExpectScalarRoundTripAndConvert(int version) {
  SCOPED_TRACE(version);
  SCOPED_TRACE(static_cast<int>(LercDataTypeFor<T>()));
  constexpr int kDepth = 2;
  constexpr int kCols = 6;
  constexpr int kRows = 5;
  constexpr int kBands = 2;
  const std::size_t n_pix = static_cast<std::size_t>(kCols) * kRows;
  std::vector<T> src(n_pix * kDepth * kBands);
  for (std::size_t b = 0; b < kBands; ++b) {
    for (std::size_t k = 0; k < n_pix; ++k) {
      for (int m = 0; m < kDepth; ++m) {
        const std::size_t idx = (b * n_pix + k) * kDepth + m;
        if constexpr (std::is_signed_v<T>) {
          src[idx] = static_cast<T>(static_cast<int>(k) - 10 +
                                    static_cast<int>(b) * 5 + m * 2);
        } else {
          src[idx] = static_cast<T>(k + b * 7 + m * 3 + 1);
        }
      }
    }
  }

  // Exercise shared mask (nMasks == 1) across 2 bands (valid k in [1..28]).
  std::vector<Byte> mask(n_pix, 1);
  mask[0] = 0;
  mask[n_pix - 1] = 0;

  // MSan safety for v6 lossless float/double depends on upstream
  // EncodeHuffman's heuristic and stream termination for this test data.
  const std::vector<Byte> blob = EncodeAll(src, version, kDepth, kCols, kRows,
                                           kBands, /*n_masks=*/1, mask.data(),
                                           /*max_z_err=*/0.0);
  ASSERT_FALSE(blob.empty());

  Lerc::LercInfo info{};
  info.version = -99;
  info.blobSize = 0xCAFEBABEU;
  std::array<double, kDepth * kBands> mins{};
  mins.fill(777.0);
  std::array<double, kDepth * kBands> maxs{};
  maxs.fill(777.0);
  ASSERT_EQ(
      ErrCode::Ok,
      Lerc::GetLercInfo(blob.data(), static_cast<unsigned int>(blob.size()),
                        info, mins.data(), maxs.data(), mins.size()));
  EXPECT_EQ(version < 0 ? Lerc2::CurrentVersion() : version, info.version);
  EXPECT_EQ(LercDataTypeFor<T>(), info.dt);
  EXPECT_EQ(kDepth, info.nDepth);
  EXPECT_EQ(kCols, info.nCols);
  EXPECT_EQ(kRows, info.nRows);
  EXPECT_EQ(kBands, info.nBands);
  EXPECT_EQ(static_cast<int>(n_pix) - 2, info.numValidPixel);
  EXPECT_EQ(1, info.nMasks);
  EXPECT_EQ(blob.size(), info.blobSize);
  for (int b = 0; b < kBands; ++b) {
    for (int m = 0; m < kDepth; ++m) {
      const std::size_t idx = static_cast<std::size_t>(b * kDepth + m);
      if constexpr (std::is_signed_v<T>) {
        EXPECT_DOUBLE_EQ(-9.0 + 5.0 * b + 2.0 * m, mins[idx]);
        EXPECT_DOUBLE_EQ(18.0 + 5.0 * b + 2.0 * m, maxs[idx]);
      } else {
        EXPECT_DOUBLE_EQ(2.0 + 7.0 * b + 3.0 * m, mins[idx]);
        EXPECT_DOUBLE_EQ(29.0 + 7.0 * b + 3.0 * m, maxs[idx]);
      }
    }
  }

  auto verify_valid_pixels = [&](const auto& decoded, auto check_eq) {
    for (std::size_t b = 0; b < kBands; ++b) {
      for (std::size_t k = 0; k < n_pix; ++k) {
        if (mask[k] != 0) {
          for (int m = 0; m < kDepth; ++m) {
            const std::size_t idx = (b * n_pix + k) * kDepth + m;
            check_eq(src[idx], decoded[idx]);
          }
        }
      }
    }
  };

  std::vector<T> dst(src.size(), static_cast<T>(77));
  std::vector<Byte> out_mask(n_pix, 0xAA);
  ASSERT_EQ(
      ErrCode::Ok,
      Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                   /*n_masks=*/1, out_mask.data(), kDepth, kCols, kRows, kBands,
                   LercDataTypeFor<T>(), dst.data(), nullptr, nullptr));
  EXPECT_EQ(mask, out_mask);
  verify_valid_pixels(dst, [](const T& expected, const T& actual) {
    EXPECT_EQ(expected, actual);
  });

  std::vector<double> as_double(src.size(), -999.0);
  const ErrCode conv_err = Lerc::ConvertToDouble(
      dst.data(), LercDataTypeFor<T>(), dst.size(), as_double.data());
  if constexpr (std::is_same_v<T, double>) {
    EXPECT_EQ(ErrCode::WrongParam, conv_err);
  } else {
    EXPECT_EQ(ErrCode::Ok, conv_err);
    verify_valid_pixels(as_double, [](const T& expected, double actual) {
      EXPECT_DOUBLE_EQ(static_cast<double>(expected), actual);
    });
  }
}

std::vector<Byte> BuildSynthesizedTwoBandLerc1Blob() {
  // 2x2 masked image (3 valid pixels, 1 invalid) so GetLercInfo collects
  // per-band min/max, nMasks = 1, and Band 1 triggers onlyZPart = true (>51B).
  BitMask mask(2, 2);
  mask.SetAllValid();
  mask.SetInvalid(1, 1);

  RLE rle;
  Byte* rle_raw = nullptr;
  std::size_t rle_len = 0;
  const bool compressed =
      rle.compress(mask.Bits(), mask.Size(), &rle_raw, rle_len, false);
  const std::unique_ptr<Byte[]> rle_owner(rle_raw);
  EXPECT_TRUE(compressed);
  if (!compressed) {
    return {};
  }

  std::vector<Byte> blob;
  AppendLerc1Header(blob, 11, Image::CNT_Z, /*height=*/2, /*width=*/2,
                    /*max_z_error=*/0.0);
  AppendLerc1PartHeader(blob, 0, 0, static_cast<int>(rle_len), 1.0f);
  blob.insert(blob.end(), rle_owner.get(), rle_owner.get() + rle_len);

  // Band 0 Z part: 1x1 raw float tile (1 byte comprFlag=0 + 3 floats = 13B).
  AppendLerc1PartHeader(blob, 1, 1, 13, 30.0f);
  blob.push_back(0);
  for (const float z : {10.2f, 20.7f, 30.0f}) {
    AppendPod(blob, z);
  }

  // Band 1: 34B header + 16B Z part header + 13B Z tile = 63B (> 51B).
  AppendLerc1Header(blob, 11, Image::CNT_Z, /*height=*/2, /*width=*/2,
                    /*max_z_error=*/0.0);
  AppendLerc1PartHeader(blob, 1, 1, 13, 25.0f);
  blob.push_back(0);
  for (const float z : {5.0f, 15.0f, 25.0f}) {
    AppendPod(blob, z);
  }
  return blob;
}

TEST(LercTest, RoundTripsAllEightScalarTypesAcrossVersionsAndConvertToDouble) {
  for (const int version : {5, -1}) {
    ExpectScalarRoundTripAndConvert<signed char>(version);
    ExpectScalarRoundTripAndConvert<Byte>(version);
    ExpectScalarRoundTripAndConvert<std::int16_t>(version);
    ExpectScalarRoundTripAndConvert<std::uint16_t>(version);
    ExpectScalarRoundTripAndConvert<int>(version);
    ExpectScalarRoundTripAndConvert<unsigned int>(version);
    ExpectScalarRoundTripAndConvert<float>(version);
    ExpectScalarRoundTripAndConvert<double>(version);
  }

  // Versions 2..4 also round-trip single-depth data, and all-valid decoding
  // fills caller pValidBytes with 1s when requested.
  std::vector<int> src(24);
  for (int i = 0; i < 24; ++i) {
    src[i] = i * 3 - 20;
  }
  for (const int version : {2, 3, 4}) {
    const std::vector<Byte> blob =
        EncodeAll(src, version, /*n_depth=*/1, /*n_cols=*/6, /*n_rows=*/4,
                  /*n_bands=*/1, /*n_masks=*/0, nullptr, /*max_z_err=*/0.5);
    ASSERT_FALSE(blob.empty());
    std::vector<int> dst(24, 999);
    std::vector<Byte> out_mask(24, 9);
    EXPECT_EQ(ErrCode::Ok,
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           /*n_masks=*/1, out_mask.data(), 1, 6, 4, 1,
                           Lerc::DT_Int, dst.data(), nullptr, nullptr));
    EXPECT_EQ(src, dst);
    EXPECT_EQ(std::vector<Byte>(24, 1), out_mask);
  }

  // ConvertToDouble invalid argument guards.
  double d_out = 0.0;
  int i_in = 3;
  EXPECT_EQ(ErrCode::WrongParam,  // pData null
            Lerc::ConvertToDouble(nullptr, Lerc::DT_Int, 1, &d_out));
  EXPECT_EQ(ErrCode::WrongParam,  // nDim == 0
            Lerc::ConvertToDouble(&i_in, Lerc::DT_Int, 0, &d_out));
  EXPECT_EQ(ErrCode::WrongParam,  // pOutData null
            Lerc::ConvertToDouble(&i_in, Lerc::DT_Int, 1, nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // dt invalid
            Lerc::ConvertToDouble(&i_in, Lerc::DT_Undefined, 1, &d_out));
}

TEST(LercTest, ParameterDimensionsBufferSizeAndMaskDifferenceGuards) {
  std::vector<int> data(32, 4);
  data[1] = 10;
  std::vector<double> d_data(16, 1.0);
  std::vector<Byte> mask1(16, 1);
  mask1[0] = 0;
  unsigned int needed = 0;

  // Invalid DataType, pointers, dimensions, masks, maxZErr, and versions.
  EXPECT_EQ(ErrCode::WrongParam,  // dt invalid
            Lerc::ComputeCompressedSize(data.data(), -1, Lerc::DT_Undefined, 1,
                                        4, 4, 1, 0, nullptr, 0.5, needed,
                                        nullptr, nullptr));
  unsigned int written = 0;
  std::vector<Byte> dummy_buf(64);
  EXPECT_EQ(
      ErrCode::WrongParam,  // dt invalid
      Lerc::Encode(data.data(), -1, Lerc::DT_Undefined, 1, 4, 4, 1, 0, nullptr,
                   0.5, dummy_buf.data(), 64, written, nullptr, nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // dt invalid
            Lerc::Decode(dummy_buf.data(), 64, 0, nullptr, 1, 4, 4, 1,
                         Lerc::DT_Undefined, data.data(), nullptr, nullptr));
  EXPECT_EQ(
      ErrCode::WrongParam,  // pData null
      Lerc::ComputeCompressedSizeTempl<int>(nullptr, -1, 1, 4, 4, 1, 0, nullptr,
                                            0.5, needed, nullptr, nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // nDepth <= 0
            Lerc::ComputeCompressedSizeTempl<int>(data.data(), -1, 0, 4, 4, 1,
                                                  0, nullptr, 0.5, needed,
                                                  nullptr, nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // maxZErr < 0
            Lerc::ComputeCompressedSizeTempl<int>(data.data(), -1, 1, 4, 4, 1,
                                                  0, nullptr, -1.0, needed,
                                                  nullptr, nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // pValidBytes null with nMasks > 0
            Lerc::ComputeCompressedSizeTempl<int>(data.data(), -1, 1, 4, 4, 2,
                                                  1, nullptr, 0.5, needed,
                                                  nullptr, nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // nMasks not in {0, 1, nBands}
            Lerc::ComputeCompressedSizeTempl<int>(data.data(), -1, 1, 4, 4, 3,
                                                  2, mask1.data(), 0.5, needed,
                                                  nullptr, nullptr));
  EXPECT_EQ(
      ErrCode::WrongParam,  // pData null
      Lerc::EncodeTempl<int>(nullptr, -1, 1, 4, 4, 1, 0, nullptr, 0.5,
                             dummy_buf.data(), 64, written, nullptr, nullptr));
  EXPECT_EQ(
      ErrCode::WrongParam,  // pValidBytes null with nMasks > 0
      Lerc::EncodeTempl<int>(data.data(), -1, 1, 4, 4, 2, 1, nullptr, 0.5,
                             dummy_buf.data(), 64, written, nullptr, nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // pLercBlob null
            Lerc::DecodeTempl<int>(nullptr, dummy_buf.data(), 64, 1, 4, 4, 1, 0,
                                   nullptr, nullptr, nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // pValidBytes null with nMasks > 0
            Lerc::DecodeTempl<int>(data.data(), dummy_buf.data(), 64, 1, 4, 4,
                                   2, 1, nullptr, nullptr, nullptr));

  // Invalid encoder version & v5 rejecting pUsesNoData, v6 rejecting null
  // noDataValues.
  const std::array<unsigned char, 2> use_nd = {1, 0};
  EXPECT_EQ(ErrCode::WrongParam,  // version < 2
            Lerc::ComputeCompressedSizeTempl<int>(data.data(), 1, 1, 4, 4, 1, 0,
                                                  nullptr, 0.5, needed, nullptr,
                                                  nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // version > CurrentVersion()
            Lerc::ComputeCompressedSizeTempl<int>(data.data(), 7, 1, 4, 4, 1, 0,
                                                  nullptr, 0.5, needed, nullptr,
                                                  nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // pUsesNoData rejected for v5
            Lerc::ComputeCompressedSizeTempl<int>(data.data(), 5, 1, 4, 4, 2, 0,
                                                  nullptr, 0.5, needed,
                                                  use_nd.data(), nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // pUsesNoData rejected for v5
            Lerc::EncodeTempl<int>(data.data(), 5, 1, 4, 4, 2, 0, nullptr, 0.5,
                                   dummy_buf.data(), 64, written, use_nd.data(),
                                   nullptr));
  EXPECT_EQ(ErrCode::WrongParam,  // noDataValues null while use_nd[0] == 1
            Lerc::ComputeCompressedSizeTempl<int>(data.data(), -1, 1, 4, 4, 2,
                                                  0, nullptr, 0.5, needed,
                                                  use_nd.data(), nullptr));

  // CheckDimensions overflow guard across ComputeCompressedSize, Encode,
  // Decode.
  EXPECT_EQ(ErrCode::DimensionsTooLarge,
            Lerc::ComputeCompressedSizeTempl<double>(
                d_data.data(), -1, 1000, 1000, 1000, 1, 0, nullptr, 0.0, needed,
                nullptr, nullptr));
  EXPECT_EQ(ErrCode::DimensionsTooLarge,
            Lerc::EncodeTempl<double>(d_data.data(), -1, 1, 50000, 50000, 1, 0,
                                      nullptr, 0.0, dummy_buf.data(), 64,
                                      written, nullptr, nullptr));
  EXPECT_EQ(
      ErrCode::DimensionsTooLarge,
      Lerc::DecodeTempl<double>(d_data.data(), dummy_buf.data(), 64, 1, 50000,
                                50000, 1, 0, nullptr, nullptr, nullptr));

  // BufferTooSmall in both EncodeInternal_v5 and EncodeInternal.
  for (const int version : {5, -1}) {
    ASSERT_EQ(ErrCode::Ok, Lerc::ComputeCompressedSizeTempl<int>(
                               data.data(), version, 1, 4, 4, 1, 0, nullptr,
                               0.5, needed, nullptr, nullptr));
    std::vector<Byte> small_buf(needed - 1, 0);
    EXPECT_EQ(ErrCode::BufferTooSmall,
              Lerc::EncodeTempl<int>(data.data(), version, 1, 4, 4, 1, 0,
                                     nullptr, 0.5, small_buf.data(), needed - 1,
                                     written, nullptr, nullptr));
  }

  // Multi-band differing masks (nMasks == nBands) across v5 and v6.
  std::vector<Byte> two_masks(32, 1);
  two_masks[0] = 0;   // band 0 invalid at (0,0)
  two_masks[17] = 0;  // band 1 invalid at (0,1)
  for (const int version : {5, -1}) {
    const std::vector<Byte> blob = EncodeAll(
        data, version, /*n_depth=*/1, /*n_cols=*/4, /*n_rows=*/4, /*n_bands=*/2,
        /*n_masks=*/2, two_masks.data(), /*max_z_err=*/0.5);
    ASSERT_FALSE(blob.empty());

    Lerc::LercInfo info{};
    ASSERT_EQ(ErrCode::Ok,
              Lerc::GetLercInfo(blob.data(),
                                static_cast<unsigned int>(blob.size()), info));
    EXPECT_EQ(2, info.nMasks);
    EXPECT_EQ(2, info.nBands);

    // GetLercInfo with nElem too small -> BufferTooSmall.
    std::array<double, 1> mins_small{}, maxs_small{};
    EXPECT_EQ(
        ErrCode::BufferTooSmall,
        Lerc::GetLercInfo(blob.data(), static_cast<unsigned int>(blob.size()),
                          info, mins_small.data(), maxs_small.data(), 1));

    // Size buffers for the largest probed dims (5 * 4 * 3) before rejecting
    // too few masks, too many bands, or mismatched column count.
    std::vector<int> dst(5 * 4 * 3, -99);
    std::vector<Byte> out_masks(5 * 4 * 3, 0xAA);
    EXPECT_EQ(ErrCode::WrongParam,  // nMasks=1 < blob's 2 differing masks
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           /*n_masks=*/1, out_masks.data(), 1, 4, 4, 2,
                           Lerc::DT_Int, dst.data(), nullptr, nullptr));
    EXPECT_EQ(ErrCode::WrongParam,  // nBands=3 > encoded 2 bands
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           /*n_masks=*/2, out_masks.data(), 1, 4, 4, 3,
                           Lerc::DT_Int, dst.data(), nullptr, nullptr));
    EXPECT_EQ(ErrCode::Failed,  // nCols=5 != encoded 4
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           /*n_masks=*/2, out_masks.data(), 1, 5, 4, 2,
                           Lerc::DT_Int, dst.data(), nullptr, nullptr));
    EXPECT_EQ(ErrCode::Ok,
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           /*n_masks=*/2, out_masks.data(), 1, 4, 4, 2,
                           Lerc::DT_Int, dst.data(), nullptr, nullptr));
    EXPECT_TRUE(
        std::equal(two_masks.begin(), two_masks.end(), out_masks.begin()));
  }
}

TEST(LercTest, GetLercInfoAndDecodeRejectCorruptAndMismatchedLerc2Blobs) {
  std::vector<int> b0(16, 1);
  b0[1] = 2;
  std::vector<int> b1(16, 3);
  b1[2] = 9;
  const std::vector<Byte> blob0 = EncodeAll(b0, 5, 1, 4, 4, 1, 0, nullptr, 0.5);
  ASSERT_FALSE(blob0.empty());
  const std::vector<Byte> blob1_diff_dim =
      EncodeAll(b1, 5, 1, 8, 2, 1, 0, nullptr, 0.5);
  ASSERT_FALSE(blob1_diff_dim.empty());
  const std::vector<Byte> blob1_same =
      EncodeAll(b1, 5, 1, 4, 4, 1, 0, nullptr, 0.5);
  ASSERT_FALSE(blob1_same.empty());

  Lerc::LercInfo info{};
  // Single band truncated before its declared blobSize -> Failed.
  EXPECT_EQ(
      ErrCode::Failed,
      Lerc::GetLercInfo(blob0.data(),
                        static_cast<unsigned int>(blob0.size() - 1), info));

  // Negative blobSize in header -> Lerc2::ReadHeader rejects blobSize <= 0, so
  // Lerc::GetLercInfo falls through to LERC1 and returns Failed.
  {
    auto bad = blob0;
    WritePodAt(bad, kLerc2BlobSizeOffset, -5);
    EXPECT_EQ(ErrCode::Failed,
              Lerc::GetLercInfo(bad.data(),
                                static_cast<unsigned int>(bad.size()), info));
  }

  // Concatenated Band 1 with mismatched dimensions -> Failed.
  {
    std::vector<Byte> mismatched = blob0;
    mismatched.insert(mismatched.end(), blob1_diff_dim.begin(),
                      blob1_diff_dim.end());
    EXPECT_EQ(
        ErrCode::Failed,
        Lerc::GetLercInfo(mismatched.data(),
                          static_cast<unsigned int>(mismatched.size()), info));
  }

  // Concatenated Band 1 truncated before Band 1 blobSize -> Failed.
  {
    std::vector<Byte> trunc_b1 = blob0;
    trunc_b1.insert(trunc_b1.end(), blob1_same.begin(), blob1_same.end() - 1);
    EXPECT_EQ(
        ErrCode::Failed,
        Lerc::GetLercInfo(trunc_b1.data(),
                          static_cast<unsigned int>(trunc_b1.size()), info));
  }

  // Corrupted payload checksum causes Decode to fail after GetLercInfo
  // succeeds.
  {
    auto corrupt = blob0;
    corrupt.back() ^= 0xFF;
    std::vector<int> dst(16, -99);
    EXPECT_EQ(
        ErrCode::Failed,
        Lerc::Decode(corrupt.data(), static_cast<unsigned int>(corrupt.size()),
                     0, nullptr, 1, 4, 4, 1, Lerc::DT_Int, dst.data(), nullptr,
                     nullptr));
  }

  // Arbitrary non-LERC1/non-LERC2 bytes fail GetLercInfo.
  {
    const std::array<Byte, 32> garbage = {1, 2, 3, 4};
    EXPECT_EQ(ErrCode::Failed, Lerc::GetLercInfo(garbage.data(), 32, info));
  }
}

TEST(LercTest, Version5NanHandlingInCheckForNanAndReplaceNanValues) {
  constexpr float kQNaN = std::numeric_limits<float>::quiet_NaN();
  // Band 0 unmasked with 1 NaN pixel -> maskBuffer modified; Band 1 all valid
  // -> hits MasksDiffer(!p0) & ReplaceNaNValues.
  {
    constexpr float kMaxZErr = 0.01f;
    std::vector<float> src(32, 10.0f);
    src[0] = kQNaN;
    src[1] = 12.0f;
    src[17] = 15.0f;
    const std::vector<Byte> blob =
        EncodeAll(src, /*version=*/5, 1, 4, 4, 2, 0, nullptr, kMaxZErr);
    ASSERT_FALSE(blob.empty());

    Lerc::LercInfo info{};
    ASSERT_EQ(ErrCode::Ok,
              Lerc::GetLercInfo(blob.data(),
                                static_cast<unsigned int>(blob.size()), info));
    EXPECT_EQ(2, info.nMasks);

    std::vector<float> dst(32, -1.0f);
    std::vector<Byte> out_masks(32, 0xAA);
    ASSERT_EQ(ErrCode::Ok,
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           2, out_masks.data(), 1, 4, 4, 2, Lerc::DT_Float,
                           dst.data(), nullptr, nullptr));
    EXPECT_EQ(0, out_masks[0]);
    EXPECT_EQ(1, out_masks[1]);
    EXPECT_EQ(1, out_masks[16]);
    EXPECT_NEAR(12.0f, dst[1], kMaxZErr);
    EXPECT_NEAR(15.0f, dst[17], kMaxZErr);
  }

  // Band 0 clean, Band 1 masked with all-NaN, mixed-NaN, and already-invalid
  // NaN in nDepth=2 double.
  {
    constexpr double kDQNaN = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> src(2 * 16 * 2, 5.0);
    // Band 1: pixel 0 has (NaN, NaN), pixel 1 has (NaN, 9.5), pixel 2 is masked
    // invalid with NaN.
    src[32 + 0] = kDQNaN;
    src[32 + 1] = kDQNaN;
    src[32 + 2] = kDQNaN;
    src[32 + 3] = 9.5;
    src[32 + 4] = kDQNaN;
    std::vector<Byte> b1_masks(32, 1);
    b1_masks[16 + 2] = 0;  // invalid pixel on Band 1 stays untouched

    const std::vector<Byte> blob =
        EncodeAll(src, /*version=*/5, 2, 4, 4, 2, 2, b1_masks.data(), 0.0);
    ASSERT_FALSE(blob.empty());

    std::vector<double> dst(src.size(), -1.0);
    std::vector<Byte> out_masks(32, 0xAA);
    ASSERT_EQ(ErrCode::Ok,
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           2, out_masks.data(), 2, 4, 4, 2, Lerc::DT_Double,
                           dst.data(), nullptr, nullptr));
    EXPECT_EQ(0, out_masks[16 + 0]);  // (NaN, NaN) masked out
    EXPECT_EQ(1, out_masks[16 + 1]);  // mixed kept
    EXPECT_EQ(0, out_masks[16 + 2]);
    EXPECT_DOUBLE_EQ(9.5, dst[32 + 3]);

    // Unmasked Band 0 clean (nullptr) -> Band 1 NaN (&maskBuffer[0]) hits
    // MasksDiffer(!p1).
    std::vector<double> unmasked(2 * 16 * 2, 5.0);
    unmasked[32 + 0] = kDQNaN;
    unmasked[32 + 1] = kDQNaN;
    const std::vector<Byte> ublob =
        EncodeAll(unmasked, /*version=*/5, 2, 4, 4, 2, 0, nullptr, 0.0);
    ASSERT_FALSE(ublob.empty());
    std::vector<double> udst(unmasked.size(), -1.0);
    std::vector<Byte> umasks(32, 0xAA);
    ASSERT_EQ(
        ErrCode::Ok,
        Lerc::Decode(ublob.data(), static_cast<unsigned int>(ublob.size()), 2,
                     umasks.data(), 2, 4, 4, 2, Lerc::DT_Double, udst.data(),
                     nullptr, nullptr));
    EXPECT_EQ(1, umasks[0]);
    EXPECT_EQ(0, umasks[16 + 0]);
  }
}

TEST(LercTest, Version6IntegerFilterNoDataAndRemapNoDataAndHasNoData) {
  // 1. Out-of-range integer noDataValue rejected for every signed/unsigned
  // type.
  {
    std::vector<signed char> c(16, 1);
    std::vector<Byte> b(16, 1);
    std::vector<std::int16_t> s(16, 1);
    std::vector<std::uint16_t> us(16, 1);
    std::vector<int> i(16, 1);
    std::vector<unsigned int> ui(16, 1);
    unsigned int needed = 0;
    const double hi = 1e12;
    const double lo = -200.0;
    EXPECT_EQ(ErrCode::WrongParam, Lerc::ComputeCompressedSizeTempl(
                                       c.data(), -1, 1, 4, 4, 1, 0, nullptr,
                                       0.5, needed, &kValidFlag, &lo));
    EXPECT_EQ(ErrCode::WrongParam, Lerc::ComputeCompressedSizeTempl(
                                       b.data(), -1, 1, 4, 4, 1, 0, nullptr,
                                       0.5, needed, &kValidFlag, &hi));
    EXPECT_EQ(ErrCode::WrongParam, Lerc::ComputeCompressedSizeTempl(
                                       s.data(), -1, 1, 4, 4, 1, 0, nullptr,
                                       0.5, needed, &kValidFlag, &hi));
    EXPECT_EQ(ErrCode::WrongParam, Lerc::ComputeCompressedSizeTempl(
                                       us.data(), -1, 1, 4, 4, 1, 0, nullptr,
                                       0.5, needed, &kValidFlag, &lo));
    EXPECT_EQ(ErrCode::WrongParam, Lerc::ComputeCompressedSizeTempl(
                                       i.data(), -1, 1, 4, 4, 1, 0, nullptr,
                                       0.5, needed, &kValidFlag, &hi));
    EXPECT_EQ(ErrCode::WrongParam, Lerc::ComputeCompressedSizeTempl(
                                       ui.data(), -1, 1, 4, 4, 1, 0, nullptr,
                                       0.5, needed, &kValidFlag, &lo));
  }

  // 2. All pixels equal to noDataValue -> minVal == DBL_MAX -> empty valid
  // mask.
  {
    std::vector<int> all_nd(16, -999);
    const double nd = -999.0;
    const std::vector<Byte> blob =
        EncodeAll(all_nd, -1, 1, 4, 4, 1, 0, nullptr, 2.0, &kValidFlag, &nd);
    ASSERT_FALSE(blob.empty());
    Lerc::LercInfo info{};
    ASSERT_EQ(ErrCode::Ok,
              Lerc::GetLercInfo(blob.data(),
                                static_cast<unsigned int>(blob.size()), info));
    EXPECT_EQ(0, info.numValidPixel);
  }

  // 3. nDepth=2 mixed valid + noData:
  //    (a) origNoData close to valid range -> lossless fallback (maxZErr =
  //    0.5); (b) remap below minVal (DT_Int); (c) minVal near typeRange.first
  //    triggers lossless minVal-1 (DT_Byte minVal=1); (d) minVal at
  //    typeRange.first falls through to maxVal+1 (DT_Byte minVal=0).
  {
    // (a) DT_Short origNoData = 5 within dist of [10, 20].
    std::vector<std::int16_t> s_close(32, 10);
    s_close[1] = 20;
    s_close[2] = 5;  // mixed pixel (5, 10)
    const double nd_close = 5.0;
    const std::vector<Byte> b_close = EncodeAll(
        s_close, -1, 2, 4, 4, 1, 0, nullptr, 6.0, &kValidFlag, &nd_close);
    ASSERT_FALSE(b_close.empty());
    Lerc::LercInfo info{};
    ASSERT_EQ(
        ErrCode::Ok,
        Lerc::GetLercInfo(b_close.data(),
                          static_cast<unsigned int>(b_close.size()), info));
    EXPECT_DOUBLE_EQ(0.5, info.maxZError);
  }
  {
    // (b) DT_Int valid [100, 200], noData = -9999, maxZErr = 2.0 -> remapped
    // to 97.
    constexpr double kMaxZErr = 2.0;
    std::vector<int> ivals(32, 100);
    ivals[0] = -9999;
    ivals[1] = -9999;  // whole pixel 0 moved to mask
    ivals[2] = -9999;  // pixel 1 mixed: (-9999, 100)
    ivals[5] = 200;
    const double nd_int = -9999.0;
    const std::vector<Byte> blob = EncodeAll(ivals, -1, 2, 4, 4, 1, 0, nullptr,
                                             kMaxZErr, &kValidFlag, &nd_int);
    ASSERT_FALSE(blob.empty());

    Lerc::LercInfo info{};
    std::array<double, 2> mins{}, maxs{};
    EXPECT_EQ(
        ErrCode::HasNoData,
        Lerc::GetLercInfo(blob.data(), static_cast<unsigned int>(blob.size()),
                          info, mins.data(), maxs.data(), 2));

    std::vector<int> dst(32, -77);
    std::vector<Byte> out_mask(16, 0xAA);
    EXPECT_EQ(ErrCode::HasNoData,
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           1, out_mask.data(), 2, 4, 4, 1, Lerc::DT_Int,
                           dst.data(), nullptr, nullptr));

    unsigned char dec_uses_nd = 0;
    double dec_nd = 0.0;
    ASSERT_EQ(ErrCode::Ok,
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           1, out_mask.data(), 2, 4, 4, 1, Lerc::DT_Int,
                           dst.data(), &dec_uses_nd, &dec_nd));
    EXPECT_EQ(1, dec_uses_nd);
    EXPECT_DOUBLE_EQ(-9999.0, dec_nd);
    EXPECT_EQ(0, out_mask[0]);
    EXPECT_EQ(1, out_mask[1]);
    EXPECT_EQ(-9999, dst[2]);
    EXPECT_NEAR(100, dst[3], kMaxZErr);
    EXPECT_NEAR(200, dst[5], kMaxZErr);
  }
  {
    // (c) & (d) DT_Byte: minVal=1 (falls back to lossless 0) & minVal=0 (maps
    // to maxVal+1).
    for (const Byte min_v : {Byte{1}, Byte{0}}) {
      std::vector<Byte> bvals(32, min_v);
      bvals[0] = 250;  // mixed (250, min_v)
      bvals[3] = 10;   // maxVal = 10
      const double nd_b = 250.0;
      const std::vector<Byte> blob =
          EncodeAll(bvals, -1, 2, 4, 4, 1, 0, nullptr, 5.0, &kValidFlag, &nd_b);
      ASSERT_FALSE(blob.empty());

      std::vector<Byte> dst(32, 99);
      std::vector<Byte> out_mask(16, 0xAA);
      unsigned char dec_uses_nd = 0;
      double dec_nd = 0.0;
      ASSERT_EQ(
          ErrCode::Ok,
          Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()), 1,
                       out_mask.data(), 2, 4, 4, 1, Lerc::DT_Byte, dst.data(),
                       &dec_uses_nd, &dec_nd));
      EXPECT_EQ(1, dec_uses_nd);
      EXPECT_DOUBLE_EQ(250.0, dec_nd);
      EXPECT_EQ(250, dst[0]);
      EXPECT_EQ(min_v, dst[1]);
      EXPECT_EQ(10, dst[3]);
    }
  }
}

TEST(LercTest, Version6FloatDoubleFilterNoDataAndNanAndFindNewNoData) {
  constexpr float kFQNaN = std::numeric_limits<float>::quiet_NaN();
  constexpr double kDQNaN = std::numeric_limits<double>::quiet_NaN();

  // 1. Float noDataValue outside [-FLT_MAX, FLT_MAX] -> WrongParam.
  {
    std::vector<float> f(16, 1.0f);
    unsigned int needed = 0;
    const double too_big = 1e100;
    EXPECT_EQ(ErrCode::WrongParam, Lerc::ComputeCompressedSizeTempl(
                                       f.data(), -1, 1, 4, 4, 1, 0, nullptr,
                                       0.1, needed, &kValidFlag, &too_big));
  }

  // 2. nDepth=1 NaN moves to mask; all-NaN tile yields numValidPixel=0;
  //    nDepth=2 mixed NaN without bPassNoDataValue -> ErrCode::NaN.
  {
    constexpr double kMaxZErr = 0.1;
    std::vector<float> f1(16, 3.5f);
    f1[0] = kFQNaN;
    const std::vector<Byte> b1 =
        EncodeAll(f1, -1, 1, 4, 4, 1, 0, nullptr, kMaxZErr);
    ASSERT_FALSE(b1.empty());
    std::vector<Byte> out_m(16, 0xAA);
    std::vector<float> d1(16, -1.0f);
    ASSERT_EQ(ErrCode::Ok,
              Lerc::Decode(b1.data(), static_cast<unsigned int>(b1.size()), 1,
                           out_m.data(), 1, 4, 4, 1, Lerc::DT_Float, d1.data(),
                           nullptr, nullptr));
    EXPECT_EQ(0, out_m[0]);
    EXPECT_NEAR(3.5f, d1[1], kMaxZErr);

    std::vector<float> all_nan(16, kFQNaN);
    const std::vector<Byte> b_empty =
        EncodeAll(all_nan, -1, 1, 4, 4, 1, 0, nullptr, kMaxZErr);
    ASSERT_FALSE(b_empty.empty());
    Lerc::LercInfo info{};
    ASSERT_EQ(
        ErrCode::Ok,
        Lerc::GetLercInfo(b_empty.data(),
                          static_cast<unsigned int>(b_empty.size()), info));
    EXPECT_EQ(0, info.numValidPixel);

    std::vector<float> mixed_nan(32, 1.5f);
    mixed_nan[0] = kFQNaN;
    unsigned int needed = 0;
    EXPECT_EQ(ErrCode::NaN, Lerc::ComputeCompressedSizeTempl(
                                mixed_nan.data(), -1, 2, 4, 4, 1, 0, nullptr,
                                kMaxZErr, needed, nullptr, nullptr));
  }

  // 3. FindNewNoDataBelowValidMin for both bAllInt==true (double) and
  //    bAllInt==false (float), plus origNoData in [min-dist, max+dist] and
  //    fallback when no candidate exceeds lowIntLimit.
  {
    // (a) bAllInt == true on double with mixed NaN + origNoData.
    constexpr double kMaxZErr = 2.0;
    std::vector<double> d_int(32, 1000.0);
    d_int[0] = kDQNaN;
    d_int[2] = 2000.0;
    const double nd = -999999.0;
    const std::vector<Byte> blob = EncodeAll(d_int, -1, 2, 4, 4, 1, 0, nullptr,
                                             kMaxZErr, &kValidFlag, &nd);
    ASSERT_FALSE(blob.empty());

    std::vector<double> dst(32, -1.0);
    std::vector<Byte> out_m(16, 0xAA);
    unsigned char dec_nd_flag = 0;
    double dec_nd = 0.0;
    ASSERT_EQ(ErrCode::Ok,
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           1, out_m.data(), 2, 4, 4, 1, Lerc::DT_Double,
                           dst.data(), &dec_nd_flag, &dec_nd));
    EXPECT_EQ(1, dec_nd_flag);
    EXPECT_DOUBLE_EQ(-999999.0, dec_nd);
    EXPECT_DOUBLE_EQ(-999999.0, dst[0]);
    EXPECT_NEAR(1000.0, dst[1], kMaxZErr);
    EXPECT_NEAR(2000.0, dst[2], kMaxZErr);
  }
  {
    // (b) bAllInt == false on float with non-integer data and mixed noData.
    constexpr double kMaxZErr = 0.5;
    std::vector<float> f_nonint(32, 10.25f);
    f_nonint[0] = kFQNaN;  // replaced by origNoData (-99999.5f)
    f_nonint[2] = -99999.5f;
    f_nonint[5] = 20.75f;
    const double nd = -99999.5;
    const std::vector<Byte> blob = EncodeAll(
        f_nonint, -1, 2, 4, 4, 1, 0, nullptr, kMaxZErr, &kValidFlag, &nd);
    ASSERT_FALSE(blob.empty());

    std::vector<float> dst(32, -1.0f);
    std::vector<Byte> out_m(16, 0xAA);
    unsigned char dec_nd_flag = 0;
    double dec_nd = 0.0;
    ASSERT_EQ(ErrCode::Ok,
              Lerc::Decode(blob.data(), static_cast<unsigned int>(blob.size()),
                           1, out_m.data(), 2, 4, 4, 1, Lerc::DT_Float,
                           dst.data(), &dec_nd_flag, &dec_nd));
    EXPECT_EQ(1, dec_nd_flag);
    EXPECT_DOUBLE_EQ(-99999.5, dec_nd);
    EXPECT_FLOAT_EQ(-99999.5f, dst[0]);
    EXPECT_FLOAT_EQ(-99999.5f, dst[2]);
    EXPECT_NEAR(10.25f, dst[1], kMaxZErr);
    EXPECT_NEAR(20.75f, dst[5], kMaxZErr);
  }
  {
    // (c) origNoData within dist triggers lossless fallback (maxZError == 0);
    //     and bAllInt float at lowIntLimit+1 (-8388607) fails FindNewNoData
    //     with origNoData >= minVal -> lossless int fallback (maxZError ==
    //     0.5).
    std::vector<double> f_near(32, 10.5);
    f_near[0] = 9.0;  // mixed noData = 9.0 within 2*2.0 of 10.5
    double nd = 9.0;
    const std::vector<Byte> b_near =
        EncodeAll(f_near, -1, 2, 4, 4, 1, 0, nullptr, 2.0, &kValidFlag, &nd);
    ASSERT_FALSE(b_near.empty());
    Lerc::LercInfo info{};
    ASSERT_EQ(ErrCode::Ok, Lerc::GetLercInfo(
                               b_near.data(),
                               static_cast<unsigned int>(b_near.size()), info));
    EXPECT_DOUBLE_EQ(0.0, info.maxZError);

    std::vector<float> f_low(32, -8388607.0f);
    f_low[0] = 50.0f;  // origNoData = 50.0 >= minVal
    f_low[5] = 0.0f;
    nd = 50.0;
    const std::vector<Byte> b_low =
        EncodeAll(f_low, -1, 2, 4, 4, 1, 0, nullptr, 2.0, &kValidFlag, &nd);
    ASSERT_FALSE(b_low.empty());
    ASSERT_EQ(ErrCode::Ok,
              Lerc::GetLercInfo(b_low.data(),
                                static_cast<unsigned int>(b_low.size()), info));
    EXPECT_DOUBLE_EQ(0.5, info.maxZError);
  }
}

TEST(LercTest, DecodesLegacyTwoBandMaskedLerc1BlobAndRejectsBadLerc1) {
  const std::vector<Byte> lerc1_blob = BuildSynthesizedTwoBandLerc1Blob();
  ASSERT_FALSE(lerc1_blob.empty());

  // 1. GetLercInfo with per-band min/max on 2-band LERC1 blob.
  Lerc::LercInfo info{};
  std::array<double, 2> mins{};
  mins.fill(777.0);
  std::array<double, 2> maxs{};
  maxs.fill(777.0);
  ASSERT_EQ(ErrCode::Ok,
            Lerc::GetLercInfo(lerc1_blob.data(),
                              static_cast<unsigned int>(lerc1_blob.size()),
                              info, mins.data(), maxs.data(), 2));
  EXPECT_EQ(0, info.version);
  EXPECT_EQ(1, info.nDepth);
  EXPECT_EQ(2, info.nCols);
  EXPECT_EQ(2, info.nRows);
  EXPECT_EQ(3, info.numValidPixel);
  EXPECT_EQ(2, info.nBands);
  EXPECT_EQ(1, info.nMasks);
  EXPECT_EQ(Lerc::DT_Float, info.dt);
  EXPECT_FLOAT_EQ(5.0f, static_cast<float>(info.zMin));
  EXPECT_FLOAT_EQ(30.0f, static_cast<float>(info.zMax));
  EXPECT_FLOAT_EQ(10.2f, static_cast<float>(mins[0]));
  EXPECT_FLOAT_EQ(30.0f, static_cast<float>(maxs[0]));
  EXPECT_FLOAT_EQ(5.0f, static_cast<float>(mins[1]));
  EXPECT_FLOAT_EQ(25.0f, static_cast<float>(maxs[1]));

  // 2. Decode into DT_Float and DT_Int with nMasks=1; nMasks=0 fails
  //    because invalid pixels exist (bMustFillMask).
  std::vector<float> dst_f(3 * 2 * 3, -1.0f);
  std::vector<Byte> mask_f(4, 0xAA);
  EXPECT_EQ(ErrCode::Failed,
            Lerc::Decode(lerc1_blob.data(),
                         static_cast<unsigned int>(lerc1_blob.size()),
                         /*n_masks=*/0, nullptr, 1, 2, 2, 2, Lerc::DT_Float,
                         dst_f.data(), nullptr, nullptr));
  ASSERT_EQ(ErrCode::Ok,
            Lerc::Decode(lerc1_blob.data(),
                         static_cast<unsigned int>(lerc1_blob.size()),
                         /*n_masks=*/1, mask_f.data(), 1, 2, 2, 2,
                         Lerc::DT_Float, dst_f.data(), nullptr, nullptr));
  EXPECT_EQ(1, mask_f[0]);
  EXPECT_EQ(1, mask_f[1]);
  EXPECT_EQ(1, mask_f[2]);
  EXPECT_EQ(0, mask_f[3]);
  EXPECT_FLOAT_EQ(10.2f, dst_f[0]);
  EXPECT_FLOAT_EQ(20.7f, dst_f[1]);
  EXPECT_FLOAT_EQ(30.0f, dst_f[2]);
  EXPECT_FLOAT_EQ(5.0f, dst_f[4]);
  EXPECT_FLOAT_EQ(15.0f, dst_f[5]);
  EXPECT_FLOAT_EQ(25.0f, dst_f[6]);

  std::vector<int> dst_i(8, -1);
  ASSERT_EQ(ErrCode::Ok,
            Lerc::Decode(lerc1_blob.data(),
                         static_cast<unsigned int>(lerc1_blob.size()),
                         /*n_masks=*/1, mask_f.data(), 1, 2, 2, 2, Lerc::DT_Int,
                         dst_i.data(), nullptr, nullptr));
  EXPECT_EQ(10, dst_i[0]);
  EXPECT_EQ(21, dst_i[1]);  // LERC1 -> DT_Int rounds 20.7f to nearest int (21)
  EXPECT_EQ(30, dst_i[2]);
  EXPECT_EQ(5, dst_i[4]);
  EXPECT_EQ(15, dst_i[5]);
  EXPECT_EQ(25, dst_i[6]);

  // 3. LERC1 error guards: wrong nCols, too many bands, bad dimensions, and
  //    1-band LERC1 followed by trailing garbage > 51 bytes.
  EXPECT_EQ(ErrCode::Failed,
            Lerc::Decode(lerc1_blob.data(),
                         static_cast<unsigned int>(lerc1_blob.size()), 1,
                         mask_f.data(), 1, 3, 2, 1, Lerc::DT_Float,
                         dst_f.data(), nullptr, nullptr));
  EXPECT_EQ(ErrCode::Failed,
            Lerc::Decode(lerc1_blob.data(),
                         static_cast<unsigned int>(lerc1_blob.size()), 1,
                         mask_f.data(), 1, 2, 2, 3, Lerc::DT_Float,
                         dst_f.data(), nullptr, nullptr));
  {
    auto bad_dim = lerc1_blob;
    WritePodAt(bad_dim, kLerc1HeightOffset, 50000);  // height > 40000
    EXPECT_EQ(
        ErrCode::Failed,
        Lerc::GetLercInfo(bad_dim.data(),
                          static_cast<unsigned int>(bad_dim.size()), info));
  }
  {
    // Band 0 + 60 zero bytes -> Band 0 succeeds, trailing garbage breaks
    // GetLercInfo's LERC1 band loop cleanly at (lercInfo.nBands > 0) ? Ok :
    // Failed.
    std::vector<Byte> b0_plus_junk(lerc1_blob.begin(),
                                   lerc1_blob.end() - kLerc1Band1Bytes);
    b0_plus_junk.resize(b0_plus_junk.size() + 60, 0);
    EXPECT_EQ(ErrCode::Ok,
              Lerc::GetLercInfo(b0_plus_junk.data(),
                                static_cast<unsigned int>(b0_plus_junk.size()),
                                info));
    EXPECT_EQ(1, info.nBands);
  }
}

}  // namespace
}  // namespace lerc
