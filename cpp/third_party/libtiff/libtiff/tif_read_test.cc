// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for libtiff strip, tile, scanline, raw, and user-buffer read
// routines (libtiff/tif_read.c).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_read.c

#include <fcntl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

// RAII owner for buffers allocated via _TIFFmallocExt/_TIFFcallocExt inside
// _TIFFReadEncodedStripAndAllocBuffer and _TIFFReadEncodedTileAndAllocBuffer.
// Declare after UniqueTiff so _TIFFfreeExt runs before TIFFClose.
class ScopedTiffExtBuffer {
 public:
  explicit ScopedTiffExtBuffer(TIFF* tif) : tif_(tif) {}
  ~ScopedTiffExtBuffer() { Reset(); }

  ScopedTiffExtBuffer(const ScopedTiffExtBuffer&) = delete;
  ScopedTiffExtBuffer& operator=(const ScopedTiffExtBuffer&) = delete;

  void** out_ptr() { return &buf_; }
  void* get() const { return buf_; }
  const uint8_t* bytes() const { return static_cast<const uint8_t*>(buf_); }

  void Reset() noexcept {
    if (buf_ != nullptr) {
      _TIFFfreeExt(tif_, buf_);
      buf_ = nullptr;
    }
  }

 private:
  TIFF* tif_;
  void* buf_ = nullptr;
};

// Reverses bits in a single byte (MSB<->LSB) matching TIFFReverseBits.
constexpr uint8_t ReverseByteBits(uint8_t v) {
  uint8_t out = 0;
  for (int i = 0; i < 8; ++i) {
    out = static_cast<uint8_t>((out << 1) | ((v >> i) & 1U));
  }
  return out;
}

// Writes a striped image into `stream`.
void WriteStripedImage(MemoryTiffStream& stream, const char* write_mode,
                       uint32_t width, uint32_t height, uint16_t bps,
                       uint16_t spp, uint32_t rows_per_strip,
                       uint16_t compression, uint16_t planar_config,
                       uint16_t fill_order, absl::Span<const uint8_t> pixels) {
  UniqueTiff writer = stream.Open(write_mode);
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, bps), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, spp), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, rows_per_strip),
            1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, compression), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_FILLORDER, fill_order), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                         spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK),
            1);

  const tmsize_t row_bytes = TIFFScanlineSize(writer.get());
  ASSERT_GT(row_bytes, 0);
  const uint16_t planes = (planar_config == PLANARCONFIG_SEPARATE) ? spp : 1;
  ASSERT_EQ(pixels.size(), static_cast<size_t>(row_bytes) * height * planes);

  // TIFFWriteScanline mutates its input buffer in place during tif_postdecode.
  std::vector<uint8_t> row_buf(static_cast<size_t>(row_bytes));
  for (uint16_t s = 0; s < planes; ++s) {
    for (uint32_t y = 0; y < height; ++y) {
      const size_t offset = (static_cast<size_t>(s) * height + y) *
                            static_cast<size_t>(row_bytes);
      std::memcpy(row_buf.data(), pixels.data() + offset,
                  static_cast<size_t>(row_bytes));
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), y, s), 1);
    }
  }
  ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  writer.reset();
}

// Writes a tiled image (supports 2D or 3D via image_depth / tile_depth).
void WriteTiledImage(MemoryTiffStream& stream, const char* write_mode,
                     uint32_t width, uint32_t height, uint32_t depth,
                     uint32_t tile_w, uint32_t tile_h, uint32_t tile_d,
                     uint16_t bps, uint16_t spp, uint16_t compression,
                     uint16_t planar_config, uint16_t fill_order,
                     absl::Span<const std::vector<uint8_t>> tile_payloads) {
  UniqueTiff writer = stream.Open(write_mode);
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, height), 1);
  if (depth > 1) {
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDEPTH, depth), 1);
  }
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, tile_w), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, tile_h), 1);
  if (tile_d > 1) {
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEDEPTH, tile_d), 1);
  }
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, bps), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, spp), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, compression), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_FILLORDER, fill_order), 1);
  ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                         spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK),
            1);

  const uint32_t ntiles = TIFFNumberOfTiles(writer.get());
  ASSERT_EQ(tile_payloads.size(), ntiles);
  const tmsize_t tile_size = TIFFTileSize(writer.get());
  ASSERT_GT(tile_size, 0);

  std::vector<uint8_t> mutable_tile(static_cast<size_t>(tile_size));
  for (uint32_t t = 0; t < ntiles; ++t) {
    ASSERT_EQ(tile_payloads[t].size(), static_cast<size_t>(tile_size));
    std::memcpy(mutable_tile.data(), tile_payloads[t].data(),
                static_cast<size_t>(tile_size));
    ASSERT_EQ(
        TIFFWriteEncodedTile(writer.get(), t, mutable_tile.data(), tile_size),
        tile_size);
  }
  ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  writer.reset();
}

// Builds a minimal ClassicTIFF or BigTIFF striped/tiled file via RawTiffBuilder
// with caller-controlled offsets, bytecounts, or dimensions.
std::vector<uint8_t> BuildRawStripedTiff(
    bool big_endian, bool big_tiff, uint32_t width, uint32_t height,
    uint32_t rows_per_strip, uint16_t compression, uint16_t bps, uint16_t spp,
    uint16_t fill_order, absl::Span<const uint8_t> strip_blob,
    std::optional<uint64_t> override_offset = std::nullopt,
    std::optional<uint64_t> override_bytecount = std::nullopt) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const uint64_t blob_off =
      override_offset.value_or(builder.AppendBlob(strip_blob));
  const uint64_t bytecount = override_bytecount.value_or(strip_blob.size());

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bps),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeShort(TIFFTAG_FILLORDER, fill_order),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, spp),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
  };
  if (big_tiff) {
    entries.push_back(builder.MakeLong8(TIFFTAG_STRIPOFFSETS, blob_off));
    entries.push_back(builder.MakeLong8(TIFFTAG_STRIPBYTECOUNTS, bytecount));
  } else {
    entries.push_back(builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                                       static_cast<uint32_t>(blob_off)));
    entries.push_back(builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                                       static_cast<uint32_t>(bytecount)));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

// Builds a 2-strip ClassicTIFF or BigTIFF file so tif_dirread's single-strip
// ByteCountLooksBad() auto-repair does not overwrite crafted bytecounts.
std::vector<uint8_t> BuildRawTwoStripTiff(
    bool big_endian, bool big_tiff, uint32_t width, uint32_t height,
    uint32_t rows_per_strip, uint16_t compression, uint16_t bps, uint16_t spp,
    uint16_t fill_order, absl::Span<const uint8_t> strip_blob,
    std::array<uint64_t, 2> bytecounts,
    std::optional<std::array<uint64_t, 2>> override_offsets = std::nullopt) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const uint64_t off0 = builder.AppendBlob(strip_blob);
  const uint64_t off1 = builder.AppendBlob(strip_blob);
  const std::array<uint64_t, 2> offsets =
      override_offsets.value_or(std::array<uint64_t, 2>{off0, off1});

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bps),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeShort(TIFFTAG_FILLORDER, fill_order),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, spp),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
  };
  if (big_tiff) {
    entries.push_back(builder.MakeLong8Array(TIFFTAG_STRIPOFFSETS, offsets));
    entries.push_back(
        builder.MakeLong8Array(TIFFTAG_STRIPBYTECOUNTS, bytecounts));
  } else {
    const std::array<uint32_t, 2> off32 = {static_cast<uint32_t>(offsets[0]),
                                           static_cast<uint32_t>(offsets[1])};
    const std::array<uint32_t, 2> bc32 = {static_cast<uint32_t>(bytecounts[0]),
                                          static_cast<uint32_t>(bytecounts[1])};
    entries.push_back(builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, off32));
    entries.push_back(builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, bc32));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint8_t> BuildRawTiledTiff(
    bool big_endian, bool big_tiff, uint32_t width, uint32_t height,
    uint32_t tile_w, uint32_t tile_h, uint16_t compression, uint16_t bps,
    uint16_t spp, uint16_t fill_order, absl::Span<const uint8_t> tile_blob,
    std::optional<uint64_t> override_offset = std::nullopt,
    std::optional<uint64_t> override_bytecount = std::nullopt) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const uint64_t blob_off =
      override_offset.value_or(builder.AppendBlob(tile_blob));
  const uint64_t bytecount = override_bytecount.value_or(tile_blob.size());

  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bps),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeShort(TIFFTAG_FILLORDER, fill_order),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, spp),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeLong(TIFFTAG_TILEWIDTH, tile_w),
      builder.MakeLong(TIFFTAG_TILELENGTH, tile_h),
  };
  if (big_tiff) {
    entries.push_back(builder.MakeLong8(TIFFTAG_TILEOFFSETS, blob_off));
    entries.push_back(builder.MakeLong8(TIFFTAG_TILEBYTECOUNTS, bytecount));
  } else {
    entries.push_back(
        builder.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(blob_off)));
    entries.push_back(builder.MakeLong(TIFFTAG_TILEBYTECOUNTS,
                                       static_cast<uint32_t>(bytecount)));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

// Configurable custom codec used to test TIFF_NOREADRAW, setupdecode failure,
// predecode failure, decoderow/strip/tile failure, and compression ratio
// checks.
struct CustomCodecBehavior {
  bool set_noreadraw = false;
  bool setupdecode_ok = true;
  bool predecode_ok = true;
  bool decode_ok = true;
  bool seek_ok = true;
  uint64_t max_compression_ratio = 0;
  int postdecode_calls = 0;
};

CustomCodecBehavior* g_codec_behavior = nullptr;

