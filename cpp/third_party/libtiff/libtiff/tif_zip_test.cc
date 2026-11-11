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

// Unit tests for libtiff ZIP/Deflate codec (libtiff/tif_zip.c).
//
// google3 caveats:
//   - LIBDEFLATE_SUPPORT is undefined in google3; ZIP_SUPPORT is backed by
//     third_party/rust/zlib_rs.
//   - Compression levels 1..9 are non-deterministic when zlib_rs debug
//     randomization is active, so tests assert lossless round-trips and valid
//     error statuses rather than exact compressed byte counts.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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

namespace libtiff {
namespace {

using ::testing::Each;

// Deterministic high-entropy pattern to exercise buffer refills across small
// raw output buffers and multiple deflate flush iterations.
std::vector<uint8_t> MakePatternBytes(size_t size,
                                      uint32_t seed = 0x12345678U) {
  std::vector<uint8_t> out(size);
  uint32_t state = seed;
  for (size_t i = 0; i < size; ++i) {
    state = state * 1664525U + 1013904223U;
    out[i] = static_cast<uint8_t>((state >> 24) ^ (i & 0xffU));
  }
  return out;
}

void ConfigureBasicStripTags(TIFF* tif, uint32_t width, uint32_t length,
                             uint16_t bits_per_sample,
                             uint16_t samples_per_pixel, uint16_t planar_config,
                             uint16_t compression, uint32_t rows_per_strip) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, length), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         samples_per_pixel >= 3 ? PHOTOMETRIC_RGB
                                                : PHOTOMETRIC_MINISBLACK),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
}

// Writes a small 8-bit single-strip Deflate TIFF in memory and extracts its raw
// compressed strip payload for crafting synthetic test cases.
std::vector<uint8_t> MakeValidDeflateBlob(absl::Span<const uint8_t> raw_pixels,
                                          uint32_t width, uint32_t length) {
  MemoryTiffStream stream;
  {
    UniqueTiff tif = stream.Open("wl");
    EXPECT_NE(tif, nullptr);
    if (tif == nullptr) return {};
    ConfigureBasicStripTags(tif.get(), width, length, /*bits_per_sample=*/8,
                            /*samples_per_pixel=*/1, PLANARCONFIG_CONTIG,
                            COMPRESSION_DEFLATE, length);
    std::vector<uint8_t> mutable_pixels(raw_pixels.begin(), raw_pixels.end());
    EXPECT_EQ(
        TIFFWriteEncodedStrip(tif.get(), 0, mutable_pixels.data(),
                              static_cast<tmsize_t>(mutable_pixels.size())),
        static_cast<tmsize_t>(mutable_pixels.size()));
    EXPECT_EQ(TIFFWriteDirectory(tif.get()), 1);
  }
  UniqueTiff reader = stream.Open("r");
  EXPECT_NE(reader, nullptr);
  if (reader == nullptr) return {};
  const tmsize_t raw_size =
      static_cast<tmsize_t>(TIFFGetStrileByteCount(reader.get(), 0));
  EXPECT_GT(raw_size, 0);
  if (raw_size <= 0) return {};
  std::vector<uint8_t> compressed(static_cast<size_t>(raw_size));
  EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, compressed.data(), raw_size),
            raw_size);
  return compressed;
}

