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

// Clean-room unit tests for libtiff predictor support (tif_predict.c).

#include "third_party/tiff/libtiff/tif_predict.h"

#include <stdio.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

struct CFreeDeleter {
  void operator()(char* ptr) const noexcept { std::free(ptr); }
};

// RAII wrapper over POSIX open_memstream() for testing TIFFPrintDirectory().
class ScopedMemStream {
 public:
  ScopedMemStream() : file_(open_memstream(&buf_, &size_)) {}

  ~ScopedMemStream() { CloseStream(); }

  ScopedMemStream(const ScopedMemStream&) = delete;
  ScopedMemStream& operator=(const ScopedMemStream&) = delete;

  FILE* file() const { return file_; }

  std::string Finish() {
    CloseStream();
    if (owned_buf_ == nullptr) return {};
    return std::string(owned_buf_.get(), size_);
  }

 private:
  void CloseStream() noexcept {
    if (file_ != nullptr) {
      fclose(file_);
      file_ = nullptr;
    }
    if (buf_ != nullptr) {
      owned_buf_.reset(buf_);
      buf_ = nullptr;
    }
  }

  char* buf_ = nullptr;
  size_t size_ = 0;
  FILE* file_ = nullptr;
  std::unique_ptr<char, CFreeDeleter> owned_buf_;
};

struct ImageSpec {
  std::string open_mode = "wl";
  uint16_t compression = COMPRESSION_LZW;
  uint16_t predictor = PREDICTOR_HORIZONTAL;
  uint16_t bits_per_sample = 8;
  uint16_t samples_per_pixel = 1;
  uint16_t planar_config = PLANARCONFIG_CONTIG;
  uint16_t sample_format = SAMPLEFORMAT_UINT;
  uint16_t photometric = PHOTOMETRIC_MINISBLACK;
  uint32_t width = 8;
  uint32_t height = 4;
  uint32_t rows_per_strip = 2;
  uint32_t tile_width = 0;
  uint32_t tile_length = 0;
};

template <typename T>
void AppendNative(std::vector<uint8_t>& dst, T value) {
  std::array<uint8_t, sizeof(T)> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(T));
  dst.insert(dst.end(), bytes.begin(), bytes.end());
}

// Generates deterministic pixel bytes across 8/16/24/32/64-bit formats,
// exercising wrap-around values for integer differencing and non-trivial IEEE
// floats/doubles for floating-point prediction.
std::vector<uint8_t> MakePatternPixels(const ImageSpec& spec) {
  const size_t bytes_per_sample = spec.bits_per_sample / 8;
  const size_t total_samples = static_cast<size_t>(spec.width) *
                               static_cast<size_t>(spec.height) *
                               static_cast<size_t>(spec.samples_per_pixel);
  std::vector<uint8_t> out;
  out.reserve(total_samples * bytes_per_sample);

  for (size_t idx = 0; idx < total_samples; ++idx) {
    if (spec.sample_format == SAMPLEFORMAT_IEEEFP) {
      if (spec.bits_per_sample == 16) {
        // Synthetic IEEE-754 binary16 bit patterns across normal & subnormal
        // ranges.
        const uint16_t half_bits =
            static_cast<uint16_t>(0x3c00U + ((idx * 137U) & 0x0fffU));
        AppendNative(out, half_bits);
      } else if (spec.bits_per_sample == 24) {
        // Synthetic 24-bit floating-point patterns.
        const uint32_t v = static_cast<uint32_t>(0x3f8000U + (idx * 919U));
        out.push_back(static_cast<uint8_t>(v & 0xffU));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xffU));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xffU));
      } else if (spec.bits_per_sample == 32) {
        const float val =
            (idx % 7 == 0)
                ? -0.0f
                : static_cast<float>((static_cast<double>(idx) - 11.5) * 1.375);
        AppendNative(out, val);
      } else {
        const double val =
            (idx % 5 == 0)
                ? -1.0 / 3.0
                : (static_cast<double>(idx) * 3.141592653589793 - 7.25);
        AppendNative(out, val);
      }
    } else {
      if (spec.bits_per_sample == 8) {
        out.push_back(static_cast<uint8_t>((idx * 73U + 19U) & 0xffU));
      } else if (spec.bits_per_sample == 16) {
        const uint16_t val =
            static_cast<uint16_t>((idx * 13107U + 0xfff0U) & 0xffffU);
        AppendNative(out, val);
      } else if (spec.bits_per_sample == 32) {
        const uint32_t val =
            static_cast<uint32_t>(idx * 0x10203041U + 0xfffffff0U);
        AppendNative(out, val);
      } else {
        const uint64_t val =
            static_cast<uint64_t>(idx) * 0x0102030405060709ULL +
            0xffffffffffffff00ULL;
        AppendNative(out, val);
      }
    }
  }
  return out;
}

bool ConfigureCommonTags(TIFF* tif, const ImageSpec& spec) {
  if (TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, spec.width) != 1) return false;
  if (TIFFSetField(tif, TIFFTAG_IMAGELENGTH, spec.height) != 1) return false;
  if (TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, spec.bits_per_sample) != 1) {
    return false;
  }
  if (TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, spec.samples_per_pixel) != 1) {
    return false;
  }
  if (TIFFSetField(tif, TIFFTAG_PLANARCONFIG, spec.planar_config) != 1) {
    return false;
  }
  if (TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, spec.photometric) != 1) {
    return false;
  }
  if (TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, spec.sample_format) != 1) {
    return false;
  }
  if (TIFFSetField(tif, TIFFTAG_COMPRESSION, spec.compression) != 1) {
    return false;
  }
  if (TIFFSetField(tif, TIFFTAG_PREDICTOR, spec.predictor) != 1) {
    return false;
  }
  if (spec.tile_width > 0 && spec.tile_length > 0) {
    if (TIFFSetField(tif, TIFFTAG_TILEWIDTH, spec.tile_width) != 1) {
      return false;
    }
    if (TIFFSetField(tif, TIFFTAG_TILELENGTH, spec.tile_length) != 1) {
      return false;
    }
  } else {
    if (TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, spec.rows_per_strip) != 1) {
      return false;
    }
  }
  return true;
}

