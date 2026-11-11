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

// Unit tests for libtiff tif_write.c (scanline, strip, tile, buffer setup,
// strip growth, append/relocation, and error handling paths).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

constexpr uint16_t kTestCodecScheme = 65123;

struct MockCodecConfig {
  bool setup_ok = true;
  bool pre_ok = true;
  bool post_ok = true;
  bool encode_row_ok = true;
  bool encode_strip_ok = true;
  bool encode_tile_ok = true;
  int setup_calls = 0;
  int pre_calls = 0;
  int post_calls = 0;
  int row_calls = 0;
  int strip_calls = 0;
  int tile_calls = 0;
};

MockCodecConfig* g_mock_codec_config = nullptr;

int MockSetupEncode(TIFF* /*tif*/) {
  if (g_mock_codec_config == nullptr) return 0;
  ++g_mock_codec_config->setup_calls;
  return g_mock_codec_config->setup_ok ? 1 : 0;
}

int MockPreEncode(TIFF* /*tif*/, uint16_t /*sample*/) {
  if (g_mock_codec_config == nullptr) return 0;
  ++g_mock_codec_config->pre_calls;
  return g_mock_codec_config->pre_ok ? 1 : 0;
}

int MockPostEncode(TIFF* /*tif*/) {
  if (g_mock_codec_config == nullptr) return 0;
  ++g_mock_codec_config->post_calls;
  return g_mock_codec_config->post_ok ? 1 : 0;
}

int MockEncodeRow(TIFF* tif, uint8_t* buf, tmsize_t cc, uint16_t /*sample*/) {
  if (g_mock_codec_config == nullptr) return 0;
  ++g_mock_codec_config->row_calls;
  if (!g_mock_codec_config->encode_row_ok) return 0;
  if (cc > 0 && tif->tif_rawcp != nullptr &&
      tif->tif_rawcc + cc <= tif->tif_rawdatasize) {
    std::memcpy(tif->tif_rawcp, buf, static_cast<size_t>(cc));
    tif->tif_rawcp += cc;
    tif->tif_rawcc += cc;
  }
  return 1;
}

int MockEncodeStrip(TIFF* tif, uint8_t* buf, tmsize_t cc, uint16_t /*sample*/) {
  if (g_mock_codec_config == nullptr) return 0;
  ++g_mock_codec_config->strip_calls;
  if (!g_mock_codec_config->encode_strip_ok) return 0;
  if (cc > 0 && tif->tif_rawcp != nullptr &&
      tif->tif_rawcc + cc <= tif->tif_rawdatasize) {
    std::memcpy(tif->tif_rawcp, buf, static_cast<size_t>(cc));
    tif->tif_rawcp += cc;
    tif->tif_rawcc += cc;
  }
  return 1;
}

int MockEncodeTile(TIFF* tif, uint8_t* buf, tmsize_t cc, uint16_t /*sample*/) {
  if (g_mock_codec_config == nullptr) return 0;
  ++g_mock_codec_config->tile_calls;
  if (!g_mock_codec_config->encode_tile_ok) return 0;
  if (cc > 0 && tif->tif_rawcp != nullptr &&
      tif->tif_rawcc + cc <= tif->tif_rawdatasize) {
    std::memcpy(tif->tif_rawcp, buf, static_cast<size_t>(cc));
    tif->tif_rawcp += cc;
    tif->tif_rawcc += cc;
  }
  return 1;
}

int InitMockCodec(TIFF* tif, int /*scheme*/) {
  tif->tif_setupencode = &MockSetupEncode;
  tif->tif_preencode = &MockPreEncode;
  tif->tif_postencode = &MockPostEncode;
  tif->tif_encoderow = &MockEncodeRow;
  tif->tif_encodestrip = &MockEncodeStrip;
  tif->tif_encodetile = &MockEncodeTile;
  return 1;
}

class ScopedMockCodec {
 public:
  explicit ScopedMockCodec(MockCodecConfig* config)
      : prev_config_(g_mock_codec_config),
        codec_(kTestCodecScheme, "MockTestCodec", &InitMockCodec) {
    g_mock_codec_config = config;
  }

  ~ScopedMockCodec() { g_mock_codec_config = prev_config_; }

  ScopedMockCodec(const ScopedMockCodec&) = delete;
  ScopedMockCodec& operator=(const ScopedMockCodec&) = delete;

  bool is_registered() const { return codec_.get() != nullptr; }

 private:
  MockCodecConfig* prev_config_;
  ScopedRegisteredCodec codec_;
};

UniqueTiffOptions MakeQuietOptions(DiagnosticRecorder& recorder,
                                   tmsize_t max_single = 0,
                                   tmsize_t max_cumulated = 0) {
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  recorder.Install(opts.get());
  if (max_single > 0) {
    TIFFOpenOptionsSetMaxSingleMemAlloc(opts.get(), max_single);
  }
  if (max_cumulated > 0) {
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), max_cumulated);
  }
  return opts;
}

void ConfigureStripedFields(TIFF* tif, uint32_t width, uint32_t length,
                            uint32_t rows_per_strip, uint16_t bits_per_sample,
                            uint16_t samples_per_pixel, uint16_t compression,
                            uint16_t planar_config) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  if (length > 0) {
    ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, length), 1);
  }
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         samples_per_pixel == 3 ? PHOTOMETRIC_RGB
                                                : PHOTOMETRIC_MINISBLACK),
            1);
}

void ConfigureTiledFields(TIFF* tif, uint32_t width, uint32_t length,
                          uint32_t tile_width, uint32_t tile_length,
                          uint16_t bits_per_sample, uint16_t samples_per_pixel,
                          uint16_t compression, uint16_t planar_config) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, length), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile_width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_TILELENGTH, tile_length), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits_per_sample), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, compression), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         samples_per_pixel == 3 ? PHOTOMETRIC_RGB
                                                : PHOTOMETRIC_MINISBLACK),
            1);
}

std::vector<uint8_t> MakePattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> out(size);
  for (size_t i = 0; i < size; ++i) {
    out[i] = static_cast<uint8_t>((seed + i * 37) & 0xffU);
  }
  return out;
}

uint8_t ReverseByteBits(uint8_t b) {
  uint8_t r = 0;
  for (int i = 0; i < 8; ++i) {
    r = static_cast<uint8_t>((r << 1) | ((b >> i) & 1U));
  }
  return r;
}

TEST(TifWriteTest, ScanlineContigRoundTripAcrossFormatsAndCodecs) {
  struct Case {
    const char* write_mode;
    uint16_t compression;
    uint16_t bits_per_sample;
    uint16_t samples_per_pixel;
    bool expect_swab;
  };
  constexpr std::array<Case, 6> kCases = {{
      {"wl", COMPRESSION_NONE, 8, 1, false},
      {"wb", COMPRESSION_NONE, 16, 1, true},
      {"w8l", COMPRESSION_LZW, 8, 3, false},
      {"w8b", COMPRESSION_PACKBITS, 32, 1, true},
      {"wl", COMPRESSION_DEFLATE, 8, 3, false},
      {"wb", COMPRESSION_LZW, 16, 3, true},
  }};

  constexpr uint32_t kWidth = 17;
  constexpr uint32_t kLength = 9;
  constexpr uint32_t kRowsPerStrip = 3;

  for (const Case& c : kCases) {
    SCOPED_TRACE(c.write_mode);
    MemoryTiffStream stream;
    std::vector<std::vector<uint8_t>> expected_rows(kLength);
    {
      UniqueTiff writer = stream.Open(c.write_mode);
      ASSERT_NE(writer, nullptr);
      ConfigureStripedFields(writer.get(), kWidth, kLength, kRowsPerStrip,
                             c.bits_per_sample, c.samples_per_pixel,
                             c.compression, PLANARCONFIG_CONTIG);
      const tmsize_t row_bytes = TIFFScanlineSize(writer.get());
      ASSERT_GT(row_bytes, 0);

      for (uint32_t r = 0; r < kLength; ++r) {
        expected_rows[r] = MakePattern(static_cast<size_t>(row_bytes),
                                       static_cast<uint8_t>(r));
        std::vector<uint8_t> mutable_row = expected_rows[r];
        ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), r, 0), 1);
        if (c.expect_swab) {
          EXPECT_NE(mutable_row, expected_rows[r]);
        } else {
          EXPECT_EQ(mutable_row, expected_rows[r]);
        }
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    const tmsize_t row_bytes = TIFFScanlineSize(reader.get());
    std::vector<uint8_t> got_row(static_cast<size_t>(row_bytes), 0);
    for (uint32_t r = 0; r < kLength; ++r) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), got_row.data(), r, 0), 1);
      EXPECT_EQ(got_row, expected_rows[r]);
    }
  }
}