std::vector<uint8_t> BuildSingleStripRawTiff(
    absl::Span<const uint8_t> compressed_blob, uint32_t width, uint32_t length,
    uint16_t compression, uint16_t predictor = PREDICTOR_NONE,
    uint16_t sample_format = SAMPLEFORMAT_UINT) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const uint64_t blob_off = builder.AppendBlob(compressed_blob);
  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, length),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, compression),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(blob_off)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, length),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                       static_cast<uint32_t>(compressed_blob.size())),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeShort(TIFFTAG_SAMPLEFORMAT, sample_format),
  };
  if (predictor != PREDICTOR_NONE) {
    entries.push_back(builder.MakeShort(TIFFTAG_PREDICTOR, predictor));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

bool HasDiagnosticSubstr(const DiagnosticRecorder& recorder,
                         std::string_view needle) {
  for (const DiagnosticRecord& rec : recorder.errors()) {
    if (rec.message.find(needle) != std::string::npos) return true;
  }
  return false;
}

TEST(TifZipTest, CodecRegistrationAndCleanupStateZero) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_DEFLATE), 1);
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_ADOBE_DEFLATE), 1);

  const TIFFCodec* deflate_codec = TIFFFindCODEC(COMPRESSION_DEFLATE);
  ASSERT_NE(deflate_codec, nullptr);
  EXPECT_EQ(deflate_codec->scheme, COMPRESSION_DEFLATE);

  const TIFFCodec* adobe_codec = TIFFFindCODEC(COMPRESSION_ADOBE_DEFLATE);
  ASSERT_NE(adobe_codec, nullptr);
  EXPECT_EQ(adobe_codec->scheme, COMPRESSION_ADOBE_DEFLATE);

  MemoryTiffStream stream;
  {
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE),
              1);
    EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 1032ULL);

    // Switch back to COMPRESSION_NONE without ever encoding or decoding to
    // cover ZIPCleanup when sp->state == 0.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 1ULL);

    // Also test COMPRESSION_ADOBE_DEFLATE max compression ratio before closing
    // without setup.
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE),
        1);
    EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 1032ULL);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifZipTest, RoundTripDeflateSchemesAndFormats) {
  struct FormatCase {
    std::string_view open_mode;
    uint16_t compression;
    uint16_t bits_per_sample;
    uint16_t samples_per_pixel;
    uint16_t planar_config;
    uint32_t width;
    uint32_t length;
    uint32_t rows_per_strip;
  };

  constexpr auto kCases = std::to_array<FormatCase>({
      {"wl", COMPRESSION_DEFLATE, 8, 1, PLANARCONFIG_CONTIG, 33, 17, 5},
      {"wb", COMPRESSION_ADOBE_DEFLATE, 8, 3, PLANARCONFIG_CONTIG, 29, 15, 4},
      {"w8l", COMPRESSION_DEFLATE, 16, 1, PLANARCONFIG_CONTIG, 24, 10, 3},
      {"w8b", COMPRESSION_ADOBE_DEFLATE, 16, 3, PLANARCONFIG_SEPARATE, 20, 8,
       4},
      {"wb", COMPRESSION_DEFLATE, 32, 1, PLANARCONFIG_CONTIG, 16, 6, 3},
  });

  for (size_t idx = 0; idx < kCases.size(); ++idx) {
    const FormatCase& tc = kCases[idx];
    SCOPED_TRACE(idx);

    const size_t bytes_per_sample = tc.bits_per_sample / 8;
    const size_t row_samples =
        tc.planar_config == PLANARCONFIG_CONTIG
            ? static_cast<size_t>(tc.width) * tc.samples_per_pixel
            : static_cast<size_t>(tc.width);
    const size_t plane_count =
        tc.planar_config == PLANARCONFIG_SEPARATE ? tc.samples_per_pixel : 1;
    const size_t row_bytes = row_samples * bytes_per_sample;
    const size_t total_bytes = row_bytes * tc.length * plane_count;

    const std::vector<uint8_t> original =
        MakePatternBytes(total_bytes, static_cast<uint32_t>(0x9000U + idx));

    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open(std::string(tc.open_mode).c_str());
      ASSERT_NE(writer, nullptr);
      ConfigureBasicStripTags(writer.get(), tc.width, tc.length,
                              tc.bits_per_sample, tc.samples_per_pixel,
                              tc.planar_config, tc.compression,
                              tc.rows_per_strip);

      // Copy per-row because TIFFWriteScanline swaps multi-byte words in place.
      std::vector<uint8_t> row_buf(row_bytes);
      for (uint16_t s = 0; s < plane_count; ++s) {
        for (uint32_t row = 0; row < tc.length; ++row) {
          const size_t off =
              (static_cast<size_t>(s) * tc.length + row) * row_bytes;
          std::memcpy(row_buf.data(), original.data() + off, row_bytes);
          ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), row, s), 1);
        }
      }
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsBigTIFF(reader.get()),
              tc.open_mode.find('8') != std::string_view::npos ? 1 : 0);
    EXPECT_EQ(TIFFIsBigEndian(reader.get()),
              tc.open_mode.find('b') != std::string_view::npos ? 1 : 0);

    uint16_t got_compression = 0;
    ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_COMPRESSION, &got_compression),
              1);
    EXPECT_EQ(got_compression, tc.compression);

    std::vector<uint8_t> decoded(total_bytes, 0);
    for (uint16_t s = 0; s < plane_count; ++s) {
      for (uint32_t row = 0; row < tc.length; ++row) {
        const size_t off =
            (static_cast<size_t>(s) * tc.length + row) * row_bytes;
        ASSERT_EQ(TIFFReadScanline(reader.get(), decoded.data() + off, row, s),
                  1);
      }
    }
    EXPECT_EQ(decoded, original);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifZipTest, ZipQualityAndSubcodecTags) {
  MemoryTiffStream stream;
  {
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureBasicStripTags(tif.get(), 16, 8, 8, 1, PLANARCONFIG_CONTIG,
                            COMPRESSION_DEFLATE, 8);

    // Defaults and delegation to parent get/set methods.
    int zip_quality = 999;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_ZIPQUALITY, &zip_quality), 1);
    EXPECT_EQ(zip_quality, -1);

    int subcodec = 999;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_DEFLATE_SUBCODEC, &subcodec), 1);
    EXPECT_EQ(subcodec, DEFLATE_SUBCODEC_ZLIB);

    uint32_t width = 0;
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
    EXPECT_EQ(width, 16U);

    // Valid subcodec setting; invalid & unsupported values must fail cleanly.
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_DEFLATE_SUBCODEC,
                           DEFLATE_SUBCODEC_ZLIB),
              1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_DEFLATE_SUBCODEC,
                           DEFLATE_SUBCODEC_LIBDEFLATE),
              0);
    EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "unsupported"));

    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_DEFLATE_SUBCODEC, 42), 0);
    EXPECT_TRUE(
        HasDiagnosticSubstr(stream.diagnostics(), "Invalid DeflateCodec"));

    // Out-of-range ZIPQUALITY checks.
    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, -2), 0);
    EXPECT_TRUE(
        HasDiagnosticSubstr(stream.diagnostics(), "Invalid ZipQuality"));

    stream.diagnostics().Clear();
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, 13), 0);
    EXPECT_TRUE(
        HasDiagnosticSubstr(stream.diagnostics(), "Invalid ZipQuality"));

    // Restore valid level > Z_BEST_COMPRESSION (10..12 capped in
    // ZIPSetupEncode).
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, 12), 1);
    ASSERT_EQ(TIFFGetField(tif.get(), TIFFTAG_ZIPQUALITY, &zip_quality), 1);
    EXPECT_EQ(zip_quality, 12);

    std::vector<uint8_t> pixels = MakePatternBytes(16U * 8U, 0xA5U);
    ASSERT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, pixels.data(),
                                    static_cast<tmsize_t>(pixels.size())),
              static_cast<tmsize_t>(pixels.size()));
    ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
  }

  // Test multiple valid quality levels including 0, 1, 9, 10, 12.
  for (int level : {-1, 0, 1, 6, 9, 11, 12}) {
    SCOPED_TRACE(level);
    const std::vector<uint8_t> expected = MakePatternBytes(128, 0x33U + level);
    MemoryTiffStream qstream;
    {
      UniqueTiff w = qstream.Open("wl");
      ASSERT_NE(w, nullptr);
      ConfigureBasicStripTags(w.get(), 32, 4, 8, 1, PLANARCONFIG_CONTIG,
                              COMPRESSION_DEFLATE, 4);
      ASSERT_EQ(TIFFSetField(w.get(), TIFFTAG_ZIPQUALITY, level), 1);
      std::vector<uint8_t> buf = expected;
      ASSERT_EQ(TIFFWriteEncodedStrip(w.get(), 0, buf.data(),
                                      static_cast<tmsize_t>(buf.size())),
                static_cast<tmsize_t>(buf.size()));
      ASSERT_EQ(TIFFWriteDirectory(w.get()), 1);
    }
    EXPECT_EQ(qstream.diagnostics().error_count(), 0U);
    UniqueTiff r = qstream.Open("r");
    ASSERT_NE(r, nullptr);
    std::vector<uint8_t> actual(expected.size(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(r.get(), 0, actual.data(),
                                   static_cast<tmsize_t>(actual.size())),
              static_cast<tmsize_t>(actual.size()));
    EXPECT_EQ(actual, expected);
  }
}