int CustomSetupDecode(TIFF* /*tif*/) {
  return (g_codec_behavior != nullptr && !g_codec_behavior->setupdecode_ok) ? 0
                                                                            : 1;
}

int CustomPreDecode(TIFF* /*tif*/, uint16_t /*sample*/) {
  return (g_codec_behavior != nullptr && !g_codec_behavior->predecode_ok) ? 0
                                                                          : 1;
}

int CustomDecode(TIFF* tif, uint8_t* buf, tmsize_t cc, uint16_t /*sample*/) {
  if (g_codec_behavior != nullptr && !g_codec_behavior->decode_ok) {
    return 0;
  }
  if (tif->tif_rawcp != nullptr && tif->tif_rawcc >= cc) {
    std::memcpy(buf, tif->tif_rawcp, static_cast<size_t>(cc));
    tif->tif_rawcp += cc;
    tif->tif_rawcc -= cc;
  } else if (buf != nullptr && cc > 0) {
    std::memset(buf, 0x5A, static_cast<size_t>(cc));
  }
  return 1;
}

int CustomSeek(TIFF* /*tif*/, uint32_t /*nrows*/) {
  return (g_codec_behavior != nullptr && !g_codec_behavior->seek_ok) ? 0 : 1;
}

void CustomPostDecode(TIFF* /*tif*/, uint8_t* /*buf*/, tmsize_t /*cc*/) {
  if (g_codec_behavior != nullptr) {
    ++g_codec_behavior->postdecode_calls;
  }
}

uint64_t CustomGetMaxCompressionRatio(TIFF* /*tif*/) {
  return g_codec_behavior != nullptr ? g_codec_behavior->max_compression_ratio
                                     : 0;
}

int CustomCodecInit(TIFF* tif, int /*scheme*/) {
  if (g_codec_behavior != nullptr && g_codec_behavior->set_noreadraw) {
    tif->tif_flags |= TIFF_NOREADRAW;
  }
  tif->tif_setupdecode = &CustomSetupDecode;
  tif->tif_predecode = &CustomPreDecode;
  tif->tif_decoderow = &CustomDecode;
  tif->tif_decodestrip = &CustomDecode;
  tif->tif_decodetile = &CustomDecode;
  tif->tif_seek = &CustomSeek;
  tif->tif_postdecode = &CustomPostDecode;
  tif->tif_getmaxcompressionratio = &CustomGetMaxCompressionRatio;
  return 1;
}

constexpr uint16_t kTestCodecScheme = 65001;

TEST(TifReadTest, ReadScanlineContigMappedAndUnmapped) {
  constexpr uint32_t kW = 5;
  constexpr uint32_t kH = 6;
  constexpr uint16_t kSpp = 3;
  std::vector<uint8_t> pixels(kW * kH * kSpp);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<uint8_t>((i * 13 + 7) & 0xff);
  }

  MemoryTiffStream stream;
  WriteStripedImage(stream, "w", kW, kH, 8, kSpp, /*rows_per_strip=*/2,
                    COMPRESSION_NONE, PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB,
                    pixels);
  stream.set_enable_mmap(true);

  for (const char* mode : {"rm", "rM"}) {
    SCOPED_TRACE(mode);
    UniqueTiff reader = stream.Open(mode);
    ASSERT_NE(reader, nullptr);
    const tmsize_t row_size = TIFFScanlineSize(reader.get());
    ASSERT_EQ(row_size, static_cast<tmsize_t>(kW * kSpp));

    std::vector<uint8_t> row(static_cast<size_t>(row_size));
    for (uint32_t y = 0; y < kH; ++y) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), y, 0), 1);
      const size_t off = static_cast<size_t>(y) * static_cast<size_t>(row_size);
      EXPECT_THAT(row,
                  ::testing::ElementsAreArray(pixels.data() + off, row_size));
    }
  }
}

TEST(TifReadTest, ReadScanlineSeparatePlanesAndSeekPatterns) {
  constexpr uint32_t kW = 4;
  constexpr uint32_t kH = 6;
  constexpr uint16_t kSpp = 3;
  std::vector<uint8_t> pixels(kW * kH * kSpp);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<uint8_t>(i + 10);
  }

  MemoryTiffStream stream;
  WriteStripedImage(stream, "w", kW, kH, 8, kSpp, /*rows_per_strip=*/4,
                    COMPRESSION_NONE, PLANARCONFIG_SEPARATE, FILLORDER_MSB2LSB,
                    pixels);

  UniqueTiff reader = stream.Open("rm");
  ASSERT_NE(reader, nullptr);
  const tmsize_t row_size = TIFFScanlineSize(reader.get());
  ASSERT_EQ(row_size, static_cast<tmsize_t>(kW));
  std::vector<uint8_t> row(static_cast<size_t>(row_size));

  // Forward skip within strip (reads row 0, skips row 1, reads row 2).
  ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
  EXPECT_THAT(row, ::testing::ElementsAreArray(pixels.data(), row_size));
  ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), 2, 0), 1);
  EXPECT_THAT(row,
              ::testing::ElementsAreArray(pixels.data() + 2 * kW, row_size));

  // Backward seek within same strip (row < td_row calls TIFFStartStrip).
  ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), 1, 0), 1);
  EXPECT_THAT(row,
              ::testing::ElementsAreArray(pixels.data() + 1 * kW, row_size));

  // Read across separate sample planes and strips.
  for (uint16_t s = 0; s < kSpp; ++s) {
    for (uint32_t y = 0; y < kH; ++y) {
      SCOPED_TRACE(::testing::Message() << "s=" << s << " y=" << y);
      ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), y, s), 1);
      const size_t off = (static_cast<size_t>(s) * kH + y) * kW;
      EXPECT_THAT(row,
                  ::testing::ElementsAreArray(pixels.data() + off, row_size));
    }
  }
}

TEST(TifReadTest, ReadScanlineErrorsAndSeekFailure) {
  // 1. Write-only handle (O_WRONLY via TIFFSetMode) fails TIFFCheckRead and
  // TIFFReadFromUserBuffer. Note "w" opens with O_RDWR | O_CREAT | O_TRUNC.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 4), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 4), 1);
    const int saved_mode = TIFFSetMode(writer.get(), O_WRONLY);
    auto restore_mode =
        absl::MakeCleanup([&] { TIFFSetMode(writer.get(), saved_mode); });
    std::array<uint8_t, 4> buf{};
    std::array<uint8_t, 4> inbuf{};
    EXPECT_EQ(TIFFReadScanline(writer.get(), buf.data(), 0, 0), -1);
    EXPECT_EQ(TIFFReadEncodedStrip(writer.get(), 0, buf.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadRawStrip(writer.get(), 0, buf.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadTile(writer.get(), buf.data(), 0, 0, 0, 0),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadEncodedTile(writer.get(), 0, buf.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadRawTile(writer.get(), 0, buf.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(
        TIFFReadFromUserBuffer(writer.get(), 0, inbuf.data(), 4, buf.data(), 4),
        0);
  }

  // 2. Tiled image fails TIFFCheckRead(tif, 0).
  {
    MemoryTiffStream stream;
    std::vector<std::vector<uint8_t>> tiles(1, std::vector<uint8_t>(256, 1));
    WriteTiledImage(stream, "w", 16, 16, 1, 16, 16, 1, 8, 1, COMPRESSION_NONE,
                    PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, tiles);
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 16> buf{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), buf.data(), 0, 0), -1);
  }

  // 3. Out-of-range row, out-of-range sample, null buf, and decode error.
  {
    std::vector<uint8_t> pixels(4 * 4 * 2, 0x33);
    MemoryTiffStream stream;
    WriteStripedImage(stream, "w", 4, 4, 8, 2, 4, COMPRESSION_NONE,
                      PLANARCONFIG_SEPARATE, FILLORDER_MSB2LSB, pixels);

    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> buf(4, 0xAA);
    EXPECT_EQ(TIFFReadScanline(reader.get(), buf.data(), 4, 0), -1);
    EXPECT_THAT(buf, ::testing::Each(0));

    EXPECT_EQ(TIFFReadScanline(reader.get(), nullptr, 4, 0), -1);

    std::fill(buf.begin(), buf.end(), 0xBB);
    EXPECT_EQ(TIFFReadScanline(reader.get(), buf.data(), 0, 2), -1);
    EXPECT_THAT(buf, ::testing::Each(0));

    // Corrupt tif_seek failure via PackBits (does not support random row seek).
    MemoryTiffStream pack_stream;
    WriteStripedImage(pack_stream, "w", 4, 4, 8, 1, 4, COMPRESSION_PACKBITS,
                      PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB,
                      std::vector<uint8_t>(16, 0x42));
    UniqueTiff pack_reader = pack_stream.Open("rm");
    ASSERT_NE(pack_reader, nullptr);
    // Reading row 2 directly requires skipping rows 0 and 1 via tif_seek.
    std::fill(buf.begin(), buf.end(), 0xCC);
    EXPECT_EQ(TIFFReadScanline(pack_reader.get(), buf.data(), 2, 0), -1);
    EXPECT_THAT(buf, ::testing::Each(0));
  }

  // 4. RowsPerStrip == 0 in TIFFSeek, and decoderow failure.
  {
    std::vector<uint8_t> blob(16, 0x11);
    MemoryTiffStream stream(BuildRawStripedTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 4, 4, /*rows_per_strip=*/4,
        COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob));
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    const uint32_t saved_rps = reader->tif_dir.td_rowsperstrip;
    auto restore =
        absl::MakeCleanup([&] { reader->tif_dir.td_rowsperstrip = saved_rps; });
    reader->tif_dir.td_rowsperstrip = 0;
    std::array<uint8_t, 4> buf{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), buf.data(), 0, 0), -1);
  }
  {
    // Strip data too small for uncompressed scanlines -> DumpModeDecode fails.
    std::vector<uint8_t> blob(2, 0x11);
    MemoryTiffStream stream(BuildRawTwoStripTiff(
        /*big_endian=*/false, /*big_tiff=*/false, 4, 4, /*rows_per_strip=*/2,
        COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob, {2ULL, 2ULL}));
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 4> buf{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), buf.data(), 0, 0), -1);
  }
  {
    // Read error in TIFFFillStrip while seeking inside TIFFReadScanline.
    std::vector<uint8_t> blob(16, 0x22);
    MemoryTiffStream stream(BuildRawStripedTiff(false, false, 4, 4, 4,
                                                COMPRESSION_NONE, 8, 1,
                                                FILLORDER_MSB2LSB, blob));
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    stream.FailReadsAfter(0);
    std::array<uint8_t, 4> buf{};
    buf.fill(0xAA);
    EXPECT_EQ(TIFFReadScanline(reader.get(), buf.data(), 0, 0), -1);
    EXPECT_THAT(buf, ::testing::Each(0));
    stream.ResetFaults();
  }
}