void VerifyScanlineRoundTrip(const ImageSpec& spec) {
  const std::vector<uint8_t> expected = MakePatternPixels(spec);
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open(spec.open_mode.c_str());
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(ConfigureCommonTags(writer.get(), spec));

    uint16_t got_pred = 0;
    ASSERT_EQ(TIFFGetField(writer.get(), TIFFTAG_PREDICTOR, &got_pred), 1);
    EXPECT_EQ(got_pred, spec.predictor);

    const tmsize_t row_bytes = TIFFScanlineSize(writer.get());
    ASSERT_GT(row_bytes, 0);
    const uint16_t planes = spec.planar_config == PLANARCONFIG_SEPARATE
                                ? spec.samples_per_pixel
                                : 1;
    ASSERT_EQ(expected.size(),
              static_cast<size_t>(row_bytes) * spec.height * planes);

    for (uint16_t plane = 0; plane < planes; ++plane) {
      for (uint32_t row = 0; row < spec.height; ++row) {
        SCOPED_TRACE(testing::Message()
                     << "write plane=" << plane << " row=" << row);
        const size_t offset =
            (static_cast<size_t>(plane) * spec.height + row) * row_bytes;
        std::vector<uint8_t> row_buf(
            expected.begin() + static_cast<std::ptrdiff_t>(offset),
            expected.begin() + static_cast<std::ptrdiff_t>(offset + row_bytes));
        ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), row, plane),
                  1);
      }
    }
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  const bool expect_swab = spec.open_mode.find('b') != std::string::npos;
  EXPECT_EQ(TIFFIsByteSwapped(reader.get()) != 0, expect_swab);

  uint16_t read_pred = 0;
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_PREDICTOR, &read_pred), 1);
  EXPECT_EQ(read_pred, spec.predictor);

  const tmsize_t row_bytes = TIFFScanlineSize(reader.get());
  ASSERT_GT(row_bytes, 0);
  const uint16_t planes =
      spec.planar_config == PLANARCONFIG_SEPARATE ? spec.samples_per_pixel : 1;
  std::vector<uint8_t> actual(expected.size(), 0);
  for (uint16_t plane = 0; plane < planes; ++plane) {
    for (uint32_t row = 0; row < spec.height; ++row) {
      SCOPED_TRACE(testing::Message()
                   << "read plane=" << plane << " row=" << row);
      const size_t offset =
          (static_cast<size_t>(plane) * spec.height + row) * row_bytes;
      ASSERT_EQ(
          TIFFReadScanline(reader.get(), actual.data() + offset, row, plane),
          1);
    }
  }
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

void VerifyEncodedStripRoundTrip(const ImageSpec& spec) {
  const std::vector<uint8_t> expected = MakePatternPixels(spec);
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open(spec.open_mode.c_str());
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(ConfigureCommonTags(writer.get(), spec));

    const uint32_t num_strips = TIFFNumberOfStrips(writer.get());
    ASSERT_GT(num_strips, 0U);
    const tmsize_t row_bytes = TIFFScanlineSize(writer.get());
    size_t offset = 0;
    for (uint32_t strip = 0; strip < num_strips; ++strip) {
      SCOPED_TRACE(testing::Message() << "write strip=" << strip);
      const uint32_t strip_in_plane =
          strip %
          ((spec.height + spec.rows_per_strip - 1) / spec.rows_per_strip);
      const uint32_t rows_left =
          spec.height - strip_in_plane * spec.rows_per_strip;
      const uint32_t strip_rows = std::min(spec.rows_per_strip, rows_left);
      const tmsize_t strip_bytes =
          static_cast<tmsize_t>(strip_rows) * row_bytes;
      std::vector<uint8_t> work(
          expected.begin() + static_cast<std::ptrdiff_t>(offset),
          expected.begin() + static_cast<std::ptrdiff_t>(offset + strip_bytes));
      ASSERT_EQ(
          TIFFWriteEncodedStrip(writer.get(), strip, work.data(), strip_bytes),
          strip_bytes);
      offset += static_cast<size_t>(strip_bytes);
    }
    ASSERT_EQ(offset, expected.size());
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  const uint32_t num_strips = TIFFNumberOfStrips(reader.get());
  std::vector<uint8_t> actual(expected.size(), 0);
  size_t offset = 0;
  for (uint32_t strip = 0; strip < num_strips; ++strip) {
    SCOPED_TRACE(testing::Message() << "read strip=" << strip);
    const tmsize_t read_bytes = TIFFReadEncodedStrip(
        reader.get(), strip, actual.data() + offset, static_cast<tmsize_t>(-1));
    ASSERT_GT(read_bytes, 0);
    offset += static_cast<size_t>(read_bytes);
  }
  EXPECT_EQ(offset, expected.size());
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

void VerifyTiledRoundTrip(const ImageSpec& spec) {
  ASSERT_GT(spec.tile_width, 0U);
  ASSERT_GT(spec.tile_length, 0U);
  MemoryTiffStream stream;
  std::vector<std::vector<uint8_t>> expected_tiles;
  {
    UniqueTiff writer = stream.Open(spec.open_mode.c_str());
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(ConfigureCommonTags(writer.get(), spec));

    const uint32_t num_tiles = TIFFNumberOfTiles(writer.get());
    const tmsize_t tile_size = TIFFTileSize(writer.get());
    ASSERT_GT(num_tiles, 0U);
    ASSERT_GT(tile_size, 0);

    for (uint32_t tile = 0; tile < num_tiles; ++tile) {
      SCOPED_TRACE(testing::Message() << "write tile=" << tile);
      std::vector<uint8_t> tile_data(static_cast<size_t>(tile_size));
      for (size_t i = 0; i < tile_data.size(); ++i) {
        tile_data[i] = static_cast<uint8_t>(((tile + 1) * 31 + i * 17) & 0xffU);
      }
      expected_tiles.push_back(tile_data);
      std::vector<uint8_t> work = tile_data;
      ASSERT_EQ(
          TIFFWriteEncodedTile(writer.get(), tile, work.data(), tile_size),
          tile_size);
    }
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  const uint32_t num_tiles = TIFFNumberOfTiles(reader.get());
  ASSERT_EQ(num_tiles, expected_tiles.size());
  const tmsize_t tile_size = TIFFTileSize(reader.get());
  for (uint32_t tile = 0; tile < num_tiles; ++tile) {
    SCOPED_TRACE(testing::Message() << "read tile=" << tile);
    std::vector<uint8_t> actual(static_cast<size_t>(tile_size), 0);
    ASSERT_EQ(TIFFReadEncodedTile(reader.get(), tile, actual.data(), tile_size),
              tile_size);
    EXPECT_EQ(actual, expected_tiles[tile]);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

// Extracts the single raw strip 0 from a written single-strip TIFF stream.
std::vector<uint8_t> ReadSingleRawStrip(MemoryTiffStream& stream) {
  UniqueTiff reader = stream.Open("r");
  if (!reader) return {};
  const tmsize_t raw_size = TIFFRawStripSize(reader.get(), 0);
  if (raw_size <= 0) return {};
  std::vector<uint8_t> raw(static_cast<size_t>(raw_size));
  if (TIFFReadRawStrip(reader.get(), 0, raw.data(), raw_size) != raw_size) {
    return {};
  }
  return raw;
}

std::vector<uint8_t> BuildRawStripTiff(
    bool big_endian, uint32_t width, uint32_t height, uint32_t rows_per_strip,
    uint16_t bits_per_sample, uint16_t samples_per_pixel, uint16_t compression,
    uint16_t predictor, uint16_t photometric, uint16_t planar_config,
    uint16_t sample_format, absl::Span<const uint8_t> strip_payload,
    std::optional<std::array<uint16_t, 2>> ycbcr_subsampling = std::nullopt) {
  RawTiffBuilder builder(big_endian, /*big_tiff=*/false);
  const uint64_t strip_off = builder.AppendBlob(strip_payload);
  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bits_per_sample),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, photometric),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(strip_off)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                       static_cast<uint32_t>(strip_payload.size())),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, planar_config),
      builder.MakeShort(TIFFTAG_PREDICTOR, predictor),
      builder.MakeShort(TIFFTAG_SAMPLEFORMAT, sample_format),
  };
  if (ycbcr_subsampling.has_value()) {
    entries.push_back(
        builder.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING, *ycbcr_subsampling));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