TEST(TifZipTest, DynamicZipQualityChangeMidEncode) {
  const std::vector<uint8_t> expected = MakePatternBytes(64U * 4U, 0x5555U);
  MemoryTiffStream stream;
  {
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureBasicStripTags(tif.get(), 64, 4, 8, 1, PLANARCONFIG_CONTIG,
                            COMPRESSION_DEFLATE, 4);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, 1), 1);

    std::vector<uint8_t> row(64);
    std::memcpy(row.data(), expected.data(), 64);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0), 1);

    // Change quality while ZSTATE_INIT_ENCODE is active, including level > 9.
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, 6), 1);
    std::memcpy(row.data(), expected.data() + 64, 64);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 1), 1);

    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, 11), 1);
    std::memcpy(row.data(), expected.data() + 128, 64);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 2), 1);

    std::memcpy(row.data(), expected.data() + 192, 64);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 3), 1);
    ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> actual(expected.size(), 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, actual.data(),
                                 static_cast<tmsize_t>(actual.size())),
            static_cast<tmsize_t>(actual.size()));
  EXPECT_EQ(actual, expected);
}

TEST(TifZipTest, DynamicZipQualityDeflateParamsErrorOnTinyBuffer) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("wl");
  ASSERT_NE(tif, nullptr);
  // At ZIPQUALITY=0, writing 65536 bytes (> pending.capacity() - header,
  // fitting inside 2*w_size=65536) through a 3-byte raw output buffer leaves
  // ~65KB in pending alongside un-emitted bytes in the window, so
  // deflateParams(Block) fills avail_out=3 and returns Z_BUF_ERROR.
  constexpr uint32_t kWideRow = 65536;
  ConfigureBasicStripTags(tif.get(), kWideRow, 2, 8, 1, PLANARCONFIG_CONTIG,
                          COMPRESSION_DEFLATE, 2);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, 0), 1);
  ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 3), 1);

  std::vector<uint8_t> row = MakePatternBytes(kWideRow, 0xCAFEU);
  ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0), 1);

  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, 9), 0);
  EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "ZLib error"));
}

