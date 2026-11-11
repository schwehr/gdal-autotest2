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

// Clean-room unit tests for libtiff PixarLog codec (libtiff/tif_pixarlog.c).
//
// google3 caveats and upstream implementation notes:
//   - PIXARLOG_SUPPORT is backed by third_party/rust/zlib_rs. Compressed byte
//     sizes are not asserted because zlib compression output is treated as
//     opaque.
//   - PixarLogClose covertly resets td_bitspersample to 8 and td_sampleformat
//     to SAMPLEFORMAT_UINT once PLSTATE_INIT is set; tests explicitly set
//     TIFFTAG_PIXARLOGDATAFMT on read when decoding 16-bit/float/11-bit/12-bit.
//   - Upstream tif_pixarlog.c relies on td_rowsperstrip in PixarLogSetupEncode
//     and does not clamp or swap sp->tbuf/td_rowsperstrip for tiled encodes or
//     big-endian encodes; tiled decode and big-endian decode byte swapping are
//     exercised via synthetic IFDs and valid zlib streams.
//   - Upstream PixarLogDecode rejects stride == 3 with PIXARLOGDATAFMT_8BITABGR
//     for any valid output buffer because nsamples is set to occ while
//     max_nsamples is computed as 3/4 of occ; tests verify those error guards
//     without expecting unreachable 3-channel ABGR expansion.

#include <stdio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"

namespace libtiff {
namespace {

constexpr int kPixarLogDataFmtUnknown = -1;
constexpr int kZNoCompression = 0;
constexpr int kZBestSpeed = 1;
constexpr int kZBestCompression = 9;
constexpr int kZDefaultCompression = -1;

constexpr float kHotHighlightTokenMaxF = 24.4f;

// Builds a valid uncompressed DEFLATE (RFC 1950 + RFC 1951 stored block) zlib
// wrapper around `payload`. Setting `is_final` to false omits the trailing
// Adler-32 checksum and BFINAL bit so inflate() stalls in Z_BUF_ERROR when no
// further input arrives.
std::vector<uint8_t> MakeStoredZlibStream(absl::Span<const uint8_t> payload,
                                          bool is_final) {
  std::vector<uint8_t> out;
  const uint16_t len = static_cast<uint16_t>(payload.size());
  const uint16_t nlen = static_cast<uint16_t>(~len);

  // Zlib header: CM=8, CINFO=7, FLEVEL=0, valid FCHECK (0x7801 % 31 == 0).
  out.push_back(0x78);
  out.push_back(0x01);

  // Deflate stored block header.
  out.push_back(is_final ? 0x01 : 0x00);
  out.push_back(static_cast<uint8_t>(len & 0xFFU));
  out.push_back(static_cast<uint8_t>((len >> 8) & 0xFFU));
  out.push_back(static_cast<uint8_t>(nlen & 0xFFU));
  out.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFFU));
  out.insert(out.end(), payload.begin(), payload.end());

  if (is_final) {
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    for (uint8_t b : payload) {
      s1 = (s1 + b) % 65521U;
      s2 = (s2 + s1) % 65521U;
    }
    const uint32_t adler = (s2 << 16) | s1;
    out.push_back(static_cast<uint8_t>((adler >> 24) & 0xFFU));
    out.push_back(static_cast<uint8_t>((adler >> 16) & 0xFFU));
    out.push_back(static_cast<uint8_t>((adler >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>(adler & 0xFFU));
  }
  return out;
}

std::vector<uint8_t> EncodeUint16Tokens(const std::vector<uint16_t>& tokens,
                                        bool big_endian) {
  std::vector<uint8_t> bytes;
  bytes.reserve(tokens.size() * 2);
  for (uint16_t v : tokens) {
    if (big_endian) {
      bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
      bytes.push_back(static_cast<uint8_t>(v & 0xFFU));
    } else {
      bytes.push_back(static_cast<uint8_t>(v & 0xFFU));
      bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
    }
  }
  return bytes;
}

UniqueTiff OpenWithMemLimit(MemoryTiffStream& stream, const char* mode,
                            tmsize_t max_cumulated = (1 << 24)) {
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  stream.diagnostics().Install(opts.get());
  if (max_cumulated > 0) {
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), max_cumulated);
  }
  return stream.Open(mode, opts.get());
}

void SetCommonStrippedTags(TIFF* tif, uint32_t width, uint32_t height,
                           uint32_t rows_per_strip, uint16_t spp, uint16_t bps,
                           uint16_t planar_config, uint16_t sample_format) {
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, spp), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar_config), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, sample_format), 1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                         spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK),
            1);
  ASSERT_EQ(TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_PIXARLOG), 1);
}