TEST(TifWriteTest, ScanlineSeparatePlanarAndOutOfRangeErrors) {
  MemoryTiffStream stream;
  constexpr uint32_t kWidth = 12;
  constexpr uint32_t kLength = 6;
  constexpr uint32_t kRowsPerStrip = 2;
  constexpr uint16_t kSamples = 3;

  std::array<std::vector<std::vector<uint8_t>>, kSamples> expected;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedFields(writer.get(), kWidth, kLength, kRowsPerStrip, 8,
                           kSamples, COMPRESSION_NONE, PLANARCONFIG_SEPARATE);
    const tmsize_t row_bytes = TIFFScanlineSize(writer.get());
    ASSERT_EQ(row_bytes, static_cast<tmsize_t>(kWidth));

    // Extending image length is disallowed for PLANARCONFIG_SEPARATE.
    std::vector<uint8_t> bad_row(static_cast<size_t>(row_bytes), 0xAA);
    EXPECT_EQ(TIFFWriteScanline(writer.get(), bad_row.data(), kLength, 0), -1);
    // Out-of-range sample is rejected.
    EXPECT_EQ(TIFFWriteScanline(writer.get(), bad_row.data(), 0, kSamples), -1);

    for (uint16_t s = 0; s < kSamples; ++s) {
      expected[s].resize(kLength);
      for (uint32_t r = 0; r < kLength; ++r) {
        expected[s][r] = MakePattern(static_cast<size_t>(row_bytes),
                                     static_cast<uint8_t>(s * 40 + r));
        std::vector<uint8_t> mutable_row = expected[s][r];
        ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), r, s), 1);
      }
    }
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> got_row(kWidth, 0);
  for (uint16_t s = 0; s < kSamples; ++s) {
    for (uint32_t r = 0; r < kLength; ++r) {
      ASSERT_EQ(TIFFReadScanline(reader.get(), got_row.data(), r, s), 1);
      EXPECT_EQ(got_row, expected[s][r]);
    }
  }
}

TEST(TifWriteTest, ScanlineDynamicGrowthAndBackwardSeekInSameStrip) {
  MemoryTiffStream stream;
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kTotalRows = 7;
  constexpr uint32_t kRowsPerStrip = 4;

  std::vector<std::vector<uint8_t>> expected_rows(kTotalRows);
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    // Omit ImageLength initially to trigger dynamic image growth &
    // TIFFGrowStrips when crossing into strip 1.
    ConfigureStripedFields(writer.get(), kWidth, 0, kRowsPerStrip, 8, 1,
                           COMPRESSION_NONE, PLANARCONFIG_CONTIG);

    for (uint32_t r = 0; r < kTotalRows; ++r) {
      expected_rows[r] = MakePattern(kWidth, static_cast<uint8_t>(r + 5));
      std::vector<uint8_t> mutable_row = expected_rows[r];
      ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), r, 0), 1);
      if (r == 6) {
        // Rewrite row 5 within strip 1 while buffered in rawdata (row < td_row
        // exercises backward seek + DumpModeSeek forward to row 5).
        expected_rows[5] = MakePattern(kWidth, 0x99);
        std::vector<uint8_t> overwrite_row = expected_rows[5];
        ASSERT_EQ(TIFFWriteScanline(writer.get(), overwrite_row.data(), 5, 0),
                  1);
        // Rewrite row 6 so td_row ends cleanly.
        mutable_row = expected_rows[6];
        ASSERT_EQ(TIFFWriteScanline(writer.get(), mutable_row.data(), 6, 0), 1);
      }
    }
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  uint32_t read_len = 0;
  ASSERT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGELENGTH, &read_len), 1);
  EXPECT_EQ(read_len, kTotalRows);

  std::vector<uint8_t> got(kWidth, 0);
  for (uint32_t r = 0; r < kTotalRows; ++r) {
    ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), r, 0), 1);
    EXPECT_EQ(got, expected_rows[r]);
  }
}

TEST(TifWriteTest, ScanlineSeekErrorsForNonSeekableCodec) {
  MemoryTiffStream stream;
  UniqueTiff writer = stream.Open("wl");
  ASSERT_NE(writer, nullptr);
  ConfigureStripedFields(writer.get(), 16, 8, 4, 8, 1, COMPRESSION_PACKBITS,
                         PLANARCONFIG_CONTIG);

  std::vector<uint8_t> row(16, 0x3C);
  ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), 1);
  // Skipping row 1 triggers _TIFFNoSeek in PackBits -> returns -1.
  EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 2, 0), -1);
  // Seeking backward within strip 0 (row 0 < td_row 1) also hits tif_seek(0).
  EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), -1);
}