TEST(TifZipTest, PredictorsAcrossBitDepthsAndEndianness) {
  // Horizontal differencing (PREDICTOR_HORIZONTAL) across 8, 16, 32-bit.
  for (uint16_t bps : {uint16_t{8}, uint16_t{16}, uint16_t{32}}) {
    for (std::string_view mode : {"wl", "wb"}) {
      SCOPED_TRACE(bps);
      SCOPED_TRACE(mode);
      const uint32_t width = 20;
      const uint32_t length = 6;
      const uint16_t spp = 3;
      const size_t row_bytes = static_cast<size_t>(width) * spp * (bps / 8);
      const std::vector<uint8_t> expected =
          MakePatternBytes(row_bytes * length, 0x77U + bps);

      MemoryTiffStream stream;
      {
        UniqueTiff writer = stream.Open(std::string(mode).c_str());
        ASSERT_NE(writer, nullptr);
        ConfigureBasicStripTags(writer.get(), width, length, bps, spp,
                                PLANARCONFIG_CONTIG, COMPRESSION_ADOBE_DEFLATE,
                                3);
        ASSERT_EQ(
            TIFFSetField(writer.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL),
            1);

        std::vector<uint8_t> row_buf(row_bytes);
        for (uint32_t r = 0; r < length; ++r) {
          std::memcpy(row_buf.data(), expected.data() + r * row_bytes,
                      row_bytes);
          ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), r, 0), 1);
        }
        ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
      }
      EXPECT_EQ(stream.diagnostics().error_count(), 0U);

      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      std::vector<uint8_t> actual(expected.size(), 0);
      for (uint32_t r = 0; r < length; ++r) {
        ASSERT_EQ(
            TIFFReadScanline(reader.get(), actual.data() + r * row_bytes, r, 0),
            1);
      }
      EXPECT_EQ(actual, expected);
    }
  }

  // Floating-point predictor (PREDICTOR_FLOATINGPOINT) for 32-bit float &
  // 64-bit double in little- and big-endian.
  for (uint16_t bps : {uint16_t{32}, uint16_t{64}}) {
    for (std::string_view mode : {"wl", "wb"}) {
      SCOPED_TRACE(bps);
      SCOPED_TRACE(mode);
      const uint32_t width = 12;
      const uint32_t length = 4;
      const size_t sample_bytes = bps / 8;
      std::vector<uint8_t> expected(width * length * sample_bytes);
      for (size_t i = 0; i < width * length; ++i) {
        if (bps == 32) {
          const float val = static_cast<float>(i) * 0.375F - 12.25F;
          std::memcpy(expected.data() + i * 4, &val, 4);
        } else {
          const double val = static_cast<double>(i) * 0.125 - 45.75;
          std::memcpy(expected.data() + i * 8, &val, 8);
        }
      }

      MemoryTiffStream stream;
      {
        UniqueTiff writer = stream.Open(std::string(mode).c_str());
        ASSERT_NE(writer, nullptr);
        ConfigureBasicStripTags(writer.get(), width, length, bps, 1,
                                PLANARCONFIG_CONTIG, COMPRESSION_DEFLATE, 4);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLEFORMAT,
                               SAMPLEFORMAT_IEEEFP),
                  1);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PREDICTOR,
                               PREDICTOR_FLOATINGPOINT),
                  1);

        std::vector<uint8_t> buf = expected;
        ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                        static_cast<tmsize_t>(buf.size())),
                  static_cast<tmsize_t>(buf.size()));
        ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
      }
      EXPECT_EQ(stream.diagnostics().error_count(), 0U);

      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      std::vector<uint8_t> actual(expected.size(), 0);
      ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, actual.data(),
                                     static_cast<tmsize_t>(actual.size())),
                static_cast<tmsize_t>(actual.size()));
      EXPECT_EQ(actual, expected);
    }
  }
}