TEST(TifReadTest, ReadEncodedStripShortcutBitRevAndErrors) {
  constexpr uint32_t kW = 4;
  constexpr uint32_t kH = 5;
  std::vector<uint8_t> pixels(kW * kH);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<uint8_t>(0x10 + i);
  }

  // 2 strips: strip 0 has 3 rows (12 bytes), strip 1 has 2 rows (8 bytes).
  MemoryTiffStream stream;
  WriteStripedImage(stream, "w", kW, kH, 8, 1, /*rows_per_strip=*/3,
                    COMPRESSION_NONE, PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB,
                    pixels);

  // Unmapped shortcut path (size >= stripsize).
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> strip0(12);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip0.data(), 12), 12);
    EXPECT_THAT(strip0, ::testing::ElementsAreArray(pixels.data(), 12));

    std::vector<uint8_t> strip1(8);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 1, strip1.data(), 8), 8);
    EXPECT_THAT(strip1, ::testing::ElementsAreArray(pixels.data() + 12, 8));

    // Non-shortcut via size == -1 and size < stripsize.
    std::vector<uint8_t> partial(5);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, partial.data(), 5), 5);
    EXPECT_THAT(partial, ::testing::ElementsAreArray(pixels.data(), 5));

    std::fill(strip1.begin(), strip1.end(), 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 1, strip1.data(), -1), 8);
    EXPECT_THAT(strip1, ::testing::ElementsAreArray(pixels.data() + 12, 8));

    // Out-of-range strip.
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 2, strip0.data(), 12),
              static_cast<tmsize_t>(-1));
  }

  // Shortcut path with bit reversal (opening MSB2LSB file with "rmL").
  {
    UniqueTiff reader = stream.Open("rmL");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> strip0(12);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip0.data(), 12), 12);
    for (size_t i = 0; i < strip0.size(); ++i) {
      EXPECT_EQ(strip0[i], ReverseByteBits(pixels[i]));
    }
  }

  // Shortcut path read failure (FailReadsAfter during TIFFReadRawStrip1).
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    stream.FailReadsAfter(0);
    std::vector<uint8_t> strip0(12);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip0.data(), 12),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();
  }

  // Non-shortcut with TIFFFillStrip failure (both non-null and null buf).
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    stream.FailReadsAfter(0);
    std::vector<uint8_t> strip0(12, 0xFF);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip0.data(), -1),
              static_cast<tmsize_t>(-1));
    EXPECT_THAT(strip0, ::testing::Each(0));
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, nullptr, -1),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();
  }

  // rowsperstrip > imagelength clamping and rowsperstrip == 0 failure.
  {
    std::vector<uint8_t> blob(16, 0x77);
    MemoryTiffStream big_rps(
        BuildRawStripedTiff(false, false, 4, 4, /*rows_per_strip=*/100,
                            COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob));
    UniqueTiff reader = big_rps.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), -1), 16);
    EXPECT_THAT(out, ::testing::Each(0x77));
  }
  {
    std::vector<uint8_t> blob(16, 0x77);
    MemoryTiffStream zero_rps(
        BuildRawStripedTiff(false, false, 4, 4, /*rows_per_strip=*/4,
                            COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob));
    UniqueTiff reader = zero_rps.Open("rm");
    ASSERT_NE(reader, nullptr);
    const uint32_t saved_rps = reader->tif_dir.td_rowsperstrip;
    auto restore =
        absl::MakeCleanup([&] { reader->tif_dir.td_rowsperstrip = saved_rps; });
    reader->tif_dir.td_rowsperstrip = 0;
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }
  {
    // TIFFVStripSize == 0 when ImageWidth == 0.
    std::vector<uint8_t> blob(16, 0x77);
    MemoryTiffStream zero_w(
        BuildRawStripedTiff(false, false, 4, 4, /*rows_per_strip=*/4,
                            COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob));
    UniqueTiff reader = zero_w.Open("rm");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_IMAGEWIDTH, 0), 1);
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }

  // Compressed strip decode failure (corrupt Deflate payload).
  {
    std::vector<uint8_t> bad_deflate = {0xFF, 0x00, 0xAA, 0x55};
    MemoryTiffStream deflate_stream(
        BuildRawStripedTiff(false, false, 4, 4, 4, COMPRESSION_DEFLATE, 8, 1,
                            FILLORDER_MSB2LSB, bad_deflate));
    UniqueTiff reader = deflate_stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }
}