std::vector<uint8_t> BuildRawPixarLogTiff(
    bool big_endian, bool big_tiff, uint32_t width, uint32_t height,
    uint32_t rows_per_strip, uint16_t spp, uint16_t bps, uint16_t sample_format,
    absl::Span<const uint8_t> strip_payload,
    uint16_t predictor = PREDICTOR_NONE) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const uint64_t strip_off = builder.AppendBlob(strip_payload);
  std::vector<RawIfdEntry> entries = {
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, bps),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PIXARLOG),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC,
                        spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK),
      big_tiff ? builder.MakeLong8(TIFFTAG_STRIPOFFSETS, strip_off)
               : builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                                  static_cast<uint32_t>(strip_off)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, spp),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      big_tiff
          ? builder.MakeLong8(TIFFTAG_STRIPBYTECOUNTS, strip_payload.size())
          : builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                             static_cast<uint32_t>(strip_payload.size())),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeShort(TIFFTAG_SAMPLEFORMAT, sample_format),
  };
  if (predictor != PREDICTOR_NONE) {
    entries.push_back(builder.MakeShort(TIFFTAG_PREDICTOR, predictor));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

TEST(TifPixarLogTest, CodecRegistrationAndMaxCompressionRatio) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_PIXARLOG), 1);
  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_PIXARLOG);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_PIXARLOG);
  EXPECT_STREQ(codec->name, "PixarLog");

  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_PIXARLOG),
            1);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 4128ULL);

  // Switching compression triggers PixarLogCleanup with PLSTATE_INIT == 0.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 1ULL);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifPixarLogTest, PseudoTagGetSetAndDefaults) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 16U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 16U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 4U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_PIXARLOG),
            1);

  int quality = 0;
  int datafmt = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PIXARLOGQUALITY, &quality), 1);
  EXPECT_EQ(quality, kZDefaultCompression);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PIXARLOGDATAFMT, &datafmt), 1);
  EXPECT_EQ(datafmt, kPixarLogDataFmtUnknown);

  // Delegate standard and predictor tags through vsetparent/vgetparent.
  uint32_t got_width = 0;
  uint16_t predictor = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &got_width), 1);
  EXPECT_EQ(got_width, 16U);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL),
            1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PREDICTOR, &predictor), 1);
  EXPECT_EQ(predictor, PREDICTOR_HORIZONTAL);
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PREDICTOR, PREDICTOR_NONE), 1);

  struct FmtExpectation {
    int fmt;
    uint16_t expected_bps;
    uint16_t expected_samplefmt;
  };
  constexpr auto kFmtCases = std::to_array<FmtExpectation>({
      {PIXARLOGDATAFMT_8BIT, 8, SAMPLEFORMAT_UINT},
      {PIXARLOGDATAFMT_8BITABGR, 8, SAMPLEFORMAT_UINT},
      {PIXARLOGDATAFMT_11BITLOG, 16, SAMPLEFORMAT_UINT},
      {PIXARLOGDATAFMT_12BITPICIO, 16, SAMPLEFORMAT_INT},
      {PIXARLOGDATAFMT_16BIT, 16, SAMPLEFORMAT_UINT},
      {PIXARLOGDATAFMT_FLOAT, 32, SAMPLEFORMAT_IEEEFP},
  });

  for (const auto& c : kFmtCases) {
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PIXARLOGDATAFMT, c.fmt), 1);
    int got_fmt = -1;
    uint16_t bps = 0;
    uint16_t samplefmt = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PIXARLOGDATAFMT, &got_fmt), 1);
    EXPECT_EQ(got_fmt, c.fmt);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
    EXPECT_EQ(bps, c.expected_bps);
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_SAMPLEFORMAT, &samplefmt), 1);
    EXPECT_EQ(samplefmt, c.expected_samplefmt);
  }

  // Unknown enum falls through switch cleanly without clobbering bps.
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PIXARLOGDATAFMT, 99), 1);
  uint16_t bps_after_unknown = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps_after_unknown),
            1);
  EXPECT_EQ(bps_after_unknown, 32);

  // Restore 8BIT and set quality before PLSTATE_INIT.
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PIXARLOGDATAFMT, PIXARLOGDATAFMT_8BIT),
      1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PIXARLOGQUALITY, kZBestCompression),
            1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PIXARLOGQUALITY, &quality), 1);
  EXPECT_EQ(quality, kZBestCompression);

  // Trigger PLSTATE_INIT via TIFFWriteScanline. Because pixarlogFields marks
  // TIFFTAG_PIXARLOGQUALITY with field_oktochange == FALSE and TIFF_BEENWRITING
  // remains active (even across TIFFCheckpointDirectory), OkToChangeTag rejects
  // changing quality mid-stream while leaving the existing value intact.
  std::vector<uint8_t> row(16 * 3, 42);
  for (uint32_t r = 0; r < 4; ++r) {
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), r, 0), 1);
  }
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PIXARLOGQUALITY, kZBestSpeed), 0);
  EXPECT_GE(stream.diagnostics().error_count(), 1U);
  stream.diagnostics().Clear();
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PIXARLOGQUALITY, &quality), 1);
  EXPECT_EQ(quality, kZBestCompression);

  for (uint32_t r = 4; r < 16; ++r) {
    ASSERT_EQ(TIFFWriteScanline(tif.get(), row.data(), r, 0), 1);
  }
  tif.reset();
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  // Exercise isTiled(tif) branch in PixarLogVSetField.
  MemoryTiffStream tiled_stream;
  UniqueTiff tiled_tif = tiled_stream.Open("w");
  ASSERT_NE(tiled_tif, nullptr);
  ASSERT_EQ(TIFFSetField(tiled_tif.get(), TIFFTAG_IMAGEWIDTH, 16U), 1);
  ASSERT_EQ(TIFFSetField(tiled_tif.get(), TIFFTAG_IMAGELENGTH, 16U), 1);
  ASSERT_EQ(TIFFSetField(tiled_tif.get(), TIFFTAG_TILEWIDTH, 16U), 1);
  ASSERT_EQ(TIFFSetField(tiled_tif.get(), TIFFTAG_TILELENGTH, 16U), 1);
  ASSERT_EQ(TIFFSetField(tiled_tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
            1);
  ASSERT_EQ(
      TIFFSetField(tiled_tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_PIXARLOG),
      1);
  EXPECT_EQ(TIFFSetField(tiled_tif.get(), TIFFTAG_PIXARLOGDATAFMT,
                         PIXARLOGDATAFMT_16BIT),
            1);
  EXPECT_EQ(tiled_stream.diagnostics().error_count(), 0U);
}

TEST(TifPixarLogTest, CloseOverridesDirectoryTagsOnlyWhenInitialized) {
  // Closing without writing leaves BITSPERSAMPLE unchanged (no PLSTATE_INIT).
  {
    MemoryTiffStream uninit_stream;
    UniqueTiff tif = uninit_stream.Open("w");
    ASSERT_NE(tif, nullptr);
    SetCommonStrippedTags(tif.get(), 4, 2, 2, 1, 16, PLANARCONFIG_CONTIG,
                          SAMPLEFORMAT_UINT);
    // Write nothing; close resets via PixarLogClose with PLSTATE_INIT == 0.
    tif.reset();
    EXPECT_EQ(uninit_stream.diagnostics().error_count(), 0U);
  }

  // Closing after writing 16-bit data rewrites header tags to 8-bit UINT.
  MemoryTiffStream stream;
  {
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    SetCommonStrippedTags(tif.get(), 4, 2, 2, 1, 16, PLANARCONFIG_CONTIG,
                          SAMPLEFORMAT_UINT);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PIXARLOGDATAFMT, PIXARLOGDATAFMT_16BIT),
        1);
    std::vector<uint16_t> pixels = {0,     16384, 32768, 65535,
                                    65535, 32768, 16384, 0};
    ASSERT_NE(TIFFWriteEncodedStrip(tif.get(), 0, pixels.data(),
                                    static_cast<tmsize_t>(pixels.size() * 2)),
              static_cast<tmsize_t>(-1));
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  uint16_t bps = 0;
  uint16_t samplefmt = 0;
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
  EXPECT_EQ(bps, 8);
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_SAMPLEFORMAT, &samplefmt), 1);
  EXPECT_EQ(samplefmt, SAMPLEFORMAT_UINT);

  // Verify setting TIFFTAG_PIXARLOGQUALITY in read mode leaves state valid.
  EXPECT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGQUALITY, kZBestSpeed),
            1);

  std::vector<uint8_t> out8(8, 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out8.data(), 8), 8);
  EXPECT_EQ(out8[0], 0);
  EXPECT_EQ(out8[3], 255);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifPixarLogTest, EightBitLosslessRoundTripAllStrides) {
  struct LayoutCase {
    uint16_t spp;
    uint16_t planar;
  };
  constexpr auto kLayouts = std::to_array<LayoutCase>({
      {1, PLANARCONFIG_CONTIG},
      {2, PLANARCONFIG_CONTIG},
      {3, PLANARCONFIG_CONTIG},
      {4, PLANARCONFIG_CONTIG},
      {3, PLANARCONFIG_SEPARATE},
  });

  constexpr uint32_t kWidth = 6;
  constexpr uint32_t kHeight = 4;
  constexpr uint32_t kRowsPerStrip = 2;

  for (const auto& layout : kLayouts) {
    SCOPED_TRACE(testing::Message()
                 << "spp=" << layout.spp << " planar=" << layout.planar);
    const uint16_t planes =
        layout.planar == PLANARCONFIG_SEPARATE ? layout.spp : 1;
    const uint16_t samples_per_plane =
        layout.planar == PLANARCONFIG_SEPARATE ? 1 : layout.spp;
    const size_t row_bytes = kWidth * samples_per_plane;

    std::vector<std::vector<uint8_t>> plane_data(planes);
    for (uint16_t p = 0; p < planes; ++p) {
      plane_data[p].resize(kHeight * row_bytes);
      for (size_t i = 0; i < plane_data[p].size(); ++i) {
        plane_data[p][i] =
            static_cast<uint8_t>((i * 53U + p * 71U + (i & 1U) * 199U) & 0xFFU);
      }
      plane_data[p][0] = 0;
      plane_data[p][1] = 255;
    }

    // Round-trip via TIFFWriteScanline / TIFFReadScanline.
    {
      MemoryTiffStream stream;
      {
        UniqueTiff writer = stream.Open("w");
        ASSERT_NE(writer, nullptr);
        SetCommonStrippedTags(writer.get(), kWidth, kHeight, kRowsPerStrip,
                              layout.spp, 8, layout.planar, SAMPLEFORMAT_UINT);
        for (uint16_t p = 0; p < planes; ++p) {
          for (uint32_t r = 0; r < kHeight; ++r) {
            std::vector<uint8_t> row_buf(
                plane_data[p].begin() + r * row_bytes,
                plane_data[p].begin() + (r + 1) * row_bytes);
            ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), r, p), 1);
          }
        }
      }
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      for (uint16_t p = 0; p < planes; ++p) {
        for (uint32_t r = 0; r < kHeight; ++r) {
          std::vector<uint8_t> got_row(row_bytes, 0);
          ASSERT_EQ(TIFFReadScanline(reader.get(), got_row.data(), r, p), 1);
          EXPECT_EQ(got_row, std::vector<uint8_t>(
                                 plane_data[p].begin() + r * row_bytes,
                                 plane_data[p].begin() + (r + 1) * row_bytes));
        }
      }
      EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    }

    // Round-trip via TIFFWriteEncodedStrip / TIFFReadEncodedStrip.
    {
      MemoryTiffStream stream;
      const tmsize_t strip_bytes =
          static_cast<tmsize_t>(kRowsPerStrip * row_bytes);
      {
        UniqueTiff writer = stream.Open("w");
        ASSERT_NE(writer, nullptr);
        SetCommonStrippedTags(writer.get(), kWidth, kHeight, kRowsPerStrip,
                              layout.spp, 8, layout.planar, SAMPLEFORMAT_UINT);
        for (uint32_t s = 0; s < TIFFNumberOfStrips(writer.get()); ++s) {
          const uint16_t p = static_cast<uint16_t>(s / 2);
          const uint32_t strip_in_plane = s % 2;
          std::vector<uint8_t> sbuf(
              plane_data[p].begin() + strip_in_plane * strip_bytes,
              plane_data[p].begin() + (strip_in_plane + 1) * strip_bytes);
          ASSERT_EQ(
              TIFFWriteEncodedStrip(writer.get(), s, sbuf.data(), strip_bytes),
              strip_bytes);
        }
      }
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      for (uint32_t s = 0; s < TIFFNumberOfStrips(reader.get()); ++s) {
        const uint16_t p = static_cast<uint16_t>(s / 2);
        const uint32_t strip_in_plane = s % 2;
        std::vector<uint8_t> got(strip_bytes, 0);
        ASSERT_EQ(
            TIFFReadEncodedStrip(reader.get(), s, got.data(), strip_bytes),
            strip_bytes);
        EXPECT_EQ(got, std::vector<uint8_t>(
                           plane_data[p].begin() + strip_in_plane * strip_bytes,
                           plane_data[p].begin() +
                               (strip_in_plane + 1) * strip_bytes));
      }
      EXPECT_EQ(stream.diagnostics().error_count(), 0U);
    }
  }
}