TEST(TifPredictTest, NonePredictorPassthrough) {
  for (uint16_t bps : {uint16_t{8}, uint16_t{16}}) {
    SCOPED_TRACE(testing::Message() << "bps=" << bps);
    VerifyScanlineRoundTrip({
        .open_mode = "wl",
        .compression = COMPRESSION_LZW,
        .predictor = PREDICTOR_NONE,
        .bits_per_sample = bps,
        .samples_per_pixel = 3,
        .width = 7,
        .height = 3,
        .rows_per_strip = 2,
    });
  }
}

TEST(TifPredictTest, Horizontal8BitAllStridesAndWidths) {
  // Covers stride 1 (fast 4x unrolled + tail), 2 (REPEAT4 case 2), 3, 4,
  // 5 (REPEAT4 default + 4..1), plus width == 1 (cc == stride).
  for (uint16_t spp :
       {uint16_t{1}, uint16_t{2}, uint16_t{3}, uint16_t{4}, uint16_t{5}}) {
    for (uint32_t width : {uint32_t{1}, uint32_t{7}, uint32_t{13}}) {
      SCOPED_TRACE(testing::Message() << "spp=" << spp << " width=" << width);
      VerifyScanlineRoundTrip({
          .open_mode = "wl",
          .compression = COMPRESSION_LZW,
          .predictor = PREDICTOR_HORIZONTAL,
          .bits_per_sample = 8,
          .samples_per_pixel = spp,
          .width = width,
          .height = 3,
          .rows_per_strip = 2,
      });
    }
  }
  // Planar separate forces stride == 1 even when SamplesPerPixel > 1.
  VerifyScanlineRoundTrip({
      .open_mode = "wl",
      .compression = COMPRESSION_LZW,
      .predictor = PREDICTOR_HORIZONTAL,
      .bits_per_sample = 8,
      .samples_per_pixel = 3,
      .planar_config = PLANARCONFIG_SEPARATE,
      .width = 9,
      .height = 3,
      .rows_per_strip = 2,
  });
}

TEST(TifPredictTest, Horizontal16Bit32Bit64BitAllStrides) {
  for (uint16_t bps : {uint16_t{16}, uint16_t{32}, uint16_t{64}}) {
    for (uint16_t spp :
         {uint16_t{1}, uint16_t{2}, uint16_t{3}, uint16_t{4}, uint16_t{5}}) {
      for (uint32_t width : {uint32_t{1}, uint32_t{6}}) {
        SCOPED_TRACE(testing::Message()
                     << "bps=" << bps << " spp=" << spp << " width=" << width);
        VerifyScanlineRoundTrip({
            .open_mode = "wl",
            .compression = COMPRESSION_LZW,
            .predictor = PREDICTOR_HORIZONTAL,
            .bits_per_sample = bps,
            .samples_per_pixel = spp,
            .width = width,
            .height = 3,
            .rows_per_strip = 2,
        });
      }
    }
    VerifyScanlineRoundTrip({
        .open_mode = "wl",
        .compression = COMPRESSION_LZW,
        .predictor = PREDICTOR_HORIZONTAL,
        .bits_per_sample = bps,
        .samples_per_pixel = 3,
        .planar_config = PLANARCONFIG_SEPARATE,
        .width = 5,
        .height = 2,
        .rows_per_strip = 2,
    });
  }
}

TEST(TifPredictTest, HorizontalBigEndianSwabbing) {
  // Big-endian ("wb") exercises TIFF_SWAB in PredictorSetupEncode/Decode,
  // swabHorDiff16/32/64, swabHorAcc16/32/64, and 8-bit under TIFF_SWAB.
  for (uint16_t bps : {uint16_t{8}, uint16_t{16}, uint16_t{32}, uint16_t{64}}) {
    for (uint16_t spp : {uint16_t{1}, uint16_t{3}, uint16_t{5}}) {
      SCOPED_TRACE(testing::Message() << "bps=" << bps << " spp=" << spp);
      VerifyScanlineRoundTrip({
          .open_mode = "wb",
          .compression = COMPRESSION_LZW,
          .predictor = PREDICTOR_HORIZONTAL,
          .bits_per_sample = bps,
          .samples_per_pixel = spp,
          .width = 6,
          .height = 3,
          .rows_per_strip = 2,
      });
      VerifyEncodedStripRoundTrip({
          .open_mode = "wb",
          .compression = COMPRESSION_LZW,
          .predictor = PREDICTOR_HORIZONTAL,
          .bits_per_sample = bps,
          .samples_per_pixel = spp,
          .width = 6,
          .height = 4,
          .rows_per_strip = 2,
      });
    }
  }
}

TEST(TifPredictTest, HorizontalMultiRowStripsAndTiles) {
  for (std::string_view mode : {"wl", "wb", "w8l", "w8b"}) {
    for (uint16_t bps :
         {uint16_t{8}, uint16_t{16}, uint16_t{32}, uint16_t{64}}) {
      SCOPED_TRACE(testing::Message() << "mode=" << mode << " bps=" << bps);
      VerifyEncodedStripRoundTrip({
          .open_mode = std::string(mode),
          .compression = COMPRESSION_LZW,
          .predictor = PREDICTOR_HORIZONTAL,
          .bits_per_sample = bps,
          .samples_per_pixel = 3,
          .width = 7,
          .height = 5,
          .rows_per_strip = 3,
      });
      VerifyTiledRoundTrip({
          .open_mode = std::string(mode),
          .compression = COMPRESSION_LZW,
          .predictor = PREDICTOR_HORIZONTAL,
          .bits_per_sample = bps,
          .samples_per_pixel = 3,
          .width = 20,
          .height = 20,
          .tile_width = 16,
          .tile_length = 16,
      });
    }
  }
}