TEST(TifReadTest, ReadEncodedStripAndAllocBufferPaths) {
  constexpr uint32_t kW = 8;
  constexpr uint32_t kH = 4;
  std::vector<uint8_t> pixels(kW * kH);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<uint8_t>(i * 3 + 1);
  }

  MemoryTiffStream stream;
  WriteStripedImage(stream, "w", kW, kH, 8, 1, 4, COMPRESSION_DEFLATE,
                    PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, pixels);

  // 1. *buf == nullptr allocation + decode with size_to_read == -1 and partial.
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    ScopedTiffExtBuffer ext_buf(reader.get());
    EXPECT_EQ(_TIFFReadEncodedStripAndAllocBuffer(reader.get(), 0,
                                                  ext_buf.out_ptr(), 32, -1),
              32);
    ASSERT_NE(ext_buf.get(), nullptr);
    EXPECT_EQ(std::memcmp(ext_buf.bytes(), pixels.data(), 32), 0);

    // 2. *buf != nullptr delegates to TIFFReadEncodedStrip.
    std::memset(ext_buf.get(), 0, 32);
    void* existing = ext_buf.get();
    EXPECT_EQ(
        _TIFFReadEncodedStripAndAllocBuffer(reader.get(), 0, &existing, 32, 16),
        16);
    EXPECT_EQ(std::memcmp(ext_buf.bytes(), pixels.data(), 16), 0);
  }

  // 3. size_to_read < this_stripsize when *buf == nullptr.
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    ScopedTiffExtBuffer ext_buf(reader.get());
    EXPECT_EQ(_TIFFReadEncodedStripAndAllocBuffer(reader.get(), 0,
                                                  ext_buf.out_ptr(), 32, 16),
              16);
    ASSERT_NE(ext_buf.get(), nullptr);
    EXPECT_EQ(std::memcmp(ext_buf.bytes(), pixels.data(), 16), 0);
  }

  // 4. Out-of-range strip & TIFFFillStrip failure (does not allocate).
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    ScopedTiffExtBuffer ext_buf(reader.get());
    EXPECT_EQ(_TIFFReadEncodedStripAndAllocBuffer(reader.get(), 5,
                                                  ext_buf.out_ptr(), 32, -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(ext_buf.get(), nullptr);

    stream.FailReadsAfter(0);
    EXPECT_EQ(_TIFFReadEncodedStripAndAllocBuffer(reader.get(), 0,
                                                  ext_buf.out_ptr(), 32, -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(ext_buf.get(), nullptr);
    stream.ResetFaults();
  }

  // 5. OOM during _TIFFcallocExt using ScopedSingleMemAllocLimit and
  // ScopedCumulatedMemAllocRemaining. Use uncompressed "rM" so TIFFFillStrip
  // and DumpMode setup perform zero allocations before _TIFFcallocExt runs.
  {
    MemoryTiffStream raw_stream;
    WriteStripedImage(raw_stream, "w", kW, kH, 8, 1, 4, COMPRESSION_NONE,
                      PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, pixels);
    raw_stream.set_enable_mmap(true);
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    raw_stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    UniqueTiff reader = raw_stream.Open("rM", opts.get());
    ASSERT_NE(reader, nullptr);

    ScopedTiffExtBuffer ext_buf(reader.get());
    {
      ScopedSingleMemAllocLimit single_oom(reader.get(), 16);
      EXPECT_EQ(_TIFFReadEncodedStripAndAllocBuffer(reader.get(), 0,
                                                    ext_buf.out_ptr(), 32, -1),
                static_cast<tmsize_t>(-1));
      EXPECT_EQ(ext_buf.get(), nullptr);
    }
    {
      ScopedCumulatedMemAllocRemaining cum_oom(reader.get(), 16);
      EXPECT_EQ(_TIFFReadEncodedStripAndAllocBuffer(reader.get(), 0,
                                                    ext_buf.out_ptr(), 32, -1),
                static_cast<tmsize_t>(-1));
      EXPECT_EQ(ext_buf.get(), nullptr);
    }
  }

  // 6. Decoder failure after buffer allocation.
  {
    std::vector<uint8_t> bad_deflate = {0xFF, 0x00, 0x11, 0x22};
    MemoryTiffStream bad_stream(
        BuildRawStripedTiff(false, false, 4, 4, 4, COMPRESSION_DEFLATE, 8, 1,
                            FILLORDER_MSB2LSB, bad_deflate));
    UniqueTiff reader = bad_stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    ScopedTiffExtBuffer ext_buf(reader.get());
    EXPECT_EQ(_TIFFReadEncodedStripAndAllocBuffer(reader.get(), 0,
                                                  ext_buf.out_ptr(), 16, -1),
              static_cast<tmsize_t>(-1));
    EXPECT_NE(ext_buf.get(), nullptr);
  }
}

TEST(TifReadTest, ReadRawStripMappedAndUnmappedAndErrors) {
  std::vector<uint8_t> pixels(24);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<uint8_t>(0x80 + i);
  }
  MemoryTiffStream stream;
  WriteStripedImage(stream, "w", 6, 4, 8, 1, 4, COMPRESSION_NONE,
                    PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, pixels);
  stream.set_enable_mmap(true);

  for (const char* mode : {"rm", "rM"}) {
    SCOPED_TRACE(mode);
    UniqueTiff reader = stream.Open(mode);
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> full(24);
    EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, full.data(), -1), 24);
    EXPECT_EQ(full, pixels);

    std::vector<uint8_t> partial(10);
    EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, partial.data(), 10), 10);
    EXPECT_THAT(partial, ::testing::ElementsAreArray(pixels.data(), 10));

    EXPECT_EQ(TIFFReadRawStrip(reader.get(), 1, full.data(), -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, full.data(), 0),
              static_cast<tmsize_t>(-1));
  }

  // Unmapped seek and read faults.
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(24);
    stream.FailSeeksAfter(0);
    EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, out.data(), 24),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();

    stream.FailReadsAfter(0);
    EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, out.data(), 24),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();
  }

  // Mapped OOB offset and truncated mapped size.
  {
    std::vector<uint8_t> blob(16, 0x44);
    MemoryTiffStream oob_off(BuildRawStripedTiff(
        false, false, 4, 4, 4, COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob,
        /*override_offset=*/100000ULL, /*override_bytecount=*/16ULL));
    oob_off.set_enable_mmap(true);
    UniqueTiff reader = oob_off.Open("rM");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
  }
  {
    std::vector<uint8_t> blob(8, 0x44);
    MemoryTiffStream oob_len(
        BuildRawTwoStripTiff(false, false, 4, 4, 2, COMPRESSION_NONE, 8, 1,
                             FILLORDER_MSB2LSB, blob, {1000ULL, 1000ULL}));
    oob_len.set_enable_mmap(true);
    UniqueTiff reader = oob_len.Open("rM");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 1000> out{};
    EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, out.data(), 1000),
              static_cast<tmsize_t>(-1));
  }
  {
    // Mapped strip where offset + size overflows TIFF_TMSIZE_T_MAX.
    std::vector<uint8_t> blob(16, 0x55);
    MemoryTiffStream big_stream(BuildRawStripedTiff(
        false, true, 4, 4, 4, COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob));
    big_stream.set_enable_mmap(true);
    UniqueTiff reader = big_stream.Open("rM");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, out.data(),
                               std::numeric_limits<tmsize_t>::max()),
              16);
    // Craft offset within tif_size via direct field check with size close to
    // max.
    reader->tif_dir.td_stripbytecount_p[0] =
        static_cast<uint64_t>(std::numeric_limits<tmsize_t>::max());
    EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }

  // Write-only handle fails TIFFReadRawStrip.
  {
    MemoryTiffStream wstream;
    UniqueTiff writer = wstream.Open("w");
    ASSERT_NE(writer, nullptr);
    std::array<uint8_t, 4> out{};
    EXPECT_EQ(TIFFReadRawStrip(writer.get(), 0, out.data(), 4),
              static_cast<tmsize_t>(-1));
  }
}

