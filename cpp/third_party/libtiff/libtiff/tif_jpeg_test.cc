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

// Clean-room C++20 unit tests for libtiff/tif_jpeg.c (COMPRESSION_JPEG codec).
//
// google3 caveats:
//   - JPEG_SUPPORT and CHECK_JPEG_YCBCR_SUBSAMPLING are enabled in tif_config.h
//     backed by third_party/libjpeg_turbo (8-bit JSAMPLE; dual-mode 8/12-bit
//     is not enabled in google3).
//   - JPEGMEM / LIBTIFF_JPEG_MAX_ALLOWED_SCAN_NUMBER /
//     LIBTIFF_ALLOW_LARGE_LIBJPEG_MEM_ALLOC tests use RAII environment guards.
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_jpeg.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

constexpr uint32_t kDefaultAllocBudget = 1U << 24;

class ScopedEnvVar {
 public:
  ScopedEnvVar(const char* name, const char* value) : name_(name) {
    const char* prev = std::getenv(name);
    if (prev != nullptr) {
      had_prev_ = true;
      prev_value_ = prev;
    }
    if (value != nullptr) {
      ::setenv(name_.c_str(), value, 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

  ~ScopedEnvVar() {
    if (had_prev_) {
      ::setenv(name_.c_str(), prev_value_.c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  std::string name_;
  bool had_prev_ = false;
  std::string prev_value_;
};

bool HasDiagnosticContaining(const std::vector<DiagnosticRecord>& records,
                             std::string_view substr) {
  return std::any_of(records.begin(), records.end(),
                     [substr](const DiagnosticRecord& rec) {
                       return rec.message.find(substr) != std::string::npos;
                     });
}

std::vector<uint8_t> MakeSmoothRgbPixels(uint32_t width, uint32_t height,
                                         uint32_t seed = 0) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 3);
  const uint32_t max_w = std::max(width, 1U);
  const uint32_t max_h = std::max(height, 1U);
  const uint32_t max_sum = std::max(width + height, 1U);
  const uint32_t offset = (seed * 19U) % 40U;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t idx = (static_cast<size_t>(y) * width + x) * 3;
      out[idx + 0] = static_cast<uint8_t>(24U + offset + (x * 160U) / max_w);
      out[idx + 1] = static_cast<uint8_t>(32U + offset + (y * 160U) / max_h);
      out[idx + 2] =
          static_cast<uint8_t>(40U + offset + ((x + y) * 140U) / max_sum);
    }
  }
  return out;
}

std::vector<uint8_t> MakeSmoothGrayPixels(uint32_t width, uint32_t height) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * height);
  const uint32_t max_sum = std::max(width + height, 1U);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      out[static_cast<size_t>(y) * width + x] =
          static_cast<uint8_t>(20U + ((x + y) * 200U) / max_sum);
    }
  }
  return out;
}

std::vector<uint8_t> MakeSmoothCmykPixels(uint32_t width, uint32_t height) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
  const uint32_t max_w = std::max(width, 1U);
  const uint32_t max_h = std::max(height, 1U);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
      out[idx + 0] = static_cast<uint8_t>(30U + (x * 160U) / max_w);
      out[idx + 1] = static_cast<uint8_t>(40U + (y * 160U) / max_h);
      out[idx + 2] =
          static_cast<uint8_t>(50U + ((x + y) * 80U) / (max_w + max_h));
      out[idx + 3] = static_cast<uint8_t>(20U + ((x * 80U) / max_w));
    }
  }
  return out;
}

std::vector<uint8_t> MakeNoisyRgbPixels(uint32_t width, uint32_t height,
                                        uint32_t seed = 0x2468ace1U) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 3);
  uint32_t state = seed != 0 ? seed : 1U;
  for (uint8_t& b : out) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    b = static_cast<uint8_t>(state & 0xffU);
  }
  return out;
}

int MaxAbsDiff(absl::Span<const uint8_t> a, absl::Span<const uint8_t> b) {
  if (a.size() != b.size()) return -1;
  int max_diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    if (d > max_diff) max_diff = d;
  }
  return max_diff;
}

// Encodes a single-strip standalone JPEG stream (JPEGTABLESMODE=0) using
// libtiff so callers get a compliant JPEG codestream for RawTiffBuilder tests.
std::vector<uint8_t> EncodeStandaloneJpegStrip(
    uint32_t width, uint32_t height, uint16_t photometric,
    uint16_t samples_per_pixel, uint16_t sub_x = 1, uint16_t sub_y = 1,
    int color_mode = JPEGCOLORMODE_RGB) {
  MemoryTiffStream mem;
  {
    UniqueTiff tif = mem.Open("w");
    EXPECT_NE(tif, nullptr);
    if (!tif) return {};
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, width), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, height), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, samples_per_pixel), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, photometric), 1);
    EXPECT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, height), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGTABLESMODE, 0), 1);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGQUALITY, 90), 1);
    if (photometric == PHOTOMETRIC_YCBCR) {
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, sub_x, sub_y),
                1);
      EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, color_mode), 1);
    }
    std::vector<uint8_t> pixels = (samples_per_pixel == 1)
                                      ? MakeSmoothGrayPixels(width, height)
                                      : MakeSmoothRgbPixels(width, height);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, pixels.data(),
                                    static_cast<tmsize_t>(pixels.size())),
              static_cast<tmsize_t>(pixels.size()));
    EXPECT_EQ(TIFFWriteDirectory(tif.get()), 1);
  }
  UniqueTiff reader = mem.Open("r");
  EXPECT_NE(reader, nullptr);
  if (!reader) return {};
  const uint64_t bc = TIFFGetStrileByteCount(reader.get(), 0);
  std::vector<uint8_t> raw(static_cast<size_t>(bc));
  EXPECT_EQ(TIFFReadRawStrip(reader.get(), 0, raw.data(),
                             static_cast<tmsize_t>(raw.size())),
            static_cast<tmsize_t>(raw.size()));
  return raw;
}

// Builds a minimal 2-scan progressive 8x8 grayscale JPEG stream (SOF2).
// Scan 1 sends DC diff=0 (bit 0); scan 2 sends AC EOB (bit 0).
std::vector<uint8_t> MakeMinimalProgressiveGray8x8Jpeg() {
  std::vector<uint8_t> out;
  // SOI
  out.insert(out.end(), {0xFF, 0xD8});
  // DQT (length=67, table 0, 8-bit)
  out.insert(out.end(), {0xFF, 0xDB, 0x00, 0x43, 0x00});
  out.insert(out.end(), 64, 0x10);
  // SOF2 (length=11, 8-bit, 8x8, 1 component, samp=1x1, qtbl=0)
  out.insert(out.end(), {0xFF, 0xC2, 0x00, 0x0B, 0x08, 0x00, 0x08, 0x00, 0x08,
                         0x01, 0x01, 0x11, 0x00});
  // DHT DC table 0: 1 symbol of length 1 (code 0 -> category 0)
  out.insert(out.end(), {0xFF, 0xC4, 0x00, 0x14, 0x00, 0x01});
  out.insert(out.end(), 15, 0x00);
  out.push_back(0x00);
  // DHT AC table 0: 1 symbol of length 1 (code 0 -> EOB)
  out.insert(out.end(), {0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01});
  out.insert(out.end(), 15, 0x00);
  out.push_back(0x00);
  // SOS scan 1 (DC: Ss=0, Se=0)
  out.insert(out.end(), {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x00,
                         0x00, 0x7F});
  // SOS scan 2 (AC: Ss=1, Se=63)
  out.insert(out.end(), {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x01, 0x3F,
                         0x00, 0x7F});
  // EOI
  out.insert(out.end(), {0xFF, 0xD9});
  return out;
}

// Builds a RawTiffBuilder IFD for a YCbCr CONTIG 3-sample JPEG image WITHOUT
// TIFFTAG_YCBCRSUBSAMPLING so JPEGFixupTagsSubsampling executes on open.
std::vector<uint8_t> BuildYcbcrNoSubsamplingTagTiff(
    uint32_t width, uint32_t height, uint32_t rows_per_strip,
    absl::Span<const uint8_t> strip_bytes, bool zero_offset = false) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  uint64_t offset = 0;
  uint32_t bytecount = 0;
  if (!zero_offset) {
    offset = builder.AppendBlob(strip_bytes);
    bytecount = static_cast<uint32_t>(strip_bytes.size());
  }
  const std::array<uint16_t, 3> bps = {8, 8, 8};
  builder.AddIfd({
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, width),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, height),
      builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(offset)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, rows_per_strip),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, bytecount),
      builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
  });
  return builder.Take();
}