TEST(TifPredictTest, HorizontalExactDiffAndAccumulateSemantics) {
  // Write known 8-bit pixels with PREDICTOR_HORIZONTAL, extract the raw LZW
  // strip, and decode it with PREDICTOR_NONE to verify exact horizontal deltas.
  const std::vector<uint8_t> pixels = {10, 13, 10, 2};
  MemoryTiffStream hor_stream;
  {
    UniqueTiff writer = hor_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(
        ConfigureCommonTags(writer.get(), {
                                              .compression = COMPRESSION_LZW,
                                              .predictor = PREDICTOR_HORIZONTAL,
                                              .bits_per_sample = 8,
                                              .samples_per_pixel = 1,
                                              .width = 4,
                                              .height = 1,
                                              .rows_per_strip = 1,
                                          }));
    std::vector<uint8_t> work = pixels;
    ASSERT_EQ(TIFFWriteScanline(writer.get(), work.data(), 0, 0), 1);
    EXPECT_EQ(work, pixels)
        << "PredictorEncodeRow must not mutate caller buffer";
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  const std::vector<uint8_t> raw_hor = ReadSingleRawStrip(hor_stream);
  ASSERT_FALSE(raw_hor.empty());

  MemoryTiffStream inspect_deltas(BuildRawStripTiff(
      /*big_endian=*/false, 4, 1, 1, 8, 1, COMPRESSION_LZW, PREDICTOR_NONE,
      PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG, SAMPLEFORMAT_UINT, raw_hor));
  UniqueTiff delta_reader = inspect_deltas.Open("r");
  ASSERT_NE(delta_reader, nullptr);
  std::vector<uint8_t> got_deltas(4, 0);
  ASSERT_EQ(TIFFReadScanline(delta_reader.get(), got_deltas.data(), 0, 0), 1);
  EXPECT_EQ(got_deltas, (std::vector<uint8_t>{10, 3, 253, 248}));

  // Now encode raw deltas with PREDICTOR_NONE and decode with
  // PREDICTOR_HORIZONTAL to verify horAcc8 accumulation independently.
  MemoryTiffStream raw_delta_stream;
  {
    UniqueTiff writer = raw_delta_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(
        ConfigureCommonTags(writer.get(), {
                                              .compression = COMPRESSION_LZW,
                                              .predictor = PREDICTOR_NONE,
                                              .bits_per_sample = 8,
                                              .samples_per_pixel = 1,
                                              .width = 4,
                                              .height = 1,
                                              .rows_per_strip = 1,
                                          }));
    std::vector<uint8_t> work = {10, 3, 253, 248};
    ASSERT_EQ(TIFFWriteScanline(writer.get(), work.data(), 0, 0), 1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  const std::vector<uint8_t> raw_none = ReadSingleRawStrip(raw_delta_stream);
  ASSERT_FALSE(raw_none.empty());

  MemoryTiffStream acc_stream(BuildRawStripTiff(
      /*big_endian=*/false, 4, 1, 1, 8, 1, COMPRESSION_LZW,
      PREDICTOR_HORIZONTAL, PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG,
      SAMPLEFORMAT_UINT, raw_none));
  UniqueTiff acc_reader = acc_stream.Open("r");
  ASSERT_NE(acc_reader, nullptr);
  std::vector<uint8_t> got_pixels(4, 0);
  ASSERT_EQ(TIFFReadScanline(acc_reader.get(), got_pixels.data(), 0, 0), 1);
  EXPECT_EQ(got_pixels, pixels);
}

TEST(TifPredictTest, FloatingPointAllBitDepthsStridesAndSimdWidths) {
  // 32-bit floats: width=1 (cc=4 <= 8), width=5 (scalar >8), width=23
  // (exercises x86_64 SSE2 16-sample SIMD loop + 7-sample scalar tail),
  // width=32 (exact 2x SIMD multiple).
  for (uint16_t spp :
       {uint16_t{1}, uint16_t{2}, uint16_t{3}, uint16_t{4}, uint16_t{5}}) {
    for (uint32_t width : {uint32_t{1}, uint32_t{5}, uint32_t{23}}) {
      SCOPED_TRACE(testing::Message()
                   << "fp32 spp=" << spp << " width=" << width);
      VerifyScanlineRoundTrip({
          .open_mode = "wl",
          .compression = COMPRESSION_LZW,
          .predictor = PREDICTOR_FLOATINGPOINT,
          .bits_per_sample = 32,
          .samples_per_pixel = spp,
          .sample_format = SAMPLEFORMAT_IEEEFP,
          .width = width,
          .height = 2,
          .rows_per_strip = 2,
      });
    }
  }

  // 16-bit half-float, 24-bit float, and 64-bit double across endiannesses.
  for (std::string_view mode : {"wl", "wb"}) {
    for (uint16_t bps :
         {uint16_t{16}, uint16_t{24}, uint16_t{32}, uint16_t{64}}) {
      for (uint16_t spp : {uint16_t{1}, uint16_t{3}, uint16_t{5}}) {
        SCOPED_TRACE(testing::Message()
                     << "mode=" << mode << " bps=" << bps << " spp=" << spp);
        VerifyScanlineRoundTrip({
            .open_mode = std::string(mode),
            .compression = COMPRESSION_LZW,
            .predictor = PREDICTOR_FLOATINGPOINT,
            .bits_per_sample = bps,
            .samples_per_pixel = spp,
            .sample_format = SAMPLEFORMAT_IEEEFP,
            .width = 7,
            .height = 2,
            .rows_per_strip = 2,
        });
      }
    }
  }

  // Planar separate and multi-row strip / tiled floating point.
  VerifyScanlineRoundTrip({
      .open_mode = "wl",
      .compression = COMPRESSION_LZW,
      .predictor = PREDICTOR_FLOATINGPOINT,
      .bits_per_sample = 32,
      .samples_per_pixel = 3,
      .planar_config = PLANARCONFIG_SEPARATE,
      .sample_format = SAMPLEFORMAT_IEEEFP,
      .width = 19,
      .height = 2,
      .rows_per_strip = 2,
  });
  VerifyEncodedStripRoundTrip({
      .open_mode = "wb",
      .compression = COMPRESSION_LZW,
      .predictor = PREDICTOR_FLOATINGPOINT,
      .bits_per_sample = 32,
      .samples_per_pixel = 1,
      .sample_format = SAMPLEFORMAT_IEEEFP,
      .width = 32,
      .height = 4,
      .rows_per_strip = 2,
  });
  VerifyTiledRoundTrip({
      .open_mode = "wl",
      .compression = COMPRESSION_LZW,
      .predictor = PREDICTOR_FLOATINGPOINT,
      .bits_per_sample = 32,
      .samples_per_pixel = 2,
      .sample_format = SAMPLEFORMAT_IEEEFP,
      .width = 20,
      .height = 20,
      .tile_width = 16,
      .tile_length = 16,
  });
  VerifyTiledRoundTrip({
      .open_mode = "wb",
      .compression = COMPRESSION_LZW,
      .predictor = PREDICTOR_FLOATINGPOINT,
      .bits_per_sample = 64,
      .samples_per_pixel = 2,
      .sample_format = SAMPLEFORMAT_IEEEFP,
      .width = 20,
      .height = 20,
      .tile_width = 16,
      .tile_length = 16,
  });
}

TEST(TifPredictTest, FloatingPointExactBytePlaneReordering) {
  // Verify fpDiff transforms 2 little-endian float32 values into MSB-first
  // byte planes followed by horizontal byte differencing, and
  // PredictorEncodeTile preserves caller buffer.
  const std::vector<float> floats = {1.0f, 2.0f};
  std::vector<uint8_t> pixels(floats.size() * sizeof(float));
  std::memcpy(pixels.data(), floats.data(), pixels.size());

  MemoryTiffStream fp_stream;
  {
    UniqueTiff writer = fp_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(ConfigureCommonTags(writer.get(),
                                    {
                                        .compression = COMPRESSION_LZW,
                                        .predictor = PREDICTOR_FLOATINGPOINT,
                                        .bits_per_sample = 32,
                                        .samples_per_pixel = 1,
                                        .sample_format = SAMPLEFORMAT_IEEEFP,
                                        .width = 2,
                                        .height = 1,
                                        .rows_per_strip = 1,
                                    }));
    std::vector<uint8_t> work = pixels;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, work.data(),
                                    static_cast<tmsize_t>(work.size())),
              static_cast<tmsize_t>(work.size()));
    EXPECT_EQ(work, pixels)
        << "PredictorEncodeTile must not mutate caller buffer";
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  const std::vector<uint8_t> raw_fp = ReadSingleRawStrip(fp_stream);
  ASSERT_FALSE(raw_fp.empty());

  // 1.0f = 0x3f800000 -> bytes BE [3f, 80, 00, 00]
  // 2.0f = 0x40000000 -> bytes BE [40, 00, 00, 00]
  // Reordered planes: [3f, 40, 80, 00, 00, 00, 00, 00]
  // Differenced (stride=1): [3f, 01, 40, 80, 00, 00, 00, 00]
  MemoryTiffStream raw_reader_stream(BuildRawStripTiff(
      /*big_endian=*/false, 2, 1, 1, 32, 1, COMPRESSION_LZW, PREDICTOR_NONE,
      PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG, SAMPLEFORMAT_IEEEFP,
      raw_fp));
  UniqueTiff reader = raw_reader_stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> got_bytes(8, 0);
  ASSERT_EQ(TIFFReadScanline(reader.get(), got_bytes.data(), 0, 0), 1);
  EXPECT_EQ(got_bytes, (std::vector<uint8_t>{0x3f, 0x01, 0x40, 0x80, 0x00, 0x00,
                                             0x00, 0x00}));
}

TEST(TifPredictTest, WorksAcrossAllPredictorEnabledCodecs) {
  const std::array<uint16_t, 3> codecs = {
      COMPRESSION_ADOBE_DEFLATE,
      COMPRESSION_ZSTD,
      COMPRESSION_LZMA,
  };
  for (uint16_t codec : codecs) {
    if (!TIFFIsCODECConfigured(codec)) continue;
    SCOPED_TRACE(testing::Message() << "codec=" << codec);
    VerifyScanlineRoundTrip({
        .open_mode = "wl",
        .compression = codec,
        .predictor = PREDICTOR_HORIZONTAL,
        .bits_per_sample = 16,
        .samples_per_pixel = 3,
        .width = 7,
        .height = 3,
        .rows_per_strip = 3,
    });
    VerifyScanlineRoundTrip({
        .open_mode = "wl",
        .compression = codec,
        .predictor = PREDICTOR_FLOATINGPOINT,
        .bits_per_sample = 32,
        .samples_per_pixel = 2,
        .sample_format = SAMPLEFORMAT_IEEEFP,
        .width = 18,
        .height = 2,
        .rows_per_strip = 2,
    });
  }
}

TEST(TifPredictTest, InvalidPredictorConfigsFailSetupOnEncodeAndDecode) {
  struct InvalidCase {
    uint16_t predictor;
    uint16_t bits_per_sample;
    uint16_t sample_format;
    std::string_view expected_err;
  };
  constexpr std::array<InvalidCase, 4> kCases = {{
      {.predictor = PREDICTOR_HORIZONTAL,
       .bits_per_sample = 4,
       .sample_format = SAMPLEFORMAT_UINT,
       .expected_err = "not supported with 4-bit samples"},
      {.predictor = PREDICTOR_FLOATINGPOINT,
       .bits_per_sample = 32,
       .sample_format = SAMPLEFORMAT_UINT,
       .expected_err = "not supported with 1 data format"},
      {.predictor = PREDICTOR_FLOATINGPOINT,
       .bits_per_sample = 8,
       .sample_format = SAMPLEFORMAT_IEEEFP,
       .expected_err = "not supported with 8-bit samples"},
      {.predictor = 99,
       .bits_per_sample = 8,
       .sample_format = SAMPLEFORMAT_UINT,
       .expected_err = "\"Predictor\" value 99 not supported"},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(testing::Message()
                 << "pred=" << tc.predictor << " bps=" << tc.bits_per_sample
                 << " fmt=" << tc.sample_format);
    // Encode setup failure.
    {
      MemoryTiffStream enc_stream;
      UniqueTiff writer = enc_stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ASSERT_TRUE(ConfigureCommonTags(writer.get(),
                                      {
                                          .compression = COMPRESSION_LZW,
                                          .predictor = tc.predictor,
                                          .bits_per_sample = tc.bits_per_sample,
                                          .samples_per_pixel = 1,
                                          .sample_format = tc.sample_format,
                                          .width = 4,
                                          .height = 1,
                                          .rows_per_strip = 1,
                                      }));
      std::vector<uint8_t> row(
          static_cast<size_t>(TIFFScanlineSize(writer.get())), 0);
      EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), -1);
      ASSERT_FALSE(enc_stream.diagnostics().errors().empty());
      EXPECT_THAT(enc_stream.diagnostics().errors().back().message,
                  HasSubstr(tc.expected_err));
    }

    // Decode setup failure with crafted TIFF IFD.
    {
      // Valid minimal LZW stream bytes (Clear=256, 0x00, EOI=257) = 0x80 0x00
      // 0x40 0x40.
      const std::vector<uint8_t> dummy_lzw = {0x80, 0x00, 0x40, 0x40};
      MemoryTiffStream dec_stream(BuildRawStripTiff(
          /*big_endian=*/false, 4, 1, 1, tc.bits_per_sample, 1, COMPRESSION_LZW,
          tc.predictor, PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG,
          tc.sample_format, dummy_lzw));
      UniqueTiff reader = dec_stream.Open("r");
      ASSERT_NE(reader, nullptr);
      std::vector<uint8_t> row(static_cast<size_t>(std::max<tmsize_t>(
                                   TIFFScanlineSize(reader.get()), 1)),
                               0);
      EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
      bool matched = false;
      for (const auto& err : dec_stream.diagnostics().errors()) {
        if (err.message.find(tc.expected_err) != std::string::npos) {
          matched = true;
          break;
        }
      }
      EXPECT_TRUE(matched) << "Missing error substring: " << tc.expected_err;
    }
  }
}

TEST(TifPredictTest, PredictorSetupFailsWhenRowSizeIsZero) {
  // 1. Strip-based PredictorSetup: unset ImageWidth (0) leaves
  // TIFFScanlineSize() == 0, causing PredictorSetup() to return 0.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                           uint16_t{COMPRESSION_LZW}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PREDICTOR,
                           uint16_t{PREDICTOR_HORIZONTAL}),
              1);
    EXPECT_EQ((*writer->tif_setupencode)(writer.get()), 0);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }

  // 2. Tile-based PredictorSetup: TileWidth=0 leaves TIFFTileRowSize() == 0,
  // exercising the isTiled(tif) rowsize == 0 error return.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                           uint16_t{COMPRESSION_LZW}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, uint32_t{0}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, uint32_t{16}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PREDICTOR,
                           uint16_t{PREDICTOR_HORIZONTAL}),
              1);
    EXPECT_EQ((*writer->tif_setupencode)(writer.get()), 0);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifPredictTest, PartialMisalignedStripAndTileSizesFail) {
  // Multi-row strip (width=8, height=4, rowsperstrip=4 -> rowsize=8, strip=32).
  MemoryTiffStream strip_stream;
  {
    UniqueTiff writer = strip_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(
        ConfigureCommonTags(writer.get(), {
                                              .compression = COMPRESSION_LZW,
                                              .predictor = PREDICTOR_HORIZONTAL,
                                              .bits_per_sample = 8,
                                              .samples_per_pixel = 1,
                                              .width = 8,
                                              .height = 4,
                                              .rows_per_strip = 4,
                                          }));
    std::vector<uint8_t> strip(32, 7);
    // Partial write not divisible by rowsize=8 hits PredictorEncodeTile check.
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(), 10),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(strip_stream.diagnostics().error_count(), 0U);
    strip_stream.diagnostics().Clear();

    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(), 32), 32);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  {
    UniqueTiff reader = strip_stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(32, 0);
    // Partial read not divisible by rowsize=8 hits PredictorDecodeTile check.
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 10),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(strip_stream.diagnostics().error_count(), 0U);
  }

  // Tiled variant (tilewidth=16, tilelength=16 -> rowsize=16, tilesize=256).
  MemoryTiffStream tile_stream;
  {
    UniqueTiff writer = tile_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(
        ConfigureCommonTags(writer.get(), {
                                              .compression = COMPRESSION_LZW,
                                              .predictor = PREDICTOR_HORIZONTAL,
                                              .bits_per_sample = 8,
                                              .samples_per_pixel = 1,
                                              .width = 16,
                                              .height = 16,
                                              .tile_width = 16,
                                              .tile_length = 16,
                                          }));
    std::vector<uint8_t> tile(256, 9);
    EXPECT_EQ(TIFFWriteEncodedTile(writer.get(), 0, tile.data(), 20),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(tile_stream.diagnostics().error_count(), 0U);
    tile_stream.diagnostics().Clear();

    ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 0, tile.data(), 256), 256);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  {
    UniqueTiff reader = tile_stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(256, 0);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, out.data(), 20),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(tile_stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifPredictTest, NonMultipleScanlineSizeTriggersStrideDivisibilityErrors) {
  // With PHOTOMETRIC_YCBCR, SamplesPerPixel=3, PlanarConfig=CONTIG,
  // YCbCrSubsampling={1, 2}, and Width=1, TIFFScanlineSize() returns 2 * bps
  // bytes while stride * bps == 3 * bps. That reaches every (cc % (bps*stride))
  // != 0 guard in horDiff*/horAcc*/swabHorDiff*/swabHorAcc*/fpDiff/fpAcc.
  struct Case {
    std::string_view mode;
    uint16_t predictor;
    uint16_t bps;
    uint16_t sample_format;
  };
  constexpr std::array<Case, 9> kCases = {{
      {.mode = "wl",
       .predictor = PREDICTOR_HORIZONTAL,
       .bps = 8,
       .sample_format = SAMPLEFORMAT_UINT},
      {.mode = "wl",
       .predictor = PREDICTOR_HORIZONTAL,
       .bps = 16,
       .sample_format = SAMPLEFORMAT_UINT},
      {.mode = "wb",
       .predictor = PREDICTOR_HORIZONTAL,
       .bps = 16,
       .sample_format = SAMPLEFORMAT_UINT},
      {.mode = "wl",
       .predictor = PREDICTOR_HORIZONTAL,
       .bps = 32,
       .sample_format = SAMPLEFORMAT_UINT},
      {.mode = "wb",
       .predictor = PREDICTOR_HORIZONTAL,
       .bps = 32,
       .sample_format = SAMPLEFORMAT_UINT},
      {.mode = "wl",
       .predictor = PREDICTOR_HORIZONTAL,
       .bps = 64,
       .sample_format = SAMPLEFORMAT_UINT},
      {.mode = "wb",
       .predictor = PREDICTOR_HORIZONTAL,
       .bps = 64,
       .sample_format = SAMPLEFORMAT_UINT},
      {.mode = "wl",
       .predictor = PREDICTOR_FLOATINGPOINT,
       .bps = 32,
       .sample_format = SAMPLEFORMAT_IEEEFP},
      {.mode = "wb",
       .predictor = PREDICTOR_FLOATINGPOINT,
       .bps = 32,
       .sample_format = SAMPLEFORMAT_IEEEFP},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(testing::Message() << "mode=" << tc.mode << " pred="
                                    << tc.predictor << " bps=" << tc.bps);
    // 1. Encode path via TIFFWriteScanline -> PredictorEncodeRow -> *Diff*.
    {
      MemoryTiffStream enc_stream;
      UniqueTiff writer = enc_stream.Open(std::string(tc.mode).c_str());
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{1}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, tc.bps), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_YCBCR}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING,
                             uint16_t{1}, uint16_t{2}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, tc.sample_format),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_LZW}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PREDICTOR, tc.predictor), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}),
                1);

      const tmsize_t row_bytes = TIFFScanlineSize(writer.get());
      ASSERT_EQ(row_bytes, static_cast<tmsize_t>(2 * (tc.bps / 8)));
      std::vector<uint8_t> row(static_cast<size_t>(row_bytes), 1);
      // TIFFWriteScanline returns the encoderow status (0 on failure).
      EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 0);
      EXPECT_GT(enc_stream.diagnostics().error_count(), 0U);
    }

    // 2. Decode path: encode valid strip with PREDICTOR_NONE, re-tag as
    // tc.predictor, and decode both via scanline and via strip.
    MemoryTiffStream valid_stream;
    {
      UniqueTiff writer = valid_stream.Open(std::string(tc.mode).c_str());
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{1}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{2}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, tc.bps), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             uint16_t{PLANARCONFIG_CONTIG}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             uint16_t{PHOTOMETRIC_YCBCR}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING,
                             uint16_t{1}, uint16_t{2}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT, tc.sample_format),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                             uint16_t{COMPRESSION_LZW}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PREDICTOR,
                             uint16_t{PREDICTOR_NONE}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{2}),
                1);
      const tmsize_t strip_bytes = TIFFStripSize(writer.get());
      ASSERT_EQ(strip_bytes, static_cast<tmsize_t>(4 * (tc.bps / 8)));
      std::vector<uint8_t> strip(static_cast<size_t>(strip_bytes), 0x22);
      ASSERT_EQ(
          TIFFWriteEncodedStrip(writer.get(), 0, strip.data(), strip_bytes),
          strip_bytes);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }
    const std::vector<uint8_t> raw_strip = ReadSingleRawStrip(valid_stream);
    ASSERT_FALSE(raw_strip.empty());

    const bool big_endian = (tc.mode == "wb");
    const std::vector<uint8_t> crafted = BuildRawStripTiff(
        big_endian, 1, 2, 2, tc.bps, 3, COMPRESSION_LZW, tc.predictor,
        PHOTOMETRIC_YCBCR, PLANARCONFIG_CONTIG, tc.sample_format, raw_strip,
        std::array<uint16_t, 2>{1, 2});

    {
      MemoryTiffStream scan_dec(crafted);
      UniqueTiff reader = scan_dec.Open("r");
      ASSERT_NE(reader, nullptr);
      std::vector<uint8_t> row(
          static_cast<size_t>(TIFFScanlineSize(reader.get())), 0);
      EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
      EXPECT_GT(scan_dec.diagnostics().error_count(), 0U);
    }
    {
      MemoryTiffStream strip_dec(crafted);
      UniqueTiff reader = strip_dec.Open("r");
      ASSERT_NE(reader, nullptr);
      std::vector<uint8_t> strip(
          static_cast<size_t>(TIFFStripSize(reader.get())), 0);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(),
                                     static_cast<tmsize_t>(strip.size())),
                static_cast<tmsize_t>(-1));
      EXPECT_GT(strip_dec.diagnostics().error_count(), 0U);
    }
  }
}