TEST(TifPixarLogTest, SixteenBitRoundTripAllStrides) {
  struct LayoutCase {
    uint16_t spp;
    uint16_t planar;
    bool set_explicit_fmt;
  };
  constexpr auto kLayouts = std::to_array<LayoutCase>({
      {1, PLANARCONFIG_CONTIG, true},
      {2, PLANARCONFIG_CONTIG, true},
      {3, PLANARCONFIG_CONTIG, false},  // Tests PixarLogGuessDataFmt -> 16BIT.
      {4, PLANARCONFIG_CONTIG, true},
      {3, PLANARCONFIG_SEPARATE, true},
  });

  constexpr uint32_t kWidth = 6;
  constexpr uint32_t kHeight = 3;

  for (const auto& layout : kLayouts) {
    SCOPED_TRACE(testing::Message()
                 << "spp=" << layout.spp << " planar=" << layout.planar);
    const uint16_t planes =
        layout.planar == PLANARCONFIG_SEPARATE ? layout.spp : 1;
    const uint16_t samples_per_plane =
        layout.planar == PLANARCONFIG_SEPARATE ? 1 : layout.spp;
    const size_t strip_samples = kWidth * kHeight * samples_per_plane;

    std::vector<std::vector<uint16_t>> input_planes(planes);
    for (uint16_t p = 0; p < planes; ++p) {
      input_planes[p].resize(strip_samples);
      for (size_t i = 0; i < strip_samples; ++i) {
        input_planes[p][i] =
            static_cast<uint16_t>((i * 4099U + p * 7919U) % 65536U);
      }
      input_planes[p][0] = 0;
      input_planes[p][1] = 65535;
    }

    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      SetCommonStrippedTags(writer.get(), kWidth, kHeight, kHeight, layout.spp,
                            16, layout.planar, SAMPLEFORMAT_UINT);
      if (layout.set_explicit_fmt) {
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PIXARLOGDATAFMT,
                               PIXARLOGDATAFMT_16BIT),
                  1);
      }
      for (uint16_t p = 0; p < planes; ++p) {
        std::vector<uint16_t> copy = input_planes[p];
        const tmsize_t byte_len = static_cast<tmsize_t>(copy.size() * 2);
        ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), p, copy.data(), byte_len),
                  byte_len);
      }
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGDATAFMT,
                           PIXARLOGDATAFMT_16BIT),
              1);
    for (uint16_t p = 0; p < planes; ++p) {
      std::vector<uint16_t> got(strip_samples, 0);
      const tmsize_t byte_len = static_cast<tmsize_t>(got.size() * 2);
      ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), p, got.data(), byte_len),
                byte_len);
      EXPECT_EQ(got[0], 0);
      EXPECT_EQ(got[1], 65535);
      for (size_t i = 0; i < strip_samples; ++i) {
        const int tol =
            std::max(12, static_cast<int>(input_planes[p][i]) / 120);
        EXPECT_NEAR(got[i], input_planes[p][i], tol) << "i=" << i;
      }
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifPixarLogTest, FloatRoundTripAndCrossFormatDecodeAllStrides) {
  struct LayoutCase {
    uint16_t spp;
    uint16_t planar;
    bool set_explicit_fmt;
  };
  constexpr auto kLayouts = std::to_array<LayoutCase>({
      {1, PLANARCONFIG_CONTIG, true},
      {2, PLANARCONFIG_CONTIG, true},
      {3, PLANARCONFIG_CONTIG, false},  // Tests PixarLogGuessDataFmt -> FLOAT.
      {4, PLANARCONFIG_CONTIG, true},
      {3, PLANARCONFIG_SEPARATE, true},
  });

  // Exercises all CLAMP(v) regions: <0, [0, 2.0), [2.0, 24.2], and >24.2.
  constexpr auto kFloatPattern = std::to_array<float>({
      -0.5f,
      0.0f,
      0.005f,
      0.25f,
      0.5f,
      1.0f,
      1.75f,
      2.0f,
      5.5f,
      18.0f,
      24.2f,
      30.0f,
  });

  constexpr uint32_t kWidth = 6;
  constexpr uint32_t kHeight = 2;

  for (const auto& layout : kLayouts) {
    SCOPED_TRACE(testing::Message()
                 << "spp=" << layout.spp << " planar=" << layout.planar);
    const uint16_t planes =
        layout.planar == PLANARCONFIG_SEPARATE ? layout.spp : 1;
    const uint16_t samples_per_plane =
        layout.planar == PLANARCONFIG_SEPARATE ? 1 : layout.spp;
    const size_t strip_samples = kWidth * kHeight * samples_per_plane;

    std::vector<std::vector<float>> input_planes(planes);
    for (uint16_t p = 0; p < planes; ++p) {
      input_planes[p].resize(strip_samples);
      for (size_t i = 0; i < strip_samples; ++i) {
        input_planes[p][i] = kFloatPattern[(i + p * 3U) % kFloatPattern.size()];
      }
    }

    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      SetCommonStrippedTags(writer.get(), kWidth, kHeight, kHeight, layout.spp,
                            32, layout.planar, SAMPLEFORMAT_IEEEFP);
      if (layout.set_explicit_fmt) {
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PIXARLOGDATAFMT,
                               PIXARLOGDATAFMT_FLOAT),
                  1);
      }
      for (uint16_t p = 0; p < planes; ++p) {
        std::vector<float> copy = input_planes[p];
        const tmsize_t byte_len =
            static_cast<tmsize_t>(copy.size() * sizeof(float));
        ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), p, copy.data(), byte_len),
                  byte_len);
      }
    }

    // 1. Decode back as FLOAT.
    {
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGDATAFMT,
                             PIXARLOGDATAFMT_FLOAT),
                1);
      for (uint16_t p = 0; p < planes; ++p) {
        std::vector<float> got(strip_samples, -1.0f);
        const tmsize_t byte_len =
            static_cast<tmsize_t>(got.size() * sizeof(float));
        ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), p, got.data(), byte_len),
                  byte_len);
        for (size_t i = 0; i < strip_samples; ++i) {
          const float src = input_planes[p][i];
          if (src <= 0.0f) {
            EXPECT_FLOAT_EQ(got[i], 0.0f);
          } else if (src > 24.2f) {
            EXPECT_GT(got[i], 24.0f);
            EXPECT_LE(got[i], kHotHighlightTokenMaxF);
          } else {
            EXPECT_NEAR(got[i], src, std::max(0.002f, src * 0.015f));
          }
        }
      }
    }

    // 2. Decode as PIXARLOGDATAFMT_12BITPICIO (verifies SCALE12 &
    // CLAMP12=3071).
    {
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGDATAFMT,
                             PIXARLOGDATAFMT_12BITPICIO),
                1);
      for (uint16_t p = 0; p < planes; ++p) {
        std::vector<int16_t> got12(strip_samples, -1);
        const tmsize_t byte_len =
            static_cast<tmsize_t>(got12.size() * sizeof(int16_t));
        ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), p, got12.data(), byte_len),
                  byte_len);
        for (size_t i = 0; i < strip_samples; ++i) {
          EXPECT_GE(got12[i], 0);
          EXPECT_LE(got12[i], 3071);
          if (input_planes[p][i] <= 0.0f) {
            EXPECT_EQ(got12[i], 0);
          } else if (input_planes[p][i] >= 2.0f) {
            EXPECT_EQ(got12[i], 3071);
          }
        }
      }
    }

    // 3. Decode as PIXARLOGDATAFMT_11BITLOG.
    {
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGDATAFMT,
                             PIXARLOGDATAFMT_11BITLOG),
                1);
      for (uint16_t p = 0; p < planes; ++p) {
        std::vector<uint16_t> got11(strip_samples, 0xFFFFU);
        const tmsize_t byte_len =
            static_cast<tmsize_t>(got11.size() * sizeof(uint16_t));
        ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), p, got11.data(), byte_len),
                  byte_len);
        for (size_t i = 0; i < strip_samples; ++i) {
          EXPECT_LE(got11[i], 2047U);
          if (input_planes[p][i] <= 0.0f) {
            EXPECT_EQ(got11[i], 0U);
          } else if (std::abs(input_planes[p][i] - 1.0f) < 1e-6f) {
            EXPECT_EQ(got11[i], 1250U);
          } else if (input_planes[p][i] > 24.2f) {
            EXPECT_EQ(got11[i], 2047U);
          }
        }
      }
    }

    // 4. Decode as PIXARLOGDATAFMT_8BIT and PIXARLOGDATAFMT_8BITABGR (stride !=
    // 3).
    {
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGDATAFMT,
                             PIXARLOGDATAFMT_8BIT),
                1);
      std::vector<uint8_t> got8(strip_samples, 0);
      ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, got8.data(),
                                     static_cast<tmsize_t>(got8.size())),
                static_cast<tmsize_t>(got8.size()));
      for (size_t i = 0; i < strip_samples; ++i) {
        if (input_planes[0][i] <= 0.0f) {
          EXPECT_EQ(got8[i], 0);
        } else if (input_planes[0][i] >= 1.0f) {
          EXPECT_EQ(got8[i], 255);
        }
      }

      if (samples_per_plane != 3) {
        UniqueTiff abgr_reader = stream.Open("r");
        ASSERT_NE(abgr_reader, nullptr);
        ASSERT_EQ(TIFFSetField(abgr_reader.get(), TIFFTAG_PIXARLOGDATAFMT,
                               PIXARLOGDATAFMT_8BITABGR),
                  1);
        std::vector<uint8_t> got_abgr(strip_samples, 0);
        ASSERT_EQ(TIFFReadEncodedStrip(abgr_reader.get(), 0, got_abgr.data(),
                                       static_cast<tmsize_t>(got_abgr.size())),
                  static_cast<tmsize_t>(got_abgr.size()));
        if (samples_per_plane == 4) {
          for (size_t px = 0; px < kWidth * kHeight; ++px) {
            EXPECT_EQ(got_abgr[px * 4 + 0], got8[px * 4 + 3]);
            EXPECT_EQ(got_abgr[px * 4 + 1], got8[px * 4 + 2]);
            EXPECT_EQ(got_abgr[px * 4 + 2], got8[px * 4 + 1]);
            EXPECT_EQ(got_abgr[px * 4 + 3], got8[px * 4 + 0]);
          }
        } else {
          EXPECT_EQ(got_abgr, got8);
        }
      }
    }

    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifPixarLogTest, GuessDataFmtFromHeaderAndRejectUnsupportedCombinations) {
  // Valid guessed decode formats without setting TIFFTAG_PIXARLOGDATAFMT:
  // (11, UINT) -> 11BITLOG; (12, INT) -> 12BITPICIO; (8, VOID) -> 8BIT;
  // (16, VOID) -> 16BIT.
  constexpr uint16_t kToken1250 = 1250;  // Linear 1.0 token.
  const std::vector<uint16_t> tokens = {0, kToken1250};
  const std::vector<uint8_t> zlib_payload =
      MakeStoredZlibStream(EncodeUint16Tokens(tokens, false), true);

  {
    const std::vector<uint8_t> tiff11 = BuildRawPixarLogTiff(
        false, false, 2, 1, 1, 1, 11, SAMPLEFORMAT_UINT, zlib_payload);
    MemoryTiffStream s11(tiff11);
    UniqueTiff tif = s11.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> user_buf = zlib_payload;
    std::array<uint16_t, 2> out11 = {};
    // Note TIFFScanlineSize for 11-bit raw tags is 3 bytes unless read as 16;
    // PIXARLOGDATAFMT_11BITLOG via explicit or guessed fmt expects 4 bytes.
    ASSERT_EQ(TIFFReadFromUserBuffer(tif.get(), 0, user_buf.data(),
                                     static_cast<tmsize_t>(user_buf.size()),
                                     out11.data(), 4),
              1);
    EXPECT_EQ(out11[0], 0U);
    EXPECT_EQ(out11[1], 1250U);
    EXPECT_EQ(s11.diagnostics().error_count(), 0U);
  }

  {
    const std::vector<uint8_t> tiff12 = BuildRawPixarLogTiff(
        false, false, 2, 1, 1, 1, 12, SAMPLEFORMAT_INT, zlib_payload);
    MemoryTiffStream s12(tiff12);
    UniqueTiff tif = s12.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> user_buf = zlib_payload;
    std::array<int16_t, 2> out12 = {};
    ASSERT_EQ(TIFFReadFromUserBuffer(tif.get(), 0, user_buf.data(),
                                     static_cast<tmsize_t>(user_buf.size()),
                                     out12.data(), 4),
              1);
    EXPECT_EQ(out12[0], 0);
    EXPECT_EQ(out12[1], 2048);
    EXPECT_EQ(s12.diagnostics().error_count(), 0U);
  }

  {
    const std::vector<uint8_t> tiff8void = BuildRawPixarLogTiff(
        false, false, 2, 1, 1, 1, 8, SAMPLEFORMAT_VOID, zlib_payload);
    MemoryTiffStream s8(tiff8void);
    UniqueTiff tif = s8.Open("r");
    ASSERT_NE(tif, nullptr);
    std::array<uint8_t, 2> out8 = {};
    ASSERT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out8.data(), 2), 2);
    EXPECT_EQ(out8[0], 0);
    EXPECT_EQ(out8[1], 255);
  }

  {
    const std::vector<uint8_t> tiff16void = BuildRawPixarLogTiff(
        false, false, 2, 1, 1, 1, 16, SAMPLEFORMAT_VOID, zlib_payload);
    MemoryTiffStream s16(tiff16void);
    UniqueTiff tif = s16.Open("r");
    ASSERT_NE(tif, nullptr);
    std::array<uint16_t, 2> out16 = {};
    ASSERT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out16.data(), 4), 4);
    EXPECT_EQ(out16[0], 0U);
    EXPECT_EQ(out16[1], 65535U);
  }

  // Unsupported (bps, sampleformat) pairs fail in SetupEncode and SetupDecode.
  struct BadPair {
    uint16_t bps;
    uint16_t samplefmt;
  };
  constexpr auto kBadPairs = std::to_array<BadPair>({
      {4, SAMPLEFORMAT_UINT},
      {8, SAMPLEFORMAT_INT},
      {11, SAMPLEFORMAT_INT},
      {12, SAMPLEFORMAT_UINT},
      {16, SAMPLEFORMAT_INT},
      {32, SAMPLEFORMAT_UINT},
  });

  for (const auto& bad : kBadPairs) {
    SCOPED_TRACE(testing::Message()
                 << "bps=" << bad.bps << " samplefmt=" << bad.samplefmt);
    {
      MemoryTiffStream stream;
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      SetCommonStrippedTags(writer.get(), 4, 1, 1, 1, bad.bps,
                            PLANARCONFIG_CONTIG, bad.samplefmt);
      std::vector<uint8_t> buf(16, 0);
      EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(), 4),
                static_cast<tmsize_t>(-1));
      EXPECT_GE(stream.diagnostics().error_count(), 1U);
    }
    {
      MemoryTiffStream stream(BuildRawPixarLogTiff(
          false, false, 2, 1, 1, 1, bad.bps, bad.samplefmt, zlib_payload));
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      std::vector<uint8_t> buf(16, 0);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, buf.data(), -1),
                static_cast<tmsize_t>(-1));
      EXPECT_GE(stream.diagnostics().error_count(), 1U);
    }
  }

  // Explicit 11BITLOG / 12BITPICIO / 8BITABGR / unknown(99) fail on encode.
  constexpr auto kUnsupportedEncodeFmts = std::to_array<int>({
      PIXARLOGDATAFMT_11BITLOG,
      PIXARLOGDATAFMT_12BITPICIO,
      PIXARLOGDATAFMT_8BITABGR,
      99,
  });
  for (int fmt : kUnsupportedEncodeFmts) {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    SetCommonStrippedTags(writer.get(), 4, 1, 1, 1, 8, PLANARCONFIG_CONTIG,
                          SAMPLEFORMAT_UINT);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PIXARLOGDATAFMT, fmt), 1);
    std::vector<uint8_t> buf(8, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(), 8),
              static_cast<tmsize_t>(-1));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // Invalid user_datafmt (99) fails cleanly in PixarLogDecode outer switch.
  {
    MemoryTiffStream stream(BuildRawPixarLogTiff(
        false, false, 2, 1, 1, 1, 8, SAMPLEFORMAT_UINT, zlib_payload));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGDATAFMT, 99), 1);
    std::array<uint8_t, 2> out = {0xAA, 0xAA};
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 2),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 0);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }
}

