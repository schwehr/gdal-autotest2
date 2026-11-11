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

// Unit tests for libtiff LZW compression/decompression (tif_lzw.c).
//
// Exercises standard MSB-first Aldus LZW encode/decode across strips, tiles,
// scanlines, predictors (horizontal and floating-point), ClassicTIFF/BigTIFF,
// endianness, bit-width transitions (9->10->11->12) and CODE_CLEAR table
// resets, compression ratio slippage adaptive resets, old-style LSB-first
// LZW_COMPAT streams, corrupt/truncated streams, OOM handling, and read/write
// mode state transitions.
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_lzw.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

constexpr uint16_t kLzwCodeClear = 256;
constexpr uint16_t kLzwCodeEoi = 257;
constexpr uint16_t kLzwCodeFirst = 258;

// Deterministic xorshift32 byte generator for reproducible pseudorandom tests.
std::vector<uint8_t> MakePseudoRandomBytes(size_t count,
                                           uint32_t seed = 0x12345678u) {
  std::vector<uint8_t> out(count);
  uint32_t state = seed;
  for (size_t i = 0; i < count; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    out[i] = static_cast<uint8_t>((state >> 16) & 0xffu);
  }
  return out;
}

// Generates up to ~65,000 bytes where no consecutive 2-byte bigram repeats,
// guaranteeing LZW adds exactly one new dictionary entry per byte after byte 0.
std::vector<uint8_t> MakeUniqueBigramBytes(size_t count) {
  std::vector<bool> seen(65536, false);
  std::vector<uint8_t> out;
  out.reserve(count);
  if (count == 0) return out;
  uint8_t prev = 0;
  out.push_back(prev);
  for (size_t i = 1; i < count; ++i) {
    bool found = false;
    for (int cand = 0; cand < 256; ++cand) {
      const uint8_t next = static_cast<uint8_t>((prev + cand * 13 + 7) & 0xff);
      const size_t idx = (static_cast<size_t>(prev) << 8) | next;
      if (!seen[idx]) {
        seen[idx] = true;
        out.push_back(next);
        prev = next;
        found = true;
        break;
      }
    }
    if (!found) break;
  }
  return out;
}

// Clean-room MSB-first LZW bitstream writer for crafting valid edge-case and
// malformed standard TIFF LZW payloads.
class MsbLzwBitWriter {
 public:
  void WriteBits(uint32_t code, int nbits) {
    acc_ = (acc_ << nbits) | static_cast<uint64_t>(code);
    bits_ += nbits;
    while (bits_ >= 8) {
      bits_ -= 8;
      out_.push_back(static_cast<uint8_t>((acc_ >> bits_) & 0xffu));
    }
  }

  std::vector<uint8_t> Finish(bool flush_partial = true) {
    if (flush_partial && bits_ > 0) {
      out_.push_back(static_cast<uint8_t>((acc_ << (8 - bits_)) & 0xffu));
    }
    acc_ = 0;
    bits_ = 0;
    return std::move(out_);
  }

 private:
  std::vector<uint8_t> out_;
  uint64_t acc_ = 0;
  int bits_ = 0;
};

// Clean-room LSB-first bit writer matching old-style LZW_COMPAT bit packing.
class LsbLzwBitWriter {
 public:
  void WriteBits(uint32_t code, int nbits) {
    acc_ |= static_cast<uint64_t>(code) << bits_;
    bits_ += nbits;
    while (bits_ >= 8) {
      out_.push_back(static_cast<uint8_t>(acc_ & 0xffu));
      acc_ >>= 8;
      bits_ -= 8;
    }
  }

  std::vector<uint8_t> Finish(bool flush_partial = true) {
    if (flush_partial && bits_ > 0) {
      out_.push_back(static_cast<uint8_t>(acc_ & 0xffu));
    }
    acc_ = 0;
    bits_ = 0;
    return std::move(out_);
  }

 private:
  std::vector<uint8_t> out_;
  uint64_t acc_ = 0;
  int bits_ = 0;
};

// Encodes raw bytes using old-style LSB-first LZW_COMPAT code-width rules
// (transition at free_ent > (1 << nbits) - 1, reset at 4096).
std::vector<uint8_t> EncodeLzwCompat(absl::Span<const uint8_t> input,
                                     bool append_eoi = true) {
  LsbLzwBitWriter writer;
  constexpr int kMaxTable = 4096;

  int nbits = 9;
  int maxcode = (1 << nbits) - 1;
  int free_ent = kLzwCodeFirst;
  std::vector<int> table(kMaxTable * 256, -1);

  writer.WriteBits(kLzwCodeClear, nbits);
  if (input.empty()) {
    if (append_eoi) writer.WriteBits(kLzwCodeEoi, nbits);
    return writer.Finish();
  }

  int ent = input[0];
  bool after_clear = true;
  for (size_t i = 1; i < input.size(); ++i) {
    const uint8_t c = input[i];
    const int key = (ent << 8) | c;
    if (table[key] >= 0) {
      ent = table[key];
      continue;
    }
    writer.WriteBits(static_cast<uint32_t>(ent), nbits);
    after_clear = false;
    if (free_ent < kMaxTable) {
      table[key] = free_ent;
    }
    ++free_ent;
    ent = c;
    // Decoder adds an entry one code later than encoder (first code after
    // CODE_CLEAR does not add a table entry in LZWDecodeCompat), so decoder's
    // post-increment free_entp equals encoder's (free_ent - 1).
    if (free_ent > kMaxTable) {
      // Decoder just wrote entry 4095 and clamped nbits to 12; emit CODE_CLEAR.
      writer.WriteBits(kLzwCodeClear, nbits);
      std::fill(table.begin(), table.end(), -1);
      free_ent = kLzwCodeFirst;
      nbits = 9;
      maxcode = (1 << nbits) - 1;
      after_clear = true;
    } else if (free_ent - 1 > maxcode) {
      ++nbits;
      maxcode = (1 << nbits) - 1;
    }
  }

  writer.WriteBits(static_cast<uint32_t>(ent), nbits);
  if (!after_clear && free_ent > maxcode && nbits < 12) {
    ++nbits;
  }
  if (append_eoi) {
    writer.WriteBits(kLzwCodeEoi, nbits);
  }
  return writer.Finish();
}