TEST(TifJpegTest, CodecRegistrationAndDefaults) {
  EXPECT_EQ(TIFFIsCODECConfigured(COMPRESSION_JPEG), 1);

  const TIFFCodec* codec = TIFFFindCODEC(COMPRESSION_JPEG);
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->scheme, COMPRESSION_JPEG);
  EXPECT_NE(codec->name, nullptr);

  UniqueTiffBuffer<TIFFCodec> codecs(TIFFGetConfiguredCODECs());
  ASSERT_NE(codecs, nullptr);
  bool found = false;
  for (const TIFFCodec* p = codecs.get(); p->name != nullptr; ++p) {
    if (p->scheme == COMPRESSION_JPEG) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST(TifJpegTest, PseudoTagsMaxCompressionRatioAndDefaultSizes) {
  MemoryTiffStream mem;
  UniqueTiff tif = mem.Open("w");
  ASSERT_NE(tif, nullptr);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{64}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{64}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
            1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR), 1);

  // Verify default pseudo-tag values and parent tag fallback via JPEGVGetField.
  int quality = 0;
  int colormode = -1;
  int tablesmode = 0;
  uint32_t width = 0;
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_JPEGQUALITY, &quality), 1);
  EXPECT_EQ(quality, 75);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_JPEGCOLORMODE, &colormode), 1);
  EXPECT_EQ(colormode, JPEGCOLORMODE_RAW);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_JPEGTABLESMODE, &tablesmode), 1);
  EXPECT_EQ(tablesmode, JPEGTABLESMODE_QUANT | JPEGTABLESMODE_HUFF);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
  EXPECT_EQ(width, 64U);

  // Before JPEGSetupEncode/Decode populates sp->h_sampling/v_sampling,
  // YCbCr contig 3-sample returns 0 (unknown); after encoding 2x2 YCbCr it
  // returns 512, whereas 2x1 YCbCr returns 0 and RGB returns 256.
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 0ULL);
  {
    MemoryTiffStream ycbcr_mem;
    UniqueTiff ycbcr_tif = ycbcr_mem.Open("w");
    ASSERT_NE(ycbcr_tif, nullptr);
    ASSERT_EQ(TIFFSetField(ycbcr_tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}),
              1);
    ASSERT_EQ(TIFFSetField(ycbcr_tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}),
              1);
    ASSERT_EQ(TIFFSetField(ycbcr_tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(
        TIFFSetField(ycbcr_tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
    ASSERT_EQ(
        TIFFSetField(ycbcr_tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        1);
    ASSERT_EQ(TIFFSetField(ycbcr_tif.get(), TIFFTAG_PLANARCONFIG,
                           PLANARCONFIG_CONTIG),
              1);
    ASSERT_EQ(
        TIFFSetField(ycbcr_tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
        1);
    ASSERT_EQ(TIFFSetField(ycbcr_tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{16}),
              1);
    ASSERT_EQ(
        TIFFSetField(ycbcr_tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
        1);
    std::vector<uint8_t> pixels = MakeSmoothRgbPixels(16, 16);
    ASSERT_EQ(TIFFWriteEncodedStrip(ycbcr_tif.get(), 0, pixels.data(),
                                    static_cast<tmsize_t>(pixels.size())),
              static_cast<tmsize_t>(pixels.size()));
    EXPECT_EQ(TIFFGetMaxCompressionRatio(ycbcr_tif.get()), 512ULL);
  }

  // Non-2x2 YCbCr returns 0 (unknown).
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, uint16_t{2},
                         uint16_t{1}),
            1);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 0ULL);
  // JPEGDefaultStripSize rounds up when < imagelength, preserves when >=
  // imagelength.
  EXPECT_EQ(TIFFDefaultStripSize(tif.get(), 1U), 8U);
  EXPECT_EQ(TIFFDefaultStripSize(tif.get(), 100U), 100U);

  uint32_t tw = 1U;
  uint32_t th = 1U;
  TIFFDefaultTileSize(tif.get(), &tw, &th);
  EXPECT_EQ(tw % 16U, 0U);
  EXPECT_EQ(th % 8U, 0U);

  // Switch JPEGCOLORMODE_RGB and verify TIFFIsUpSampled and cached sizes
  // update.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
            1);
  EXPECT_EQ(TIFFIsUpSampled(tif.get()), 1);
  EXPECT_EQ(TIFFScanlineSize(tif.get()), 64 * 3);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RAW),
            1);
  EXPECT_EQ(TIFFIsUpSampled(tif.get()), 0);
  EXPECT_GT(TIFFScanlineSize(tif.get()), 0);

  // Test JPEGResetUpsampled when td_tilesize > 0 already exists.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, uint32_t{32}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, uint32_t{32}), 1);
  EXPECT_GT(TIFFTileSize(tif.get()), 0);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
            1);
  EXPECT_EQ(TIFFTileSize(tif.get()), 32 * 32 * 3);

  // Non-YCbCr returns max compression ratio 256 (384 for 12-bit).
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
  EXPECT_EQ(TIFFIsUpSampled(tif.get()), 0);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 256ULL);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{12}), 1);
  EXPECT_EQ(TIFFGetMaxCompressionRatio(tif.get()), 384ULL);

  // Zero-length TIFFTAG_JPEGTABLES must be rejected.
  const uint8_t dummy = 0;
  EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGTABLES, uint32_t{0}, &dummy),
            0);

  // 12-bit YCbCr 2x2 CONTIG returns 768 after JPEGSetupDecode records sampling.
  {
    const std::vector<uint8_t> ycbcr_strip =
        EncodeStandaloneJpegStrip(16, 16, PHOTOMETRIC_YCBCR, 3, /*sub_x=*/2,
                                  /*sub_y=*/2, JPEGCOLORMODE_RGB);
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(ycbcr_strip);
    const std::array<uint16_t, 3> bps12 = {12, 12, 12};
    const std::array<uint16_t, 2> sub22 = {2, 2};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps12),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 16),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(ycbcr_strip.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        builder.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING, sub22),
    });
    MemoryTiffStream mem12(builder.Take());
    UniqueTiff tif12 = mem12.Open("r");
    ASSERT_NE(tif12, nullptr);
    std::vector<uint8_t> out(16 * 16 * 3 * 2);
    EXPECT_EQ(TIFFReadEncodedStrip(tif12.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_EQ(TIFFGetMaxCompressionRatio(tif12.get()), 768ULL);
  }
}

void CustomParentPrintDir(TIFF* /*tif*/, FILE* fd,
                          long /*flags*/) {  // NOLINT(runtime/int)
  std::fprintf(fd, "  CustomParentPrintDir\n");
}

TEST(TifJpegTest, PrintDirectoryWithAndWithoutJpegTables) {
  for (int tables_mode : {JPEGTABLESMODE_QUANT | JPEGTABLESMODE_HUFF, 0}) {
    SCOPED_TRACE(::testing::Message() << "tables_mode=" << tables_mode);
    MemoryTiffStream mem;
    {
      UniqueTiff writer = mem.Open("w");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      if (tables_mode != 0) {
        // Hook per-handle parent printdir before COMPRESSION_JPEG captures it.
        writer->tif_tagmethods.printdir = &CustomParentPrintDir;
      }
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{16}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGTABLESMODE, tables_mode),
                1);
      std::vector<uint8_t> pixels = MakeSmoothRgbPixels(16, 16);
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, pixels.data(),
                                      static_cast<tmsize_t>(pixels.size())),
                static_cast<tmsize_t>(pixels.size()));
      if (tables_mode != 0) {
        char* w_buf = nullptr;
        size_t w_size = 0;
        FILE* w_fp = ::open_memstream(&w_buf, &w_size);
        ASSERT_NE(w_fp, nullptr);
        auto w_guard = absl::Cleanup([&] {
          if (w_fp != nullptr) std::fclose(w_fp);
          std::free(w_buf);
        });
        TIFFPrintDirectory(writer.get(), w_fp, 0);
        std::fclose(w_fp);
        w_fp = nullptr;
        std::string w_printed(w_buf != nullptr ? w_buf : "", w_size);
        EXPECT_NE(w_printed.find("CustomParentPrintDir"), std::string::npos);
        EXPECT_NE(w_printed.find("JPEG Tables:"), std::string::npos);
      }
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = mem.Open("r");
    ASSERT_NE(reader, nullptr);

    char* mem_buf = nullptr;
    size_t mem_size = 0;
    FILE* fp = ::open_memstream(&mem_buf, &mem_size);
    ASSERT_NE(fp, nullptr);
    auto fp_guard = absl::Cleanup([&] {
      if (fp != nullptr) std::fclose(fp);
      std::free(mem_buf);
    });
    TIFFPrintDirectory(reader.get(), fp, 0);
    std::fclose(fp);
    fp = nullptr;
    std::string printed(mem_buf != nullptr ? mem_buf : "", mem_size);
    if (tables_mode != 0) {
      EXPECT_NE(printed.find("JPEG Tables:"), std::string::npos);
    } else {
      EXPECT_EQ(printed.find("JPEG Tables:"), std::string::npos);
    }
  }
}

TEST(TifJpegTest, RgbScanlineRoundTripEndianAndBigTiff) {
  for (const char* write_mode : {"wl", "wb", "w8"}) {
    SCOPED_TRACE(::testing::Message() << "mode=" << write_mode);
    constexpr uint32_t kW = 24;
    constexpr uint32_t kH = 16;
    const std::vector<uint8_t> expected =
        MakeSmoothRgbPixels(kW, kH, /*seed=*/2);

    MemoryTiffStream mem;
    {
      UniqueTiff writer = mem.Open(write_mode);
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{8}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 95), 1);

      for (uint32_t row = 0; row < kH; ++row) {
        std::vector<uint8_t> row_buf(
            expected.begin() + static_cast<std::ptrdiff_t>(row * kW * 3),
            expected.begin() + static_cast<std::ptrdiff_t>((row + 1) * kW * 3));
        ASSERT_EQ(TIFFWriteScanline(writer.get(), row_buf.data(), row, 0), 1);
      }
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = mem.Open("r");
    ASSERT_NE(reader, nullptr);
    uint32_t jtbl_len = 0;
    const void* jtbl_data = nullptr;
    EXPECT_EQ(
        TIFFGetField(reader.get(), TIFFTAG_JPEGTABLES, &jtbl_len, &jtbl_data),
        1);
    EXPECT_GT(jtbl_len, 0U);
    EXPECT_NE(jtbl_data, nullptr);

    std::vector<uint8_t> decoded(expected.size());
    for (uint32_t row = 0; row < kH; ++row) {
      ASSERT_EQ(
          TIFFReadScanline(reader.get(), decoded.data() + row * kW * 3, row, 0),
          1);
    }
    EXPECT_LE(MaxAbsDiff(expected, decoded), 16);
    EXPECT_EQ(mem.diagnostics().error_count(), 0U);
  }
}