TEST(TifPixarLogTest, EightBitAbgrStride3BufferValidationGuards) {
  auto encode_rgb_8bit = [](MemoryTiffStream& stream, uint32_t w, uint32_t h) {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    SetCommonStrippedTags(writer.get(), w, h, h, 3, 8, PLANARCONFIG_CONTIG,
                          SAMPLEFORMAT_UINT);
    std::vector<uint8_t> pixels(w * h * 3, 128);
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, pixels.data(),
                                    static_cast<tmsize_t>(pixels.size())),
              static_cast<tmsize_t>(pixels.size()));
  };

  // 1. Scanline buffer too small: occ = 12 < required = 16 -> error, zeroes
  // buf.
  {
    MemoryTiffStream stream;
    encode_rgb_8bit(stream, 4, 2);
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGDATAFMT,
                           PIXARLOGDATAFMT_8BITABGR),
              1);
    std::vector<uint8_t> row(12, 0x77);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row.data(), 0, 0), -1);
    EXPECT_THAT(row, testing::Each(0));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 2. Fractional scanline: occ = 24, required = 16 -> occ % 16 == 8.
  {
    MemoryTiffStream stream;
    encode_rgb_8bit(stream, 4, 2);
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGDATAFMT,
                           PIXARLOGDATAFMT_8BITABGR),
              1);
    std::vector<uint8_t> strip(24, 0x77);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(), 24),
              static_cast<tmsize_t>(-1));
    EXPECT_THAT(strip, testing::Each(0));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 3. Expanded ABGR rows overflow occ: w=4, h=4 -> occ=48 (divisible by 16),
  //    nsamples=48 > max_nsamples=(48/16)*12=36.
  {
    MemoryTiffStream stream;
    encode_rgb_8bit(stream, 4, 4);
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGDATAFMT,
                           PIXARLOGDATAFMT_8BITABGR),
              1);
    std::vector<uint8_t> strip(48, 0x77);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, strip.data(), 48),
              static_cast<tmsize_t>(-1));
    EXPECT_THAT(strip, testing::Each(0));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }
}