std::vector<uint8_t> BuildSingleStripLzwTiff(uint32_t width, uint32_t height,
                                             absl::Span<const uint8_t> raw_lzw,
                                             uint16_t predictor = 1,
                                             uint32_t rows_per_strip = 0) {
  if (rows_per_strip == 0) rows_per_strip = height;
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const uint64_t off = builder.AppendBlob(raw_lzw);
  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_LZW),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                       static_cast<uint32_t>(raw_lzw.size())),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
  };
  if (predictor != 1) {
    entries.push_back(builder.MakeShort(TIFFTAG_PREDICTOR, predictor));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

void ConfigureBasicStripTags(TIFF* tif, uint32_t width, uint32_t height,
                             uint16_t bps, uint16_t spp,
                             uint32_t rows_per_strip,
                             uint16_t planar = PLANARCONFIG_CONTIG) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, spp), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar), 1);
}

TEST(TifLzwTest, CodecIsConfiguredAndMaxRatioReported) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_LZW), 1);
  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_LZW);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_LZW);
  EXPECT_NE(codec->name, nullptr);

  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW), 1);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 1400ULL);
}

TEST(TifLzwTest, CleanupOnCompressionSwitchAndRoundTrip) {
  MemoryTiffStream stream;
  {
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ConfigureBasicStripTags(tif.get(), 8, 4, 8, 1, 4);

    // Switching compression triggers LZWCleanup and restores defaults.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 1ULL);

    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW), 1);
    EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 1400ULL);

    std::vector<uint8_t> strip(32, 0x42);
    ASSERT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, strip.data(),
                                    static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(strip.size()));
    ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  uint16_t comp = 0;
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &comp), 1);
  EXPECT_EQ(comp, COMPRESSION_LZW);
  std::vector<uint8_t> out(32, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                 static_cast<tmsize_t>(out.size())),
            32);
  EXPECT_EQ(out, std::vector<uint8_t>(32, 0x42));
}

TEST(TifLzwTest, InitSetupEncodeAndDecodeOomFailures) {
  // 1. TIFFInitLZW OOM when allocating LZWCodecState.
  {
    MemoryTiffStream stream;
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    UniqueTiff tif = stream.Open("w", opts.get());
    ASSERT_NE(tif, nullptr);
    ScopedSingleMemAllocLimit limit(tif.get(), 8);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW), 0);
    EXPECT_GT(recorder.error_count(), 0u);
  }

  // 2. LZWSetupEncode OOM when allocating enc_hashtab.
  {
    MemoryTiffStream stream;
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    UniqueTiff tif = stream.Open("w", opts.get());
    ASSERT_NE(tif, nullptr);
    ConfigureBasicStripTags(tif.get(), 16, 4, 8, 1, 4);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 4096), 1);

    ScopedSingleMemAllocLimit limit(tif.get(), 4096);
    std::vector<uint8_t> strip(64, 0x11);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, strip.data(),
                                    static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(recorder.error_count(), 0u);
  }

  // 3. LZWSetupDecode OOM when allocating dec_codetab.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripTags(writer.get(), 16, 4, 8, 1, 4);
      std::vector<uint8_t> strip(64, 0x22);
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                      static_cast<tmsize_t>(strip.size())),
                64);
    }

    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    UniqueTiff reader = stream.Open("r", opts.get());
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFReadBufferSetup(reader.get(), nullptr, 4096), 1);
    ScopedSingleMemAllocLimit limit(reader.get(), 4096);
    std::vector<uint8_t> out(64, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(recorder.error_count(), 0u);
  }
}

