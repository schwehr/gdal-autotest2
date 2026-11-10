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
// Unit tests for LercNS::CntZImage
// (third_party/lerc/LercLib/Lerc1Decode/CntZImage.cpp).
//
// Note: google3 builds with -funsigned-char, so CntZImage::numBytesFlt() and
// readFlt(..., 1) treat 1-byte floats via plain `char` as unsigned [0, 255].
//
// See also:
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc1Decode/CntZImage.h
//   https://github.com/Esri/lerc/blob/master/src/LercLib/Lerc1Decode/CntZImage.cpp

#include "third_party/lerc/LercLib/Lerc1Decode/CntZImage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/lerc/LercLib/BitMask.h"
#include "third_party/lerc/LercLib/Defines.h"
#include "third_party/lerc/LercLib/Lerc1Decode/Image.h"
#include "third_party/lerc/LercLib/RLE.h"
#include "third_party/lerc/google/util/lerc_private_test_util.h"
#include "third_party/lerc/google/util/lerc_test_util.h"

#ifndef HAVE_LERC1_DECODE
#error "Lerc1Decode tests require HAVE_LERC1_DECODE"
#endif

namespace lerc {
namespace {

using ::LercNS::BitMask;
using ::LercNS::Byte;
using ::LercNS::CntZImage;
using ::LercNS::Image;
using ::LercNS::RLE;

static_assert(!std::numeric_limits<char>::is_signed,
              "-funsigned-char assumed by numBytesFlt/readFlt 1-byte pins.");

// Header + cnt part + z part + 1B tile comprFlag vs z-only header size.
constexpr std::size_t kLerc1FullHeaderBytes =
    kLerc1HeaderBytes + 2U * kLerc1PartHeaderBytes + 1U;
constexpr std::size_t kLerc1ZOnlyHeaderBytes =
    kLerc1HeaderBytes + kLerc1PartHeaderBytes + 1U;
static_assert(kLerc1FullHeaderBytes == 67U);
static_assert(kLerc1ZOnlyHeaderBytes == 51U);

class TestableCntZImage final : public CntZImage {
 public:
  using CntZImage::numBytesFlt;
  using CntZImage::readCntTile;
  using CntZImage::readFlt;
  using CntZImage::readTiles;
};

void AppendFltOffset(std::vector<Byte>& out, float offset, int num_bytes) {
  if (num_bytes == 1) {
    out.push_back(static_cast<Byte>(static_cast<int>(offset)));
  } else if (num_bytes == 2) {
    const auto s = static_cast<std::int16_t>(offset);
    AppendPod(out, s);
  } else {
    AppendPod(out, offset);
  }
}

TEST(CntZImageTest, MetadataAndUntiledConstCntWithAllTiledZModes) {
  EXPECT_EQ("CntZImage ", CntZImage().getTypeString());
  EXPECT_EQ(kLerc1FullHeaderBytes,
            CntZImage::computeNumBytesNeededToReadHeader(false));
  EXPECT_EQ(kLerc1ZOnlyHeaderBytes,
            CntZImage::computeNumBytesNeededToReadHeader(true));

  CntZImage img;
  EXPECT_FALSE(img.resizeFill0(0, 4));
  EXPECT_FALSE(img.resizeFill0(4, -1));
  ASSERT_TRUE(img.resizeFill0(4, 4));
  EXPECT_FLOAT_EQ(0.0f, img(3, 3).cnt);
  EXPECT_FLOAT_EQ(0.0f, img(3, 3).z);

  // Build a 4x4 LERC1 blob: untiled constant valid cnt (maxValInImg = 1.0f,
  // setting m_bDecoderCanIgnoreMask = true), and 2x2 tiles for z covering
  // comprFlag 0 (raw floats), 2 (const 0), 3 (const 1B offset), and
  // 1 (2B offset + bit-stuffed uints clamped to maxValInImg = 50.0f), followed
  // by a 2nd band header+z part to exercise onlyHeader and onlyZPart.
  std::vector<Byte> blob;
  AppendLerc1Header(blob, 11, Image::CNT_Z, /*height=*/4, /*width=*/4,
                    /*max_z_error=*/0.25);

  // Untiled const cnt = 1.0f (0 payload bytes).
  AppendLerc1PartHeader(blob, /*tiles_v=*/0, /*tiles_h=*/0, /*num_bytes=*/0,
                        /*max_val=*/1.0f);

  const std::size_t z_nb_off = AppendLerc1PartHeader(
      blob, /*tiles_v=*/2, /*tiles_h=*/2, /*num_bytes=*/0, /*max_val=*/50.0f);
  const std::size_t z_payload = blob.size();

  // Tile (0,0): comprFlag 0, 4 raw floats.
  blob.push_back(0);
  for (const float z : {10.0f, 11.0f, 12.0f, 13.0f}) {
    AppendPod(blob, z);
  }
  // Tile (0,1): comprFlag 2, const 0.
  blob.push_back(2);
  // Tile (1,0): comprFlag 3 with bits67=2 -> 1B offset (200.0f under unsigned
  // char).
  blob.push_back(static_cast<Byte>(3U | kBitStufferOneByteCount));
  AppendFltOffset(blob, 200.0f, 1);
  // Tile (1,1): comprFlag 1 with bits67=1 -> 2B offset (-5.0f) + bit-stuffed
  // {2, 12, 80, 300}; invScale = 0.5 -> {-4.0, 1.0, 35.0, 145->clamped 50.0}.
  blob.push_back(static_cast<Byte>(1U | kBitStufferTwoByteCount));
  AppendFltOffset(blob, -5.0f, 2);
  ASSERT_TRUE(AppendEncodedSimpleV2(blob, {2U, 12U, 80U, 300U}));
  PatchLerc1PartNumBytes(blob, z_nb_off, z_payload);
  const std::size_t band0_end = blob.size();

  // Band 1: header + Z part only; tile (0,0) uses comprFlag 2 (proving
  // readZTile zeroes non-zero pixels without resizeFill0), tiles (0,1)..(1,1)
  // use comprFlag 3 with 4B offset = 7.25f + t.
  AppendLerc1Header(blob, 11, Image::CNT_Z, /*height=*/4, /*width=*/4,
                    /*max_z_error=*/0.25);
  const std::size_t z2_nb_off = AppendLerc1PartHeader(
      blob, /*tiles_v=*/2, /*tiles_h=*/2, /*num_bytes=*/0, /*max_val=*/10.0f);
  const std::size_t z2_payload = blob.size();
  blob.push_back(2);  // Tile (0,0): const 0 overwriting Band 0's {10..13}.
  for (int t = 1; t < 4; ++t) {
    blob.push_back(3);  // bits67 = 0 -> 4B float offset
    AppendFltOffset(blob, 7.25f + static_cast<float>(t), 4);
  }
  PatchLerc1PartNumBytes(blob, z2_nb_off, z2_payload);

  // 1. Header-only read checks maxZErrorInFile against requested maxZError.
  const Byte* ptr = blob.data();
  EXPECT_TRUE(img.read(&ptr, blob.data() + blob.size(), 0.25,
                       /*onlyHeader=*/true, /*onlyZPart=*/false));
  EXPECT_EQ(blob.data() + kLerc1HeaderBytes, ptr);

  // 2. Full Band 0 decode.
  ptr = blob.data();
  ASSERT_TRUE(img.read(&ptr, blob.data() + blob.size(), 0.25,
                       /*onlyHeader=*/false, /*onlyZPart=*/false));
  EXPECT_EQ(blob.data() + band0_end, ptr);
  EXPECT_FLOAT_EQ(1.0f, img(0, 0).cnt);
  EXPECT_FLOAT_EQ(10.0f, img(0, 0).z);
  EXPECT_FLOAT_EQ(13.0f, img(1, 1).z);
  EXPECT_FLOAT_EQ(0.0f, img(0, 2).z);
  EXPECT_FLOAT_EQ(200.0f, img(2, 0).z);
  EXPECT_FLOAT_EQ(-4.0f, img(2, 2).z);
  EXPECT_FLOAT_EQ(1.0f, img(2, 3).z);
  EXPECT_FLOAT_EQ(35.0f, img(3, 2).z);
  EXPECT_FLOAT_EQ(50.0f, img(3, 3).z);  // clamped to maxZInImg

  // 3. Band 1 decode with onlyZPart = true reuses Band 0 cnt mask.
  ASSERT_TRUE(img.read(&ptr, blob.data() + blob.size(), 0.5,
                       /*onlyHeader=*/false, /*onlyZPart=*/true));
  EXPECT_EQ(blob.data() + blob.size(), ptr);
  EXPECT_FLOAT_EQ(0.0f, img(0, 0).z);
  EXPECT_FLOAT_EQ(0.0f, img(0, 1).z);
  EXPECT_FLOAT_EQ(8.25f, img(0, 2).z);
  EXPECT_FLOAT_EQ(9.25f, img(2, 0).z);
  EXPECT_FLOAT_EQ(10.25f, img(2, 2).z);
}

TEST(CntZImageTest, UntiledRleCntWithNonDivisibleTilingAndMaskedZ) {
  // 5x7 image with 2x2 tiles -> tile sizes {2,2,1} vertically x {3,3,1}
  // horizontally, covering remainder tiles (height % 2 != 0, width % 2 != 0).
  constexpr int kHeight = 5;
  constexpr int kWidth = 7;
  BitMask mask(kWidth, kHeight);
  mask.SetAllInvalid();
  mask.SetValid(0, 0);
  mask.SetValid(0, 1);
  mask.SetValid(1, 3);  // in tile (0,1): rows 0..1, cols 3..5
  mask.SetValid(4, 6);  // in remainder tile (2,2): row 4, col 6

  RLE rle;
  Byte* rle_raw = nullptr;
  std::size_t rle_len = 0;
  const bool rle_ok =
      rle.compress(mask.Bits(), mask.Size(), &rle_raw, rle_len, false);
  const std::unique_ptr<Byte[]> rle_owner(rle_raw);
  ASSERT_TRUE(rle_ok);

  std::vector<Byte> blob;
  AppendLerc1Header(blob, 11, Image::CNT_Z, kHeight, kWidth,
                    /*max_z_error=*/0.5);

  // Untiled RLE cnt part (numTilesVert=0, numTilesHori=0, numBytes > 0).
  AppendLerc1PartHeader(blob, 0, 0, static_cast<int>(rle_len), 1.0f);
  blob.insert(blob.end(), rle_owner.get(), rle_owner.get() + rle_len);

  // 2x2 tiled z part => 3x3 actual tiles because kHeight%2==1 and kWidth%2==1.
  const std::size_t z_nb_off =
      AppendLerc1PartHeader(blob, /*tiles_v=*/2, /*tiles_h=*/2, 0,
                            /*max_val=*/99.0f);
  const std::size_t z_payload = blob.size();

  // Tile (0,0) [rows 0..1, cols 0..2]: 2 valid pixels at (0,0),(0,1) ->
  // comprFlag 1 (bits67=0, 4B offset = 1.25f) with
  // m_bDecoderCanIgnoreMask=false.
  blob.push_back(1);
  AppendFltOffset(blob, 1.25f, 4);
  ASSERT_TRUE(
      AppendEncodedSimpleV2(blob, {2U, 5U}));  // 3.25f, 6.25f (invScale=1.0)

  // Tile (0,1) [rows 0..1, cols 3..5]: 1 valid pixel at (1,3) -> raw float.
  blob.push_back(0);
  AppendPod(blob, 42.5f);

  // Tiles (0,2), (1,0), (1,1), (1,2), (2,0), (2,1): no valid pixels -> flag 2.
  for (int t = 0; t < 6; ++t) {
    blob.push_back(2);
  }
  // Remainder tile (2,2) [row 4, col 6]: 1 valid pixel -> 1B offset = 15.0f.
  blob.push_back(static_cast<Byte>(3U | kBitStufferOneByteCount));
  AppendFltOffset(blob, 15.0f, 1);
  PatchLerc1PartNumBytes(blob, z_nb_off, z_payload);

  CntZImage img;
  const Byte* ptr = blob.data();
  ASSERT_TRUE(img.read(&ptr, blob.data() + blob.size(), 0.5));
  EXPECT_EQ(blob.data() + blob.size(), ptr);
  EXPECT_FLOAT_EQ(1.0f, img(0, 0).cnt);
  EXPECT_FLOAT_EQ(3.25f, img(0, 0).z);
  EXPECT_FLOAT_EQ(6.25f, img(0, 1).z);
  EXPECT_FLOAT_EQ(0.0f, img(0, 2).cnt);
  EXPECT_FLOAT_EQ(42.5f, img(1, 3).z);
  EXPECT_FLOAT_EQ(15.0f, img(4, 6).z);
}

TEST(CntZImageTest, TiledCntAllCompressionFlagsAndBitStuffedCnt) {
  // 1. 4x4 image with 2x2 tiled cnt covering flags 0 (raw floats), 2 (const 0),
  // 3 (const -1), and 4 (const +1).
  {
    std::vector<Byte> blob;
    AppendLerc1Header(blob, 11, Image::CNT_Z, 4, 4, 0.5);
    const std::size_t cnt_off = AppendLerc1PartHeader(blob, 2, 2, 0, 2.0f);
    const std::size_t cnt_payload = blob.size();
    blob.push_back(0);
    for (const float c : {1.0f, 0.0f, 2.0f, 1.0f}) {
      AppendPod(blob, c);
    }
    blob.push_back(2);  // const 0
    blob.push_back(3);  // const -1
    blob.push_back(4);  // const +1
    PatchLerc1PartNumBytes(blob, cnt_off, cnt_payload);

    // 1x1 tiled const-0 z part.
    AppendLerc1PartHeader(blob, 1, 1, 1, 0.0f);
    blob.push_back(2);

    CntZImage img;
    const Byte* ptr = blob.data();
    ASSERT_TRUE(img.read(&ptr, blob.data() + blob.size(), 0.5));
    EXPECT_EQ(blob.data() + blob.size(), ptr);
    EXPECT_FLOAT_EQ(2.0f, img(1, 0).cnt);
    EXPECT_FLOAT_EQ(0.0f, img(0, 2).cnt);
    EXPECT_FLOAT_EQ(-1.0f, img(2, 0).cnt);
    EXPECT_FLOAT_EQ(1.0f, img(2, 2).cnt);
  }

  // 2. 2x2 image with tiled bit-stuffed cnt (comprFlag 1, 1B offset + uints).
  {
    std::vector<Byte> blob;
    AppendLerc1Header(blob, 11, Image::CNT_Z, 2, 2, 0.5);
    const std::size_t cnt_off = AppendLerc1PartHeader(blob, 1, 1, 0, 3.0f);
    const std::size_t cnt_payload = blob.size();
    blob.push_back(
        static_cast<Byte>(1U | kBitStufferOneByteCount));  // 1B offset = 1.0f
    AppendFltOffset(blob, 1.0f, 1);
    ASSERT_TRUE(AppendEncodedSimpleV2(blob, {0U, 1U, 2U, 0U}));
    PatchLerc1PartNumBytes(blob, cnt_off, cnt_payload);

    AppendLerc1PartHeader(blob, 1, 1, 1, 0.0f);
    blob.push_back(2);

    CntZImage img;
    const Byte* ptr = blob.data();
    ASSERT_TRUE(img.read(&ptr, blob.data() + blob.size(), 0.5));
    EXPECT_EQ(blob.data() + blob.size(), ptr);
    EXPECT_FLOAT_EQ(1.0f, img(0, 0).cnt);
    EXPECT_FLOAT_EQ(2.0f, img(0, 1).cnt);
    EXPECT_FLOAT_EQ(3.0f, img(1, 0).cnt);
  }

  // 3. Untiled const cnt == 0.0f (all invalid) with z flag 3 offset 5.0f:
  //    mask must stay honored so z remains 0.0f. (Avoid z flag 1 with zero
  //    valid pixels because upstream readZTile indexes &dataVec[0] when
  //    dataVec is empty, triggering hardened libc++ abort.)
  {
    std::vector<Byte> blob;
    AppendLerc1Header(blob, 11, Image::CNT_Z, 2, 2, 0.5);
    AppendLerc1PartHeader(blob, 0, 0, 0, 0.0f);  // untiled all-invalid cnt=0
    AppendLerc1PartHeader(blob, 1, 1, 5, 5.0f);
    blob.push_back(3);  // 4B offset = 5.0f
    AppendFltOffset(blob, 5.0f, 4);

    CntZImage img;
    const Byte* ptr = blob.data();
    ASSERT_TRUE(img.read(&ptr, blob.data() + blob.size(), 0.5));
    EXPECT_EQ(blob.data() + blob.size(), ptr);
    for (int r = 0; r < 2; ++r) {
      for (int c = 0; c < 2; ++c) {
        EXPECT_FLOAT_EQ(0.0f, img(r, c).cnt);
        EXPECT_FLOAT_EQ(0.0f, img(r, c).z);
      }
    }
  }
}

TEST(CntZImageTest, RejectsInvalidHeadersParametersTilesAndTruncations) {
  CntZImage img;
  EXPECT_FALSE(img.read(nullptr, nullptr, 1.0));
  const Byte* null_ptr = nullptr;
  EXPECT_FALSE(img.read(&null_ptr, nullptr, 1.0));

  // Build one minimal valid 2x2 LERC1 blob for mutation and truncation tests.
  std::vector<Byte> valid;
  AppendLerc1Header(valid, 11, Image::CNT_Z, 2, 2, 0.5);
  AppendLerc1PartHeader(valid, 0, 0, 0, 1.0f);
  AppendLerc1PartHeader(valid, 1, 1, 1, 0.0f);
  valid.push_back(2);

  // Every truncated length strictly less than full blob fails cleanly.
  for (std::size_t trunc = 0; trunc < valid.size(); ++trunc) {
    const Byte* ptr = valid.data();
    EXPECT_FALSE(img.read(&ptr, valid.data() + trunc, 0.5))
        << "trunc=" << trunc;
  }

  // Bad magic, version, type, dimensions, and maxZError.
  {
    auto bad = valid;
    bad[0] = 'X';
    const Byte* ptr = bad.data();
    EXPECT_FALSE(img.read(&ptr, bad.data() + bad.size(), 1.0));
  }
  {
    auto bad = valid;
    WritePodAt(bad, kLerc1VersionOffset, 10);  // version != 11
    const Byte* ptr = bad.data();
    EXPECT_FALSE(img.read(&ptr, bad.data() + bad.size(), 1.0));
  }
  {
    auto bad = valid;
    WritePodAt(bad, kLerc1TypeOffset,
               static_cast<int>(Image::FLOAT));  // wrong type
    const Byte* ptr = bad.data();
    EXPECT_FALSE(img.read(&ptr, bad.data() + bad.size(), 1.0));
  }
  {
    auto bad = valid;
    WritePodAt(bad, kLerc1HeightOffset, 40001);  // height > 40000
    const Byte* ptr = bad.data();
    EXPECT_FALSE(img.read(&ptr, bad.data() + bad.size(), 1.0));
  }
  {
    auto bad = valid;
    WritePodAt(bad, kLerc1HeightOffset, 40000);
    WritePodAt(bad, kLerc1WidthOffset, 40000);  // sizeof(CntZ) * 40000^2 > max
    const Byte* ptr = bad.data();
    EXPECT_FALSE(img.read(&ptr, bad.data() + bad.size(), 1.0));
  }
  {
    const Byte* ptr = valid.data();
    EXPECT_FALSE(img.read(&ptr, valid.data() + valid.size(), 0.25));  // tol
  }

  // Corrupt RLE cnt payload.
  {
    std::vector<Byte> bad;
    AppendLerc1Header(bad, 11, Image::CNT_Z, 2, 2, 0.5);
    AppendLerc1PartHeader(bad, 0, 0, 2, 1.0f);
    bad.push_back(0x7F);
    bad.push_back(0x7F);
    AppendLerc1PartHeader(bad, 1, 1, 1, 0.0f);
    bad.push_back(2);
    const Byte* ptr = bad.data();
    EXPECT_FALSE(img.read(&ptr, bad.data() + bad.size(), 1.0));
  }

  // Invalid readTiles dimensions, invalid cnt/z comprFlags, and invalid
  // readFlt width in comprFlag bits 6..7. Note: readZTile flag 1 lacks a
  // dataVec.size() >= numValid check (unlike readCntTile), so positive z tests
  // must always match element counts.
  {
    auto bad = valid;
    WritePodAt(bad, kLerc1ZTilesVOffset, 0);  // z numTilesVert = 0
    const Byte* ptr = bad.data();
    EXPECT_FALSE(img.read(&ptr, bad.data() + bad.size(), 1.0));
  }
  {
    auto bad = valid;
    WritePodAt(bad, kLerc1ZTilesHOffset, 0);  // z numTilesHori = 0
    const Byte* ptr = bad.data();
    EXPECT_FALSE(img.read(&ptr, bad.data() + bad.size(), 1.0));
  }
  {
    auto bad = valid;
    bad.back() = 4;  // z comprFlag = 4 (> 3)
    const Byte* ptr = bad.data();
    EXPECT_FALSE(img.read(&ptr, bad.data() + bad.size(), 1.0));
  }
  {
    auto bad = valid;
    bad.back() =
        static_cast<Byte>(1U | kBitStufferInvalidCount);  // z bits67=3 -> 0B
    const Byte* ptr = bad.data();
    EXPECT_FALSE(img.read(&ptr, bad.data() + bad.size(), 1.0));
  }
  {
    std::vector<Byte> bad_cnt;
    AppendLerc1Header(bad_cnt, 11, Image::CNT_Z, 2, 2, 0.5);
    AppendLerc1PartHeader(bad_cnt, 1, 1, 1, 1.0f);
    bad_cnt.push_back(5);  // cnt comprFlag = 5 (> 4)
    const Byte* ptr = bad_cnt.data();
    EXPECT_FALSE(img.read(&ptr, bad_cnt.data() + bad_cnt.size(), 1.0));
  }
  {
    std::vector<Byte> bad_cnt_flt;
    AppendLerc1Header(bad_cnt_flt, 11, Image::CNT_Z, 2, 2, 0.5);
    AppendLerc1PartHeader(bad_cnt_flt, 1, 1, 1, 1.0f);
    bad_cnt_flt.push_back(
        static_cast<Byte>(1U | kBitStufferInvalidCount));  // 0B readFlt
    const Byte* ptr = bad_cnt_flt.data();
    EXPECT_FALSE(img.read(&ptr, bad_cnt_flt.data() + bad_cnt_flt.size(), 1.0));
  }

  // Protected helpers: -funsigned-char gives [0, 255] for 1-byte floats.
  EXPECT_EQ(1, TestableCntZImage::numBytesFlt(200.0f));
  EXPECT_EQ(2, TestableCntZImage::numBytesFlt(-5.0f));
  EXPECT_EQ(4, TestableCntZImage::numBytesFlt(1.5f));

  const std::array<Byte, 4> dummy = {1, 2, 3, 4};
  const Byte* uptr = dummy.data();
  float z = 0.0f;
  EXPECT_FALSE(TestableCntZImage::readFlt(&uptr, z, 3));

  TestableCntZImage timg;
  ASSERT_TRUE(timg.resizeFill0(2, 2));
  EXPECT_FALSE(timg.readCntTile(&uptr, 1, 1, 0, 1));
  EXPECT_FALSE(timg.readTiles(true, 1.0, 3, 1, 1.0f, dummy.data()));
}

}  // namespace
}  // namespace lerc