TEST(TifWriteTest, ScanlineErrorPathsAndOom) {
  // 1. Read-only handle fails WRITECHECKSTRIPS.
  {
    MemoryTiffStream ro_stream;
    {
      UniqueTiff w = ro_stream.Open("wl");
      ASSERT_NE(w, nullptr);
      ConfigureStripedFields(w.get(), 8, 2, 2, 8, 1, COMPRESSION_NONE,
                             PLANARCONFIG_CONTIG);
      std::vector<uint8_t> r(8, 1);
      ASSERT_EQ(TIFFWriteScanline(w.get(), r.data(), 0, 0), 1);
      ASSERT_EQ(TIFFWriteScanline(w.get(), r.data(), 1, 0), 1);
    }
    UniqueTiff r = ro_stream.Open("r");
    ASSERT_NE(r, nullptr);
    std::vector<uint8_t> buf(8, 0);
    EXPECT_EQ(TIFFWriteScanline(r.get(), buf.data(), 0, 0), -1);
  }

  // 2. BUFFERCHECK OOM (single alloc limit < 8192 min raw buffer).
  {
    MemoryTiffStream stream;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 4096, 0);
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 4, 2, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> row(16, 0x11);
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), -1);
    EXPECT_GT(diag.error_count(), 0);
  }

  // 3. TIFFGrowStrips OOM (cumulated limit tightened before crossing to strip
  // 1).
  {
    MemoryTiffStream stream;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 0, 1 << 24);
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 0, 1, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> row(16, 0x22);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
    // Flush strip 0 before tightening OOM so TIFFClose does not flush buffered
    // data after TIFFGrowStrips resets td_nstrips to 0.
    ASSERT_EQ(TIFFFlushData(tif.get()), 1);

    ScopedCumulatedMemAllocRemaining tight_oom(tif.get(), 4);
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 1, 0), -1);
  }

  // 4. TIFFGrowStrips 2nd realloc failure (allows 1st realloc, fails 2nd).
  {
    MemoryTiffStream stream;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 0, 1 << 24);
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 0, 1, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> row(16, 0x22);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
    ASSERT_EQ(TIFFFlushData(tif.get()), 1);

    // First realloc goes from 1*8 -> 2*8 (+8 net bytes); second also +8 bytes.
    // Remaining budget of 12 bytes lets first succeed and fails second.
    ScopedCumulatedMemAllocRemaining half_oom(tif.get(), 12);
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 1, 0), -1);
  }

  // 5. TIFFFlushData write failure when changing strips.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 4, 1, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> row(16, 0x33);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 1, 0), -1);
    stream.ResetFaults();
  }

  // 6. Mock codec setupencode, preencode, postencode, encoderow failures.
  {
    MockCodecConfig cfg;
    ScopedMockCodec mock(&cfg);
    ASSERT_TRUE(mock.is_registered());

    // setupencode fails.
    {
      cfg = MockCodecConfig{.setup_ok = false};
      MemoryTiffStream stream;
      UniqueTiff tif = stream.Open("wl");
      ASSERT_NE(tif, nullptr);
      ConfigureStripedFields(tif.get(), 16, 2, 1, 8, 1, kTestCodecScheme,
                             PLANARCONFIG_CONTIG);
      std::vector<uint8_t> row(16, 0);
      EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), -1);
    }
    // preencode fails.
    {
      cfg = MockCodecConfig{.pre_ok = false};
      MemoryTiffStream stream;
      UniqueTiff tif = stream.Open("wl");
      ASSERT_NE(tif, nullptr);
      ConfigureStripedFields(tif.get(), 16, 2, 1, 8, 1, kTestCodecScheme,
                             PLANARCONFIG_CONTIG);
      std::vector<uint8_t> row(16, 0);
      EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), -1);
    }
    // encoderow fails.
    {
      cfg = MockCodecConfig{.encode_row_ok = false};
      MemoryTiffStream stream;
      UniqueTiff tif = stream.Open("wl");
      ASSERT_NE(tif, nullptr);
      ConfigureStripedFields(tif.get(), 16, 2, 1, 8, 1, kTestCodecScheme,
                             PLANARCONFIG_CONTIG);
      std::vector<uint8_t> row(16, 0);
      EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 0);
    }
    // postencode fails when switching strips.
    {
      cfg = MockCodecConfig{.post_ok = false};
      MemoryTiffStream stream;
      UniqueTiff tif = stream.Open("wl");
      ASSERT_NE(tif, nullptr);
      ConfigureStripedFields(tif.get(), 16, 2, 1, 8, 1, kTestCodecScheme,
                             PLANARCONFIG_CONTIG);
      std::vector<uint8_t> row(16, 0);
      ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
      EXPECT_EQ(TIFFWriteScanline(tif.get(), row.data(), 1, 0), -1);
    }
  }
}

TEST(TifWriteTest, FillOrderLsb2MsbBitReversalAcrossPaths) {
  // 1. Scanline path ("wL" reverses bits on flush via TIFFFlushData1;
  //    explicit TIFFTAG_FILLORDER = FILLORDER_LSB2MSB with "wl" round-trips
  //    cleanly with default "r").
  {
    MemoryTiffStream stream;
    const std::vector<uint8_t> orig = MakePattern(16, 0x5A);
    {
      UniqueTiff writer = stream.Open("wL");
      ASSERT_NE(writer, nullptr);
      ASSERT_NO_FATAL_FAILURE(ConfigureStripedFields(
          writer.get(), 16, 2, 2, 8, 1, COMPRESSION_NONE, PLANARCONFIG_CONTIG));
      for (uint32_t r = 0; r < 2; ++r) {
        std::vector<uint8_t> buf = orig;
        ASSERT_EQ(TIFFWriteScanline(writer.get(), buf.data(), r, 0), 1);
      }
    }
    {
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      std::vector<uint8_t> got(16, 0);
      ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), 0, 0), 1);
      for (size_t i = 0; i < orig.size(); ++i) {
        EXPECT_EQ(got[i], ReverseByteBits(orig[i]));
      }
    }
    {
      UniqueTiff reader = stream.Open("rL");
      ASSERT_NE(reader, nullptr);
      std::vector<uint8_t> got(16, 0);
      ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), 0, 0), 1);
      EXPECT_EQ(got, orig);
    }
  }
  {
    MemoryTiffStream stream;
    const std::vector<uint8_t> orig = MakePattern(16, 0x5A);
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ASSERT_NO_FATAL_FAILURE(ConfigureStripedFields(
          writer.get(), 16, 2, 2, 8, 1, COMPRESSION_NONE, PLANARCONFIG_CONTIG));
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_FILLORDER,
                             uint16_t{FILLORDER_LSB2MSB}),
                1);
      for (uint32_t r = 0; r < 2; ++r) {
        std::vector<uint8_t> buf = orig;
        ASSERT_EQ(TIFFWriteScanline(writer.get(), buf.data(), r, 0), 1);
      }
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(16, 0);
    ASSERT_EQ(TIFFReadScanline(reader.get(), got.data(), 0, 0), 1);
    EXPECT_EQ(got, orig);
  }

  // 2. EncodedStrip with COMPRESSION_NONE and COMPRESSION_PACKBITS.
  {
    MemoryTiffStream stream;
    const std::vector<uint8_t> orig = MakePattern(32, 0x13);
    {
      UniqueTiff writer = stream.Open("wL");
      ASSERT_NE(writer, nullptr);
      ASSERT_NO_FATAL_FAILURE(ConfigureStripedFields(
          writer.get(), 16, 2, 2, 8, 1, COMPRESSION_NONE, PLANARCONFIG_CONTIG));
      std::vector<uint8_t> buf = orig;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                      static_cast<tmsize_t>(buf.size())),
                static_cast<tmsize_t>(orig.size()));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(orig.size(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, got.data(),
                                   static_cast<tmsize_t>(got.size())),
              static_cast<tmsize_t>(orig.size()));
    for (size_t i = 0; i < orig.size(); ++i) {
      EXPECT_EQ(got[i], ReverseByteBits(orig[i]));
    }
  }
  for (uint16_t comp :
       {uint16_t{COMPRESSION_NONE}, uint16_t{COMPRESSION_PACKBITS}}) {
    MemoryTiffStream stream;
    const std::vector<uint8_t> orig = MakePattern(32, 0x13);
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ASSERT_NO_FATAL_FAILURE(ConfigureStripedFields(
          writer.get(), 16, 2, 2, 8, 1, comp, PLANARCONFIG_CONTIG));
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_FILLORDER,
                             uint16_t{FILLORDER_LSB2MSB}),
                1);
      std::vector<uint8_t> buf = orig;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(),
                                      static_cast<tmsize_t>(buf.size())),
                static_cast<tmsize_t>(orig.size()));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(orig.size(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, got.data(),
                                   static_cast<tmsize_t>(got.size())),
              static_cast<tmsize_t>(orig.size()));
    EXPECT_EQ(got, orig);
  }

  // 3. EncodedTile with COMPRESSION_NONE and COMPRESSION_PACKBITS.
  {
    MemoryTiffStream stream;
    const std::vector<uint8_t> orig = MakePattern(256, 0x71);
    {
      UniqueTiff writer = stream.Open("wL");
      ASSERT_NE(writer, nullptr);
      ASSERT_NO_FATAL_FAILURE(ConfigureTiledFields(writer.get(), 16, 16, 16, 16,
                                                   8, 1, COMPRESSION_NONE,
                                                   PLANARCONFIG_CONTIG));
      std::vector<uint8_t> buf = orig;
      ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 0, buf.data(),
                                     static_cast<tmsize_t>(buf.size())),
                static_cast<tmsize_t>(orig.size()));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(orig.size(), 0);
    ASSERT_EQ(TIFFReadEncodedTile(reader.get(), 0, got.data(),
                                  static_cast<tmsize_t>(got.size())),
              static_cast<tmsize_t>(orig.size()));
    for (size_t i = 0; i < orig.size(); ++i) {
      EXPECT_EQ(got[i], ReverseByteBits(orig[i]));
    }
  }
  for (uint16_t comp :
       {uint16_t{COMPRESSION_NONE}, uint16_t{COMPRESSION_PACKBITS}}) {
    MemoryTiffStream stream;
    const std::vector<uint8_t> orig = MakePattern(256, 0x71);
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ASSERT_NO_FATAL_FAILURE(ConfigureTiledFields(
          writer.get(), 16, 16, 16, 16, 8, 1, comp, PLANARCONFIG_CONTIG));
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_FILLORDER,
                             uint16_t{FILLORDER_LSB2MSB}),
                1);
      std::vector<uint8_t> buf = orig;
      ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 0, buf.data(),
                                     static_cast<tmsize_t>(buf.size())),
                static_cast<tmsize_t>(orig.size()));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(orig.size(), 0);
    ASSERT_EQ(TIFFReadEncodedTile(reader.get(), 0, got.data(),
                                  static_cast<tmsize_t>(got.size())),
              static_cast<tmsize_t>(orig.size()));
    EXPECT_EQ(got, orig);
  }
}