TEST(TifLzwTest, StripAndScanlineRoundTripEndianAndBigTiff) {
  struct FormatCase {
    const char* open_mode;
    uint16_t bps;
    uint16_t spp;
    uint32_t width;
    uint32_t height;
    uint32_t rows_per_strip;
  };
  constexpr std::array<FormatCase, 4> kCases = {{
      {.open_mode = "wl",
       .bps = 8,
       .spp = 1,
       .width = 23,
       .height = 13,
       .rows_per_strip = 4},
      {.open_mode = "wb",
       .bps = 16,
       .spp = 1,
       .width = 19,
       .height = 11,
       .rows_per_strip = 4},
      {.open_mode = "w8l",
       .bps = 8,
       .spp = 3,
       .width = 17,
       .height = 9,
       .rows_per_strip = 3},
      {.open_mode = "w8b",
       .bps = 32,
       .spp = 1,
       .width = 11,
       .height = 7,
       .rows_per_strip = 3},
  }};

  for (size_t idx = 0; idx < kCases.size(); ++idx) {
    SCOPED_TRACE(idx);
    const FormatCase& tc = kCases[idx];
    const size_t row_bytes =
        static_cast<size_t>(tc.width) * tc.spp * (tc.bps / 8);
    const std::vector<uint8_t> expected = MakePseudoRandomBytes(
        row_bytes * tc.height, 0x9001u + static_cast<uint32_t>(idx));

    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(tc.open_mode);
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripTags(writer.get(), tc.width, tc.height, tc.bps, tc.spp,
                              tc.rows_per_strip);
      for (uint32_t row = 0; row < tc.height; ++row) {
        // Pass a mutable copy because TIFFWriteScanline swaps in place.
        std::vector<uint8_t> row_buf(expected.begin() + row * row_bytes,
                                     expected.begin() + (row + 1) * row_bytes);
        ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), row, 0), 1);
      }
    }

    // Read back with mmap enabled on one case and normal read on others.
    stream.set_enable_mmap(idx == 0);
    UniqueTiff reader = stream.Open(idx == 0 ? "rm" : "r");
    ASSERT_NE(reader, nullptr);
    uint16_t comp = 0;
    ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &comp), 1);
    EXPECT_EQ(comp, COMPRESSION_LZW);

    std::vector<uint8_t> actual(expected.size(), 0);
    for (uint32_t row = 0; row < tc.height; ++row) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), actual.data() + row * row_bytes,
                                 row, 0),
                1);
    }
    EXPECT_EQ(actual, expected);

    // Also test backward scanline seek (rewinds strip) and forward skip failure
    // within the same strip (rows_per_strip >= 3 for all test cases).
    std::vector<uint8_t> rewind_row(row_bytes, 0);
    ASSERT_EQ(TIFFReadScanline(reader.get(), rewind_row.data(), 0, 0), 1);
    EXPECT_TRUE(
        std::equal(rewind_row.begin(), rewind_row.end(), expected.begin()));
    EXPECT_EQ(TIFFReadScanline(reader.get(), rewind_row.data(), 2, 0), -1);
  }
}

TEST(TifLzwTest, TiledRoundTripAndPlanarSeparate) {
  for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
    SCOPED_TRACE(planar);
    constexpr uint32_t kWidth = 40;
    constexpr uint32_t kHeight = 35;
    constexpr uint32_t kTileW = 16;
    constexpr uint32_t kTileH = 16;
    constexpr uint16_t kSpp = 3;

    MemoryTiffStream stream;
    std::vector<std::vector<uint8_t>> expected_tiles;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kHeight), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, kSpp), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, planar), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, kTileW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, kTileH), 1);

      const uint32_t num_tiles = TIFFNumberOfTiles(writer.get());
      const tmsize_t tile_size = TIFFTileSize(writer.get());
      ASSERT_GT(tile_size, 0);
      expected_tiles.resize(num_tiles);
      for (uint32_t t = 0; t < num_tiles; ++t) {
        expected_tiles[t] =
            MakePseudoRandomBytes(static_cast<size_t>(tile_size), 0x4200u + t);
        std::vector<uint8_t> mutable_tile = expected_tiles[t];
        ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), t, mutable_tile.data(),
                                       tile_size),
                  tile_size);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    const uint32_t num_tiles = TIFFNumberOfTiles(reader.get());
    ASSERT_EQ(num_tiles, expected_tiles.size());
    const tmsize_t tile_size = TIFFTileSize(reader.get());
    for (uint32_t t = 0; t < num_tiles; ++t) {
      std::vector<uint8_t> out(static_cast<size_t>(tile_size), 0);
      ASSERT_EQ(TIFFReadEncodedTile(reader.get(), t, out.data(), tile_size),
                tile_size);
      EXPECT_EQ(out, expected_tiles[t]);
    }
  }
}