TEST(TifReadTest, FillStripLargeThresholdOomCorruptAndMappedTransitions) {
  // 1. Large strip (> 1 MiB) exercises INITIAL_THRESHOLD loop in
  // TIFFReadAndRealloc.
  {
    constexpr uint32_t kW = 1024;
    constexpr uint32_t kH = 1152;  // 1,179,648 bytes (> 1 MiB).
    std::vector<uint8_t> pixels(kW * kH);
    for (size_t i = 0; i < pixels.size(); ++i) {
      pixels[i] = static_cast<uint8_t>((i * 29) & 0xff);
    }
    MemoryTiffStream stream;
    WriteStripedImage(stream, "w", kW, kH, 8, 1, kH, COMPRESSION_NONE,
                      PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, pixels);

    UniqueTiff reader = stream.Open("rmc");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> decoded(pixels.size());
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(), -1),
              static_cast<tmsize_t>(pixels.size()));
    EXPECT_EQ(decoded, pixels);
  }

  // 2. OOM inside TIFFReadAndRealloc (initial alloc and realloc growth).
  {
    std::vector<uint8_t> pixels(8 * 256, 0x22);
    MemoryTiffStream big_strip_stream;
    WriteStripedImage(big_strip_stream, "w", 8, 256, 8, 1,
                      /*rows_per_strip=*/256, COMPRESSION_NONE,
                      PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, pixels);
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    big_strip_stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    UniqueTiff reader2 = big_strip_stream.Open("rm", opts.get());
    ASSERT_NE(reader2, nullptr);
    ASSERT_EQ(TIFFReadBufferSetup(reader2.get(), nullptr, 64), 1);
    ASSERT_NE(reader2->tif_rawdata, nullptr);
    std::vector<uint8_t> out(2048);
    {
      ScopedSingleMemAllocLimit oom(reader2.get(), 1500);
      EXPECT_EQ(TIFFReadEncodedStrip(reader2.get(), 0, out.data(), -1),
                static_cast<tmsize_t>(-1));
      EXPECT_EQ(reader2->tif_rawdata, nullptr);
      EXPECT_EQ(reader2->tif_rawdatasize, 0);
    }
  }

  // 3. Invalid bytecount (0 and > TIFF_INT64_MAX) in BigTIFF.
  {
    std::vector<uint8_t> blob(8, 0x33);
    MemoryTiffStream zero_bc(
        BuildRawTwoStripTiff(false, true, 4, 4, 2, COMPRESSION_NONE, 8, 1,
                             FILLORDER_MSB2LSB, blob, {0ULL, 0ULL}));
    UniqueTiff r0 = zero_bc.Open("rm");
    ASSERT_NE(r0, nullptr);
    std::array<uint8_t, 8> out{};
    EXPECT_EQ(TIFFReadEncodedStrip(r0.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));

    MemoryTiffStream huge_bc(BuildRawTwoStripTiff(
        false, true, 4, 4, 2, COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob,
        {0x8000000000000001ULL, 0x8000000000000001ULL}));
    UniqueTiff r1 = huge_bc.Open("rm");
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(TIFFReadEncodedStrip(r1.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }

  // 4. Excessive bytecount warning & clamping (> 1 MiB and >
  // 10*stripsize+4096). Use 2 strips + "rmc" so ByteCountLooksBad() does not
  // overwrite bytecount.
  {
    // stripsize = 8 -> clamped newbytecount = 8*10 + 4096 = 4176.
    std::vector<uint8_t> padded(4300, 0x61);
    MemoryTiffStream warn_stream(BuildRawTwoStripTiff(
        false, false, 4, 4, /*rows_per_strip=*/2, COMPRESSION_NONE, 8, 1,
        FILLORDER_MSB2LSB, padded, {2000000ULL, 2000000ULL}));
    UniqueTiff reader = warn_stream.Open("rmc");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 8> out{};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), -1), 8);
    EXPECT_THAT(out, ::testing::Each(0x61));
    EXPECT_GT(warn_stream.diagnostics().warning_count(), 0U);
  }

  // 5. Huge uncompressed strip (> 100 MiB) with tiny bytecount is rejected.
  // Use 2 strips + "rmc" so stripchop/ByteCountLooksBad() do not alter it.
  {
    std::vector<uint8_t> blob(16, 0x11);
    MemoryTiffStream ratio_stream(
        BuildRawTwoStripTiff(false, false, /*width=*/1024, /*height=*/205000,
                             /*rows_per_strip=*/102500, COMPRESSION_NONE, 8, 1,
                             FILLORDER_MSB2LSB, blob, {16ULL, 16ULL}));
    UniqueTiff reader = ratio_stream.Open("rmc");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, nullptr, 16),
              static_cast<tmsize_t>(-1));
  }

  // 6. Mapped OOB, freeing TIFF_MYBUFFER when switching to mapped direct, and
  // clearing TIFF_BUFFERMMAP when reading mapped with bit reversal.
  {
    std::vector<uint8_t> pixels(16);
    for (size_t i = 0; i < pixels.size(); ++i) {
      pixels[i] = static_cast<uint8_t>(i + 1);
    }
    MemoryTiffStream stream;
    WriteStripedImage(stream, "w", 4, 4, 8, 1, /*rows_per_strip=*/2,
                      COMPRESSION_NONE, PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB,
                      pixels);
    stream.set_enable_mmap(true);

    // Pre-existing TIFF_MYBUFFER freed when mapped direct fills strip 0.
    UniqueTiff reader = stream.Open("rM");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFReadBufferSetup(reader.get(), nullptr, 64), 1);
    std::vector<uint8_t> out0(8);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out0.data(), 8), 8);
    EXPECT_NE(reader->tif_flags & TIFF_BUFFERMMAP, 0U);

    // Switch fillorder so next fill enters non-direct branch and clears
    // TIFF_BUFFERMMAP.
    ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_FILLORDER, FILLORDER_LSB2MSB),
              1);
    std::vector<uint8_t> out(8);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 1, out.data(), -1), 8);
    for (size_t i = 0; i < out.size(); ++i) {
      EXPECT_EQ(out[i], ReverseByteBits(pixels[8 + i]));
    }
  }

  // Mapped OOB check in TIFFFillStrip.
  {
    std::vector<uint8_t> blob(16, 0x11);
    MemoryTiffStream oob_mapped(BuildRawStripedTiff(
        false, false, 4, 4, 4, COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob,
        /*override_offset=*/50000ULL, /*override_bytecount=*/16ULL));
    oob_mapped.set_enable_mmap(true);
    UniqueTiff reader = oob_mapped.Open("rM");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }

  // Mapped bit-reversal with user buffer too small -> fails.
  {
    std::vector<uint8_t> pixels(16, 0x55);
    MemoryTiffStream stream;
    WriteStripedImage(stream, "w", 4, 4, 8, 1, 4, COMPRESSION_NONE,
                      PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, pixels);
    stream.set_enable_mmap(true);
    UniqueTiff reader = stream.Open("rML");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 4> small_buf{};
    ASSERT_EQ(TIFFReadBufferSetup(reader.get(), small_buf.data(), 4), 1);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }

  // Mapped bit-reversal with OOM during TIFFReadBufferSetup.
  {
    std::vector<uint8_t> pixels(2048, 0x55);
    MemoryTiffStream stream;
    WriteStripedImage(stream, "w", 32, 64, 8, 1, 64, COMPRESSION_NONE,
                      PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, pixels);
    stream.set_enable_mmap(true);
    UniqueTiff reader = stream.Open("rML");
    ASSERT_NE(reader, nullptr);
    ScopedSingleMemAllocLimit oom(reader.get(), 512);
    std::vector<uint8_t> out(2048);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }

  // Unmapped seek error in TIFFFillStrip -> TIFFReadRawStripOrTile2.
  {
    std::vector<uint8_t> blob(16, 0x55);
    MemoryTiffStream stream(BuildRawStripedTiff(false, false, 4, 4, 4,
                                                COMPRESSION_NONE, 8, 1,
                                                FILLORDER_MSB2LSB, blob));
    UniqueTiff r_unmap = stream.Open("rm");
    ASSERT_NE(r_unmap, nullptr);
    stream.FailSeeksAfter(0);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(TIFFReadEncodedStrip(r_unmap.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();
  }
}

TEST(TifReadTest, ReadTileEncodedTileAndAllocBufferAndRawTile) {
  // 2D multi-tile separate-plane and 3D tiled images.
  constexpr uint32_t kW = 32;
  constexpr uint32_t kH = 32;
  constexpr uint32_t kTw = 16;
  constexpr uint32_t kTh = 16;
  constexpr uint16_t kSpp = 3;
  // 2x2 tiles * 3 planes = 12 tiles of 256 bytes each.
  std::vector<std::vector<uint8_t>> tiles_sep(12, std::vector<uint8_t>(256));
  for (size_t t = 0; t < tiles_sep.size(); ++t) {
    for (size_t b = 0; b < 256; ++b) {
      tiles_sep[t][b] = static_cast<uint8_t>((t * 17 + b) & 0xff);
    }
  }

  MemoryTiffStream sep_stream;
  WriteTiledImage(sep_stream, "w", kW, kH, 1, kTw, kTh, 1, 8, kSpp,
                  COMPRESSION_NONE, PLANARCONFIG_SEPARATE, FILLORDER_MSB2LSB,
                  tiles_sep);
  sep_stream.set_enable_mmap(true);

  for (const char* mode : {"rm", "rM"}) {
    SCOPED_TRACE(mode);
    UniqueTiff reader = sep_stream.Open(mode);
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> buf(256);
    for (uint16_t s = 0; s < kSpp; ++s) {
      for (uint32_t y = 0; y < kH; y += kTh) {
        for (uint32_t x = 0; x < kW; x += kTw) {
          const uint32_t tid = TIFFComputeTile(reader.get(), x, y, 0, s);
          EXPECT_EQ(TIFFReadTile(reader.get(), buf.data(), x, y, 0, s), 256);
          EXPECT_EQ(buf, tiles_sep[tid]);
        }
      }
    }

    // Out-of-bounds x, y, z, s and out-of-range tile index.
    EXPECT_EQ(TIFFReadTile(reader.get(), buf.data(), kW, 0, 0, 0),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadTile(reader.get(), buf.data(), 0, kH, 0, 0),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadTile(reader.get(), buf.data(), 0, 0, 1, 0),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadTile(reader.get(), buf.data(), 0, 0, 0, kSpp),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 12, buf.data(), 256),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadRawTile(reader.get(), 12, buf.data(), 256),
              static_cast<tmsize_t>(-1));
  }

  // TIFFReadEncodedTile shortcut, partial, clamped, and bit reversal.
  {
    UniqueTiff reader = sep_stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> buf(256);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 3, buf.data(), 512), 256);
    EXPECT_EQ(buf, tiles_sep[3]);

    std::vector<uint8_t> partial(64);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 3, partial.data(), 64), 64);
    EXPECT_THAT(partial, ::testing::ElementsAreArray(tiles_sep[3].data(), 64));

    // Mapped with size > tilesize clamps size to tilesize.
    UniqueTiff mreader = sep_stream.Open("rM");
    ASSERT_NE(mreader, nullptr);
    EXPECT_EQ(TIFFReadEncodedTile(mreader.get(), 4, buf.data(), 512), 256);
    EXPECT_EQ(buf, tiles_sep[4]);

    // Shortcut bit reversal ("rmL").
    UniqueTiff lreader = sep_stream.Open("rmL");
    ASSERT_NE(lreader, nullptr);
    EXPECT_EQ(TIFFReadEncodedTile(lreader.get(), 0, buf.data(), 256), 256);
    for (size_t i = 0; i < buf.size(); ++i) {
      EXPECT_EQ(buf[i], ReverseByteBits(tiles_sep[0][i]));
    }
  }

  // Error paths on striped / write-only handles.
  {
    std::vector<uint8_t> s_pixels(16, 0x11);
    MemoryTiffStream striped_stream;
    WriteStripedImage(striped_stream, "w", 4, 4, 8, 1, 4, COMPRESSION_NONE,
                      PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, s_pixels);
    UniqueTiff sreader = striped_stream.Open("rm");
    ASSERT_NE(sreader, nullptr);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(TIFFReadTile(sreader.get(), out.data(), 0, 0, 0, 0),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadEncodedTile(sreader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadRawTile(sreader.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    ScopedTiffExtBuffer ext(sreader.get());
    EXPECT_EQ(_TIFFReadTileAndAllocBuffer(sreader.get(), ext.out_ptr(), 16, 0,
                                          0, 0, 0),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(_TIFFReadEncodedTileAndAllocBuffer(sreader.get(), 0,
                                                 ext.out_ptr(), 16, -1),
              static_cast<tmsize_t>(-1));
  }
  {
    MemoryTiffStream wstream;
    UniqueTiff writer = wstream.Open("w");
    ASSERT_NE(writer, nullptr);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(TIFFReadEncodedTile(writer.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
  }

  // _TIFFReadTileAndAllocBuffer / _TIFFReadEncodedTileAndAllocBuffer:
  // uncompressed success requires tif_rawdatasize == tilesize (e.g. "rM" or
  // 1024-byte tile), whereas 256-byte tile in "rm" triggers mismatch check!
  {
    UniqueTiff mreader = sep_stream.Open("rM");
    ASSERT_NE(mreader, nullptr);
    ScopedTiffExtBuffer ext(mreader.get());
    EXPECT_EQ(_TIFFReadTileAndAllocBuffer(mreader.get(), ext.out_ptr(), 256, 16,
                                          0, 0, 1),
              256);
    ASSERT_NE(ext.get(), nullptr);
    const uint32_t tid = TIFFComputeTile(mreader.get(), 16, 0, 0, 1);
    EXPECT_EQ(std::memcmp(ext.bytes(), tiles_sep[tid].data(), 256), 0);

    // *buf != nullptr delegates to TIFFReadEncodedTile.
    void* existing = ext.get();
    EXPECT_EQ(_TIFFReadEncodedTileAndAllocBuffer(mreader.get(), 0, &existing,
                                                 256, 256),
              256);
    EXPECT_EQ(std::memcmp(ext.bytes(), tiles_sep[0].data(), 256), 0);

    // Non-existent tile & out-of-bounds tile coords.
    ScopedTiffExtBuffer ext2(mreader.get());
    EXPECT_EQ(_TIFFReadEncodedTileAndAllocBuffer(mreader.get(), 99,
                                                 ext2.out_ptr(), 256, -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(ext2.get(), nullptr);
  }
  {
    // Unmapped 256-byte uncompressed tile has tif_rawdatasize == 1024 != 256.
    UniqueTiff reader = sep_stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    ScopedTiffExtBuffer ext(reader.get());
    EXPECT_EQ(_TIFFReadEncodedTileAndAllocBuffer(reader.get(), 0, ext.out_ptr(),
                                                 256, -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(ext.get(), nullptr);
  }

  // Compressed tile with _TIFFReadEncodedTileAndAllocBuffer (success, OOM,
  // size_to_read > tilesize, and corrupt decode failure).
  {
    std::vector<std::vector<uint8_t>> comp_tiles(
        1, std::vector<uint8_t>(256, 0x3C));
    MemoryTiffStream comp_stream;
    WriteTiledImage(comp_stream, "w", 16, 16, 1, 16, 16, 1, 8, 1,
                    COMPRESSION_DEFLATE, PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB,
                    comp_tiles);

    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    comp_stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
    UniqueTiff reader = comp_stream.Open("rm", opts.get());
    ASSERT_NE(reader, nullptr);

    {
      ScopedTiffExtBuffer ext(reader.get());
      EXPECT_EQ(_TIFFReadEncodedTileAndAllocBuffer(reader.get(), 0,
                                                   ext.out_ptr(), 256, 512),
                256);
      ASSERT_NE(ext.get(), nullptr);
      EXPECT_EQ(std::memcmp(ext.bytes(), comp_tiles[0].data(), 256), 0);
    }
    {
      ScopedTiffExtBuffer ext(reader.get());
      ScopedSingleMemAllocLimit oom(reader.get(), 64);
      EXPECT_EQ(_TIFFReadEncodedTileAndAllocBuffer(reader.get(), 0,
                                                   ext.out_ptr(), 256, -1),
                static_cast<tmsize_t>(-1));
      EXPECT_EQ(ext.get(), nullptr);
    }
  }
  {
    std::vector<uint8_t> bad_deflate = {0xFF, 0x00, 0x12, 0x34};
    MemoryTiffStream bad_tile(
        BuildRawTiledTiff(false, false, 16, 16, 16, 16, COMPRESSION_DEFLATE, 8,
                          1, FILLORDER_MSB2LSB, bad_deflate));
    UniqueTiff reader = bad_tile.Open("rm");
    ASSERT_NE(reader, nullptr);
    ScopedTiffExtBuffer ext(reader.get());
    EXPECT_EQ(_TIFFReadEncodedTileAndAllocBuffer(reader.get(), 0, ext.out_ptr(),
                                                 256, -1),
              static_cast<tmsize_t>(-1));
    EXPECT_NE(ext.get(), nullptr);

    // Regular TIFFReadEncodedTile decode failure.
    std::vector<uint8_t> out(256);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }
  {
    // TIFFFillTile failure in _TIFFReadEncodedTileAndAllocBuffer (*buf ==
    // null).
    UniqueTiff reader = sep_stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    sep_stream.FailReadsAfter(0);
    ScopedTiffExtBuffer ext(reader.get());
    EXPECT_EQ(_TIFFReadEncodedTileAndAllocBuffer(reader.get(), 0, ext.out_ptr(),
                                                 256, -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(ext.get(), nullptr);
    sep_stream.ResetFaults();
  }
}

TEST(TifReadTest, RawTileAndFillTileFaultsCorruptAndMappedTransitions) {
  std::vector<std::vector<uint8_t>> tiles(2, std::vector<uint8_t>(256, 0x2A));
  for (size_t i = 0; i < 256; ++i) {
    tiles[1][i] = static_cast<uint8_t>(i);
  }
  MemoryTiffStream stream;
  WriteTiledImage(stream, "w", 32, 16, 1, 16, 16, 1, 8, 1, COMPRESSION_NONE,
                  PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, tiles);
  stream.set_enable_mmap(true);

  // TIFFReadRawTile unmapped & mapped + partial + size == 0 + faults.
  for (const char* mode : {"rm", "rM"}) {
    SCOPED_TRACE(mode);
    UniqueTiff reader = stream.Open(mode);
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> full(256);
    EXPECT_EQ(TIFFReadRawTile(reader.get(), 1, full.data(), -1), 256);
    EXPECT_EQ(full, tiles[1]);

    std::vector<uint8_t> partial(32);
    EXPECT_EQ(TIFFReadRawTile(reader.get(), 1, partial.data(), 32), 32);
    EXPECT_THAT(partial, ::testing::ElementsAreArray(tiles[1].data(), 32));

    EXPECT_EQ(TIFFReadRawTile(reader.get(), 0, full.data(), 0),
              static_cast<tmsize_t>(-1));
  }

  // Unmapped seek and read errors in TIFFReadRawTile1 and
  // TIFFReadRawStripOrTile2.
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(256);
    stream.FailSeeksAfter(0);
    EXPECT_EQ(TIFFReadRawTile(reader.get(), 0, out.data(), 256),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
    EXPECT_THAT(out, ::testing::Each(0));
    stream.ResetFaults();

    stream.FailReadsAfter(0);
    EXPECT_EQ(TIFFReadRawTile(reader.get(), 0, out.data(), 256),
              static_cast<tmsize_t>(-1));
    // Shortcut TIFFReadEncodedTile (size >= tilesize, !isMapped) read error.
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out.data(), 256),
              static_cast<tmsize_t>(-1));
    // Also test null buf on TIFFFillTile failure in TIFFReadEncodedTile.
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, nullptr, -1),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();
  }

  // Mapped raw tile OOB / overflow.
  {
    std::vector<uint8_t> blob(256, 0x11);
    MemoryTiffStream oob_tile(BuildRawTiledTiff(
        false, false, 16, 16, 16, 16, COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB,
        blob, /*override_offset=*/50000ULL, /*override_bytecount=*/256ULL));
    oob_tile.set_enable_mmap(true);
    UniqueTiff reader = oob_tile.Open("rM");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(256);
    EXPECT_EQ(TIFFReadRawTile(reader.get(), 0, out.data(), 256),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }
  {
    std::vector<uint8_t> blob(256, 0x11);
    MemoryTiffStream len_tile(BuildRawTiledTiff(
        false, false, 16, 16, 16, 16, COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB,
        blob, std::nullopt, /*override_bytecount=*/2000ULL));
    len_tile.set_enable_mmap(true);
    UniqueTiff reader = len_tile.Open("rM");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(2000);
    EXPECT_EQ(TIFFReadRawTile(reader.get(), 0, out.data(), 2000),
              static_cast<tmsize_t>(-1));
  }

  // TIFFFillTile: bytecount == 0, bytecount > TIFF_INT64_MAX, huge bytecount
  // warning + clamping, and tilesize > 100 MiB ratio error.
  {
    std::vector<uint8_t> blob(256, 0x12);
    MemoryTiffStream zero_bc(BuildRawTiledTiff(
        false, true, 16, 16, 16, 16, COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB,
        blob, std::nullopt, /*override_bytecount=*/0ULL));
    UniqueTiff r0 = zero_bc.Open("rm");
    ASSERT_NE(r0, nullptr);
    std::vector<uint8_t> out(256);
    EXPECT_EQ(TIFFReadEncodedTile(r0.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));

    MemoryTiffStream huge_bc(BuildRawTiledTiff(
        false, true, 16, 16, 16, 16, COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB,
        blob, std::nullopt, /*override_bytecount=*/0x8000000000000001ULL));
    UniqueTiff r1 = huge_bc.Open("rm");
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(TIFFReadEncodedTile(r1.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }
  {
    // tilesize = 256 -> clamped = 256*10 + 4096 = 6656.
    std::vector<uint8_t> padded(7000, 0x4E);
    MemoryTiffStream warn_tile(BuildRawTiledTiff(
        false, false, 16, 16, 16, 16, COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB,
        padded, std::nullopt, /*override_bytecount=*/2000000ULL));
    UniqueTiff reader = warn_tile.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(256);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out.data(), -1), 256);
    EXPECT_THAT(out, ::testing::Each(0x4E));
    EXPECT_GT(warn_tile.diagnostics().warning_count(), 0U);
  }
  {
    std::vector<uint8_t> blob(16, 0x11);
    MemoryTiffStream ratio_tile(BuildRawTiledTiff(
        false, false, 16384, 16384, /*tile_w=*/16384, /*tile_h=*/8192,
        COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob));
    UniqueTiff reader = ratio_tile.Open("rm");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, nullptr, 16),
              static_cast<tmsize_t>(-1));
  }

  // Mapped direct freeing TIFF_MYBUFFER and mapped bitrev clearing
  // TIFF_BUFFERMMAP + user buffer too small + OOM on TIFFReadBufferSetup.
  {
    UniqueTiff reader = stream.Open("rM");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFReadBufferSetup(reader.get(), nullptr, 64), 1);
    std::vector<uint8_t> out0(256);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out0.data(), -1), 256);
    EXPECT_NE(reader->tif_flags & TIFF_BUFFERMMAP, 0U);

    ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_FILLORDER, FILLORDER_LSB2MSB),
              1);
    std::vector<uint8_t> out(256);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 1, out.data(), -1), 256);
    for (size_t i = 0; i < out.size(); ++i) {
      EXPECT_EQ(out[i], ReverseByteBits(tiles[1][i]));
    }
  }
  {
    UniqueTiff reader = stream.Open("rML");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 8> small_buf{};
    ASSERT_EQ(TIFFReadBufferSetup(reader.get(), small_buf.data(), 8), 1);
    std::vector<uint8_t> out(256);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }
  {
    UniqueTiff reader = stream.Open("rML");
    ASSERT_NE(reader, nullptr);
    ScopedSingleMemAllocLimit oom(reader.get(), 64);
    std::vector<uint8_t> out(256);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out.data(), -1),
              static_cast<tmsize_t>(-1));
  }
}