TEST(TifWriteTest, EncodedStripRoundTripGrowthAndErrors) {
  // 1. Contig + separate round trip with swabbing and cc==0.
  {
    MemoryTiffStream stream;
    const std::vector<uint8_t> strip0 = MakePattern(64, 1);
    const std::vector<uint8_t> strip1 = MakePattern(64, 90);
    {
      UniqueTiff writer = stream.Open("wb");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedFields(writer.get(), 16, 4, 2, 16, 1, COMPRESSION_NONE,
                             PLANARCONFIG_CONTIG);
      std::vector<uint8_t> b0 = strip0;
      std::vector<uint8_t> b1 = strip1;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, b0.data(), 64), 64);
      EXPECT_NE(b0, strip0);  // Swabbed in-place.
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, b1.data(), 64), 64);
      // Zero-byte write on COMPRESSION_NONE succeeds without appending.
      EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, b1.data(), 0), 0);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(64, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, got.data(), 64), 64);
    EXPECT_EQ(got, strip0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 1, got.data(), 64), 64);
    EXPECT_EQ(got, strip1);
  }

  // 2. Dynamic strip growth in CONTIG vs rejection in SEPARATE.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 2, 2, 8, 1, COMPRESSION_LZW,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> buf = MakePattern(32, 0x07);
    ASSERT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, buf.data(), 32), 32);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 4U), 1);
    // Strip 1 triggers TIFFGrowStrips(tif, 1).
    ASSERT_EQ(TIFFWriteEncodedStrip(tif.get(), 1, buf.data(), 32), 32);
    // Strip 5 (> td_nstrips + 1) triggers _TIFFReserveLargeEnoughWriteBuffer
    // invalid strip_or_tile check.
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 5, buf.data(), 32),
              static_cast<tmsize_t>(-1));
  }

  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 2, 2, 8, 3, COMPRESSION_NONE,
                           PLANARCONFIG_SEPARATE);
    std::vector<uint8_t> buf(32, 0x42);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 3, buf.data(), 32),
              static_cast<tmsize_t>(-1));
  }

  // 3. Error paths: BUFFERCHECK OOM, GrowStrips OOM, write fault, tiled
  // mismatch.
  {
    MemoryTiffStream stream;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 4096, 0);
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 2, 2, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> buf(32, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, buf.data(), 32),
              static_cast<tmsize_t>(-1));
  }
  {
    MemoryTiffStream stream;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 0, 1 << 24);
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 2, 2, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> buf(32, 0);
    ASSERT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, buf.data(), 32), 32);
    ScopedCumulatedMemAllocRemaining oom(tif.get(), 4);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 1, buf.data(), 32),
              static_cast<tmsize_t>(-1));
  }
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureTiledFields(tif.get(), 16, 16, 16, 16, 8, 1, COMPRESSION_NONE,
                         PLANARCONFIG_CONTIG);
    std::vector<uint8_t> buf(256, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, buf.data(), 256),
              static_cast<tmsize_t>(-1));
  }
  {
    // Mock codec failures for EncodedStrip.
    MockCodecConfig cfg;
    ScopedMockCodec mock(&cfg);
    ASSERT_TRUE(mock.is_registered());
    for (int mode = 0; mode < 4; ++mode) {
      cfg = MockCodecConfig{};
      if (mode == 0) cfg.setup_ok = false;
      if (mode == 1) cfg.pre_ok = false;
      if (mode == 2) cfg.encode_strip_ok = false;
      if (mode == 3) cfg.post_ok = false;
      MemoryTiffStream stream;
      UniqueTiff tif = stream.Open("wl");
      ASSERT_NE(tif, nullptr);
      ConfigureStripedFields(tif.get(), 16, 2, 2, 8, 1, kTestCodecScheme,
                             PLANARCONFIG_CONTIG);
      std::vector<uint8_t> buf(32, 0x55);
      EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, buf.data(), 32),
                static_cast<tmsize_t>(-1));
    }
  }
  {
    // Write fault on COMPRESSION_NONE and compressed EncodedStrip.
    for (uint16_t comp :
         {uint16_t{COMPRESSION_NONE}, uint16_t{COMPRESSION_PACKBITS}}) {
      MemoryTiffStream stream;
      UniqueTiff tif = stream.Open("wl");
      ASSERT_NE(tif, nullptr);
      ConfigureStripedFields(tif.get(), 16, 2, 2, 8, 1, comp,
                             PLANARCONFIG_CONTIG);
      std::vector<uint8_t> buf = MakePattern(32, 0x11);
      stream.FailWritesAfter(stream.write_calls());
      EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, buf.data(), 32),
                static_cast<tmsize_t>(-1));
    }
  }
}

TEST(TifWriteTest, WriteRawStripAndErrors) {
  MemoryTiffStream stream;
  const std::vector<uint8_t> part_a = MakePattern(20, 1);
  const std::vector<uint8_t> part_b = MakePattern(12, 55);
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedFields(writer.get(), 16, 2, 2, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> a = part_a;
    std::vector<uint8_t> b = part_b;
    // Append twice to strip 0 (second call leaves td_curstrip == 0).
    ASSERT_EQ(TIFFWriteRawStrip(writer.get(), 0, a.data(), 20), 20);
    ASSERT_EQ(TIFFWriteRawStrip(writer.get(), 0, b.data(), 12), 12);

    // Grow strip 1 dynamically.
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 4U), 1);
    std::vector<uint8_t> s1 = MakePattern(32, 0x80);
    ASSERT_EQ(TIFFWriteRawStrip(writer.get(), 1, s1.data(), 32), 32);

    // NOSTRIP fails cleanly in TIFFAppendToStrip.
    EXPECT_EQ(TIFFWriteRawStrip(writer.get(), static_cast<uint32_t>(-1),
                                s1.data(), 4),
              static_cast<tmsize_t>(-1));
  }

  // Read back strip 0 raw bytes and verify concatenation.
  {
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(32, 0);
    ASSERT_EQ(TIFFReadRawStrip(reader.get(), 0, got.data(), 32), 32);
    EXPECT_TRUE(std::equal(part_a.begin(), part_a.end(), got.begin()));
    EXPECT_TRUE(
        std::equal(part_b.begin(), part_b.end(), got.begin() + part_a.size()));
  }

  // Separate planar config cannot grow strips in TIFFWriteRawStrip.
  {
    MemoryTiffStream s2;
    UniqueTiff tif = s2.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 8, 2, 2, 8, 3, COMPRESSION_NONE,
                           PLANARCONFIG_SEPARATE);
    std::vector<uint8_t> data(16, 0);
    EXPECT_EQ(TIFFWriteRawStrip(tif.get(), 3, data.data(), 16),
              static_cast<tmsize_t>(-1));
  }

  // Tiled image fails WRITECHECKSTRIPS in TIFFWriteRawStrip.
  {
    MemoryTiffStream s3;
    UniqueTiff tif = s3.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureTiledFields(tif.get(), 16, 16, 16, 16, 8, 1, COMPRESSION_NONE,
                         PLANARCONFIG_CONTIG);
    std::vector<uint8_t> data(16, 0);
    EXPECT_EQ(TIFFWriteRawStrip(tif.get(), 0, data.data(), 16),
              static_cast<tmsize_t>(-1));
  }

  // OOM during TIFFGrowStrips in TIFFWriteRawStrip + write failure.
  {
    MemoryTiffStream s4;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 0, 1 << 24);
    UniqueTiff tif = s4.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 8, 2, 2, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> data(16, 0);
    ASSERT_EQ(TIFFWriteRawStrip(tif.get(), 0, data.data(), 16), 16);
    {
      ScopedCumulatedMemAllocRemaining oom(tif.get(), 4);
      EXPECT_EQ(TIFFWriteRawStrip(tif.get(), 1, data.data(), 16),
                static_cast<tmsize_t>(-1));
    }
    s4.FailWritesAfter(s4.write_calls());
    EXPECT_EQ(TIFFWriteRawStrip(tif.get(), 0, data.data(), 16),
              static_cast<tmsize_t>(-1));
  }
}

