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
// Unit tests for the public C API
// (third_party/lerc/LercLib/Lerc_c_api_impl.cpp).
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/include/Lerc_c_api.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/include/Lerc_types.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc_c_api_impl.cpp

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/lerc/LercLib/include/Lerc_c_api.h"
#include "third_party/lerc/LercLib/include/Lerc_types.h"

namespace lerc {
namespace {

using ::LercNS::DataRangeArrOrder;
using ::LercNS::DataType;
using ::LercNS::ErrCode;
using ::LercNS::InfoArrOrder;

constexpr lerc_status kOk = static_cast<lerc_status>(ErrCode::Ok);
constexpr lerc_status kFailed = static_cast<lerc_status>(ErrCode::Failed);
constexpr lerc_status kWrongParam =
    static_cast<lerc_status>(ErrCode::WrongParam);
constexpr lerc_status kBufferTooSmall =
    static_cast<lerc_status>(ErrCode::BufferTooSmall);
constexpr lerc_status kHasNoData = static_cast<lerc_status>(ErrCode::HasNoData);

// Bump on upstream codec format upgrades.
constexpr unsigned int kCurrentCodecVersion = 6U;
constexpr std::size_t kInfoArrSize =
    static_cast<std::size_t>(InfoArrOrder::_last);
constexpr std::size_t kDataRangeArrSize =
    static_cast<std::size_t>(DataRangeArrOrder::_last);

constexpr std::size_t Idx(InfoArrOrder field) noexcept {
  return static_cast<std::size_t>(field);
}
constexpr std::size_t Idx(DataRangeArrOrder field) noexcept {
  return static_cast<std::size_t>(field);
}

static_assert(LERC_VERSION_MAJOR == 4);
static_assert(LERC_VERSION_MINOR == 2);
static_assert(LERC_VERSION_PATCH == 0);
static_assert(LERC_VERSION_NUMBER == 40200);
static_assert(LERC_AT_LEAST_VERSION(4, 2, 0));
static_assert(!LERC_AT_LEAST_VERSION(4, 2, 1));
static_assert(kOk == 0U);
static_assert(kInfoArrSize == 11U);
static_assert(kDataRangeArrSize == 3U);

template <typename T>
constexpr unsigned int CApiDataType() {
  if constexpr (std::is_same_v<T, signed char>) {
    return static_cast<unsigned int>(DataType::dt_char);
  } else if constexpr (std::is_same_v<T, unsigned char>) {
    return static_cast<unsigned int>(DataType::dt_uchar);
  } else if constexpr (std::is_same_v<T, std::int16_t>) {
    return static_cast<unsigned int>(DataType::dt_short);
  } else if constexpr (std::is_same_v<T, std::uint16_t>) {
    return static_cast<unsigned int>(DataType::dt_ushort);
  } else if constexpr (std::is_same_v<T, int>) {
    return static_cast<unsigned int>(DataType::dt_int);
  } else if constexpr (std::is_same_v<T, unsigned int>) {
    return static_cast<unsigned int>(DataType::dt_uint);
  } else if constexpr (std::is_same_v<T, float>) {
    return static_cast<unsigned int>(DataType::dt_float);
  } else {
    static_assert(std::is_same_v<T, double>);
    return static_cast<unsigned int>(DataType::dt_double);
  }
}

template <typename T>
void ExpectCApiRoundTripForType() {
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
          src[idx] = static_cast<T>(static_cast<int>(k) - 8 +
                                    static_cast<int>(b) * 4 + m * 3);
        } else {
          src[idx] = static_cast<T>(k + b * 5 + m * 2 + 1);
        }
      }
    }
  }

  // Band 0 masks pixel 0 (valid k in [1..29]); band 1 masks pixel 3 (valid k in
  // [0..2, 4..29]).
  std::vector<unsigned char> valid_bytes(n_pix * kBands, 1);
  valid_bytes[0] = 0;
  valid_bytes[n_pix + 3] = 0;

  const unsigned int dt = CApiDataType<T>();
  unsigned int num_bytes = 0;
  ASSERT_EQ(kOk, lerc_computeCompressedSize(
                     src.data(), dt, kDepth, kCols, kRows, kBands, kBands,
                     valid_bytes.data(), /*maxZErr=*/0.0, &num_bytes));
  ASSERT_GT(num_bytes, 0U);

  std::vector<unsigned char> blob(num_bytes, 0);
  unsigned int n_written = 0;
  ASSERT_EQ(kOk, lerc_encode(src.data(), dt, kDepth, kCols, kRows, kBands,
                             kBands, valid_bytes.data(), 0.0, blob.data(),
                             num_bytes, &n_written));
  EXPECT_EQ(num_bytes, n_written);

  std::array<unsigned int, kInfoArrSize> info{};
  info.fill(0xCAFEBABEU);
  std::array<double, kDataRangeArrSize> ranges{};
  ranges.fill(777.0);
  ASSERT_EQ(kOk, lerc_getBlobInfo(blob.data(), n_written, info.data(),
                                  ranges.data(), static_cast<int>(info.size()),
                                  static_cast<int>(ranges.size())));
  EXPECT_EQ(kCurrentCodecVersion, info[Idx(InfoArrOrder::version)]);
  EXPECT_EQ(dt, info[Idx(InfoArrOrder::dataType)]);
  EXPECT_EQ(static_cast<unsigned int>(kDepth), info[Idx(InfoArrOrder::nDim)]);
  EXPECT_EQ(static_cast<unsigned int>(kDepth), info[Idx(InfoArrOrder::nDepth)]);
  EXPECT_EQ(static_cast<unsigned int>(kCols), info[Idx(InfoArrOrder::nCols)]);
  EXPECT_EQ(static_cast<unsigned int>(kRows), info[Idx(InfoArrOrder::nRows)]);
  EXPECT_EQ(static_cast<unsigned int>(kBands), info[Idx(InfoArrOrder::nBands)]);
  EXPECT_EQ(static_cast<unsigned int>(n_pix - 1),
            info[Idx(InfoArrOrder::nValidPixels)]);
  EXPECT_EQ(n_written, info[Idx(InfoArrOrder::blobSize)]);
  EXPECT_EQ(static_cast<unsigned int>(kBands), info[Idx(InfoArrOrder::nMasks)]);
  EXPECT_EQ(0U, info[Idx(InfoArrOrder::nUsesNoDataValue)]);

  const double expected_global_min = std::is_signed_v<T> ? -7.0 : 2.0;
  const double expected_global_max = std::is_signed_v<T> ? 28.0 : 37.0;
  EXPECT_DOUBLE_EQ(expected_global_min, ranges[Idx(DataRangeArrOrder::zMin)]);
  EXPECT_DOUBLE_EQ(expected_global_max, ranges[Idx(DataRangeArrOrder::zMax)]);
  // Integer types and all-integer float/double rasters map maxZErr == 0.0 to
  // 0.5 (integer lossless quantization) in Lerc::FilterNoDataAndNaN / Lerc2.
  EXPECT_DOUBLE_EQ(0.5, ranges[Idx(DataRangeArrOrder::maxZErrUsed)]);

  std::array<double, kDepth * kBands> mins{};
  mins.fill(777.0);
  std::array<double, kDepth * kBands> maxs{};
  maxs.fill(777.0);
  ASSERT_EQ(kOk, lerc_getDataRanges(blob.data(), n_written, kDepth, kBands,
                                    mins.data(), maxs.data()));
  for (int b = 0; b < kBands; ++b) {
    for (int m = 0; m < kDepth; ++m) {
      const std::size_t idx = static_cast<std::size_t>(b * kDepth + m);
      if constexpr (std::is_signed_v<T>) {
        EXPECT_DOUBLE_EQ(b == 0 ? (-7.0 + 3.0 * m) : (-4.0 + 3.0 * m),
                         mins[idx]);
        EXPECT_DOUBLE_EQ(21.0 + 4.0 * b + 3.0 * m, maxs[idx]);
      } else {
        EXPECT_DOUBLE_EQ(b == 0 ? (2.0 + 2.0 * m) : (6.0 + 2.0 * m), mins[idx]);
        EXPECT_DOUBLE_EQ(30.0 + 5.0 * b + 2.0 * m, maxs[idx]);
      }
    }
  }

  auto verify_valid_pixels = [&](const auto& decoded, auto check_eq) {
    for (std::size_t b = 0; b < kBands; ++b) {
      for (std::size_t k = 0; k < n_pix; ++k) {
        if (valid_bytes[b * n_pix + k] != 0) {
          for (int m = 0; m < kDepth; ++m) {
            const std::size_t idx = (b * n_pix + k) * kDepth + m;
            check_eq(src[idx], decoded[idx]);
          }
        }
      }
    }
  };

  // Typed decode with sentinel-prefilled output buffer.
  // MSan safety for lossless float/double depends on upstream EncodeHuffman's
  // heuristic and stream termination for this test data.
  std::vector<T> dst(src.size(), static_cast<T>(55));
  std::vector<unsigned char> out_masks(valid_bytes.size(), 0xAA);
  ASSERT_EQ(kOk, lerc_decode(blob.data(), n_written, kBands, out_masks.data(),
                             kDepth, kCols, kRows, kBands, dt, dst.data()));
  EXPECT_EQ(valid_bytes, out_masks);
  verify_valid_pixels(dst, [](const T& expected, const T& actual) {
    EXPECT_EQ(expected, actual);
  });

  // decodeToDouble exercises both in-place offset conversion (T != double)
  // and direct double decoding (T == double).
  std::vector<double> dbl_dst(src.size(), -9999.0);
  std::vector<unsigned char> dbl_masks(valid_bytes.size(), 0x55);
  ASSERT_EQ(
      kOk, lerc_decodeToDouble(blob.data(), n_written, kBands, dbl_masks.data(),
                               kDepth, kCols, kRows, kBands, dbl_dst.data()));
  EXPECT_EQ(valid_bytes, dbl_masks);
  verify_valid_pixels(dbl_dst, [](const T& expected, double actual) {
    EXPECT_DOUBLE_EQ(static_cast<double>(expected), actual);
  });
}