TEST(TifLzwTest, PredictorHorizontalAndFloatingPointRoundTrips) {
  // Horizontal differencing across 8-bit/16-bit/32-bit and little/big endian,
  // both stripped (scanlines) and tiled.
  for (const char* mode : {"wl", "wb"}) {
    for (uint16_t bps : {uint16_t{8}, uint16_t{16}, uint16_t{32}}) {
      SCOPED_TRACE(mode);
      SCOPED_TRACE(bps);
      constexpr uint32_t kWidth = 32;
      constexpr uint32_t kHeight = 8;
      constexpr uint16_t kSpp = 3;
      const size_t row_bytes = static_cast<size_t>(kWidth) * kSpp * (bps / 8);
      std::vector<uint8_t> expected(row_bytes * kHeight);
      for (size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<uint8_t>((i * 5 + (i >> 3)) & 0xffu);
      }

      MemoryTiffStream stream;
      {
        UniqueTiff writer = stream.Open(mode);
        ASSERT_NE(writer, nullptr);
        ConfigureBasicStripTags(writer.get(), kWidth, kHeight, bps, kSpp, 4);
        ASSERT_EQ(
            TIFFSetField(writer.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL),
            1);
        for (uint32_t r = 0; r < kHeight; ++r) {
          std::vector<uint8_t> row_buf(expected.begin() + r * row_bytes,
                                       expected.begin() + (r + 1) * row_bytes);
          ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), r, 0), 1);
        }
      }

      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      uint16_t pred = 0;
      ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_PREDICTOR, &pred), 1);
      EXPECT_EQ(pred, PREDICTOR_HORIZONTAL);
      std::vector<uint8_t> actual(expected.size(), 0);
      for (uint32_t r = 0; r < kHeight; ++r) {
        ASSERT_EQ(
            TIFFReadScanline(reader.get(), actual.data() + r * row_bytes, r, 0),
            1);
      }
      EXPECT_EQ(actual, expected);
    }
  }

  // Floating point predictor (32-bit IEEE float and 64-bit IEEE double) in
  // both endiannesses with tiled and stripped layouts.
  for (const char* mode : {"wl", "wb"}) {
    SCOPED_TRACE(mode);
    constexpr uint32_t kWidth = 32;
    constexpr uint32_t kHeight = 16;
    std::vector<float> expected(kWidth * kHeight);
    for (size_t i = 0; i < expected.size(); ++i) {
      expected[i] = static_cast<float>(i) * 0.125f - 10.5f;
    }

    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(mode);
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kHeight), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, 32), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP),
          1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             PHOTOMETRIC_MINISBLACK),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, 16), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, 16), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PREDICTOR,
                             PREDICTOR_FLOATINGPOINT),
                1);

      for (uint32_t tx = 0; tx < kWidth; tx += 16) {
        std::vector<float> tile(16 * 16);
        for (uint32_t y = 0; y < 16; ++y) {
          for (uint32_t x = 0; x < 16; ++x) {
            tile[y * 16 + x] = expected[y * kWidth + tx + x];
          }
        }
        const uint32_t tile_idx = TIFFComputeTile(writer.get(), tx, 0, 0, 0);
        ASSERT_EQ(TIFFWriteEncodedTile(
                      writer.get(), tile_idx, tile.data(),
                      static_cast<tmsize_t>(tile.size() * sizeof(float))),
                  static_cast<tmsize_t>(tile.size() * sizeof(float)));
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    for (uint32_t tx = 0; tx < kWidth; tx += 16) {
      std::vector<float> out_tile(16 * 16, 0.0f);
      const uint32_t tile_idx = TIFFComputeTile(reader.get(), tx, 0, 0, 0);
      ASSERT_EQ(TIFFReadEncodedTile(
                    reader.get(), tile_idx, out_tile.data(),
                    static_cast<tmsize_t>(out_tile.size() * sizeof(float))),
                static_cast<tmsize_t>(out_tile.size() * sizeof(float)));
      for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
          EXPECT_FLOAT_EQ(out_tile[y * 16 + x], expected[y * kWidth + tx + x]);
        }
      }
    }
  }
}

TEST(TifLzwTest, LargePseudorandomCodeWidthsAndTableResets) {
  // 96 KiB single strip of pseudorandom data forces 9->10->11->12 bit code
  // transitions, multiple CODE_CLEAR table resets at CODE_MAX-1, hash
  // secondary probing with wraparound, and TIFFFlushData1 buffer flushes.
  constexpr uint32_t kWidth = 256;
  constexpr uint32_t kHeight = 384;
  const std::vector<uint8_t> expected =
      MakePseudoRandomBytes(static_cast<size_t>(kWidth) * kHeight, 0xCAFEBABEu);

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripTags(writer.get(), kWidth, kHeight, 8, 1, kHeight);
    std::vector<uint8_t> strip = expected;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                    static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(strip.size()));
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> actual(expected.size(), 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, actual.data(),
                                 static_cast<tmsize_t>(actual.size())),
            static_cast<tmsize_t>(actual.size()));
  EXPECT_EQ(actual, expected);
}

TEST(TifLzwTest, CompressionRatioAdaptiveResetAndImprovement) {
  // Case A: First 10,500 zeros establish high enc_ratio at checkpoint=10000;
  // then 3,000 random bytes followed by 7,500 zeros drop compression ratio at
  // checkpoint=20000 below sp->enc_ratio, triggering adaptive CODE_CLEAR reset.
  {
    std::vector<uint8_t> data(10500, 0x00);
    const std::vector<uint8_t> noise = MakePseudoRandomBytes(3000, 0x7777u);
    data.insert(data.end(), noise.begin(), noise.end());
    data.insert(data.end(), 7500, 0x00);

    const uint32_t width = static_cast<uint32_t>(data.size());
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripTags(writer.get(), width, 1, 8, 1, 1);
      std::vector<uint8_t> strip = data;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                      static_cast<tmsize_t>(strip.size())),
                static_cast<tmsize_t>(strip.size()));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(data.size(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(out.size()));
    EXPECT_EQ(out, data);
  }

  // Case B: 25,000 constant bytes keep compression ratio increasing across
  // checkpoints 10000 and 20000 (hits `sp->enc_ratio = rat;`).
  {
    constexpr uint32_t kWidth = 25000;
    const std::vector<uint8_t> data(kWidth, 0x3C);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripTags(writer.get(), kWidth, 1, 8, 1, 1);
      std::vector<uint8_t> strip = data;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                      static_cast<tmsize_t>(strip.size())),
                static_cast<tmsize_t>(strip.size()));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(kWidth, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(out.size()));
    EXPECT_EQ(out, data);
  }
}