TEST(TifJpegTest, MinisBlackMinisWhiteSeparatedAndUnknownPhotometrics) {
  struct PhotometricCase {
    uint16_t photometric;
    uint16_t samples;
    bool set_cmyk_inkset;
    bool set_extra_sample;
  };
  constexpr std::array<PhotometricCase, 4> kCases = {{
      {PHOTOMETRIC_MINISBLACK, 1, false, false},
      {PHOTOMETRIC_MINISWHITE, 1, false, false},
      {PHOTOMETRIC_SEPARATED, 4, true, false},
      {PHOTOMETRIC_RGB, 4, false,
       true},  // Triggers JCS_UNKNOWN in setup/predecode.
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(::testing::Message()
                 << "photometric=" << tc.photometric << " spp=" << tc.samples);
    constexpr uint32_t kW = 16;
    constexpr uint32_t kH = 16;
    std::vector<uint8_t> expected;
    if (tc.samples == 1) {
      expected = MakeSmoothGrayPixels(kW, kH);
    } else {
      expected = MakeSmoothCmykPixels(kW, kH);
    }

    MemoryTiffStream mem;
    {
      UniqueTiff writer = mem.Open("w");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, tc.samples),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, tc.photometric),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 95), 1);
      if (tc.set_cmyk_inkset) {
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_INKSET, INKSET_CMYK), 1);
      }
      if (tc.set_extra_sample) {
        const uint16_t extra = EXTRASAMPLE_UNASSALPHA;
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_EXTRASAMPLES, uint16_t{1},
                               &extra),
                  1);
      }
      std::vector<uint8_t> mutable_pixels = expected;
      ASSERT_EQ(
          TIFFWriteEncodedStrip(writer.get(), 0, mutable_pixels.data(),
                                static_cast<tmsize_t>(mutable_pixels.size())),
          static_cast<tmsize_t>(mutable_pixels.size()));
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = mem.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> decoded(expected.size());
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                   static_cast<tmsize_t>(decoded.size())),
              static_cast<tmsize_t>(decoded.size()));
    EXPECT_LE(MaxAbsDiff(expected, decoded), 16);
    EXPECT_EQ(mem.diagnostics().error_count(), 0U);
  }
}

TEST(TifJpegTest, YcbcrSubsamplingRgbModeAndReferenceBlackWhite) {
  constexpr std::array<std::pair<uint16_t, uint16_t>, 3> kSubsamplings = {{
      {1, 1},
      {2, 1},
      {2, 2},
  }};
  for (size_t idx = 0; idx < kSubsamplings.size(); ++idx) {
    const auto [sx, sy] = kSubsamplings[idx];
    SCOPED_TRACE(::testing::Message() << "subsampling=" << sx << "x" << sy);
    constexpr uint32_t kW = 32;
    constexpr uint32_t kH = 16;
    const std::vector<uint8_t> expected = MakeSmoothRgbPixels(kW, kH);

    MemoryTiffStream mem;
    {
      UniqueTiff writer = mem.Open("w");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
          1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING, sx, sy),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 95), 1);

      // Explicitly set ReferenceBlackWhite on the first case to hit both
      // user-supplied and auto-populated branches in JPEGSetupEncode.
      if (idx == 0) {
        const std::array<float, 6> refbw = {0.0f,   255.0f, 128.0f,
                                            255.0f, 128.0f, 255.0f};
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_REFERENCEBLACKWHITE,
                               refbw.data()),
                  1);
      }

      std::vector<uint8_t> mutable_pixels = expected;
      ASSERT_EQ(
          TIFFWriteEncodedStrip(writer.get(), 0, mutable_pixels.data(),
                                static_cast<tmsize_t>(mutable_pixels.size())),
          static_cast<tmsize_t>(mutable_pixels.size()));
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = mem.Open("r");
    ASSERT_NE(reader, nullptr);
    uint16_t got_sx = 0;
    uint16_t got_sy = 0;
    EXPECT_EQ(TIFFGetFieldDefaulted(reader.get(), TIFFTAG_YCBCRSUBSAMPLING,
                                    &got_sx, &got_sy),
              1);
    EXPECT_EQ(got_sx, sx);
    EXPECT_EQ(got_sy, sy);
    float* got_refbw = nullptr;
    EXPECT_EQ(
        TIFFGetField(reader.get(), TIFFTAG_REFERENCEBLACKWHITE, &got_refbw), 1);
    EXPECT_NE(got_refbw, nullptr);

    ASSERT_EQ(
        TIFFSetField(reader.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
        1);
    std::vector<uint8_t> decoded(expected.size());
    for (uint32_t row = 0; row < kH; ++row) {
      ASSERT_EQ(
          TIFFReadScanline(reader.get(), decoded.data() + row * kW * 3, row, 0),
          1);
    }
    EXPECT_LE(MaxAbsDiff(expected, decoded), 20);
    EXPECT_EQ(mem.diagnostics().error_count(), 0U);
  }
}

TEST(TifJpegTest, YcbcrRawDownsampledEncodeDecodeAndDecodeRowError) {
  // Test multiple subsamplings and non-MCU dimensions to exercise:
  // - 2x2 (19x20, rowsperstrip=32 -> scancount >= DCTSIZE flush + PostEncode
  // padding)
  // - 2x1 (17x12, rowsperstrip=16 -> PostEncode padding, v_sampling=1)
  // - 1x1 raw (16x16 -> JPEGDecode non-downsampled JCS_UNKNOWN path)
  struct RawCase {
    uint32_t width;
    uint32_t height;
    uint32_t rows_per_strip;
    uint16_t sx;
    uint16_t sy;
    bool expect_decode_row_error;
  };
  constexpr std::array<RawCase, 3> kCases = {{
      {19, 20, 32, 2, 2, true},
      {17, 12, 16, 2, 1, true},
      {16, 16, 16, 1, 1, false},
  }};

  for (const auto& tc : kCases) {
    SCOPED_TRACE(::testing::Message() << tc.width << "x" << tc.height
                                      << " sub=" << tc.sx << "x" << tc.sy);
    MemoryTiffStream mem;
    std::vector<uint8_t> raw_ycbcr;
    {
      UniqueTiff writer = mem.Open("w");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, tc.width), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, tc.height), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
          1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, tc.rows_per_strip),
          1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING, tc.sx, tc.sy),
          1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RAW),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 95), 1);

      const tmsize_t strip_sz = TIFFVStripSize(writer.get(), tc.height);
      ASSERT_GT(strip_sz, 0);
      raw_ycbcr.resize(static_cast<size_t>(strip_sz));
      const size_t clump_samples = static_cast<size_t>(tc.sx) * tc.sy + 2;
      const size_t clumps_per_row = (tc.width + tc.sx - 1) / tc.sx;
      const size_t clump_rows = (tc.height + tc.sy - 1) / tc.sy;
      for (size_t cy = 0; cy < clump_rows; ++cy) {
        for (size_t cx = 0; cx < clumps_per_row; ++cx) {
          const size_t base = (cy * clumps_per_row + cx) * clump_samples;
          const uint8_t y_val =
              static_cast<uint8_t>(60 + ((cx * 10 + cy * 7) % 120));
          for (size_t k = 0; k < static_cast<size_t>(tc.sx) * tc.sy; ++k) {
            raw_ycbcr[base + k] = y_val;
          }
          raw_ycbcr[base + static_cast<size_t>(tc.sx) * tc.sy] = 128;
          raw_ycbcr[base + static_cast<size_t>(tc.sx) * tc.sy + 1] = 128;
        }
      }
      std::vector<uint8_t> mutable_raw = raw_ycbcr;
      ASSERT_EQ(
          TIFFWriteEncodedStrip(writer.get(), 0, mutable_raw.data(), strip_sz),
          strip_sz);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = mem.Open("r");
    ASSERT_NE(reader, nullptr);
    const tmsize_t strip_sz = TIFFVStripSize(reader.get(), tc.height);
    ASSERT_EQ(strip_sz, static_cast<tmsize_t>(raw_ycbcr.size()));
    std::vector<uint8_t> decoded(static_cast<size_t>(strip_sz));
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(), strip_sz),
              strip_sz);

    // Check active pixels match closely (note trailing dummy pixels in
    // odd-sized raw YCbCr buffers may be zero-padded by JPEGDecodeRaw).
    if (!tc.expect_decode_row_error) {
      EXPECT_LE(MaxAbsDiff(raw_ycbcr, decoded), 12);
    } else {
      std::vector<uint8_t> scanline(
          static_cast<size_t>(TIFFScanlineSize(reader.get())));
      EXPECT_EQ(TIFFReadScanline(reader.get(), scanline.data(), 0, 0), -1);
      EXPECT_TRUE(HasDiagnosticContaining(
          mem.diagnostics().errors(),
          "scanline oriented access is not supported for downsampled JPEG"));
    }
  }
}

TEST(TifJpegTest, TiledRgbAndYcbcrRoundTripsAndReadRgbaImage) {
  for (uint16_t photometric : {PHOTOMETRIC_RGB, PHOTOMETRIC_YCBCR}) {
    SCOPED_TRACE(::testing::Message() << "photometric=" << photometric);
    constexpr uint32_t kW = 32;
    constexpr uint32_t kH = 32;
    constexpr uint32_t kTw = 16;
    constexpr uint32_t kTh = 16;
    const std::vector<uint8_t> expected = MakeSmoothRgbPixels(kW, kH);

    MemoryTiffStream mem;
    {
      UniqueTiff writer = mem.Open("w");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, kTw), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, kTh), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, photometric),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 95), 1);
      if (photometric == PHOTOMETRIC_YCBCR) {
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING,
                               uint16_t{2}, uint16_t{2}),
                  1);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGCOLORMODE,
                               JPEGCOLORMODE_RGB),
                  1);
      }

      const tmsize_t tile_sz = TIFFTileSize(writer.get());
      ASSERT_EQ(tile_sz, static_cast<tmsize_t>(kTw * kTh * 3));
      for (uint32_t ty = 0; ty < kH; ty += kTh) {
        for (uint32_t tx = 0; tx < kW; tx += kTw) {
          std::vector<uint8_t> tile(static_cast<size_t>(tile_sz));
          for (uint32_t y = 0; y < kTh; ++y) {
            std::memcpy(tile.data() + y * kTw * 3,
                        expected.data() + ((ty + y) * kW + tx) * 3, kTw * 3);
          }
          ASSERT_EQ(TIFFWriteTile(writer.get(), tile.data(), tx, ty, 0, 0),
                    tile_sz);
        }
      }
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = mem.Open("r");
    ASSERT_NE(reader, nullptr);
    if (photometric == PHOTOMETRIC_YCBCR) {
      // Read first tile in raw downsampled mode, then switch to RGB for RGBA.
      const tmsize_t raw_tile_sz = TIFFTileSize(reader.get());
      ASSERT_GT(raw_tile_sz, 0);
      std::vector<uint8_t> raw_tile(static_cast<size_t>(raw_tile_sz));
      EXPECT_EQ(
          TIFFReadEncodedTile(reader.get(), 0, raw_tile.data(), raw_tile_sz),
          raw_tile_sz);
    }

    std::vector<uint32_t> raster(kW * kH, 0);
    ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), kW, kH, raster.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    for (uint32_t y = 0; y < kH; ++y) {
      for (uint32_t x = 0; x < kW; ++x) {
        const uint32_t px = raster[y * kW + x];
        const size_t idx = (y * kW + x) * 3;
        EXPECT_NEAR(static_cast<int>(TIFFGetR(px)), expected[idx + 0], 24);
        EXPECT_NEAR(static_cast<int>(TIFFGetG(px)), expected[idx + 1], 24);
        EXPECT_NEAR(static_cast<int>(TIFFGetB(px)), expected[idx + 2], 24);
        EXPECT_EQ(TIFFGetA(px), 255U);
      }
    }
    EXPECT_EQ(mem.diagnostics().error_count(), 0U);
  }
}