TEST(TifPixarLogTest, PredictorInteractionAndPrintDir) {
  MemoryTiffStream stream;
  const std::vector<uint8_t> expected = {
      10, 20, 30, 40, 50, 60, 120, 140, 160, 200, 220, 240,
      15, 25, 35, 95, 85, 75, 130, 150, 170, 210, 230, 250,
  };
  {
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    SetCommonStrippedTags(writer.get(), 4, 2, 2, 3, 8, PLANARCONFIG_CONTIG,
                          SAMPLEFORMAT_UINT);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL), 1);
    std::vector<uint8_t> copy = expected;
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, copy.data(),
                                    static_cast<tmsize_t>(copy.size())),
              static_cast<tmsize_t>(copy.size()));
  }

  UniqueTiff reader = stream.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> decoded(expected.size(), 0);
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                 static_cast<tmsize_t>(decoded.size())),
            static_cast<tmsize_t>(decoded.size()));
  EXPECT_EQ(decoded, expected);

  // Exercise PredictorPrintDir hooked during TIFFInitPixarLog -> PredictorInit.
  char* mem_buf = nullptr;
  size_t mem_size = 0;
  FILE* fp = open_memstream(&mem_buf, &mem_size);
  ASSERT_NE(fp, nullptr);
  absl::Cleanup close_mem = [&] {
    if (fp != nullptr) {
      std::fclose(fp);
    }
    std::free(mem_buf);
  };
  TIFFPrintDirectory(reader.get(), fp, TIFFPRINT_NONE);
  std::fflush(fp);
  std::string dir_dump(mem_buf, mem_size);
  EXPECT_THAT(dir_dump, testing::HasSubstr("PixarLog"));
  EXPECT_THAT(dir_dump, testing::HasSubstr("horizontal differencing"));

  // Re-entry to PixarLogSetupDecode when PredictorSetup fails after
  // PixarLogSetupDecode succeeds hits `if (sp->state & PLSTATE_INIT) return
  // 1;`.
  const std::vector<uint8_t> payload =
      MakeStoredZlibStream(EncodeUint16Tokens({0, 0}, false), true);
  MemoryTiffStream bad_pred_stream(BuildRawPixarLogTiff(
      false, false, 2, 1, 1, 1, 8, SAMPLEFORMAT_UINT, payload, 99));
  UniqueTiff bad_pred_tif = bad_pred_stream.Open("r");
  ASSERT_NE(bad_pred_tif, nullptr);
  std::array<uint8_t, 2> row = {};
  EXPECT_EQ(TIFFReadScanline(bad_pred_tif.get(), row.data(), 0, 0), -1);
  EXPECT_EQ(TIFFReadScanline(bad_pred_tif.get(), row.data(), 0, 0), -1);
  EXPECT_GE(bad_pred_stream.diagnostics().error_count(), 2U);
}