TEST(TifReadTest, ReadBufferSetupAndUserBufferStripedAndTiled) {
  // TIFFReadBufferSetup user buffer, reallocating, invalid size 0, and OOM.
  {
    std::vector<uint8_t> pixels(16, 0x99);
    MemoryTiffStream stream;
    WriteStripedImage(stream, "w", 4, 4, 8, 1, 4, COMPRESSION_NONE,
                      PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, pixels);

    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> user_raw(1024, 0);
    EXPECT_EQ(TIFFReadBufferSetup(reader.get(), user_raw.data(), 1024), 1);
    EXPECT_EQ(reader->tif_flags & TIFF_MYBUFFER, 0U);

    // Replacing a user-owned buffer with bp == nullptr must not free user_raw.
    EXPECT_EQ(TIFFReadBufferSetup(reader.get(), nullptr, 512), 1);
    EXPECT_NE(reader->tif_flags & TIFF_MYBUFFER, 0U);

    // Replacing an owned buffer frees it and allocates new one.
    EXPECT_EQ(TIFFReadBufferSetup(reader.get(), nullptr, 2048), 1);

    // Size 0 fails with Invalid buffer size.
    EXPECT_EQ(TIFFReadBufferSetup(reader.get(), nullptr, 0), 0);

    // OOM inside TIFFReadBufferSetup.
    ScopedSingleMemAllocLimit oom(reader.get(), 256);
    EXPECT_EQ(TIFFReadBufferSetup(reader.get(), nullptr, 1024), 0);
  }

  // TIFFReadFromUserBuffer on striped and tiled images (positive insize only).
  {
    std::vector<uint8_t> pixels(16);
    for (size_t i = 0; i < pixels.size(); ++i) {
      pixels[i] = static_cast<uint8_t>(i + 0x20);
    }
    MemoryTiffStream sstream;
    WriteStripedImage(sstream, "w", 4, 4, 8, 1, /*rows_per_strip=*/10,
                      COMPRESSION_NONE, PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB,
                      pixels);

    UniqueTiff sreader = sstream.Open("rm");
    ASSERT_NE(sreader, nullptr);
    const uint32_t saved_flags = sreader->tif_flags;
    std::vector<uint8_t> inbuf = pixels;
    std::vector<uint8_t> outbuf(16);
    EXPECT_EQ(TIFFReadFromUserBuffer(sreader.get(), 0, inbuf.data(), 16,
                                     outbuf.data(), 16),
              1);
    EXPECT_EQ(outbuf, pixels);
    EXPECT_EQ(sreader->tif_flags & (TIFF_MYBUFFER | TIFF_BUFFERMMAP),
              saved_flags & (TIFF_MYBUFFER | TIFF_BUFFERMMAP));

    // Bit reversal on input buffer restores caller's inbuf afterwards.
    UniqueTiff lreader = sstream.Open("rmL");
    ASSERT_NE(lreader, nullptr);
    std::vector<uint8_t> inbuf_copy = pixels;
    std::fill(outbuf.begin(), outbuf.end(), 0);
    EXPECT_EQ(TIFFReadFromUserBuffer(lreader.get(), 0, inbuf_copy.data(), 16,
                                     outbuf.data(), 16),
              1);
    EXPECT_EQ(inbuf_copy, pixels);
    for (size_t i = 0; i < outbuf.size(); ++i) {
      EXPECT_EQ(outbuf[i], ReverseByteBits(pixels[i]));
    }

    // Decode failure (insize < outsize for DumpModeDecode).
    EXPECT_EQ(TIFFReadFromUserBuffer(sreader.get(), 0, inbuf.data(), 4,
                                     outbuf.data(), 16),
              0);

    // Write-only handle fails.
    MemoryTiffStream wstream;
    UniqueTiff writer = wstream.Open("w");
    ASSERT_NE(writer, nullptr);
    EXPECT_EQ(TIFFReadFromUserBuffer(writer.get(), 0, inbuf.data(), 16,
                                     outbuf.data(), 16),
              0);
  }

  // TIFFReadFromUserBuffer with rowsperstrip == 0.
  {
    std::vector<uint8_t> blob(16, 0x11);
    MemoryTiffStream stream(
        BuildRawStripedTiff(false, false, 4, 4, /*rows_per_strip=*/4,
                            COMPRESSION_NONE, 8, 1, FILLORDER_MSB2LSB, blob));
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    const uint32_t saved_rps = reader->tif_dir.td_rowsperstrip;
    auto restore =
        absl::MakeCleanup([&] { reader->tif_dir.td_rowsperstrip = saved_rps; });
    reader->tif_dir.td_rowsperstrip = 0;
    std::vector<uint8_t> inbuf = blob;
    std::vector<uint8_t> outbuf(16);
    EXPECT_EQ(TIFFReadFromUserBuffer(reader.get(), 0, inbuf.data(), 16,
                                     outbuf.data(), 16),
              0);
  }

  // TIFFReadFromUserBuffer on tiled image (success & failure).
  {
    std::vector<std::vector<uint8_t>> tiles(1, std::vector<uint8_t>(256, 0x4B));
    MemoryTiffStream tstream;
    WriteTiledImage(tstream, "w", 16, 16, 1, 16, 16, 1, 8, 1, COMPRESSION_NONE,
                    PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, tiles);
    UniqueTiff treader = tstream.Open("rm");
    ASSERT_NE(treader, nullptr);
    std::vector<uint8_t> inbuf = tiles[0];
    std::vector<uint8_t> outbuf(256);
    EXPECT_EQ(TIFFReadFromUserBuffer(treader.get(), 0, inbuf.data(), 256,
                                     outbuf.data(), 256),
              1);
    EXPECT_EQ(outbuf, tiles[0]);

    // Decode failure when inbuf shorter than outbuf.
    EXPECT_EQ(TIFFReadFromUserBuffer(treader.get(), 0, inbuf.data(), 16,
                                     outbuf.data(), 256),
              0);

    // TIFFStartTile zero tilewidth / imagewidth / imagelength guards.
    ASSERT_EQ(TIFFSetField(treader.get(), TIFFTAG_TILEWIDTH, 0), 1);
    EXPECT_EQ(TIFFReadFromUserBuffer(treader.get(), 0, inbuf.data(), 256,
                                     outbuf.data(), 256),
              0);
    ASSERT_EQ(TIFFSetField(treader.get(), TIFFTAG_TILEWIDTH, 16), 1);

    ASSERT_EQ(TIFFSetField(treader.get(), TIFFTAG_IMAGEWIDTH, 0), 1);
    EXPECT_EQ(TIFFReadFromUserBuffer(treader.get(), 0, inbuf.data(), 256,
                                     outbuf.data(), 256),
              0);
    ASSERT_EQ(TIFFSetField(treader.get(), TIFFTAG_IMAGEWIDTH, 16), 1);

    ASSERT_EQ(TIFFSetField(treader.get(), TIFFTAG_IMAGELENGTH, 0), 1);
    EXPECT_EQ(TIFFReadFromUserBuffer(treader.get(), 0, inbuf.data(), 256,
                                     outbuf.data(), 256),
              0);
  }

  // TIFFStartStrip zero td_stripsperimage guard (empty "w" handle before
  // writing a directory has td_stripsperimage == 0 and tif_mode != O_WRONLY).
  {
    MemoryTiffStream empty_w;
    UniqueTiff writer = empty_w.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 4), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 4), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 4), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    std::array<uint8_t, 16> inbuf{};
    std::array<uint8_t, 16> outbuf{};
    EXPECT_EQ(TIFFReadFromUserBuffer(writer.get(), 0, inbuf.data(), 16,
                                     outbuf.data(), 16),
              0);
  }
}