TEST(TifJpegTest, PlanarSeparateRgbAndYcbcrSubsampled) {
  for (uint16_t photometric : {PHOTOMETRIC_RGB, PHOTOMETRIC_YCBCR}) {
    SCOPED_TRACE(::testing::Message() << "photometric=" << photometric);
    constexpr uint32_t kW = 16;
    constexpr uint32_t kH = 16;
    MemoryTiffStream mem;
    std::array<std::vector<uint8_t>, 3> plane_data;
    {
      UniqueTiff writer = mem.Open("w");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, photometric),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                             PLANARCONFIG_SEPARATE),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 95), 1);
      if (photometric == PHOTOMETRIC_YCBCR) {
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING,
                               uint16_t{1}, uint16_t{1}),
                  1);
      }
      for (uint16_t s = 0; s < 3; ++s) {
        plane_data[s] = MakeSmoothGrayPixels(kW, kH);
        std::vector<uint8_t> mutable_plane = plane_data[s];
        const tmsize_t sz = static_cast<tmsize_t>(mutable_plane.size());
        ASSERT_EQ(
            TIFFWriteEncodedStrip(writer.get(), s, mutable_plane.data(), sz),
            sz);
      }
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = mem.Open("r");
    ASSERT_NE(reader, nullptr);
    for (uint16_t s = 0; s < 3; ++s) {
      std::vector<uint8_t> decoded(plane_data[s].size());
      const tmsize_t sz = static_cast<tmsize_t>(decoded.size());
      ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), s, decoded.data(), sz), sz);
      EXPECT_LE(MaxAbsDiff(plane_data[s], decoded), 16);
    }
    EXPECT_EQ(mem.diagnostics().error_count(), 0U);
  }
}

TEST(TifJpegTest, JpegTablesModesAllCombinations) {
  constexpr std::array<int, 4> kModes = {
      JPEGTABLESMODE_QUANT | JPEGTABLESMODE_HUFF,
      JPEGTABLESMODE_QUANT,
      JPEGTABLESMODE_HUFF,
      0,
  };
  for (int mode : kModes) {
    SCOPED_TRACE(::testing::Message() << "jpegtablesmode=" << mode);
    constexpr uint32_t kW = 16;
    constexpr uint32_t kH = 16;
    const std::vector<uint8_t> expected = MakeSmoothRgbPixels(kW, kH);

    MemoryTiffStream mem;
    {
      UniqueTiff writer = mem.Open("w");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
          1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kH), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING,
                             uint16_t{1}, uint16_t{1}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGTABLESMODE, mode), 1);
      std::vector<uint8_t> mutable_pixels = expected;
      ASSERT_EQ(
          TIFFWriteEncodedStrip(writer.get(), 0, mutable_pixels.data(),
                                static_cast<tmsize_t>(mutable_pixels.size())),
          static_cast<tmsize_t>(mutable_pixels.size()));
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }

    UniqueTiff reader = mem.Open("r");
    ASSERT_NE(reader, nullptr);
    uint32_t len = 0;
    const void* tables = nullptr;
    const int has_tables =
        TIFFGetField(reader.get(), TIFFTAG_JPEGTABLES, &len, &tables);
    if (mode != 0) {
      EXPECT_EQ(has_tables, 1);
      EXPECT_GT(len, 0U);
      EXPECT_NE(tables, nullptr);
    } else {
      EXPECT_EQ(has_tables, 0);
    }
    ASSERT_EQ(
        TIFFSetField(reader.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
        1);
    std::vector<uint8_t> decoded(expected.size());
    ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                   static_cast<tmsize_t>(decoded.size())),
              static_cast<tmsize_t>(decoded.size()));
    EXPECT_LE(MaxAbsDiff(expected, decoded), 20);
    EXPECT_EQ(mem.diagnostics().error_count(), 0U);
  }
}

TEST(TifJpegTest, LargeStripBufferFlushAndFlushFailureRecovery) {
  // Force std_empty_output_buffer to flush multiple times using a 1024-byte
  // output buffer and high-entropy RGB pixels.
  constexpr uint32_t kW = 64;
  constexpr uint32_t kH = 64;
  const std::vector<uint8_t> noisy = MakeNoisyRgbPixels(kW, kH);

  MemoryTiffStream mem;
  {
    UniqueTiff writer = mem.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kH), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 100), 1);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 1024), 1);

    std::vector<uint8_t> mutable_noisy = noisy;
    ASSERT_EQ(
        TIFFWriteEncodedStrip(writer.get(), 0, mutable_noisy.data(),
                              static_cast<tmsize_t>(mutable_noisy.size())),
        static_cast<tmsize_t>(mutable_noisy.size()));
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = mem.Open("r");
  ASSERT_NE(reader, nullptr);
  std::vector<uint8_t> decoded(noisy.size());
  ASSERT_EQ(TIFFReadEncodedStrip(reader.get(), 0, decoded.data(),
                                 static_cast<tmsize_t>(decoded.size())),
            static_cast<tmsize_t>(decoded.size()));
  EXPECT_EQ(mem.diagnostics().error_count(), 0U);

  // Now fail writes inside std_empty_output_buffer -> TIFFFlushData1 failure
  // -> ERREXIT via setjmp recovery in JPEGEncode and JPEGEncodeRaw.
  {
    MemoryTiffStream fail_mem;
    UniqueTiff writer = fail_mem.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kH), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 100), 1);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 512), 1);
    // Allow header write (call 0), fail subsequent strip flushes.
    fail_mem.FailWritesAfter(1);

    std::vector<uint8_t> mutable_noisy = noisy;
    EXPECT_EQ(
        TIFFWriteEncodedStrip(writer.get(), 0, mutable_noisy.data(),
                              static_cast<tmsize_t>(mutable_noisy.size())),
        static_cast<tmsize_t>(-1));
  }
  {
    // For YCbCr 2x1 raw mode (v_sampling == 1), TIFFScanlineSize ==
    // bytesperclumpline, so TIFFWriteScanline invokes JPEGEncodeRaw without
    // resetting encode_raw_error between rows. Failing write inside
    // std_empty_output_buffer triggers encode_raw_error = TRUE on one row,
    // and the next TIFFWriteScanline hits the sticky error guard.
    constexpr uint32_t kFailW = 128;
    constexpr uint32_t kFailH = 128;
    MemoryTiffStream fail_mem;
    UniqueTiff writer = fail_mem.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kFailW), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kFailH), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kFailH), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 100), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING, uint16_t{2},
                           uint16_t{1}),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RAW),
        1);
    ASSERT_EQ(TIFFWriteBufferSetup(writer.get(), nullptr, 128), 1);
    // Allow header write (call 0), fail first compressed strip flush.
    fail_mem.FailWritesAfter(1);

    const tmsize_t row_sz = TIFFScanlineSize(writer.get());
    ASSERT_GT(row_sz, 0);
    std::vector<uint8_t> row_data(static_cast<size_t>(row_sz));
    uint32_t state = 0x13579bdfU;
    uint32_t failed_row = kFailH;
    for (uint32_t r = 0; r < kFailH; ++r) {
      for (uint8_t& b : row_data) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        b = static_cast<uint8_t>(state & 0xffU);
      }
      // TIFFWriteScanline forwards tif_encoderow status (0 on JPEGEncodeRaw
      // failure, whereas pre-encode/bounds errors return -1).
      const int rc = TIFFWriteScanline(writer.get(), row_data.data(), r, 0);
      if (rc != 1) {
        EXPECT_EQ(rc, 0);
        failed_row = r;
        break;
      }
    }
    ASSERT_LT(failed_row + 1U, kFailH);
    EXPECT_EQ(
        TIFFWriteScanline(writer.get(), row_data.data(), failed_row + 1U, 0),
        0);
    EXPECT_TRUE(HasDiagnosticContaining(fail_mem.diagnostics().errors(),
                                        "JPEGEncodeRaw() already failed"));
  }
}