TEST(LercCApiImplTest, RoundTripsAllEightTypesAndVersionedEncode) {
  ExpectCApiRoundTripForType<signed char>();
  ExpectCApiRoundTripForType<unsigned char>();
  ExpectCApiRoundTripForType<std::int16_t>();
  ExpectCApiRoundTripForType<std::uint16_t>();
  ExpectCApiRoundTripForType<int>();
  ExpectCApiRoundTripForType<unsigned int>();
  ExpectCApiRoundTripForType<float>();
  ExpectCApiRoundTripForType<double>();

  // Explicit codec versions 3..6 via ForVersion entry points.
  std::vector<int> src(24);
  for (int i = 0; i < 24; ++i) {
    src[i] = i * 2 - 5;
  }
  const unsigned int dt_int = CApiDataType<int>();
  for (const int ver : {3, 4, 5, 6}) {
    unsigned int needed = 0;
    ASSERT_EQ(kOk, lerc_computeCompressedSizeForVersion(src.data(), ver, dt_int,
                                                        1, 6, 4, 1, 0, nullptr,
                                                        0.5, &needed));
    std::vector<unsigned char> blob(needed, 0);
    unsigned int written = 0;
    ASSERT_EQ(kOk, lerc_encodeForVersion(src.data(), ver, dt_int, 1, 6, 4, 1, 0,
                                         nullptr, 0.5, blob.data(), needed,
                                         &written));
    EXPECT_EQ(needed, written);

    std::array<unsigned int, kInfoArrSize> info{};
    ASSERT_EQ(kOk, lerc_getBlobInfo(blob.data(), written, info.data(), nullptr,
                                    static_cast<int>(info.size()), 0));
    EXPECT_EQ(static_cast<unsigned int>(ver), info[Idx(InfoArrOrder::version)]);

    std::vector<int> dst(24, -1);
    ASSERT_EQ(kOk, lerc_decode(blob.data(), written, 0, nullptr, 1, 6, 4, 1,
                               dt_int, dst.data()));
    EXPECT_EQ(src, dst);
  }
}