TEST(TifZipTest, TiledRoundTripAndRandomScanlineSeek) {
  // Tiled RGB image with horizontal differencing and partial border tiles.
  constexpr uint32_t kTileW = 16;
  constexpr uint32_t kTileH = 16;
  constexpr uint32_t kWidth = 40;
  constexpr uint32_t kHeight = 36;
  constexpr uint16_t kSpp = 3;
  constexpr size_t kTileBytes = static_cast<size_t>(kTileW) * kTileH * kSpp;

  MemoryTiffStream tile_stream;
  const uint32_t tiles_x = (kWidth + kTileW - 1) / kTileW;
  const uint32_t tiles_y = (kHeight + kTileH - 1) / kTileH;
  const size_t n_tiles = tiles_x * tiles_y;
  std::vector<std::vector<uint8_t>> expected_tiles;
  expected_tiles.reserve(n_tiles);
  for (size_t t = 0; t < n_tiles; ++t) {
    expected_tiles.push_back(
        MakePatternBytes(kTileBytes, static_cast<uint32_t>(0x400U + t)));
  }

  {
    UniqueTiff writer = tile_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kWidth), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kHeight), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, kSpp), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION,
                           COMPRESSION_ADOBE_DEFLATE),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, kTileW), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, kTileH), 1);

    uint32_t tile_idx = 0;
    for (uint32_t y = 0; y < kHeight; y += kTileH) {
      for (uint32_t x = 0; x < kWidth; x += kTileW) {
        std::vector<uint8_t> mutable_tile = expected_tiles[tile_idx++];
        ASSERT_EQ(TIFFWriteTile(writer.get(), mutable_tile.data(), x, y, 0, 0),
                  static_cast<tmsize_t>(kTileBytes));
      }
    }
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  EXPECT_EQ(tile_stream.diagnostics().error_count(), 0U);

  UniqueTiff tile_reader = tile_stream.Open("r");
  ASSERT_NE(tile_reader, nullptr);
  ASSERT_EQ(TIFFTileSize(tile_reader.get()), static_cast<tmsize_t>(kTileBytes));
  std::vector<uint8_t> got_tile(kTileBytes, 0);
  for (uint32_t ty = 0; ty < tiles_y; ++ty) {
    for (uint32_t tx = 0; tx < tiles_x; ++tx) {
      const uint32_t x = tx * kTileW;
      const uint32_t y = ty * kTileH;
      const uint32_t t = TIFFComputeTile(tile_reader.get(), x, y, 0, 0);
      std::fill(got_tile.begin(), got_tile.end(), 0);
      ASSERT_EQ(TIFFReadTile(tile_reader.get(), got_tile.data(), x, y, 0, 0),
                static_cast<tmsize_t>(kTileBytes));
      EXPECT_EQ(got_tile, expected_tiles[t]);
    }
  }

  // Random scanline seek forward and backward within and across strips.
  constexpr uint32_t kStripImgW = 24;
  constexpr uint32_t kStripImgH = 12;
  constexpr uint32_t kRowsPerStrip = 4;
  const std::vector<uint8_t> strip_pixels =
      MakePatternBytes(kStripImgW * kStripImgH, 0x888U);
  MemoryTiffStream strip_stream;
  {
    UniqueTiff writer = strip_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripTags(writer.get(), kStripImgW, kStripImgH, 8, 1,
                            PLANARCONFIG_CONTIG, COMPRESSION_DEFLATE,
                            kRowsPerStrip);
    for (uint32_t r = 0; r < kStripImgH; ++r) {
      std::vector<uint8_t> row(strip_pixels.begin() + r * kStripImgW,
                               strip_pixels.begin() + (r + 1) * kStripImgW);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), r), 1);
    }
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = strip_stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> row_out(kStripImgW);
  // Out-of-order strip starts + backward rewind to strip start (0) +
  // sequential continuation within each strip.
  for (uint32_t target_row : {0U, 8U, 9U, 4U, 5U, 0U, 1U}) {
    SCOPED_TRACE(target_row);
    ASSERT_EQ(TIFFReadScanline(reader.get(), row_out.data(), target_row), 1);
    EXPECT_THAT(row_out,
                ::testing::ElementsAreArray(
                    strip_pixels.data() + target_row * kStripImgW, kStripImgW));
  }
  EXPECT_EQ(strip_stream.diagnostics().error_count(), 0U);

  // After reading row 1, td_row is 2; seeking forward mid-strip to row 3 skips
  // row 2 and fails with _TIFFNoSeek.
  EXPECT_EQ(TIFFReadScanline(reader.get(), row_out.data(), 3), -1);
  EXPECT_TRUE(HasDiagnosticSubstr(strip_stream.diagnostics(), "random access"));
}