TEST(TifJpegTest, UpdateModeSwitchesBetweenDecompressorAndCompressor) {
  constexpr uint32_t kW = 16;
  constexpr uint32_t kH = 16;
  constexpr uint32_t kRps = 8;
  const std::vector<uint8_t> initial = MakeSmoothRgbPixels(kW, kH, /*seed=*/1);
  const std::vector<uint8_t> replacement =
      MakeSmoothRgbPixels(kW, kH, /*seed=*/3);

  MemoryTiffStream mem;
  {
    UniqueTiff writer = mem.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kRps), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_JPEGQUALITY, 95), 1);

    const tmsize_t strip_sz = TIFFVStripSize(writer.get(), kRps);
    std::vector<uint8_t> s0(initial.begin(), initial.begin() + strip_sz);
    std::vector<uint8_t> s1(initial.begin() + strip_sz, initial.end());
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(), strip_sz),
              strip_sz);
    ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, s1.data(), strip_sz),
              strip_sz);
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  {
    UniqueTiff rw = mem.Open("r+");
    ASSERT_NE(rw, nullptr);
    const tmsize_t strip_sz = TIFFVStripSize(rw.get(), kRps);
    std::vector<uint8_t> buf(static_cast<size_t>(strip_sz));

    // Read strip 0 -> sets TIFF_CODERSETUP with is_decompressor == 1.
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, buf.data(), strip_sz),
              strip_sz);

    // Write strip 1 -> JPEGPreEncode sees is_decompressor == 1 and re-inits
    // compressor (destroying decompressor cinfo). Match original JPEGQUALITY
    // so re-encoded strip uses the file's existing JPEGTABLES quantization
    // tables.
    ASSERT_EQ(TIFFSetField(rw.get(), TIFFTAG_JPEGQUALITY, 95), 1);
    std::vector<uint8_t> new_s1(replacement.begin() + strip_sz,
                                replacement.end());
    ASSERT_EQ(TIFFWriteEncodedStrip(rw.get(), 1, new_s1.data(), strip_sz),
              strip_sz);
    ASSERT_EQ(TIFFFlush(rw.get()), 1);

    // Read strip 0 again on same handle -> JPEGPreDecode sees is_decompressor
    // == 0 and re-inits decompressor (destroying compressor cinfo).
    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 0, buf.data(), strip_sz),
              strip_sz);
    absl::Span<const uint8_t> expected_s0(initial.data(),
                                          static_cast<size_t>(strip_sz));
    EXPECT_LE(MaxAbsDiff(expected_s0, buf), 16);

    ASSERT_EQ(TIFFReadEncodedStrip(rw.get(), 1, buf.data(), strip_sz),
              strip_sz);
    absl::Span<const uint8_t> expected_s1(replacement.data() + strip_sz,
                                          static_cast<size_t>(strip_sz));
    EXPECT_LE(MaxAbsDiff(expected_s1, buf), 16);
    EXPECT_EQ(mem.diagnostics().error_count(), 0U);
  }
}

TEST(TifJpegTest, FixupTagsSubsamplingAutoCorrectionAndMarkerSkip) {
  // Encode a standalone 16x16 1x1 YCbCr JPEG strip, then splice 0xFF padding,
  // COM, DRI, and an oversized (>2048 byte) APP1 marker before SOF0 so
  // JPEGFixupTagsSubsampling exercises marker skipping, buffer refill, and
  // auto-correction from default [2,2] to [1,1].
  const std::vector<uint8_t> base_jpeg =
      EncodeStandaloneJpegStrip(16, 16, PHOTOMETRIC_YCBCR, 3, /*sub_x=*/1,
                                /*sub_y=*/1, JPEGCOLORMODE_RGB);
  ASSERT_GT(base_jpeg.size(), 4U);
  ASSERT_EQ(base_jpeg[0], 0xFF);
  ASSERT_EQ(base_jpeg[1], 0xD8);

  std::vector<uint8_t> padded_jpeg;
  padded_jpeg.insert(padded_jpeg.end(), {0xFF, 0xD8});
  // Leading 0x00 junk bytes + extra 0xFF padding before marker.
  padded_jpeg.insert(padded_jpeg.end(), {0x00, 0xFF, 0xFF, 0xFF});
  // COM marker (0xFE) with 4 bytes payload (length = 6).
  padded_jpeg.insert(padded_jpeg.end(), {0xFE, 0x00, 0x06, 't', 'e', 's', 't'});
  // DRI marker (0xDD) with 2 bytes payload (length = 4).
  padded_jpeg.insert(padded_jpeg.end(), {0xFF, 0xDD, 0x00, 0x04, 0x00, 0x00});
  // Large APP1 marker (0xE1) with 2502 bytes length (>2048 buffer) to trigger
  // JPEGFixupTagsSubsamplingSkip across file refill boundaries.
  padded_jpeg.insert(padded_jpeg.end(), {0xFF, 0xE1, 0x09, 0xC6});
  padded_jpeg.insert(padded_jpeg.end(), 2500, 0x41);
  // Remainder of base JPEG (DQT, SOF0, DHT, SOS, data, EOI).
  padded_jpeg.insert(padded_jpeg.end(), base_jpeg.begin() + 2, base_jpeg.end());

  MemoryTiffStream mem(BuildYcbcrNoSubsamplingTagTiff(16, 16, 16, padded_jpeg));
  UniqueTiff tif = mem.Open("r");
  ASSERT_NE(tif, nullptr);

  uint16_t sx = 0;
  uint16_t sy = 0;
  ASSERT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, &sx, &sy), 1);
  EXPECT_EQ(sx, 1);
  EXPECT_EQ(sy, 1);
  EXPECT_TRUE(
      HasDiagnosticContaining(mem.diagnostics().warnings(),
                              "Auto-corrected former TIFF subsampling values"));

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
            1);
  std::vector<uint8_t> decoded(16 * 16 * 3);
  EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, decoded.data(),
                                 static_cast<tmsize_t>(decoded.size())),
            static_cast<tmsize_t>(decoded.size()));
  EXPECT_EQ(mem.diagnostics().error_count(), 0U);
}

TEST(TifJpegTest, FixupTagsSubsamplingErrorAndWarningEdgeCases) {
  // 1. Zero strip offset -> early return without error/warning.
  {
    MemoryTiffStream mem(
        BuildYcbcrNoSubsamplingTagTiff(16, 16, 16, {}, /*zero_offset=*/true));
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    uint16_t sx = 0;
    uint16_t sy = 0;
    EXPECT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, &sx, &sy),
        1);
    EXPECT_EQ(sx, 2);
    EXPECT_EQ(sy, 2);
  }

  // 2. Various malformed marker sequences inside JPEGFixupTagsSubsamplingSec.
  const std::vector<std::vector<uint8_t>> malformed_streams = {
      // Unknown marker 0xC8 -> default return 0 -> warning.
      {0xFF, 0xD8, 0xFF, 0xC8},
      // Truncated before any marker -> ReadByte EOF -> warning.
      {0xFF, 0xD8, 0xFF},
      // Truncated inside APP0 2-byte length word (high byte and low byte).
      {0xFF, 0xD8, 0xFF, 0xE0},
      {0xFF, 0xD8, 0xFF, 0xE0, 0x00},
      // APP0 with invalid length < 2 (n == 1) -> warning.
      {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x01},
      // APP0 whose skip length exceeds strip bytecount -> truncated skip ->
      // warning.
      {0xFF, 0xD8, 0xFF, 0xE0, 0x10, 0x00, 0x00},
      // SOF0 truncated at length, at comp 0 sampling, and at comp 1 sampling.
      {0xFF, 0xD8, 0xFF, 0xC0},
      {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03,
       0x01},
      {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03,
       0x01, 0x22, 0x00, 0x02},
      // SOF0 with wrong length (not 8 + 3*3 = 17) -> warning.
      {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x10, 0x00, 0x10, 0x01,
       0x01, 0x11, 0x00},
      // SOF0 with invalid Cb/Cr sampling factor (0x21 instead of 0x11) ->
      // warning.
      {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10,
       0x03, 0x01, 0x22, 0x00, 0x02, 0x21, 0x01, 0x03, 0x11, 0x01},
      // SOF0 with non-TIFF-equivalent Y sampling factor (3x3 -> 0x33) ->
      // warning.
      {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10,
       0x03, 0x01, 0x33, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01},
  };
  for (size_t i = 0; i < malformed_streams.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "malformed_idx=" << i);
    MemoryTiffStream mem(
        BuildYcbcrNoSubsamplingTagTiff(16, 16, 16, malformed_streams[i]));
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_GT(mem.diagnostics().warning_count(), 0U);
  }

  // Auto-correction when horizontal sampling matches default (2) and vertical
  // differs (1) -> exercises pv != td_ycbcrsubsampling[1] branch.
  {
    const std::vector<uint8_t> jpeg_2x1 =
        EncodeStandaloneJpegStrip(16, 16, PHOTOMETRIC_YCBCR, 3, /*sub_x=*/2,
                                  /*sub_y=*/1, JPEGCOLORMODE_RGB);
    MemoryTiffStream mem(BuildYcbcrNoSubsamplingTagTiff(16, 16, 16, jpeg_2x1));
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    uint16_t sx = 0;
    uint16_t sy = 0;
    ASSERT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, &sx, &sy),
        1);
    EXPECT_EQ(sx, 2);
    EXPECT_EQ(sy, 1);
    EXPECT_TRUE(HasDiagnosticContaining(
        mem.diagnostics().warnings(),
        "Auto-corrected former TIFF subsampling values"));
  }

  // 3. Read failure / short read during JPEGFixupTagsSubsamplingReadByte.
  //    Claim StripByteCounts=4096 inside a much smaller file so IFD open
  //    succeeds cleanly, then JPEGFixupTagsSubsampling reads past EOF.
  {
    const std::vector<uint8_t> tiny_prefix = {0xFF, 0xD8, 0xFF, 0xE0};
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(tiny_prefix);
    const std::array<uint16_t, 3> bps = {8, 8, 8};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 16),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS, 4096),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_TRUE(
        HasDiagnosticContaining(mem.diagnostics().warnings(),
                                "Unable to auto-correct subsampling values"));
  }

  // 4. OOM for the 2048-byte buffer in JPEGFixupTagsSubsampling, and seek
  //    failure inside JPEGFixupTagsSubsamplingReadByte when reading directory
  //    #1.
  {
    const std::vector<uint8_t> valid_jpeg = EncodeStandaloneJpegStrip(
        16, 16, PHOTOMETRIC_YCBCR, 3, 1, 1, JPEGCOLORMODE_RGB);
    RawTiffBuilder builder;
    const std::vector<uint8_t> gray_pixels(64, 128);
    const uint64_t off0 = builder.AppendBlob(gray_pixels);
    const uint64_t off1 = builder.AppendBlob(valid_jpeg);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off0)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(gray_pixels.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    const std::array<uint16_t, 3> bps = {8, 8, 8};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off1)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 16),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(valid_jpeg.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    const std::vector<uint8_t> two_dir_tiff = builder.Take();
    {
      MemoryTiffStream mem(two_dir_tiff);
      UniqueTiff tif = mem.Open("r");
      ASSERT_NE(tif, nullptr);
      ScopedSingleMemAllocLimit limit(tif.get(), 2047);
      ASSERT_EQ(TIFFSetDirectory(tif.get(), 1), 1);
      EXPECT_TRUE(HasDiagnosticContaining(
          mem.diagnostics().warnings(),
          "Unable to allocate memory for auto-correcting"));
    }
    bool hit_seek_fail = false;
    for (size_t extra_seeks = 0; extra_seeks < 6 && !hit_seek_fail;
         ++extra_seeks) {
      MemoryTiffStream mem(two_dir_tiff);
      UniqueTiff tif = mem.Open("r");
      ASSERT_NE(tif, nullptr);
      mem.FailSeeksAfter(mem.seek_calls() + extra_seeks);
      if (TIFFSetDirectory(tif.get(), 1) == 1 &&
          HasDiagnosticContaining(
              mem.diagnostics().warnings(),
              "Unable to auto-correct subsampling values")) {
        hit_seek_fail = true;
      }
    }
    EXPECT_TRUE(hit_seek_fail);
  }
}