TEST(TifReadTest, CustomCodecPathsNoReadRawStartFailuresAndRatioGuards) {
  CustomCodecBehavior behavior;
  g_codec_behavior = &behavior;
  auto clear_behavior = absl::MakeCleanup([] { g_codec_behavior = nullptr; });
  ScopedRegisteredCodec reg(kTestCodecScheme, "TestReadCodec",
                            &CustomCodecInit);

  std::vector<uint8_t> sblob(16, 0x3A);
  MemoryTiffStream sstream(BuildRawStripedTiff(
      false, false, 4, 4, 4, kTestCodecScheme, 8, 1, FILLORDER_MSB2LSB, sblob));
  std::vector<uint8_t> tblob(256, 0x7B);
  MemoryTiffStream tstream(BuildRawTiledTiff(false, false, 16, 16, 16, 16,
                                             kTestCodecScheme, 8, 1,
                                             FILLORDER_MSB2LSB, tblob));

  // 1. TIFF_NOREADRAW set by codec init.
  behavior.set_noreadraw = true;
  {
    UniqueTiff sr = sstream.Open("rm");
    ASSERT_NE(sr, nullptr);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(TIFFReadRawStrip(sr.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(
        TIFFReadFromUserBuffer(sr.get(), 0, sblob.data(), 16, out.data(), 16),
        0);
    EXPECT_EQ(TIFFReadEncodedStrip(sr.get(), 0, out.data(), 16), 16);
    EXPECT_THAT(out, ::testing::Each(0x5A));
  }
  {
    UniqueTiff tr = tstream.Open("rm");
    ASSERT_NE(tr, nullptr);
    std::array<uint8_t, 256> out{};
    EXPECT_EQ(TIFFReadRawTile(tr.get(), 0, out.data(), 256),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadEncodedTile(tr.get(), 0, out.data(), 256), 256);
    EXPECT_THAT(out, ::testing::Each(0x5A));
  }

  // 2. setupdecode and predecode failures in TIFFStartStrip / TIFFStartTile,
  // including TIFFReadFromUserBuffer zeroing outbuf on start failure.
  behavior.set_noreadraw = false;
  behavior.setupdecode_ok = false;
  {
    UniqueTiff sr = sstream.Open("rm");
    ASSERT_NE(sr, nullptr);
    std::array<uint8_t, 16> out{};
    out.fill(0xAA);
    EXPECT_EQ(TIFFReadEncodedStrip(sr.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(
        TIFFReadFromUserBuffer(sr.get(), 0, sblob.data(), 16, out.data(), 16),
        0);
    EXPECT_THAT(out, ::testing::Each(0));
    EXPECT_EQ(TIFFReadFromUserBuffer(sr.get(), 0, sblob.data(), 16, nullptr, 0),
              0);

    UniqueTiff tr = tstream.Open("rm");
    ASSERT_NE(tr, nullptr);
    std::array<uint8_t, 256> tout{};
    tout.fill(0xAA);
    EXPECT_EQ(TIFFReadEncodedTile(tr.get(), 0, tout.data(), 256),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFReadFromUserBuffer(tr.get(), 0, tblob.data(), 256,
                                     tout.data(), 256),
              0);
    EXPECT_THAT(tout, ::testing::Each(0));
    EXPECT_EQ(
        TIFFReadFromUserBuffer(tr.get(), 0, tblob.data(), 256, nullptr, 0), 0);
  }

  behavior.setupdecode_ok = true;
  behavior.predecode_ok = false;
  {
    UniqueTiff sr = sstream.Open("rm");
    ASSERT_NE(sr, nullptr);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(TIFFReadEncodedStrip(sr.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(sr->tif_dir.td_curstrip, NOSTRIP);

    UniqueTiff tr = tstream.Open("rm");
    ASSERT_NE(tr, nullptr);
    std::array<uint8_t, 256> tout{};
    EXPECT_EQ(TIFFReadEncodedTile(tr.get(), 0, tout.data(), 256),
              static_cast<tmsize_t>(-1));
  }

  // 3. Custom seek failure and backward-seek TIFFStartStrip failure in
  // TIFFReadScanline.
  behavior.predecode_ok = true;
  behavior.seek_ok = false;
  {
    UniqueTiff sr = sstream.Open("rm");
    ASSERT_NE(sr, nullptr);
    std::array<uint8_t, 4> row{};
    EXPECT_EQ(TIFFReadScanline(sr.get(), row.data(), 2, 0), -1);
  }
  {
    behavior.seek_ok = true;
    UniqueTiff sr = sstream.Open("rm");
    ASSERT_NE(sr, nullptr);
    std::array<uint8_t, 4> row{};
    EXPECT_EQ(TIFFReadScanline(sr.get(), row.data(), 2, 0), 1);
    behavior.predecode_ok = false;
    EXPECT_EQ(TIFFReadScanline(sr.get(), row.data(), 0, 0), -1);
  }

  // 4. Compression ratio guard in _TIFFReadEncodedStripAndAllocBuffer and
  // _TIFFReadEncodedTileAndAllocBuffer (using TIFF_NOREADRAW so TIFFFill*
  // doesn't fail first, leaving tif_rawdatasize == 0).
  behavior.set_noreadraw = true;
  behavior.predecode_ok = true;
  behavior.seek_ok = true;
  behavior.max_compression_ratio = 10;
  {
    UniqueTiff sr = sstream.Open("rm");
    ASSERT_NE(sr, nullptr);
    ScopedTiffExtBuffer ext(sr.get());
    EXPECT_EQ(_TIFFReadEncodedStripAndAllocBuffer(sr.get(), 0, ext.out_ptr(),
                                                  101 * 1024 * 1024, -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(ext.get(), nullptr);

    UniqueTiff tr = tstream.Open("rm");
    ASSERT_NE(tr, nullptr);
    ScopedTiffExtBuffer text(tr.get());
    EXPECT_EQ(_TIFFReadEncodedTileAndAllocBuffer(tr.get(), 0, text.out_ptr(),
                                                 101 * 1024 * 1024, -1),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(text.get(), nullptr);
  }

  g_codec_behavior = nullptr;
}

TEST(TifReadTest, BigEndianPostDecodeSwabAllBitDepths) {
  // Exercises _TIFFNoPostDecode (8-bit) and _TIFFSwab{16,24,32,64}BitData
  // when reading a big-endian ("wb") TIFF on a little-endian host.
  for (uint16_t bps :
       {uint16_t{8}, uint16_t{16}, uint16_t{24}, uint16_t{32}, uint16_t{64}}) {
    SCOPED_TRACE(bps);
    constexpr uint32_t kW = 4;
    constexpr uint32_t kH = 2;
    const size_t bytes_per_sample = bps / 8;
    std::vector<uint8_t> pixels(kW * kH * bytes_per_sample);
    for (size_t i = 0; i < pixels.size(); ++i) {
      pixels[i] = static_cast<uint8_t>((i * 37 + 3) & 0xff);
    }

    MemoryTiffStream stream;
    WriteStripedImage(stream, "wb", kW, kH, bps, 1, kH, COMPRESSION_NONE,
                      PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB, pixels);

    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    EXPECT_NE(TIFFIsByteSwapped(reader.get()), 0);
    std::vector<uint8_t> out(pixels.size());
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(out.size()));
    EXPECT_EQ(out, pixels);
  }
}

TEST(TifReadTest, RealFileMmapAndUnmappedRoundTrip) {
  TempFile temp_file("tif_read_mmap_test.tif");
  std::vector<uint8_t> pixels(64);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<uint8_t>(i * 5 + 9);
  }

  {
    UniqueTiff writer(TIFFOpen(temp_file.c_str(), "w8"));
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 8), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 8), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 4), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    std::array<uint8_t, 8> row_buf{};
    for (uint32_t y = 0; y < 8; ++y) {
      std::memcpy(row_buf.data(), pixels.data() + y * 8, 8);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), y, 0), 1);
    }
  }

  for (const char* mode : {"rM", "rm"}) {
    SCOPED_TRACE(mode);
    UniqueTiff reader(TIFFOpen(temp_file.c_str(), mode));
    ASSERT_NE(reader, nullptr);
    EXPECT_NE(TIFFIsBigTIFF(reader.get()), 0);
    std::vector<uint8_t> strip0(32);
    std::vector<uint8_t> strip1(32);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip0.data(), 32), 32);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 1, strip1.data(), 32), 32);
    EXPECT_THAT(strip0, ::testing::ElementsAreArray(pixels.data(), 32));
    EXPECT_THAT(strip1, ::testing::ElementsAreArray(pixels.data() + 32, 32));
  }
}