TEST(TifZipTest, DecodeAndEncodeModeTransitionsOnUpdateHandle) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 4;
  constexpr uint32_t kRowsPerStrip = 2;
  constexpr size_t kStripBytes = kWidth * kRowsPerStrip;

  const std::vector<uint8_t> orig_strip0 = MakePatternBytes(kStripBytes, 0x10U);
  const std::vector<uint8_t> orig_strip1 = MakePatternBytes(kStripBytes, 0x20U);
  const std::vector<uint8_t> new_strip0 = MakePatternBytes(kStripBytes, 0x30U);
  const std::vector<uint8_t> new_strip1 = MakePatternBytes(kStripBytes, 0x40U);

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureBasicStripTags(writer.get(), kWidth, kHeight, 8, 1,
                            PLANARCONFIG_CONTIG, COMPRESSION_DEFLATE,
                            kRowsPerStrip);
    std::vector<uint8_t> s0 = orig_strip0;
    std::vector<uint8_t> s1 = orig_strip1;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(), kStripBytes),
              static_cast<tmsize_t>(kStripBytes));
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, s1.data(), kStripBytes),
              static_cast<tmsize_t>(kStripBytes));
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  // Transition 1: Encode -> Decode in same "r+" session.
  {
    UniqueTiff rw = stream.Open("r+");
    ASSERT_NE(rw, nullptr);
    std::vector<uint8_t> w0 = new_strip0;
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 0, w0.data(), kStripBytes),
              static_cast<tmsize_t>(kStripBytes));

    // TIFF_CODERSETUP is already set, so ZIPPreDecode transitions
    // ZSTATE_INIT_ENCODE -> deflateEnd -> inflateInit.
    std::vector<uint8_t> r1(kStripBytes, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 1, r1.data(), kStripBytes),
              static_cast<tmsize_t>(kStripBytes));
    EXPECT_EQ(r1, orig_strip1);
    ASSERT_EQ(TIFFWriteDirectory(rw.get()), 1);
  }

  // Transition 2: Decode -> Encode in same "r+" session.
  {
    UniqueTiff rw = stream.Open("r+");
    ASSERT_NE(rw, nullptr);
    std::vector<uint8_t> r0(kStripBytes, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, r0.data(), kStripBytes),
              static_cast<tmsize_t>(kStripBytes));
    EXPECT_EQ(r0, new_strip0);

    // ZIPPreEncode sees ZSTATE_INIT_DECODE and calls ZIPSetupEncode ->
    // inflateEnd -> deflateInit.
    std::vector<uint8_t> w1 = new_strip1;
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 1, w1.data(), kStripBytes),
              static_cast<tmsize_t>(kStripBytes));
    ASSERT_EQ(TIFFWriteDirectory(rw.get()), 1);
  }

  UniqueTiff verify = stream.Open("r");
  ASSERT_NE(verify, nullptr);
  std::vector<uint8_t> final0(kStripBytes, 0);
  std::vector<uint8_t> final1(kStripBytes, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(verify.get(), 0, final0.data(), kStripBytes),
            static_cast<tmsize_t>(kStripBytes));
  ASSERT_EQ(TIFFReadEncodedStrip(verify.get(), 1, final1.data(), kStripBytes),
            static_cast<tmsize_t>(kStripBytes));
  EXPECT_EQ(final0, new_strip0);
  EXPECT_EQ(final1, new_strip1);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifZipTest, SetupDecodeReenteredAfterPredictorFailure) {
  const std::vector<uint8_t> expected = MakePatternBytes(16, 0x222U);
  const std::vector<uint8_t> compressed = MakeValidDeflateBlob(expected, 4, 4);
  ASSERT_FALSE(compressed.empty());

  // Craft a file whose 8-bit UINT directory declares PREDICTOR_FLOATINGPOINT.
  // PredictorSetupDecode calls ZIPSetupDecode (sets ZSTATE_INIT_DECODE), then
  // PredictorSetup fails without setting TIFF_CODERSETUP.
  MemoryTiffStream stream(
      BuildSingleStripRawTiff(compressed, 4, 4, COMPRESSION_DEFLATE,
                              PREDICTOR_FLOATINGPOINT, SAMPLEFORMAT_UINT));

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);

  std::vector<uint8_t> out(16, 0);
  EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 16),
            static_cast<tmsize_t>(-1));
  EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "Floating point"));

  // Correct the predictor tag on the open handle; the retry re-enters
  // ZIPSetupDecode when ZSTATE_INIT_DECODE is already set.
  stream.diagnostics().Clear();
  ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PREDICTOR, PREDICTOR_NONE), 1);
  EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 16),
            static_cast<tmsize_t>(16));
  EXPECT_EQ(out, expected);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifZipTest, CorruptTruncatedShortAndStickyReadErrorDecode) {
  // 1. Bad deflate block bytes + subsequent scanline in same strip hits
  // sp->read_error fast-abort and zero-fills buffer.
  {
    constexpr std::array<uint8_t, 6> kBadDeflate = {0x78, 0x9C, 0xFF,
                                                    0xFF, 0xFF, 0xFF};
    MemoryTiffStream stream(
        BuildSingleStripRawTiff(kBadDeflate, 8, 4, COMPRESSION_DEFLATE));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::vector<uint8_t> row0(8, 0xAA);
    EXPECT_EQ(TIFFReadScanline(tif.get(), row0.data(), 0), -1);
    EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "Decoding error"));

    stream.diagnostics().Clear();
    std::vector<uint8_t> row1(8, 0xBB);
    EXPECT_EQ(TIFFReadScanline(tif.get(), row1.data(), 1), -1);
    EXPECT_THAT(row1, Each(0));
    EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(),
                                    "cannot be read due to previous error"));
  }

  // 2. Stored zlib block with valid payload ('ABCD') and corrupted Adler32
  // trailer triggers Z_DATA_ERROR after partial output.
  {
    constexpr std::array<uint8_t, 15> kBadAdler = {
        0x78, 0x01,              // zlib header (no preset dict)
        0x01, 0x04, 0x00,        // final stored block, LEN = 4
        0xFB, 0xFF,              // NLEN = ~4
        'A',  'B',  'C',  'D',   // literal bytes
        0x00, 0x00, 0x00, 0x00,  // corrupted Adler-32
    };
    MemoryTiffStream stream(
        BuildSingleStripRawTiff(kBadAdler, 4, 1, COMPRESSION_DEFLATE));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::vector<uint8_t> out(4, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "Decoding error"));
  }

  // 3. zlib FDICT header triggers Z_NEED_DICT (state != Z_OK branch).
  {
    constexpr std::array<uint8_t, 6> kNeedDict = {0x78, 0xBB, 0x00,
                                                  0x00, 0x00, 0x01};
    MemoryTiffStream stream(
        BuildSingleStripRawTiff(kNeedDict, 4, 1, COMPRESSION_DEFLATE));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::vector<uint8_t> out(4, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "ZLib error"));
  }

  // 4. Truncated zlib stream (header only) triggers Z_BUF_ERROR (state !=
  // Z_OK).
  {
    constexpr std::array<uint8_t, 2> kTruncated = {0x78, 0x9C};
    MemoryTiffStream stream(
        BuildSingleStripRawTiff(kTruncated, 4, 4, COMPRESSION_DEFLATE));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::vector<uint8_t> out(16, 0x55);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_THAT(out, Each(0));
    EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "ZLib error"));
  }

  // 5. Complete zlib stream yielding fewer bytes than strip expects triggers
  // Z_STREAM_END with occ != 0 ("Not enough data at scanline").
  {
    const std::vector<uint8_t> short_payload = {'W', 'X', 'Y', 'Z'};
    const std::vector<uint8_t> short_blob =
        MakeValidDeflateBlob(short_payload, 4, 1);
    ASSERT_FALSE(short_blob.empty());

    MemoryTiffStream stream(
        BuildSingleStripRawTiff(short_blob, 4, 4, COMPRESSION_DEFLATE));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);

    std::vector<uint8_t> out(16, 0xAA);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(),
                                    "Not enough data at scanline"));
    EXPECT_THAT(absl::Span<const uint8_t>(out).subspan(4), Each(0));
  }
}