TEST(TifPredictTest, UnderlyingCodecDecodeAndEncodeFailuresPropagate) {
  // 1. Corrupted LZW strip fails in sp->decoderow / sp->decodetile before
  // predictor accumulation runs.
  const std::vector<uint8_t> bad_lzw = {0xff, 0xff, 0xff, 0xff};
  {
    MemoryTiffStream stream(BuildRawStripTiff(
        /*big_endian=*/false, 4, 2, 2, 8, 1, COMPRESSION_LZW,
        PREDICTOR_HORIZONTAL, PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG,
        SAMPLEFORMAT_UINT, bad_lzw));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 8> buf{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), buf.data(), 0, 0), -1);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }
  {
    MemoryTiffStream stream(BuildRawStripTiff(
        /*big_endian=*/false, 4, 2, 2, 8, 1, COMPRESSION_LZW,
        PREDICTOR_HORIZONTAL, PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG,
        SAMPLEFORMAT_UINT, bad_lzw));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 8> buf{};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, buf.data(), 8),
              static_cast<tmsize_t>(-1));
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }

  // 2. Old-style LZW compat header (0x00, 0x01) triggers LZWPreDecode to swap
  // tif_decoderow and invoke PredictorSetupDecode a second time.
  {
    const std::vector<uint8_t> compat_lzw = {0x00, 0x01, 0xff, 0xff};
    MemoryTiffStream stream(BuildRawStripTiff(
        /*big_endian=*/false, 4, 1, 1, 8, 1, COMPRESSION_LZW,
        PREDICTOR_HORIZONTAL, PHOTOMETRIC_MINISBLACK, PLANARCONFIG_CONTIG,
        SAMPLEFORMAT_UINT, compat_lzw));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::array<uint8_t, 4> buf{};
    EXPECT_EQ(TIFFReadScanline(reader.get(), buf.data(), 0, 0), -1);
    EXPECT_GT(stream.diagnostics().warning_count(), 0U);
  }

  // 3. Re-invoking tif_setupdecode on LZW/ZIP after first setup hits the
  // "already hooked" branch safely without leaking.
  for (uint16_t pred :
       {uint16_t{PREDICTOR_HORIZONTAL}, uint16_t{PREDICTOR_FLOATINGPOINT}}) {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ASSERT_TRUE(ConfigureCommonTags(
          writer.get(), {
                            .compression = COMPRESSION_ADOBE_DEFLATE,
                            .predictor = pred,
                            .bits_per_sample = 32,
                            .samples_per_pixel = 1,
                            .sample_format = SAMPLEFORMAT_IEEEFP,
                            .width = 4,
                            .height = 1,
                            .rows_per_strip = 1,
                        }));
      std::vector<uint8_t> row(16, 0);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 1);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> row(16, 0);
    ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    EXPECT_EQ((*reader->tif_setupdecode)(reader.get()), 1);
  }

  // 4. Write failure during scanline encode (PredictorEncodeRow returns
  // (*sp->encoderow)(...) failure == 0 when underlying LZW flushes to failing
  // I/O).
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(
        ConfigureCommonTags(writer.get(), {
                                              .compression = COMPRESSION_LZW,
                                              .predictor = PREDICTOR_HORIZONTAL,
                                              .bits_per_sample = 8,
                                              .samples_per_pixel = 1,
                                              .width = 512,
                                              .height = 2,
                                              .rows_per_strip = 2,
                                          }));
    // Use a small raw output buffer so LZWEncode flushes mid-scanline.
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 128), 1);
    std::vector<uint8_t> row(512);
    uint32_t rng = 0x12345678U;
    for (size_t i = 0; i < row.size(); ++i) {
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      row[i] = static_cast<uint8_t>(rng & 0xffU);
    }
    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 0);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
    stream.ResetFaults();
  }
}