TEST(TifReadTest, BackwardSeekPartialStripReloadAndStripIndexOverflow) {
  constexpr uint32_t kWidth = 4;
  constexpr uint32_t kHeight = 4;
  std::vector<uint8_t> pixels(kWidth * kHeight);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<uint8_t>(0x30 + i);
  }

  MemoryTiffStream stream;
  WriteStripedImage(stream, "w", kWidth, kHeight, 8, 1, /*rows_per_strip=*/4,
                    COMPRESSION_NONE, PLANARCONFIG_CONTIG, FILLORDER_MSB2LSB,
                    pixels);

  // Backward scanline seek with non-zero tif_rawdataoff invokes
  // TIFFFillStripPartial(..., restart=1) cleanly on unmapped handles.
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 4> row_buf{};
    ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 2, 0), 1);
    EXPECT_THAT(row_buf, ::testing::ElementsAreArray(pixels.data() + 8, 4));
    ASSERT_EQ(reader->tif_rawdataoff, 0U);

    reader->tif_rawdataoff = 4;
    ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 0, 0), 1);
    EXPECT_EQ(reader->tif_rawdataoff, 0U);
    EXPECT_THAT(row_buf, ::testing::ElementsAreArray(pixels.data(), 4));
  }

  // Same reload path with FILLORDER_LSB2MSB ("rmL") exercises bit reversal.
  {
    UniqueTiff reader = stream.Open("rmL");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 4> row_buf{};
    ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 2, 0), 1);
    reader->tif_rawdataoff = 4;
    ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 0, 0), 1);
    for (size_t i = 0; i < row_buf.size(); ++i) {
      EXPECT_EQ(row_buf[i], ReverseByteBits(pixels[i]));
    }
  }

  // Seek error inside TIFFFillStripPartial.
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 4> row_buf{};
    ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 2, 0), 1);
    reader->tif_rawdataoff = 4;
    stream.FailSeeksAfter(0);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 0, 0), -1);
    stream.ResetFaults();
  }

  // Read error inside TIFFFillStripPartial.
  {
    UniqueTiff reader = stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 4> row_buf{};
    ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 2, 0), 1);
    reader->tif_rawdataoff = 4;
    stream.FailReadsAfter(0);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 0, 0), -1);
    stream.ResetFaults();
  }

  // Separate-plane strip index uint64->uint32 overflow guard in TIFFSeek.
  {
    constexpr uint16_t kSpp = 3;
    std::vector<uint8_t> sep_pixels(kWidth * 2 * kSpp, 0x7E);
    MemoryTiffStream sep_stream;
    WriteStripedImage(sep_stream, "w", kWidth, 2, 8, kSpp, /*rows_per_strip=*/2,
                      COMPRESSION_NONE, PLANARCONFIG_SEPARATE,
                      FILLORDER_MSB2LSB, sep_pixels);
    UniqueTiff reader = sep_stream.Open("rm");
    ASSERT_NE(reader, nullptr);
    const uint32_t saved_strips_per_image = reader->tif_dir.td_stripsperimage;
    auto restore = absl::MakeCleanup(
        [&] { reader->tif_dir.td_stripsperimage = saved_strips_per_image; });
    reader->tif_dir.td_stripsperimage = 0x80000000U;
    std::array<uint8_t, 4> row_buf{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 0, 2), -1);
  }
}

}  // namespace
}  // namespace libtiff