TEST(LercCApiImplTest, BlobInfoPartialOversizedArraysAnd4DNoDataApi) {
  // 2-band, nDepth=2 int image where Band 0 has no noData and Band 1 has a
  // mixed valid/noData pixel -> bUsesNoData triggers zMin/zMax = -1 reset.
  constexpr int kDepth = 2;
  constexpr int kCols = 4;
  constexpr int kRows = 4;
  constexpr int kBands = 2;
  std::vector<int> src(kBands * kRows * kCols * kDepth, 100);
  src[5] = 200;
  // Band 1 pixel 0: mixed (-9999, 100).
  src[kRows * kCols * kDepth + 0] = -9999;
  src[kRows * kCols * kDepth + 3] = 250;

  const std::array<unsigned char, kBands> use_nd = {0, 1};
  const std::array<double, kBands> nd_vals = {0.0, -9999.0};
  const unsigned int dt_int = CApiDataType<int>();

  unsigned int needed = 0;
  ASSERT_EQ(kOk, lerc_computeCompressedSize_4D(
                     src.data(), dt_int, kDepth, kCols, kRows, kBands, 0,
                     nullptr, 2.0, &needed, use_nd.data(), nd_vals.data()));
  std::vector<unsigned char> blob(needed, 0);
  unsigned int written = 0;
  ASSERT_EQ(kOk, lerc_encode_4D(src.data(), dt_int, kDepth, kCols, kRows,
                                kBands, 0, nullptr, 2.0, blob.data(), needed,
                                &written, use_nd.data(), nd_vals.data()));
  EXPECT_EQ(needed, written);

  // Check lerc_getBlobInfo resets dataRangeArray[zMin/zMax] to -1 when
  // nDepth > 1 && nUsesNoDataValue > 0, and supports dataRangeArray-only,
  // partial sizes, and oversized sentinel arrays without overwriting past size.
  {
    std::array<double, 6> ranges{};
    ranges.fill(777.0);
    ASSERT_EQ(kOk, lerc_getBlobInfo(blob.data(), written, nullptr,
                                    ranges.data(), 0, 3));
    EXPECT_DOUBLE_EQ(-1.0, ranges[Idx(DataRangeArrOrder::zMin)]);
    EXPECT_DOUBLE_EQ(-1.0, ranges[Idx(DataRangeArrOrder::zMax)]);
    EXPECT_DOUBLE_EQ(2.0, ranges[Idx(DataRangeArrOrder::maxZErrUsed)]);
    EXPECT_DOUBLE_EQ(777.0, ranges[3]);
    EXPECT_DOUBLE_EQ(777.0, ranges[5]);
  }
  {
    std::array<unsigned int, 16> info{};
    info.fill(0xCAFEBABEU);
    std::array<double, 2> small_ranges{};
    small_ranges.fill(777.0);
    ASSERT_EQ(kOk, lerc_getBlobInfo(blob.data(), written, info.data(),
                                    small_ranges.data(), 3, 2));
    EXPECT_EQ(kCurrentCodecVersion, info[Idx(InfoArrOrder::version)]);
    EXPECT_EQ(dt_int, info[Idx(InfoArrOrder::dataType)]);
    EXPECT_EQ(2U, info[Idx(InfoArrOrder::nDim)]);
    EXPECT_EQ(0xCAFEBABEU, info[3]);
    EXPECT_EQ(0xCAFEBABEU, info[15]);
    EXPECT_DOUBLE_EQ(-1.0, small_ranges[0]);
    EXPECT_DOUBLE_EQ(-1.0, small_ranges[1]);

    // Populate full infoArray via exact kInfoArrSize call.
    ASSERT_EQ(kOk, lerc_getBlobInfo(blob.data(), written, info.data(), nullptr,
                                    static_cast<int>(kInfoArrSize), 0));
    EXPECT_EQ(2U, info[Idx(InfoArrOrder::nUsesNoDataValue)]);
    EXPECT_EQ(0xCAFEBABEU, info[kInfoArrSize]);

    // infoArraySize=0 with non-null infoArray leaves info unchanged while
    // dataRangeArraySize=1 returns only zMin.
    const auto info_snapshot = info;
    std::array<double, 2> one_range = {99.0, 88.0};
    ASSERT_EQ(kOk, lerc_getBlobInfo(blob.data(), written, info.data(),
                                    one_range.data(), 0, 1));
    EXPECT_EQ(info_snapshot, info);
    EXPECT_DOUBLE_EQ(-1.0, one_range[0]);
    EXPECT_DOUBLE_EQ(88.0, one_range[1]);
  }

  // Non-4D getDataRanges / decode / decodeToDouble reject 4D mixed noData
  // with ErrCode::HasNoData, while _4D variants succeed and restore noData.
  std::array<double, kDepth * kBands> mins{};
  std::array<double, kDepth * kBands> maxs{};
  EXPECT_EQ(kHasNoData, lerc_getDataRanges(blob.data(), written, kDepth, kBands,
                                           mins.data(), maxs.data()));
  std::vector<int> dst_i(src.size(), 0);
  EXPECT_EQ(kHasNoData,
            lerc_decode(blob.data(), written, 0, nullptr, kDepth, kCols, kRows,
                        kBands, dt_int, dst_i.data()));
  std::vector<double> dst_d(src.size(), 0.0);
  EXPECT_EQ(kHasNoData,
            lerc_decodeToDouble(blob.data(), written, 0, nullptr, kDepth, kCols,
                                kRows, kBands, dst_d.data()));

  std::array<unsigned char, kBands> dec_uses_nd{};
  std::array<double, kBands> dec_nd_vals{};
  ASSERT_EQ(kOk, lerc_decode_4D(blob.data(), written, 0, nullptr, kDepth, kCols,
                                kRows, kBands, dt_int, dst_i.data(),
                                dec_uses_nd.data(), dec_nd_vals.data()));
  EXPECT_EQ(0, dec_uses_nd[0]);
  EXPECT_EQ(1, dec_uses_nd[1]);
  EXPECT_DOUBLE_EQ(-9999.0, dec_nd_vals[1]);
  EXPECT_EQ(-9999, dst_i[kRows * kCols * kDepth + 0]);

  dec_uses_nd.fill(9);
  dec_nd_vals.fill(123.0);
  ASSERT_EQ(kOk,
            lerc_decodeToDouble_4D(blob.data(), written, 0, nullptr, kDepth,
                                   kCols, kRows, kBands, dst_d.data(),
                                   dec_uses_nd.data(), dec_nd_vals.data()));
  EXPECT_EQ(0, dec_uses_nd[0]);
  EXPECT_EQ(1, dec_uses_nd[1]);
  EXPECT_DOUBLE_EQ(-9999.0, dec_nd_vals[1]);
  EXPECT_DOUBLE_EQ(-9999.0, dst_d[kRows * kCols * kDepth + 0]);
}