TEST(TifLzwTest, EncodeBoundaryLengthsPostEncodeAndFlushFailures) {
  // Exact lengths of unique-bigram inputs hit:
  // - 6 bytes: nextbits == 0 at LZWPostEncode EOI tail check
  // - 254 bytes: free_ent == 511 -> ++free_ent > maxcode in LZWPostEncode
  // - 3836 bytes: free_ent == 4093 -> ++free_ent == CODE_MAX-1 in LZWPostEncode
  for (uint32_t len :
       {uint32_t{1}, uint32_t{6}, uint32_t{254}, uint32_t{3836}}) {
    SCOPED_TRACE(len);
    const std::vector<uint8_t> expected = MakeUniqueBigramBytes(len);
    ASSERT_EQ(expected.size(), len);

    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripTags(writer.get(), len, 1, 8, 1, 1);
      std::vector<uint8_t> strip = expected;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                      static_cast<tmsize_t>(strip.size())),
                static_cast<tmsize_t>(strip.size()));
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(len, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(len));
    EXPECT_EQ(out, expected);
  }

  // Zero-byte encode hits cc == 0 in LZWEncode and enc_oldcode == -1 in
  // LZWPostEncode.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripTags(writer.get(), 4, 1, 8, 1, 1);
    uint8_t dummy = 0;
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, &dummy, 0), 0);
  }

  // Trigger TIFFFlushData1 failure inside LZWEncode (op > limit).
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    const std::vector<uint8_t> data = MakePseudoRandomBytes(20000, 0xABCDu);
    ConfigureBasicStripTags(writer.get(), 20000, 1, 8, 1, 1);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 4096), 1);
    stream.FailWritesAfter(0);
    std::vector<uint8_t> strip = data;
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                    static_cast<tmsize_t>(strip.size())),
              static_cast<tmsize_t>(-1));
  }

  // Trigger op > sp->enc_rawlimit inside LZWPostEncode (both success and
  // TIFFFlushData1 failure).
  // With a 4096-byte raw buffer, enc_rawlimit = rawdata + 4091. Sweeping
  // scanline counts hits the exact boundary where LZWEncode ends at 4092..4094,
  // causing TIFFFlushData to invoke WriteProc twice (once in LZWPostEncode and
  // once for the trailing EOI byte in TIFFFlushData1).
  uint32_t postencode_flush_rows = 0;
  for (uint32_t rows = 2950; rows < 2980 && postencode_flush_rows == 0;
       ++rows) {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripTags(writer.get(), 1, rows, 8, 1, rows);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 4096), 1);
    const size_t header_writes = stream.write_calls();
    const std::vector<uint8_t> data = MakeUniqueBigramBytes(rows);
    for (uint32_t r = 0; r < rows; ++r) {
      uint8_t px = data[r];
      ASSERT_EQ(TIFFWriteScanline(writer.get(), &px, r, 0), 1);
    }
    if (stream.write_calls() == header_writes) {
      ASSERT_EQ(TIFFFlushData(writer.get()), 1);
      if (stream.write_calls() == header_writes + 2) {
        postencode_flush_rows = rows;
      }
    }
  }
  ASSERT_GT(postencode_flush_rows, 0u);

  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripTags(writer.get(), 1, postencode_flush_rows, 8, 1,
                            postencode_flush_rows);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 4096), 1);
    const size_t header_writes = stream.write_calls();
    const std::vector<uint8_t> data =
        MakeUniqueBigramBytes(postencode_flush_rows);
    for (uint32_t r = 0; r < postencode_flush_rows; ++r) {
      uint8_t px = data[r];
      ASSERT_EQ(TIFFWriteScanline(writer.get(), &px, r, 0), 1);
    }
    ASSERT_EQ(stream.write_calls(), header_writes);
    stream.FailWritesAfter(header_writes);
    EXPECT_EQ(TIFFFlushData(writer.get()), 0);
  }
}

TEST(TifLzwTest, DecodeRestartRepeatedAndNonRepeatedStrings) {
  // 1. Constant 0x55 across 64 1-byte rows: covers code_clear occ==0,
  // len==2/3/4+ too_short_buffer, dec_restart residue > occ and <= occ, and
  // repeated string memset optimization.
  {
    constexpr uint32_t kWidth = 1;
    constexpr uint32_t kHeight = 64;
    const std::vector<uint8_t> expected(kHeight, 0x55);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripTags(writer.get(), kWidth, kHeight, 8, 1, kHeight);
      std::vector<uint8_t> strip = expected;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                      static_cast<tmsize_t>(strip.size())),
                static_cast<tmsize_t>(kHeight));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    for (uint32_t r = 0; r < kHeight; ++r) {
      uint8_t px = 0;
      ASSERT_EQ(TIFFReadScanline(reader.get(), &px, r, 0), 1);
      EXPECT_EQ(px, 0x55);
    }
    // Also test occ == 0 inside LZWDecode via 0-size strip read.
    uint8_t dummy = 0;
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, &dummy, 0), 0);
  }

  // 2. Width 2 and 3 with constant data: covers len==2 (occ==2) and len==3
  // (occ==3) exact-fit fast paths, plus repeated len>=4 with occ==0 and >0.
  for (uint32_t width : {uint32_t{2}, uint32_t{3}, uint32_t{16}}) {
    SCOPED_TRACE(width);
    constexpr uint32_t kHeight = 32;
    const std::vector<uint8_t> expected(width * kHeight, 0xA3);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripTags(writer.get(), width, kHeight, 8, 1, kHeight);
      std::vector<uint8_t> strip = expected;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                      static_cast<tmsize_t>(strip.size())),
                static_cast<tmsize_t>(expected.size()));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> row_buf(width, 0);
    for (uint32_t r = 0; r < kHeight; ++r) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), r, 0), 1);
      EXPECT_EQ(row_buf, std::vector<uint8_t>(width, 0xA3));
    }
  }

  // 3. Non-constant repeating pattern ("ABCDEFG") across 32 rows of width 21:
  // covers non-repeated len==4 and len>=5 unrolled copy loop (tp > op) and
  // occ == len / occ > len transitions.
  {
    constexpr uint32_t kWidth = 21;
    constexpr uint32_t kHeight = 32;
    std::vector<uint8_t> expected(kWidth * kHeight);
    for (size_t i = 0; i < expected.size(); ++i) {
      expected[i] = static_cast<uint8_t>('A' + (i % 7));
    }
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripTags(writer.get(), kWidth, kHeight, 8, 1, kHeight);
      std::vector<uint8_t> strip = expected;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(),
                                      static_cast<tmsize_t>(strip.size())),
                static_cast<tmsize_t>(expected.size()));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> actual(expected.size(), 0);
    for (uint32_t r = 0; r < kHeight; ++r) {
      ASSERT_EQ(
          TIFFReadScanline(reader.get(), actual.data() + r * kWidth, r, 0), 1);
    }
    EXPECT_EQ(actual, expected);
  }
}