TEST(TifZipTest, SmallBufferRefillAndEncodeFailurePaths) {
  // 1. Very small 4-byte raw output buffer exercises ZIPEncode avail_out == 0
  // flush loop and ZIPPostEncode Z_OK multi-flush loop cleanly.
  {
    const std::vector<uint8_t> expected = MakePatternBytes(512, 0xDEADU);
    MemoryTiffStream stream;
    {
      UniqueTiff tif = stream.Open("wl");
      ASSERT_NE(tif, nullptr);
      ConfigureBasicStripTags(tif.get(), 64, 8, 8, 1, PLANARCONFIG_CONTIG,
                              COMPRESSION_DEFLATE, 8);
      ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 4), 1);

      std::vector<uint8_t> buf = expected;
      ASSERT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, buf.data(),
                                      static_cast<tmsize_t>(buf.size())),
                static_cast<tmsize_t>(buf.size()));
      ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> actual(expected.size(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, actual.data(),
                                   static_cast<tmsize_t>(actual.size())),
              static_cast<tmsize_t>(actual.size()));
    EXPECT_EQ(actual, expected);
  }

  // 2. Zero-sized user raw buffer causes deflate(Z_NO_FLUSH) error in ZIPEncode
  // and deflate(Z_FINISH) error in ZIPPostEncode on flush.
  {
    MemoryTiffStream stream;
    std::array<uint8_t, 1> dummy{};
    {
      UniqueTiff tif = stream.Open("wl");
      ASSERT_NE(tif, nullptr);
      ConfigureBasicStripTags(tif.get(), 16, 2, 8, 1, PLANARCONFIG_CONTIG,
                              COMPRESSION_DEFLATE, 2);
      ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), dummy.data(), 0), 1);

      std::vector<uint8_t> row(16, 1);
      // TIFFWriteScanline forwards *tif_encoderow's 0 on codec failure.
      EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0), 0);
      EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "Encoder error"));

      stream.diagnostics().Clear();
      EXPECT_EQ(TIFFFlush(tif.get()), 0);
      EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "ZLib error"));
    }
  }

  // 3. Write failure during ZIPEncode flush when raw buffer fills mid-strip.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    // Use 128 KiB (> w_size * 2) at level 0 with a 4-byte raw buffer so
    // ZIPEncode fills avail_out and triggers TIFFFlushData1 before
    // ZIPPostEncode.
    constexpr uint32_t kWidth = 16384;
    constexpr uint32_t kLength = 8;
    ConfigureBasicStripTags(tif.get(), kWidth, kLength, 8, 1,
                            PLANARCONFIG_CONTIG, COMPRESSION_DEFLATE, kLength);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ZIPQUALITY, 0), 1);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 4), 1);

    std::vector<uint8_t> data = MakePatternBytes(kWidth * kLength, 0xFACEU);
    stream.FailWritesAfter(0);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, data.data(),
                                    static_cast<tmsize_t>(data.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "Write error"));
    stream.ResetFaults();
  }

  // 4. Write failure during ZIPPostEncode final flush.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureBasicStripTags(tif.get(), 16, 1, 8, 1, PLANARCONFIG_CONTIG,
                            COMPRESSION_DEFLATE, 1);

    std::vector<uint8_t> data(16, 7);
    stream.FailWritesAfter(0);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, data.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticSubstr(stream.diagnostics(), "Write error"));
    stream.ResetFaults();
  }
}