TEST(TifJpegTest, ProgressiveJpegScanLimitsAndFullStripRequired) {
  const std::vector<uint8_t> prog_jpeg = MakeMinimalProgressiveGray8x8Jpeg();

  // Build a 2-strip 8x16 grayscale TIFF containing two progressive SOF2 strips.
  auto build_prog_tiff = [&]() {
    RawTiffBuilder builder;
    const uint64_t off0 = builder.AppendBlob(prog_jpeg);
    const uint64_t off1 = builder.AppendBlob(prog_jpeg);
    const std::array<uint32_t, 2> offsets = {static_cast<uint32_t>(off0),
                                             static_cast<uint32_t>(off1)};
    const std::array<uint32_t, 2> bytecounts = {
        static_cast<uint32_t>(prog_jpeg.size()),
        static_cast<uint32_t>(prog_jpeg.size())};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offsets),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, bytecounts),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    return builder.Take();
  };

  // 1. Decode both strips normally: progressive warning appears once; pixels ==
  // 128.
  {
    MemoryTiffStream mem(build_prog_tiff());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);

    // Directly test TIFFJPEGIsFullStripRequired on progressive raw strip.
    ASSERT_EQ(TIFFReadBufferSetup(tif.get(), nullptr,
                                  static_cast<tmsize_t>(prog_jpeg.size())),
              1);
    ASSERT_EQ(TIFFReadRawStrip(tif.get(), 0, tif->tif_rawdata,
                               static_cast<tmsize_t>(prog_jpeg.size())),
              static_cast<tmsize_t>(prog_jpeg.size()));
    tif->tif_rawcc = static_cast<tmsize_t>(prog_jpeg.size());
    EXPECT_EQ(TIFFJPEGIsFullStripRequired(tif.get()), 1);

    std::vector<uint8_t> strip(64, 0);
    ASSERT_EQ(TIFFReadEncodedStrip(tif.get(), 0, strip.data(), 64), 64);
    for (uint8_t px : strip) {
      EXPECT_EQ(px, 128U);
    }
    ASSERT_EQ(TIFFReadEncodedStrip(tif.get(), 1, strip.data(), 64), 64);
    int prog_warnings = 0;
    for (const auto& w : mem.diagnostics().warnings()) {
      if (w.message.find("progressive mode") != std::string::npos) {
        ++prog_warnings;
      }
    }
    EXPECT_EQ(prog_warnings, 1);
  }

  // 2. LIBTIFF_JPEG_MAX_ALLOWED_SCAN_NUMBER=1 aborts via progress monitor.
  {
    ScopedEnvVar scan_limit("LIBTIFF_JPEG_MAX_ALLOWED_SCAN_NUMBER", "1");
    MemoryTiffStream mem(build_prog_tiff());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> strip(64, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, strip.data(), 64),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().errors(),
                                        "exceeds maximum scans"));
  }

  // 3. JPEGMEM="500k" triggers max_memory_to_use rejection for multi-scan JPEG,
  //    then LIBTIFF_ALLOW_LARGE_LIBJPEG_MEM_ALLOC="1" overrides it.
  {
    ScopedEnvVar jpegmem("JPEGMEM", "500k");
    {
      MemoryTiffStream mem(build_prog_tiff());
      UniqueTiff tif = mem.Open("r");
      ASSERT_NE(tif, nullptr);
      std::vector<uint8_t> strip(64, 0);
      EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, strip.data(), 64),
                static_cast<tmsize_t>(-1));
      EXPECT_TRUE(HasDiagnosticContaining(
          mem.diagnostics().errors(), "require libjpeg to allocate at least"));
    }
    {
      ScopedEnvVar allow_large("LIBTIFF_ALLOW_LARGE_LIBJPEG_MEM_ALLOC", "1");
      MemoryTiffStream mem(build_prog_tiff());
      UniqueTiff tif = mem.Open("r");
      ASSERT_NE(tif, nullptr);
      std::vector<uint8_t> strip(64, 0);
      EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, strip.data(), 64), 64);
    }
  }

  // 4. TIFFJPEGIsFullStripRequired returns 0 on baseline and corrupt data.
  {
    const std::vector<uint8_t> baseline =
        EncodeStandaloneJpegStrip(8, 8, PHOTOMETRIC_MINISBLACK, 1);
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(baseline);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(baseline.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFReadBufferSetup(tif.get(), nullptr,
                                  static_cast<tmsize_t>(baseline.size())),
              1);
    ASSERT_EQ(TIFFReadRawStrip(tif.get(), 0, tif->tif_rawdata,
                               static_cast<tmsize_t>(baseline.size())),
              static_cast<tmsize_t>(baseline.size()));
    tif->tif_rawcc = static_cast<tmsize_t>(baseline.size());
    EXPECT_EQ(TIFFJPEGIsFullStripRequired(tif.get()), 0);

    // Corrupt header causes TIFFjpeg_read_header != JPEG_HEADER_OK -> 0.
    tif->tif_rawdata[0] = 0x00;
    tif->tif_rawdata[1] = 0x00;
    EXPECT_EQ(TIFFJPEGIsFullStripRequired(tif.get()), 0);
  }
}

TEST(TifJpegTest, SetupEncodeValidationErrors) {
  auto test_encode_error = [](auto setup_fn, std::string_view expected_err,
                              bool is_tile = false) {
    MemoryTiffStream mem;
    UniqueTiff tif = mem.Open("w");
    ASSERT_NE(tif, nullptr);
    setup_fn(tif.get());
    if (is_tile) {
      std::array<uint8_t, 64> buf{};
      EXPECT_EQ(TIFFWriteEncodedTile(tif.get(), 0, buf.data(), buf.size()),
                static_cast<tmsize_t>(-1));
    } else {
      std::array<uint8_t, 64> buf{};
      EXPECT_EQ(TIFFWriteScanline(tif.get(), buf.data(), 0, 0), -1);
    }
    EXPECT_TRUE(
        HasDiagnosticContaining(mem.diagnostics().errors(), expected_err))
        << "Expected error containing '" << expected_err << "'";
  };

  // Disallowed photometrics: PALETTE and MASK.
  for (uint16_t photo : {PHOTOMETRIC_PALETTE, PHOTOMETRIC_MASK}) {
    test_encode_error(
        [photo](TIFF* tif) {
          TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t{8});
          TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t{8});
          TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8});
          TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1});
          TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
          TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, photo);
          TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
          TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, uint32_t{8});
        },
        "not allowed for JPEG");
  }

  // Invalid BitsPerSample for RGB (16) and YCbCr (>16 and 16).
  test_encode_error(
      [](TIFF* tif) {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t{8});
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t{8});
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{16});
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{3});
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, uint32_t{8});
      },
      "BitsPerSample 16 not allowed for JPEG");

  test_encode_error(
      [](TIFF* tif) {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t{8});
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t{8});
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{32});
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{3});
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, uint32_t{8});
      },
      "BitsPerSample 32 not allowed for JPEG");

  // Zero YCbCr subsampling (set JPEGCOLORMODE_RGB so TIFFScanlineSize skips its
  // earlier check and JPEGSetupEncode reports the invalid sampling value).
  test_encode_error(
      [](TIFF* tif) {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t{16});
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t{16});
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8});
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{3});
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, uint32_t{16});
        TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
        TIFFSetField(tif, TIFFTAG_YCBCRSUBSAMPLING, uint16_t{0}, uint16_t{2});
      },
      "horizontal/vertical sampling value");

  // Bad tile width / tile height multiples (use subsampling 4 to exceed the
  // 16-multiple enforced by TIFFSetField and reach JPEGSetupEncode's check).
  test_encode_error(
      [](TIFF* tif) {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t{32});
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t{32});
        TIFFSetField(tif, TIFFTAG_TILEWIDTH, uint32_t{16});
        TIFFSetField(tif, TIFFTAG_TILELENGTH, uint32_t{32});
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8});
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{3});
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
        TIFFSetField(tif, TIFFTAG_YCBCRSUBSAMPLING, uint16_t{4}, uint16_t{2});
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
      },
      "JPEG tile width must be multiple", /*is_tile=*/true);

  test_encode_error(
      [](TIFF* tif) {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t{32});
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t{32});
        TIFFSetField(tif, TIFFTAG_TILEWIDTH, uint32_t{32});
        TIFFSetField(tif, TIFFTAG_TILELENGTH, uint32_t{16});
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8});
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{3});
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
        TIFFSetField(tif, TIFFTAG_YCBCRSUBSAMPLING, uint16_t{2}, uint16_t{4});
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
      },
      "JPEG tile height must be multiple", /*is_tile=*/true);

  // Bad rowsperstrip (< imagelength and not a multiple of v_sampling * 8).
  test_encode_error(
      [](TIFF* tif) {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t{16});
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t{20});
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8});
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{3});
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, uint32_t{5});
      },
      "RowsPerStrip must be multiple");

  // Strile width > JPEG_MAX_DIMENSION (65500) caught in JPEGPreEncode.
  test_encode_error(
      [](TIFF* tif) {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t{65501});
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t{8});
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t{8});
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t{1});
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, uint32_t{8});
      },
      "Strip/tile too large for JPEG");
}