TEST(TifLzwTest, DecodeCorruptAndTruncatedMsbStreams) {
  auto expect_decode_fails = [](absl::Span<const uint8_t> payload,
                                uint32_t width, uint32_t height,
                                bool check_second_scanline = false) {
    MemoryTiffStream stream(
        BuildSingleStripLzwTiff(width, height, payload, 1, height));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    if (check_second_scanline) {
      std::vector<uint8_t> row(width, 0xff);
      EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
      EXPECT_EQ(row, std::vector<uint8_t>(width, 0));
      // Second scanline in same strip hits sp->read_error branch.
      std::fill(row.begin(), row.end(), 0xee);
      EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 1, 0), -1);
      EXPECT_EQ(row, std::vector<uint8_t>(width, 0));
    } else {
      std::vector<uint8_t> out(static_cast<size_t>(width) * height, 0xff);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                     static_cast<tmsize_t>(out.size())),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_GT(stream.diagnostics().error_count(), 0u);
  };

  // (a) Missing initial CODE_CLEAR: literal byte hits codep > free_entp.
  {
    MsbLzwBitWriter w;
    w.WriteBits('A', 9);
    w.WriteBits(kLzwCodeEoi, 9);
    expect_decode_fails(w.Finish(), 4, 2, /*check_second_scanline=*/true);
  }

  // (b) Code > CODE_EOI immediately after CODE_CLEAR.
  {
    MsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits(260, 9);
    w.WriteBits(kLzwCodeEoi, 9);
    expect_decode_fails(w.Finish(), 4, 1);
  }

  // (c) Consecutive CODE_CLEAR codes followed by CODE_EOI (short data).
  {
    MsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits(kLzwCodeEoi, 9);
    expect_decode_fails(w.Finish(), 4, 1);
  }

  // (d) Out-of-table code > free_entp after initial literal.
  {
    MsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits('A', 9);
    w.WriteBits(300, 9);
    w.WriteBits(kLzwCodeEoi, 9);
    expect_decode_fails(w.Finish(), 4, 1);
  }

  // (e) Early CODE_EOI before occ bytes satisfied.
  {
    MsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits('A', 9);
    w.WriteBits('B', 9);
    w.WriteBits(kLzwCodeEoi, 9);
    expect_decode_fails(w.Finish(), 8, 1);
  }

  // (f) Truncated 9-bit stream without CODE_EOI (< 8 bits left -> no_eoi).
  {
    MsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits('A', 9);
    expect_decode_fails(w.Finish(), 8, 1);
  }

  // (g) Truncated 10-bit stream leaving 1 trailing byte where GetNextCodeLZW
  // needs a second byte (hits inner `if (dec_bitsleft < 8) goto no_eoi`).
  {
    MsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    for (int i = 0; i < 254; ++i) {
      w.WriteBits(static_cast<uint32_t>(i & 0xff), 9);
    }
    // 255 * 9 = 2295 bits -> flush partial gives 287 bytes (1 bit left).
    std::vector<uint8_t> payload = w.Finish(/*flush_partial=*/true);
    // Append 1 extra byte so nextbits=1 at 10-bit read consumes 1 byte
    // (nextbits=-1), then finds dec_bitsleft==0 < 8.
    payload.push_back(0x00);
    expect_decode_fails(payload, 512, 1);
  }

  // (h) Stream without CODE_CLEAR overflowing CSIZE via code_below_256
  // (covers ++nbits > BITS_MAX and free_entp >= &dec_codetab[CSIZE]).
  {
    MsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    int nbits = 9;
    int next_bump = (1 << nbits) - 2;
    int free_ent = kLzwCodeFirst;
    w.WriteBits(0, nbits);
    for (int i = 1; i <= 4900; ++i) {
      w.WriteBits(static_cast<uint32_t>(i & 0xff), nbits);
      if (++free_ent > next_bump && nbits < 12) {
        ++nbits;
        next_bump = (1 << nbits) - 2;
      }
    }
    w.WriteBits(kLzwCodeEoi, 12);
    expect_decode_fails(w.Finish(), 5500, 1);
  }

  // (i) Stream without CODE_CLEAR overflowing CSIZE via code >= CODE_FIRST
  // (repeatedly emitting code 258 after defining 258="AB" overflows CSIZE
  // with bounded output size).
  {
    MsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    int nbits = 9;
    int next_bump = (1 << nbits) - 2;
    int free_ent = kLzwCodeFirst;
    w.WriteBits('A', nbits);
    w.WriteBits('B', nbits);
    ++free_ent;  // 258 = "AB"
    w.WriteBits('A', nbits);
    ++free_ent;  // 259 = "BA"
    for (int i = 0; i < 4870; ++i) {
      w.WriteBits(kLzwCodeFirst, nbits);
      if (++free_ent > next_bump && nbits < 12) {
        ++nbits;
        next_bump = (1 << nbits) - 2;
      }
    }
    w.WriteBits(kLzwCodeEoi, 12);
    expect_decode_fails(w.Finish(), 11000, 1);
  }
}