TEST(TifPredictTest, OutOfMemoryLimitsInSetupAndRowTileBuffers) {
  // 1. PredictorSetupDecode fails when sp->setupdecode hits OOM (LZW
  // dec_codetab).
  MemoryTiffStream fp_stream;
  {
    UniqueTiff writer = fp_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(ConfigureCommonTags(writer.get(),
                                    {
                                        .compression = COMPRESSION_LZW,
                                        .predictor = PREDICTOR_FLOATINGPOINT,
                                        .bits_per_sample = 32,
                                        .samples_per_pixel = 1,
                                        .sample_format = SAMPLEFORMAT_IEEEFP,
                                        .width = 64,
                                        .height = 2,
                                        .rows_per_strip = 1,
                                    }));
    std::vector<uint8_t> row0(256, 0x3f);
    std::vector<uint8_t> row1(256, 0x40);
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row0.data(), 0, 0), 1);
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row1.data(), 1, 0), 1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  {
    UniqueTiff reader = fp_stream.Open("r");
    ASSERT_NE(reader, nullptr);
    // TIFFReadBufferSetup allocates >= 1024 bytes; LZWSetupDecode allocates
    // CSIZE * sizeof(code_t) (> 100 KB), so 4096 allows buffer setup and fails
    // setupdecode.
    ScopedSingleMemAllocLimit limit(reader.get(), 4096);
    std::vector<uint8_t> row(256, 0);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_GT(fp_stream.diagnostics().error_count(), 0U);
  }

  // 2. fpAcc OOM during PredictorDecodeRow and PredictorDecodeTile.
  {
    UniqueTiff reader = fp_stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> row(256, 0);
    ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), 1);
    {
      ScopedSingleMemAllocLimit limit(reader.get(), 128);
      EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 1, 0), -1);
    }
    EXPECT_GT(fp_stream.diagnostics().error_count(), 0U);
  }
  {
    UniqueTiff reader = fp_stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> strip(256, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(), 256), 256);
    {
      ScopedSingleMemAllocLimit limit(reader.get(), 128);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 1, strip.data(), 256),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_GT(fp_stream.diagnostics().error_count(), 0U);
  }

  // 3. PredictorSetupEncode fails when sp->setupencode hits OOM (LZW
  // enc_hashtab = ~144 KB > 16 KB, while TIFFWriteBufferSetup = 8 KB succeeds).
  {
    MemoryTiffStream enc_stream;
    UniqueTiff writer = enc_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(
        ConfigureCommonTags(writer.get(), {
                                              .compression = COMPRESSION_LZW,
                                              .predictor = PREDICTOR_HORIZONTAL,
                                              .bits_per_sample = 8,
                                              .samples_per_pixel = 1,
                                              .width = 64,
                                              .height = 1,
                                              .rows_per_strip = 1,
                                          }));
    ScopedSingleMemAllocLimit limit(writer.get(), 16384);
    std::vector<uint8_t> row(64, 1);
    EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), -1);
    EXPECT_GT(enc_stream.diagnostics().error_count(), 0U);
  }

  // 4. PredictorEncodeRow working_copy OOM and fpDiff tmp OOM.
  {
    MemoryTiffStream enc_stream;
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    UniqueTiff writer = enc_stream.Open("wl", opts.get());
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(ConfigureCommonTags(writer.get(),
                                    {
                                        .compression = COMPRESSION_LZW,
                                        .predictor = PREDICTOR_FLOATINGPOINT,
                                        .bits_per_sample = 32,
                                        .samples_per_pixel = 1,
                                        .sample_format = SAMPLEFORMAT_IEEEFP,
                                        .width = 64,
                                        .height = 3,
                                        .rows_per_strip = 1,
                                    }));
    std::vector<uint8_t> row(256, 0x3f);
    ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 1);

    // Tight single limit fails working_copy allocation in PredictorEncodeRow.
    {
      ScopedSingleMemAllocLimit single_limit(writer.get(), 128);
      EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 1, 0), 0);
    }
    EXPECT_GT(recorder.error_count(), 0U);
    recorder.Clear();

    // Budget of 256 allows working_copy (256 bytes) but fails fpDiff's tmp (256
    // bytes), exercising PredictorEncodeRow cleanup on encodepfunc failure.
    {
      ScopedCumulatedMemAllocRemaining cum_limit(writer.get(), 256);
      EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 2, 0), 0);
    }
    EXPECT_GT(recorder.error_count(), 0U);
  }

  // 5. PredictorEncodeTile working_copy OOM.
  {
    MemoryTiffStream enc_stream;
    UniqueTiff writer = enc_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(
        ConfigureCommonTags(writer.get(), {
                                              .compression = COMPRESSION_LZW,
                                              .predictor = PREDICTOR_HORIZONTAL,
                                              .bits_per_sample = 8,
                                              .samples_per_pixel = 1,
                                              .width = 256,
                                              .height = 2,
                                              .rows_per_strip = 1,
                                          }));
    std::vector<uint8_t> strip(256, 5);
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, strip.data(), 256), 256);
    {
      ScopedSingleMemAllocLimit limit(writer.get(), 128);
      EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, strip.data(), 256),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_GT(enc_stream.diagnostics().error_count(), 0U);
  }
}