TEST(TifPixarLogTest, BigTiffAndTiledDecodeSupport) {
  // BigTIFF write + read round-trip.
  {
    MemoryTiffStream stream;
    const std::vector<float> expected = {
        0.0f, 0.25f, 0.5f, 1.0f, 1.5f, 3.0f, 10.0f, 20.0f,
        0.1f, 0.2f,  0.4f, 0.8f, 1.6f, 3.2f, 6.4f,  12.8f,
        0.0f, 0.5f,  1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 24.0f,
    };
    {
      UniqueTiff writer = stream.Open("w8");
      ASSERT_NE(writer, nullptr);
      SetCommonStrippedTags(writer.get(), 4, 2, 1, 3, 32, PLANARCONFIG_CONTIG,
                            SAMPLEFORMAT_IEEEFP);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PIXARLOGDATAFMT,
                             PIXARLOGDATAFMT_FLOAT),
                1);
      for (uint32_t r = 0; r < 2; ++r) {
        std::vector<float> row(expected.begin() + r * 12,
                               expected.begin() + (r + 1) * 12);
        ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), r, 0), 1);
      }
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    ASSERT_EQ(TIFFSetField(reader.get(), TIFFTAG_PIXARLOGDATAFMT,
                           PIXARLOGDATAFMT_FLOAT),
              1);
    for (uint32_t r = 0; r < 2; ++r) {
      std::vector<float> row(12, 0.0f);
      ASSERT_EQ(TIFFReadScanline(reader.get(), row.data(), r, 0), 1);
      for (size_t c = 0; c < 12; ++c) {
        EXPECT_NEAR(row[c], expected[r * 12 + c],
                    std::max(0.002f, expected[r * 12 + c] * 0.015f));
      }
    }
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }

  // Craft valid tiled PixarLog TIFF (2 tiles of 16x16 in a 16x32 image) using
  // compressed strips generated cleanly by libtiff.
  constexpr uint32_t kTileW = 16;
  constexpr uint32_t kTileH = 16;
  std::vector<uint8_t> tile0(kTileW * kTileH * 3);
  std::vector<uint8_t> tile1(kTileW * kTileH * 3);
  for (size_t i = 0; i < tile0.size(); ++i) {
    tile0[i] = static_cast<uint8_t>((i * 17U) & 0xFFU);
    tile1[i] = static_cast<uint8_t>((255U - i * 11U) & 0xFFU);
  }

  std::vector<uint8_t> compressed_tile0;
  std::vector<uint8_t> compressed_tile1;
  {
    MemoryTiffStream strip_src;
    {
      UniqueTiff w = strip_src.Open("w");
      ASSERT_NE(w, nullptr);
      SetCommonStrippedTags(w.get(), kTileW, kTileH * 2, kTileH, 3, 8,
                            PLANARCONFIG_CONTIG, SAMPLEFORMAT_UINT);
      std::vector<uint8_t> c0 = tile0;
      std::vector<uint8_t> c1 = tile1;
      ASSERT_EQ(TIFFWriteEncodedStrip(w.get(), 0, c0.data(),
                                      static_cast<tmsize_t>(c0.size())),
                static_cast<tmsize_t>(c0.size()));
      ASSERT_EQ(TIFFWriteEncodedStrip(w.get(), 1, c1.data(),
                                      static_cast<tmsize_t>(c1.size())),
                static_cast<tmsize_t>(c1.size()));
    }
    UniqueTiff r = strip_src.Open("r");
    ASSERT_NE(r, nullptr);
    uint64_t* byte_counts = nullptr;
    ASSERT_EQ(TIFFGetField(r.get(), TIFFTAG_STRIPBYTECOUNTS, &byte_counts), 1);
    compressed_tile0.resize(static_cast<size_t>(byte_counts[0]));
    compressed_tile1.resize(static_cast<size_t>(byte_counts[1]));
    ASSERT_EQ(TIFFReadRawStrip(r.get(), 0, compressed_tile0.data(),
                               static_cast<tmsize_t>(byte_counts[0])),
              static_cast<tmsize_t>(byte_counts[0]));
    ASSERT_EQ(TIFFReadRawStrip(r.get(), 1, compressed_tile1.data(),
                               static_cast<tmsize_t>(byte_counts[1])),
              static_cast<tmsize_t>(byte_counts[1]));
  }

  RawTiffBuilder builder(false, false);
  const uint64_t off0 = builder.AppendBlob(compressed_tile0);
  const uint64_t off1 = builder.AppendBlob(compressed_tile1);
  const std::array<uint32_t, 2> offsets = {static_cast<uint32_t>(off0),
                                           static_cast<uint32_t>(off1)};
  const std::array<uint32_t, 2> counts = {
      static_cast<uint32_t>(compressed_tile0.size()),
      static_cast<uint32_t>(compressed_tile1.size())};
  builder.AddIfd({
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, kTileW),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, kTileH * 2),
      builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_PIXARLOG),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      builder.MakeLong(TIFFTAG_TILEWIDTH, kTileW),
      builder.MakeLong(TIFFTAG_TILELENGTH, kTileH),
      builder.MakeLongArray(TIFFTAG_TILEOFFSETS, offsets),
      builder.MakeLongArray(TIFFTAG_TILEBYTECOUNTS, counts),
      builder.MakeShort(TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT),
  });

  MemoryTiffStream tiled_stream(builder.Take());
  UniqueTiff tiled_reader = tiled_stream.Open("r");
  ASSERT_NE(tiled_reader, nullptr);
  ASSERT_TRUE(TIFFIsTiled(tiled_reader.get()));

  std::vector<uint8_t> got0(tile0.size(), 0);
  std::vector<uint8_t> got1(tile1.size(), 0);
  ASSERT_EQ(TIFFReadEncodedTile(tiled_reader.get(), 0, got0.data(),
                                static_cast<tmsize_t>(got0.size())),
            static_cast<tmsize_t>(got0.size()));
  ASSERT_EQ(TIFFReadTile(tiled_reader.get(), got1.data(), 0, kTileH, 0, 0),
            static_cast<tmsize_t>(got1.size()));
  EXPECT_EQ(got0, tile0);
  EXPECT_EQ(got1, tile1);
  EXPECT_EQ(tiled_stream.diagnostics().error_count(), 0U);
}