TEST(TifWriteTest, WriteTileEncodedTileAndRawTileRoundTripAndErrors) {
  // 1. Contiguous tiled image via TIFFWriteTile (LZW & big-endian 16-bit).
  {
    MemoryTiffStream stream;
    const tmsize_t tile_bytes = 16 * 16 * 2;
    std::array<std::vector<uint8_t>, 4> expected;
    {
      UniqueTiff writer = stream.Open("wb");
      ASSERT_NE(writer, nullptr);
      ConfigureTiledFields(writer.get(), 32, 32, 16, 16, 16, 1,
                           COMPRESSION_NONE, PLANARCONFIG_CONTIG);
      ASSERT_EQ(TIFFTileSize(writer.get()), tile_bytes);

      // Out of bounds x/y/z in TIFFWriteTile.
      std::vector<uint8_t> tmp(static_cast<size_t>(tile_bytes), 0);
      EXPECT_EQ(TIFFWriteTile(writer.get(), tmp.data(), 32, 0, 0, 0),
                static_cast<tmsize_t>(-1));
      EXPECT_EQ(TIFFWriteTile(writer.get(), tmp.data(), 0, 32, 0, 0),
                static_cast<tmsize_t>(-1));
      EXPECT_EQ(TIFFWriteTile(writer.get(), tmp.data(), 0, 0, 1, 0),
                static_cast<tmsize_t>(-1));

      for (int idx = 0; idx < 4; ++idx) {
        uint32_t x = (idx % 2) * 16;
        uint32_t y = (idx / 2) * 16;
        expected[idx] = MakePattern(static_cast<size_t>(tile_bytes),
                                    static_cast<uint8_t>(idx * 23));
        std::vector<uint8_t> mut = expected[idx];
        ASSERT_EQ(TIFFWriteTile(writer.get(), mut.data(), x, y, 0, 0),
                  tile_bytes);
        EXPECT_NE(mut, expected[idx]);  // Swabbed in-place.
      }
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(static_cast<size_t>(tile_bytes), 0);
    for (int idx = 0; idx < 4; ++idx) {
      uint32_t x = (idx % 2) * 16;
      uint32_t y = (idx / 2) * 16;
      ASSERT_EQ(TIFFReadTile(reader.get(), got.data(), x, y, 0, 0), tile_bytes);
      EXPECT_EQ(got, expected[idx]);
    }
  }

  // 2. Separate planar 3D tiled image with LZW + clamping cc in EncodedTile.
  {
    MemoryTiffStream stream;
    constexpr tmsize_t kTileBytes = 16 * 16;
    {
      UniqueTiff writer = stream.Open("w8l");
      ASSERT_NE(writer, nullptr);
      ConfigureTiledFields(writer.get(), 16, 16, 16, 16, 8, 2, COMPRESSION_LZW,
                           PLANARCONFIG_SEPARATE);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEDEPTH, 2U), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEDEPTH, 1U), 1);

      std::vector<uint8_t> t0 = MakePattern(kTileBytes, 10);
      // Sample out of range in TIFFWriteTile.
      EXPECT_EQ(TIFFWriteTile(writer.get(), t0.data(), 0, 0, 0, 2),
                static_cast<tmsize_t>(-1));
      // Out of range tile index in TIFFWriteEncodedTile.
      EXPECT_EQ(TIFFWriteEncodedTile(writer.get(), 4, t0.data(), kTileBytes),
                static_cast<tmsize_t>(-1));
      // cc < 1 clamps to td_tilesize.
      ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 0, t0.data(), -1),
                kTileBytes);
      // cc > td_tilesize clamps to td_tilesize.
      std::vector<uint8_t> big_buf = MakePattern(kTileBytes + 64, 20);
      ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 1, big_buf.data(),
                                     kTileBytes + 64),
                kTileBytes);
      std::vector<uint8_t> t2 = MakePattern(kTileBytes, 30);
      std::vector<uint8_t> t3 = MakePattern(kTileBytes, 40);
      ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 2, t2.data(), kTileBytes),
                kTileBytes);
      ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 3, t3.data(), kTileBytes),
                kTileBytes);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(kTileBytes, 0);
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 0, got.data(), kTileBytes),
              kTileBytes);
    EXPECT_EQ(got, MakePattern(kTileBytes, 10));
    EXPECT_EQ(TIFFReadEncodedTile(reader.get(), 1, got.data(), kTileBytes),
              kTileBytes);
    EXPECT_EQ(got, MakePattern(kTileBytes, 20));
  }

  // 3. TIFFWriteRawTile round trip and error paths.
  {
    MemoryTiffStream stream;
    const std::vector<uint8_t> raw0 = MakePattern(256, 0x31);
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureTiledFields(writer.get(), 16, 16, 16, 16, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
      std::vector<uint8_t> buf = raw0;
      ASSERT_EQ(TIFFWriteRawTile(writer.get(), 0, buf.data(), 256), 256);
      EXPECT_EQ(TIFFWriteRawTile(writer.get(), 1, buf.data(), 256),
                static_cast<tmsize_t>(-1));
      stream.FailWritesAfter(stream.write_calls());
      EXPECT_EQ(TIFFWriteRawTile(writer.get(), 0, buf.data(), 256),
                static_cast<tmsize_t>(-1));
      stream.ResetFaults();
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(256, 0);
    ASSERT_EQ(TIFFReadRawTile(reader.get(), 0, got.data(), 256), 256);
    EXPECT_EQ(got, raw0);
  }

  // 4. EncodedTile error paths: striped mismatch, OOM, codec failures, write
  // fault.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 16, 16, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> buf(256, 0);
    EXPECT_EQ(TIFFWriteEncodedTile(tif.get(), 0, buf.data(), 256),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFWriteRawTile(tif.get(), 0, buf.data(), 256),
              static_cast<tmsize_t>(-1));
  }
  {
    MemoryTiffStream stream;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 4096, 0);
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    ConfigureTiledFields(tif.get(), 16, 16, 16, 16, 8, 1, COMPRESSION_NONE,
                         PLANARCONFIG_CONTIG);
    std::vector<uint8_t> buf(256, 0);
    EXPECT_EQ(TIFFWriteEncodedTile(tif.get(), 0, buf.data(), 256),
              static_cast<tmsize_t>(-1));
  }
  {
    MockCodecConfig cfg;
    ScopedMockCodec mock(&cfg);
    ASSERT_TRUE(mock.is_registered());
    for (int mode = 0; mode < 4; ++mode) {
      cfg = MockCodecConfig{};
      if (mode == 0) cfg.setup_ok = false;
      if (mode == 1) cfg.pre_ok = false;
      if (mode == 2) cfg.encode_tile_ok = false;
      if (mode == 3) cfg.post_ok = false;
      MemoryTiffStream stream;
      UniqueTiff tif = stream.Open("wl");
      ASSERT_NE(tif, nullptr);
      ConfigureTiledFields(tif.get(), 16, 16, 16, 16, 8, 1, kTestCodecScheme,
                           PLANARCONFIG_CONTIG);
      std::vector<uint8_t> buf(256, 0);
      EXPECT_EQ(TIFFWriteEncodedTile(tif.get(), 0, buf.data(), 256),
                static_cast<tmsize_t>(-1));
    }
  }
  for (uint16_t comp :
       {uint16_t{COMPRESSION_NONE}, uint16_t{COMPRESSION_PACKBITS}}) {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureTiledFields(tif.get(), 16, 16, 16, 16, 8, 1, comp,
                         PLANARCONFIG_CONTIG);
    std::vector<uint8_t> buf = MakePattern(256, 0x29);
    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFWriteEncodedTile(tif.get(), 0, buf.data(), 256),
              static_cast<tmsize_t>(-1));
  }
}