void CustomParentPrintDir(TIFF* /*tif*/, FILE* fd,
                          long /*flags*/) {  // NOLINT(runtime/int)
  fprintf(fd, "  CustomParentPrintDir: ok\n");
}

void ExtendWithParentPrintDir(TIFF* tif) {
  tif->tif_tagmethods.printdir = &CustomParentPrintDir;
}

TEST(TifPredictTest, PrintDirAndTagMethodsAndCleanup) {
  // 1. PredictorPrintDir with FIELD_PREDICTOR unset vs 1, 2, 3, and unknown.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, uint16_t{COMPRESSION_LZW}),
        1);

    // Default value before explicit TIFFSetField is reported by
    // TIFFGetFieldDefaulted (TIFFGetField returns 0 while FIELD_PREDICTOR is
    // unset).
    uint16_t default_pred = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PREDICTOR, &default_pred), 0);
    EXPECT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PREDICTOR, &default_pred), 1);
    EXPECT_EQ(default_pred, 1);

    // FIELD_PREDICTOR not set -> no Predictor line printed.
    {
      ScopedMemStream ms;
      ASSERT_NE(ms.file(), nullptr);
      TIFFPrintDirectory(tif.get(), ms.file(), 0);
      EXPECT_THAT(ms.Finish(), Not(HasSubstr("Predictor:")));
    }

    struct PrintCase {
      uint16_t value;
      std::string_view expected_substr;
    };
    constexpr std::array<PrintCase, 4> kPrintCases = {{
        {.value = PREDICTOR_NONE, .expected_substr = "Predictor: none 1 (0x1)"},
        {.value = PREDICTOR_HORIZONTAL,
         .expected_substr = "Predictor: horizontal differencing 2 (0x2)"},
        {.value = PREDICTOR_FLOATINGPOINT,
         .expected_substr = "Predictor: floating point predictor 3 (0x3)"},
        {.value = 42, .expected_substr = "Predictor: 42 (0x2a)"},
    }};
    for (const auto& pc : kPrintCases) {
      ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PREDICTOR, pc.value), 1);
      uint16_t got_pred = 0;
      ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_PREDICTOR, &got_pred), 1);
      EXPECT_EQ(got_pred, pc.value);

      ScopedMemStream ms;
      ASSERT_NE(ms.file(), nullptr);
      TIFFPrintDirectory(tif.get(), ms.file(), 0);
      EXPECT_THAT(ms.Finish(), HasSubstr(pc.expected_substr));
    }
  }

  // 2. Chaining to sp->printdir via tag extender installed prior to codec init.
  {
    ScopedTagExtender extender(&ExtendWithParentPrintDir);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, uint16_t{COMPRESSION_LZW}),
        1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PREDICTOR,
                           uint16_t{PREDICTOR_HORIZONTAL}),
              1);
    ScopedMemStream ms;
    ASSERT_NE(ms.file(), nullptr);
    TIFFPrintDirectory(tif.get(), ms.file(), 0);
    const std::string printed = ms.Finish();
    EXPECT_THAT(printed,
                HasSubstr("Predictor: horizontal differencing 2 (0x2)"));
    EXPECT_THAT(printed, HasSubstr("CustomParentPrintDir: ok"));
  }

  // 3. Switching compression invokes TIFFPredictorCleanup and restores parent
  // methods.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, uint16_t{COMPRESSION_LZW}),
        1);
    uint16_t pred = 0;
    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PREDICTOR, &pred), 1);
    EXPECT_EQ(pred, PREDICTOR_NONE);
    // Switch back to COMPRESSION_NONE -> LZWCleanup -> TIFFPredictorCleanup.
    // Leaving FIELD_PREDICTOR unset avoids an orphaned codec field bit when
    // flushing the directory on close after PredictorVGetField is unhooked.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION,
                           uint16_t{COMPRESSION_NONE}),
              1);
    uint16_t comp = 0;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_COMPRESSION, &comp), 1);
    EXPECT_EQ(comp, COMPRESSION_NONE);

    // Without predictor codec active, TIFFGetFieldDefaulted reports missing
    // plugin (tif_aux.c) cleanly without crashing.
    pred = 0;
    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PREDICTOR, &pred), 0);
    EXPECT_GT(stream.diagnostics().error_count(), 0U);
  }
}

}  // namespace
}  // namespace libtiff