TEST(TifPixarLogTest, BigEndianByteSwappedDecode) {
  // Craft a Big-Endian ('M','M') TIFF whose compressed stream contains
  // big-endian 11-bit diff tokens: pixel 0 = (0,0,0), pixel 1..3 =
  // (255,255,255) via token delta +1250 (= ONE = linear 1.0) on pixel 1 and 0
  // thereafter.
  const std::vector<uint16_t> be_tokens = {
      0, 0, 0, 1250, 1250, 1250, 0, 0, 0, 0, 0, 0,
  };
  const std::vector<uint8_t> be_zlib =
      MakeStoredZlibStream(EncodeUint16Tokens(be_tokens, true), true);

  for (bool big_tiff : {false, true}) {
    SCOPED_TRACE(testing::Message() << "big_tiff=" << big_tiff);
    MemoryTiffStream stream(BuildRawPixarLogTiff(true, big_tiff, 4, 1, 1, 3, 8,
                                                 SAMPLEFORMAT_UINT, be_zlib));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(TIFFIsByteSwapped(reader.get()), 1);

    std::vector<uint8_t> got(12, 99);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, got.data(), 12), 12);
    const std::vector<uint8_t> expected = {
        0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    };
    EXPECT_EQ(got, expected);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifPixarLogTest, LargeStripBufferFlushAndWriteErrorPaths) {
  // 256x64 8-bit 1-channel strip with incompressible data & Z_NO_COMPRESSION
  // expands into 32768 raw bytes in sp->tbuf (> ~18022 tif_rawdatasize),
  // forcing TIFFFlushData1() inside PixarLogEncode() and PixarLogPostEncode().
  constexpr uint32_t kW = 256;
  constexpr uint32_t kH = 64;
  std::vector<uint8_t> noise(kW * kH);
  uint32_t rng = 0x12345678U;
  for (uint8_t& px : noise) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    px = static_cast<uint8_t>(rng & 0xFFU);
  }

  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      SetCommonStrippedTags(writer.get(), kW, kH, kH, 1, 8, PLANARCONFIG_CONTIG,
                            SAMPLEFORMAT_UINT);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PIXARLOGQUALITY, kZNoCompression),
          1);
      std::vector<uint8_t> copy = noise;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, copy.data(),
                                      static_cast<tmsize_t>(copy.size())),
                static_cast<tmsize_t>(copy.size()));
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> got(noise.size(), 0);
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, got.data(),
                                   static_cast<tmsize_t>(got.size())),
              static_cast<tmsize_t>(got.size()));
    EXPECT_EQ(got, noise);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }

  // Write flush failure inside PixarLogEncode (after header write succeeds).
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    SetCommonStrippedTags(writer.get(), kW, kH, kH, 1, 8, PLANARCONFIG_CONTIG,
                          SAMPLEFORMAT_UINT);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PIXARLOGQUALITY, kZNoCompression),
        1);
    stream.FailWritesAfter(stream.write_calls());
    std::vector<uint8_t> copy = noise;
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, copy.data(),
                                    static_cast<tmsize_t>(copy.size())),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // Write flush failure inside PixarLogPostEncode for small strip.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    SetCommonStrippedTags(writer.get(), 8, 2, 2, 1, 8, PLANARCONFIG_CONTIG,
                          SAMPLEFORMAT_UINT);
    stream.FailWritesAfter(stream.write_calls());
    std::vector<uint8_t> small_buf(16, 7);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, small_buf.data(), 16),
              static_cast<tmsize_t>(-1));
    stream.ResetFaults();
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // Oversized input triggers "Too many input bytes provided".
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    SetCommonStrippedTags(writer.get(), 4, 2, 2, 1, 8, PLANARCONFIG_CONTIG,
                          SAMPLEFORMAT_UINT);
    std::vector<uint8_t> too_big(16, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, too_big.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }
}

TEST(TifPixarLogTest, ZlibAndBufferDecodeErrorPaths) {
  // 1. Invalid quality before SetupEncode triggers deflateInit error.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    SetCommonStrippedTags(writer.get(), 4, 1, 1, 1, 8, PLANARCONFIG_CONTIG,
                          SAMPLEFORMAT_UINT);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PIXARLOGQUALITY, 99), 1);
    std::vector<uint8_t> row(4, 1);
    EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), -1);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 2. Corrupt zlib bytes trigger Z_DATA_ERROR and zero output buffer.
  {
    const std::vector<uint8_t> corrupt = {0xFF, 0xFE, 0xFD, 0xFC};
    MemoryTiffStream stream(BuildRawPixarLogTiff(false, false, 4, 1, 1, 1, 8,
                                                 SAMPLEFORMAT_UINT, corrupt));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(4, 0x55);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_THAT(out, testing::Each(0));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 3. Valid Z_STREAM_END stored block shorter than requested samples
  //    hits `sp->stream.avail_out != 0` ("Not enough data at scanline").
  {
    const std::vector<uint8_t> short_final =
        MakeStoredZlibStream(EncodeUint16Tokens({0, 0}, false), true);
    MemoryTiffStream stream(BuildRawPixarLogTiff(
        false, false, 4, 1, 1, 1, 8, SAMPLEFORMAT_UINT, short_final));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(4, 0x55);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_THAT(out, testing::Each(0));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 4. Non-final stored block truncated before Z_STREAM_END triggers
  //    Z_BUF_ERROR on second inflate call (`state != Z_OK`).
  {
    const std::vector<uint8_t> nonfinal =
        MakeStoredZlibStream(EncodeUint16Tokens({0, 0}, false), false);
    MemoryTiffStream stream(BuildRawPixarLogTiff(false, false, 4, 1, 1, 1, 8,
                                                 SAMPLEFORMAT_UINT, nonfinal));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(4, 0x55);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_THAT(out, testing::Each(0));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 5. Partial output read hits `nsamples % llen` warning and salvages whole
  // row.
  {
    MemoryTiffStream stream;
    const std::vector<uint8_t> expected = {
        10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
        11, 22, 33, 44, 55, 66, 77, 88, 99, 101, 111, 121,
    };
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      SetCommonStrippedTags(writer.get(), 4, 2, 2, 3, 8, PLANARCONFIG_CONTIG,
                            SAMPLEFORMAT_UINT);
      std::vector<uint8_t> copy = expected;
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, copy.data(), 24), 24);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> partial(18, 0xCC);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, partial.data(), 18), 18);
    EXPECT_GE(stream.diagnostics().warning_count(), 1U);
    EXPECT_EQ(std::vector<uint8_t>(partial.begin(), partial.begin() + 12),
              std::vector<uint8_t>(expected.begin(), expected.begin() + 12));
  }

  // 6. Calling TIFFReadFromUserBuffer with outsize exceeding tbuf_size hits
  //    `sp->stream.avail_out > sp->tbuf_size`.
  {
    const std::vector<uint8_t> raw_zlib = MakeStoredZlibStream(
        EncodeUint16Tokens({0, 0, 0, 0, 0, 0, 0, 0}, false), true);
    MemoryTiffStream stream(BuildRawPixarLogTiff(false, false, 2, 1, 1, 1, 8,
                                                 SAMPLEFORMAT_UINT, raw_zlib));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    // First read completes SetupDecode with tbuf_size = (2 + 1) * 2 = 6 bytes.
    std::array<uint8_t, 2> ok_out = {};
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, ok_out.data(), 2), 2);

    std::vector<uint8_t> in_copy = raw_zlib;
    std::vector<uint8_t> big_out(8, 0x55);
    EXPECT_EQ(TIFFReadFromUserBuffer(reader.get(), 0, in_copy.data(),
                                     static_cast<tmsize_t>(in_copy.size()),
                                     big_out.data(), 8),
              0);
    EXPECT_THAT(big_out, testing::Each(0));
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }
}