TEST(TifJpegTest, DecodeValidationErrorsTruncatedAndCorruptStreams) {
  const std::vector<uint8_t> valid_8x8_gray =
      EncodeStandaloneJpegStrip(8, 8, PHOTOMETRIC_MINISBLACK, 1);
  const std::vector<uint8_t> valid_16x16_ycbcr_2x2 =
      EncodeStandaloneJpegStrip(16, 16, PHOTOMETRIC_YCBCR, 3, /*sub_x=*/2,
                                /*sub_y=*/2, JPEGCOLORMODE_RGB);
  const std::vector<uint8_t> valid_16x16_ycbcr_1x1 =
      EncodeStandaloneJpegStrip(16, 16, PHOTOMETRIC_YCBCR, 3, /*sub_x=*/1,
                                /*sub_y=*/1, JPEGCOLORMODE_RGB);

  // 1. Bogus JPEGTables field in JPEGSetupDecode.
  {
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(valid_8x8_gray);
    const std::vector<uint8_t> bogus_tables = {0x00, 0x01, 0x02, 0x03};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(valid_8x8_gray.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        builder.MakeOutOfLine(TIFFTAG_JPEGTABLES, TIFF_UNDEFINED,
                              bogus_tables.size(), bogus_tables),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> out(64);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 64),
              static_cast<tmsize_t>(-1));
    // Re-calling on the same handle after JPEGSetupDecode failed (before
    // TIFF_CODERSETUP is set) exercises JPEGInitializeLibJPEG reuse in same
    // mode.
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 64),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().errors(),
                                        "Bogus JPEGTables field"));
  }

  // 2. Corrupt JPEG markers and truncated stream (setjmp error recovery +
  //    std_fill_input_buffer dummy EOI + std_skip_input_data overrun).
  {
    std::vector<uint8_t> truncated = valid_8x8_gray;
    truncated.resize(truncated.size() > 6 ? truncated.size() - 6 : 2);
    // Insert APP0 with oversized length that triggers std_skip_input_data
    // overrun.
    std::vector<uint8_t> skip_overrun = {0xFF, 0xD8, 0xFF, 0xE0,
                                         0x20, 0x00, 0x01};
    const std::vector<uint8_t> garbage = {0x12, 0x34, 0x56, 0x78};

    for (const auto& stream : {truncated, skip_overrun, garbage}) {
      RawTiffBuilder builder;
      const uint64_t off = builder.AppendBlob(stream);
      builder.AddIfd({
          builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
          builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
          builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
          builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
          builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
          builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
          builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
          builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
          builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                           static_cast<uint32_t>(stream.size())),
          builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
      });
      MemoryTiffStream mem(builder.Take());
      UniqueTiff tif = mem.Open("r");
      ASSERT_NE(tif, nullptr);
      std::vector<uint8_t> out(64);
      const tmsize_t rc = TIFFReadEncodedStrip(tif.get(), 0, out.data(), 64);
      // Truncated stream with valid header warns JWRN_JPEG_EOF via dummy EOI;
      // corrupt headers fail via setjmp error_exit. Neither may crash or leak.
      if (stream == garbage || stream == skip_overrun) {
        EXPECT_EQ(rc, static_cast<tmsize_t>(-1));
        EXPECT_GT(mem.diagnostics().error_count(), 0U);
      } else {
        EXPECT_GT(mem.diagnostics().warning_count(), 0U);
      }
    }
  }

  // 3. JPEG dimensions mismatch cases:
  //    a) JPEG larger than strip on non-last strip -> error.
  //    b) JPEG taller than truncated last strip (same width) -> warns and
  //    succeeds. c) JPEG smaller than strip -> warns and zero-pads remainder.
  {
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(valid_8x8_gray);
    // 4x4 TIFF with 8x8 JPEG -> exceeds expected dimensions -> error.
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 4),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 4),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(valid_8x8_gray.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> out(16);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 16),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        mem.diagnostics().errors(),
        "JPEG strip/tile size exceeds expected dimensions"));
  }

  {
    // 8x6 TIFF (same width 8, truncated last strip height 6 < JPEG height 8).
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(valid_8x8_gray);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 6),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(valid_8x8_gray.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> out(48);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 48), 48);
    EXPECT_TRUE(
        HasDiagnosticContaining(mem.diagnostics().warnings(),
                                "JPEG strip size exceeds expected dimensions"));
  }

  {
    // 16x16 TIFF with 8x8 JPEG -> smaller than strip -> warns & zero-pads.
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(valid_8x8_gray);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 16),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(valid_8x8_gray.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> out(256, 0xAA);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 256), 256);
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().warnings(),
                                        "Improper JPEG strip/tile size"));
    // Bottom rows (rows 8..15) must have been zero-initialized.
    for (size_t i = 8 * 16; i < out.size(); ++i) {
      EXPECT_EQ(out[i], 0U);
    }
  }

  {
    // 16x8 TIFF with 8x8 JPEG -> strile_width > image_width with equal height
    // takes the else-if zero-init branch in JPEGDecode.
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(valid_8x8_gray);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(valid_8x8_gray.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> out(16 * 8, 0xAA);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 16 * 8), 16 * 8);
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().warnings(),
                                        "Improper JPEG strip/tile size"));
    for (uint32_t r = 0; r < 8; ++r) {
      for (uint32_t c = 8; c < 16; ++c) {
        EXPECT_EQ(out[r * 16 + c], 0U);
      }
    }
  }

  {
    // 16x10 YCbCr 2x2 raw TIFF where last strip (rows 8..9) contains a 16x8
    // JPEG stream -> JPEGDecodeRaw clamps nrows to remaining image height (2).
    const std::vector<uint8_t> strip_16x8 =
        EncodeStandaloneJpegStrip(16, 8, PHOTOMETRIC_YCBCR, 3, /*sub_x=*/2,
                                  /*sub_y=*/2, JPEGCOLORMODE_RGB);
    RawTiffBuilder builder;
    const uint64_t off0 = builder.AppendBlob(strip_16x8);
    const uint64_t off1 = builder.AppendBlob(strip_16x8);
    const std::array<uint16_t, 3> bps = {8, 8, 8};
    const std::array<uint16_t, 2> sub = {2, 2};
    const std::array<uint32_t, 2> offs = {static_cast<uint32_t>(off0),
                                          static_cast<uint32_t>(off1)};
    const std::array<uint32_t, 2> bcs = {
        static_cast<uint32_t>(strip_16x8.size()),
        static_cast<uint32_t>(strip_16x8.size())};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 10),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
        builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offs),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, bcs),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        builder.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING, sub),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    const tmsize_t s1_sz = TIFFVStripSize(tif.get(), 2);
    ASSERT_GT(s1_sz, 0);
    std::vector<uint8_t> out(static_cast<size_t>(s1_sz));
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 1, out.data(), s1_sz), s1_sz);
    EXPECT_TRUE(
        HasDiagnosticContaining(mem.diagnostics().warnings(),
                                "JPEG strip size exceeds expected dimensions"));
  }

  // 4. Component count, precision, and sampling factor mismatches.
  {
    // 1-component JPEG inside 3-sample contig TIFF -> Improper JPEG component
    // count.
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(valid_8x8_gray);
    const std::array<uint16_t, 3> bps = {8, 8, 8};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(valid_8x8_gray.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> out(8 * 8 * 3);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().errors(),
                                        "Improper JPEG component count"));
  }

  {
    // BitsPerSample=16 in TIFF with 8-bit JPEG -> Improper JPEG data precision.
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(valid_8x8_gray);
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
        builder.MakeShort(TIFFTAG_BITSPERSAMPLE, 16),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(valid_8x8_gray.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> out(8 * 8 * 2);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(),
                                   static_cast<tmsize_t>(out.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().errors(),
                                        "Improper JPEG data precision"));
  }

  {
    // Sampling mismatch: TIFF YCbCr says [2,2] while JPEG stream has [1,1].
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(valid_16x16_ycbcr_1x1);
    const std::array<uint16_t, 3> bps = {8, 8, 8};
    const std::array<uint16_t, 2> sub = {2, 2};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 16),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(valid_16x16_ycbcr_1x1.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        builder.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING, sub),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    const tmsize_t sz = TIFFStripSize(tif.get());
    std::vector<uint8_t> out(static_cast<size_t>(sz));
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), sz),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().errors(),
                                        "Improper JPEG sampling factors"));
  }

  {
    // CONTIG YCbCr 2x2 where component 1 (Cb) has 2x1 sampling instead of 1x1.
    std::vector<uint8_t> bad_cb_jpeg = {0xFF, 0xD8, 0xFF, 0xDB,
                                        0x00, 0x43, 0x00};
    bad_cb_jpeg.insert(bad_cb_jpeg.end(), 64, 0x10);
    bad_cb_jpeg.insert(
        bad_cb_jpeg.end(),
        {0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03, 0x01, 0x22,
         0x00, 0x02, 0x21, 0x00, 0x03, 0x11, 0x00, 0xFF, 0xDA, 0x00, 0x0C, 0x03,
         0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0xD9});
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(bad_cb_jpeg);
    const std::array<uint16_t, 3> bps = {8, 8, 8};
    const std::array<uint16_t, 2> sub = {2, 2};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 16),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(bad_cb_jpeg.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        builder.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING, sub),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    const tmsize_t sz = TIFFStripSize(tif.get());
    std::vector<uint8_t> out(static_cast<size_t>(sz));
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), sz),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().errors(),
                                        "Improper JPEG sampling factors"));
  }

  {
    // SEPARATE 3-plane RGB where plane 0 JPEG SOF0 declares 2x1 instead of 1x1.
    std::vector<uint8_t> non_unit_plane_jpeg = {0xFF, 0xD8, 0xFF, 0xDB,
                                                0x00, 0x43, 0x00};
    non_unit_plane_jpeg.insert(non_unit_plane_jpeg.end(), 64, 0x10);
    non_unit_plane_jpeg.insert(
        non_unit_plane_jpeg.end(),
        {0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08, 0x00, 0x08,
         0x01, 0x01, 0x21, 0x00, 0xFF, 0xDA, 0x00, 0x08, 0x01,
         0x01, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0xD9});
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(non_unit_plane_jpeg);
    const std::array<uint16_t, 3> bps = {8, 8, 8};
    const uint32_t off32 = static_cast<uint32_t>(off);
    const uint32_t bc32 = static_cast<uint32_t>(non_unit_plane_jpeg.size());
    const std::array<uint32_t, 3> offs = {off32, off32, off32};
    const std::array<uint32_t, 3> bcs = {bc32, bc32, bc32};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
        builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offs),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, bcs),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> out(64);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, out.data(), 64),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().errors(),
                                        "Improper JPEG sampling factors"));
  }

  {
    // SEPARATE YCbCr with YCbCrSubsampling=[0,2] fails in
    // JPEGComputeStrileWidthHeightBytesPerLine when decoding plane s = 1.
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(valid_8x8_gray);
    const std::array<uint16_t, 3> bps = {8, 8, 8};
    const std::array<uint16_t, 2> sub = {0, 2};
    const uint32_t off32 = static_cast<uint32_t>(off);
    const uint32_t bc32 = static_cast<uint32_t>(valid_8x8_gray.size());
    const std::array<uint32_t, 3> offs = {off32, off32, off32};
    const std::array<uint32_t, 3> bcs = {bc32, bc32, bc32};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 8),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 8),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
        builder.MakeLongArray(TIFFTAG_STRIPOFFSETS, offs),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 8),
        builder.MakeLongArray(TIFFTAG_STRIPBYTECOUNTS, bcs),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE),
        builder.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING, sub),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> out(64);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 1, out.data(), 64),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        mem.diagnostics().errors(),
        "JPEG horizontal or vertical sampling is zero"));
  }

  {
    // JPEGDecodeRaw buffer too small checks (< bytesperline, and < clump
    // stride).
    RawTiffBuilder builder;
    const uint64_t off = builder.AppendBlob(valid_16x16_ycbcr_2x2);
    const std::array<uint16_t, 3> bps = {8, 8, 8};
    const std::array<uint16_t, 2> sub = {2, 2};
    builder.AddIfd({
        builder.MakeLong(TIFFTAG_IMAGEWIDTH, 16),
        builder.MakeLong(TIFFTAG_IMAGELENGTH, 16),
        builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps),
        builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
        builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
        builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 16),
        builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                         static_cast<uint32_t>(valid_16x16_ycbcr_2x2.size())),
        builder.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        builder.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING, sub),
    });
    MemoryTiffStream mem(builder.Take());
    UniqueTiff tif = mem.Open("r");
    ASSERT_NE(tif, nullptr);
    std::vector<uint8_t> small_buf(64);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, small_buf.data(), 4),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(
        mem.diagnostics().errors(),
        "application buffer not large enough for all data"));

    const tmsize_t scan_sz = TIFFScanlineSize(tif.get());
    ASSERT_GT(scan_sz, 0);
    EXPECT_EQ(TIFFReadEncodedStrip(tif.get(), 0, small_buf.data(), scan_sz),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().errors(),
                                        "possible subsampling issue"));
  }

  // 5. Partial scanline abort between strips and fractional cc warnings.
  {
    MemoryTiffStream mem;
    {
      UniqueTiff writer = mem.Open("w");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{8}), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}),
                1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC,
                             PHOTOMETRIC_MINISBLACK),
                1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{8}),
                1);
      std::vector<uint8_t> s0(64, 100);
      std::vector<uint8_t> s1(64, 150);
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 0, s0.data(), 64), 64);
      ASSERT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, s1.data(), 64), 64);
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }
    UniqueTiff reader = mem.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint8_t> row_buf(8);
    // Read only row 0 from strip 0, then jump to row 8 in strip 1 (triggers
    // output_scanline < output_height and TIFFjpeg_abort on strip change).
    EXPECT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 0, 0), 1);
    EXPECT_EQ(TIFFReadScanline(reader.get(), row_buf.data(), 8, 0), 1);
    // Fractional cc in TIFFReadEncodedStrip triggers fractional scanline
    // warning.
    std::vector<uint8_t> partial(11);
    EXPECT_EQ(TIFFReadEncodedStrip(reader.get(), 0, partial.data(), 11), 11);
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().warnings(),
                                        "fractional scanline not read"));
  }
}