TEST(LercCApiImplTest, RejectsInvalidParametersSmallBuffersAndCorruptBlobs) {
  std::vector<int> src(16, 5);
  src[1] = 12;
  std::vector<double> dbl_src(16, 2.5);
  dbl_src[2] = 9.5;
  std::array<unsigned char, 16> mask{};
  mask.fill(1);
  unsigned int num_bytes = 999;
  unsigned int written = 999;
  std::vector<unsigned char> out_buf(256);

  // lerc_computeCompressedSize[_ForVersion / _4D] WrongParam guards.
  EXPECT_EQ(kWrongParam,
            lerc_computeCompressedSize(src.data(), 4, 1, 4, 4, 1, 0, nullptr,
                                       0.5, nullptr));  // blobSize out invalid
  EXPECT_EQ(kWrongParam, lerc_computeCompressedSizeForVersion(
                             nullptr, -1, 4, 1, 4, 4, 1, 0, nullptr, 0.5,
                             &num_bytes));  // pData null
  EXPECT_EQ(0U, num_bytes);
  EXPECT_EQ(kWrongParam, lerc_computeCompressedSizeForVersion(
                             src.data(), -1, 8, 1, 4, 4, 1, 0, nullptr, 0.5,
                             &num_bytes));  // dataType=8 invalid
  EXPECT_EQ(kWrongParam,
            lerc_computeCompressedSizeForVersion(
                src.data(), -1, 4, 1, 4, 4, 2, 1, nullptr, 0.5,
                &num_bytes));  // validBytes null with nMasks=1 invalid
  EXPECT_EQ(kWrongParam,
            lerc_computeCompressedSizeForVersion(
                src.data(), -1, 4, 1, 4, 4, 3, 2, mask.data(), 0.5,
                &num_bytes));  // nMasks=2 != {0,1,nBands=3} invalid
  EXPECT_EQ(kWrongParam,
            lerc_computeCompressedSize_4D(src.data(), 4, 1, 4, 4, 1, 0, nullptr,
                                          0.5, nullptr, nullptr,
                                          nullptr));  // blobSize out null
  EXPECT_EQ(kWrongParam,
            lerc_computeCompressedSize_4D(src.data(), 99, 1, 4, 4, 1, 0,
                                          nullptr, 0.5, &num_bytes, nullptr,
                                          nullptr));  // dataType=99 invalid
  EXPECT_EQ(kWrongParam,
            lerc_computeCompressedSize_4D(
                src.data(), 4, 1, 4, 4, 2, 1, nullptr, 0.5, &num_bytes, nullptr,
                nullptr));  // validBytes null with nMasks=1 invalid

  // lerc_encode[_ForVersion / _4D] WrongParam & BufferTooSmall guards.
  EXPECT_EQ(kWrongParam,
            lerc_encodeForVersion(src.data(), -1, 4, 1, 4, 4, 1, 0, nullptr,
                                  0.5, out_buf.data(), 256,
                                  nullptr));  // nBytesWritten out null invalid
  EXPECT_EQ(kWrongParam,
            lerc_encodeForVersion(src.data(), -1, 4, 1, 4, 4, 1, 0, nullptr,
                                  0.5, nullptr, 256,
                                  &written));  // pOutBuffer null invalid
  EXPECT_EQ(0U, written);
  EXPECT_EQ(kWrongParam,
            lerc_encodeForVersion(src.data(), -1, 4, 1, 4, 4, 2, 1, nullptr,
                                  0.5, out_buf.data(), 256,
                                  &written));  // validBytes null invalid
  EXPECT_EQ(kWrongParam,
            lerc_encode_4D(src.data(), 4, 1, 4, 4, 1, 0, nullptr, 0.5,
                           out_buf.data(), 256, nullptr, nullptr,
                           nullptr));  // nBytesWritten out null invalid
  EXPECT_EQ(kWrongParam,
            lerc_encode_4D(src.data(), 8, 1, 4, 4, 1, 0, nullptr, 0.5,
                           out_buf.data(), 256, &written, nullptr,
                           nullptr));  // dataType=8 invalid
  EXPECT_EQ(kWrongParam,
            lerc_encode_4D(src.data(), 4, 1, 4, 4, 2, 1, nullptr, 0.5,
                           out_buf.data(), 256, &written, nullptr,
                           nullptr));  // validBytes null invalid
  EXPECT_EQ(
      kBufferTooSmall,
      lerc_encode(src.data(), 4, 1, 4, 4, 1, 0, nullptr, 0.5, out_buf.data(), 4,
                  &written));  // outBufferSize=4 too small

  // Build valid int and double blobs to test decode / info / range failures.
  ASSERT_EQ(kOk, lerc_computeCompressedSize(src.data(), 4, 1, 4, 4, 1, 0,
                                            nullptr, 0.5, &num_bytes));
  std::vector<unsigned char> int_blob(num_bytes);
  ASSERT_EQ(kOk, lerc_encode(src.data(), 4, 1, 4, 4, 1, 0, nullptr, 0.5,
                             int_blob.data(), num_bytes, &written));
  ASSERT_EQ(
      kOk, lerc_computeCompressedSize(dbl_src.data(), CApiDataType<double>(), 1,
                                      4, 4, 1, 0, nullptr, 0.0, &num_bytes));
  std::vector<unsigned char> dbl_blob(num_bytes);
  ASSERT_EQ(kOk,
            lerc_encode(dbl_src.data(), CApiDataType<double>(), 1, 4, 4, 1, 0,
                        nullptr, 0.0, dbl_blob.data(), num_bytes, &written));

  // lerc_getBlobInfo & lerc_getDataRanges guards.
  std::array<unsigned int, kInfoArrSize> info{};
  std::array<double, kDataRangeArrSize> ranges{};
  constexpr int kInfoSize = static_cast<int>(kInfoArrSize);
  constexpr int kRangeSize = static_cast<int>(kDataRangeArrSize);
  EXPECT_EQ(kWrongParam,
            lerc_getBlobInfo(nullptr, 10, info.data(), ranges.data(), kInfoSize,
                             kRangeSize));  // pLercBlob null invalid
  EXPECT_EQ(kWrongParam,
            lerc_getBlobInfo(int_blob.data(), 0, info.data(), ranges.data(),
                             kInfoSize, kRangeSize));  // blobSize=0 invalid
  EXPECT_EQ(kWrongParam,
            lerc_getBlobInfo(int_blob.data(), 10, nullptr, nullptr, kInfoSize,
                             kRangeSize));  // both outputs null invalid
  EXPECT_EQ(kWrongParam,
            lerc_getBlobInfo(int_blob.data(), 10, info.data(), ranges.data(), 0,
                             0));  // both output sizes 0 invalid
  const std::array<unsigned char, 16> junk = {1, 2, 3, 4};
  EXPECT_EQ(kFailed, lerc_getBlobInfo(junk.data(), 16, info.data(),
                                      ranges.data(), kInfoSize, kRangeSize));

  std::array<double, 2> mins{};
  std::array<double, 2> maxs{};
  EXPECT_EQ(kWrongParam,
            lerc_getDataRanges(nullptr, 10, 1, 1, mins.data(),
                               maxs.data()));  // pLercBlob null invalid
  EXPECT_EQ(kWrongParam,
            lerc_getDataRanges(int_blob.data(), 10, 0, 1, mins.data(),
                               maxs.data()));  // nDepth=0 invalid
  EXPECT_EQ(kFailed, lerc_getDataRanges(junk.data(), 16, 1, 1, mins.data(),
                                        maxs.data()));

  // lerc_decode_4D & lerc_decodeToDouble_4D WrongParam and Failed guards.
  std::vector<int> dst_i(16, 0);
  std::vector<double> dst_d(20, 0.0);  // sized for max requested 5x4 dims
  EXPECT_EQ(kWrongParam,
            lerc_decode_4D(nullptr, 10, 0, nullptr, 1, 4, 4, 1, 4, dst_i.data(),
                           nullptr, nullptr));  // pLercBlob null invalid
  EXPECT_EQ(kWrongParam,
            lerc_decode_4D(int_blob.data(),
                           static_cast<unsigned int>(int_blob.size()), 0,
                           nullptr, 1, 4, 4, 1, 8, dst_i.data(), nullptr,
                           nullptr));  // dataType=8 invalid
  EXPECT_EQ(kWrongParam,
            lerc_decode_4D(int_blob.data(),
                           static_cast<unsigned int>(int_blob.size()), 1,
                           nullptr, 1, 4, 4, 1, 4, dst_i.data(), nullptr,
                           nullptr));  // validBytes null with nMasks=1 invalid

  EXPECT_EQ(kWrongParam,
            lerc_decodeToDouble_4D(nullptr, 10, 0, nullptr, 1, 4, 4, 1,
                                   dst_d.data(), nullptr,
                                   nullptr));  // pLercBlob null invalid
  EXPECT_EQ(kWrongParam,
            lerc_decodeToDouble_4D(
                int_blob.data(), static_cast<unsigned int>(int_blob.size()), 1,
                nullptr, 1, 4, 4, 1, dst_d.data(), nullptr,
                nullptr));  // validBytes null with nMasks=1 invalid
  EXPECT_EQ(kFailed, lerc_decodeToDouble_4D(junk.data(), 16, 0, nullptr, 1, 4,
                                            4, 1, dst_d.data(), nullptr,
                                            nullptr));  // corrupt header
  // Wrong expected dimensions vs header fails in lerc_decodeToDouble_4D.
  EXPECT_EQ(kFailed,
            lerc_decodeToDouble_4D(
                int_blob.data(), static_cast<unsigned int>(int_blob.size()), 0,
                nullptr, 1, 5, 4, 1, dst_d.data(), nullptr, nullptr));

  // Valid header + corrupted payload checksum triggers Decode failure on both
  // dt == DT_Double and dt != DT_Double paths.
  // MSan safety on dbl_blob decode depends on upstream EncodeHuffman's
  // heuristic for this data.
  auto bad_dbl = dbl_blob;
  bad_dbl.back() ^= 0xFF;
  EXPECT_EQ(kFailed,
            lerc_decodeToDouble_4D(
                bad_dbl.data(), static_cast<unsigned int>(bad_dbl.size()), 0,
                nullptr, 1, 4, 4, 1, dst_d.data(), nullptr, nullptr));

  auto bad_int = int_blob;
  bad_int.back() ^= 0xFF;
  EXPECT_EQ(kFailed,
            lerc_decodeToDouble_4D(
                bad_int.data(), static_cast<unsigned int>(bad_int.size()), 0,
                nullptr, 1, 4, 4, 1, dst_d.data(), nullptr, nullptr));
}

}  // namespace
}  // namespace lerc