TEST(TifPixarLogTest, OverflowAndOomGuards) {
  // 1. SetupEncode tbuf_size multiply overflow -> returns -1 safely.
  //    Height=1 keeps TIFFScanlineSize valid (2 GiB <= INT64_MAX) while a
  //    caller-supplied raw buffer skips multi-GB automatic allocation;
  //    PixarLogSetupEncode multiplies by td_rowsperstrip (0xFFFFFFFFU), which
  //    overflows tmsize_t.
  {
    MemoryTiffStream stream;
    UniqueTiff writer = stream.Open("w");
    ASSERT_NE(writer, nullptr);
    SetCommonStrippedTags(writer.get(), 0x40000000U, 1U, 0xFFFFFFFFU, 2, 8,
                          PLANARCONFIG_CONTIG, SAMPLEFORMAT_UINT);
    std::vector<uint8_t> raw_buf(8192, 0);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), raw_buf.data(),
                                   static_cast<tmsize_t>(raw_buf.size())),
              1);
    std::vector<uint8_t> buf(16, 0);
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, buf.data(), 16),
              static_cast<tmsize_t>(-1));
  }

  // 2. SetupDecode multiply_ms overflow and add_ms overflow.
  {
    const std::vector<uint8_t> payload =
        MakeStoredZlibStream(EncodeUint16Tokens({0, 0}, false), true);
    std::vector<uint8_t> copy = payload;
    std::vector<uint8_t> out(16, 0);

    // multiply_ms overflow: 1 * 2^31 * 2^31 * 2 = 2^63 > TMSIZE_T_MAX.
    // rows_per_strip = 0xFFFFFFFF avoids TIFFhowmany_32(2^31, 2^31) == 0 while
    // PixarLogSetupDecode clamps strip_height to imagelength.
    {
      MemoryTiffStream s1(BuildRawPixarLogTiff(false, true, 0x80000000U,
                                               0x80000000U, 0xFFFFFFFFU, 1, 8,
                                               SAMPLEFORMAT_UINT, payload));
      UniqueTiff tif = s1.Open("r");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(TIFFReadFromUserBuffer(tif.get(), 0, copy.data(),
                                       static_cast<tmsize_t>(copy.size()),
                                       out.data(), 4),
                0);
    }

    // add_ms overflow: (2^31 - 1) * (2^31 + 1) * 2 = (2^63 - 2); +2 overflows.
    // rows_per_strip = 0xFFFFFFFF avoids TIFFhowmany_32 overflow during open
    // while PixarLogSetupDecode clamps strip_height to imagelength.
    {
      MemoryTiffStream s2(BuildRawPixarLogTiff(false, true, (1U << 31) - 1U,
                                               (1U << 31) + 1U, 0xFFFFFFFFU, 1,
                                               8, SAMPLEFORMAT_UINT, payload));
      UniqueTiff tif = s2.Open("r");
      ASSERT_NE(tif, nullptr);
      EXPECT_EQ(TIFFReadFromUserBuffer(tif.get(), 0, copy.data(),
                                       static_cast<tmsize_t>(copy.size()),
                                       out.data(), 4),
                0);
    }
  }

  // 3. OOM on sp->tbuf in PixarLogSetupDecode.
  {
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("w");
      ASSERT_NE(writer, nullptr);
      SetCommonStrippedTags(writer.get(), 64, 64, 64, 3, 8, PLANARCONFIG_CONTIG,
                            SAMPLEFORMAT_UINT);
      std::vector<uint8_t> pixels(64 * 64 * 3, 5);
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, pixels.data(),
                                      static_cast<tmsize_t>(pixels.size())),
                static_cast<tmsize_t>(pixels.size()));
    }
    UniqueTiff reader = OpenWithMemLimit(stream, "r", 1 << 24);
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> out(64 * 64 * 3, 0);
    {
      // tbuf_size = (64 * 64 * 3 + 3) * 2 = 24582 bytes; fail with 4096 limit.
      ScopedSingleMemAllocLimit limit(reader.get(), 4096);
      EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                     static_cast<tmsize_t>(out.size())),
                static_cast<tmsize_t>(-1));
    }
    // After lifting OOM limit, reading succeeds cleanly.
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(out.size()));
    EXPECT_THAT(out, testing::Each(5));
  }

  // 4. OOM on sp->tbuf in PixarLogSetupEncode (tbuf=98304 > rawbuf=54067).
  {
    MemoryTiffStream stream;
    UniqueTiff writer = OpenWithMemLimit(stream, "w", 1 << 24);
    ASSERT_NE(writer, nullptr);
    SetCommonStrippedTags(writer.get(), 128, 128, 128, 3, 8,
                          PLANARCONFIG_CONTIG, SAMPLEFORMAT_UINT);
    std::vector<uint8_t> row(128 * 3, 12);
    {
      ScopedSingleMemAllocLimit limit(writer.get(), 65536);
      EXPECT_EQ(TIFFWriteScanline(writer.get(), row.data(), 0, 0), -1);
    }
    writer.reset();
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // 5. OOM inside TIFFInitPixarLog (PixarLogState allocation failure and
  // partial
  //    PixarLogMakeTables allocations). Use remaining budget = 100 bytes so the
  //    +16B _TIFFMergeFields realloc succeeds while sizeof(PixarLogState) fails
  //    cleanly at goto bad without hitting the upstream _TIFFMergeFields OOM
  //    bug (tif_dirinfo.c zeroes tif_nfields before freeing tif_fields).
  {
    MemoryTiffStream stream;
    UniqueTiff tif = OpenWithMemLimit(stream, "w", 1 << 24);
    ASSERT_NE(tif, nullptr);
    {
      ScopedCumulatedMemAllocRemaining state_oom(tif.get(), 100);
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_PIXARLOG),
          0);
    }
    tif.reset();
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }

  // Sweep cumulative memory budgets across table allocations in
  // PixarLogMakeTables so partial allocations are cleanly unwound and freed.
  constexpr auto kBudgets = std::to_array<tmsize_t>({
      5000,  // Allows _TIFFMergeFields + PixarLogState; fails FromLT2 (~54 KB).
      60000,  // Allows FromLT2; fails From14 (32 KB).
      90000,  // Allows FromLT2 + From14 + From8; fails ToLinearF/16/8.
      100000,
      104000,
  });
  for (tmsize_t budget : kBudgets) {
    MemoryTiffStream stream;
    UniqueTiff tif = OpenWithMemLimit(stream, "w", 1 << 24);
    ASSERT_NE(tif, nullptr);
    {
      ScopedCumulatedMemAllocRemaining guard(tif.get(), budget);
      // Do not encode/decode after MakeTables OOM; just close via UniqueTiff.
      EXPECT_EQ(
          TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_PIXARLOG),
          1);
    }
    tif.reset();
  }
}

}  // namespace
}  // namespace libtiff