TEST(TifZipTest, OomFailuresInInitSetupDecodeAndEncode) {
  // 1. Fail _TIFFcallocExt for ZIPState in TIFFInitZIP while allowing
  // _TIFFMergeFields (+16 bytes) to succeed cleanly without leaking tif_fields.
  {
    MemoryTiffStream stream;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    stream.diagnostics().Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);

    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    {
      ScopedCumulatedMemAllocRemaining budget(tif.get(), 64);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE), 0);
    }
    EXPECT_TRUE(
        HasDiagnosticSubstr(stream.diagnostics(), "No space for ZIP state"));
  }

  // 2. Fail inflateInit in ZIPSetupDecode via ScopedSingleMemAllocLimit while
  // mmap avoids separate raw buffer allocation.
  const std::vector<uint8_t> s0 = MakePatternBytes(16, 0x111U);
  const std::vector<uint8_t> s1 = MakePatternBytes(16, 0x222U);
  MemoryTiffStream two_strip_stream;
  {
    UniqueTiff w = two_strip_stream.Open("wl");
    ASSERT_NE(w, nullptr);
    ConfigureBasicStripTags(w.get(), 8, 4, 8, 1, PLANARCONFIG_CONTIG,
                            COMPRESSION_DEFLATE, 2);
    std::vector<uint8_t> b0 = s0;
    std::vector<uint8_t> b1 = s1;
    ASSERT_EQ(TIFFWriteEncodedStrip(w.get(), 0, b0.data(), 16), 16);
    ASSERT_EQ(TIFFWriteEncodedStrip(w.get(), 1, b1.data(), 16), 16);
    ASSERT_EQ(TIFFWriteDirectory(w.get()), 1);
  }

  {
    two_strip_stream.set_enable_mmap(true);
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    two_strip_stream.diagnostics().Install(opts.get());

    UniqueTiff r = two_strip_stream.Open("rm", opts.get());
    ASSERT_NE(r, nullptr);
    std::vector<uint8_t> out(16, 0);
    {
      ScopedSingleMemAllocLimit limit(r.get(), 1024);
      EXPECT_EQ(TIFFReadEncodedStrip(r.get(), 0, out.data(), 16),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_NE(two_strip_stream.diagnostics().error_count(), 0U);
  }

  // 3. Fail ZIPPreDecode (inflateReset returns Z_STREAM_ERROR -> 0) when
  // encode->decode transition in "r+" cannot allocate inflate state.
  {
    MemoryTiffStream rw_stream(two_strip_stream.data());
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    rw_stream.diagnostics().Install(opts.get());
    UniqueTiff rw = rw_stream.Open("r+", opts.get());
    ASSERT_NE(rw, nullptr);

    std::vector<uint8_t> b0 = s0;
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 0, b0.data(), 16), 16);

    std::vector<uint8_t> out(16, 0);
    {
      ScopedSingleMemAllocLimit limit(rw.get(), 1024);
      EXPECT_EQ(TIFFReadEncodedStrip(rw.get(), 1, out.data(), 16),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_NE(rw_stream.diagnostics().error_count(), 0U);
    ASSERT_EQ(TIFFWriteDirectory(rw.get()), 1);
  }

  // 4. Fail deflateInit in ZIPSetupEncode via ScopedSingleMemAllocLimit
  // (raw write buffer is 8192 bytes <= 16384; deflate state > 16384).
  {
    MemoryTiffStream stream;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    stream.diagnostics().Install(opts.get());
    UniqueTiff w = stream.Open("wl", opts.get());
    ASSERT_NE(w, nullptr);
    ConfigureBasicStripTags(w.get(), 8, 2, 8, 1, PLANARCONFIG_CONTIG,
                            COMPRESSION_DEFLATE, 2);
    std::vector<uint8_t> data = s0;
    {
      ScopedSingleMemAllocLimit limit(w.get(), 16384);
      EXPECT_EQ(TIFFWriteEncodedStrip(w.get(), 0, data.data(), 16),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_NE(stream.diagnostics().error_count(), 0U);
  }

  // 5. Fail ZIPPreEncode (deflateReset returns Z_STREAM_ERROR -> 0) when
  // decode->encode transition in "r+" cannot allocate deflate state.
  {
    MemoryTiffStream rw_stream(two_strip_stream.data());
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts, nullptr);
    rw_stream.diagnostics().Install(opts.get());
    UniqueTiff rw = rw_stream.Open("r+", opts.get());
    ASSERT_NE(rw, nullptr);

    std::vector<uint8_t> out(16, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, out.data(), 16), 16);
    EXPECT_EQ(out, s0);

    std::vector<uint8_t> w1 = s1;
    {
      ScopedSingleMemAllocLimit limit(rw.get(), 16384);
      EXPECT_EQ(TIFFWriteEncodedStrip(rw.get(), 1, w1.data(), 16),
                static_cast<tmsize_t>(-1));
    }
    EXPECT_NE(rw_stream.diagnostics().error_count(), 0U);
    ASSERT_EQ(TIFFWriteDirectory(rw.get()), 1);
  }
}

}  // namespace
}  // namespace libtiff