TEST(TifWriteTest, SetupStripsWriteCheckAndWriteBufferSetup) {
  // 1. ClassicTIFF and BigTIFF too-many-strips limit in TIFFSetupStrips.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 1, 0x20000000U, 1, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    EXPECT_EQ(TIFFSetupStrips(tif.get()), 0);
    EXPECT_EQ(TIFFWriteCheck(tif.get(), 0, "test"), 0);
  }
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w8l");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 1, 0x10000000U, 1, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    EXPECT_EQ(TIFFSetupStrips(tif.get()), 0);
  }

  // 2. Unspecified ImageLength on tiled image + calling TIFFSetupStrips twice +
  //    OOM on stripbytecount array.
  {
    MemoryTiffStream stream;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 0, 1 << 24);
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
        1);
    ASSERT_EQ(TIFFSetupStrips(tif.get()), 1);
    // Calling TIFFSetupStrips a second time frees previous arrays cleanly.
    ASSERT_EQ(TIFFSetupStrips(tif.get()), 1);

    // Reset strip arrays to NULL via TIFFCreateDirectory, then fail 1st and
    // 2nd allocations in TIFFSetupStrips cleanly.
    ASSERT_EQ(TIFFCreateDirectory(tif.get()), 0);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
        1);
    {
      ScopedSingleMemAllocLimit single_oom(tif.get(), 8);
      EXPECT_EQ(TIFFSetupStrips(tif.get()), 0);
    }
    // 28 bytes remaining allows first 24-byte alloc, fails second 24-byte
    // alloc.
    ScopedCumulatedMemAllocRemaining half_oom(tif.get(), 28);
    EXPECT_EQ(TIFFSetupStrips(tif.get()), 0);
  }

  // 3. TIFFWriteCheck edge errors: missing ImageWidth, zero tilesize, zero
  //    scanlinesize.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFWriteCheck(tif.get(), 0, "test"), 0);

    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 16U), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, 16U), 1);
    // TileLength still 0 -> td_tilesize == 0.
    EXPECT_EQ(TIFFWriteCheck(tif.get(), 1, "test"), 0);
  }
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 4, 2, 0, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    // BitsPerSample == 0 -> td_scanlinesize == 0.
    EXPECT_EQ(TIFFWriteCheck(tif.get(), 0, "test"), 0);
  }

  // 4. TIFFWriteBufferSetup with user-owned buffer, auto-sized, and OOM.
  {
    std::vector<uint8_t> user_buf(4096, 0);
    MemoryTiffStream stream;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 0, 1 << 24);
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 4, 2, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    ASSERT_EQ(TIFFWriteCheck(tif.get(), 0, "test"), 1);

    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), user_buf.data(), 4096), 1);
    // Replace caller-owned buffer with library-owned auto buffer, then explicit
    // sized library buffer (frees previous TIFF_MYBUFFER), then OOM.
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, -1), 1);
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 1024), 1);
    {
      ScopedSingleMemAllocLimit oom(tif.get(), 512);
      EXPECT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 4096), 0);
    }
    // Restore user buffer so tif->tif_rawdata is non-null & clean on close.
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), user_buf.data(), 4096), 1);
  }
}

TEST(TifWriteTest, DeferredStrileArrayWritingForcesStrileWriteOnCheck) {
  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedFields(writer.get(), 16, 2, 2, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    ASSERT_EQ(TIFFDeferStrileArrayWriting(writer.get()), 1);
    ASSERT_EQ(TIFFWriteCheck(writer.get(), 0, "test"), 1);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }
  {
    UniqueTiff updater = stream.Open("r+");
    ASSERT_NE(updater, nullptr);
    std::vector<uint8_t> strip = MakePattern(32, 0x44);
    ASSERT_EQ(TIFFWriteEncodedStrip(updater.get(), 0, strip.data(), 32), 32);
  }
  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> got(32, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, got.data(), 32), 32);
  EXPECT_EQ(got, MakePattern(32, 0x44));
}

TEST(TifWriteTest, ReserveLargeEnoughWriteBufferAndOom) {
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedFields(writer.get(), 16, 16, 16, 8, 1, COMPRESSION_NONE,
                             PLANARCONFIG_CONTIG);
      std::vector<uint8_t> s0 = MakePattern(256, 1);
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(), 256), 256);
    }

    // Reopen in r+ with a tiny user buffer so
    // _TIFFReserveLargeEnoughWriteBuffer sees tif_rawdatasize <= stripbytecount
    // + 5 and reallocates.
    std::vector<uint8_t> small_buf(64, 0);
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 0, 1 << 24);
    UniqueTiff updater = stream.Open("r+", opts.get());
    ASSERT_NE(updater, nullptr);
    ASSERT_EQ(TIFFWriteBufferSetup(updater.get(), small_buf.data(), 64), 1);
    {
      ScopedSingleMemAllocLimit oom(updater.get(), 256);
      std::vector<uint8_t> s0 = MakePattern(256, 2);
      EXPECT_EQ(TIFFWriteEncodedStrip(updater.get(), 0, s0.data(), 256),
                static_cast<tmsize_t>(-1));
    }
    ASSERT_EQ(TIFFWriteBufferSetup(updater.get(), small_buf.data(), 64), 1);
    std::vector<uint8_t> s0 = MakePattern(256, 3);
    EXPECT_EQ(TIFFWriteEncodedStrip(updater.get(), 0, s0.data(), 256), 256);
  }

  // Same reallocation OOM guard on TIFFWriteEncodedTile.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureTiledFields(writer.get(), 16, 16, 16, 16, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
      std::vector<uint8_t> t0 = MakePattern(256, 4);
      ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 0, t0.data(), 256), 256);
    }

    std::vector<uint8_t> small_buf(64, 0);
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 0, 1 << 24);
    UniqueTiff updater = stream.Open("r+", opts.get());
    ASSERT_NE(updater, nullptr);
    ASSERT_EQ(TIFFWriteBufferSetup(updater.get(), small_buf.data(), 64), 1);
    {
      ScopedSingleMemAllocLimit oom(updater.get(), 256);
      std::vector<uint8_t> t0 = MakePattern(256, 5);
      EXPECT_EQ(TIFFWriteEncodedTile(updater.get(), 0, t0.data(), 256),
                static_cast<tmsize_t>(-1));
    }
    ASSERT_EQ(TIFFWriteBufferSetup(updater.get(), small_buf.data(), 64), 1);
    std::vector<uint8_t> t0 = MakePattern(256, 6);
    EXPECT_EQ(TIFFWriteEncodedTile(updater.get(), 0, t0.data(), 256), 256);
  }
}