TEST(TifLzwTest, DecodeOldStyleLzwCompatValidAndErrorStreams) {
  // 1. Multi-strip large old-style LZW_COMPAT stream (> 16 KiB) with
  // 9->10->11->12 bit transitions and table resets at 4096. Strip 0 triggers
  // warning with dec_decode==NULL; strip 1 exercises reuse of
  // dec_decode==LZWDecodeCompat.
  {
    constexpr uint32_t kWidth = 256;
    constexpr uint32_t kHeight = 96;
    constexpr uint32_t kRowsPerStrip = 48;
    const std::vector<uint8_t> strip0_plain =
        MakePseudoRandomBytes(kWidth * kRowsPerStrip, 0x1111u);
    const std::vector<uint8_t> strip1_plain =
        MakePseudoRandomBytes(kWidth * kRowsPerStrip, 0x2222u);
    const std::vector<uint8_t> strip0_lzw = EncodeLzwCompat(strip0_plain);
    const std::vector<uint8_t> strip1_lzw = EncodeLzwCompat(strip1_plain);
    ASSERT_EQ(strip0_lzw[0], 0x00);
    ASSERT_EQ(strip0_lzw[1] & 0x1u, 0x1u);

    RawTiffBuilder builder;
    const uint64_t off0 = builder.AppendBlob(strip0_lzw);
    const uint64_t off1 = builder.AppendBlob(strip1_lzw);
    const std::array<uint32_t, 2> offs = {static_cast<uint32_t>(off0),
                                          static_cast<uint32_t>(off1)};
    const std::array<uint32_t, 2> counts = {
        static_cast<uint32_t>(strip0_lzw.size()),
        static_cast<uint32_t>(strip1_lzw.size())};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, kWidth),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, kHeight),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_LZW),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offs),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, kRowsPerStrip),
        builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, counts),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });

    MemoryTiffStream stream(builder.Take());
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    std::vector<uint8_t> out0(strip0_plain.size(), 0);
    std::vector<uint8_t> out1(strip1_plain.size(), 0);
    auto format_errors = [&]() {
      std::string out;
      for (const DiagnosticRecord& rec : stream.diagnostics().errors()) {
        out += rec.module + ": " + rec.message + "; ";
      }
      return out;
    };
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out0.data(),
                                   static_cast<tmsize_t>(out0.size())),
              static_cast<tmsize_t>(out0.size()))
        << format_errors();
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 1, out1.data(),
                                   static_cast<tmsize_t>(out1.size())),
              static_cast<tmsize_t>(out1.size()));
    EXPECT_EQ(out0, strip0_plain);
    EXPECT_EQ(out1, strip1_plain);
    EXPECT_EQ(stream.diagnostics().warning_count(), 1u);
    EXPECT_EQ(stream.diagnostics().error_count(), 0u);
  }

  // 2. Old-style LZW_COMPAT scanlines with dec_restart (residue > occ and
  // residue <= occ) plus horizontal predictor re-hook in LZWPreDecode.
  {
    constexpr uint32_t kWidth = 1;
    constexpr uint32_t kHeight = 40;
    const std::vector<uint8_t> expected(kHeight, 0x6D);
    // Horizontal difference of constant 0x6D with width=1 (stride=1): each row
    // is a single pixel so row-by-row horDiff8 leaves 0x6D unchanged.
    const std::vector<uint8_t> lzw = EncodeLzwCompat(expected);
    MemoryTiffStream stream(BuildSingleStripLzwTiff(
        kWidth, kHeight, lzw, PREDICTOR_HORIZONTAL, kHeight));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    for (uint32_t r = 0; r < kHeight; ++r) {
      uint8_t px = 0;
      ASSERT_EQ(TIFFReadScanline(reader.get(), &px, r, 0), 1);
      EXPECT_EQ(px, 0x6D);
    }
    EXPECT_EQ(stream.diagnostics().warning_count(), 1u);
    EXPECT_EQ(stream.diagnostics().error_count(), 0u);
  }

  // 3. Old-style LZW_COMPAT with consecutive CODE_CLEARs and EOI, and with
  // missing EOI / corrupt codes / CSIZE overflow.
  {
    // Consecutive CODE_CLEARs then valid data + CODE_EOI.
    LsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits('X', 9);
    w.WriteBits('Y', 9);
    w.WriteBits(kLzwCodeEoi, 9);
    const std::vector<uint8_t> payload = w.Finish();
    MemoryTiffStream stream(BuildSingleStripLzwTiff(2, 1, payload, 1, 1));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 2> out{};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 2), 2);
    EXPECT_EQ(out[0], 'X');
    EXPECT_EQ(out[1], 'Y');
  }

  // Missing EOI in LZW_COMPAT warns via NextCode and fails short-data check.
  {
    const std::vector<uint8_t> lzw =
        EncodeLzwCompat(std::array<uint8_t, 2>{'A', 'B'}, /*append_eoi=*/false);
    MemoryTiffStream stream(BuildSingleStripLzwTiff(8, 1, lzw, 1, 1));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(8, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 8),
              static_cast<tmsize_t>(-1));
    EXPECT_GE(stream.diagnostics().warning_count(), 2u);
    EXPECT_GT(stream.diagnostics().error_count(), 0u);
  }

  // CODE_CLEAR immediately followed by code > CODE_CLEAR (258).
  {
    LsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits(258, 9);
    w.WriteBits(kLzwCodeEoi, 9);
    MemoryTiffStream stream(BuildSingleStripLzwTiff(4, 1, w.Finish(), 1, 1));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(4, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(stream.diagnostics().error_count(), 0u);
  }

  // CODE_CLEAR immediately followed by CODE_EOI.
  {
    LsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits(kLzwCodeEoi, 9);
    MemoryTiffStream stream(BuildSingleStripLzwTiff(4, 1, w.Finish(), 1, 1));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(4, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(stream.diagnostics().error_count(), 0u);
  }

  // Unpopulated code 300 in LZW_COMPAT -> codep->length == 0 error.
  {
    LsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    w.WriteBits('A', 9);
    w.WriteBits(300, 9);
    w.WriteBits(kLzwCodeEoi, 9);
    MemoryTiffStream stream(BuildSingleStripLzwTiff(4, 1, w.Finish(), 1, 1));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(4, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(stream.diagnostics().error_count(), 0u);
  }

  // Overflow CSIZE in LZW_COMPAT without CODE_CLEAR -> Corrupted LZW table.
  {
    LsbLzwBitWriter w;
    w.WriteBits(kLzwCodeClear, 9);
    int nbits = 9;
    int maxcode = 511;
    int free_ent = kLzwCodeFirst;
    w.WriteBits('A', nbits);
    for (int i = 0; i < 4900; ++i) {
      w.WriteBits(static_cast<uint32_t>(i & 0xff), nbits);
      if (++free_ent > maxcode && nbits < 12) {
        ++nbits;
        maxcode = (1 << nbits) - 1;
      }
    }
    w.WriteBits(kLzwCodeEoi, 12);
    MemoryTiffStream stream(BuildSingleStripLzwTiff(5100, 1, w.Finish(), 1, 1));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(5100, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 5100),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(stream.diagnostics().error_count(), 0u);
  }
}

TEST(TifLzwTest, ReadWritePlusLazySetupTransitions) {
  auto make_two_strip_stream = [](MemoryTiffStream& stream) {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripTags(writer.get(), 8, 4, 8, 1, 2);
    std::vector<uint8_t> s0(16, 0x11);
    std::vector<uint8_t> s1(16, 0x22);
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(), 16), 16);
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, s1.data(), 16), 16);
  };

  // Case A: Read first (sets TIFF_CODERSETUP + dec_codetab), then encode in
  // same TIFF* -> LZWPreEncode lazily calls LZWSetupEncode.
  {
    MemoryTiffStream stream;
    make_two_strip_stream(stream);
    {
      UniqueTiff rw = stream.Open("r+");
      ASSERT_NE(rw, nullptr);
      std::vector<uint8_t> out(16, 0);
      ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, out.data(), 16), 16);
      EXPECT_EQ(out, std::vector<uint8_t>(16, 0x11));

      std::vector<uint8_t> replacement(16, 0x99);
      ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 1, replacement.data(), 16), 16);
    }
    UniqueTiff verify = stream.Open("r");
    ASSERT_NE(verify, nullptr);
    std::vector<uint8_t> out1(16, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(verify.get(), 1, out1.data(), 16), 16);
    EXPECT_EQ(out1, std::vector<uint8_t>(16, 0x99));
  }

  // Case B: Encode first (sets TIFF_CODERSETUP + enc_hashtab), then decode
  // strip 0 in same TIFF* -> LZWPreDecode lazily calls LZWSetupDecode.
  {
    MemoryTiffStream stream;
    make_two_strip_stream(stream);
    UniqueTiff rw = stream.Open("r+");
    ASSERT_NE(rw, nullptr);
    std::vector<uint8_t> replacement(16, 0x77);
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 1, replacement.data(), 16), 16);

    std::vector<uint8_t> out0(16, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, out0.data(), 16), 16);
    EXPECT_EQ(out0, std::vector<uint8_t>(16, 0x11));
  }

  // Case C: Encode first, then inject OOM before decoding -> LZWPreDecode
  // fails cleanly when dec_codetab remains NULL after tif_setupdecode.
  {
    MemoryTiffStream stream;
    make_two_strip_stream(stream);
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    UniqueTiff rw = stream.Open("r+", opts.get());
    ASSERT_NE(rw, nullptr);
    std::vector<uint8_t> replacement(16, 0x88);
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 1, replacement.data(), 16), 16);

    ScopedSingleMemAllocLimit limit(rw.get(), 1024);
    std::vector<uint8_t> out0(16, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(rw.get(), 0, out0.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(recorder.error_count(), 0u);
  }
}

}  // namespace
}  // namespace libtiff