TEST(TifJpegTest, AllocationLimitsOomDuringInitAndTablesDest) {
  // 1. OOM inside TIFFInitJPEG for JPEGState allocation (leave enough budget
  //    for _TIFFMergeFields realloc delta, fail on sizeof(JPEGState)).
  {
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

    MemoryTiffStream mem;
    UniqueTiff tif = mem.Open("w", opts.get());
    ASSERT_NE(tif, nullptr);
    ScopedCumulatedMemAllocRemaining tighten(tif.get(), 128);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              0);
    EXPECT_TRUE(HasDiagnosticContaining(recorder.errors(),
                                        "No space for JPEG state block"));
  }

  // 2. OOM inside TIFFInitJPEG for initial 2000-byte JPEGTables buffer.
  {
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

    MemoryTiffStream mem;
    UniqueTiff tif = mem.Open("w", opts.get());
    ASSERT_NE(tif, nullptr);
    // Enough for _TIFFMergeFields + JPEGState (~1000 bytes), not for +2000.
    ScopedCumulatedMemAllocRemaining tighten(tif.get(), 1500);
    EXPECT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              0);
    EXPECT_TRUE(HasDiagnosticContaining(
        recorder.errors(), "Failed to allocate memory for JPEG tables"));
  }

  // 3. OOM inside TIFFjpeg_tables_dest during JPEGSetupEncode (1000-byte
  // alloc).
  {
    DiagnosticRecorder recorder;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    recorder.Install(opts.get());
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), kDefaultAllocBudget);

    MemoryTiffStream mem;
    UniqueTiff tif = mem.Open("w", opts.get());
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, uint32_t{8}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, uint32_t{8}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{8}), 1);

    // Pre-allocate write buffer before tightening single-alloc limit to 999
    // so TIFFjpeg_tables_dest's 1000-byte allocation fails deterministically.
    ASSERT_EQ(TIFFWriteBufferSetup(tif.get(), nullptr, 512), 1);
    ScopedSingleMemAllocLimit limit(tif.get(), 999);
    std::vector<uint8_t> pixels(8 * 8 * 3, 128);
    EXPECT_EQ(TIFFWriteEncodedStrip(tif.get(), 0, pixels.data(),
                                    static_cast<tmsize_t>(pixels.size())),
              static_cast<tmsize_t>(-1));
    EXPECT_TRUE(
        HasDiagnosticContaining(recorder.errors(), "No space for JPEGTables"));
  }
}

TEST(TifJpegTest, EncodeFractionalAndTruncatedStripEdgeCases) {
  // 1. JPEGEncode fractional trailing byte warning + last-strip row clamping.
  {
    constexpr uint32_t kW = 8;
    constexpr uint32_t kH = 10;
    constexpr uint32_t kRps = 8;
    MemoryTiffStream mem;
    UniqueTiff writer = mem.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, kW), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, kH), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, kRps), 1);

    const tmsize_t row_sz = TIFFScanlineSize(writer.get());
    ASSERT_EQ(row_sz, static_cast<tmsize_t>(kW * 3));
    std::vector<uint8_t> buf(static_cast<size_t>(row_sz * kRps + 1), 64);
    EXPECT_EQ(
        TIFFWriteEncodedStrip(writer.get(), 0, buf.data(), row_sz * kRps + 1),
        row_sz * kRps + 1);
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().warnings(),
                                        "fractional scanline discarded"));
    // Pass 8 rows' worth of bytes into the 2-row truncated final strip.
    EXPECT_EQ(TIFFWriteEncodedStrip(writer.get(), 1, buf.data(), row_sz * kRps),
              row_sz * kRps);
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 1);
    EXPECT_EQ(mem.diagnostics().error_count(), 0U);
  }

  // 2. JPEGEncodeRaw fractional trailing byte warning on YCbCr 2x2 raw write.
  {
    MemoryTiffStream mem;
    UniqueTiff writer = mem.Open("w");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, uint32_t{16}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, uint32_t{16}), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{3}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_JPEG),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR), 1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, uint32_t{16}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_YCBCRSUBSAMPLING, uint16_t{2},
                           uint16_t{2}),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RAW),
        1);

    const tmsize_t strip_sz = TIFFStripSize(writer.get());
    ASSERT_GT(strip_sz, 0);
    std::vector<uint8_t> raw_buf(static_cast<size_t>(strip_sz + 1), 128);
    EXPECT_EQ(
        TIFFWriteEncodedStrip(writer.get(), 0, raw_buf.data(), strip_sz + 1),
        strip_sz + 1);
    EXPECT_TRUE(HasDiagnosticContaining(mem.diagnostics().warnings(),
                                        "fractional scanline discarded"));
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 1);
    EXPECT_EQ(mem.diagnostics().error_count(), 0U);
  }
}

}  // namespace
}  // namespace libtiff