TEST(TifWriteTest, AppendToStripInPlaceRewriteAndEofRelocation) {
  // 1. Small/same-size in-place rewrite & multi-call relocation to EOF.
  {
    MemoryTiffStream stream;
    const std::vector<uint8_t> orig0 = MakePattern(256, 1);
    const std::vector<uint8_t> orig1 = MakePattern(256, 2);
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ConfigureStripedFields(writer.get(), 16, 32, 16, 8, 1, COMPRESSION_NONE,
                             PLANARCONFIG_CONTIG);
      std::vector<uint8_t> b0 = orig0;
      std::vector<uint8_t> b1 = orig1;
      ASSERT_EQ(TIFFWriteRawStrip(writer.get(), 0, b0.data(), 256), 256);
      ASSERT_EQ(TIFFWriteRawStrip(writer.get(), 1, b1.data(), 256), 256);
    }

    // Reopen r+: rewrite strip 1 with exact same byte count, and rewrite strip
    // 0 in two calls (64 bytes in-place, then 256 bytes triggering relocation).
    const std::vector<uint8_t> part1 = MakePattern(64, 0xA0);
    const std::vector<uint8_t> part2 = MakePattern(256, 0xB0);
    {
      UniqueTiff updater = stream.Open("r+");
      ASSERT_NE(updater, nullptr);
      std::vector<uint8_t> b1 = orig1;
      ASSERT_EQ(TIFFWriteRawStrip(updater.get(), 1, b1.data(), 256), 256);

      std::vector<uint8_t> p1 = part1;
      std::vector<uint8_t> p2 = part2;
      ASSERT_EQ(TIFFWriteRawStrip(updater.get(), 0, p1.data(), 64), 64);
      ASSERT_EQ(TIFFWriteRawStrip(updater.get(), 0, p2.data(), 256), 256);
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got0(320, 0);
    std::vector<uint8_t> got1(256, 0);
    ASSERT_EQ(TIFFReadRawStrip(reader.get(), 0, got0.data(), 320), 320);
    EXPECT_TRUE(std::equal(part1.begin(), part1.end(), got0.begin()));
    EXPECT_TRUE(
        std::equal(part2.begin(), part2.end(), got0.begin() + part1.size()));
    ASSERT_EQ(TIFFReadRawStrip(reader.get(), 1, got1.data(), 256), 256);
    EXPECT_EQ(got1, orig1);
  }

  // 2. Relocation > 1 MiB chunk copy loop (toCopy > 1024*1024).
  //    Use 2 strips (ImageLength = 1201, RowsPerStrip = 1200) so
  //    STRIPCHOP_DEFAULT does not chop strip 0 into 8 KB chunks on "r+".
  {
    MemoryTiffStream stream;
    constexpr tmsize_t kOrigSize = 1200000;
    constexpr tmsize_t kPart1Size = 1100000;
    constexpr tmsize_t kPart2Size = 200000;
    const std::vector<uint8_t> strip1_orig = MakePattern(1000, 0x3A);
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ASSERT_NO_FATAL_FAILURE(
          ConfigureStripedFields(writer.get(), 1000, 1201, 1200, 8, 1,
                                 COMPRESSION_NONE, PLANARCONFIG_CONTIG));
      std::vector<uint8_t> orig(kOrigSize, 0x5E);
      ASSERT_EQ(TIFFWriteRawStrip(writer.get(), 0, orig.data(), kOrigSize),
                kOrigSize);
      std::vector<uint8_t> s1 = strip1_orig;
      ASSERT_EQ(TIFFWriteRawStrip(writer.get(), 1, s1.data(), 1000), 1000);
    }
    const std::vector<uint8_t> part1 = MakePattern(kPart1Size, 0x11);
    const std::vector<uint8_t> part2 = MakePattern(kPart2Size, 0x77);
    {
      UniqueTiff updater = stream.Open("r+");
      ASSERT_NE(updater, nullptr);
      std::vector<uint8_t> p1 = part1;
      std::vector<uint8_t> p2 = part2;
      ASSERT_EQ(TIFFWriteRawStrip(updater.get(), 0, p1.data(), kPart1Size),
                kPart1Size);
      ASSERT_EQ(TIFFWriteRawStrip(updater.get(), 0, p2.data(), kPart2Size),
                kPart2Size);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(kPart1Size + kPart2Size, 0);
    ASSERT_EQ(
        TIFFReadRawStrip(reader.get(), 0, got.data(), kPart1Size + kPart2Size),
        kPart1Size + kPart2Size);
    EXPECT_TRUE(std::equal(part1.begin(), part1.end(), got.begin()));
    EXPECT_TRUE(
        std::equal(part2.begin(), part2.end(), got.begin() + kPart1Size));
    std::vector<uint8_t> got1(1000, 0);
    ASSERT_EQ(TIFFReadRawStrip(reader.get(), 1, got1.data(), 1000), 1000);
    EXPECT_EQ(got1, strip1_orig);
  }
}

TEST(TifWriteTest, AppendToStripRelocationFaultInjectionAndOom) {
  MemoryTiffStream base_stream;
  {
    UniqueTiff writer = base_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedFields(writer.get(), 16, 16, 16, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> orig(256, 0x41);
    ASSERT_EQ(TIFFWriteRawStrip(writer.get(), 0, orig.data(), 256), 256);
  }

  std::vector<uint8_t> p1(64, 0x42);
  std::vector<uint8_t> p2(256, 0x43);

  // 1. SeekOK failure when starting in-place rewrite.
  {
    MemoryTiffStream s(base_stream.data());
    UniqueTiff tif = s.Open("r+");
    ASSERT_NE(tif, nullptr);
    s.FailSeeksAfter(s.seek_calls());
    EXPECT_EQ(TIFFWriteRawStrip(tif.get(), 0, p1.data(), 64),
              static_cast<tmsize_t>(-1));
  }

  // 2. OOM allocating temp copy buffer during relocation.
  {
    MemoryTiffStream s(base_stream.data());
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 0, 1 << 24);
    UniqueTiff tif = s.Open("r+", opts.get());
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFWriteRawStrip(tif.get(), 0, p1.data(), 64), 64);
    ScopedSingleMemAllocLimit oom(tif.get(), 16);
    EXPECT_EQ(TIFFWriteRawStrip(tif.get(), 0, p2.data(), 256),
              static_cast<tmsize_t>(-1));
  }

  // 3. Relocation SeekOK (read), ReadOK, SeekOK (write), and WriteOK faults.
  for (int fault_kind = 0; fault_kind < 4; ++fault_kind) {
    MemoryTiffStream s(base_stream.data());
    UniqueTiff tif = s.Open("r+");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFWriteRawStrip(tif.get(), 0, p1.data(), 64), 64);

    if (fault_kind == 0) {
      // Allow SEEK_END (offsetWrite), fail SeekOK(offsetRead).
      s.FailSeeksAfter(s.seek_calls() + 1);
    } else if (fault_kind == 1) {
      // Fail ReadOK during copy.
      s.FailReadsAfter(s.read_calls());
    } else if (fault_kind == 2) {
      // Allow SEEK_END and SeekOK(offsetRead), fail SeekOK(offsetWrite).
      s.FailSeeksAfter(s.seek_calls() + 2);
    } else {
      // Fail WriteOK during copy.
      s.FailWritesAfter(s.write_calls());
    }
    EXPECT_EQ(TIFFWriteRawStrip(tif.get(), 0, p2.data(), 256),
              static_cast<tmsize_t>(-1));
  }
}

TEST(TifWriteTest, AppendToStripMaxFileSizeOverflowClassicAndBigTiff) {
  // ClassicTIFF 4 GiB boundary check via TIFFSetWriteOffset.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 4, 4, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> d(16, 0x11);
    ASSERT_EQ(TIFFWriteRawStrip(tif.get(), 0, d.data(), 10), 10);
    TIFFSetWriteOffset(tif.get(), 0xFFFFFFF8ULL);
    EXPECT_EQ(TIFFWriteRawStrip(tif.get(), 0, d.data(), 16),
              static_cast<tmsize_t>(-1));
    TIFFSetWriteOffset(tif.get(), 0);
  }

  // BigTIFF uint64 wraparound check via TIFFSetWriteOffset.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w8l");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 4, 4, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> d(16, 0x22);
    ASSERT_EQ(TIFFWriteRawStrip(tif.get(), 0, d.data(), 10), 10);
    TIFFSetWriteOffset(tif.get(), std::numeric_limits<uint64_t>::max() - 8);
    EXPECT_EQ(TIFFWriteRawStrip(tif.get(), 0, d.data(), 16),
              static_cast<tmsize_t>(-1));
    TIFFSetWriteOffset(tif.get(), 0);
  }
}

struct SpoofedEofStream {
  std::vector<uint8_t> data;
  toff_t pos = 0;
  bool spoof_huge_eof = false;
  DiagnosticRecorder diagnostics;

  static tmsize_t ReadProc(thandle_t h, void* buf, tmsize_t sz) {
    auto* s = static_cast<SpoofedEofStream*>(h);
    if (sz < 0 || s->pos >= s->data.size()) return 0;
    const size_t n = std::min(static_cast<size_t>(sz),
                              s->data.size() - static_cast<size_t>(s->pos));
    std::memcpy(buf, s->data.data() + s->pos, n);
    s->pos += n;
    return static_cast<tmsize_t>(n);
  }

  static tmsize_t WriteProc(thandle_t h, void* buf, tmsize_t sz) {
    auto* s = static_cast<SpoofedEofStream*>(h);
    if (sz < 0) return -1;
    const size_t end = static_cast<size_t>(s->pos) + static_cast<size_t>(sz);
    if (end > s->data.size()) s->data.resize(end, 0);
    std::memcpy(s->data.data() + s->pos, buf, static_cast<size_t>(sz));
    s->pos = end;
    return sz;
  }

  static toff_t SeekProc(thandle_t h, toff_t off, int whence) {
    auto* s = static_cast<SpoofedEofStream*>(h);
    if (whence == SEEK_END) {
      if (s->spoof_huge_eof && off == 0) {
        s->pos = 0xFFFFFFF0ULL;
        return s->pos;
      }
      s->pos = s->data.size() + off;
      return s->pos;
    }
    if (whence == SEEK_SET) {
      s->pos = off;
      return s->pos;
    }
    if (whence == SEEK_CUR) {
      s->pos += off;
      return s->pos;
    }
    return static_cast<toff_t>(-1);
  }

  static int CloseProc(thandle_t /*h*/) { return 0; }
  static toff_t SizeProc(thandle_t h) {
    return static_cast<SpoofedEofStream*>(h)->data.size();
  }
};

TEST(TifWriteTest, AppendToStripRelocationClassicMaxFileSizeExceeded) {
  MemoryTiffStream init_stream;
  {
    UniqueTiff writer = init_stream.Open("wl");
    ASSERT_NE(writer, nullptr);
    ConfigureStripedFields(writer.get(), 16, 16, 16, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> orig(256, 0x33);
    ASSERT_EQ(TIFFWriteRawStrip(writer.get(), 0, orig.data(), 256), 256);
  }

  SpoofedEofStream spoof;
  spoof.data = init_stream.data();
  UniqueTiffOptions opts = MakeQuietOptions(spoof.diagnostics);
  UniqueTiff tif(TIFFClientOpenExt(
      "spoof.tiff", "r+", static_cast<thandle_t>(&spoof),
      &SpoofedEofStream::ReadProc, &SpoofedEofStream::WriteProc,
      &SpoofedEofStream::SeekProc, &SpoofedEofStream::CloseProc,
      &SpoofedEofStream::SizeProc, nullptr, nullptr, opts.get()));
  ASSERT_NE(tif, nullptr);

  std::vector<uint8_t> p1(64, 0x44);
  std::vector<uint8_t> p2(256, 0x55);
  ASSERT_EQ(TIFFWriteRawStrip(tif.get(), 0, p1.data(), 64), 64);

  spoof.spoof_huge_eof = true;
  EXPECT_EQ(TIFFWriteRawStrip(tif.get(), 0, p2.data(), 256),
            static_cast<tmsize_t>(-1));
  spoof.spoof_huge_eof = false;
}

TEST(TifWriteTest, FlushData1NullStripArraysAndWriteErrors) {
  // 1. No-op when rawcc == 0.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFFlushData1(tif.get()), 1);
  }

  // 2. Write failure inside TIFFFlushData1 resets rawcc and rawcp.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ConfigureStripedFields(tif.get(), 16, 4, 4, 8, 1, COMPRESSION_NONE,
                           PLANARCONFIG_CONTIG);
    std::vector<uint8_t> row(16, 0x77);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
    stream.FailWritesAfter(stream.write_calls());
    EXPECT_EQ(TIFFFlushData1(tif.get()), 0);
    EXPECT_EQ(TIFFFlushData1(tif.get()), 1);
    stream.ResetFaults();
  }

  // 3a. td_stripoffset_p == NULL check in TIFFFlushData1.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("wl");
    ASSERT_NE(tif, nullptr);
    ASSERT_NO_FATAL_FAILURE(ConfigureStripedFields(
        tif.get(), 16, 4, 4, 8, 1, COMPRESSION_NONE, PLANARCONFIG_CONTIG));
    std::vector<uint8_t> row(16, 0x88);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);

    // TIFFCreateDirectory resets tif_dir (td_stripoffset_p == NULL) while
    // TIFF_BUF4WRITE and tif_rawcc > 0 remain.
    ASSERT_EQ(TIFFCreateDirectory(tif.get()), 0);
    EXPECT_EQ(TIFFFlushData1(tif.get()), 0);
  }

  // 3b. td_stripbytecount_p == NULL check in TIFFFlushData1 and
  //     _TIFFReserveLargeEnoughWriteBuffer.
  {
    MemoryTiffStream stream;
    DiagnosticRecorder diag;
    UniqueTiffOptions opts = MakeQuietOptions(diag, 0, 1 << 24);
    UniqueTiff tif = stream.Open("wl", opts.get());
    ASSERT_NE(tif, nullptr);
    ASSERT_NO_FATAL_FAILURE(ConfigureStripedFields(
        tif.get(), 16, 4, 4, 8, 1, COMPRESSION_NONE, PLANARCONFIG_CONTIG));
    // Fail 2nd 8-byte malloc in TIFFSetupStrips so td_stripoffset_p != NULL
    // and td_stripbytecount_p == NULL.
    {
      ScopedCumulatedMemAllocRemaining half_oom(tif.get(), 10);
      EXPECT_EQ(TIFFSetupStrips(tif.get()), 0);
    }
    // TIFFWriteCheck passes since td_stripoffset_p != NULL; TIFFWriteScanline
    // buffers 16 bytes, then TIFFFlushData1 rejects td_stripbytecount_p == NULL
    // and resets tif_rawcc to 0.
    std::vector<uint8_t> row(16, 0x88);
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), 0, 0), 1);
    EXPECT_EQ(TIFFFlushData1(tif.get()), 0);

    // _TIFFReserveLargeEnoughWriteBuffer also rejects td_stripbytecount_p ==
    // NULL.
    std::vector<uint8_t> strip(64, 0x99);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, strip.data(), 64),
              static_cast<tmsize_t>(-1));
  }
}

}  // namespace
}  // namespace libtiff
